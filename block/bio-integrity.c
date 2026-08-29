// SPDX-License-Identifier: GPL-2.0
/*
 * bio-integrity.c - bio data integrity extensions
 *
 * Copyright (C) 2007, 2008, 2009 Oracle Corporation
 * Written by: Martin K. Petersen <martin.petersen@oracle.com>
 */
/*
 * [한국어 설명] bio-integrity.c — bio T10 PI(Protection Information) 메타데이터
 * 부착/전파/해제 (bio-integrity.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 bio(Block I/O descriptor)에 T10 PI(Protection Information,
 * DIF(Data Integrity Field)/DIX(Data Integrity Extension)) 무결성 메타데이터를
 * 부착·전파·해제하는 확장 기능을 구현한다. 파일시스템이나 사용자 공간이
 * 요청한 Guard(CRC/체크섬), Application Tag, Reference Tag를
 * bio_integrity_payload(이하 bip)라는 별도 자료구조에 담아, 실제 데이터를
 * 가리키는 bi_io_vec과는 독립적인 metadata bio_vec 체인으로 관리한다.
 * NVMe 관점에서 이 메타데이터는 NVMe 커맨드의 별도 Metadata Pointer(MPTR)
 * 필드 또는 확장 LBA(extended LBA)로 SQ(Submission Queue)에 실려, 컨트롤러가
 * 하드웨어 레벨에서 CRC/RefTag/AppTag를 검증(PRACT/PRCHK 비트에 따라)하거나
 * host가 소프트웨어로 직접 계산·검증한다. 이 파일이 없으면 block layer는
 * 조용한 데이터 손상(silent data corruption)을 SCSI DIF/NVMe PI 하드웨어에
 * 위임해 탐지할 방법이 없다. bio 분할/병합/클론이 일어날 때마다 metadata
 * iterator를 함께 갱신해야 하므로, 이 파일은 block/bio.c의 bio 생명주기
 * 관리와 항상 짝을 이루어 동작한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (위 → 아래):
 *   파일시스템/사용자 공간(O_DIRECT + PI, io_uring/nvme-cli passthrough 등)
 *     → bio_integrity_alloc() / bio_integrity_map_user() /
 *       bio_integrity_map_iter()                             ← 이 파일
 *     → submit_bio() / submit_bio_noacct()                   ← block/blk-core.c
 *     → blk_mq_submit_bio() → blk_rq_bio_prep()
 *       → __bio_integrity_action()                           ← blk-mq.c ↔ 이 파일
 *     → nvme_queue_rq() → nvme_setup_cmd() → nvme_map_data()
 *       → nvme_map_metadata()                                ← drivers/nvme/host/pci.c
 *     → NVMe 컨트롤러 SQ(Submission Queue)
 *       → PRACT/PRCHK 비트에 따라 컨트롤러가 PI 생성/검증 수행
 *     → NVMe CQ(Completion Queue) → nvme_complete_rq() → bio_endio()
 *     → bio_integrity_free() / bio_integrity_unmap_user()     ← 이 파일 (완료 정리)
 *
 * 실행 컨텍스트: 커널 태스크 컨텍스트(주로 submit 경로, GFP_NOIO/GFP_KERNEL
 * 할당 가능)에서 주로 실행되며, bio_integrity_advance()/bio_integrity_trim()은
 * blk-mq의 분할·병합·완료 경로를 통해 소프트/하드 IRQ 컨텍스트에서도 호출될
 * 수 있다(완료 처리는 NVMe 인터럽트 핸들러 → softirq 경로를 거치는 경우가
 * 많다).
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - include/linux/bio-integrity.h : struct bio_integrity_payload,
 *     enum bip_flags 정의. 본 저장소는 drivers/pci, block, NVMe 관련 헤더만
 *     남긴 sparse checkout이라 이 헤더의 실물은 트리에 없다 — 이 파일의
 *     실제 사용 패턴(필드 대입/참조)을 근거로 아래 "struct
 *     bio_integrity_payload 필드 참조" 블록에 필드를 문서화했다.
 *   - include/linux/t10-pi.h        : T10 PI tuple 레이아웃(Guard/AppTag/RefTag),
 *     CRC16/CRC64(T10-DIF/DIF64) 계산에 필요한 상수/헬퍼.
 *   - block/blk.h                   : bio_integrity_prep()/request 단위 병합
 *     헬퍼 등 내부(non-export) 인터페이스 선언.
 *   - block/blk-integrity.c         : struct blk_integrity(디스크/네임스페이스
 *     단위 PI 프로파일: csum_type, metadata_size, pi_tuple_size, flags)를
 *     제공 — 이 파일의 거의 모든 함수가 blk_get_integrity()로 이를 조회한다.
 *   - block/bio.c                   : struct bio, bio_alloc()/bio_endio()/
 *     bio_split() — 이 파일의 모든 함수는 bio->bi_integrity를 다루므로 bio
 *     생명주기(할당→분할→완료→해제)와 반드시 동기화되어야 한다.
 *   - drivers/nvme/host/pci.c       : nvme_map_metadata()(pci.c:1790)가
 *     bip_vec을 실제 DMA 주소로 변환한다. 세그먼트가 하나뿐이면
 *     nvme_pci_setup_meta_mptr()로 SQE의 MPTR 필드에 단일 주소를 넣고,
 *     여러 개면 nvme_pci_setup_meta_iter()로 메타데이터 SGL을 구성한다.
 *     PCIe NVMe에서 max_integrity_segments가 SGL 지원 시
 *     NVME_MAX_META_SEGS, 아니면 1인 이유가 이 두 경로의 차이다.
 *
 * 데이터 흐름:
 *   사용자 metadata 버퍼(iov_iter) 또는 파일시스템 PI 생성기
 *     → bio_integrity_alloc()/bio_integrity_add_page() → bip->bip_vec[]
 *       (물리 페이지 배열)
 *     → __bio_integrity_action()이 host 검증/생성 필요 여부 및 NVMe offload
 *       가능 여부 판정 → 필요 시 bio_integrity_alloc_buf()가 커널 버퍼를 채움
 *     → NVMe 드라이버가 bip_vec을 DMA 매핑 → NVMe 커맨드 metadata pointer
 *     → 컨트롤러 완료(CQ) 후 bio_integrity_unmap_user()가 사용자 공간으로
 *       메타데이터를 복사하거나 페이지 pin을 해제
 *
 * 공유 핵심 자료구조:
 *   struct bio_integrity_payload(bip) : 이 파일이 생성·갱신·해제를 전담하는
 *     핵심 자료구조. bio->bi_integrity에 매달리며, REQ_INTEGRITY 플래그로
 *     그 존재가 표시된다. 필드별 상세 설명은 본 파일 하단 "struct
 *     bio_integrity_payload 필드 참조" 블록 참고.
 *   struct blk_integrity(bi)          : block/blk-integrity.c가 소유하는
 *     디스크 단위 PI 프로파일(이 파일에서는 읽기 전용으로만 참조).
 *   struct uio_meta                   : io_uring 등 사용자 공간 PI passthrough
 *     시 seed/iterator/flags를 실어 나르는 임시 전달 구조체
 *     (bio_integrity_map_iter()가 소비).
 *
 * === 주요 함수/구조체 요약 ===
 * __bio_integrity_action()   : READ/WRITE에 따라 host 검증/생성 필요 여부와
 *                              NVMe offload 가능 여부를 판정(BI_ACT_* 비트 반환)
 * bio_integrity_alloc()      : bio_integrity_payload + 가변 길이 bvec 배열을
 *                              한 번에 할당해 bio에 연결
 * bio_integrity_alloc_buf()  : 커널 소유 metadata 버퍼를 kmalloc 또는 mempool로
 *                              확보해 bip_vec[0]에 연결
 * bio_integrity_map_user()   : 사용자 공간 iov_iter를 pin/DMA 정렬 검사 후
 *                              직접 매핑하거나 bounce 버퍼로 복사
 * bio_integrity_advance()    : 부분 완료된 데이터 바이트 수에 대응하는 metadata
 *                              바이트만큼 iterator/RefTag seed를 전진
 * bio_integrity_trim()       : 복제된 bio의 metadata iterator 크기를 실제
 *                              데이터 섹터 수에 맞춰 재계산
 * bio_integrity_clone()      : 원본 bio의 bip 상태(iterator/seed/flags/app_tag)를
 *                              새 bio로 복제
 *
 * struct bio_integrity_payload(bip) 요약(상세는 하단 참조 블록):
 *   bip_iter     : metadata 진행 위치(offset) 및 RefTag seed(bi_sector)
 *   bip_vcnt     : 현재 채워진 metadata bio_vec 개수
 *   bip_max_vcnt : metadata bio_vec 배열의 최대 슬롯 수
 *   bip_flags    : 검증/버퍼 관리 상태 비트마스크(enum bip_flags)
 *   app_tag      : T10 PI Application Tag 값
 *   bip_vec      : 실제 metadata 물리 페이지들을 가리키는 bio_vec 배열
 */

#include <linux/blk-integrity.h>	/* [한국어] struct blk_integrity, blk_get_integrity(), BLK_INTEGRITY_* 플래그,
					 * queue_max_integrity_segments() 등 — 디스크/큐 단위 PI 프로파일 조회에 필수.
					 * NVMe에서는 namespace format(NVM Format)의 metadata 크기/타입이 이 구조체로 노출된다. */
#include <linux/t10-pi.h>		/* [한국어] T10 PI(Protection Information) tuple 레이아웃 상수/헬퍼.
					 * SCSI DIF와 NVMe DIX가 공유하는 Guard/AppTag/RefTag 8바이트 tuple 정의가 포함되어
					 * 있으며, 이 파일이 직접 CRC를 계산하지는 않지만 관련 상수(csum_type 판별 등)에 의존한다. */
#include "blk.h"			/* [한국어] block layer 내부(비공개) 헤더 — bio_integrity_prep()/request 병합
					 * 헬퍼 등, 모듈 외부에 노출되지 않는 blk-mq 연동 인터페이스 선언을 가져온다. */

/*
 * [한국어] struct bio_integrity_payload 필드 참조
 * ----------------------------------------------------------------------------
 * 아래는 include/linux/bio-integrity.h에 정의된(본 sparse checkout에는 실물이
 * 없는) struct bio_integrity_payload의 필드를 §4 규칙에 따라 정리한 것이다.
 * 이 파일의 모든 함수가 bip-> 형태로 이 구조체를 읽고 쓰므로, 아래 문서를
 * 먼저 숙지하면 개별 함수의 인라인 주석을 다른 파일로 점프하지 않고도 이해할
 * 수 있다.
 *
 *   struct bio_integrity_payload {
 *           struct bvec_iter bip_iter;
 *           unsigned short   bip_vcnt;
 *           unsigned short   bip_max_vcnt;
 *           unsigned short   bip_flags;
 *           u16              app_tag;
 *           struct bio_vec  *bip_vec;
 *   };
 *
 * bip_iter
 *   역할: metadata에 대한 현재 진행 위치를 추적하는 반복자. bi_sector(RefTag
 *     seed로 재사용됨), bi_size(잔여 metadata 바이트 수), bi_idx(bip_vec 배열
 *     내 현재 인덱스), bi_bvec_done(현재 bvec 내 처리된 바이트 수) 서브필드로
 *     구성된다(struct bvec_iter 정의는 blk_types.h, 본 파일에는 없음).
 *   설정자: bio_integrity_alloc_buf()/bio_integrity_init_user()/
 *     bio_integrity_copy_user()가 bi_size를 최초 설정, bio_integrity_setup_default()
 *     와 bip_set_seed()가 bi_sector를 RefTag seed로 설정, bio_integrity_advance()가
 *     부분 완료 시 bi_sector/bi_size를 전진.
 *   읽는 자: bio_integrity_trim()이 bi_size를 재계산해 clone bio에 맞추고,
 *     NVMe 경로에서는 nvme_setup_rw()가 이 값을 기반으로 SQE의 reftag
 *     필드를 채운다(core.c의 set_ref_tag 경로).
 *   값 범위: bi_sector는 SLBA(Starting LBA) 단위 섹터 번호, bi_size는
 *     0 ~ bio 전체 metadata 바이트 수.
 *   동기화: 하나의 bio는 한 시점에 하나의 I/O 경로(submit 또는 completion)
 *     에서만 다뤄지므로 별도 락이 불필요하다. 단, split된 자식 bio들은 각자
 *     독립된 bip_iter를 가지므로(clone 시 값만 복사) 서로 간섭하지 않는다.
 *
 * bip_vcnt
 *   역할: bip_vec 배열에 현재 유효하게 채워진 bio_vec(세그먼트) 개수.
 *   설정자: bio_integrity_alloc_buf()가 1로 고정 설정(단일 커널 버퍼),
 *     bio_integrity_add_page()가 세그먼트 추가 시마다 증가, bio_integrity_copy_user()
 *     가 bounce 버퍼 사용 후 원본 세그먼트 수로 재설정.
 *   읽는 자: bio_integrity_add_page()가 병합/segment 한도 검사에 사용하고,
 *     NVMe에서는 nvme_map_metadata()가 이 개수로 MPTR 단일 주소 경로와
 *     메타데이터 SGL 경로 중 하나를 고른다.
 *   값 범위: 0 ~ bip_max_vcnt.
 *   동기화: bio를 소유한 스레드에서만 갱신되므로 별도 동기화가 불필요하다.
 *
 * bip_max_vcnt
 *   역할: bip_vec 배열이 담을 수 있는 최대 세그먼트 수(슬롯 수) — 할당
 *     시점에 고정되는 상한.
 *   설정자: bio_integrity_init()이 nr_vecs 인자로 최초 설정(bio_integrity_alloc()
 *     이 이 함수를 호출).
 *   읽는 자: bio_integrity_add_page()가 세그먼트 추가 가능 여부 판단에
 *     queue_max_integrity_segments()와 min()으로 비교하고, bio_integrity_uncopy_user()
 *     가 원본 벡터 개수(bip_max_vcnt - 1)를 역산하는 데 사용한다.
 *   값 범위: 0(클론 시 벡터를 공유해 별도 슬롯 불필요) 또는 호출자가 요청한
 *     nr_vecs 값.
 *   동기화: 할당 시 한 번 설정된 후 불변(immutable)이므로 이후에는 읽기
 *     전용으로만 접근된다.
 *
 * bip_flags
 *   역할: enum bip_flags 비트마스크. BIP_BLOCK_INTEGRITY/BIP_MAPPED_INTEGRITY/
 *     BIP_DISK_NOCHECK/BIP_IP_CHECKSUM/BIP_COPY_USER/BIP_CHECK_GUARD/
 *     BIP_CHECK_REFTAG/BIP_CHECK_APPTAG 및 이 파일이 사용하는 BIP_MEMPOOL 등
 *     PI 검증 방식과 버퍼 관리 상태를 나타내는 비트들의 조합이다.
 *   설정자: bio_integrity_setup_default()/bio_uio_meta_to_bip()가 BIP_CHECK_*
 *     비트를 채우고, bio_integrity_alloc_buf()가 mempool 경로 사용 시
 *     BIP_MEMPOOL을, bio_integrity_copy_user()가 BIP_COPY_USER를 설정한다.
 *   읽는 자: bio_integrity_free_buf()가 BIP_MEMPOOL로 회수 경로를 선택하고,
 *     bio_integrity_unmap_user()가 BIP_COPY_USER로 uncopy 필요 여부를 판단하며,
 *     NVMe에서는 nvme_setup_rw()(core.c)가 이 비트들을 그대로 SQE의
 *     PRINFO 필드로 옮긴다:
 *       BIP_CHECK_GUARD  → NVME_RW_PRINFO_PRCHK_GUARD
 *       BIP_CHECK_REFTAG → NVME_RW_PRINFO_PRCHK_REF
 *       BIP_CHECK_APPTAG → NVME_RW_PRINFO_PRCHK_APP
 *     즉 이 플래그가 "컨트롤러에게 무엇을 검증하라고 지시할지"를 결정한다.
 *   값 범위: 위 enum bip_flags 값들의 OR 조합. clone 시에는 BIP_CLONE_FLAGS로
 *     마스킹된 부분집합만 전파된다.
 *   동기화: bio를 소유한 스레드에서만 수정되므로 별도 락이 불필요하다.
 *
 * app_tag
 *   역할: Application Tag 값 — T10 PI tuple의 2바이트 App Tag 필드에 대응하며,
 *     상위 계층(파일시스템/애플리케이션)이 자유롭게 정의해 사용할 수 있는 태그.
 *   설정자: bio_uio_meta_to_bip()가 uio_meta->app_tag로부터 복사하고,
 *     bio_integrity_clone()이 원본 bip_src->app_tag를 그대로 복제한다.
 *   읽는 자: NVMe에서는 nvme_setup_rw()가 BIP_CHECK_APPTAG가 설정된 경우
 *     PRINFO의 PRCHK_APP 비트를 세우고 이 값을 apptag 필드에 실어 보낸다.
 *     그러면 컨트롤러가 매체의 App Tag와 이 값을 대조해 검증한다.
 *   값 범위: 0 ~ 0xFFFF. NVMe 사양에서 App Tag가 0xFFFF이면 해당 논리 블록의
 *     PI 검사를 생략하라는 뜻이며(T10 DIF도 동일 관례), 초기화되지 않은
 *     블록을 읽을 때 오탐을 막는 데 쓰인다.
 *   동기화: bio를 소유한 스레드에서만 설정되므로 별도 락이 불필요하다.
 *
 * bip_vec
 *   역할: 실제 integrity metadata가 담긴 물리 페이지들을 가리키는 bio_vec
 *     배열의 포인터 — bip_iter와 짝을 이루어 순회한다(bip_for_each_vec 매크로).
 *   설정자: bio_integrity_init()이 struct bio_integrity_alloc의 유연 배열
 *     멤버(bia->bvecs[])를 가리키도록 설정하거나, bio_integrity_clone()이
 *     원본의 포인터를 그대로 공유하도록 설정한다.
 *   읽는 자: bio_integrity_add_page()/bvec_from_pages()가 세그먼트를 채우고,
 *     NVMe에서는 nvme_map_metadata()가 이 배열을 순회하며 DMA 매핑을 수행하고,
 *     bio_integrity_uncopy_user()가 원본 사용자 bvec 복원에 사용한다.
 *   값 범위: 유효한 bio_vec 배열 포인터(NULL 불가). 단 bip_max_vcnt == 0인
 *     clone은 원본과 포인터를 공유한다.
 *   동기화: 클론된 bio들이 이 포인터를 공유할 수 있으므로, 원본 bio가 clone
 *     보다 먼저 해제되어서는 안 된다. bio_integrity_clone()이 bip_max_vcnt를
 *     0으로 두는 것이 "이 배열의 소유권이 없다"는 표시이며,
 *     bio_integrity_free()가 그 값을 보고 해제를 건너뛴다.
 * ----------------------------------------------------------------------------
 */

