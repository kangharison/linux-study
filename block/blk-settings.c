// SPDX-License-Identifier: GPL-2.0
/*
 * Functions related to setting various queue properties from drivers
 *
 * NVMe 관점 파일 요약:
 * 이 파일은 block layer의 request_queue(q)와 queue_limits(lim)를 초기화/검증/스택하는
 * 함수들을 담고 있다. NVMe SSD 드라이버(drivers/nvme/host/pci.c 등)가 호스트 메모리와
 * NVMe 컨트롤러 간 SQ/CQ, PRP/SGL, doorbell, CID 등의 하드웨어 제약을 queue_limits
 * 형태로 등록하면, 본 파일이 이를 정규화하여 상위 bio/request 경로
 * (submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *  nvme_submit_cmd(doorbell))로 전달될 I/O 크기/정렬/세그먼트 한도를 확정한다.
 * blk-mq core, elevator, rq-qos, partition, integrity, zoned block 장치들과
 * 밀접하게 연결된다.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/blk-integrity.h>
#include <linux/pagemap.h>
#include <linux/backing-dev-defs.h>
#include <linux/gcd.h>
#include <linux/lcm.h>
#include <linux/jiffies.h>
#include <linux/gfp.h>
#include <linux/dma-mapping.h>
#include <linux/t10-pi.h>
#include <linux/crc64.h>

#include "blk.h"
#include "blk-rq-qos.h"
#include "blk-wbt.h"

/*
 * struct request_queue NVMe 관련 주요 필드:
 *   queue_depth: NVMe IO SQ/CQ pair의 최대 엔트리 수(Create IO Queue에서 설정).
 *   rq_timeout: NVMe command timeout 후 abort/reset 처리 기준(밀리초).
 *   limits: queue_limits. NVMe Identify/Controller capability가 저장됨.
 *   tags/tag_set: blk-mq tag ↔ NVMe CID/SQ slot 매핑을 위한 자료구조.
 *   (추정) bdi: read-ahead, writeback 시 NVMe optimal I/O 크기 힌트 반영.
 */
void blk_queue_rq_timeout(struct request_queue *q, unsigned int timeout)
{
	/* q->rq_timeout은 nvme_timeout -> nvme_abort_req -> Abort 명령 CID
	 * 선택 기준이 되며, jiffies 단위로 변환되어 nvme watchdog에서 사용된다. */
	WRITE_ONCE(q->rq_timeout, timeout);
}
EXPORT_SYMBOL_GPL(blk_queue_rq_timeout);

/**
 * blk_set_stacking_limits - set default limits for stacking devices
 * @lim:  the queue_limits structure to reset
 *
 * Prepare queue limits for applying limits from underlying devices using
 * blk_stack_limits().
 *
 * NVMe 관점:
 *   상위 가상 장치(MD/DM 등)가 하위 NVMe SSD의 q->limits를 상속받기 전에
 *   lim을 "제한 없음" 상태로 초기화한다. 이후 blk_stack_limits()에서
 *   NVMe max_hw_sectors, max_segments, discard, zoned 등을 교차 병합한다.
 */
void blk_set_stacking_limits(struct queue_limits *lim)
{
	/* 상속 전 구조체를 0으로 클리어하여 하위 NVMe 값과 min/max 병합 시
	 * neutral identity(max 또는 0)가 된다. */
	memset(lim, 0, sizeof(*lim));
	/* NVMe LBA Data Size 기본값: 드라이버가 명시하지 않으면 512B. */
	lim->logical_block_size = SECTOR_SIZE;
	/* NVMe 물리 페이지 정렬 기본값, LBAF에서 4K일 경우 갱신됨. */
	lim->physical_block_size = SECTOR_SIZE;
	lim->io_min = SECTOR_SIZE;
	/* NVMe Deallocate granularity 기본값, Identify에서 갱신됨. */
	lim->discard_granularity = SECTOR_SIZE;
	/* NVMe DMA/PRP 시작 주소 정렬 기본값: 512B 경계. */
	lim->dma_alignment = SECTOR_SIZE - 1;
	/* PRP/SGL 엔트리가 넘지 않을 기본 4GB 물리 경계. */
	lim->seg_boundary_mask = BLK_SEG_BOUNDARY_MASK;

	/* Inherit limits from component devices */
	/* 하위 NVMe max_segments(PRP/SGL 최대 엔트리)와 min 병합 준비. */
	lim->max_segments = USHRT_MAX;
	/* NVMe DSM/Deallocate scatter-gather segment 상한 준비. */
	lim->max_discard_segments = USHRT_MAX;
	/* 하위 NVMe MDTS와 min 병합 준비. */
	lim->max_hw_sectors = UINT_MAX;
	/* 단일 PRP/SGL 엔트리 바이트 상한 준비. */
	lim->max_segment_size = UINT_MAX;
	/* 사용자/커널 최종 I/O 크기 상한 준비. */
	lim->max_sectors = UINT_MAX;
	lim->max_dev_sectors = UINT_MAX;
	/* NVMe Write Zeroes 최대 섹터 상한 준비. */
	lim->max_write_zeroes_sectors = UINT_MAX;
	lim->max_hw_wzeroes_unmap_sectors = UINT_MAX;
	lim->max_user_wzeroes_unmap_sectors = UINT_MAX;
	/* NVMe ZNS Zone Append 최대 섹터 상한 준비. */
	lim->max_hw_zone_append_sectors = UINT_MAX;
	lim->max_user_discard_sectors = UINT_MAX;
	lim->atomic_write_hw_max = UINT_MAX;
}
EXPORT_SYMBOL(blk_set_stacking_limits);

/**
 * blk_apply_bdi_limits - NVMe optimal I/O 크기를 read-ahead/writeback에 반영
 * @bdi:  상위 block device의 backing_dev_info
 * @lim:  정규화된 queue_limits
 *
 * NVMe 연결 지점:
 *   lim->io_opt는 NVMe 컨트롤러가 권장하는 최적 I/O 크기(Optimal Write Size 등).
 *   bdi->ra_pages, bdi->io_pages가 이 값을 반영하면 NVMe SQ 엔트리 효율과
 *   read-ahead 적중률이 개선된다.
 *   (추정) NVMe SSD는 회전식 디스크와 달리 BLK_FEAT_ROTATIONAL이 꺼져 있으므로
 *   io_opt가 0이면 max_sectors 기반의 큰 read-ahead를 사용하지 않는다.
 */
void blk_apply_bdi_limits(struct backing_dev_info *bdi,
		struct queue_limits *lim)
{
	u64 io_opt = lim->io_opt;

	/*
	 * For read-ahead of large files to be effective, we need to read ahead
	 * at least twice the optimal I/O size. For rotational devices that do
	 * not report an optimal I/O size (e.g. ATA HDDs), use the maximum I/O
	 * size to avoid falling back to the (rather inefficient) small default
	 * read-ahead size.
	 *
	 * There is no hardware limitation for the read-ahead size and the user
	 * might have increased the read-ahead size through sysfs, so don't ever
	 * decrease it.
	 */
	/* NVMe SSD는 BLK_FEAT_ROTATIONAL 미설정이므로 io_opt=0이면 아래 fallback
	 * 경로로 들어가지 않고 max_sectors 기반 read-ahead도 사용하지 않는다. */
	if (!io_opt && (lim->features & BLK_FEAT_ROTATIONAL))
		io_opt = (u64)lim->max_sectors << SECTOR_SHIFT;

	/* NVMe io_opt 기반 read-ahead 페이지 수 = 최소 2*io_opt/PAGE. */
	bdi->ra_pages = max3(bdi->ra_pages,
				io_opt * 2 >> PAGE_SHIFT,
				VM_READAHEAD_PAGES);
	/* writeback 단위: NVMe max_sectors/PAGE 수로, SQ 엔트리당 전송 크기에
	 * 직접 영향을 준다. */
	bdi->io_pages = lim->max_sectors >> PAGE_SECTORS_SHIFT;
}

/**
 * blk_validate_zoned_limits - ZNS(Zoned Namespace) NVMe 한도 검증
 * @lim: 검증할 queue_limits
 *
 * NVMe 연결 지점:
 *   NVMe ZNS SSD는 zone open/active 수, zone append 크기, zone write granularity
 *   등을 Identify Namespace/Controller 데이터로 보고한다.
 *   BLK_FEAT_ZONED가 설정되지 않은 일반 NVMe namespace에서는 관련 필드가 0이어야
 *   하며, 그렇지 않으면 -EINVAL을 반환한다.
 */
