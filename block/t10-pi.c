// SPDX-License-Identifier: GPL-2.0
/*
 * t10_pi.c - Functions for generating and verifying T10 Protection
 *	      Information.
 */
/*
 * [한국어 설명] T10 PI(Protection Information) Guard/AppTag/RefTag 생성과
 * 검증을 수행하는 핵심 계산 엔진 (t10_pi.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SCSI DIF(Data Integrity Field)/NVMe DIX(Data Integrity
 * Extension) 계열의 T10 PI(Protection Information) tuple을 바이트 단위로
 * 생성하고 검증하는 실질적인 계산 엔진이다. bio 하나의 데이터 버퍼를 논리
 * 블록(보호 interval) 단위로 순회하면서, 각 interval에 대해 Guard(체크섬:
 * csum_type이 지정하는 CRC16-T10DIF/CRC64-NVME/IP 체크섬 중 하나),
 * Application Tag(불투명 태그, escape 값으로 검사 생략을 지시), Reference
 * Tag(대상 LBA 하위 비트, 위치 오검출을 잡아내는 용도)를 계산해서 쓰거나
 * (WRITE 경로), 저장된 값과 비교한다(READ 완료 경로). block/bio-integrity.c와
 * block/blk-integrity.c는 각각 bio_integrity_payload(bip) 컨테이너의 수명
 * 관리와 request_queue 단위 blk_integrity 프로파일의 등록·병합 호환성
 * 검사를 담당할 뿐이고, 실제로 데이터 바이트를 훑어 체크섬을 계산하는
 * 코드는 이 파일에만 있다. 또한 이 파일은 request가 파티션/dm-linear 같은
 * 스택 디바이스를 거치며 Reference Tag가 가리키는 LBA 기준이 달라질 때,
 * 그 태그를 가상 주소<->물리 주소 사이로 재사상(remap)하는 별도 기능도
 * 함께 구현한다. 이 파일이 없으면 PRACT=0(호스트가 PI를 직접 관리)으로
 * 설정된 NVMe 네임스페이스나 소프트웨어 DIF를 쓰는 SCSI 장치에서, 데이터가
 * 매체에 쓰이기 전/후로 무결성 태그를 실제로 계산하거나 검증할 방법이
 * 전혀 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일의 함수들은 서로 독립적인 두 흐름에서 호출된다.
 *
 * (1) Guard/AppTag/RefTag 생성·검증 흐름 (체크섬 계산 본연의 작업):
 *   파일시스템이 스스로 PI 버퍼를 준비하는 경로 (block/bio-integrity-fs.c):
 *     fs_bio_integrity_generate() -> bio_integrity_generate() [본 파일]
 *       -> blk_integrity_iterate() -> blk_integrity_interval()
 *       -> blk_integrity_set() -> blk_set_*_pi()
 *     fs_bio_integrity_verify() -> bio_integrity_verify() [본 파일]
 *       -> blk_integrity_iterate() -> blk_integrity_interval()
 *       -> blk_integrity_verify() -> blk_verify_*_pi()
 *   파일시스템이 PI를 준비하지 않아 블록 계층이 대신 처리하는 경로
 *   (block/bio-integrity-auto.c):
 *     bio_integrity_prep()(WRITE) -> bio_integrity_generate() [본 파일]
 *     bio_integrity_verify_fn()(kintegrityd workqueue, READ 완료 후 지연
 *     실행) -> bio_integrity_verify() [본 파일]
 *   실행 컨텍스트: WRITE 생성은 제출자의 프로세스 컨텍스트, READ 검증은
 *   kintegrityd 워커 스레드의 프로세스 컨텍스트(인터럽트 컨텍스트에서 CRC
 *   계산 같은 무거운 연산을 피하기 위함)에서 실행된다.
 *
 * (2) Reference Tag remap 흐름 (체크섬과 무관, LBA 재사상만 수행):
 *     blk_mq_start_request() (block/blk-mq.c, WRITE 제출 직전)
 *       -> blk_integrity_prepare() [본 파일] -> blk_integrity_remap(prep=true)
 *       -> __blk_reftag_remap() -> blk_reftag_remap_prepare()
 *     blk_update_request()/blk_complete_request() (block/blk-mq.c, READ 완료)
 *       -> blk_integrity_complete() [본 파일] -> blk_integrity_remap(prep=false)
 *       -> __blk_reftag_remap() -> blk_reftag_remap_complete()
 *   실행 컨텍스트: prepare는 제출 경로의 프로세스 컨텍스트, complete는
 *   드라이버 완료 인터럽트/softirq 컨텍스트(예: NVMe MSI-X ISR 이후)에서
 *   실행된다. 이 두 함수는 request에 속한 모든 bio를 순회하며 이미 쓰인
 *   PI tuple의 ref_tag 필드만 고쳐 쓸 뿐 guard/csum은 건드리지 않는다 —
 *   파티션/dm 스택을 거치며 원래 제출자가 알던 LBA(virt)와 디바이스가
 *   보는 물리 LBA(ref)가 달라지는 상황을 보정하기 위함이다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - include/linux/t10-pi.h : struct t10_pi_tuple(guard_tag/app_tag/ref_tag,
 *     8바이트, T10 PI Type 1/2/3), struct crc64_pi_tuple(guard_tag 8B +
 *     app_tag 2B + ref_tag 6B = 16B, NVMe CRC64 확장 PI 형식),
 *     lower_48_bits() 헬퍼가 정의되어 있다.
 *   - include/linux/blk-integrity.h : struct blk_integrity(flags/csum_type/
 *     metadata_size/pi_offset/interval_exp/tag_size/pi_tuple_size),
 *     enum blk_integrity_flags(BLK_INTEGRITY_REF_TAG 등), blk_get_integrity()
 *     선언.
 *   - include/linux/bio-integrity.h : struct bio_integrity_payload(bip_iter/
 *     bip_vec/bip_flags/app_tag), enum bip_flags(BIP_MAPPED_INTEGRITY 등),
 *     bio_integrity(), bip_get_seed() 선언.
 *   - include/linux/crc-t10dif.h, include/linux/crc64.h : 실제 CRC16-T10DIF,
 *     CRC64-NVME 다항식 계산 루틴(crc_t10dif_update(), crc64_nvme()) 제공.
 *   - net/checksum.h : IP 체크섬 계열(csum_partial()/csum_fold()) —
 *     csum_type이 BLK_INTEGRITY_CSUM_IP인 레거시 경로에서 사용.
 *   - include/linux/unaligned.h : 빅엔디안 비정렬 접근 헬퍼
 *     (get/put_unaligned_beNN, get/put_unaligned_be48) — PI tuple은 네트워크
 *     바이트오더(빅엔디안)이고 구조체 경계에 정렬 보장이 없으므로 반드시
 *     이 헬퍼를 통해서만 접근해야 한다.
 *   - block/blk.h : 이 서브시스템 내부에서만 공유하는 비공개 선언
 *     (bio_integrity_generate/verify, blk_integrity_prepare/complete의 원형).
 *
 * 이 파일에 의존하는 모듈:
 *   - block/bio-integrity-auto.c : bio_integrity_prep()이 WRITE에서
 *     bio_integrity_generate()를, bio_integrity_verify_fn()이 READ 완료
 *     후 bio_integrity_verify()를 호출한다.
 *   - block/bio-integrity-fs.c : 파일시스템이 직접 준비한 PI 버퍼에 대해
 *     동일하게 bio_integrity_generate()/verify()를 호출한다.
 *   - block/blk-mq.c : blk_mq_start_request()/blk_update_request()/
 *     blk_complete_request()가 blk_integrity_prepare()/blk_integrity_complete()
 *     를 호출해 ref tag remap을 수행한다.
 *
 * 데이터 흐름: 사용자 데이터(bio->bi_io_vec) + 무결성 메타데이터 버퍼
 * (bip->bip_vec)를 나란히 순회하며, 데이터 쪽에서 계산한 체크섬(csum)을
 * 메타데이터 쪽 tuple에 쓰거나(WRITE), tuple에서 읽은 값과 비교한다(READ).
 * 두 버퍼 모두 kmap_local()로 임시 매핑해 접근한 뒤 즉시 매핑을 해제하므로,
 * 이 파일 자체는 별도의 영속적 상태를 갖지 않으며 모든 상태는 스택 위의
 * struct blk_integrity_iter/union pi_tuple에만 머문다.
 *
 * 공유 핵심 자료구조:
 *   struct blk_integrity_iter : 이 파일 안에서만 쓰이는 반복자 — 데이터/
 *     메타데이터 bvec_iter, 현재 interval에서 누적 중인 csum, ref tag로
 *     쓰일 seed를 한데 묶는다.
 *   union pi_tuple : t10_pi_tuple 또는 crc64_pi_tuple을 담는 스택 스크래치
 *     공간 — PI tuple이 두 개의 bio_vec에 걸쳐 있어 한 번에 매핑할 수 없을
 *     때 임시 복사본으로 쓰인다.
 *
 * === 주요 함수/구조체 요약 ===
 * blk_calculate_guard()    : csum_type에 따라 CRC64/CRC16-T10DIF/IP 체크섬을
 *                            누적 계산 — 이 파일의 가장 저수준 산술 함수.
 * blk_integrity_iterate()  : bio의 데이터 bvec을 interval 단위로 순회하며
 *                            매 interval마다 blk_integrity_interval()을 호출.
 * blk_integrity_interval() : 한 interval의 PI tuple을 매핑하고 생성
 *                            (blk_integrity_set) 또는 검증
 *                            (blk_integrity_verify)을 위임.
 * bio_integrity_generate()/bio_integrity_verify() : 이 파일의 공개 진입점 —
 *                            csum_type이 유효할 때만 blk_integrity_iterate()를
 *                            호출.
 * __blk_reftag_remap()/blk_integrity_remap() : request의 모든 bio에 대해
 *                            ref tag를 가상<->물리 LBA 사이로 재사상.
 * blk_integrity_prepare()/blk_integrity_complete() : blk-mq.c가 호출하는
 *                            공개 진입점 — 실제 작업은 blk_integrity_remap()에
 *                            위임.
 */

/* [한국어] struct t10_pi_tuple(guard_tag/app_tag/ref_tag 8바이트)와
 * struct crc64_pi_tuple(16바이트), lower_48_bits() 등 PI tuple의 온디스크
 * 레이아웃 정의 - 이 파일 전역에서 guard/app/ref 필드 오프셋을 해석하는
 * 근거가 된다 */
#include <linux/t10-pi.h>
/* [한국어] struct blk_integrity(csum_type/flags/interval_exp/pi_offset/
 * metadata_size/pi_tuple_size), blk_get_integrity() 선언 - 디스크 단위
 * 무결성 프로파일을 조회하기 위해 필요 */
#include <linux/blk-integrity.h>
/* [한국어] crc_t10dif_update() 선언 - 전통적 T10 DIF CRC16(다항식 0x8BB7)
 * guard 계산에 사용 */
#include <linux/crc-t10dif.h>
/* [한국어] crc64_nvme() 선언 - NVMe CRC64 확장 PI(csum_type==CRC64) guard
 * 계산에 사용 */
#include <linux/crc64.h>
/* [한국어] csum_partial()/csum_fold() 선언 - csum_type==IP(1의 보수 IP
 * 체크섬) 경로에서 guard 계산에 사용. 네트워크 체크섬 코드를 재사용 */
#include <net/checksum.h>
/* [한국어] get/put_unaligned_beNN(), get/put_unaligned_be48() 등 빅엔디안
 * 비정렬 접근 헬퍼 - PI tuple 필드들이 패킹된 빅엔디안 구조체라 컴파일러가
 * 정렬을 보장하지 않으므로, 이 헬퍼 없이 직접 역참조하면 일부 아키텍처에서
 * 버스 에러/성능 저하가 발생할 수 있다 */
#include <linux/unaligned.h>
/* [한국어] 이 블록 계층 서브시스템 내부에서만 공유하는 비공개 선언 헤더 -
 * bio_integrity_generate()/verify(), blk_integrity_prepare()/complete()의
 * 원형이 여기 선언되어 있어 block/blk-mq.c 등 다른 .c 파일이 이 심볼들을
 * 쓸 수 있게 한다 */
#include "blk.h"

/* [한국어] APP_TAG_ESCAPE: Application Tag 필드가 이 값(전 비트 1)이면
 * 해당 interval에 대한 PI 검사를 생략하라는 T10 PI 표준의 "escape" 관례.
 * blk_verify_ext_pi()/blk_verify_pi()가 app 필드를 이 값과 비교해 조기에
 * BLK_STS_OK를 반환하는 데 쓰인다. (NVMe PRCHK[AppTag] 우회와 연동되는
 * 값이다) */
#define APP_TAG_ESCAPE 0xffff
/* [한국어] REF_TAG_ESCAPE: 표준 4바이트 Reference Tag(t10_pi_tuple)의
 * escape 값(전 비트 1) - BLK_INTEGRITY_REF_TAG 플래그가 꺼져 있는 PI
 * Type(예: Type 3)에서 app/ref 모두 escape이면 guard까지 통째로 검사를
 * 생략하는 조건에 쓰인다. crc64_pi_tuple의 48비트 ref tag는 별도로
 * ext_pi_ref_escape()가 담당한다 */
#define REF_TAG_ESCAPE 0xffffffff

/*
 * This union is used for onstack allocations when the pi field is split across
 * segments. blk_validate_integrity_limits() guarantees pi_tuple_size matches
 * the sizeof one of these two types.
 */
/*
 * [한국어] NVMe/SCSI PI tuple의 두 가지 온디스크 표현을 겹쳐 담는 union.
 * PI tuple이 한 bio_vec 페이지 경계를 넘어 쪼개져 있어 kmap_local()로
 * 한 번에 매핑할 수 없을 때, 이 union 크기만큼 스택 스크래치 버퍼를 만들어
 * blk_integrity_copy_to_tuple()/blk_integrity_copy_from_tuple()로 복사해
 * 온전한 tuple을 조립한다. 어느 멤버를 쓸지는 iter->bi->csum_type(또는
 * bi->csum_type)이 결정하며, blk_validate_integrity_limits()가 이미
 * pi_tuple_size와 두 구조체 중 하나의 sizeof가 정확히 일치함을 보장하므로
 * 여기서는 크기 검사를 다시 하지 않는다.
 */
union pi_tuple {
	struct crc64_pi_tuple	crc64_pi;
	/* NVMe CRC64 확장 PI 뷰 (guard_tag 8B + app_tag 2B + ref_tag 6B = 16B).
	 * 설정자: csum_type==BLK_INTEGRITY_CSUM_CRC64일 때 blk_set_ext_pi()가
	 *   guard/app/ref를 채우고, blk_set_ext_map_ref()/blk_set_ext_unmap_ref()가
	 *   remap 시 ref_tag만 고쳐 쓴다.
	 * 읽는 자: blk_verify_ext_pi()가 검증 시 세 필드를 읽고,
	 *   blk_reftag_remap_prepare/complete()가 ref_tag를 읽어 비교한다.
	 * 값 범위: guard_tag는 64비트 CRC 전체, app_tag는 보통 0 또는
	 *   APP_TAG_ESCAPE, ref_tag는 48비트 LBA 하위 비트(lower_48_bits).
	 * 동기화: 스택 지역 변수(또는 kmap된 단일 bio_vec)라 별도 락 불필요 -
	 *   한 interval은 항상 단일 스레드가 순서대로 처리. */
	struct t10_pi_tuple	t10_pi;
	/* 표준 T10 DIF PI 뷰 (guard_tag 2B + app_tag 2B + ref_tag 4B = 8B).
	 * 설정자: csum_type==CRC(전통 CRC16) 또는 IP일 때 blk_set_t10_pi()/
	 *   blk_set_ip_pi()(내부적으로 공통 blk_set_pi())가 채우고,
	 *   blk_set_t10_map_ref()/blk_set_t10_unmap_ref()가 remap 시 ref_tag만
	 *   고쳐 쓴다.
	 * 읽는 자: blk_verify_t10_pi()/blk_verify_ip_pi()(공통 blk_verify_pi())가
	 *   검증 시 읽고, blk_reftag_remap_prepare/complete()가 ref_tag를 읽는다.
	 * 값 범위: guard_tag는 CRC16 결과 또는 IP 체크섬 16비트, app_tag는
	 *   보통 0 또는 APP_TAG_ESCAPE, ref_tag는 32비트 LBA 하위 비트 또는
	 *   REF_TAG_ESCAPE.
	 * 동기화: crc64_pi와 동일 - 단일 스레드 순차 처리. */
};

/*
 * [한국어]
 * T10 PI 처리 반복자 - bio의 데이터 영역과 무결성 메타데이터 영역을 동시에
 * 순회하면서, 현재 interval에 대해 계산 중인 체크섬과 다음에 쓸 Reference
 * Tag(seed)를 함께 들고 다니는 스택 지역 구조체. blk_integrity_iterate()가
 * 스택에 생성해 blk_integrity_interval()/blk_calculate_guard() 등 이 파일의
 * 거의 모든 하위 헬퍼에 포인터로 전달하는 "작업 컨텍스트"이며, 힙에 할당되지
 * 않고 함수 호출 스택에만 존재하므로 별도의 락이나 참조 카운트가 없다.
 */
