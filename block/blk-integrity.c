// SPDX-License-Identifier: GPL-2.0
/*
 * blk-integrity.c - Block layer data integrity extensions
 *
 * Copyright (C) 2007, 2008 Oracle Corporation
 * Written by: Martin K. Petersen <martin.petersen@oracle.com>
 */
/*
 * [한국어 설명] blk-integrity.c — request_queue 단위 무결성(integrity)
 * 프로파일 등록/조회, 스택 디바이스 호환성 검사, sysfs 노출 (blk-integrity.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 request_queue(디스크/네임스페이스) 단위로 무결성 프로파일인
 * struct blk_integrity를 다루는 블록 계층 코드다. 구조체 자체의 정의는
 * include/linux/blkdev.h에 있지만(flags/csum_type/metadata_size/pi_offset/
 * interval_exp/tag_size/pi_tuple_size 7개 필드), 이 프로파일을 실제로
 * 소비·검사·노출하는 로직은 모두 이 파일에 모여 있다. 주요 역할은 네
 * 가지다: (1) request 병합 시 두 request/bio의 PI 설정 호환성 검사
 * (blk_integrity_merge_rq/bio), (2) bio의 무결성 bio_vec들을 scatterlist
 * 세그먼트 수로 환산(blk_rq_count_integrity_sg), (3) 사용자 공간
 * passthrough를 위한 메타데이터 매핑과 능력(capability) 조회
 * (blk_rq_integrity_map_user, blk_get_meta_cap), (4) /sys/block/<disk>/
 * integrity/ 아래 sysfs 속성을 통한 프로파일 노출(blk_integrity_attr_group과
 * 그 하위 show/store 함수들). T10 PI(Protection Information,
 * DIF(Data Integrity Field)/DIX(Data Integrity Extension)) 스펙은 논리
 * 블록(섹터)마다 Guard(체크섬)/Application Tag/Reference Tag로 구성된
 * 8바이트 tuple을 별도 메타데이터 영역에 두는데, 이 파일은 그 tuple의
 * 존재 여부(csum_type)·크기(metadata_size/pi_tuple_size)·검증 방식(flags)을
 * 디스크 단위로 표현하고 다룬다. 이 파일이 없으면 dm/md 같은 스택 디바이스를
 * 구성할 때 하위 디바이스들의 PI 설정 호환 여부를 검증할 방법이 없고,
 * 사용자 공간도 디스크의 PI 능력을 조회할 표준 sysfs/ioctl 인터페이스를
 * 갖지 못한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (위 → 아래):
 *   파일시스템/사용자 공간(O_DIRECT+PI, nvme-cli passthrough, io_uring 등)
 *     → blk_rq_integrity_map_user() / blk_get_meta_cap()          ← 이 파일
 *   블록 계층 병합 경로:
 *     submit_bio() → blk_mq_submit_bio()
 *       → blk_attempt_bio_merge()/attempt_merge() (blk-merge.c)
 *         → blk_integrity_merge_bio() / blk_integrity_merge_rq()  ← 이 파일
 *     → blk_rq_map_sg() (blk-merge.c)가 세그먼트를 구성할 때
 *       blk_rq_count_integrity_sg()로 필요한 세그먼트 수를 사전 계산 ← 이 파일
 *     → nvme_map_data()/nvme_setup_prps()/nvme_setup_sgls()
 *       (drivers/nvme/host/, 추정) → NVMe 컨트롤러 SQ(Submission Queue)
 *   드라이버 등록 경로:
 *     NVMe 드라이버가 Identify Namespace의 metadata 포맷을 파싱해
 *     q->limits.integrity(struct blk_integrity)를 채움
 *     (drivers/nvme/host/core.c, 추정)
 *       → 사용자 공간이 /sys/block/<disk>/integrity/ 아래 sysfs 속성으로 조회
 *
 * 실행 컨텍스트: merge/count/map_user/get_meta_cap 계열 함수는 I/O 제출
 * 경로의 프로세스 컨텍스트에서 호출된다. sysfs show/store 콜백은 사용자
 * 공간이 read(2)/write(2)로 sysfs 파일에 접근할 때 그 호출 프로세스
 * 컨텍스트에서 직접 실행되며, flag_store()는 queue_limits_commit_update_frozen()
 * 을 통해 큐를 잠깐 freeze한 뒤 갱신한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - include/linux/blkdev.h    : struct blk_integrity,
 *     enum blk_integrity_checksum(BLK_INTEGRITY_CSUM_NONE/IP/CRC/CRC64) 정의.
 *     request_queue->limits.integrity 필드로 내장되어 있다.
 *   - include/linux/blk-integrity.h : blk_get_integrity(), blk_integrity_rq(),
 *     BLK_INTEGRITY_*(DEVICE_CAPABLE/REF_TAG/NOVERIFY/NOGENERATE) 플래그 선언.
 *   - include/linux/t10-pi.h    : struct t10_pi_tuple/struct crc64_pi_tuple 등
 *     T10 PI tuple 레이아웃 — blk_get_meta_cap()이 ref_tag 필드 크기를
 *     sizeof_field()로 조회할 때 사용한다.
 *   - block/blk.h               : blk_integrity_rq(), integrity_req_gap_back_merge()
 *     등 병합 헬퍼와 dev_to_disk() 등 내부(non-export) 인터페이스 선언.
 *   - block/bio-integrity.c     : struct bio_integrity_payload(bip)를
 *     생성·관리 — 이 파일의 blk_integrity_merge_rq/bio(),
 *     blk_rq_count_integrity_sg()가 bio_integrity()로 그 내용을 읽는다.
 *   - block/blk-settings.c      : queue_limits_start_update()/
 *     queue_limits_commit_update_frozen() — flag_store()가 sysfs write로
 *     받은 값을 큐에 반영할 때 사용하는 원자적 갱신 인프라.
 *   - drivers/nvme/host/        : Identify Namespace/Controller의 PI 필드를
 *     파싱해 q->limits.integrity를 채우는 코드(core.c, 추정) — 이 파일이
 *     다루는 blk_integrity 값의 실제 출처.
 *
 * 데이터 흐름:
 *   NVMe Identify Namespace(LBA Format, metadata size, PI type)
 *     → 드라이버가 struct blk_integrity로 변환 → q->limits.integrity에 저장
 *     → 이 파일의 sysfs show 콜백들이 읽어 사용자 공간에 문자열/숫자로 노출
 *     → 이 파일의 merge/count 함수들이 I/O 제출 경로에서 매 request/bio마다
 *       참조해 병합 가능 여부와 필요 세그먼트 수를 계산
 *     → blk_get_meta_cap()이 struct logical_block_metadata_cap으로 재포장해
 *       FS_IOC_GETLBMD_CAP ioctl 사용자에게 반환
 *
 * 공유 핵심 자료구조:
 *   struct blk_integrity(bi)   : request_queue->limits.integrity에 내장되는
 *     디스크 단위 PI 프로파일. flags/csum_type/metadata_size/pi_offset/
 *     interval_exp/tag_size/pi_tuple_size 7개 필드로 구성되며, 이 파일의
 *     거의 모든 함수가 읽거나(sysfs show, merge 검사) 쓴다
 *     (flag_store를 통한 flags 갱신).
 *   struct bio_integrity_payload(bip) : block/bio-integrity.c가 소유.
 *     이 파일은 blk_integrity_merge_rq/bio()에서 bip_flags/app_tag를
 *     읽기 전용으로 비교하고, blk_rq_count_integrity_sg()에서
 *     bio_for_each_integrity_vec()로 그 bio_vec 배열을 순회한다.
 *   struct logical_block_metadata_cap : FS_IOC_GETLBMD_CAP ioctl의 사용자
 *     공간 결과 구조체 — blk_get_meta_cap()이 blk_integrity 값을 이
 *     형태로 변환해 채운다.
 *
 * === 주요 함수/구조체 요약 ===
 * blk_rq_count_integrity_sg() : bio의 무결성 bio_vec들을 물리적 연속성/
 *                               세그먼트 크기 한도로 병합해 필요한
 *                               scatterlist 세그먼트 수를 계산.
 * blk_get_meta_cap()          : FS_IOC_GETLBMD_CAP ioctl 핸들러 — blk_integrity
 *                               프로파일을 사용자 공간 logical_block_metadata_cap
 *                               구조체로 변환해 복사.
 * blk_rq_integrity_map_user() : 사용자 공간 버퍼(passthrough 명령)를 request의
 *                               bio에 무결성 메타데이터로 매핑.
 * blk_integrity_merge_rq()/blk_integrity_merge_bio() : 두 request 또는
 *                               request+bio의 PI 설정(플래그/app_tag/세그먼트
 *                               한도/gap)이 호환되는지 검사해 병합 가능
 *                               여부를 결정.
 * blk_integrity_profile_name() : csum_type과 REF_TAG 플래그 조합을
 *                               "T10-DIF-TYPE1-CRC" 같은 표준 프로파일 이름
 *                               문자열로 변환.
 * flag_store()/flag_show()   : sysfs를 통한 반전(inverted) 불리언 플래그
 *                               저장/조회 공통 헬퍼 — read_verify/write_generate
 *                               속성이 재사용.
 * blk_integrity_attr_group    : /sys/block/<disk>/integrity/ 디렉터리에 format,
 *                               tag_size, protection_interval_bytes,
 *                               read_verify, write_generate,
 *                               device_is_integrity_capable 6개 속성을
 *                               등록하는 attribute_group.
 */

