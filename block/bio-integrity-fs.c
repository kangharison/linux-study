// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Christoph Hellwig.
 */

/*
 * NVMe 관점 파일 요약
 *
 * 이 파일은 파일시스템 레벨 bio integrity(payload) 버퍼를 할당/해제/생성/검증하는
 * 블록 계층 헬퍼를 제공한다. NVMe에서는 End-to-End Data Protection(Metadata,
 * T10 PI/DIF 등)이 활성화된 namespace에서 I/O를 전송할 때 필수적인 경로로,
 * 데이터와 함께 메타데이터(보호 정보)를 SQ(Submission Queue)의 PRP/SGL 영역에
 * 포함시키기 전에 준비하는 역할을 한다.
 *
 * 상위 흐름:
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 * nvme_submit_cmd(doorbell)
 *
 * 파일계층이 데이터를 쓰기 전/읽은 후 bio_integrity_generate() /
 * fs_bio_integrity_verify()를 통해 CRC/체크섬을 생성·검증하며, 이후 NVMe
 * 드라이버가 해당 bio를 기반으로 CID를 할당받아 doorbell을 울린다.
 *
 * (추정) fs_bio_integrity_buf는 fs/direct-io 등에서 간접적으로 사용될 수 있으며,
 * 실제 NVMe metadata buffer 매핑은 nvme-driver 측의 PRP/SGL 구성 단계에서
 * 완료된다.
 */
#include <linux/blk-integrity.h>	/* NVMe namespace의 integrity profile(nvme_ns->disk->integrity)과 연결 */
#include <linux/bio-integrity.h>	/* bio_integrity_payload가 NVMe PRP/SGL 구성 시 metadata segment로 사용됨 */
#include "blk.h"			/* blk-mq tag/request 할당, hctx 선택, scheduler 관련 낮은 수준 헬퍼 포함 */

/* fs_bio_integrity_buf:
 *  - bip: NVMe metadata(Protection Information)를 담을 bio_integrity_payload.
 *         NVMe namespace의 PI 타입과 연동되어 PRP/SGL에 매핑된다.
 *  - bvec: 단일 bio_vec로, metadata 페이지를 가리킨다. NVMe I/O가 SG list를
 *          구성할 때 PRP entry 또는 SGL segment로 전환된다.
 */
struct fs_bio_integrity_buf {
	struct bio_integrity_payload	bip;	/* NVMe 쓰기/읽기 시 PRP/SGL에 탑재될 PI payload */
	struct bio_vec			bvec;	/* 단일 metadata page; 큰 I/O에서는 추가 bio_vec 배열로 확장(추정) */
};

/* kmem_cache/mempool: NOIO 상황에서도 integrity 버퍼를 안정적으로 할당하기
 * 위한 풀. NVMe I/O 완료 콜백 경로에서 해제될 수 있으므로 메모리 재귀를
 * 방지해야 한다.
 */
static struct kmem_cache *fs_bio_integrity_cache;	/* SLAB 단위 metadata descriptor; NVMe PI 활성화 namespace에서 bio당 1회 사용 */
static mempool_t fs_bio_integrity_pool;			/* NOIO 예약 풀; nvme_irq -> bio_endio 완료 경로에서 mempool_free() 호출 시 재귀 방지 */

/**
 * fs_bio_integrity_alloc - bio integrity payload 할당 및 초기화
 * @bio: 대상 bio (NVMe I/O로 변환될 후보)
 *
 * 목적: 파일시스템이 bio를 제출하기 직전, integrity metadata를 위한 메모리를
 *       미리 할당하고 초기화한다.
 *
 * 호출 경로:
 * fs_bio_integrity_generate() -> fs_bio_integrity_alloc() ->
 * bio_integrity_init() / bio_integrity_alloc_buf() / bio_integrity_setup_default()
 * -> 이후 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 * nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *
 * NVMe 연결점:
 *  - action이 0이면 NVMe metadata가 필요 없는 plain I/O로 간주한다.
 *  - BI_ACT_ZERO가 설정되면 NVMe metadata 영역을 0으로 초기화(쓰기 경로).
 *  - BI_ACT_CHECK가 설정되면 기본 integrity 프로파일을 설정하여 이후
 *    NVMe controller의 PI 검증과 대응할 수 있도록 준비한다.
 */
