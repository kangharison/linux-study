// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cgroups support for the BFQ I/O scheduler.
 */
/*
 * [한국어 설명] BFQ의 blkio cgroup 정책 구현체 (bfq-cgroup.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 blk-cgroup 프레임워크(block/blk-cgroup.c)가 정의하는 struct
 * blkcg_policy 인터페이스를 BFQ용으로 구현한다. cgroup(blkcg)이 생성/이동/
 * 삭제될 때마다 struct bfq_group(디바이스-cgroup 쌍 단위 스케줄링 단위)을
 * blkg_policy_data로 붙이고(cpd_alloc_fn/pd_alloc_fn/pd_init_fn), 사용자가
 * cgroupfs로 지정한 io.weight/io.bfq.weight 값을 struct bfq_group_data에
 * 저장했다가 bfq_entity->weight에 반영한다. 또한 CONFIG_BFQ_CGROUP_DEBUG가
 * 켜진 커널에서는 각 bfq_group의 완료/대기/서비스 시간을 bfqg_stats로
 * 누적해 cgroupfs 디버그 파일로 노출한다. 요약하면 "cgroup 계층 ↔ BFQ
 * H-WF2Q+ 스케줄링 트리"를 잇는 접착(glue) 코드다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (cgroup 라이프사이클):
 *   cgroup mkdir/rmdir → cgroup core → blkcg_css_alloc/online/offline/free()
 *     [block/blk-cgroup.c] → blkcg_policy_bfq.{cpd,pd}_{alloc,init,offline,free}_fn
 *     [이 파일] → struct bfq_group 생성/초기화/재배치/해제
 *
 * 호출 체인 (I/O 제출 시 cgroup 결정):
 *   submit_bio() → bio_associate_blkg() [blk-cgroup.c] → bio->bi_blkg 설정
 *     → blk_mq_submit_bio() → bfq_insert_request() [bfq-iosched.c]
 *     → bfq_bic_update_cgroup() [이 파일] → bfq_bio_bfqg() [이 파일]
 *     → (cgroup 변경 시) bfq_link_bfqg()/bfq_bfqq_move() [이 파일]
 *     → entity->sched_data 갱신 → B-WF2Q+ 트리 재편입
 *     → bfq_dispatch_request() → blk-mq hctx → nvme_queue_rq()
 *     → nvme_sq_copy_cmd/nvme_write_sq_db() (doorbell)
 *
 * 호출 체인 (사용자 weight 설정):
 *   `echo N > cgroupfs/io.bfq.weight` → kernfs write → bfq_io_set_weight[_legacy]()
 *     [이 파일] → bfq_group_set_weight() [이 파일] → entity->new_weight 갱신
 *     → entity->prio_changed = 1 → 다음 스케줄링 시점에
 *     __bfq_entity_update_weight_prio() [bfq-wf2q.c]가 실제 weight 반영
 *
 * 실행 컨텍스트: cgroup_mutex/rcu하에 실행되는 cgroup 라이프사이클 콜백
 * (process context), bfqd->lock을 쥔 상태의 스케줄러 콜백(bio 제출 경로,
 * process context 또는 blk-mq softirq), cgroupfs 파일 read/write 핸들러
 * (kernfs, process context).
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈:
 *   - block/bfq-iosched.h : struct bfq_group/bfq_group_data/bfq_entity/
 *     bfq_queue/bfq_data/bfq_sched_data/bfq_service_tree 등 핵심 자료구조와
 *     인라인 헬퍼(for_each_entity, bfq_entity_to_bfqq 등) 선언.
 *   - block/blk-cgroup.h : struct blkcg/blkcg_gq/blkg_policy_data/
 *     blkcg_policy_data/blkg_rwstat 등 blk-cgroup 코어 자료구조.
 *   - block/blk-cgroup.c : blkcg_policy_register(), blkg_conf_prep(),
 *     blkcg_print_blkgs() 등 정책 등록·설정·통계 출력 헬퍼.
 *   - block/bfq-iosched.c : bfq_activate_bfqq/bfq_deactivate_bfqq/
 *     bfq_bfqq_expire/bfq_schedule_dispatch 등 스케줄링 코어 함수.
 *   - block/bfq-wf2q.c : bfq_entity_service_tree/__bfq_deactivate_entity/
 *     bfq_put_idle_entity 등 B-WF2Q+ 트리 연산.
 * 의존받는 모듈:
 *   - block/bfq-iosched.c의 elevator 콜백들이 bfq_bic_update_cgroup(),
 *     bfq_bfqq_move(), bfq_bio_bfqg(), bfq_init_entity(),
 *     bfq_create_group_hierarchy() 등을 호출해 cgroup 계층을 참조한다.
 * 데이터 흐름: bio->bi_blkg(cgroup 링크) → bfq_bio_bfqg()가 struct
 *   bfq_group으로 변환 → bfq_group.entity.weight가 B-WF2Q+ 가상 시간
 *   계산에 입력 → bfq_group.sched_data.service_tree[]에 자식 bfq_queue/
 *   bfq_group entity가 배치되어 dispatch 순서를 결정.
 * 공유 핵심 자료구조: struct bfq_group(디바이스-cgroup 쌍 스케줄 단위,
 *   blkg_policy_data pd를 통해 blkcg_gq와 연결), struct bfq_group_data
 *   (per-blkcg weight 설정, blkcg_policy_data cpd를 통해 blkcg와 연결),
 *   struct bfqg_stats(cgroup별 완료/대기 시간 통계).
 *
 * === 주요 함수/구조체 요약 ===
 * bfq_pd_alloc/bfq_pd_init/bfq_pd_offline/bfq_pd_free() — bfq_group의
 *   생성-초기화-오프라인-해제 4단계 라이프사이클 콜백(blkcg_policy_bfq에 등록).
 * bfq_bio_bfqg()          — bio->bi_blkg로부터 사용할 bfq_group을 결정.
 * bfq_bic_update_cgroup()  — bio 제출 시 프로세스의 cgroup 변경 여부를 확인·반영.
 * bfq_bfqq_move()          — bfq_queue를 다른 bfq_group으로 이전(cgroup 이동).
 * bfq_group_set_weight()   — entity weight 갱신 및 prio_changed 트리거.
 * bfq_create_group_hierarchy() — elevator 초기화 시 blkcg 정책 활성화 및
 *   root bfq_group 획득.
 * struct bfq_group_data    — per-cgroup weight 저장소(struct bfq-iosched.h에
 *   정의, 여기서는 cpd_alloc_fn/cpd_free_fn으로 생성·해제만 담당).
 * struct bfqg_stats        — cgroup별 완료/대기/idle 시간 등 디버그 통계
 *   (struct bfq-iosched.h에 정의, 여기서 갱신 함수들을 구현).
 * blkcg_policy_bfq         — 이 파일이 blk-cgroup 코어에 등록하는 정책
 *   디스크립터(각 콜백 함수 포인터 테이블).
 */
#include <linux/module.h>
/* [한국어] MODULE_ALIAS 등 모듈 매크로 - BFQ가 "bfq" 커널 모듈로 빌드될 때
 * 필요한 모듈 등록 인프라를 제공한다(모듈 등록 자체는 bfq-iosched.c에서 수행). */
#include <linux/slab.h>
/* [한국어] kzalloc_node/kzalloc_obj/kfree 선언 - bfq_group/bfq_group_data를
 * cgroup 생성 시 동적 할당하고, 해제 시 반환하는 데 사용된다. */
#include <linux/blkdev.h>
/* [한국어] struct request_queue/gendisk, blk_opf_t, blk_rq_bytes() 등 블록
 * 계층 공통 선언 - bfqg_stats_update_*() 계열 함수가 request의 방향(read/
 * write)과 바이트 수를 읽어 cgroup 통계에 반영하는 데 필요하다. */
#include <linux/cgroup.h>
/* [한국어] struct cgroup_subsys_state(css)/cftype 등 cgroup 코어 API -
 * bfq.weight 등 cgroupfs 파일(cftype 배열)을 정의하고, css로부터 blkcg를
 * 역산(css_to_blkcg)하는 데 필요하다. */
#include <linux/ktime.h>
/* [한국어] blk_time_get_ns() 등이 기반하는 단조 시간 API - bfqg_stats의
 * wait_time/service_time/idle_time 등을 ns 단위로 측정하는 데 쓰인다. */
#include <linux/rbtree.h>
/* [한국어] struct rb_root/rb_node, rb_first() 등 rb-tree 연산 - bfq_group의
 * rq_pos_tree(LBA 위치 정렬)와 service_tree의 active/idle rb-tree 순회에
 * 필요하다(bfq_reparent_active_queues/bfq_flush_idle_tree 등). */
#include <linux/ioprio.h>
/* [한국어] IOPRIO_CLASS_* 등 I/O 우선순위 클래스 매크로 - bfq_reparent_*()가
 * BFQ_IOPRIO_CLASSES(RT/BE/IDLE)별로 service_tree를 순회할 때 사용된다. */
#include <linux/sbitmap.h>
/* [한국어] bfq-iosched.h가 간접적으로 요구하는 sbitmap 관련 선언 - 이
 * 파일 자체에서 직접 쓰이지는 않으나 헤더 의존성 충족을 위해 포함된다. */
#include <linux/delay.h>
/* [한국어] msleep 등 지연 함수 선언 - 핫패스보다는 헤더 의존성 및 향후
 * 확장을 위해 포함되어 있다(이 파일의 현재 코드 경로에서는 직접 사용되지 않음). */

#include "elevator.h"
/* [한국어] struct elevator_queue 등 elevator 프레임워크 선언 - bfq_pd_init()
 * 등에서 blkg->q->elevator->elevator_data로 bfq_data를 얻는 데 필요하다. */
#include "bfq-iosched.h"
/* [한국어] 이 파일이 구현을 채워 넣는 대상 헤더 - struct bfq_group/
 * bfq_group_data/bfq_entity/bfq_queue/bfq_data 등 모든 핵심 자료구조와
 * bfq_activate_bfqq/bfq_deactivate_bfqq 같은 스케줄링 코어 함수 선언이
 * 여기서 온다. bfq-iosched.c, bfq-wf2q.c와 이 헤더를 공유한다. */

#ifdef CONFIG_BFQ_CGROUP_DEBUG
/* [한국어] CONFIG_BFQ_CGROUP_DEBUG: 이 매크로가 꺼지면 아래 bfq_stat_*()
 * 계열 함수 전체와 세부 지연시간 cgroupfs 파일들이 컴파일에서 빠진다.
 * 디버그 통계는 percpu_counter/atomic64 갱신 비용이 있으므로, 운영
 * 환경에서는 끄고 기본 bytes/ios 통계만 유지하는 것이 일반적이다. */
/*
 * [한국어]
 * bfq_stat_init - per-cpu 카운터 기반 bfq_stat 하나를 초기화
 *
 * @stat: 초기화할 struct bfq_stat(대개 struct bfqg_stats의 개별 필드).
 * @gfp: percpu_counter 내부 배열 할당에 쓸 GFP 플래그.
 * @return: 0=성공, 음수=percpu_counter_init() 실패 시 그 에러코드.
 *
 * struct bfq_stat은 "percpu_counter cpu_cnt + atomic64_t aux_cnt"의
 * 조합으로, 다중 CPU에서 빈번히 갱신되는 완료/대기 시간 등을 락 없이
 * 누적하기 위한 자료구조다. cpu_cnt는 CPU마다 로컬로 갱신되다가 필요할
 * 때만 합산되고, aux_cnt는 자식 cgroup이 사라질 때 그 누적치를 부모로
 * 이관하는 보조 카운터다. 이 함수는 bfqg_stats_init()이 struct bfqg_stats
 * 안의 각 bfq_stat 필드마다 호출한다.
 * 실행 컨텍스트: bfq_pd_alloc() 호출 경로(cgroup 생성, process context).
 *
 * 호출 체인:
 *   bfqg_stats_init() → [bfq_stat_init] → percpu_counter_init()
 */
static int bfq_stat_init(struct bfq_stat *stat, gfp_t gfp)
{
	int ret; /* [한국어] percpu_counter_init()의 반환값(0=성공, 음수=메모리 부족 등 에러) 임시 저장 */

	/* [한국어] cpu_cnt를 초깃값 0으로 하는 percpu_counter로 초기화 -
	 * 내부적으로 CPU 개수만큼의 배열을 gfp 플래그로 할당한다. */
	ret = percpu_counter_init(&stat->cpu_cnt, 0, gfp);
	/* [한국어] percpu 배열 할당 실패(메모리 부족) 시 즉시 에러 반환 -
	 * 이 경우 aux_cnt는 아직 건드리지 않았으므로 별도 롤백 불필요 */
	if (ret)
		return ret;

	/* [한국어] aux_cnt를 0으로 초기화 - 아직 어떤 자식 cgroup의 통계도
	 * 이관받지 않은 최초 상태를 의미한다. */
	atomic64_set(&stat->aux_cnt, 0);
	return 0; /* [한국어] 초기화 성공 */
}

/*
 * [한국어]
 * bfq_stat_exit - bfq_stat이 점유한 percpu 메모리 해제
 *
 * @stat: 해제할 struct bfq_stat.
 * @return: 없음(void).
 *
 * bfq_stat_init()으로 할당된 percpu_counter 배열을 반환한다. aux_cnt는
 * atomic64_t라 별도 해제가 필요 없다. 이 그룹이 완전히 offline/free될 때
 * bfqg_stats_exit()이 각 필드에 대해 호출한다.
 *
 * 호출 체인:
 *   bfqg_stats_exit() → [bfq_stat_exit] → percpu_counter_destroy()
 */
static void bfq_stat_exit(struct bfq_stat *stat)
{
	percpu_counter_destroy(&stat->cpu_cnt); /* [한국어] percpu_counter가 보유한 CPU별 배열 메모리 해제 */
}

/**
 * bfq_stat_add - add a value to a bfq_stat
 * @stat: target bfq_stat
 * @val: value to add
 *
 * Add @val to @stat.  The caller must ensure that IRQ on the same CPU
 * don't re-enter this function for the same counter.
 */
/*
 * [한국어]
 * bfq_stat_add - bfq_stat에 값을 누적
 *
 * @stat: 누적 대상 struct bfq_stat.
 * @val: 더할 값(부호 없는 64비트로 취급되지만 내부적으로 음수 델타도 허용).
 * @return: 없음(void).
 *
 * percpu_counter_add_batch()를 그대로 감싸는 얇은 래퍼다. 배치 임계값
 * BLKG_STAT_CPU_BATCH를 넘을 때만 전역 카운트를 갱신하고, 그 전에는
 * CPU-로컬 카운터만 건드려 갱신 비용을 낮춘다. 원본 주석대로, 동일 CPU의
 * IRQ 컨텍스트가 같은 카운터에 대해 이 함수를 재진입하면 percpu_counter의
 * 로컬 카운트 갱신이 서로 겹쳐 값이 왜곡될 수 있으므로, 호출자가 그런
 * 재진입이 없음을 보장해야 한다(대개 bfqd->lock을 쥔 상태에서 호출).
 *
 * 호출 체인:
 *   bfqg_stats_update_*() 계열 → [bfq_stat_add] → percpu_counter_add_batch()
 */
static inline void bfq_stat_add(struct bfq_stat *stat, uint64_t val)
{
	percpu_counter_add_batch(&stat->cpu_cnt, val, BLKG_STAT_CPU_BATCH);
	/* [한국어] val을 CPU-로컬 카운터에 더하고, 로컬 누적치가
	 * BLKG_STAT_CPU_BATCH를 넘으면 전역 카운트에 반영 - 잦은 갱신 시
	 * 캐시라인 경합과 원자 연산 비용을 줄이기 위한 배치 처리다. */
}

/**
 * bfq_stat_read - read the current value of a bfq_stat
 * @stat: bfq_stat to read
 */
/*
 * [한국어]
 * bfq_stat_read - bfq_stat의 현재 합산값을 읽음
 *
 * @stat: 읽을 struct bfq_stat.
 * @return: 전역 카운트 + 모든 CPU의 로컬 잔여분을 합산한 현재값(음수면 0으로 보정).
 *
 * percpu_counter_sum_positive()는 모든 CPU를 순회하며 로컬 카운터를
 * 더하므로 percpu_counter_read()보다 느리지만 정확하다. cgroupfs
 * read(seq_show) 경로처럼 빈도가 낮고 정확도가 중요한 곳에서 쓰인다.
 *
 * 호출 체인:
 *   bfqg_prfill_*()/bfq_stat_add_aux() → [bfq_stat_read] → percpu_counter_sum_positive()
 */
static inline uint64_t bfq_stat_read(struct bfq_stat *stat)
{
	return percpu_counter_sum_positive(&stat->cpu_cnt);
	/* [한국어] 전역 카운트에 모든 CPU의 로컬 잔여값을 더한 합을 반환 -
	 * 다른 CPU가 갱신 중이더라도 최종적으로 음수가 되지 않도록 0으로 클램프됨 */
}

/**
 * bfq_stat_reset - reset a bfq_stat
 * @stat: bfq_stat to reset
 */
/*
 * [한국어]
 * bfq_stat_reset - bfq_stat을 0으로 리셋
 *
 * @stat: 리셋할 struct bfq_stat.
 * @return: 없음(void).
 *
 * cpu_cnt와 aux_cnt를 모두 0으로 되돌린다. cgroupfs의 stat reset
 * 인터페이스(pd_reset_stats_fn)나 자식 통계를 부모로 이관한 뒤 자식
 * 자신의 통계를 비우는 bfqg_stats_xfer_dead() 경로에서 사용된다.
 *
 * 호출 체인:
 *   bfqg_stats_reset() → [bfq_stat_reset]
 */
static inline void bfq_stat_reset(struct bfq_stat *stat)
{
	percpu_counter_set(&stat->cpu_cnt, 0); /* [한국어] percpu 전역/로컬 카운트를 모두 0으로 설정 */
	atomic64_set(&stat->aux_cnt, 0); /* [한국어] 이관받았던 자식 통계 잔재도 함께 제거 */
}

/**
 * bfq_stat_add_aux - add a bfq_stat into another's aux count
 * @to: the destination bfq_stat
 * @from: the source
 *
 * Add @from's count including the aux one to @to's aux count.
 */
/*
 * [한국어]
 * bfq_stat_add_aux - @from의 값을 @to의 aux_cnt에 누적
 *
 * @to: 누적받을 목적지 bfq_stat(대개 부모 그룹의 필드).
 * @from: 누적할 원본 bfq_stat(대개 사라지는 자식 그룹의 필드).
 * @return: 없음(void).
 *
 * @from의 "현재 합산값(cpu_cnt 총합) + 이미 이관받은 aux_cnt"를 함께
 * @to->aux_cnt에 더한다. 이렇게 하면 자식이 여러 세대에 걸쳐 이관을
 * 반복해도(예: 손자 cgroup까지) 값이 누락되지 않는다. cgroup이 offline될
 * 때(bfqg_stats_xfer_dead) 자식의 완료 통계가 조상 cgroup의 재귀 통계에서
 * 계속 보이도록 하기 위한 메커니즘이다.
 *
 * 호출 체인:
 *   bfqg_stats_add_aux() → [bfq_stat_add_aux] → bfq_stat_read()
 */
static inline void bfq_stat_add_aux(struct bfq_stat *to,
				     struct bfq_stat *from)
{
	atomic64_add(bfq_stat_read(from) + atomic64_read(&from->aux_cnt),
		      /* [한국어] from의 현재 합산값 + from이 이미 보유한 aux_cnt를 더해 이관 총량 계산 */
		     &to->aux_cnt);
		     /* [한국어] to->aux_cnt에 원자적으로 더함 - 동시에 여러 자식이 같은 부모로
		      * 이관되는 경쟁 상황에서도 atomic64_add로 안전 */
}

/**
 * blkg_prfill_stat - prfill callback for bfq_stat
 * @sf: seq_file to print to
 * @pd: policy private data of interest
 * @off: offset to the bfq_stat in @pd
 *
 * prfill callback for printing a bfq_stat.
 */
/*
 * [한국어]
 * blkg_prfill_stat - bfq_stat 필드 하나를 seq_file에 u64로 출력하는 콜백
 *
 * @sf: 출력 대상 seq_file(cgroupfs read 핸들러가 제공).
 * @pd: 현재 순회 중인 blkg의 policy data(struct bfq_group의 pd 필드).
 * @off: pd 시작 주소로부터 대상 bfq_stat 필드까지의 바이트 오프셋
 *       (offsetof(struct bfq_group, stats.xxx) 형태로 cftype.private에 저장됨).
 * @return: __blkg_prfill_u64()의 반환값(내부적으로 항상 0, blkcg_print_blkgs
 *          콜백 시그니처 준수용).
 *
 * blkcg_print_blkgs()가 각 blkg를 순회하며 이 콜백을 호출한다. (void *)pd + off
 * 포인터 산술로 pd 구조체 내부의 특정 bfq_stat 필드 주소를 계산한 뒤
 * bfq_stat_read()로 합산값을 얻어 출력한다.
 *
 * 호출 체인:
 *   bfqg_print_stat() → blkcg_print_blkgs() → [blkg_prfill_stat] (per-blkg)
 */
static u64 blkg_prfill_stat(struct seq_file *sf, struct blkg_policy_data *pd,
		int off)
{
	return __blkg_prfill_u64(sf, pd, bfq_stat_read((void *)pd + off));
	/* [한국어] pd+off 위치의 bfq_stat 값을 읽어(bfq_stat_read) u64 형식으로
	 * seq_file에 출력(__blkg_prfill_u64) - cftype.private에 저장된 오프셋으로
	 * 어떤 통계 필드를 보여줄지 결정된다. */
}

/* bfqg stats flags */
/* [한국어] struct bfqg_stats.flags에 저장되는 비트 플래그 열거형 - 각 값은
 * "현재 어떤 시간 측정이 진행 중인가"를 나타내며, 대응하는 start_*_time
 * 필드와 짝을 이뤄 중복 시작/중복 종료를 막는 데 쓰인다. */
enum bfqg_stats_flags {
	BFQG_stats_waiting = 0,
	/* [한국어] group_wait_time 측정이 진행 중임을 표시하는 비트 인덱스(0).
	 * 설정: bfqg_stats_set_start_group_wait_time()이 대기 시작 시 mark.
	 * 해제: bfqg_stats_update_group_wait_time()이 대기 종료(서비스 시작) 시 clear.
	 * 의미: 이 그룹이 in_service_queue로 선정되기를 기다리는 중인지 여부. */
	BFQG_stats_idling,
	/* [한국어] idle_time 측정이 진행 중임을 표시하는 비트 인덱스(1).
	 * 설정: bfqg_stats_set_start_idle_time()이 idling 시작 시 mark.
	 * 해제: bfqg_stats_update_idle_time()이 idling 종료 시 clear.
	 * 의미: BFQ가 이 그룹을 위해 장치를 일부러 놀리고(idle) 있는 중인지 여부. */
	BFQG_stats_empty,
	/* [한국어] empty_time 측정이 진행 중임을 표시하는 비트 인덱스(2).
	 * 설정: bfqg_stats_set_start_empty_time()이 대기 request가 없어질 때 mark.
	 * 해제: bfqg_stats_end_empty_time()이 새 request 도착 시 clear.
	 * 의미: 이 그룹에 dispatch 후보 request가 하나도 없는 상태인지 여부. */
};

/*
 * [한국어]
 * BFQG_FLAG_FNS(name) - bfqg_stats_{mark,clear,name}_##name 3종 함수를
 *   한 번에 찍어내는 매크로
 *
 * @name: waiting/idling/empty 중 하나(대응하는 BFQG_stats_##name enum 값 필요).
 *
 * 매크로 확장으로 아래 3개 정적 함수가 생성된다:
 *   - bfqg_stats_mark_##name(stats)  : 해당 비트를 세팅(flags |= ...)
 *   - bfqg_stats_clear_##name(stats) : 해당 비트를 해제(flags &= ~...)
 *   - bfqg_stats_##name(stats)       : 해당 비트가 세팅되어 있는지 bool로 반환
 * 세 함수 모두 struct bfqg_stats.flags 하나의 비트만 다루는 단순 접근자이며,
 * 별도 락을 잡지 않으므로 호출자가 bfqd->lock을 쥔 상태에서 호출해야 한다.
 * 이 매크로는 BFQG_FLAG_FNS(waiting)/idling/empty 세 번 호출된 뒤
 * #undef로 즉시 제거되어(매크로 오염 방지) 재사용되지 않는다.
 *
 * 호출 체인:
 *   bfqg_stats_update_*()/bfqg_stats_set_start_*() → [bfqg_stats_{mark,clear}_name] / [bfqg_stats_name]
 */