static int blk_validate_zoned_limits(struct queue_limits *lim)
{
	/* BLK_FEAT_ZONED 미설정 시 ZNS 관련 값이 residual하면 컨트롤러 상태 불일치. */
	if (!(lim->features & BLK_FEAT_ZONED)) {
		if (WARN_ON_ONCE(lim->max_open_zones) ||
		    WARN_ON_ONCE(lim->max_active_zones) ||
		    WARN_ON_ONCE(lim->zone_write_granularity) ||
		    WARN_ON_ONCE(lim->max_zone_append_sectors))
			return -EINVAL;
		return 0;
	}

	if (WARN_ON_ONCE(!IS_ENABLED(CONFIG_BLK_DEV_ZONED)))
		return -EINVAL;

	/*
	 * Given that active zones include open zones, the maximum number of
	 * open zones cannot be larger than the maximum number of active zones.
	 */
	/* NVMe ZNS: Open Zone 수는 Active Zone 수를 초과할 수 없음(Identify). */
	if (lim->max_active_zones &&
	    lim->max_open_zones > lim->max_active_zones)
		return -EINVAL;

	/* NVMe ZNS zone write granularity는 최소 LBA 크기 이상이어야 함. */
	if (lim->zone_write_granularity < lim->logical_block_size)
		lim->zone_write_granularity = lim->logical_block_size;

	/*
	 * The Zone Append size is limited by the maximum I/O size and the zone
	 * size given that it can't span zones.
	 *
	 * If no max_hw_zone_append_sectors limit is provided, the block layer
	 * will emulated it, else we're also bound by the hardware limit.
	 *
	 * NVMe ZNS: Zone Append 명령은 zone 경계를 넘을 수 없으므로 MDTS와
	 * zone 크기 중 작은 값으로 제한한다.
	 */
	/* NVMe ZNS Zone Append 최종 한도 = min(HW limit, zone 크기, MDTS). */
	lim->max_zone_append_sectors =
		min_not_zero(lim->max_hw_zone_append_sectors,
			min(lim->chunk_sectors, lim->max_hw_sectors));
	return 0;
}

/**
 * blk_validate_integrity_limits - NVMe DIF/DIX end-to-end data protection 검증
 * @lim: 검증할 queue_limits
 *
 * NVMe 연결 지점:
 *   NVMe PI(Protection Information, DIF/DIX) 설정은 Identify Namespace의
 *   E2E Data Protection Type, Formats로부터 채워진다.
 *   metadata_size, pi_tuple_size, csum_type, interval_exp, tag_size를 검증하여
 *   상위 bio 경로에서 잘못된 PI 조합이 생성되지 않도록 한다.
 *
 * struct blk_integrity NVMe 관련 필드 설명:
 *   metadata_size: NVMe DIF/DIX 보호 정보(metadata) 바이트 크기.
 *   pi_tuple_size: T10 PI/CRC64 PI tuple 크기.
 *   csum_type: CRC, IP, CRC64 등 NVMe E2E checksum 종류.
 *   interval_exp: PI 검증 간격(2^x 바이트), NVMe LBA 데이터 크기와 연관.
 *   tag_size: NVMe PI Reftag 크기.
 *   flags: NOGENERATE/NOVERIFY/DEVICE_CAPABLE/REF_TAG/SPLIT_INTERVAL_CAPABLE
 *          등 DIF 처리 방식을 지정.
 */
static int blk_validate_integrity_limits(struct queue_limits *lim)
{
	struct blk_integrity *bi = &lim->integrity;

	/* NVMe PI metadata_size=0이면 checksum/reftag도 없어야 정상. */
	if (!bi->metadata_size) {
		if (bi->csum_type != BLK_INTEGRITY_CSUM_NONE ||
		    bi->tag_size || ((bi->flags & BLK_INTEGRITY_REF_TAG))) {
			pr_warn("invalid PI settings.\n");
			return -EINVAL;
		}
		/* PI offloads 안 됨: block layer가 generate/verify하지 않음. */
		bi->flags |= BLK_INTEGRITY_NOGENERATE | BLK_INTEGRITY_NOVERIFY;
		return 0;
	}

	if (!IS_ENABLED(CONFIG_BLK_DEV_INTEGRITY)) {
		pr_warn("integrity support disabled.\n");
		return -EINVAL;
	}

	/* NVMe: ref tag는 checksum과 함께만 사용 가능(PI Type 1/2/3). */
	if (bi->csum_type == BLK_INTEGRITY_CSUM_NONE &&
	    (bi->flags & BLK_INTEGRITY_REF_TAG)) {
		pr_warn("ref tag not support without checksum.\n");
		return -EINVAL;
	}

	/* NVMe DIF: pi_offset+pi_tuple_size가 metadata_size 이내여야 함. */
	if (bi->pi_offset + bi->pi_tuple_size > bi->metadata_size) {
		pr_warn("pi_offset (%u) + pi_tuple_size (%u) exceeds metadata_size (%u)\n",
			bi->pi_offset, bi->pi_tuple_size, bi->metadata_size);
		return -EINVAL;
	}

	switch (bi->csum_type) {
	case BLK_INTEGRITY_CSUM_NONE:
		if (bi->pi_tuple_size) {
			pr_warn("pi_tuple_size must be 0 when checksum type is none\n");
			return -EINVAL;
		}
		break;
	case BLK_INTEGRITY_CSUM_CRC:
	case BLK_INTEGRITY_CSUM_IP:
		/* NVMe T10 PI tuple 크기는 8바이트여야 함. */
		if (bi->pi_tuple_size != sizeof(struct t10_pi_tuple)) {
			pr_warn("pi_tuple_size mismatch for T10 PI: expected %zu, got %u\n",
				 sizeof(struct t10_pi_tuple),
				 bi->pi_tuple_size);
			return -EINVAL;
		}
		break;
	case BLK_INTEGRITY_CSUM_CRC64:
		/* NVMe CRC64 PI tuple 크기는 16바이트여야 함. */
		if (bi->pi_tuple_size != sizeof(struct crc64_pi_tuple)) {
			pr_warn("pi_tuple_size mismatch for CRC64 PI: expected %zu, got %u\n",
				 sizeof(struct crc64_pi_tuple),
				 bi->pi_tuple_size);
			return -EINVAL;
		}
		break;
	}

	/* NVMe PI interval_exp 기본값 = log2(LBA Data Size). */
	if (!bi->interval_exp) {
		bi->interval_exp = ilog2(lim->logical_block_size);
	} else if (bi->interval_exp < SECTOR_SHIFT ||
		   bi->interval_exp > ilog2(lim->logical_block_size)) {
		pr_warn("invalid interval_exp %u\n", bi->interval_exp);
		return -EINVAL;
	}

	/*
	 * Some IO controllers can not handle data intervals straddling
	 * multiple bio_vecs.  For those, enforce alignment so that those are
	 * never generated, and that each buffer is aligned as expected.
	 *
	 * NVMe: PRP/SGL 엔트리가 interval 경계를 넘지 않도록 dma_alignment를
	 * 보강한다. (추정) 컨트롤러가 SPLIT_INTERVAL_CAPABLE을 선언하지 않은 경우
	 * block layer가 미리 정렬된 버퍼만 생성하게 만든다.
	 */
	/* NVMe DIF: interval 경계가 bio_vec를 넘지 않도록 dma_alignment 강화. */
	if (!(bi->flags & BLK_SPLIT_INTERVAL_CAPABLE) && bi->csum_type) {
		lim->dma_alignment = max(lim->dma_alignment,
					(1U << bi->interval_exp) - 1);
	}

	/*
	 * The block layer automatically adds integrity data for bios that don't
	 * already have it.  Limit the I/O size so that a single maximum size
	 * metadata segment can cover the integrity data for the entire I/O.
	 *
	 * NVMe: PI metadata를 담을 수 있는 단일 메타데이터 세그먼트가 전체 I/O를
	 * 커버할 수 있도록 max_sectors를 추가로 제한한다.
	 */
	/* NVMe PI metadata 버퍼 용량으로 max_sectors 추가 제한. */
	lim->max_sectors = min(lim->max_sectors,
		max_integrity_io_size(lim) >> SECTOR_SHIFT);

	return 0;
}

/*
 * Returns max guaranteed bytes which we can fit in a bio.
 *
 * We request that an atomic_write is ITER_UBUF iov_iter (so a single vector),
 * so we assume that we can fit in at least PAGE_SIZE in a segment, apart from
 * the first and last segments.
 *
 * NVMe atomic write: 컨트롤러가 FUA/FAW(Firmware Activation Without Reset) 등을
 * 통해 보장하는 원자적 쓰기 단위를 계산할 때 사용된다. (추정) NVMe FAW 단위는
 * 보통 power-of-2이며, PRP/SGL segment 수와 LBA 정렬을 동시에 만족해야 한다.
 */
static unsigned int blk_queue_max_guaranteed_bio(struct queue_limits *lim)
{
	/* NVMe atomic write: PRP/SGL 최대 엔트리 수와 BIO_MAX_VECS 중 작은 값. */
	unsigned int max_segments = min(BIO_MAX_VECS, lim->max_segments);
	unsigned int length;

	/* 처음/마지막 세그먼트는 LBA 정렬을 위해 logical_block_size만 사용. */
	length = min(max_segments, 2) * lim->logical_block_size;
	/* 중간 세그먼트는 PAGE 단위로 DMA 매핑 가능. */
	if (max_segments > 2)
		length += (max_segments - 2) * PAGE_SIZE;

	return length;
}

