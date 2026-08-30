// SPDX-License-Identifier: GPL-2.0
/*
 * Functions related to segment and merge handling
 *
 * [한국어 설명] 블록 계층 bio/request 분할·병합 핵심 구현 (blk-merge.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 파일시스템·응용이 발행한 bio를 NVMe 컨트롤러(또는 임의의 블록
 * 디바이스)가 처리할 수 있는 크기와 형태로 정제하는 두 가지 핵심 역할을 한다:
 *
 * 1) 분할(split): bio가 queue_limits(max_sectors, max_segments, MDTS,
 *    virt_boundary_mask 등)을 초과하면 여러 개의 작은 bio로 쪼개어 각각
 *    NVMe SQ 엔트리 하나로 매핑되도록 한다.
 * 2) 병합(merge): 인접한 LBA를 가진 두 request 또는 bio를 하나로 합쳐
 *    SQ 엔트리 수와 doorbell 횟수를 줄이고 처리량을 높인다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * split 경로:
 *   응용 write(2) → submit_bio()
 *       ↓
 *   [blk-mq] blk_mq_submit_bio()
 *       ↓ → bio_split_to_limits()  ← 이 파일의 진입점
 *       ↓ → bio_split_rw()         ← queue_limits 위반 시 분할
 *       ↓ → bio_submit_split_bioset() ← 분할된 앞부분 먼저 제출
 *       ↓
 *   [nvme_queue_rq] → SQ doorbell
 *
 * merge 경로:
 *   blk_mq_submit_bio → elv_merge() [elevator.c]
 *       ↓ → ll_back_merge_fn() / ll_front_merge_fn()  ← 이 파일
 *       ↓ → blk_attempt_req_merge()
 *       ↓ → attempt_merge() → ll_merge_requests_fn()
 *       ↓
 *   병합된 request → nvme_queue_rq → SQ doorbell
 *
 * 실행 컨텍스트: bio 제출 경로(프로세스/IRQ 컨텍스트), 분할된 bio는 재귀
 * submit_bio_noacct_nocheck() 경로로 다시 진입하여 추가 분할될 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - block/blk-settings.c: queue_limits 값(max_sectors, max_segments,
 *     discard_granularity, logical_block_size 등) 초기화
 *   - block/bio.c: bio_split(), bio_chain(), bio_get_last_bvec() 등 bio 조작
 *   - block/elevator.c: elv_merge()에서 ll_back/front_merge_fn 호출
 *   - block/blk-mq.c: bio_split_to_limits() 호출, request 완료 통계 기록
 *   - drivers/nvme/host/pci.c|tcp.c: 최종 nvme_queue_rq()에서 분할된
 *     bio를 PRP/SGL로 매핑하여 SQ에 제출
 * 공유 자료구조:
 *   - struct queue_limits: max_sectors, max_segments, virt_boundary_mask,
 *     discard_granularity 등 — 분할·병합 판단의 기준
 *   - struct bio_vec: 물리 페이지 기술자 — PRP/SGL 엔트리로 변환되는 단위
 *
 * === 주요 함수/구조체 요약 ===
 * bio_split_to_limits()      - 모든 bio가 거치는 분할 진입점; queue_limits 검사 후 분기
 * bio_split_rw()             - 읽기/쓰기 bio를 세그먼트·섹터 한계에 맞게 분할
 * bio_split_io_at()          - 분할 지점(섹터 수)을 계산해 반환
 * bio_submit_split_bioset()  - 분할 실행 + 앞부분 즉시 submit
 * ll_back_merge_fn()         - back-merge 가능 여부 최종 확인 (세그먼트 수, gap)
 * ll_front_merge_fn()        - front-merge 가능 여부 최종 확인
 * ll_merge_requests_fn()     - 두 request를 완전 병합; 세그먼트 합산 검사
 * attempt_merge()            - back/front/discard merge 시도 전체 로직
 * blk_attempt_req_merge()    - elevator에서 호출하는 request 병합 공개 API
 * blk_rq_merge_ok()          - 두 request가 병합 가능한지 기본 조건 검사
 * bio_will_gap()             - PRP/SGL 물리 불연속(gap) 여부 검사
 */
#include <linux/kernel.h>		/* [한국어] min/max, ALIGN 등 — 분할 경계 계산 전반 */
#include <linux/module.h>		/* [한국어] EXPORT_SYMBOL — bio_split_to_limits 등을 스택 드라이버에 공개 */
#include <linux/bio.h>			/* [한국어] bio_for_each_bvec, bio_split — 분할의 실제 도구 */
#include <linux/blkdev.h>		/* [한국어] queue_limits — 이 파일의 모든 판정이 여기서 나온다
					 * (max_hw_sectors, max_segments, virt_boundary_mask, chunk_sectors) */
#include <linux/blk-integrity.h>	/* [한국어] 무결성 페이로드도 데이터와 같은 지점에서 함께 쪼개야 하므로 필요 */
#include <linux/part_stat.h>		/* [한국어] part_stat_* — 분할·병합 시 파티션 통계를 갱신한다 */
#include <linux/blk-cgroup.h>		/* [한국어] blk_cgroup_mergeable — 소속 cgroup 이 다르면 병합을 막는다.
					 * 합치면 두 cgroup 의 IO 회계가 뒤섞여 격리가 깨지기 때문이다 */

#include <trace/events/block.h>	/* [한국어] trace_block_split — bio 가 쪼개질 때마다 tracepoint 를 남긴다.
				 * 분할이 잦다는 것은 큐 한계에 자주 걸린다는 뜻이라 성능 분석의 단서가 된다 */

#include "blk.h"		/* [한국어] rq_mergeable, bio_may_need_split 등 판정 헬퍼의 정의처 */
#include "blk-mq-sched.h"	/* [한국어] 스케줄러 큐에 있는 요청과의 병합 시도 경로 */
#include "blk-rq-qos.h"		/* [한국어] rq_qos_merge — 병합이 일어나면 QoS 계층에도 알려야 회계가 맞는다 */
#include "blk-throttle.h"	/* [한국어] blk_throtl_bio 관련 — 스로틀 중인 bio 의 병합 취급 */

/*
 * [한국어]
 * bio_get_first_bvec - bio의 첫 번째 bvec(물리 페이지 기술자)를 추출
 *
 * @bio: 첫 번째 bvec를 가져올 bio
 * @bv:  결과를 담을 bio_vec 포인터
 *
 * bi_iter(현재 이터레이터 위치)를 기준으로 첫 번째 bvec를 읽는다.
 * NVMe PRP/SGL 구성 시 첫 번째 페이지의 시작 오프셋(bv_offset)과
 * 길이(bv_len)가 DMA 주소 계산의 기준이 된다.
 * bio_will_gap()에서 연속 bio 병합 시 gap 검사에 사용된다.
 *
 * 호출 체인:
 *   bio_will_gap → [bio_get_first_bvec]
 */
static inline void bio_get_first_bvec(struct bio *bio, struct bio_vec *bv)
{
/* mp_bvec_iter_bvec: 이터레이터 위치에서 현재 bvec를 반환 (multi-page 지원) */
	*bv = mp_bvec_iter_bvec(bio->bi_io_vec, bio->bi_iter);
}

/*
 * [한국어]
 * bio_get_last_bvec - bio의 마지막 bvec(물리 페이지 기술자)를 추출
 *
 * @bio: 마지막 bvec를 가져올 bio
 * @bv:  결과를 담을 bio_vec 포인터
 *
 * bio의 전체 크기(bi_size)만큼 이터레이터를 전진시킨 뒤, 마지막으로
 * 실제 bvec가 있는 인덱스를 찾아 반환한다. bi_bvec_done이 0이 아니면
 * 마지막 bvec의 중간에서 bio가 끝나는 것이므로 bv_len을 조정한다.
 * NVMe 병합 시 두 bio의 물리 경계 연속성 확인에 필수적이다.
 *
 * 호출 체인:
 *   bio_will_gap → [bio_get_last_bvec]
 */
static inline void bio_get_last_bvec(struct bio *bio, struct bio_vec *bv)
{
	/* [한국어] bio->bi_iter를 값으로 복사한다. bio_advance_iter()가 이터레이터를
	 * 파괴적으로 전진시키므로 원본 bi_iter를 건드리면 아직 제출되지 않은 bio의
	 * 진행 상태가 깨진다. 복사본 위에서만 전진시켜 원본을 보존한다. */
	struct bvec_iter iter = bio->bi_iter;
	/* [한국어] bi_io_vec[] 배열에서 "마지막 bvec"의 인덱스를 담을 지역 변수.
	 * 아래에서 bi_bvec_done(마지막 bvec를 몇 바이트까지 소비했는지) 값에 따라
	 * bi_idx - 1 또는 bi_idx 중 하나로 결정된다. */
	int idx;

	/* [한국어] 먼저 첫 번째 bvec를 채워 둔다. bvec가 하나뿐인 bio(대부분의
	 * 4KiB 단일 페이지 I/O)라면 첫 bvec가 곧 마지막 bvec이므로 아래 early
	 * return으로 끝난다. 즉 흔한 경우에 대한 빠른 경로다. */
	bio_get_first_bvec(bio, bv);
	/* [한국어] 첫 bvec의 길이가 bio 전체 크기와 같다 == bvec가 하나뿐이다.
	 * 이때는 이미 bv에 정답이 들어 있으므로 이터레이터 전진 없이 반환한다. */
	if (bv->bv_len == bio->bi_iter.bi_size)
		return;		/* this bio only has a single bvec */

	/* [한국어] 이터레이터를 bio 전체 크기(bi_size)만큼 전진시켜 "끝"으로 보낸다.
	 * 전진 후 iter.bi_idx는 마지막으로 소비한 bvec의 다음 위치를 가리키고,
	 * iter.bi_bvec_done은 그 bvec를 몇 바이트 소비했는지를 담는다. */
	bio_advance_iter(bio, &iter, iter.bi_size);

	/* [한국어] bi_bvec_done == 0이면 bio가 bvec 경계에서 정확히 끝났다는 뜻이다.
	 * 이터레이터는 이미 다음 bvec로 넘어가 있으므로 -1을 해야 실제 마지막
	 * bvec를 가리킨다. */
	if (!iter.bi_bvec_done)
		idx = iter.bi_idx - 1;
	else	/* in the middle of bvec */
		/* [한국어] bi_bvec_done != 0이면 bio가 bvec 중간에서 끝났다. 이터레이터가
		 * 아직 그 bvec 위에 머물러 있으므로 bi_idx가 곧 마지막 bvec 인덱스다.
		 * (하나의 물리 페이지를 두 bio가 앞뒤로 나눠 갖는 분할 이후 상황) */
		idx = iter.bi_idx;

	/* [한국어] 결정된 인덱스로 bi_io_vec[] 원본 배열에서 bvec를 통째로 복사한다.
	 * 여기서 얻은 (bv_page, bv_offset, bv_len)이 bio_will_gap()에서 다음 bio의
	 * 첫 bvec와 물리적으로 이어지는지 판단하는 기준이 된다. */
	*bv = bio->bi_io_vec[idx];

	/*
	 * iter.bi_bvec_done records actual length of the last bvec
	 * if this bio ends in the middle of one io vector
	 */
	/* [한국어] bvec 중간에서 끝난 경우 원본 bvec의 bv_len은 "페이지 전체 길이"라
	 * 실제 이 bio가 쓰는 길이보다 크다. bi_bvec_done(실제 소비 바이트)으로
	 * 덮어써야 gap 검사가 올바른 끝 주소를 본다. 이 보정을 빼먹으면 물리적으로
	 * 이어지지 않는 두 bio를 이어진다고 오판해 NVMe가 잘못된 PRP를 받게 된다. */
	if (iter.bi_bvec_done)
		bv->bv_len = iter.bi_bvec_done;
}

/*
 * [한국어]
 * bio_will_gap - 두 bio를 병합하면 virt_boundary(=PRP 페이지 경계)를 위반하는지 검사
 *
 * @q:       대상 request_queue. q->limits.virt_boundary_mask를 판단 기준으로 읽는다.
 * @prev_rq: back-merge 검사 시 기존 request(첫 bvec의 offset 확인용). front-merge
 *           검사에서는 NULL이 전달되어 prev bio 자체를 기준으로 삼는다.
 * @prev:    물리적으로 앞에 오는 bio (병합 후 앞부분이 될 쪽)
 * @next:    물리적으로 뒤에 오는 bio (병합 후 뒷부분이 될 쪽)
 * @return:  true = gap이 생기므로 병합 불가, false = 병합해도 경계 위반 없음
 *
 * === 왜 필요한가: NVMe PRP의 구조적 제약 ===
 * NVMe PCIe는 데이터 버퍼를 PRP(Physical Region Page) 또는 SGL로 기술한다.
 * PRP는 "주소 목록"일 뿐 길이 필드가 없어서, NVMe 사양상 다음 규칙을 강제한다:
 *   - PRP1(첫 엔트리)만 페이지 중간 오프셋을 가질 수 있다.
 *   - PRP2 이후의 모든 엔트리는 반드시 페이지 오프셋 0이어야 한다.
 *   - 마지막을 제외한 모든 엔트리는 페이지 끝까지 꽉 차야 한다.
 * 따라서 버퍼 중간에 "페이지 경계에서 시작하지 않는 조각"이 끼면 PRP로 표현할
 * 방법이 아예 없다. 이 제약을 블록 계층에 알리는 값이 virt_boundary_mask다.
 *
 * 실제 값의 출처(이 트리에서 확인 가능):
 *   drivers/nvme/host/pci.c:nvme_pci_get_virt_boundary()
 *     → SGL 미지원이거나 admin 큐면 NVME_CTRL_PAGE_SIZE - 1 (= 4096-1 = 0xFFF)
 *     → SGL을 쓰는 I/O 큐면 0 (SGL은 엔트리마다 길이가 있어 gap 제약이 없음)
 *   drivers/nvme/host/core.c:2440 에서 lim->virt_boundary_mask에 대입된다.
 * 즉 이 함수가 true를 반환하는 상황은 "PRP 모드 NVMe에서 병합하면 안 되는 조합"과
 * 정확히 같다. SGL 모드에서는 mask가 0이라 아래 첫 조건에서 즉시 false가 된다.
 *
 * 실행 컨텍스트: bio 제출 경로(프로세스 컨텍스트, 플러그 병합 시 plug 리스트 순회
 * 중). 락은 잡지 않으며 읽기 전용 판단만 수행한다.
 *
 * 에러 경로: 이 함수 자체는 실패하지 않는다. true를 반환하면 상위 ll_*_merge_fn이
 * 병합을 포기하고 새 request를 할당하는 정상 경로로 빠진다.
 *
 * 호출 체인:
 *   blk_mq_submit_bio → blk_attempt_plug_merge/blk_mq_sched_try_merge
 *     → blk_try_merge → bio_attempt_back_merge/bio_attempt_front_merge
 *     → ll_back_merge_fn/ll_front_merge_fn
 *     → req_gap_back_merge/req_gap_front_merge → [bio_will_gap]
 *       → bio_get_first_bvec / bio_get_last_bvec / biovec_phys_mergeable
 */
static inline bool bio_will_gap(struct request_queue *q,
		struct request *prev_rq, struct bio *prev, struct bio *next)
{
	struct bio_vec pb, nb;

	/* [한국어] 두 가지 조기 탈출.
	 * (1) bio_has_data(prev)==false: REQ_OP_FLUSH처럼 데이터 페이로드가 없는
	 *     bio는 애초에 PRP/SGL을 만들지 않으므로 경계 위반이 있을 수 없다.
	 * (2) queue_virt_boundary(q)==0: 큐가 virt boundary 제약을 두지 않는다는 뜻.
	 *     NVMe에서는 SGL을 쓰는 I/O 큐가 여기 해당한다(nvme_pci_get_virt_boundary가
	 *     0을 반환). SGL 디스크립터는 (주소,길이) 쌍이라 페이지 중간에서 시작해도
	 *     되므로 gap 개념 자체가 없다. 둘 중 하나라도 참이면 병합을 허용한다. */
	if (!bio_has_data(prev) || !queue_virt_boundary(q))
		return false;

	/*
	 * Don't merge if the 1st bio starts with non-zero offset, otherwise it
	 * is quite difficult to respect the sg gap limit.  We work hard to
	 * merge a huge number of small single bios in case of mkfs.
	 */
	/* [한국어] 병합 결과물 전체의 "첫 bvec"를 pb에 담는다.
	 * back-merge(prev_rq != NULL)면 request의 선두 bio(prev_rq->bio)가 첫 bvec의
	 * 주인이고, front-merge(prev_rq == NULL)면 지금 앞에 붙이려는 prev bio가
	 * 첫 bvec의 주인이다. 어느 쪽이든 "합쳐진 버퍼의 맨 앞"을 봐야 한다. */
	if (prev_rq)
		bio_get_first_bvec(prev_rq->bio, &pb);
	else
		bio_get_first_bvec(prev, &pb);
	/* [한국어] 첫 bvec의 오프셋이 페이지 경계에 정렬되어 있지 않으면 병합을 포기한다.
	 * PRP1만은 중간 오프셋을 가질 수 있으므로 "단일 bio"라면 문제가 없지만, 오프셋이
	 * 어긋난 상태에서 뒤에 계속 이어 붙이면 두 번째 이후 조각이 페이지 경계에
	 * 안 맞게 되어 PRP로 표현할 수 없는 요청이 만들어진다. 위쪽 영문 주석이 말하듯
	 * 이런 bio까지 일반화해 처리하려면 로직이 지나치게 복잡해지므로, 커널은
	 * "정렬된 것끼리만 공격적으로 병합"하는 단순한 정책을 택했다. mkfs처럼 작은
	 * bio가 폭주하는 워크로드에서 병합 이득을 얻기 위한 타협점이다. */
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
	/* [한국어] 이제 실제 이음매를 본다: prev의 "마지막" bvec(pb)와 next의 "첫"
	 * bvec(nb). 이 두 조각이 맞닿는 지점이 병합으로 새로 생기는 유일한 경계다. */
	bio_get_last_bvec(prev, &pb);
	bio_get_first_bvec(next, &nb);
	/* [한국어] 두 bvec가 물리 주소상 완전히 연속이면(page_to_phys(pb)+pb.bv_len ==
	 * page_to_phys(nb)+nb.bv_offset) 이음매가 사라지고 하나의 세그먼트로 합쳐진다.
	 * 새 PRP 엔트리가 생기지 않으므로 경계 위반도 없다 → 병합 허용. */
	if (biovec_phys_mergeable(q, &pb, &nb))
		return false;
	/* [한국어] 물리적으로 떨어져 있다면 병합 시 별도 세그먼트(=별도 PRP 엔트리)가
	 * 생긴다. 그 새 엔트리가 규칙을 지키려면 (a) pb가 페이지 끝까지 꽉 차야 하고
	 * (b) nb가 페이지 오프셋 0에서 시작해야 한다. __bvec_gap_to_prev()가 바로 이
	 * 두 조건을 virt_boundary_mask로 검사한다. 하나라도 어기면 true(=gap 있음,
	 * 병합 금지)를 반환하고, 상위 함수는 별도 request를 만들어 각각 SQ 엔트리로
	 * 내보낸다. */
	return __bvec_gap_to_prev(&q->limits, &pb, nb.bv_offset);
}

/*
 * [한국어]
 * req_gap_back_merge - request의 뒤(biotail)에 bio를 붙일 때 PRP/SGL gap 발생 여부 검사
 *
 * @req: 뒤에 bio를 붙이려는 기존 request
 * @bio: 병합하려는 새 bio
 * @return: true이면 gap이 생겨 병합 불가; false이면 gap 없음(병합 안전)
 *
 * request의 마지막 bio(biotail)와 새 bio 사이에 물리 불연속이 생기는지
 * bio_will_gap()으로 확인한다. NVMe PRP/SGL 한 엔트리에 연속된 물리 메모리만
 * 들어갈 수 있으므로, gap이 있으면 SGL 엔트리를 추가해야 하는데 이미 한계에
 * 달했다면 병합을 거부한다.
 *
 * 호출 체인:
 *   ll_back_merge_fn → [req_gap_back_merge] → bio_will_gap
 */
static inline bool req_gap_back_merge(struct request *req, struct bio *bio)
{
/* req->biotail: request 마지막 bio; 새 bio를 그 뒤에 붙일 때의 gap 검사 */
	return bio_will_gap(req->q, req, req->biotail, bio);
}

/*
 * [한국어]
 * req_gap_front_merge - request의 앞(bio)에 bio를 붙일 때 PRP/SGL gap 발생 여부 검사
 *
 * @req: 앞에 bio를 붙이려는 기존 request
 * @bio: 병합하려는 새 bio
 * @return: true이면 gap이 생겨 병합 불가; false이면 gap 없음(병합 안전)
 *
 * 새 bio를 request의 첫 번째 bio 앞에 붙이는(front-merge) 경우의 gap 검사.
 * bio_will_gap()에 prev_rq=NULL을 전달해 "앞부분 bio만" 기준으로 경계 확인.
 *
 * 호출 체인:
 *   ll_front_merge_fn → [req_gap_front_merge] → bio_will_gap
 */
static inline bool req_gap_front_merge(struct request *req, struct bio *bio)
{
/* prev_rq=NULL: front-merge 시 req->bio(첫 bio) 기준의 gap 검사 */
	return bio_will_gap(req->q, NULL, bio, req->bio);
}

/*
 * [한국어]
 * bio_allowed_max_sectors - bio 하나가 담을 수 있는 절대 상한(섹터)을 계산
 *
 * @lim: queue_limits. logical_block_size만 참조한다.
 * @return: BIO_MAX_SIZE를 논리 블록 크기로 내림 정렬한 뒤 섹터 단위로 변환한 값.
 *
 * BIO_MAX_SIZE는 bi_size(bio가 표현 가능한 최대 바이트 수) 자료형에서 오는
 * 소프트웨어적 한계다. 이를 그대로 쓰면 논리 블록 중간에서 잘릴 수 있으므로
 * round_down으로 블록 경계에 맞춘다. 하드웨어는 논리 블록보다 작은 단위를
 * 받지 못하기 때문이다(영문 주석의 "minimum accepted unit by hardware").
 *
 * NVMe에서 logical_block_size는 Identify Namespace의 LBA Format(LBAF) 항목 중
 * LBADS(LBA Data Size) 필드에서 유래하며 보통 512B 또는 4096B다.
 * drivers/nvme/host/core.c의 nvme_update_disk_info()가 lim->logical_block_size에
 * 대입한다. 이 값이 곧 NVMe Read/Write 커맨드의 SLBA/NLB 단위이기도 하다.
 *
 * 실행 컨텍스트: bio 제출 경로. 순수 계산 함수로 부수 효과가 없다.
 *
 * 호출 체인:
 *   bio_split_discard → __bio_split_discard → [bio_allowed_max_sectors]
 *   blk_rq_get_max_sectors → [bio_allowed_max_sectors]  (discard 계열 한정)
 */
/*
 * The maximum size that a bio can fit has to be aligned down to the
 * logical block size, which is the minimum accepted unit by hardware.
 */