/*
 * [한국어]
 * struct bio_integrity_alloc - bip와 가변 길이 bvec 배열을 한 번에 담는 내장 컨테이너
 *
 * bio_integrity_alloc()이 kmalloc_flex()로 이 구조체 전체(고정 헤더 bip +
 * 가변 길이 bvecs[])를 단일 메모리 블록으로 할당하기 위해 사용하는 내부 전용
 * 래퍼다. 이렇게 하나로 묶어 할당하면 bip와 bvec 배열 사이에 추가 포인터
 * 추적이나 별도 해제 경로가 필요 없어, NVMe I/O 핫패스에서 할당 오버헤드가
 * 줄어든다. bio_integrity_alloc()의 반환값은 &bia->bip이며, 호출자는 이
 * struct bio_integrity_alloc의 존재를 알지 못한 채 struct bio_integrity_payload
 * 포인터만 사용한다(container_of류 은닉 패턴).
 */
struct bio_integrity_alloc {
	struct bio_integrity_payload	bip;
	/* [한국어] 이 얼록의 본체가 되는 integrity payload.
	 * 설정자: bio_integrity_alloc()이 kmalloc_flex() 직후 bio_integrity_init()으로
	 *   0 초기화 및 bio 연결을 수행.
	 * 읽는 자: bio_integrity_alloc()의 반환값(&bia->bip)을 통해 이 파일의
	 *   나머지 모든 함수(add_page/advance/trim/clone 등)가 접근.
	 * 값 범위: 유효한 bio_integrity_payload — NVMe 관점에서는 Guard/RefTag/
	 *   AppTag 상태와 metadata bvec 목록을 함께 들고 있는 핵심 상태.
	 * 동기화: 이 구조체를 소유한 bio와 생명주기를 같이하며, 별도 락 없이
	 *   해당 bio를 다루는 단일 컨텍스트에서만 접근된다. */

	struct bio_vec			bvecs[];
	/* [한국어] metadata 페이지들을 위한 가변 길이(flexible array) bvec 배열.
	 * 설정자: bio_integrity_alloc()이 nr_vecs 개수만큼 할당 공간을 확보하고,
	 *   bio_integrity_init()이 bip.bip_vec이 이 배열을 가리키도록 연결.
	 * 읽는 자: bio_integrity_add_page()/bio_integrity_init_user() 등이
	 *   bip->bip_vec을 통해 이 배열 원소에 페이지/길이/오프셋을 채운다.
	 * 값 범위: 원소 0개(clone 시 nr_vecs=0으로 별도 배열 없이 원본 공유) ~
	 *   nr_vecs개의 bio_vec. NVMe에서는 nvme_map_metadata()가 이 배열을 순회해
 *   메타데이터 SGL 디스크립터를 만들거나(여러 개인 경우), 단일 MPTR
 *   주소로 변환한다(하나뿐인 경우).
	 * 동기화: bip 필드와 동일하게 해당 bio를 다루는 단일 컨텍스트에서만
	 *   접근되므로 별도 락이 필요 없다. */
};

static mempool_t integrity_buf_pool;
/* [한국어] integrity metadata 커널 버퍼 할당이 실패했을 때 사용하는 전역
 * fallback mempool.
 * 설정자: bio_integrity_initfn()이 subsys_initcall 단계에서 페이지 기반
 *   mempool로 1회 초기화(BIO_POOL_SIZE개의 예약 페이지, BLK_INTEGRITY_MAX_SIZE
 *   크기 단위).
 * 읽는 자: bio_integrity_alloc_buf()가 kmalloc()이 실패했을 때 mempool_alloc()
 *   으로 예약 페이지를 꺼내고, bio_integrity_free_buf()가 BIP_MEMPOOL 플래그를
 *   보고 mempool_free()로 반환한다.
 * 값 범위: mempool_t는 내부적으로 락과 대기열을 가진 불투명 구조체이며,
 *   페이지 풀 크기는 초기화 시 고정된다.
 * 동기화: mempool 자체가 내부 스핀락으로 동시 접근을 보호하므로, 이 파일의
 *   호출자는 별도 락 없이 mempool_alloc()/mempool_free()를 호출해도 된다.
 *   NVMe I/O 경로에서 GFP_NOIO/GFP_NOFS로도 항상 진행이 보장되도록 하는
 *   안전망 역할이다 — mempool은 예약된 최소 개수를 미리 확보해 두므로
 *   메모리가 고갈되어도 진행 중인 I/O가 완료될 만큼은 항상 할당된다. */

/*
 * [한국어]
 * bi_offload_capable - 컨트롤러가 PI 생성/검증을 하드웨어로 offload할 수 있는지 판단
 *
 * @bi: 대상 디스크/네임스페이스의 blk_integrity 프로파일. metadata_size와
 *      pi_tuple_size를 비교 대상으로 사용한다.
 * @return: true면 컨트롤러가 PI tuple 전체를 metadata로 취급해 하드웨어에서
 *      생성/검증이 가능함을 의미하고, false면 metadata 안에 PI 이외의 데이터가
 *      섞여 있어 host 소프트웨어 개입이 필요함을 의미한다.
 *
 * NVMe End-to-End Data Protection 스펙에서, namespace format의 metadata 크기가
 * PI tuple 크기(전형적으로 8바이트: Guard 2 + AppTag 2 + RefTag 4)와 정확히
 * 같다면 컨트롤러가 metadata 버퍼 전체를 PI로 해석해 PRACT(Protection
 * Information Action)/PRCHK(Protection Information Check) 비트만으로 생성·
 * 검증을 수행할 수 있다. metadata_size가 더 크면(예: 사용자 정의 메타데이터가
 * 추가된 포맷) host가 어느 구간이 PI인지 별도로 관리해야 하므로 offload가
 * 불가능하다고 간주한다.
 * 이 함수는 순수 계산 함수로 락이나 부수효과가 없으며, 재진입/동시호출에
 * 안전하다. __bio_integrity_action()이 READ/WRITE 각각의 NOVERIFY/NOGENERATE
 * 분기에서 이 함수를 호출해 offload 가능 여부를 확인한다.
 *
 * 호출 체인:
 *   __bio_integrity_action() → [bi_offload_capable] → (없음, 리프 함수)
 */
static bool bi_offload_capable(struct blk_integrity *bi)
{
	/* [한국어] metadata_size(네임스페이스 포맷상 전체 metadata 바이트 수)와
	 * pi_tuple_size(PI 8바이트 tuple 크기)가 정확히 일치하면 metadata 버퍼 전체가
	 * PI이므로 컨트롤러에 검증/생성을 통째로 맡길 수 있다.
	 * 반대로 metadata_size > pi_tuple_size이면 PI 8바이트 외에 사용자/제조사
	 * 전용 영역이 섞여 있어, 그 부분은 호스트가 직접 채워야 하므로 완전
	 * offload가 불가능하다. */
	return bi->metadata_size == bi->pi_tuple_size;
}

/*
 * [한국어]
 * __bio_integrity_action - 현재 bio에 대해 소프트웨어 PI 처리 방식을 결정
 *
 * @bio: PI 처리 여부를 판정할 대상 bio. bio_op()으로 READ/WRITE를 구분하고,
 *      bio->bi_bdev->bd_disk에서 blk_integrity 프로파일을 얻는다.
 * @return: BI_ACT_* 비트마스크(BI_ACT_BUFFER: 버퍼 준비 필요, BI_ACT_CHECK:
 *      소프트웨어 검증 필요, BI_ACT_ZERO: 미초기화 영역을 0으로 채워야 함).
 *      0이면 host가 아무 작업도 할 필요가 없고 NVMe 컨트롤러 offload에
 *      전적으로 의존함을 뜻한다.
 *
 * 파일시스템이 생성한 bio가 REQ_OP_READ/WRITE인지에 따라, host(소프트웨어)가
 * Guard/RefTag/AppTag를 생성·검증해야 하는지, 아니면 NVMe 컨트롤러 하드웨어
 * offload에 맡길 수 있는지를 판정하는 것이 이 함수의 목적이다. blk_integrity의
 * BLK_INTEGRITY_NOVERIFY/BLK_INTEGRITY_NOGENERATE 플래그와 bi_offload_capable()
 * 결과를 조합해 READ는 검증 필요 여부를, WRITE는 생성 필요 여부를 판단한다.
 * 이 함수는 순수 판정 함수로 상태를 변경하지 않으며, submit_bio 경로의 태스크
 * 컨텍스트에서 매 bio마다 호출되므로 재진입에 안전해야 한다(락 없음, 부수효과
 * 없음). 상위 호출자는 blk-mq의 무결성 준비 경로이며,
 * 반환된 비트에 따라 실제 버퍼 준비(bio_integrity_alloc_buf() 등)를 이어서
 * 수행한다. bio_has_crypt_ctx()로 감지되는 암호화 컨텍스트와의 동시 사용은
 * 현재 지원되지 않으므로 WARN_ON_ONCE로 경고 후 0(처리 없음)을 반환한다.
 *
 * 호출 체인:
 *   submit_bio → blk_mq_submit_bio → blk_mq_get_request
 *   → nvme_queue_rq → nvme_setup_cmd → [__bio_integrity_action]
 *   → bio_integrity_alloc_buf (필요 시)
 */