static void blk_atomic_writes_update_limits(struct queue_limits *lim)
{
	/* NVMe FAW 단위는 MDTS와 보장 bio 크기 중 작은 값으로 제한. */
	unsigned int unit_limit = min(lim->max_hw_sectors << SECTOR_SHIFT,
					blk_queue_max_guaranteed_bio(lim));

	/* NVMe atomic write 단위는 2의 거듭제곱이어야 함(컨트롤러 요구). */
	unit_limit = rounddown_pow_of_two(unit_limit);

	lim->atomic_write_max_sectors =
		min(lim->atomic_write_hw_max >> SECTOR_SHIFT,
			lim->max_hw_sectors);
	lim->atomic_write_unit_min =
		min(lim->atomic_write_hw_unit_min, unit_limit);
	lim->atomic_write_unit_max =
		min(lim->atomic_write_hw_unit_max, unit_limit);
	lim->atomic_write_boundary_sectors =
		lim->atomic_write_hw_boundary >> SECTOR_SHIFT;
}

/*
 * Test whether any boundary is aligned with any chunk size. Stacked
 * devices store any stripe size in t->chunk_sectors.
 */
static bool blk_valid_atomic_writes_boundary(unsigned int chunk_sectors,
					unsigned int boundary_sectors)
{
	/* chunk 또는 boundary가 0이면 정렬 조건 의미 없음. */
	if (!chunk_sectors || !boundary_sectors)
		return true;

	/* NVMe atomic boundary가 chunk(RAID stripe) 배수인지 검사. */
	if (boundary_sectors > chunk_sectors &&
	    boundary_sectors % chunk_sectors)
		return false;

	if (chunk_sectors > boundary_sectors &&
	    chunk_sectors % boundary_sectors)
		return false;

	return true;
}

/**
 * blk_validate_atomic_write_limits - NVMe 원자적 쓰기 한도 검증
 * @lim: 검증할 queue_limits
 *
 * NVMe 연결 지점:
 *   BLK_FEAT_ATOMIC_WRITES가 설정되면 컨트롤러가 보장하는 원자적 쓰기 단위를
 *   검증한다. NVMe 표준의 Atomic Write Unit Normal/Power Fail, Atomic Boundary
 *   등(Identify Controller)에서 채워진 값을 반영한다.
 *   단위는 2의 거듭제곱이어야 하며, chunk_sectors(RAID stripe)와 정렬해야 한다.
 */
static void blk_validate_atomic_write_limits(struct queue_limits *lim)
{
	unsigned int boundary_sectors;
	/* NVMe FAW를 섹터 단위로 변환. */
	unsigned int atomic_write_hw_max_sectors =
			lim->atomic_write_hw_max >> SECTOR_SHIFT;

	/* BLK_FEAT_ATOMIC_WRITES 미설정 시 NVMe atomic write 미지원으로 처리. */
	if (!(lim->features & BLK_FEAT_ATOMIC_WRITES))
		goto unsupported;

	/* UINT_MAX indicates stacked limits in initial state */
	if (lim->atomic_write_hw_max == UINT_MAX)
		goto unsupported;

	if (!lim->atomic_write_hw_max)
		goto unsupported;

	/* NVMe atomic_write_hw_unit_min은 2의 거듭제곱이어야 함. */
	if (WARN_ON_ONCE(!is_power_of_2(lim->atomic_write_hw_unit_min)))
		goto unsupported;

	/* NVMe atomic_write_hw_unit_max도 2의 거듭제곱이어야 함. */
	if (WARN_ON_ONCE(!is_power_of_2(lim->atomic_write_hw_unit_max)))
		goto unsupported;

	/* NVMe atomic_write_hw_unit_min <= unit_max <= hw_max 검증. */
	if (WARN_ON_ONCE(lim->atomic_write_hw_unit_min >
			 lim->atomic_write_hw_unit_max))
		goto unsupported;

	if (WARN_ON_ONCE(lim->atomic_write_hw_unit_max >
			 lim->atomic_write_hw_max))
		goto unsupported;

	/* NVMe FAW는 chunk_sectors(RAID stripe)를 초과할 수 없음. */
	if (WARN_ON_ONCE(lim->chunk_sectors &&
			atomic_write_hw_max_sectors > lim->chunk_sectors))
		goto unsupported;

	boundary_sectors = lim->atomic_write_hw_boundary >> SECTOR_SHIFT;

	if (boundary_sectors) {
		/* NVMe atomic boundary는 FAW 이상이어야 함. */
		if (WARN_ON_ONCE(lim->atomic_write_hw_max >
				 lim->atomic_write_hw_boundary))
			goto unsupported;

		/* NVMe atomic boundary와 chunk_sectors 정렬 검증. */
		if (WARN_ON_ONCE(!blk_valid_atomic_writes_boundary(
			lim->chunk_sectors, boundary_sectors)))
			goto unsupported;

		/*
		 * The boundary size just needs to be a multiple of unit_max
		 * (and not necessarily a power-of-2), so this following check
		 * could be relaxed in future.
		 * Furthermore, if needed, unit_max could even be reduced so
		 * that it is compliant with a !power-of-2 boundary.
		 */
		if (!is_power_of_2(boundary_sectors))
			goto unsupported;
	}

	blk_atomic_writes_update_limits(lim);
	return;

unsupported:
	/* NVMe atomic write 미지원 시 상위 경로에서 0으로 표시. */
	lim->atomic_write_max_sectors = 0;
	lim->atomic_write_boundary_sectors = 0;
	lim->atomic_write_unit_min = 0;
	lim->atomic_write_unit_max = 0;
}

/**
 * blk_validate_limits - NVMe queue_limits 정규화 및 검증
 * @lim: 검증할 queue_limits
 *
 * 목적:
 *   NVMe 컨트롤러가 노출한 max_hw_sectors, max_segments, logical_block_size,
 *   discard, zoned, integrity, atomic_write 등의 하드웨어 한도를 block layer
 *   표준 형식으로 변환하고, 상호 불가능한 조합을 걸러낸다.
 *
 * 주요 호출 경로:
 *   nvme_reset_work -> nvme_configure_admin_queue -> blk_mq_init_queue /
 *   queue_limits_set -> blk_validate_limits
 *   (PCI transport에서 queue_depth, max_hw_sectors 등을 채운 뒤 호출)
 *
 * NVMe 연결 지점:
 *   - max_hw_sectors: NVMe Identify I/O Queue Command Set의 MDTS(Maximum Data
 *     Transfer Size) 또는 드라이버가 계산한 최대 섹터 수.
 *   - max_segments: PRP/SGL scatter-gather list의 최대 항목 수 제한.
 *   - logical_block_size: NVMe LBA Format의 LBA Data Size(보통 512B 또는 4KB).
 *   - features: BLK_FEAT_POLL, BLK_FEAT_NOWAIT, BLK_FEAT_FUA,
 *     BLK_FEAT_ZONED, BLK_FEAT_ATOMIC_WRITES 등 NVMe capability 반영.
 *   - discard_*: NVMe Deallocate/DSM 관련 granularities.
 *
 * struct queue_limits NVMe 관련 주요 필드 설명:
 *   logical_block_size: NVMe LBA Data Size, LBA 단위(512B/4KB 등).
 *   physical_block_size: NVMe 미디어 물리 페이지/블록 정렬.
 *   io_min/io_opt: NVMe 권장 최소/최적 I/O 크기. SQ 엔트리 효율에 영향.
 *   max_hw_sectors: 컨트롤러가 단일 명령으로 수용하는 최대 섹터 수(MDTS 기반).
 *   max_segments: PRP/SGL 리스트 최대 엔트리 수.
 *   max_segment_size: 단일 PRP/SGL 엔트리가 가리킬 수 있는 최대 바이트.
 *   seg_boundary_mask/virt_boundary_mask: 연속 PRP/SGL 엔트리가 넘지 말아야 할
 *     물리/가상 주소 경계.
 *   dma_alignment: DMA 엔진이 요구하는 시작 주소 정렬(보통 512B).
 *   features: BLK_FEAT_POLL/NOWAIT/FUA/ZONED/ATOMIC_WRITES 등 NVMe capability.
 *   discard_*: NVMe Deallocate/DSM 명령의 granularities.
 *   atomic_write_*: NVMe FAW/FUA 기반 원자 쓰기 단위(컨트롤러 종속).
 *   integrity: NVMe DIF/DIX 형식의 end-to-end data protection 설정.
 */
