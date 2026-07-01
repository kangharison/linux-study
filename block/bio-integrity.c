// SPDX-License-Identifier: GPL-2.0
/*
 * bio-integrity.c - bio data integrity extensions
 *
 * Copyright (C) 2007, 2008, 2009 Oracle Corporation
 * Written by: Martin K. Petersen <martin.petersen@oracle.com>
 *
 * ============================================================================
 * NVMe SSD 관점 파일 요약
 * ============================================================================
 * 이 파일은 block layer의 bio 단위로 end-to-end 데이터 보호(Integrity) 메타데이터를
 * 관리한다. 파일시스템 -> submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 * -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로에서, NVMe SSD로 날아가는
 * PRP/SGL과 함께 T10 DIF/DIX 형태의 Guard/App Tag/Ref Tag를 매핑하고 검증 플래그를
 * 설정하는 역할을 담당한다. bio-integrity는 NVMe namespace의 Format(NVM Format)에
 * 의해 정의된 메타데이터 크기/위치와 연결되며, NVMe 명령어의 Protection Information
 * 필드와 직접적으로 대응한다.
 * ============================================================================
 */

#include <linux/blk-integrity.h>
#include <linux/t10-pi.h>
#include "blk.h"

/*
 * bio_integrity_alloc: bio_integrity_payload(bip)와 가변 길이 bvecs를 한 번에
 * 할당하기 위한 내장 구조체. NVMe에서는 이 bvec들이 데이터 PRP/SGL과 별도로
 * Controller Memory Buffer(CMB) 또는 PCIe 메모리로 전송될 수 있는 integrity
 * 메타데이터 SGL을 구성한다.
 */
struct bio_integrity_alloc {
	struct bio_integrity_payload	bip;	/* NVMe 관점: Guard/RefTag/AppTag
						 * 상태가 들어 있는 payload
						 */
	struct bio_vec			bvecs[];	/* 메타데이터를 위한 bvec
						 * 배열, NVMe SGL/PRP 조각과
						 * 대응 (추정)
						 */
};

static mempool_t integrity_buf_pool;	/* GFP_NOIO 실패 시 fallback용 mempool */

/*
 * bi_offload_capable: 컨트롤러가 PI 생성/검증을 offload할 수 있는지 판단.
 * NVMe End-to-end Data Protection: metadata 크기가 PI tuple 크기와 정확히
 * 일치하면 NVMe 컨트롤러 낸에서 Guard 생성/검증이 가능하다.
 */
static bool bi_offload_capable(struct blk_integrity *bi)
{
	/* metadata_size == pi_tuple_size 이면 NVMe PRCHK/PRACT offload 가능 (추정) */
	return bi->metadata_size == bi->pi_tuple_size;
}

/*
 * __bio_integrity_action - 현재 bio에 대해 integrity 메타데이터를 어떻게 처리할지
 * 결정한다.
 *
 * 목적: 파일시스템이 생성한 bio의 REQ_OP_READ/WRITE에 따라, 소프트웨어에서
 * Guard/RefTag/AppTag를 생성/검증해야 하는지, 아니면 NVMe 컨트롤러 offload에
 * 맡길지를 판정한다.
 *
 * NVMe 연결:
 *   - NVMe namespace format에서 메타데이터가 활성화된 경우, host가 생성한 PI는
 *     nvme_setup_cmd()에서 PRP/SGL 메타데이터 버퍼로 연결된다.
 *   - NVMe 컨트롤러가 offload 가능하면 소프트웨어 검증/생성을 생략하고
 *     controller 낸 PRACT=0/PRACT=1 처리를 활용할 수 있다 (추정).
 *
 * 호출 경로:
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *   -> nvme_queue_rq -> nvme_setup_cmd -> __bio_integrity_action
 */
