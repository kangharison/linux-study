// SPDX-License-Identifier: GPL-2.0
/*
 * Functions related to generic helpers functions
 *
 * ===================================================================
 * NVMe SSD 관점 파일 요약 (file-top summary)
 * ===================================================================
 * block/blk-lib.c는 블록 계층(block layer)의 공용 헬퍼 라이브러리로,
 * discard, write zeroes, secure erase 등의 고수준 I/O 변환을 담당한다.
 * 파일 시스템이나 사용자 공간에서 호출된 후,
 * blk-lib.c에서 bio로 변환된 요청은 submit_bio()를 거쳐
 * blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 * nvme_submit_cmd(doorbell) 경로로 NVMe 컨트롤러에 전달된다.
 * 즉, 이 파일은 NVMe 명령어(DSM Deallocate, Write Zeroes,
 * Sanitize/Format 등)로 최종 매핑되기 직전의 블록 계층 처리를 수행한다.
 * ===================================================================
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/scatterlist.h>

#include "blk.h"

/*
 * bio_discard_limit - NVMe Deallocate(Trim) 요청 분할 기준 계산
 *
 * 목적:
 *   discard_granularity(일반적으로 NVMe namespace의 NAWUN/NAWUPF에 대응)에
 *   맞춰 bio를 분할할 수 있도록, 한 번에 처리할 수 있는 최대 섹터 수를
 *   반환한다.
 *
 * 호출 경로 (추정):
 *   blkdev_issue_discard -> __blkdev_issue_discard ->
 *   blk_alloc_discard_bio -> bio_discard_limit
 *
 * NVMe 연결점:
 *   - bdev_discard_granularity는 NVMe namespace의 discard granularity와
 *     연결된다.
 *   - 정렬되지 않은 시작 섹터일 경우 NVMe는 NAWUN 정렬 조건을 요구할 수
 *     있으므로, 다음 bio가 granularity 경계에서 시작하도록 조정한다.
 */
static sector_t bio_discard_limit(struct block_device *bdev, sector_t sector)
{
	unsigned int discard_granularity = bdev_discard_granularity(bdev); /* NVMe namespace granularity(보통 4KB 단위) 읽기 */
	sector_t granularity_aligned_sector;

	if (bdev_is_partition(bdev))
		sector += bdev->bd_start_sect; /* 파티션 offset 적용 (NVMe LBA 변환 시 필요) */

	granularity_aligned_sector =
		round_up(sector, discard_granularity >> SECTOR_SHIFT); /* NAWUN/NAWUPF 경계로 올림 정렬 (추정) */

	/*
	 * Make sure subsequent bios start aligned to the discard granularity if
	 * it needs to be split.
	 */
	if (granularity_aligned_sector != sector)
		return granularity_aligned_sector - sector; /* 비정렬 구간만큼 먼저 분할, 이후 bio는 granularity 경계 시작 */

	/*
	 * Align the bio size to the discard granularity to make splitting the bio
	 * at discard granularity boundaries easier in the driver if needed.
	 */
	return round_down(BIO_MAX_SIZE, discard_granularity) >> SECTOR_SHIFT; /* NVMe DSM Range 정렬을 위해 granularity 배수로 제한 */
}

/*
 * blk_alloc_discard_bio - 단일 discard bio 할당 및 초기화
 *
 * 목적:
 *   주어진 섹터 범위에서 하나의 REQ_OP_DISCARD bio를 생성한다.
 *
 * 호출 경로 (추정):
 *   blkdev_issue_discard -> __blkdev_issue_discard -> blk_alloc_discard_bio
 *
 * NVMe 연결점:
 *   - REQ_OP_DISCARD는 NVMe 드라이버(nvme_queue_rq)에서
 *     DSM(Deallocate) 명령어로 변환될 수 있다.
 *   - bio->bi_iter.bi_sector/bi_size는 이후 NVMe SQ에 기록될
 *     SLBA(Sector/LBA) 및 Length 계산의 기초가 된다.
 */