#define BFQG_FLAG_FNS(name)						\
static void bfqg_stats_mark_##name(struct bfqg_stats *stats)	\
{									\
	stats->flags |= (1 << BFQG_stats_##name);			\
	/* [한국어] name에 대응하는 비트를 flags에 OR로 세팅 - "측정 진행 중" 표시 */ \
}									\
static void bfqg_stats_clear_##name(struct bfqg_stats *stats)	\
{									\
	stats->flags &= ~(1 << BFQG_stats_##name);			\
	/* [한국어] name에 대응하는 비트를 flags에서 AND-NOT으로 해제 - "측정 종료" 표시 */ \
}									\
static int bfqg_stats_##name(struct bfqg_stats *stats)		\
{									\
	return (stats->flags & (1 << BFQG_stats_##name)) != 0;		\
	/* [한국어] name 비트가 세팅되어 있으면 1(true), 아니면 0(false) 반환 */ \
}									\

BFQG_FLAG_FNS(waiting) /* [한국어] bfqg_stats_{mark,clear}_waiting()/bfqg_stats_waiting() 생성 */
BFQG_FLAG_FNS(idling) /* [한국어] bfqg_stats_{mark,clear}_idling()/bfqg_stats_idling() 생성 */
BFQG_FLAG_FNS(empty) /* [한국어] bfqg_stats_{mark,clear}_empty()/bfqg_stats_empty() 생성 */
#undef BFQG_FLAG_FNS /* [한국어] 매크로 확장 완료 후 즉시 제거 - 이후 코드에서 오용 방지 */

/*
 * [한국어]
 * bfqg_stats_update_group_wait_time - 진행 중이던 group_wait_time 측정을 종료·누적
 *
 * @stats: 갱신할 struct bfqg_stats(bfq_group.stats).
 * @return: 없음(void).
 *
 * bfqg_stats_set_start_group_wait_time()이 기록해 둔 start_group_wait_time부터
 * 지금까지 흐른 시간을 group_wait_time에 더하고 waiting 플래그를 해제한다.
 * "그룹이 서비스를 받기 시작하는 순간"(예: 새 bfqq가 in_service_queue가
 * 되거나 이 그룹으로 첫 request가 들어오는 순간)에 호출되어 대기 구간을
 * 마감짓는다. bfqd->lock을 쥔 상태에서 호출해야 한다(원본 주석).
 *
 * 호출 체인:
 *   bfqg_stats_update_avg_queue_size() → [bfqg_stats_update_group_wait_time]
 */
static void bfqg_stats_update_group_wait_time(struct bfqg_stats *stats)
{
	u64 now; /* [한국어] 현재 시각(ns) - waiting 중이었다면 대기 종료 시각으로 쓰임 */

	/* [한국어] waiting 플래그가 꺼져 있으면(측정 중이 아니면) 아무 것도 하지 않고 반환 */
	if (!bfqg_stats_waiting(stats))
		return;

	now = blk_time_get_ns(); /* [한국어] 단조 증가 시계로 현재 시각을 얻음 - start_group_wait_time과 같은 시간축 */
	/* [한국어] 시작 시각보다 현재가 커야(정상적인 시간 흐름) 델타를 누적 -
	 * 시계 이상 등으로 역전된 경우는 방어적으로 스킵 */
	if (now > stats->start_group_wait_time)
		bfq_stat_add(&stats->group_wait_time,
			      now - stats->start_group_wait_time);
			      /* [한국어] 대기 시작부터 지금까지의 경과 시간을 group_wait_time에 누적 */
	bfqg_stats_clear_waiting(stats); /* [한국어] 측정을 마쳤으므로 waiting 플래그 해제 - 다음 대기 시작을 받아들일 준비 */
}

/* This should be called with the scheduler lock held. */
/*
 * [한국어]
 * bfqg_stats_set_start_group_wait_time - group_wait_time 측정 시작점 기록
 *
 * @bfqg: 대기를 시작하는(아직 서비스받지 못한) bfq_group.
 * @curr_bfqg: 현재 실제로 서비스 중인(in_service_queue가 속한) bfq_group.
 * @return: 없음(void).
 *
 * @bfqg가 @curr_bfqg와 동일하면(즉 이미 서비스 중이면) 대기라고 볼 수
 * 없으므로 아무 것도 하지 않는다. 그렇지 않고 아직 waiting 상태가 아니면
 * 현재 시각을 start_group_wait_time에 기록하고 waiting 플래그를 세운다.
 * 새 request가 이 그룹에 들어올 때(bfqg_stats_update_io_add)마다 호출되어,
 * 이 그룹이 얼마나 오래 서비스를 기다리는지 측정할 준비를 한다.
 *
 * 호출 체인:
 *   bfqg_stats_update_io_add() → [bfqg_stats_set_start_group_wait_time]
 */
static void bfqg_stats_set_start_group_wait_time(struct bfq_group *bfqg,
						 struct bfq_group *curr_bfqg)
{
	struct bfqg_stats *stats = &bfqg->stats; /* [한국어] 갱신 대상 통계 구조체 포인터 */

	/* [한국어] 이미 측정 중이면 시작 시각을 덮어쓰지 않고 그대로 반환(중복 시작 방지) */
	if (bfqg_stats_waiting(stats))
		return;
	/* [한국어] 자기 자신이 이미 서비스 중인 그룹이면 대기가 아니므로 측정 불필요 */
	if (bfqg == curr_bfqg)
		return;
	stats->start_group_wait_time = blk_time_get_ns(); /* [한국어] 대기 시작 시각 기록 */
	bfqg_stats_mark_waiting(stats); /* [한국어] waiting 플래그를 세워 "측정 진행 중" 표시 */
}

/* This should be called with the scheduler lock held. */
/*
 * [한국어]
 * bfqg_stats_end_empty_time - 진행 중이던 empty_time 측정을 종료·누적
 *
 * @stats: 갱신할 struct bfqg_stats.
 * @return: 없음(void).
 *
 * bfqg_stats_set_start_empty_time()이 기록한 start_empty_time부터 지금까지
 * 흐른 시간을 empty_time에 더한다. 이 그룹에 dispatch 후보가 하나도 없다가
 * (empty 상태) 새 request가 들어오는 순간(bfqg_stats_update_io_add)
 * 호출되어 "그룹이 비어 있던 시간"을 마감짓는다.
 *
 * 호출 체인:
 *   bfqg_stats_update_io_add() → [bfqg_stats_end_empty_time]
 */
static void bfqg_stats_end_empty_time(struct bfqg_stats *stats)
{
	u64 now; /* [한국어] 현재 시각(ns) */

	/* [한국어] empty 플래그가 꺼져 있으면(측정 중이 아니면) 즉시 반환 */
	if (!bfqg_stats_empty(stats))
		return;

	now = blk_time_get_ns(); /* [한국어] 현재 시각 조회 */
	/* [한국어] 정상적인 시간 흐름일 때만 델타 누적(시계 역전 방어) */
	if (now > stats->start_empty_time)
		bfq_stat_add(&stats->empty_time,
			      now - stats->start_empty_time);
			      /* [한국어] empty 상태로 머문 경과 시간을 empty_time에 누적 */
	bfqg_stats_clear_empty(stats); /* [한국어] 측정 종료 - empty 플래그 해제 */
}

/*
 * [한국어]
 * bfqg_stats_update_dequeue - bfq_group의 dequeue 카운터 증가
 *
 * @bfqg: 대상 bfq_group.
 * @return: 없음(void).
 *
 * 이 그룹의 entity가 B-WF2Q+ service tree에서 제거(dequeue)될 때마다
 * 호출되는 단순 카운터 증가 함수다. 스케줄링 이벤트 빈도를 관찰하기 위한
 * 디버그 지표이며, CONFIG_BFQ_CGROUP_DEBUG가 꺼지면 파일 하단의 no-op
 * 스텁으로 대체된다.
 *
 * 호출 체인:
 *   bfq-wf2q.c의 entity dequeue 경로 → [bfqg_stats_update_dequeue]
 */
void bfqg_stats_update_dequeue(struct bfq_group *bfqg)
{
	bfq_stat_add(&bfqg->stats.dequeue, 1); /* [한국어] dequeue 카운터를 1 증가 */
}

/*
 * [한국어]
 * bfqg_stats_set_start_empty_time - empty_time 측정 시작점 기록
 *
 * @bfqg: 대상 bfq_group.
 * @return: 없음(void).
 *
 * 이 그룹에 큐잉된 request가 하나도 남지 않은 순간(queued 통계 합이 0)에
 * 호출되어 "그룹이 비기 시작한 시각"을 기록한다. 이미 empty로 표시되어
 * 있으면 원본 주석대로 "bfqq가 부모 그룹에서 새 request를 받고 service
 * tree에 추가되는 도중 이 그룹으로 옮겨온" 것과 같은 중복 이벤트일 수
 * 있으므로 무시한다.
 *
 * 호출 체인:
 *   bfq-wf2q.c/bfq-iosched.c의 큐 비활성화 경로 → [bfqg_stats_set_start_empty_time]
 */
void bfqg_stats_set_start_empty_time(struct bfq_group *bfqg)
{
	struct bfqg_stats *stats = &bfqg->stats; /* [한국어] 대상 통계 구조체 포인터 */

	/* [한국어] 아직 큐잉된 request가 남아 있으면(합이 0이 아니면) empty가 아니므로 반환 */
	if (blkg_rwstat_total(&stats->queued))
		return;

	/*
	 * group is already marked empty. This can happen if bfqq got new
	 * request in parent group and moved to this group while being added
	 * to service tree. Just ignore the event and move on.
	 */
	/* [한국어] 이미 empty로 표시된 경우 원본 주석의 레이스를 무시하고 그대로 반환 */
	if (bfqg_stats_empty(stats))
		return;

	stats->start_empty_time = blk_time_get_ns(); /* [한국어] empty 시작 시각 기록 */
	bfqg_stats_mark_empty(stats); /* [한국어] empty 플래그를 세워 측정 진행 중 표시 */
}

/*
 * [한국어]
 * bfqg_stats_update_idle_time - 진행 중이던 idle_time 측정을 종료·누적
 *
 * @bfqg: 대상 bfq_group.
 * @return: 없음(void).
 *
 * bfqg_stats_set_start_idle_time()이 기록한 start_idle_time부터 지금까지의
 * 경과를 idle_time에 더한다. BFQ가 이 그룹을 위해 device를 의도적으로
 * idling(다음 request를 기다리며 dispatch를 미룸)하다가 idling을 끝내는
 * 시점에 호출된다.
 *
 * 호출 체인:
 *   bfq-iosched.c의 idle timer 만료/재요청 도착 경로 → [bfqg_stats_update_idle_time]
 */
void bfqg_stats_update_idle_time(struct bfq_group *bfqg)
{
	struct bfqg_stats *stats = &bfqg->stats; /* [한국어] 대상 통계 구조체 포인터 */

	/* [한국어] idling 플래그가 세팅되어 있을 때만(측정 중일 때만) 종료 처리 */
	if (bfqg_stats_idling(stats)) {
		u64 now = blk_time_get_ns(); /* [한국어] 현재 시각(ns) */

		/* [한국어] 정상적인 시간 흐름일 때만 누적(시계 역전 방어) */
		if (now > stats->start_idle_time)
			bfq_stat_add(&stats->idle_time,
				      now - stats->start_idle_time);
				      /* [한국어] idling으로 보낸 경과 시간을 idle_time에 누적 */
		bfqg_stats_clear_idling(stats); /* [한국어] 측정 종료 - idling 플래그 해제 */
	}
}

/*
 * [한국어]
 * bfqg_stats_set_start_idle_time - idle_time 측정 시작점 기록
 *
 * @bfqg: 대상 bfq_group.
 * @return: 없음(void).
 *
 * BFQ가 이 그룹을 위해 device idling을 시작하는 순간 호출되어 시작
 * 시각을 기록하고 idling 플래그를 세운다. (waiting/empty와 달리 이미
 * idling 중인지 별도 확인 없이 무조건 덮어쓰는데, idling 시작은 항상
 * 명시적인 단일 지점에서만 트리거되기 때문이다.)
 *
 * 호출 체인:
 *   bfq-iosched.c의 idling 시작 결정 경로 → [bfqg_stats_set_start_idle_time]
 */
void bfqg_stats_set_start_idle_time(struct bfq_group *bfqg)
{
	struct bfqg_stats *stats = &bfqg->stats; /* [한국어] 대상 통계 구조체 포인터 */

	stats->start_idle_time = blk_time_get_ns(); /* [한국어] idling 시작 시각 기록 */
	bfqg_stats_mark_idling(stats); /* [한국어] idling 플래그 세팅 */
}

/*
 * [한국어]
 * bfqg_stats_update_avg_queue_size - 평균 큐 크기 계산용 표본 누적
 *
 * @bfqg: 대상 bfq_group.
 * @return: 없음(void).
 *
 * 현재 이 그룹에 큐잉된 request 수(queued 통계의 방향 합)를
 * avg_queue_size_sum에 더하고 표본 수(avg_queue_size_samples)를 1
 * 늘린다. 이후 bfqg_prfill_avg_queue_size()가 sum/samples로 평균을
 * 계산해 cgroupfs에 노출한다. 매 표본 채취 시점에 마침 group_wait_time
 * 측정도 함께 마감짓는다(같은 이벤트 지점에서 여러 통계를 갱신).
 *
 * 호출 체인:
 *   bfq-iosched.c의 dispatch/활성화 경로 → [bfqg_stats_update_avg_queue_size]
 *     → bfqg_stats_update_group_wait_time()
 */
void bfqg_stats_update_avg_queue_size(struct bfq_group *bfqg)
{
	struct bfqg_stats *stats = &bfqg->stats; /* [한국어] 대상 통계 구조체 포인터 */

	/* [한국어] 현재 큐잉 개수(읽기+쓰기 합)를 누적 합에 더함 - 평균의 분자 */
	bfq_stat_add(&stats->avg_queue_size_sum,
		      blkg_rwstat_total(&stats->queued));
	bfq_stat_add(&stats->avg_queue_size_samples, 1); /* [한국어] 표본 개수 1 증가 - 평균의 분모 */
	bfqg_stats_update_group_wait_time(stats); /* [한국어] 이 시점에 group_wait_time 측정도 함께 마감 */
}

/*
 * [한국어]
 * bfqg_stats_update_io_add - request가 이 bfq_group에 추가될 때 통계 갱신
 *
 * @bfqg: request가 추가된 bfq_group.
 * @bfqq: request를 보유한 bfq_queue.
 * @opf: request의 방향/플래그(REQ_OP_READ/WRITE 등, blkg_rwstat 방향 결정에 사용).
 * @return: 없음(void).
 *
 * queued 통계를 1 늘리고, 지금까지 empty 상태였다면 그 구간을
 * 마감짓는다(bfqg_stats_end_empty_time). 그리고 이 request의 bfqq가
 * 현재 in_service_queue가 아니라면(=아직 서비스를 못 받고 있다면)
 * group_wait_time 측정을 시작한다.
 *
 * 호출 체인:
 *   bfq_insert_request() [bfq-iosched.c] → [bfqg_stats_update_io_add]
 *     → bfqg_stats_end_empty_time() / bfqg_stats_set_start_group_wait_time()
 */
void bfqg_stats_update_io_add(struct bfq_group *bfqg, struct bfq_queue *bfqq,
			      blk_opf_t opf)
{
	blkg_rwstat_add(&bfqg->stats.queued, opf, 1); /* [한국어] opf 방향의 queued 카운터를 1 증가 */
	bfqg_stats_end_empty_time(&bfqg->stats); /* [한국어] 지금까지 empty였다면 그 구간을 마감 */
	/* [한국어] 이 bfqq가 현재 서비스 중인 큐가 아니면(=대기 상태) */
	if (!(bfqq == bfqg->bfqd->in_service_queue))
		bfqg_stats_set_start_group_wait_time(bfqg, bfqq_group(bfqq));
		/* [한국어] bfqq_group(bfqq)=현재 실제 서비스 중인 그룹을 기준으로 group_wait_time 측정 시작 */
}

/*
 * [한국어]
 * bfqg_stats_update_io_remove - request가 이 bfq_group에서 제거될 때 통계 갱신
 *
 * @bfqg: request가 제거된 bfq_group.
 * @opf: request의 방향/플래그.
 * @return: 없음(void).
 *
 * request가 완료되거나(완료 처리 경로) 취소/병합되어 사라질 때 queued
 * 카운터를 1 줄인다. bfqg_stats_update_io_add()와 정확히 대칭되는 함수다.
 *
 * 호출 체인:
 *   bfq_remove_request() [bfq-iosched.c] → [bfqg_stats_update_io_remove]
 */
void bfqg_stats_update_io_remove(struct bfq_group *bfqg, blk_opf_t opf)
{
	blkg_rwstat_add(&bfqg->stats.queued, opf, -1); /* [한국어] opf 방향의 queued 카운터를 1 감소 */
}

/*
 * [한국어]
 * bfqg_stats_update_io_merged - bio-merge로 request 하나가 다른 request에 흡수될 때 통계 갱신
 *
 * @bfqg: 대상 bfq_group.
 * @opf: merge된 bio/request의 방향/플래그.
 * @return: 없음(void).
 *
 * merged 카운터를 1 늘리는 단순 디버그 통계 함수다. merge가 많을수록
 * 실제로 디바이스까지 내려가는 독립 명령 수가 줄어드는 효과를 보여준다.
 *
 * 호출 체인:
 *   bfq_bio_merge()/bfq_request_merged() [bfq-iosched.c] → [bfqg_stats_update_io_merged]
 */
void bfqg_stats_update_io_merged(struct bfq_group *bfqg, blk_opf_t opf)
{
	blkg_rwstat_add(&bfqg->stats.merged, opf, 1); /* [한국어] opf 방향의 merged 카운터를 1 증가 */
}

/*
 * [한국어]
 * bfqg_stats_update_completion - request 완료 시점의 wait_time/service_time 기록
 *
 * @bfqg: 완료된 request가 속했던 bfq_group.
 * @start_time_ns: request가 생성(할당)된 시각.
 * @io_start_time_ns: request가 실제로 디바이스에 issue된 시각.
 * @opf: request의 방향/플래그.
 * @return: 없음(void).
 *
 * 두 구간을 각각 계산해 누적한다:
 *  - service_time = now - io_start_time_ns : 디바이스가 실제로 처리하는 데
 *    걸린 시간(issue부터 완료까지).
 *  - wait_time = io_start_time_ns - start_time_ns : request가 생성된 뒤
 *    BFQ 스케줄러 큐에서 issue되기까지 기다린 시간.
 * 각 델타는 음수가 되지 않도록(now/io_start_time_ns가 더 커야만) 조건부로
 * 누적하여 시계 역전이나 잘못된 타임스탬프에 방어적이다.
 *
 * 호출 체인:
 *   blk_mq_end_request() 경로 → bfq 완료 콜백 → [bfqg_stats_update_completion]
 */
void bfqg_stats_update_completion(struct bfq_group *bfqg, u64 start_time_ns,
				  u64 io_start_time_ns, blk_opf_t opf)
{
	struct bfqg_stats *stats = &bfqg->stats; /* [한국어] 대상 통계 구조체 포인터 */
	u64 now = blk_time_get_ns(); /* [한국어] 완료 시각(ns) */

	/* [한국어] issue 시각보다 완료 시각이 커야(정상 흐름) service_time 계산 */
	if (now > io_start_time_ns)
		blkg_rwstat_add(&stats->service_time, opf,
				now - io_start_time_ns);
				/* [한국어] issue부터 완료까지의 디바이스 처리 시간을 누적 */
	if (io_start_time_ns > start_time_ns)
		/* [한국어] 생성 시각보다 issue 시각이 커야(정상 흐름) wait_time 계산 */
		blkg_rwstat_add(&stats->wait_time, opf,
				io_start_time_ns - start_time_ns);
				/* [한국어] 생성부터 issue까지 스케줄러 큐에서 대기한 시간을 누적 */
}

#else /* CONFIG_BFQ_CGROUP_DEBUG */
/* [한국어] CONFIG_BFQ_CGROUP_DEBUG가 꺼진 빌드에서는 위 상세 통계 함수들이
 * 존재하지 않으므로, 호출부(bfq-iosched.c 등)가 조건부 컴파일 없이 그대로
 * 호출할 수 있도록 동일한 이름의 빈 no-op 함수를 정의한다. 컴파일러가 이
 * 함수들을 인라인해 호출 비용을 사실상 0으로 만든다. */

/*
 * [한국어]
 * bfqg_stats_update_io_remove (no-op 버전) - 디버그 비활성 빌드 스텁
 * @bfqg: 사용되지 않음. @opf: 사용되지 않음. @return: 없음(void).
 * CONFIG_BFQ_CGROUP_DEBUG=n일 때 queued 통계 자체를 추적하지 않으므로 아무 일도 하지 않는다.
 */
void bfqg_stats_update_io_remove(struct bfq_group *bfqg, blk_opf_t opf) { } /* [한국어] 본문 없음 - 디버그 통계 미사용 */
/*
 * [한국어]
 * bfqg_stats_update_io_merged (no-op 버전) - 디버그 비활성 빌드 스텁
 * @bfqg: 사용되지 않음. @opf: 사용되지 않음. @return: 없음(void).
 */
void bfqg_stats_update_io_merged(struct bfq_group *bfqg, blk_opf_t opf) { } /* [한국어] 본문 없음 - merged 통계 미사용 */
/*
 * [한국어]
 * bfqg_stats_update_completion (no-op 버전) - 디버그 비활성 빌드 스텁
 * @bfqg/@start_time_ns/@io_start_time_ns/@opf: 사용되지 않음. @return: 없음(void).
 */
void bfqg_stats_update_completion(struct bfq_group *bfqg, u64 start_time_ns,
				  u64 io_start_time_ns, blk_opf_t opf) { } /* [한국어] 본문 없음 - wait/service time 미측정 */
/*
 * [한국어]
 * bfqg_stats_update_dequeue (no-op 버전) - 디버그 비활성 빌드 스텁
 * @bfqg: 사용되지 않음. @return: 없음(void).
 */
void bfqg_stats_update_dequeue(struct bfq_group *bfqg) { } /* [한국어] 본문 없음 - dequeue 카운트 미사용 */
/*
 * [한국어]
 * bfqg_stats_set_start_idle_time (no-op 버전) - 디버그 비활성 빌드 스텁
 * @bfqg: 사용되지 않음. @return: 없음(void).
 */
void bfqg_stats_set_start_idle_time(struct bfq_group *bfqg) { } /* [한국어] 본문 없음 - idle_time 미측정 */

#endif /* CONFIG_BFQ_CGROUP_DEBUG */

#ifdef CONFIG_BFQ_GROUP_IOSCHED

/*
 * blk-cgroup policy-related handlers
 * The following functions help in converting between blk-cgroup
 * internal structures and BFQ-specific structures.
 */
/* [한국어] 아래 pd_to_bfqg()/bfqg_to_blkg()/blkg_to_bfqg()는 blk-cgroup
 * 코어가 다루는 범용 자료구조(struct blkg_policy_data, struct blkcg_gq)와
 * BFQ 전용 자료구조(struct bfq_group)를 상호 변환하는 접근자들이다.
 * container_of()/offsetof 기반이라 런타임 비용이 거의 없다. */

/*
 * [한국어]
 * pd_to_bfqg - blkg_policy_data 포인터로부터 그것을 감싼 bfq_group을 역산
 *
 * @pd: struct bfq_group.pd 필드의 주소(NULL 가능).
 * @return: pd를 포함하는 struct bfq_group의 포인터, pd가 NULL이면 NULL.
 *
 * struct bfq_group의 첫 멤버가 pd이므로(bfq-iosched.h 참고)
 * container_of()로 단순 포인터 산술을 통해 역산한다. blk-cgroup 코어가
 * void* 대신 struct blkg_policy_data*로 전달하는 pd_alloc_fn 등 콜백
 * 인자를 BFQ 쪽 구체 타입으로 되돌리는 데 쓰인다.
 *
 * 호출 체인:
 *   bfq_pd_free/bfq_pd_reset_stats/bfqg_prfill_*() 등 → [pd_to_bfqg]
 */
static struct bfq_group *pd_to_bfqg(struct blkg_policy_data *pd)
{
	return pd ? container_of(pd, struct bfq_group, pd) : NULL;
	/* [한국어] pd가 NULL이 아니면 container_of로 bfq_group 시작 주소 계산, NULL이면 그대로 NULL 전파 */
}

/*
 * [한국어]
 * bfqg_to_blkg - bfq_group으로부터 그에 대응하는 blkcg_gq(blkg)를 얻음
 *
 * @bfqg: 변환할 bfq_group.
 * @return: bfqg->pd가 소속된 struct blkcg_gq 포인터.
 *
 * pd_to_blkg()(blk-cgroup 코어 인라인)에 &bfqg->pd를 넘겨 blkg를 얻는다.
 * bfq_group은 (디바이스, cgroup) 쌍마다 하나씩 존재하며, 그 짝이 되는
 * blkcg_gq를 통해 부모/자식 계층(blkg->parent)이나 소속 request_queue
 * (blkg->q)에 접근할 때 이 함수가 진입점이 된다.
 *
 * 호출 체인:
 *   bfqg_parent()/bfqg_and_blkg_get()/bfq_create_group_hierarchy() 등 → [bfqg_to_blkg]
 */
struct blkcg_gq *bfqg_to_blkg(struct bfq_group *bfqg)
{
	return pd_to_blkg(&bfqg->pd); /* [한국어] bfqg->pd의 주소를 blk-cgroup 코어 헬퍼에 넘겨 blkg를 얻음 */
}

/*
 * [한국어]
 * blkg_to_bfqg - blkcg_gq(blkg)로부터 BFQ 정책의 bfq_group을 얻음
 *
 * @blkg: 변환할 struct blkcg_gq.
 * @return: 해당 blkg에 붙은 BFQ 전용 policy data(bfq_group), 정책 미등록 시 NULL.
 *
 * blkg_to_pd()(blk-cgroup 코어)로 &blkcg_policy_bfq에 해당하는
 * blkg_policy_data를 얻은 뒤 pd_to_bfqg()로 구체 타입으로 변환한다.
 * bio->bi_blkg → bfq_group 변환의 마지막 단계로, cgroup 정책 결정 경로
 * 전체에서 가장 빈번히 쓰이는 변환 함수 중 하나다.
 *
 * 호출 체인:
 *   bfq_bio_bfqg()/__bfq_bic_change_cgroup() 등 → [blkg_to_bfqg] → pd_to_bfqg()
 */
static struct bfq_group *blkg_to_bfqg(struct blkcg_gq *blkg)
{
	return pd_to_bfqg(blkg_to_pd(blkg, &blkcg_policy_bfq));
	/* [한국어] blkg에서 blkcg_policy_bfq에 해당하는 policy_data를 얻고(blkg_to_pd),
	 * 그것을 다시 bfq_group으로 역산(pd_to_bfqg) */
}

/*
 * bfq_group handlers
 * The following functions help in navigating the bfq_group hierarchy
 * by allowing to find the parent of a bfq_group or the bfq_group
 * associated to a bfq_queue.
 */

/*
 * [한국어]
 * bfqg_parent - 주어진 bfq_group의 부모 bfq_group을 찾음
 *
 * @bfqg: 부모를 찾을 대상 bfq_group.
 * @return: 부모 bfq_group 포인터, @bfqg가 최상위(root)라 부모 blkg가 없으면 NULL.
 *
 * bfq_group 자신은 부모 포인터를 직접 갖지 않으므로, 대응하는
 * blkg->parent(cgroup 계층에서의 부모 blkcg_gq)를 따라간 뒤 다시
 * blkg_to_bfqg()로 변환하는 우회 경로를 쓴다. H-WF2Q+ 계층에서 이
 * bfq_group의 entity가 어느 sched_data에 편입되어야 하는지를 결정하는
 * bfq_link_bfqg()/bfq_group_set_parent()가 이 함수에 의존한다.
 *
 * 호출 체인:
 *   bfq_link_bfqg()/bfqg_stats_xfer_dead() → [bfqg_parent] → bfqg_to_blkg()/blkg_to_bfqg()
 */
static struct bfq_group *bfqg_parent(struct bfq_group *bfqg)
{
	struct blkcg_gq *pblkg = bfqg_to_blkg(bfqg)->parent;
	/* [한국어] bfqg를 blkg로 변환한 뒤 그 parent 필드(cgroup 계층상의 부모 blkg)를 읽음 */

	return pblkg ? blkg_to_bfqg(pblkg) : NULL;
	/* [한국어] 부모 blkg가 있으면 bfq_group으로 변환해 반환, 없으면(최상위) NULL 반환 -
	 * 호출자가 NULL을 받으면 보통 root_group으로 폴백 처리한다 */
}

/*
 * [한국어]
 * bfqq_group - bfq_queue가 소속된 bfq_group을 반환
 *
 * @bfqq: 대상 bfq_queue.
 * @return: bfqq가 속한 bfq_group. bfqq의 entity가 어떤 그룹에도 속하지
 *          않으면(parent==NULL) 디바이스의 root_group.
 *
 * bfq_queue의 entity.parent는 이 큐가 편입된 "부모 entity"인데, 그
 * 부모가 다름 아닌 bfq_group.entity이므로 container_of로 역산할 수
 * 있다. entity.parent가 NULL이라는 것은 이 큐가 최상위(root_group)에
 * 직접 붙어 있다는 뜻이다.
 *
 * 호출 체인:
 *   bfqg_stats_update_io_add()/bfq_bfqq_move()/여러 스케줄링 함수
 *     → [bfqq_group] → container_of
 */
struct bfq_group *bfqq_group(struct bfq_queue *bfqq)
{
	struct bfq_entity *group_entity = bfqq->entity.parent;
	/* [한국어] bfqq의 entity가 편입된 부모 entity - NULL이면 root_group 소속 */

	return group_entity ? container_of(group_entity, struct bfq_group,
					   entity) :
			      bfqq->bfqd->root_group;
	/* [한국어] 부모 entity가 있으면 그것을 포함하는 bfq_group을 역산해 반환,
	 * 없으면 bfqd->root_group(디바이스의 최상위 그룹)을 반환 */
}

/*
 * The following two functions handle get and put of a bfq_group by
 * wrapping the related blk-cgroup hooks.
 */

/*
 * [한국어]
 * bfqg_get - bfq_group 자체의 참조 카운트(ref) 증가
 *
 * @bfqg: 참조를 얻을 bfq_group.
 * @return: 없음(void).
 *
 * struct bfq_group.ref(refcount_t)를 1 증가시킨다. bfq_group은
 * blkcg_gq(blkg)와 별개의 자체 refcount를 갖는데, bic(bfq_io_cq)가
 * cgroup 이전 도중에도 이전 그룹을 붙잡고 있어야 하는 경우가 있어서다
 * (bfq_bic_update_cgroup 주석 참고). bfqd->lock을 쥔 상태에서 호출된다.
 *
 * 호출 체인:
 *   bfqg_and_blkg_get() → [bfqg_get] → refcount_inc()
 */
static void bfqg_get(struct bfq_group *bfqg)
{
	refcount_inc(&bfqg->ref); /* [한국어] 원자적으로 ref 카운트를 1 증가 */
}

/*
 * [한국어]
 * bfqg_put - bfq_group 참조 카운트 감소, 0이 되면 메모리 해제
 *
 * @bfqg: 참조를 반환할 bfq_group.
 * @return: 없음(void).
 *
 * refcount_dec_and_test()로 ref를 원자적으로 감소시키고, 그 결과 0이
 * 되었으면(이 bfq_group을 참조하는 곳이 더 이상 없으면) kfree()로
 * 메모리를 해제한다. 이 시점에는 in-flight request나 통계 콜백이 이미
 * 이 그룹을 참조하지 않는다고 보장되어야 한다(호출자가 이를 보장).
 *
 * 호출 체인:
 *   bfqg_and_blkg_put() → [bfqg_put] → refcount_dec_and_test()/kfree()
 */
static void bfqg_put(struct bfq_group *bfqg)
{
	if (refcount_dec_and_test(&bfqg->ref))
		/* [한국어] 감소 후 값이 0이 되었을 때만(마지막 참조였을 때만) true */
		kfree(bfqg); /* [한국어] 마지막 참조가 사라졌으므로 bfq_group 메모리를 즉시 해제 */
}

/*
 * [한국어]
 * bfqg_and_blkg_get - bfq_group과 그에 대응하는 blkcg_gq(blkg) 참조를 함께 획득
 *
 * @bfqg: 대상 bfq_group.
 * @return: 없음(void).
 *
 * bfq_group 자체의 refcount(bfqg_get)와, 그 짝이 되는 blkg의
 * percpu_ref(blkg_get)를 모두 증가시킨다. 어떤 bfq_queue의 entity가 이
 * bfq_group에 편입되어 있는 동안에는 bfq_group과 blkg 둘 다 살아있어야
 * 하므로 항상 쌍으로 획득/반환한다(bfqg_and_blkg_put과 짝).
 *
 * 호출 체인:
 *   bfq_init_entity()/bfq_bfqq_move() → [bfqg_and_blkg_get] → bfqg_get()/blkg_get()
 */
static void bfqg_and_blkg_get(struct bfq_group *bfqg)
{
	/* see comments in bfq_bic_update_cgroup for why refcounting bfqg */
	bfqg_get(bfqg); /* [한국어] bfq_group 자체 refcount 증가 */

	blkg_get(bfqg_to_blkg(bfqg)); /* [한국어] 대응하는 blkg의 percpu_ref도 함께 증가 - 두 객체의 수명을 동기화 */
}

/*
 * [한국어]
 * bfqg_and_blkg_put - bfqg_and_blkg_get()으로 얻은 참조 두 개를 함께 반환
 *
 * @bfqg: 대상 bfq_group.
 * @return: 없음(void).
 *
 * blkg_put()과 bfqg_put()을 이 순서로 호출해 참조를 반환한다. blkg를
 * 먼저 놓고 bfqg를 나중에 놓는 순서는, bfqg_put()이 실제로 메모리를
 * 해제할 수 있는 마지막 단계이므로 그 전에 다른 자원(blkg) 정리를
 * 끝내 두기 위함이다.
 *
 * 호출 체인:
 *   bfq_bfqq_move()/bfq_release_process_ref() 등 → [bfqg_and_blkg_put]
 *     → blkg_put() → bfqg_put()
 */
void bfqg_and_blkg_put(struct bfq_group *bfqg)
{
	blkg_put(bfqg_to_blkg(bfqg)); /* [한국어] blkg의 percpu_ref 감소 - 0이면 blkg 자체 해제 절차 진행 */

	bfqg_put(bfqg); /* [한국어] bfq_group 자체 refcount 감소 - 0이면 kfree */
}

/*
 * [한국어]
 * bfqg_stats_update_legacy_io - request 완료 시 bytes/ios 기본 통계 갱신
 *
 * @q: request가 속했던 request_queue(이 함수 자체에서는 직접 쓰이지 않고
 *     시그니처 호환을 위해 유지됨).
 * @rq: 완료된 struct request.
 * @return: 없음(void).
 *
 * rq->bio->bi_blkg(이 request가 속한 cgroup 링크)로부터 bfq_group을
 * 찾아 bytes(바이트 수)와 ios(개수) 통계를 방향별로 누적한다. bytes/ios는
 * CONFIG_BFQ_CGROUP_DEBUG와 무관하게 항상 존재하는 "기본" 통계이며,
 * cgroupfs의 io.stat(bfq.io_service_bytes 등)로 노출된다. bfqg가
 * 없다면(bi_blkg이 아직 설정되지 않았거나 정책 비활성 등) 그냥 반환한다.
 *
 * 호출 체인:
 *   blk_mq_end_request() 완료 경로 → [bfqg_stats_update_legacy_io]
 */
void bfqg_stats_update_legacy_io(struct request_queue *q, struct request *rq)
{
	struct bfq_group *bfqg = blkg_to_bfqg(rq->bio->bi_blkg);
	/* [한국어] request의 bio에 연결된 blkg를 bfq_group으로 변환 */

	if (!bfqg)
		return; /* [한국어] 대응하는 bfq_group이 없으면(정책 비활성 등) 통계 기록 없이 반환 */

	blkg_rwstat_add(&bfqg->stats.bytes, rq->cmd_flags, blk_rq_bytes(rq));
	/* [한국어] rq->cmd_flags로 방향(read/write/discard 등)을 판별해 처리 바이트 수를 누적 */
	blkg_rwstat_add(&bfqg->stats.ios, rq->cmd_flags, 1);
	/* [한국어] 같은 방향의 완료 개수(IOPS 산출용)를 1 증가 */
}

/* @stats = 0 */
/*
 * [한국어]
 * bfqg_stats_reset - bfqg_stats의 누적 카운터들을 0으로 초기화
 *
 * @stats: 초기화할 struct bfqg_stats.
 * @return: 없음(void).
 *
 * 주석("queued stats shouldn't be cleared")대로 queued(현재 큐잉 개수)와
 * bytes/ios(기본 통계, CONFIG_BFQ_CGROUP_DEBUG 밖에 있음)는 "누적 이력"이
 * 아니라 "현재 상태/총량"에 가까워 이 함수의 리셋 대상에서 제외된다.
 * CONFIG_BFQ_CGROUP_DEBUG가 꺼지면 리셋할 디버그 통계 자체가 없으므로
 * 함수 본문이 비게 된다.
 *
 * 호출 체인:
 *   bfq_pd_reset_stats()/bfqg_stats_xfer_dead() → [bfqg_stats_reset]
 */
static void bfqg_stats_reset(struct bfqg_stats *stats)
{
#ifdef CONFIG_BFQ_CGROUP_DEBUG
	/* queued stats shouldn't be cleared */
	blkg_rwstat_reset(&stats->merged); /* [한국어] merge된 IO 개수 리셋 */
	blkg_rwstat_reset(&stats->service_time); /* [한국어] 디바이스 서비스 시간 리셋 */
	blkg_rwstat_reset(&stats->wait_time); /* [한국어] 스케줄러 대기 시간 리셋 */
	bfq_stat_reset(&stats->time); /* [한국어] 총 디스패치 시간/섹터 통계 리셋 */
	bfq_stat_reset(&stats->avg_queue_size_sum); /* [한국어] 평균 큐 크기 계산용 합계 리셋 */
	bfq_stat_reset(&stats->avg_queue_size_samples); /* [한국어] 평균 큐 크기 계산용 표본 수 리셋 */
	bfq_stat_reset(&stats->dequeue); /* [한국어] service tree dequeue 횟수 리셋 */
	bfq_stat_reset(&stats->group_wait_time); /* [한국어] 그룹 서비스 대기 시간 리셋 */
	bfq_stat_reset(&stats->idle_time); /* [한국어] device idling 시간 리셋 */
	bfq_stat_reset(&stats->empty_time); /* [한국어] 그룹 empty 시간 리셋 */
#endif
}

/* @to += @from */
/*
 * [한국어]
 * bfqg_stats_add_aux - @from의 모든 디버그 통계를 @to의 aux 카운터에 누적
 *
 * @to: 누적받을 목적지 bfqg_stats(대개 부모 그룹의 stats).
 * @from: 누적할 원본 bfqg_stats(대개 사라지는 자식 그룹의 stats).
 * @return: 없음(void).
 *
 * to 또는 from이 NULL이면(예: root_group처럼 상위가 없거나 아직 stats가
 * 초기화되지 않은 경우) 아무 것도 하지 않는다. CONFIG_BFQ_CGROUP_DEBUG가
 * 켜진 경우에만 merged부터 empty_time까지 각 필드를 blkg_rwstat_add_aux()
 * 또는 bfq_stat_add_aux()로 이관한다(bytes/ios/queued는 이관 대상이 아님 -
 * queued는 "현재 상태"라 자식이 사라지면 자연히 0이 되어야 하고, bytes/ios는
 * blkg_rwstat_recursive_sum()이 계층을 직접 순회해 별도 aux 이관 없이도
 * 조상 통계에 반영되기 때문이다).
 *
 * 호출 체인:
 *   bfqg_stats_xfer_dead() → [bfqg_stats_add_aux]
 */
static void bfqg_stats_add_aux(struct bfqg_stats *to, struct bfqg_stats *from)
{
	if (!to || !from)
		return; /* [한국어] 둘 중 하나라도 유효하지 않으면 이관 불가능하므로 즉시 반환 */

#ifdef CONFIG_BFQ_CGROUP_DEBUG
	/* queued stats shouldn't be cleared */
	blkg_rwstat_add_aux(&to->merged, &from->merged); /* [한국어] merged 통계 이관 */
	blkg_rwstat_add_aux(&to->service_time, &from->service_time); /* [한국어] service_time 이관 */
	blkg_rwstat_add_aux(&to->wait_time, &from->wait_time); /* [한국어] wait_time 이관 */
	bfq_stat_add_aux(&to->time, &from->time); /* [한국어] time(총 디스패치 시간/섹터) 이관 */
	bfq_stat_add_aux(&to->avg_queue_size_sum, &from->avg_queue_size_sum); /* [한국어] 평균 큐 크기 합계 이관 */
	/* [한국어] 평균 큐 크기 표본 수 이관 */
	bfq_stat_add_aux(&to->avg_queue_size_samples,
			  &from->avg_queue_size_samples);
	bfq_stat_add_aux(&to->dequeue, &from->dequeue); /* [한국어] dequeue 횟수 이관 */
	bfq_stat_add_aux(&to->group_wait_time, &from->group_wait_time); /* [한국어] group_wait_time 이관 */
	bfq_stat_add_aux(&to->idle_time, &from->idle_time); /* [한국어] idle_time 이관 */
	bfq_stat_add_aux(&to->empty_time, &from->empty_time); /* [한국어] empty_time 이관 */
#endif
}

/*
 * Transfer @bfqg's stats to its parent's aux counts so that the ancestors'
 * recursive stats can still account for the amount used by this bfqg after
 * it's gone.
 */
/*
 * [한국어]
 * bfqg_stats_xfer_dead - offline되는 bfq_group의 통계를 부모의 aux로 이관
 *
 * @bfqg: 곧 사라질(offline 처리 중인) bfq_group.
 * @return: 없음(void).
 *
 * cgroup이 삭제(rmdir)되어 이 bfq_group이 offline되더라도, 그동안 이
 * 그룹이 처리한 통계가 조상 cgroup의 "재귀(recursive) 통계"에서 사라지면
 * 안 되므로, bfqg_stats_add_aux()로 부모의 aux 카운터에 합산한 뒤 자신의
 * 통계는 리셋한다. bfqg가 NULL이면(즉 애초에 root_group 처리 경로 등에서
 * 그룹 자체가 없으면) 이관할 대상이 없으므로 반환한다. 부모가 없으면
 * (unlikely, 최상위 root_group 자신이 이 함수에 들어온 경우) 마찬가지로
 * 이관 없이 반환한다.
 * queue_lock을 쥔 상태에서 호출되어야 하며(lockdep_assert_held), 이는
 * 이관 도중 다른 CPU의 완료 콜백이 같은 bfqg->stats를 동시에 갱신해
 * 값이 유실되는 것을 막기 위함이다.
 *
 * 호출 체인:
 *   bfq_pd_offline() → [bfqg_stats_xfer_dead] → bfqg_parent()/bfqg_stats_add_aux()/bfqg_stats_reset()
 */
static void bfqg_stats_xfer_dead(struct bfq_group *bfqg)
{
	struct bfq_group *parent; /* [한국어] 통계를 이관받을 부모 bfq_group */

	/* [한국어] bfqg 자체가 없으면(원본 주석: root_group의 경우) 이관 불필요 */
	if (!bfqg) /* root_group */
		return;

	parent = bfqg_parent(bfqg); /* [한국어] 계층상의 부모 bfq_group 탐색 */

	/* [한국어] 이 함수가 반드시 queue_lock을 쥔 상태에서 호출되어야 함을 런타임에
	 * 검증(lockdep) - 이관 도중 통계 갱신과의 경쟁을 방지하기 위한 전제조건 */
	lockdep_assert_held(&bfqg_to_blkg(bfqg)->q->queue_lock);

	/* [한국어] 부모가 없으면(이 bfqg 자신이 최상위) 이관할 곳이 없으므로 반환 */
	if (unlikely(!parent))
		return;

	/* [한국어] 자식(bfqg)의 통계를 부모(parent)의 aux 카운터에 합산 */
	bfqg_stats_add_aux(&parent->stats, &bfqg->stats);
	/* [한국어] 이관을 마쳤으므로 자식 자신의 통계는 0으로 리셋(중복 집계 방지) */
	bfqg_stats_reset(&bfqg->stats);
}

/*
 * [한국어]
 * bfq_init_entity - bfq_entity를 특정 bfq_group에 연결하고 초기 weight/ioprio 반영
 *
 * @entity: 초기화할 struct bfq_entity(bfq_queue.entity 또는 bfq_group.entity).
 * @bfqg: 이 entity가 소속될 bfq_group.
 * @return: 없음(void).
 *
 * entity->new_weight(가장 최근에 설정된 목표 weight)를 실제 사용되는
 * weight/orig_weight에 반영한다. entity가 leaf(즉 bfq_queue에 속한
 * entity)라면 ioprio/ioprio_class도 new_* 값으로 확정하고, bfqg와 그
 * blkg가 이 entity보다 먼저 사라지지 않도록 참조를 추가로 획득한다(그룹
 * entity 자신은 이미 grouping 코드가 생명주기를 관리하므로 제외). 마지막에
 * entity->parent와 sched_data를 @bfqg 기준으로 설정해, 이후 이 entity가
 * bfqg->sched_data의 service_tree에 편입될 수 있게 한다.
 * bfqd->lock을 쥔 상태에서 호출된다(entity/스케줄러 상태 변경이므로).
 *
 * 호출 체인:
 *   bfq_get_queue()/bfq_create_group_hierarchy() 등 → [bfq_init_entity]
 *     → bfqg_and_blkg_get()
 */
void bfq_init_entity(struct bfq_entity *entity, struct bfq_group *bfqg)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);
	/* [한국어] entity가 leaf(bfq_queue 소속)라면 그 bfq_queue를, group entity라면 NULL을 반환 */

	entity->weight = entity->new_weight;
	/* [한국어] 실제 스케줄링에 쓰이는 weight를 최신 목표값(new_weight)으로 확정 */
	entity->orig_weight = entity->new_weight;
	/* [한국어] weight-raising 등으로 임시 조정되기 전의 "원래 weight"도 함께 갱신 */
	if (bfqq) {
		/* [한국어] leaf entity(bfq_queue)인 경우에만 ioprio 확정 및 참조 획득 수행 */
		bfqq->ioprio = bfqq->new_ioprio;
		/* [한국어] 사용 중인 ioprio를 최신 목표값으로 확정 */
		bfqq->ioprio_class = bfqq->new_ioprio_class;
		/* [한국어] ioprio 클래스(RT/BE/IDLE)도 최신 목표값으로 확정 */
		/*
		 * Make sure that bfqg and its associated blkg do not
		 * disappear before entity.
		 */
		bfqg_and_blkg_get(bfqg);
		/* [한국어] 이 bfqq의 entity가 bfqg에 편입되어 있는 동안 bfqg/blkg가
		 * 먼저 해제되지 않도록 참조 획득(entity 자체는 bfqq가 소유) */
	}
	entity->parent = bfqg->my_entity; /* NULL for root group */
	/* [한국어] 부모 entity를 bfqg의 대표 entity로 설정 - bfqg가 root_group이면
	 * my_entity가 NULL이라 entity->parent도 NULL(최상위 직속)이 됨 */
	entity->sched_data = &bfqg->sched_data;
	/* [한국어] 이후 이 entity가 bfqg의 service_tree[]에서 스케줄링되도록 연결 */
}

/*
 * [한국어]
 * bfqg_stats_exit - bfqg_stats가 보유한 모든 percpu/rwstat 자료구조 해제
 *
 * @stats: 해제할 struct bfqg_stats.
 * @return: 없음(void).
 *
 * bfqg_stats_init()으로 할당된 blkg_rwstat/bfq_stat 필드들을 역순으로
 * 정리한다. bytes/ios는 항상 존재하므로 무조건 해제하고, 나머지는
 * CONFIG_BFQ_CGROUP_DEBUG가 켜진 빌드에서만 해제 대상이 된다. 이 함수는
 * bfq_pd_free()에서 호출되므로, 이 시점에는 이미 이 bfq_group을 참조하는
 * in-flight request나 통계 갱신 호출이 없어야 안전하다.
 *
 * 호출 체인:
 *   bfq_pd_free()/bfqg_stats_init()(에러 경로) → [bfqg_stats_exit]
 */
static void bfqg_stats_exit(struct bfqg_stats *stats)
{
	blkg_rwstat_exit(&stats->bytes); /* [한국어] bytes(방향별 누적 바이트) 통계 자료구조 해제 */
	blkg_rwstat_exit(&stats->ios); /* [한국어] ios(방향별 완료 개수) 통계 자료구조 해제 */
#ifdef CONFIG_BFQ_CGROUP_DEBUG
	blkg_rwstat_exit(&stats->merged); /* [한국어] merged 통계 해제 */
	blkg_rwstat_exit(&stats->service_time); /* [한국어] service_time 통계 해제 */
	blkg_rwstat_exit(&stats->wait_time); /* [한국어] wait_time 통계 해제 */
	blkg_rwstat_exit(&stats->queued); /* [한국어] queued 통계 해제 */
	bfq_stat_exit(&stats->time); /* [한국어] time(percpu_counter 기반) 통계 해제 */
	bfq_stat_exit(&stats->avg_queue_size_sum); /* [한국어] 평균 큐 크기 합계 통계 해제 */
	bfq_stat_exit(&stats->avg_queue_size_samples); /* [한국어] 평균 큐 크기 표본 수 통계 해제 */
	bfq_stat_exit(&stats->dequeue); /* [한국어] dequeue 횟수 통계 해제 */
	bfq_stat_exit(&stats->group_wait_time); /* [한국어] group_wait_time 통계 해제 */
	bfq_stat_exit(&stats->idle_time); /* [한국어] idle_time 통계 해제 */
	bfq_stat_exit(&stats->empty_time); /* [한국어] empty_time 통계 해제 */
#endif
}

/*
 * [한국어]
 * bfqg_stats_init - bfqg_stats의 모든 percpu/rwstat 자료구조를 할당·초기화
 *
 * @stats: 초기화할 struct bfqg_stats(대개 새로 할당된 bfq_group.stats).
 * @gfp: 각 하위 통계 자료구조 할당에 쓸 GFP 플래그.
 * @return: 0=성공, -ENOMEM=하나라도 할당 실패 시.
 *
 * bytes/ios는 항상 초기화하고(디버그 여부 무관), CONFIG_BFQ_CGROUP_DEBUG가
 * 켜지면 merged/service_time/wait_time/queued(blkg_rwstat)와
 * time, avg_queue_size_sum, avg_queue_size_samples, dequeue,
 * group_wait_time, idle_time, empty_time
 * (bfq_stat, 즉 percpu_counter 기반)까지 추가로 초기화한다. 어느 하나라도
 * 실패하면 error 레이블로 점프해 이미 성공적으로 초기화된 필드들을
 * bfqg_stats_exit()로 롤백한 뒤 -ENOMEM을 반환한다(부분 초기화 상태로
 * 남기지 않기 위함).
 *
 * 호출 체인:
 *   bfq_pd_alloc() → [bfqg_stats_init] → blkg_rwstat_init()/bfq_stat_init()
 */
static int bfqg_stats_init(struct bfqg_stats *stats, gfp_t gfp)
{
	if (blkg_rwstat_init(&stats->bytes, gfp) ||
	    /* [한국어] bytes 또는 ios 중 하나라도 초기화 실패 시 error 레이블로 이동 */
	    blkg_rwstat_init(&stats->ios, gfp))
		goto error;

#ifdef CONFIG_BFQ_CGROUP_DEBUG
	/* [한국어] 디버그 통계 필드들 중 하나라도 초기화 실패 시 error 레이블로 이동 -
	 * ||의 단락 평가(short-circuit) 덕분에 실패 지점 이후 필드는 아예 시도되지 않음 */
	/* [한국어] 아래 디버그 통계는 CONFIG_BFQ_CGROUP_DEBUG일 때만 존재한다.
	 * 하나라도 실패하면 error로 빠져 이미 초기화한 것까지 모두 되돌린다 -
	 * percpu 할당이라 부분 초기화 상태로 두면 해제 경로에서 터진다. */
	if (blkg_rwstat_init(&stats->merged, gfp) ||
	    blkg_rwstat_init(&stats->service_time, gfp) ||
	    blkg_rwstat_init(&stats->wait_time, gfp) ||
	    blkg_rwstat_init(&stats->queued, gfp) ||
	    bfq_stat_init(&stats->time, gfp) ||
	    bfq_stat_init(&stats->avg_queue_size_sum, gfp) ||
	    bfq_stat_init(&stats->avg_queue_size_samples, gfp) ||
	    bfq_stat_init(&stats->dequeue, gfp) ||
	    bfq_stat_init(&stats->group_wait_time, gfp) ||
	    bfq_stat_init(&stats->idle_time, gfp) ||
	    bfq_stat_init(&stats->empty_time, gfp))
		goto error;
#endif

	return 0; /* [한국어] 모든 필드 초기화 성공 */

error:
	/* [한국어] 지금까지 성공적으로 초기화된 필드들을 포함해 안전하게 해제(각 exit
	 * 함수는 초기화되지 않은 필드에 대해서도 안전하도록 설계된 blk-cgroup 관례를 따름) */
	bfqg_stats_exit(stats);
	return -ENOMEM; /* [한국어] 메모리 부족으로 통계 구조체 준비 실패를 호출자에게 알림 */
}

/*
 * [한국어]
 * cpd_to_bfqgd - blkcg_policy_data 포인터로부터 bfq_group_data를 역산
 *
 * @cpd: struct bfq_group_data.pd 필드의 주소(NULL 가능).
 * @return: cpd를 포함하는 struct bfq_group_data 포인터, cpd가 NULL이면 NULL.
 *
 * pd_to_bfqg()와 대칭되는 cpd(per-blkcg, "cgroup policy data") 버전이다.
 * struct bfq_group_data의 첫 멤버가 pd이므로 container_of로 역산 가능하다.
 *
 * 호출 체인:
 *   blkcg_to_bfqgd()/bfq_cpd_free() → [cpd_to_bfqgd]
 */
static struct bfq_group_data *cpd_to_bfqgd(struct blkcg_policy_data *cpd)
{
	return cpd ? container_of(cpd, struct bfq_group_data, pd) : NULL;
	/* [한국어] cpd가 유효하면 container_of로 bfq_group_data 시작 주소 계산, 아니면 NULL 전파 */
}

/*
 * [한국어]
 * blkcg_to_bfqgd - blkcg로부터 이 cgroup의 BFQ per-cgroup 설정(weight 등)을 얻음
 *
 * @blkcg: 대상 struct blkcg.
 * @return: 해당 blkcg에 연결된 bfq_group_data, BFQ 정책 미등록 시 NULL.
 *
 * blkcg_to_cpd()(blk-cgroup 코어)로 &blkcg_policy_bfq에 해당하는
 * blkcg_policy_data를 얻은 뒤 cpd_to_bfqgd()로 구체 타입으로 변환한다.
 * cgroupfs write 핸들러(bfq_io_set_weight_legacy 등)가 사용자가 설정한
 * weight를 저장할 위치를 찾는 첫 단계다.
 *
 * 호출 체인:
 *   bfq_pd_init()/bfq_io_show_weight[_legacy]()/bfq_io_set_weight_legacy() 등
 *     → [blkcg_to_bfqgd] → cpd_to_bfqgd()
 */
static struct bfq_group_data *blkcg_to_bfqgd(struct blkcg *blkcg)
{
	return cpd_to_bfqgd(blkcg_to_cpd(blkcg, &blkcg_policy_bfq));
	/* [한국어] blkcg에서 blkcg_policy_bfq에 해당하는 policy_data를 얻고(blkcg_to_cpd),
	 * 그것을 다시 bfq_group_data로 역산(cpd_to_bfqgd) */
}

/*
 * [한국어]
 * bfq_cpd_alloc - cgroup 생성 시 bfq_group_data(per-cgroup weight 저장소)를 할당
 *
 * @gfp: 메모리 할당 플래그(cgroup 생성 컨텍스트에서 전달됨).
 * @return: 새로 할당된 struct bfq_group_data.pd 주소, 메모리 부족 시 NULL.
 *
 * blkcg_policy_bfq.cpd_alloc_fn으로 등록되어, 새 cgroup이 생성될 때마다
 * blk-cgroup 코어(blkcg_css_alloc)가 각 등록된 정책에 대해 호출한다.
 * 할당된 구조체의 weight를 CGROUP_WEIGHT_DFL(cgroup 서브시스템 공통
 * 기본값)으로 초기화해, 사용자가 아직 io.bfq.weight를 쓰지 않은 cgroup도
 * 합리적인 기본 서비스 비율을 갖게 한다.
 *
 * 호출 체인:
 *   blkcg_css_alloc() [blk-cgroup.c] → blkcg_policy_bfq.cpd_alloc_fn → [bfq_cpd_alloc]
 */
static struct blkcg_policy_data *bfq_cpd_alloc(gfp_t gfp)
{
	struct bfq_group_data *bgd; /* [한국어] 새로 할당할 bfq_group_data 포인터 */

	/* [한국어] *bgd 크기만큼 0으로 채워 할당(kzalloc_obj는 sizeof(*bgd)를 자동 계산하는 헬퍼) */
	bgd = kzalloc_obj(*bgd, gfp);
	/* [한국어] 메모리 부족 시 cgroup 생성 자체가 실패하도록 NULL 반환 */
	if (!bgd)
		return NULL;

	/* [한국어] 사용자 설정 전 기본 weight - cgroup v2 공통 기본값(보통 100)으로 초기화 */
	bgd->weight = CGROUP_WEIGHT_DFL;
	return &bgd->pd; /* [한국어] blk-cgroup 코어가 기대하는 blkcg_policy_data* 타입으로 반환 */
}

/*
 * [한국어]
 * bfq_cpd_free - cgroup 삭제 시 bfq_group_data 메모리 해제
 *
 * @cpd: 해제할 blkcg_policy_data(bfq_group_data.pd).
 * @return: 없음(void).
 *
 * blkcg_policy_bfq.cpd_free_fn으로 등록되어 cgroup이 완전히 해제될 때
 * blk-cgroup 코어가 호출한다. bfq_group_data는 내부에 별도로 해제할
 * 동적 자원(포인터 등)이 없으므로 단순 kfree로 충분하다.
 *
 * 호출 체인:
 *   blkcg_css_free() [blk-cgroup.c] → blkcg_policy_bfq.cpd_free_fn → [bfq_cpd_free]
 */
static void bfq_cpd_free(struct blkcg_policy_data *cpd)
{
	kfree(cpd_to_bfqgd(cpd)); /* [한국어] cpd를 bfq_group_data로 역산한 뒤 메모리 해제 */
}

/*
 * [한국어]
 * bfq_pd_alloc - (디바이스, cgroup) 쌍을 대표할 bfq_group 메모리 할당
 *
 * @disk: 이 bfq_group이 속할 gendisk(디바이스).
 * @blkcg: 이 bfq_group이 속할 cgroup(초기화는 bfq_pd_init에서 수행되므로
 *         이 함수에서는 직접 쓰이지 않음).
 * @gfp: 메모리 할당 플래그.
 * @return: 새로 할당된 struct bfq_group.pd 주소, 메모리 부족 시 NULL.
 *
 * blkcg_policy_bfq.pd_alloc_fn으로 등록되어, (디바이스, cgroup) 조합이
 * 처음 필요해질 때(blkg_alloc) blk-cgroup 코어가 호출한다. 할당만 담당하고
 * 스케줄링 관련 필드(entity/weight 등)의 초기화는 뒤이어 호출되는
 * bfq_pd_init()이 맡는다(이 시점에는 아직 pd->blkg 등이 완전히 연결되지
 * 않아 상위 정보에 의존하는 초기화를 할 수 없기 때문). 통계 자료구조
 * (bfqg->stats)는 disk 정보와 무관하므로 이 단계에서 함께 초기화한다.
 *
 * 호출 체인:
 *   blkg_alloc() [blk-cgroup.c] → blkcg_policy_bfq.pd_alloc_fn → [bfq_pd_alloc]
 *     → bfqg_stats_init()
 */
static struct blkg_policy_data *bfq_pd_alloc(struct gendisk *disk,
		struct blkcg *blkcg, gfp_t gfp)
{
	struct bfq_group *bfqg; /* [한국어] 새로 할당할 bfq_group 포인터 */

	/* [한국어] disk->node_id(디바이스가 선호하는 NUMA 노드)에 맞춰 0으로 채워 할당 -
	 * 이후 이 그룹에 접근하는 스케줄링 코드의 메모리 지역성을 높이기 위함 */
	bfqg = kzalloc_node(sizeof(*bfqg), gfp, disk->node_id);
	/* [한국어] 메모리 부족 시 blkg 생성 자체가 실패하도록 NULL 반환 */
	if (!bfqg)
		return NULL;

	/* [한국어] 통계 자료구조(percpu_counter 등) 할당이 실패한 경우 */
	if (bfqg_stats_init(&bfqg->stats, gfp)) {
		kfree(bfqg); /* [한국어] 이미 할당한 bfqg 본체도 함께 롤백 */
		return NULL; /* [한국어] 상위 blkg_alloc()이 실패로 처리하도록 NULL 반환 */
	}

	/* see comments in bfq_bic_update_cgroup for why refcounting */
	/* [한국어] 참조 카운트를 1로 초기화 - 이 할당 자체가 첫 번째(생성자) 참조 */
	refcount_set(&bfqg->ref, 1);
	return &bfqg->pd; /* [한국어] blk-cgroup 코어가 기대하는 blkg_policy_data* 타입으로 반환 */
}

/*
 * [한국어]
 * bfq_pd_init - bfq_group의 스케줄링 관련 필드를 실제 값으로 초기화
 *
 * @pd: bfq_pd_alloc()이 반환한 struct blkg_policy_data(bfq_group.pd).
 * @return: 없음(void).
 *
 * bfq_pd_alloc() 직후 blk-cgroup 코어가 pd->blkg 등 상위 연결을 완료한
 * 뒤 호출하는 두 번째 초기화 단계다. 이 시점에는 pd_to_blkg()로 blkg를,
 * blkg->q->elevator->elevator_data로 디바이스 전역 bfq_data를 안전하게
 * 얻을 수 있다. entity의 weight를 이 cgroup에 설정된 값(d->weight)으로
 * 맞추고, 그룹 자신의 서비스 트리(sched_data)를 자식 entity들이 참조할
 * my_sched_data로 등록한다. rq_pos_tree 등 그룹 전용 자료구조도 빈
 * 상태로 초기화한다.
 *
 * 호출 체인:
 *   blkg_create()/blkg_alloc() 이후 [blk-cgroup.c] → blkcg_policy_bfq.pd_init_fn
 *     → [bfq_pd_init]
 */
static void bfq_pd_init(struct blkg_policy_data *pd)
{
	struct blkcg_gq *blkg = pd_to_blkg(pd);
	/* [한국어] pd가 속한 blkcg_gq - blkg->q(request_queue)/blkg->blkcg(cgroup) 접근용 */
	struct bfq_group *bfqg = blkg_to_bfqg(blkg);
	/* [한국어] 초기화 대상 bfq_group 자신(pd를 포함하는 구조체) */
	struct bfq_data *bfqd = blkg->q->elevator->elevator_data;
	/* [한국어] 이 디바이스의 elevator private data = 디바이스 전역 struct bfq_data */
	struct bfq_entity *entity = &bfqg->entity;
	/* [한국어] 이 그룹을 상위 그룹의 B-WF2Q+ 트리에 등록할 entity */
	struct bfq_group_data *d = blkcg_to_bfqgd(blkg->blkcg);
	/* [한국어] 이 cgroup에 설정된 weight 등을 담은 per-cgroup 데이터 */

	entity->orig_weight = entity->weight = entity->new_weight = d->weight;
	/* [한국어] weight/orig_weight/new_weight 세 필드를 모두 cgroup 설정값(d->weight)으로
	 * 통일 - 아직 weight-raising 등 임시 조정이 적용되지 않은 최초 상태이므로 셋을 같게 둠 */
	entity->my_sched_data = &bfqg->sched_data;
	/* [한국어] "이 entity(그룹)를 통해 접근 가능한 자식 스케줄러"를 자기 자신의
	 * sched_data로 설정 - 그룹이 스케줄링 대상(entity)이자 스케줄러(sched_data)라는
	 * 이중 역할을 완성 */
	entity->last_bfqq_created = NULL;
	/* [한국어] 최근 생성된 bfqq 캐시(협력 큐 탐색 최적화용)를 아직 없음으로 초기화 */

	bfqg->my_entity = entity; /*
				   * the root_group's will be set to NULL
				   * in bfq_init_queue()
				   */
	/* [한국어] 자기 참조 설정 - root_group은 이후 bfq_init_queue()에서 별도로
	 * NULL로 재설정되어 "내가 최상위"임을 표시하게 됨(원본 주석) */
	bfqg->bfqd = bfqd;
	/* [한국어] 이 그룹이 속한 디바이스의 전역 상태 포인터 저장 */
	bfqg->active_entities = 0;
	/* [한국어] 아직 활성(backlogged) 자식 entity가 없는 초기 상태 */
	bfqg->num_queues_with_pending_reqs = 0;
	/* [한국어] 아직 pending request를 가진 bfq_queue가 없는 초기 상태 */
	bfqg->rq_pos_tree = RB_ROOT;
	/* [한국어] LBA 위치 기반 cooperator 탐색용 rb-tree를 빈 트리로 초기화 */
}

/*
 * [한국어]
 * bfq_pd_free - bfq_group의 통계 자료구조를 정리하고 참조를 반환
 *
 * @pd: 해제할 struct blkg_policy_data(bfq_group.pd).
 * @return: 없음(void).
 *
 * blkcg_policy_bfq.pd_free_fn으로 등록되어, blkg가 완전히 해제되는
 * 마지막 단계에서 blk-cgroup 코어가 호출한다. bfqg_stats_exit()로
 * percpu 통계 자료구조를 먼저 해제한 뒤, bfqg_put()으로 이 함수 호출
 * 자체가 쥐고 있던 참조를 반환한다(다른 참조가 남아 있지 않다면 이때
 * bfq_group 메모리 자체가 kfree된다).
 *
 * 호출 체인:
 *   blkg_free_workfn() [blk-cgroup.c] → blkcg_policy_bfq.pd_free_fn → [bfq_pd_free]
 *     → bfqg_stats_exit()/bfqg_put()
 */
static void bfq_pd_free(struct blkg_policy_data *pd)
{
	struct bfq_group *bfqg = pd_to_bfqg(pd);
	/* [한국어] pd를 포함하는 bfq_group 자신 */

	bfqg_stats_exit(&bfqg->stats); /* [한국어] percpu_counter/blkg_rwstat 등 통계 자료구조 해제 */
	bfqg_put(bfqg); /* [한국어] 이 해제 경로가 쥐고 있던 참조 반환 - 마지막 참조면 kfree */
}

/*
 * [한국어]
 * bfq_pd_reset_stats - cgroupfs의 stat reset 요청 처리
 *
 * @pd: 대상 struct blkg_policy_data(bfq_group.pd).
 * @return: 없음(void).
 *
 * blkcg_policy_bfq.pd_reset_stats_fn으로 등록되어, 사용자가 cgroupfs의
 * 통계 리셋 인터페이스를 통해 이 그룹의 누적 통계를 0으로 되돌릴 때
 * 호출된다. 실제 작업은 bfqg_stats_reset()에 위임한다.
 *
 * 호출 체인:
 *   blkcg_reset_stats() [blk-cgroup.c, cgroupfs write 핸들러] → [bfq_pd_reset_stats]
 *     → bfqg_stats_reset()
 */
static void bfq_pd_reset_stats(struct blkg_policy_data *pd)
{
	struct bfq_group *bfqg = pd_to_bfqg(pd); /* [한국어] pd를 포함하는 bfq_group 자신 */

	bfqg_stats_reset(&bfqg->stats); /* [한국어] 이 그룹의 누적 디버그 통계를 0으로 리셋 */
}

/*
 * [한국어]
 * bfq_group_set_parent - bfq_group의 entity를 지정된 부모 그룹 아래로 연결
 *
 * @bfqg: 연결할 자식 bfq_group.
 * @parent: 연결할 부모 bfq_group.
 *
 * @bfqg->entity.parent를 @parent->my_entity로, entity.sched_data를
 * &parent->sched_data로 설정한다. 이렇게 하면 다음 스케줄링 라운드에서
 * @bfqg의 entity가 @parent의 service_tree[]에 편입되어, H-WF2Q+ 계층에서
 * @parent 아래 자식으로 취급된다. bfq_link_bfqg()가 cgroup 계층을 BFQ
 * 내부 구조에 반영할 때 사용하는 하위 헬퍼다.
 *
 * 호출 체인:
 *   bfq_link_bfqg() → [bfq_group_set_parent]
 */
static void bfq_group_set_parent(struct bfq_group *bfqg,
					struct bfq_group *parent)
{
	struct bfq_entity *entity; /* [한국어] 재배치할 대상 entity(항상 bfqg 자신의 entity) */

	entity = &bfqg->entity; /* [한국어] bfqg를 상위 트리에 등록하는 데 쓰이는 entity 포인터 획득 */
	/* [한국어] 부모 entity로 parent의 대표 entity(my_entity)를 설정 - parent가
	 * root_group이면 my_entity가 NULL이라 entity->parent도 NULL이 됨 */
	entity->parent = parent->my_entity;
	/* [한국어] 이 entity가 실제로 편입될 서비스 트리를 parent의 sched_data로 지정 */
	entity->sched_data = &parent->sched_data;
}

/*
 * [한국어]
 * bfq_link_bfqg - bfqg로부터 root까지의 조상 체인을 cgroup 계층에 맞춰 연결
 *
 * @bfqd: 디바이스 전역 struct bfq_data(root_group을 담고 있음).
 * @bfqg: 연결을 시작할(대개 리프에 해당하는) bfq_group.
 * @return: 없음(void).
 *
 * cgroup은 동적으로 생성/삭제되므로, 어떤 leaf bfq_group이 필요해진
 * 시점에 그 조상들 중 일부가 아직 BFQ 내부 계층에 연결되지 않았을 수
 * 있다(원본 주석). for_each_entity()로 @bfqg의 entity부터 시작해 위로
 * (entity->parent를 따라) 올라가며, root_group이 아닌 모든 조상에 대해
 * bfqg_parent()로 실제 cgroup 부모를 찾고 bfq_group_set_parent()로
 * 연결한다. bfqg_parent()가 NULL을 반환하면(아직 매핑 안 된 중간
 * cgroup) root_group으로 잠정 연결해, 최소한 스케줄링 트리가 끊기지
 * 않도록 보장한다.
 * bfqd->lock을 쥔 상태에서 호출된다(entity 트리 변경이므로).
 *
 * 호출 체인:
 *   bfq_bic_update_cgroup() → [bfq_link_bfqg] → bfqg_parent()/bfq_group_set_parent()
 */
static void bfq_link_bfqg(struct bfq_data *bfqd, struct bfq_group *bfqg)
{
	struct bfq_group *parent; /* [한국어] 현재 순회 중인 그룹의 실제 cgroup 부모 */
	struct bfq_entity *entity; /* [한국어] for_each_entity 순회에 쓰이는 반복자 */

	/*
	 * Update chain of bfq_groups as we might be handling a leaf group
	 * which, along with some of its relatives, has not been hooked yet
	 * to the private hierarchy of BFQ.
	 */
	entity = &bfqg->entity; /* [한국어] 순회 시작점 - 대상 leaf 그룹 자신의 entity */
	/* [한국어] entity->parent를 따라 root까지 상향 순회(bfq-iosched.h의
	 * for_each_entity 매크로) - 이미 연결된 조상까지도 재확인/재연결한다 */
	for_each_entity(entity) {
		struct bfq_group *curr_bfqg = container_of(entity,
						/* [한국어] 현재 순회 위치의 entity를 포함하는 bfq_group 획득 */
						struct bfq_group, entity);
		/* [한국어] root_group 자신은 연결할 부모가 없으므로 제외 */
		if (curr_bfqg != bfqd->root_group) {
			parent = bfqg_parent(curr_bfqg);
			/* [한국어] cgroup 계층상의 실제 부모 bfq_group 탐색 */
			if (!parent)
				parent = bfqd->root_group;
				/* [한국어] 아직 매핑되지 않은 부모라면 잠정적으로 root_group에 연결 -
				 * 이후 부모 자신이 처리될 때 다시 갱신될 수 있음 */
			bfq_group_set_parent(curr_bfqg, parent);
			/* [한국어] 결정된 parent로 curr_bfqg의 entity/sched_data 연결 */
		}
	}
}

/*
 * [한국어]
 * bfq_bio_bfqg - bio->bi_blkg로부터 실제로 사용할 online 상태의 bfq_group을 찾음
 *
 * @bfqd: 디바이스 전역 struct bfq_data(root_group 폴백에 사용).
 * @bio: cgroup을 판별할 대상 bio.
 * @return: online 상태가 확인된 bfq_group. 적절한 조상을 하나도 못 찾으면
 *          bfqd->root_group.
 *
 * bio->bi_blkg는 bio_associate_blkg()가 제출 시점에 설정해 둔 cgroup
 * 링크이지만, 그 cgroup의 blkg나 policy data(pd)가 아직 online되지
 * 않았을 수 있다(cgroup 생성 직후 등 레이스 상황). 이 함수는 bi_blkg부터
 * blkg->parent를 따라 위로 올라가며 "blkg 자체가 online이고 그 bfq_group의
 * pd도 online인" 첫 조상을 찾는다. 찾으면 bio_associate_blkg_from_css()로
 * bio의 cgroup 연결을 그 조상으로 갱신해(이후 merge/재조회 시 다시 이
 * 탐색을 반복하지 않도록) 반환한다. 끝까지 못 찾으면 root_group으로
 * bio를 재연결하고 root_group을 반환한다.
 *
 * 호출 체인:
 *   bfq_bic_update_cgroup() → [bfq_bio_bfqg] → blkg_to_bfqg()/bio_associate_blkg_from_css()
 */
struct bfq_group *bfq_bio_bfqg(struct bfq_data *bfqd, struct bio *bio)
{
	struct blkcg_gq *blkg = bio->bi_blkg;
	/* [한국어] bio 제출 시점에 이미 연결된 cgroup 링크(자기 자신 cgroup의 blkg)에서 탐색 시작 */
	struct bfq_group *bfqg; /* [한국어] 현재 순회 위치의 blkg에 대응하는 bfq_group */

	while (blkg) {
		/* [한국어] blkg가 NULL이 될 때까지(=root까지 다 올라갈 때까지) 부모 방향으로 순회 */
		if (!blkg->online) {
			/* [한국어] 이 blkg 자체가 아직 online 처리되지 않았다면(생성 레이스 등) 사용 불가 */
			blkg = blkg->parent; /* [한국어] 한 단계 위 조상으로 이동해 재시도 */
			continue; /* [한국어] 아래의 pd.online 검사를 건너뛰고 while 루프 조건부터 재평가 */
		}
		bfqg = blkg_to_bfqg(blkg);
		/* [한국어] blkg를 BFQ 전용 bfq_group으로 변환 */
		if (bfqg->pd.online) {
			/* [한국어] blkg뿐 아니라 BFQ policy data(pd) 자체도 online이어야
			 * (pd_init_fn까지 끝난 상태여야) 안전하게 사용 가능 */
			bio_associate_blkg_from_css(bio, &blkg->blkcg->css);
			/* [한국어] bio의 cgroup 연결을 실제로 찾은 이 blkg의 css로 갱신 -
			 * 다음번 같은 bio/파생 bio에서는 곧바로 이 지점부터 시작 가능 */
			return bfqg; /* [한국어] 사용 가능한 bfq_group을 찾았으므로 반환 */
		}
		blkg = blkg->parent; /* [한국어] pd가 아직 online이 아니면 더 위 조상으로 계속 탐색 */
	}
	bio_associate_blkg_from_css(bio,
				&bfqg_to_blkg(bfqd->root_group)->blkcg->css);
	/* [한국어] 끝까지 online인 조상을 못 찾았으므로 bio를 root_group의 css로 재연결 */
	return bfqd->root_group; /* [한국어] 최종 폴백으로 root_group 반환 - 항상 online 상태로 보장됨 */
}

/**
 * bfq_bfqq_move - migrate @bfqq to @bfqg.
 * @bfqd: queue descriptor.
 * @bfqq: the queue to move.
 * @bfqg: the group to move to.
 *
 * Move @bfqq to @bfqg, deactivating it from its old group and reactivating
 * it on the new one.  Avoid putting the entity on the old group idle tree.
 *
 * Must be called under the scheduler lock, to make sure that the blkg
 * owning @bfqg does not disappear (see comments in
 * bfq_bic_update_cgroup on guaranteeing the consistency of blkg
 * objects).
 */
/*
 * [한국어]
 * bfq_bfqq_move - bfq_queue를 다른 bfq_group으로 이전(cgroup 변경 반영)
 *
 * @bfqd: 디바이스 전역 struct bfq_data.
 * @bfqq: 이전할 대상 bfq_queue.
 * @bfqg: 이전해 갈 목적지 bfq_group.
 * @return: 없음(void).
 *
 * 이 함수는 실행 중인 프로세스가 다른 cgroup으로 옮겨지거나, cgroup이
 * offline되어 leaf 큐들이 root_group으로 재배치될 때(bfq_reparent_*())
 * 호출되는 핵심 이전 로직이다. 순서가 중요하다:
 *   1) 이미 목적지와 같은 그룹이면(예: root_group offline 중 중복 호출)
 *      아무 일도 하지 않고 반환.
 *   2) oom_bfqq(메모리 부족 시 폴백용 특수 큐)는 항상 root_group에
 *      고정되어야 하므로 이동을 거부.
 *   3) 이동 도중 expire/deactivate로 bfqq가 해제되지 않도록 임시로
 *      참조를 하나 더 얻는다.
 *   4) pending-reqs 그룹 리스트에서 잠시 빼고, 현재 서비스 중이면
 *      expire, busy 상태면 deactivate(또는 idle tree에 있으면 그
 *      entity를 제거)해 옛 그룹의 스케줄링 자료구조에서 완전히 뗀다.
 *   5) 옛 그룹(old_parent)에 대한 참조를 반환하고, entity의 parent/
 *      sched_data를 새 그룹 기준으로 갱신한 뒤 새 그룹에 대한 참조를
 *      얻는다(pin).
 *   6) pending-reqs였다면 새 그룹 기준으로 다시 등록하고, busy 상태였다면
 *      새 그룹에서 다시 activate한다.
 *   7) 마지막으로 dispatch가 멈춰 있었다면 재개를 예약하고, 처음에 얻은
 *      임시 참조를 반환한다(이 시점에 bfqq가 실제로 해제될 수 있음).
 * bfqd->lock을 쥔 상태에서 호출되어야 하며(원본 주석), 그래야 @bfqg가
 * 속한 blkg가 이동 도중 사라지지 않음을 보장할 수 있다.
 *
 * 호출 체인:
 *   bfq_sync_bfqq_move()/bfq_reparent_leaf_entity() → [bfq_bfqq_move]
 *     → bfq_bfqq_expire()/bfq_deactivate_bfqq()/bfq_activate_bfqq()/
 *       bfq_schedule_dispatch()
 */
void bfq_bfqq_move(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		   struct bfq_group *bfqg)
{
	struct bfq_entity *entity = &bfqq->entity;
	/* [한국어] 이동 대상 bfqq의 스케줄링 entity */
	struct bfq_group *old_parent = bfqq_group(bfqq);
	/* [한국어] 이동 전 현재 bfqq가 속해 있던 bfq_group(반환할 참조의 대상) */
	bool has_pending_reqs = false;
	/* [한국어] bfqq가 pending-reqs 그룹 리스트에 있었는지 여부 - 이동 후 복원 판단에 사용 */

	/*
	 * No point to move bfqq to the same group, which can happen when
	 * root group is offlined
	 */
	if (old_parent == bfqg)
		return; /* [한국어] 이미 목적지와 같은 그룹이면 이동할 필요가 없으므로 즉시 반환 */

	/*
	 * oom_bfqq is not allowed to move, oom_bfqq will hold ref to root_group
	 * until elevator exit.
	 */
	if (bfqq == &bfqd->oom_bfqq)
		return; /* [한국어] oom_bfqq(메모리 부족 시 폴백 큐)는 elevator 종료까지 root_group에 고정되어야 하므로 이동 거부 */
	/*
	 * Get extra reference to prevent bfqq from being freed in
	 * next possible expire or deactivate.
	 */
	bfqq->ref++;
	/* [한국어] 아래 expire/deactivate 호출이 bfqq를 완전히 해제해 버리지 않도록
	 * 임시로 참조 카운트를 하나 더 늘림(함수 끝에서 bfq_put_queue로 반환) */

	if (entity->in_groups_with_pending_reqs) {
		has_pending_reqs = true;
		/* [한국어] 이동 전 pending-reqs 리스트에 있었음을 기록해 두어, 이동 후
		 * 새 그룹 기준으로 다시 등록할지 판단하는 데 사용 */
		bfq_del_bfqq_in_groups_with_pending_reqs(bfqq);
		/* [한국어] 옛 그룹 기준의 pending-reqs 리스트에서 우선 제거 - 그룹이 바뀌는
		 * 도중에는 어느 그룹에도 등록되어 있지 않은 일관된 상태를 유지 */
	}

	/* If bfqq is empty, then bfq_bfqq_expire also invokes
	 * bfq_del_bfqq_busy, thereby removing bfqq and its entity
	 * from data structures related to current group. Otherwise we
	 * need to remove bfqq explicitly with bfq_deactivate_bfqq, as
	 * we do below.
	 */
	if (bfqq == bfqd->in_service_queue)
		/* [한국어] 지금 이 bfqq가 실제로 서비스(dispatch) 중이라면 */
		bfq_bfqq_expire(bfqd, bfqd->in_service_queue,
				false, BFQQE_PREEMPTED);
				/* [한국어] BFQQE_PREEMPTED 사유로 강제 만료시켜 서비스를 중단 -
				 * bfqq가 비어 있었다면 이 호출이 bfq_del_bfqq_busy까지 연쇄
				 * 호출해 옛 그룹 자료구조에서 이미 제거를 완료함(원본 주석) */

	if (bfq_bfqq_busy(bfqq))
		/* [한국어] 위 expire로도 제거되지 않고 여전히 busy(active tree에 남아 있음)면 */
		bfq_deactivate_bfqq(bfqd, bfqq, false, false);
		/* [한국어] 명시적으로 비활성화해 active tree에서 제거(가상 시간 갱신 포함) */
	else if (entity->on_st_or_in_serv)
		/* [한국어] busy는 아니지만 여전히 idle tree 등 서비스 트리에 걸려 있다면 */
		bfq_put_idle_entity(bfq_entity_service_tree(entity), entity);
		/* [한국어] idle tree에서 제거 - 옛 그룹의 idle tree에 남겨두지 않기 위함(원본 주석) */
	bfqg_and_blkg_put(old_parent);
	/* [한국어] 옛 그룹(old_parent)에 대한 참조 반환 - entity가 더 이상 이 그룹을
	 * 참조하지 않으므로 안전 */

	bfq_reassign_last_bfqq(bfqq, NULL);
	/* [한국어] "가장 최근 생성된 bfqq" 캐시가 이 bfqq를 옛 그룹 기준으로 가리키고
	 * 있었다면 그 연결을 해제(그룹이 바뀌므로 캐시 무효화) */
	entity->parent = bfqg->my_entity;
	/* [한국어] entity의 부모를 새 그룹의 대표 entity로 갱신 */
	entity->sched_data = &bfqg->sched_data;
	/* [한국어] entity가 편입될 서비스 트리를 새 그룹의 sched_data로 갱신 */
	/* pin down bfqg and its associated blkg  */
	bfqg_and_blkg_get(bfqg);
	/* [한국어] 새 그룹(bfqg)과 그 blkg에 대한 참조를 획득(pin) - entity가 이 그룹을
	 * 참조하는 동안 그룹이 사라지지 않도록 보장 */

	if (has_pending_reqs)
		/* [한국어] 이동 전 pending-reqs 리스트에 있었다면 */
		bfq_add_bfqq_in_groups_with_pending_reqs(bfqq);
		/* [한국어] 새 그룹 기준으로 다시 pending-reqs 리스트에 등록 */

	if (bfq_bfqq_busy(bfqq)) {
		/* [한국어] 이동 후에도 여전히 busy(처리할 request가 있음)라면 새 그룹에서 재활성화 */
		if (unlikely(!bfqd->nonrot_with_queueing))
			/* [한국어] NCQ 큐잉을 지원하지 않는 비회전 장치가 아닌 일반적인 경우
			 * (원문 조건의 부정: nonrot_with_queueing이 거짓인 흔치 않은 경우) */
			bfq_pos_tree_add_move(bfqd, bfqq);
			/* [한국어] LBA 위치 기반 rq_pos_tree에도 새 그룹 기준으로 재등록해
			 * cooperator 탐색이 새 그룹 내에서 이루어지도록 함 */
		bfq_activate_bfqq(bfqd, bfqq);
		/* [한국어] 새 그룹의 service_tree에 bfqq의 entity를 다시 삽입(활성화) */
	}

	if (!bfqd->in_service_queue && !bfqd->tot_rq_in_driver)
		/* [한국어] 현재 서비스 중인 큐가 없고 드라이버에 in-flight request도
		 * 없다면(즉 디스패치가 멈춰 있을 수 있는 상태라면) */
		bfq_schedule_dispatch(bfqd);
		/* [한국어] 새로 활성화된 bfqq가 서비스받을 수 있도록 dispatch 재개를 예약 */
	/* release extra ref taken above, bfqq may happen to be freed now */
	bfq_put_queue(bfqq);
	/* [한국어] 함수 초입에 얻은 임시 참조를 반환 - 다른 참조가 전혀 남아있지 않다면
	 * 이 시점에 bfqq 자체가 해제될 수 있음(원본 주석) */
}

/*
 * [한국어]
 * bfq_sync_bfqq_move - 동기(sync) bfq_queue를 cgroup 변경에 맞춰 이동시키되
 *   cooperative merge 체인의 cgroup 일관성을 검증
 *
 * @bfqd: 디바이스 전역 struct bfq_data.
 * @sync_bfqq: 프로세스의 동기 bfq_queue(bic->bfqq[1][act_idx]).
 * @bic: 이 프로세스의 bfq_io_cq(merge 취소 시 bic_set_bfqq 호출에 필요).
 * @bfqg: 이동해 갈 목적지 bfq_group.
 * @act_idx: 대상 actuator 인덱스(멀티 액추에이터 디바이스 지원용).
 * @return: 없음(void).
 *
 * sync_bfqq가 다른 큐와 병합(cooperative merge, new_bfqq 체인)되어 있지
 * 않다면 단순히 bfq_bfqq_move()로 이동시키면 끝이다. 그러나 병합된
 * 상태라면 문제가 생긴다: merge 체인에 속한 다른 bfq_queue들은 원래
 * "서로 인접한 LBA에 접근하는 협력 관계"라는 이유로 하나로 묶인 것인데,
 * 그중 일부만 새 cgroup으로 옮겨가면 서로 다른 cgroup의 I/O가 하나의
 * bfqq로 합쳐진 채 남아 우선순위 격리가 깨진다. 그래서 체인 전체
 * (sync_bfqq->new_bfqq->new_bfqq->...)를 순회하며 모두 같은 sched_data
 * (=같은 cgroup)에 속하는지 확인하고, 하나라도 다르면 병합 자체를
 * 취소한다(merge를 조용히 유지한 채 옮길 수는 없으므로).
 *
 * 호출 체인:
 *   __bfq_bic_change_cgroup() → [bfq_sync_bfqq_move] → bfq_bfqq_move() /
 *     bfq_put_cooperator()/bic_set_bfqq()/bfq_release_process_ref()
 */
static void bfq_sync_bfqq_move(struct bfq_data *bfqd,
			       struct bfq_queue *sync_bfqq,
			       struct bfq_io_cq *bic,
			       struct bfq_group *bfqg,
			       unsigned int act_idx)
{
	struct bfq_queue *bfqq; /* [한국어] merge 체인 순회용 반복자 */

	/* [한국어] 다른 큐로 병합되지도 않았고(new_bfqq==NULL) 협력 큐 플래그도
	 * 없다면 이 bic만의 전용 큐이므로 병합 검증이 필요 없음 */
	if (!sync_bfqq->new_bfqq && !bfq_bfqq_coop(sync_bfqq)) {
		/* We are the only user of this bfqq, just move it */
		/* [한국어] 이미 목적지 그룹에 속해 있지 않을 때만 이동 수행(불필요한 이동 방지) */
		if (sync_bfqq->entity.sched_data != &bfqg->sched_data)
			bfq_bfqq_move(bfqd, sync_bfqq, bfqg);
		return; /* [한국어] 단순 이동 경로는 여기서 종료 */
	}

	/*
	 * The queue was merged to a different queue. Check
	 * that the merge chain still belongs to the same
	 * cgroup.
	 */
	/* [한국어] sync_bfqq부터 시작해 new_bfqq 포인터를 따라 병합 체인 전체를 순회 */
	for (bfqq = sync_bfqq; bfqq; bfqq = bfqq->new_bfqq)
		if (bfqq->entity.sched_data != &bfqg->sched_data)
			/* [한국어] 체인 중 하나라도 목적지 그룹과 sched_data가 다르면(=다른 cgroup) */
			break; /* [한국어] 루프를 중단 - bfqq가 NULL이 아닌 상태로 남아 아래 조건이 참이 됨 */
	if (bfqq) {
		/* [한국어] 위 루프가 break로 끝났다면(=끝까지 못 가고 불일치 발견) bfqq != NULL */
		/*
		 * Some queue changed cgroup so the merge is not valid
		 * anymore. We cannot easily just cancel the merge (by
		 * clearing new_bfqq) as there may be other processes
		 * using this queue and holding refs to all queues
		 * below sync_bfqq->new_bfqq. Similarly if the merge
		 * already happened, we need to detach from bfqq now
		 * so that we cannot merge bio to a request from the
		 * old cgroup.
		 */
		bfq_put_cooperator(sync_bfqq);
		/* [한국어] sync_bfqq의 협력(merge) 관계를 해제 - 서로 다른 cgroup으로
		 * 갈라선 큐들이 계속 하나로 묶여 있지 않도록 함 */
		bic_set_bfqq(bic, NULL, true, act_idx);
		/* [한국어] 이 bic의 동기 슬롯 연결을 끊음 - 다음 bio 제출 시
		 * bfq_get_queue()가 새 cgroup에 맞는 새 bfqq를 만들게 됨 */
		bfq_release_process_ref(bfqd, sync_bfqq);
		/* [한국어] 이 bic이 쥐고 있던 sync_bfqq에 대한 참조를 반환 */
	}
}

/**
 * __bfq_bic_change_cgroup - move @bic to @bfqg.
 * @bfqd: the queue descriptor.
 * @bic: the bic to move.
 * @bfqg: the group to move to.
 *
 * Move bic to blkcg, assuming that bfqd->lock is held; which makes
 * sure that the reference to cgroup is valid across the call (see
 * comments in bfq_bic_update_cgroup on this issue)
 */
/*
 * [한국어]
 * __bfq_bic_change_cgroup - 한 프로세스(bic)가 소유한 모든 actuator의
 *   async/sync bfq_queue를 새 cgroup(bfqg)으로 이전
 *
 * @bfqd: 디바이스 전역 struct bfq_data.
 * @bic: 대상 프로세스의 bfq_io_cq.
 * @bfqg: 이전해 갈 목적지 bfq_group.
 * @return: 없음(void).
 *
 * 멀티 액추에이터(다중 헤드) 디바이스에서는 actuator마다 독립적인
 * async_bfqq/sync_bfqq 슬롯이 있으므로, bfqd->num_actuators만큼 루프를
 * 돌며 각 actuator에 대해 처리한다. async 큐는 그룹 단위로 공유되는
 * 구조라(bfq_group.async_bfqq[][][]) 프로세스가 cgroup을 옮기면 기존
 * async 큐와의 연결을 그냥 끊어버리고(bic_set_bfqq(NULL)), 다음
 * 비동기 I/O 발생 시 새 그룹의 공유 async 큐를 새로 얻게 한다(단순
 * 재할당이 이동보다 쉬움). sync 큐는 프로세스 전용이므로
 * bfq_sync_bfqq_move()로 실제 이전(및 필요 시 merge 체인 검증)을
 * 수행한다.
 *
 * 호출 체인:
 *   bfq_bic_update_cgroup() → [__bfq_bic_change_cgroup]
 *     → bic_set_bfqq()/bfq_release_process_ref()/bfq_sync_bfqq_move()
 */
static void __bfq_bic_change_cgroup(struct bfq_data *bfqd,
				    struct bfq_io_cq *bic,
				    struct bfq_group *bfqg)
{
	unsigned int act_idx; /* [한국어] 0..num_actuators-1 범위의 actuator 인덱스 */

	/* [한국어] 이 디바이스의 모든 actuator에 대해 반복 처리 */
	for (act_idx = 0; act_idx < bfqd->num_actuators; act_idx++) {
		struct bfq_queue *async_bfqq = bic_to_bfqq(bic, false, act_idx);
		/* [한국어] 이 actuator의 async(비동기) 큐 - 그룹 공유 자원 */
		struct bfq_queue *sync_bfqq = bic_to_bfqq(bic, true, act_idx);
		/* [한국어] 이 actuator의 sync(동기) 큐 - 프로세스 전용 자원 */

		if (async_bfqq &&
		    async_bfqq->entity.sched_data != &bfqg->sched_data) {
			/* [한국어] async 큐가 존재하고 이미 목적지 그룹 소속이 아니라면 */
			bic_set_bfqq(bic, NULL, false, act_idx);
			/* [한국어] bic의 async 슬롯 연결을 끊음(이동시키지 않고 그냥 분리) */
			bfq_release_process_ref(bfqd, async_bfqq);
			/* [한국어] 이 bic이 쥐고 있던 async_bfqq 참조를 반환 */
		}

		if (sync_bfqq)
			/* [한국어] sync 큐가 존재하면(아직 생성되지 않았을 수도 있음) */
			bfq_sync_bfqq_move(bfqd, sync_bfqq, bic, bfqg, act_idx);
			/* [한국어] merge 체인 검증을 포함한 안전한 이전 수행 */
	}
}

/*
 * [한국어]
 * bfq_bic_update_cgroup - bio 제출 시점에 프로세스의 cgroup 변경 여부를 확인·반영
 *
 * @bic: 이 bio를 제출한 프로세스의 bfq_io_cq.
 * @bio: 제출된 bio(cgroup 판별에 bio->bi_blkg 사용).
 * @return: 없음(void).
 *
 * 프로세스는 실행 도중 다른 cgroup으로 옮겨질 수 있으므로(cgroup.procs에
 * write), 매 bio 제출마다 "지금 이 bio가 속한 cgroup"과 "이 bic이 마지막
 * 으로 확인한 cgroup"이 같은지 확인해야 한다. bfq_bio_bfqg()로 현재
 * bfq_group을 얻고, 그 blkg의 cgroup serial_nr(cgroup이 바뀔 때마다
 * 증가하는 토큰)을 bic->blkcg_serial_nr과 비교한다. 같으면(가장 흔한
 * 경우) 아무 것도 하지 않고 바로 반환해 매 bio마다의 오버헤드를 최소화한다.
 * 다르면 bfq_link_bfqg()로 이 bfqg부터 root까지의 조상 체인이 BFQ
 * 내부 계층에 제대로 연결되어 있는지 보정하고,
 * __bfq_bic_change_cgroup()으로 이 프로세스의 모든 bfq_queue를 실제로
 * 새 그룹에 이전한 뒤, blkcg_serial_nr을 갱신해 다음 호출에서 다시
 * 감지되지 않게 한다.
 *
 * 호출 체인:
 *   bfq_init_rq()/bfq_insert_request() [bfq-iosched.c] → [bfq_bic_update_cgroup]
 *     → bfq_bio_bfqg()/bfq_link_bfqg()/__bfq_bic_change_cgroup()
 */
void bfq_bic_update_cgroup(struct bfq_io_cq *bic, struct bio *bio)
{
	struct bfq_data *bfqd = bic_to_bfqd(bic);
	/* [한국어] 이 bic이 속한 디바이스의 전역 bfq_data */
	struct bfq_group *bfqg = bfq_bio_bfqg(bfqd, bio);
	/* [한국어] 이 bio가 실제로 속해야 할(online 상태가 확인된) bfq_group */
	uint64_t serial_nr;
	/* [한국어] bfqg가 속한 cgroup의 css.serial_nr - cgroup이 바뀔 때마다 갱신되는 세대 토큰 */

	serial_nr = bfqg_to_blkg(bfqg)->blkcg->css.serial_nr;
	/* [한국어] bfqg -> blkg -> blkcg -> css 경로로 현재 cgroup의 serial_nr을 읽음 */

	/*
	 * Check whether blkcg has changed.  The condition may trigger
	 * spuriously on a newly created cic but there's no harm.
	 */
	if (unlikely(!bfqd) || likely(bic->blkcg_serial_nr == serial_nr))
		/* [한국어] bfqd가 아직 없거나(초기화 레이스) serial_nr이 이전과 같다면
		 * (cgroup 변경 없음, 가장 흔한 빠른 경로) 아무 작업 없이 반환 */
		return;

	/*
	 * New cgroup for this process. Make sure it is linked to bfq internal
	 * cgroup hierarchy.
	 */
	/* [한국어] bfqg부터 root까지의 BFQ 내부 계층 연결을 최신 cgroup 구조에 맞춰 보정 */
	bfq_link_bfqg(bfqd, bfqg);
	/* [한국어] 이 프로세스(bic)의 모든 actuator별 async/sync 큐를 실제로 새 그룹으로 이전 */
	__bfq_bic_change_cgroup(bfqd, bic, bfqg);
	/* [한국어] 다음 bio 제출부터는 이 값과 비교해 변경 여부를 판단하도록 캐시 갱신 */
	bic->blkcg_serial_nr = serial_nr;
}

/**
 * bfq_flush_idle_tree - deactivate any entity on the idle tree of @st.
 * @st: the service tree being flushed.
 */
/*
 * [한국어]
 * bfq_flush_idle_tree - 서비스 트리의 idle tree에 남은 모든 entity를 비활성화
 *
 * @st: 대상 struct bfq_service_tree(한 그룹의 특정 ioprio_class 트리).
 * @return: 없음(void).
 *
 * idle tree는 최근까지 active였다가 잠시 쉬고 있는(그러나 완전히
 * 제거되지는 않은) entity를 보관하는 rb-tree다. cgroup이 offline될 때는
 * 이런 entity도 정리해야 하므로, st->first_idle이 가리키는 entity를
 * 하나씩 __bfq_deactivate_entity()로 완전히 비활성화하며, 그 호출이
 * st->first_idle을 갱신하므로 반복문은 first_idle이 NULL이 될 때까지
 * (더 이상 idle entity가 없을 때까지) 계속된다.
 *
 * 호출 체인:
 *   bfq_pd_offline() → [bfq_flush_idle_tree] → __bfq_deactivate_entity()
 */
static void bfq_flush_idle_tree(struct bfq_service_tree *st)
{
	struct bfq_entity *entity = st->first_idle;
	/* [한국어] idle tree의 첫 entity(가장 먼저 idle이 된 것)부터 순회 시작 */

	for (; entity ; entity = st->first_idle)
		/* [한국어] entity가 NULL이 아닌 동안(=아직 idle tree에 뭔가 남아 있는 동안)
		 * 매 반복마다 st->first_idle을 다시 읽어 최신 상태 반영 */
		__bfq_deactivate_entity(entity, false);
		/* [한국어] 이 entity를 idle tree에서 제거하고 완전히 비활성화 - 내부적으로
		 * st->first_idle이 다음 idle entity로 갱신됨 */
}

/**
 * bfq_reparent_leaf_entity - move leaf entity to the root_group.
 * @bfqd: the device data structure with the root group.
 * @entity: the entity to move, if entity is a leaf; or the parent entity
 *	    of an active leaf entity to move, if entity is not a leaf.
 * @ioprio_class: I/O priority class to reparent.
 */
/*
 * [한국어]
 * bfq_reparent_leaf_entity - 계층을 타고 내려가 실제 leaf(bfq_queue) entity를
 *   찾아 root_group으로 이동
 *
 * @bfqd: root_group을 담고 있는 디바이스 전역 struct bfq_data.
 * @entity: 시작점 entity. 그 자체가 leaf일 수도 있고, 활성 자손을 둔
 *          중간(그룹) entity일 수도 있다.
 * @ioprio_class: reparent 대상 I/O 우선순위 클래스(RT/BE/IDLE 중 하나).
 * @return: 없음(void).
 *
 * BFQ의 계층 구조에서 그룹 entity는 자신만의 my_sched_data(자식들을 위한
 * 스케줄러)를 갖는 반면, leaf(bfq_queue) entity는 my_sched_data가 NULL이다.
 * 이 함수는 my_sched_data가 NULL이 될 때까지(=leaf에 도달할 때까지)
 * 계속 한 단계 아래로 내려간다: 현재 entity의 my_sched_data에서
 * ioprio_class에 해당하는 service_tree를 골라, 그 active tree의 첫
 * entity를 다음 후보로 삼는다. active tree가 비어 있다면(활성 자식이
 * 없다면) 그 대신 in_service_entity(현재 서비스 중이라 active tree에서
 * 빠져 있는 entity)를 후보로 삼는다. 최종적으로 leaf에 도달하면 그것을
 * bfq_queue로 변환해 bfq_bfqq_move()로 root_group으로 옮긴다.
 *
 * 호출 체인:
 *   bfq_reparent_active_queues() → [bfq_reparent_leaf_entity] → bfq_bfqq_move()
 */
static void bfq_reparent_leaf_entity(struct bfq_data *bfqd,
				     struct bfq_entity *entity,
				     int ioprio_class)
{
	struct bfq_queue *bfqq; /* [한국어] 최종적으로 찾아낼 leaf entity에 대응하는 bfq_queue */
	/* [한국어] 하향 탐색에 쓰이는 현재 위치 entity(처음엔 entity 자신) */
	struct bfq_entity *child_entity = entity;

	/* [한국어] my_sched_data가 있다는 것은 아직 그룹(중간 노드) entity라는 뜻 -
	 * NULL이 될 때까지(=leaf bfq_queue entity에 도달할 때까지) 반복 */
	while (child_entity->my_sched_data) { /* leaf not reached yet */
		struct bfq_sched_data *child_sd = child_entity->my_sched_data;
		/* [한국어] 현재 그룹 entity의 자식 스케줄러 */
		struct bfq_service_tree *child_st = child_sd->service_tree +
			ioprio_class;
			/* [한국어] BFQ_IOPRIO_CLASSES 배열 중 지정된 클래스의 서비스 트리 선택 */
		struct rb_root *child_active = &child_st->active;
		/* [한국어] 그 트리의 active(현재 스케줄링 대상) rb-tree */

		child_entity = bfq_entity_of(rb_first(child_active));
		/* [한국어] active 트리의 첫 entity(가장 왼쪽 노드)를 다음 하향 후보로 삼음 */

		if (!child_entity)
			/* [한국어] active 트리가 비어 있다면(활성 자식이 없다면) */
			child_entity = child_sd->in_service_entity;
			/* [한국어] 대신 현재 서비스 중이라 active tree 밖에 있는 entity를 후보로 사용 */
	}

	bfqq = bfq_entity_to_bfqq(child_entity);
	/* [한국어] 루프 종료 시 child_entity는 leaf(bfq_queue) entity이므로 그 bfq_queue를 획득 */
	bfq_bfqq_move(bfqd, bfqq, bfqd->root_group);
	/* [한국어] 찾은 leaf bfq_queue를 root_group으로 이동시켜 cgroup offline 이후에도
	 * 통계/스케줄링이 계속될 수 있는 안전한 그룹에 정착시킴 */
}

/**
 * bfq_reparent_active_queues - move to the root group all active queues.
 * @bfqd: the device data structure with the root group.
 * @bfqg: the group to move from.
 * @st: the service tree to start the search from.
 * @ioprio_class: I/O priority class to reparent.
 */
/*
 * [한국어]
 * bfq_reparent_active_queues - 한 서비스 트리(st)에 남은 모든 활성 leaf 큐를
 *   root_group으로 이동
 *
 * @bfqd: root_group을 담고 있는 디바이스 전역 struct bfq_data.
 * @bfqg: 비워낼 대상 bfq_group(offline 처리 중).
 * @st: 탐색을 시작할 struct bfq_service_tree(bfqg->sched_data의 한 클래스).
 * @ioprio_class: reparent 대상 I/O 우선순위 클래스.
 * @return: 없음(void).
 *
 * st->active rb-tree의 첫 entity를 반복적으로 꺼내(rb_first) leaf까지
 * 하향 탐색(bfq_reparent_leaf_entity)해 root_group으로 이동시키는
 * 과정을, active 트리가 완전히 빌 때까지 반복한다. bfq_bfqq_move()가
 * 내부적으로 옛 그룹에서 entity를 제거하므로, 매 반복마다 rb_first가
 * 다른(다음) entity를 가리키게 되어 결국 트리가 빈다. active 트리가
 * 빈 뒤에도 in_service_entity(active 트리 밖에서 현재 서비스 중인
 * entity)가 남아 있을 수 있으므로 별도로 한 번 더 reparent한다.
 *
 * 호출 체인:
 *   bfq_pd_offline() → [bfq_reparent_active_queues] → bfq_reparent_leaf_entity()
 */
static void bfq_reparent_active_queues(struct bfq_data *bfqd,
				       struct bfq_group *bfqg,
				       struct bfq_service_tree *st,
				       int ioprio_class)
{
	struct rb_root *active = &st->active;
	/* [한국어] 이 서비스 트리의 active rb-tree(비어 있는지 여부로 반복 종료 판단) */
	struct bfq_entity *entity; /* [한국어] rb_first로 얻은 현재 반복의 entity */

	while ((entity = bfq_entity_of(rb_first(active))))
		/* [한국어] active 트리가 비어(rb_first가 NULL) 있지 않은 동안 반복 */
		bfq_reparent_leaf_entity(bfqd, entity, ioprio_class);
		/* [한국어] 이 entity(또는 그 하위 leaf)를 root_group으로 이동 - 이동 결과로
		 * active 트리에서 항목이 하나 줄어들어 다음 반복의 rb_first가 갱신됨 */

	if (bfqg->sched_data.in_service_entity)
		/* [한국어] active 트리가 다 빈 뒤에도 현재 서비스 중인 entity가 남아 있다면 */
		bfq_reparent_leaf_entity(bfqd,
					 bfqg->sched_data.in_service_entity,
					 ioprio_class);
					 /* [한국어] 그 in_service_entity(또는 그 하위 leaf)도 root_group으로 이동 */
}

/**
 * bfq_pd_offline - deactivate the entity associated with @pd,
 *		    and reparent its children entities.
 * @pd: descriptor of the policy going offline.
 *
 * blkio already grabs the queue_lock for us, so no need to use
 * RCU-based magic
 */
/*
 * [한국어]
 * bfq_pd_offline - cgroup이 offline될 때 이 bfq_group을 스케줄링 트리에서
 *   제거하고 남은 자식들을 root_group으로 재배치
 *
 * @pd: offline 처리 중인 정책의 struct blkg_policy_data(bfq_group.pd).
 * @return: 없음(void).
 *
 * blkcg_policy_bfq.pd_offline_fn으로 등록되어, cgroup이 rmdir되어
 * blkg가 offline 상태로 전환될 때 blk-cgroup 코어가 호출한다. 호출자
 * (blkio core)가 이미 queue_lock을 잡고 호출하므로 RCU 없이도
 * bfqd->lock만 추가로 잡으면 충분하다(원본 주석). entity가 NULL이면
 * (this bfqg가 root_group 자신이면) 스케줄링 트리 정리 없이 async 큐
 * 정리로 바로 건너뛴다.
 * 그 외의 경우, 이 그룹의 모든 ioprio_class별 service_tree에 대해
 * active 트리를 root_group으로 reparent하고(bfq_reparent_active_queues)
 * idle 트리를 비운(bfq_flush_idle_tree) 뒤, 그룹 자신의 entity도
 * __bfq_deactivate_entity()로 비활성화한다. 마지막으로 async 큐들을
 * 정리하고(bfq_put_async_queues), 통계는 bfqg_stats_xfer_dead()로 부모에
 * 이관한다(이 시점 이후 완료되는 in-flight I/O의 통계는 유실될 수
 * 있음을 원본 주석이 명시적으로 인정하고 있다 - "Oh well...").
 *
 * 호출 체인:
 *   blkcg_css_offline()/blkg_destroy() [blk-cgroup.c] → blkcg_policy_bfq.pd_offline_fn
 *     → [bfq_pd_offline] → bfq_reparent_active_queues()/bfq_flush_idle_tree()/
 *       bfq_put_async_queues()/bfqg_stats_xfer_dead()
 */
static void bfq_pd_offline(struct blkg_policy_data *pd)
{
	struct bfq_service_tree *st; /* [한국어] 루프에서 순회할 현재 ioprio_class의 서비스 트리 */
	/* [한국어] offline 처리 대상 bfq_group */
	struct bfq_group *bfqg = pd_to_bfqg(pd);
	struct bfq_data *bfqd = bfqg->bfqd; /* 오프라인 대상 bfq_group */
	/* [한국어] 스케줄러 락(bfqd->lock)과 root_group을 얻기 위한 디바이스 전역 상태 */
	struct bfq_entity *entity = bfqg->my_entity; /* 오프라인할 group entity */
	/* [한국어] 이 그룹 자신을 상위 트리에 등록했던 entity - root_group이면 NULL */
	unsigned long flags; /* [한국어] spin_lock_irqsave가 복원할 인터럽트 플래그 저장소 */
	int i; /* [한국어] BFQ_IOPRIO_CLASSES 루프 인덱스 */

	spin_lock_irqsave(&bfqd->lock, flags);
	/* [한국어] 인터럽트를 비활성화하며 스케줄러 락 획득 - 완료 콜백(hardirq/softirq에서도
	 * 갱신될 수 있는 통계/entity 상태)과의 경쟁을 막기 위함 */

	if (!entity) /* root group */
		goto put_async_queues; /* [한국어] root_group 자신은 재배치할 상위가 없으므로 async 큐 정리로 바로 진행 */

	/*
	 * Empty all service_trees belonging to this group before
	 * deactivating the group itself.
	 */
	for (i = 0; i < BFQ_IOPRIO_CLASSES; i++) {
		/* [한국어] RT/BE/IDLE 각 클래스별 서비스 트리를 순서대로 처리 */
		st = bfqg->sched_data.service_tree + i;
		/* [한국어] i번째 클래스의 서비스 트리 포인터 계산 */

		/*
		 * It may happen that some queues are still active
		 * (busy) upon group destruction (if the corresponding
		 * processes have been forced to terminate). We move
		 * all the leaf entities corresponding to these queues
		 * to the root_group.
		 * Also, it may happen that the group has an entity
		 * in service, which is disconnected from the active
		 * tree: it must be moved, too.
		 * There is no need to put the sync queues, as the
		 * scheduler has taken no reference.
		 */
		bfq_reparent_active_queues(bfqd, bfqg, st, i);
		/* [한국어] 이 트리에 남은 active/in-service leaf 큐들을 모두 root_group으로 이동 -
		 * 프로세스가 강제 종료되었더라도 큐 자체는 아직 활성 상태일 수 있으므로 필요 */

		/*
		 * The idle tree may still contain bfq_queues
		 * belonging to exited task because they never
		 * migrated to a different cgroup from the one being
		 * destroyed now. In addition, even
		 * bfq_reparent_active_queues() may happen to add some
		 * entities to the idle tree. It happens if, in some
		 * of the calls to bfq_bfqq_move() performed by
		 * bfq_reparent_active_queues(), the queue to move is
		 * empty and gets expired.
		 */
		bfq_flush_idle_tree(st);
		/* [한국어] idle tree에 남은(종료된 태스크 소유 등) entity들도 모두 비활성화 -
		 * 위 reparent 과정에서 새로 idle tree에 추가된 entity까지 포함해 정리 */
	}

	__bfq_deactivate_entity(entity, false);
	/* [한국어] 그룹 자신의 entity를 상위 스케줄링 트리에서 비활성화 - 이제 이 그룹은
	 * 더 이상 부모의 service_tree에서 스케줄링 대상이 아님 */

put_async_queues:
	bfq_put_async_queues(bfqd, bfqg);
	/* [한국어] 이 그룹이 소유한 공유 async 큐들(그룹 전체 태스크가 공유)에 대한 참조를 반환 */

	spin_unlock_irqrestore(&bfqd->lock, flags);
	/* [한국어] 스케줄러 락 해제 및 인터럽트 상태 복원 */
	/*
	 * @blkg is going offline and will be ignored by
	 * blkg_[rw]stat_recursive_sum().  Transfer stats to the parent so
	 * that they don't get lost.  If IOs complete after this point, the
	 * stats for them will be lost.  Oh well...
	 */
	bfqg_stats_xfer_dead(bfqg);
	/* [한국어] offline된 blkg는 이후 재귀 통계 순회에서 제외되므로, 지금까지의
	 * 누적 통계를 부모의 aux 카운터로 이관 - 원본 주석대로 이 시점 이후 완료되는
	 * in-flight I/O의 통계는 어쩔 수 없이 유실될 수 있음을 인지하고 있음 */
}

/*
 * [한국어]
 * bfq_end_wr_async - 디바이스의 모든 cgroup에 걸쳐 async 큐의
 *   weight-raising(WR) 상태를 일괄 종료
 *
 * @bfqd: 디바이스 전역 struct bfq_data.
 * @return: 없음(void).
 *
 * weight-raising은 원래 interactive/soft-realtime 프로세스를 위한
 * 기능이지만, 특정 조건(예: 저지연 모드에서 async 큐도 일시적으로
 * 우대)에서는 async 큐에도 적용될 수 있다. 이 함수는 그런 WR 상태를
 * 강제로 끝내야 할 때(예: low_latency 파라미터가 꺼지거나 device idling
 * 관련 통계가 리셋될 때) 호출되며, request_queue에 연결된 모든 blkg를
 * 순회하며 각 bfq_group의 async 큐들에 대해 bfq_end_wr_async_queues()를
 * 적용한다. blkg_list에는 root_group에 대응하는 blkg가 포함되지 않을 수
 * 있으므로, 루프 이후 root_group도 별도로 명시적으로 처리한다.
 *
 * 호출 체인:
 *   bfq_end_wr()/CONFIG 변경 처리 [bfq-iosched.c] → [bfq_end_wr_async]
 *     → bfq_end_wr_async_queues() (그룹별)
 */
void bfq_end_wr_async(struct bfq_data *bfqd)
{
	struct blkcg_gq *blkg; /* [한국어] blkg_list 순회용 반복자 */

	/* [한국어] 이 request_queue(디바이스)에 연결된 모든 blkg(=모든 cgroup)를 순회 */
	list_for_each_entry(blkg, &bfqd->queue->blkg_list, q_node) {
		struct bfq_group *bfqg = blkg_to_bfqg(blkg);
		/* [한국어] 해당 blkg의 BFQ 전용 bfq_group으로 변환 */

		bfq_end_wr_async_queues(bfqd, bfqg);
		/* [한국어] 이 그룹에 속한 모든 async 큐(및 async_idle_bfqq)의 WR 상태 종료 */
	}
	bfq_end_wr_async_queues(bfqd, bfqd->root_group);
	/* [한국어] blkg_list 순회에 포함되지 않을 수 있는 root_group도 별도로 동일하게 처리 */
}

/*
 * [한국어]
 * bfq_io_show_weight_legacy - cgroup v1 "blkio.bfq.weight" read 핸들러
 *
 * @sf: 출력 대상 seq_file.
 * @v: 사용되지 않음(seq_file 반복자 프로토콜의 관례적 인자).
 * @return: 항상 0(seq_show 콜백 관례).
 *
 * 현재 cgroup(seq_css(sf)로 얻음)의 bfq_group_data에서 weight 값을 읽어
 * 그대로 출력한다. bfqgd가 NULL이면(정책이 이 cgroup에 아직 연결되지
 * 않은 극히 드문 경우) 0을 출력한다. cgroup v1은 per-cgroup 기본값
 * 하나만 노출하는 단순한 인터페이스다(v2의 "default N\n<device> N\n"
 * 형식과 달리 숫자 하나만 출력).
 *
 * 호출 체인:
 *   kernfs seq_show → [bfq_io_show_weight_legacy] → blkcg_to_bfqgd()
 */
static int bfq_io_show_weight_legacy(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));
	/* [한국어] 이 cgroupfs 파일을 읽는 현재 cgroup의 blkcg */
	struct bfq_group_data *bfqgd = blkcg_to_bfqgd(blkcg);
	/* [한국어] 이 cgroup에 저장된 BFQ per-cgroup 데이터(weight 포함) */
	unsigned int val = 0; /* [한국어] 출력할 값 - bfqgd가 없으면 0으로 폴백 */

	if (bfqgd)
		val = bfqgd->weight; /* [한국어] 정상적으로 존재하면 실제 설정된 weight 값 사용 */

	seq_printf(sf, "%u\n", val); /* [한국어] "N\n" 형식으로 단일 숫자 출력 */

	return 0; /* [한국어] seq_show 콜백은 항상 0 반환 관례 */
}