struct blk_integrity_iter {
	struct bio			*bio;
	/* 처리 중인 원본 bio - 실제 순회는 iter.data_iter/iter.prot_iter가
	 * 담당하므로 이 필드 자체는 순회에 쓰이지 않고, 에러 메시지에서
	 * bio->bi_bdev->bd_disk->disk_name(디스크 이름)을 뽑아내는 용도로만
	 * 참조된다.
	 * 설정자: blk_integrity_iterate()가 최초 1회 대입.
	 * 읽는 자: blk_verify_ext_pi()/blk_verify_pi()가 pr_err() 에러 로그에
	 *   디스크 이름을 넣을 때.
	 * 값 범위: 유효한 bio 포인터, NULL 불가.
	 * 동기화: 한 bio는 한 시점에 한 스레드(제출자 또는 kintegrityd 워커)만
	 *   순회하므로 락 불필요. */
	struct bio_integrity_payload	*bip;
	/* 이 bio에 붙은 무결성 메타데이터(PI) payload - 실제 guard/app/ref
	 * tuple이 저장된 bio_vec 배열(bip->bip_vec)의 소유자다.
	 * 설정자: blk_integrity_iterate()가 bio_integrity(bio)로 조회해 대입.
	 * 읽는 자: blk_integrity_csum_offset()/blk_integrity_copy_from_tuple()/
	 *   blk_integrity_copy_to_tuple()/blk_integrity_interval()이 bip->bip_vec을
	 *   직접 참조해 PI 버퍼 페이지를 kmap한다.
	 * 값 범위: bio_integrity(bio)가 REQ_INTEGRITY 플래그가 선 bio에 대해
	 *   반환하는 유효 포인터(이 경로가 호출될 때는 항상 non-NULL로 보장됨,
	 *   호출자가 csum_type 유효성으로 이미 필터링한다).
	 * 동기화: bio 단위로 소유되며 별도 락 없음. */
	struct blk_integrity		*bi;
	/* 디스크 단위 무결성 프로파일 - csum_type, interval_exp, pi_offset,
	 * metadata_size, pi_tuple_size, flags(BLK_INTEGRITY_REF_TAG 등)를 담아
	 * 이 파일의 사실상 모든 분기 조건(csum_type switch, REF_TAG 검사 여부)의
	 * 근거가 된다.
	 * 설정자: blk_integrity_iterate()가 blk_get_integrity(disk)로 조회해 대입.
	 * 읽는 자: blk_calculate_guard(), blk_integrity_csum_finish(),
	 *   blk_verify_*_pi(), blk_integrity_set/verify() 등 사실상 전체.
	 * 값 범위: request_queue->limits.integrity의 주소 - 디스크가 등록되어
	 *   있는 한(gendisk 수명 동안) 유효.
	 * 동기화: 읽기 전용 참조만 하며, 프로파일 자체의 변경(sysfs를 통한
	 *   flag_store 등)은 blk-integrity.c가 큐를 freeze한 뒤 갱신하므로
	 *   이 경로와 경쟁하지 않는다. */
	struct bvec_iter		data_iter;
	/* 사용자 데이터(LBA 데이터) 영역을 가리키는 bvec 반복자 -
	 * bio_integrity_generate()/verify()가 넘겨준 원본 iterator(bio->bi_iter
	 * 또는 saved_iter)의 복사본으로 시작해, 매 bvec 조각을 소비하며 전진한다.
	 * 설정자: blk_integrity_iterate()가 *data_iter로 초기화.
	 * 갱신자: blk_integrity_iterate() 루프가 bvec_iter_bvec()으로 현재
	 *   조각을 읽고 bvec_iter_advance_single()로 전진시킨다.
	 * 값 범위: bi_size(남은 바이트 수)가 0이 되면 순회 종료 조건.
	 * 동기화: 스택 지역 상태, 단일 스레드 전용. */
	struct bvec_iter		prot_iter;
	/* PI 메타데이터 버퍼(bip->bip_vec) 안에서의 진행 위치 - data_iter와는
	 * 별도의 속도로 전진한다(메타데이터는 interval마다 pi_tuple_size바이트,
	 * 데이터는 interval_exp 크기만큼 소비되므로 서로 다른 리듬으로 전진).
	 * 설정자: blk_integrity_iterate()가 bip->bip_iter로 초기화.
	 * 갱신자: blk_integrity_csum_offset()(pi_offset 만큼 padding skip),
	 *   blk_integrity_interval()(한 interval의 metadata_size만큼 전진).
	 * 값 범위: bi_size가 0이 되면 더 이상 처리할 PI tuple이 없음을 의미.
	 * 동기화: 스택 지역 상태, 단일 스레드 전용. */
	unsigned int			interval_remaining;
	/* 현재 interval에서 아직 체크섬 계산에 반영하지 못한 데이터 바이트 수 -
	 * 매 interval 시작 시 1 << bi->interval_exp(즉 보호 interval 크기,
	 * 보통 512B 또는 4096B)로 리셋되고, blk_integrity_iterate()가 데이터를
	 * 소비할 때마다 감소하다가 0이 되면 그 interval의 PI tuple 처리
	 * (blk_integrity_interval)가 트리거된다.
	 * 설정자: 최초 초기화는 blk_integrity_iterate(), 매 interval 종료 후
	 *   리셋은 blk_integrity_interval().
	 * 갱신자: blk_integrity_iterate() 내부 루프의 감산(-=).
	 * 값 범위: 0 ~ (1 << interval_exp). 0이 되는 순간이 interval 경계. */
	u64				seed;
	/* 다음에 기록/검증할 Reference Tag의 기준값 - 최초에는 bio의 시작
	 * 섹터(data_iter->bi_sector)이며, interval 하나를 처리할 때마다 1씩
	 * 증가한다(연속된 논리 블록은 LBA가 1씩 증가하므로).
	 * 설정자: 초기화는 blk_integrity_iterate()(.seed = data_iter->bi_sector),
	 *   증가는 blk_integrity_interval()의 iter->seed++.
	 * 읽는 자: blk_verify_ext_pi()가 lower_48_bits()로, blk_verify_pi()가
	 *   lower_32_bits()로 잘라 tuple의 ref_tag와 비교하고, blk_set_ext_pi()/
	 *   blk_set_pi()가 그대로(또는 잘라서) ref_tag 필드에 기록한다.
	 * 값 범위: 섹터 번호 기반이므로 논리적으로 LBA 범위 내 - u64로 저장해
	 *   48비트/32비트 ref tag보다 넓은 폭을 안전하게 수용. */
	u64				csum;
	/* 현재 interval에 대해 누적 중인 체크섬(guard 후보값) - blk_calculate_guard()가
	 * 매 데이터 조각마다 갱신하고, interval 경계에서 blk_integrity_verify()/
	 * blk_integrity_set()이 최종값을 tuple의 guard_tag와 비교하거나 그
	 * 필드에 기록한 뒤 blk_integrity_interval()이 0으로 리셋한다.
	 * 설정자/갱신자: blk_calculate_guard(), blk_integrity_csum_finish()
	 *   (IP 체크섬의 최종 1의 보수 fold), blk_integrity_interval()(리셋).
	 * 읽는 자: blk_verify_ext_pi()/blk_verify_pi()/blk_set_ext_pi()/
	 *   blk_set_pi().
	 * 값 범위: csum_type에 따라 유효 비트 폭이 다르다 - CRC64는 64비트
	 *   전체가 유의미, CRC(T10 DIF)/IP는 하위 16비트만 유의미하며 나머지
	 *   상위 비트는 0으로 유지된다. */
};

/*
 * [한국어]
 * blk_calculate_guard - 데이터 조각 하나에 대해 Guard(체크섬) 후보값을 누적 계산
 *
 * @iter: 현재 처리 중인 T10 PI 반복자. iter->bi->csum_type으로 알고리즘을
 *        고르고 iter->csum에 누적 결과를 저장하는 입출력 겸용 파라미터.
 * @data: 체크섬을 계산할 데이터의 커널 가상 주소(bvec_kmap_local()로 매핑된
 *        페이지 내부 주소).
 * @len: data에서 이번에 반영할 바이트 수 - 한 bvec 조각과 interval의 남은
 *       바이트 수 중 작은 쪽으로, 호출자가 미리 min()으로 계산해 넘긴다.
 * @return: 없음(void) - 결과는 iter->csum에 누적됨.
 *
 * T10 PI Guard 필드는 컨트롤러/드라이버가 매체 손상을 검출하도록 데이터 위에
 * 얹는 체크섬이다. 이 함수는 디스크 프로파일(csum_type)이 지정한 알고리즘 —
 * NVMe CRC64(64비트), 전통적 T10 DIF CRC16(crc_t10dif, 다항식 0x8BB7),
 * 또는 IP 체크섬(1의 보수 합) — 중 하나로 데이터를 훑어 누적 체크섬을
 * 갱신한다. 한 interval(예: 512바이트 논리 블록)은 여러 bio_vec 조각에
 * 걸쳐 있을 수 있으므로, 이 함수는 한 번에 전체 interval이 아니라 임의
 * 길이의 부분 데이터에 대해 "누적(incremental)" 방식으로 동작한다 - 즉
 * 여러 번 호출한 결과가 한 번에 전체를 계산한 것과 같아야 한다(선택된
 * 세 알고리즘 모두 이런 결합 성질을 만족). 알 수 없는 csum_type이 프로파일에
 * 들어있다면 이는 상위 계층(blk_validate_integrity_limits() 등)의 설정
 * 오류이므로 WARN_ON_ONCE로 커널 로그에 스택트레이스를 남기고, 이후 guard
 * 비교가 항상 실패하도록 csum을 U64_MAX(전 비트 1)로 강제해 잘못된
 * 프로파일로 데이터가 조용히 통과되는 것을 막는다.
 * 실행 컨텍스트: 호출자(blk_integrity_iterate()/blk_integrity_csum_offset())와
 * 동일한 컨텍스트(제출자 프로세스 또는 kintegrityd 워커)에서 실행되며, iter는
 * 스택 지역 상태이므로 별도 동기화가 필요 없다.
 * 호출자(caller): blk_integrity_iterate()(interval 안의 각 데이터 조각마다),
 * blk_integrity_csum_offset()(metadata padding 영역도 체크섬에 포함해야 하는
 * 포맷에서).
 * 호출되는 함수(callee): crc64_nvme(), crc_t10dif_update(), csum_partial().
 * 에러 처리: 알 수 없는 csum_type은 복구 불가능한 설정 오류로 간주해
 * WARN_ON_ONCE + csum 오염(U64_MAX)으로 처리하며, 반환형이 void이므로 상위
 * 호출자에게 별도 에러 코드를 전달하지 않는다 - 결국 뒤이은 guard 비교
 * 단계에서 항상 불일치로 검출된다.
 *
 * 호출 체인:
 *   blk_integrity_iterate / blk_integrity_csum_offset -> [blk_calculate_guard]
 *   -> crc64_nvme() / crc_t10dif_update() / csum_partial()
 */
static void blk_calculate_guard(struct blk_integrity_iter *iter, void *data,
				unsigned int len)
{
	/* [한국어] 디스크 무결성 프로파일의 csum_type(CRC64/CRC/IP)에 따라
	 * 서로 다른 체크섬 알고리즘으로 분기 - 프로파일은 네임스페이스/디스크
	 * 포맷 시점에 고정되므로 매 호출마다 동일한 case로 진입한다 */
	switch (iter->bi->csum_type) {
	/* [한국어] NVMe CRC64 확장 PI 형식(csum_type==CRC64) 분기 */
	case BLK_INTEGRITY_CSUM_CRC64:
		/* [한국어] iter->csum(이전까지의 누적값, 최초 호출 시 0)을 seed로
		 * 이어 len 바이트만큼 CRC64-NVME를 계산한 새 누적값을 저장 -
		 * crc64_nvme()는 NVM Command Set 스펙이 정의한 반전(inversion)까지
		 * 포함해 계산한다 */
		iter->csum = crc64_nvme(iter->csum, data, len);
		break;
		/* [한국어] CRC64 분기 종료 - default로 폴스루(fall-through)되지
		 * 않도록 break */
	/* [한국어] 전통적 T10 DIF CRC16 형식(csum_type==CRC) 분기 */
	case BLK_INTEGRITY_CSUM_CRC:
		/* [한국어] crc_t10dif_update()가 이전 누적값(iter->csum의 하위
		 * 16비트가 유의미)을 이어받아 계산 - 반환형은 u16이지만
		 * iter->csum(u64)에 대입되며 상위 비트는 자동으로 0으로 채워짐 */
		iter->csum = crc_t10dif_update(iter->csum, data, len);
		break;
		/* [한국어] CRC(T10 DIF) 분기 종료 */
	/* [한국어] IP(인터넷) 체크섬 형식(csum_type==IP) 분기 - 레거시/SCSI
	 * SCSI DIF의 IP 체크섬 변형과의 호환을 위해 유지된다 */
	case BLK_INTEGRITY_CSUM_IP:
		/* [한국어] csum_partial()은 1의 보수 부분합 타입 __wsum을 쓰므로
		 * iter->csum(u64)을 __force 캐스트로 __wsum처럼 재해석해 이어서
		 * 계산하고, 결과도 __force로 u32에 담아 저장 - 아직 최종 1의
		 * 보수 fold는 하지 않음(다음 두 줄에 이어짐) */
		iter->csum = (__force u32)csum_partial(data, len,
						(__force __wsum)iter->csum);
		/* [한국어] IP 체크섬 최종 fold는 blk_integrity_csum_finish()가
		 * 별도로 수행 - 여기서는 부분합만 누적 */
		break;
		/* [한국어] IP 체크섬 분기 종료 */
	/* [한국어] 알려지지 않은 csum_type - blk_validate_integrity_limits()가
	 * 이미 검증했어야 하므로 여기 도달하면 상위 계층의 버그 */
	default:
		/* [한국어] 커널 로그에 1회만 경고(스택트레이스 포함)를 남겨
		 * 개발자가 잘못된 프로파일 설정 원인을 추적할 수 있게 함 */
		WARN_ON_ONCE(1);
		/* [한국어] 계산을 포기하는 대신 csum을 의도적으로 "guard 비교가
		 * 절대 성공할 수 없는" 값(U64_MAX, 전 비트 1)으로 오염시켜, 이후
		 * 검증/생성 단계가 항상 실패하도록 강제 - 잘못된 프로파일로
		 * 무결성 검사가 조용히 우회되는 사고를 방지 */
		iter->csum = U64_MAX;
		break;
		/* [한국어] default 분기 종료 */
	}
}

/*
 * [한국어]
 * blk_integrity_csum_finish - IP 체크섬 방식에 대해 최종 1의 보수 fold를 수행
 *
 * @iter: iter->csum에 누적된 부분합(IP 체크섬의 경우)을 최종 16비트 값으로
 *        접어(fold) 다시 iter->csum에 저장한다.
 * @return: 없음(void).
 *
 * csum_partial()로 누적한 IP 체크섬은 32비트(또는 그 이상) 부분합 상태이며,
 * 실제 tuple에 저장/비교 가능한 16비트 체크섬으로 만들려면 캐리(carry)를
 * 접어 넣는 1의 보수 fold(csum_fold()) 연산이 필요하다. CRC64/CRC16-T10DIF는
 * 애초에 매 바이트를 반영할 때마다 이미 최종 폭으로 계산되므로 이 마무리
 * 단계가 필요 없어 default 분기가 아무 일도 하지 않는다.
 * 실행 컨텍스트: 호출자(blk_integrity_csum_offset())와 동일 컨텍스트 -
 * 스택 지역 상태만 다루므로 동기화 불필요.
 * 호출자(caller): blk_integrity_csum_offset() - 한 interval의 metadata
 * padding 영역까지 체크섬에 반영한 직후, tuple을 실제로 읽거나 쓰기 전에
 * 최종 마무리를 위해 호출.
 * 호출되는 함수(callee): csum_fold().
 * 에러 처리: 없음 - 순수 산술 변환.
 *
 * 호출 체인:
 *   blk_integrity_interval -> blk_integrity_csum_offset ->
 *   [blk_integrity_csum_finish] -> csum_fold()
 */
static void blk_integrity_csum_finish(struct blk_integrity_iter *iter)
{
	/* [한국어] csum_type이 IP일 때만 fold가 필요 - 그 외 알고리즘은 이미
	 * 최종 형태이므로 default에서 아무 처리도 하지 않음 */
	switch (iter->bi->csum_type) {
	/* [한국어] IP 체크섬(csum_type==IP) 분기 */
	case BLK_INTEGRITY_CSUM_IP:
		/* [한국어] __wsum(1의 보수 부분합)으로 재해석한 iter->csum을
		 * csum_fold()로 접어 최종 16비트 IP 체크섬을 만들고, 다시
		 * __force로 u16 재해석 후 iter->csum(u64)에 저장(상위 비트는
		 * 0으로 채워짐) - 이 값이 그대로 guard_tag로 쓰이거나 비교된다 */
		iter->csum = (__force u16)csum_fold((__force __wsum)iter->csum);
		break;
		/* [한국어] IP 분기 종료 */
	/* [한국어] CRC64/CRC(T10 DIF) 등 이미 최종 폭으로 계산된 알고리즘은
	 * fold가 필요 없으므로 아무 것도 하지 않고 통과 */
	default:
		break;
		/* [한국어] default 분기 - 아무 동작 없이 switch 종료 */
	}
}

/*
 * Update the csum for formats that have metadata padding in front of the data
 * integrity field
 */
/*
 * [한국어]
 * blk_integrity_csum_offset - 메타데이터 앞쪽 padding 영역을 건너뛰며, 그
 * 영역이 체크섬에 포함돼야 하는 포맷이면 미리 반영해 둔다
 *
 * @iter: 대상 반복자. iter->bi->pi_offset(메타데이터 안에서 PI tuple이
 *        시작되는 오프셋), iter->prot_iter(메타데이터 bvec 진행 위치),
 *        iter->csum(누적 체크섬)을 사용/갱신한다.
 * @return: 없음(void).
 *
 * 일부 PI 포맷은 metadata_size 전체가 PI 8~16바이트가 아니라, 앞쪽에
 * pi_offset바이트만큼의 벤더 전용/사용자 정의 padding이 먼저 오고 그
 * 뒤에 실제 PI tuple이 온다. 이 padding 영역이 guard 체크섬 계산 범위에
 * 포함돼야 하는 포맷(metadata_size > pi_tuple_size인 NVMe 포맷)이
 * 있으므로, 이 함수는 prot_iter를 pi_offset만큼 전진시키면서 그 경로에
 * 있는 바이트들을 blk_calculate_guard()로 체크섬에 먼저 반영한다. offset이
 * 0이면(대부분의 표준 PI 포맷) while 루프를 건너뛰고 바로 fold만 수행한다.
 * 실행 컨텍스트: 호출자와 동일 - 제출자 프로세스 또는 kintegrityd 워커.
 * 호출자(caller): blk_integrity_interval() - 매 interval 처리 시작 시
 * 가장 먼저 호출되어, 이후 tuple 매핑/처리 전에 padding을 정리한다.
 * 호출되는 함수(callee): bvec_iter_bvec(), bvec_kmap_local(),
 * blk_calculate_guard(), kunmap_local(), bvec_iter_advance_single(),
 * blk_integrity_csum_finish().
 * 에러 처리: 없음 - offset이 이미 blk_validate_integrity_limits()로
 * 검증된 값이라고 가정한다.
 *
 * 호출 체인:
 *   blk_integrity_interval -> [blk_integrity_csum_offset] ->
 *   blk_calculate_guard -> blk_integrity_csum_finish
 */
static void blk_integrity_csum_offset(struct blk_integrity_iter *iter)
{
	/* [한국어] 메타데이터 안에서 실제 PI tuple이 시작되기 전 padding
	 * 바이트 수 - 대부분의 표준 포맷에서는 0 */
	unsigned int offset = iter->bi->pi_offset;
	/* [한국어] PI 메타데이터가 저장된 bio_vec 배열 - bvec_iter_bvec()이
	 * iter->prot_iter의 현재 위치에 해당하는 페이지/오프셋/길이를 뽑아내는
	 * 데 필요 */
	struct bio_vec *bvec = iter->bip->bip_vec;

	/* [한국어] offset이 남아있는 동안 padding 영역을 한 bvec 조각씩
	 * 소비하며 체크섬에 반영 - 하나의 padding이 여러 bio_vec 페이지에
	 * 걸쳐 있을 수 있으므로 루프로 처리 */
	while (offset > 0) {
		/* [한국어] 현재 prot_iter 위치의 bio_vec 조각(페이지+오프셋+길이)을
		 * 계산 - 실제 메모리 접근 전 아직 매핑은 하지 않음 */
		struct bio_vec pbv = bvec_iter_bvec(bvec, iter->prot_iter);
		/* [한국어] 이번에 처리할 길이 = 남은 offset과 현재 bvec 조각
		 * 길이 중 작은 쪽 - 조각 경계를 넘어서 접근하지 않기 위함 */
		unsigned int len = min(pbv.bv_len, offset);
		/* [한국어] 해당 bio_vec 페이지를 커널 가상 주소 공간에 임시
		 * 매핑(kmap_local) - highmem 페이지도 안전하게 접근하기 위해
		 * 필요(단일 페이지 매핑이므로 mp_ 계열이 아닌 단순 kmap 사용) */
		void *prot_buf = bvec_kmap_local(&pbv);

		/* [한국어] 이번 padding 조각을 체크섬에 반영 - padding도 매체에
		 * 그대로 기록/전송되므로, 포맷에 따라 guard 계산 범위에 포함될
		 * 수 있다 */
		blk_calculate_guard(iter, prot_buf, len);
		/* [한국어] 매핑 해제 - 이 조각에 대한 접근이 끝났으므로 즉시
		 * 반납(커널 가상 주소 공간은 한정된 자원) */
		kunmap_local(prot_buf);
		/* [한국어] 처리한 만큼 남은 offset을 감소 - 0이 되면 while 루프
		 * 종료 조건 충족 */
		offset -= len;
		/* [한국어] prot_iter를 len바이트만큼 전진 - 다음 반복에서 다음
		 * padding 조각(또는 실제 PI tuple 시작 위치)을 가리키게 됨 */
		bvec_iter_advance_single(bvec, &iter->prot_iter, len);
	}
	/* [한국어] padding 반영이 끝난 뒤, IP 체크섬이면 최종 fold를 수행 -
	 * CRC 계열은 이 함수 내부에서 아무 일도 하지 않음 */
	blk_integrity_csum_finish(iter);
}