struct bio *blk_alloc_discard_bio(struct block_device *bdev,
		sector_t *sector, sector_t *nr_sects, gfp_t gfp_mask)
{
	sector_t bio_sects = min(*nr_sects, bio_discard_limit(bdev, *sector)); /* NVMe DSM Range 최대 길이 제한 적용 */
	struct bio *bio;

	if (!bio_sects)
		return NULL; /* 남은 discard 범위 없음, NVMe SQ에 더 이상 기록하지 않음 */

	bio = bio_alloc(bdev, 0, REQ_OP_DISCARD, gfp_mask); /* 데이터 페이지 없이 NVMe DSM Deallocate bio 할당 */
	if (!bio)
		return NULL; /* bio 풀 고갈, NVMe CID/PRP/SGL 배정 이전에 실패 */
	bio->bi_iter.bi_sector = *sector; /* NVMe SLBA 계산의 기초 */
	bio->bi_iter.bi_size = bio_sects << SECTOR_SHIFT; /* NVMe DSM Length(섹터 기준) 계산의 기초 */
	*sector += bio_sects; /* 다음 bio 시작 LBA 갱신, NVMe DSM Range 연속성 유지 */
	*nr_sects -= bio_sects; /* 남은 discard 길이 갱신 */
	/*
	 * We can loop for a long time in here if someone does full device
	 * discards (like mkfs).  Be nice and allow us to schedule out to avoid
	 * softlocking if preempt is disabled.
	 */
	cond_resched(); /* 장시간 bio 분할 루프 중 선점 yield, SQ doorbell 지연 가능 (추정) */
	return bio;
}

/*
 * __blkdev_issue_discard - 사용자/파일시스템의 discard 요청을 bio 체인으로 변환
 *
 * 목적:
 *   [sector, sector + nr_sects) 범위의 discard를 여러 bio로 분할하여
 *   biop에 연결한다.
 *
 * 호출 경로 (추정):
 *   blkdev_issue_discard -> __blkdev_issue_discard ->
 *   bio_chain_and_submit -> submit_bio -> blk_mq_submit_bio -> ... -> nvme_queue_rq
 *
 * NVMe 연결점:
 *   - NVMe DSM 명령은 최대 256개의 Range까지 한 명령에 담을 수 있지만,
 *     블록 계층에서는 우선 bio 단위로 분할하여 전달한다.
 *   - nvme驱动는 이 bio들을 모아 하나 이상의 DSM CID로 SQ에 배치한다.
 */
void __blkdev_issue_discard(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp_mask, struct bio **biop)
{
	struct bio *bio;

	while ((bio = blk_alloc_discard_bio(bdev, &sector, &nr_sects,
			gfp_mask)))
		*biop = bio_chain_and_submit(*biop, bio); /* bio 체인 연결 후 submit_bio로 진입 */
}
EXPORT_SYMBOL(__blkdev_issue_discard);

/**
 * blkdev_issue_discard - queue a discard
 * @bdev:	blockdev to issue discard for
 * @sector:	start sector
 * @nr_sects:	number of sectors to discard
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 *
 * Description:
 *    Issue a discard request for the sectors in question.
 *
 * NVMe 연결점 (추정):
 *   - 이 함수는 파일시스템(fstrim, mount -o discard 등)에서 호출된다.
 *   - blk_start_plug()/blk_finish_plug() 사이에서 bio가 batched 되어
 *     NVMe SQ doorbell 갱신 횟수를 줄이는 데 도움이 된다.
 *   - -EOPNOTSUPP는 NVMe 컨트롤러가 discard를 지원하지 않을 때
 *     드라이버나 블록 계층에서 반환할 수 있다.
 */
int blkdev_issue_discard(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp_mask)
{
	struct bio *bio = NULL;
	struct blk_plug plug;
	int ret = 0;

	blk_start_plug(&plug); /* 플러깅 시작: NVMe SQ doorbell 배칭 최적화 */
	__blkdev_issue_discard(bdev, sector, nr_sects, gfp_mask, &bio);
	if (bio) {
		ret = submit_bio_wait(bio); /* submit_bio -> blk_mq_submit_bio -> ... -> nvme_queue_rq -> nvme_submit_cmd(doorbell) */
		if (ret == -EOPNOTSUPP)
			ret = 0; /* NVMe discard 미지원 시 상위에 무시, 큐 능력 비트 갱신 가능 (추정) */
		bio_put(bio);
	}
	blk_finish_plug(&plug); /* 플러깅 종료 및 미처리 doorbell 한 번에 갱신 */

	return ret;
}
EXPORT_SYMBOL(blkdev_issue_discard);

