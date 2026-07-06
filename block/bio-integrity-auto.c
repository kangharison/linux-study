// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2007, 2008, 2009 Oracle Corporation
 * Written by: Martin K. Petersen <martin.petersen@oracle.com>
 *
 * Automatically generate and verify integrity data on PI capable devices if the
 * bio submitter didn't provide PI itself.  This ensures that kernel verifies
 * data integrity even if the file system (or other user of the block device) is
 * not aware of PI.
 */

/*
 * [한국어 설명] PI(Protection Information) 미제공 bio에 대한 자동 무결성
 * 생성/검증 계층 (bio-integrity-auto.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 파일시스템 등 bio 제출자가 PI(Protection Information, T10
 * DIF/DIX 계열 무결성 메타데이터)를 스스로 채워 넣지 않은 bio에 대해,
 * 블록 계층이 대신 PI를 생성(WRITE)하거나 검증(READ)하도록 만드는 "자동"
 * 경로를 구현한다. 구체적으로는 bio_integrity_prep()에서 필요한 만큼의 PI
 * 메타데이터 버퍼를 할당하고, WRITE라면 Guard(체크섬)/Reference Tag/
 * Application Tag를 즉시 계산해 채우며, READ라면 완료 시점에 검증할 수
 * 있도록 원본 bvec_iter를 저장해 둔다. 검증 자체는 인터럽트/softirq
 * 컨텍스트에서 수행하기에는 CPU 비용이 크므로, __bio_integrity_endio()가
 * kintegrityd라는 전용 workqueue로 실제 검증(bio_integrity_verify_fn)을
 * 미뤄 인터럽트 지연시간을 보호한다. 이 파일이 없다면 PI를 스스로 관리하지
 * 않는 일반 파일시스템/사용자는 T10 PI가 활성화된 디스크에서 무결성 보호를
 * 전혀 받지 못하게 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 블록 계층의 무결성 서브시스템은 크게 세 갈래로 나뉜다:
 *   block/bio-integrity.c    : 공통 코어 API (alloc/free/setup_default 등)
 *   block/bio-integrity-fs.c : 파일시스템이 스스로 PI를 준비하는 경로
 *   block/bio-integrity-auto.c (현재 파일) : 파일시스템이 PI를 준비하지
 *                              않았을 때 블록 계층이 대신 생성/검증
 *   block/t10-pi.c           : 실제 Guard/Reference/App Tag 계산 알고리즘
 * 제출 경로에서는 blk_mq_submit_bio()가 bio_integrity_action()으로 필요한
 * 조치(BI_ACT_BUFFER/CHECK/ZERO 비트마스크)를 계산한 뒤 이 파일의
 * bio_integrity_prep()을 호출한다. 이는 blk_mq_get_request 이전, 즉 실제로
 * 드라이버(NVMe 등)에 요청이 전달되기 전 단계다.
 *   submit_bio -> blk_mq_submit_bio -> bio_integrity_action
 *             -> [bio_integrity_prep] (본 파일)
 *             -> blk_mq_get_request -> nvme_queue_rq -> 도어벨(doorbell) 기록
 * 완료 경로에서는 드라이버 인터럽트(NVMe라면 MSI-X ISR)가 bio_endio()를
 * 호출하고, bio_endio()는 block/blk.h의 인라인 함수 bio_integrity_endio()를
 * 거쳐 이 파일의 __bio_integrity_endio()로 진입한다.
 *   NVMe MSI-X ISR -> nvme_complete_rq -> bio_endio -> bio_integrity_endio
 *             -> [__bio_integrity_endio] (본 파일)
 *             -> (검증 필요) queue_work(kintegrityd_wq)
 *             -> [bio_integrity_verify_fn] (본 파일, 지연 실행)
 *             -> bio_integrity_verify -> bio_integrity_finish -> bio_endio
 * 즉 이 파일은 "제출 직전"과 "완료 직후"라는 두 시점 모두에 관여하는 훅
 * (hook) 모듈이다. 실행 컨텍스트는 함수별로 다르다 — bio_integrity_prep()은
 * 제출자의 프로세스 컨텍스트에서, __bio_integrity_endio()는 완료
 * 인터럽트/softirq 컨텍스트에서, bio_integrity_verify_fn()은 kintegrityd
 * 워커 스레드의 프로세스 컨텍스트에서 각각 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일은 block/bio-integrity.c가 제공하는 bio_integrity_init(),
 * bio_integrity_alloc_buf(), bio_integrity_free_buf(),
 * bio_integrity_setup_default()와 block/t10-pi.c 계열이 구현하는
 * bio_integrity_generate()/bio_integrity_verify()를 그대로 재사용하며,
 * 자신은 그 위에 "언제 호출할지"를 결정하는 자동화 정책과 상태 보관
 * (struct bio_integrity_data)만 얹는다. include/linux/blk-integrity.h는
 * struct blk_integrity, bio_integrity_action(), BI_ACT_* 열거형을
 * 제공하고, (blk-integrity.h가 포함하는) include/linux/bio-integrity.h는
 * struct bio_integrity_payload와 bip_flags(BIP_CHECK_GUARD/REFTAG/APPTAG,
 * BIP_BLOCK_INTEGRITY 등)를 정의한다. include/linux/t10-pi.h는 PI의
 * 최종 온디스크 표현인 struct t10_pi_tuple(guard_tag/app_tag/ref_tag)을
 * 정의하는데, 이 파일 자체는 그 필드를 직접 다루지 않고 block/t10-pi.c가
 * 그 형식으로 버퍼를 채우거나 읽는다. "blk.h"는 이 서브시스템 내부에서만
 * 공유하는 비공개 선언을 제공한다. 데이터 흐름 관점에서 보면, WRITE는
 * "상위 계층 데이터 -> bio_integrity_prep()에서 PI 생성 -> bip에 저장 ->
 * 드라이버가 PRP/SGL/메타데이터 포인터로 전송"이고, READ는 "드라이버가
 * 채운 데이터+메타데이터 -> __bio_integrity_endio()가 kintegrityd에 위임 ->
 * bio_integrity_verify_fn()이 검증 -> 결과를 bio->bi_status에 반영 ->
 * bio_endio()로 상위 계층에 통지"이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct bio_integrity_data: 자동 PI 처리 동안 필요한 모든 상태(원본
 *   bio, 복원용 bvec_iter, 지연 검증용 work_struct, 실제 무결성 페이로드
 *   bip, 단일 bio_vec)를 한 번에 묶어 mempool로 할당/반환하는 컨테이너.
 * - bio_integrity_prep(): 제출 경로에서 PI 버퍼를 할당하고 WRITE는 즉시
 *   생성, READ는 검증을 위한 상태를 저장하는 진입점(공개 API,
 *   EXPORT_SYMBOL).
 * - __bio_integrity_endio(): 완료 경로에서 READ 검증이 필요한지 판단해
 *   kintegrityd로 위임하거나 즉시 정리하는 훅.
 * - bio_integrity_verify_fn(): kintegrityd workqueue에서 실제 검증을
 *   수행한 뒤 bio_endio()로 완료를 마무리하는 work 콜백.
 * - bio_integrity_finish(): bio와 PI 페이로드의 연결을 끊고 bid를
 *   mempool로 반환하는 공통 정리 루틴.
 * - bip_should_check(): BIP_CHECK_GUARD/REFTAG/APPTAG 중 하나라도 켜져
 *   있는지로 소프트웨어 생성/검증이 실제로 필요한지 판별.
 * - blk_integrity_auto_init()/blk_flush_integrity(): 서브시스템 초기화
 *   시 슬랩/mempool/workqueue를 준비하고, 필요 시 미완료 work를 flush
 *   하는 생명주기 관리 함수.
 */