unsigned int __bio_integrity_action(struct bio *bio)
{
	/* bio가 속한 disk의 integrity profile 획득: NVMe namespace format에서 유래 */
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);

	/*
	 * 암호화와 integrity를 동시에 처리하는 것은 현재 지원하지 않는다.
	 * NVMe: NVMe 2.0 이후 cryptography/PI 조합에 대한 주의가 필요 (추정).
	 */
	if (WARN_ON_ONCE(bio_has_crypt_ctx(bio)))
		return 0;

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		/*
		 * NVMe READ 명령: controller가 검증을 하지 않도록 설정된
		 * 경우(BLK_INTEGRITY_NOVERIFY)에는 software 검증을 생략하거나,
		 * offload 불가 시에만 host에서 검증한다.
		 */
		if (bi->flags & BLK_INTEGRITY_NOVERIFY) {
			/* NOVERIFY + offload 가능: NVMe 컨트롤러가 PI 검증을 대신 수행 */
			if (bi_offload_capable(bi))
				return 0;	/* NVMe 컨트롤러가 PI 검증 offload */
			/* host 측 buffer만 준비, software 검증은 생략 */
			return BI_ACT_BUFFER;
		}
		/* host 측에서 Guard/RefTag 검증 수행 -> NVMe PRCHK offload 안 함 */
		return BI_ACT_BUFFER | BI_ACT_CHECK;
	case REQ_OP_WRITE:
		/*
		 * Flush masquerading as write?
		 */
		/* bio_sectors가 0이면 실제 데이터가 없으므로 integrity 메타데이터도
		 * 필요 없다. NVMe FLUSH는 데이터/메타데이터가 아닌 휘발성 쓰기 버퍼를
		 * durable하게 만든다.
		 */
		if (!bio_sectors(bio))
			return 0;

		/*
		 * Zero the memory allocated to not leak uninitialized kernel
		 * memory to disk for non-integrity metadata where nothing else
		 * initializes the memory.
		 */
		/* 초기화되지 않은 커널 메모리가 디스크로 유출되지 않도록, host가
		 * 생성하지 않는 메타데이터 영역은 0으로 채운다. NVMe WRITE에서
		 * metadata buffer가 0이 아닌 쓰레기값이면 SSD 측 Guard 검증이
		 * 실패할 수 있다 (추정).
		 */
		if (bi->flags & BLK_INTEGRITY_NOGENERATE) {
			/* NOGENERATE + offload: NVMe 컨트롤러가 Guard/AppTag/RefTag 생성 */
			if (bi_offload_capable(bi))
				return 0;	/* NVMe 컨트롤러가 PI 생성 offload */
			/* host는 buffer만 0으로 채우고 controller가 PI 생성한다 (추정) */
			return BI_ACT_BUFFER | BI_ACT_ZERO;
		}

		/*
		 * metadata_size > pi_tuple_size: NVMe namespace format에서
		 * 메타데이터 일부만 PI이고 나머지는 사용자/제조사 전용 데이터일
		 * 가능성이 있다 (추정). 이때는 체크섬 생성 구간을 명시적으로
		 * 제외하고 나머지는 0으로 초기화해야 한다.
		 */
		if (bi->metadata_size > bi->pi_tuple_size)
			return BI_ACT_BUFFER | BI_ACT_CHECK | BI_ACT_ZERO;
		/* 일반 WRITE: host가 PI 생성 후 NVMe metadata SGL/PRP로 전송 */
		return BI_ACT_BUFFER | BI_ACT_CHECK;
	default:
		/* READ/WRITE 외 연산은 NVMe 명령에 metadata가 필요 없음 */
		return 0;
	}
}
EXPORT_SYMBOL_GPL(__bio_integrity_action);

/*
 * bio_integrity_alloc_buf - integrity 메타데이터를 담을 낸 버퍼를 할당한다.
 *
 * 목적: bio_sectors에 해당하는 metadata 바이트 수만큼 커널 메모리를 할당하고
 * bip_vec[0]에 연결한다.
 *
 * NVMe 연결:
 *   - NVMe WRITE/READ의 metadata SGL/PRP는 데이터 버퍼와 별도로 구성된다.
 *   - 이 함수에서 할당된 버퍼가 NVMe 명령의 Metadata Region Pointer로
 *     매핑된다 (추정).
 */
void bio_integrity_alloc_buf(struct bio *bio, bool zero_buffer)
{
	/* bio의 disk integrity profile: NVMe namespace format 기반 */
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);
	/* bio에 이미 연결된 integrity payload */
	struct bio_integrity_payload *bip = bio_integrity(bio);
	/* 섹터 수 -> metadata 바이트 수 변환: NVMe LBA * metadata_size */
	unsigned int len = bio_integrity_bytes(bi, bio_sectors(bio));
	/* zero_buffer가 true면 metadata 버퍼를 0으로 초기화 (NVMe NOGENERATE 경로) */
	gfp_t gfp = GFP_NOIO | (zero_buffer ? __GFP_ZERO : 0);
	void *buf;

	/*
	 * 일반 kmalloc 시도: direct reclaim을 끄고 메모리 부족 시 fallback.
	 * NVMe I/O 경로에서는 GFP_NOIO를 유지해야 한다. 그렇지 않으면
	 * nvme_queue_rq -> nvme_submit_cmd(doorbell) 발행 지연으로 SQ head
	 * 도어티가 밀릴 수 있다 (추정).
	 */
	buf = kmalloc(len, (gfp & ~__GFP_DIRECT_RECLAIM) |
			__GFP_NOMEMALLOC | __GFP_NORETRY | __GFP_NOWARN);
	if (unlikely(!buf)) {
		struct page *page;

		/*
		 * mempool fallback: integrity_buf_pool에서 미리 예약된 페이지를
		 * 꺼낸다. NVMe latency에 민감한 경로에서 reclaim 없이 메모리를
		 * 확보하기 위한 안전장치.
		 */
		page = mempool_alloc(&integrity_buf_pool, GFP_NOFS);
		if (zero_buffer)
			memset(page_address(page), 0, len);
		bvec_set_page(&bip->bip_vec[0], page, len, 0);
		bip->bip_flags |= BIP_MEMPOOL;
	} else {
		/* kmalloc로 얻은 virtual address -> page/offset 변환 후 bvec 설정 */
		bvec_set_page(&bip->bip_vec[0], virt_to_page(buf), len,
				offset_in_page(buf));
	}

	/* 단일 bvec에 전체 metadata가 담김 -> NVMe metadata pointer 1개로 매핑 */
	bip->bip_vcnt = 1;
	bip->bip_iter.bi_size = len;
}