/*
 * bio_write_zeroes_limit - NVMe Write Zeroes 명령 최대 길이 산출
 *
 * 목적:
 *   NVMe Write Zeroes offload를 사용할 때, 한 bio에 담을 수 있는
 *   최대 섹터 수를 논리 블록 크기로 정렬하여 반환한다.
 *
 * NVMe 연결점:
 *   - bdev_write_zeroes_sectors는 NVMe Identify Namespace의
 *     AWUN(Atomic Write Unit Normal)이나 Write Zeroes 최대 섹터 수와
 *     관련될 수 있다.
 *   - bs_mask는 LBA 어드레스/길이가 NVMe LBAF 포맷의 논리 블록 크기에
 *     맞춰져 있음을 보장한다.
 */
static sector_t bio_write_zeroes_limit(struct block_device *bdev)
{
	sector_t bs_mask = (bdev_logical_block_size(bdev) >> 9) - 1; /* LBAF 논리 블록 크기 -> 섹터 마스크 변환 */

	return min(bdev_write_zeroes_sectors(bdev), BIO_MAX_SECTORS & ~bs_mask); /* NVMe Write Zeroes 최대 섹터 수와 bio 최대 범위 중 작은 값 */
}

/*
 * There is no reliable way for the SCSI subsystem to determine whether a
 * device supports a WRITE SAME operation without actually performing a write
 * to media. As a result, write_zeroes is enabled by default and will be
 * disabled if a zeroing operation subsequently fails. This means that this
 * queue limit is likely to change at runtime.
 */
/*
 * __blkdev_issue_write_zeroes - 하드웨어 offload 기반 zero-fill bio 생성
 *
 * 목적:
 *   REQ_OP_WRITE_ZEROES bio를 limit 크기로 분할 생성한다.
 *
 * 호출 경로 (추정):
 *   blkdev_issue_zeroout -> blkdev_issue_write_zeroes ->
 *   __blkdev_issue_write_zeroes -> bio_chain_and_submit -> submit_bio
 *
 * NVMe 연결점:
 *   - REQ_OP_WRITE_ZEROES는 NVMe Write Zeroes 명령(CID 배정 후 SQ에 기록)으로
 *     변환될 수 있다.
 *   - BLKDEV_ZERO_NOUNMAP 플래그는 NVMe Deallocate bit가 0인 Write Zeroes
 *     명령으로 매핑될 수 있다.
 *   - BLKDEV_ZERO_KILLABLE은 시그널 수신 시 NVMe SQ 진입 전 bio 생성을
 *     중단하여 불필요한 CID 소모를 막는다.
 */
static void __blkdev_issue_write_zeroes(struct block_device *bdev,
		sector_t sector, sector_t nr_sects, gfp_t gfp_mask,
		struct bio **biop, unsigned flags, sector_t limit)
{

	while (nr_sects) { /* NVMe Write Zeroes Range 분할 루프 */
		unsigned int len = min(nr_sects, limit); /* 한 bio/CID당 처리할 최대 섹터 수, AWUN 제약 반영 (추정) */
		struct bio *bio;

		if ((flags & BLKDEV_ZERO_KILLABLE) &&
		    fatal_signal_pending(current))
			break; /* 시그널 수신: SQ doorbell 진입 전 bio 생성 중단, CID/태그 낭비 방지 */

		bio = bio_alloc(bdev, 0, REQ_OP_WRITE_ZEROES, gfp_mask); /* 데이터 페이지 없는 NVMe Write Zeroes bio 할당 */
		bio->bi_iter.bi_sector = sector; /* NVMe SLBA 기초 */
		if (flags & BLKDEV_ZERO_NOUNMAP)
			bio->bi_opf |= REQ_NOUNMAP; /* NVMe Write Zeroes의 Deallocate=0에 대응 (추정) */

		bio->bi_iter.bi_size = len << SECTOR_SHIFT; /* NVMe Write Zeroes Length(섹터) */
		*biop = bio_chain_and_submit(*biop, bio); /* submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) */

		nr_sects -= len; /* 남은 zero-fill 범위 감소 */
		sector += len; /* 다음 NVMe SLBA 갱신 */
		cond_resched(); /* 장시간 루프 yield, doorbell 배칭 지연 가능 (추정) */
	}
}

