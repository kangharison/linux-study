// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Christoph Hellwig
 */
/*
 * [한국어 설명] request의 물리 세그먼트를 DMA 주소로 변환하는 blk-mq 반복자 계층 (blk-mq-dma.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 blk-mq가 만든 struct request에 매달린 bio 체인(및 integrity metadata)을
 * 순회하면서, 각 bio_vec이 가리키는 물리 페이지를 디바이스가 DMA할 수 있는 주소로
 * 바꿔주는 "반복자(iterator)" 기반 인프라를 제공한다. 핵심은 두 단계다. (1)
 * blk_map_iter_next()가 여러 bio에 걸쳐 흩어진 bio_vec들을 queue_limits의
 * max_segment_size/seg_boundary_mask에 맞춰 최대한 병합하여 struct phys_vec
 * {paddr, len} 단위의 물리 세그먼트를 뽑아낸다. (2) blk_dma_map_direct()/
 * blk_rq_dma_map_iova()/blk_dma_map_bus() 세 경로 중 하나가 그 물리 세그먼트를
 * 실제 DMA 가능한 주소(dma_addr_t)로 바꾼다. IOMMU가 있는 시스템에서는 여러
 * 세그먼트를 하나의 IOVA(IO Virtual Address) 범위로 묶어(coalesce) 매핑 오버헤드를
 * 줄이고, 그렇지 않으면 세그먼트마다 dma_map_phys()를 호출하는 "direct" 경로를
 * 쓴다. 두 장치가 호스트 브리지를 거치지 않고 직접 통신하는 PCI P2PDMA(peer-to-peer
 * DMA)도 별도 분기로 처리한다. 마지막으로 __blk_rq_map_sg()/
 * blk_rq_map_integrity_sg()는 IOMMU를 쓰지 않는 구형 드라이버를 위해 동일한 물리
 * 세그먼트를 struct scatterlist 배열로 채워주는 하위 호환 경로다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * blk-mq 계층에서 request가 블록 드라이버의 ->queue_rq()로 넘어가, 드라이버가 DMA
 * descriptor(NVMe라면 PRP=Physical Region Page 또는 SGL=Scatter Gather List)를
 * 채우는 단계에서 이 파일의 함수들이 호출된다. 대표 호출 체인은 다음과 같다:
 * blk_mq_submit_bio() -> blk_mq_get_request()로 request가 만들어진 뒤, 드라이버의
 * ->queue_rq (예: nvme_queue_rq) -> nvme_setup_cmd() -> nvme_map_data()가
 * blk_rq_dma_map_iter_start()/blk_rq_dma_map_iter_next()를 반복 호출하며 세그먼트를
 * 하나씩 꺼내 PRP1/PRP2 또는 SGL entry에 채우고, 다 채우면 nvme_sq_copy_cmd/nvme_write_sq_db()로
 * SQ(Submission Queue)에 넣고 doorbell을 울린다. 이 파일 자체는 별도 커널 스레드나
 * 인터럽트 컨텍스트를 만들지 않는 순수 라이브러리 함수 모음이며, 블록 I/O를
 * 제출한 프로세스 컨텍스트(동기 read/write, io_uring worker 등) 안에서 호출자의
 * 스택 위에 그대로 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일은 include/linux/blk-mq-dma.h가 선언하는 struct blk_map_iter/
 * struct blk_dma_iter를 사용하고, block/blk.h의 get_max_segment_size()/
 * biovec_phys_mergeable()로 세그먼트 병합 가능 여부를 판단하며,
 * kernel/dma/mapping.c 및 drivers/iommu/dma-iommu.c가 구현하는 dma_map_phys()/
 * dma_iova_*() 계열 API로 실제 DMA 매핑을 수행한다. PCI P2PDMA 판별은
 * drivers/pci/p2pdma.c의 pci_p2pdma_state()/pci_p2pdma_bus_addr_map()에
 * 위임한다. 데이터 흐름은 "bio->bi_io_vec (또는 bio_integrity->bip_vec) ->
 * struct phys_vec(물리 주소+길이) -> dma_addr_t(버스/IOVA 주소)" 순으로 좁혀지며,
 * 이 dma_addr_t가 최종적으로 NVMe/SCSI 등 실제 드라이버의 명령어 descriptor에
 * 기록된다. CONFIG_BLK_DEV_INTEGRITY가 켜진 시스템에서는 동일한 반복자 인프라가
 * 데이터 대신 bio_integrity_payload의 bip_vec/bip_iter를 순회해 DIF/DIX(Data
 * Integrity Field/Extension) metadata용 DMA 주소도 만들어 준다.
 *
 * === 주요 함수/구조체 요약 ===
 * - __blk_map_iter_next()/blk_map_iter_next(): bio 체인을 넘나들며 병합 가능한
 *   bio_vec들을 하나의 struct phys_vec으로 뭉쳐 반환하는 핵심 반복자.
 * - blk_dma_map_iter_start()/blk_rq_dma_map_iter_start()/
 *   blk_rq_dma_map_iter_next(): 첫 세그먼트에서 P2P 여부와 IOVA 가능 여부를
 *   판단해 매핑 경로를 고정하고, 이후 세그먼트를 순차 매핑하는 공개 API.
 * - blk_dma_map_direct()/blk_rq_dma_map_iova()/blk_dma_map_bus(): 각각 "직접
 *   dma_map_phys()", "IOVA 영역에 링크 후 일괄 매핑", "P2P bus address 변환"을
 *   수행하는 3가지 실제 매핑 구현체.
 * - __blk_rq_map_sg()/blk_rq_map_integrity_sg(): 동일한 phys_vec 순회 결과를
 *   struct scatterlist 배열에 채우는 레거시(비-IOMMU) 경로.
 * - struct blk_map_iter{iter,bio,bvecs,is_integrity},
 *   struct blk_dma_iter{addr,len,p2pdma,status,iter},
 *   struct phys_vec{paddr,len}는 각각 include/linux/blk-mq-dma.h,
 *   include/linux/types.h에 정의되어 있으며, 이 파일은 그 구조체들을
 *   소비/생산하는 구현부다.
 */
#include <linux/blk-integrity.h>	/* [한국어] blk_integrity_rq(), bio_integrity_bytes() 등 DIF/DIX(Data Integrity Field/Extension) 선언 - integrity 매핑 함수들이 사용 */
#include <linux/blk-mq-dma.h>	/* [한국어] 이 파일이 구현하는 struct blk_map_iter/blk_dma_iter 및 공개 API 원형(prototype) 선언 */
#include "blk.h"	/* [한국어] block 계층 내부 헬퍼 - get_max_segment_size(), biovec_phys_mergeable(), blk_rq_nr_phys_segments() 등 */

/*
 * [한국어] struct blk_map_iter 필드 설명 (정의: include/linux/blk-mq-dma.h)
 *
 * 이 구조체는 request(또는 integrity payload) 하나의 데이터 순회 상태를 담는다.
 * blk_rq_map_iter_init()이 처음 채우고, __blk_map_iter_next()/blk_map_iter_next()가
 * 호출될 때마다 전진시킨다.
 *
 * struct bvec_iter iter;
 *   현재 bio_vec 배열(bvecs) 안에서의 순회 위치(bi_size/bi_idx/bi_bvec_done 등).
 *   설정자: blk_rq_map_iter_init()이 최초 값(bio->bi_iter 등)을 채우고,
 *     bvec_iter_advance_single()이 소비한 만큼 전진시키며, __blk_map_iter_next()가
 *     다음 bio로 넘어갈 때 새 bio 기준으로 재설정한다.
 *   읽는 자: blk_map_iter_next()가 bi_size(더 읽을 데이터가 있는지),
 *     bi_bvec_done(현재 entry 경계에 있는지)을 검사한다.
 *   값 범위: bi_size는 0이면 "이 bio_vec 배열 순회 끝"을 의미.
 *   동기화: 요청을 소유한 단일 프로세스 컨텍스트에서만 갱신 - 락 불필요.
 *
 * struct bio *bio;
 *   현재 순회 중인 bio. bi_next로 체인된 다음 bio로 넘어갈 때 기준이 된다.
 *   설정자: blk_rq_map_iter_init()(첫 bio), __blk_map_iter_next()(bio->bi_next).
 *   읽는 자: __blk_map_iter_next()가 bio->bi_next로 체인 끝 여부를 판단.
 *   값 범위: RQF_SPECIAL_PAYLOAD 요청이나 bio 없는 내부 flush 요청에서는 NULL일
 *     수 있다 - 이 경우 bi_next 체인 이동 자체가 일어나지 않는다.
 *   동기화: 락 불필요(단일 컨텍스트 소유).
 *
 * struct bio_vec *bvecs;
 *   iter가 인덱싱하는 bio_vec 배열의 시작 포인터. 데이터 순회면 bio->bi_io_vec,
 *   integrity 순회면 bio_integrity(bio)->bip_vec.
 *   설정자/읽는 자: 위 iter 필드와 항상 짝을 이뤄 mp_bvec_iter_bvec(bvecs, iter)
 *     형태로 함께 쓰인다.
 *   동기화: 락 불필요.
 *
 * bool is_integrity;
 *   true면 데이터가 아니라 DIF/DIX integrity metadata를 순회 중임을 표시.
 *   설정자: blk_rq_integrity_dma_map_iter_start()/blk_rq_map_integrity_sg()가
 *     구조체 리터럴로 true를 채운다(일반 데이터 반복자는 이 필드를 아예
 *     생략해 false로 남긴다).
 *   읽는 자: __blk_map_iter_next()가 이 플래그를 보고 다음 bio로 넘어갈 때
 *     bi_iter/bi_io_vec 대신 bio_integrity()->bip_iter/bip_vec을 쓸지 결정한다.
 *   동기화: 락 불필요, 생성 시 한 번 정해지면 이후 바뀌지 않는다.
 */

/*
 * [한국어]
 * __blk_map_iter_next - 현재 bio를 다 소진했으면 체인의 다음 bio로 넘어간다
 *
 * @iter: bio/bvec 순회 상태를 담은 반복자. iter->iter(bvec_iter)의 bi_size가 0이
 *        되어 현재 bio를 다 읽었을 때 이 함수가 다음 bio로 갱신한다.
 * @return: 다음으로 진행할 데이터가 있으면 true, bio 체인이 완전히 끝났으면 false.
 *
 * request 하나에는 여러 bio가 bi_next로 체인되어 매달릴 수 있다(bio merge).
 * blk_map_iter_next()가 현재 bio_vec 배열을 다 읽으면 이 함수를 호출해 다음
 * bio로 넘어가야 하는지 판단한다. 아직 현재 bio에 남은 bvec이 있으면
 * (iter->iter.bi_size != 0) 아무 것도 하지 않고 true를 반환해 호출자가 같은
 * bio를 계속 읽게 한다. 현재 bio를 다 읽었는데 다음 bio도 없으면 request
 * 전체를 다 순회한 것이므로 false를 반환한다. 다음 bio가 있으면 iter->bio를
 * 그 bio로 옮기고, is_integrity 플래그에 따라 데이터 영역(bi_iter/bi_io_vec)
 * 또는 integrity metadata 영역(bio_integrity()->bip_iter/bip_vec) 중 알맞은
 * 쪽으로 반복자를 재설정한다.
 * 실행 컨텍스트: blk_map_iter_next()의 내부 병합 루프에서만 호출되는 static
 * 함수이므로 별도의 잠금 없이 호출자(request를 소유한 프로세스 컨텍스트)
 * 스택 안에서 순차 실행된다. 같은 request가 이 시점에 다른 스레드와 동시에
 * 매핑되는 일은 없다고 가정한다.
 * 호출자: blk_map_iter_next() (같은 파일, 세그먼트 병합 루프 안에서만).
 * 피호출자: bio_integrity() (bio-integrity.h의 인라인 헬퍼).
 * 에러 경로: 별도의 에러 코드는 없다 - "더 이상 없음"만 false로 표현하며,
 * 호출자는 이를 정상적인 순회 종료로 해석한다.
 *
 * 호출 체인:
 *   blk_map_iter_next -> [__blk_map_iter_next] -> bio_integrity
 */