/*
 * bio_integrity_free_buf - bio_integrity_alloc_buf에서 할당한 버퍼를 해제한다.
 *
 * NVMe 관점: NVMe completion(CQ) 이후 메타데이터 버퍼를 회수하는 경로에서
 * 호출된다. blk_mq_complete_request -> nvme_complete_rq -> bio_endio
 * -> bio_integrity_free_buf (추정).
 */
void bio_integrity_free_buf(struct bio_integrity_payload *bip)
{
	struct bio_vec *bv = &bip->bip_vec[0];

	/* BIP_MEMPOOL 플래그에 따라 mempool 또는 kmalloc 버퍼 회수 */
	if (bip->bip_flags & BIP_MEMPOOL)
		mempool_free(bv->bv_page, &integrity_buf_pool);
	else
		kfree(bvec_virt(bv));
}

/*
 * bio_integrity_setup_default - 기본 integrity 플래그와 seed를 설정한다.
 *
 * NVMe 연결:
 *   - bip seed는 NVMe READ/WRITE 명령의 Reference Tag 초기값과 관련된다.
	 *     NVMe PI Type 1/2/3에 따라 시작 섹터 번호가 RefTag seed가 된다.
 *   - BIP_CHECK_GUARD: 16-bit CRC(Guard) 검증을 활성화. NVMe Guard Type.
 *   - BIP_IP_CHECKSUM: IP checksum 형식을 사용하는 NVMe namespace에
 *     해당 (추정).
 *   - BIP_CHECK_REFTAG: Reference Tag 검증 활성화.
 */
void bio_integrity_setup_default(struct bio *bio)
{
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);
	struct bio_integrity_payload *bip = bio_integrity(bio);

	/* Reference Tag seed를 bio 시작 섹터로 설정. NVMe SLBA와 연결됨. */
	bip_set_seed(bip, bio->bi_iter.bi_sector);

	if (bi->csum_type) {
		/* Guard 검증이 필요함: NVMe PRCHK[Guard] = 1 로 대응 */
		bip->bip_flags |= BIP_CHECK_GUARD;
		if (bi->csum_type == BLK_INTEGRITY_CSUM_IP)
			bip->bip_flags |= BIP_IP_CHECKSUM;
	}
	if (bi->flags & BLK_INTEGRITY_REF_TAG)
		/* Reference Tag 검증이 필요함: NVMe PRCHK[RefTag] = 1 로 대응 */
		bip->bip_flags |= BIP_CHECK_REFTAG;
}

/**
 * bio_integrity_free - Free bio integrity payload
 * @bio:	bio containing bip to be freed
 *
 * Description: Free the integrity portion of a bio.
 */
void bio_integrity_free(struct bio *bio)
{
	/* bio_integrity_payload 메모리 해제 */
	kfree(bio_integrity(bio));
	/* bio가 더 이상 integrity metadata를 참조하지 않음 */
	bio->bi_integrity = NULL;
	/* REQ_INTEGRITY 플래그 클리어: NVMe driver는 metadata 없는 일반 명령으로 인식 */
	bio->bi_opf &= ~REQ_INTEGRITY;
}

/*
 * bio_integrity_init - 새 bip를 0으로 초기화하고 bio에 연결한다.
 *
 * NVMe 관점: REQ_INTEGRITY 플래그가 bio에 설정되면, blk-mq가 request를
 * 할당할 때 integrity_segments 한도를 고려하고, NVMe 드라이버는 metadata
 * 버퍼를 명령어에 첨부해야 한다는 신호로 해석한다 (추정).
 */
void bio_integrity_init(struct bio *bio, struct bio_integrity_payload *bip,
		struct bio_vec *bvecs, unsigned int nr_vecs)
{
	memset(bip, 0, sizeof(*bip));
	/* 최대 integrity bvec 수 설정 -> NVMe metadata SGL 최대 segment 수 */
	bip->bip_max_vcnt = nr_vecs;
	if (nr_vecs)
		bip->bip_vec = bvecs;