/*
 * [한국어]
 * bfqg_prfill_weight_device - per-device weight 값을 prfill 콜백 형식으로 출력
 *
 * @sf: 출력 대상 seq_file.
 * @pd: 현재 순회 중인 blkg의 policy data(bfq_group.pd).
 * @off: 사용되지 않음(blkcg_print_blkgs 콜백 시그니처 통일을 위한 매개변수).
 * @return: 이 blkg에 대한 출력 바이트 수(dev_weight가 0이면 아무것도 출력하지
 *          않았다는 뜻으로 0).
 *
 * bfqg->entity.dev_weight(이 특정 디바이스에 대해서만 설정된 weight,
 * 0이면 "설정 안 됨"을 의미)를 확인해, 설정되어 있을 때만
 * __blkg_prfill_u64()로 "<major>:<minor> N" 형식의 한 줄을 출력한다.
 * 이를 통해 사용자는 default weight와 다르게 설정된 디바이스만 골라
 * 볼 수 있다.
 *
 * 호출 체인:
 *   bfq_io_show_weight() → blkcg_print_blkgs() → [bfqg_prfill_weight_device] (per-blkg)
 */
static u64 bfqg_prfill_weight_device(struct seq_file *sf,
				     struct blkg_policy_data *pd, int off)
{
	struct bfq_group *bfqg = pd_to_bfqg(pd);
	/* [한국어] pd를 포함하는 bfq_group */