static bool __blk_map_iter_next(struct blk_map_iter *iter)
{
	if (iter->iter.bi_size)	/* [한국어] 현재 bio_vec 배열에 아직 읽지 않은 바이트가 남아 있으면(bi_size != 0) 다음 bio로 넘어갈 필요 없음 - 같은 bio를 계속 순회 */
		return true;
	if (!iter->bio || !iter->bio->bi_next)	/* [한국어] 현재 bio가 없거나(초기화 안 됨) bi_next 체인의 끝이면 더 순회할 데이터가 없음 */
		return false;

	iter->bio = iter->bio->bi_next;	/* [한국어] bi_next 체인을 따라 다음 bio로 이동 - bio merge로 여러 bio가 한 request에 매달린 경우 순차 이동 */
	if (iter->is_integrity) {	/* [한국어] integrity metadata 순회 중이면(is_integrity==true) DIF/DIX 전용 필드로 재설정 */
		iter->iter = bio_integrity(iter->bio)->bip_iter;	/* [한국어] 새 bio의 integrity bip_iter로 반복자 갱신 - 새 bio는 처음부터 순회 시작하는 상태 */
		iter->bvecs = bio_integrity(iter->bio)->bip_vec;	/* [한국어] 새 bio의 integrity bip_vec 배열 포인터로 교체 */
	} else {	/* [한국어] 일반 데이터 영역이면 표준 bi_iter/bi_io_vec으로 재설정 */
		iter->iter = iter->bio->bi_iter;	/* [한국어] 새 bio의 bvec_iter로 갱신 */
		iter->bvecs = iter->bio->bi_io_vec;	/* [한국어] 새 bio의 bio_vec 배열 포인터로 교체 */
	}
	return true;	/* [한국어] 다음 bio로 성공적으로 이동했으므로 계속 순회 가능함을 알림 */
}

/*
 * [한국어]
 * blk_map_iter_next - 반복자에서 병합된 물리 세그먼트 하나(phys_vec)를 꺼낸다
 *
 * @req:  세그먼트가 속한 request. queue_limits(req->q->limits)를 참조해 세그먼트
 *        병합 한도를 계산하는 데 쓰인다.
 * @iter: 순회 상태. 호출할 때마다 전진하며, 다음 호출 시 그 다음 세그먼트를
 *        가리키게 된다.
 * @vec:  출력 파라미터. 성공 시 vec->paddr(물리 주소)과 vec->len(병합된 길이)이
 *        채워진다.
 * @return: 꺼낼 세그먼트가 있었으면 true, iter가 이미 끝(bi_size == 0)이면 false.
 *
 * request의 데이터는 여러 bio_vec(각각 별도 물리 페이지+오프셋+길이)으로 나뉘어
 * 있는데, 이를 그대로 DMA 매핑하면 물리적으로 인접한 페이지도 매번 별도
 * 세그먼트로 취급되어 PRP/SGL entry 수가 불필요하게 늘어난다. 이 함수는 (1)
 * 현재 bio_vec 하나를 꺼내 max_segment_size로 길이를 클리핑하고, (2) while
 * 루프를 돌며 물리적으로 바로 이어지는 다음 bio_vec들을 biovec_phys_mergeable()로
 * 검사해 max_size 한도 내에서 계속 하나의 세그먼트로 합친다. bio 체인의 경계를
 * 넘는 이동은 __blk_map_iter_next()가 처리한다.
 * 실행 컨텍스트: request를 제출한 프로세스 컨텍스트에서 동기적으로 실행되며
 * 락 없이 iter/vec을 직접 갱신한다(단일 스레드 소유 가정).
 * 호출자: blk_dma_map_iter_start(), blk_rq_dma_map_iter_next(),
 *         blk_rq_integrity_dma_map_iter_next(), blk_rq_dma_map_iova()(do-while
 *         안), __blk_rq_map_sg(), blk_rq_map_integrity_sg().
 * 피호출자: mp_bvec_iter_bvec(), bvec_phys(), get_max_segment_size(),
 *          bvec_iter_advance_single(), __blk_map_iter_next(),
 *          biovec_phys_mergeable().
 * 에러 경로: 없음 - false는 "더 이상 세그먼트가 없다"는 정상 종료 신호다.
 *
 * 호출 체인:
 *   blk_dma_map_iter_start / blk_rq_dma_map_iter_next / __blk_rq_map_sg
 *   -> [blk_map_iter_next] -> __blk_map_iter_next
 */
static bool blk_map_iter_next(struct request *req, struct blk_map_iter *iter,
			      struct phys_vec *vec)
{
	unsigned int max_size;	/* [한국어] 이 세그먼트가 가질 수 있는 최대 바이트 수 - queue_limits 기준으로 계산됨 */
	struct bio_vec bv;	/* [한국어] 현재 병합 중인 임시 bio_vec - paddr 계산의 시작점이자 병합 누적 버퍼 */

	if (!iter->iter.bi_size)	/* [한국어] 반복자가 소진되었으면(더 읽을 바이트 없음) 세그먼트 없음을 알림 */
		return false;

	bv = mp_bvec_iter_bvec(iter->bvecs, iter->iter);	/* [한국어] 현재 위치의 bio_vec(페이지+오프셋+길이)을 값으로 추출 */
	vec->paddr = bvec_phys(&bv);	/* [한국어] page_to_phys(page)+offset - 이 세그먼트의 시작 물리 주소 */
	max_size = get_max_segment_size(&req->q->limits, vec->paddr, UINT_MAX);	/* [한국어] 이 물리 주소에서 seg_boundary_mask/max_segment_size를 넘지 않는 최대 길이 계산 - queue_limits가 강제하는 세그먼트 상한 */
	bv.bv_len = min(bv.bv_len, max_size);	/* [한국어] bio_vec 길이가 한도를 넘으면 잘라냄 - 초과분은 다음 호출에서 별도 세그먼트로 처리 */
	bvec_iter_advance_single(iter->bvecs, &iter->iter, bv.bv_len);	/* [한국어] 소비한 만큼 반복자 전진 - bi_bvec_done/bi_idx/bi_size 갱신 */

	/*
	 * If we are entirely done with this bi_io_vec entry, check if the next
	 * one could be merged into it.  This typically happens when moving to
	 * the next bio, but some callers also don't pack bvecs tight.
	 */
	/*
	 * [한국어] 위 원문 주석: 이 bio_vec entry를 완전히 다 썼다면(entry 경계에 딱
	 * 맞게 소비했다면) 바로 다음 entry가 물리적으로 이어붙는지 검사해 병합을
	 * 시도한다. 주로 다음 bio로 넘어갈 때 발생하지만, 일부 호출자는 bvec을
	 * 빽빽하게 채우지 않을 수도 있어 같은 bio 안에서도 일어난다.
	 * 진입 조건: bi_size==0(현재 bio_vec 배열 소진, 다음 bio 확인 필요)이거나
	 * bi_bvec_done==0(방금 entry 경계에 정확히 도달) - 두 경우 모두 "다음 물리
	 * 페이지가 이어붙는지" 검사할 시점이다.
	 */
	while (!iter->iter.bi_size || !iter->iter.bi_bvec_done) {
		struct bio_vec next;	/* [한국어] 병합 후보로 검사할 다음 bio_vec */

		if (!__blk_map_iter_next(iter))	/* [한국어] 다음 bio_vec/bio로 전진 시도 - bio 체인이 끝났으면 병합 시도 중단 */
			break;	/* [한국어] 더 병합할 대상 없음 - 루프 탈출 */

		next = mp_bvec_iter_bvec(iter->bvecs, iter->iter);	/* [한국어] 후보 bio_vec 추출(peek) - 아직 반복자는 실제로 전진시키지 않음 */
		/*
		 * [한국어] 병합 시 max_size를 넘거나, 두 bio_vec이 물리적으로 인접하지 않으면
		 * (offset+len이 다음 paddr과 불일치, seg_boundary 교차, KMSAN/Xen 특수 조건 등
		 * biovec_phys_mergeable() 내부 조건 불만족) 별도 세그먼트로 남겨야 하므로
		 * 병합을 중단한다.
		 */
		if (bv.bv_len + next.bv_len > max_size ||
		    !biovec_phys_mergeable(req->q, &bv, &next))
			break;	/* [한국어] 병합 조건 불만족 - 별도 세그먼트로 남김(루프 탈출) */

		bv.bv_len += next.bv_len;	/* [한국어] 병합 성공 - 누적 길이에 다음 entry 길이를 더함 */
		bvec_iter_advance_single(iter->bvecs, &iter->iter, next.bv_len);	/* [한국어] 병합한 만큼 실제로 반복자 전진(peek 확정) */
	}

	vec->len = bv.bv_len;	/* [한국어] 최종 병합 길이를 출력 - DMA 매핑/PRP·SGL entry 길이로 쓰임 */
	return true;	/* [한국어] 세그먼트 하나(paddr,len)를 성공적으로 채웠음을 알림 */
}

/*
 * [한국어] struct phys_vec 필드 설명 (정의: include/linux/types.h)
 *
 * 이 구조체는 blk_map_iter_next()가 계산한 "하나의 연속된 물리 메모리 범위"를
 * 표현하며, 이후 DMA 매핑 함수들의 입력으로만 쓰이는 값 타입(스택에 있는 지역
 * 변수로 복사되어 쓰인다)이다.
 *
 * phys_addr_t paddr;
 *   이 세그먼트의 시작 물리 주소.
 *   설정자: blk_map_iter_next()가 bvec_phys(&bv)로 계산해 채운다.
 *   읽는 자: blk_dma_map_bus()/blk_dma_map_direct()/blk_rq_dma_map_iova()가 이
 *     값을 dma_map_phys()/dma_iova_link()/pci_p2pdma_bus_addr_map()에 그대로
 *     전달해 실제 DMA/bus 주소로 변환한다. phys_to_page(vec.paddr) 형태로
 *     struct page를 역산해 P2P 페이지 여부 판별이나 scatterlist 구성(sg_set_page)
 *     에도 쓰인다.
 *   값 범위: 유효한 물리 주소(아키텍처의 phys_addr_t 폭 내). 페이지+오프셋에서
 *     계산되므로 통상 페이지 정렬은 보장되지 않는다(offset 포함 가능).
 *   동기화: 스택 지역 변수라 별도 동기화가 필요 없다 - 한 프로세스 컨텍스트
 *     안에서만 생성/소비된다.
 *
 * size_t len;
 *   paddr부터 시작해 DMA로 다룰 바이트 수. get_max_segment_size()와
 *   biovec_phys_mergeable()에 의해 여러 bio_vec이 병합된 결과일 수 있다.
 *   설정자: blk_map_iter_next()가 병합 루프 종료 시 vec->len = bv.bv_len으로
 *     기록한다.
 *   읽는 자: 각 DMA 매핑 함수가 dma_map_phys()/dma_iova_link()의 size 인자로
 *     사용하며, __blk_rq_map_sg()/blk_rq_map_integrity_sg()는 sg_set_page()의
 *     length 인자로 사용한다(단 그 전에 overflows_type()으로 unsigned int
 *     범위를 넘지 않는지 확인한다).
 *   값 범위: 1 이상, get_max_segment_size()가 반환하는 queue 세그먼트 한도
 *     이하(이 파일의 호출에서는 UINT_MAX를 len 인자로 넘기므로 실질적으로
 *     max_segment_size가 상한).
 *   동기화: paddr와 동일하게 지역 변수 수준의 값이라 동기화 불필요.
 */