unsigned int __bio_integrity_action(struct bio *bio)
{
	/* [한국어] bio가 속한 디스크의 integrity profile 획득 — NVMe namespace
	 * format(metadata_size, pi_tuple_size, csum_type, flags)에서 유래한다. */
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);

	/*
	 * Encrypted bios can't have integrity metadata.
	 */
	/* [한국어] 암호화(inline crypto)와 integrity를 동시에 처리하는 조합은 현재
	 * 지원하지 않는다 — 개발자 실수(버그) 탐지를 위해 WARN_ON_ONCE로 1회만 경고. */
	if (WARN_ON_ONCE(bio_has_crypt_ctx(bio)))
		return 0;	/* [한국어] 암호화 컨텍스트가 있으면 PI 처리를 하지 않고 즉시 종료 */

	/* [한국어] bio의 연산 종류(READ/WRITE/기타)에 따라 처리 방식이 완전히
	 * 다르므로 여기서 분기한다 — READ는 검증 관점, WRITE는 생성 관점. */
	switch (bio_op(bio)) {
	case REQ_OP_READ:
		/* [한국어] READ 방향: 디스크에서 읽어온 데이터의 PI를 검증할지 결정 */
		if (bi->flags & BLK_INTEGRITY_NOVERIFY) {
			/* [한국어] 프로파일이 "검증하지 않음"으로 설정된 경우 —
			 * 그래도 offload 가능 여부는 별개로 확인해야 버퍼 준비 필요성을
			 * 판단할 수 있다. */
			if (bi_offload_capable(bi))
				/* [한국어] offload 가능 + NOVERIFY: 컨트롤러도 검증하지
				 * 않고 host도 검증하지 않으므로 아무 처리도 필요 없다. */
				return 0;
			/* [한국어] offload 불가하지만 검증도 안 함: 그래도 controller가
			 * metadata를 채워 반환하므로 버퍼(BI_ACT_BUFFER)는 준비해야 한다. */
			return BI_ACT_BUFFER;
		}
		/* [한국어] NOVERIFY가 아니면 host가 항상 소프트웨어로 Guard/RefTag를
		 * 검증해야 한다 — 버퍼 준비 + 체크 두 동작 모두 필요. */
		return BI_ACT_BUFFER | BI_ACT_CHECK;
	case REQ_OP_WRITE:
		/*
		 * Flush masquerading as write?
		 */
		/* [한국어] flush 성격의 write 요청인지 확인 — 실제 데이터 섹터가
		 * 0이면(예: REQ_PREFLUSH만 있는 빈 쓰기) integrity metadata도
		 * 필요 없다. NVMe FLUSH 커맨드는 데이터/메타데이터 없이 휘발성
		 * 쓰기 버퍼(volatile write cache)만 durable하게 만든다. */
		if (!bio_sectors(bio))
			return 0;	/* [한국어] 섹터 수 0: PI 처리 대상 데이터가 없으므로 즉시 종료 */

		/*
		 * Zero the memory allocated to not leak uninitialized kernel
		 * memory to disk for non-integrity metadata where nothing else
		 * initializes the memory.
		 */
		/* [한국어] 초기화되지 않은 커널 메모리가 디스크로 유출되는 것을 막기
		 * 위해, host가 직접 PI를 생성하지 않는 경로에서는 버퍼를 0으로
		 * 채워야 한다 — NVMe WRITE에서 metadata 버퍼가 쓰레기 값이면 SSD
		 * 측 Guard 검증이 예기치 않게 실패하거나, 임의 커널 데이터가
		 * 디스크에 영구 기록되는 정보 유출이 발생할 수 있다. */
		if (bi->flags & BLK_INTEGRITY_NOGENERATE) {
			/* [한국어] "생성하지 않음" 프로파일: 컨트롤러가 대신 PI를
			 * 만들 수 있는지(offload 가능 여부)를 확인한다. */
			if (bi_offload_capable(bi))
				/* [한국어] offload 가능 + NOGENERATE: 컨트롤러가 Guard/
				 * AppTag/RefTag를 전부 생성하므로 host는 아무 것도 안 함. */
				return 0;
			/* [한국어] offload 불가: host는 버퍼만 0으로 채우고(BI_ACT_ZERO)
			 * 실제 PI 생성은 여전히 컨트롤러에 위임한다. 이 경로에 오는 것은
			 * metadata_size > pi_tuple_size, 즉 PI 외 영역이 있는 포맷이라
			 * bi_offload_capable()이 거짓이 된 경우다. */
			return BI_ACT_BUFFER | BI_ACT_ZERO;
		}

		/* [한국어] metadata_size가 pi_tuple_size보다 크면, metadata 안에
		 * PI 8바이트 외의 사용자/제조사 전용 영역이 섞여 있다는 뜻이다.
		 * 이 경우 host가 PI 부분은 체크섬 생성(BI_ACT_CHECK)하면서, 나머지
		 * 비-PI 영역은 커널 정보 유출 방지를 위해 0으로 채워야(BI_ACT_ZERO)
		 * 한다. */
		if (bi->metadata_size > bi->pi_tuple_size)
			return BI_ACT_BUFFER | BI_ACT_CHECK | BI_ACT_ZERO;
		/* [한국어] 일반적인 WRITE 경로: host가 버퍼를 준비하고 PI를
		 * 생성(체크섬 계산)한 뒤 NVMe metadata SGL/PRP로 전송한다. */
		return BI_ACT_BUFFER | BI_ACT_CHECK;
	default:
		/* [한국어] READ/WRITE가 아닌 다른 연산(예: DISCARD 등)은 NVMe
		 * 명령에 metadata 자체가 필요 없으므로 처리 없음을 반환. */
		return 0;
	}
}
EXPORT_SYMBOL_GPL(__bio_integrity_action);	/* [한국어] blk-mq.c 등 다른 컴파일
					 * 유닛(및 향후 모듈)에서 이 심볼을 호출할 수 있도록 공개.
					 * _GPL 접미사는 GPL 호환 모듈만 사용 가능함을 의미한다. */

/*
 * [한국어]
 * bio_integrity_alloc_buf - integrity 메타데이터를 담을 커널 버퍼를 할당
 *
 * @bio: 버퍼를 붙일 대상 bio. bio_sectors(bio)로 필요한 metadata 길이를
 *      계산하고, bio_integrity(bio)로 이미 연결된 bip을 얻는다.
 * @zero_buffer: true면 할당된 버퍼를 0으로 초기화한다(__bio_integrity_action()이
 *      BI_ACT_ZERO를 반환한 WRITE 경로에서 사용).
 * @return: 없음(void). 실패 시에도 mempool_alloc()이 블로킹하며 반드시 성공을
 *      보장하므로(GFP_NOFS, mempool은 항상 예약분을 갖고 있음) 이 함수는
 *      실패를 반환하지 않는 설계다.
 *
 * bio_sectors(bio)에 해당하는 metadata 바이트 수만큼 커널 메모리를 확보해
 * bip->bip_vec[0]에 단일 세그먼트로 연결하는 것이 이 함수의 목적이다. NVMe
 * WRITE/READ의 metadata SGL/PRP는 데이터 버퍼와 별도로 구성되므로, 여기서
 * 확보한 버퍼가 그 metadata pointer가 되는 실제 물리 페이지다. 먼저
 * direct reclaim을 끈 kmalloc()으로 빠른 경로를 시도하고, 실패하면(고메모리
 * 압박 상황) integrity_buf_pool mempool에서 페이지를 꺼내는 2단계 전략을
 * 사용한다 — NVMe I/O 제출 경로(예: nvme_queue_rq())에서 reclaim에 의한 지연
 * 없이 항상 진행이 보장되어야 하기 때문이다. __bio_integrity_action()이
 * BI_ACT_BUFFER를 반환한 직후 호출되는 것이 일반적인 사용 패턴이다. 이
 * 함수는 태스크 컨텍스트에서 실행되며 GFP_NOIO를 사용하므로 파일시스템
 * reclaim 재진입은 없지만, 메모리 회수(reclaim) 자체는 발생할 수 있다.
 *
 * 호출 체인:
 *   __bio_integrity_action (block/bio-integrity-auto.c:635) → [bio_integrity_alloc_buf]
 *   → kmalloc / mempool_alloc → bvec_set_page
 */
void bio_integrity_alloc_buf(struct bio *bio, bool zero_buffer)
{
	/* [한국어] bio가 속한 disk의 integrity profile — NVMe namespace format 기반. */
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);
	/* [한국어] bio에 이미 연결되어 있는 integrity payload — 호출 전 bio_integrity_alloc()
	 * 등으로 미리 붙어 있어야 한다. */
	struct bio_integrity_payload *bip = bio_integrity(bio);
	/* [한국어] 섹터 수를 metadata 바이트 수로 변환 — NVMe LBA 개수 * metadata_size 공식. */
	unsigned int len = bio_integrity_bytes(bi, bio_sectors(bio));
	/* [한국어] zero_buffer가 true면 __GFP_ZERO를 추가해 할당과 동시에 0으로 채운다
	 * (NVMe NOGENERATE/ZERO 경로에서 커널 메모리 유출 방지 목적). */
	gfp_t gfp = GFP_NOIO | (zero_buffer ? __GFP_ZERO : 0);
	void *buf;	/* [한국어] 최종 확보된 metadata 버퍼의 커널 가상 주소를 담을 변수 */

	/*
	 * Using multiple pages can cause the same performance concerns as
	 * regular kmalloc. Guard against large sizes for now.
	 */
	/* [한국어] 일반 kmalloc 시도: direct reclaim(__GFP_DIRECT_RECLAIM)을 끄고,
	 * OOM killer 유발(__GFP_NOMEMALLOC/__GFP_NORETRY)과 경고 로그(__GFP_NOWARN)를
	 * 억제해 "실패해도 즉시 반환"하는 저비용 빠른 경로로 시도한다. NVMe I/O
	 * 경로에서는 GFP_NOIO를 유지해야 nvme_queue_rq → doorbell 제출이 reclaim
	 * 대기로 지연되지 않는다. */
	buf = kmalloc(len, (gfp & ~__GFP_DIRECT_RECLAIM) |
			__GFP_NOMEMALLOC | __GFP_NORETRY | __GFP_NOWARN);
	if (unlikely(!buf)) {
		/* [한국어] 빠른 kmalloc이 실패한 드문 경우(메모리 압박) — mempool
		 * fallback 경로로 진입, 이 경로는 반드시 성공한다. */
		struct page *page;	/* [한국어] mempool에서 꺼낸 예약 페이지를 담을 포인터 */

		/*
		 * Try to allocate a bio integrity buffer using the last
		 * reserved page from the mempool, and if that fails, drop
		 * into the mempool waitqueue to wait for a page.
		 */
		/* [한국어] mempool fallback: integrity_buf_pool에서 미리 예약된 페이지를
		 * 꺼낸다 — GFP_NOFS로 파일시스템 재진입은 막되, 필요 시 다른 사용자가
		 * 페이지를 반환할 때까지 대기(block)할 수 있다. NVMe latency에 민감한
		 * 경로에서도 reclaim 없이 메모리를 확보하기 위한 최후의 안전장치다. */
		page = mempool_alloc(&integrity_buf_pool, GFP_NOFS);
		if (zero_buffer)
			/* [한국어] mempool 페이지는 재사용된 것이라 이전 내용이 남아
			 * 있을 수 있으므로, zero_buffer 요청 시 명시적으로 0으로 채운다. */
			memset(page_address(page), 0, len);
		/* [한국어] mempool 페이지를 bip_vec[0]에 등록 — offset 0부터 len 바이트. */
		bvec_set_page(&bip->bip_vec[0], page, len, 0);
		/* [한국어] BIP_MEMPOOL 플래그 설정: 이후 bio_integrity_free_buf()가
		 * kfree() 대신 mempool_free()로 회수하도록 표시. */
		bip->bip_flags |= BIP_MEMPOOL;
	} else {
		/* [한국어] kmalloc 성공: virtual address(buf)를 물리 page + page 내
		 * offset으로 변환해 bvec에 설정 — DMA/스캐터-게더 리스트 구성에
		 * 필요한 표준 변환. */
		bvec_set_page(&bip->bip_vec[0], virt_to_page(buf), len,
				offset_in_page(buf));
	}

	/* [한국어] 이 함수는 항상 단일 bvec(세그먼트 1개)에 전체 metadata를 담으므로
	 * bip_vcnt를 1로 고정 — NVMe metadata pointer 1개로 매핑됨을 의미. */
	bip->bip_vcnt = 1;
	/* [한국어] 전체 metadata 바이트 수를 iterator 크기에 반영 — 이후
	 * bio_integrity_advance()/bio_integrity_trim()이 이 값을 기준으로 진행. */
	bip->bip_iter.bi_size = len;
}

/*
 * [한국어]
 * bio_integrity_free_buf - bio_integrity_alloc_buf()가 할당한 버퍼를 해제
 *
 * @bip: 해제 대상 integrity payload. bip_flags의 BIP_MEMPOOL 비트로 어떤
 *      할당 경로였는지(kmalloc vs mempool)를 판별한다.
 * @return: 없음(void).
 *
 * bio_integrity_alloc_buf()가 확보한 단일 metadata 버퍼(bip_vec[0])를 원래
 * 출처에 맞게 되돌리는 것이 목적이다 — kmalloc으로 받았으면 kfree(), mempool
 * 페이지였으면 mempool_free()로 반환해야 mempool의 예약분이 다시 채워진다.
 * 잘못된 경로로 해제하면(예: mempool 페이지를 kfree) 메모리 커럽션이나
 * mempool 예약분 고갈로 이어질 수 있다. NVMe 관점에서는 완료(CQ) 처리 후
 * bio_endio() 경로를 통해 metadata 버퍼를 회수하는 정리 단계에서 호출된다
 * (blk_mq_complete_request → nvme_complete_rq → bio_endio → ... →
 * bio_integrity_free_buf). 이 함수는 완료 컨텍스트(태스크 또는
 * 소프트 IRQ)에서 호출될 수 있으며, 자체적으로 락을 잡지 않는다.
 *
 * 호출 체인:
 *   bio_endio → bio_integrity_endio → bio_integrity_verify_fn
 *     (block/bio-integrity-auto.c:298) → [bio_integrity_free_buf]
 *     → kfree / mempool_free
 */
void bio_integrity_free_buf(struct bio_integrity_payload *bip)
{
	/* [한국어] 이 함수가 다루는 유일한 세그먼트 — bio_integrity_alloc_buf()가
	 * 항상 bip_vec[0] 하나에만 버퍼를 채웠으므로 인덱스 0 고정. */
	struct bio_vec *bv = &bip->bip_vec[0];

	/* [한국어] BIP_MEMPOOL 플래그로 할당 경로 판별 — 설정 시 mempool에서
	 * 꺼낸 페이지였다는 뜻. */
	if (bip->bip_flags & BIP_MEMPOOL)
		/* [한국어] mempool 페이지를 반환 — 다른 대기자가 재사용할 수 있도록
		 * 예약 풀에 되돌린다. */
		mempool_free(bv->bv_page, &integrity_buf_pool);
	else
		/* [한국어] 일반 kmalloc 버퍼였으므로 가상 주소로 되돌려 kfree — bvec_virt()가
		 * page+offset을 다시 가상 주소로 변환해 준다. */
		kfree(bvec_virt(bv));
}

/*
 * [한국어]
 * bio_integrity_setup_default - 기본 integrity 검증 플래그와 RefTag seed를 설정
 *
 * @bio: 대상 bio. 이미 bio_integrity(bio)로 조회 가능한 bip이 붙어 있어야 한다.
 * @return: 없음(void).
 *
 * 별도의 사용자 지정 플래그 없이, 디스크의 blk_integrity 프로파일(csum_type,
 * BLK_INTEGRITY_REF_TAG 여부)만 보고 표준적인 BIP_CHECK_* 플래그 조합과
 * RefTag seed를 설정하는 "기본값 설정" 헬퍼다. bio 시작 섹터(bi_iter.bi_sector)를
 * RefTag seed로 사용하는데, 이는 NVMe PI Type 1/2/3 스펙에서 RefTag가 SLBA와
 * 연동되는 규약을 따른 것이다. NVMe PI Type 1/2에서 RefTag는 각 논리 블록의
 * LBA 하위 32비트와 일치해야 하고, Type 3은 검사하지 않는다.
 * csum_type이 설정되어 있으면
 * Guard 검증을(추가로 IP 체크섬 타입이면 BIP_IP_CHECKSUM도), REF_TAG 플래그가
 * 있으면 RefTag 검증을 활성화한다. 이 함수는 태스크 컨텍스트에서 bio 준비
 * 단계에 1회 호출되는 것이 일반적이며 락이 필요 없다.
 *
 * 호출 체인:
 *   blk_mq_submit_bio → bio_integrity_prep → __bio_integrity_action
 *     (block/bio-integrity-auto.c:647) → [bio_integrity_setup_default]
 *   → bip_set_seed
 */