#include <linux/blk-integrity.h>	/* [한국어] struct blk_integrity, blk_get_integrity(), blk_integrity_rq(),
					 * BLK_INTEGRITY_* 플래그 선언 — 이 파일 전역에서 다루는 PI 프로파일의 타입. */
#include <linux/backing-dev.h>		/* [한국어] backing_dev_info 관련 선언 — request_queue의 읽기 앞서읽기(read-ahead)
					 * 등 bdi 계층과 연동되는 헬퍼(직접 사용은 없으나 blkdev.h 체인이 요구). */
#include <linux/mempool.h>		/* [한국어] mempool_t 타입 선언 — 이 파일 자체는 mempool을 쓰지 않지만
					 * blk-integrity.h/bio-integrity.c와 헤더 의존성을 공유하기 위해 포함. */
#include <linux/bio.h>			/* [한국어] struct bio, bio_for_each_integrity_vec(), bio_op() 등 — bio 단위로
					 * 무결성 bio_vec을 순회하거나 연산 종류(READ/WRITE)를 판별할 때 필요. */
#include <linux/scatterlist.h>		/* [한국어] scatterlist 관련 타입/헬퍼 — blk_rq_count_integrity_sg()가 계산하는
					 * "세그먼트 수"가 최종적으로 매핑될 자료구조의 정의를 제공. */
#include <linux/export.h>		/* [한국어] EXPORT_SYMBOL_GPL() 매크로 — blk_rq_integrity_map_user(),
					 * blk_integrity_profile_name() 등을 다른 컴파일 유닛/모듈에 공개하기 위해 필요. */
#include <linux/slab.h>			/* [한국어] kmalloc/kfree 계열 선언 — 이 파일 자체는 직접 호출하지 않지만
					 * 포함 체인상 blk_integrity 관련 헬퍼들이 요구하는 공통 슬랩 할당자 선언. */
#include <linux/t10-pi.h>		/* [한국어] struct t10_pi_tuple, struct crc64_pi_tuple 등 T10 PI(Guard/AppTag/
					 * RefTag) tuple 레이아웃 — blk_get_meta_cap()이 sizeof_field()로 ref_tag
					 * 필드 크기를 조회할 때 사용한다. */

#include "blk.h"			/* [한국어] block layer 내부(비공개) 헤더 — dev_to_disk(), blk_integrity_rq(),
					 * integrity_req_gap_back_merge() 등 모듈 외부에 노출되지 않는 선언을 가져온다. */

/**
 * blk_rq_count_integrity_sg - Count number of integrity scatterlist elements
 * @q:		request queue
 * @bio:	bio with integrity metadata attached
 *
 * Description: Returns the number of elements required in a
 * scatterlist corresponding to the integrity metadata in a bio.
 */
/*
 * [한국어]
 * blk_rq_count_integrity_sg - bio의 무결성 메타데이터가 필요로 하는
 *                             scatterlist 세그먼트 수를 계산
 *
 * @q: bio가 제출될 request_queue. queue_max_segment_size()로 세그먼트
 *      최대 크기 한도를 조회하는 데 사용된다.
 * @bio: 무결성 메타데이터가 이미 부착된 bio. bio_for_each_integrity_vec()로
 *      그 metadata bio_vec 배열을 순회한다.
 * @return: 이 bio의 무결성 메타데이터를 담기 위해 필요한 scatterlist
 *      세그먼트(엔트리) 개수. 물리적으로 인접하고 세그먼트 크기 한도
 *      이내인 bvec들은 하나의 세그먼트로 합쳐지므로, 반환값은 항상
 *      bio_for_each_integrity_vec()가 순회하는 bvec 개수 이하다.
 *
 * NVMe SQ(Submission Queue) 명령은 데이터 버퍼뿐 아니라 선택적 PI(Protection
 * Information) 메타데이터 버퍼도 PRP(Physical Region Page)/SGL(Scatter
 * Gather List)로 기술해야 하므로(metadata를 가진 LBA Format), 드라이버가
 * 실제 PRP/SGL 엔트리를 만들기 전에 몇 개의 엔트리가 필요한지 미리 알아야
 * 한다. 이 함수는 bio에 매달린 무결성 bio_vec들을 순서대로 훑으면서,
 * 이전 bvec과 물리적으로 연속(biovec_phys_mergeable())하고 합친 크기가
 * queue_max_segment_size() 한도를 넘지 않으면 세그먼트를 늘리지 않고
 * 누적만 하며, 그렇지 않으면 새 세그먼트를 하나 더 센다. 이 함수는 순수
 * 계산 함수로 상태를 변경하지 않으며, 병합/세그먼트 매핑 경로(프로세스
 * 컨텍스트)에서 재진입 없이 호출된다.
 *
 * 호출 체인:
 *   blk_rq_map_sg() (blk-merge.c, 추정) → [blk_rq_count_integrity_sg]
 *   → biovec_phys_mergeable → nvme_setup_prps/nvme_setup_sgls (드라이버, 추정)
 */
int blk_rq_count_integrity_sg(struct request_queue *q, struct bio *bio)
{
	struct bio_vec iv, ivprv = { NULL };	/* 현재/이전 integrity 벡터; 각 bvec은 PI 메타데이터 물리 페이지를 가리키며 NVMe PRP/SGL 엔트리 후보 */
	unsigned int segments = 0;		/* 최종 sg 세그먼트 수 (NVMe PRP/SGL 엔트리 수 추정) */
	unsigned int seg_size = 0;		/* 현재 누적 세그먼트 크기; NVMe SGL segment 한도 추적용 */
	struct bvec_iter iter;			/* bio_integrity 벡터 순회용; NVMe DMA sg 엔트리 생성 전 단계 */
	int prev = 0;				/* 첫 번째 integrity bvec 여부; 연속성 판단 상태 */

	/* bio의 무결성 메타데이터 영역을 순회; 데이터 영역과 별도의 bvec이다. */
	bio_for_each_integrity_vec(iv, bio, iter) {

		if (prev) {	/* 두 번째 이후 반복: 직전 bvec(ivprv)과 비교할 대상이 있음 */
			/* 이전 bvec과 현재 bvec의 물리 연속성 검사;
			 * 연속이면 NVMe SGL segment 하나로 합쳐진다. */
			if (!biovec_phys_mergeable(q, &ivprv, &iv))
				goto new_segment;	/* 물리적으로 불연속 — 새 세그먼트로 분리해야 함 */
			/* queue_max_segment_size()는 DMA/PCIe 메모리 매핑 단위를
			 * 고려한 값; NVMe 컨트롤러의 max sdata segment 크기와
			 * 관련될 수 있다(추정). */
			if (seg_size + iv.bv_len > queue_max_segment_size(q))
				goto new_segment;	/* 합치면 세그먼트 크기 한도 초과 — 새 세그먼트로 분리 */

			seg_size += iv.bv_len;	/* 현재 segment의 누적 크기 갱신 */
		} else {	/* 첫 번째 bvec(prev==0)이면 비교 대상이 없으므로 곧바로 새 세그먼트 취급 */
new_segment:		/* 물리적 불연속 또는 크기 한도 초과로 새 세그먼트가 필요한 지점 */
			segments++;		/* 새로운 sg/PRP/SGL 엔트리 추가 (추정) */
			seg_size = iv.bv_len;	/* 새 segment 시작; NVMe PRP/SGL의 다음 엔트리 크기 */
		}

		prev = 1;			/* 다음 루프부터 연속성 비교 가능 */
		ivprv = iv;			/* 이전 벡터 저장; 다음 iter에서 biovec_phys_mergeable() 입력 */
	}

	return segments;	/* 최종 세그먼트(엔트리) 수 반환 — 호출자가 PRP/SGL 배열 크기 산정에 사용 */
}