unsigned int fs_bio_integrity_alloc(struct bio *bio)	/* submit_bio 이전, NVMe doorbell 발행 전 metadata 버퍼를 준비하는 단계 */
{
	struct fs_bio_integrity_buf *iib;	/* NVMe metadata를 담을 fs_bio_integrity_buf; 이후 bio->bi_integrity에 연결됨 */
	unsigned int action;			/* bio_integrity_action() 마스크; NVMe command의 metadata/PRACT/Guard 필드 생성에 영향(추정) */

	/* bio의 integrity 동작 마스크를 읽는다. 0이면 NVMe metadata 없이
	 * 진행한다. */
	action = bio_integrity_action(bio);
	if (!action)				/* action == 0이면 NVMe SQ entry의 metadata 관련 필드를 채우지 않고 CID/tag 할당만으로 진행(추정) */
		return 0;

	/* NOIO로 mempool에서 할당: NVMe 완료 interrupt 콜백 낶에서 재호출될
	 * 가능성이 있으므로 일반 할당자 대신 mempool을 사용한다. */
	iib = mempool_alloc(&fs_bio_integrity_pool, GFP_NOIO);
	bio_integrity_init(bio, &iib->bip, &iib->bvec, 1);	/* bio_integrity_payload를 1개 bvec로 초기화; NVMe driver는 이를 SG list로 순회하며 PRP/SGL entry로 변환(추정) */

	/* 쓰기 시 metadata 버퍼를 0으로 채워 NVMe controller의 PI 검증이
	 * 초기화된 상태에서 시작하도록 한다. */
	bio_integrity_alloc_buf(bio, action & BI_ACT_ZERO);
	if (action & BI_ACT_CHECK)		/* BI_ACT_CHECK가 설정되면 NVMe namespace의 PI 타입(Guard/Reference/AppTag)과 매핑할 기본 프로파일 적용(추정) */
		bio_integrity_setup_default(bio);
	return action;				/* 호출자(fs_bio_integrity_generate)가 action 비트를 참조해 이후 NVMe command 조립 시 metadata 포함 여부 결정(추정) */
}

/**
 * fs_bio_integrity_free - bio integrity payload 해제
 * @bio: 해제할 bio
 *
 * 목적: NVMe I/O 완료 후 bio에 붙은 integrity 메모리를 풀로 반환하고 bio를
 *       integrity 관련 상태에서 벗어나게 한다.
 *
 * 호출 경로 (추정):
 * nvme_irq -> nvme_complete_rq -> bio_endio -> ... -> fs_bio_integrity_free()
 *
 * NVMe 연결점:
 *  - NVMe controller가 PRP/SGL을 통해 메타데이터를 처리한 후, 상위 레이어로
 *    완료를 보고하면서 이 함수가 호출되어 버퍼를 재활용한다.
 *  - REQ_INTEGRITY 플래그를 지워 이후 동일 bio가 plain I/O로 재사용되지
 *    않도록 한다.
 */
void fs_bio_integrity_free(struct bio *bio)	/* NVMe CQ 처리 후 bio_endio() 경로에서 호출; 완료 순서상 PRP/SGL DMA unmap 이후여야 함(추정) */
{
	struct bio_integrity_payload *bip = bio_integrity(bio);	/* NVMe 드라이버가 SQ 발행 전 참조했던 동일 bip; DMA 매핑 해제 후 접근 */

	/* metadata 버퍼(PI)를 먼저 해제한다. */
	bio_integrity_free_buf(bip);
	mempool_free(container_of(bip, struct fs_bio_integrity_buf, bip),
			&fs_bio_integrity_pool);	/* mempool으로 반환; NVMe queue depth만큼의 동시 I/O를 재귀 없이 처리 가능 */

	/* bio가 더 이상 integrity payload를 참조하지 않도록 정리한다. */
	bio->bi_integrity = NULL;
	bio->bi_opf &= ~REQ_INTEGRITY;		/* REQ_INTEGRITY 클리어; 이 bio를 재사용할 때 NVMe controller가 plain I/O로 인식하도록 함(추정) */
}