void bio_integrity_setup_default(struct bio *bio)
{
	/* [한국어] 디스크의 PI 프로파일(csum_type, REF_TAG 지원 여부 등) 조회 */
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);
	/* [한국어] 플래그/seed를 채워 넣을 대상 integrity payload */
	struct bio_integrity_payload *bip = bio_integrity(bio);

	/* [한국어] Reference Tag seed를 bio 시작 섹터로 설정 — NVMe SLBA(Starting LBA)와
	 * 직접 연결되는 값이다. */
	bip_set_seed(bip, bio->bi_iter.bi_sector);

	/* [한국어] 디스크가 체크섬(Guard) 타입을 사용하도록 설정되어 있는지 확인 */
	if (bi->csum_type) {
		/* [한국어] Guard(체크섬) 검증이 필요함을 표시 — NVMe PRCHK[Guard]=1에 대응. */
		bip->bip_flags |= BIP_CHECK_GUARD;
		/* [한국어] 체크섬 종류가 IP 체크섬이면 계산 알고리즘이 다르므로 별도
		 * 플래그로 구분해 두어야 검증 함수가 올바른 알고리즘을 선택한다. */
		if (bi->csum_type == BLK_INTEGRITY_CSUM_IP)
			bip->bip_flags |= BIP_IP_CHECKSUM;
	}
	/* [한국어] 디스크 프로파일이 Reference Tag 사용을 요구하는지 확인 */
	if (bi->flags & BLK_INTEGRITY_REF_TAG)
		/* [한국어] Reference Tag 검증이 필요함을 표시 — NVMe PRCHK[RefTag]=1에 대응. */
		bip->bip_flags |= BIP_CHECK_REFTAG;
}

/**
 * bio_integrity_free - Free bio integrity payload
 * @bio:	bio containing bip to be freed
 *
 * Description: Free the integrity portion of a bio.
 */
/*
 * [한국어]
 * bio_integrity_free - bio에 붙은 integrity payload를 해제
 *
 * @bio: bip을 해제할 대상 bio.
 * @return: 없음(void).
 *
 * bio_integrity_alloc()이 할당한 bio_integrity_payload(과 그에 딸린
 * struct bio_integrity_alloc 전체 블록)를 kfree()로 해제하고, bio에서
 * REQ_INTEGRITY 플래그와 bi_integrity 포인터를 제거해 "이 bio는 더 이상
 * integrity metadata를 갖지 않는다"는 상태로 되돌리는 것이 목적이다. 이
 * 함수는 bio_integrity_copy_user()의 실패 경로 및 일반적인 bio 소멸 경로
 * (bio_uninit(), block/bio.c:515)에서 호출된다. NVMe 관점에서는 REQ_INTEGRITY가
 * 클리어되면 NVMe 드라이버가 이 bio에서 유래한 request를 metadata 없는
 * 일반 명령으로 취급하게 된다. 별도 락 없이 bio를 소유한 컨텍스트에서만
 * 호출되어야 한다(다른 스레드가 동시에 이 bio의 bi_integrity를 사용 중이면
 * use-after-free 위험이 있다).
 *
 * 호출 체인:
 *   bio_uninit (block/bio.c:515) 또는 bio_integrity_unmap_user
 *     (block/bio-integrity.c:1110) → [bio_integrity_free]
 *   → kfree
 */
void bio_integrity_free(struct bio *bio)
{
	/* [한국어] bio_integrity_alloc()에서 kmalloc_flex()로 할당했던
	 * struct bio_integrity_alloc(bip 포함) 전체 블록을 해제. */
	kfree(bio_integrity(bio));
	/* [한국어] bio가 더 이상 유효한 integrity 포인터를 참조하지 않도록
	 * NULL로 초기화 — 해제된 메모리에 대한 dangling pointer 방지. */
	bio->bi_integrity = NULL;
	/* [한국어] REQ_INTEGRITY 플래그 클리어 — 이후 bio_integrity(bio)는
	 * 항상 NULL을 반환하게 되며, NVMe 드라이버는 metadata 없는 일반 명령으로 처리. */
	bio->bi_opf &= ~REQ_INTEGRITY;
}

/*
 * [한국어]
 * bio_integrity_init - 새로 할당된 bip를 0으로 초기화하고 bio에 연결
 *
 * @bio: bip를 연결할 대상 bio.
 * @bip: 아직 초기화되지 않은(호출자가 막 할당한) integrity payload.
 * @bvecs: bip가 사용할 bio_vec 배열의 시작 주소. nr_vecs가 0이면 NULL이거나
 *      무시될 수 있다(clone처럼 원본 배열을 나중에 별도로 공유하는 경우).
 * @nr_vecs: bvecs 배열이 담을 수 있는 최대 세그먼트 수.
 * @return: 없음(void).
 *
 * bio_integrity_alloc()이 새 struct bio_integrity_alloc을 할당한 직후 항상
 * 호출하는 공통 초기화 루틴이다. memset으로 이전 메모리 잔재를 지운 뒤,
 * bip_max_vcnt와 bip_vec을 설정하고, bio 쪽에는 bi_integrity 포인터와
 * REQ_INTEGRITY 플래그를 세팅해 "이 bio는 PI metadata를 갖는다"는 사실을
 * blk-mq/NVMe 드라이버가 인식할 수 있게 한다. blk-mq는 REQ_INTEGRITY가
 * 설정된 request에 대해 integrity_segments 한도를 별도로 계산하고, NVMe
 * 드라이버는 명령어 조립 시 metadata 버퍼를 첨부해야 한다는 신호로
 * 해석한다. 이 함수는 태스크 컨텍스트에서 1회성으로 실행되며 락이
 * 필요 없다.
 *
 * 호출 체인:
 *   bio_integrity_alloc → [bio_integrity_init] → (없음, 필드 대입만 수행)
 */
void bio_integrity_init(struct bio *bio, struct bio_integrity_payload *bip,
		struct bio_vec *bvecs, unsigned int nr_vecs)
{
	/* [한국어] bip 전체를 0으로 초기화 — bip_vcnt/bip_flags/app_tag 등 모든
	 * 필드가 확정된 값 없이 남는 것을 방지하고, 새로 할당된 메모리의 이전
	 * 내용(재사용된 슬랩 등)이 잘못 해석되지 않도록 한다. */
	memset(bip, 0, sizeof(*bip));
	/* [한국어] 최대 integrity bvec 슬롯 수 설정 — NVMe metadata SGL의 최대
	 * segment 수 상한에 대응하는 값. */
	bip->bip_max_vcnt = nr_vecs;
	if (nr_vecs)
		/* [한국어] 세그먼트가 하나 이상 필요한 일반적인 경우에만 bvecs
		 * 배열을 연결 — nr_vecs가 0이면(clone 등) 배열을 연결하지 않고
		 * 나중에 별도로 bip_vec을 채운다. */
		bip->bip_vec = bvecs;

	/* [한국어] bio와 bip을 연결 — 이후 bio_integrity(bio) 호출이 이 bip을 반환. */
	bio->bi_integrity = bip;
	/* [한국어] REQ_INTEGRITY 플래그 설정 — blk-mq/NVMe 드라이버가 이 bio에
	 * metadata가 딸려 있음을 인식하게 되는 유일한 신호. */
	bio->bi_opf |= REQ_INTEGRITY;
}

/**
 * bio_integrity_alloc - Allocate integrity payload and attach it to bio
 * @bio:	bio to attach integrity metadata to
 * @gfp_mask:	Memory allocation mask
 * @nr_vecs:	Number of integrity metadata scatter-gather elements
 *
 * Description: This function prepares a bio for attaching integrity
 * metadata.  nr_vecs specifies the maximum number of pages containing
 * integrity metadata that can be attached.
 */
/*
 * [한국어]
 * bio_integrity_alloc - integrity payload를 할당해 bio에 연결
 *
 * @bio: metadata를 붙일 대상 bio.
 * @gfp_mask: 메모리 할당 플래그(GFP_KERNEL/GFP_NOIO 등 호출자가 실행 컨텍스트에
 *      맞게 지정).
 * @nr_vecs: 이 bip이 담을 수 있는 최대 integrity scatter-gather(bio_vec) 개수.
 * @return: 성공 시 새로 할당된 bio_integrity_payload 포인터, 실패 시
 *      ERR_PTR(-EOPNOTSUPP)(암호화 컨텍스트와 공존 불가) 또는
 *      ERR_PTR(-ENOMEM)(메모리 부족). 호출자는 IS_ERR()로 검사해야 한다.
 *
 * 이 파일의 거의 모든 상위 진입점(bio_integrity_alloc_buf 호출 전,
 * bio_integrity_copy_user/init_user, bio_integrity_clone 등)이 공통으로
 * 사용하는 할당 루틴이다. struct bio_integrity_alloc(bip 헤더 + 가변 길이
 * bvecs[])를 kmalloc_flex()로 한 번에 할당한 뒤 bio_integrity_init()으로
 * 0 초기화 및 bio 연결까지 마친다. 암호화 컨텍스트가 있는 bio는 PI와
 * 동시 지원이 안 되므로 WARN_ON_ONCE로 개발자에게 알리고 즉시 실패를
 * 반환한다. 이 함수는 태스크 컨텍스트에서 호출되며, gfp_mask가
 * GFP_KERNEL이면 reclaim이 발생할 수 있으므로 원자적(atomic) 컨텍스트에서는
 * 호출하면 안 된다. 실패 시 호출자는 별도 정리 없이 오류를 전파하면 된다
 * (아직 bio에 아무것도 연결되지 않았으므로).
 *
 * 호출 체인:
 *   bio_integrity_copy_user / bio_integrity_init_user / bio_integrity_clone
 *   → [bio_integrity_alloc] → kmalloc_flex → bio_integrity_init
 */
struct bio_integrity_payload *bio_integrity_alloc(struct bio *bio,
						  gfp_t gfp_mask,
						  unsigned int nr_vecs)
{
	struct bio_integrity_alloc *bia;	/* [한국어] bip + 가변 bvecs를 한 번에 담을 할당 결과 */

	/*
	 * Whitelist the operations that will not confuse checksumming.
	 */
	/* [한국어] 암호화(bio_has_crypt_ctx)와 integrity를 동시에 요청하는 것은
	 * 현재 지원하지 않는다 — 체크섬 계산이 암호문 기준인지 평문 기준인지
	 * 모호해지므로, NVMe End-to-end PI와 inline encryption은 컨트롤러
	 * 이 둘은 상호 배타적으로 취급된다. 인라인 암호화는 데이터를 변환하는데,
	 * PI는 변환 전 데이터에 대해 계산되어야 할지 후에 계산되어야 할지가
	 * 계층 간에 합의되어 있지 않기 때문이다. */
	if (WARN_ON_ONCE(bio_has_crypt_ctx(bio)))
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 지원하지 않는 조합 — 즉시 실패 반환 */

	/* [한국어] kmalloc_flex로 bip(고정 헤더) + bvecs[nr_vecs](가변 배열)를 한 번에
	 * 할당 — NVMe metadata SGL 슬롯 공간을 단일 블록으로 확보한다. */
	bia = kmalloc_flex(*bia, bvecs, nr_vecs, gfp_mask);
	if (unlikely(!bia))
		return ERR_PTR(-ENOMEM);	/* [한국어] 할당 실패 — 메모리 부족을 호출자에게 전파 */
	/* [한국어] 방금 할당한 bip을 0으로 초기화하고 bio에 연결 — bip_max_vcnt/bip_vec
	 * 설정 및 REQ_INTEGRITY 플래그 세팅까지 이 한 호출로 완료된다. */
	bio_integrity_init(bio, &bia->bip, bia->bvecs, nr_vecs);
	/* [한국어] 호출자에게는 struct bio_integrity_alloc의 존재를 감추고
	 * bip 포인터만 반환 — container_of 은닉 패턴. */
	return &bia->bip;
}
EXPORT_SYMBOL(bio_integrity_alloc);	/* [한국어] 모듈(드라이버)에서도 이 할당 함수를
					 * 직접 호출할 수 있도록 공개 심볼로 노출. */

/*
 * [한국어]
 * bio_integrity_unpin_bvec - 사용자 공간에서 pin된 페이지들의 참조 카운트를 감소
 *
 * @bv: pin 해제할 bio_vec 배열의 시작 주소.
 * @nr_vecs: bv 배열의 원소 개수.
 * @return: 없음(void).
 *
 * iov_iter_extract_pages() 등으로 사용자 페이지를 pin(get_user_pages류로
 * 참조 카운트를 올려 스와핑/이동을 막음)한 뒤, 더 이상 커널이 그 페이지를
 * 참조하지 않게 되었을 때 반드시 짝을 맞춰 unpin해야 하는 정리 헬퍼다.
 * NVMe 관점에서는 사용자 모드 프로그램(nvme-cli passthrough 등)이 자신의
 * 버퍼를 직접 제공했을 때, DMA 매핑이 끝난 뒤 혹은 bounce 복사가 끝난 뒤
 * pin된 페이지를 해제하는 역할을 한다. 이 함수는 완료 컨텍스트 또는 오류
 * 처리 경로에서 호출될 수 있으며, 페이지별로 독립적인 unpin_user_page()
 * 호출이므로 재진입에 안전하다.
 *
 * 호출 체인:
 *   bio_integrity_copy_user / bio_integrity_map_user / bio_integrity_uncopy_user
 *   → [bio_integrity_unpin_bvec] → unpin_user_page
 */
static void bio_integrity_unpin_bvec(struct bio_vec *bv, int nr_vecs)
{
	int i;	/* [한국어] bv 배열 순회 인덱스 */

	/* [한국어] 모든 integrity metadata 페이지에 대해 pin count를 하나씩
	 * 감소 — NVMe DMA 전송이 끝난 뒤 사용자 페이지 소유권을 반환하는 정리 과정. */
	for (i = 0; i < nr_vecs; i++)
		unpin_user_page(bv[i].bv_page);
}

/*
 * [한국어]
 * bio_integrity_uncopy_user - READ 완료 시 bounce 버퍼의 metadata를 사용자 원본 버퍼로 복사
 *
 * @bip: bounce 버퍼(bip_vec[0])와 원본 사용자 bvec(bip_vec[1..])을 함께 담고
 *      있는 integrity payload. bio_integrity_copy_user()가 READ 방향으로
 *      설정해 둔 레이아웃을 전제로 한다.
 * @return: 없음(void). 내부적으로 copy_to_iter() 결과를 WARN_ON_ONCE로만 검사한다
 *      (완료 경로라 실패해도 별도 오류 반환 채널이 없음).
 *
 * bio_integrity_copy_user()가 READ 요청을 위해 만들어 둔 커널 bounce 버퍼에
 * NVMe 컨트롤러가 채워 넣은 metadata를, completion 시점에 사용자가 원래
 * 요청한 버퍼(bip_vec[1..]에 보존해 둔 원본 bvec)로 복사하는 것이 이
 * 함수의 목적이다. NVMe CQ(Completion Queue)에서 상태가 성공이면, 컨트롤러가
 * 반환한 PI metadata를 사용자 공간에 전달해야 하므로 이 함수는
 * bio_integrity_map_user() → ... → nvme_complete_rq() 경로의 완료 처리
 * 단계에서 READ 방향으로만 호출된다. 복사가 끝나면 원본 사용자
 * 페이지의 pin을 해제해 참조 카운트를 정리한다.
 *
 * 호출 체인:
 *   bio_integrity_unmap_user → [bio_integrity_uncopy_user]
 *   → copy_to_iter → bio_integrity_unpin_bvec
 */