int blk_validate_limits(struct queue_limits *lim)
{
	unsigned int max_hw_sectors;
	unsigned int logical_block_sectors;
	unsigned long seg_size;
	int err;

	/*
	 * Unless otherwise specified, default to 512 byte logical blocks and a
	 * physical block size equal to the logical block size.
	 *
	 * NVMe: 드라이버가 Identify Namespace로부터 LBA Data Size를 채우지 않으면
	 * 안전한 512B 기본값을 사용한다.
	 */
	/* NVMe LBA Data Size 미보고 시 512B 기본값. */
	if (!lim->logical_block_size)
		lim->logical_block_size = SECTOR_SIZE;
	else if (blk_validate_block_size(lim->logical_block_size)) {
		pr_warn("Invalid logical block size (%d)\n", lim->logical_block_size);
		return -EINVAL;
	}
	/* NVMe physical block size는 logical block size 이상의 2의 거듭제곱. */
	if (lim->physical_block_size < lim->logical_block_size) {
		lim->physical_block_size = lim->logical_block_size;
	} else if (!is_power_of_2(lim->physical_block_size)) {
		pr_warn("Invalid physical block size (%d)\n", lim->physical_block_size);
		return -EINVAL;
	}

	/*
	 * The minimum I/O size defaults to the physical block size unless
	 * explicitly overridden.
	 *
	 * NVMe: io_min은 NVMe NAND 프로그램/페이지 단위와 관련된 물리적 최소 I/O
	 * 크기를 반영한다.
	 */
	/* NVMe io_min은 physical_block_size 미만으로 낮아질 수 없음. */
	if (lim->io_min < lim->physical_block_size)
		lim->io_min = lim->physical_block_size;

	/*
	 * The optimal I/O size may not be aligned to physical block size
	 * (because it may be limited by dma engines which have no clue about
	 * block size of the disks attached to them), so we round it down here.
	 *
	 * NVMe: io_opt는 컨트롤러가 권장하는 최적 I/O 크기로, physical_block_size
	 * 배수로 내림한다.
	 */
	/* NVMe io_opt는 physical_block_size 배수로 내림(정렬 보장). */
	lim->io_opt = round_down(lim->io_opt, lim->physical_block_size);

	/*
	 * max_hw_sectors has a somewhat weird default for historical reason,
	 * but driver really should set their own instead of relying on this
	 * value.
	 *
	 * The block layer relies on the fact that every driver can
	 * handle at lest a page worth of data per I/O, and needs the value
	 * aligned to the logical block size.
	 *
	 * NVMe: NVMe 드라이버가 MDTS를 채우지 않으면 안전값 BLK_SAFE_MAX_SECTORS를
	 * 사용하지만, 실제로는 Identify에서 보고한 MDTS를 기반으로 설정해야 한다.
	 */
	/* NVMe MDTS 미보고 시 안전값 BLK_SAFE_MAX_SECTORS 사용. */
	if (!lim->max_hw_sectors)
		lim->max_hw_sectors = BLK_SAFE_MAX_SECTORS;
	/* NVMe 컨트롤러는 최소 PAGE_SIZE 이상의 I/O를 처리해야 함. */
	if (WARN_ON_ONCE(lim->max_hw_sectors < PAGE_SECTORS))
		return -EINVAL;
	/* NVMe LBA 크기(섹터) 계산. */
	logical_block_sectors = lim->logical_block_size >> SECTOR_SHIFT;
	/* NVMe LBA Data Size가 MDTS보다 클 수 없음. */
	if (WARN_ON_ONCE(logical_block_sectors > lim->max_hw_sectors))
		return -EINVAL;
	/* NVMe max_hw_sectors는 LBA 단위로 정렬. */
	lim->max_hw_sectors = round_down(lim->max_hw_sectors,
			logical_block_sectors);

	/*
	 * The actual max_sectors value is a complex beast and also takes the
	 * max_dev_sectors value (set by SCSI ULPs) and a user configurable
	 * value into account.  The ->max_sectors value is always calculated
	 * from these, so directly setting it won't have any effect.
	 *
	 * NVMe: 상위 bio 경로(submit_bio -> blk_mq_submit_bio)에서 사용할 최종
	 * 단일 I/O 크기 한도. max_hw_sectors(NVMe MDTS), max_dev_sectors,
	 * max_user_sectors 중 최소값이 적용된다.
	 */
	/* NVMe 최종 max_sectors = min(MDTS, max_dev_sectors). */
	max_hw_sectors = min_not_zero(lim->max_hw_sectors,
				lim->max_dev_sectors);
	if (lim->max_user_sectors) {
		/* 사용자가 sysfs로 제한한 최소 세그먼트 크기 미만이면 거부. */
		if (lim->max_user_sectors < BLK_MIN_SEGMENT_SIZE / SECTOR_SIZE)
			return -EINVAL;
		/* NVMe: 사용자 한도와 MDTS 중 작은 값이 최종 I/O 크기. */
		lim->max_sectors = min(max_hw_sectors, lim->max_user_sectors);
	} else if (lim->io_opt > (BLK_DEF_MAX_SECTORS_CAP << SECTOR_SHIFT)) {
		/* NVMe io_opt가 기본 캡보다 크면 io_opt 기반으로 확장. */
		lim->max_sectors =
			min(max_hw_sectors, lim->io_opt >> SECTOR_SHIFT);
	} else if (lim->io_min > (BLK_DEF_MAX_SECTORS_CAP << SECTOR_SHIFT)) {
		/* NVMe io_min이 기본 캡보다 크면 io_min 기반으로 확장. */
		lim->max_sectors =
			min(max_hw_sectors, lim->io_min >> SECTOR_SHIFT);
	} else {
		/* NVMe: 기본 BLK_DEF_MAX_SECTORS_CAP 적용. */
		lim->max_sectors = min(max_hw_sectors, BLK_DEF_MAX_SECTORS_CAP);
	}
	/* NVMe 최종 max_sectors도 LBA 단위로 정렬. */
	lim->max_sectors = round_down(lim->max_sectors,
			logical_block_sectors);

	/*
	 * Random default for the maximum number of segments.  Driver should not
	 * rely on this and set their own.
	 *
	 * NVMe: 드라이버가 PRP/SGL 최대 엔트리 수를 채우지 않으면 기본값을 사용.
	 * 실제로는 Identify에서 보고한 Maximum PRP Entry/Number of SGL
	 * Descriptors 등을 반영해야 한다.
	 */
	/* NVMe max_segments 미보고 시 BLK_MAX_SEGMENTS 기본값. */
	if (!lim->max_segments)
		lim->max_segments = BLK_MAX_SEGMENTS;

	/* NVMe Write Zeroes unmap 한도 불일치 시 드라이버 설정 오류. */
	if (lim->max_hw_wzeroes_unmap_sectors &&
	    lim->max_hw_wzeroes_unmap_sectors != lim->max_write_zeroes_sectors)
		return -EINVAL;
	lim->max_wzeroes_unmap_sectors = min(lim->max_hw_wzeroes_unmap_sectors,
			lim->max_user_wzeroes_unmap_sectors);

	/* NVMe Deallocate 최종 한도 = min(HW, 사용자). */
	lim->max_discard_sectors =
		min(lim->max_hw_discard_sectors, lim->max_user_discard_sectors);

	/*
	 * When discard is not supported, discard_granularity should be reported
	 * as 0 to userspace.
	 *
	 * NVMe: Deallocate(Trim/Discard)를 지원하지 않는 namespace에서는
	 * discard_granularity가 0으로 보고된다.
	 */
	/* NVMe Deallocate 지원 시 granularity는 physical_block_size 이상. */
	if (lim->max_discard_sectors)
		lim->discard_granularity =
			max(lim->discard_granularity, lim->physical_block_size);
	else
		lim->discard_granularity = 0;

	/* NVMe DSM/Deallocate scatter-gather는 최소 1개 segment. */
	if (!lim->max_discard_segments)
		lim->max_discard_segments = 1;

	/*
	 * By default there is no limit on the segment boundary alignment,
	 * but if there is one it can't be smaller than the page size as
	 * that would break all the normal I/O patterns.
	 */
	/* NVMe PRP/SGL seg_boundary_mask 미보고 시 기본값. */
	if (!lim->seg_boundary_mask)
		lim->seg_boundary_mask = BLK_SEG_BOUNDARY_MASK;
	/* NVMe segment boundary는 최소 PAGE_SIZE-1 이상이어야 함. */
	if (WARN_ON_ONCE(lim->seg_boundary_mask < BLK_MIN_SEGMENT_SIZE - 1))
		return -EINVAL;

	/*
	 * Stacking device may have both virtual boundary and max segment
	 * size limit, so allow this setting now, and long-term the two
	 * might need to move out of stacking limits since we have immutable
	 * bvec and lower layer bio splitting is supposed to handle the two
	 * correctly.
	 *
	 * NVMe: max_segment_size는 단일 PRP/SGL 엔트리가 가리킬 수 있는 최대
	 * 바이트. 4KB PRP entry 기준으로는 보통 4KB 또는 64KB 제한이 적용된다.
	 */
	if (lim->virt_boundary_mask) {
		/* virtual boundary가 있으면 max_segment_size는 UINT_MAX로 둔다. */
		if (!lim->max_segment_size)
			lim->max_segment_size = UINT_MAX;
	} else {
		/*
		 * The maximum segment size has an odd historic 64k default that
		 * drivers probably should override.  Just like the I/O size we
		 * require drivers to at least handle a full page per segment.
		 */
		/* NVMe max_segment_size 미보고 시 64KB 기본값. */
		if (!lim->max_segment_size)
			lim->max_segment_size = BLK_MAX_SEGMENT_SIZE;
		/* NVMe 단일 PRP/SGL 엔트리는 최소 PAGE_SIZE 이상 처리해야 함. */
		if (WARN_ON_ONCE(lim->max_segment_size < BLK_MIN_SEGMENT_SIZE))
			return -EINVAL;
	}

	/* setup max segment size for building new segment in fast path */
	/* NVMe fast-path segment 크기 = min(max_segment_size, seg_boundary+1, PAGE). */
	if (lim->seg_boundary_mask > lim->max_segment_size - 1)
		seg_size = lim->max_segment_size;
	else
		seg_size = lim->seg_boundary_mask + 1;
	lim->max_fast_segment_size = min_t(unsigned int, seg_size, PAGE_SIZE);

	/*
	 * We require drivers to at least do logical block aligned I/O, but
	 * historically could not check for that due to the separate calls
	 * to set the limits.  Once the transition is finished the check
	 * below should be narrowed down to check the logical block size.
	 *
	 * NVMe: DMA 엔진은 LBA 경계에 맞춘 시작 주소를 요구하므로, 최소 512B
	 * 정렬을 보장한다.
	 */
	/* NVMe dma_alignment 미보고 시 512B-1 정렬. */
	if (!lim->dma_alignment)
		lim->dma_alignment = SECTOR_SIZE - 1;
	/* NVMe DMA alignment는 PAGE_SIZE를 초과할 수 없음. */
	if (WARN_ON_ONCE(lim->dma_alignment > PAGE_SIZE))
		return -EINVAL;

	/* NVMe 파티션/스택 alignment_offset 마스크 처리. */
	if (lim->alignment_offset) {
		lim->alignment_offset &= (lim->physical_block_size - 1);
		lim->flags &= ~BLK_FLAG_MISALIGNED;
	}

	/*
	 * NVMe: Volatile Write Cache(VWC)를 지원하지 않는 컨트롤러에서는 FUA bit를
	 * 강제로 제거하여 상위 계층이 불필요한 FUA 명령을 복잡하게 제출하지
	 * 않도록 한다.
	 */
	/* NVMe VWC 미지원 시 BLK_FEAT_FUA 클리어. */
	if (!(lim->features & BLK_FEAT_WRITE_CACHE))
		lim->features &= ~BLK_FEAT_FUA;

	blk_validate_atomic_write_limits(lim);

	err = blk_validate_integrity_limits(lim);
	if (err)
		return err;
	return blk_validate_zoned_limits(lim);
}
EXPORT_SYMBOL_GPL(blk_validate_limits);