	if (!bfqg->entity.dev_weight)
		return 0; /* [한국어] per-device weight가 설정되지 않았으면(0) 이 디바이스는 출력하지 않음 */
	return __blkg_prfill_u64(sf, pd, bfqg->entity.dev_weight);
	/* [한국어] 설정된 per-device weight 값을 "<디바이스> N" 형식으로 출력 */
}

/*
 * [한국어]
 * bfq_io_show_weight - cgroup v2 "io.bfq.weight" read 핸들러
 *
 * @sf: 출력 대상 seq_file.
 * @v: 사용되지 않음.
 * @return: 항상 0.
 *
 * 먼저 "default N\n" 한 줄로 이 cgroup의 기본 weight를 출력한 뒤,
 * blkcg_print_blkgs()로 이 cgroup에 속한 모든 blkg(디바이스)를 순회하며
 * bfqg_prfill_weight_device()가 per-device 오버라이드가 있는 디바이스만
 * "<디바이스> N" 형식으로 추가 출력한다. 이는 io.weight/io.bfq.weight
 * cgroup v2 파일의 표준 출력 형식(default + 예외 목록)이다.
 *
 * 호출 체인:
 *   kernfs seq_show → [bfq_io_show_weight] → blkcg_to_bfqgd()/blkcg_print_blkgs()
 */