	/* bio와 bip 연결: 이후 blk-mq/NVMe 경로가 REQ_INTEGRITY를 인식 */
	bio->bi_integrity = bip;
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
struct bio_integrity_payload *bio_integrity_alloc(struct bio *bio,
						  gfp_t gfp_mask,
						  unsigned int nr_vecs)
{
	struct bio_integrity_alloc *bia;

	/*
	 * 암호화(bio_has_crypt_ctx)와 integrity를 동시에 요청하는 것은
	 * 현재 지원하지 않는다. NVMe: End-to-end PI와 inline encryption은
	 * controller capability에 따라 상호 배타적일 수 있다 (추정).
	 */
	if (WARN_ON_ONCE(bio_has_crypt_ctx(bio)))
		return ERR_PTR(-EOPNOTSUPP);

	/* kmalloc_flex로 bip + 가변 bvecs 한 번에 할당: NVMe metadata SGL 공간 */
	bia = kmalloc_flex(*bia, bvecs, nr_vecs, gfp_mask);
	if (unlikely(!bia))
		return ERR_PTR(-ENOMEM);
	bio_integrity_init(bio, &bia->bip, bia->bvecs, nr_vecs);
	return &bia->bip;
}
EXPORT_SYMBOL(bio_integrity_alloc);

/*
 * bio_integrity_unpin_bvec - 사용자 공간에서 pin한 페이지들의 ref count를
 * 감소시킨다. NVMe 관점: user-mode nvme-cli 등이 pass-through로 본인 버퍼를
 * 사용할 때, DMA mapping 전에 pin된 페이지를 해제하는 역할.
 */
static void bio_integrity_unpin_bvec(struct bio_vec *bv, int nr_vecs)
{
	int i;

	/* 모든 integrity metadata 페이지에 대해 pin count 감소: NVMe DMA 후 정리 */
	for (i = 0; i < nr_vecs; i++)
		unpin_user_page(bv[i].bv_page);
}

/*
 * bio_integrity_uncopy_user - READ 완료 시 bounce 버퍼의 메타데이터를 사용자
 * 원본 버퍼로 복사한다.
 *
 * NVMe 연결: NVMe completion CQ에서 상태가 성공이면, controller가 반환한
 * integrity 메타데이터를 사용자 공간에 전달해야 한다. 이 함수는
 * bio_integrity_map_user -> ... -> nvme_complete_rq 경로에서 READ 방향으로
 * 호출된다 (추정).
 */
static void bio_integrity_uncopy_user(struct bio_integrity_payload *bip)
{
	/* 원본 사용자 bvec은 bip_vec[1..]에 보관되어 있음 */
	unsigned short orig_nr_vecs = bip->bip_max_vcnt - 1;
	struct bio_vec *orig_bvecs = &bip->bip_vec[1];
	/* bounce 버퍼는 bip_vec[0], controller가 채운 metadata */
	struct bio_vec *bounce_bvec = &bip->bip_vec[0];
	size_t bytes = bounce_bvec->bv_len;
	struct iov_iter orig_iter;
	int ret;

	/* 원본 사용자 iterator 복원 후 controller 메타데이터 복사 */
	iov_iter_bvec(&orig_iter, ITER_DEST, orig_bvecs, orig_nr_vecs, bytes);
	ret = copy_to_iter(bvec_virt(bounce_bvec), bytes, &orig_iter);
	WARN_ON_ONCE(ret != bytes);

	/* 복사 완료 후 사용자 페이지 pin 해제: NVMe READ completion cleanup */
	bio_integrity_unpin_bvec(orig_bvecs, orig_nr_vecs);
}

/**
 * bio_integrity_unmap_user - Unmap user integrity payload
 * @bio:	bio containing bip to be unmapped
 *
 * Unmap the user mapped integrity portion of a bio.
 */
void bio_integrity_unmap_user(struct bio *bio)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);

	if (bip->bip_flags & BIP_COPY_USER) {
		/* READ 방향이면 controller가 채운 메타데이터를 사용자 버퍼로 복사 */
		if (bio_data_dir(bio) == READ)
			bio_integrity_uncopy_user(bip);
		/* bounce 버퍼 해제: NVMe metadata DMA buffer 반납 */
		kfree(bvec_virt(bip->bip_vec));
		return;
	}

	/* bounce 없이 사용자 페이지를 직접 사용한 경우 pin 해제 */
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
int bio_integrity_add_page(struct bio *bio, struct page *page,
			   unsigned int len, unsigned int offset)
{
	struct request_queue *q = bdev_get_queue(bio->bi_bdev);
	struct bio_integrity_payload *bip = bio_integrity(bio);

	if (bip->bip_vcnt > 0) {
		struct bio_vec *bv = &bip->bip_vec[bip->bip_vcnt - 1];

		/*
		 * zone device(pmem/zoned) 페이지들이 동일한 pgmap를 공유하지
		 * 않으면 병합 불가. NVMe ZNS/PMem + PI 사용 시 주의.
		 */
		if (!zone_device_pages_have_same_pgmap(bv->bv_page, page))
			return 0;

		/*
		 * 같은 물리 페이지에 인접한 메타데이터면 merge. NVMe SGL/PRP
		 * 세그먼트 수를 줄여 DMA 효율을 높인다.
		 */
		if (bvec_try_merge_hw_page(q, bv, page, len, offset)) {
			bip->bip_iter.bi_size += len;
			return len;
		}

		/*
		 * NVMe 컨트롤러의 max_integrity_segments 한도를 초과하면
		 * 추가 세그먼트를 허용하지 않는다.
		 */
		if (bip->bip_vcnt >=
		    min(bip->bip_max_vcnt, queue_max_integrity_segments(q)))
			return 0;

		/*
		 * 큐가 SG gap을 지원하지 않으면 연속하지 않은 메타데이터 추가를
		 * 금지. NVMe PRP List는 4K 정렬을 요구하는 경우가 많다 (추정).
		 */
		if (bvec_gap_to_prev(&q->limits, bv, offset))
			return 0;
	}

	/* 새로운 integrity bvec 추가: NVMe metadata SGL/PRP entry 증가 */
	bvec_set_page(&bip->bip_vec[bip->bip_vcnt], page, len, offset);
	bip->bip_vcnt++;
	bip->bip_iter.bi_size += len;

	return len;
}
EXPORT_SYMBOL(bio_integrity_add_page);