static unsigned int bio_allowed_max_sectors(const struct queue_limits *lim)
{
	/* [한국어] BIO_MAX_SIZE(바이트)를 논리 블록 크기의 배수로 내린 뒤, SECTOR_SHIFT(=9)
	 * 만큼 우측 시프트해 512B 섹터 단위로 바꾼다. 블록 계층의 모든 섹터 카운트는
	 * logical_block_size와 무관하게 항상 512B 단위라는 점에 주의 — 4Kn NVMe라도
	 * bi_sector/bio_sectors()는 512B 단위로 표현되고, NVMe 드라이버가 제출 직전에
	 * nvme_sect_to_lba()로 실제 LBA 단위로 나눠 변환한다. */
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
	/* [한국어] blktrace/tracepoint에 분할 사건을 기록한다. blkparse나 perf로
	 * "어떤 I/O가 어느 섹터에서 몇 조각으로 쪼개졌는가"를 관찰할 수 있어,
	 * NVMe 성능 분석 시 MDTS/max_segments 튜닝의 근거가 된다. */
	trace_block_split(split, bio->bi_iter.bi_sector);
	/* [한국어] zone write plugging이 걸린 bio는 여기 오면 안 된다는 불변식 검사.
	 * ZNS SSD의 순차 쓰기 보장은 blk-zoned.c의 zone write plug가 순서를 직렬화해
	 * 유지하는데, 그 상태의 bio를 여기서 다시 쪼개 재제출하면 zone write pointer
	 * 순서가 뒤집힐 수 있다. ONCE 변형이라 로그 폭주 없이 한 번만 경고한다. */
	WARN_ON_ONCE(bio_zone_write_plugging(bio));

	/* [한국어] 뒷부분(원본 bio)을 다시 블록 계층 입구로 되돌려 보내는 3-way 분기.
	 * 이 재제출 덕분에 뒷부분도 다시 분할 검사를 받아, 아무리 큰 I/O도 결국
	 * queue_limits를 만족하는 조각들로 수렴한다. */
	if (should_fail_bio(bio))
		/* [한국어] fault injection(CONFIG_FAIL_MAKE_REQUEST)이 이 bio를 실패시키도록
		 * 지정한 경우. 테스트 전용 경로로, 즉시 -EIO로 종료시킨다. */
		bio_io_error(bio);
	else if (!blk_throtl_bio(bio))
		/* [한국어] blk_throtl_bio()가 true를 반환하면 cgroup I/O 스로틀이 이 bio를
		 * 자기 대기열에 붙잡아 둔 것이므로 여기서 더 할 일이 없다(나중에 스로틀
		 * 워커가 대신 제출한다). false면 통과이므로 직접 재제출한다.
		 * submit_bio_noacct_nocheck(bio, true)의 두 번째 인자는 "이미 계정 처리를
		 * 마쳤다"는 표시로, I/O 통계가 중복 집계되는 것을 막는다.
		 * 이후 경로: submit_bio_noacct_nocheck → blk_mq_submit_bio →
		 * (재분할/병합) → blk_mq_dispatch → nvme_queue_rq → SQ doorbell. */
		submit_bio_noacct_nocheck(bio, true);

	/* [한국어] 앞부분 split bio를 호출자에게 반환한다. 호출자(bio_split_to_limits의
	 * 상위인 blk_mq_submit_bio)는 이 조각을 이번 라운드에서 request로 만들어
	 * 드라이버로 내려보낸다. */
	return split;
}
EXPORT_SYMBOL_GPL(bio_submit_split_bioset);

/*
 * [한국어]
 * bio_submit_split - 분할 판정 결과(섹터 수 또는 음수 errno)를 실제 분할 동작으로 변환
 *
 * @bio:           분할 대상 bio
 * @split_sectors: 상위 bio_split_*_at() 계열이 계산한 결과.
 *                 음수  = 분할 자체가 불가능한 에러(-EINVAL/-EAGAIN 등)
 *                 0     = 분할 불필요, bio를 그대로 쓰면 됨
 *                 양수  = 이 섹터 위치에서 잘라야 함
 * @return: 이번에 제출해야 할 bio(분할했다면 앞부분, 안 했다면 원본).
 *          에러 처리로 bio를 끝냈다면 NULL — 호출자는 즉시 반환해야 한다.
 *
 * 이 함수는 "판정"과 "실행"을 분리하기 위한 얇은 어댑터다. bio_split_io_at()류는
 * 순수하게 어디서 자를지만 계산하고, 실제 bio_split()/재제출/에러 종료는 전부
 * 여기로 모인다. 덕분에 rw/discard/zone-append/write-zeroes 네 갈래의 분할 정책이
 * 동일한 실행 경로를 공유한다.
 *
 * 실행 컨텍스트: submit_bio 경로(프로세스 컨텍스트). bio_endio()를 호출할 수 있어
 * 완료 콜백이 이 컨텍스트에서 바로 돌 수 있다.
 *
 * 에러 경로: split_sectors < 0이면 errno를 blk_status_t로 변환해 bi_status에 싣고
 * bio_endio()로 즉시 완료시킨다. 이 bio는 NVMe SQ에 도달하지 못한다.
 *
 * 호출 체인:
 *   bio_split_rw / bio_split_zone_append / bio_split_write_zeroes /
 *   __bio_split_discard → [bio_submit_split] → bio_submit_split_bioset → bio_split
 */
static struct bio *bio_submit_split(struct bio *bio, int split_sectors)
{
	/* [한국어] 음수는 "이 bio는 어떻게 잘라도 queue_limits를 만족시킬 수 없다"는 뜻.
	 * -EINVAL(정렬 위반/세그먼트 초과), -EAGAIN(REQ_NOWAIT인데 분할 필요) 등이
	 * 여기로 온다. unlikely()는 정상 I/O에서는 거의 발생하지 않음을 컴파일러에
	 * 알려 분기 예측을 정상 경로 쪽으로 유도한다. */
	if (unlikely(split_sectors < 0)) {
		/* [한국어] 음수 errno(-EINVAL 등)를 블록 계층 상태 코드(BLK_STS_*)로 변환해
		 * bi_status에 기록한다. 상위 파일시스템/DIO는 이 값을 다시 errno로 되돌려
		 * 사용자에게 전달한다. */
		bio->bi_status = errno_to_blk_status(split_sectors);
		/* [한국어] bio를 즉시 완료 처리한다. bi_end_io 콜백이 이 자리에서 동기적으로
		 * 실행되며, 이후 이 bio를 만지면 use-after-free가 되므로 반드시 NULL을
		 * 반환해 호출자가 손을 떼게 한다. */
		bio_endio(bio);
		return NULL;
	}

	/* [한국어] split_sectors == 0이면 이 bio는 이미 queue_limits를 만족하므로 아무
	 * 것도 하지 않고 원본을 그대로 돌려준다. 0이 아니면 실제 분할에 들어간다. */
	if (split_sectors) {
		/* [한국어] gendisk마다 준비된 전용 bio_set(bd_disk->bio_split)에서 분할용
		 * bio를 할당한다. 전역 풀이 아니라 디스크별 mempool을 쓰는 이유는, 메모리
		 * 압박 상황에서 한 디스크의 분할이 다른 디스크의 진행을 막아 데드락을
		 * 일으키는 것을 방지하기 위해서다(forward progress 보장). */
		bio = bio_submit_split_bioset(bio, split_sectors,
				&bio->bi_bdev->bd_disk->bio_split);
		/* [한국어] 분할에 성공한 앞부분 bio에 REQ_NOMERGE를 세운다. 이 bio는 이미
		 * "한계에 꽉 맞춰" 잘린 조각이므로 뒤에 무언가를 더 병합하면 곧바로 한계를
		 * 다시 넘게 된다. 병합 시도 자체를 차단해 자르고-합치고-다시 자르는 낭비를
		 * 없앤다. NVMe 관점에서는 이 조각 하나가 SQ 엔트리 하나로 확정된다는 뜻. */
		if (bio)
			bio->bi_opf |= REQ_NOMERGE;
	}

	/* [한국어] 호출자에게 "지금 처리할 bio"를 넘긴다. 분할했다면 앞부분, 안 했다면
	 * 원본, 에러로 끝냈다면 위에서 이미 NULL을 반환했다. 남은 뒷부분은
	 * bio_submit_split_bioset() 안에서 이미 큐에 재제출되어 있다. */
	return bio;
}

/*
 * [한국어]
 * __bio_split_discard - Discard/Secure Erase bio를 granularity 경계에 맞춰 분할
 *
 * @bio:         REQ_OP_DISCARD 또는 REQ_OP_SECURE_ERASE bio
 * @lim:         queue_limits (discard_granularity, discard_alignment, logical_block_size)
 * @nsegs:  [out] 이 request가 차지할 세그먼트 수. discard는 데이터 버퍼가 없으므로 항상 1.
 * @max_sectors: 한 번에 처리 가능한 최대 섹터 수. 호출자가 discard/secure-erase 중
 *               해당하는 한계를 골라 넘긴다.
 * @return: 이번에 제출할 bio(분할했다면 앞부분). 분할이 불필요하면 원본 그대로.
 *
 * === 왜 필요한가 ===
 * Discard는 데이터를 옮기지 않고 "이 LBA 범위는 이제 무효" 라고 알리는 연산이라
 * 일반 R/W와 한계 계산 방식이 다르다. 크기 제한뿐 아니라 "granularity 경계에
 * 맞춰 잘라야" 장치가 실제로 해당 블록을 회수할 수 있다. 경계를 어긴 조각은
 * 장치가 조용히 무시하는 경우가 많아 성능·수명 이득이 사라진다.
 *
 * === NVMe에서 각 한계의 실제 출처 (이 트리에서 확인) ===
 *   lim->discard_granularity
 *     ← drivers/nvme/host/core.c: max(NPDG/NPDGL, NPDA/NPDAL) × logical_block_size.
 *       NPDG = Namespace Preferred Deallocate Granularity,
 *       NPDA = Namespace Preferred Deallocate Alignment (Identify Namespace).
 *       넷 다 미지원이면 logical_block_size로 폴백한다.
 *   lim->max_hw_discard_sectors
 *     ← ctrl->dmrsl (DMRSL, Dataset Management Range Size Limit, Identify Controller).
 *       dmrsl이 0이면 UINT_MAX(사실상 무제한), ONCS에 DSM 비트가 없으면 0(미지원).
 *   lim->max_discard_segments
 *     ← ctrl->dmrl (DMRL, Dataset Management Ranges Limit), 없으면
 *       NVME_DSM_MAX_RANGES = 256. NVMe DSM 커맨드 하나가 담을 수 있는
 *       nvme_dsm_range 디스크립터 개수의 상한이다.
 * 최종적으로 이 bio는 nvme_setup_discard()에서 nvme_dsm_range[] 배열로 변환되어
 * NVMe Dataset Management(opcode 0x09, Deallocate 속성) 커맨드가 된다.
 *
 * 실행 컨텍스트: submit_bio 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   bio_split_to_limits → bio_split_discard → [__bio_split_discard]
 *     → bio_allowed_max_sectors / bio_submit_split
 */
static struct bio *__bio_split_discard(struct bio *bio,
		const struct queue_limits *lim, unsigned *nsegs,
		unsigned int max_sectors)
{
	unsigned int max_discard_sectors, granularity;	/* [한국어] 한 번에 해제 가능한 최대 섹터 수와 해제 단위 */
	sector_t tmp;					/* [한국어] granularity 경계 계산용 임시값. sector_t 인 이유는
							 * 아래 나눗셈이 64비트 섹터 번호를 다루기 때문이다 */
	unsigned split_sectors;				/* [한국어] 이번에 잘라 낼 앞부분의 섹터 수 */

	/* [한국어] discard bio는 전송할 데이터 페이지가 없어 bvec도 없다. 따라서
	 * "세그먼트 수"는 R/W처럼 계산할 대상이 아니라 상수 1로 고정한다. 실제 DSM
	 * range 개수는 나중에 blk-lib.c의 discard 병합과 nvme_setup_discard()가
	 * 결정하며, 그쪽 상한이 max_discard_segments(=DMRL 또는 256)다. */
	*nsegs = 1;

	/* [한국어] discard_granularity(바이트)를 512B 섹터 단위로 변환한다(>> 9).
	 * max(..., 1U)로 하한을 두는 이유: granularity가 512B 미만이거나 0이면
	 * 시프트 결과가 0이 되어 아래 나눗셈/나머지 연산에서 0으로 나누게 된다.
	 * NVMe에서 이 값은 NPDG/NPDA × logical_block_size에서 온다. */
	granularity = max(lim->discard_granularity >> 9, 1U);

	/* [한국어] 실제 상한은 (a) 장치가 허용하는 max_sectors와 (b) bio 자료구조가
	 * 표현 가능한 최대치 중 작은 쪽이다. 둘 다 만족해야 하므로 min을 취한다. */
	max_discard_sectors = min(max_sectors, bio_allowed_max_sectors(lim));
	/* [한국어] 상한을 granularity의 배수로 내림 정렬한다. 이렇게 해야 잘린 각
	 * 조각의 끝이 항상 granularity 경계에 떨어지고, 그 결과 다음 조각의 시작도
	 * 경계에 맞는다. 경계에 걸치는 부분 블록은 장치가 회수하지 못한다. */
	max_discard_sectors -= max_discard_sectors % granularity;
	/* [한국어] 내림 정렬 결과가 0이 되는 경우 — max_sectors가 granularity보다 작다.
	 * 이 장치에서는 의미 있는 크기로 자를 수 없으므로 분할을 포기하고 원본을
	 * 그대로 돌려준다. 상위에서 그대로 제출되거나 -EOPNOTSUPP으로 끝난다. */
	if (unlikely(!max_discard_sectors))
		return bio;

	/* [한국어] bio 전체가 이미 상한 이내면 자를 필요가 없다. 흔한 경로(파일 하나
	 * 삭제로 인한 소규모 discard)이므로 여기서 바로 반환된다. */
	if (bio_sectors(bio) <= max_discard_sectors)
		return bio;

	/* [한국어] 일단 상한만큼 자르는 것을 기본값으로 삼고, 아래에서 시작 오프셋
	 * (discard_alignment)을 반영해 필요한 만큼 뒤로 당긴다. */
	split_sectors = max_discard_sectors;

	/*
	 * If the next starting sector would be misaligned, stop the discard at
	 * the previous aligned sector.
	 */
	/* [한국어] discard_alignment는 "LBA 0이 granularity 경계로부터 얼마나 밀려
	 * 있는가"를 나타내는 오프셋이다. 파티션이 granularity 경계에 정렬되지 않은
	 * 위치에서 시작할 때 0이 아니게 된다(NVMe에서는 NPDA/NPDAL 기반).
	 *
	 * 여기서 tmp는 "이번 조각이 끝나는 지점이 정렬 경계에서 몇 섹터 넘어섰는가"다.
	 *   tmp = (현재 시작 섹터 + 자를 길이 - 정렬 오프셋)
	 * 를 만든 뒤 granularity로 나눈 나머지를 구한다. */
	tmp = bio->bi_iter.bi_sector + split_sectors -
		((lim->discard_alignment >> 9) % granularity);
	/* [한국어] sector_div()는 64비트 sector_t를 32비트로 나누는 아키텍처 안전 매크로다.
	 * 몫은 tmp에 되쓰고 나머지를 반환하는데, 여기서는 반환값(나머지)만 쓴다.
	 * 32비트 플랫폼에서 do_div 없이 64비트 나눗셈을 하면 링크 에러가 나므로 필수. */
	tmp = sector_div(tmp, granularity);

	/* [한국어] 넘어선 만큼(tmp) 뒤로 당겨서 끝을 정확히 정렬 경계에 맞춘다.
	 * split_sectors > tmp 조건은 당긴 결과가 0 이하가 되는 것을 막는 안전장치다
	 * (0 섹터짜리 조각을 만들면 진행이 멈춘다). 영문 주석의 "stop the discard at
	 * the previous aligned sector"가 바로 이 동작이다. */
	if (split_sectors > tmp)
		split_sectors -= tmp;

	/* [한국어] 계산된 위치에서 실제 분할·재제출을 수행한다. 잘린 앞 조각이
	 * nvme_setup_discard()로 흘러가 DSM range 하나로 변환된다. */
	return bio_submit_split(bio, split_sectors);
}

/*
 * [한국어]
 * bio_split_discard - discard 계열 bio의 분할 진입점(연산 종류별 한계 선택)
 *
 * @bio:   REQ_OP_DISCARD 또는 REQ_OP_SECURE_ERASE bio
 * @lim:   queue_limits
 * @nsegs: [out] 세그먼트 수 (__bio_split_discard가 1로 채운다)
 * @return: 이번에 제출할 bio (분할했다면 앞부분, 아니면 원본)
 *
 * 하는 일은 단 하나 — 두 종류의 "지우기" 연산 중 어느 한계를 쓸지 고르고
 * __bio_split_discard()에 위임한다. 두 연산이 같은 분할 알고리즘을 공유하되
 * 상한만 다르기 때문에 이렇게 분리되어 있다.
 *
 * REQ_OP_DISCARD    : 데이터를 지운다고 보장하지 않고 "이 LBA는 무효"만 알린다.
 *                     NVMe에서는 Dataset Management(opcode 0x09)의 AD(Deallocate)
 *                     속성으로 매핑된다. 읽으면 0이 나올 수도, 옛 데이터가 나올
 *                     수도 있다(DLFEAT 필드가 무엇을 보장하는지 알려준다).
 * REQ_OP_SECURE_ERASE: 데이터가 실제로 복구 불가능하게 지워짐을 보장해야 한다.
 *                     NVMe에서는 Format NVM의 SES(Secure Erase Settings)나
 *                     Sanitize 커맨드가 이 의미에 대응하며, 보장 수준이 다르므로
 *                     별도의 상한(max_secure_erase_sectors)을 둔다.
 *
 * 실행 컨텍스트: submit_bio 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   blk_mq_submit_bio → bio_split_to_limits → [bio_split_discard]
 *     → __bio_split_discard → bio_submit_split
 */
struct bio *bio_split_discard(struct bio *bio, const struct queue_limits *lim,
		unsigned *nsegs)
{
	unsigned int max_sectors;

	/* [한국어] bio_op()는 bi_opf 하위 비트에서 연산 코드만 추출하는 매크로다.
	 * SECURE_ERASE는 "실제로 지워졌음"을 보장해야 하므로 컨트롤러가 별도로 보고한
	 * 상한을 쓴다. NVMe에서는 Sanitize/Format 계열의 능력에서 유도되며, 장치가
	 * 이를 지원하지 않으면 0이 되어 상위 blk_ioctl_secure_erase()가 -EOPNOTSUPP을
	 * 반환한다. */
	if (bio_op(bio) == REQ_OP_SECURE_ERASE)
		max_sectors = lim->max_secure_erase_sectors;
	else
		/* [한국어] 일반 discard 경로. max_discard_sectors는 하드웨어 한계
		 * (max_hw_discard_sectors ← NVMe DMRSL)와 sysfs의 discard_max_bytes
		 * (관리자가 낮출 수 있는 소프트 상한) 중 작은 값으로 blk-settings.c에서
		 * 결정된다. 큰 discard 하나가 컨트롤러를 오래 붙잡아 다른 I/O의 지연을
		 * 키우는 것을 막기 위해 튜닝 가능하게 열어 둔 것이다. */
		max_sectors = lim->max_discard_sectors;

	/* [한국어] 고른 상한을 들고 공통 분할 로직으로 진입한다. */
	return __bio_split_discard(bio, lim, nsegs, max_sectors);
}

/*
 * [한국어]
 * blk_boundary_sectors - I/O가 넘어서면 안 되는 "경계"의 크기를 고른다
 *
 * @lim:       queue_limits
 * @is_atomic: 이 bio가 REQ_ATOMIC(원자적 쓰기)인지 여부
 * @return: 경계 크기(섹터). 0이면 이런 경계 제약이 없다는 뜻.
 *
 * 여기서 말하는 "경계"는 크기 상한(max_sectors)과 다른 개념이다. 상한은 "한 번에
 * 이만큼까지 보낼 수 있다"이고, 경계는 "이 주소 배수를 걸쳐서는 안 된다"이다.
 * 예를 들어 경계가 256섹터일 때 섹터 200에서 시작하는 I/O는 크기가 아무리 작아도
 * 섹터 256을 넘어갈 수 없어서 최대 56섹터까지만 허용된다.
 *
 * === 두 종류의 경계 ===
 * lim->atomic_write_boundary_sectors
 *   원자성이 깨지는 경계. NVMe에서는 Identify Namespace의 NABSPF(Namespace Atomic
 *   Boundary Size Power Fail)에서 유래한다 —
 *   drivers/nvme/host/core.c:nvme_configure_atomic_write()가
 *   boundary = (NABSPF + 1) × logical_block_size 를 계산해
 *   lim->atomic_write_hw_boundary에 넣는다. 이 경계를 걸치는 쓰기는 전원 손실 시
 *   찢어질(torn) 수 있으므로 REQ_ATOMIC 요청이라면 반드시 경계 안에 들어와야 한다.
 *   (참고: FUA와는 무관한 개념이다. FUA는 "휘발성 캐시를 건너뛰고 매체에 쓰라"는
 *    지속성 요구이고, 여기는 "찢어지지 않게 하라"는 원자성 요구다.)
 * lim->chunk_sectors
 *   성능/구조상의 스트라이프 경계. NVMe에서는 Identify Namespace의 NOIOB
 *   (Namespace Optimal IO Boundary)나 ZNS의 zone 크기가 여기에 대입된다
 *   (nvme_set_chunk_sectors()). RAID에서는 스트라이프 폭이 들어간다.
 *
 * 영문 주석이 밝히듯 둘 다 0이 아니면 chunk_sectors가 atomic 경계의 배수여야
 * 하므로, atomic 경계를 반환해도 chunk 경계를 자동으로 만족한다. 그래서 단순히
 * atomic 쪽을 우선 반환하는 것으로 충분하다.
 *
 * 실행 컨텍스트: bio 제출 경로. 순수 계산 함수.
 *
 * 호출 체인:
 *   get_max_io_size → [blk_boundary_sectors]
 *   blk_rq_get_max_sectors → [blk_boundary_sectors]
 */
static inline unsigned int blk_boundary_sectors(const struct queue_limits *lim,
						bool is_atomic)
{
	/*
	 * chunk_sectors must be a multiple of atomic_write_boundary_sectors if
	 * both non-zero.
	 */
	/* [한국어] REQ_ATOMIC이면서 장치가 원자 경계를 보고한 경우에만 그 경계를 쓴다.
	 * 위 영문 주석의 불변식(chunk_sectors는 atomic 경계의 배수) 덕분에 더 촘촘한
	 * atomic 경계를 지키면 chunk 경계도 자동으로 지켜진다. */
	if (is_atomic && lim->atomic_write_boundary_sectors)
		return lim->atomic_write_boundary_sectors;

	/* [한국어] 그 외에는 chunk_sectors(NOIOB / ZNS zone 크기 / RAID 스트라이프).
	 * 설정되지 않았다면 0이며, 호출자는 0을 "경계 제약 없음"으로 해석한다. */
	return lim->chunk_sectors;
}