static int bfq_io_show_weight(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));
	/* [한국어] 이 파일을 읽는 현재 cgroup의 blkcg */
	struct bfq_group_data *bfqgd = blkcg_to_bfqgd(blkcg);
	/* [한국어] 이 cgroup의 BFQ per-cgroup 데이터 */

	seq_printf(sf, "default %u\n", bfqgd->weight);
	/* [한국어] 이 cgroup 전체의 기본 weight를 먼저 출력 */
	blkcg_print_blkgs(sf, blkcg, bfqg_prfill_weight_device,
			  &blkcg_policy_bfq, 0, false);
			  /* [한국어] 이 cgroup에 속한 각 blkg를 순회하며 per-device weight override를
			   * bfqg_prfill_weight_device 콜백으로 조건부 출력 (false=값 없으면 라인 생략) */
	return 0;
}

/*
 * [한국어]
 * bfq_group_set_weight - bfq_group의 entity weight를 갱신하고 재계산을 예약
 *
 * @bfqg: 대상 bfq_group.
 * @weight: 이 cgroup의 기본(default) weight 값.
 * @dev_weight: 이 특정 디바이스에 대한 per-device weight(0이면 미설정 =
 *              @weight를 그대로 사용).
 * @return: 없음(void).
 *
 * dev_weight가 0이 아니면 그것을 우선 사용하고(?:), 0이면 default
 * weight를 사용한다. entity.dev_weight 필드는 항상 원본 dev_weight
 * 값(0 포함)으로 갱신해 "이 디바이스에 override가 있는지"를 그대로
 * 기록해 둔다(bfqg_prfill_weight_device가 이 필드를 그대로 읽어 출력
 * 여부를 판단).
 * 실제 적용될 weight가 이전 new_weight와 다를 때만 entity->new_weight를
 * 갱신하고 prio_changed 플래그를 세운다(불필요한 재계산 방지). 원본
 * 주석대로, new_weight 저장과 prio_changed=1 사이에 smp_wmb()를 둬야
 * 하는 이유는: 다른 락 하에서 비동기적으로 prio_changed를 읽는 코드
 * (__bfq_entity_update_weight_prio(), bfq-wf2q.c)가 그 플래그를 봤을 때
 * new_weight의 최신 값도 함께 보이도록 메모리 순서를 보장해야 하기
 * 때문이다(플래그만 보이고 값은 예전 것을 읽는 상황을 방지).
 *
 * 호출 체인:
 *   bfq_io_set_weight_legacy()/bfq_io_set_device_weight() → [bfq_group_set_weight]
 *     → (플래그 확인 후) __bfq_entity_update_weight_prio() [bfq-wf2q.c, 다음 스케줄링 시점에 지연 실행]
 */