static void bio_integrity_uncopy_user(struct bio_integrity_payload *bip)
{
	/* [한국어] 원본 사용자 bvec 개수 — bip_max_vcnt는 "원본 nr_vecs + bounce 1개"로
	 * 할당되었으므로 1을 빼야 실제 원본 개수가 된다(bio_integrity_copy_user() 참고). */
	unsigned short orig_nr_vecs = bip->bip_max_vcnt - 1;
	/* [한국어] 원본 사용자 bvec 배열은 인덱스 1부터 보관되어 있음(인덱스 0은 bounce) */
	struct bio_vec *orig_bvecs = &bip->bip_vec[1];
	/* [한국어] 인덱스 0이 bounce 버퍼 — 컨트롤러가 실제로 채운 metadata가 여기 담김 */
	struct bio_vec *bounce_bvec = &bip->bip_vec[0];
	size_t bytes = bounce_bvec->bv_len;	/* [한국어] 복사할 총 metadata 바이트 수 */
	struct iov_iter orig_iter;	/* [한국어] 원본 사용자 bvec을 순회하기 위한 반복자 */
	int ret;	/* [한국어] copy_to_iter()의 반환값(복사된 바이트 수)을 담을 변수 */

	/* [한국어] 원본 사용자 bvec 배열로부터 쓰기(ITER_DEST) 방향 iterator를 구성 —
	 * 이후 copy_to_iter가 이 iterator를 목적지로 사용. */
	iov_iter_bvec(&orig_iter, ITER_DEST, orig_bvecs, orig_nr_vecs, bytes);
	/* [한국어] bounce 버퍼(컨트롤러가 채운 실제 데이터)를 원본 사용자 버퍼로 복사 —
	 * bvec_virt()로 bounce_bvec의 가상 주소를 얻어 소스로 사용. */
	ret = copy_to_iter(bvec_virt(bounce_bvec), bytes, &orig_iter);
	/* [한국어] 요청한 바이트 수만큼 정확히 복사되었는지 확인 — 실패는 버그를
	 * 의미하므로(사용자 iterator 크기는 이미 검증된 상태) WARN_ON_ONCE로만 표시. */
	WARN_ON_ONCE(ret != bytes);

	/* [한국어] 복사가 끝났으므로 원본 사용자 페이지의 pin을 해제 —
	 * NVMe READ completion cleanup의 마지막 단계. */
	bio_integrity_unpin_bvec(orig_bvecs, orig_nr_vecs);
}

/**
 * bio_integrity_unmap_user - Unmap user integrity payload
 * @bio:	bio containing bip to be unmapped
 *
 * Unmap the user mapped integrity portion of a bio.
 */
/*
 * [한국어]
 * bio_integrity_unmap_user - 사용자 공간에 매핑되었던 integrity payload를 해제
 *
 * @bio: unmap 대상 bio. bio_integrity_map_user()로 매핑되어 있어야 한다.
 * @return: 없음(void).
 *
 * bio_integrity_map_user()가 사용한 두 가지 경로(직접 매핑 vs bounce 버퍼
 * 복사) 각각에 맞는 정리 작업을 수행하는 대칭 함수다. BIP_COPY_USER
 * 플래그가 설정되어 있으면 bounce 버퍼 경로였다는 뜻이므로, READ 방향일
 * 때는 먼저 bio_integrity_uncopy_user()로 컨트롤러가 채운 데이터를 사용자
 * 버퍼에 반영한 뒤 bounce 버퍼 자체를 kfree()하고, WRITE 방향이었다면
 * 이미 사용자 데이터를 복사해 두었으므로 바로 kfree()만 한다. bounce가
 * 아니었다면 사용자 페이지를 직접 pin해서 썼던 것이므로 pin만 해제한다.
 * 이 함수는 NVMe completion(CQ) 처리 이후 사용자 공간 I/O(io_uring
 * passthrough, nvme-cli 등)를 완료 처리하는 경로에서 호출된다.
 *
 * 호출 체인:
 *   nvme_complete_rq → blk_mq_end_request → (passthrough 완료 콜백)
 *     → blk_rq_unmap_user (block/blk-map.c:1440)
 *     → [bio_integrity_unmap_user]
 *   → bio_integrity_uncopy_user / bio_integrity_unpin_bvec
 */
void bio_integrity_unmap_user(struct bio *bio)
{
	/* [한국어] unmap 대상 payload 조회 */
	struct bio_integrity_payload *bip = bio_integrity(bio);

	/* [한국어] BIP_COPY_USER 플래그: bounce 버퍼를 사용 중이었는지 여부를 판별 */
	if (bip->bip_flags & BIP_COPY_USER) {
		/* [한국어] READ 방향이면 컨트롤러가 채운 metadata를 사용자 버퍼로
		 * 복사해야 함 — WRITE는 이미 복사가 끝난 상태라 필요 없음. */
		if (bio_data_dir(bio) == READ)
			bio_integrity_uncopy_user(bip);
		/* [한국어] bounce 버퍼(커널 메모리) 자체를 해제 — NVMe metadata DMA
		 * buffer 반납. bvec_virt()로 가상 주소를 얻어 kfree. */
		kfree(bvec_virt(bip->bip_vec));
		return;	/* [한국어] bounce 경로는 여기서 정리 완료 — 아래 pin 해제 경로는 건너뜀 */
	}

	/* [한국어] bounce 없이 사용자 페이지를 직접 pin해서 사용한 경로 —
	 * DMA/복사가 모두 끝났으므로 pin된 모든 페이지의 참조 카운트를 낮춘다. */
	bio_integrity_unpin_bvec(bip->bip_vec, bip->bip_max_vcnt);
}

/**
 * bio_integrity_add_page - Attach integrity metadata
 * @bio:	bio to update
 * @page:	page containing integrity metadata
 * @len:	number of bytes of integrity metadata in page
 * @offset:	start offset within page
 *
 * Description: Attach a page containing integrity metadata to bio.
 */
/*
 * [한국어]
 * bio_integrity_add_page - integrity metadata 페이지 하나를 bio에 추가
 *
 * @bio: 세그먼트를 추가할 대상 bio.
 * @page: metadata가 담긴 물리 페이지.
 * @len: 해당 페이지 안에서 유효한 metadata 바이트 수.
 * @offset: 페이지 시작에서 metadata까지의 오프셋.
 * @return: 실제로 추가된 바이트 수(성공 시 len과 동일) 또는 0(용량/한도
 *      초과로 추가할 수 없음). bio_add_page()류와 동일한 관용적 반환 규약이다.
 *
 * bio_add_page()가 데이터 페이지에 대해 하는 일을, integrity metadata
 * 페이지에 대해 수행하는 함수다. 이전에 추가된 마지막 세그먼트와 물리적으로
 * 인접하면 별도 슬롯을 늘리지 않고 병합(merge)해 NVMe metadata SGL/PRP의
 * 세그먼트 수를 최소화하고, 그렇지 않으면 새 슬롯에 추가한다. 컨트롤러의
 * max_integrity_segments 한도, zone device(pmem/ZNS) pgmap 일치 여부, SG
 * gap 제약(virt_boundary_mask — NVMe PRP 모드에서는 4KiB 페이지 정렬)을
 * 모두 검사해 하나라도 위반하면
 * 추가를 거부(0 반환)한다. 이 함수는 bio를 조립 중인 태스크 컨텍스트에서
 * 세그먼트 개수만큼 반복 호출되는 것이 일반적이다.
 *
 * 호출 체인:
 *   bio_integrity_init_user (block/bio-integrity.c:1092) 및
 *   bio_integrity_copy_user → [bio_integrity_add_page]
 *   → bvec_try_merge_hw_page / bvec_set_page
 */
int bio_integrity_add_page(struct bio *bio, struct page *page,
			   unsigned int len, unsigned int offset)
{
	/* [한국어] bio가 속한 request_queue — NVMe controller의 queue limits(세그먼트
	 * 한도, DMA 정렬 등)를 담고 있다. */
	struct request_queue *q = bdev_get_queue(bio->bi_bdev);
	/* [한국어] 세그먼트를 추가할 대상 integrity payload */
	struct bio_integrity_payload *bip = bio_integrity(bio);

	/* [한국어] 이미 최소 1개 이상의 세그먼트가 있어야 "이전 세그먼트와 병합"
	 * 시도가 의미가 있다 — 첫 세그먼트는 무조건 새로 추가. */
	if (bip->bip_vcnt > 0) {
		/* [한국어] 가장 최근에 추가된(마지막) bvec — 병합 후보 */
		struct bio_vec *bv = &bip->bip_vec[bip->bip_vcnt - 1];

		/*
		 * If the queue doesn't support SG gaps and adding this
		 * segment would create a gap, disallow it.
		 */
		/* [한국어] zone device(pmem/zoned) 페이지들이 서로 다른 pgmap(페이지
		 * 소유 매핑 도메인)에 속하면 물리적으로 인접해도 병합할 수 없다 —
		 * pmem처럼 ZONE_DEVICE 페이지를 metadata 버퍼로 쓰는 경우에 걸린다. */
		if (!zone_device_pages_have_same_pgmap(bv->bv_page, page))
			return 0;	/* [한국어] pgmap 불일치 — 병합 불가, 이 페이지는 추가 거부 */

		/*
		 * See if we can merge into a physically contiguous mapping.
		 */
		/* [한국어] 같은 물리 페이지 계열에서 인접한 metadata면 기존 세그먼트와
		 * 병합 시도 — 성공하면 NVMe SGL/PRP 세그먼트 수가 늘지 않아 DMA
		 * 효율이 높아진다. */
		if (bvec_try_merge_hw_page(q, bv, page, len, offset)) {
			/* [한국어] 병합 성공: 새 슬롯 없이 길이만 누적 */
			bip->bip_iter.bi_size += len;
			return len;	/* [한국어] 요청한 len 전체가 반영되었음을 알림 */
		}

		/*
		 * If the queue doesn't support SG gaps and adding this
		 * segment would create a gap, disallow it.
		 */
		/* [한국어] 병합이 안 되면 새 슬롯이 필요한데, 컨트롤러의
		 * max_integrity_segments(또는 이 bip의 bip_max_vcnt) 한도를 이미
		 * 채웠다면 더 이상 추가할 수 없다. */
		if (bip->bip_vcnt >=
		    min(bip->bip_max_vcnt, queue_max_integrity_segments(q)))
			return 0;	/* [한국어] 세그먼트 한도 초과 — 추가 거부 */

		/* [한국어] 큐가 SG(scatter-gather) gap을 지원하지 않는데 이 페이지를
		 * 추가하면 이전 세그먼트와 사이에 gap이 생기는 경우 — NVMe PRP
		 * list는 흔히 페이지 정렬/연속성을 요구하므로 이런 gap을 허용하지
		 * 않는다. NVMe PRP 모드가 정확히 이 경우로, PRP2 이후 엔트리는 페이지
		 * 오프셋 0에서 시작해야 하므로 gap이 있으면 표현할 수 없다. */
		if (bvec_gap_to_prev(&q->limits, bv, offset))
			return 0;	/* [한국어] gap 발생 — 추가 거부 */
	}

	/* [한국어] 병합 불가 또는 첫 세그먼트: 새로운 슬롯에 페이지/길이/오프셋을
	 * 기록 — NVMe metadata SGL/PRP entry가 하나 늘어남을 의미. */
	bvec_set_page(&bip->bip_vec[bip->bip_vcnt], page, len, offset);
	bip->bip_vcnt++;	/* [한국어] 채워진 세그먼트 개수 증가 */
	bip->bip_iter.bi_size += len;	/* [한국어] 전체 metadata 바이트 수 누적 */

	return len;	/* [한국어] 요청한 길이 전체가 성공적으로 추가되었음을 알림 */
}
EXPORT_SYMBOL(bio_integrity_add_page);	/* [한국어] 드라이버/파일시스템에서 직접 호출 가능하도록 공개 */

/*
 * [한국어]
 * bio_integrity_copy_user - 사용자 공간 metadata를 커널 bounce 버퍼로 복사(또는 준비)
 *
 * @bio: bounce 버퍼를 붙일 대상 bio. bio_data_dir(bio)로 READ/WRITE를 구분한다.
 * @bvec: 원본 사용자 metadata를 가리키는 bio_vec 배열(호출자가 이미 pin해 둠).
 * @nr_vecs: bvec 배열의 원소 개수.
 * @len: 전체 metadata 바이트 수.
 * @return: 성공 시 0, 실패 시 음수 errno(-ENOMEM, -EFAULT 등). 실패하면
 *      내부에서 이미 필요한 정리(bio_integrity_free/kfree)를 마친 뒤 반환한다.
 *
 * 사용자 버퍼가 DMA alignment나 padding 제약을 만족하지 못할 때, 커널이
 * 대신 소유하는 bounce 버퍼를 하나 마련해 그 버퍼를 실제 NVMe metadata
 * pointer로 사용하기 위한 함수다. WRITE 방향이면 사용자가 준비한 metadata를
 * 지금 즉시 bounce 버퍼로 복사해 두고(컨트롤러는 이 사본만 본다), 사용자
 * 페이지는 더 이상 필요 없으므로 바로 pin을 해제한다. READ 방향이면
 * bounce 버퍼를 0으로 비워 컨트롤러가 채울 공간만 마련하고, 완료 시점에
 * 사용자 버퍼로 되돌려 복사할 수 있도록 원본 bvec을 bip_vec[1..]에 보존해
 * 둔다(bio_integrity_uncopy_user()가 나중에 사용). 이 함수는 태스크
 * 컨텍스트에서 GFP_KERNEL로 커널 버퍼를 할당하므로 reclaim이 발생할 수
 * 있다. 실패 경로는 free_bip/free_buf 레이블로 순차적으로 정리한다.
 *
 * 호출 체인:
 *   bio_integrity_map_user → [bio_integrity_copy_user]
 *   → bio_integrity_alloc → bio_integrity_add_page
 */