/*
 * [한국어]
 * get_max_io_size - bio 시작 위치를 고려한 "이번 조각의 최대 섹터 수"를 계산
 *
 * @bio: 분할 여부를 판단할 bio (bi_sector와 bi_opf를 읽는다)
 * @lim: queue_limits
 * @return: 이 bio의 시작 섹터에서부터 한 request로 보낼 수 있는 최대 섹터 수.
 *          bio_split_rw()가 이 값을 바이트로 바꿔 bio_split_io_at()에 넘긴다.
 *
 * === 세 단계로 상한을 좁혀 간다 ===
 * 1) 연산 종류별 기본 상한 선택 (write-zeroes / atomic / 일반)
 * 2) 경계(zone·NOIOB·atomic boundary)까지 남은 거리로 한 번 더 자름
 * 3) 끝을 물리 블록(PBS) 경계에 맞춰 정렬 — 불가능하면 논리 블록(LBS)으로 정렬
 *
 * === 3단계가 왜 중요한가 ===
 * 512e SSD(논리 512B, 물리 4096B)에서 물리 블록 중간에서 끝나는 쓰기는 장치가
 * read-modify-write를 수행해야 해서 눈에 띄게 느려진다. 그래서 커널은 크기를
 * 조금 손해 보더라도 조각의 끝을 물리 블록 경계에 맞춘다. 흥미로운 점은 bio의
 * "시작"이 어긋나 있어도 "끝"을 정렬하면, 다음 조각의 시작이 정렬되므로 이후
 * 조각들은 전부 정렬 상태로 이어진다는 것이다. 영문 주석의 마지막 문장이
 * 말하는 바가 이것이다.
 *
 * === NVMe에서 각 값의 출처 ===
 *   lim->max_sectors        ← max_hw_sectors(= MDTS에서 유도) 와 sysfs
 *                             max_sectors_kb 중 작은 값. MDTS는 Identify
 *                             Controller의 Maximum Data Transfer Size로,
 *                             nvme_mps_to_sectors(ctrl, id->mdts)가 변환한다.
 *                             MDTS = 0이면 무제한(UINT_MAX).
 *   lim->physical_block_size ← Identify Namespace의 NPWG(Namespace Preferred
 *                             Write Granularity) 등에서 유도. 4Kn/512e 구분.
 *   lim->logical_block_size  ← LBA Format(LBAF)의 LBADS.
 *   lim->atomic_write_max_sectors ← NAWUPF 기반(nvme_configure_atomic_write).
 *   lim->max_write_zeroes_sectors ← Write Zeroes 지원 여부와 WZSL 기반
 *                                   (nvme_init_non_mdts_limits).
 *
 * 실행 컨텍스트: submit_bio 경로. 순수 계산 함수로 락을 잡지 않는다.
 *
 * 호출 체인:
 *   bio_split_to_limits → bio_split_rw → [get_max_io_size]
 *     → blk_boundary_sectors / blk_boundary_sectors_left
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
	/* [한국어] 물리 블록 크기를 바이트에서 512B 섹터 단위로 변환. 4Kn/512e NVMe면
	 * 4096 >> 9 = 8섹터가 된다. 아래 정렬 계산이 전부 섹터 단위라 미리 맞춰 둔다. */
	unsigned pbs = lim->physical_block_size >> SECTOR_SHIFT;
	/* [한국어] 논리 블록 크기도 같은 방식으로 섹터 단위 변환. 512B면 1, 4096B면 8.
	 * PBS 정렬이 실패했을 때의 폴백 정렬 단위로 쓰인다. */
	unsigned lbs = lim->logical_block_size >> SECTOR_SHIFT;
	/* [한국어] REQ_ATOMIC: 이 쓰기는 전원 손실 시에도 전부 반영되거나 전혀 반영되지
	 * 않아야 한다(찢어지면 안 됨). 상한과 경계 선택 규칙이 통째로 달라지므로
	 * 먼저 판별해 둔다. NVMe는 이 요청을 nvme_valid_atomic_write()에서 한 번 더
	 * 검증하고, 위반하면 BLK_STS_INVAL로 거부한다. */
	bool is_atomic = bio->bi_opf & REQ_ATOMIC;
	/* [한국어] 걸쳐서는 안 되는 경계 크기(zone/NOIOB/atomic boundary). 0이면 없음. */
	unsigned boundary_sectors = blk_boundary_sectors(lim, is_atomic);
	/* [한국어] max_sectors = 최종 상한, start = 시작이 PBS 경계에서 벗어난 정도,
	 * end = PBS로 내림 정렬한 끝 위치. 아래 3단계에서 차례로 채워진다. */
	unsigned max_sectors, start, end;

	/*
	 * We ignore lim->max_sectors for atomic writes because it may less
	 * than the actual bio size, which we cannot tolerate.
	 */
	/* [한국어] 1단계 — 연산 종류에 따라 기본 상한을 고른다. */
	if (bio_op(bio) == REQ_OP_WRITE_ZEROES)
		/* [한국어] Write Zeroes는 데이터 페이로드를 전송하지 않고 "이 범위를 0으로
		 * 채워라"만 지시하므로, 데이터 전송량 기반 상한(MDTS)과 무관한 별도 한계를
		 * 쓴다. NVMe에서는 Write Zeroes(opcode 0x08) 커맨드의 NLB 필드 표현 범위와
		 * WZSL(Write Zeroes Size Limit)에서 유도된다(nvme_init_non_mdts_limits). */
		max_sectors = lim->max_write_zeroes_sectors;
	else if (is_atomic)
		/* [한국어] 원자적 쓰기는 max_sectors를 의도적으로 무시한다. 위 영문 주석이
		 * 설명하듯 max_sectors가 실제 bio보다 작을 수 있는데, 원자적 쓰기를 쪼개면
		 * 원자성이 깨져 버려서 "작으니까 나눠 보내자"가 성립하지 않는다. 그래서
		 * 반드시 원자성 전용 상한(NAWUPF 유래)만 본다. 그래도 넘치면 나중에
		 * bio_split_io_at()이 -EINVAL로 거부한다 — 조용히 쪼개는 것보다 낫다. */
		max_sectors = lim->atomic_write_max_sectors;
	else
		/* [한국어] 일반 R/W. NVMe에서는 MDTS에서 유도된 max_hw_sectors와 사용자가
		 * /sys/block/nvmeXnY/queue/max_sectors_kb로 낮춘 값 중 작은 쪽이다. */
		max_sectors = lim->max_sectors;

	/* [한국어] 2단계 — 경계 제약 반영. blk_boundary_sectors_left()는 현재 시작
	 * 섹터에서 다음 경계까지 몇 섹터가 남았는지를 돌려준다. 예: zone 크기가
	 * 256MiB인 ZNS에서 zone 끝 근처에 있다면 남은 거리만큼만 허용된다.
	 * 이 처리가 없으면 ZNS zone 경계를 넘는 쓰기가 만들어져 컨트롤러가
	 * Zone Boundary Error로 거부한다. boundary_sectors == 0이면 이 블록을
	 * 통째로 건너뛴다(제약 없음). */
	if (boundary_sectors) {
		max_sectors = min(max_sectors,
			blk_boundary_sectors_left(bio->bi_iter.bi_sector,
					      boundary_sectors));
	}

	/* [한국어] 3단계 — 끝을 물리 블록 경계에 맞춘다.
	 * start: 시작 섹터가 물리 블록 경계에서 얼마나 밀려 있는지(0 ~ pbs-1).
	 * pbs는 항상 2의 거듭제곱이라 (pbs - 1) 마스크로 나머지를 구할 수 있다. */
	start = bio->bi_iter.bi_sector & (pbs - 1);
	/* [한국어] end: (밀린 정도 + 최대 길이)를 물리 블록 경계로 내림 정렬한 위치.
	 * ~(pbs - 1)은 하위 비트를 지우는 내림 정렬 마스크다. start를 더해서 계산하는
	 * 이유는 "절대 섹터 주소"가 아니라 "물리 블록 경계 기준 상대 위치"에서 정렬을
	 * 따져야 하기 때문이다. */
	end = (start + max_sectors) & ~(pbs - 1);
	/* [한국어] end > start이면 물리 블록 경계가 구간 안에 하나 이상 존재한다는 뜻.
	 * (end - start)가 곧 "끝이 물리 블록 경계에 딱 맞는" 길이다. 원래 max_sectors
	 * 보다 조금 짧아지지만, read-modify-write를 피하는 편이 훨씬 이득이다. */
	if (end > start)
		return end - start;
	/* [한국어] end <= start이면 구간 안에 물리 블록 경계가 없다 — 애초에 max_sectors가
	 * 물리 블록 하나보다 작은 경우다. 물리 정렬은 포기하고 최소한 논리 블록 경계에만
	 * 맞춰 반환한다. 하드웨어는 논리 블록 미만 단위를 받지 못하므로 이 정렬은 선택이
	 * 아니라 필수다. */
	return max_sectors & ~(lbs - 1);
}

/*
 * [한국어]
 * bvec_split_segs - bvec 하나를 세그먼트 여러 개로 쪼개 담고, 다 담기지 않으면 알린다
 *
 * @lim:       queue_limits (max_segment_size, virt_boundary_mask 참조)
 * @bv:        검사 대상 bvec (멀티페이지 bvec이면 수 MiB에 이를 수 있다)
 * @nsegs:     [in,out] 지금까지 쌓인 세그먼트 수. 이 bvec에서 담아낸 만큼 증가한다.
 * @bytes:     [in,out] 지금까지 쌓인 바이트 수. 담아낸 만큼 증가한다.
 * @max_segs:  세그먼트 수 상한 (lim->max_segments)
 * @max_bytes: 바이트 수 상한
 * @return: true = 이 bvec를 전부 담지 못했다 → 호출자는 분할해야 한다.
 *          false = 전부 담았다 → 다음 bvec로 진행해도 된다.
 *
 * === "세그먼트"란 무엇인가 ===
 * 세그먼트 = DMA 디스크립터 하나로 기술 가능한 연속 물리 메모리 조각.
 * 커널의 bvec는 멀티페이지(physically contiguous 여러 페이지)를 하나로 묶어
 * 표현할 수 있는데, 그 크기가 장치가 허용하는 디스크립터 하나의 최대 길이
 * (max_segment_size)를 넘으면 여러 세그먼트로 쪼개야 한다. 그래서 "bvec 개수"와
 * "세그먼트 개수"는 같지 않다 — 이 함수가 그 변환을 담당한다.
 *
 * === NVMe에서 이 개수가 갖는 의미 ===
 * SGL 모드: 세그먼트 하나가 nvme_sgl_desc 하나에 대응한다. 그래서 상한
 *   NVME_MAX_SEGS = NVME_CTRL_PAGE_SIZE / sizeof(struct nvme_sgl_desc)
 *   = 4096 / 16 = 256 으로, 디스크립터 페이지 한 장에 들어가는 개수다
 *   (drivers/nvme/host/pci.c:128).
 * PRP 모드: PRP는 링크 리스트로 무한히 확장 가능해서 세그먼트 수 자체가 직접
 *   제약이 되지 않는다 — 대신 앞서 본 virt_boundary_mask(페이지 정렬) 제약이
 *   훨씬 강하게 작동한다. pci.c의 영문 주석 "For PRPs, segments don't matter at
 *   all."이 이 사실을 명시한다.
 *
 * 실행 컨텍스트: submit_bio 경로. 순수 계산이며 락을 잡지 않는다.
 *
 * 호출 체인:
 *   bio_split_io_at → [bvec_split_segs] → get_max_segment_size / bvec_phys
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
	/* [한국어] 바이트 예산에서 아직 남은 양. 이미 max_bytes까지 채웠다면 0이 되어
	 * 아래 루프가 한 번도 돌지 않는다. */
	unsigned max_len = max_bytes - *bytes;
	/* [한국어] 이번 bvec에서 실제로 담아볼 길이 = min(bvec 전체 길이, 남은 예산).
	 * bvec가 예산보다 크면 앞부분만 담고 나머지는 다음 bio로 넘어간다. */
	unsigned len = min(bv->bv_len, max_len);
	/* [한국어] 이 bvec에서 지금까지 실제로 담아낸 누적 바이트. 루프가 끝난 뒤
	 * *bytes에 더해지고, bvec_phys()에 더해 다음 세그먼트의 물리 시작 주소를
	 * 구하는 데에도 쓰인다. */
	unsigned total_len = 0;
	/* [한국어] 이번 반복에서 잘라낸 세그먼트 하나의 크기. 루프 밖에서 선언한 것은
	 * 단순히 재사용을 위한 것이고, 루프 종료 후 값은 사용되지 않는다. */
	unsigned seg_size = 0;

	/* [한국어] 남은 길이가 있고 세그먼트 예산도 남아 있는 동안 반복한다.
	 * 두 조건 중 어느 쪽이 먼저 걸리는지가 중요하다:
	 *   len == 0으로 끝나면 → 이 bvec을 전부 담았다(정상).
	 *   *nsegs == max_segs로 끝나면 → 세그먼트가 동나 못 담았다 → 분할 필요. */
	while (len && *nsegs < max_segs) {
		/* [한국어] bvec_phys(bv) + total_len = 이번 세그먼트가 시작할 물리 주소.
		 * get_max_segment_size()는 그 주소에서 시작해 (a) max_segment_size와
		 * (b) seg_boundary_mask(장치가 넘지 못하는 DMA 경계)를 모두 지키면서
		 * 뻗을 수 있는 최대 길이를 돌려준다. 즉 "여기서부터 한 디스크립터로
		 * 얼마까지 표현 가능한가"의 답이다. */
		seg_size = get_max_segment_size(lim, bvec_phys(bv) + total_len, len);

		/* [한국어] 세그먼트 하나를 확정했으므로 카운터를 올린다. 이 값이 최종적으로
		 * request->nr_phys_segments가 되고, NVMe SGL 모드에서는 곧 디스크립터
		 * 개수가 된다. */
		(*nsegs)++;
		/* [한국어] 담아낸 누적 길이를 갱신 — 다음 세그먼트의 시작 오프셋이 된다. */
		total_len += seg_size;
		/* [한국어] 남은 길이를 줄인다. 0이 되면 루프가 정상 종료된다. */
		len -= seg_size;

		/* [한국어] 지금 도달한 위치가 virt boundary(NVMe PRP 모드에서는 4KiB 페이지
		 * 경계)에 걸리지 않았다면 == 페이지 중간에서 세그먼트가 끝났다면, 그 뒤를
		 * 이어 붙이는 순간 "페이지 중간에서 시작하는 PRP 엔트리"가 만들어져 사양
		 * 위반이 된다. 그래서 여기서 즉시 멈춘다.
		 * 주의: 조건이 참(비트가 남아 있음 = 경계에 안 맞음)일 때 break라는 점.
		 * mask가 0인 SGL 모드에서는 항상 거짓이라 이 break가 발동하지 않는다. */
		if ((bv->bv_offset + total_len) & lim->virt_boundary_mask)
			break;
	}

	/* [한국어] 이 bvec에서 담아낸 만큼을 호출자의 누적 바이트에 반영한다.
	 * 호출자(bio_split_io_at)는 이 값을 최종 분할 지점 계산에 쓴다. */
	*bytes += total_len;

	/* tell the caller to split the bvec if it is too big to fit */
	/* [한국어] 두 가지 중 하나라도 참이면 "다 못 담았으니 분할하라"고 알린다.
	 *   len > 0            : 세그먼트 예산이 동나거나 virt boundary에서 멈춰
	 *                        bvec의 뒷부분이 남았다.
	 *   bv->bv_len > max_len: 애초에 bvec가 바이트 예산보다 커서 위에서 min()으로
	 *                        잘라 들어왔다. 담긴 부분은 유효하지만 나머지는 다음
	 *                        bio 몫이다.
	 * true를 받은 bio_split_io_at()은 goto split으로 빠져 지금까지 누적된 bytes를
	 * 분할 지점으로 확정한다. */
	return len > 0 || bv->bv_len > max_len;
}

/*
 * [한국어]
 * bio_split_alignment - 분할 지점이 지켜야 할 정렬 단위(바이트)를 고른다
 *
 * @bio: 분할 대상 bio (연산이 쓰기인지 판별)
 * @lim: queue_limits
 * @return: 분할 크기를 내림 정렬할 단위(바이트)
 *
 * 분할 지점을 아무 데나 잡으면 안 된다. 최소한 논리 블록 경계여야 하고, ZNS처럼
 * 더 큰 쓰기 단위를 요구하는 장치라면 그 단위에 맞춰야 한다. bio_split_io_at()이
 * 마지막에 ALIGN_DOWN(bytes, 이 함수의 반환값)으로 분할 크기를 다듬는다.
 *
 * zone_write_granularity가 쓰기에만 적용되는 이유: ZNS의 순차 쓰기 제약은 zone
 * write pointer를 전진시키는 쓰기에만 걸린다. 읽기는 zone 안 어디든 임의 접근이
 * 가능하므로 논리 블록 정렬만으로 충분하다.
 *
 * NVMe ZNS에서 zone_write_granularity는 drivers/nvme/host/zns.c가 설정하며,
 * 보통 논리 블록 크기와 같지만 컨트롤러가 더 큰 값을 요구할 수도 있다.
 *
 * 실행 컨텍스트: submit_bio 경로. 순수 계산 함수.
 *
 * 호출 체인:
 *   bio_split_io_at → [bio_split_alignment]
 */
static unsigned int bio_split_alignment(struct bio *bio,
		const struct queue_limits *lim)
{
	/* [한국어] 쓰기이면서 장치가 zone write granularity를 보고한 경우(= ZNS SSD).
	 * op_is_write()는 WRITE/WRITE_ZEROES/ZONE_APPEND 등 쓰기 계열 opcode를 모두
	 * 참으로 판정한다. 이 단위를 어기면 zone write pointer가 어긋나 컨트롤러가
	 * Zone Invalid Write 오류를 반환한다. */
	if (op_is_write(bio_op(bio)) && lim->zone_write_granularity)
		return lim->zone_write_granularity;
	/* [한국어] 일반 경로 — 논리 블록 크기. NVMe에서는 LBAF의 LBADS(보통 512 또는
	 * 4096). 하드웨어가 받을 수 있는 최소 단위이므로 이보다 작게 자를 수 없다. */
	return lim->logical_block_size;
}

/*
 * [한국어]
 * bvec_seg_gap - 인접한 두 bvec 이음매의 "정렬 상태 비트"를 한 워드로 압축
 *
 * @bvprv: 앞 bvec
 * @bv:    뒤 bvec
 * @return: 두 위치를 OR한 값. 하위 비트가 0일수록 잘 정렬된 이음매다.
 *
 * === 이 이상한 OR 연산의 의도 ===
 * 반환값은 주소도 길이도 아니고 "정렬 정보"다. 두 값을 OR하면
 *   - bv->bv_offset            : 뒤 조각이 페이지 안에서 시작하는 오프셋
 *   - bvprv->bv_offset+bv_len  : 앞 조각이 페이지 안에서 끝나는 오프셋
 * 중 어느 하나라도 2^k 자리에 1이 있으면 결과의 2^k 자리도 1이 된다.
 * 따라서 결과의 하위 k비트가 전부 0이라는 것은 "두 위치 모두 2^k로 정렬되어
 * 있다"와 정확히 같은 뜻이다.
 *
 * 호출자 bio_split_io_at()은 이 값을 모든 이음매에 걸쳐 계속 OR로 누적한 뒤
 * (gaps |= ...), 마지막에 ffs(gaps)로 "가장 낮은 1비트의 위치"를 구해
 * bio->bi_bvec_gap_bit에 저장한다. 즉 bio 전체를 통틀어 가장 나쁜 이음매의
 * 정렬 수준이 한 정수에 요약된다. 값이 클수록 정렬이 좋다.
 *
 * === 이 요약값이 최종적으로 쓰이는 곳: IOMMU IOVA 병합 판단 ===
 * bi_bvec_gap_bit는 request로 승격될 때 rq->phys_gap_bit에 복사되고
 * (blk-mq.c:5091, blk-map.c:1082), 병합이 일어날 때마다 bio_seg_gap()이
 * min_not_zero로 갱신한다(더 나쁜 이음매가 생기면 값이 내려간다).
 *
 * 최종 소비자는 block/blk-mq-dma.c의 blk_can_dma_map_iova()다:
 *   return !(req_phys_gap_mask(req) & dma_get_merge_boundary(dma_dev));
 * req_phys_gap_mask()는 phys_gap_bit 이상의 비트를 1로 채운 마스크를 만들고
 * (include/linux/blk-mq.h), dma_get_merge_boundary()는 IOMMU가 세그먼트를 하나의
 * IOVA 범위로 이어 붙일 때 요구하는 경계를 돌려준다. 두 마스크가 겹치면 이
 * request의 정렬이 IOMMU의 병합 요구보다 거칠다는 뜻이므로 IOVA 병합 경로를
 * 포기하고 세그먼트별 직접 매핑으로 내려간다.
 *
 * NVMe 관점에서 이 판단의 결과는 크다. IOVA 병합에 성공하면 물리적으로 흩어진
 * 여러 페이지가 IOMMU에서 하나의 연속된 IOVA 구간으로 보이므로, PRP 엔트리나
 * SGL 디스크립터 개수가 극적으로 줄어든다. 실패하면 세그먼트마다 디스크립터를
 * 만들어야 해서 커맨드 준비 비용과 컨트롤러 fetch 비용이 모두 늘어난다.
 * 즉 이 한 줄의 OR 연산이 NVMe 커맨드의 최종 모양에까지 영향을 준다.
 *
 * 실행 컨텍스트: submit_bio 경로, bvec 순회 루프 내부(핫패스). inline 필수.
 *
 * 호출 체인:
 *   bio_split_io_at (bvec 루프) → [bvec_seg_gap]
 */
static inline unsigned int bvec_seg_gap(struct bio_vec *bvprv,
					struct bio_vec *bv)
{
	/* [한국어] 뒤 조각의 시작 오프셋과 앞 조각의 끝 오프셋을 OR한다. 덧셈이 아니라
	 * OR인 이유는 위 설명대로 "값"이 아니라 "어느 비트 자리까지 정렬되었는가"를
	 * 모으는 것이 목적이기 때문이다. */
	return bv->bv_offset | (bvprv->bv_offset + bvprv->bv_len);
}