/**
 * blk_get_meta_cap - 파일 시스템이 볼 수 있는 블록 디바이스의 무결성 능력 조회
 * @bdev:	블록 디바이스
 * @cmd:	ioctl 명령
 * @argp:	사용자 공간 구조체 포인터
 */
/*
 * [한국어]
 * blk_get_meta_cap - FS_IOC_GETLBMD_CAP ioctl 핸들러: 디스크의 무결성 능력을
 *                    사용자 공간 구조체(logical_block_metadata_cap)로 변환
 *
 * @bdev: 능력을 조회할 대상 블록 디바이스. bdev->bd_disk의 request_queue에서
 *      blk_integrity 프로파일을 얻는다.
 * @cmd: ioctl 명령 번호. _IOC_SIZE(cmd)로 사용자 구조체 크기를 얻어
 *      extensible_ioctl_valid()가 FS_IOC_GETLBMD_CAP/LBMD_SIZE_VER0 대비
 *      버전 호환성을 검사하는 데 쓰인다.
 * @return: 성공 시 copy_struct_to_user()가 반환하는 값(보통 0), 실패 시
 *      음수 errno — cmd가 FS_IOC_GETLBMD_CAP 계열이 아니거나 크기가
 *      호환되지 않으면 -ENOIOCTLCMD, 사용자 공간 쓰기 실패 시
 *      copy_struct_to_user() 내부에서 반환하는 -EFAULT 등.
 *
 * 파일시스템/사용자 공간이 O_DIRECT+PI 조합으로 직접 무결성 메타데이터를
 * 다루려 할 때(io_uring/nvme-cli passthrough 등), 대상 디스크가 어떤 PI
 * 능력(Guard 종류, tag 크기, 보호 구간 크기 등)을 갖는지 표준 ioctl로 조회할
 * 수 있게 하는 것이 이 함수의 목적이다. blk_get_integrity()로 얻은
 * blk_integrity 프로파일의 저수준 필드(flags/csum_type/metadata_size/
 * pi_offset/interval_exp)를 FS_IOC_GETLBMD_CAP 전용 uAPI 구조체
 * logical_block_metadata_cap의 필드로 하나씩 변환한다. 디스크가 PI를 전혀
 * 노출하지 않으면(blk_get_integrity()가 NULL) 모든 필드가 0인 meta_cap을
 * 그대로 반환해 "능력 없음"을 표현한다. 이 함수는 ioctl 시스템 호출 경로의
 * 프로세스 컨텍스트에서 실행되며 락을 잡지 않는 순수 조회 함수다.
 *
 * 호출 체인:
 *   사용자 공간 ioctl(FS_IOC_GETLBMD_CAP) → VFS ioctl 디스패치(추정)
 *   → [blk_get_meta_cap] → blk_get_integrity → copy_struct_to_user
 */
int blk_get_meta_cap(struct block_device *bdev, unsigned int cmd,
		     struct logical_block_metadata_cap __user *argp)
{
	struct blk_integrity *bi;	/* NVMe Identify Namespace의 PI 능력이 매핑된 블록 계층 포인터 */
	struct logical_block_metadata_cap meta_cap = {};	/* 사용자 공간으로 복사할 capability 구조체; NVMe LBA Format의 PI 필드를 추상화 */
	size_t usize = _IOC_SIZE(cmd);	/* ioctl 크기 검증; compatibility ioctl 경로 */

	/* cmd가 FS_IOC_GETLBMD_CAP 계열인지, 요청 크기가 LBMD_SIZE_VER0 이상의
	 * 호환 가능한 크기인지 검사 — 커널/사용자 공간 구조체 버전 불일치 방어. */
	if (!extensible_ioctl_valid(cmd, FS_IOC_GETLBMD_CAP, LBMD_SIZE_VER0))
		return -ENOIOCTLCMD;	/* 지원하지 않는 ioctl/크기 — VFS가 다른 핸들러를 시도하도록 표준 errno 반환 */

	/* bdev의 request_queue에서 무결성 프로파일 획득; NVMe namespace
	 * 등록 시 nvme driver가 설정한 값이다. */
	bi = blk_get_integrity(bdev->bd_disk);
	if (!bi)
		goto out;			/* NVMe namespace가 PI를 노출하지 않는 경우(NVMe 1.0 / PI 미지원 / disabled) */

	/* NVMe 컨트롤러가 End-to-end Data Protection을 지원하는지 여부
	 * (ID_CTRL.DPS bit 연동 추정). */
	if (bi->flags & BLK_INTEGRITY_DEVICE_CAPABLE)
		meta_cap.lbmd_flags |= LBMD_PI_CAP_INTEGRITY;	/* 디스크가 하드웨어 PI 생성/검증 지원 — 능력 비트 설정 */
	/* NVMe Reference Tag(Logical Block Reference Tag) 검사 지원. */
	if (bi->flags & BLK_INTEGRITY_REF_TAG)
		meta_cap.lbmd_flags |= LBMD_PI_CAP_REFTAG;	/* RefTag 검증 지원 — 능력 비트 설정 */
	/* interval_exp: 2의 거듭제곱 형태의 보호 구간 크기;
	 * NVMe PI는 일반적으로 논리 블록 단위(LBA Data Size)의 배수로
	 * 구간을 설정한다(추정). */
	meta_cap.lbmd_interval = 1 << bi->interval_exp;	/* 2^interval_exp 바이트마다 PI tuple 하나가 대응 */
	/* metadata_size: NVMe LBA Format에서 보고하는 메타데이터 바이트 수;
	 * PI tuple(Guard/App/Ref) + opaque 영역을 포함할 수 있다. */
	meta_cap.lbmd_size = bi->metadata_size;		/* 논리 블록당 전체 메타데이터 바이트 수 그대로 복사 */
	/* NVMe PRCHK/PRACT가 참조하는 Guard/App/Ref tuple 크기. */
	meta_cap.lbmd_pi_size = bi->pi_tuple_size;		/* PI tuple 자체의 바이트 수(전형적으로 8바이트) */
	/* 메타데이터 내 PI tuple 위치; NVMe Format에 따른 메타데이터 배치(처음/끝/별도 버퍼)와 연관. */
	meta_cap.lbmd_pi_offset = bi->pi_offset;		/* 메타데이터 버퍼 내 PI tuple 시작 오프셋 그대로 복사 */
	/* PI tuple 외 NVMe namespace별 추가 메타데이터 영역. */
	meta_cap.lbmd_opaque_size = bi->metadata_size - bi->pi_tuple_size;	/* 전체 메타데이터에서 PI tuple을 뺀 나머지(사용자 정의 영역) 크기 */
	/* PI tuple이 메타데이터 선두가 아닌 경우 opaque offset 조정. */
	if (meta_cap.lbmd_opaque_size && !bi->pi_offset)
		meta_cap.lbmd_opaque_offset = bi->pi_tuple_size;	/* PI tuple이 선두(오프셋 0)이면 opaque 영역은 그 바로 뒤(pi_tuple_size)부터 시작 */