/*
 * The IOVA-based DMA API wants to be able to coalesce at the minimal IOMMU page
 * size granularity (which is guaranteed to be <= PAGE_SIZE and usually 4k), so
 * we need to ensure our segments are aligned to this as well.
 *
 * Note that there is no point in using the slightly more complicated IOVA based
 * path for single segment mappings.
 */
/*
 * [한국어] (위 원문 주석 번역) IOVA 기반 DMA API는 IOMMU의 최소 페이지 크기
 * (PAGE_SIZE 이하, 보통 4KB) 단위로 세그먼트를 이어붙이려 하므로, 우리
 * 세그먼트들도 그 경계에 맞춰 정렬되어 있어야 한다. 그리고 세그먼트가 하나뿐인
 * 매핑에는 (뒤에서 볼) 좀 더 복잡한 IOVA 경로를 쓸 이유가 없다.
 *
 * [한국어]
 * blk_can_dma_map_iova - 이 request에 IOVA 기반 DMA 매핑을 써도 안전한지 검사
 *
 * @req:     검사 대상 request. req->phys_gap_bit(요청 내 세그먼트들이 공통으로
 *           보장하는 최소 정렬 비트)를 req_phys_gap_mask()를 통해 사용한다.
 * @dma_dev: DMA 대상 장치. dma_get_merge_boundary()로 이 장치(또는 그 뒤의
 *           IOMMU)가 세그먼트를 병합할 때 요구하는 경계 마스크를 얻는다.
 * @return:  true면 IOVA 기반 병합 경로(blk_rq_dma_map_iova)를 시도해도 안전,
 *           false면 세그먼트별 직접 매핑(blk_dma_map_direct)만 써야 함.
 *
 * req_phys_gap_mask(req)는 이 request 안의 모든 세그먼트 경계에서 공통으로
 * 보장되는 정렬 비트 이상을 1로 채운 마스크를 반환한다(blk-mq.h 참고: "Returns
 * a mask with all bits starting at req->phys_gap_bit set to 1"). dma_get_merge_
 * boundary(dma_dev)는 IOMMU(또는 dma_map_ops)가 세그먼트를 하나의 IOVA 범위로
 * 병합할 때 요구하는 경계 마스크다(예: iommu_dma_get_merge_boundary()). 두
 * 마스크에 겹치는 비트가 있다는 것은 request가 보장하는 정렬 단위가 IOMMU의
 * 병합 요구 경계보다 거칠다는(정렬이 어긋날 수 있다는) 뜻이므로, 겹치는 비트가
 * 하나라도 있으면 IOVA 병합의 안전성을 보장할 수 없어 false를 반환한다.
 * 실행 컨텍스트: blk_dma_map_iter_start()가 첫 세그먼트를 얻은 직후, 매핑 경로를
 * 고르는 순간에만 한 번 호출되는 순수 계산 함수(부수효과 없음).
 * 호출자: blk_dma_map_iter_start().
 * 피호출자: req_phys_gap_mask()(include/linux/blk-mq.h 인라인),
 *          dma_get_merge_boundary()(kernel/dma/mapping.c, IOMMU가 있으면
 *          iommu_dma_get_merge_boundary()로 위임, 없으면 0을 반환해 병합 불가로
 *          처리).
 * 에러 경로: 없음 - 항상 정상적으로 bool을 반환하는 순수 판별 함수.
 *
 * 호출 체인:
 *   blk_dma_map_iter_start -> [blk_can_dma_map_iova] -> req_phys_gap_mask
 *                                                      -> dma_get_merge_boundary
 */
static inline bool blk_can_dma_map_iova(struct request *req,
		struct device *dma_dev)
{
	return !(req_phys_gap_mask(req) & dma_get_merge_boundary(dma_dev));	/* [한국어] request가 보장하는 정렬 마스크와 IOMMU가 요구하는 병합 경계 마스크가 겹치지 않아야(AND==0) IOVA 병합을 안전하게 쓸 수 있음 - 겹치면 false로 direct 매핑 강제 */
}

/*
 * [한국어] struct blk_dma_iter 필드 설명 (정의: include/linux/blk-mq-dma.h)
 *
 * blk_rq_dma_map_iter_start()가 호출자 스택에 만들어 채우고, 이후
 * blk_rq_dma_map_iter_next()를 반복 호출할 때도 같은 인스턴스를 넘겨 순회 상태를
 * 유지하는 "출력 + 내부상태" 겸용 구조체다.
 *
 * dma_addr_t addr;
 *   이번 호출에서 매핑된 세그먼트의 DMA(버스 또는 IOVA) 시작 주소.
 *   설정자: blk_dma_map_bus()(P2P bus addr), blk_dma_map_direct()
 *     (dma_map_phys() 반환값), blk_rq_dma_map_iova()(state->addr, IOVA 영역
 *     시작 주소) 중 실제 선택된 경로가 채운다.
 *   읽는 자: 이 함수들의 호출자(NVMe 등 블록 드라이버)가 PRP1/PRP2 또는 SGL
 *     descriptor의 주소 필드에 그대로 기록한다.
 *   값 범위: 유효한 DMA 주소. 매핑 실패 시에는 의미가 없으며 대신 status를
 *     확인해야 한다.
 *   동기화: 호출자가 소유한 스택/힙 메모리이므로 별도 락 불필요 - 한 request는
 *     한 번에 한 컨텍스트에서만 매핑된다.
 *
 * u32 len;
 *   addr부터 매핑된 바이트 수(세그먼트 하나의 phys_vec.len, 또는 IOVA 병합
 *   시에는 dma_iova_size()로 계산된 IOVA 전체 길이).
 *   설정자: 위와 동일한 3개 매핑 함수.
 *   읽는 자: 드라이버가 descriptor의 length 필드로 사용.
 *   값 범위: 1 이상, 세그먼트 하나 기준으로는 max_segment_size 이하지만 IOVA
 *     병합 시에는 여러 세그먼트 합산 길이라 더 클 수 있다.
 *   동기화: addr과 동일.
 *
 * struct pci_p2pdma_map_state p2pdma;
 *   PCI P2PDMA(peer-to-peer DMA) 판별 결과 - { struct p2pdma_provider *mem;
 *   enum pci_p2pdma_map_type map; } (정의: include/linux/pci-p2pdma.h).
 *   설정자: blk_dma_map_iter_start()가 memset으로 초기화한 뒤 pci_p2pdma_state()
 *     로 채운다.
 *   읽는 자: blk_rq_dma_map_iter_next()/blk_rq_integrity_dma_map_iter_next()가
 *     map 필드를 보고 이후 세그먼트들도 같은 P2P 경로(bus address)를 타야
 *     하는지 판단한다.
 *   값 범위: map은 PCI_P2PDMA_MAP_NONE/BUS_ADDR/THRU_HOST_BRIDGE/
 *     NOT_SUPPORTED/UNKNOWN 중 하나.
 *   동기화: request 단위로만 쓰이는 값이라 락 불필요.
 *
 * blk_status_t status;
 *   매핑 실패 시의 블록 계층 에러 코드. blk_rq_dma_map_iter_start/next 등이
 *   false를 반환했을 때만 유효한 값을 가진다.
 *   설정자: blk_dma_map_direct()(BLK_STS_RESOURCE), blk_dma_map_iter_start()
 *     (BLK_STS_INVAL, P2P 상태 오류), blk_rq_dma_map_iova()(errno_to_blk_status()
 *     로 변환).
 *   읽는 자: 호출자가 false 반환 시 이 필드를 읽어 request를 어떤 blk_status_t로
 *     완료시킬지 결정한다(예: BLK_STS_RESOURCE -> 재시큐 유도).
 *   값 범위: BLK_STS_OK를 포함한 blk_status_t 전체 값 공간.
 *
 * struct blk_map_iter iter;
 *   이 반복자 내부에서만 쓰는 하위 순회 상태(bio/bvecs/bvec_iter/is_integrity).
 *   외부 호출자는 직접 건드리지 않고 blk_rq_dma_map_iter_start()가 초기화한다.
 *   설정자/읽는 자: blk_map_iter_next()/__blk_map_iter_next()가 전용으로
 *     갱신·소비한다.
 */

/*
 * [한국어]
 * blk_dma_map_bus - PCI P2PDMA bus address 경로로 세그먼트를 매핑
 *
 * @iter: p2pdma.mem(P2P 메모리 제공자 정보)을 이미 채워둔 반복자. 결과인
 *        addr/len이 이 iter에 기록된다.
 * @vec:  매핑할 물리 세그먼트(paddr, len).
 * @return: 이 경로는 실패할 수 없으므로 항상 true.
 *
 * 두 PCI 장치가 host bridge(그리고 그 뒤의 IOMMU/CPU 메모리 컨트롤러)를 거치지
 * 않고 PCI 스위치를 통해 직접 통신할 수 있는 경우(PCI_P2PDMA_MAP_BUS_ADDR),
 * 물리 주소를 CPU 관점이 아니라 PCI bus 관점의 주소로 변환해야 한다.
 * pci_p2pdma_bus_addr_map()은 paddr에 provider->bus_offset을 더해 이 변환을
 * 수행한다(dma_map_phys()/IOMMU를 거치지 않음 - 애초에 host bridge를 지나지
 * 않는 전송이기 때문).
 * 실행 컨텍스트: 매핑을 요청한 프로세스 컨텍스트, 락 없음.
 * 호출자: blk_dma_map_iter_start(), blk_rq_dma_map_iter_next(),
 *         blk_rq_integrity_dma_map_iter_next() - 모두 iter->p2pdma.map ==
 *         PCI_P2PDMA_MAP_BUS_ADDR일 때만 이 함수를 선택한다.
 * 피호출자: pci_p2pdma_bus_addr_map()(include/linux/pci-p2pdma.h 인라인).
 * 에러 경로: 없음 - bus address 계산은 실패할 수 없는 순수 산술 연산.
 *
 * 호출 체인:
 *   blk_dma_map_iter_start / blk_rq_dma_map_iter_next
 *   -> [blk_dma_map_bus] -> pci_p2pdma_bus_addr_map
 */