/*
 * [한국어]
 * blk_integrity_copy_from_tuple - 스택의 tuple 스크래치 버퍼 내용을 실제
 * PI 메타데이터 bio_vec 버퍼로 복사(WRITE 시 tuple 조립 결과를 반영)
 *
 * @bip: 대상 무결성 payload - bip->bip_vec이 실제 복사 대상 페이지들을 제공.
 * @iter: 메타데이터 버퍼 내 현재 진행 위치(bvec_iter) - 복사가 끝난 만큼
 *        전진시킨다(입출력 겸용).
 * @tuple: 복사 원본 - union pi_tuple(또는 그 일부)이 담긴 스택 버퍼의 포인터.
 *        tuple_size만큼 복사한 뒤 그 포인터 자체도 전진(tuple += len)한다.
 * @tuple_size: 복사할 총 바이트 수 - 보통 bi->pi_tuple_size(PI tuple 크기).
 * @return: 없음(void).
 *
 * PI tuple이 metadata bio_vec의 페이지 경계를 걸쳐 있어 한 번의
 * kmap_local()로 통째로 매핑할 수 없을 때, blk_integrity_interval()이나
 * blk_tuple_remap_begin()은 스택 위에 union pi_tuple 크기의 임시 버퍼를
 * 만들어 그 안에서 값을 계산/수정한 뒤, 이 함수를 통해 실제 여러 bio_vec
 * 페이지에 걸쳐 나눠 써 넣는다. while 루프는 tuple_size가 소진될 때까지
 * 매 반복마다 현재 bio_vec 조각의 남은 길이와 tuple_size 중 작은 쪽만큼만
 * memcpy하고 iter/tuple 포인터를 함께 전진시킨다.
 * 실행 컨텍스트: 호출자와 동일 - 제출자 프로세스 또는 kintegrityd 워커,
 * 혹은 remap 관련 호출자의 컨텍스트(완료 인터럽트 포함).
 * 호출자(caller): blk_integrity_interval()(WRITE 생성 결과를 tuple에서
 * 실제 버퍼로 되돌려 쓸 때), blk_tuple_remap_end()(remap 결과 반영).
 * 호출되는 함수(callee): bvec_iter_bvec(), bvec_kmap_local(), memcpy(),
 * kunmap_local(), bvec_iter_advance_single().
 * 에러 처리: 없음 - tuple_size는 항상 남은 bio_vec 총량 이내라고 가정
 * (pi_tuple_size가 metadata_size를 넘지 않도록 blk_validate_integrity_limits()가
 * 미리 검증한다).
 *
 * 호출 체인:
 *   blk_integrity_interval / blk_tuple_remap_end -> [blk_integrity_copy_from_tuple]
 *   -> memcpy()
 */
static void blk_integrity_copy_from_tuple(struct bio_integrity_payload *bip,
					  struct bvec_iter *iter, void *tuple,
					  unsigned int tuple_size)
{
	/* [한국어] 요청한 tuple_size 바이트를 모두 복사할 때까지 반복 - 한
	 * bio_vec 조각이 tuple_size보다 짧을 수 있으므로(페이지 경계) 루프 필요 */
	while (tuple_size) {
		/* [한국어] 메타데이터 버퍼의 현재 위치에 해당하는 bio_vec
		 * 조각(페이지+오프셋+길이)을 계산 */
		struct bio_vec pbv = bvec_iter_bvec(bip->bip_vec, *iter);
		/* [한국어] 이번에 복사할 길이 = 남은 tuple_size와 현재 bvec
		 * 조각 길이 중 작은 쪽 - 페이지 경계를 넘어 매핑하지 않기 위함 */
		unsigned int len = min(tuple_size, pbv.bv_len);
		/* [한국어] 대상 페이지를 커널 가상 주소로 임시 매핑 */
		void *prot_buf = bvec_kmap_local(&pbv);

		/* [한국어] 실제 복사: tuple(스택 스크래치) -> prot_buf(메타데이터
		 * 실버퍼) 방향 - 이름과 반대로 "tuple로부터 버퍼로" 복사하는
		 * 함수이므로 WRITE 결과 반영에 쓰인다 */
		memcpy(prot_buf, tuple, len);
		/* [한국어] 매핑 해제 - 이 조각 처리 종료 */
		kunmap_local(prot_buf);
		/* [한국어] 메타데이터 bvec_iter를 len만큼 전진 - 다음 조각으로 이동 */
		bvec_iter_advance_single(bip->bip_vec, iter, len);
		/* [한국어] 남은 tuple_size 감소 */
		tuple_size -= len;
		/* [한국어] tuple(스크래치 버�퍼) 포인터도 이미 복사한 만큼 전진
		 * - 다음 반복에서 이어지는 바이트를 가리키게 함 */
		tuple += len;
	}
}

/*
 * [한국어]
 * blk_integrity_copy_to_tuple - 여러 bio_vec에 나뉜 실제 PI 메타데이터를
 * 스택의 tuple 스크래치 버퍼로 모아 옴(READ 검증 또는 remap 준비 시 사용)
 *
 * @bip: 대상 무결성 payload - bip->bip_vec에서 원본 페이지들을 읽어온다.
 * @iter: 메타데이터 버퍼 내 현재 진행 위치(bvec_iter) - 읽은 만큼
 *        전진시킨다(입출력 겸용).
 * @tuple: 복사 대상 - union pi_tuple이 담길 스택 버퍼의 포인터. tuple_size만큼
 *        채운 뒤 그 포인터도 전진(tuple += len)한다.
 * @tuple_size: 모아 올 총 바이트 수 - 보통 bi->pi_tuple_size.
 * @return: 없음(void).
 *
 * blk_integrity_copy_from_tuple()과 정반대 방향의 memcpy를 수행하는
 * 쌍둥이 함수다. PI tuple이 metadata bio_vec 페이지 경계에 걸쳐 있어
 * 한 번에 매핑할 수 없을 때, 검증(READ) 전이나 remap 준비 단계에서
 * 실제 여러 페이지에 흩어진 바이트들을 스택 위 하나의 연속된 tuple
 * 버퍼로 모아 옴으로써, 이후 코드가 &tuple->guard_tag처럼 단순 포인터
 * 연산으로 필드를 읽을 수 있게 해준다.
 * 실행 컨텍스트: 호출자와 동일.
 * 호출자(caller): blk_integrity_interval()(READ 검증 시 tuple 모으기),
 * blk_tuple_remap_begin()(remap 대상 tuple 모으기).
 * 호출되는 함수(callee): bvec_iter_bvec(), bvec_kmap_local(), memcpy(),
 * kunmap_local(), bvec_iter_advance_single().
 * 에러 처리: 없음.
 *
 * 호출 체인:
 *   blk_integrity_interval / blk_tuple_remap_begin -> [blk_integrity_copy_to_tuple]
 *   -> memcpy()
 */
static void blk_integrity_copy_to_tuple(struct bio_integrity_payload *bip,
					struct bvec_iter *iter, void *tuple,
					unsigned int tuple_size)
{
	/* [한국어] tuple_size 바이트를 모두 모을 때까지 반복 */
	while (tuple_size) {
		/* [한국어] 메타데이터 버퍼의 현재 위치에 해당하는 bio_vec
		 * 조각을 계산 */
		struct bio_vec pbv = bvec_iter_bvec(bip->bip_vec, *iter);
		/* [한국어] 이번에 읽을 길이 = 남은 tuple_size와 현재 bvec 조각
		 * 길이 중 작은 쪽 */
		unsigned int len = min(tuple_size, pbv.bv_len);
		/* [한국어] 원본 페이지를 커널 가상 주소로 임시 매핑 */
		void *prot_buf = bvec_kmap_local(&pbv);

		/* [한국어] 실제 복사: prot_buf(메타데이터 실버퍼) -> tuple(스택
		 * 스크래치) 방향 - "버퍼로부터 tuple로" 모아옴 */
		memcpy(tuple, prot_buf, len);
		/* [한국어] 매핑 해제 */
		kunmap_local(prot_buf);
		/* [한국어] 메타데이터 bvec_iter를 len만큼 전진 */
		bvec_iter_advance_single(bip->bip_vec, iter, len);
		/* [한국어] 남은 tuple_size 감소 */
		tuple_size -= len;
		/* [한국어] tuple(스크래치 버퍼) 포인터를 전진 - 다음 반복에서
		 * 이어지는 위치에 채워 넣게 함 */
		tuple += len;
	}
}

/*
 * [한국어]
 * ext_pi_ref_escape - CRC64 확장 PI의 48비트 Reference Tag가 escape
 * 값(전 비트 1)인지 검사
 *
 * @ref_tag: crc64_pi_tuple.ref_tag - 6바이트(48비트) 배열로 저장된 Reference
 *           Tag 원본 바이트(빅엔디안, get_unaligned_be48()로 아직 정수로
 *           변환하기 전의 원시 배열 형태).
 * @return: true = 6바이트가 모두 0xff(escape 값, 검사 생략 대상),
 *          false = 그 외(정상 검사 대상).
 *
 * 표준 4바이트 Reference Tag는 REF_TAG_ESCAPE(0xffffffff) 정수 비교로 충분하지만,
 * CRC64 확장 PI의 ref_tag는 u8[6] 배열로 선언되어 있어 단일 정수형이 아니므로
 * memcmp()로 6바이트 전부가 0xff인지 비교하는 별도 헬퍼가 필요하다. static
 * const 배열 ref_escape는 함수 호출마다 스택에 새로 만들지 않고 컴파일 시
 * 한 번만 초기화되는 상수 데이터로 유지된다.
 * 실행 컨텍스트: 호출자(blk_verify_ext_pi())와 동일 - 순수 함수라 동기화
 * 불필요.
 * 호출자(caller): blk_verify_ext_pi() - BLK_INTEGRITY_REF_TAG 플래그가
 * 꺼져 있을 때 app/ref 모두 escape인지 판정하기 위해.
 * 호출되는 함수(callee): memcmp().
 * 에러 처리: 없음 - 순수 비교 함수.
 *
 * 호출 체인:
 *   blk_verify_ext_pi -> [ext_pi_ref_escape] -> memcmp()
 */