/*
 * blkdev_issue_write_zeroes - NVMe Write Zeroes offload 발행
 *
 * 목적:
 *   NVMe가 지원하는 경우 하드웨어 Write Zeroes 명령으로 영역을 0으로 채운다.
 *
 * NVMe 연결점:
 *   - 하드웨어 offload가 실패하면 bdev_write_zeroes_sectors가 0으로
 *     갱신되어 이후 NVMe Write Zeroes 대신 평범한 WRITE(Zero Page) 경로로
 *     fallback한다.
 *   - -EOPNOTSUPP는 NVMe 컨트롤러가 Write Zeroes를 지원하지 않음을 의미한다.
 */
static int blkdev_issue_write_zeroes(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp, unsigned flags)
{
	sector_t limit = bio_write_zeroes_limit(bdev); /* NVMe Write Zeroes 한 명령 최대 길이(섹터) */
	struct bio *bio = NULL;
	struct blk_plug plug;
	int ret = 0;

	blk_start_plug(&plug); /* NVMe SQ doorbell batching 시작 */
	__blkdev_issue_write_zeroes(bdev, sector, nr_sects, gfp, &bio,
			flags, limit);
	if (bio) {
		ret = bio_submit_or_kill(bio, flags); /* submit_bio_wait 동등 경로 -> nvme_queue_rq -> nvme_submit_cmd(doorbell) */
		bio_put(bio);
	}
	blk_finish_plug(&plug); /* 배칭된 doorbell 일괄 갱신, SQ tail 진행 (추정) */

	/*
	 * For some devices there is no non-destructive way to verify whether
	 * WRITE ZEROES is actually supported.  These will clear the capability
	 * on an I/O error, in which case we'll turn any error into
	 * "not supported" here.
	 */
	if (ret && !bdev_write_zeroes_sectors(bdev))
		return -EOPNOTSUPP; /* NVMe Write Zeroes 능력 클리어 시 capability 재탐색 유도 */
	return ret;
}

/*
 * Convert a number of 512B sectors to a number of pages.
 * The result is limited to a number of pages that can fit into a BIO.
 * Also make sure that the result is always at least 1 (page) for the cases
 * where nr_sects is lower than the number of sectors in a page.
 */
/*
 * __blkdev_sectors_to_bio_pages - 섹터 수를 bio 벡터 페이지 수로 변환
 *
 * NVMe 연결점:
 *   - NVMe PRP/SGL list는 물리 페이지 단위로 구성되므로, 이 함수는
 *     zero page fallback 시 bio에 들어갈 최대 페이지 수를 제한한다.
 *   - BIO_MAX_VECS 제한은 NVMe SGL/PRP entry 수와 직접 관련이 있다.
 */
static unsigned int __blkdev_sectors_to_bio_pages(sector_t nr_sects)
{
	sector_t pages = DIV_ROUND_UP_SECTOR_T(nr_sects, PAGE_SIZE / 512); /* 512B 섹터 -> 물리 페이지 수 변환, PRP/SGL entry 개수 산정 기초 */

	return min(pages, (sector_t)BIO_MAX_VECS); /* bio 벡터/PRP/SGL entry 상한 적용 */
}

/*
 * __blkdev_issue_zero_pages - zero-filled page를 직접 쓰는 fallback bio 생성
 *
 * 목적:
 *   NVMe Write Zeroes offload를 사용할 수 없을 때, 미리 할당된 zero folio를
 *   데이터 버퍼로 사용하여 REQ_OP_WRITE bio를 생성한다.
 *
 * 호출 경로 (추정):
 *   blkdev_issue_zeroout -> blkdev_issue_zero_pages ->
 *   __blkdev_issue_zero_pages -> bio_chain_and_submit -> submit_bio
 *
 * NVMe 연결점:
 *   - 이 경로는 NVMe Write Zeroes 미지원 시 사용되며, PRP/SGL에 zero page의
 *     물리 주소가 기록된다.
 *   - 데이터 패턴이 모두 0이므로 NVMe NAND 쓰기로 변환된다.
 */