#include <linux/blk-integrity.h>
	/* [한국어] struct blk_integrity, bio_integrity_action(), BI_ACT_*
	 * 열거형 및 (간접 포함되는 bio-integrity.h를 통해) struct
	 * bio_integrity_payload/bip_flags 정의를 가져온다. 이 파일의
	 * bio_integrity_prep()/__bio_integrity_endio() 선언도 이 헤더에
	 * 있어, 다른 파일(blk-mq.c 등)이 그 프로토타입을 보고 호출할 수
	 * 있게 한다. */
#include <linux/t10-pi.h>
	/* [한국어] T10 Protection Information의 최종 표현인 enum
	 * t10_dif_type, struct t10_pi_tuple(guard_tag/app_tag/ref_tag)을
	 * 정의한다. 이 .c 파일 코드 자체는 그 심볼을 직접 호출하지 않지만,
	 * 이 파일이 채우는 메타데이터 버퍼가 실제로 어떤 3-tuple 형식을
	 * 따르는지 이해하는 데 필요한 정의이며, block/t10-pi.c가 그 형식에
	 * 맞춰 버퍼를 읽고 쓴다. */
#include <linux/workqueue.h>
	/* [한국어] struct work_struct, INIT_WORK(), queue_work(),
	 * alloc_workqueue(), flush_workqueue() 등 지연 실행 인프라를
	 * 가져온다. READ 완료 시 PI 검증을 인터럽트 컨텍스트 밖으로 미루는
	 * 이 파일의 핵심 메커니즘(kintegrityd)이 이 헤더에 의존한다. */
#include "blk.h"
	/* [한국어] 블록 계층 서브시스템 내부에서만 공유하는 비공개 선언
	 * 모음. 이 파일이 정의하는 bio_integrity_prep()/blk_flush_integrity()
	 * 등을 다른 block/*.c 파일이 어떻게 재사용하는지에 대한 내부용
	 * 프로토타입이 이 헤더를 통해 오간다. */

/*
 * [한국어]
 * bio_integrity_data - 자동 PI 처리를 위해 필요한 임시 상태 구조체
 *
 * 이 구조체는 상위 계층이 제공하지 않은 무결성 메타데이터를 커널이 자동으로
 * 생성하거나 완료 시 검증할 때 사용하는 낮은 수준의 상태이다. bio 하나당
 * 정확히 하나가 mempool에서 할당되며, bio_integrity_prep()이 만들고
 * bio_integrity_finish()가 되돌려준다. NVMe 관점에서는 각 bio가 실제
 * PRP/SGL 데이터 외에 추가로 전달해야 할 PI 정보를 담는 컨테이너라고 볼 수
 * 있다.
 */
struct bio_integrity_data {
	struct bio			*bio;
		/* [한국어] NVMe/일반 블록 드라이버에 전달될 실제 데이터 bio에
		 * 대한 포인터.
		 * 설정자: bio_integrity_prep()이 mempool_alloc() 직후
		 *   bid->bio = bio; 로 대입.
		 * 읽는 자: bio_integrity_finish()가 bi_integrity/bi_opf를
		 *   정리할 때, bio_integrity_verify_fn()이 bio_endio()를
		 *   호출할 때 이 필드로 원본 bio를 복원.
		 * 값 범위: 유효한 bio 포인터(NULL 불가). bio_integrity_finish()
		 *   호출 이후에는 bid 자체가 mempool로 반환되므로 이 필드를
		 *   더 이상 참조해서는 안 된다.
		 * 동기화: 한 bio는 한 순간에 하나의 컨텍스트(제출 스레드
		 *   또는 단일 완료 인터럽트/워커)에서만 처리되도록 블록
		 *   계층이 보장하므로 별도 락 불필요. */
	struct bvec_iter		saved_bio_iter;
		/* [한국어] READ 완료 후 PI 검증을 수행할 때 사용할, 데이터
		 * 진행 전 시점의 bvec_iter 스냅샷.
		 * 설정자: bio_integrity_prep()의 else 분기(WRITE이면서
		 *   검증이 필요한 경우가 아닐 때, 즉 주로 READ)에서
		 *   bio->bi_iter 값을 그대로 복사.
		 * 읽는 자: bio_integrity_verify_fn()이
		 *   bio_integrity_verify(bio, &bid->saved_bio_iter) 호출 시
		 *   두 번째 인자로 전달해 원본 범위 기준으로 재검증.
		 * 값 범위: bio_integrity_prep() 시점의 bi_iter 값 그대로(그
		 *   시점 이후 아직 아무 진행도 없는 상태). WRITE이면서
		 *   bip_should_check()가 참인 경로에서는 이 필드가 설정되지
		 *   않고 대신 bio_integrity_generate() 경로를 탄다.
		 * 동기화: 없음 — 단일 bio 생명주기 동안 한 컨텍스트에서만
		 *   쓰고 읽는다. */
	struct work_struct		work;
		/* [한국어] PI 검증을 인터럽트/softirq 밖으로 미루기 위한
		 * 지연 실행 단위(work_struct).
		 * 설정자: __bio_integrity_endio()가 INIT_WORK(&bid->work,
		 *   bio_integrity_verify_fn)로 초기화한 뒤 queue_work()로
		 *   kintegrityd_wq에 등록.
		 * 읽는 자: workqueue 코어가 kintegrityd_wq에서 이 work를
		 *   꺼내 bio_integrity_verify_fn()을 실행하며, 그 함수는
		 *   container_of(work, struct bio_integrity_data, work)로
		 *   이 필드의 주소에서 bid 전체를 역산한다.
		 * 값 범위: 검증이 필요한 READ 완료 시에만 초기화/큐잉된다.
		 *   WRITE/에러/검증 불필요 시에는 이 필드가 전혀 쓰이지
		 *   않는다.
		 * 동기화: workqueue 코어가 자체 락으로 큐잉/실행 순서를
		 *   보장하므로 이 파일에서 추가 락은 불필요. kintegrityd는
		 *   max_active=1로 생성되어 동시에 하나의 work만 실행한다. */
	struct bio_integrity_payload	bip;
		/* [한국어] 블록 무결성 서브시스템 공통 자료구조의 실체 —
		 * bip_iter/bip_vcnt/bip_flags/app_tag/bip_vec를 담는다.
		 * 설정자: bio_integrity_prep()이 bio_integrity_init()으로
		 *   초기화하고, 곧이어 bip_flags |= BIP_BLOCK_INTEGRITY로
		 *   "블록 계층이 소유/생성한 PI"임을 표시.
		 * 읽는 자: bio_integrity(bio)가 반환하는 포인터를 통해
		 *   bio_integrity_generate()/verify(), bip_should_check(),
		 *   block/blk.h의 bio_integrity_endio()가 참조한다.
		 *   container_of(bip, struct bio_integrity_data, bip)로
		 *   이 필드의 주소에서 bid 전체를 역산하는 것이 이 파일의
		 *   핵심 트릭이다.
		 * 값 범위: bio_integrity_init() 내부에서 bio->bi_integrity가
		 *   이 필드의 주소를 가리키도록 연결된다. bio_integrity_finish()
		 *   이후에는 bio->bi_integrity가 NULL로 끊긴다.
		 * 동기화: bio 단위 컨텍스트에서만 접근되므로 별도 락
		 *   불필요. */
	struct bio_vec			bvec;
		/* [한국어] PI 메타데이터 버퍼 1개를 표현하는 단일 bio_vec
		 * 슬롯 — bip.bip_vec가 가리키는 배열의 실체.
		 * 설정자: bio_integrity_init(bio, &bid->bip, &bid->bvec, 1)
		 *   호출에서 bip_vec 포인터로 등록되고, 곧이어
		 *   bio_integrity_alloc_buf()가 bvec_set_page()로 실제
		 *   page/len/offset을 채운다.
		 * 읽는 자: bio_integrity_generate()/verify() 및 드라이버가
		 *   bip_for_each_vec 매크로로 순회하며 참조.
		 * 값 범위: nr_vecs=1로 고정 — 이 자동 경로는 항상 단일
		 *   연속 버퍼만 사용하며 여러 세그먼트로 나뉘지 않는다
		 *   (파일시스템이 직접 PI를 붙이는 fs 경로와의 차이점).
		 * 동기화: 없음 — 단일 bio 생명주기 내에서만 유효. */
};

