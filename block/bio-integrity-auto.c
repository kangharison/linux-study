// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2007, 2008, 2009 Oracle Corporation
 * Written by: Martin K. Petersen <martin.petersen@oracle.com>
 *
 * Automatically generate and verify integrity data on PI capable devices if the
 * bio submitter didn't provide PI itself.  This ensures that kernel verifies
 * data integrity even if the file system (or other user of the block device) is
 * not aware of PI.
 *
 * ========================================================================
 * [NVMe 관점 파일 개요]
 * 이 파일은 상위 계층(파일시스템 등)이 Protection Information(PI)를 제공하지
 * 않았을 때, 커널이 자동으로 T10 PI 메타데이터를 생성/검증하는 경로를 담당한다.
 * NVMe SSD에서 End-to-End Data Protection(E2E, DIF/DIX)을 사용하려면 각 I/O에
 * Guard(예: CRC), Reference Tag, Application Tag가 포함된 PRP/SGL 메모리
 * 영역이 필요하다. 이 파일은 그 메타데이터를 자동으로 채워주는 블록 계층의
 * 보조 모듈이다.
 *
 * 전체 블록 무결성 흐름에서의 위치:
 *   block/bio-integrity.c    : bio_integrity 핵심 API
 *   block/bio-integrity-fs.c : 파일시스템이 직접 PI를 제공한 경우
 *   block/bio-integrity-auto.c (현재 파일) : 커널이 자동으로 PI 생성/검증
 *   block/t10-pi.c           : T10 PI Guard/Reference Tag 알고리즘
 *
 * NVMe I/O 경로와의 연결 예시:
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *             -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *             -> NVMe controller가 PRP/SGL과 함께 Guard/RT/AT를 해석
 */
#include <linux/blk-integrity.h>
#include <linux/t10-pi.h>
#include <linux/workqueue.h>
#include "blk.h"

/*
 * bio_integrity_data - 자동 PI 처리를 위해 필요한 임시 상태 구조체
 *
 * 이 구조체는 상위 계층이 제공하지 않은 무결성 메타데이터를 커널이 자동으로
 * 생성하거나 완료 시 검증할 때 사용하는 낮은 수준의 상태이다. NVMe 입장에서는
 * 각 bio가 실제 PRP/SGL 데이터 외에 추가로 전달해야 할 PI 정보를 담는
 * 컨테이너라고 볼 수 있다.
 */
struct bio_integrity_data {
	struct bio			*bio;
		/*
		 * NVMe PRP/SGL에 매핑될 실제 데이터 bio.
		 * submit_bio -> blk_mq_submit_bio 경로에서 전달되며,
		 * NVMe 드라이버는 이 bio의 데이터 버퍼를 PRP 리스트로 변환한다.
		 */
	struct bvec_iter		saved_bio_iter;
		/*
		 * READ 완료 후 PI 검증을 수행할 때 원본 bio_iter를 복원하기 위해
		 * 저장해 둔다. NVMe 컨트롤러가 CQ(Completion Queue)에 CID와 함께
		 * 완료 응답을 볼 때, 커널은 이 saved_bio_iter를 기준으로 데이터와
		 * 메타데이터를 다시 맞춰 검증한다.
		 */
	struct work_struct		work;
		/*
		 * PI 검증은 인터럽트 컨텍스트에서 수행하기에 CPU 집약적이므로
		 * kintegrityd workqueue로 지연 실행된다. NVMe MSI-X ISR에서
		 * bio_endio 이전에 검증 작업을 queue_work()한다.
		 */
	struct bio_integrity_payload	bip;
		/*
		 * 블록 무결성 서브시스템에서 사용하는 메타데이터 디스크립터.
		 * NVMe E2E에서는 Guard/Reference Tag/Application Tag의 위치와
		 * 길이를 정의하는 역할을 한다.
		 */
	struct bio_vec			bvec;
		/*
		 * PI 메타데이터가 담긴 단일 bio_vec. NVMe SGL이나 PRP에
		 * 메타데이터 버퍼를 노출할 때 사용된다 (추정).
		 */
};

static struct kmem_cache *bid_slab;
	/* NVMe E2E PI용 bio_integrity_data 객체 슬랩: SQ 제출 시 CID/tag 단위로 할당됨 */
static mempool_t bid_pool;
	/* GFP_NOIO 예약 풀: NVMe queue_rq 경로에서 메모리 재귀 할당(deadlock) 방지 */