static int bio_integrity_copy_user(struct bio *bio, struct bio_vec *bvec,
				   int nr_vecs, unsigned int len)
{
	/* [한국어] WRITE 방향이면 "사용자 데이터를 bounce로 밀어넣기", READ 방향이면
	 * "컨트롤러 결과를 나중에 사용자에게 복사"로 처리 경로가 갈린다. */
	bool write = op_is_write(bio_op(bio));
	struct bio_integrity_payload *bip;	/* [한국어] bio_integrity_alloc()이 반환할 payload */
	struct iov_iter iter;	/* [한국어] 사용자 bvec ↔ 커널 버퍼 사이 복사에 쓰일 반복자 */
	void *buf;	/* [한국어] 새로 할당할 커널 bounce 버퍼의 가상 주소 */
	int ret;	/* [한국어] 각 단계의 성공/실패(errno) 값을 담아 최종 반환 */

	/* [한국어] 커널 bounce 버퍼 할당 — NVMe DMA가 요구하는 alignment를
	 * 사용자 버퍼가 만족하지 못할 때 이 버퍼가 대신 DMA 대상이 된다. */
	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;	/* [한국어] 할당 실패 — 더 진행할 수 없으므로 즉시 오류 반환 */

	if (write) {
		/* [한국어] WRITE: 사용자 metadata → 커널 bounce 버퍼로 복사 —
		 * 이후 NVMe 컨트롤러는 DMA로 이 bounce 버퍼만 읽는다. */
		iov_iter_bvec(&iter, ITER_SOURCE, bvec, nr_vecs, len);
		if (!copy_from_iter_full(buf, len, &iter)) {
			/* [한국어] 사용자 주소 공간 접근 실패(페이지 폴트 등) — 부분
			 * 복사는 무의미하므로 오류로 처리. */
			ret = -EFAULT;
			goto free_buf;	/* [한국어] 지금까지 할당한 buf만 정리하고 반환 */
		}

		/* [한국어] WRITE는 bounce 버퍼 1개 세그먼트만 필요 — NVMe metadata
		 * pointer 슬롯 1개로 충분하다. */
		bip = bio_integrity_alloc(bio, GFP_KERNEL, 1);
	} else {
		/* [한국어] READ: 아직 컨트롤러가 아무것도 채우지 않았으므로 0으로
		 * 초기화 — 미초기화 커널 메모리가 사용자에게 노출되는 것을 방지. */
		memset(buf, 0, len);

		/*
		 * We need to preserve the original bvec and the number of vecs
		 * in it for completion handling
		 */
		/* [한국어] READ 완료 시 bio_integrity_uncopy_user()가 원본 사용자
		 * bvec으로 복사해야 하므로, bounce 1개 + 원본 nr_vecs개를 합쳐
		 * 총 nr_vecs+1 슬롯으로 할당해 둔다. */
		bip = bio_integrity_alloc(bio, GFP_KERNEL, nr_vecs + 1);
	}

	if (IS_ERR(bip)) {
		/* [한국어] payload 할당 실패 — errno를 추출해 이후 정리 경로로 이동 */
		ret = PTR_ERR(bip);
		goto free_buf;	/* [한국어] bip이 없으므로 bio_integrity_free는 호출할 필요 없음 */
	}

	if (write)
		/* [한국어] WRITE는 사용자 데이터를 이미 bounce로 복사했으므로 사용자
		 * 페이지를 더 이상 참조하지 않음 — 즉시 pin 해제. */
		bio_integrity_unpin_bvec(bvec, nr_vecs);
	else
		/* [한국어] READ는 completion 시 사용자 버퍼로 복사할 수 있도록 원본
		 * bvec을 bip_vec[1..]에 그대로 보존(memcpy로 배열 복사, pin은 유지). */
		memcpy(&bip->bip_vec[1], bvec, nr_vecs * sizeof(*bvec));

	/* [한국어] bounce 버퍼(가상 주소 buf)를 물리 페이지로 변환해 bip에 세그먼트로
	 * 추가 — 이 세그먼트가 실제 NVMe metadata buffer로 매핑된다. */
	ret = bio_integrity_add_page(bio, virt_to_page(buf), len,
				     offset_in_page(buf));
	if (ret != len) {
		/* [한국어] 세그먼트 추가가 요청한 길이만큼 반영되지 않음(한도 초과 등) —
		 * 논리적으로는 add_page 실패이지만 호출자에게는 메모리 부족으로 보고. */
		ret = -ENOMEM;
		goto free_bip;	/* [한국어] 이미 만든 bip까지 포함해 정리 필요 */
	}

	/* [한국어] BIP_COPY_USER: 이 bip이 bounce 버퍼를 사용 중임을 표시 —
	 * bio_integrity_unmap_user()가 이 플래그로 uncopy 필요 여부를 판단. */
	bip->bip_flags |= BIP_COPY_USER;
	/* [한국어] READ 방향에서 add_page가 늘려 놓은 bip_vcnt를 실제 유효
	 * 세그먼트 수(원본 bvec 개수)로 재설정 — bounce 세그먼트 1개만 유효하게 취급. */
	bip->bip_vcnt = nr_vecs;
	return 0;	/* [한국어] 정상 완료 */
free_bip:
	/* [한국어] bip까지 할당된 상태에서 실패 — bio에서 bip을 완전히 떼어내고 해제 */
	bio_integrity_free(bio);
free_buf:
	/* [한국어] 최소한 buf는 항상 할당되어 있으므로 반드시 해제 */
	kfree(buf);
	return ret;	/* [한국어] 누적된 errno를 호출자에게 전달 */
}

/*
 * [한국어]
 * bio_integrity_init_user - 사용자 metadata를 복사 없이 bip에 바로 매핑
 *
 * @bio: metadata를 붙일 대상 bio.
 * @bvec: 이미 DMA 가능/정렬 조건을 만족하는 사용자 metadata bio_vec 배열.
 * @nr_vecs: bvec 배열의 원소 개수.
 * @len: 전체 metadata 바이트 수.
 * @return: 성공 시 0, 실패 시 PTR_ERR(bip)에서 추출한 음수 errno.
 *
 * bio_integrity_copy_user()의 "bounce 버퍼 복사" 경로와 대비되는, 복사 없는
 * 최적 경로다. 사용자 공간 metadata가 이미 DMA alignment 요구사항을
 * 만족한다고 판단되었을 때(bio_integrity_map_user()의 copy 여부 판정 결과)
 * 사용되며, 사용자가 직접 pin한 페이지를 그대로 bip_vec에 등록해 복사
 * 오버헤드를 없앤다. NVMe 관점에서는 nvme-cli passthrough 등에서 사용자가
 * 직접 할당한 metadata 버퍼가 곧바로 NVMe metadata SGL/PRP로 사용되는
 * 경로에 해당한다. 이 함수는 태스크 컨텍스트에서 호출되며 별도
 * 정리 로직이 없다(bio_integrity_alloc 실패 시 그대로 오류 전파).
 *
 * 호출 체인:
 *   bio_integrity_map_user → [bio_integrity_init_user] → bio_integrity_alloc
 */
static int bio_integrity_init_user(struct bio *bio, struct bio_vec *bvec,
				   int nr_vecs, unsigned int len)
{
	struct bio_integrity_payload *bip;	/* [한국어] bio_integrity_alloc()의 반환값을 담을 변수 */

	/* [한국어] nr_vecs개 슬롯을 가진 payload를 새로 할당 — 이 시점에는 아직
	 * bip_vec 내용이 채워지지 않은 빈 배열 상태. */
	bip = bio_integrity_alloc(bio, GFP_KERNEL, nr_vecs);
	if (IS_ERR(bip))
		return PTR_ERR(bip);	/* [한국어] 할당 실패 — errno 그대로 전파 */

	/* [한국어] 사용자 bvec 배열을 통째로 복사(포인터 배열 복사이지 데이터 복사가
	 * 아님) — 페이지 자체는 사용자가 pin한 것을 그대로 재사용, NVMe DMA가
	 * 이 페이지에 직접 접근한다고 가정한다. */
	memcpy(bip->bip_vec, bvec, nr_vecs * sizeof(*bvec));
	bip->bip_iter.bi_size = len;	/* [한국어] 전체 metadata 바이트 수 기록 */
	bip->bip_vcnt = nr_vecs;	/* [한국어] 채워진 세그먼트 수 = 원본 개수 그대로 */
	return 0;	/* [한국어] 정상 완료 */
}

/*
 * [한국어]
 * bvec_from_pages - 추출된 페이지 배열을 연속 물리 영역 단위로 병합해 bvec 배열로 변환
 *
 * @bvec: 결과를 채울 bio_vec 배열(호출자가 nr_vecs 크기 이상으로 미리 할당).
 * @pages: iov_iter_extract_pages()로 얻은, pin된 페이지 포인터 배열.
 * @nr_vecs: pages 배열의 원소 개수(입력 페이지 수).
 * @bytes: 전체 metadata 바이트 수(페이지 경계에 걸친 마지막 페이지의 유효
 *      길이를 계산하기 위해 필요).
 * @offset: 첫 페이지 안에서 metadata가 시작하는 오프셋.
 * @is_p2p: 출력 파라미터. 병합 과정에서 발견한 페이지 중 하나라도 PCI
 *      peer-to-peer DMA 페이지면 true로 설정된다.
 * @return: 실제로 채워진 bvec 개수(병합으로 인해 nr_vecs보다 작거나 같음).
 *
 * iov_iter_extract_pages()가 반환하는 개별 페이지 배열은 서로 인접한
 * 물리 페이지라도 별도 원소로 나열되어 있으므로, 이를 그대로 NVMe SGL/PRP에
 * 넣으면 불필요하게 세그먼트 수가 늘어난다. 이 함수는 같은 folio에 속하고
 * 물리적으로 연속된(pages[j] == pages[j-1] + 1) 페이지들을 하나의 bvec으로
 * 합쳐 세그먼트 수를 줄인다. 병합되어 대표 bvec에 흡수된 페이지는
 * unpin_user_page()로 개별 pin을 즉시 낮추는데(대표 페이지의 pin은 유지),
 * 이는 folio 참조 카운트 체계에서 이러한 정리가 안전하다는 가정 하에
 * 이루어진다. 순수 계산/변환 함수로 락이 없으며 재진입에 안전하다.
 *
 * 호출 체인:
 *   bio_integrity_map_user → [bvec_from_pages] → page_folio / unpin_user_page /
 *   is_pci_p2pdma_page / bvec_set_page
 */
static unsigned int bvec_from_pages(struct bio_vec *bvec, struct page **pages,
				    int nr_vecs, ssize_t bytes, ssize_t offset,
				    bool *is_p2p)
{
	unsigned int nr_bvecs = 0;	/* [한국어] 지금까지 채운 결과 bvec 개수(반환값이 됨) */
	int i, j;	/* [한국어] i: 현재 병합 그룹의 시작 인덱스, j: 그룹 내부 탐색 인덱스 */

	/* [한국어] 페이지 배열을 순회하며 연속된 물리 페이지 그룹을 하나씩 찾아 병합 —
	 * i가 다음 그룹 시작으로 j값을 이어받아 건너뛴다(for (i = 0; i < nr_vecs; i = j)). */
	for (i = 0; i < nr_vecs; i = j) {
		/* [한국어] 현재 페이지에서 실제 유효한 바이트 수 — 페이지 크기에서
		 * 시작 오프셋을 뺀 값과 남은 전체 바이트 수 중 작은 쪽. */
		size_t size = min_t(size_t, bytes, PAGE_SIZE - offset);
		/* [한국어] 현재 페이지가 속한 folio(연속된 페이지 묶음 단위) — 같은
		 * folio 여부로 물리적 연속성을 빠르게 판별하는 데 사용. */
		struct folio *folio = page_folio(pages[i]);

		bytes -= size;	/* [한국어] 첫 페이지 분량을 차감해 남은 전체 바이트 수 갱신 */
		for (j = i + 1; j < nr_vecs; j++) {
			/* [한국어] 다음 페이지에서 사용할 바이트 수 — 온전한 한 페이지
			 * 또는 남은 전체 바이트 수 중 작은 쪽. */
			size_t next = min_t(size_t, PAGE_SIZE, bytes);

			/*
			 * As long as the pages are folio contiguous and
			 * physically contiguous merge them into the current
			 * bvec.
			 */
			/* [한국어] 같은 folio에 속하지 않거나 바로 이전 페이지의 다음
			 * 물리 페이지가 아니면 더 이상 병합할 수 없으므로 그룹을
			 * 종료 — NVMe SGL/PRP에서 불필요한 세그먼트 분할을 막기 위한
			 * 병합 조건. */
			if (page_folio(pages[j]) != folio ||
			    pages[j] != pages[j - 1] + 1)
				break;	/* [한국어] 연속성이 끊김 — 현재 그룹을 여기서 마감 */
			/* [한국어] 병합되어 대표 bvec 하나로 흡수될 페이지이므로 개별
			 * pin을 미리 낮춘다(대표 페이지 자체의 pin은 그대로 유지됨). */
			unpin_user_page(pages[j]);
			size += next;	/* [한국어] 병합된 세그먼트의 누적 길이 증가 */
			bytes -= next;	/* [한국어] 남은 전체 바이트 수에서 이번 페이지 분량 차감 */
		}

		/*
		 * If we are working on a p2pdma mapping, we need to note this
		 * in the io so that DMA to it can be disallowed.
		 */
		/* [한국어] peer-to-peer DMA 페이지(다른 PCIe 장치의 BAR/CMB 메모리)가
		 * 섞여 있으면 이후 merge/재배치 정책이 달라져야 하므로 플래그로
		 * 상위 호출자에게 알린다. P2PDMA 메모리는 일반 메모리와 DMA 매핑 방식이
		 * 달라(버스 주소 vs IOVA) 한 요청에 섞을 수 없기 때문이다. */
		if (is_pci_p2pdma_page(pages[i]))
			*is_p2p = true;

		/* [한국어] 이번에 병합된 연속 물리 영역 전체를 하나의 bvec 원소로
		 * 기록 — NVMe SGL 세그먼트 1개에 대응. */
		bvec_set_page(&bvec[nr_bvecs], pages[i], size, offset);
		offset = 0;	/* [한국어] 두 번째 그룹부터는 페이지 시작부터이므로 오프셋 0 */
		nr_bvecs++;	/* [한국어] 채워진 결과 bvec 개수 증가 */
	}

	return nr_bvecs;	/* [한국어] 병합 후 실제로 필요한 세그먼트 개수를 호출자에게 알림 */
}