static struct kmem_cache *bid_slab;
	/* [한국어] bio_integrity_data 구조체 전용 kmem_cache.
	 * 설정자: blk_integrity_auto_init()에서 서브시스템 초기화 시 1회
	 *   생성.
	 * 읽는 자: 곧이어 bid_pool을 만들 때 mempool_init_slab_pool()의
	 *   백엔드로 전달.
	 * 값 범위: 초기화 성공 시 유효한 kmem_cache 포인터. SLAB_PANIC
	 *   플래그로 생성했기 때문에 실패 시 커널이 즉시 panic하므로
	 *   NULL이 되는 경로 자체가 존재하지 않는다.
	 * 동기화: 초기화 이후에는 읽기 전용으로만 참조되므로 락 불필요. */
static mempool_t bid_pool;
	/* [한국어] bid 객체를 GFP_NOIO로도 확보할 수 있도록 예비 객체를
	 * 갖고 있는 mempool — I/O 제출 경로에서 메모리 회수(reclaim)로
	 * 재귀 진입하는 것을 막기 위한 장치.
	 * 설정자: blk_integrity_auto_init()에서 BIO_POOL_SIZE(2)개 예약.
	 * 읽는 자: bio_integrity_prep()의 mempool_alloc(),
	 *   bio_integrity_finish()의 mempool_free().
	 * 값 범위: 슬랩 할당이 실패하더라도 예약분에서 최소
	 *   BIO_POOL_SIZE개까지는 항상 할당 가능하도록 mempool 계층이
	 *   보장.
	 * 동기화: mempool 내부 스핀락/대기큐가 동시 접근을 보호하므로 이
	 *   파일에서 추가 락은 불필요. */
static struct workqueue_struct *kintegrityd_wq;
	/* [한국어] READ PI 검증을 인터럽트 밖 프로세스 컨텍스트로 미루기
	 * 위한 전용 workqueue.
	 * 설정자: blk_integrity_auto_init()에서 WQ_MEM_RECLAIM |
	 *   WQ_HIGHPRI | WQ_CPU_INTENSIVE 플래그와 max_active=1로 생성.
	 * 읽는 자: __bio_integrity_endio()의 queue_work(),
	 *   blk_flush_integrity()의 flush_workqueue().
	 * 값 범위: 초기화 성공 시 유효한 workqueue_struct 포인터. 생성
	 *   실패 시 즉시 panic하므로 이후 코드에서 NULL 케이스를 고려할
	 *   필요가 없다.
	 * 동기화: workqueue 코어가 자체적으로 동시성을 관리하며,
	 *   max_active=1이므로 이 workqueue 안에서는 항상 최대 1개의
	 *   work만 동시 실행된다(검증 순서를 어느 정도 직렬화). */

/*
 * [한국어]
 * bio_integrity_finish - 자동 생성된 PI 상태를 정리하고 bio에서 분리
 *
 * @bid: 정리할 무결성 상태. 함수가 반환될 때 이미 mempool로 반환되어
 *   재사용될 수 있으므로, 호출자는 이후 이 포인터를 다시 참조해서는
 *   안 된다.
 * @return: 없음(void).
 *
 * 동기/배경: bio_integrity_prep()에서 mempool_alloc()으로 얻은 bid는
 * WRITE 생성이든 READ 검증이든 완료 시점에 반드시 해제되어야 한다. 이
 * 함수가 없다면 매 I/O마다 bid와 PI 버퍼가 누수되어 bid_pool이
 * 고갈되고, 이후 mempool_alloc(GFP_NOIO)가 새 객체 확보를 시도하다
 * I/O 제출 경로 자체가 정지(stall)할 위험이 있다.
 * 동작 단계:
 *   1) bid->bio->bi_integrity를 NULL로 만들어 이 bio가 더 이상 PI
 *      페이로드를 참조하지 않게 한다(다음 재사용 시 잔여 포인터로
 *      오작동하는 것을 방지).
 *   2) bi_opf에서 REQ_INTEGRITY 플래그를 지워, 이후 경로(재시도, 스택
 *      드라이버 등)가 이 bio를 "PI 없는 일반 bio"로 취급하게 한다.
 *   3) bio_integrity_free_buf()로 PI 메타데이터 버퍼(kmalloc 또는
 *      integrity_buf_pool 유래 페이지)를 되돌려준다.
 *   4) bid 자체를 bid_pool로 반환한다.
 * 실행 컨텍스트: 호출자에 따라 다르다 — __bio_integrity_endio()에서
 * 직접 호출될 때는 완료 인터럽트/softirq 컨텍스트, bio_integrity_verify_fn()
 * 에서 호출될 때는 kintegrityd 워커의 프로세스 컨텍스트다. 한 bid는
 * 단일 bio의 생명주기 동안 한 스레드/컨텍스트에서만 다뤄지므로 별도
 * 락은 없다.
 * 호출자: __bio_integrity_endio()(READ가 아니거나 검증 불필요/에러일
 *   때 즉시), bio_integrity_verify_fn()(지연 검증 완료 후).
 * 피호출자: bio_integrity_free_buf(), mempool_free().
 * 에러 경로: 이 함수 자체는 반환값이 없고 항상 성공하는 정리 동작이라
 *   실패 경로가 없다.
 *
 * 호출 체인:
 *   __bio_integrity_endio -> [bio_integrity_finish] -> (호출자로 복귀)
 *   bio_integrity_verify_fn -> [bio_integrity_finish] -> bio_endio
 */
static void bio_integrity_finish(struct bio_integrity_data *bid)
{
	bid->bio->bi_integrity = NULL;
		/* [한국어] bio에서 무결성 페이로드 참조를 제거한다 - 이후
		 * bio_integrity(bio)를 호출해도 REQ_INTEGRITY만 남아있지
		 * 않다면(다음 줄에서 곧 지움) NULL이 반환되어 이미 반환된
		 * bid를 다시 가리키는 일이 없다. */
	bid->bio->bi_opf &= ~REQ_INTEGRITY;
		/* [한국어] REQ_INTEGRITY 플래그를 지워 이 bio를 더 이상
		 * "무결성 처리 대상"으로 표시하지 않는다 - bio_integrity()
		 * 매크로가 이 플래그를 검사하므로, 지우지 않으면 이미 해제된
		 * bip를 가리키는 위험한 상태가 남는다. */
	bio_integrity_free_buf(&bid->bip);
		/* [한국어] PI 메타데이터 버퍼(bio_integrity_alloc_buf()가
		 * kmalloc 또는 integrity_buf_pool에서 확보했던 페이지)를
		 * 해제한다 - BIP_MEMPOOL 플래그 여부에 따라 내부에서
		 * mempool_free 또는 kfree 경로로 분기(block/bio-integrity.c
		 * 참고). */
	mempool_free(bid, &bid_pool);
		/* [한국어] bio_integrity_data 구조체 자체를 사전 예약 풀로
		 * 반환한다 - 이 시점 이후 이 함수의 인자였던 bid 포인터는
		 * 무효(다른 I/O가 즉시 재사용할 수 있음). */
}

