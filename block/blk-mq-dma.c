// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Christoph Hellwig
 */

/*
 * blk-mq DMA 매핑 헬퍼 (block/blk-mq-dma.c)
 *
 * 이 파일은 blk-mq가 생성한 request의 물리적 메모리 세그먼트를 DMA 주소로
 * 변환하는 반복자 기반 인프라를 제공한다. NVMe SSD 관점에서 본다면,
 * 상위 I/O 경로가 만든 bio/req의 데이터 버퍼를 컨트롤러가 SQ에 넣을
 * 명령어(CID)의 PRP 또는 SGL에 담기 위해 DMA 주소로 변환하는 중간 단계다.
 *
 * 주요 호출 흐름:
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
 *   nvme_queue_rq -> nvme_setup_cmd -> nvme_map_data
 *   nvme_map_data -> blk_rq_dma_map_iter_start -> dma_map_phys / IOVA
 *
 * 이 파일은 blk-mq.c의 request 할당/준비 단계 다음에 위치하며, NVMe 드라이버
 * (drivers/nvme/host/pci.c 등)가 실제 doorbell 쓰기 전에 호출하는 DMA 준비
 * 레이어 역할을 한다.
 */

#include <linux/blk-integrity.h>	/* integrity metadata DMA 매핑용 */
#include <linux/blk-mq-dma.h>		/* blk_dma_iter, blk_map_iter 선언 */
#include "blk.h"			/* blk_rq_* 헬퍼, queue limits */

/*
 * struct blk_map_iter 필드와 NVMe 동작 연관성:
 *   - bio / bvecs / iter: request의 데이터 버퍼를 순회한다.
 *     NVMe 명령어 하나(CID)가 여러 bio를 포함할 수 있으므로(bio merge),
 *     이 반복자가 모든 물리 페이지를 PRP/SGL entry로 변환하는 기반이 된다.
 *   - is_integrity: integrity metadata(DIF/DIX 등)를 순회할 때 true.
 *     NVMe End-to-End Data Protection을 사용하는 경우 메타데이터 DMA
 *     주소도 별도로 준비해야 한다.
 */

/*
 * __blk_map_iter_next - 현재 bio를 모두 소진하면 다음 bio로 이동
 *
 * @iter: bio/vec 반복자
 *
 * 한 bio의 모든 bvec을 다 사용하면 bi_next 체인을 따라 다음 bio로 진행한다.
 * NVMe에서는 여러 bio가 merge되어 하나의 CID로 SQ에 들어갈 수 있으므로,
 * 이 함수가 bio 체인을 따라 PRP/SGL entry를 채우는 기반이 된다.
 *
 * 호출 경로:
 *   blk_rq_map_iter_init -> blk_map_iter_next -> __blk_map_iter_next
 */
static bool __blk_map_iter_next(struct blk_map_iter *iter)
{
	/* 아직 현재 bio에 처리할 데이터가 남아 있으면 true: 같은 CID 범위 내에서 다음 bvec/PRP로 진행 */
	if (iter->iter.bi_size)
		return true;
	/* 현재 bio가 없거나 다음 bio가 없으면 순회 종료: NVMe SQ에 넣을 PRP/SGL entry 소진 */
	if (!iter->bio || !iter->bio->bi_next)
		return false;

	iter->bio = iter->bio->bi_next;	/* bio chain 이동: bio merge 시 단일 CID가 여러 bio를 커버 */
	if (iter->is_integrity) {
		/* integrity metadata 반복자/vec 설정 (NVMe DIF/DIX 메타데이터) */
		iter->iter = bio_integrity(iter->bio)->bip_iter;
		iter->bvecs = bio_integrity(iter->bio)->bip_vec;
	} else {
		/* 일반 데이터 영역 반복자/vec 설정 */
		iter->iter = iter->bio->bi_iter;
		iter->bvecs = iter->bio->bi_io_vec;
	}
	return true;
}