static struct workqueue_struct *kintegrityd_wq;
	/* NVMe MSI-X ISR -> __bio_integrity_endio -> queue_work 지연 PI 검증 워크큐 */

/*
 * bio_integrity_finish - 자동 생성된 PI 상태를 정리하고 bio에서 분리
 *
 * 목적:
 *   I/O 완료 후 bio와 무결성 메타데이터 간의 연결을 끊고, 할당받은
 *   bio_integrity_data 객체를 메모리 풀에 반환한다.
 *
 * 호출 경로:
 *   __bio_integrity_endio (읽기 검증 생략 시 또는 WRITE 완료 직접)
 *     -> bio_integrity_finish
 *   bio_integrity_verify_fn (지연 검증 완료 후)
 *     -> bio_integrity_finish -> bio_endio
 *
 * NVMe 연결:
 *   NVMe 컨트롤러가 CQ에 상태를 기록하고 ISR이 bio_endio를 부르기 전,
 *   이 함수로 인해 bio는 더 이상 PI 페이로드를 참조하지 않는다.
 */
static void bio_integrity_finish(struct bio_integrity_data *bid)
	/* NVMe CQ 완료 후 bio->bip 분리 및 PRP/SGL 메타데이터 해제 경로 */
{
	bid->bio->bi_integrity = NULL;
		/* bio에서 무결성 페이로드 참조 제거: NVMe SQ/CID 완료 후 bio 재사용 방지 */
	bid->bio->bi_opf &= ~REQ_INTEGRITY;
		/* REQ_INTEGRITY 플래그 클리어: 이 bio는 이제 일반 I/O로 취급 */
	bio_integrity_free_buf(&bid->bip);
		/* PI 메타데이터 버퍼 해제: NVMe SGL/PRP에 매핑되었던 메모리 반납 */
	mempool_free(bid, &bid_pool);
		/* bio_integrity_data 구조체를 사전 할당 풀에 반환 */
}

/*
 * bio_integrity_verify_fn - 프로세스 컨텍스트에서 PI 검증을 수행하는 work handler
 *
 * 목적:
 *   kintegrityd workqueue에 의해 스케줄되며, 저장해 둔 saved_bio_iter를
 *   기준으로 읽어온 데이터의 T10 PI 메타데이터를 검증한다.
 *
 * 호출 경로:
 *   NVMe MSI-X ISR -> __bio_integrity_endio -> queue_work(kintegrityd_wq)
 *     -> (나중에) bio_integrity_verify_fn
 *     -> bio_integrity_verify -> bio_integrity_finish -> bio_endio
 *
 * NVMe 연결:
 *   NVMe controller가 데이터와 함께 Guard/Reference Tag를 반환하면,
 *   이 work 함수가 실제로 그 값이 손상되지 않았는지 확인한다.
 */
static void bio_integrity_verify_fn(struct work_struct *work)
	/* kintegrityd 워커: NVMe READ CQ 완료 후 PRP/SGL 메모리의 T10 PI 검증 */
{
	struct bio_integrity_data *bid =
		container_of(work, struct bio_integrity_data, work);
		/* work_struct에서 bid 복원: CID 완료 항목과 연결된 무결성 상태 */
	struct bio *bio = bid->bio;
		/* NVMe controller가 CQ로 완료한 원본 bio 참조 */

	bio->bi_status = bio_integrity_verify(bio, &bid->saved_bio_iter);
		/*
		 * saved_bio_iter 기준으로 데이터와 PI를 재맞춤.
		 * NVMe READ 완료 후 PRP/SGL이 가리키는 메모리에서
		 * Guard/Reference Tag/App Tag 일치 여부를 확인한다.
		 */
	bio_integrity_finish(bid);
		/* 검증 완료 후 bid 해제: NVMe CID 완료 후 bio와 PI 페이로드 분리 */
	bio_endio(bio);
		/* 검증 결과 bio->bi_status를 설정한 뒤 상위 계층으로 완료 전파 */
}

#define BIP_CHECK_FLAGS (BIP_CHECK_GUARD | BIP_CHECK_REFTAG | BIP_CHECK_APPTAG)
	/* NVMe E2E DIF Type 1/2/3에서 검사할 Guard/Reference/App Tag 플래그 마스크 */

