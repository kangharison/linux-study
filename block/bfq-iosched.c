// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Budget Fair Queueing (BFQ) I/O scheduler.
 *
 * Based on ideas and code from CFQ:
 * Copyright (C) 2003 Jens Axboe <axboe@kernel.dk>
 *
 * Copyright (C) 2008 Fabio Checconi <fabio@gandalf.sssup.it>
 *		      Paolo Valente <paolo.valente@unimore.it>
 *
 * Copyright (C) 2010 Paolo Valente <paolo.valente@unimore.it>
 *                    Arianna Avanzini <avanzini@google.com>
 *
 * Copyright (C) 2017 Paolo Valente <paolo.valente@linaro.org>
 *
 * BFQ is a proportional-share I/O scheduler, with some extra
 * low-latency capabilities. BFQ also supports full hierarchical
 * scheduling through cgroups. Next paragraphs provide an introduction
 * on BFQ inner workings. Details on BFQ benefits, usage and
 * limitations can be found in Documentation/block/bfq-iosched.rst.
 *
 * BFQ is a proportional-share storage-I/O scheduling algorithm based
 * on the slice-by-slice service scheme of CFQ. But BFQ assigns
 * budgets, measured in number of sectors, to processes instead of
 * time slices. The device is not granted to the in-service process
 * for a given time slice, but until it has exhausted its assigned
 * budget. This change from the time to the service domain enables BFQ
 * to distribute the device throughput among processes as desired,
 * without any distortion due to throughput fluctuations, or to device
 * internal queueing. BFQ uses an ad hoc internal scheduler, called
 * B-WF2Q+, to schedule processes according to their budgets. More
 * precisely, BFQ schedules queues associated with processes. Each
 * process/queue is assigned a user-configurable weight, and B-WF2Q+
 * guarantees that each queue receives a fraction of the throughput
 * proportional to its weight. Thanks to the accurate policy of
 * B-WF2Q+, BFQ can afford to assign high budgets to I/O-bound
 * processes issuing sequential requests (to boost the throughput),
 * and yet guarantee a low latency to interactive and soft real-time
 * applications.
 *
 * In particular, to provide these low-latency guarantees, BFQ
 * explicitly privileges the I/O of two classes of time-sensitive
 * applications: interactive and soft real-time. In more detail, BFQ
 * behaves this way if the low_latency parameter is set (default
 * configuration). This feature enables BFQ to provide applications in
 * these classes with a very low latency.
 *
 * To implement this feature, BFQ constantly tries to detect whether
 * the I/O requests in a bfq_queue come from an interactive or a soft
 * real-time application. For brevity, in these cases, the queue is
 * said to be interactive or soft real-time. In both cases, BFQ
 * privileges the service of the queue, over that of non-interactive
 * and non-soft-real-time queues. This privileging is performed,
 * mainly, by raising the weight of the queue. So, for brevity, we
 * call just weight-raising periods the time periods during which a
 * queue is privileged, because deemed interactive or soft real-time.
 *
 * The detection of soft real-time queues/applications is described in
 * detail in the comments on the function
 * bfq_bfqq_softrt_next_start. On the other hand, the detection of an
 * interactive queue works as follows: a queue is deemed interactive
 * if it is constantly non empty only for a limited time interval,
 * after which it does become empty. The queue may be deemed
 * interactive again (for a limited time), if it restarts being
 * constantly non empty, provided that this happens only after the
 * queue has remained empty for a given minimum idle time.
 *
 * By default, BFQ computes automatically the above maximum time
 * interval, i.e., the time interval after which a constantly
 * non-empty queue stops being deemed interactive. Since a queue is
 * weight-raised while it is deemed interactive, this maximum time
 * interval happens to coincide with the (maximum) duration of the
 * weight-raising for interactive queues.
 *
 * Finally, BFQ also features additional heuristics for
 * preserving both a low latency and a high throughput on NCQ-capable,
 * rotational or flash-based devices, and to get the job done quickly
 * for applications consisting in many I/O-bound processes.
 *
 * NOTE: if the main or only goal, with a given device, is to achieve
 * the maximum-possible throughput at all times, then do switch off
 * all low-latency heuristics for that device, by setting low_latency
 * to 0.
 *
 * BFQ is described in [1], where also a reference to the initial,
 * more theoretical paper on BFQ can be found. The interested reader
 * can find in the latter paper full details on the main algorithm, as
 * well as formulas of the guarantees and formal proofs of all the
 * properties.  With respect to the version of BFQ presented in these
 * papers, this implementation adds a few more heuristics, such as the
 * ones that guarantee a low latency to interactive and soft real-time
 * applications, and a hierarchical extension based on H-WF2Q+.
 *
 * B-WF2Q+ is based on WF2Q+, which is described in [2], together with
 * H-WF2Q+, while the augmented tree used here to implement B-WF2Q+
 * with O(log N) complexity derives from the one introduced with EEVDF
 * in [3].
 *
 * [1] P. Valente, A. Avanzini, "Evolution of the BFQ Storage I/O
 *     Scheduler", Proceedings of the First Workshop on Mobile System
 *     Technologies (MST-2015), May 2015.
 *     http://algogroup.unimore.it/people/paolo/disk_sched/mst-2015.pdf
 *
 * [2] Jon C.R. Bennett and H. Zhang, "Hierarchical Packet Fair Queueing
 *     Algorithms", IEEE/ACM Transactions on Networking, 5(5):675-689,
 *     Oct 1997.
 *
 * http://www.cs.cmu.edu/~hzhang/papers/TON-97-Oct.ps.gz
 *
 * [3] I. Stoica and H. Abdel-Wahab, "Earliest Eligible Virtual Deadline
 *     First: A Flexible and Accurate Mechanism for Proportional Share
 *     Resource Allocation", technical report.
 *
 * http://www.cs.berkeley.edu/~istoica/papers/eevdf-tr-95.pdf
 */
/*
 * [한국어 설명] BFQ(Budget Fair Queuing) I/O 스케줄러 메인 구현 (bfq-iosched.c)
 *
 * === 파일의 역할 ===
 * bfq-iosched.c는 리눅스 커널의 BFQ I/O 스케줄러를 구현한다. BFQ는 CFQ에서
 * 파생된 비례-공유(proportional-share) I/O 스케줄러로, 시간 슬라이스 대신
 * 섹터 단위 예산(budget)을 각 프로세스에 할당한다. 예산이 소진될 때까지 해당
 * 프로세스가 장치를 독점 사용하고, 소진되면 B-WF2Q+ 알고리즘에 따라 다음
 * 프로세스로 전환한다. interactive/soft real-time 애플리케이션에 저지연을
 * 보장하면서도 I/O 집약적 프로세스에 대한 높은 처리량을 동시에 제공하는 것이
 * 핵심 설계 목표이다. 또한 cgroup 계층을 통한 완전한 계층적 I/O 스케줄링을
 * 지원한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 블록 계층의 elevator(I/O 스케줄러) 레이어에 위치하며, blk-mq 위에서 동작한다.
 * 상위 호출 체인: 파일시스템 → VFS → bio 생성 → blk_mq_submit_bio()
 *   → blk_mq_get_request() → bfq_limit_depth() → bfq_insert_request()
 *   → bfq_add_request() (내부 request 큐에 삽입)
 * 하위 호출 체인: blk_mq_run_hw_queues() → bfq_dispatch_request()
 *   → blk-mq hctx → 장치 드라이버의 queue_rq() (NVMe라면 nvme_queue_rq())
 * 실행 컨텍스트: blk-mq softirq, process context(insert 경로),
 *   hrtimer 콜백(idle slice timer), kblockd workqueue(dispatch).
 * B-WF2Q+ (Budgeted WF2Q+) 알고리즘과 H-WF2Q+ cgroup 계층 확장으로
 * 각 프로세스/cgroup 가중치에 비례하는 처리량을 보장한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: block/elevator.c (스케줄러 등록/인터페이스),
 *   block/blk-mq.c (request 할당·dispatch), block/blk-mq-sched.c
 *   (스케줄러 공통 훅 인터페이스), block/bfq-iosched.h (핵심 구조체·상수 정의),
 *   block/bfq-wf2q.c (B-WF2Q+ 서비스 트리 연산), block/bfq-cgroup.c
 *   (cgroup 계층 관리), block/blk-wbt.c (writeback throttle 연동).
 * 데이터 흐름: bio → struct request (blk-mq 할당) → bfq_queue.sort_list
 *   (LBA 정렬 rb-tree) → dispatch 시 blk-mq hw_queue → 드라이버 SQ.
 * 공유 핵심 자료구조: struct bfq_data (장치당 스케줄러 상태),
 *   struct bfq_queue (프로세스당 I/O 큐), struct bfq_entity (B-WF2Q+ 스케줄 단위),
 *   struct bfq_io_cq (프로세스 I/O 컨텍스트, per-io_context 추적).
 * 장치 특성과의 연결: BFQ는 드라이버를 알지 못하고, 장치 특성을 오직
 *   blk_queue_rot()(회전 디스크 여부)과 스스로 관찰해 추정한
 *   hw_tag(장치가 실제로 여러 요청을 동시에 처리하는가)로만 받아들인다.
 *   이 둘이 idling·budget·inject_limit 휴리스틱의 방향을 바꾼다. NVMe처럼
 *   내부 병렬성이 큰 장치에서는 idling이 처리량 손실이 되고 bfqd->lock이
 *   장치 단위 전역 락이라 확장성 병목이 되므로, 그런 장치의 기본
 *   스케줄러로는 none이나 mq-deadline이 쓰이는 것이 일반적이다.
 *
 * === 주요 함수/구조체 요약 ===
 * bfq_dispatch_request()  — 메인 dispatch 엔트리: in-service queue 선택 후
 *                           request 1개를 blk-mq 드라이버 큐로 전달.
 * bfq_add_request()       — blk-mq가 request 삽입 시 호출; sort_list/fifo에 추가.
 * bfq_remove_request()    — request 취소/병합 시 bfqq에서 제거.
 * bfq_bfqq_expire()       — in-service queue를 만료(budget 소진·타임아웃·요청 없음).
 * bfq_update_peak_rate()  — 장치 최대 처리율(섹터/μs) 추정; budget 자동 계산에 사용.
 * bfq_better_to_idle()    — idling 필요 여부 판단 (throughput vs. 공정성 트레이드오프).
 * bfq_set_next_ioprio_data() — cgroup/ionice 기반 우선순위 상속 처리.
 * struct bfq_data         — 장치당 스케줄러 전역 상태 (in_service_queue, busy_queues,
 *                           peak_rate, idle_slice_timer, queue_weights_tree 등).
 * struct bfq_queue        — 프로세스당 I/O 큐 (sort_list, fifo, entity, wr_coeff 등).
 * struct bfq_io_cq        — 프로세스 I/O 컨텍스트 (bfqq[sync][actuator], ttime 등).
 *
 * NVMe SSD 관점 추가 요약:
 * 이 파일은 blk-mq 스케줄러 인터페이스를 구현하며, 파일 시스템이 생성한
 * bio → request 를 BFQ 내부 queue(bfqq)에 배분하고, dispatch 시점에
 * blk_mq_hw_ctx 를 통해 NVMe 드라이버로 request 를 전달한다.
 * NVMe 다중 actuator: bfqq[sync/async][actuator_idx] 구조로 actuator별
 * request를 분리하여 독립 접근 영역(blk_independent_access_ranges)을 활용한다.
 * inject_limit: service hole(서비스 공백)을 메울 때 다른 bfqq request를 끼워
 * 넣는 최대 수; NVMe NCQ queue depth(최대 32 또는 드라이버 설정)를 고려한다.
 */
#include <linux/module.h>
/* [한국어] MODULE_ALIAS/module_init/module_exit 등 모듈 등록 매크로 -
 * BFQ는 "bfq" 커널 모듈로 빌드/로드될 수 있으므로 모듈 라이프사이클 API가 필요하다. */
#include <linux/slab.h>
/* [한국어] kmem_cache_create/kmem_cache_alloc/kmem_cache_free 선언 -
 * 아래 bfq_pool(struct bfq_queue 전용 슬랩 캐시) 생성·할당·해제에 사용된다.
 * bfq_queue는 프로세스/그룹마다 빈번히 할당·해제되므로 범용 kmalloc보다
 * 전용 슬랩 캐시를 쓰는 편이 캐시 지역성과 할당 속도 면에서 유리하다. */
#include <linux/blkdev.h>
/* [한국어] struct request, struct request_queue, blk_rq_xxx/blk_queue_xxx 헬퍼 선언 -
 * BFQ가 스케줄링하는 최소 단위(struct request)와 상위 request_queue 정의가
 * 여기서 온다. 아래 BFQ_RQ_SEEKY에서 쓰는 blk_queue_rot() 같은 회전형 여부
 * 판별 헬퍼도 이 헤더가 제공한다. */
#include <linux/cgroup.h>
/* [한국어] cgroup_subsys/struct cgroup_subsys_state(css) 등 cgroup 코어 API -
 * BFQ의 io cgroup 계층(CONFIG_BFQ_GROUP_IOSCHED, block/bfq-cgroup.c)이 각
 * cgroup을 struct bfq_group으로 매핑해 H-WF2Q+ 계층 스케줄링을 구성하는 데
 * 필요하다. */
#include <linux/ktime.h>
/* [한국어] ktime_get_ns()/ktime_to_ns() 등 단조 증가(monotonic) 시간 API -
 * think-time(ttime) 측정, idle_slice_timer 만료 시각 계산, peak_rate 추정을
 * 위한 관측 구간 측정 등 BFQ 전역의 시간 기반 휴리스틱이 이 API에 의존한다. */
#include <linux/rbtree.h>
/* [한국어] struct rb_root/rb_node와 rb_insert_color/rb_erase 등 rb-tree 연산 -
 * bfqq->sort_list(LBA 정렬 pending request), bfqd의 rq_pos_tree(인접 LBA
 * 큐 탐색), queue_weights_tree(가중치별 큐 카운트) 등 BFQ 전역 자료구조가
 * 모두 rb-tree 기반이므로 필수이다. */
#include <linux/ioprio.h>
/* [한국어] IOPRIO_CLASS_*, IOPRIO_PRIO_CLASS/DATA 등 I/O 우선순위 매크로 -
 * ionice(1)/ioprio_set() 시스템 콜로 설정된 프로세스별 우선순위를 BFQ의
 * weight·entity ioprio_class로 매핑하는 bfq_set_next_ioprio_data()에서 쓰인다. */
#include <linux/sbitmap.h>
/* [한국어] struct sbitmap/sbitmap_queue 등 확장성 있는 비트맵 API -
 * blk-mq의 태그(sched_tags) 할당과 연동되는 depth 제한(bfq_limit_depth)에서
 * 남은 태그 수 계산이나 태그 대기(wait queue) 처리에 사용된다. */
#include <linux/delay.h>
/* [한국어] msleep/udelay 등 짧은 지연 함수 -
 * 스케줄러 핫패스보다는 드문 초기화·디버그성 대기 경로에서 필요할 때 사용된다. */
#include <linux/backing-dev.h>
/* [한국어] struct backing_dev_info, bdi_* 접근자 -
 * writeback throttle(wbt, 아래 blk-wbt.h)이 참조하는 장치의 backing-dev
 * 통계(더티 페이지 수, 혼잡도 등)에 접근하기 위해 필요하다. */

#include <trace/events/block.h>
/* [한국어] trace_block_rq_insert/trace_block_rq_issue 등 블록 계층 tracepoint 정의 -
 * ftrace/blktrace로 BFQ가 request를 삽입·dispatch·완료하는 시점을 관측할 수
 * 있게 해주는 계측(instrumentation) 지점을 제공한다. */

#include "elevator.h"
/* [한국어] struct elevator_queue/elevator_type, elv_register/elv_unregister 등 -
 * BFQ를 blk-mq I/O 스케줄러(elevator)로 등록하기 위한 공통 프레임워크 인터페이스.
 * 파일 하단의 bfq 모듈 초기화 코드가 이 인터페이스로 "bfq" 스케줄러를 등록한다. */
#include "blk.h"
/* [한국어] 블록 계층 내부(비공개) 공용 선언 모음 -
 * request_queue의 내부 필드나 서브시스템 내부 헬퍼처럼 드라이버에는 공개되지
 * 않는 블록 레이어 전용 API에 접근하기 위해 포함한다. */
#include "blk-mq.h"
/* [한국어] blk_mq_alloc_request/blk_mq_run_hw_queue 등 blk-mq 코어 API -
 * BFQ가 최종적으로 선택한 request를 blk-mq 하드웨어 큐(hctx)로 넘겨 드라이버
 * (예: nvme_queue_rq)에 전달하는 dispatch 경로에 필요하다. */
#include "blk-mq-sched.h"
/* [한국어] struct elevator_mq_ops와 blk_mq_sched_* 헬퍼 선언 -
 * BFQ가 구현해야 하는 표준 스케줄러 콜백(insert_requests, dispatch_request,
 * has_work, completed_request 등)의 시그니처와 등록 방식을 정의한다. */
#include "bfq-iosched.h"
/* [한국어] BFQ 자체의 핵심 구조체(bfq_data/bfq_queue/bfq_entity/bfq_io_cq/
 * bfq_group)와 상수·인라인 헬퍼 선언 - 이 .c 파일이 구현하는 대상 헤더이며,
 * bfq-wf2q.c, bfq-cgroup.c 등 다른 BFQ 소스 파일과도 공유된다. */
#include "blk-wbt.h"
/* [한국어] writeback throttle(wbt) 연동 API -
 * 버퍼드 쓰기(write-back)가 폭주해 읽기(read) 지연을 유발하지 않도록, BFQ가
 * wbt와 협력해 비동기 쓰기 처리량을 조절하는 데 사용된다. */

/*
 * 주요 구조체와 스케줄링 동작의 연결
 *
 * (BFQ 는 장치 종류에 중립적인 스케줄러다. 아래 필드 중 어느 것도 특정
 *  드라이버의 큐/커맨드 구조를 알지 못하며, 장치 특성은 오직 두 경로로만
 *  들어온다: blk_queue_rot()이 알려주는 회전 여부와, BFQ 가 in-flight
 *  요청 수를 관찰해 스스로 추정하는 hw_tag(장치가 실제로 여러 요청을
 *  동시에 처리하는가)다.)
 *
 * struct bfq_queue (프로세스 단위, actuator 단위로 분리된 잎 노드)
 *   - sort_list: LBA 순으로 정렬된 pending request 의 rb-tree. 위치가
 *     가까운 요청을 연달아 내보내기 위한 정렬이며, 실제로 어떤 순서로
 *     완료될지는 장치가 정한다.
 *   - next_rq: 이 큐에서 다음에 내보낼 후보 request. bfq_choose_req()가
 *     마지막 서비스 위치(last_position)를 기준으로 고른다.
 *   - dispatched/queued: 각각 "이미 드라이버로 넘어가 완료를 기다리는 수"와
 *     "아직 BFQ 안에 대기 중인 수". 전자는 idling/injection 판단에,
 *     후자는 큐가 비었는지(만료 사유 판정)에 쓰인다.
 *   - actuator_idx: 이 bfqq 가 담당하는 독립 접근 영역의 인덱스.
 *     gendisk->ia_ranges 가 없으면 항상 0 이다(대다수 장치가 이 경우).
 *   - inject_limit: in-service 큐가 idling 중일 때 다른 큐의 요청을 몇 개나
 *     끼워 넣어도 되는지의 상한. 값은 고정 상수가 아니라, 주입 전후의
 *     서비스 시간(last_serv_time_ns)을 실측해 늘리거나 줄인다.
 *   - wr_coeff/wr_cur_max_time: weight-raising 배수와 그 유지 기간.
 *     대화형/soft real-time 으로 보이는 큐를 한시적으로 우대해, 처리량을
 *     조금 내주는 대신 응답성을 산다.
 *   - entity: B-WF2Q+ 스케줄링 단위. F_i = S_i + budget/weight 로 가상
 *     종료 시각을 계산해, weight 에 비례하는 대역폭 몫을 보장한다.
 *
 * struct bfq_data (request_queue 하나당 하나)
 *   - queue: 이 스케줄러가 붙어 있는 blk-mq request_queue.
 *   - dispatch: 스케줄링을 건너뛰고 곧바로 내보낼 request 리스트(예:
 *     requeue 된 요청). bfq_dispatch_request()가 서비스 트리보다 먼저 본다.
 *   - in_service_queue: 지금 장치를 점유해 서비스받고 있는 bfqq.
 *   - tot_rq_in_driver/rq_in_driver[]: 스케줄러를 떠나 드라이버에서
 *     처리 중인 request 수(전체 / actuator 별). injection 한도와 hw_tag
 *     판정, peak_rate 표본의 유효성 판단에 쓰인다.
 *   - hw_tag/nonrot_with_queueing: hw_tag 는 "장치가 실제로 여러 요청을
 *     동시에 처리하는가"를 관찰로 추정한 값(-1=미확정)이고,
 *     nonrot_with_queueing 은 여기에 "회전 디스크가 아님"을 더한 것이다.
 *     참이면 한 큐를 기다리는 idling 이 대부분 손해이므로 idling 과 큐
 *     병합 휴리스틱이 크게 완화된다.
 *   - num_actuators/sector[]/nr_sectors[]: blk_independent_access_ranges
 *     에서 복사한 독립 접근 영역 정보. 영역별로 부하를 따로 세어, 한
 *     영역의 혼잡이 다른 영역의 판단을 오염시키지 않게 한다.
 *   - peak_rate/delta_from_first: 순차 구간을 표본으로 삼아 저역통과
 *     필터로 추정한 장치 처리율(고정소수점 섹터/us). max_budget 자동
 *     조정과 weight-raising 지속 기간 계산의 입력이다.
 *   - idle_slice_timer: 다음 요청을 기다리며 장치를 비워 두는 idling 의
 *     만기 타이머. 만료 전에 요청이 오면 도박에 성공한 것이고, 만료되면
 *     그 시간만큼 장치를 놀린 셈이 된다.
 *
 * struct bfq_io_cq (io_context 단위, 즉 태스크 단위)
 *   - bfqq[2][BFQ_MAX_ACTUATORS]: sync/async x actuator 별 bfqq 행렬.
 *     같은 프로세스라도 접근 영역이 다르면 별도의 큐로 나눠 추적한다.
 *   - requests: 이 태스크가 현재 점유 중인 request 수. bfq_limit_depth()가
 *     가중치 몫을 넘겨 태그를 독점하는 태스크를 억제할 때 참조한다.
 *
 * struct bfq_group (cgroup 단위)
 *   - rq_pos_tree: 인접 LBA 를 다루는 bfqq 를 빠르게 찾아 병합 후보로
 *     삼는다. 서로 다른 프로세스가 하나의 순차 스트림을 번갈아 내는
 *     경우를 발견하기 위한 것이며, nonrot_with_queueing 장치에서는 병합
 *     이득이 작아 대부분 수행되지 않는다.
 *   - async_bfqq[][][]: 그룹 내 비동기(주로 writeback) 요청이 공유하는 큐.
 *     async I/O 는 발생시킨 프로세스를 특정하기 어렵고 지연시간을 지켜 줄
 *     이유도 적으므로, 프로세스별로 나누지 않고 그룹 단위로 모은다.
 */
/*
 * [한국어]
 * BFQ_BFQQ_FNS(name) - bfq_queue 상태 플래그 1개당 mark/clear/test 3종
 *                       접근자 함수를 한 번에 찍어내는 코드-생성 매크로.
 *
 * @name: block/bfq-iosched.h 의 enum bfqq_state_flags 에 정의된 BFQQF_##name
 *        플래그의 접미사(예: busy, sync, coop 등). 토큰 붙이기(##)로
 *        BFQQF_##name, bfq_mark_bfqq_##name 등의 실제 식별자를 만든다.
 *
 * 이 매크로를 한 번 호출(BFQ_BFQQ_FNS(busy); 등)하면 아래 3개 함수가
 * 동시에 정의된다:
 *   - void bfq_mark_bfqq_##name(bfqq)  : flags 의 BFQQF_##name 비트를 set.
 *   - void bfq_clear_bfqq_##name(bfqq) : 같은 비트를 clear.
 *   - int  bfq_bfqq_##name(bfqq)       : 같은 비트를 test 해 0/1 반환.
 * 12개 상태 플래그 x 3개 함수 = 36개 함수를 반복 없이 생성하기 위한
 * 전형적인 X-매크로(반복되는 보일러플레이트 코드 생성) 패턴이다.
 *
 * bfqq->flags 는 unsigned long 하나에 여러 boolean 상태를 비트로 압축해
 * 담는 필드이며(선언은 struct bfq_queue, bfq-iosched.h), 여기서는
 * __set_bit/__clear_bit/test_bit 의 "비원자적(non-atomic, LOCK 프리픽스가
 * 붙는 set_bit 계열과 달리 CPU 락 신호 없이 동작)" 버전을 사용한다. bfqq
 * 의 상태 갱신은 항상 해당 장치의 bfqd->lock 스핀락을 쥔 상태에서만
 * 이루어지므로(호출자가 이를 보장), 원자적 비트 연산의 CAS 비용을 지불할
 * 필요가 없다 - bfqd->lock 자체가 곧 이 필드에 대한 상호배제 수단이다.
 *
 * 실행 컨텍스트: bfq_insert_request/bfq_dispatch_request/bfq_bfqq_expire 등
 * 스케줄러 콜백 내부에서, 항상 bfqd->lock 보유 중에 호출된다.
 * 호출자: 아래 12개의 BFQ_BFQQ_FNS() 매크로 호출로 생성된 각 함수들이며,
 * 이 함수들은 이 파일 전역에서 bfqq 의 상태 전이(활성화/유휴대기/병합 등)를
 * 표현하는 데 광범위하게 쓰인다.
 */
#define BFQ_BFQQ_FNS(name)						\
void bfq_mark_bfqq_##name(struct bfq_queue *bfqq)			\
{									\
	__set_bit(BFQQF_##name, &(bfqq)->flags);	/* [한국어] 비트 set(비원자적) - bfqd->lock 보유 중 호출 전제 */ \
}									\
void bfq_clear_bfqq_##name(struct bfq_queue *bfqq)			\
{									\
	__clear_bit(BFQQF_##name, &(bfqq)->flags);	/* [한국어] 비트 clear - 상태 종료/전이 시 호출 */ \
}									\
int bfq_bfqq_##name(const struct bfq_queue *bfqq)			\
{									\
	return test_bit(BFQQF_##name, &(bfqq)->flags);	/* [한국어] 비트 상태 조회, 0/1 반환 - 조건 분기에서 사용 */ \
}

/* [한국어] just_created: bfqq가 방금 새로 할당되어 아직 첫 request의 실제
 * 서비스 패턴을 관찰하지 못한 "따끈따끈한" 상태임을 표시. bfqq를 새로 만들
 * 때 set 되고, 첫 request가 실제로 dispatch/idle 판정을 거치는 시점에
 * clear 되어 burst 감지·협력 큐 병합(merge) 판단에 사용된다. 이 플래그가
 * 서 있는 동안에는 아직 신뢰할 통계가 없으므로 여러 휴리스틱이 판단을
 * 보류한다. */
BFQ_BFQQ_FNS(just_created);
/* [한국어] busy: bfqq가 최소 1개의 pending/in-service request를 갖고 있어
 * 스케줄러의 활성(active) 큐 목록에 포함돼 있음을 의미. 큐가 비활성 상태
 * (요청 0개)에서 벗어날 때 set, 마지막 request가 완료·제거되어 큐가 비면
 * clear 된다. bfqd->busy_queues 카운트, B-WF2Q+ 서비스 트리 삽입 여부 등
 * "이 큐가 지금 스케줄링 대상인가"를 판단하는 가장 기본적인 상태 플래그다. */
BFQ_BFQQ_FNS(busy);
/* [한국어] wait_request: 현재 in-service 상태인 bfqq에 새 request가 없어서,
 * 바로 다음 큐로 넘기지 않고 idle_slice_timer로 짧게 대기하며 "같은
 * 프로세스가 곧 또 request를 낼 것"을 기다리는 중임을 표시. 타이머를
 * 걸 때 set, 새 request 도착 또는 타이머 만료 시 clear. 이 플래그가 서
 * 있는 동안 도착하는 request는 think-time(ttime) 추정에 반영된다. */
BFQ_BFQQ_FNS(wait_request);
/* [한국어] non_blocking_wait_rq: wait_request 와 함께 쓰이되, "장치를
 * idle 상태로 두지 않고" request 도착을 기다리는 특수 케이스임을 표시
 * (커널 원문 주석: "waiting for a request without idling the device").
 * 주로 soft real-time 큐가 다음 주기적 request를 기다릴 때 사용되며, 이
 * 경우 실제 idling(장치를 놀리는 것)은 하지 않으면서도 소프트 리얼타임
 * 판정을 위한 대기 상태만 표시한다. */
BFQ_BFQQ_FNS(non_blocking_wait_rq);
/* [한국어] fifo_expire: 이번 서비스 슬라이스(in-service 구간) 동안 이미
 * FIFO(선입선출) 기반 만료 검사를 한 번 수행했음을 표시해 같은 슬라이스
 * 내에서 중복 검사를 막는 플래그. 다음 dispatch 후보를 고를 때 set 하고,
 * 큐가 만료되어 새 서비스 슬라이스를 시작할 때 clear 된다. 아래
 * bfq_fifo_expire[sync/async] 시간 안에 서비스되지 못한 request를 강제로
 * 우선시키는 데드라인 보장 메커니즘과 짝을 이룬다. */
BFQ_BFQQ_FNS(fifo_expire);
/* [한국어] has_short_ttime: 이 큐를 만드는 프로세스의 think-time(요청과
 * 요청 사이 대기 시간, ttime)이 BFQ_MIN_TT/idle 임계값보다 짧다고 판단됨을
 * 표시 - 즉 "빠르게 연달아 요청을 내는" 대화형/버스트형 패턴. 관측된 평균
 * ttime을 근거로 set/clear 하며, 이 값이 true면 device idling을 적용해
 * 다음 request가 도착할 때까지 장치를 비워두는 것이 지연시간 관점에서
 * 유리하다고 판단하는 근거가 된다. */
BFQ_BFQQ_FNS(has_short_ttime);
/* [한국어] sync: 이 bfqq가 동기(synchronous) I/O 전용 큐인지(true) 아니면
 * 비동기(주로 buffered write) I/O 전용 큐인지(false)를 표시. bfqq 생성
 * 시 rq_is_sync(bio)/ioprio 문맥에 따라 고정적으로 set 되며(한 bfqq는
 * 평생 sync 아니면 async), bic->bfqq[is_sync][actuator] 배열의 인덱스와도
 * 대응한다. sync 큐는 async 큐보다 낮은 지연시간을 목표로 하며, 아래
 * bfq_async_charge_factor로 인한 페널티도 받지 않는다. */
BFQ_BFQQ_FNS(sync);
/* [한국어] IO_bound: 이 큐가 최소 한 번 이상, 할당 예산(budget)의 20%
 * (2/10) 이하만 소비한 채로 타임아웃된 적이 있어, "CPU-bound가 아니라
 * 실제로 I/O를 계속 필요로 하는 프로세스"로 판단됨을 표시(커널 원문 주석
 * 참고). weight-raising 갱신 로직에서 이 플래그를 근거로 soft real-time
 * 재승격 여부를 결정한다. */
BFQ_BFQQ_FNS(IO_bound);
/* [한국어] in_large_burst: 이 큐가 "대량 동시 생성(burst)" 상황(예:
 * make -j, 셸 스크립트가 짧은 명령을 fork/exec로 연달아 실행) 중에
 * 만들어졌다고 bfq_handle_burst()가 판정했음을 표시. burst로 생성된
 * 큐들은 서로 cooperator로 오인되어 잘못 병합되거나, 불필요하게
 * weight-raising 되면 오히려 전체 처리량을 해치므로, 이 플래그가 서
 * 있는 큐는 weight-raising 적용 대상에서 제외된다. */
BFQ_BFQQ_FNS(in_large_burst);
/* [한국어] coop: 이 bfqq가 둘 이상의 프로세스(I/O 컨텍스트)가 공유하는
 * "협력(cooperating) 큐"로 병합되어 있음을 표시. bfq_setup_cooperator()가
 * 인접 LBA 접근 패턴을 근거로 두 bfqq를 하나로 합칠 때 set 되며, 이후
 * bic_to_bfqq()로 조회되는 큐가 실제로는 여러 bic에서 공유된다. 순차
 * 접근하는 여러 프로세스(예: 병렬 압축 해제)를 하나의 스케줄링 단위로
 * 묶어 예산을 합산함으로써 처리량을 높이기 위한 최적화다. */
BFQ_BFQQ_FNS(coop);
/* [한국어] split_coop: 위 coop 로 합쳐졌던 공유 큐를 다시 원래 프로세스별
 * 큐로 분리(split)해야 함을 표시하는 예약 플래그. 공유된 두 프로세스의
 * 접근 패턴이 더 이상 인접하지 않게 되면(cooperator 관계 해소) 이 플래그가
 * set 되고, 다음 기회에 실제 분리가 수행된 뒤 clear 된다. 잘못된 병합을
 * 오래 유지하지 않고 되돌리기 위한 안전장치다. */
BFQ_BFQQ_FNS(split_coop);
/* [한국어] softrt_update: 이 큐의 "소프트 리얼타임(soft real-time) 다음
 * 시작 시각"(soft_rt_next_start, bfq_bfqq_softrt_next_start()가 계산)을
 * 다음 기회에 재계산해야 함을 표시하는 지연(deferred) 갱신 플래그. 매
 * request 마다 즉시 재계산하는 대신, 상태가 실제로 바뀐 시점(예: 만료·
 * 재활성화)에만 set 해 두었다가 필요한 시점에 한 번만 계산 비용을 지불
 * 하기 위한 최적화다. */
BFQ_BFQQ_FNS(softrt_update);
#undef BFQ_BFQQ_FNS						\

/* Expiration time of async (0) and sync (1) requests, in ns. */
/* [한국어] 비동기(0)/동기(1) request가 큐에 대기할 수 있는 최대 시간(FIFO
 * 데드라인, ns 단위: async=250ms, sync=125ms). bfq_check_fifo()가 이
 * 시간을 넘긴 가장 오래된 request를 발견하면, LBA 정렬(sort_list) 순서를
 * 무시하고 그 request를 강제로 다음 dispatch 후보로 승격시켜 "무한
 * 기아(starvation)"를 막는다. sync 쪽이 async 쪽보다 짧아 대화형 읽기가
 * 버퍼드 쓰기에 밀려 너무 오래 대기하지 않도록 한다. */
static const u64 bfq_fifo_expire[2] = { NSEC_PER_SEC / 4, NSEC_PER_SEC / 8 };

/* Maximum backwards seek (magic number lifted from CFQ), in KiB. */
/* [한국어] "뒤로"(더 작은 LBA로) 향하는 seek을 허용하는 최대 거리(KiB).
 * bfq_choose_req()가 다음 dispatch할 request를 고를 때, 현재 위치보다
 * 뒤에 있는 request가 이 거리 이내면 "충분히 가깝다"고 보고 앞쪽(전진)
 * request와 동등하게 비교 대상에 포함시킨다. 이 거리를 넘어서면 뒤로
 * 가는 대신 다음 앞쪽 request를 기다리는 편이 유리하다고 판단한다
 * (CFQ에서 물려받은 튜닝 값). */
static const int bfq_back_max = 16 * 1024;

/* Penalty of a backwards seek, in number of sectors. */
/* [한국어] 뒤로 가는 seek에 부여하는 가상 거리 페널티 배수. bfq_choose_req()
 * 는 실제 섹터 거리에 이 배수를 곱해 "체감 거리"를 늘림으로써, 앞쪽(전진)
 * request와 뒤쪽(후진) request가 실제로는 비슷한 거리라도 뒤쪽을 상대적
 * 으로 불리하게 만든다. 회전형 디스크에서 헤드가 되돌아가는 물리적 탐색
 * 비용을 반영한 것이며, 바로 위 bfq_back_max와 함께 사용된다. */
static const int bfq_back_penalty = 2;

/* Idling period duration, in ns. */
/* [한국어] device idling(장치를 일부러 놀리며 대기하는 시간)의 기본
 * 길이(ns, 8ms). in-service bfqq에 다음 request가 없을 때, 이 시간만큼
 * idle_slice_timer를 걸어 두고 같은 프로세스의 다음 request를 기다린다.
 * 이 시간 안에 request가 오면 대기 없이 이어서 처리해 지연을 낮추고
 * (공정성/저지연 목표), 안 오면 타이머 만료 후 다음 큐로 넘어가 처리량을
 * 확보한다. const가 아닌 static u64인 이유는 sysfs(/sys/block/<disk>/queue/
 * iosched/slice_idle)를 통해 사용자가 런타임에 튜닝할 수 있게 하기
 * 위함이다. */
static u64 bfq_slice_idle = NSEC_PER_SEC / 125;

/* Minimum number of assigned budgets for which stats are safe to compute. */
/* [한국어] peak_rate 등 장치 통계를 신뢰할 수 있다고 판단하기 위해 필요한
 * 최소 "예산 할당(=서비스 슬라이스 시작)" 횟수. 이 횟수 미만일 때는 아직
 * 표본이 부족해 추정치(peak rate, weight-raising duration 등)의 변동성이
 * 크므로, 관련 로직이 자동 계산 대신 기본값/보수적 값을 사용한다. */
static const int bfq_stats_min_budgets = 194;

/* Default maximum budget values, in sectors and number of requests. */
/* [한국어] budget 자동 조정(peak-rate 기반)이 아직 이루어지지 않았거나
 * 비활성화된 경우 사용하는 기본 최대 예산(섹터 단위, 16*1024=16384섹터
 * =8MiB). bfq_queue가 새로 생성될 때 entity의 max_budget 초기값으로
 * 쓰이며, 이후 bfq_update_peak_rate()가 실제 장치 처리율을 학습하면
 * 장치별 최적값으로 대체된다. */
static const int bfq_default_max_budget = 16 * 1024;

/*
 * When a sync request is dispatched, the queue that contains that
 * request, and all the ancestor entities of that queue, are charged
 * with the number of sectors of the request. In contrast, if the
 * request is async, then the queue and its ancestor entities are
 * charged with the number of sectors of the request, multiplied by
 * the factor below. This throttles the bandwidth for async I/O,
 * w.r.t. to sync I/O, and it is done to counter the tendency of async
 * writes to steal I/O throughput to reads.
 *
 * The current value of this parameter is the result of a tuning with
 * several hardware and software configurations. We tried to find the
 * lowest value for which writes do not cause noticeable problems to
 * reads. In fact, the lower this parameter, the stabler I/O control,
 * in the following respect.  The lower this parameter is, the less
 * the bandwidth enjoyed by a group decreases
 * - when the group does writes, w.r.t. to when it does reads;
 * - when other groups do reads, w.r.t. to when they do writes.
 */
/* [한국어] 비동기(주로 buffered write) request가 소비한 섹터 수에 곱해지는
 * "가중 페널티" 배수(=3). sync request는 실제 섹터 수 그대로, async
 * request는 (섹터 수 * 3)만큼 entity의 budget/서비스 카운터를 차감시켜,
 * 동일한 처리량이라도 async 쪽이 예산을 3배 빨리 소진하게(=더 자주 선점
 * 당하게) 만든다. 이렇게 함으로써 버퍼드 쓰기가 읽기(read)의 처리량을
 * 잠식하는 CFQ 이래의 고질적 문제(쓰기가 읽기 대역폭을 훔치는 현상)를
 * 완화한다. entity 서비스 차감 경로에서 사용된다. */
static const int bfq_async_charge_factor = 3;

/* Default timeout values, in jiffies, approximating CFQ defaults. */
/* [한국어] 하나의 bfqq가 in-service 상태로 있을 수 있는 최대 시간(HZ/8 =
 * 125ms @ HZ=1000)을 규정하는 시간 기반 백스톱. BFQ는 원칙적으로 "시간"이
 * 아니라 "budget(섹터)" 소진 여부로 서비스 슬라이스를 끝내지만, budget을
 * 다 못 채우고도 지나치게 오래 장치를 붙잡는 것을 막기 위해 이 타임아웃도
 * 함께 적용한다(budget timeout 만료 사유). CFQ의 기본 시간 슬라이스
 * 값을 근사하도록 선택된 값이다. */
const int bfq_timeout = HZ / 8;

/*
 * Time limit for merging (see comments in bfq_setup_cooperator). Set
 * to the slowest value that, in our tests, proved to be effective in
 * removing false positives, while not causing true positives to miss
 * queue merging.
 *
 * As can be deduced from the low time limit below, queue merging, if
 * successful, happens at the very beginning of the I/O of the involved
 * cooperating processes, as a consequence of the arrival of the very
 * first requests from each cooperator.  After that, there is very
 * little chance to find cooperators.
 */
/* [한국어] 두 bfqq를 협력(cooperating) 관계로 보고 병합할지 판단하는 시간
 * 창(window, HZ/10 = 100ms). bfq_setup_cooperator()는 새 bfqq가 생성된
 * 시각과 병합 후보 bfqq가 생성된 시각의 차이가 이 값 이내일 때만 병합을
 * 시도한다. 두 프로세스가 "거의 동시에" I/O를 시작했다는 것은 서로 협력
 * 관계(예: 병렬 압축 해제/스트리밍 복사)일 가능성이 높다는 경험적 신호이며,
 * 값이 작을수록 오탐(false positive)은 줄지만 실제 협력 관계를 놓칠
 * 위험도 커지는 트레이드오프가 있다. */
static const unsigned long bfq_merge_time_limit = HZ/10;

/* [한국어] struct bfq_queue 전용 슬랩 캐시 핸들. bfq 모듈 초기화 시
 * kmem_cache_create("bfq_queue", sizeof(struct bfq_queue), ...)로 생성되고,
 * bfq_get_queue() 경로에서 kmem_cache_alloc(bfq_pool, ...)로 새 bfqq를
 * 할당, bfq_put_queue()에서 마지막 참조가 사라질 때 kmem_cache_free(bfq_pool,
 * bfqq)로 반환한다. 프로세스/그룹마다 빈번히 생성·소멸되는 bfqq를 범용
 * kmalloc 대신 전용 캐시로 관리해 할당 지역성과 속도를 높인다. */
static struct kmem_cache *bfq_pool;

/* Below this threshold (in ns), we consider thinktime immediate. */
/* [한국어] 프로세스의 think-time(요청 완료 후 다음 요청까지의 대기 시간,
 * ttime)이 이 값(2ms) 미만이면 "사실상 대기 시간이 없다(immediate)"고
 * 간주한다. 관측된 ttime을 이 임계값과 비교해 has_short_ttime 플래그
 * (위 BFQ_BFQQ_FNS(has_short_ttime) 참고) 판정의 기초 자료로 사용한다. */
#define BFQ_MIN_TT		(2 * NSEC_PER_MSEC)

/* hw_tag detection: parallel requests threshold and min samples needed. */
/* [한국어] 장치가 실제로 "하드웨어 큐잉(NCQ 등 다중 명령 동시 처리)"을
 * 지원하는지(hw_tag) 자동 감지하기 위한 두 파라미터. 최근
 * BFQ_HW_QUEUE_SAMPLES(32)번의 request 완료 동안 관측된 평균 동시
 * in-flight 요청 수가 BFQ_HW_QUEUE_THRESHOLD(3)를 넘으면 hw_tag=1로
 * 판정한다. NVMe SSD는 태생적으로 다중 큐(SQ 여러 개, 큐당 다수 CID)를
 * 지원하므로 사실상 항상 hw_tag=1로 수렴하며, 이 값은 idling 적용 여부
 * (hw_tag=1인 장치는 병렬성이 이미 충분하므로 idling을 덜 적용) 판단에
 * 쓰인다. */
#define BFQ_HW_QUEUE_THRESHOLD	3
#define BFQ_HW_QUEUE_SAMPLES	32

#define BFQQ_SEEK_THR		(sector_t)(8 * 100)
/* [한국어] "seek(탐색)"으로 간주할 최소 거리(섹터, 8*100=800섹터≈400KiB).
 * 직전 dispatch 위치(last_pos)와 이번 request 위치의 거리가 이보다 멀면
 * 순차(sequential) 접근이 아니라 탐색성(random-ish) 접근으로 본다. */
#define BFQQ_SECT_THR_NONROT	(sector_t)(2 * 32)
/* [한국어] 비회전형(SSD/NVMe) 장치에서 "이 request 자체가 작다"고 볼
 * 임계 크기(섹터, 2*32=64섹터=32KiB). SSD는 물리적 헤드 이동이 없어 먼
 * 거리 접근 자체는 페널티가 없지만, "작은 요청이 여기저기 흩어져 온다"는
 * 패턴은 여전히 캐시/컨트롤러 효율을 떨어뜨리는 신호로 취급한다. */
#define BFQ_RQ_SEEKY(bfqd, last_pos, rq) \
	(get_sdist(last_pos, rq) >			\
	 BFQQ_SEEK_THR &&				\
	 (blk_queue_rot(bfqd->queue) ||			\
	  blk_rq_sectors(rq) < BFQQ_SECT_THR_NONROT))
/* [한국어] 이번 request가 "seeky"한지 판정하는 매크로. (1) 거리 조건:
 * get_sdist(직전 위치와의 절대 섹터 거리)가 BFQQ_SEEK_THR를 넘어야 하고,
 * (2) 매체 조건: 회전형 디스크라면 거리 조건만으로 충분히 seeky로 보지만,
 * 비회전형(SSD/NVMe)이라면 추가로 request 크기가 BFQQ_SECT_THR_NONROT
 * 보다 작을 때만 seeky로 인정한다 - SSD에서는 크고 순차에 가까운 request는
 * 멀리 있어도 탐색 페널티가 없으므로 관대하게 봐준다는 뜻이다. 결과는
 * bfqq->seek_history 비트마스크(아래 BFQQ_SEEKY 참고)에 매 request마다
 * 누적된다. */
#define BFQQ_CLOSE_THR		(sector_t)(8 * 1024)
/* [한국어] 서로 "인접한 LBA"로 볼 거리 임계값(섹터, 8*1024=8192섹터=4MiB).
 * bfqq->pos_root(rq_pos_tree)에서 협력(cooperator) 큐 후보를 찾을 때,
 * 이 거리 이내에 있는 다른 bfqq를 "근접 접근"으로 보고 병합(merge) 후보로
 * 고려한다. */
#define BFQQ_SEEKY(bfqq)	(hweight32(bfqq->seek_history) > 19)
/* [한국어] bfqq->seek_history는 최근 32개 request 각각이 seeky했는지를
 * 비트로 기록한 이동 창(sliding window) 비트마스크다. hweight32로 그 중
 * 1로 설정된 비트 수(=최근 32개 중 seeky했던 request 수)를 세어 19개
 * (약 60%)를 넘으면 "이 큐는 전반적으로 탐색성(seeky) 패턴"이라고
 * 판정한다. seeky한 큐는 순차 큐보다 soft real-time/interactive 오판
 * 가능성이 낮게 취급된다. */
/*
 * Sync random I/O is likely to be confused with soft real-time I/O,
 * because it is characterized by limited throughput and apparently
 * isochronous arrival pattern. To avoid false positives, queues
 * containing only random (seeky) I/O are prevented from being tagged
 * as soft real-time.
 */
/* [한국어] seek_history의 모든 32비트가 다 1(-1은 unsigned 캐스팅 시
 * 0xFFFFFFFF)이라는 것은 "최근 32개 request 전부가 seeky했다"는 뜻이다.
 * 이런 큐는 위 BFQQ_SEEKY보다 더 강한 확신을 가지고 "완전히 탐색성"으로
 * 판정하며, soft real-time 승격 후보에서 원천 배제해 sync random I/O를
 * soft-rt로 오판하는 것을 막는다(위 커널 원문 주석 참고). */
#define BFQQ_TOTALLY_SEEKY(bfqq)	(bfqq->seek_history == -1)

/* Min number of samples required to perform peak-rate update */
/* [한국어] bfq_update_peak_rate()가 peak_rate 추정치를 갱신하기 전에
 * 필요한 최소 관측 request 수(32개). 표본이 너무 적으면 순간적인 튐
 * (burst)을 장치의 실제 최대 처리율로 오인할 수 있어 이 하한을 둔다. */
#define BFQ_RATE_MIN_SAMPLES	32
/* Min observation time interval required to perform a peak-rate update (ns) */
/* [한국어] peak_rate 갱신 1회를 위해 필요한 최소 관측 시간(300ms). 너무
 * 짧은 구간으로 순간 처리율을 계산하면 노이즈가 커지므로, 표본 개수
 * 조건(BFQ_RATE_MIN_SAMPLES)과 함께 "충분히 긴 구간을 관측했는가"도
 * 검증한다. */
#define BFQ_RATE_MIN_INTERVAL	(300*NSEC_PER_MSEC)
/* Target observation time interval for a peak-rate update (ns) */
/* [한국어] peak_rate 관측 구간의 "목표" 길이(1초). 실제 관측 구간이 이
 * 값에 못 미치면 다음 request들까지 계속 같은 구간에 누적해, 통계적으로
 * 안정된 1초 분량의 표본을 모은 뒤에야 peak_rate를 갱신하려 시도한다. */
#define BFQ_RATE_REF_INTERVAL	NSEC_PER_SEC

/*
 * Shift used for peak-rate fixed precision calculations.
 * With
 * - the current shift: 16 positions
 * - the current type used to store rate: u32
 * - the current unit of measure for rate: [sectors/usec], or, more precisely,
 *   [(sectors/usec) / 2^BFQ_RATE_SHIFT] to take into account the shift,
 * the range of rates that can be stored is
 * [1 / 2^BFQ_RATE_SHIFT, 2^(32 - BFQ_RATE_SHIFT)] sectors/usec =
 * [1 / 2^16, 2^16] sectors/usec = [15e-6, 65536] sectors/usec =
 * [15, 65G] sectors/sec
 * Which, assuming a sector size of 512B, corresponds to a range of
 * [7.5K, 33T] B/sec
 */
/* [한국어] peak_rate를 정수(u32) 안에 고정소수점으로 저장하기 위한 시프트
 * 비트 수(16). 실제 저장값 = (섹터/usec 단위의 실제 처리율) << 16, 즉
 * "1/65536 섹터/usec" 단위로 표현한다. 나눗셈 대신 곱셈+시프트 연산으로
 * 처리해 나눗셈 비용을 피하면서도(고정소수점 산술) 위 원문 주석이 계산한
 * 대로 약 [7.5KB/s, 33TB/s] 범위의 처리율을 정수 오버플로 없이 표현할
 * 수 있다. */
#define BFQ_RATE_SHIFT		16

/*
 * When configured for computing the duration of the weight-raising
 * for interactive queues automatically (see the comments at the
 * beginning of this file), BFQ does it using the following formula:
 * duration = (ref_rate / r) * ref_wr_duration,
 * where r is the peak rate of the device, and ref_rate and
 * ref_wr_duration are two reference parameters.  In particular,
 * ref_rate is the peak rate of the reference storage device (see
 * below), and ref_wr_duration is about the maximum time needed, with
 * BFQ and while reading two files in parallel, to load typical large
 * applications on the reference device (see the comments on
 * max_service_from_wr below, for more details on how ref_wr_duration
 * is obtained).  In practice, the slower/faster the device at hand
 * is, the more/less it takes to load applications with respect to the
 * reference device.  Accordingly, the longer/shorter BFQ grants
 * weight raising to interactive applications.
 *
 * BFQ uses two different reference pairs (ref_rate, ref_wr_duration),
 * depending on whether the device is rotational or non-rotational.
 *
 * In the following definitions, ref_rate[0] and ref_wr_duration[0]
 * are the reference values for a rotational device, whereas
 * ref_rate[1] and ref_wr_duration[1] are the reference values for a
 * non-rotational device. The reference rates are not the actual peak
 * rates of the devices used as a reference, but slightly lower
 * values. The reason for using slightly lower values is that the
 * peak-rate estimator tends to yield slightly lower values than the
 * actual peak rate (it can yield the actual peak rate only if there
 * is only one process doing I/O, and the process does sequential
 * I/O).
 *
 * The reference peak rates are measured in sectors/usec, left-shifted
 * by BFQ_RATE_SHIFT.
 */
/* [한국어] weight-raising 지속시간(duration) 자동 계산 공식
 * duration = (ref_rate / 실제 peak_rate) * ref_wr_duration 에서 쓰이는
 * "기준 장치의 peak_rate" 두 값. ref_rate[0]=14000은 회전형(HDD) 기준
 * 장치, ref_rate[1]=33000은 비회전형(SSD) 기준 장치의 처리율이며, 단위는
 * BFQ_RATE_SHIFT만큼 왼쪽 시프트된 섹터/usec 고정소수점 값이다. 실제
 * 측정 장치가 이 기준보다 빠르면(r > ref_rate) duration이 짧아지고,
 * 느리면 길어진다 - "느린 장치일수록 인터랙티브 앱 로딩에 시간이 더
 * 걸리니 weight-raising도 더 오래 유지해준다"는 논리다. */
static int ref_rate[2] = {14000, 33000};
/*
 * To improve readability, a conversion function is used to initialize
 * the following array, which entails that the array can be
 * initialized only in a function.
 */
/* [한국어] ref_rate[]에 대응하는 "기준 weight-raising 지속시간"(ms 단위,
 * 회전형/비회전형 각각). 위 duration 공식의 두 번째 인자이며, 이 배열
 * 자체는 0으로 초기화된 채 선언만 되고, 실제 값은 함수 내부에서
 * msecs_to_jiffies() 등을 이용해 런타임에 채워진다(그래서 정적 초기화
 * 리스트 대신 함수 호출로 초기화해야 함 - 바로 위 원문 주석 참고). */
static int ref_wr_duration[2];

/*
 * BFQ uses the above-detailed, time-based weight-raising mechanism to
 * privilege interactive tasks. This mechanism is vulnerable to the
 * following false positives: I/O-bound applications that will go on
 * doing I/O for much longer than the duration of weight
 * raising. These applications have basically no benefit from being
 * weight-raised at the beginning of their I/O. On the opposite end,
 * while being weight-raised, these applications
 * a) unjustly steal throughput to applications that may actually need
 * low latency;
 * b) make BFQ uselessly perform device idling; device idling results
 * in loss of device throughput with most flash-based storage, and may
 * increase latencies when used purposelessly.
 *
 * BFQ tries to reduce these problems, by adopting the following
 * countermeasure. To introduce this countermeasure, we need first to
 * finish explaining how the duration of weight-raising for
 * interactive tasks is computed.
 *
 * For a bfq_queue deemed as interactive, the duration of weight
 * raising is dynamically adjusted, as a function of the estimated
 * peak rate of the device, so as to be equal to the time needed to
 * execute the 'largest' interactive task we benchmarked so far. By
 * largest task, we mean the task for which each involved process has
 * to do more I/O than for any of the other tasks we benchmarked. This
 * reference interactive task is the start-up of LibreOffice Writer,
 * and in this task each process/bfq_queue needs to have at most ~110K
 * sectors transferred.
 *
 * This last piece of information enables BFQ to reduce the actual
 * duration of weight-raising for at least one class of I/O-bound
 * applications: those doing sequential or quasi-sequential I/O. An
 * example is file copy. In fact, once started, the main I/O-bound
 * processes of these applications usually consume the above 110K
 * sectors in much less time than the processes of an application that
 * is starting, because these I/O-bound processes will greedily devote
 * almost all their CPU cycles only to their target,
 * throughput-friendly I/O operations. This is even more true if BFQ
 * happens to be underestimating the device peak rate, and thus
 * overestimating the duration of weight raising. But, according to
 * our measurements, once transferred 110K sectors, these processes
 * have no right to be weight-raised any longer.
 *
 * Basing on the last consideration, BFQ ends weight-raising for a
 * bfq_queue if the latter happens to have received an amount of
 * service at least equal to the following constant. The constant is
 * set to slightly more than 110K, to have a minimum safety margin.
 *
 * This early ending of weight-raising reduces the amount of time
 * during which interactive false positives cause the two problems
 * described at the beginning of these comments.
 */
/* [한국어] 한 bfqq가 weight-raising 상태에서 실제로 서비스받은 섹터 수가
 * 이 값(약 120,000섹터 ≈ 58.6MiB)에 도달하면, 아직 duration 타이머가
 * 안 끝났더라도 weight-raising을 조기 종료시키는 임계값. LibreOffice
 * Writer 기동 같은 "진짜 인터랙티브" 작업은 대개 110K섹터 이전에 로딩이
 * 끝나므로, 그보다 훨씬 더 많은 서비스를 받고 있다는 것은 이 큐가 사실
 * "오래 지속되는 순차 I/O 작업"(파일 복사 등)이지 인터랙티브가 아니라는
 * 뜻으로 보고 특혜를 회수한다. */
static const unsigned long max_service_from_wr = 120000;

/*
 * Maximum time between the creation of two queues, for stable merge
 * to be activated (in ms)
 */
/* [한국어] "안정적 병합(stable merge)" 후보로 고려되기 위해 두 bfqq의
 * 생성 시각이 이 값(600ms) 이내로 가까워야 한다는 시간 창. stable merge는
 * bfq_merge_time_limit 기반의 즉시적 협력(coop) 병합과 달리, 과거에
 * 병합된 적 있는 프로세스 쌍의 이력을 기억해 두었다가(bic->bfqq_data의
 * stable_merge_bfqq 등) 큐가 재생성되어도 다시 병합을 시도하는 메커니즘
 * 이며, 이 값은 그 후보 등록의 1차 시간 조건이다. */
static const unsigned long bfq_activation_stable_merging = 600;
/*
 * Minimum time to be waited before evaluating delayed stable merge (in ms)
 */
/* [한국어] 위 stable merge 후보로 등록된 뒤, 실제로 병합을 평가(evaluate)
 * 하기까지 최소한 기다려야 하는 시간(600ms). 큐 생성 직후 곧바로 병합을
 * 확정하지 않고 유예 기간을 두어, 그 사이 큐의 실제 I/O 패턴을 조금 더
 * 관찰한 뒤(예: 진짜 협력 관계인지) 최종 판단하기 위함이다. */
static const unsigned long bfq_late_stable_merging = 600;

#define RQ_BIC(rq)		((struct bfq_io_cq *)((rq)->elv.priv[0]))
/* [한국어] struct request의 blk-mq 스케줄러 전용 슬롯 elv.priv[0]에
 * bfq_prepare_request()가 미리 저장해 둔 struct bfq_io_cq*(요청을 낸
 * 프로세스의 I/O 컨텍스트)를 꺼내는 캐스팅 매크로. elv.priv[]는 blk-mq가
 * 스케줄러에게 request마다 자유롭게 쓰라고 내주는 2개의 void* 슬롯이며,
 * BFQ는 이를 통해 "이 request가 누구 것인지"를 매 콜백에서 다시 찾지
 * 않고 O(1)로 알아낸다. */
#define RQ_BFQQ(rq)		((rq)->elv.priv[1])
/* [한국어] 같은 방식으로 elv.priv[1]에 저장해 둔 struct bfq_queue*(이
 * request가 최종적으로 배정된 bfqq)를 꺼내는 매크로. RQ_BIC과 짝을
 * 이루며, request 완료·제거 경로에서 "이 request가 어느 bfqq 소속이었는지"
 * 를 빠르게 되찾는 데 쓰인다. */

/*
 * [한국어]
 * bic_to_bfqq - bic(bfq_io_cq, 프로세스별 I/O 컨텍스트)에서 해당 프로세스가
 *               사용 중인 bfq_queue(bfqq)를 조회한다.
 *
 * @bic: 조회 대상 프로세스의 I/O 컨텍스트. task_struct의 io_context 아래에
 *       elevator별로 하나씩 존재하며, bfqq[sync/async][actuator] 2차원
 *       배열로 이 프로세스가 소유한 큐들을 보관한다.
 * @is_sync: true면 동기(sync) I/O용 bfqq, false면 비동기(async) I/O용
 *           bfqq를 조회한다.
 * @actuator_idx: 멀티 액추에이터(다중 헤드) 디스크에서 이 요청이 속하는
 *                액추에이터 인덱스. 단일 액추에이터 장치에서는 항상 0.
 * @return: 조회된 bfq_queue 포인터. 해당 프로세스가 그 방향(sync/async)의
 *          큐를 아직 생성한 적이 없으면 NULL.
 *
 * BFQ는 프로세스(bic)마다 sync/async 방향과 actuator별로 별도의 bfqq를
 * 유지하는데, 이 함수는 bic->bfqq[is_sync][actuator_idx] 배열을 그대로
 * 인덱싱하는 단순 접근자(accessor)다. 별도의 락을 잡지 않으므로, 호출자가
 * 이미 bfqd->lock을 들고 있거나(스케줄러 콜백 경로) bic가 현재 태스크
 * 전용이라 동시 수정 우려가 없는 컨텍스트에서만 호출해야 한다.
 *
 * 호출 체인:
 *   bfq_get_queue/bfq_limit_depth/bfqq_request_over_limit 등
 *   → [bic_to_bfqq] → (배열 인덱싱, 하위 호출 없음)
 */
struct bfq_queue *bic_to_bfqq(struct bfq_io_cq *bic, bool is_sync,
			      unsigned int actuator_idx)
{
	if (is_sync) /* 동기 I/O는 BFQ 내부 관례상 bfqq[1] 슬롯에 저장됨 */
		return bic->bfqq[1][actuator_idx]; /* 해당 actuator의 동기용 bfqq 반환 */

	return bic->bfqq[0][actuator_idx]; /* 비동기(async)는 bfqq[0] 슬롯에서 반환 */
}

/* [한국어] stable-merge(큐 생성 직후의 조기 병합) 후보로 붙잡아 둔 bfqq의
 * 참조를 되돌려주는 함수의 전방 선언. 정의는 파일 뒤쪽에 있지만
 * bic_set_bfqq()가 그보다 먼저 이 함수를 호출해야 하므로 여기에서
 * 미리 선언한다. stable merge는 "아직 병합할지 말지 결정하지 않은"
 * 상태에서 상대 큐가 해제되지 않도록 별도의 참조(stable_merge_bfqq)를
 * 잡아 두는데, bic가 다른 bfqq로 재연결되면 그 참조를 여기서 푼다. */
static void bfq_put_stable_ref(struct bfq_queue *bfqq);

/*
 * [한국어]
 * bic_set_bfqq - bic(프로세스 I/O 컨텍스트)의 지정된 방향/actuator 슬롯에
 *                bfqq를 등록하거나 기존 연결을 해제한다.
 *
 * @bic: 갱신할 프로세스의 I/O 컨텍스트.
 * @bfqq: 새로 연결할 bfq_queue. NULL이면 해당 슬롯의 연결을 끊는다(큐 해제/
 *        병합 시 호출).
 * @is_sync: true=동기 슬롯(bfqq[1]), false=비동기 슬롯(bfqq[0]) 선택.
 * @actuator_idx: 대상 액추에이터 인덱스.
 * @return: 없음(void).
 *
 * bic->bfqq[is_sync][actuator_idx]에 새 큐를 대입하기 전에, 그 슬롯에
 * 이전에 있던 old_bfqq가 오직 이 bic만을 소유자로 여기고 있었다면
 * (old_bfqq->bic == bic) 그 역참조를 끊는다. bfqq가 다른 프로세스와의
 * 협력(cooperator) 병합으로 재사용되는 중에 이전 소유자 포인터가
 * 대롱대롱 남는 것을 막기 위함이다. 또한 새로 연결하려는 bfqq가 마침
 * 이 bic이 예약해 둔 "안정적 병합(stable merge)" 후보(stable_merge_bfqq)와
 * 같다면, bfqq가 분리(split) 후 자기 자신과 다시 병합되는 모순을 막기
 * 위해 예약된 stable merge를 즉시 취소한다(참조 카운트 반납 포함).
 * bic/bfqq는 장치 전역 스케줄러 상태와 얽혀 있으므로 이 함수는
 * bfqd->lock을 쥔 상태에서 호출되어야 한다.
 *
 * 호출 체인:
 *   bfq_get_queue/bfq_merge_bfqqs/bfq_bfqq_move 등
 *   → [bic_set_bfqq] → bfq_put_stable_ref
 */
void bic_set_bfqq(struct bfq_io_cq *bic,
		  struct bfq_queue *bfqq,
		  bool is_sync,
		  unsigned int actuator_idx)
{
	/* 이 슬롯에 이전에 연결되어 있던 bfqq - 아래에서 역참조 해제 여부를 판단하는 데 사용 */
	struct bfq_queue *old_bfqq = bic->bfqq[is_sync][actuator_idx];

	/*
	 * If bfqq != NULL, then a non-stable queue merge between
	 * bic->bfqq and bfqq is happening here. This causes troubles
	 * in the following case: bic->bfqq has also been scheduled
	 * for a possible stable merge with bic->stable_merge_bfqq,
	 * and bic->stable_merge_bfqq == bfqq happens to
	 * hold. Troubles occur because bfqq may then undergo a split,
	 * thereby becoming eligible for a stable merge. Yet, if
	 * bic->stable_merge_bfqq points exactly to bfqq, then bfqq
	 * would be stably merged with itself. To avoid this anomaly,
	 * we cancel the stable merge if
	 * bic->stable_merge_bfqq == bfqq.
	 */
	/* 이 actuator에 대한 bic 전용 저장 상태(merge 시 saved_* 필드, stable merge 후보 등)에 접근 */
	struct bfq_iocq_bfqq_data *bfqq_data = &bic->bfqq_data[actuator_idx];

	/* Clear bic pointer if bfqq is detached from this bic */
	/* old_bfqq가 여전히 이 bic를 유일한 소유자로 가리키고 있다면(공유 큐로 전환된 게 아니라면) */
	if (old_bfqq && old_bfqq->bic == bic)
		old_bfqq->bic = NULL; /* 역참조를 끊어 old_bfqq가 이 bic 없이도 안전하게 재사용/해제되도록 함 */

	if (is_sync) /* 동기 슬롯 갱신 */
		bic->bfqq[1][actuator_idx] = bfqq; /* 새 bfqq(또는 NULL)를 동기 슬롯에 연결 */
	else /* 비동기 슬롯 갱신 */
		bic->bfqq[0][actuator_idx] = bfqq; /* 새 bfqq(또는 NULL)를 비동기 슬롯에 연결 */

	/* 새로 연결하는 bfqq가 이 bic이 예약해 둔 stable-merge 대상과 우연히 일치하는 경우 */
	if (bfqq && bfqq_data->stable_merge_bfqq == bfqq) {
		/*
		 * Actually, these same instructions are executed also
		 * in bfq_setup_cooperator, in case of abort or actual
		 * execution of a stable merge. We could avoid
		 * repeating these instructions there too, but if we
		 * did so, we would nest even more complexity in this
		 * function.
		 */
		/* bfqq가 자기 자신과 병합되는 모순을 막기 위해 예약된 stable merge를 취소하며,
		 * stable_merge_bfqq가 쥐고 있던 참조 카운트를 반납한다 */
		bfq_put_stable_ref(bfqq_data->stable_merge_bfqq);

		bfqq_data->stable_merge_bfqq = NULL; /* 취소되었으므로 후보 포인터도 제거 */
	}
}

/*
 * [한국어]
 * bic_to_bfqd - bic가 속한 request_queue의 elevator private 데이터,
 *               즉 장치 전체를 대표하는 bfq_data(bfqd)를 얻는다.
 *
 * @bic: 대상 프로세스의 I/O 컨텍스트. bic->icq.q에 이 bic가 등록된
 *       request_queue 포인터가 들어 있다.
 * @return: 해당 request_queue의 elevator에 연결된 bfq_data 포인터.
 *
 * io_cq(icq)는 (request_queue, io_context) 쌍마다 하나씩 존재하고
 * icq->q는 그 request_queue를 가리킨다. request_queue->elevator->elevator_data는
 * BFQ 등록(bfq_init_queue) 시 저장해 둔 bfq_data이므로, 이 함수는 bic로부터
 * 장치 전역 상태로 가는 지름길(accessor) 역할을 한다. 포인터만 따라가므로
 * 어떤 컨텍스트에서도 호출 가능하지만, 반환된 bfqd의 필드를 실제로 읽고
 * 쓰려면 호출자가 별도로 bfqd->lock을 잡아야 한다.
 *
 * 호출 체인:
 *   bfq_prepare_request/bfq_insert_request 등 elevator 콜백
 *   → [bic_to_bfqd] → (포인터 역참조, 하위 호출 없음)
 */
struct bfq_data *bic_to_bfqd(struct bfq_io_cq *bic)
{
	/* icq.q: 이 bic가 등록된 request_queue, ->elevator->elevator_data: BFQ가
	 * elevator 등록 시 저장해 둔 bfq_data 포인터를 따라가 반환 */
	return bic->icq.q->elevator->elevator_data;
}

/**
 * icq_to_bic - convert iocontext queue structure to bfq_io_cq.
 * @icq: the iocontext queue.
 */
/*
 * [한국어]
 * icq_to_bic - 범용 io_cq 포인터를 BFQ 전용 bfq_io_cq 포인터로 변환한다.
 *
 * @icq: blk-mq I/O 스케줄러 공통 계층이 관리하는 io_cq(요청큐 x I/O
 *       컨텍스트) 구조체 포인터. bfq_io_cq는 이 icq를 첫 번째 멤버로
 *       포함(embed)하는 형태로 정의되어 있다.
 * @return: icq를 감싸고 있는 bfq_io_cq의 시작 주소. icq가 NULL이면 NULL.
 *
 * BFQ는 elevator 등록 시 icq_size/icq_align을 bfq_io_cq 크기로 지정하므로,
 * 커널 공통 코드가 넘겨주는 io_cq*는 실제로는 bfq_io_cq의 첫 필드 주소와
 * 같다. container_of()로 오프셋(여기서는 0)을 빼서 감싸는 구조체 주소를
 * 계산하며, icq가 첫 멤버이기 때문에 NULL 입력도 그대로 NULL로 변환된다
 * (아래 주석 참조).
 *
 * 호출 체인:
 *   bfq_bic_lookup/bfq_icq_init 등 → [icq_to_bic] → (포인터 변환, 하위 호출 없음)
 */
static struct bfq_io_cq *icq_to_bic(struct io_cq *icq)
{
	/* bic->icq is the first member, %NULL will convert to %NULL */
	/* icq 포인터에서 bfq_io_cq::icq 필드의 오프셋(0)을 빼서 감싸는 구조체의
	 * 시작 주소를 계산 - icq가 첫 멤버이므로 NULL도 안전하게 NULL로 유지됨 */
	return container_of(icq, struct bfq_io_cq, icq);
}

/**
 * bfq_bic_lookup - search into @ioc a bic associated to @bfqd.
 * @q: the request queue.
 */
/*
 * [한국어]
 * bfq_bic_lookup - 현재 실행 중인 태스크(current)의 io_context에서 이
 *                  request_queue(@q)에 대응하는 bfq_io_cq를 찾는다.
 *
 * @q: 조회 대상 request_queue(디스크 큐).
 * @return: 찾은 bfq_io_cq 포인터. current에 io_context가 없거나 해당 큐에
 *          대한 icq가 아직 만들어지지 않았으면 NULL.
 *
 * 이 프로세스가 이 디스크에 이미 한 번이라도 I/O를 낸 적이 있다면, 커널
 * 공통 코드(ioc_lookup_icq)가 만들어 둔 io_cq가 current->io_context에
 * 걸려 있다. 이 함수는 그 icq를 찾아 bfq_io_cq로 변환해 반환한다. 주로
 * request가 아직 할당되지 않아 rq->elv.priv[]로 bic를 얻을 수 없는 시점
 * (예: 태그/깊이 제한 결정 단계)에, 현재 프로세스의 BFQ 컨텍스트를 미리
 * 알아내기 위해 쓰인다. 프로세스 컨텍스트에서 호출되며, current->io_context는
 * 해당 태스크 자신만 수정하므로 별도 락 없이 안전하다.
 *
 * 호출 체인:
 *   bfq_limit_depth → [bfq_bic_lookup] → ioc_lookup_icq → icq_to_bic
 */
static struct bfq_io_cq *bfq_bic_lookup(struct request_queue *q)
{
	/* 현재 태스크가 io_context 자체를 아직 할당받지 않았다면 icq도 존재할 수 없음 */
	if (!current->io_context)
		return NULL; /* 조회 실패 - 호출자(bfq_limit_depth)는 무제한 depth 등 기본 경로로 진행 */

	/* current의 io_context에서 이 request_queue에 연결된 icq를 찾아 bic로 변환해 반환 */
	return icq_to_bic(ioc_lookup_icq(q));
}

/*
 * Scheduler run of queue, if there are requests pending and no one in the
 * driver that will restart queueing.
 */
/*
 * [한국어]
 * bfq_schedule_dispatch - BFQ 가 더 이상 처리할 request 가 있을 때
 * blk-mq 의 hw queue 를 깨워 dispatch 를 재개하도록 예약한다.
 *
 * @bfqd: 디스크 전역 BFQ 스케줄러 상태. bfqd->queue(request_queue)와
 *        bfqd->queued(대기 중인 request 수)를 참조한다.
 * @return: 없음(void).
 *
 * BFQ는 idling/budget 제약 때문에 request가 큐에 남아 있어도 즉시
 * dispatch하지 않는 경우가 있는데, 그 제약이 풀렸을 때(예: request 완료,
 * 병합/삽입 등) blk-mq 계층에게 "지금 dispatch를 다시 시도해도 된다"고
 * 알려야 한다. bfqd->queued가 0이 아니면(아직 처리할 request가 남아
 * 있으면) blk_mq_run_hw_queues()를 호출해 모든 하드웨어 큐(hctx)를 깨워
 * dispatch 루틴(bfq_dispatch_request 등)이 다시 실행되도록 예약한다.
 * 호출 시점에 이미 bfqd->lock을 들고 있어야 하며(lockdep_assert_held로
 * 검증), 이 함수 자체는 락을 새로 걸거나 풀지 않는다.
 *
 * 호출 경로: bfq_completed_request -> bfq_schedule_dispatch ->
 *          blk_mq_run_hw_queues -> blk_mq_run_hw_queue -> nvme_queue_rq
 * NVMe 연결: NVMe 드라이버가 SQ 에 빈 슬롯이 있다고 알릴 때까지
 *            blk_mq_run_hw_queues 가 polling/sleeping hctx 를 깨운다.
 *
 * 호출 체인:
 *   bfq_completed_request/bfq_insert_request/bfq_idle_slice_timer 등
 *   → [bfq_schedule_dispatch] → blk_mq_run_hw_queues
 */
void bfq_schedule_dispatch(struct bfq_data *bfqd)
{
	/* 이 함수는 bfqd 전역 상태(queued)를 읽으므로 호출자가 락을 쥐고 있음을 확인 */
	lockdep_assert_held(&bfqd->lock);

	/* 아직 dispatch를 기다리는 request가 있을 때만 hw queue를 깨울 필요가 있음 */
	if (bfqd->queued != 0) {
		bfq_log(bfqd, "schedule dispatch"); /* 디버그 로그 - dispatch 재예약 사실을 트레이스에 남김 */
		/* async=true: 현재 컨텍스트(인터럽트/락 보유 등)에서 즉시 실행하지 않고
		 * 워크큐 등을 통해 비동기로 hctx들을 재실행 - SQ에 빈 슬롯이 생겼음을 알림 */
		blk_mq_run_hw_queues(bfqd->queue, true); // NVMe hw queue 깨우기: SQ 에 채울 request 가 있음을 알린다.
	}
}

#define bfq_class_idle(bfqq)	((bfqq)->ioprio_class == IOPRIO_CLASS_IDLE)

#define bfq_sample_valid(samples)	((samples) > 80)

/*
 * Lifted from AS - choose which of rq1 and rq2 that is best served now.
 * We choose the request that is closer to the head right now.  Distance
 * behind the head is penalized and only allowed to a certain extent.
 */
/*
 * [한국어]
 * bfq_choose_req - 두 후보 request(rq1, rq2) 중 현재 디스크 헤드 위치(last)
 *                  기준으로 더 먼저 서비스하기 좋은 쪽을 고른다.
 *
 * @bfqd: 디스크 전역 BFQ 상태. bfq_back_max/bfq_back_penalty 튜닝값을 참조.
 * @rq1: 첫 번째 후보 request (NULL 가능).
 * @rq2: 두 번째 후보 request (NULL 가능, rq1과 동일 포인터일 수도 있음).
 * @last: 기준이 되는 현재 디스크 헤드 위치(섹터), 보통 방금 처리한 request의 끝 위치.
 * @return: rq1과 rq2 중 선택된 request. 회전형 디스크의 탐색(seek) 비용을
 *          최소화하는 방향으로 고른다.
 *
 * 원래 CFQ/AS(Anticipatory Scheduler)에서 가져온 로직으로, 동기/메타데이터
 * 우선 규칙을 먼저 적용한 뒤, 남은 경우에는 head 위치(last)로부터의 거리가
 * 가까운 쪽을 고른다. 뒤쪽(backward) 탐색은 앞쪽(forward) 탐색보다 대개
 * 비용이 크므로 bfq_back_penalty로 가중치를 주고, 허용 범위(bfq_back_max)를
 * 넘는 뒤쪽 위치는 "wrap"(디스크 끝을 돌아 앞쪽에서 다시 오는 것과 유사하게
 * 취급)으로 표시해 최후순위로 민다. bfqd->lock을 쥔 스케줄러 콜백 경로에서
 * 호출되며 상태를 변경하지 않는 순수 비교 함수다.
 *
 * 호출 체인:
 *   bfq_find_next_rq → [bfq_choose_req] → (비교만 수행, 하위 호출 없음)
 */
static struct request *bfq_choose_req(struct bfq_data *bfqd,
				      struct request *rq1,
				      struct request *rq2,
				      sector_t last)
{
	sector_t s1, s2, d1 = 0, d2 = 0; /* s1/s2: 각 rq의 시작 섹터, d1/d2: head(last)로부터의 유효 거리 */
	unsigned long back_max; /* 뒤쪽 탐색을 허용하는 최대 거리(섹터, bfq_back_max*2) */
#define BFQ_RQ1_WRAP	0x01 /* request 1 wraps */
#define BFQ_RQ2_WRAP	0x02 /* request 2 wraps */
	/* wrap: rq1/rq2가 각각 "허용 범위를 넘는 뒤쪽" 위치라 wrap 취급되는지 나타내는 비트마스크 */
	unsigned int wrap = 0; /* bit mask: requests behind the disk head? */

	if (!rq1 || rq1 == rq2) /* rq1이 없거나 두 후보가 사실상 같은 request면 */
		return rq2; /* rq2를 그대로 반환(같으면 어느 쪽이든 무방) */
	if (!rq2) /* rq2만 없으면 */
		return rq1; /* 유일하게 유효한 rq1 반환 */

	if (rq_is_sync(rq1) && !rq_is_sync(rq2)) /* rq1만 동기(sync) I/O라면 */
		return rq1; /* 동기 요청을 비동기보다 우선 - 지연시간 민감한 쪽을 먼저 서비스 */
	else if (rq_is_sync(rq2) && !rq_is_sync(rq1)) /* 반대로 rq2만 동기라면 */
		return rq2; /* rq2 우선 */
	if ((rq1->cmd_flags & REQ_META) && !(rq2->cmd_flags & REQ_META)) /* rq1만 메타데이터 I/O라면 */
		return rq1; /* 파일시스템 메타데이터 갱신은 후속 I/O 의존성이 크므로 우선 처리 */
	else if ((rq2->cmd_flags & REQ_META) && !(rq1->cmd_flags & REQ_META)) /* 반대로 rq2만 메타데이터라면 */
		return rq2; /* rq2 우선 */

	s1 = blk_rq_pos(rq1); /* rq1의 시작 LBA(섹터 번호) - 탐색 거리 계산에 사용 */
	s2 = blk_rq_pos(rq2); /* rq2의 시작 LBA */

	/*
	 * By definition, 1KiB is 2 sectors.
	 */
	back_max = bfqd->bfq_back_max * 2; /* 튜닝 가능한 back_max(KiB 단위)를 섹터 단위로 환산 */

	/*
	 * Strict one way elevator _except_ in the case where we allow
	 * short backward seeks which are biased as twice the cost of a
	 * similar forward seek.
	 */
	if (s1 >= last) /* rq1이 head보다 앞쪽(forward)에 있으면 */
		d1 = s1 - last; /* 순수 forward 거리 */
	else if (s1 + back_max >= last) /* 뒤쪽이지만 허용 범위(back_max) 안이면 */
		d1 = (last - s1) * bfqd->bfq_back_penalty; /* 뒤쪽 탐색 비용에 페널티 배율을 곱해 거리로 환산 */
	else /* 허용 범위를 넘는 뒤쪽 위치라면 */
		wrap |= BFQ_RQ1_WRAP; /* rq1을 wrap(최후순위) 취급으로 표시 */

	if (s2 >= last) /* rq2가 head보다 앞쪽이면 */
		d2 = s2 - last; /* 순수 forward 거리 */
	else if (s2 + back_max >= last) /* 뒤쪽이지만 허용 범위 안이면 */
		d2 = (last - s2) * bfqd->bfq_back_penalty; /* 페널티 적용 거리 */
	else /* 허용 범위를 넘는 뒤쪽 위치라면 */
		wrap |= BFQ_RQ2_WRAP; /* rq2를 wrap 취급으로 표시 */

	/* Found required data */

	/*
	 * By doing switch() on the bit mask "wrap" we avoid having to
	 * check two variables for all permutations: --> faster!
	 */
	switch (wrap) { /* 두 개의 불리언(rq1/rq2 wrap 여부) 대신 비트마스크 하나로 분기해 속도를 높임 */
	/* 둘 다 wrap 아님(가장 흔한 경우) - 순수 거리 비교로 결정 */
	case 0: /* common case for CFQ: rq1 and rq2 not wrapped */
		if (d1 < d2) /* rq1이 head에 더 가까우면 */
			return rq1; /* rq1 선택 - 탐색 거리 최소화 */
		else if (d2 < d1) /* rq2가 더 가까우면 */
			return rq2; /* rq2 선택 */

		if (s1 >= s2) /* 거리가 같다면(d1==d2) 섹터 번호가 큰(뒤쪽) 쪽을 우선 */
			return rq1; /* rq1이 더 뒤쪽(더 큰 섹터)이면 rq1 선택 */
		else
			return rq2; /* 아니면 rq2 선택 */

	case BFQ_RQ2_WRAP:
		return rq1; /* rq2만 wrap이면 무조건 정상 범위인 rq1을 우선 */
	case BFQ_RQ1_WRAP:
		return rq2; /* rq1만 wrap이면 무조건 rq2를 우선 */
	case BFQ_RQ1_WRAP|BFQ_RQ2_WRAP: /* both rqs wrapped */
	default: /* 두 요청 모두 wrap인 경우 - 위 두 case를 모두 포함하므로 default는 실질적으로 도달하지 않음 */
		/*
		 * Since both rqs are wrapped,
		 * start with the one that's further behind head
		 * (--> only *one* back seek required),
		 * since back seek takes more time than forward.
		 */
		if (s1 <= s2) /* rq1이 더 head에서 먼(작은 섹터=더 뒤쪽) 위치라면 */
			return rq1; /* 더 먼 쪽을 먼저 처리해 back seek를 한 번만 수행하도록 함 */
		else
			return rq2; /* rq2가 더 먼 위치이면 rq2 우선 */
	}
}

#define BFQ_LIMIT_INLINE_DEPTH 16 /* 스택에 인라인으로 담을 수 있는 cgroup 계층 최대 깊이 - 대부분의 경우 힙 할당을 회피 */

#ifdef CONFIG_BFQ_GROUP_IOSCHED
/*
 * [한국어]
 * bfqq_request_over_limit - bfqq로부터 루트 cgroup까지 이어지는 계층의
 *                            어느 한 레벨이라도, 그 레벨의 가중치(weight)
 *                            몫에 비해 이미 할당된(진행 중인) request 수가
 *                            한도를 넘었는지 검사한다.
 *
 * @bfqd: 디스크 전역 BFQ 상태. entity/서비스 트리 접근을 bfqd->lock으로 보호.
 * @bic: request를 낼 프로세스의 I/O 컨텍스트 - 여기서 대상 bfqq를 찾는다.
 * @opf: 이 request의 연산 플래그(REQ_OP_*) - sync/async 판별에 사용.
 * @act_idx: 대상 액추에이터 인덱스.
 * @limit: 호출자(bfq_limit_depth)가 전달한 "가중치 1당" 기준 한도. 이 함수
 *         내부에서 각 레벨의 entity->weight/wsum 비율로 재조정된다.
 * @return: true면 계층 중 한 레벨이라도 한도를 초과해 depth를 1로 줄여야
 *          함을 의미. false면 아직 여유가 있어 정상 태그 할당이 가능함.
 *
 * cgroup 기반 BFQ에서는 하나의 bfqq가 여러 단계의 상위 entity(cgroup)를
 * 거쳐 B-WF2Q+ 스케줄을 받으므로, 어느 한 cgroup이 자신의 weight 몫 이상
 * 으로 태그(진행 중 request, entity->allocated)를 독점하면 형제 cgroup이
 * 굶주릴 수 있다. 이 함수는 bfqq에서 시작해 for_each_entity로 조상
 * entity들을 배열(entities[])에 모은 뒤, 배열을 역순으로(level-- 즉 루트에
 * 가까운 쪽부터) 순회하면서 각 레벨의 서비스 트리 가중치 합(wsum) 대비
 * entity->weight 비율로 limit을 다시 계산하고, entity->allocated가 그
 * limit 이상이면 초과로 판정해 즉시 중단한다. 계층 깊이가 인라인 배열
 * (BFQ_LIMIT_INLINE_DEPTH)보다 깊으면 락을 풀고 힙에 재할당한 뒤 retry로
 * 되돌아간다. 이 함수 스스로 bfqd->lock을 잡았다 풀며(호출자는 락 없이
 * 호출), 실패 시(entities 할당 실패) false를 반환해 호출자가 보수적으로
 * 동작하도록 한다.
 *
 * 호출 체인:
 *   bfq_limit_depth → [bfqq_request_over_limit] → bfq_entity_service_tree/bfqg_to_blkg
 */
static bool bfqq_request_over_limit(struct bfq_data *bfqd,
				    struct bfq_io_cq *bic, blk_opf_t opf,
				    unsigned int act_idx, int limit)
{
	struct bfq_entity *inline_entities[BFQ_LIMIT_INLINE_DEPTH]; /* 얕은 계층에서 힙 할당 없이 조상 entity를 담는 스택 배열 */
	struct bfq_entity **entities = inline_entities; /* 실제 사용할 배열 포인터 - 깊으면 아래에서 힙 배열로 교체됨 */
	int alloc_depth = BFQ_LIMIT_INLINE_DEPTH; /* entities가 현재 담을 수 있는 최대 개수 */
	struct bfq_sched_data *sched_data; /* bfqq 자신의 스케줄 데이터 - ioprio 클래스별 service_tree[] 배열 보유 */
	struct bfq_entity *entity; /* 순회 중인 entity(bfqq 자신 또는 조상 cgroup) */
	struct bfq_queue *bfqq; /* bic/opf/act_idx로부터 조회한 대상 bfq_queue */
	unsigned long wsum; /* 한 레벨에서의 가중치 합(weighted sum) - limit 재조정의 분모 */
	bool ret = false; /* 최종 반환값 - 기본은 "한도 초과 아님" */
	int depth; /* bfqq의 cgroup 계층 깊이(루트 제외, bfqq 자신 포함) */
	int level; /* 배열 인덱스 겸 현재 순회 중인 계층 레벨 */

retry:
	spin_lock_irq(&bfqd->lock); /* entity/서비스 트리를 스케줄러 상태와 함께 보호하기 위해 irq까지 막고 락 획득 */
	bfqq = bic_to_bfqq(bic, op_is_sync(opf), act_idx); /* 이 프로세스/방향/actuator에 대응하는 bfqq 조회 */
	if (!bfqq) /* 아직 이 방향의 bfqq가 생성되지 않았다면 */
		goto out; /* 검사할 대상이 없으므로 unlock 후 ret(false) 그대로 반환 */

	entity = &bfqq->entity; /* bfqq를 감싸는 스케줄링 엔티티부터 조상 탐색 시작 */
	if (!entity->on_st_or_in_serv) /* bfqq가 서비스 트리에도, in-service 상태에도 없다면(아직 비활성) */
		goto out; /* 굳이 제한할 필요 없음 - BFQ가 활성화하도록 정상적으로 큐잉 허용 */

	/* +1 for bfqq entity, root cgroup not included */
	depth = bfqg_to_blkg(bfqq_group(bfqq))->blkcg->css.cgroup->level + 1; /* bfqq가 속한 blkcg의 cgroup 트리 깊이 + bfqq 자신 1단계 */
	if (depth > alloc_depth) { /* 인라인 스택 배열보다 계층이 더 깊으면 */
		spin_unlock_irq(&bfqd->lock); /* 힙 할당(GFP_NOIO)은 잠들 수 있으므로 먼저 락 해제 */
		if (entities != inline_entities) /* 이전 retry에서 이미 힙 배열로 교체된 적이 있다면 */
			kfree(entities); /* 이전 힙 할당분을 해제(메모리 누수 방지) 후 더 큰 배열로 재할당 예정 */
		entities = kmalloc_objs(*entities, depth, GFP_NOIO); /* depth개의 포인터를 담을 배열을 새로 할당 - GFP_NOIO: I/O 경로 재진입으로 인한 데드락 방지 */
		if (!entities) /* 할당 실패(메모리 부족) 시 */
			return false; /* 보수적으로 "한도 초과 아님" 반환 - 다음 시도에서 다시 판단 */
		alloc_depth = depth; /* 새 배열의 용량을 갱신 */
		goto retry; /* 락을 다시 잡고 처음부터(bfqq 재조회 포함) 다시 시도 - 그 사이 상태가 바뀌었을 수 있음 */
	}

	sched_data = entity->sched_data; /* bfqq 자신의 스케줄 데이터 - 아래에서 ioprio 클래스별 wsum 계산에 사용 */
	/* Gather our ancestors as we need to traverse them in reverse order */
	level = 0; /* 배열 채우기 인덱스 초기화 */
	for_each_entity(entity) { /* entity->parent를 따라 bfqq에서 루트 방향으로 올라가며 순회 */
		/*
		 * If at some level entity is not even active, allow request
		 * queueing so that BFQ knows there's work to do and activate
		 * entities.
		 */
		if (!entity->on_st_or_in_serv) /* 조상 중 하나가 아직 활성화(스케줄 트리 등록)되지 않았다면 */
			goto out; /* 제한하지 않고 큐잉을 허용해 BFQ가 이 entity 계층을 활성화하도록 유도 */
		/* Uh, more parents than cgroup subsystem thinks? */
		if (WARN_ON_ONCE(level >= depth)) /* 계산했던 depth보다 실제 조상 수가 많다면(불일치, 버그 징후) */
			break; /* 배열 오버플로 방지를 위해 순회 중단 */
		entities[level++] = entity; /* 현재 레벨의 entity를 배열에 저장하고 다음 레벨로 */
	}
	WARN_ON_ONCE(level != depth); /* 실제로 모은 레벨 수가 앞서 계산한 depth와 다르면 cgroup 계층 가정이 깨진 것 - 디버그 경고 */
	for (level--; level >= 0; level--) { /* 배열의 마지막(루트에 가장 가까운 레벨)부터 역순으로 - bfqq 자신은 마지막에 검사 */
		entity = entities[level]; /* 이번에 검사할 레벨의 entity */
		if (level > 0) { /* bfqq 자신이 아니라 중간 cgroup entity라면 */
			wsum = bfq_entity_service_tree(entity)->wsum; /* 그 entity가 속한 서비스 트리의 전체 가중치 합 */
		} else { /* level == 0, 즉 bfqq 자신을 검사하는 마지막 단계 */
			int i; /* ioprio 클래스 순회 인덱스 */
			/*
			 * For bfqq itself we take into account service trees
			 * of all higher priority classes and multiply their
			 * weights so that low prio queue from higher class
			 * gets more requests than high prio queue from lower
			 * class.
			 */
			wsum = 0; /* 클래스별 가중치를 누적할 초기값 */
			for (i = 0; i <= bfqq->ioprio_class - 1; i++) { /* bfqq보다 우선순위가 높은(수치가 작은) 클래스들까지 포함 */
				wsum = wsum * IOPRIO_BE_NR + /* 상위 클래스의 영향력을 크게 반영하기 위해 진법처럼 자리올림(스케일 업) */
					sched_data->service_tree[i].wsum; /* 해당 클래스 서비스 트리의 가중치 합을 더함 */
			}
		}
		if (!wsum) /* 이 레벨에 활성 가중치가 전혀 없다면(0으로 나누기 방지) */
			continue; /* 이 레벨은 판정 불가 - 다음(더 상위 또는 bfqq 자신) 레벨로 */
		limit = DIV_ROUND_CLOSEST(limit * entity->weight, wsum); /* limit을 "전체 대비 이 entity의 weight 비율"만큼 축소/재조정(반올림) */
		if (entity->allocated >= limit) { /* 이 entity가 이미 재조정된 한도 이상으로 request를 점유했다면 */
			/* [한국어] 디버그 로그 - 어느 레벨에서 얼마나 초과했는지 기록 */
			bfq_log_bfqq(bfqq->bfqd, bfqq,
				"too many requests: allocated %d limit %d level %d",
				entity->allocated, limit, level);
			ret = true; /* 한도 초과 확정 */
			break; /* 더 볼 필요 없이 즉시 루프 종료 */
		}
	}
out:
	spin_unlock_irq(&bfqd->lock); /* retry 재진입/조기 goto 모든 경로가 이 라벨로 모여 락을 해제 */
	if (entities != inline_entities) /* 힙에 할당한 배열을 사용했다면 */
		kfree(entities); /* 스택 배열이 아니므로 반드시 해제해 메모리 누수 방지 */
	return ret; /* true=depth를 1로 제한해야 함, false=정상 depth 유지 */
}
#else
/*
 * [한국어]
 * bfqq_request_over_limit - CONFIG_BFQ_GROUP_IOSCHED(cgroup 지원)가 꺼져
 *                            있을 때 사용되는 대체(stub) 구현.
 *
 * @bfqd: 사용되지 않음(cgroup 계층이 없으므로 검사할 대상도 없음).
 * @bic: 사용되지 않음.
 * @opf: 사용되지 않음.
 * @act_idx: 사용되지 않음.
 * @limit: 사용되지 않음.
 * @return: 항상 false.
 *
 * cgroup 계층이 존재하지 않으면 "cgroup이 자기 몫 이상으로 태그를 독점"
 * 하는 시나리오 자체가 성립하지 않으므로, 이 빌드 옵션에서는 항상 한도
 * 초과가 아니라고 답해 호출자(bfq_limit_depth)가 depth 제한 없이 정상
 * 동작하도록 한다. 컴파일러가 미사용 인자 경고를 내지 않도록 함수 시그니처는
 * 위 CONFIG_BFQ_GROUP_IOSCHED 버전과 동일하게 유지된다.
 *
 * 호출 체인:
 *   bfq_limit_depth → [bfqq_request_over_limit(stub)] → (하위 호출 없음)
 */
static bool bfqq_request_over_limit(struct bfq_data *bfqd,
				    struct bfq_io_cq *bic, blk_opf_t opf,
				    unsigned int act_idx, int limit)
{
	return false; /* cgroup 계층이 없어 검사 대상이 없으므로 항상 "한도 초과 아님" */
}
#endif

/*
 * Async I/O can easily starve sync I/O (both sync reads and sync
 * writes), by consuming all tags. Similarly, storms of sync writes,
 * such as those that sync(2) may trigger, can starve sync reads.
 * Limit depths of async I/O and sync writes so as to counter both
 * problems.
 *
 * Also if a bfq queue or its parent cgroup consume more tags than would be
 * appropriate for their weight, we trim the available tag depth to 1. This
 * avoids a situation where one cgroup can starve another cgroup from tags and
 * thus block service differentiation among cgroups. Note that because the
 * queue / cgroup already has many requests allocated and queued, this does not
 * significantly affect service guarantees coming from the BFQ scheduling
 * algorithm.
 */
/*
 * bfq_limit_depth: request 할당 단계에서 sync/async 및 cgroup 가중치에
 * 따라 blk_mq_alloc_data->shallow_depth 를 제한한다.
 * 호출 경로: blk_mq_get_request -> blk_mq_sched_get_request ->
 *          elevator_ops.limit_depth(bfq_limit_depth)
 * NVMe 연결: NVMe controller 의 SQ/tag 풀 소진을 막기 위해 읽기/쓰기/
 *           cgroup 별로 tag 깊이를 조절한다. 이 제한이 없으면 특정
 *           cgroup 이 모든 CID/tag 를 독점할 수 있다.
 *
 * [한국어]
 * @opf: 할당하려는 request의 연산 플래그(REQ_OP_* | REQ_* 조합) - 동기/비동기,
 *       읽기/쓰기 판별에 사용된다.
 * @data: blk-mq 할당 컨텍스트. data->q(request_queue)와 결과를 담을
 *        data->shallow_depth 필드를 갖는다.
 * @return: 없음(void). 결과는 data->shallow_depth에 기록되어 blk_mq_get_tag가
 *          실제 태그 비트맵에서 탐색할 깊이를 제한하는 데 쓰인다.
 *
 * 이 함수는 elevator_mq_ops.limit_depth 콜백으로 등록되어, blk-mq가 태그
 * (하드웨어 큐 슬롯, NVMe 관점에서는 CID)를 할당하기 직전에 호출된다.
 * 먼저 동기 읽기(sync read)는 항상 전체 depth(data->q->nr_requests)를
 * 허용하고, 그 외(비동기 또는 동기 쓰기)는 bfqd->async_depths 표에서
 * "현재 weight-raised busy queue가 있는지"와 "sync 여부"에 따라 미리
 * 계산해 둔 기본 한도를 가져온다. 그 다음 이 프로세스가 이미 생성한
 * bfqq들(액추에이터별로 최대 bfqd->num_actuators개)을 순회하며,
 * bfqq_request_over_limit()이 "어느 cgroup이 자기 몫 이상으로 태그를
 * 점유 중"이라고 판정하면 즉시 limit을 1로 강제 축소해 그 프로세스가
 * 더 이상 태그를 쉽게 얻지 못하도록 한다. 프로세스 컨텍스트에서 호출되며
 * bfq_bic_lookup/bic_to_bfqq는 락 없이 호출되지만, bfqq_request_over_limit
 * 내부에서 필요한 순간에만 bfqd->lock을 잡는다.
 *
 * 호출 체인:
 *   blk_mq_get_request → blk_mq_sched_get_request → elevator_ops.limit_depth
 *   → [bfq_limit_depth] → bfq_bic_lookup/bic_to_bfqq/bfqq_request_over_limit
 */
static void bfq_limit_depth(blk_opf_t opf, struct blk_mq_alloc_data *data)
{
	struct bfq_data *bfqd = data->q->elevator->elevator_data; /* 이 request_queue의 BFQ 전역 상태 */
	struct bfq_io_cq *bic = bfq_bic_lookup(data->q); /* 현재 프로세스가 이 큐에 대해 가진 I/O 컨텍스트(없으면 NULL) */
	unsigned int limit, act_idx; /* limit: 최종 결정될 depth 한도, act_idx: 액추에이터 순회 인덱스 */

	/* Sync reads have full depth available */
	if (blk_mq_is_sync_read(opf)) /* 동기 읽기는 지연시간이 가장 민감하므로 */
		limit = data->q->nr_requests; /* 제한 없이 큐의 전체 depth를 그대로 허용 */
	else /* 비동기 I/O 또는 동기 쓰기라면 */
		limit = bfqd->async_depths[!!bfqd->wr_busy_queues][op_is_sync(opf)]; /* weight-raised 큐 존재 여부 x sync 여부로 미리 계산된 기본 한도 테이블에서 조회 */

	for (act_idx = 0; bic && act_idx < bfqd->num_actuators; act_idx++) { /* bic이 있을 때만(신규 프로세스면 검사할 bfqq 자체가 없음) 각 액추에이터를 순회 */
		/* Fast path to check if bfqq is already allocated. */
		if (!bic_to_bfqq(bic, op_is_sync(opf), act_idx)) /* 이 방향/actuator의 bfqq가 아직 없다면(과도한 사용 이력이 있을 수 없음) */
			continue; /* 검사 없이 다음 actuator로 - 헛되이 락을 잡지 않기 위한 빠른 경로 */

		/*
		 * Does queue (or any parent entity) exceed number of
		 * requests that should be available to it? Heavily
		 * limit depth so that it cannot consume more
		 * available requests and thus starve other entities.
		 */
		if (bfqq_request_over_limit(bfqd, bic, opf, act_idx, limit)) { /* 이 bfqq 또는 상위 cgroup 중 하나라도 가중치 몫을 초과했다면 */
			limit = 1; /* 사실상 "한 번에 하나씩만" 허용해 더 이상의 독점을 강하게 억제 */
			break; /* 한 actuator에서라도 초과가 확인되면 나머지는 볼 필요 없이 종료 */
		}
	}

	/* [한국어] 디버그 로그 - 최종 결정된 depth와 그 근거가 된 상태값 기록 */
	bfq_log(bfqd, "[%s] wr_busy %d sync %d depth %u",
		__func__, bfqd->wr_busy_queues, op_is_sync(opf), limit);

	if (limit < data->q->nr_requests) /* 계산된 limit이 큐의 기본 depth보다 작을 때만 */
		data->shallow_depth = limit; // blk-mq의 sbitmap 태그 풀에서 이 할당 요청이 쓸 수 있는 최대 깊이 - 장치의 큐가 아니라 request_queue의 nr_requests를 나누는 값이다.
}

/*
 * [한국어]
 * bfq_rq_pos_tree_lookup - LBA(섹터) 위치 기준으로 정렬된 rb-tree(position
 *                          tree)에서 주어진 sector와 가장 가까운 bfqq를
 *                          찾고, 삽입 지점(parent/rb_link)도 함께 계산한다.
 *
 * @bfqd: 디스크 전역 BFQ 상태 - 디버그 로그(bfq_log)에만 사용.
 * @root: 탐색할 rb-tree의 루트. 보통 bfqq가 속한 cgroup의
 *        bfq_group->rq_pos_tree(같은 cgroup 내 bfqq들을 next_rq의 시작
 *        섹터로 정렬한 트리)이다.
 * @sector: 찾고자 하는 기준 섹터(보통 새 request의 시작 LBA 또는 어떤
 *          bfqq의 next_rq 위치).
 * @ret_parent: [출력] 트리 탐색이 끝난 지점의 부모 노드 - 정확히 일치하는
 *              노드가 없을 때 rb_link_node()로 삽입할 위치를 구성하는 데 쓰인다.
 * @rb_link: [출력, NULL 허용] 삽입 지점을 가리키는 이중 포인터(&parent->rb_left
 *           또는 &parent->rb_right). 순수 조회만 필요하면 NULL을 넘겨도 된다.
 * @return: sector와 정확히 같은 next_rq 위치를 가진 bfqq가 있으면 그 bfqq,
 *          없으면 NULL(이 경우에도 *ret_parent/*rb_link는 삽입 지점으로 유효).
 *
 * BFQ는 협력 프로세스(cooperating processes, 예: 같은 파일을 순차적으로
 * 나눠 읽는 여러 스레드)를 찾아 하나의 bfqq로 병합(merge)하기 위해, 같은
 * cgroup 안의 bfqq들을 next_rq의 LBA로 정렬한 rb-tree(position tree)를
 * 유지한다. 이 함수는 표준 rb-tree 이진 탐색으로 sector 위치를 찾아가며,
 * 정확히 일치하는 노드를 발견하면 그 자리에서 멈추고(break) 해당 bfqq를
 * 반환한다. 일치하는 노드가 끝내 없으면 bfqq는 NULL로 리셋되어 반환되고,
 * 대신 *ret_parent/*rb_link에 새 노드를 삽입할 위치가 남는다. 호출자가
 * bfqd->lock을 쥔 상태에서 호출하는 것이 원칙이며(트리 구조체를 공유
 * 상태로 다루므로), 이 함수 자체는 트리를 변경하지 않는 순수 탐색이다.
 *
 * 호출 체인:
 *   bfq_pos_tree_add_move/bfq_find_close_cooperator 등
 *   → [bfq_rq_pos_tree_lookup] → rb_entry/blk_rq_pos (하위 호출 없음, 순수 탐색)
 */
static struct bfq_queue *
bfq_rq_pos_tree_lookup(struct bfq_data *bfqd, struct rb_root *root,
		     sector_t sector, struct rb_node **ret_parent,
		     struct rb_node ***rb_link)
{
	struct rb_node **p, *parent; /* p: 현재 탐색 위치를 가리키는 이중 포인터, parent: 마지막으로 지나온 노드 */
	struct bfq_queue *bfqq = NULL; /* 탐색 결과 - 정확히 일치하는 bfqq를 찾으면 갱신됨 */

	parent = NULL; /* 트리가 비어 있을 수도 있으므로 초기값은 NULL */
	p = &root->rb_node; /* 루트의 최상위 노드 포인터에서 탐색 시작 */
	while (*p) { /* 현재 위치에 노드가 있는 동안(리프에 도달하기 전까지) 계속 내려감 */
		struct rb_node **n; /* 다음으로 내려갈 방향(좌/우) 포인터 */

		parent = *p; /* 이번 반복에서 검사할 노드를 parent로 기록 - 못 찾으면 삽입 지점의 부모가 됨 */
		bfqq = rb_entry(parent, struct bfq_queue, pos_node); /* rb_node 포인터에서 이를 감싸는 bfq_queue 구조체 주소를 역산 */

		/*
		 * Sort strictly based on sector. Smallest to the left,
		 * largest to the right.
		 */
		if (sector > blk_rq_pos(bfqq->next_rq)) /* 찾는 섹터가 이 노드의 bfqq보다 뒤쪽(더 큰 LBA)이면 */
			n = &(*p)->rb_right; /* 오른쪽 서브트리로 이동 - 정렬 규칙(작은 값 왼쪽, 큰 값 오른쪽) */
		else if (sector < blk_rq_pos(bfqq->next_rq)) /* 찾는 섹터가 더 앞쪽(더 작은 LBA)이면 */
			n = &(*p)->rb_left; /* 왼쪽 서브트리로 이동 */
		else /* 정확히 같은 섹터를 가진 bfqq를 발견 */
			break; /* 더 내려가지 않고 즉시 종료 - bfqq에 결과가 담긴 채로 루프 탈출 */
		p = n; /* 다음 레벨로 이동 */
		bfqq = NULL; /* 아직 정확히 일치하는 노드를 못 찾았으므로 결과를 다시 NULL로 리셋 */
	}

	*ret_parent = parent; /* 탐색이 멈춘 지점의 부모를 호출자에게 반환 - 삽입 시 필요 */
	if (rb_link) /* 호출자가 삽입 지점 정보를 요청했다면(NULL이 아니면) */
		*rb_link = p; /* 새 노드를 연결할 자리(부모의 왼쪽/오른쪽 포인터 주소)를 반환 */

	bfq_log(bfqd, "rq_pos_tree_lookup %llu: returning %d",
		(unsigned long long)sector,
		bfqq ? bfqq->pid : 0); /* 디버그 로그 - 어떤 섹터를 찾았고 어떤 pid의 bfqq가 매칭됐는지 기록 */

	return bfqq; /* 정확히 일치하는 bfqq(있으면) 또는 NULL(없으면 - 삽입 지점만 유효) */
}

/*
 * [한국어]
 * bfq_too_late_for_merging - bfqq가 협력 큐 병합(cooperator merge) 후보로
 *                             고려되기에는 이미 시간이 너무 지났는지 판단한다.
 *
 * @bfqq: 판단 대상 bfq_queue.
 * @return: true면 이 bfqq는 더 이상 병합 후보가 될 수 없음(너무 늦음).
 *          false면 아직 병합 후보로 고려 가능.
 *
 * BFQ의 큐 병합(협력 프로세스 탐지)은 프로세스들이 "비슷한 시점에 유사한
 * 위치의 I/O를 낼 때"만 유효하다. bfqq가 backlogged(밀린 요청이 있는)
 * 상태에서 이미 상당한 서비스(service_from_backlogged > 0)를 받았고,
 * 그 큐의 첫 I/O 시각(first_IO_time)으로부터 bfq_merge_time_limit(병합
 * 허용 시간 한도)이 지났다면, 이제 와서 병합해도 실질적인 이득(탐색 시간
 * 절감, 공정성 개선)이 거의 없다고 보고 병합 후보에서 제외한다. 순수
 * 판정 함수로 상태를 변경하지 않으며, 호출자의 락 보유 여부를 가리지 않는다.
 *
 * 호출 체인:
 *   bfq_pos_tree_add_move/bfq_setup_cooperator 등
 *   → [bfq_too_late_for_merging] → time_is_before_jiffies
 */
static bool bfq_too_late_for_merging(struct bfq_queue *bfqq)
{
	return bfqq->service_from_backlogged > 0 && /* 이미 backlogged 상태에서 서비스를 받은 적이 있고 */
		time_is_before_jiffies(bfqq->first_IO_time + /* 그 큐의 첫 I/O 이후 */
				       bfq_merge_time_limit); /* 병합 허용 시간 한도가 지났다면 true(너무 늦음) */
}

/*
 * The following function is not marked as __cold because it is
 * actually cold, but for the same performance goal described in the
 * comments on the likely() at the beginning of
 * bfq_setup_cooperator(). Unexpectedly, to reach an even lower
 * execution time for the case where this function is not invoked, we
 * had to add an unlikely() in each involved if().
 */
/*
 * [한국어]
 * bfq_pos_tree_add_move - bfqq를 소속 cgroup의 position tree(LBA 정렬
 *                         rb-tree)에서 제거한 뒤, 여전히 병합 후보 자격이
 *                         있으면 (갱신된 next_rq 위치 기준으로) 다시 삽입한다.
 *
 * @bfqd: 디스크 전역 BFQ 상태.
 * @bfqq: 재배치 대상 bfq_queue. next_rq(다음 dispatch 후보 request)가
 *        바뀌었을 때(새 request 삽입, 기존 request 완료 등) 호출된다.
 * @return: 없음(void).
 *
 * BFQ는 협력 프로세스 탐지를 위해 각 cgroup마다 활성 bfqq들을 next_rq의
 * LBA로 정렬한 rb-tree(position tree)를 유지한다. bfqq의 next_rq가
 * 바뀌면 트리에서의 정렬 위치도 바뀌어야 하므로, 이 함수는 먼저 기존
 * 위치에서 무조건 제거(rb_erase)한 뒤, 여러 조건(oom 큐 여부, 병합
 * 타임아웃 경과 여부, idle 클래스 여부, next_rq 존재 여부)을 검사해
 * 병합 후보 자격이 없으면 그대로 트리 밖에 남겨둔다. 자격이 있으면
 * bfq_rq_pos_tree_lookup()으로 새 삽입 위치를 찾아 다시 삽입하는데,
 * 이때 정확히 같은 LBA를 next_rq로 갖는 다른 bfqq(__bfqq)가 이미 있다면
 * 트리에 중복 삽입하지 않고 pos_root를 NULL로 남겨(트리 밖 상태) 둔다.
 * __cold로 표시되어 컴파일러가 이 함수를 실행 빈도가 낮은 코드로 취급해
 * 핫 경로(호출자인 bfq_add_request 등)를 최적화하도록 유도한다. 호출자가
 * bfqd->lock을 쥔 상태에서 호출해야 한다(공유 rb-tree를 직접 조작하므로).
 *
 * 호출 체인:
 *   bfq_add_request/bfq_remove_request 등
 *   → [bfq_pos_tree_add_move] → bfq_rq_pos_tree_lookup/rb_erase/rb_insert_color
 */
void __cold
bfq_pos_tree_add_move(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	struct rb_node **p, *parent; /* 새로 삽입할 위치를 가리키는 이중 포인터와 그 부모 노드 */
	struct bfq_queue *__bfqq; /* 동일 LBA(next_rq 위치)를 가진 기존 bfqq가 있는지 담을 변수 */

	if (bfqq->pos_root) { /* 이미 어떤 position tree에 속해 있다면(이전 next_rq 기준 위치) */
		rb_erase(&bfqq->pos_node, bfqq->pos_root); /* 옛 위치에서 노드를 제거 - 재삽입 전 필수 단계 */
		bfqq->pos_root = NULL; /* 트리에서 빠졌음을 표시(임시 상태) */
	}

	/* oom_bfqq does not participate in queue merging */
	if (bfqq == &bfqd->oom_bfqq) /* 메모리 부족 시 폴백으로 쓰는 공용 oom 큐라면 */
		return; /* 여러 프로세스가 공유하는 특수 큐이므로 병합 대상에서 애초에 제외 */

	/*
	 * bfqq cannot be merged any longer (see comments in
	 * bfq_setup_cooperator): no point in adding bfqq into the
	 * position tree.
	 */
	if (bfq_too_late_for_merging(bfqq)) /* 이미 병합하기엔 너무 늦은 큐라면(위 함수 참고) */
		return; /* 트리에 넣어봐야 무의미하므로 트리 밖 상태(pos_root==NULL)로 남겨둠 */

	if (bfq_class_idle(bfqq)) /* IOPRIO_CLASS_IDLE(최저 우선순위) 큐라면 */
		return; /* idle 큐는 협력 탐지 대상으로 삼지 않음 */
	if (!bfqq->next_rq) /* 현재 dispatch할 다음 request가 없다면(비어 있음) */
		return; /* 정렬 기준이 될 위치 자체가 없으므로 트리에 넣을 수 없음 */

	bfqq->pos_root = &bfqq_group(bfqq)->rq_pos_tree; /* 이 bfqq가 속한 cgroup의 position tree를 대상으로 지정 */
	/* [한국어] next_rq의 시작 섹터로 삽입 위치를 탐색 - 동일 위치의 기존 bfqq도 함께 찾음 */
	__bfqq = bfq_rq_pos_tree_lookup(bfqd, bfqq->pos_root,
			blk_rq_pos(bfqq->next_rq), &parent, &p);
	if (!__bfqq) { /* 동일한 LBA를 가진 다른 bfqq가 없다면(트리에 새로 넣을 수 있음) */
		rb_link_node(&bfqq->pos_node, parent, p); /* 탐색으로 찾은 parent/p 위치에 새 노드를 연결 */
		rb_insert_color(&bfqq->pos_node, bfqq->pos_root); /* rb-tree 균형(적흑 규칙)을 맞추며 실제로 삽입 완료 */
	} else /* 이미 동일 위치에 다른 bfqq가 존재한다면(중복 키) */
		bfqq->pos_root = NULL; /* 삽입을 포기하고 트리 밖 상태로 남김 - 두 bfqq를 구분할 필요가 없을 만큼 위치가 같음 */
}

/*
 * The following function returns false either if every active queue
 * must receive the same share of the throughput (symmetric scenario),
 * or, as a special case, if bfqq must receive a share of the
 * throughput lower than or equal to the share that every other active
 * queue must receive.  If bfqq does sync I/O, then these are the only
 * two cases where bfqq happens to be guaranteed its share of the
 * throughput even if I/O dispatching is not plugged when bfqq remains
 * temporarily empty (for more details, see the comments in the
 * function bfq_better_to_idle()). For this reason, the return value
 * of this function is used to check whether I/O-dispatch plugging can
 * be avoided.
 *
 * The above first case (symmetric scenario) occurs when:
 * 1) all active queues have the same weight,
 * 2) all active queues belong to the same I/O-priority class,
 * 3) all active groups at the same level in the groups tree have the same
 *    weight,
 * 4) all active groups at the same level in the groups tree have the same
 *    number of children.
 *
 * Unfortunately, keeping the necessary state for evaluating exactly
 * the last two symmetry sub-conditions above would be quite complex
 * and time consuming. Therefore this function evaluates, instead,
 * only the following stronger three sub-conditions, for which it is
 * much easier to maintain the needed state:
 * 1) all active queues have the same weight,
 * 2) all active queues belong to the same I/O-priority class,
 * 3) there is at most one active group.
 * In particular, the last condition is always true if hierarchical
 * support or the cgroups interface are not enabled, thus no state
 * needs to be maintained in this case.
 */
/*
 * [한국어]
 * bfq_asymmetric_scenario - 현재 디스크의 활성 큐 구성이 "비대칭
 *                           (asymmetric)" 시나리오인지 판정한다.
 *
 * @bfqd: 디스크 전역 BFQ 상태. queue_weights_tree(활성 큐들의 가중치
 *        분포)와 busy_queues[](클래스별 활성 큐 수)를 참조한다.
 * @bfqq: 판정 기준이 되는 bfq_queue(자신이 최소 가중치를 갖는지 확인용).
 *        NULL을 넘기면 smallest_weight 판단이 자동으로 false 처리된다.
 * @return: true면 비대칭 시나리오(각 큐가 받아야 할 처리량 몫이 서로
 *          다를 수 있음) - 이 경우 device idling 등으로 공정성을 강제로
 *          보장해야 할 수 있다. false면 대칭 시나리오(idling 없이도
 *          자연히 공정한 분배가 이뤄짐) 또는 bfqq가 최소 가중치를 가져
 *          idling이 필요 없는 특수 케이스.
 *
 * BFQ는 모든 활성 큐가 동일한 처리량 몫을 받아야 하는 "대칭적" 상황
 *에서는 dispatch plugging(디바이스 idling)을 생략해도 공정성이 자연히
 * 보장된다는 사실을 이용해 성능을 높인다. 이 함수는 그 반대인 "비대칭"
 * 조건 중 유지 비용이 낮은 세 가지만 근사적으로 검사한다: (1) 활성 큐들의
 * 가중치가 서로 다른가(varied_queue_weights, 트리에 노드가 2개 이상이고
 * bfqq가 그중 최솟값이 아닌 경우), (2) 서로 다른 I/O 우선순위 클래스의
 * 큐들이 동시에 활성인가(multiple_classes_busy), (3, cgroup 빌드에서만)
 * 활성 요청을 가진 cgroup이 2개 이상인가. 원래 정의(가중치/클래스/그룹
 * 가중치/그룹 자식 수까지 4가지 정확한 조건)보다 단순화된 근사치이며,
 * 이 단순화 덕분에 상태 유지 비용이 낮다는 점이 주석에 설명되어 있다.
 * 상태를 변경하지 않는 순수 판정 함수로, 스케줄러 콜백(주로
 * bfq_better_to_idle 경로) 안에서 bfqd->lock을 쥔 채 호출된다.
 *
 * 호출 체인:
 *   bfq_better_to_idle/bfq_serv_to_charge 등
 *   → [bfq_asymmetric_scenario] → (rb-tree 조회만 수행, 하위 호출 없음)
 */
static bool bfq_asymmetric_scenario(struct bfq_data *bfqd,
				   struct bfq_queue *bfqq)
{
	bool smallest_weight = bfqq && /* bfqq가 존재하고 */
		bfqq->weight_counter && /* 가중치 카운터 트리에 자신의 카운터가 연결되어 있으며 */
		bfqq->weight_counter ==
		container_of(
			rb_first_cached(&bfqd->queue_weights_tree), /* 트리에서 가장 작은(왼쪽 끝) 가중치 노드 */
			struct bfq_weight_counter,
			weights_node); /* 그 최솟값 노드가 바로 bfqq 자신의 카운터인지 비교 - 맞으면 bfqq가 최저 가중치 소유 */

	/*
	 * For queue weights to differ, queue_weights_tree must contain
	 * at least two nodes.
	 */
	bool varied_queue_weights = !smallest_weight && /* bfqq가 최소 가중치가 아니면서(비교할 다른 값이 있다는 뜻) */
		!RB_EMPTY_ROOT(&bfqd->queue_weights_tree.rb_root) && /* 트리가 비어있지 않고 */
		(bfqd->queue_weights_tree.rb_root.rb_node->rb_left || /* 루트가 왼쪽 자식을 갖거나 */
		 bfqd->queue_weights_tree.rb_root.rb_node->rb_right); /* 오른쪽 자식을 가지면 - 즉 노드가 2개 이상 존재하면 가중치가 다양하다고 판단 */

	bool multiple_classes_busy = /* 서로 다른 두 IO 우선순위 클래스(RT/BE/IDLE)가 동시에 활성 request를 갖는지 */
		(bfqd->busy_queues[0] && bfqd->busy_queues[1]) || /* 클래스0(RT)과 클래스1(BE) 둘 다 활성 */
		(bfqd->busy_queues[0] && bfqd->busy_queues[2]) || /* 클래스0(RT)과 클래스2(IDLE) 둘 다 활성 */
		(bfqd->busy_queues[1] && bfqd->busy_queues[2]); /* 클래스1(BE)과 클래스2(IDLE) 둘 다 활성 */

	return varied_queue_weights || multiple_classes_busy /* 가중치가 다르거나 클래스가 섞여 있으면 이미 비대칭 */
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	       || bfqd->num_groups_with_pending_reqs > 1 /* cgroup 지원 빌드에서는 활성 요청을 가진 그룹이 2개 이상이어도 비대칭으로 간주 */
#endif
		;
}

/*
 * If the weight-counter tree passed as input contains no counter for
 * the weight of the input queue, then add that counter; otherwise just
 * increment the existing counter.
 *
 * Note that weight-counter trees contain few nodes in mostly symmetric
 * scenarios. For example, if all queues have the same weight, then the
 * weight-counter tree for the queues may contain at most one node.
 * This holds even if low_latency is on, because weight-raised queues
 * are not inserted in the tree.
 * In most scenarios, the rate at which nodes are created/destroyed
 * should be low too.
 */
/*
 * [한국어]
 * bfq_weights_tree_add - bfqq의 가중치(entity->weight)에 대응하는 카운터
 *                        노드를 디스크 전역 queue_weights_tree에 추가하거나,
 *                        이미 있으면 참조 카운트(num_active)만 증가시킨다.
 *
 * @bfqq: 활성화(backlogged)되는 bfq_queue. 이 큐의 현재 가중치를 트리에
 *        반영한다.
 * @return: 없음(void).
 *
 * queue_weights_tree는 "현재 활성 큐들의 가중치 분포"를 근사하기 위한
 * 자료구조로, bfq_asymmetric_scenario()가 "모든 큐의 가중치가 같은지"를
 * 빠르게 판단하는 데 쓰인다. 같은 weight 값을 가진 큐가 여러 개 있어도
 * 트리에는 weight 값 하나당 하나의 bfq_weight_counter 노드만 존재하며,
 * num_active로 그 weight를 공유하는 큐의 수를 센다. 이 함수는 bfqq의
 * weight로 rb-tree를 이진 탐색해, 같은 weight의 카운터를 찾으면
 * inc_counter로 점프해 참조만 늘리고, 없으면 새 bfq_weight_counter를
 * GFP_ATOMIC(스케줄러 락 보유 중이라 잠들 수 없음)으로 할당해 트리에
 * 삽입한다. bfqq->weight_counter가 이미 설정돼 있으면(주석에 설명된
 * 이중 호출 경합 상황) 아무 것도 하지 않고 조기 반환한다. 할당 실패
 * 시에는 이 bfqq의 weight가 비대칭 시나리오 판정에서 누락되지만, 트리에
 * 없는 채로도 bfq_weights_tree_remove가 안전하게 아무 일도 하지 않으므로
 * 일관성은 유지된다. bfqd->lock을 쥔 스케줄러 콜백(주로 bfqq가 backlogged로
 * 전이할 때) 안에서 호출된다.
 *
 * 호출 체인:
 *   bfq_activate_bfqq/__bfq_activate_entity 등
 *   → [bfq_weights_tree_add] → kzalloc_obj/rb_insert_color_cached
 */
void bfq_weights_tree_add(struct bfq_queue *bfqq)
{
	struct rb_root_cached *root = &bfqq->bfqd->queue_weights_tree; /* 디스크 전역 가중치 분포 트리(왼쪽 끝 캐시 포함) */
	struct bfq_entity *entity = &bfqq->entity; /* bfqq를 감싸는 스케줄링 엔티티 - weight 필드 보유 */
	struct rb_node **new = &(root->rb_root.rb_node), *parent = NULL; /* 삽입 위치 탐색용 이중 포인터와 부모 */
	bool leftmost = true; /* 지금까지 탐색 경로가 항상 왼쪽이었는지 - rb_root_cached의 캐시 갱신에 필요 */

	/*
	 * Do not insert if the queue is already associated with a
	 * counter, which happens if:
	 *   1) a request arrival has caused the queue to become both
	 *      non-weight-raised, and hence change its weight, and
	 *      backlogged; in this respect, each of the two events
	 *      causes an invocation of this function,
	 *   2) this is the invocation of this function caused by the
	 *      second event. This second invocation is actually useless,
	 *      and we handle this fact by exiting immediately. More
	 *      efficient or clearer solutions might possibly be adopted.
	 */
	if (bfqq->weight_counter) /* 이미 이 bfqq가 어떤 카운터에 연결되어 있다면(중복 호출) */
		return; /* 재등록할 필요 없이 즉시 반환 */

	while (*new) { /* 리프에 도달할 때까지 이진 탐색 계속 */
		/* [한국어] 현재 노드를 감싸는 bfq_weight_counter 획득 */
		struct bfq_weight_counter *__counter = container_of(*new,
						struct bfq_weight_counter,
						weights_node);
		parent = *new; /* 못 찾으면 삽입 지점의 부모가 될 후보로 기록 */

		if (entity->weight == __counter->weight) { /* 이미 같은 weight의 카운터가 존재하면 */
			bfqq->weight_counter = __counter; /* bfqq를 그 카운터에 연결 */
			goto inc_counter; /* 새로 만들 필요 없이 바로 참조 카운트 증가 단계로 이동 */
		}
		if (entity->weight < __counter->weight) /* 찾는 weight가 더 작으면 */
			new = &((*new)->rb_left); /* 왼쪽 서브트리로 - 정렬 규칙(작은 값이 왼쪽) */
		else { /* 찾는 weight가 더 크면 */
			new = &((*new)->rb_right); /* 오른쪽 서브트리로 이동 */
			leftmost = false; /* 오른쪽으로 한 번이라도 이동했다면 더 이상 leftmost 경로가 아님 */
		}
	}

	/* [한국어] 같은 weight의 카운터가 없으므로 새로 0-초기화 할당 - GFP_ATOMIC: 락 보유 중이라 슬립 불가 */
	bfqq->weight_counter = kzalloc_obj(struct bfq_weight_counter,
					   GFP_ATOMIC);

	/*
	 * In the unlucky event of an allocation failure, we just
	 * exit. This will cause the weight of queue to not be
	 * considered in bfq_asymmetric_scenario, which, in its turn,
	 * causes the scenario to be deemed wrongly symmetric in case
	 * bfqq's weight would have been the only weight making the
	 * scenario asymmetric.  On the bright side, no unbalance will
	 * however occur when bfqq becomes inactive again (the
	 * invocation of this function is triggered by an activation
	 * of queue).  In fact, bfq_weights_tree_remove does nothing
	 * if !bfqq->weight_counter.
	 */
	if (unlikely(!bfqq->weight_counter)) /* 메모리 부족으로 할당 실패 시 */
		return; /* 이 bfqq의 weight는 트리에 반영되지 않은 채로 조용히 포기(위 주석 설명대로 일관성은 유지됨) */

	bfqq->weight_counter->weight = entity->weight; /* 새 카운터에 이 weight 값을 기록 */
	rb_link_node(&bfqq->weight_counter->weights_node, parent, new); /* 탐색으로 찾은 위치에 새 노드를 연결 */
	/* [한국어] rb-tree 균형을 맞추며 삽입 완료 - leftmost면 캐시된 최소값 포인터도 갱신 */
	rb_insert_color_cached(&bfqq->weight_counter->weights_node, root,
				leftmost);

inc_counter:
	bfqq->weight_counter->num_active++; /* 이 weight를 공유하는 활성 큐 수 증가 */
	bfqq->ref++; /* 카운터가 bfqq를 참조하는 형태이므로 bfqq 자체의 참조 카운트도 증가시켜 조기 해제를 방지 */
}

/*
 * Decrement the weight counter associated with the queue, and, if the
 * counter reaches 0, remove the counter from the tree.
 * See the comments to the function bfq_weights_tree_add() for considerations
 * about overhead.
 */
/*
 * [한국어]
 * bfq_weights_tree_remove - bfqq가 비활성화될 때 queue_weights_tree에서
 *                           이 bfqq의 가중치 참조를 해제하고, 더 이상 그
 *                           weight를 공유하는 큐가 없으면 카운터 노드
 *                           자체를 트리에서 제거한다.
 *
 * @bfqq: 비활성화(idle로 전이하거나 소멸)되는 bfq_queue.
 * @return: 없음(void).
 *
 * bfq_weights_tree_add()의 짝이 되는 함수로, num_active(이 weight를 공유
 * 하는 큐 수)를 감소시킨 뒤 아직 다른 큐가 이 카운터를 쓰고 있으면
 * (num_active > 0) 카운터는 트리에 남겨두고 bfqq의 연결만 끊는다
 * (reset_entity_pointer로 goto). num_active가 0이 되면(마지막 사용자
 * 였다면) rb_erase_cached로 트리에서 카운터 노드를 완전히 제거하고
 * kfree로 메모리를 반환한다. 마지막으로 bfqq->weight_counter를 NULL로
 * 리셋하고, bfq_weights_tree_add에서 추가했던 bfqq 참조 카운트를
 * bfq_put_queue로 되돌려준다(대칭적인 get/put 쌍). bfqq->weight_counter가
 * 애초에 NULL이면(트리에 추가된 적이 없으면, 예: 할당 실패했던 경우)
 * 아무 일도 하지 않고 조기 반환해 안전하게 idempotent(멱등)하다.
 * bfqd->lock을 쥔 스케줄러 콜백(주로 bfqq가 idle로 전이할 때) 안에서
 * 호출된다.
 *
 * 호출 체인:
 *   bfq_deactivate_bfqq/__bfq_deactivate_entity 등
 *   → [bfq_weights_tree_remove] → rb_erase_cached/kfree/bfq_put_queue
 */
void bfq_weights_tree_remove(struct bfq_queue *bfqq)
{
	struct rb_root_cached *root; /* bfqq가 속한 디스크의 가중치 분포 트리 - 아래에서 지연 초기화 */

	if (!bfqq->weight_counter) /* 애초에 트리에 등록된 적이 없다면(예: 이전 add에서 할당 실패) */
		return; /* 할 일이 없으므로 조기 반환 - 멱등성 보장 */

	root = &bfqq->bfqd->queue_weights_tree; /* 이 디스크의 가중치 트리를 대상으로 지정 */
	bfqq->weight_counter->num_active--; /* 이 weight를 쓰던 활성 큐 수를 하나 줄임 */
	if (bfqq->weight_counter->num_active > 0) /* 아직 이 weight를 쓰는 다른 큐가 남아 있다면 */
		goto reset_entity_pointer; /* 카운터 노드 자체는 트리에 유지한 채 bfqq 연결만 해제하러 이동 */

	rb_erase_cached(&bfqq->weight_counter->weights_node, root); /* 마지막 사용자였으므로 트리에서 노드를 제거(캐시된 leftmost 포인터도 함께 갱신) */
	kfree(bfqq->weight_counter); /* 더 이상 참조되지 않는 카운터 구조체 메모리 해제 */

reset_entity_pointer:
	bfqq->weight_counter = NULL; /* 이 bfqq는 더 이상 어떤 카운터도 가리키지 않도록 정리 */
	bfq_put_queue(bfqq); // NVMe bfqq 반납: 참조 카운트가 0이면 bfqq를 해제하며, 남은 request는 완료/abort로 정리되어야 함
}

/*
 * Return expired entry, or NULL to just start from scratch in rbtree.
 */
/*
 * [한국어]
 * bfq_check_fifo - bfqq의 FIFO(도착 순서) 리스트에서 가장 오래된
 *                  request가 이미 시간 한도(fifo_time)를 넘겼는지 확인해,
 *                  그렇다면 그 request를 강제로 다음 dispatch 후보로 반환한다.
 *
 * @bfqq: 검사 대상 bfq_queue. bfqq->fifo는 도착 순서를 유지하는 리스트,
 *        bfqq->sort_list와 별개로 "얼마나 오래 기다렸는지"만 추적한다.
 * @last: 방금 처리한(혹은 기준이 되는) request. FIFO 헤드가 이 request와
 *        같으면 이미 처리된 것이므로 만료 처리를 건너뛴다.
 * @return: FIFO 만료로 강제 처리해야 할 request, 또는 만료된 것이 없으면
 *          NULL(이 경우 호출자는 LBA 기준 rb-tree 탐색으로 되돌아감).
 *
 * BFQ는 기본적으로 LBA(디스크 위치) 순서로 request를 골라 탐색 비용을
 * 줄이지만, 그렇게 하면 디스크 뒤쪽에 위치한 request가 영원히 밀릴 수
 * 있다(starvation). 이를 막기 위해 각 큐는 자신의 request들에 대해
 * fifo_time(허용 대기 한도)을 두고, 주기적으로(bfq_bfqq_fifo_expire
 * 플래그로 "이번 dispatch에서 이미 확인했는지" 추적하며) 가장 오래된
 * request가 시간을 넘겼는지 확인한다. 이미 이번 라운드에서 확인했다면
 * (bfq_bfqq_fifo_expire(bfqq)==true) 다시 확인하지 않고 NULL을 반환해
 * 정상적인 LBA 기준 선택을 계속하도록 한다. 아니라면 확인했다는 플래그를
 * 세우고, FIFO 헤드가 last와 같거나(이미 방금 처리된 것) 아직 만료
 * 시각에 도달하지 않았으면 NULL을 반환한다. 그 외의 경우 만료된 request를
 * 반환해 호출자가 강제로 그것을 다음으로 dispatch하도록 만든다. bfqd->lock을
 * 쥔 스케줄러 콜백 안에서 호출된다.
 *
 * 호출 체인:
 *   bfq_find_next_rq → [bfq_check_fifo] → rq_entry_fifo/blk_time_get_ns
 */
static struct request *bfq_check_fifo(struct bfq_queue *bfqq,
				      struct request *last)
{
	struct request *rq; /* FIFO 리스트 헤드(가장 오래된 request) 후보 */

	if (bfq_bfqq_fifo_expire(bfqq)) /* 이번 dispatch 라운드에서 이미 FIFO 만료를 확인했다면 */
		return NULL; /* 중복 확인을 피하고 정상 LBA 기준 선택으로 위임 */

	bfq_mark_bfqq_fifo_expire(bfqq); /* 이번 라운드에는 확인했음을 표시 - 다음 호출까지 재확인 방지 */

	rq = rq_entry_fifo(bfqq->fifo.next); /* FIFO 리스트에서 가장 먼저 도착한(가장 오래 기다린) request */

	if (rq == last || blk_time_get_ns() < rq->fifo_time) /* 그 request가 이미 처리 대상(last)이거나 아직 만료 시각 전이면 */
		return NULL; /* 강제 처리할 필요 없음 - 정상 경로로 진행 */

	bfq_log_bfqq(bfqq->bfqd, bfqq, "check_fifo: returned %p", rq); /* 디버그 로그 - 어떤 request가 강제로 만료 처리되는지 기록 */
	return rq; /* 시간 한도를 넘긴 request를 반환해 starvation 방지를 위해 우선 dispatch되도록 함 */
}

/*
 * [한국어]
 * bfq_find_next_rq - bfqq의 sort_list(rb-tree)에서 last 다음으로 dispatch할
 *                    최적의 request를 찾는다. FIFO 만료 우선, 없으면
 *                    LBA 인접성 기준 선택.
 *
 * @bfqd: 디스크 전역 BFQ 상태 - bfq_choose_req의 back_max/penalty 등에 사용.
 * @bfqq: 탐색 대상 bfq_queue.
 * @last: 기준이 되는 request(방금 dispatch했거나 현재 next_rq였던 것).
 * @return: 다음으로 dispatch하기 가장 적합한 request. bfqq에 남은 request가
 *          없으면 NULL일 수 있음(bfq_choose_req가 둘 다 NULL을 받으면 NULL 반환).
 *
 * 먼저 bfq_check_fifo()로 "너무 오래 기다린" request가 있는지 확인해
 * 있으면 그것을 최우선으로 반환한다(starvation 방지). 없으면 last를
 * 기준으로 rb-tree 상에서 바로 다음(rbnext)과 바로 이전(rbprev) 노드를
 * 후보로 삼는데, rbnext가 없으면(last가 트리의 마지막 노드) 트리의
 * 맨 처음(rb_first)으로 되돌아가 순환하듯 다음 후보를 찾는다(단, 그것이
 * last 자신이 아닐 때만). 최종적으로 prev/next 두 후보를 bfq_choose_req로
 * 넘겨 head 위치(last의 섹터) 기준 탐색 비용이 더 적은 쪽을 고른다.
 * bfqd->lock을 쥔 스케줄러 콜백(주로 dispatch 경로) 안에서 호출된다.
 *
 * 호출 체인:
 *   bfq_dispatch_request/bfq_updated_next_req 등
 *   → [bfq_find_next_rq] → bfq_check_fifo/rb_next/rb_prev/bfq_choose_req
 */
static struct request *bfq_find_next_rq(struct bfq_data *bfqd,
					struct bfq_queue *bfqq,
					struct request *last)
{
	struct rb_node *rbnext = rb_next(&last->rb_node); /* rb-tree에서 last 바로 다음(더 큰 섹터) 노드 */
	struct rb_node *rbprev = rb_prev(&last->rb_node); /* rb-tree에서 last 바로 이전(더 작은 섹터) 노드 */
	struct request *next, *prev = NULL; /* 최종 비교에 넘길 다음/이전 후보 request */

	/* Follow expired path, else get first next available. */
	next = bfq_check_fifo(bfqq, last); /* 시간 한도를 넘긴 request가 있는지 먼저 확인 */
	if (next) /* 만료된 request가 있다면 */
		return next; /* 다른 비교 없이 즉시 그것을 최우선으로 반환 */

	if (rbprev) /* 이전 노드가 존재하면 */
		prev = rb_entry_rq(rbprev); /* rb_node를 감싸는 request로 변환 */

	if (rbnext) /* 다음 노드가 존재하면(last가 트리의 마지막이 아니면) */
		next = rb_entry_rq(rbnext); /* 그 노드를 다음 후보로 사용 */
	else { /* last가 트리에서 가장 큰 섹터(마지막 노드)였다면 */
		rbnext = rb_first(&bfqq->sort_list); /* 트리의 맨 앞(가장 작은 섹터)으로 순환해서 되돌아감 */
		if (rbnext && rbnext != &last->rb_node) /* 트리가 비어있지 않고, 되돌아간 지점이 last 자신이 아니라면(단일 노드 방지) */
			next = rb_entry_rq(rbnext); /* 그 노드를 다음 후보로 사용 */
	}

	return bfq_choose_req(bfqd, next, prev, blk_rq_pos(last)); /* next/prev 두 후보 중 last(head) 위치 기준 탐색 비용이 더 적은 쪽을 최종 선택 */
}

/* see the definition of bfq_async_charge_factor for details */
/*
 * [한국어]
 * bfq_serv_to_charge - 이 request를 처리하는 데 bfqq의 예산(budget)에서
 *                      얼마를 차감(charge)할지 계산한다.
 *
 * @rq: 차감량을 계산할 대상 request.
 * @bfqq: 이 request가 속한 bfq_queue.
 * @return: budget에서 차감할 섹터 수. 동기/weight-raised/비대칭 시나리오
 *          에서는 실제 섹터 수 그대로, 그 외(대칭 시나리오의 순수 비동기)
 *          에서는 bfq_async_charge_factor 배만큼 부풀린 값.
 *
 * BFQ는 budget을 "처리한 섹터 수"로 소모시켜, 예산이 바닥나면 다음 큐로
 * 서비스를 넘긴다. 동기(sync) I/O이거나, 이 큐가 현재 weight-raised
 * (wr_coeff > 1)이거나, 비대칭 시나리오(bfq_asymmetric_scenario)라서
 * 어차피 idling 등으로 별도 공정성 보장이 필요한 상황이라면 있는 그대로의
 * 섹터 수만 차감한다. 반대로 대칭 시나리오에서 순수 비동기 I/O라면, 비동기
 * 요청이 동기 요청보다 실제 체감 지연에 덜 민감하다는 점을 이용해 charge를
 * 인위적으로 부풀림으로써(async_charge_factor) 비동기 큐의 budget이 더
 * 빨리 소진되게 만들고, 결과적으로 동기 큐에 더 많은 서비스 기회를
 * 넘겨준다. 상태를 변경하지 않는 순수 계산 함수로, bfqd->lock을 쥔
 * 스케줄러 콜백 안에서 호출된다.
 *
 * 호출 체인:
 *   bfq_updated_next_req/bfq_dispatch_request 등
 *   → [bfq_serv_to_charge] → blk_rq_sectors/bfq_asymmetric_scenario
 */
static unsigned long bfq_serv_to_charge(struct request *rq,
					struct bfq_queue *bfqq)
{
	if (bfq_bfqq_sync(bfqq) || bfqq->wr_coeff > 1 || /* 동기 큐이거나 weight-raised 상태이거나 */
	    bfq_asymmetric_scenario(bfqq->bfqd, bfqq)) /* 이미 비대칭 시나리오라 별도 공정성 보정이 필요하다면 */
		return blk_rq_sectors(rq); /* 실제 섹터 수 그대로 charge - 부풀리지 않음 */

	return blk_rq_sectors(rq) * bfq_async_charge_factor; /* 대칭 시나리오의 순수 비동기 I/O는 charge를 배율만큼 부풀려 budget을 빨리 소진시킴 */
}

/**
 * bfq_updated_next_req - update the queue after a new next_rq selection.
 * @bfqd: the device data the queue belongs to.
 * @bfqq: the queue to update.
 *
 * If the first request of a queue changes we make sure that the queue
 * has enough budget to serve at least its first request (if the
 * request has grown).  We do this because if the queue has not enough
 * budget for its first request, it has to go through two dispatch
 * rounds to actually get it dispatched.
 */
/*
 * [한국어]
 * @bfqd: 디스크 전역 BFQ 상태 - bfq_requeue_bfqq 등에 전달됨.
 * @bfqq: next_rq가 갱신된 bfq_queue.
 * @return: 없음(void).
 *
 * bfqq->next_rq(다음 dispatch 후보)가 바뀌면(더 큰 request로 교체되는
 * 등), 그 새 request를 한 번에 서비스할 수 있을 만큼 budget이 충분한지
 * 재확인해야 한다. budget이 부족하면 이 큐는 첫 request조차 한 번의
 * dispatch 라운드로 끝내지 못하고 두 번째 라운드까지 필요해지는 비효율이
 * 생기므로, 이 함수는 max_budget/charge량/이미 소모한 service량 중 가장
 * 큰 값으로 budget을 미리 늘려둔다. 단, 이미 in-service(현재 디스패치
 * 중인 큐)로 선택된 엔티티는 B-WF2Q+ 알고리즘의 공정성 보장을 깨뜨리지
 * 않기 위해 budget을 도중에 바꿀 수 없으므로 조기 반환한다. budget이
 * 실제로 바뀌면 bfq_requeue_bfqq로 이 큐를 서비스 트리에서 재정렬한다
 * (budget이 바뀌면 B-WF2Q+ 상의 마감시각(finish time)도 바뀌므로).
 * bfqd->lock을 쥔 스케줄러 콜백(request 삽입/삭제 경로) 안에서 호출된다.
 *
 * 호출 체인:
 *   bfq_add_request/bfq_remove_request 등
 *   → [bfq_updated_next_req] → bfq_serv_to_charge/bfq_requeue_bfqq
 */
static void bfq_updated_next_req(struct bfq_data *bfqd,
				 struct bfq_queue *bfqq)
{
	struct bfq_entity *entity = &bfqq->entity; /* bfqq를 감싸는 스케줄링 엔티티 - budget/service 필드 보유 */
	struct request *next_rq = bfqq->next_rq; /* 갱신된 새 dispatch 후보 request */
	unsigned long new_budget; /* 재계산될 budget 값 */

	if (!next_rq) /* 다음 request 자체가 없다면(큐가 비었거나 방금 비워짐) */
		return; /* 재계산할 대상이 없으므로 그대로 반환 */

	if (bfqq == bfqd->in_service_queue) /* 이 bfqq가 지금 막 서비스 중인(선택된) 큐라면 */
		/*
		 * In order not to break guarantees, budgets cannot be
		 * changed after an entity has been selected.
		 */
		return; /* B-WF2Q+ 공정성 보장을 지키기 위해 이미 선택된 엔티티의 budget은 변경 금지 */

	new_budget = max_t(unsigned long,
			   max_t(unsigned long, bfqq->max_budget, /* 이 큐가 지금까지 가져본 최대 budget과 */
				 bfq_serv_to_charge(next_rq, bfqq)), /* 새 next_rq를 한 번에 처리하는 데 필요한 charge 중 큰 값 */
			   entity->service); /* 그리고 현재까지 이미 소모한 service량 중에서도 가장 큰 값을 최종 budget으로 선택 */
	if (entity->budget != new_budget) { /* 재계산한 값이 기존 budget과 다르면(변경이 실제로 필요하면) */
		entity->budget = new_budget; /* budget 갱신 */
		/* [한국어] 디버그 로그 - 새 budget 값 기록 */
		bfq_log_bfqq(bfqd, bfqq, "updated next rq: new budget %lu",
					 new_budget);
		bfq_requeue_bfqq(bfqd, bfqq, false); /* budget 변경으로 B-WF2Q+ 마감시각이 바뀌므로 서비스 트리에서 위치 재조정 */
	}
}

/*
 * [한국어]
 * bfq_wr_duration - 현재 측정된 디스크 성능(peak_rate)에 맞춰 weight-raising
 *                   (가중치 상승) 기간을 얼마나 유지할지 계산한다.
 *
 * @bfqd: 디스크 전역 BFQ 상태. rate_dur_prod(기준 "처리율 x 기간" 상수)와
 *        peak_rate(현재까지 측정된 최고 처리율)를 참조한다.
 * @return: weight-raising을 유지할 기간(jiffies 단위), 3~25초 범위로 clamp됨.
 *
 * BFQ는 interactive/soft-real-time 프로세스에게 일정 기간 동안 가중치를
 * 높여주는데(weight-raising), 그 기간이 너무 짧으면 느린 디스크에서 앱
 * 로딩이 끝나기 전에 특혜가 풀려버리고, 너무 길면 비대화형 앱까지
 * 부당하게 오래 우대받는다. 이를 위해 rate_dur_prod(속도x기간의 기준
 * 상수)를 실제 측정된 peak_rate로 나누어, 디스크가 느릴수록(peak_rate가
 * 작을수록) duration이 길어지고 빠를수록 짧아지도록 반비례 스케일링한다.
 * 그 결과를 3초~25초(msecs_to_jiffies로 변환) 범위로 clamp하는데, 상한은
 * 매우 느린 디스크에서 실측된 최악의 경우(주석에 인용된 QEMU/5400rpm HDD
 * 사례, mplayer 시작에 23초)를 참고해 보수적으로 정했고, 하한은 3초보다
 * 짧으면 대부분의 대화형 작업이 끝나기도 전에 WR이 풀리는 문제를 막기
 * 위함이다. 순수 계산 함수로 상태를 변경하지 않으며, bfqd->lock을 쥔
 * 컨텍스트에서 주로 호출된다.
 *
 * 호출 체인:
 *   bfq_bfqq_resume_state/switch_back_to_interactive_wr/bfq_add_bfqq_busy 등
 *   → [bfq_wr_duration] → do_div/clamp_val
 */
static unsigned int bfq_wr_duration(struct bfq_data *bfqd)
{
	u64 dur; /* 64비트 중간 계산값 - do_div가 상위 32비트 오버플로를 막기 위해 필요 */

	dur = bfqd->rate_dur_prod; /* 기준 "처리율 x 기간" 상수(장치 종류에 따라 초기화 시 설정됨) */
	do_div(dur, bfqd->peak_rate); /* 실제 측정된 최고 처리율로 나눠 이 장치에 맞는 duration을 역산 - do_div는 64비트 나눗셈을 32비트 아키텍처에서도 지원 */

	/*
	 * Limit duration between 3 and 25 seconds. The upper limit
	 * has been conservatively set after the following worst case:
	 * on a QEMU/KVM virtual machine
	 * - running in a slow PC
	 * - with a virtual disk stacked on a slow low-end 5400rpm HDD
	 * - serving a heavy I/O workload, such as the sequential reading
	 *   of several files
	 * mplayer took 23 seconds to start, if constantly weight-raised.
	 *
	 * As for higher values than that accommodating the above bad
	 * scenario, tests show that higher values would often yield
	 * the opposite of the desired result, i.e., would worsen
	 * responsiveness by allowing non-interactive applications to
	 * preserve weight raising for too long.
	 *
	 * On the other end, lower values than 3 seconds make it
	 * difficult for most interactive tasks to complete their jobs
	 * before weight-raising finishes.
	 */
	return clamp_val(dur, msecs_to_jiffies(3000), msecs_to_jiffies(25000)); /* 계산된 duration을 3~25초 범위로 강제 제한해 반환 */
}

/* switch back from soft real-time to interactive weight raising */
/*
 * [한국어]
 * switch_back_to_interactive_wr - soft real-time(연성 실시간) weight-raising
 *                                 상태였던 bfqq를 원래의 interactive(대화형)
 *                                 weight-raising 상태로 되돌린다.
 *
 * @bfqq: 되돌릴 대상 bfq_queue.
 * @bfqd: 디스크 전역 BFQ 상태 - 표준 WR 계수(bfq_wr_coeff)와 duration
 *        계산에 쓰인다.
 * @return: 없음(void).
 *
 * BFQ는 처음에 interactive WR을 주다가, 그 프로세스가 소프트 실시간
 * (soft real-time, 예: 주기적으로 조금씩 I/O를 내는 미디어 재생 등)
 * 패턴으로 판정되면 더 긴/다른 파라미터의 SRT WR로 전환한다. 이 함수는
 * 그 SRT WR이 만료되었을 때(또는 SRT 조건이 더 이상 유지되지 않을 때)
 * 다시 표준 interactive WR 상태로 복귀시키는 역할을 한다: wr_coeff를
 * 디스크의 표준 interactive 계수(bfqd->bfq_wr_coeff)로, wr_cur_max_time을
 * bfq_wr_duration()으로 새로 계산한 기본 interactive 기간으로 재설정하고,
 * last_wr_start_finish(현재 WR 구간의 시작 시각)를 SRT로 전환하기 직전에
 * 저장해 둔 wr_start_at_switch_to_srt로 되돌려, 마치 SRT로 빠지지 않고
 * 계속 interactive WR을 유지해 온 것처럼 남은 기간을 이어서 계산하게
 * 한다. bfqd->lock을 쥔 스케줄러 콜백(주로 bfq_bfqq_resume_state) 안에서
 * 호출된다.
 *
 * 호출 체인:
 *   bfq_bfqq_resume_state/bfq_bfqq_softrt_next_start 관련 로직
 *   → [switch_back_to_interactive_wr] → bfq_wr_duration
 */
static void switch_back_to_interactive_wr(struct bfq_queue *bfqq,
					  struct bfq_data *bfqd)
{
	bfqq->wr_coeff = bfqd->bfq_wr_coeff; /* SRT 전용 계수 대신 디스크의 표준 interactive WR 계수로 되돌림 */
	bfqq->wr_cur_max_time = bfq_wr_duration(bfqd); /* interactive WR의 기본 유지 기간으로 재계산 */
	bfqq->last_wr_start_finish = bfqq->wr_start_at_switch_to_srt; /* SRT로 전환하기 직전 저장해 둔 시각으로 시작점을 되돌려 남은 interactive 기간을 이어서 계산 */
}

/*
 * [한국어]
 * bfq_bfqq_resume_state - 큐 병합(merge)에서 분리(split)되어 나온(또는
 *                         재사용되는) bfqq에, bic에 저장해 두었던 병합
 *                         이전의 상태(ttime, IO_bound, weight-raising 등)를
 *                         복원한다.
 *
 * @bfqq: 상태를 복원할 대상 bfq_queue. 분리 시 기존 bfqq를 재사용하는
 *        경우이거나, 새로 할당된 bfqq일 수 있다(아래 @bfq_already_existing
 *        참고).
 * @bfqd: 디스크 전역 BFQ 상태. wr_busy_queues 카운터와 low_latency/
 *        bfq_wr_rt_max_time 등 튜닝값을 참조한다.
 * @bic: 이 프로세스의 I/O 컨텍스트. bic->bfqq_data[a_idx]에 병합 시점에
 *       저장해 두었던 saved_* 필드들이 들어 있다(merge 직전 스냅샷).
 * @bfq_already_existing: true면 bfqq가 "이 프로세스가 마지막 참조자였던
 *        기존 큐를 그대로 재사용"하는 경우(bfq_split_bfqq가 non-NULL을
 *        반환), false면 분리로 인해 완전히 새로 할당된 bfqq인 경우.
 *        이 값이 true일 때만 busy 여부를 실제로 검사해 wr_busy_queues
 *        카운터를 조정한다(새 큐는 아직 busy일 수 없으므로).
 * @return: 없음(void).
 *
 * BFQ는 협력 프로세스로 판단해 두 bfqq를 하나로 병합했다가, 나중에 그
 * 협력 관계가 끝나면(예: seek 패턴이 다시 벌어짐) 다시 원래의 개별 bfqq로
 * 분리한다. 병합되는 순간 원래 bfqq의 여러 상태(think-time 추정치,
 * IO-bound 여부, weight-raising 계수와 잔여 시간, injection 한도 등)는
 * bic->bfqq_data[]에 스냅샷으로 저장돼 있었는데, 분리가 일어나면 이
 * 함수가 그 스냅샷을 다시 bfqq(재사용되는 기존 큐 또는 새로 할당된 큐)에
 * 되돌려 놓아, 병합되지 않았던 것처럼 연속성 있는 스케줄링 상태를 유지
 * 하게 한다. weight-raising 계수는 bfqd->low_latency가 켜져 있을 때만
 * 복원하며(꺼져 있으면 애초에 WR을 쓰지 않으므로), 복원 후 WR이 이미
 * 만료 조건(large burst에 속하거나 wr_cur_max_time을 초과)을 만족하면
 * SRT(soft real-time)에서 interactive로 되돌리거나(switch_back_to_interactive_wr)
 * 아예 WR을 끈다(wr_coeff = 1). 마지막으로 이 bfqq가 이미 busy(활성)
 * 상태였다면(재사용 케이스에서만 의미 있음) WR 여부 변화에 맞춰
 * bfqd->wr_busy_queues 전역 카운터도 갱신한다. bfqd->lock을 쥔 컨텍스트
 * (bfq_get_bfqq_handle_split 경로) 안에서 호출된다.
 *
 * 호출 체인:
 *   bfq_get_bfqq_handle_split → [bfq_bfqq_resume_state]
 *   → switch_back_to_interactive_wr/bfq_wr_duration/bfq_log_bfqq
 */
static void
bfq_bfqq_resume_state(struct bfq_queue *bfqq, struct bfq_data *bfqd,
		      struct bfq_io_cq *bic, bool bfq_already_existing)
{
	unsigned int old_wr_coeff = 1; /* WR 전이 감지용 - 복원 전 wr_coeff를 담아 뒤에서 1<->>1 전환 여부 판단 */
	bool busy = bfq_already_existing && bfq_bfqq_busy(bfqq); /* 기존 큐 재사용 케이스에서만 실제 busy 여부 확인(새 큐는 아직 busy일 수 없음) */
	unsigned int a_idx = bfqq->actuator_idx; /* 이 bfqq가 속한 액추에이터 인덱스 - bic의 per-actuator 저장 데이터 선택에 사용 */
	struct bfq_iocq_bfqq_data *bfqq_data = &bic->bfqq_data[a_idx]; /* 병합 시점에 저장해 둔 saved_* 필드 모음 */

	if (bfqq_data->saved_has_short_ttime) /* 병합 전 "think time이 짧은(interactive에 가까운)" 상태였다면 */
		bfq_mark_bfqq_has_short_ttime(bfqq); /* 그 플래그를 복원 */
	/* [한국어] 아니었다면 플래그 해제 */
	else
		bfq_clear_bfqq_has_short_ttime(bfqq);

	if (bfqq_data->saved_IO_bound) /* 병합 전 "I/O 바운드(순수 I/O 위주)" 분류였다면 */
		bfq_mark_bfqq_IO_bound(bfqq); /* 그 분류를 복원 */
	/* [한국어] 아니었다면 해제 */
	else
		bfq_clear_bfqq_IO_bound(bfqq);

	bfqq->last_serv_time_ns = bfqq_data->saved_last_serv_time_ns; /* 마지막 서비스 시간 추정치 복원 - injection 한도 계산에 사용됨 */
	bfqq->inject_limit = bfqq_data->saved_inject_limit; /* injection(다른 큐 request 끼워넣기) 허용 한도 복원 */
	bfqq->decrease_time_jif = bfqq_data->saved_decrease_time_jif; /* injection 한도를 낮출 다음 시각 복원 */

	bfqq->entity.new_weight = bfqq_data->saved_weight; /* 병합 전 가중치를 복원 - 다음 prio_changed 처리 때 반영됨 */
	bfqq->ttime = bfqq_data->saved_ttime; /* think-time 통계(평균/샘플수) 복원 - seek/interactive 판별의 근거 */
	bfqq->io_start_time = bfqq_data->saved_io_start_time; /* 이 활성 구간의 I/O 시작 시각 복원 */
	bfqq->tot_idle_time = bfqq_data->saved_tot_idle_time; /* 누적 idle 시간 복원 - IO_bound 재판정 등에 사용 */
	/*
	 * Restore weight coefficient only if low_latency is on
	 */
	if (bfqd->low_latency) { /* 이 디스크가 low_latency(WR 사용) 모드일 때만 */
		old_wr_coeff = bfqq->wr_coeff; /* 복원 전 현재 wr_coeff를 저장 - 아래에서 전이 여부 판단에 사용 */
		bfqq->wr_coeff = bfqq_data->saved_wr_coeff; /* 병합 전 WR 계수를 복원 */
	}
	bfqq->service_from_wr = bfqq_data->saved_service_from_wr; /* WR 기간 중 누적 서비스량 복원 */
	/* [한국어] SRT 전환 시각 복원 - switch_back_to_interactive_wr 등에서 참조 */
	bfqq->wr_start_at_switch_to_srt =
		bfqq_data->saved_wr_start_at_switch_to_srt;
	bfqq->last_wr_start_finish = bfqq_data->saved_last_wr_start_finish; /* 현재 WR 구간의 시작/직전 종료 시각 복원 */
	bfqq->wr_cur_max_time = bfqq_data->saved_wr_cur_max_time; /* WR 최대 유지 기간 복원 */

	if (bfqq->wr_coeff > 1 && (bfq_bfqq_in_large_burst(bfqq) || /* 복원 결과 여전히 WR 중인데, large burst에 속하거나 */
	    time_is_before_jiffies(bfqq->last_wr_start_finish +
				   bfqq->wr_cur_max_time))) { /* 이미 WR 유지 기간을 초과했다면 - WR을 재조정해야 함 */
		if (bfqq->wr_cur_max_time == bfqd->bfq_wr_rt_max_time && /* 그 WR이 SRT(soft real-time)용 최대 시간을 쓰고 있었고 */
		    !bfq_bfqq_in_large_burst(bfqq) && /* large burst에 속한 게 아니며(순수 시간 초과) */
		    time_is_after_eq_jiffies(bfqq->wr_start_at_switch_to_srt +
					     bfq_wr_duration(bfqd))) { /* SRT 전환 후 표준 interactive WR 기간만큼도 이미 지났다면 */
			switch_back_to_interactive_wr(bfqq, bfqd); /* SRT에서 interactive WR로 복귀시켜 표준 파라미터로 이어감 */
		} else { /* 그 외의 초과/burst 상황이라면 */
			bfqq->wr_coeff = 1; /* WR을 아예 종료(계수를 1로) - 더 이상 우대할 근거가 없음 */
			/* [한국어] 디버그 로그 - WR 종료 사실 기록 */
			bfq_log_bfqq(bfqq->bfqd, bfqq,
				     "resume state: switching off wr");
		}
	}

	/* make sure weight will be updated, however we got here */
	bfqq->entity.prio_changed = 1; /* new_weight 등 복원된 값이 실제 entity->weight에 반영되도록 재계산 트리거 */

	if (likely(!busy)) /* 재사용 케이스가 아니거나(새 큐), busy가 아니었다면 */
		return; /* wr_busy_queues 카운터를 건드릴 필요 없음(활성 상태 변화가 없었으므로) */

	if (old_wr_coeff == 1 && bfqq->wr_coeff > 1) /* busy 상태에서 WR이 없다가 생겼다면 */
		bfqd->wr_busy_queues++; /* 전역 "WR 중이며 busy인 큐" 카운터 증가 */
	else if (old_wr_coeff > 1 && bfqq->wr_coeff == 1) /* 반대로 WR이 있다가 없어졌다면 */
		bfqd->wr_busy_queues--; /* 카운터 감소 */
}

/*
 * [한국어]
 * bfqq_process_refs - bfqq에 대해 실제 "프로세스"가 들고 있는 참조 개수를 계산
 *
 * @bfqq: 참조 개수를 셀 대상 bfq_queue
 * @return: bfqq->ref에서 스케줄러 내부 부기(bookkeeping)용 참조를 뺀 나머지,
 *          즉 io_context(bic)를 통해 실제 프로세스가 붙잡고 있는 참조 수
 *
 * bfqq->ref는 엔티티가 서비스 트리에 올라가 있거나(on_st_or_in_serv),
 * entity가 스케줄러에 등록되어 있거나(allocated), 가중치 카운터(weight_counter)에
 * 연결되어 있거나, 안정적 병합(stable merge) 관계로 고정 참조(stable_ref)를
 * 갖는 등 스케줄러 내부 사정으로도 증가한다. 이런 내부 참조를 모두 빼야
 * "이 큐를 실제로 사용 중인 프로세스가 몇 개인가"를 정확히 알 수 있다.
 * 이 값이 1이면 해당 bfqq는 단 하나의 프로세스만 사용 중이라는 뜻이므로
 * 큐 병합(cooperating queue merge) 여부를 결정할 때 안전하게 병합/분리
 * 판단을 내릴 수 있고, 0이면 이미 다른 bfqq로 병합되어 실질적으로
 * 비어 있는 큐라는 뜻이다.
 * 실행 컨텍스트: bfqd->lock을 쥔 상태에서 호출되는 것이 일반적이며
 * (큐 병합/분리 판단, in-service 큐 유효성 검사 시) 별도의 락을 걸지 않는다.
 * caller: bfq_setup_merge/bfq_setup_cooperator, 안정 병합 로직,
 * idling 필요 여부 판단(idling_needed_for_service_guarantees 등),
 * bfq_release_process_ref 계열 등 다수.
 * callee: 없음 (단순 산술).
 *
 * 호출 체인:
 *   bfq_setup_cooperator/idling_needed_for_service_guarantees 등 → [bfqq_process_refs]
 */
static int bfqq_process_refs(struct bfq_queue *bfqq)
{
	/* bfqq->ref: 이 bfqq에 대한 전체 참조 카운트(프로세스 + 스케줄러 내부).
	 * entity.allocated: entity가 상위 스케줄러 트리에 등록되어 있으면 1 -
	 *                    이 등록 자체가 참조를 하나 차지하므로 빼야 한다.
	 * entity.on_st_or_in_serv: 서비스 트리에 있거나 in-service 상태이면 1 -
	 *                    스케줄러가 능동적으로 붙잡고 있는 참조이므로 제외.
	 * (weight_counter != NULL): 가중치별 큐 개수를 세는 전역 rb-tree에
	 *                    연결되어 있으면 1 - 이것도 프로세스 참조가 아님.
	 * stable_ref: 안정 병합 대상으로 지정되어 고정된 참조 수 - 마찬가지로
	 *                    실제 프로세스 사용과 무관하므로 제외. */
	return bfqq->ref - bfqq->entity.allocated -
		bfqq->entity.on_st_or_in_serv -
		(bfqq->weight_counter != NULL) - bfqq->stable_ref;
		/* 위 네 항목을 뺀 결과가 곧 "실제 프로세스 참조 수"이며,
		 * 호출자는 이 값이 1인지(단일 소유자) 0인지(고아 큐)를 검사해
		 * 병합/idling 여부를 결정한다. */
}

/* Empty burst list and add just bfqq (see comments on bfq_handle_burst) */
/*
 * [한국어]
 * bfq_reset_burst_list - burst 리스트를 비우고 bfqq 하나만 새로 등록
 *
 * @bfqd: 이 장치의 스케줄러 데이터 (burst_list, burst_size, burst_parent_entity 보유)
 * @bfqq: 새로 시작되는(가능성 있는) burst의 첫 번째 큐가 될 bfqq
 * @return: 없음 (void)
 *
 * 현재 진행 중이던 burst가 "끝났다"고 판단되었을 때 호출된다(호출자
 * bfq_handle_burst 참고). 기존에 burst_list에 걸려 있던 모든 bfqq의
 * burst_list_node를 hlist에서 제거해 리스트를 비우고, 활성 큐가 하나도
 * 없는 경우에만(bfq_tot_busy_queues(bfqd) == 0) bfqq 자신을 새 burst의
 * 첫 큐로 등록한다. 활성 큐가 있다면 지금 활성화되는 bfqq는 burst의
 * 시작점으로 보기 애매하므로 burst_size를 0으로만 리셋하고 리스트에는
 * 넣지 않는다(다음 번 호출에서 다시 판단).
 * 실행 컨텍스트: bfqd->lock을 쥔 상태(큐 활성화 경로, bfq_handle_burst 내부)에서
 * 호출되므로 burst_list/burst_size에 대한 동시 접근 걱정은 없다.
 * caller: bfq_handle_burst (burst 종료로 판단된 경로).
 * callee: bfq_tot_busy_queues, hlist_for_each_entry_safe/hlist_del_init/hlist_add_head.
 *
 * 호출 체인:
 *   bfq_handle_burst → [bfq_reset_burst_list] → bfq_tot_busy_queues
 */
static void bfq_reset_burst_list(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	struct bfq_queue *item;
	/* burst_list를 순회할 때 쓰는 커서 - 곧 리스트에서 제거될 각 bfqq를 가리킨다. */
	struct hlist_node *n;
	/* hlist_for_each_entry_safe가 순회 도중 노드 삭제를 안전하게 하기 위해
	 * 사용하는 임시 포인터(다음 노드를 미리 저장). */

	hlist_for_each_entry_safe(item, n, &bfqd->burst_list, burst_list_node)
		/* burst_list에 걸려 있던 이전 burst의 큐들을 하나씩 리스트에서
		 * 분리한다 - bfqq 자체를 free하지는 않고 노드만 초기화(hlist_del_init)해
		 * 이후 이 큐가 다시 burst_list에 들어갈 수 있도록 한다. */
		hlist_del_init(&item->burst_list_node);

	/*
	 * Start the creation of a new burst list only if there is no
	 * active queue. See comments on the conditional invocation of
	 * bfq_handle_burst().
	 */
	/* 현재 장치에 활성(busy) 상태인 bfqq가 하나도 없다는 뜻 - 즉 bfqq가
	 * 정말로 "새로운 burst의 첫 큐"일 가능성이 있으므로 리스트에 등록한다. */
	if (bfq_tot_busy_queues(bfqd) == 0) {
		hlist_add_head(&bfqq->burst_list_node, &bfqd->burst_list);
		/* bfqq를 burst_list의 head에 삽입 - 이후 짧은 시간 안에 다른 큐가
		 * 생성되면 bfq_add_to_burst에서 이 리스트에 이어 붙는다. */
		bfqd->burst_size = 1;
		/* burst 크기를 1로 초기화 - bfqq 자신이 burst의 첫 멤버. */
	} else
		bfqd->burst_size = 0;
		/* 활성 큐가 존재하면 bfqq를 burst의 시작점으로 볼 수 없으므로
		 * burst_size만 0으로 리셋하고 리스트에는 아무것도 넣지 않는다. */

	bfqd->burst_parent_entity = bfqq->entity.parent;
	/* 이후 도착하는 큐가 "같은 그룹(cgroup)에서 만들어졌는지"를 비교할
	 * 기준점을 갱신 - burst는 같은 부모 엔티티(같은 cgroup) 안에서만
	 * 유효한 것으로 취급된다(bfq_handle_burst의 parent 비교 참고). */
}

/* Add bfqq to the list of queues in current burst (see bfq_handle_burst) */
/*
 * [한국어]
 * bfq_add_to_burst - 현재 진행 중인 burst 리스트에 bfqq를 추가하고,
 *                     대량(large) burst 여부를 판정
 *
 * @bfqd: burst_size/large_burst/burst_list를 보유한 장치 스케줄러 데이터
 * @bfqq: 방금 활성화되어 burst의 멤버로 추가될 bfq_queue
 * @return: 없음 (void)
 *
 * bfq_handle_burst가 "bfqq는 직전 큐 생성 직후 짧은 시간 안에 생성되었다"고
 * 판단했을 때 호출된다. burst_size를 증가시키고, 그 값이 임계치
 * (bfq_large_burst_thresh)에 도달하면 지금까지 burst_list에 쌓인 모든 큐와
 * bfqq를 "large burst에 속함(in_large_burst)"으로 표시한 뒤 burst_list 자체를
 * 비운다 - 대량 burst로 확정된 이후에는 burst_list가 더 이상 필요 없고,
 * 새로 생성되는 큐는 bfqd->large_burst 플래그만 보고 즉시 large burst로
 * 표시할 수 있기 때문이다(bfq_handle_burst 참고). 아직 임계치에 못
 * 미쳤다면 단순히 bfqq를 burst_list에 이어 붙인다.
 * 실행 컨텍스트: bfqd->lock 보유 상태(bfq_handle_burst 내부, 큐 활성화 경로).
 * caller: bfq_handle_burst (large_burst가 아직 false인 경로에서만 호출됨).
 * callee: bfq_mark_bfqq_in_large_burst, hlist_add_head/hlist_del_init 계열.
 *
 * 호출 체인:
 *   bfq_handle_burst → [bfq_add_to_burst] → bfq_mark_bfqq_in_large_burst
 */
static void bfq_add_to_burst(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	/* Increment burst size to take into account also bfqq */
	/* 지금 활성화되는 bfqq도 burst의 일원으로 카운트에 반영한다. */
	bfqd->burst_size++;

	/* burst_size가 정확히 임계값에 도달한 순간 - 이 시점에 딱 한 번만
	 * "large burst로 전환"하는 부수효과(리스트 비우기 포함)를 수행한다. */
	if (bfqd->burst_size == bfqd->bfq_large_burst_thresh) {
		struct bfq_queue *pos, *bfqq_item;
		/* burst_list를 순회하며 각 큐를 large-burst로 표시할 때 쓰는 커서들. */
		struct hlist_node *n;
		/* hlist_for_each_entry_safe용 임시 다음-노드 포인터. */

		/*
		 * Enough queues have been activated shortly after each
		 * other to consider this burst as large.
		 */
		bfqd->large_burst = true;
		/* 장치 전역 플래그를 세팅 - 이후 짧은 간격으로 생성되는 큐는
		 * burst_list를 거치지 않고 bfq_handle_burst에서 바로 large burst로
		 * 판정된다(systemd 부팅, git grep처럼 프로세스를 대량 fork하는
		 * 워크로드를 인식하기 위함). */

		/*
		 * We can now mark all queues in the burst list as
		 * belonging to a large burst.
		 */
		hlist_for_each_entry(bfqq_item, &bfqd->burst_list,
				     burst_list_node)
			/* 지금까지 burst_list에 쌓여 있던(=이번 burst에서 먼저
			 * 생성된) 큐들을 모두 large-burst 멤버로 표시 - 이들은
			 * weight-raising 대상에서 제외되어 처리량 위주로 처리된다. */
			bfq_mark_bfqq_in_large_burst(bfqq_item);
		/* 방금 임계치를 채운 bfqq 자신도 large-burst 멤버로 표시. */
		bfq_mark_bfqq_in_large_burst(bfqq);

		/*
		 * From now on, and until the current burst finishes, any
		 * new queue being activated shortly after the last queue
		 * was inserted in the burst can be immediately marked as
		 * belonging to a large burst. So the burst list is not
		 * needed any more. Remove it.
		 */
		/* large_burst 플래그로 판정이 넘어갔으므로 burst_list
		 * 자체는 더 이상 쓰이지 않는다 - 각 노드를 리스트에서
		 * 제거해(hlist_del_init) bfqq들이 이후 다른 burst_list에
		 * 재사용될 수 있도록 정리한다. */
		hlist_for_each_entry_safe(pos, n, &bfqd->burst_list,
					  burst_list_node)
			hlist_del_init(&pos->burst_list_node);
	} else /*
		* Burst not yet large: add bfqq to the burst list. Do
		* not increment the ref counter for bfqq, because bfqq
		* is removed from the burst list before freeing bfqq
		* in put_queue.
		*/
		/* 아직 large burst로 확정되지 않았으므로 bfqq를 burst_list
		 * head에 추가만 해 두고, 다음 큐 생성 시 이 리스트 길이가
		 * 다시 검사된다. */
		hlist_add_head(&bfqq->burst_list_node, &bfqd->burst_list);
}

/*
 * If many queues belonging to the same group happen to be created
 * shortly after each other, then the processes associated with these
 * queues have typically a common goal. In particular, bursts of queue
 * creations are usually caused by services or applications that spawn
 * many parallel threads/processes. Examples are systemd during boot,
 * or git grep. To help these processes get their job done as soon as
 * possible, it is usually better to not grant either weight-raising
 * or device idling to their queues, unless these queues must be
 * protected from the I/O flowing through other active queues.
 *
 * In this comment we describe, firstly, the reasons why this fact
 * holds, and, secondly, the next function, which implements the main
 * steps needed to properly mark these queues so that they can then be
 * treated in a different way.
 *
 * The above services or applications benefit mostly from a high
 * throughput: the quicker the requests of the activated queues are
 * cumulatively served, the sooner the target job of these queues gets
 * completed. As a consequence, weight-raising any of these queues,
 * which also implies idling the device for it, is almost always
 * counterproductive, unless there are other active queues to isolate
 * these new queues from. If there no other active queues, then
 * weight-raising these new queues just lowers throughput in most
 * cases.
 *
 * On the other hand, a burst of queue creations may be caused also by
 * the start of an application that does not consist of a lot of
 * parallel I/O-bound threads. In fact, with a complex application,
 * several short processes may need to be executed to start-up the
 * application. In this respect, to start an application as quickly as
 * possible, the best thing to do is in any case to privilege the I/O
 * related to the application with respect to all other
 * I/O. Therefore, the best strategy to start as quickly as possible
 * an application that causes a burst of queue creations is to
 * weight-raise all the queues created during the burst. This is the
 * exact opposite of the best strategy for the other type of bursts.
 *
 * In the end, to take the best action for each of the two cases, the
 * two types of bursts need to be distinguished. Fortunately, this
 * seems relatively easy, by looking at the sizes of the bursts. In
 * particular, we found a threshold such that only bursts with a
 * larger size than that threshold are apparently caused by
 * services or commands such as systemd or git grep. For brevity,
 * hereafter we call just 'large' these bursts. BFQ *does not*
 * weight-raise queues whose creation occurs in a large burst. In
 * addition, for each of these queues BFQ performs or does not perform
 * idling depending on which choice boosts the throughput more. The
 * exact choice depends on the device and request pattern at
 * hand.
 *
 * Unfortunately, false positives may occur while an interactive task
 * is starting (e.g., an application is being started). The
 * consequence is that the queues associated with the task do not
 * enjoy weight raising as expected. Fortunately these false positives
 * are very rare. They typically occur if some service happens to
 * start doing I/O exactly when the interactive task starts.
 *
 * Turning back to the next function, it is invoked only if there are
 * no active queues (apart from active queues that would belong to the
 * same, possible burst bfqq would belong to), and it implements all
 * the steps needed to detect the occurrence of a large burst and to
 * properly mark all the queues belonging to it (so that they can then
 * be treated in a different way). This goal is achieved by
 * maintaining a "burst list" that holds, temporarily, the queues that
 * belong to the burst in progress. The list is then used to mark
 * these queues as belonging to a large burst if the burst does become
 * large. The main steps are the following.
 *
 * . when the very first queue is created, the queue is inserted into the
 *   list (as it could be the first queue in a possible burst)
 *
 * . if the current burst has not yet become large, and a queue Q that does
 *   not yet belong to the burst is activated shortly after the last time
 *   at which a new queue entered the burst list, then the function appends
 *   Q to the burst list
 *
 * . if, as a consequence of the previous step, the burst size reaches
 *   the large-burst threshold, then
 *
 *     . all the queues in the burst list are marked as belonging to a
 *       large burst
 *
 *     . the burst list is deleted; in fact, the burst list already served
 *       its purpose (keeping temporarily track of the queues in a burst,
 *       so as to be able to mark them as belonging to a large burst in the
 *       previous sub-step), and now is not needed any more
 *
 *     . the device enters a large-burst mode
 *
 * . if a queue Q that does not belong to the burst is created while
 *   the device is in large-burst mode and shortly after the last time
 *   at which a queue either entered the burst list or was marked as
 *   belonging to the current large burst, then Q is immediately marked
 *   as belonging to a large burst.
 *
 * . if a queue Q that does not belong to the burst is created a while
 *   later, i.e., not shortly after, than the last time at which a queue
 *   either entered the burst list or was marked as belonging to the
 *   current large burst, then the current burst is deemed as finished and:
 *
 *        . the large-burst mode is reset if set
 *
 *        . the burst list is emptied
 *
 *        . Q is inserted in the burst list, as Q may be the first queue
 *          in a possible new burst (then the burst list contains just Q
 *          after this step).
 */
/*
 * [한국어]
 * bfq_handle_burst - bfqq가 burst(대량 큐 동시 생성)의 일부인지 판정하고
 *                     burst 관련 상태(burst_list/large_burst)를 갱신
 *
 * @bfqd: burst_list, large_burst, last_ins_in_burst, burst_parent_entity를
 *        보유한 장치 스케줄러 데이터
 * @bfqq: 방금 활성화된(유휴 상태에서 처음 요청을 받은) bfq_queue
 * @return: 없음 (void)
 *
 * systemd 부팅이나 git grep처럼 짧은 시간 안에 여러 프로세스를 fork하는
 * 워크로드는 그만큼 짧은 시간 안에 여러 bfqq를 생성한다. 이런 "burst"에
 * 속한 큐들은 weight-raising(및 그에 따른 device idling)을 적용하면
 * 오히려 전체 처리량만 떨어뜨리므로, BFQ는 large burst로 판정된 큐들에는
 * weight-raising을 적용하지 않는다(바로 위 영어 주석 참고).
 * 판정 로직은 세 단계로 진행된다.
 * 1) 이미 burst_list에 있거나, 이미 large_burst로 표시되었거나, 최근
 *    10ms 안에 병합에서 분리(split)된 큐라면 더 볼 것 없이 그대로 종료.
 * 2) 마지막 burst 큐 삽입(last_ins_in_burst) 이후 bfq_burst_interval보다
 *    오래 지났거나, bfqq의 부모 엔티티(cgroup)가 현재 burst의 부모와
 *    다르면 "이전 burst는 끝났다"고 보고 burst 상태를 리셋한 뒤 bfqq를
 *    새 burst의 첫 큐 후보로 등록(bfq_reset_burst_list).
 * 3) 그렇지 않고 이미 large_burst 상태라면 bfqq를 즉시 large burst
 *    멤버로 표시. 아직 large가 아니라면 burst_list에 추가하며 임계치
 *    도달 여부를 검사(bfq_add_to_burst).
 * 실행 컨텍스트: 큐가 유휴에서 busy로 전이하는 경로
 * (bfq_bfqq_handle_idle_busy_switch 등)에서 bfqd->lock을 쥔 채 호출된다.
 * caller: bfqq가 idle→busy로 전이할 때 burst 판정이 필요한 상위 로직
 * (예: bfq_add_bfqq_busy 이전 단계, 큐 삽입 경로).
 * callee: bfq_reset_burst_list, bfq_mark_bfqq_in_large_burst, bfq_add_to_burst.
 *
 * 호출 체인:
 *   (큐 활성화 경로) → [bfq_handle_burst] → bfq_reset_burst_list / bfq_add_to_burst
 */
static void bfq_handle_burst(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	/*
	 * If bfqq is already in the burst list or is part of a large
	 * burst, or finally has just been split, then there is
	 * nothing else to do.
	 */
	/* bfqq->burst_list_node가 이미 어떤 hlist엔가 걸려 있다는 뜻 -
	 * 즉 이미 현재(또는 과거) burst_list의 멤버로 등록된 상태. */
	if (!hlist_unhashed(&bfqq->burst_list_node) ||
	    bfq_bfqq_in_large_burst(bfqq) ||
	    /* 이미 large burst 멤버로 표시된 큐라면 재판정이 불필요. */
	    time_is_after_eq_jiffies(bfqq->split_time +
				     msecs_to_jiffies(10)))
	    /* 협조 큐 병합에서 최근 10ms 이내에 분리(split)된 큐 - 분리
	     * 직후에는 burst 판정을 건드리지 않고 그대로 둔다. */
		return;
		/* 위 세 조건 중 하나라도 참이면 더 손댈 것이 없으므로 즉시 반환. */

	/*
	 * If bfqq's creation happens late enough, or bfqq belongs to
	 * a different group than the burst group, then the current
	 * burst is finished, and related data structures must be
	 * reset.
	 *
	 * In this respect, consider the special case where bfqq is
	 * the very first queue created after BFQ is selected for this
	 * device. In this case, last_ins_in_burst and
	 * burst_parent_entity are not yet significant when we get
	 * here. But it is easy to verify that, whether or not the
	 * following condition is true, bfqq will end up being
	 * inserted into the burst list. In particular the list will
	 * happen to contain only bfqq. And this is exactly what has
	 * to happen, as bfqq may be the first queue of the first
	 * burst.
	 */
	if (time_is_before_jiffies(bfqd->last_ins_in_burst +
	    bfqd->bfq_burst_interval) ||
	    /* 마지막으로 burst에 큐가 삽입된 시각(last_ins_in_burst)으로부터
	     * bfq_burst_interval 이상 지났다 - 더 이상 "짧은 시간 안의 연쇄
	     * 생성"으로 볼 수 없으므로 이전 burst는 종료된 것으로 간주. */
	    bfqq->entity.parent != bfqd->burst_parent_entity) {
	    /* 혹은 bfqq가 이전 burst와 다른 cgroup(부모 엔티티)에 속한다 -
	     * burst는 같은 그룹 안에서만 유효한 개념이므로 그룹이 다르면
	     * 별개의 burst로 취급해야 한다. */
		bfqd->large_burst = false;
		/* 새 burst를 시작하는 것이므로 이전 large_burst 판정은 무효화. */
		bfq_reset_burst_list(bfqd, bfqq);
		/* burst_list를 비우고 bfqq를 새 burst의 첫 멤버 후보로 등록. */
		goto end;
		/* 아래 공통 마무리(last_ins_in_burst 갱신)로 점프. */
	}

	/*
	 * If we get here, then bfqq is being activated shortly after the
	 * last queue. So, if the current burst is also large, we can mark
	 * bfqq as belonging to this large burst immediately.
	 */
	if (bfqd->large_burst) {
	/* 이미 large burst로 확정된 상태에서 bfqq가 짧은 간격 안에 도착했다 -
	 * burst_list를 거칠 필요 없이 곧바로 large burst 멤버로 확정. */
		bfq_mark_bfqq_in_large_burst(bfqq);
		/* 공통 마무리로 점프 - last_ins_in_burst만 갱신하면 된다. */
		goto end;
	}

	/*
	 * If we get here, then a large-burst state has not yet been
	 * reached, but bfqq is being activated shortly after the last
	 * queue. Then we add bfqq to the burst.
	 */
	/* 아직 large 판정 전이므로 burst_list에 bfqq를 추가하고 임계치
	 * 도달 여부를 검사(임계치 도달 시 그 함수 내부에서 large 전환). */
	bfq_add_to_burst(bfqd, bfqq);
end:
	/*
	 * At this point, bfqq either has been added to the current
	 * burst or has caused the current burst to terminate and a
	 * possible new burst to start. In particular, in the second
	 * case, bfqq has become the first queue in the possible new
	 * burst.  In both cases last_ins_in_burst needs to be moved
	 * forward.
	 */
	/* 세 경로(리셋/large 즉시 편입/burst_list 추가) 모두 공통으로
	 * "가장 최근에 burst에 큐가 삽입된 시각"을 지금으로 갱신 - 다음
	 * bfq_handle_burst 호출에서 시간 간격 판정의 기준이 된다. */
	bfqd->last_ins_in_burst = jiffies;
}

/*
 * [한국어]
 * bfq_bfqq_budget_left - bfqq에 할당된 예산 중 아직 쓰지 않고 남은 양을 계산
 *
 * @bfqq: 잔여 예산을 알고 싶은 bfq_queue
 * @return: entity->budget에서 entity->service를 뺀 값(섹터 단위) -
 *          아직 소비하지 않은 예산
 *
 * BFQ는 각 bfqq에 한 번의 서비스 라운드(in-service 구간)마다 "예산"
 * (budget, 섹터 단위)을 배정하고, 실제로 서비스된 양을 entity->service에
 * 누적한다. 이 함수는 그 차이를 구해 "이번 서비스 라운드에서 얼마나
 * 더 서비스받을 수 있는가"를 알려준다. idle→busy 전이 시 잔여 예산을
 * 재사용할지 판단하거나(bfq_bfqq_update_budg_for_activation), preemption
 * 여부를 결정하는 등 스케줄링 결정 전반에 쓰인다.
 * 실행 컨텍스트: bfqd->lock을 쥔 스케줄러 콜백 경로에서 호출되는 단순
 * 산술 함수 - 별도 동기화 불필요.
 * caller: bfq_bfqq_update_budg_for_activation 등 예산/선점 판단 경로.
 * callee: 없음.
 *
 * 호출 체인:
 *   bfq_bfqq_update_budg_for_activation 등 → [bfq_bfqq_budget_left]
 */
static int bfq_bfqq_budget_left(struct bfq_queue *bfqq)
{
	struct bfq_entity *entity = &bfqq->entity;
	/* bfqq에 내장된 스케줄링 엔티티 - budget/service 필드를 갖고 있다. */

	return entity->budget - entity->service;
	/* budget: 이번 라운드에 배정된 최대 서비스량(섹터).
	 * service: 지금까지 실제로 서비스한 양(섹터).
	 * 차이가 곧 "아직 남은 예산" - B-WF2Q+ 알고리즘이 이 값을 보고
	 * 큐를 만료시킬지, 계속 서비스할지를 판단하는 데 쓰인다. */
}

/*
 * If enough samples have been computed, return the current max budget
 * stored in bfqd, which is dynamically updated according to the
 * estimated disk peak rate; otherwise return the default max budget
 */
/*
 * [한국어]
 * bfq_max_budget - 장치에 적용할 "최대 예산" 값을 반환
 *
 * @bfqd: peak_rate 추정치와 budgets_assigned 카운터를 보유한 장치 데이터
 * @return: 아직 충분한 샘플이 쌓이지 않았으면 기본값(bfq_default_max_budget),
 *          충분하면 실측 peak_rate 기반으로 갱신된 bfqd->bfq_max_budget
 *
 * BFQ는 장치의 순간 최대 처리율(peak_rate)을 관찰해 예산 상한을 동적으로
 * 조정한다(부팅 초기처럼 표본이 부족할 때는 안전한 기본값을 쓴다).
 * budgets_assigned가 bfq_stats_min_budgets 미만이면 아직 peak_rate 추정이
 * 신뢰할 만큼 충분히 관측되지 않은 것으로 보고 고정 기본값을 사용한다.
 * 실행 컨텍스트: bfqd->lock 보유 상태에서 예산 배정 시 호출되는 단순 조회 함수.
 * caller: 큐에 새 예산을 배정하는 경로(예산 클램핑, __bfq_bfqq_recalc_budget 등).
 * callee: 없음.
 *
 * 호출 체인:
 *   (예산 배정 경로) → [bfq_max_budget]
 */
static int bfq_max_budget(struct bfq_data *bfqd)
{
	if (bfqd->budgets_assigned < bfq_stats_min_budgets)
	/* 아직 충분한 수의 예산 배정 샘플이 쌓이지 않음 - peak_rate 기반
	 * 추정치를 신뢰하기 이르므로 안전한 고정 기본값을 사용한다. */
		return bfq_default_max_budget;
	/* 충분한 샘플이 쌓였다 - 실측 peak_rate로부터 갱신된, 장치별로
	 * 최적화된 최대 예산 값을 사용한다. */
	else
		return bfqd->bfq_max_budget;
}

/*
 * Return min budget, which is a fraction of the current or default
 * max budget (trying with 1/32)
 */
/*
 * [한국어]
 * bfq_min_budget - 장치에 적용할 "최소 예산" 값을 반환
 *
 * @bfqd: budgets_assigned/bfq_max_budget을 보유한 장치 데이터
 * @return: 현재(또는 기본) 최대 예산의 1/32에 해당하는 값
 *
 * 최소 예산은 최대 예산에 종속적으로 계산된다 - 최대 예산이 peak_rate에
 * 맞춰 커지거나 작아지면 최소 예산도 비례해서 조정되어야 짧은 요청만
 * 발생시키는 큐(예: 대화형 프로세스)에도 지나치게 작지 않은 서비스
 * 슬롯을 보장할 수 있다. weight-raising 시작 시 예산을 줄이는 계산
 * (2 * bfq_min_budget(bfqd))에도 사용된다.
 * 실행 컨텍스트: bfqd->lock 보유 상태에서 호출되는 단순 조회 함수.
 * caller: bfq_update_bfqq_wr_on_rq_arrival(예산 축소 계산), 예산 초기화 경로.
 * callee: 없음.
 *
 * 호출 체인:
 *   bfq_update_bfqq_wr_on_rq_arrival 등 → [bfq_min_budget]
 */
static int bfq_min_budget(struct bfq_data *bfqd)
{
	if (bfqd->budgets_assigned < bfq_stats_min_budgets)
	/* 샘플 부족 - 고정 기본 최대 예산(bfq_default_max_budget)의 1/32을 사용. */
		return bfq_default_max_budget / 32;
	/* 샘플 충분 - 실측 기반 최대 예산(bfqd->bfq_max_budget)의 1/32을 사용. */
	else
		return bfqd->bfq_max_budget / 32;
}

/*
 * The next function, invoked after the input queue bfqq switches from
 * idle to busy, updates the budget of bfqq. The function also tells
 * whether the in-service queue should be expired, by returning
 * true. The purpose of expiring the in-service queue is to give bfqq
 * the chance to possibly preempt the in-service queue, and the reason
 * for preempting the in-service queue is to achieve one of the two
 * goals below.
 *
 * 1. Guarantee to bfqq its reserved bandwidth even if bfqq has
 * expired because it has remained idle. In particular, bfqq may have
 * expired for one of the following two reasons:
 *
 * - BFQQE_NO_MORE_REQUESTS bfqq did not enjoy any device idling
 *   and did not make it to issue a new request before its last
 *   request was served;
 *
 * - BFQQE_TOO_IDLE bfqq did enjoy device idling, but did not issue
 *   a new request before the expiration of the idling-time.
 *
 * Even if bfqq has expired for one of the above reasons, the process
 * associated with the queue may be however issuing requests greedily,
 * and thus be sensitive to the bandwidth it receives (bfqq may have
 * remained idle for other reasons: CPU high load, bfqq not enjoying
 * idling, I/O throttling somewhere in the path from the process to
 * the I/O scheduler, ...). But if, after every expiration for one of
 * the above two reasons, bfqq has to wait for the service of at least
 * one full budget of another queue before being served again, then
 * bfqq is likely to get a much lower bandwidth or resource time than
 * its reserved ones. To address this issue, two countermeasures need
 * to be taken.
 *
 * First, the budget and the timestamps of bfqq need to be updated in
 * a special way on bfqq reactivation: they need to be updated as if
 * bfqq did not remain idle and did not expire. In fact, if they are
 * computed as if bfqq expired and remained idle until reactivation,
 * then the process associated with bfqq is treated as if, instead of
 * being greedy, it stopped issuing requests when bfqq remained idle,
 * and restarts issuing requests only on this reactivation. In other
 * words, the scheduler does not help the process recover the "service
 * hole" between bfqq expiration and reactivation. As a consequence,
 * the process receives a lower bandwidth than its reserved one. In
 * contrast, to recover this hole, the budget must be updated as if
 * bfqq was not expired at all before this reactivation, i.e., it must
 * be set to the value of the remaining budget when bfqq was
 * expired. Along the same line, timestamps need to be assigned the
 * value they had the last time bfqq was selected for service, i.e.,
 * before last expiration. Thus timestamps need to be back-shifted
 * with respect to their normal computation (see [1] for more details
 * on this tricky aspect).
 *
 * Secondly, to allow the process to recover the hole, the in-service
 * queue must be expired too, to give bfqq the chance to preempt it
 * immediately. In fact, if bfqq has to wait for a full budget of the
 * in-service queue to be completed, then it may become impossible to
 * let the process recover the hole, even if the back-shifted
 * timestamps of bfqq are lower than those of the in-service queue. If
 * this happens for most or all of the holes, then the process may not
 * receive its reserved bandwidth. In this respect, it is worth noting
 * that, being the service of outstanding requests unpreemptible, a
 * little fraction of the holes may however be unrecoverable, thereby
 * causing a little loss of bandwidth.
 *
 * The last important point is detecting whether bfqq does need this
 * bandwidth recovery. In this respect, the next function deems the
 * process associated with bfqq greedy, and thus allows it to recover
 * the hole, if: 1) the process is waiting for the arrival of a new
 * request (which implies that bfqq expired for one of the above two
 * reasons), and 2) such a request has arrived soon. The first
 * condition is controlled through the flag non_blocking_wait_rq,
 * while the second through the flag arrived_in_time. If both
 * conditions hold, then the function computes the budget in the
 * above-described special way, and signals that the in-service queue
 * should be expired. Timestamp back-shifting is done later in
 * __bfq_activate_entity.
 *
 * 2. Reduce latency. Even if timestamps are not backshifted to let
 * the process associated with bfqq recover a service hole, bfqq may
 * however happen to have, after being (re)activated, a lower finish
 * timestamp than the in-service queue.	 That is, the next budget of
 * bfqq may have to be completed before the one of the in-service
 * queue. If this is the case, then preempting the in-service queue
 * allows this goal to be achieved, apart from the unpreemptible,
 * outstanding requests mentioned above.
 *
 * Unfortunately, regardless of which of the above two goals one wants
 * to achieve, service trees need first to be updated to know whether
 * the in-service queue must be preempted. To have service trees
 * correctly updated, the in-service queue must be expired and
 * rescheduled, and bfqq must be scheduled too. This is one of the
 * most costly operations (in future versions, the scheduling
 * mechanism may be re-designed in such a way to make it possible to
 * know whether preemption is needed without needing to update service
 * trees). In addition, queue preemptions almost always cause random
 * I/O, which may in turn cause loss of throughput. Finally, there may
 * even be no in-service queue when the next function is invoked (so,
 * no queue to compare timestamps with). Because of these facts, the
 * next function adopts the following simple scheme to avoid costly
 * operations, too frequent preemptions and too many dependencies on
 * the state of the scheduler: it requests the expiration of the
 * in-service queue (unconditionally) only for queues that need to
 * recover a hole. Then it delegates to other parts of the code the
 * responsibility of handling the above case 2.
 */
/*
 * [한국어]
 * bfq_bfqq_update_budg_for_activation - idle에서 busy로 전이하는 bfqq의
 *                                        예산을 갱신하고 in-service 큐를
 *                                        선점(expire)해야 하는지 판단
 *
 * @bfqd: 장치 스케줄러 데이터
 * @bfqq: 방금 idle에서 busy로 전이한 bfq_queue
 * @arrived_in_time: bfqq가 만료된 뒤 "충분히 빨리" 새 요청을 받았는지 여부
 *                    (bfq_bfqq_handle_idle_busy_switch에서 계산되어 전달됨)
 * @return: true면 호출자가 현재 in-service 큐를 만료시켜야 함(bfqq가 서비스
 *          공백(hole)을 회복할 기회를 주기 위해), false면 일반적인 예산
 *          재계산만 수행했다는 뜻
 *
 * bfqq가 (a) non_blocking_wait_rq 상태이고(즉 새 요청을 기다리다 만료된
 * 상태) (b) 그 요청이 충분히 빨리 도착했고(arrived_in_time) (c) 아직 잔여
 * 예산이 남아 있다면, 이 bfqq는 "탐욕적으로 I/O를 내는 프로세스인데
 * 우연히 idling/CPU 스케줄링 등으로 서비스 공백이 생긴 것"으로 간주한다.
 * 이 경우 예산과 타임스탬프를 "마치 만료되지 않았던 것처럼" 되돌려
 * (remaining budget을 그대로 이어받고), true를 반환해 호출자가 현재
 * in-service 큐를 즉시 만료시키고 bfqq에게 선점 기회를 주도록 한다.
 * 그렇지 않다면(정말로 오래 쉬다가 새 요청을 받은 경우 등) 일반적인
 * 만료 처리(예산을 새로 max_budget 또는 요청 크기 기준으로 재설정)만
 * 수행하고 false를 반환한다.
 * 실행 컨텍스트: bfqd->lock을 쥔 큐 활성화 경로(idle→busy 전이)에서 호출.
 * caller: bfq_bfqq_handle_idle_busy_switch.
 * callee: bfq_bfqq_budget_left, bfq_serv_to_charge, bfq_clear_bfqq_non_blocking_wait_rq.
 *
 * 호출 체인:
 *   bfq_bfqq_handle_idle_busy_switch → [bfq_bfqq_update_budg_for_activation] → bfq_bfqq_budget_left / bfq_serv_to_charge
 */
static bool bfq_bfqq_update_budg_for_activation(struct bfq_data *bfqd,
						struct bfq_queue *bfqq,
						bool arrived_in_time)
{
	struct bfq_entity *entity = &bfqq->entity;
	/* bfqq의 스케줄링 엔티티 - budget/service 필드를 이 함수에서 직접 갱신한다. */

	/*
	 * In the next compound condition, we check also whether there
	 * is some budget left, because otherwise there is no point in
	 * trying to go on serving bfqq with this same budget: bfqq
	 * would be expired immediately after being selected for
	 * service. This would only cause useless overhead.
	 */
	if (bfq_bfqq_non_blocking_wait_rq(bfqq) && arrived_in_time &&
	    /* non_blocking_wait_rq: 이 큐가 새 요청 도착을 기다리다 만료된
	     * 상태였는지 - true여야 "서비스 공백 회복" 시나리오 후보가 된다. */
	    bfq_bfqq_budget_left(bfqq) > 0) {
	    /* 잔여 예산이 실제로 남아 있어야 재사용할 의미가 있다 - 0 이하면
	     * 어차피 선택 즉시 다시 만료될 것이므로 이 분기를 탈 필요가 없다. */
		/*
		 * We do not clear the flag non_blocking_wait_rq here, as
		 * the latter is used in bfq_activate_bfqq to signal
		 * that timestamps need to be back-shifted (and is
		 * cleared right after).
		 */

		/*
		 * In next assignment we rely on that either
		 * entity->service or entity->budget are not updated
		 * on expiration if bfqq is empty (see
		 * __bfq_bfqq_recalc_budget). Thus both quantities
		 * remain unchanged after such an expiration, and the
		 * following statement therefore assigns to
		 * entity->budget the remaining budget on such an
		 * expiration.
		 */
		entity->budget = min_t(unsigned long,
				       bfq_bfqq_budget_left(bfqq),
				       bfqq->max_budget);
				       /* 예산을 "만료 시점에 남아 있던 잔여 예산"과
				        * "이 큐의 최대 예산" 중 작은 쪽으로 설정 -
				        * 서비스 공백을 회복시키되 과도한 예산
				        * 재부여는 막는다. */

		/*
		 * At this point, we have used entity->service to get
		 * the budget left (needed for updating
		 * entity->budget). Thus we finally can, and have to,
		 * reset entity->service. The latter must be reset
		 * because bfqq would otherwise be charged again for
		 * the service it has received during its previous
		 * service slot(s).
		 */
		entity->service = 0;
		/* 이전 서비스 슬롯에서 이미 소비한 service 양을 0으로 리셋 -
		 * 그렇지 않으면 다음 라운드에서 과거 서비스량이 중복으로
		 * 차감되어 bfqq가 부당하게 짧은 예산만 받게 된다. */

		return true;
		/* 호출자(bfq_bfqq_handle_idle_busy_switch)에게 "in-service 큐를
		 * 만료시켜 bfqq에게 선점 기회를 줘야 한다"고 신호. */
	}

	/*
	 * We can finally complete expiration, by setting service to 0.
	 */
	entity->service = 0;
	/* 위의 "공백 회복" 케이스가 아닌 일반적인 만료 완료 처리 - 이전 서비스
	 * 카운터를 리셋해 새 라운드를 깨끗하게 시작한다. */
	entity->budget = max_t(unsigned long, bfqq->max_budget,
			       bfq_serv_to_charge(bfqq->next_rq, bfqq));
			       /* 새 예산을 "이 큐의 기존 최대 예산"과 "다음 요청을
			        * 서비스하는 데 필요한 최소 charge" 중 큰 쪽으로
			        * 설정 - 최소한 다음 요청 하나는 이번 라운드
			        * 안에 처리될 수 있도록 보장한다. */
	bfq_clear_bfqq_non_blocking_wait_rq(bfqq);
	/* 공백 회복 시나리오가 아니므로 대기 플래그를 내려 정상 상태로 되돌린다. */
	return false;
	/* 호출자에게 "특별한 선점이 필요하지 않다"고 신호 - 일반적인 예산
	 * 재계산만 수행되었음을 뜻한다. */
}

/*
 * Return the farthest past time instant according to jiffies
 * macros.
 */
/*
 * [한국어]
 * bfq_smallest_from_now - jiffies 기준으로 표현 가능한 "가장 먼 과거" 시각을 반환
 *
 * @param: 없음
 * @return: 현재 jiffies에서 MAX_JIFFY_OFFSET을 뺀 값 - jiffies 비교 매크로
 *          (time_before/time_after 등)로 다뤄도 오버플로/언더플로 없이
 *          항상 "이미 지난 시각"으로 취급되는 안전한 하한값
 *
 * wr_start_at_switch_to_srt 같은 "아직 유효한 값이 없음"을 표현해야 하는
 * 타임스탬프 필드에 사용된다. 0이나 임의의 작은 값을 넣으면 jiffies
 * 오버플로 시 미래 시각으로 잘못 해석될 수 있으므로, jiffies 매크로가
 * 다룰 수 있는 범위 안에서 가장 먼 과거를 명시적으로 계산해 대입한다.
 * 실행 컨텍스트: 락 없이도 안전한 순수 계산 함수(jiffies 읽기만 수행).
 * caller: bfq_update_bfqq_wr_on_rq_arrival (soft-rt 전환 시
 * wr_start_at_switch_to_srt를 "무한히 먼 과거"로 초기화).
 * callee: 없음.
 *
 * 호출 체인:
 *   bfq_update_bfqq_wr_on_rq_arrival → [bfq_smallest_from_now]
 */
static unsigned long bfq_smallest_from_now(void)
{
	return jiffies - MAX_JIFFY_OFFSET;
	/* 현재 jiffies에서 표현 가능한 최대 오프셋을 빼 "항상 과거로 판정되는"
	 * 값을 만든다 - time_is_after_eq_jiffies() 같은 비교에서 절대
	 * 참이 되지 않도록(즉 "아직 도달 안 함"으로 오해되지 않도록) 하는 트릭. */
}

/*
 * [한국어]
 * bfq_update_bfqq_wr_on_rq_arrival - 새 요청 도착 시 bfqq의 weight-raising
 *                                     상태(wr_coeff, wr_cur_max_time 등)를 갱신
 *
 * @bfqd: 장치 스케줄러 데이터 (bfq_wr_coeff, bfq_wr_rt_max_time 등 전역 설정 보유)
 * @bfqq: weight-raising 상태를 갱신할 bfq_queue
 * @old_wr_coeff: 이 함수 호출 전, 즉 이번 요청 도착 이전의 wr_coeff 값
 * @wr_or_deserves_wr: 현재 이미 weight-raised 상태이거나, 새로 weight-raising을
 *                      받을 자격이 있다고 판정되었는지 여부
 * @interactive: bfqq가 "대화형(interactive)"으로 판정되었는지 여부
 *               (idle_for_long_time && 기본 가중치)
 * @in_burst: bfqq가 대량 큐 생성 burst의 일부로 판정되었는지 여부
 * @soft_rt: bfqq가 "소프트 실시간(soft real-time)" 워크로드로 판정되었는지 여부
 * @return: 없음 (void) - bfqq->wr_coeff/wr_cur_max_time/entity.budget 등을
 *          직접 갱신한다
 *
 * BFQ의 저지연(low_latency) 모드는 대화형/소프트 실시간 프로세스의 큐에
 * wr_coeff(>1)를 곱해 가중치를 일시적으로 높인다. 이 함수는 그 상태
 * 전이를 담당하는 핵심 로직으로, 크게 두 경우로 나뉜다.
 * (1) old_wr_coeff == 1 && wr_or_deserves_wr: 지금까지 가중치가 정상이던
 *     큐가 새로 weight-raising 자격을 얻은 경우 - interactive면 일반
 *     wr_coeff/기간(bfq_wr_duration)을, 아니면(soft-rt 시작) 더 큰
 *     BFQ_SOFTRT_WEIGHT_FACTOR 배율과 bfq_wr_rt_max_time을 적용한다.
 *     추가로 지연을 줄이기 위해 예산을 2*min_budget으로 캡핑한다.
 * (2) old_wr_coeff > 1: 이미 weight-raised 상태였던 큐의 상태를 갱신 -
 *     여전히 interactive면 기간을 갱신(연장)하고, in_burst로 새로
 *     판정되면 즉시 wr_coeff를 1로 내려 weight-raising을 중단하며,
 *     soft_rt 조건을 만족하면 소프트 실시간 기간으로 재충전(recharge)한다.
 * 실행 컨텍스트: bfqd->lock을 쥔 요청 삽입 경로에서 호출되며, 호출자인
 * bfq_bfqq_handle_idle_busy_switch가 bfqd->low_latency와 split_time 조건을
 * 먼저 검사한 뒤에만 이 함수를 호출한다.
 * caller: bfq_bfqq_handle_idle_busy_switch.
 * callee: bfq_wr_duration, bfq_min_budget, bfq_smallest_from_now.
 * 에러 처리: 없음 - 모든 분기가 상태 갱신으로만 끝난다.
 *
 * 호출 체인:
 *   bfq_bfqq_handle_idle_busy_switch → [bfq_update_bfqq_wr_on_rq_arrival] → bfq_wr_duration / bfq_min_budget
 */
static void bfq_update_bfqq_wr_on_rq_arrival(struct bfq_data *bfqd,
					     struct bfq_queue *bfqq,
					     unsigned int old_wr_coeff,
					     bool wr_or_deserves_wr,
					     bool interactive,
					     bool in_burst,
					     bool soft_rt)
{
	if (old_wr_coeff == 1 && wr_or_deserves_wr) {
	/* 지금까지는 가중치가 정상(1배)이었는데, 이번 요청 도착으로 새롭게
	 * weight-raising 자격을 얻은 경우 - 새로운 wr 구간을 시작한다. */
		/* start a weight-raising period */
		if (interactive) {
		/* 대화형 프로세스로 판정된 경우 - 일반적인(더 짧고, 배율이
		 * 작은) interactive weight-raising 구간을 적용한다. */
			bfqq->service_from_wr = 0;
			/* wr 구간 동안 서비스받은 누적량을 0부터 다시 세기
			 * 시작 - 이 값은 wr 구간 종료 판단(과도한 서비스를
			 * 받았는지)에 쓰인다. */
			bfqq->wr_coeff = bfqd->bfq_wr_coeff;
			/* 장치 전역 설정값(bfq_wr_coeff)으로 가중치 배율을
			 * 올린다 - B-WF2Q+ 스케줄러가 이 배율만큼 bfqq를
			 * 우선적으로 서비스하게 된다. */
			bfqq->wr_cur_max_time = bfq_wr_duration(bfqd);
			/* 이번 wr 구간이 유지될 최대 시간(jiffies)을 peak_rate
			 * 기반 추정 함수로 계산해 저장 - 이 시간이 지나면
			 * weight-raising이 해제된다(다른 함수에서 검사). */
		} else {
		/* soft real-time으로 인해 wr 자격을 얻은 경우(interactive는
		 * 아님) - 배율/기간이 다른 별도의 wr 구간을 시작한다. */
			/*
			 * No interactive weight raising in progress
			 * here: assign minus infinity to
			 * wr_start_at_switch_to_srt, to make sure
			 * that, at the end of the soft-real-time
			 * weight raising periods that is starting
			 * now, no interactive weight-raising period
			 * may be wrongly considered as still in
			 * progress (and thus actually started by
			 * mistake).
			 */
			bfqq->wr_start_at_switch_to_srt =
				bfq_smallest_from_now();
			/* "직전에 interactive wr이 진행 중이었다"는 오해를
			 * 방지하기 위해, 표현 가능한 가장 먼 과거로 설정해
			 * 두어 이후 비교에서 항상 "이미 지난 시각"으로
			 * 취급되게 한다. */
			bfqq->wr_coeff = bfqd->bfq_wr_coeff *
				BFQ_SOFTRT_WEIGHT_FACTOR;
			/* soft-rt 전용 배율(BFQ_SOFTRT_WEIGHT_FACTOR)을 기본
			 * wr_coeff에 추가로 곱해 더 공격적으로 가중치를
			 * 높인다 - 소프트 실시간 워크로드는 지연에 더
			 * 민감하다고 가정. */
			bfqq->wr_cur_max_time =
				bfqd->bfq_wr_rt_max_time;
			/* soft-rt 전용 최대 지속 시간(bfq_wr_rt_max_time)을
			 * 적용 - 일반 interactive wr 기간과 별도로 관리된다. */
		}

		/*
		 * If needed, further reduce budget to make sure it is
		 * close to bfqq's backlog, so as to reduce the
		 * scheduling-error component due to a too large
		 * budget. Do not care about throughput consequences,
		 * but only about latency. Finally, do not assign a
		 * too small budget either, to avoid increasing
		 * latency by causing too frequent expirations.
		 */
		bfqq->entity.budget = min_t(unsigned long,
					    bfqq->entity.budget,
					    2 * bfq_min_budget(bfqd));
		/* wr이 막 시작된 큐는 지연을 최소화하는 것이 목표이므로,
		 * 예산을 "최소 예산의 2배"를 넘지 않도록 캡핑 - 지나치게
		 * 큰 예산이 스케줄링 오차(다른 큐 대비 지연)를 키우는 것을
		 * 막으면서도, 너무 작게 잡아 잦은 만료를 유발하지 않도록
		 * 하한(min_budget 기반)도 암묵적으로 유지한다. */
	} else if (old_wr_coeff > 1) {
	/* 이미 이전부터 weight-raised 상태였던 큐 - 이번 요청 도착을 계기로
	 * 그 상태를 계속 유지할지, 연장할지, 중단할지를 재평가한다. */
		if (interactive) { /* update wr coeff and duration */
		/* 여전히(혹은 새로) interactive로 판정 - 일반 wr 배율과
		 * 기간을 다시 적용해 사실상 구간을 연장하는 효과를 낸다. */
			bfqq->wr_coeff = bfqd->bfq_wr_coeff;
			/* soft-rt 배율이 적용되어 있었을 수도 있으므로, 여기서
			 * 다시 일반 배율로 되돌려(또는 재확인하여) 설정. */
			bfqq->wr_cur_max_time = bfq_wr_duration(bfqd);
			/* 최신 peak_rate 추정치를 반영해 wr 지속 시간을
			 * 다시 계산 - 장치 성능이 변했다면 기간도 그에 맞춰
			 * 조정된다. */
		} else if (in_burst)
			bfqq->wr_coeff = 1;
			/* 대량 burst의 일원으로 새로 판정되었다 - 이런 큐는
			 * weight-raising 대상에서 제외해야 하므로(파일 상단
			 * 큰 주석 참고) 즉시 배율을 1로 되돌려 wr을 중단한다. */
		else if (soft_rt) {
		/* interactive도 burst도 아니지만 soft real-time 조건을
		 * 만족 - soft-rt 기간으로 전환하거나 재충전한다. */
			/*
			 * The application is now or still meeting the
			 * requirements for being deemed soft rt.  We
			 * can then correctly and safely (re)charge
			 * the weight-raising duration for the
			 * application with the weight-raising
			 * duration for soft rt applications.
			 *
			 * In particular, doing this recharge now, i.e.,
			 * before the weight-raising period for the
			 * application finishes, reduces the probability
			 * of the following negative scenario:
			 * 1) the weight of a soft rt application is
			 *    raised at startup (as for any newly
			 *    created application),
			 * 2) since the application is not interactive,
			 *    at a certain time weight-raising is
			 *    stopped for the application,
			 * 3) at that time the application happens to
			 *    still have pending requests, and hence
			 *    is destined to not have a chance to be
			 *    deemed soft rt before these requests are
			 *    completed (see the comments to the
			 *    function bfq_bfqq_softrt_next_start()
			 *    for details on soft rt detection),
			 * 4) these pending requests experience a high
			 *    latency because the application is not
			 *    weight-raised while they are pending.
			 */
			if (bfqq->wr_cur_max_time !=
				bfqd->bfq_wr_rt_max_time) {
			/* 아직 soft-rt 전용 기간으로 전환되지 않은 상태(예:
			 * 지금까지는 interactive wr 기간을 쓰고 있었음) -
			 * 이번에 soft-rt 기간으로 전환한다. */
				bfqq->wr_start_at_switch_to_srt =
					bfqq->last_wr_start_finish;
				/* interactive wr이 시작되었던 시각을 기록해
				 * 두어, 이후 "이 soft-rt 구간이 끝났을 때
				 * interactive wr로 되돌아가야 하는지"
				 * (switch_back_to_interactive_wr) 판단의
				 * 기준으로 삼는다. */

				bfqq->wr_cur_max_time =
					bfqd->bfq_wr_rt_max_time;
				/* wr 지속 시간을 soft-rt 전용 값으로 교체. */
				bfqq->wr_coeff = bfqd->bfq_wr_coeff *
					BFQ_SOFTRT_WEIGHT_FACTOR;
				/* 배율도 soft-rt 전용 배율로 교체(재충전). */
			}
			bfqq->last_wr_start_finish = jiffies;
			/* soft-rt 조건을 계속 만족하는 한(위 if 분기를 타지
			 * 않는 경우에도) "마지막으로 wr이 갱신된 시각"은
			 * 매번 지금 시각으로 갱신 - 이 시각 + wr_cur_max_time이
			 * wr 만료 시점 판단 기준이 되므로, 매 요청마다 갱신하면
			 * 사실상 soft-rt 큐가 계속 활동하는 한 wr이 만료되지
			 * 않고 유지된다. */
		}
	}
}

/*
 * [한국어]
 * bfq_bfqq_idle_for_long_time - bfqq가 "충분히 오래" idle 상태였는지 판정
 *
 * @bfqd: bfq_wr_min_idle_time 설정값을 보유한 장치 데이터
 * @bfqq: idle 지속 시간을 판정할 bfq_queue
 * @return: dispatch 중인 요청이 없고(dispatched == 0), 마지막 예산 만료
 *          시각(budget_timeout)으로부터 bfq_wr_min_idle_time 이상 지났으면 true
 *
 * 대화형(interactive) 프로세스는 전형적으로 "요청을 조금 보내고 - 사용자
 * 입력이나 생각 시간만큼 오래 쉬고 - 다시 요청을 보내는" 패턴을 보인다.
 * 이 함수는 그 "오래 쉬었는지"를 판정하는 데 쓰이며, 그 결과(*interactive
 * 판정의 한 요소)는 bfq_bfqq_handle_idle_busy_switch에서 weight-raising
 * 자격 판정에 직접 사용된다. dispatched != 0이면(아직 장치에 전달되어
 * 완료를 기다리는 요청이 있으면) 진짜로 idle이었다고 보기 어려우므로
 * 무조건 false를 반환한다.
 * 실행 컨텍스트: bfqd->lock을 쥔 큐 활성화 경로에서 호출되는 순수 판정 함수.
 * caller: bfq_bfqq_handle_idle_busy_switch (idle_for_long_time 계산),
 * burst 멤버십 해제 조건 판정 등.
 * callee: 없음 (jiffies 비교만 수행).
 *
 * 호출 체인:
 *   bfq_bfqq_handle_idle_busy_switch → [bfq_bfqq_idle_for_long_time]
 */
static bool bfq_bfqq_idle_for_long_time(struct bfq_data *bfqd,
					struct bfq_queue *bfqq)
{
	return bfqq->dispatched == 0 &&
	       /* 아직 장치에 전달되어 완료되지 않은 요청이 없어야 한다 -
	        * 하나라도 남아 있으면 이 큐는 여전히 "활동 중"으로 봐야
	        * 하므로 idle 판정에서 제외한다. */
		time_is_before_jiffies(
			bfqq->budget_timeout +
			bfqd->bfq_wr_min_idle_time);
			/* 마지막 예산 만료 시각(budget_timeout)에 최소 idle
			 * 시간(bfq_wr_min_idle_time)을 더한 시점이 이미
			 * 지났는지 검사 - 지났다면 "충분히 오래 쉬었다"고
			 * 판정해 대화형 패턴의 근거로 삼는다. */
}


/*
 * Return true if bfqq is in a higher priority class, or has a higher
 * weight than the in-service queue.
 */
/*
 * [한국어]
 * bfq_bfqq_higher_class_or_weight - bfqq가 in-service 큐보다 더 높은
 *                                    우선순위 클래스이거나 더 높은 가중치인지 비교
 *
 * @bfqq: 우선순위/가중치를 비교할 대상(방금 활성화된) bfq_queue
 * @in_serv_bfqq: 현재 장치에서 서비스 중인 bfq_queue
 * @return: bfqq의 ioprio_class가 더 높거나(수치가 더 작을수록 높은 클래스),
 *          같은 클래스 내에서 유효 가중치가 더 크면 true
 *
 * BFQ의 ioprio_class는 RT(real-time) < BE(best-effort) < IDLE 순서로
 * 수치가 작을수록 우선순위가 높다. 클래스가 다르면 클래스 비교만으로
 * 결정하고, 같은 클래스라면(더 정확히는 같은 부모 엔티티를 공유하면)
 * entity.weight를 직접 비교하며, 부모가 다르면(cgroup 계층이 다르면)
 * 각자의 최상위(부모) 엔티티 가중치로 비교한다. 이 판정 결과는
 * bfq_bfqq_handle_idle_busy_switch에서 "in-service 큐를 선점해도 되는가"를
 * 결정하는 조건 중 하나로 쓰인다 - 더 높은 우선순위/가중치를 가진 큐가
 * 새로 활성화되면 즉시 선점할 가치가 있기 때문이다.
 * 실행 컨텍스트: bfqd->lock을 쥔 큐 활성화 경로에서 호출되는 순수 비교 함수.
 * caller: bfq_bfqq_handle_idle_busy_switch (preemption 조건 검사).
 * callee: 없음.
 *
 * 호출 체인:
 *   bfq_bfqq_handle_idle_busy_switch → [bfq_bfqq_higher_class_or_weight]
 */
static bool bfq_bfqq_higher_class_or_weight(struct bfq_queue *bfqq,
					    struct bfq_queue *in_serv_bfqq)
{
	int bfqq_weight, in_serv_weight;
	/* 비교에 사용할 두 큐의 유효 가중치를 담을 임시 변수 - 클래스가
	 * 같을 때만 실제로 계산되어 쓰인다. */

	if (bfqq->ioprio_class < in_serv_bfqq->ioprio_class)
		/* ioprio_class는 숫자가 작을수록 높은 우선순위(RT가 가장
		 * 작음) - bfqq가 in-service 큐보다 더 높은 클래스라면
		 * 가중치를 볼 것도 없이 즉시 true. */
		return true;

	/* 두 큐가 같은 부모 엔티티(같은 cgroup 서비스 트리)를 공유한다 -
	 * 이 경우 각 큐 자신의 entity.weight를 직접 비교하면 된다. */
	if (in_serv_bfqq->entity.parent == bfqq->entity.parent) {
		bfqq_weight = bfqq->entity.weight;
		/* bfqq 자신의 유효 가중치(weight-raising이 적용된 경우
		 * 이미 반영된 값). */
		in_serv_weight = in_serv_bfqq->entity.weight;
		/* in-service 큐의 유효 가중치. */
	} else {
	/* 두 큐가 서로 다른 cgroup(부모 엔티티)에 속한다 - 이 경우 큐
	 * 자체의 가중치만으로는 계층 구조상 공정한 비교가 안 되므로,
	 * 가능하면 각자의 최상위 그룹 가중치로 비교한다. */
		if (bfqq->entity.parent)
			bfqq_weight = bfqq->entity.parent->weight;
			/* bfqq가 cgroup에 속해 있다면 그 부모(그룹) 엔티티의
			 * 가중치를 대표값으로 사용. */
		else
			bfqq_weight = bfqq->entity.weight;
			/* 최상위(루트) 그룹 소속이라 parent가 없다면 큐
			 * 자신의 가중치를 그대로 사용. */
		if (in_serv_bfqq->entity.parent)
			in_serv_weight = in_serv_bfqq->entity.parent->weight;
			/* in-service 큐도 마찬가지로 부모 그룹 가중치를 사용. */
		else
			in_serv_weight = in_serv_bfqq->entity.weight;
			/* 부모가 없다면 큐 자신의 가중치 사용. */
	}

	return bfqq_weight > in_serv_weight;
	/* 최종적으로 계산된 두 가중치를 비교 - bfqq 쪽이 엄격히 크면
	 * "더 높은 가중치"로 판정해 true 반환. */
}

/*
 * Get the index of the actuator that will serve bio.
 */
/*
 * [한국어]
 * bfq_actuator_index - bio가 속할 액추에이터(독립 접근 영역) 인덱스를 조회
 *
 * @bfqd: sector[]/nr_sectors[]/num_actuators 배열을 보유한 장치 스케줄러 데이터
 * @bio: 어느 액추에이터 영역에 속하는지 판정할 대상 bio
 * @return: bio의 마지막 섹터가 속하는 액추에이터의 인덱스(0..num_actuators-1).
 *          액추에이터가 1개(또는 0개) 뿐이면 항상 0. 범위를 못 찾으면
 *          경고를 남기고 0을 반환(안전한 폴백)
 *
 * 멀티 액추에이터(multi-actuator) HDD처럼 하나의 블록 장치가 물리적으로
 * 독립된 여러 액추에이터(각각 자신만의 섹터 범위를 담당)로 구성된 경우,
 * BFQ는 액추에이터별로 별도의 스케줄링 상태(별도 bfq_queue, 별도
 * in-service 큐 등)를 유지해야 한다. 이 함수는 bio가 커버하는 섹터
 * 범위(끝 섹터 기준)를 bfqd->sector[i]~sector[i]+nr_sectors[i] 구간들과
 * 비교해 어느 액추에이터에 속하는지 찾아낸다. 단일 액추에이터 장치
 * (num_actuators == 1, 대다수의 SSD/NVMe 포함)에서는 탐색 없이 바로 0을
 * 반환해 오버헤드를 없앤다.
 * 실행 컨텍스트: bio를 다루는 삽입/병합 경로에서 호출되며 별도 락 없이
 * 읽기 전용으로 bfqd의 정적 배열(sector/nr_sectors)만 참조한다.
 * caller: bfq_bfqq_handle_idle_busy_switch, bfq_init_rq, bfq_get_queue,
 * bfq_insert_request 등 bio/rq로부터 액추에이터를 알아내야 하는 모든 경로.
 * callee: bio_end_sector, WARN_ONCE.
 *
 * 호출 체인:
 *   bfq_bfqq_handle_idle_busy_switch/bfq_insert_request 등 → [bfq_actuator_index]
 */
static unsigned int bfq_actuator_index(struct bfq_data *bfqd, struct bio *bio)
{
	unsigned int i;
	/* sector 범위 배열을 순회하는 인덱스. */
	sector_t end;
	/* bio가 커버하는 마지막 섹터(끝 섹터 - 1) - 이 값이 어느 액추에이터의
	 * 담당 범위에 들어가는지로 판정한다. */

	/* no search needed if one or zero ranges present */
	if (bfqd->num_actuators == 1)
		/* 액추에이터가 하나뿐인 장치(대부분의 SSD/NVMe 포함) - 범위
		 * 탐색이 무의미하므로 즉시 0(유일한 인덱스)을 반환. */
		return 0;

	/* bio_end_sector(bio) gives the sector after the last one */
	/* bio_end_sector()는 "마지막 섹터 다음" 값을 주므로 1을 빼서 실제
	 * bio가 다루는 마지막 섹터 번호를 구한다. */
	end = bio_end_sector(bio) - 1;

	/* 등록된 모든 액추에이터의 섹터 범위를 순서대로 검사 - 액추에이터
	 * 개수는 보통 2~수 개 수준이라 선형 탐색으로 충분하다. */
	for (i = 0; i < bfqd->num_actuators; i++) {
		if (end >= bfqd->sector[i] &&
		    /* end가 i번째 액추에이터의 시작 섹터 이상이고, 그 범위
		     * (시작 + 섹터 수) 안에 들어가면 이 bio는 액추에이터 i가
		     * 담당하는 영역에 속한다. */
		    end < bfqd->sector[i] + bfqd->nr_sectors[i])
			return i;
	}

	/* 어떤 액추에이터 범위에도 속하지 않는 섹터 - 정상적으로는 발생하면
	 * 안 되는 상황(설정 오류 또는 장치 topology 불일치)이므로 커널
	 * 로그에 한 번만 경고를 남긴다. */
	WARN_ONCE(true,
		  "bfq_actuator_index: bio sector out of ranges: end=%llu\n",
		  end);
	return 0;
	/* 안전한 폴백으로 0번 액추에이터를 반환해 크래시 대신 계속 동작하게 한다. */
}

static bool bfq_better_to_idle(struct bfq_queue *bfqq);
/* [한국어] 전방 선언 - 실제 정의(및 idling 필요 여부에 대한 상세 주석)는
 * 파일 뒤쪽에 있다. 아래 bfq_bfqq_handle_idle_busy_switch가 "예산 선점이
 * 필요 없더라도 in-service 큐를 위한 idling을 계속할 이유가 있는지"를
 * preemption 최종 판단에서 참조해야 하므로, 정의보다 앞서 선언만 해 둔다. */

/*
 * [한국어]
 * bfq_bfqq_handle_idle_busy_switch - bfqq가 idle에서 busy로 전이할 때
 *      weight-raising/burst/in-large-burst 상태를 갱신하고, 필요하면
 *      in-service 큐를 선점(expire)
 *
 * @bfqd: 장치 스케줄러 데이터
 * @bfqq: 방금 새 요청 rq를 받아 idle에서 busy로 전이하는 bfq_queue
 * @old_wr_coeff: 이번 전이 이전의 wr_coeff 값(호출 후 실제로 바뀌었는지
 *                비교하기 위해 호출자가 미리 저장해 전달)
 * @rq: 방금 도착해 bfqq를 busy로 만든 요청 - bio를 통해 액추에이터 인덱스를
 *      알아내는 데도 쓰인다
 * @interactive: [출력 파라미터] 이 함수가 계산한 "bfqq가 대화형인지" 판정
 *               결과를 호출자에게 돌려준다
 * @return: 없음(void) - 부수효과로 bfqq의 각종 상태와, 필요 시 in-service
 *          큐의 만료를 유발한다
 *
 * BFQ에서 idle→busy 전이는 "이 큐가 대화형/소프트 실시간/burst 중 어디에
 * 해당하는가"를 다시 평가할 유일한 시점이다. 이 함수는 (1) idle 지속
 * 시간과 도착 타이밍으로 soft_rt/interactive 여부를 계산하고, (2) 그
 * 결과로 weight-raising 자격(wr_or_deserves_wr)을 계산해
 * bfq_update_bfqq_wr_on_rq_arrival에 위임하며, (3) 오래 idle 상태였다면
 * burst 멤버십을 해제하고, (4) bfqq를 busy 큐 목록에 편입시킨 뒤, (5)
 * 마지막으로 "현재 in-service 큐를 선점할 필요가 있는가"를 판단해
 * 필요하면 bfq_bfqq_expire를 호출한다.
 * 실행 컨텍스트: 요청 삽입 경로(bfq_insert_request 계열)에서 bfqd->lock을
 * 쥔 채 호출된다. 이 함수 자체는 재진입하지 않으며, 호출 중 lock을
 * 놓지 않는다.
 * caller: 요청이 삽입되어 큐가 idle에서 busy로 바뀌는 지점(예:
 * __bfq_insert_request 계열).
 * callee: bfq_bfqq_idle_for_long_time, bfq_actuator_index,
 * bfq_bfqq_update_budg_for_activation, bfq_update_bfqq_wr_on_rq_arrival,
 * bfq_add_bfqq_busy, bfq_bfqq_higher_class_or_weight, bfq_better_to_idle,
 * next_queue_may_preempt, bfq_bfqq_expire.
 * 에러 처리: 별도 에러 경로 없음(정책 결정 함수).
 *
 * 호출 체인:
 *   (요청 삽입 경로) → [bfq_bfqq_handle_idle_busy_switch] → bfq_update_bfqq_wr_on_rq_arrival / bfq_bfqq_expire
 */
static void bfq_bfqq_handle_idle_busy_switch(struct bfq_data *bfqd,
					     struct bfq_queue *bfqq,
					     int old_wr_coeff,
					     struct request *rq,
					     bool *interactive)
{
	bool soft_rt, in_burst,	wr_or_deserves_wr,
		/* idle 상태가 "충분히 길었는지" - interactive 판정과, 아래의
		 * 오래된 burst 멤버십 해제 판단 양쪽에 쓰인다. */
		bfqq_wants_to_preempt,
		idle_for_long_time = bfq_bfqq_idle_for_long_time(bfqd, bfqq),
		/*
		 * See the comments on
		 * bfq_bfqq_update_budg_for_activation for
		 * details on the usage of the next variable.
		 */
		arrived_in_time =  blk_time_get_ns() <=
			bfqq->ttime.last_end_request +
			bfqd->bfq_slice_idle * 3;
			/* 마지막 요청 완료 시각으로부터 "slice_idle의 3배" 이내에
			 * 새 요청이 도착했는지 - 짧으면 "탐욕적으로 I/O를
			 * 내다가 우연히 공백이 생긴 프로세스"로 간주해 서비스
			 * 공백 회복(budg_for_activation) 대상이 될 수 있다. */
	unsigned int act_idx = bfq_actuator_index(bfqd, rq->bio);
	/* 이 요청이 속하는 액추에이터 인덱스 - 멀티 액추에이터 장치에서
	 * bic->bfqq_data[]를 액추에이터별로 분리해서 참조하기 위해 필요. */
	bool bfqq_non_merged_or_stably_merged =
		bfqq->bic || RQ_BIC(rq)->bfqq_data[act_idx].stably_merged;
		/* bfqq->bic가 non-NULL이면 bfqq가 아직 다른 큐와 병합되지
		 * 않은(merge 안 된) 정상 큐라는 뜻. bic가 NULL이라도(즉
		 * 현재는 협조 큐 병합으로 다른 bfqq에 흡수된 상태라도)
		 * 이 rq를 발생시킨 io_context의 병합 기록이
		 * "stably_merged"(안정적 병합, 우연이 아니라 반복적으로
		 * 병합되는 관계)라면 예외적으로 weight-raising 대상에
		 * 포함시킨다 - 아래 wr_or_deserves_wr 계산에서 사용. */

	/*
	 * bfqq deserves to be weight-raised if:
	 * - it is sync,
	 * - it does not belong to a large burst,
	 * - it has been idle for enough time or is soft real-time,
	 * - is linked to a bfq_io_cq (it is not shared in any sense),
	 * - has a default weight (otherwise we assume the user wanted
	 *   to control its weight explicitly)
	 */
	in_burst = bfq_bfqq_in_large_burst(bfqq);
	/* 이 큐가 이미 large burst의 멤버로 표시되어 있는지 - burst 멤버는
	 * 아래 soft_rt/interactive 판정과 무관하게 weight-raising에서
	 * 제외되어야 한다. */
	soft_rt = bfqd->bfq_wr_max_softrt_rate > 0 &&
		!BFQQ_TOTALLY_SEEKY(bfqq) &&
		!in_burst &&
		time_is_before_jiffies(bfqq->soft_rt_next_start) &&
		bfqq->dispatched == 0 &&
		bfqq->entity.new_weight == 40;
		/* soft real-time 판정 - (1) 소프트 rt 감지 기능이 켜져 있고
		 * (bfq_wr_max_softrt_rate > 0), (2) seek 위주 워크로드가
		 * 아니며(BFQQ_TOTALLY_SEEKY는 seek_history == -1), (3) burst
		 * 멤버가 아니고, (4) 다음 soft-rt 판정 가능 시각(soft_rt_next_start,
		 * 최근 관측된 처리율로부터 계산됨)이 이미 지났고, (5) 아직
		 * dispatch 중인 요청이 없으며, (6) 사용자가 가중치를 직접
		 * 바꾸지 않은 기본 가중치(40)인 경우에만 soft-rt로 인정한다. */
	*interactive = !in_burst && idle_for_long_time &&
		bfqq->entity.new_weight == 40;
		/* interactive 판정 - burst 멤버가 아니고, 충분히 오래
		 * idle이었으며, 가중치를 사용자가 커스텀하지 않은 기본값(40)일
		 * 때만 대화형으로 간주한다(사용자가 명시적으로 가중치를
		 * 바꿨다면 자동 weight-raising 로직을 적용하지 않는다는 뜻). */
	/*
	 * Merged bfq_queues are kept out of weight-raising
	 * (low-latency) mechanisms. The reason is that these queues
	 * are usually created for non-interactive and
	 * non-soft-real-time tasks. Yet this is not the case for
	 * stably-merged queues. These queues are merged just because
	 * they are created shortly after each other. So they may
	 * easily serve the I/O of an interactive or soft-real time
	 * application, if the application happens to spawn multiple
	 * processes. So let also stably-merged queued enjoy weight
	 * raising.
	 */
	wr_or_deserves_wr = bfqd->low_latency &&
		(bfqq->wr_coeff > 1 ||
		 (bfq_bfqq_sync(bfqq) && bfqq_non_merged_or_stably_merged &&
		  (*interactive || soft_rt)));
		  /* 최종 weight-raising 대상 판정 - 저지연 모드가 켜져 있고,
		   * 그리고 (이미 wr 중이거나) 또는 (동기 큐이고, 일반적으로
		   * 병합되지 않았거나 안정적으로 병합된 큐이며, interactive나
		   * soft_rt 중 하나를 만족)이면 wr 대상으로 인정한다. 이
		   * 결과는 바로 아래 bfq_update_bfqq_wr_on_rq_arrival에
		   * 그대로 전달된다. */

	/*
	 * Using the last flag, update budget and check whether bfqq
	 * may want to preempt the in-service queue.
	 */
	bfqq_wants_to_preempt =
		bfq_bfqq_update_budg_for_activation(bfqd, bfqq,
						    arrived_in_time);
		/* 예산을 갱신하면서, bfqq가 "서비스 공백을 회복하기 위해
		 * in-service 큐를 선점하고 싶어하는지" 여부를 받아온다 -
		 * 아래 preemption 최종 판단의 첫 번째 조건으로 쓰인다. */

	/*
	 * If bfqq happened to be activated in a burst, but has been
	 * idle for much more than an interactive queue, then we
	 * assume that, in the overall I/O initiated in the burst, the
	 * I/O associated with bfqq is finished. So bfqq does not need
	 * to be treated as a queue belonging to a burst
	 * anymore. Accordingly, we reset bfqq's in_large_burst flag
	 * if set, and remove bfqq from the burst list if it's
	 * there. We do not decrement burst_size, because the fact
	 * that bfqq does not need to belong to the burst list any
	 * more does not invalidate the fact that bfqq was created in
	 * a burst.
	 */
	if (likely(!bfq_bfqq_just_created(bfqq)) &&
	    /* 방금 막 생성된 큐(bfq_get_queue 직후)가 아니어야 한다 - 갓
	     * 생성된 큐는 애초에 burst 판정 자체가 이제 막 이뤄지는
	     * 중이므로 여기서 조기에 burst를 해제하면 안 된다. */
	    idle_for_long_time &&
	    /* 충분히 오래 idle이었어야 "burst의 초기 I/O가 이미 끝났다"고
	     * 볼 수 있다. */
	    time_is_before_jiffies(
		    bfqq->budget_timeout +
		    msecs_to_jiffies(10000))) {
		    /* 마지막 예산 만료 후 10초 이상 지났다면, interactive
		     * 판정 기준(bfq_wr_min_idle_time, 보통 훨씬 짧음)보다도
		     * 훨씬 오래 쉰 것이므로 burst의 일부로 계속 취급하는
		     * 것은 부적절하다고 본다. */
		hlist_del_init(&bfqq->burst_list_node);
		/* 혹시 아직 burst_list에 남아 있었다면(large 판정 전 단계)
		 * 리스트에서 제거 - 더 이상 burst의 활성 멤버가 아님. */
		bfq_clear_bfqq_in_large_burst(bfqq);
		/* large_burst 멤버 플래그도 해제 - burst_size는 감소시키지
		 * 않는다(이 큐가 "burst에서 태어났다"는 과거 사실 자체는
		 * 바뀌지 않으므로, 통계적 의미의 burst_size는 그대로 둔다). */
	}

	bfq_clear_bfqq_just_created(bfqq);
	/* "방금 생성됨" 플래그를 내린다 - 이 시점 이후로는 이 bfqq가 최소
	 * 한 번은 idle→busy 전이(즉 실제 I/O 활동)를 겪었다는 뜻이 되어,
	 * 다음 번 idle→busy 전이부터는 위 burst-해제 로직이 정상 동작한다. */

	if (bfqd->low_latency) {
	/* 저지연(weight-raising) 모드가 켜져 있을 때만 wr 상태 갱신 로직을
	 * 수행 - 꺼져 있으면 아래 블록 전체를 건너뛴다. */
		if (unlikely(time_is_after_jiffies(bfqq->split_time)))
			/* wraparound */
			/* split_time이 "미래"로 보인다 - jiffies가 오버플로되어
			 * 감싸 돈(wraparound) 것으로 판단, 비교가 깨지지
			 * 않도록 split_time을 현재 기준으로 재보정한다. */
			bfqq->split_time =
				jiffies - bfqd->bfq_wr_min_idle_time - 1;

		/* 협조 큐 병합에서 분리(split)된 시각으로부터 최소
		 * idle 시간이 지났는지 검사 - 너무 최근에 분리된
		 * 큐라면(아직 병합 이력의 영향권) wr 상태를 성급하게
		 * 바꾸지 않기 위해 이 블록 전체를 건너뛴다. */
		if (time_is_before_jiffies(bfqq->split_time +
					   bfqd->bfq_wr_min_idle_time)) {
			/* [한국어] 실제 wr_coeff/wr_cur_max_time 갱신은 이 함수에
			 * 위임한다. old_wr_coeff를 함께 넘기는 이유는, 그 안에서
			 * "WR을 새로 시작하는 것인지(1 -> N) 이미 진행 중이던
			 * WR을 이어가는 것인지"를 구분해야 wr_busy_queues 카운터가
			 * 이중 증가하지 않기 때문이다. in_burst/soft_rt는 각각
			 * "large burst의 일원이라 WR 대상에서 제외" / "soft
			 * real-time으로 판정되어 더 긴 wr_cur_max_time을 부여"라는
			 * 상반된 결정을 유발하는 입력이다. */
			bfq_update_bfqq_wr_on_rq_arrival(bfqd, bfqq,
							 old_wr_coeff,
							 wr_or_deserves_wr,
							 *interactive,
							 in_burst,
							 soft_rt);

			/* 위 호출로 실제 wr_coeff가 바뀌었다면 -
			 * entity의 유효 가중치가 달라졌으므로 상위
			 * 스케줄러가 이를 인지하도록 표시해야 한다. */
			if (old_wr_coeff != bfqq->wr_coeff)
				bfqq->entity.prio_changed = 1;
				/* B-WF2Q+ 트리에서 다음에 이 entity를 다룰 때
				 * 가중치를 다시 계산하도록 하는 플래그. */
		}
	}

	bfqq->last_idle_bklogged = jiffies;
	/* "마지막으로 idle 상태에서 backlog(대기 요청)가 생긴 시각"을 갱신 -
	 * service_from_backlogged 계산 및 이후 idle 판정의 기준점으로 쓰인다. */
	bfqq->service_from_backlogged = 0;
	/* 새 backlog 구간이 시작되므로 그 구간 동안 서비스된 양 카운터를 리셋. */
	bfq_clear_bfqq_softrt_update(bfqq);
	/* soft-rt 재판정이 필요하다는 플래그를 내린다 - 이번 전이에서 이미
	 * soft_rt 여부를 판정했으므로 더 이상 "갱신 대기" 상태가 아니다. */

	bfq_add_bfqq_busy(bfqq);
	/* bfqq를 실제로 busy 큐 목록/서비스 트리에 편입시킨다 - 이 시점부터
	 * B-WF2Q+ 스케줄러가 이 큐를 서비스 후보로 고려하기 시작한다. */

	/*
	 * Expire in-service queue if preemption may be needed for
	 * guarantees or throughput. As for guarantees, we care
	 * explicitly about two cases. The first is that bfqq has to
	 * recover a service hole, as explained in the comments on
	 * bfq_bfqq_update_budg_for_activation(), i.e., that
	 * bfqq_wants_to_preempt is true. However, if bfqq does not
	 * carry time-critical I/O, then bfqq's bandwidth is less
	 * important than that of queues that carry time-critical I/O.
	 * So, as a further constraint, we consider this case only if
	 * bfqq is at least as weight-raised, i.e., at least as time
	 * critical, as the in-service queue.
	 *
	 * The second case is that bfqq is in a higher priority class,
	 * or has a higher weight than the in-service queue. If this
	 * condition does not hold, we don't care because, even if
	 * bfqq does not start to be served immediately, the resulting
	 * delay for bfqq's I/O is however lower or much lower than
	 * the ideal completion time to be guaranteed to bfqq's I/O.
	 *
	 * In both cases, preemption is needed only if, according to
	 * the timestamps of both bfqq and of the in-service queue,
	 * bfqq actually is the next queue to serve. So, to reduce
	 * useless preemptions, the return value of
	 * next_queue_may_preempt() is considered in the next compound
	 * condition too. Yet next_queue_may_preempt() just checks a
	 * simple, necessary condition for bfqq to be the next queue
	 * to serve. In fact, to evaluate a sufficient condition, the
	 * timestamps of the in-service queue would need to be
	 * updated, and this operation is quite costly (see the
	 * comments on bfq_bfqq_update_budg_for_activation()).
	 *
	 * As for throughput, we ask bfq_better_to_idle() whether we
	 * still need to plug I/O dispatching. If bfq_better_to_idle()
	 * says no, then plugging is not needed any longer, either to
	 * boost throughput or to perserve service guarantees. Then
	 * the best option is to stop plugging I/O, as not doing so
	 * would certainly lower throughput. We may end up in this
	 * case if: (1) upon a dispatch attempt, we detected that it
	 * was better to plug I/O dispatch, and to wait for a new
	 * request to arrive for the currently in-service queue, but
	 * (2) this switch of bfqq to busy changes the scenario.
	 */
	/* 애초에 현재 in-service 큐가 있어야 선점할 대상이 존재한다. */
	if (bfqd->in_service_queue &&
	    ((bfqq_wants_to_preempt &&
	      bfqq->wr_coeff >= bfqd->in_service_queue->wr_coeff) ||
	      /* (조건 A) bfqq가 서비스 공백을 회복하고 싶어하고, 동시에
	       * bfqq의 wr 배율이 in-service 큐보다 낮지 않다 - 시간
	       * 민감도가 밀리지 않는 경우에만 선점을 고려한다. */
	     bfq_bfqq_higher_class_or_weight(bfqq, bfqd->in_service_queue) ||
	     /* (조건 B) bfqq가 in-service 큐보다 더 높은 우선순위 클래스나
	      * 가중치를 가진다 - 그 자체로 선점을 고려할 이유가 된다. */
	     !bfq_better_to_idle(bfqd->in_service_queue)) &&
	     /* (조건 C) 더 이상 in-service 큐를 위해 idling(대기)할 필요가
	      * 없다고 판단됨 - 즉 idling을 계속하는 것이 처리량에도
	      * 도움이 안 되는 상황이라면 굳이 선점을 미룰 이유가 없다. */
	    next_queue_may_preempt(bfqd))
	    /* 위 A/B/C 중 하나라도 참이면서, 또한 "bfqq가 실제로 다음
	     * 서비스 대상이 될 자격이 있다"는 필요조건까지 만족해야
	     * 최종적으로 선점을 실행한다(불필요한 선점을 줄이기 위한
	     * 추가 안전장치). */
		bfq_bfqq_expire(bfqd, bfqd->in_service_queue,
				false, BFQQE_PREEMPTED);
				/* 현재 in-service 큐를 BFQQE_PREEMPTED 사유로
				 * 강제 만료시킨다 - 이후 스케줄러가 다시 큐를
				 * 선택할 때 bfqq가 선택될 기회를 얻는다.
				 * force=false이므로 idling이 필요하다고 다시
				 * 판단되면 그 결정은 존중된다. */
}

/*
 * [한국어]
 * bfq_reset_inject_limit - bfqq의 "주입(injection) 한도"를 초기 상태로 리셋
 *
 * @bfqd: waited_rq(주입된 요청 완료 대기 포인터)를 보유한 장치 스케줄러 데이터
 * @bfqq: 주입 한도를 리셋할 대상 bfq_queue
 * @return: 없음 (void)
 *
 * BFQ는 in-service 큐가 think-time 때문에 잠깐 요청을 내지 않는 틈을 타
 * 다른 큐의 요청을 "주입(inject)"해 장치를 놀리지 않고 처리량을 끌어올릴
 * 수 있다. 하지만 잘못된 주입은 오히려 in-service 큐의 지연시간을
 * 늘릴 수 있으므로, BFQ는 bfqq마다 "한 번에 몇 개까지 주입을 허용할지"를
 * 나타내는 inject_limit을 적응적으로 갱신한다(이 함수 밖의
 * bfq_update_inject_limit 참고). 이 함수는 그 적응 절차를 처음부터(또는
 * 상황이 크게 바뀌어 재측정이 필요할 때) 다시 시작하기 위해 호출되며,
 * think time이 짧은 큐는 주입에 더 취약하다고 보고 한도를 보수적으로
 * 0에서 시작하고, think time이 긴 큐는 1에서 시작한다(위 영어 주석에
 * 근거와 트레이드오프가 상세히 설명되어 있다).
 * 실행 컨텍스트: bfqd->lock을 쥔 상태에서 호출 - think-time 갱신, 큐
 * split/merge, idle 판정 변화 등 주입 조건이 바뀌는 다양한 지점에서
 * 호출될 수 있다.
 * caller: think-time이 짧아지거나 길어질 때, 혹은 큐의 동기화 상대가
 * 바뀔 때 등 주입 조건 재평가가 필요한 여러 경로.
 * callee: bfq_bfqq_has_short_ttime.
 *
 * 호출 체인:
 *   (think-time/병합 상태 변경 경로) → [bfq_reset_inject_limit]
 */
static void bfq_reset_inject_limit(struct bfq_data *bfqd,
				   struct bfq_queue *bfqq)
{
	/* invalidate baseline total service time */
	/* 주입 효과를 측정하기 위한 기준(baseline) 총 서비스 시간을
	 * 무효화(0) - 이후 bfq_update_inject_limit이 이 값이 0인 것을
	 * 보고 "아직 기준 측정이 안 됐다"고 인식해 새로 측정을 시작한다. */
	bfqq->last_serv_time_ns = 0;

	/*
	 * Reset pointer in case we are waiting for
	 * some request completion.
	 */
	/* 혹시 이전에 "이 요청의 완료를 기다려 서비스 시간을 측정하겠다"고
	 * 지정해 둔 요청 포인터가 있었다면 무효화 - 리셋 도중에는 그
	 * 측정이 더 이상 의미가 없으므로 추적을 중단한다. */
	bfqd->waited_rq = NULL;

	/*
	 * If bfqq has a short think time, then start by setting the
	 * inject limit to 0 prudentially, because the service time of
	 * an injected I/O request may be higher than the think time
	 * of bfqq, and therefore, if one request was injected when
	 * bfqq remains empty, this injected request might delay the
	 * service of the next I/O request for bfqq significantly. In
	 * case bfqq can actually tolerate some injection, then the
	 * adaptive update will however raise the limit soon. This
	 * lucky circumstance holds exactly because bfqq has a short
	 * think time, and thus, after remaining empty, is likely to
	 * get new I/O enqueued---and then completed---before being
	 * expired. This is the very pattern that gives the
	 * limit-update algorithm the chance to measure the effect of
	 * injection on request service times, and then to update the
	 * limit accordingly.
	 *
	 * However, in the following special case, the inject limit is
	 * left to 1 even if the think time is short: bfqq's I/O is
	 * synchronized with that of some other queue, i.e., bfqq may
	 * receive new I/O only after the I/O of the other queue is
	 * completed. Keeping the inject limit to 1 allows the
	 * blocking I/O to be served while bfqq is in service. And
	 * this is very convenient both for bfqq and for overall
	 * throughput, as explained in detail in the comments in
	 * bfq_update_has_short_ttime().
	 *
	 * On the opposite end, if bfqq has a long think time, then
	 * start directly by 1, because:
	 * a) on the bright side, keeping at most one request in
	 * service in the drive is unlikely to cause any harm to the
	 * latency of bfqq's requests, as the service time of a single
	 * request is likely to be lower than the think time of bfqq;
	 * b) on the downside, after becoming empty, bfqq is likely to
	 * expire before getting its next request. With this request
	 * arrival pattern, it is very hard to sample total service
	 * times and update the inject limit accordingly (see comments
	 * on bfq_update_inject_limit()). So the limit is likely to be
	 * never, or at least seldom, updated.  As a consequence, by
	 * setting the limit to 1, we avoid that no injection ever
	 * occurs with bfqq. On the downside, this proactive step
	 * further reduces chances to actually compute the baseline
	 * total service time. Thus it reduces chances to execute the
	 * limit-update algorithm and possibly raise the limit to more
	 * than 1.
	 */
	/* think time이 짧은(연속적으로 요청을 내는 경향이 강한) 큐 -
	 * 주입된 요청의 서비스 시간이 이 큐의 think time보다 길면
	 * 오히려 지연이 커질 수 있으므로 일단 보수적으로 주입을
	 * 아예 금지(0)하고 시작한다. 감내 가능하다고 판명되면
	 * bfq_update_inject_limit이 곧 한도를 올려줄 것이다. */
	if (bfq_bfqq_has_short_ttime(bfqq))
		bfqq->inject_limit = 0;
	/* think time이 긴 큐 - 요청 하나 정도의 주입은 지연에 큰
	 * 해가 되지 않을 가능성이 높으므로 처음부터 1을 허용해,
	 * 장치가 놀지 않도록 한다(위 영어 주석의 트레이드오프 참고). */
	else
		bfqq->inject_limit = 1;

	/* 한도를 낮춰야 할지 재평가할 기준 시각을 지금으로 갱신 - 이후
	 * bfq_update_inject_limit이 이 시각 이후 얼마나 지났는지를 보고
	 * 한도 조정 주기를 판단한다. */
	bfqq->decrease_time_jif = jiffies;
}

/*
 * [한국어]
 * bfq_update_io_intensity - bfqq가 최근 관찰 윈도우 동안 얼마나 "바쁘게"
 * I/O를 냈는지를 추적하여 IO_bound 플래그를 갱신한다.
 *
 * @bfqq: 갱신 대상 bfq_queue. sort_list(대기 request rb-tree), dispatched
 *        (드라이브에 내려가 아직 완료되지 않은 개수), io_start_time/
 *        tot_idle_time(관찰 윈도우 시작 시각과 누적 idle 시간) 필드를 사용.
 * @now_ns: 호출 시점의 현재 시각(나노초, blk_time_get_ns()로 얻은 값).
 * @return: 없음. bfqq->tot_idle_time/io_start_time과 IO_bound 플래그를
 *          제자리에서(in-place) 갱신한다.
 *
 * BFQ는 "지속적으로 I/O를 내는(I/O bound)" 프로세스와 "생각 시간이 긴
 * (interactive/seeky)" 프로세스를 구분해 idling 여부와 budget 배분을
 * 다르게 적용한다. 이 함수는 bfqq의 sort_list가 비고 dispatched된 요청도
 * 없는(=드라이브에 아무 것도 내보내지 않은) 순간마다 idle 시간을 누적하고,
 * 관찰 구간(tot_io_time) 대비 idle 비율이 20%를 넘으면(=busy 비율이 80%
 * 미만) IO_bound 플래그를 내리며, 그렇지 않으면 세운다. 또한 관찰 윈도우가
 * 200ms를 넘으면 오래된 통계 비중이 최근 패턴을 가리지 않도록 시작 시각과
 * idle 누적치를 절반으로 줄여 윈도우를 최근 쪽으로 옮긴다. bfqd->lock을
 * 쥔 상태(bfq_add_request 경로)에서 호출되므로 별도의 동기화는 필요 없다.
 *
 * 호출 체인:
 *   bfq_add_request → [bfq_update_io_intensity] → bfq_mark/clear_bfqq_IO_bound
 */
static void bfq_update_io_intensity(struct bfq_queue *bfqq, u64 now_ns)
{
	u64 tot_io_time = now_ns - bfqq->io_start_time;
	/* [한국어] 관찰을 시작한 시각(io_start_time)부터 지금까지 흘러온 총 경과
	 * 시간. 이 값과 tot_idle_time 누적치를 비교해 busy/idle 비율을 계산한다. */

	/* [한국어] 대기 중인 request가 없고(sort_list 비어 있음) 드라이브로
	 * 내려가 아직 완료 안 된 request도 없다면(dispatched==0), 이 순간
	 * bfqq는 드라이브 관점에서 완전히 idle 상태다. */
	if (RB_EMPTY_ROOT(&bfqq->sort_list) && bfqq->dispatched == 0)
		bfqq->tot_idle_time +=
			now_ns - bfqq->ttime.last_end_request;
			/* [한국어] 마지막 request 완료 시각(ttime.last_end_request)부터 지금까지
			 * 걸린 시간을 idle 누적치에 더한다 - 프로세스가 새 I/O를 내기까지
			 * '생각한' 시간을 재는 것이다. */

	/* [한국어] 방금 생성된 큐라면 아직 유의미한 idle/busy 통계가 쌓이지
	 * 않았으므로 판단을 보류하고 반환한다(unlikely: 대부분은 기존 큐). */
	if (unlikely(bfq_bfqq_just_created(bfqq)))
		return;

	/*
	 * Must be busy for at least about 80% of the time to be
	 * considered I/O bound.
	 */
	/* [한국어] idle_time*5 > tot_io_time 은 idle 비율이 20% 초과(=busy
	 * 80% 미만)임을 뜻하는 정수 비교 - 나눗셈 대신 곱셈으로 계산한다. */
	if (bfqq->tot_idle_time * 5 > tot_io_time)
		bfq_clear_bfqq_IO_bound(bfqq);
		/* [한국어] IO_bound 플래그를 내림 - idling을 감수할 만큼 꾸준히 I/O를
		 * 내는 프로세스가 아니라고 판단(idling은 오히려 처리량 손해). */
	else
		bfq_mark_bfqq_IO_bound(bfqq);
		/* [한국어] busy 비율 80% 이상 - IO_bound로 표시. 이후 idling으로
		 * 시퀀셜/서비스 보장을 얻을 가치가 있는 큐로 취급된다. */

	/*
	 * Keep an observation window of at most 200 ms in the past
	 * from now.
	 */
	if (tot_io_time > 200 * NSEC_PER_MSEC) {
	/* [한국어] 관찰 구간이 200ms를 넘으면 오래된 통계 비중이 커져 최근
	 * 패턴을 반영하지 못하므로, 아래에서 윈도우 길이를 절반으로 줄인다. */
		bfqq->io_start_time = now_ns - (tot_io_time>>1);
		/* [한국어] 관찰 시작점을 '현재 - tot_io_time/2'로 앞당겨, 사실상
		 * 관찰 윈도우 길이를 절반으로 줄인다(최근 데이터 위주로 재조정). */
		bfqq->tot_idle_time >>= 1;
		/* [한국어] idle 누적치도 동일 비율(1/2)로 줄여 새 윈도우 길이와
		 * idle/tot_io_time 비율의 일관성을 유지한다. */
		/* [한국어] 윈도우 축소가 끝났으므로 이번 관찰 주기의 갱신을 마친다. */
	}
}

/*
 * Detect whether bfqq's I/O seems synchronized with that of some
 * other queue, i.e., whether bfqq, after remaining empty, happens to
 * receive new I/O only right after some I/O request of the other
 * queue has been completed. We call waker queue the other queue, and
 * we assume, for simplicity, that bfqq may have at most one waker
 * queue.
 *
 * A remarkable throughput boost can be reached by unconditionally
 * injecting the I/O of the waker queue, every time a new
 * bfq_dispatch_request happens to be invoked while I/O is being
 * plugged for bfqq.  In addition to boosting throughput, this
 * unblocks bfqq's I/O, thereby improving bandwidth and latency for
 * bfqq. Note that these same results may be achieved with the general
 * injection mechanism, but less effectively. For details on this
 * aspect, see the comments on the choice of the queue for injection
 * in bfq_select_queue().
 *
 * Turning back to the detection of a waker queue, a queue Q is deemed as a
 * waker queue for bfqq if, for three consecutive times, bfqq happens to become
 * non empty right after a request of Q has been completed within given
 * timeout. In this respect, even if bfqq is empty, we do not check for a waker
 * if it still has some in-flight I/O. In fact, in this case bfqq is actually
 * still being served by the drive, and may receive new I/O on the completion
 * of some of the in-flight requests. In particular, on the first time, Q is
 * tentatively set as a candidate waker queue, while on the third consecutive
 * time that Q is detected, the field waker_bfqq is set to Q, to confirm that Q
 * is a waker queue for bfqq. These detection steps are performed only if bfqq
 * has a long think time, so as to make it more likely that bfqq's I/O is
 * actually being blocked by a synchronization. This last filter, plus the
 * above three-times requirement and time limit for detection, make false
 * positives less likely.
 *
 * NOTE
 *
 * The sooner a waker queue is detected, the sooner throughput can be
 * boosted by injecting I/O from the waker queue. Fortunately,
 * detection is likely to be actually fast, for the following
 * reasons. While blocked by synchronization, bfqq has a long think
 * time. This implies that bfqq's inject limit is at least equal to 1
 * (see the comments in bfq_update_inject_limit()). So, thanks to
 * injection, the waker queue is likely to be served during the very
 * first I/O-plugging time interval for bfqq. This triggers the first
 * step of the detection mechanism. Thanks again to injection, the
 * candidate waker queue is then likely to be confirmed no later than
 * during the next I/O-plugging interval for bfqq.
 *
 * ISSUE
 *
 * On queue merging all waker information is lost.
 */
/*
 * [한국어]
 * bfq_check_waker - bfqq가 다른 큐(waker)의 요청 완료 직후에만 새 I/O를
 * 내는 패턴을 보이는지 감지해 waker_bfqq를 확정한다.
 *
 * @bfqd: 전역 BFQ 스케줄러 상태. last_completed_rq_bfqq/last_completion에
 *        "가장 최근에 완료된 요청이 속한 큐/시각" 정보가 들어 있다.
 * @bfqq: 검사 대상 bfq_queue. 방금 idle에서 busy로 전환되어 새 요청을 받은 큐.
 * @now_ns: 현재 시각(나노초).
 * @return: 없음. 조건을 만족하면 bfqq->tentative_waker_bfqq/waker_bfqq,
 *          num_waker_detections, woken_list 등을 갱신한다.
 *
 * 어떤 프로세스(A)가 다른 프로세스(B)의 I/O 완료를 기다렸다가만 I/O를
 * 내는 동기화 패턴(예: 파이프라인, lock 대기)이 있으면, B의 요청을 즉시
 * injection 해 주는 것이 A의 지연시간과 전체 처리량에 크게 도움이 된다.
 * 이 함수는 "bfqq가 비어있다가 last_completed_rq_bfqq 직후에 다시
 * 채워지는" 일이 연속 3회 관측되면 그 큐를 waker_bfqq로 확정한다.
 * bfqd->lock을 쥔 컨텍스트(bfq_add_request 경로)에서 호출된다.
 *
 * 호출 체인:
 *   bfq_add_request → [bfq_check_waker] → hlist_add_head(woken_list)
 */
static void bfq_check_waker(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			    u64 now_ns)
{
	char waker_name[MAX_BFQQ_NAME_LENGTH];
	/* [한국어] 로그 출력용으로 waker 큐를 식별하는 이름 문자열을 담을
	 * 버퍼(bfq_bfqq_name()이 pid 등을 조합해 채운다). */

	/* [한국어] 아래 여섯 조건 중 하나라도 참이면 waker 감지를 포기하고
	 * 반환한다: 완료된 요청이 아직 없거나, 마지막 완료 큐가 bfqq
	 * 자신이거나, bfqq의 think time이 짧아 이미 동기 도착 패턴이거나,
	 * 마지막 완료로부터 4ms 이상 지나 시간적 인접성이 없거나, 완료
	 * 큐/bfqq 자신이 oom_bfqq(메모리 부족 임시 큐)라 상태가 불안정한
	 * 경우다. */
	if (!bfqd->last_completed_rq_bfqq ||
	    bfqd->last_completed_rq_bfqq == bfqq ||
	    bfq_bfqq_has_short_ttime(bfqq) ||
	    now_ns - bfqd->last_completion >= 4 * NSEC_PER_MSEC ||
	    bfqd->last_completed_rq_bfqq == &bfqd->oom_bfqq ||
	    bfqq == &bfqd->oom_bfqq)
		return;
		/* [한국어] 위 조건 중 하나라도 성립하면 감지를 시도할 전제 자체가
		 * 깨지므로 즉시 반환한다. */

	/*
	 * We reset waker detection logic also if too much time has passed
 	 * since the first detection. If wakeups are rare, pointless idling
	 * doesn't hurt throughput that much. The condition below makes sure
	 * we do not uselessly idle blocking waker in more than 1/64 cases.
	 */
	/* [한국어] 이번에 완료된 큐가 지금까지의 후보(tentative_waker_bfqq)와
	 * 다르거나, 첫 감지 이후 128*bfq_slice_idle만큼 시간이 지나 감지
	 * 시도가 만료됐다면 - 후보를 초기화하고 새로 카운트를 시작한다. */
	if (bfqd->last_completed_rq_bfqq !=
	    bfqq->tentative_waker_bfqq ||
	    now_ns > bfqq->waker_detection_started +
					128 * (u64)bfqd->bfq_slice_idle) {
		/*
		 * First synchronization detected with a
		 * candidate waker queue, or with a different
		 * candidate waker queue from the current one.
		 */
		/* [한국어] 이번에 완료된 큐를 새로운 후보 waker로 설정. */
		bfqq->tentative_waker_bfqq =
			bfqd->last_completed_rq_bfqq;
		/* [한국어] 감지 카운터를 1로 리셋 - 이번이 첫 관측. */
		bfqq->num_waker_detections = 1;
		/* [한국어] 감지 시작 시각 기록 - 위 128*slice_idle 만료 판정의 기준점. */
		bfqq->waker_detection_started = now_ns;
		/* [한국어] 디버그 로그용으로 후보 waker 큐의 이름 문자열을 만든다. */
		bfq_bfqq_name(bfqq->tentative_waker_bfqq, waker_name,
			      MAX_BFQQ_NAME_LENGTH);
		/* [한국어] '임시 waker 설정됨' 트레이스 로그. */
		bfq_log_bfqq(bfqd, bfqq, "set tentative waker %s", waker_name);
	/* [한국어] 같은 후보가 다시 관측됨 - 연속 관측 횟수를 증가시킨다. */
	} else /* Same tentative waker queue detected again */
		bfqq->num_waker_detections++;

	/* [한국어] 동일한 후보가 연속 3회 관측되면 우연이 아니라 실제
	 * 동기화 관계로 판단하고 waker로 확정한다. */
	if (bfqq->num_waker_detections == 3) {
		bfqq->waker_bfqq = bfqd->last_completed_rq_bfqq;
		/* [한국어] waker_bfqq를 확정 - 이후 bfq_select_queue()의 injection
		 * 로직이 이 큐의 요청을 우선 주입할 수 있게 참조한다. */
		bfqq->tentative_waker_bfqq = NULL;
		/* [한국어] 확정되었으므로 임시 후보 슬롯은 비운다. */
		/* [한국어] 확정된 waker 큐의 이름을 로그용으로 생성. */
		bfq_bfqq_name(bfqq->waker_bfqq, waker_name,
			      MAX_BFQQ_NAME_LENGTH);
		/* [한국어] 'waker 확정됨' 트레이스 로그. */
		bfq_log_bfqq(bfqd, bfqq, "set waker %s", waker_name);

		/*
		 * If the waker queue disappears, then
		 * bfqq->waker_bfqq must be reset. To
		 * this goal, we maintain in each
		 * waker queue a list, woken_list, of
		 * all the queues that reference the
		 * waker queue through their
		 * waker_bfqq pointer. When the waker
		 * queue exits, the waker_bfqq pointer
		 * of all the queues in the woken_list
		 * is reset.
		 *
		 * In addition, if bfqq is already in
		 * the woken_list of a waker queue,
		 * then, before being inserted into
		 * the woken_list of a new waker
		 * queue, bfqq must be removed from
		 * the woken_list of the old waker
		 * queue.
		 */
		/* [한국어] bfqq가 이전에 다른 waker의 woken_list에 이미 연결되어
		 * 있다면(unhashed 아님), 새 waker 리스트에 넣기 전에 먼저 그
		 * 연결을 끊어야 한다. */
		if (!hlist_unhashed(&bfqq->woken_list_node))
			hlist_del_init(&bfqq->woken_list_node);
			/* [한국어] 이전 waker의 woken_list에서 제거하고 노드를 초기화한다. */
		/* [한국어] bfqq를 새로 확정된 waker의 woken_list 헤드에 추가한다 -
		 * waker가 사라질 때 이 리스트를 순회하며 각 bfqq->waker_bfqq를
		 * 리셋하기 위함. */
		hlist_add_head(&bfqq->woken_list_node,
			       &bfqd->last_completed_rq_bfqq->woken_list);
	}
}

/*
 * [한국어]
 * bfq_add_request - 새 request를 bfqq의 sort_list(rb-tree)/fifo에 삽입하고,
 * weight-raising/waker 검출/injection 샘플링 상태를 함께 갱신한다.
 *
 * @rq: 방금 bfqq에 배정되어 삽입할 struct request. RQ_BFQQ(rq)로 소속
 *      bfq_queue를, RQ_BIC(rq)로 소속 io_cq(bfq_io_cq)를 얻는다.
 * @return: 없음. bfqq->sort_list/next_rq/queued/wr_coeff 등과 bfqd->queued,
 *          bfqd->wait_dispatch/rqs_injected 등 전역 상태를 갱신한다.
 *
 * bio가 request로 승격되어 BFQ에 최종 삽입되는 지점이다. 이후 이 request는
 * bfq_dispatch_request()가 골라 blk-mq를 통해 드라이버(NVMe라면 SQ)로
 * 내려보내기 전까지 bfqq->sort_list에 LBA 순으로 대기한다. 이 함수는
 * (1) bfqq->queued/bfqd->queued 카운터 증가, (2) 동기 큐이고 아직 io_cq의
 * process reference가 1개 이하일 때 waker 감지(bfq_check_waker)와 주기적
 * inject limit 리셋, injection 총 서비스시간 샘플링 상태 머신 시작,
 * (3) IO intensity 갱신(bfq_update_io_intensity), (4) rb-tree 삽입과
 * next_rq 갱신, (5) idle→busy 전환 처리 또는 이미 busy인 경우 비동기
 * 큐의 weight-raising 승격 검토, (6) last_wr_start_finish 갱신 순으로
 * 진행된다. bfqd->lock을 쥔 채(bfq_insert_request 경로) 호출된다.
 *
 * 호출 체인:
 *   bfq_insert_request → __bfq_insert_request → [bfq_add_request]
 *   → bfq_check_waker / bfq_update_io_intensity / bfq_bfqq_handle_idle_busy_switch
 */
static void bfq_add_request(struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq);
	/* [한국어] 이 request가 배정된 bfq_queue를 request의 elevator private
	 * 데이터(RQ_BFQQ 매크로)에서 얻는다. */
	struct bfq_data *bfqd = bfqq->bfqd;
	/* [한국어] bfqq가 속한 전역 BFQ 스케줄러 상태에 대한 포인터. */
	struct request *next_rq, *prev;
	/* [한국어] next_rq: 이번 삽입으로 bfqq의 '다음 서비스 후보'가
	 * 바뀌는지 비교하기 위한 새 후보. prev: 삽입 전의 기존 next_rq
	 * 값(변경 여부 판단용). */
	unsigned int old_wr_coeff = bfqq->wr_coeff;
	/* [한국어] 삽입 전 weight-raising 계수를 저장 - 이 함수 뒤쪽에서
	 * '이번 삽입으로 wr 상태가 새로 시작/변경됐는지' 판단하는 기준값. */
	bool interactive = false;
	/* [한국어] idle→busy 전환 처리(bfq_bfqq_handle_idle_busy_switch)가
	 * '이번 전환이 interactive 승격을 유발했는지'를 돌려주는 출력 변수. */
	u64 now_ns = blk_time_get_ns();
	/* [한국어] 이 삽입 시점의 타임스탬프(나노초) - waker 감지, IO
	 * intensity 갱신, injection 샘플링 시작 시각 등에 공통 사용. */

	bfq_log_bfqq(bfqd, bfqq, "add_request %d", rq_is_sync(rq));
	/* [한국어] BFQ 트레이스 로그 - 동기(1)/비동기(0) 여부와 함께 기록. */
	bfqq->queued[rq_is_sync(rq)]++;
	/* [한국어] bfqq 안에서 동기/비동기별 대기 request 개수 카운터 증가. */
	/*
	 * Updating of 'bfqd->queued' is protected by 'bfqd->lock', however, it
	 * may be read without holding the lock in bfq_has_work().
	 */
	WRITE_ONCE(bfqd->queued, bfqd->queued + 1); // [한국어] 스케줄러 전체에서 아직 드라이버로 내려가지 않고 대기 중인 request 총수 증가 - bfq_has_work()가 락 없이 읽으므로 WRITE_ONCE로 보호.

	if (bfq_bfqq_sync(bfqq) && RQ_BIC(rq)->requests <= 1) {
	/* [한국어] bfqq가 동기 큐이고, 이 request를 낸 io_cq(bic)가 아직
	 * '요청 1개 이하'만 발행한 상태(사실상 첫 요청)일 때만 waker 감지와
	 * injection 관련 상태 갱신을 시도한다 - 이미 여러 요청이 몰려 있으면
	 * waker 판단의 전제(직전까지 idle)가 깨진다. */
	/* [한국어] waker 감지를 시도한다. */
		bfq_check_waker(bfqd, bfqq, now_ns);
		/* [한국어] bfqq가 다른 큐의 완료 직후에만 깨어나는 패턴인지 검사해
		 * waker_bfqq를 갱신한다. */

		/*
		 * Periodically reset inject limit, to make sure that
		 * the latter eventually drops in case workload
		 * changes, see step (3) in the comments on
		 * bfq_update_inject_limit().
		 */
		/* [한국어] 마지막으로 inject_limit을 낮춘 뒤 1초 이상 지났다면,
		 * 워크로드가 바뀌었을 수 있으니 강제로 리셋해 다시 관찰한다. */
		if (time_is_before_eq_jiffies(bfqq->decrease_time_jif +
					     msecs_to_jiffies(1000)))
			bfq_reset_inject_limit(bfqd, bfqq);
			/* [한국어] inject_limit과 관련 샘플링 상태를 초기값으로 되돌린다
			 * (경계 함수 bfq_reset_inject_limit, 다른 담당 구간에서 주석 작성). */

		/*
		 * The following conditions must hold to setup a new
		 * sampling of total service time, and then a new
		 * update of the inject limit:
		 * - bfqq is in service, because the total service
		 *   time is evaluated only for the I/O requests of
		 *   the queues in service;
		 * - this is the right occasion to compute or to
		 *   lower the baseline total service time, because
		 *   there are actually no requests in the drive,
		 *   or
		 *   the baseline total service time is available, and
		 *   this is the right occasion to compute the other
		 *   quantity needed to update the inject limit, i.e.,
		 *   the total service time caused by the amount of
		 *   injection allowed by the current value of the
		 *   limit. It is the right occasion because injection
		 *   has actually been performed during the service
		 *   hole, and there are still in-flight requests,
		 *   which are very likely to be exactly the injected
		 *   requests, or part of them;
		 * - the minimum interval for sampling the total
		 *   service time and updating the inject limit has
		 *   elapsed.
		 */
		/* [한국어] bfqq가 현재 서비스 중이고, 드라이브에 아무 요청도 없거나
		 * (baseline 측정 적기) 또는 이미 baseline이 있고 injection이 실제로
		 * 일어난 상태(inject 효과 측정 적기)이며, 마지막 샘플링 후 최소
		 * 10ms가 지났다면 - 새 서비스시간 샘플링을 시작한다. */
		if (bfqq == bfqd->in_service_queue &&
		    (bfqd->tot_rq_in_driver == 0 ||
		     (bfqq->last_serv_time_ns > 0 &&
		      bfqd->rqs_injected && bfqd->tot_rq_in_driver > 0)) &&
		    time_is_before_eq_jiffies(bfqq->decrease_time_jif +
					      msecs_to_jiffies(10))) {
			bfqd->last_empty_occupied_ns = blk_time_get_ns();
			/* [한국어] 샘플링 시작 시각을 기록 - 완료 시점 경과 시간의 기준점. */
			/*
			 * Start the state machine for measuring the
			 * total service time of rq: setting
			 * wait_dispatch will cause bfqd->waited_rq to
			 * be set when rq will be dispatched.
			 */
			bfqd->wait_dispatch = true;
			/* [한국어] '이 rq가 dispatch되면 waited_rq로 지정해 추적하라'는
			 * 상태 플래그 - 서비스시간 측정 상태 머신의 시작 신호. */
			/*
			 * If there is no I/O in service in the drive,
			 * then possible injection occurred before the
			 * arrival of rq will not affect the total
			 * service time of rq. So the injection limit
			 * must not be updated as a function of such
			 * total service time, unless new injection
			 * occurs before rq is completed. To have the
			 * injection limit updated only in the latter
			 * case, reset rqs_injected here (rqs_injected
			 * will be set in case injection is performed
			 * on bfqq before rq is completed).
			 */
			/* [한국어] 드라이브가 완전히 비어 있던 상황이면 이전 injection 기록은
			 * 이번 측정과 무관하므로 rqs_injected를 초기화해 '이번 rq 처리
			 * 도중 새로 injection이 발생했는가'만 다시 관찰한다. */
			if (bfqd->tot_rq_in_driver == 0)
				bfqd->rqs_injected = false;
				/* [한국어] injection 발생 이력을 초기화. */
		}
	}

	/* [한국어] 동기 큐에 대해서만 IO intensity(바쁨 비율)를 갱신한다 -
	 * 비동기 큐는 프로세스의 think time 개념이 성립하지 않는다. */
	if (bfq_bfqq_sync(bfqq))
		bfq_update_io_intensity(bfqq, now_ns);

	elv_rb_add(&bfqq->sort_list, rq); // [한국어] request를 bfqq의 rb-tree(sort_list)에 LBA(섹터) 순으로 삽입 - NVMe controller의 명령 재정렬과는 별개인 BFQ 내부 정렬 순서.

	/*
	 * Check if this request is a better next-serve candidate.
	 */
	/* [한국어] 삽입 전 next_rq를 보존 - 아래에서 값이 바뀌었는지 비교할
	 * 기준으로 사용. */
	prev = bfqq->next_rq;
	/* [한국어] (관련) next_rq 재계산 직전의 기존 값. */
	next_rq = bfq_choose_req(bfqd, bfqq->next_rq, rq, bfqd->last_position); // [한국어] 기존 next_rq 후보와 방금 삽입한 rq 중 last_position 기준으로 더 유리한 쪽을 선택 - 다음 서비스 시 실제로 내보낼 request 후보.
	/* [한국어] (관련) 위에서 계산한 새 next_rq 후보. */
	bfqq->next_rq = next_rq;
	/* [한국어] 새로 고른 후보를 next_rq에 반영. */
	/* [한국어] (관련) next_rq 갱신 완료 - 아래에서 위치 트리 반영 여부를 검토한다. */

	/*
	 * Adjust priority tree position, if next_rq changes.
	 * See comments on bfq_pos_tree_add_move() for the unlikely().
	 */
	/* [한국어] NVMe처럼 큐잉을 지원하는 논-로테이셔널 장치가 아닐 때만
	 * (nonrot_with_queueing==false) next_rq 변경에 맞춰 위치 트리
	 * (rq_pos_tree)에서 bfqq 위치를 갱신한다 - NVMe SSD는 병렬 dispatch로
	 * cooperator merge 이득이 적어 unlikely()로 표시될 만큼 드물다. */
	if (unlikely(!bfqd->nonrot_with_queueing && prev != bfqq->next_rq))
		bfq_pos_tree_add_move(bfqd, bfqq);

	/* [한국어] bfqq가 지금까지 idle(비활성) 상태였다가 이번 삽입으로
	 * busy로 전환되는 경우 - weight-raising 시작 여부, soft-rt 판정,
	 * 스케줄링 트리 등록 등을 처리하는 전용 경로로 분기한다. */
	if (!bfq_bfqq_busy(bfqq)) /* switching to busy ... */ // [한국어] idle→busy 전환 처리로 분기.
		bfq_bfqq_handle_idle_busy_switch(bfqd, bfqq, old_wr_coeff,
						 rq, &interactive);
	else {
	/* [한국어] bfqq가 이미 busy 상태였던 경우 - idle→busy 전환 로직
	 * 대신 비동기 큐의 뒤늦은 weight-raising 승격과 next_rq 갱신에
	 * 따른 예산 재계산만 처리한다. */
	/* [한국어] 아래에서 이 비동기 큐가 뒤늦은 weight-raising 대상인지
	 * 검사한다. */
		/* [한국어] low_latency 모드이고, 지금까지 wr_coeff가 1(가중치 상승
		 * 없음)이었으며, 이번 요청이 비동기이고, 마지막 wr 종료 후 최소
		 * 간격(bfq_wr_min_inter_arr_async)이 지났다면 - 이 비동기 큐를
		 * 새로 weight-raise 한다(연속 비동기 I/O 프로세스의 지연 완화). */
		if (bfqd->low_latency && old_wr_coeff == 1 && !rq_is_sync(rq) &&
		    time_is_before_jiffies(
				bfqq->last_wr_start_finish +
				bfqd->bfq_wr_min_inter_arr_async)) {
			bfqq->wr_coeff = bfqd->bfq_wr_coeff;
			/* [한국어] weight-raising 계수를 설정값(bfq_wr_coeff)으로 올려 이
			 * 큐의 스케줄링 가중치를 일시적으로 높인다. */
			bfqq->wr_cur_max_time = bfq_wr_duration(bfqd);
			/* [한국어] 이번 weight-raising이 지속될 최대 시간(jiffies)을 계산해
			 * 저장 - 이 시간이 지나면 bfq_bfqq_end_wr로 원복된다. */
			/* [한국어] 아래에서 전역 wr 큐 카운터와 엔티티 상태를 갱신한다. */

			bfqd->wr_busy_queues++;
			/* [한국어] 전역적으로 '현재 weight-raised 상태인 busy 큐' 개수를
			 * 증가시켜 스케줄러가 전체 wr 부하를 추적하게 한다. */
			bfqq->entity.prio_changed = 1;
			/* [한국어] 엔티티(스케줄링 트리 노드) 가중치가 바뀌었음을 표시 -
			 * 다음 __bfq_entity_update_weight_prio 호출에서 실제로 트리상의
			 * 위치/가중치를 재계산하도록 트리거한다. */
		}
		/* [한국어] busy 상태를 유지한 채로도 next_rq가 바뀌었다면(더 좋은
		 * 후보를 찾음) 예산 관련 상태를 갱신해야 한다. */
		if (prev != bfqq->next_rq)
			bfq_updated_next_req(bfqd, bfqq);
			/* [한국어] 새 next_rq 크기에 맞춰 bfqq의 예산을 재계산한다. */
	}

	/*
	 * Assign jiffies to last_wr_start_finish in the following
	 * cases:
	 *
	 * . if bfqq is not going to be weight-raised, because, for
	 *   non weight-raised queues, last_wr_start_finish stores the
	 *   arrival time of the last request; as of now, this piece
	 *   of information is used only for deciding whether to
	 *   weight-raise async queues
	 *
	 * . if bfqq is not weight-raised, because, if bfqq is now
	 *   switching to weight-raised, then last_wr_start_finish
	 *   stores the time when weight-raising starts
	 *
	 * . if bfqq is interactive, because, regardless of whether
	 *   bfqq is currently weight-raised, the weight-raising
	 *   period must start or restart (this case is considered
	 *   separately because it is not detected by the above
	 *   conditions, if bfqq is already weight-raised)
	 *
	 * last_wr_start_finish has to be updated also if bfqq is soft
	 * real-time, because the weight-raising period is constantly
	 * restarted on idle-to-busy transitions for these queues, but
	 * this is already done in bfq_bfqq_handle_idle_busy_switch if
	 * needed.
	 */
	if (bfqd->low_latency &&
		(old_wr_coeff == 1 || bfqq->wr_coeff == 1 || interactive))
		/* [한국어] (a) 원래 wr 상태가 아니었거나, (b) 지금도 wr 상태가
		 * 아니거나, (c) 이번에 interactive로 판정된 경우 중 하나라도
		 * 해당하면 last_wr_start_finish를 지금 시각으로 갱신한다 -
		 * 각각 '마지막 요청 도착 시각 기록', 'wr 시작 시각 기록',
		 * 'interactive wr 기간 재시작'의 의미를 가진다. */
		bfqq->last_wr_start_finish = jiffies;
}

/*
 * [한국어]
 * bfq_find_rq_fmerge - 새 bio가 이어붙을 수 있는 "front merge" 후보
 * request를 bfqd->bio_bfqq의 sort_list에서 찾는다.
 *
 * @bfqd: 전역 BFQ 상태. bio_bfqq는 bfq_bio_merge()가 미리 찾아 캐시해 둔,
 *        이 bio를 낼 프로세스에 대응하는 bfq_queue.
 * @bio: 병합을 시도 중인 새 bio.
 * @q: 요청 큐(사용되지 않지만 elevator 콜백 시그니처를 맞추기 위해 존재).
 * @return: bio의 끝 섹터(bio_end_sector)와 정확히 맞물리는 기존 request가
 *          있으면 그 request, 없거나 bio_bfqq가 없으면 NULL.
 *
 * elv_bio_merge_ok()로 최종 검증되기 전에, "이 bio 바로 앞에 이어붙을 수
 * 있는 request가 rb-tree 안에 있는가"만 빠르게 조회하는 헬퍼다. bfqd->lock을
 * 쥔 채(bfq_request_merge 경로) 호출된다.
 *
 * 호출 체인:
 *   bfq_request_merge → [bfq_find_rq_fmerge] → elv_rb_find
 */
static struct request *bfq_find_rq_fmerge(struct bfq_data *bfqd,
					  struct bio *bio,
					  struct request_queue *q)
{
	struct bfq_queue *bfqq = bfqd->bio_bfqq;
	/* [한국어] bfq_bio_merge()가 미리 조회해 bfqd->bio_bfqq에 캐시해
	 * 둔 '이 bio를 낼 프로세스의 bfq_queue'를 그대로 재사용한다. */


	/* [한국어] 대응하는 bfqq를 찾았다면, 그 큐의 rb-tree에서 이 bio의
	 * 끝 섹터와 정확히 일치하는 request를 찾아본다(front merge 후보 -
	 * 새 bio가 기존 request 바로 앞에 붙는 경우). */
	if (bfqq)
		return elv_rb_find(&bfqq->sort_list, bio_end_sector(bio));
		/* [한국어] 정확히 일치하는 request가 있으면 그것을 front-merge
		 * 후보로 반환한다. */

	/* [한국어] 이 bio에 대응하는 bfq_queue 자체가 없으면(예: 아직
	 * io_cq가 없거나 cgroup 미확정) merge 후보를 찾을 수 없다. */
	return NULL;
	/* [한국어] 대응하는 bfq_queue가 없어 병합 후보를 찾지 못했음을 알린다. */
}

/*
 * [한국어]
 * get_sdist - 마지막 서비스 위치(last_pos)와 이 request의 시작 섹터
 * 사이의 절대 거리(seek distance)를 계산한다.
 *
 * @last_pos: 최근에 서비스된(또는 기준이 되는) 섹터 위치. 0이면 아직
 *            기준 위치가 없음으로 취급.
 * @rq: 거리를 잴 대상 request.
 * @return: |blk_rq_pos(rq) - last_pos| (섹터 단위), last_pos가 0이면 0.
 *
 * 이 값은 bfq_rq_close_to_sector()/bfqq_find_close() 등에서 '두 request가
 * LBA상 인접한가'를 판단해 cooperator(협조 큐) 병합 여부를 정하는 데
 * 쓰인다. 순수 계산 함수로 락/상태 변경이 없다.
 *
 * 호출 체인:
 *   bfq_rq_close_to_sector 계열 → [get_sdist] → blk_rq_pos
 */
static sector_t get_sdist(sector_t last_pos, struct request *rq)
{
	/* [한국어] 유효한 기준 위치가 있으면 이 request의 시작 섹터와의
	 * 절대 차이를 반환한다(seek distance). */
	if (last_pos)
		return abs(blk_rq_pos(rq) - last_pos);
		/* [한국어] abs()로 부호 없는 sector_t 뺄셈 결과를 안전하게
		 * 절대값 처리한다. */

	/* [한국어] 기준 위치가 아직 없으면(예: 첫 요청) 거리를 0으로
	 * 취급해 '가깝다'고 낙관적으로 판단한다. */
	return 0;
	/* [한국어] last_pos가 없을 때의 반환 경로 - 별도 오류 처리 없이
	 * 0을 그대로 반환한다. */
}

/*
 * [한국어]
 * bfq_remove_request - request를 bfqq의 자료구조(sort_list, fifo, hash,
 * position tree)에서 완전히 제거한다.
 *
 * @q: 이 request가 속한 request_queue. q->elevator->hash에서도 제거해야
 *     하므로 함께 전달된다.
 * @rq: 제거할 request. 이미 dispatch되어 완료됐거나, merge로 흡수됐거나,
 *      abort/requeue 등으로 스케줄러에서 빠지는 모든 경우에 호출된다.
 * @return: 없음.
 *
 * bfq_add_request()의 역연산에 해당한다. next_rq가 이 rq를 가리키고
 * 있었다면 다음 후보를 다시 계산하고, sort_list rb-tree/fifo 리스트/
 * elevator 해시 테이블에서 rq를 제거하며, 큐가 완전히 비게 되면 busy
 * 리스트와 위치 트리(rq_pos_tree)에서도 bfqq 자신을 제거한다. bfqd->lock을
 * 쥔 상태에서 호출된다.
 *
 * 호출 체인:
 *   bfq_dispatch_request / bfq_requests_merged / bfq_finish_requeue_request
 *   → [bfq_remove_request] → bfq_del_bfqq_busy / elv_rb_del
 */
static void bfq_remove_request(struct request_queue *q,
			       struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq);
	/* [한국어] rq가 속한 bfq_queue. */
	struct bfq_data *bfqd = bfqq->bfqd;
	/* [한국어] 전역 BFQ 스케줄러 상태. */
	const int sync = rq_is_sync(rq);
	/* [한국어] 이 request가 동기(1)/비동기(0)인지 - queued[] 카운터
	 * 인덱스로 사용. */

	if (bfqq->next_rq == rq) {
	/* [한국어] 제거할 rq가 마침 '다음 서비스 후보(next_rq)'였다면,
	 * 제거 후 후보가 사라지므로 rb-tree에서 다음으로 적합한 request를
	 * 다시 탐색해 next_rq를 갱신해야 한다. */
	/* [한국어] rq를 기준으로 LBA상 다음 순서의 request를 rb-tree에서
	 * 찾아 새 next_rq로 지정(rq는 아직 트리에서 제거 전이므로 기준점
	 * 으로 사용 가능). */
		bfqq->next_rq = bfq_find_next_rq(bfqd, bfqq, rq);
		/* [한국어] 새로 찾은 request를 next_rq로 반영한다. */
		bfq_updated_next_req(bfqd, bfqq);
		/* [한국어] next_rq가 바뀌었으므로 그에 맞춰 bfqq의 예산/엔트리
		 * 상태를 갱신한다. */
	}

	/* [한국어] rq->queuelist가 자기 자신을 가리키지 않는다는 것은
	 * fifo 리스트에 실제로 연결되어 있다는 뜻 - 연결된 경우에만
	 * 안전하게 해제한다. */
	if (rq->queuelist.prev != &rq->queuelist)
		list_del_init(&rq->queuelist);
		/* [한국어] fifo(도착 순서) 리스트에서 rq를 제거하고 노드를
		 * 초기화(자기 자신을 가리키게)한다. */
	bfqq->queued[sync]--;
	/* [한국어] bfqq 안의 동기/비동기별 대기 개수 카운터 감소. */
	/*
	 * Updating of 'bfqd->queued' is protected by 'bfqd->lock', however, it
	 * may be read without holding the lock in bfq_has_work().
	 */
	WRITE_ONCE(bfqd->queued, bfqd->queued - 1);
	/* [한국어] 전역 대기 request 카운터 감소 - bfq_has_work()가 락
	 * 없이 읽으므로 WRITE_ONCE로 원자적 단일 저장을 보장. */
	elv_rb_del(&bfqq->sort_list, rq);
	/* [한국어] rq를 bfqq의 LBA 정렬 rb-tree(sort_list)에서 제거한다. */

	elv_rqhash_del(q, rq);
	/* [한국어] elevator 전역 해시 테이블(q->elevator->hash)에서도 rq를
	 * 제거한다 - 이 해시는 새 bio 도착 시 merge 후보를 빠르게 찾는
	 * 데 쓰이므로, 스케줄러를 떠나는 rq는 여기서도 반드시 빠져야 한다. */
	/* [한국어] 마지막으로 병합에 성공했던 캐시(q->last_merge)가 하필
	 * 이 rq를 가리키고 있었다면 무효화해야 dangling 포인터로 남지
	 * 않는다. */
	if (q->last_merge == rq)
		q->last_merge = NULL;

	/* [한국어] 이번 제거로 bfqq의 sort_list가 완전히 비었다면 - bfqq
	 * 자체가 '빈 큐'로 전환되는 부수 처리를 진행한다. */
	if (RB_EMPTY_ROOT(&bfqq->sort_list)) {
	/* [한국어] (계속) 아래에서 busy 리스트/위치 트리 정리를 진행한다. */
		bfqq->next_rq = NULL;
		/* [한국어] 대기 중인 request가 없으므로 next_rq도 없음으로 설정. */

		if (bfq_bfqq_busy(bfqq) && bfqq != bfqd->in_service_queue) {
		/* [한국어] bfqq가 아직 busy(스케줄링 트리에 활성 상태)로 표시돼
		 * 있는데도 현재 서비스 중인 큐가 아니라면 - 서비스 중이 아닌데
		 * 비게 된 예외적 경로이므로 아래에서 busy 리스트 제거와 함께
		 * 예산/서비스량을 리셋해 일관된 상태로 되돌린다. */
		/* [한국어] (계속) busy 리스트에서 제거하고 예산/서비스량을 리셋한다. */
			bfq_del_bfqq_busy(bfqq, false);
			/* [한국어] bfqq를 busy(활성) 리스트/스케줄링 트리에서 제거한다.
			 * 두 번째 인자 false는 '만료(expire)로 인한 제거가 아님'을
			 * 의미(만료 통계 갱신을 건너뜀). */
			/*
			 * bfqq emptied. In normal operation, when
			 * bfqq is empty, bfqq->entity.service and
			 * bfqq->entity.budget must contain,
			 * respectively, the service received and the
			 * budget used last time bfqq emptied. These
			 * facts do not hold in this case, as at least
			 * this last removal occurred while bfqq is
			 * not in service. To avoid inconsistencies,
			 * reset both bfqq->entity.service and
			 * bfqq->entity.budget, if bfqq has still a
			 * process that may issue I/O requests to it.
			 */
			bfqq->entity.budget = bfqq->entity.service = 0;
			/* [한국어] 서비스 중이 아닌 상태로 비게 된 예외 케이스이므로
			 * 이전 budget/service 값을 신뢰할 수 없다 - 둘 다 0으로 리셋해
			 * 다음 활성화 시 잘못된 잔여 예산으로 시작하지 않게 한다. */
			/* [한국어] (관련) budget/service를 0으로 리셋 완료. */
		}

		/*
		 * Remove queue from request-position tree as it is empty.
		 */
		if (bfqq->pos_root) {
		/* [한국어] bfqq가 위치 트리(rq_pos_tree, cooperator 탐색용)에
		 * 등록되어 있었다면, 요청이 없으니 트리에서 제거한다 - 빈 큐는
		 * '다음 요청 위치'가 없어 근접도 비교 대상이 될 수 없다. */
		/* [한국어] (계속) 위치 트리에서 bfqq 노드를 제거한다. */
			rb_erase(&bfqq->pos_node, bfqq->pos_root);
			/* [한국어] rb-tree(rq_pos_tree)에서 bfqq->pos_node를 제거. */
			bfqq->pos_root = NULL;
			/* [한국어] 더 이상 어떤 위치 트리에도 속하지 않음을 표시. */
		}
	} else {
	/* [한국어] 제거 후에도 sort_list가 비지 않았다면(다른 request가
	 * 남아 있음) - bfqq는 여전히 유효한 위치를 가지므로 위치 트리
	 * 에서의 위치만 필요 시 재조정한다. */
	/* [한국어] (계속) sort_list가 비지 않았으므로 위치 트리 재조정만 검토한다. */
		/* see comments on bfq_pos_tree_add_move() for the unlikely() */
		/* [한국어] NVMe 같은 큐잉 지원 비회전형 장치가 아닌 경우에만
		 * (예: 회전형 HDD) 위치 트리 재배치 비용을 들인다 - NVMe에서는
		 * 이 경로가 unlikely로 표시될 만큼 드물게 실행된다. */
		if (unlikely(!bfqd->nonrot_with_queueing))
			bfq_pos_tree_add_move(bfqd, bfqq);
	}

	/* [한국어] rq가 메타데이터 I/O(REQ_META, 예: 저널/inode 갱신)였다면
	 * bfqq가 추적하는 미완료 메타데이터 요청 수를 감소시킨다 - 메타
	 * 데이터 request는 별도로 우선순위 판단에 활용된다. */
	if (rq->cmd_flags & REQ_META)
		bfqq->meta_pending--;

}

/*
 * [한국어]
 * bfq_bio_merge - blk-mq의 bio-merge 콜백. 새 bio를 기존 request에
 * 병합할 수 있는지 판단하고(elevator_ops.bio_merge), 이 과정에서 발견된
 * cgroup/bfqq 정보를 갱신한다.
 *
 * @q: 이 bio가 제출된 request_queue.
 * @bio: 새로 제출된 bio.
 * @nr_segs: bio의 세그먼트 수(병합 후 세그먼트 제한 검사에 사용).
 * @return: true면 어떤 기존 request와 병합됨(새 request로 submit할 필요
 *          없음), false면 병합 실패(새 request 생성 필요).
 *
 * blk_mq_submit_bio()가 bio를 request로 만들기 직전에 호출해, 이 bio를
 * 기존 대기 중인 request 앞/뒤에 붙일 수 있는지 BFQ에 묻는다. 병합에
 * 성공하면 NVMe 관점에서 SQ(Submission Queue)에 올라갈 명령의 크기가
 * 커지고 PRP/SGL 체인은 줄어들며, 결과적으로 doorbell 횟수가 줄어 커맨드
 * 오버헤드가 낮아진다. 이 함수는 먼저 bio를 낸 프로세스의 io_cq(bic)를
 * 찾아 cgroup 정보를 최신화하고 bfqd->bio_bfqq/bio_bic 캐시를 채운 뒤,
 * 실제 병합 판단은 blk_mq_sched_try_merge()에 위임한다. bfqd->lock을
 * 직접 잡고/푸는 top-level 콜백이다.
 *
 * 호출 체인:
 *   blk_mq_submit_bio → blk_attempt_bio_merge → elevator_ops.bio_merge
 *   → [bfq_bio_merge] → blk_mq_sched_try_merge → bfq_request_merge
 */
static bool bfq_bio_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	/* [한국어] 이 request_queue에 연결된 BFQ 전역 상태. */
	struct bfq_io_cq *bic = bfq_bic_lookup(q);
	/* [한국어] 현재 컨텍스트(제출 프로세스)에 대응하는 io_cq를
	 * 조회한다 - 없으면(예: 아직 초기화 전) NULL일 수 있다. */
	struct request *free = NULL;
	/* [한국어] 병합 과정에서 남는 request가 있으면(예: 두 request가
	 * 합쳐지며 하나가 불필요해짐) 여기 담겨 나중에 해제된다. */
	bool ret;
	/* [한국어] blk_mq_sched_try_merge()의 병합 성공 여부를 담을 변수. */

	spin_lock_irq(&bfqd->lock);
	/* [한국어] bfqd 전역 상태(bio_bfqq/bio_bic, sort_list 등)를 건드리므로
	 * 인터럽트를 막고 락을 획득 - 완료 인터럽트 컨텍스트와의 경쟁 방지. */

	if (bic) {
	/* [한국어] io_cq를 찾았다면 - 프로세스의 cgroup이 바뀌었을 수
	 * 있으니 병합 판단 전에 최신 상태로 갱신하고, bio에 대응하는
	 * bfq_queue를 찾아 캐시한다. */
	/* [한국어] (계속) 아래에서 cgroup 갱신 및 bfq_queue 조회를 진행한다. */
		/*
		 * Make sure cgroup info is uptodate for current process before
		 * considering the merge.
		 */
		bfq_bic_update_cgroup(bic, bio);
		/* [한국어] 프로세스의 cgroup이 마지막 확인 이후 바뀌었는지 검사
		 * 하고, 바뀌었다면 bic가 참조하는 bfq_group을 새 cgroup으로
		 * 교체한다. */

		/* [한국어] bic로부터 이 bio의 동기 여부와, 이 bio가 속한 actuator
		 * (다중 액추에이터 HDD의 독립 접근 영역, 단일 액추에이터/NVMe는
		 * 항상 0)를 키로 대응하는 bfq_queue를 조회해 캐시한다 - 이후
		 * bfq_find_rq_fmerge()가 재사용. */
		bfqd->bio_bfqq = bic_to_bfqq(bic, op_is_sync(bio->bi_opf),
					     bfq_actuator_index(bfqd, bio));
	} else {
	/* [한국어] io_cq를 찾지 못했다면(초기화 전이거나 특수 상황) 병합
	 * 후보를 찾을 근거가 없으므로 캐시를 비운다. */
	/* [한국어] (계속) bio_bfqq 캐시를 비운다. */
		bfqd->bio_bfqq = NULL;
		/* [한국어] bio에 대응하는 bfqq 없음으로 표시. */
	}
	bfqd->bio_bic = bic;
	/* [한국어] 이번 병합 시도에 사용한 io_cq도 캐시 - 이후 단계에서
	 * 재사용될 수 있다. */

	ret = blk_mq_sched_try_merge(q, bio, nr_segs, &free);
	/* [한국어] blk-mq 공통 병합 로직에 위임 - 내부적으로 elevator_ops의
	 * request_merge/request_merged 콜백(bfq_request_merge 등)을 호출해
	 * 실제 rb-tree/해시 기반 병합을 수행한다. */

	spin_unlock_irq(&bfqd->lock);
	/* [한국어] 병합 판단이 끝났으므로 락 해제. */
	/* [한국어] 병합 과정에서 더 이상 필요 없어진 request가 있다면
	 * (예: 두 request 병합 후 하나를 반납) blk-mq에 반환한다. */
	if (free)
		blk_mq_free_request(free);

	return ret;
	/* [한국어] 병합 성공 여부를 호출자(blk_mq_submit_bio)에 돌려준다 -
	 * true면 bio가 기존 request에 흡수되어 새 request가 필요 없다. */
}

/*
 * [한국어]
 * bfq_request_merge - elevator_ops.request_merge 콜백. bio가 front-merge
 * (기존 request 앞에 붙는 병합) 대상이 될 수 있는지 판정한다.
 *
 * @q: request_queue.
 * @req: [출력] 병합 대상으로 판정된 기존 request를 담아 반환.
 * @bio: 병합을 시도 중인 새 bio.
 * @return: ELEVATOR_FRONT_MERGE(앞쪽 병합 가능), ELEVATOR_DISCARD_MERGE
 *          (discard 요청끼리의 병합), 또는 ELEVATOR_NO_MERGE(병합 불가).
 *
 * bfq_bio_merge()가 호출한 blk_mq_sched_try_merge()의 내부 단계로, 이미
 * bfqd->bio_bfqq에 캐시된 큐에서 bfq_find_rq_fmerge()로 후보를 찾고,
 * elv_bio_merge_ok()로 병합 가능 조건(같은 방향, 크기 제한 등)을 검증한다.
 * bfqd->lock을 쥔 채(bfq_bio_merge 경로) 호출된다.
 *
 * 호출 체인:
 *   blk_mq_sched_try_merge → elevator_ops.request_merge
 *   → [bfq_request_merge] → bfq_find_rq_fmerge / elv_bio_merge_ok
 */
static int bfq_request_merge(struct request_queue *q, struct request **req,
			     struct bio *bio)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	/* [한국어] 전역 BFQ 상태. */
	struct request *__rq;
	/* [한국어] bfq_find_rq_fmerge가 찾아준 후보 request. */

	__rq = bfq_find_rq_fmerge(bfqd, bio, q);
	/* [한국어] bio의 끝 섹터와 정확히 맞물리는 request를 bio_bfqq의
	 * sort_list에서 찾는다(front-merge 후보). */
	if (__rq && elv_bio_merge_ok(__rq, bio)) {
	/* [한국어] 후보가 있고, elv_bio_merge_ok()가 실제로 병합 가능
	 * (같은 cgroup, 같은 데이터 방향, 보안/무결성 속성 호환 등)이라고
	 * 확인해 준 경우에만 병합을 진행한다. */
	/* [한국어] (계속) 아래에서 req 포인터를 채우고 병합 종류를 결정한다. */
		*req = __rq;
		/* [한국어] 호출자에게 병합 대상 request를 돌려준다. */

		/* [한국어] 이 request가 discard(TRIM) 요청이고 병합 가능한 형태라면,
		 * 일반 front-merge와 다른 discard 전용 병합 타입으로 알린다 -
		 * discard는 데이터가 아닌 범위 병합이기 때문. */
		if (blk_discard_mergable(__rq))
			return ELEVATOR_DISCARD_MERGE;
			/* [한국어] discard 전용 병합 타입을 알린다. */
			/* [한국어] discard가 아니라면 아래에서 일반 front-merge로 판정한다. */
		return ELEVATOR_FRONT_MERGE;
		/* [한국어] discard가 아닌 일반적인 경우 - bio가 __rq 앞쪽에 이어
		 * 붙는 front-merge로 판정해 결과를 호출자에게 반환한다. */
		/* [한국어] (계속) if 블록이 끝났으므로 아래에서 병합 실패 경로를
		 * 처리한다. */
	}

	/* [한국어] 후보가 없거나 병합 조건을 만족하지 못하면 병합 불가 -
	 * 이 bio는 결국 새 request로 만들어져 삽입된다. */
	return ELEVATOR_NO_MERGE;
	/* [한국어] 병합 불가 결과를 호출자에게 반환. */
}

/*
 * [한국어]
 * bfq_request_merged - elevator_ops.request_merged 콜백. front-merge가
 * 실제로 성사된 직후 호출되어, 병합으로 섹터 위치가 바뀐 request를
 * bfqq의 자료구조에서 재정렬한다.
 *
 * @q: request_queue.
 * @req: 병합 결과로 확장된 request(병합 전 rb-tree에 있던 그 request).
 * @type: 이번에 일어난 병합의 종류(ELEVATOR_FRONT_MERGE 등).
 * @return: 없음.
 *
 * front-merge는 새 bio가 req의 시작 섹터보다 앞쪽에 붙는 병합이므로,
 * req의 시작 섹터 값 자체가 줄어든다. 그 결과 rb-tree(sort_list)에서
 * req의 정렬 위치가 앞의 노드보다 작아져 트리 불변식이 깨질 수 있어,
 * 이 함수가 req를 트리에서 빼서 다시 넣어(reposition) 정렬을 복구한다.
 * 이어서 next_rq 후보도 다시 계산한다. bfqd->lock을 쥔 채 호출된다.
 *
 * 호출 체인:
 *   blk_mq_sched_try_merge → elevator_ops.request_merged
 *   → [bfq_request_merged] → elv_rb_del/elv_rb_add → bfq_updated_next_req
 */
static void bfq_request_merged(struct request_queue *q, struct request *req,
			       enum elv_merge type)
{
	/* [한국어] 이번 병합이 front-merge였고, req가 rb-tree에서 이전
	 * 노드(rb_prev)를 가지며, 병합으로 줄어든 req의 시작 섹터가 그
	 * 이전 노드의 섹터보다 작아졌다면 - rb-tree의 정렬 불변식이 깨진
	 * 것이므로 재정렬이 필요하다. */
	if (type == ELEVATOR_FRONT_MERGE &&
	    rb_prev(&req->rb_node) &&
	    blk_rq_pos(req) <
	    blk_rq_pos(container_of(rb_prev(&req->rb_node),
				    struct request, rb_node))) {
		struct bfq_queue *bfqq = RQ_BFQQ(req);
		/* [한국어] req가 속한 bfq_queue. */
		struct bfq_data *bfqd;
		/* [한국어] bfqq가 속한 전역 BFQ 상태(아래에서 초기화). */
		struct request *prev, *next_rq;
		/* [한국어] prev: 재계산 전 next_rq 값. next_rq: 재계산된 새 후보. */

		if (!bfqq)
		/* [한국어] 이 request가 어떤 이유로든 bfq_queue에 속해 있지 않다면
		 * (비정상 상태) 더 진행할 수 없으므로 반환한다. */
			return;

		/* [한국어] bfqq로부터 전역 bfqd를 얻는다. */
		bfqd = bfqq->bfqd;

		/* Reposition request in its sort_list */
		/* [한국어] 정렬이 깨진 req를 일단 rb-tree에서 제거한다. */
		elv_rb_del(&bfqq->sort_list, req);
		/* [한국어] 병합으로 바뀐 새 섹터 값 기준으로 다시 삽입해 정렬을
		 * 복구한다. */
		elv_rb_add(&bfqq->sort_list, req);

		/* Choose next request to be served for bfqq */
		/* [한국어] 재계산 전 next_rq를 보존해 변경 여부 비교에 사용. */
		prev = bfqq->next_rq;
		/* [한국어] 재정렬된 req와 기존 next_rq 후보 중 last_position
		 * 기준으로 더 나은 쪽을 다시 선택한다. */
		next_rq = bfq_choose_req(bfqd, bfqq->next_rq, req,
					 bfqd->last_position);
		/* [한국어] 새 next_rq 후보를 반영한다. */
		bfqq->next_rq = next_rq;
		/* [한국어] next_rq 갱신이 끝났으므로 아래에서 변경 여부에 따라
		 * 예산/위치 트리를 갱신한다. */
		/*
		 * If next_rq changes, update both the queue's budget to
		 * fit the new request and the queue's position in its
		 * rq_pos_tree.
		 */
		if (prev != bfqq->next_rq) {
		/* [한국어] next_rq가 실제로 바뀌었다면 예산과 위치 트리 등록을
		 * 함께 갱신해야 일관성이 유지된다. */
		/* [한국어] (계속) 아래에서 예산을 재계산한다. */
			bfq_updated_next_req(bfqd, bfqq);
			/* [한국어] 새 next_rq 크기에 맞춰 bfqq의 예산을 재계산한다. */
			/*
			 * See comments on bfq_pos_tree_add_move() for
			 * the unlikely().
			 */
			/* [한국어] NVMe 같은 큐잉 지원 비회전형 장치가 아닐 때만 위치
			 * 트리 재배치를 수행한다(회전형 HDD 등에서 cooperator 탐색
			 * 정확도를 유지하기 위함). */
			if (unlikely(!bfqd->nonrot_with_queueing))
				bfq_pos_tree_add_move(bfqd, bfqq);
		}
	}
}

/*
 * This function is called to notify the scheduler that the requests
 * rq and 'next' have been merged, with 'next' going away.  BFQ
 * exploits this hook to address the following issue: if 'next' has a
 * fifo_time lower that rq, then the fifo_time of rq must be set to
 * the value of 'next', to not forget the greater age of 'next'.
 *
 * NOTE: in this function we assume that rq is in a bfq_queue, basing
 * on that rq is picked from the hash table q->elevator->hash, which,
 * in its turn, is filled only with I/O requests present in
 * bfq_queues, while BFQ is in use for the request queue q. In fact,
 * the function that fills this hash table (elv_rqhash_add) is called
 * only by bfq_insert_request.
 */
/*
 * [한국어]
 * bfq_requests_merged - elevator_ops.requests_merged 콜백. 두 request가
 * blk-mq 계층에서 하나로 합쳐져 'next'가 사라질 때 BFQ 쪽 상태를 정리한다.
 *
 * @q: request_queue.
 * @rq: 병합 후에도 살아남는 request(다른 request의 데이터를 흡수).
 * @next: 병합되어 사라질 request. 이 함수가 끝나면 next는 BFQ의 어떤
 *        자료구조에도 남아있지 않아야 한다.
 * @return: 없음.
 *
 * rq와 next가 같은 fifo_time(도착 순서 타임스탬프)을 공유해야 하는데,
 * next가 rq보다 오래된 request였다면(fifo_time이 더 작았다면) 그 나이
 * 정보를 잃지 않도록 rq의 fifo_time을 next의 것으로 대체한다. 이어서
 * bfqq->next_rq가 next를 가리키고 있었다면 rq로 교체하고, 마지막으로
 * bfq_remove_request()를 호출해 next를 sort_list/해시/fifo에서 완전히
 * 제거한다. bfqd->lock을 쥔 채 호출된다. 이 함수는 rq가 반드시
 * bfq_queue에 속해 있다고 가정하는데, 이는 q->elevator->hash에 등록된
 * request는 항상 bfq_queue 소속이기 때문이다(그 해시를 채우는
 * elv_rqhash_add는 bfq_insert_request에서만 호출됨).
 *
 * 호출 체인:
 *   blk_mq_sched_try_merge → elevator_ops.requests_merged
 *   → [bfq_requests_merged] → bfq_remove_request
 */
static void bfq_requests_merged(struct request_queue *q, struct request *rq,
				struct request *next)
{
	/* [한국어] bfqq: rq가 속한 큐. next_bfqq: next가 속한 큐(다를
	 * 수도 있으나 실제로는 대부분 같은 큐 - blk-mq가 인접 request만
	 * 병합한다). */
	struct bfq_queue *bfqq = RQ_BFQQ(rq),
		*next_bfqq = RQ_BFQQ(next);

	if (!bfqq)
	/* [한국어] rq가 bfq_queue에 속해 있지 않다면(비정상/과도기 상태)
	 * fifo 재조정은 건너뛰고 바로 next 제거 단계(remove 레이블)로
	 * 이동한다. */
		goto remove;
		/* [한국어] bfqq가 없으므로 fifo 재배치는 의미가 없다 - remove
		 * 레이블로 건너뛰어 next 제거만 수행한다. */

	/*
	 * If next and rq belong to the same bfq_queue and next is older
	 * than rq, then reposition rq in the fifo (by substituting next
	 * with rq). Otherwise, if next and rq belong to different
	 * bfq_queues, never reposition rq: in fact, we would have to
	 * reposition it with respect to next's position in its own fifo,
	 * which would most certainly be too expensive with respect to
	 * the benefits.
	 */
	/* [한국어] 같은 bfqq에 속하고, 둘 다 fifo 리스트에 실제로 연결되어
	 * 있으며, next가 rq보다 먼저 도착했다면(fifo_time이 더 작음) -
	 * rq가 next의 '더 오래됨' 정보를 이어받아야 한다. */
	if (bfqq == next_bfqq &&
	    !list_empty(&rq->queuelist) && !list_empty(&next->queuelist) &&
	    next->fifo_time < rq->fifo_time) {
		list_del_init(&rq->queuelist);
		/* [한국어] rq를 현재 fifo 위치에서 제거한다. */
		list_replace_init(&next->queuelist, &rq->queuelist);
		/* [한국어] next가 있던 fifo 자리에 rq를 대신 넣는다 - next의
		 * 리스트 노드는 초기화되어 더 이상 연결되지 않는다. */
		rq->fifo_time = next->fifo_time;
		/* [한국어] rq의 fifo 타임스탬프를 next의 것(더 이른 시각)으로
		 * 갱신 - 이후 fifo 만료 검사에서 실제 '가장 오래된 대기 시간'
		 * 기준으로 판단되도록 한다. */
	}

	/* [한국어] bfqq가 다음 서비스 후보로 next를 가리키고 있었다면,
	 * next는 곧 사라지므로 rq로 후보를 교체해야 한다. */
	if (bfqq->next_rq == next)
		bfqq->next_rq = rq;
		/* [한국어] next_rq 후보를 rq로 교체. */

	bfqg_stats_update_io_merged(bfqq_group(bfqq), next->cmd_flags);
	/* [한국어] cgroup 통계(blkio) 쪽에 '요청 병합 발생'을 기록 -
	 * blkio 컨트롤러가 병합 횟수를 통계로 노출하기 위함. */
remove:
	/* Merged request may be in the IO scheduler. Remove it. */
	if (!RB_EMPTY_NODE(&next->rb_node)) {
	/* [한국어] next의 rb_node가 아직 어떤 트리에도 연결되어 있다면
	 * (아직 BFQ의 sort_list에 남아 있다는 뜻) - 완전히 제거해야
	 * dangling 포인터나 이중 스케줄링을 방지할 수 있다. */
	/* [한국어] (계속) 아래에서 next를 스케줄러에서 완전히 제거한다. */
		bfq_remove_request(next->q, next);
		/* [한국어] next를 sort_list/fifo/해시/위치 트리 등 BFQ의 모든
		 * 자료구조에서 제거한다. 이 시점의 next는 이미 드라이버로
		 * 내려간 것이 아니라 아직 대기 중이던 request이며, 여기서
		 * 제거되는 것은 스케줄러 내부 큐에서일 뿐 드라이버 완료 경로와는
		 * 무관하다. */
		/* [한국어] next가 속했던 큐가 존재한다면(next_bfqq) 그 cgroup
		 * 통계에도 '요청 제거'를 반영한다. */
		if (next_bfqq)
			bfqg_stats_update_io_remove(bfqq_group(next_bfqq),
						    next->cmd_flags);
	}
}

/* Must be called with bfqq != NULL */
/*
 * [한국어]
 * bfq_bfqq_end_wr - bfqq 하나의 weight-raising(가중치 상승) 기간을
 * 즉시 종료시키고 원래 가중치로 되돌린다.
 *
 * @bfqq: weight-raising을 종료시킬 대상 큐. NULL이 아니어야 한다(위
 *        "Must be called with bfqq != NULL" 주석 참고).
 * @return: 없음.
 *
 * BFQ는 interactive/soft-rt 워크로드에 일시적으로 높은 가중치(wr_coeff)를
 * 부여해 응답성을 높이는데, 그 기간이 끝나면(타임아웃 또는 명시적 호출로)
 * 원래 가중치(1)로 복귀시켜야 공정성이 회복된다. 이 함수는 (1) 이 큐가
 * interactive 승격이었다면(soft-rt 최대 시간과 다르면) soft_rt_next_start를
 * 리셋해 향후 soft-rt로도 재평가될 수 있게 하고, (2) 큐가 현재 busy면
 * 전역 wr_busy_queues 카운터를 감소시키고, (3) wr_coeff/wr_cur_max_time을
 * 초기값으로 되돌리고, (4) last_wr_start_finish를 갱신하고, (5) 엔티티의
 * prio_changed 플래그를 세워 다음 스케줄링 갱신 시 실제 가중치가
 * 재계산되게 한다. bfqd->lock을 쥔 채 호출되어야 한다.
 *
 * 호출 체인:
 *   bfq_end_wr_async_queues / bfq_end_wr → [bfq_bfqq_end_wr]
 *   → (entity.prio_changed를 통해 __bfq_entity_update_weight_prio)
 */
static void bfq_bfqq_end_wr(struct bfq_queue *bfqq)
{
	/*
	 * If bfqq has been enjoying interactive weight-raising, then
	 * reset soft_rt_next_start. We do it for the following
	 * reason. bfqq may have been conveying the I/O needed to load
	 * a soft real-time application. Such an application actually
	 * exhibits a soft real-time I/O pattern after it finishes
	 * loading, and finally starts doing its job. But, if bfqq has
	 * been receiving a lot of bandwidth so far (likely to happen
	 * on a fast device), then soft_rt_next_start now contains a
	 * high value that. So, without this reset, bfqq would be
	 * prevented from being possibly considered as soft_rt for a
	 * very long time.
	 */

	/* [한국어] 현재 적용 중인 wr 최대 지속시간이 'soft-rt 전용
	 * 최대시간(bfq_wr_rt_max_time)'과 다르다는 것은 이 큐가 soft-rt가
	 * 아니라 interactive 사유로 승격되어 있었다는 뜻 - 이 경우 위
	 * 주석대로 soft_rt_next_start를 리셋해 향후 soft-rt 재평가
	 * 기회를 막지 않는다. */
	if (bfqq->wr_cur_max_time !=
	    bfqq->bfqd->bfq_wr_rt_max_time)
		bfqq->soft_rt_next_start = jiffies;

	/* [한국어] 이 큐가 현재 활성(busy) 상태라면, 전역 wr 활성 큐
	 * 카운터도 하나 줄여야 정확한 집계가 유지된다. */
	if (bfq_bfqq_busy(bfqq))
		bfqq->bfqd->wr_busy_queues--;
	/* [한국어] weight-raising 계수를 1(가중치 상승 없음, 원래
	 * 가중치)로 되돌린다. */
	bfqq->wr_coeff = 1;
	/* [한국어] 남은 wr 지속시간을 0으로 만들어 '더 이상 wr 기간이
	 * 아님'을 명시한다. */
	bfqq->wr_cur_max_time = 0;
	/* [한국어] wr가 '끝난' 시각을 기록 - 이후 재승격 여부 판단(예:
	 * bfq_wr_min_inter_arr_async 간격 계산)의 기준점이 된다. */
	bfqq->last_wr_start_finish = jiffies;
	/*
	 * Trigger a weight change on the next invocation of
	 * __bfq_entity_update_weight_prio.
	 */
	/* [한국어] 엔티티(B-WF2Q+ 스케줄링 트리 노드) 가중치가 바뀌었음을
	 * 표시 - 다음 __bfq_entity_update_weight_prio 호출 시 실제로
	 * 트리상의 타임스탬프/가중치를 원래 값으로 재계산하도록 트리거. */
	bfqq->entity.prio_changed = 1;
}

/*
 * [한국어]
 * bfq_end_wr_async_queues - 한 bfq_group(cgroup) 안의 모든 비동기 큐에
 * 대해 weight-raising을 일괄 종료시킨다.
 *
 * @bfqd: 전역 BFQ 상태(actuator 개수 num_actuators 확인용).
 * @bfqg: weight-raising을 종료할 대상 cgroup의 bfq_group.
 * @return: 없음.
 *
 * 비동기 큐(async_bfqq)는 우선순위 클래스 방향 인덱스(i)와 ioprio
 * 레벨(j), 그리고 다중 액추에이터 하드디스크의 독립 접근 영역(k,
 * actuator index)별로 별도로 존재한다. 이 함수는 그 3중 배열 전체와
 * async_idle_bfqq까지 순회하며 존재하는 모든 비동기 큐에
 * bfq_bfqq_end_wr()을 적용한다. bfqd->lock을 쥔 채(bfq_end_wr 또는
 * cgroup 종료 경로) 호출되어야 한다.
 *
 * 호출 체인:
 *   bfq_end_wr / bfqg_put(cgroup 정리) → [bfq_end_wr_async_queues]
 *   → bfq_bfqq_end_wr (각 async 큐마다)
 */
void bfq_end_wr_async_queues(struct bfq_data *bfqd,
			     struct bfq_group *bfqg)
{
	int i, j, k;
	/* [한국어] i: 큐 방향성 인덱스(우선순위 클래스 그룹), j:
	 * IOPRIO_NR_LEVELS 만큼의 ioprio 레벨, k: actuator(다중
	 * 액추에이터 하드디스크의 독립 헤드) 인덱스. */

	for (k = 0; k < bfqd->num_actuators; k++) {
	/* [한국어] 장치가 지원하는 액추에이터 개수만큼 반복 - 일반
	 * NVMe/단일 액추에이터 HDD는 num_actuators==1이라 한 번만
	 * 돈다. 다중 액추에이터 HDD(예: Seagate MACH.2)에서는
	 * 액추에이터별로 독립된 비동기 큐 집합이 있어 각각 처리한다. */
	/* [한국어] 아래 이중 루프로 (i,j,k) 조합의 모든 비동기 큐를 순회한다. */
		for (i = 0; i < 2; i++)
			for (j = 0; j < IOPRIO_NR_LEVELS; j++)
				/* [한국어] 이 (i,j,k) 조합에 해당하는 비동기 큐가 실제로 생성되어
				 * 있으면(지연 생성이라 NULL일 수 있음) wr을 종료시킨다. */
				if (bfqg->async_bfqq[i][j][k])
					bfq_bfqq_end_wr(bfqg->async_bfqq[i][j][k]);
		/* [한국어] 이 액추에이터의 idle 클래스 비동기 큐도 존재하면
		 * 마찬가지로 wr을 종료시킨다. */
		if (bfqg->async_idle_bfqq[k])
			bfq_bfqq_end_wr(bfqg->async_idle_bfqq[k]);
	}
}

/*
 * [한국어]
 * bfq_end_wr - 스케줄러 전체(모든 cgroup, 모든 큐)의 weight-raising을
 * 강제로 즉시 종료시킨다.
 *
 * @bfqd: 전역 BFQ 스케줄러 상태.
 * @return: 없음.
 *
 * 사용자가 sysfs를 통해 low_latency 옵션을 끄는 등, wr 정책 자체를
 * 비활성화할 때 호출되어 지금 진행 중인 모든 wr 기간을 일괄 종료한다.
 * 각 actuator별 active_list(busy 상태 큐 리스트)와 idle_list(idle 상태
 * 큐 리스트)를 모두 순회하며 bfq_bfqq_end_wr()을 적용하고, 마지막으로
 * bfq_end_wr_async()로 최상위 비동기 큐들도 정리한다. 함수 스스로
 * bfqd->lock을 잡고 푸는 top-level 진입점이다.
 *
 * 호출 체인:
 *   (sysfs low_latency 속성 write 핸들러) → [bfq_end_wr]
 *   → bfq_bfqq_end_wr / bfq_end_wr_async
 */
static void bfq_end_wr(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq;
	/* [한국어] 리스트 순회용 커서. */
	int i;
	/* [한국어] actuator 인덱스. */

	spin_lock_irq(&bfqd->lock);
	/* [한국어] active_list/idle_list와 각 bfqq의 wr 상태를 일관되게
	 * 수정해야 하므로 락을 잡고 인터럽트를 막는다. */

	for (i = 0; i < bfqd->num_actuators; i++) {
	/* [한국어] 액추에이터별로 독립된 active_list가 있으므로 각각
	 * 순회한다(단일 액추에이터/NVMe는 1회만 순회). */
	/* [한국어] 아래에서 이 actuator의 active_list를 순회한다. */
		list_for_each_entry(bfqq, &bfqd->active_list[i], bfqq_list)
		/* [한국어] 현재 busy(스케줄링 가능) 상태인 모든 큐에 대해 wr
		 * 종료를 적용한다. */
			bfq_bfqq_end_wr(bfqq);
	}
	/* [한국어] 현재 idle(비활성) 상태이지만 아직 존재하는 큐들도
	 * 마찬가지로 wr 상태를 정리해, 나중에 다시 활성화될 때 이전 wr
	 * 잔여 상태가 남아있지 않게 한다. */
	list_for_each_entry(bfqq, &bfqd->idle_list, bfqq_list)
		bfq_bfqq_end_wr(bfqq);
	/* [한국어] 개별 bfq_group에 속한 것이 아닌, 최상위(root cgroup)
	 * 비동기 큐들까지 포함해 정리한다. */
	bfq_end_wr_async(bfqd);

	/* [한국어] 모든 정리가 끝났으므로 락 해제. */
	spin_unlock_irq(&bfqd->lock);
}

/*
 * [한국어]
 * bfq_io_struct_pos - request 또는 bio 어느 쪽이든 관계 없이 '시작
 * 섹터 위치'를 통일된 방식으로 뽑아낸다.
 *
 * @io_struct: struct request* 또는 struct bio*를 가리키는 void 포인터.
 *             어떤 타입인지는 @request 인자로 구분한다.
 * @request: true면 io_struct를 struct request*로, false면 struct bio*로
 *           해석한다.
 * @return: 해당 구조체의 시작 섹터(sector_t).
 *
 * bfq_rq_close_to_sector()/bfqq_find_close() 등 cooperator 탐색 로직은
 * request와 bio를 동일한 방식으로 다뤄야 하는데, 두 타입은 섹터 필드의
 * 위치가 다르므로(request는 blk_rq_pos(), bio는 bi_iter.bi_sector) 이
 * 헬퍼가 그 차이를 감춘다. 순수 조회 함수로 상태 변경이 없다.
 *
 * 호출 체인:
 *   bfq_rq_close_to_sector → [bfq_io_struct_pos] → blk_rq_pos
 */
static sector_t bfq_io_struct_pos(void *io_struct, bool request)
{
	/* [한국어] request로 해석하라고 지정된 경우 - blk_rq_pos()로
	 * request의 시작 섹터를 얻는다(내부적으로 rq->__sector 참조). */
	if (request)
		return blk_rq_pos(io_struct);
		/* [한국어] bio로 해석하는 경우(아래 else) - bio_iter의 현재
		 * 섹터 위치를 그대로 반환한다(아직 request로 승격되기 전 원본
		 * bio의 위치). */
	else
		return ((struct bio *)io_struct)->bi_iter.bi_sector;
}

/*
 * [한국어]
 * bfq_rq_close_to_sector - request/bio의 위치가 주어진 섹터와 충분히
 * 가까운지(cooperator로 볼 만큼 인접한지) 판정한다.
 *
 * @io_struct: 비교 대상 request 또는 bio.
 * @request: io_struct의 실제 타입 구분 플래그(bfq_io_struct_pos 참고).
 * @sector: 비교 기준이 되는 섹터(보통 다른 큐의 next_rq 위치 등).
 * @return: 두 위치의 절대 거리가 BFQQ_CLOSE_THR(근접 판정 임계값, 섹터
 *          단위) 이하이면 true(1), 아니면 false(0).
 *
 * 서로 다른 프로세스가 디스크의 같은 영역을 향해 I/O를 내고 있는지
 * (LBA 인접성)를 판정하는 기본 단위 함수로, bfqq_find_close()가 이
 * 함수를 반복 호출해 근접한 큐를 찾는다. 순수 계산 함수.
 *
 * 호출 체인:
 *   bfqq_find_close → [bfq_rq_close_to_sector] → bfq_io_struct_pos
 */
static int bfq_rq_close_to_sector(void *io_struct, bool request,
				  sector_t sector)
{
	return abs(bfq_io_struct_pos(io_struct, request) - sector) <=
		/* [한국어] io_struct의 위치와 기준 섹터의 절대 거리를 구해,
		 * 미리 정의된 근접 임계값(BFQQ_CLOSE_THR) 이하이면 '가깝다'
		 * (참)를, 아니면 거짓을 반환한다 - 이 거리 안이면 순차 접근
		 * 이득을 볼 수 있다고 판단한다. */
	       BFQQ_CLOSE_THR;
}

/*
 * [한국어]
 * bfqq_find_close - 같은 cgroup의 위치 트리(rq_pos_tree)에서 주어진
 * 섹터에 가장 가까운 next_rq를 가진 bfq_queue를 찾는다.
 *
 * @bfqd: 전역 BFQ 상태(bfq_rq_pos_tree_lookup 호출에 필요).
 * @bfqq: 기준이 되는 현재 큐(같은 그룹의 위치 트리를 사용하기 위해
 *        그룹만 참조되며, 자기 자신이 결과에서 자동 제외되지는 않는다 -
 *        그 필터링은 호출자인 bfq_find_close_cooperator가 담당).
 * @sector: 근접도를 비교할 기준 섹터(보통 cur_bfqq의 마지막 요청 위치).
 * @return: 찾은 bfq_queue 포인터, 트리가 비어 있거나 근접한 큐가 없으면
 *          NULL.
 *
 * bfqq_group(bfqq)->rq_pos_tree는 같은 cgroup에 속한 모든 활성
 * bfq_queue를 각 큐의 next_rq 섹터 위치 기준으로 정렬해 둔 rb-tree다.
 * 이 함수는 (1) 정확히 일치하거나 바로 다음 위치를 찾아주는
 * bfq_rq_pos_tree_lookup으로 우선 시도하고, (2) 실패하면 탐색이 멈춘
 * 지점의 parent 노드를 기준으로 근접한지 확인하며, (3) 그래도 아니면
 * rb_next/rb_prev로 한 칸 더 옮겨가며 재확인한다. 호출자(결국
 * bfq_setup_cooperator)가 bfqd->lock을 쥔 상태에서 호출한다.
 *
 * 호출 체인:
 *   bfq_find_close_cooperator → [bfqq_find_close] → bfq_rq_pos_tree_lookup
 */
static struct bfq_queue *bfqq_find_close(struct bfq_data *bfqd,
					 struct bfq_queue *bfqq,
					 sector_t sector)
{
	struct rb_root *root = &bfqq_group(bfqq)->rq_pos_tree;
	/* [한국어] bfqq가 속한 cgroup(bfq_group)의 위치 트리 루트. */
	struct rb_node *parent, *node;
	/* [한국어] parent: 트리 탐색이 리프에서 멈췄을 때의 부모 노드
	 * (가장 근접한 후보). node: rb_next/rb_prev로 이동한 인접 노드. */
	struct bfq_queue *__bfqq;
	/* [한국어] 탐색 중 후보로 확인할 bfq_queue. */

	/* [한국어] 이 cgroup에 등록된 큐가 하나도 없으면(트리가 비어
	 * 있으면) 애초에 비교할 대상이 없으므로 즉시 NULL. */
	if (RB_EMPTY_ROOT(root))
		return NULL;
		/* [한국어] 트리가 비어 근접 큐를 찾을 수 없음을 알린다. */

	/*
	 * First, if we find a request starting at the end of the last
	 * request, choose it.
	 */
	__bfqq = bfq_rq_pos_tree_lookup(bfqd, root, sector, &parent, NULL);
	/* [한국어] sector와 정확히 일치하는(또는 그 바로 다음인) next_rq를
	 * 가진 bfq_queue를 rb-tree에서 찾는다. parent는 실패 시(NULL
	 * 반환 시) 탐색이 멈춘 위치의 부모 노드로 채워진다. */
	/* [한국어] 정확히 이어지는 큐를 찾았다면 - 가장 이상적인
	 * cooperator 후보이므로 바로 반환한다. */
	if (__bfqq)
		return __bfqq;
		/* [한국어] 정확한 일치를 찾았으므로 이 큐를 그대로 반환한다. */

	/*
	 * If the exact sector wasn't found, the parent of the NULL leaf
	 * will contain the closest sector (rq_pos_tree sorted by
	 * next_request position).
	 */
	__bfqq = rb_entry(parent, struct bfq_queue, pos_node);
	/* [한국어] 정확한 일치가 없었으므로, 탐색이 멈춘 NULL 리프의
	 * 부모 노드가 트리 정렬상 가장 가까운 섹터를 가진 큐다 -
	 * 이를 후보로 삼는다. */
	/* [한국어] 그 후보의 next_rq 위치가 실제로 임계값 이내로 가까운지
	 * 확인 - 트리상 '가장 가까움'이 곧 '충분히 가까움'을 보장하진
	 * 않으므로 별도 검증이 필요하다. */
	if (bfq_rq_close_to_sector(__bfqq->next_rq, true, sector))
		return __bfqq;
		/* [한국어] 임계값 이내로 가까우므로 이 후보를 채택한다. */

	/* [한국어] 후보의 위치가 기준보다 앞(작음)이라면, 기준보다 뒤에
	 * 있는 다음 노드 쪽을 마저 확인해봐야 한다(양쪽 이웃 모두 검토). */
	if (blk_rq_pos(__bfqq->next_rq) < sector)
		node = rb_next(&__bfqq->pos_node);
	/* [한국어] 후보의 위치가 기준보다 뒤라면 반대로 이전 노드를
	 * 확인한다. */
	else
		node = rb_prev(&__bfqq->pos_node);
	/* [한국어] 그 방향에 더 이상 노드가 없다면(트리의 끝) 더 볼
	 * 후보가 없으므로 NULL. */
	if (!node)
		return NULL;
		/* [한국어] 더 볼 후보가 없어 근접 큐를 찾지 못했음을 알린다. */

	__bfqq = rb_entry(node, struct bfq_queue, pos_node);
	/* [한국어] 반대편(또는 다음) 이웃 노드를 두 번째 후보로 삼는다. */
	if (bfq_rq_close_to_sector(__bfqq->next_rq, true, sector))
		return __bfqq;
		/* [한국어] 이 두 번째 후보가 임계값 이내로 가까우면 채택한다. */

	return NULL;
	/* [한국어] 양쪽 이웃 모두 충분히 가깝지 않으면 근접한 cooperator
	 * 후보가 없다고 결론짓는다. */
}

/*
 * [한국어]
 * bfq_find_close_cooperator - cur_bfqq와 LBA상 인접한 다른 큐(잠재적
 * cooperator)를 찾되, 자기 자신은 후보에서 제외한다.
 *
 * @bfqd: 전역 BFQ 상태.
 * @cur_bfqq: 병합을 고려 중인 현재 bfq_queue.
 * @sector: 근접도 비교 기준 섹터.
 * @return: 찾은 근접 큐(자기 자신이 아닌 것), 없으면 NULL.
 *
 * bfqq_find_close()는 트리 구조상 자기 자신을 반환할 수도 있으므로
 * (cur_bfqq 스스로가 트리에서 가장 가까운 노드일 수 있음), 이 얇은
 * 래퍼가 '자기 자신이면 무효'라는 추가 조건을 적용한다. cooperator를
 * 찾는 목적은 같은 영역을 오가는 여러 프로세스를 하나로 묶어 (1)
 * 불필요한 idling을 없애고 (2) 두 큐의 요청을 합쳐 처리량에 가장
 * 유리한 순서로 서비스하기 위함이다.
 *
 * 호출 체인:
 *   bfq_setup_cooperator → [bfq_find_close_cooperator] → bfqq_find_close
 */
static struct bfq_queue *bfq_find_close_cooperator(struct bfq_data *bfqd,
						   struct bfq_queue *cur_bfqq,
						   sector_t sector)
{
	struct bfq_queue *bfqq;
	/* [한국어] bfqq_find_close가 찾아준 후보를 담을 변수. */

	/*
	 * We shall notice if some of the queues are cooperating,
	 * e.g., working closely on the same area of the device. In
	 * that case, we can group them together and: 1) don't waste
	 * time idling, and 2) serve the union of their requests in
	 * the best possible order for throughput.
	 */
	bfqq = bfqq_find_close(bfqd, cur_bfqq, sector);
	/* [한국어] cur_bfqq가 속한 그룹의 위치 트리에서 sector에 가장
	 * 가까운 큐를 찾는다. */
	/* [한국어] 후보가 아예 없거나(!bfqq), 찾은 후보가 자기 자신
	 * (cur_bfqq)이라면 - 유효한 cooperator가 아니므로 NULL 반환.
	 * 자기 자신과의 '병합'은 의미가 없다. */
	if (!bfqq || bfqq == cur_bfqq)
		return NULL;
		/* [한국어] 유효한 cooperator가 아니므로 NULL을 반환한다. */

	return bfqq;
	/* [한국어] 자기 자신이 아닌 유효한 근접 큐를 cooperator 후보로
	 * 반환한다. */
}

static struct bfq_queue *
/*
 * [한국어]
 * bfq_setup_merge - bfqq를 new_bfqq로 실질적으로 merge(redirection)
 * 시키기 위한 유효성 검사와 참조 카운트 조정을 수행한다.
 *
 * @bfqq: merge의 '흡수되는' 쪽 큐 - 이 큐로 들어오던 프로세스들이
 *        이제 new_bfqq로 리다이렉트된다.
 * @new_bfqq: merge의 '흡수하는' 쪽 큐 - 실제로 요청을 계속 서비스할 큐.
 * @return: merge가 성사되면 new_bfqq(참조 카운트가 증가된 채로), 여러
 *          안전성 조건 중 하나라도 위반되면 NULL(merge 취소).
 *
 * BFQ의 queue merging은 실제로 두 큐의 요청을 자료구조 차원에서 합치는
 * 것이 아니라, 이 프로세스가 새로 내는 I/O를 앞으로 new_bfqq가 대신
 * 처리하게 리다이렉트하는 방식으로 이뤄진다. 이 함수는 (1) new_bfqq에
 * 이미 프로세스 참조가 없다면 ->new_bfqq 체인 자체가 신뢰할 수 없으므로
 * 즉시 실패, (2) 순환 참조를 피하기 위해 new_bfqq->new_bfqq 체인을 끝까지
 * 따라가며 도중에 bfqq 자신을 다시 만나면(순환) 실패, (3) 두 큐 중
 * 하나라도 이미 프로세스 참조가 0이면(소유 프로세스가 이미 사라짐) 실패,
 * (4) 두 큐의 스케줄링 엔티티 parent(같은 cgroup)가 다르면 실패, 이 모든
 * 조건을 통과하면 bfqq->new_bfqq = new_bfqq로 리다이렉션을 걸고 향후
 * bfqq에서 옮겨올 것으로 예상되는 프로세스 수만큼 new_bfqq->ref를 미리
 * 증가시킨다. bfqd->lock을 쥔 채(bfq_setup_cooperator/
 * bfq_setup_stable_merge 경로) 호출된다.
 *
 * 호출 체인:
 *   bfq_setup_cooperator / bfq_setup_stable_merge → [bfq_setup_merge]
 *   → bfqq_process_refs
 */
bfq_setup_merge(struct bfq_queue *bfqq, struct bfq_queue *new_bfqq)
{
	int process_refs, new_process_refs;
	/* [한국어] process_refs: bfqq를 실제 소유(참조)하는 프로세스 수.
	 * new_process_refs: new_bfqq를 소유하는 프로세스 수. */
	struct bfq_queue *__bfqq;
	/* [한국어] new_bfqq->new_bfqq 체인을 따라가며 검사할 임시 포인터. */

	/*
	 * If there are no process references on the new_bfqq, then it is
	 * unsafe to follow the ->new_bfqq chain as other bfqq's in the chain
	 * may have dropped their last reference (not just their last process
	 * reference).
	 */
	/* [한국어] new_bfqq를 실제로 소유하는 프로세스가 하나도 없다면,
	 * 체인 뒤쪽의 다른 bfqq들이 이미 완전히 해제됐을 수 있어
	 * ->new_bfqq를 계속 따라가는 것 자체가 위험하다 - 즉시 포기. */
	if (!bfqq_process_refs(new_bfqq))
		return NULL;
		/* [한국어] new_bfqq에 소유 프로세스가 없어 merge를 포기한다. */

	/* Avoid a circular list and skip interim queue merges. */
	while ((__bfqq = new_bfqq->new_bfqq)) {
	/* [한국어] new_bfqq가 이미 다른 큐로 리다이렉트되어 있다면
	 * (new_bfqq->new_bfqq != NULL), 체인의 끝(실제로 서비스하는
	 * 최종 큐)까지 따라간다 - 중간 단계 큐로 병합하는 것을 피하기
	 * 위함. */
	/* [한국어] 아래에서 체인 끝까지 이동하며 순환을 검사한다. */
		/* [한국어] 체인을 따라가다 다시 bfqq 자신을 만났다면 순환(cycle)이
		 * 생기는 것 - merge를 허용하면 무한 루프/일관성 붕괴로 이어지므로
		 * 실패 처리. */
		if (__bfqq == bfqq)
			return NULL;
			/* [한국어] 순환이 감지되어 merge를 포기한다. */
		new_bfqq = __bfqq;
		/* [한국어] 체인의 다음 단계로 이동해 계속 탐색한다. */
	}

	process_refs = bfqq_process_refs(bfqq);
	/* [한국어] bfqq를 소유한 프로세스 수를 확인. */
	new_process_refs = bfqq_process_refs(new_bfqq);
	/* [한국어] (체인 끝의) new_bfqq를 소유한 프로세스 수를 확인. */
	/*
	 * If the process for the bfqq has gone away, there is no
	 * sense in merging the queues.
	 */
	/* [한국어] 둘 중 하나라도 소유 프로세스가 이미 사라졌다면(예:
	 * 프로세스 종료 경합) merge할 실익이 없다 - 실패 처리. */
	if (process_refs == 0 || new_process_refs == 0)
		return NULL;
		/* [한국어] 소유 프로세스가 사라져 merge를 포기한다. */

	/*
	 * Make sure merged queues belong to the same parent. Parents could
	 * have changed since the time we decided the two queues are suitable
	 * for merging.
	 */
	/* [한국어] 병합을 결정한 시점 이후 cgroup이 바뀌어 두 큐의
	 * 스케줄링 엔티티 parent(소속 bfq_group)가 달라졌을 수 있다 -
	 * 서로 다른 cgroup의 큐를 합치면 대역폭 격리가 깨지므로 금지. */
	if (new_bfqq->entity.parent != bfqq->entity.parent)
		return NULL;
		/* [한국어] 소속 cgroup(parent)이 달라 merge를 포기한다. */

	/* [한국어] 어떤 pid의 큐와 병합을 예약하는지 트레이스 로그로
	 * 남긴다. */
	bfq_log_bfqq(bfqq->bfqd, bfqq, "scheduling merge with queue %d",
		new_bfqq->pid);

	/*
	 * Merging is just a redirection: the requests of the process
	 * owning one of the two queues are redirected to the other queue.
	 * The latter queue, in its turn, is set as shared if this is the
	 * first time that the requests of some process are redirected to
	 * it.
	 *
	 * We redirect bfqq to new_bfqq and not the opposite, because
	 * we are in the context of the process owning bfqq, thus we
	 * have the io_cq of this process. So we can immediately
	 * configure this io_cq to redirect the requests of the
	 * process to new_bfqq. In contrast, the io_cq of new_bfqq is
	 * not available any more (new_bfqq->bic == NULL).
	 *
	 * Anyway, even in case new_bfqq coincides with the in-service
	 * queue, redirecting requests the in-service queue is the
	 * best option, as we feed the in-service queue with new
	 * requests close to the last request served and, by doing so,
	 * are likely to increase the throughput.
	 */
	/* [한국어] 실제 리다이렉션 연결 - 이후 bfqq에 새 I/O가 도착하면
	 * 이 포인터를 통해 new_bfqq 쪽으로 넘겨진다(위 English 주석
	 * 설명 참고). */
	bfqq->new_bfqq = new_bfqq;
	/*
	 * The above assignment schedules the following redirections:
	 * each time some I/O for bfqq arrives, the process that
	 * generated that I/O is disassociated from bfqq and
	 * associated with new_bfqq. Here we increases new_bfqq->ref
	 * in advance, adding the number of processes that are
	 * expected to be associated with new_bfqq as they happen to
	 * issue I/O.
	 */
	/* [한국어] bfqq를 소유했던 프로세스 수만큼 new_bfqq의 참조
	 * 카운트를 미리 올려둔다 - 실제 재배정은 각 프로세스가 다음
	 * I/O를 낼 때 일어나지만, 그 사이에 new_bfqq가 조기 해제되지
	 * 않도록 참조를 선반영하는 것. */
	new_bfqq->ref += process_refs;
	/* [한국어] 병합이 성사된 대상 큐(공유 큐)를 호출자에게 반환한다. */
	return new_bfqq;
}

/*
 * [한국어]
 * bfq_may_be_close_cooperator - bfqq와 new_bfqq가 cooperator merge를
 * 시도해볼 만한 자격이 있는지 여러 조건으로 사전 필터링한다.
 *
 * @bfqq: 병합을 고려 중인 현재 큐.
 * @new_bfqq: 병합 후보로 발견된 다른 큐.
 * @return: 아래 조건을 모두 통과하면 true(병합 시도 가치가 있음), 하나
 *          라도 위반하면 false.
 *
 * 실제 LBA 근접성(bfq_rq_close_to_sector 등)과는 별개로, 애초에 병합이
 * 의미 없거나 해로운 조합을 걸러낸다: 병합 시도 시각이 이미 너무 늦었거나
 * (bfq_too_late_for_merging), 어느 한쪽이 idle 우선순위 클래스이거나
 * 서로 다른 ioprio 클래스이거나(다른 서비스 등급을 섞으면 안 됨), 이미
 * seeky로 판정된 큐가 있거나(순차 접근을 만들 가능성이 낮음), 어느
 * 한쪽이 비동기 큐이면(interleaved I/O 최적화는 read 워크로드에서만
 * 의미가 있음) 병합을 시도하지 않는다. 순수 판정 함수로 상태 변경이 없다.
 *
 * 호출 체인:
 *   bfq_setup_cooperator → [bfq_may_be_close_cooperator]
 *   → bfq_too_late_for_merging
 */
static bool bfq_may_be_close_cooperator(struct bfq_queue *bfqq,
					struct bfq_queue *new_bfqq)
{
	/* [한국어] new_bfqq가 이미 병합을 시도하기엔 너무 늦은 시점(예:
	 * 곧 만료되거나 이미 오래 서비스됨)이라면 병합 이득보다
	 * 오버헤드가 크다고 보고 거부한다. */
	if (bfq_too_late_for_merging(new_bfqq))
		return false;
		/* [한국어] 병합 시도 시점이 너무 늦어 거부한다. */

	/* [한국어] 둘 중 하나라도 idle 우선순위 클래스이거나(idle 큐는
	 * 다른 큐와 섞이면 서비스 보장이 왜곡됨), 두 큐의 ioprio
	 * 클래스(RT/BE/IDLE)가 서로 다르면 - 서비스 등급이 다른 I/O를
	 * 하나로 합치는 것은 우선순위 의미를 훼손하므로 거부한다. */
	if (bfq_class_idle(bfqq) || bfq_class_idle(new_bfqq) ||
	    (bfqq->ioprio_class != new_bfqq->ioprio_class))
		return false;
		/* [한국어] 우선순위 클래스가 다르거나 idle 클래스라서 거부한다. */

	/*
	 * If either of the queues has already been detected as seeky,
	 * then merging it with the other queue is unlikely to lead to
	 * sequential I/O.
	 */
	/* [한국어] 둘 중 하나라도 seeky(무작위 접근 패턴)로 판정됐다면,
	 * 합쳐도 순차 접근이 되기 어려우므로 병합의 핵심 이득(순차화로
	 * 인한 처리량 향상)을 기대할 수 없어 거부한다. */
	if (BFQQ_SEEKY(bfqq) || BFQQ_SEEKY(new_bfqq))
		return false;
		/* [한국어] seeky 큐가 있어 순차화 이득을 기대할 수 없으므로 거부한다. */

	/*
	 * Interleaved I/O is known to be done by (some) applications
	 * only for reads, so it does not make sense to merge async
	 * queues.
	 */
	/* [한국어] 둘 중 하나라도 비동기 큐라면 거부 - 인터리브드 I/O
	 * 최적화 대상은 경험적으로 read(동기) 워크로드에서만 관찰되므로
	 * write-back 등 비동기 큐를 병합하는 것은 의미가 없다. */
	if (!bfq_bfqq_sync(bfqq) || !bfq_bfqq_sync(new_bfqq))
		return false;
		/* [한국어] 비동기 큐가 섞여 있어 거부한다. */

	return true;
	/* [한국어] 모든 사전 필터를 통과했으므로 실제 LBA 근접성 등
	 * 다음 단계 판정으로 넘어갈 자격이 있다고 알린다. */
}

static bool idling_boosts_thr_without_issues(struct bfq_data *bfqd,
					     struct bfq_queue *bfqq);
/* [한국어] 전방 선언 - 실제 정의는 이 파일 뒤쪽(예산/idling 관련 섹션)에
 * 있다. bfqq에 대해 idling이 부작용 없이 처리량을 높일 수 있는 상태인지
 * 판정하며, 아래 bfq_setup_stable_merge()가 '이 큐는 이미 idling만으로도
 * 충분히 처리량을 얻고 있으니 merge까지 할 필요 없다'를 판단하는 데 쓴다. */

static struct bfq_queue *
/*
 * [한국어]
 * bfq_setup_stable_merge - 과거에 이미 안정적으로 병합됐던 이력이 있는
 * 큐(stable_merge_bfqq)와 bfqq를 다시 병합할지 결정하고 실행한다.
 *
 * @bfqd: 전역 BFQ 상태.
 * @bfqq: 병합을 고려 중인 현재 큐.
 * @stable_merge_bfqq: 과거 이 프로세스(또는 그 io_cq)와 안정적으로
 *        병합됐던 이력이 있는 큐 - bfq_setup_cooperator 쪽에서 미리
 *        골라 전달한다.
 * @bfqq_data: 이 프로세스의 io_cq 안에 있는, actuator별 병합 상태
 *        (stable_merge_bfqq/stably_merged 등)를 담은 구조체.
 * @return: 병합이 성사되면 그 결과 큐(new_bfqq), 아니면 NULL.
 *
 * '안정 병합(stable merge)'은 BFQ가 과거에 같은 두 프로세스를 성공적으로
 * 병합했던 경험을 기억해 두었다가, 다시 비슷한 상황이 되면(idling만으로는
 * 처리량이 충분치 않을 때) 굳이 처음부터 근접도 재탐색 없이 곧바로 병합을
 * 재시도하는 최적화다. idling만으로 이미 부작용 없이 충분한 처리량을 낼
 * 수 있는 상황이거나(idling_boosts_thr_without_issues) 어느 한쪽의
 * 프로세스 참조가 이미 0이면 병합을 시도하지 않고 out으로 건너뛴다.
 * 그렇지 않으면 bfq_setup_merge()로 실제 병합을 수행하고, 성공 시 양쪽의
 * actuator별 bfqq_data에 stably_merged 플래그를 세워 이 병합이 안정
 * 병합 경로로 이뤄졌음을 기록한다. 마지막에는 항상 stable_merge_bfqq에
 * 대해 쥐고 있던 참조(stable ref)를 반납한다. bfqd->lock을 쥔 채
 * 호출된다.
 *
 * 호출 체인:
 *   bfq_setup_cooperator → [bfq_setup_stable_merge] → bfq_setup_merge
 */
bfq_setup_stable_merge(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		       struct bfq_queue *stable_merge_bfqq,
		       struct bfq_iocq_bfqq_data *bfqq_data)
{
	/* [한국어] 두 큐 중 프로세스 참조 수가 더 적은 쪽 값을 취한다 -
	 * 어느 한쪽이라도 참조가 0에 가까우면(소유 프로세스가 사라지는
	 * 중) 병합 자체가 무의미하므로, 더 취약한 쪽 기준으로 안전하게
	 * 판단하기 위함. */
	int proc_ref = min(bfqq_process_refs(bfqq),
			   bfqq_process_refs(stable_merge_bfqq));
	/* [한국어] 병합 성공 시 결과 큐를 담을 변수 - 기본값은 '병합 안 됨'. */
	struct bfq_queue *new_bfqq = NULL;

	/* [한국어] 이번 시도로 안정 병합 후보 슬롯은 소진되므로(성공하든
	 * 실패하든) 미리 비워, 같은 후보로 중복 재시도되지 않게 한다. */
	bfqq_data->stable_merge_bfqq = NULL;
	/* [한국어] bfqq가 idling만으로 이미 부작용 없이 충분한 처리량을
	 * 내고 있다면 병합으로 얻을 추가 이득이 없고, proc_ref가 0이면
	 * 어느 한쪽 프로세스가 이미 사라져 병합할 대상 자체가 없다 -
	 * 두 경우 모두 병합을 포기하고 정리 단계(out)로 건너뛴다. */
	if (idling_boosts_thr_without_issues(bfqd, bfqq) || proc_ref == 0)
		goto out;
		/* [한국어] 병합을 포기하고 out 레이블(정리 단계)로 이동한다. */

	/* next function will take at least one ref */
	new_bfqq = bfq_setup_merge(bfqq, stable_merge_bfqq);
	/* [한국어] 실제 병합 가능 여부를 다시 한번 정밀 검증(순환/parent/
	 * 참조 등)하고, 통과하면 리다이렉션을 설정하는 bfq_setup_merge를
	 * 호출한다. 위 주석대로 성공 시 최소 1개 이상의 참조를
	 * stable_merge_bfqq 쪽에 추가로 가져간다. */

	if (new_bfqq) {
	/* [한국어] 병합이 실제로 성사됐다면(new_bfqq가 NULL이 아님) -
	 * 이 병합이 '안정 병합 경로'였음을 기록해 둔다. */
	/* [한국어] (계속) 아래에서 stably_merged 플래그를 세운다. */
		bfqq_data->stably_merged = true;
		/* [한국어] 현재 프로세스(bfqq 쪽) io_cq 데이터에 안정 병합 표시. */
		if (new_bfqq->bic) {
		/* [한국어] new_bfqq에 아직 자신의 io_cq(bic)가 연결되어 있다면
		 * (즉 new_bfqq 자신도 아직 어떤 프로세스에 직접 종속된 상태라면)
		 * 그쪽에도 동일하게 표시해 둔다. */
		/* [한국어] (계속) new_bfqq 쪽 actuator별 데이터도 표시한다. */
			unsigned int new_a_idx = new_bfqq->actuator_idx;
			/* [한국어] new_bfqq가 속한 액추에이터(다중 액추에이터 하드디스크의
			 * 독립 접근 영역) 인덱스 - 액추에이터별로 bfqq_data가 따로
			 * 있으므로 올바른 슬롯을 찾기 위해 필요하다. */
			/* [한국어] 이 인덱스로 new_bfqq 소유 프로세스의 actuator별 데이터를 찾는다. */
			/* [한국어] new_bfqq를 소유한 프로세스의 io_cq에서, 같은
			 * 액추에이터 인덱스에 해당하는 bfqq_data 슬롯을 찾는다. */
			struct bfq_iocq_bfqq_data *new_bfqq_data =
				&new_bfqq->bic->bfqq_data[new_a_idx];

			/* [한국어] new_bfqq 쪽에도 안정 병합 표시를 남겨, 이후 이
			 * 프로세스 쌍이 다시 분리되더라도 '예전에 안정 병합됐었다'는
			 * 이력을 참조할 수 있게 한다. */
			new_bfqq_data->stably_merged = true;
		}
	}

out:
	/* deschedule stable merge, because done or aborted here */
	/* [한국어] 함수 진입 전 stable_merge_bfqq에 대해 별도로 쥐고
	 * 있던 '안정 병합 예약용' 참조(stable ref)를 여기서 반납한다 -
	 * 병합이 성사됐든 포기됐든 이 예약 참조는 더 이상 필요 없다. */
	bfq_put_stable_ref(stable_merge_bfqq);

	/* [한국어] 병합 결과(성공 시 공유 큐, 실패 시 NULL)를 호출자에게
	 * 반환한다. */
	return new_bfqq;
}

/*
 * Attempt to schedule a merge of bfqq with the currently in-service
 * queue or with a close queue among the scheduled queues.  Return
 * NULL if no merge was scheduled, a pointer to the shared bfq_queue
 * structure otherwise.
 *
 * The OOM queue is not allowed to participate to cooperation: in fact, since
 * the requests temporarily redirected to the OOM queue could be redirected
 * again to dedicated queues at any time, the state needed to correctly
 * handle merging with the OOM queue would be quite complex and expensive
 * to maintain. Besides, in such a critical condition as an out of memory,
 * the benefits of queue merging may be little relevant, or even negligible.
 *
 * WARNING: queue merging may impair fairness among non-weight raised
 * queues, for at least two reasons: 1) the original weight of a
 * merged queue may change during the merged state, 2) even being the
 * weight the same, a merged queue may be bloated with many more
 * requests than the ones produced by its originally-associated
 * process.
 */
/*
 * [한국어]
 * bfq_setup_cooperator - 인접 LBA(Logical Block Address, 논리 블록 주소)에
 * 접근하는 서로 다른 프로세스들의 bfqq(bfq_queue)를 찾아 하나의 bfqq로
 * 병합(merge)할지 판단한다.
 *
 * @bfqd: 장치 전역 BFQ 스케줄러 상태. in_service_queue(현재 장치를 점유한
 *        bfqq), nonrot_with_queueing(비회전 매체+컨트롤러 내부 큐잉 지원
 *        여부) 등을 조회하는 데 쓰인다.
 * @bfqq: 병합을 시도할 기준(cur) bfqq. 새로 도착한 bio/request가 다른
 *        bfqq와 인접 LBA에 접근하는지 검사하는 주체가 된다.
 * @io_struct: 병합 판단 기준이 되는 struct bio * 또는 struct request *.
 *             실제 타입은 request 인자로 구분한다.
 * @request: true면 io_struct가 struct request *, false면 struct bio *.
 * @bic: 이 I/O를 발행한 프로세스의 bfq_io_cq. stable merge 후보
 *       (bfqq_data->stable_merge_bfqq)를 조회하는 데 사용한다.
 * @return: 병합 대상 bfq_queue 포인터. 이미 병합이 예약돼 있거나 새로
 *          병합 조건을 만족하면 그 bfqq를, 병합할 필요/자격이 없으면
 *          NULL을 반환한다.
 *
 * cooperator merge는 서로 다른 프로세스가 인접한 디스크 영역을 나눠
 * 접근하는 패턴(예: 여러 스레드가 같은 파일을 분담해서 순차적으로 읽는
 * 경우)을 감지해 하나의 bfqq로 합침으로써 B-WF2Q+ 스케줄링 오버헤드를
 * 줄이고 sequential I/O 패턴을 만들어 처리량을 높이는 최적화다. 다만
 * NVMe SSD처럼 nonrot_with_queueing(비회전 매체이면서 컨트롤러 차원의
 * 내부 큐잉/재정렬을 지원)이 true인 장치에서는, 컨트롤러가 이미 내부
 * NCQ/재정렬로 동일한 효과를 얻으므로 아래 bfqd->nonrot_with_queueing
 * 분기에서 항상 조기에 NULL을 반환하도록 되어 있다(코드로 확정된 동작).
 *
 * 동작 순서:
 *   1) bfqq->new_bfqq 체인이 이미 설정돼 있으면 체인 끝(최종 병합
 *      대상)까지 따라가 반환한다(이미 병합이 예약된 경우).
 *   2) 회전식 디스크이거나 컨트롤러 내부 큐잉이 없는 장치라면, 과거
 *      병합 이력이 있는 "stable merge" 후보가 있고 생성/분리 시각이
 *      충분히 지났으면 bfq_setup_stable_merge()로 위임한다.
 *   3) 장치가 nonrot_with_queueing이면 곧바로 NULL(가장 흔한 NVMe 경로,
 *      likely()로 분기 예측 최적화).
 *   4) bfqq가 너무 오래 전에 생성됐거나, io_struct가 없거나, oom_bfqq
 *      이거나, 활성 큐가 1개뿐이면 병합 후보를 찾을 필요가 없어 NULL.
 *   5) in_service_queue가 인접 LBA에 있고 같은 parent(같은 cgroup)이며
 *      협력 관계로 보이면 bfq_setup_merge()로 병합 시도.
 *   6) 그렇지 않으면 rq_pos_tree(섹터 정렬 rb-tree)에서 인접 위치의
 *      다른 bfqq를 찾아(bfq_find_close_cooperator) 같은 방식으로 시도.
 *
 * 실행 컨텍스트: bfqd->lock을 호출자(bfq_allow_bio_merge,
 * bfq_insert_request 등)가 이미 보유한 상태에서 호출된다. 재진입 없음.
 *
 * 호출 체인:
 *   bfq_allow_bio_merge / bfq_insert_request → [bfq_setup_cooperator]
 *   → bfq_setup_stable_merge / bfq_setup_merge / bfq_find_close_cooperator
 */
static struct bfq_queue *
bfq_setup_cooperator(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		     void *io_struct, bool request, struct bfq_io_cq *bic)
{
	struct bfq_queue *in_service_bfqq, *new_bfqq;
	unsigned int a_idx = bfqq->actuator_idx; // bfqq가 속한 actuator(멀티 액추에이터 HDD의 헤드 단위) 인덱스 - 병합은 반드시 같은 actuator 내 큐끼리만 검토
	struct bfq_iocq_bfqq_data *bfqq_data = &bic->bfqq_data[a_idx]; // 이 프로세스의 actuator별 저장 데이터(stable_merge_bfqq 등)에 접근

	/* if a merge has already been setup, then proceed with that first */
	new_bfqq = bfqq->new_bfqq; // bfqq가 과거에 이미 다른 bfqq로 병합 예약돼 있었는지 확인
	if (new_bfqq) { // 이미 병합이 예약된 경우 - 새로 후보를 찾지 않고 기존 병합 체인을 그대로 따라간다
		while (new_bfqq->new_bfqq) // new_bfqq 자신도 또 다른 큐로 재병합됐을 수 있으므로 체인의 끝까지 이동
			new_bfqq = new_bfqq->new_bfqq; // new_bfqq 링크는 항상 "합쳐진 뒤 살아남은 큐"를 가리키므로, 끝까지 따라가면 현재 실제로 request를 담고 있는 큐가 나온다(체인은 순환하지 않는다)
		return new_bfqq; // 체인의 최종 목적지 bfqq를 병합 대상으로 반환
	}

	/*
	 * Check delayed stable merge for rotational or non-queueing
	 * devs. For this branch to be executed, bfqq must not be
	 * currently merged with some other queue (i.e., bfqq->bic
	 * must be non null). If we considered also merged queues,
	 * then we should also check whether bfqq has already been
	 * merged with bic->stable_merge_bfqq. But this would be
	 * costly and complicated.
	 */
	if (unlikely(!bfqd->nonrot_with_queueing)) { // 회전식 디스크이거나 컨트롤러 내부 큐잉이 없는(단순) 장치인 경우에만 stable merge 후보를 검사 - NVMe처럼 내부 재정렬이 있는 장치는 여기로 들어오지 않음(unlikely)
		/*
		 * Make sure also that bfqq is sync, because
		 * bic->stable_merge_bfqq may point to some queue (for
		 * stable merging) also if bic is associated with a
		 * sync queue, but this bfqq is async
		 */
		if (bfq_bfqq_sync(bfqq) && bfqq_data->stable_merge_bfqq && // bfqq가 동기(sync) 큐이고, 과거에 병합했던 상대(stable_merge_bfqq) 기록이 남아 있으며
		    !bfq_bfqq_just_created(bfqq) && // bfqq가 방금 생성된 큐가 아니어야(초기 판단 노이즈 배제) 하고
		    time_is_before_jiffies(bfqq->split_time +
					  msecs_to_jiffies(bfq_late_stable_merging)) && // 마지막으로 분리(split)된 뒤 bfq_late_stable_merging(ms)이 충분히 지났고
		    time_is_before_jiffies(bfqq->creation_time +
					   msecs_to_jiffies(bfq_late_stable_merging))) { // bfqq가 생성된 뒤에도 그만큼 시간이 지났다면(너무 이른 재병합 방지)
			/* [한국어] 과거 병합 상대였던 bfqq를 지역 변수로 확보 */
			struct bfq_queue *stable_merge_bfqq =
				bfqq_data->stable_merge_bfqq;

			/* [한국어] stable merge 재시도는 별도 함수로 위임하고 그 결과를 그대로 반환 */
			return bfq_setup_stable_merge(bfqd, bfqq,
						      stable_merge_bfqq,
						      bfqq_data);
		}
	}

	/*
	 * Do not perform queue merging if the device is non
	 * rotational and performs internal queueing. In fact, such a
	 * device reaches a high speed through internal parallelism
	 * and pipelining. This means that, to reach a high
	 * throughput, it must have many requests enqueued at the same
	 * time. But, in this configuration, the internal scheduling
	 * algorithm of the device does exactly the job of queue
	 * merging: it reorders requests so as to obtain as much as
	 * possible a sequential I/O pattern. As a consequence, with
	 * the workload generated by processes doing interleaved I/O,
	 * the throughput reached by the device is likely to be the
	 * same, with and without queue merging.
	 *
	 * Disabling merging also provides a remarkable benefit in
	 * terms of throughput. Merging tends to make many workloads
	 * artificially more uneven, because of shared queues
	 * remaining non empty for incomparably more time than
	 * non-merged queues. This may accentuate workload
	 * asymmetries. For example, if one of the queues in a set of
	 * merged queues has a higher weight than a normal queue, then
	 * the shared queue may inherit such a high weight and, by
	 * staying almost always active, may force BFQ to perform I/O
	 * plugging most of the time. This evidently makes it harder
	 * for BFQ to let the device reach a high throughput.
	 *
	 * Finally, the likely() macro below is not used because one
	 * of the two branches is more likely than the other, but to
	 * have the code path after the following if() executed as
	 * fast as possible for the case of a non rotational device
	 * with queueing. We want it because this is the fastest kind
	 * of device. On the opposite end, the likely() may lengthen
	 * the execution time of BFQ for the case of slower devices
	 * (rotational or at least without queueing). But in this case
	 * the execution time of BFQ matters very little, if not at
	 * all.
	 */
	if (likely(bfqd->nonrot_with_queueing)) // NVMe SSD 등 컨트롤러가 내부적으로 요청을 재정렬하는 장치의 가장 흔한 경로 - likely()로 분기예측 최적화
		return NULL; // 이 장치에서는 merge를 아예 시도하지 않는다 - 컨트롤러 내부 스케줄러가 이미 동일한 순차화 효과를 내기 때문

	/*
	 * Prevent bfqq from being merged if it has been created too
	 * long ago. The idea is that true cooperating processes, and
	 * thus their associated bfq_queues, are supposed to be
	 * created shortly after each other. This is the case, e.g.,
	 * for KVM/QEMU and dump I/O threads. Basing on this
	 * assumption, the following filtering greatly reduces the
	 * probability that two non-cooperating processes, which just
	 * happen to do close I/O for some short time interval, have
	 * their queues merged by mistake.
	 */
	if (bfq_too_late_for_merging(bfqq)) // bfqq가 생성된 지 bfq_merge_time_limit(기본 HZ/10)보다 오래됐으면 진짜 협력 프로세스가 아닐 가능성이 높음
		return NULL; // 오탐(false positive) 병합을 막기 위해 병합 시도를 포기

	if (!io_struct || unlikely(bfqq == &bfqd->oom_bfqq)) // 판단 근거가 되는 bio/request가 없거나, bfqq가 OOM 상황에서 임시로 쓰는 공용 큐이면
		return NULL; // oom_bfqq는 상태 관리가 복잡해지므로 병합 후보에서 제외

	/* If there is only one backlogged queue, don't search. */
	if (bfq_tot_busy_queues(bfqd) == 1) // 현재 활성(backlogged) bfqq가 bfqq 자신 하나뿐이면
		return NULL; // 병합할 상대가 있을 수 없으므로 탐색을 생략해 오버헤드를 줄인다

	in_service_bfqq = bfqd->in_service_queue; // 현재 장치를 점유 중인 bfqq(다음 후보로 가장 먼저 검사)

	if (in_service_bfqq && in_service_bfqq != bfqq && // in-service 큐가 존재하고 bfqq 자신이 아니며
	    likely(in_service_bfqq != &bfqd->oom_bfqq) && // oom_bfqq가 아니고(가장 흔한 경우이므로 likely)
	    bfq_rq_close_to_sector(io_struct, request,
				   bfqd->in_serv_last_pos) && // 새 bio/request의 섹터가 in-service 큐가 마지막으로 처리한 위치와 인접하고
	    bfqq->entity.parent == in_service_bfqq->entity.parent && // 같은 cgroup(entity.parent)에 속해 있으며
	    bfq_may_be_close_cooperator(bfqq, in_service_bfqq)) { // seeky/ioprio class/sync 여부 등 기본 조건도 만족하면
		new_bfqq = bfq_setup_merge(bfqq, in_service_bfqq); // 실제 병합 자료구조(new_bfqq 체인, ref count)를 설정
		if (new_bfqq) // 병합이 성공적으로 예약됐다면
			return new_bfqq; // in-service 큐를 병합 대상으로 즉시 반환 - 가장 유리한 후보이므로 추가 탐색 불필요
	}
	/*
	 * Check whether there is a cooperator among currently scheduled
	 * queues. The only thing we need is that the bio/request is not
	 * NULL, as we need it to establish whether a cooperator exists.
	 */
	/* [한국어] in-service 큐가 후보가 아니었다면 rq_pos_tree에서 섹터 위치가 가장 가까운 다른 bfqq를 탐색 */
	new_bfqq = bfq_find_close_cooperator(bfqd, bfqq,
			bfq_io_struct_pos(io_struct, request));

	if (new_bfqq && likely(new_bfqq != &bfqd->oom_bfqq) && // 인접한 bfqq를 찾았고 oom_bfqq가 아니며
	    bfq_may_be_close_cooperator(bfqq, new_bfqq)) // 협력 조건도 만족하면
		return bfq_setup_merge(bfqq, new_bfqq); // 해당 bfqq와 병합을 설정하고 결과를 반환

	return NULL; // 어떤 협력 후보도 찾지 못함 - 병합 없이 원래 bfqq를 그대로 사용
}

/*
 * [한국어]
 * bfq_bfqq_save_state - bfqq가 다른 bfqq와 병합(merge)되어 사라지기 직전,
 * 나중에 분리(split)될 때 복원할 수 있도록 idle-window/weight-raising 등의
 * 상태를 bic->bfqq_data(스냅샷 저장 공간)에 백업한다.
 *
 * @bfqq: 병합으로 인해 곧 bic으로부터 연결이 끊길 bfqq. 이 함수 호출 후
 *        bfqq는 여전히 존재할 수 있지만(공유 큐 자격 유지), 프로세스가
 *        다시 분리될 때는 이 백업으로부터 상태를 복원해야 한다.
 * @return: 없음(void).
 *
 * bfqq가 cooperator merge로 다른 bfqq(new_bfqq)에 합쳐지면, bfqq에 쌓여
 * 있던 학습된 상태(think time, IO_bound 여부, weight-raising 진행 상황
 * 등)를 잃게 된다. 그런데 나중에 프로세스가 다시 독립적인 I/O 패턴을
 * 보이면 bfqq는 분리(split)되어 원래 상태로 되돌아가야 공정성이 유지된다.
 * 이 함수는 그 복원에 필요한 값들을 bic->bfqq_data[a_idx](이 프로세스
 * 전용 영역)에 미리 복사해 둔다.
 *
 * 동작:
 *   - bfqq->bic이 이미 NULL이면(이미 공유 큐이거나 이미 리다이렉트된
 *     경우) 저장할 것이 없으므로 바로 반환한다.
 *   - think time, injection limit, weight, IO_bound, large burst 소속
 *     여부 등 일반 필드를 그대로 복사한다.
 *   - weight-raising 관련 필드는 두 가지 경우로 나뉜다: (a) bfqq가 막
 *     생성되어 아직 실제로 weight-raising 상태에 들어가지 못했지만
 *     low_latency 모드에서는 원래 interactive WR을 받았어야 하는 경우,
 *     그 WR이 적용됐을 때의 값을 미리 계산해 저장한다(조기 병합으로
 *     인한 불이익 방지). (b) 그 외의 일반적인 경우는 현재 bfqq의 WR
 *     상태를 그대로 저장한다.
 *
 * 실행 컨텍스트: bfqd->lock을 호출자(bfq_merge_bfqqs)가 보유한 상태에서
 * 실행된다. bfqq/bic 모두 병합을 발행한 프로세스 컨텍스트에서만 접근.
 *
 * 호출 체인:
 *   bfq_merge_bfqqs → [bfq_bfqq_save_state]
 */
static void bfq_bfqq_save_state(struct bfq_queue *bfqq)
{
	struct bfq_io_cq *bic = bfqq->bic; // bfqq를 만든 프로세스의 io_cq - 병합 후 이 포인터를 통해 저장 공간을 찾는다
	unsigned int a_idx = bfqq->actuator_idx; // bfqq가 속한 actuator 인덱스 - 저장 공간도 actuator별로 분리되어 있음
	struct bfq_iocq_bfqq_data *bfqq_data = &bic->bfqq_data[a_idx]; // 이 프로세스/actuator 전용 스냅샷 저장 구조체

	/*
	 * If !bfqq->bic, the queue is already shared or its requests
	 * have already been redirected to a shared queue; both idle window
	 * and weight raising state have already been saved. Do nothing.
	 */
	if (!bic) // bfqq가 이미 공유 큐이거나 이미 다른 곳으로 리다이렉트되어 bic 연결이 끊긴 경우
		return; // 저장할 대상 프로세스가 없으므로 아무 것도 하지 않고 반환

	bfqq_data->saved_last_serv_time_ns = bfqq->last_serv_time_ns; // 마지막 서비스(dispatch~완료) 소요 시간 - injection 한계 재계산에 사용되므로 보존
	bfqq_data->saved_inject_limit =	bfqq->inject_limit; // 다른 큐의 요청을 얼마나 끼워넣기(inject) 허용했는지의 한계값 보존
	bfqq_data->saved_decrease_time_jif = bfqq->decrease_time_jif; // inject_limit을 마지막으로 낮춘 시각(jiffies) 보존 - 재평가 주기 유지

	bfqq_data->saved_weight = bfqq->entity.orig_weight; // 병합 전 원래 스케줄링 가중치 - 분리 시 이 가중치로 복원해야 공정성 유지
	bfqq_data->saved_ttime = bfqq->ttime; // 평균 think time(요청 간 유휴 시간) 통계 보존 - 병합 후에도 프로세스 패턴 학습을 이어가기 위함
	/* [한국어] "think time이 짧다(interactive 성향)" 플래그 - 분리 후 idle 정책 판단에 재사용 */
	bfqq_data->saved_has_short_ttime =
		bfq_bfqq_has_short_ttime(bfqq);
	bfqq_data->saved_IO_bound = bfq_bfqq_IO_bound(bfqq); // I/O-bound(끊임없이 요청을 내는) 프로세스 여부 플래그 보존
	bfqq_data->saved_io_start_time = bfqq->io_start_time; // 이 bfqq가 I/O를 시작한 시각 - soft real-time 판정 등에 사용되므로 보존
	bfqq_data->saved_tot_idle_time = bfqq->tot_idle_time; // 누적 유휴 시간 - burst/idle 통계 연속성 유지
	bfqq_data->saved_in_large_burst = bfq_bfqq_in_large_burst(bfqq); // "대량 큐 생성 burst"에 속했는지 여부 보존 - 분리 후에도 burst 판정을 이어감
	/* [한국어] burst_list에 실제로 연결(hashed)돼 있었는지 확인 - 아직 리스트에 남아있는 채로 병합됐는지 기록 */
	bfqq_data->was_in_burst_list =
		!hlist_unhashed(&bfqq->burst_list_node);

	/* [한국어] bfqq가 생성된 직후 곧바로 병합됐고(정상적으로 WR을 받을 기회가 없었음), burst가 아니며,
	 * low_latency 모드가 켜져 있다면 */
	if (unlikely(bfq_bfqq_just_created(bfqq) &&
		     !bfq_bfqq_in_large_burst(bfqq) &&
		     bfqq->bfqd->low_latency)) {
		/*
		 * bfqq being merged right after being created: bfqq
		 * would have deserved interactive weight raising, but
		 * did not make it to be set in a weight-raised state,
		 * because of this early merge.	Store directly the
		 * weight-raising state that would have been assigned
		 * to bfqq, so that to avoid that bfqq unjustly fails
		 * to enjoy weight raising if split soon.
		 */
		bfqq_data->saved_wr_coeff = bfqq->bfqd->bfq_wr_coeff; // 실제로는 못 받았지만 받았어야 할 WR(weight-raising) 계수를 대신 저장
		/* [한국어] soft-real-time 전환 시각을 "지금"으로 설정 - 아직 SRT 전환 이력이 없으므로 최솟값
		 * 사용 */
		bfqq_data->saved_wr_start_at_switch_to_srt =
			bfq_smallest_from_now();
		/* [한국어] 이 장치의 peak_rate 기반 WR 지속시간을 그대로 사용 */
		bfqq_data->saved_wr_cur_max_time =
			bfq_wr_duration(bfqq->bfqd);
		bfqq_data->saved_last_wr_start_finish = jiffies; // WR이 지금 막 시작한 것처럼 현재 시각을 기록
	} else { // 그 외 일반적인 경우 - 이미 WR 상태(또는 비-WR 상태)가 실제로 반영돼 있으므로 있는 그대로 저장
		bfqq_data->saved_wr_coeff = bfqq->wr_coeff; // 현재 적용 중인 WR 계수(1이면 WR 없음, >1이면 가중치 배수)를 그대로 보존
		/* [한국어] soft-real-time으로 전환된 시각을 그대로 보존 */
		bfqq_data->saved_wr_start_at_switch_to_srt =
			bfqq->wr_start_at_switch_to_srt;
		/* [한국어] WR 기간 동안 이미 받은 서비스량 보존 - 남은 WR 자격 계산에 필요 */
		bfqq_data->saved_service_from_wr =
			bfqq->service_from_wr;
		/* [한국어] 마지막 WR 시작/갱신 시각 보존 */
		bfqq_data->saved_last_wr_start_finish =
			bfqq->last_wr_start_finish;
		bfqq_data->saved_wr_cur_max_time = bfqq->wr_cur_max_time; // 이번 WR 구간에 적용 중인 최대 지속시간 보존
	}
}


/*
 * [한국어]
 * bfq_reassign_last_bfqq - "가장 최근에 생성된 bfqq" 포인터(last_bfqq_created)가
 * 병합으로 사라지는 cur_bfqq를 가리키고 있었다면 new_bfqq로 갱신한다.
 *
 * @cur_bfqq: 병합되어 사라지거나 재배치되는 원래 bfqq.
 * @new_bfqq: cur_bfqq 대신 참조를 넘겨받을 bfqq (병합 시 병합 대상,
 *            해제 시 NULL).
 * @return: 없음(void).
 *
 * BFQ는 "형제 큐들이 서로 가까운 시점에 생성됐는가"를 stable-merge 판단
 * 근거로 쓰기 위해, cgroup 엔티티(entity.parent) 또는 장치 전체
 * (bfqd) 단위로 마지막에 생성된 bfqq를 last_bfqq_created 필드에 계속
 * 추적해 둔다. cur_bfqq가 병합되어 사라지거나 참조가 해제될 때 이 추적
 * 포인터가 이미 사라질 큐를 가리킨 채로 남아 있으면 안 되므로, 이
 * 함수가 그 포인터를 new_bfqq(병합 시 병합 대상, 해제 시 NULL)로
 * 갱신해 댕글링(dangling) 포인터를 방지한다.
 *
 * 실행 컨텍스트: 호출자(bfq_merge_bfqqs, bfq_release_process_ref)가
 * bfqd->lock을 보유한 상태에서 호출.
 *
 * 호출 체인:
 *   bfq_merge_bfqqs / bfq_release_process_ref → [bfq_reassign_last_bfqq]
 */
void bfq_reassign_last_bfqq(struct bfq_queue *cur_bfqq,
			    struct bfq_queue *new_bfqq)
{
	if (cur_bfqq->entity.parent && // cur_bfqq가 cgroup 엔티티에 속해 있고
	    cur_bfqq->entity.parent->last_bfqq_created == cur_bfqq) // 그 cgroup의 "마지막 생성 bfqq" 추적 포인터가 바로 cur_bfqq를 가리키고 있었다면
		cur_bfqq->entity.parent->last_bfqq_created = new_bfqq; // 댕글링을 막기 위해 new_bfqq(또는 NULL)로 갱신
	else if (cur_bfqq->bfqd && cur_bfqq->bfqd->last_bfqq_created == cur_bfqq) // cgroup이 없는 경우엔 장치 전역 추적 포인터가 cur_bfqq를 가리키는지 확인
		cur_bfqq->bfqd->last_bfqq_created = new_bfqq; // 마찬가지로 new_bfqq(또는 NULL)로 갱신
}

/*
 * [한국어]
 * bfq_release_process_ref - 한 프로세스가 bfqq에 대해 갖고 있던 참조
 * (process reference)를 반납하고, 마지막 참조였다면 bfqq를 실제로 해제한다.
 *
 * @bfqd: 이 bfqq가 속한 장치의 전역 스케줄러 상태.
 * @bfqq: 참조를 반납할 대상 bfq_queue. cooperator merge로 사라지는
 *        원래 큐이거나, io_cq 해제 시점의 bfqq일 수 있다.
 * @return: 없음(void).
 *
 * 프로세스가 종료되거나(exit), 병합으로 인해 다른 bfqq로 리다이렉트되면
 * 더 이상 이 bfqq에 새 요청을 보내지 않는다. 이때 bfqq가 여전히
 * "busy"(스케줄링 트리에 남아 서비스를 기다리는 중)이지만 큐가 비어
 * 있다면, 더 이상 서비스를 받을 필요가 없으므로 스케줄링 구조에서
 * 제거해야 한다(그렇지 않으면 아무도 요청하지 않는 큐가 계속 서비스
 * 순번을 차지해 다른 큐들의 공정성을 해친다).
 *
 * 동작:
 *   1) bfqq가 busy 상태이고, 정렬 트리(sort_list)가 비어 있으며(대기 중인
 *      요청이 없음), 현재 in_service_queue도 아니라면
 *      bfq_del_bfqq_busy()로 스케줄링 트리에서 제거한다.
 *   2) last_bfqq_created 추적 포인터가 이 bfqq를 가리키고 있었다면
 *      bfq_reassign_last_bfqq()로 NULL로 정리한다.
 *   3) bfq_put_queue()로 참조 카운트를 감소시키고, 0이 되면 bfqq 자체를
 *      해제한다.
 *
 * 실행 컨텍스트: 호출자(bfq_merge_bfqqs, bic 해제 경로 등)가 bfqd->lock을
 * 보유한 상태에서 호출.
 *
 * 호출 체인:
 *   bfq_merge_bfqqs / bfq_exit_icq 등 → [bfq_release_process_ref]
 *   → bfq_del_bfqq_busy / bfq_reassign_last_bfqq / bfq_put_queue
 */
void bfq_release_process_ref(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	/*
	 * To prevent bfqq's service guarantees from being violated,
	 * bfqq may be left busy, i.e., queued for service, even if
	 * empty (see comments in __bfq_bfqq_expire() for
	 * details). But, if no process will send requests to bfqq any
	 * longer, then there is no point in keeping bfqq queued for
	 * service. In addition, keeping bfqq queued for service, but
	 * with no process ref any longer, may have caused bfqq to be
	 * freed when dequeued from service. But this is assumed to
	 * never happen.
	 */
	if (bfq_bfqq_busy(bfqq) && RB_EMPTY_ROOT(&bfqq->sort_list) && // bfqq가 여전히 스케줄링 대상(busy)이고, 대기 중인 요청이 하나도 없으며(rb-tree가 비어 있음)
	    bfqq != bfqd->in_service_queue) // 지금 장치를 점유 중인 큐가 아니라면(점유 중이면 expire 경로에서 별도 처리)
		bfq_del_bfqq_busy(bfqq, false); // 더 이상 서비스를 기다릴 이유가 없으므로 busy 트리에서 제거

	bfq_reassign_last_bfqq(bfqq, NULL); // "마지막 생성 bfqq" 추적 포인터가 이 bfqq를 가리키던 것을 NULL로 정리(댕글링 방지)

	bfq_put_queue(bfqq); // 프로세스 참조 카운트 감소 - 0이 되면 bfqq 메모리 자체가 해제됨(kmem_cache_free 경로)
}

/*
 * [한국어]
 * bfq_merge_bfqqs - bic(bfq_io_cq)이 발행하는 요청을 bfqq에서 new_bfqq로
 * 실제로 리다이렉트시켜, 두 bfq_queue를 하나의 공유(shared) 큐로 만든다.
 *
 * @bfqd: 장치 전역 BFQ 스케줄러 상태. wr_busy_queues(가중치 상승 중인
 *        busy 큐 개수) 카운터를 갱신하는 데 쓰인다.
 * @bic: 병합을 요청한 프로세스의 io_cq. 이 함수 실행 후 이 bic이 발행하는
 *       요청은 new_bfqq로 들어가게 된다.
 * @bfqq: 병합되어 사라질(리다이렉트 원본) bfq_queue. bfqq->new_bfqq에
 *        병합 대상이 이미 설정돼 있어야 한다(bfq_setup_merge가 준비).
 * @return: 병합 후 실제로 사용해야 할 bfq_queue, 즉 new_bfqq(=bfqq->new_bfqq).
 *
 * bfq_setup_cooperator()/bfq_setup_merge()가 "병합하겠다"는 결정과
 * ->new_bfqq 포인터 연결, 참조 카운트 증가까지는 이미 끝내 놓았지만,
 * 실제로 bic이 이 프로세스의 향후 I/O를 new_bfqq로 보내도록 라우팅을
 * 바꾸는 것은 이 함수의 역할이다. 병합 시점에 bfqq에 쌓여 있던 학습된
 * 상태(think time, weight-raising 진행도, IO_bound 여부, waker 관계
 * 등)를 new_bfqq로 이전하거나 병합 이전 상태로 보존해, new_bfqq가 마치
 * "두 프로세스의 합"으로서 적절한 서비스를 받도록 한다.
 *
 * 동작 순서:
 *   1) bfq_bfqq_save_state()로 bfqq와 new_bfqq 양쪽의 상태를 각각의
 *      bic 저장 공간에 백업(나중에 분리될 때 복원용).
 *   2) bfqq가 IO_bound였다면 new_bfqq도 IO_bound로 표시(더 활동적인
 *      쪽의 특성을 채택), bfqq 자신의 IO_bound 플래그는 해제.
 *   3) bfqq에 waker(이 큐의 요청 뒤에 나타나는 상대 큐, injection 허용
 *      대상)가 있고 new_bfqq에는 없었다면 그 waker 관계를 new_bfqq로
 *      이전한다(협력 프로세스들은 같은 waker 혜택을 공유해야 하므로).
 *   4) bfqq가 weight-raised 상태였다면(방금 생성돼 미처 WR을 적용받지
 *      못한 경우는 제외) 그 WR 계수/기간/시작시각을 new_bfqq에 그대로
 *      승계시키고, bfqq 자신은 WR을 반납(wr_coeff=1)한다. wr_busy_queues
 *      카운터도 이 이전에 맞춰 증감시킨다.
 *   5) bic_set_bfqq()로 실제 라우팅 테이블(bic->bfqq[]) 을 new_bfqq로
 *      갱신하고 bfq_mark_bfqq_coop()으로 new_bfqq에 "협력 큐(공유
 *      가능)" 표시를 남긴다.
 *   6) new_bfqq->bic을 NULL로 만들어 "이 큐는 이제 여러 bic이 공유하는
 *      큐"임을 표시하고, pid를 -1(SHARED 로깅용)로 바꾼다.
 *   7) bfq_reassign_last_bfqq()로 댕글링 포인터를 정리하고,
 *      bfq_release_process_ref()로 bfqq에 대한 이 프로세스의 참조를
 *      반납한다(참조가 0이 되면 bfqq 자체가 해제될 수 있음).
 *
 * 실행 컨텍스트: 호출자(bfq_allow_bio_merge, bfq_setup_cooperator 계열
 * 호출 경로)가 bfqd->lock을 보유한 상태에서 실행. 재진입 없음.
 *
 * 호출 체인:
 *   bfq_allow_bio_merge / bfq_insert_request → [bfq_merge_bfqqs]
 *   → bfq_bfqq_save_state / bic_set_bfqq / bfq_reassign_last_bfqq
 *   / bfq_release_process_ref
 */
static struct bfq_queue *bfq_merge_bfqqs(struct bfq_data *bfqd,
					 struct bfq_io_cq *bic,
					 struct bfq_queue *bfqq)
{
	struct bfq_queue *new_bfqq = bfqq->new_bfqq; // bfq_setup_merge()가 미리 설정해 둔 병합 목적지 큐

	/* [한국어] 디버그 트레이스: 어떤 pid의 큐로 병합되는지 기록 */
	bfq_log_bfqq(bfqd, bfqq, "merging with queue %lu",
		(unsigned long)new_bfqq->pid);
	/* Save weight raising and idle window of the merged queues */
	bfq_bfqq_save_state(bfqq); // 사라질 bfqq의 상태를 이 프로세스의 bic 저장 공간에 백업(향후 분리 시 복원용)
	bfq_bfqq_save_state(new_bfqq); // new_bfqq 쪽 상태도 함께 백업 - new_bfqq 자신의 원래 프로세스가 나중에 분리될 수도 있으므로
	if (bfq_bfqq_IO_bound(bfqq)) // bfqq가 끊임없이 요청을 내는(IO-bound) 프로세스였다면
		bfq_mark_bfqq_IO_bound(new_bfqq); // 병합된 큐 전체를 IO_bound로 취급 - 둘 중 하나라도 활발하면 공유 큐도 활발한 것으로 간주
	bfq_clear_bfqq_IO_bound(bfqq); // bfqq 자신의 플래그는 해제 - 이제 실질적으로 요청을 받지 않을 큐이므로 의미가 없어짐

	/*
	 * The processes associated with bfqq are cooperators of the
	 * processes associated with new_bfqq. So, if bfqq has a
	 * waker, then assume that all these processes will be happy
	 * to let bfqq's waker freely inject I/O when they have no
	 * I/O.
	 */
	if (bfqq->waker_bfqq && !new_bfqq->waker_bfqq && // bfqq에게는 waker(이 큐가 쉴 때 요청을 끼워넣어도 되는 상대 큐)가 있는데 new_bfqq에는 아직 없고
	    bfqq->waker_bfqq != new_bfqq) { // 그 waker가 new_bfqq 자신이 아니라면(자기 자신을 waker로 설정하는 모순 방지)
		new_bfqq->waker_bfqq = bfqq->waker_bfqq; // bfqq의 waker 관계를 new_bfqq가 그대로 물려받음 - 협력 프로세스들은 같은 injection 혜택을 공유
		new_bfqq->tentative_waker_bfqq = NULL; // 아직 확정되지 않은 "잠정 waker 후보" 상태는 초기화(확정된 waker로 대체됐으므로)

		/*
		 * If the waker queue disappears, then
		 * new_bfqq->waker_bfqq must be reset. So insert
		 * new_bfqq into the woken_list of the waker. See
		 * bfq_check_waker for details.
		 */
		/* [한국어] waker 큐가 나중에 사라질 때 new_bfqq->waker_bfqq를 역참조로 찾아 NULL로
		 * 정리할 수 있도록 waker의 woken_list에 등록 */
		hlist_add_head(&new_bfqq->woken_list_node,
			       &new_bfqq->waker_bfqq->woken_list);

	}

	/*
	 * If bfqq is weight-raised, then let new_bfqq inherit
	 * weight-raising. To reduce false positives, neglect the case
	 * where bfqq has just been created, but has not yet made it
	 * to be weight-raised (which may happen because EQM may merge
	 * bfqq even before bfq_add_request is executed for the first
	 * time for bfqq). Handling this case would however be very
	 * easy, thanks to the flag just_created.
	 */
	if (new_bfqq->wr_coeff == 1 && bfqq->wr_coeff > 1) { // new_bfqq는 WR 중이 아닌데 bfqq는 WR 중이었다면(WR을 물려줄 필요가 있는 경우)
		new_bfqq->wr_coeff = bfqq->wr_coeff; // bfqq의 WR 배수를 new_bfqq에 그대로 적용
		new_bfqq->wr_cur_max_time = bfqq->wr_cur_max_time; // 이번 WR 구간의 최대 지속시간도 함께 승계
		new_bfqq->last_wr_start_finish = bfqq->last_wr_start_finish; // WR이 마지막으로 시작/갱신된 시각도 승계 - 잔여 WR 기간 계산의 기준점 유지
		/* [한국어] soft-real-time 전환 시각도 함께 승계 */
		new_bfqq->wr_start_at_switch_to_srt =
			bfqq->wr_start_at_switch_to_srt;
		if (bfq_bfqq_busy(new_bfqq)) // new_bfqq가 이미 busy(스케줄링 대상)라면
			bfqd->wr_busy_queues++; // WR 중인 busy 큐 개수를 하나 증가 - new_bfqq가 새로 WR 집합에 편입됐으므로
		new_bfqq->entity.prio_changed = 1; // 엔티티 우선순위/가중치가 바뀌었으니 B-WF2Q+ 트리 재삽입이 필요함을 표시
	}

	/* [한국어] bfqq는 이제 WR을 new_bfqq에게 넘겼으므로 자신의 배수를 1(비-WR)로 되돌림 */
	if (bfqq->wr_coeff > 1) { /* bfqq has given its wr to new_bfqq */
		bfqq->wr_coeff = 1;
		bfqq->entity.prio_changed = 1; // bfqq의 엔티티도 가중치가 바뀌었음을 표시(다만 곧 release될 큐이므로 영향은 제한적)
		if (bfq_bfqq_busy(bfqq)) // bfqq가 아직 busy 상태였다면
			bfqd->wr_busy_queues--; // WR busy 큐 카운터에서 하나 제거 - bfqq가 WR 집합에서 빠졌으므로
	}

	/* [한국어] 디버그 트레이스: 병합 후 WR busy 큐 개수 기록 */
	bfq_log_bfqq(bfqd, new_bfqq, "merge_bfqqs: wr_busy %d",
		     bfqd->wr_busy_queues);

	/*
	 * Merge queues (that is, let bic redirect its requests to new_bfqq)
	 */
	bic_set_bfqq(bic, new_bfqq, true, bfqq->actuator_idx); // 실제 라우팅 갱신: 이 프로세스(bic)가 이후 sync 요청을 new_bfqq로 보내도록 bic->bfqq[][] 테이블을 갱신
	bfq_mark_bfqq_coop(new_bfqq); // new_bfqq에 "협력(cooperator) 큐로서 공유 중" 플래그를 세움 - 이후 정책 판단(예: idling 여부)에 반영됨
	/*
	 * new_bfqq now belongs to at least two bics (it is a shared queue):
	 * set new_bfqq->bic to NULL. bfqq either:
	 * - does not belong to any bic any more, and hence bfqq->bic must
	 *   be set to NULL, or
	 * - is a queue whose owning bics have already been redirected to a
	 *   different queue, hence the queue is destined to not belong to
	 *   any bic soon and bfqq->bic is already NULL (therefore the next
	 *   assignment causes no harm).
	 */
	new_bfqq->bic = NULL; // 이제 new_bfqq는 단일 프로세스 소유가 아니라 여러 bic이 공유하는 큐이므로 단일 bic 역참조를 무효화
	/*
	 * If the queue is shared, the pid is the pid of one of the associated
	 * processes. Which pid depends on the exact sequence of merge events
	 * the queue underwent. So printing such a pid is useless and confusing
	 * because it reports a random pid during those of the associated
	 * processes.
	 * We mark such a queue with a pid -1, and then print SHARED instead of
	 * a pid in logging messages.
	 */
	new_bfqq->pid = -1; // 공유 큐 표시 - 로깅 코드가 이 값을 보고 특정 pid 대신 "SHARED" 문자열을 출력하게 됨
	bfqq->bic = NULL; // bfqq도 더 이상 이 bic(또는 어떤 bic도)에 속하지 않으므로 역참조를 끊음

	bfq_reassign_last_bfqq(bfqq, new_bfqq); // "마지막 생성 bfqq" 추적 포인터가 bfqq를 가리키고 있었다면 new_bfqq로 갱신(댕글링 방지)

	bfq_release_process_ref(bfqd, bfqq); // bfqq에 대한 이 프로세스의 참조 반납 - 참조가 0이 되면 bfqq 메모리 자체가 해제될 수 있음

	return new_bfqq; // 이후 이 프로세스의 I/O를 다뤄야 할 실제 bfq_queue를 호출자에게 알려줌
}

/*
 * [한국어]
 * bfq_allow_bio_merge - 블록 계층(block layer)이 새 bio를 기존 request에
 * 병합해도 되는지 BFQ에 물어보는 elevator 콜백. 이 기회를 이용해 조기
 * cooperator 큐 병합(early merge)도 함께 수행한다.
 *
 * @q: 이 요청이 속한 request_queue. q->elevator->elevator_data로 bfqd를
 *     얻는다.
 * @rq: bio가 병합될 후보 request(이미 큐에 들어가 있는 요청).
 * @bio: 새로 도착해 병합을 시도하는 bio.
 * @return: true면 bio를 rq에 병합해도 좋다는 뜻, false면 병합 불가.
 *
 * 블록 계층의 bio 병합 로직(blk_mq_sched_bio_merge 등)은 elevator에게
 * "이 bio를 이 rq에 붙여도 되느냐"를 묻는데, BFQ는 이 시점을 이용해
 * 부수적으로 "이 bio를 발행한 프로세스의 bfqq와, 인접 LBA에 접근하는
 * 다른 bfqq가 협력 관계인지"도 함께 판단해 조기에 큐 병합을 수행한다.
 * bio 병합 자체가 성립하려면 결국 두 bfqq가 같은 큐(new_bfqq)로
 * 합쳐져 있어야 하므로, 이 콜백이 자연스러운 병합 수행 지점이 된다.
 *
 * 동작 순서:
 *   1) sync bio를 async request에 붙이는 것은 금지(정책 위반이므로
 *      즉시 false).
 *   2) bfqd->bio_bfqq(이 bio가 최종적으로 들어갈 bfqq, bfq_bio_merge에서
 *      미리 설정)가 없으면 병합 불가.
 *   3) bfq_setup_cooperator()로 협력 큐 병합 후보를 찾는다. 후보가
 *      있으면 bfq_merge_bfqqs()를 반복 호출해(->new_bfqq 체인을 끝까지
 *      따라가며) 실제 병합을 수행하고, bfqd->bio_bfqq도 최종 목적지로
 *      갱신한다.
 *   4) 최종적으로 bfqq(병합 후 큐)가 rq가 실제로 속한 bfqq(RQ_BFQQ(rq))와
 *      같은지 비교해 반환 - 같아야 진짜로 bio를 이 rq에 붙일 수 있다.
 *
 * 실행 컨텍스트: 블록 계층이 bfqd->lock을 보유한 상태에서 호출(elevator
 * ops 콜백). bio 병합 경로이므로 아주 빈번하게 호출될 수 있음.
 *
 * 호출 체인:
 *   blk_mq_sched_bio_merge / blk_mq_attempt_bio_merge → [bfq_allow_bio_merge]
 *   → bfq_setup_cooperator → bfq_merge_bfqqs
 */
static bool bfq_allow_bio_merge(struct request_queue *q, struct request *rq,
				struct bio *bio)
{
	struct bfq_data *bfqd = q->elevator->elevator_data; // elevator 사설 데이터에서 BFQ 전역 상태를 얻음
	bool is_sync = op_is_sync(bio->bi_opf); // 새 bio가 동기(sync) I/O인지(REQ_SYNC 플래그) 확인
	struct bfq_queue *bfqq = bfqd->bio_bfqq, *new_bfqq; // bfq_bio_merge()에서 미리 찾아둔, 이 bio가 속할 bfqq

	/*
	 * Disallow merge of a sync bio into an async request.
	 */
	if (is_sync && !rq_is_sync(rq)) // bio는 동기인데 대상 rq는 비동기라면 - 서로 다른 지연시간 요구사항을 가진 요청을 섞으면 안 됨
		return false; // 병합 불허

	/*
	 * Lookup the bfqq that this bio will be queued with. Allow
	 * merge only if rq is queued there.
	 */
	if (!bfqq) // 이 bio가 속할 bfqq를 아직 찾지 못했다면(아직 큐가 배정되지 않은 상태)
		return false; // 어느 큐와도 비교할 수 없으므로 병합 불허

	/*
	 * We take advantage of this function to perform an early merge
	 * of the queues of possible cooperating processes.
	 */
	new_bfqq = bfq_setup_cooperator(bfqd, bfqq, bio, false, bfqd->bio_bic); // bio 기준으로 협력(cooperator) 큐 병합 후보를 탐색(request가 아니라 bio 이므로 request=false)
	if (new_bfqq) { // 병합할 후보가 발견됐다면
		/*
		 * bic still points to bfqq, then it has not yet been
		 * redirected to some other bfq_queue, and a queue
		 * merge between bfqq and new_bfqq can be safely
		 * fulfilled, i.e., bic can be redirected to new_bfqq
		 * and bfqq can be put.
		 */
		while (bfqq != new_bfqq) // new_bfqq가 병합 체인의 중간 노드일 수 있으므로 최종 목적지에 도달할 때까지 반복
			bfqq = bfq_merge_bfqqs(bfqd, bfqd->bio_bic, bfqq); // 실제 병합 수행 - bic의 라우팅을 다음 단계로 갱신하고 반환된 큐로 계속 진행

		/*
		 * Change also bqfd->bio_bfqq, as
		 * bfqd->bio_bic now points to new_bfqq, and
		 * this function may be invoked again (and then may
		 * use again bqfd->bio_bfqq).
		 */
		bfqd->bio_bfqq = bfqq; // 이 bio 처리 도중 이 함수가 재호출될 경우를 대비해 최신 bfqq로 갱신해 둠
	}

	return bfqq == RQ_BFQQ(rq); // 병합 후 최종 bfqq가 rq가 실제로 속한 bfqq와 일치해야만 진짜로 병합 가능
}

/*
 * Set the maximum time for the in-service queue to consume its
 * budget. This prevents seeky processes from lowering the throughput.
 * In practice, a time-slice service scheme is used with seeky
 * processes.
 */
/*
 * [한국어]
 * bfq_set_budget_timeout - in-service 큐(현재 장치를 점유 중인 bfqq)가
 * 자신의 budget(섹터 예산)을 다 쓸 때까지 허용되는 "시간 제한"
 * (budget_timeout)을 계산해 설정한다.
 *
 * @bfqd: 장치 전역 상태. bfq_timeout(기본 타임아웃, 기본값 HZ/8)을
 *        조회한다.
 * @bfqq: 시간 제한을 설정할 대상 bfqq (곧 in-service가 될 큐).
 * @return: 없음(void).
 *
 * budget(섹터 단위 예산)만으로 서비스를 제한하면, seek이 잦아 초당
 * 처리 섹터 수가 매우 낮은 프로세스는 자신의 budget을 다 쓰는 데
 * 지나치게 오래 걸려 다른 큐들의 처리량을 해칠 수 있다. 이를 막기
 * 위해 budget과 별도로 "이 시각까지는 반드시 서비스를 끝내야 한다"는
 * 시간 제한(time-slice)을 함께 둔다. weight-raised soft-real-time
 * 큐(wr_cur_max_time == bfq_wr_rt_max_time)는 이미 낮은 지연시간을
 * 보장받도록 설계돼 있으므로 배수(timeout_coeff)를 1로 고정하고,
 * 그 외의 큐는 자신의 가중치 비율(weight/orig_weight, 즉 현재
 * weight-raising 배수)만큼 타임아웃을 늘려 WR 중인 큐가 부당하게 짧은
 * 시간 제한을 받지 않게 한다.
 *
 * 실행 컨텍스트: 호출자(__bfq_set_in_service_queue)가 bfqd->lock을
 * 보유한 상태에서 실행.
 *
 * 호출 체인:
 *   __bfq_set_in_service_queue → [bfq_set_budget_timeout]
 */
static void bfq_set_budget_timeout(struct bfq_data *bfqd,
				   struct bfq_queue *bfqq)
{
	unsigned int timeout_coeff;

	if (bfqq->wr_cur_max_time == bfqd->bfq_wr_rt_max_time) // 이 큐가 soft-real-time 목적의 weight-raising 중이라면(짧고 엄격한 WR 구간)
		timeout_coeff = 1; // 이미 지연시간 보장이 목적이므로 타임아웃을 늘리지 않고 기본값 그대로 사용
	/* [한국어] 그 외에는 현재 가중치/원래 가중치 비율(=WR 배수)만큼 타임아웃을 늘려 WR 큐가 짧은 슬라이스로 손해보지 않게
	 * 함 */
	else
		timeout_coeff = bfqq->entity.weight / bfqq->entity.orig_weight;

	bfqd->last_budget_start = blk_time_get(); // 이번 budget 소비 구간이 시작된 시각(모노토닉 클록)을 기록 - 나중에 실제 소비 시간 계산에 사용

	/* [한국어] 기본 타임아웃(bfq_timeout, 기본 HZ/8)에 배수를 곱해 만료 시각(jiffies 단위)을 설정 */
	bfqq->budget_timeout = jiffies +
		bfqd->bfq_timeout * timeout_coeff;
}

/*
 * [한국어]
 * __bfq_set_in_service_queue - bfqd->in_service_queue(현재 장치를 점유해
 * 서비스받는 bfqq)를 실제로 갱신하는 내부 헬퍼. NULL을 넘기면 "점유 큐
 * 없음" 상태로 만든다.
 *
 * @bfqd: 장치 전역 상태. in_service_queue, budgets_assigned(지수
 *        가중이동평균으로 추정한 평균 budget 크기) 등을 갱신한다.
 * @bfqq: 새로 in-service로 지정할 bfqq. NULL이면 in-service 큐를
 *        비운다(예: expire 직후 아직 다음 큐를 고르기 전).
 * @return: 없음(void).
 *
 * 이 함수는 실제 큐 선택 로직(bfq_get_next_queue)을 포함하지 않고,
 * 오직 "이 bfqq를 지금부터 서비스 대상으로 확정"하는 부수 효과만
 * 처리한다: fifo_expire 플래그 초기화, 평균 budget 통계 갱신, 그리고
 * soft-real-time 큐가 최근 서비스를 못 받았을 때 WR 시작 시각을
 * 보정하는 특수 로직, budget_timeout 설정까지 담당한다.
 *
 * 동작 순서:
 *   1) bfqq가 NULL이 아니면(실제로 큐를 점유시키는 경우):
 *      a) fifo_expire 플래그 해제(새 서비스 구간이 시작됐으므로).
 *      b) budgets_assigned를 지수 가중이동평균(가중치 7/8, 새 값
 *         256/8배수)으로 갱신 - "평균적으로 얼마의 budget이 할당돼
 *         왔는지" 추정치, budget 자동조정 등에 활용.
 *      c) 이 큐가 soft-real-time WR 중이고, 마지막 WR 시작 이후
 *         시간이 지났으며, 직전 budget_timeout도 이미 지난(즉 오래
 *         서비스를 못 받은) 상태라면, last_wr_start_finish를 그
 *         서비스 공백만큼 앞으로 당겨 WR 구간이 부당하게 일찍
 *         끝나지 않도록 보정한다.
 *      d) bfq_set_budget_timeout()으로 이번 서비스 구간의 시간 제한을
 *         설정.
 *   2) bfqd->in_service_queue = bfqq로 갱신, in_serv_last_pos(마지막
 *      처리 위치)는 0으로 리셋(새 큐이므로 위치 이력 무효화).
 *
 * 실행 컨텍스트: 호출자(bfq_set_in_service_queue 등)가 bfqd->lock을
 * 보유한 상태에서 실행.
 *
 * 호출 체인:
 *   bfq_set_in_service_queue → [__bfq_set_in_service_queue]
 *   → bfq_set_budget_timeout
 */
static void __bfq_set_in_service_queue(struct bfq_data *bfqd,
				       struct bfq_queue *bfqq)
{
	if (bfqq) { // 실제로 점유시킬 큐가 주어진 경우(NULL이 아니면)
		bfq_clear_bfqq_fifo_expire(bfqq); // "FIFO 순서상 만료됨" 플래그 해제 - 새로 서비스를 시작하므로 이전 만료 상태를 무효화

		bfqd->budgets_assigned = (bfqd->budgets_assigned * 7 + 256) / 8; // 평균 budget 크기의 지수 가중이동평균 갱신(가중치 7/8은 과거, 나머지는 256을 새 표본처럼 반영하는 근사식)

		/* [한국어] WR이 시작된 뒤 시간이 흘렀고, 실제로 WR 중이며, soft-real-time 유형의 WR이고,
		 * 마지막 budget_timeout도 이미 지난(서비스 공백이 있었던) 경우 */
		if (time_is_before_jiffies(bfqq->last_wr_start_finish) &&
		    bfqq->wr_coeff > 1 &&
		    bfqq->wr_cur_max_time == bfqd->bfq_wr_rt_max_time &&
		    time_is_before_jiffies(bfqq->budget_timeout)) {
			/*
			 * For soft real-time queues, move the start
			 * of the weight-raising period forward by the
			 * time the queue has not received any
			 * service. Otherwise, a relatively long
			 * service delay is likely to cause the
			 * weight-raising period of the queue to end,
			 * because of the short duration of the
			 * weight-raising period of a soft real-time
			 * queue.  It is worth noting that this move
			 * is not so dangerous for the other queues,
			 * because soft real-time queues are not
			 * greedy.
			 *
			 * To not add a further variable, we use the
			 * overloaded field budget_timeout to
			 * determine for how long the queue has not
			 * received service, i.e., how much time has
			 * elapsed since the queue expired. However,
			 * this is a little imprecise, because
			 * budget_timeout is set to jiffies if bfqq
			 * not only expires, but also remains with no
			 * request.
			 */
			if (time_after(bfqq->budget_timeout,
				       bfqq->last_wr_start_finish)) // budget_timeout이 WR 시작 시각보다 나중이면(정상적인 순서라면 항상 참)
				bfqq->last_wr_start_finish +=
					jiffies - bfqq->budget_timeout; // WR 시작 시각을 "서비스를 못 받은 시간(jiffies - budget_timeout)"만큼 앞으로 밀어 WR 잔여 기간을 보정
			/* [한국어] 시간 관계가 역전된 예외적 경우엔 그냥 지금 시각으로 재설정(방어적 처리) */
			else
				bfqq->last_wr_start_finish = jiffies;
		}

		bfq_set_budget_timeout(bfqd, bfqq); // 이번 서비스 구간의 시간 제한(budget_timeout)을 새로 계산
		/* [한국어] 디버그 트레이스: 이 큐에 부여된 현재 budget(섹터 단위) 기록 */
		bfq_log_bfqq(bfqd, bfqq,
			     "set_in_service_queue, cur-budget = %d",
			     bfqq->entity.budget);
	}

	bfqd->in_service_queue = bfqq; // 장치를 실제로 점유하는 bfqq를 갱신(NULL이면 "점유 큐 없음")
	bfqd->in_serv_last_pos = 0; // 새 큐로 바뀌었으므로 이전 큐의 마지막 처리 위치 이력을 무효화
}

/*
 * Get and set a new queue for service.
 */
/*
 * [한국어]
 * bfq_set_in_service_queue - B-WF2Q+ 스케줄링 트리에서 다음에 서비스할
 * bfqq를 골라(bfq_get_next_queue) 실제로 in-service 큐로 지정한다.
 *
 * @bfqd: 장치 전역 상태.
 * @return: 새로 in-service로 지정된 bfq_queue(없으면 NULL, 즉 대기 중인
 *          큐가 하나도 없는 상태).
 *
 * 디스패치 루프(bfq_dispatch_request 등)가 in-service 큐가 비었을 때
 * 다음 서비스 대상을 정하기 위해 호출하는 진입점이다. 실제 트리 탐색은
 * bfq_get_next_queue()에 위임하고, 이 함수는 그 결과를
 * __bfq_set_in_service_queue()로 확정 짓는 역할만 한다.
 *
 * 실행 컨텍스트: 호출자가 bfqd->lock을 보유한 상태에서 실행.
 *
 * 호출 체인:
 *   bfq_select_queue / bfq_dispatch_request → [bfq_set_in_service_queue]
 *   → bfq_get_next_queue, __bfq_set_in_service_queue
 */
static struct bfq_queue *bfq_set_in_service_queue(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq = bfq_get_next_queue(bfqd); // B-WF2Q+ 트리에서 가상 완료시각(finish time)이 가장 이른 큐를 선택

	__bfq_set_in_service_queue(bfqd, bfqq); // 선택된 큐를 실제 in-service 큐로 확정(budget_timeout 설정 등 부수효과 처리)
	return bfqq; // 선택된 큐(없으면 NULL)를 호출자에게 반환
}

/*
 * [한국어]
 * bfq_arm_slice_timer - 현재 in-service 큐가 다음 요청을 낼 때까지
 * 디스패치를 잠시 멈추고 기다리도록 idle timer(hrtimer)를 무장(arm)한다.
 *
 * @bfqd: 장치 전역 상태. idle_slice_timer(hrtimer), bfq_slice_idle
 *        (기본 idle 대기시간), last_idling_start 등을 다룬다.
 * @return: 없음(void).
 *
 * in-service 큐(bfqd->in_service_queue)가 방금 마지막 요청을 완료했지만
 * 아직 budget이 남아 있고, 곧 이 프로세스가 다음 요청을 낼 것으로
 * 예상되는 경우, 장치를 즉시 다른 큐에게 넘기지 않고 잠깐(sl 나노초)
 * 기다린다. 이렇게 하면 다음 request도 같은 bfqq가 이어받아 서비스
 * 순서를 어지럽히지 않고 공정성을 지킬 수 있다. NVMe 관점에서는 이
 * 대기 동안 SQ(Submission Queue)에 다음 CID(Command ID)를 채우는
 * 시점이 늦춰지므로, 처리량 일부를 공정성과 맞바꾸는 트레이드오프다.
 *
 * 동작:
 *   1) bfq_mark_bfqq_wait_request()로 "이 큐가 다음 요청을 기다리는
 *      중" 플래그를 세운다(bfq_add_request 등에서 이 플래그를 보고
 *      idle을 해제).
 *   2) 기본 대기시간 sl = bfqd->bfq_slice_idle(설정 가능한 기본값,
 *      보통 수 ms)로 시작한다.
 *   3) 이 큐가 seeky(BFQQ_SEEKY, 최근 요청들의 seek 히스토리 비트가
 *      많이 서 있음)하고 WR 중이 아니며, 비대칭 시나리오(다른 큐들과
 *      가중치가 다르거나 그룹이 섞여 있어 idling이 꼭 필요한 상황)가
 *      아니라면, 대기시간을 BFQ_MIN_TT(2ms)로 최소화한다 - seeky
 *      프로세스를 오래 기다려봤자 처리량에 도움이 안 되기 때문.
 *   4) 반대로 이 큐가 weight-raised(wr_coeff > 1) 상태라면 최소
 *      20ms까지 늘려서라도 충분히 기다려, interactive/soft-rt 큐가
 *      자신의 지연시간 보장을 받을 기회를 늘린다.
 *   5) 대기 시작 시각(last_idling_start, jiffies 버전 포함)을 기록하고,
 *      hrtimer_start()로 idle_slice_timer를 sl 나노초 뒤 상대시각
 *      (HRTIMER_MODE_REL)에 만료되도록 무장한다. 타이머가 만료되면
 *      bfq_idle_slice_timer 콜백이 실행되어 실제로 큐를 만료시키고
 *      다음 큐로 넘어간다.
 *   6) bfqg_stats_set_start_idle_time()으로 cgroup 통계에도 idle 시작
 *      시각을 기록(blkio.* 통계 파일에 노출).
 *
 * 실행 컨텍스트: 호출자(bfq_dispatch_request 등)가 bfqd->lock을 보유한
 * 상태에서 호출. hrtimer 콜백 자체는 별도의 소프트IRQ/hrtimer 컨텍스트
 * 에서 나중에 비동기로 실행됨(이 함수 자신은 타이머를 걸기만 하고
 * 즉시 반환).
 *
 * 호출 체인:
 *   bfq_dispatch_request / bfq_completed_request → [bfq_arm_slice_timer]
 *   → hrtimer_start (→ 훗날 bfq_idle_slice_timer 콜백)
 */
static void bfq_arm_slice_timer(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq = bfqd->in_service_queue; // idle을 적용할 대상은 항상 현재 장치를 점유 중인 큐
	u32 sl; // 실제로 적용할 idle 대기시간(나노초 단위)

	bfq_mark_bfqq_wait_request(bfqq); // "다음 요청을 기다리는 중" 플래그 설정 - 이 요청이 도착하면 idle을 즉시 해제하고 서비스 재개

	/*
	 * We don't want to idle for seeks, but we do want to allow
	 * fair distribution of slice time for a process doing back-to-back
	 * seeks. So allow a little bit of time for him to submit a new rq.
	 */
	sl = bfqd->bfq_slice_idle; // 기본 idle 대기시간(사용자 설정 가능, sysfs slice_idle)으로 시작
	/*
	 * Unless the queue is being weight-raised or the scenario is
	 * asymmetric, grant only minimum idle time if the queue
	 * is seeky. A long idling is preserved for a weight-raised
	 * queue, or, more in general, in an asymmetric scenario,
	 * because a long idling is needed for guaranteeing to a queue
	 * its reserved share of the throughput (in particular, it is
	 * needed if the queue has a higher weight than some other
	 * queue).
	 */
	if (BFQQ_SEEKY(bfqq) && bfqq->wr_coeff == 1 && // seeky 큐는 기다려 봐야 다음 요청이 인접 LBA로 오지 않으므로 idling의 순차성 이득이 없고, WR 중도 아니라 지연시간을 지켜줄 이유도 없다
	    !bfq_asymmetric_scenario(bfqd, bfqq)) // 이 큐가 seeky(무작위 접근 패턴)하고 WR 중이 아니며, 대칭적인(공정성 위협이 적은) 시나리오라면
		sl = min_t(u64, sl, BFQ_MIN_TT); // 오래 기다려봐야 순차 I/O로 이어지지 않으므로 대기시간을 최소값(2ms)으로 낮춤 - 처리량 우선
	else if (bfqq->wr_coeff > 1) // 반대로 이 큐가 weight-raised(interactive/soft-rt) 상태라면
		sl = max_t(u32, sl, 20ULL * NSEC_PER_MSEC); // 지연시간 보장을 위해 최소 20ms까지는 기다려 다음 요청을 받을 기회를 늘림

	bfqd->last_idling_start = blk_time_get(); // idle이 시작된 시각(모노토닉 클록) 기록 - 이후 idle 소요시간 통계에 사용
	bfqd->last_idling_start_jiffies = jiffies; // jiffies 단위로도 기록 - 다른 시간 비교 로직과의 호환을 위해 병행 유지

	/* [한국어] sl 나노초 뒤(현재 시각 기준 상대시간) 만료되는 고해상도 타이머를 무장 - 만료 시
	 * bfq_idle_slice_timer 콜백이 실행되어 큐를 만료시킴 */
	hrtimer_start(&bfqd->idle_slice_timer, ns_to_ktime(sl),
		      HRTIMER_MODE_REL);
	bfqg_stats_set_start_idle_time(bfqq_group(bfqq)); // 이 bfqq가 속한 cgroup의 blkio 통계에도 idle 시작 시각을 기록
}

/*
 * In autotuning mode, max_budget is dynamically recomputed as the
 * amount of sectors transferred in timeout at the estimated peak
 * rate. This enables BFQ to utilize a full timeslice with a full
 * budget, even if the in-service queue is served at peak rate. And
 * this maximises throughput with sequential workloads.
 */
/*
 * [한국어]
 * bfq_calc_max_budget - 추정 peak_rate(장치 최대 처리율)를 기준으로,
 * 한 타임아웃(bfq_timeout) 구간 동안 이 장치가 처리할 수 있는 섹터
 * 수를 계산해 max_budget으로 사용한다.
 *
 * @bfqd: 장치 전역 상태. peak_rate(섹터/usec, BFQ_RATE_SHIFT만큼
 *        좌측 시프트된 고정소수점), bfq_timeout(기본 타임아웃, jiffies)
 *        을 입력으로 사용한다.
 * @return: 계산된 max_budget(섹터 단위).
 *
 * budget을 고정값으로 두면, 장치가 빠를 때는 타임아웃 전에 budget을
 * 다 써버려 슬라이스를 낭비하고, 장치가 느릴 때는 budget을 다 쓰기도
 * 전에 타임아웃이 먼저 걸려 순차 I/O의 이점을 살리지 못한다. 이
 * 함수는 "이 장치가 한 타임아웃 구간 동안 peak_rate로 처리할 수 있는
 * 섹터 수"를 역산해 max_budget으로 삼음으로써, 순차 워크로드가 항상
 * 타임아웃이 아니라 budget 소진으로 슬라이스를 마치도록(=최대 처리량)
 * 자동 조정한다.
 *
 * 계산식: peak_rate[sectors/usec, <<BFQ_RATE_SHIFT] * USEC_PER_MSEC
 *         * bfq_timeout(ms 환산) >> BFQ_RATE_SHIFT
 *   = peak_rate(고정소수점) * (타임아웃을 usec로 환산한 값) 을
 *     BFQ_RATE_SHIFT(16)만큼 우측 시프트해 고정소수점 스케일을
 *     되돌린 실제 섹터 수.
 *
 * 실행 컨텍스트: 호출자(update_thr_responsiveness_params, 초기화 경로
 * 등)가 bfqd->lock을 보유한 상태에서 실행.
 *
 * 호출 체인:
 *   update_thr_responsiveness_params / bfq_init_queue 등
 *   → [bfq_calc_max_budget]
 */
static unsigned long bfq_calc_max_budget(struct bfq_data *bfqd)
{
	return (u64)bfqd->peak_rate * USEC_PER_MSEC *
		jiffies_to_msecs(bfqd->bfq_timeout)>>BFQ_RATE_SHIFT; // peak_rate(<<16 고정소수점, 섹터/usec) * 타임아웃(usec 환산) 을 구한 뒤 >>BFQ_RATE_SHIFT로 고정소수점 스케일을 되돌려 실제 섹터 수를 얻음
}

/*
 * Update parameters related to throughput and responsiveness, as a
 * function of the estimated peak rate. See comments on
 * bfq_calc_max_budget(), and on the ref_wr_duration array.
 */
/*
 * [한국어]
 * update_thr_responsiveness_params - peak_rate 추정치가 갱신될 때마다,
 * 사용자가 max_budget을 수동 고정하지 않은 경우(자동 튜닝 모드) 이를
 * 다시 계산해 반영한다.
 *
 * @bfqd: 장치 전역 상태. bfq_user_max_budget(0이면 자동 계산 모드,
 *        0이 아니면 사용자가 sysfs로 고정한 값)을 확인한다.
 * @return: 없음(void).
 *
 * BFQ는 max_budget을 sysfs를 통해 사용자가 직접 고정할 수도 있고,
 * peak_rate 추정에 맞춰 자동으로 조정할 수도 있다. 이 함수는 자동
 * 모드(bfq_user_max_budget == 0)일 때만 bfq_calc_max_budget()의
 * 결과로 bfqd->bfq_max_budget을 갱신한다. weight-raising 지속시간
 * (ref_wr_duration 기반)도 peak_rate에 연동되지만, 그 갱신은 이
 * 함수가 아니라 별도 경로(bfq_wr_duration 등)에서 peak_rate를 직접
 * 참조하는 방식으로 이뤄진다.
 *
 * 실행 컨텍스트: 호출자(bfq_update_rate_reset)가 bfqd->lock을 보유한
 * 상태에서 실행.
 *
 * 호출 체인:
 *   bfq_update_rate_reset → [update_thr_responsiveness_params]
 *   → bfq_calc_max_budget
 */
static void update_thr_responsiveness_params(struct bfq_data *bfqd)
{
	if (bfqd->bfq_user_max_budget == 0) { // 사용자가 sysfs로 max_budget을 고정하지 않은 경우(자동 튜닝 모드)
		/* [한국어] 새로 추정된 peak_rate를 반영해 max_budget을 재계산 */
		bfqd->bfq_max_budget =
			bfq_calc_max_budget(bfqd);
		bfq_log(bfqd, "new max_budget = %d", bfqd->bfq_max_budget); // 디버그 트레이스: 갱신된 max_budget 값 기록
	}
}

/*
 * [한국어]
 * bfq_reset_rate_computation - peak_rate 관측 구간(observation
 * interval)을 새로 시작하기 위해 표본 카운터들을 초기화한다.
 *
 * @bfqd: 장치 전역 상태. peak_rate_samples, sequential_samples,
 *        tot_sectors_dispatched, first_dispatch/last_dispatch 등
 *        관측 상태를 관리한다.
 * @rq: 이번에 새로 디스패치된 request(있으면), 없으면(NULL) 완전
 *      초기화가 아니라 표본 수만 리셋.
 * @return: 없음(void).
 *
 * peak_rate 추정은 일정 관측 구간 동안 디스패치된 요청들을 표본으로
 * 삼아 rate를 계산하는데(bfq_update_rate_reset 참고), 관측이 끝나면
 * (성공하든 조건 미달로 포기하든) 다음 관측을 위해 상태를 리셋해야
 * 한다. rq가 있으면(즉, 이번 리셋이 새 dispatch 시점에 발생한 것이면)
 * "표본 1개, 지금까지 디스패치된 섹터 수 = 이번 rq의 섹터 수"로 완전히
 * 새로 시작하고, rq가 없으면(예: 관측 조건 미달로 포기하는 경우) 표본
 * 수만 0으로 만들어 다음 dispatch 때 전체 재초기화(peak_rate_samples
 * == 0 분기)가 일어나도록 유도한다.
 *
 * 실행 컨텍스트: 호출자(bfq_update_rate_reset, bfq_update_peak_rate)가
 * bfqd->lock을 보유한 상태에서 실행.
 *
 * 호출 체인:
 *   bfq_update_rate_reset / bfq_update_peak_rate
 *   → [bfq_reset_rate_computation]
 */
static void bfq_reset_rate_computation(struct bfq_data *bfqd,
				       struct request *rq)
{
	if (rq != NULL) { /* new rq dispatch now, reset accordingly */ // 이번 리셋이 실제 새 dispatch 시점에 일어난 경우(완전 재초기화)
		bfqd->last_dispatch = bfqd->first_dispatch = blk_time_get_ns(); // 새 관측 구간의 시작 시각을 "지금"으로 설정 - 이후 dispatch/completion과의 시간차 계산 기준점
		bfqd->peak_rate_samples = 1; // 이 rq 자체가 첫 표본이므로 표본 수를 1로 시작
		bfqd->sequential_samples = 0; // 순차 표본 카운터는 아직 없음(첫 표본은 순차 여부를 판단할 이전 표본이 없으므로 0)
		/* [한국어] 누적 디스패치 섹터 수와 "최근 최대 요청 크기"를 모두 이번 rq의 섹터 수로 초기화 */
		bfqd->tot_sectors_dispatched = bfqd->last_rq_max_size =
			blk_rq_sectors(rq);
	} else /* no new rq dispatched, just reset the number of samples */
		bfqd->peak_rate_samples = 0; /* full re-init on next disp. */ // rq가 없는 리셋(조건 미달 등)은 표본 수만 0으로 만들어, 다음 dispatch 때 위 if(rq != NULL) 분기가 아니라 "peak_rate_samples == 0"이라는 완전 초기화 분기가 실행되도록 유도

	/* [한국어] 디버그 트레이스: 리셋 직후의 표본 상태 기록 */
	bfq_log(bfqd,
		"reset_rate_computation at end, sample %u/%u tot_sects %llu",
		bfqd->peak_rate_samples, bfqd->sequential_samples,
		bfqd->tot_sectors_dispatched);
}

/*
 * [한국어]
 * bfq_update_rate_reset - 관측 구간(observation interval) 하나가 끝났을 때
 * 그 구간의 표본들로부터 새 peak_rate(장치 최대 처리율)를 저역통과필터
 * (low-pass filter)로 반영하고, 다음 관측을 위해 상태를 리셋한다.
 *
 * @bfqd: 장치 전역 상태. peak_rate_samples, sequential_samples,
 *        delta_from_first, tot_sectors_dispatched, peak_rate 등 관측/
 *        추정 상태를 갱신한다.
 * @rq: 이 리셋을 유발한 request(있으면 다음 관측 구간을 이 rq로 새로
 *      시작, 없으면 표본 카운터만 리셋). bfq_reset_rate_computation에
 *      그대로 전달된다.
 * @return: 없음(void).
 *
 * BFQ는 dispatch 시각들 사이의 간격과 그 사이 처리된 섹터 수로부터
 * "장치가 낼 수 있는 최대 처리율(peak_rate)"을 추정한다(자세한 원리는
 * bfq_update_peak_rate() 주석 참고). 표본이 충분히 쌓이고(peak_rate_samples
 * >= BFQ_RATE_MIN_SAMPLES) 관측 시간도 충분히 길면(delta_from_first >=
 * BFQ_RATE_MIN_INTERVAL) 관측 구간의 rate를 계산해 저역통과필터로
 * peak_rate에 반영한다. 표본이 순차적일수록, 관측 구간이 길수록 이번
 * 측정값(rate)에 더 큰 신뢰(가중치)를 주어 peak_rate를 더 빠르게
 * 갱신하고, 반대의 경우 과거 추정치(bfqd->peak_rate)를 더 신뢰해
 * 노이즈에 덜 흔들리게 한다. rate와 peak_rate는 모두 BFQ_RATE_SHIFT
 * (16비트) 좌측 시프트된 고정소수점 [sectors/usec] 단위로 표현된다.
 *
 * 동작 순서:
 *   1) 표본 수/관측 시간이 최소 기준에 못 미치면 이번 구간은 버리고
 *      reset_computation으로 건너뛴다(rate 갱신 없이 리셋만).
 *   2) 마지막 dispatch 이후에 완료(completion)가 발생했다면, 관측
 *      구간을 그 완료 시점까지 늘려(delta_from_first를 더 큰 값으로
 *      확장) 더 정확한 rate를 얻는다.
 *   3) rate = tot_sectors_dispatched(<<BFQ_RATE_SHIFT) / delta_from_first
 *      (usec 단위) - 관측 구간 동안의 평균 처리율을 고정소수점으로
 *      계산.
 *   4) "표본의 3/4 미만이 순차적이면서 rate가 기존 peak_rate 이하"
 *      이거나 "rate가 20M sectors/sec을 초과"하면(비정상치) 이번
 *      측정은 신뢰할 수 없다고 보고 리셋만 수행.
 *   5) 그렇지 않으면 실제로 peak_rate를 갱신: 순차성과 관측 시간
 *      길이에 비례하는 weight(0~8)를 계산하고, divisor = 10 - weight
 *      (2~10 범위)를 스무딩 상수(smoothing constant, alpha = 1/divisor)
 *      로 삼아 peak_rate = peak_rate*(divisor-1)/divisor + rate/divisor
 *      형태의 저역통과필터를 적용한다. weight가 클수록(순차적이고 관측이
 *      길수록) divisor가 작아져(최소 2) 새 측정값의 비중이 커진다.
 *   6) peak_rate가 0이 되면(매우 느린 장치) 나눗셈 오류 방지를 위해
 *      최소 1로 클램프.
 *   7) update_thr_responsiveness_params()로 max_budget 등 파생값을
 *      재계산.
 *   8) reset_computation 레이블: 다음 관측 구간을 위해
 *      bfq_reset_rate_computation()으로 표본 상태를 리셋(공통 종료
 *      경로 - goto는 실패/성공 모든 경로가 반드시 리셋을 거치도록
 *      강제하기 위함).
 *
 * 실행 컨텍스트: 호출자(bfq_update_peak_rate)가 bfqd->lock을 보유한
 * 상태에서 실행. dispatch 경로에서 호출되므로 매 dispatch마다 실행될
 * 수 있음.
 *
 * 호출 체인:
 *   bfq_update_peak_rate → [bfq_update_rate_reset]
 *   → update_thr_responsiveness_params, bfq_reset_rate_computation
 */
static void bfq_update_rate_reset(struct bfq_data *bfqd, struct request *rq)
{
	u32 rate, weight, divisor; // rate: 이번 구간 측정 처리율, weight: 신뢰도(0~8), divisor: 저역통과필터 스무딩 상수(2~10)

	/*
	 * For the convergence property to hold (see comments on
	 * bfq_update_peak_rate()) and for the assessment to be
	 * reliable, a minimum number of samples must be present, and
	 * a minimum amount of time must have elapsed. If not so, do
	 * not compute new rate. Just reset parameters, to get ready
	 * for a new evaluation attempt.
	 */
	if (bfqd->peak_rate_samples < BFQ_RATE_MIN_SAMPLES || // 표본이 BFQ_RATE_MIN_SAMPLES(32개) 미만이거나
	    bfqd->delta_from_first < BFQ_RATE_MIN_INTERVAL) // 관측 시간이 BFQ_RATE_MIN_INTERVAL(300ms) 미만이면
		goto reset_computation; // 신뢰할 수 없는 관측이므로 rate 갱신 없이 바로 리셋 단계로 이동

	/*
	 * If a new request completion has occurred after last
	 * dispatch, then, to approximate the rate at which requests
	 * have been served by the device, it is more precise to
	 * extend the observation interval to the last completion.
	 */
	/* [한국어] 마지막 dispatch 이후 완료가 있었다면 그 시점까지 관측 구간을 늘려 실제 서비스 시간에 더 가깝게 근사 */
	bfqd->delta_from_first =
		max_t(u64, bfqd->delta_from_first,
		      bfqd->last_completion - bfqd->first_dispatch);

	/*
	 * Rate computed in sects/usec, and not sects/nsec, for
	 * precision issues.
	 */
	/* [한국어] rate[sectors/usec, <<16] = (누적 디스패치 섹터 수 << 16) / (관측 시간을 나노초에서
	 * 마이크로초로 변환한 값) - usec 단위로 나눠 정밀도 손실을 줄임 */
	rate = div64_ul(bfqd->tot_sectors_dispatched<<BFQ_RATE_SHIFT,
			div_u64(bfqd->delta_from_first, NSEC_PER_USEC));

	/*
	 * Peak rate not updated if:
	 * - the percentage of sequential dispatches is below 3/4 of the
	 *   total, and rate is below the current estimated peak rate
	 * - rate is unreasonably high (> 20M sectors/sec)
	 */
	if ((bfqd->sequential_samples < (3 * bfqd->peak_rate_samples)>>2 && // 순차 표본이 전체의 3/4 미만(>>2는 /4) - peak "최고" 처리율을 논하기엔 랜덤 성분이 너무 많은 구간이다
	     rate <= bfqd->peak_rate) || // 순차 표본 비율이 75% 미만이면서 이번 rate가 기존 peak_rate보다 낮으면(신뢰도 낮은 하향 측정)
		rate > 20<<BFQ_RATE_SHIFT) // 또는 rate가 20M sectors/sec(<<16 스케일)을 초과하는 비현실적인 값이면
		goto reset_computation; // 이번 측정은 반영하지 않고 리셋만 수행(노이즈로 peak_rate가 오염되는 것을 방지)

	/*
	 * We have to update the peak rate, at last! To this purpose,
	 * we use a low-pass filter. We compute the smoothing constant
	 * of the filter as a function of the 'weight' of the new
	 * measured rate.
	 *
	 * As can be seen in next formulas, we define this weight as a
	 * quantity proportional to how sequential the workload is,
	 * and to how long the observation time interval is.
	 *
	 * The weight runs from 0 to 8. The maximum value of the
	 * weight, 8, yields the minimum value for the smoothing
	 * constant. At this minimum value for the smoothing constant,
	 * the measured rate contributes for half of the next value of
	 * the estimated peak rate.
	 *
	 * So, the first step is to compute the weight as a function
	 * of how sequential the workload is. Note that the weight
	 * cannot reach 9, because bfqd->sequential_samples cannot
	 * become equal to bfqd->peak_rate_samples, which, in its
	 * turn, holds true because bfqd->sequential_samples is not
	 * incremented for the first sample.
	 */
	weight = (9 * bfqd->sequential_samples) / bfqd->peak_rate_samples; // 1단계: 순차 표본 비율에 비례하는 weight(0~8 범위, 전부 순차여도 9에는 못 미침) 계산 - 순차적일수록 이번 측정을 더 신뢰

	/*
	 * Second step: further refine the weight as a function of the
	 * duration of the observation interval.
	 */
	/* [한국어] 2단계: 관측 시간이 기준 구간(BFQ_RATE_REF_INTERVAL, 1초)에 비해 얼마나 긴지도 반영 - 길게
	 * 관측할수록 신뢰도(weight)를 최대 8까지 끌어올림 */
	weight = min_t(u32, 8,
		       div_u64(weight * bfqd->delta_from_first,
			       BFQ_RATE_REF_INTERVAL));

	/*
	 * Divisor ranging from 10, for minimum weight, to 2, for
	 * maximum weight.
	 */
	divisor = 10 - weight; // weight가 0이면 divisor=10(과거 값 위주, 변화 완만), weight가 8이면 divisor=2(새 측정값 비중 최대 1/2)

	/*
	 * Finally, update peak rate:
	 *
	 * peak_rate = peak_rate * (divisor-1) / divisor  +  rate / divisor
	 */
	bfqd->peak_rate *= divisor-1; // 저역통과필터 항 1: 기존 peak_rate에 (divisor-1)을 곱함 - 다음 줄의 나눗셈과 합쳐 (divisor-1)/divisor 가중치를 만듦
	bfqd->peak_rate /= divisor; // divisor로 나눠 (divisor-1)/divisor 비율만 남김 - 이것이 "과거 추정치를 얼마나 유지할지"의 비중(alpha의 여집합)
	rate /= divisor; /* smoothing constant alpha = 1/divisor */ // 이번 측정값 rate에는 1/divisor 비중만 반영되도록 나눔 - alpha(새 값 반영 비율) = 1/divisor

	bfqd->peak_rate += rate; // 두 항을 더해 peak_rate = 기존값*(divisor-1)/divisor + rate*(1/divisor) 완성 - 저역통과필터의 최종 갱신식

	/*
	 * For a very slow device, bfqd->peak_rate can reach 0 (see
	 * the minimum representable values reported in the comments
	 * on BFQ_RATE_SHIFT). Push to 1 if this happens, to avoid
	 * divisions by zero where bfqd->peak_rate is used as a
	 * divisor.
	 */
	bfqd->peak_rate = max_t(u32, 1, bfqd->peak_rate); // peak_rate가 정수 나눗셈으로 0이 되는 것을 방지 - 이후 peak_rate가 제수로 쓰이는 곳(bfq_calc_max_budget 등)에서 0-division을 막기 위한 안전장치

	update_thr_responsiveness_params(bfqd); // 새 peak_rate를 반영해 max_budget 등 파생 파라미터를 재계산

reset_computation: // 성공/실패(조건 미달) 모든 경로가 공통으로 거치는 리셋 지점
	bfq_reset_rate_computation(bfqd, rq); // 다음 관측 구간을 위해 표본 카운터/누적값을 초기화(rq가 있으면 이번 rq로 새 구간 시작)
}

/*
 * Update the read/write peak rate (the main quantity used for
 * auto-tuning, see update_thr_responsiveness_params()).
 *
 * It is not trivial to estimate the peak rate (correctly): because of
 * the presence of sw and hw queues between the scheduler and the
 * device components that finally serve I/O requests, it is hard to
 * say exactly when a given dispatched request is served inside the
 * device, and for how long. As a consequence, it is hard to know
 * precisely at what rate a given set of requests is actually served
 * by the device.
 *
 * On the opposite end, the dispatch time of any request is trivially
 * available, and, from this piece of information, the "dispatch rate"
 * of requests can be immediately computed. So, the idea in the next
 * function is to use what is known, namely request dispatch times
 * (plus, when useful, request completion times), to estimate what is
 * unknown, namely in-device request service rate.
 *
 * The main issue is that, because of the above facts, the rate at
 * which a certain set of requests is dispatched over a certain time
 * interval can vary greatly with respect to the rate at which the
 * same requests are then served. But, since the size of any
 * intermediate queue is limited, and the service scheme is lossless
 * (no request is silently dropped), the following obvious convergence
 * property holds: the number of requests dispatched MUST become
 * closer and closer to the number of requests completed as the
 * observation interval grows. This is the key property used in
 * the next function to estimate the peak service rate as a function
 * of the observed dispatch rate. The function assumes to be invoked
 * on every request dispatch.
 */
/*
 * [한국어]
 * bfq_update_peak_rate - request가 dispatch될 때마다 호출되어, dispatch
 * 시각들 사이의 간격으로부터 장치의 실제 서비스 처리율(peak_rate)을
 * 간접적으로 추정한다.
 *
 * @bfqd: 장치 전역 상태. peak_rate_samples, last_dispatch,
 *        first_dispatch, tot_rq_in_driver(현재 장치에 내려간 미완료
 *        요청 수), last_position(마지막 처리 섹터 위치) 등을 관리한다.
 * @rq: 방금 dispatch된 request. 섹터 수/위치가 표본에 반영된다.
 * @return: 없음(void).
 *
 * NVMe SSD 같은 장치는 스케줄러와 실제 미디어 사이에 컨트롤러의 SQ/CQ,
 * 내부 병렬 채널 등 여러 단계가 있어 "이 request가 정확히 언제, 얼마나
 * 걸려 서비스됐는지"를 스케줄러가 직접 알 방법이 없다. 반면 dispatch
 * 시각은 스케줄러가 항상 정확히 알 수 있다. 이 함수는 위 파일 상단
 * 주석에서 설명한 수렴 성질(관측 구간이 길어질수록 dispatch 속도가
 * 실제 서비스 속도에 수렴)을 이용해, dispatch 시각 간격만으로 장치의
 * peak_rate를 근사한다. 매 dispatch마다 호출되는 것을 전제로 한다.
 *
 * 동작 순서:
 *   1) peak_rate_samples == 0(이번이 첫 dispatch, 또는 직전에 완전
 *      리셋된 상태)이면 bfq_reset_rate_computation()으로 새 관측
 *      구간을 시작하고 update_last_values로 건너뛴다(표본 1개는 곧
 *      추가됨).
 *   2) 마지막 dispatch 이후 100ms 넘게 지났고 현재 장치에 진행 중인
 *      요청이 하나도 없다면(장치가 오래 idle 상태였다면), 그 사이의
 *      "빈 시간"이 관측 구간에 섞이면 rate가 왜곡되므로
 *      update_rate_and_reset으로 건너뛰어 지금까지의 구간을 마감하고
 *      바로 rate를 계산한 뒤 이번 dispatch로 새 구간을 시작한다.
 *   3) 그 외의 정상 경로: 표본 수(peak_rate_samples)를 늘리고, 만약
 *      "현재 진행 중인 요청이 있거나(즉 장치가 바빴거나) 마지막 완료
 *      이후 BFQ_MIN_TT(2ms) 이내"이면서 이번 rq가 이전 위치와
 *      비-seeky(순차적)하면 sequential_samples도 증가시킨다.
 *   4) 누적 디스패치 섹터 수(tot_sectors_dispatched)를 갱신하고, 32
 *      dispatch마다 "최근 관측된 최대 요청 크기"(last_rq_max_size)를
 *      리셋해 통계가 오래된 이상치에 고착되지 않게 한다.
 *   5) delta_from_first(첫 dispatch 이후 경과 시간)을 갱신하고, 아직
 *      목표 관측 시간(BFQ_RATE_REF_INTERVAL, 1초)에 못 미쳤으면
 *      update_last_values로 건너뛰어 표본을 계속 쌓는다.
 *   6) 목표 시간에 도달했으면 update_rate_and_reset 레이블로 진입해
 *      bfq_update_rate_reset()으로 실제 rate 계산/peak_rate 갱신을
 *      수행한다.
 *   7) update_last_values 레이블: 이번 rq의 위치(+크기)를
 *      last_position에 기록하고, 이 rq가 현재 in-service 큐 소속이면
 *      in_serv_last_pos도 갱신, last_dispatch를 지금 시각으로 갱신.
 *
 * 실행 컨텍스트: 호출자(bfq_dispatch_remove)가 bfqd->lock을 보유한
 * 상태에서 dispatch 경로 중 실행. 매 request dispatch마다 호출되므로
 * I/O 핫패스에 포함.
 *
 * 호출 체인:
 *   bfq_dispatch_remove → [bfq_update_peak_rate]
 *   → bfq_reset_rate_computation / bfq_update_rate_reset
 */
static void bfq_update_peak_rate(struct bfq_data *bfqd, struct request *rq)
{
	u64 now_ns = blk_time_get_ns(); // 이번 dispatch의 현재 시각(모노토닉, 나노초) - 이후 모든 시간차 계산의 기준

	if (bfqd->peak_rate_samples == 0) { /* first dispatch */ // 표본이 아예 없는 상태 - 스케줄러 시작 직후이거나 직전 리셋에서 완전 초기화된 경우
		/* [한국어] 디버그 트레이스: 리셋 경로로 들어감을 기록 */
		bfq_log(bfqd, "update_peak_rate: goto reset, samples %d",
			bfqd->peak_rate_samples);
		bfq_reset_rate_computation(bfqd, rq); // 이번 rq를 첫 표본으로 삼아 관측 구간을 새로 시작
		goto update_last_values; /* will add one sample */ // rate 계산은 아직 이르므로(표본 1개) 바로 위치/시각 갱신 단계로 건너뜀
	}

	/*
	 * Device idle for very long: the observation interval lasting
	 * up to this dispatch cannot be a valid observation interval
	 * for computing a new peak rate (similarly to the late-
	 * completion event in bfq_completed_request()). Go to
	 * update_rate_and_reset to have the following three steps
	 * taken:
	 * - close the observation interval at the last (previous)
	 *   request dispatch or completion
	 * - compute rate, if possible, for that observation interval
	 * - start a new observation interval with this dispatch
	 */
	if (now_ns - bfqd->last_dispatch > 100*NSEC_PER_MSEC && // 100ms는 "관측 구간이 유휴로 오염됐다"고 볼 만큼 넉넉한 값 - 기준 관측 구간(BFQ_RATE_REF_INTERVAL, 1초)의 1/10이라 이보다 짧은 공백은 노이즈로 흡수한다
	    bfqd->tot_rq_in_driver == 0) // 마지막 dispatch로부터 100ms 넘게 지났고 그동안 장치에 진행 중인 요청도 없었다면(장치가 오래 놀았음)
		goto update_rate_and_reset; // 그 유휴 구간이 rate 계산을 왜곡하지 않도록, 지금까지의 구간을 즉시 마감하고 rate를 계산한 뒤 새 구간을 시작

	/* Update sampling information */
	bfqd->peak_rate_samples++; // 정상적인 연속 dispatch이므로 표본 수 증가

	if ((bfqd->tot_rq_in_driver > 0 || // 장치가 실제로 일하고 있어야 이 표본이 "처리율"을 반영한다 - 놀고 있는 구간의 dispatch는 rate를 과소평가하게 만든다
		now_ns - bfqd->last_completion < BFQ_MIN_TT) // 또는 직전 완료가 BFQ_MIN_TT(2ms) 이내였다면 장치가 사실상 연속 가동 중이라고 간주
	    && !BFQ_RQ_SEEKY(bfqd, bfqd->last_position, rq)) // 장치가 바쁘거나(진행 중 요청 존재) 최근 완료가 있었고(2ms 이내), 이번 rq가 이전 위치에서 순차적(비-seeky)이면
		bfqd->sequential_samples++; // "순차 표본" 카운터 증가 - 이 비율이 bfq_update_rate_reset의 신뢰도(weight) 계산에 쓰임

	bfqd->tot_sectors_dispatched += blk_rq_sectors(rq); // 누적 디스패치 섹터 수에 이번 rq의 섹터 수를 더함 - rate 계산의 분자가 됨

	/* Reset max observed rq size every 32 dispatches */
	if (likely(bfqd->peak_rate_samples % 32)) // 32번째 dispatch가 아니면(대부분의 경우)
		bfqd->last_rq_max_size = // 이 값은 bfq_update_peak_rate()가 "이번 dispatch가 관측 구간을 늘릴 만큼 큰 요청이었는지" 판단할 때 쓰이는 참조 크기다
			max_t(u32, blk_rq_sectors(rq), bfqd->last_rq_max_size); // 지금까지 관측된 최대 요청 크기와 이번 rq 크기 중 큰 값으로 갱신(단조 누적)
	/* [한국어] 32번째마다 누적을 리셋해 이번 rq 크기로 다시 시작 - 오래된 이상치가 영구히 남지 않도록 주기적으로 갱신 */
	else
		bfqd->last_rq_max_size = blk_rq_sectors(rq);

	bfqd->delta_from_first = now_ns - bfqd->first_dispatch; // 첫 dispatch 이후 지금까지 경과한 시간(관측 구간 길이) 갱신

	/* Target observation interval not yet reached, go on sampling */
	if (bfqd->delta_from_first < BFQ_RATE_REF_INTERVAL) // 목표 관측 시간(1초)에 아직 못 미쳤다면
		goto update_last_values; // rate 계산 없이 표본을 계속 쌓기 위해 건너뜀

update_rate_and_reset: // 목표 관측 시간 도달, 또는 장기 유휴 이후 조기 마감 경로가 공통으로 도달하는 지점
	bfq_update_rate_reset(bfqd, rq); // 실제 rate 계산과 peak_rate 저역통과필터 갱신, 그리고 다음 구간을 위한 리셋을 수행
update_last_values: // 표본 갱신과 무관하게 항상 실행되어야 하는 "마지막 상태 갱신" 공통 지점
	bfqd->last_position = blk_rq_pos(rq) + blk_rq_sectors(rq); // 이번 rq가 끝나는 섹터 위치를 기록 - 다음 dispatch의 seeky 판정 기준점이 됨
	if (RQ_BFQQ(rq) == bfqd->in_service_queue) // 이 rq가 현재 in-service 큐 소속이라면
		bfqd->in_serv_last_pos = bfqd->last_position; // in-service 큐 전용 마지막 위치도 함께 갱신(cooperator merge의 "인접 위치" 판정 등에 사용)
	bfqd->last_dispatch = now_ns; // 다음 호출에서 dispatch 간격을 계산할 수 있도록 이번 dispatch 시각을 기록
}

/*
 * Remove request from internal lists.
 */
/*
 * [한국어]
 * bfq_dispatch_remove - dispatch가 확정된 request를 BFQ 내부 자료구조
 * (bfqq의 정렬 트리 등)에서 제거하고, peak_rate 추정용 통계를 갱신한다.
 *
 * @q: 이 request가 속한 request_queue.
 * @rq: 방금 디스패치되어 드라이버로 넘어가는 request.
 * @return: 없음(void).
 *
 * elevator가 request를 실제로 드라이버(블록 디바이스 드라이버, NVMe의
 * 경우 nvme_queue_rq 등)에 넘기기 직전에 호출되어, BFQ 내부 장부
 * (bfqq->dispatched 카운터, 정렬 트리)를 정리하고 peak_rate 관측
 * 표본을 하나 추가한다.
 *
 * 동작:
 *   1) bfqq->dispatched를 먼저 증가시킨다. 원칙적으로는 요청을 큐에서
 *      제거하고 실제로 디스패치한 "뒤에" 증가시키는 것이 논리적으로
 *      맞지만, 효율을 위해 순서를 바꿔 먼저 증가시킨다(주석에 설명된
 *      대로, in-service가 아닌 bfqq에 대한 dispatch가 뒤이어 곧바로
 *      감소/재증가되는 낭비를 피하기 위함).
 *   2) bfq_update_peak_rate()로 이번 dispatch를 표본 삼아 장치
 *      처리율 추정을 갱신한다.
 *   3) bfq_remove_request()로 이 rq를 bfqq의 정렬 트리(sort_list)와
 *      위치 트리(rq_pos_tree) 등 BFQ 내부 자료구조에서 실제로
 *      제거한다(드라이버의 SQ/CQ에서 제거하는 것이 아니라, BFQ가
 *      더 이상 스케줄링 대상으로 추적하지 않는다는 의미).
 *
 * 실행 컨텍스트: 호출자(bfq_dispatch_request)가 bfqd->lock을 보유한
 * 상태에서 실행. dispatch 핫패스.
 *
 * 호출 체인:
 *   bfq_dispatch_request → [bfq_dispatch_remove]
 *   → bfq_update_peak_rate, bfq_remove_request
 */
static void bfq_dispatch_remove(struct request_queue *q, struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq); // 이 request가 속한 bfq_queue를 request의 elevator_private 필드에서 복원

	/*
	 * For consistency, the next instruction should have been
	 * executed after removing the request from the queue and
	 * dispatching it.  We execute instead this instruction before
	 * bfq_remove_request() (and hence introduce a temporary
	 * inconsistency), for efficiency.  In fact, should this
	 * dispatch occur for a non in-service bfqq, this anticipated
	 * increment prevents two counters related to bfqq->dispatched
	 * from risking to be, first, uselessly decremented, and then
	 * incremented again when the (new) value of bfqq->dispatched
	 * happens to be taken into account.
	 */
	bfqq->dispatched++; // 이 bfqq가 드라이버에 내려보낸(아직 완료되지 않은) 요청 수를 증가 - 순서상 다소 이르지만 효율을 위해 제거 전에 미리 반영
	bfq_update_peak_rate(q->elevator->elevator_data, rq); // 이번 dispatch를 표본으로 삼아 장치 peak_rate 추정을 갱신

	bfq_remove_request(q, rq); // BFQ 내부 정렬 트리/위치 트리에서 이 request를 제거 - 이후 BFQ는 이 rq를 더 이상 스케줄링 대상으로 보지 않음(완료 처리는 별도 경로)
}

/*
 * There is a case where idling does not have to be performed for
 * throughput concerns, but to preserve the throughput share of
 * the process associated with bfqq.
 *
 * To introduce this case, we can note that allowing the drive
 * to enqueue more than one request at a time, and hence
 * delegating de facto final scheduling decisions to the
 * drive's internal scheduler, entails loss of control on the
 * actual request service order. In particular, the critical
 * situation is when requests from different processes happen
 * to be present, at the same time, in the internal queue(s)
 * of the drive. In such a situation, the drive, by deciding
 * the service order of the internally-queued requests, does
 * determine also the actual throughput distribution among
 * these processes. But the drive typically has no notion or
 * concern about per-process throughput distribution, and
 * makes its decisions only on a per-request basis. Therefore,
 * the service distribution enforced by the drive's internal
 * scheduler is likely to coincide with the desired throughput
 * distribution only in a completely symmetric, or favorably
 * skewed scenario where:
 * (i-a) each of these processes must get the same throughput as
 *	 the others,
 * (i-b) in case (i-a) does not hold, it holds that the process
 *       associated with bfqq must receive a lower or equal
 *	 throughput than any of the other processes;
 * (ii)  the I/O of each process has the same properties, in
 *       terms of locality (sequential or random), direction
 *       (reads or writes), request sizes, greediness
 *       (from I/O-bound to sporadic), and so on;

 * In fact, in such a scenario, the drive tends to treat the requests
 * of each process in about the same way as the requests of the
 * others, and thus to provide each of these processes with about the
 * same throughput.  This is exactly the desired throughput
 * distribution if (i-a) holds, or, if (i-b) holds instead, this is an
 * even more convenient distribution for (the process associated with)
 * bfqq.
 *
 * In contrast, in any asymmetric or unfavorable scenario, device
 * idling (I/O-dispatch plugging) is certainly needed to guarantee
 * that bfqq receives its assigned fraction of the device throughput
 * (see [1] for details).
 *
 * The problem is that idling may significantly reduce throughput with
 * certain combinations of types of I/O and devices. An important
 * example is sync random I/O on flash storage with command
 * queueing. So, unless bfqq falls in cases where idling also boosts
 * throughput, it is important to check conditions (i-a), i(-b) and
 * (ii) accurately, so as to avoid idling when not strictly needed for
 * service guarantees.
 *
 * Unfortunately, it is extremely difficult to thoroughly check
 * condition (ii). And, in case there are active groups, it becomes
 * very difficult to check conditions (i-a) and (i-b) too.  In fact,
 * if there are active groups, then, for conditions (i-a) or (i-b) to
 * become false 'indirectly', it is enough that an active group
 * contains more active processes or sub-groups than some other active
 * group. More precisely, for conditions (i-a) or (i-b) to become
 * false because of such a group, it is not even necessary that the
 * group is (still) active: it is sufficient that, even if the group
 * has become inactive, some of its descendant processes still have
 * some request already dispatched but still waiting for
 * completion. In fact, requests have still to be guaranteed their
 * share of the throughput even after being dispatched. In this
 * respect, it is easy to show that, if a group frequently becomes
 * inactive while still having in-flight requests, and if, when this
 * happens, the group is not considered in the calculation of whether
 * the scenario is asymmetric, then the group may fail to be
 * guaranteed its fair share of the throughput (basically because
 * idling may not be performed for the descendant processes of the
 * group, but it had to be).  We address this issue with the following
 * bi-modal behavior, implemented in the function
 * bfq_asymmetric_scenario().
 *
 * If there are groups with requests waiting for completion
 * (as commented above, some of these groups may even be
 * already inactive), then the scenario is tagged as
 * asymmetric, conservatively, without checking any of the
 * conditions (i-a), (i-b) or (ii). So the device is idled for bfqq.
 * This behavior matches also the fact that groups are created
 * exactly if controlling I/O is a primary concern (to
 * preserve bandwidth and latency guarantees).
 *
 * On the opposite end, if there are no groups with requests waiting
 * for completion, then only conditions (i-a) and (i-b) are actually
 * controlled, i.e., provided that conditions (i-a) or (i-b) holds,
 * idling is not performed, regardless of whether condition (ii)
 * holds.  In other words, only if conditions (i-a) and (i-b) do not
 * hold, then idling is allowed, and the device tends to be prevented
 * from queueing many requests, possibly of several processes. Since
 * there are no groups with requests waiting for completion, then, to
 * control conditions (i-a) and (i-b) it is enough to check just
 * whether all the queues with requests waiting for completion also
 * have the same weight.
 *
 * Not checking condition (ii) evidently exposes bfqq to the
 * risk of getting less throughput than its fair share.
 * However, for queues with the same weight, a further
 * mechanism, preemption, mitigates or even eliminates this
 * problem. And it does so without consequences on overall
 * throughput. This mechanism and its benefits are explained
 * in the next three paragraphs.
 *
 * Even if a queue, say Q, is expired when it remains idle, Q
 * can still preempt the new in-service queue if the next
 * request of Q arrives soon (see the comments on
 * bfq_bfqq_update_budg_for_activation). If all queues and
 * groups have the same weight, this form of preemption,
 * combined with the hole-recovery heuristic described in the
 * comments on function bfq_bfqq_update_budg_for_activation,
 * are enough to preserve a correct bandwidth distribution in
 * the mid term, even without idling. In fact, even if not
 * idling allows the internal queues of the device to contain
 * many requests, and thus to reorder requests, we can rather
 * safely assume that the internal scheduler still preserves a
 * minimum of mid-term fairness.
 *
 * More precisely, this preemption-based, idleless approach
 * provides fairness in terms of IOPS, and not sectors per
 * second. This can be seen with a simple example. Suppose
 * that there are two queues with the same weight, but that
 * the first queue receives requests of 8 sectors, while the
 * second queue receives requests of 1024 sectors. In
 * addition, suppose that each of the two queues contains at
 * most one request at a time, which implies that each queue
 * always remains idle after it is served. Finally, after
 * remaining idle, each queue receives very quickly a new
 * request. It follows that the two queues are served
 * alternatively, preempting each other if needed. This
 * implies that, although both queues have the same weight,
 * the queue with large requests receives a service that is
 * 1024/8 times as high as the service received by the other
 * queue.
 *
 * The motivation for using preemption instead of idling (for
 * queues with the same weight) is that, by not idling,
 * service guarantees are preserved (completely or at least in
 * part) without minimally sacrificing throughput. And, if
 * there is no active group, then the primary expectation for
 * this device is probably a high throughput.
 *
 * We are now left only with explaining the two sub-conditions in the
 * additional compound condition that is checked below for deciding
 * whether the scenario is asymmetric. To explain the first
 * sub-condition, we need to add that the function
 * bfq_asymmetric_scenario checks the weights of only
 * non-weight-raised queues, for efficiency reasons (see comments on
 * bfq_weights_tree_add()). Then the fact that bfqq is weight-raised
 * is checked explicitly here. More precisely, the compound condition
 * below takes into account also the fact that, even if bfqq is being
 * weight-raised, the scenario is still symmetric if all queues with
 * requests waiting for completion happen to be
 * weight-raised. Actually, we should be even more precise here, and
 * differentiate between interactive weight raising and soft real-time
 * weight raising.
 *
 * The second sub-condition checked in the compound condition is
 * whether there is a fair amount of already in-flight I/O not
 * belonging to bfqq. If so, I/O dispatching is to be plugged, for the
 * following reason. The drive may decide to serve in-flight
 * non-bfqq's I/O requests before bfqq's ones, thereby delaying the
 * arrival of new I/O requests for bfqq (recall that bfqq is sync). If
 * I/O-dispatching is not plugged, then, while bfqq remains empty, a
 * basically uncontrolled amount of I/O from other queues may be
 * dispatched too, possibly causing the service of bfqq's I/O to be
 * delayed even longer in the drive. This problem gets more and more
 * serious as the speed and the queue depth of the drive grow,
 * because, as these two quantities grow, the probability to find no
 * queue busy but many requests in flight grows too. By contrast,
 * plugging I/O dispatching minimizes the delay induced by already
 * in-flight I/O, and enables bfqq to recover the bandwidth it may
 * lose because of this delay.
 *
 * As a side note, it is worth considering that the above
 * device-idling countermeasures may however fail in the following
 * unlucky scenario: if I/O-dispatch plugging is (correctly) disabled
 * in a time period during which all symmetry sub-conditions hold, and
 * therefore the device is allowed to enqueue many requests, but at
 * some later point in time some sub-condition stops to hold, then it
 * may become impossible to make requests be served in the desired
 * order until all the requests already queued in the device have been
 * served. The last sub-condition commented above somewhat mitigates
 * this problem for weight-raised queues.
 *
 * However, as an additional mitigation for this problem, we preserve
 * plugging for a special symmetric case that may suddenly turn into
 * asymmetric: the case where only bfqq is busy. In this case, not
 * expiring bfqq does not cause any harm to any other queues in terms
 * of service guarantees. In contrast, it avoids the following unlucky
 * sequence of events: (1) bfqq is expired, (2) a new queue with a
 * lower weight than bfqq becomes busy (or more queues), (3) the new
 * queue is served until a new request arrives for bfqq, (4) when bfqq
 * is finally served, there are so many requests of the new queue in
 * the drive that the pending requests for bfqq take a lot of time to
 * be served. In particular, event (2) may case even already
 * dispatched requests of bfqq to be delayed, inside the drive. So, to
 * avoid this series of events, the scenario is preventively declared
 * as asymmetric also if bfqq is the only busy queues
 */
/*
 * [한국어]
 * idling_needed_for_service_guarantees - bfqq 의 대역폭 보장을 위해
 * device idling 이 "반드시" 필요한지 판단한다 (처리량 손해를 감수해서라도).
 *
 * @bfqd: 디스크 전체 상태를 담는 BFQ 스케줄러 데이터.
 * @bfqq: 방금 request 가 바닥나 비어있게 된, idling 여부를 판단할 대상 큐.
 * @return: true 면 idling(디스패치 중단)이 서비스 보장을 위해 필요하다는 뜻.
 *          이 경우 처리량이 떨어지더라도 idling 을 강행해야 한다.
 *          false 면 이 관점에서는 idling 이 굳이 필요 없다는 뜻(다른 이유로
 *          idling 할 수는 있음, bfq_better_to_idle 의 다른 항 참고).
 *
 * 위쪽의 긴 커널 원본 주석이 설명하듯, NVMe/SATA NCQ 처럼 컨트롤러가
 * 내부적으로 여러 request 를 재정렬(reorder)할 수 있는 장치에서는,
 * 대칭적(symmetric)이지 않은 시나리오(가중치가 서로 다르거나, cgroup 이
 * 섞여 있거나, in-flight 상태로 남아있는 타 큐의 요청이 많은 경우)에는
 * device 의 내부 스케줄러에 순서 결정을 맡기면 BFQ 가 부여하려는
 * 가중치/우선순위 배분이 깨진다. 이런 경우엔 bfqq 가 비어도 즉시 다른
 * 큐로 전환하지 않고 짧게 idle 하여 bfqq 의 다음 request 가 순서를
 * 지키며 도착하도록 기다려야 공정성이 보장된다.
 *
 * 실행 컨텍스트: in-service 큐가 비게 된 시점에 스케줄링 결정을 내리는
 * 경로(bfq_select_queue 계열)에서 bfqd->lock 을 쥔 채 호출된다.
 *
 * 호출 체인:
 *   bfq_better_to_idle → [idling_needed_for_service_guarantees] → bfq_asymmetric_scenario
 */
static bool idling_needed_for_service_guarantees(struct bfq_data *bfqd,
						 struct bfq_queue *bfqq)
{
	/* [한국어] 현재 활성 상태(request 대기 중이거나 in-flight 요청이 있는) bfq_queue 총 개수 -
	 * 이 값이 1이면 경쟁 큐가 없다는 뜻이라 공정성 문제 자체가 성립하지 않는다. */
	int tot_busy_queues = bfq_tot_busy_queues(bfqd);

	/* No point in idling for bfqq if it won't get requests any longer */
	/* [한국어] bfqq 를 참조하는 프로세스(io_context)가 이미 하나도 남지 않았다면
	 * (프로세스가 종료되었거나 다른 큐로 옮겨간 경우) 앞으로 새 request 가 절대
	 * 도착하지 않으므로, 기다려도 의미가 없다 - idling 불필요 판정. */
	if (unlikely(!bfqq_process_refs(bfqq)))
		return false;
		/* [한국어] false 반환: idling 을 강제할 근거가 없음 - 즉시 다음 큐로 전환해도 무방 */

	/* [한국어] 아래 OR 로 연결된 세 조건 중 하나라도 참이면 idling 이 서비스 보장에 필요하다:
	 * (1) bfqq->wr_coeff > 1 (weight-raised, 즉 interactive/soft-rt 로 가중치가 상향된 큐)이면서
	 *     - bfqd->wr_busy_queues < tot_busy_queues: weight-raised 가 아닌 "일반" 큐가 하나라도
	 *       섞여 활성 상태라면(비대칭), 그 큐들이 idling 없이 SQ 를 채우면 bfqq 의 몫을 빼앗아갈 수 있음.
	 *     - 또는 bfqd->tot_rq_in_driver >= bfqq->dispatched + 4: 드라이브에 이미 내려간 전체
	 *       in-flight 요청 수가 bfqq 자신이 낸 것보다 4개 이상 많다 - 타 큐의 요청이 드라이브
	 *       내부 큐를 상당히 채우고 있어 재정렬로 인한 지연 위험이 크다는 뜻.
	 * (2) bfq_asymmetric_scenario(bfqd, bfqq): 위 원본 주석이 설명하는 일반적 비대칭 판정
	 *     (서로 다른 가중치의 큐가 섞여 있거나, 활성 group 이 존재하는 경우 등).
	 * (3) tot_busy_queues == 1: 활성 큐가 bfqq 자신 하나뿐인 특수한 "대칭" 케이스도, bfqq 를
	 *     만료시켰다가 그 사이 다른 낮은 가중치의 큐가 끼어들면 향후 지연이 커질 수 있어
	 *     예방적으로 비대칭처럼 취급한다(위 원본 주석 참고). */
	return (bfqq->wr_coeff > 1 &&
		(bfqd->wr_busy_queues < tot_busy_queues ||
		 bfqd->tot_rq_in_driver >= bfqq->dispatched + 4)) ||
		bfq_asymmetric_scenario(bfqd, bfqq) ||
		tot_busy_queues == 1;
		/* [한국어] 위 세 조건 중 하나라도 참이면 true - bfq_better_to_idle 에서
		 * idling_boosts_thr_without_issues 가 false 여도 이 값 때문에 idling 이 결정될 수 있다 */
}

/*
 * [한국어]
 * __bfq_bfqq_expire - bfqq 를 실제로 busy 트리에서 제거(또는 재예약)하고
 * in-service 엔티티 상태를 리셋하는, budget expiration 의 "후반부" 처리.
 *
 * @bfqd: 디스크 전체 상태를 담는 BFQ 스케줄러 데이터.
 * @bfqq: 만료 처리 중인 in-service 큐.
 * @reason: bfq_bfqq_expire 가 전달한 만료 사유(enum bfqq_expiration) - 이 함수
 *          안에서는 BFQQE_PREEMPTED 인지 여부만 특별 취급한다.
 * @return: true 면 이 함수 호출 도중 bfqq 가 실제로 해제(free)되었다는 뜻이라
 *          호출자(bfq_bfqq_expire)는 bfqq 에 더 이상 접근하면 안 된다.
 *          false 면 bfqq 가 여전히 유효한 포인터로 남아있다는 뜻.
 *
 * bfq_bfqq_expire 가 budget 재계산(__bfq_bfqq_recalc_budget)을 마친 뒤 호출하는
 * 후속 단계로, B-WF2Q+ 스케줄링 트리에서 bfqq 를 실제로 어떻게 빼거나
 * 다시 넣을지를 결정한다. RB_EMPTY_ROOT(&bfqq->sort_list) 로 판단하듯
 * 대기 중인 request 가 전혀 없으면 보통 busy 트리에서 완전히 제거하지만,
 * 강제 선점(preemption)으로 인한 만료이면서 idling_needed_for_service_guarantees
 * 가 참인 특수한 경우엔, bfqq 가 비어 있어도 다시 큐에 넣어(requeue) 다음
 * 스케줄링 라운드에서 다른 큐들보다 먼저 서비스받을 자격을 유지시켜 준다
 * (그렇지 않으면 finish-time 이 더 낮은 다른 큐들이 bfqq 보다 먼저 서비스되어
 * bfqq 의 대역폭 보장이 깨질 수 있음).
 *
 * 실행 컨텍스트: bfqd->lock 을 쥔 채 스케줄링 결정 경로에서 호출된다.
 * 이 함수 도중 bfqq 의 참조 카운트가 0 이 되면 bfqq 메모리가 해제될 수
 * 있으므로, 호출 직후 반환값을 반드시 확인해야 한다.
 *
 * 호출 체인:
 *   bfq_bfqq_expire → [__bfq_bfqq_expire] → bfq_del_bfqq_busy / bfq_requeue_bfqq → __bfq_bfqd_reset_in_service
 */
static bool __bfq_bfqq_expire(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			      enum bfqq_expiration reason)
{
	/*
	 * If this bfqq is shared between multiple processes, check
	 * to make sure that those processes are still issuing I/Os
	 * within the mean seek distance. If not, it may be time to
	 * break the queues apart again.
	 */
	/* [한국어] bfqq 가 cooperator 병합(coop merge)으로 여러 프로세스가 공유 중이고,
	 * 현재 seek 패턴이 seeky(원거리 탐색)로 판정되면 - 병합의 이점(순차성 공유)이
	 * 사라졌다는 뜻이므로, split 후보로 표시해 둔다(실제 분리는 다른 경로에서 수행). */
	if (bfq_bfqq_coop(bfqq) && BFQQ_SEEKY(bfqq))
		bfq_mark_bfqq_split_coop(bfqq);

	/*
	 * Consider queues with a higher finish virtual time than
	 * bfqq. If idling_needed_for_service_guarantees(bfqq) returns
	 * true, then bfqq's bandwidth would be violated if an
	 * uncontrolled amount of I/O from these queues were
	 * dispatched while bfqq is waiting for its new I/O to
	 * arrive. This is exactly what may happen if this is a forced
	 * expiration caused by a preemption attempt, and if bfqq is
	 * not re-scheduled. To prevent this from happening, re-queue
	 * bfqq if it needs I/O-dispatch plugging, even if it is
	 * empty. By doing so, bfqq is granted to be served before the
	 * above queues (provided that bfqq is of course eligible).
	 */
	/* [한국어] 조건: bfqq 에 대기 중인 request 가 없고(RB_EMPTY_ROOT), 그리고
	 * "선점으로 인한 만료이면서 서비스 보장을 위해 idling 이 필요한" 특수 케이스가
	 * *아닐* 때만 - 즉, 정말로 더 서비스할 것이 없어서 완전히 빼도 되는 "일반적인" 경우다. */
	if (RB_EMPTY_ROOT(&bfqq->sort_list) &&
	    !(reason == BFQQE_PREEMPTED &&
	      idling_needed_for_service_guarantees(bfqd, bfqq))) {
		/* [한국어] 아직 드라이브에 내려간(dispatched) request 가 하나도 없다면 -
		 * 즉 이 bfqq 가 in-flight 요청조차 없는 완전한 유휴 상태라면. */
		if (bfqq->dispatched == 0)
			/*
			 * Overloading budget_timeout field to store
			 * the time at which the queue remains with no
			 * backlog and no outstanding request; used by
			 * the weight-raising mechanism.
			 */
			/* [한국어] budget_timeout 필드를 원래 용도(예산 타임아웃 시각)가 아니라
			 * "이 큐가 backlog/미완료 요청 없이 유휴 상태가 된 시각"을 기록하는 용도로
			 * 재사용(overload)한다 - 이후 weight-raising 갱신 로직이 이 값을 읽어
			 * 큐가 얼마나 오래 쉬었는지 판단하는 데 사용한다. */
			bfqq->budget_timeout = jiffies;

		/* [한국어] busy 트리(active 스케줄링 구조)에서 bfqq 를 완전히 제거한다.
		 * 두 번째 인자 true 는 "만료로 인한 제거"임을 알려 관련 통계/타임스탬프 처리를
		 * 그에 맞게 수행하도록 한다. */
		bfq_del_bfqq_busy(bfqq, true);
	} else {
		/* [한국어] else 분기: bfqq 가 여전히 request 를 갖고 있거나(정상적으로 서비스
		 * 계속), 혹은 방금 위에서 설명한 "선점 + 서비스 보장 필요" 특수 케이스에 해당 -
		 * 두 경우 모두 bfqq 를 완전히 빼지 않고 스케줄링 트리에 다시 넣어야 한다. */
		/* [한국어] bfqq 를 B-WF2Q+ 트리에 다시 삽입(requeue)한다. true 인자는 이 큐가
		 * (전혀 새로 시작하는 것이 아니라) 만료 후 재진입임을 알려, 필요 시 timestamp
		 * back-shifting 등 기존 타임스탬프를 보존하는 로직이 적용되게 한다. */
		bfq_requeue_bfqq(bfqd, bfqq, true);
		/*
		 * Resort priority tree of potential close cooperators.
		 * See comments on bfq_pos_tree_add_move() for the unlikely().
		 */
		/* [한국어] 이 device 가 NCQ 를 지원하지 않는(non-queueing) 회전식 디스크이고,
		 * bfqq 에 여전히 대기 중인 request 가 남아 있다면 - 향후 cooperator(근접 seek
		 * 대상) 탐색을 돕기 위해 position-tree(rq 위치 기준 트리) 상의 위치를 갱신한다.
		 * NVMe 같은 nonrot_with_queueing 장치에서는 이 최적화가 의미 없어 건너뛴다. */
		if (unlikely(!bfqd->nonrot_with_queueing &&
			     !RB_EMPTY_ROOT(&bfqq->sort_list)))
			bfq_pos_tree_add_move(bfqd, bfqq);
	}

	/*
	 * All in-service entities must have been properly deactivated
	 * or requeued before executing the next function, which
	 * resets all in-service entities as no more in service. This
	 * may cause bfqq to be freed. If this happens, the next
	 * function returns true.
	 */
	/* [한국어] 위에서 bfqq(및 그 상위 entity 체인)를 적절히 비활성화/재큐잉했으므로,
	 * 이제 "현재 서비스 중"으로 표시된 모든 entity 를 더 이상 in-service 가 아니라고
	 * 리셋한다. 이 과정에서 bfqq 의 참조 카운트가 0 이 되어 실제로 free 될 수도 있어,
	 * 반환값으로 그 사실을 호출자에게 알린다. */
	return __bfq_bfqd_reset_in_service(bfqd);
}

/**
 * __bfq_bfqq_recalc_budget - try to adapt the budget to the @bfqq behavior.
 * @bfqd: device data.
 * @bfqq: queue to update.
 * @reason: reason for expiration.
 *
 * Handle the feedback on @bfqq budget at queue expiration.
 * See the body for detailed comments.
 */
/*
 * [한국어]
 * __bfq_bfqq_recalc_budget - 만료 사유(reason)에 따라 bfqq 의 다음 서비스
 * 슬롯에 부여할 budget(섹터 단위 예산)을 재계산한다.
 *
 * @bfqd: 디스크 전체 상태를 담는 BFQ 스케줄러 데이터.
 * @bfqq: budget 을 갱신할 큐 - 방금 만료(expire)된 in-service 큐.
 * @reason: 만료 사유(enum bfqq_expiration) - TOO_IDLE/BUDGET_TIMEOUT/
 *          BUDGET_EXHAUSTED/NO_MORE_REQUESTS 중 하나로, 각 사유마다
 *          budget 을 늘릴지/줄일지/그대로 둘지의 판단 기준이 다르다.
 * @return: 없음(void) - bfqq->max_budget 과 bfqq->entity.budget 을 직접 갱신.
 *
 * BFQ 는 seeky/지연 유발 프로세스에는 작은 budget 을, 순차적(sequential)이고
 * I/O-bound 인 "착한" 프로세스에는 큰 budget 을 주어 처리량(throughput)을
 * 끌어올리는 피드백 제어를 수행한다. 이 함수가 그 피드백 로직의 핵심이며,
 * bfq_bfqq_expire() 가 만료 처리 초반에 호출한다. NVMe SSD 처럼 병렬성이
 * 높은 장치에서는, 크게 키운 budget 이 한 번에 더 많은 순차 request 를
 * SQ(Submission Queue)에 밀어 넣게 해 컨트롤러의 병렬 처리 능력을 활용하게
 * 한다.
 *
 * 실행 컨텍스트: bfqd->lock 을 쥔 채, bfq_bfqq_expire() 안에서 in-service
 * 큐가 확정적으로 만료되는 시점에 호출된다.
 *
 * 호출 체인:
 *   bfq_bfqq_expire → [__bfq_bfqq_recalc_budget] → bfq_min_budget / bfq_serv_to_charge
 */
static void __bfq_bfqq_recalc_budget(struct bfq_data *bfqd,
				     struct bfq_queue *bfqq,
				     enum bfqq_expiration reason)
{
	/* [한국어] bfqq 의 backlog 가 아직 남아있을 때, 그 다음 request 를 커버하기 위해
	 * 새로 배정할 budget 크기를 계산하는 데 쓸 "다음 request" 포인터. */
	struct request *next_rq;
	/* [한국어] budget: 이번 서비스 슬롯 동안 bfqq 에 부여될 예산(섹터 단위).
	 * min_budget: 장치/스케줄러 설정에 따른 최소 허용 budget - 이보다 작게 깎이지 않는다. */
	int budget, min_budget;

	/* [한국어] bfqd 의 설정(bfq_wr_min_idle_time 등)과 현재 최대 budget으로부터
	 * 계산된 "최소 budget" 값을 가져온다 - 모든 budget 조정의 하한선으로 쓰인다. */
	min_budget = bfq_min_budget(bfqd);

	/* [한국어] wr_coeff == 1 은 weight-raising 이 적용되지 않은 "평범한" 큐라는 뜻 -
	 * 이 경우 지난 서비스 슬롯에서 이미 설정돼 있던 max_budget 을 출발점으로 삼는다. */
	if (bfqq->wr_coeff == 1)
		budget = bfqq->max_budget;
	else /*
	      * Use a constant, low budget for weight-raised queues,
	      * to help achieve a low latency. Keep it slightly higher
	      * than the minimum possible budget, to cause a little
	      * bit fewer expirations.
	      */
		/* [한국어] weight-raised 큐(interactive/soft-rt 로 우선순위가 상향된 큐)는
		 * 낮은 지연시간이 목표이므로, 큰 budget 대신 최소값의 2배라는 작고 일정한
		 * budget 을 사용한다 - 큐를 오래 붙잡지 않아 빠르게 서비스를 순환시킨다. */
		budget = 2 * min_budget;

	/* [한국어] 디버그 트레이스 로그 - 직전 budget 과 남은 budget(budget_left)을 기록,
	 * 실제 스케줄링 로직에는 영향 없음. */
	bfq_log_bfqq(bfqd, bfqq, "recalc_budg: last budg %d, budg left %d",
		bfqq->entity.budget, bfq_bfqq_budget_left(bfqq));
	/* [한국어] 디버그 트레이스 로그 - 방금 계산한 budget 후보값과 min_budget 을 기록. */
	bfq_log_bfqq(bfqd, bfqq, "recalc_budg: last max_budg %d, min budg %d",
		budget, bfq_min_budget(bfqd));
	/* [한국어] 디버그 트레이스 로그 - 동기(sync) 여부와 in-service 큐의 seeky 여부를 기록. */
	bfq_log_bfqq(bfqd, bfqq, "recalc_budg: sync %d, seeky %d",
		bfq_bfqq_sync(bfqq), BFQQ_SEEKY(bfqd->in_service_queue));

	/* [한국어] bfqq 가 동기(sync, 즉 결과를 기다리는 read/O_DIRECT write 류) 큐이고
	 * weight-raising 이 적용되지 않은 경우에만 - 만료 사유별로 세밀하게 budget 을
	 * 조정하는 switch 문으로 진입한다 (비동기 큐나 weight-raised 큐는 아래 다른
	 * 분기에서 별도로 처리). */
	if (bfq_bfqq_sync(bfqq) && bfqq->wr_coeff == 1) {
		switch (reason) {
		/*
		 * Caveat: in all the following cases we trade latency
		 * for throughput.
		 */
		case BFQQE_TOO_IDLE: // idle 타이머가 만료되도록 다음 요청이 오지 않아 만료된 경우 - budget을 "줄일" 수 있는 유일한 사유다
			/*
			 * This is the only case where we may reduce
			 * the budget: if there is no request of the
			 * process still waiting for completion, then
			 * we assume (tentatively) that the timer has
			 * expired because the batch of requests of
			 * the process could have been served with a
			 * smaller budget.  Hence, betting that
			 * process will behave in the same way when it
			 * becomes backlogged again, we reduce its
			 * next budget.  As long as we guess right,
			 * this budget cut reduces the latency
			 * experienced by the process.
			 *
			 * However, if there are still outstanding
			 * requests, then the process may have not yet
			 * issued its next request just because it is
			 * still waiting for the completion of some of
			 * the still outstanding ones.  So in this
			 * subcase we do not reduce its budget, on the
			 * contrary we increase it to possibly boost
			 * the throughput, as discussed in the
			 * comments to the BUDGET_TIMEOUT case.
			 */
			/* [한국어] 아직 드라이브에서 완료되지 않은 request(dispatched > 0)가
			 * 남아 있다면 - "요청이 없어서 쉰 것"이 아니라 "완료를 기다리느라
			 * 새 요청을 못 낸 것"일 수 있으므로, budget 을 줄이지 않고 오히려
			 * 2배로 늘려 처리량을 높일 기회를 준다(BUDGET_TIMEOUT 사례와 동일 논리). */
			if (bfqq->dispatched > 0) /* still outstanding reqs */
				budget = min(budget * 2, bfqd->bfq_max_budget);
			else {
				/* [한국어] 진짜로 idle 타임아웃(TOO_IDLE)이면서 in-flight 요청도
				 * 없는 경우 - 이 큐가 작은 budget 으로도 충분했다는 뜻이므로
				 * budget 을 깎아 다음번엔 더 빨리 만료되게 하여 지연시간을 줄인다. */
				if (budget > 5 * min_budget)
					/* [한국어] 여유가 충분하면(5*min 초과) 4*min 만큼만 깎아
					 * 급격한 축소를 피한다(완만한 감소). */
					budget -= 4 * min_budget;
				/* [한국어] 여유가 적으면 아예 최소값까지 낮춘다. */
				else
					budget = min_budget;
			}
			break;
		case BFQQE_BUDGET_TIMEOUT: // 예산(섹터)은 남았는데 시간 제한 budget_timeout이 먼저 끝난 경우 - "장치가 느렸다"는 뜻이지 큐가 나쁘다는 뜻이 아니다
			/*
			 * We double the budget here because it gives
			 * the chance to boost the throughput if this
			 * is not a seeky process (and has bumped into
			 * this timeout because of, e.g., ZBR).
			 */
			/* [한국어] budget timeout 으로 만료된 경우 - seeky 가 아니라 ZBR
			 * (Zone Bit Recording, 디스크 바깥쪽/안쪽 트랙의 속도 차이) 같은
			 * 이유로 시간이 걸렸을 수 있으므로, budget 을 2배로 늘려 다음번엔
			 * 시간 안에 더 많은 서비스를 받을 기회를 준다. */
			budget = min(budget * 2, bfqd->bfq_max_budget);
			break;
		case BFQQE_BUDGET_EXHAUSTED: // 배정된 예산을 시간 안에 전부 소진한 경우 - 순차적이고 think time도 짧은 "우량" 큐라는 증거다
			/*
			 * The process still has backlog, and did not
			 * let either the budget timeout or the disk
			 * idling timeout expire. Hence it is not
			 * seeky, has a short thinktime and may be
			 * happy with a higher budget too. So
			 * definitely increase the budget of this good
			 * candidate to boost the disk throughput.
			 */
			/* [한국어] budget 을 다 써버릴 만큼 활발히 I/O 를 내는 "우량 고객"
			 * 큐이므로, budget 을 4배로 대폭 늘려 처리량을 극대화한다 - seeky
			 * 하지도, thinktime 이 길지도 않다는 것이 이미 입증된 셈. */
			budget = min(budget * 4, bfqd->bfq_max_budget);
			break;
		case BFQQE_NO_MORE_REQUESTS: // 예산도 시간도 남았는데 큐가 비어 자연 만료된 경우 - 실제 필요량에 budget을 맞춰야 B-WF2Q+ 타임스탬프가 어긋나지 않는다
			/*
			 * For queues that expire for this reason, it
			 * is particularly important to keep the
			 * budget close to the actual service they
			 * need. Doing so reduces the timestamp
			 * misalignment problem described in the
			 * comments in the body of
			 * __bfq_activate_entity. In fact, suppose
			 * that a queue systematically expires for
			 * BFQQE_NO_MORE_REQUESTS and presents a
			 * new request in time to enjoy timestamp
			 * back-shifting. The larger the budget of the
			 * queue is with respect to the service the
			 * queue actually requests in each service
			 * slot, the more times the queue can be
			 * reactivated with the same virtual finish
			 * time. It follows that, even if this finish
			 * time is pushed to the system virtual time
			 * to reduce the consequent timestamp
			 * misalignment, the queue unjustly enjoys for
			 * many re-activations a lower finish time
			 * than all newly activated queues.
			 *
			 * The service needed by bfqq is measured
			 * quite precisely by bfqq->entity.service.
			 * Since bfqq does not enjoy device idling,
			 * bfqq->entity.service is equal to the number
			 * of sectors that the process associated with
			 * bfqq requested to read/write before waiting
			 * for request completions, or blocking for
			 * other reasons.
			 */
			/* [한국어] 더 이상 request 가 없어 만료된 경우 - budget 을 크게
			 * 잡지 않고 "실제로 소비한 서비스량"(entity.service)에 최대한
			 * 맞춰서(단, min_budget 이상으로) 설정한다. 이렇게 해야 timestamp
			 * misalignment(가상 종료 시각 왜곡)로 인해 이 큐가 부당하게
			 * 낮은 finish-time 을 반복해서 누리는 것을 방지한다. */
			budget = max_t(int, bfqq->entity.service, min_budget);
			break;
		default:
			/* [한국어] BFQQE_PREEMPTED 등 위에서 다루지 않은 사유는 budget 을
			 * 건드리지 않고 그대로 반환 - 강제 선점은 이 큐의 "행동"과 무관하므로
			 * 피드백 대상이 아니다. */
			return;
		}
	} else if (!bfq_bfqq_sync(bfqq)) { // 위 switch는 sync이면서 WR이 아닌 큐 전용 - 여기로 오는 것은 async 큐뿐이다(sync + WR 큐는 두 분기 모두 건너뛴다)
		/*
		 * Async queues get always the maximum possible
		 * budget, as for them we do not care about latency
		 * (in addition, their ability to dispatch is limited
		 * by the charging factor).
		 */
		/* [한국어] 비동기(async, 즉 writeback 류) 큐는 지연시간을 신경 쓸 필요가
		 * 없으므로 - 항상 최대 budget 을 부여해 처리량만 극대화한다(대신 charging
		 * factor 로 별도 제한됨). */
		budget = bfqd->bfq_max_budget;
	}
	/* [한국어] 위 else-if 에도 해당하지 않는 경우(즉, sync 이면서 wr_coeff > 1 인
	 * weight-raised 큐)는 이미 함수 시작부에서 budget = 2 * min_budget 로 설정된
	 * 값을 그대로 사용한다 - 별도 분기가 필요 없음. */

	/* [한국어] 지금까지 계산한 budget 후보값을 bfqq 의 공식 max_budget 필드에 반영. */
	bfqq->max_budget = budget;

	/* [한국어] 통계가 충분히 쌓였고(budgets_assigned 임계치 초과) 사용자가 수동으로
	 * max_budget 을 고정하지 않았다면 - 시스템 전체의 bfq_max_budget 상한을 넘지
	 * 않도록 다시 한번 clamp 한다(자동 튜닝된 상한 적용). */
	if (bfqd->budgets_assigned >= bfq_stats_min_budgets &&
	    !bfqd->bfq_user_max_budget)
		bfqq->max_budget = min(bfqq->max_budget, bfqd->bfq_max_budget);

	/*
	 * If there is still backlog, then assign a new budget, making
	 * sure that it is large enough for the next request.  Since
	 * the finish time of bfqq must be kept in sync with the
	 * budget, be sure to call __bfq_bfqq_expire() *after* this
	 * update.
	 *
	 * If there is no backlog, then no need to update the budget;
	 * it will be updated on the arrival of a new request.
	 */
	/* [한국어] bfqq 에 아직 대기 중인 다음 request 가 있는지 확인 - 있다면 그 request
	 * 를 서비스하기에 충분한 budget 을 즉시 확정해야 한다(엔티티의 finish-time 계산이
	 * budget 값에 의존하므로, 이 갱신은 반드시 __bfq_bfqq_expire() 호출 전에 끝나야 함). */
	next_rq = bfqq->next_rq;
	/* [한국어] 엔티티에 실제로 부여할 budget 은 (a) 방금 정한 max_budget 과
	 * (b) 다음 request 하나를 서비스하는 데 필요한 최소 charge(섹터 환산량)
	 * 중 더 큰 쪽으로 설정한다 - budget 이 다음 request 하나도 못 채울 만큼
	 * 작아지는 것을 방지. */
	if (next_rq)
		bfqq->entity.budget = max_t(unsigned long, bfqq->max_budget,
					    bfq_serv_to_charge(next_rq, bfqq));

	/* [한국어] 디버그 트레이스 로그 - 다음 request 의 섹터 수와 최종 확정된 budget 을 기록. */
	bfq_log_bfqq(bfqd, bfqq, "head sect: %u, new budget %d",
			next_rq ? blk_rq_sectors(next_rq) : 0,
			bfqq->entity.budget);
}

/*
 * Return true if the process associated with bfqq is "slow". The slow
 * flag is used, in addition to the budget timeout, to reduce the
 * amount of service provided to seeky processes, and thus reduce
 * their chances to lower the throughput. More details in the comments
 * on the function bfq_bfqq_expire().
 *
 * An important observation is in order: as discussed in the comments
 * on the function bfq_update_peak_rate(), with devices with internal
 * queues, it is hard if ever possible to know when and for how long
 * an I/O request is processed by the device (apart from the trivial
 * I/O pattern where a new request is dispatched only after the
 * previous one has been completed). This makes it hard to evaluate
 * the real rate at which the I/O requests of each bfq_queue are
 * served.  In fact, for an I/O scheduler like BFQ, serving a
 * bfq_queue means just dispatching its requests during its service
 * slot (i.e., until the budget of the queue is exhausted, or the
 * queue remains idle, or, finally, a timeout fires). But, during the
 * service slot of a bfq_queue, around 100 ms at most, the device may
 * be even still processing requests of bfq_queues served in previous
 * service slots. On the opposite end, the requests of the in-service
 * bfq_queue may be completed after the service slot of the queue
 * finishes.
 *
 * Anyway, unless more sophisticated solutions are used
 * (where possible), the sum of the sizes of the requests dispatched
 * during the service slot of a bfq_queue is probably the only
 * approximation available for the service received by the bfq_queue
 * during its service slot. And this sum is the quantity used in this
 * function to evaluate the I/O speed of a process.
 */
/*
 * [한국어]
 * bfq_bfqq_is_slow - bfqq 에 연관된 프로세스가 "느린(slow)" 프로세스인지
 * 판정한다 - seeky/저속 프로세스에게 부여되는 서비스량을 줄이기 위한 근거.
 *
 * @bfqd: 디스크 전체 상태를 담는 BFQ 스케줄러 데이터 (peak-rate 추정치 보유).
 * @bfqq: 판정 대상 큐 - 방금 만료(또는 idling 타임아웃)된 in-service 큐.
 * @compensate: true 면 idling 으로 소비된 시간을 보정해서 계산(디스크가 실제로
 *              일한 시간이 아니라 idling 시작 시각 기준으로 delta 계산).
 * @delta_ms: [출력] 이번 서비스 슬롯이 실제로 걸린 시간(ms) - 호출자가
 *            bfq_bfqq_charge_time 등에서 시간 기반 과금에 사용.
 * @return: true 면 이 프로세스는 "느리다"고 판정 - budget timeout 과 함께
 *          seeky 프로세스에게 돌아가는 서비스량을 제한하는 근거가 된다.
 *          false 면 충분히 빠른(정상) 프로세스.
 *
 * NVMe 같은 내부 큐잉(NCQ) 장치에서는 어떤 request 가 정확히 언제, 얼마 동안
 * 처리되었는지 알 수 없다(요청이 여러 개 동시에 device 내부에 있을 수 있으므로).
 * 그래서 BFQ 는 in-service 슬롯 동안 "디스패치된 request 크기의 합"을 그
 * 프로세스의 실효 서비스 속도로 근사한다 - 이 함수는 그 근사치를 바탕으로
 * 프로세스가 느린지(=디스크 대역폭을 갉아먹는지) 판정한다.
 *
 * 실행 컨텍스트: bfqd->lock 을 쥔 채, bfq_bfqq_expire() 안에서 호출된다.
 *
 * 호출 체인:
 *   bfq_bfqq_expire → [bfq_bfqq_is_slow] → blk_time_get / blk_queue_rot
 */
static bool bfq_bfqq_is_slow(struct bfq_data *bfqd, struct bfq_queue *bfqq,
				 bool compensate, unsigned long *delta_ms)
{
	/* [한국어] 이번 서비스 슬롯의 경과 시간을 담을 ktime 변수 - compensate 여부에 따라
	 * 기준 시각이 달라진다. */
	ktime_t delta_ktime;
	/* [한국어] delta_ktime 을 마이크로초로 환산한 값 - 아래 두 임계값(1ms, 20ms) 비교에 사용. */
	u32 delta_usecs;
	/* [한국어] 기본값으로 BFQQ_SEEKY(bfqq)(이미 seeky 로 판정된 큐인지)를 사용 - 시간 간격이
	 * 너무 짧아 신뢰할 수 있는 속도 추정이 불가능할 때는 이 seeky 플래그를 그대로 slow 로 채택. */
	bool slow = BFQQ_SEEKY(bfqq); /* if delta too short, use seekyness */

	/* [한국어] 비동기(async) 큐는 애초에 "느림/빠름"을 지연시간 관점에서 따질 필요가 없으므로
	 * (동기 큐만 latency-sensitive 판정 대상) 무조건 false(느리지 않음)로 처리한다. */
	if (!bfq_bfqq_sync(bfqq))
		return false;

	/* [한국어] compensate == true: idling 타임아웃으로 인한 만료라 실제 device 사용 시간이
	 * idling 시작 시각(last_idling_start)까지로 한정된다 - idling 중 흘러간 시간은
	 * 서비스 시간으로 치지 않기 위한 보정. */
	if (compensate)
		delta_ktime = bfqd->last_idling_start;
	/* [한국어] compensate == false: 지금 이 순간까지를 서비스 슬롯의 끝으로 본다. */
	else
		delta_ktime = blk_time_get();
	/* [한국어] 슬롯 시작 시각(last_budget_start)을 빼서 실제 경과 시간을 구한다. */
	delta_ktime = ktime_sub(delta_ktime, bfqd->last_budget_start);
	/* [한국어] ktime 값을 마이크로초 정수로 변환 - 이후 비교/환산에 사용. */
	delta_usecs = ktime_to_us(delta_ktime);

	/* don't use too short time intervals */
	/* [한국어] 경과 시간이 1ms 미만이면 - 측정 오차가 실제 신호보다 커서 신뢰할 수 없으므로
	 * 아래 블록에서 delta_ms 만 안전한 기본값으로 채우고 slow 값은 위에서 정한 seeky 여부를
	 * 그대로 반환한다. */
	if (delta_usecs < 1000) {
		/* [한국어] 회전식이 아닌(non-rotational, 즉 SSD/NVMe) 장치라면 - idling 과 동일한
		 * 최악의 경우 보장을 주기 위해 BFQ_MIN_TT(최소 처리시간 상수)를 delta_ms 로 사용. */
		if (!blk_queue_rot(bfqd->queue))
			 /*
			  * give same worst-case guarantees as idling
			  * for seeky
			  */
			*delta_ms = BFQ_MIN_TT / NSEC_PER_MSEC;
		else /* charge at least one seek */
			/* [한국어] 회전식 디스크라면 - 최소 한 번의 seek 시간(bfq_slice_idle 로 근사)을
			 * 과금해, 짧은 시간에도 seek 비용이 있었던 것으로 보수적으로 계산. */
			*delta_ms = bfq_slice_idle / NSEC_PER_MSEC;

		return slow;
		/* [한국어] 시간 간격이 너무 짧아 신뢰할 수 없으므로, 기존 seeky 플래그 값을
		 * 그대로 "느림" 판정으로 사용해 반환한다. */
	}

	/* [한국어] 정상적으로 측정 가능한 구간이므로, usec 값을 ms 로 환산해 출력 파라미터에 기록. */
	*delta_ms = delta_usecs / USEC_PER_MSEC;

	/*
	 * Use only long (> 20ms) intervals to filter out excessive
	 * spikes in service rate estimation.
	 */
	/* [한국어] 20ms 를 초과하는 "충분히 긴" 구간에서만 실제 slow 판정을 갱신한다 - 짧은
	 * 구간에서는 순간적인 속도 스파이크가 오판을 유발할 수 있어 걸러낸다. */
	if (delta_usecs > 20000) {
		/*
		 * Caveat for rotational devices: processes doing I/O
		 * in the slower disk zones tend to be slow(er) even
		 * if not seeky. In this respect, the estimated peak
		 * rate is likely to be an average over the disk
		 * surface. Accordingly, to not be too harsh with
		 * unlucky processes, a process is deemed slow only if
		 * its rate has been lower than half of the estimated
		 * peak rate.
		 */
		/* [한국어] 이번 슬롯에서 실제로 받은 서비스량(entity.service, 섹터 단위)이
		 * 추정된 최대 처리율(bfq_max_budget)의 절반보다 작을 때만 slow 로 판정 -
		 * 회전식 디스크의 느린 트랙 영역에 있는 "운 나쁜" 프로세스까지 과도하게
		 * 벌주지 않기 위한 여유(50% 마진)를 둔다. */
		slow = bfqq->entity.service < bfqd->bfq_max_budget / 2;
	}

	/* [한국어] 디버그 트레이스 로그 - 최종 slow 판정 결과 기록. */
	bfq_log_bfqq(bfqd, bfqq, "bfq_bfqq_is_slow: slow %d", slow);

	/* [한국어] 최종 판정값 반환 - bfq_bfqq_expire() 는 이 값이 true 이면 시간 기반 과금
	 * (bfq_bfqq_charge_time)을 적용해 seeky/저속 프로세스의 향후 서비스를 제한한다. */
	return slow;
}


/*
 * To be deemed as soft real-time, an application must meet two
 * requirements. First, the application must not require an average
 * bandwidth higher than the approximate bandwidth required to playback or
 * record a compressed high-definition video.
 * The next function is invoked on the completion of the last request of a
 * batch, to compute the next-start time instant, soft_rt_next_start, such
 * that, if the next request of the application does not arrive before
 * soft_rt_next_start, then the above requirement on the bandwidth is met.
 *
 * The second requirement is that the request pattern of the application is
 * isochronous, i.e., that, after issuing a request or a batch of requests,
 * the application stops issuing new requests until all its pending requests
 * have been completed. After that, the application may issue a new batch,
 * and so on.
 * For this reason the next function is invoked to compute
 * soft_rt_next_start only for applications that meet this requirement,
 * whereas soft_rt_next_start is set to infinity for applications that do
 * not.
 *
 * Unfortunately, even a greedy (i.e., I/O-bound) application may
 * happen to meet, occasionally or systematically, both the above
 * bandwidth and isochrony requirements. This may happen at least in
 * the following circumstances. First, if the CPU load is high. The
 * application may stop issuing requests while the CPUs are busy
 * serving other processes, then restart, then stop again for a while,
 * and so on. The other circumstances are related to the storage
 * device: the storage device is highly loaded or reaches a low-enough
 * throughput with the I/O of the application (e.g., because the I/O
 * is random and/or the device is slow). In all these cases, the
 * I/O of the application may be simply slowed down enough to meet
 * the bandwidth and isochrony requirements. To reduce the probability
 * that greedy applications are deemed as soft real-time in these
 * corner cases, a further rule is used in the computation of
 * soft_rt_next_start: the return value of this function is forced to
 * be higher than the maximum between the following two quantities.
 *
 * (a) Current time plus: (1) the maximum time for which the arrival
 *     of a request is waited for when a sync queue becomes idle,
 *     namely bfqd->bfq_slice_idle, and (2) a few extra jiffies. We
 *     postpone for a moment the reason for adding a few extra
 *     jiffies; we get back to it after next item (b).  Lower-bounding
 *     the return value of this function with the current time plus
 *     bfqd->bfq_slice_idle tends to filter out greedy applications,
 *     because the latter issue their next request as soon as possible
 *     after the last one has been completed. In contrast, a soft
 *     real-time application spends some time processing data, after a
 *     batch of its requests has been completed.
 *
 * (b) Current value of bfqq->soft_rt_next_start. As pointed out
 *     above, greedy applications may happen to meet both the
 *     bandwidth and isochrony requirements under heavy CPU or
 *     storage-device load. In more detail, in these scenarios, these
 *     applications happen, only for limited time periods, to do I/O
 *     slowly enough to meet all the requirements described so far,
 *     including the filtering in above item (a). These slow-speed
 *     time intervals are usually interspersed between other time
 *     intervals during which these applications do I/O at a very high
 *     speed. Fortunately, exactly because of the high speed of the
 *     I/O in the high-speed intervals, the values returned by this
 *     function happen to be so high, near the end of any such
 *     high-speed interval, to be likely to fall *after* the end of
 *     the low-speed time interval that follows. These high values are
 *     stored in bfqq->soft_rt_next_start after each invocation of
 *     this function. As a consequence, if the last value of
 *     bfqq->soft_rt_next_start is constantly used to lower-bound the
 *     next value that this function may return, then, from the very
 *     beginning of a low-speed interval, bfqq->soft_rt_next_start is
 *     likely to be constantly kept so high that any I/O request
 *     issued during the low-speed interval is considered as arriving
 *     to soon for the application to be deemed as soft
 *     real-time. Then, in the high-speed interval that follows, the
 *     application will not be deemed as soft real-time, just because
 *     it will do I/O at a high speed. And so on.
 *
 * Getting back to the filtering in item (a), in the following two
 * cases this filtering might be easily passed by a greedy
 * application, if the reference quantity was just
 * bfqd->bfq_slice_idle:
 * 1) HZ is so low that the duration of a jiffy is comparable to or
 *    higher than bfqd->bfq_slice_idle. This happens, e.g., on slow
 *    devices with HZ=100. The time granularity may be so coarse
 *    that the approximation, in jiffies, of bfqd->bfq_slice_idle
 *    is rather lower than the exact value.
 * 2) jiffies, instead of increasing at a constant rate, may stop increasing
 *    for a while, then suddenly 'jump' by several units to recover the lost
 *    increments. This seems to happen, e.g., inside virtual machines.
 * To address this issue, in the filtering in (a) we do not use as a
 * reference time interval just bfqd->bfq_slice_idle, but
 * bfqd->bfq_slice_idle plus a few jiffies. In particular, we add the
 * minimum number of jiffies for which the filter seems to be quite
 * precise also in embedded systems and KVM/QEMU virtual machines.
 */
/*
 * [한국어]
 * bfq_bfqq_softrt_next_start - bfqq 가 soft real-time 판정을 받으려면
 * 다음 request 가 몇 jiffies 이후에 도착해야 하는지(soft_rt_next_start)를
 * 계산한다.
 *
 * @bfqd: 디스크 전체 상태를 담는 BFQ 스케줄러 데이터.
 * @bfqq: 마지막 request 배치(batch)가 방금 완료된 큐 - 등시성(isochronous)
 *        패턴인지 판정 대상.
 * @return: 이 시각(jiffies) 이전에 다음 request 가 도착하면 대역폭 요건을
 *          위반하는 것으로 간주되는 기준 시각. 호출자는 이 값을
 *          bfqq->soft_rt_next_start 에 저장해 두고, 다음 request 도착 시각과
 *          비교해 soft real-time 여부를 판정한다.
 *
 * 함수 위의 긴 원본 주석이 설명하듯, 미디어 플레이어처럼 "주기적으로 적은
 * 양의 I/O를 냈다가 다음 배치 전까지 조용히 있는" 프로세스를 soft real-time
 * 으로 인식해 weight-raising(가중치 상향)을 부여하기 위한 계산이다. greedy한
 * (I/O-bound) 프로세스가 우연히 이 조건을 통과하지 못하도록, 아래 max3()
 * 세 값 중 가장 큰 값을 하한선으로 사용해 필터링을 강화한다.
 *
 * 실행 컨텍스트: bfqd->lock 을 쥔 채, bfq_bfqq_expire() 안에서 bfqq 가
 * 완전히 비어(RB_EMPTY_ROOT) isochronous 패턴이 확인된 시점에 호출된다.
 *
 * 호출 체인:
 *   bfq_bfqq_expire → [bfq_bfqq_softrt_next_start] (하위 호출 없음, 순수 계산)
 */
static unsigned long bfq_bfqq_softrt_next_start(struct bfq_data *bfqd,
						struct bfq_queue *bfqq)
{
	/* [한국어] max3()로 세 후보 시각 중 가장 늦은(가장 큰) 값을 취한다:
	 * (1) bfqq->soft_rt_next_start: 이전 호출에서 계산해 둔 값 - 고속 구간 직후에는
	 *     이 값이 매우 커서, 이어지는 저속 구간의 오판정을 막는 하한선 역할(원본 주석
	 *     item (b) 참고).
	 * (2) bfqq->last_idle_bklogged + HZ * service_from_backlogged / bfq_wr_max_softrt_rate:
	 *     "이 큐가 지금까지 처리한 backlog 서비스량을, 허용된 soft-rt 최대 대역폭으로
	 *     나눈 시간"만큼을 마지막으로 idle-backlogged 상태가 된 시점에 더한 것 - 즉
	 *     "이 정도 서비스를 이 정도 대역폭으로 소비하려면 최소 이 시각까지는 걸려야
	 *     한다"는 대역폭 기준 하한(원본 주석의 핵심 대역폭 요건).
	 * (3) jiffies + nsecs_to_jiffies(bfq_slice_idle) + 4: 현재 시각에 idling 대기시간과
	 *     약간의 여유 jiffies(HZ 가 낮은 환경/가상머신에서의 jiffies 점프 대응, 원본 주석
	 *     하단 설명 참고)를 더한 값 - greedy 프로세스가 "완료 직후 즉시 재요청"하는 패턴을
	 *     걸러내기 위한 최소 대기 시간. */
	return max3(bfqq->soft_rt_next_start,
		    bfqq->last_idle_bklogged +
		    HZ * bfqq->service_from_backlogged /
		    bfqd->bfq_wr_max_softrt_rate,
		    jiffies + nsecs_to_jiffies(bfqq->bfqd->bfq_slice_idle) + 4);
}

/**
 * bfq_bfqq_expire - expire a queue.
 * @bfqd: device owning the queue.
 * @bfqq: the queue to expire.
 * @compensate: if true, compensate for the time spent idling.
 * @reason: the reason causing the expiration.
 *
 * If the process associated with bfqq does slow I/O (e.g., because it
 * issues random requests), we charge bfqq with the time it has been
 * in service instead of the service it has received (see
 * bfq_bfqq_charge_time for details on how this goal is achieved). As
 * a consequence, bfqq will typically get higher timestamps upon
 * reactivation, and hence it will be rescheduled as if it had
 * received more service than what it has actually received. In the
 * end, bfqq receives less service in proportion to how slowly its
 * associated process consumes its budgets (and hence how seriously it
 * tends to lower the throughput). In addition, this time-charging
 * strategy guarantees time fairness among slow processes. In
 * contrast, if the process associated with bfqq is not slow, we
 * charge bfqq exactly with the service it has received.
 *
 * Charging time to the first type of queues and the exact service to
 * the other has the effect of using the WF2Q+ policy to schedule the
 * former on a timeslice basis, without violating service domain
 * guarantees among the latter.
 */
/*
 * [한국어]
 * bfq_bfqq_expire - in-service 큐 bfqq 의 이번 서비스 슬롯을 종료(만료)
 * 시키는 최상위 진입점. budget 재계산, 시간 과금, soft-rt 판정, 실제
 * 트리 제거/재큐잉까지 만료 절차 전체를 이 함수 하나가 오케스트레이션한다.
 *
 * @bfqd: 디스크 전체 상태를 담는 BFQ 스케줄러 데이터.
 * @bfqq: 만료시킬 in-service 큐.
 * @compensate: true 면 idling 으로 흘려보낸 시간을 보정해서 slow 판정
 *              (bfq_bfqq_is_slow) 을 수행 - 주로 idle 타임아웃 만료 시 사용.
 * @return: 없음(void) - bfqq/entity 상태를 직접 갱신하고, 필요 시 bfqq 를
 *          해제할 수도 있다(__bfq_bfqq_expire 반환값 참고).
 *
 * BFQ 는 seeky/저속 프로세스는 "받은 서비스량"이 아니라 "서비스에 소비한
 * 시간"으로 과금해(bfq_bfqq_charge_time) 순차 워크로드를 우대하고, 정상
 * 프로세스는 실제 받은 서비스량으로만 과금해 WF2Q+ 의 대역폭 보장을
 * 지킨다. 이 두 가지 과금 방식을 구분해 적용하는 것이 이 함수의 핵심
 * 책임 중 하나이며, 그 외에 soft real-time 판정, weight-raising 시작
 * 시각 기록, 그리고 최종적으로 __bfq_bfqq_recalc_budget/__bfq_bfqq_expire
 * 를 호출해 실제 만료를 완수한다.
 *
 * 실행 컨텍스트: bfqd->lock 을 쥔 채, bfq_select_queue, bfq_completed_request,
 * bfq_dispatch_rq_from_bfqq 등 "다음 큐를 선택해야 하는" 모든 경로에서
 * 공통적으로 호출되는 유일한 만료 진입점이다. NVMe 처럼 컨트롤러에 이미
 * 내려간(dispatched) request 는 취소할 수 없으므로, 이 함수는 앞으로 낼
 * 새 CID(Command ID) 묶음의 스케줄링 우선순위를 재조정하는 역할만 한다.
 *
 * 호출 체인:
 *   bfq_select_queue/bfq_completed_request → [bfq_bfqq_expire] →
 *     bfq_bfqq_is_slow / bfq_bfqq_charge_time / __bfq_bfqq_recalc_budget / __bfq_bfqq_expire
 */
void bfq_bfqq_expire(struct bfq_data *bfqd,
		     struct bfq_queue *bfqq,
		     bool compensate,
		     enum bfqq_expiration reason)
{
	/* [한국어] bfq_bfqq_is_slow() 판정 결과를 담을 변수 - true 면 시간 기반 과금 후보. */
	bool slow;
	/* [한국어] bfq_bfqq_is_slow() 가 채워주는 "이번 슬롯의 실제 경과 시간(ms)" - 시간
	 * 기반 과금(bfq_bfqq_charge_time) 시 사용. 함수 실패/조기 반환 대비 0으로 초기화. */
	unsigned long delta = 0;
	/* [한국어] bfqq 자신의 스케줄링 엔티티에서 시작해, 아래에서 부모 방향으로 순회하며
	 * service 카운터를 리셋하는 데 재사용되는 포인터. */
	struct bfq_entity *entity = &bfqq->entity;

	/*
	 * Check whether the process is slow (see bfq_bfqq_is_slow).
	 */
	/* [한국어] 이 프로세스가 seeky/저속인지 판정하고, 동시에 이번 슬롯의 경과 시간(delta)도
	 * 함께 얻는다 - 아래 과금 여부 결정에 두 값 모두 사용. */
	slow = bfq_bfqq_is_slow(bfqd, bfqq, compensate, &delta);

	/*
	 * As above explained, charge slow (typically seeky) and
	 * timed-out queues with the time and not the service
	 * received, to favor sequential workloads.
	 *
	 * Processes doing I/O in the slower disk zones will tend to
	 * be slow(er) even if not seeky. Therefore, since the
	 * estimated peak rate is actually an average over the disk
	 * surface, these processes may timeout just for bad luck. To
	 * avoid punishing them, do not charge time to processes that
	 * succeeded in consuming at least 2/3 of their budget. This
	 * allows BFQ to preserve enough elasticity to still perform
	 * bandwidth, and not time, distribution with little unlucky
	 * or quasi-sequential processes.
	 */
	/* [한국어] 시간 기반 과금 조건: weight-raised 가 아니면서(wr_coeff==1, 이미 우대받는
	 * 큐는 추가로 벌줄 필요 없음), 그리고 (slow 판정을 받았거나) 또는 (budget timeout
	 * 으로 만료됐지만 budget 을 2/3 이상이나 남긴 채였다면 - "운 나쁘게" 느린 존(zone)
	 * 을 사용했을 가능성이 있어 시간 과금 대상). 이 조건이면 서비스량이 아니라 소비
	 * 시간으로 과금해 seeky/저속 프로세스가 향후 받을 서비스를 줄인다. */
	if (bfqq->wr_coeff == 1 &&
	    (slow ||
	     (reason == BFQQE_BUDGET_TIMEOUT &&
	      bfq_bfqq_budget_left(bfqq) >=  entity->budget / 3)))
		bfq_bfqq_charge_time(bfqd, bfqq, delta);

	/* [한국어] low_latency(interactive 등 지연시간 최적화) 모드이고 아직 weight-raising
	 * 이 적용되지 않은 큐라면 - 지금 이 시점을 "마지막 weight-raising 시작/종료 기준
	 * 시각"으로 기록해 둔다. 이후 새 request 도착 시 이 값 기준으로 wr(가중치 상향)을
	 * 새로 부여할지 판단하는 데 쓰인다. */
	if (bfqd->low_latency && bfqq->wr_coeff == 1)
		bfqq->last_wr_start_finish = jiffies;

	/* [한국어] low_latency 모드이고 soft-rt 최대 허용 대역폭(bfq_wr_max_softrt_rate)이
	 * 설정되어 있으며, bfqq 에 대기 중인 request 가 전혀 없다면(RB_EMPTY_ROOT) -
	 * soft real-time 판정을 시도할 수 있는 조건이 갖춰진 것. */
	if (bfqd->low_latency && bfqd->bfq_wr_max_softrt_rate > 0 &&
	    RB_EMPTY_ROOT(&bfqq->sort_list)) {
		/*
		 * If we get here, and there are no outstanding
		 * requests, then the request pattern is isochronous
		 * (see the comments on the function
		 * bfq_bfqq_softrt_next_start()). Therefore we can
		 * compute soft_rt_next_start.
		 *
		 * If, instead, the queue still has outstanding
		 * requests, then we have to wait for the completion
		 * of all the outstanding requests to discover whether
		 * the request pattern is actually isochronous.
		 */
		/* [한국어] in-flight(dispatched) 요청도 0개라면 - 정말로 "요청을 다 내고 조용히
		 * 있는" 등시성(isochronous) 패턴이 확정된 것이므로, 지금 바로 다음 soft-rt
		 * 기준 시각을 계산해 둔다. */
		if (bfqq->dispatched == 0)
			bfqq->soft_rt_next_start =
				bfq_bfqq_softrt_next_start(bfqd, bfqq);
		else if (bfqq->dispatched > 0) { // 아직 완료를 기다리는 in-flight 요청이 남아 있어, 지금 판정하면 "요청이 끊긴 것"과 "완료 대기 중인 것"을 구분할 수 없다
			/*
			 * Schedule an update of soft_rt_next_start to when
			 * the task may be discovered to be isochronous.
			 */
			/* [한국어] 아직 완료를 기다리는 in-flight 요청이 있다면 - 지금은 진짜
			 * isochronous 인지 판단할 수 없으므로(요청 완료를 기다리는 중일 수도
			 * 있음), softrt_update 플래그만 표시해 두고 나중에(모든 요청 완료 시)
			 * soft_rt_next_start 재계산을 예약한다. */
			bfq_mark_bfqq_softrt_update(bfqq);
		}
	}

	/* [한국어] 디버그 트레이스 로그 - 만료 사유, slow 판정, in-flight 개수, short-thinktime
	 * 여부를 한 번에 기록해 두어 사후 분석 시 만료 패턴을 추적할 수 있게 한다. */
	bfq_log_bfqq(bfqd, bfqq,
		"expire (%d, slow %d, num_disp %d, short_ttime %d)", reason,
		slow, bfqq->dispatched, bfq_bfqq_has_short_ttime(bfqq));

	/*
	 * bfqq expired, so no total service time needs to be computed
	 * any longer: reset state machine for measuring total service
	 * times.
	 */
	/* [한국어] bfqq 가 만료되었으므로, injection(다른 큐의 request 를 끼워넣어 service
	 * hole 을 메우는 기법)의 총 서비스 시간 측정용 상태 머신을 리셋한다 -
	 * rqs_injected(이번 슬롯에 injection 이 있었는지), wait_dispatch(디스패치 대기 중인지)
	 * 를 모두 false 로. */
	bfqd->rqs_injected = bfqd->wait_dispatch = false;
	/* [한국어] 측정 대상으로 추적하던 "대기 중인 request" 포인터도 더 이상 유효하지 않으므로 해제. */
	bfqd->waited_rq = NULL;

	/*
	 * Increase, decrease or leave budget unchanged according to
	 * reason.
	 */
	/* [한국어] 만료 사유에 따라 다음 서비스 슬롯의 budget 을 늘리거나 줄이거나 그대로
	 * 두는 피드백 계산(§ __bfq_bfqq_recalc_budget 참고)을 수행. */
	__bfq_bfqq_recalc_budget(bfqd, bfqq, reason);
	/* [한국어] 실제로 bfqq 를 busy 트리에서 빼거나 재큐잉하고, in-service 상태를 리셋하는
	 * 후반부 처리(§ __bfq_bfqq_expire 참고)를 수행한다. 이 과정에서 bfqq 가 해제될 수 있음. */
	if (__bfq_bfqq_expire(bfqd, bfqq, reason))
		/* bfqq is gone, no more actions on it */
		/* [한국어] bfqq 메모리가 이미 해제되었으므로, 이 포인터에 더 이상 접근하면 안 된다 -
		 * 즉시 함수를 종료. */
		return;

	/* mark bfqq as waiting a request only if a bic still points to it */
	/* [한국어] bfqq 가 (해제되지 않고) 여전히 존재하지만 busy 트리에서는 빠진 상태이고,
	 * budget timeout/exhausted 로 인한 "정상 소진" 만료가 아니라면(즉 request 가 없어서
	 * 등 다른 사유) - 곧 새 request 가 도착할 수 있는 경우이므로 "non-blocking wait" 로
	 * 표시해 다음 request 도착 시 빠르게 재활성화되도록 한다. */
	if (!bfq_bfqq_busy(bfqq) &&
	    reason != BFQQE_BUDGET_TIMEOUT &&
	    reason != BFQQE_BUDGET_EXHAUSTED) {
		bfq_mark_bfqq_non_blocking_wait_rq(bfqq);
		/*
		 * Not setting service to 0, because, if the next rq
		 * arrives in time, the queue will go on receiving
		 * service with this same budget (as if it never expired)
		 */
		/* [한국어] 이 분기에서는 entity->service 를 일부러 0으로 리셋하지 않는다 - 다음
		 * request 가 제때 도착하면 마치 만료된 적 없었던 것처럼 같은 budget 으로 서비스를
		 * 이어가야 하기 때문(연속성 보존). */
	} else
		/* [한국어] budget timeout/exhausted 로 "정상적으로" 만료됐거나 여전히 busy 라면 -
		 * 이번 슬롯에서 소비한 서비스량을 0으로 리셋해 다음 슬롯을 깨끗하게 시작. */
		entity->service = 0;

	/*
	 * Reset the received-service counter for every parent entity.
	 * Differently from what happens with bfqq->entity.service,
	 * the resetting of this counter never needs to be postponed
	 * for parent entities. In fact, in case bfqq may have a
	 * chance to go on being served using the last, partially
	 * consumed budget, bfqq->entity.service needs to be kept,
	 * because if bfqq then actually goes on being served using
	 * the same budget, the last value of bfqq->entity.service is
	 * needed to properly decrement bfqq->entity.budget by the
	 * portion already consumed. In contrast, it is not necessary
	 * to keep entity->service for parent entities too, because
	 * the bubble up of the new value of bfqq->entity.budget will
	 * make sure that the budgets of parent entities are correct,
	 * even in case bfqq and thus parent entities go on receiving
	 * service with the same budget.
	 */
	/* [한국어] bfqq 자신의 entity 는 위에서 조건부로 처리했으니, 이제 부모 계층
	 * (cgroup 트리 상의 상위 스케줄링 엔티티)으로 이동한다 - bfqq->entity.service 와
	 * 달리 부모 엔티티의 service 카운터는 리셋을 미룰 이유가 없다(위 원본 주석 설명). */
	entity = entity->parent;
	/* [한국어] for_each_entity 매크로로 entity 부터 루트(cgroup 트리 최상위)까지 부모
	 * 방향으로 순회하며, 각 계층의 받은-서비스 카운터를 0으로 리셋한다 - bfqq->entity.budget
	 * 의 새 값이 상위로 전파(bubble up)되면서 부모 budget 들도 자동으로 올바르게 맞춰지므로
	 * 부모의 service 값 자체는 보존할 필요가 없다. */
	for_each_entity(entity)
		entity->service = 0;
}

/*
 * Budget timeout is not implemented through a dedicated timer, but
 * just checked on request arrivals and completions, as well as on
 * idle timer expirations.
 */
/*
 * [한국어]
 * bfq_bfqq_budget_timeout - bfqq 에 부여된 budget 시간 예산이 만료되었는지
 * 검사한다 (전용 타이머 없이, 여러 이벤트 시점에 폴링 방식으로 확인).
 *
 * @bfqq: 검사할 큐.
 * @return: true 면 budget_timeout 시각이 이미 지났다는 뜻(시간 초과).
 *
 * budget timeout 은 별도의 커널 타이머(hrtimer 등)로 구현되지 않고, request
 * 도착/완료 시점과 idle 타이머 만료 시점에 이 함수를 호출해 "이미 지났는지"만
 * 확인하는 지연(lazy) 방식으로 구현된다 - 타이머 인터럽트 오버헤드를 피하기
 * 위한 설계.
 *
 * 실행 컨텍스트: bfqd->lock 을 쥔 여러 스케줄링 경로에서 호출.
 *
 * 호출 체인:
 *   bfq_may_expire_for_budg_timeout → [bfq_bfqq_budget_timeout] → time_is_before_eq_jiffies
 */
static bool bfq_bfqq_budget_timeout(struct bfq_queue *bfqq)
{
	/* [한국어] bfqq->budget_timeout(만료 예정 jiffies 값)이 현재 jiffies 보다 이전이거나
	 * 같으면 true - 즉 이 큐에 할당된 시간 예산이 이미 다 지났다는 뜻. */
	return time_is_before_eq_jiffies(bfqq->budget_timeout);
}

/*
 * If we expire a queue that is actively waiting (i.e., with the
 * device idled) for the arrival of a new request, then we may incur
 * the timestamp misalignment problem described in the body of the
 * function __bfq_activate_entity. Hence we return true only if this
 * condition does not hold, or if the queue is slow enough to deserve
 * only to be kicked off for preserving a high throughput.
 */
/*
 * [한국어]
 * bfq_may_expire_for_budg_timeout - "budget timeout" 사유로 bfqq 를 지금
 * 만료시켜도 안전한지(timestamp misalignment 문제를 일으키지 않는지) 판단.
 *
 * @bfqq: 만료 여부를 검토 중인 in-service 큐.
 * @return: true 면 budget timeout 을 사유로 즉시 만료시켜도 된다는 뜻.
 *          false 면 아직 만료시키기엔 이르다(예: device idling 대기 중이며
 *          budget 이 아직 충분히 남아있는 경우) - 이 경우 만료를 보류해야
 *          __bfq_activate_entity 에서 설명하는 타임스탬프 왜곡 문제를 피한다.
 *
 * bfqq 가 device idling(새 request 도착을 기다리며 능동적으로 대기 중)
 * 상태에서 만료되면, 그 큐는 다시 활성화될 때 낮은 finish-time 을 부당하게
 * 유지하려 들 수 있어 타임스탬프 정합성이 깨질 위험이 있다. 그래서 이
 * 함수는 (a) 애초에 idling 대기 중이 아니거나, (b) idling 대기 중이더라도
 * budget 을 이미 2/3 이상 써서(즉 남은 budget 이 1/3 미만) 더 서비스해도
 * 얻을 게 적은 "느린" 큐인 경우에만, 그리고 (c) 실제로 timeout 시각이
 * 지났을 때만 true 를 반환하도록 설계되었다.
 *
 * 실행 컨텍스트: bfqd->lock 을 쥔 채 bfq_select_queue 계열에서 호출.
 *
 * 호출 체인:
 *   bfq_select_queue → [bfq_may_expire_for_budg_timeout] → bfq_bfqq_budget_timeout / bfq_bfqq_wait_request
 */
static bool bfq_may_expire_for_budg_timeout(struct bfq_queue *bfqq)
{
	/* [한국어] 디버그 트레이스 로그 - device idling 대기 여부, budget 여유 조건,
	 * timeout 여부 세 값을 한 번에 기록해 만료 판단 근거를 추적할 수 있게 한다. */
	bfq_log_bfqq(bfqq->bfqd, bfqq,
		"may_budget_timeout: wait_request %d left %d timeout %d",
		bfq_bfqq_wait_request(bfqq),
			bfq_bfqq_budget_left(bfqq) >=  bfqq->entity.budget / 3,
		bfq_bfqq_budget_timeout(bfqq));

	/* [한국어] 최종 판정: (bfqq 가 device idling 대기 중이 아니거나) 또는 (남은 budget 이
	 * 전체 budget 의 1/3 이상 - 아직 많이 남았다는 뜻이라 idling 을 깨도 손해가 적음),
	 * 그리고 반드시 budget timeout 시각 자체는 지나 있어야 한다(AND 로 연결). 두 조건이
	 * 모두 참이어야만 이 사유로 만료를 허용한다. */
	return (!bfq_bfqq_wait_request(bfqq) ||
		bfq_bfqq_budget_left(bfqq) >=  bfqq->entity.budget / 3)
		&&
		bfq_bfqq_budget_timeout(bfqq);
}

/*
 * [한국어]
 * idling_boosts_thr_without_issues - "부작용 없이" device idling 이
 * 순수하게 처리량(throughput)을 높이는 상황인지 판단한다.
 *
 * @bfqd: 디스크 전체 상태를 담는 BFQ 스케줄러 데이터 (장치 특성: 회전식 여부,
 *        NCQ/hw_tag 지원 여부 등을 담고 있음).
 * @bfqq: idling 대상 큐 - 방금 비어(request 소진) idling 여부를 판단 중.
 * @return: true 면 idling 이 처리량 관점에서 이득이고 부작용도 없다는 뜻 -
 *          bfq_better_to_idle 이 이 값을 근거로 idling 을 허용할 수 있다.
 *          false 면 idling 이 처리량을 깎아먹거나(NCQ 플래시 장치의 순차 I/O
 *          케이스) weight-raised 큐에 부작용을 줄 수 있다는 뜻.
 *
 * 회전식 디스크에서는 seek 비용이 커서, idling 으로 다음 request 를 기다려
 * 순차성을 지키는 편이 처리량에 이득이다. 반면 NVMe/NCQ 플래시 장치는
 * 컨트롤러가 여러 request 를 동시에 병렬 처리할 수 있어, idling 으로 SQ 를
 * 비워두면 오히려 컨트롤러의 병렬성을 놀리는 셈이 되어 손해다. 이 함수는
 * 이 두 상반된 상황을 장치 특성과 bfqq 의 I/O 패턴(순차적/IO-bound 여부)으로
 * 구분해 낸다. 추가로, weight-raised 큐가 활성 상태인 특수한 경우엔 NCQ
 * 장치에서 request 풀 기아(starvation) 문제를 피하기 위해 idling 을 강제로
 * 금지(false)한다.
 *
 * 실행 컨텍스트: bfqd->lock 을 쥔 채 bfq_better_to_idle 에서 호출.
 *
 * 호출 체인:
 *   bfq_better_to_idle → [idling_boosts_thr_without_issues] → bfqq_process_refs / BFQQ_SEEKY
 */
static bool idling_boosts_thr_without_issues(struct bfq_data *bfqd,
					     struct bfq_queue *bfqq)
{
	/* [한국어] 세 지역 변수 선언(C 문법상 콤마 연산자로 여러 변수를 한 번에 선언):
	 * rot_without_queueing: 회전식 디스크이면서 NCQ 를 지원하지 않는(!hw_tag) 장치인지 -
	 *   이 경우는 idling 이 "거의 항상" 처리량에 이득이 되는 대표 케이스(원본 주석 (a)).
	 * bfqq_sequential_and_IO_bound: 아래에서 채워질, bfqq 가 순차적이고 IO-bound 인지 플래그.
	 * idling_boosts_thr: 최종적으로 "idling 이 처리량을 높이는가"를 나타낼 결과 플래그. */
	bool rot_without_queueing =
		blk_queue_rot(bfqd->queue) && !bfqd->hw_tag,
		bfqq_sequential_and_IO_bound,
		idling_boosts_thr;

	/* No point in idling for bfqq if it won't get requests any longer */
	/* [한국어] bfqq 를 참조하는 프로세스가 이미 하나도 남지 않았다면 - 앞으로 request 가
	 * 절대 오지 않으므로 idling 은 무의미(처리량에 도움이 될 여지 자체가 없음). */
	if (unlikely(!bfqq_process_refs(bfqq)))
		return false;

	/* [한국어] bfqq 가 (1) seeky 하지 않고(BFQQ_SEEKY false, 즉 근거리/순차 접근),
	 * (2) IO-bound(연속적으로 I/O 를 내는 경향)하며, (3) thinktime 이 짧은(요청 간
	 * 텀이 짧은) 프로세스인지를 한 번에 판정 - 이 세 조건을 모두 만족하면 "순차적이고
	 * 활발한" 이상적인 idling 후보로 본다. */
	bfqq_sequential_and_IO_bound = !BFQQ_SEEKY(bfqq) &&
		bfq_bfqq_IO_bound(bfqq) && bfq_bfqq_has_short_ttime(bfqq);

	/*
	 * The next variable takes into account the cases where idling
	 * boosts the throughput.
	 *
	 * The value of the variable is computed considering, first, that
	 * idling is virtually always beneficial for the throughput if:
	 * (a) the device is not NCQ-capable and rotational, or
	 * (b) regardless of the presence of NCQ, the device is rotational and
	 *     the request pattern for bfqq is I/O-bound and sequential, or
	 * (c) regardless of whether it is rotational, the device is
	 *     not NCQ-capable and the request pattern for bfqq is
	 *     I/O-bound and sequential.
	 *
	 * Secondly, and in contrast to the above item (b), idling an
	 * NCQ-capable flash-based device would not boost the
	 * throughput even with sequential I/O; rather it would lower
	 * the throughput in proportion to how fast the device
	 * is. Accordingly, the next variable is true if any of the
	 * above conditions (a), (b) or (c) is true, and, in
	 * particular, happens to be false if bfqd is an NCQ-capable
	 * flash-based device.
	 */
	/* [한국어] idling_boosts_thr = (a) NCQ 없는 회전식 디스크이거나, 또는 (b)/(c) 회전식
	 * 이거나 NCQ 가 없으면서 순차적/IO-bound 패턴인 경우. 결과적으로 "NCQ 를 지원하는
	 * 플래시 장치(NVMe SSD 등)에서 순차 I/O"인 경우에만 이 값이 false 가 되어, idling 을
	 * 하지 않고 SQ 를 계속 채우는 쪽을 택하게 만든다(원본 주석의 핵심 결론). */
	idling_boosts_thr = rot_without_queueing ||
		((blk_queue_rot(bfqd->queue) || !bfqd->hw_tag) &&
		 bfqq_sequential_and_IO_bound);

	/*
	 * The return value of this function is equal to that of
	 * idling_boosts_thr, unless a special case holds. In this
	 * special case, described below, idling may cause problems to
	 * weight-raised queues.
	 *
	 * When the request pool is saturated (e.g., in the presence
	 * of write hogs), if the processes associated with
	 * non-weight-raised queues ask for requests at a lower rate,
	 * then processes associated with weight-raised queues have a
	 * higher probability to get a request from the pool
	 * immediately (or at least soon) when they need one. Thus
	 * they have a higher probability to actually get a fraction
	 * of the device throughput proportional to their high
	 * weight. This is especially true with NCQ-capable drives,
	 * which enqueue several requests in advance, and further
	 * reorder internally-queued requests.
	 *
	 * For this reason, we force to false the return value if
	 * there are weight-raised busy queues. In this case, and if
	 * bfqq is not weight-raised, this guarantees that the device
	 * is not idled for bfqq (if, instead, bfqq is weight-raised,
	 * then idling will be guaranteed by another variable, see
	 * below). Combined with the timestamping rules of BFQ (see
	 * [1] for details), this behavior causes bfqq, and hence any
	 * sync non-weight-raised queue, to get a lower number of
	 * requests served, and thus to ask for a lower number of
	 * requests from the request pool, before the busy
	 * weight-raised queues get served again. This often mitigates
	 * starvation problems in the presence of heavy write
	 * workloads and NCQ, thereby guaranteeing a higher
	 * application and system responsiveness in these hostile
	 * scenarios.
	 */
	/* [한국어] 최종 반환값: idling_boosts_thr 이 참이더라도, 현재 활성 weight-raised
	 * 큐가 하나라도 있으면(wr_busy_queues != 0) 무조건 false 로 강제한다 - write hog
	 * 등으로 request 풀이 포화된 상황에서, non-weight-raised 큐(bfqq)까지 idling 하면
	 * weight-raised 큐들이 request 풀을 독점해 기아를 유발할 위험이 있기 때문. bfqq 를
	 * idling 하지 않고 빨리 돌려보내야 bfqq 가 다음 request 를 더 자주/빨리 요청하게
	 * 되어 상대적으로 공정한 풀 점유가 유지된다. */
	return idling_boosts_thr &&
		bfqd->wr_busy_queues == 0;
}

/*
 * For a queue that becomes empty, device idling is allowed only if
 * this function returns true for that queue. As a consequence, since
 * device idling plays a critical role for both throughput boosting
 * and service guarantees, the return value of this function plays a
 * critical role as well.
 *
 * In a nutshell, this function returns true only if idling is
 * beneficial for throughput or, even if detrimental for throughput,
 * idling is however necessary to preserve service guarantees (low
 * latency, desired throughput distribution, ...). In particular, on
 * NCQ-capable devices, this function tries to return false, so as to
 * help keep the drives' internal queues full, whenever this helps the
 * device boost the throughput without causing any service-guarantee
 * issue.
 *
 * Most of the issues taken into account to get the return value of
 * this function are not trivial. We discuss these issues in the two
 * functions providing the main pieces of information needed by this
 * function.
 */
/*
 * [한국어]
 * bfq_better_to_idle - bfqq 가 비어있게 된 시점에, device idling(디스패치
 * 중단하고 새 request 를 잠시 기다리는 것)이 만료보다 더 나은 선택인지
 * 최종 판단하는 상위 진입점.
 *
 * @bfqq: 방금 request 가 소진된 in-service 큐.
 * @return: true 면 idling 이 낫다는 뜻 - 호출자는 bfqq 를 만료시키지 않고
 *          device 를 idle 상태로 두어 새 request 도착을 기다려야 한다.
 *          false 면 idling 할 이유가 없다는 뜻 - 즉시 만료시키고 다음
 *          B-WF2Q+ 큐로 전환해도 무방(오히려 처리량에 유리).
 *
 * idling_boosts_thr_without_issues(처리량 관점)와
 * idling_needed_for_service_guarantees(공정성/서비스 보장 관점)라는 두
 * 개의 하위 판단을 OR 로 결합한다 - 이 함수는 이 두 조각을 조립하는 최종
 * 의사결정자다. NCQ 를 지원하는 NVMe SSD(bfqd->nonrot_with_queueing == true)
 * 에서는 보통 idling 을 피해 SQ 를 계속 채워 컨트롤러 병렬성을 활용하려
 * 하지만, weight-raised 큐나 비대칭 시나리오에서 sync 큐의 지연시간/대역폭
 * 보장이 걸려 있을 때는 예외적으로 idling 을 허용한다.
 *
 * 실행 컨텍스트: bfqd->lock 을 쥔 채 bfq_select_queue/bfq_completed_request
 * 에서 in-service 큐가 비었을 때 호출된다.
 *
 * 호출 체인:
 *   bfq_select_queue/bfq_completed_request → [bfq_better_to_idle] →
 *     idling_boosts_thr_without_issues / idling_needed_for_service_guarantees
 */
static bool bfq_better_to_idle(struct bfq_queue *bfqq)
{
	/* [한국어] bfqq 가 속한 디스크 전체 상태 - 이하 여러 판단에서 장치 특성/글로벌
	 * 카운터에 접근하기 위해 캐싱. */
	struct bfq_data *bfqd = bfqq->bfqd;
	/* [한국어] idling_boosts_thr_with_no_issue: "부작용 없이 처리량을 높이는가" 판정 결과.
	 * idling_needed_for_service_guar: "공정성/대역폭 보장을 위해 반드시 필요한가" 판정 결과.
	 * 이 두 값의 OR 이 최종 반환값이 된다. */
	bool idling_boosts_thr_with_no_issue, idling_needed_for_service_guar;

	/* No point in idling for bfqq if it won't get requests any longer */
	/* [한국어] bfqq 를 참조하는 프로세스가 이미 하나도 남지 않았다면 - 기다려도 새
	 * request 가 오지 않으므로 idling 은 무의미, 즉시 만료 처리하도록 false 반환. */
	if (unlikely(!bfqq_process_refs(bfqq)))
		return false;

	/* [한국어] strict_guarantees(엄격한 서비스 보장 모드, 사용자가 명시적으로 설정하는
	 * 튜닝 옵션)가 켜져 있다면 - 처리량 손해를 감수하고서라도 항상 idling 을 강제한다
	 * (이 옵션 자체가 "무조건 지연시간/공정성 우선"이라는 관리자 의도이므로 다른 판단을
	 * 건너뛰고 즉시 true). */
	if (unlikely(bfqd->strict_guarantees))
		return true;

	/*
	 * Idling is performed only if slice_idle > 0. In addition, we
	 * do not idle if
	 * (a) bfqq is async
	 * (b) bfqq is in the idle io prio class: in this case we do
	 * not idle because we want to minimize the bandwidth that
	 * queues in this class can steal to higher-priority queues
	 */
	/* [한국어] 세 가지 "idling 자체가 불가능/불필요한" 조건 중 하나라도 해당하면 즉시
	 * false: (1) bfq_slice_idle == 0(관리자가 idling 자체를 0으로 꺼둔 설정),
	 * (2) bfqq 가 비동기(async, 결과를 기다리지 않는 writeback 류) 큐 - 지연시간을
	 * 신경 쓸 필요가 없어 idling 대상이 아님, (3) IDLE io 우선순위 클래스 - 의도적으로
	 * 낮은 우선순위이므로 idling 으로 상위 우선순위 큐의 대역폭을 빼앗으면 안 됨. */
	if (bfqd->bfq_slice_idle == 0 || !bfq_bfqq_sync(bfqq) ||
	   bfq_class_idle(bfqq))
		return false;

	/* [한국어] "idling 이 부작용 없이 처리량을 높이는가"를 판정 - 장치 특성(회전식/NCQ)과
	 * bfqq 의 I/O 패턴(순차적/IO-bound), 그리고 weight-raised 큐 존재 여부를 종합. */
	idling_boosts_thr_with_no_issue =
		idling_boosts_thr_without_issues(bfqd, bfqq);

	/* [한국어] "idling 이 처리량엔 손해더라도 공정성/대역폭 보장을 위해 반드시
	 * 필요한가"를 판정 - 비대칭 가중치/그룹 구성, in-flight 요청 비중 등을 종합. */
	idling_needed_for_service_guar =
		idling_needed_for_service_guarantees(bfqd, bfqq);

	/*
	 * We have now the two components we need to compute the
	 * return value of the function, which is true only if idling
	 * either boosts the throughput (without issues), or is
	 * necessary to preserve service guarantees.
	 */
	/* [한국어] 두 판정 중 하나라도 참이면 idling 을 선택 - "처리량에 이득"이거나
	 * "공정성 보장에 필수"인 경우 모두 idling 이 만료보다 낫다는 결론. */
	return idling_boosts_thr_with_no_issue ||
		idling_needed_for_service_guar;
}

/*
 * If the in-service queue is empty but the function bfq_better_to_idle
 * returns true, then:
 * 1) the queue must remain in service and cannot be expired, and
 * 2) the device must be idled to wait for the possible arrival of a new
 *    request for the queue.
 * See the comments on the function bfq_better_to_idle for the reasons
 * why performing device idling is the best choice to boost the throughput
 * and preserve service guarantees when bfq_better_to_idle itself
 * returns true.
 */
/*
 * [한국어]
 * bfq_bfqq_must_idle - 지금 이 순간 device 를 반드시 idle 상태로 두어야
 * 하는지(=bfqq 를 만료시키면 안 되는지) 최종 결론을 내리는 짧은 판정 함수.
 *
 * @bfqq: in-service 큐 - 현재 대기 중인 request 가 있는지, idling 이 필요한지
 *        확인할 대상.
 * @return: true 면 (1) bfqq 를 계속 in-service 상태로 유지하며 만료시키지
 *          말아야 하고, (2) device 를 idle 상태로 두어 bfqq 의 새 request
 *          도착을 기다려야 한다는 뜻. false 면 곧바로 만료 절차를 진행해도
 *          된다는 뜻.
 *
 * bfq_better_to_idle() 이 계산한 "idling 이 나은가"라는 판단에, "애초에
 * bfqq 가 정말로 비어 있는가"(RB_EMPTY_ROOT)라는 전제 조건을 추가로 검사한
 * 최종 게이트다. bfqq 에 아직 대기 중인 request 가 남아 있다면 idling
 * 여부를 따질 필요조차 없이(당연히 계속 서비스), 완전히 빈 경우에만
 * bfq_better_to_idle 의 복잡한 판단이 의미를 가진다.
 *
 * 실행 컨텍스트: bfqd->lock 을 쥔 채 bfq_select_queue 에서 다음 큐를 고를지,
 * 아니면 현재 큐를 계속 유지할지 결정하는 시점에 호출된다.
 *
 * 호출 체인:
 *   bfq_select_queue → [bfq_bfqq_must_idle] → bfq_better_to_idle
 */
static bool bfq_bfqq_must_idle(struct bfq_queue *bfqq)
{
	/* [한국어] bfqq 의 정렬 트리(sort_list, 대기 중인 request 들의 위치 기반 RB-tree)가
	 * 완전히 비어 있고(RB_EMPTY_ROOT), 그리고 bfq_better_to_idle()이 idling 이 낫다고
	 * 판단할 때만 true - 둘 다 만족해야 device 를 실제로 idle 시킨다. */
	return RB_EMPTY_ROOT(&bfqq->sort_list) && bfq_better_to_idle(bfqq);
}

/*
 * This function chooses the queue from which to pick the next extra
 * I/O request to inject, if it finds a compatible queue. See the
 * comments on bfq_update_inject_limit() for details on the injection
 * mechanism, and for the definitions of the quantities mentioned
 * below.
 */
/*
 * [한국어]
 * bfq_choose_bfqq_for_injection - in-service 큐(bfqq)가 비어 service hole
 * 이 생겼을 때, 그 공백을 메울 "주입(injection)용" request 를 가진 다른
 * bfqq 를 찾아낸다.
 *
 * @bfqd: 디스크 전체 상태를 담는 BFQ 스케줄러 데이터.
 * @return: 주입 가능한 request 를 가진 bfq_queue 포인터. 적합한 큐를 찾지
 *          못했거나(inject_limit 초과 등) 아무 큐도 조건을 만족하지 못하면
 *          NULL 을 반환 - 호출자는 이 경우 주입 없이 device 를 그대로 idle
 *          시키거나 다른 경로로 진행한다.
 *
 * BFQ 는 in-service 큐가 순간적으로 비어도 즉시 idling 하지 않고, 다른 대기
 * 큐의 request 를 "끼워 넣어(inject)" NVMe NCQ 처럼 컨트롤러가 지원하는
 * 내부 큐 깊이(queue depth, 통상 최대 32~64)를 계속 채움으로써 드라이브가
 * 노는 시간을 줄인다. 다만 무분별하게 끼워 넣으면 원래 in-service 큐의
 * 서비스 보장이 깨질 수 있으므로, inject_limit(bfq_update_inject_limit 이
 * 계산)이라는 상한 안에서만, 그리고 weight-raised 큐나 thinktime 이 긴
 * (즉 다른 큐의 개입을 흡수할 여유가 있는) in-service 큐에 대해서만 제한
 * 없이 주입을 허용한다.
 *
 * 실행 컨텍스트: bfqd->lock 을 쥔 채 bfq_select_queue 에서 in-service 큐가
 * 비었을 때 호출된다.
 *
 * 호출 체인:
 *   bfq_select_queue → [bfq_choose_bfqq_for_injection] → bfq_serv_to_charge / bfq_bfqq_budget_left
 */
static struct bfq_queue *
bfq_choose_bfqq_for_injection(struct bfq_data *bfqd)
{
	/* [한국어] bfqq: 순회하며 검사할 후보 큐. in_serv_bfqq: 현재 in-service 상태인
	 * (비어서 injection 대상을 찾아야 하는) 큐 - bfqd 에서 직접 가져온다. */
	struct bfq_queue *bfqq, *in_serv_bfqq = bfqd->in_service_queue;
	/* [한국어] 이번에 주입을 허용할 최대 in-flight 요청 수 - in-service 큐 자체에
	 * 저장된 inject_limit 값(bfq_update_inject_limit 이 관찰 기반으로 동적 산출)에서 시작. */
	unsigned int limit = in_serv_bfqq->inject_limit;
	/* [한국어] 아래 for 루프에서 사용할 액추에이터(독립 접근 영역, 멀티 액추에이터
	 * 디스크 지원용) 인덱스. */
	int i;

	/*
	 * If
	 * - bfqq is not weight-raised and therefore does not carry
	 *   time-critical I/O,
	 * or
	 * - regardless of whether bfqq is weight-raised, bfqq has
	 *   however a long think time, during which it can absorb the
	 *   effect of an appropriate number of extra I/O requests
	 *   from other queues (see bfq_update_inject_limit for
	 *   details on the computation of this number);
	 * then injection can be performed without restrictions.
	 */
	/* [한국어] in-service 큐 자신이 (1) weight-raised 가 아니어서(wr_coeff==1) 시간에
	 * 민감한 I/O 를 나르지 않거나, 또는 (2) weight-raised 여부와 무관하게 thinktime
	 * 이 길어서(!short_ttime) 다른 큐의 request 가 끼어들어도 자신의 다음 request 를
	 * 낼 때까지 시간 여유가 있다면 - "제한 없이 주입 허용" 플래그를 켠다. */
	bool in_serv_always_inject = in_serv_bfqq->wr_coeff == 1 ||
		!bfq_bfqq_has_short_ttime(in_serv_bfqq);

	/*
	 * If
	 * - the baseline total service time could not be sampled yet,
	 *   so the inject limit happens to be still 0, and
	 * - a lot of time has elapsed since the plugging of I/O
	 *   dispatching started, so drive speed is being wasted
	 *   significantly;
	 * then temporarily raise inject limit to one request.
	 */
	/* [한국어] limit 이 아직 0(기준 서비스 시간을 한 번도 측정하지 못해 안전하게
	 * 0으로 초기화된 상태)이고, in-service 큐가 request 도착을 기다리는 중이며,
	 * idling 이 시작된 지 이미 bfq_slice_idle 만큼 시간이 지났다면 - 드라이브가
	 * 상당 시간 놀고 있다는 뜻이므로, 임시로 limit 을 1로 올려 최소한 하나는
	 * 주입을 시도해 본다(측정 데이터가 없어 보수적으로 시작만 허용). */
	if (limit == 0 && in_serv_bfqq->last_serv_time_ns == 0 &&
	    bfq_bfqq_wait_request(in_serv_bfqq) &&
	    time_is_before_eq_jiffies(bfqd->last_idling_start_jiffies +
				      bfqd->bfq_slice_idle)
		)
		limit = 1;

	/* [한국어] 이미 드라이브에 내려가 있는(in-flight) 전체 request 수가 이번에 허용된
	 * limit 이상이면 - 더 이상 주입할 여유가 없으므로(컨트롤러 큐가 이미 충분히
	 * 차 있음) 즉시 포기하고 NULL 반환. */
	if (bfqd->tot_rq_in_driver >= limit)
		return NULL;

	/*
	 * Linear search of the source queue for injection; but, with
	 * a high probability, very few steps are needed to find a
	 * candidate queue, i.e., a queue with enough budget left for
	 * its next request. In fact:
	 * - BFQ dynamically updates the budget of every queue so as
	 *   to accommodate the expected backlog of the queue;
	 * - if a queue gets all its requests dispatched as injected
	 *   service, then the queue is removed from the active list
	 *   (and re-added only if it gets new requests, but then it
	 *   is assigned again enough budget for its new backlog).
	 */
	/* [한국어] 멀티 액추에이터 디스크를 지원하기 위해, 액추에이터(독립 접근 영역)별로
	 * 별도의 active_list 를 순회한다 - 각 액추에이터는 물리적으로 독립적인 헤드/영역을
	 * 가지므로 주입 후보도 액추에이터 단위로 찾아야 한다. */
	for (i = 0; i < bfqd->num_actuators; i++) {
		/* [한국어] 이 액추에이터의 활성 큐 리스트를 선형 탐색 - bfqq 에 대기 중인
		 * request 가 있고(RB_EMPTY_ROOT 아님), (in-service 큐가 제한 없이 주입을
		 * 허용하거나 bfqq 자신이 weight-raised 이며), bfqq 의 다음 request 를
		 * 서비스하는 데 필요한 charge 가 bfqq 에 남은 budget 이내라면 - 주입 가능한
		 * 후보로 간주한다. */
		list_for_each_entry(bfqq, &bfqd->active_list[i], bfqq_list)
			if (!RB_EMPTY_ROOT(&bfqq->sort_list) &&
				(in_serv_always_inject || bfqq->wr_coeff > 1) &&
				bfq_serv_to_charge(bfqq->next_rq, bfqq) <=
				bfq_bfqq_budget_left(bfqq)) {
			/*
			 * Allow for only one large in-flight request
			 * on non-rotational devices, for the
			 * following reason. On non-rotationl drives,
			 * large requests take much longer than
			 * smaller requests to be served. In addition,
			 * the drive prefers to serve large requests
			 * w.r.t. to small ones, if it can choose. So,
			 * having more than one large requests queued
			 * in the drive may easily make the next first
			 * request of the in-service queue wait for so
			 * long to break bfqq's service guarantees. On
			 * the bright side, large requests let the
			 * drive reach a very high throughput, even if
			 * there is only one in-flight large request
			 * at a time.
			 */
			/* [한국어] 비회전식(SSD/NVMe) 장치이고, 후보 큐의 다음 request 가
			 * 큰 request(BFQQ_SECT_THR_NONROT 섹터 임계값 이상)이며, 이미
			 * in-flight 요청이 1개 이상 있다면 - 이 후보는 건너뛴다(continue).
			 * 큰 request 를 두 개 이상 동시에 밀어 넣으면 in-service 큐의 다음
			 * request 가 그 뒤에서 오래 기다려야 해 서비스 보장이 깨질 수 있기
			 * 때문 - 큰 request 는 한 번에 하나씩만 in-flight 로 허용. */
			if (!blk_queue_rot(bfqd->queue) &&
			    blk_rq_sectors(bfqq->next_rq) >=
			    BFQQ_SECT_THR_NONROT &&
			    bfqd->tot_rq_in_driver >= 1)
				continue;
			else {
				/* [한국어] 위 "큰 request 중복" 예외에 걸리지 않는, 유효한
				 * 주입 후보를 찾았다는 뜻 - 이번 서비스 슬롯에 injection 이
				 * 일어났음을 기록해 두면(rqs_injected), 이후 총 서비스 시간
				 * 측정 로직이 injection 여부를 고려해 계산을 조정할 수 있다. */
				bfqd->rqs_injected = true;
				/* [한국어] 이 후보 큐를 반환 - 호출자(bfq_select_queue)가 이
				 * 큐의 request 를 실제로 디스패치해 service hole 을 메운다. */
				return bfqq;
			}
		}
	}

	/* [한국어] 모든 액추에이터의 활성 리스트를 다 뒤졌지만 조건을 만족하는 큐를
	 * 찾지 못했다는 뜻 - 주입 없이 in-service 큐가 새 request 를 받을 때까지
	 * 기다리거나(idling) 다른 경로로 진행해야 한다. */
	return NULL;
}

/*
 * [한국어]
 * bfq_find_active_bfqq_for_actuator - idx번째 actuator(독립 접근 영역)에 대해
 * 지금 당장 서비스할 수 있는 bfqq 하나를 찾는다.
 *
 * @bfqd: 이 블록 장치(request_queue)에 대응하는 BFQ 스케줄러 전역 상태.
 * @idx: 검사할 actuator(멀티 액추에이터 HDD 또는 독립 접근 영역을 가진
 *       장치에서 0..num_actuators-1 범위의 인덱스) 번호.
 * @return: idx번째 actuator로 즉시 dispatch 가능한 bfq_queue 포인터.
 *          해당하는 큐가 하나도 없으면 NULL.
 *
 * bfq_find_bfqq_for_underused_actuator()가 부하가 낮은 actuator를 찾은
 * 뒤, 그 actuator로 실제로 보낼 I/O가 존재하는지 확인하려고 호출하는
 * 보조 함수다. 우선 현재 in_service_queue가 이미 이 actuator에 속해
 * 있으면(다른 actuator 전환 없이 이어서 서비스 가능하면) 그 큐를 그대로
 * 반환한다. 그렇지 않으면 idx 전용 활성 리스트(active_list[idx])를 앞에서
 * 부터 순회하며, dispatch되지 않은 요청이 있고(sort_list가 비어있지 않음)
 * budget이 남아 있는 첫 번째 큐를 골라 반환한다(최적 탐색이 아닌 첫-적합
 * 방식). 실행 컨텍스트: 호출자(bfq_select_queue)가 이미 bfqd->lock을
 * 보유한 상태에서 호출되므로 이 함수는 별도 락을 잡지 않는다. NVMe
 * 관점에서는, 여러 독립 접근 영역(actuator)을 가진 장치에서 유휴 상태인
 * 영역으로 보낼 다음 CID 후보 request를 찾는 역할을 한다.
 *
 * 호출 체인:
 *   bfq_find_bfqq_for_underused_actuator → [bfq_find_active_bfqq_for_actuator] → (리프 함수, 하위 호출 없음)
 */
static struct bfq_queue *
bfq_find_active_bfqq_for_actuator(struct bfq_data *bfqd, int idx)
{
	struct bfq_queue *bfqq;
	/* [한국어] active_list[idx] 순회용 커서. 아래 list_for_each_entry에서만 사용 */

	if (bfqd->in_service_queue &&
	    /* [한국어] 현재 서비스 중인 큐가 존재하는지 확인 - dispatch 직전 캐시된 in_service_queue */
	    bfqd->in_service_queue->actuator_idx == idx)
		/* [한국어] 그 큐가 마침 우리가 찾는 idx번째 actuator에 속하는지 검사 */
		return bfqd->in_service_queue;
		/* [한국어] 이미 서비스 중인 큐를 그대로 재사용 - 큐 전환/재-idle 비용을 피하기 위함 */

	list_for_each_entry(bfqq, &bfqd->active_list[idx], bfqq_list) {
	/* [한국어] idx번째 actuator에 걸린 "활성"(요청을 가진) bfqq들만 순회 - 다른 actuator의 큐는 이 리스트에 없음 */
		if (!RB_EMPTY_ROOT(&bfqq->sort_list) &&
			/* [한국어] 이 큐에 아직 dispatch되지 않은 요청이 최소 1개 있는지 확인(정렬 rb-tree가 비어있지 않은지) */
			bfq_serv_to_charge(bfqq->next_rq, bfqq) <=
			/* [한국어] 다음 요청을 서비스하는 데 필요한 비용(섹터 환산 charge)을 계산 */
				bfq_bfqq_budget_left(bfqq)) {
				/* [한국어] 그 비용이 큐에 남은 budget 이하인지 확인 - budget 초과 큐는 후보에서 제외 */
			return bfqq;
			/* [한국어] 조건을 만족하는 첫 큐를 즉시 반환(첫-적합 방식, 최적 큐 탐색이 아님) */
		}
	}

	return NULL;
	/* [한국어] idx번째 actuator로 지금 보낼 수 있는 큐가 없음을 호출자에게 알림 - 호출자는 다른 actuator나 다른 경로를 계속 검사 */
}

/*
 * Perform a linear scan of each actuator, until an actuator is found
 * for which the following three conditions hold: the load of the
 * actuator is below the threshold (see comments on
 * actuator_load_threshold for details) and lower than that of the
 * next actuator (comments on this extra condition below), and there
 * is a queue that contains I/O for that actuator. On success, return
 * that queue.
 *
 * Performing a plain linear scan entails a prioritization among
 * actuators. The extra condition above breaks this prioritization and
 * tends to distribute injection uniformly across actuators.
 */
/*
 * [한국어]
 * bfq_find_bfqq_for_underused_actuator - actuator_load_threshold 보다
 * 부하가 낮은(=한산한) actuator를 찾아, 그 actuator로 보낼 수 있는 bfqq를
 * 반환한다.
 *
 * @bfqd: BFQ 스케줄러 전역 상태. num_actuators/rq_in_driver[] 배열을 담고 있음.
 * @return: 부하가 낮은 actuator에 대해 즉시 dispatch 가능한 bfq_queue 포인터.
 *          모든 actuator가 이미 충분히 바쁘거나 보낼 I/O가 없으면 NULL.
 *
 * bfq_select_queue()가 현재 in-service 큐 외에 "추가로 injection할" 큐를
 * 찾을 때 호출한다. 각 actuator(독립 접근 영역, 예: 멀티 액추에이터 HDD의
 * 헤드 그룹)를 인덱스 순서대로 선형 스캔하면서, i번째 actuator의 in-flight
 * 요청 수(rq_in_driver[i])가 threshold보다 낮고 동시에 다음 actuator보다도
 * 낮은 경우에만 후보로 삼는다. 이 "다음 actuator보다 낮아야 한다"는 추가
 * 조건이 없으면 선형 스캔 순서상 앞쪽 actuator가 항상 우선권을 갖게 되어
 * injection이 특정 actuator에 편중되므로, 이 조건으로 균등 분산을 유도한다.
 * 조건을 만족하는 actuator를 찾으면 bfq_find_active_bfqq_for_actuator()로
 * 실제 dispatch 가능한 큐가 있는지 확인하고, 있으면 즉시 반환한다(첫-적합).
 * 실행 컨텍스트: 호출자가 bfqd->lock을 보유한 상태에서 실행되므로 락을
 * 별도로 잡지 않는다.
 * NVMe 관점: 다중 actuator(또는 다중 독립 접근 영역) 장치에서 한산한
 * 영역의 SQ/CQ 대역폭을 적극 활용해 전체 처리량을 끌어올리는 역할이다.
 *
 * 호출 체인:
 *   bfq_select_queue → [bfq_find_bfqq_for_underused_actuator] → bfq_find_active_bfqq_for_actuator
 */
static struct bfq_queue *
bfq_find_bfqq_for_underused_actuator(struct bfq_data *bfqd)
{
	int i;
	/* [한국어] 0..num_actuators-1 범위를 순회하는 actuator 인덱스 */

	for (i = 0 ; i < bfqd->num_actuators; i++) {
	/* [한국어] 모든 actuator를 인덱스 순서(선형 스캔)로 검사 */
		if (bfqd->rq_in_driver[i] < bfqd->actuator_load_threshold &&
		    /* [한국어] i번째 actuator의 현재 in-flight 요청 수가 부하 임계값보다 낮은지(=한산한지) 확인 */
		    (i == bfqd->num_actuators - 1 ||
		     /* [한국어] 마지막 actuator라면 "다음 actuator와 비교" 조건은 건너뜀(비교 대상이 없으므로) */
		     bfqd->rq_in_driver[i] < bfqd->rq_in_driver[i+1])) {
		     /* [한국어] 마지막이 아니라면 다음 actuator보다도 부하가 낮아야 후보로 인정 - 특정 actuator로의 injection 편중을 막기 위한 추가 조건 */
			struct bfq_queue *bfqq =
				/* [한국어] 이 한산한 actuator로 실제로 보낼 수 있는 bfqq가 있는지 조회 */
				bfq_find_active_bfqq_for_actuator(bfqd, i);

			/* [한국어] 조건을 만족하는 첫 actuator에서 큐를 찾으면 더 스캔하지 않고 즉시 반환 */
			if (bfqq)
				return bfqq;
		}
	}

	/* [한국어] 한산한 actuator가 없거나, 있어도 보낼 I/O가 없음을 호출자에게 알림 */
	return NULL;
}


/*
 * Select a queue for service.  If we have a current queue in service,
 * check whether to continue servicing it, or retrieve and set a new one.
 */
/*
 * [한국어]
 * bfq_select_queue - 다음에 서비스할(=dispatch할 request를 뽑아낼) bfqq를
 * 선정한다. budget 소진 여부, idling 여부, injection(다른 큐 끼워넣기)
 * 가능성, actuator 부하 균형을 모두 고려해 최종적으로 하나의 bfqq(또는
 * NULL)를 돌려준다.
 *
 * @bfqd: BFQ 스케줄러 전역 상태. in_service_queue, idle_slice_timer 등을 포함.
 * @return: 다음에 서비스할 bfq_queue. 지금 당장 아무 것도 서비스할 수
 *          없으면(예: idling 유지 중이고 injection도 불가능) NULL.
 *
 * BFQ는 한 번에 하나의 "in-service queue"만 서비스하는 것을 기본으로
 * 하되(B-WF2Q+ 스케줄링 정확성 보장), 처리량 손실을 막기 위해 예외적으로
 * 다른 큐의 I/O를 "주입(inject)"하는 것을 허용한다. 이 함수는 그 결정을
 * 내리는 핵심 로직이다: (1) 이미 in-service 큐가 있으면 budget timeout /
 * budget exhaustion 여부를 검사해 만료(expire)할지 판단하고, (2) 만료하지
 * 않는다면 다른 한산한 actuator로 injection할지, 아니면 idling을 유지한
 * 채 필요하면 waker/woken 큐의 I/O를 주입할지 결정하며, (3) in-service
 * 큐가 아예 없으면 bfq_set_in_service_queue()로 새 큐를 뽑는다.
 * goto 기반 상태 기계로 구현되어 있으며 각 레이블(check_queue, expire,
 * new_queue, keep_queue)은 재진입 지점 역할을 한다.
 * 실행 컨텍스트: 호출자 __bfq_dispatch_request()가 이미 bfqd->lock을
 * 보유한 상태에서 호출되므로 별도 락을 잡지 않는다. blk-mq dispatch
 * 컨텍스트(하드웨어 큐당 하나)에서 실행된다.
 * NVMe 관점: 이 함수가 고른 bfqq의 next_rq가 바로 다음 SQ(Submission
 * Queue) 엔트리로 변환될 request이며, injection 메커니즘은 NVMe
 * controller의 큐 깊이(queue depth)를 최대한 채워 처리량을 높이면서도
 * 특정 프로세스의 latency를 해치지 않으려는 목적을 갖는다.
 *
 * 호출 체인:
 *   __bfq_dispatch_request → [bfq_select_queue] → bfq_find_bfqq_for_underused_actuator,
 *   bfq_bfqq_expire, bfq_set_in_service_queue, bfq_choose_bfqq_for_injection
 */
static struct bfq_queue *bfq_select_queue(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq, *inject_bfqq;
	/* [한국어] bfqq: 최종적으로 선택/반환될 큐. inject_bfqq: 한산한 actuator에 주입할 후보 큐(임시 변수) */
	struct request *next_rq;
	/* [한국어] in-service 큐의 다음 dispatch 후보 request 포인터 - budget 초과 여부 판단에 사용 */
	enum bfqq_expiration reason = BFQQE_BUDGET_TIMEOUT;
	/* [한국어] bfqq를 만료(expire)시킬 경우 그 사유 코드. 기본값은 budget timeout이며 아래에서 상황에 따라 덮어씀 */

	bfqq = bfqd->in_service_queue;
	/* [한국어] 현재 서비스 중인 큐를 우선 후보로 삼음 - 없으면(NULL) 처음부터 새 큐를 뽑아야 함 */
	if (!bfqq)
		/* [한국어] in-service 큐가 아예 없는 초기/유휴 상태 - 새 큐 선정 경로로 점프 */
		goto new_queue;

	/* [한국어] 디버그 트레이스 로그 - 이미 in-service인 큐를 계속 검사하는 경로로 들어왔음을 기록 */
	bfq_log_bfqq(bfqd, bfqq, "select_queue: already in-service queue");

	/*
	 * Do not expire bfqq for budget timeout if bfqq may be about
	 * to enjoy device idling. The reason why, in this case, we
	 * prevent bfqq from expiring is the same as in the comments
	 * on the case where bfq_bfqq_must_idle() returns true, in
	 * bfq_completed_request().
	 */
	/* [한국어] budget timeout으로 만료할 만한 조건인지 확인(시간 초과로 인한 강제 만료 후보) */
	if (bfq_may_expire_for_budg_timeout(bfqq) &&
	    !bfq_bfqq_must_idle(bfqq))
		/* [한국어] 동시에, 이 큐가 device idling을 계속 누려야 하는 상황이 아닌지도 확인 - idling이 필요하면 timeout이어도 만료하지 않음 */
		goto expire;
		/* [한국어] budget timeout 만료 조건 성립 - expire 레이블로 점프해 실제 만료 처리 */

check_queue:
	/*
	 *  If some actuator is underutilized, but the in-service
	 *  queue does not contain I/O for that actuator, then try to
	 *  inject I/O for that actuator.
	 */
	inject_bfqq = bfq_find_bfqq_for_underused_actuator(bfqd);
	/* [한국어] 한산한(부하가 낮은) actuator가 있으면 그쪽으로 보낼 수 있는 bfqq를 조회 */
	if (inject_bfqq && inject_bfqq != bfqq)
		/* [한국어] 찾은 큐가 존재하고, 그 큐가 지금 검사 중인 bfqq와 다르면(=다른 actuator의 별개 큐라면) */
		return inject_bfqq;
		/* [한국어] 즉시 그 큐를 반환해 한산한 actuator로 injection - 현재 큐의 in-service 상태는 그대로 유지됨 */

	/*
	 * This loop is rarely executed more than once. Even when it
	 * happens, it is much more convenient to re-execute this loop
	 * than to return NULL and trigger a new dispatch to get a
	 * request served.
	 */
	next_rq = bfqq->next_rq;
	/* [한국어] 현재 bfqq에서 다음으로 dispatch할 request를 가져옴 - NULL이면 대기 중인 요청이 없다는 뜻 */
	/*
	 * If bfqq has requests queued and it has enough budget left to
	 * serve them, keep the queue, otherwise expire it.
	 */
	if (next_rq) {
	/* [한국어] 대기 중인 다음 요청이 있는 경우의 분기 - budget이 남아있으면 큐를 유지, 아니면 만료 */
		if (bfq_serv_to_charge(next_rq, bfqq) >
			bfq_bfqq_budget_left(bfqq)) {
			/* [한국어] 다음 요청을 서비스하는 데 필요한 비용이 남은 budget을 초과하는지 확인 */
			/*
			 * Expire the queue for budget exhaustion,
			 * which makes sure that the next budget is
			 * enough to serve the next request, even if
			 * it comes from the fifo expired path.
			 */
			reason = BFQQE_BUDGET_EXHAUSTED;
			/* [한국어] 만료 사유를 "budget 소진"으로 설정 - 다음 budget 재할당 시 이 request를 충분히 처리할 수 있도록 함 */
			goto expire;
			/* [한국어] budget이 부족하므로 이 큐를 만료시키는 경로로 점프 */
		} else {
		/* [한국어] budget이 충분한 경우 - 이 큐를 계속 서비스할 수 있음 */
			/*
			 * The idle timer may be pending because we may
			 * not disable disk idling even when a new request
			 * arrives.
			 */
			if (bfq_bfqq_wait_request(bfqq)) {
			/* [한국어] 이 큐가 새 request 도착을 기다리며 idle 타이머를 걸어둔 상태인지 확인 */
				/*
				 * If we get here: 1) at least a new request
				 * has arrived but we have not disabled the
				 * timer because the request was too small,
				 * 2) then the block layer has unplugged
				 * the device, causing the dispatch to be
				 * invoked.
				 *
				 * Since the device is unplugged, now the
				 * requests are probably large enough to
				 * provide a reasonable throughput.
				 * So we disable idling.
				 */
				bfq_clear_bfqq_wait_request(bfqq);
				/* [한국어] wait_request 플래그 해제 - 더 이상 새 request를 기다리며 idle할 필요가 없다고 판단 */
				hrtimer_try_to_cancel(&bfqd->idle_slice_timer);
				/* [한국어] idle slice 타이머를 취소 시도 - block layer가 이미 unplug했으므로 인위적 idling을 중단하고 즉시 dispatch 진행 */
			}
			goto keep_queue;
			/* [한국어] 현재 bfqq를 그대로 유지한 채 함수를 종료하는 경로로 점프 */
		}
	}

	/*
	 * No requests pending. However, if the in-service queue is idling
	 * for a new request, or has requests waiting for a completion and
	 * may idle after their completion, then keep it anyway.
	 *
	 * Yet, inject service from other queues if it boosts
	 * throughput and is possible.
	 */
	if (bfq_bfqq_wait_request(bfqq) ||
	    /* [한국어] 대기 중인 request는 없지만, 이 큐가 새 request 도착을 기다리며 idling 중인지 확인 */
	    (bfqq->dispatched != 0 && bfq_better_to_idle(bfqq))) {
	    /* [한국어] 또는 이미 디스패치되어 완료를 기다리는 request가 있고(dispatched != 0), 완료 후에도 idling하는 편이 나은지 확인 */
		unsigned int act_idx = bfqq->actuator_idx;
		/* [한국어] 이 bfqq가 속한 actuator 인덱스 - bic->bfqq[][] 2차원 배열에서 같은 actuator의 비동기 큐를 찾을 때 사용 */
		struct bfq_queue *async_bfqq = NULL;
		/* [한국어] 같은 프로세스(bic)의 비동기 I/O 큐 후보 - 아래에서 조건을 만족하면 채워짐 */
		struct bfq_queue *blocked_bfqq =
			!hlist_empty(&bfqq->woken_list) ?
			/* [한국어] bfqq가 깨운(wake) 큐 목록이 비어있지 않은지 확인 */
			container_of(bfqq->woken_list.first,
				     struct bfq_queue,
				     woken_list_node)
				     /* [한국어] 비어있지 않다면 그 목록의 첫 번째 큐를 woken_list_node 필드를 통해 bfq_queue 구조체로 역산(container_of) */
			: NULL;
			/* [한국어] 비어있으면 NULL - bfqq에 종속되어 깨어난 큐가 없다는 뜻 */

		if (bfqq->bic && bfqq->bic->bfqq[0][act_idx] &&
		    /* [한국어] bfqq에 연결된 io_cq(bic)가 있고, 같은 actuator의 비동기(0=async) 큐가 존재하는지 확인 */
		    bfq_bfqq_busy(bfqq->bic->bfqq[0][act_idx]) &&
		    /* [한국어] 그 비동기 큐가 활성(active tree에 존재) 상태인지 확인 */
		    bfqq->bic->bfqq[0][act_idx]->next_rq)
		    /* [한국어] 그 비동기 큐에 dispatch할 다음 request가 실제로 있는지 확인 */
			async_bfqq = bfqq->bic->bfqq[0][act_idx];
			/* [한국어] 세 조건을 모두 만족하면 이 비동기 큐를 injection 후보로 채택 */
		/*
		 * The next four mutually-exclusive ifs decide
		 * whether to try injection, and choose the queue to
		 * pick an I/O request from.
		 *
		 * The first if checks whether the process associated
		 * with bfqq has also async I/O pending. If so, it
		 * injects such I/O unconditionally. Injecting async
		 * I/O from the same process can cause no harm to the
		 * process. On the contrary, it can only increase
		 * bandwidth and reduce latency for the process.
		 *
		 * The second if checks whether there happens to be a
		 * non-empty waker queue for bfqq, i.e., a queue whose
		 * I/O needs to be completed for bfqq to receive new
		 * I/O. This happens, e.g., if bfqq is associated with
		 * a process that does some sync. A sync generates
		 * extra blocking I/O, which must be completed before
		 * the process associated with bfqq can go on with its
		 * I/O. If the I/O of the waker queue is not served,
		 * then bfqq remains empty, and no I/O is dispatched,
		 * until the idle timeout fires for bfqq. This is
		 * likely to result in lower bandwidth and higher
		 * latencies for bfqq, and in a severe loss of total
		 * throughput. The best action to take is therefore to
		 * serve the waker queue as soon as possible. So do it
		 * (without relying on the third alternative below for
		 * eventually serving waker_bfqq's I/O; see the last
		 * paragraph for further details). This systematic
		 * injection of I/O from the waker queue does not
		 * cause any delay to bfqq's I/O. On the contrary,
		 * next bfqq's I/O is brought forward dramatically,
		 * for it is not blocked for milliseconds.
		 *
		 * The third if checks whether there is a queue woken
		 * by bfqq, and currently with pending I/O. Such a
		 * woken queue does not steal bandwidth from bfqq,
		 * because it remains soon without I/O if bfqq is not
		 * served. So there is virtually no risk of loss of
		 * bandwidth for bfqq if this woken queue has I/O
		 * dispatched while bfqq is waiting for new I/O.
		 *
		 * The fourth if checks whether bfqq is a queue for
		 * which it is better to avoid injection. It is so if
		 * bfqq delivers more throughput when served without
		 * any further I/O from other queues in the middle, or
		 * if the service times of bfqq's I/O requests both
		 * count more than overall throughput, and may be
		 * easily increased by injection (this happens if bfqq
		 * has a short think time). If none of these
		 * conditions holds, then a candidate queue for
		 * injection is looked for through
		 * bfq_choose_bfqq_for_injection(). Note that the
		 * latter may return NULL (for example if the inject
		 * limit for bfqq is currently 0).
		 *
		 * NOTE: motivation for the second alternative
		 *
		 * Thanks to the way the inject limit is updated in
		 * bfq_update_has_short_ttime(), it is rather likely
		 * that, if I/O is being plugged for bfqq and the
		 * waker queue has pending I/O requests that are
		 * blocking bfqq's I/O, then the fourth alternative
		 * above lets the waker queue get served before the
		 * I/O-plugging timeout fires. So one may deem the
		 * second alternative superfluous. It is not, because
		 * the fourth alternative may be way less effective in
		 * case of a synchronization. For two main
		 * reasons. First, throughput may be low because the
		 * inject limit may be too low to guarantee the same
		 * amount of injected I/O, from the waker queue or
		 * other queues, that the second alternative
		 * guarantees (the second alternative unconditionally
		 * injects a pending I/O request of the waker queue
		 * for each bfq_dispatch_request()). Second, with the
		 * fourth alternative, the duration of the plugging,
		 * i.e., the time before bfqq finally receives new I/O,
		 * may not be minimized, because the waker queue may
		 * happen to be served only after other queues.
		 */
		/* [한국어] 아래 네 개의 else-if는 상호 배타적이며, 위에서부터 우선순위 순으로 injection 대상을 고른다:
		 * (1) 같은 프로세스의 비동기 큐 무조건 주입, (2) bfqq를 깨워줄 waker 큐 주입,
		 * (3) bfqq가 깨운 woken(blocked) 큐 주입, (4) 위 셋 다 아니면 일반 injection 후보 탐색(bfq_choose_bfqq_for_injection) */
		if (async_bfqq &&
		    /* [한국어] (1) 비동기 큐 후보가 존재하고 */
		    icq_to_bic(async_bfqq->next_rq->elv.icq) == bfqq->bic &&
		    /* [한국어] 그 큐의 다음 request가 정말 같은 프로세스(bic)에서 온 것인지 재확인 */
		    bfq_serv_to_charge(async_bfqq->next_rq, async_bfqq) <=
		    bfq_bfqq_budget_left(async_bfqq))
		    /* [한국어] 그 비동기 큐 자체의 budget도 충분히 남아있는지 확인 */
			bfqq = async_bfqq;
			/* [한국어] 조건 충족 - 이번 dispatch 대상을 비동기 큐로 교체(무조건 주입) */
		else if (bfqq->waker_bfqq &&
			   /* [한국어] (2) bfqq를 깨워줄 waker 큐가 등록되어 있고 */
			   bfq_bfqq_busy(bfqq->waker_bfqq) &&
			   /* [한국어] 그 waker 큐가 활성 상태이며 */
			   bfqq->waker_bfqq->next_rq &&
			   /* [한국어] 실제로 dispatch할 request를 갖고 있고 */
			   bfq_serv_to_charge(bfqq->waker_bfqq->next_rq,
					      bfqq->waker_bfqq) <=
			   bfq_bfqq_budget_left(bfqq->waker_bfqq)
			   /* [한국어] waker 큐의 budget도 충분한지 확인 */
			)
			bfqq = bfqq->waker_bfqq;
			/* [한국어] 조건 충족 - waker 큐의 I/O를 먼저 서비스해 bfqq가 빨리 새 I/O를 받을 수 있게 함 */
		else if (blocked_bfqq &&
			   /* [한국어] (3) 위에서 계산해둔 woken(blocked) 큐가 존재하고 */
			   bfq_bfqq_busy(blocked_bfqq) &&
			   /* [한국어] 활성 상태이며 */
			   blocked_bfqq->next_rq &&
			   /* [한국어] dispatch할 request가 있고 */
			   bfq_serv_to_charge(blocked_bfqq->next_rq,
					      blocked_bfqq) <=
			   bfq_bfqq_budget_left(blocked_bfqq)
			   /* [한국어] budget도 충분한지 확인 */
			)
			bfqq = blocked_bfqq;
			/* [한국어] 조건 충족 - bfqq가 깨운 큐를 주입해도 bfqq 자신의 대역폭을 뺏지 않으므로 안전하게 서비스 */
		else if (!idling_boosts_thr_without_issues(bfqd, bfqq) &&
			 /* [한국어] (4) idling이 부작용 없이 처리량을 높여주는 상황이 아니고(=idling만으로는 부족하고) */
			 (bfqq->wr_coeff == 1 || bfqd->wr_busy_queues > 1 ||
			  /* [한국어] bfqq가 weight-raised 상태가 아니거나, weight-raised 큐가 여럿이거나 */
			  !bfq_bfqq_has_short_ttime(bfqq)))
			  /* [한국어] 또는 bfqq의 think-time이 짧지 않은(=injection에 민감하지 않은) 경우 */
			bfqq = bfq_choose_bfqq_for_injection(bfqd);
			/* [한국어] 위 세 특수 후보가 모두 없을 때 일반적인 injection 후보 탐색 함수를 호출 - NULL을 반환할 수도 있음(inject limit 0 등) */
		else
			bfqq = NULL;
			/* [한국어] injection을 시도하지 않는 편이 낫다고 판단되는 경우 - 이번엔 아무 것도 dispatch하지 않음 */

		goto keep_queue;
		/* [한국어] 위에서 결정된 bfqq(주입 대상 또는 NULL)를 그대로 갖고 함수 종료 지점으로 이동 - 만료(expire) 처리는 하지 않음 */
	}

	reason = BFQQE_NO_MORE_REQUESTS;
	/* [한국어] 여기까지 왔다면 대기 중인 request도 없고 idling/injection 조건도 아님 - 만료 사유를 "더 이상 요청 없음"으로 설정 */
expire:
	bfq_bfqq_expire(bfqd, bfqq, false, reason);
	/* [한국어] 결정된 사유(reason)로 현재 bfqq를 만료 처리 - B-WF2Q+ 가상 시간(vtime)/타임스탬프 갱신이 여기서 발생 */
new_queue:
	bfqq = bfq_set_in_service_queue(bfqd);
	/* [한국어] B-WF2Q+ 스케줄링 트리에서 다음으로 서비스할 큐를 새로 선정해 in_service_queue로 설정 */
	if (bfqq) {
	/* [한국어] 새 큐가 성공적으로 선정되었는지 확인 - 활성 큐가 아예 없으면 NULL일 수 있음 */
		bfq_log_bfqq(bfqd, bfqq, "select_queue: checking new queue");
		/* [한국어] 디버그 로그 - 새로 선정된 큐를 다시 check_queue 단계부터 검증하기 위함 */
		goto check_queue;
		/* [한국어] 새로 뽑은 큐도 injection/budget 로직을 동일하게 거쳐야 하므로 check_queue로 재진입 */
	}
keep_queue:
	if (bfqq)
		/* [한국어] 최종적으로 반환할 큐가 있는 경우 */
		bfq_log_bfqq(bfqd, bfqq, "select_queue: returned this queue");
	/* [한국어] 반환할 큐가 없는 경우(NULL) - dispatch할 것이 없음을 로그로 남김 */
	else
		bfq_log(bfqd, "select_queue: no queue returned");

	/* [한국어] 최종 선택된 bfqq(또는 NULL)를 호출자(__bfq_dispatch_request)에게 반환 */
	return bfqq;
}

/*
 * [한국어]
 * bfq_update_wr_data - bfqq의 weight-raising(가중치 일시 상승) 상태를
 * 갱신하고, 필요하면 종료(end) 시키거나 entity 가중치를 재계산한다.
 *
 * @bfqd: BFQ 스케줄러 전역 상태. bfq_wr_rt_max_time 등 wr 관련 튜닝값 보유.
 * @bfqq: weight-raising 상태를 검사/갱신할 대상 큐. dispatch 직후(즉,
 *        이 큐가 실제로 서비스를 받은 시점)에 호출됨.
 * @return: 없음(void). bfqq->wr_coeff, entity->weight 등을 직접 갱신.
 *
 * BFQ는 상호작용성(interactive) 프로세스나 소프트 실시간(soft real-time)
 * 프로세스의 응답성을 높이기 위해, 그런 프로세스의 bfqq에 한시적으로
 * 가중치를 크게 올려주는 weight-raising 메커니즘을 쓴다(wr_coeff > 1).
 * 이 함수는 매 dispatch마다 호출되어 (1) 현재 weight-raised 상태인지
 * 확인하고, (2) burst(짧은 시간에 대량 생성된 프로세스 그룹)로 활성화된
 * 큐이거나 wr 기간(wr_cur_max_time)이 만료되었으면 weight-raising을
 * 종료하며, (3) soft-rt wr이 끝나가는데 아직 interactive wr 기간 내라면
 * interactive wr로 전환하고, (4) 특정 큐가 wr 기간 동안 이미 과도한
 * service_from_wr을 받았다면 형평성을 위해 조기 종료시킨다. 마지막으로
 * entity의 실제 weight(entity->weight)와 wr_coeff에 따른 목표 상태가
 * 어긋나면 즉시(bfqq가 이번에 처음 wr을 벗어나거나 새로 wr에 들어간
 * 경우) __bfq_entity_update_weight_prio()로 재계산해, 다음 활성화까지
 * 기다리지 않고 바로 반영한다.
 * 실행 컨텍스트: 호출자(bfq_dispatch_rq_from_bfqq)가 bfqd->lock을
 * 보유한 상태에서 실행되며, entity가 스케줄링 트리에 붙어 있을 수
 * 있으므로 entity 갱신 함수 호출 시 마지막 인자(in_service)를 false로
 * 넘겨 트리 재조정 규칙을 지킨다.
 * NVMe 관점: BFQ의 weight는 NVMe 자체의 우선순위 큐(SQ 우선순위/CID)와는
 * 무관하며, 순전히 소프트웨어 스케줄러 계층에서 "이 bfqq가 얼마나 자주,
 * 얼마나 많이 SQ 슬롯을 배정받는지"를 결정하는 값이다.
 *
 * 호출 체인:
 *   bfq_dispatch_rq_from_bfqq → [bfq_update_wr_data] → bfq_bfqq_end_wr, switch_back_to_interactive_wr, __bfq_entity_update_weight_prio
 */
static void bfq_update_wr_data(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	struct bfq_entity *entity = &bfqq->entity;
	/* [한국어] bfqq에 내장된 스케줄링 엔티티 - B-WF2Q+ 트리에서 이 큐를 대표하는 노드 */

	if (bfqq->wr_coeff > 1) { /* queue is being weight-raised */
	/* [한국어] wr_coeff(가중치 배율)가 1보다 크면 현재 weight-raising 적용 중이라는 뜻 - 이 블록 전체가 wr 종료 조건 검사 */
		bfq_log_bfqq(bfqd, bfqq,
			"raising period dur %u/%u msec, old coeff %u, w %d(%d)",
			jiffies_to_msecs(jiffies - bfqq->last_wr_start_finish),
			jiffies_to_msecs(bfqq->wr_cur_max_time),
			bfqq->wr_coeff,
			bfqq->entity.weight, bfqq->entity.orig_weight);
			/* [한국어] 디버그 트레이스: 현재까지 진행된 wr 기간, 최대 허용 기간, 배율, 실제/원본 가중치를 기록 */

		if (entity->prio_changed)
			/* [한국어] 아직 반영되지 않은 ioprio 변경이 남아있는지 확인 - 있으면 안 됨(모순 상태) */
			bfq_log_bfqq(bfqd, bfqq, "WARN: pending prio change");
			/* [한국어] 로직상 있어서는 안 되는 상태이므로 경고만 남기고 계속 진행 */

		/*
		 * If the queue was activated in a burst, or too much
		 * time has elapsed from the beginning of this
		 * weight-raising period, then end weight raising.
		 */
		if (bfq_bfqq_in_large_burst(bfqq))
			/* [한국어] 이 큐가 대량 프로세스 생성(burst)의 일부로 활성화되었는지 확인 - burst 큐는 진짜 interactive가 아닐 가능성이 높음 */
			bfq_bfqq_end_wr(bfqq);
			/* [한국어] burst로 판명되면 즉시 weight-raising 종료 - 오분류로 인한 부당한 가중치 상승을 회수 */
		else if (time_is_before_jiffies(bfqq->last_wr_start_finish +
						bfqq->wr_cur_max_time)) {
						/* [한국어] burst는 아니지만, wr 시작 시각 + 최대 허용 기간이 이미 지났는지(=기간 만료) 확인 */
			if (bfqq->wr_cur_max_time != bfqd->bfq_wr_rt_max_time ||
			/* [한국어] 현재 적용 중인 최대 기간이 soft-rt 전용 최대 기간과 다르면(=interactive wr이면) */
			time_is_before_jiffies(bfqq->wr_start_at_switch_to_srt +
					       bfq_wr_duration(bfqd))) {
					       /* [한국어] 또는 soft-rt로 전환된 시점 기준으로도 이미 interactive wr 기간이 다 지났으면 */
				/*
				 * Either in interactive weight
				 * raising, or in soft_rt weight
				 * raising with the
				 * interactive-weight-raising period
				 * elapsed (so no switch back to
				 * interactive weight raising).
				 */
				bfq_bfqq_end_wr(bfqq);
				/* [한국어] 두 경우 모두 interactive wr로 되돌아갈 여지가 없으므로 완전히 wr 종료 */
			} else { /*
				  * soft_rt finishing while still in
				  * interactive period, switch back to
				  * interactive weight raising
				  */
				switch_back_to_interactive_wr(bfqq, bfqd);
				/* [한국어] soft-rt wr은 끝나가지만 아직 interactive wr 기간 내이므로 interactive wr 설정으로 되돌림 - 완전 종료 대신 전환 */
				bfqq->entity.prio_changed = 1;
				/* [한국어] weight 재계산이 필요함을 표시 - 아래 마지막 블록 또는 다음 활성화 시 반영됨 */
			}
		}
		if (bfqq->wr_coeff > 1 &&
		    /* [한국어] 여전히 weight-raised 상태이고 */
		    bfqq->wr_cur_max_time != bfqd->bfq_wr_rt_max_time &&
		    /* [한국어] soft-rt 전용 wr이 아니며(=interactive wr이며) */
		    bfqq->service_from_wr > max_service_from_wr) {
		    /* [한국어] 이 wr 기간 동안 이미 규정된 한도(max_service_from_wr)보다 많은 서비스를 받았는지 확인 */
			/* see comments on max_service_from_wr */
			bfq_bfqq_end_wr(bfqq);
			/* [한국어] 한도를 초과했으면 다른 큐와의 형평성을 위해 wr을 조기 종료 */
		}
	}
	/*
	 * To improve latency (for this or other queues), immediately
	 * update weight both if it must be raised and if it must be
	 * lowered. Since, entity may be on some active tree here, and
	 * might have a pending change of its ioprio class, invoke
	 * next function with the last parameter unset (see the
	 * comments on the function).
	 */
	if ((entity->weight > entity->orig_weight) != (bfqq->wr_coeff > 1))
		/* [한국어] "현재 적용된 weight가 원본보다 높다"는 사실과 "wr_coeff>1(=wr 중)"이라는 사실이 서로 어긋나는지 확인
		 * - 예: wr은 방금 끝났는데 weight는 아직 올라간 상태로 남아있는 경우 */
		__bfq_entity_update_weight_prio(bfq_entity_service_tree(entity),
						entity, false);
						/* [한국어] 어긋남이 발견되면 즉시 entity의 실제 weight를 재계산 - 마지막 인자 false는 "지금 in-service 큐가 아닌 것처럼" 처리해 트리 재조정 규칙을 따르게 함 */
}

/*
 * Dispatch next request from bfqq.
 */
/*
 * [한국어]
 * bfq_dispatch_rq_from_bfqq - 선택된 bfqq에서 실제로 dispatch할
 * request(next_rq) 하나를 꺼내고, 그에 따른 budget 소모/weight-raising
 * 갱신/필요 시 큐 만료까지 처리한다.
 *
 * @bfqd: BFQ 스케줄러 전역 상태.
 * @bfqq: bfq_select_queue()가 이번에 서비스하기로 결정한 큐.
 * @return: bfqq->next_rq였던 request 포인터. blk-mq가 이를 그대로
 *          하드웨어 큐에 전달한다.
 *
 * __bfq_dispatch_request()가 bfq_select_queue()로 큐를 고른 뒤, 그 큐의
 * 실제 request를 꺼내 blk-mq에 넘기기 위해 호출한다. 처리 순서는:
 * (1) 서비스 비용(service_to_charge)을 계산해 bfq_bfqq_served()로
 * budget을 차감하고, (2) 다른 코드 경로(bfq_update_peak_rate 등)가 이
 * dispatch를 기다리고 있었다면(wait_dispatch) 그 사실을 통지, (3)
 * bfq_dispatch_remove()로 요청을 BFQ 내부 자료구조(정렬 트리/dispatch
 * 리스트)에서 제거, (4) bfqq가 여전히 in-service 큐라면
 * bfq_update_wr_data()로 weight-raising 상태를 즉시 갱신, (5) bfqq가
 * CLASS_IDLE(가장 낮은 ionice 클래스)이고 다른 큐들이 대기 중이면,
 * budget이 남아 있어도 즉시 만료시켜 다른 큐에 기회를 준다(IDLE
 * 클래스는 항상 남에게 양보).
 * 실행 컨텍스트: 호출자가 bfqd->lock을 보유한 상태에서 실행.
 * NVMe 관점: 이 함수가 반환하는 request가 곧 blk_mq_run_hw_queue를
 * 거쳐 NVMe controller의 SQ(Submission Queue)에 CID로 기록되며,
 * doorbell은 이후 blk-mq 계층에서 일괄적으로 울린다.
 *
 * 호출 체인:
 *   __bfq_dispatch_request → [bfq_dispatch_rq_from_bfqq] → bfq_bfqq_served, bfq_dispatch_remove, bfq_update_wr_data, bfq_bfqq_expire
 */
static struct request *bfq_dispatch_rq_from_bfqq(struct bfq_data *bfqd,
						 struct bfq_queue *bfqq)
{
	struct request *rq = bfqq->next_rq;
	/* [한국어] 이번에 실제로 dispatch할 request - bfq_select_queue 단계에서 이미 budget 검증을 마친 후보 */
	unsigned long service_to_charge;
	/* [한국어] 이 request를 서비스하는 데 필요한 비용(섹터 환산) - 아래에서 계산해 budget 차감에 사용 */

	service_to_charge = bfq_serv_to_charge(rq, bfqq);
	/* [한국어] rq의 크기와 bfqq의 상태(seeky 여부 등)를 반영해 charge할 비용 계산 */

	bfq_bfqq_served(bfqq, service_to_charge);
	/* [한국어] 계산된 비용만큼 bfqq의 budget을 차감하고, B-WF2Q+ 가상 시간(finish time)을 갱신 */

	if (bfqq == bfqd->in_service_queue && bfqd->wait_dispatch) {
	/* [한국어] 이 큐가 현재 in-service 큐이고, 어떤 코드가 "다음 dispatch"를 기다리고 있었는지(wait_dispatch) 확인 - peak-rate 측정 등에서 사용 */
		bfqd->wait_dispatch = false;
		/* [한국어] 대기 조건 해제 - 이번 dispatch로 그 대기가 충족됨 */
		bfqd->waited_rq = rq;
		/* [한국어] 대기하던 쪽이 참조할 수 있도록 방금 dispatch한 request를 기록 */
	}

	bfq_dispatch_remove(bfqd->queue, rq);
	/* [한국어] rq를 BFQ의 내부 자료구조(정렬 rb-tree, dispatch 리스트 등)에서 제거 - 이제 BFQ가 아닌 blk-mq/드라이버가 소유 */

	if (bfqq != bfqd->in_service_queue)
		/* [한국어] bfqq가 (dispatch 리스트에서 직접 꺼낸 경우 등으로) 더 이상 in-service 큐가 아니라면 */
		return rq;
		/* [한국어] weight-raising/만료 처리는 in-service 큐에만 의미가 있으므로 그대로 조기 반환 */

	/*
	 * If weight raising has to terminate for bfqq, then next
	 * function causes an immediate update of bfqq's weight,
	 * without waiting for next activation. As a consequence, on
	 * expiration, bfqq will be timestamped as if has never been
	 * weight-raised during this service slot, even if it has
	 * received part or even most of the service as a
	 * weight-raised queue. This inflates bfqq's timestamps, which
	 * is beneficial, as bfqq is then more willing to leave the
	 * device immediately to possible other weight-raised queues.
	 */
	bfq_update_wr_data(bfqd, bfqq);
	/* [한국어] weight-raising 종료 조건 검사 및 필요 시 즉시 weight 재계산(위 함수 설명 참고) */

	/*
	 * Expire bfqq, pretending that its budget expired, if bfqq
	 * belongs to CLASS_IDLE and other queues are waiting for
	 * service.
	 */
	if (bfq_tot_busy_queues(bfqd) > 1 && bfq_class_idle(bfqq))
		/* [한국어] 다른 busy 큐가 하나 이상 있고, 이 bfqq가 IOPRIO_CLASS_IDLE에 속하는지 확인 */
		bfq_bfqq_expire(bfqd, bfqq, false, BFQQE_BUDGET_EXHAUSTED);
		/* [한국어] IDLE 클래스는 항상 다른 클래스에 양보해야 하므로, budget이 남았어도 "소진된 것처럼" 강제 만료시켜 즉시 다른 큐에 기회를 줌 */

	return rq;
	/* [한국어] dispatch된 request를 호출자(__bfq_dispatch_request)에게 반환 - 이 request가 곧 blk-mq를 거쳐 드라이버로 전달됨 */
}

/*
 * [한국어]
 * bfq_has_work - blk-mq 스케줄러 ops의 has_work 콜백. 이 하드웨어 큐에
 * dispatch할 작업이 남아 있는지 빠르게(락 없이) 판단한다.
 *
 * @hctx: blk-mq 하드웨어 컨텍스트(하나의 SQ/CQ 쌍 또는 CPU 그룹에 대응).
 * @return: dispatch 리스트나 BFQ 내부 큐에 아직 처리할 request가 있으면
 *          true, 완전히 비어 있으면 false.
 *
 * blk-mq는 하드웨어 큐를 idle 상태로 둘지, 아니면 dispatch를 다시
 * 시도할지 판단하기 위해 이 콜백을 자주(락 없이) 호출한다. 그래서 이
 * 함수는 정확성보다 "빠른 근사치"를 우선시하도록 설계되어 있다:
 * bfqd->dispatch 리스트가 비어있지 않거나, bfqd->queued(대기 중인 총
 * 요청 수 카운터)가 0이 아니면 true를 반환한다. 두 값 모두 락 없이
 * 읽으므로(list_empty_careful, READ_ONCE) 다른 CPU가 동시에 갱신 중일
 * 수 있지만, 최악의 경우라도 "일할 게 없는데 dispatch를 한 번 더
 * 시도"하는 정도의 비용만 발생하고 정확성 문제는 생기지 않는다(반대로
 * 거짓 false는 request 유실로 이어질 수 있어 더 위험하므로, 이 경합은
 * 의도적으로 감수된다).
 * 실행 컨텍스트: 락을 잡지 않고 실행되는 lock-less fast path이며,
 * 여러 CPU에서 동시에 호출될 수 있다.
 * NVMe 관점: NVMe controller가 CQ(Completion Queue)에서 완료를 처리한
 * 뒤 해당 hctx에 다시 SQ를 채울 작업이 있는지 신속히 확인하는 용도로
 * 쓰인다.
 *
 * 호출 체인:
 *   blk_mq_has_work → elevator_ops.has_work → [bfq_has_work] → (리프 함수, 하위 호출 없음)
 */
static bool bfq_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct bfq_data *bfqd = hctx->queue->elevator->elevator_data;
	/* [한국어] hctx가 속한 request_queue의 elevator private data에서 BFQ 전역 상태를 얻음 */

	/*
	 * Avoiding lock: a race on bfqd->queued should cause at
	 * most a call to dispatch for nothing
	 */
	return !list_empty_careful(&bfqd->dispatch) ||
		/* [한국어] dispatch 리스트(이미 선택되어 blk-mq에 넘길 준비가 된 request 리스트)가 비어있지 않은지 락 없이 확인 */
		READ_ONCE(bfqd->queued);
		/* [한국어] 또는 BFQ 큐들에 남아있는 전체 대기 request 수(queued)가 0이 아닌지 확인 - 컴파일러 재정렬/캐싱을 막기 위해 READ_ONCE 사용 */
}

/*
 * [한국어]
 * __bfq_dispatch_request - BFQ의 핵심 dispatch 루틴. 이번 dispatch
 * 호출에서 blk-mq에 넘길 request 딱 1개를 골라 반환하고, 그 과정에서
 * per-actuator/전체 inflight 카운터를 증가시킨다.
 *
 * @hctx: dispatch를 요청한 blk-mq 하드웨어 컨텍스트.
 * @return: 이번에 드라이버로 넘길 request. 지금 넘길 게 없으면 NULL.
 *
 * bfq_dispatch_request()가 락을 잡은 상태에서 호출하는 실제 작업
 * 함수다. 두 개의 서로 다른 소스에서 request를 얻을 수 있다:
 * (A) bfqd->dispatch 리스트 - bfq_bfqq_expire() 등에서 이미 "다음에
 *     반드시 내보내야 할 request"로 지정해 둔 것들의 큐. 이 리스트가
 *     비어있지 않으면 최우선으로 여기서 꺼낸다. 이 경로는 표준
 *     bfq_dispatch_rq_from_bfqq() 경로를 거치지 않으므로 dispatched
 *     카운터를 이 함수가 직접 증가시켜야 한다. 이 request가 bfqq에
 *     속하지 않는 경우(RQ_BFQQ(rq)==NULL, 즉 BFQ가 관리하지 않는
 *     elevator-private가 아닌 request)는 tot_rq_in_driver 카운터를
 *     증가시키지 않고 시작만 한다(주석에 설명된 알려진 근사치).
 * (B) 활성 busy 큐가 있으면(bfq_tot_busy_queues > 0), strict_guarantees
 *     모드가 아니거나 현재 in-driver request가 없을 때 bfq_select_queue()
 *     로 큐를 고르고 bfq_dispatch_rq_from_bfqq()로 request를 뽑는다.
 * strict_guarantees 모드는 항상 1개의 request만 드라이버에 남겨두어
 * 서비스 순서를 완벽히 보장하되 처리량을 희생하는 트레이드오프다.
 * 실행 컨텍스트: 호출자 bfq_dispatch_request()가 bfqd->lock을 보유한
 * 상태에서 실행되므로 이 함수는 락을 잡지 않는다.
 * NVMe 관점: 이 함수가 고른 request는 blk_mq_run_hw_queue()를 거쳐
 * nvme_queue_rq()로 전달되고, 뒤이어 nvme_submit_cmd()가 SQ doorbell을
 * 울린다. rq_in_driver[]는 각 actuator(독립 접근 영역)별 in-flight
 * 카운터로, NVMe의 큐 깊이(queue depth) 관리와 유사한 역할을 한다.
 *
 * 호출 체인:
 *   bfq_dispatch_request → [__bfq_dispatch_request] → bfq_select_queue, bfq_dispatch_rq_from_bfqq
 */
static struct request *__bfq_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct bfq_data *bfqd = hctx->queue->elevator->elevator_data;
	/* [한국어] hctx가 속한 request_queue의 elevator private data에서 BFQ 전역 상태 획득 */
	struct request *rq = NULL;
	/* [한국어] 최종적으로 반환할 request - 아직 아무 것도 못 찾았으면 NULL 유지 */
	struct bfq_queue *bfqq = NULL;
	/* [한국어] rq가 속한 bfqq - dispatch 리스트 경로에서는 RQ_BFQQ(rq)로 채워짐 */

	if (!list_empty(&bfqd->dispatch)) {
	/* [한국어] "예약된" dispatch 리스트에 이미 내보낼 request가 대기 중인지 확인 - 있으면 최우선으로 처리 */
		rq = list_first_entry(&bfqd->dispatch, struct request,
				      queuelist);
				      /* [한국어] dispatch 리스트의 첫 번째 request를 획득(FIFO) */
		list_del_init(&rq->queuelist);
		/* [한국어] 리스트에서 제거하고 노드를 초기화 - 더 이상 이 리스트에 속하지 않음을 명시 */

		bfqq = RQ_BFQQ(rq);
		/* [한국어] 이 request가 어떤 bfqq에 속하는지 rq의 elevator private 데이터에서 조회 - BFQ가 관리하지 않는 request면 NULL */

		if (bfqq) {
		/* [한국어] BFQ가 관리하는 bfqq에 속한 request인 경우 */
			/*
			 * Increment counters here, because this
			 * dispatch does not follow the standard
			 * dispatch flow (where counters are
			 * incremented)
			 */
			bfqq->dispatched++;
			/* [한국어] 표준 경로(bfq_dispatch_rq_from_bfqq)를 거치지 않으므로 이 함수가 직접 dispatched 카운터를 증가 - 카운터 불균형 방지 */

			goto inc_in_driver_start_rq;
			/* [한국어] 전체/actuator별 in-driver 카운터 증가와 RQF_STARTED 설정을 공유하기 위해 아래 공통 레이블로 점프 */
		}

		/*
		 * We exploit the bfq_finish_requeue_request hook to
		 * decrement tot_rq_in_driver, but
		 * bfq_finish_requeue_request will not be invoked on
		 * this request. So, to avoid unbalance, just start
		 * this request, without incrementing tot_rq_in_driver. As
		 * a negative consequence, tot_rq_in_driver is deceptively
		 * lower than it should be while this request is in
		 * service. This may cause bfq_schedule_dispatch to be
		 * invoked uselessly.
		 *
		 * As for implementing an exact solution, the
		 * bfq_finish_requeue_request hook, if defined, is
		 * probably invoked also on this request. So, by
		 * exploiting this hook, we could 1) increment
		 * tot_rq_in_driver here, and 2) decrement it in
		 * bfq_finish_requeue_request. Such a solution would
		 * let the value of the counter be always accurate,
		 * but it would entail using an extra interface
		 * function. This cost seems higher than the benefit,
		 * being the frequency of non-elevator-private
		 * requests very low.
		 */
		goto start_rq;
		/* [한국어] bfqq가 없는(elevator-private가 아닌) request는 tot_rq_in_driver 증가 없이 RQF_STARTED만 설정하는 경로로 점프 - 위 주석에 설명된 의도적 근사치 */
	}

	bfq_log(bfqd, "dispatch requests: %d busy queues",
		bfq_tot_busy_queues(bfqd));
		/* [한국어] 디버그 트레이스: 현재 busy(활성) 큐 개수를 기록 */

	if (bfq_tot_busy_queues(bfqd) == 0)
		/* [한국어] 활성 큐가 하나도 없으면(=BFQ가 관리할 I/O가 전혀 없으면) */
		goto exit;
		/* [한국어] 더 볼 것 없이 종료 경로로 점프 - rq는 NULL로 반환됨 */

	/*
	 * Force device to serve one request at a time if
	 * strict_guarantees is true. Forcing this service scheme is
	 * currently the ONLY way to guarantee that the request
	 * service order enforced by the scheduler is respected by a
	 * queueing device. Otherwise the device is free even to make
	 * some unlucky request wait for as long as the device
	 * wishes.
	 *
	 * Of course, serving one request at a time may cause loss of
	 * throughput.
	 */
	if (bfqd->strict_guarantees && bfqd->tot_rq_in_driver > 0)
		/* [한국어] strict_guarantees(엄격한 서비스 순서 보장) 모드이고, 이미 드라이버에 처리 중인 request가 1개 이상 있는지 확인 */
		goto exit;
		/* [한국어] 이미 하나가 진행 중이면 추가로 내보내지 않음 - 드라이버/컨트롤러의 내부 재정렬(reordering)로 서비스 순서가 깨지는 것을 원천 차단, 대신 처리량은 희생 */

	bfqq = bfq_select_queue(bfqd);
	/* [한국어] B-WF2Q+ 스케줄링 로직으로 다음에 서비스할 큐를 선정(budget/injection/actuator 균형 모두 고려) */
	if (!bfqq)
		/* [한국어] 선정된 큐가 없으면(당장 아무 것도 내보낼 수 없으면) */
		goto exit;
		/* [한국어] rq는 NULL 상태로 종료 경로로 점프 */

	rq = bfq_dispatch_rq_from_bfqq(bfqd, bfqq);
	/* [한국어] 선정된 큐에서 실제 request를 꺼내며 budget 차감/weight-raising 갱신까지 함께 수행 */

	if (rq) {
	/* [한국어] request를 실제로 얻었는지 확인(선정된 큐가 이 사이 비워졌을 가능성 등으로 NULL일 수도 있음) */
inc_in_driver_start_rq:
		bfqd->rq_in_driver[bfqq->actuator_idx]++;
		/* [한국어] 이 request가 속한 actuator(독립 접근 영역)의 in-flight 카운터 증가 - 다중 actuator 부하 분산 판단에 사용 */
		bfqd->tot_rq_in_driver++;
		/* [한국어] 전체 in-flight 카운터도 함께 증가 - strict_guarantees 검사 및 peak-rate 추정에 사용 */
start_rq:
		rq->rq_flags |= RQF_STARTED;
		/* [한국어] 이 request가 드라이버로 전달되어 서비스가 시작되었음을 표시 - 이후 완료(completion) 처리 경로에서 이 플래그를 참조 */
	}
exit:
	return rq;
	/* [한국어] 이번 dispatch에서 넘길 request(또는 NULL)를 호출자(bfq_dispatch_request)에게 반환 */
}

/*
 * [한국어]
 * bfq_update_dispatch_stats - (CONFIG_BFQ_CGROUP_DEBUG 빌드에서) 방금
 * dispatch된 request에 대한 blkio cgroup 디버그 통계를 갱신한다.
 *
 * @q: 이 요청이 속한 request_queue.
 * @rq: 방금 __bfq_dispatch_request()가 골라 낸 request(없을 수도 있음, NULL 가능).
 * @in_serv_queue: idle 타이머가 막 비활성화됐을 때만 유효한, 그 시점의
 *                 in-service 큐. idle_timer_disabled가 false면 의미 없음.
 * @idle_timer_disabled: bfq_dispatch_request()에서 이번 dispatch로
 *                        idle 타이머가 막 꺼졌는지 여부.
 * @return: 없음(void). bfqg(BFQ cgroup)의 디버그 통계 필드들을 갱신.
 *
 * bfq_dispatch_request()가 스케줄러 락을 놓은 뒤 호출하는 순수 통계용
 * 후처리 함수다(dispatch 자체의 정확성에는 영향 없음). idle_timer가
 * 비활성화됐다면 그 그룹의 idle-time 통계를 갱신하고, rq가 속한 bfqq가
 * 있다면 평균 큐 크기, "그룹이 비게 된 시각", I/O 제거(remove) 통계를
 * 갱신한다. 함수 진입 시 rq/bfqq는 이미 RQF_STARTED가 설정된 뒤이므로
 * (병합되어 해제될 수 없으므로) 이 함수가 끝날 때까지 유효함이
 * 보장된다는 점이 주석으로 설명되어 있다.
 * 실행 컨텍스트: bfq_dispatch_request()가 bfqd->lock을 이미 놓은 뒤
 * 호출하지만, 이 함수 내부에서 q->queue_lock을 별도로 잡아 blkg 통계
 * 구조체(bfqg)에 대한 동시 접근을 보호한다(cgroup 계층 변경/삭제와의
 * 경합 방지).
 * NVMe 관점: 이 통계는 NVMe 자체 동작에는 관여하지 않고, cgroup별
 * I/O 스케줄링 품질을 사후 분석하기 위한 디버그 전용 부가 정보다.
 *
 * 호출 체인:
 *   bfq_dispatch_request → [bfq_update_dispatch_stats] → bfqg_stats_update_idle_time, bfqg_stats_update_avg_queue_size, bfqg_stats_set_start_empty_time, bfqg_stats_update_io_remove
 */
#ifdef CONFIG_BFQ_CGROUP_DEBUG
static void bfq_update_dispatch_stats(struct request_queue *q,
				      struct request *rq,
				      struct bfq_queue *in_serv_queue,
				      bool idle_timer_disabled)
{
	struct bfq_queue *bfqq = rq ? RQ_BFQQ(rq) : NULL;
	/* [한국어] rq가 있으면 그 rq가 속한 bfqq를 조회, rq가 NULL이면 bfqq도 NULL */

	if (!idle_timer_disabled && !bfqq)
		/* [한국어] idle 타이머도 비활성화되지 않았고 bfqq도 없다면 - 갱신할 통계가 전혀 없는 상황 */
		return;
		/* [한국어] 할 일이 없으므로 락도 잡지 않고 즉시 반환 - 통계 갱신 경로의 오버헤드를 최소화 */

	/*
	 * rq and bfqq are guaranteed to exist until this function
	 * ends, for the following reasons. First, rq can be
	 * dispatched to the device, and then can be completed and
	 * freed, only after this function ends. Second, rq cannot be
	 * merged (and thus freed because of a merge) any longer,
	 * because it has already started. Thus rq cannot be freed
	 * before this function ends, and, since rq has a reference to
	 * bfqq, the same guarantee holds for bfqq too.
	 *
	 * In addition, the following queue lock guarantees that
	 * bfqq_group(bfqq) exists as well.
	 */
	spin_lock_irq(&q->queue_lock);
	/* [한국어] blkg/bfqg 통계 구조체 접근을 보호하기 위해 큐 락 획득 - cgroup 제거 등과의 경합 방지 */
	if (idle_timer_disabled)
		/* [한국어] 이번 dispatch로 idle 타이머가 막 꺼졌다면(=in_serv_queue가 유효한 상황이면) */
		/*
		 * Since the idle timer has been disabled,
		 * in_serv_queue contained some request when
		 * __bfq_dispatch_request was invoked above, which
		 * implies that rq was picked exactly from
		 * in_serv_queue. Thus in_serv_queue == bfqq, and is
		 * therefore guaranteed to exist because of the above
		 * arguments.
		 */
		bfqg_stats_update_idle_time(bfqq_group(in_serv_queue));
		/* [한국어] 그 큐가 속한 cgroup(bfqg)의 idle 소요 시간 통계를 갱신 */
	if (bfqq) {
	/* [한국어] rq가 BFQ가 관리하는 bfqq에 속해 있다면(=elevator-private request라면) */
		struct bfq_group *bfqg = bfqq_group(bfqq);
		/* [한국어] bfqq가 속한 cgroup 그룹 조회 */

		bfqg_stats_update_avg_queue_size(bfqg);
		/* [한국어] 그룹의 평균 큐 크기(대기 중인 요청 수) 통계 갱신 */
		bfqg_stats_set_start_empty_time(bfqg);
		/* [한국어] 그룹이 다시 비게 될 경우를 대비해 "비기 시작한 시각" 기준점을 갱신 */
		bfqg_stats_update_io_remove(bfqg, rq->cmd_flags);
		/* [한국어] 이 request가 BFQ 대기 큐에서 제거(dispatch)되었다는 io_remove 통계를 cmd_flags(읽기/쓰기 등)별로 갱신 */
	}
	spin_unlock_irq(&q->queue_lock);
	/* [한국어] 통계 갱신 완료 - 큐 락 해제 */
}
#else
/*
 * [한국어]
 * bfq_update_dispatch_stats - CONFIG_BFQ_CGROUP_DEBUG가 꺼진 빌드에서는
 * cgroup 디버그 통계 자체가 컴파일되지 않으므로, 호출부 코드를 그대로
 * 두면서도 비용이 전혀 들지 않도록 완전히 빈 인라인 함수로 대체한다.
 *
 * @q, @rq, @in_serv_queue, @idle_timer_disabled: 위 CONFIG_BFQ_CGROUP_DEBUG
 *     버전과 동일한 시그니처를 유지 - 호출부(bfq_dispatch_request)가
 *     빌드 옵션에 따라 분기하지 않아도 되게 하기 위함.
 * @return: 없음. 아무 동작도 하지 않는다.
 *
 * static inline + 빈 본문이므로 컴파일러가 호출 지점에서 완전히
 * 제거(no-op)해 런타임 비용이 0이 된다. 디버그 빌드가 아닌 일반
 * 커널에서 dispatch 핫패스에 불필요한 락/통계 갱신 비용을 주지 않기
 * 위한 전형적인 커널 관용구다.
 *
 * 호출 체인:
 *   bfq_dispatch_request → [bfq_update_dispatch_stats(빈 스텁)] → (없음)
 */
static inline void bfq_update_dispatch_stats(struct request_queue *q,
					     struct request *rq,
					     struct bfq_queue *in_serv_queue,
					     bool idle_timer_disabled) {}
#endif /* CONFIG_BFQ_CGROUP_DEBUG */

/*
 * [한국어]
 * bfq_dispatch_request - blk-mq 스케줄러 ops의 dispatch_request 콜백.
 * bfqd->lock을 잡고 __bfq_dispatch_request()를 호출해 실제 dispatch를
 * 수행한 뒤, idle 타이머가 이번 호출로 비활성화됐는지 판단해 통계
 * 갱신 함수에 넘긴다.
 *
 * @hctx: blk-mq가 dispatch를 요청한 하드웨어 컨텍스트.
 * @return: 이번에 드라이버로 넘길 request(__bfq_dispatch_request의
 *          반환값을 그대로 전달), 없으면 NULL.
 *
 * blk-mq 코어가 하드웨어 큐를 서비스할 때마다 호출하는 최상위
 * 진입점이다. 실제 로직은 모두 __bfq_dispatch_request()에 있고, 이
 * 함수는 (1) bfqd->lock으로 그 로직 전체를 보호하고, (2) dispatch
 * 전후로 in_service_queue와 "새 request를 기다리는 중(wait_request)"
 * 플래그를 비교해 이번 dispatch로 idle 타이머가 새로 꺼졌는지
 * (idle_timer_disabled)를 계산하는 두 가지 부가 역할을 한다. 이
 * idle_timer_disabled 값은 락 해제 후 bfq_update_dispatch_stats()에
 * 전달되어 cgroup 디버그 통계(idle-time)에 반영된다.
 * 실행 컨텍스트: blk-mq dispatch 컨텍스트(보통 소프트 IRQ나 워커
 * 컨텍스트)에서 호출되며, 이 함수 안에서 스핀락으로 임계구역을
 * 보호한다(spin_lock_irq이므로 로컬 인터럽트도 비활성화).
 * NVMe 관점: 이 함수가 반환한 request는 blk-mq를 거쳐 드라이버에
 * enqueue되어 CID를 할당받고, 완료(completion)가 CQ로 돌아오면
 * bfq_finish_requeue_request가 호출되어 BFQ 쪽 통계/스케줄링 상태를
 * 마무리한다.
 *
 * 호출 체인:
 *   blk_mq_dispatch_rq_list → blk_mq_do_dispatch_sched → elevator_ops.dispatch_request →
 *   [bfq_dispatch_request] → __bfq_dispatch_request, bfq_update_dispatch_stats
 */
static struct request *bfq_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct bfq_data *bfqd = hctx->queue->elevator->elevator_data;
	/* [한국어] hctx로부터 BFQ 전역 스케줄러 상태를 획득 */
	struct request *rq;
	/* [한국어] __bfq_dispatch_request()가 돌려줄 request 결과를 담을 변수 */
	struct bfq_queue *in_serv_queue;
	/* [한국어] dispatch 이전 시점의 in-service 큐 스냅샷 - idle 타이머 비활성화 여부 판단의 기준점 */
	bool waiting_rq, idle_timer_disabled = false;
	/* [한국어] waiting_rq: dispatch 이전에 그 큐가 새 request를 기다리며 idle 중이었는지.
	 * idle_timer_disabled: 이번 dispatch 호출로 그 idling이 실제로 해제됐는지(기본값 false) */

	spin_lock_irq(&bfqd->lock);
	/* [한국어] BFQ 스케줄러 전역 상태를 보호하는 락 획득 + 로컬 인터럽트 비활성화 - dispatch 로직 전체를 원자적으로 실행하기 위함 */

	in_serv_queue = bfqd->in_service_queue;
	/* [한국어] dispatch 수행 전의 in-service 큐를 기록 - __bfq_dispatch_request 호출 후 값이 바뀔 수 있으므로 미리 스냅샷 */
	waiting_rq = in_serv_queue && bfq_bfqq_wait_request(in_serv_queue);
	/* [한국어] 그 큐가 존재하고, 새 request 도착을 기다리며 idle 타이머를 걸어둔 상태였는지 확인 */

	rq = __bfq_dispatch_request(hctx);
	/* [한국어] 실제 dispatch 로직 수행 - 이 호출 중에 idle 타이머가 해제될 수 있음(bfq_select_queue 내부에서) */
	if (in_serv_queue == bfqd->in_service_queue) {
	/* [한국어] dispatch 이후에도 in-service 큐가 그대로인지 확인 - 큐가 바뀌었다면 idle 타이머 비교 자체가 무의미하므로 그 경우는 건너뜀 */
		idle_timer_disabled =
			waiting_rq && !bfq_bfqq_wait_request(in_serv_queue);
			/* [한국어] dispatch 전에는 기다리는 중이었는데 dispatch 후에는 더 이상 기다리지 않는다면, 이번 호출로 idle 타이머가 막 꺼진 것으로 판단 */
	}

	spin_unlock_irq(&bfqd->lock);
	/* [한국어] 스케줄러 락 해제 - 이후 통계 갱신은 별도의 큐 락(q->queue_lock)으로 보호됨 */
	bfq_update_dispatch_stats(hctx->queue, rq,
			idle_timer_disabled ? in_serv_queue : NULL,
			/* [한국어] idle 타이머가 실제로 꺼졌을 때만 in_serv_queue를 전달 - 아니면 NULL(통계 함수 내부에서 idle_timer_disabled로 다시 분기) */
				idle_timer_disabled);

	/* [한국어] dispatch된 request(또는 NULL)를 blk-mq 코어에 반환 */
	return rq;
}

/*
 * Task holds one reference to the queue, dropped when task exits.  Each rq
 * in-flight on this queue also holds a reference, dropped when rq is freed.
 *
 * Scheduler lock must be held here. Recall not to use bfqq after calling
 * this function on it.
 */
/*
 * [한국어]
 * bfq_put_queue - bfqq의 참조 카운트(ref)를 1 감소시키고, 0이 되면
 * bfqq를 실제로 해제(kmem_cache_free)한다.
 *
 * @bfqq: 참조를 반납할 bfq_queue. 이 함수 호출 후에는(ref가 0이 되어
 *        해제됐을 수 있으므로) 더 이상 이 포인터를 사용하면 안 된다.
 * @return: 없음(void).
 *
 * bfqq는 여러 주체가 동시에 참조할 수 있는 공유 객체다 - 이 큐를 만든
 * 프로세스(task)가 하나의 참조를 갖고, 이 큐에 속한 각 in-flight
 * request가 각각 하나씩의 참조를 갖으며, 그 외에도 bic(io_cq),
 * stable-merge 캐시, cur_bfqq, waker/woken 관계 등 다양한 자료구조가
 * 포인터를 들고 있는 동안 참조를 유지한다. 이 함수는 그런 참조 하나를
 * "반납"하는 표준 진입점이며, ref가 0에 도달했을 때만(=마지막 참조가
 * 사라졌을 때만) 실제 메모리 해제와 부가 정리 작업을 수행한다:
 * (1) burst_list(짧은 시간에 대량 생성된 프로세스 그룹 추적 리스트)에
 * 남아 있으면 제거하고 burst_size를 보정, (2) 이 큐를 깨웠거나(waker)
 * 이 큐가 깨운(woken) 다른 큐들과의 연결을 모두 끊어 dangling 포인터를
 * 방지, (3) bfqd->last_completed_rq_bfqq가 이 큐를 가리키고 있었다면
 * NULL로 재설정, (4) fifo/sort_list/dispatched가 모두 비어 있어야
 *한다는 불변조건을 WARN_ON_ONCE로 검증(이 시점엔 이 큐에 남은 request가
 * 없어야 정상), (5) 마지막으로 kmem_cache_free()로 bfqq 메모리 자체를
 * bfq_pool에 반납하고, bfqg_and_blkg_put()으로 이 큐가 속했던 cgroup
 * 그룹/blkg에 대한 참조도 함께 반납한다.
 * 실행 컨텍스트: 호출자가 스케줄러 락(bfqd->lock)을 보유한 상태에서
 * 호출해야 한다(주석에 명시). ref 감소 자체는 락 보호 하에 이루어지므로
 * 별도 원자적 연산(atomic_t)이 아니어도 안전하다.
 * NVMe 관점: bfqq가 곧바로 NVMe request pool과 연결된 것은 아니지만,
 * bfqq가 오래 살아남아 해제되지 않으면 관련 request들의 tag 회수가
 * 지연될 수 있어 간접적으로 큐 깊이 관리에 영향을 준다.
 *
 * 호출 체인:
 *   (다수: bfq_exit_bfqq, bfq_put_cooperator, bfq_put_stable_ref 등) → [bfq_put_queue] → kmem_cache_free, bfqg_and_blkg_put
 */
void bfq_put_queue(struct bfq_queue *bfqq)
{
	struct bfq_queue *item;
	/* [한국어] woken_list 순회용 커서 - 이 bfqq가 깨운 다른 큐들을 하나씩 방문 */
	struct hlist_node *n;
	/* [한국어] hlist_for_each_entry_safe에서 사용하는 "다음 노드" 임시 저장 - 순회 중 노드를 제거해도 안전하게 이어가기 위함 */
	struct bfq_group *bfqg = bfqq_group(bfqq);
	/* [한국어] bfqq가 속한 cgroup 그룹을 미리 얻어둠 - bfqq 해제 후에는 bfqq를 통해 조회할 수 없으므로 먼저 캡처 */

	bfq_log_bfqq(bfqq->bfqd, bfqq, "put_queue: %p %d", bfqq, bfqq->ref);
	/* [한국어] 디버그 트레이스: 이 시점의 포인터와 (아직 감소하기 전) 참조 카운트를 기록 */

	bfqq->ref--;
	/* [한국어] 참조 카운트 1 감소 - 스케줄러 락 보호 하에 호출된다는 전제이므로 atomic 연산 없이 일반 감소로 충분 */
	if (bfqq->ref)
		/* [한국어] 감소 후에도 여전히 다른 참조가 남아있는지 확인 */
		return;
		/* [한국어] 아직 참조가 남아있다면 해제하지 않고 즉시 반환 - 다른 소유자가 계속 사용 중 */

	if (!hlist_unhashed(&bfqq->burst_list_node)) {
	/* [한국어] 마지막 참조 해제 확정 - 이 큐가 burst_list(급증 프로세스 그룹 추적)에 아직 걸려 있는지 확인 */
		hlist_del_init(&bfqq->burst_list_node);
		/* [한국어] burst_list에서 이 노드를 제거하고 초기화 - 이제 곧 해제될 큐이므로 더 이상 burst 추적 대상이 아님 */
		/*
		 * Decrement also burst size after the removal, if the
		 * process associated with bfqq is exiting, and thus
		 * does not contribute to the burst any longer. This
		 * decrement helps filter out false positives of large
		 * bursts, when some short-lived process (often due to
		 * the execution of commands by some service) happens
		 * to start and exit while a complex application is
		 * starting, and thus spawning several processes that
		 * do I/O (and that *must not* be treated as a large
		 * burst, see comments on bfq_handle_burst).
		 *
		 * In particular, the decrement is performed only if:
		 * 1) bfqq is not a merged queue, because, if it is,
		 * then this free of bfqq is not triggered by the exit
		 * of the process bfqq is associated with, but exactly
		 * by the fact that bfqq has just been merged.
		 * 2) burst_size is greater than 0, to handle
		 * unbalanced decrements. Unbalanced decrements may
		 * happen in te following case: bfqq is inserted into
		 * the current burst list--without incrementing
		 * bust_size--because of a split, but the current
		 * burst list is not the burst list bfqq belonged to
		 * (see comments on the case of a split in
		 * bfq_set_request).
		 */
		if (bfqq->bic && bfqq->bfqd->burst_size > 0)
			/* [한국어] 병합되지 않은(진짜 프로세스 종료로 해제되는) 큐이고, burst_size가 이미 0보다 큰 경우에만 감소 - 불균형 감소 방지 */
			bfqq->bfqd->burst_size--;
			/* [한국어] burst 크기 카운터 보정 - 짧게 살다 간 프로세스를 "대량 burst"로 오분류하지 않도록 함 */
	}

	/*
	 * bfqq does not exist any longer, so it cannot be woken by
	 * any other queue, and cannot wake any other queue. Then bfqq
	 * must be removed from the woken list of its possible waker
	 * queue, and all queues in the woken list of bfqq must stop
	 * having a waker queue. Strictly speaking, these updates
	 * should be performed when bfqq remains with no I/O source
	 * attached to it, which happens before bfqq gets freed. In
	 * particular, this happens when the last process associated
	 * with bfqq exits or gets associated with a different
	 * queue. However, both events lead to bfqq being freed soon,
	 * and dangling references would come out only after bfqq gets
	 * freed. So these updates are done here, as a simple and safe
	 * way to handle all cases.
	 */
	/* remove bfqq from woken list */
	if (!hlist_unhashed(&bfqq->woken_list_node))
		/* [한국어] 이 bfqq 자신이 다른 어떤 큐의 woken_list(그 큐가 깨운 목록)에 걸려 있는지 확인 */
		hlist_del_init(&bfqq->woken_list_node);
		/* [한국어] 걸려 있다면 제거 - bfqq가 해제된 뒤 그 리스트를 순회하다 댕글링 포인터를 만나는 것을 방지 */

	/* reset waker for all queues in woken list */
	hlist_for_each_entry_safe(item, n, &bfqq->woken_list,
				  woken_list_node) {
				  /* [한국어] 반대로 bfqq가 "깨운" 큐들의 목록(woken_list)을 순회 - safe 변형을 써서 순회 중 제거 가능하게 함 */
		item->waker_bfqq = NULL;
		/* [한국어] 그 큐들 각각에서 "나를 깨워주던 waker가 bfqq였다"는 역참조를 끊음 - bfqq가 사라지므로 더 이상 waker 역할 불가 */
		hlist_del_init(&item->woken_list_node);
		/* [한국어] 해당 큐를 bfqq의 woken_list에서도 제거 - 양방향 연결을 완전히 해소 */
	}

	if (bfqq->bfqd->last_completed_rq_bfqq == bfqq)
		/* [한국어] 전역 상태(bfqd)가 "마지막으로 완료된 request가 속했던 큐"로 이 bfqq를 캐시해두고 있었는지 확인 - waker 자동 탐지에 사용되는 캐시 */
		bfqq->bfqd->last_completed_rq_bfqq = NULL;
		/* [한국어] bfqq가 곧 해제되므로 그 캐시 포인터를 NULL로 재설정해 댕글링 참조 방지 */

	WARN_ON_ONCE(!list_empty(&bfqq->fifo));
	/* [한국어] 불변조건 검증: 이 시점에 fifo 리스트(만료 시간 순 정렬 리스트)가 비어 있어야 정상 - 남아 있다면 버그(아직 처리 안 된 request를 두고 해제하는 상황) */
	WARN_ON_ONCE(!RB_EMPTY_ROOT(&bfqq->sort_list));
	/* [한국어] 불변조건 검증: LBA 정렬 rb-tree도 비어 있어야 함 - 대기 중인 request가 남아있으면 안 됨 */
	WARN_ON_ONCE(bfqq->dispatched);
	/* [한국어] 불변조건 검증: dispatched(드라이버에 전달되었으나 아직 완료 통지 안 된 개수)도 0이어야 함 - 그렇지 않으면 in-flight request를 잃어버리는 버그 */

	kmem_cache_free(bfq_pool, bfqq);
	/* [한국어] 실제 메모리 해제 - bfqq 전용 slab 캐시(bfq_pool)에 반납. 이 시점 이후 bfqq 포인터는 완전히 무효 */
	bfqg_and_blkg_put(bfqg);
	/* [한국어] bfqq가 들고 있던 cgroup 그룹(bfqg)/blkg에 대한 참조도 함께 반납 - bfqq 참조와 그룹 참조가 1:1로 짝지어 관리됨 */
}

/*
 * [한국어]
 * bfq_put_stable_ref - "stable merge" 캐시가 붙잡고 있던 bfqq 참조를
 * 반납한다.
 *
 * @bfqq: stable_ref로 참조를 유지하고 있던 bfq_queue.
 * @return: 없음(void).
 *
 * BFQ는 협조적으로(cooperatively) I/O하는 프로세스들을 자동 병합할 때,
 * 병합 대상 후보를 나중에 다시 찾기 쉽도록 io_cq(bic)에 일정 기간
 * "안정적인(stable)" 참조를 별도로 보관해 둔다(stable_merge_bfqq).
 * 이 함수는 그 안정적 참조를 해제하는 전용 진입점으로, bfqq->stable_ref
 * (안정 참조 전용 카운터)를 감소시킨 뒤 실제 ref 감소/해제는
 * bfq_put_queue()에 위임한다. stable_ref는 bfqq->ref의 부분집합으로,
 * 이 값이 있는 동안에는 bfqq->ref도 그만큼 높게 유지되어 있었다.
 * 실행 컨텍스트: 호출자가 bfqd->lock을 보유한 상태에서 호출.
 *
 * 호출 체인:
 *   _bfq_exit_icq → [bfq_put_stable_ref] → bfq_put_queue
 */
static void bfq_put_stable_ref(struct bfq_queue *bfqq)
{
	bfqq->stable_ref--;
	/* [한국어] "안정적 병합 후보" 전용 참조 카운터 감소 - bfqq->ref와는 별개의 보조 카운터지만 실제 참조는 ref에 합산되어 있음 */
	bfq_put_queue(bfqq);
	/* [한국어] 표준 참조 반납 경로 호출 - bfqq->ref가 0이 되면 이 안에서 실제로 해제됨 */
}

/*
 * [한국어]
 * bfq_put_cooperator - bfqq와 병합(merge) 체인으로 엮여 있던 다른
 * bfqq들에 대한 참조를 모두 반납한다.
 *
 * @bfqq: 병합 체인의 시작점이 되는 bfq_queue(자신이 다른 큐로 병합하기로
 *        예정되어 있었을 수 있는 큐).
 * @return: 없음(void).
 *
 * BFQ는 협조 프로세스들을 하나의 bfqq로 합쳐 서비스하는 큐 병합
 * 최적화를 지원한다(bfq_setup_merge/bfq_merge_bfqqs 참고). 병합이
 * 설정되면 bfqq->new_bfqq가 병합 대상 큐를 가리키며, 그 대상도 참조
 * 카운트가 증가된 채로 유지된다(체인으로 여러 단계 병합될 수도 있음).
 * 이 함수는 그 체인을 new_bfqq 포인터를 따라가며 각 큐에 대해
 * bfq_put_queue()를 호출해 병합 시 얻어둔 참조들을 모두 반납한다.
 * bfqq가 해제되는 시점(bfq_exit_bfqq)에 호출되어, 병합 체인으로 인한
 * 참조 누수를 막는다.
 * 실행 컨텍스트: 호출자가 bfqd->lock을 보유한 상태에서 호출.
 *
 * 호출 체인:
 *   bfq_exit_bfqq → [bfq_put_cooperator] → bfq_put_queue
 */
void bfq_put_cooperator(struct bfq_queue *bfqq)
{
	struct bfq_queue *__bfqq, *next;
	/* [한국어] __bfqq: 병합 체인을 따라가는 순회 커서. next: 해제 전에 다음 노드를 미리 저장(use-after-free 방지) */

	/*
	 * If this queue was scheduled to merge with another queue, be
	 * sure to drop the reference taken on that queue (and others in
	 * the merge chain). See bfq_setup_merge and bfq_merge_bfqqs.
	 */
	__bfqq = bfqq->new_bfqq;
	/* [한국어] bfqq가 병합되기로 예정된 대상 큐부터 시작 - 병합 예정이 없었다면 NULL이라 루프가 아예 돌지 않음 */
	while (__bfqq) {
	/* [한국어] 병합 체인이 끝날 때까지(new_bfqq가 NULL이 될 때까지) 반복 - 다단계 병합을 모두 처리하기 위함 */
		next = __bfqq->new_bfqq;
		/* [한국어] bfq_put_queue 호출로 __bfqq가 해제될 수 있으므로, 그 전에 다음 체인 포인터를 먼저 백업 */
		bfq_put_queue(__bfqq);
		/* [한국어] 병합 시 이 큐에 대해 잡아두었던 참조 하나를 반납 - 다른 참조가 없다면 여기서 실제 해제됨 */
		__bfqq = next;
		/* [한국어] 미리 저장해둔 다음 체인 노드로 이동 */
	}
}

/*
 * [한국어]
 * bfq_exit_bfqq - 프로세스가 종료되거나 다른 큐로 재배정되어 bfqq가
 * 더 이상 필요 없어질 때 호출되는 정리(cleanup) 함수. in-service
 * 상태라면 먼저 만료시키고, 병합 체인/프로세스 참조를 모두 반납한다.
 *
 * @bfqd: BFQ 스케줄러 전역 상태.
 * @bfqq: 종료(exit) 처리할 bfq_queue.
 * @return: 없음(void).
 *
 * io_cq(bic)가 해제되거나(bfq_exit_icq_bfqq) 프로세스가 다른 bfqq로
 * 갈아탈 때 호출되어, 그 프로세스가 이 bfqq에 대해 갖고 있던 연결을
 * 정리한다. 만약 이 bfqq가 하필 지금 in-service 큐라면, 그대로 두고
 * 참조만 반납하면 스케줄러가 이미 사라진 큐를 계속 서비스하려 드는
 * 상태가 되므로, 먼저 __bfq_bfqq_expire()로 강제 만료시키고
 * bfq_schedule_dispatch()로 다른 큐가 뽑히도록 dispatch를 재예약한다.
 * 그 다음 bfq_put_cooperator()로 병합 체인 참조를 반납하고,
 * bfq_release_process_ref()로 이 프로세스(bic)가 쥐고 있던 마지막
 * 참조를 반납한다(이 마지막 호출에서 실제 bfq_put_queue를 거쳐 ref가
 * 0이 되면 kmem_cache_free까지 이어질 수 있음).
 * 실행 컨텍스트: 호출자가 bfqd->lock을 보유한 상태에서 호출.
 * NVMe 관점: in-service 큐를 강제 만료시키는 것은 그 큐가 갖고 있던
 * "다음 SQ 후보"로서의 지위를 즉시 박탈하고 다른 프로세스의 request가
 * 대신 선택되게 하는 것과 같다.
 *
 * 호출 체인:
 *   bfq_exit_icq_bfqq → [bfq_exit_bfqq] → __bfq_bfqq_expire, bfq_schedule_dispatch, bfq_put_cooperator, bfq_release_process_ref
 */
static void bfq_exit_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	if (bfqq == bfqd->in_service_queue) {
	/* [한국어] 지금 해제하려는 큐가 하필 현재 서비스 중인 큐인지 확인 - 그렇다면 먼저 안전하게 내려놓아야 함 */
		__bfq_bfqq_expire(bfqd, bfqq, BFQQE_BUDGET_TIMEOUT);
		/* [한국어] budget timeout 사유로 강제 만료 처리 - in_service_queue 지위를 내려놓고 타임스탬프를 정리 */
		bfq_schedule_dispatch(bfqd);
		/* [한국어] 다른 큐가 새로 선택될 수 있도록 dispatch를 재예약 - 이 큐가 사라져도 서비스가 멈추지 않게 함 */
	}

	bfq_log_bfqq(bfqd, bfqq, "exit_bfqq: %p, %d", bfqq, bfqq->ref);
	/* [한국어] 디버그 트레이스: 종료 처리 시작 시점의 포인터와 현재 참조 카운트 기록 */

	bfq_put_cooperator(bfqq);
	/* [한국어] 병합 체인으로 엮여 있던 다른 큐들에 대한 참조 반납 */

	bfq_release_process_ref(bfqd, bfqq);
	/* [한국어] 이 프로세스(io_cq)가 bfqq에 대해 갖고 있던 마지막 참조를 반납 - 내부적으로 bfq_put_queue를 호출하며, 다른 참조가 전혀 없었다면 여기서 실제 메모리 해제까지 이어짐 */
}

/*
 * [한국어]
 * bfq_exit_icq_bfqq - 특정 io_cq(bic)가 특정 actuator/동기·비동기
 * 조합에 대해 갖고 있는 bfqq 연결을 끊고 그 bfqq를 종료 처리한다.
 *
 * @bic: 프로세스별 io_context에 대응하는 BFQ 확장 구조체(bfq_io_cq).
 * @is_sync: true면 동기(sync) I/O용 bfqq, false면 비동기(async) bfqq를 대상으로 함.
 * @actuator_idx: 다중 actuator 장치에서 대상 actuator의 인덱스.
 * @return: 없음(void).
 *
 * 하나의 io_cq는 actuator마다, 그리고 sync/async마다 별도의 bfqq를
 * bic->bfqq[is_sync][actuator_idx] 형태(bic_to_bfqq로 조회)로 들고 있을
 * 수 있다. 이 함수는 그 중 하나의 슬롯에 대해, (1) 실제로 연결된 bfqq가
 * 있는지 확인하고, (2) 그 bfqq가 속한 bfqd(스케줄러 전역 상태)가 아직
 * 살아있는지 확인한 뒤(스케줄러 자체가 이미 종료됐다면 bfqd는 NULL),
 * (3) 살아있다면 bic_set_bfqq()로 이 슬롯의 포인터를 NULL로 지워 연결을
 * 끊고, bfq_exit_bfqq()를 호출해 그 bfqq의 종료(참조 반납/필요시 만료)
 * 처리를 수행한다. bfqd가 이미 NULL이면(스케줄러가 먼저 사라진 경우)
 * bfqq 관련 정리는 이미 다른 경로(스케줄러 exit)에서 끝났다고 보고 아무
 * 것도 하지 않는다.
 * 실행 컨텍스트: 호출자 _bfq_exit_icq()가 필요 시 bfqd->lock을 잡은
 * 상태에서 호출.
 *
 * 호출 체인:
 *   _bfq_exit_icq → [bfq_exit_icq_bfqq] → bic_set_bfqq, bfq_exit_bfqq
 */
static void bfq_exit_icq_bfqq(struct bfq_io_cq *bic, bool is_sync,
			      unsigned int actuator_idx)
{
	struct bfq_queue *bfqq = bic_to_bfqq(bic, is_sync, actuator_idx);
	/* [한국어] 이 io_cq가 (is_sync, actuator_idx) 조합에 대해 들고 있는 bfqq 포인터 조회 - 없으면 NULL */
	struct bfq_data *bfqd;
	/* [한국어] bfqq가 속한 스케줄러 전역 상태 - 스케줄러가 이미 종료됐다면 아래에서 NULL로 남을 수 있음(초기화 안 됨에 유의, bfqq가 NULL이면 참조하지 않음) */

	if (bfqq)
		/* [한국어] 연결된 bfqq가 실제로 있는 경우에만 */
		bfqd = bfqq->bfqd; /* NULL if scheduler already exited */
		/* [한국어] bfqq를 통해 bfqd를 얻음 - 스케줄러가 이미 종료 처리 중이라면 bfqq->bfqd 자체가 NULL일 수 있음 */

	if (bfqq && bfqd) {
	/* [한국어] bfqq도 있고 소속 스케줄러(bfqd)도 아직 유효한 경우에만 정리 수행 */
		bic_set_bfqq(bic, NULL, is_sync, actuator_idx);
		/* [한국어] 이 io_cq 슬롯의 bfqq 포인터를 NULL로 설정 - 이후 이 프로세스가 새 I/O를 내면 새 bfqq를 다시 만들게 됨 */
		bfq_exit_bfqq(bfqd, bfqq);
		/* [한국어] 실제 종료 처리 위임 - in-service였다면 만료시키고, 병합/프로세스 참조를 반납 */
	}
}

/*
 * [한국어]
 * _bfq_exit_icq - 하나의 io_cq(bic)에 딸린 모든 actuator × sync/async
 * 조합의 bfqq들과, stable-merge 캐시 참조를 전부 정리한다.
 *
 * @bic: 종료 처리할 io_context의 BFQ 확장 구조체.
 * @num_actuators: 순회할 actuator 개수(호출자가 상황에 맞게 결정해 전달).
 * @return: 없음(void).
 *
 * bfq_exit_icq()의 실제 작업 본체다. 각 actuator 인덱스에 대해:
 * (1) 그 actuator 슬롯에 안정적 병합 후보로 캐시해 둔 stable_merge_bfqq가
 * 있으면 bfq_put_stable_ref()로 그 참조부터 반납하고, (2) 동기(sync)
 * bfqq와 (3) 비동기(async) bfqq 각각에 대해 bfq_exit_icq_bfqq()를 호출해
 * 연결을 끊고 종료 처리한다. num_actuators를 인자로 받는 이유는, 호출
 * 시점에 bfqd(따라서 실제 actuator 개수)가 이미 사라졌을 수도 있어서,
 * 그런 경우 호출자가 안전하게 "가능한 최대값"(BFQ_MAX_ACTUATORS)을
 * 넘겨 미사용 슬롯까지 포함해 전부 훑게 하기 위함이다(bic은 생성 시
 * 0으로 초기화되므로 미사용 슬롯은 자연히 NULL).
 * 실행 컨텍스트: 호출자가 필요 시 bfqd->lock을 잡은 상태에서 호출.
 *
 * 호출 체인:
 *   bfq_exit_icq → [_bfq_exit_icq] → bfq_put_stable_ref, bfq_exit_icq_bfqq
 */
static void _bfq_exit_icq(struct bfq_io_cq *bic, unsigned int num_actuators)
{
	struct bfq_iocq_bfqq_data *bfqq_data = bic->bfqq_data;
	/* [한국어] 이 io_cq의 actuator별 bfqq 관련 부가 데이터 배열(stable_merge_bfqq 등)에 대한 포인터 */
	unsigned int act_idx;
	/* [한국어] 0..num_actuators-1 범위를 순회하는 인덱스 */

	for (act_idx = 0; act_idx < num_actuators; act_idx++) {
	/* [한국어] 호출자가 지정한 개수만큼(실제 actuator 수 또는 최대 가능 수) 모든 슬롯을 순회 */
		if (bfqq_data[act_idx].stable_merge_bfqq)
			/* [한국어] 이 actuator 슬롯에 안정 병합 후보로 캐시된 bfqq가 있는지 확인 */
			bfq_put_stable_ref(bfqq_data[act_idx].stable_merge_bfqq);
			/* [한국어] 있다면 그 안정 참조부터 먼저 반납 - 일반 sync/async 슬롯 정리보다 먼저 처리 */

		bfq_exit_icq_bfqq(bic, true, act_idx);
		/* [한국어] 이 actuator의 동기(sync) I/O용 bfqq 연결 해제 및 종료 처리 */
		bfq_exit_icq_bfqq(bic, false, act_idx);
		/* [한국어] 이 actuator의 비동기(async) I/O용 bfqq 연결 해제 및 종료 처리 */
	}
}

/*
 * [한국어]
 * bfq_exit_icq - elevator ops의 icq(io context) exit 콜백. 프로세스의
 * io_context가 소멸될 때(또는 elevator가 교체될 때) 호출되어, 그
 * 프로세스와 연결된 모든 bfqq를 정리한다.
 *
 * @icq: 코어 블록 계층의 io_cq 객체(BFQ 확장 필드 포함).
 * @return: 없음(void).
 *
 * 커널의 io_cq 서브시스템이 어떤 프로세스의 io_context가 더 이상
 * 필요 없어졌다고 판단하면 이 콜백을 호출한다. bic_to_bfqd()로 이
 * icq가 속했던 bfqd를 조회하는데, 이미 스케줄러 자체가 exit 중이라면
 * bfqd가 NULL일 수 있다(주석에 설명된 대로, bic은 생성 시 0으로
 * 초기화되므로 이 경우에도 미사용 필드는 안전하게 NULL로 남아있다).
 * bfqd가 살아있으면 그 lock을 잡고 실제 actuator 개수만큼만
 * _bfq_exit_icq()를 호출하며, bfqd가 이미 사라졌다면 락 없이(어차피
 * 스케줄러 데이터 구조가 없으므로) BFQ_MAX_ACTUATORS(가능한 최대
 * actuator 수)만큼 순회해 모든 슬롯을 훑는다 - 이것이 이 큐들에
 * 접근하는 마지막 기회이기 때문이다.
 * 실행 컨텍스트: 프로세스/io_context 소멸 경로에서 호출되며, bfqd가
 * 존재하는 동안은 spin_lock_irqsave로 스케줄러 락을 잡아 dispatch 등
 * 다른 경로와의 경합을 막는다.
 *
 * 호출 체인:
 *   (블록 계층 io_cq 소멸 경로, elevator_ops.exit_icq) → [bfq_exit_icq] → _bfq_exit_icq
 */
static void bfq_exit_icq(struct io_cq *icq)
{
	struct bfq_io_cq *bic = icq_to_bic(icq);
	/* [한국어] 범용 io_cq를 BFQ 확장 타입으로 변환(container_of 패턴) */
	struct bfq_data *bfqd = bic_to_bfqd(bic);
	/* [한국어] 이 io_cq가 속했던 BFQ 스케줄러 전역 상태 조회 - 스케줄러가 이미 사라졌으면 NULL */
	unsigned long flags;
	/* [한국어] spin_lock_irqsave/irqrestore에 쓰일 인터럽트 상태 저장용 */

	/*
	 * If bfqd and thus bfqd->num_actuators is not available any
	 * longer, then cycle over all possible per-actuator bfqqs in
	 * next loop. We rely on bic being zeroed on creation, and
	 * therefore on its unused per-actuator fields being NULL.
	 *
	 * bfqd is NULL if scheduler already exited, and in that case
	 * this is the last time these queues are accessed.
	 */
	if (bfqd) {
	/* [한국어] 스케줄러가 아직 살아있는 경우 */
		spin_lock_irqsave(&bfqd->lock, flags);
		/* [한국어] 스케줄러 락 획득(+인터럽트 저장) - dispatch/completion 등 다른 경로와의 동시 접근을 막기 위함 */
		_bfq_exit_icq(bic, bfqd->num_actuators);
		/* [한국어] 실제 actuator 개수만큼만 순회하며 정리 - bfqd가 알고 있는 정확한 값 사용 */
		spin_unlock_irqrestore(&bfqd->lock, flags);
		/* [한국어] 락 해제 및 인터럽트 상태 복원 */
	} else {
	/* [한국어] 스케줄러가 이미 종료되어 bfqd를 알 수 없는 경우 */
		_bfq_exit_icq(bic, BFQ_MAX_ACTUATORS);
		/* [한국어] 락 없이(스케줄러 데이터 구조 자체가 없으므로 경합 대상도 없음) 가능한 최대 actuator 수만큼 순회 - bic이 0으로 초기화되어 있으므로 미사용 슬롯은 안전하게 건너뜀 */
	}
}

/*
 * Update the entity prio values; note that the new values will not
 * be used until the next (re)activation.
 */
/*
 * [한국어]
 * bfq_set_next_ioprio_data - ionice(IOPRIO_CLASS_RT/BE/IDLE 및 레벨)
 * 값을 BFQ 내부 가중치(weight)로 변환해 bfqq->entity.new_weight에
 * 저장한다. "new_" 접두사가 붙은 필드들은 즉시 적용되지 않고, 다음
 * (재)활성화 시점에 실제 weight/ioprio로 반영된다.
 *
 * @bfqq: ioprio를 갱신할 대상 bfq_queue.
 * @bic: 이 프로세스의 io_context에 저장된 현재 ioprio 값(bic->ioprio)을
 *       담고 있는 구조체.
 * @return: 없음(void). bfqq->new_ioprio, new_ioprio_class,
 *          entity.new_weight, entity.prio_changed를 갱신.
 *
 * 프로세스가 ioprio_set() 등으로 자신의 I/O 우선순위를 바꾸거나, 새
 * bfqq가 처음 만들어질 때 호출되어 리눅스의 3단계 ionice 클래스
 * (IOPRIO_CLASS_RT 실시간, IOPRIO_CLASS_BE 최선-노력, IOPRIO_CLASS_IDLE
 * 유휴시에만)와 클래스 내 레벨을 BFQ의 단일 weight 값으로 매핑한다.
 * IOPRIO_CLASS_NONE(설정 안 함)이거나 알 수 없는 클래스(default: 커널
 * 로그로 에러 출력 후 fallthrough)라면 CPU 스케줄링 nice 값으로부터
 * ioprio를 유추한다(task_nice_ioprio/task_nice_ioclass). 계산된
 * new_ioprio가 유효 범위(IOPRIO_NR_LEVELS)를 벗어나면(이런 일은
 * 정상적으로는 발생하지 않아야 함) 커널 크리티컬 로그를 남기고 가장
 * 낮은 우선순위로 강제 보정한다. 마지막으로 bfq_ioprio_to_weight()로
 * 실제 weight 수치를 계산해 저장하고, entity.prio_changed = 1로
 * 표시해 스케줄러가 다음 기회에 이 변경을 실제로 반영하도록 한다.
 * 실행 컨텍스트: 호출자가 bfqd->lock을 보유한 상태에서 호출된다고
 * 가정. bfqd가 NULL이면(스케줄러가 이미 종료 중이면) 아무 것도 하지
 * 않고 조기 반환한다.
 * NVMe 관점: 여기서 계산되는 weight는 B-WF2Q+ 스케줄러가 이 bfqq에
 * 얼마나 자주/많이 SQ 슬롯을 배정할지를 정하는 소프트웨어 계층의
 * 값이며, NVMe 커맨드 자체의 우선순위 필드나 CID 순서와는 무관하다.
 *
 * 호출 체인:
 *   bfq_check_ioprio_change (다른 에이전트 담당 구간) → [bfq_set_next_ioprio_data] → bfq_ioprio_to_weight
 */
static void
bfq_set_next_ioprio_data(struct bfq_queue *bfqq, struct bfq_io_cq *bic)
{
	struct task_struct *tsk = current;
	/* [한국어] 현재 실행 중인(이 ioprio 변경을 유발한) 태스크 - IOPRIO_CLASS_NONE일 때 nice 값을 얻는 데 사용 */
	int ioprio_class;
	/* [한국어] bic->ioprio에서 추출한 ionice 클래스(RT/BE/IDLE/NONE) */
	struct bfq_data *bfqd = bfqq->bfqd;
	/* [한국어] bfqq가 속한 스케줄러 전역 상태 - 로그 출력 등에 사용 */

	if (!bfqd)
		/* [한국어] 스케줄러가 이미 종료 중이어서 bfqd를 알 수 없는 경우 */
		return;
		/* [한국어] 더 이상 의미 있는 갱신을 할 수 없으므로 조기 반환 */

	ioprio_class = IOPRIO_PRIO_CLASS(bic->ioprio);
	/* [한국어] bic에 저장된 원본 ioprio 값에서 상위 비트인 클래스 부분만 추출(IOPRIO_PRIO_CLASS 매크로) */
	switch (ioprio_class) {
	default:
		/* [한국어] 알려지지 않은(잘못된) ioprio 클래스 값인 경우 */
		pr_err("bdi %s: bfq: bad prio class %d\n",
			bdi_dev_name(bfqq->bfqd->queue->disk->bdi),
			ioprio_class);
			/* [한국어] 어떤 블록 장치(bdi 이름)에서 잘못된 클래스 값이 들어왔는지 커널 로그에 에러로 기록 */
		fallthrough;
		/* [한국어] 에러를 기록한 뒤 IOPRIO_CLASS_NONE과 동일하게 처리하도록 의도적으로 다음 case로 흘러감(fallthrough 명시로 컴파일러 경고 방지) */
	case IOPRIO_CLASS_NONE:
		/*
		 * No prio set, inherit CPU scheduling settings.
		 */
		bfqq->new_ioprio = task_nice_ioprio(tsk);
		/* [한국어] 명시적 ionice 설정이 없으므로 CPU 스케줄링 nice 값으로부터 상응하는 ioprio 레벨을 유추 */
		bfqq->new_ioprio_class = task_nice_ioclass(tsk);
		/* [한국어] 마찬가지로 nice 값 기반으로 ioprio 클래스(보통 BE)를 유추 */
		break;
	/* [한국어] 실시간(RT) 클래스 - bic에 저장된 원본 값에서 레벨(하위 비트)만 추출 */
	case IOPRIO_CLASS_RT:
		bfqq->new_ioprio = IOPRIO_PRIO_LEVEL(bic->ioprio);
		/* [한국어] 클래스를 RT로 확정 - BFQ에서 가장 높은 우선순위/가중치로 매핑됨 */
		bfqq->new_ioprio_class = IOPRIO_CLASS_RT;
		break;
	/* [한국어] 최선-노력(Best-Effort) 클래스의 레벨 값 추출 - 대부분의 일반 프로세스가 이 클래스 */
	case IOPRIO_CLASS_BE:
		bfqq->new_ioprio = IOPRIO_PRIO_LEVEL(bic->ioprio);
		bfqq->new_ioprio_class = IOPRIO_CLASS_BE; // 클래스를 BE로 확정 - RT와 IDLE 사이의 중간 서비스 트리에 배치된다
		break;
	/* [한국어] 유휴시에만(IDLE) 클래스로 확정 - 다른 모든 클래스에 항상 양보 */
	case IOPRIO_CLASS_IDLE:
		bfqq->new_ioprio_class = IOPRIO_CLASS_IDLE;
		/* [한국어] IDLE 클래스는 레벨 개념이 없으므로 가장 낮은(최후순위) 레벨 값으로 고정 */
		bfqq->new_ioprio = IOPRIO_NR_LEVELS - 1;
		break;
	}

	/* [한국어] 위 계산 결과가 유효한 레벨 범위(0..IOPRIO_NR_LEVELS-1)를 벗어났는지 검증 - 정상 흐름에서는 발생하면 안 되는 방어적 코드 */
	if (bfqq->new_ioprio >= IOPRIO_NR_LEVELS) {
		pr_crit("bfq_set_next_ioprio_data: new_ioprio %d\n",
			/* [한국어] 커널 크리티컬 로그로 비정상 값을 기록 - 디버깅을 위한 흔적 남기기 */
			bfqq->new_ioprio);
		/* [한국어] 범위를 벗어난 값을 가장 낮은 우선순위로 강제 보정해 이후 배열 인덱싱 등에서 out-of-bounds가 나지 않도록 함 */
		bfqq->new_ioprio = IOPRIO_NR_LEVELS - 1;
	}

	/* [한국어] 확정된 레벨 값을 BFQ의 실제 스케줄링 가중치(weight) 수치로 변환해 저장 - 값이 클수록 더 많은 서비스를 받음 */
	bfqq->entity.new_weight = bfq_ioprio_to_weight(bfqq->new_ioprio);
	/* [한국어] 디버그 트레이스: 변경된 ioprio 레벨과 그에 대응하는 weight를 기록 */
	bfq_log_bfqq(bfqd, bfqq, "new_ioprio %d new_weight %d",
		     bfqq->new_ioprio, bfqq->entity.new_weight);
	/* [한국어] "적용 대기 중인 우선순위 변경이 있다"는 플래그 설정 - 다음 활성화(재-스케줄링) 시점에 이 new_weight/new_ioprio가 실제 weight/ioprio로 반영됨 */
	bfqq->entity.prio_changed = 1;
}

/* [한국어] bfq_get_queue()의 전방 선언. 정의는 파일 뒤쪽에 있으나
 * bfq_check_ioprio_change()가 ioprio 변경을 감지하면 기존 async 큐를
 * 버리고 새 우선순위에 맞는 큐를 다시 얻어야 하므로, 그보다 앞선
 * 이 위치에서 선언이 필요하다. respawn 인자는 "기존 큐를 재사용하지
 * 말고 새로 만들라"는 뜻으로, 바로 이 ioprio 변경 경로에서 true가 된다. */
static struct bfq_queue *bfq_get_queue(struct bfq_data *bfqd,
				       struct bio *bio, bool is_sync,
				       struct bfq_io_cq *bic,
				       bool respawn);

/*
 * [한국어]
 * bfq_check_ioprio_change - bic(bfq_io_cq)에 캐시된 ioprio와 태스크의
 * 현재 ioprio(ioc->ioprio)가 어긋났는지 검사하고, 바뀌었다면 async bfqq
 * 재획득과 sync bfqq 가중치 재계산을 수행한다.
 *
 * @bic: 이 프로세스(io_context)와 bfqd를 연결하는 bfq_io_cq. ioprio 캐시(bic->ioprio)를 보유.
 * @bio: 지금 처리 중인 bio. actuator index 계산(bfq_actuator_index)에 쓰인다.
 * @return: 없음(void). 부작용으로 bic->ioprio, 그리고 bic가 가리키는 async/sync
 *          bfqq의 entity.new_weight/new_ioprio 등이 갱신된다.
 *
 * ionice(2)로 IOPRIO_CLASS_RT/BE/IDLE 이나 IOPRIO_PRIO_DATA(우선순위 레벨)가
 * 바뀌면 그 값은 프로세스의 io_context->ioprio 필드에만 즉시 반영되고,
 * 이미 만들어져 있는 bfq_queue의 entity 가중치는 자동으로 바뀌지 않는다.
 * 이 함수는 새 request가 들어올 때마다(bfq_init_rq 경로) 호출되어, 캐시된
 * ioprio와 실제 ioprio를 비교하고 달라졌으면 관련 bfqq들을 갱신한다.
 * async bfqq는 ioprio_class/ioprio 조합별로 bfqg에 캐시되어 공유되므로,
 * ioprio가 바뀌면 기존 async bfqq를 버리고 새 조합에 해당하는 async bfqq를
 * 다시 얻어와야 한다(bfq_get_queue). 반면 sync bfqq는 프로세스 전용이라
 * 교체할 필요 없이 entity.new_weight만 재계산하면 된다
 * (bfq_set_next_ioprio_data).
 * 실행 컨텍스트: 이 bic를 소유한 프로세스의 제출 경로에서 bfqd->lock을
 * 잡은 채로 호출되므로 재진입 걱정은 없다.
 * caller: bfq_init_rq(). callee: bic_to_bfqq(), bfq_get_queue(),
 * bic_set_bfqq(), bfq_release_process_ref(), bfq_set_next_ioprio_data().
 *
 * 호출 체인:
 *   bfq_init_rq() → [bfq_check_ioprio_change] → bfq_get_queue() / bfq_set_next_ioprio_data()
 */
static void bfq_check_ioprio_change(struct bfq_io_cq *bic, struct bio *bio)
{
	struct bfq_data *bfqd = bic_to_bfqd(bic); // bic로부터 이 프로세스가 속한 bfq_data(스케줄러 전역 상태)를 역참조.
	struct bfq_queue *bfqq; // 이 함수 안에서 async bfqq -> sync bfqq 순으로 재사용되는 임시 포인터.
	int ioprio = bic->icq.ioc->ioprio; // io_context에 설정된 "현재" ioprio(ionice로 바뀔 수 있는 값) 스냅샷.

	/*
	 * This condition may trigger on a newly created bic, be sure to
	 * drop the lock before returning.
	 */
	/* [한국어] bfqd가 아직 연결되지 않은 갓 생성된 bic이거나, 캐시된 ioprio와
	 * 실제 값이 같아(가장 흔한 경우) 갱신할 것이 없으므로 즉시 반환.
	 * likely/unlikely 배치에 주목: ionice 변경은 극히 드물기 때문에 이
	 * 조기 반환이 정상 경로가 되도록 분기 예측을 유도한다. */
	if (unlikely(!bfqd) || likely(bic->ioprio == ioprio))
		return;

	bic->ioprio = ioprio; // 새 ioprio를 bic에 캐시해, 다음 호출부터는 이 값과 비교해 중복 갱신을 피한다.

	bfqq = bic_to_bfqq(bic, false, bfq_actuator_index(bfqd, bio)); // 이 bic·actuator에 연결된 async(쓰기-지연 등 비동기) bfqq를 조회.
	if (bfqq) { // 이미 async bfqq를 갖고 있었다면, 그 bfqq는 옛 ioprio 조합 슬롯에 캐시된 것이므로 새 ioprio에 맞는 것으로 바꿔야 한다.
		struct bfq_queue *old_bfqq = bfqq; // 교체 전 옛 bfqq를 보관해 두었다가 아래에서 참조를 반납.

		bfqq = bfq_get_queue(bfqd, bio, false, bic, true); // 새 ioprio_class/ioprio 조합에 대응하는 async bfqq를 획득(없으면 새로 할당). respawn=true라 stable merge 시도는 생략.
		bic_set_bfqq(bic, bfqq, false, bfq_actuator_index(bfqd, bio)); // bic가 이 actuator에 대해 가리키는 async bfqq 포인터를 새 bfqq로 교체.
		bfq_release_process_ref(bfqd, old_bfqq); // 옛 async bfqq에 걸려 있던 이 프로세스 몫의 참조를 반납(ref--; 0이 되면 해제 경로로 진입).
	}

	bfqq = bic_to_bfqq(bic, true, bfq_actuator_index(bfqd, bio)); // 이번엔 sync(동기 I/O) bfqq를 조회 - sync bfqq는 프로세스 전용이라 교체가 아니라 가중치만 갱신하면 된다.
	if (bfqq) // sync bfqq가 존재하는 일반적인 경우
		bfq_set_next_ioprio_data(bfqq, bic); // 새 ioprio를 바탕으로 entity.new_ioprio/new_weight를 재계산하고 prio_changed=1로 표시(다음 재활성화 시 실제 weight에 반영).
}

/*
 * [한국어]
 * bfq_init_bfqq - 새로 할당된 bfq_queue(bfqq)의 모든 필드를 초기값으로
 * 채워, 스케줄링/가중치/워크로드 추정에 곧바로 쓰일 수 있는 상태로 만든다.
 *
 * @bfqd: 이 bfqq가 속하게 될 스케줄러 전역 상태(bfq_data).
 * @bfqq: 방금 kmem_cache_alloc_node()로 할당된, 아직 초기화되지 않은 bfq_queue.
 * @bic: 이 bfqq를 발생시킨 프로세스의 bfq_io_cq. NULL이면(예: oom_bfqq) ioprio
 *       초기화를 건너뛴다.
 * @pid: bfqq를 소유하는 프로세스의 pid(디버그 로그/식별용).
 * @is_sync: 동기 I/O 큐(1)인지 비동기/공유 큐(0)인지.
 * @act_idx: 이 bfqq가 담당할 actuator(다중 액추에이터 SSD/HDD의 독립 병렬
 *           접근 영역) 인덱스.
 * @return: 없음(void). bfqq의 모든 필드를 in-place로 채운다.
 *
 * bfq_get_queue()가 새 bfqq를 kmem_cache로 할당한 직후 반드시 호출되어,
 * entity/타이머/통계 관련 필드들을 안전한 초기값으로 맞춘다. 특히
 * ttime.last_end_request를 "지금+1"로 두어 최초 request가 도착하기 전까지
 * think-time 계산이 터무니없이 커지지 않게 하고, seek_history=1로 두어
 * 첫 request는 일단 seek(탐색)한 것으로 간주한다(참고 데이터가 없으니
 * 보수적으로 처리).
 * 실행 컨텍스트: bfq_get_queue() 내부에서 bfqd->lock을 쥔 채로 호출된다.
 * caller: bfq_get_queue(). callee: bfq_set_next_ioprio_data(),
 * bfq_class_idle(), bfq_mark_bfqq_*(), bfq_max_budget(), bfq_smallest_from_now().
 *
 * 호출 체인:
 *   bfq_get_queue() → [bfq_init_bfqq] → bfq_set_next_ioprio_data()
 */
static void bfq_init_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			  struct bfq_io_cq *bic, pid_t pid, int is_sync,
			  unsigned int act_idx)
{
	u64 now_ns = blk_time_get_ns(); // 초기화 시각을 나노초 단조 시계로 기록 - ttime/io_start_time 계산의 기준점.

	bfqq->actuator_idx = act_idx; // 이 bfqq가 담당하는 다중 액추에이터 인덱스를 고정 - 이후 rq_in_driver[]/weights_tree 등이 이 인덱스별로 분리 관리된다.
	RB_CLEAR_NODE(&bfqq->entity.rb_node); // entity가 아직 어떤 서비스 rb-tree에도 연결되지 않았음을 표시 - 활성화(bfq_activate_bfqq) 전 상태.
	INIT_LIST_HEAD(&bfqq->fifo); // rq->queuelist가 매달릴 FIFO 리스트 헤드를 자기 자신을 가리키는 빈 리스트로 초기화 - fifo_expire 타임아웃 판단에 사용.
	INIT_HLIST_NODE(&bfqq->burst_list_node); // burst(동시 다발 큐 생성) 감지용 해시리스트 노드 초기화 - 아직 어떤 burst 리스트에도 엮여 있지 않다.
	INIT_HLIST_NODE(&bfqq->woken_list_node); // 이 bfqq가 다른 큐를 "깨우는(waker)" 관계로 엮일 때 쓰는 리스트 노드 초기화.
	INIT_HLIST_HEAD(&bfqq->woken_list); // 반대로 이 bfqq가 깨운 큐들의 목록 헤드 초기화 - 아직 아무도 깨우지 않은 상태.

	bfqq->ref = 0; // 참조 카운트를 0으로 시작 - 아래에서 bfq_get_queue()의 out: 라벨이 process reference를 +1 해 실질적으로 소유권을 부여한다.
	bfqq->bfqd = bfqd; // 이 bfqq가 속한 스케줄러 인스턴스를 역참조로 고정 - 이후 bfqd->lock/bfqd->queue 등에 접근할 때 사용.

	if (bic) // oom_bfqq처럼 특정 프로세스에 종속되지 않는 bfqq는 bic가 NULL일 수 있다.
		bfq_set_next_ioprio_data(bfqq, bic); // bic의 현재 ioprio를 반영해 entity.new_ioprio/new_weight를 최초 계산.

	if (is_sync) { // 동기 큐(포그라운드 프로세스가 직접 낸 read/write)인 경우
		/*
		 * No need to mark as has_short_ttime if in
		 * idle_class, because no device idling is performed
		 * for queues in idle class
		 */
		if (!bfq_class_idle(bfqq)) // IOPRIO_CLASS_IDLE이 아니라면(idle 클래스는 애초에 idling을 하지 않으므로 이 표시가 무의미)
			/* tentatively mark as has_short_ttime */
			bfq_mark_bfqq_has_short_ttime(bfqq); // 일단 "생각 시간이 짧다(interactive 가능성)"고 낙관적으로 표시 - 실측이 쌓이면 bfq_update_has_short_ttime()이 정정한다.
		bfq_mark_bfqq_sync(bfqq); // BFQQF_sync 플래그 설정 - 동기 큐로 등록되어 idling/burst 등의 정책 대상이 됨.
		bfq_mark_bfqq_just_created(bfqq); // "방금 생성됨" 플래그 설정 - stable-merge 판단(bfq_do_or_sched_stable_merge) 등에서 신생 큐임을 구분하는 데 쓰인다.
	} else // 비동기 큐(백그라운드 쓰기 등, 프로세스와 1:1이 아니라 ioprio별로 공유될 수 있음)
		bfq_clear_bfqq_sync(bfqq); // sync 플래그를 명시적으로 해제 - kmem_cache_alloc_node가 __GFP_ZERO라 이미 0이지만, 의도를 명확히 하기 위함.

	/* set end request to minus infinity from now */
	bfqq->ttime.last_end_request = now_ns + 1; // "마지막 request 종료 시각"을 지금보다 살짝 미래로 둬, 최초 think-time 샘플이 사실상 무한(매우 큼)이 되게 만드는 트릭 - 갓 생성된 큐를 "생각 시간이 김"으로 취급해 오작동을 방지.

	bfqq->creation_time = jiffies; // 이 bfqq가 생성된 시각(jiffies 틱) 기록 - stable merge 시 "얼마나 최근에 만들어졌는지" 비교 기준.

	bfqq->io_start_time = now_ns; // 이 큐의 I/O 활동이 시작된 시각 기록(나노초) - 장기 통계/디버깅용.

	bfq_mark_bfqq_IO_bound(bfqq); // "I/O bound(CPU보다 I/O를 많이 쓰는 프로세스)"로 잠정 표시 - budget 부여 정책에 영향.

	bfqq->pid = pid; // 디버그 로그(bfq_log_bfqq)에서 어떤 프로세스의 큐인지 식별하기 위해 pid 저장.

	/* Tentative initial value to trade off between thr and lat */
	bfqq->max_budget = (2 * bfq_max_budget(bfqd)) / 3; // 최초 예산 상한을 "장치 최대 예산의 2/3"로 잡아 처리량(throughput)과 지연시간(latency) 사이 절충을 시도 - 실측 후 점차 조정된다.
	bfqq->budget_timeout = bfq_smallest_from_now(); // 예산 타임아웃 시각을 "가능한 가장 이른 미래"로 설정해, 아직 실제 타임아웃이 계산되지 않았음을 나타낸다.

	bfqq->wr_coeff = 1; // weight-raising(가중치 상향) 계수를 1(상향 없음)로 초기화 - interactive/soft-rt로 판정되면 이후 값이 올라간다.
	bfqq->last_wr_start_finish = jiffies; // 마지막 weight-raising 시작/종료 시각을 현재로 설정 - 아직 raising 이력이 없음을 표시.
	bfqq->wr_start_at_switch_to_srt = bfq_smallest_from_now(); // soft-rt(소프트 실시간) 전환 시점을 "가장 이른 미래"로 둬, 아직 전환된 적 없음을 표시.
	bfqq->split_time = bfq_smallest_from_now(); // cooperating(협력) 큐에서 분리된 시각을 아직 없음(가장 이른 미래)으로 초기화 - 분리 이력 판단에 사용.

	/*
	 * To not forget the possibly high bandwidth consumed by a
	 * process/queue in the recent past,
	 * bfq_bfqq_softrt_next_start() returns a value at least equal
	 * to the current value of bfqq->soft_rt_next_start (see
	 * comments on bfq_bfqq_softrt_next_start).  Set
	 * soft_rt_next_start to now, to mean that bfqq has consumed
	 * no bandwidth so far.
	 */
	bfqq->soft_rt_next_start = jiffies; // "아직 대역폭을 소비한 적 없음"을 의미하도록 현재 시각으로 초기화 - 이후 soft-rt 판정 로직의 하한선 역할.

	/* first request is almost certainly seeky */
	bfqq->seek_history = 1; // 첫 request는 참고할 이력이 없으므로 보수적으로 "seek(탐색)했다"고 가정 - 시퀀셜 여부 판정 비트마스크의 초기값.

	bfqq->decrease_time_jif = jiffies; // inject limit(주입 한도) 감소 시각의 기준을 현재로 설정 - 100ms 히스테리시스 계산의 시작점.
}

/*
 * [한국어]
 * bfq_async_queue_prio - ioprio_class/ioprio/actuator 조합에 해당하는
 * "공유 async bfqq 포인터 슬롯"의 주소를 계산해 돌려준다.
 *
 * @bfqd: 스케줄러 전역 상태(이 함수 안에서는 직접 쓰이지 않지만 인터페이스
 *        일관성을 위해 전달된다).
 * @bfqg: 이 bio가 속한 cgroup의 bfq_group. async bfqq 캐시 배열
 *        (async_bfqq/async_idle_bfqq)을 보유한다.
 * @ioprio_class: IOPRIO_CLASS_RT/BE/IDLE/NONE - ionice(2) 우선순위 클래스.
 * @ioprio: IOPRIO_PRIO_DATA로 뽑아낸 우선순위 레벨(0..IOPRIO_NR_LEVELS-1).
 *          IDLE/NONE 처리 중 값이 조정될 수 있다.
 * @act_idx: 다중 actuator 인덱스.
 * @return: bfqg 내부 배열 원소의 주소(포인터의 포인터). 잘못된 ioprio_class면 NULL.
 *
 * BFQ는 비동기(async, 예: 지연 쓰기) I/O에 대해서는 프로세스마다 별도
 * bfqq를 만들지 않고, 같은 cgroup 안에서 같은 (class, priority) 조합을
 * 공유하는 하나의 bfqq를 재사용한다. 이렇게 하면 다수의 짧은 비동기
 * 쓰기 프로세스가 스케줄링 오버헤드 없이 하나의 큐로 합쳐진다. 이 함수는
 * 그 공유 슬롯을 "값"이 아니라 "주소"로 돌려주므로, 호출자(bfq_get_queue)가
 * 그 자리에 새로 만든 bfqq를 대입하거나(*async_bfqq = bfqq), 이미 있으면
 * 그대로 재사용할 수 있다.
 * 실행 컨텍스트: bfqd->lock을 쥔 상태에서 bfq_get_queue()가 호출한다.
 * caller: bfq_get_queue(). callee: 없음(단순 배열 인덱싱).
 *
 * 호출 체인:
 *   bfq_get_queue() → [bfq_async_queue_prio] → (배열 인덱싱만 수행)
 */
static struct bfq_queue **bfq_async_queue_prio(struct bfq_data *bfqd,
					       struct bfq_group *bfqg,
					       int ioprio_class, int ioprio, int act_idx)
{
	switch (ioprio_class) { // ionice 클래스에 따라 서로 다른 캐시 배열/인덱스를 선택.
	case IOPRIO_CLASS_RT: // 실시간(Real-Time) 클래스 - 별도의 [0] 서브배열에 저장.
		return &bfqg->async_bfqq[0][ioprio][act_idx]; // RT 우선순위 레벨/actuator별 공유 슬롯의 주소 반환.
	case IOPRIO_CLASS_NONE: // ioprio가 명시적으로 설정되지 않은 경우
		ioprio = IOPRIO_BE_NORM; // BE(Best-Effort) 클래스의 기본(중간) 우선순위로 취급.
		fallthrough; // BE 클래스 처리 로직을 그대로 이어받기 위해 의도적으로 case를 관통(fallthrough).
	case IOPRIO_CLASS_BE: // Best-Effort 클래스(가장 흔한 일반 프로세스)
		return &bfqg->async_bfqq[1][ioprio][act_idx]; // BE는 [1] 서브배열에 저장 - RT와 네임스페이스를 분리.
	case IOPRIO_CLASS_IDLE: // 유휴(idle) 클래스 - 다른 I/O가 없을 때만 서비스됨
		return &bfqg->async_idle_bfqq[act_idx]; // idle 클래스는 우선순위 레벨 구분이 없어 actuator별 슬롯 하나만 사용.
	default: // 알 수 없는 ioprio_class(정상 경로에서는 도달하지 않아야 함)
		return NULL; // 호출자(bfq_get_queue)가 NULL 슬롯을 받으면 async_bfqq 캐시를 쓰지 않는 경로로 빠진다.
	}
}

/*
 * [한국어]
 * bfq_do_early_stable_merge - 새로 생성 중인 bfqq를 직전에 생성된
 * last_bfqq_created와 "즉시(early)" 안정적으로(stably) 병합을 시도한다.
 *
 * @bfqd: 스케줄러 전역 상태.
 * @bfqq: 방금 만들어진(혹은 만들어지는 중인) bfq_queue - 병합의 대상(피흡수 큐가 됨).
 * @bic: bfqq를 요청한 프로세스의 bfq_io_cq - 병합 이력(stably_merged) 기록에 쓰인다.
 * @last_bfqq_created: 직전에 이 그룹/actuator에서 마지막으로 생성된 bfqq -
 *                     병합의 "생존 큐" 후보.
 * @return: 병합이 성사되면 병합 결과로 살아남은 bfqq(대개 last_bfqq_created
 *          계열), 병합할 수 없으면 원래의 bfqq 그대로.
 *
 * 부팅 시나 병렬 스트림 워크로드처럼, 사실상 같은 애플리케이션이 아주
 * 짧은 시간 간격으로 여러 bfqq를 만들어내는 경우, 그 bfqq들을 개별
 * 스케줄링 단위로 다루면 오버헤드만 커지고 서비스 품질 이득은 없다.
 * bfq_do_or_sched_stable_merge()가 "지금 당장 합쳐도 된다"고 판단했을 때
 * (주로 nonrot_with_queueing, 즉 NVMe SSD처럼 큐잉 가능한 비회전 장치일 때)
 * 호출되어, bfq_setup_merge()로 실제 병합 가능 여부/새 큐를 계산하고,
 * 성공하면 stably_merged 플래그를 bic 양쪽에 남겨 이후 split(분리) 시에도
 * 이 병합 이력을 참조할 수 있게 한다.
 * 실행 컨텍스트: bfqd->lock을 쥔 상태에서 bfq_do_or_sched_stable_merge()가 호출.
 * caller: bfq_do_or_sched_stable_merge(). callee: bfq_setup_merge(),
 * bfq_merge_bfqqs().
 *
 * 호출 체인:
 *   bfq_do_or_sched_stable_merge() → [bfq_do_early_stable_merge] → bfq_merge_bfqqs()
 */
static struct bfq_queue *
bfq_do_early_stable_merge(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			  struct bfq_io_cq *bic,
			  struct bfq_queue *last_bfqq_created)
{
	unsigned int a_idx = last_bfqq_created->actuator_idx; // 병합 대상(last_bfqq_created)이 속한 actuator 인덱스 - bic->bfqq_data[]도 actuator별로 나뉘어 있어 인덱스를 맞춰야 한다.
	/* [한국어] 두 bfqq를 실제로 합칠 수 있는지 검사하고, 가능하면 병합 후 살아남을 bfqq를 계산(협력 큐 병합과 동일한
	 * 로직을 재사용). */
	struct bfq_queue *new_bfqq =
		bfq_setup_merge(bfqq, last_bfqq_created);

	if (!new_bfqq) // bfq_setup_merge가 병합 불가로 판단한 경우(예: 이미 다른 큐와 병합이 진행 중)
		return bfqq; // 병합하지 않고 원래의 bfqq를 그대로 반환.

	if (new_bfqq->bic) // 병합 결과로 살아남은 큐가 자신만의 bic(단일 소유 프로세스)를 갖고 있다면
		new_bfqq->bic->bfqq_data[a_idx].stably_merged = true; // 그 소유자 쪽에도 "안정적으로 병합되었다"는 이력을 남겨, 나중에 split될 때 다시 병합을 시도할 근거로 삼는다.
	bic->bfqq_data[a_idx].stably_merged = true; // 지금 이 요청을 낸 프로세스(bic) 쪽에도 같은 이력을 기록.

	/*
	 * Reusing merge functions. This implies that
	 * bfqq->bic must be set too, for
	 * bfq_merge_bfqqs to correctly save bfqq's
	 * state before killing it.
	 */
	bfqq->bic = bic; // bfq_merge_bfqqs가 (곧 사라질) bfqq의 상태를 병합 전에 저장하려면 bfqq->bic가 유효해야 하므로 여기서 명시적으로 연결.
	return bfq_merge_bfqqs(bfqd, bic, bfqq); // 실제 병합을 수행(피흡수 큐 bfqq의 통계를 new_bfqq로 이전하고 bfqq를 정리) - 반환값은 이후 이 프로세스가 사용할, 병합에서 살아남은 bfqq.
}

/*
 * Many throughput-sensitive workloads are made of several parallel
 * I/O flows, with all flows generated by the same application, or
 * more generically by the same task (e.g., system boot). The most
 * counterproductive action with these workloads is plugging I/O
 * dispatch when one of the bfq_queues associated with these flows
 * remains temporarily empty.
 *
 * To avoid this plugging, BFQ has been using a burst-handling
 * mechanism for years now. This mechanism has proven effective for
 * throughput, and not detrimental for service guarantees. The
 * following function pushes this mechanism a little bit further,
 * basing on the following two facts.
 *
 * First, all the I/O flows of a the same application or task
 * contribute to the execution/completion of that common application
 * or task. So the performance figures that matter are total
 * throughput of the flows and task-wide I/O latency.  In particular,
 * these flows do not need to be protected from each other, in terms
 * of individual bandwidth or latency.
 *
 * Second, the above fact holds regardless of the number of flows.
 *
 * Putting these two facts together, this commits merges stably the
 * bfq_queues associated with these I/O flows, i.e., with the
 * processes that generate these IO/ flows, regardless of how many the
 * involved processes are.
 *
 * To decide whether a set of bfq_queues is actually associated with
 * the I/O flows of a common application or task, and to merge these
 * queues stably, this function operates as follows: given a bfq_queue,
 * say Q2, currently being created, and the last bfq_queue, say Q1,
 * created before Q2, Q2 is merged stably with Q1 if
 * - very little time has elapsed since when Q1 was created
 * - Q2 has the same ioprio as Q1
 * - Q2 belongs to the same group as Q1
 *
 * Merging bfq_queues also reduces scheduling overhead. A fio test
 * with ten random readers on /dev/nullb shows a throughput boost of
 * 40%, with a quadcore. Since BFQ's execution time amounts to ~50% of
 * the total per-request processing time, the above throughput boost
 * implies that BFQ's overhead is reduced by more than 50%.
 *
 * This new mechanism most certainly obsoletes the current
 * burst-handling heuristics. We keep those heuristics for the moment.
 */
/*
 * [한국어]
 * bfq_do_or_sched_stable_merge - 새 bfqq를 "최근에 만들어진 큐"와 즉시
 * 병합하거나, 나중에 병합할지 결정을 유예(스케줄)한다.
 *
 * @bfqd: 스케줄러 전역 상태. bfqd->last_bfqq_created(그룹이 없을 때의
 *        "마지막 생성 큐" 슬롯)를 보유.
 * @bfqq: 방금 만들어진 bfq_queue - 이 함수가 병합 대상 여부를 판단하는 큐.
 * @bic: bfqq를 요청한 프로세스의 bfq_io_cq - 지연 병합 예약 정보(stable_merge_bfqq)
 *       기록에 쓰인다.
 * @return: 즉시 병합이 이루어졌다면 병합 결과로 살아남은 bfqq, 아니면 원래의 bfqq.
 *
 * 위의 영문 주석(원저자 설명)에 나온 대로, 부팅 시퀀스나 병렬 스트림
 * 워크로드처럼 "사실상 같은 작업"이 짧은 시간 안에 여러 bfq_queue를
 * 만들어내는 상황을 감지해, 그 큐들을 안정적으로(stably) 하나로 합쳐
 * 스케줄링 오버헤드를 줄이는 것이 이 함수의 목적이다. last_bfqq_created는
 * cgroup(entity.parent)마다, 혹은 cgroup이 없으면 장치(bfqd) 전체에 대해
 * "가장 최근에 만들어진 bfqq"를 추적하는 포인터이며, 이 함수가 호출될
 * 때마다 갱신되거나 병합 판단의 기준으로 쓰인다. 회전식 디스크(HDD)처럼
 * NCQ 큐잉이 없는 장치에서는 병합을 바로 하지 않고 일정 시간 유예해,
 * bfqq가 시퀀셜 I/O로 판명되면 병합을 취소할 여지를 남긴다. 반면 NVMe SSD
 * 같은 nonrot_with_queueing 장치는 유예 없이 즉시 병합한다.
 * 실행 컨텍스트: bfqd->lock을 쥔 상태에서 bfq_get_queue()가 호출한다.
 * caller: bfq_get_queue(). callee: bfq_do_early_stable_merge().
 *
 * 호출 체인:
 *   bfq_get_queue() → [bfq_do_or_sched_stable_merge] → bfq_do_early_stable_merge()
 */
static struct bfq_queue *bfq_do_or_sched_stable_merge(struct bfq_data *bfqd,
						      struct bfq_queue *bfqq,
						      struct bfq_io_cq *bic)
{
	struct bfq_queue **source_bfqq = bfqq->entity.parent ? // bfqq가 cgroup에 속해 있다면(계층적 I/O 제어 사용 중이라면)
		&bfqq->entity.parent->last_bfqq_created : // 그 부모 entity(cgroup 서비스 트리 노드) 소속의 "마지막 생성 큐" 슬롯을 기준으로 삼는다.
		&bfqd->last_bfqq_created; // cgroup이 없으면 장치 전체 공용 슬롯(bfqd->last_bfqq_created)을 사용.

	struct bfq_queue *last_bfqq_created = *source_bfqq; // 위에서 고른 슬롯의 실제 값(직전에 기록해 둔 bfqq 포인터)을 읽어온다 - 아직 없으면 NULL.

	/*
	 * If last_bfqq_created has not been set yet, then init it. If
	 * it has been set already, but too long ago, then move it
	 * forward to bfqq. Finally, move also if bfqq belongs to a
	 * different group than last_bfqq_created, or if bfqq has a
	 * different ioprio, ioprio_class or actuator_idx. If none of
	 * these conditions holds true, then try an early stable merge
	 * or schedule a delayed stable merge. As for the condition on
	 * actuator_idx, the reason is that, if queues associated with
	 * different actuators are merged, then control is lost on
	 * each actuator. Therefore some actuator may be
	 * underutilized, and throughput may decrease.
	 *
	 * A delayed merge is scheduled (instead of performing an
	 * early merge), in case bfqq might soon prove to be more
	 * throughput-beneficial if not merged. Currently this is
	 * possible only if bfqd is rotational with no queueing. For
	 * such a drive, not merging bfqq is better for throughput if
	 * bfqq happens to contain sequential I/O. So, we wait a
	 * little bit for enough I/O to flow through bfqq. After that,
	 * if such an I/O is sequential, then the merge is
	 * canceled. Otherwise the merge is finally performed.
	 */
	if (!last_bfqq_created || // 아직 이 그룹/장치에서 "마지막 생성 큐"가 기록된 적이 없거나(최초 큐)
	    time_before(last_bfqq_created->creation_time + // 기록된 큐가 너무 오래 전에 만들어졌다면(활성 병합 유효 시간을 넘겼다면)
			msecs_to_jiffies(bfq_activation_stable_merging),
			bfqq->creation_time) ||
		bfqq->entity.parent != last_bfqq_created->entity.parent || // 서로 다른 cgroup에 속한다면 병합 대상이 아님
		bfqq->ioprio != last_bfqq_created->ioprio || // ionice 우선순위 레벨이 다르면 병합 대상이 아님
		bfqq->ioprio_class != last_bfqq_created->ioprio_class || // ionice 클래스(RT/BE/IDLE)가 다르면 병합 대상이 아님
		bfqq->actuator_idx != last_bfqq_created->actuator_idx) // 서로 다른 actuator에 속하면 병합 시 해당 actuator 제어를 잃으므로 병합하지 않음
		*source_bfqq = bfqq; // 위 조건 중 하나라도 참이면 병합을 시도하지 않고, 대신 bfqq를 새로운 "마지막 생성 큐"로 기록해 다음 큐 생성 시 비교 기준으로 삼는다.
	else if (time_after_eq(last_bfqq_created->creation_time + // 마지막 생성 큐가 burst_interval 이내(즉 아주 최근)에 만들어졌다면
				 bfqd->bfq_burst_interval,
				 bfqq->creation_time)) {
		if (likely(bfqd->nonrot_with_queueing)) // 장치가 NVMe SSD처럼 non-rotational이면서 NCQ 큐잉을 지원한다면(가장 흔한 현대 SSD 케이스)
			/*
			 * With this type of drive, leaving
			 * bfqq alone may provide no
			 * throughput benefits compared with
			 * merging bfqq. So merge bfqq now.
			 */
			bfqq = bfq_do_early_stable_merge(bfqd, bfqq, // 유예 없이 지금 바로 안정적 병합을 수행 - SSD에서는 굳이 시퀀셜 여부를 기다릴 필요가 없어 즉시 합치는 편이 유리.
							 bic,
							 last_bfqq_created);
		else { /* schedule tentative stable merge */ // 회전식 디스크(HDD, 큐잉 없음)라면 성급히 합치지 않고 "잠정적" 병합만 예약.
			/*
			 * get reference on last_bfqq_created,
			 * to prevent it from being freed,
			 * until we decide whether to merge
			 */
			last_bfqq_created->ref++; // 병합 여부를 결정할 때까지 last_bfqq_created가 해제되지 않도록 참조 카운트를 미리 올려 둔다.
			/*
			 * need to keep track of stable refs, to
			 * compute process refs correctly
			 */
			last_bfqq_created->stable_ref++; // "안정적 병합 예약"으로 인한 참조임을 별도로 추적 - 나중에 순수 프로세스 참조 수를 정확히 계산하기 위함.
			/*
			 * Record the bfqq to merge to.
			 */
			/* [한국어] 이 프로세스(bic)가 다음 기회(예: 다음 I/O 도착 시)에
			 * last_bfqq_created와 병합해야 함을 예약해 둔다. */
			bic->bfqq_data[last_bfqq_created->actuator_idx].stable_merge_bfqq =
				last_bfqq_created;
		}
	}

	return bfqq; // 즉시 병합이 일어났으면 병합 후 살아남은 큐를, 아니면(병합 안 함/예약만 함) 원래의 bfqq를 그대로 반환.
}


/*
 * [한국어]
 * bfq_get_queue - 이 프로세스(bic)/cgroup(bfqg)/actuator 조합에 대응하는
 * bfq_queue를 찾아 반환하거나, 없으면 새로 할당해 반환한다.
 *
 * @bfqd: 스케줄러 전역 상태 - kmem_cache_alloc_node의 NUMA 노드 힌트
 *        (bfqd->queue->node)를 제공한다.
 * @bio: 지금 처리 중인 bio - actuator index/소속 cgroup(bfqg) 계산에 쓰인다.
 * @is_sync: true면 동기(sync) bfqq, false면 여러 프로세스가 공유하는
 *           async bfqq를 다룬다.
 * @bic: 이 bio를 낸 프로세스의 I/O 컨텍스트 - stable merge 예약/판단에 사용.
 * @respawn: true면(예: ioprio 변경으로 인한 재생성) 방금 만든 bfqq에 대해
 *           stable merge 시도를 건너뛴다.
 * @return: 사용할 bfq_queue. 메모리 할당 실패 시에도 항상 유효한 포인터
 *          (최후 수단인 bfqd->oom_bfqq)를 반환하며 NULL은 반환하지 않는다.
 *
 * async bfqq는 (ioprio_class, ioprio, actuator) 조합별로 bfqg에 캐시되어
 * 여러 프로세스가 공유하므로, 먼저 bfq_async_queue_prio()로 캐시 슬롯을
 * 찾아보고 있으면 그대로 재사용한다(out: 라벨로 점프). 캐시에 없거나
 * 애초에 sync bfqq를 요청한 경우에는 kmem_cache_alloc_node()로 새
 * bfq_queue를 할당하고 bfq_init_bfqq()로 초기화한다. 할당이 실패하면
 * (메모리 부족) 스케줄러가 요청을 잃어버리지 않도록 전역 예비 큐인
 * oom_bfqq로 대체한다. 마지막으로 sync bfqq이고 respawn이 아니면, 최근에
 * 생성된 다른 bfqq와 안정적으로 병합할 수 있는지
 * bfq_do_or_sched_stable_merge()로 검사한다.
 * 실행 컨텍스트: bfqd->lock을 쥔 상태에서 호출된다(async_bfqq 캐시와
 * bfqg를 여러 프로세스가 공유하므로 락 없이 호출하면 경쟁 상태가 생긴다).
 * caller: bfq_check_ioprio_change(), bfq_init_rq() 경로의 bfqq 획득 로직.
 * callee: bfq_bio_bfqg(), bfq_async_queue_prio(), kmem_cache_alloc_node(),
 * bfq_init_bfqq(), bfq_init_entity(), bfq_do_or_sched_stable_merge().
 *
 * 호출 체인:
 *   bfq_init_rq()/bfq_check_ioprio_change() → [bfq_get_queue] → bfq_do_or_sched_stable_merge()
 */
static struct bfq_queue *bfq_get_queue(struct bfq_data *bfqd,
				       struct bio *bio, bool is_sync,
				       struct bfq_io_cq *bic,
				       bool respawn)
{
	const int ioprio = IOPRIO_PRIO_LEVEL(bic->ioprio); // bic에 캐시된 ioprio 값에서 우선순위 레벨(0..IOPRIO_NR_LEVELS-1)만 추출.
	const int ioprio_class = IOPRIO_PRIO_CLASS(bic->ioprio); // 같은 값에서 클래스(RT/BE/IDLE/NONE)만 추출.
	struct bfq_queue **async_bfqq = NULL; // async 캐시 슬롯의 주소 - sync 요청이면 끝까지 NULL로 남아 아래 "그룹 참조 추가" 분기를 건너뛰게 한다.
	struct bfq_queue *bfqq; // 최종적으로 반환할 bfq_queue를 담을 지역 변수.
	struct bfq_group *bfqg; // 이 bio가 속한 cgroup의 bfq_group.

	bfqg = bfq_bio_bfqg(bfqd, bio); // bio의 blkcg(블록 cgroup) 연결 정보로부터 대응하는 bfq_group을 조회.
	if (!is_sync) { // 비동기 요청이면 프로세스 전용이 아니라 그룹 내에서 공유되는 슬롯을 먼저 찾는다.
		async_bfqq = bfq_async_queue_prio(bfqd, bfqg, ioprio_class, // (class, priority, actuator) 조합에 대응하는 공유 슬롯의 주소를 계산.
						  ioprio,
						  bfq_actuator_index(bfqd, bio));
		bfqq = *async_bfqq; // 그 슬롯에 이미 채워진 bfqq가 있는지 확인.
		if (bfqq) // 캐시 적중 - 이미 이 조합의 async bfqq가 존재.
			goto out; // 새로 만들 필요 없이 바로 참조 카운트 증가/반환 단계로 이동.
	}

	bfqq = kmem_cache_alloc_node(bfq_pool, GFP_NOWAIT | __GFP_ZERO, // sync 요청이거나 async 캐시가 비어 있으면 새 bfq_queue를 슬랩 캐시에서 할당. GFP_NOWAIT: 스케줄러 락 보유 중이라 잠들 수 없음. __GFP_ZERO: 모든 필드를 0으로 밀어 두고 뒤이은 bfq_init_bfqq()가 나머지를 채우게 함.
				     bfqd->queue->node); // 이 요청 큐가 선호하는 NUMA 노드에 할당해 접근 지역성을 높인다.

	if (bfqq) { // 할당 성공
		bfq_init_bfqq(bfqd, bfqq, bic, current->pid, // 새 bfqq의 모든 필드(ttime, budget, wr_coeff 등)를 초기화.
			      is_sync, bfq_actuator_index(bfqd, bio));
		bfq_init_entity(&bfqq->entity, bfqg); // B-WF2Q+ 스케줄링 단위인 entity를 이 bfqg(소속 cgroup) 아래에 연결하고 가중치 등을 설정.
		bfq_log_bfqq(bfqd, bfqq, "allocated"); // 디버그 트레이스: 새 bfqq가 할당되었음을 기록.
	} else { // kmem_cache_alloc_node가 메모리 부족 등으로 실패한 경우
		bfqq = &bfqd->oom_bfqq; // 절대 실패하지 않는 전역 예비 큐(oom_bfqq)로 대체 - 요청을 잃어버리지 않기 위한 최후 수단.
		bfq_log_bfqq(bfqd, bfqq, "using oom bfqq"); // 디버그 트레이스: oom_bfqq 경로를 탔음을 기록.
		goto out; // oom_bfqq는 이미 초기화되어 있고 그룹 참조도 필요 없으므로 바로 out:으로 이동.
	}

	/*
	 * Pin the queue now that it's allocated, scheduler exit will
	 * prune it.
	 */
	if (async_bfqq) { // 방금 새로 할당한 것이 async bfqq라면(캐시 슬롯이 비어 있었던 경우)
		/* [한국어] async 큐는 프로세스가 아니라 bfq_group의 캐시 슬롯이
		 * 소유한다. sync 큐라면 bic가 참조를 들고 있다가 프로세스 종료 시
		 * 놓아주지만, async 큐에는 그런 소유자가 없으므로 슬롯 몫의 참조를
		 * 여기서 하나 더 올려 두고 그룹이 사라질 때에야 되돌려준다. */
		bfqq->ref++; /*
			      * Extra group reference, w.r.t. sync
			      * queue. This extra reference is removed
			      * only if bfqq->bfqg disappears, to
			      * guarantee that this queue is not freed
			      * until its group goes away.
			      */ // 캐시 슬롯 자체가 이 bfqq를 계속 참조하고 있음을 반영해 참조 카운트를 하나 더 올린다 - bfqg(소속 그룹)가 사라질 때만 이 참조가 해제된다.
		bfq_log_bfqq(bfqd, bfqq, "get_queue, bfqq not in async: %p, %d", // 디버그 트레이스: 새로 캐시에 등록되는 async bfqq와 그 시점의 참조 카운트를 기록.
			     bfqq, bfqq->ref);
		*async_bfqq = bfqq; // bfqg의 캐시 슬롯에 새로 만든 bfqq를 등록 - 다음에 같은 조합의 async I/O가 오면 이 큐가 재사용된다.
	}

out:
	bfqq->ref++; /* get a process reference to this queue */ // 지금 이 요청을 하는 프로세스(bic) 몫의 참조를 추가 - 캐시 적중/신규 할당/oom_bfqq 세 경로 모두 공통으로 거치는 지점.

	if (bfqq != &bfqd->oom_bfqq && is_sync && !respawn) // oom_bfqq가 아니고, 동기 큐이며, ioprio 변경으로 인한 재생성(respawn)이 아닌 "진짜 신규 생성"인 경우에만
		bfqq = bfq_do_or_sched_stable_merge(bfqd, bfqq, bic); // 최근에 만들어진 다른 bfqq와 안정적으로 병합할 수 있는지 검사 - 성사되면 반환값이 병합 후 살아남은 큐로 바뀔 수 있다.
	return bfqq; // 최종적으로 이 프로세스가 사용할 bfq_queue를 호출자에게 반환.
}

/*
 * [한국어]
 * bfq_update_io_thinktime - bfqq가 "요청이 없어 쉬고 있다가 다시 바빠지기
 * 까지" 걸리는 시간(think time, 프로세스의 생각/처리 시간)의 지수이동평균
 * (EWMA)을 갱신한다.
 *
 * @bfqd: 스케줄러 전역 상태 - bfq_slice_idle(유휴 슬라이스 상한) 값을 참조.
 * @bfqq: think time을 갱신할 대상 bfq_queue.
 * @return: 없음(void). bfqq->ttime의 ttime_samples/ttime_total/ttime_mean이 갱신된다.
 *
 * think time이 짧다는 것은 이 큐(프로세스)가 요청을 자주, 끊김 없이
 * 낸다는 뜻으로 interactive/동기적 패턴을 시사하며, BFQ는 이런 큐에
 * device idling(짧게 기다려주는 정책)을 적용해 다음 요청을 놓치지 않게
 * 한다. 이 함수는 새 request가 bfqq에 들어올 때(__bfq_insert_request에서
 * bfq_add_request 전에) 호출되어, "이번에 큐가 비어 있다가 다시 채워지기
 * 까지 걸린 시간"을 표본으로 삼아 7/8 가중치의 지수이동평균을 갱신한다.
 * 실행 컨텍스트: bfqd->lock을 쥔 상태에서 __bfq_insert_request()가 호출.
 * caller: __bfq_insert_request(). callee: blk_time_get_ns(), bfq_bfqq_busy(),
 * div_u64(), div64_ul().
 *
 * 호출 체인:
 *   __bfq_insert_request() → [bfq_update_io_thinktime] → (통계 갱신, 하위 호출 없음)
 */
static void bfq_update_io_thinktime(struct bfq_data *bfqd,
				    struct bfq_queue *bfqq)
{
	struct bfq_ttime *ttime = &bfqq->ttime; // think-time 통계(표본 수/누적 합/평균)를 담는 구조체에 대한 참조.
	u64 elapsed; // 이번에 측정할 "쉬었다가 다시 바빠지기까지" 걸린 시간(나노초 단위).

	/*
	 * We are really interested in how long it takes for the queue to
	 * become busy when there is no outstanding IO for this queue. So
	 * ignore cases when the bfq queue has already IO queued.
	 */
	if (bfqq->dispatched || bfq_bfqq_busy(bfqq)) // 아직 처리 중인 request가 있거나(dispatched>0) 이미 busy 상태라면 "쉬다가 깨어남"이 아니라 "계속 바빴음"이므로 think-time 표본으로 부적절.
		return; // 표본으로 쓸 수 없으므로 갱신 없이 종료.
	elapsed = blk_time_get_ns() - bfqq->ttime.last_end_request; // 마지막 request 종료 시각(last_end_request)부터 지금까지 흐른 실제 경과 시간을 계산.
	elapsed = min_t(u64, elapsed, 2ULL * bfqd->bfq_slice_idle); // idle 슬라이스의 2배를 상한으로 클램프 - 아주 오랫동안 쉰 경우(유휴 프로세스 등)까지 그대로 반영하면 평균이 왜곡되므로 상한을 둔다.

	ttime->ttime_samples = (7*ttime->ttime_samples + 256) / 8; // 표본 수 항목을 7/8 계수로 감쇠시키고 256(=2^8, 새 표본 1개를 고정소수점으로 표현한 값)을 더함 - 지수이동평균의 "가중치" 갱신.
	ttime->ttime_total = div_u64(7*ttime->ttime_total + 256*elapsed,  8); // 누적 합계도 같은 7/8 감쇠 계수로 갱신하고 새 표본(elapsed)을 256배 가중치로 반영 - 정수 연산에서 정밀도를 유지하기 위한 고정소수점 스케일링.
	/* [한국어] 반올림 보정(+128, 256의 절반)을 적용해 누적 합계를 표본 수로 나눠 최종 평균 think time을 계산
	 * - 이 값이 bfq_update_has_short_ttime()의 판정 기준이 된다. */
	ttime->ttime_mean = div64_ul(ttime->ttime_total + 128,
				     ttime->ttime_samples);
}

/*
 * [한국어]
 * bfq_update_io_seektime - 새 request의 시작 위치와 이 큐의 직전 요청
 * 위치를 비교해 "탐색(seek) 여부" 이력 비트마스크를 갱신하고, 그 결과에
 * 따라 soft-rt weight-raising 상태를 조정한다.
 *
 * @bfqd: 스케줄러 전역 상태 - BFQ_RQ_SEEKY 판정에 쓰이는 seek 임계값 등을 제공.
 * @bfqq: seek 이력을 갱신할 대상 bfq_queue.
 * @rq: 방금 도착한 request - 이 request의 시작 LBA와 큐의 last_request_pos를 비교.
 * @return: 없음(void). bfqq->seek_history 비트마스크와(조건부로) wr_coeff/
 *          entity.prio_changed가 갱신된다.
 *
 * seek_history는 "최근 몇 개의 request가 탐색성(비시퀀셜)이었는지"를 담는
 * 시프트 레지스터로, BFQQ_TOTALLY_SEEKY() 등에서 "이 큐는 완전히 탐색형
 * 워크로드다"를 판단하는 근거가 된다. 이 큐가 현재 soft real-time
 * weight-raising 상태이면서 실제로는 탐색형 워크로드로 판명되면(soft-rt로
 * 오판했을 가능성), soft-rt 유예 기간이 끝났는지에 따라 weight-raising을
 * 완전히 종료하거나 interactive weight-raising으로 되돌린다.
 * 실행 컨텍스트: bfqd->lock을 쥔 상태에서 __bfq_insert_request()가 호출.
 * caller: __bfq_insert_request(). callee: BFQ_RQ_SEEKY(), bfq_bfqq_end_wr(),
 * switch_back_to_interactive_wr().
 *
 * 호출 체인:
 *   __bfq_insert_request() → [bfq_update_io_seektime] → bfq_bfqq_end_wr() / switch_back_to_interactive_wr()
 */
static void
bfq_update_io_seektime(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		       struct request *rq)
{
	bfqq->seek_history <<= 1; // 이력 비트마스크를 한 칸 왼쪽으로 밀어 가장 오래된 표본을 자연히 밀어내고, 새 표본이 들어갈 최하위 비트 자리를 비운다.
	bfqq->seek_history |= BFQ_RQ_SEEKY(bfqd, bfqq->last_request_pos, rq); // 이번 request가 직전 요청 위치(last_request_pos)로부터 임계값 이상 떨어져 있으면(탐색성이면) 최하위 비트를 1로 설정.

	if (bfqq->wr_coeff > 1 && // 현재 weight-raising이 적용 중이고(가중치 상향 계수 > 1)
	    bfqq->wr_cur_max_time == bfqd->bfq_wr_rt_max_time && // 그 raising이 "soft real-time" 종류이며(soft-rt 전용 최대 지속시간 값과 일치)
	    BFQQ_TOTALLY_SEEKY(bfqq)) { // 최근 이력 전체가 탐색성으로 판명된 경우(soft-rt로 보기엔 워크로드 패턴이 맞지 않음)
		if (time_is_before_jiffies(bfqq->wr_start_at_switch_to_srt + // soft-rt로 전환된 시점부터 interactive weight-raising의 지속 기간이 이미 다 지났다면
					   bfq_wr_duration(bfqd))) {
			/*
			 * In soft_rt weight raising with the
			 * interactive-weight-raising period
			 * elapsed (so no switch back to
			 * interactive weight raising).
			 */
			bfq_bfqq_end_wr(bfqq); // 되돌아갈 interactive 기간도 이미 끝났으므로 weight-raising 자체를 완전히 종료.
		} else { /*
			  * stopping soft_rt weight raising
			  * while still in interactive period,
			  * switch back to interactive weight
			  * raising
			  */
			switch_back_to_interactive_wr(bfqq, bfqd); // 아직 interactive 기간이 남아 있으므로 soft-rt raising을 중단하고 interactive weight-raising으로 되돌린다.
			bfqq->entity.prio_changed = 1; // entity의 가중치가 바뀌었음을 표시해 다음 재활성화 시 서비스 트리에 새 가중치가 반영되게 함.
		}
	}
}

/*
 * [한국어]
 * bfq_update_has_short_ttime - 프로세스의 평균 think time을 근거로 bfqq가
 * "생각 시간이 짧다(interactive에 가깝다)"고 볼 수 있는지 판정하고, 판정
 * 전환 시 inject limit(주입 한도) 리셋 여부까지 함께 결정한다.
 *
 * @bfqd: 스케줄러 전역 상태 - bfq_slice_idle(유휴 슬라이스 상한) 참조.
 * @bfqq: 판정 대상 bfq_queue.
 * @bic: 이 bfqq를 사용하는 프로세스의 I/O 컨텍스트 - 프로세스 생존 여부
 *       (active_ref)를 확인하는 데 쓰인다.
 * @return: 없음(void). bfqq의 has_short_ttime 플래그와(조건부로) inject
 *          limit이 갱신된다.
 *
 * "생각 시간이 짧다"로 판정된 bfqq는 device idling(짧게 기다려주는
 * 정책)의 대상이 되어 interactive 응답성을 보장받는다. async 큐나 idle
 * 클래스, 혹은 idling 자체가 비활성화된 장치에서는 이 판정이 무의미하므로
 * 건너뛴다. 방금 cooperator에서 분리(split)되어 통계가 아직 신뢰할 수
 * 없는 경우도 건너뛴다. 평균 think time(ttime_mean)이 idle 슬라이스의
 * 절반보다 크면 "생각 시간이 길다"로, 그렇지 않으면(혹은 살아있는
 * 프로세스가 연결되어 있지 않아 무한대로 간주해야 하면) 반대로 판정한다.
 * 판정이 바뀌었고(state_changed) 아직 total-service-time 기준값이
 * 계산되지 않았다면, 100ms 히스테리시스 규칙에 따라 inject limit을
 * 리셋할지 결정한다(자세한 근거는 아래 원본 영문 주석 참고).
 * 실행 컨텍스트: bfqd->lock을 쥔 상태에서 __bfq_insert_request()가 호출.
 * caller: __bfq_insert_request(). callee: bfq_bfqq_sync(), bfq_class_idle(),
 * bfq_sample_valid(), bfq_mark_bfqq_has_short_ttime(),
 * bfq_clear_bfqq_has_short_ttime(), bfq_reset_inject_limit().
 *
 * 호출 체인:
 *   __bfq_insert_request() → [bfq_update_has_short_ttime] → bfq_reset_inject_limit()
 */
static void bfq_update_has_short_ttime(struct bfq_data *bfqd,
				       struct bfq_queue *bfqq,
				       struct bfq_io_cq *bic)
{
	bool has_short_ttime = true, state_changed; // has_short_ttime: 이번 판정 결과(기본값 "짧다"로 낙관적 시작). state_changed: 이전 플래그와 달라졌는지 여부.

	/*
	 * No need to update has_short_ttime if bfqq is async or in
	 * idle io prio class, or if bfq_slice_idle is zero, because
	 * no device idling is performed for bfqq in this case.
	 */
	if (!bfq_bfqq_sync(bfqq) || bfq_class_idle(bfqq) || // 비동기 큐이거나 idle 우선순위 클래스이면 애초에 idling을 적용하지 않으므로 판정이 무의미.
	    bfqd->bfq_slice_idle == 0) // idling 자체가 비활성화된 장치(bfq_slice_idle=0)라면 역시 판정할 이유가 없음.
		return; // 위 조건 중 하나라도 해당하면 아무것도 갱신하지 않고 종료.

	/* Idle window just restored, statistics are meaningless. */
	if (time_is_after_eq_jiffies(bfqq->split_time + // 협력(cooperating) 큐에서 방금 분리(split)되어 idle window가 막 복원된 직후라면
				     bfqd->bfq_wr_min_idle_time))
		return; // 아직 통계가 새 워크로드를 반영하지 못해 의미가 없으므로 이번 갱신은 건너뛴다.

	/* Think time is infinite if no process is linked to
	 * bfqq. Otherwise check average think time to decide whether
	 * to mark as has_short_ttime. To this goal, compare average
	 * think time with half the I/O-plugging timeout.
	 */
	if (atomic_read(&bic->icq.ioc->active_ref) == 0 || // 이 bfqq를 실제로 쓰는 프로세스가 이미 종료되어(io_context 참조 카운트 0) think time을 무한대로 간주해야 하거나
	    (bfq_sample_valid(bfqq->ttime.ttime_samples) && // 혹은 표본 수가 통계적으로 유효할 만큼 쌓였고
	     bfqq->ttime.ttime_mean > bfqd->bfq_slice_idle>>1)) // 평균 think time이 idle 슬라이스의 절반을 초과한다면(생각 시간이 길다고 볼 근거)
		has_short_ttime = false; // "생각 시간이 짧다"는 판정을 취소.

	state_changed = has_short_ttime != bfq_bfqq_has_short_ttime(bfqq); // 새로 계산한 판정이 이전에 설정돼 있던 플래그와 다른지(즉 방금 상태가 전환됐는지) 기록.

	if (has_short_ttime) // 새 판정이 "짧다"이면
		bfq_mark_bfqq_has_short_ttime(bfqq); // 플래그 설정 - 이후 idling 대상으로 취급됨.
	else // 새 판정이 "길다"이면
		bfq_clear_bfqq_has_short_ttime(bfqq); // 플래그 해제 - idling 비대상으로 취급됨.

	/*
	 * Until the base value for the total service time gets
	 * finally computed for bfqq, the inject limit does depend on
	 * the think-time state (short|long). In particular, the limit
	 * is 0 or 1 if the think time is deemed, respectively, as
	 * short or long (details in the comments in
	 * bfq_update_inject_limit()). Accordingly, the next
	 * instructions reset the inject limit if the think-time state
	 * has changed and the above base value is still to be
	 * computed.
	 *
	 * However, the reset is performed only if more than 100 ms
	 * have elapsed since the last update of the inject limit, or
	 * (inclusive) if the change is from short to long think
	 * time. The reason for this waiting is as follows.
	 *
	 * bfqq may have a long think time because of a
	 * synchronization with some other queue, i.e., because the
	 * I/O of some other queue may need to be completed for bfqq
	 * to receive new I/O. Details in the comments on the choice
	 * of the queue for injection in bfq_select_queue().
	 *
	 * As stressed in those comments, if such a synchronization is
	 * actually in place, then, without injection on bfqq, the
	 * blocking I/O cannot happen to served while bfqq is in
	 * service. As a consequence, if bfqq is granted
	 * I/O-dispatch-plugging, then bfqq remains empty, and no I/O
	 * is dispatched, until the idle timeout fires. This is likely
	 * to result in lower bandwidth and higher latencies for bfqq,
	 * and in a severe loss of total throughput.
	 *
	 * On the opposite end, a non-zero inject limit may allow the
	 * I/O that blocks bfqq to be executed soon, and therefore
	 * bfqq to receive new I/O soon.
	 *
	 * But, if the blocking gets actually eliminated, then the
	 * next think-time sample for bfqq may be very low. This in
	 * turn may cause bfqq's think time to be deemed
	 * short. Without the 100 ms barrier, this new state change
	 * would cause the body of the next if to be executed
	 * immediately. But this would set to 0 the inject
	 * limit. Without injection, the blocking I/O would cause the
	 * think time of bfqq to become long again, and therefore the
	 * inject limit to be raised again, and so on. The only effect
	 * of such a steady oscillation between the two think-time
	 * states would be to prevent effective injection on bfqq.
	 *
	 * In contrast, if the inject limit is not reset during such a
	 * long time interval as 100 ms, then the number of short
	 * think time samples can grow significantly before the reset
	 * is performed. As a consequence, the think time state can
	 * become stable before the reset. Therefore there will be no
	 * state change when the 100 ms elapse, and no reset of the
	 * inject limit. The inject limit remains steadily equal to 1
	 * both during and after the 100 ms. So injection can be
	 * performed at all times, and throughput gets boosted.
	 *
	 * An inject limit equal to 1 is however in conflict, in
	 * general, with the fact that the think time of bfqq is
	 * short, because injection may be likely to delay bfqq's I/O
	 * (as explained in the comments in
	 * bfq_update_inject_limit()). But this does not happen in
	 * this special case, because bfqq's low think time is due to
	 * an effective handling of a synchronization, through
	 * injection. In this special case, bfqq's I/O does not get
	 * delayed by injection; on the contrary, bfqq's I/O is
	 * brought forward, because it is not blocked for
	 * milliseconds.
	 *
	 * In addition, serving the blocking I/O much sooner, and much
	 * more frequently than once per I/O-plugging timeout, makes
	 * it much quicker to detect a waker queue (the concept of
	 * waker queue is defined in the comments in
	 * bfq_add_request()). This makes it possible to start sooner
	 * to boost throughput more effectively, by injecting the I/O
	 * of the waker queue unconditionally on every
	 * bfq_dispatch_request().
	 *
	 * One last, important benefit of not resetting the inject
	 * limit before 100 ms is that, during this time interval, the
	 * base value for the total service time is likely to get
	 * finally computed for bfqq, freeing the inject limit from
	 * its relation with the think time.
	 */
	if (state_changed && bfqq->last_serv_time_ns == 0 && // 판정이 바뀌었고, 아직 total-service-time 기준값(last_serv_time_ns)이 한 번도 계산된 적 없으며
	    (time_is_before_eq_jiffies(bfqq->decrease_time_jif + // (a) 마지막 inject limit 변경 이후 100ms가 이미 지났거나
				      msecs_to_jiffies(100)) ||
	     !has_short_ttime)) // (b) 이번 전환이 "짧다 -> 길다"인 경우라면(오실레이션 문제가 없으므로 즉시 반영해도 안전)
		bfq_reset_inject_limit(bfqd, bfqq); // inject limit을 리셋해 think-time 상태에 따른 기본값(0 또는 1)부터 다시 계산되게 한다.
}

/*
 * Called when a new fs request (rq) is added to bfqq.  Check if there's
 * something we should do about it.
 */
/*
 * [한국어]
 * bfq_rq_enqueued - 새 request가 bfqq에 추가된 직후 호출되어, in-service
 * queue의 idling(유휴 대기) 상태를 계속 유지할지, 지금 당장 풀고 예산
 * 타임아웃을 처리할지를 결정한다.
 *
 * @bfqd: 스케줄러 전역 상태 - idle_slice_timer 등 idling 관련 필드에 접근.
 * @bfqq: request가 추가된 bfq_queue.
 * @rq: 방금 추가된 request.
 * @return: 없음(void). 조건에 따라 bfqq의 wait_request 플래그를 해제하고
 *          idle_slice_timer를 취소하거나, budget timeout으로 bfqq를 만료시킬 수 있다.
 *
 * BFQ가 "다음 request를 좀 더 기다려 보자"며 device idling 중일 때
 * (bfq_bfqq_wait_request), 아주 작은 request 하나만 도착한 경우라면 굳이
 * 지금 장치를 깨워 그 작은 request만 내보내는 것보다는, 블록 계층의
 * request 병합(plug/unplug) 메커니즘이 조금 더 큰 request로 합쳐줄
 * 때까지 기다리는 편이 처리량에 유리하다. 반대로 충분히 큰 request가
 * 오거나, 이미 budget timeout이 발생했다면 idling을 즉시 중단하고
 * 필요하면 예산 타임아웃으로 큐를 만료시킨다.
 * 실행 컨텍스트: bfqd->lock을 쥔 상태에서 __bfq_insert_request()가 호출.
 * caller: __bfq_insert_request(). callee: idling_boosts_thr_without_issues(),
 * bfq_clear_bfqq_wait_request(), hrtimer_try_to_cancel(), bfq_bfqq_expire().
 *
 * 호출 체인:
 *   __bfq_insert_request() → [bfq_rq_enqueued] → bfq_bfqq_expire()
 */
static void bfq_rq_enqueued(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			    struct request *rq)
{
	if (rq->cmd_flags & REQ_META) // 이 request가 메타데이터(파일시스템 저널/inode 등) I/O라면
		bfqq->meta_pending++; // 메타데이터 request 대기 수를 증가 - 통계/우선 처리 판단에 활용될 수 있음.

	bfqq->last_request_pos = blk_rq_pos(rq) + blk_rq_sectors(rq); // 이 request의 "끝" 위치(시작 LBA + 섹터 수)를 기록 - 다음 request의 seek 여부(BFQ_RQ_SEEKY) 판정 기준점이 된다.

	if (bfqq == bfqd->in_service_queue && bfq_bfqq_wait_request(bfqq)) { // 지금 서비스 중인 큐(in-service)가 바로 이 bfqq이고, 그 큐가 "다음 request를 기다리며 idling 중"이라면
		bool small_req = bfqq->queued[rq_is_sync(rq)] == 1 && // 이 방향(sync/async)에 대기 중인 request가 이번 것 하나뿐이고
				 blk_rq_sectors(rq) < 32; // 그 request의 크기가 32섹터(16KiB) 미만으로 작다면 "작은 요청"으로 간주.
		bool budget_timeout = bfq_bfqq_budget_timeout(bfqq); // 이 큐의 예산이 이미 시간 초과되었는지 확인.

		/*
		 * There is just this request queued: if
		 * - the request is small, and
		 * - we are idling to boost throughput, and
		 * - the queue is not to be expired,
		 * then just exit.
		 *
		 * In this way, if the device is being idled to wait
		 * for a new request from the in-service queue, we
		 * avoid unplugging the device and committing the
		 * device to serve just a small request. In contrast
		 * we wait for the block layer to decide when to
		 * unplug the device: hopefully, new requests will be
		 * merged to this one quickly, then the device will be
		 * unplugged and larger requests will be dispatched.
		 */
		if (small_req && idling_boosts_thr_without_issues(bfqd, bfqq) && // 작은 request이고, idling이 이 상황에서 부작용 없이 처리량을 높여준다고 판단되며
		    !budget_timeout) // 아직 예산 타임아웃도 아니라면
			return; // 지금 장치를 깨우지 말고 idling(대기) 상태를 유지 - 더 큰 request로 병합될 기회를 준다.

		/*
		 * A large enough request arrived, or idling is being
		 * performed to preserve service guarantees, or
		 * finally the queue is to be expired: in all these
		 * cases disk idling is to be stopped, so clear
		 * wait_request flag and reset timer.
		 */
		bfq_clear_bfqq_wait_request(bfqq); // 위 조건에 해당하지 않으므로(충분히 큰 request이거나 서비스 보장/타임아웃 상황) 더 기다릴 필요가 없다고 판단해 wait_request 플래그를 해제.
		hrtimer_try_to_cancel(&bfqd->idle_slice_timer); // 예약해 둔 idle 슬라이스 타이머(hrtimer)를 취소 - 타이머 만료를 통한 별도의 idling 종료 처리가 필요 없어짐.

		/*
		 * The queue is not empty, because a new request just
		 * arrived. Hence we can safely expire the queue, in
		 * case of budget timeout, without risking that the
		 * timestamps of the queue are not updated correctly.
		 * See [1] for more details.
		 */
		if (budget_timeout) // 예산이 이미 타임아웃되었다면
			bfq_bfqq_expire(bfqd, bfqq, false, // 지금 이 큐를 만료시켜 B-WF2Q+ 스케줄러가 다음 큐를 선택하도록 함.
					BFQQE_BUDGET_TIMEOUT); // 만료 사유: 예산 타임아웃.
	}
}

/*
 * [한국어]
 * bfqq_request_allocated - bfqq와 그 상위 entity 체인 전체에 대해
 * "할당된(진행 중인) request 수"를 하나 증가시킨다.
 *
 * @bfqq: request가 새로 배정된 bfq_queue.
 * @return: 없음(void). bfqq부터 루트까지 각 entity->allocated가 1씩 증가한다.
 *
 * B-WF2Q+ 계층 구조에서 bfq_queue의 entity는 상위 bfq_group의 entity를
 * 부모로 두는 트리 형태를 이룬다. 특정 bfqq에 request가 배정되면, 그
 * 사실이 자신뿐 아니라 조상(cgroup) entity들에도 "이 서브트리에 아직
 * 처리 중인 request가 있다"로 반영되어야 idle/활성 판단이 정확해진다.
 * for_each_entity 매크로가 entity->parent를 따라 루트까지 순회하며
 * allocated 카운터를 증가시킨다.
 * 실행 컨텍스트: bfqd->lock을 쥔 상태에서 호출.
 * caller: __bfq_insert_request(). callee: 없음(카운터 증가만 수행).
 *
 * 호출 체인:
 *   __bfq_insert_request() → [bfqq_request_allocated] → (entity 카운터 갱신)
 */
static void bfqq_request_allocated(struct bfq_queue *bfqq)
{
	struct bfq_entity *entity = &bfqq->entity; // 이 bfqq의 스케줄링 단위(entity)에서 순회를 시작.

	for_each_entity(entity) // entity->parent를 따라 최상위 조상까지 순회(cgroup 계층 전체에 반영하기 위함).
		entity->allocated++; // 이 계층의 "할당된 request 수"를 하나 증가 - 하위 서브트리에 미처리 request가 있음을 표시.
}

/*
 * [한국어]
 * bfqq_request_freed - bfqq_request_allocated()의 반대 동작으로, bfqq와
 * 그 상위 entity 체인 전체의 "할당된 request 수"를 하나 감소시킨다.
 *
 * @bfqq: request가 해제(완료/다른 큐로 병합)된 bfq_queue.
 * @return: 없음(void). bfqq부터 루트까지 각 entity->allocated가 1씩 감소한다.
 *
 * request가 완료되거나 병합으로 다른 큐로 옮겨갈 때 호출되어, 더 이상
 * 이 bfqq(및 조상 entity들)가 그 request를 책임지지 않음을 반영한다.
 * 실행 컨텍스트: bfqd->lock을 쥔 상태에서 호출.
 * caller: __bfq_insert_request()(큐 병합 시 이전 큐의 카운터 감소에 사용).
 * callee: 없음(카운터 감소만 수행).
 *
 * 호출 체인:
 *   __bfq_insert_request() → [bfqq_request_freed] → (entity 카운터 갱신)
 */
static void bfqq_request_freed(struct bfq_queue *bfqq)
{
	struct bfq_entity *entity = &bfqq->entity; // 이 bfqq의 스케줄링 단위(entity)에서 순회를 시작.

	for_each_entity(entity) // entity->parent를 따라 최상위 조상까지 순회.
		entity->allocated--; // 이 계층의 "할당된 request 수"를 하나 감소 - bfqq_request_allocated()에서 올린 카운트를 되돌림.
}

/* returns true if it causes the idle timer to be disabled */
/*
 * [한국어]
 * __bfq_insert_request - request를 실제로 BFQ의 내부 자료구조(sort_list/
 * fifo)에 삽입하는 핵심 로직. 필요하면 cooperating(협력) 큐 병합을
 * 완료하고, think-time/seek-time 통계를 갱신한 뒤 bfq_add_request()로
 * request를 등록한다.
 *
 * @bfqd: 스케줄러 전역 상태.
 * @rq: 방금 blk-mq로부터 전달되어 BFQ에 삽입될 request. rq->elv.priv[]에
 *      이미 bic/bfqq 포인터가 채워져 있다(bfq_init_rq에서 설정).
 * @return: 이 삽입으로 인해 idle timer(idle_slice_timer)가 비활성화되었으면
 *          true, 아니면 false. 호출자(bfq_insert_request)가 cgroup 통계
 *          갱신 여부 판단에 사용한다.
 *
 * bfq_setup_cooperator()는 이 rq의 bfqq가 다른 프로세스의 bfqq와 "협력
 * 병합(cooperator merge)" 대상인지 검사한다. 병합 대상(new_bfqq)이 있으면
 * request의 소유권(allocated 카운트, ref)을 new_bfqq로 이전하고, bic이
 * 아직 옛 bfqq를 가리키고 있다면 병합을 완료해 이후 이 프로세스의 모든
 * request가 new_bfqq로 흐르도록 한다. 병합 처리 후에는(병합되지 않은
 * 경우도 포함해) think-time/has-short-ttime/seek-time 통계를 갱신하고,
 * bfq_add_request()로 request를 실제 서비스 트리(sort_list)와 FIFO
 * 리스트에 등록한다. 마지막으로 bfq_rq_enqueued()를 호출해 idling 계속
 * 여부/예산 타임아웃을 처리한다.
 * 실행 컨텍스트: bfqd->lock을 쥔 상태에서 bfq_insert_request()가 호출.
 * caller: bfq_insert_request(). callee: bfq_setup_cooperator(),
 * bfqq_request_allocated(), bfqq_request_freed(), bic_to_bfqq(),
 * bfq_merge_bfqqs(), bfq_put_queue(), bfq_update_io_thinktime(),
 * bfq_update_has_short_ttime(), bfq_update_io_seektime(), bfq_add_request(),
 * bfq_rq_enqueued().
 *
 * 호출 체인:
 *   bfq_insert_request() → [__bfq_insert_request] → bfq_add_request() / bfq_rq_enqueued()
 */
static bool __bfq_insert_request(struct bfq_data *bfqd, struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq), // rq->elv.priv[1]에 저장된, 이 request의 현재 소유 bfqq를 꺼낸다.
		*new_bfqq = bfq_setup_cooperator(bfqd, bfqq, rq, true, // 이 bfqq가 다른 프로세스의 bfqq와 협력(인접 영역을 동시에 접근) 관계라 병합 가능한지 검사 - 가능하면 병합 대상 bfqq를 반환, 아니면 NULL.
						 RQ_BIC(rq));
	bool waiting, idle_timer_disabled = false; // waiting: 삽입 전 이 큐가 "request를 기다리며 idling 중"이었는지. idle_timer_disabled: 삽입으로 인해 그 idling이 해제되었는지(최종 반환값).

	if (new_bfqq) { // 협력 큐 병합이 가능하다고 판단된 경우
		struct bfq_queue *old_bfqq = bfqq; // 병합 전 원래 bfqq를 보관.
		/*
		 * Release the request's reference to the old bfqq
		 * and make sure one is taken to the shared queue.
		 */
		bfqq_request_allocated(new_bfqq); // 이 request가 이제 new_bfqq(및 그 조상 entity)의 "할당된 request"로 계산되도록 카운터를 반영.
		bfqq_request_freed(bfqq); // 옛 bfqq(및 조상)에서는 더 이상 이 request를 책임지지 않으므로 카운터 반영을 해제.
		new_bfqq->ref++; // 이 rq가 new_bfqq를 참조하게 되었으므로 new_bfqq의 참조 카운트 증가.
		/*
		 * If the bic associated with the process
		 * issuing this request still points to bfqq
		 * (and thus has not been already redirected
		 * to new_bfqq or even some other bfq_queue),
		 * then complete the merge and redirect it to
		 * new_bfqq.
		 */
		if (bic_to_bfqq(RQ_BIC(rq), true, // 이 request를 낸 프로세스(bic)의 sync bfqq 슬롯이 아직 옛 bfqq를 가리키고 있는지 확인하고
				bfq_actuator_index(bfqd, rq->bio)) == bfqq) { // (다른 경로로 이미 재지향되지 않았는지 재확인)
			while (bfqq != new_bfqq) // bfqq가 new_bfqq에 도달할 때까지(체인 병합이 여러 단계일 수 있으므로 루프)
				bfqq = bfq_merge_bfqqs(bfqd, RQ_BIC(rq), bfqq); // 실제 병합을 한 단계 수행하고 살아남은 큐로 bfqq를 갱신 - bic도 함께 재지향된다.
		}

		bfq_clear_bfqq_just_created(old_bfqq); // 옛 bfqq가 병합되어 "방금 생성됨" 상태가 더 이상 유효하지 않으므로 플래그 해제.
		/*
		 * rq is about to be enqueued into new_bfqq,
		 * release rq reference on bfqq
		 */
		bfq_put_queue(old_bfqq); // 이 rq가 더 이상 old_bfqq를 참조하지 않으므로 old_bfqq의 참조를 반납(0이 되면 실제 해제).
		rq->elv.priv[1] = new_bfqq; // rq의 소유 bfqq 포인터를 new_bfqq로 갱신 - 이후 완료 처리(bfq_completed_request 등)가 올바른 큐를 참조하게 한다.
	}

	bfq_update_io_thinktime(bfqd, bfqq); // (병합 여부와 무관하게) 최종 bfqq의 think-time 통계를 갱신.
	bfq_update_has_short_ttime(bfqd, bfqq, RQ_BIC(rq)); // 갱신된 think-time을 바탕으로 has_short_ttime 판정을 갱신.
	bfq_update_io_seektime(bfqd, bfqq, rq); // 이 request의 위치로 seek 이력을 갱신.

	waiting = bfqq && bfq_bfqq_wait_request(bfqq); // 삽입 이전 시점 기준으로 이 큐가 idling(wait_request) 중이었는지 기록 - 아래에서 "이 request로 인해 idling이 풀렸는지" 비교하기 위한 기준값.
	bfq_add_request(rq); // request를 실제로 bfqq의 rb-tree(sort_list, LBA 정렬)에 삽입 - dispatch 시 next_rq 후보로 선택될 수 있게 된다.
	idle_timer_disabled = waiting && !bfq_bfqq_wait_request(bfqq); // 삽입 전엔 기다리고 있었는데 삽입 후엔 더 이상 기다리지 않는다면, 이번 삽입이 idling을 해제시킨 것 - 반환값으로 기록.

	rq->fifo_time = blk_time_get_ns() + bfqd->bfq_fifo_expire[rq_is_sync(rq)]; // 이 request의 FIFO 만료 시각을 계산(sync/async별로 만료 시간이 다름) - 너무 오래 기다린 request를 강제로 우선 처리하기 위한 기준.
	list_add_tail(&rq->queuelist, &bfqq->fifo); // 이 request를 bfqq의 FIFO 리스트 끝에 추가 - fifo_time 기준 만료 검사에 쓰인다.

	bfq_rq_enqueued(bfqd, bfqq, rq); // 삽입 후처리: in-service 큐의 idling을 계속 유지할지, 예산 타임아웃을 처리할지 결정.

	return idle_timer_disabled; // 이 삽입으로 idle 타이머가 실제로 해제되었는지를 호출자에게 알린다.
}

/*
 * [한국어]
 * bfq_update_insert_stats (CONFIG_BFQ_CGROUP_DEBUG 활성 버전) - request
 * 삽입이 끝난 뒤 cgroup 디버그 통계(bfqg_stats)를 갱신한다.
 *
 * @q: 이 request가 속한 request_queue - q->queue_lock으로 cgroup 통계
 *     갱신 구간을 보호한다(bfqd->lock과는 별개의 락).
 * @bfqq: 이번에 request가 삽입된 bfq_queue. NULL이면(예: dispatch 리스트로
 *        직접 들어간 경우) 아무 것도 하지 않는다.
 * @idle_timer_disabled: __bfq_insert_request()가 반환한, 이 삽입으로
 *                        idle 타이머가 해제되었는지 여부.
 * @cmd_flags: 삽입된 request의 명령 플래그(REQ_OP_* 등) - 읽기/쓰기 구분
 *             등 cgroup 통계 항목 분류에 쓰인다.
 * @return: 없음(void).
 *
 * bfq_insert_request()는 bfqd->lock을 해제한 "직후"에 이 함수를 호출한다.
 * cgroup 통계용 락(q->queue_lock)은 스케줄러 락(bfqd->lock)과 별개이며,
 * 스케줄러 핫패스에서 굳이 두 락을 동시에 쥐고 있을 필요가 없기 때문이다.
 * bfqq가 이 시점까지 여전히 유효함은, 이 흐름을 실행 중인 프로세스
 * 자신만이 그 bfqq를 병합/해제할 수 있다는 사실로 보장된다.
 * 실행 컨텍스트: bfqd->lock 해제 후, q->queue_lock을 새로 획득하는
 * 별도의 임계구역(process context)에서 실행된다.
 * caller: bfq_insert_request(). callee: bfqg_stats_update_io_add(),
 * bfqg_stats_update_idle_time().
 *
 * 호출 체인:
 *   bfq_insert_request() → [bfq_update_insert_stats] → bfqg_stats_update_io_add()
 */
#ifdef CONFIG_BFQ_CGROUP_DEBUG
static void bfq_update_insert_stats(struct request_queue *q,
				    struct bfq_queue *bfqq,
				    bool idle_timer_disabled,
				    blk_opf_t cmd_flags)
{
	if (!bfqq) // dispatch 리스트로 직접 들어가 특정 bfqq에 속하지 않은 request라면
		return; // 갱신할 cgroup 통계 대상이 없으므로 종료.

	/*
	 * bfqq still exists, because it can disappear only after
	 * either it is merged with another queue, or the process it
	 * is associated with exits. But both actions must be taken by
	 * the same process currently executing this flow of
	 * instructions.
	 *
	 * In addition, the following queue lock guarantees that
	 * bfqq_group(bfqq) exists as well.
	 */
	spin_lock_irq(&q->queue_lock); // cgroup 통계(bfqg_stats)를 보호하는 큐 락 획득 - blkcg 통계는 관례적으로 이 락 아래에서 갱신된다.
	bfqg_stats_update_io_add(bfqq_group(bfqq), bfqq, cmd_flags); // 이 bfqq가 속한 cgroup(bfq_group)의 "삽입된 I/O" 카운터를 cmd_flags(read/write 등)에 따라 증가.
	if (idle_timer_disabled) // 이번 삽입이 idle 타이머 해제를 유발했다면
		bfqg_stats_update_idle_time(bfqq_group(bfqq)); // 그 그룹이 "idling 상태로 보낸 시간" 통계를 마감/기록.
	spin_unlock_irq(&q->queue_lock); // 큐 락 해제.
}
#else
/*
 * [한국어]
 * bfq_update_insert_stats (CONFIG_BFQ_CGROUP_DEBUG 미설정 시 스텁) -
 * cgroup 디버그 통계 기능이 빌드에서 빠졌을 때, 호출부(bfq_insert_request)
 * 코드를 #ifdef로 감싸지 않고도 그대로 둘 수 있도록 아무 일도 하지 않는
 * 빈 인라인 함수로 대체한다.
 *
 * @q, @bfqq, @idle_timer_disabled, @cmd_flags: 위 CONFIG_BFQ_CGROUP_DEBUG
 * 버전과 동일한 시그니처지만, 본문이 비어 있어 실제로는 쓰이지 않는다.
 * @return: 없음(void).
 *
 * static inline으로 선언되어 있어 컴파일러가 호출부에서 완전히 제거할
 * 수 있으므로, CONFIG_BFQ_CGROUP_DEBUG가 꺼진 빌드에서는 성능 비용이
 * 전혀 없다.
 * caller: bfq_insert_request(). callee: 없음.
 *
 * 호출 체인:
 *   bfq_insert_request() → [bfq_update_insert_stats(stub)] → (없음)
 */
static inline void bfq_update_insert_stats(struct request_queue *q,
					   struct bfq_queue *bfqq,
					   bool idle_timer_disabled,
					   blk_opf_t cmd_flags) {} // CONFIG_BFQ_CGROUP_DEBUG 미설정 빌드용 no-op 스텁 - 파라미터를 그대로 무시.
#endif /* CONFIG_BFQ_CGROUP_DEBUG */

/*
 * [한국어] 전방 선언(forward declaration): 이 시점에서는 아직 bfq_init_rq()의
 * 실제 정의가 파일에 등장하지 않았으므로, 바로 아래 bfq_insert_request()가
 * 이를 미리 참조(호출)할 수 있도록 프로토타입만 선언해 둔다. 실제 함수
 * 본문은 이 파일 뒤쪽(다른 구간)에 정의되어 있으며, request의 bic/bfqq
 * 연결과 ioprio 반영 등을 수행한다.
 */
static struct bfq_queue *bfq_init_rq(struct request *rq);

/*
 * bfq_insert_request: blk-mq 스케줄러 ops 의 insert_requests 콜백.
 * request 를 BFQ 내부 queue 에 넣거나 dispatch 리스트(head/tail)로
 * 직접 추가한다.
 * 호출 경로: blk_mq_sched_insert_requests -> elevator_ops.insert_requests
 *          (bfq_insert_request)
 * NVMe 연결: bio -> request 변환이 끝난 직후이며, 드라이버로 가기 전
 *           BFQ 가 request 를 정렬/병합/우선순위 배정하는 관문이다.
 */
/*
 * [한국어]
 * bfq_insert_request - blk-mq 스케줄러 ops의 insert_requests 콜백 중
 * request 1개를 처리하는 하위 함수. request를 BFQ 내부 큐에 넣거나,
 * head/tail dispatch 리스트에 직접 추가한다.
 *
 * @hctx: 이 request가 제출된 blk-mq 하드웨어 컨텍스트(대략 NVMe의 SQ/CQ
 *        1쌍에 대응) - hctx->queue로 상위 request_queue를 얻는다.
 * @rq: 삽입할 request. bio -> request 변환이 이미 끝난 상태.
 * @flags: BLK_MQ_INSERT_AT_HEAD 등 삽입 방식을 지정하는 플래그.
 * @return: 없음(void).
 *
 * 먼저 bfq_init_rq()로 이 request가 속할 bfqq를 결정한다(ioprio 변경
 * 반영, bic/bfqq 연결, stable merge 예약/수행 등을 포함). blk_mq_sched_
 * try_insert_merge()가 기존 request와 병합 가능하다고 판단하면 그쪽으로
 * 흡수시키고 이 rq는 즉시 해제한다. BLK_MQ_INSERT_AT_HEAD 플래그가
 * 있으면(예: 재시도/우선 처리 요청) BFQ의 스케줄링을 거치지 않고 바로
 * dispatch 리스트 앞에 넣는다. bfqq가 없다면(예외 상황) dispatch 리스트
 * 뒤에 넣는다. 정상적인 경우에는 __bfq_insert_request()로 실제 BFQ 내부
 * 자료구조에 삽입한다. 마지막으로 bfqd->lock을 놓은 뒤 cgroup 디버그
 * 통계를 갱신한다.
 * 실행 컨텍스트: blk-mq가 request를 스케줄러에 넘기는 process context
 * (혹은 rq 재시도 경로)에서 호출되며, 함수 내부에서 bfqd->lock을 직접
 * 획득/해제한다.
 * caller: blk_mq_sched_insert_requests() → elevator_ops.insert_requests
 * (bfq_insert_requests()가 리스트를 순회하며 이 함수를 반복 호출).
 * callee: bfq_init_rq(), blk_mq_sched_try_insert_merge(), __bfq_insert_request(),
 * elv_rqhash_add(), bfq_update_insert_stats().
 *
 * 호출 체인:
 *   bfq_insert_requests() → [bfq_insert_request] → __bfq_insert_request() / bfq_update_insert_stats()
 */
static void bfq_insert_request(struct blk_mq_hw_ctx *hctx, struct request *rq,
			       blk_insert_t flags)
{
	struct request_queue *q = hctx->queue; // 이 하드웨어 컨텍스트가 속한 상위 request_queue(디바이스 전체 큐)를 얻는다.
	struct bfq_data *bfqd = q->elevator->elevator_data; // request_queue에 연결된 elevator(BFQ)의 전용 상태(bfq_data)를 꺼낸다.
	struct bfq_queue *bfqq; // 이 rq가 최종적으로 속하게 될 bfq_queue.
	bool idle_timer_disabled = false; // __bfq_insert_request()의 반환값을 저장할 변수(기본값: 해제 안 됨).
	blk_opf_t cmd_flags; // 락 해제 후에도 안전하게 쓸 수 있도록 미리 복사해 둘 rq->cmd_flags.
	LIST_HEAD(free); // blk_mq_sched_try_insert_merge()가 병합으로 인해 즉시 해제해야 할 request들을 모아 둘 임시 리스트.

#ifdef CONFIG_BFQ_GROUP_IOSCHED // cgroup 기반 계층적 I/O 스케줄링이 빌드에 포함된 경우에만
	if (!cgroup_subsys_on_dfl(io_cgrp_subsys) && rq->bio) // cgroup1(레거시) 모드로 blkio가 마운트되어 있고 이 rq에 원본 bio가 있다면
		bfqg_stats_update_legacy_io(q, rq); // cgroup1 전용 레거시 I/O 통계를 갱신(cgroup2 unified hierarchy에서는 다른 경로로 통계를 낸다).
#endif
	spin_lock_irq(&bfqd->lock); // 이 시점부터 BFQ 내부 자료구조(스케줄러 상태 전체)를 보호하는 스핀락 획득 - 인터럽트 컨텍스트로부터의 재진입도 차단.
	bfqq = bfq_init_rq(rq); // request 를 소유할 bfqq(따라서 담당 actuator) 결정.
	if (blk_mq_sched_try_insert_merge(q, rq, &free)) { // 이 rq를 기존에 대기 중인 다른 request와 (request 단위로) 병합할 수 있는지 시도
		spin_unlock_irq(&bfqd->lock); // 병합되어 이 rq가 더 이상 필요 없어졌으므로 락을 먼저 풀고
		blk_mq_free_requests(&free); // 병합으로 인해 불필요해진 request(들)를 blk-mq에 반납.
		return; // 이 rq는 별도로 스케줄링할 필요가 없으므로 함수 종료.
	}

	trace_block_rq_insert(rq); // 트레이스포인트: request가 스케줄러에 삽입되는 이벤트를 기록(blktrace/perf 등에서 관찰 가능).

	if (flags & BLK_MQ_INSERT_AT_HEAD) { // 호출자가 "다른 것보다 먼저 나가야 한다"고 명시한 경우(예: 에러 처리/재시도 request)
		list_add(&rq->queuelist, &bfqd->dispatch); // BFQ의 스케줄링 정책을 완전히 우회해 dispatch 리스트의 맨 앞에 직접 삽입 - 다음 dispatch 시 최우선으로 나간다.
	} else if (!bfqq) { // bfq_init_rq가 유효한 bfqq를 결정하지 못한 경우(초기화 실패 등 예외 상황)
		list_add_tail(&rq->queuelist, &bfqd->dispatch); // 어쩔 수 없이 dispatch 리스트 뒤쪽에 추가 - BFQ의 우선순위/idling 정책 없이 순서대로만 나간다.
	} else { // 정상적인 경우(bfqq가 결정되었고 head 삽입도 아님)
		idle_timer_disabled = __bfq_insert_request(bfqd, rq); // BFQ 내부 queue 에 삽입; 드라이버로는 아직 전달되지 않고 dispatch될 때까지 대기.
		/*
		 * Update bfqq, because, if a queue merge has occurred
		 * in __bfq_insert_request, then rq has been
		 * redirected into a new queue.
		 */
		bfqq = RQ_BFQQ(rq); // __bfq_insert_request 내부에서 협력 큐 병합이 일어났을 수 있으므로, rq에 실제로 최종 연결된 bfqq를 다시 읽어온다.

		if (rq_mergeable(rq)) { // 이 request가 향후 다른 bio와 병합 가능한 형태라면(REQ_NOMERGE 등이 아니라면)
			elv_rqhash_add(q, rq); // 빠른 병합 후보 탐색을 위한 elevator 해시 테이블에 이 request를 등록.
			if (!q->last_merge) // 아직 "마지막으로 병합에 성공한 request" 캐시가 비어 있다면
				q->last_merge = rq; // 이 request를 그 캐시로 등록해 다음 병합 시도의 첫 후보로 삼는다.
		}
	}

	/*
	 * Cache cmd_flags before releasing scheduler lock, because rq
	 * may disappear afterwards (for example, because of a request
	 * merge).
	 */
	cmd_flags = rq->cmd_flags; // 락 해제 후 rq가 병합 등으로 해제될 수도 있으므로, 이후 통계에 쓸 cmd_flags를 미리 로컬 변수에 복사.
	spin_unlock_irq(&bfqd->lock); // BFQ 스케줄러 락 해제 - 이 시점 이후로는 rq/bfqq를 직접 역참조하면 안전하지 않을 수 있다(그래서 위에서 cmd_flags를 미리 복사해 둠).

	bfq_update_insert_stats(q, bfqq, idle_timer_disabled, // 스케줄러 락 밖에서 별도의 큐 락으로 보호되는 cgroup 디버그 통계를 갱신.
				cmd_flags);
}

/*
 * [한국어]
 * bfq_insert_requests - blk-mq 스케줄러 ops의 insert_requests 콜백 본체.
 * 여러 request로 이루어진 리스트를 하나씩 꺼내 bfq_insert_request()로
 * 위임한다.
 *
 * @hctx: blk-mq 하드웨어 컨텍스트.
 * @list: 삽입할 request들의 연결 리스트(blk_mq_sched_insert_requests가
 *        일괄로 넘겨준 배치).
 * @flags: 모든 request에 공통으로 적용할 삽입 플래그(BLK_MQ_INSERT_AT_HEAD 등).
 * @return: 없음(void). 함수가 끝나면 list는 비어 있다.
 *
 * blk-mq는 plug list(프로세스별 임시 배치 큐)에 쌓인 여러 request를 한
 * 번에 스케줄러로 넘길 수 있는데, 이 함수가 그 리스트를 순서대로
 * 소비하며 각 request를 개별적으로 bfq_insert_request()에 전달한다.
 * 이렇게 배치로 넘기면 매 request마다 스케줄러 콜백을 별도로 호출하는
 * 것보다 오버헤드가 줄어든다.
 * 실행 컨텍스트: blk-mq submit 경로의 process context.
 * caller: blk_mq_sched_insert_requests() (elevator_ops.insert_requests).
 * callee: bfq_insert_request().
 *
 * 호출 체인:
 *   blk_mq_sched_insert_requests() → [bfq_insert_requests] → bfq_insert_request()
 */
static void bfq_insert_requests(struct blk_mq_hw_ctx *hctx,
				struct list_head *list,
				blk_insert_t flags)
{
	while (!list_empty(list)) { // 리스트에 아직 처리할 request가 남아 있는 동안 반복.
		struct request *rq; // 이번 반복에서 꺼낼 request.

		rq = list_first_entry(list, struct request, queuelist); // 리스트의 첫 번째 request를 꺼낸다(FIFO 순서 유지).
		list_del_init(&rq->queuelist); // 그 request를 원래 리스트에서 분리 - 초기화된 리스트 노드 상태로 되돌려 재사용 가능하게 함.
		bfq_insert_request(hctx, rq, flags); // 이 request 하나를 실제로 BFQ 스케줄러에 삽입.
	}
}

/*
 * bfq_update_hw_tag: 장치가 실제로 여러 request 를 동시에 queueing
 * 하는지(hw_tag) 감지한다.
 * 호출 경로: bfq_completed_request -> bfq_update_hw_tag
 * NVMe 연결: NVMe SSD 는 항상 NCQ 능력이 있으므로 hw_tag 가 1로
 *           수렴하며, nonrot_with_queueing=true 가 되어 merge/idling
 *           정책이 SSD 에 맞게 조정된다.
 */
/*
 * [한국어]
 * bfq_update_hw_tag - 최근 관찰된 in-flight request 수를 근거로, 장치가
 * 실제로 여러 request를 동시에 처리(큐잉)할 수 있는지(hw_tag) 판정한다.
 *
 * @bfqd: 스케줄러 전역 상태 - max_rq_in_driver/hw_tag_samples 등 관찰용
 *        누적 필드를 갱신한다.
 * @return: 없음(void). 판정이 확정되면 bfqd->hw_tag와
 *          bfqd->nonrot_with_queueing이 갱신된다.
 *
 * 이 함수는 request가 완료될 때마다(bfq_completed_request에서) 호출되어,
 * "지금까지 관찰된 최대 동시 in-flight 수"가 임계값(BFQ_HW_QUEUE_THRESHOLD)을
 * 넘는지로 장치의 큐잉 능력을 추정한다. 표본이 왜곡되지 않도록, 요청
 * 수 자체가 적어 병렬성이 드러나지 않는 구간이나, idling 정책 때문에
 * 일부러 dispatch를 적게 한 구간은 표본에서 제외한다. 충분한 표본
 * (BFQ_HW_QUEUE_SAMPLES)이 쌓이면 최종적으로 hw_tag를 확정하고, 이후에는
 * 재평가하지 않는다. NVMe SSD처럼 NCQ 큐잉 능력이 있는 비회전 장치는
 * 결국 hw_tag=1, nonrot_with_queueing=true로 수렴해 merge보다 idling/
 * injection 정책이 우선 적용되도록 조정된다.
 * 실행 컨텍스트: bfqd->lock을 쥔 상태에서 bfq_completed_request()가 호출.
 * caller: bfq_completed_request(). callee: blk_queue_rot().
 *
 * 호출 체인:
 *   bfq_completed_request() → [bfq_update_hw_tag] → (bfqd 필드 갱신, 하위 호출 없음)
 */
static void bfq_update_hw_tag(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq = bfqd->in_service_queue; // 현재 서비스 중인 큐(있다면) - "idling 때문에 dispatch가 적은 것뿐인지"를 판단하는 아래 조건에서 쓰인다.

	bfqd->max_rq_in_driver = max_t(int, bfqd->max_rq_in_driver, // 지금까지 관찰된 "드라이버에 동시에 내려가 있던 최대 request 수"를 계속 갱신
				       bfqd->tot_rq_in_driver); // 이번 순간의 in-flight 총합(tot_rq_in_driver)과 비교해 더 큰 값을 취한다.

	if (bfqd->hw_tag == 1) // 이미 "장치가 큐잉을 지원한다"고 확정되었다면
		return; // 더 판단할 필요 없이 종료(한 번 1로 확정되면 이 함수는 사실상 no-op이 된다).

	/*
	 * This sample is valid if the number of outstanding requests
	 * is large enough to allow a queueing behavior.  Note that the
	 * sum is not exact, as it's not taking into account deactivated
	 * requests.
	 */
	if (bfqd->tot_rq_in_driver + bfqd->queued <= BFQ_HW_QUEUE_THRESHOLD) // 드라이버에 내려간 것과 아직 BFQ에 대기 중인 것을 합쳐도 임계값 이하라면
		return; // 요청 수 자체가 적어 큐잉 능력이 드러나지 않는 표본이므로 채택하지 않는다.

	/*
	 * If active queue hasn't enough requests and can idle, bfq might not
	 * dispatch sufficient requests to hardware. Don't zero hw_tag in this
	 * case
	 */
	if (bfqq && bfq_bfqq_has_short_ttime(bfqq) && // 현재 서비스 중인 큐가 있고, 그 큐가 "생각 시간이 짧다(interactive)"고 판정되어 idling 대상이며
	    bfqq->dispatched + bfqq->queued[0] + bfqq->queued[1] < // 그 큐 자체의 미처리+대기 요청 수가
	    BFQ_HW_QUEUE_THRESHOLD && // 임계값(4)은 "이 정도도 동시에 안 나간다면 장치의 큐잉 능력 문제가 아니라 BFQ가 idling으로 억제한 결과"라고 볼 하한선이다
	    bfqd->tot_rq_in_driver < BFQ_HW_QUEUE_THRESHOLD) // 임계값보다 적고, 드라이버 전체 in-flight도 임계값 미만이라면
		return; // 큐잉 미지원이 아니라 idling 정책 때문에 dispatch가 적은 상황일 수 있으므로 판정을 유보(hw_tag를 깎지 않음).

	if (bfqd->hw_tag_samples++ < BFQ_HW_QUEUE_SAMPLES) // 유효 표본 수를 하나 늘리면서, 아직 충분히 쌓이지 않았다면
		return; // 판정을 내리기엔 이르므로 다음 완료 시점까지 더 기다린다.

	bfqd->hw_tag = bfqd->max_rq_in_driver > BFQ_HW_QUEUE_THRESHOLD; // 충분한 표본이 모였으므로, 관찰된 최대 동시 in-flight 수가 임계값을 넘었는지로 "장치가 실제로 큐잉을 한다(hw_tag)"를 최종 판정.
	bfqd->max_rq_in_driver = 0; // 다음 판정 주기를 위해 최대값 관찰치를 리셋.
	bfqd->hw_tag_samples = 0; // 표본 수도 리셋해 새 관찰 구간을 시작.

	/* [한국어] 비회전(SSD, 예: NVMe)이면서 큐잉까지 지원하는 장치인지 최종 결정 - true면 merge보다
	 * idling/inject 정책이 SSD에 맞게 조정된다. */
	bfqd->nonrot_with_queueing =
		!blk_queue_rot(bfqd->queue) && bfqd->hw_tag;
}

/*
 * bfq_completed_request: 하나의 request 가 완료되었을 때 BFQ 의
 * per-actuator inflight 카운트를 줄이고, in-service queue 의 idle/
 * budget-timeout/만료를 처리한다.
 * 호출 경로: bfq_finish_requeue_request -> bfq_completed_request
 * NVMe 연결: NVMe CQ 완료 핸들러(nvme_process_cq) 경로 하부에서
 *           불리며, controller 가 처리를 마친 CID/PRP/SGL 영역을
 *           회수하는 시점과 맞물린다.
 */
/*
 * [한국어]
 * bfq_completed_request - request 완료(NVMe 관점의 CQ completion에 대응)
 * 시점에 BFQ의 in-flight 카운트, 처리율(rate) 추정, waker 큐 추적, 그리고
 * in-service 큐의 idling/만료 여부까지 한 번에 처리하는 완료 경로의
 * 핵심 함수.
 *
 * @bfqq: 완료된 request가 속했던 bfq_queue.
 * @bfqd: 스케줄러 전역 상태.
 * @return: 없음(void). bfqd/bfqq의 다수 필드가 갱신되고, 필요하면
 *          bfq_bfqq_expire() 또는 bfq_schedule_dispatch()가 호출된다.
 *
 * 먼저 bfq_update_hw_tag()로 장치의 큐잉 능력 판정을 갱신하고, 이
 * request가 차지하던 actuator별/전체 in-flight 카운트를 감소시킨다.
 * 큐가 완전히 비었다면 weight-raising 관련 타임스탬프를 남기고 추적
 * 자료구조에서 제거한다. 완료 간격(delta_us)이 비정상적으로 길어 최근
 * 관찰 구간의 처리율 추정이 신뢰할 수 없다면 관찰 구간을 리셋한다.
 * 공유(협력) 큐가 아니라면 "가장 최근에 완료된 큐"로 기록해 waker 큐
 * 탐지에 활용한다. soft real-time 판정 대기 중이면 필요한 조건이
 * 갖춰졌을 때 soft_rt_next_start를 계산한다. 마지막으로, 완료된 request의
 * 큐가 현재 in-service 큐라면 idling을 유지할지, 예산 타임아웃/요청
 * 소진으로 만료시킬지를 결정하고, 장치 전체가 idle해졌다면 혹시 놓친
 * dispatch가 없는지 재확인을 예약한다.
 * 실행 컨텍스트: bfqd->lock을 쥔 상태에서 bfq_finish_requeue_request()가
 * 호출(request 완료/requeue 경로, blk-mq softirq 또는 process context).
 * caller: bfq_finish_requeue_request(). callee: bfq_update_hw_tag(),
 * bfq_del_bfqq_in_groups_with_pending_reqs(), bfq_weights_tree_remove(),
 * bfq_update_rate_reset(), bfq_bfqq_softrt_next_start(), bfq_arm_slice_timer(),
 * bfq_bfqq_expire(), bfq_schedule_dispatch().
 *
 * 호출 체인:
 *   bfq_finish_requeue_request() → [bfq_completed_request] → bfq_bfqq_expire() / bfq_schedule_dispatch()
 */
static void bfq_completed_request(struct bfq_queue *bfqq, struct bfq_data *bfqd)
{
	u64 now_ns; // request 완료 시각을 담을 나노초 단조 시계 값.
	u32 delta_us; // 직전 완료(혹은 dispatch)로부터 이번 완료까지 걸린 시간(마이크로초).

	bfq_update_hw_tag(bfqd); // 완료 시점에 맞춰 장치의 큐잉 능력(hw_tag/nonrot_with_queueing) 판정을 갱신.

	bfqd->rq_in_driver[bfqq->actuator_idx]--; // 이 bfqq가 속한 actuator의 in-flight 카운트를 감소 - 방금 완료된 request 하나만큼 반영.
	bfqd->tot_rq_in_driver--; // 장치 전체 in-flight 총합도 감소.
	bfqq->dispatched--; // 이 bfqq 자체의 "dispatch되었지만 아직 완료 안 된 request 수"도 감소.

	if (!bfqq->dispatched && !bfq_bfqq_busy(bfqq)) { // 이 bfqq에 더 이상 진행 중인 request가 없고, 대기 중인(backlog) request도 없다면(완전히 비었다면)
		/*
		 * Set budget_timeout (which we overload to store the
		 * time at which the queue remains with no backlog and
		 * no outstanding request; used by the weight-raising
		 * mechanism).
		 */
		bfqq->budget_timeout = jiffies; // "완전히 빈 시점"을 budget_timeout 필드에 겸용으로 기록 - weight-raising 로직이 "얼마나 오래 비어 있었는지" 판단하는 데 재사용.

		bfq_del_bfqq_in_groups_with_pending_reqs(bfqq); // 이 bfqq를 "아직 처리할 request가 남은 큐들" 추적 리스트(cgroup별)에서 제거 - 완전히 비었으므로 더 이상 추적 대상이 아니다.
		bfq_weights_tree_remove(bfqq); // 가중치 기반 서비스 트리(weights_tree)에서도 제거 - 빈 큐는 가중치 통계에서 제외된다.
	}

	now_ns = blk_time_get_ns(); // 완료 처리 시각을 나노초로 기록.

	bfqq->ttime.last_end_request = now_ns; // 이 큐의 "마지막 request 종료 시각"을 갱신 - 다음 think-time 계산(bfq_update_io_thinktime)의 기준점이 된다.

	/*
	 * Using us instead of ns, to get a reasonable precision in
	 * computing rate in next check.
	 */
	delta_us = div_u64(now_ns - bfqd->last_completion, NSEC_PER_USEC); // 직전 완료 시각(bfqd->last_completion)부터 지금까지 걸린 시간을 마이크로초 단위로 환산 - 아래 처리율(rate) 계산에 적합한 정밀도.

	/*
	 * If the request took rather long to complete, and, according
	 * to the maximum request size recorded, this completion latency
	 * implies that the request was certainly served at a very low
	 * rate (less than 1M sectors/sec), then the whole observation
	 * interval that lasts up to this time instant cannot be a
	 * valid time interval for computing a new peak rate.  Invoke
	 * bfq_update_rate_reset to have the following three steps
	 * taken:
	 * - close the observation interval at the last (previous)
	 *   request dispatch or completion
	 * - compute rate, if possible, for that observation interval
	 * - reset to zero samples, which will trigger a proper
	 *   re-initialization of the observation interval on next
	 *   dispatch
	 */
	if (delta_us > BFQ_MIN_TT/NSEC_PER_USEC && // 완료 간격이 최소 유효 시간(BFQ_MIN_TT)보다 충분히 길고
	   (bfqd->last_rq_max_size<<BFQ_RATE_SHIFT)/delta_us < // 지금까지 기록된 최대 request 크기를 이 간격으로 나눈 처리율이
			1UL<<(BFQ_RATE_SHIFT - 10)) // 매우 낮은 임계값(대략 1M 섹터/초 미만)보다 작다면 - "비정상적으로 느리게 처리된 구간"으로 판단
		bfq_update_rate_reset(bfqd, NULL); // 지금까지의 관찰 구간을 무효화하고 peak rate 계산을 리셋 - 다음 dispatch부터 새 관찰 구간을 시작하게 한다.
	bfqd->last_completion = now_ns; // 다음 계산을 위해 "마지막 완료 시각"을 지금으로 갱신.
	/*
	 * Shared queues are likely to receive I/O at a high
	 * rate. This may deceptively let them be considered as wakers
	 * of other queues. But a false waker will unjustly steal
	 * bandwidth to its supposedly woken queue. So considering
	 * also shared queues in the waking mechanism may cause more
	 * control troubles than throughput benefits. Then reset
	 * last_completed_rq_bfqq if bfqq is a shared queue.
	 */
	if (!bfq_bfqq_coop(bfqq)) // 이 bfqq가 여러 프로세스와 공유되는 협력(cooperating) 큐가 아니라면(단일 프로세스 전용이라면)
		bfqd->last_completed_rq_bfqq = bfqq; // "가장 최근에 완료된 request의 큐"로 기록 - waker(다른 큐를 깨우는 큐) 탐지 로직의 후보로 사용된다.
	else // 공유(협력) 큐라면
		bfqd->last_completed_rq_bfqq = NULL; // waker 후보에서 제외 - 공유 큐는 여러 프로세스의 I/O가 섞여 있어 "누가 누구를 깨웠는지" 인과관계를 신뢰할 수 없다.

	/*
	 * If we are waiting to discover whether the request pattern
	 * of the task associated with the queue is actually
	 * isochronous, and both requisites for this condition to hold
	 * are now satisfied, then compute soft_rt_next_start (see the
	 * comments on the function bfq_bfqq_softrt_next_start()). We
	 * do not compute soft_rt_next_start if bfqq is in interactive
	 * weight raising (see the comments in bfq_bfqq_expire() for
	 * an explanation). We schedule this delayed update when bfqq
	 * expires, if it still has in-flight requests.
	 */
	if (bfq_bfqq_softrt_update(bfqq) && bfqq->dispatched == 0 && // 이 큐가 "soft real-time 여부를 갱신해야 함" 표시가 있고, 방금 마지막 in-flight request까지 완료되었으며(dispatched==0)
	    RB_EMPTY_ROOT(&bfqq->sort_list) && // 대기 중인 request도 전혀 없고(완전히 비었고)
	    bfqq->wr_coeff != bfqd->bfq_wr_coeff) // 현재 interactive weight-raising 계수가 아니라면(즉 interactive raising 중이 아니라면)
		bfqq->soft_rt_next_start =
			bfq_bfqq_softrt_next_start(bfqd, bfqq); // 이 큐가 다음에 soft-rt로 인정받을 수 있는 최소 시점을 계산해 기록 - 대역폭을 실제로 다 썼는지 판단하는 데 쓰인다.

	/*
	 * If this is the in-service queue, check if it needs to be expired,
	 * or if we want to idle in case it has no pending requests.
	 */
	if (bfqd->in_service_queue == bfqq) { // 방금 완료된 request의 큐가 바로 "지금 서비스 중인 큐"라면(가장 흔한 경우)
		if (bfq_bfqq_must_idle(bfqq)) { // 이 큐에 대해 device idling을 계속 유지해야 한다고 판단되면(서비스 보장을 위해)
			if (bfqq->dispatched == 0) // 그리고 이제 진짜로 in-flight request가 하나도 없다면(방금 것이 마지막이었다면)
				bfq_arm_slice_timer(bfqd); // idle 슬라이스 타이머를 실제로 가동(arm) - 다음 request를 일정 시간 기다린다.
			/*
			 * If we get here, we do not expire bfqq, even
			 * if bfqq was in budget timeout or had no
			 * more requests (as controlled in the next
			 * conditional instructions). The reason for
			 * not expiring bfqq is as follows.
			 *
			 * Here bfqq->dispatched > 0 holds, but
			 * bfq_bfqq_must_idle() returned true. This
			 * implies that, even if no request arrives
			 * for bfqq before bfqq->dispatched reaches 0,
			 * bfqq will, however, not be expired on the
			 * completion event that causes bfqq->dispatch
			 * to reach zero. In contrast, on this event,
			 * bfqq will start enjoying device idling
			 * (I/O-dispatch plugging).
			 *
			 * But, if we expired bfqq here, bfqq would
			 * not have the chance to enjoy device idling
			 * when bfqq->dispatched finally reaches
			 * zero. This would expose bfqq to violation
			 * of its reserved service guarantees.
			 */
			return; // idling을 적용해야 하는 상황이므로 아래의 만료(expire) 로직은 건너뛰고 바로 종료 - 이 큐는 계속 in-service로 남아 idling 혜택을 받는다.
		} else if (bfq_may_expire_for_budg_timeout(bfqq)) // idling이 필요 없고, 예산 타임아웃으로 만료 가능한 상태라면
			bfq_bfqq_expire(bfqd, bfqq, false, // 이 큐를 만료시켜 다음 큐로 전환하고
					BFQQE_BUDGET_TIMEOUT); // 만료 사유: 예산 타임아웃.
		else if (RB_EMPTY_ROOT(&bfqq->sort_list) && // 혹은 대기 중인 request가 전혀 없고(sort_list가 비어 있고)
			 (bfqq->dispatched == 0 || // in-flight도 전혀 없거나
			  !bfq_better_to_idle(bfqq))) // (in-flight가 있더라도) idling이 유리하지 않다고 판단되면
			bfq_bfqq_expire(bfqd, bfqq, false, // 이 큐를 만료시키고
					BFQQE_NO_MORE_REQUESTS); // 만료 사유: 더 이상 처리할 request가 없음.
	}

	if (!bfqd->tot_rq_in_driver) // 장치 전체를 통틀어 in-flight request가 하나도 남지 않았다면(완전히 idle 상태가 되었다면)
		bfq_schedule_dispatch(bfqd); // 혹시 dispatch할 것이 더 있는데 놓친 것은 아닌지 재확인하도록 dispatch 워크를 스케줄 - 장치가 불필요하게 계속 idle 상태로 남는 것을 방지한다.
}

/*
 * The processes associated with bfqq may happen to generate their
 * cumulative I/O at a lower rate than the rate at which the device
 * could serve the same I/O. This is rather probable, e.g., if only
 * one process is associated with bfqq and the device is an SSD. It
 * results in bfqq becoming often empty while in service. In this
 * respect, if BFQ is allowed to switch to another queue when bfqq
 * remains empty, then the device goes on being fed with I/O requests,
 * and the throughput is not affected. In contrast, if BFQ is not
 * allowed to switch to another queue---because bfqq is sync and
 * I/O-dispatch needs to be plugged while bfqq is temporarily
 * empty---then, during the service of bfqq, there will be frequent
 * "service holes", i.e., time intervals during which bfqq gets empty
 * and the device can only consume the I/O already queued in its
 * hardware queues. During service holes, the device may even get to
 * remaining idle. In the end, during the service of bfqq, the device
 * is driven at a lower speed than the one it can reach with the kind
 * of I/O flowing through bfqq.
 *
 * To counter this loss of throughput, BFQ implements a "request
 * injection mechanism", which tries to fill the above service holes
 * with I/O requests taken from other queues. The hard part in this
 * mechanism is finding the right amount of I/O to inject, so as to
 * both boost throughput and not break bfqq's bandwidth and latency
 * guarantees. In this respect, the mechanism maintains a per-queue
 * inject limit, computed as below. While bfqq is empty, the injection
 * mechanism dispatches extra I/O requests only until the total number
 * of I/O requests in flight---i.e., already dispatched but not yet
 * completed---remains lower than this limit.
 *
 * A first definition comes in handy to introduce the algorithm by
 * which the inject limit is computed.  We define as first request for
 * bfqq, an I/O request for bfqq that arrives while bfqq is in
 * service, and causes bfqq to switch from empty to non-empty. The
 * algorithm updates the limit as a function of the effect of
 * injection on the service times of only the first requests of
 * bfqq. The reason for this restriction is that these are the
 * requests whose service time is affected most, because they are the
 * first to arrive after injection possibly occurred.
 *
 * To evaluate the effect of injection, the algorithm measures the
 * "total service time" of first requests. We define as total service
 * time of an I/O request, the time that elapses since when the
 * request is enqueued into bfqq, to when it is completed. This
 * quantity allows the whole effect of injection to be measured. It is
 * easy to see why. Suppose that some requests of other queues are
 * actually injected while bfqq is empty, and that a new request R
 * then arrives for bfqq. If the device does start to serve all or
 * part of the injected requests during the service hole, then,
 * because of this extra service, it may delay the next invocation of
 * the dispatch hook of BFQ. Then, even after R gets eventually
 * dispatched, the device may delay the actual service of R if it is
 * still busy serving the extra requests, or if it decides to serve,
 * before R, some extra request still present in its queues. As a
 * conclusion, the cumulative extra delay caused by injection can be
 * easily evaluated by just comparing the total service time of first
 * requests with and without injection.
 *
 * The limit-update algorithm works as follows. On the arrival of a
 * first request of bfqq, the algorithm measures the total time of the
 * request only if one of the three cases below holds, and, for each
 * case, it updates the limit as described below:
 *
 * (1) If there is no in-flight request. This gives a baseline for the
 *     total service time of the requests of bfqq. If the baseline has
 *     not been computed yet, then, after computing it, the limit is
 *     set to 1, to start boosting throughput, and to prepare the
 *     ground for the next case. If the baseline has already been
 *     computed, then it is updated, in case it results to be lower
 *     than the previous value.
 *
 * (2) If the limit is higher than 0 and there are in-flight
 *     requests. By comparing the total service time in this case with
 *     the above baseline, it is possible to know at which extent the
 *     current value of the limit is inflating the total service
 *     time. If the inflation is below a certain threshold, then bfqq
 *     is assumed to be suffering from no perceivable loss of its
 *     service guarantees, and the limit is even tentatively
 *     increased. If the inflation is above the threshold, then the
 *     limit is decreased. Due to the lack of any hysteresis, this
 *     logic makes the limit oscillate even in steady workload
 *     conditions. Yet we opted for it, because it is fast in reaching
 *     the best value for the limit, as a function of the current I/O
 *     workload. To reduce oscillations, this step is disabled for a
 *     short time interval after the limit happens to be decreased.
 *
 * (3) Periodically, after resetting the limit, to make sure that the
 *     limit eventually drops in case the workload changes. This is
 *     needed because, after the limit has gone safely up for a
 *     certain workload, it is impossible to guess whether the
 *     baseline total service time may have changed, without measuring
 *     it again without injection. A more effective version of this
 *     step might be to just sample the baseline, by interrupting
 *     injection only once, and then to reset/lower the limit only if
 *     the total service time with the current limit does happen to be
 *     too large.
 *
 * More details on each step are provided in the comments on the
 * pieces of code that implement these steps: the branch handling the
 * transition from empty to non empty in bfq_add_request(), the branch
 * handling injection in bfq_select_queue(), and the function
 * bfq_choose_bfqq_for_injection(). These comments also explain some
 * exceptions, made by the injection mechanism in some special cases.
 */
/*
 * [한국어]
 * bfq_update_inject_limit - injection(다른 bfqq 의 request 를 in-service
 * bfqq 의 idle 구간에 끼워 넣는 기법)이 in-service bfqq 의 서비스 시간에
 * 끼친 영향을 측정해 inject_limit(동시에 주입 가능한 request 수 상한)를
 * 늘리거나 줄인다.
 *
 * @bfqd: 이 device 의 BFQ 전역 상태. last_empty_occupied_ns(주입 측정
 *        구간이 시작된 시각), rqs_injected(이번 구간에 실제로 주입이
 *        있었는지), tot_rq_in_driver(드라이버에 남아있는 미완료 request
 *        총수), max_rq_in_driver(관측된 driver 동시 처리 최대치) 를 읽어
 *        계산의 입력으로 사용한다.
 * @bfqq: 방금 request 가 완료된 in-service bfqq. inject_limit,
 *        last_serv_time_ns, decrease_time_jif 필드가 이 함수에서 갱신된다.
 * @return: 없음(void). 결과는 bfqq->inject_limit/last_serv_time_ns 와
 *          bfqd->waited_rq/rqs_injected 갱신으로 반영된다.
 *
 * in-service bfqq 가 다음 request 를 아직 만들어내지 못해 대기(idle)하는
 * 동안, BFQ 는 그 유휴 시간을 낭비하지 않으려고 다른 bfqq 의 request 를
 * NVMe controller 로 미리 흘려보낸다(injection). 문제는 injection 이
 * 과하면 in-service bfqq 의 request 가 controller queue 뒤에서 밀려
 * 응답 지연시간(latency)이 늘어난다는 것이다. 이 함수는 in-service bfqq
 * 의 request 가 완료될 때마다 "이번 구간의 실측 소요시간"을 "injection이
 * 전혀 없었던 기준 시간(baseline)"과 비교해, 지연이 threshold(기준시간의
 * 1.5배) 이상이면 inject_limit 를 낮추고, threshold 미만이면서 driver
 * queue 에 여유가 있으면 inject_limit 를 높인다. baseline 자체도 매
 * 요청마다 갱신되어 워크로드 변화(요청 크기/지역성 변화 등)를 뒤따라간다.
 * 실행 컨텍스트: bfqd->lock 을 쥔 상태로 completion 경로(blk-mq completion
 * 콜백 하부)에서 호출되므로 별도의 락은 이 함수 내부에서 잡지 않는다.
 * caller: bfq_finish_requeue_request (완료된 rq 가 bfqd->waited_rq 와
 *         같을 때만 호출 — 즉 "이번 injection 측정 대상"으로 지목된 그
 *         request 일 때만 측정을 수행한다).
 * callee: blk_time_get_ns()(단조 시계 읽기) 외에는 순수 필드 대입/계산뿐.
 * 에러 경로: 없음 — 실패 개념이 없는 통계/휴리스틱 갱신 함수이다.
 *
 * 호출 체인:
 *   bfq_finish_requeue_request → [bfq_update_inject_limit] → (필드 갱신으로 종료)
 */
static void bfq_update_inject_limit(struct bfq_data *bfqd,
				    struct bfq_queue *bfqq)
{
	u64 tot_time_ns = blk_time_get_ns() - bfqd->last_empty_occupied_ns;
	/* [한국어] 이번 injection 측정 구간의 총 경과 시간(ns). last_empty_occupied_ns 는
	 * in-service bfqq 가 비어(idle) 진입한 시각이므로, 그 시점부터 지금(이 request 완료
	 * 시점)까지가 "injection 이 섞인 채로 걸린 실제 서비스 시간"이 된다. */
	unsigned int old_limit = bfqq->inject_limit;
	/* [한국어] 이번 계산 이전의 inject_limit 값을 백업. 아래 조건에서 "감소 이전 값" 기준으로
	 * driver queue 여유(bfqd->max_rq_in_driver)와 비교해야 하므로 미리 저장해 둔다. */

	if (bfqq->last_serv_time_ns > 0 && bfqd->rqs_injected) {
	/* [한국어] baseline(순수 서비스 시간)이 이미 한 번 계산돼 있고, 이번 구간에 실제로 다른
	 * bfqq 의 request 가 주입됐을 때만 "injection 이 지연을 얼마나 유발했는지" 평가할 수
	 * 있다. 주입이 없었다면 비교할 지연 원인이 없으므로 이 분기에 들어오지 않는다. */
		u64 threshold = (bfqq->last_serv_time_ns * 3)>>1;
		/* [한국어] baseline 서비스 시간의 1.5배(*3 후 >>1, 즉 /2)를 허용 임계값으로 설정.
		 * 이 정도 지연 증가까지는 "injection 으로 얻는 처리량 이득이 손해보다 크다"고 보는
		 * 경험적 마진이며, 별도 hysteresis 가 없어 진동(oscillation)이 발생할 수 있다. */

		if (tot_time_ns >= threshold && old_limit > 0) {
		/* [한국어] 실측 시간이 임계값을 넘었다 = injection 이 in-service bfqq 의 완료를
		 * 과도하게 늦췄다는 뜻. inject_limit 을 낮춰 다음부터는 덜 주입하게 한다. */
			bfqq->inject_limit--;
			/* [한국어] 동시 주입 가능 개수를 1 감소시켜, 드라이버 큐에 함께
			 * 밀어 넣는 "남의 request" 수를 줄이고 in-service bfqq 의 completion 지연을
			 * 완화한다. */
			bfqq->decrease_time_jif = jiffies;
			/* [한국어] 감소가 일어난 시각을 jiffies 로 기록. injection 판단 로직이 이
			 * 시각 이후 짧은 기간 동안은 limit 을 다시 올리지 않도록 해 진동을 억제. */
		} else if (tot_time_ns < threshold &&
			   old_limit <= bfqd->max_rq_in_driver)
			/* [한국어] 지연이 임계값 아래(=injection 이 해롭지 않았음)이고, 현재 limit 이
			 * driver 가 실제 감당 가능한 최대 동시 요청 수(max_rq_in_driver) 이하라면
			 * 더 주입해도 안전하다고 보고 limit 을 늘려 처리량을 끌어올린다. */
			bfqq->inject_limit++;
	}

	/*
	 * Either we still have to compute the base value for the
	 * total service time, and there seem to be the right
	 * conditions to do it, or we can lower the last base value
	 * computed.
	 *
	 * NOTE: (bfqd->tot_rq_in_driver == 1) means that there is no I/O
	 * request in flight, because this function is in the code
	 * path that handles the completion of a request of bfqq, and,
	 * in particular, this function is executed before
	 * bfqd->tot_rq_in_driver is decremented in such a code path.
	 */
	/* [한국어] (a) 아직 baseline 이 계산된 적 없고 지금 driver 에 미완료 request 가 이것
	 * 하나뿐이거나(순수 측정 가능 시점), 또는 (b) 이번 실측 시간이 기존 baseline 보다
	 * 짧다(=더 낮은/정확한 기준을 발견) — 두 경우 모두 baseline 을 갱신할 신호다. */
	if ((bfqq->last_serv_time_ns == 0 && bfqd->tot_rq_in_driver == 1) ||
	    tot_time_ns < bfqq->last_serv_time_ns) {
		if (bfqq->last_serv_time_ns == 0) {
		/* [한국어] 이번이 baseline 최초 계산이라면, 지금까지는 injection 효과를 평가할
		 * 기준이 없어 injection 자체를 시도하지 못했다는 뜻 — 이제 baseline 이 생겼으니
		 * injection 실험을 시작하도록 limit 을 올려 준다. */
			/*
			 * Now we certainly have a base value: make sure we
			 * start trying injection.
			 */
			bfqq->inject_limit = max_t(unsigned int, 1, old_limit);
			/* [한국어] limit 을 최소 1 로 설정(기존 old_limit 이 더 크면 그대로 유지)해
			 * "일단 한 번은 injection 을 시도해 보는" 상태로 전환한다. */
		}
		bfqq->last_serv_time_ns = tot_time_ns;
		/* [한국어] 새 baseline(순수/개선된 서비스 시간)을 저장. 다음 completion 때 이 값과
		 * 비교해 injection 의 악영향 여부를 다시 판정한다. */
	} else if (!bfqd->rqs_injected && bfqd->tot_rq_in_driver == 1)
		/*
		 * No I/O injected and no request still in service in
		 * the drive: these are the exact conditions for
		 * computing the base value of the total service time
		 * for bfqq. So let's update this value, because it is
		 * rather variable. For example, it varies if the size
		 * or the spatial locality of the I/O requests in bfqq
		 * change.
		 */
		bfqq->last_serv_time_ns = tot_time_ns;
		/* [한국어] 이번 구간엔 injection 이 전혀 없었고(rqs_injected==false) driver 에도
		 * 이 request 하나만 있었다 — "가장 순수한" 서비스 시간 측정 조건이므로, 요청
		 * 크기/지역성 변화 등 워크로드 특성 변화를 baseline 이 계속 따라가게 한다. */


	/* update complete, not waiting for any request completion any longer */
	bfqd->waited_rq = NULL;
	/* [한국어] "이번 injection 측정 대상"으로 지목해 두었던 request 포인터를 해제. 이 함수가
	 * 호출됐다는 것 자체가 그 request 의 완료 처리를 마쳤다는 뜻이므로 더 이상 추적할 필요가
	 * 없다. 다음 측정 대상은 bfq_select_queue() 가 새 injection 후보를 고를 때 다시 설정한다. */
	bfqd->rqs_injected = false;
	/* [한국어] "이번 측정 구간에 injection 이 있었다" 플래그를 리셋해, 다음 idle 구간부터
	 * injection 여부를 새로 관찰할 수 있게 한다. */
}

/*
 * Handle either a requeue or a finish for rq. The things to do are
 * the same in both cases: all references to rq are to be dropped. In
 * particular, rq is considered completed from the point of view of
 * the scheduler.
 */
/*
 * [한국어]
 * bfq_finish_requeue_request - request rq 가 completion 또는 requeue 로
 * 수명을 다할 때 호출되어, BFQ 가 그 rq 에 대해 들고 있던 모든 참조/카운트를
 * 정리한다.
 *
 * @rq: 방금 completion(CQ 도착) 또는 requeue(재제출 예정)로 처리된 request.
 *      rq->elv.priv[0]/[1] 에 각각 bic(io_context), bfqq 가 들어 있으며,
 *      이 함수가 끝나면 두 필드 모두 NULL 로 재설정된다.
 * @return: 없음(void).
 *
 * blk-mq 관점에서 requeue 와 finish 는 "이 rq 를 더 이상 BFQ 내부 큐 상태로
 * 취급하지 않는다"는 점에서 동일하게 처리해야 한다. 이 함수는 (1) rq 가
 * 이미 dispatch 되어 driver 로 내려간 적이 있으면(RQF_STARTED) cgroup 통계와
 * bfq_completed_request() 를 통해 bfqq/entity 의 budget·inflight 를 갱신하고,
 * (2) bfqq 의 allocated 카운트를 낮추고 bfqq 자체의 참조 카운트를 반납하며,
 * (3) 이 rq 를 발급한 io_context(bic) 의 in-flight 카운트를 낮추고,
 * (4) rq->elv.priv[]를 초기화해 이후 동일 rq 가 재사용될 때 오작동을 막는다.
 * 실행 컨텍스트: blk-mq 의 request 해제/재제출 경로(주로 completion softirq
 * 또는 요청 실패 처리 경로)에서 호출되며, bfqd->lock 을 직접 획득/해제해
 * dispatch 경로와의 자료구조 경쟁을 막는다.
 * caller: bfq_finish_request(정상 finish 경로), 그리고 elevator_ops 의
 *         requeue_request 훅으로 직접 등록되어 requeue 경로에서도 호출된다.
 * callee: bfqg_stats_update_completion(), bfq_update_inject_limit(),
 *         bfq_completed_request(), bfqq_request_freed(), bfq_put_queue().
 * 에러 경로: icq 나 bfqq 가 이미 없으면(중복 호출 등) 아무 것도 하지 않고
 *           즉시 반환한다 — 이는 blk-mq 가 같은 rq 에 대해 finish/requeue
 *           훅을 여러 번 부를 수 있는 상황에 대한 방어 코드다.
 *
 * 호출 체인:
 *   blk_mq_free_request/requeue 경로 → [bfq_finish_requeue_request] → bfq_completed_request
 */
static void bfq_finish_requeue_request(struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq);
	/* [한국어] rq 준비 시 rq->elv.priv[1] 에 저장해 두었던 bfqq 를 꺼낸다 - 이 rq 가
	 * 아직 어느 bfqq 에도 배정되지 않았다면 NULL 이 나올 수 있다(아래에서 검사). */
	struct bfq_data *bfqd;
	/* [한국어] bfqq 가 확정된 뒤에야 알 수 있는 소속 스케줄러 전역 상태 포인터. */
	unsigned long flags;
	/* [한국어] spin_lock_irqsave 로 인터럽트 상태를 저장할 변수 - completion 이 인터럽트
	 * 컨텍스트에서 올 수 있으므로 로컬 인터럽트를 반드시 함께 막아야 한다. */

	/*
	 * rq either is not associated with any icq, or is an already
	 * requeued request that has not (yet) been re-inserted into
	 * a bfq_queue.
	 */
	if (!rq->elv.icq || !bfqq)
		/* [한국어] icq 가 없다(애초에 bfq 가 관리하지 않는 rq) 또는 bfqq 가 없다(이미
		 * requeue 되어 priv 가 비워진 rq 에 대해 finish 훅이 재차 불린 경우) - 정리할
		 * 상태가 없으므로 바로 반환해 이중 처리(double-free 유사 상황)를 막는다. */
		return;

	/* [한국어] bfqq 로부터 소속 bfqd(스케줄러 전역 상태)를 얻는다 - 이후 락/통계 갱신에 사용. */
	bfqd = bfqq->bfqd;

	/* [한국어] rq 가 실제로 driver(NVMe controller)에 dispatch 된 적이 있는지 확인 -
	 * dispatch 되지 않고 취소된 rq 는 서비스 시간 통계에 반영하면 왜곡되므로 제외. */
	if (rq->rq_flags & RQF_STARTED)
		bfqg_stats_update_completion(bfqq_group(bfqq),
					     rq->start_time_ns,
					     rq->io_start_time_ns,
					     rq->cmd_flags);
		/* [한국어] cgroup(blkio) 통계에 이 rq 의 대기시간/서비스시간을 반영 - bfqq 가
		 * 속한 bfq_group 단위로 처리량/지연시간을 계정한다. */

	spin_lock_irqsave(&bfqd->lock, flags);
	/* [한국어] bfqd->lock 획득 + 로컬 인터럽트 비활성화 - 이 아래에서 bfqq/entity 필드를
	 * 갱신하는 동안 dispatch 경로(bfq_dispatch_request)나 다른 CPU 의 completion 이
	 * 동시에 같은 자료구조를 건드리지 못하게 막는다. */
	if (likely(rq->rq_flags & RQF_STARTED)) {
	/* [한국어] 실제 dispatch 이력이 있는 rq 에 대해서만 완료 회계를 수행 - dispatch 되지
	 * 않은 rq 는 bfqq 의 inflight/budget 계산에 애초에 반영된 적이 없다. */
		if (rq == bfqd->waited_rq)
			/* [한국어] 이 rq 가 bfq_select_queue() 가 injection 효과 측정용으로
			 * 지목해 둔 바로 그 request 라면, 완료 시점에 inject_limit 을 조정한다. */
			bfq_update_inject_limit(bfqd, bfqq);

		/* [한국어] bfqq/entity 의 dispatched 카운트, budget 소비, tot_rq_in_driver 등을
		 * 갱신하고 필요하면 idle 타이머를 무장하거나 in-service bfqq 만료를 판단한다 -
		 * NVMe CQ completion 이 도착했을 때 BFQ 스케줄 상태를 실제로 갱신하는 핵심 지점. */
		bfq_completed_request(bfqq, bfqd);
	}
	/* [한국어] bfqq(및 상위 entity 체인)의 "allocated request 수"를 1 감소 - 이 카운트는
	 * cgroup 별 동시 tag 사용 한도를 넘지 않도록 bfq_limit_depth 가 참조한다. */
	bfqq_request_freed(bfqq);
	/* [한국어] 이 rq 가 쥐고 있던 bfqq 참조를 반납(ref--) - 참조 카운트가 0 이 되면 bfqq
	 * 자체가 free 된다. rq 준비 시(bfq_init_rq) ref++ 했던 것과 짝을 이루는 해제. */
	bfq_put_queue(bfqq);
	/* [한국어] 이 rq 를 발급한 io_context(bic) 의 in-flight request 수를 감소 - 프로세스
	 * 단위 동시 발급 요청 수를 추적해 cooperating queue 병합/분리 판단 등에 쓰인다. */
	RQ_BIC(rq)->requests--;
	/* [한국어] 락 해제 및 인터럽트 상태 복원 - 위 임계구역에서의 자료구조 갱신이 끝났다. */
	spin_unlock_irqrestore(&bfqd->lock, flags);

	/*
	 * Reset private fields. In case of a requeue, this allows
	 * this function to correctly do nothing if it is spuriously
	 * invoked again on this same request (see the check at the
	 * beginning of the function). Probably, a better general
	 * design would be to prevent blk-mq from invoking the requeue
	 * or finish hooks of an elevator, for a request that is not
	 * referred by that elevator.
	 *
	 * Resetting the following fields would break the
	 * request-insertion logic if rq is re-inserted into a bfq
	 * internal queue, without a re-preparation. Here we assume
	 * that re-insertions of requeued requests, without
	 * re-preparation, can happen only for pass_through or at_head
	 * requests (which are not re-inserted into bfq internal
	 * queues).
	 */
	/* [한국어] bic 포인터 제거 - 위에서 이미 icq/bic 기반 정리를 마쳤으므로, 이 rq 가
	 * 재사용되더라도 낡은 bic 를 다시 참조하지 않도록 끊는다. */
	rq->elv.priv[0] = NULL;
	/* [한국어] bfqq 포인터 제거 - 함수 맨 위의 "!bfqq" 검사가 재호출 시 조기 반환하도록
	 * 만드는 핵심 장치이며, 재삽입 시에는 bfq_init_rq 가 이 필드들을 다시 채운다. */
	rq->elv.priv[1] = NULL;
}

/*
 * [한국어]
 * bfq_finish_request - elevator_ops.finish_request 콜백. request 가
 * 완전히 끝났을 때(더 이상 requeue 되지 않고 정말 해제될 때) 호출되어
 * bfq 상태 정리와 io_context 참조 해제를 함께 수행한다.
 *
 * @rq: 완전히 종료되는 request. bfq_finish_requeue_request 처리가 끝난
 *      뒤에는 rq->elv.priv[]가 이미 NULL 이 된 상태다.
 * @return: 없음(void).
 *
 * bfq_finish_requeue_request 하나만으로는 rq->elv.icq 에 걸린 io_context
 * 참조(ioc)까지는 정리하지 않는다 — requeue 되는 rq 는 같은 icq 를 다시
 * 쓸 수 있어야 하기 때문이다. 반면 이 함수가 불리는 시점은 rq 가 정말로
 * 폐기되는 시점이므로, io_context 참조도 함께 놓아 주어야 icq 의 참조
 * 카운트가 새지 않는다.
 * 실행 컨텍스트: blk-mq 의 request 완전 해제 경로(__blk_mq_free_request)에서
 * 호출되며, 내부에서 호출하는 bfq_finish_requeue_request 가 자체적으로
 * bfqd->lock 을 잡았다 놓으므로 이 함수 자체는 별도 락이 필요 없다.
 * caller: elevator_ops.finish_request 훅을 통해 blk-mq 코어가 호출.
 * callee: bfq_finish_requeue_request(), put_io_context().
 * 에러 경로: 없음 — icq 가 이미 없으면 단순히 두 번째 블록을 건너뛴다.
 *
 * 호출 체인:
 *   __blk_mq_free_request → elevator_ops.finish_request → [bfq_finish_request] → bfq_finish_requeue_request
 */
static void bfq_finish_request(struct request *rq)
{
	bfq_finish_requeue_request(rq);
	/* [한국어] bfqq/entity 관련 카운트 정리는 공통 로직인 bfq_finish_requeue_request 에
	 * 위임 - requeue 와 finish 가 동일하게 처리해야 하는 부분을 재사용한다. */

	if (rq->elv.icq) {
	/* [한국어] 이 rq 가 여전히 io_context 를 붙들고 있다면(정상적인 경우 대부분 그렇다) -
	 * 이제 rq 가 완전히 폐기되므로 그 io_context 참조도 함께 반납해야 한다. */
		put_io_context(rq->elv.icq->ioc);
		/* [한국어] icq 가 쥐고 있던 struct io_context 의 참조 카운트를 감소 - ioc 는
		 * 프로세스가 종료되거나 마지막 icq 가 해제될 때 실제로 free 된다. */
		rq->elv.icq = NULL;
		/* [한국어] icq 포인터 자체를 제거해, 이 rq 구조체가 재사용(mempool 재할당)될 때
		 * 낡은 icq 를 다시 가리키지 않도록 한다. */
	}
}

/*
 * [한국어]
 * bfq_split_bfqq - 현재 태스크와 공유(cooperating) 중이던 bfqq 사이의
 * 연결을 끊는다. bfqq 가 더 이상 다른 프로세스와 협력할 필요가 없어졌을
 * 때(예: 순차 I/O 패턴이 깨져 협력의 이득보다 손해가 커졌을 때) 호출되어,
 * 이 프로세스만을 위한 새 bfqq 를 만들 수 있도록 기존 연결을 해제한다.
 *
 * @bic: 현재 태스크의 BFQ io_context. bic_set_bfqq() 를 통해 bfqq 와의
 *       연결(bic->bfqq_data[]) 이 갱신된다.
 * @bfqq: 지금까지 이 태스크와 공유되던 bfq_queue. 다른 프로세스도 참조
 *        중이라면 참조 카운트만 낮아지고, 이 태스크가 유일한 참조자였다면
 *        오히려 그대로 재사용된다(아래 참고).
 * @return: bfqq 를 그대로 재사용해도 되면 bfqq 자기 자신을 반환(호출자는
 *          bic 를 이 bfqq 에 다시 연결); 새 bfqq 를 할당해야 하면 NULL 을
 *          반환(호출자가 bfq_get_queue() 등으로 새로 만든다).
 *
 * BFQ 의 협력(cooperation) 메커니즘은 서로 다른 프로세스가 인접한 섹터를
 * 순차적으로 읽고 쓸 때 하나의 bfqq 로 묶어 큐 분할 오버헤드를 줄인다.
 * 하지만 그 패턴이 깨지면(다른 프로세스의 I/O 로 인해 seek 이 잦아지면)
 * 오히려 손해이므로 분리해야 한다. 이 함수는 (a) 이 bfqq 를 참조하는
 * 프로세스가 이 태스크 하나뿐이고 아직 merge 대상(new_bfqq)이 없다면,
 * bfqq 를 그대로 이 태스크 전용으로 재사용하도록 pid/coop 플래그만
 * 정리해 반환하고, (b) 그렇지 않다면(여전히 다른 프로세스와 공유 중이면)
 * bic 의 bfqq 연결을 끊고 협력자 링크(new_bfqq 체인)에서도 빠져나오게 한
 * 뒤 NULL 을 반환해 호출자가 새 bfqq 를 만들게 한다.
 * 실행 컨텍스트: bfq_get_bfqq_handle_split() 에서 bfqd->lock 을 쥔 상태로
 * 호출되는 request 준비 경로의 일부.
 * caller: bfq_get_bfqq_handle_split() 에서 "너무 오래 seek 가 잦았던"
 *         bfqq 를 분리할 때 호출.
 * callee: bfqq_process_refs(), bic_set_bfqq(), bfq_put_cooperator(),
 *         bfq_release_process_ref().
 * 에러 경로: 없음 — 반환값(NULL 또는 bfqq)으로 이후 처리를 분기시킬 뿐이다.
 *
 * 호출 체인:
 *   bfq_get_bfqq_handle_split → [bfq_split_bfqq] → bfq_release_process_ref
 */
static struct bfq_queue *
bfq_split_bfqq(struct bfq_io_cq *bic, struct bfq_queue *bfqq)
{
	bfq_log_bfqq(bfqq->bfqd, bfqq, "splitting queue");
	/* [한국어] BFQ 트레이스 로그 - 이 bfqq 가 분리(split) 절차에 들어감을 기록해
	 * 디버깅 시 협력 관계 해제 시점을 추적할 수 있게 한다. */

	if (bfqq_process_refs(bfqq) == 1 && !bfqq->new_bfqq) {
	/* [한국어] 이 bfqq 를 참조하는 프로세스가 이 태스크 하나뿐이고(process_refs==1),
	 * 아직 다른 bfqq 로 병합(merge)되도록 지정된 상태(new_bfqq)도 아니라면 - 굳이
	 * 새 bfqq 를 만들 필요 없이 이 bfqq 를 그대로 이 태스크 전용으로 재사용 가능. */
		bfqq->pid = current->pid;
		/* [한국어] bfqq 소유권을 명시적으로 현재 태스크의 pid 로 갱신 - 이후
		 * bfq_bfqq_should_merge() 등 비교 로직이 이 값을 참조한다. */
		bfq_clear_bfqq_coop(bfqq);
		/* [한국어] "협력 중" 플래그 해제 - 더 이상 다른 프로세스와 묶인 큐가 아님을
		 * 표시해, 이후 병합 후보 탐색에서 협력 대상으로 다시 고려되지 않게 한다. */
		bfq_clear_bfqq_split_coop(bfqq);
		/* [한국어] "분리가 필요했던 협력 큐" 플래그도 해제 - 이번 분리로 그 상태가
		 * 해소되었음을 표시(재분리 판단 로직의 중복 트리거 방지). */
		return bfqq;
		/* [한국어] 새로 할당할 필요 없이 기존 bfqq 를 그대로 반환 - 호출자는 이
		 * bfqq 를 계속 사용한다. */
	}

	bic_set_bfqq(bic, NULL, true, bfqq->actuator_idx);
	/* [한국어] bic(이 태스크의 io_context)와 bfqq 사이의 연결을 끊는다(bic->bfqq_data
	 * 에서 이 actuator 슬롯을 NULL 로) - 다른 프로세스도 참조 중인 bfqq 이므로 이
	 * 태스크만 분리되고, bfqq 자체는 다른 프로세스를 위해 계속 살아있을 수 있다. */

	bfq_put_cooperator(bfqq);
	/* [한국어] 이 bfqq 가 협력 체인(new_bfqq 링크)에 참여 중이었다면 그 링크에서도
	 * 빠져나오게 하고 필요한 참조 카운트를 정리한다. */

	bfq_release_process_ref(bfqq->bfqd, bfqq);
	/* [한국어] 이 태스크가 쥐고 있던 process 참조를 반납(ref--) - 다른 참조자가
	 * 없다면 이 호출로 bfqq 가 실제로 해제될 수도 있다. */
	return NULL;
	/* [한국어] 이 태스크를 위한 새 bfqq 가 필요함을 호출자에게 알린다 - 호출자
	 * (bfq_get_bfqq_handle_split)는 __bfq_get_bfqq_handle_split 을 다시 불러
	 * split=true 로 새 큐를 할당받는다. */
}

/*
 * [한국어]
 * __bfq_get_bfqq_handle_split - bic 에 이미 연결된 bfqq 가 있으면 그것을
 * 재사용하고, 없거나 oom_bfqq(메모리 부족 시의 임시 폴백 큐) 뿐이라면
 * bfq_get_queue() 로 새 bfqq 를 할당한 뒤, split(분리 직후) 이었다면
 * burst(대량 생성) 관련 상태를 복원한다.
 *
 * @bfqd: 이 device 의 BFQ 전역 상태 - bfq_get_queue() 호출과 large_burst
 *        판정에 사용.
 * @bic: 현재 태스크의 BFQ io_context. bfqq_data[act_idx] 에 was_in_burst_list/
 *       saved_in_large_burst 같은 "분리 전에 저장해 둔 상태"가 들어 있다.
 * @bio: 이번 I/O 가 속한 bio - actuator_index 계산과 bfq_get_queue() 의
 *       ioprio/cgroup 판단에 쓰인다.
 * @split: 이 호출이 "협력 관계가 막 깨져 분리된 직후"의 재획득인지 여부.
 *         true 면 burst_list 복원과 split_time 기록을 수행한다.
 * @is_sync: 동기 I/O(예: 읽기, O_DIRECT 쓰기) 인지 여부 - bic_to_bfqq()/
 *           bfq_get_queue() 가 sync/async 큐를 구분하는 데 사용.
 * @new_queue: [출력] 이번에 실제로 새 bfqq 를 할당했다면 true 로 설정되어
 *             호출자가 이후의 seeky/coop 판정을 건너뛰게 한다.
 * @return: 이 bio 가 속해야 할 bfq_queue 포인터(항상 유효, NULL 없음 —
 *          최악의 경우 bfqd->oom_bfqq 를 반환).
 *
 * bic_to_bfqq() 로 얻은 기존 bfqq 가 있고 그것이 oom_bfqq(진짜 큐를 만들
 * 여유조차 없을 때 쓰는 fallback)가 아니면 그대로 재사용한다 — 이것이
 * 가장 흔한 빠른 경로다. 그렇지 않으면(첫 I/O 이거나 이전에 OOM 으로
 * 임시 큐를 썼던 경우) bfq_get_queue() 로 진짜 bfqq 를 새로 만든다.
 * 마지막으로, 이 호출이 방금 협력 관계가 깨져 분리(split)된 직후라면,
 * 분리되기 전 이 프로세스가 "burst(짧은 시간에 다수 큐가 한꺼번에 생성)"
 * 목록에 있었는지, large burst 로 판정됐었는지를 bic 에 저장해 둔 값에서
 * 복원해 새 bfqq 에도 이어 붙인다 — 그렇지 않으면 분리 때마다 burst
 * 감지가 리셋되어 통계가 왜곡된다.
 * 실행 컨텍스트: request 준비 경로(bfq_init_rq)의 일부로 bfqd->lock 을
 * 쥔 채 호출된다.
 * caller: bfq_get_bfqq_handle_split() (최초 조회와, split 이후 재조회 두 번
 *         모두 이 함수를 거친다).
 * callee: bic_to_bfqq(), bfq_put_queue(), bfq_get_queue(), bic_set_bfqq(),
 *         bfq_mark/clear_bfqq_in_large_burst(), hlist_add_head().
 * 에러 경로: bfq_get_queue() 가 실패하면(커널 내부적으로 oom_bfqq 를 반환)
 *           그대로 통과시켜 상위에서 oom_bfqq 여부를 검사하게 한다.
 *
 * 호출 체인:
 *   bfq_get_bfqq_handle_split → [__bfq_get_bfqq_handle_split] → bfq_get_queue
 */
static struct bfq_queue *
__bfq_get_bfqq_handle_split(struct bfq_data *bfqd, struct bfq_io_cq *bic,
			    struct bio *bio, bool split, bool is_sync,
			    bool *new_queue)
{
	unsigned int act_idx = bfq_actuator_index(bfqd, bio);
	/* [한국어] 이 bio 가 속한 독립 접근 영역(actuator) 인덱스를 계산 - 멀티 actuator
	 * NVMe/HDD 에서 각 actuator 마다 별도의 bfqq 슬롯(bic->bfqq_data[act_idx])을
	 * 유지하기 위한 키 값이다. */
	struct bfq_queue *bfqq = bic_to_bfqq(bic, is_sync, act_idx);
	/* [한국어] 이 프로세스가 이 actuator/sync 조합에 대해 이미 갖고 있는 bfqq 를 조회 -
	 * 첫 I/O 라면 NULL 일 수 있다. */
	struct bfq_iocq_bfqq_data *bfqq_data = &bic->bfqq_data[act_idx];
	/* [한국어] 이 프로세스·actuator 조합에 대해 bic 가 보관 중인 부가 상태(분리 전
	 * burst 참여 여부 등)를 가리키는 포인터 - split 처리 시 참조. */

	if (likely(bfqq && bfqq != &bfqd->oom_bfqq))
		/* [한국어] 이미 진짜 bfqq 를 갖고 있다면(oom 폴백이 아니라면) - 가장 흔한
		 * 경로이므로 likely() 로 분기 예측 힌트를 준다. */
		return bfqq;
		/* [한국어] 기존 bfqq 를 그대로 재사용 - 새로 할당할 필요가 없다. */

	if (new_queue)
		*new_queue = true;
		/* [한국어] 호출자가 관심 있어 하면 "이번에 새 큐를 만들었다"는 사실을
		 * 알려준다 - bfq_get_bfqq_handle_split 은 이 값이 true 면 seeky/coop
		 * 판정을 건너뛰고 곧바로 새 bfqq 를 반환한다(막 생성된 큐이므로). */

	if (bfqq)
		/* [한국어] 기존에 oom_bfqq(진짜 큐가 아닌 임시 폴백)를 갖고 있었다면 -
		 * 이제 진짜 큐로 교체할 것이므로 그 임시 참조를 먼저 반납한다. */
		bfq_put_queue(bfqq);
	/* [한국어] 진짜 bfq_queue 를 새로 찾거나 할당 - ioprio/cgroup 기준으로 기존
	 * 큐를 재사용할 수도 있고, 정말 새로 kmem_cache 할당할 수도 있다(내부에서
	 * 메모리 부족 시 oom_bfqq 로 폴백). */
	bfqq = bfq_get_queue(bfqd, bio, is_sync, bic, split);

	/* [한국어] 이 프로세스·actuator 슬롯에 새로 얻은 bfqq 를 연결 - 다음 I/O 부터는
	 * bic_to_bfqq() 로 곧장 이 bfqq 를 찾을 수 있게 된다. */
	bic_set_bfqq(bic, bfqq, is_sync, act_idx);
	/* [한국어] 이번 호출이 "협력 관계가 깨져 분리된 직후"이고 동기 I/O 라면 - burst
	 * 감지 상태를 새 bfqq 로 이어 붙여야 한다(비동기 큐는 burst 판정 대상이 아님). */
	if (split && is_sync) {
		if ((bfqq_data->was_in_burst_list && bfqd->large_burst) ||
		    /* [한국어] 분리 전에 burst 목록에 있었고 현재 large_burst 상태이거나,
		     * 이미 large burst 로 저장돼 있었다면 - 새 bfqq 도 곧바로 large burst
		     * 로 표시해 weight-raise 배제 등의 정책을 이어가게 한다. */
		    bfqq_data->saved_in_large_burst)
			bfq_mark_bfqq_in_large_burst(bfqq);
		else {
		/* [한국어] large burst 조건이 아니라면 일단 large burst 표시를 지운다 -
		 * 아래에서 필요 시 burst_list 에만 재등록한다. */
			bfq_clear_bfqq_in_large_burst(bfqq);
			if (bfqq_data->was_in_burst_list)
				/*
				 * If bfqq was in the current
				 * burst list before being
				 * merged, then we have to add
				 * it back. And we do not need
				 * to increase burst_size, as
				 * we did not decrement
				 * burst_size when we removed
				 * bfqq from the burst list as
				 * a consequence of a merge
				 * (see comments in
				 * bfq_put_queue). In this
				 * respect, it would be rather
				 * costly to know whether the
				 * current burst list is still
				 * the same burst list from
				 * which bfqq was removed on
				 * the merge. To avoid this
				 * cost, if bfqq was in a
				 * burst list, then we add
				 * bfqq to the current burst
				 * list without any further
				 * check. This can cause
				 * inappropriate insertions,
				 * but rarely enough to not
				 * harm the detection of large
				 * bursts significantly.
				 */
				/* [한국어] 원본 주석대로, 정확한 burst 목록 일치 여부를 따지는
				 * 비용을 피하려고 "예전에 burst 목록에 있었다"는 사실만으로
				 * 무조건 현재 burst_list 에 재등록한다 - 드물게 부정확할 수
				 * 있으나 large burst 탐지에 미치는 영향은 미미하다. */
				hlist_add_head(&bfqq->burst_list_node,
					       &bfqd->burst_list);
		}
		/* [한국어] 분리가 일어난 시각을 기록 - 이후 "너무 빨리 다시 분리/병합을
		 * 반복하는지" 등을 판단하는 데 참조 시각으로 쓰인다. */
		bfqq->split_time = jiffies;
	}

	/* [한국어] 이번 bio 가 실제로 속할 bfqq 를 호출자에게 반환. */
	return bfqq;
}

/*
 * Only reset private fields. The actual request preparation will be
 * performed by bfq_init_rq, when rq is either inserted or merged. See
 * comments on bfq_init_rq for the reason behind this delayed
 * preparation.
 */
/*
 * [한국어]
 * bfq_prepare_request - elevator_ops.prepare_request 콜백. blk-mq 가
 * request 구조체를 막 할당했을 때 호출되어, 이번 rq 가 사용할 io_context
 * (icq)를 미리 찾아 두고, 이전에 이 rq 슬롯을 썼던 다른 request 의 흔적
 * (bic/bfqq 포인터)을 지운다.
 *
 * @rq: blk-mq 가 방금 할당한(아직 bio 가 완전히 붙지 않았을 수도 있는)
 *      request. rq->elv.icq, rq->elv.priv[0]/[1] 이 이 함수에서 설정된다.
 * @return: 없음(void).
 *
 * request 구조체는 mempool 에서 재사용되므로, 새로 할당된 rq 라도
 * elv.priv[] 필드에는 과거에 그 슬롯을 쓰던 request 의 bic/bfqq 포인터가
 * 그대로 남아 있을 수 있다. 이 함수는 그런 낡은 포인터를 확실히 지워서
 * 이후 bfq_init_rq() 가 이 rq 를 초기 상태로 취급하게 만든다. 또한 이
 * 시점에 미리 icq(이 프로세스의 io_context 를 elevator 가 보는 관점)를
 * 찾아 rq 에 매달아 두면, 실제 request 준비(bfq_init_rq)가 나중에
 * 지연되더라도 icq 조회를 다시 할 필요가 없다.
 * 실행 컨텍스트: blk-mq 의 request 할당 경로(제출 프로세스 컨텍스트)에서
 * 호출되며, 아직 bfqd->lock 이 필요한 상태 변경은 하지 않는다.
 * caller: blk-mq 코어가 elevator_ops.prepare_request 훅을 통해 호출.
 * callee: ioc_find_get_icq().
 * 에러 경로: icq 를 찾지 못하면(예: io_context 가 없는 커널 스레드의 I/O)
 *           rq->elv.icq 가 NULL 로 남고, 이후 bfq_init_rq()/
 *           bfq_finish_requeue_request() 가 이를 검사해 이 rq 를 bfq 관리
 *           대상에서 제외한다.
 *
 * 호출 체인:
 *   blk_mq_get_request → elevator_ops.prepare_request → [bfq_prepare_request]
 */
static void bfq_prepare_request(struct request *rq)
{
	rq->elv.icq = ioc_find_get_icq(rq->q);
	/* [한국어] 현재 프로세스의 io_context 에 대응하는 icq(I/O Context per-queue)
	 * 를 찾아 참조를 얻어 rq 에 매단다 - icq 는 이후 icq_to_bic() 로 bfq_io_cq
	 * 로 캐스팅되어 bfqq 조회의 시작점이 된다. */

	/*
	 * Regardless of whether we have an icq attached, we have to
	 * clear the scheduler pointers, as they might point to
	 * previously allocated bic/bfqq structs.
	 */
	rq->elv.priv[0] = rq->elv.priv[1] = NULL;
	/* [한국어] priv[0](bic), priv[1](bfqq) 를 모두 NULL 로 초기화 - rq 가 request
	 * mempool 에서 재사용된 경우 남아있을 수 있는 이전 소유자의 포인터를 지워,
	 * bfq_init_rq() 가 "아직 배정되지 않은 rq"로 올바르게 인식하게 한다. */
}

/*
 * [한국어]
 * bfq_waker_bfqq - bfqq 를 "깨우는(waker)" 관계에 있는 bfqq(waker_bfqq)가
 * 여전히 유효한 참조인지 확인하고, 유효하면 반환한다. waker 관계란 한
 * bfqq(waker)의 I/O 완료가 다른 bfqq(현재 bfqq)의 I/O 를 유발하는 인과
 * 관계(예: 저널 쓰기 완료 후 데이터 쓰기가 뒤따르는 패턴)를 뜻한다.
 *
 * @bfqq: waker 관계를 확인하려는 대상 bfqq. bfqq->waker_bfqq 가 후보이고,
 *        bfqq->new_bfqq 체인(병합으로 흡수된 큐들의 연결 리스트)도 함께
 *        검사한다.
 * @return: 여전히 참조 가능한 waker_bfqq 포인터, 또는 이미 소멸(참조 카운트
 *          0) 했거나 판단이 불확실하면 NULL.
 *
 * bfqq 는 병합(merge)되어 다른 bfqq 로 흡수될 수 있는데, 이 경우 원래의
 * waker_bfqq 포인터가 이미 해제된 bfq_queue 를 가리키게 될 위험이 있다.
 * 이 함수는 두 단계로 안전성을 검사한다: (1) new_bfqq 체인을 따라가며
 * waker_bfqq 가 그 병합 체인 안에 있는지 확인하고, 있다면 그 큐를 아직
 * 참조하는 프로세스가 이 하나뿐인지(process_refs==1) 봐서 마지막
 * 참조자라면 곧 해제될 것이므로 NULL 을 반환한다. (2) 병합 체인에 없다면,
 * waker_bfqq 자체의 참조 카운트가 이미 0인지 확인해 0이면 NULL 을
 * 반환한다. 이 방어적 검사가 없으면 use-after-free 로 이어질 수 있다.
 * 실행 컨텍스트: bfq_get_bfqq_handle_split() 에서 bfqd->lock 을 쥔 채
 * 호출되는 request 준비 경로의 일부.
 * caller: bfq_get_bfqq_handle_split() (분리 직전에 현재 waker 관계를
 *         보존해 새 bfqq 에 이어 붙이기 위해 호출).
 * callee: bfqq_process_refs().
 * 에러 경로: 없음 — 반환값(NULL 여부)으로 안전성 여부만 알려준다.
 *
 * 호출 체인:
 *   bfq_get_bfqq_handle_split → [bfq_waker_bfqq] → bfqq_process_refs
 */
static struct bfq_queue *bfq_waker_bfqq(struct bfq_queue *bfqq)
{
	struct bfq_queue *new_bfqq = bfqq->new_bfqq;
	/* [한국어] bfqq 가 다른 bfqq 로 병합되었다면 그 병합 대상(new_bfqq) - 병합
	 * 체인을 따라가며 waker_bfqq 가 그 안에 있는지 검사하는 시작점. */
	struct bfq_queue *waker_bfqq = bfqq->waker_bfqq;
	/* [한국어] 검사 대상인 waker 후보 포인터를 로컬 변수에 저장 - 아래에서 안전성을
	 * 확인한 뒤 그대로 반환하거나 NULL 로 대체한다. */

	if (!waker_bfqq)
		return NULL;
		/* [한국어] 애초에 waker 관계가 설정된 적이 없다면 - 확인할 것도 없이 NULL. */

	while (new_bfqq) {
	/* [한국어] bfqq 가 병합되어 흡수된 큐들의 체인(new_bfqq -> new_bfqq -> ...)을
	 * 따라가며, waker_bfqq 가 이 체인 안의 큐 중 하나와 같은지 검사한다 - 같다면
	 * waker_bfqq 는 "이미 병합되어 없어질 예정인 큐"라는 뜻이다. */
		if (new_bfqq == waker_bfqq) {
		/* [한국어] waker_bfqq 가 병합 체인 안에서 발견됐다 - 이 경우 그 큐가 아직
		 * 다른 프로세스에게도 유효한 참조인지 추가로 확인해야 한다. */
			/*
			 * If waker_bfqq is in the merge chain, and current
			 * is the only process, waker_bfqq can be freed.
			 */
			if (bfqq_process_refs(waker_bfqq) == 1)
				return NULL;
				/* [한국어] 이 병합 체인을 참조하는 프로세스가 하나뿐이라면 -
				 * 곧 그 유일한 참조자마저 정리되면 waker_bfqq 가 해제될 것이므로
				 * 안전하게 재사용할 수 없다고 보고 NULL 을 반환한다. */

			return waker_bfqq;
			/* [한국어] 참조자가 둘 이상이면 아직 살아있을 것이 보장되므로 그대로
			 * 반환해 새 bfqq 에도 이 waker 관계를 이어 붙일 수 있게 한다. */
		}

		new_bfqq = new_bfqq->new_bfqq;
		/* [한국어] 병합 체인의 다음 링크로 이동해 검사를 계속한다. */
	}

	/*
	 * If waker_bfqq is not in the merge chain, and it's procress reference
	 * is 0, waker_bfqq can be freed.
	 */
	if (bfqq_process_refs(waker_bfqq) == 0)
		return NULL;
		/* [한국어] 병합 체인에는 없었지만, waker_bfqq 자체의 process 참조가 이미
		 * 0 이라면(다른 경로로 이미 해제 대상이 되었다면) - 역시 안전하지 않으므로
		 * NULL 을 반환한다. */

	return waker_bfqq;
	/* [한국어] 병합 체인에도 없고 참조도 살아있다 - 안전한 waker_bfqq 이므로 그대로
	 * 반환한다. */
}

/*
 * [한국어]
 * bfq_get_bfqq_handle_split - 이 bio 를 처리할 bfq_queue 를 얻되, 그 큐가
 * 오랫동안 seek 위주(비순차) I/O 를 보여 협력(cooperation) 이점이 사라진
 * 상태라면 분리(split)해서 이 프로세스 전용의 새 bfqq 로 갈아 끼운다.
 *
 * @bfqd: 이 device 의 BFQ 전역 상태.
 * @bic: 현재 태스크의 BFQ io_context.
 * @bio: 이번 요청이 속한 bio.
 * @idx: bfq_actuator_index() 로 계산된 actuator 인덱스 - bic->bfqq_data[idx]
 *       에 접근할 때 사용.
 * @is_sync: 동기 I/O 여부.
 * @return: 이 bio 가 최종적으로 속해야 할 bfq_queue 포인터(항상 유효).
 *
 * 먼저 __bfq_get_bfqq_handle_split() 으로 기존/새 bfqq 를 얻는다. 만약
 * 방금 새로 생성된 큐라면(new_queue==true) 분리를 고려할 필요조차 없이
 * 즉시 반환한다. 기존 큐라면, 그 큐가 "협력 중(coop)"이면서 "분리가
 * 필요하다고 표시된(split_coop)" 상태이고 아직 안정적으로 병합된
 * (stably_merged) 것이 아니라면, 이 프로세스만 따로 떼어낸다: 먼저 현재의
 * waker 관계를 안전하게 보존(bfq_waker_bfqq)하고, large-burst 상태를 bic 에
 * 저장한 뒤, bfq_split_bfqq() 로 실제 분리를 수행한다. bfq_split_bfqq() 가
 * 기존 bfqq 를 그대로 반환하면(이 프로세스가 마지막 참조자였던 경우) 그
 * 상태를 복원해 반환하고, NULL 을 반환하면(여전히 다른 프로세스와 공유
 * 중이었던 경우) __bfq_get_bfqq_handle_split() 을 split=true 로 다시 호출해
 * 이 프로세스만의 새 bfqq 를 만들고 waker 관계를 새 큐에 이어 붙인다.
 * 실행 컨텍스트: bfq_init_rq() 에서 bfqd->lock 을 쥔 채 호출되는 request
 * 준비 경로.
 * caller: bfq_init_rq().
 * callee: __bfq_get_bfqq_handle_split(), bfq_waker_bfqq(), bfq_split_bfqq(),
 *         bfq_bfqq_resume_state(), hlist_add_head().
 * 에러 경로: 특별한 실패 경로는 없다 — 모든 갈래가 유효한 bfqq(필요하면
 *           oom_bfqq)를 반환한다.
 *
 * 호출 체인:
 *   bfq_init_rq → [bfq_get_bfqq_handle_split] → bfq_split_bfqq / __bfq_get_bfqq_handle_split
 */
static struct bfq_queue *bfq_get_bfqq_handle_split(struct bfq_data *bfqd,
						   struct bfq_io_cq *bic,
						   struct bio *bio,
						   unsigned int idx,
						   bool is_sync)
{
	struct bfq_queue *waker_bfqq;
	/* [한국어] 분리 전에 보존해 둘 waker 관계 - 아래에서 실제로 분리가 일어날 때만
	 * 채워지고, 새 bfqq 에 이어 붙이는 데 쓰인다. */
	struct bfq_queue *bfqq;
	/* [한국어] 이 함수가 최종적으로 반환할 bfq_queue. */
	bool new_queue = false;
	/* [한국어] __bfq_get_bfqq_handle_split 가 "이번에 새로 생성했다"고 알려주는
	 * 출력 플래그 - true 면 분리 판단 자체를 건너뛴다(막 만든 큐이므로 seek 이력이
	 * 있을 수 없다). */

	bfqq = __bfq_get_bfqq_handle_split(bfqd, bic, bio, false, is_sync,
					   &new_queue);
	/* [한국어] 아직 분리 상황이 아니므로 split=false 로 호출해 기존 bfqq 를 얻는다
	 * (또는 정말 처음이면 새로 만든다). */
	if (unlikely(new_queue))
		return bfqq;
		/* [한국어] 방금 새로 만들어진 큐라면 seek 이력이 없으므로 분리 판단 없이
		 * 곧바로 반환한다. */

	/* If the queue was seeky for too long, break it apart. */
	if (!bfq_bfqq_coop(bfqq) || !bfq_bfqq_split_coop(bfqq) ||
	    bic->bfqq_data[idx].stably_merged)
		return bfqq;
		/* [한국어] 애초에 협력 중이 아니거나(coop 플래그 없음), 분리가 필요하다는
		 * 신호(split_coop)가 없거나, 이미 "안정적으로 병합됨" 판정을 받은
		 * 큐라면 - 분리하지 않고 그대로 반환한다(안정 병합은 seek 패턴이
		 * 일시적으로 나빠져도 유지할 가치가 있다고 판단된 상태). */

	waker_bfqq = bfq_waker_bfqq(bfqq);
	/* [한국어] 분리로 인해 bfqq 참조를 잃기 전에, 현재 유효한 waker 관계를 안전하게
	 * 확보해 둔다 - 분리 후 새로 만들어질 bfqq 에도 이 인과관계를 이어줘야
	 * injection/waking 로직이 계속 정확히 동작한다. */

	/* Update bic before losing reference to bfqq */
	if (bfq_bfqq_in_large_burst(bfqq))
		bic->bfqq_data[idx].saved_in_large_burst = true;
		/* [한국어] 분리로 이 bfqq 에 대한 참조를 놓기 전에, "large burst 상태였다"는
		 * 사실을 bic 에 저장해 둔다 - 이후 재획득(__bfq_get_bfqq_handle_split)
		 * 시 이 저장값을 읽어 새 bfqq 에도 large burst 상태를 이어 붙인다. */

	bfqq = bfq_split_bfqq(bic, bfqq);
	/* [한국어] 실제 분리 시도 - 이 프로세스가 마지막 참조자였다면 기존 bfqq 를 그대로
	 * 재사용하도록 반환하고, 아니라면 bic 연결만 끊고 NULL 을 반환한다. */
	if (bfqq) {
	/* [한국어] bfq_split_bfqq 가 bfqq 를 그대로 반환했다 = 이 프로세스가 유일한
	 * 참조자였어서 새로 만들 필요 없이 재사용 가능하다는 뜻. */
		bfq_bfqq_resume_state(bfqq, bfqd, bic, true);
		/* [한국어] bic 에 저장돼 있던 이전 상태(ioprio, weight-raise 등)를 이
		 * bfqq 에 복원 - true 인자는 "분리로 인한 복원"임을 표시. */
		return bfqq;
	}

	/* [한국어] 여전히 다른 프로세스와 공유 중이어서 분리가 실제로 일어났다 - 이번엔
	 * split=true 로 호출해 이 프로세스 전용의 완전히 새로운 bfqq 를 할당받는다. */
	bfqq = __bfq_get_bfqq_handle_split(bfqd, bic, bio, true, is_sync, NULL);
	/* [한국어] 메모리 부족으로 새 큐 할당에 실패해 oom_bfqq 로 폴백된 경우 -
	 * waker 관계 등 추가 상태 복원은 의미가 없으므로 그대로 반환. */
	if (unlikely(bfqq == &bfqd->oom_bfqq))
		return bfqq;

	/* [한국어] 새로 만든 bfqq 에 bic 가 저장해 둔 이전 상태를 복원 - false 인자는
	 * "완전히 새로 생성된 큐로의 복원"임을 표시(위의 true 케이스와 구분). */
	bfq_bfqq_resume_state(bfqq, bfqd, bic, false);
	/* [한국어] 분리 전에 확보해 둔 waker 관계를 새 bfqq 로 이어 붙인다. */
	bfqq->waker_bfqq = waker_bfqq;
	/* [한국어] "아직 확정되지 않은 waker 후보" 필드는 새 큐이므로 초기화 - 이후
	 * bfq_check_waker() 가 새로 관찰하며 채워 나간다. */
	bfqq->tentative_waker_bfqq = NULL;

	/*
	 * If the waker queue disappears, then new_bfqq->waker_bfqq must be
	 * reset. So insert new_bfqq into the
	 * woken_list of the waker. See
	 * bfq_check_waker for details.
	 */
	/* [한국어] waker_bfqq 가 나중에 소멸할 때 이 새 bfqq->waker_bfqq 포인터도
	 * 함께 정리(리셋)될 수 있도록, waker 쪽의 woken_list 에 이 bfqq 를
	 * 등록해 둔다 - 상세 정리 로직은 bfq_check_waker() 참고. */
	if (waker_bfqq)
		hlist_add_head(&bfqq->woken_list_node,
			       &bfqq->waker_bfqq->woken_list);

	/* [한국어] 분리를 거쳐 이 프로세스 전용으로 확정된 새 bfqq 를 반환. */
	return bfqq;
}

/*
 * If needed, init rq, allocate bfq data structures associated with
 * rq, and increment reference counters in the destination bfq_queue
 * for rq. Return the destination bfq_queue for rq, or NULL is rq is
 * not associated with any bfq_queue.
 *
 * This function is invoked by the functions that perform rq insertion
 * or merging. One may have expected the above preparation operations
 * to be performed in bfq_prepare_request, and not delayed to when rq
 * is inserted or merged. The rationale behind this delayed
 * preparation is that, after the prepare_request hook is invoked for
 * rq, rq may still be transformed into a request with no icq, i.e., a
 * request not associated with any queue. No bfq hook is invoked to
 * signal this transformation. As a consequence, should these
 * preparation operations be performed when the prepare_request hook
 * is invoked, and should rq be transformed one moment later, bfq
 * would end up in an inconsistent state, because it would have
 * incremented some queue counters for an rq destined to
 * transformation, without any chance to correctly lower these
 * counters back. In contrast, no transformation can still happen for
 * rq after rq has been inserted or merged. So, it is safe to execute
 * these preparation operations when rq is finally inserted or merged.
 */
/*
 * [한국어]
 * bfq_init_rq - request rq 가 BFQ 내부 큐에 삽입되거나 다른 rq 와 병합될
 * 때 호출되어, 이 rq 가 속할 bfq_io_cq(bic)와 bfq_queue(bfqq)를 찾거나
 * 새로 만들고, rq->elv.priv[]에 확정한다. BFQ 로 들어오는 모든 request 가
 * 어느 bfqq(프로세스/cgroup/actuator 조합)에 속하는지를 결정하는 지점이다.
 *
 * @rq: 삽입 또는 병합되는 request. rq->bio 로부터 actuator/cgroup 정보를
 *      얻고, rq->elv.icq(이미 bfq_prepare_request 에서 채워짐)로부터
 *      io_context 를 얻는다.
 * @return: 이 rq 가 속하는 bfq_queue 포인터. rq 에 icq 가 없으면(bfq 가
 *          관리하지 않는 request) NULL 을 반환한다.
 *
 * 이 함수의 준비 작업(bic/bfqq 조회, 참조 카운트 증가)이 bfq_prepare_request
 * 에서 곧바로 이뤄지지 않고 삽입/병합 시점까지 지연되는 이유는 파일 상단의
 * 긴 주석에 설명돼 있다: prepare_request 시점 이후에도 rq 가 icq 없는
 * request 로 변형될 수 있는데, 그 경우 이미 증가시킨 큐 카운터를 되돌릴
 * 훅이 없기 때문이다. 삽입/병합 시점 이후에는 그런 변형이 다시 일어나지
 * 않으므로 안전하게 카운터를 증가시킬 수 있다. 동작 순서는: (1) icq 가
 * 없으면 즉시 NULL, (2) 이미 RQ_BFQQ(rq) 가 설정돼 있으면(재호출) 그대로
 * 반환, (3) icq 로부터 bic 를 얻고 ioprio/cgroup 변경을 반영, (4)
 * bfq_get_bfqq_handle_split() 으로 실제 bfqq 를 확정, (5) allocated/ref
 * 카운트를 올리고 rq->elv.priv[]에 bic/bfqq 를 저장, (6) 이 bfqq 가 유일한
 * 참조자를 갖는다면 bfqq->bic 를 직접 연결(fast-path 최적화), (7) 새로
 * 생성된 bfqq 가 burst(짧은 시간 내 다수 큐 동시 생성) 패턴에 해당하면
 * bfq_handle_burst() 로 넘겨 weight-raise 여부를 판단시킨다.
 * 실행 컨텍스트: bfq_insert_request()/병합 경로에서 bfqd->lock 을 쥔 채
 * 호출된다.
 * caller: bfq_insert_request(), bfq_bio_merge() 등 rq 가 실제로 BFQ 내부
 *         자료구조에 편입되는 지점.
 * callee: icq_to_bic(), bfq_check_ioprio_change(), bfq_bic_update_cgroup(),
 *         bfq_get_bfqq_handle_split(), bfqq_request_allocated(),
 *         bfq_handle_burst().
 * 에러 경로: icq 가 없으면 NULL 반환 — 호출자는 이 rq 를 BFQ 관리 밖의
 *           request 로 취급(pass-through 등)한다.
 *
 * 호출 체인:
 *   bfq_insert_request → [bfq_init_rq] → bfq_get_bfqq_handle_split
 */
static struct bfq_queue *bfq_init_rq(struct request *rq)
{
	struct request_queue *q = rq->q;
	/* [한국어] 이 rq 가 속한 request_queue - elevator(BFQ) 인스턴스를 찾기 위한 경유지. */
	struct bio *bio = rq->bio;
	/* [한국어] rq 의 첫 bio - actuator_index 계산과 ioprio/cgroup 판단의 입력이 된다. */
	struct bfq_data *bfqd = q->elevator->elevator_data;
	/* [한국어] 이 request_queue 에 연결된 BFQ 전역 상태 - bfq_init_queue() 가 등록해
	 * 둔 그 bfqd 포인터를 elevator_data 에서 꺼낸다. */
	struct bfq_io_cq *bic;
	/* [한국어] 이 rq 를 발급한 프로세스의 BFQ 전용 io_context - icq_to_bic() 로
	 * 아래에서 채워진다. */
	const int is_sync = rq_is_sync(rq);
	/* [한국어] 동기 I/O 여부(REQ_SYNC 플래그 기반) - sync/async bfqq 를 구분하는
	 * 기준. */
	struct bfq_queue *bfqq;
	/* [한국어] 최종적으로 이 rq 가 배정될 bfq_queue - 아래에서 결정된다. */
	unsigned int a_idx = bfq_actuator_index(bfqd, bio);
	/* [한국어] 이 bio 가 속한 독립 접근 영역(actuator) 인덱스 - 멀티 actuator 장치의
	 * SQ/네임스페이스 라우팅을 위해 bfqq 를 actuator 별로 나눠 관리하는 키. */

	if (unlikely(!rq->elv.icq))
		return NULL;
		/* [한국어] bfq_prepare_request() 가 icq 를 찾지 못했다면(io_context 없는
		 * 요청) - 이 rq 는 BFQ 가 관리할 수 없으므로 곧바로 NULL 을 반환해
		 * 호출자가 pass-through 등으로 처리하게 한다. */

	/*
	 * Assuming that RQ_BFQQ(rq) is set only if everything is set
	 * for this rq. This holds true, because this function is
	 * invoked only for insertion or merging, and, after such
	 * events, a request cannot be manipulated any longer before
	 * being removed from bfq.
	 */
	if (RQ_BFQQ(rq))
		return RQ_BFQQ(rq);
		/* [한국어] 이미 이 rq 에 bfqq 가 배정돼 있다면(예: 병합 재시도 등으로
		 * bfq_init_rq 가 다시 호출된 경우) - 중복 배정하지 않고 기존 값을
		 * 그대로 반환한다. */

	bic = icq_to_bic(rq->elv.icq);
	/* [한국어] 범용 icq 포인터를 BFQ 전용 구조체(bfq_io_cq)로 캐스팅 - container_of
	 * 패턴으로, icq 는 elevator 공통 필드이고 bic 는 그 뒤에 이어지는 BFQ 확장. */
	bfq_check_ioprio_change(bic, bio);
	/* [한국어] 이 프로세스의 I/O 우선순위(ioprio)가 마지막 확인 이후 바뀌었는지 검사하고,
	 * 바뀌었다면 관련 bfqq 들의 weight/prio 재계산을 예약한다. */
	bfq_bic_update_cgroup(bic, bio);
	/* [한국어] 이 bio 가 속한 cgroup(blkio controller)이 바뀌었는지 확인하고, 바뀌었다면
	 * bic 가 참조하는 bfqq 를 새 cgroup 의 bfq_group 으로 재배치한다. */
	bfqq = bfq_get_bfqq_handle_split(bfqd, bic, bio, a_idx, is_sync);
	/* [한국어] 이 프로세스·actuator·sync 조합에 맞는 bfqq 를 확정 - 필요하면 협력
	 * 큐를 분리(split)하거나 새로 생성한다. 이 rq 가 실제로 배정될 큐. */

	bfqq_request_allocated(bfqq);
	/* [한국어] bfqq 및 상위 entity(cgroup 계층) 체인의 "allocated request 수"를
	 * 증가 - cgroup 별 동시 tag 사용 한도(bfq_limit_depth)를 계산하는 근거가 된다. */
	bfqq->ref++;
	/* [한국어] 이 rq 가 bfqq 를 참조하는 동안 bfqq 가 해제되지 않도록 참조 카운트
	 * 증가 - bfq_finish_requeue_request() 의 bfq_put_queue() 와 짝을 이룬다. */
	bic->requests++;
	/* [한국어] 이 프로세스(bic)가 현재 발급한 in-flight request 수를 증가 - 협력
	 * 큐 판단이나 프로세스별 동시성 추적에 쓰인다. */
	bfq_log_bfqq(bfqd, bfqq, "get_request %p: bfqq %p, %d",
		     rq, bfqq, bfqq->ref);
	/* [한국어] BFQ 트레이스 로그 - 이 rq 가 어떤 bfqq 에 배정됐고 참조 카운트가
	 * 몇인지 기록해 디버깅 시 추적할 수 있게 한다. */

	rq->elv.priv[0] = bic;
	/* [한국어] rq->elv.priv[0]에 bic 를 저장 - 이후 RQ_BIC(rq) 매크로로 꺼내 쓰이며,
	 * completion 시 bic->requests-- 등에 사용된다. */
	rq->elv.priv[1] = bfqq;
	/* [한국어] rq->elv.priv[1]에 bfqq 를 저장 - 이후 RQ_BFQQ(rq) 매크로로 꺼내 쓰이며,
	 * 이 rq 가 어느 bfqq 에 속하는지의 유일한 근거가 된다. */

	/*
	 * If a bfq_queue has only one process reference, it is owned
	 * by only this bic: we can then set bfqq->bic = bic. in
	 * addition, if the queue has also just been split, we have to
	 * resume its state.
	 */
	if (likely(bfqq != &bfqd->oom_bfqq) && !bfqq->new_bfqq &&
	    bfqq_process_refs(bfqq) == 1)
		/* [한국어] 이 bfqq 가 (a) 메모리 부족 폴백 큐가 아니고, (b) 다른 큐로
		 * 병합되도록 지정되지도 않았고, (c) 참조하는 프로세스가 이 하나뿐이라면 -
		 * bfqq 를 이 bic 가 독점 소유한다고 볼 수 있다. */
		bfqq->bic = bic;
		/* [한국어] bfqq->bic 를 직접 연결해 둔다 - 이후 여러 곳(예: 병합 판단,
		 * 통계)에서 "이 bfqq 의 유일한 소유 프로세스"를 빠르게 역참조할 수
		 * 있게 하는 fast-path 캐시. */

	/*
	 * Consider bfqq as possibly belonging to a burst of newly
	 * created queues only if:
	 * 1) A burst is actually happening (bfqd->burst_size > 0)
	 * or
	 * 2) There is no other active queue. In fact, if, in
	 *    contrast, there are active queues not belonging to the
	 *    possible burst bfqq may belong to, then there is no gain
	 *    in considering bfqq as belonging to a burst, and
	 *    therefore in not weight-raising bfqq. See comments on
	 *    bfq_handle_burst().
	 *
	 * This filtering also helps eliminating false positives,
	 * occurring when bfqq does not belong to an actual large
	 * burst, but some background task (e.g., a service) happens
	 * to trigger the creation of new queues very close to when
	 * bfqq and its possible companion queues are created. See
	 * comments on bfq_handle_burst() for further details also on
	 * this issue.
	 */
	if (unlikely(bfq_bfqq_just_created(bfqq) &&
		     (bfqd->burst_size > 0 ||
		      bfq_tot_busy_queues(bfqd) == 0)))
		/* [한국어] bfqq 가 방금 막 생성됐고(just_created), 이미 burst 감지가
		 * 진행 중이거나(burst_size>0) 다른 활성 큐가 전혀 없다면(bfq_tot_busy_queues
		 * ==0) - 이 bfqq 를 "다수 큐가 한꺼번에 생성되는 burst"의 일부로 볼
		 * 후보로 삼는다(그렇지 않으면 weight-raise 배제 오판을 막기 위해 무시). */
		bfq_handle_burst(bfqd, bfqq);
		/* [한국어] burst 판정 로직에 이 bfqq 를 넘겨, burst 목록에 추가하거나
		 * large-burst 임계치를 넘었는지 등을 갱신시킨다. */

	return bfqq;
	/* [한국어] 이 rq 가 최종적으로 배정된 bfq_queue 를 호출자에게 반환. */
}

/*
 * [한국어]
 * bfq_idle_slice_timer_body - in-service bfqq 에 대해 무장(arm)해 두었던
 * idle slice timer 가 만료됐을 때 실제 처리를 수행한다: bfqq 가 여전히
 * in-service 상태인지 재확인한 뒤, budget timeout 또는 "너무 오래
 * idle"했다는 이유로 bfqq 를 만료(expire)시키고 dispatch 를 재개시킨다.
 *
 * @bfqd: 이 device 의 BFQ 전역 상태 - bfqd->lock, in_service_queue 를
 *        참조/갱신한다.
 * @bfqq: 타이머가 걸릴 당시 in-service 였던 bfq_queue. 함수 진입 시점에는
 *        이미 다른 bfqq 로 바뀌었을 수 있어 재검증이 필요하다.
 * @return: 없음(void).
 *
 * BFQ 는 sync bfqq 가 다음 request 를 곧 낼 것으로 예상될 때, 다른 큐로
 * 넘어가지 않고 짧게 대기(idling)하며 idle_slice_timer 를 무장해 둔다.
 * 이 함수는 그 타이머가 실제로 만료됐을 때 호출되어, (1) 타이머가 걸린
 * bfqq 가 지금도 in-service 인지 확인하고(레이스로 이미 바뀌었을 수
 * 있음), 아니라면 아무 것도 하지 않고 반환한다. (2) wait_request 플래그를
 * 지워 "더 이상 idling 중이 아님"을 표시한다. (3) budget timeout 이 났으면
 * BFQQE_BUDGET_TIMEOUT 사유로, 아니면(여전히 request 가 없다면)
 * BFQQE_TOO_IDLE 사유로 만료를 결정한다. request 가 이미 도착해 있다면
 * (레이스로 타이머 비활성화가 늦어진 경우) 만료하지 않고 그냥 dispatch 를
 * 재개시킨다. (4) 실제로 만료가 결정됐다면 bfq_bfqq_expire() 로 다음 큐를
 * 선택할 수 있게 하고, 마지막에 항상 bfq_schedule_dispatch() 로 dispatch
 * 워크를 재개시킨다.
 * 실행 컨텍스트: hrtimer softirq 컨텍스트(bfq_idle_slice_timer)에서
 * 호출되며, bfqd->lock 을 직접 획득/해제한다.
 * caller: bfq_idle_slice_timer() (hrtimer 콜백 하나뿐).
 * callee: bfq_clear_bfqq_wait_request(), bfq_bfqq_budget_timeout(),
 *         bfq_bfqq_expire(), bfq_schedule_dispatch().
 * 에러 경로: bfqq 가 더 이상 in-service 가 아니면 조용히 반환 — 이는
 *           오류가 아니라 정상적인 레이스 처리다.
 *
 * 호출 체인:
 *   bfq_idle_slice_timer → [bfq_idle_slice_timer_body] → bfq_bfqq_expire
 */
static void
bfq_idle_slice_timer_body(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	enum bfqq_expiration reason;
	/* [한국어] bfqq 를 만료시킬 사유 코드 - 아래 분기에서 결정되고 bfq_bfqq_expire()
	 * 에 전달되어 통계/정책 판단에 쓰인다. */
	unsigned long flags;
	/* [한국어] spin_lock_irqsave 로 보존할 인터럽트 상태 - hrtimer 콜백은 인터럽트
	 * 컨텍스트에서 실행될 수 있으므로 로컬 인터럽트를 반드시 함께 막는다. */

	spin_lock_irqsave(&bfqd->lock, flags);
	/* [한국어] bfqd->lock 획득 - in_service_queue 조회 및 bfqq 만료 처리 동안
	 * dispatch/completion 경로와의 자료구조 경쟁을 막는다. */

	/*
	 * Considering that bfqq may be in race, we should firstly check
	 * whether bfqq is in service before doing something on it. If
	 * the bfqq in race is not in service, it has already been expired
	 * through __bfq_bfqq_expire func and its wait_request flags has
	 * been cleared in __bfq_bfqd_reset_in_service func.
	 */
	if (bfqq != bfqd->in_service_queue) {
	/* [한국어] 타이머가 걸린 시점과 이 콜백이 실행되는 시점 사이에 in-service 큐가
	 * 바뀌었을 수 있다(레이스) - 이미 다른 경로로 만료 처리가 끝난 상태이므로
	 * 여기서 중복 처리하면 안 된다. */
		spin_unlock_irqrestore(&bfqd->lock, flags);
		/* [한국어] 락 해제 및 인터럽트 상태 복원 - 아무 것도 바꾸지 않고 빠져나간다. */
		return;
	}

	/* [한국어] "request 도착을 기다리며 idling 중" 플래그를 해제 - 타이머가 만료돼
	 * idling 구간이 끝났음을 반영한다. */
	bfq_clear_bfqq_wait_request(bfqq);

	if (bfq_bfqq_budget_timeout(bfqq)) // idling으로 기다리는 사이에 budget 시간 제한까지 지나버린 경우 - idling의 목적(다음 요청 확보)보다 시간 제한이 우선한다
		/*
		 * Also here the queue can be safely expired
		 * for budget timeout without wasting
		 * guarantees
		 */
		/* [한국어] 이 bfqq 에 할당된 budget(서비스 시간/섹터 한도) 이 이미 소진돼
		 * 타임아웃됐다면 - budget timeout 사유로 만료해도 서비스 보장을 해치지
		 * 않는다고 판단한다. */
		reason = BFQQE_BUDGET_TIMEOUT;
	else if (bfqq->queued[0] == 0 && bfqq->queued[1] == 0) // 기다린 보람 없이 sync/async 어느 쪽에도 요청이 도착하지 않은 경우 - idling이 순수 손실이었다는 뜻
		/*
		 * The queue may not be empty upon timer expiration,
		 * because we may not disable the timer when the
		 * first request of the in-service queue arrives
		 * during disk idling.
		 */
		/* [한국어] queued[0](동기)/queued[1](비동기) 모두 0, 즉 아직도 대기 중인
		 * request 가 전혀 없다면 - 예상했던 request 가 결국 오지 않았으므로
		 * "너무 오래 idle 했다"는 사유로 만료한다. */
		reason = BFQQE_TOO_IDLE;
	/* [한국어] 이미 request 가 도착해 있다면(idling 중 타이머 해제가 레이스로
	 * 늦어진 경우) - 만료하지 않고 곧바로 dispatch 재개 단계로 건너뛴다. */
	else
		goto schedule_dispatch;

	/* [한국어] 결정된 사유로 bfqq 를 만료시켜 B-WF2Q+ 스케줄러가 다음 in-service
	 * 큐를 새로 선택하게 한다 - true 인자는 "타이머 콜백에 의한 강제 만료"임을
	 * 표시(자발적 만료와 구분해 통계 처리가 달라질 수 있음). */
	bfq_bfqq_expire(bfqd, bfqq, true, reason);

schedule_dispatch:
	/* [한국어] blk-mq 에 dispatch 워크를 예약 - 만료로 새 in-service 큐가 정해졌든,
	 * 만료 없이 그냥 넘어왔든 대기 중인 request 처리를 재개시킨다. */
	bfq_schedule_dispatch(bfqd);
	/* [한국어] 락 해제 및 인터럽트 상태 복원. */
	spin_unlock_irqrestore(&bfqd->lock, flags);
}

/*
 * Handler of the expiration of the timer running if the in-service queue
 * is idling inside its time slice.
 */
/*
 * [한국어]
 * bfq_idle_slice_timer - hrtimer(bfqd->idle_slice_timer)의 만료 콜백.
 * in-service bfqq 가 다음 request 도착을 기다리며 idling 하던 시간 슬라이스가
 * 다 됐을 때 호출되어, 실제 만료 판단을 bfq_idle_slice_timer_body() 에
 * 위임한다.
 *
 * @timer: 만료된 hrtimer. container_of() 로 bfqd->idle_slice_timer 를
 *         감싸는 struct bfq_data 를 역참조하는 데 쓰인다.
 * @return: HRTIMER_NORESTART — 이 타이머는 일회성(one-shot)이며, 다음
 *          idling 이 필요할 때 bfq_arm_slice_timer() 가 다시 무장한다.
 *
 * hrtimer 인프라는 콜백에 timer 포인터만 넘겨주므로, 이 함수는
 * container_of 로 원래의 bfq_data 를 복원한 뒤, 그 시점의 in-service
 * bfqq 를 읽어 실제 처리 함수에 넘긴다. 콜백이 실행되는 시점과 타이머가
 * 걸렸던 시점 사이에 이론적인 레이스가 있을 수 있다(원본 주석 참고) —
 * in_service_queue 가 NULL 이거나 이미 다른 bfqq 로 바뀌었을 수 있는데,
 * 최악의 경우라도 "어떤 큐를 조금 일찍 만료시키는" 정도의 영향뿐이므로
 * 별도의 방어 락 없이 bfq_idle_slice_timer_body() 내부에서 재검증한다.
 * 실행 컨텍스트: hrtimer softirq(HRTIMER_MODE_REL, CLOCK_MONOTONIC) 콜백
 * 컨텍스트 — 인터럽트 컨텍스트에 준하므로 잠들 수 없다.
 * caller: 커널 hrtimer 서브시스템이 만료 시각에 자동으로 호출.
 * callee: bfq_idle_slice_timer_body().
 * 에러 경로: in_service_queue 가 NULL 이면(이미 만료 처리됨) 아무 것도
 *           하지 않고 그냥 NORESTART 를 반환한다.
 *
 * 호출 체인:
 *   hrtimer softirq → [bfq_idle_slice_timer] → bfq_idle_slice_timer_body
 */
static enum hrtimer_restart bfq_idle_slice_timer(struct hrtimer *timer)
{
	struct bfq_data *bfqd = container_of(timer, struct bfq_data,
					     /* [한국어] hrtimer 포인터로부터 그것을 멤버로 포함하는 struct bfq_data 전체를
					      * 역산(container_of) - hrtimer API 는 콜백에 timer 자체만 넘기므로, BFQ
					      * 전역 상태에 접근하려면 이 매크로로 바깥 구조체를 복원해야 한다. */
					     idle_slice_timer);
	/* [한국어] 현재(콜백 실행 시점) in-service 인 bfqq 를 읽는다 - 타이머가 걸렸을
	 * 때와 동일한 큐라는 보장은 없으며, 아래 주석처럼 레이스가 있을 수 있다. */
	struct bfq_queue *bfqq = bfqd->in_service_queue;

	/*
	 * Theoretical race here: the in-service queue can be NULL or
	 * different from the queue that was idling if a new request
	 * arrives for the current queue and there is a full dispatch
	 * cycle that changes the in-service queue.  This can hardly
	 * happen, but in the worst case we just expire a queue too
	 * early.
	 */
	/* [한국어] in-service 큐가 존재한다면(가장 흔한 경우) 실제 만료 판단과
	 * dispatch 재개 처리를 위임한다 - 내부에서 다시 한 번 이 bfqq 가 여전히
	 * in-service 인지 재검증한다. */
	if (bfqq)
		bfq_idle_slice_timer_body(bfqd, bfqq);

	/* [한국어] 이 hrtimer 는 반복(periodic) 타이머가 아니라 매번 새로 무장되는
	 * 일회성 타이머이므로, 커널이 자동 재시작하지 않도록 NORESTART 를 반환한다 -
	 * 다음 idling 이 필요해지면 bfq_arm_slice_timer() 가 다시 hrtimer_start() 한다. */
	return HRTIMER_NORESTART;
}

/*
 * [한국어]
 * __bfq_put_async_bfqq - bfqq_ptr 가 가리키는 비동기(async) bfq_queue 를
 * root cgroup 으로 재소속(reparent)시킨 뒤 참조를 반납하고 포인터를
 * NULL 로 지운다.
 *
 * @bfqd: 이 device 의 BFQ 전역 상태 - bfq_bfqq_move() 의 목적지로 쓰이는
 *        bfqd->root_group 을 제공한다.
 * @bfqq_ptr: bfq_group 안의 async_bfqq[][][]/async_idle_bfqq[] 배열 슬롯 중
 *            하나를 가리키는 이중 포인터. 이 함수가 끝나면 *bfqq_ptr 은
 *            NULL 이 된다.
 * @return: 없음(void).
 *
 * 비동기 bfqq(버퍼드 쓰기 등)는 특정 cgroup(bfq_group)에 소속돼 생성되지만,
 * 그 cgroup 이 삭제될 때도 이미 큐에 쌓인 request 들은 어딘가에 계속
 * 소속돼 있어야 한다. 이 함수는 그런 bfqq 를 device 전체에서 유일하게
 * 영속적으로 존재가 보장되는 root_group 으로 옮긴 뒤(bfq_bfqq_move), 이
 * bfq_group 이 쥐고 있던 참조를 반납한다(bfq_put_queue). *bfqq_ptr 이
 * 이미 NULL(해당 슬롯에 큐가 없음)이면 아무 일도 하지 않는다.
 * 실행 컨텍스트: bfq_put_async_queues() 로부터 bfqd->lock 을 쥔 채
 * 호출된다.
 * caller: bfq_put_async_queues() (모든 ioprio/actuator 슬롯에 대해 반복
 *         호출).
 * callee: bfq_bfqq_move(), bfq_put_queue().
 * 에러 경로: 없음 — *bfqq_ptr 이 NULL 이면 그냥 건너뛴다.
 *
 * 호출 체인:
 *   bfq_put_async_queues → [__bfq_put_async_bfqq] → bfq_bfqq_move
 */
static void __bfq_put_async_bfqq(struct bfq_data *bfqd,
				 struct bfq_queue **bfqq_ptr)
{
	struct bfq_queue *bfqq = *bfqq_ptr;
	/* [한국어] 이중 포인터가 가리키는 실제 bfq_queue 포인터를 로컬 변수로 꺼낸다 -
	 * 이후 검사/조작은 이 로컬 bfqq 로 수행하고, 마지막에 원본 슬롯을 NULL 로
	 * 되돌린다. */

	bfq_log(bfqd, "put_async_bfqq: %p", bfqq);
	/* [한국어] BFQ 트레이스 로그 - 어떤 bfqq 가 반납 대상인지(NULL 포함) 기록. */
	if (bfqq) {
	/* [한국어] 이 슬롯에 실제로 배정된 bfqq 가 있을 때만 처리 - 애초에 생성된 적
	 * 없는 ioprio 레벨이라면 bfqq 가 NULL 일 수 있다. */
		bfq_bfqq_move(bfqd, bfqq, bfqd->root_group);
		/* [한국어] 이 bfqq 를 원래 소속된 cgroup(bfq_group)에서 root_group 으로
		 * 옮긴다 - 원래 cgroup 이 곧 해제되더라도, 이 bfqq 에 남아있는 request
		 * 들이 계속 유효한 그룹에 속해 서비스받을 수 있게 하기 위함. */

		bfq_log_bfqq(bfqd, bfqq, "put_async_bfqq: putting %p, %d",
			     bfqq, bfqq->ref);
		/* [한국어] 반납 직전의 참조 카운트를 로그로 남겨, 이 시점에 몇 개의
		 * 참조가 남아있었는지(디버깅용) 기록한다. */
		bfq_put_queue(bfqq);
		/* [한국어] bfq_group 이 쥐고 있던 이 bfqq 에 대한 참조를 반납(ref--) -
		 * 다른 참조자(진행 중인 request 등)가 없다면 여기서 실제로 해제된다. */
		*bfqq_ptr = NULL;
		/* [한국어] 원본 슬롯을 NULL 로 지워, 이 bfq_group 이 더 이상 이 bfqq 를
		 * 참조하지 않음을 명시하고 이중 해제를 방지한다. */
	}
}

/*
 * Release all the bfqg references to its async queues.  If we are
 * deallocating the group these queues may still contain requests, so
 * we reparent them to the root cgroup (i.e., the only one that will
 * exist for sure until all the requests on a device are gone).
 */
/*
 * [한국어]
 * bfq_put_async_queues - bfqg(bfq_group, cgroup 대응 엔티티)가 갖고 있는
 * 모든 비동기(async) bfq_queue 참조를 해제한다. 그룹이 해제되는 중이라도
 * 이 큐들에 아직 request 가 남아있을 수 있으므로, 실제 해제 대신 먼저
 * root cgroup 으로 재소속시킨다.
 *
 * @bfqd: 이 device 의 BFQ 전역 상태.
 * @bfqg: 해제(또는 async queue 정리)가 필요한 bfq_group. bfqg->async_bfqq
 *        [i][ioprio][actuator], bfqg->async_idle_bfqq[actuator] 슬롯들이
 *        대상이다.
 * @return: 없음(void).
 *
 * bfq_group 은 (배열 첫 차원 × IOPRIO_NR_LEVELS 개의 ioprio 레벨 ×
 * num_actuators 개의 actuator) 조합마다 별도의 비동기 bfqq 슬롯을 갖고,
 * 별도로 idle-class 비동기 슬롯(async_idle_bfqq)도 actuator 별로 갖는다.
 * 이 함수는 모든 actuator 에 대해 이 슬롯들을 전부 순회하며
 * __bfq_put_async_bfqq() 로 하나씩 정리한다.
 * 실행 컨텍스트: bfq_exit_queue()(elevator 해제 시, CONFIG_BFQ_GROUP_IOSCHED
 * 미설정 빌드의 root_group 정리 경로) 또는 cgroup 관련 코드(blkcg 정책
 * 해제 경로)에서 bfqd->lock 을 쥔 채 호출된다.
 * caller: bfq_exit_queue(), bfq_pd_offline()(cgroup 코드, 이 파일 밖).
 * callee: __bfq_put_async_bfqq().
 * 에러 경로: 없음 — 슬롯이 비어 있으면 __bfq_put_async_bfqq 내부에서
 *           조용히 건너뛴다.
 *
 * 호출 체인:
 *   bfq_exit_queue → [bfq_put_async_queues] → __bfq_put_async_bfqq
 */
void bfq_put_async_queues(struct bfq_data *bfqd, struct bfq_group *bfqg)
{
	int i, j, k;
	/* [한국어] i: async_bfqq 배열의 첫 차원, j: ioprio 레벨(IOPRIO_NR_LEVELS 개),
	 * k: actuator 인덱스 - 세 중첩 루프로 모든 조합을 순회한다. */

	for (k = 0; k < bfqd->num_actuators; k++) {
	/* [한국어] 이 device 가 갖는 모든 독립 접근 영역(actuator) 각각에 대해 -
	 * 각 actuator 는 자신만의 async bfqq 슬롯 집합을 갖기 때문이다. */
		for (i = 0; i < 2; i++)
			/* [한국어] async_bfqq 배열의 첫 차원을 순회 - 이 배열 전체가 이
			 * bfq_group 의 비동기 슬롯을 담는다. */
			for (j = 0; j < IOPRIO_NR_LEVELS; j++)
				/* [한국어] 모든 ioprio 레벨(RT/BE/IDLE 세분화)에 대해 순회. */
				__bfq_put_async_bfqq(bfqd, &bfqg->async_bfqq[i][j][k]);
				/* [한국어] (i, j, k) 조합에 해당하는 슬롯을 root_group 으로
				 * 재소속시키고 참조 반납. */

		__bfq_put_async_bfqq(bfqd, &bfqg->async_idle_bfqq[k]);
		/* [한국어] 이 actuator 의 idle 클래스 전용 비동기 슬롯도 별도로 정리 -
		 * idle 클래스는 ioprio 레벨 배열과 별개로 관리되므로 따로 처리한다. */
	}
}

/*
 * See the comments on bfq_limit_depth for the purpose of
 * the depths set in the function. Return minimum shallow depth we'll use.
 */
/*
 * [한국어]
 * bfq_depth_updated - elevator_ops.depth_updated 콜백. request_queue 의
 * async_depth(사용자가 nr_requests 등을 통해 조정 가능한 비동기 tag 심도
 * 기준값)가 바뀔 때 호출되어, [weight-raised 여부][sync 여부]로 세분화된
 * bfqd->async_depths[2][2] 테이블을 재계산하고, blk-mq 의 최소 shallow depth
 * 를 설정한다.
 *
 * @q: async_depth 가 갱신된 request_queue. q->elevator->elevator_data 로
 *     bfqd 를 얻고, q->async_depth 를 기준값으로 읽는다. bfq_init_queue()
 *     가 이 값을 nr_requests 의 3/4(75%)로 초기값 설정해 두었음을 기억해
 *     두면 아래 각 슬롯이 "nr_requests 대비 몇 %"인지 계산할 수 있다.
 * @return: 없음(void).
 *
 * bfq_limit_depth()(이 파일의 별도 위치)는 dispatch 시점에 이
 * bfqd->async_depths[][] 테이블을 참조해, 각 프로세스/bfqq 가 동시에
 * 사용할 수 있는 blk-mq tag 심도(shallow depth)의 상한을 정한다. 기본
 * 정책은 원본 주석에 요약돼 있듯: sync 읽기는 제한 없음, weight-raise
 * 되지 않은 sync 쓰기는 nr_requests 의 75%, 비동기 I/O 는 50%, weight-raise
 * 된 경우는 각각 약 37%/18% 로 더 좁게 제한한다 — weight-raise 된(응답성
 * 우대) 큐가 존재할 때는 다른 큐들이 tag 를 덜 뺏어가게 하려는 의도다.
 * 사용자가 request_queue 의 async_depth(스토리지의 hw 큐 개수 등에 맞춰
 * /sys 로 조정 가능)를 바꾸면 이 비율들도 함께 비례 조정된다.
 * 실행 컨텍스트: blk_mq_update_nr_requests() 등 sysfs 를 통한 큐 파라미터
 * 변경 경로(프로세스 컨텍스트)에서 호출되며, queue freeze 상태에서
 * 호출되므로 별도의 bfqd->lock 없이 필드를 갱신해도 안전하다.
 * caller: blk_mq_sched_depth_updated() → elevator_ops.depth_updated.
 *         또한 bfq_init_queue() 가 초기화 마지막 단계에서 직접 호출한다.
 * callee: blk_mq_set_min_shallow_depth().
 * 에러 경로: 없음 — 순수 계산/설정 함수.
 *
 * 호출 체인:
 *   blk_mq_update_nr_requests → elevator_ops.depth_updated → [bfq_depth_updated]
 */
static void bfq_depth_updated(struct request_queue *q)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	/* [한국어] 이 request_queue 에 연결된 BFQ 전역 상태를 얻는다 - async_depths[]
	 * 테이블이 이 안에 있다. */
	unsigned int async_depth = q->async_depth;
	/* [한국어] blk-mq/사용자가 정한 "비동기 I/O 에 허용할 기준 tag 심도" - bfq_init_queue()
	 * 에서 nr_requests 의 3/4(75%)로 초기화되며, 이후 사용자가 sysfs 로 조정할 수도
	 * 있다. 아래 4 개 대입식은 모두 이 값을 기준으로 비율을 나눈 것이다. */

	/*
	 * By default:
	 *  - sync reads are not limited
	 * If bfqq is not being weight-raised:
	 *  - sync writes are limited to 75%(async depth default value)
	 *  - async IO are limited to 50%
	 * If bfqq is being weight-raised:
	 *  - sync writes are limited to ~37%
	 *  - async IO are limited to ~18
	 *
	 * If request_queue->async_depth is updated by user, all limit are
	 * updated relatively.
	 */
	bfqd->async_depths[0][1] = async_depth;
	/* [한국어] [weight-raised 아님(0)][sync(1)] 슬롯 = async_depth 그대로. async_depth
	 * 자체가 이미 nr_requests 의 75%이므로, 이 슬롯은 결과적으로 "전체 tag 수의
	 * 75%까지 동기 쓰기에 허용"을 의미한다(원본 주석의 "sync writes limited to 75%"). */
	bfqd->async_depths[0][0] = max(async_depth * 2 / 3, 1U);
	/* [한국어] [weight-raised 아님(0)][async(0)] 슬롯 = async_depth 의 2/3, 즉
	 * nr_requests 의 75% * 2/3 = 50% - 원본 주석의 "async IO limited to 50%"에
	 * 해당. 최소 1 을 보장해 tag 가 아예 0개로 죽지 않게 한다. */
	bfqd->async_depths[1][1] = max(async_depth >> 1, 1U);
	/* [한국어] [weight-raised(1)][sync(1)] 슬롯 = async_depth 의 1/2, 즉 nr_requests
	 * 의 75% * 1/2 = 37.5% - 원본 주석의 "sync writes limited to ~37%"에 해당.
	 * 인터랙티브(weight-raised) 큐가 있을 때는 동기 쓰기 tag 사용도 더 보수적으로
	 * 제한한다. */
	bfqd->async_depths[1][0] = max(async_depth >> 2, 1U);
	/* [한국어] [weight-raised(1)][async(0)] 슬롯 = async_depth 의 1/4, 즉 nr_requests
	 * 의 75% * 1/4 = 18.75% - 원본 주석의 "async IO limited to ~18%"에 해당.
	 * weight-raised 인터랙티브 큐가 존재하는 동안 비동기 I/O 에 가장 강한 제한을
	 * 건다. */

	/*
	 * Due to cgroup qos, the allowed request for bfqq might be 1
	 */
	blk_mq_set_min_shallow_depth(q, 1);
	/* [한국어] cgroup QoS(예: io.max 등)로 인해 특정 bfqq 에 허용되는 동시 요청
	 * 수가 1까지 낮아질 수 있으므로, blk-mq 의 tag 할당기에 "shallow depth 최소값
	 * 1"을 알려 그 극단적인 경우에도 tag 할당 로직이 오작동하지 않게 한다. */
}

/*
 * [한국어]
 * bfq_exit_queue - elevator_ops.exit_sched 콜백. request_queue 에서 BFQ
 * 스케줄러를 떼어낼 때(다른 스케줄러로 교체되거나 device 제거 시) 호출되어,
 * idle 타이머/idle 큐/cgroup 정책/통계 훅 등 bfq_init_queue() 가 만들어
 * 둔 모든 자원을 역순으로 정리하고 마지막에 bfq_data 자체를 해제한다.
 *
 * @e: 해제할 elevator_queue. e->elevator_data 에 이 device 의 bfq_data
 *     포인터가 들어 있다.
 * @return: 없음(void).
 *
 * 정리 순서는 대략 초기화의 역순을 따른다: 먼저 idle slice timer 를
 * 취소해 더 이상 만료 콜백이 실행되지 않게 하고, idle_list 에 남아있는
 * bfqq 들을 모두 비활성화(deactivate)한다. 이어서 driver 에 아직 in-flight
 * request 가 남아있지 않은지 WARN_ON_ONCE 로 검증한다(정상적으로 해제되는
 * 경우라면 0이어야 하며, 0이 아니면 버그를 의미). 그다음 oom_bfqq 가
 * 쥐고 있던 root_group 참조를 반납하고, cgroup 지원 여부(CONFIG_
 * BFQ_GROUP_IOSCHED)에 따라 blkcg 정책을 비활성화하거나(cgroup 지원 시)
 * 직접 async queue 를 반납하고 root_group 을 kfree 한다(미지원 시). 마지막
 * 으로 이 device 에 걸어 두었던 통계/WBT(Writeback Throttling) 관련 훅을
 * 원상복구하고 bfqd 자체를 해제한다.
 * 실행 컨텍스트: elevator_exit() 호출 경로(프로세스 컨텍스트, 보통 device
 * 제거나 스케줄러 전환 syscall/sysfs 경로)에서 호출되며, 이 시점에는 이미
 * 새 I/O 가 이 스케줄러로 들어오지 않음이 보장된다.
 * caller: elevator_exit() → elevator_ops.exit_sched.
 * callee: hrtimer_cancel(), bfq_deactivate_bfqq(), bfqg_and_blkg_put(),
 *         blkcg_deactivate_policy(), bfq_put_async_queues(),
 *         blk_stat_disable_accounting(), wbt_enable_default().
 * 에러 경로: 없음 — WARN_ON_ONCE 는 버그 검출용이며 해제 자체를 막지 않는다.
 *
 * 호출 체인:
 *   elevator_exit → [bfq_exit_queue] → bfq_put_async_queues / blkcg_deactivate_policy
 */
static void bfq_exit_queue(struct elevator_queue *e)
{
	struct bfq_data *bfqd = e->elevator_data;
	/* [한국어] elevator_data 에서 이 device 의 BFQ 전역 상태를 꺼낸다 - bfq_init_queue()
	 * 가 eq->elevator_data = bfqd 로 등록해 둔 바로 그 포인터. */
	struct bfq_queue *bfqq, *n;
	/* [한국어] idle_list 순회용 반복자(bfqq)와 안전한 순회를 위한 다음 노드 포인터(n) -
	 * list_for_each_entry_safe 는 순회 중 현재 노드를 리스트에서 제거해도 안전하게
	 * 다음으로 넘어갈 수 있게 해준다. */
	unsigned int actuator;
	/* [한국어] 아래에서 모든 actuator 의 in-flight 카운트를 검사하기 위한 루프 변수. */

	hrtimer_cancel(&bfqd->idle_slice_timer);
	/* [한국어] idle slice hrtimer 를 취소 - 아직 만료 대기 중이었다면 콜백이 더 이상
	 * 실행되지 않도록 동기적으로 취소(콜백이 실행 중이면 끝날 때까지 대기)한다.
	 * 스케줄러 해제 이후 콜백이 이미 해제된 bfqd 를 참조하는 use-after-free 를
	 * 막기 위한 필수 단계. */

	spin_lock_irq(&bfqd->lock);
	/* [한국어] idle_list 순회 및 비활성화 동안 다른 CPU 의 dispatch/completion 경로와의
	 * 경쟁을 막기 위해 락 획득. */
	list_for_each_entry_safe(bfqq, n, &bfqd->idle_list, bfqq_list)
		bfq_deactivate_bfqq(bfqd, bfqq, false, false);
		/* [한국어] idle_list(활성 서비스 트리에서 빠져 있지만 아직 완전히 소멸하지
		 * 않은 bfqq 들)에 남은 모든 bfqq 를 명시적으로 비활성화 - 두 false 인자는
		 * "만료로 인한 것이 아님", "요청을 requeue 하지 않음" 등 일반적인 종료
		 * 경로의 의미를 나타낸다. */
	spin_unlock_irq(&bfqd->lock);
	/* [한국어] 락 해제 - 아래의 WARN_ON_ONCE 검사는 락 없이도 수행 가능한 단순 값
	 * 비교이다. */

	for (actuator = 0; actuator < bfqd->num_actuators; actuator++)
		WARN_ON_ONCE(bfqd->rq_in_driver[actuator]);
		/* [한국어] 이 시점에 각 actuator 별 in-flight(dispatch 됐지만 아직 완료 안 된)
		 * request 카운트가 0이어야 정상 - 0이 아니면 아직 미완료 request 를 남긴 채
		 * 스케줄러가 해제되는 버그 상황이므로 커널 경고를 남긴다(해제 자체는 계속
		 * 진행). */
	WARN_ON_ONCE(bfqd->tot_rq_in_driver);
	/* [한국어] actuator 별 합계인 전체 in-flight 카운트도 0이어야 정상 - 위 개별 검사와
	 * 이중으로 확인해 집계 로직 자체의 버그도 함께 잡아낸다. */

	hrtimer_cancel(&bfqd->idle_slice_timer);
	/* [한국어] idle slice timer 를 다시 한 번 취소 - 위의 idle_list 비활성화 과정에서
	 * 혹시라도 새로 in-service 큐가 선정되며 타이머가 재무장됐을 가능성에 대비한
	 * 방어적 재취소. */

	/* release oom-queue reference to root group */
	bfqg_and_blkg_put(bfqd->root_group);
	/* [한국어] bfq_init_queue() 에서 oom_bfqq 를 root_group 에 연결할 때 잡아 두었던
	 * blkg(block cgroup) 참조를 반납 - cgroup 참조 카운트 누수를 막는다. */

#ifdef CONFIG_BFQ_GROUP_IOSCHED
	blkcg_deactivate_policy(bfqd->queue->disk, &blkcg_policy_bfq);
	/* [한국어] cgroup 지원 빌드에서는 blkcg 코어에 BFQ 정책 비활성화를 요청 - 이 호출이
	 * 내부적으로 모든 bfq_group(및 그 안의 async bfqq)을 순회하며 정리하므로,
	 * 여기서 별도로 root_group 을 kfree 할 필요가 없다(blkcg 가 소유권을 갖는다). */
#else
	spin_lock_irq(&bfqd->lock);
	/* [한국어] cgroup 미지원 빌드에서는 bfq_group 이 이 root_group 하나뿐이므로 직접
	 * 정리해야 한다 - async queue 반납 동안 락으로 보호. */
	bfq_put_async_queues(bfqd, bfqd->root_group);
	/* [한국어] root_group 에 남아있는 모든 비동기 bfqq 참조를 반납. */
	kfree(bfqd->root_group);
	/* [한국어] cgroup 미지원 빌드에서 유일했던 bfq_group 구조체 자체를 해제 - cgroup
	 * 지원 빌드에서는 blkcg 코어가 pd(policy data)로 관리하므로 별도 kfree 가
	 * 필요 없지만, 미지원 빌드에서는 bfq_init_queue() 가 kzalloc 으로 직접 할당한
	 * 것이므로 여기서 직접 해제해야 한다. */
	spin_unlock_irq(&bfqd->lock);
#endif

	/* [한국어] bfq_init_queue() 에서 활성화했던 blk_stat(요청 완료 시간 통계 수집)을
	 * 비활성화 - peak_rate 추정 등에 쓰이던 통계 수집 훅을 해제한다. */
	blk_stat_disable_accounting(bfqd->queue);
	/* [한국어] BFQ 가 초기화 시 설정했던 "기본 WBT(Writeback Throttling) 비활성화"
	 * 플래그를 다시 지운다 - BFQ 자체가 쓰기 흐름을 조절하므로 WBT 를 꺼 두었던
	 * 것을 원상복구하는 절차의 일부. */
	blk_queue_flag_clear(QUEUE_FLAG_DISABLE_WBT_DEF, bfqd->queue);
	/* [한국어] 다른 스케줄러(WBT 를 필요로 할 수 있는)로 전환될 것에 대비해 WBT 를
	 * 기본 정책대로 다시 활성화 - BFQ 는 자체 저지연 로직이 있어 WBT 와 중복
	 * 제어를 피하려고 꺼 두었지만, 스케줄러가 바뀌면 이 배려가 필요 없다. */
	wbt_enable_default(bfqd->queue->disk);

	/* [한국어] 이 device 의 BFQ 전역 상태 전체를 해제 - bfq_init_queue() 에서
	 * kzalloc_node() 로 할당했던 바로 그 메모리. 이 시점 이후로는 bfqd 를 참조하는
	 * 어떤 콜백도 실행되지 않음이 보장돼야 한다(위에서 타이머/훅을 모두 정리했으므로). */
	kfree(bfqd);
}

/*
 * [한국어]
 * bfq_init_root_group - device 전체에 하나뿐인 root bfq_group 을 초기
 * 상태로 설정한다. root_group 은 cgroup 계층의 최상위(모든 프로세스의
 * 기본 소속)이자, cgroup 을 지원하지 않는 빌드에서는 유일하게 존재하는
 * bfq_group 이다.
 *
 * @root_group: 초기화할 root bfq_group. 호출자(bfq_init_queue)가
 *              bfq_create_group_hierarchy() 로 이미 메모리를 할당해 둔
 *              상태에서 넘어온다.
 * @bfqd: 이 root_group 이 속할 BFQ 전역 상태 - CONFIG_BFQ_GROUP_IOSCHED
 *        빌드에서 root_group->bfqd 역참조에 쓰인다.
 * @return: 없음(void).
 *
 * bfq_group 은 그 안에 struct bfq_sched_data(서비스 트리들의 집합)를
 * 내장하고 있는데, 이는 B-WF2Q+ 알고리즘이 이 그룹 산하의 bfqq/하위
 * 그룹들 사이에서 서비스 순서를 매기는 자료구조다. 이 함수는 (cgroup
 * 지원 빌드에서) entity 계층 포인터를 최상위로 설정(parent=NULL)하고,
 * rq_pos_tree(위치 기반 병합 후보 탐색용 rb-tree)를 비우고, ioprio 클래스
 * 개수(BFQ_IOPRIO_CLASSES)만큼의 서비스 트리를 초기 상태로 설정하며,
 * idle 클래스의 마지막 서비스 시각을 현재 시각으로 맞춰 둔다.
 * 실행 컨텍스트: bfq_init_queue() 초기화 경로(프로세스 컨텍스트)에서
 * 호출되며, 아직 이 스케줄러로 I/O 가 들어오지 않는 시점이므로 락이
 * 필요 없다.
 * caller: bfq_init_queue() (root_group 생성 직후 한 번).
 * callee: 없음(순수 필드 초기화).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   bfq_init_queue → [bfq_init_root_group] → (필드 초기화로 종료)
 */
static void bfq_init_root_group(struct bfq_group *root_group,
				struct bfq_data *bfqd)
{
	int i;
	/* [한국어] 아래에서 ioprio 클래스별 서비스 트리를 초기화하기 위한 루프 변수. */

#ifdef CONFIG_BFQ_GROUP_IOSCHED
	root_group->entity.parent = NULL;
	/* [한국어] root_group 의 entity(스케줄링 엔티티)는 계층의 최상위이므로 부모가
	 * 없음을 명시 - B-WF2Q+ 가 상위로 더 올라가려 하지 않도록 한다. */
	root_group->my_entity = NULL;
	/* [한국어] "이 그룹을 나타내는, 상위 그룹 관점에서의 entity" 포인터도 NULL -
	 * root_group 은 어떤 상위 그룹에도 속하지 않으므로 자신을 표현하는 entity 가
	 * 없다. */
	root_group->bfqd = bfqd;
	/* [한국어] 이 그룹이 속한 BFQ 전역 상태를 역참조할 수 있게 연결 - cgroup 코드
	 * 경로에서 bfq_group 하나만 갖고도 bfqd 를 찾을 수 있게 한다. */
#endif
	root_group->rq_pos_tree = RB_ROOT;
	/* [한국어] 섹터 위치 기반으로 인접한 bfqq 를 찾는 rb-tree(협력 큐 탐색용)를
	 * 빈 트리로 초기화. */
	for (i = 0; i < BFQ_IOPRIO_CLASSES; i++)
		root_group->sched_data.service_tree[i] = BFQ_SERVICE_TREE_INIT;
		/* [한국어] RT/BE/IDLE 등 ioprio 클래스 개수만큼 존재하는 서비스 트리 각각을
		 * 빈 상태(BFQ_SERVICE_TREE_INIT 매크로가 정의한 초기값)로 설정 - 이 트리들이
		 * B-WF2Q+ 알고리즘이 실제로 bfqq/entity 를 정렬해 두는 자료구조다. */
	root_group->sched_data.bfq_class_idle_last_service = jiffies;
	/* [한국어] IDLE 클래스가 마지막으로 서비스받은 시각을 현재 시각으로 설정 - IDLE
	 * 클래스는 다른 클래스가 전혀 활동하지 않을 때만 서비스받으므로, 초기값을
	 * "방금 서비스했다"로 잡아 두어야 최초 기아 상태 판단이 왜곡되지 않는다. */
}

/*
 * [한국어]
 * bfq_init_queue - elevator_ops.init_sched 콜백. 사용자가 이 device 에
 * BFQ 스케줄러를 선택했을 때(또는 device 등록 시 기본 스케줄러로 선택될
 * 때) 호출되어, struct bfq_data(bfqd, 이 device 의 BFQ 전역 상태)를
 * 할당하고 모든 튜너블 기본값·자료구조를 초기화하는 BFQ 의 진입점이다.
 *
 * @q: BFQ 를 붙일 request_queue. q->disk->ia_ranges(독립 접근 영역),
 *     q->node(NUMA 노드), q->nr_requests(tag 개수) 등을 읽어 초기화에
 *     반영한다.
 * @eq: blk-mq 코어가 미리 할당해 둔 elevator_queue 컨테이너. 이 함수는
 *      eq->elevator_data 에 새로 할당한 bfqd 를 연결해 이후 모든
 *      elevator_ops 콜백이 이 bfqd 를 찾을 수 있게 한다.
 * @return: 성공 시 0, bfqd 또는 root_group 할당에 실패하면 -ENOMEM.
 *
 * 초기화는 크게 다음 단계로 진행된다: (1) bfqd 를 kzalloc_node 로 할당하고
 * elevator 에 연결, (2) 메모리 부족 시 폴백으로 쓰이는 oom_bfqq 를
 * 준비(영구 참조를 잡아 두어 정상 경로에서 실수로 해제되지 않게 함),
 * (3) 이 device 가 멀티 actuator(예: 여러 개의 독립 접근 영역을 갖는
 * 특수 HDD/SSD)를 지원하면 그 정보를 bfqd->sector[]/nr_sectors[] 에
 * 복사하고, 아니면 단일 actuator 로 취급, (4) dispatch 리스트/idle
 * hrtimer/active_list/idle_list/burst_list 등 런타임 자료구조 초기화,
 * (5) hw_tag/bfq_max_budget/bfq_fifo_expire 등 스케줄링 튜너블 기본값
 * 설정, (6) weight-raise(응답성 우대) 관련 계수와 peak_rate 초기 추정치
 * 설정, (7) bfqd->lock 초기화, (8) cgroup 계층(root_group)을 생성하고
 * 그 안에서 oom_bfqq 의 entity 를 초기화, (9) 이 스케줄러가 요구하는
 * request_queue 플래그(QUEUE_FLAG_SQ_SCHED, WBT 비활성화 등)를 설정하고
 * 통계 수집을 켠 뒤 async_depth 초기값을 계산한다.
 * bfq_create_group_hierarchy() 호출이 다른 모든 필드 초기화보다 뒤에
 * 오는 이유는 원본 주석에 설명돼 있다: 그 함수가 blkcg_activate_policy
 * → blk_mq_freeze_queue 로 이어지는 호출 체인을 통해 has_work 훅을
 * 건드릴 수 있는데, has_work 가 false 를 정확히 반환하려면 그 전에
 * 스케줄러 데이터가 이미 일관된 상태여야 하기 때문이다.
 * 실행 컨텍스트: 스케줄러 전환/device 등록 경로(프로세스 컨텍스트,
 * 보통 sysfs 를 통한 스케줄러 변경이나 device 초기화 중)에서 호출되며,
 * 이 시점에는 아직 이 bfqd 로 어떤 I/O 도 들어오지 않는다.
 * caller: elevator_init() → elevator_ops.init_sched.
 * callee: kzalloc_node(), bfq_init_bfqq(), bfq_ioprio_to_weight(),
 *         hrtimer_setup(), bfq_create_group_hierarchy(),
 *         bfq_init_root_group(), bfq_init_entity(), bfq_depth_updated().
 * 에러 경로: bfqd 할당 실패 시 즉시 -ENOMEM. root_group 생성 실패 시
 *           out_free 로 점프해 bfqd 를 해제하고 -ENOMEM 을 반환한다.
 *
 * 호출 체인:
 *   elevator_init → [bfq_init_queue] → bfq_create_group_hierarchy
 */
static int bfq_init_queue(struct request_queue *q, struct elevator_queue *eq)
{
	struct bfq_data *bfqd;
	/* [한국어] 이 함수에서 새로 할당해 device 전체의 BFQ 상태를 담을 포인터 - 성공하면
	 * eq->elevator_data 로 등록된다. */
	unsigned int i;
	/* [한국어] 아래에서 actuator 배열을 순회하기 위한 루프 변수. */
	struct blk_independent_access_ranges *ia_ranges = q->disk->ia_ranges;
	/* [한국어] 이 disk 가 멀티 actuator(예: 여러 헤드가 독립적으로 움직이는 HDD,
	 * 또는 여러 독립 네임스페이스 영역을 갖는 장치)를 지원한다면, gendisk 가 미리
	 * 채워 둔 "독립 접근 영역" 배열 - NULL 이면 단일 actuator 장치. */

	bfqd = kzalloc_node(sizeof(*bfqd), GFP_KERNEL, q->node);
	/* [한국어] bfq_data 전체를 0으로 초기화된 상태로 할당 - GFP_KERNEL(휴면 가능한
	 * 일반 할당), q->node 는 이 request_queue 와 같은 NUMA 노드에 메모리를 배치해
	 * 캐시 지역성을 높인다. kzalloc 이므로 명시적으로 대입하지 않는 필드는 모두
	 * 0/NULL/false 로 시작한다(아래에서 그 위에 필요한 값만 덮어씀). */
	if (!bfqd)
		return -ENOMEM;
		/* [한국어] 할당 실패(메모리 부족) - elevator_init() 에게 실패를 알려 다른
		 * 스케줄러로 폴백하거나 에러를 사용자에게 보고하게 한다. */

	eq->elevator_data = bfqd;
	/* [한국어] elevator_queue 컨테이너에 방금 할당한 bfqd 를 연결 - 이후 모든
	 * elevator_ops 콜백은 q->elevator->elevator_data 로 이 bfqd 를 찾는다. */

	spin_lock_irq(&q->queue_lock);
	/* [한국어] q->elevator 필드 갱신 동안 이 request_queue 의 큐 락을 잡아, 동시에
	 * 이 필드를 읽는 다른 경로(예: sysfs 조회)와의 경쟁을 막는다. */
	q->elevator = eq;
	/* [한국어] request_queue 가 사용할 elevator 를 이 eq(그리고 그 안의 bfqd)로
	 * 확정 - 이 시점부터 새 request 는 BFQ 의 insert/dispatch 훅을 거치게 된다. */
	spin_unlock_irq(&q->queue_lock);
	/* [한국어] 큐 락 해제. */

	/*
	 * Our fallback bfqq if bfq_find_alloc_queue() runs into OOM issues.
	 * Grab a permanent reference to it, so that the normal code flow
	 * will not attempt to free it.
	 * Set zero as actuator index: we will pretend that
	 * all I/O requests are for the same actuator.
	 */
	bfq_init_bfqq(bfqd, &bfqd->oom_bfqq, NULL, 1, 0, 0);
	/* [한국어] 메모리 부족으로 진짜 bfq_queue 를 할당할 수 없을 때 임시로 쓰이는
	 * "OOM 폴백 큐"(oom_bfqq, bfqd 안에 내장된 정적 멤버)를 초기화 - bic(NULL),
	 * is_sync(1: 동기로 취급), pid(0), actuator_idx(0: 항상 0번 actuator 로
	 * 간주)로 생성해, 실제 kmem_cache 할당 없이도 항상 유효한 bfqq 를 제공한다. */
	bfqd->oom_bfqq.ref++;
	/* [한국어] 참조 카운트를 미리 1 올려 "영구 참조"를 만든다 - 정상적인
	 * bfq_put_queue() 호출들이 반복되어도 이 참조 때문에 ref 가 0 이 되지 않으므로
	 * oom_bfqq 는 절대 free 되지 않는다(bfqd 수명과 함께함). */
	bfqd->oom_bfqq.new_ioprio = BFQ_DEFAULT_QUEUE_IOPRIO;
	/* [한국어] oom_bfqq 의 ioprio 를 BFQ 기본값으로 고정 - 이 큐는 특정 프로세스를
	 * 대표하지 않으므로 임의의 합리적 기본값을 쓴다. */
	bfqd->oom_bfqq.new_ioprio_class = IOPRIO_CLASS_BE;
	/* [한국어] ioprio 클래스도 일반적인 Best-Effort 로 고정. */
	bfqd->oom_bfqq.entity.new_weight =
		bfq_ioprio_to_weight(bfqd->oom_bfqq.new_ioprio);
	/* [한국어] 위에서 정한 ioprio 값을 B-WF2Q+ 가 사용하는 weight(가중치) 값으로
	 * 변환해 저장 - entity.weight 는 실제 활성화 시점에 new_weight 로부터
	 * 갱신된다(prio_changed 플래그를 통해, 아래에서 1로 설정). */

	/* oom_bfqq does not participate to bursts */
	bfq_clear_bfqq_just_created(&bfqd->oom_bfqq);
	/* [한국어] "방금 생성됨" 플래그를 꺼서 oom_bfqq 가 burst(다수 큐 동시 생성)
	 * 감지 로직의 대상이 되지 않게 한다 - oom_bfqq 는 여러 프로세스가 공유하는
	 * 특수한 폴백 큐이므로 일반적인 burst/weight-raise 판단에서 제외해야 한다. */

	/*
	 * Trigger weight initialization, according to ioprio, at the
	 * oom_bfqq's first activation. The oom_bfqq's ioprio and ioprio
	 * class won't be changed any more.
	 */
	bfqd->oom_bfqq.entity.prio_changed = 1;
	/* [한국어] entity 가 처음 활성화될 때 위에서 설정한 new_ioprio/new_weight 를
	 * 실제 weight 에 반영하도록 "prio 변경됨" 플래그를 미리 세팅 - 이후 oom_bfqq
	 * 의 ioprio 는 다시 바뀌지 않으므로 이 트리거는 최초 1회만 의미가 있다. */

	bfqd->queue = q;
	/* [한국어] bfqd 가 자신이 속한 request_queue 를 역참조할 수 있도록 저장 -
	 * 이후 여러 함수(bfq_exit_queue 등)에서 bfqd->queue 로 q 를 다시 찾는다. */

	bfqd->num_actuators = 1;
	/* [한국어] 기본값으로 "단일 actuator" 가정 - 아래에서 ia_ranges 를 확인해
	 * 실제로 멀티 actuator 라면 이 값을 덮어쓴다. */
	/*
	 * If the disk supports multiple actuators, copy independent
	 * access ranges from the request queue structure.
	 */
	spin_lock_irq(&q->queue_lock);
	/* [한국어] ia_ranges 조회 및 bfqd->sector[]/nr_sectors[] 기록 동안 q->queue_lock
	 * 을 잡아, gendisk 의 ia_ranges 갱신(드물지만 가능)과의 경쟁을 막는다. */
	if (ia_ranges) {
	/* [한국어] 이 disk 가 독립 접근 영역 정보를 제공한다면(멀티 actuator 가능성) -
	 * 그 개수와 범위를 확인한다. */
		/*
		 * Check if the disk ia_ranges size exceeds the current bfq
		 * actuator limit.
		 */
		if (ia_ranges->nr_ia_ranges > BFQ_MAX_ACTUATORS) {
		/* [한국어] 이 disk 가 보고하는 독립 영역 개수가 BFQ 가 정적으로 지원하는
		 * 최대 actuator 수(BFQ_MAX_ACTUATORS, bfqd->sector[]/nr_sectors[] 배열
		 * 크기)를 초과한다면 - 배열 오버플로를 피하기 위해 지원 범위를 넘는
		 * 나머지는 무시하고 단일 actuator 로 폴백해야 한다. */
			pr_crit("nr_ia_ranges higher than act limit: iars=%d, max=%d.\n",
				ia_ranges->nr_ia_ranges, BFQ_MAX_ACTUATORS);
				/* [한국어] 이례적인 하드웨어 구성이므로 커널 로그에 critical
				 * 수준으로 남겨 관리자가 인지할 수 있게 한다. */
			pr_crit("Falling back to single actuator mode.\n");
			/* [한국어] 실제 폴백 동작(아래 num_actuators==1 처리)이 뒤따를
			 * 것임을 로그로 명시. */
		} else {
		/* [한국어] 지원 범위 내의 정상적인 멀티 actuator 구성이라면 - 실제 개수와
		 * 각 actuator 의 LBA 범위를 bfqd 에 복사한다. */
			bfqd->num_actuators = ia_ranges->nr_ia_ranges;
			/* [한국어] 이 disk 가 보고한 실제 actuator(독립 접근 영역) 개수로
			 * 갱신 - 이후 모든 per-actuator 배열/루프가 이 값을 기준으로
			 * 동작한다. */

			for (i = 0; i < bfqd->num_actuators; i++) {
			/* [한국어] 각 actuator 에 대해 그 담당 LBA(Logical Block Address)
			 * 범위를 복사 - bfq_actuator_index() 가 이후 bio 의 시작 섹터를
			 * 이 범위들과 비교해 소속 actuator 를 판별하는 데 쓰인다. */
				bfqd->sector[i] = ia_ranges->ia_range[i].sector;
				/* [한국어] i 번째 actuator 가 담당하는 영역의 시작 섹터. */
				bfqd->nr_sectors[i] =
					ia_ranges->ia_range[i].nr_sectors;
				/* [한국어] i 번째 actuator 가 담당하는 영역의 섹터 개수(길이) -
				 * [sector[i], sector[i]+nr_sectors[i]) 범위가 이 actuator 의
				 * 담당 영역이 된다. */
			}
		}
	}

	/* Otherwise use single-actuator dev info */
	if (bfqd->num_actuators == 1) {
	/* [한국어] (ia_ranges 가 애초에 없었거나, 범위 초과로 폴백했거나, 원래도 1개인
	 * 경우) 최종적으로 단일 actuator 로 확정됐다면 - 이 disk 전체를 하나의
	 * actuator 영역으로 취급한다. */
		bfqd->sector[0] = 0;
		/* [한국어] 단일 actuator 는 섹터 0부터 시작 - 사실상 disk 전체. */
		bfqd->nr_sectors[0] = get_capacity(q->disk);
		/* [한국어] 이 disk 의 전체 용량(섹터 단위)을 그대로 유일한 actuator 의
		 * 범위로 설정 - 모든 bio 가 이 하나의 actuator 로 매핑된다. */
	}
	spin_unlock_irq(&q->queue_lock);
	/* [한국어] 큐 락 해제 - actuator 정보 설정이 끝났다. */

	INIT_LIST_HEAD(&bfqd->dispatch);
	/* [한국어] dispatch 대기 리스트(스케줄러가 골라 blk-mq 에 넘기기 직전 request 들이
	 * 잠시 머무는 리스트)를 빈 리스트로 초기화. */

	hrtimer_setup(&bfqd->idle_slice_timer, bfq_idle_slice_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	/* [한국어] idle slice hrtimer 를 준비 - 만료 콜백을 bfq_idle_slice_timer 로
	 * 지정하고, CLOCK_MONOTONIC(시스템 시각 재설정에 영향받지 않는 단조 시계),
	 * HRTIMER_MODE_REL(상대 시간 기준으로 무장)로 설정한다. 실제 시작(hrtimer_start)
	 * 은 나중에 bfq_arm_slice_timer() 가 필요할 때마다 수행한다. */

	bfqd->queue_weights_tree = RB_ROOT_CACHED;
	/* [한국어] device 전체에서 활성 bfqq 들의 weight 분포를 추적하는 rb-tree(캐시된
	 * 최좌측 노드 포인터 포함)를 빈 트리로 초기화 - weight-raise 여부 판단 등에
	 * 쓰인다. */
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	bfqd->num_groups_with_pending_reqs = 0;
	/* [한국어] cgroup 지원 빌드에서, 대기 중인 request 를 가진 bfq_group 의 개수를
	 * 0으로 초기화 - 그룹 간 공정성 판단(예: 활성 그룹이 하나뿐이면 idling을
	 * 다르게 처리)에 쓰인다. */
#endif

	INIT_LIST_HEAD(&bfqd->active_list[0]);
	/* [한국어] 활성 bfqq 리스트[0](일반적으로 한쪽 actuator/클래스 계열)를 빈
	 * 리스트로 초기화 - weight-raise 통계 등에서 활성 큐 전체를 순회할 때 쓰인다. */
	INIT_LIST_HEAD(&bfqd->active_list[1]);
	/* [한국어] 활성 bfqq 리스트[1](나머지 계열)도 동일하게 초기화 - 멀티 actuator
	 * 환경에서 두 개의 병렬 순회 리스트를 두는 구조. */
	INIT_LIST_HEAD(&bfqd->idle_list);
	/* [한국어] 서비스 트리에서 빠졌지만 아직 완전히 제거되지 않은 bfqq 들을 담는
	 * idle_list 를 빈 리스트로 초기화 - bfq_exit_queue() 가 해제 시 이 리스트를
	 * 순회하며 마무리 정리를 한다. */
	INIT_HLIST_HEAD(&bfqd->burst_list);
	/* [한국어] "짧은 시간에 한꺼번에 생성된 큐들"을 추적하는 burst_list(해시 리스트)
	 * 를 빈 상태로 초기화 - bfq_handle_burst() 가 이 리스트에 새 bfqq 를 추가/판정. */

	bfqd->hw_tag = -1;
	/* [한국어] "이 device 가 NCQ 류의 다중 in-flight tag 를 지원하는지" 를 나타내는
	 * 3 상태 플래그(-1: 아직 판정 전, 0: 미지원, 1: 지원) - 초기값 -1 은 "아직
	 * 충분한 표본을 관측하지 못했다"는 뜻이며, bfq_update_hw_tag() 가 실행 중
	 * 통계로 이 값을 확정짓는다. */
	bfqd->nonrot_with_queueing = !blk_queue_rot(bfqd->queue);
	/* [한국어] blk_queue_rot() 이 false(즉 회전형이 아닌 SSD/NVMe 라면) 이 필드가
	 * true 가 된다 - NVMe SSD 는 대개 이 조건에 해당하며, true 일 때 BFQ 는 seek
	 * 비용이 없다고 가정해 병합(merge)/idling 정책을 rotational 장치와 다르게
	 * (더 적은 idling, 더 적은 병합 시도) 적용한다. */

	bfqd->bfq_max_budget = bfq_default_max_budget;
	/* [한국어] 이 device 의 in-service bfqq 에 부여할 기본 최대 budget(섹터 또는
	 * 서비스 시간 한도) - 모듈 파라미터 bfq_default_max_budget 의 값을 그대로
	 * 채택하며, 이후 bfq_update_peak_rate() 등이 실측 처리율에 맞춰 조정한다. */

	bfqd->bfq_fifo_expire[0] = bfq_fifo_expire[0];
	/* [한국어] 동기(sync) request 의 FIFO 만료 시간(이 시간을 넘기면 순서를 어기고
	 * 서둘러 dispatch) 을 모듈 파라미터에서 가져온다 - 인덱스 0 은 sync. */
	bfqd->bfq_fifo_expire[1] = bfq_fifo_expire[1];
	/* [한국어] 비동기(async) request 의 FIFO 만료 시간 - 인덱스 1 은 async. 보통
	 * sync 보다 더 긴 만료 시간이 허용된다(응답성 요구가 낮으므로). */
	bfqd->bfq_back_max = bfq_back_max;
	/* [한국어] "뒤로 가는(backward seek)" request 를 허용할 최대 거리(섹터) -
	 * 이 거리를 넘는 역방향 seek 는 페널티를 받아 우선순위가 낮아진다. */
	bfqd->bfq_back_penalty = bfq_back_penalty;
	/* [한국어] 역방향 seek 에 적용할 페널티 배수 - 정렬(position 기준) 시 역방향
	 * 거리에 이 배수를 곱해 "사실상 더 먼 거리"로 취급, 앞쪽 request 를 우선한다. */
	bfqd->bfq_slice_idle = bfq_slice_idle;
	/* [한국어] sync bfqq 가 다음 request 를 기다리며 idling 할 기본 시간(ns 단위) -
	 * bfq_arm_slice_timer() 가 이 값을 기준으로 idle_slice_timer 를 무장한다. */
	bfqd->bfq_timeout = bfq_timeout;
	/* [한국어] in-service bfqq 에 허용하는 최대 서비스 시간(이 시간이 지나면 budget
	 * timeout 으로 만료 후보가 됨) - bfq_bfqq_budget_timeout() 이 이 값을 기준으로
	 * 판단한다. */

	bfqd->bfq_large_burst_thresh = 8;
	/* [한국어] 이 개수 이상의 bfqq 가 짧은 시간 안에 한꺼번에 생성되면 "large burst"
	 * (예: 셸 스크립트가 다수의 자식 프로세스를 fork 하며 각각 I/O 하는 상황)로
	 * 판정 - large burst 로 판정된 큐들은 weight-raise 대상에서 제외돼 오탐으로
	 * 인한 불공정을 막는다. */
	bfqd->bfq_burst_interval = msecs_to_jiffies(180);
	/* [한국어] 위 large_burst_thresh 개수를 셀 때 사용하는 "짧은 시간"의 기준 -
	 * 180ms 이내에 임계치만큼 큐가 생성되면 burst 로 인정한다. */

	bfqd->low_latency = true;
	/* [한국어] 저지연(low-latency) 모드를 기본 활성화 - 이 모드가 켜져 있으면
	 * 인터랙티브/소프트 리얼타임으로 보이는 bfqq 에 weight-raise 를 적용해
	 * 응답성을 우선한다(순수 처리량 최적화보다 사용자 체감 지연을 우선하는
	 * BFQ 의 기본 철학). */

	/*
	 * Trade-off between responsiveness and fairness.
	 */
	bfqd->bfq_wr_coeff = 30;
	/* [한국어] weight-raise 시 원래 weight 에 곱할 배수 - 인터랙티브 큐를 최대
	 * 30 배 우대해 즉각적인 응답성을 확보하되, 그 대가로 다른 큐들의 공정성이
	 * 일시적으로 희생된다(이름 그대로 "responsiveness vs fairness" 트레이드오프). */
	bfqd->bfq_wr_rt_max_time = msecs_to_jiffies(300);
	/* [한국어] 소프트 리얼타임(soft real-time)으로 판정된 bfqq 가 weight-raise 상태를
	 * 유지할 수 있는 최대 시간 - 이 시간을 넘기면 그냥 인터랙티브 판정 기준으로
	 * 되돌아간다. */
	bfqd->bfq_wr_min_idle_time = msecs_to_jiffies(2000);
	/* [한국어] bfqq 가 이 시간 이상 idle(요청 없음) 상태였다가 다시 활동을 재개하면
	 * "인터랙티브하게 재시작했다"고 보고 weight-raise 후보로 고려하는 최소 idle
	 * 시간 기준. */
	bfqd->bfq_wr_min_inter_arr_async = msecs_to_jiffies(500);
	/* [한국어] 비동기 bfqq 사이의 최소 도착 간격 기준 - 이보다 요청 간격이 넓은
	 * 비동기 워크로드는 인터랙티브에 가깝다고 보아 weight-raise 판단에 반영된다. */
	bfqd->bfq_wr_max_softrt_rate = 7000; /*
					      * Approximate rate required
					      * to playback or record a
					      * high-definition compressed
					      * video.
					      */
	/* [한국어] 소프트 리얼타임으로 인정할 최대 요청률(초당 섹터 등 단위) - 7000 이라는
	 * 값은 원본 주석대로 HD 압축 동영상 재생/녹화에 필요한 대략적 처리율을 기준
	 * 삼은 경험치이며, 이보다 빠른 워크로드는 리얼타임이 아니라 일반 처리량
	 * 위주로 간주한다. */
	bfqd->wr_busy_queues = 0;
	/* [한국어] 현재 weight-raise 상태로 활성 중인 bfqq 개수 카운터를 0으로 시작 -
	 * 이후 weight-raise 진입/해제 시마다 증감되며, 그룹 idling 판단 등에 쓰인다. */

	/*
	 * Begin by assuming, optimistically, that the device peak
	 * rate is equal to 2/3 of the highest reference rate.
	 */
	bfqd->rate_dur_prod = ref_rate[!blk_queue_rot(bfqd->queue)] *
		ref_wr_duration[!blk_queue_rot(bfqd->queue)];
	/* [한국어] 이 device 가 회전형인지(인덱스 0) SSD/NVMe 인지(인덱스 1)에 따라
	 * 서로 다른 참조 처리율 테이블(ref_rate)과 참조 weight-raise 지속시간
	 * 테이블(ref_wr_duration)에서 값을 골라 곱한 것 - "처리율 × 지속시간"의
	 * 기준값(rate_dur_prod)으로, 이후 실측 처리율에 비례해 weight-raise 지속시간을
	 * 스케일링하는 데 쓰인다. */
	bfqd->peak_rate = ref_rate[!blk_queue_rot(bfqd->queue)] * 2 / 3;
	/* [한국어] 아직 실측치가 없는 초기 상태이므로, 참조 처리율의 2/3(낙관적이지만
	 * 과대평가는 피한 값)을 이 device 의 최대 처리율(peak_rate) 추정치로 잠정
	 * 사용 - budget 계산과 인터랙티브 latency 예측의 기준이 되며, 실제 I/O 가
	 * 진행되면서 bfq_update_peak_rate() 가 점차 실측값으로 갱신한다. */

	/* see comments on the definition of next field inside bfq_data */
	bfqd->actuator_load_threshold = 4;
	/* [한국어] 멀티 actuator 환경에서 "이 actuator 가 충분히 부하를 받고 있다"고
	 * 판단할 in-flight request 수 임계값 - bfq_data 구조체 정의부 주석에 상세한
	 * 배경이 설명돼 있으며, 이 값 이상이면 해당 actuator 로의 추가 injection 등을
	 * 조절하는 판단 기준으로 쓰인다. */

	spin_lock_init(&bfqd->lock);
	/* [한국어] 이 device 의 BFQ 전역 스핀락을 초기화 - 이제부터 dispatch/completion/
	 * insert 등 거의 모든 BFQ 경로가 이 락으로 상호 배제된다. */

	/*
	 * The invocation of the next bfq_create_group_hierarchy
	 * function is the head of a chain of function calls
	 * (bfq_create_group_hierarchy->blkcg_activate_policy->
	 * blk_mq_freeze_queue) that may lead to the invocation of the
	 * has_work hook function. For this reason,
	 * bfq_create_group_hierarchy is invoked only after all
	 * scheduler data has been initialized, apart from the fields
	 * that can be initialized only after invoking
	 * bfq_create_group_hierarchy. This, in particular, enables
	 * has_work to correctly return false. Of course, to avoid
	 * other inconsistencies, the blk-mq stack must then refrain
	 * from invoking further scheduler hooks before this init
	 * function is finished.
	 */
	bfqd->root_group = bfq_create_group_hierarchy(bfqd, q->node);
	/* [한국어] cgroup 계층의 최상위(또는 cgroup 미지원 빌드에서는 유일한) bfq_group
	 * 을 생성 - 원본 주석대로 이 호출은 blkcg_activate_policy/blk_mq_freeze_queue
	 * 를 거쳐 has_work 훅까지 건드릴 수 있는 복잡한 체인의 시작점이므로, 반드시
	 * 다른 모든 bfqd 필드가 이미 일관된 상태로 설정된 뒤에 호출해야 한다. */
	if (!bfqd->root_group)
		goto out_free;
		/* [한국어] 그룹 계층 생성 실패(메모리 부족 등) - 이미 할당해 둔 bfqd 를
		 * 정리하는 out_free 레이블로 점프한다. */
	bfq_init_root_group(bfqd->root_group, bfqd);
	/* [한국어] 방금 만든 root_group 의 서비스 트리/rq_pos_tree 등을 초기 상태로
	 * 설정(위에서 정의한 bfq_init_root_group 함수). */
	bfq_init_entity(&bfqd->oom_bfqq.entity, bfqd->root_group);
	/* [한국어] oom_bfqq 의 스케줄링 entity 를 root_group 에 소속시켜 초기화 - 이제
	 * oom_bfqq 도 B-WF2Q+ 계층의 정상적인 구성원으로 활성화될 수 있다. */
	bfq_depth_updated(q);
	/* [한국어] async_depths[][] 테이블과 shallow depth 를 이 시점의 q->async_depth
	 * 기준으로 미리 한 번 계산해 둔다 - 아래에서 q->async_depth 를 다시 설정하지만,
	 * 이 함수는 재호출되지 않으므로 실제 유효 기준값은 아래 대입 이후의
	 * async_depth 이며, 이후 sysfs 를 통한 nr_requests 변경 시 bfq_depth_updated 가
	 * 다시 호출되어 갱신된다. */

	/* We dispatch from request queue wide instead of hw queue */
	blk_queue_flag_set(QUEUE_FLAG_SQ_SCHED, q);
	/* [한국어] "이 스케줄러는 하드웨어 큐(hctx) 단위가 아니라 request_queue 전체
	 * 단위로 dispatch 결정을 내린다"는 플래그를 설정 - BFQ 는 멀티 큐 하드웨어에서도
	 * 전역적으로 공정성을 관리해야 하므로, 개별 hctx 가 아니라 큐 전체 관점에서
	 * 스케줄링한다(NVMe 의 경우 여러 SQ 가 있어도 BFQ 는 이를 단일 관점으로 본다). */

	blk_queue_flag_set(QUEUE_FLAG_DISABLE_WBT_DEF, q);
	/* [한국어] 이 큐에 대해 "기본 WBT(Writeback Throttling) 비활성화"를 요청하는
	 * 플래그를 세운다 - BFQ 자신이 이미 쓰기 흐름을 조절하는 정교한 로직을 갖고
	 * 있으므로 WBT 와 이중으로 제어하면 오히려 성능/지연이 나빠질 수 있다. */
	wbt_disable_default(q->disk);
	/* [한국어] 실제로 이 disk 의 WBT 를 비활성화 - 위에서 세운 플래그를 즉시
	 * 적용하는 호출. */
	blk_stat_enable_accounting(q);
	/* [한국어] 이 request_queue 에 대해 blk_stat(요청 완료 시간 통계 수집)을
	 * 활성화 - bfq_update_peak_rate() 등이 참조하는 처리율 추정 통계의 원천. */
	q->async_depth = (q->nr_requests * 3) >> 2;
	/* [한국어] 비동기 I/O 에 허용할 기준 tag 심도를 nr_requests(이 큐의 전체 tag
	 * 수)의 3/4(75%)로 설정 - bfq_depth_updated() 가 이 값을 기준으로 sync/async
	 * × weight-raised 조합별 세부 비율(75/50/37.5/18.75%)을 계산한다. */

	return 0;
	/* [한국어] 모든 초기화가 성공적으로 끝났음을 elevator_init() 에 알린다. */

out_free:
	kfree(bfqd);
	/* [한국어] root_group 생성 실패로 인한 에러 경로 - 앞서 할당했던 bfqd 전체를
	 * 해제해 메모리 누수를 막는다(아직 어떤 I/O 도 이 bfqd 를 참조하지 않는
	 * 시점이므로 별도의 quiesce 없이 바로 해제해도 안전하다). */
	return -ENOMEM;
	/* [한국어] 초기화 실패를 elevator_init() 에 알린다. */
}

/*
 * [한국어]
 * bfq_slab_kill - bfq_queue 전용 kmem_cache(slab 캐시)를 해제하는 모듈 정리 함수
 *
 * @return: 없음 (void)
 *
 * BFQ는 프로세스/cgroup마다 struct bfq_queue를 매우 빈번하게 할당/해제하므로,
 * 범용 kmalloc 대신 bfq_queue 전용 slab 캐시(bfq_pool)를 만들어 할당 속도와
 * 캐시 지역성을 높인다. 이 함수는 그 slab 캐시를 파괴하는 카운터파트로,
 * bfq_slab_setup()에서 만든 bfq_pool을 kmem_cache_destroy()로 반납한다.
 * 실행 컨텍스트: 모듈 초기화 실패 경로(bfq_init()의 에러 처리) 또는 모듈
 * 언로드 경로(bfq_exit())에서만 호출되며, 이 시점에는 이미 모든 elevator
 * 인스턴스가 해제되어 살아있는 bfq_queue가 없어야 하므로 락이 필요 없다.
 * caller: bfq_init()(초기화 실패 시), bfq_exit()(모듈 제거 시).
 * callee: kmem_cache_destroy() (슬랩 서브시스템).
 *
 * 호출 체인:
 *   bfq_init()/bfq_exit() → [bfq_slab_kill] → kmem_cache_destroy()
 */
static void bfq_slab_kill(void)
{
	kmem_cache_destroy(bfq_pool);
	/* [한국어] bfq_slab_setup()에서 KMEM_CACHE()로 만든 bfq_queue 전용 slab을
	 * 파괴한다 - 이 시점 이후로는 bfq_pool에서 새 bfq_queue를 할당할 수 없다.
	 * NVMe 관점: bfq_queue는 NVMe request의 상위 스케줄링 단위이며, 이 slab은
	 * NVMe controller의 request_pool과는 별개의 커널 메모리 영역이다. */
}

/*
 * [한국어]
 * bfq_slab_setup - bfq_queue 전용 kmem_cache(slab 캐시)를 생성하는 모듈 초기화 함수
 *
 * @return: 성공 시 0, bfq_pool 할당 실패 시 -ENOMEM
 *
 * BFQ는 I/O를 발생시키는 프로세스/cgroup마다 struct bfq_queue를 동적으로
 * 할당하는데, 이 구조체는 크기가 크고 할당 빈도가 높아 전용 slab 캐시를
 * 두면 단편화를 줄이고 할당/해제 속도를 높일 수 있다. 이 함수는 모듈이
 * 처음 로드될 때(__init) 단 한 번 호출되어 전역 bfq_pool 포인터를 초기화한다.
 * 실행 컨텍스트: 모듈 적재 시 단일 스레드 컨텍스트(동시성 걱정 없음).
 * caller: bfq_init().
 * callee: KMEM_CACHE() (kmem_cache_create()의 래퍼 매크로).
 * 에러 처리: 할당 실패 시 -ENOMEM을 반환하고, 호출자인 bfq_init()은
 * err_pol_unreg 레이블로 점프해 이미 등록한 blkcg policy를 되돌린다.
 *
 * 호출 체인:
 *   bfq_init() → [bfq_slab_setup] → KMEM_CACHE()/kmem_cache_create()
 */
static int __init bfq_slab_setup(void)
{
	/* [한국어] struct bfq_queue 크기에 맞춘 전용 slab 캐시를 생성한다.
	 * 플래그 0은 SLAB_HWCACHE_ALIGN 등의 추가 옵션 없이 기본 정책을 사용한다는 뜻.
	 * 이 캐시는 이후 bfq_get_queue() 등에서 kmem_cache_alloc_node()로 사용된다. */
	bfq_pool = KMEM_CACHE(bfq_queue, 0);
	/* [한국어] slab 생성 실패 - 시스템 메모리 부족 등 극단적 상황.
	 * 이 경우 BFQ 자체를 elevator로 등록할 수 없으므로 즉시 실패 반환. */
	if (!bfq_pool)
		return -ENOMEM;
	return 0;
	/* [한국어] 정상 초기화 완료 - bfq_init()이 이어서 elv_register()를 호출하도록 허용. */
}

/*
 * [한국어]
 * bfq_var_show - sysfs show 콜백 공용 헬퍼: unsigned int 값을 문자열로 직렬화
 *
 * @var: 출력할 정수 값 (SHOW_FUNCTION 매크로가 생성한 각 *_show 함수에서 이미
 *       jiffies→ms 또는 ns→ms/us 단위 변환을 마친 값을 전달)
 * @page: sysfs read(2)가 채워질 출력 버퍼 (PAGE_SIZE 크기가 보장됨)
 * @return: sprintf()가 기록한 바이트 수 (sysfs 계층이 이 값을 read() 반환값으로 사용)
 *
 * /sys/block/<dev>/queue/iosched/<tunable> 파일을 cat 했을 때 커널이 호출하는
 * 모든 BFQ show 콜백이 마지막에 공통으로 거치는 경로다. 각 튜너블마다 단위
 * 변환 로직만 다르고 "숫자 하나를 개행 포함 문자열로 만든다"는 동작은 동일하므로
 * 이 함수로 중복을 제거했다. 실행 컨텍스트: sysfs read syscall을 처리하는
 * 유저 프로세스 컨텍스트(프로세스 컨텍스트, 인터럽트 아님).
 * caller: SHOW_FUNCTION/USEC_SHOW_FUNCTION 매크로가 생성하는 모든 *_show 함수.
 * callee: sprintf() (커널 문자열 포맷팅).
 *
 * 호출 체인:
 *   sysfs read() → elv_attr_show() → bfq_*_show() → [bfq_var_show] → sprintf()
 */
static ssize_t bfq_var_show(unsigned int var, char *page)
{
	return sprintf(page, "%u\n", var);
	/* [한국어] "<정수>\n" 형태로 페이지 버퍼에 기록 - sysfs 관례상 값 뒤에
	 * 개행을 붙여 cat 출력이 줄바꿈되도록 한다. 반환값은 기록된 문자 수. */
}

/*
 * [한국어]
 * bfq_var_store - sysfs store 콜백 공용 헬퍼: 문자열을 unsigned long으로 파싱
 *
 * @var: 파싱 결과를 저장할 출력 파라미터 - 호출자가 스택에 둔 지역 변수의 주소
 * @page: sysfs write(2)로 유저가 넘긴 입력 문자열 (10진수 텍스트 기대)
 * @return: 성공 시 0, 파싱 실패 시 kstrtoul()의 음수 에러코드 (예: -EINVAL)
 *
 * /sys/block/<dev>/queue/iosched/<tunable> 파일에 echo로 값을 쓸 때, 모든 BFQ
 * store 콜백이 공통으로 거치는 첫 단계다. 각 튜너블은 이 함수로 원시 정수를
 * 얻은 뒤 자신만의 MIN/MAX 클램프와 단위 변환(__CONV)을 추가로 적용한다.
 * 실행 컨텍스트: sysfs write syscall을 처리하는 유저 프로세스 컨텍스트.
 * caller: STORE_FUNCTION/USEC_STORE_FUNCTION 매크로가 생성하는 모든 *_store
 * 함수, 그리고 bfq_max_budget_store/bfq_timeout_sync_store/
 * bfq_strict_guarantees_store/bfq_low_latency_store.
 * callee: kstrtoul() (커널 문자열→정수 변환, 10진수 밑수 고정).
 * 에러 처리: 잘못된 입력(숫자가 아님, 오버플로 등)이면 kstrtoul()이 음수를
 * 반환하고, 이 값이 그대로 호출자에게 전파되어 write(2)가 실패로 리턴된다.
 *
 * 호출 체인:
 *   sysfs write() → elv_attr_store() → bfq_*_store() → [bfq_var_store] → kstrtoul()
 */
static int bfq_var_store(unsigned long *var, const char *page)
{
	unsigned long new_val;
	/* [한국어] 파싱 결과를 담을 지역 변수 - 성공 시에만 *var에 반영. */
	int ret = kstrtoul(page, 10, &new_val);
	/* [한국어] page 문자열을 10진수로 파싱해 new_val에 저장 - 실패 시 ret에 음수 에러코드. */

	if (ret)
		/* [한국어] 파싱 실패(숫자가 아니거나 범위 초과) - *var는 건드리지 않고 에러를 그대로 반환. */
		return ret;
	*var = new_val;
	/* [한국어] 파싱 성공 시에만 출력 파라미터에 반영 - 호출자는 이 값을 MIN/MAX로 클램프한다. */
	return 0;
	/* [한국어] 정상 종료 - 호출자(각 *_store)가 이어서 단위 변환/클램프를 수행하도록 함. */
}

/*
 * [한국어]
 * SHOW_FUNCTION(__FUNC, __VAR, __CONV) - sysfs "show" 콜백을 찍어내는 코드 생성 매크로
 *
 * @__FUNC: 생성할 함수 이름 (예: bfq_fifo_expire_sync_show). BFQ_ATTR(name) 매크로가
 *          "bfq_##name##_show" 규칙으로 이 이름을 찾아 bfq_attrs[]의 show 콜백에 연결한다.
 * @__VAR: elevator_queue->elevator_data(=bfqd)에서 읽어올 필드 표현식
 *         (예: bfqd->bfq_slice_idle). 매크로 전개 시 그대로 대입되어 초기값이 된다.
 * @__CONV: 출력 단위 변환 선택자 - 0=변환 없음(raw 값 그대로),
 *          1=jiffies_to_msecs()로 jiffies→ms 변환, 2=div_u64(.., NSEC_PER_MSEC)로
 *          나노초→ms 변환. 커널 내부 저장 단위(jiffies/ns)를 sysfs 사용자가 보기
 *          편한 ms 단위로 맞추기 위함이다.
 * @return: (전개되는 함수의 반환값) bfq_var_show()가 반환하는, sprintf()가 쓴 바이트 수
 *
 * BFQ는 fifo_expire_sync/async, back_seek_max/penalty, slice_idle, max_budget,
 * timeout_sync, strict_guarantees, low_latency 등 여러 sysfs 튜너블을 갖는데,
 * 이들의 show 콜백은 "bfqd 필드 하나를 읽어 단위 변환 후 문자열로 낸다"는
 * 동일한 패턴을 반복한다. 매번 손으로 같은 코드를 작성하면 오타/불일치 위험이
 * 있으므로, 이 매크로가 __FUNC/__VAR/__CONV 세 인자만 바꿔 함수 본체를 통째로
 * 찍어낸다(X-매크로 패턴). 매크로가 #undef 되기 전까지 아래 9개 SHOW_FUNCTION()
 * 호출이 실제로 9개의 static ssize_t bfq_*_show(struct elevator_queue *, char *)
 * 함수를 만들어낸다. 매크로 본문 내부는 backslash-newline으로 이어진 단일
 * 논리 줄이라 라인별 주석을 넣을 수 없으므로, 각 분기의 의미는 이 블록과
 * 아래 개별 호출 라인의 주석으로 설명한다: struct bfq_data *bfqd = e->elevator_data
 * 로 사설 데이터를 얻고, u64 __data = __VAR로 초기값을 읽은 뒤, __CONV==1이면
 * jiffies_to_msecs(), __CONV==2면 div_u64(.., NSEC_PER_MSEC)로 변환하고,
 * 마지막에 bfq_var_show()로 문자열화해 반환한다.
 * 실행 컨텍스트: 전개되어 생성된 각 *_show 함수는 사용자가 sysfs 파일을 cat 할 때
 * read(2) syscall을 처리하는 유저 프로세스 컨텍스트에서 실행된다(인터럽트 아님).
 * caller: elv_attr_show()가 elv_fs_entry.show 함수 포인터를 통해 각 *_show 호출.
 * callee: bfq_var_show() (문자열 직렬화 공통 헬퍼).
 *
 * 호출 체인:
 *   sysfs read() → elv_attr_show() → bfq_<name>_show() (매크로 전개 결과)
 *   → [bfq_var_show] → sprintf()
 */
#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)				\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct bfq_data *bfqd = e->elevator_data;			\
	u64 __data = __VAR;						\
	if (__CONV == 1)						\
		__data = jiffies_to_msecs(__data);			\
	else if (__CONV == 2)						\
		__data = div_u64(__data, NSEC_PER_MSEC);		\
	return bfq_var_show(__data, (page));				\
}
SHOW_FUNCTION(bfq_fifo_expire_sync_show, bfqd->bfq_fifo_expire[1], 2); /* [한국어] sysfs: .../iosched/fifo_expire_sync (show) - 동기 요청 FIFO 만료시간을 ns→ms로 변환해 노출(index 1=sync) */
SHOW_FUNCTION(bfq_fifo_expire_async_show, bfqd->bfq_fifo_expire[0], 2); /* [한국어] sysfs: .../iosched/fifo_expire_async (show) - 비동기 요청 FIFO 만료시간을 ns→ms로 변환해 노출(index 0=async) */
SHOW_FUNCTION(bfq_back_seek_max_show, bfqd->bfq_back_max, 0); /* [한국어] sysfs: .../iosched/back_seek_max (show) - 역방향(backward) seek 허용 최대 섹터 거리, 변환 없이 그대로 노출 */
SHOW_FUNCTION(bfq_back_seek_penalty_show, bfqd->bfq_back_penalty, 0); /* [한국어] sysfs: .../iosched/back_seek_penalty (show) - 역방향 seek에 곱해지는 배율 페널티, 변환 없이 그대로 노출 */
SHOW_FUNCTION(bfq_slice_idle_show, bfqd->bfq_slice_idle, 2); /* [한국어] sysfs: .../iosched/slice_idle (show) - 큐를 유휴 대기시키는 최대 시간을 ns→ms로 변환해 노출 */
SHOW_FUNCTION(bfq_max_budget_show, bfqd->bfq_user_max_budget, 0); /* [한국어] sysfs: .../iosched/max_budget (show) - 사용자가 지정한 최대 예산(섹터 수), 0이면 autotuning 모드 */
SHOW_FUNCTION(bfq_timeout_sync_show, bfqd->bfq_timeout, 1); /* [한국어] sysfs: .../iosched/timeout_sync (show) - 큐 예산 소비 제한시간을 jiffies→ms로 변환해 노출(sync/async 공용) */
SHOW_FUNCTION(bfq_strict_guarantees_show, bfqd->strict_guarantees, 0); /* [한국어] sysfs: .../iosched/strict_guarantees (show) - 강제 idling 활성화 여부(0/1), 변환 없이 노출 */
SHOW_FUNCTION(bfq_low_latency_show, bfqd->low_latency, 0); /* [한국어] sysfs: .../iosched/low_latency (show) - low-latency(weight-raising) 휴리스틱 활성화 여부(0/1) */
#undef SHOW_FUNCTION

/*
 * [한국어]
 * USEC_SHOW_FUNCTION(__FUNC, __VAR) - us(마이크로초) 단위 sysfs show 콜백 생성 매크로
 *
 * @__FUNC: 생성할 함수 이름 (예: bfq_slice_idle_us_show)
 * @__VAR: bfqd에서 읽어올 ns 단위 필드 표현식 (예: bfqd->bfq_slice_idle)
 * @return: (전개되는 함수의 반환값) sprintf()가 쓴 바이트 수
 *
 * SHOW_FUNCTION과 달리 __CONV 분기가 없고 항상 div_u64(.., NSEC_PER_USEC)로
 * ns→us 고정 변환만 수행한다. slice_idle은 기본 ms show(bfq_slice_idle_show)
 * 외에 더 세밀한 us 단위로도 읽고 싶은 사용자를 위해 slice_idle_us라는 별도
 * sysfs 파일을 하나 더 제공하기 위한 매크로다.
 * 실행 컨텍스트: sysfs read(2) 유저 프로세스 컨텍스트.
 * caller: elv_attr_show() → bfq_slice_idle_us_show().
 * callee: bfq_var_show().
 *
 * 호출 체인:
 *   sysfs read() → elv_attr_show() → bfq_slice_idle_us_show() → [bfq_var_show] → sprintf()
 */
#define USEC_SHOW_FUNCTION(__FUNC, __VAR)				\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct bfq_data *bfqd = e->elevator_data;			\
	u64 __data = __VAR;						\
	__data = div_u64(__data, NSEC_PER_USEC);			\
	return bfq_var_show(__data, (page));				\
}
USEC_SHOW_FUNCTION(bfq_slice_idle_us_show, bfqd->bfq_slice_idle); /* [한국어] sysfs: .../iosched/slice_idle_us (show) - slice_idle을 us 단위로 노출(ns/1000, 세밀 조정용) */
#undef USEC_SHOW_FUNCTION

/*
 * [한국어]
 * STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV) - sysfs "store" 콜백을 찍어내는 코드 생성 매크로
 *
 * @__FUNC: 생성할 함수 이름 (예: bfq_fifo_expire_sync_store)
 * @__PTR: 값을 써넣을 bfqd 필드의 주소 (예: &bfqd->bfq_fifo_expire[1])
 * @MIN: 허용 최소값 - 이보다 작게 입력하면 MIN으로 클램프
 * @MAX: 허용 최대값 - 이보다 크게 입력하면 MAX로 클램프
 * @__CONV: 입력 단위 변환 선택자 - 0=변환 없음(raw 대입), 1=msecs_to_jiffies()로
 *          ms→jiffies 변환, 2=(u64)__data*NSEC_PER_MSEC 로 ms→ns 변환.
 *          사용자가 echo로 쓰는 값은 항상 ms 단위이고, 커널 내부 저장 형식
 *          (jiffies 또는 ns)에 맞춰 변환해 저장한다.
 * @return: (전개되는 함수의 반환값) 성공 시 write(2)가 요청한 count(전체 문자열을
 *          소비했다는 뜻), 파싱 실패 시 bfq_var_store()의 음수 에러코드
 *
 * SHOW_FUNCTION의 대응(store) 버전으로, "문자열 파싱 → MIN/MAX 클램프 → 단위
 * 변환 → bfqd 필드에 대입"이라는 동일 패턴을 반복하는 5개 store 콜백을 한
 * 매크로로 찍어낸다(매크로 본문: bfq_var_store()로 파싱 → __min/__max로 클램프
 * → __CONV 분기(1=msecs_to_jiffies, 2=ms*NSEC_PER_MSEC, else=raw)로 변환해
 * *(__PTR)에 대입 → count 반환). max_budget/timeout_sync/strict_guarantees/
 * low_latency는 단순 클램프 이상의 부수 효과(예: max_budget 재계산, WR 종료)가
 * 필요해 이 매크로를 쓰지 않고 별도 함수로 직접 작성되어 있다.
 * 실행 컨텍스트: sysfs write(2) 유저 프로세스 컨텍스트. bfqd 필드를 락 없이
 * 직접 갱신하므로, 동시에 여러 프로세스가 같은 sysfs 파일에 쓰면 값 경쟁이
 * 있을 수 있으나 단순 튜너블이라 치명적이지 않다.
 * caller: elv_attr_store() → bfq_<name>_store().
 * callee: bfq_var_store() (문자열→정수 파싱 공통 헬퍼).
 *
 * 호출 체인:
 *   sysfs write() → elv_attr_store() → bfq_<name>_store() (매크로 전개 결과)
 *   → [bfq_var_store] → kstrtoul()
 */
#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t								\
__FUNC(struct elevator_queue *e, const char *page, size_t count)	\
{									\
	struct bfq_data *bfqd = e->elevator_data;			\
	unsigned long __data, __min = (MIN), __max = (MAX);		\
	int ret;							\
									\
	ret = bfq_var_store(&__data, (page));				\
	if (ret)							\
		return ret;						\
	if (__data < __min)						\
		__data = __min;						\
	else if (__data > __max)					\
		__data = __max;						\
	if (__CONV == 1)						\
		*(__PTR) = msecs_to_jiffies(__data);			\
	else if (__CONV == 2)						\
		*(__PTR) = (u64)__data * NSEC_PER_MSEC;			\
	else								\
		*(__PTR) = __data;					\
	return count;							\
}
STORE_FUNCTION(bfq_fifo_expire_sync_store, &bfqd->bfq_fifo_expire[1], 1,
		INT_MAX, 2); /* [한국어] sysfs: .../iosched/fifo_expire_sync (store) - 동기 FIFO 만료시간(ms 입력)을 1~INT_MAX로 클램프 후 ns로 변환해 저장(index 1=sync) */
STORE_FUNCTION(bfq_fifo_expire_async_store, &bfqd->bfq_fifo_expire[0], 1,
		INT_MAX, 2); /* [한국어] sysfs: .../iosched/fifo_expire_async (store) - 비동기 FIFO 만료시간(ms 입력)을 1~INT_MAX로 클램프 후 ns로 변환해 저장(index 0=async) */
STORE_FUNCTION(bfq_back_seek_max_store, &bfqd->bfq_back_max, 0, INT_MAX, 0); /* [한국어] sysfs: .../iosched/back_seek_max (store) - 역방향 seek 허용 최대 거리(섹터)를 0~INT_MAX로 클램프, 변환 없이 저장 */
STORE_FUNCTION(bfq_back_seek_penalty_store, &bfqd->bfq_back_penalty, 1,
		INT_MAX, 0); /* [한국어] sysfs: .../iosched/back_seek_penalty (store) - 역방향 seek 페널티 배율을 1~INT_MAX로 클램프, 변환 없이 저장 */
STORE_FUNCTION(bfq_slice_idle_store, &bfqd->bfq_slice_idle, 0, INT_MAX, 2); /* [한국어] sysfs: .../iosched/slice_idle (store) - 유휴 대기 최대시간(ms 입력)을 0~INT_MAX로 클램프 후 ns로 변환해 저장 */
#undef STORE_FUNCTION

/*
 * [한국어]
 * USEC_STORE_FUNCTION(__FUNC, __PTR, MIN, MAX) - us(마이크로초) 단위 sysfs store 콜백 생성 매크로
 *
 * @__FUNC: 생성할 함수 이름 (예: bfq_slice_idle_us_store)
 * @__PTR: 값을 써넣을 bfqd 필드 주소 (예: &bfqd->bfq_slice_idle)
 * @MIN: 허용 최소값(us 단위)
 * @MAX: 허용 최대값(us 단위)
 * @return: (전개되는 함수의 반환값) 성공 시 count, 실패 시 음수 에러코드
 *
 * STORE_FUNCTION과 달리 __CONV 분기 없이 (u64)__data*NSEC_PER_USEC 로 us→ns
 * 고정 변환만 수행한다. slice_idle_us sysfs 파일을 통해 ms보다 세밀한 us
 * 단위로 idle 시간을 직접 설정하고 싶은 사용자를 위한 매크로다.
 * 실행 컨텍스트: sysfs write(2) 유저 프로세스 컨텍스트.
 * caller: elv_attr_store() → bfq_slice_idle_us_store().
 * callee: bfq_var_store().
 *
 * 호출 체인:
 *   sysfs write() → elv_attr_store() → bfq_slice_idle_us_store()
 *   → [bfq_var_store] → kstrtoul()
 */
#define USEC_STORE_FUNCTION(__FUNC, __PTR, MIN, MAX)			\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)\
{									\
	struct bfq_data *bfqd = e->elevator_data;			\
	unsigned long __data, __min = (MIN), __max = (MAX);		\
	int ret;							\
									\
	ret = bfq_var_store(&__data, (page));				\
	if (ret)							\
		return ret;						\
	if (__data < __min)						\
		__data = __min;						\
	else if (__data > __max)					\
		__data = __max;						\
	*(__PTR) = (u64)__data * NSEC_PER_USEC;				\
	return count;							\
}
USEC_STORE_FUNCTION(bfq_slice_idle_us_store, &bfqd->bfq_slice_idle, 0,
		    UINT_MAX); /* [한국어] sysfs: .../iosched/slice_idle_us (store) - 유휴 대기 최대시간(us 입력)을 0~UINT_MAX로 클램프 후 ns로 변환해 저장 */
#undef USEC_STORE_FUNCTION

/*
 * [한국어]
 * bfq_max_budget_store - /sys/.../iosched/max_budget sysfs store 콜백: 최대 예산(budget) 설정
 *
 * @e: 이 BFQ 인스턴스의 elevator_queue - e->elevator_data에서 struct bfq_data를 얻음
 * @page: 유저가 write(2)로 넘긴 문자열 (섹터 수, 10진수, 0이면 autotuning 모드 요청)
 * @count: 입력 바이트 수 - 성공 시 그대로 반환해 write()가 전체 소비했음을 알림
 * @return: 성공 시 count, 파싱 실패 시 bfq_var_store()의 음수 에러코드
 *
 * BFQ의 budget은 한 bfq_queue가 in-service 상태에서 연속으로 처리할 수 있는
 * 섹터 수 상한이다. 사용자가 0을 쓰면 "자동조절(autotuning)" 모드로 전환되어
 * bfq_calc_max_budget()이 추정된 peak_rate로부터 매 갱신 시점마다 budget을
 * 재계산한다. 0이 아닌 값을 쓰면 그 값(INT_MAX로 상한 클램프)을 고정 budget으로
 * 사용하며, bfq_user_max_budget에 원본 입력을 기록해 이후 autotuning 여부
 * 판단(다른 함수에서 == 0 검사)에 사용한다.
 * 실행 컨텍스트: sysfs write(2) 유저 프로세스 컨텍스트. bfqd 필드를 락 없이
 * 직접 갱신하지만, 스케줄러 io_context/entity 갱신과는 독립적인 튜너블이라
 * 안전하다.
 * caller: elv_attr_store() (elv_fs_entry.store를 통해 BFQ_ATTR(max_budget) 연결).
 * callee: bfq_var_store(), bfq_calc_max_budget().
 *
 * 호출 체인:
 *   sysfs write() → elv_attr_store() → [bfq_max_budget_store] → bfq_calc_max_budget()
 */
static ssize_t bfq_max_budget_store(struct elevator_queue *e,
				    const char *page, size_t count)
{
	struct bfq_data *bfqd = e->elevator_data;
	/* [한국어] elevator_queue의 private 데이터에서 이 큐가 속한 bfq_data를 꺼낸다 -
	 * 모든 bfqd->bfq_* 튜너블 필드가 이 포인터를 통해 갱신된다. */
	unsigned long __data;
	/* [한국어] bfq_var_store()가 파싱한 원시 입력값(섹터 수)을 담을 지역 변수. */
	int ret;
	/* [한국어] bfq_var_store() 반환값(0=성공, 음수=파싱 실패) 보관용. */

	ret = bfq_var_store(&__data, (page));
	/* [한국어] 유저 입력 문자열을 10진수 unsigned long으로 파싱해 __data에 저장. */
	if (ret)
		return ret;
		/* [한국어] 파싱 실패(숫자가 아님 등) - bfqd를 건드리지 않고 에러 그대로 반환. */

	if (__data == 0)
		/* [한국어] 0 입력 = "autotuning 모드로 전환" 요청 - 고정값 대신 현재
		 * peak_rate 추정치로부터 budget을 즉시 계산해 채운다. */
		bfqd->bfq_max_budget = bfq_calc_max_budget(bfqd);
	else {
		/* [한국어] 0이 아닌 값 = 사용자가 고정 budget을 직접 지정 - 이후에는
		 * autotuning이 이 값을 덮어쓰지 않는다(bfq_user_max_budget != 0 확인). */
		if (__data > INT_MAX)
			/* [한국어] budget 필드가 int이므로 INT_MAX를 넘는 입력은 잘라낸다
			 * (오버플로/부호 반전 방지). */
			__data = INT_MAX;
		/* [한국어] 클램프된 값을 실제 사용 중인 max_budget에 즉시 반영. */
		bfqd->bfq_max_budget = __data;
	}

	/* [한국어] 사용자가 마지막으로 지정한 원본 값을 별도로 기록 - 0이면 이후
	 * bfq_timeout_sync_store() 등에서 "autotuning 모드"로 판단하는 기준이 된다. */
	bfqd->bfq_user_max_budget = __data;

	/* [한국어] write(2)가 입력 전체를 소비한 것으로 간주하도록 count를 그대로 반환. */
	return count;
}

/*
 * [한국어]
 * bfq_timeout_sync_store - /sys/.../iosched/timeout_sync sysfs store 콜백: 예산 소비 제한시간 설정
 *
 * @e: 이 BFQ 인스턴스의 elevator_queue
 * @page: 유저 입력 문자열 (ms 단위 정수)
 * @count: 입력 바이트 수
 * @return: 성공 시 count, 파싱 실패 시 음수 에러코드
 *
 * bfq_timeout은 bfq_queue 하나가 budget을 다 쓰지 못하더라도 강제로 서비스가
 * 종료되는 제한시간이다(cfq의 slice와 유사한 개념이며, 함수 이름에 "sync"가
 * 남아있는 건 cfq와의 명명 호환성 때문 - 실제로는 sync/async 공용). 값을
 * ms 단위로 받아 1~INT_MAX로 클램프한 뒤 msecs_to_jiffies()로 jiffies 단위로
 * 바꿔 저장한다. 이 타임아웃이 바뀌면 (peak_rate 기준) autotuning 중인
 * max_budget도 재계산해 timeout과 budget이 일관되도록 맞춘다.
 * 실행 컨텍스트: sysfs write(2) 유저 프로세스 컨텍스트.
 * caller: elv_attr_store() (BFQ_ATTR(timeout_sync) 연결).
 * callee: bfq_var_store(), bfq_calc_max_budget().
 *
 * 호출 체인:
 *   sysfs write() → elv_attr_store() → [bfq_timeout_sync_store] → bfq_calc_max_budget()
 */
/*
 * Leaving this name to preserve name compatibility with cfq
 * parameters, but this timeout is used for both sync and async.
 */
static ssize_t bfq_timeout_sync_store(struct elevator_queue *e,
				      const char *page, size_t count)
{
	struct bfq_data *bfqd = e->elevator_data;
	/* [한국어] elevator_queue에서 bfq_data를 꺼낸다. */
	unsigned long __data;
	/* [한국어] 파싱된 ms 단위 입력값. */
	int ret;
	/* [한국어] 파싱 결과 코드. */

	ret = bfq_var_store(&__data, (page));
	/* [한국어] 문자열을 정수로 파싱. */
	if (ret)
		return ret;
		/* [한국어] 파싱 실패 시 즉시 에러 반환. */

	if (__data < 1)
		/* [한국어] 0 이하는 의미 없는 타임아웃이므로 최소 1ms로 강제. */
		__data = 1;
	/* [한국어] jiffies 변환 후 int 필드에 담기므로 상한을 INT_MAX로 제한. */
	else if (__data > INT_MAX)
		__data = INT_MAX;

	/* [한국어] ms 입력을 jiffies로 변환해 실제 타임아웃 필드에 저장 - 이후
	 * bfq_queue의 budget 만료 판정에서 이 값이 기준이 된다. */
	bfqd->bfq_timeout = msecs_to_jiffies(__data);
	/* [한국어] 사용자가 max_budget을 0(autotuning)으로 둔 상태라면, 새
	 * timeout에 맞춰 peak_rate 기반 budget도 즉시 다시 계산해 둔다 -
	 * timeout과 budget이 서로 어긋난 채로 남아있지 않도록. */
	if (bfqd->bfq_user_max_budget == 0)
		bfqd->bfq_max_budget = bfq_calc_max_budget(bfqd);

	/* [한국어] 입력 전체를 소비한 것으로 간주. */
	return count;
}

/*
 * [한국어]
 * bfq_strict_guarantees_store - /sys/.../iosched/strict_guarantees sysfs store 콜백: 강제 idling 스위치
 *
 * @e: 이 BFQ 인스턴스의 elevator_queue
 * @page: 유저 입력 문자열 ("0" 또는 "1", 그 외 값은 1로 클램프되는 불리언 튜너블)
 * @count: 입력 바이트 수
 * @return: 성공 시 count, 파싱 실패 시 음수 에러코드
 *
 * strict_guarantees는 BFQ가 서비스 시간 보장을 위해 필요하면 짧게라도 무조건
 * idling(다음 요청을 기다림)을 강제할지 여부를 결정하는 불리언이다. 이 값을
 * 0→1로 켤 때, 만약 현재 slice_idle이 8ms보다 작게 설정돼 있으면 강제로
 * 8ms로 올린다 - strict 모드가 의미를 가지려면 idling 시간이 너무 짧아서는
 * 안 되기 때문이다(0에 가까운 idle로는 정확한 서비스 순서 보장이 무력화됨).
 * 실행 컨텍스트: sysfs write(2) 유저 프로세스 컨텍스트.
 * caller: elv_attr_store() (BFQ_ATTR(strict_guarantees) 연결).
 * callee: bfq_var_store().
 *
 * 호출 체인:
 *   sysfs write() → elv_attr_store() → [bfq_strict_guarantees_store]
 */
static ssize_t bfq_strict_guarantees_store(struct elevator_queue *e,
				     const char *page, size_t count)
{
	struct bfq_data *bfqd = e->elevator_data;
	/* [한국어] elevator_queue에서 bfq_data를 꺼낸다. */
	unsigned long __data;
	/* [한국어] 파싱된 입력값(0 또는 1로 클램프될 예정). */
	int ret;
	/* [한국어] 파싱 결과 코드. */

	ret = bfq_var_store(&__data, (page));
	/* [한국어] 문자열을 정수로 파싱. */
	if (ret)
		return ret;
		/* [한국어] 파싱 실패 시 즉시 에러 반환. */

	if (__data > 1)
		/* [한국어] 불리언 필드이므로 1을 초과하는 값은 모두 1(true)로 취급. */
		__data = 1;
	/* [한국어] 지금까지 꺼져 있던 strict_guarantees를 새로 켜는 전이(0→1)이고,
	 * 동시에 현재 idle 시간이 8ms 미만이면 - strict 모드가 실효성을 가지도록
	 * 최소 8ms로 끌어올린다(이 분기에 안 걸리면 기존 idle 값 유지). */
	if (!bfqd->strict_guarantees && __data == 1
	    && bfqd->bfq_slice_idle < 8 * NSEC_PER_MSEC)
		bfqd->bfq_slice_idle = 8 * NSEC_PER_MSEC;

	/* [한국어] 최종 클램프된 값을 실제 플래그 필드에 반영. */
	bfqd->strict_guarantees = __data;

	/* [한국어] 입력 전체를 소비한 것으로 간주. */
	return count;
}

/*
 * [한국어]
 * bfq_low_latency_store - /sys/.../iosched/low_latency sysfs store 콜백: low-latency 휴리스틱 스위치
 *
 * @e: 이 BFQ 인스턴스의 elevator_queue
 * @page: 유저 입력 문자열 ("0" 또는 "1", 그 외는 1로 클램프)
 * @count: 입력 바이트 수
 * @return: 성공 시 count, 파싱 실패 시 음수 에러코드
 *
 * low_latency는 대화형/소프트 실시간 워크로드에 weight-raising(가중치 상승)
 * 등의 지연시간 최적화 휴리스틱을 적용할지 여부다. 이 값을 1→0으로 끌 때는
 * 현재 진행 중인 모든 weight-raising 기간을 즉시 종료시켜야 하므로
 * bfq_end_wr()을 호출해 모든 활성/유휴 bfq_queue의 WR 상태를 강제로 해제한다
 * (0→1이나 1→1, 0→0 전이에서는 bfq_end_wr() 호출 없이 플래그만 갱신).
 * 실행 컨텍스트: sysfs write(2) 유저 프로세스 컨텍스트. bfq_end_wr() 내부에서
 * bfqd->lock을 직접 잡고 모든 active_list/idle_list를 순회하므로, 이 함수
 * 자체는 락을 잡지 않아도 안전하다.
 * caller: elv_attr_store() (BFQ_ATTR(low_latency) 연결).
 * callee: bfq_var_store(), bfq_end_wr().
 *
 * 호출 체인:
 *   sysfs write() → elv_attr_store() → [bfq_low_latency_store] → bfq_end_wr()
 */
static ssize_t bfq_low_latency_store(struct elevator_queue *e,
				     const char *page, size_t count)
{
	struct bfq_data *bfqd = e->elevator_data;
	/* [한국어] elevator_queue에서 bfq_data를 꺼낸다. */
	unsigned long __data;
	/* [한국어] 파싱된 입력값(0 또는 1로 클램프될 예정). */
	int ret;
	/* [한국어] 파싱 결과 코드. */

	ret = bfq_var_store(&__data, (page));
	/* [한국어] 문자열을 정수로 파싱. */
	if (ret)
		return ret;
		/* [한국어] 파싱 실패 시 즉시 에러 반환. */

	if (__data > 1)
		/* [한국어] 불리언 필드이므로 1을 초과하는 값은 모두 1(true)로 취급. */
		__data = 1;
	/* [한국어] "켜짐 → 꺼짐" 전이를 감지 - low_latency 기능을 끄는 순간
	 * 이미 진행 중인 weight-raising을 계속 두면 정책 불일치가 생기므로
	 * 모든 큐의 WR을 즉시 강제 종료한다. */
	if (__data == 0 && bfqd->low_latency != 0)
		bfq_end_wr(bfqd);
	/* [한국어] 최종 클램프된 값을 실제 플래그 필드에 반영. */
	bfqd->low_latency = __data;

	/* [한국어] 입력 전체를 소비한 것으로 간주. */
	return count;
}

/*
 * [한국어]
 * BFQ_ATTR(name) - sysfs 속성 하나(elv_fs_entry)를 조립하는 편의 매크로
 *
 * @name: 튜너블 이름 접미사 (예: slice_idle) - 실제로는 "bfq_slice_idle_show"와
 *        "bfq_slice_idle_store"라는 두 함수 이름과, sysfs에 노출될 파일명
 *        "slice_idle" 세 가지를 동시에 만들어낸다.
 * @return: (매크로 전개 결과) __ATTR(...)이 만든 struct elv_fs_entry 초기화 리터럴
 *
 * __ATTR(name, mode, show, store)는 <linux/sysfs.h> 계열의 표준 속성 생성
 * 매크로로, attr.name/attr.mode/show/store 필드를 한 번에 채운다. 0644는
 * 소유자 read/write, 그룹/기타 read만 허용하는 permission bit - 즉 root는
 * 값을 바꿀 수 있고 일반 사용자는 읽기만 가능하다. 이 매크로 덕분에
 * bfq_attrs[] 배열의 각 항목을 한 줄로 선언할 수 있다.
 *
 * 호출 체인:
 *   bfq_attrs[] 초기화 → [BFQ_ATTR] → __ATTR() → struct elv_fs_entry 리터럴
 */
#define BFQ_ATTR(name) \
	__ATTR(name, 0644, bfq_##name##_show, bfq_##name##_store)

/*
 * [한국어]
 * bfq_attrs - BFQ가 /sys/block/<dev>/queue/iosched/ 아래에 노출하는 sysfs 속성 테이블
 *
 * elevator_type.elevator_attrs에 연결되어, blk-mq elevator 코어(elv_register()
 * 경로에서 kobject/sysfs 트리를 만들 때)가 이 배열을 순회하며 파일들을 실제로
 * 생성한다. 배열은 __ATTR_NULL로 끝나는 sentinel 종료 방식이며, 각 항목은
 * BFQ_ATTR() 매크로로 만들어진 struct elv_fs_entry (attr.name/mode/show/store)다.
 * 사용자는 이 파일들에 값을 쓰거나 읽어 런타임에 BFQ 동작을 튜닝할 수 있다.
 */
static const struct elv_fs_entry bfq_attrs[] = {
	BFQ_ATTR(fifo_expire_sync), /* [한국어] sysfs 파일: fifo_expire_sync - 동기 요청 FIFO 만료시간(ms) */
	BFQ_ATTR(fifo_expire_async), /* [한국어] sysfs 파일: fifo_expire_async - 비동기 요청 FIFO 만료시간(ms) */
	BFQ_ATTR(back_seek_max), /* [한국어] sysfs 파일: back_seek_max - 역방향 seek 허용 최대 거리(섹터) */
	BFQ_ATTR(back_seek_penalty), /* [한국어] sysfs 파일: back_seek_penalty - 역방향 seek 페널티 배율 */
	BFQ_ATTR(slice_idle), /* [한국어] sysfs 파일: slice_idle - 유휴 대기 최대시간(ms) */
	BFQ_ATTR(slice_idle_us), /* [한국어] sysfs 파일: slice_idle_us - 유휴 대기 최대시간(us, 세밀 조정용) */
	BFQ_ATTR(max_budget), /* [한국어] sysfs 파일: max_budget - 최대 예산(섹터), 0=autotuning */
	BFQ_ATTR(timeout_sync), /* [한국어] sysfs 파일: timeout_sync - 예산 소비 제한시간(ms, sync/async 공용) */
	BFQ_ATTR(strict_guarantees), /* [한국어] sysfs 파일: strict_guarantees - 강제 idling 활성화 여부(0/1) */
	BFQ_ATTR(low_latency), /* [한국어] sysfs 파일: low_latency - low-latency(weight-raising) 휴리스틱 활성화 여부(0/1) */
	__ATTR_NULL /* [한국어] sysfs 배열 종료 sentinel - elevator 코어가 이 값을 만나면 순회를 멈춘다 */
};

/*
 * [한국어]
 * iosched_bfq_mq - BFQ를 blk-mq elevator 프레임워크에 등록하기 위한 elevator_type 인스턴스
 *
 * blk-mq 코어는 이 구조체 하나로 BFQ의 모든 진입점(콜백 함수 포인터)과
 * 메타데이터(icq 크기/정렬, sysfs 속성, 이름, 모듈 소유권)를 인식한다.
 * bfq_init()이 elv_register(&iosched_bfq_mq)로 이 구조체를 전역 elevator
 * 목록에 등록하면, 사용자가 "echo bfq > /sys/block/<dev>/queue/scheduler"로
 * 이 스케줄러를 선택할 수 있게 된다. .ops 안의 각 함수 포인터는 request가
 * blk-mq 큐를 거쳐가는 각 단계(depth 제한, prepare, dispatch, merge, 초기화/
 * 종료 등)에서 blk-mq 코어가 호출하는 BFQ 콜백들이다.
 */
static struct elevator_type iosched_bfq_mq = {
	.ops = {
		.limit_depth		= bfq_limit_depth, /* [한국어] tag 할당 전 depth 제한 - sync/async/cgroup별 blk-mq tag 수 제한(장치 슬롯 배분) */
		.prepare_request	= bfq_prepare_request, /* [한국어] request가 스케줄러에 삽입되기 전 bfq_queue와 연결 준비 */
		.requeue_request        = bfq_finish_requeue_request, /* [한국어] dispatch 되었다가 재큐잉되는 request 처리(에러/재시도 경로) */
		.finish_request		= bfq_finish_request, /* [한국어] request 완전 종료 시 bfq_queue/entity 정리 */
		.exit_icq		= bfq_exit_icq, /* [한국어] io_context 당 icq(bfq_io_cq) 해제 시 콜백 */
		.insert_requests	= bfq_insert_requests, /* [한국어] 새 request를 해당 bfq_queue의 정렬 트리/디스패치 리스트에 삽입 */
		.dispatch_request	= bfq_dispatch_request, /* [한국어] 다음에 driver로 내려보낼 request 선택(B-WF2Q+ 스케줄링 핵심) */
		.next_request		= elv_rb_latter_request, /* [한국어] 정렬 트리에서 현재 request 다음 request 조회(공용 elevator 헬퍼) */
		.former_request		= elv_rb_former_request, /* [한국어] 정렬 트리에서 현재 request 이전 request 조회(공용 elevator 헬퍼) */
		.allow_merge		= bfq_allow_bio_merge, /* [한국어] bio를 기존 request에 병합해도 되는지 정책적으로 허용/거부 */
		.bio_merge		= bfq_bio_merge, /* [한국어] 새 bio가 들어올 때 병합 가능한 request 탐색 */
		.request_merge		= bfq_request_merge, /* [한국어] request 단위 병합 가능 여부/방향(front/back) 판단 */
		.requests_merged	= bfq_requests_merged, /* [한국어] 두 request가 실제로 병합된 후 스케줄러 상태 갱신 */
		.request_merged		= bfq_request_merged, /* [한국어] bio가 기존 request에 병합된 후 스케줄러 상태 갱신 */
		.has_work		= bfq_has_work, /* [한국어] 이 hctx에 디스패치할 작업이 남아있는지 조회(idle 진입 판단) */
		.depth_updated		= bfq_depth_updated, /* [한국어] nr_requests 변경 시 sync/async tag 배분 재계산 */
		.init_sched		= bfq_init_queue, /* [한국어] 스케줄러 인스턴스(bfq_data) 생성 및 초기화(다른 에이전트 담당 구간) */
		.exit_sched		= bfq_exit_queue, /* [한국어] 스케줄러 인스턴스 해제(디바이스 제거/스케줄러 전환 시) */
	},

	.icq_size =		sizeof(struct bfq_io_cq), /* [한국어] per-io_context 컨텍스트(icq) 할당 크기 - blk-mq가 자동으로 할당/해제 */
	.icq_align =		__alignof__(struct bfq_io_cq), /* [한국어] icq 메모리 정렬 요구사항 - 구조체 내부 필드 정렬 보장 */
	.elevator_attrs =	bfq_attrs, /* [한국어] 이 스케줄러의 sysfs 튜너블 테이블(위에서 정의한 bfq_attrs[]) */
	.elevator_name =	"bfq", /* [한국어] /sys/block/<dev>/queue/scheduler 에서 선택하는 이름 문자열 */
	.elevator_owner =	THIS_MODULE, /* [한국어] 모듈 참조 카운트 - 사용 중에는 모듈 언로드를 막는다 */
};
MODULE_ALIAS("bfq-iosched"); /* [한국어] 구 모듈 이름("bfq-iosched")으로 요청해도 이 모듈이 로드되도록 별칭 등록 - 하위 호환성 유지 */

/*
 * [한국어]
 * bfq_init - BFQ 모듈 적재 진입점: elevator_type과 blkcg_policy를 커널에 등록
 *
 * @return: 성공 시 0, 실패 시 음수 에러코드(-ENOMEM 등)
 *
 * insmod/modprobe로 bfq 모듈이 로드될 때(__init) 단 한 번 호출된다. 순서가
 * 중요한 3단계 초기화를 수행한다: (1) CONFIG_BFQ_GROUP_IOSCHED가 켜져 있으면
 * cgroup 연동을 위한 blkcg_policy_bfq를 blkcg 서브시스템에 등록, (2) bfq_queue
 * 전용 slab 캐시 생성(bfq_slab_setup), (3) weight-raising 기준 지속시간
 * (ref_wr_duration[]) 초기화 후 elv_register()로 elevator_type을 블록 계층에
 * 등록. 각 단계는 실패 시 이전 단계에서 확보한 자원을 역순으로 되돌리는
 * goto 기반 에러 처리 체인을 따른다(slab_kill → err_pol_unreg).
 * 실행 컨텍스트: 모듈 로드 시 단일 스레드 컨텍스트 - 동시성 문제 없음.
 * caller: 커널 모듈 로더 (module_init(bfq_init) 매크로가 연결).
 * callee: blkcg_policy_register(), bfq_slab_setup(), elv_register().
 * 에러 처리: 각 단계 실패마다 이미 등록/할당된 자원을 정리하는 레이블로 점프.
 *
 * 호출 체인:
 *   insmod/modprobe → module_init 매크로 → [bfq_init] →
 *   blkcg_policy_register()/bfq_slab_setup()/elv_register()
 */
static int __init bfq_init(void)
{
	int ret;
	/* [한국어] 각 등록 단계의 성공/실패 코드를 담아 최종 반환값으로 쓸 변수. */

#ifdef CONFIG_BFQ_GROUP_IOSCHED
	/* [한국어] cgroup(blkio/io) 연동 빌드에서만 blkcg policy를 등록한다 - 이
	 * 설정이 꺼져 있으면 BFQ는 cgroup 가중치 기능 없이 동작한다. */
	ret = blkcg_policy_register(&blkcg_policy_bfq);
	/* [한국어] blkcg 서브시스템에 BFQ 전용 policy(cgroup별 통계/설정 구조체 정의)를
	 * 등록 - 이후 각 request_queue가 BFQ를 쓸 때 cgroup 계층 구조를 활용 가능. */
	if (ret)
		return ret;
		/* [한국어] policy 등록 자체가 실패하면(중복 등록 등) 더 진행할 이유가
		 * 없으므로 즉시 에러 반환 - 아직 아무 자원도 추가로 할당하지 않았음. */
#endif

	ret = -ENOMEM;
	/* [한국어] 이후 실패 경로들의 기본 에러코드를 미리 -ENOMEM으로 설정
	 * (slab 생성 실패도 사실상 메모리 부족이 원인이므로). */
	if (bfq_slab_setup())
		goto err_pol_unreg;
		/* [한국어] bfq_queue 전용 slab 생성 실패 - 앞서 등록한 blkcg policy를
		 * 되돌려야 하므로 err_pol_unreg 레이블로 점프(slab_kill은 건너뜀,
		 * 아직 slab을 만들지 못했으므로 destroy 할 것도 없음). */

	/*
	 * Times to load large popular applications for the typical
	 * systems installed on the reference devices (see the
	 * comments before the definition of the next
	 * array). Actually, we use slightly lower values, as the
	 * estimated peak rate tends to be smaller than the actual
	 * peak rate.  The reason for this last fact is that estimates
	 * are computed over much shorter time intervals than the long
	 * intervals typically used for benchmarking. Why? First, to
	 * adapt more quickly to variations. Second, because an I/O
	 * scheduler cannot rely on a peak-rate-evaluation workload to
	 * be run for a long time.
	 */
	ref_wr_duration[0] = msecs_to_jiffies(7000); /* actually 8 sec */
	/* [한국어] 회전형(rotational) 디스크(HDD) 기준 weight-raising 지속시간 - 실제
	 * "8초" 목표보다 살짝 짧은 7000ms를 써서 peak_rate 과대추정을 보정(위 주석 참고). */
	ref_wr_duration[1] = msecs_to_jiffies(2500); /* actually 3 sec */
	/* [한국어] 비회전형(nonrot, SSD/NVMe) 디스크 기준 weight-raising 지속시간 -
	 * NVMe처럼 빠른 장치는 "3초" 목표보다 짧은 2500ms로 설정. blk_queue_rot()의
	 * 결과(0=nonrot)로 이 배열의 인덱스를 선택해 사용한다(ref_wr_duration[!rot]). */

	ret = elv_register(&iosched_bfq_mq);
	/* [한국어] elevator_type 코어에 iosched_bfq_mq를 등록 - 이 호출이 성공하면
	 * 비로소 사용자가 scheduler=bfq로 전환하고 sysfs 속성 파일들이 나타난다. */
	if (ret)
		goto slab_kill;
		/* [한국어] elevator 등록 실패 - 이번엔 slab까지 만들어진 상태이므로
		 * slab_kill 레이블부터 시작해 slab 해제 → blkcg policy 해제까지
		 * 역순으로 모두 되돌린다. */

	return 0;
	/* [한국어] 3단계 모두 성공 - BFQ가 정상적으로 커널에 등록 완료. */

slab_kill:
	bfq_slab_kill();
	/* [한국어] elv_register() 실패 복구: 앞서 만든 bfq_queue slab을 파괴. */
err_pol_unreg:
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	blkcg_policy_unregister(&blkcg_policy_bfq);
	/* [한국어] bfq_slab_setup() 또는 elv_register() 실패 복구: 앞서 등록한
	 * blkcg policy를 해제해 부분적으로 초기화된 상태가 남지 않도록 한다. */
#endif
	return ret;
	/* [한국어] 실패 지점까지 누적된 에러코드를 그대로 모듈 로더에 반환 -
	 * insmod/modprobe가 이 값을 보고 로드 실패를 사용자에게 알린다. */
}

/*
 * [한국어]
 * bfq_exit - BFQ 모듈 제거 진입점: bfq_init()에서 등록한 자원을 역순으로 모두 해제
 *
 * @return: 없음 (void)
 *
 * rmmod로 모듈이 언로드될 때(__exit) 호출된다. bfq_init()이 등록한 순서의
 * 정확히 반대 순서로 정리한다: elevator_type 등록 해제 → (cgroup 빌드라면)
 * blkcg policy 등록 해제 → bfq_queue slab 캐시 파괴. elv_unregister()가
 * 먼저 호출되어야 하는 이유는, 이 시점에 이미 이 스케줄러를 사용 중인
 * request_queue가 없어야 이후 slab/policy 해제가 안전하기 때문이다(elevator
 * 코어가 elv_unregister() 내부에서 사용 중인 큐가 있으면 이 스케줄러로의
 * 재전환을 막는다).
 * 실행 컨텍스트: 모듈 언로드 시 단일 스레드 컨텍스트.
 * caller: 커널 모듈 언로더 (module_exit(bfq_exit) 매크로가 연결).
 * callee: elv_unregister(), blkcg_policy_unregister(), bfq_slab_kill().
 *
 * 호출 체인:
 *   rmmod → module_exit 매크로 → [bfq_exit] →
 *   elv_unregister()/blkcg_policy_unregister()/bfq_slab_kill()
 */
static void __exit bfq_exit(void)
{
	elv_unregister(&iosched_bfq_mq);
	/* [한국어] blk-mq elevator 코어에서 iosched_bfq_mq 등록을 해제 - 이후
	 * 사용자는 더 이상 scheduler=bfq를 선택할 수 없다. */
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	blkcg_policy_unregister(&blkcg_policy_bfq);
	/* [한국어] bfq_init()에서 등록했던 cgroup policy를 blkcg 서브시스템에서
	 * 제거 - bfq_init()과 정확히 대칭되는 정리 작업. */
#endif
	bfq_slab_kill();
	/* [한국어] bfq_queue 전용 slab 캐시를 마지막으로 파괴 - 이 시점에는 이미
	 * elv_unregister()로 모든 bfq_queue가 정리되었어야 하므로 안전하게 반납. */
}

module_init(bfq_init); /* [한국어] 모듈 적재(insmod/modprobe) 시 bfq_init()을 커널 init 콜 체인에 등록하는 표준 매크로 */
module_exit(bfq_exit); /* [한국어] 모듈 언로드(rmmod) 시 bfq_exit()을 커널 exit 콜 체인에 등록하는 표준 매크로 */

MODULE_AUTHOR("Paolo Valente"); /* [한국어] modinfo에 표시될 원작자 정보 - BFQ 알고리즘/구현의 원 저자 */
MODULE_LICENSE("GPL"); /* [한국어] 라이선스 선언 - GPL 전용 커널 심볼(EXPORT_SYMBOL_GPL 등) 사용을 허용받기 위한 필수 매크로 */
MODULE_DESCRIPTION("MQ Budget Fair Queueing I/O Scheduler"); /* [한국어] modinfo에 표시될 한 줄 설명 - blk-mq 기반 예산 공정 큐잉 스케줄러임을 명시 */
/*
 * NVMe 관점 핵심 요약
 *
 *  - BFQ 는 blk-mq 와 NVMe 드라이버 사이에서 request 를 정렬/선별/제한하며,
 *    장치/CQ 의 queue depth 와 CID 할당에 직접적인 영향을 준다.
 *  - nonrot_with_queueing=true 인 NVMe SSD 에서는 merge 를 억제하고
 *    injection 으로 NCQ depth 를 적극 활용하며, 필요할 때만 idle 을 허용한다.
 *  - num_actuators/rq_in_driver[] 를 통해 다중 actuator NVMe 장치의
 *    per-actuator 부하를 추정하고 병렬 throughput 을 극대화한다(추정).
 *  - bfq_limit_depth/bfq_depth_updated 는 장치/tag 자원을
 *    sync/async/cgroup 우선순위에 따라 배분한다.
 *  - bfq_finish_requeue_request/bfq_update_peak_rate 는 NVMe CQ completion
 *    정보를 바탕으로 처리율을 추정하고 다음 dispatch 결정에 반영한다.
 */
