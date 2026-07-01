// SPDX-License-Identifier: GPL-2.0
/*
 * Functions related to segment and merge handling
 *
 * ============================================================================
 * NVMe SSD 관점 파일 요약
 * ============================================================================
 * 이 파일은 block layer의 bio/request 분할(segmentation)과 병합(merge)을 담당한다.
 * 파일 시스템이나 상위 I/O 경로에서 날라온 bio를 NVMe 컨트롤러가 소화할 수 있는
 * 크기와 형태로 정제하며, NVMe 입장에서는 SQ(Submission Queue) 엔트리 하나에
 * 매핑될 I/O의 크기, PRP/SGL 개수, 물리 연속성, Discard 범위 등을 결정하는
 * 핵심 전처리 단계이다.
 *
 * 대표적 호출 경로:
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   bio_split_to_limits -> bio_split_rw -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *
 * 관련 파일: block/blk-mq.c(실제 요청 할당/제출), block/blk-settings.c(queue_limits
 *           초기화), block/bio.c(bio 할당/분할), drivers/nvme/host/pci.c나
 *           tcp.c에서 nvme_queue_rq가 최종적으로 doorbell을 울림).
 * ============================================================================
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/blk-integrity.h>
#include <linux/part_stat.h>
#include <linux/blk-cgroup.h>

#include <trace/events/block.h>

#include "blk.h"
#include "blk-mq-sched.h"
#include "blk-rq-qos.h"
#include "blk-throttle.h"

/*
 * bio의 첫 번째 bvec를 가져온다. NVMe SGL/PRP 엔트리 시작점을 계산할 때
 * bv_offset와 bv_len이 DMA 주소 및 길이로 변환된다.
 */
static inline void bio_get_first_bvec(struct bio *bio, struct bio_vec *bv)
{
	*bv = mp_bvec_iter_bvec(bio->bi_io_vec, bio->bi_iter);
}

/*
 * bio의 마지막 bvec를 가져온다. 연속된 두 bio를 NVMe PRP 리스트로 연결할 때
 * 마지막 페이지 경계가 물리적으로 인접한지 확인해야 하므로 중요하다.
 */
static inline void bio_get_last_bvec(struct bio *bio, struct bio_vec *bv)
{
	struct bvec_iter iter = bio->bi_iter;
	int idx;

	bio_get_first_bvec(bio, bv);
	if (bv->bv_len == bio->bi_iter.bi_size)
		return;		/* this bio only has a single bvec */

	bio_advance_iter(bio, &iter, iter.bi_size);

	if (!iter.bi_bvec_done)
		idx = iter.bi_idx - 1;
	else	/* in the middle of bvec */
		idx = iter.bi_idx;

	*bv = bio->bi_io_vec[idx];

	/*
	 * iter.bi_bvec_done records actual length of the last bvec
	 * if this bio ends in the middle of one io vector
	 */
	if (iter.bi_bvec_done)
		bv->bv_len = iter.bi_bvec_done;
}

/*
 * 두 bio를 병합했을 때 NVMe SGL/PRP 상의 물리적 불연속(gap)이 발생하는지 검사한다.
 * q->limits.virt_boundary_mask는 NVMe BAR/IOMMU 설정에서 유도된 DMA 경계(추정)를
 * 반영할 수 있으며, 이를 어기면 컨트롤러가 PRP/SGL을 제대로 해석하지 못한다.
 *
 * 호출 경로: blk_attempt_bio_merge -> blk_try_merge -> bio_attempt_*_merge ->
 *           ll_*_merge_fn -> req_gap_*_merge -> bio_will_gap
 */
static inline bool bio_will_gap(struct request_queue *q,
		struct request *prev_rq, struct bio *prev, struct bio *next)
{
	struct bio_vec pb, nb;

	/* bio_has_data: data payload가 없는 bio(예: REQ_OP_FLUSH)는 PRP/SGL이
	 * 없으므로 gap 검사가 불필요하다. queue_virt_boundary(q)는 NVMe DMA가
	 * 요구하는 가상 경계로, 설정되어 있을 때만 gap이 의미를 갖는다(추정). */
	if (!bio_has_data(prev) || !queue_virt_boundary(q))
		return false;

	/*
	 * Don't merge if the 1st bio starts with non-zero offset, otherwise it
	 * is quite difficult to respect the sg gap limit.  We work hard to
	 * merge a huge number of small single bios in case of mkfs.
	 */
	/* prev_rq이 주어지면 front merge 가능성을 보고, 그렇지 않으면 prev
	 * bio 자체를 본다. bv_offset이 virt_boundary_mask와 겹치면 NVMe PRP/SGL
	 * 경계를 넘어가므로 병합하지 않는다(추정). */
	if (prev_rq)
		bio_get_first_bvec(prev_rq->bio, &pb);
	else
		bio_get_first_bvec(prev, &pb);
	if (pb.bv_offset & queue_virt_boundary(q))
		return true;

	/*
	 * We don't need to worry about the situation that the merged segment
	 * ends in unaligned virt boundary:
	 *
	 * - if 'pb' ends aligned, the merged segment ends aligned
	 * - if 'pb' ends unaligned, the next bio must include
	 *   one single bvec of 'nb', otherwise the 'nb' can't
	 *   merge with 'pb'
	 */
	/* prev의 마지막 bvec와 next의 첫 번째 bvec의 물리 주소 연속성을
	 * 확인한다. biovec_phys_mergeable이 false면 두 페이지가 물리적으로
	 * 인접하지 않으므로 NVMe SGL/PRP 엔트리가 하나 더 필요하다(추정). */
	bio_get_last_bvec(prev, &pb);
	bio_get_first_bvec(next, &nb);
	if (biovec_phys_mergeable(q, &pb, &nb))
		return false;
	return __bvec_gap_to_prev(&q->limits, &pb, nb.bv_offset);
}

static inline bool req_gap_back_merge(struct request *req, struct bio *bio)
{
	return bio_will_gap(req->q, req, req->biotail, bio);
}

static inline bool req_gap_front_merge(struct request *req, struct bio *bio)
{
	return bio_will_gap(req->q, NULL, bio, req->bio);
}

/*
 * bio가 담을 수 있는 최대 섹터 수를 논리 블록 크기로 내린다.
 * NVMe Identify Namespace에서 보고한 lba_data_size(lbaf)에 해당하는
 * logical_block_size를 기준으로 하드웨어가 인식할 수 있는 최소 단위로 맞춘다.
 */
/*
 * The maximum size that a bio can fit has to be aligned down to the
 * logical block size, which is the minimum accepted unit by hardware.
 */
static unsigned int bio_allowed_max_sectors(const struct queue_limits *lim)
{
	return round_down(BIO_MAX_SIZE, lim->logical_block_size) >>
			SECTOR_SHIFT;
}

/*
 * bio_submit_split_bioset - bio를 지정한 섹터 경계에서 분할하여 제출한다.
 * @bio: 분할할 원본 bio
 * @split_sectors: 분할 위치(섹터 단위)
 * @bs: 분할된 bio 할당에 사용할 bio_set
 *
 * NVMe 입장에서 이 함수는 하나의 큰 I/O가 컨트롤러의 Max Data Transfer Size
 * (MDTS)나 PRP/SGL 한계를 초과할 때 SQ 엔트리 여러 개로 쪼개는 지점이다.
 * 분할된 앞부분은 호출자가 다시 submit_bio_noacct_nocheck를 통해 큐로 복귀시킨다.
 *
 * 호출 경로: bio_split_rw -> bio_submit_split -> bio_submit_split_bioset
 */
/*
 * bio_submit_split_bioset - Submit a bio, splitting it at a designated sector
 * @bio:		the original bio to be submitted and split
 * @split_sectors:	the sector count at which to split
 * @bs:			the bio set used for allocating the new split bio
 *
 * The original bio is modified to contain the remaining sectors and submitted.
 * The caller is responsible for submitting the returned bio.
 *
 * If succeed, the newly allocated bio representing the initial part will be
 * returned, on failure NULL will be returned and original bio will fail.
 */
struct bio *bio_submit_split_bioset(struct bio *bio, unsigned int split_sectors,
				    struct bio_set *bs)
{
	/* bio_split은 남은 부분만을 갖는 새 bio를 할당하고 원본 bio를 앞쪽
	 * 섹터로 줄인다. GFP_NOIO는 I/O 경로에서 메모리 회수를 유발하지 않도록
	 * 하는 플래그이며, 실패 시 NVMe SQ로 날라가지 못하고 bio_endio로 종료된다. */
	struct bio *split = bio_split(bio, split_sectors, GFP_NOIO, bs);

	if (IS_ERR(split)) {
		/* 메모리 부족 등으로 bio 분할 실패. NVMe 입장에서는 이 I/O가
		 * SQ에 도달하지 못하므로 상위에 즉시 에러를 반환해야 한다. */
		bio->bi_status = errno_to_blk_status(PTR_ERR(split));
		bio_endio(bio);
		return NULL;
	}