/**
 * fs_bio_integrity_generate - bio에 대한 integrity 값 생성
 * @bio: 대상 bio
 *
 * 목적: 쓰기 경로에서 데이터에 대한 CRC/체크섬(PI)을 생성하여 NVMe metadata
 *       영역에 기록한다.
 *
 * 호출 경로:
 * 파일시스템 쓰기 -> fs_bio_integrity_generate() ->
 * fs_bio_integrity_alloc() -> bio_integrity_generate() ->
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 * nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *
 * NVMe 연결점:
 *  - 생성된 PI는 NVMe controller가 namespace에 기록할 때 LBA 단위 metadata로
 *    전송된다.
 *  - PRP/SGL에 data segment 다음에 metadata segment가 배치되어 SQ entry의
 *    관련 필드에 반영된다.
 */
void fs_bio_integrity_generate(struct bio *bio)	/* NVMe 쓰기 경로; scheduler plug/batch에 들어가기 전 PI를 생성해야 데이터-메타데이터 일관성이 유지됨(추정) */
{
	if (fs_bio_integrity_alloc(bio))	/* action != 0이면 NVMe metadata 버퍼 확보; 실패 시 bio_integrity_generate()가 호출되지 않아 NVMe SQ 발행이 무결성 없이 진행되거나 상위에서 abort(추정) */
		bio_integrity_generate(bio);	/* CRC/Guard/Reference/AppTag 생성; NVMe controller가 namespace에 쓸 metadata 완성 */
}
EXPORT_SYMBOL_GPL(fs_bio_integrity_generate);	/* 파일시스템/FS-DAX 등 상위 레이어가 NVMe PI namespace에 쓰기 전 호출 가능하도록 export */

/**
 * fs_bio_integrity_verify - bio integrity 값 검증
 * @bio:    검증 대상 bio
 * @sector: 원본 섹터 위치 (LBA 변환에 사용)
 * @size:   검증할 데이터 크기
 *
 * 목적: 읽기 완료 후 NVMe controller가 반환한 metadata(PI)와 데이터를 비교하여
 *       무결성을 검증한다.
 *
 * 호출 경로:
 * bio_endio(읽기 완료) -> fs_bio_integrity_verify() -> bio_integrity_verify()
 * -> blk_status_to_errno()
 *
 * NVMe 연결점:
 *  - NVMe CQ(Completion Queue) entry가 도착하여 doorbell/intr가 처리된 후,
 *    상위 레이어가 sector/size를 기억해 두었다가 본 함수를 호출한다.
 *  - bip_iter를 재초기화하여 NVMe가 반환한 metadata의 시작 섹터와 크기를
 *    정확히 매핑한다.
 */
int fs_bio_integrity_verify(struct bio *bio, sector_t sector, unsigned int size)	/* NVMe CQ 완료 후; doorbell/ISR 처리가 끝난 데이터를 상위에서 검증 */
{
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);	/* NVMe namespace의 disk->integrity profile; sector 크기/PI 타입 결정 */
	struct bio_integrity_payload *bip = bio_integrity(bio);	/* NVMe controller가 반환한 metadata가 담긴 payload; DMA sync 이후 CPU가 읽음(추정) */

	/*
	 * Reinitialize bip->bip_iter.
	 *
	 * This is for use in the submitter after the driver is done with the
	 * bio.  Requires the submitter to remember the sector and the size.
	 */
	memset(&bip->bip_iter, 0, sizeof(bip->bip_iter));	/* NVMe 완료로 변경된 iterator를 원본 LBA 기준으로 재설정; 이후 bvec 순회가 metadata 시작점부터 진행 */
	bip->bip_iter.bi_sector = sector;			/* 원본 LBA; NVMe namespace 내 논리 주소와 정렬되어 PI 검증 기준 주소가 됨 */
	bip->bip_iter.bi_size = bio_integrity_bytes(bi, size >> SECTOR_SHIFT);			/* 데이터 섹터 수(size >> 9)에 해당하는 NVMe metadata 바이트 수 계산; PRP/SGL의 metadata 길이와 일치해야 함 */
	return blk_status_to_errno(bio_integrity_verify(bio, &bip->bip_iter));		/* PI 불일치 시 BLK_STS_IOERR 등을 errno로 변환; NVMe status(End-to-end Guard Check 등)와 유사한 상위 오류 보고 */
}