/**
 * blk_set_default_limits - 새로 할당된 request_queue의 기본 한도 설정
 * @lim: 초기화할 queue_limits
 *
 * NVMe 연결 지점:
 *   NVMe 드라이버가 blk_mq_init_queue() 등으로 큐를 생성할 때 호출되며,
 *   사용자가 덮어쓸 수 있는 discard/write-zeroes 한도를 UINT_MAX로 초기화한
 *   뒤 blk_validate_limits()를 통해 NVMe 하드웨어 제약을 정규화한다.
 */
int blk_set_default_limits(struct queue_limits *lim)
{
	/*
	 * Most defaults are set by capping the bounds in blk_validate_limits,
	 * but these limits are special and need an explicit initialization to
	 * the max value here.
	 */
	/* NVMe Deallocate 사용자 한도를 최대로 두고 HW 값과 min 병합 준비. */
	lim->max_user_discard_sectors = UINT_MAX;
	/* NVMe Write Zeroes unmap 사용자 한도를 최대로 두고 HW 값과 min 준비. */
	lim->max_user_wzeroes_unmap_sectors = UINT_MAX;
	return blk_validate_limits(lim);
}

/**
 * queue_limits_commit_update - queue_limits 원자적 갱신 커밋
 * @q: 갱신할 request_queue
 * @lim: 적용할 queue_limits
 *
 * NVMe 연결 지점:
 *   NVMe 드라이버가 reset 후 Identify/Controller 정보를 다시 읽어 q->limits를
 *   갱신할 때 사용된다. limits_lock 아래에서 blk_validate_limits()로 검증한
 *   뒤 q->limits에 복사하고, disk->bdi에도 optimal I/O 크기를 전파한다.
 *   (주의) 호출자는 큐를 freeze하거나 outstanding I/O가 없음을 보장해야 한다.
 */
int queue_limits_commit_update(struct request_queue *q,
		struct queue_limits *lim)
{
	int error;

	/* NVMe limits 갱신은 limits_lock 보호 아래 수행되어야 함. */
	lockdep_assert_held(&q->limits_lock);

	/* NVMe Identify/Controller 값을 정규화; 실패 시 unlock 후 반환. */
	error = blk_validate_limits(lim);
	if (error)
		goto out_unlock;

#ifdef CONFIG_BLK_INLINE_ENCRYPTION
	/* NVMe inline encryption과 DIF/DIX는 동시에 활성화할 수 없음(호환성). */
	if (q->crypto_profile && lim->integrity.tag_size) {
		pr_warn("blk-integrity: Integrity and hardware inline encryption are not supported together.\n");
		error = -EINVAL;
		goto out_unlock;
	}
#endif

	/* q->limits에 NVMe 컨트롤러 capability를 원자적으로 커밋. */
	q->limits = *lim;
	/* disk->bdi에 NVMe optimal I/O 크기를 전파하여 read-ahead에 반영. */
	if (q->disk)
		blk_apply_bdi_limits(q->disk->bdi, lim);
out_unlock:
	mutex_unlock(&q->limits_lock);
	return error;
}
EXPORT_SYMBOL_GPL(queue_limits_commit_update);

/**
 * queue_limits_commit_update_frozen - 큐를 freeze한 상태에서 limits 갱신
 * @q: 갱신할 request_queue
 * @lim: 적용할 queue_limits
 *
 * NVMe 연결 지점:
 *   NVMe reset/work에서 queue_limits_start_update()로 복사본을 얻은 뒤
 *   Identify 값으로 채우고, 본 함수를 통해 I/O를 멈춘 상태에서 안전하게
 *   커밋한다. blk_mq_freeze_queue/unfreeze_queue로 SQ/CQ에 새 request가
 *   들어가지 않음을 보장한다.
 */
int queue_limits_commit_update_frozen(struct request_queue *q,
		struct queue_limits *lim)
{
	unsigned int memflags;
	int ret;

	/* NVMe reset 중 SQ/CQ에 새 request 진입 차단(freeze). */
	memflags = blk_mq_freeze_queue(q);
	/* freeze 상태에서 q->limits 커밋. */
	ret = queue_limits_commit_update(q, lim);
	/* NVMe I/O 재개: 새 limits 하에서 SQ/CQ에 request 투입 가능. */
	blk_mq_unfreeze_queue(q, memflags);

	return ret;
}
EXPORT_SYMBOL_GPL(queue_limits_commit_update_frozen);

/**
 * queue_limits_set - 새로 초기화된 queue_limits를 큐에 적용
 * @q: 갱신할 request_queue
 * @lim: 적용할 queue_limits
 *
 * NVMe 연결 지점:
 *   NVMe PCI/FC/TCP transport에서 request_queue를 생성할 때 처음으로 호출.
 *   blk_mq_init_queue -> queue_limits_set -> blk_validate_limits 경로를 통해
 *   NVMe 컨트롤러 capability를 block layer에 등록한다.
 */
int queue_limits_set(struct request_queue *q, struct queue_limits *lim)
{
	mutex_lock(&q->limits_lock);
	return queue_limits_commit_update(q, lim);
}
EXPORT_SYMBOL_GPL(queue_limits_set);

/**
 * queue_limit_alignment_offset - 파티션/스택 시작 섹터의 정렬 오프셋 계산
 * @lim: queue_limits
 * @sector: 대상 섹터
 *
 * NVMe 연결 지점:
 *   NVMe namespace 위의 파티션이나 MD/DM 스택 장치에서 물리 블록 경계와
 *   데이터 시작 위치가 맞지 않을 때 alignment_offset을 계산한다. 이 값은
 *   상위 bio의 LBA 정렬 검사에 사용된다.
 */
static int queue_limit_alignment_offset(const struct queue_limits *lim,
		sector_t sector)
{
	/* NVMe 정렬 검사 기준 = max(physical_block_size, io_min). */
	unsigned int granularity = max(lim->physical_block_size, lim->io_min);
	/* NVMe sector에서 granularity 단위 remainder를 바이트로 변환. */
	unsigned int alignment = sector_div(sector, granularity >> SECTOR_SHIFT)
		<< SECTOR_SHIFT;

	return (granularity + lim->alignment_offset - alignment) % granularity;
}

/**
 * queue_limit_discard_alignment - discard 정렬 오프셋 계산
 * @lim: queue_limits
 * @sector: 대상 섹터
 *
 * NVMe 연결 지점:
 *   NVMe Deallocate(Trim) 명령은 discard_granularity 단위로 정렬되어야
 *   효율적으로 동작한다. 파티션 시작 섹터를 고려하여 discard alignment를
 *   보정한다.
 */
static unsigned int queue_limit_discard_alignment(
		const struct queue_limits *lim, sector_t sector)
{
	unsigned int alignment, granularity, offset;

	/* NVMe Deallocate 미지원 시 alignment 0 반환. */
	if (!lim->max_discard_sectors)
		return 0;

	/* Why are these in bytes, not sectors? */
	/* NVMe discard_alignment를 섹터 단위로 변환. */
	alignment = lim->discard_alignment >> SECTOR_SHIFT;
	/* NVMe discard_granularity를 섹터 단위로 변환. */
	granularity = lim->discard_granularity >> SECTOR_SHIFT;

	/* Offset of the partition start in 'granularity' sectors */
	/* NVMe 파티션 시작 섹터의 granularity 내 offset. */
	offset = sector_div(sector, granularity);

	/* And why do we do this modulus *again* in blkdev_issue_discard()? */
	/* NVMe Deallocate 시작 위치를 granularity 경계로 보정. */
	offset = (granularity + alignment - offset) % granularity;

	/* Turn it back into bytes, gaah */
	return offset << SECTOR_SHIFT;
}