/*
 * [한국어]
 * bio_integrity_verify_fn - 프로세스 컨텍스트에서 PI 검증을 수행하는 work
 * 콜백
 *
 * @work: __bio_integrity_endio()가 queue_work()로 kintegrityd_wq에 등록한
 *   work_struct. bio_integrity_data.work 필드의 주소가 그대로 전달된다.
 * @return: 없음(void). 검증 결과는 bio->bi_status를 통해 상위 계층에
 *   전달된다.
 *
 * 동기/배경: PI 검증(Guard CRC 재계산, Reference/Application Tag 비교)은
 * CPU 사이클을 소모하는 작업이라 인터럽트/softirq 컨텍스트에서 수행하면
 * 다른 인터럽트 처리를 지연시킨다. 이 함수는 kintegrityd workqueue
 * 워커 스레드에서 실행되어 그 비용을 인터럽트 밖으로 옮긴다.
 * 동작 단계:
 *   1) container_of()로 work_struct 포인터에서 이를 포함하는
 *      bio_integrity_data 전체(bid)를 역산한다.
 *   2) bid->bio로 원본 bio를 얻는다.
 *   3) bio_integrity_verify(bio, &bid->saved_bio_iter)를 호출해
 *      saved_bio_iter가 가리키는 원본 데이터 범위 기준으로 실제
 *      검증을 수행하고, 그 결과(BLK_STS_OK 또는 에러 코드)를
 *      bio->bi_status에 대입한다.
 *   4) bio_integrity_finish(bid)로 bid/PI 버퍼를 정리한다.
 *   5) bio_endio(bio)를 호출해 상위 계층(파일시스템 등)에 완료를
 *      통지한다 - __bio_integrity_endio()가 앞서 false를 반환하며
 *      미뤄두었던 완료 통지를 이 시점에 대신 수행하는 것이다.
 * 실행 컨텍스트: kintegrityd workqueue의 워커 스레드(프로세스
 * 컨텍스트). max_active=1이므로 이 workqueue 안에서는 항상 하나의
 * work만 동시에 실행된다.
 * 호출자: workqueue 코어(process_one_work() 등)가 queue_work()로 등록된
 *   work를 꺼내 실행.
 * 피호출자: bio_integrity_verify(), bio_integrity_finish(), bio_endio().
 * 에러 경로: 검증 실패는 예외(에러 리턴)가 아니라 bio->bi_status에
 *   에러 코드를 기록하는 방식으로 전달되며, bio_endio()가 그 값을 보고
 *   상위 계층에 실패를 알린다.
 *
 * 호출 체인:
 *   kintegrityd worker -> [bio_integrity_verify_fn]
 *     -> bio_integrity_verify -> bio_integrity_finish -> bio_endio
 */
static void bio_integrity_verify_fn(struct work_struct *work)
{
	struct bio_integrity_data *bid =
		container_of(work, struct bio_integrity_data, work);
		/* [한국어] work_struct 필드의 오프셋을 이용해 이를 포함하는
		 * bio_integrity_data 구조체 전체의 시작 주소를 역산한다 -
		 * __bio_integrity_endio()가 &bid->work를 큐잉했으므로 이
		 * 계산이 정확히 원래의 bid를 되돌려준다. */
	struct bio *bio = bid->bio;
		/* [한국어] bid에 저장해 둔 원본 bio 포인터를 지역 변수로
		 * 꺼내 이후 코드를 간결하게 한다. */

	bio->bi_status = bio_integrity_verify(bio, &bid->saved_bio_iter);
		/* [한국어] saved_bio_iter(검증 이전 시점의 데이터 범위)를
		 * 기준으로 실제 Guard/Reference/Application Tag 검증을
		 * 수행하고, 그 결과(BLK_STS_OK 또는 BLK_STS_IOERR 계열
		 * 에러)를 bio->bi_status에 직접 대입한다 - bio_endio()가
		 * 이 필드를 읽어 상위 계층에 성공/실패를 알린다. */
	bio_integrity_finish(bid);
		/* [한국어] 검증이 끝났으므로 bid와 PI 메타데이터 버퍼를
		 * 즉시 정리한다 - bio_endio() 호출 전에 정리해야, 상위
		 * 계층이 bio를 재사용/해제하는 시점에 이 파일의 상태가
		 * 남아있지 않다. */
	bio_endio(bio);
		/* [한국어] bio->bi_status에 담긴 검증 결과를 가지고 상위
		 * 계층(파일시스템 등)에 완료를 통지한다 - 이 호출이 바로
		 * __bio_integrity_endio()가 false를 반환하며 미뤄두었던
		 * "진짜" 완료 처리이다. */
}

#define BIP_CHECK_FLAGS (BIP_CHECK_GUARD | BIP_CHECK_REFTAG | BIP_CHECK_APPTAG)
	/* [한국어] bip_flags 중 소프트웨어 검사가 필요한 세 종류(Guard
	 * CRC/Reference Tag/Application Tag)를 한 번에 검사하기 위한
	 * 비트마스크. bio_integrity_setup_default()가 디스크의
	 * blk_integrity 프로필(csum_type, BLK_INTEGRITY_REF_TAG 플래그
	 * 등)에 따라 이 비트들 중 일부를 bip_flags에 켠다. 이 파일은 그
	 * 결과를 bip_should_check()으로 다시 조회해 소프트웨어 생성/검증
	 * 여부를 결정한다. */

/*
 * [한국어]
 * bip_should_check - 소프트웨어로 검증/생성이 필요한 PI 필드가 있는지 확인
 *
 * @bip: 검사할 bio_integrity_payload(이 파일에서는 항상
 *   bio_integrity_data.bip의 주소).
 * @return: BIP_CHECK_FLAGS 중 하나라도 켜져 있으면 true(검증/생성 필요),
 *   전부 꺼져 있으면 false(디스크 컨트롤러가 자체적으로 처리하거나 애초에
 *   검사할 필드가 없음을 의미).
 *
 * 동기/배경: bio_integrity_setup_default()는 blk_integrity 프로필의
 * BLK_INTEGRITY_NOVERIFY/NOGENERATE 플래그와 디바이스의 offload 가능
 * 여부에 따라 bip_flags에 BIP_CHECK_* 비트를 켤 수도, 켜지 않을 수도
 * 있다. 이 함수는 그 결과만 보고 "이 bio에 대해 소프트웨어가 실제로
 * Guard/Reference/Application Tag를 계산하거나 비교해야 하는가"를
 * 한 번에 판별하는 얇은 헬퍼다.
 * 동작: bip->bip_flags를 BIP_CHECK_FLAGS로 마스킹한 뒤 그 결과의 참/거짓
 * 만 반환한다.
 * 실행 컨텍스트: 호출자에 종속적 — bio_integrity_prep()에서 호출될 때는
 * 제출자의 프로세스 컨텍스트, __bio_integrity_endio()에서 호출될 때는
 * 완료 인터럽트/softirq 컨텍스트. 순수 조회 함수라 부작용이 없어 어느
 * 컨텍스트에서 불러도 안전하다.
 * 호출자: bio_integrity_prep()(WRITE 시 생성 여부 결정),
 *   __bio_integrity_endio()(READ 시 지연 검증 여부 결정).
 * 피호출자: 없음(단순 비트 연산).
 * 에러 경로: 없음 — 항상 정의된 bool 값을 반환.
 */
static bool bip_should_check(struct bio_integrity_payload *bip)
{
	return bip->bip_flags & BIP_CHECK_FLAGS;
		/* [한국어] BIP_CHECK_GUARD/REFTAG/APPTAG 중 하나라도 설정돼
		 * 있으면 0이 아닌 값(true로 변환)을, 셋 다 꺼져 있으면 0
		 * (false)을 반환한다. */
}