	/* csum_type(Guard 알고리즘 종류)에 따라 사용자 공간 열거형으로 매핑 —
	 * 두 열거형(blk_integrity_checksum ↔ lbmd_guard_tag_type)의 값 공간이
	 * 다르므로 반드시 명시적으로 변환해야 한다. */
	switch (bi->csum_type) {
	case BLK_INTEGRITY_CSUM_NONE:
		meta_cap.lbmd_guard_tag_type = LBMD_PI_CSUM_NONE;	/* Guard 미사용 — PI 자체가 없는 포맷 */
		break;
	case BLK_INTEGRITY_CSUM_IP:
		/* NVMe IP checksum guard 지원 시(추정). */
		meta_cap.lbmd_guard_tag_type = LBMD_PI_CSUM_IP;
		break;
	case BLK_INTEGRITY_CSUM_CRC:
		/* NVMe CRC16/T10-DIF guard; 가장 일반적인 NVMe PI-16 모드. */
		meta_cap.lbmd_guard_tag_type = LBMD_PI_CSUM_CRC16_T10DIF;
		break;
	case BLK_INTEGRITY_CSUM_CRC64:
		/* NVMe CRC64 guard; T10-DIF 64-bit 확장 또는 NVMe PI-64
		 * 모드와 매핑(추정). */
		meta_cap.lbmd_guard_tag_type = LBMD_PI_CSUM_CRC64_NVME;
		break;
	}	/* default 분기 없음 — enum blk_integrity_checksum의 모든 값이 위 4개 case로 열거됨 */

	/* PI 검사가 활성화된 NVMe namespace에 대해 App Tag 필드 존재. */
	if (bi->csum_type != BLK_INTEGRITY_CSUM_NONE)
		meta_cap.lbmd_app_tag_size = 2;	/* NVMe APP TAG는 2바이트(T10 PI 표준) */

	/* Ref Tag 크기는 checksum 종류에 따라 다름; NVMe PI Type1/Type2/Type3와 매핑. */
	if (bi->flags & BLK_INTEGRITY_REF_TAG) {	/* RefTag 자체를 쓰지 않는 프로파일이면 아래 크기 계산이 무의미 */
		switch (bi->csum_type) {
		case BLK_INTEGRITY_CSUM_CRC64:
			meta_cap.lbmd_ref_tag_size =
				sizeof_field(struct crc64_pi_tuple, ref_tag);	/* CRC64 tuple의 ref_tag 필드 폭(바이트)을 그대로 조회 */
			break;
		case BLK_INTEGRITY_CSUM_CRC:
		case BLK_INTEGRITY_CSUM_IP:
			meta_cap.lbmd_ref_tag_size =
				sizeof_field(struct t10_pi_tuple, ref_tag);	/* 표준 T10 PI tuple의 ref_tag 필드 폭(바이트) 조회 */
			break;
		default:
			break;	/* CSUM_NONE 등 RefTag 폭을 정의할 수 없는 조합 — 0으로 남김 */
		}
	}

out:
	/* 사용자 공간 ioctl 결과 복사; 실패 시 -EFAULT. */
	return copy_struct_to_user(argp, usize, &meta_cap, sizeof(meta_cap),
				   NULL);	/* usize 이후 잘려나간 필드는 커널이 알아서 0으로 채워 반환(확장 가능한 ioctl 관례) */
}

/**
 * blk_rq_integrity_map_user - 사용자 공간 버퍼의 무결성 메타데이터를 bio에 매핑
 * @rq:		블록 요청(request)
 * @ubuf:	사용자 공간 메타데이터 버퍼
 * @bytes:	매핑할 바이트 수
 */
/*
 * [한국어]
 * blk_rq_integrity_map_user - 사용자 공간 무결성 메타데이터 버퍼를
 *                             request의 bio에 매핑하고 세그먼트 정보 갱신
 *
 * @rq: 이미 데이터 bio가 부착된 블록 요청(request). rq->bio에 무결성
 *      메타데이터를 매핑한다.
 * @ubuf: 사용자 공간의 무결성 메타데이터 버퍼 시작 주소.
 * @bytes: ubuf에서 매핑할 바이트 수.
 * @return: 성공 시 0, 실패 시 bio_integrity_map_user()가 반환한 음수
 *      errno(예: 사용자 주소 접근 실패 시 -EFAULT, 메모리 부족 시
 *      -ENOMEM)를 그대로 전달.
 *
 * passthrough(ioctl) 계열 명령에서 사용자가 데이터 버퍼뿐 아니라 무결성
 * 메타데이터 버퍼도 직접 제공하는 경우, 그 사용자 버퍼를 rq->bio에 매핑해
 * 커널 병합/세그먼트 계산 경로가 일반 bio와 동일하게 다룰 수 있도록
 * 준비하는 것이 이 함수의 목적이다. iov_iter_ubuf()로 rq_data_dir(rq)
 * 방향(READ/WRITE)에 맞는 반복자를 만든 뒤 bio_integrity_map_user()에
 * 위임하고, 매핑이 끝나면 blk_rq_count_integrity_sg()로 필요한 세그먼트
 * 수를 계산해 request에 기록하며 REQ_INTEGRITY 플래그를 세워 이 request가
 * 무결성 메타데이터를 갖는다는 사실을 이후 병합/드라이버 계층에 알린다.
 * 이 함수는 ioctl 시스템 호출 경로의 프로세스 컨텍스트에서 실행되며,
 * bio_integrity_map_user()가 사용자 페이지를 pin(get_user_pages류)할 수
 * 있으므로 원자적 컨텍스트에서 호출해서는 안 된다.
 *
 * 호출 체인:
 *   (NVMe passthrough ioctl 핸들러, 추정) → [blk_rq_integrity_map_user]
 *   → bio_integrity_map_user → blk_rq_count_integrity_sg
 */
int blk_rq_integrity_map_user(struct request *rq, void __user *ubuf,
			      ssize_t bytes)
{
	int ret;				/* bio_integrity_map_user() 반환값; NVMe passthrough 메타데이터 매핑 성공/실패 */
	struct iov_iter iter;			/* 사용자 공간 버퍼를 기술; NVMe SQ 명령의 PI 메타데이터 소스 */

	/* READ/WRITE 방향에 따라 사용자 버퍼 iterator 초기화;
	 * NVMe opcode 방향과 일치해야 함. */
	iov_iter_ubuf(&iter, rq_data_dir(rq), ubuf, bytes);
	/* PI 메타데이터를 bio integrity payload로 매핑; NVMe passthrough에서만 사용(추정). */
	ret = bio_integrity_map_user(rq->bio, &iter);
	if (ret)
		return ret;			/* 매핑 실패 시 NVMe 명령 생성 전 단계에서 리턴; -ENOMEM/-EINVAL 등 */

	/* 무결성 세그먼트 수를 계산하여 request에 기록;
	 * nvme_setup_prps()/nvme_setup_sgl() 등에서 DMA sg 매핑 시 활용
	 * (NVMe driver 낮은 계층으로 전달됨). */
	rq->nr_integrity_segments = blk_rq_count_integrity_sg(rq->q, rq->bio);
	rq->cmd_flags |= REQ_INTEGRITY;	/* 이 request에 PI 메타데이터가 있음을 표시 */
	return 0;	/* 매핑 및 세그먼트 계산 정상 완료 */
}
EXPORT_SYMBOL_GPL(blk_rq_integrity_map_user);	/* [한국어] 드라이버(NVMe ioctl 핸들러 등)가 모듈 경계 밖에서도
						 * 이 매핑 함수를 호출할 수 있도록 공개 심볼로 노출. */

/**
 * blk_integrity_merge_rq - 두 request를 병합할 때 무결성 호환성 검사
 * @q:		request queue
 * @req:		기존 request
 * @next:	병합 후보 request
 */