static void bfq_group_set_weight(struct bfq_group *bfqg, u64 weight, u64 dev_weight)
{
	weight = dev_weight ?: weight;
	/* [한국어] dev_weight가 0이 아니면(설정되어 있으면) 그 값을, 아니면 default weight를 사용 */

	bfqg->entity.dev_weight = dev_weight;
	/* [한국어] "override가 설정되어 있는지" 자체를 기록 - 0이면 override 없음을 의미 */
	/*
	 * Setting the prio_changed flag of the entity
	 * to 1 with new_weight == weight would re-set
	 * the value of the weight to its ioprio mapping.
	 * Set the flag only if necessary.
	 */
	if ((unsigned short)weight != bfqg->entity.new_weight) {
		/* [한국어] 계산된 weight가 현재 목표값(new_weight)과 실제로 다를 때만 갱신 진행 */
		bfqg->entity.new_weight = (unsigned short)weight;
		/* [한국어] weight 필드의 실제 자료형(unsigned short) 범위로 캐스팅해 저장 */
		/*
		 * Make sure that the above new value has been
		 * stored in bfqg->entity.new_weight before
		 * setting the prio_changed flag. In fact,
		 * this flag may be read asynchronously (in
		 * critical sections protected by a different
		 * lock than that held here), and finding this
		 * flag set may cause the execution of the code
		 * for updating parameters whose value may
		 * depend also on bfqg->entity.new_weight (in
		 * __bfq_entity_update_weight_prio).
		 * This barrier makes sure that the new value
		 * of bfqg->entity.new_weight is correctly
		 * seen in that code.
		 */
		smp_wmb();
		/* [한국어] 쓰기 메모리 배리어 - new_weight 저장이 prio_changed 세팅보다
		 * 먼저 다른 CPU/락 도메인에 관측되도록 순서를 강제 */
		bfqg->entity.prio_changed = 1;
		/* [한국어] 이 플래그를 본 스케줄링 코드가 다음 기회에 new_weight를 실제
		 * weight/가상시간 계산에 반영하도록 트리거 */
	}
}

/*
 * [한국어]
 * bfq_io_set_weight_legacy - cgroup v1 "blkio.bfq.weight" write 핸들러
 *   (이 cgroup의 default weight를 모든 디바이스에 일괄 적용)
 *
 * @css: write가 발생한 cgroup의 cgroup_subsys_state.
 * @cftype: 사용되지 않음(cftype.write_u64 콜백 시그니처 통일용).
 * @val: 사용자가 쓴 새 weight 값(문자열 파싱은 cgroup core가 이미 처리).
 * @return: 0=성공, -ERANGE=val이 [BFQ_MIN_WEIGHT, BFQ_MAX_WEIGHT] 범위 밖.
 *
 * 먼저 범위를 검사해 벗어나면 즉시 실패시킨다. 통과하면 이 blkcg의
 * bfq_group_data.weight를 새 값으로 갱신하고, blkcg->blkg_list(이
 * cgroup이 연결된 모든 디바이스의 blkg)를 순회하며 각 bfq_group에 대해
 * bfq_group_set_weight()를 dev_weight=0으로 호출한다. 이는 새 default를
 * 적용함과 동시에 그동안 개별 디바이스에 설정되어 있던 per-device
 * override(entity.dev_weight)까지 모두 초기화한다는 뜻이다. blkcg->lock으로
 * 이 순회 동안 blkg_list 변경(다른 디바이스가 새로 연결되는 등)과의
 * 경쟁을 막는다.
 *
 * 호출 체인:
 *   kernfs write(cftype.write_u64) → [bfq_io_set_weight_legacy]
 *     → blkcg_to_bfqgd()/bfq_group_set_weight()
 */
static int bfq_io_set_weight_legacy(struct cgroup_subsys_state *css,
				    struct cftype *cftype,
				    u64 val)
{
	struct blkcg *blkcg = css_to_blkcg(css);
	/* [한국어] write가 발생한 cgroup의 blkcg */
	struct bfq_group_data *bfqgd = blkcg_to_bfqgd(blkcg);
	/* [한국어] 갱신 대상 per-cgroup weight 저장소 */
	struct blkcg_gq *blkg; /* [한국어] blkg_list 순회용 반복자 */
	int ret = -ERANGE; /* [한국어] 기본값을 실패(-ERANGE)로 두고, 범위 검사 통과 시에만 0으로 바꿈 */

	if (val < BFQ_MIN_WEIGHT || val > BFQ_MAX_WEIGHT)
		return ret; /* [한국어] 허용 범위를 벗어난 weight는 즉시 거부(entity weight 자료형 오버플로 등 방지) */

	ret = 0; /* [한국어] 범위 검사를 통과했으므로 성공으로 전환 */
	spin_lock_irq(&blkcg->lock);
	/* [한국어] blkcg 자체의 락 - blkg_list 순회 도중 다른 CPU가 blkg를 추가/제거하는
	 * 것과 경쟁하지 않도록 보호(하드IRQ에서의 접근 가능성 때문에 irq 버전 사용) */
	bfqgd->weight = (unsigned short)val;
	/* [한국어] 이 cgroup의 새 default weight를 저장(이후 새로 생성되는 bfq_group의 초깃값으로도 쓰임) */
	hlist_for_each_entry(blkg, &blkcg->blkg_list, blkcg_node) {
		/* [한국어] 이 cgroup이 연결된 모든 디바이스의 blkg를 순회 */
		struct bfq_group *bfqg = blkg_to_bfqg(blkg);
		/* [한국어] 해당 blkg의 BFQ 전용 bfq_group(정책 미등록 디바이스면 NULL 가능) */

		if (bfqg)
			bfq_group_set_weight(bfqg, val, 0);
			/* [한국어] dev_weight=0으로 넘겨 새 default를 적용함과 동시에 이 디바이스에
			 * 있었을 per-device override(entity.dev_weight)도 함께 초기화 */
	}
	spin_unlock_irq(&blkcg->lock); /* [한국어] 락 해제 */

	return ret; /* [한국어] 0(성공) 반환 */
}