static bool ext_pi_ref_escape(const u8 ref_tag[6])
{
	/* [한국어] "48비트 전부 escape(1)" 상태를 나타내는 상수 패턴 - 매
	 * 호출마다 재생성되지 않도록 static const로 선언 */
	static const u8 ref_escape[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	/* [한국어] 전달받은 ref_tag 6바이트와 escape 패턴을 바이트 단위로
	 * 비교 - 완전히 같으면(반환값 0) true, 하나라도 다르면 false */
	return memcmp(ref_tag, ref_escape, sizeof(ref_escape)) == 0;
}

/*
 * [한국어]
 * blk_verify_ext_pi - NVMe CRC64 확장 PI tuple의 Guard/AppTag/RefTag를 검증
 *
 * @iter: 현재 interval의 반복자 - iter->seed(기대하는 ref tag 기준값),
 *        iter->csum(방금 계산된 guard 후보값), iter->bi->flags(REF_TAG 검사
 *        여부), iter->bio(에러 로그용 디스크 이름)를 사용.
 * @pi: 읽어들인(또는 kmap으로 직접 매핑된) crc64_pi_tuple - 매체/메타데이터
 *      버퍼에 실제로 저장돼 있던 값.
 * @return: BLK_STS_OK = 검증 통과(또는 escape로 검사 생략), 그 외에는
 *          BLK_STS_PROTECTION = Guard 또는 Reference Tag 불일치(무결성 위반).
 *
 * NVMe CRC64 확장 PI는 8바이트 Guard + 2바이트 AppTag + 6바이트(48비트)
 * RefTag로 구성된 16바이트 tuple이다(다른 T10 PI들의 8바이트 tuple보다
 * 크다). 이 함수는 먼저 tuple에서 seed/guard/ref/app 네 값을 빅엔디안
 * 비정렬 읽기 헬퍼로 추출한 뒤, BLK_INTEGRITY_REF_TAG 플래그(이 디스크가
 * ref tag 검사를 요구하는 PI Type 1/2인지)에 따라 두 갈래로 나뉜다.
 * 플래그가 켜져 있으면 app==APP_TAG_ESCAPE일 때 즉시 통과시키고, 아니면
 * ref가 기대한 seed(lower_48_bits(iter->seed))와 일치하는지 검사해 불일치
 * 시 pr_err()로 위치/기대값/수신값을 로그에 남기고 BLK_STS_PROTECTION을
 * 반환한다. 플래그가 꺼져 있으면(ref tag를 애초에 검사하지 않는 PI Type,
 * Type 3) app과 ref가 모두 escape 값일 때만 guard 검사까지 건너뛴다. 이
 * 두 갈래를 통과하면 마지막으로 guard(수신된 CRC64)와 iter->csum(호스트가
 * 방금 데이터로부터 재계산한 CRC64)을 비교해 불일치 시 역시
 * BLK_STS_PROTECTION을 반환한다.
 * 실행 컨텍스트: 호출자(blk_integrity_verify())와 동일 - READ 완료 경로이므로
 * kintegrityd 워커 프로세스 컨텍스트 또는 파일시스템이 직접 검증하는
 * 프로세스 컨텍스트.
 * 호출자(caller): blk_integrity_verify() - csum_type==CRC64일 때 선택.
 * 호출되는 함수(callee): lower_48_bits(), get_unaligned_be64/48/16(),
 * ext_pi_ref_escape(), pr_err().
 * 에러 처리: 불일치를 발견하는 즉시 pr_err()로 사람이 읽을 수 있는 진단
 * 메시지를 남기고 BLK_STS_PROTECTION을 반환 - 호출 체인을 따라
 * blk_integrity_interval() -> blk_integrity_iterate()까지 그대로
 * 전파되어 이후 interval 처리를 중단시킨다.
 *
 * 호출 체인:
 *   blk_integrity_verify -> [blk_verify_ext_pi] -> ext_pi_ref_escape() / pr_err()
 */
static blk_status_t blk_verify_ext_pi(struct blk_integrity_iter *iter,
				      struct crc64_pi_tuple *pi)
{
	/* [한국어] 이번 interval에서 기대하는 ref tag 값 - iter->seed(u64,
	 * 시작 섹터+처리한 interval 수)의 하위 48비트만 취함(crc64 확장
	 * ref_tag가 48비트이므로) */
	u64 seed = lower_48_bits(iter->seed);
	/* [한국어] tuple에서 실제 저장된 Guard(64비트 CRC) 값을 빅엔디안
	 * 비정렬 읽기로 추출 */
	u64 guard = get_unaligned_be64(&pi->guard_tag);
	/* [한국어] tuple에서 실제 저장된 Reference Tag(48비트, u8[6] 배열)를
	 * 빅엔디안 비정렬 읽기로 정수화 - &가 없는 이유는 pi->ref_tag 자체가
	 * 이미 배열(포인터로 decay)이기 때문 */
	u64 ref = get_unaligned_be48(pi->ref_tag);
	/* [한국어] tuple에서 실제 저장된 Application Tag(16비트)를 빅엔디안
	 * 비정렬 읽기로 추출 - escape 판정에 사용 */
	u16 app = get_unaligned_be16(&pi->app_tag);

	/* [한국어] 이 디스크 프로파일이 Reference Tag 검사를 요구하는 PI
	 * Type인지 확인(BLK_INTEGRITY_REF_TAG 비트) - 켜져 있으면 ref tag
	 * 불일치도 무결성 위반으로 취급 */
	if (iter->bi->flags & BLK_INTEGRITY_REF_TAG) {
		/* [한국어] app tag가 escape 값이면 T10 PI 규약상 이 interval에
		 * 대한 검사(ref 포함)를 생략하라는 지시 */
		if (app == APP_TAG_ESCAPE)
			/* [한국어] escape 지시에 따라 조기 반환 - guard/ref 비교
			 * 없이 통과 처리 */
			return BLK_STS_OK;
		/* [한국어] 실제 ref tag가 기대한 seed와 다르면 위치 오검출
		 * (misdirected I/O) 가능성 - 예: 다른 LBA의 데이터가 잘못
		 * 배달된 경우 */
		if (ref != seed) {
			/* [한국어] 사람이 읽을 수 있는 진단 로그: 디스크 이름,
			 * 기대한 위치(seed), 실제 수신한 ref 값을 %llu로 출력 */
			pr_err("%s: ref tag error at location %llu (rcvd %llu)\n",
				iter->bio->bi_bdev->bd_disk->disk_name, seed,
				ref);
			/* [한국어] ref tag 불일치는 무결성 위반이므로
			 * BLK_STS_PROTECTION을 반환해 상위(블록 계층/파일시스템)에
			 * I/O 에러로 전파 */
			return BLK_STS_PROTECTION;
		}
	/* [한국어] REF_TAG 검사가 필요 없는 프로파일(else 분기) - app과 ref
	 * 모두 escape일 때만("완전 무보호" 표시) guard 검사까지 건너뜀.
	 * ext_pi_ref_escape()로 48비트 ref_tag 배열 전체가 0xff인지 확인 */
	} else if (app == APP_TAG_ESCAPE && ext_pi_ref_escape(pi->ref_tag)) {
		/* [한국어] 둘 다 escape면 이 interval은 애초에 보호되지 않는
		 * 데이터로 간주해 통과 */
		return BLK_STS_OK;
	}

	/* [한국어] 여기까지 왔다면 ref/app 검사는 통과(또는 대상 아님) -
	 * 마지막으로 Guard(체크섬) 자체를 비교. guard = 매체에 저장돼 있던
	 * 값, iter->csum = 방금 데이터로부터 재계산한 값 */
	if (guard != iter->csum) {
		/* [한국어] guard 불일치 진단 로그 - 디스크 이름, 섹터 위치,
		 * 수신값(rcvd)과 기대값(want)을 16자리 16진수(%016llx)로 출력해
		 * 어느 비트가 깨졌는지 분석 가능하게 함 */
		pr_err("%s: guard tag error at sector %llu (rcvd %016llx, want %016llx)\n",
			iter->bio->bi_bdev->bd_disk->disk_name, iter->seed,
			guard, iter->csum);
		/* [한국어] guard 불일치는 매체 손상(bit rot)이나 전송 오류를
		 * 의미하는 가장 직접적인 무결성 위반 - BLK_STS_PROTECTION 반환 */
		return BLK_STS_PROTECTION;
	}

	/* [한국어] 모든 검사를 통과 - 이 interval의 데이터는 무결성이 확인됨 */
	return BLK_STS_OK;
}

/*
 * [한국어]
 * blk_verify_pi - 표준 T10 PI(8바이트) tuple의 AppTag/RefTag/Guard를 검증
 *
 * @iter: 현재 interval의 반복자 - iter->seed(기대 ref tag), iter->csum(재계산된
 *        guard 후보), iter->bi->flags, iter->bio(에러 로그용)를 사용.
 * @pi: 읽어들인 t10_pi_tuple - app_tag/ref_tag 필드를 추출하는 데 사용.
 * @guard: 이미 호출자가(포맷별 정렬 방식에 맞게) 읽어온 16비트 Guard 값 -
 *         blk_verify_t10_pi()/blk_verify_ip_pi() 두 포맷이 guard_tag를 서로
 *         다른 방식(빅엔디안 vs 네이티브)으로 읽으므로, 공통 로직은 이미
 *         읽힌 값을 파라미터로 받아 포맷 차이를 감춘다.
 * @return: BLK_STS_OK = 검증 통과(또는 escape), BLK_STS_PROTECTION = 위반.
 *
 * blk_verify_ext_pi()의 표준(비확장) T10 PI 버전이다. RefTag가 32비트
 * 정수(t10_pi_tuple.ref_tag)이고 REF_TAG_ESCAPE(0xffffffff) 하나의 정수
 * 비교로 escape를 판정할 수 있다는 점이 crc64 확장 버전과 다르다(그래서
 * ext_pi_ref_escape() 같은 별도 헬퍼가 필요 없음). 로직 구조는 동일 -
 * REF_TAG 플래그가 켜져 있으면 app==ESCAPE로 조기 통과 또는 ref 불일치
 * 시 에러, 꺼져 있으면 app과 ref 모두 escape일 때만 조기 통과, 마지막으로
 * guard를 (u16)iter->csum과 비교.
 * 실행 컨텍스트: 호출자(blk_verify_t10_pi/blk_verify_ip_pi)와 동일.
 * 호출자(caller): blk_verify_t10_pi(), blk_verify_ip_pi() - guard 파라미터만
 * 다르게 읽어서 넘겨준다.
 * 호출되는 함수(callee): lower_32_bits(), get_unaligned_be32/16(), pr_err().
 * 에러 처리: blk_verify_ext_pi()와 동일한 패턴 - 진단 로그 후
 * BLK_STS_PROTECTION 반환.
 *
 * 호출 체인:
 *   blk_verify_t10_pi / blk_verify_ip_pi -> [blk_verify_pi] -> pr_err()
 */
static blk_status_t blk_verify_pi(struct blk_integrity_iter *iter,
				      struct t10_pi_tuple *pi, u16 guard)
{
	/* [한국어] 이번 interval에서 기대하는 ref tag - iter->seed(u64)의
	 * 하위 32비트(표준 ref_tag는 32비트 필드이므로) */
	u32 seed = lower_32_bits(iter->seed);
	/* [한국어] tuple에서 실제 저장된 Reference Tag(32비트)를 빅엔디안
	 * 비정렬 읽기로 추출 */
	u32 ref = get_unaligned_be32(&pi->ref_tag);
	/* [한국어] tuple에서 실제 저장된 Application Tag(16비트)를 빅엔디안
	 * 비정렬 읽기로 추출 */
	u16 app = get_unaligned_be16(&pi->app_tag);

	/* [한국어] 이 디스크 프로파일이 Reference Tag 검사를 요구하는지 확인 */
	if (iter->bi->flags & BLK_INTEGRITY_REF_TAG) {
		/* [한국어] app tag escape면 이 interval 검사를 생략하라는 지시 */
		if (app == APP_TAG_ESCAPE)
			/* [한국어] escape 처리 - 조기 통과 */
			return BLK_STS_OK;
		/* [한국어] ref tag가 기대한 seed와 다르면 위치 오검출 가능성 */
		if (ref != seed) {
			/* [한국어] 진단 로그: 디스크 이름, 기대 위치(seed), 실제
			 * 수신값(ref)을 10진수(%u)로 출력 - 표준 32비트라 llu가
			 * 아닌 u 포맷 사용 */
			pr_err("%s: ref tag error at location %u (rcvd %u)\n",
				iter->bio->bi_bdev->bd_disk->disk_name, seed,
				ref);
			/* [한국어] ref tag 불일치 - 무결성 위반으로 반환 */
			return BLK_STS_PROTECTION;
		}
	/* [한국어] REF_TAG 검사가 필요 없는 프로파일 - app과 ref 모두
	 * escape(REF_TAG_ESCAPE, 정수 0xffffffff)일 때만 조기 통과 */
	} else if (app == APP_TAG_ESCAPE && ref == REF_TAG_ESCAPE) {
		/* [한국어] 완전 무보호 데이터로 간주해 통과 */
		return BLK_STS_OK;
	}

	/* [한국어] 마지막으로 Guard 비교 - guard는 호출자가 미리 읽어온 16비트
	 * 수신값, (u16)iter->csum은 재계산된 값(상위 비트는 버림) */
	if (guard != (u16)iter->csum) {
		/* [한국어] guard 불일치 진단 로그 - 섹터 위치, 수신값/기대값을
		 * 4자리 16진수(%04x)로 출력(16비트이므로 016llx 대신 04x) */
		pr_err("%s: guard tag error at sector %llu (rcvd %04x, want %04x)\n",
			iter->bio->bi_bdev->bd_disk->disk_name, iter->seed,
			guard, (u16)iter->csum);
		/* [한국어] guard 불일치는 매체 손상/전송 오류 - 무결성 위반 반환 */
		return BLK_STS_PROTECTION;
	}

	/* [한국어] 모든 검사 통과 */
	return BLK_STS_OK;
}

/*
 * [한국어]
 * blk_verify_t10_pi - CRC(T10 DIF) csum_type용 Guard 필드 읽기 + 공통 검증 위임
 *
 * @iter: 현재 interval 반복자.
 * @pi: 읽어들인 t10_pi_tuple.
 * @return: blk_verify_pi()의 반환값을 그대로 전달 - BLK_STS_OK 또는
 *          BLK_STS_PROTECTION.
 *
 * t10_pi_tuple.guard_tag는 __be16(빅엔디안 16비트)으로 선언되어 있으므로,
 * CRC(T10 DIF) 포맷에서는 항상 빅엔디안으로 저장된 값을 get_unaligned_be16()으로
 * 읽어야 한다. 이 얇은 래퍼는 그 포맷별 읽기 방식만 담당하고, 실제 비교
 * 로직은 공통 함수 blk_verify_pi()에 위임한다.
 * 실행 컨텍스트: 호출자(blk_integrity_verify())와 동일.
 * 호출자(caller): blk_integrity_verify() - csum_type==CRC일 때 선택.
 * 호출되는 함수(callee): get_unaligned_be16(), blk_verify_pi().
 * 에러 처리: 없음(자체 로직 없음) - blk_verify_pi()에 위임.
 *
 * 호출 체인:
 *   blk_integrity_verify -> [blk_verify_t10_pi] -> blk_verify_pi
 */
static blk_status_t blk_verify_t10_pi(struct blk_integrity_iter *iter,
				      struct t10_pi_tuple *pi)
{
	/* [한국어] 표준 T10 DIF는 guard_tag가 빅엔디안으로 저장되므로
	 * get_unaligned_be16()으로 바이트오더를 맞춰 읽음 */
	u16 guard = get_unaligned_be16(&pi->guard_tag);

	/* [한국어] 읽어온 guard 값을 넘겨 공통 검증 로직(RefTag/AppTag/Guard
	 * 비교)을 그대로 재사용 */
	return blk_verify_pi(iter, pi, guard);
}

/*
 * [한국어]
 * blk_verify_ip_pi - IP 체크섬 csum_type용 Guard 필드 읽기 + 공통 검증 위임
 *
 * @iter: 현재 interval 반복자.
 * @pi: 읽어들인 t10_pi_tuple.
 * @return: blk_verify_pi()의 반환값을 그대로 전달.
 *
 * IP 체크섬 포맷은 blk_calculate_guard()에서 csum_partial()이 계산 도중
 * 네이티브(호스트) 바이트오더의 __wsum으로 다루므로, 저장된 값도 빅엔디안
 * 변환 없이 네이티브 바이트오더 그대로 get_unaligned()로 읽어야 CRC(T10
 * DIF) 방식의 get_unaligned_be16()과 다르게 대칭이 맞는다(IP 체크섬은
 * 네트워크 코드 관례상 네이티브 표현을 그대로 재사용). 나머지 로직은
 * blk_verify_t10_pi()와 완전히 동일하게 blk_verify_pi()에 위임한다.
 * 실행 컨텍스트: 호출자와 동일.
 * 호출자(caller): blk_integrity_verify() - csum_type==IP일 때 선택.
 * 호출되는 함수(callee): get_unaligned(), blk_verify_pi().
 * 에러 처리: 없음 - blk_verify_pi()에 위임.
 *
 * 호출 체인:
 *   blk_integrity_verify -> [blk_verify_ip_pi] -> blk_verify_pi
 */
static blk_status_t blk_verify_ip_pi(struct blk_integrity_iter *iter,
				     struct t10_pi_tuple *pi)
{
	/* [한국어] IP 체크섬은 네이티브 바이트오더로 다루므로 get_unaligned()로
	 * (빅엔디안 변환 없이) 그대로 읽음 - CRC 포맷의 get_unaligned_be16()과
	 * 대비됨 */
	u16 guard = get_unaligned((u16 *)&pi->guard_tag);

	/* [한국어] 읽어온 guard 값을 넘겨 공통 검증 로직 재사용 */
	return blk_verify_pi(iter, pi, guard);
}

/*
 * [한국어]
 * blk_integrity_verify - csum_type에 맞는 검증 함수로 디스패치
 *
 * @iter: 현재 interval 반복자 - iter->bi->csum_type으로 알고리즘 선택.
 * @tuple: union pi_tuple - 실제 사용할 멤버(crc64_pi 또는 t10_pi)는
 *         csum_type에 따라 결정됨.
 * @return: 선택된 하위 검증 함수의 반환값(BLK_STS_OK/BLK_STS_PROTECTION),
 *          알 수 없는 csum_type이면 BLK_STS_OK(방어적 기본값 - 도달하면
 *          안 되는 경로).
 *
 * 이 파일 최상위의 "검증" 진입점 하나로, 세 가지 알고리즘(CRC64/CRC/IP) 중
 * 어느 것을 쓸지는 이미 blk_calculate_guard() 등에서 확인했던 것과 동일한
 * iter->bi->csum_type 값으로 다시 한번 분기해, 그에 맞는 tuple 레이아웃
 * 해석 함수(blk_verify_ext_pi/blk_verify_t10_pi/blk_verify_ip_pi)로 위임한다.
 * default 분기가 BLK_STS_OK를 반환하는 것은 blk_calculate_guard()의
 * WARN_ON_ONCE+오염 패턴과 달리 방어적으로 "통과"를 택한 것인데, 이는
 * csum_type이 유효하지 않으면 애초에 bio_integrity_generate/verify()의
 * switch에서 이 함수 자체가 호출되지 않도록 상위에서 걸러지기 때문에
 * 실질적으로 도달 불가능한 경로로 간주된다.
 * 실행 컨텍스트: 호출자(blk_integrity_interval())와 동일.
 * 호출자(caller): blk_integrity_interval() - verify==true일 때.
 * 호출되는 함수(callee): blk_verify_ext_pi(), blk_verify_t10_pi(),
 * blk_verify_ip_pi().
 * 에러 처리: 하위 함수의 BLK_STS_PROTECTION을 그대로 전파.
 *
 * 호출 체인:
 *   blk_integrity_interval -> [blk_integrity_verify] -> blk_verify_ext_pi /
 *   blk_verify_t10_pi / blk_verify_ip_pi
 */
static blk_status_t blk_integrity_verify(struct blk_integrity_iter *iter,
					 union pi_tuple *tuple)
{
	/* [한국어] csum_type에 따라 tuple의 어느 멤버(crc64_pi/t10_pi)를 어떤
	 * 함수로 검증할지 분기 */
	switch (iter->bi->csum_type) {
	/* [한국어] NVMe CRC64 확장 PI(16바이트, ref_tag 48비트) 검증 경로 */
	case BLK_INTEGRITY_CSUM_CRC64:
		/* [한국어] union의 crc64_pi 멤버 주소를 넘겨 확장 PI 전용 검증
		 * 수행 후 그 결과를 그대로 반환 */
		return blk_verify_ext_pi(iter, &tuple->crc64_pi);
	/* [한국어] 표준 T10 DIF CRC16 검증 경로 */
	case BLK_INTEGRITY_CSUM_CRC:
		/* [한국어] union의 t10_pi 멤버 주소를 넘겨 CRC 포맷 전용 검증 */
		return blk_verify_t10_pi(iter, &tuple->t10_pi);
	/* [한국어] IP 체크섬 검증 경로 */
	case BLK_INTEGRITY_CSUM_IP:
		/* [한국어] union의 t10_pi 멤버 주소를 넘겨 IP 포맷 전용 검증
		 * (tuple 레이아웃 자체는 CRC와 동일한 t10_pi_tuple, guard 읽는
		 * 방식만 다름) */
		return blk_verify_ip_pi(iter, &tuple->t10_pi);
	/* [한국어] 알 수 없는 csum_type - 도달해서는 안 되는 방어적 분기 */
	default:
		/* [한국어] 검증할 방법이 없으므로 통과 처리 - 상위에서 이미
		 * csum_type 유효성을 걸러냈다고 가정한다 */
		return BLK_STS_OK;
	}
}

/*
 * [한국어]
 * blk_set_ext_pi - NVMe CRC64 확장 PI tuple(16바이트)을 새로 기록(WRITE 생성)
 *
 * @iter: 현재 interval 반복자 - iter->csum(방금 계산한 guard), iter->seed
 *        (이번 interval의 ref tag 기준값)을 tuple에 기록할 원본으로 사용.
 * @pi: 기록 대상 crc64_pi_tuple - 매체/메타데이터 버퍼에 실제로 쓰여질 위치.
 * @return: 없음(void).
 *
 * WRITE 경로에서 Guard/AppTag/RefTag 세 필드를 모두 새로 채워 넣는다.
 * Guard는 방금 계산된 CRC64(iter->csum) 그대로, AppTag는 0(사용자가 별도
 * app tag를 요청하지 않는 기본 경로이므로 escape가 아닌 0으로 고정 - 실제
 * 값 지정이 필요하면 상위 계층이 이후 덮어쓴다), RefTag는
 * iter->seed(48비트로 잘려 저장, put_unaligned_be48이 내부적으로 하위
 * 48비트만 사용)를 빅엔디안으로 기록한다.
 * 실행 컨텍스트: 호출자(blk_integrity_set())와 동일 - WRITE 제출 경로의
 * 프로세스 컨텍스트.
 * 호출자(caller): blk_integrity_set() - csum_type==CRC64일 때.
 * 호출되는 함수(callee): put_unaligned_be64(), put_unaligned_be16(),
 * put_unaligned_be48().
 * 에러 처리: 없음 - 항상 성공하는 순수 기록 함수.
 *
 * 호출 체인:
 *   blk_integrity_set -> [blk_set_ext_pi] -> put_unaligned_be64/16/48()
 */
static void blk_set_ext_pi(struct blk_integrity_iter *iter,
			   struct crc64_pi_tuple *pi)
{
	/* [한국어] 방금 계산된 64비트 CRC(iter->csum)를 빅엔디안으로 guard_tag
	 * 필드에 기록 - 이 값이 이후 READ 시 blk_verify_ext_pi()가 비교할
	 * 기준이 됨 */
	put_unaligned_be64(iter->csum, &pi->guard_tag);
	/* [한국어] AppTag를 0으로 고정 기록 - escape(0xffff)가 아닌 "검사
	 * 대상" 상태를 나타냄(기본 경로에서는 app tag를 별도로 쓰지 않음,
	 */
	put_unaligned_be16(0, &pi->app_tag);
	/* [한국어] RefTag를 iter->seed(현재 interval의 LBA 기준)로 기록 -
	 * put_unaligned_be48()이 48비트만 취해 u8[6] 배열에 빅엔디안으로 씀 */
	put_unaligned_be48(iter->seed, &pi->ref_tag);
}

/*
 * [한국어]
 * blk_set_pi - 표준 T10 PI tuple(8바이트)의 공통 기록 로직 - Guard 값만
 * 포맷별로 다르게 받고, AppTag/RefTag는 공통으로 처리
 *
 * @iter: 현재 interval 반복자 - iter->seed를 RefTag로 사용.
 * @pi: 기록 대상 t10_pi_tuple.
 * @csum: 이미 포맷에 맞게(빅엔디안 또는 네이티브 그대로) 변환된 16비트
 *        Guard 값 - 호출자(blk_set_t10_pi/blk_set_ip_pi)가 csum_type별
 *        차이를 흡수해 넘겨준다.
 * @return: 없음(void).
 *
 * blk_verify_pi()의 기록(WRITE) 버전 대응 함수다. put_unaligned()는
 * (csum의 실제 타입 __be16 그대로) 별도 바이트오더 변환 없이 그대로
 * 기록하므로, 호출자가 이미 알맞은 표현으로 csum을 준비해 와야 한다.
 * AppTag는 0, RefTag는 iter->seed의 하위 32비트(put_unaligned_be32가
 * u64 인자를 받아 하위 32비트를 빅엔디안으로 기록)로 채운다.
 * 실행 컨텍스트: 호출자(blk_set_t10_pi/blk_set_ip_pi)와 동일.
 * 호출자(caller): blk_set_t10_pi(), blk_set_ip_pi().
 * 호출되는 함수(callee): put_unaligned(), put_unaligned_be16(),
 * put_unaligned_be32().
 * 에러 처리: 없음.
 *
 * 호출 체인:
 *   blk_set_t10_pi / blk_set_ip_pi -> [blk_set_pi] -> put_unaligned*()
 */
static void blk_set_pi(struct blk_integrity_iter *iter,
		       struct t10_pi_tuple *pi, __be16 csum)
{
	/* [한국어] 호출자가 이미 적절히 변환해 넘긴 csum(__be16)을 guard_tag에
	 * 그대로 기록 - put_unaligned()는 추가 바이트오더 변환을 하지 않음 */
	put_unaligned(csum, &pi->guard_tag);
	/* [한국어] AppTag를 0으로 고정 기록 */
	put_unaligned_be16(0, &pi->app_tag);
	/* [한국어] RefTag를 iter->seed의 하위 32비트로 빅엔디안 기록 */
	put_unaligned_be32(iter->seed, &pi->ref_tag);
}

/*
 * [한국어]
 * blk_set_t10_pi - CRC(T10 DIF) csum_type용 Guard 변환 + 공통 기록 위임
 *
 * @iter: 현재 interval 반복자 - iter->csum(계산된 CRC16)을 사용.
 * @pi: 기록 대상 t10_pi_tuple.
 * @return: 없음(void).
 *
 * t10_pi_tuple.guard_tag가 __be16이므로, 네이티브 u16으로 들고 있는
 * iter->csum의 하위 16비트를 cpu_to_be16()으로 빅엔디안 변환한 뒤
 * blk_set_pi()에 위임한다.
 * 실행 컨텍스트: 호출자(blk_integrity_set())와 동일.
 * 호출자(caller): blk_integrity_set() - csum_type==CRC일 때.
 * 호출되는 함수(callee): cpu_to_be16(), blk_set_pi().
 * 에러 처리: 없음.
 *
 * 호출 체인:
 *   blk_integrity_set -> [blk_set_t10_pi] -> blk_set_pi
 */
static void blk_set_t10_pi(struct blk_integrity_iter *iter,
			   struct t10_pi_tuple *pi)
{
	/* [한국어] iter->csum(u64)의 하위 16비트를 u16으로 잘라 cpu_to_be16()으로
	 * 빅엔디안 변환한 뒤 공통 기록 로직에 위임 */
	blk_set_pi(iter, pi, cpu_to_be16((u16)iter->csum));
}

/*
 * [한국어]
 * blk_set_ip_pi - IP 체크섬 csum_type용 Guard 변환 + 공통 기록 위임
 *
 * @iter: 현재 interval 반복자 - iter->csum(fold까지 끝난 IP 체크섬)을 사용.
 * @pi: 기록 대상 t10_pi_tuple.
 * @return: 없음(void).
 *
 * IP 체크섬은 이미 blk_integrity_csum_finish()에서 fold를 마쳐 네이티브
 * u16 값으로 iter->csum에 들어있으므로, cpu_to_be16() 같은 바이트오더
 * 변환 없이 __force 캐스트로 __be16처럼 "재해석"만 해서 그대로 기록한다 -
 * blk_verify_ip_pi()가 get_unaligned()로 네이티브 그대로 읽는 것과 대칭.
 * 실행 컨텍스트: 호출자(blk_integrity_set())와 동일.
 * 호출자(caller): blk_integrity_set() - csum_type==IP일 때.
 * 호출되는 함수(callee): blk_set_pi().
 * 에러 처리: 없음.
 *
 * 호출 체인:
 *   blk_integrity_set -> [blk_set_ip_pi] -> blk_set_pi
 */
static void blk_set_ip_pi(struct blk_integrity_iter *iter,
			  struct t10_pi_tuple *pi)
{
	/* [한국어] iter->csum의 하위 16비트를 __force 캐스트로 __be16처럼
	 * 재해석(실제 바이트오더 변환은 하지 않음 - IP 체크섬은 네이티브
	 * 표현을 그대로 저장하는 관례) 후 공통 기록 로직에 위임 */
	blk_set_pi(iter, pi, (__force __be16)(u16)iter->csum);
}

/*
 * [한국어]
 * blk_integrity_set - csum_type에 맞는 기록(WRITE 생성) 함수로 디스패치
 *
 * @iter: 현재 interval 반복자 - iter->bi->csum_type으로 알고리즘 선택.
 * @tuple: 기록 대상 union pi_tuple.
 * @return: 없음(void).
 *
 * blk_integrity_verify()의 기록(WRITE) 버전 대응 함수 - csum_type에 따라
 * blk_set_ext_pi/blk_set_t10_pi/blk_set_ip_pi 중 하나로 위임한다. 검증
 * 함수와 달리 default 분기에서는 WARN_ON_ONCE로 경고를 남기는데(검증
 * 쪽은 방어적으로 통과), 이는 WRITE 시 잘못된 프로파일로 tuple을 아예
 * 채우지 못하면(default가 아무 것도 쓰지 않음) 이후 READ에서 무조건
 * 검증 실패가 나기 때문에 더 적극적으로 문제를 알린다.
 * 실행 컨텍스트: 호출자(blk_integrity_interval())와 동일 - WRITE 제출
 * 경로의 프로세스 컨텍스트.
 * 호출자(caller): blk_integrity_interval() - verify==false일 때.
 * 호출되는 함수(callee): blk_set_ext_pi(), blk_set_t10_pi(), blk_set_ip_pi().
 * 에러 처리: 알 수 없는 csum_type은 WARN_ON_ONCE만 남기고 tuple을 채우지
 * 않은 채 반환 - 반환형이 void라 호출자에게 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   blk_integrity_interval -> [blk_integrity_set] -> blk_set_ext_pi /
 *   blk_set_t10_pi / blk_set_ip_pi
 */
static void blk_integrity_set(struct blk_integrity_iter *iter,
			      union pi_tuple *tuple)
{
	/* [한국어] csum_type에 따라 tuple의 어느 멤버를 어떤 함수로 채울지 분기 */
	switch (iter->bi->csum_type) {
	/* [한국어] NVMe CRC64 확장 PI 기록 경로 */
	case BLK_INTEGRITY_CSUM_CRC64:
		/* [한국어] union의 crc64_pi 멤버 주소를 넘겨 확장 PI 기록.
		 * return을 써서 void 함수 호출과 동시에 이 함수도 종료(관용구) */
		return blk_set_ext_pi(iter, &tuple->crc64_pi);
	/* [한국어] 표준 T10 DIF CRC16 기록 경로 */
	case BLK_INTEGRITY_CSUM_CRC:
		/* [한국어] union의 t10_pi 멤버 주소를 넘겨 CRC 포맷 기록 */
		return blk_set_t10_pi(iter, &tuple->t10_pi);
	/* [한국어] IP 체크섬 기록 경로 */
	case BLK_INTEGRITY_CSUM_IP:
		/* [한국어] union의 t10_pi 멤버 주소를 넘겨 IP 포맷 기록 */
		return blk_set_ip_pi(iter, &tuple->t10_pi);
	/* [한국어] 알 수 없는 csum_type - 설정 오류 */
	default:
		/* [한국어] 커널 로그에 1회 경고 - WRITE인데 tuple을 채울 방법이
		 * 없다는 심각한 상황을 알림 */
		WARN_ON_ONCE(1);
		/* [한국어] tuple을 채우지 않은 채 그냥 반환 - 호출자는 이 실패를
		 * 알 수 없으므로 이후 READ 시 guard 불일치로 뒤늦게 발견됨 */
		return;
	}
}

/*
 * [한국어]
 * blk_integrity_interval - 한 보호 interval에 대한 PI tuple을 매핑하고
 * 생성 또는 검증을 수행한 뒤 다음 interval을 위해 반복자 상태를 리셋
 *
 * @iter: 현재 처리 중인 반복자 - iter->prot_iter(메타데이터 진행 위치),
 *        iter->csum(누적 체크섬), iter->seed(ref tag 기준값),
 *        iter->interval_remaining(다음 interval 크기)을 모두 갱신한다.
 * @verify: true = READ 검증(blk_integrity_verify() 경로), false = WRITE
 *          생성(blk_integrity_set() 경로).
 * @return: verify==true일 때 blk_integrity_verify()의 결과(BLK_STS_OK/
 *          BLK_STS_PROTECTION), verify==false일 때는 항상 BLK_STS_OK
 *          (생성은 실패하지 않음).
 *
 * blk_integrity_iterate()의 바깥 루프가 데이터 쪽에서 한 interval 분량을
 * 다 소비했을 때(interval_remaining==0) 호출되는 "interval 경계" 처리
 * 함수다. 먼저 blk_integrity_csum_offset()으로 metadata padding을 건너뛰며
 * 필요하면 체크섬에 반영한다. 그 다음 현재 prot_iter 위치의 bio_vec 조각이
 * PI tuple 전체(pi_tuple_size바이트)를 담을 만큼 충분히 크면(pbv.bv_len >=
 * pi_tuple_size) kmap_local()로 직접 매핑해 포인터(ptuple)로 쓰고, 그렇지
 * 않으면(tuple이 두 bio_vec에 걸쳐 있는 드문 경우) verify 시에만
 * blk_integrity_copy_to_tuple()로 스택 스크래치 tuple에 모아 온다(WRITE
 * 시에는 어차피 새로 채울 것이므로 미리 읽어올 필요가 없다 - 스택 tuple이
 * 이미 미초기화 상태로 존재). 이어서 verify 플래그에 따라
 * blk_integrity_verify() 또는 blk_integrity_set()을 호출해 실제 검증/생성을
 * 수행한다. 마지막으로 ptuple이 kmap된 실제 버퍼를 가리켰다면(가장 흔한
 * 경우) kunmap_local()로 해제하고, 스택 스크래치를 썼다면(!verify, 즉 WRITE)
 * blk_integrity_copy_from_tuple()로 방금 채운 스크래치 내용을 실제 메타데이터
 * 버퍼에 되돌려 쓴다. 함수 끝에서 interval_remaining을 다시 한 interval
 * 크기로 리셋하고, csum을 0으로 초기화하며, seed를 1 증가시켜 다음 interval을
 * 준비한다.
 * 실행 컨텍스트: 호출자(blk_integrity_iterate())와 동일.
 * 호출자(caller): blk_integrity_iterate() - 매 interval 경계마다.
 * 호출되는 함수(callee): blk_integrity_csum_offset(), bvec_iter_bvec(),
 * bvec_kmap_local(), bvec_iter_advance_single(), blk_integrity_copy_to_tuple(),
 * blk_integrity_verify(), blk_integrity_set(), kunmap_local(),
 * blk_integrity_copy_from_tuple().
 * 에러 처리: verify==true일 때 blk_integrity_verify()가 반환한
 * BLK_STS_PROTECTION을 ret에 담아 그대로 반환 - 이 반환값을 받은
 * blk_integrity_iterate()의 바깥 루프는 ret != BLK_STS_OK 조건으로 즉시
 * 순회를 중단한다.
 *
 * 호출 체인:
 *   blk_integrity_iterate -> [blk_integrity_interval] -> blk_integrity_csum_offset
 *   / blk_integrity_verify / blk_integrity_set
 */
static blk_status_t blk_integrity_interval(struct blk_integrity_iter *iter,
					   bool verify)
{
	/* [한국어] 이번 interval의 처리 결과 - 기본값 OK, verify 실패 시에만
	 * BLK_STS_PROTECTION으로 덮어써짐 */
	blk_status_t ret = BLK_STS_OK;
	/* [한국어] PI tuple이 두 bio_vec에 걸쳐 있을 때 쓸 스택 스크래치 공간 -
	 * 보통은 사용되지 않고 kmap된 실제 버퍼를 직접 쓴다 */
	union pi_tuple tuple;
	/* [한국어] 실제로 tuple 데이터를 담을 포인터 - 기본값은 스택 tuple의
	 * 주소이지만, 아래에서 실제 버퍼를 직접 kmap할 수 있으면 그 주소로
	 * 교체된다 */
	void *ptuple = &tuple;
	/* [한국어] 현재 prot_iter 위치에 해당하는 메타데이터 bio_vec 조각을
	 * 담을 지역 변수 */
	struct bio_vec pbv;

	/* [한국어] 이번 interval 처리에 앞서, metadata 안의 padding 영역을
	 * 건너뛰고(포맷에 따라) 체크섬에 반영 - prot_iter가 이 함수 호출 후
	 * PI tuple의 실제 시작 위치를 가리키게 됨 */
	blk_integrity_csum_offset(iter);
	/* [한국어] padding을 건너뛴 뒤 prot_iter가 가리키는 현재 bio_vec
	 * 조각(페이지+오프셋+길이)을 계산 - 아직 매핑은 하지 않음 */
	pbv = bvec_iter_bvec(iter->bip->bip_vec, iter->prot_iter);
	/* [한국어] 이 bio_vec 조각 하나가 PI tuple 전체(pi_tuple_size바이트)를
	 * 담을 만큼 충분히 길면(페이지 경계에 걸치지 않는 일반적인 경우) */
	if (pbv.bv_len >= iter->bi->pi_tuple_size) {
		/* [한국어] 실제 메타데이터 버퍼 페이지를 직접 kmap해 ptuple이
		 * 그 주소를 가리키게 함 - 복사 없이 바로 읽고/쓸 수 있어 효율적 */
		ptuple = bvec_kmap_local(&pbv);
		/* [한국어] prot_iter를 metadata_size - pi_offset만큼(즉 padding을
		 * 뺀, PI tuple을 포함한 나머지 전체 메타데이터 길이만큼) 전진 -
		 * pi_tuple_size가 아니라 metadata_size 기준인 이유는 tuple 뒤에
		 * 남는 추가 메타데이터(있다면)까지 다음 interval 이전에 건너뛰기
		 * 위해서다 */
		bvec_iter_advance_single(iter->bip->bip_vec, &iter->prot_iter,
				iter->bi->metadata_size - iter->bi->pi_offset);
	/* [한국어] bio_vec 조각이 tuple 전체를 담기엔 부족한 경우(페이지 경계에
	 * 걸침) - verify(READ)일 때만 기존 값을 읽어와야 하므로 스택 tuple로
	 * 복사해 모은다 */
	} else if (verify) {
		/* [한국어] 여러 bio_vec에 흩어진 실제 PI tuple 바이트를 스택
		 * 스크래치(ptuple, 초기값 &tuple)로 모아 옴 - 이후 blk_integrity_verify가
		 * 이 스크래치를 tuple처럼 읽는다 */
		blk_integrity_copy_to_tuple(iter->bip, &iter->prot_iter,
				ptuple, iter->bi->pi_tuple_size);
	}

	/* [한국어] verify 플래그에 따라 검증 또는 생성 중 하나만 수행 */
	if (verify)
		/* [한국어] READ 검증: ptuple(실제 버퍼 또는 스크래치 복사본)에
		 * 저장된 값과 iter->csum(재계산된 guard)/iter->seed(기대 ref)를
		 * 비교한 결과를 ret에 저장 */
		ret = blk_integrity_verify(iter, ptuple);
	else
		/* [한국어] WRITE 생성: ptuple 위치에 새 guard/app/ref 값을 기록 -
		 * 반환값이 없으므로(void) ret은 BLK_STS_OK로 유지됨 */
		blk_integrity_set(iter, ptuple);

	/* [한국어] ptuple이 스택 tuple의 주소가 아니라면(likely - 대부분 실제
	 * 버퍼를 직접 kmap한 경우), 그 kmap을 해제 */
	if (likely(ptuple != &tuple)) {
		/* [한국어] 실제 메타데이터 페이지 매핑 해제 - 이미 그 버퍼에
		 * 직접 쓰거나 읽었으므로 별도 복사가 필요 없음 */
		kunmap_local(ptuple);
	/* [한국어] ptuple이 스택 tuple 그대로였고(페이지 경계에 걸쳐 직접 매핑
	 * 못한 경우), 이번이 WRITE(!verify)라면 */
	} else if (!verify) {
		/* [한국어] 방금 스택 tuple에 새로 채운 guard/app/ref 값을 실제
		 * 여러 bio_vec에 걸친 메타데이터 버퍼로 되돌려 씀 - verify(READ)
		 * 였다면 이미 다 읽은 값이라 되돌려 쓸 필요가 없으므로 이 분기를
		 * 타지 않음 */
		blk_integrity_copy_from_tuple(iter->bip, &iter->prot_iter,
				ptuple, iter->bi->pi_tuple_size);
	}

	/* [한국어] 다음 interval을 위해 남은 바이트 카운터를 한 interval
	 * 크기(1 << interval_exp)로 리셋 */
	iter->interval_remaining = 1 << iter->bi->interval_exp;
	/* [한국어] 다음 interval의 체크섬 누적을 0부터 새로 시작하도록 리셋 */
	iter->csum = 0;
	/* [한국어] ref tag 기준값을 1 증가 - 다음 interval은 LBA가 1 논리
	 * 블록만큼 전진한 데이터이므로 */
	iter->seed++;
	/* [한국어] 이번 interval의 최종 결과(OK 또는 PROTECTION)를 바깥
	 * 루프(blk_integrity_iterate)에 반환 */
	return ret;
}

/*
 * [한국어]
 * blk_integrity_iterate - bio의 데이터 영역 전체를 훑으며 interval마다
 * PI 생성/검증을 수행하는 이 파일의 핵심 순회 엔진
 *
 * @bio: 대상 bio - 디스크의 blk_integrity 프로파일과 bio_integrity_payload를
 *       이 bio에서 얻는다.
 * @data_iter: 순회를 시작할 데이터 영역의 bvec_iter - bio_integrity_generate()는
 *       &bio->bi_iter(bio 전체)를, bio_integrity_verify()는 완료 시점에 저장해
 *       둔 saved_iter(원래 제출 시점의 iter, 이미 진행된 iter가 아님)를 넘긴다.
 * @verify: true = 검증(READ 완료), false = 생성(WRITE 제출 전).
 * @return: BLK_STS_OK = 전체 bio에 대해 성공(또는 생성은 항상 성공),
 *          BLK_STS_PROTECTION = 어느 interval에서든 검증 실패가 발생하면
 *          그 시점에서 즉시 반환.
 *
 * struct blk_integrity_iter를 스택에 초기화(디스크 프로파일 bi, payload bip,
 * 데이터/메타데이터 iterator, 초기 interval 크기, 초기 seed=시작 섹터, csum=0)한
 * 뒤, 데이터가 남아있는 동안(iter.data_iter.bi_size) 바깥 while 루프를 돈다.
 * 바깥 루프는 한 번에 bio_vec 조각 하나(kaddr로 kmap)를 통째로 매핑하고,
 * 안쪽 while 루프가 그 조각 안에서 다시 "현재 interval에서 남은 바이트"와
 * "이 bio_vec 조각에서 남은 바이트" 중 작은 쪽(len)만큼씩 반복해서
 * blk_calculate_guard()를 호출한다. interval_remaining이 정확히 0이 되는
 * 순간(interval 경계에 도달) blk_integrity_interval()을 호출해 그 interval의
 * PI tuple 처리를 완료하고, 반환값이 BLK_STS_OK가 아니면(검증 실패) 두
 * while 루프의 조건(ret == BLK_STS_OK)이 모두 거짓이 되어 즉시 순회 전체가
 * 중단된다 - 이후 남은 데이터는 검사하지 않고 첫 실패만 보고한다. 이런
 * 이중 루프 구조가 필요한 이유는 "bio_vec 조각의 경계"와 "PI interval의
 * 경계"가 일반적으로 일치하지 않기 때문이다(하나의 bio_vec이 여러 interval을
 * 포함할 수도, 하나의 interval이 여러 bio_vec에 걸칠 수도 있음).
 * 실행 컨텍스트: 호출자(bio_integrity_generate/verify())와 동일 - WRITE
 * 생성은 제출자 프로세스 컨텍스트, READ 검증은 kintegrityd 워커 또는
 * 파일시스템 직접 검증 경로의 프로세스 컨텍스트.
 * 호출자(caller): bio_integrity_generate(), bio_integrity_verify().
 * 호출되는 함수(callee): blk_get_integrity(), bio_integrity(),
 * bvec_iter_bvec(), bvec_kmap_local(), bvec_iter_advance_single(),
 * blk_calculate_guard(), blk_integrity_interval(), kunmap_local().
 * 에러 처리: blk_integrity_interval()이 반환한 BLK_STS_PROTECTION이 두
 * while 루프의 종료 조건에 즉시 반영되어, 나머지 인터벌은 처리하지 않고
 * 함수가 그 값을 그대로 반환한다.
 *
 * 호출 체인:
 *   bio_integrity_generate / bio_integrity_verify -> [blk_integrity_iterate]
 *   -> blk_calculate_guard / blk_integrity_interval
 */
static blk_status_t blk_integrity_iterate(struct bio *bio,
					  struct bvec_iter *data_iter,
					  bool verify)
{
	/* [한국어] 이 bio가 속한 디스크의 무결성 프로파일(csum_type/interval_exp/
	 * pi_offset/metadata_size/pi_tuple_size/flags)을 조회 */
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);
	/* [한국어] 이 bio에 붙은 PI 메타데이터 payload(guard/app/ref tuple이
	 * 담긴 bio_vec 배열의 소유자)를 조회 */
	struct bio_integrity_payload *bip = bio_integrity(bio);
	/* [한국어] 이번 순회 전체에 쓸 반복자를 스택에서 지정 초기화(designated
	 * initializer) - C99 스타일로 각 필드를 명시적으로 채움 */
	struct blk_integrity_iter iter = {
		/* [한국어] 에러 로그용 원본 bio 저장 */
		.bio = bio,
		/* [한국어] PI 메타데이터 payload 저장 */
		.bip = bip,
		/* [한국어] 디스크 무결성 프로파일 저장 */
		.bi = bi,
		/* [한국어] 호출자가 넘긴 데이터 iterator를 값으로 복사 - 원본
		 * *data_iter 자체는 건드리지 않고 이 지역 복사본만 전진시킴 */
		.data_iter = *data_iter,
		/* [한국어] 메타데이터 iterator는 bip->bip_iter(현재 payload의
		 * 진행 위치)로 초기화 - generate 시 최초 호출이면 처음부터,
		 * verify 시에도 매 bio마다 처음부터 훑음 */
		.prot_iter = bip->bip_iter,
		/* [한국어] 첫 interval의 크기를 1 << interval_exp(예: 512B,
		 * 2^9)로 설정 */
		.interval_remaining = 1 << bi->interval_exp,
		/* [한국어] ref tag 기준값(seed)의 초기값 - 호출자가 넘긴
		 * data_iter의 시작 섹터(bi_sector) */
		.seed = data_iter->bi_sector,
		/* [한국어] 체크섬 누적값 초기화 - 아직 아무 데이터도 반영 안 됨 */
		.csum = 0,
	};
	/* [한국어] 전체 순회 결과 - 기본 OK, interval 처리 실패 시 갱신됨 */
	blk_status_t ret = BLK_STS_OK;

	/* [한국어] 바깥 루프: 데이터가 남아있고(bi_size != 0) 지금까지 성공한
	 * 동안 계속 - 실패하면 즉시 종료해 나머지 데이터를 건너뜀 */
	while (iter.data_iter.bi_size && ret == BLK_STS_OK) {
		/* [한국어] 현재 data_iter 위치의 bio_vec 조각(페이지+오프셋+길이)을
		 * 계산 */
		struct bio_vec bv = bvec_iter_bvec(iter.bio->bi_io_vec,
						   iter.data_iter);
		/* [한국어] 이 데이터 페이지를 커널 가상 주소로 임시 매핑 -
		 * 이후 이 조각 안에서 여러 interval 경계를 넘나들며 반복 접근 */
		void *kaddr = bvec_kmap_local(&bv);
		/* [한국어] 실제로 전진시킬 작업용 포인터 - kaddr(매핑 시작
		 * 주소)는 kunmap 시 필요하므로 그대로 두고 별도 변수로 이동 */
		void *data = kaddr;
		/* [한국어] 안쪽 루프에서 이번에 처리할 길이를 담을 변수 */
		unsigned int len;

		/* [한국어] data_iter를 이 bio_vec 조각의 전체 길이(bv.bv_len)만큼
		 * 미리 전진 - 실제 바이트 처리는 아래에서 bv.bv_len을 로컬로
		 * 감소시키며 수행하지만, 반복자 자체는 다음 조각을 가리키도록
		 * 여기서 한 번에 이동해 둠(단순화를 위해 원본 bv.bv_len을
		 * 캡처해 둔 뒤 사용) */
		bvec_iter_advance_single(iter.bio->bi_io_vec, &iter.data_iter,
					 bv.bv_len);
		/* [한국어] 안쪽 루프: 이 bio_vec 조각 안에 처리할 바이트가
		 * 남아있고 지금까지 성공한 동안 계속 - "interval 경계"와
		 * "bio_vec 조각 경계"가 다르므로 이중 루프가 필요 */
		while (bv.bv_len && ret == BLK_STS_OK) {
			/* [한국어] 이번에 처리할 길이 = 현재 interval에서 남은
			 * 바이트 수와 이 조각에서 남은 바이트 수 중 작은 쪽 -
			 * interval 경계와 bio_vec 경계 중 먼저 오는 쪽까지만 처리 */
			len = min(iter.interval_remaining, bv.bv_len);
			/* [한국어] len바이트만큼 체크섬을 누적 계산 */
			blk_calculate_guard(&iter, data, len);
			/* [한국어] 이 조각에서 len바이트를 소비했으므로 로컬
			 * 길이 카운터 감소(위에서 iter.data_iter는 이미 조각
			 * 전체만큼 전진해 뒀으므로, 이 bv.bv_len은 순수하게 이
			 * 루프의 반복 종료 조건 판단용) */
			bv.bv_len -= len;
			/* [한국어] 다음 반복을 위해 데이터 포인터를 len만큼 전진 */
			data += len;
			/* [한국어] 현재 interval에서 남은 바이트 수 감소 */
			iter.interval_remaining -= len;
			/* [한국어] interval_remaining이 정확히 0이 되었는지 확인 -
			 * 즉 한 interval 전체(예: 512바이트)의 체크섬 계산이
			 * 방금 끝났는지 판정 */
			if (!iter.interval_remaining)
				/* [한국어] interval 경계 도달 - 이 interval의
				 * PI tuple을 실제로 생성/검증하고, 결과를 ret에
				 * 반영(실패 시 두 while 루프 모두 다음 반복에서
				 * 종료됨) */
				ret = blk_integrity_interval(&iter, verify);
		}
		/* [한국어] 이 bio_vec 조각에 대한 매핑 해제 - 다음 바깥 루프
		 * 반복에서 새 조각을 매핑하기 전에 반드시 반납 */
		kunmap_local(kaddr);
	}

	/* [한국어] 마지막 interval까지의 처리 결과(또는 도중 실패한 결과)를
	 * 호출자(bio_integrity_generate/verify)에 반환 */
	return ret;
}