	/* bio_chain: split(앞부분) 완료 후 bio(남은 부분)를 자동 제출하도록
	 * 연결한다. 이후 submit_bio_noacct_nocheck(bio)가 blk-mq로 다시 진입해
	 * 추가 분할/병합을 거친다 -> nvme_queue_rq -> doorbell. */
	bio_chain(split, bio);
	trace_block_split(split, bio->bi_iter.bi_sector);
	WARN_ON_ONCE(bio_zone_write_plugging(bio));

	if (should_fail_bio(bio))
		bio_io_error(bio);
	else if (!blk_throtl_bio(bio))
		submit_bio_noacct_nocheck(bio, true);

	return split;
}
EXPORT_SYMBOL_GPL(bio_submit_split_bioset);

static struct bio *bio_submit_split(struct bio *bio, int split_sectors)
{
	if (unlikely(split_sectors < 0)) {
		bio->bi_status = errno_to_blk_status(split_sectors);
		bio_endio(bio);
		return NULL;
	}

	/* split_sectors가 0이면 분할 불필요. 0보다 크면 bio_split으로 자르고
	 * 남은 bio에 REQ_NOMERGE를 설정해 재귀 분할을 막는다. 이는 NVMe SQ
	 * 엔트리당 크기가 일단 고정되었음을 의미한다. */
	if (split_sectors) {
		bio = bio_submit_split_bioset(bio, split_sectors,
				&bio->bi_bdev->bd_disk->bio_split);
		if (bio)
			bio->bi_opf |= REQ_NOMERGE;
	}

	return bio;
}

/*
 * __bio_split_discard - Discard/Secure Erase용 bio를 NVMe Deallocate 범위에 맞춰 분할한다.
 * @nsegs: 분할 후 Discard 세그먼트 수를 반환(여러 범위가 하나의 Dataset Management
 *         명령으로 모이는 경우를 대비).
 *
 * queue_limits.discard_granularity와 discard_alignment는 NVMe Identify Namespace의
 * NANW(Namespace Atomic Write Unit), NAWUPF, NABO/NABSPF 등에서 유래한다(추정).
 */
static struct bio *__bio_split_discard(struct bio *bio,
		const struct queue_limits *lim, unsigned *nsegs,
		unsigned int max_sectors)
{
	unsigned int max_discard_sectors, granularity;
	sector_t tmp;
	unsigned split_sectors;

	/* Discard bio는 데이터 버퍼가 없으므로 세그먼트는 1개로 시작. NVMe
	 * Dataset Management는 하나의 range 또는 여러 range 리스트를 담는다. */
	*nsegs = 1;

	/* discard_granularity >> 9는 바이트->섹터 변환. NANW/NAWUPF 단위를
	 * 벗어나지 않도록 정렬한다(추정). */
	granularity = max(lim->discard_granularity >> 9, 1U);

	max_discard_sectors = min(max_sectors, bio_allowed_max_sectors(lim));
	max_discard_sectors -= max_discard_sectors % granularity;
	if (unlikely(!max_discard_sectors))
		return bio;

	/* bio 전체가 한 번의 NVMe DSM 명령으로 커버되면 분할 불필요. */
	if (bio_sectors(bio) <= max_discard_sectors)
		return bio;

	split_sectors = max_discard_sectors;

	/*
	 * If the next starting sector would be misaligned, stop the discard at
	 * the previous aligned sector.
	 */
	/* discard_alignment는 NABO/NABSPF에서 유래한 오프셋이다(추정). 다음
	 * 시작 섹터가 granularity 단위로 정렬되지 않으면 이전 aligned 경계에서
	 * 자른다. 그렇지 않으면 NVMe 컨트롤러가 Deallocate를 거부할 수 있다. */
	tmp = bio->bi_iter.bi_sector + split_sectors -
		((lim->discard_alignment >> 9) % granularity);
	tmp = sector_div(tmp, granularity);

	if (split_sectors > tmp)
		split_sectors -= tmp;

	return bio_submit_split(bio, split_sectors);
}

/*
 * bio_split_discard - NVMe Dataset Management(DSM)에 대응하는 Discard bio 분할.
 * REQ_OP_SECURE_ERASE일 때는 max_secure_erase_sectors를, 아니면 max_discard_sectors를
 * 사용한다. NVMe 컨트롤러별로 Deallocate/Format/Sanitize 동작 한계가 다르다.
 */
struct bio *bio_split_discard(struct bio *bio, const struct queue_limits *lim,
		unsigned *nsegs)
{
	unsigned int max_sectors;

	/* REQ_OP_SECURE_ERASE는 NVMe Sanitize/Format 경로에 대응할 수 있고,
	 * 일반 discard는 Dataset Management(Deallocate)에 대응한다. 각각의
	 * max_*_sectors는 컨트롤러 한계에서 비롯된다. */
	if (bio_op(bio) == REQ_OP_SECURE_ERASE)
		max_sectors = lim->max_secure_erase_sectors;
	else
		max_sectors = lim->max_discard_sectors;

	return __bio_split_discard(bio, lim, nsegs, max_sectors);
}

/*
 * atomic_write_boundary_sectors는 NVMe FUA/Atomic Write 확장에 대응할 수 있다(추정).
 * chunk_sectors는 namespace의 단위 청크 크기로, ZNS SSD라면 zone 크기와도 연관된다.
 */
static inline unsigned int blk_boundary_sectors(const struct queue_limits *lim,
						bool is_atomic)
{
	/*
	 * chunk_sectors must be a multiple of atomic_write_boundary_sectors if
	 * both non-zero.
	 */
	/* atomic_write_boundary_sectors가 설정되면 NVMe FUA/atomic write 단위
	 * 경계를 먼저 따른다. ZNS SSD라면 chunk_sectors가 zone 크기에 해당할
	 * 수 있다(추정). */
	if (is_atomic && lim->atomic_write_boundary_sectors)
		return lim->atomic_write_boundary_sectors;

	return lim->chunk_sectors;
}

/*
 * get_max_io_size - bio의 시작부터 한 번에 제출할 수 있는 최대 섹터 수를 반환한다.
 * @lim: request_queue의 queue_limits; max_sectors는 NVMe Identify Controller의
 *       MDTS를 blk layer가 변환한 값이며, physical_block_size는 format된 LBAF의
 *       하위 물리 정렬 단위를 의미한다(추정).
 *
 * bio 시작이 physical block 경계에 맞지 않으면 끝을 PBS로 먼저 정렬하고, 그렇지
 * 않으면 LBS로 정렬하여 NVMe PRP/SGL 준비 시 불필요한 misaligned I/O를 줄인다.
 */
/*
 * Return the maximum number of sectors from the start of a bio that may be
 * submitted as a single request to a block device. If enough sectors remain,
 * align the end to the physical block size. Otherwise align the end to the
 * logical block size. This approach minimizes the number of non-aligned
 * requests that are submitted to a block device if the start of a bio is not
 * aligned to a physical block boundary.
 */
static inline unsigned get_max_io_size(struct bio *bio,
				       const struct queue_limits *lim)
{
	unsigned pbs = lim->physical_block_size >> SECTOR_SHIFT;
	unsigned lbs = lim->logical_block_size >> SECTOR_SHIFT;
	bool is_atomic = bio->bi_opf & REQ_ATOMIC;
	unsigned boundary_sectors = blk_boundary_sectors(lim, is_atomic);
	unsigned max_sectors, start, end;

	/* max_sectors 선택: WRITE ZEROES는 NZW(Max Write Zeroes Sectors),
	 * atomic write는 atomic_write_max_sectors, 일반 I/O는 max_sectors
	 * (NVMe MDTS에서 변환된 값, 추정)를 따른다. */
	/*
	 * We ignore lim->max_sectors for atomic writes because it may less
	 * than the actual bio size, which we cannot tolerate.
	 */
	if (bio_op(bio) == REQ_OP_WRITE_ZEROES)
		max_sectors = lim->max_write_zeroes_sectors;
	else if (is_atomic)
		max_sectors = lim->atomic_write_max_sectors;
	else
		max_sectors = lim->max_sectors;

	/* boundary_sectors가 있으면(예: ZNS zone/atomic 경계) 해당 경계까지의
	 * 남은 섹터 수와 max_sectors 중 작은 값을 취한다. */
	if (boundary_sectors) {
		max_sectors = min(max_sectors,
			blk_boundary_sectors_left(bio->bi_iter.bi_sector,
					      boundary_sectors));
	}

	/* pbs 정렬: bio 시작이 physical block 경계에서 얼마나 떨어져 있는지
	 * 계산하고, 끝을 다음 physical block 경계로 맞춘다. 이는 NVMe PRP/SGL
	 * 준비 시 메모리 정렬을 돕는다(추정). */
	start = bio->bi_iter.bi_sector & (pbs - 1);
	end = (start + max_sectors) & ~(pbs - 1);
	if (end > start)
		return end - start;
	return max_sectors & ~(lbs - 1);
}