/*
 * [한국어]
 * blk_integrity_merge_rq - 두 request를 하나로 합칠 때 PI 설정이 서로
 *                          호환되는지 검사
 *
 * @q: 두 request가 속한 request_queue. q->limits.max_integrity_segments로
 *      합산 세그먼트 수의 한도를 검사한다.
 * @req: 병합 대상이 되는(다른 bio/request를 흡수할) 기존 request.
 * @next: req 뒤에 이어붙이려는 병합 후보 request.
 * @return: true면 두 request를 하나로 합쳐도 안전(PI 설정 호환), false면
 *      병합을 거부해야 함(별도 SQ 명령으로 유지).
 *
 * I/O 스케줄러/plug 병합 단계에서 두 request를 하나로 합치면 NVMe SQ에
 * 제출되는 명령 수가 줄어 처리량이 늘지만, 두 request의 PI(Protection
 * Information) 검사 방식이 다르면 컨트롤러가 하나의 명령(CID)으로 두 가지
 * 서로 다른 PRCHK/PRACT 정책을 적용할 수 없으므로 병합해서는 안 된다.
 * 이 함수는 (1) 양쪽 모두 PI가 없으면 무조건 병합 허용, (2) 한쪽에만
 * 있으면 무조건 거부, (3) 양쪽 다 있으면 bip_flags/app_tag가 완전히
 * 같은지, 세그먼트 합이 큐 한도를 넘지 않는지, 두 request의 무결성
 * 버퍼 사이에 gap이 없는지를 순서대로 검사하는 다단계 판정 로직이다.
 * 이 함수는 병합 판단 경로(프로세스 컨텍스트)에서 호출되며 상태를
 * 변경하지 않는 순수 판정 함수다.
 *
 * 호출 체인:
 *   blk_attempt_req_merge() (blk-merge.c) → [blk_integrity_merge_rq]
 *   → bio_integrity, integrity_req_gap_back_merge
 */
bool blk_integrity_merge_rq(struct request_queue *q, struct request *req,
			    struct request *next)
{
	struct bio_integrity_payload *bip, *bip_next;	/* 각 request의 bio에 부착된 PI payload; NVMe PRCHK/PRACT 설정의 근원 */

	/* 양쪽 모두 PI 메타데이터가 없으면 병합 가능; NVMe에서는 일반
	 * READ/WRITE 명령으로 처리. */
	if (blk_integrity_rq(req) == 0 && blk_integrity_rq(next) == 0)
		return true;	/* 둘 다 PI 없음 — 무조건 병합 허용 */

	/* 한쪽에만 PI가 있으면 동일한 CID 하나로는 서로 다른 PI 정책을
	 * 처리할 수 없어 병합 거부. */
	if (blk_integrity_rq(req) == 0 || blk_integrity_rq(next) == 0)
		return false;	/* 한쪽만 PI 보유 — 정책 불일치로 병합 거부 */

	bip = bio_integrity(req->bio);		/* req의 첫 bio에서 PI payload 획득; NVMe PRCHK 설정 원본 */
	bip_next = bio_integrity(next->bio);	/* next request의 PI payload 획득 */
	/* BIPFlags: BIP_CHECK_APPTAG 등 NVMe PRCHK 비트와 대응되는
	 * 블록 계층 무결성 검사 플래그(추정). */
	if (bip->bip_flags != bip_next->bip_flags)
		return false;	/* 검사 플래그 조합이 다름 — 동일 정책이 아니므로 병합 거부 */

	/* app_tag가 다륾면 NVMe APP TAG 검사 정책이 달라 병합 불가. */
	if (bip->bip_flags & BIP_CHECK_APPTAG &&
	    bip->app_tag != bip_next->app_tag)
		return false;	/* App Tag 값 불일치 — 병합 거부 */

	/* integrity 세그먼트 수 합이 queue 한도를 초과하면 NVMe SGL/PRP
	 * 엔트리 한도를 넘을 수 있으므로 병합 불가(추정). */
	if (req->nr_integrity_segments + next->nr_integrity_segments >
	    q->limits.max_integrity_segments)
		return false;	/* 합산 세그먼트 수가 큐 한도 초과 — 병합 거부 */

	/* 두 request 사이에 PI 메타데이터의 논리적 연속성이 없으면 병합
	 * 불가; NVMe Ref Tag 연속성과 관련. */
	if (integrity_req_gap_back_merge(req, next->bio))
		return false;	/* 무결성 버퍼 사이 gap 존재 — 병합 거부 */

	return true;	/* 모든 호환성 검사 통과 — 병합 허용 */
}

/**
 * blk_integrity_merge_bio - request에 새로운 bio를 병합할 때 무결성 검사
 * @q:		request queue
 * @req:		기존 request
 * @bio:		병합 후보 bio
 */
/*
 * [한국어]
 * blk_integrity_merge_bio - 기존 request에 새 bio를 추가로 병합할 때
 *                           PI 설정이 호환되는지 검사
 *
 * @q: req/bio가 속한 request_queue. 세그먼트 한도 검사에 사용.
 * @req: bio를 흡수하려는 기존 request.
 * @bio: req에 병합하려는 새 bio.
 * @return: true면 병합 허용, false면 병합 거부.
 *
 * blk_integrity_merge_rq()와 동일한 원리를 (request, request) 쌍이 아니라
 * (request, bio) 쌍에 적용한 버전이다. bio-based 병합 경로에서
 * blk_mq_submit_bio()나 I/O 스케줄러가 새 bio를 기존 request에 붙이기 전에
 * 호출하며, 검사 순서(PI 유무 → bip_flags → app_tag → 세그먼트 한도)는
 * blk_integrity_merge_rq()와 동일하다. 세그먼트 수는 이미 계산되어 있는
 * req->nr_integrity_segments에 blk_rq_count_integrity_sg()로 새로 계산한
 * bio의 세그먼트 수를 더해 한도를 검사한다. 이 함수는 병합 판단 경로
 * (프로세스 컨텍스트)에서 호출되는 순수 판정 함수다.
 *
 * 호출 체인:
 *   ll_new_hw_segment() / blk_rq_merge_ok() (blk-merge.c)
 *   → [blk_integrity_merge_bio] → blk_rq_count_integrity_sg
 */
bool blk_integrity_merge_bio(struct request_queue *q, struct request *req,
			     struct bio *bio)
{
	struct bio_integrity_payload *bip, *bip_bio = bio_integrity(bio);	/* req/bio의 PI payload; NVMe PRCHK/PRACT 정책 비교용 */
	int nr_integrity_segs;							/* 추가 bio가 기여할 sg/PRP/SGL 엔트리 수 */

	/* 양쪽 모두 PI 없음: 일반 NVMe READ/WRITE로 통합 가능. */
	if (blk_integrity_rq(req) == 0 && bip_bio == NULL)
		return true;	/* 둘 다 PI 없음 — 무조건 병합 허용 */

	/* PI 정책 불일치: 동일 CID 명령 처리 불가. */
	if (blk_integrity_rq(req) == 0 || bip_bio == NULL)
		return false;	/* 한쪽만 PI 보유 — 병합 거부 */

	bip = bio_integrity(req->bio);		/* 기존 req의 PI 정책 획득 */
	if (bip->bip_flags != bip_bio->bip_flags)
		return false;	/* 검사 플래그 조합 불일치 — 병합 거부 */

	if (bip->bip_flags & BIP_CHECK_APPTAG &&
	    bip->app_tag != bip_bio->app_tag)
		return false;	/* App Tag 불일치 — 병합 거부 */

	/* 추가될 bio의 integrity sg 수를 계산하여 NVMe SGL/PRP 한도
	 * 초과 여부 판단(추정). */
	nr_integrity_segs = blk_rq_count_integrity_sg(q, bio);
	if (req->nr_integrity_segments + nr_integrity_segs >
	    q->limits.max_integrity_segments)
		return false;	/* 세그먼트 합이 한도 초과 — 병합 거부 */

	return true;	/* 모든 검사 통과 — 병합 허용 */
}

/*
 * [한국어]
 * dev_to_bi - sysfs device 객체로부터 대상 디스크의 blk_integrity
 *             프로파일 포인터를 역참조
 *
 * @dev: sysfs 콜백에 전달된 struct device(실제로는 gendisk에 내장된 것).
 * @return: dev가 속한 gendisk의 request_queue->limits.integrity 필드 주소.
 *      항상 유효한 포인터를 반환한다(struct blk_integrity 자체가
 *      queue_limits에 내장된 값 타입이라 NULL이 될 수 없다 — PI 미지원
 *      디스크는 필드 값이 전부 0인 상태로 존재한다).
 *
 * 이 파일 하단의 모든 sysfs show 콜백(format_show, tag_size_show 등)이
 * 공통으로 사용하는 조회 헬퍼다. dev_to_disk()로 struct device를
 * struct gendisk로 역캐스팅한 뒤 ->queue->limits.integrity의 주소를
 * 취한다. 이 함수는 sysfs read(2) 경로의 호출 프로세스 컨텍스트에서
 * 실행되며 락 없이 단순 포인터 산술만 수행한다.
 *
 * 호출 체인:
 *   format_show / tag_size_show / ... → [dev_to_bi] → dev_to_disk
 */