/*
 * [한국어]
 * bio_integrity_generate - WRITE bio에 대해 PI(Guard/AppTag/RefTag)를 생성해
 * 메타데이터 버퍼에 채우는 이 파일의 공개 진입점
 *
 * @bio: PI를 생성할 대상 bio - 이미 bio_integrity_payload가 붙어 있어야
 *       한다(bip가 없으면 이 함수 이전 단계에서 호출 자체가 걸러진다).
 * @return: 없음(void) - 생성은 실패라는 개념이 없다(항상 로컬 계산이므로).
 *
 * 디스크 프로파일의 csum_type이 이 파일이 지원하는 세 알고리즘(CRC64/CRC/IP)
 * 중 하나일 때만 실제 작업(blk_integrity_iterate(bio, &bio->bi_iter, false))을
 * 수행하고, 그 외(BLK_INTEGRITY_CSUM_NONE 등)에는 아무 것도 하지 않는다.
 * 이 함수는 block/bio-integrity-fs.c의 fs_bio_integrity_generate()(파일시스템이
 * 스스로 PI 버퍼를 준비한 경우)와 block/bio-integrity-auto.c의
 * bio_integrity_prep()(블록 계층이 대신 준비하는 자동 경로) 양쪽에서 모두
 * 재사용되는 공통 하부 엔진이다.
 * 실행 컨텍스트: 제출자의 프로세스 컨텍스트(submit_bio() 호출 스레드) -
 * 아직 드라이버에 요청이 전달되기 전 단계에서 실행된다.
 * 호출자(caller): fs_bio_integrity_generate()(block/bio-integrity-fs.c),
 * bio_integrity_prep()(block/bio-integrity-auto.c, WRITE 경로).
 * 호출되는 함수(callee): blk_get_integrity(), blk_integrity_iterate().
 * 에러 처리: 없음 - 반환형 자체가 void.
 *
 * 호출 체인:
 *   fs_bio_integrity_generate / bio_integrity_prep -> [bio_integrity_generate]
 *   -> blk_integrity_iterate
 */