static void __blkdev_issue_zero_pages(struct block_device *bdev,
		sector_t sector, sector_t nr_sects, gfp_t gfp_mask,
		struct bio **biop, unsigned int flags)
{
	struct folio *zero_folio = largest_zero_folio(); /* 모든 0으로 채워진 공유 folio, DMA/PRP 물리 주소 재사용 가능 (추정) */

	while (nr_sects) { /* zero page WRITE bio 분할 루프 */
		unsigned int nr_vecs = __blkdev_sectors_to_bio_pages(nr_sects); /* 한 bio당 PRP/SGL entry 수 상한 */
		struct bio *bio;

		if ((flags & BLKDEV_ZERO_KILLABLE) &&
		    fatal_signal_pending(current))
			break; /* SQ/CID 진입 전 시그널로 중단, abort/requeue 없음 */

		bio = bio_alloc(bdev, nr_vecs, REQ_OP_WRITE, gfp_mask); /* zero page를 data buffer로 하는 NVMe WRITE bio 할당 */
		bio->bi_iter.bi_sector = sector; /* NVMe SLBA 기초 */

		do {
			unsigned int len;

			len = min_t(sector_t, folio_size(zero_folio),
				    nr_sects << SECTOR_SHIFT); /* folio 크기와 남은 섹터 중 작은 값, NVMe LBA 길이 정렬 */
			if (!bio_add_folio(bio, zero_folio, len, 0))
				break; /* bio가 허용하는 최대 PRP/SGL 엔트리에 도달 (추정) */
			nr_sects -= len >> SECTOR_SHIFT; /* 남은 섹터 감소 */
			sector += len >> SECTOR_SHIFT; /* 다음 SLBA 갱신 */
		} while (nr_sects);

		*biop = bio_chain_and_submit(*biop, bio); /* submit_bio -> ... -> nvme_queue_rq -> nvme_submit_cmd(doorbell) */
		cond_resched();
	}
}

/*
 * blkdev_issue_zero_pages - NVMe Write Zeroes offload 없이 zero-fill 수행
 *
 * NVMe 연결점:
 *   - BLKDEV_ZERO_NOFALLBACK 플래그가 설정되면 NVMe Write Zeroes를
 *     지원하지 않을 때 -EOPNOTSUPP를 즉시 반환한다.
 *   - zero page를 사용하므로 실제 데이터 전송 버스 트래픽이 발생한다.
 */
static int blkdev_issue_zero_pages(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp, unsigned flags)
{
	struct bio *bio = NULL;
	struct blk_plug plug;
	int ret = 0;

	if (flags & BLKDEV_ZERO_NOFALLBACK)
		return -EOPNOTSUPP; /* NVMe Write Zeroes offload 강제 실패 시 fallback 금지 */

	blk_start_plug(&plug); /* zero page WRITE bio batching, SQ doorbell 최소화 */
	__blkdev_issue_zero_pages(bdev, sector, nr_sects, gfp, &bio, flags);
	if (bio) {
		ret = bio_submit_or_kill(bio, flags); /* submit_bio_wait 경로 -> nvme_queue_rq -> doorbell */
		bio_put(bio);
	}
	blk_finish_plug(&plug); /* batched doorbell flush, NVMe SQ tail 갱신 (추정) */

	return ret;
}

/**
 * __blkdev_issue_zeroout - generate number of zero filed write bios
 * @bdev:	blockdev to issue
 * @sector:	start sector
 * @nr_sects:	number of sectors to write
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 * @biop:	pointer to anchor bio
 * @flags:	controls detailed behavior
 *
 * Description:
 *  Zero-fill a block range, either using hardware offload or by explicitly
 *  writing zeroes to the device.
 *
 *  If a device is using logical block provisioning, the underlying space will
 *  not be released if %flags contains BLKDEV_ZERO_NOUNMAP.
 *
 *  If %flags contains BLKDEV_ZERO_NOFALLBACK, the function will return
 *  -EOPNOTSUPP if no explicit hardware offload for zeroing is provided.
 *
 * NVMe 연결점 (추정):
 *  - limit != 0이면 NVMe Write Zeroes offload를 사용하고,
 *    limit == 0이면 zero page를 이용한 평범한 WRITE로 fallback한다.
 *  - BLKDEV_ZERO_NOUNMAP은 NVMe Write Zeroes 명령의 Deallocate 비트를 0으로
 *    설정하여 공간 해제를 방지한다.
 */