/*
 * blk_map_iter_next - request에서 다음 물리적 세그먼트(phys_vec)를 얻음
 *
 * @req: blk-mq request
 * @iter: bio/vec 반복자
 * @vec:  출력용 물리적 주소(paddr)와 길이(len)
 *
 * bio_vec 단위로 물리 주소와 길이를 추출하고, queue limits의
 * max_segment_size와 biovec_phys_mergeable() 조건을 만족하면 인접한
 * bvec들을 하나의 세그먼트로 병합한다. NVMe PRP/SGL에서는 이 병합 결과가
 * PRP entry 하나나 SGL segment 하나로 변환된다.
 *
 * 호출 경로:
 *   blk_rq_dma_map_iter_start -> blk_map_iter_next
 *   __blk_rq_map_sg -> blk_map_iter_next
 */
static bool blk_map_iter_next(struct request *req, struct blk_map_iter *iter,
			      struct phys_vec *vec)
{
	unsigned int max_size;
	struct bio_vec bv;

	/* 현재 bio 내 처리할 데이터가 없으면 false: PRP/SGL 채울 페이지 없음 */
	if (!iter->iter.bi_size)
		return false;

	bv = mp_bvec_iter_bvec(iter->bvecs, iter->iter);	/* 현재 bvec 가져오기: PRP/SGL entry의 후보 */
	vec->paddr = bvec_phys(&bv);				/* 물리 주소: NVMe PRP entry 주소 필드로 사용 */
	/* queue limits에서 이 주소에 허용되는 최대 세그먼트 크기 계산: NVMe max_transfer_size/MDTS 제약 반영 */
	max_size = get_max_segment_size(&req->q->limits, vec->paddr, UINT_MAX);
	bv.bv_len = min(bv.bv_len, max_size);			/* 세그먼트 크기 클리핑: PRP entry가 queue limit 초과하지 않도록 */
	bvec_iter_advance_single(iter->bvecs, &iter->iter, bv.bv_len);	/* 반복자 전진: 다음 bvec/PRP entry 준비 */

	/*
	 * If we are entirely done with this bi_io_vec entry, check if the next
	 * one could be merged into it.  This typically happens when moving to
	 * the next bio, but some callers also don't pack bvecs tight.
	 */
	/*
	 * 현재 bio_vec entry를 모두 소진했으면 다음 entry와 병합 가능한지
	 * 검사한다. 이는 보통 다음 bio로 넘어갈 때 발생하며, bvec이 꽉 채워지지
	 * 않은 경우에도 생길 수 있다. NVMe SGL 구성 시 인접 물리 페이지를 하나의
	 * sg entry로 합쳐 PRP/SGL 효율을 높인다.
	 */
	while (!iter->iter.bi_size || !iter->iter.bi_bvec_done) {
		struct bio_vec next;

		/* 다음 bio/vec로 이동할 수 있는지 확인 (bio chain 순회) */
		if (!__blk_map_iter_next(iter))
			break;

		next = mp_bvec_iter_bvec(iter->bvecs, iter->iter);	/* 후속 bvec: 연속 물리 페이지인지 검사 대상 */
		/* max_segment_size 초과 또는 물리적으로 연속되지 않으면 병합 중단: PRP list/SGL 분기 필요 */
		if (bv.bv_len + next.bv_len > max_size ||
		    !biovec_phys_mergeable(req->q, &bv, &next))
			break;

		bv.bv_len += next.bv_len;				/* 인접 페이지 병합: PRP entry 수 감소, SGL 길이 축소 */
		bvec_iter_advance_single(iter->bvecs, &iter->iter, next.bv_len);	/* 병합된 만큼 반복자 전진 */
	}

	vec->len = bv.bv_len;						/* 최종 세그먼트 길이: NVMe PRP/SGL length 필드 */
	return true;
}

/*
 * struct phys_vec 필드와 NVMe 동작 연관성:
 *   - paddr: DMA 매핑 대상이 되는 물리 주소. NVMe PRP entry의 주소
 *     필드 또는 SGL segment의 페이지/오프셋으로 변환된다.
 *   - len:   DMA로 전송할 바이트 수. PRP entry의 길이(보통 4k 배수) 또는
 *     SGL segment의 길이가 된다.
 */