static unsigned int blk_round_down_sectors(unsigned int sectors, unsigned int lbs)
{
	/* NVMe I/O 크기를 LBA 단위로 내림. */
	sectors = round_down(sectors, lbs >> SECTOR_SHIFT);
	/* NVMe: 최소 PAGE_SIZE 이상 보장. */
	if (sectors < PAGE_SIZE >> SECTOR_SHIFT)
		sectors = PAGE_SIZE >> SECTOR_SHIFT;
	return sectors;
}

/* Check if second and later bottom devices are compliant */
static bool blk_stack_atomic_writes_tail(struct queue_limits *t,
				struct queue_limits *b)
{
	/* We're not going to support different boundary sizes.. yet */
	/* NVMe atomic boundary가 다른 멀티 장치 스택은 아직 미지원. */
	if (t->atomic_write_hw_boundary != b->atomic_write_hw_boundary)
		return false;

	/* Can't support this */
	/* NVMe t의 최소 단위가 b의 최대 단위보다 크면 호환 불가. */
	if (t->atomic_write_hw_unit_min > b->atomic_write_unit_max)
		return false;

	/* Or this */
	/* NVMe t의 최대 단위가 b의 최소 단위보다 작으면 호환 불가. */
	if (t->atomic_write_hw_unit_max < b->atomic_write_unit_min)
		return false;

	/* NVMe 멀티 장치 스택 시 FAW/unit는 교차 병합. */
	t->atomic_write_hw_max = min(t->atomic_write_hw_max,
				b->atomic_write_hw_max);
	t->atomic_write_hw_unit_min = max(t->atomic_write_hw_unit_min,
				b->atomic_write_hw_unit_min);
	t->atomic_write_hw_unit_max = min(t->atomic_write_hw_unit_max,
				b->atomic_write_unit_max);
	return true;
}

static void blk_stack_atomic_writes_chunk_sectors(struct queue_limits *t)
{
	unsigned int chunk_bytes;

	/* chunk_sectors가 0이면 atomic write와의 정렬 검사 불필요. */
	if (!t->chunk_sectors)
		return;

	/*
	 * If chunk sectors is so large that its value in bytes overflows
	 * UINT_MAX, then just shift it down so it definitely will fit.
	 * We don't support atomic writes of such a large size anyway.
	 */
	/* NVMe: chunk_sectors*SECTOR_SHIFT overflow 방지. */
	if (check_shl_overflow(t->chunk_sectors, SECTOR_SHIFT, &chunk_bytes))
		chunk_bytes = t->chunk_sectors;

	/*
	 * Find values for limits which work for chunk size.
	 * b->atomic_write_hw_unit_{min, max} may not be aligned with chunk
	 * size, as the chunk size is not restricted to a power-of-2.
	 * So we need to find highest power-of-2 which works for the chunk
	 * size.
	 * As an example scenario, we could have t->unit_max = 16K and
	 * t->chunk_sectors = 24KB. For this case, reduce t->unit_max to a
	 * value aligned with both limits, i.e. 8K in this example.
	 */
	/* NVMe atomic write unit_max를 chunk_bytes의 최대 2의 거듭제곱 인자로 제한. */
	t->atomic_write_hw_unit_max = min(t->atomic_write_hw_unit_max,
					max_pow_of_two_factor(chunk_bytes));

	/* NVMe unit_min은 unit_max를 초과할 수 없도록 보정. */
	t->atomic_write_hw_unit_min = min(t->atomic_write_hw_unit_min,
					  t->atomic_write_hw_unit_max);
	/* NVMe FAW는 chunk_bytes 이하로 제한. */
	t->atomic_write_hw_max = min(t->atomic_write_hw_max, chunk_bytes);
}

/* Check stacking of first bottom device */
static bool blk_stack_atomic_writes_head(struct queue_limits *t,
				struct queue_limits *b)
{
	/* NVMe atomic boundary가 chunk_sectors와 정렬되어야 첫 장치 채택 가능. */
	if (!blk_valid_atomic_writes_boundary(t->chunk_sectors,
			b->atomic_write_hw_boundary >> SECTOR_SHIFT))
		return false;

	/* NVMe 첫 하위 장치의 atomic write 단위를 상위로 상속. */
	t->atomic_write_hw_unit_max = b->atomic_write_hw_unit_max;
	t->atomic_write_hw_unit_min = b->atomic_write_hw_unit_min;
	t->atomic_write_hw_max = b->atomic_write_hw_max;
	t->atomic_write_hw_boundary = b->atomic_write_hw_boundary;
	return true;
}

/**
 * blk_stack_atomic_writes_limits - 스택형 장치의 NVMe 원자적 쓰기 한도 병합
 * @t: 상위 queue_limits
 * @b: 하위 queue_limits
 * @start: 하위 장치 내 시작 섹터
 *
 * NVMe 연결 지점:
 *   MD/DM 등이 여러 NVMe 장치를 묶을 때 각 장치의 atomic write 단위가
 *   호환되는지 검사하고, 호환되면 상위 장치로 병합한다. 호환되지 않으면
 *   상위 장치는 atomic write를 노출하지 않는다.
 */
static void blk_stack_atomic_writes_limits(struct queue_limits *t,
				struct queue_limits *b, sector_t start)
{
	/* NVMe 하위 장치가 atomic write를 지원하지 않으면 상위도 미지원. */
	if (!(b->features & BLK_FEAT_ATOMIC_WRITES))
		goto unsupported;

	/* NVMe atomic_write_hw_unit_min이 0이면 실제 지원 아님. */
	if (!b->atomic_write_hw_unit_min)
		goto unsupported;

	/* NVMe start 섹터가 atomic write 단위로 정렬되어야 함. */
	if (!blk_atomic_write_start_sect_aligned(start, b))
		goto unsupported;

	/* UINT_MAX indicates no stacking of bottom devices yet */
	/* NVMe 첫 하위 장치 병합 시 head 함수 사용. */
	if (t->atomic_write_hw_max == UINT_MAX) {
		if (!blk_stack_atomic_writes_head(t, b))
			goto unsupported;
	} else {
		/* NVMe 두 번째 이후 하위 장치 병합 시 tail 함수 사용. */
		if (!blk_stack_atomic_writes_tail(t, b))
			goto unsupported;
	}
	blk_stack_atomic_writes_chunk_sectors(t);
	return;

unsupported:
	/* NVMe atomic write 호환 실패 시 상위 장치에서 0으로 표시. */
	t->atomic_write_hw_max = 0;
	t->atomic_write_hw_unit_max = 0;
	t->atomic_write_hw_unit_min = 0;
	t->atomic_write_hw_boundary = 0;
}

/**
 * blk_stack_limits - MD/DM 등 스택형 장치의 queue_limits 병합
 * @t: 상위 장치 queue_limits
 * @b: 하위 구성 장치 queue_limits
 * @start: 하위 장치 내 첫 데이터 섹터
 *
 * 목적:
 *   NVMe SSD 위에 software RAID, device mapper, LUKS 등 스택형 장치를 얹을 때
 *   각 구성 장치의 block size, alignment, max_sectors, discard, zoned,
 *   atomic_write 한도를 교차 검증하고 최소 공통 분모로 병합한다.
 *
 * 호출 경로:
 *   md_run -> queue_limits_stack_bdev -> blk_stack_limits
 *   (또는 dm-table-load 경로)
 *
 * NVMe 연결 지점:
 *   - 하위 NVMe q->limits를 복사하여 상위 가상 장치의 bio splitting/merge
 *     조건으로 재사용한다.
 *   - BLK_FEAT_POLL/NOWAIT 등 NVMe 특화 feature를 상위로 상속한다.
 *   - zoned NVMe(ZNS)의 zone_append_sectors, zone_write_granularity를 병합.
 */