/*
 * bip_should_check - 검증이 필요한 PI 필드가 있는지 확인
 *
 * 목적:
 *   bip->bip_flags 중 BIP_CHECK_* 플래그가 설정되어 있으면
 *   커널이 메타데이터 검증을 수행해야 함을 나타낸다.
 *
 * NVMe 연결:
 *   NVMe E2E DIF는 Guard, Reference Tag, App Tag 중 일부 또는 전체를
 *   체크할 수 있다. 이 플래그는 NVMe namespace에서 활성화된 PI 형식과
 *   대응한다 (추정).
 */
static bool bip_should_check(struct bio_integrity_payload *bip)
	/* NVMe namespace PI 형식에 따라 체크할 PI 필드 존재 여부 판단 */
{
	return bip->bip_flags & BIP_CHECK_FLAGS;
		/* BIP_CHECK_* 비트 테스트: NVMe E2E에서 체크할 PI 필드 마스킹 */
}

/**
 * __bio_integrity_endio - Integrity I/O completion function
 * @bio:	Protected bio
 *
 * Normally I/O completion is done in interrupt context.  However, verifying I/O
 * integrity is a time-consuming task which must be run in process context.
 *
 * This function postpones completion accordingly.
 *
 * [NVMe 관점 추가 설명]
 * 목적:
 *   NVMe READ I/O가 완료되었을 때, 인터럽트 컨텍스트에서 직접 PI 검증을
 *   하지 않고 프로세스 컨텍스트로 미루어 검증이 끝난 후 bio_endio를 호출한다.
 *
 * 호출 경로:
 *   NVMe ISR -> nvme_complete_rq -> bio_endio -> __bio_integrity_endio
 *     -> (검증 필요 시) queue_work(kintegrityd_wq, &bid->work)
 *     -> (검증 불필요/WRITE 시) bio_integrity_finish -> true 반환
 *
 * NVMe 연결:
 *   NVMe controller가 CQ 항목을 post하면 doorbell을 울려 ISR이 실행되고,
 *   그 ISR 맥락에서 이 함수가 호출된다. 검증이 필요하면 ISR은 즉시
 *   bio_endio를 호출하지 않고 workqueue에 위임한다.
 */