/*
 * blk_can_dma_map_iova - IOVA 기반 DMA 매핑이 가능한지 검사
 *
 * IOVA 기반 DMA API는 IOMMU page 크기(<= PAGE_SIZE, 보통 4k) 단위로
 * 분산 물리 페이지를 하나의 연속 DMA 주소로 묶는다. 요청 내 물리적
 * 메모리 간 gap mask가 dma_get_merge_boundary()와 겹치지 않을 때만 이
 * 경로를 사용할 수 있다. NVMe에서는 IOMMU 뒤에 있는 컨트롤러도 단일
 * IOVA 매핑으로 PRP list 대신 효율적으로 DMA를 수행할 수 있다(추정).
 */
static inline bool blk_can_dma_map_iova(struct request *req,
					struct device *dma_dev)
{
	/* req의 물리적 gap mask와 IOMMU 병합 경계가 겹치지 않으면 IOVA 병합 가능: PRP list 대체 가능 (추정) */
	return !(req_phys_gap_mask(req) & dma_get_merge_boundary(dma_dev));
}

/*
 * struct blk_dma_iter 필드와 NVMe 동작 연관성:
 *   - iter:  blk_map_iter를 포함, request의 데이터/integrity를 순회한다.
 *   - addr:  DMA 매핑 결과로 얻은 bus 주소. NVMe PRP1/PRP2 또는 SGL
 *            descriptor에 기록된다.
 *   - len:   addr로 매핑된 길이. PRP entry 길이 또는 SGL segment 길이.
 *   - status: 매핑 실패 원인(BLK_STS_RESOURCE 등). nvme_submit_cmd
 *             직전에 오류 처리에 사용된다.
 *   - p2pdma: PCI Peer-to-Peer DMA 상태. NVMe CMB나 peer NVMe 장치 간
 *             직접 DMA 경로를 사용할 때有意義하다(추정).
 */

/*
 * blk_dma_map_bus - P2P(bus address) 매핑 수행
 *
 * PCI P2PDMA가 활성화된 경우, 호스트 메모리가 아닌 peer 장치의 bus
 * address로 매핑한다. NVMe CMB(Controller Memory Buffer)나 peer-to-peer
 * NVMe 간 복사 시 이 경로를 탈 수 있다(추정).
 */
static bool blk_dma_map_bus(struct blk_dma_iter *iter, struct phys_vec *vec)
{
	iter->addr = pci_p2pdma_bus_addr_map(iter->p2pdma.mem, vec->paddr);	/* peer 장치 bus 주소: NVMe CMB/p2p DMA target */
	iter->len = vec->len;							/* 매핑 길이: PRP/SGL entry 길이와 동일 */
	return true;
}

/*
 * blk_dma_map_direct - 단일 물리적 세그먼트를 DMA 주소로 직접 매핑
 *
 * dma_map_phys()를 이용해 vec->paddr를 dma_dev 주소 공간에 매핑하고,
 * 매핑 실패 시 BLK_STS_RESOURCE를 반환한다. NVMe PRP entry의
 * PRP1/PRP2 또는 SGL entry에 기록될 DMA 주소가 여기서 생성된다.
 */
static bool blk_dma_map_direct(struct request *req, struct device *dma_dev,
				struct blk_dma_iter *iter, struct phys_vec *vec)
{
	unsigned int attrs = 0;

	/* host bridge를 통과하는 P2P는 MMIO 속성이 필요하다(추정) */
	if (iter->p2pdma.map == PCI_P2PDMA_MAP_THRU_HOST_BRIDGE)
		attrs |= DMA_ATTR_MMIO;

	iter->addr = dma_map_phys(dma_dev, vec->paddr, vec->len,
				rq_dma_dir(req), attrs);		/* 물리 주소 -> bus 주소: NVMe PRP1/PRP2 또는 SGL 주소 */
	/* DMA 매핑 실패: NVMe는 보통 이 명령어(CID)를 재시도/에러 완료 처리 */
	if (dma_mapping_error(dma_dev, iter->addr)) {
		iter->status = BLK_STS_RESOURCE;		/* 리소스 부족: NVMe queue_rq가 BLK_STS_RESOURCE 반환 -> 재시큐/abort 가능 */
		return false;
	}
	iter->len = vec->len;					/* 매핑 길이 확정: PRP/SGL length */
	return true;
}