void bio_integrity_generate(struct bio *bio)
{
	/* [한국어] 이 bio가 속한 디스크의 무결성 프로파일을 조회 - csum_type을
	 * 확인해 이 파일이 지원하는 알고리즘인지 판단하기 위함 */
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);

	/* [한국어] 지원하는 csum_type인지에 따라 분기 - 세 알고리즘 모두 동일한
	 * blk_integrity_iterate() 호출로 이어지므로 case를 나열해 fall-through */
	switch (bi->csum_type) {
	/* [한국어] NVMe CRC64 확장 PI - 아래 CRC/IP와 함께 동일 처리로 폴스루 */
	case BLK_INTEGRITY_CSUM_CRC64:
	/* [한국어] 표준 T10 DIF CRC16 - 아래로 폴스루 */
	case BLK_INTEGRITY_CSUM_CRC:
	/* [한국어] IP 체크섬 - 세 case 모두 이 한 줄로 수렴 */
	case BLK_INTEGRITY_CSUM_IP:
		/* [한국어] bio 전체(&bio->bi_iter)를 데이터 시작점으로 삼아
		 * WRITE 생성 모드(verify=false)로 순회 시작 */
		blk_integrity_iterate(bio, &bio->bi_iter, false);
		/* [한국어] switch 종료 */
		break;
	/* [한국어] csum_type이 NONE 등 이 파일이 다루지 않는 값이면 */
	default:
		/* [한국어] 아무 처리도 하지 않음 - PI가 없는 디스크는 애초에
		 * 생성할 태그가 없음 */
		break;
	}
}

/*
 * [한국어]
 * bio_integrity_verify - READ 완료된 bio의 PI(Guard/AppTag/RefTag)를 검증하는
 * 이 파일의 공개 진입점
 *
 * @bio: 검증할 완료된 bio.
 * @saved_iter: 검증에 사용할 데이터 영역의 원래 bvec_iter - READ 완료 시점의
 *        bio->bi_iter는 이미 데이터를 다 소비해 전진된 상태이므로, 호출자가
 *        제출 시점(또는 재초기화한) iterator를 별도로 보관해 두었다가 넘겨줌.
 * @return: blk_integrity_iterate()의 결과를 그대로 전달 - BLK_STS_OK(검증
 *        통과) 또는 BLK_STS_PROTECTION(무결성 위반), csum_type이 지원 대상이
 *        아니면 무조건 BLK_STS_OK.
 *
 * bio_integrity_generate()의 검증(READ) 버전 대응 함수 - 디스크 프로파일의
 * csum_type이 유효할 때만 blk_integrity_iterate(bio, saved_iter, true)를
 * 호출해 실제 비교를 수행하고 그 결과를 그대로 반환하며, 유효하지 않으면
 * (PI 없는 디스크) 무조건 통과(BLK_STS_OK)로 처리한다.
 * 실행 컨텍스트: 호출자에 따라 다름 - block/bio-integrity-auto.c의
 * bio_integrity_verify_fn()에서 호출될 때는 kintegrityd 워커 스레드의
 * 프로세스 컨텍스트(인터럽트 컨텍스트에서 무거운 CRC 계산을 피하기 위해
 * 지연 실행), block/bio-integrity-fs.c의 fs_bio_integrity_verify()에서
 * 호출될 때는 파일시스템의 완료 콜백을 호출한 스레드의 컨텍스트.
 * 호출자(caller): bio_integrity_verify_fn()(block/bio-integrity-auto.c),
 * fs_bio_integrity_verify()(block/bio-integrity-fs.c).
 * 호출되는 함수(callee): blk_get_integrity(), blk_integrity_iterate().
 * 에러 처리: blk_integrity_iterate()가 반환한 BLK_STS_PROTECTION을 그대로
 * 전파 - 호출자가 이를 bio->bi_status 등에 반영해 상위 계층(파일시스템,
 * 사용자 공간)에 I/O 에러로 알린다.
 *
 * 호출 체인:
 *   bio_integrity_verify_fn / fs_bio_integrity_verify -> [bio_integrity_verify]
 *   -> blk_integrity_iterate
 */
blk_status_t bio_integrity_verify(struct bio *bio, struct bvec_iter *saved_iter)
{
	/* [한국어] 이 bio가 속한 디스크의 무결성 프로파일을 조회 */
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);

	/* [한국어] 지원하는 csum_type인지에 따라 분기 */
	switch (bi->csum_type) {
	/* [한국어] NVMe CRC64 확장 PI - 폴스루 */
	case BLK_INTEGRITY_CSUM_CRC64:
	/* [한국어] 표준 T10 DIF CRC16 - 폴스루 */
	case BLK_INTEGRITY_CSUM_CRC:
	/* [한국어] IP 체크섬 - 세 case 모두 이 한 줄로 수렴 */
	case BLK_INTEGRITY_CSUM_IP:
		/* [한국어] saved_iter(제출 시점의 원본 데이터 iterator)를 검증
		 * 모드(verify=true)로 순회하고, 그 결과를 즉시 반환(위의
		 * bio_integrity_generate와 달리 break가 아니라 return -
		 * 반환값이 있는 함수이므로) */
		return blk_integrity_iterate(bio, saved_iter, true);
	/* [한국어] csum_type이 NONE 등 이 파일이 다루지 않는 값이면 */
	default:
		/* [한국어] switch를 그냥 빠져나감 - 아래에서 BLK_STS_OK를 반환 */
		break;
	}

	/* [한국어] PI가 없는 디스크는 검증할 대상이 없으므로 무조건 통과 처리 */
	return BLK_STS_OK;
}

/*
 * Advance @iter past the protection offset for protection formats that
 * contain front padding on the metadata region.
 */
/*
 * [한국어]
 * blk_pi_advance_offset - remap 대상 iterator를 metadata 앞쪽 padding만큼
 * 전진시켜 실제 PI tuple 시작 위치로 이동
 *
 * @bi: 디스크 무결성 프로파일 - bi->pi_offset(건너뛸 padding 바이트 수)을 사용.
 * @bip: 무결성 payload - bip->bip_vec에서 실제 bio_vec 조각을 얻는다.
 * @iter: 전진시킬 메타데이터 bvec_iter(입출력 겸용).
 * @return: 없음(void).
 *
 * blk_integrity_csum_offset()과 목적은 비슷하지만(둘 다 pi_offset만큼
 * 건너뜀), 이 함수는 remap 경로 전용이라 체크섬 계산을 하지 않고 순수하게
 * iterator만 전진시킨다는 점이 다르다 - ref tag remap은 guard/csum을 전혀
 * 건드리지 않으므로 padding 영역의 내용을 읽을 필요조차 없다. mp_bvec_iter_bvec()
 * (멀티페이지 버전)을 쓰는 것도 특징인데, 이는 이 함수가 실제 데이터를
 * 읽지 않고 길이 계산만 하므로 단일 페이지 매핑 제약(bvec_iter_bvec())이
 * 필요 없기 때문이다.
 * 실행 컨텍스트: 호출자(blk_tuple_remap_begin())와 동일 - blk_integrity_prepare()
 * (제출 경로) 또는 blk_integrity_complete()(완료 인터럽트/softirq 경로)에서
 * 파생된 컨텍스트.
 * 호출자(caller): blk_tuple_remap_begin().
 * 호출되는 함수(callee): mp_bvec_iter_bvec(), bvec_iter_advance_single().
 * 에러 처리: 없음.
 *
 * 호출 체인:
 *   blk_tuple_remap_begin -> [blk_pi_advance_offset] -> bvec_iter_advance_single()
 */
static void blk_pi_advance_offset(struct blk_integrity *bi,
				  struct bio_integrity_payload *bip,
				  struct bvec_iter *iter)
{
	/* [한국어] 건너뛸 padding 바이트 수 - 대부분의 표준 포맷에서는 0 */
	unsigned int offset = bi->pi_offset;

	/* [한국어] offset이 남아있는 동안 padding 영역을 한 bvec 조각씩 건너뜀 */
	while (offset > 0) {
		/* [한국어] 현재 iter 위치의 bio_vec 조각 계산 - 멀티페이지
		 * 버전(mp_bvec_iter_bvec)을 사용해 페이지 경계 제약 없이 길이
		 * 정보만 얻음(실제 kmap하지 않으므로 단일 페이지 제한이 필요 없음) */
		struct bio_vec bv = mp_bvec_iter_bvec(bip->bip_vec, *iter);
		/* [한국어] 이번에 건너뛸 길이 = 남은 offset과 현재 조각 길이
		 * 중 작은 쪽 */
		unsigned int len = min(bv.bv_len, offset);

		/* [한국어] iter를 len만큼 전진 - 실제 데이터 접근 없이 위치만 이동 */
		bvec_iter_advance_single(bip->bip_vec, iter, len);
		/* [한국어] 남은 offset 감소 */
		offset -= len;
	}
}

/*
 * [한국어]
 * blk_tuple_remap_begin - remap할 PI tuple 하나를 매핑(또는 스크래치로 모아
 * 옴)해 그 포인터를 반환 - blk_integrity_interval()의 remap 버전에 해당
 *
 * @tuple: bio_vec 경계에 걸려 직접 매핑할 수 없을 때 쓸 스택 스크래치 버퍼.
 * @bi: 디스크 무결성 프로파일 - pi_offset/pi_tuple_size를 사용.
 * @bip: 무결성 payload.
 * @iter: 메타데이터 bvec_iter(입출력 겸용) - padding을 건너뛴 뒤의 위치를
 *        가리키도록 전진된다.
 * @return: 실제 매체 버퍼를 직접 가리키는 kmap 포인터(가장 흔한 경우),
 *          또는 tuple(스택 스크래치, bio_vec 경계에 걸린 드문 경우).
 *
 * 먼저 blk_pi_advance_offset()으로 padding을 건너뛴 뒤, 그 위치의 bio_vec
 * 조각이 tuple 전체를 담을 만큼 크면(likely) 바로 kmap_local()해서 반환한다 -
 * 이 경우 이후 blk_tuple_remap_end()가 이 포인터가 tuple이 아님을 보고 단순
 * kunmap만 하면 된다. 그렇지 않으면(페이지 경계에 걸림) 원본 iter를 건드리지
 * 않기 위해 임시 titer 사본을 만들어 blk_integrity_copy_to_tuple()로 기존
 * 값을 스택 tuple에 모아 온 뒤 그 주소를 반환한다 - remap은 ref_tag "일부만"
 * 수정하고 나머지(guard 등)는 그대로 보존해야 하므로, WRITE 생성과 달리
 * 반드시 기존 값을 먼저 읽어와야 한다(그래서 blk_integrity_interval()의
 * WRITE 분기와 달리 여기서는 무조건 copy_to_tuple을 수행 - verify 조건
 * 분기가 없음).
 * 실행 컨텍스트: 호출자(__blk_reftag_remap())와 동일.
 * 호출자(caller): __blk_reftag_remap() - 매 interval마다.
 * 호출되는 함수(callee): blk_pi_advance_offset(), bvec_iter_bvec(),
 * bvec_kmap_local(), blk_integrity_copy_to_tuple().
 * 에러 처리: 없음.
 *
 * 호출 체인:
 *   __blk_reftag_remap -> [blk_tuple_remap_begin] -> blk_pi_advance_offset /
 *   blk_integrity_copy_to_tuple
 */
static void *blk_tuple_remap_begin(union pi_tuple *tuple,
				   struct blk_integrity *bi,
				   struct bio_integrity_payload *bip,
				   struct bvec_iter *iter)
{
	/* [한국어] 원본 iter를 보존하기 위한 임시 사본 - 페이지 경계에 걸린
	 * 경우에만 사용 */
	struct bvec_iter titer;
	/* [한국어] 현재 iter 위치의 bio_vec 조각을 담을 지역 변수 */
	struct bio_vec pbv;