/**
 * __bio_integrity_endio - Integrity I/O completion function
 * @bio:	Protected bio
 *
 * Normally I/O completion is done in interrupt context.  However, verifying I/O
 * integrity is a time-consuming task which must be run in process context.
 *
 * This function postpones completion accordingly.
 */
/*
 * [한국어]
 * __bio_integrity_endio - 완료된 bio에 대해 지연 PI 검증이 필요한지
 * 판단하고 위임하는 완료 훅
 *
 * @bio: 완료된(드라이버가 처리를 끝낸) bio. bio_integrity_endio()(blk.h,
 *   BIP_BLOCK_INTEGRITY가 설정된 경우에만)에서 그대로 전달된다.
 * @return: true=지금 즉시 완료 처리를 계속해도 됨(검증이 필요 없었거나
 *   이미 정리까지 끝난 상태), false=완료 처리를 미룸(kintegrityd가
 *   나중에 bio_endio()를 대신 호출할 것이므로 호출자는 지금 더 이상
 *   진행하면 안 됨).
 *
 * 동기/배경: NVMe 등에서 READ가 완료되면 컨트롤러가 데이터와 함께
 * Guard/Reference/Application Tag도 반환하는데, 이 값들을 실제 데이터와
 * 대조하는 계산은 완료 인터럽트/softirq에서 수행하기에는 무겁다. 이
 * 함수는 "지금 검증할 필요가 있는가"만 빠르게 판단하고, 필요하다면 무거운
 * 작업을 kintegrityd workqueue로 넘긴다.
 * 동작 단계:
 *   1) bio_integrity(bio)로 이 bio에 연결된 bio_integrity_payload(bip)를
 *      얻는다.
 *   2) container_of(bip, struct bio_integrity_data, bip)로 bip를 포함하는
 *      bid 전체를 역산한다 - bip는 항상 bid 구조체의 한 필드로서
 *      할당되었기 때문에 이 역산이 안전하다.
 *   3) READ 연산이고, 완료 상태에 이미 에러가 없고, bip_should_check()가
 *      참(즉 소프트웨어가 검사해야 할 PI 필드가 있음)인 경우에만 지연
 *      검증 경로로 들어간다: work를 초기화하고 kintegrityd_wq에
 *      큐잉한 뒤 false를 반환해 호출자가 지금 bio_endio를 진행하지
 *      않도록 한다.
 *   4) 그 외의 모든 경우(WRITE 완료, READ였지만 이미 에러, 또는 검사할
 *      PI 필드가 없음)에는 bio_integrity_finish()로 즉시 정리하고
 *      true를 반환해 호출자가 곧바로 완료를 진행하게 한다.
 * 실행 컨텍스트: 드라이버 완료 경로(NVMe라면 MSI-X 인터럽트 또는 그
 * 소프트 처리 경로)에서 bio_endio() -> bio_integrity_endio()를 거쳐
 * 호출되므로 인터럽트/softirq 컨텍스트로 간주해야 한다. 이 함수 자체는
 * 무거운 계산을 하지 않고 조건 판단과 큐잉만 수행해 그 컨텍스트에서도
 * 안전하다.
 * 호출자: block/blk.h의 인라인 함수 bio_integrity_endio()(bip_flags에
 *   BIP_BLOCK_INTEGRITY가 설정된 경우에만 이 함수를 호출; 설정되지
 *   않았다면 제출자가 PI를 직접 관리하는 것이므로 이 파일은 관여하지
 *   않는다).
 * 피호출자: bio_integrity(), bip_should_check(), INIT_WORK(),
 *   queue_work(), bio_integrity_finish().
 * 에러 경로: 이미 에러가 난 bio(bio->bi_status가 0이 아님)는 검증을
 *   건너뛰고 바로 정리한다 - 손상 여부를 알 수 없는 데이터를 검증해
 *   봐야 의미가 없고, 원래 에러 코드를 그대로 상위에 전달하는 것이
 *   맞기 때문이다.
 *
 * 호출 체인:
 *   bio_endio -> bio_integrity_endio -> [__bio_integrity_endio]
 *     -> (검증 필요) queue_work(kintegrityd_wq) -> bio_integrity_verify_fn
 *     -> (검증 불필요) bio_integrity_finish
 */
bool __bio_integrity_endio(struct bio *bio)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);
		/* [한국어] bio->bi_integrity(REQ_INTEGRITY가 설정된 경우에만
		 * 유효)로부터 이 bio에 연결된 PI 페이로드를 얻는다. 여기서는
		 * 호출자(bio_integrity_endio())가 이미 bip가 존재함을
		 * 확인했으므로 NULL이 아님을 전제로 사용한다. */
	struct bio_integrity_data *bid =
		container_of(bip, struct bio_integrity_data, bip);
		/* [한국어] bip 필드의 오프셋을 이용해 이를 포함하는
		 * bio_integrity_data 구조체 전체의 시작 주소를 역산한다 -
		 * bio_integrity_prep()이 mempool에서 bid 전체를 할당한 뒤
		 * 그 안의 bip 필드만 bio->bi_integrity에 연결해 두었기
		 * 때문에 이 역산이 항상 유효하다. */

	if (bio_op(bio) == REQ_OP_READ && !bio->bi_status &&
	    bip_should_check(bip)) {
		/* [한국어] 세 조건을 모두 만족할 때만 지연 검증 경로로
		 * 진입한다: (1) READ 연산이어야 함 - WRITE는 이미 생성된
		 * 태그를 다시 검증할 이유가 없다, (2) bi_status가 아직
		 * 0(에러 없음)이어야 함 - 이미 실패한 I/O의 데이터는
		 * 신뢰할 수 없으므로 검증이 무의미하다, (3)
		 * bip_should_check(bip)가 참이어야 함 - 애초에 소프트웨어가
		 * 검사할 PI 필드(Guard/RefTag/AppTag)가 하나도 없다면 검증
		 * 자체를 건너뛴다. */
		INIT_WORK(&bid->work, bio_integrity_verify_fn);
			/* [한국어] bid->work를 bio_integrity_verify_fn을
			 * 실행할 work_struct로 초기화한다 - 큐잉 전에 매번
			 * 다시 초기화해 이전 사용 흔적(연결 리스트 포인터
			 * 등)을 지운다. */
		queue_work(kintegrityd_wq, &bid->work);
			/* [한국어] kintegrityd workqueue에 검증 작업을
			 * 등록한다 - 이 호출이 반환된 시점부터 워커 스레드가
			 * 임의의 시점에 bio_integrity_verify_fn()을 실행할
			 * 수 있으므로, 이 함수는 이후 bid/bio를 더 이상
			 * 건드리지 않는다. */
		return false;
			/* [한국어] 호출자(bio_integrity_endio() 및 그
			 * 호출자인 bio_endio())에게 "아직 완료 처리를 끝내지
			 * 말라"고 알린다 - 실제 완료 통지는 나중에
			 * bio_integrity_verify_fn()이 대신 bio_endio()를
			 * 호출하는 시점에 이루어진다. */
	}

	bio_integrity_finish(bid);
		/* [한국어] 지연 검증이 필요 없는 모든 경우(WRITE 완료,
		 * READ였지만 이미 에러, 검사할 PI 필드 없음)에는 지금 바로
		 * bid와 PI 버퍼를 정리한다. */
	return true;
		/* [한국어] 호출자에게 "지금 완료 처리를 계속 진행해도 된다"
		 * 고 알린다 - bio_integrity_endio()는 이 값을 그대로
		 * bio_endio()의 진행 여부 판단에 사용한다. */
}