static inline struct blk_integrity *dev_to_bi(struct device *dev)
{
	return &dev_to_disk(dev)->queue->limits.integrity;	/* gendisk -> request_queue -> integrity limits 체인 */
}

/**
 * blk_integrity_profile_name - 무결성 checksum 프로파일의 문자열 이름 반환
 * @bi:		blk_integrity 구조체
 */
/*
 * [한국어]
 * blk_integrity_profile_name - csum_type/REF_TAG 조합을 표준 PI 프로파일
 *                              이름 문자열로 변환
 *
 * @bi: 이름을 조회할 대상 blk_integrity 프로파일.
 * @return: "T10-DIF-TYPE1-CRC"처럼 사람이 읽을 수 있는 정적 문자열 상수
 *      (호출자가 free할 필요 없음). csum_type이 BLK_INTEGRITY_CSUM_NONE이면
 *      "nop"을 반환해 "PI 미사용"을 나타낸다.
 *
 * format_show() sysfs 콜백이 /sys/block/<disk>/integrity/format 파일에
 * 그대로 노출할 이름을 만들어 주는 순수 변환 함수다. Type1/Type3 구분은
 * BLK_INTEGRITY_REF_TAG 플래그 유무로 결정하는데(Type1/Type2는 RefTag를
 * 쓰고 Type3는 쓰지 않는 T10 DIF 스펙 관례를 이 파일이 Type1로 단순화해
 * 표현), CRC64는 표준 T10-DIF 8바이트 tuple보다 확장된 tuple을 쓰므로
 * "EXT-DIF-" 접두사로 구분한다. 이 함수는 락이나 부수효과가 없는 순수
 * 함수로 재진입에 안전하다.
 *
 * 호출 체인:
 *   format_show → [blk_integrity_profile_name] → (없음, 리프 함수)
 */
const char *blk_integrity_profile_name(struct blk_integrity *bi)
{
	switch (bi->csum_type) {	/* Guard 알고리즘 종류별로 반환할 프로파일 이름이 완전히 다름 */
	case BLK_INTEGRITY_CSUM_IP:
		if (bi->flags & BLK_INTEGRITY_REF_TAG)	/* RefTag까지 사용하는 조합인지 확인 */
			return "T10-DIF-TYPE1-IP";	/* NVMe Type1 + IP checksum PI(추정) */
		return "T10-DIF-TYPE3-IP";		/* NVMe Type3 + IP checksum PI(추정) */
	case BLK_INTEGRITY_CSUM_CRC:
		if (bi->flags & BLK_INTEGRITY_REF_TAG)	/* RefTag까지 사용하는 조합인지 확인 */
			return "T10-DIF-TYPE1-CRC";	/* NVMe Type1 + CRC16 guard; PRCHK.Guard 활성 */
		return "T10-DIF-TYPE3-CRC";		/* NVMe Type3 + CRC16 guard */
	case BLK_INTEGRITY_CSUM_CRC64:
		/* NVMe PI-64(CRC64 guard)에 대응하는 확장 DIF 프로파일. */
		if (bi->flags & BLK_INTEGRITY_REF_TAG)	/* RefTag까지 사용하는 조합인지 확인 */
			return "EXT-DIF-TYPE1-CRC64";	/* NVMe Type1 + CRC64 guard(추정) */
		return "EXT-DIF-TYPE3-CRC64";		/* NVMe Type3 + CRC64 guard(추정) */
	case BLK_INTEGRITY_CSUM_NONE:
		break;	/* PI 미사용 — switch를 빠져나가 아래 공통 "nop" 반환으로 처리 */
	}

	return "nop";	/* NVMe PI 미사용 namespace; PRCHK/PRACT 무효 */
}
EXPORT_SYMBOL_GPL(blk_integrity_profile_name);	/* [한국어] sysfs format_show()와 드라이버/유틸리티가
						 * 모듈 경계 밖에서도 이 이름 변환 함수를 호출할 수 있도록 공개. */

/*
 * [한국어]
 * flag_store - sysfs write(2)로 받은 0/1 값을 반전(inverted)해 blk_integrity
 *              플래그 하나를 설정/해제하는 공통 저장 헬퍼
 *
 * @dev: sysfs kobject에 연결된 struct device(대상 디스크의 gendisk 내장 device).
 * @page: 사용자 공간이 write(2)한 문자열 버퍼("0" 또는 "1" 등 10진수 텍스트).
 * @count: page 버퍼의 바이트 수(sysfs write 관례상 store 콜백이 그대로
 *      반환해 "전부 소비했다"고 VFS에 알려야 함).
 * @flag: 이 호출에서 토글할 대상 비트(BLK_INTEGRITY_NOVERIFY 또는
 *      BLK_INTEGRITY_NOGENERATE) — read_verify_store/write_generate_store가
 *      각각 다른 flag 값으로 이 함수를 호출한다.
 * @return: 성공 시 count(요청한 바이트를 모두 소비했다는 sysfs 관례),
 *      실패 시 음수 errno — kstrtoul() 파싱 실패 시 그 반환값,
 *      queue_limits_commit_update_frozen() 실패 시 그 반환값(-EINVAL 등).
 *
 * read_verify/write_generate 두 sysfs 속성은 "이 값이 1이면 커널이 해당
 * 동작을 한다"는 사용자 친화적 의미를 갖지만, 내부 플래그(BLK_INTEGRITY_
 * NOVERIFY/NOGENERATE)는 "이 비트가 서 있으면 하지 않는다"는 반대 의미로
 * 정의되어 있다. 그래서 val이 1(사용자가 "검증/생성함"을 요청)이면
 * 내부적으로는 NO* 비트를 클리어하고, val이 0이면 반대로 NO* 비트를
 * 세운다 — 이 반전 관계가 이 함수의 핵심이다. 실제 갱신은 queue_limits
 * 원자적 갱신 프로토콜(queue_limits_start_update() → 필드 수정 →
 * queue_limits_commit_update_frozen())을 따르는데, 이는 다른 스레드가
 * 동시에 q->limits를 읽고 있는 동안 절반만 반영된 상태가 노출되는 것을
 * 막고, 큐를 freeze해 진행 중인 I/O와 새 PI 정책이 뒤섞이지 않게 하기
 * 위함이다. 이 함수는 sysfs write(2) 시스템 호출 경로의 프로세스
 * 컨텍스트에서 실행되며, queue_limits_start_update()가 q->limits_lock
 * 뮤텍스를 잡으므로 슬립 가능한 컨텍스트여야 한다.
 *
 * 호출 체인:
 *   read_verify_store / write_generate_store → [flag_store]
 *   → queue_limits_start_update → queue_limits_commit_update_frozen
 */
static ssize_t flag_store(struct device *dev, const char *page, size_t count,
		unsigned char flag)
{
	struct request_queue *q = dev_to_disk(dev)->queue;	/* NVMe namespace의 request_queue; limits.integrity 갱신 대상 */
	struct queue_limits lim;				/* 임시 queue limits; integrity.flags 외에도 다른 한도 보존 */
	unsigned long val;					/* sysfs에서 읽은 0/1 값; NVMe PI 소프트웨어 검증 정책 토글 */
	int err;						/* kstrtoul()/commit 단계의 오류 코드를 담아 그대로 반환 */

	err = kstrtoul(page, 10, &val);	/* 사용자 입력 문자열을 10진수 unsigned long으로 파싱 */
	if (err)
		return err;			/* 파싱 실패("abc" 등 숫자가 아닌 입력) — errno 그대로 전달 */

	/* note that the flags are inverted vs the values in the sysfs files */
	lim = queue_limits_start_update(q);		/* queue_limits 업데이트 시작; NVMe queue depth/segment 등 다른 한도는 그대로 */
	if (val)
		lim.integrity.flags &= ~flag;		/* 플래그 클리어: NVMe PI 커널 검증/생성 비활성화(추정) */
	else
		lim.integrity.flags |= flag;		/* 플래그 설정: NVMe PI 커널 검증/생성 활성화(추정) */

	err = queue_limits_commit_update_frozen(q, &lim);	/* 업데이트 커밋; NVMe I/O 경로에 새로운 PI 정책 반영(추정) */
	if (err)
		return err;		/* 커밋 실패(검증 오류 등) — errno 그대로 전달, q->limits는 변경되지 않음 */
	return count;			/* 성공 — sysfs 관례상 요청 바이트 수 전체를 소비했다고 보고 */
}