static bool blk_dma_map_bus(struct blk_dma_iter *iter, struct phys_vec *vec)
{
	iter->addr = pci_p2pdma_bus_addr_map(iter->p2pdma.mem, vec->paddr);	/* [한국어] paddr + bus_offset = PCI bus에서 본 주소 - peer 장치가 이 주소로 DMA */
	iter->len = vec->len;	/* [한국어] 매핑 길이는 물리 세그먼트 길이 그대로 - 변환 과정에 크기 변화 없음 */
	return true;	/* [한국어] bus address 변환은 실패 케이스가 없으므로 항상 성공 보고 */
}

/*
 * [한국어]
 * blk_dma_map_direct - 단일 물리 세그먼트를 dma_map_phys()로 직접 DMA 매핑
 *
 * @req:     매핑 방향(rq_dma_dir)을 얻기 위한 request.
 * @dma_dev: DMA 대상 장치.
 * @iter:    p2pdma.map 값을 참조해 MMIO 속성 여부를 판단하고, 결과(addr/len)
 *           또는 실패 코드(status)를 기록할 반복자.
 * @vec:     매핑할 물리 세그먼트(paddr, len).
 * @return:  매핑 성공 시 true(iter->addr/len 유효), 실패 시 false
 *           (iter->status == BLK_STS_RESOURCE).
 *
 * IOVA 경로를 쓸 수 없거나(정렬 불일치, IOMMU 없음 등) 아직 dma_iova_try_alloc()
 * 을 시도하지 않은 세그먼트를, 세그먼트 단위로 하나씩 dma_map_phys()에 넘겨
 * 매핑하는 가장 단순한 경로다. P2P 전송이 host bridge를 거치는 경우
 * (PCI_P2PDMA_MAP_THRU_HOST_BRIDGE)에는 대상 주소가 일반 캐시 가능 시스템
 * 메모리가 아니라 MMIO(BAR) 영역이므로 DMA_ATTR_MMIO 속성을 추가해 캐시 동기화를
 * 건너뛰도록 dma_map_phys()에 알린다. dma_mapping_error()로 반환된 주소가
 * DMA_MAPPING_ERROR 특수값인지 확인해 실패를 판별하고, 실패하면
 * BLK_STS_RESOURCE(자원 부족 - 상위 계층이 request를 재시도/재큐잉할 수 있는
 * 코드)를 기록한다.
 * 실행 컨텍스트: 매핑 호출자의 프로세스 컨텍스트, request/세그먼트당 최대 한 번
 * 호출.
 * 호출자: blk_dma_map_iter_start(), blk_rq_dma_map_iter_next(),
 *         blk_rq_integrity_dma_map_iter_next() - 모두 P2P bus-address 경로가
 *         아닐 때 선택.
 * 피호출자: dma_map_phys()(kernel/dma/mapping.c, 아키텍처의 dma_map_ops 또는
 *          IOMMU 도메인에 위임), dma_mapping_error(), rq_dma_dir().
 * 에러 경로: dma_map_phys() 실패 시 iter->status = BLK_STS_RESOURCE 후 false
 *          반환 - 호출자가 이 값을 그대로 request 완료 코드로 전파한다.
 *
 * 호출 체인:
 *   blk_dma_map_iter_start / blk_rq_dma_map_iter_next
 *   -> [blk_dma_map_direct] -> dma_map_phys
 */
static bool blk_dma_map_direct(struct request *req, struct device *dma_dev,
		struct blk_dma_iter *iter, struct phys_vec *vec)
{
	unsigned int attrs = 0;	/* [한국어] dma_map_phys()에 넘길 매핑 속성 플래그 - 기본은 0(일반 캐시 가능 메모리) */

	if (iter->p2pdma.map == PCI_P2PDMA_MAP_THRU_HOST_BRIDGE)	/* [한국어] host bridge를 거치는 P2P는 대상이 일반 RAM이 아니라 MMIO(BAR) 영역 - 캐시 동기화를 건너뛰도록 지시 필요 */
		attrs |= DMA_ATTR_MMIO;	/* [한국어] MMIO 속성 추가 - dma_map_phys가 이 주소를 cacheable로 매핑하지 않도록 함 */

	iter->addr = dma_map_phys(dma_dev, vec->paddr, vec->len,	/* [한국어] 물리 주소 -> 실제 DMA/버스 주소 변환 수행 (IOMMU 있으면 그 도메인에, 없으면 dma-direct 구현으로) */
			rq_dma_dir(req), attrs);	/* [한국어] 방향(rq_dma_dir: 쓰기=DMA_TO_DEVICE, 읽기=DMA_FROM_DEVICE)과 속성 전달 */
	if (dma_mapping_error(dma_dev, iter->addr)) {	/* [한국어] DMA_MAPPING_ERROR 특수값과 비교해 매핑 실패 여부 확인 */
		iter->status = BLK_STS_RESOURCE;	/* [한국어] 리소스 부족 등으로 매핑 실패 - 블록 계층 에러 코드 기록 */
		return false;	/* [한국어] 실패 반환 - 이 request는 이후 재시도/재큐잉 대상이 될 수 있음 */
	}
	iter->len = vec->len;	/* [한국어] 매핑 성공 - 세그먼트 길이를 그대로 기록 */
	return true;	/* [한국어] 성공 반환 */
}

/*
 * [한국어]
 * blk_rq_dma_map_iova - request의 모든 남은 세그먼트를 하나의 IOVA 범위에 매핑
 *
 * @req:     매핑할 request. rq_dma_dir()로 DMA 방향을 얻는 데 쓰인다.
 * @dma_dev: DMA 대상 장치(IOMMU 도메인을 가진 장치여야 dma_iova_try_alloc()이
 *           성공했을 것이다).
 * @state:   blk_dma_map_iter_start()에서 dma_iova_try_alloc()으로 이미 IOVA
 *           공간을 할당받은 상태. addr에 시작 IOVA가 들어 있다.
 * @iter:    출력 - addr/len에 이번에 매핑된 IOVA 범위 전체가 기록된다.
 * @vec:     호출 시점에는 이미 꺼내 둔 첫 세그먼트. do-while 루프 안에서
 *           blk_map_iter_next()가 다음 세그먼트로 갱신해 나간다.
 * @return:  true면 request의 남은 모든 세그먼트를 하나의 IOVA 범위로 매핑
 *           완료, false면 도중에 실패해 이미 링크된 부분을 롤백함.
 *
 * dma_iova_try_alloc()이 이미 request 전체 payload 크기만큼 연속된 IOVA 공간을
 * 예약해 두었으므로, 이 함수는 물리적으로 흩어진 세그먼트들을 dma_iova_link()로
 * 그 IOVA 공간의 서로 다른 offset에 순서대로 연결하기만 하면 된다.
 * dma_iova_link()는 매번 IOTLB(IOMMU TLB)를 동기화하지 않는 "빠른" 링크만
 * 수행하므로, 모든 세그먼트를 다 연결한 뒤 마지막에 dma_iova_sync() 한 번으로
 * IOTLB 동기화 비용을 상각한다. 중간에 dma_iova_link()가 실패하면(예: IOVA
 * 공간 부족) out_unlink 레이블로 점프해 지금까지 링크된 [0, mapped) 구간만
 * dma_iova_destroy()로 되돌리고 errno를 blk_status_t로 변환해 iter->status에
 * 기록한다.
 * 실행 컨텍스트: 매핑을 요청한 프로세스 컨텍스트, request당 한 번만 실행(재진입
 * 하지 않음). do-while 루프 안에서 blk_map_iter_next()를 반복 호출하므로 이
 * 함수 하나가 남은 세그먼트를 전부 소비한다 - 호출자는 (P2P bus address 경로와
 * 달리) 이후 blk_rq_dma_map_iter_next()를 호출할 필요가 없다
 * (blk_rq_dma_map_coalesce()로 이 사실을 확인 가능 - include/linux/blk-mq-dma.h
 * 에 정의, 이 파일 범위 밖).
 * 호출자: blk_dma_map_iter_start() (blk_can_dma_map_iova() &&
 *         dma_iova_try_alloc()이 모두 참일 때만).
 * 피호출자: rq_dma_dir(), dma_iova_size(), dma_iova_link(), dma_iova_sync(),
 *          blk_map_iter_next(), dma_iova_destroy(), errno_to_blk_status().
 * 에러 경로: dma_iova_link()/dma_iova_sync() 실패 시 out_unlink에서
 *          dma_iova_destroy()로 부분 매핑을 해제하고 false 반환.
 *
 * 호출 체인:
 *   blk_dma_map_iter_start -> [blk_rq_dma_map_iova] -> dma_iova_link/sync/destroy
 */
static bool blk_rq_dma_map_iova(struct request *req, struct device *dma_dev,
		struct dma_iova_state *state, struct blk_dma_iter *iter,
		struct phys_vec *vec)
{
	enum dma_data_direction dir = rq_dma_dir(req);	/* [한국어] 쓰기면 DMA_TO_DEVICE, 읽기면 DMA_FROM_DEVICE - IOMMU 매핑 방향 결정 */
	unsigned int attrs = 0;	/* [한국어] dma_iova_link()에 넘길 매핑 속성 플래그 - 기본 0 */
	size_t mapped = 0;	/* [한국어] 지금까지 IOVA 공간에 링크한 누적 바이트 수 - sync/destroy 범위 계산에 사용 */
	int error;	/* [한국어] dma_iova_link()/dma_iova_sync()의 errno 반환값 임시 저장 */

	iter->addr = state->addr;	/* [한국어] dma_iova_try_alloc()이 예약해 둔 IOVA 시작 주소 - 이후 PRP1/SGL pointer로 쓰일 값 */
	iter->len = dma_iova_size(state);	/* [한국어] 예약된 IOVA 전체 크기(스왑IOTLB 사용 플래그 비트는 제외한 실제 크기) */

	if (iter->p2pdma.map == PCI_P2PDMA_MAP_THRU_HOST_BRIDGE)	/* [한국어] host bridge를 거치는 P2P는 대상이 MMIO 영역이므로 캐시 동기화 회피 필요 */
		attrs |= DMA_ATTR_MMIO;	/* [한국어] MMIO 속성 추가 */

	/*
	 * [한국어] 첫 세그먼트(vec)부터 시작해 request의 남은 모든 세그먼트를 순서대로
	 * IOVA state에 링크한다. IOTLB 동기화는 각 링크마다 하지 않고 루프가 끝난 뒤
	 * 한 번만 수행해 비용을 줄인다.
	 */
	do {
		error = dma_iova_link(dma_dev, state, vec->paddr, mapped,	/* [한국어] 현재 phys_vec을 IOVA state의 mapped 오프셋 위치에 연결 시도 */
				vec->len, dir, attrs);	/* [한국어] vec->len 만큼, IOTLB 동기화는 아직 하지 않음(비용 절감) */
		if (error)
			goto out_unlink;	/* [한국어] 링크 실패 - 지금까지 링크된 부분만 되돌리러 점프 */
		mapped += vec->len;	/* [한국어] 누적 매핑 길이 갱신 - 다음 링크의 offset이자 최종 sync/destroy 길이 */
	} while (blk_map_iter_next(req, &iter->iter, vec));	/* [한국어] 다음 물리 세그먼트로 vec 갱신 - 더 없으면(false) 루프 종료, request의 마지막 세그먼트까지 반복 */