int __blkdev_issue_zeroout(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp_mask, struct bio **biop,
		unsigned flags)
{
	sector_t limit = bio_write_zeroes_limit(bdev); /* NVMe Write Zeroes 최대 섹터 한도, 0이면 offload 불가 */

	if (bdev_read_only(bdev))
		return -EPERM; /* 읽기 전용 NVMe namespace, SQ 기록 불가 */

	if (limit) {
		__blkdev_issue_write_zeroes(bdev, sector, nr_sects,
				gfp_mask, biop, flags, limit); /* NVMe Write Zeroes 경로: CID/태그/PRP 미사용, SLBA+Length만 기록 */
	} else {
		if (flags & BLKDEV_ZERO_NOFALLBACK)
			return -EOPNOTSUPP; /* offload 불가 + fallback 금지 */
		__blkdev_issue_zero_pages(bdev, sector, nr_sects, gfp_mask,
				biop, flags); /* zero page를 DMA buffer로 하는 NVMe WRITE 경로 */
	}
	return 0;
}
EXPORT_SYMBOL(__blkdev_issue_zeroout);

/**
 * blkdev_issue_zeroout - zero-fill a block range
 * @bdev:	blockdev to write
 * @sector:	start sector
 * @nr_sects:	number of sectors to write
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 * @flags:	controls detailed behavior
 *
 * Description:
 *  Zero-fill a block range, either using hardware offload or by explicitly
 *  writing zeroes to the device.  See __blkdev_issue_zeroout() for the
 *  valid values for %flags.
 *
 * NVMe 연결점 (추정):
 *  - 먼저 NVMe Write Zeroes offload 시도 후, -EOPNOTSUPP면 zero page
 *    fallback 경로로 전환한다.
 *  - 섹터/길이가 bdev_logical_block_size로 정렬되어 있어야 NVMe LBA 단위
 *    조건을 만족한다.
 */
int blkdev_issue_zeroout(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp_mask, unsigned flags)
{
	int ret;

	if ((sector | nr_sects) & ((bdev_logical_block_size(bdev) >> 9) - 1))
		return -EINVAL; /* NVMe LBA 어드레스/길이 정렬 조건 불만족 */
	if (bdev_read_only(bdev))
		return -EPERM; /* NVMe namespace read-only */

	if (bdev_write_zeroes_sectors(bdev)) { /* NVMe Write Zeroes 능력 유무 확인 */
		ret = blkdev_issue_write_zeroes(bdev, sector, nr_sects,
				gfp_mask, flags);
		if (ret != -EOPNOTSUPP)
			return ret; /* NVMe Write Zeroes 성공 또는 복구 불가 오류 */
	}

	return blkdev_issue_zero_pages(bdev, sector, nr_sects, gfp_mask, flags); /* NVMe Write Zeroes 미지원 또는 실패 시 zero page WRITE fallback */
}
EXPORT_SYMBOL(blkdev_issue_zeroout);

/*
 * blkdev_issue_secure_erase - NVMe Secure Erase/Sanitize 대응 bio 발행
 *
 * 목적:
 *   보안 삭제(secure erase) 요청을 REQ_OP_SECURE_ERASE bio로 변환한다.
 *
 * 호출 경로 (추정):
 *   사용자/파일시스템 -> blkdev_issue_secure_erase ->
 *   blk_next_bio -> submit_bio_wait -> blk_mq_submit_bio -> ... ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *
 * NVMe 연결점:
 *   - REQ_OP_SECURE_ERASE는 NVMe Sanitize 또는 Format NVM 등의
 *     보안 삭제 메커니즘으로 매핑될 수 있다.
 *   - bdev_max_secure_erase_sectors는 NVMe 컨트롤러가 한 번에 처리할 수
 *     있는 최대 보안 삭제 섹터 수를 반영한다 (추정).
 *   - max_sectors가 0이면 NVMe 보안 삭제 미지원으로 -EOPNOTSUPP 반환.
 */