/*
 * bio_integrity_copy_user - 사용자 공간 integrity 메타데이터를 커널 bounce
 * 버퍼로 복사한다.
 *
 * NVMe 연결: 사용자 버퍼가 DMA alignment/constraint를 만족하지 않으면, 이
 * bounce 버퍼를 NVMe 명령의 metadata pointer로 사용한다. WRITE 시에는
 * 사용자 메타데이터를 복사하고, READ 시에는 controller가 채운 메타데이터를
 * 사용자에게 돌려준다.
 */
static int bio_integrity_copy_user(struct bio *bio, struct bio_vec *bvec,
				   int nr_vecs, unsigned int len)
{
	/* WRITE/READ 방향에 따라 사용자 데이터를 bounce로 복사하거나 controller 결과를 복사 */
	bool write = op_is_write(bio_op(bio));
	struct bio_integrity_payload *bip;
	struct iov_iter iter;
	void *buf;
	int ret;

	/* 커널 bounce 버퍼 할당: NVMe DMA 요구 alignment를 맞추기 위함 */
	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (write) {
		/* WRITE: 사용자 메타데이터 -> 커널 bounce 버퍼. NVMe가 DMA로 읽음. */
		iov_iter_bvec(&iter, ITER_SOURCE, bvec, nr_vecs, len);
		if (!copy_from_iter_full(buf, len, &iter)) {
			ret = -EFAULT;
			goto free_buf;
		}

		/* WRITE는 bounce 버퍼 1개만 필요: NVMe metadata pointer 1개 */
		bip = bio_integrity_alloc(bio, GFP_KERNEL, 1);
	} else {
		/* READ: NVMe가 채울 버퍼를 0으로 초기화, 원본 bvec 보존. */
		memset(buf, 0, len);

		/*
		 * We need to preserve the original bvec and the number of vecs
		 * in it for completion handling
		 */
		bip = bio_integrity_alloc(bio, GFP_KERNEL, nr_vecs + 1);
	}

	if (IS_ERR(bip)) {
		ret = PTR_ERR(bip);
		goto free_buf;
	}

	if (write)
		/* WRITE는 사용자 페이지를 더 이상 참조하지 않으므로 pin 해제 */
		bio_integrity_unpin_bvec(bvec, nr_vecs);
	else
		/* READ는 completion 시 사용자 버퍼로 복사할 수 있도록 원본 bvec 보존 */
		memcpy(&bip->bip_vec[1], bvec, nr_vecs * sizeof(*bvec));

	/* bounce 버퍼를 bip에 추가: NVMe metadata buffer로 매핑됨 */
	ret = bio_integrity_add_page(bio, virt_to_page(buf), len,
				     offset_in_page(buf));
	if (ret != len) {
		ret = -ENOMEM;
		goto free_bip;
	}

	/* BIP_COPY_USER: bounce 버퍼 사용 중임을 표시 */
	bip->bip_flags |= BIP_COPY_USER;
	/* READ 방향에서 실제 vcnt는 원본 bvec 수와 동일하게 유지 */
	bip->bip_vcnt = nr_vecs;
	return 0;
free_bip:
	bio_integrity_free(bio);
free_buf:
	kfree(buf);
	return ret;
}