int blk_stack_limits(struct queue_limits *t, struct queue_limits *b,
		     sector_t start)
{
	unsigned int top, bottom, alignment;
	int ret = 0;

	/* NVMe feature flags 중 상위로 상속 가능한 마스크 복사. */
	t->features |= (b->features & BLK_FEAT_INHERIT_MASK);

	/*
	 * Some feaures need to be supported both by the stacking driver and all
	 * underlying devices.  The stacking driver sets these flags before
	 * stacking the limits, and this will clear the flags if any of the
	 * underlying devices does not support it.
	 *
	 * NVMe: BLK_FEAT_NOWAIT는 polled completions 외에도 async poll 경로와
	 * 관련. BLK_FEAT_POLL은 nvme_poll_irqdisable 등 상위 poll 경로에 영향.
	 */
	/* NVMe NOWAIT 지원: 모든 하위 장치가 NOWAIT 지원해야 상속. */
	if (!(b->features & BLK_FEAT_NOWAIT))
		t->features &= ~BLK_FEAT_NOWAIT;
	/* NVMe POLL 지원: 모든 하위 장치가 POLL 지원해야 상속. */
	if (!(b->features & BLK_FEAT_POLL))
		t->features &= ~BLK_FEAT_POLL;

	/* NVMe misalignment flag는 하위 장치가 한 개라도 설정되면 상속. */
	t->flags |= (b->flags & BLK_FLAG_MISALIGNED);

	/*
	 * NVMe: 상위 장치의 I/O 크기는 하위 NVMe max_sectors를 초과할 수 없다.
	 * bio가 클 경우 blk_queue_split()에서 분할된다.
	 */
	/* NVMe 상위 max_sectors = min(상위, 하위). */
	t->max_sectors = min_not_zero(t->max_sectors, b->max_sectors);
	/* NVMe 상위 사용자 한도 = min(상위, 하위). */
	t->max_user_sectors = min_not_zero(t->max_user_sectors,
			b->max_user_sectors);
	/* NVMe 상위 MDTS = min(상위, 하위). */
	t->max_hw_sectors = min_not_zero(t->max_hw_sectors, b->max_hw_sectors);
	/* NVMe SCSI ULP max_dev_sectors 병합. */
	t->max_dev_sectors = min_not_zero(t->max_dev_sectors, b->max_dev_sectors);
	/* NVMe Write Zeroes 최대 섹터 병합. */
	t->max_write_zeroes_sectors = min(t->max_write_zeroes_sectors,
					b->max_write_zeroes_sectors);
	t->max_user_wzeroes_unmap_sectors =
			min(t->max_user_wzeroes_unmap_sectors,
			    b->max_user_wzeroes_unmap_sectors);
	t->max_hw_wzeroes_unmap_sectors =
			min(t->max_hw_wzeroes_unmap_sectors,
			    b->max_hw_wzeroes_unmap_sectors);

	/* NVMe ZNS Zone Append 최대 섹터 병합. */
	t->max_hw_zone_append_sectors = min(t->max_hw_zone_append_sectors,
					b->max_hw_zone_append_sectors);

	/*
	 * NVMe: PRP/SGL segment 경계는 상위/하위 중 더 작은 값을 따라야 한다.
	 * 그렇지 않으면 상위 bio를 분할할 때 NVMe DMA 엔진이 경계를 넘는
	 * descriptor list를 받게 될 수 있다.
	 */
	/* NVMe PRP/SGL seg_boundary_mask 병합: 더 작은 경계 선택. */
	t->seg_boundary_mask = min_not_zero(t->seg_boundary_mask,
					    b->seg_boundary_mask);
	/* NVMe virtual boundary 병합: 더 작은 경계 선택. */
	t->virt_boundary_mask = min_not_zero(t->virt_boundary_mask,
					    b->virt_boundary_mask);

	/* NVMe PRP/SGL 최대 segment 수 병합. */
	t->max_segments = min_not_zero(t->max_segments, b->max_segments);
	/* NVMe DSM/Deallocate 최대 segment 수 병합. */
	t->max_discard_segments = min_not_zero(t->max_discard_segments,
					       b->max_discard_segments);
	/* NVMe PI metadata segment 수 병합. */
	t->max_integrity_segments = min_not_zero(t->max_integrity_segments,
						 b->max_integrity_segments);

	/* NVMe 단일 PRP/SGL 엔트리 크기 병합. */
	t->max_segment_size = min_not_zero(t->max_segment_size,
					   b->max_segment_size);

	/* NVMe 하위 장치의 시작 섹터 기준 alignment offset 계산. */
	alignment = queue_limit_alignment_offset(b, start);

	/* Bottom device has different alignment.  Check that it is
	 * compatible with the current top alignment.
	 */
	/* NVMe 상위/하위 alignment가 다른 경우 호환성 검사. */
	if (t->alignment_offset != alignment) {

		/* NVMe 상위 장치의 정렬 구간(바이트). */
		top = max(t->physical_block_size, t->io_min)
			+ t->alignment_offset;
		/* NVMe 하위 장치의 정렬 구간(바이트). */
		bottom = max(b->physical_block_size, b->io_min) + alignment;

		/* Verify that top and bottom intervals line up */
		/* NVMe 상위/하위 정렬 구간이 서로 배수 관계가 아니면 misaligned. */
		if (max(top, bottom) % min(top, bottom)) {
			t->flags |= BLK_FLAG_MISALIGNED;
			ret = -1;
		}
	}

	/* NVMe logical_block_size는 상위/하위 중 큰 값으로 상속. */
	t->logical_block_size = max(t->logical_block_size,
				    b->logical_block_size);

	/* NVMe physical_block_size는 상위/하위 중 큰 값으로 상속. */
	t->physical_block_size = max(t->physical_block_size,
				     b->physical_block_size);

	/* NVMe io_min은 상위/하위 중 큰 값으로 상속. */
	t->io_min = max(t->io_min, b->io_min);
	/* NVMe io_opt는 LCM으로 병합하여 둘 다 만족. */
	t->io_opt = lcm_not_zero(t->io_opt, b->io_opt);
	/* NVMe DMA alignment는 상위/하위 중 큰 값으로 상속. */
	t->dma_alignment = max(t->dma_alignment, b->dma_alignment);

	/* Set non-power-of-2 compatible chunk_sectors boundary */
	/* NVMe chunk_sectors는 RAID stripe 등을 고려하여 GCD 병합. */
	if (b->chunk_sectors)
		t->chunk_sectors = gcd(t->chunk_sectors, b->chunk_sectors);

	/* Physical block size a multiple of the logical block size? */
	/* NVMe physical_block_size가 logical_block_size 배수가 아니면 보정. */
	if (t->physical_block_size & (t->logical_block_size - 1)) {
		t->physical_block_size = t->logical_block_size;
		t->flags |= BLK_FLAG_MISALIGNED;
		ret = -1;
	}

	/* Minimum I/O a multiple of the physical block size? */
	/* NVMe io_min이 physical_block_size 배수가 아니면 보정. */
	if (t->io_min & (t->physical_block_size - 1)) {
		t->io_min = t->physical_block_size;
		t->flags |= BLK_FLAG_MISALIGNED;
		ret = -1;
	}

	/* Optimal I/O a multiple of the physical block size? */
	/* NVMe io_opt가 physical_block_size 배수가 아니면 0으로 보정. */
	if (t->io_opt & (t->physical_block_size - 1)) {
		t->io_opt = 0;
		t->flags |= BLK_FLAG_MISALIGNED;
		ret = -1;
	}

	/* chunk_sectors a multiple of the physical block size? */
	/* NVMe chunk_sectors가 physical_block_size 배수가 아니면 0으로 보정. */
	if (t->chunk_sectors % (t->physical_block_size >> SECTOR_SHIFT)) {
		t->chunk_sectors = 0;
		t->flags |= BLK_FLAG_MISALIGNED;
		ret = -1;
	}

	/* Find lowest common alignment_offset */
	/* NVMe alignment_offset는 LCM으로 병합 후 granularity 내로 정규화. */
	t->alignment_offset = lcm_not_zero(t->alignment_offset, alignment)
		% max(t->physical_block_size, t->io_min);

	/* Verify that new alignment_offset is on a logical block boundary */
	/* NVMe alignment_offset가 logical_block_size 경계에 있지 않으면 misaligned. */
	if (t->alignment_offset & (t->logical_block_size - 1)) {
		t->flags |= BLK_FLAG_MISALIGNED;
		ret = -1;
	}

	/* NVMe I/O 크기 한도를 LBA 단위로 내림. */
	t->max_sectors = blk_round_down_sectors(t->max_sectors, t->logical_block_size);
	t->max_hw_sectors = blk_round_down_sectors(t->max_hw_sectors, t->logical_block_size);
	t->max_dev_sectors = blk_round_down_sectors(t->max_dev_sectors, t->logical_block_size);

	/* Discard alignment and granularity */
	/* NVMe Deallocate 지원 하위 장치가 있을 때 discard 한도 병합. */
	if (b->discard_granularity) {
		alignment = queue_limit_discard_alignment(b, start);

		/* NVMe 상위 max_discard_sectors = min(상위, 하위). */
		t->max_discard_sectors = min_not_zero(t->max_discard_sectors,
						      b->max_discard_sectors);
		t->max_hw_discard_sectors = min_not_zero(t->max_hw_discard_sectors,
							 b->max_hw_discard_sectors);
		/* NVMe discard_granularity는 더 큰 값으로 상속(보수적). */
		t->discard_granularity = max(t->discard_granularity,
					     b->discard_granularity);
		/* NVMe discard_alignment는 LCM 병합 후 granularity 내 정규화. */
		t->discard_alignment = lcm_not_zero(t->discard_alignment, alignment) %
			t->discard_granularity;
	}
	/* NVMe secure erase 최대 섹터 병합. */
	t->max_secure_erase_sectors = min_not_zero(t->max_secure_erase_sectors,
						   b->max_secure_erase_sectors);
	/* NVMe ZNS zone_write_granularity는 더 큰 값으로 상속. */
	t->zone_write_granularity = max(t->zone_write_granularity,
					b->zone_write_granularity);
	/* NVMe ZNS가 아닌 장치에서는 ZNS 관련 필드 클리어. */
	if (!(t->features & BLK_FEAT_ZONED)) {
		t->zone_write_granularity = 0;
		t->max_zone_append_sectors = 0;
	}
	blk_stack_atomic_writes_limits(t, b, start);

	return ret;
}
EXPORT_SYMBOL(blk_stack_limits);