/*
 * [한국어]
 * bio_split_io_at - bio를 처음부터 훑으며 "어디서 잘라야 하는지"를 판정하는 핵심 함수
 *
 * @bio:            분할 여부를 판정할 bio
 * @lim:            queue_limits (dma_alignment, max_segments, max_fast_segment_size,
 *                  virt_boundary_mask, logical_block_size 등을 모두 참조)
 * @segs:      [out] 앞쪽 조각이 갖게 될 세그먼트 수. 호출자가 request의
 *                  nr_phys_segments로 넘겨 최종적으로 NVMe 디스크립터 개수가 된다.
 * @max_bytes:      이 조각이 담을 수 있는 최대 바이트(get_max_io_size 결과의 바이트 환산)
 * @len_align_mask: 각 bvec 길이가 지켜야 할 정렬 마스크. 호출 경로마다 다르다
 *                  (일반 R/W는 0, 무결성/암호화가 걸리면 더 엄격해진다).
 * @return: 음수  = 이 bio는 분할로도 한계를 만족시킬 수 없음(-EINVAL/-EAGAIN)
 *          0     = 분할 불필요, 그대로 제출 가능
 *          양수  = 이 섹터 위치에서 잘라야 함
 *
 * === 이 파일에서 가장 중요한 함수인 이유 ===
 * NVMe로 내려가는 모든 R/W I/O는 예외 없이 이 함수를 통과한다. 여기서 세는
 * 세그먼트 수(nsegs)가 그대로 request->nr_phys_segments가 되고, 그 값이
 * nvme_queue_rq() → blk_rq_dma_map_iter_start() 경로에서 PRP 리스트를 쓸지
 * SGL을 쓸지, 디스크립터 페이지를 몇 장 할당할지를 결정한다. 즉 "블록 계층의
 * 판정"과 "NVMe 커맨드의 물리적 모양"이 만나는 지점이다.
 *
 * === 세 가지 한계를 동시에 본다 ===
 * 1) 정렬     — bvec의 시작 오프셋과 길이가 dma_alignment(NVMe는 3, 즉 4바이트)와
 *               len_align_mask를 지키는가. 위반하면 즉시 -EINVAL(분할해도 못 고침).
 * 2) 경계     — 이전 bvec와의 이음매가 virt_boundary_mask(NVMe PRP 모드에서 4KiB)를
 *               위반하는가. 위반하면 그 지점에서 자른다.
 * 3) 용량     — 세그먼트 수가 max_segments를, 누적 바이트가 max_bytes를 넘는가.
 *               넘으면 그 지점에서 자른다.
 *
 * === 반환값 규약이 세 갈래인 이유 ===
 * 호출자 bio_submit_split()이 "에러 / 분할 불필요 / 분할 필요" 세 경우를 한 번의
 * 정수 비교로 구분할 수 있게 하기 위해서다. 별도의 out 파라미터나 에러 포인터
 * 없이 int 하나로 세 상태를 표현하는 커널의 흔한 관용구다.
 *
 * 실행 컨텍스트: submit_bio 경로(프로세스 컨텍스트, 일부 재제출 경로에서는
 * 워커 컨텍스트). 락을 잡지 않으며 bio의 bvec 배열을 읽기만 한다. 단
 * bi_bvec_gap_bit와 bi_opf(REQ_POLLED 해제)는 수정한다.
 *
 * 에러 경로: -EINVAL은 정렬 위반/원자적 쓰기 분할 불가/유효한 블록 경계 부재,
 * -EAGAIN은 REQ_NOWAIT bio가 분할을 필요로 하는 경우. 둘 다 bio_submit_split()이
 * bio_endio()로 즉시 완료시켜 상위(주로 io_uring/DIO)에 전달한다.
 *
 * 호출 체인:
 *   blk_mq_submit_bio → bio_split_to_limits → bio_split_rw / bio_split_zone_append
 *     → bio_split_rw_at → [bio_split_io_at]
 *       → bvec_gap_to_prev / bvec_seg_gap / bvec_split_segs / bio_split_alignment
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
	/* [한국어] 이 bio에 인라인 암호화 컨텍스트가 붙어 있는지 확인한다. fscrypt가
	 * 하드웨어 인라인 암호화(blk-crypto)를 쓰는 경우에만 NULL이 아니다. */
	struct bio_crypt_ctx *bc = bio_crypt_ctx(bio);
	/* [한국어] bv = 현재 순회 중인 bvec, bvprv = 직전 bvec의 복사본,
	 * bvprvp = bvprv를 가리키는 포인터이자 "직전 bvec이 존재하는가"의 플래그.
	 * 첫 반복에서는 NULL이라 이음매 검사를 건너뛴다. */
	struct bio_vec bv, bvprv, *bvprvp = NULL;
	/* [한국어] nsegs = 누적 세그먼트 수(→ nr_phys_segments → NVMe 디스크립터 수),
	 * bytes = 누적 바이트(→ 분할 지점),
	 * gaps  = 모든 이음매의 정렬 비트를 OR로 누적한 값(→ bi_bvec_gap_bit). */
	unsigned nsegs = 0, bytes = 0, gaps = 0;
	/* [한국어] bio_for_each_bvec()이 사용할 순회 상태. 원본 bi_iter를 건드리지
	 * 않도록 매크로가 내부에서 복사본을 관리한다. */
	struct bvec_iter iter;
	/* [한국어] 각 bvec의 시작 오프셋이 지켜야 할 정렬 마스크. 기본값은 장치의
	 * dma_alignment로, NVMe는 drivers/nvme/host/core.c에서 lim->dma_alignment = 3
	 * (4바이트 정렬)을 설정한다. NVMe 사양이 데이터 포인터의 하위 2비트를 0으로
	 * 요구하기 때문이다. */
	unsigned start_align_mask = lim->dma_alignment;

	/* [한국어] 인라인 암호화가 걸린 bio는 정렬 요구가 더 엄격해진다.
	 * 암호화 엔진은 data_unit_size(보통 4096B) 단위로만 암복호를 수행하므로,
	 * 그 단위 중간에서 조각이 시작하거나 끝나면 IV/블록 경계가 어긋나 잘못된
	 * 데이터가 만들어진다. 그래서 시작 정렬과 길이 정렬 양쪽 마스크에
	 * (data_unit_size - 1)을 OR해 요구 조건을 강화한다.
	 * OR로 합치는 이유: 두 제약을 모두 만족해야 하므로 더 엄격한 쪽(비트가 많은
	 * 쪽)이 자동으로 선택된다.
	 * 주의: 이것은 NVMe 자체 기능이 아니라 호스트/컨트롤러의 인라인 암호화
	 * (예: UFS ICE, eMMC CQHCI, 또는 blk-crypto-fallback의 소프트웨어 경로)
	 * 를 위한 것으로, TCG Opal/SED(block/sed-opal.c)의 자기암호화와는 별개다. */
	if (bc) {
		start_align_mask |= (bc->bc_key->crypto_cfg.data_unit_size - 1);
		len_align_mask |= (bc->bc_key->crypto_cfg.data_unit_size - 1);
	}

	/* [한국어] bio의 모든 bvec을 앞에서부터 순회한다. bio_for_each_bvec()은
	 * 멀티페이지 bvec을 쪼개지 않고 통째로 넘겨주는(= _segment 버전과 다른)
	 * 매크로라, 물리적으로 연속인 큰 덩어리를 한 번에 볼 수 있어 효율적이다.
	 * 루프를 끝까지 완주하면 분할 불필요(0 반환), 중간에 goto split으로 빠지면
	 * 그 시점의 bytes가 분할 지점이 된다. */
	bio_for_each_bvec(bv, bio, iter) {
		/* [한국어] 정렬 위반은 분할로 해결되지 않는 치명적 오류다. 조각을 어떻게
		 * 나눠도 시작 주소나 길이가 어긋난 채로 남기 때문이다. 따라서 goto split이
		 * 아니라 즉시 -EINVAL로 반환한다.
		 * 이런 bio가 오는 경우는 대개 O_DIRECT 사용자가 정렬되지 않은 버퍼를
		 * 넘겼을 때이며, 최종적으로 사용자에게 EINVAL이 전달된다. */
		if (bv.bv_offset & start_align_mask ||
		    bv.bv_len & len_align_mask)
			return -EINVAL;

		/*
		 * If the queue doesn't support SG gaps and adding this
		 * offset would create a gap, disallow it.
		 */
		/* [한국어] 두 번째 bvec부터(bvprvp != NULL) 이음매를 검사한다. */
		if (bvprvp) {
			/* [한국어] bvec_gap_to_prev()는 "직전 bvec이 페이지 끝까지 꽉 차지
			 * 않았는데 현재 bvec이 페이지 오프셋 0이 아닌 곳에서 시작하는가"를
			 * virt_boundary_mask로 검사한다. 참이면 NVMe PRP로 표현 불가능한
			 * 조합이므로 여기서 잘라야 한다 → goto split.
			 * SGL 모드(mask == 0)에서는 항상 거짓이라 분할되지 않는다. */
			if (bvec_gap_to_prev(lim, bvprvp, bv.bv_offset))
				goto split;
			/* [한국어] 경계는 지켰지만 정렬 수준은 기록해 둔다. 모든 이음매의
			 * 정렬 비트를 OR로 누적해, 나중에 ffs()로 "가장 나쁜 이음매"를
			 * 뽑아 bi_bvec_gap_bit에 저장한다. 이 값이 IOMMU IOVA 병합 가부를
			 * 결정해 NVMe 디스크립터 개수에 영향을 준다. */
			gaps |= bvec_seg_gap(bvprvp, &bv);
		}

		/* [한국어] 빠른 경로 — 세 조건을 모두 만족하면 이 bvec 전체가 세그먼트
		 * 하나로 깔끔하게 들어간다. 조건별 의미:
		 *   nsegs < max_segments            : 세그먼트 예산이 남았는가
		 *   bytes + bv_len <= max_bytes     : 바이트 예산이 남았는가
		 *   bv_offset + bv_len <= max_fast_segment_size
		 *       : 이 bvec이 "쪼갤 필요 없음이 자명한" 크기인가. 이 상한 이하이면
		 *         get_max_segment_size()를 불러 볼 것도 없이 한 세그먼트임이
		 *         보장되므로, 흔한 경우에 느린 경로를 통째로 건너뛴다. */
		if (nsegs < lim->max_segments &&
		    bytes + bv.bv_len <= max_bytes &&
		    bv.bv_offset + bv.bv_len <= lim->max_fast_segment_size) {
			/* [한국어] 세그먼트 하나 추가 — 최종적으로 NVMe 디스크립터 하나. */
			nsegs++;
			/* [한국어] 누적 바이트 갱신 — 분할 지점 계산의 기준. */
			bytes += bv.bv_len;
		} else {
			/* [한국어] 느린 경로 — 셋 중 하나라도 걸렸다. bvec_split_segs()가
			 * 이 bvec을 max_segment_size 단위로 쪼개 담을 수 있는 만큼 담고,
			 * 다 담지 못했으면 true를 반환한다. 그때가 분할 지점이다.
			 * nsegs와 bytes는 이 함수가 직접 갱신한다(in/out 파라미터). */
			if (bvec_split_segs(lim, &bv, &nsegs, &bytes,
					lim->max_segments, max_bytes))
				goto split;
		}

		/* [한국어] 현재 bvec을 "직전 bvec"으로 저장한다. 포인터가 아니라 값 복사인
		 * 이유: bio_for_each_bvec()의 bv는 매 반복마다 덮어써지는 스택 변수라,
		 * 주소만 들고 있으면 다음 반복에서 내용이 바뀌어 버린다. */
		bvprv = bv;
		/* [한국어] 포인터를 세워 다음 반복부터 이음매 검사가 활성화되게 한다. */
		bvprvp = &bvprv;
	}

	/* [한국어] 루프를 끝까지 완주 = 모든 bvec이 한계 안에 들어왔다 = 분할 불필요. */
	*segs = nsegs;
	/* [한국어] 누적된 정렬 비트에서 가장 낮은 1비트 위치를 뽑아 bio에 기록한다.
	 * gaps == 0(이음매가 전부 완벽 정렬이거나 bvec이 하나뿐)이면 ffs(0) == 0이
	 * 되어 "제약 없음"을 뜻하게 된다. 이 값은 request로 승격될 때
	 * rq->phys_gap_bit로 복사된다(blk-mq.c). */
	bio->bi_bvec_gap_bit = ffs(gaps);
	/* [한국어] 0 = "자를 필요 없음". 호출자는 원본 bio를 그대로 제출한다. */
	return 0;
split:
	/* [한국어] 여기 도달했다는 것은 "잘라야 한다"는 판정이 났다는 뜻이다.
	 * 하지만 자르면 안 되는 bio가 두 종류 있어, 실제로 자르기 전에 걸러낸다. */

	/* [한국어] (1) 원자적 쓰기. REQ_ATOMIC은 "전부 반영되거나 전혀 반영되지 않아야
	 * 한다"는 요구인데, 쪼개서 여러 커맨드로 보내면 그중 일부만 성공하는 상태가
	 * 가능해져 원자성이 근본적으로 깨진다. 조용히 쪼개는 것보다 -EINVAL로 실패를
	 * 알리는 편이 옳다. NVMe에서는 NAWUPF/NABSPF가 정한 단위를 넘는 요청이
	 * 여기로 오며, 사용자는 EINVAL을 받고 더 작은 단위로 재시도해야 한다. */
	if (bio->bi_opf & REQ_ATOMIC)
		return -EINVAL;

	/*
	 * We can't sanely support splitting for a REQ_NOWAIT bio. End it
	 * with EAGAIN if splitting is required and return an error pointer.
	 */
	/* [한국어] (2) REQ_NOWAIT — "블로킹이 발생할 것 같으면 즉시 EAGAIN으로 실패하라"
	 * 는 요구다(io_uring의 IOSQE_ASYNC 미지정 제출, preadv2/pwritev2의 RWF_NOWAIT).
	 * 분할은 bio 할당(GFP_NOIO)과 재제출을 수반해 블로킹 가능성이 있고, 무엇보다
	 * 뒷부분 bio가 나중에 별도로 완료되면서 "NOWAIT인데 완료가 지연되는" 모순이
	 * 생긴다. 그래서 분할이 필요하다고 판정되는 순간 -EAGAIN을 돌려준다.
	 * io_uring은 이 EAGAIN을 받아 해당 요청을 블로킹 가능한 워커 스레드로
	 * 재제출하고, 거기서는 NOWAIT 없이 정상적으로 분할된다. */
	if (bio->bi_opf & REQ_NOWAIT)
		return -EAGAIN;

	/* [한국어] 분할이 확정되었으므로 앞쪽 조각의 세그먼트 수를 호출자에게 알린다.
	 * 이 값이 request->nr_phys_segments가 되어 NVMe 디스크립터 개수를 결정한다. */
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
	/* [한국어] 지금까지 누적한 bytes는 bvec 경계에서 끊긴 값이라 논리 블록 경계와
	 * 무관할 수 있다. 하드웨어는 블록 미만 단위를 받지 못하므로 아래로 정렬한다.
	 * 하드웨어 한계를 100% 쓰지 못하더라도(영문 주석의 "even if we do not use the
	 * full hardware limits") 정렬이 우선이다. ZNS 쓰기라면 bio_split_alignment()가
	 * 더 큰 zone_write_granularity를 돌려준다. */
	bytes = ALIGN_DOWN(bytes, bio_split_alignment(bio, lim));
	/* [한국어] 정렬 결과가 0바이트 = "블록 하나조차 담지 못했다"는 뜻으로, 이 bio는
	 * 어떻게 잘라도 유효한 I/O가 될 수 없다. 위 영문 주석이 두 원인을 명시한다:
	 *   (a) 불연속 벡터가 max_segments보다 많아 블록 하나를 채우기 전에 세그먼트가
	 *       동남 — NVMe SGL 모드에서 256개 상한에 걸리는 극단적 스캐터 버퍼.
	 *   (b) virt boundary gap이 블록 경계보다 앞에서 발생 — PRP 모드에서 페이지
	 *       정렬이 어긋난 버퍼.
	 * 분할로 해결 불가능하므로 -EINVAL로 실패시킨다. */
	if (!bytes)
		return -EINVAL;

	/*
	 * Bio splitting may cause subtle trouble such as hang when doing sync
	 * iopoll in direct IO routine. Given performance gain of iopoll for
	 * big IO can be trival, disable iopoll when split needed.
	 */
	/* [한국어] 분할되는 bio에서는 iopoll(REQ_POLLED)을 해제한다.
	 * 이유(영문 주석 요약): 동기 DIO가 iopoll로 완료를 기다릴 때, 제출자는 자기가
	 * 아는 bio 하나만 폴링한다. 그런데 분할로 생긴 뒷부분 bio는 별도로 제출되어
	 * 폴링 대상에 들어오지 않으므로, 아무도 그 완료를 확인하지 않아 영원히 대기하는
	 * 행(hang)이 발생할 수 있다. 큰 I/O에서 iopoll의 이득은 어차피 작으므로
	 * (인터럽트 오버헤드가 전송 시간에 비해 미미) 안전 쪽을 택해 폴링을 끈다.
	 * NVMe 관점: 이 bio는 이제 poll 큐가 아닌 일반 IRQ 큐로 제출되어
	 * 인터럽트로 완료 통지를 받는다. */
	bio_clear_polled(bio);
	/* [한국어] 완주 경로와 동일하게 이음매 정렬 요약을 기록한다. 여기서 기록되는
	 * gaps는 "분할 지점까지" 누적된 값이므로 앞쪽 조각의 성질을 정확히 반영한다. */
	bio->bi_bvec_gap_bit = ffs(gaps);
	/* [한국어] 바이트를 512B 섹터 단위로 변환해 반환한다(>> 9). 양수이므로 호출자는
	 * "이 위치에서 자르라"로 해석하고 bio_submit_split()이 실제 분할을 수행한다. */
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
	int split_sectors;	/* [한국어] 양수면 "여기서 잘라야 한다", 0 이면 분할 불필요, 음수면 에러 */

	/* [한국어] Zone Append 는 **쪼갤 수 없다**. 일반 쓰기라면 앞뒤로 나눠 두 명령을
	 * 보내면 되지만, append 는 기록 위치를 장치가 정해 하나의 LBA 로 돌려주므로
	 * 둘로 나누면 반환할 위치가 둘이 되어 의미가 성립하지 않는다.
	 * 그래서 여기서는 "잘라야 한다"는 결과 자체가 상위 계층의 버그다 —
	 * 애초에 max_zone_append_sectors 를 넘는 append bio 를 만들면 안 된다.
	 * bio_split_rw_at() 을 부르는 것은 자르기 위해서가 아니라 **검사하기 위해서**다. */
	split_sectors = bio_split_rw_at(bio, lim, nr_segs,
			lim->max_zone_append_sectors << SECTOR_SHIFT);
	if (WARN_ON_ONCE(split_sectors > 0))	/* [한국어] 양수 = 잘라야 한다 = 있어서는 안 될 상황 */
		split_sectors = -EINVAL;	/* [한국어] 자르는 대신 에러로 바꿔 제출을 실패시킨다.
					 * 잘못 잘라 조용히 틀린 위치를 보고하는 것보다 실패가 낫다. */
	return bio_submit_split(bio, split_sectors);	/* [한국어] 0 이면 원본 그대로, 음수면 bio 를 에러로 완료시킨다 */
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
	unsigned int nr_phys_segs = 0;	/* [한국어] 누적 세그먼트 수 — 이 함수의 결과 */
	unsigned int bytes = 0;		/* [한국어] 누적 바이트 수. 세그먼트 경계 판정(크기 상한)에 쓰인다 */
	struct req_iterator iter;	/* [한국어] request 안의 모든 bio 를 가로질러 bvec 을 훑는 커서 */
	struct bio_vec bv;		/* [한국어] 순회 중 현재 bvec (복사본으로 받는다) */

	if (!rq->bio)
		return 0;		/* [한국어] bio 가 없는 request(패스스루 중 데이터 없는 명령 등)는 세그먼트도 0 이다 */

	/* rq->bio의 op별로 세그먼트 계산 방식이 다르다. 데이터 버퍼가 없는
	 * Discard/Write Zeroes는 별도 처리. */
	switch (bio_op(rq->bio)) {
	case REQ_OP_DISCARD:
	case REQ_OP_SECURE_ERASE:
		/* NVMe DSM은 여러 range를 하나의 명령에 담을 수 있다.
		 * queue_max_discard_segments가 1이면 range 1개로 계산. */
		if (queue_max_discard_segments(rq->q) > 1) {
			struct bio *bio = rq->bio;	/* [한국어] 체인의 시작 */

			/* [한국어] discard 는 bvec 이 아니라 **bio 하나가 구간 하나**다.
			 * 병합된 discard request 는 bio 체인으로 여러 구간을 들고 있고,
			 * 그 개수가 곧 DSM 명령에 실릴 range 개수가 된다. */
			for_each_bio(bio)
				nr_phys_segs++;
			return nr_phys_segs;
		}
		return 1;	/* [한국어] 장치가 구간 하나만 받는다면 병합도 없었을 테니 1 이다 */
	case REQ_OP_WRITE_ZEROES:
		/* NVMe Write Zeroes는 데이터 버퍼/PRP/SGL이 필요 없다. */
		return 0;
	default:
		break;
	}

	/* [한국어] 일반 R/W — request에 매달린 모든 bio의 모든 bvec을 순회하며
	 * 세그먼트를 다시 센다. rq_for_each_bvec()은 rq->bio부터 bi_next 체인을 따라
	 * 여러 bio를 가로질러 순회하므로, 병합으로 여러 bio가 붙은 request도 하나의
	 * 연속된 버퍼처럼 취급된다.
	 *
	 * 여기서 max_segs에 UINT_MAX, max_bytes에 BIO_MAX_SIZE를 넘기는 것이 핵심이다.
	 * 이 호출의 목적은 "한계를 넘는지 판정"이 아니라 "실제 세그먼트 수를 세는 것"
	 * 이므로, 한계 검사가 세는 도중에 루프를 끊지 못하도록 사실상 무한대를 준다.
	 * 그래서 bvec_split_segs()의 반환값(분할 필요 여부)도 무시한다.
	 *
	 * 결과 nr_phys_segs는 rq->nr_phys_segments에 저장되고, nvme_queue_rq()가
	 * blk_rq_nr_phys_segments()로 읽어 (a) 세그먼트가 1개면 PRP1만 쓰는 최적 경로,
	 * (b) 적으면 PRP 리스트, (c) 많으면 SGL 등 커맨드 구성 방식을 고르는 데 쓴다. */
	rq_for_each_bvec(bv, rq, iter)
		bvec_split_segs(&rq->q->limits, &bv, &nr_phys_segs, &bytes,
				UINT_MAX, BIO_MAX_SIZE);
	/* [한국어] 다시 센 세그먼트 수를 반환한다. 호출자(blk_recalc_rq_segments의
	 * 유일한 소비자인 blk_update_request 등)가 rq->nr_phys_segments에 대입한다. */
	return nr_phys_segs;
}

/*
 * [한국어]
 * blk_rq_get_max_sectors - 이 request가 커질 수 있는 최대 섹터 수를 계산
 *
 * @rq:     대상 request
 * @offset: 경계 계산의 기준이 될 시작 섹터. back-merge면 request의 시작
 *          (blk_rq_pos(rq)), front-merge면 새로 앞에 붙일 bio의 시작 섹터가 온다.
 * @return: 이 request가 가질 수 있는 최대 섹터 수. 호출자는 "현재 크기 + 붙일 크기"가
 *          이 값을 넘으면 병합을 거부한다.
 *
 * get_max_io_size()가 "새 bio를 자를 때"의 상한이라면, 이 함수는 "기존 request에
 * 더 붙일 때"의 상한이다. 둘 다 같은 queue_limits를 보지만 진입 상황이 달라
 * 별도 함수로 존재한다. 특히 offset을 인자로 받는 이유가 중요하다 — front-merge는
 * request의 시작 위치 자체가 앞으로 당겨지므로, 경계까지 남은 거리를 새 시작점
 * 기준으로 다시 계산해야 하기 때문이다.
 *
 * 실행 컨텍스트: 병합 판정 경로(프로세스 컨텍스트). 순수 계산 함수.
 *
 * 호출 체인:
 *   ll_back_merge_fn / ll_front_merge_fn / ll_merge_requests_fn
 *     → [blk_rq_get_max_sectors]
 *       → blk_boundary_sectors / blk_queue_get_max_sectors
 *       → blk_boundary_sectors_left
 */
static inline unsigned int blk_rq_get_max_sectors(struct request *rq,
						  sector_t offset)
{
	/* [한국어] request가 속한 큐와 그 한계 테이블을 지역 변수로 꺼내 둔다.
	 * 아래에서 여러 번 참조하므로 포인터 추적 비용을 줄인다. */
	struct request_queue *q = rq->q;
	struct queue_limits *lim = &q->limits;
	/* [한국어] max_sectors = 크기 상한, boundary_sectors = 걸치면 안 되는 경계 크기. */
	unsigned int max_sectors, boundary_sectors;
	/* [한국어] 원자적 쓰기 여부. get_max_io_size()와 마찬가지로 경계 선택이 달라진다. */
	bool is_atomic = rq->cmd_flags & REQ_ATOMIC;

	/* [한국어] passthrough request는 파일시스템 I/O가 아니라 사용자가 직접 만든
	 * 명령이다(예: nvme-cli가 /dev/nvme0의 ioctl로 보내는 Admin/IO 커맨드,
	 * SG_IO). 이런 요청은 블록 계층이 크기를 다듬거나 병합해서는 안 되고, 오직
	 * 하드웨어가 물리적으로 받을 수 있는 절대 상한(max_hw_sectors, NVMe에서는
	 * MDTS 유래)만 지키면 된다. sysfs로 낮춘 소프트 상한(max_sectors)이나 경계
	 * 제약은 적용하지 않는다 — 사용자가 의도한 명령을 그대로 전달해야 하므로. */
	if (blk_rq_is_passthrough(rq))
		return q->limits.max_hw_sectors;

	/* [한국어] 경계 크기(zone/NOIOB/atomic boundary)를 구한다. 0이면 제약 없음. */
	boundary_sectors = blk_boundary_sectors(lim, is_atomic);
	/* [한국어] 연산 종류별 크기 상한을 구한다. blk_queue_get_max_sectors()는
	 * discard/write-zeroes/atomic/일반을 구분해 각각의 max_*_sectors를 돌려준다
	 * (block/blk.h에 정의). get_max_io_size()의 1단계와 같은 역할이다. */
	max_sectors = blk_queue_get_max_sectors(rq);

	/* [한국어] 경계 제약을 건너뛰는 세 경우:
	 *   boundary_sectors == 0 : 애초에 경계가 없다.
	 *   DISCARD / SECURE_ERASE: 데이터를 전송하지 않고 LBA 범위만 지정하는 연산이라
	 *     zone/NOIOB 같은 전송 경계를 걸쳐도 무방하다. 오히려 큰 범위를 한 커맨드로
	 *     보내는 편이 효율적이다(NVMe DSM은 range 배열로 여러 구간을 담는다).
	 * 이 경우들은 크기 상한만 적용하고 반환한다. */
	if (!boundary_sectors ||
	    req_op(rq) == REQ_OP_DISCARD ||
	    req_op(rq) == REQ_OP_SECURE_ERASE)
		return max_sectors;
	/* [한국어] 그 외에는 크기 상한과 "offset에서 다음 경계까지 남은 거리" 중 작은 쪽.
	 * offset을 인자로 받는 이유가 여기서 드러난다 — front-merge로 시작점이 앞으로
	 * 당겨지면 경계까지의 거리가 달라지므로 호출자가 올바른 기준점을 넘겨야 한다. */
	return min(max_sectors,
		   blk_boundary_sectors_left(offset, boundary_sectors));
}