	/* [한국어] metadata 앞쪽 padding을 건너뛰어 iter가 실제 PI tuple의
	 * 시작 위치를 가리키게 함 */
	blk_pi_advance_offset(bi, bip, iter);
	/* [한국어] padding을 건너뛴 뒤 iter가 가리키는 bio_vec 조각을 계산 */
	pbv = bvec_iter_bvec(bip->bip_vec, *iter);
	/* [한국어] 이 조각 하나가 tuple 전체(pi_tuple_size)를 담을 만큼
	 * 충분히 길면(일반적인 경우) */
	if (likely(pbv.bv_len >= bi->pi_tuple_size))
		/* [한국어] 실제 매체 버퍼 페이지를 직접 kmap해 그 포인터를
		 * 그대로 반환 - 복사 없이 바로 읽고 쓸 수 있음 */
		return bvec_kmap_local(&pbv);

	/*
	 * We need to preserve the state of the original iter for the
	 * copy_from_tuple at the end, so make a temp iter for here.
	 */
	/* [한국어] 원본 iter를 여기서 그대로 전진시켜 버리면, 이 함수가
	 * 반환한 뒤 blk_tuple_remap_end()가 "tuple을 다시 써넣기 위해" 같은
	 * 위치에서 다시 읽어야 할 iter가 이미 앞으로 가버려 위치가 어긋난다.
	 * 그래서 titer라는 임시 사본만 전진시켜 이번 읽기에 사용하고, 원본
	 * iter는 blk_tuple_remap_end()가 쓸 수 있도록 그대로 보존한다 */
	/* [한국어] 원본 iter를 그대로 복사(대입) - 이 복사본만 아래에서 전진됨 */
	titer = *iter;
	/* [한국어] 임시 사본(titer)을 이용해 여러 bio_vec에 흩어진 기존 PI
	 * tuple 값을 스택 tuple로 모아 옴 - remap은 이 기존 값(특히 guard,
	 * app tag)을 보존한 채 ref_tag만 바꿔야 하므로 반드시 먼저 읽어야 함 */
	blk_integrity_copy_to_tuple(bip, &titer, tuple, bi->pi_tuple_size);
	/* [한국어] 스택 tuple의 주소를 반환 - 호출자는 이 값이 &tuple과
	 * 같은지 비교해(blk_tuple_remap_end에서) kmap 여부를 판단 */
	return tuple;
}

/*
 * [한국어]
 * blk_tuple_remap_end - blk_tuple_remap_begin()이 매핑/수집한 tuple의 뒷정리 -
 * kmap 해제 또는 수정된 스크래치를 실제 버퍼에 반영하고 iter를 다음
 * interval로 전진
 *
 * @tuple: blk_tuple_remap_begin()에 넘겼던 스택 스크래치 버퍼의 주소(비교용).
 * @ptuple: blk_tuple_remap_begin()이 반환했던 실제 포인터 - tuple과 같은
 *        주소인지로 "직접 kmap이었는지" 여부를 판별.
 * @bi: 디스크 무결성 프로파일 - metadata_size/pi_offset/pi_tuple_size 사용.
 * @bip: 무결성 payload.
 * @iter: blk_tuple_remap_begin() 호출 후 padding을 건너뛴 상태의 iter -
 *        여기서 tuple 뒤 나머지 길이까지 마저 전진시켜 다음 interval의
 *        시작 위치로 만든다.
 * @return: 없음(void).
 *
 * ptuple이 tuple(스택 스크래치)과 다른 주소라면(likely - 직접 kmap한 경우)
 * 단순히 kunmap_local()로 매핑만 해제하면 된다 - remap 함수
 * (blk_reftag_remap_prepare/complete)가 이미 그 kmap된 메모리에 직접
 * ref_tag를 써 넣었으므로 별도 반영이 필요 없다. 반대로 ptuple이 tuple
 * 그대로라면(페이지 경계에 걸려 스크래치를 썼던 경우) blk_integrity_copy_from_tuple()로
 * 수정된 스크래치 내용을 실제 여러 bio_vec에 되돌려 쓰고, 이미 pi_tuple_size만큼은
 * 그 안에서 전진했으므로 마지막에 iter를 전진시킬 len에서 pi_tuple_size를
 * 빼 이중으로 전진하지 않도록 보정한다. 마지막으로 bvec_iter_advance()로
 * (advance_single이 아닌 일반 advance를 쓰는 이유는 len이 한 bio_vec 조각의
 * 길이를 넘어설 수 있기 때문 - metadata_size - pi_offset은 여러 조각에
 * 걸칠 수 있음) iter를 다음 interval의 metadata 시작 위치로 이동시킨다.
 * 실행 컨텍스트: 호출자(__blk_reftag_remap())와 동일.
 * 호출자(caller): __blk_reftag_remap() - 매 interval마다 remap 완료 후.
 * 호출되는 함수(callee): kunmap_local(), blk_integrity_copy_from_tuple(),
 * bvec_iter_advance().
 * 에러 처리: 없음.
 *
 * 호출 체인:
 *   __blk_reftag_remap -> [blk_tuple_remap_end] -> blk_integrity_copy_from_tuple
 *   / bvec_iter_advance
 */
static void blk_tuple_remap_end(union pi_tuple *tuple, void *ptuple,
				struct blk_integrity *bi,
				struct bio_integrity_payload *bip,
				struct bvec_iter *iter)
{
	/* [한국어] iter를 전진시킬 기본 길이 - metadata_size(전체 메타데이터
	 * 크기)에서 pi_offset(이미 blk_pi_advance_offset이 건너뛴 padding)을
	 * 뺀 나머지(즉 PI tuple + tuple 뒤 여분 메타데이터) */
	unsigned int len = bi->metadata_size - bi->pi_offset;

	/* [한국어] ptuple이 스택 tuple 주소와 다르면(likely) 실제 매체 버퍼를
	 * 직접 kmap했던 경우 */
	if (likely(ptuple != tuple)) {
		/* [한국어] 이미 그 메모리에 직접 ref_tag를 써 넣었으므로 단순히
		 * 매핑만 해제 */
		kunmap_local(ptuple);
	/* [한국어] ptuple이 스택 tuple 그대로였다면(else, 페이지 경계에
	 * 걸렸던 경우) */
	} else {
		/* [한국어] 수정된 스크래치(guard/app은 그대로, ref만 바뀐 tuple)
		 * 내용을 실제 여러 bio_vec에 걸친 메타데이터 버퍼로 되돌려 씀 */
		blk_integrity_copy_from_tuple(bip, iter, ptuple,
				bi->pi_tuple_size);
		/* [한국어] copy_from_tuple 안에서 iter가 이미 pi_tuple_size만큼
		 * 전진했으므로, 아래에서 다시 len(전체 metadata_size-pi_offset)
		 * 만큼 전진시키면 이중 전진이 되어 다음 interval 위치가 어긋난다.
		 * 이를 보정하기 위해 len에서 pi_tuple_size를 미리 빼 둠 */
		len -= bi->pi_tuple_size;
	}

	/* [한국어] 남은 길이(len)만큼 iter를 전진 - advance_single이 아닌
	 * bvec_iter_advance()를 쓰는 이유는 이 길이가 하나의 bio_vec 조각을
	 * 넘어 여러 조각에 걸칠 수 있기 때문(일반 버전은 내부적으로 조각을
	 * 넘나들며 인덱스를 갱신) */
	bvec_iter_advance(bip->bip_vec, iter, len);
}

/*
 * [한국어]
 * blk_set_ext_unmap_ref - CRC64 확장 PI tuple의 ref_tag를 "물리 LBA 기준
 * (ref_tag)"에서 "원래 가상/제출 LBA 기준(virt)"으로 되돌림(완료 시점)
 *
 * @pi: 대상 crc64_pi_tuple - 현재 ref_tag 필드를 읽고 필요시 덮어씀.
 * @virt: 되돌릴 목표 값 - bio 제출 당시 사용자가 알던 원래(가상) LBA 기준
 *        Reference Tag(48비트로 잘려 사용됨).
 * @ref_tag: 이 tuple이 현재 담고 있을 것으로 "기대"되는 물리 LBA 기준 값 -
 *        blk_integrity_prepare()가 이전에 이 값으로 덮어썼어야 정상.
 * @return: 없음(void).
 *
 * I/O 완료 후 ref tag를 원상 복구하는 안전한 조건부 쓰기다. 먼저 tuple에
 * 실제로 저장된 ref 값을 읽어, 그 값이 정확히 우리가 예상한 물리 기준값
 * (lower_48_bits(ref_tag))과 "일치"하고 동시에 이미 목표 virt 값과는
 * "다를" 때만 되돌려 쓴다. 두 조건 중 하나라도 어긋나면(예: 드라이버가
 * PRACT로 이 필드를 이미 다른 값으로 바꿔놓았거나, escape 값이 들어있는
 * 경우) 아무 것도 건드리지 않는다 - 이는 이 함수가 "확실히 우리가 이전에
 * 매핑해 둔 값일 때만" 안전하게 되돌리도록 하는 방어적 설계다.
 * 실행 컨텍스트: 호출자(blk_reftag_remap_complete())와 동일 - 완료
 * 인터럽트/softirq 컨텍스트.
 * 호출자(caller): blk_reftag_remap_complete() - csum_type==CRC64일 때.
 * 호출되는 함수(callee): get_unaligned_be48(), lower_48_bits(),
 * put_unaligned_be48().
 * 에러 처리: 없음 - 조건 불충족 시 조용히 아무 일도 하지 않음(부작용 없는
 * no-op).
 *
 * 호출 체인:
 *   blk_reftag_remap_complete -> [blk_set_ext_unmap_ref]
 */
static void blk_set_ext_unmap_ref(struct crc64_pi_tuple *pi, u64 virt,
				  u64 ref_tag)
{
	/* [한국어] tuple에 현재 저장된 ref_tag(48비트)를 읽음 - 아직 아무것도
	 * 바꾸지 않은 상태의 값 */
	u64 ref = get_unaligned_be48(&pi->ref_tag);

	/* [한국어] 두 조건을 모두 만족할 때만 되돌림: (1) 현재 값이 우리가
	 * prepare 단계에서 써 넣었을 것으로 기대하는 물리 기준값(ref_tag)과
	 * 정확히 같고, (2) 이미 목표(virt)와는 다른 경우(즉 아직 되돌리지
	 * 않은 상태) - 둘 다 아니면 건드릴 필요가 없거나 건드리면 위험함 */
	if (ref == lower_48_bits(ref_tag) && ref != lower_48_bits(virt))
		/* [한국어] ref_tag 필드를 virt(원래 가상/제출 LBA 기준)로
		 * 덮어써 물리 매핑을 원상 복구 */
		put_unaligned_be48(virt, pi->ref_tag);
}

/*
 * [한국어]
 * blk_set_t10_unmap_ref - 표준 T10 PI tuple의 ref_tag를 물리 LBA 기준에서
 * 가상/제출 LBA 기준으로 되돌림(완료 시점) - blk_set_ext_unmap_ref()의
 * 32비트 버전
 *
 * @pi: 대상 t10_pi_tuple.
 * @virt: 되돌릴 목표 값(32비트 가상 기준 ref tag).
 * @ref_tag: 현재 저장돼 있을 것으로 기대하는 물리 기준 값(32비트).
 * @return: 없음(void).
 *
 * 로직은 blk_set_ext_unmap_ref()와 완전히 동일하되, ref_tag가 48비트 배열이
 * 아니라 32비트 단일 정수(t10_pi_tuple.ref_tag)라서 lower_48_bits() 같은
 * 마스킹이 필요 없다는 점만 다르다.
 * 실행 컨텍스트: 호출자(blk_reftag_remap_complete())와 동일.
 * 호출자(caller): blk_reftag_remap_complete() - csum_type==CRC 또는 IP일 때.
 * 호출되는 함수(callee): get_unaligned_be32(), put_unaligned_be32().
 * 에러 처리: 없음 - 조건 불충족 시 no-op.
 *
 * 호출 체인:
 *   blk_reftag_remap_complete -> [blk_set_t10_unmap_ref]
 */
static void blk_set_t10_unmap_ref(struct t10_pi_tuple *pi, u32 virt,
				  u32 ref_tag)
{
	/* [한국어] tuple에 현재 저장된 ref_tag(32비트)를 읽음 */
	u32 ref = get_unaligned_be32(&pi->ref_tag);

	/* [한국어] 현재 값이 기대하는 물리 기준값(ref_tag)과 같고, 아직
	 * 목표(virt)로 되돌려지지 않았을 때만 덮어씀 */
	if (ref == ref_tag && ref != virt)
		/* [한국어] ref_tag 필드를 virt로 덮어써 원상 복구 */
		put_unaligned_be32(virt, &pi->ref_tag);
}

/*
 * [한국어]
 * blk_reftag_remap_complete - I/O 완료 시점에 csum_type에 맞는 "unmap"
 * 함수로 디스패치 - ref tag를 물리 LBA 기준에서 가상 LBA 기준으로 되돌림
 *
 * @bi: 디스크 무결성 프로파일 - csum_type으로 어느 unmap 함수를 쓸지 결정.
 * @tuple: remap 대상 PI tuple(union) - blk_tuple_remap_begin()이 마련해 준
 *        포인터가 가리키는 실제(또는 스크래치) 메모리.
 * @virt: 되돌릴 목표 값 - 원래 제출 시점의 가상 LBA 기준.
 * @ref: 현재 이 tuple에 들어있을 것으로 기대하는 물리 LBA 기준.
 * @return: 없음(void).
 *
 * __blk_reftag_remap()이 prep==false(즉 blk_integrity_complete() 경로)로
 * 호출했을 때 선택되는 쪽 - csum_type이 CRC64면 blk_set_ext_unmap_ref(),
 * CRC/IP(둘 다 표준 8바이트 tuple을 쓰므로 case를 공유)면
 * blk_set_t10_unmap_ref()로 위임한다. 알 수 없는 csum_type은
 * WARN_ON_ONCE로 설정 오류를 알린다.
 * 실행 컨텍스트: 호출자(__blk_reftag_remap())와 동일 - 완료 인터럽트/softirq
 * 컨텍스트(blk_integrity_complete() 경로).
 * 호출자(caller): __blk_reftag_remap() - prep==false일 때.
 * 호출되는 함수(callee): blk_set_ext_unmap_ref(), blk_set_t10_unmap_ref().
 * 에러 처리: 알 수 없는 csum_type은 WARN_ON_ONCE만 남기고 아무 tuple도
 * 건드리지 않음(반환형이 void라 에러를 알릴 방법이 없음).
 *
 * 호출 체인:
 *   __blk_reftag_remap -> [blk_reftag_remap_complete] -> blk_set_ext_unmap_ref
 *   / blk_set_t10_unmap_ref
 */
static void blk_reftag_remap_complete(struct blk_integrity *bi,
				      union pi_tuple *tuple, u64 virt, u64 ref)
{
	/* [한국어] csum_type에 따라 tuple의 어느 멤버를 어떤 unmap 함수로
	 * 되돌릴지 분기 */
	switch (bi->csum_type) {
	/* [한국어] NVMe CRC64 확장 PI 경로 */
	case BLK_INTEGRITY_CSUM_CRC64:
		/* [한국어] union의 crc64_pi 멤버를 48비트 unmap 함수에 전달 */
		blk_set_ext_unmap_ref(&tuple->crc64_pi, virt, ref);
		break;
	/* [한국어] 표준 T10 DIF CRC16과 IP 체크섬은 tuple 레이아웃(32비트
	 * ref_tag)이 같으므로 같은 처리로 폴스루 */
	case BLK_INTEGRITY_CSUM_CRC:
	/* [한국어] IP 체크섬도 동일 tuple 레이아웃이므로 위 case와 공유 */
	case BLK_INTEGRITY_CSUM_IP:
		/* [한국어] union의 t10_pi 멤버를 32비트 unmap 함수에 전달 */
		blk_set_t10_unmap_ref(&tuple->t10_pi, virt, ref);
		break;
	/* [한국어] 알 수 없는 csum_type - 설정 오류 */
	default:
		/* [한국어] 커널 로그에 1회 경고 */
		WARN_ON_ONCE(1);
		break;
	}
}

/*
 * [한국어]
 * blk_set_ext_map_ref - CRC64 확장 PI tuple의 ref_tag를 "원래 가상/제출
 * LBA 기준(virt)"에서 "물리 LBA 기준(ref_tag)"으로 바꿔 씀(제출 시점)
 *
 * @pi: 대상 crc64_pi_tuple.
 * @virt: 현재 저장돼 있을 것으로 기대하는 원래(가상) 값(48비트로 마스킹해 비교).
 * @ref_tag: 새로 써 넣을 목표 값 - 실제 이 request가 물리적으로 도달할 LBA
 *        기준 Reference Tag.
 * @return: 없음(void).
 *
 * blk_set_ext_unmap_ref()와 방향이 정반대인 짝 함수 - "아직 가상 기준값이
 * 그대로 들어있고, 목표 물리값과 다를 때만" 물리값으로 덮어쓴다. 이 안전
 * 조건 덕분에, 이미 다른 이유로 remap된 적이 있는 tuple을 실수로 재차 덮어
 * 쓰는 것을 막는다.
 * 실행 컨텍스트: 호출자(blk_reftag_remap_prepare())와 동일 - 제출 경로의
 * 프로세스 컨텍스트.
 * 호출자(caller): blk_reftag_remap_prepare() - csum_type==CRC64일 때.
 * 호출되는 함수(callee): get_unaligned_be48(), lower_48_bits(),
 * put_unaligned_be48().
 * 에러 처리: 없음 - 조건 불충족 시 no-op.
 *
 * 호출 체인:
 *   blk_reftag_remap_prepare -> [blk_set_ext_map_ref]
 */
static void blk_set_ext_map_ref(struct crc64_pi_tuple *pi, u64 virt,
				u64 ref_tag)
{
	/* [한국어] tuple에 현재 저장된 ref_tag(48비트)를 읽음 */
	u64 ref = get_unaligned_be48(&pi->ref_tag);

	/* [한국어] 현재 값이 아직 원래(가상) 기준값(virt)과 같고, 목표
	 * 물리값(ref_tag)과는 다를 때만 덮어씀 */
	if (ref == lower_48_bits(virt) && ref != ref_tag)
		/* [한국어] ref_tag 필드를 목표 물리 기준값(ref_tag)으로 덮어써
		 * 실제 제출될 물리 LBA에 맞춤 */
		put_unaligned_be48(ref_tag, pi->ref_tag);
}

/*
 * [한국어]
 * blk_set_t10_map_ref - 표준 T10 PI tuple의 ref_tag를 가상 LBA 기준에서
 * 물리 LBA 기준으로 바꿔 씀(제출 시점) - blk_set_ext_map_ref()의 32비트 버전
 *
 * @pi: 대상 t10_pi_tuple.
 * @virt: 현재 저장돼 있을 것으로 기대하는 원래(가상) 값(32비트).
 * @ref_tag: 새로 써 넣을 목표 물리 기준 값(32비트).
 * @return: 없음(void).
 *
 * 로직은 blk_set_ext_map_ref()와 동일하되 32비트 단일 정수 필드를 다룬다는
 * 점만 다르다.
 * 실행 컨텍스트: 호출자(blk_reftag_remap_prepare())와 동일.
 * 호출자(caller): blk_reftag_remap_prepare() - csum_type==CRC 또는 IP일 때.
 * 호출되는 함수(callee): get_unaligned_be32(), put_unaligned_be32().
 * 에러 처리: 없음 - 조건 불충족 시 no-op.
 *
 * 호출 체인:
 *   blk_reftag_remap_prepare -> [blk_set_t10_map_ref]
 */