/*
 * blk_rq_dma_map_iova - IOVA 상태를 이용한 연속 DMA 매핑
 *
 * IOMMU page 크기 단위로 분산된 물리 페이지들을 하나의 IOVA 영역으로
 * 묶어 DMA mapping table을 구성한다. NVMe는 scatter-gather list가 긴
 * 명령어도 단일 연속 DMA 주소로 submit할 수 있어 PRP list 길이 제한을
 * 줄이는 데 도움이 된다(추정).
 *
 * 호출 경로:
 *   blk_dma_map_iter_start -> blk_rq_dma_map_iova
 */
static bool blk_rq_dma_map_iova(struct request *req, struct device *dma_dev,
				struct dma_iova_state *state, struct blk_dma_iter *iter,
				struct phys_vec *vec)
{
	enum dma_data_direction dir = rq_dma_dir(req);			/* NVMe 명령어 방향: DMA_TO/DEVICE/FROM/BIDIRECTIONAL */
	unsigned int attrs = 0;
	size_t mapped = 0;						/* 누적 매핑 바이트: PRP list 대체 IOVA 크기 */
	int error;

	/* IOVA 상태에서 이미 할당받은 시작 DMA 주소/크기를 반복자에 설정 */
	iter->addr = state->addr;					/* 연속 IOVA 시작 주소: NVMe PRP1/SGL pointer로 기록 (추정) */
	iter->len = dma_iova_size(state);				/* IOVA 영역 전체 크기: command dptr 길이 근거 */

	/* host bridge를 통과하는 P2P는 MMIO 속성 필요 (추정) */
	if (iter->p2pdma.map == PCI_P2PDMA_MAP_THRU_HOST_BRIDGE)
		attrs |= DMA_ATTR_MMIO;

	do {
		/* 현재 phys_vec를 IOVA 매핑에 연결 (NVMe PRP/SGL 물리 페이지 추가) */
		error = dma_iova_link(dma_dev, state, vec->paddr, mapped,
				      vec->len, dir, attrs);
		if (error)
			goto out_unlink;			/* 부분 매핑 실패: NVMe 명령어 abort/재시큐 경로 */
		mapped += vec->len;				/* 누적 매핑 길이 갱신: IOMMU TLB 동기화 범위 */
		/* 다음 세그먼트도 같은 IOVA 영역에 포함 (추정) */
	} while (blk_map_iter_next(req, &iter->iter, vec));

	/* 모든 페이지를 IOVA에 연결 후 IOMMU TLB 동기화 (추정) */
	error = dma_iova_sync(dma_dev, state, 0, mapped);
	if (error)
		goto out_unlink;

	return true;

out_unlink:
	/* 일부만 매핑된 IOVA entry를 정리하고 블록 오류 코드 기록 */
	dma_iova_destroy(dma_dev, state, mapped, dir, attrs);	/* 매핑 해제: NVMe abort 시 DMA 리소스 정리 */
	iter->status = errno_to_blk_status(error);		/* 오류 코드: nvme_queue_rq -> BLK_STS_* 변환 */
	return false;
}

/*
 * blk_rq_map_iter_init - request로부터 bio/vec 반복자 초기화
 *
 * @rq:  초기화할 request
 * @iter: 반복자 출력
 *
 * RQF_SPECIAL_PAYLOAD(예: 드라이버 낸부 명령)인 경우 special_vec을,
 * 일반 요청은 rq->bio의 bi_io_vec을, flush 요청은 빈 반복자를 설정한다.
 * NVMe admin/IO queue에서 사용하는 nvme_command는 대부분 rq->bio 기반
 * 데이터 버퍼를 여기서 읽기 시작한다.
 */
static inline void blk_rq_map_iter_init(struct request *rq,
					struct blk_map_iter *iter)
{
	struct bio *bio = rq->bio;

	if (rq->rq_flags & RQF_SPECIAL_PAYLOAD) {
		/* NVMe admin command 등 드라이버 낸부 payload: PRP/SGL이 아닌 special_vec 사용 */
		*iter = (struct blk_map_iter) {
			.bvecs = &rq->special_vec,
			.iter = {
				.bi_size = rq->special_vec.bv_len,
			}
		};
	} else if (bio) {
		/* 일반 I/O: rq->bio -> bi_io_vec부터 PRP/SGL entry 순회 시작 */
		*iter = (struct blk_map_iter) {
			.bio = bio,
			.bvecs = bio->bi_io_vec,
			.iter = bio->bi_iter,
		};
	} else {
		/* 낸부 flush request는 bio가 붙지 않을 수 있으므로 빈 반복자 사용 */
		*iter = (struct blk_map_iter) {};
	}
}