/*
 * bvec_split_segs - 한 bvec이 중간에 쪼개져야 하는지 검사한다.
 * @lim: queue_limits; max_segment_size는 NVMe PRP/SGL 항목 하나가 표현할 수 있는
 *       최대 연속 물리 메모리 길이와 관련된다(추정).
 * @nsegs: [in,out] 현재 bio의 물리 세그먼트 수. NVMe SGL/PRP 엔트리 개수와 직결된다.
 * @bytes: [in,out] 누적 바이트. Max Data Transfer Size 초과 여부 판단에 사용.
 * @max_segs: NVMe Identify Controller의 Maximum PRP Entry/Maximum SGL Descriptor
 *            등에서 비롯된 한계(추정).
 *
 * bvec 하나가 max_segment_size보다 크면 NVMe SGL에서는 하나의 디스크립터로
 * 표현할 수 없으므로 여러 세그먼트로 분할해야 한다.
 */
/**
 * bvec_split_segs - verify whether or not a bvec should be split in the middle
 * @lim:      [in] queue limits to split based on
 * @bv:       [in] bvec to examine
 * @nsegs:    [in,out] Number of segments in the bio being built. Incremented
 *            by the number of segments from @bv that may be appended to that
 *            bio without exceeding @max_segs
 * @bytes:    [in,out] Number of bytes in the bio being built. Incremented
 *            by the number of bytes from @bv that may be appended to that
 *            bio without exceeding @max_bytes
 * @max_segs: [in] upper bound for *@nsegs
 * @max_bytes: [in] upper bound for *@bytes
 *
 * When splitting a bio, it can happen that a bvec is encountered that is too
 * big to fit in a single segment and hence that it has to be split in the
 * middle. This function verifies whether or not that should happen. The value
 * %true is returned if and only if appending the entire @bv to a bio with
 * *@nsegs segments and *@sectors sectors would make that bio unacceptable for
 * the block driver.
 */
static bool bvec_split_segs(const struct queue_limits *lim,
		const struct bio_vec *bv, unsigned *nsegs, unsigned *bytes,
		unsigned max_segs, unsigned max_bytes)
{
	unsigned max_len = max_bytes - *bytes;
	unsigned len = min(bv->bv_len, max_len);
	unsigned total_len = 0;
	unsigned seg_size = 0;

	/* 한 bvec을 max_segment_size 단위로 쪼개면서 nsegs를 증가시킨다.
	 * 이 루프에서 생성된 세그먼트 수는 nvme_setup_cmd -> dma mapping 시
	 * 필요한 PRP/SGL 엔트리 수의 상한이 된다(추정). */
	while (len && *nsegs < max_segs) {
		seg_size = get_max_segment_size(lim, bvec_phys(bv) + total_len, len);

		(*nsegs)++;
		total_len += seg_size;
		len -= seg_size;

		/* virt_boundary_mask 경계에 도달하면 추가 병합을 중단. 이 지점
		 * 이후는 별도의 NVMe DMA 디스크립터가 필요하다(추정). */
		if ((bv->bv_offset + total_len) & lim->virt_boundary_mask)
			break;
	}

	*bytes += total_len;

	/* tell the caller to split the bvec if it is too big to fit */
	/* 분할이 필요한 경우: 아직 처리하지 못한 len이 남았거나, 원래 bvec이
	 * max_bytes보다 커서 bio를 둘로 나눠야 한다. */
	return len > 0 || bv->bv_len > max_len;
}

static unsigned int bio_split_alignment(struct bio *bio,
		const struct queue_limits *lim)
{
	/* ZNS sequential write zone이면 zone write granularity에 맞춰
	 * 분할하고, 아니면 일반 logical_block_size 단위로 정렬한다. */
	if (op_is_write(bio_op(bio)) && lim->zone_write_granularity)
		return lim->zone_write_granularity;
	return lim->logical_block_size;
}

static inline unsigned int bvec_seg_gap(struct bio_vec *bvprv,
					struct bio_vec *bv)
{
	return bv->bv_offset | (bvprv->bv_offset + bvprv->bv_len);
}

/*
 * bio_split_io_at - bio가 queue_limits를 초과하면 어디서 분할할지 결정한다.
 * @bio: 분할 대상 bio
 * @lim: queue_limits (max_segments, max_sectors, virt_boundary_mask 등)
 * @segs: [out] 분할 후 앞쪽 bio의 세그먼트 수 = NVMe PRP/SGL 엔트리 수
 * @max_bytes: bio당 최대 바이트(일반적으로 max_sectors << SECTOR_SHIFT)
 * @len_align_mask: 벡터 길이 정렬 마스크
 *
 * 이 함수는 NVMe I/O를 SQ 엔트리 하나에 실을 수 있는 형태로 자르는 핵심 지점이다.
 * max_segments를 초과하면 PRP/SGL capacity를 넘게 되므로 강제 분할하고,
 * virt_boundary_mask 위반 시에는 메모리 상 discontinuity가 생겨 NVMe DMA가
 * 올바르지 않을 수 있으므로 분할한다(추정).
 *
 * 호출 경로: bio_split_rw -> bio_split_rw_at -> bio_split_io_at
 */
/**
 * bio_split_io_at - check if and where to split a bio
 * @bio:  [in] bio to be split
 * @lim:  [in] queue limits to split based on
 * @segs: [out] number of segments in the bio with the first half of the sectors
 * @max_bytes: [in] maximum number of bytes per bio
 * @len_align_mask: [in] length alignment mask for each vector
 *
 * Find out if @bio needs to be split to fit the queue limits in @lim and a
 * maximum size of @max_bytes.  Returns a negative error number if @bio can't be
 * split, 0 if the bio doesn't have to be split, or a positive sector offset if
 * @bio needs to be split.
 */
int bio_split_io_at(struct bio *bio, const struct queue_limits *lim,
		unsigned *segs, unsigned max_bytes, unsigned len_align_mask)
{
	struct bio_crypt_ctx *bc = bio_crypt_ctx(bio);
	struct bio_vec bv, bvprv, *bvprvp = NULL;
	unsigned nsegs = 0, bytes = 0, gaps = 0;
	struct bvec_iter iter;
	unsigned start_align_mask = lim->dma_alignment;

	/* inline encryption(예: NVMe TC OPAL/SED)가 활성화되면 data_unit_size
	 * 단위로 정렬을 추가 검사한다. crypto context가 다를 경우 NVMe 컨트롤러
	 * 내 keyslot/encryption engine에서 오동작할 수 있다(추정). */
	if (bc) {
		start_align_mask |= (bc->bc_key->crypto_cfg.data_unit_size - 1);
		len_align_mask |= (bc->bc_key->crypto_cfg.data_unit_size - 1);
	}

	/* bio를 구성하는 모든 bvec을 순회하며 세그먼트/바이트 한계를 검사.
	 * 이 루프의 결과가 NVMe SGL/PRP 엔트리 수와 1:1로 연결된다. */
	bio_for_each_bvec(bv, bio, iter) {
		if (bv.bv_offset & start_align_mask ||
		    bv.bv_len & len_align_mask)
			return -EINVAL;

		/*
		 * If the queue doesn't support SG gaps and adding this
		 * offset would create a gap, disallow it.
		 */
		/* 이전 bvec와 현재 bvec 사이의 물리적 gap을 검사. gap이 있으면
		 * NVMe PRP/SGL에서 별도 엔트리가 필요해지므로 분할 지점이 된다. */
		if (bvprvp) {
			if (bvec_gap_to_prev(lim, bvprvp, bv.bv_offset))
				goto split;
			gaps |= bvec_seg_gap(bvprvp, &bv);
		}

		/* max_segments, max_bytes(max_sectors 변환), max_fast_segment_size
		 * 조건을 모두 만족하면 bvec 전체를 현재 bio에 흡수. 하나라도 초과하면
		 * bvec_split_segs로 추가 분할 가능성을 확인한다. */
		if (nsegs < lim->max_segments &&
		    bytes + bv.bv_len <= max_bytes &&
		    bv.bv_offset + bv.bv_len <= lim->max_fast_segment_size) {
			nsegs++;
			bytes += bv.bv_len;
		} else {
			if (bvec_split_segs(lim, &bv, &nsegs, &bytes,
					lim->max_segments, max_bytes))
				goto split;
		}

		bvprv = bv;
		bvprvp = &bvprv;
	}

	*segs = nsegs;
	bio->bi_bvec_gap_bit = ffs(gaps);
	return 0;
split:
	/* atomic write bio는 분할 불가. NVMe atomic write 경계를 어기면
	 * atomicity 보장이 깨지므로 에러로 처리한다. */
	if (bio->bi_opf & REQ_ATOMIC)
		return -EINVAL;

	/*
	 * We can't sanely support splitting for a REQ_NOWAIT bio. End it
	 * with EAGAIN if splitting is required and return an error pointer.
	 */
	if (bio->bi_opf & REQ_NOWAIT)
		return -EAGAIN;

	*segs = nsegs;

	/*
	 * Individual bvecs might not be logical block aligned. Round down the
	 * split size so that each bio is properly block size aligned, even if
	 * we do not use the full hardware limits.
	 *
	 * It is possible to submit a bio that can't be split into a valid io:
	 * there may either be too many discontiguous vectors for the max
	 * segments limit, or contain virtual boundary gaps without having a
	 * valid block sized split. A zero byte result means one of those
	 * conditions occured.
	 */
	bytes = ALIGN_DOWN(bytes, bio_split_alignment(bio, lim));
	if (!bytes)
		return -EINVAL;

	/*
	 * Bio splitting may cause subtle trouble such as hang when doing sync
	 * iopoll in direct IO routine. Given performance gain of iopoll for
	 * big IO can be trival, disable iopoll when split needed.
	 */
	bio_clear_polled(bio);
	bio->bi_bvec_gap_bit = ffs(gaps);
	return bytes >> SECTOR_SHIFT;
}
EXPORT_SYMBOL_GPL(bio_split_io_at);