static void blk_set_t10_map_ref(struct t10_pi_tuple *pi, u32 virt, u32 ref_tag)
{
	/* [한국어] tuple에 현재 저장된 ref_tag(32비트)를 읽음 */
	u32 ref = get_unaligned_be32(&pi->ref_tag);

	/* [한국어] 현재 값이 원래(가상) 기준값(virt)과 같고, 목표 물리값
	 * (ref_tag)과는 다를 때만 덮어씀 */
	if (ref == virt && ref != ref_tag)
		/* [한국어] ref_tag 필드를 목표 물리 기준값으로 덮어씀 */
		put_unaligned_be32(ref_tag, &pi->ref_tag);
}

/*
 * [한국어]
 * blk_reftag_remap_prepare - 제출 시점에 csum_type에 맞는 "map" 함수로
 * 디스패치 - ref tag를 가상 LBA 기준에서 물리 LBA 기준으로 바꿈
 *
 * @bi: 디스크 무결성 프로파일 - csum_type으로 어느 map 함수를 쓸지 결정.
 * @tuple: remap 대상 PI tuple(union).
 * @virt: 현재 저장돼 있을 것으로 기대하는 원래(가상) 값.
 * @ref: 새로 써 넣을 목표 물리 기준 값.
 * @return: 없음(void).
 *
 * blk_reftag_remap_complete()의 반대 방향 짝 함수 - __blk_reftag_remap()이
 * prep==true(blk_integrity_prepare() 경로)로 호출했을 때 선택된다.
 * csum_type이 CRC64면 blk_set_ext_map_ref(), CRC/IP면 blk_set_t10_map_ref()로
 * 위임한다.
 * 실행 컨텍스트: 호출자(__blk_reftag_remap())와 동일 - 제출 경로의 프로세스
 * 컨텍스트(blk_integrity_prepare() 경로).
 * 호출자(caller): __blk_reftag_remap() - prep==true일 때.
 * 호출되는 함수(callee): blk_set_ext_map_ref(), blk_set_t10_map_ref().
 * 에러 처리: 알 수 없는 csum_type은 WARN_ON_ONCE만 남기고 아무 tuple도
 * 건드리지 않음.
 *
 * 호출 체인:
 *   __blk_reftag_remap -> [blk_reftag_remap_prepare] -> blk_set_ext_map_ref
 *   / blk_set_t10_map_ref
 */
static void blk_reftag_remap_prepare(struct blk_integrity *bi,
				     union pi_tuple *tuple,
				     u64 virt, u64 ref)
{
	/* [한국어] csum_type에 따라 tuple의 어느 멤버를 어떤 map 함수로 바꿀지 분기 */
	switch (bi->csum_type) {
	/* [한국어] NVMe CRC64 확장 PI 경로 */
	case BLK_INTEGRITY_CSUM_CRC64:
		/* [한국어] union의 crc64_pi 멤버를 48비트 map 함수에 전달 */
		blk_set_ext_map_ref(&tuple->crc64_pi, virt, ref);
		break;
	/* [한국어] 표준 T10 DIF CRC16과 IP 체크섬은 동일 tuple 레이아웃이므로
	 * 폴스루로 공유 */
	case BLK_INTEGRITY_CSUM_CRC:
	/* [한국어] IP 체크섬도 동일 레이아웃 공유 */
	case BLK_INTEGRITY_CSUM_IP:
		/* [한국어] union의 t10_pi 멤버를 32비트 map 함수에 전달 */
		blk_set_t10_map_ref(&tuple->t10_pi, virt, ref);
		break;
	/* [한국어] 알 수 없는 csum_type - 설정 오류 */
	default:
		/* [한국어] 커널 로그에 1회 경고 */
		WARN_ON_ONCE(1);
		break;
	}
}

/*
 * [한국어]
 * __blk_reftag_remap - 한 bio에 속한 interval들을 순회하며 ref tag를
 * remap(prep=true: 가상->물리) 또는 unremap(prep=false: 물리->가상)
 *
 * @bio: 대상 bio - bio_integrity(bio)로 payload를 얻는다.
 * @bi: 디스크 무결성 프로파일.
 * @intervals: 처리할 남은 interval 수(입출력) - 이 함수가 처리한 만큼 감소
 *        시키며, 상위(blk_integrity_remap())가 여러 bio에 걸쳐 공유하는
 *        카운터.
 * @ref: 물리 LBA 기준 Reference Tag의 현재 값(입출력) - 이 함수가 처리한
 *        interval 수만큼 증가시켜, 다음 bio 호출 시 이어지는 물리 LBA를
 *        가리키게 한다.
 * @prep: true = 제출 전 준비(가상->물리 map), false = 완료 후 정리
 *        (물리->가상 unmap).
 * @return: 없음(void).
 *
 * 먼저 이 bio가 이미 remap이 완료된 상태인지(BIP_MAPPED_INTEGRITY 플래그)
 * 확인한다 - prep==true인데 이미 매핑돼 있다면(예: 재시도 등으로 이 함수가
 * 두 번 호출된 경우) 실제 tuple을 다시 건드리지 않고 *ref 카운터만
 * 이 bio가 담당하는 interval 수만큼 앞으로 건너뛰어(bio->bi_iter.bi_size
 * >> interval_exp) 다음 bio 처리를 위한 기준을 맞춘 뒤 조기 반환한다 -
 * 이는 이미 매핑된 tuple을 또 map하면 원래 가상값을 잃어버리는 것을 막기
 * 위한 멱등성(idempotency) 보장 장치다.
 * 그렇지 않다면 bip->bip_iter로 초기화한 iter로 시작해, 남은 interval이
 * 있고(iter.bi_size) 상위가 지정한 처리 개수(*intervals)가 남아있는 동안
 * 반복한다: blk_tuple_remap_begin()으로 이번 interval의 tuple을 확보하고,
 * prep 값에 따라 blk_reftag_remap_prepare() 또는 blk_reftag_remap_complete()로
 * 실제 ref_tag를 고쳐 쓴 뒤, blk_tuple_remap_end()로 마무리(kunmap 또는
 * 스크래치 반영)하고 iter를 다음 위치로 전진시킨다. 매 반복마다 intervals를
 * 감소시키고 ref/virt를 1씩 증가시켜 다음 논리 블록으로 넘어간다. 마지막으로
 * prep==true였다면(방금 매핑을 마쳤다면) bip->bip_flags에 BIP_MAPPED_INTEGRITY를
 * 세워 이후 재호출 시 위 조기 반환 경로를 타게 한다.
 * 실행 컨텍스트: 호출자(blk_integrity_remap())와 동일 - prep=true는 제출
 * 경로 프로세스 컨텍스트, prep=false는 완료 인터럽트/softirq 컨텍스트.
 * 호출자(caller): blk_integrity_remap() - request에 속한 각 bio에 대해
 * __rq_for_each_bio()로 순회하며 호출.
 * 호출되는 함수(callee): bio_integrity(), bip_get_seed(),
 * blk_tuple_remap_begin(), blk_reftag_remap_prepare(),
 * blk_reftag_remap_complete(), blk_tuple_remap_end().
 * 에러 처리: 없음 - 반환형이 void이고, 모든 하위 map/unmap 함수도 실패
 * 개념이 없다(조건 불충족 시 조용히 no-op).
 *
 * 호출 체인:
 *   blk_integrity_remap -> [__blk_reftag_remap] -> blk_tuple_remap_begin /
 *   blk_reftag_remap_prepare / blk_reftag_remap_complete / blk_tuple_remap_end
 */
static void __blk_reftag_remap(struct bio *bio, struct blk_integrity *bi,
			       unsigned *intervals, u64 *ref, bool prep)
{
	/* [한국어] 이 bio에 붙은 PI 메타데이터 payload 조회 */
	struct bio_integrity_payload *bip = bio_integrity(bio);
	/* [한국어] payload의 현재 진행 위치(bip_iter)로 로컬 순회용 iter를
	 * 초기화 - bip 자체의 상태는 건드리지 않도록 복사본 사용 */
	struct bvec_iter iter = bip->bip_iter;
	/* [한국어] 이 bio가 제출될 당시 알고 있던 "가상" Reference Tag 기준값
	 * (bip->bip_iter.bi_sector, bip_get_seed()가 이를 그대로 반환) */
	u64 virt = bip_get_seed(bip);
	/* [한국어] blk_tuple_remap_begin()이 반환하는, 실제 remap 대상 tuple을
	 * 가리키는 포인터(kmap된 실버퍼 또는 스택 tuple) */
	union pi_tuple *ptuple;
	/* [한국어] tuple이 bio_vec 경계에 걸렸을 때 쓸 스택 스크래치 공간 */
	union pi_tuple tuple;

	/* [한국어] prep(제출 전 준비) 단계인데 이 bio가 이미 매핑 완료
	 * 상태(BIP_MAPPED_INTEGRITY)라면 - 재시도 등으로 중복 호출된 경우 */
	if (prep && bip->bip_flags & BIP_MAPPED_INTEGRITY) {
		/* [한국어] 실제 tuple은 다시 건드리지 않고, 이 bio가 담당하는
		 * interval 수(bio 전체 크기 >> interval_exp)만큼만 *ref
		 * 카운터를 건너뛰어 다음 bio 처리의 기준을 맞춤 */
		*ref += bio->bi_iter.bi_size >> bi->interval_exp;
		/* [한국어] 이미 매핑된 tuple을 또 덮어쓰지 않도록 조기 반환 -
		 * 멱등성 보장 */
		return;
	}

	/* [한국어] 이 bio 안에 남은 interval이 있고, 상위가 지정한 처리
	 * 개수(*intervals)가 아직 남아있는 동안 반복 */
	while (iter.bi_size && *intervals) {
		/* [한국어] 이번 interval의 tuple을 확보 - 실제 버퍼 kmap 또는
		 * 스택 스크래치로 모아 옴(내부적으로 padding skip 포함) */
		ptuple = blk_tuple_remap_begin(&tuple, bi, bip, &iter);

		/* [한국어] prep(제출 전)이면 가상->물리 방향으로 map */
		if (prep)
			/* [한국어] virt(가상 기준)에서 *ref(물리 기준)로 ref_tag를
			 * 바꿔 씀 */
			blk_reftag_remap_prepare(bi, ptuple, virt, *ref);
		/* [한국어] 그렇지 않으면(완료 후) 물리->가상 방향으로 unmap */
		else
			/* [한국어] *ref(물리 기준)에서 virt(가상 기준)로 ref_tag를
			 * 되돌림 */
			blk_reftag_remap_complete(bi, ptuple, virt, *ref);

		/* [한국어] tuple 처리 마무리(kunmap 또는 스크래치를 실제
		 * 버퍼로 반영)와 함께 iter를 다음 interval 위치로 전진 */
		blk_tuple_remap_end(&tuple, ptuple, bi, bip, &iter);
		/* [한국어] 상위가 요청한 처리 개수를 하나 소진 - 0이 되면 이
		 * bio 안에서도 루프 종료 가능 */
		(*intervals)--;
		/* [한국어] 물리 기준 ref 카운터를 다음 논리 블록으로 1 증가 */
		(*ref)++;
		/* [한국어] 가상 기준 카운터도 함께 1 증가 - 물리/가상 모두
		 * 연속된 LBA라는 전제 하에 나란히 전진 */
		virt++;
	}

	/* [한국어] 방금 이 bio의 매핑을 완료했다면(prep==true) */
	if (prep)
		/* [한국어] BIP_MAPPED_INTEGRITY 플래그를 세워, 이후 재호출 시
		 * 위쪽의 조기 반환 경로(이미 매핑됨)를 타도록 표시 */
		bip->bip_flags |= BIP_MAPPED_INTEGRITY;
}

/*
 * [한국어]
 * blk_integrity_remap - request에 속한 모든 bio에 대해 ref tag remap을
 * 수행 - blk_integrity_prepare()/blk_integrity_complete()의 공통 구현
 *
 * @rq: 대상 request - rq->q->limits.integrity(디스크 프로파일)와
 *        blk_rq_pos(rq)(이 request의 물리 시작 LBA)를 사용.
 * @nr_bytes: 이번에 처리할 바이트 수 - prepare 시에는 request 전체
 *        (blk_rq_bytes(rq)), complete 시에는 부분 완료를 지원하기 위해
 *        이번에 완료된 바이트 수만 넘어온다.
 * @prep: true = 제출 전(가상->물리 map), false = 완료 후(물리->가상 unmap).
 * @return: 없음(void).
 *
 * 이 request가 실제로 도달할(또는 도달했던) 물리 LBA 기준 Reference Tag의
 * 시작값(ref)을 blk_rq_pos(rq)(물리 섹터 위치)를 interval 크기로 나누어
 * 계산하고, 처리할 interval 개수(intervals)를 nr_bytes를 interval 크기로
 * 나누어 계산한다. 이 디스크 프로파일이 애초에 Reference Tag 검사를
 * 요구하지 않으면(BLK_INTEGRITY_REF_TAG 미설정) remap 자체가 의미 없으므로
 * 즉시 반환한다. 그렇지 않으면 __rq_for_each_bio()로 이 request에 매달린
 * 모든 bio를 순회하며 __blk_reftag_remap()에 위임하고, 남은 intervals가
 * 0이 되면(이 request의 모든 interval을 처리했으면) 나머지 bio는 건드릴
 * 필요가 없으므로 순회를 즉시 중단한다.
 * 실행 컨텍스트: 호출자(blk_integrity_prepare/complete())와 동일.
 * 호출자(caller): blk_integrity_prepare()(prep=true), blk_integrity_complete()
 * (prep=false).
 * 호출되는 함수(callee): __rq_for_each_bio(), __blk_reftag_remap().
 * 에러 처리: 없음.
 *
 * 호출 체인:
 *   blk_integrity_prepare / blk_integrity_complete -> [blk_integrity_remap]
 *   -> __blk_reftag_remap
 */
static void blk_integrity_remap(struct request *rq, unsigned int nr_bytes,
				bool prep)
{
	/* [한국어] 이 request가 속한 디스크의 무결성 프로파일 - request_queue의
	 * limits에서 직접 참조(구조체 포함 방식이라 별도 조회 함수 없이 &로 접근) */
	struct blk_integrity *bi = &rq->q->limits.integrity;
	/* [한국어] 이 request의 물리 시작 섹터(blk_rq_pos)를 논리 블록/보호
	 * interval 단위로 환산 - interval_exp가 섹터 단위(SECTOR_SHIFT=9,
	 * 512바이트)보다 큰 만큼 오른쪽으로 시프트해 "몇 번째 interval"인지
	 * 계산 */
	u64 ref = blk_rq_pos(rq) >> (bi->interval_exp - SECTOR_SHIFT);
	/* [한국어] 이번에 처리할 바이트 수를 interval 크기로 나누어 처리할
	 * interval 개수를 계산 */
	unsigned intervals = nr_bytes >> bi->interval_exp;
	/* [한국어] __rq_for_each_bio 순회에 쓸 지역 변수 */
	struct bio *bio;

	/* [한국어] 이 디스크가 Reference Tag 검사를 요구하지 않으면(BLK_INTEGRITY_REF_TAG
	 * 미설정, 예: PI Type 3 등) remap할 대상 자체가 없음 */
	if (!(bi->flags & BLK_INTEGRITY_REF_TAG))
		/* [한국어] 아무 것도 하지 않고 즉시 반환 */
		return;

	/* [한국어] request에 매달린 모든 bio를 순회 - 하나의 request는 여러
	 * bio가 병합되어 구성될 수 있음 */
	__rq_for_each_bio(bio, rq) {
		/* [한국어] 이 bio에 대해 실제 remap(또는 unremap) 수행 -
		 * intervals/ref는 여러 bio에 걸쳐 누적/전진되는 공유 카운터 */
		__blk_reftag_remap(bio, bi, &intervals, &ref, prep);
		/* [한국어] 이 request가 요구한 interval을 모두 처리했다면 */
		if (!intervals)
			/* [한국어] 남은 bio는 처리할 필요가 없으므로 순회 중단 */
			break;
	}
}

/*
 * [한국어]
 * blk_integrity_prepare - request 제출 직전 ref tag를 물리 LBA 기준으로
 * remap(map) - blk-mq.c가 호출하는 이 파일의 공개 진입점
 *
 * @rq: 곧 드라이버에 제출될 request.
 * @return: 없음(void).
 *
 * blk_integrity_remap(rq, blk_rq_bytes(rq), true)를 호출해, 이 request
 * 전체 바이트에 해당하는 interval들의 ref tag를 가상(제출자가 알던) LBA
 * 기준에서 물리(이 request가 실제 도달할) LBA 기준으로 바꿔 쓴다. 이는
 * bio_integrity_generate()가 하는 Guard/AppTag/RefTag "생성" 작업과는
 * 별개로, 이미 생성된 tuple의 ref_tag "필드만" 스택 디바이스(파티션,
 * dm-linear 등)를 거치며 달라진 물리 위치에 맞게 보정하는 것이다.
 * 실행 컨텍스트: 제출 경로의 프로세스 컨텍스트 - blk_mq_start_request()
 * (block/blk-mq.c)가 REQ_OP_WRITE이고 blk_integrity_rq(rq)가 참일 때 호출.
 * 호출자(caller): blk_mq_start_request()(block/blk-mq.c).
 * 호출되는 함수(callee): blk_integrity_remap().
 * 에러 처리: 없음.
 *
 * 호출 체인:
 *   blk_mq_start_request -> [blk_integrity_prepare] -> blk_integrity_remap
 */
void blk_integrity_prepare(struct request *rq)
{
	/* [한국어] request 전체 바이트(blk_rq_bytes)에 대해 prep=true(가상->물리
	 * map)로 remap 수행 */
	blk_integrity_remap(rq, blk_rq_bytes(rq), true);
}

/*
 * [한국어]
 * blk_integrity_complete - request 완료 시점에 ref tag를 다시 가상 LBA
 * 기준으로 remap(unmap) - blk-mq.c가 호출하는 이 파일의 공개 진입점
 *
 * @rq: 완료 처리 중인 request(부분 완료 가능).
 * @nr_bytes: 이번에 완료 처리된 바이트 수 - 부분 완료 시 전체가 아닌 이번
 *        구간만 대상으로 함.
 * @return: 없음(void).
 *
 * blk_integrity_remap(rq, nr_bytes, false)를 호출해, blk_integrity_prepare()가
 * 물리 기준으로 바꿔 두었던 ref tag를 다시 원래(가상) 기준으로 되돌린다.
 * 이렇게 해야 이후 bio_integrity_verify()나 상위 계층이 tuple을 볼 때
 * 제출자가 원래 기대했던 ref tag 값을 그대로 확인할 수 있다(물리 위치는
 * 스택 디바이스 내부 구현 세부사항이므로 상위에 노출되면 안 된다).
 * 실행 컨텍스트: 완료 인터럽트/softirq 컨텍스트 - blk_update_request()/
 * blk_complete_request()(block/blk-mq.c)가 REQ_OP_READ이고
 * blk_integrity_rq(rq)가 참일 때 호출.
 * 호출자(caller): blk_update_request(), blk_complete_request()
 * (둘 다 block/blk-mq.c).
 * 호출되는 함수(callee): blk_integrity_remap().
 * 에러 처리: 없음.
 *
 * 호출 체인:
 *   blk_update_request / blk_complete_request -> [blk_integrity_complete]
 *   -> blk_integrity_remap
 */
void blk_integrity_complete(struct request *rq, unsigned int nr_bytes)
{
	/* [한국어] 이번에 완료된 바이트(nr_bytes)에 대해 prep=false(물리->가상
	 * unmap)로 remap 수행 */
	blk_integrity_remap(rq, nr_bytes, false);
}