/*
 * blk_dma_map_iter_start - 첫 번째 DMA 세그먼트 매핑의 낸부 구현
 *
 * @req:      매핑할 request
 * @dma_dev:  DMA 매핑 대상 장치 (NVMe 컨트롤러의 pci_dev->dev)
 * @state:    DMA IOVA 상태
 * @iter:     blk_dma_iter 반복자
 * @total_len: 매핑할 전체 payload 길이
 *
 * P2PDMA 상태를 먼저 확인하고, P2P이면 bus address 매핑을, 아니면
 * IOVA 병합 가능 여부에 따라 blk_rq_dma_map_iova() 또는
 * blk_dma_map_direct()를 선택한다. NVMe 드라이버 입장에서는
 * nvme_map_data() 안에서 이 함수를 호출해 PRP/SGL의 첫 엔트리를 채운다.
 *
 * 호출 경로:
 *   blk_rq_dma_map_iter_start -> blk_dma_map_iter_start
 *   blk_rq_integrity_dma_map_iter_start -> blk_dma_map_iter_start
 */
static bool blk_dma_map_iter_start(struct request *req, struct device *dma_dev,
				   struct dma_iova_state *state, struct blk_dma_iter *iter,
				   unsigned int total_len)
{
	struct phys_vec vec;

	/* p2pdma 상태를 초기화: 기본값은 P2P 미사용 */
	memset(&iter->p2pdma, 0, sizeof(iter->p2pdma));
	iter->status = BLK_STS_OK;					/* 초기 상태: 이후 NVMe queue_rq 성공/실패 판단 */
	iter->p2pdma.map = PCI_P2PDMA_MAP_NONE;			/* P2P 기본값: 일반 host memory DMA 경로 */

	/*
	 * P2P 전송 판별에 첫 번째 세그먼트가 필요하므로 가능한 한 빨리
	 * phys_vec를 가져온다. 이 주소가 NVMe PRP1/SGL 첫 entry가 된다.
	 */
	if (!blk_map_iter_next(req, &iter->iter, &vec))
		return false;						/* 매핑할 세그먼트 없음: 제로-바이트 NVMe 명령어 가능 */

	switch (pci_p2pdma_state(&iter->p2pdma, dma_dev,
				 phys_to_page(vec.paddr))) {
	case PCI_P2PDMA_MAP_BUS_ADDR:
		/* peer 장치 bus address로 직접 매핑 (NVMe CMB/p2p 경로) */
		return blk_dma_map_bus(iter, &vec);
	case PCI_P2PDMA_MAP_THRU_HOST_BRIDGE:
		/*
		 * host bridge를 통과하는 P2P는 일반 DMA 경로와 동일하게
		 * 처리되며 unmap 시에도 동일하게 처리한다.
		 */
	case PCI_P2PDMA_MAP_NONE:
		break;
	default:
		iter->status = BLK_STS_INVAL;				/* P2P 상태 오류: NVMe 명령어 제출 불가 */
		return false;
	}

	/* IOVA 병합이 가능하면 분산 페이지를 하나의 연속 DMA 주소로 매핑 */
	if (blk_can_dma_map_iova(req, dma_dev) &&
	    dma_iova_try_alloc(dma_dev, state, vec.paddr, total_len))
		return blk_rq_dma_map_iova(req, dma_dev, state, iter, &vec);
	/* IOVA 불가능하면 단일 세그먼트 단위로 직접 DMA 매핑 (NVMe PRP list) */
	memset(state, 0, sizeof(*state));				/* IOVA 미사용: 이후 dma_iova_destroy() 호출 방지 */
	return blk_dma_map_direct(req, dma_dev, iter, &vec);	/* 첫 PRP1/SGL entry 매핑 완료 */
}