/*
 * [한국어]
 * flag_show - blk_integrity 플래그 하나를 반전해 0/1 문자열로 sysfs에 노출
 *
 * @dev: sysfs kobject에 연결된 struct device.
 * @page: 결과 문자열("0\n" 또는 "1\n")을 써 넣을 PAGE_SIZE 버퍼(sysfs read
 *      콜백 관례상 커널이 미리 할당해 전달).
 * @flag: 조회할 대상 비트(BLK_INTEGRITY_NOVERIFY 또는 BLK_INTEGRITY_NOGENERATE).
 * @return: sysfs_emit()이 반환하는, page에 기록된 바이트 수.
 *
 * flag_store()가 저장할 때 값을 반전시켰으므로(사용자 의미 ↔ 내부 NO*
 * 비트), 읽을 때도 대칭적으로 반전해야 사용자에게 "read_verify=1"처럼
 * 직관적인 의미로 보여줄 수 있다. 이 함수는 sysfs read(2) 경로의 프로세스
 * 컨텍스트에서 호출되며 락이 필요 없는 단순 조회 함수다.
 *
 * 호출 체인:
 *   read_verify_show / write_generate_show → [flag_show]
 *   → dev_to_bi → sysfs_emit
 */
static ssize_t flag_show(struct device *dev, char *page, unsigned char flag)
{
	struct blk_integrity *bi = dev_to_bi(dev);	/* 대상 디스크의 PI 프로파일 조회 */

	return sysfs_emit(page, "%d\n", !(bi->flags & flag));	/* NO* 비트가 꺼져 있으면(검증/생성 함) 1, 서 있으면 0 — 저장 시 반전과 대칭 */
}

/*
 * [한국어]
 * format_show - /sys/block/<disk>/integrity/format 속성의 read(2) 콜백
 *
 * @dev: 대상 디스크의 struct device.
 * @attr: 이 show 콜백이 연결된 device_attribute(DEVICE_ATTR_RO(format)로
 *      선언, 이 함수 본문에서는 사용하지 않음 — sysfs 콜백 시그니처 규약).
 * @page: 결과 문자열을 써 넣을 PAGE_SIZE 버퍼.
 * @return: sysfs_emit()이 반환하는, page에 기록된 바이트 수.
 *
 * 디스크가 PI 메타데이터를 전혀 갖지 않으면(metadata_size == 0) "none"을,
 * 그렇지 않으면 blk_integrity_profile_name()이 계산한 "T10-DIF-TYPE1-CRC"
 * 같은 표준 프로파일 이름을 한 줄로 출력한다. 이 함수는 sysfs read(2)
 * 경로의 프로세스 컨텍스트에서 호출되는 순수 조회 함수다.
 *
 * 호출 체인:
 *   sysfs read(2) → dev_attr_format.show → [format_show]
 *   → dev_to_bi → blk_integrity_profile_name
 */
static ssize_t format_show(struct device *dev, struct device_attribute *attr,
			   char *page)
{
	struct blk_integrity *bi = dev_to_bi(dev);	/* 대상 디스크의 PI 프로파일 조회 */

	if (!bi->metadata_size)
		return sysfs_emit(page, "none\n");	/* NVMe PI 미지원 또는 metadata_size=0 namespace */
	return sysfs_emit(page, "%s\n", blk_integrity_profile_name(bi));	/* csum_type/REF_TAG 조합을 표준 이름 문자열로 변환해 출력 */
}

/*
 * [한국어]
 * tag_size_show - /sys/block/<disk>/integrity/tag_size 속성의 read(2) 콜백
 *
 * @dev: 대상 디스크의 struct device.
 * @attr: sysfs 콜백 시그니처 규약상 전달되나 이 함수는 사용하지 않음.
 * @page: 결과 문자열을 써 넣을 PAGE_SIZE 버퍼.
 * @return: sysfs_emit()이 반환하는 바이트 수.
 *
 * blk_integrity->tag_size(스택 디바이스 구성 시 사용되는 태그 바이트 수)
 * 값을 그대로 10진수 문자열로 노출한다. 이 함수는 sysfs read(2) 경로의
 * 프로세스 컨텍스트에서 호출되는 순수 조회 함수다.
 *
 * 호출 체인:
 *   sysfs read(2) → dev_attr_tag_size.show → [tag_size_show] → dev_to_bi
 */
static ssize_t tag_size_show(struct device *dev, struct device_attribute *attr,
			     char *page)
{
	struct blk_integrity *bi = dev_to_bi(dev);	/* 대상 디스크의 PI 프로파일 조회 */

	return sysfs_emit(page, "%u\n", bi->tag_size);	/* tag_size 필드를 그대로 10진수로 출력 */
}

/*
 * [한국어]
 * protection_interval_bytes_show - /sys/block/<disk>/integrity/
 *                                  protection_interval_bytes 속성의 read(2) 콜백
 *
 * @dev: 대상 디스크의 struct device.
 * @attr: sysfs 콜백 시그니처 규약상 전달되나 이 함수는 사용하지 않음.
 * @page: 결과 문자열을 써 넣을 PAGE_SIZE 버퍼.
 * @return: sysfs_emit()이 반환하는 바이트 수.
 *
 * interval_exp는 "2의 지수" 형태로 압축 저장된 보호 구간 크기이므로,
 * 사용자 공간에는 1 << interval_exp로 환산한 실제 바이트 수를 노출한다.
 * interval_exp가 0이면(보호 구간이 정의되지 않은 디스크) 그대로 0을
 * 노출해 "PI 구간 없음"을 나타낸다. 이 함수는 sysfs read(2) 경로의
 * 프로세스 컨텍스트에서 호출되는 순수 조회 함수다.
 *
 * 호출 체인:
 *   sysfs read(2) → dev_attr_protection_interval_bytes.show
 *   → [protection_interval_bytes_show] → dev_to_bi
 */
static ssize_t protection_interval_bytes_show(struct device *dev,
					      struct device_attribute *attr,
					      char *page)
{
	struct blk_integrity *bi = dev_to_bi(dev);	/* 대상 디스크의 PI 프로파일 조회 */

	return sysfs_emit(page, "%u\n",
			  bi->interval_exp ? 1 << bi->interval_exp : 0);	/* interval_exp==0이면 "구간 없음"으로 0을 그대로 출력, 아니면 2^interval_exp 바이트로 환산 */
}

/*
 * [한국어]
 * read_verify_store - /sys/block/<disk>/integrity/read_verify 속성의
 *                     write(2) 콜백
 *
 * @dev: 대상 디스크의 struct device.
 * @attr: sysfs 콜백 시그니처 규약상 전달되나 이 함수는 사용하지 않음.
 * @page: 사용자 공간이 write(2)한 "0" 또는 "1" 문자열.
 * @count: page의 바이트 수.
 * @return: flag_store()의 반환값을 그대로 전달(성공 시 count, 실패 시 음수 errno).
 *
 * BLK_INTEGRITY_NOVERIFY 비트를 대상으로 flag_store() 공통 로직을 호출하는
 * 얇은 래퍼다. 사용자가 1을 쓰면(READ 시 검증함) NOVERIFY 비트가 클리어되고,
 * 0을 쓰면(검증 안 함) NOVERIFY 비트가 세워진다 — NVMe 컨트롤러가 이미
 * PRCHK로 하드웨어 검증을 수행하더라도, 이 값이 1이면 host가 추가로
 * 소프트웨어 검증을 수행한다(__bio_integrity_action() 참고).
 *
 * 호출 체인:
 *   sysfs write(2) → dev_attr_read_verify.store → [read_verify_store]
 *   → flag_store
 */