/*
 * bio_integrity_init_user - 사용자 공간 메타데이터가 이미 DMA 가능하고
 * alignment를 만족할 때, 복사 없이 바로 bip에 연결한다.
 *
 * NVMe: nvme-cli pass-through 등에서 사용자가 직접 할당한 메타데이터 버퍼를
 * NVMe 명령의 metadata SGL/PRP로 사용하는 최적 경로 (추정).
 */
static int bio_integrity_init_user(struct bio *bio, struct bio_vec *bvec,
				   int nr_vecs, unsigned int len)
{
	struct bio_integrity_payload *bip;

	bip = bio_integrity_alloc(bio, GFP_KERNEL, nr_vecs);
	if (IS_ERR(bip))
		return PTR_ERR(bip);

	/* 사용자 페이지를 그대로 복사: NVMe DMA가 직접 접근 가능하다고 가정 */
	memcpy(bip->bip_vec, bvec, nr_vecs * sizeof(*bvec));
	bip->bip_iter.bi_size = len;
	bip->bip_vcnt = nr_vecs;
	return 0;
}

/*
 * bvec_from_pages - iov_iter_extract_pages로 얻은 페이지들을 연속된 물리
 * 영역 단위로 병합하여 bvec 배열로 변환한다.
 *
 * NVMe 관점: integrity 메타데이터 페이지들을 NVMe PRP List나 SGL에 넣기 전
 * 전처리 단계. 연속된 페이지는 단일 SGL 세그먼트로 병합하여 segment 수를
 * 줄인다.
 */
static unsigned int bvec_from_pages(struct bio_vec *bvec, struct page **pages,
				    int nr_vecs, ssize_t bytes, ssize_t offset,
				    bool *is_p2p)
{
	unsigned int nr_bvecs = 0;
	int i, j;

	/* 페이지 배열을 순회하며 연속된 물리 페이지를 병합: NVMe SGL/PRP 최적화 */
	for (i = 0; i < nr_vecs; i = j) {
		size_t size = min_t(size_t, bytes, PAGE_SIZE - offset);
		struct folio *folio = page_folio(pages[i]);

		bytes -= size;
		for (j = i + 1; j < nr_vecs; j++) {
			size_t next = min_t(size_t, PAGE_SIZE, bytes);

			/*
			 * 같은 folio 내 연속된 물리 페이지인 경우 merge.
			 * NVMe SGL/PRP에서 불필요한 세그먼트 분할을 막는다.
			 */
			if (page_folio(pages[j]) != folio ||
			    pages[j] != pages[j - 1] + 1)
				break;
			unpin_user_page(pages[j]);
			size += next;
			bytes -= next;
		}

		/*
		 * peer-to-peer DMA 페이지인 경우 NVMe CMB/P2P 경로로 메타데이터가
		 * 전송될 수 있음을 표시 (추정).
		 */
		if (is_pci_p2pdma_page(pages[i]))
			*is_p2p = true;

		/* 병합된 연속 영역을 하나의 bvec으로 변환: NVMe SGL 세그먼트 1개 */
		bvec_set_page(&bvec[nr_bvecs], pages[i], size, offset);
		offset = 0;
		nr_bvecs++;
	}

	return nr_bvecs;
}

/*
 * bio_integrity_map_user - 사용자 공간 iterator(iter)의 integrity 메타데이터를
 * bio의 bip에 매핑한다.
 *
 * 목적: 사용자가 직접 전달한 integrity 메타데이터 영역을 bio에 연결하여
 * block layer가 이를 NVMe 명령과 함께 전송할 수 있게 한다.
 *
 * NVMe 연결:
 *   - queue_max_integrity_segments() 초과 시 bounce 버퍼로 복사.
 *   - P2P DMA 페이지면 REQ_NOMERGE를 설정하여 merge 금지.
 *
 * 호출 경로:
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *   -> nvme_queue_rq -> nvme_setup_cmd -> bio_integrity_map_user (추정)
 */