/*
 * [한국어]
 * ll_new_hw_segment - 병합의 마지막 관문: 정책·용량 검사 후 세그먼트 카운터 갱신
 *
 * @rq/@req:       bio를 흡수할 기존 request
 * @bio:           붙이려는 bio
 * @nr_phys_segs:  @bio가 추가할 물리 세그먼트 수(호출자가 미리 계산해 전달)
 * @return: 1 = 병합 허용(카운터 갱신 완료), 0 = 병합 거부(REQ_NOMERGE 설정됨)
 *
 * "ll"은 low-level의 약자로, 상위 스케줄러가 "이 둘은 LBA가 인접하니 붙이자"고
 * 판단한 뒤 실제로 하드웨어 제약과 정책을 만족하는지 확인하는 최종 검문소다.
 * 검사를 통과하면 부수 효과로 req->nr_phys_segments를 직접 갱신하므로, 이 함수가
 * 1을 반환한 이후에는 호출자가 되돌릴 수 없다(커밋 지점).
 *
 * === 거부 시 REQ_NOMERGE를 설정하는 이유 ===
 * 한 번 한계에 걸린 request는 앞으로도 계속 걸릴 가능성이 높다. 매번 병합을
 * 시도했다 실패하는 것은 순수한 낭비이므로, req_set_nomerge()로 표시해 이후
 * 시도 자체를 차단한다. 병합 실패가 곧 성능 저하로 이어지는 것을 막는 캐싱이다.
 *
 * 실행 컨텍스트: 병합 경로. 스케줄러 병합이면 해당 스케줄러 락(mq-deadline의
 * dd->lock 등) 아래, plug 병합이면 자기 plug 리스트 위(락 없음)에서 실행된다.
 *
 * 호출 체인:
 *   ll_back_merge_fn / ll_front_merge_fn → [ll_new_hw_segment]
 *     → blk_cgroup_mergeable / blk_integrity_merge_bio
 *     → blk_rq_get_max_segments / req_set_nomerge
 */
static inline int ll_new_hw_segment(struct request *req, struct bio *bio,
		unsigned int nr_phys_segs)
{
	/* [한국어] 검사 1 — cgroup 소속. 서로 다른 blkcg에 속한 I/O를 하나의 request로
	 * 합치면, 완료 시점에 그 I/O가 어느 cgroup의 몫이었는지 구분할 수 없게 된다.
	 * blk-throttle의 대역폭 제한이나 blk-iocost의 가중치 배분이 잘못된 cgroup에
	 * 청구되어 격리가 무너지므로, 소속이 다르면 성능을 포기하고 병합하지 않는다.
	 * (cgroup이 설정되지 않은 시스템에서는 항상 true라 비용이 없다.) */
	if (!blk_cgroup_mergeable(req, bio))
		goto no_merge;

	/* [한국어] 검사 2 — 데이터 무결성(T10 PI / NVMe End-to-End Data Protection).
	 * PI가 걸린 I/O는 논리 블록마다 8바이트 보호 정보(guard/apptag/reftag)가
	 * 따라붙고, reftag는 LBA와 연동되어 증가한다. PI 유형이나 사용 여부가 다른
	 * 두 I/O를 합치면 메타데이터 배열의 일관성이 깨져 컨트롤러가 검증에 실패한다
	 * (NVMe에서는 Invalid Protection Information 상태 코드로 되돌아온다). */
	if (blk_integrity_merge_bio(req->q, req, bio) == false)
		goto no_merge;

	/* discard request merge won't add new segment */
	/* [한국어] Discard는 데이터 버퍼가 없어 물리 세그먼트를 늘리지 않는다. 따라서
	 * 아래 세그먼트 용량 검사가 무의미하므로 여기서 조기 성공 반환한다.
	 * (discard의 range 개수 제약은 max_discard_segments로 별도 관리되며,
	 * req_attempt_discard_merge()가 담당한다.) */
	if (req_op(req) == REQ_OP_DISCARD)
		return 1;

	/* [한국어] 검사 3 — 세그먼트 용량. 이것이 하드웨어 한계와 직결되는 핵심 검사다.
	 * blk_rq_get_max_segments()는 일반 I/O면 lim->max_segments를, 무결성 데이터가
	 * 붙은 request면 max_integrity_segments를 돌려준다.
	 * NVMe에서 max_segments는 PCIe의 경우 NVME_MAX_SEGS = 4096/16 = 256으로,
	 * SGL 디스크립터 페이지 한 장에 들어가는 개수다. 이를 넘으면 디스크립터
	 * 페이지를 추가로 할당해야 해서 드라이버가 처리할 수 없다. */
	if (req->nr_phys_segments + nr_phys_segs > blk_rq_get_max_segments(req))
		goto no_merge;

	/*
	 * This will form the start of a new hw segment.  Bump both
	 * counters.
	 */
	/* [한국어] 모든 검사 통과 — 여기서부터는 되돌릴 수 없는 커밋 구간이다.
	 * 물리 세그먼트 수를 누적한다. 이 값이 그대로 nvme_queue_rq()에서
	 * PRP/SGL 디스크립터 개수 결정에 쓰인다. */
	req->nr_phys_segments += nr_phys_segs;
	/* [한국어] 이 bio에 무결성 페이로드(PI 메타데이터)가 붙어 있으면 메타데이터용
	 * 세그먼트 수도 별도로 누적한다. NVMe에서 메타데이터는 데이터와 다른 포인터
	 * (MPTR 또는 메타데이터 SGL)로 전달되므로 카운터가 분리되어 있다. PCIe NVMe의
	 * 메타데이터 세그먼트 상한은 SGL 지원 시 NVME_MAX_META_SEGS, 아니면 1
	 * (MPTR은 단일 주소만 담을 수 있으므로)이다. */
	if (bio_integrity(bio))
		req->nr_integrity_segments += blk_rq_count_integrity_sg(req->q,
									bio);
	/* [한국어] 병합 허용. 호출자가 실제로 bio를 request의 bio 리스트에 연결한다. */
	return 1;

no_merge:
	/* [한국어] 실패 경로 공통 처리 — request에 REQ_NOMERGE를 새겨 앞으로의 병합
	 * 시도를 차단하고, 큐 통계에 병합 실패를 기록한다. 0을 반환하면 호출자는
	 * 이 bio로 새 request를 할당하는 경로로 간다. */
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
	/* [한국어] 검사 1 — 데이터 버퍼의 virt boundary. request의 마지막 bio(biotail)와
	 * 새 bio 사이에 PRP로 표현 불가능한 이음매가 생기는지 본다. NVMe PRP 모드에서
	 * 가장 자주 병합을 막는 조건이다. */
	if (req_gap_back_merge(req, bio))
		return 0;
	/* [한국어] 검사 2 — 무결성(PI) 메타데이터 버퍼의 virt boundary. 데이터 버퍼와
	 * 별개로 메타데이터 버퍼도 자체 DMA 매핑을 가지므로 같은 경계 검사가 필요하다.
	 * blk_integrity_rq()로 PI가 실제로 걸린 request일 때만 검사해 비용을 아낀다. */
	if (blk_integrity_rq(req) &&
	    integrity_req_gap_back_merge(req, bio))
		return 0;
	/* [한국어] 검사 3 — 인라인 암호화 컨텍스트. 두 I/O가 서로 다른 키나 다른
	 * DUN(Data Unit Number, IV 역할)을 쓰면 하나의 요청으로 합칠 수 없다.
	 * back-merge에서는 앞 요청의 DUN이 뒤 bio의 DUN과 연속인지까지 확인한다. */
	if (!bio_crypt_ctx_back_mergeable(req, bio))
		return 0;
	/* [한국어] 검사 4 — 크기 상한. 합친 섹터 수가 이 request가 가질 수 있는 최대치를
	 * 넘는지 본다. NVMe에서 이 상한은 MDTS(Maximum Data Transfer Size)에서 유래하며,
	 * 넘으면 컨트롤러가 커맨드를 거부한다. 기준점으로 blk_rq_pos(req)(request의
	 * 현재 시작 섹터)를 넘기는 이유는 back-merge라 시작점이 바뀌지 않기 때문이다.
	 * 여기서만 REQ_NOMERGE를 세우는 것은, 앞의 세 검사와 달리 "크기 초과"는
	 * 앞으로도 계속 참일 가능성이 매우 높기 때문이다. */
	if (blk_rq_sectors(req) + bio_sectors(bio) >
	    blk_rq_get_max_sectors(req, blk_rq_pos(req))) {
		req_set_nomerge(req->q, req);
		return 0;
	}

	/* [한국어] 네 검사를 모두 통과했으면 최종 관문(cgroup/PI 정책 + 세그먼트 용량)
	 * 으로 넘긴다. 여기서 1이 반환되면 병합이 확정되고 카운터가 갱신된다. */
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
	/* [한국어] 검사 1 — virt boundary. back과 대칭이지만 이음매의 방향이 반대다:
	 * 새 bio가 앞, request의 첫 bio가 뒤가 되는 이음매를 검사한다. */
	if (req_gap_front_merge(req, bio))
		return 0;
	/* [한국어] 검사 2 — PI 메타데이터 버퍼의 경계(front 방향). */
	if (blk_integrity_rq(req) &&
	    integrity_req_gap_front_merge(req, bio))
		return 0;
	/* [한국어] 검사 3 — 인라인 암호화 컨텍스트(front 방향). 새 bio의 DUN이
	 * request 첫 bio의 DUN 바로 앞에 오는 값인지 확인한다. */
	if (!bio_crypt_ctx_front_mergeable(req, bio))
		return 0;
	/* [한국어] 검사 4 — 크기 상한. back-merge와 결정적으로 다른 점은 기준점이
	 * blk_rq_pos(req)가 아니라 bio->bi_iter.bi_sector라는 것이다.
	 * front-merge는 request의 시작 위치를 새 bio의 시작으로 앞당기므로,
	 * zone/NOIOB 경계까지 남은 거리를 반드시 새 시작점 기준으로 계산해야 한다.
	 * 기존 시작점으로 계산하면 경계를 넘는 request가 만들어질 수 있다. */
	if (blk_rq_sectors(req) + bio_sectors(bio) >
	    blk_rq_get_max_sectors(req, bio->bi_iter.bi_sector)) {
		req_set_nomerge(req->q, req);
		return 0;
	}

	/* [한국어] 최종 관문으로 위임. 세그먼트 카운터 갱신은 방향과 무관하므로
	 * back-merge와 같은 함수를 공유한다. */
	return ll_new_hw_segment(req, bio, nr_segs);
}

/*
 * [한국어]
 * req_attempt_discard_merge - 두 discard request를 DSM range 배열 하나로 합칠지 판정
 *
 * @q:    대상 request_queue
 * @req:  기준 request (병합의 주체, 여기에 next가 흡수된다)
 * @next: 흡수될 request
 * @return: true = 병합 가능(req->nr_phys_segments 갱신 완료), false = 불가
 *
 * === discard 병합이 일반 R/W 병합과 다른 점 ===
 * 일반 R/W는 "LBA가 인접해야" 병합할 수 있다. 데이터 버퍼를 하나로 이어야 하기
 * 때문이다. 그런데 discard는 전송할 데이터가 없고 "지울 LBA 범위 목록"만 보내므로,
 * 범위가 인접하지 않아도 목록에 항목을 추가하는 방식으로 얼마든지 합칠 수 있다.
 * 그래서 여기서는 세그먼트 수 대신 "range 개수"가 제약이 된다.
 *
 * NVMe에서 이 범위 목록은 Dataset Management(opcode 0x09) 커맨드의
 * nvme_dsm_range[] 배열로 전달된다(drivers/nvme/host/core.c:nvme_setup_discard).
 * 배열 크기 상한 queue_max_discard_segments(q)는 컨트롤러의 DMRL(Dataset
 * Management Ranges Limit) 또는 기본값 NVME_DSM_MAX_RANGES(256)에서 온다.
 * 여러 개의 흩어진 파일 삭제가 하나의 DSM 커맨드로 묶이는 것이 이 최적화의 효과다.
 *
 * nr_phys_segments 필드를 range 개수 저장용으로 재사용한다는 점에 주의해야 한다.
 * discard에는 물리 세그먼트 개념이 없으므로 같은 필드를 다른 의미로 쓰는 것이며,
 * blk_rq_nr_discard_segments()가 이 해석을 캡슐화한다.
 *
 * 실행 컨텍스트: request 병합 경로. 스케줄러 락 또는 plug 컨텍스트.
 *
 * 호출 체인:
 *   attempt_merge → [req_attempt_discard_merge]
 *     → blk_rq_nr_discard_segments / queue_max_discard_segments
 */
static bool req_attempt_discard_merge(struct request_queue *q, struct request *req,
		struct request *next)
{
	/* [한국어] req가 현재 담고 있는 discard range 개수. nr_phys_segments를 읽되
	 * 0이면 1로 보정해 주는 헬퍼다(range가 없는 discard는 존재하지 않으므로). */
	unsigned short segments = blk_rq_nr_discard_segments(req);

	/* [한국어] 검사 1 — range 배열 용량. 이미 상한에 도달했으면 더 담을 칸이 없다.
	 * NVMe DSM 커맨드의 range 배열은 고정 크기 버퍼로 할당되므로
	 * (core.c의 alloc_size = sizeof(*range) * NVME_DSM_MAX_RANGES) 이 상한을
	 * 넘기면 버퍼 오버런이 된다. */
	if (segments >= queue_max_discard_segments(q))
		goto no_merge;
	/* [한국어] 검사 2 — 총 섹터 수 상한. range 개수와 별개로, 한 커맨드가 다룰 수
	 * 있는 전체 LBA 양에도 한계가 있다(NVMe에서는 DMRSL, Dataset Management
	 * Range Size Limit). next->bio만 보는 것은 discard request 하나가 bio 하나로
	 * 구성되기 때문이다. */
	if (blk_rq_sectors(req) + bio_sectors(next->bio) >
	    blk_rq_get_max_sectors(req, blk_rq_pos(req)))
		goto no_merge;

	/* [한국어] 병합 확정 — 두 request의 range 개수를 더해 저장한다. 앞서 말했듯
	 * nr_phys_segments 필드를 range 개수 용도로 재사용하고 있다. 이후
	 * nvme_setup_discard()가 이 개수만큼 nvme_dsm_range 항목을 채운다. */
	req->nr_phys_segments = segments + blk_rq_nr_discard_segments(next);
	return true;
no_merge:
	/* [한국어] 실패 — 이후 병합 시도를 차단하고 통계에 기록한다. 두 request는
	 * 각각 별도의 DSM 커맨드로 컨트롤러에 전달된다. */
	req_set_nomerge(q, req);
	return false;
}

/*
 * [한국어]
 * ll_merge_requests_fn - 이미 만들어진 두 request를 하나로 합쳐도 되는지 최종 판정
 *
 * @q:    대상 request_queue
 * @req:  앞쪽 request (병합 후 살아남는 쪽)
 * @next: 뒤쪽 request (병합 후 해제되는 쪽)
 * @return: 1 = 병합 가능(req의 카운터 갱신 완료), 0 = 불가
 *
 * ll_back_merge_fn()이 "request + bio"를 다뤘다면 이 함수는 "request + request"를
 * 다룬다. 이 경로가 존재하는 이유: 서로 다른 시점에 도착해 각각 request가 된 두
 * I/O가, 나중에 보니 LBA가 인접해 있는 경우가 있다. I/O 스케줄러(mq-deadline 등)가
 * 정렬된 red-black 트리에서 이런 쌍을 찾아내 뒤늦게 합친다.
 *
 * NVMe 관점에서 이 병합의 이득은 크다. request 두 개는 SQ 엔트리 두 개, doorbell
 * 두 번, 완료 인터럽트 두 번을 뜻하는데, 합치면 각각 절반이 된다. 특히 순차 읽기/
 * 쓰기 워크로드에서 IOPS 한계가 아니라 커맨드 처리 오버헤드가 병목일 때 효과적이다.
 *
 * 이 함수는 "판정 + 카운터 갱신"만 하고, 실제 bio 리스트 연결과 next 해제는
 * 호출자 attempt_merge()가 수행한다.
 *
 * 실행 컨텍스트: 스케줄러 병합 경로. mq-deadline이면 dd->lock을 잡은 상태다.
 *
 * 호출 체인:
 *   elv_attempt_insert_merge / attempt_back_merge / attempt_front_merge
 *     → attempt_merge → [ll_merge_requests_fn]
 *       → req_gap_back_merge / blk_rq_get_max_sectors
 *       → blk_rq_get_max_segments / blk_cgroup_mergeable
 *       → blk_integrity_merge_rq / bio_crypt_ctx_merge_rq
 */
static int ll_merge_requests_fn(struct request_queue *q, struct request *req,
				struct request *next)
{
	/* [한국어] 두 request의 세그먼트 합계를 담을 변수. 검사를 통과해야만
	 * req->nr_phys_segments에 반영된다(검사 도중 포기해도 원본이 손상되지 않도록
	 * 임시 변수를 거친다). */
	int total_phys_segments;

	/* [한국어] 검사 1 — 이음매의 virt boundary. req의 마지막 bio와 next의 첫 bio가
	 * 맞닿는 지점이 NVMe PRP 규칙을 만족하는지 본다. request 대 request라도
	 * 실제로 검사할 대상은 결국 이음매를 이루는 두 bio다. */
	if (req_gap_back_merge(req, next->bio))
		return 0;

	/*
	 * Will it become too large?
	 */
	/* [한국어] 검사 2 — 크기 상한. 두 request의 섹터 수를 더한 값이 MDTS 유래
	 * 상한을 넘으면 컨트롤러가 처리할 수 없다. 여기서는 REQ_NOMERGE를 세우지
	 * 않는데, ll_back_merge_fn과 달리 이 경로는 스케줄러가 주기적으로 재시도하는
	 * 성질의 것이 아니라 일회성 판정이기 때문이다. */
	if ((blk_rq_sectors(req) + blk_rq_sectors(next)) >
	    blk_rq_get_max_sectors(req, blk_rq_pos(req)))
		return 0;

	/* [한국어] 검사 3 — 세그먼트 용량. 두 request가 각자 세고 있던 물리 세그먼트
	 * 수를 단순히 더한다. 이음매에서 두 세그먼트가 물리적으로 이어져 하나로
	 * 합쳐질 가능성도 있지만, 여기서는 보수적으로 더하기만 한다(과대 추정은
	 * 안전하고, 정확한 재계산은 blk_recalc_rq_segments()가 필요할 때 수행한다).
	 * NVMe SGL 모드라면 이 합계가 256(NVME_MAX_SEGS)을 넘지 않아야 한다. */
	total_phys_segments = req->nr_phys_segments + next->nr_phys_segments;
	if (total_phys_segments > blk_rq_get_max_segments(req))
		return 0;

	/* [한국어] 검사 4 — cgroup 소속 일치. 다르면 I/O 계정이 엉켜 blk-throttle /
	 * blk-iocost의 격리가 깨진다. next->bio로 비교하는 것은 request의 cgroup
	 * 정보가 그 안의 bio에 붙어 있기 때문이다. */
	if (!blk_cgroup_mergeable(req, next->bio))
		return 0;

	/* [한국어] 검사 5 — 무결성(PI) 설정 일치. request 단위 버전으로, 두 request의
	 * PI 유형·메타데이터 크기·세그먼트 수 합계가 모두 호환되는지 확인한다.
	 * NVMe End-to-End Data Protection에서 reftag 연속성이 깨지면 컨트롤러가
	 * 검증 실패를 반환하므로 반드시 필요한 검사다. */
	if (blk_integrity_merge_rq(q, req, next) == false)
		return 0;

	/* [한국어] 검사 6 — 인라인 암호화 컨텍스트 병합 가능성. 같은 키를 쓰고 DUN이
	 * 연속이어야 한다. 이 함수는 판정과 동시에 req의 crypt 컨텍스트를 확장하는
	 * 부수 효과가 있어, 검사 순서상 마지막에 놓여 있다(앞선 검사에서 실패하면
	 * 아예 호출되지 않아 되돌릴 일이 없다). */
	if (!bio_crypt_ctx_merge_rq(req, next))
		return 0;

	/* Merge is OK... */
	/* [한국어] 모든 검사 통과 — 카운터를 커밋한다. 물리 세그먼트 수는 임시 변수의
	 * 합계로 교체하고, 무결성 세그먼트 수는 누적한다.
	 * nr_phys_segments 는 드라이버가 실제로 읽는 값이다 — NVMe PCIe 는
	 * blk_rq_nr_phys_segments(req) 로 DMA 기술 방식을 고른다:
	 *   == 1 이면 nvme_pci_setup_data_simple() 로 iterator 없이 곧장 처리하고(pci.c:1645),
	 *   그보다 많으면 PRP 리스트나 SGL 을 구성한다(pci.c:1533).
	 * 즉 여기서 세그먼트를 하나로 합쳐 두면 드라이버의 고속 경로가 열린다.
	 * 실제 bio 리스트 연결(req->biotail->bi_next = next->bio)과 next 해제는
	 * 호출자 attempt_merge()가 이어서 수행한다. */
	req->nr_phys_segments = total_phys_segments;
	req->nr_integrity_segments += next->nr_integrity_segments;
	return 1;
}

/*
 * blk_rq_set_mixed_merge - request를 mixed merge 상태로 표시한다.
 * @rq: 표시할 request
 *
 * 여러 bio의 FAILFAST 속성(REQ_FAILFAST_DEV/REQ_FAILFAST_TRANSPORT/
 * REQ_FAILFAST_DRIVER)을 하나의 request 로 합치는 경우, 재시도 정책
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
	blk_opf_t ff = rq->cmd_flags & REQ_FAILFAST_MASK;	/* [한국어] request 대표값으로 삼을 failfast 조합.
							 * 첫 bio 의 것이며, 이것을 나머지 전부에 퍼뜨린다 */
	struct bio *bio;					/* [한국어] 체인 순회 커서 */

	/* 이미 mixed merge로 표시되면 추가 분배 불필요. */
	if (rq->rq_flags & RQF_MIXED_MERGE)
		return;

	/*
	 * @rq will no longer represent mixable attributes for all the
	 * contained bios.  It will just track those of the first one.
	 * Distributes the attributs to each bio.
	 */
	/* [한국어] request 가 여러 bio 를 품게 되면 대표 플래그 하나로는 개별 bio 의
	 * 원래 속성을 표현할 수 없다. 그래서 대표값을 각 bio 에 복사해 두고,
	 * 완료 시점에 각 bio 가 자기 플래그를 보고 판단하게 만든다.
	 *
	 * failfast 의 뜻: "이 요청이 실패하면 재시도하지 말고 즉시 실패를 알려라".
	 * 주로 MD/DM 멀티패스가 쓴다 — 한 경로가 죽었을 때 재시도로 시간을 끌지 않고
	 * 곧바로 다른 경로로 넘기기 위해서다.
	 *
	 * 확인 결과: drivers/nvme/ 에는 REQ_FAILFAST_* 를 읽는 코드가 없다.
	 * (NVMe 의 NVME_CTRL_FAILFAST_EXPIRED 는 fabrics 의 fast_io_fail_tmo 상태로
	 *  이름만 비슷할 뿐 별개다.) 따라서 이 플래그는 NVMe 단독 구성에서는 사실상
	 * 쓰이지 않고, NVMe 위에 MD/DM 을 얹은 구성에서 의미를 갖는다. */
	for (bio = rq->bio; bio; bio = bio->bi_next) {
		WARN_ON_ONCE((bio->bi_opf & REQ_FAILFAST_MASK) &&
			     (bio->bi_opf & REQ_FAILFAST_MASK) != ff);	/* [한국어] 이미 다른 조합을 갖고 있었다면 병합 판정이 잘못된 것이다 —
									 * 서로 다른 failfast 요구를 한 request 로 묶으면 한쪽 요구가 조용히 사라진다 */
		bio->bi_opf |= ff;	/* [한국어] 덮어쓰기가 아니라 OR — 원래 갖고 있던 비트는 보존한다 */
	}
	rq->rq_flags |= RQF_MIXED_MERGE;
}

/*
 * [한국어]
 * bio_failfast - 이 bio가 가져야 할 failfast 플래그 조합을 결정
 *
 * @bio: 대상 bio
 * @return: REQ_FAILFAST_DEV | REQ_FAILFAST_TRANSPORT | REQ_FAILFAST_DRIVER 중
 *          이 bio에 적용될 비트들의 조합
 *
 * === failfast란 ===
 * "이 I/O가 실패하면 재시도하지 말고 즉시 에러를 돌려달라"는 요청이다. 세 종류의
 * 실패 원인별로 비트가 나뉜다:
 *   REQ_FAILFAST_DEV       - 장치 자체가 보고한 오류(NVMe 상태 코드 등)
 *   REQ_FAILFAST_TRANSPORT - 전송 계층 오류(PCIe 링크 다운, TCP 연결 끊김 등)
 *   REQ_FAILFAST_DRIVER    - 드라이버 내부 오류
 * NVMe에서는 drivers/nvme/host/core.c의 nvme_decide_disposition()이 이 플래그를
 * 확인해 재시도(enum nvme_disposition 의 RETRY) 대신 즉시 완료(COMPLETE)를 선택한다.
 *
 * === read-ahead를 무조건 failfast로 만드는 이유 ===
 * read-ahead(REQ_RAHEAD)는 "혹시 필요할지 모르니 미리 읽어 두는" 투기적 I/O다.
 * 아무도 그 데이터를 당장 기다리고 있지 않으므로, 실패했을 때 재시도에 시간을
 * 쓰는 것은 순수한 낭비다. 실제로 그 데이터가 필요해지면 그때 정식 읽기가
 * 다시 발생하고, 그 요청은 failfast 없이 정상적으로 재시도된다.
 * 특히 장치가 불안정한 상황에서 read-ahead 재시도가 큐를 막으면 정작 필요한
 * I/O의 지연이 커지므로, 조기에 포기하는 편이 전체 응답성에 유리하다.
 *
 * 실행 컨텍스트: 병합 판정 경로. 순수 계산 함수.
 *
 * 호출 체인:
 *   blk_attempt_bio_merge → [bio_failfast]
 */