/*
 * [한국어]
 * bfq_io_set_device_weight - cgroup v1/v2 공통 per-device weight write 처리
 *
 * @of: 이 write가 발생한 kernfs_open_file(어떤 cgroupfs 파일인지 포함).
 * @buf: 사용자가 쓴 원본 문자열("<MAJ>:<MIN> N" 또는 "<MAJ>:<MIN> default").
 * @nbytes: buf의 바이트 길이.
 * @off: cftype.private 등 오프셋 정보(이 함수 자체에서는 직접 쓰이지 않음).
 * @return: 성공 시 nbytes(전체를 소비했다는 관례), 실패 시 음수 errno.
 *
 * blkg_conf_prep()으로 buf의 "<MAJ>:<MIN>" 부분을 파싱해 대상 디바이스의
 * blkg를 찾고, 나머지 부분(ctx.body)에서 숫자 또는 "default" 문자열을
 * 읽는다. 숫자면 0을 허용하지 않는데(0은 반드시 "default"로 표현해야
 * 함), 이는 사용자 입력 실수를 방지하기 위한 방어적 설계다. 유효한
 * 값이면 bfq_group_set_weight()를 호출하되, 두 번째 인자(default weight)로
 * 이 그룹의 현재 weight(bfqg->entity.weight)를 그대로 넘겨 "default는
 * 바꾸지 않고 이 디바이스만 override"함을 보장한다.
 *
 * 호출 체인:
 *   bfq_io_set_weight() (per-device 경로 판별 시) → [bfq_io_set_device_weight]
 *     → blkg_conf_prep()/bfq_group_set_weight()
 */
static ssize_t bfq_io_set_device_weight(struct kernfs_open_file *of,
					char *buf, size_t nbytes,
					loff_t off)
{
	int ret; /* [한국어] 각 단계의 성공/실패 코드(음수 errno 또는 0) */
	struct blkg_conf_ctx ctx; /* [한국어] blkg_conf_prep/exit이 관리하는 파싱 컨텍스트(대상 blkg 등) */
	/* [한국어] 이 write가 발생한 cgroup의 blkcg */
	struct blkcg *blkcg = css_to_blkcg(of_css(of));
	struct bfq_group *bfqg; /* [한국어] 파싱된 대상 디바이스의 bfq_group */
	u64 v; /* [한국어] 파싱된 weight 값(0=default로 되돌림을 의미) */

	/* [한국어] ctx를 buf 기준으로 초기화 - 이후 blkg_conf_prep가 이 버퍼를 파싱 */
	blkg_conf_init(&ctx, buf);

	/* [한국어] buf 앞부분의 "<MAJ>:<MIN>"을 파싱해 대상 gendisk/blkg를 찾고 ctx에 채움 */
	ret = blkg_conf_prep(blkcg, &blkcg_policy_bfq, &ctx);
	/* [한국어] 디바이스 지정이 잘못되었거나 blkg 준비 실패 시 즉시 정리 후 반환 */
	if (ret)
		goto out;

	/* [한국어] "<MAJ>:<MIN> N" 형식 - 나머지 본문이 순수 숫자로 파싱되는 경우 */
	if (sscanf(ctx.body, "%llu", &v) == 1) {
		/* require "default" on dfl */
		/* [한국어] 숫자 0은 허용하지 않음 - 0을 원하면 반드시 "default"라고 써야 함 */
		ret = -ERANGE;
		if (!v) // 숫자로 0을 준 경우 - cgroup v2 인터페이스에서는 "가중치 없음"을 0이 아니라 "default"라는 낱말로만 표현하게 강제한다
			goto out;
	/* [한국어] "<MAJ>:<MIN> default" 형식 - 본문이 숫자로 파싱되지 않았다면
	 * 앞뒤 공백을 떼고 정확히 "default"인지 확인한다. 이 형식은 해당
	 * 디바이스에 걸어 둔 per-device override를 지우고 cgroup 기본
	 * weight로 되돌리라는 뜻이다. */
	} else if (!strcmp(strim(ctx.body), "default")) {
		v = 0; /* [한국어] v=0은 아래에서 "override 없음"으로 해석됨 */
	} else {
		/* [한국어] 숫자도 "default"도 아니면 잘못된 입력으로 거부 */
		ret = -EINVAL;
		goto out;
	}

	/* [한국어] blkg_conf_prep가 찾아준 대상 blkg를 bfq_group으로 변환 */
	bfqg = blkg_to_bfqg(ctx.blkg);

	/* [한국어] v==0(default로 복귀)이거나 정상 범위 내의 값일 때만 적용 허용 */
	ret = -ERANGE;
	/* [한국어] 두 번째 인자로 "현재 weight"(사실상 default 역할)를 그대로 넘기고,
	 * 세 번째 인자 v를 dev_weight로 넘겨 이 디바이스만의 override를 적용/해제 */
	if (!v || (v >= BFQ_MIN_WEIGHT && v <= BFQ_MAX_WEIGHT)) {
		bfq_group_set_weight(bfqg, bfqg->entity.weight, v);
		ret = 0; /* [한국어] 적용 성공 */
	}
out:
	blkg_conf_exit(&ctx); /* [한국어] blkg_conf_prep가 잡은 참조/락 등을 정리 */
	return ret ?: nbytes; /* [한국어] 성공(ret==0)이면 관례대로 전체 바이트 수를 반환, 실패면 에러코드 */
}

/*
 * [한국어]
 * bfq_io_set_weight - cgroup v2 "io.bfq.weight" write 핸들러
 *   (입력 형식에 따라 default 갱신 또는 per-device 갱신으로 분기)
 *
 * @of: write가 발생한 kernfs_open_file.
 * @buf: 사용자 입력 문자열("N", "default N", 또는 "<MAJ>:<MIN> N"/"default").
 * @nbytes: buf 길이.
 * @off: 사용되지 않음(cftype.write 콜백 시그니처 통일용).
 * @return: 성공 시 nbytes, 실패 시 음수 errno.
 *
 * 입력이 순수 숫자("N")이거나 "default N" 형식이면 cgroup 전체 기본
 * weight를 설정하는 것으로 보고 bfq_io_set_weight_legacy()에 위임한다
 * (v1/v2가 같은 파싱 로직을 공유). 그렇지 않다면(즉 "<MAJ>:<MIN> ..."
 * 형식) per-device 설정으로 간주해 bfq_io_set_device_weight()로 넘긴다.
 * simple_strtoull()이 buf 전체를 숫자로 완전히 소비했는지(*endp=='\0')로
 * 첫 번째 경우를 판별한다.
 *
 * 호출 체인:
 *   kernfs write(cftype.write) → [bfq_io_set_weight]
 *     → bfq_io_set_weight_legacy() / bfq_io_set_device_weight()
 */
static ssize_t bfq_io_set_weight(struct kernfs_open_file *of,
				 char *buf, size_t nbytes,
				 loff_t off)
{
	char *endp; /* [한국어] simple_strtoull이 파싱을 멈춘 지점(전부 소비했으면 '\0') */
	int ret; /* [한국어] 레거시 경로 위임 시 반환값 */
	u64 v; /* [한국어] 파싱된 weight 값 */

	buf = strim(buf); /* [한국어] 입력 문자열 앞뒤 공백 제거 */

	/* "WEIGHT" or "default WEIGHT" sets the default weight */
	/* [한국어] buf를 숫자로 파싱 시도 - "<MAJ>:<MIN> ..."이면 콜론에서 파싱이 멈춰
	 * endp가 문자열 끝이 아니게 됨 */
	v = simple_strtoull(buf, &endp, 0);
	/* [한국어] buf 전체가 순수 숫자였거나("N"), "default N" 형식과 일치하면
	 * cgroup 전체 default 설정 경로 */
	if (*endp == '\0' || sscanf(buf, "default %llu", &v) == 1) {
		ret = bfq_io_set_weight_legacy(of_css(of), NULL, v);
		/* [한국어] 파싱된 v로 이 cgroup 전체의 default weight를 설정(모든 디바이스 override도 초기화됨) */
		return ret ?: nbytes; /* [한국어] 성공이면 nbytes, 실패면 에러코드 반환 */
	}

	return bfq_io_set_device_weight(of, buf, nbytes, off);
	/* [한국어] 위 두 형식에 해당하지 않으면 "<MAJ>:<MIN> ..." per-device 형식으로 간주해 위임 */
}

/*
 * [한국어]
 * bfqg_print_rwstat - 이 cgroup(비재귀) 단위 방향별(read/write) 통계를 출력
 *
 * @sf: 출력 대상 seq_file.
 * @v: 사용되지 않음.
 * @return: 항상 0.
 *
 * blkg_prfill_rwstat(blk-cgroup 코어 공용 콜백)을 사용해, seq_cft(sf)->private
 * (cftype 등록 시 offsetof(struct bfq_group, stats.xxx)로 지정된 오프셋)
 * 위치의 blkg_rwstat 필드를 방향별로 출력한다. bfq.io_service_bytes/
 * bfq.io_serviced 등 "_recursive가 붙지 않은" cgroupfs 파일들의 공통
 * 구현체다.
 *
 * 호출 체인:
 *   kernfs seq_show → [bfqg_print_rwstat] → blkcg_print_blkgs() → blkg_prfill_rwstat() (per-blkg)
 */
static int bfqg_print_rwstat(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), blkg_prfill_rwstat,
			  /* [한국어] 마지막 인자 true는 "0 값도 라인을 생략하지 않고 출력"을 의미 -
			   * rwstat은 항상 방향별 4개 카운터(read/write/sync/async 등)를 함께 보여줘야 하므로 */
			  &blkcg_policy_bfq, seq_cft(sf)->private, true);
	return 0;
}

/*
 * [한국어]
 * bfqg_prfill_rwstat_recursive - 이 blkg와 그 하위 cgroup을 모두 합산한
 *   방향별 통계를 prfill 콜백 형식으로 출력
 *
 * @sf: 출력 대상 seq_file.
 * @pd: 현재 순회 중인 blkg의 policy data.
 * @off: 대상 blkg_rwstat 필드까지의 오프셋(cftype.private).
 * @return: __blkg_prfill_rwstat()의 반환값(관례상 0).
 *
 * blkg_rwstat_recursive_sum()으로 pd가 속한 blkg 자신 + 그 모든 자손
 * blkg의 같은 오프셋 필드를 합산한 뒤 출력한다. "_recursive" 접미사가
 * 붙은 cgroupfs 파일(bfq.io_service_bytes_recursive 등)이 이 콜백을
 * 사용해, 하위 cgroup의 사용량까지 포함한 계층적 합계를 보여준다.
 *
 * 호출 체인:
 *   bfqg_print_rwstat_recursive() → blkcg_print_blkgs() → [bfqg_prfill_rwstat_recursive] (per-blkg)
 */
static u64 bfqg_prfill_rwstat_recursive(struct seq_file *sf,
					struct blkg_policy_data *pd, int off)
{
	struct blkg_rwstat_sample sum; /* [한국어] 재귀 합산 결과를 담을 임시 구조체 */

	/* [한국어] pd의 blkg부터 하위 cgroup 트리 전체를 순회하며 off 위치의 rwstat을 합산 */
	blkg_rwstat_recursive_sum(pd_to_blkg(pd), &blkcg_policy_bfq, off, &sum);
	/* [한국어] 합산 결과를 방향별로 seq_file에 출력 */
	return __blkg_prfill_rwstat(sf, pd, &sum);
}

/*
 * [한국어]
 * bfqg_print_rwstat_recursive - "_recursive" cgroupfs 파일들의 seq_show 진입점
 *
 * @sf: 출력 대상 seq_file.
 * @v: 사용되지 않음.
 * @return: 항상 0.
 *
 * bfqg_print_rwstat()과 거의 동일하지만 prfill 콜백으로
 * bfqg_prfill_rwstat_recursive()를 사용해 재귀(하위 cgroup 포함) 합계를
 * 출력한다는 점이 다르다.
 *
 * 호출 체인:
 *   kernfs seq_show → [bfqg_print_rwstat_recursive] → blkcg_print_blkgs()
 *     → bfqg_prfill_rwstat_recursive() (per-blkg)
 */
static int bfqg_print_rwstat_recursive(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  /* [한국어] true=값이 0이어도 라인 생략하지 않음(비재귀 버전과 동일 규칙) */
			  bfqg_prfill_rwstat_recursive, &blkcg_policy_bfq,
			  seq_cft(sf)->private, true);
	return 0;
}

#ifdef CONFIG_BFQ_CGROUP_DEBUG
/*
 * [한국어]
 * bfqg_print_stat - CONFIG_BFQ_CGROUP_DEBUG 전용 bfq_stat 필드의 비재귀 출력
 *
 * @sf: 출력 대상 seq_file. @v: 사용되지 않음.
 * @return: 항상 0.
 *
 * blkg_prfill_stat() 콜백으로 seq_cft(sf)->private 오프셋의 bfq_stat 값을
 * (이 cgroup 자신만, 하위 미포함) 출력한다. bfq.time/bfq.group_wait_time
 * 등 디버그 전용 파일들의 공통 구현체다.
 *
 * 호출 체인:
 *   kernfs seq_show → [bfqg_print_stat] → blkcg_print_blkgs() → blkg_prfill_stat()
 */
static int bfqg_print_stat(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), blkg_prfill_stat,
			  /* [한국어] 마지막 인자 false는 "값이 0이면 해당 blkg 라인 자체를 생략"을 의미 */
			  &blkcg_policy_bfq, seq_cft(sf)->private, false);
	return 0;
}

/*
 * [한국어]
 * bfqg_prfill_stat_recursive - 이 blkg와 모든 자손의 bfq_stat 필드를 직접 순회 합산
 *
 * @sf: 출력 대상 seq_file.
 * @pd: 현재 blkg의 policy data.
 * @off: 대상 bfq_stat 필드까지의 오프셋.
 * @return: __blkg_prfill_u64()의 반환값(관례상 0).
 *
 * bfq_stat은 aux_cnt(자식이 사라질 때 이관된 값)를 갖고 있어, 살아있는
 * 자손 blkg를 rcu 보호 하에 직접 순회하며 "현재 카운트 + 그 자신의
 * aux_cnt"를 모두 더해야 정확한 재귀 합계가 나온다(단순히 최상위의
 * aux_cnt만 보면 안 됨 - 조상 각자가 자기 자식의 몫만 이관받으므로).
 * blkg_for_each_descendant_pre()로 pd의 blkg부터 pre-order로 자손을
 * 순회하며, online이 아닌(이미 offline된) blkg는 건너뛴다(그런 blkg의
 * 값은 이미 부모의 aux_cnt에 반영되어 있으므로 중복 계산 방지).
 * queue_lock을 쥔 상태여야 하며(lockdep_assert_held), 그 안에서
 * rcu_read_lock()으로 descendant 리스트 순회의 안전성을 추가로 보장한다.
 *
 * 호출 체인:
 *   bfqg_print_stat_recursive() → blkcg_print_blkgs() → [bfqg_prfill_stat_recursive]
 *     → blkg_for_each_descendant_pre()/bfq_stat_read()
 */
static u64 bfqg_prfill_stat_recursive(struct seq_file *sf,
				      struct blkg_policy_data *pd, int off)
{
	struct blkcg_gq *blkg = pd_to_blkg(pd); /* [한국어] 합산을 시작할 최상위 blkg */
	struct blkcg_gq *pos_blkg; /* [한국어] descendant 순회의 현재 위치 */
	struct cgroup_subsys_state *pos_css; /* [한국어] blkg_for_each_descendant_pre 내부 순회용 css 커서 */
	u64 sum = 0; /* [한국어] 누적 합계 */

	/* [한국어] 호출자가 queue_lock을 쥐고 있어야 함을 런타임 검증 - descendant
	 * 트리 구조 변경(cgroup 생성/삭제)과의 경쟁 방지 전제조건 */
	lockdep_assert_held(&blkg->q->queue_lock);

	/* [한국어] blkg_for_each_descendant_pre 내부가 RCU로 blkg 트리를 순회하므로 보호 구간 진입 */
	rcu_read_lock();
	/* [한국어] blkg를 루트로 하는 서브트리를 pre-order(부모 먼저)로 순회 */
	blkg_for_each_descendant_pre(pos_blkg, pos_css, blkg) {
		struct bfq_stat *stat; /* [한국어] 이번 순회 위치의 대상 bfq_stat 필드 포인터 */

		/* [한국어] offline된 blkg는 이미 부모 aux_cnt에 흡수되었으므로 건너뜀 */
		if (!pos_blkg->online)
			continue;

		/* [한국어] 이 blkg의 policy_data 시작 주소 + off로 대상 필드 주소 계산 */
		stat = (void *)blkg_to_pd(pos_blkg, &blkcg_policy_bfq) + off;
		/* [한국어] 현재 percpu 합산값과, 이미 이 blkg가 이관받은 aux_cnt를 함께 더함 */
		sum += bfq_stat_read(stat) + atomic64_read(&stat->aux_cnt);
	}
	rcu_read_unlock(); /* [한국어] RCU 보호 구간 종료 */

	return __blkg_prfill_u64(sf, pd, sum); /* [한국어] 최종 합계를 seq_file에 출력 */
}

/*
 * [한국어]
 * bfqg_print_stat_recursive - "_recursive" 디버그 통계 cgroupfs 파일의 seq_show 진입점
 *
 * @sf: 출력 대상 seq_file. @v: 사용되지 않음.
 * @return: 항상 0.
 *
 * 호출 체인:
 *   kernfs seq_show → [bfqg_print_stat_recursive] → blkcg_print_blkgs()
 *     → bfqg_prfill_stat_recursive()
 */
static int bfqg_print_stat_recursive(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  /* [한국어] false=0이면 라인 생략(디버그 stat 계열 공통 규칙) */
			  bfqg_prfill_stat_recursive, &blkcg_policy_bfq,
			  seq_cft(sf)->private, false);
	return 0;
}

/*
 * [한국어]
 * bfqg_prfill_sectors - 이 그룹(비재귀)의 처리 바이트를 섹터 수로 변환해 출력
 *
 * @sf: 출력 대상 seq_file.
 * @pd: 현재 blkg의 policy data.
 * @off: 사용되지 않음(항상 stats.bytes 고정 사용, 시그니처 통일용 매개변수).
 * @return: __blkg_prfill_u64()의 반환값.
 *
 * bytes(방향 무관 총 바이트, blkg_rwstat_total로 read+write+... 합산)를
 * 512바이트로 나눈(>>9) "섹터 수"로 변환해 bfq.sectors 파일에 노출한다.
 * 커널 블록 계층 전통에서 섹터는 항상 512바이트 단위로 정의된다.
 *
 * 호출 체인:
 *   bfqg_print_stat_sectors() → blkcg_print_blkgs() → [bfqg_prfill_sectors]
 */
static u64 bfqg_prfill_sectors(struct seq_file *sf, struct blkg_policy_data *pd,
			       int off)
{
	struct bfq_group *bfqg = blkg_to_bfqg(pd->blkg); /* [한국어] pd가 속한 blkg로부터 bfq_group 획득 */
	/* [한국어] bytes 통계의 모든 방향(read+write 등)을 합산한 총 바이트 수 */
	u64 sum = blkg_rwstat_total(&bfqg->stats.bytes);

	/* [한국어] 512바이트 단위(섹터)로 변환(>>9 = /512)해 출력 */
	return __blkg_prfill_u64(sf, pd, sum >> 9);
}

/*
 * [한국어]
 * bfqg_print_stat_sectors - bfq.sectors(비재귀 섹터 수) 파일의 seq_show 진입점
 *
 * @sf: 출력 대상 seq_file. @v: 사용되지 않음.
 * @return: 항상 0.
 *
 * 호출 체인:
 *   kernfs seq_show → [bfqg_print_stat_sectors] → blkcg_print_blkgs() → bfqg_prfill_sectors()
 */
static int bfqg_print_stat_sectors(struct seq_file *sf, void *v)
{
	/* [한국어] 마지막 인자 false는 "값이 0인 blkg는 출력 줄 자체를 생략"을
	 * 뜻한다. 섹터 수가 0인 cgroup까지 모두 찍으면 계층이 깊을 때 출력이
	 * 무의미하게 길어지기 때문이다. 네 번째 인자 0은 prfill 콜백이 오프셋
	 * 대신 stats.bytes를 직접 읽으므로 쓰이지 않는 자리다. */
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  bfqg_prfill_sectors, &blkcg_policy_bfq, 0, false);
	return 0;
}

/*
 * [한국어]
 * bfqg_prfill_sectors_recursive - 이 그룹과 모든 자손의 처리 바이트를 재귀
 *   합산해 섹터 수로 변환·출력
 *
 * @sf: 출력 대상 seq_file.
 * @pd: 현재 blkg의 policy data.
 * @off: 사용되지 않음(bytes 필드 오프셋을 직접 offsetof로 계산해 사용).
 * @return: __blkg_prfill_u64()의 반환값.
 *
 * blkg_rwstat_recursive_sum()으로 stats.bytes 필드를 하위 cgroup까지
 * 포함해 합산한 뒤, read+write 카운트만 더해(discard 등 다른 방향은
 * 섹터 통계에서 제외) 512바이트 단위로 변환한다.
 *
 * 호출 체인:
 *   bfqg_print_stat_sectors_recursive() → blkcg_print_blkgs()
 *     → [bfqg_prfill_sectors_recursive]
 */
static u64 bfqg_prfill_sectors_recursive(struct seq_file *sf,
					 struct blkg_policy_data *pd, int off)
{
	struct blkg_rwstat_sample tmp; /* [한국어] 재귀 합산 결과를 담을 임시 구조체 */

	/* [한국어] stats.bytes 필드를 명시적 offsetof로 지정해 하위 cgroup 포함 합산 */
	blkg_rwstat_recursive_sum(pd->blkg, &blkcg_policy_bfq,
			offsetof(struct bfq_group, stats.bytes), &tmp);

	/* [한국어] read+write 두 방향만 더한 뒤 섹터 단위(>>9)로 변환해 출력 */
	return __blkg_prfill_u64(sf, pd,
		(tmp.cnt[BLKG_RWSTAT_READ] + tmp.cnt[BLKG_RWSTAT_WRITE]) >> 9);
}

/*
 * [한국어]
 * bfqg_print_stat_sectors_recursive - bfq.sectors_recursive 파일의 seq_show 진입점
 *
 * @sf: 출력 대상 seq_file. @v: 사용되지 않음.
 * @return: 항상 0.
 *
 * 호출 체인:
 *   kernfs seq_show → [bfqg_print_stat_sectors_recursive] → blkcg_print_blkgs()
 *     → bfqg_prfill_sectors_recursive()
 */
static int bfqg_print_stat_sectors_recursive(struct seq_file *sf, void *v)
{
	/* [한국어] 비재귀 버전과 같은 규칙(0이면 줄 생략)이며, prfill 콜백만
	 * 하위 cgroup까지 합산하는 recursive 버전으로 바뀐다. */
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  bfqg_prfill_sectors_recursive, &blkcg_policy_bfq, 0,
			  false);
	return 0;
}

/*
 * [한국어]
 * bfqg_prfill_avg_queue_size - 누적 표본으로부터 평균 큐 크기를 계산해 출력
 *
 * @sf: 출력 대상 seq_file.
 * @pd: 현재 blkg의 policy data.
 * @off: 사용되지 않음.
 * @return: 항상 0(이 함수는 __blkg_prfill_u64의 반환값을 버리고 직접 0을 반환).
 *
 * avg_queue_size_samples(표본 수)가 0이면(아직 한 번도 표본을 채취하지
 * 않았으면) 평균을 0으로 둔다(0으로 나누기 방지). 그렇지 않으면
 * avg_queue_size_sum(누적 합) / avg_queue_size_samples를 64비트
 * 나눗셈(div64_u64, 32비트 아키텍처에서도 64비트 나눗셈을 지원하기 위한
 * 커널 헬퍼)으로 계산해 출력한다.
 *
 * 호출 체인:
 *   bfqg_print_avg_queue_size() → blkcg_print_blkgs() → [bfqg_prfill_avg_queue_size]
 */
static u64 bfqg_prfill_avg_queue_size(struct seq_file *sf,
				      struct blkg_policy_data *pd, int off)
{
	struct bfq_group *bfqg = pd_to_bfqg(pd); /* [한국어] pd를 포함하는 bfq_group */
	/* [한국어] 지금까지 채취된 표본 수(평균의 분모) */
	u64 samples = bfq_stat_read(&bfqg->stats.avg_queue_size_samples);
	u64 v = 0; /* [한국어] 계산될 평균값 - 표본이 없으면 0 유지 */

	/* [한국어] 표본이 하나라도 있을 때만 실제 나눗셈 수행 */
	if (samples) {
		v = bfq_stat_read(&bfqg->stats.avg_queue_size_sum);
		/* [한국어] 누적된 큐 크기 합(분자) 읽기 */
		v = div64_u64(v, samples);
		/* [한국어] 64비트 나눗셈으로 평균 계산 - 32비트 CPU에서도 정확한 64/64 나눗셈 보장 */
	}
	__blkg_prfill_u64(sf, pd, v);
	/* [한국어] 계산된 평균값을 seq_file에 출력(반환값은 사용하지 않음) */
	return 0; /* [한국어] blkcg_print_blkgs가 기대하는 prfill 콜백 시그니처(u64) 규약상 값 자체는
		   * 이미 위에서 출력했으므로 여기서는 무관한 0을 반환 */
}