int bio_integrity_map_user(struct bio *bio, struct iov_iter *iter)
{
	/* bio의 request_queue: NVMe controller queue limits 포함 */
	struct request_queue *q = bdev_get_queue(bio->bi_bdev);
	struct page *stack_pages[UIO_FASTIOV], **pages = stack_pages;
	struct bio_vec stack_vec[UIO_FASTIOV], *bvec = stack_vec;
	iov_iter_extraction_t extraction_flags = 0;
	size_t offset, bytes = iter->count;
	bool copy, is_p2p = false;
	unsigned int nr_bvecs;
	int ret, nr_vecs;

	/* 이미 integrity payload가 있으면 중복 매핑 방지. */
	if (bio_integrity(bio))
		return -EINVAL;
	/* NVMe 컨트롤러의 max_hw_sectors 제한을 초과하는 메타데이터는 거부. */
	if (bytes >> SECTOR_SHIFT > queue_max_hw_sectors(q))
		return -E2BIG;

	/* 사용자 iterator에서 필요한 최대 페이지 수 추정: NVMe metadata SGL 크기 상한 */
	nr_vecs = iov_iter_npages(iter, BIO_MAX_VECS + 1);
	if (nr_vecs > BIO_MAX_VECS)
		return -E2BIG;
	if (nr_vecs > UIO_FASTIOV) {
		/* 빠른 경로 초과 시 동적 bvec/pages 배열 할당 */
		bvec = kzalloc_objs(*bvec, nr_vecs);
		if (!bvec)
			return -ENOMEM;
		pages = NULL;
	}

	/*
	 * DMA alignment와 padding을 맞추지 못하면 bounce 버퍼로 복사.
	 * NVMe PRP는 일반적으로 4K 정렬 및 특정 boundary 제한이 있다 (추정).
	 */
	copy = iov_iter_alignment(iter) &
			blk_lim_dma_alignment_and_pad(&q->limits);

	/* 큐가 P2PDMA를 지원하면 P2P 페이지 추출 허용: NVMe CMB/P2P metadata 경로 */
	if (blk_queue_pci_p2pdma(q))
		extraction_flags |= ITER_ALLOW_P2PDMA;

	ret = iov_iter_extract_pages(iter, &pages, bytes, nr_vecs,
					extraction_flags, &offset);
	if (unlikely(ret < 0))
		goto free_bvec;

	nr_bvecs = bvec_from_pages(bvec, pages, nr_vecs, bytes, offset,
				   &is_p2p);
	if (pages != stack_pages)
		kvfree(pages);
	/* NVMe integrity segment 한도를 초과하면 bounce 버퍼 복사 필요. */
	if (nr_bvecs > queue_max_integrity_segments(q))
		copy = true;
	/* P2P DMA 페이지가 섞이면 merge를 금지해 정확한 DMA 라우팅 보장. */
	if (is_p2p)
		bio->bi_opf |= REQ_NOMERGE;

	if (copy)
		ret = bio_integrity_copy_user(bio, bvec, nr_bvecs, bytes);
	else
		ret = bio_integrity_init_user(bio, bvec, nr_bvecs, bytes);
	if (ret)
		goto release_pages;
	if (bvec != stack_vec)
		kfree(bvec);

	return 0;

release_pages:
	bio_integrity_unpin_bvec(bvec, nr_bvecs);
free_bvec:
	if (bvec != stack_vec)
		kfree(bvec);
	return ret;
}

/*
 * bio_uio_meta_to_bip - uio_meta 구조체의 플래그와 app_tag를 bip 플래그로
 * 변환한다.
 *
 * NVMe 관점: uio_meta 플래그는 NVMe 명령의 PRACT/PRCHK 비트와 의미적으로
 * 대응한다.
 *   - IO_INTEGRITY_CHK_GUARD  -> PRCHK[Guard]
 *   - IO_INTEGRITY_CHK_APPTAG -> PRCHK[AppTag]
 *   - IO_INTEGRITY_CHK_REFTAG -> PRCHK[RefTag]
 * app_tag는 NVMe WRITE/READ의 Application Tag 필드에 사용된다 (추정).
 */
static void bio_uio_meta_to_bip(struct bio *bio, struct uio_meta *meta)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);

	if (meta->flags & IO_INTEGRITY_CHK_GUARD)
		bip->bip_flags |= BIP_CHECK_GUARD;
	if (meta->flags & IO_INTEGRITY_CHK_APPTAG)
		bip->bip_flags |= BIP_CHECK_APPTAG;
	if (meta->flags & IO_INTEGRITY_CHK_REFTAG)
		bip->bip_flags |= BIP_CHECK_REFTAG;

	bip->app_tag = meta->app_tag;
}

/*
 * bio_integrity_map_iter - uio_meta 기반으로 현재 bio에 맞는 integrity
 * 메타데이터만 매핑한다.
 *
 * NVMe 연결: pass-through 또는 user-space NVMe I/O에서, meta->seed가 NVMe
 * Reference Tag이고, bio_sectors에 해당하는 metadata 길이만큼만 iterator를
 * 진행한다. NVMe CID 단위로 잘린 I/O에도 올바른 RefTag가 유지되도록 한다.
 */