/*
 * bio_split_rw - 읽기/쓰기 bio를 NVMe PRP/SGL 한계에 맞춰 분할한다.
 * get_max_io_size는 NVMe Max Data Transfer Size(MDTS)와 물리 정렬을 동시에
 * 고려한 값이며, bio_split_rw_at은 그 값을 바이트로 변환해 bio_split_io_at에
 * 전달한다.
 */
struct bio *bio_split_rw(struct bio *bio, const struct queue_limits *lim,
		unsigned *nr_segs)
{
	/* get_max_io_size << SECTOR_SHIFT는 NVMe MDTS 및 물리 정렬을 반영한
	 * 최대 바이트. bio_split_rw_at이 이 값을 넘어서는 지점을 섹터 단위로
	 * 반환하면 bio_submit_split이 분할한다. */
	return bio_submit_split(bio,
		bio_split_rw_at(bio, lim, nr_segs,
			get_max_io_size(bio, lim) << SECTOR_SHIFT));
}

/*
 * bio_split_zone_append - ZNS SSD zone append용 bio는 block layer에서 분할되어선
 * 안 된다. NVMe Zone Append 명령은 쓰기 위치를 zone write pointer가 결정하므로
 * 분할 시 쓰기 순서와 위치가 달라진다. 다만 세그먼트 수 계산은 그대로 수행해
 * submitter가 올바른 bio를 만들었는지 검증한다.
 */
/*
 * REQ_OP_ZONE_APPEND bios must never be split by the block layer.
 *
 * But we want the nr_segs calculation provided by bio_split_rw_at, and having
 * a good sanity check that the submitter built the bio correctly is nice to
 * have as well.
 */
struct bio *bio_split_zone_append(struct bio *bio,
		const struct queue_limits *lim, unsigned *nr_segs)
{
	int split_sectors;

	/* max_zone_append_sectors는 NVMe Zone Append 명령이 한 번에 처리할 수
	 * 있는 최대 섹터 수. 분할이 필요하면 경고로 잡아내는데, ZNS의 경우
	 * block layer에서 Zone Append bio를 분할해서는 안 되기 때문이다. */
	split_sectors = bio_split_rw_at(bio, lim, nr_segs,
			lim->max_zone_append_sectors << SECTOR_SHIFT);
	if (WARN_ON_ONCE(split_sectors > 0))
		split_sectors = -EINVAL;
	return bio_submit_split(bio, split_sectors);
}

/*
 * bio_split_write_zeroes - Write Zeroes 명령을 NVMe Write Zeroes 한계에 맞춰 분할.
 * NVMe Write Zeroes 명령은 Max Write Zeroes Sectors(NZW)를 초과할 수 없으므로
 * 그 값을 기준으로 bio를 자른다.
 */
struct bio *bio_split_write_zeroes(struct bio *bio,
		const struct queue_limits *lim, unsigned *nsegs)
{
	unsigned int max_sectors = get_max_io_size(bio, lim);

	/* Write Zeroes는 데이터 버퍼가 없으므로 세그먼트 수는 0으로 시작. */
	*nsegs = 0;

	/*
	 * An unset limit should normally not happen, as bio submission is keyed
	 * off having a non-zero limit.  But SCSI can clear the limit in the
	 * I/O completion handler, and we can race and see this.  Splitting to a
	 * zero limit obviously doesn't make sense, so band-aid it here.
	 */
	/* max_sectors가 0이면 NVMe Write Zeroes를 허용하지 않는 하위 장치로
	 * 보고 bio를 그대로 반환. */
	if (!max_sectors)
		return bio;
	if (bio_sectors(bio) <= max_sectors)
		return bio;
	return bio_submit_split(bio, max_sectors);
}

/*
 * bio_split_to_limits - bio를 해당 큐의 queue_limits에 맞춰 분할한다.
 * @bio: 분할 대상 bio
 *
 * NVMe 호스트 드라이버가 요청을 할당하기 전에 호출되어, 컨트롤러가 받아들일 수
 * 있는 섹터/세그먼트/정렬 조건을 만족하도록 bio를 정제한다. 분할된 앞부분은
 * q->bio_split에서 할당되며 submit_bio_noacct_nocheck로 다시 큐에 진입한다.
 *
 * 호출 경로: submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *           bio_split_to_limits -> bio_split_rw
 */
/**
 * bio_split_to_limits - split a bio to fit the queue limits
 * @bio:     bio to be split
 *
 * Check if @bio needs splitting based on the queue limits of @bio->bi_bdev, and
 * if so split off a bio fitting the limits from the beginning of @bio and
 * return it.  @bio is shortened to the remainder and re-submitted.
 *
 * The split bio is allocated from @q->bio_split, which is provided by the
 * block layer.
 */
struct bio *bio_split_to_limits(struct bio *bio)
{
	unsigned int nr_segs;

	return __bio_split_to_limits(bio, bdev_limits(bio->bi_bdev), &nr_segs);
}
EXPORT_SYMBOL(bio_split_to_limits);

/*
 * blk_recalc_rq_segments - request에 포함된 bio들로부터 물리 세그먼트 수를 재계산.
 * 결과인 nr_phys_segments는 NVMe SGL/PRP 리스트를 구성할 때 필요한 엔트리 개수의
 * 상한이 된다. Discard/Secure Erase/Write Zeroes는 데이터 버퍼가 없으므로 별도
 * 계산한다.
 */
unsigned int blk_recalc_rq_segments(struct request *rq)
{
	unsigned int nr_phys_segs = 0;
	unsigned int bytes = 0;
	struct req_iterator iter;
	struct bio_vec bv;

	if (!rq->bio)
		return 0;

	/* rq->bio의 op별로 세그먼트 계산 방식이 다르다. 데이터 버퍼가 없는
	 * Discard/Write Zeroes는 별도 처리. */
	switch (bio_op(rq->bio)) {
	case REQ_OP_DISCARD:
	case REQ_OP_SECURE_ERASE:
		/* NVMe DSM은 여러 range를 하나의 명령에 담을 수 있다.
		 * queue_max_discard_segments가 1이면 range 1개로 계산. */
		if (queue_max_discard_segments(rq->q) > 1) {
			struct bio *bio = rq->bio;

			for_each_bio(bio)
				nr_phys_segs++;
			return nr_phys_segs;
		}
		return 1;
	case REQ_OP_WRITE_ZEROES:
		/* NVMe Write Zeroes는 데이터 버퍼/PRP/SGL이 필요 없다. */
		return 0;
	default:
		break;
	}

	/* 일반 read/write: request의 모든 bvec를 순회하며 세그먼트 수를
	 * 누적. 이 값이 nvme_map_data에서 dma map할 SGL/PRP 엔트리 수의
	 * 기초가 된다(추정). */
	rq_for_each_bvec(bv, rq, iter)
		bvec_split_segs(&rq->q->limits, &bv, &nr_phys_segs, &bytes,
				UINT_MAX, BIO_MAX_SIZE);
	return nr_phys_segs;
}

/*
 * blk_rq_get_max_sectors - request가 추가로 확장할 수 있는 최대 섹터 수.
 * chunk_sectors/atomic_write_boundary_sectors는 NVMe namespace의 단위 경계를
 * 넘지 않도록 I/O를 제한하여, ZNS/atomic write 단위 경계를 어기지 않게 한다(추정).
 */
static inline unsigned int blk_rq_get_max_sectors(struct request *rq,
						  sector_t offset)
{
	struct request_queue *q = rq->q;
	struct queue_limits *lim = &q->limits;
	unsigned int max_sectors, boundary_sectors;
	bool is_atomic = rq->cmd_flags & REQ_ATOMIC;

	/* passthrough request(예: SG_IO, nvme-cli admin 명령)는 일반 I/O
	 * 제한 대신 max_hw_sectors를 따른다. */
	if (blk_rq_is_passthrough(rq))
		return q->limits.max_hw_sectors;

	boundary_sectors = blk_boundary_sectors(lim, is_atomic);
	max_sectors = blk_queue_get_max_sectors(rq);

	/* Discard/Secure Erase는 boundary(예: ZNS zone) 제약을 받지 않는
	 * 경로가 있으므로 일반 max_sectors를 그대로 사용. */
	if (!boundary_sectors ||
	    req_op(rq) == REQ_OP_DISCARD ||
	    req_op(rq) == REQ_OP_SECURE_ERASE)
		return max_sectors;
	return min(max_sectors,
		   blk_boundary_sectors_left(offset, boundary_sectors));
}