/**
 * queue_limits_stack_bdev - block device 기반 queue_limits 스택 병합
 * @t: 상위 queue_limits
 * @bdev: 하위 block device
 * @offset: 하위 장치 내 데이터 시작 오프셋
 * @pfx: 경고 메시지 접두사
 *
 * NVMe 연결 지점:
 *   bdev_limits(bdev)로 하위 NVMe 장치의 q->limits를 가져와
 *   blk_stack_limits()로 병합한다. 파티션의 bd_start_sect를 더해 파티션
 *   시작 위치를 반영한다.
 */
void queue_limits_stack_bdev(struct queue_limits *t, struct block_device *bdev,
		sector_t offset, const char *pfx)
{
	/* NVMe 파티션 시작 섹터를 offset에 더해 alignment 검사. */
	if (blk_stack_limits(t, bdev_limits(bdev),
			get_start_sect(bdev) + offset))
		pr_notice("%s: Warning: Device %pg is misaligned\n",
			pfx, bdev);
}
EXPORT_SYMBOL_GPL(queue_limits_stack_bdev);

/**
 * queue_limits_stack_integrity - 스택형 장치의 integrity profile 병합
 * @t: target queue limits
 * @b: base queue limits
 *
 * NVMe 연결 지점:
 *   NVMe DIF/DIX PI 설정이 하위 장치에서 상위 스택 장치로 상속될 수 있는지
 *   검사한다. 메타데이터 크기, interval, tag size, checksum 종류 등이
 *   일치해야 상위 장치도 PI를 노출할 수 있다.
 */
bool queue_limits_stack_integrity(struct queue_limits *t,
		struct queue_limits *b)
{
	struct blk_integrity *ti = &t->integrity;
	struct blk_integrity *bi = &b->integrity;

	if (!IS_ENABLED(CONFIG_BLK_DEV_INTEGRITY))
		return true;

	/* NVMe PI가 이미 스택된 상태면 하위 장치와 모든 필드 일치 필요. */
	if (ti->flags & BLK_INTEGRITY_STACKED) {
		/* NVMe DIF/DIX metadata_size 불일치 시 상속 불가. */
		if (ti->metadata_size != bi->metadata_size)
			goto incompatible;
		/* NVMe PI interval_exp 불일치 시 상속 불가. */
		if (ti->interval_exp != bi->interval_exp)
			goto incompatible;
		/* NVMe PI tag_size 불일치 시 상속 불가. */
		if (ti->tag_size != bi->tag_size)
			goto incompatible;
		/* NVMe PI checksum type 불일치 시 상속 불가. */
		if (ti->csum_type != bi->csum_type)
			goto incompatible;
		/* NVMe PI tuple size 불일치 시 상속 불가. */
		if (ti->pi_tuple_size != bi->pi_tuple_size)
			goto incompatible;
		/* NVMe REF_TAG flag 불일치 시 상속 불가. */
		if ((ti->flags & BLK_INTEGRITY_REF_TAG) !=
		    (bi->flags & BLK_INTEGRITY_REF_TAG))
			goto incompatible;
		/* NVMe SPLIT_INTERVAL_CAPABLE는 하위 장치가 지원 안 하면 클리어. */
		if ((ti->flags & BLK_SPLIT_INTERVAL_CAPABLE) &&
		    !(bi->flags & BLK_SPLIT_INTERVAL_CAPABLE))
			ti->flags &= ~BLK_SPLIT_INTERVAL_CAPABLE;
	} else {
		/* NVMe PI profile을 상위 장치로 처음 복사. */
		ti->flags = BLK_INTEGRITY_STACKED;
		ti->flags |= (bi->flags & BLK_INTEGRITY_DEVICE_CAPABLE) |
			     (bi->flags & BLK_INTEGRITY_REF_TAG) |
			     (bi->flags & BLK_SPLIT_INTERVAL_CAPABLE);
		ti->csum_type = bi->csum_type;
		ti->pi_tuple_size = bi->pi_tuple_size;
		ti->metadata_size = bi->metadata_size;
		ti->pi_offset = bi->pi_offset;
		ti->interval_exp = bi->interval_exp;
		ti->tag_size = bi->tag_size;
	}
	return true;

incompatible:
	/* NVMe PI 호환 실패 시 상위 장치는 PI 미지원으로 초기화. */
	memset(ti, 0, sizeof(*ti));
	return false;
}
EXPORT_SYMBOL_GPL(queue_limits_stack_integrity);

/**
 * blk_set_queue_depth - NVMe queue depth 등록
 * @q: 등록할 request_queue
 * @depth: IO SQ에서 동시 진행 가능한 request 수
 *
 * 목적:
 *   NVMe 컨트롤러의 Create IO Queue pair에서 설정한 Queue Size를 block layer에
 *   알려, inflight request 수와 tag 할당 범위를 맞춘다.
 *
 * 호출 경로:
 *   nvme_alloc_io_queues -> nvme_setup_io_queues -> blk_mq_tag_set_depth ->
 *   blk_set_queue_depth
 *
 * NVMe 연결 지점:
 *   - q->queue_depth는 tag_set->queue_depth와 연동되어 SQ tail doorbell write
 *     횟수 및 CID 재사용에 영향을 준다.
 *   - rq_qos_queue_depth_changed()로 QoS/scheduler에도 전파된다.
 */
void blk_set_queue_depth(struct request_queue *q, unsigned int depth)
{
	/* NVMe IO SQ/CQ pair의 Queue Size를 block layer queue_depth에 반영.
	 * 이 값은 blk-mq tag 범위와 CID 할당 상한을 결정한다. */
	q->queue_depth = depth;
	/* NVMe queue depth 변경을 QoS/scheduler에 전파하여 inflight 제한 갱신. */
	rq_qos_queue_depth_changed(q);
}
EXPORT_SYMBOL(blk_set_queue_depth);

/**
 * bdev_alignment_offset - block device의 시작 위치 정렬 오프셋 반환
 * @bdev: 대상 block device
 *
 * NVMe 연결 지점:
 *   NVMe namespace 위의 파티션이나 스택 장치가 물리 블록 경계에 맞지 않게
 *   시작할 때, 상위 bio 경로에서 사용할 alignment_offset을 반환한다.
 *   BLK_FLAG_MISALIGNED가 설정되면 -1을 반환하여 상위에서 처리를 강제한다.
 */
int bdev_alignment_offset(struct block_device *bdev)
{
	struct request_queue *q = bdev_get_queue(bdev);

	/* NVMe misalignment 발생 시 상위에서 강제 처리를 위해 -1 반환. */
	if (q->limits.flags & BLK_FLAG_MISALIGNED)
		return -1;
	/* NVMe 파티션의 경우 bd_start_sect를 고려한 offset 계산. */
	if (bdev_is_partition(bdev))
		return queue_limit_alignment_offset(&q->limits,
				bdev->bd_start_sect);
	return q->limits.alignment_offset;
}
EXPORT_SYMBOL_GPL(bdev_alignment_offset);

/**
 * bdev_discard_alignment - block device의 discard 정렬 오프셋 반환
 * @bdev: 대상 block device
 *
 * NVMe 연결 지점:
 *   NVMe Deallocate(Trim) 명령을 발행할 때 discard_granularity 경계에 맞추기
 *   위해 파티션 시작 섹터를 고려한 discard_alignment를 반환한다.
 */
unsigned int bdev_discard_alignment(struct block_device *bdev)
{
	struct request_queue *q = bdev_get_queue(bdev);

	/* NVMe 파티션의 경우 bd_start_sect를 고려한 discard alignment 계산. */
	if (bdev_is_partition(bdev))
		return queue_limit_discard_alignment(&q->limits,
				bdev->bd_start_sect);
	return q->limits.discard_alignment;
}
EXPORT_SYMBOL_GPL(bdev_discard_alignment);

/* NVMe 관점 핵심 요약
 *
 * - 본 파일은 NVMe 컨트롤러가 보고한 MDTS, LBA format, PRP/SGL segment 한도,
 *   feature flags를 block layer 표준 queue_limits로 정규화한다.
 * - 정규화된 한도는 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로에서 request 조립,
 *   bio splitting, tag/CID 할당에 직접 사용된다.
 * - NVMe SSD 위에 MD/DM 등 스택형 장치가 있을 때 본 파일의 stacking 함수가
 *   하위 NVMe q->limits를 상위 가상 장치로 병합하여 하드웨어 특성이 올바르게
 *   상속되도록 한다.
 * - queue_depth 설정은 NVMe IO SQ 크기와 blk-mq tag 범위를 일치시켜 CID
 *   고갈 및 doorbell overflow를 방지한다.
 * - integrity/atomic_write/zoned 검증은 NVMe DIF/DIX, FUA, ZNS 등 확장
 *   기능을 block layer에서 안전하게 노출하기 위한 게이트키퍼 역할을 한다.
 */