/**
 * fs_bio_integrity_init - 모듈 초기화
 *
 * 목적: integrity 버퍼용 kmem_cache와 mempool을 미리 생성하여 NVMe I/O 경로
 *       에서 메모리 부족을 방지한다.
 *
 * 호출 경로:
 * fs_initcall -> fs_bio_integrity_init() -> kmem_cache_create() /
 * mempool_init_slab_pool()
 *
 * NVMe 연결점:
 *  - 이 풀이 없으면 NVMe metadata를 처리할 bio integrity 버퍼를 할당할 수
 *    없어 PI 활성화 namespace에서 I/O가 실패할 수 있다.
 *  - SLAB_PANIC 플래그로 인해 초기화 실패 시 커널이 패닉한다.
 */
static int __init fs_bio_integrity_init(void)	/* NVMe driver 로드 전에 fs_initcall로 실행; PI namespace I/O를 위한 예약 자원 확보 */
{
	fs_bio_integrity_cache = kmem_cache_create("fs_bio_integrity",
			sizeof(struct fs_bio_integrity_buf), 0,
			SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);	/* HWCACHE_ALIGN은 metadata가 DMA/PRP 정렬에 유리하도록 배치(추정); SLAB_PANIC은 초기화 실패를 커널 패닉으로 처리 */
	if (mempool_init_slab_pool(&fs_bio_integrity_pool, BIO_POOL_SIZE,
			fs_bio_integrity_cache))
		panic("fs_bio_integrity: can't create pool\n");	/* mempool 생성 실패 시 NVMe PI 경로의 메모리 보장이 물러가므로 커널을 멈춤; 이는 NVMe abort/queue drain보다 먼저 처리되는 초기화 오류 */
	return 0;
}
fs_initcall(fs_bio_integrity_init);	/* NVMe core driver보다 먼저 초기화되어 nvme_probe 시점에 풀이 준비되어 있어야 함(추정) */

/* NVMe 관점 핵심 요약 */
/*
 * - 이 파일은 파일시스템-블록계층 간 bio integrity 버퍼를 관리하며, NVMe
 *   namespace의 End-to-End Data Protection(Metadata/PI) 활성화 여부와 직접
 *   연결된다.
 * - fs_bio_integrity_generate()는 쓰기 경로에서 NVMe SQ로 전송될 metadata를
 *   생성하고, fs_bio_integrity_verify()는 NVMe CQ 완료 후 상위 레이어에서
 *   metadata를 검증한다.
 * - 실제 NVMe command 조립(PRP/SGL 배치, doorbell 발행, CID 할당)은 본 파일
 *   이후의 nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로에서 수행된다.
 * - 본 파일은 block/bio-integrity.c, include/linux/bio-integrity.h 등과
 *   함께 bio integrity 프레임워크를 구성하며, NVMe 드라이버는 이를 통해
 *   metadata 버퍼에 접근한다.
 * - (추정) fs_bio_integrity_buf의 단일 bvec 구조는 대부분의 NVMe metadata
 *   크기를 커버하며, 큰 메타데이터를 요구하는 경우 상위 bio_vec 배열이
 *   추가로 할당될 수 있다.
 */