int bio_integrity_map_iter(struct bio *bio, struct uio_meta *meta)
{
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);
	unsigned int integrity_bytes;
	int ret;
	struct iov_iter it;

	/* disk가 integrity를 지원하지 않으면 NVMe PI 경로 없음 */
	if (!bi)
		return -EINVAL;
	/*
	 * original meta iterator can be bigger.
	 * process integrity info corresponding to current data buffer only.
	 */
	it = meta->iter;
	/* 현재 bio 데이터 섹터 수에 해당하는 metadata 바이트 수 산출 */
	integrity_bytes = bio_integrity_bytes(bi, bio_sectors(bio));
	if (it.count < integrity_bytes)
		return -EINVAL;

	/* should fit into two bytes */
	BUILD_BUG_ON(IO_INTEGRITY_VALID_FLAGS >= (1 << 16));

	if (meta->flags && (meta->flags & ~IO_INTEGRITY_VALID_FLAGS))
		return -EINVAL;

	/* 현재 bio에 맞게 iterator 길이 제한: NVMe CID 단위 RefTag 정확성 유지 */
	it.count = integrity_bytes;
	ret = bio_integrity_map_user(bio, &it);
	if (!ret) {
		bio_uio_meta_to_bip(bio, meta);
		/* seed를 현재 bio의 시작 RefTag로 설정: NVMe SLBA 기준 */
		bip_set_seed(bio_integrity(bio), meta->seed);
		iov_iter_advance(&meta->iter, integrity_bytes);
		/*
		 * seed를 다음 bio의 시작 RefTag로 진행. NVMe PI Type 1/2/3
		 * 모두에서 RefTag는 섹터/interval 단위로 증가한다 (추정).
		 */
		meta->seed += bio_integrity_intervals(bi, bio_sectors(bio));
	}
	return ret;
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
void bio_integrity_advance(struct bio *bio, unsigned int bytes_done)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);
	/* 완료된 데이터 바이트 -> metadata 바이트 변환: NVMe LBA 기준 */
	unsigned bytes = bio_integrity_bytes(bi, bytes_done >> 9);

	/*
	 * 완료된 데이터 바이트 수만큼 RefTag seed를 전진. NVMe controller가
	 * partial completion 후에도 다음 섹터의 Reference Tag가 올바르게
	 * 이어지도록 한다 (추정).
	 */
	bip->bip_iter.bi_sector += bio_integrity_intervals(bi, bytes_done >> 9);
	bvec_iter_advance(bip->bip_vec, &bip->bip_iter, bytes);
}

/**
 * bio_integrity_trim - Trim integrity vector
 * @bio:	bio whose integrity vector to update
 *
 * Description: Used to trim the integrity vector in a cloned bio.
 */
void bio_integrity_trim(struct bio *bio)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);

	/*
	 * 복제된 bio의 integrity iterator 크기를 실제 데이터 섹터 수에 맞춘다.
	 * NVMe split/clone 후에도 metadata 길이가 데이터 길이와 일치해야
	 * controller가 정상 처리한다.
	 */
	bip->bip_iter.bi_size = bio_integrity_bytes(bi, bio_sectors(bio));
}
EXPORT_SYMBOL(bio_integrity_trim);

/**
 * bio_integrity_clone - Callback for cloning bios with integrity metadata
 * @bio:	New bio
 * @bio_src:	Original bio
 * @gfp_mask:	Memory allocation mask
 *
 * Description:	Called to allocate a bip when cloning a bio
 */
int bio_integrity_clone(struct bio *bio, struct bio *bio_src,
			gfp_t gfp_mask)
{
	struct bio_integrity_payload *bip_src = bio_integrity(bio_src);
	struct bio_integrity_payload *bip;

	BUG_ON(bip_src == NULL);

	bip = bio_integrity_alloc(bio, gfp_mask, 0);
	if (IS_ERR(bip))
		return PTR_ERR(bip);

	/*
	 * 원본 bio의 integrity vector와 seed, 검증 플래그를 공유 복사.
	 * NVMe: request split/clone 시에도 동일한 RefTag seed와 Guard/AppTag
	 * 검증 설정이 유지되어야 한다.
	 */
	bip->bip_vec = bip_src->bip_vec;
	bip->bip_iter = bip_src->bip_iter;
	bip->bip_flags = bip_src->bip_flags & BIP_CLONE_FLAGS;
	bip->app_tag = bip_src->app_tag;

	return 0;
}

/*
 * bio_integrity_initfn - 모듈 초기화 시 integrity_buf_pool을 생성한다.
 *
 * NVMe 관점: I/O 중 메모리 부족 시에도 메타데이터 버퍼를 확보할 수 있도록
 * 미리 PAGE_SIZE 기반 mempool을 준비. 이 풀은 nvme_queue_rq 경로에서
 * GFP_NOIO 상황에서도 fallback으로 사용된다 (추정).
 */
static int __init bio_integrity_initfn(void)
{
	/* BLK_INTEGRITY_MAX_SIZE 크기의 페이지 pool 생성: NVMe metadata bounce 버퍼용 */
	if (mempool_init_page_pool(&integrity_buf_pool, BIO_POOL_SIZE,
			get_order(BLK_INTEGRITY_MAX_SIZE)))
		panic("bio: can't create integrity buf pool\n");
	return 0;
}
subsys_initcall(bio_integrity_initfn);

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