/*
 * ll_new_hw_segment - 새 bio를 request에 붙이면서 세그먼트 수를 갱신한다.
 * req->nr_phys_segments가 NVMe SGL/PRP 최대 엔트리 수를 초과하면 병합을 거부하고
 * REQ_NOMERGE를 설정해 이후 추가 병합을 막는다. integrity segment는 NVMe PI
 * (Protection Information) 메타데이터를 위한 추가 SGL/PRP에 해당한다(추정).
 */
static inline int ll_new_hw_segment(struct request *req, struct bio *bio,
		unsigned int nr_phys_segs)
{
	/* blkcg가 다를 경우 NVMe 명령 내 io cgroup accounting이 달라져
	 * throttling/weight 보장이 깨지므로 병합하지 않는다(추정). */
	if (!blk_cgroup_mergeable(req, bio))
		goto no_merge;

	/* Protection Information(NVMe end-to-end data protection) 설정이
	 * 일치하지 않으면 병합 불가. */
	if (blk_integrity_merge_bio(req->q, req, bio) == false)
		goto no_merge;

	/* discard request merge won't add new segment */
	if (req_op(req) == REQ_OP_DISCARD)
		return 1;

	/* 병합 후 세그먼트 수가 NVMe max_segments를 초과하면 SGL/PRP capacity
	 * 초과이므로 병합을 거부하고 REQ_NOMERGE를 설정한다. */
	if (req->nr_phys_segments + nr_phys_segs > blk_rq_get_max_segments(req))
		goto no_merge;

	/*
	 * This will form the start of a new hw segment.  Bump both
	 * counters.
	 */
	req->nr_phys_segments += nr_phys_segs;
	if (bio_integrity(bio))
		req->nr_integrity_segments += blk_rq_count_integrity_sg(req->q,
									bio);
	return 1;

no_merge:
	req_set_nomerge(req->q, req);
	return 0;
}

/*
 * ll_back_merge_fn - request 뒤에 bio를 병합할 수 있는지 검사한다.
 * NVMe 관점에서 back merge는 SQ 엔트리 하나에 더 많은 논리 블록을 담아
 * doorbell 횟수와 명령 오버헤드를 줄이는 효과가 있다. 다만 물리적 불연속,
 * PI/암호화 정책, max_sectors 한계를 넘으면 병합하지 않는다.
 */
int ll_back_merge_fn(struct request *req, struct bio *bio, unsigned int nr_segs)
{
	/* 뒤쪽 병합 시 prev bio와 next bio 사이의 물리적 gap을 먼저 검사. */
	if (req_gap_back_merge(req, bio))
		return 0;
	if (blk_integrity_rq(req) &&
	    integrity_req_gap_back_merge(req, bio))
		return 0;
	if (!bio_crypt_ctx_back_mergeable(req, bio))
		return 0;
	/* 병합 후 섹터 수가 NVMe max_sectors(MDTS 변환)를 초과하면 더 이상
	 * 병합하지 않고 REQ_NOMERGE로 마킹한다. */
	if (blk_rq_sectors(req) + bio_sectors(bio) >
	    blk_rq_get_max_sectors(req, blk_rq_pos(req))) {
		req_set_nomerge(req->q, req);
		return 0;
	}

	return ll_new_hw_segment(req, bio, nr_segs);
}

/*
 * ll_front_merge_fn - request 앞에 bio를 병합할 수 있는지 검사한다.
 * front merge는 상대적으로 드물지만, submit_bio 경로에서 out-of-order한 bio가
 * 들어올 때 NVMe 명령을 합치는 데 사용된다.
 */
static int ll_front_merge_fn(struct request *req, struct bio *bio,
		unsigned int nr_segs)
{
	/* 앞쪽 병합 시 bio(앞)와 req->bio(뒤)의 LBA 연속성 및 물리적 gap
	 * 조건을 검사한다. */
	if (req_gap_front_merge(req, bio))
		return 0;
	if (blk_integrity_rq(req) &&
	    integrity_req_gap_front_merge(req, bio))
		return 0;
	if (!bio_crypt_ctx_front_mergeable(req, bio))
		return 0;
	if (blk_rq_sectors(req) + bio_sectors(bio) >
	    blk_rq_get_max_sectors(req, bio->bi_iter.bi_sector)) {
		req_set_nomerge(req->q, req);
		return 0;
	}

	return ll_new_hw_segment(req, bio, nr_segs);
}

/*
 * req_attempt_discard_merge - 두 Discard request를 NVMe Dataset Management
 *                             명령 하나로 합칠 수 있는지 시도한다.
 * queue_max_discard_segments는 NVMe 컨트롤러가 한 번의 DSM 명령으로 처리할 수
 * 있는 range 개수에 대응한다(추정).
 */
static bool req_attempt_discard_merge(struct request_queue *q, struct request *req,
		struct request *next)
{
	unsigned short segments = blk_rq_nr_discard_segments(req);

	/* queue_max_discard_segments는 NVMe Dataset Management 명령이 한 번에
	 * 처리할 수 있는 range 개수. 초과하면 병합 불가. */
	if (segments >= queue_max_discard_segments(q))
		goto no_merge;
	if (blk_rq_sectors(req) + bio_sectors(next->bio) >
	    blk_rq_get_max_sectors(req, blk_rq_pos(req)))
		goto no_merge;

	req->nr_phys_segments = segments + blk_rq_nr_discard_segments(next);
	return true;
no_merge:
	req_set_nomerge(q, req);
	return false;
}

/*
 * ll_merge_requests_fn - scheduler가 관리하는 두 request를 병합한다.
 * 두 request의 물리 세그먼트 합이 blk_rq_get_max_segments를 초과하면 NVMe
 * PRP/SGL capacity를 넘게 되므로 병합을 거부한다.
 */
static int ll_merge_requests_fn(struct request_queue *q, struct request *req,
				struct request *next)
{
	int total_phys_segments;

	if (req_gap_back_merge(req, next->bio))
		return 0;

	/*
	 * Will it become too large?
	 */
	/* 두 request를 합친 섹터 수가 NVMe max_sectors(MDTS)를 초과하면
	 * 병합하지 않는다. */
	if ((blk_rq_sectors(req) + blk_rq_sectors(next)) >
	    blk_rq_get_max_sectors(req, blk_rq_pos(req)))
		return 0;

	/* 두 request의 세그먼트 합이 max_segments를 초과하면 NVMe SGL/PRP
	 * capacity를 넘게 되므로 병합하지 않는다. */
	total_phys_segments = req->nr_phys_segments + next->nr_phys_segments;
	if (total_phys_segments > blk_rq_get_max_segments(req))
		return 0;

	if (!blk_cgroup_mergeable(req, next->bio))
		return 0;

	if (blk_integrity_merge_rq(q, req, next) == false)
		return 0;

	if (!bio_crypt_ctx_merge_rq(req, next))
		return 0;

	/* Merge is OK... */
	req->nr_phys_segments = total_phys_segments;
	req->nr_integrity_segments += next->nr_integrity_segments;
	return 1;
}

/*
 * blk_rq_set_mixed_merge - request를 mixed merge 상태로 표시한다.
 * @rq: 표시할 request
 *
 * 여러 bio의 FAILFAST 속성(REQ_FAILFAST_DEV/REQ_FAILFAST_TRANSPORT/
 * REQ_FAILFAST_DRIVER)을 하나의 request로 합치는 경우, NVMe timeout/retry
 * 정책에 영향을 주는 플래그들을 각 bio에도 분산시켜야 나중에 분할 완료나
 * partial completion 처리 시 일관성이 유지된다.
 */
/**
 * blk_rq_set_mixed_merge - mark a request as mixed merge
 * @rq: request to mark as mixed merge
 *
 * Description:
 *     @rq is about to be mixed merged.  Make sure the attributes
 *     which can be mixed are set in each bio and mark @rq as mixed
 *     merged.
 */