/* print avg_queue_size */
/*
 * [한국어]
 * bfqg_print_avg_queue_size - bfq.avg_queue_size 파일의 seq_show 진입점
 *
 * @sf: 출력 대상 seq_file. @v: 사용되지 않음.
 * @return: 항상 0.
 *
 * 호출 체인:
 *   kernfs seq_show → [bfqg_print_avg_queue_size] → blkcg_print_blkgs()
 *     → bfqg_prfill_avg_queue_size()
 */
static int bfqg_print_avg_queue_size(struct seq_file *sf, void *v)
{
	/* [한국어] 평균 큐 크기는 sum/samples 나눗셈 결과라 표본이 0인 그룹은
	 * 출력할 값 자체가 없으므로, 여기서도 false(0이면 줄 생략)를 쓴다. */
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  bfqg_prfill_avg_queue_size, &blkcg_policy_bfq,
			  0, false);
	return 0;
}
#endif /* CONFIG_BFQ_CGROUP_DEBUG */

/*
 * [한국어]
 * bfq_create_group_hierarchy - 이 디바이스에 blkcg_policy_bfq를 활성화하고
 *   root bfq_group을 얻음
 *
 * @bfqd: 디바이스 전역 struct bfq_data(elevator 초기화 중).
 * @node: 사용되지 않음(CONFIG_BFQ_GROUP_IOSCHED가 꺼진 빌드의 동명 함수와
 *        시그니처를 맞추기 위해 유지되는 매개변수).
 * @return: 이 디바이스의 root bfq_group, 정책 활성화 실패 시 NULL.
 *
 * blkcg_activate_policy()(blk-cgroup 코어)를 호출해, 이 gendisk의 기존
 * 모든 cgroup(및 향후 생성될 cgroup)에 대해 blkcg_policy_bfq의
 * pd_alloc_fn/pd_init_fn이 실행되도록 만든다. 그 결과로 이미 만들어져
 * 있는 request_queue->root_blkg에도 이제 BFQ policy data(bfq_group)가
 * 붙어 있으므로, blkg_to_bfqg()로 꺼내 root_group으로 반환한다. 이후
 * bfq_init_queue()가 이 반환값을 bfqd->root_group에 저장한다.
 *
 * 호출 체인:
 *   bfq_init_queue() [bfq-iosched.c, elevator 등록 시] → [bfq_create_group_hierarchy]
 *     → blkcg_activate_policy()/blkg_to_bfqg()
 */
struct bfq_group *bfq_create_group_hierarchy(struct bfq_data *bfqd, int node)
{
	int ret; /* [한국어] blkcg_activate_policy()의 반환값(0=성공, 음수=실패) */

	/* [한국어] 이 gendisk의 모든 cgroup에 대해 BFQ 정책의 pd_alloc/init 콜백을 실행시킴 */
	ret = blkcg_activate_policy(bfqd->queue->disk, &blkcg_policy_bfq);
	/* [한국어] 정책 활성화 실패(메모리 부족 등) 시 그룹 계층을 만들 수 없으므로 NULL 반환 */
	if (ret)
		return NULL;

	/* [한국어] 활성화가 끝난 root_blkg에서 이제 유효해진 bfq_group을 꺼내 반환 */
	return blkg_to_bfqg(bfqd->queue->root_blkg);
}

/*
 * struct blkcg_policy blkcg_policy_bfq - BFQ의 blk-cgroup 정책 등록 구조체
 */
/*
 * [한국어] blkcg_policy_bfq - BFQ가 blk-cgroup 코어에 자신을 등록하는 정책
 *   디스크립터(콜백 함수 포인터 테이블)
 *
 * blkcg_policy_register(&blkcg_policy_bfq)(bfq-iosched.c의 모듈 초기화
 * 코드에서 호출)로 blk-cgroup 코어의 전역 blkcg_policy[] 테이블에
 * 등록된다. 이후 cgroup 라이프사이클의 각 단계(할당/초기화/오프라인/
 * 해제/통계리셋)마다 blk-cgroup 코어가 아래 함수 포인터들을 호출해
 * BFQ가 자신의 bfq_group/bfq_group_data 상태를 최신으로 유지하게 한다.
 * dfl_cftypes/legacy_cftypes는 각각 cgroup v2/v1 인터페이스의 cgroupfs
 * 파일 목록(struct cftype 배열)이다.
 *
 * 설정자: 이 파일의 정적 초기화(컴파일 타임 상수).
 * 읽는 자: block/blk-cgroup.c의 blkcg_policy_register()/blkg_create()/
 *   blkg_destroy()/blkcg_css_alloc() 등이 등록된 함수 포인터를 호출.
 * 값 범위: 모듈 로드 후 불변(각 필드는 이 파일에 정의된 static 함수 주소).
 * 동기화: blkcg_pol_register_mutex(등록 시점)로 보호, 이후는 읽기 전용.
 */
struct blkcg_policy blkcg_policy_bfq = {
	.dfl_cftypes		= bfq_blkg_files,
	/* [한국어] cgroup v2(io.bfq.weight 등)에서 노출할 cftype 배열 */
	.legacy_cftypes		= bfq_blkcg_legacy_files,
	/* [한국어] cgroup v1(blkio.bfq.weight/blkio.bfq.io_service_bytes 등)에서 노출할 cftype 배열 */

	.cpd_alloc_fn		= bfq_cpd_alloc,
	/* [한국어] cgroup 생성 시 per-cgroup weight 저장소(bfq_group_data) 할당 */
	.cpd_free_fn		= bfq_cpd_free,
	/* [한국어] cgroup 해제 시 bfq_group_data 메모리 반환 */

	.pd_alloc_fn		= bfq_pd_alloc,
	/* [한국어] (디바이스, cgroup) 쌍이 필요해질 때 bfq_group 메모리 할당 */
	.pd_init_fn		= bfq_pd_init,
	/* [한국어] 할당된 bfq_group의 entity/weight/sched_data 등 실제 초기화 */
	.pd_offline_fn		= bfq_pd_offline,
	/* [한국어] cgroup offline 시 active/idle 큐를 root_group으로 재배치하고 통계 이관 */
	.pd_free_fn		= bfq_pd_free,
	/* [한국어] bfq_group의 통계 자료구조 해제 및 참조 반환(최종 메모리 해제 트리거 가능) */
	.pd_reset_stats_fn	= bfq_pd_reset_stats,
	/* [한국어] cgroupfs 통계 리셋 요청 시 누적 통계를 0으로 초기화 */
};

/*
 * [한국어] bfq_blkcg_legacy_files[] - cgroup v1(legacy hierarchy)에서 노출되는
 *   "blkio.bfq.*" 파일들의 정의 테이블
 *
 * struct cftype(cgroup core 정의)의 각 항목은 하나의 cgroupfs 파일에
 * 대응한다. 주요 필드 의미:
 *   - name: cgroupfs 상의 파일명("blkio." 접두사는 cgroup core가 자동으로 붙임).
 *   - flags: CFTYPE_NOT_ON_ROOT 등 - 루트 cgroup에는 파일을 만들지 않음 등의 속성.
 *   - private: seq_show/write 콜백에 전달될 offsetof(struct bfq_group, ...) 값 -
 *     같은 콜백 함수를 여러 통계 필드에 재사용할 수 있게 해주는 매개변수.
 *   - seq_show/write/write_u64: 이 파일을 읽거나 쓸 때 호출될 콜백 함수.
 * 마지막 배열 원소는 항상 이름이 없는 빈 { }로, cgroup core가 이를
 * 배열의 끝(sentinel)으로 인식한다. 이 배열은 blkcg_policy_bfq.legacy_cftypes에
 * 연결되어 blkcg_policy_register() 시점에 cgroup v1 계층에 등록된다.
 */
struct cftype bfq_blkcg_legacy_files[] = {
	{
		/* [한국어] 이 cgroup의 기본(default) I/O weight를 읽고 쓰는 파일 -
		 * read: bfq_io_show_weight_legacy(), write: bfq_io_set_weight_legacy() */
		.name = "bfq.weight",
		.flags = CFTYPE_NOT_ON_ROOT,
		/* [한국어] 루트 cgroup은 weight 개념이 없으므로(항상 최상위 우선) 이 파일을 만들지 않음 */
		.seq_show = bfq_io_show_weight_legacy,
		.write_u64 = bfq_io_set_weight_legacy,
		/* [한국어] write_u64는 cgroup core가 문자열을 u64로 파싱한 뒤 넘겨주는 콜백 형태 */
	},
	{
		.name = "bfq.weight_device",
		/* [한국어] 특정 디바이스(major:minor)에 대한 weight override를 읽고 쓰는 파일 */
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = bfq_io_show_weight,
		/* [한국어] show 콜백은 v2와 동일한 bfq_io_show_weight()를 재사용(default+디바이스별 override 출력) */
		.write = bfq_io_set_weight,
		/* [한국어] write는 raw 문자열을 그대로 받는 콜백 - "<MAJ>:<MIN> N|default" 파싱은 콜백 내부에서 처리 */
	},

	/* statistics, covers only the tasks in the bfqg */
	{
		.name = "bfq.io_service_bytes",
		/* [한국어] 이 cgroup(비재귀) 자신의 방향별 누적 처리 바이트 수 */
		.private = offsetof(struct bfq_group, stats.bytes),
		/* [한국어] bfqg_print_rwstat이 이 오프셋으로 stats.bytes 필드를 찾음 */
		.seq_show = bfqg_print_rwstat,
	},
	{
		.name = "bfq.io_serviced",
		/* [한국어] 이 cgroup(비재귀)의 방향별 누적 완료 개수(IOPS 산출용) */
		.private = offsetof(struct bfq_group, stats.ios),
		.seq_show = bfqg_print_rwstat,
	},
#ifdef CONFIG_BFQ_CGROUP_DEBUG
	{
		.name = "bfq.time",
		/* [한국어] (디버그) 이 그룹이 디스크를 사용한 총 시간 */
		.private = offsetof(struct bfq_group, stats.time),
		.seq_show = bfqg_print_stat,
	},
	{
		.name = "bfq.sectors",
		/* [한국어] (디버그) 이 그룹이 처리한 섹터 수(bfqg_prfill_sectors가 bytes에서 변환) -
		 * private을 쓰지 않고 콜백 자체가 항상 stats.bytes를 참조하도록 고정됨 */
		.seq_show = bfqg_print_stat_sectors,
	},
	{
		.name = "bfq.io_service_time",
		/* [한국어] (디버그) 디바이스가 실제로 request를 처리하는 데 걸린 시간 */
		.private = offsetof(struct bfq_group, stats.service_time),
		.seq_show = bfqg_print_rwstat,
	},
	{
		.name = "bfq.io_wait_time",
		/* [한국어] (디버그) request가 BFQ 스케줄러 큐에서 대기한 시간 */
		.private = offsetof(struct bfq_group, stats.wait_time),
		.seq_show = bfqg_print_rwstat,
	},
	{
		.name = "bfq.io_merged",
		/* [한국어] (디버그) bio-merge로 다른 request에 흡수된 IO 개수 */
		.private = offsetof(struct bfq_group, stats.merged),
		.seq_show = bfqg_print_rwstat,
	},
	{
		.name = "bfq.io_queued",
		/* [한국어] (디버그) 현재 이 그룹에 큐잉되어 있는 IO 개수 */
		.private = offsetof(struct bfq_group, stats.queued),
		.seq_show = bfqg_print_rwstat,
	},
#endif /* CONFIG_BFQ_CGROUP_DEBUG */

	/* the same statistics which cover the bfqg and its descendants */
	{
		.name = "bfq.io_service_bytes_recursive",
		/* [한국어] 위 io_service_bytes와 동일 필드를 하위 cgroup까지 포함해 재귀 합산 출력 */
		.private = offsetof(struct bfq_group, stats.bytes),
		.seq_show = bfqg_print_rwstat_recursive,
	},
	{
		.name = "bfq.io_serviced_recursive",
		/* [한국어] io_serviced의 재귀(하위 cgroup 포함) 버전 */
		.private = offsetof(struct bfq_group, stats.ios),
		.seq_show = bfqg_print_rwstat_recursive,
	},
#ifdef CONFIG_BFQ_CGROUP_DEBUG
	{
		.name = "bfq.time_recursive",
		/* [한국어] bfq.time의 재귀 버전 */
		.private = offsetof(struct bfq_group, stats.time),
		.seq_show = bfqg_print_stat_recursive,
	},
	{
		.name = "bfq.sectors_recursive",
		/* [한국어] bfq.sectors의 재귀 버전(bfqg_prfill_sectors_recursive가 하위 포함 합산) */
		.seq_show = bfqg_print_stat_sectors_recursive,
	},
	{
		.name = "bfq.io_service_time_recursive",
		/* [한국어] bfq.io_service_time의 재귀 버전 */
		.private = offsetof(struct bfq_group, stats.service_time),
		.seq_show = bfqg_print_rwstat_recursive,
	},
	{
		.name = "bfq.io_wait_time_recursive",
		/* [한국어] bfq.io_wait_time의 재귀 버전 */
		.private = offsetof(struct bfq_group, stats.wait_time),
		.seq_show = bfqg_print_rwstat_recursive,
	},
	{
		.name = "bfq.io_merged_recursive",
		/* [한국어] bfq.io_merged의 재귀 버전 */
		.private = offsetof(struct bfq_group, stats.merged),
		.seq_show = bfqg_print_rwstat_recursive,
	},
	{
		.name = "bfq.io_queued_recursive",
		/* [한국어] bfq.io_queued의 재귀 버전 */
		.private = offsetof(struct bfq_group, stats.queued),
		.seq_show = bfqg_print_rwstat_recursive,
	},
	{
		.name = "bfq.avg_queue_size",
		/* [한국어] (디버그) 평균 큐 크기(avg_queue_size_sum/avg_queue_size_samples) - 재귀 개념 없음 */
		.seq_show = bfqg_print_avg_queue_size,
	},
	{
		.name = "bfq.group_wait_time",
		/* [한국어] (디버그) 그룹이 서비스를 받기까지 기다린 누적 시간 */
		.private = offsetof(struct bfq_group, stats.group_wait_time),
		.seq_show = bfqg_print_stat,
	},
	{
		.name = "bfq.idle_time",
		/* [한국어] (디버그) BFQ가 이 그룹을 위해 device idling한 누적 시간 */
		.private = offsetof(struct bfq_group, stats.idle_time),
		.seq_show = bfqg_print_stat,
	},
	{
		.name = "bfq.empty_time",
		/* [한국어] (디버그) 이 그룹에 dispatch 후보가 없었던 누적 시간 */
		.private = offsetof(struct bfq_group, stats.empty_time),
		.seq_show = bfqg_print_stat,
	},
	{
		.name = "bfq.dequeue",
		/* [한국어] (디버그) 이 그룹의 entity가 service tree에서 제거된 횟수 */
		.private = offsetof(struct bfq_group, stats.dequeue),
		.seq_show = bfqg_print_stat,
	},
#endif	/* CONFIG_BFQ_CGROUP_DEBUG */
	{ }	/* terminate */
	/* [한국어] cgroup core가 배열의 끝을 인식하는 sentinel(이름 없는 빈 항목) */
};

/*
 * [한국어] bfq_blkg_files[] - cgroup v2(unified hierarchy)에서 노출되는
 *   "io.bfq.*" 파일 정의 테이블
 *
 * v2는 v1과 달리 통계 파일들이 io.stat이라는 공통 파일(blk-cgroup.c가
 * 관리)로 통합되어 있어, BFQ 고유의 v2 파일은 weight 하나뿐이다.
 * blkcg_policy_bfq.dfl_cftypes에 연결된다.
 */
struct cftype bfq_blkg_files[] = {
	{
		/* [한국어] cgroup v2의 "io.bfq.weight" 파일 - default+per-device weight를 하나의
		 * 파일에서 함께 다룸(bfq_io_show_weight/bfq_io_set_weight) */
		.name = "bfq.weight",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = bfq_io_show_weight,
		.write = bfq_io_set_weight,
		/* [한국어] write 콜백이 입력 형식을 보고 default/per-device 설정을 자동 판별(bfq_io_set_weight 참고) */
	},
	{} /* terminate */
	/* [한국어] 배열 끝을 나타내는 sentinel */
};

#else	/* CONFIG_BFQ_GROUP_IOSCHED */
/* [한국어] CONFIG_BFQ_GROUP_IOSCHED가 꺼진 커널(cgroup 기반 I/O 격리 미지원
 * 빌드)에서는 위에서 정의한 모든 cgroup 관리 로직이 통째로 컴파일되지
 * 않는다. 대신 여기서 같은 이름의 훨씬 단순한 함수들을 정의해, 이 파일
 * 밖(bfq-iosched.c 등)의 호출부가 CONFIG 분기 없이 동일한 함수 이름을
 * 그대로 호출할 수 있게 한다. 이 빌드에서는 "그룹"이 디바이스마다 단
 * 하나(root_group)뿐이므로, 모든 이동/조회 함수가 사실상 자명한 값을
 * 반환하거나 아무 일도 하지 않는다. */

/*
 * [한국어]
 * bfq_bfqq_move (cgroup 미지원 버전) - 이동할 다른 그룹이 없으므로 no-op
 *
 * @bfqd/@bfqq/@bfqg: 사용되지 않음. @return: 없음(void).
 *
 * cgroup 지원이 없으므로 모든 bfq_queue는 이미 유일한 root_group에
 * 속해 있어 "이동"이라는 개념 자체가 성립하지 않는다.
 */
void bfq_bfqq_move(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		   struct bfq_group *bfqg) {} /* [한국어] 본문 없음 - 이동할 필요 자체가 없음 */

/*
 * [한국어]
 * bfq_init_entity (cgroup 미지원 버전) - entity를 유일한 root_group에 연결
 *
 * @entity: 초기화할 bfq_entity.
 * @bfqg: 항상 디바이스의 유일한 root_group.
 * @return: 없음(void).
 *
 * CONFIG_BFQ_GROUP_IOSCHED 버전과 달리 bfqg_and_blkg_get() 호출이 없다 -
 * blkg 자체가 이 빌드에서는 BFQ와 무관하므로 참조 관리가 불필요하다.
 *
 * 호출 체인:
 *   bfq_get_queue() 등 → [bfq_init_entity]
 */
void bfq_init_entity(struct bfq_entity *entity, struct bfq_group *bfqg)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);
	/* [한국어] entity가 leaf(bfq_queue)라면 그 bfq_queue를, 아니면 NULL을 반환 */

	entity->weight = entity->new_weight; /* [한국어] weight를 최신 목표값으로 확정 - WFQ 계산의 입력값 */
	entity->orig_weight = entity->new_weight; /* [한국어] weight-raising 이전 원본 weight도 동일하게 갱신 */
	if (bfqq) {
		/* [한국어] leaf entity라면 ioprio도 함께 확정 */
		bfqq->ioprio = bfqq->new_ioprio; /* [한국어] 사용 중인 ioprio를 최신 목표값으로 확정 */
		bfqq->ioprio_class = bfqq->new_ioprio_class; /* [한국어] ioprio 클래스도 최신 목표값으로 확정 */
	}
	entity->sched_data = &bfqg->sched_data; /* [한국어] 유일한 root_group의 서비스 트리에 편입 */
}

/*
 * [한국어]
 * bfq_bic_update_cgroup (cgroup 미지원 버전) - no-op
 * @bic/@bio: 사용되지 않음. @return: 없음(void).
 * cgroup 자체가 없으므로 변경을 감지·반영할 대상이 없다.
 */
void bfq_bic_update_cgroup(struct bfq_io_cq *bic, struct bio *bio) {} /* [한국어] 본문 없음 */

/*
 * [한국어]
 * bfq_end_wr_async (cgroup 미지원 버전) - root_group의 async 큐만 처리
 * @bfqd: 디바이스 전역 struct bfq_data. @return: 없음(void).
 * 그룹이 root_group 하나뿐이므로 blkg_list 순회 없이 바로 처리한다.
 */
void bfq_end_wr_async(struct bfq_data *bfqd)
{
	bfq_end_wr_async_queues(bfqd, bfqd->root_group);
	/* [한국어] 유일한 root_group의 async 큐들에 대해 WR 상태 종료 적용 */
}

/*
 * [한국어]
 * bfq_bio_bfqg (cgroup 미지원 버전) - 항상 root_group 반환
 * @bfqd: 디바이스 전역 struct bfq_data. @bio: 사용되지 않음.
 * @return: bfqd->root_group(무조건).
 * cgroup 판별 로직 자체가 없으므로 모든 bio가 유일한 그룹으로 라우팅된다.
 */
struct bfq_group *bfq_bio_bfqg(struct bfq_data *bfqd, struct bio *bio)
{
	return bfqd->root_group; /* [한국어] 유일한 그룹이므로 무조건 이것을 반환 */
}

/*
 * [한국어]
 * bfqq_group (cgroup 미지원 버전) - 항상 root_group 반환
 * @bfqq: 대상 bfq_queue. @return: bfqq->bfqd->root_group(무조건).
 */
struct bfq_group *bfqq_group(struct bfq_queue *bfqq)
{
	return bfqq->bfqd->root_group; /* [한국어] 모든 bfqq가 항상 이 유일한 그룹에 속함 */
}

/*
 * [한국어]
 * bfqg_and_blkg_put (cgroup 미지원 버전) - no-op
 * @bfqg: 사용되지 않음. @return: 없음(void).
 * blkg 참조 관리 자체가 이 빌드와 무관하므로 아무 것도 하지 않는다.
 */
void bfqg_and_blkg_put(struct bfq_group *bfqg) {} /* [한국어] 본문 없음 */

/*
 * [한국어]
 * bfq_create_group_hierarchy (cgroup 미지원 버전) - root_group 하나만 직접 할당
 *
 * @bfqd: 사용되지 않음(CONFIG_BFQ_GROUP_IOSCHED 버전과 시그니처 통일용).
 * @node: root_group을 할당할 NUMA 노드.
 * @return: 새로 할당된 root_group, 메모리 부족 시 NULL.
 *
 * blk-cgroup 정책 활성화 없이, kmalloc_node()로 struct bfq_group(cgroup
 * 미지원 버전의 축소판, bfq-iosched.h 참고) 하나만 직접 할당하고 0으로
 * 채운(__GFP_ZERO) 뒤 서비스 트리를 초기화한다.
 *
 * 호출 체인:
 *   bfq_init_queue() [bfq-iosched.c] → [bfq_create_group_hierarchy]
 */
struct bfq_group *bfq_create_group_hierarchy(struct bfq_data *bfqd, int node)
{
	struct bfq_group *bfqg; /* [한국어] 새로 할당할 유일한 root_group */
	int i; /* [한국어] BFQ_IOPRIO_CLASSES 루프 인덱스 */

	/* [한국어] node에 지역성을 갖도록 할당하고 __GFP_ZERO로 0 초기화(kzalloc_node와 동등) */
	bfqg = kmalloc_node(sizeof(*bfqg), GFP_KERNEL | __GFP_ZERO, node);
	/* [한국어] 메모리 부족 시 elevator 초기화 자체가 실패하도록 NULL 반환 */
	if (!bfqg)
		return NULL;

	/* [한국어] RT/BE/IDLE 각 클래스의 서비스 트리를 초기화 */
	for (i = 0; i < BFQ_IOPRIO_CLASSES; i++)
		bfqg->sched_data.service_tree[i] = BFQ_SERVICE_TREE_INIT;
		/* [한국어] 빈 active/idle rb-tree, vtime=0 등으로 이루어진 초기 상태 대입 */

	return bfqg; /* [한국어] 초기화된 root_group을 elevator 초기화 코드로 반환 */
}
#endif	/* CONFIG_BFQ_GROUP_IOSCHED */

/*
 * [한국어] 파일 하단 보충 요약
 *
 * 위 4섹션(=== 파일의 역할 === 등)에서 설명한 내용의 요점만 다시 정리하면:
 * - 이 파일은 blk-cgroup 정책 인터페이스(struct blkcg_policy)를 BFQ에 맞춰
 *   구현한 접착 코드로, cgroup 계층을 BFQ의 H-WF2Q+ 스케줄링 트리(bfq_group/
 *   bfq_entity)에 매핑한다.
 * - bfq_group.entity.weight가 B-WF2Q+ 가상 시간 계산의 입력이 되어, 결과적으로
 *   bfq_dispatch_request() → blk-mq → nvme_queue_rq() → doorbell 경로에서
 *   어느 cgroup의 I/O가 더 자주/빨리 선택되는지를 결정한다.
 * - cgroup 생성(cpd/pd alloc+init) → 이동(bfq_bic_update_cgroup/bfq_bfqq_move)
 *   → 오프라인(bfq_pd_offline, active/idle 큐 root_group 재배치 + 통계 이관)
 *   → 해제(bfq_pd_free)의 전체 생명주기를 이 파일이 책임진다.
 * - CONFIG_BFQ_CGROUP_DEBUG가 켜지면 wait_time/service_time/idle_time/
 *   empty_time 등 세부 지연 통계를 cgroup 단위로 노출해 디버깅을 돕는다.
 * - CONFIG_BFQ_GROUP_IOSCHED가 꺼지면 파일 하단의 단순화된 버전이 대신
 *   컴파일되어, 디바이스마다 단 하나의 root_group만 존재하는 것처럼 동작한다.
 */