/**
 * bio_integrity_prep - Prepare bio for integrity I/O
 * @bio:	bio to prepare
 * @action:	preparation action needed (BI_ACT_*)
 *
 * Allocate the integrity payload.  For writes, generate the integrity metadata
 * and for reads, setup the completion handler to verify the metadata.
 *
 * This is used for bios that do not have user integrity payloads attached.
 */
/*
 * [한국어]
 * bio_integrity_prep - PI(Protection Information)가 없는 bio에 대해
 * 무결성 처리를 준비
 *
 * @bio: 준비할 대상 bio. 제출자가 스스로 PI를 붙이지 않았음을
 *   bio_integrity_action()이 이미 확인한 상태로 넘어온다.
 * @action: 필요한 조치의 비트마스크(BI_ACT_BUFFER=버퍼 할당,
 *   BI_ACT_CHECK=소프트웨어 생성/검증 수행, BI_ACT_ZERO=버퍼를 0으로
 *   초기화). block/bio-integrity.c의 __bio_integrity_action()이
 *   REQ_OP_READ/WRITE와 blk_integrity 프로필(NOVERIFY/NOGENERATE,
 *   컨트롤러 offload 가능 여부, metadata_size 대 pi_tuple_size 비교
 *   등)을 근거로 계산해 호출자(blk_mq_submit_bio())로부터 전달된다.
 * @return: 없음(void). 실패 시에도 별도 에러 반환 없이 하위 함수의
 *   GFP_NOIO/panic 정책을 그대로 따른다(즉 이 경로는 항상 성공을
 *   전제로 설계돼 있다).
 *
 * 동기/배경: 파일시스템 등 상위 계층이 PI를 스스로 준비하지 않았더라도,
 * 디스크가 PI 포맷으로 만들어져 있다면 커널이 대신 무결성 보호를
 * 제공해야 한다. 이 함수가 그 "대신 준비"를 전담한다.
 * 동작 단계:
 *   1) bid_pool에서 GFP_NOIO로 bio_integrity_data를 확보한다 - I/O
 *      제출 경로이므로 일반 GFP_KERNEL 회수가 재귀적으로 또 다른
 *      I/O를 유발하는 것을 피해야 한다.
 *   2) bio_integrity_init()으로 bip/bvec를 bio에 연결한다.
 *   3) bid->bio에 원본 bio를 저장해 완료 시점에 되찾을 수 있게 한다.
 *   4) bip_flags에 BIP_BLOCK_INTEGRITY를 켜서 "이 PI는 제출자가 아니라
 *      블록 계층이 소유/생성했다"는 것을 표시한다 - 이 플래그가
 *      block/blk.h의 bio_integrity_endio()가 __bio_integrity_endio()를
 *      호출할지 말지를 가르는 기준이 된다.
 *   5) BI_ACT_ZERO 여부에 따라 버퍼를 0으로 초기화할지 결정하며
 *      bio_integrity_alloc_buf()로 실제 메모리를 확보한다.
 *   6) BI_ACT_CHECK가 설정된 경우에만 bio_integrity_setup_default()로
 *      기본 seed(시작 섹터)와 BIP_CHECK_* 플래그를 설정한다 - 이
 *      플래그가 없으면 버퍼는 있지만 이후 bip_should_check()는 항상
 *      false가 되어 소프트웨어 생성/검증을 건너뛴다.
 *   7) WRITE이면서 bip_should_check()가 참이면 즉시
 *      bio_integrity_generate()로 Guard/Reference/Application Tag를
 *      계산해 버퍼를 채운다.
 *   8) 그렇지 않으면(주로 READ) 나중에 __bio_integrity_endio()가 지연
 *      검증을 수행할 수 있도록 bio->bi_iter를 saved_bio_iter에 저장해
 *      둔다.
 * 실행 컨텍스트: 제출자의 프로세스 컨텍스트(블록 계층 제출 경로 내부).
 * 인터럽트 컨텍스트에서 호출되지 않으므로 GFP_NOIO만으로 충분하다.
 * 호출자: block/blk-mq.c의 blk_mq_submit_bio() — bio_integrity_action()
 *   이 0이 아닌 값을 반환했을 때만 호출한다.
 * 피호출자: bio_integrity_init(), bio_integrity_alloc_buf(),
 *   bio_integrity_setup_default(), bio_integrity_generate(),
 *   bip_should_check().
 * 에러 경로: mempool_alloc(GFP_NOIO)는 예약된 BIO_POOL_SIZE개 덕분에
 *   사실상 항상 성공하도록 설계돼 있어, 이 함수 내부에는 실패를
 *   가정한 분기가 없다.
 *
 * 호출 체인:
 *   blk_mq_submit_bio -> bio_integrity_action -> [bio_integrity_prep]
 *     -> bio_integrity_init / bio_integrity_alloc_buf
 *     -> (WRITE) bio_integrity_generate
 */