static void blk_rq_set_mixed_merge(struct request *rq)
{
	blk_opf_t ff = rq->cmd_flags & REQ_FAILFAST_MASK;
	struct bio *bio;

	/* 이미 mixed merge로 표시되면 추가 분배 불필요. */
	if (rq->rq_flags & RQF_MIXED_MERGE)
		return;

	/*
	 * @rq will no longer represent mixable attributes for all the
	 * contained bios.  It will just track those of the first one.
	 * Distributes the attributs to each bio.
	 */
	/* rq에 포함된 모든 bio에 failfast 플래그를 분배. NVMe timeout/abort
	 * 처리 시 각 bio 단위로 retry 여부를 결정할 수 있게 한다(추정). */
	for (bio = rq->bio; bio; bio = bio->bi_next) {
		WARN_ON_ONCE((bio->bi_opf & REQ_FAILFAST_MASK) &&
			     (bio->bi_opf & REQ_FAILFAST_MASK) != ff);
		bio->bi_opf |= ff;
	}
	rq->rq_flags |= RQF_MIXED_MERGE;
}

static inline blk_opf_t bio_failfast(const struct bio *bio)
{
	/* read-ahead bio는 항상 failfast로 처리해 NVMe timeout 시 aggressive
	 * retry를 피하고 throughput을 우선한다. */
	if (bio->bi_opf & REQ_RAHEAD)
		return REQ_FAILFAST_MASK;

	return bio->bi_opf & REQ_FAILFAST_MASK;
}

/*
 * MIXED_MERGE 상태에서 새로 들어온 RA(read-ahead) bio는 failfast로 강제되며,
 * front merge인 경우 request의 failfast 플래그도 새 bio의 값으로 갱신된다.
 * NVMe read-ahead는 대개 성능 우선으로, 에러 시 aggressive하게 failfast 처리한다.
 */
/*
 * After we are marked as MIXED_MERGE, any new RA bio has to be updated
 * as failfast, and request's failfast has to be updated in case of
 * front merge.
 */
static inline void blk_update_mixed_merge(struct request *req,
		struct bio *bio, bool front_merge)
{
	if (req->rq_flags & RQF_MIXED_MERGE) {
		if (bio->bi_opf & REQ_RAHEAD)
			bio->bi_opf |= REQ_FAILFAST_MASK;

		/* front merge인 경우 req의 failfast 플래그를 새 bio의 값으로
		 * 갱신. 이 값은 이후 NVMe abort/retry 경로에서 사용된다. */
		if (front_merge) {
			req->cmd_flags &= ~REQ_FAILFAST_MASK;
			req->cmd_flags |= bio->bi_opf & REQ_FAILFAST_MASK;
		}
	}
}

static void blk_account_io_merge_request(struct request *req)
{
	if (req->rq_flags & RQF_IO_STAT) {
		part_stat_lock();
		part_stat_inc(req->part, merges[op_stat_group(req_op(req))]);
		part_stat_local_dec(req->part,
				    in_flight[op_is_write(req_op(req))]);
		part_stat_unlock();
	}
}

static enum elv_merge blk_try_req_merge(struct request *req,
					struct request *next)
{
	/* Discard merge 가능이면 Dataset Management로 합친다. 일반 I/O는
	 * LBA 연속성에 따라 back merge 판단. front merge는 요청 단위에서는
	 * 검사하지 않는다. */
	if (blk_discard_mergable(req))
		return ELEVATOR_DISCARD_MERGE;
	else if (blk_rq_pos(req) + blk_rq_sectors(req) == blk_rq_pos(next))
		return ELEVATOR_BACK_MERGE;

	return ELEVATOR_NO_MERGE;
}

static bool blk_atomic_write_mergeable_rq_bio(struct request *rq,
					      struct bio *bio)
{
	/* atomic write 속성이 일치해야만 같은 NVMe atomic write 명령으로
	 * 병합할 수 있다. */
	return (rq->cmd_flags & REQ_ATOMIC) == (bio->bi_opf & REQ_ATOMIC);
}

static bool blk_atomic_write_mergeable_rqs(struct request *rq,
					   struct request *next)
{
	return (rq->cmd_flags & REQ_ATOMIC) == (next->cmd_flags & REQ_ATOMIC);
}

/*
 * bio_seg_gap - 연속된 두 bio 사이의 물리적 불연속 비트를 계산한다.
 * NVMe SGL/PRP 구성 시 이 값을 통해 컨트롤러가 다음 페이지로 넘어가야 하는
 * 지점을 미리 파악할 수 있다(추정). gap이 생기면 별도의 PRP/SGL 엔트리가
 * 필요하다.
 */
u8 bio_seg_gap(struct request_queue *q, struct bio *prev, struct bio *next,
	       u8 gaps_bit)
{
	struct bio_vec pb, nb;

	if (!bio_has_data(prev))
		return 0;

	/* 두 bio가 각자 남긴 gap 비트와 실제 bvec 간 물리적 gap 중 가장
	 * 작은(가장 일찍 발생하는) bit를 선택. NVMe DMA mapping은 이 값을
	 * 통해 추가 PRP/SGL 엔트리가 필요한 지점을 판단할 수 있다(추정). */
	gaps_bit = min_not_zero(gaps_bit, prev->bi_bvec_gap_bit);
	gaps_bit = min_not_zero(gaps_bit, next->bi_bvec_gap_bit);

	bio_get_last_bvec(prev, &pb);
	bio_get_first_bvec(next, &nb);
	if (!biovec_phys_mergeable(q, &pb, &nb))
		gaps_bit = min_not_zero(gaps_bit, ffs(bvec_seg_gap(&pb, &nb)));
	return gaps_bit;
}

/*
 * attempt_merge - 두 request를 실제로 병합한다.
 * @q:  request_queue, NVMe 호스트의 hardware queue context를 간접 참조
 * @req: 기준이 될 request
 * @next: 병합될 대상 request
 *
 * 이 함수는 blk-mq 스케줄러나 plug list에서 두 request를 하나로 합쳐 SQ에
 * 들어갈 명령 수를 줄인다. 병합 성공 시 next는 호출자에 의해 해제되며,
 * req->nr_phys_segments 등이 갱신되어 nvme_queue_rq에서 PRP/SGL을 만들 때
 * 사용된다.
 *
 * 호출 경로: blk_mq_sched_try_merge -> attempt_back_merge/attempt_front_merge ->
 *           attempt_merge
 */
/*
 * For non-mq, this has to be called with the request spinlock acquired.
 * For mq with scheduling, the appropriate queue wide lock should be held.
 */
static struct request *attempt_merge(struct request_queue *q,
				     struct request *req, struct request *next)
{
	/* 병합 가능 플래그(REQ_NOMERGE 등)가 설정되어 있으면 즉시 포기. */
	if (!rq_mergeable(req) || !rq_mergeable(next))
		return NULL;

	/* op가 다른 한 NVMe 명령 opcode가 달라지므로 병합 불가. */
	if (req_op(req) != req_op(next))
		return NULL;

	/* write_hint/stream/ioprio가 다른 NVMe 명령 내 동일한 우선순위/
	 * 힌트 유지가 어려우므로 병합 불가(추정). */
	if (req->bio->bi_write_hint != next->bio->bi_write_hint)
		return NULL;
	if (req->bio->bi_write_stream != next->bio->bi_write_stream)
		return NULL;
	if (req->bio->bi_ioprio != next->bio->bi_ioprio)
		return NULL;
	if (!blk_atomic_write_mergeable_rqs(req, next))
		return NULL;

	/*
	 * If we are allowed to merge, then append bio list
	 * from next to rq and release next. merge_requests_fn
	 * will have updated segment counts, update sector
	 * counts here. Handle DISCARDs separately, as they
	 * have separate settings.
	 */

	/* 병합 종류에 따라 세그먼트/섹터 제약을 추가 검사. */
	switch (blk_try_req_merge(req, next)) {
	case ELEVATOR_DISCARD_MERGE:
		if (!req_attempt_discard_merge(q, req, next))
			return NULL;
		break;
	case ELEVATOR_BACK_MERGE:
		if (!ll_merge_requests_fn(q, req, next))
			return NULL;
		break;
	default:
		return NULL;
	}

	/*
	 * If failfast settings disagree or any of the two is already
	 * a mixed merge, mark both as mixed before proceeding.  This
	 * makes sure that all involved bios have mixable attributes
	 * set properly.
	 */
	if (((req->rq_flags | next->rq_flags) & RQF_MIXED_MERGE) ||
	    (req->cmd_flags & REQ_FAILFAST_MASK) !=
	    (next->cmd_flags & REQ_FAILFAST_MASK)) {
		blk_rq_set_mixed_merge(req);
		blk_rq_set_mixed_merge(next);
	}

	/*
	 * At this point we have either done a back merge or front merge. We
	 * need the smaller start_time_ns of the merged requests to be the
	 * current request for accounting purposes.
	 */
	/* timeout 측정 기준을 병합된 요청 중 가장 오래된 시작 시점으로 맞춘다.
	 * 이 값은 이후 NVMe timeout/abort 판단에 사용된다(추정). */
	if (next->start_time_ns < req->start_time_ns)
		req->start_time_ns = next->start_time_ns;

	req->phys_gap_bit = bio_seg_gap(req->q, req->biotail, next->bio,
					min_not_zero(next->phys_gap_bit,
						     req->phys_gap_bit));
	req->biotail->bi_next = next->bio;
	req->biotail = next->biotail;