	error = dma_iova_sync(dma_dev, state, 0, mapped);	/* [한국어] 지금까지 링크한 [0,mapped) 전체에 대해 한 번만 IOTLB 동기화 수행 */
	if (error)
		goto out_unlink;	/* [한국어] 동기화 실패 - 이미 링크된 매핑을 모두 되돌림 */

	return true;	/* [한국어] request의 모든 남은 세그먼트가 하나의 IOVA 범위로 매핑 완료 */

out_unlink:
	dma_iova_destroy(dma_dev, state, mapped, dir, attrs);	/* [한국어] 부분적으로 링크된 IOVA 범위를 해제하고 예약된 IOVA 공간 자체도 반환 */
	iter->status = errno_to_blk_status(error);	/* [한국어] IOMMU errno(-ENOMEM 등)를 블록 계층 blk_status_t로 변환해 보고 */
	return false;	/* [한국어] 매핑 실패를 호출자에 알림 - 호출자는 iter->status로 원인 파악 */
}

/*
 * [한국어]
 * blk_rq_map_iter_init - request로부터 데이터 반복자(blk_map_iter)를 초기화
 *
 * @rq:   초기화 기준이 되는 request.
 * @iter: 출력 - 이 request의 데이터 순회를 시작할 수 있는 초기 상태로 채워짐.
 * @return: 없음(void) - iter를 직접 채워서 결과를 돌려준다.
 *
 * request가 데이터를 담는 방식은 세 가지다. (1) RQF_SPECIAL_PAYLOAD 플래그가
 * 있으면 일반 bio가 아니라 드라이버가 직접 채운 rq->special_vec 하나만
 * 존재한다. (2) 일반적인 경우 rq->bio가 있고, bio->bi_io_vec/bi_iter로부터
 * 순회를 시작한다(bio 체인의 첫 bio만 여기서 초기화하고, 이후
 * __blk_map_iter_next()가 bi_next를 따라 다음 bio로 넘어간다). (3) 내부적으로
 * 생성되는 flush request는 아예 bio가 없을 수 있으므로 빈 반복자({}는 모든
 * 필드가 0/NULL - 특히 iter.bi_size == 0)로 초기화해 첫 blk_map_iter_next()
 * 호출이 즉시 false를 반환하게 한다.
 * 실행 컨텍스트: 매핑을 시작하는 호출자의 프로세스 컨텍스트, request당 한 번만
 * 호출됨(반복자 순회 시작점).
 * 호출자: blk_rq_dma_map_iter_start(), __blk_rq_map_sg().
 * 피호출자: 없음(단순 구조체 대입만 수행).
 * 에러 경로: 없음 - 실패할 수 없는 초기화 함수.
 *
 * 호출 체인:
 *   blk_rq_dma_map_iter_start / __blk_rq_map_sg -> [blk_rq_map_iter_init]
 */
static inline void blk_rq_map_iter_init(struct request *rq,
					struct blk_map_iter *iter)
{
	struct bio *bio = rq->bio;	/* [한국어] request에 매달린 첫 bio - NULL일 수 있음(내부 flush request) */

	if (rq->rq_flags & RQF_SPECIAL_PAYLOAD) {	/* [한국어] 드라이버가 bio 없이 직접 채운 특수 payload(RQF_SPECIAL_PAYLOAD) - 예: 일부 discard/내부 명령 페이로드 */
		*iter = (struct blk_map_iter) {	/* [한국어] RQF_SPECIAL_PAYLOAD 전용 반복자 리터럴 생성 시작 */
			.bvecs = &rq->special_vec,	/* [한국어] bio_vec 배열 대신 special_vec 단일 엔트리를 배열처럼 취급 */
			.iter = {	/* [한국어] 내부 bvec_iter를 별도로 초기화 - bi_size만 지정하고 나머지(bi_idx 등)는 0 */
				.bi_size = rq->special_vec.bv_len,	/* [한국어] 순회할 전체 크기 - special_vec 하나의 길이 */
			}
		};
	} else if (bio) {	/* [한국어] 일반적인 I/O 경로 - 첫 bio의 bi_io_vec/bi_iter에서 순회 시작 */
		*iter = (struct blk_map_iter) {	/* [한국어] 일반 bio 기반 반복자 리터럴 생성 시작 */
			.bio = bio,	/* [한국어] bio 체인 순회의 시작점 - __blk_map_iter_next가 bi_next로 이어감 */
			.bvecs = bio->bi_io_vec,	/* [한국어] 이 bio의 bio_vec 배열 포인터 */
			.iter = bio->bi_iter,	/* [한국어] 이 bio의 현재 bvec_iter 상태(보통 처음부터: bi_idx=0 등) */
		};
	} else {	/* [한국어] 원문 주석대로 내부 flush request는 bio가 붙지 않을 수 있음 */
		/* the internal flush request may not have bio attached */
		*iter = (struct blk_map_iter) {};	/* [한국어] 모든 필드 0/NULL로 초기화 - bi_size==0이라 첫 호출에서 바로 순회 종료 */
	}
}

/*
 * [한국어]
 * blk_dma_map_iter_start - 첫 세그먼트를 얻고 P2P/IOVA 여부에 따라 매핑 경로를 고정
 *
 * @req:      매핑할 request.
 * @dma_dev:  DMA 대상 장치.
 * @state:    IOVA 상태 - IOVA 경로를 타면 여기 예약 정보가 채워지고, 아니면
 *            memset으로 완전히 0으로 만들어 "IOVA 미사용"임을 표시한다
 *            (dma_use_iova()가 이후 false를 반환하게 됨).
 * @iter:     출력 - p2pdma 상태와 status, 그리고 매핑된 첫 세그먼트의 addr/len이
 *            기록된다.
 * @total_len: 이 request(또는 integrity 데이터)의 총 payload 바이트 수 -
 *            dma_iova_try_alloc()이 미리 예약할 IOVA 공간 크기로 쓰인다.
 * @return: 매핑할 세그먼트가 없거나 오류가 나면 false(iter->status에 원인),
 *          첫 세그먼트를 성공적으로 매핑했으면 true.
 *
 * blk_rq_dma_map_iter_start()와 blk_rq_integrity_dma_map_iter_start() 양쪽 모두가
 * 위임하는 공통 구현이다. 먼저 iter->p2pdma/status를 초기값으로 리셋하고,
 * blk_map_iter_next()로 request의 첫 물리 세그먼트를 최대한 빨리 확보한다 - P2P
 * 여부 판별에 그 세그먼트의 struct page가 필요하기 때문이다(원문 주석 참고).
 * pci_p2pdma_state()가 그 페이지를 보고 P2P 종류를 판별하면: BUS_ADDR이면 즉시
 * blk_dma_map_bus()로 처리하고, NONE 또는 THRU_HOST_BRIDGE(호스트 브리지를
 * 넘어가는 P2P는 이후 일반 경로와 동일하게 취급 - 원문 주석 참고)면 계속
 * 진행하며, 그 외 값(NOT_SUPPORTED 등)이면 BLK_STS_INVAL로 실패시킨다. P2P가
 * 아니라면 blk_can_dma_map_iova()로 이 request의 정렬이 IOVA 병합과 맞는지
 * 보고, 맞으면서 dma_iova_try_alloc()이 total_len만큼 IOVA 공간을 성공적으로
 * 예약했을 때만 blk_rq_dma_map_iova()(전체 세그먼트를 한 번에 처리)로 넘어가고,
 * 그렇지 않으면 state를 완전히 0으로 지운 뒤 blk_dma_map_direct()(세그먼트
 * 하나만 처리, 나머지는 blk_rq_dma_map_iter_next()가 이어서 처리)로 넘어간다.
 * 실행 컨텍스트: 매핑 시작 시 한 번만 호출되는 프로세스 컨텍스트 함수.
 * 호출자: blk_rq_dma_map_iter_start(), blk_rq_integrity_dma_map_iter_start().
 * 피호출자: blk_map_iter_next(), pci_p2pdma_state(), phys_to_page(),
 *          blk_dma_map_bus(), blk_can_dma_map_iova(), dma_iova_try_alloc(),
 *          blk_rq_dma_map_iova(), blk_dma_map_direct().
 * 에러 경로: 세그먼트 없음(false, status는 호출 전 BLK_STS_OK 그대로), P2P
 *          상태가 알 수 없는 값이면 BLK_STS_INVAL, 그 외 오류는
 *          blk_rq_dma_map_iova()/blk_dma_map_direct() 내부에서 설정.
 *
 * 호출 체인:
 *   blk_rq_dma_map_iter_start / blk_rq_integrity_dma_map_iter_start
 *   -> [blk_dma_map_iter_start] -> blk_dma_map_bus / blk_rq_dma_map_iova
 *                                  / blk_dma_map_direct
 */
static bool blk_dma_map_iter_start(struct request *req, struct device *dma_dev,
		struct dma_iova_state *state, struct blk_dma_iter *iter,
		unsigned int total_len)
{
	struct phys_vec vec;	/* [한국어] blk_map_iter_next()로 꺼낼 첫 물리 세그먼트를 담을 임시 변수 */

	memset(&iter->p2pdma, 0, sizeof(iter->p2pdma));	/* [한국어] p2pdma 상태를 깨끗이 초기화 - 이전 매핑의 잔여값 방지 */
	iter->status = BLK_STS_OK;	/* [한국어] 성공을 기본값으로 설정 - 실패 시에만 아래에서 덮어씀 */
	iter->p2pdma.map = PCI_P2PDMA_MAP_NONE;	/* [한국어] P2P 판별 전 기본값 - 일반 host memory DMA로 간주 */

	/*
	 * Grab the first segment ASAP because we'll need it to check for P2P
	 * transfers.
	 */
	/*
	 * [한국어] 위 원문 주석: P2P 전송 여부를 판별하려면 첫 세그먼트의 물리 페이지가
	 * 필요하므로, 가능한 한 빨리 첫 세그먼트를 확보해 둔다.
	 */
	if (!blk_map_iter_next(req, &iter->iter, &vec))
		return false;	/* [한국어] 매핑할 데이터가 전혀 없음(zero-length 요청 등) - status는 BLK_STS_OK인 채로 false만 반환 */

	/*
	 * [한국어] 첫 세그먼트가 P2P(peer-to-peer) 메모리를 가리키는지, 가리킨다면 어떤
	 * 방식으로 처리해야 하는지 판별한다.
	 */
	switch (pci_p2pdma_state(&iter->p2pdma, dma_dev,
				 phys_to_page(vec.paddr))) {	/* [한국어] phys_to_page(vec.paddr)로 물리 주소를 struct page*로 역산해 P2P provider 소속 여부 판별 */
	case PCI_P2PDMA_MAP_BUS_ADDR:	/* [한국어] 두 장치가 PCI 스위치로 직접 연결되어 host bridge를 거치지 않는 경우 */
		return blk_dma_map_bus(iter, &vec);	/* [한국어] host bridge를 거치지 않는 순수 P2P - bus address 변환만 하면 됨 */
	case PCI_P2PDMA_MAP_THRU_HOST_BRIDGE:	/* [한국어] host bridge를 통과하는 P2P - 아래 원문 설명대로 이후 일반 경로와 동일 취급(fallthrough) */
		/*
		 * P2P transfers through the host bridge are treated the
		 * same as non-P2P transfers below and during unmap.
		 */
	case PCI_P2PDMA_MAP_NONE:	/* [한국어] P2P가 아닌 일반 전송 */
		break;	/* [한국어] P2P가 아니거나 host-bridge 경유 P2P - 아래 일반 IOVA/direct 판단으로 진행 */
	default:	/* [한국어] PCI_P2PDMA_MAP_NOT_SUPPORTED 등 매핑 불가 상태 */
		iter->status = BLK_STS_INVAL;	/* [한국어] 잘못된 요청으로 보고 - 호출자가 이 request를 즉시 에러 완료시킴 */
		return false;	/* [한국어] 실패 반환 */
	}

	/*
	 * [한국어] IOVA 정렬 조건을 만족하고, total_len만큼의 연속 IOVA 공간을 실제로
	 * 예약할 수 있을 때만 "전체를 한 번에" 매핑하는 IOVA 경로를 사용한다.
	 */
	if (blk_can_dma_map_iova(req, dma_dev) &&
	    dma_iova_try_alloc(dma_dev, state, vec.paddr, total_len))
		return blk_rq_dma_map_iova(req, dma_dev, state, iter, &vec);	/* [한국어] IOVA 조건 충족 - 전체 세그먼트를 한 번에 매핑하는 경로로 위임 */
	memset(state, 0, sizeof(*state));	/* [한국어] IOVA를 쓰지 않기로 했으므로 state를 완전히 0으로 만들어 dma_use_iova()가 이후 항상 false 반환하게 함(unmap 분기용) */
	return blk_dma_map_direct(req, dma_dev, iter, &vec);	/* [한국어] 첫 세그먼트 하나만 직접 매핑 - 나머지는 iter_next()가 이어서 처리 */
}