/*
 * blk_rq_dma_map_iter_start - 외부 호출용 첫 DMA 매핑 인터페이스
 *
 * NVMe 드라이버는 이 함수를 통해 request의 payload 길이만큼 DMA 맵핑을
 * 시작한다. 반환된 iter->addr/len은 PRP1 또는 SGL 첫 entry에 기록된다.
 * @state는 unmap 시까지 유지해야 하며, @iter는 map 시에만 사용된다.
 *
 * 호출 경로:
 *   nvme_queue_rq -> nvme_setup_cmd -> nvme_map_data
 *   -> blk_rq_dma_map_iter_start
 */
bool blk_rq_dma_map_iter_start(struct request *req, struct device *dma_dev,
			       struct dma_iova_state *state, struct blk_dma_iter *iter)
{
	blk_rq_map_iter_init(req, &iter->iter);			/* bio chain -> PRP/SGL 반복자 초기화 */
	return blk_dma_map_iter_start(req, dma_dev, state, iter,
				      blk_rq_payload_bytes(req));				/* payload 길이: NVMe NLB/Length 계산과 연결 */
}
EXPORT_SYMBOL_GPL(blk_rq_dma_map_iter_start);


/*
 * blk_rq_dma_map_iter_next - 이후 DMA 세그먼트를 순차 매핑
 *
 * 첫 세그먼트 이후의 PRP2, PRP list, 또는 SGL 추가 entry를 채울 때
 * 사용한다. P2P 경로가 아니면 blk_dma_map_direct()로 한 세그먼트씩
 * 매핑한다.
 *
 * 호출 경로:
 *   nvme_map_data -> blk_rq_dma_map_iter_start -> blk_rq_dma_map_iter_next
 */
bool blk_rq_dma_map_iter_next(struct request *req, struct device *dma_dev,
			      struct blk_dma_iter *iter)
{
	struct phys_vec vec;

	/* 더 이상 매핑할 세그먼트가 없으면 false: PRP list/SGL 끝에 도달 */
	if (!blk_map_iter_next(req, &iter->iter, &vec))
		return false;

	/* P2P bus address 경로이면 별도 매핑, 아니면 직접 DMA 매핑 */
	if (iter->p2pdma.map == PCI_P2PDMA_MAP_BUS_ADDR)
		return blk_dma_map_bus(iter, &vec);		/* NVMe CMB/p2p: PRP2/list 추가 entry */
	return blk_dma_map_direct(req, dma_dev, iter, &vec);	/* NVMe PRP2 또는 SGL 다음 segment */
}
EXPORT_SYMBOL_GPL(blk_rq_dma_map_iter_next);

/*
 * blk_next_sg - scatterlist에서 다음 sg entry 포인터를 반환
 *
 * @sg:     현재 sg entry 포인터의 포인터
 * @sglist: sglist 배열 시작
 *
 * 드라이버가 매번 sg_init_table()을 호출하지 않아도 이전 termination
 * bit를 강제로 지워 연속 매핑이 가능하게 한다. NVMe SGL 구성 시
 * sglist 배열을 순회하며 sg_set_page()로 챴는다.
 */
static inline struct scatterlist *
blk_next_sg(struct scatterlist **sg, struct scatterlist *sglist)
{
	if (!*sg)
		return sglist;						/* 첫 sg entry: NVMe SGL 첫 descriptor 위치 */

	/*
	 * If the driver previously mapped a shorter list, we could see a
	 * termination bit prematurely unless it fully inits the sg table
	 * on each mapping. We KNOW that there must be more entries here
	 * or the driver would be buggy, so force clear the termination bit
	 * to avoid doing a full sg_init_table() in drivers for each command.
	 */
	/*
	 * 드라이버가 이전에 더 짧은 sglist를 매핑했다면 sg 마지막 end bit가
	 * 남아 있을 수 있다. 드라이버가 매 명령마다 sg_init_table()을 하지
	 * 않아도 되도록 end bit를 강제로 지운다. NVMe SGL은 이 sglist를
	 * 기반으로 PRP/SGL descriptor를 만든다.
	 */
	sg_unmark_end(*sg);						/* sg chain 연장: NVMe SGL segment 연결 */
	return sg_next(*sg);						/* 다음 sg entry: NVMe SGL 다음 descriptor */
}