	req->__data_len += blk_rq_bytes(next);

	if (!blk_discard_mergable(req))
		elv_merge_requests(q, req, next);

	blk_crypto_rq_put_keyslot(next);

	/*
	 * 'next' is going away, so update stats accordingly
	 */
	blk_account_io_merge_request(next);

	trace_block_rq_merge(next);

	/*
	 * ownership of bio passed from next to req, return 'next' for
	 * the caller to free
	 */
	/* next의 bio ownership은 req로 이전. 호출자가 next request를 해제하면
	 * req만 남아 NVMe SQ로 향한다. */
	next->bio = NULL;
	return next;
}

static struct request *attempt_back_merge(struct request_queue *q,
		struct request *rq)
{
	/* scheduler list에서 rq의 뒤쪽 이웃 request를 찾아 병합. */
	struct request *next = elv_latter_request(q, rq);

	if (next)
		return attempt_merge(q, rq, next);

	return NULL;
}

static struct request *attempt_front_merge(struct request_queue *q,
		struct request *rq)
{
	/* scheduler list에서 rq의 앞쪽 이웃 request를 찾아 prev <- rq 형태로
	 * 병합. */
	struct request *prev = elv_former_request(q, rq);

	if (prev)
		return attempt_merge(q, prev, rq);

	return NULL;
}

/*
 * blk_attempt_req_merge - 두 request를 병합한다.
 * NVMe multi-queue 환경에서 scheduler나 timeout/abort 경로에서 호출되어,
 * SQ에 들어가는 명령 수를 줄이거나 abort 시 상위 bio 단위를 재구성할 때
 * 사용된다.
 */
/*
 * Try to merge 'next' into 'rq'. Return true if the merge happened, false
 * otherwise. The caller is responsible for freeing 'next' if the merge
 * happened.
 */
bool blk_attempt_req_merge(struct request_queue *q, struct request *rq,
			   struct request *next)
{
	return attempt_merge(q, rq, next);
}

/*
 * blk_rq_merge_ok - request와 bio가 병합 가능한 기본 조건을 만족하는지 검사.
 * op, cgroup, integrity, crypto, write_hint/stream, ioprio, atomic write
 * 속성이 모두 일치해야 한다. NVMe 컨트롤러는 한 명령 내에서 이러한 속성이
 * 달라지는 것을 허용하지 않으므로(예: PI 활성화 여부, FUA, atomic 영역),
 * 사전에 병합을 차단한다.
 */
bool blk_rq_merge_ok(struct request *rq, struct bio *bio)
{
	if (!rq_mergeable(rq) || !bio_mergeable(bio))
		return false;

	if (req_op(rq) != bio_op(bio))
		return false;

	if (!blk_cgroup_mergeable(rq, bio))
		return false;
	if (blk_integrity_merge_bio(rq->q, rq, bio) == false)
		return false;
	if (!bio_crypt_rq_ctx_compatible(rq, bio))
		return false;
	if (rq->bio->bi_write_hint != bio->bi_write_hint)
		return false;
	if (rq->bio->bi_write_stream != bio->bi_write_stream)
		return false;
	if (rq->bio->bi_ioprio != bio->bi_ioprio)
		return false;
	if (blk_atomic_write_mergeable_rq_bio(rq, bio) == false)
		return false;

	return true;
}

/*
 * blk_try_merge - request에 대해 bio가 수행할 수 있는 병합 종류를 판별.
 * ELEVATOR_BACK_MERGE/FONT_MERGE 판단은 LBA 연속성을 기준으로 하며,
 * NVMe 명령의 Starting LBA(SLBA)와 Length(NLB)를 합칠 수 있는지를 사전 검사한다.
 */
enum elv_merge blk_try_merge(struct request *rq, struct bio *bio)
{
	if (blk_discard_mergable(rq))
		return ELEVATOR_DISCARD_MERGE;
	else if (blk_rq_pos(rq) + blk_rq_sectors(rq) == bio->bi_iter.bi_sector)
		return ELEVATOR_BACK_MERGE;
	else if (blk_rq_pos(rq) - bio_sectors(bio) == bio->bi_iter.bi_sector)
		return ELEVATOR_FRONT_MERGE;
	return ELEVATOR_NO_MERGE;
}

static void blk_account_io_merge_bio(struct request *req)
{
	if (req->rq_flags & RQF_IO_STAT) {
		part_stat_lock();
		part_stat_inc(req->part, merges[op_stat_group(req_op(req))]);
		part_stat_unlock();
	}
}

/*
 * bio_attempt_back_merge - request 뒤에 bio를 병합한다.
 * ll_back_merge_fn으로 물리/논리적 가능성을 검증한 뒤 bio 리스트를 연결하고
 * req->__data_len, req->nr_phys_segments를 갱신한다. NVMe에서는 이 request가
 * SQ에 삽입될 때 하나의 I/O 명령으로 변환된다.
 */
enum bio_merge_status bio_attempt_back_merge(struct request *req,
		struct bio *bio, unsigned int nr_segs)
{
	const blk_opf_t ff = bio_failfast(bio);

	if (!ll_back_merge_fn(req, bio, nr_segs))
		return BIO_MERGE_FAILED;

	trace_block_bio_backmerge(bio);
	rq_qos_merge(req->q, req, bio);

	if ((req->cmd_flags & REQ_FAILFAST_MASK) != ff)
		blk_rq_set_mixed_merge(req);

	blk_update_mixed_merge(req, bio, false);

	if (req->rq_flags & RQF_ZONE_WRITE_PLUGGING)
		blk_zone_write_plug_bio_merged(bio);

	req->phys_gap_bit = bio_seg_gap(req->q, req->biotail, bio,
					req->phys_gap_bit);
	req->biotail->bi_next = bio;
	req->biotail = bio;
	req->__data_len += bio->bi_iter.bi_size;

	bio_crypt_free_ctx(bio);

	blk_account_io_merge_bio(req);
	return BIO_MERGE_OK;
}

/*
 * bio_attempt_front_merge - request 앞에 bio를 병합한다.
 * Zoned SSD의 sequential write zone에 대해서는 out-of-order front merge를
 * 허용하지 않아 zone write pointer 순서를 보호한다. NVMe ZNS의 경우 이 제약이
 * 데이터 무결성과 직결된다.
 */
static enum bio_merge_status bio_attempt_front_merge(struct request *req,
		struct bio *bio, unsigned int nr_segs)
{
	const blk_opf_t ff = bio_failfast(bio);

	/*
	 * A front merge for writes to sequential zones of a zoned block device
	 * can happen only if the user submitted writes out of order. Do not
	 * merge such write to let it fail.
	 */
	if (req->rq_flags & RQF_ZONE_WRITE_PLUGGING)
		return BIO_MERGE_FAILED;

	if (!ll_front_merge_fn(req, bio, nr_segs))
		return BIO_MERGE_FAILED;

	trace_block_bio_frontmerge(bio);
	rq_qos_merge(req->q, req, bio);

	if ((req->cmd_flags & REQ_FAILFAST_MASK) != ff)
		blk_rq_set_mixed_merge(req);

	blk_update_mixed_merge(req, bio, true);

	req->phys_gap_bit = bio_seg_gap(req->q, bio, req->bio,
					req->phys_gap_bit);
	bio->bi_next = req->bio;
	req->bio = bio;

	req->__sector = bio->bi_iter.bi_sector;
	req->__data_len += bio->bi_iter.bi_size;

	bio_crypt_do_front_merge(req, bio);

	blk_account_io_merge_bio(req);
	return BIO_MERGE_OK;
}

/*
 * bio_attempt_discard_merge - Discard bio를 request 뒤에 병합한다.
 * queue_max_discard_segments가 NVMe Dataset Management의 max range 수와
 * 연결되므로 초과하면 병합하지 않는다.
 */
static enum bio_merge_status bio_attempt_discard_merge(struct request_queue *q,
		struct request *req, struct bio *bio)
{
	unsigned short segments = blk_rq_nr_discard_segments(req);

	if (segments >= queue_max_discard_segments(q))
		goto no_merge;
	if (blk_rq_sectors(req) + bio_sectors(bio) >
	    blk_rq_get_max_sectors(req, blk_rq_pos(req)))
		goto no_merge;

	rq_qos_merge(q, req, bio);

	req->biotail->bi_next = bio;
	req->biotail = bio;
	req->__data_len += bio->bi_iter.bi_size;
	req->nr_phys_segments = segments + 1;

	blk_account_io_merge_bio(req);
	return BIO_MERGE_OK;
no_merge:
	req_set_nomerge(q, req);
	return BIO_MERGE_FAILED;
}

/*
 * blk_attempt_bio_merge - 단일 bio를 큐의 기존 request와 병합을 시도한다.
 * blk_mq_sched_try_merge나 blk_attempt_plug_merge, blk_bio_list_merge에서
 * 호출되며, NVMe SQ 엔트리 수를 줄이는 핵심 경로이다.
 */