/**
 * blk_rq_dma_map_iter_start - map the first DMA segment for a request
 * @req:	request to map
 * @dma_dev:	device to map to
 * @state:	DMA IOVA state
 * @iter:	block layer DMA iterator
 *
 * Start DMA mapping @req to @dma_dev.  @state and @iter are provided by the
 * caller and don't need to be initialized.  @state needs to be stored for use
 * at unmap time, @iter is only needed at map time.
 *
 * Returns %false if there is no segment to map, including due to an error, or
 * %true ft it did map a segment.
 *
 * If a segment was mapped, the DMA address for it is returned in @iter.addr and
 * the length in @iter.len.  If no segment was mapped the status code is
 * returned in @iter.status.
 *
 * The caller can call blk_rq_dma_map_coalesce() to check if further segments
 * need to be mapped after this, or go straight to blk_rq_dma_map_iter_next()
 * to try to map the following segments.
 */
/*
 * [한국어] (위 kerneldoc 원문 번역 및 보강)
 * blk_rq_dma_map_iter_start - request에 대한 DMA 매핑을 시작해 첫 세그먼트를 매핑
 *
 * @req:     매핑할 request. blk_rq_payload_bytes(req)로 계산한 전체 payload
 *           길이가 IOVA 예약 크기로 쓰인다.
 * @dma_dev: 매핑 대상 장치(NVMe라면 컨트롤러의 PCI device, pci_dev->dev).
 * @state:   DMA IOVA 상태 - 호출자가 초기화할 필요 없이 이 함수가 채운다.
 *           unmap 시점(blk_rq_dma_unmap())까지 호출자가 보관해 두어야 한다.
 * @iter:    블록 계층 DMA 반복자 - map 단계에서만 필요하고 unmap 시에는 쓰이지
 *           않는다.
 * @return:  매핑할 세그먼트가 없거나(빈 request) 오류가 있으면 false, 첫
 *           세그먼트를 매핑했으면 true.
 *
 * 매핑에 성공하면 iter->addr/iter->len에 DMA 주소와 길이가 채워지고, 실패
 * 시에는 iter->status에 원인이 채워진다. 이는 request의 데이터 버퍼를 DMA
 * 매핑하는 전체 흐름의 진입점으로, 드라이버(NVMe 등)가 ->queue_rq() 안에서
 * 명령어(CID)의 첫 PRP/SGL entry를 채울 때 호출한다. 내부적으로는
 * blk_rq_map_iter_init()으로 이 request의 데이터 반복자를 초기화한 뒤
 * blk_dma_map_iter_start()에 실제 작업(P2P 판별, IOVA vs direct 선택)을
 * 위임한다.
 * 실행 컨텍스트: 드라이버의 ->queue_rq() 콜백 안, request를 제출한 프로세스(또는
 * io_uring/커널 워커) 컨텍스트. 인터럽트 컨텍스트에서 호출하지 않는다(같은
 * request는 동시에 하나의 컨텍스트에서만 매핑됨).
 * 호출자: 블록 드라이버의 DMA 준비 함수(예: nvme_map_data() - 이 파일 범위
 *         밖인 drivers/ 트리에 위치).
 * 피호출자: blk_rq_map_iter_init(), blk_dma_map_iter_start().
 * 에러 경로: blk_dma_map_iter_start()가 false를 반환하면 그대로 전파,
 *          iter->status에 실패 사유(BLK_STS_RESOURCE/INVAL 등)가 남는다.
 *
 * 호출 체인:
 *   (블록 드라이버 ->queue_rq, 예: nvme_queue_rq -> nvme_map_data)
 *   -> [blk_rq_dma_map_iter_start] -> blk_rq_map_iter_init
 *                                    -> blk_dma_map_iter_start
 *
 * 이후 흐름: 호출자는 blk_rq_dma_map_coalesce(state)로 이 한 번의 호출이 모든
 * 세그먼트를 IOVA로 흡수했는지 확인하고, 그렇지 않다면
 * blk_rq_dma_map_iter_next()를 반복 호출해 나머지 세그먼트를 마저 매핑한다.
 */
bool blk_rq_dma_map_iter_start(struct request *req, struct device *dma_dev,
		struct dma_iova_state *state, struct blk_dma_iter *iter)
{
	blk_rq_map_iter_init(req, &iter->iter);	/* [한국어] request -> 데이터 반복자 초기화 (bio 체인 또는 special_vec 시작점 설정) */
	return blk_dma_map_iter_start(req, dma_dev, state, iter,
				      blk_rq_payload_bytes(req));	/* [한국어] 실제 매핑 위임 - total_len은 IOVA 공간 예약 크기 계산에 사용 */
}
EXPORT_SYMBOL_GPL(blk_rq_dma_map_iter_start);	/* [한국어] GPL 전용 심볼 공개 - 최신 IOVA 기반 반복자 API라 GPL 드라이버(NVMe 등 인트리 드라이버)만 사용 가능 */

/**
 * blk_rq_dma_map_iter_next - map the next DMA segment for a request
 * @req:	request to map
 * @dma_dev:	device to map to
 * @iter:	block layer DMA iterator
 *
 * Iterate to the next mapping after a previous call to
 * blk_rq_dma_map_iter_start().  See there for a detailed description of the
 * arguments.
 *
 * Returns %false if there is no segment to map, including due to an error, or
 * %true ft it did map a segment.
 *
 * If a segment was mapped, the DMA address for it is returned in @iter.addr and
 * the length in @iter.len.  If no segment was mapped the status code is
 * returned in @iter.status.
 */
/*
 * [한국어] (위 kerneldoc 원문 번역 및 보강)
 * blk_rq_dma_map_iter_next - request의 다음 DMA 세그먼트를 매핑
 *
 * @req:     매핑할 request.
 * @dma_dev: 매핑 대상 장치.
 * @iter:    blk_rq_dma_map_iter_start() 호출 이후 이어서 쓰는 반복자 - 그 함수의
 *           인자 설명을 그대로 따른다.
 * @return:  더 매핑할 세그먼트가 없거나 오류면 false, 매핑했으면 true.
 *
 * blk_rq_dma_map_iter_start() 한 번 호출 이후, 남은 세그먼트를 하나씩 마저
 * 매핑할 때 호출한다. 단, blk_rq_dma_map_iter_start()가 IOVA 경로
 * (blk_rq_dma_map_iova())를 탔다면 그 안의 do-while 루프가 이미 request의 모든
 * 세그먼트를 한 번에 소비했으므로, 이 함수가 실제로 두 번째 이후 세그먼트를
 * 순회하게 되는 것은 P2P bus-address 경로이거나 direct 매핑 경로를 탄 경우뿐이다.
 * 매핑에 성공하면 iter->addr/len에, 실패하면 iter->status에 결과가 기록된다.
 * 실행 컨텍스트: 드라이버의 ->queue_rq() 안에서 blk_rq_dma_map_iter_start()와
 * 같은 프로세스 컨텍스트, 세그먼트 개수만큼 반복 호출됨.
 * 호출자: 블록 드라이버(예: nvme_map_data() 루프 - 이 파일 범위 밖).
 * 피호출자: blk_map_iter_next(), blk_dma_map_bus(), blk_dma_map_direct().
 * 에러 경로: blk_dma_map_direct() 실패 시 BLK_STS_RESOURCE가 iter->status에
 *          남고 false 반환.
 *
 * 호출 체인:
 *   (블록 드라이버, 예: nvme_map_data 루프)
 *   -> [blk_rq_dma_map_iter_next] -> blk_map_iter_next -> blk_dma_map_bus
 *                                                        / blk_dma_map_direct
 */
bool blk_rq_dma_map_iter_next(struct request *req, struct device *dma_dev,
		struct blk_dma_iter *iter)
{
	struct phys_vec vec;	/* [한국어] 이번에 꺼낼 물리 세그먼트를 담을 임시 변수 */

	if (!blk_map_iter_next(req, &iter->iter, &vec))	/* [한국어] 다음 물리 세그먼트를 꺼냄 - IOVA 경로였다면 이미 모든 세그먼트가 소비되어 여기서 항상 false */
		return false;	/* [한국어] 더 이상 세그먼트 없음 - 정상적인 순회 종료 */

	if (iter->p2pdma.map == PCI_P2PDMA_MAP_BUS_ADDR)	/* [한국어] 첫 세그먼트가 P2P bus address 경로였으면 이후 세그먼트도 같은 경로 유지 */
		return blk_dma_map_bus(iter, &vec);	/* [한국어] bus address 매핑 */
	return blk_dma_map_direct(req, dma_dev, iter, &vec);	/* [한국어] 그 외에는 직접 DMA 매핑(dma_map_phys) */
}
EXPORT_SYMBOL_GPL(blk_rq_dma_map_iter_next);	/* [한국어] GPL 전용 심볼 공개 */