/*
 * [한국어]
 * bio_integrity_map_user - 사용자 공간 iov_iter의 metadata를 bio의 bip에 매핑
 *
 * @bio: metadata를 붙일 대상 bio. 아직 bip이 없어야 한다(중복 매핑 방지).
 * @iter: 사용자 공간 metadata 버퍼를 기술하는 반복자.
 * @return: 성공 시 0, 실패 시 음수 errno(-EINVAL: 이미 매핑됨, -E2BIG: 크기
 *      초과, 그 외 iov_iter_extract_pages()/copy/init_user 실패 코드 전파).
 *
 * 사용자가 직접 전달한 integrity metadata 영역을 bio에 연결해, block layer가
 * 이를 NVMe 명령과 함께 전송할 수 있도록 준비하는 상위 진입점이다. 먼저
 * 크기 제한(max_hw_sectors, BIO_MAX_VECS)을 검사하고, 필요한 페이지 수만큼
 * 스택 배열(UIO_FASTIOV 이내) 또는 동적 배열을 준비한 뒤,
 * iov_iter_extract_pages()로 사용자 페이지를 pin한다. DMA alignment/padding을
 * 만족하지 못하거나(copy=true), 병합 후에도 max_integrity_segments를
 * 초과하면 bio_integrity_copy_user()로 bounce 복사 경로를, 그렇지 않으면
 * bio_integrity_init_user()로 무복사(zero-copy) 경로를 선택한다. P2P DMA
 * 페이지가 섞여 있으면 REQ_NOMERGE를 설정해 이후 병합기가 이 bio를
 * 건드리지 못하게 한다. 이 함수는 태스크 컨텍스트에서 사용자 주소 공간에
 * 접근하므로 페이지 폴트가 발생할 수 있다.
 *
 * 호출 체인:
 *   blk_rq_integrity_map_user (block/blk-integrity.c:395, nvme-cli 등의
 *     passthrough ioctl 경로) → [bio_integrity_map_user]
 *   → iov_iter_extract_pages → bvec_from_pages
 *   → bio_integrity_copy_user / bio_integrity_init_user
 */
int bio_integrity_map_user(struct bio *bio, struct iov_iter *iter)
{
	/* [한국어] bio의 request_queue — NVMe controller queue limits(세그먼트 한도,
	 * DMA 정렬 등) 포함. */
	struct request_queue *q = bdev_get_queue(bio->bi_bdev);
	/* [한국어] 페이지 포인터를 담을 스택 배열과, 필요 시 이를 대체할 포인터 */
	struct page *stack_pages[UIO_FASTIOV], **pages = stack_pages;
	/* [한국어] 결과 bvec을 담을 스택 배열과, 필요 시 이를 대체할 포인터 */
	struct bio_vec stack_vec[UIO_FASTIOV], *bvec = stack_vec;
	/* [한국어] iov_iter_extract_pages()에 전달할 추출 옵션 플래그(P2P 허용 여부 등) */
	iov_iter_extraction_t extraction_flags = 0;
	/* [한국어] offset: 첫 페이지 내 시작 오프셋(추출 결과로 채워짐),
	 * bytes: 처리할 전체 metadata 바이트 수(iter->count로 초기화) */
	size_t offset, bytes = iter->count;
	/* [한국어] copy: bounce 복사 경로 사용 여부, is_p2p: P2P DMA 페이지 포함 여부 */
	bool copy, is_p2p = false;
	unsigned int nr_bvecs;	/* [한국어] bvec_from_pages()가 병합 후 반환할 실제 세그먼트 수 */
	int ret, nr_vecs;	/* [한국어] ret: 최종 반환값(errno), nr_vecs: 필요한 최대 페이지 수 */

	/* [한국어] 이미 integrity payload가 붙어 있으면 이중 매핑을 막기 위해 오류 반환. */
	if (bio_integrity(bio))
		return -EINVAL;
	/* [한국어] NVMe 컨트롤러의 max_hw_sectors 제한을 초과하는 metadata 요청은
	 * 애초에 하나의 bio/request로 처리할 수 없으므로 거부. */
	if (bytes >> SECTOR_SHIFT > queue_max_hw_sectors(q))
		return -E2BIG;

	/* [한국어] 사용자 iterator를 표현하는 데 필요한 최대 페이지 수를 미리 센다 —
	 * BIO_MAX_VECS+1까지만 세어 상한 초과를 빠르게 감지. */
	nr_vecs = iov_iter_npages(iter, BIO_MAX_VECS + 1);
	if (nr_vecs > BIO_MAX_VECS)
		return -E2BIG;	/* [한국어] NVMe metadata SGL로 표현하기엔 세그먼트가 너무 많음 */
	if (nr_vecs > UIO_FASTIOV) {
		/* [한국어] 스택 배열(빠른 경로) 용량을 초과 — 힙에 동적으로 bvec 배열 할당 */
		bvec = kzalloc_objs(*bvec, nr_vecs);
		if (!bvec)
			return -ENOMEM;	/* [한국어] 동적 배열 할당 실패 */
		pages = NULL;	/* [한국어] pages는 뒤이어 iov_iter_extract_pages()가 별도로 할당하게 함 */
	}

	/*
	 * We're taking a reference to each of the pages further down, so
	 * we need to make sure the mapping actually fulfils the DMA
	 * requirements of the queue.
	 */
	/* [한국어] DMA alignment/padding 요구사항을 사용자 iterator가 만족하지
	 * 못하면(비트마스크 AND 결과가 0이 아니면) bounce 버퍼로 복사해야 한다 —
	 * NVMe PRP 모드는 4KiB 페이지 정렬을 요구하므로 어긋난 사용자 버퍼는
	 * 그대로 매핑할 수 없다. */
	copy = iov_iter_alignment(iter) &
			blk_lim_dma_alignment_and_pad(&q->limits);

	/* [한국어] 큐가 PCI peer-to-peer DMA를 지원하면 추출 시 P2P 페이지도
	 * 허용하도록 플래그 추가. 이 플래그가 없으면 iov_iter가 장치 메모리
	 * 페이지를 거부한다. */
	if (blk_queue_pci_p2pdma(q))
		extraction_flags |= ITER_ALLOW_P2PDMA;

	/* [한국어] 사용자 iterator에서 실제로 페이지들을 추출(pin)하고, 시작
	 * 오프셋을 offset에 채운다 — 이 시점부터 pages[]의 각 페이지는 참조
	 * 카운트가 올라간 상태이므로 반드시 짝을 맞춰 unpin해야 한다. */
	ret = iov_iter_extract_pages(iter, &pages, bytes, nr_vecs,
					extraction_flags, &offset);
	if (unlikely(ret < 0))
		goto free_bvec;	/* [한국어] 추출 실패 — 아직 pin된 페이지가 없으므로 bvec만 정리 */

	/* [한국어] 추출된 개별 페이지들을 연속 영역 단위로 병합해 실제 필요한
	 * bvec 개수(nr_bvecs)를 얻는다. */
	nr_bvecs = bvec_from_pages(bvec, pages, nr_vecs, bytes, offset,
				   &is_p2p);
	if (pages != stack_pages)
		kvfree(pages);	/* [한국어] 동적으로 할당했던 pages 배열은 이 시점 이후 불필요 — 즉시 해제 */
	/* [한국어] 병합 후에도 컨트롤러의 max_integrity_segments를 초과하면 무복사
	 * 경로를 쓸 수 없으므로 bounce 복사로 강제 전환. */
	if (nr_bvecs > queue_max_integrity_segments(q))
		copy = true;
	/* [한국어] P2P DMA 페이지가 섞여 있으면 이후 요청 병합기가 이 bio를 다른
	 * bio와 합치지 못하도록 REQ_NOMERGE를 설정 — 정확한 DMA 라우팅 보장. */
	if (is_p2p)
		bio->bi_opf |= REQ_NOMERGE;

	/* [한국어] 앞서 결정된 copy 여부에 따라 bounce 복사 경로 또는 무복사
	 * 직접 매핑 경로 중 하나를 선택해 실제 bip 구성을 위임한다. */
	if (copy)
		ret = bio_integrity_copy_user(bio, bvec, nr_bvecs, bytes);
	else
		ret = bio_integrity_init_user(bio, bvec, nr_bvecs, bytes);
	if (ret)
		goto release_pages;	/* [한국어] 매핑 실패 — pin된 페이지를 되돌려야 함 */
	if (bvec != stack_vec)
		kfree(bvec);	/* [한국어] 동적 bvec 배열은 이미 bip 안으로 내용이 복사되었으므로 해제 */

	return 0;	/* [한국어] 정상 완료 */

release_pages:
	/* [한국어] bip 구성 실패 시, 이미 pin된 모든 페이지의 참조 카운트를 되돌림 */
	bio_integrity_unpin_bvec(bvec, nr_bvecs);
free_bvec:
	/* [한국어] pin 여부와 무관하게, 동적으로 할당한 bvec 배열은 항상 정리 */
	if (bvec != stack_vec)
		kfree(bvec);
	return ret;	/* [한국어] 누적된 errno를 호출자에게 전달 */
}

/*
 * [한국어]
 * bio_uio_meta_to_bip - uio_meta의 플래그/app_tag를 bip 플래그로 변환·복사
 *
 * @bio: 대상 bio(이미 bio_integrity_map_user() 등으로 bip이 붙어 있어야 함).
 * @meta: 사용자 공간에서 전달된 PI 옵션(플래그, seed, app_tag, iterator)을
 *      담은 임시 구조체.
 * @return: 없음(void).
 *
 * uio_meta는 io_uring 등 사용자 공간 인터페이스가 커널에 PI 검증 옵션을
 * 전달하는 통로이며, 그 IO_INTEGRITY_CHK_* 플래그들은 NVMe 커맨드의
 * PRCHK(Protection Information Check) 비트들과 의미상 1:1로 대응한다
 * (Guard/AppTag/RefTag 각각 독립적으로 켜고 끌 수 있음). 이 함수는 그
 * 대응 관계를 실제 bip->bip_flags 비트로 반영하고, app_tag 값도 그대로
 * 복사하는 단순 변환 함수다. 부수효과는 bip 필드 대입뿐이며 락이 필요
 * 없다.
 *
 * 호출 체인:
 *   bio_integrity_map_iter → [bio_uio_meta_to_bip] → (없음, 필드 대입만 수행)
 */
static void bio_uio_meta_to_bip(struct bio *bio, struct uio_meta *meta)
{
	/* [한국어] 플래그를 채워 넣을 대상 payload — 호출 시점에 이미 존재해야 함 */
	struct bio_integrity_payload *bip = bio_integrity(bio);

	/* [한국어] 사용자가 Guard(체크섬) 검증을 요청했는지 확인 */
	if (meta->flags & IO_INTEGRITY_CHK_GUARD)
		/* [한국어] NVMe PRCHK[Guard]에 대응하는 BIP_CHECK_GUARD 비트 설정 */
		bip->bip_flags |= BIP_CHECK_GUARD;
	/* [한국어] 사용자가 Application Tag 검증을 요청했는지 확인 */
	if (meta->flags & IO_INTEGRITY_CHK_APPTAG)
		/* [한국어] NVMe PRCHK[AppTag]에 대응하는 BIP_CHECK_APPTAG 비트 설정 */
		bip->bip_flags |= BIP_CHECK_APPTAG;
	/* [한국어] 사용자가 Reference Tag 검증을 요청했는지 확인 */
	if (meta->flags & IO_INTEGRITY_CHK_REFTAG)
		/* [한국어] NVMe PRCHK[RefTag]에 대응하는 BIP_CHECK_REFTAG 비트 설정 */
		bip->bip_flags |= BIP_CHECK_REFTAG;

	/* [한국어] Application Tag 값 자체를 그대로 복사 — NVMe 커맨드의 App Tag
	 * 필드 구성에 사용될 값. nvme_setup_rw()가 BIP_CHECK_APPTAG와 함께
	 * SQE의 apptag 필드로 옮긴다. */
	bip->app_tag = meta->app_tag;
}

/*
 * [한국어]
 * bio_integrity_map_iter - uio_meta 기반으로 현재 bio 분량만큼만 metadata를 매핑
 *
 * @bio: metadata를 매핑할 대상 bio.
 * @meta: 전체 I/O에 대한 PI 옵션과 iterator를 담은 구조체. 하나의 큰 사용자
 *      요청이 여러 bio로 분할될 때, 이 구조체의 iterator/seed가 각 bio 호출
 *      사이에서 누적 전진한다.
 * @return: 성공 시 0, 실패 시 음수 errno(-EINVAL: PI 미지원/크기 불일치/플래그
 *      오류, 그 외 bio_integrity_map_user() 실패 코드 전파).
 *
 * bio_integrity_map_user()가 iov_iter 전체를 통째로 매핑하는 것과 달리, 이
 * 함수는 meta->iter라는 "더 큰 전체 iterator"에서 현재 bio의 데이터 섹터
 * 수에 해당하는 만큼만 잘라 매핑하는 래퍼다. 하나의 사용자 I/O 요청이
 * MDTS(Maximum Data Transfer Size)나 세그먼트 한도로 인해 여러 bio로
 * split될 때, 각 bio는 자신의 데이터 길이에 맞는 RefTag seed와 metadata
 * 길이만 가져야 하므로, meta->seed를 매 호출 후 처리한 interval 수만큼
 * 전진시켜 다음 bio 호출에 대비한다. NVMe pass-through/io_uring PI
 * 인터페이스에서 반복 호출되는 것을 전제로 설계되었다.
 *
 * 호출 체인:
 *   blk_rq_integrity_map_user (nvme-cli/io_uring passthrough)
 *     → [bio_integrity_map_iter]
 *   → bio_integrity_map_user → bio_uio_meta_to_bip
 */
int bio_integrity_map_iter(struct bio *bio, struct uio_meta *meta)
{
	/* [한국어] 디스크의 PI 프로파일 — NULL이면 애초에 PI를 지원하지 않는 디스크 */
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);
	unsigned int integrity_bytes;	/* [한국어] 이번 bio 분량에 해당하는 metadata 바이트 수 */
	int ret;	/* [한국어] bio_integrity_map_user() 호출 결과를 저장할 변수 */
	struct iov_iter it;	/* [한국어] meta->iter를 복사해 이번 호출 범위로 길이를 제한할 로컬 iterator */

	/* [한국어] 디스크가 integrity를 지원하지 않으면 NVMe PI 경로 자체가
	 * 성립하지 않으므로 즉시 오류. */
	if (!bi)
		return -EINVAL;
	/*
	 * original meta iterator can be bigger.
	 * process integrity info corresponding to current data buffer only.
	 */
	/* [한국어] meta->iter(원본)는 전체 I/O 요청 분량을 아우르는 더 큰
	 * iterator일 수 있으므로, 복사본(it)만 이번 bio 분량으로 잘라 쓴다 —
	 * 원본은 다음 bio 호출을 위해 그대로 보존해야 한다. */
	it = meta->iter;
	/* [한국어] 현재 bio의 데이터 섹터 수에 대응하는 metadata 바이트 수 계산 */
	integrity_bytes = bio_integrity_bytes(bi, bio_sectors(bio));
	if (it.count < integrity_bytes)
		return -EINVAL;	/* [한국어] 남은 metadata가 이번 bio 분량보다 적음 — 요청 불일치 오류 */

	/* should fit into two bytes */
	/* [한국어] IO_INTEGRITY_VALID_FLAGS가 16비트(u16 meta->flags)에 담기는지
	 * 컴파일 타임에 검증 — 런타임 분기 없이 빌드 시점에 assert. */
	BUILD_BUG_ON(IO_INTEGRITY_VALID_FLAGS >= (1 << 16));

	/* [한국어] 사용자가 정의되지 않은(알 수 없는) 플래그 비트를 설정했는지 검사 —
	 * 향후 커널이 모르는 플래그를 조용히 무시하지 않고 명시적으로 거부. */
	if (meta->flags && (meta->flags & ~IO_INTEGRITY_VALID_FLAGS))
		return -EINVAL;

	/* [한국어] 로컬 iterator의 길이를 이번 bio 분량으로 제한 — NVMe CID
	 * (Command ID) 단위로 분할된 각 bio가 정확한 RefTag 구간만 다루게 한다. */
	it.count = integrity_bytes;
	/* [한국어] 제한된 iterator를 실제 매핑 함수에 전달해 페이지 pin/bounce 등을 수행 */
	ret = bio_integrity_map_user(bio, &it);
	if (!ret) {
		/* [한국어] 매핑 성공 시에만 플래그/app_tag를 bip에 반영 */
		bio_uio_meta_to_bip(bio, meta);
		/* [한국어] seed를 현재 bio의 시작 RefTag로 설정 — NVMe SLBA 기준. */
		bip_set_seed(bio_integrity(bio), meta->seed);
		/* [한국어] 원본 meta->iter를 이번에 소비한 만큼 전진 — 다음 bio 호출 시
		 * 이어지는 지점부터 매핑을 시작할 수 있게 한다. */
		iov_iter_advance(&meta->iter, integrity_bytes);
		/*
		 * Advance seed for the next call, so the next bio will
		 * pick the right sector.
		 */
		/* [한국어] seed를 다음 bio의 시작 RefTag로 전진 — NVMe PI Type 1/2/3
		 * 모두에서 RefTag는 interval(논리 블록) 단위로 1씩 증가한다. */
		meta->seed += bio_integrity_intervals(bi, bio_sectors(bio));
	}
	return ret;	/* [한국어] map_user()의 성공/실패 코드를 그대로 호출자에게 전달 */
}