static enum bio_merge_status blk_attempt_bio_merge(struct request_queue *q,
						   struct request *rq,
						   struct bio *bio,
						   unsigned int nr_segs,
						   bool sched_allow_merge)
{
	if (!blk_rq_merge_ok(rq, bio))
		return BIO_MERGE_NONE;

	/* blk_try_merge가 LBA 연속성을 판별하고, sched_allow_merge가 true면
	 * blk_mq_sched_allow_merge를 통해 scheduler가 추가 제약(예: I/O
	 * isolation, queue depth)을 검사한다. */
	switch (blk_try_merge(rq, bio)) {
	case ELEVATOR_BACK_MERGE:
		if (!sched_allow_merge || blk_mq_sched_allow_merge(q, rq, bio))
			return bio_attempt_back_merge(rq, bio, nr_segs);
		break;
	case ELEVATOR_FRONT_MERGE:
		if (!sched_allow_merge || blk_mq_sched_allow_merge(q, rq, bio))
			return bio_attempt_front_merge(rq, bio, nr_segs);
		break;
	case ELEVATOR_DISCARD_MERGE:
		return bio_attempt_discard_merge(q, rq, bio);
	default:
		return BIO_MERGE_NONE;
	}

	return BIO_MERGE_FAILED;
}

/*
 * blk_attempt_plug_merge - 현재 태스크의 plug list에 있는 request와 bio를 병합.
 * @q: bio가 큐잉되는 request_queue(NVMe hardware queue를 간접 참조)
 * @bio: 새로 들어온 bio
 * @nr_segs: bio의 세그먼트 수
 *
 * Plugging은 동일한 issuer의 작은 I/O들을 scheduler에 가기 전에 먼저 모아
 * NVMe SQ에 들어갈 request 크기를 키우는 메커니즘이다. 이 단계에서 병합이
 * 성공하면 doorbell 횟수를 줄이고 CPU 부하를 낮출 수 있다.
 *
 * 호출 경로: submit_bio -> blk_mq_submit_bio -> blk_attempt_plug_merge
 */
/**
 * blk_attempt_plug_merge - try to merge with %current's plugged list
 * @q: request_queue new bio is being queued at
 * @bio: new bio being queued
 * @nr_segs: number of segments in @bio
 * from the passed in @q already in the plug list
 *
 * Determine whether @bio being queued on @q can be merged with the previous
 * request on %current's plugged list.  Returns %true if merge was successful,
 * otherwise %false.
 *
 * Plugging coalesces IOs from the same issuer for the same purpose without
 * going through @q->queue_lock.  As such it's more of an issuing mechanism
 * than scheduling, and the request, while may have elvpriv data, is not
 * added on the elevator at this point.  In addition, we don't have
 * reliable access to the elevator outside queue lock.  Only check basic
 * merging parameters without querying the elevator.
 *
 * Caller must ensure !blk_queue_nomerges(q) beforehand.
 */
bool blk_attempt_plug_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs)
{
	struct blk_plug *plug = current->plug;
	struct request *rq;

	/* plug가 없거나 비어 있으면 병합 대상이 없음. */
	if (!plug || rq_list_empty(&plug->mq_list))
		return false;

	/* tail request가 같은 큐(q)에 속하면 먼저 시도. plug list는 동일
	 * issuer의 I/O를 모으므로 LBA 연속성이 가장 높을 가능성이 크다. */
	rq = plug->mq_list.tail;
	if (rq->q == q)
		return blk_attempt_bio_merge(q, rq, bio, nr_segs, false) ==
			BIO_MERGE_OK;
	else if (!plug->multiple_queues)
		return false;

	/* multiple_queues가 설정되면 다른 큐의 request도 순회. 같은 q를
	 * 찾으면 병합 시도 후 즉시 종료. */
	rq_list_for_each(&plug->mq_list, rq) {
		if (rq->q != q)
			continue;
		if (blk_attempt_bio_merge(q, rq, bio, nr_segs, false) ==
		    BIO_MERGE_OK)
			return true;
		break;
	}
	return false;
}

/*
 * blk_bio_list_merge - request 리스트를 역순으로 탐색하며 bio와 병합 가능 여부를
 *                      검사한다.
 * NVMe multi-queue scheduler 낮에서 I/O를 dispatch하기 전에 호출되어, 리스트
 * 내 연속된 LBA를 가진 request들을 하나로 합친다.
 */
/*
 * Iterate list of requests and see if we can merge this bio with any
 * of them.
 */
bool blk_bio_list_merge(struct request_queue *q, struct list_head *list,
			struct bio *bio, unsigned int nr_segs)
{
	struct request *rq;
	int checked = 8;

	/* list 끝에서부터 최대 8개 request까지만 검사. 이 제한은 scheduler
	 * dispatch 전 병합 오버헤드를 줄이며, NVMe SQ 도달 전 큰 request를
	 * 만드는 것이 목표다. */
	list_for_each_entry_reverse(rq, list, queuelist) {
		if (!checked--)
			break;

		switch (blk_attempt_bio_merge(q, rq, bio, nr_segs, true)) {
		case BIO_MERGE_NONE:
			continue;
		case BIO_MERGE_OK:
			return true;
		case BIO_MERGE_FAILED:
			return false;
		}

	}

	return false;
}
EXPORT_SYMBOL_GPL(blk_bio_list_merge);

/*
 * blk_mq_sched_try_merge - blk-mq scheduler가 bio를 기존 request와 병합한다.
 * @merged_request: 병합 후 합쳐진 상대 request가 반환되면 호출자가 해제해야 함.
 *
 * 이 함수는 submit_bio -> blk_mq_submit_bio 경로에서 bio가 새 request가 되기
 * 전에 마지막으로 병합을 시도한다. 성공 시 NVMe SQ에는 더 큰 단위의 명령이
 * 하나 들어가며, 실패 시 별도의 request가 할당된다.
 *
 * 호출 경로: submit_bio -> blk_mq_submit_bio -> blk_mq_sched_try_merge ->
 *           bio_attempt_back_merge/attempt_back_merge ->
 *           nvme_queue_rq -> nvme_submit_cmd(doorbell)
 */
bool blk_mq_sched_try_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs, struct request **merged_request)
{
	struct request *rq;

	/* elv_merge가 scheduler의 hint를 이용해 병합 후보를 찾는다. 성공
	 * 시 bio_attempt_*_merge -> attempt_*_merge를 통해 request가 커지고,
	 * 이후 blk_mq_get_request -> nvme_queue_rq로 넘어간다. */
	switch (elv_merge(q, &rq, bio)) {
	case ELEVATOR_BACK_MERGE:
		if (!blk_mq_sched_allow_merge(q, rq, bio))
			return false;
		if (bio_attempt_back_merge(rq, bio, nr_segs) != BIO_MERGE_OK)
			return false;
		*merged_request = attempt_back_merge(q, rq);
		if (!*merged_request)
			elv_merged_request(q, rq, ELEVATOR_BACK_MERGE);
		return true;
	case ELEVATOR_FRONT_MERGE:
		if (!blk_mq_sched_allow_merge(q, rq, bio))
			return false;
		if (bio_attempt_front_merge(rq, bio, nr_segs) != BIO_MERGE_OK)
			return false;
		*merged_request = attempt_front_merge(q, rq);
		if (!*merged_request)
			elv_merged_request(q, rq, ELEVATOR_FRONT_MERGE);
		return true;
	case ELEVATOR_DISCARD_MERGE:
		return bio_attempt_discard_merge(q, rq, bio) == BIO_MERGE_OK;
	default:
		return false;
	}
}
EXPORT_SYMBOL_GPL(blk_mq_sched_try_merge);

/*
 * ============================================================================
 * NVMe 관점 핵심 요약
 * ============================================================================
 *  - 이 파일은 상위 bio를 NVMe SQ 엔트리 하나에 실을 수 있는 크기/형태로
 *    분할/병합하며, 결과적으로 doorbell 횟수와 PRP/SGL 엔트리 수를 결정한다.
 *  - queue_limits의 max_segments, max_sectors, virt_boundary_mask는 NVMe
 *    Identify Controller/Namespace에서 보고된 MDTS, maximum SGL/PRP,
 *    DMA 정렬 특성을 blk layer 형식으로 표현한 것이다(추정).
 *  - bio_will_gap, bvec_split_segs 등은 메모리 물리 주소의 연속성을 검사해
 *    NVMe DMA(SGL/PRP)가 올바르게 구성되도록 보장한다(추정).
 *  - Discard/Secure Erase/Write Zeroes는 각각 NVMe Dataset Management,
 *    Sanitize, Write Zeroes 명령의 제약을 반영하여 분할/병합한다.
 *  - 이 파일의 처리 이후 request는 block/blk-mq.c의 blk_mq_get_request를
 *    거쳐 nvme_queue_rq로 전달되고, 최종적으로 nvme_submit_cmd()에서
 *    doorbell을 울려 NVMe 컨트롤러에 전송된다.
 * ============================================================================
 */