/*
 * [한국어]
 * blk_next_sg - scatterlist에서 다음으로 채울 sg entry 포인터를 반환
 *
 * @sg:     현재 마지막으로 채운 sg entry를 가리키는 포인터의 포인터(*sg가
 *          NULL이면 아직 아무것도 채우지 않은 상태).
 * @sglist: 대상 scatterlist 배열의 시작 포인터.
 * @return: 다음으로 채울 sg entry의 포인터. 첫 호출이면 sglist 자체, 아니면
 *          sg_next(*sg).
 *
 * 블록 드라이버(특히 IOMMU를 쓰지 않는 구형 드라이버)는 명령어마다 sg_init_table
 * ()로 scatterlist 전체를 재초기화하지 않는 경우가 있다. 이때 이전에 더 짧은
 * 리스트를 매핑했다면 마지막 entry에 종료(end) 비트가 남아 있을 수 있는데,
 * 이 함수는 sg_unmark_end()로 그 비트를 강제로 지운 뒤 sg_next()로 다음 슬롯을
 * 반환해, 매 명령마다 전체 재초기화를 하지 않고도 안전하게 이어붙일 수 있게
 * 한다.
 * 실행 컨텍스트: __blk_rq_map_sg()/blk_rq_map_integrity_sg()의 세그먼트 순회
 * 루프 안에서 세그먼트마다 한 번씩 호출되는 프로세스 컨텍스트 함수.
 * 호출자: __blk_rq_map_sg(), blk_rq_map_integrity_sg().
 * 피호출자: sg_unmark_end(), sg_next()(둘 다 scatterlist 공통 헬퍼).
 * 에러 경로: 없음 - 항상 유효한 포인터를 반환.
 *
 * 호출 체인:
 *   __blk_rq_map_sg / blk_rq_map_integrity_sg -> [blk_next_sg]
 */
static inline struct scatterlist *
blk_next_sg(struct scatterlist **sg, struct scatterlist *sglist)
{
	if (!*sg)	/* [한국어] 아직 sg 리스트를 채우기 시작하지 않았으면(첫 호출, *sg==NULL) */
		return sglist;	/* [한국어] sglist 배열의 첫 entry를 반환 - 이후 sg_set_page()가 여기부터 채움 */

	/*
	 * If the driver previously mapped a shorter list, we could see a
	 * termination bit prematurely unless it fully inits the sg table
	 * on each mapping. We KNOW that there must be more entries here
	 * or the driver would be buggy, so force clear the termination bit
	 * to avoid doing a full sg_init_table() in drivers for each command.
	 */
	sg_unmark_end(*sg);	/* [한국어] 이전 entry의 종료(end) 비트를 강제로 지움 - 뒤에 더 채울 entry가 있음을 보장 */
	return sg_next(*sg);	/* [한국어] 실제 다음 sg entry 포인터 반환 */
}

/*
 * Map a request to scatterlist, return number of sg entries setup. Caller
 * must make sure sg can hold rq->nr_phys_segments entries.
 */
/*
 * [한국어] (위 원문 주석 번역 및 보강) request를 scatterlist로 매핑하고 채운 sg
 * entry 개수를 반환한다. 호출자는 sg가 rq->nr_phys_segments개 entry를 담을 수
 * 있을 만큼 미리 할당해 두어야 한다.
 *
 * [한국어]
 * __blk_rq_map_sg - request의 물리 세그먼트들을 struct scatterlist 배열로 변환
 *
 * @rq:      매핑할 request.
 * @sglist:  채워 넣을 대상 scatterlist 배열(호출자가 rq->nr_phys_segments 이상
 *           크기로 미리 할당).
 * @last_sg: 마지막으로 채운 sg entry를 가리키는 포인터의 포인터 - blk_next_sg()
 *           가 다음 슬롯을 계산하는 데 사용하는 상태.
 * @return: 채운 sg entry 총 개수.
 *
 * IOMMU 기반 IOVA API를 쓰지 않는(또는 쓸 수 없는) 구형 DMA 매핑 경로를 위한
 * 호환 함수다. blk_rq_map_iter_init()으로 반복자를 초기화한 뒤,
 * blk_map_iter_next()가 반환하는 병합된 phys_vec마다 blk_next_sg()로 다음 sg
 * 슬롯을 얻고 sg_set_page()로 물리 페이지/오프셋/길이를 채운다. 다 채운 뒤에는
 * sg_mark_end()로 마지막 entry에 종료 비트를 세워 리스트 끝을 표시한다.
 * 실행 컨텍스트: DMA 매핑을 시작하는 프로세스 컨텍스트, request당 한 번 호출.
 * 호출자: 구형/비-IOVA 블록 드라이버(예: dma_map_sg() 계열 API를 쓰는 드라이버 -
 *         이 파일 범위 밖).
 * 피호출자: blk_rq_map_iter_init(), blk_map_iter_next(), blk_next_sg(),
 *          sg_set_page(), sg_mark_end().
 * 에러 경로: 명시적 에러 반환은 없다 - WARN_ON()으로 "계산된 세그먼트 수가
 *          사전에 추정한 rq->nr_phys_segments를 넘는" 내부 불일치만 경고한다
 *          (커널 설정에 따라 패닉하지 않고 경고만 남김).
 *
 * 호출 체인:
 *   (블록 드라이버의 구형 DMA 매핑 경로) -> [__blk_rq_map_sg] -> blk_next_sg
 */
int __blk_rq_map_sg(struct request *rq, struct scatterlist *sglist,
		    struct scatterlist **last_sg)
{
	struct blk_map_iter iter;	/* [한국어] 이 request 전용 데이터 반복자 - blk_rq_map_iter_init()으로 초기화 후 루프에서 전진 */
	struct phys_vec vec;	/* [한국어] 매 반복마다 꺼낼 물리 세그먼트 임시 저장소 */
	int nsegs = 0;	/* [한국어] 채워 넣은 scatterlist entry 개수 카운터 - 반환값이자 오버플로 검증 기준 */

	blk_rq_map_iter_init(rq, &iter);	/* [한국어] request로부터 데이터 반복자 초기화 - bio 체인 또는 special_vec 시작점 설정 */
	while (blk_map_iter_next(rq, &iter, &vec)) {	/* [한국어] 남은 물리 세그먼트가 있는 동안 반복 - 세그먼트 하나당 sg entry 하나 */
		*last_sg = blk_next_sg(last_sg, sglist);	/* [한국어] 이전 entry 다음 슬롯을 확보(첫 호출이면 sglist 시작) */

		WARN_ON_ONCE(overflows_type(vec.len, unsigned int));	/* [한국어] vec.len(size_t)이 unsigned int 범위를 넘으면 경고 - sg_set_page()의 length 인자 타입이 unsigned int이기 때문 */
		sg_set_page(*last_sg, phys_to_page(vec.paddr), vec.len,	/* [한국어] 이 세그먼트를 나타내는 struct page/길이로 entry 채우기 시작 */
				offset_in_page(vec.paddr));	/* [한국어] offset_in_page(paddr)로 페이지 내 오프셋 계산해 함께 기록 */
		nsegs++;	/* [한국어] entry 카운트 증가 */
	}

	if (*last_sg)	/* [한국어] 하나 이상 채웠으면(NULL이 아니면) */
		sg_mark_end(*last_sg);	/* [한국어] 마지막으로 채운 entry에 종료 비트 설정 - sglist 순회 시 이 지점에서 끝남을 표시 */

	/*
	 * Something must have been wrong if the figured number of
	 * segment is bigger than number of req's physical segments
	 */
	WARN_ON(nsegs > blk_rq_nr_phys_segments(rq));	/* [한국어] 실제 채운 개수가 request 준비 시 미리 계산한 물리 세그먼트 수를 넘으면 안 됨 - 넘으면 사전 계산과 실제 병합 로직 불일치 버그 */

	return nsegs;	/* [한국어] 채운 scatterlist entry 총 개수를 반환 - 호출자가 DMA API의 nents 인자로 사용 */
}
EXPORT_SYMBOL(__blk_rq_map_sg);	/* [한국어] 비-GPL 심볼 공개 - 레거시 scatterlist 매핑 API라 GPL 아닌 서드파티 드라이버도 링크 가능하게 공개 */

#ifdef CONFIG_BLK_DEV_INTEGRITY	/* [한국어] Data Integrity(DIF/DIX) 설정이 켜진 커널에서만 아래 integrity 전용 매핑/변환 함수 3개를 컴파일 */
/**
 * blk_rq_integrity_dma_map_iter_start - map the first integrity DMA segment
 * 					 for a request
 * @req:	request to map
 * @dma_dev:	device to map to
 * @state:	DMA IOVA state
 * @iter:	block layer DMA iterator
 *
 * Start DMA mapping @req integrity data to @dma_dev.  @state and @iter are
 * provided by the caller and don't need to be initialized.  @state needs to be
 * stored for use at unmap time, @iter is only needed at map time.
 *
 * Returns %false if there is no segment to map, including due to an error, or
 * %true if it did map a segment.
 *
 * If a segment was mapped, the DMA address for it is returned in @iter.addr
 * and the length in @iter.len.  If no segment was mapped the status code is
 * returned in @iter.status.
 *
 * The caller can call blk_rq_dma_map_coalesce() to check if further segments
 * need to be mapped after this, or go straight to blk_rq_dma_map_iter_next()
 * to try to map the following segments.
 */
/*
 * [한국어] (위 kerneldoc 원문 번역 및 보강)
 * blk_rq_integrity_dma_map_iter_start - integrity metadata의 첫 DMA 세그먼트를 매핑
 *
 * @req:     매핑할 request(integrity 정보가 req->bio에 붙어 있어야 함).
 * @dma_dev: 매핑 대상 장치.
 * @state:   DMA IOVA 상태 - blk_rq_dma_map_iter_start()와 동일하게 호출자가
 *           초기화할 필요 없이 이 함수가 채우고, unmap까지 보관해야 한다.
 * @iter:    블록 계층 DMA 반복자 - map 단계에서만 필요.
 * @return:  매핑할 세그먼트가 없거나 오류면 false, 첫 세그먼트를 매핑했으면
 *           true.
 *
 * blk_rq_dma_map_iter_start()의 "integrity metadata 버전"이다. 데이터 대신
 * bio_integrity(bio)->bip_iter/bip_vec을 순회하도록 iter->iter를 직접 구성하고
 * (is_integrity = true), payload 길이 대신 bio_integrity_bytes()로 계산한
 * metadata 총 바이트 수를 total_len으로 넘겨 공통 구현
 * blk_dma_map_iter_start()에 위임한다. NVMe로 치면 End-to-End Data Protection
 * (DIF/DIX)을 쓸 때 데이터 PRP/SGL과는 별도로 필요한 metadata buffer descriptor
 * 용 DMA 주소를 여기서 준비하는 것에 해당한다.
 * 실행 컨텍스트: integrity가 있는 request를 매핑하는 드라이버의 프로세스
 * 컨텍스트, request당 한 번 호출.
 * 호출자: 블록 드라이버의 integrity DMA 준비 경로(이 파일 범위 밖).
 * 피호출자: bio_integrity_bytes(), bio_integrity(), blk_dma_map_iter_start().
 * 에러 경로: blk_dma_map_iter_start()와 동일 - false 반환 시 iter->status 확인.
 *
 * 호출 체인:
 *   (블록 드라이버의 integrity DMA 준비 경로)
 *   -> [blk_rq_integrity_dma_map_iter_start] -> blk_dma_map_iter_start
 */