static ssize_t read_verify_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *page, size_t count)
{
	return flag_store(dev, page, count, BLK_INTEGRITY_NOVERIFY);	/* NOVERIFY 비트를 대상으로 반전 저장 위임 */
}

/*
 * [한국어]
 * read_verify_show - /sys/block/<disk>/integrity/read_verify 속성의
 *                    read(2) 콜백
 *
 * @dev: 대상 디스크의 struct device.
 * @attr: sysfs 콜백 시그니처 규약상 전달되나 이 함수는 사용하지 않음.
 * @page: 결과 문자열을 써 넣을 PAGE_SIZE 버퍼.
 * @return: flag_show()가 반환하는, page에 기록된 바이트 수.
 *
 * BLK_INTEGRITY_NOVERIFY 비트를 대상으로 flag_show() 공통 로직을 호출하는
 * 얇은 래퍼다.
 *
 * 호출 체인:
 *   sysfs read(2) → dev_attr_read_verify.show → [read_verify_show]
 *   → flag_show
 */
static ssize_t read_verify_show(struct device *dev,
				struct device_attribute *attr, char *page)
{
	return flag_show(dev, page, BLK_INTEGRITY_NOVERIFY);	/* NOVERIFY 비트를 대상으로 반전 조회 위임 */
}

/*
 * [한국어]
 * write_generate_store - /sys/block/<disk>/integrity/write_generate 속성의
 *                        write(2) 콜백
 *
 * @dev: 대상 디스크의 struct device.
 * @attr: sysfs 콜백 시그니처 규약상 전달되나 이 함수는 사용하지 않음.
 * @page: 사용자 공간이 write(2)한 "0" 또는 "1" 문자열.
 * @count: page의 바이트 수.
 * @return: flag_store()의 반환값을 그대로 전달.
 *
 * BLK_INTEGRITY_NOGENERATE 비트를 대상으로 flag_store() 공통 로직을
 * 호출하는 얇은 래퍼다. 사용자가 1을 쓰면(WRITE 시 생성함) NOGENERATE
 * 비트가 클리어되어 host가 Guard/App/Ref 태그를 소프트웨어로 채우고,
 * 0을 쓰면 host 생성을 끄고 NVMe 컨트롤러의 PRACT=1 하드웨어 생성에
 * 맡긴다(추정, __bio_integrity_action() 참고).
 *
 * 호출 체인:
 *   sysfs write(2) → dev_attr_write_generate.store → [write_generate_store]
 *   → flag_store
 */
static ssize_t write_generate_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *page, size_t count)
{
	return flag_store(dev, page, count, BLK_INTEGRITY_NOGENERATE);	/* NOGENERATE 비트를 대상으로 반전 저장 위임 */
}

/*
 * [한국어]
 * write_generate_show - /sys/block/<disk>/integrity/write_generate 속성의
 *                       read(2) 콜백
 *
 * @dev: 대상 디스크의 struct device.
 * @attr: sysfs 콜백 시그니처 규약상 전달되나 이 함수는 사용하지 않음.
 * @page: 결과 문자열을 써 넣을 PAGE_SIZE 버퍼.
 * @return: flag_show()가 반환하는 바이트 수.
 *
 * BLK_INTEGRITY_NOGENERATE 비트를 대상으로 flag_show() 공통 로직을
 * 호출하는 얇은 래퍼다.
 *
 * 호출 체인:
 *   sysfs read(2) → dev_attr_write_generate.show → [write_generate_show]
 *   → flag_show
 */
static ssize_t write_generate_show(struct device *dev,
				   struct device_attribute *attr, char *page)
{
	return flag_show(dev, page, BLK_INTEGRITY_NOGENERATE);	/* NOGENERATE 비트를 대상으로 반전 조회 위임 */
}

/*
 * [한국어]
 * device_is_integrity_capable_show - /sys/block/<disk>/integrity/
 *                                    device_is_integrity_capable 속성의
 *                                    read(2) 콜백
 *
 * @dev: 대상 디스크의 struct device.
 * @attr: sysfs 콜백 시그니처 규약상 전달되나 이 함수는 사용하지 않음.
 * @page: 결과 문자열("0\n"/"1\n")을 써 넣을 PAGE_SIZE 버퍼.
 * @return: sysfs_emit()이 반환하는 바이트 수.
 *
 * read_verify/write_generate와 달리 이 속성은 읽기 전용(RO)이며 반전되지
 * 않는다 — BLK_INTEGRITY_DEVICE_CAPABLE 비트가 그대로 "디바이스(컨트롤러)가
 * 하드웨어 수준에서 End-to-end Data Protection을 지원하는가"를 의미하므로,
 * 사용자가 끄고 켤 수 있는 정책이 아니라 하드웨어 능력을 있는 그대로
 * 보고하는 값이다. 이 함수는 sysfs read(2) 경로의 프로세스 컨텍스트에서
 * 호출되는 순수 조회 함수다.
 *
 * 호출 체인:
 *   sysfs read(2) → dev_attr_device_is_integrity_capable.show
 *   → [device_is_integrity_capable_show] → dev_to_bi
 */
static ssize_t device_is_integrity_capable_show(struct device *dev,
						struct device_attribute *attr,
						char *page)
{
	struct blk_integrity *bi = dev_to_bi(dev);	/* 대상 디스크의 PI 프로파일 조회 */

	return sysfs_emit(page, "%u\n",
			  !!(bi->flags & BLK_INTEGRITY_DEVICE_CAPABLE));	/* DEVICE_CAPABLE 비트를 0/1 불리언으로 정규화해 출력 */
}

static DEVICE_ATTR_RO(format);		/* [한국어] /sys/block/<disk>/integrity/format 속성 정의 — 읽기 전용,
					 * format_show()를 .show 콜백으로 등록. PI 프로파일 문자열 노출. */
static DEVICE_ATTR_RO(tag_size);	/* [한국어] /sys/block/<disk>/integrity/tag_size 속성 정의 — 읽기 전용,
					 * tag_size_show()를 .show 콜백으로 등록. */
static DEVICE_ATTR_RO(protection_interval_bytes);	/* [한국어] /sys/block/<disk>/integrity/protection_interval_bytes
							 * 속성 정의 — 읽기 전용, protection_interval_bytes_show() 등록. */
static DEVICE_ATTR_RW(read_verify);	/* [한국어] /sys/block/<disk>/integrity/read_verify 속성 정의 — 읽기/쓰기,
					 * read_verify_show()/read_verify_store()를 각각 등록. */
static DEVICE_ATTR_RW(write_generate);	/* [한국어] /sys/block/<disk>/integrity/write_generate 속성 정의 — 읽기/쓰기,
					 * write_generate_show()/write_generate_store()를 각각 등록. */
static DEVICE_ATTR_RO(device_is_integrity_capable);	/* [한국어] /sys/block/<disk>/integrity/device_is_integrity_capable
							 * 속성 정의 — 읽기 전용, device_is_integrity_capable_show() 등록. */

/* integrity 관련 sysfs 속성 그룹; 대상 디스크(예: NVMe namespace) block
 * device의 /sys/block/<disk>/integrity/ 아래 항목으로 노출. */
static struct attribute *integrity_attrs[] = {
	&dev_attr_format.attr,				/* format 속성의 kobject attribute 핸들 등록 */
	&dev_attr_tag_size.attr,			/* tag_size 속성의 kobject attribute 핸들 등록 */
	&dev_attr_protection_interval_bytes.attr,	/* protection_interval_bytes 속성의 kobject attribute 핸들 등록 */
	&dev_attr_read_verify.attr,			/* read_verify 속성의 kobject attribute 핸들 등록 */
	&dev_attr_write_generate.attr,			/* write_generate 속성의 kobject attribute 핸들 등록 */
	&dev_attr_device_is_integrity_capable.attr,	/* device_is_integrity_capable 속성의 kobject attribute 핸들 등록 */
	NULL						/* sysfs 관례상 배열의 끝을 표시하는 sentinel */
};

const struct attribute_group blk_integrity_attr_group = {
	.name = "integrity",		/* sysfs 디렉터리 이름; 대상 디스크별 PI 노출 경로 — /sys/block/<disk>/integrity/ */
	.attrs = integrity_attrs,	/* 위에서 정의한 PI sysfs 속성 배열 */
};