/*
 * __blk_rq_map_sg - request의 세그먼트를 scatterlist로 변환
 *
 * @rq:      매핑할 request
 * @sglist:  대상 scatterlist 배열
 * @last_sg: 마지막으로 챴은 sg entry 포인터의 포인터
 *
 * blk_map_iter_next()로 얻은 phys_vec를 sg_set_page()를 이용해
 * scatterlist에 챴는다. NVMe 드라이버는 이 scatterlist를 기반으로
 * SGL(Scatter-Gather List) 형식의 DMA descriptor를 컨트롤러에 제출한다.
 *
 * 호출 경로:
 *   nvme_map_data -> sg_alloc_table -> __blk_rq_map_sg (추정)
 */
int __blk_rq_map_sg(struct request *rq, struct scatterlist *sglist,
		    struct scatterlist **last_sg)
{
	struct blk_map_iter iter;
	struct phys_vec vec;
	int nsegs = 0;

	blk_rq_map_iter_init(rq, &iter);				/* request -> bio/vec 반복자 초기화 */
	while (blk_map_iter_next(rq, &iter, &vec)) {			/* 세그먼트 순회: NVMe SGL entry 수만큼 반복 */
		*last_sg = blk_next_sg(last_sg, sglist);

		WARN_ON_ONCE(overflows_type(vec.len, unsigned int));
		/* 물리 페이지/오프셋/길이로 sg entry 설정 (NVMe SGL 한 조각) */
		sg_set_page(*last_sg, phys_to_page(vec.paddr), vec.len,
			    offset_in_page(vec.paddr));
		nsegs++;						/* sg entry 카운트: NVMe SGL segment 수 / PRP list 길이 상한 */
	}

	if (*last_sg)
		sg_mark_end(*last_sg);					/* sglist 종료: NVMe SGL Last/Large descriptor 전환점 */

	/*
	 * Something must have been wrong if the figured number of
	 * segment is bigger than number of req's physical segments
	 */
	/*
	 * 계산된 세그먼트 수가 request의 물리 세그먼트 수보다 많으면
	 * 버그이므로 경고한다. NVMe에서는 이 값이 PRP list 길이나 SGL
	 * segment 수 제한을 초과하면 안 된다.
	 */
	WARN_ON(nsegs > blk_rq_nr_phys_segments(rq));

	return nsegs;							/* 총 sg entry 수: NVMe Identify/SGL limit 검증에 사용 */
}
EXPORT_SYMBOL(__blk_rq_map_sg);

#ifdef CONFIG_BLK_DEV_INTEGRITY

/*
 * blk_rq_integrity_dma_map_iter_start - integrity metadata의 첫 DMA 매핑
 *
 * NVMe DIF/DIX(End-to-End Data Protection)를 사용할 때, 데이터 외에
 * metadata(guard/app/reference tag)도 DMA 가능한 주소로 변환한다.
 * 이 DMA 주소는 NVMe 명령어의 metadata buffer descriptor에 들어간다(추정).
 */
bool blk_rq_integrity_dma_map_iter_start(struct request *req,
					 struct device *dma_dev,  struct dma_iova_state *state,
					 struct blk_dma_iter *iter)
{
	unsigned len = bio_integrity_bytes(&req->q->limits.integrity,
					   blk_rq_sectors(req));	/* NVMe PI metadata 길이: sector 수 * metadata bytes/sector */
	struct bio *bio = req->bio;

	iter->iter = (struct blk_map_iter) {
		.bio = bio,
		.iter = bio_integrity(bio)->bip_iter,		/* integrity bip_iter: NVMe metadata buffer 순회 시작점 */
		.bvecs = bio_integrity(bio)->bip_vec,		/* integrity bip_vec: NVMe PI metadata physical page 목록 */
		.is_integrity = true,
	};
	/* metadata DMA 매핑 시작: NVMe PRP1/PRP2 metadata pointer */
	return blk_dma_map_iter_start(req, dma_dev, state, iter, len);
}
EXPORT_SYMBOL_GPL(blk_rq_integrity_dma_map_iter_start);


/*
 * blk_rq_integrity_dma_map_iter_next - integrity metadata의 다음 DMA 매핑
 *
 * NVMe Data Protection를 위한 메타데이터의 후속 세그먼트를 매핑한다.
 */
bool blk_rq_integrity_dma_map_iter_next(struct request *req,
					struct device *dma_dev, struct blk_dma_iter *iter)
{
	struct phys_vec vec;