bool blk_rq_integrity_dma_map_iter_start(struct request *req,
		struct device *dma_dev,  struct dma_iova_state *state,
		struct blk_dma_iter *iter)
{
	unsigned len = bio_integrity_bytes(&req->q->limits.integrity,	/* [한국어] queue의 integrity profile과 이 request의 섹터 수를 근거로 metadata 총 길이 계산 시작 */
					   blk_rq_sectors(req));	/* [한국어] blk_rq_sectors(req): 이 request가 담당하는 논리 섹터 수 - bio_integrity_bytes()가 섹터당 metadata_size를 곱해 총 길이를 계산 */
	struct bio *bio = req->bio;	/* [한국어] integrity payload를 가진 bio - request의 첫 bio 기준 */

	iter->iter = (struct blk_map_iter) {	/* [한국어] 데이터용이 아닌 integrity 전용 반복자로 초기화 시작 */
		.bio = bio,	/* [한국어] bio 포인터 저장 - __blk_map_iter_next가 bi_next 체인을 따라갈 때 기준이 됨 */
		.iter = bio_integrity(bio)->bip_iter,	/* [한국어] integrity payload의 bvec_iter(순회 위치) - 처음이므로 payload 시작점 */
		.bvecs = bio_integrity(bio)->bip_vec,	/* [한국어] integrity 전용 bio_vec 배열(데이터 페이지가 아닌 metadata 페이지 목록) */
		.is_integrity = true,	/* [한국어] 이후 __blk_map_iter_next가 bio 전환 시 데이터 대신 integrity 필드를 갱신하도록 지시하는 플래그 */
	};
	return blk_dma_map_iter_start(req, dma_dev, state, iter, len);	/* [한국어] 공통 매핑 로직에 위임 - len(metadata 총 길이)을 total_len으로 전달해 IOVA 예약 크기로 사용 */
}
EXPORT_SYMBOL_GPL(blk_rq_integrity_dma_map_iter_start);	/* [한국어] GPL 전용 심볼 공개 */

/**
 * blk_rq_integrity_dma_map_iter_next - map the next integrity DMA segment for
 * 					 a request
 * @req:	request to map
 * @dma_dev:	device to map to
 * @state:	DMA IOVA state
 * @iter:	block layer DMA iterator
 *
 * Iterate to the next integrity mapping after a previous call to
 * blk_rq_integrity_dma_map_iter_start().  See there for a detailed description
 * of the arguments.
 *
 * Returns %false if there is no segment to map, including due to an error, or
 * %true if it did map a segment.
 *
 * If a segment was mapped, the DMA address for it is returned in @iter.addr and
 * the length in @iter.len.  If no segment was mapped the status code is
 * returned in @iter.status.
 */
/*
 * [한국어] (위 kerneldoc 원문 번역 및 보강)
 * blk_rq_integrity_dma_map_iter_next - integrity metadata의 다음 DMA 세그먼트를 매핑
 *
 * @req:     매핑할 request.
 * @dma_dev: 매핑 대상 장치.
 * @iter:    blk_rq_integrity_dma_map_iter_start() 이후 이어서 쓰는 반복자.
 * @return:  더 매핑할 세그먼트가 없거나 오류면 false, 매핑했으면 true.
 *
 * blk_rq_dma_map_iter_next()와 동일한 구조이나 데이터가 아닌 integrity
 * metadata 세그먼트를 대상으로 한다. IOVA 경로를 탔다면 이미 모든 metadata
 * 세그먼트가 blk_dma_map_iter_start() 안에서 소비되었을 것이므로, 이 함수가
 * 실질적으로 쓰이는 것은 P2P bus-address 또는 direct 매핑 경로를 탄 경우다.
 * 실행 컨텍스트: 드라이버의 integrity DMA 준비 루프 안, 프로세스 컨텍스트.
 * 호출자: 블록 드라이버의 integrity DMA 준비 경로(이 파일 범위 밖).
 * 피호출자: blk_map_iter_next(), blk_dma_map_bus(), blk_dma_map_direct().
 * 에러 경로: blk_dma_map_direct() 실패 시 BLK_STS_RESOURCE.
 *
 * 호출 체인:
 *   (블록 드라이버의 integrity DMA 준비 루프)
 *   -> [blk_rq_integrity_dma_map_iter_next] -> blk_map_iter_next
 *                                             -> blk_dma_map_bus / blk_dma_map_direct
 */
bool blk_rq_integrity_dma_map_iter_next(struct request *req,
               struct device *dma_dev, struct blk_dma_iter *iter)
{
	struct phys_vec vec;	/* [한국어] 이번에 꺼낼 integrity metadata 물리 세그먼트 임시 변수 */

	if (!blk_map_iter_next(req, &iter->iter, &vec))	/* [한국어] 다음 metadata 세그먼트를 꺼냄 */
		return false;	/* [한국어] 더 이상 세그먼트 없음 - 정상 종료 */

	if (iter->p2pdma.map == PCI_P2PDMA_MAP_BUS_ADDR)	/* [한국어] 첫 세그먼트가 P2P bus address 경로였으면 이후에도 유지 */
		return blk_dma_map_bus(iter, &vec);	/* [한국어] bus address 매핑 */
	return blk_dma_map_direct(req, dma_dev, iter, &vec);	/* [한국어] 그 외에는 직접 DMA 매핑 */
}
EXPORT_SYMBOL_GPL(blk_rq_integrity_dma_map_iter_next);	/* [한국어] GPL 전용 심볼 공개 */

/**
 * blk_rq_map_integrity_sg - Map integrity metadata into a scatterlist
 * @rq:		request to map
 * @sglist:	target scatterlist
 *
 * Description: Map the integrity vectors in request into a
 * scatterlist.  The scatterlist must be big enough to hold all
 * elements.  I.e. sized using blk_rq_count_integrity_sg() or
 * rq->nr_integrity_segments.
 */
/*
 * [한국어] (위 kerneldoc 원문 번역 및 보강)
 * blk_rq_map_integrity_sg - integrity metadata를 struct scatterlist로 변환
 *
 * @rq:     매핑할 request(integrity 정보가 붙어 있어야 함).
 * @sglist: 채워 넣을 대상 scatterlist 배열 - rq->nr_integrity_segments 이상
 *          (blk_rq_count_integrity_sg()로 미리 계산) 크기로 할당되어 있어야
 *          한다.
 * @return: 채운 integrity sg entry 총 개수.
 *
 * __blk_rq_map_sg()의 integrity metadata 버전으로, 데이터가 아니라
 * bio_integrity(bio)->bip_iter/bip_vec을 순회하며 scatterlist를 채운다.
 * blk_next_sg()로 슬롯을 얻고 sg_set_page()로 물리 페이지/오프셋/길이를
 * 채운 뒤, 다 채운 entry에는 sg_mark_end()로 종료 비트를 세운다.
 * 주의(비-integrity 버전과의 차이): 계산된 segments 수가 한도를 넘으면
 * __blk_rq_map_sg()는 WARN_ON()으로 경고만 하지만, 이 함수는 BUG_ON()으로 즉시
 * 커널 패닉을 일으킨다 - integrity metadata buffer overflow는 조용히 넘어가면
 * 데이터 손상/보안 문제로 이어질 수 있어 더 엄격하게 처리하는 것으로 보인다
 * (추정: 커밋 이력/설계 의도까지는 이 파일만으로 확정할 수 없음).
 * 실행 컨텍스트: 구형/비-IOVA 블록 드라이버가 integrity 데이터를 매핑하는
 * 프로세스 컨텍스트, request당 한 번 호출.
 * 호출자: 구형/비-IOVA 블록 드라이버의 integrity DMA 준비 경로(이 파일 범위
 *         밖).
 * 피호출자: bio_integrity(), blk_map_iter_next(), blk_next_sg(), sg_set_page(),
 *          sg_mark_end(), queue_max_integrity_segments().
 * 에러 경로: BUG_ON() 두 곳 - segments가 rq->nr_integrity_segments 또는
 *          queue_max_integrity_segments(q)를 초과하면 커널 패닉(복구 불가,
 *          드라이버/블록 계층의 세그먼트 계산 불일치를 의미하는 치명적 버그로
 *          간주).
 *
 * 호출 체인:
 *   (구형 블록 드라이버의 integrity DMA 준비 경로)
 *   -> [blk_rq_map_integrity_sg] -> blk_next_sg
 */
int blk_rq_map_integrity_sg(struct request *rq, struct scatterlist *sglist)
{
	struct request_queue *q = rq->q;	/* [한국어] queue_max_integrity_segments() 등 큐 한도 조회에 사용할 request_queue 포인터 */
	struct scatterlist *sg = NULL;	/* [한국어] blk_next_sg()가 첫 호출인지 판단하는 기준(NULL이면 첫 entry) */
	struct bio *bio = rq->bio;	/* [한국어] integrity payload를 담은 bio */
	unsigned int segments = 0;	/* [한국어] 채운 integrity sg entry 개수 카운터 */
	struct phys_vec vec;	/* [한국어] 순회 중 꺼낼 물리 세그먼트 임시 저장 */

	struct blk_map_iter iter = {	/* [한국어] integrity 전용 반복자를 지역 변수로 직접 초기화(다른 integrity 함수와 달리 iter 자체가 지역 변수) */
		.bio = bio,	/* [한국어] bio 포인터 저장 */
		.iter = bio_integrity(bio)->bip_iter,	/* [한국어] integrity bvec_iter 시작점 */
		.bvecs = bio_integrity(bio)->bip_vec,	/* [한국어] integrity bio_vec 배열 */
		.is_integrity = true,	/* [한국어] integrity 전용 순회임을 표시 */
	};

	while (blk_map_iter_next(rq, &iter, &vec)) {	/* [한국어] 남은 integrity 세그먼트가 있는 동안 반복 */
		sg = blk_next_sg(&sg, sglist);	/* [한국어] 다음 sg entry 슬롯 확보 */

		WARN_ON_ONCE(overflows_type(vec.len, unsigned int));	/* [한국어] 길이 오버플로 경고 - sg_set_page() length 인자가 unsigned int이기 때문 */
		sg_set_page(sg, phys_to_page(vec.paddr), vec.len,	/* [한국어] 물리 페이지+길이로 sg entry 채우기 시작 */
				offset_in_page(vec.paddr));	/* [한국어] offset_in_page(paddr)로 페이지 내 오프셋 계산해 함께 기록 */
		segments++;	/* [한국어] entry 카운트 증가 */
	}

	if (sg)	/* [한국어] 하나 이상 채웠으면 */
	        sg_mark_end(sg);	/* [한국어] 마지막 entry에 종료 비트 설정 (원본 코드의 들여쓰기를 그대로 유지) */

	/*
	 * Something must have been wrong if the figured number of segment
	 * is bigger than number of req's physical integrity segments
	 */
	BUG_ON(segments > rq->nr_integrity_segments);	/* [한국어] segments가 request 준비 시 계산된 한도를 넘으면 안 됨 - 비-integrity 버전과 달리 WARN_ON이 아닌 BUG_ON으로 즉시 패닉 */
	BUG_ON(segments > queue_max_integrity_segments(q));	/* [한국어] queue가 지원하는 최대 integrity segment 수도 초과 금지 */
	return segments;	/* [한국어] 채운 metadata sg entry 총 개수 반환 */
}
EXPORT_SYMBOL(blk_rq_map_integrity_sg);	/* [한국어] 비-GPL 심볼 공개 - 레거시 scatterlist 기반 API */
#endif	/* [한국어] CONFIG_BLK_DEV_INTEGRITY 조건부 컴파일 종료 */