void bio_integrity_prep(struct bio *bio, unsigned int action)
{
	struct bio_integrity_data *bid;
		/* [한국어] 이 bio 전용으로 mempool에서 확보할 무결성 상태를
		 * 담을 지역 포인터. */

	bid = mempool_alloc(&bid_pool, GFP_NOIO);
		/* [한국어] I/O 제출 경로이므로 GFP_NOIO로 할당한다 - 파일
		 * 시스템 I/O를 다시 유발할 수 있는 GFP_KERNEL 회수 경로를
		 * 피해 재귀적 락/메모리 데드락을 방지한다. bid_pool이
		 * 예약분을 갖고 있어 실질적으로 항상 성공한다. */
	bio_integrity_init(bio, &bid->bip, &bid->bvec, 1);
		/* [한국어] bio_integrity_payload(bid->bip)를 초기화하고
		 * bip_vec를 단일 슬롯(&bid->bvec, nr_vecs=1)으로 연결한
		 * 뒤, bio->bi_integrity가 &bid->bip를 가리키도록
		 * 설정한다(block/bio-integrity.c 참고) - 이후
		 * bio_integrity(bio) 호출이 이 bip를 돌려준다. */
	bid->bio = bio;
		/* [한국어] 완료 시점(bio_integrity_finish(),
		 * bio_integrity_verify_fn())에 원본 bio를 되찾을 수 있도록
		 * bid에 역참조를 저장한다. */
	bid->bip.bip_flags |= BIP_BLOCK_INTEGRITY;
		/* [한국어] "이 PI 페이로드는 제출자가 아니라 블록 계층이
		 * 생성/소유한다"는 사실을 표시한다 - block/blk.h의
		 * bio_integrity_endio()는 이 플래그가 켜져 있을 때만
		 * __bio_integrity_endio()를 호출해 검증/정리를 수행하고,
		 * 꺼져 있으면(제출자가 스스로 관리) bio_uninit()이 나중에
		 * 별도로 해제하도록 맡긴다. */
	bio_integrity_alloc_buf(bio, action & BI_ACT_ZERO);
		/* [한국어] 실제 PI 메타데이터를 담을 메모리를 확보한다.
		 * action에 BI_ACT_ZERO 비트가 있으면(예: 컨트롤러가 PI를
		 * 대신 생성하는 NOGENERATE+offload 경로에서 커널 메모리가
		 * 그대로 디스크에 유출되는 것을 막기 위해) 버퍼를 0으로
		 * 채우도록 요청한다. */
	if (action & BI_ACT_CHECK)
		/* [한국어] BI_ACT_CHECK가 설정된 경우에만 기본 검사 설정을
		 * 적용한다 - NOVERIFY/NOGENERATE이면서 컨트롤러가 자체
		 * offload를 지원하지 않는 경우 등에는 이 비트가 빠져
		 * 있을 수 있고, 그때는 버퍼만 있고 소프트웨어 검사 플래그는
		 * 켜지지 않는다. */
		bio_integrity_setup_default(bio);
		/* [한국어] bip의 seed(시작 섹터, Reference Tag 계산
		 * 기준)를 bio의 시작 섹터로 맞추고, 디스크의 blk_integrity
		 * 프로필(csum_type, BLK_INTEGRITY_REF_TAG)에 따라
		 * BIP_CHECK_GUARD/IP_CHECKSUM/REFTAG 플래그를 켠다
		 * (block/bio-integrity.c 참고) - 이 플래그들이 바로
		 * bip_should_check()가 조회하는 대상이다. */

	/* Auto-generate integrity metadata if this is a write */
	if (bio_data_dir(bio) == WRITE && bip_should_check(&bid->bip))
		/* [한국어] WRITE이면서(디스크로 나가는 방향), 방금 설정된
		 * BIP_CHECK_* 플래그 중 하나라도 켜져 있어 소프트웨어가
		 * 실제로 태그를 계산해야 하는 경우에만 즉시 생성 경로로
		 * 들어간다. WRITE인데도 이 조건이 거짓이면(예:
		 * BI_ACT_CHECK가 아예 없었던 경우) 버퍼는 있지만 내용은
		 * 컨트롤러가 채우도록 맡겨진다. */
		bio_integrity_generate(bio);
		/* [한국어] block/t10-pi.c 계열 구현을 통해 데이터 각
		 * 블록에 대응하는 Guard(CRC 등 체크섬)/Reference
		 * Tag/Application Tag를 계산해 방금 할당한 버퍼에 채운다 -
		 * 이 값들이 실제로 디스크(또는 NVMe 컨트롤러)에 메타데이터
		 * 형태로 전달된다. */
	else
		/* [한국어] WRITE가 아니거나(주로 READ), WRITE라도 검사할
		 * 필드가 없는 경우 이 분기로 들어온다 - READ의 경우 아직
		 * 데이터가 도착하지 않았으므로 지금은 생성/검증할 것이
		 * 없고, 완료 시점에 검증하기 위한 준비만 해 둔다. */
		bid->saved_bio_iter = bio->bi_iter;
		/* [한국어] 현재 bio->bi_iter(아직 아무 진행도 없는 시작
		 * 상태)를 그대로 복사해 둔다 - 완료 시점에 실제 I/O 처리로
		 * 인해 bi_iter 자체가 전진해 있을 수 있으므로, 검증에 쓸
		 * 원본 범위를 미리 스냅샷으로 남겨두는 것이다. */
}
EXPORT_SYMBOL(bio_integrity_prep);
	/* [한국어] blk-mq 코어(block/blk-mq.c) 이외에, 이 함수를 직접
	 * 호출해야 할 수 있는 다른 인트리(in-tree)/아웃오브트리
	 * 모듈에서도 링크 가능하도록 심볼을 공개한다. */

/*
 * [한국어]
 * blk_flush_integrity - kintegrityd workqueue에 남아있는 모든 지연 검증
 * work가 끝날 때까지 대기
 *
 * @return: 없음(void). 반환 시점에는 이전에 큐잉된 모든
 *   bio_integrity_verify_fn() 실행이 완료되어 있음이 보장된다.
 *
 * 동기/배경: 큐(request_queue)나 디스크가 제거되는 등 이 서브시스템이
 * 참조하던 자료구조(디스크, blk_integrity 프로필 등)가 곧 해제될 상황
 * 에서는, 아직 실행 중이거나 대기 중인 검증 work가 이미 사라진 자료를
 * 건드리지 않도록 먼저 모두 끝내 두어야 한다.
 * 동작: flush_workqueue(kintegrityd_wq) 한 줄로, workqueue 코어에
 * "현재 큐잉되어 있거나 실행 중인 모든 work가 끝날 때까지 이 함수
 * 호출자를 재워 달라"고 요청한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(디스크/큐 해제 경로) — 대기(sleep)가
 * 가능한 컨텍스트여야 하며, 인터럽트 컨텍스트에서 호출하면 안 된다.
 * 호출자: 디스크/큐 해제 경로(예: del_gendisk() 계열, 블록 디바이스
 *   드라이버 정리 경로) — 이 파일 자체에는 정확한 호출자가 나타나지
 *   않으며 blk.h를 통해 다른 block/*.c 파일에 노출된다.
 * 피호출자: flush_workqueue().
 * 에러 경로: 없음 — flush_workqueue()는 값을 반환하지 않고 항상 대기를
 *   완료한 뒤 복귀한다.
 *
 * 호출 체인:
 *   (디스크/큐 해제 경로) -> [blk_flush_integrity] -> flush_workqueue
 */
void blk_flush_integrity(void)
{
	flush_workqueue(kintegrityd_wq);
		/* [한국어] kintegrityd_wq에 큐잉되어 있거나 현재 실행 중인
		 * 모든 work(bio_integrity_verify_fn 인스턴스들)가 전부
		 * 끝날 때까지 호출자를 블록시킨다 - 반환 후에는 이
		 * workqueue를 통해 지연된 검증이 하나도 남아있지 않음이
		 * 보장된다. */
}

/*
 * [한국어]
 * blk_integrity_auto_init - 자동 PI 모듈(서브시스템) 초기화
 *
 * @return: 0=성공. 실패 시에는 값을 반환하지 않고 panic()으로 즉시
 *   부팅을 중단시키므로, 호출자 입장에서는 사실상 항상 0만 받는다.
 *
 * 동기/배경: bio_integrity_prep()과 bio_integrity_finish()가 I/O
 * 제출/완료라는 매우 빈번한 경로에서 매번 새 메모리를 할당/해제하지
 * 않도록, 부팅 초기에 전용 슬랩 캐시와 예약 mempool, 그리고 지연 검증에
 * 쓸 workqueue를 미리 만들어 둔다.
 * 동작 단계:
 *   1) kmem_cache_create()로 bio_integrity_data 전용 슬랩(bid_slab)을
 *      만든다. SLAB_HWCACHE_ALIGN으로 캐시 라인 경계에 맞춰 정렬해
 *      false sharing을 줄이고, SLAB_PANIC으로 생성 실패 시 커널이
 *      스스로 panic하게 한다(이 서브시스템 없이는 안전한 I/O 무결성
 *      보장이 불가능하다고 판단하기 때문).
 *   2) mempool_init_slab_pool()로 bid_slab을 백엔드로 하는
 *      BIO_POOL_SIZE(2)개짜리 예약 mempool(bid_pool)을 만든다.
 *      실패하면(매우 이례적인 초기 메모리 부족) panic으로 즉시
 *      중단한다 - 이 mempool이 없으면 GFP_NOIO 보장이 깨져 I/O
 *      경로에서 교착 위험이 생기기 때문이다.
 *   3) alloc_workqueue()로 kintegrityd workqueue를 생성한다.
 *      WQ_MEM_RECLAIM은 메모리 회수 경로에서도 이 workqueue가 진행될
 *      수 있도록 전용 rescuer 스레드를 붙이고, WQ_HIGHPRI는 검증
 *      지연을 줄이기 위해 높은 스케줄링 우선순위를 준다,
 *      WQ_CPU_INTENSIVE는 CPU를 오래 쓰는 work로 표시해 동시성 제한
 *      계산에서 다른 일반 work들에 영향을 덜 주게 한다. max_active를
 *      1로 지정해 동시에 최대 하나의 검증 work만 실행되게 해 캐시
 *      효율을 높인다(원 주석: kintegrityd는 거의 블록하지 않지만 CPU
 *      사이클은 많이 태울 수 있다).
 *   4) workqueue 생성이 실패하면 panic으로 즉시 중단한다.
 *   5) 성공 시 0을 반환한다.
 * 실행 컨텍스트: 커널 부팅 시퀀스 중 subsys_initcall 단계 — 아직
 * 멀티태스킹/인터럽트가 완전히 열려 있지 않은 이른 시점의 단일 흐름
 * 컨텍스트다.
 * 호출자: 커널 초기화 인프라(do_initcalls())가
 *   subsys_initcall(blk_integrity_auto_init) 매크로가 등록한 함수
 *   포인터를 통해 자동으로 호출한다.
 * 피호출자: kmem_cache_create(), mempool_init_slab_pool(), panic(),
 *   alloc_workqueue().
 * 에러 경로: 이 함수의 두 가지 실패 지점(mempool 초기화 실패,
 *   workqueue 생성 실패)은 모두 panic()으로 이어진다 - 정상적인 에러
 *   반환 대신 부팅 자체를 중단시키는 것은, PI 자동 처리 인프라 없이
 *   시스템을 계속 띄우면 조용한 데이터 손상을 감지하지 못한 채
 *   운영될 위험이 있다고 보기 때문이다.
 *
 * 호출 체인:
 *   do_initcalls -> [blk_integrity_auto_init]
 *     -> kmem_cache_create / mempool_init_slab_pool / alloc_workqueue
 */