int blkdev_issue_secure_erase(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp)
{
	sector_t bs_mask = (bdev_logical_block_size(bdev) >> 9) - 1; /* NVMe LBAF 논리 블록 크기 마스크 */
	unsigned int max_sectors = bdev_max_secure_erase_sectors(bdev); /* NVMe Sanitize/Format 최대 섹터 수 (추정) */
	struct bio *bio = NULL;
	struct blk_plug plug;
	int ret = 0;

	/* make sure that "len << SECTOR_SHIFT" doesn't overflow */
	if (max_sectors > BIO_MAX_SECTORS)
		max_sectors = BIO_MAX_SECTORS; /* bio 최대 범위로 클램핑, NVMe command Dword 전송 한계 반영 (추정) */
	max_sectors &= ~bs_mask; /* 논리 블록 크기로 정렬 (NVMe LBA 단위) */

	if (max_sectors == 0)
		return -EOPNOTSUPP; /* NVMe 보안 삭제 미지원 */
	if ((sector | nr_sects) & bs_mask)
		return -EINVAL; /* NVMe LBA 정렬 조건 불만족 */
	if (bdev_read_only(bdev))
		return -EPERM; /* NVMe namespace read-only, Sanitize/Format 거부 */

	blk_start_plug(&plug); /* NVMe SQ doorbell batching 시작 */
	while (nr_sects) { /* Secure Erase/Sanitize bio 분할 루프 */
		unsigned int len = min_t(sector_t, nr_sects, max_sectors); /* 한 NVMe 보안 삭제 명령당 최대 길이 */

		bio = blk_next_bio(bio, bdev, 0, REQ_OP_SECURE_ERASE, gfp); /* 데이터 페이지 없는 NVMe Sanitize/Format bio */
		bio->bi_iter.bi_sector = sector; /* NVMe SLBA 기초 (Sanitize 범위) */
		bio->bi_iter.bi_size = len << SECTOR_SHIFT; /* NVMe Sanitize/Format Length(섹터) */

		sector += len; /* 다음 SLBA */
		nr_sects -= len; /* 남은 보안 삭제 범위 감소 */
		cond_resched(); /* 장시간 루프 yield */
	}
	if (bio) {
		ret = submit_bio_wait(bio); /* submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) */
		bio_put(bio);
	}
	blk_finish_plug(&plug); /* batched doorbell flush, NVMe SQ tail 갱신 (추정) */

	return ret;
}
EXPORT_SYMBOL(blkdev_issue_secure_erase);

/*
 * ===================================================================
 * NVMe 관점 핵심 요약
 * ===================================================================
 * - block/blk-lib.c는 파일시스템/사용자 공간의 discard, write zeroes,
 *   secure erase 요청을 bio로 변환하여 blk-mq -> nvme_queue_rq 경로로
 *   본격적인 NVMe 명령어 생성에 진입시킨다.
 *
 * - REQ_OP_DISCARD는 NVMe DSM Deallocate, REQ_OP_WRITE_ZEROES는 NVMe
 *   Write Zeroes, REQ_OP_SECURE_ERASE는 NVMe Sanitize/Format NVM 등으로
 *   매핑될 수 있다.
 *
 * - bio_write_zeroes_limit, bio_discard_limit 등은 NVMe namespace의
 *   논리 블록 크기, AWUN, NAWUN 등의 제약을 반영하여 bio 분할 정책을
 *   결정한다.
 *
 * - blk_plug는 여러 bio를 모아 NVMe SQ doorbell 갱신 횟수를 줄이고,
 *   submit_bio_wait는 NVMe CQ 완료(단, 이 파일에서 직접 CQ를 다루지는
 *   않음)까지 대기한 뒤 결과를 반환한다.
 *
 * - 이 파일은 block/bio.c(bio 할당/체인), block/blk-mq.c(태그/큐 관리),
 *   drivers/nvme/host/pci.c 또는 core.c(SQ/CQ/PRP/SGL/doorbell)와
 *   논리적으로 연결된다.
 * ===================================================================
 */