/**
 * bio_integrity_advance - Advance integrity vector
 * @bio:	bio whose integrity vector to update
 * @bytes_done:	number of data bytes that have been completed
 *
 * Description: This function calculates how many integrity bytes the
 * number of completed data bytes correspond to and advances the
 * integrity vector accordingly.
 */
/*
 * [한국어]
 * bio_integrity_advance - 데이터 완료량에 맞춰 integrity iterator를 전진
 *
 * @bio: iterator를 갱신할 대상 bio.
 * @bytes_done: 방금 완료 처리된 데이터 바이트 수(섹터 단위로 환산해 사용).
 * @return: 없음(void).
 *
 * 하나의 bio가 여러 번에 걸쳐 부분 완료될 수 있는 경우(예: 컨트롤러가 부분
 * 전송을 보고하거나, request가 여러 세그먼트로 나뉘어 순차 완료되는 경우),
 * 완료된 데이터 바이트 수만큼 metadata iterator도 같은 비율로 전진시켜야
 * 다음 완료 처리 시 올바른 위치의 metadata를 가리키게 된다. bi_sector(RefTag
 * seed)도 완료된 섹터 수만큼 전진시켜, NVMe 컨트롤러가 partial completion
 * 이후에도 다음 섹터에 대한 Reference Tag를 올바르게 이어받도록 한다.
 * 이 함수는 blk-mq의 완료 처리 경로에서 호출되므로 소프트/하드 IRQ
 * 컨텍스트에서 실행될 수 있다.
 *
 * 호출 체인:
 *   blk_update_request → bio_advance → bio_advance_iter
 *     (block/bio.c:3033) → [bio_integrity_advance] → bvec_iter_advance
 */
void bio_integrity_advance(struct bio *bio, unsigned int bytes_done)
{
	/* [한국어] 전진시킬 대상 integrity payload */
	struct bio_integrity_payload *bip = bio_integrity(bio);
	/* [한국어] 바이트 ↔ 섹터 환산에 필요한 디스크 PI 프로파일 */
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);
	/* [한국어] 완료된 데이터 바이트 수(>>9로 섹터 환산) → metadata 바이트 수로 변환 */
	unsigned bytes = bio_integrity_bytes(bi, bytes_done >> 9);

	/* [한국어] 완료된 데이터 바이트 수만큼 RefTag seed(bi_sector)를 전진 —
	 * NVMe 컨트롤러가 partial completion 후에도 다음 섹터의 Reference Tag가
	 * 올바르게 이어지도록 하기 위함이다. RefTag는 논리 블록마다 1씩 증가하는
	 * 값이므로, 앞부분을 소비했으면 seed도 그만큼 전진해야 남은 부분의
	 * 기대 RefTag가 맞는다. */
	bip->bip_iter.bi_sector += bio_integrity_intervals(bi, bytes_done >> 9);
	/* [한국어] metadata iterator 자체(bi_size/bi_idx/bi_bvec_done)도 방금 계산한
	 * bytes만큼 전진 — 다음 부분 완료 처리 시 정확한 위치를 가리키게 한다. */
	bvec_iter_advance(bip->bip_vec, &bip->bip_iter, bytes);
}

/**
 * bio_integrity_trim - Trim integrity vector
 * @bio:	bio whose integrity vector to update
 *
 * Description: Used to trim the integrity vector in a cloned bio.
 */
/*
 * [한국어]
 * bio_integrity_trim - 복제된 bio의 integrity iterator 크기를 재계산
 *
 * @bio: iterator 크기를 재계산할 대상(주로 clone/split된) bio.
 * @return: 없음(void).
 *
 * bio_split() 등으로 원본 bio를 여러 조각으로 나눈 뒤, 각 조각(clone)은
 * 원본과 동일한 bip_vec을 공유하지만 실제로 담당하는 데이터 섹터 수는
 * 원본보다 작다. 이 함수는 그 clone의 bip_iter.bi_size를 clone 자신의
 * bio_sectors(bio) 기준으로 다시 계산해, metadata 길이가 데이터 길이와
 * 정확히 일치하도록 맞춘다. NVMe 컨트롤러는 metadata 길이가 실제 전송
 * 데이터 길이와 어긋나면 명령을 거부하거나 오동작할 수 있으므로, split
 * 직후 반드시 호출되어야 하는 정합성 보정 함수다. 태스크 컨텍스트에서
 * bio 분할 직후 호출되는 것이 일반적이다.
 *
 * 호출 체인:
 *   bio_split (block/bio.c:3464) / bio_trim (block/bio.c:3520)
 *     → [bio_integrity_trim] → bio_integrity_bytes
 */
void bio_integrity_trim(struct bio *bio)
{
	/* [한국어] 크기를 재계산할 대상 payload */
	struct bio_integrity_payload *bip = bio_integrity(bio);
	/* [한국어] 바이트 환산에 필요한 디스크 PI 프로파일 */
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);

	/*
	 * Guard/RefTag/AppTag checks are meaningless on merged pages, so
	 * we've been carrying the size of the entire payload since split.
	 * Adjust the amount to what's really there.
	 */
	/* [한국어] 복제된 bio의 integrity iterator 크기를 실제 데이터 섹터 수에
	 * 맞춘다 — NVMe split/clone 후에도 metadata 길이가 데이터 길이와 일치해야
	 * 컨트롤러가 정상적으로 명령을 처리한다. */
	bip->bip_iter.bi_size = bio_integrity_bytes(bi, bio_sectors(bio));
}
EXPORT_SYMBOL(bio_integrity_trim);	/* [한국어] block layer 내부 분할 경로 등 다른 컴파일
					 * 유닛에서도 clone 처리 시 호출할 수 있도록 공개. */

/**
 * bio_integrity_clone - Callback for cloning bios with integrity metadata
 * @bio:	New bio
 * @bio_src:	Original bio
 * @gfp_mask:	Memory allocation mask
 *
 * Description:	Called to allocate a bip when cloning a bio
 */
/*
 * [한국어]
 * bio_integrity_clone - integrity metadata를 가진 bio를 복제할 때의 콜백
 *
 * @bio: 새로 만들어지는 clone bio(아직 bip이 없는 상태).
 * @bio_src: 복제 원본 bio(반드시 유효한 bip을 갖고 있어야 함).
 * @gfp_mask: bio_integrity_alloc()에 전달할 메모리 할당 플래그.
 * @return: 성공 시 0, 실패 시 bio_integrity_alloc()이 반환한 음수 errno.
 *
 * bio_clone_fast() 등 bio 복제 경로가 integrity metadata까지 함께 복제해야
 * 할 때 호출하는 콜백이다. 새 bip을 nr_vecs=0으로 할당한 뒤(별도 bvec 배열을
 * 새로 만들지 않고), 원본의 bip_vec 포인터·bip_iter(진행 위치/RefTag seed)·
 * app_tag를 그대로 복사하고, bip_flags는 BIP_CLONE_FLAGS로 마스킹해 클론에도
 * 의미 있는 검증/체크섬 관련 플래그만 전파한다(예: BIP_MEMPOOL처럼 원본의
 * 버퍼 소유권과 관련된 플래그는 전파하지 않아야 이중 해제를 피할 수 있다,
 * 공유한다). NVMe 관점에서는 request split/clone 후에도 동일한 RefTag seed와
 * Guard/AppTag 검증 설정이 유지되어야 컨트롤러 검증이 일관되게 동작한다.
 *
 * 호출 체인:
 *   __bio_clone (block/bio.c:1707) → [bio_integrity_clone]
 *     → bio_integrity_alloc
 */
int bio_integrity_clone(struct bio *bio, struct bio *bio_src,
			gfp_t gfp_mask)
{
	/* [한국어] 복제 원본의 integrity payload — 존재가 보장되어야 함(호출자 계약) */
	struct bio_integrity_payload *bip_src = bio_integrity(bio_src);
	struct bio_integrity_payload *bip;	/* [한국어] 새로 만들 clone bio의 payload */

	/* [한국어] 원본에 bip이 없는데 이 함수가 호출되는 것은 호출자 버그 —
	 * 즉시 커널 패닉으로 조기 발견. */
	BUG_ON(bip_src == NULL);

	/* [한국어] clone은 별도 bvec 배열이 필요 없으므로(원본과 공유) nr_vecs=0으로
	 * 헤더(bip)만 할당. */
	bip = bio_integrity_alloc(bio, gfp_mask, 0);
	if (IS_ERR(bip))
		return PTR_ERR(bip);	/* [한국어] 할당 실패 — errno 그대로 전파 */

	/*
	 * Copy the auxiliary data and mapped iterator including the flags.
	 */
	/* [한국어] 원본 bio의 integrity vector와 seed, 검증 플래그를 복제한다.
	 * NVMe: request split/clone 시에도 동일한 RefTag seed와 Guard/AppTag
	 * 검증 설정이 유지되어야 한다. */
	bip->bip_vec = bip_src->bip_vec;	/* [한국어] 별도 배열 없이 원본의 bvec 배열을 그대로 공유(포인터 복사) */
	bip->bip_iter = bip_src->bip_iter;	/* [한국어] 진행 위치/RefTag seed(bi_sector)/bi_size를 그대로 복제 */
	bip->bip_flags = bip_src->bip_flags & BIP_CLONE_FLAGS;	/* [한국어] 클론에 전파해도 안전한 플래그만 마스킹해 복사 */
	bip->app_tag = bip_src->app_tag;	/* [한국어] Application Tag 값도 그대로 복제 */

	return 0;	/* [한국어] 정상 완료 */
}

/*
 * [한국어]
 * bio_integrity_initfn - 모듈/서브시스템 초기화 시 integrity_buf_pool을 생성
 *
 * @return: 0(성공). mempool 생성 실패는 panic()으로 즉시 부팅을 중단시키므로
 *      이 함수가 정상 반환되는 경우는 항상 성공을 의미한다.
 *
 * 커널 부팅 과정의 subsys_initcall 단계에서 1회 실행되어, 이후 I/O 경로 중
 * 메모리 부족 상황에서도 metadata 버퍼를 확보할 수 있도록 미리
 * BLK_INTEGRITY_MAX_SIZE 크기의 페이지 mempool을 준비하는 초기화 함수다.
 * 이 mempool은 bio_integrity_alloc_buf()가 GFP_NOIO 상황에서 kmalloc이
 * 실패했을 때 fallback으로 사용하므로, mempool 자체의 생성 실패는 이후
 * 모든 NVMe I/O의 안전망이 사라짐을 의미해 panic()으로 처리한다(초기화
 * 시점이므로 이 정도로 치명적인 처리가 정당화된다). 부팅 시퀀스 중 단
 * 한 번만 실행되므로 동시성 문제는 없다.
 *
 * 호출 체인:
 *   (커널 부팅, subsys_initcall 매크로가 등록) → [bio_integrity_initfn]
 *   → mempool_init_page_pool
 */
static int __init bio_integrity_initfn(void)
{
	/* [한국어] BLK_INTEGRITY_MAX_SIZE 크기의 페이지를 BIO_POOL_SIZE개 미리
	 * 예약하는 mempool 생성 — NVMe metadata bounce 버퍼용 최후의 안전망.
	 * 실패 시 이후 모든 PI 사용 I/O가 메모리 부족 상황에서 진행 불가하므로
	 * panic으로 부팅을 중단한다. */
	if (mempool_init_page_pool(&integrity_buf_pool, BIO_POOL_SIZE,
			get_order(BLK_INTEGRITY_MAX_SIZE)))
		panic("bio: can't create integrity buf pool\n");	/* [한국어] 복구 불가능한 초기화 실패 — 즉시 패닉 */
	return 0;	/* [한국어] 정상 완료 */
}
subsys_initcall(bio_integrity_initfn);	/* [한국어] 다른 서브시스템(block layer 본체)이
					 * 초기화된 이후, 드라이버 프로브 이전 단계에서 이 초기화
					 * 함수를 자동 실행하도록 등록 — 부팅 순서 보장용 매크로. */

/*
 * ============================================================================
 * NVMe 관점 핵심 요약
 * ============================================================================
 * - 이 파일은 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *   -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로에서, 데이터와 함께
 *   전달되는 T10 DIF/DIX 형식의 integrity 메타데이터(Guard/AppTag/RefTag)를
 *   bio 단위로 관리한다.
 * - NVMe namespace format에 의해 결정된 metadata_size/pi_tuple_size를
 *   기준으로, NVMe 컨트롤러가 PI 생성/검증을 offload할지, 아니면 host
 *   소프트웨어에서 수행할지를 __bio_integrity_action()에서 판단한다.
 * - bio_integrity_alloc_buf()로 할당된 메타데이터 버퍼는 NVMe READ/WRITE
 *   명령의 metadata SGL/PRP로 매핑되며, bio_integrity_advance()와
 *   bio_integrity_trim()은 split/clone 후에도 Reference Tag seed가
 *   연속성을 유지하도록 돕는다.
 * - bio_integrity_map_user()와 bio_integrity_copy_user()는 사용자 공간
 *   메타데이터를 NVMe DMA 요구사항(alignment, segment 제한, P2PDMA)에 맞게
 *   전처리한다.
 * - 본 파일은 block/bio.c(기본 bio 관리) 및 block/blk-integrity.c
 *   (request_queue integrity profile)와 논리적으로 연결되며, NVMe 드라이버의
 *   drivers/nvme/host/pci.c 등에서 실제 명령어 조립(PRACT/PRCHK, metadata
 *   pointer)으로 이어진다.
 * ============================================================================
 */