static int __init blk_integrity_auto_init(void)
{
	bid_slab = kmem_cache_create("bio_integrity_data",
			sizeof(struct bio_integrity_data), 0,
			SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);
		/* [한국어] "bio_integrity_data"라는 이름으로,
		 * bio_integrity_data 구조체 크기만큼의 객체를 찍어낼 슬랩
		 * 캐시를 만든다. 정렬 인자 0은 기본 정렬을 쓰겠다는 뜻이고,
		 * SLAB_HWCACHE_ALIGN은 그 위에 하드웨어 캐시 라인 경계
		 * 정렬을 추가로 요구한다. SLAB_PANIC이 설정돼 있으므로 이
		 * 호출 자체가 실패하면 이 줄에서 이미 커널이 panic하며,
		 * 마지막 인자 NULL은 객체 생성자(constructor) 콜백이
		 * 없음을 뜻한다. */

	if (mempool_init_slab_pool(&bid_pool, BIO_POOL_SIZE, bid_slab))
		/* [한국어] bid_slab을 백엔드로 삼아 BIO_POOL_SIZE(2)개의
		 * bio_integrity_data 객체를 미리 확보해 두는 mempool을
		 * 초기화한다. 이 함수가 0이 아닌 값(실패)을 반환하는
		 * 경우로 들어온 조건문이다. */
		panic("bio: can't create integrity pool\n");
		/* [한국어] mempool 예약 실패 시 즉시 부팅을 중단한다 - 이
		 * 풀이 없으면 bio_integrity_prep()의
		 * mempool_alloc(GFP_NOIO)가 I/O 경로에서 실패하거나 무한정
		 * 대기할 위험이 있어, 이런 이례적 상황에서는 계속 부팅을
		 * 진행하는 것보다 즉시 알리는 편이 안전하다고 판단한
		 * 것이다. */

	/*
	 * kintegrityd won't block much but may burn a lot of CPU cycles.
	 * Make it highpri CPU intensive wq with max concurrency of 1.
	 */
	kintegrityd_wq = alloc_workqueue("kintegrityd", WQ_MEM_RECLAIM |
					 WQ_HIGHPRI | WQ_CPU_INTENSIVE, 1);
		/* [한국어] "kintegrityd"라는 이름의 전용 workqueue를 만든다.
		 * WQ_MEM_RECLAIM: 메모리 회수 경로에서도 진행을 보장하기
		 * 위한 전용 rescuer 스레드를 붙인다. WQ_HIGHPRI: 검증
		 * 지연을 줄이기 위해 일반 workqueue보다 높은 우선순위로
		 * 스케줄된다. WQ_CPU_INTENSIVE: CPU를 많이 쓰는 work로
		 * 표시해, 동시성 제한 계산 시 이 workqueue의 work가 다른
		 * per-CPU work들의 동시 실행 기회를 빼앗지 않도록 한다.
		 * 마지막 인자 1은 max_active(동시에 실행할 수 있는 work의
		 * 최대 개수)로, kintegrityd 안에서는 항상 최대 1개의 검증만
		 * 동시에 돌게 제한한다. */
	if (!kintegrityd_wq)
		/* [한국어] alloc_workqueue()가 NULL을 반환(할당 실패)했는지
		 * 검사한다. */
		panic("Failed to create kintegrityd\n");
		/* [한국어] workqueue 생성 실패 시 즉시 부팅을 중단한다 -
		 * 이 workqueue가 없으면 __bio_integrity_endio()의
		 * queue_work() 호출이 애초에 불가능해 READ PI 검증 경로
		 * 전체가 성립하지 않기 때문이다. */
	return 0;
		/* [한국어] 여기까지 도달했다면 슬랩/mempool/workqueue가
		 * 모두 준비된 것이므로 initcall 성공(0)을 반환한다. */
}
subsys_initcall(blk_integrity_auto_init);
	/* [한국어] 커널 부팅 초기화 단계 중 subsys_initcall 레벨에서 이
	 * 함수를 자동 실행되도록 등록한다 - 블록 서브시스템 자체가 이
	 * 레벨에서 초기화되므로, 이후 등록되는 개별 블록 드라이버(NVMe
	 * 등)가 PI가 활성화된 디스크를 등록하기 전에 bid_pool/kintegrityd_wq
	 * 가 이미 준비되어 있음을 보장한다. */

/*
 * ========================================================================
 * [한국어] 자동 PI 경로 핵심 요약 (파일 하단 보충 설명)
 * ========================================================================
 * - 이 파일은 상위(파일시스템 등)가 PI를 제공하지 않은 경우, 블록 계층이
 *   대신 T10 PI(Guard/Reference Tag/Application Tag)를 생성하고 READ 완료
 *   시 검증하는 자동 무결성 경로를 구현한다.
 *
 * - bio_integrity_prep()이 설정하는 BIP_BLOCK_INTEGRITY 플래그가 바로
 *   "이 PI는 블록 계층이 소유한다"는 표식이며, block/blk.h의
 *   bio_integrity_endio()는 이 플래그를 보고 __bio_integrity_endio()를
 *   호출할지(이 파일이 관여) 아니면 제출자가 알아서 정리하도록 맡길지를
 *   가른다.
 *
 * - WRITE는 bio_integrity_prep() 단계에서 이미 태그를 계산해 버퍼에
 *   채워두므로, 완료 경로에서는 별도 지연 처리 없이 bio_integrity_finish()
 *   로 즉시 정리된다.
 *
 * - READ는 __bio_integrity_endio()에서 진짜 완료 처리를 뒤로 미루고,
 *   kintegrityd workqueue의 bio_integrity_verify_fn()이 실제 검증과
 *   bio_endio() 호출을 대신 수행한다 - 이렇게 함으로써 CPU 비용이 큰 검증
 *   계산이 완료 인터럽트/softirq의 지연시간에 영향을 주지 않는다.
 *
 * - 전체 블록 무결성 처리 순서는 대략 다음과 같다:
 *   block/bio-integrity.c(코어 API) -> block/bio-integrity-auto.c(현재
 *   파일, 자동 생성/검증 정책) -> block/t10-pi.c(실제 Guard/RefTag/AppTag
 *   계산 알고리즘). block/bio-integrity-fs.c는 이 파일과 대칭적인 경로로,
 *   파일시스템이 스스로 PI를 준비한 경우를 다룬다.
 * ========================================================================
 */