	if (!blk_map_iter_next(req, &iter->iter, &vec))
		return false;

	if (iter->p2pdma.map == PCI_P2PDMA_MAP_BUS_ADDR)
		return blk_dma_map_bus(iter, &vec);		/* NVMe PI metadata p2p 경로 */
	return blk_dma_map_direct(req, dma_dev, iter, &vec);	/* NVMe PI metadata 후속 PRP/SGL segment */
}
EXPORT_SYMBOL_GPL(blk_rq_integrity_dma_map_iter_next);


/*
 * blk_rq_map_integrity_sg - integrity metadata를 scatterlist로 변환
 *
 * NVMe 프로텍션 정보(PI)가 포함된 요청의 메타데이터를 sglist에 담아
 * 컨트롤러가 DMA할 수 있도록 준비한다. sglist 크기는
 * rq->nr_integrity_segments 이상이어야 한다.
 */
int blk_rq_map_integrity_sg(struct request *rq, struct scatterlist *sglist)
{
	struct request_queue *q = rq->q;
	struct scatterlist *sg = NULL;
	struct bio *bio = rq->bio;
	unsigned int segments = 0;					/* NVMe PI metadata SGL segment 카운트 */
	struct phys_vec vec;

	struct blk_map_iter iter = {
		.bio = bio,
		.iter = bio_integrity(bio)->bip_iter,
		.bvecs = bio_integrity(bio)->bip_vec,
		.is_integrity = true,
	};

	while (blk_map_iter_next(rq, &iter, &vec)) {			/* NVMe metadata SGL entry 순회 */
		sg = blk_next_sg(&sg, sglist);

		WARN_ON_ONCE(overflows_type(vec.len, unsigned int));
		/* integrity metadata용 sg entry 설정 (NVMe PI metadata SGL) */
		sg_set_page(sg, phys_to_page(vec.paddr), vec.len,
			    offset_in_page(vec.paddr));
		segments++;						/* metadata segment 수: NVMe MPS/MSS 제약 검사 */
	}

	if (sg)
		sg_mark_end(sg);					/* NVMe metadata SGL 종료 */

	/*
	 * Something must have been wrong if the figured number of segment
	 * is bigger than number of req's physical integrity segments
	 */
	/*
	 * 계산된 integrity 세그먼트 수가 request의 허용치를 초과하면
	 * 치명적 오류다. NVMe PI 경로에서 이는 metadata buffer overflow
	 * 가능성을 의미한다.
	 */
	BUG_ON(segments > rq->nr_integrity_segments);			/* NVMe metadata segment 수 초과: 드라이버/장치 불일치 */
	BUG_ON(segments > queue_max_integrity_segments(q));		/* queue limit 초과: NVMe PI metadata buffer overflow */
	return segments;						/* NVMe metadata SGL 총 entry 수 */
}
EXPORT_SYMBOL(blk_rq_map_integrity_sg);
#endif

/*
 * NVMe 관점 핵심 요약
 *
 * - 이 파일은 blk-mq request의 물리 메모리 세그먼트를 DMA 주소로 변환하는
 *   레이어로, NVMe 드라이버가 nvme_submit_cmd(doorbell)을 호출하기 전에
 *   PRP/SGL descriptor를 준비하는 데 필수적이다.
 *
 * - blk_map_iter_next()가 bio chain과 bvec 병합을 처리하므로, NVMe는
 *   여러 bio가 merge된 I/O도 단일 CID로 SQ에 submit할 수 있다.
 *
 * - blk_dma_map_direct()와 blk_rq_dma_map_iova()가 생성하는 iter->addr은
 *   NVMe PRP1/PRP2 또는 SGL entry에 기록되며, 컨트롤러가 DMA를 수행할
 *   bus/IOMMU 주소 공간을 나타낸다.
 *
 * - PCI P2PDMA 경로(PCI_P2PDMA_MAP_BUS_ADDR)를 지원하므로 NVMe CMB나
 *   peer-to-peer NVMe 간 데이터 이동도 고려된 설계다(추정).
 *
 * - integrity DMA 매핑 함수들은 NVMe End-to-End Data Protection(DIF/DIX)
 *   메타데이터를 DMA할 수 있게 하여 데이터 무결성을 보장한다(추정).
 */