static inline blk_opf_t bio_failfast(const struct bio *bio)
{
	/* [한국어] read-ahead면 세 가지 failfast 비트를 전부 켠 마스크를 돌려준다.
	 * 원래 bio에 어떤 비트가 있었는지와 무관하게 "모든 종류의 실패에 대해
	 * 재시도하지 않음"으로 강제하는 것이다. */
	if (bio->bi_opf & REQ_RAHEAD)
		return REQ_FAILFAST_MASK;

	/* [한국어] 일반 bio는 제출자가 명시적으로 설정한 failfast 비트만 그대로
	 * 추출해 돌려준다. 대부분의 파일시스템 I/O는 0(재시도 허용)이고, dm/md 같은
	 * 다중 경로 스택이 "이 경로가 실패하면 다른 경로로 넘어가겠다"는 의도로
	 * 비트를 세워 보낸다. */
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

/*
 * [한국어]
 * blk_account_io_merge_request - request가 다른 request에 흡수될 때의 통계 갱신
 *
 * @req: 병합되어 사라질 request
 * @return: 없음
 *
 * request 하나가 소멸하므로 두 가지를 기록한다:
 *   (1) merges 카운터 증가 — /proc/diskstats와 iostat의 rrqm/s, wrqm/s
 *       ("merged requests per second") 항목이 된다. 이 값이 높다면 순차 I/O가
 *       잘 합쳐지고 있다는 뜻으로, NVMe 성능 분석에서 커맨드 수 절감 효과를
 *       확인하는 지표다.
 *   (2) in_flight 카운터 감소 — "지금 장치에서 처리 중인 요청 수"에서 하나를 뺀다.
 *       이 request는 장치에 도달하지 않고 사라지므로 빼지 않으면 in_flight가
 *       영원히 줄지 않아 iostat의 큐 깊이와 utilization이 잘못 표시된다.
 *
 * RQF_IO_STAT가 없는 request(주로 passthrough)는 애초에 통계 대상이 아니므로
 * 전체를 건너뛴다.
 *
 * 실행 컨텍스트: 병합 경로. part_stat_lock()은 preempt_disable() 수준의 가벼운
 * per-CPU 보호로, 통계가 per-CPU 카운터에 누적되므로 실제 스핀락은 없다.
 *
 * 호출 체인:
 *   attempt_merge → [blk_account_io_merge_request]
 */
static void blk_account_io_merge_request(struct request *req)
{
	/* [한국어] RQF_IO_STAT는 "이 request를 디스크 통계에 반영하라"는 표시다.
	 * blk_mq_submit_bio()가 일반 I/O에만 설정하고, nvme-cli의 passthrough
	 * 커맨드처럼 통계에 넣으면 안 되는 요청에는 설정하지 않는다. */
	if (req->rq_flags & RQF_IO_STAT) {
		/* [한국어] per-CPU 통계 영역 접근을 보호한다. 내부적으로는
		 * preempt_disable()이라 선점만 막을 뿐 CPU 간 경합은 없다 —
		 * 각 CPU가 자기 카운터에만 쓰고 읽을 때 합산하는 구조다. */
		part_stat_lock();
		/* [한국어] 병합 카운터 증가. op_stat_group()이 읽기/쓰기/discard/flush를
		 * 구분해 인덱스를 만들므로, iostat에서 rrqm과 wrqm이 따로 표시된다.
		 * req->part는 이 I/O가 향한 파티션(또는 디스크 전체)의 통계 블록이다. */
		part_stat_inc(req->part, merges[op_stat_group(req_op(req))]);
		/* [한국어] in_flight 감소. 이 request는 곧 사라지므로 "처리 중" 집합에서
		 * 빼야 한다. op_is_write()로 읽기(0)/쓰기(1) 슬롯을 구분한다.
		 * _local_ 접두사는 per-CPU 로컬 카운터를 직접 조작한다는 뜻으로,
		 * 원자적 연산 없이 갱신해 핫패스 비용을 줄인다. */
		part_stat_local_dec(req->part,
				    in_flight[op_is_write(req_op(req))]);
		/* [한국어] per-CPU 보호 해제(preempt_enable). */
		part_stat_unlock();
	}
}

/*
 * [한국어]
 * blk_try_req_merge - 두 request의 위치 관계로 병합 유형을 분류
 *
 * @req:  앞쪽 request
 * @next: 뒤쪽 request
 * @return: ELEVATOR_DISCARD_MERGE = discard range 목록 병합,
 *          ELEVATOR_BACK_MERGE    = LBA가 인접해 뒤에 이어 붙일 수 있음,
 *          ELEVATOR_NO_MERGE      = 병합 불가
 *
 * "합쳐도 되는가"(정책·용량 검사)가 아니라 "어떤 방식으로 합칠 수 있는가"만
 * 판단하는 분류 함수다. 실제 가능 여부는 호출자 attempt_merge()가 이 결과에
 * 따라 ll_merge_requests_fn() 또는 req_attempt_discard_merge()를 불러 확인한다.
 *
 * front merge를 검사하지 않는 이유: 호출자 attempt_merge(q, req, next)는 이미
 * "req가 앞, next가 뒤"라는 순서를 정해 놓고 들어온다. 반대 방향의 병합은
 * 호출자가 인자 순서를 바꿔 다시 부르면 되므로(attempt_front_merge가 그렇게
 * 한다) 이 함수에서 양방향을 다룰 필요가 없다.
 *
 * 실행 컨텍스트: request 병합 경로. 순수 계산 함수.
 *
 * 호출 체인:
 *   attempt_merge → [blk_try_req_merge] → blk_discard_mergable
 */
static enum elv_merge blk_try_req_merge(struct request *req,
					struct request *next)
{
	/* [한국어] discard 계열은 LBA 인접성을 따지지 않는다. 앞서 설명했듯 전송할
	 * 데이터가 없고 "지울 범위 목록"만 보내므로, 떨어진 범위끼리도 하나의 NVMe
	 * DSM 커맨드에 담을 수 있기 때문이다. blk_discard_mergable()은 연산이
	 * discard이고 큐가 다중 range를 지원하는지(max_discard_segments > 1)를
	 * 확인한다. */
	if (blk_discard_mergable(req))
		return ELEVATOR_DISCARD_MERGE;
	/* [한국어] 일반 I/O는 LBA가 정확히 맞닿아야 한다.
	 * (req의 시작 + req의 길이) == next의 시작 이면 next가 req 바로 뒤에
	 * 이어지므로 back merge가 성립한다. 한 섹터라도 어긋나면 하나의 연속된
	 * NVMe Read/Write 커맨드(SLBA + NLB)로 표현할 수 없다. */
	else if (blk_rq_pos(req) + blk_rq_sectors(req) == blk_rq_pos(next))
		return ELEVATOR_BACK_MERGE;

	/* [한국어] 어느 쪽에도 해당하지 않으면 병합 불가. 두 request는 각각 별개의
	 * SQ 엔트리로 컨트롤러에 전달된다. */
	return ELEVATOR_NO_MERGE;
}

/*
 * [한국어]
 * blk_atomic_write_mergeable_rq_bio - request와 bio의 원자성 요구가 같은지 확인
 *
 * @rq:  기존 request
 * @bio: 붙이려는 bio
 * @return: true = 둘 다 원자적이거나 둘 다 아니어서 병합 가능, false = 서로 달라 불가
 *
 * 원자적 쓰기와 일반 쓰기를 한 request로 합치면 안 되는 이유는 양방향 모두 문제가
 * 되기 때문이다:
 *   - 원자적 요청에 일반 데이터가 섞이면, 커진 요청이 NAWUPF(원자 단위) 상한을
 *     넘어 원자성 보장이 깨진다.
 *   - 일반 요청에 원자적 데이터가 섞이면, 그 부분만 원자적으로 처리해 달라고
 *     장치에 요구할 방법이 없다. NVMe에서 원자성은 커맨드 단위 속성이지
 *     커맨드 내 일부 구간에 적용할 수 있는 것이 아니다.
 * 그래서 "같은가"만 확인하면 충분하다.
 *
 * 실행 컨텍스트: 병합 판정 경로. 순수 계산 함수.
 *
 * 호출 체인:
 *   blk_rq_merge_ok → [blk_atomic_write_mergeable_rq_bio]
 */
static bool blk_atomic_write_mergeable_rq_bio(struct request *rq,
					      struct bio *bio)
{
	/* [한국어] 두 REQ_ATOMIC 비트를 각각 추출해 값이 같은지 비교한다.
	 * 논리 부정(!!)을 쓰지 않아도 되는 것은 양쪽 모두 같은 마스크로 걸러
	 * 같은 비트 위치의 값이 나오기 때문이다(둘 다 REQ_ATOMIC이거나 둘 다 0). */
	return (rq->cmd_flags & REQ_ATOMIC) == (bio->bi_opf & REQ_ATOMIC);
}

/*
 * [한국어]
 * blk_atomic_write_mergeable_rqs - 두 request의 원자성 요구가 같은지 확인
 *
 * @rq:   앞쪽 request
 * @next: 뒤쪽 request
 * @return: true = 원자성 속성이 일치해 병합 가능, false = 불가
 *
 * blk_atomic_write_mergeable_rq_bio()의 request 대 request 버전이다. 판단 근거는
 * 동일하다 — NVMe에서 원자성은 커맨드 전체에 적용되는 속성이므로, 원자적 요청과
 * 일반 요청이 섞인 커맨드는 만들 수 없다.
 *
 * 실행 컨텍스트: request 병합 경로. 순수 계산 함수.
 *
 * 호출 체인:
 *   attempt_merge → [blk_atomic_write_mergeable_rqs]
 */
static bool blk_atomic_write_mergeable_rqs(struct request *rq,
					   struct request *next)
{
	/* [한국어] 두 request의 REQ_ATOMIC 비트를 비교한다. 다르면 병합을 포기하고
	 * 각각 별도의 NVMe 커맨드로 제출된다. */
	return (rq->cmd_flags & REQ_ATOMIC) == (next->cmd_flags & REQ_ATOMIC);
}

/*
 * [한국어]
 * bio_seg_gap - 두 bio를 병합했을 때의 "최악 이음매 정렬 비트"를 갱신해 반환
 *
 * @q:        대상 request_queue (biovec_phys_mergeable 판정에 limits가 필요)
 * @prev:     앞에 오는 bio
 * @next:     뒤에 오는 bio
 * @gaps_bit: 지금까지 누적된 정렬 비트(보통 req->phys_gap_bit). 0은 "아직 제약 없음".
 * @return:   병합 결과물이 보장하는 정렬 비트. 작을수록 정렬이 나쁘다.
 *
 * bvec_seg_gap()이 bio 내부의 이음매를 요약했다면, 이 함수는 bio와 bio 사이에
 * 새로 생기는 이음매까지 포함해 request 전체의 요약값을 갱신한다. 병합은 정렬을
 * 좋게 만들 수 없고 나쁘게만 만들 수 있으므로 연산은 항상 min_not_zero다 —
 * "가장 나쁜 이음매"가 request 전체의 성질을 결정하기 때문이다.
 *
 * min_not_zero를 쓰는 이유: 0은 "정보 없음/제약 없음"을 뜻하는 특별한 값이라
 * 일반 min()을 쓰면 0이 항상 이겨서 잘못된 결론(정렬이 최악이라는 판정)이 난다.
 * min_not_zero는 0이 아닌 값들 중에서만 최솟값을 고른다.
 *
 * 반환값은 req->phys_gap_bit에 저장되어 최종적으로 blk-mq-dma.c의
 * blk_can_dma_map_iova()가 IOMMU IOVA 병합 가능 여부를 판단하는 데 쓰인다.
 * 이 판단이 NVMe PRP 엔트리/SGL 디스크립터 개수를 좌우한다.
 *
 * 실행 컨텍스트: 병합 경로(프로세스 컨텍스트). 스케줄러 병합이라면 해당
 * 스케줄러 락, plug 병합이라면 락 없이 자기 plug 리스트 위에서 실행된다.
 *
 * 호출 체인:
 *   attempt_merge / bio_attempt_back_merge / bio_attempt_front_merge /
 *   blk_rq_append_bio → [bio_seg_gap]
 *     → bio_get_last_bvec / bio_get_first_bvec / biovec_phys_mergeable
 *     → bvec_seg_gap
 */
u8 bio_seg_gap(struct request_queue *q, struct bio *prev, struct bio *next,
	       u8 gaps_bit)
{
	/* [한국어] pb = prev의 마지막 bvec, nb = next의 첫 bvec. 두 bio가 맞닿는
	 * 이음매를 이루는 두 조각이다. */
	struct bio_vec pb, nb;

	/* [한국어] 데이터가 없는 bio(FLUSH 등)는 bvec도 DMA 매핑도 없으므로 정렬을
	 * 따질 대상이 아니다. 0("제약 없음")을 반환해 호출자가 그대로 두게 한다. */
	if (!bio_has_data(prev))
		return 0;

	/* [한국어] 각 bio가 이미 자기 내부 이음매들을 요약해 둔 bi_bvec_gap_bit를
	 * 누적값에 반영한다. 두 번 호출해 양쪽 bio의 내부 사정을 모두 흡수한다.
	 * min_not_zero이므로 셋 중 가장 나쁜(가장 작은 비트) 값이 남는다. */
	gaps_bit = min_not_zero(gaps_bit, prev->bi_bvec_gap_bit);
	gaps_bit = min_not_zero(gaps_bit, next->bi_bvec_gap_bit);

	/* [한국어] 이제 병합으로 새로 생기는 이음매 하나를 따진다. */
	bio_get_last_bvec(prev, &pb);
	bio_get_first_bvec(next, &nb);
	/* [한국어] 두 조각이 물리적으로 완전히 연속이면 이음매 자체가 사라지므로
	 * 정렬을 나쁘게 만들지 않는다 — 아무것도 하지 않는다.
	 * 연속이 아닐 때만 새 이음매의 정렬 비트를 계산해 반영한다.
	 * ffs()는 가장 낮은 1비트의 위치(1-based)를 돌려주므로, bvec_seg_gap()이
	 * 만든 OR 값에서 "몇 번째 비트까지 정렬이 보장되는가"를 뽑아내는 셈이다. */
	if (!biovec_phys_mergeable(q, &pb, &nb))
		gaps_bit = min_not_zero(gaps_bit, ffs(bvec_seg_gap(&pb, &nb)));
	/* [한국어] 갱신된 요약값을 반환한다. 호출자는 이를 req->phys_gap_bit에 저장하고,
	 * 나중에 blk_can_dma_map_iova()가 이 값으로 IOVA 병합 가부를 결정한다. */
	return gaps_bit;
}

/*
 * [한국어]
 * attempt_merge - 두 request를 실제로 하나로 합치는 함수(판정 + 상태 이전 + 통계)
 *
 * @q:    두 request가 속한 request_queue
 * @req:  병합 후 살아남는 request (앞쪽, LBA 작은 쪽)
 * @next: 병합 후 소멸하는 request (뒤쪽, LBA 큰 쪽)
 * @return: 성공 시 @next 포인터(호출자가 blk_mq_free_request로 해제해야 함),
 *          실패 시 NULL
 *
 * === 이 함수의 구조 ===
 * 1단계 — 빠른 거부: 두 request의 속성이 근본적으로 호환되지 않으면 즉시 포기.
 * 2단계 — 용량 판정: blk_try_req_merge()로 유형을 정하고 해당 검사 함수 호출.
 * 3단계 — 커밋: bio 리스트 연결, 길이/시작시각/gap 비트 갱신, 통계, 소유권 이전.
 * 2단계까지는 아무것도 바꾸지 않으므로 언제든 NULL로 빠져나갈 수 있다. 3단계에
 * 진입하면 되돌릴 수 없다.
 *
 * === 반환값이 next인 이유 ===
 * 이 함수는 next를 해제하지 않고 포인터만 돌려준다. 해제 시점은 호출자가 잡고
 * 있는 락의 종류에 따라 달라야 하기 때문이다 — 스케줄러 락을 쥔 채로
 * blk_mq_free_request()를 부르면 락 순서 역전이 발생할 수 있어, 호출자가 락을
 * 푼 뒤 해제하도록 책임을 넘긴다.
 *
 * === NVMe 관점에서의 이득 ===
 * request 하나가 사라진다는 것은 SQ 엔트리 하나, doorbell 쓰기 하나(플러그
 * 단위로 묶이므로 항상은 아니다), 완료 CQ 엔트리 하나, 그리고 태그 하나가
 * 절약된다는 뜻이다. 순차 워크로드에서 이 병합이 잘 동작하면 같은 대역폭을
 * 훨씬 적은 커맨드로 달성할 수 있다.
 *
 * 실행 컨텍스트: 위 영문 주석대로 적절한 락을 쥔 상태에서 호출되어야 한다.
 * blk-mq + 스케줄러라면 스케줄러의 큐 전역 락(mq-deadline의 dd->lock 등).
 *
 * 호출 체인:
 *   blk_mq_sched_try_merge / elv_attempt_insert_merge
 *     → attempt_back_merge / attempt_front_merge → [attempt_merge]
 *       → blk_try_req_merge → ll_merge_requests_fn / req_attempt_discard_merge
 *       → blk_rq_set_mixed_merge / bio_seg_gap / elv_merge_requests
 *       → blk_account_io_merge_request
 */
/*
 * For non-mq, this has to be called with the request spinlock acquired.
 * For mq with scheduling, the appropriate queue wide lock should be held.
 */
static struct request *attempt_merge(struct request_queue *q,
				     struct request *req, struct request *next)
{
	/* [한국어] 1단계 — 빠른 거부 검사들. 어느 하나라도 걸리면 아무것도 바꾸지 않고
	 * NULL로 빠진다. */

	/* [한국어] rq_mergeable()은 REQ_NOMERGE 플래그, passthrough 여부, FLUSH/FUA
	 * 같은 병합 불가 연산인지를 한 번에 확인한다. 앞선 병합 시도에서 한계에
	 * 걸려 REQ_NOMERGE가 새겨진 request가 여기서 걸러진다. */
	if (!rq_mergeable(req) || !rq_mergeable(next))
		return NULL;

	/* [한국어] 연산 종류가 다르면 애초에 하나의 커맨드가 될 수 없다. 읽기와 쓰기는
	 * NVMe opcode 자체가 다르고(0x02 Read / 0x01 Write), discard는 DSM(0x09)이다. */
	if (req_op(req) != req_op(next))
		return NULL;

	/* [한국어] write hint가 다르면 병합 불가. write hint는 "이 데이터가 얼마나 오래
	 * 살아 있을 것인가"를 장치에 알려주는 수명 힌트로, NVMe에서는 Directives의
	 * Streams나 최근의 FDP(Flexible Data Placement)로 전달되어 컨트롤러가 같은
	 * 수명의 데이터를 같은 물리 블록에 모으는 데 쓴다. 수명이 다른 데이터를 한
	 * 커맨드로 합치면 이 힌트의 의미가 사라져 GC(가비지 컬렉션) 효율과 WAF(쓰기
	 * 증폭)가 나빠진다. */
	if (req->bio->bi_write_hint != next->bio->bi_write_hint)
		return NULL;
	/* [한국어] write stream이 다르면 병합 불가. stream은 hint보다 더 명시적인
	 * 분리 요구로, NVMe Streams Directive의 Stream Identifier에 대응한다.
	 * 서로 다른 스트림은 물리적으로 분리해 저장되어야 하므로 하나의 커맨드에
	 * 담을 수 없다. */
	if (req->bio->bi_write_stream != next->bio->bi_write_stream)
		return NULL;
	/* [한국어] I/O 우선순위가 다르면 병합 불가. 합치면 둘 중 하나의 우선순위를
	 * 버려야 하는데, 낮은 쪽을 택하면 높은 우선순위 I/O가 손해를 보고 높은 쪽을
	 * 택하면 우선순위 제어가 무의미해진다. NVMe에서는 이 값이 커맨드의
	 * Command Priority(가중 라운드로빈 중재 사용 시)로 전달될 수 있다. */
	if (req->bio->bi_ioprio != next->bio->bi_ioprio)
		return NULL;
	/* [한국어] 원자성 요구가 다르면 병합 불가. 앞서 설명했듯 NVMe에서 원자성은
	 * 커맨드 단위 속성이라 부분 적용이 불가능하다. */
	if (!blk_atomic_write_mergeable_rqs(req, next))
		return NULL;

	/*
	 * If we are allowed to merge, then append bio list
	 * from next to rq and release next. merge_requests_fn
	 * will have updated segment counts, update sector
	 * counts here. Handle DISCARDs separately, as they
	 * have separate settings.
	 */

	/* [한국어] 2단계 — 유형 분류 후 해당 용량 검사를 수행한다. 이 switch를
	 * 통과해야만 아래 커밋 구간으로 진입한다. */
	switch (blk_try_req_merge(req, next)) {
	case ELEVATOR_DISCARD_MERGE:
		/* [한국어] discard 계열 — range 개수와 총 섹터 수 상한을 확인한다.
		 * 성공하면 req->nr_phys_segments가 합산된 range 개수로 갱신된다. */
		if (!req_attempt_discard_merge(q, req, next))
			return NULL;
		break;
	case ELEVATOR_BACK_MERGE:
		/* [한국어] 일반 I/O — virt boundary, 크기, 세그먼트, cgroup, PI, 암호화를
		 * 모두 확인한다. 성공하면 세그먼트 카운터들이 갱신된다. */
		if (!ll_merge_requests_fn(q, req, next))
			return NULL;
		break;
	default:
		/* [한국어] ELEVATOR_NO_MERGE — LBA가 인접하지 않아 합칠 방법이 없다. */
		return NULL;
	}

	/*
	 * If failfast settings disagree or any of the two is already
	 * a mixed merge, mark both as mixed before proceeding.  This
	 * makes sure that all involved bios have mixable attributes
	 * set properly.
	 */
	/* [한국어] 3단계 시작 — 여기서부터 되돌릴 수 없다.
	 *
	 * failfast 속성 정리. request는 failfast 플래그를 cmd_flags에 하나만 갖는데,
	 * 서로 다른 failfast 설정을 가진 bio들이 한 request에 모이면 그 값 하나로는
	 * 각 bio의 요구를 표현할 수 없다. 해법이 "mixed merge"다:
	 * blk_rq_set_mixed_merge()가 현재 request의 failfast 값을 소속 bio 각각의
	 * bi_opf에 복사해 두고 RQF_MIXED_MERGE를 세운다. 이후 부분 완료 시
	 * blk_update_request()가 bio별 플래그를 보고 개별적으로 재시도 여부를 판단한다.
	 *
	 * 조건이 두 갈래인 이유: 둘 중 하나라도 이미 mixed이면(첫째 항) 그 상태를
	 * 유지해야 하고, failfast 값이 서로 다르면(둘째 항) 새로 mixed로 전환해야
	 * 한다. 양쪽 모두에 set을 호출하는 것은 두 request의 bio 전부가 자기 플래그를
	 * 갖도록 보장하기 위해서다. */
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
	/* [한국어] 두 request 중 더 이른 시작 시각을 채택한다. start_time_ns는 I/O
	 * 지연 시간 통계(iostat의 await, blk-iolatency/blk-wbt의 지연 추적)의 기준점
	 * 이다. 나중에 시작한 쪽을 기준으로 삼으면 실제보다 짧은 지연이 기록되어,
	 * 오래 기다린 I/O가 통계에서 감춰진다. 영문 주석의 "we need the smaller
	 * start_time_ns ... for accounting purposes"가 이 뜻이다.
	 * (참고: NVMe 커맨드 타임아웃은 이 값이 아니라 blk-mq가 dispatch 시점에
	 *  기록하는 rq->deadline으로 판정되므로, 여기 갱신은 통계 전용이다.) */
	if (next->start_time_ns < req->start_time_ns)
		req->start_time_ns = next->start_time_ns;

	/* [한국어] 이음매가 새로 생기므로 정렬 요약 비트를 갱신한다. 두 request가
	 * 각자 갖고 있던 phys_gap_bit 중 나쁜 쪽을 기준으로 삼고(min_not_zero),
	 * 새 이음매까지 반영해 최종값을 만든다. 이 값이 나중에
	 * blk_can_dma_map_iova()에서 IOMMU IOVA 병합 가부를 결정한다.
	 * 반드시 bio 리스트를 연결하기 "전에" 계산해야 한다 — 연결 후에는
	 * req->biotail이 바뀌어 원래 이음매를 찾을 수 없기 때문이다. */
	req->phys_gap_bit = bio_seg_gap(req->q, req->biotail, next->bio,
					min_not_zero(next->phys_gap_bit,
						     req->phys_gap_bit));
	/* [한국어] 실제 bio 리스트 연결 — req의 마지막 bio 뒤에 next의 첫 bio를 잇는다.
	 * bio들은 bi_next로 엮인 단일 연결 리스트이고, request는 head(rq->bio)와
	 * tail(rq->biotail)을 모두 들고 있어 O(1) 추가가 가능하다. */
	req->biotail->bi_next = next->bio;
	/* [한국어] tail 포인터를 next의 tail로 옮겨 리스트의 끝을 갱신한다. */
	req->biotail = next->biotail;

	/* [한국어] 전체 데이터 길이를 합산한다. __data_len은 "아직 완료되지 않은
	 * 바이트 수"로, 완료 처리 때 blk_update_request()가 줄여 나간다.
	 * NVMe 커맨드의 NLB(Number of Logical Blocks) 필드는 이 값에서 유도된다. */
	req->__data_len += blk_rq_bytes(next);

	/* [한국어] I/O 스케줄러에게 병합 사실을 알려 내부 자료구조를 갱신하게 한다.
	 * mq-deadline이라면 정렬된 rb-tree와 FIFO 리스트에서 next를 제거하는 작업이다.
	 * discard 병합에서 건너뛰는 이유: discard는 LBA 인접성 없이 합쳐지므로
	 * 스케줄러의 위치 기반 자료구조 갱신이 의미가 없고, 오히려 잘못된 위치
	 * 정보를 남길 수 있다. */
	if (!blk_discard_mergable(req))
		elv_merge_requests(q, req, next);

	/* [한국어] next가 점유하고 있던 인라인 암호화 keyslot을 반납한다. keyslot은
	 * 하드웨어의 유한한 자원(보통 수십 개)이라 즉시 돌려주지 않으면 고갈되어
	 * 다른 I/O가 대기하게 된다. next의 데이터는 이미 req의 crypt 컨텍스트로
	 * 흡수되었으므로(bio_crypt_ctx_merge_rq) 안전하게 반납할 수 있다. */
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
	/* [한국어] next의 bio 포인터를 NULL로 끊는다. 이것이 소유권 이전의 핵심이다 —
	 * bio들은 이제 전부 req의 리스트에 속하므로, next를 해제할 때 그 bio들까지
	 * 함께 해제되면 안 된다. NULL로 만들어 두면 blk_mq_free_request()가 bio를
	 * 건드리지 않고 request 구조체(와 태그)만 반납한다.
	 * 이 한 줄을 빠뜨리면 살아 있는 bio가 해제되는 use-after-free가 된다. */
	next->bio = NULL;
	/* [한국어] 소멸시킬 request를 호출자에게 돌려준다. 호출자는 자신이 쥔 락을
	 * 적절히 정리한 뒤 blk_mq_free_request(next)로 태그를 반납한다. 태그가
	 * 반납되어야 그 자리에 새 NVMe 커맨드가 들어갈 수 있다. */
	return next;
}

/*
 * [한국어]
 * attempt_back_merge - 스케줄러 큐에서 @rq 바로 뒤 request와 back-merge 시도
 *
 * @q:  request_queue
 * @rq: 기준 request (앞쪽, LBA 작은 쪽)
 * @return: 병합 후 소멸된 next 포인터(호출자가 free); 실패 시 NULL
 *
 * elv_latter_request()로 스케줄러 내부에서 @rq 바로 뒤(LBA 큰 쪽) request를
 * 찾아 attempt_merge(rq, next)로 넘긴다. back-merge가 성공하면 next가 rq에
 * 흡수되어 NVMe SQ에 제출되는 request 수가 줄어든다.
 *
 * 호출 체인:
 *   blk_mq_sched_try_merge / elv_attempt_insert_merge
 *       → [attempt_back_merge] → elv_latter_request → attempt_merge
 */
static struct request *attempt_back_merge(struct request_queue *q,
		struct request *rq)
{
/* 스케줄러 dispatch 순서에서 rq 다음에 오는 request(LBA 큰 쪽) 탐색 */
	struct request *next = elv_latter_request(q, rq);

/* 다음 request가 있으면 rq <- next 방향으로 back-merge 시도 */
	if (next)
		return attempt_merge(q, rq, next);

	return NULL;
}

/*
 * [한국어]
 * attempt_front_merge - 스케줄러 큐에서 @rq 바로 앞 request와 front-merge 시도
 *
 * @q:  request_queue
 * @rq: 기준 request (뒤쪽, LBA 큰 쪽)
 * @return: 병합 후 소멸된 rq 포인터(호출자가 free); 실패 시 NULL
 *
 * elv_former_request()로 @rq 바로 앞(LBA 작은 쪽) request prev를 찾아
 * attempt_merge(prev, rq) 형태로 prev에 rq를 흡수시킨다.
 * front-merge는 back-merge보다 드물지만, out-of-order bio 제출 시 발생한다.
 *
 * 호출 체인:
 *   blk_mq_sched_try_merge → [attempt_front_merge] → elv_former_request → attempt_merge
 */
static struct request *attempt_front_merge(struct request_queue *q,
		struct request *rq)
{
/* 스케줄러 dispatch 순서에서 rq 이전에 오는 request(LBA 작은 쪽) 탐색 */
	struct request *prev = elv_former_request(q, rq);

/* 앞 request가 있으면 prev <- rq 방향으로 front-merge 시도 */
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
	/* [한국어] 검사 1 — 양쪽 모두 병합 가능한 상태인가.
	 * rq_mergeable(): REQ_NOMERGE가 없고, passthrough가 아니고, FLUSH/FUA처럼
	 *   순서 보장이 필요한 연산이 아닌지 확인.
	 * bio_mergeable(): bio 쪽 REQ_NOMERGE 확인.
	 * 가장 싸고 가장 자주 걸리는 검사라 맨 앞에 있다. */
	if (!rq_mergeable(rq) || !bio_mergeable(bio))
		return false;

	/* [한국어] 검사 2 — 연산 종류 일치. 읽기(NVMe opcode 0x02)와 쓰기(0x01)는
	 * 서로 다른 커맨드이므로 절대 합칠 수 없다. */
	if (req_op(rq) != bio_op(bio))
		return false;

	/* [한국어] 검사 3 — cgroup 소속 일치. 다르면 I/O 계정이 잘못된 cgroup에
	 * 청구되어 blk-throttle/blk-iocost의 자원 격리가 무너진다. */
	if (!blk_cgroup_mergeable(rq, bio))
		return false;
	/* [한국어] 검사 4 — 무결성(T10 PI / NVMe End-to-End Protection) 호환성.
	 * PI 유형이나 메타데이터 크기가 다르면 메타데이터 배열을 이어 붙일 수 없다. */
	if (blk_integrity_merge_bio(rq->q, rq, bio) == false)
		return false;
	/* [한국어] 검사 5 — 인라인 암호화 컨텍스트 호환성. 같은 키와 알고리즘을
	 * 써야 하나의 요청으로 암복호할 수 있다. (여기서는 "호환 가능한가"만 보고,
	 * DUN 연속성 같은 방향별 조건은 ll_back/front_merge_fn이 따로 확인한다.) */
	if (!bio_crypt_rq_ctx_compatible(rq, bio))
		return false;
	/* [한국어] 검사 6 — write hint(데이터 수명 힌트) 일치. NVMe FDP/Streams로
	 * 전달되어 컨트롤러가 같은 수명 데이터를 같은 물리 블록에 모으는 근거가
	 * 되므로, 다른 수명끼리 합치면 GC 효율과 쓰기 증폭이 나빠진다. */
	if (rq->bio->bi_write_hint != bio->bi_write_hint)
		return false;
	/* [한국어] 검사 7 — write stream 일치. NVMe Streams Directive의 Stream ID로,
	 * 물리적 분리 저장을 명시적으로 요구하므로 섞을 수 없다. */
	if (rq->bio->bi_write_stream != bio->bi_write_stream)
		return false;
	/* [한국어] 검사 8 — I/O 우선순위 일치. 합치면 한쪽의 우선순위를 버려야 한다. */
	if (rq->bio->bi_ioprio != bio->bi_ioprio)
		return false;
	/* [한국어] 검사 9 — 원자성 요구 일치. NVMe에서 원자성은 커맨드 단위 속성이라
	 * 한 커맨드 안에서 일부만 원자적으로 만들 수 없다. */
	if (blk_atomic_write_mergeable_rq_bio(rq, bio) == false)
		return false;

	/* [한국어] 아홉 가지를 모두 통과 — 정책상 병합 가능. 다만 이것은 "속성이
	 * 호환된다"까지만 보증하며, 실제 하드웨어 용량(세그먼트 수, 크기, virt
	 * boundary) 검사는 호출자가 이어서 부르는 ll_back/front_merge_fn의 몫이다. */
	return true;
}

/*
 * [한국어]
 * blk_try_merge - request와 bio의 LBA 위치 관계로 병합 방향을 판별
 *
 * @rq:  기존 request
 * @bio: 붙이려는 bio
 * @return: ELEVATOR_DISCARD_MERGE / ELEVATOR_BACK_MERGE /
 *          ELEVATOR_FRONT_MERGE / ELEVATOR_NO_MERGE
 *
 * blk_try_req_merge()가 request 대 request였다면 이쪽은 request 대 bio 버전이고,
 * 결정적인 차이는 front merge도 판별한다는 점이다. bio는 아직 request가 되기
 * 전이라 어느 방향으로든 붙일 수 있기 때문이다.
 *
 * NVMe 커맨드 관점에서 세 경우는 다음을 뜻한다:
 *   BACK  : 기존 커맨드의 NLB(Number of Logical Blocks)를 늘린다. SLBA는 그대로.
 *   FRONT : SLBA를 앞으로 당기고 NLB도 늘린다.
 *   DISCARD: DSM range 배열에 항목을 추가한다(위치 무관).
 *
 * 실행 컨텍스트: 병합 판정 경로. 순수 계산 함수.
 *
 * 호출 체인:
 *   blk_attempt_bio_merge / blk_mq_sched_try_merge → [blk_try_merge]
 */
enum elv_merge blk_try_merge(struct request *rq, struct bio *bio)
{
	/* [한국어] discard 계열은 위치를 따지지 않는다. 데이터 전송이 없으므로 떨어진
	 * 범위끼리도 하나의 DSM 커맨드의 range 배열에 담을 수 있다. */
	if (blk_discard_mergable(rq))
		return ELEVATOR_DISCARD_MERGE;
	/* [한국어] back merge — request의 끝(시작 + 길이)이 bio의 시작과 정확히
	 * 일치하는가. 순차 읽기/쓰기에서 압도적으로 흔한 경우라 먼저 검사한다. */
	else if (blk_rq_pos(rq) + blk_rq_sectors(rq) == bio->bi_iter.bi_sector)
		return ELEVATOR_BACK_MERGE;
	/* [한국어] front merge — bio의 끝이 request의 시작과 일치하는가.
	 * 식이 (rq 시작 - bio 길이 == bio 시작) 형태인 것에 주의: 양변에 bio 길이를
	 * 더하면 (rq 시작 == bio 시작 + bio 길이)가 되어 "bio 바로 뒤에 rq가 온다"는
	 * 뜻임이 분명해진다. 역순으로 도착한 I/O(역방향 순차 읽기, 일부 로그 구조
	 * 파일시스템의 쓰기 패턴)에서 발생한다. */
	else if (blk_rq_pos(rq) - bio_sectors(bio) == bio->bi_iter.bi_sector)
		return ELEVATOR_FRONT_MERGE;
	/* [한국어] 어느 쪽으로도 인접하지 않음 — 새 request를 만들어야 한다. */
	return ELEVATOR_NO_MERGE;
}

/*
 * [한국어]
 * blk_account_io_merge_bio - bio 병합 시 파티션 통계의 merge 카운터 증가
 *
 * @req: 병합이 일어난 request (통계 기록 대상)
 *
 * bio가 기존 request에 병합될 때 호출된다. RQF_IO_STAT 플래그가 설정된
 * request만 /proc/diskstats나 sysfs stats에 카운터가 반영된다.
 * part_stat_lock/unlock은 per-CPU stat을 집계할 때의 동시성 보호이다.
 *
 * 호출 체인:
 *   bio_attempt_back_merge / bio_attempt_front_merge → [blk_account_io_merge_bio]
 */
static void blk_account_io_merge_bio(struct request *req)
{
/* RQF_IO_STAT: 이 request가 diskstats에 집계되어야 하는지 여부 */
	if (req->rq_flags & RQF_IO_STAT) {
/* per-CPU stat 집계 보호 */
		part_stat_lock();
/* merges[읽기/쓰기/기타] 카운터 증가: iostat의 "merges" 열에 반영 */
		part_stat_inc(req->part, merges[op_stat_group(req_op(req))]);
		part_stat_unlock();
	}
}

/*
 * [한국어]
 * bio_attempt_back_merge - bio를 request 뒤에 실제로 이어 붙인다
 *
 * @req:      bio를 흡수할 request
 * @bio:      뒤에 붙일 bio (LBA가 req의 끝과 인접함이 이미 확인된 상태)
 * @nr_segs:  @bio가 추가할 물리 세그먼트 수
 * @return: BIO_MERGE_OK = 병합 완료, BIO_MERGE_FAILED = 하드웨어 한계로 실패
 *
 * 검사(ll_back_merge_fn)와 실행(리스트 연결·카운터 갱신)을 함께 수행하는 함수다.
 * 이 함수가 BIO_MERGE_OK를 반환하면 bio는 더 이상 독립적인 존재가 아니라
 * request의 일부가 되며, 나중에 request가 완료될 때 함께 완료 처리된다.
 *
 * NVMe 관점: 이 병합의 결과로 request 하나가 표현하는 LBA 범위가 늘어난다.
 * 최종적으로 nvme_setup_rw()가 이 request를 SLBA = blk_rq_pos(req),
 * NLB = blk_rq_sectors(req) - 1 인 커맨드 하나로 변환하므로, 병합될수록
 * 같은 데이터량을 더 적은 커맨드로 전송하게 된다.
 *
 * 실행 컨텍스트: bio 제출 경로. plug 병합이면 락 없이 자기 plug 리스트 위,
 * 스케줄러 병합이면 스케줄러 락 아래.
 *
 * 에러 경로: ll_back_merge_fn() 실패 시 BIO_MERGE_FAILED를 반환하며, 이때
 * request에는 이미 REQ_NOMERGE가 새겨져 있다. 호출자는 이 bio로 새 request를
 * 할당하는 경로로 진행한다.
 *
 * 호출 체인:
 *   blk_attempt_bio_merge → [bio_attempt_back_merge]
 *     → ll_back_merge_fn → ll_new_hw_segment
 *     → bio_seg_gap / blk_account_io_merge_bio
 */
enum bio_merge_status bio_attempt_back_merge(struct request *req,
		struct bio *bio, unsigned int nr_segs)
{
	/* [한국어] bio가 가져야 할 failfast 플래그를 미리 구해 둔다. read-ahead면
	 * 전부 켜진 마스크가, 아니면 원래 플래그가 온다. 아래에서 request의 현재
	 * failfast와 비교해 mixed merge 전환이 필요한지 판단한다. */
	const blk_opf_t ff = bio_failfast(bio);

	/* [한국어] 하드웨어 한계 검사 + 세그먼트 카운터 갱신. 실패하면 아무것도
	 * 바꾸지 않은 채 즉시 반환한다(ll_new_hw_segment 내부에서 REQ_NOMERGE만 설정). */
	if (!ll_back_merge_fn(req, bio, nr_segs))
		return BIO_MERGE_FAILED;

	/* [한국어] blktrace에 back-merge 사건을 기록. blkparse/btt로 병합률을 관찰할 수
	 * 있어 NVMe 커맨드 수 절감 효과를 정량화하는 데 쓰인다. */
	trace_block_bio_backmerge(bio);
	/* [한국어] rq-qos 계층(blk-wbt, blk-iolatency, blk-iocost)에 병합을 통지한다.
	 * 이들은 "제출된 I/O 수"를 추적해 지연을 제어하는데, 병합으로 bio 하나가
	 * request에 흡수되면 그 사실을 알아야 계정이 맞는다. */
	rq_qos_merge(req->q, req, bio);

	/* [한국어] request의 현재 failfast와 새 bio의 failfast가 다르면, 하나의
	 * cmd_flags로는 두 요구를 표현할 수 없다. blk_rq_set_mixed_merge()가 기존
	 * bio들에게 현재 값을 나눠 준 뒤 RQF_MIXED_MERGE를 세워, 이후 부분 완료 시
	 * bio별로 재시도 여부를 판단할 수 있게 한다. */
	if ((req->cmd_flags & REQ_FAILFAST_MASK) != ff)
		blk_rq_set_mixed_merge(req);

	/* [한국어] 이미 mixed 상태라면 새로 들어오는 bio에도 규칙을 적용한다
	 * (read-ahead면 failfast 강제). 세 번째 인자 false는 back merge라 request의
	 * cmd_flags를 갱신할 필요가 없다는 뜻이다 — front merge와 달리 request의
	 * "첫 bio"가 바뀌지 않기 때문이다. */
	blk_update_mixed_merge(req, bio, false);

	/* [한국어] ZNS 순차 쓰기 존에 대한 write plug가 걸린 request라면, 병합된 bio도
	 * plug의 관리 대상에 포함시켜야 한다. zone write plug는 한 존에 대한 쓰기를
	 * 직렬화해 zone write pointer 순서를 지키는 장치인데, 병합으로 들어온 bio가
	 * 등록되지 않으면 완료 처리 시 plug의 카운트가 어긋난다. */
	if (req->rq_flags & RQF_ZONE_WRITE_PLUGGING)
		blk_zone_write_plug_bio_merged(bio);

	/* [한국어] 새 이음매(req의 마지막 bio ↔ 새 bio)를 반영해 정렬 요약 비트를
	 * 갱신한다. 반드시 리스트 연결 전에 계산해야 req->biotail이 아직 "이전
	 * 마지막 bio"를 가리킨다. 이 값이 IOMMU IOVA 병합 가부를 결정해 NVMe
	 * 디스크립터 개수에 영향을 준다. */
	req->phys_gap_bit = bio_seg_gap(req->q, req->biotail, bio,
					req->phys_gap_bit);
	/* [한국어] bio 리스트의 끝에 새 bio를 연결한다. */
	req->biotail->bi_next = bio;
	/* [한국어] tail 포인터를 새 bio로 옮긴다. head(req->bio)는 그대로이므로
	 * request의 시작 LBA(__sector)도 바뀌지 않는다 — back merge의 특징이다. */
	req->biotail = bio;
	/* [한국어] request의 전체 데이터 길이에 새 bio의 크기를 더한다. 이 값이
	 * NVMe 커맨드의 NLB 필드로 변환된다. */
	req->__data_len += bio->bi_iter.bi_size;

	/* [한국어] bio가 들고 있던 암호화 컨텍스트를 해제한다. request가 이미 호환
	 * 가능한 컨텍스트를 갖고 있음이 blk_rq_merge_ok()에서 확인되었으므로,
	 * bio 쪽 사본은 중복이라 즉시 반납해 메모리를 아낀다. */
	bio_crypt_free_ctx(bio);

	/* [한국어] /proc/diskstats의 merges 카운터를 증가시킨다(iostat의 rrqm/wrqm). */
	blk_account_io_merge_bio(req);
	/* [한국어] 병합 성공. 호출자는 이 bio에 대해 더 이상 아무것도 하지 않는다 —
	 * 이제 request의 일부이므로 request 완료 시 함께 완료된다. */
	return BIO_MERGE_OK;
}

/*
 * [한국어]
 * bio_attempt_front_merge - bio를 request 앞에 실제로 이어 붙인다
 *
 * @req:     bio를 흡수할 request
 * @bio:     앞에 붙일 bio (LBA가 req의 시작 바로 앞임이 확인된 상태)
 * @nr_segs: @bio가 추가할 물리 세그먼트 수
 * @return: BIO_MERGE_OK = 병합 완료, BIO_MERGE_FAILED = 실패
 *
 * back merge와 대칭이지만 세 가지가 다르다:
 *   1) request의 시작 위치(__sector)가 앞으로 당겨진다 → NVMe SLBA가 바뀐다.
 *   2) bio 리스트의 head가 교체된다(tail이 아니라).
 *   3) ZNS 순차 쓰기 존에서는 아예 금지된다(아래 상세).
 *
 * front merge 자체가 드문 이유: 대부분의 워크로드는 LBA가 증가하는 방향으로
 * I/O를 발행하므로 새 bio는 기존 request의 뒤에 붙는다. front merge는 역방향
 * 순차 접근이나, 여러 스레드가 인접 영역을 순서 없이 쓰는 경우에 나타난다.
 *
 * 실행 컨텍스트: bio 제출 경로(plug 또는 스케줄러 병합).
 *
 * 호출 체인:
 *   blk_attempt_bio_merge → [bio_attempt_front_merge]
 *     → ll_front_merge_fn → ll_new_hw_segment
 *     → bio_seg_gap / bio_crypt_do_front_merge
 */
static enum bio_merge_status bio_attempt_front_merge(struct request *req,
		struct bio *bio, unsigned int nr_segs)
{
	/* [한국어] bio의 failfast 플래그를 미리 구해 둔다(back merge와 동일). */
	const blk_opf_t ff = bio_failfast(bio);

	/*
	 * A front merge for writes to sequential zones of a zoned block device
	 * can happen only if the user submitted writes out of order. Do not
	 * merge such write to let it fail.
	 */
	/* [한국어] ZNS 순차 쓰기 존에 대한 front merge는 무조건 거부한다.
	 * 논리가 흥미로운데, 영문 주석의 요지는 이렇다: 순차 존에서는 반드시 zone
	 * write pointer 위치에 써야 하므로, "기존 request보다 앞의 LBA에 쓰는 bio"가
	 * 존재한다는 것 자체가 이미 사용자가 순서를 어겼다는 증거다. 그런 쓰기는
	 * 병합해서 조용히 성공시키면 안 되고, 그대로 장치에 내려보내 Zone Invalid
	 * Write 오류로 실패하게 두어야 사용자가 버그를 발견할 수 있다.
	 * RQF_ZONE_WRITE_PLUGGING은 이 request가 zone write plug의 관리를 받고 있음을
	 * 뜻하며, 곧 순차 존 쓰기라는 의미다. */
	if (req->rq_flags & RQF_ZONE_WRITE_PLUGGING)
		return BIO_MERGE_FAILED;

	/* [한국어] 하드웨어 한계 검사 + 세그먼트 카운터 갱신(front 방향). */
	if (!ll_front_merge_fn(req, bio, nr_segs))
		return BIO_MERGE_FAILED;

	/* [한국어] blktrace에 front-merge 사건 기록. */
	trace_block_bio_frontmerge(bio);
	/* [한국어] rq-qos 계층에 병합 통지(계정 일관성 유지). */
	rq_qos_merge(req->q, req, bio);

	/* [한국어] failfast가 다르면 mixed merge로 전환(back merge와 동일한 이유). */
	if ((req->cmd_flags & REQ_FAILFAST_MASK) != ff)
		blk_rq_set_mixed_merge(req);

	/* [한국어] 세 번째 인자가 true — front merge이므로 request의 cmd_flags에 실린
	 * failfast를 새 bio의 값으로 갱신해야 한다. request의 failfast는 관례상
	 * "첫 bio"의 것을 대표값으로 삼는데, front merge로 첫 bio가 교체되기 때문이다. */
	blk_update_mixed_merge(req, bio, true);

	/* [한국어] 새 이음매(새 bio ↔ req의 기존 첫 bio)의 정렬 비트를 반영한다.
	 * 인자 순서가 (bio, req->bio)로 back merge와 반대인 점에 주의 — 물리적으로
	 * 앞에 오는 쪽을 prev로 넘겨야 이음매 방향이 맞는다. */
	req->phys_gap_bit = bio_seg_gap(req->q, bio, req->bio,
					req->phys_gap_bit);
	/* [한국어] 새 bio를 리스트의 맨 앞에 끼워 넣는다: 새 bio → 기존 첫 bio → ... */
	bio->bi_next = req->bio;
	/* [한국어] head 포인터를 새 bio로 교체한다. tail(biotail)은 그대로다. */
	req->bio = bio;

	/* [한국어] request의 시작 섹터를 새 bio의 시작으로 당긴다. 이것이 front merge의
	 * 핵심 부수 효과다 — NVMe 커맨드의 SLBA(Starting LBA)가 바뀐다는 뜻이다.
	 * back merge에는 이 줄이 없다. */
	req->__sector = bio->bi_iter.bi_sector;
	/* [한국어] 전체 길이 누적(NLB 증가). */
	req->__data_len += bio->bi_iter.bi_size;

	/* [한국어] 암호화 컨텍스트 처리도 back merge와 다르다. back merge는 bio의
	 * 컨텍스트를 그냥 버렸지만(bio_crypt_free_ctx), front merge에서는 request의
	 * DUN(Data Unit Number, 암호화 IV의 기준값) 시작점이 새 bio의 것으로 바뀌어야
	 * 하므로 전용 함수로 앞당겨 갱신한다. 이를 빠뜨리면 복호화 시 잘못된 IV를
	 * 사용해 데이터가 깨진다. */
	bio_crypt_do_front_merge(req, bio);

	/* [한국어] merges 통계 카운터 증가. */
	blk_account_io_merge_bio(req);
	return BIO_MERGE_OK;
}

/*
 * [한국어]
 * bio_attempt_discard_merge - discard bio를 request의 range 목록에 추가
 *
 * @q:   대상 request_queue
 * @req: discard request
 * @bio: 추가할 discard bio
 * @return: BIO_MERGE_OK = 병합 완료, BIO_MERGE_FAILED = 용량 초과
 *
 * req_attempt_discard_merge()의 "request + bio" 버전이다. 로직은 같지만 추가되는
 * range가 항상 1개라는 점만 다르다(bio 하나 = range 하나).
 *
 * 일반 R/W 병합과 달리 LBA 인접성을 요구하지 않는다는 점이 핵심이다. 서로 멀리
 * 떨어진 파일들을 삭제해도 그 discard들이 하나의 NVMe DSM 커맨드에 담긴다.
 * ll_new_hw_segment()를 거치지 않는 이유도 같다 — 데이터 버퍼가 없어 물리
 * 세그먼트나 virt boundary 개념이 적용되지 않기 때문이다.
 *
 * 실행 컨텍스트: bio 제출 경로(plug 또는 스케줄러 병합).
 *
 * 호출 체인:
 *   blk_attempt_bio_merge → [bio_attempt_discard_merge]
 *     → blk_rq_nr_discard_segments / blk_rq_get_max_sectors
 */
static enum bio_merge_status bio_attempt_discard_merge(struct request_queue *q,
		struct request *req, struct bio *bio)
{
	/* [한국어] request가 현재 담고 있는 discard range 개수. */
	unsigned short segments = blk_rq_nr_discard_segments(req);

	/* [한국어] range 배열이 꽉 찼는지 확인. 상한은 NVMe DMRL 또는
	 * NVME_DSM_MAX_RANGES(256)에서 온다. 드라이버가 고정 크기 버퍼를 할당하므로
	 * 이를 넘기면 버퍼 오버런이 된다. */
	if (segments >= queue_max_discard_segments(q))
		goto no_merge;
	/* [한국어] 총 섹터 수 상한(NVMe DMRSL 유래) 확인. range 개수와 별개로
	 * 한 커맨드가 다룰 수 있는 전체 LBA 양에도 한계가 있다. */
	if (blk_rq_sectors(req) + bio_sectors(bio) >
	    blk_rq_get_max_sectors(req, blk_rq_pos(req)))
		goto no_merge;

	/* [한국어] rq-qos 계층에 병합 통지. */
	rq_qos_merge(q, req, bio);

	/* [한국어] bio를 리스트 끝에 연결한다. discard는 항상 뒤에 추가하는데,
	 * 순서가 의미를 갖지 않기 때문이다(range 배열의 항목 순서는 무관). */
	req->biotail->bi_next = bio;
	req->biotail = bio;
	/* [한국어] 전체 지울 바이트 수 누적. 통계와 상한 검사에 쓰인다. */
	req->__data_len += bio->bi_iter.bi_size;
	/* [한국어] range 개수를 하나 늘린다. nr_phys_segments 필드를 range 카운터로
	 * 재사용하고 있다는 점을 다시 확인할 수 있다. 이 개수만큼
	 * nvme_setup_discard()가 nvme_dsm_range 배열 항목을 채운다. */
	req->nr_phys_segments = segments + 1;

	/* [한국어] merges 통계 카운터 증가. */
	blk_account_io_merge_bio(req);
	return BIO_MERGE_OK;
no_merge:
	/* [한국어] 용량 초과 — 이후 병합 시도를 막고 실패를 알린다. 이 bio는 새
	 * discard request가 되어 별도의 DSM 커맨드로 전달된다. */
	req_set_nomerge(q, req);
	return BIO_MERGE_FAILED;
}

/*
 * [한국어]
 * blk_attempt_bio_merge - bio 하나와 request 하나의 병합 시도를 총괄하는 디스패처
 *
 * @q:                 대상 request_queue
 * @rq:                병합 상대 request
 * @bio:               병합하려는 bio
 * @nr_segs:           @bio의 물리 세그먼트 수
 * @sched_allow_merge: true면 I/O 스케줄러에게도 허락을 구한다.
 *                     plug 병합(false)은 스케줄러를 거치지 않으므로 묻지 않고,
 *                     스케줄러 경로(true)는 스케줄러의 정책까지 존중해야 한다.
 * @return: BIO_MERGE_OK     = 병합 성공
 *          BIO_MERGE_NONE   = 병합할 수 없는 조합(다른 request를 계속 시도해도 됨)
 *          BIO_MERGE_FAILED = 인접하지만 하드웨어 한계로 실패(더 시도해도 무의미)
 *
 * === 반환값이 세 갈래인 이유 ===
 * NONE과 FAILED의 구분이 중요하다. 호출자 blk_bio_list_merge()는 여러 request를
 * 순회하며 시도하는데, NONE이면 "이 request와는 안 맞으니 다음 것을 보자"이고
 * FAILED면 "인접한 request를 찾긴 했는데 한계에 걸렸으니 더 봐야 소용없다"이다.
 * 이 구분 덕분에 불필요한 순회를 조기에 끊을 수 있다.
 *
 * 두 단계로 나뉜 검사 구조가 이 파일 전체의 설계를 요약한다:
 *   1단계 blk_rq_merge_ok()  — 속성 호환성(연산 종류, cgroup, PI, 암호화, 우선순위)
 *   2단계 blk_try_merge()    — 위치 관계(back/front/discard)
 *   3단계 bio_attempt_*()    — 하드웨어 용량(세그먼트, 크기, virt boundary) + 실행
 *
 * 실행 컨텍스트: bio 제출 경로. plug 병합이면 락 없음, 스케줄러 병합이면
 * 스케줄러 락 아래.
 *
 * 호출 체인:
 *   blk_attempt_plug_merge / blk_bio_list_merge / blk_mq_sched_try_merge
 *     → [blk_attempt_bio_merge]
 *       → blk_rq_merge_ok → blk_try_merge → blk_mq_sched_allow_merge
 *       → bio_attempt_back_merge / bio_attempt_front_merge / bio_attempt_discard_merge
 */
static enum bio_merge_status blk_attempt_bio_merge(struct request_queue *q,
						   struct request *rq,
						   struct bio *bio,
						   unsigned int nr_segs,
						   bool sched_allow_merge)
{
	if (!blk_rq_merge_ok(rq, bio))
		return BIO_MERGE_NONE;

	/* [한국어] 위치 관계를 판별해 해당 방향의 실행 함수로 분기한다. */
	switch (blk_try_merge(rq, bio)) {
	case ELEVATOR_BACK_MERGE:
		/* [한국어] 뒤에 붙일 수 있다. sched_allow_merge가 false면(plug 경로)
		 * 스케줄러에게 묻지 않고 바로 진행하고, true면 스케줄러의 허락을 받는다.
		 * 단락 평가 덕분에 false일 때는 blk_mq_sched_allow_merge()가 아예
		 * 호출되지 않는다. 스케줄러가 거부하는 예: BFQ가 서로 다른 큐(프로세스)의
		 * I/O를 섞으면 공정성 보장이 깨진다고 판단하는 경우. */
		if (!sched_allow_merge || blk_mq_sched_allow_merge(q, rq, bio))
			return bio_attempt_back_merge(rq, bio, nr_segs);
		/* [한국어] 스케줄러가 거부 — break로 빠져나가 아래 FAILED를 반환한다.
		 * 인접한 request를 이미 찾았으므로 다른 request를 더 봐도 소용없다. */
		break;
	case ELEVATOR_FRONT_MERGE:
		/* [한국어] 앞에 붙일 수 있다. 스케줄러 허락 로직은 back과 동일하다. */
		if (!sched_allow_merge || blk_mq_sched_allow_merge(q, rq, bio))
			return bio_attempt_front_merge(rq, bio, nr_segs);
		break;
	case ELEVATOR_DISCARD_MERGE:
		/* [한국어] discard는 스케줄러에게 묻지 않는다. 위치 기반 정책(공정성,
		 * 순서 보장)이 적용되지 않는 연산이고, range 목록에 항목을 추가하는 것은
		 * 스케줄러의 순서 결정과 무관하기 때문이다. */
		return bio_attempt_discard_merge(q, rq, bio);
	default:
		/* [한국어] ELEVATOR_NO_MERGE — LBA가 인접하지 않는다. 다른 request와는
		 * 맞을 수도 있으므로 NONE(계속 시도 가능)을 반환한다. */
		return BIO_MERGE_NONE;
	}

	/* [한국어] break로 여기 도달 = 위치는 맞았지만 스케줄러가 거부했다.
	 * FAILED를 반환해 호출자가 순회를 중단하게 한다. */
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
	/* [한국어] 현재 태스크의 plug를 가져온다. blk_start_plug()가 설정하고
	 * blk_finish_plug()가 해제하는, task_struct에 매달린 per-task 요청 스택이다.
	 * 락 없이 접근 가능한 이유는 오직 자기 자신만 이 리스트를 만지기 때문이다. */
	struct blk_plug *plug = current->plug;
	struct request *rq;

	/* [한국어] plug를 쓰지 않는 경로이거나(현재 태스크가 blk_start_plug를 부르지
	 * 않음) 아직 모인 request가 없으면 병합할 상대가 없다. */
	if (!plug || rq_list_empty(&plug->mq_list))
		return false;

	/* [한국어] 가장 최근에 추가된 request(tail)부터 확인한다. plug 리스트는 한
	 * 스레드가 연달아 발행한 I/O를 모으므로, 직전 I/O가 지금 bio와 LBA상 인접할
	 * 확률이 압도적으로 높다(순차 읽기/쓰기). 그래서 tail 하나만 보고 성공하면
	 * 끝내는 것이 대부분의 경우 최적이다.
	 * sched_allow_merge에 false를 넘기는 이유는 위 영문 주석이 설명한다 — plug
	 * 단계의 request는 아직 elevator에 삽입되지 않았고, 큐 락 밖에서는 elevator에
	 * 안전하게 접근할 수 없으므로 스케줄러에게 물을 수 없다. */
	rq = plug->mq_list.tail;
	if (rq->q == q)
		return blk_attempt_bio_merge(q, rq, bio, nr_segs, false) ==
			BIO_MERGE_OK;
	/* [한국어] tail이 다른 큐의 request이고, 이 plug에 여러 큐가 섞여 있지도
	 * 않다면(multiple_queues == false) 리스트 전체가 다른 큐 것이므로 즉시 포기.
	 * 이 플래그가 순회 비용을 아끼는 빠른 판별자 역할을 한다. */
	else if (!plug->multiple_queues)
		return false;

	/* [한국어] 여러 큐가 섞인 plug(예: dm/md 위에서 여러 NVMe 장치에 동시 I/O)
	 * 라면 리스트를 순회하며 같은 큐의 request를 찾는다. */
	rq_list_for_each(&plug->mq_list, rq) {
		/* [한국어] 다른 큐의 request는 건너뛴다 — 큐가 다르면 애초에 병합 불가. */
		if (rq->q != q)
			continue;
		/* [한국어] 같은 큐를 찾았으니 병합을 시도한다. */
		if (blk_attempt_bio_merge(q, rq, bio, nr_segs, false) ==
		    BIO_MERGE_OK)
			return true;
		/* [한국어] 같은 큐의 첫 request에서 실패하면 더 뒤로 가지 않고 즉시
		 * 중단한다. 리스트는 시간 역순이라 뒤로 갈수록 오래된 I/O이고, 최근
		 * I/O와 인접하지 않았다면 더 오래된 것과 인접할 가능성은 낮다.
		 * 병합 탐색 비용을 상수로 묶어 두는 의도적인 제한이다. */
		break;
	}
	/* [한국어] 병합 실패 — 호출자 blk_mq_submit_bio()는 이 bio로 새 request를
	 * 할당해 plug 리스트에 추가한다. */
	return false;
}

/*
 * [한국어]
 * blk_bio_list_merge - 스케줄러가 들고 있는 request 리스트를 훑으며 병합 상대를 찾는다
 *
 * @q:       대상 request_queue
 * @list:    후보 request들의 리스트(스케줄러가 관리)
 * @bio:     병합하려는 bio
 * @nr_segs: @bio의 물리 세그먼트 수
 * @return: true = 병합 성공, false = 실패(호출자가 새 request를 만들어야 함)
 *
 * 정렬된 자료구조(rb-tree)를 갖지 않는 단순한 스케줄러가 쓰는 병합 헬퍼다.
 * mq-deadline처럼 위치 기반 인덱스를 유지하는 스케줄러는 elv_merge()로 O(log n)
 * 탐색을 하지만, kyber나 BFQ의 일부 경로처럼 그런 인덱스가 없는 곳에서는 이렇게
 * 선형 탐색을 한다. EXPORT_SYMBOL_GPL로 공개되어 스케줄러 모듈이 호출한다.
 *
 * 실행 컨텍스트: 스케줄러의 bio_merge 콜백 안. 해당 스케줄러의 락을 쥔 상태다.
 *
 * 호출 체인:
 *   blk_mq_submit_bio → blk_mq_attempt_bio_merge → (스케줄러의 bio_merge 콜백,
 *   예: kyber_bio_merge / bfq_bio_merge) → [blk_bio_list_merge]
 *     → blk_attempt_bio_merge
 */
/*
 * Iterate list of requests and see if we can merge this bio with any
 * of them.
 */
bool blk_bio_list_merge(struct request_queue *q, struct list_head *list,
			struct bio *bio, unsigned int nr_segs)
{
	struct request *rq;
	/* [한국어] 탐색 횟수 상한. 리스트가 아무리 길어도 8개까지만 본다.
	 * 이 매직 넘버는 "병합 이득"과 "탐색 비용"의 경험적 타협점이다. 순차
	 * 워크로드라면 인접한 request는 리스트 뒤쪽(최근)에 있을 가능성이 높아 8개면
	 * 충분하고, 랜덤 워크로드라면 아무리 뒤져도 못 찾으므로 오래 찾을수록 손해다.
	 * 고성능 NVMe에서는 제출 경로의 CPU 사이클이 곧 IOPS 한계이므로 이런 상한이
	 * 특히 중요하다. */
	int checked = 8;

	/* [한국어] 리스트를 역순(최근 추가된 것부터)으로 순회한다. plug 병합과 같은
	 * 논리로, 최근 request일수록 지금 bio와 시간적·공간적 지역성이 높다. */
	list_for_each_entry_reverse(rq, list, queuelist) {
		/* [한국어] 후위 감소이므로 checked가 0이 되는 순간(8번 검사 후) 중단한다. */
		if (!checked--)
			break;

		/* [한국어] sched_allow_merge에 true를 넘긴다 — 이 경로는 스케줄러 락
		 * 안에서 실행되므로 스케줄러에게 안전하게 허락을 구할 수 있다.
		 * plug 경로(false)와 대비되는 지점이다. */
		switch (blk_attempt_bio_merge(q, rq, bio, nr_segs, true)) {
		case BIO_MERGE_NONE:
			/* [한국어] 이 request와는 인접하지 않는다 — 다음 후보로 계속. */
			continue;
		case BIO_MERGE_OK:
			/* [한국어] 병합 성공 — 즉시 종료. */
			return true;
		case BIO_MERGE_FAILED:
			/* [한국어] 인접했지만 하드웨어 한계나 스케줄러 거부로 실패했다.
			 * 위치상 맞는 상대를 이미 찾았으므로 다른 request를 더 봐도
			 * 소용없다. 순회를 끝내고 실패를 반환한다. */
			return false;
		}

	}

	/* [한국어] 8개를 다 봤거나 리스트가 짧아 상대를 못 찾았다. 호출자는 이 bio로
	 * 새 request를 할당해 스케줄러에 삽입한다. */
	return false;
}
EXPORT_SYMBOL_GPL(blk_bio_list_merge);

/*
 * [한국어]
 * blk_mq_sched_try_merge - 스케줄러 인덱스로 병합 상대를 찾고, 연쇄 병합까지 수행
 *
 * @q:              대상 request_queue
 * @bio:            병합하려는 bio
 * @nr_segs:        @bio의 물리 세그먼트 수
 * @merged_request: [out] 연쇄 병합으로 소멸한 request. NULL이 아니면 호출자가
 *                  blk_mq_free_request()로 해제해야 한다.
 * @return: true = 병합 성공, false = 실패
 *
 * === 이 함수만의 특징: 2단계 연쇄 병합 ===
 * 다른 병합 경로는 "bio를 request에 붙이는" 1단계로 끝난다. 그런데 bio를 붙여
 * request가 커지면, 그 커진 request가 이제는 이웃 request와도 인접해질 수 있다.
 * 예를 들어 [0~100] request와 [200~300] request 사이에 [100~200] bio가 들어오면,
 * bio를 앞 request에 붙여 [0~200]이 된 순간 뒤 request와 맞닿는다.
 * 그래서 이 함수는 bio_attempt_*_merge() 직후 attempt_*_merge()를 한 번 더 불러
 * request 대 request 병합까지 시도한다. 세 조각이 하나의 NVMe 커맨드가 되는
 * 셈으로, 병합의 효과가 가장 극적으로 나타나는 경로다.
 *
 * === elv_merged_request()를 부르는 조건 ===
 * 연쇄 병합이 일어나지 않았을 때(*merged_request == NULL)만 호출한다.
 * 이 콜백은 "request의 크기/위치가 바뀌었으니 인덱스를 갱신하라"는 통지인데,
 * 연쇄 병합이 성공했다면 attempt_merge() 내부의 elv_merge_requests()가 이미
 * 더 완전한 갱신(한쪽 제거 + 다른 쪽 갱신)을 수행했으므로 중복 호출이 된다.
 *
 * 실행 컨텍스트: 스케줄러 락을 쥔 상태. 반환 후 호출자가 락을 풀고
 * *merged_request를 해제한다.
 *
 * 호출 체인:
 *   blk_mq_submit_bio → blk_mq_attempt_bio_merge → (스케줄러 bio_merge 콜백,
 *   예: dd_bio_merge) → [blk_mq_sched_try_merge]
 *     → elv_merge → bio_attempt_back/front_merge → attempt_back/front_merge
 *   이후: 커진 request → blk_mq_dispatch → nvme_queue_rq → SQ doorbell
 */
bool blk_mq_sched_try_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs, struct request **merged_request)
{
	/* [한국어] elv_merge()가 찾아낸 병합 후보 request를 받을 변수. */
	struct request *rq;

	/* [한국어] 스케줄러에게 병합 후보를 물어본다. elv_merge()는 세 가지를 순서대로
	 * 시도한다: (1) 마지막 병합 위치 캐시(one-hit cache), (2) 스케줄러의
	 * elevator_merge 콜백(mq-deadline이면 정렬된 rb-tree에서 O(log n) 탐색),
	 * (3) 실패 시 NO_MERGE. 선형 탐색인 blk_bio_list_merge()보다 훨씬 효율적이라
	 * 인덱스를 갖춘 스케줄러는 이 경로를 쓴다. */
	switch (elv_merge(q, &rq, bio)) {
	case ELEVATOR_BACK_MERGE:
		/* [한국어] 스케줄러 정책 확인(BFQ의 큐 격리 등). 거부하면 즉시 포기. */
		if (!blk_mq_sched_allow_merge(q, rq, bio))
			return false;
		/* [한국어] 1단계 — bio를 request 뒤에 실제로 붙인다. 하드웨어 한계에
		 * 걸리면 실패로 끝난다. */
		if (bio_attempt_back_merge(rq, bio, nr_segs) != BIO_MERGE_OK)
			return false;
		/* [한국어] 2단계 — 커진 request가 이제 뒤 이웃과 맞닿는지 확인해 연쇄
		 * 병합을 시도한다. 성공하면 흡수된 request 포인터가 나오고, 호출자가
		 * 락을 푼 뒤 해제한다. */
		*merged_request = attempt_back_merge(q, rq);
		/* [한국어] 연쇄 병합이 없었을 때만 인덱스 갱신을 통지한다. 있었다면
		 * attempt_merge() 내부에서 이미 더 완전한 갱신이 끝났다. */
		if (!*merged_request)
			elv_merged_request(q, rq, ELEVATOR_BACK_MERGE);
		return true;
	case ELEVATOR_FRONT_MERGE:
		/* [한국어] front 방향도 구조가 동일하다. 스케줄러 허락 → bio 붙이기 →
		 * 앞 이웃과의 연쇄 병합 시도 → 인덱스 갱신. */
		if (!blk_mq_sched_allow_merge(q, rq, bio))
			return false;
		if (bio_attempt_front_merge(rq, bio, nr_segs) != BIO_MERGE_OK)
			return false;
		/* [한국어] front merge로 request의 시작 LBA가 앞당겨졌으므로, 이제 앞
		 * 이웃과 맞닿을 수 있다. attempt_front_merge()가 그 확인을 담당한다. */
		*merged_request = attempt_front_merge(q, rq);
		if (!*merged_request)
			elv_merged_request(q, rq, ELEVATOR_FRONT_MERGE);
		return true;
	case ELEVATOR_DISCARD_MERGE:
		/* [한국어] discard는 연쇄 병합을 하지 않는다. range 목록에 항목을 추가할
		 * 뿐 request의 LBA 범위가 "확장"되는 것이 아니라, 이웃과 새로 맞닿는
		 * 일이 생기지 않기 때문이다. 그래서 1단계로 끝난다. */
		return bio_attempt_discard_merge(q, rq, bio) == BIO_MERGE_OK;
	default:
		/* [한국어] 스케줄러가 후보를 찾지 못했다. 호출자 blk_mq_submit_bio()가
		 * 새 request를 할당해 태그를 받고 스케줄러에 삽입한다. */
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
 *    뒤이은 DMA 매핑이 큐 한계를 만족하도록 보장한다.
 *  - Discard/Secure Erase/Write Zeroes는 각각 NVMe Dataset Management,
 *    Sanitize, Write Zeroes 명령의 제약을 반영하여 분할/병합한다.
 *  - 이 파일의 처리 이후 request는 block/blk-mq.c의 blk_mq_get_request를
 *    거쳐 nvme_queue_rq로 전달되고, 최종적으로 nvme_sq_copy_cmd/nvme_write_sq_db()에서
 *    doorbell을 울려 NVMe 컨트롤러에 전송된다.
 * ============================================================================
 */