bool __bio_integrity_endio(struct bio *bio)
	/* NVMe ISR 컨텍스트 진입 지점: CQ 항목 처리 후 PI 검증 지연 또는 즉시 완료 */
{
	struct bio_integrity_payload *bip = bio_integrity(bio);
		/* bio->bi_integrity에서 NVMe PRP/SGL과 매핑된 PI 페이로드 획득 */
	struct bio_integrity_data *bid =
		container_of(bip, struct bio_integrity_data, bip);
		/* bip를 포함한 bid 복원: CID/tag 단위 무결성 컨텍스트 */

	if (bio_op(bio) == REQ_OP_READ && !bio->bi_status &&
		/* REQ_OP_READ && NVMe CQ SC=0 성공 && 검증 플래그 설정 시 지연 검증 */
	    bip_should_check(bip)) {
		/*
		 * READ 성공이고 검증할 PI 필드가 있을 때만 지연 검증 수행.
		 * NVMe WRITE completion은 호스트 측에서 데이터를 다시 읽어
		 * 검증할 필요가 없으므로 즉시 완료 처리한다.
		 */
		INIT_WORK(&bid->work, bio_integrity_verify_fn);
		queue_work(kintegrityd_wq, &bid->work);
			/*
			 * kintegrityd workqueue에 검증 작업 등록.
			 * NVMe CQ 처리 ISR에서 CPU 소모적인 PI 검증을
			 * 인터럽트 밖으로 낮추는 핵심 지점.
			 */
		return false;
			/* NVMe ISR에서 즉시 bio_endio 미룸: kintegrityd에서 PI 검증 후 완료 */
	}

	bio_integrity_finish(bid);
		/* WRITE/오류 완료 시에도 NVMe SQ/CID용 bid 및 PI 메모리 정리 */
	return true;
		/* WRITE 또는 NVMe 오류(CQ SC != 0) 시: 상위 완료 콜백 직접 진행 */
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
 *
 * [NVMe 관점 추가 설명]
 * 목적:
 *   상위가 PI를 제공하지 않은 bio에 대해, 커널이 자동으로 PI 메타데이터
 *   영역을 할당하고 WRITE라면 메타데이터를 생성, READ라면 완료 시 검증을
 *   설정한다.
 *
 * 호출 경로:
 *   submit_bio -> blk_mq_submit_bio
 *     -> bio_integrity_prep (해당 블록 장치가 PI 자동 생성을 요청한 경우)
 *     -> 이후 nvme_queue_rq에서 bio의 bip 정보를 PRP/SGL에 반영
 *
 * NVMe 연결:
 *   NVMe namespace가 End-to-End Data Protection을 지원하도록 포맷되고,
 *   blk_integrity_register()로 PI 정보가 등록된 경우, bio가 NVMe SQ의
 *   PRP/SGL과 함께 전달될 PI 메모리를 이 함수에서 준비한다.
 */
void bio_integrity_prep(struct bio *bio, unsigned int action)
	/* NVMe SQ 제출 전(bio -> nvme_queue_rq 전) PI 메타데이터 준비 진입점 */
{
	struct bio_integrity_data *bid;
		/* CID/tag 단위로 할당될 무결성 컨텍스트 */

	bid = mempool_alloc(&bid_pool, GFP_NOIO);
		/*
		 * I/O 경로에서 NOIO로 사전 할당된 풀에서 bid를 꺼냄.
		 * NVMe I/O 제출 경로에서 메모리 할당 재귀(deadlock)를 방지.
		 */
	bio_integrity_init(bio, &bid->bip, &bid->bvec, 1);
		/*
		 * bio_integrity_payload 초기화.
		 * NVMe PRP/SGL에 PI 메타데이터를 노출할 bip/bvec 구조를 세팅.
		 */
	bid->bio = bio;
		/* bid와 NVMe I/O bio 연결: SQ 제출~CQ 완료까지 동일 생명주기 */
	bid->bip.bip_flags |= BIP_BLOCK_INTEGRITY;
		/* 이 메타데이터가 블록 레벨 PI임을 표시 (추정) */
	bio_integrity_alloc_buf(bio, action & BI_ACT_ZERO);
		/*
		 * PI 메타데이터 버퍼 할당.
		 * BI_ACT_ZERO가 설정되면 0으로 초기화하여 NVMe controller가
		 * App/Reference Tag를 0으로 다루도록 준비한다 (추정).
		 */
	if (action & BI_ACT_CHECK)
		/* NVMe namespace PI 검증 활성화 시 기본 태그 seed/논리 블록 기준 적용 */
		bio_integrity_setup_default(bio);
		/*
		 * 검증이 필요하면 기본 PI 설정(seed, 태그 시작값 등)을 bio에 적용.
		 * NVMe namespace의 PI 형식(타입 1/2/3)에 따른 초기 태그값 세팅.
		 */

	/* Auto-generate integrity metadata if this is a write */
	if (bio_data_dir(bio) == WRITE && bip_should_check(&bid->bip))
		/* WRITE && NVMe E2E 체크 플래그: SQ 삽입 전 Guard/RT/AT 생성 */
		bio_integrity_generate(bio);
		/*
		 * WRITE인 경우 Guard(CRC)와 Reference/App Tag를 생성.
		 * NVMe SQ에 CID로 등록되기 전, PRP/SGL 메모리의 PI 영역이
		 * 올바르게 채워지도록 보장.
		 */
	else
		/* READ: NVMe CQ 완료 후 데이터 영역 복원 기준 저장 */
		bid->saved_bio_iter = bio->bi_iter;
		/*
		 * READ인 경우 완료 후 검증을 위해 현재 bio_iter를 저장.
		 * NVMe CQ가 반환된 후에도 원본 데이터 범위를 알 수 있게 함.
		 */
}
EXPORT_SYMBOL(bio_integrity_prep);
	/* NVMe 드라이버 외 다른 저수준 드라이버에서도 PI 자동 준비 API 노출 */

/*
 * blk_flush_integrity - kintegrityd workqueue의 모든 작업이 끝날 때까지 대기
 *
 * 목적:
 *   시스템 종료이나 모듈 제거 전에 진행 중인 PI 검증을 모두 마친다.
 *
 * NVMe 연결:
 *   NVMe controller reset이나 드라이버 unload 전, 완료되지 않은
 *   READ PI 검증 work들이 메모리를 참조하지 않도록 동기화한다 (추정).
 */
void blk_flush_integrity(void)
	/* NVMe controller reset/unload 전 미완료 PI 검증 work 동기화 */
{
	flush_workqueue(kintegrityd_wq);
		/* kintegrityd 대기: NVMe controller reset 후에도 안전한 메모리 반납 보장 */
}

/*
 * blk_integrity_auto_init - 자동 PI 모듈 초기화
 *
 * 목적:
 *   bio_integrity_data 객체용 슬랩/메모리 풀과 kintegrityd workqueue를
 *   생성한다.
 *
 * 호출 경로:
 *   커널 서브시스템 초기화 시 subsys_initcall로 자동 실행.
 *
 * NVMe 연결:
 *   NVMe End-to-End Data Protection을 사용하는 I/O가 제출되기 전에
 *   사전 할당 풀이 준비되어 있어야 submit 경로에서 GFP_NOIO로
 *   bid를 빠르게 얻을 수 있다.
 */
static int __init blk_integrity_auto_init(void)
	/* 서브시스템 초기화: NVMe E2E 사용 전 bid 풀과 kintegrityd 준비 */
{
	bid_slab = kmem_cache_create("bio_integrity_data",
			sizeof(struct bio_integrity_data), 0,
			SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);
		/* bid 구조체용 하드웨어 캐시 정렬 슬랩 생성 */

	if (mempool_init_slab_pool(&bid_pool, BIO_POOL_SIZE, bid_slab))
		/* bid_pool 예약 실패: NVMe E2E I/O 제출 시 GFP_NOIO 할당 보장 불가 -> 패닉 */
		panic("bio: can't create integrity pool\n");
		/*
		 * BIO_POOL_SIZE개의 객체를 미리 예약한 mempool 초기화.
		 * NVMe I/O 경로에서 메모리 부족으로 인한 재균 할당 실패 방지.
		 */

	/*
	 * kintegrityd won't block much but may burn a lot of CPU cycles.
	 * Make it highpri CPU intensive wq with max concurrency of 1.
	 */
	kintegrityd_wq = alloc_workqueue("kintegrityd", WQ_MEM_RECLAIM |
					 WQ_HIGHPRI | WQ_CPU_INTENSIVE, 1);
		/*
		 * PI 검증은 CPU 집약적이지만 I/O를 block하지 않으므로
		 * 우선순위가 높고 CPU 집약적인 workqueue로 생성.
		 * 동시 실행 작업 수를 1로 제한하여 캐시 효율을 높임.
		 */
	if (!kintegrityd_wq)
		/* kintegrityd 실패 시 NVMe READ PI 지연 검증 경로 부재 -> 패닉 */
		panic("Failed to create kintegrityd\n");
	return 0;
		/* 초기화 성공: NVMe E2E I/O 제출 가능 상태 */
}
subsys_initcall(blk_integrity_auto_init);
	/* 커널 서브시스템 초기화 단계에서 NVMe 드라이버보다 먼저 준비 (추정) */

/*
 * ========================================================================
 * NVMe 관점 핵심 요약
 * ========================================================================
 * - 이 파일은 상위(파일시스템 등)가 PI를 제공하지 않은 경우, 커널이 대신
 *   T10 PI(Guard/Reference Tag/App Tag)를 생성하고 READ 완료 시 검증하는
 *   자동 무결성 경로를 구현한다.
 *
 * - NVMe End-to-End Data Protection이 활성화된 namespace에서, bio는
 *   데이터(PRP/SGL) 외에 추가 PI 메모리 영역(bip)을 가지며, NVMe controller는
 *   SQ submission 시 이 메모리를 CID와 함께 해석한다.
 *
 * - WRITE에서는 bio_integrity_prep() 단계에서 PI 메타데이터를 생성하여
 *   NVMe SQ에 넣기 전 PRP/SGL의 무결성 영역이 채워지도록 한다.
 *
 * - READ 완료는 NVMe ISR에서 __bio_integrity_endio()로 들어오며, PI 검증이
 *   필요하면 kintegrityd workqueue로 지연시켜 인터럽트 컨텍스트를 보호한다.
 *
 * - 전체 블록 무결성 순서는
 *   bio-integrity.c(코어) -> bio-integrity-auto.c(현재, 자동 PI) ->
 *   bio-integrity-fs.c(파일시스템 제공 PI) / t10-pi.c(PI 알고리즘)로
 *   이루어지며, 현재 파일은 자동 생성/검증의 핵심 처리를 담당한다.
 * ========================================================================
 */
