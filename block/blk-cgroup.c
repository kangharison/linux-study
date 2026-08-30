// SPDX-License-Identifier: GPL-2.0
/*
 * Common Block IO controller cgroup interface
 *
 * Based on ideas and code from CFQ, CFS and BFQ:
 * Copyright (C) 2003 Jens Axboe <axboe@kernel.dk>
 *
 * Copyright (C) 2008 Fabio Checconi <fabio@gandalf.sssup.it>
 *		      Paolo Valente <paolo.valente@unimore.it>
 *
 * Copyright (C) 2009 Vivek Goyal <vgoyal@redhat.com>
 * 	              Nauman Rafique <nauman@google.com>
 *
 * For policy-specific per-blkcg data:
 * Copyright (C) 2015 Paolo Valente <paolo.valente@unimore.it>
 *                    Arianna Avanzini <avanzini.arianna@gmail.com>
 */
/*
 * [한국어 설명] 블록 cgroup 공통 제어 인프라 (blk-cgroup.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 block layer 의 cgroup(control group) 통합 "공통 기반"을 구현한다.
 * 개별 IO 제어 정책(blk-throttle, iocost, iolatency, BFQ, blk-ioprio)이 아니라,
 * 그 정책들이 공통으로 올라탈 뼈대만 제공한다. 구체적으로 세 가지다.
 * (1) blkg(struct blkcg_gq) 라이프사이클: cgroup 하나와 블록 장치 큐 하나의
 *     교차점을 나타내는 객체를 만들고(blkg_create), 찾고(blkg_lookup), 지우고
 *     (blkg_destroy), 지연 해제(blkg_free_workfn)한다.
 * (2) 정책 등록/활성화 프레임워크: blkcg_policy_register() 가 정책마다 plid 를
 *     배정하고, blkcg_activate_policy() 가 그 장치의 모든 blkg 에 정책 전용
 *     데이터(pd)를 붙인다. 정책 코드는 blkg->pd[plid] 만 보면 된다.
 * (3) per-cgroup IO 통계: blk_cgroup_bio_start() 가 per-cpu 로 바이트/건수를
 *     누적하고, cgroup rstat 프레임워크가 flush 를 요청할 때
 *     __blkcg_rstat_flush() 가 상위 cgroup 으로 전파하여 io.stat 로 출력된다.
 * 여기에 더해 cgroup 서브시스템 콜백(blkcg_css_alloc/online/offline/free)과
 * cgroupfs 파일 파싱 헬퍼(blkg_conf_*), delay 기반 태스크 throttle 훅
 * (blkcg_maybe_throttle_current)도 이 파일이 담당한다.
 *
 * 이 파일에는 장치 종류에 의존하는 코드가 전혀 없다. NVMe/SCSI/virtio/loop 등
 * 어떤 드라이버든 request_queue 를 가진 gendisk 라면 동일하게 동작한다.
 * (이 파일의 코드에는 nvme 관련 식별자가 하나도 없다 — grep 으로 확인함.)
 *
 * === 전체 아키텍처에서의 위치 ===
 * blkcg 가 IO 경로에 개입하는 유일한 지점은 bio 가 submit_bio() 로 들어올 때다.
 *
 * 호출 체인 (bio 에 cgroup 을 붙이는 시점):
 *   submit_bio() → __submit_bio_noacct()/blk_mq_submit_bio() 이전 단계에서
 *   bio_associate_blkg() [이 파일] → blkg_lookup_create() [이 파일]
 *     → bio->bi_blkg 설정 → 이후 정책(blk-throttle 등)이 이 bi_blkg 를 본다
 *
 * 호출 체인 (IO 통계):
 *   submit_bio_noacct() → blk_cgroup_bio_start() [이 파일]
 *     → blkg->iostat_cpu 갱신 + blkcg->lhead(per-cpu lockless list) 등록
 *     → cgroup rstat flush 시 blkcg_rstat_flush() → __blkcg_rstat_flush() [이 파일]
 *     → blkg->iostat.cur → blkcg_print_stat() → cgroupfs io.stat
 *
 * 호출 체인 (정책 등록/활성화):
 *   blk_throtl_init()/iocost/iolatency/bfq 모듈 init
 *     → blkcg_policy_register() [이 파일] : pol->plid 배정
 *   디스크에 정책이 처음 필요해질 때
 *     → blkcg_activate_policy() [이 파일] : 그 disk 의 모든 blkg 에 pd 부착
 *
 * 실행 컨텍스트: 대부분 프로세스 컨텍스트(submit_bio 경로, cgroupfs write,
 * cgroup 콜백)이며, 통계 갱신 경로는 preempt 를 끈 상태에서 per-cpu 로 동작한다.
 * blkg 해제는 RCU 콜백과 workqueue(kworker)로 넘어간다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - block/blk-cgroup.h : blkcg, blkcg_gq, blkg_policy_data, blkcg_policy 정의
 *   - block/blk-throttle.h, block/blk-ioprio.h : 내장 정책의 init/exit 훅
 *   - block/blk.h : block layer 내부 공통 헬퍼(bdev_get_queue 등)
 *   - kernel/cgroup/ : cgroup_subsys 콜백과 rstat(css_rstat_updated/flush) 프레임워크
 *   - include/linux/percpu-refcount.h : blkg->refcnt 의 percpu_ref 구현
 *   - include/linux/radix-tree.h : blkcg->blkg_tree 색인
 *
 * 의존받는 모듈:
 *   - block/blk-throttle.c, block/blk-iocost.c, block/blk-iolatency.c,
 *     block/bfq-cgroup.c, block/blk-ioprio.c : 모두 blkcg_policy 로 등록되어
 *     blkg->pd[plid] 슬롯 위에서 동작한다
 *   - block/bio.c : bio_associate_blkg(), bio_clone_blkg_association() 호출
 *   - block/genhd.c : blkcg_init_disk()/blkcg_exit_disk() 호출
 *   - mm/page-writeback.c, fs/fs-writeback.c : cgroup writeback(cgwb) 리스트 공유
 *
 * 데이터 흐름:
 *   bio → bi_blkg(blkcg_gq) → blkg->iostat_cpu(per-cpu) → blkcg->lhead(llist)
 *     → __blkcg_rstat_flush → blkg->iostat.cur(전역) → io.stat 출력
 *   cgroupfs write("<major>:<minor> <값>") → blkg_conf_prep() → 정책 pd 갱신
 *
 * === 주요 함수/구조체 요약 ===
 * blkg_lookup()/blkg_lookup_create() - (blkcg, disk->queue) 쌍의 blkg 조회/생성
 * blkg_alloc()/blkg_create()/blkg_destroy()/blkg_free() - blkg 라이프사이클
 * blkcg_policy_register()/unregister() - 정책 전역 등록, pol->plid 배정
 * blkcg_activate_policy()/deactivate_policy() - 한 disk 의 blkg 들에 pd 부착/제거
 * blk_cgroup_bio_start() - bio 제출 시 per-cpu 통계 누적 + llist 등록
 * __blkcg_rstat_flush() - per-cpu 통계를 blkg->iostat.cur 와 부모로 전파
 * blkcg_print_blkgs() - 정책의 seq_file 출력 공통 루프(정책별 prfill 콜백 호출)
 * blkg_conf_prep()/blkg_conf_exit() - cgroupfs 설정 입력 파싱과 정리
 * bio_associate_blkg() - bio 에 현재 태스크의 blkg 를 결합
 * blkcg_maybe_throttle_current() - 누적된 delay_nsec 만큼 유저 복귀 시 태스크 지연
 *
 * 핵심 자료구조 (정의는 blk-cgroup.h):
 *   struct blkcg      : cgroup 하나의 블록 서브시스템 상태. blkg_tree(radix)로
 *                       (queue id → blkg) 색인, blkg_list 로 소유 blkg 나열.
 *   struct blkcg_gq   : (blkcg, request_queue) 쌍마다 정확히 하나. pd[] 에 정책
 *                       데이터, iostat_cpu 에 통계, refcnt(percpu_ref)로 수명 관리.
 *   struct blkg_policy_data : 정책 하나가 blkg 하나에 붙이는 데이터의 공통 헤더.
 *   struct blkcg_policy     : 정책이 구현해야 할 콜백 모음. blkcg_policy[plid] 에 등록.
 */

#include <linux/ioprio.h>
/* [한국어] IOPRIO_PRIO_* 매크로. blk-ioprio 정책과 bio->bi_ioprio 처리를 위해 필요. */
#include <linux/kdev_t.h>
/* [한국어] MAJOR()/MINOR()/MKDEV(). cgroupfs 입력이 "<major>:<minor> <값>" 형식이라
 *          blkg_conf_prep() 에서 장치 번호를 파싱·조립하는 데 쓴다. */
#include <linux/module.h>
/* [한국어] module_param(blkcg_debug_stats,...) 과 EXPORT_SYMBOL_GPL() 을 위해 필요. */
#include <linux/sched/signal.h>
/* [한국어] blkcg_maybe_throttle_blkg() 가 fatal_signal_pending(current) 로
 *          치명 시그널을 받은 태스크는 delay 대기를 중단시키기 위해 필요. */
#include <linux/err.h>
/* [한국어] ERR_PTR()/PTR_ERR()/IS_ERR(). blkg_lookup_create() 등이 실패를
 *          포인터에 인코딩해 돌려주므로 필수. */
#include <linux/blkdev.h>
/* [한국어] struct request_queue, struct gendisk, struct block_device 정의.
 *          blkg 가 (blkcg, request_queue) 쌍이므로 큐 정의가 필요하다. */
#include <linux/backing-dev.h>
/* [한국어] blkcg_fill_root_iostats() 가 bdi 하위 장치를 순회할 때, 그리고
 *          cgroup writeback 연동에서 backing_dev_info 를 참조한다. */
#include <linux/slab.h>
/* [한국어] kzalloc/kfree. blkg, blkcg, pd(policy data) 할당에 사용. */
#include <linux/delay.h>
/* [한국어] blkcg_destroy_blkgs() 가 queue_lock 획득 실패 시 cpu_relax 대신
 *          잠깐 쉬어가는 등 지연 루프에 사용. */
#include <linux/wait_bit.h>
/* [한국어] blkcg_maybe_throttle_blkg() 의 대기 관련 헬퍼. */
#include <linux/atomic.h>
/* [한국어] atomic64_* (blkg->delay_nsec, delay_start), atomic_* (use_delay,
 *          congestion_count) 원자 연산에 필요. */
#include <linux/ctype.h>
/* [한국어] blkg_conf_prep() 이 입력 문자열에서 isspace() 로 토큰을 나눌 때 사용. */
#include <linux/resume_user_mode.h>
/* [한국어] set_notify_resume(). 유저 공간 복귀 직전에
 *          blkcg_maybe_throttle_current() 를 호출하도록 태스크에 표시한다. */
#include <linux/psi.h>
/* [한국어] psi_memstall_enter/leave. use_memdelay 로 지연될 때 그 시간을
 *          메모리 stall 로 계상해 PSI 압력 지표에 반영한다. */
#include <linux/part_stat.h>
/* [한국어] part_stat_read_all(). root cgroup 통계는 blkg 가 아니라 디스크
 *          파티션 통계에서 직접 채우므로(blkcg_fill_root_iostats) 필요. */
#include "blk.h"
/* [한국어] block layer 내부 전용 헬퍼(bdev_get_queue, blk_queue_* 등). */
#include "blk-cgroup.h"
/* [한국어] 이 파일의 짝이 되는 헤더. blkcg/blkcg_gq/blkg_policy_data/
 *          blkcg_policy 구조체와 blkg_lookup() 같은 인라인 헬퍼가 여기 있다. */
#include "blk-ioprio.h"
/* [한국어] blk_ioprio_init()/exit() 선언. blkcg_init_disk() 가 디스크마다
 *          내장 정책을 켤 때 호출한다. */
#include "blk-throttle.h"
/* [한국어] blk_throtl_init()/exit()/cancel_bios() 선언. 위와 같은 이유. */

/* [한국어] 전방 선언: __blkcg_rstat_flush() 는 파일 아래쪽(통계 절)에 정의되지만
 *          blkcg_destroy_blkgs() 등 위쪽 코드가 먼저 호출하므로 미리 선언한다. */
static void __blkcg_rstat_flush(struct blkcg *blkcg, int cpu);

/*
 * blkcg_pol_mutex protects blkcg_policy[] and policy [de]activation.
 * blkcg_pol_register_mutex nests outside of it and synchronizes entire
 * policy [un]register operations including cgroup file additions /
 * removals.  Putting cgroup file registration outside blkcg_pol_mutex
 * allows grabbing it from cgroup callbacks.
 */
/* [한국어] 정책 등록/해제 전체를 직렬화하는 바깥쪽 뮤텍스.
 * blkcg_pol_mutex 보다 바깥에 중첩(nest)되며, cgroupfs 파일 등록/제거
 * (cgroup_add_dfl_cftypes 등)까지 이 락 안에서 수행한다. 파일 등록을
 * blkcg_pol_mutex 밖에 두는 이유는 cgroup 콜백 안에서 blkcg_pol_mutex 를
 * 잡을 수 있어야 하기 때문(위 영문 주석 참조). */
static DEFINE_MUTEX(blkcg_pol_register_mutex);
/* [한국어] blkcg_policy[] 테이블 자체와 정책 활성/비활성(activate/deactivate)을
 * 보호하는 안쪽 뮤텍스. blkg->pd[] 슬롯 배열을 건드리는 모든 경로가 이 락을
 * 요구한다. 잡는 순서는 항상 blkcg_pol_register_mutex → blkcg_pol_mutex. */
static DEFINE_MUTEX(blkcg_pol_mutex);

/* [한국어] 루트 cgroup 의 blkcg. 정적 전역이라 부팅 초기부터 존재하며,
 * cgroup 이 지정되지 않았거나 소멸 중인 모든 IO 가 되돌아갈 기준점이다.
 * blkcg_css_alloc(parent==NULL) 이 이 전역을 그대로 재사용한다. */
struct blkcg blkcg_root;
EXPORT_SYMBOL_GPL(blkcg_root);

/* [한국어] blkcg_root 의 css 를 가리키는 상수 포인터. cgroup 코어는 blkcg 가
 * 아니라 css 단위로 다루므로, 루트를 css 로 가리켜야 하는 코드
 * (bio_associate_blkg_from_css 등)가 이 심볼을 쓴다. */
struct cgroup_subsys_state * const blkcg_root_css = &blkcg_root.css;
EXPORT_SYMBOL_GPL(blkcg_root_css);

/* [한국어] 등록된 정책들의 전역 테이블. 인덱스가 곧 pol->plid 이고,
 * 같은 인덱스가 모든 blkg 의 pd[] 슬롯 번호로도 그대로 쓰인다.
 * 즉 blkcg_policy[i] 와 blkg->pd[i] 는 항상 같은 정책을 가리킨다.
 * 크기는 BLKCG_MAX_POLS(현재 6) 로 고정 — 정책 수가 컴파일 타임 상수다.
 * 설정자: blkcg_policy_register()/unregister(). 보호: blkcg_pol_mutex. */
static struct blkcg_policy *blkcg_policy[BLKCG_MAX_POLS];

/* [한국어] 시스템의 모든 blkcg 를 잇는 전역 리스트(각 blkcg->all_blkcgs_node).
 * 정책이 나중에 등록될 때 이미 존재하는 모든 cgroup 에 cpd(cgroup 단위 정책
 * 데이터)를 소급 할당해야 하므로 전체 열거 수단이 필요하다. */
static LIST_HEAD(all_blkcgs);		/* protected by blkcg_pol_mutex */

/* [한국어] io.stat 에 디버그용 추가 필드(use_delay/delay_nsec, 정책별 pd_stat)를
 * 출력할지 여부. 파일 끝의 module_param 으로 런타임 변경 가능(0644). */
bool blkcg_debug_stats = false;

/* [한국어] blkg 통계의 전역(cur/last) 부분을 갱신할 때만 잡는 raw spinlock.
 * per-cpu 누적은 락 없이 하지만, flush 로 상위에 합산하는 순간은 서로 다른
 * CPU 가 같은 blkg->iostat 을 건드리므로 직렬화가 필요하다. raw_ 인 이유는
 * PREEMPT_RT 에서도 잠들지 않는 짧은 임계구역이어야 하기 때문. */
static DEFINE_RAW_SPINLOCK(blkg_stat_lock);

/* [한국어] blkg_destroy_all() 이 한 번에 지우는 blkg 개수 상한.
 * 디스크 하나에 cgroup 이 수천 개 붙어 있으면 queue_lock 을 쥔 채 전부 지우다
 * softlockup 이 날 수 있어, 64개마다 락을 놓고 cond_resched() 한다. */
#define BLKG_DESTROY_BATCH_SIZE  64

/*
 * Lockless lists for tracking IO stats update
 *
 * New IO stats are stored in the percpu iostat_cpu within blkcg_gq (blkg).
 * There are multiple blkg's (one for each block device) attached to each
 * blkcg. The rstat code keeps track of which cpu has IO stats updated,
 * but it doesn't know which blkg has the updated stats. If there are many
 * block devices in a system, the cost of iterating all the blkg's to flush
 * out the IO stats can be high. To reduce such overhead, a set of percpu
 * lockless lists (lhead) per blkcg are used to track the set of recently
 * updated iostat_cpu's since the last flush. An iostat_cpu will be put
 * onto the lockless list on the update side [blk_cgroup_bio_start()] if
 * not there yet and then removed when being flushed [blkcg_rstat_flush()].
 * References to blkg are gotten and then put back in the process to
 * protect against blkg removal.
 *
 * Return: 0 if successful or -ENOMEM if allocation fails.
 */
/*
 * [한국어]
 * init_blkcg_llists - blkcg 의 per-cpu lockless 통계 리스트(lhead) 할당·초기화
 *
 * @blkcg: 방금 kzalloc 된, 아직 공개되지 않은 blkcg. 이 함수가 lhead 필드를 채운다.
 * @return: 0 이면 성공, -ENOMEM 이면 per-cpu 영역 할당 실패.
 *          호출자(blkcg_css_alloc)는 실패 시 blkcg 전체를 되돌리고 ERR_PTR 을 반환한다.
 *
 * 왜 필요한가: 위 영문 주석이 설명하듯, cgroup rstat 프레임워크는 "어느 CPU 에서
 * 통계가 갱신됐는지"까지만 기억하고 "그 CPU 에서 어느 blkg 가 갱신됐는지"는 모른다.
 * 한 blkcg 에는 블록 장치 수만큼 blkg 가 달려 있으므로, flush 때마다 그 blkcg 의
 * blkg 를 전부 훑으면 장치가 많은 시스템에서 비용이 커진다. 그래서 blkcg 마다
 * per-cpu lockless list 를 두고, 갱신된 blkg 의 iostat_cpu 만 그 리스트에 매달아
 * flush 때 그것만 처리한다. 리스트에 넣는 쪽이 blk_cgroup_bio_start(),
 * 빼는 쪽이 __blkcg_rstat_flush() 다.
 *
 * 실행 컨텍스트: blkcg 생성 시점(프로세스 컨텍스트, GFP_KERNEL 로 잠들 수 있음).
 * 아직 아무도 이 blkcg 를 볼 수 없는 상태라 별도 동기화가 필요 없다.
 *
 * 호출 체인:
 *   blkcg_css_alloc() → [init_blkcg_llists] → alloc_percpu_gfp()/init_llist_head()
 */
static int init_blkcg_llists(struct blkcg *blkcg)
{
	/* [한국어] for_each_possible_cpu 순회용 CPU 인덱스. possible(부팅 후 온라인이
	 * 될 수 있는 모든 CPU)을 도는 이유는, CPU 가 나중에 핫플러그로 올라와도
	 * 그 CPU 의 lhead 가 이미 초기화돼 있어야 하기 때문이다. */
	int cpu;

	/* [한국어] CPU 개수만큼의 struct llist_head 를 per-cpu 영역에 할당한다.
	 * GFP_KERNEL 이므로 잠들 수 있고, 따라서 이 함수는 락 없는 문맥에서만 호출된다.
	 * lockless list 를 쓰는 이유: 갱신 측(blk_cgroup_bio_start)은 IO 제출 핫패스라
	 * 스핀락조차 피하고 cmpxchg 한 번으로 끝내야 하기 때문. */
	blkcg->lhead = alloc_percpu_gfp(struct llist_head, GFP_KERNEL);
	if (!blkcg->lhead)
		/* [한국어] per-cpu 할당 실패. 통계 인프라 없이는 blkcg 를 만들 수 없으므로
		 * -ENOMEM 을 올려 blkcg_css_alloc() 이 전체를 롤백하게 한다. */
		return -ENOMEM;

	/* [한국어] 할당된 per-cpu 슬롯을 모두 "빈 리스트" 상태로 만든다.
	 * alloc_percpu_gfp 는 0 으로 채워주지만, llist_head 의 초기값이 NULL 이라는
	 * 사실에 의존하지 않고 명시적으로 초기화하는 것이 커널 관례다. */
	for_each_possible_cpu(cpu)
		/* [한국어] per_cpu_ptr 로 cpu 번째 슬롯 주소를 얻어 llist 를 빈 상태로 만든다. */
		init_llist_head(per_cpu_ptr(blkcg->lhead, cpu));
	/* [한국어] 모든 CPU 슬롯 초기화 완료 — 성공. */
	return 0;
}

/**
 * blkcg_css - find the current css
 *
 * Find the css associated with either the kthread or the current task.
 * This may return a dying css, so it is up to the caller to use tryget logic
 * to confirm it is alive and well.
 */
/*
 * [한국어]
 * blkcg_css - "지금 이 IO 는 어느 cgroup 것인가"를 결정하는 함수
 *
 * @return: 현재 문맥에 해당하는 blkcg 의 css 포인터. NULL 을 반환하지 않는다
 *          (최소한 태스크가 속한 io cgroup 의 css 가 나온다).
 *          단, 위 영문 주석대로 이미 죽어가는(dying) css 일 수 있으므로,
 *          참조를 잡으려는 호출자는 css_tryget_online() 류로 생존을 확인해야 한다.
 *
 * 왜 필요한가: bio 에 cgroup 을 붙일 때 기준이 되는 "현재 cgroup" 은 두 종류다.
 * (1) 일반 태스크: 자기가 속한 io cgroup.
 * (2) 워커 kthread: 자기 자신의 cgroup 이 아니라, 일을 위임한 원래 cgroup.
 *     대표적으로 cgroup writeback 이 파일 페이지를 내려쓸 때 writeback 워커는
 *     kthread_associate_blkcg() 로 "이 IO 는 저 cgroup 것" 이라고 미리 표시해 둔다.
 * 그래서 kthread 표시를 먼저 보고, 없을 때만 current 의 cgroup 을 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(submit_bio 경로). task_css() 접근을 위해
 * rcu_read_lock() 안에서 호출되어야 한다 — 호출자인 bio_associate_blkg() 가
 * rcu_read_lock 을 잡고 들어온다.
 *
 * 호출 체인:
 *   bio_associate_blkg() / blkcg_maybe_throttle_current()
 *     → [blkcg_css] → kthread_blkcg() 또는 task_css(current, io_cgrp_id)
 */
static struct cgroup_subsys_state *blkcg_css(void)
{
	/* [한국어] 반환할 css. 아래 두 경로 중 하나에서 채워진다. */
	struct cgroup_subsys_state *css;

	/* [한국어] 이 커널 스레드가 다른 cgroup 을 대신해 IO 를 내는 중인지 확인.
	 * kthread_associate_blkcg() 로 설정돼 있으면 그 css 가, 아니면 NULL 이 온다.
	 * 일반 유저 태스크에서는 항상 NULL. */
	css = kthread_blkcg();
	if (css)
		/* [한국어] 위임된 cgroup 이 있으므로 그것을 그대로 쓴다. 이렇게 해야
		 * writeback 으로 미뤄진 IO 도 원래 cgroup 의 제한/통계에 잡힌다. */
		return css;
	/* [한국어] 일반 경로: current 태스크가 붙어 있는 io cgroup 의 css 를 반환한다.
	 * io_cgrp_id 는 cgroup 코어가 io 서브시스템에 부여한 인덱스이고,
	 * task_css() 는 그 인덱스로 태스크의 css_set 에서 css 를 꺼낸다.
	 * task_css() 는 RCU 보호를 전제로 하므로 호출자가 rcu_read_lock 을 쥐고 있어야 한다. */
	return task_css(current, io_cgrp_id);
}

/*
 * [한국어]
 * blkg_free_workfn - blkg 의 실제 자원 해제를 잠들 수 있는 문맥에서 수행
 *
 * @work: blkg->free_work. container_of 로 blkg 본체를 복원한다.
 * @return: 없음(workqueue 콜백).
 *
 * 왜 필요한가: blkg 해제는 두 가지 이유로 잠들 수 있다.
 * (1) 정책의 pd_free_fn() 이 잠들 수 있고, (2) blk_put_queue() 가 마지막 참조를
 * 놓으면 request_queue 의 release 핸들러가 실행되며 이 역시 잠들 수 있다.
 * 그런데 blkg 해제의 시작점(percpu_ref 가 0 이 되는 순간, 그리고 그 뒤의 RCU
 * 콜백)은 아토믹/소프트IRQ 문맥일 수 있다. 그래서 blkg_free() 가 이 함수를
 * workqueue 에 얹고, 여기서 잠들 수 있는 정리를 마저 한다.
 *
 * 해제 순서가 중요한 이유: 같은 pd 를 blkcg_deactivate_policy() 도 해제할 수
 * 있으므로(위 영문 주석), 둘 사이를 q->blkcg_mutex 로 직렬화한다. 또한
 * blkg 를 q->blkg_list 에서 떼는 list_del_init 을 blkg_destroy() 가 아니라
 * 여기까지 미룬 이유도 같다 — deactivate 쪽이 리스트를 순회하며 pd 를 지울 때
 * 아직 pd 를 해제하지 않은 blkg 가 리스트에 남아 있어야 순서가 보장된다.
 *
 * 실행 컨텍스트: 시스템 workqueue 의 kworker(프로세스 컨텍스트, 잠들 수 있음).
 * 이 시점에는 blkg 를 가리키는 참조가 이미 0 이므로 다른 CPU 가 이 blkg 를
 * 새로 볼 수 없다.
 *
 * 호출 체인:
 *   blkg_destroy() → blkg_put() → percpu_ref 0 → blkg_release()
 *     → call_rcu(__blkg_release) → blkg_free() → schedule_work()
 *     → [blkg_free_workfn] → pd_free_fn / blk_put_queue / kfree
 */
static void blkg_free_workfn(struct work_struct *work)
{
	/* [한국어] workqueue 는 struct work_struct 포인터만 넘겨주므로,
	 * blkg 안에 임베드된 free_work 필드의 오프셋을 빼서 blkg 본체를 되찾는다. */
	struct blkcg_gq *blkg = container_of(work, struct blkcg_gq,
					     free_work);
	/* [한국어] 이 blkg 가 붙어 있던 request_queue. 아래에서 queue_lock 을 잡고
	 * blkg_list 에서 떼어낸 뒤 참조를 놓기 위해 미리 지역 변수에 담아 둔다
	 * (kfree(blkg) 이후에는 blkg->q 를 읽을 수 없으므로). */
	struct request_queue *q = blkg->q;
	/* [한국어] pd[] 슬롯 순회용 인덱스(= 정책의 plid). */
	int i;

	/*
	 * pd_free_fn() can also be called from blkcg_deactivate_policy(),
	 * in order to make sure pd_free_fn() is called in order, the deletion
	 * of the list blkg->q_node is delayed to here from blkg_destroy(), and
	 * blkcg_mutex is used to synchronize blkg_free_workfn() and
	 * blkcg_deactivate_policy().
	 */
	/* [한국어] pd 해제 경로가 여기(blkg 소멸)와 blkcg_deactivate_policy() 두 곳이므로
	 * q 단위 뮤텍스로 직렬화한다. 뮤텍스이므로 잠들 수 있는 문맥이어야 하고,
	 * 그것이 이 정리를 workqueue 로 미룬 이유 중 하나다. */
	mutex_lock(&q->blkcg_mutex);
	/* [한국어] 이 blkg 에 붙어 있는 모든 정책 데이터를 plid 순서대로 해제한다.
	 * BLKCG_MAX_POLS 는 pd[] 배열 크기이자 등록 가능한 정책 수의 상한. */
	for (i = 0; i < BLKCG_MAX_POLS; i++)
		/* [한국어] 그 정책이 이 큐에서 활성화되지 않았다면 pd[i] 는 NULL 이다. */
		if (blkg->pd[i])
			/* [한국어] 정책이 스스로 정의한 해제 콜백을 호출한다.
			 * 예: blk-throttle 은 throtl_grp 를, iocost 는 ioc_gq 를 반납한다.
			 * 여기서 blkcg_policy[i] 를 곧바로 참조할 수 있는 이유는,
			 * pd[i] 가 살아 있다는 것 자체가 그 정책이 아직 등록돼 있고
			 * 이 큐에서 활성 상태임을 뜻하기 때문이다. */
			blkcg_policy[i]->pd_free_fn(blkg->pd[i]);
	/* [한국어] blkg 는 생성 시 부모 blkg 의 참조를 잡아 두었다(자식이 살아 있는 한
	 * 부모는 죽지 않는다). 루트 blkg 는 부모가 없어 NULL 검사가 필요하다.
	 * 이 put 이 부모의 마지막 참조라면 부모도 같은 해제 사슬을 타게 된다. */
	if (blkg->parent)
		blkg_put(blkg->parent);
	/* [한국어] q->blkg_list 는 queue_lock 으로 보호된다. IO 완료 등 IRQ 문맥에서도
	 * 이 리스트를 만질 수 있으므로 _irq 변형으로 로컬 인터럽트까지 막는다. */
	spin_lock_irq(&q->queue_lock);
	/* [한국어] 큐가 소유한 blkg 목록에서 이 blkg 를 뗀다. blkg_destroy() 가 아니라
	 * pd 해제를 끝낸 지금에야 떼는 것이 핵심(위 영문 주석의 순서 보장). */
	list_del_init(&blkg->q_node);
	spin_unlock_irq(&q->queue_lock);
	mutex_unlock(&q->blkcg_mutex);

	/* [한국어] blkg_alloc() 에서 잡았던 request_queue 참조를 반납한다.
	 * 마지막 참조라면 큐의 release 핸들러가 여기서 실행되며, 이 역시 잠들 수 있다. */
	blk_put_queue(q);
	/* [한국어] per-cpu IO 통계 영역 해제. 이 시점에는 __blkg_release() 가 이미
	 * 모든 CPU 의 통계를 flush 했으므로 잃어버리는 수치가 없다. */
	free_percpu(blkg->iostat_cpu);
	/* [한국어] percpu_ref 가 내부적으로 들고 있던 per-cpu 카운터를 해제한다.
	 * percpu_ref_kill() → 참조 0 → release 콜백까지 모두 끝난 뒤여야 안전하다. */
	percpu_ref_exit(&blkg->refcnt);
	/* [한국어] 마지막으로 blkg 구조체 자체를 반납한다. 이 줄 이후 blkg 접근 금지. */
	kfree(blkg);
}

/**
 * blkg_free - free a blkg
 * @blkg: blkg to free
 *
 * Free @blkg which may be partially allocated.
 */
/*
 * [한국어]
 * blkg_free - blkg 해제를 workqueue 로 넘기는 얇은 진입점
 *
 * @blkg: 해제할 blkg. NULL 이거나 "부분적으로만 할당된" 상태일 수 있다
 *        (blkg_alloc() 중간에 실패한 경우 — 위 영문 주석의 partially allocated).
 * @return: 없음.
 *
 * 왜 필요한가: 실제 해제(blkg_free_workfn)는 잠들 수 있는데, 호출자는 그렇지
 * 않은 문맥일 수 있다. 두 호출자를 보면 이유가 분명하다.
 *   - blkg_alloc()/blkg_create() 실패 경로: 잠들 수 있는 문맥이지만 코드 단순화를
 *     위해 같은 경로를 쓴다.
 *   - __blkg_release(): RCU 콜백이라 잠들 수 없다. 여기서는 반드시 비동기여야 한다.
 * 그래서 무조건 work 로 넘겨 한 갈래로 통일한다.
 *
 * 실행 컨텍스트: 임의(RCU 콜백 포함). 이 함수 자체는 잠들지 않는다.
 *
 * 호출 체인:
 *   blkg_alloc() 실패 / blkg_create() 실패 / __blkg_release()
 *     → [blkg_free] → schedule_work() → blkg_free_workfn()
 */
static void blkg_free(struct blkcg_gq *blkg)
{
	/* [한국어] 할당 실패 경로에서 NULL 이 그대로 넘어올 수 있으므로 방어한다. */
	if (!blkg)
		return;

	/*
	 * Both ->pd_free_fn() and request queue's release handler may
	 * sleep, so free us by scheduling one work func
	 */
	/* [한국어] blkg 안에 임베드된 work_struct 를 blkg_free_workfn 으로 초기화한다.
	 * 별도 할당이 필요 없도록 구조체 안에 work 를 품고 있는 전형적인 패턴 —
	 * 해제 경로에서 메모리 할당이 실패할 여지를 없앤다. */
	INIT_WORK(&blkg->free_work, blkg_free_workfn);
	/* [한국어] 시스템 기본 workqueue 에 얹는다. 이 호출 이후 blkg 는 kworker 소유가
	 * 되므로 호출자는 blkg 를 더 이상 건드리면 안 된다. */
	schedule_work(&blkg->free_work);
}

/*
 * [한국어]
 * __blkg_release - RCU grace period 가 지난 뒤 실행되는 blkg 해제 2단계
 *
 * @rcu: blkg->rcu_head. container_of 로 blkg 를 복원한다.
 * @return: 없음(RCU 콜백).
 *
 * 왜 필요한가: blkg 조회 경로(blkg_lookup)는 RCU read-side 에서 락 없이 포인터를
 * 읽는다. 따라서 참조 카운트가 0 이 되었더라도, 그 직전에 blkg 포인터를 집어간
 * 독자가 아직 남아 있을 수 있다. RCU grace period 를 한 번 기다린 뒤에야
 * "더 이상 이 blkg 를 새로 볼 사람이 없다" 가 보장되므로 여기서 뒷정리를 한다.
 *
 * 여기서 통계를 flush 하는 이유: blkg 가 사라지면 그 blkg 의 per-cpu 통계도
 * 함께 사라진다. 그런데 그 수치는 부모 cgroup 의 누적치에도 반영돼야 하므로,
 * 메모리를 놓기 전에 모든 CPU 의 lockless list 를 비워 전역/부모로 올린다.
 * 이렇게 하지 않으면 cgroup 을 지울 때마다 io.stat 수치가 줄어드는 현상이 생긴다.
 *
 * 실행 컨텍스트: RCU 콜백 — 소프트IRQ 문맥이며 잠들 수 없다. 그래서 실제
 * kfree/뮤텍스 구간은 다시 blkg_free() → workqueue 로 넘긴다.
 *
 * 호출 체인:
 *   blkg_release() → call_rcu() → [__blkg_release] → __blkcg_rstat_flush(),
 *                                                    css_put(), blkg_free()
 */
static void __blkg_release(struct rcu_head *rcu)
{
	/* [한국어] RCU 콜백 인자는 blkg 안에 임베드된 rcu_head 이므로 본체를 복원한다. */
	struct blkcg_gq *blkg = container_of(rcu, struct blkcg_gq, rcu_head);
	/* [한국어] 통계를 밀어 올릴 대상 cgroup. 아래 css_put 전에 읽어 두어야 한다. */
	struct blkcg *blkcg = blkg->blkcg;
	/* [한국어] 모든 possible CPU 를 도는 인덱스. */
	int cpu;

#ifdef CONFIG_BLK_CGROUP_PUNT_BIO
	/* [한국어] PUNT_BIO 기능이 켜진 커널에서만 존재하는 대기 bio 리스트 검사.
	 * blkg 가 죽는 시점에 아직 제출되지 않은 bio 가 남아 있다면 그 IO 는 영원히
	 * 유실되므로, 논리 오류를 잡기 위한 WARN 이다(패닉이 아닌 경고). */
	WARN_ON(!bio_list_empty(&blkg->async_bios));
#endif
	/*
	 * Flush all the non-empty percpu lockless lists before releasing
	 * us, given these stat belongs to us.
	 *
	 * blkg_stat_lock is for serializing blkg stat update
	 */
	/* [한국어] 이 blkg 의 통계가 아직 어느 CPU 의 lockless list 에 걸려 있을지
	 * 모르므로 모든 CPU 를 훑는다. __blkcg_rstat_flush 는 해당 CPU 의 llist 를
	 * 통째로 떼어내 각 blkg 의 iostat.cur 과 부모로 합산한다. */
	for_each_possible_cpu(cpu)
		/* [한국어] blkcg 단위로 flush 한다 — 이 blkg 것뿐 아니라 같은 CPU 에
		 * 걸려 있던 형제 blkg 통계도 함께 정리되지만, 그래도 정확성은 유지된다. */
		__blkcg_rstat_flush(blkcg, cpu);

	/* release the blkcg and parent blkg refs this blkg has been holding */
	/* [한국어] blkg 는 자신이 속한 cgroup 의 css 참조를 잡고 있었다(살아 있는 blkg 가
	 * 있는 한 blkcg 는 해제되지 않는다). 이제 그 참조를 놓는다.
	 * 부모 blkg 참조는 blkg_free_workfn() 에서 놓는다. */
	css_put(&blkg->blkcg->css);
	/* [한국어] 남은 정리(pd 해제, 큐 참조 반납, kfree)는 잠들 수 있으므로
	 * workqueue 로 넘긴다. */
	blkg_free(blkg);
}

/*
 * A group is RCU protected, but having an rcu lock does not mean that one
 * can access all the fields of blkg and assume these are valid.  For
 * example, don't try to follow throtl_data and request queue links.
 *
 * Having a reference to blkg under an rcu allows accesses to only values
 * local to groups like group stats and group rate limits.
 */
/*
 * [한국어]
 * blkg_release - blkg->refcnt(percpu_ref) 가 0 이 되었을 때 불리는 release 콜백
 *
 * @ref: blkg 안에 임베드된 percpu_ref. container_of 로 blkg 를 복원한다.
 * @return: 없음.
 *
 * === percpu_ref 생명주기 ===
 * blkg->refcnt 는 percpu_ref 다. 살아 있는 동안(per-cpu 모드)에는 참조 증감이
 * per-cpu 카운터 증감일 뿐이라 원자적 연산조차 없다 — bio 마다 blkg 참조를
 * 잡아야 하는 핫패스이므로 이 비용이 중요하다. blkg_destroy() 가
 * percpu_ref_kill() 을 부르면 그때부터 atomic 모드로 전환되고, 흩어져 있던
 * per-cpu 카운터가 합산되어 실제 0 인지 판정된다. 0 이 되는 순간 이 콜백이 불린다.
 * 즉 이 함수가 불렸다는 것은 "kill 되었고, 마지막 참조까지 반납됐다" 는 뜻이다.
 *
 * 왜 곧바로 해제하지 않는가: 위 영문 주석대로 blkg 는 RCU 로 보호된다.
 * RCU 독자가 blkg 포인터를 이미 들고 있을 수 있으므로 grace period 를 기다린다.
 * (영문 주석은 덧붙여, RCU 락만으로는 blkg 의 "모든" 필드가 유효하다고 가정하면
 *  안 된다고 경고한다 — q 나 정책 데이터 같은 외부 링크는 따라가면 안 되고,
 *  그룹 통계·요율 같은 blkg 로컬 값만 읽어야 한다.)
 *
 * 실행 컨텍스트: percpu_ref 의 마지막 put 을 한 문맥 그대로. IRQ/소프트IRQ 일 수
 * 있으므로 잠들 수 없다. call_rcu 는 잠들지 않으므로 여기서 안전하다.
 *
 * 호출 체인:
 *   blkg_put() → percpu_ref_put() → (0 도달) → [blkg_release]
 *     → call_rcu() → __blkg_release() → blkg_free() → blkg_free_workfn()
 */
static void blkg_release(struct percpu_ref *ref)
{
	/* [한국어] percpu_ref 는 blkg 안에 refcnt 필드로 임베드돼 있으므로 본체 복원. */
	struct blkcg_gq *blkg = container_of(ref, struct blkcg_gq, refcnt);

	/* [한국어] RCU grace period 이후 __blkg_release 를 실행하도록 예약한다.
	 * 이 시점 이후로도 기존 RCU 독자는 잠시 blkg 를 볼 수 있지만,
	 * 그들은 이미 tryget 에 실패했거나 참조를 들고 있던 쪽이다. */
	call_rcu(&blkg->rcu_head, __blkg_release);
}

#ifdef CONFIG_BLK_CGROUP_PUNT_BIO
/* [한국어] punt(우회 제출)된 bio 를 대신 제출해 줄 전용 워크큐.
 * 이 기능을 쓰는 코드가 없는 커널 구성에서는 통째로 컴파일되지 않는다. */
static struct workqueue_struct *blkcg_punt_bio_wq;

/*
 * [한국어]
 * blkg_async_bio_workfn - 워커 문맥에서 대기 중인 bio 들을 실제로 제출
 *
 * @work: blkg->async_bio_work. container_of 로 blkg 를 복원한다.
 * @return: 없음(workqueue 콜백).
 *
 * 왜 필요한가: blkcg_punt_bio_submit() 이 "지금 이 스레드에서 제출하면 안 되는"
 * bio 를 blkg->async_bios 에 쌓아 두었다. 이 함수가 그 목록을 통째로 가져와
 * 워커 문맥에서 submit_bio() 를 호출한다. 우선순위 역전(공유 kthread 가 특정
 * cgroup 의 제한에 걸려 멈추는 상황)을 피하는 것이 목적이다.
 *
 * 실행 컨텍스트: blkcg_punt_bio_wq 의 kworker(프로세스 컨텍스트, 잠들 수 있음).
 * 대기 중인 bio 가 있는 한 blkg 는 사라지지 않는다(영문 주석) — bio 자체가
 * blkg 참조를 들고 있기 때문이다.
 *
 * 호출 체인:
 *   blkcg_punt_bio_submit() → queue_work() → [blkg_async_bio_workfn]
 *     → submit_bio()
 */
static void blkg_async_bio_workfn(struct work_struct *work)
{
	/* [한국어] work_struct 에서 이 bio 들의 주인 blkg 를 복원한다. */
	struct blkcg_gq *blkg = container_of(work, struct blkcg_gq,
					     async_bio_work);
	/* [한국어] blkg->async_bios 를 통째로 옮겨 받을 지역 리스트.
	 * 지역으로 옮기면 락을 짧게 잡고, 제출은 락 밖에서 할 수 있다. */
	struct bio_list bios = BIO_EMPTY_LIST;
	/* [한국어] 리스트에서 하나씩 꺼낸 bio. */
	struct bio *bio;
	/* [한국어] 여러 bio 를 모아 한 번에 드라이버로 내려보내기 위한 plug 컨텍스트.
	 * plug 는 현재 태스크(current->plug)에 매달리는 스택 지역 구조체다. */
	struct blk_plug plug;
	/* [한국어] plug 을 실제로 시작했는지 여부. 끝에서 짝 맞춰 finish 하기 위함. */
	bool need_plug = false;

	/* as long as there are pending bios, @blkg can't go away */
	/* [한국어] async_bios 는 제출자(punt_bio_submit)와 이 워커가 동시에 만지므로
	 * 스핀락으로 보호한다. IRQ 문맥에서는 접근하지 않으므로 _irq 변형이 아니다. */
	spin_lock(&blkg->async_bio_lock);
	/* [한국어] blkg->async_bios 의 내용을 지역 bios 로 옮기고 원본은 비운다.
	 * 이후 새로 들어오는 bio 는 비워진 리스트에 쌓이고 다음 work 실행이 처리한다. */
	bio_list_merge_init(&bios, &blkg->async_bios);
	/* [한국어] 리스트 이관이 끝났으므로 즉시 락을 놓는다. submit_bio 는 오래 걸리고
	 * 잠들 수도 있어 락을 쥔 채 부르면 안 된다. */
	spin_unlock(&blkg->async_bio_lock);

	/* start plug only when bio_list contains at least 2 bios */
	/* [한국어] head 가 있고 head->bi_next 도 있으면 bio 가 2개 이상이라는 뜻.
	 * 1개뿐이면 plug 을 걸어도 병합할 상대가 없어 오히려 오버헤드만 는다. */
	if (bios.head && bios.head->bi_next) {
		need_plug = true;
		/* [한국어] 이 구간의 submit_bio 결과 request 들이 곧바로 드라이버로 가지 않고
		 * current->plug 리스트에 모였다가 finish 시점에 일괄 dispatch 된다. */
		blk_start_plug(&plug);
	}
	/* [한국어] 리스트가 빌 때까지 하나씩 꺼내 정상 제출 경로로 넘긴다.
	 * bio->bi_blkg 는 이미 원래 cgroup 으로 설정돼 있으므로, 제출자가 워커라도
	 * 요금은 원래 cgroup 에 매겨진다. */
	while ((bio = bio_list_pop(&bios)))
		submit_bio(bio);
	/* [한국어] plug 을 시작했던 경우에만 닫는다. 여기서 모인 요청들이 실제로
	 * 드라이버 큐로 내려간다. */
	if (need_plug)
		blk_finish_plug(&plug);
}

/*
 * When a shared kthread issues a bio for a cgroup, doing so synchronously can
 * lead to priority inversions as the kthread can be trapped waiting for that
 * cgroup.  Use this helper instead of submit_bio to punt the actual issuing to
 * a dedicated per-blkcg work item to avoid such priority inversions.
 */
/*
 * [한국어]
 * blkcg_punt_bio_submit - submit_bio() 대신 쓰는, 우회 제출용 진입점
 *
 * @bio: 제출할 bio. bi_blkg 가 이미 설정돼 있어야 한다(호출 전에
 *       bio_associate_blkg 계열로 cgroup 이 붙어 있는 상태).
 * @return: 없음. bio 의 완료는 평소처럼 bi_end_io 로 통지된다.
 *
 * 왜 필요한가(위 영문 주석의 요지): 여러 cgroup 이 공유하는 kthread 가 특정
 * cgroup 을 대신해 bio 를 동기적으로 제출하면, 그 cgroup 의 IO 제한에 걸려
 * kthread 가 통째로 멈출 수 있다. 그러면 다른 cgroup 의 작업까지 함께 굶는
 * 우선순위 역전이 일어난다. 그래서 제출 자체를 blkg 별 work item 으로 넘긴다.
 *
 * 실행 컨텍스트: 호출자 문맥(주로 kthread, 프로세스 컨텍스트).
 * 이 함수는 잠들지 않는다 — 실제 제출은 워커가 대신 한다.
 *
 * 호출 체인:
 *   (공유 kthread 의 bio 제출 지점) → [blkcg_punt_bio_submit]
 *     → queue_work() → blkg_async_bio_workfn() → submit_bio()
 */
void blkcg_punt_bio_submit(struct bio *bio)
{
	/* [한국어] 이 bio 가 어느 cgroup 몫인지는 bi_blkg 에 이미 박혀 있다. */
	struct blkcg_gq *blkg = bio->bi_blkg;

	/* [한국어] parent 가 있다 == 루트가 아닌 cgroup 이다.
	 * 루트에는 IO 제한이 없으므로 우회할 이유가 없다(아래 else). */
	if (blkg->parent) {
		/* [한국어] 이 blkg 의 대기 리스트를 워커와 공유하므로 스핀락으로 보호. */
		spin_lock(&blkg->async_bio_lock);
		/* [한국어] 제출하지 않고 리스트 꼬리에 매단다. bio 가 blkg 참조를 들고
		 * 있으므로, 리스트에 남아 있는 동안 blkg 는 해제되지 않는다. */
		bio_list_add(&blkg->async_bios, bio);
		spin_unlock(&blkg->async_bio_lock);
		/* [한국어] blkg 전용 work 를 큐잉한다. 이미 큐잉/실행 중이면 queue_work 는
		 * 아무 일도 하지 않고 false 를 반환하는데, 그래도 문제없다 —
		 * 워커가 리스트를 통째로 비우는 방식이라 새 항목도 함께 처리되거나
		 * 다음 큐잉에서 처리된다. */
		queue_work(blkcg_punt_bio_wq, &blkg->async_bio_work);
	} else {
		/* never bounce for the root cgroup */
		/* [한국어] 루트 cgroup 은 제한을 받지 않으므로 우회 지연을 만들 필요가 없다.
		 * 곧바로 정상 제출 경로로 보낸다. */
		submit_bio(bio);
	}
}
EXPORT_SYMBOL_GPL(blkcg_punt_bio_submit);

/*
 * [한국어]
 * blkcg_punt_bio_init - kthread 우회 bio 제출용 전용 워크큐를 만든다
 *
 * @return: 0 성공, -ENOMEM 실패
 *
 * === 왜 별도 워크큐가 필요한가 ===
 * 어떤 bio는 제출 스레드에서 바로 내보내면 안 된다. 대표적으로 btrfs 같은
 * 파일시스템이 압축/체크섬 작업을 위해 kthread에서 bio를 만드는 경우인데,
 * 그 kthread는 특정 cgroup에 속하지 않는다. 그대로 제출하면 I/O가 root
 * cgroup 몫으로 계산되어 cgroup 대역폭 제한이 우회된다.
 * 그래서 원래 요청자의 cgroup 정보를 실어 이 워크큐로 넘기고, 워커가
 * 그 cgroup의 컨텍스트에서 대신 제출한다("punt" = 넘긴다).
 *
 * === 워크큐 플래그의 의미 ===
 *   WQ_MEM_RECLAIM - 메모리 회수 경로에서도 진행이 보장되어야 한다.
 *     write-back I/O가 이 워크큐를 거치는데, 메모리가 부족할 때 이 워커가
 *     막히면 회수가 끝나지 않아 시스템 전체가 멈춘다. 이 플래그가
 *     전용 rescuer 스레드를 붙여 그 상황을 방지한다.
 *   WQ_FREEZABLE - 시스템 suspend 시 이 워커를 멈춰, 얼어붙은 장치로
 *     I/O가 나가지 않게 한다.
 *   WQ_UNBOUND - 특정 CPU에 묶지 않는다. 제출 CPU 지역성보다 지연 없이
 *     실행되는 것이 중요하기 때문이다.
 *   WQ_SYSFS - /sys/bus/workqueue/devices/blkcg_punt_bio로 노출해
 *     관리자가 nice 값이나 CPU 마스크를 조정할 수 있게 한다.
 *
 * 실행 컨텍스트: subsys_initcall — 부팅 중 블록 계층 초기화 시점.
 *
 * 호출 체인:
 *   subsys_initcall → [blkcg_punt_bio_init] → alloc_workqueue
 */
static int __init blkcg_punt_bio_init(void)
{
	/* [한국어] 플래그 조합의 의미는 위 함수 주석 참조. 마지막 인자 0 은
	 * max_active(동시 실행 work 수 제한) 를 기본값에 맡긴다는 뜻이다. */
	blkcg_punt_bio_wq = alloc_workqueue("blkcg_punt_bio",
					    WQ_MEM_RECLAIM | WQ_FREEZABLE |
					    WQ_UNBOUND | WQ_SYSFS, 0);
	if (!blkcg_punt_bio_wq)
		/* [한국어] 부팅 초기 메모리 부족. initcall 이 음수를 반환하면 커널이
		 * 경고를 남기지만 부팅은 계속된다. */
		return -ENOMEM;
	/* [한국어] 워크큐 준비 완료. 이제 blkcg_punt_bio_submit() 을 쓸 수 있다. */
	return 0;
}
/* [한국어] 부팅 시 subsys 초기화 단계에서 위 함수를 실행하도록 등록한다.
 * 블록 계층이 올라오기 전에 워크큐가 준비돼 있어야 하므로 이 단계를 쓴다. */
subsys_initcall(blkcg_punt_bio_init);
#endif /* CONFIG_BLK_CGROUP_PUNT_BIO */

/**
 * bio_blkcg_css - return the blkcg CSS associated with a bio
 * @bio: target bio
 *
 * This returns the CSS for the blkcg associated with a bio, or %NULL if not
 * associated. Callers are expected to either handle %NULL or know association
 * has been done prior to calling this.
 */
/*
 * [한국어]
 * bio_blkcg_css - bio 가 어느 cgroup 것인지 css 형태로 되돌려주는 조회 헬퍼
 *
 * @bio: 대상 bio. NULL 이어도 된다(아래에서 방어한다).
 * @return: bio 에 결합된 blkcg 의 css. 결합되지 않았거나 bio 가 NULL 이면 %NULL.
 *          위 영문 주석대로, 호출자는 NULL 을 처리하거나 "이 시점에는 반드시
 *          결합돼 있다" 를 스스로 알고 있어야 한다.
 *
 * 왜 필요한가: blkg 는 (blkcg, queue) 쌍이므로 bio->bi_blkg 하나에서 cgroup 을
 * 역으로 알아낼 수 있다. cgroup 단위 비교/판정이 필요한 곳(예: 서로 다른
 * cgroup 의 bio 를 하나의 request 로 합치면 요금 계산이 틀어지므로 병합을
 * 막는 검사)에서 이 헬퍼로 css 를 꺼내 비교한다.
 *
 * 실행 컨텍스트: 제한 없음. 다만 반환된 css 는 참조를 잡아 주지 않으므로,
 * bio 가 살아 있는 동안에만 유효하다고 보아야 한다(bio 가 blkg 참조를,
 * blkg 가 css 참조를 들고 있어 그 사슬로 생존이 보장된다).
 *
 * 호출 체인:
 *   blk_cgroup_mergeable() 등 cgroup 비교가 필요한 지점 → [bio_blkcg_css]
 */
struct cgroup_subsys_state *bio_blkcg_css(struct bio *bio)
{
	/* [한국어] bio 자체가 없거나, 아직 bio_associate_blkg() 를 거치지 않아
	 * cgroup 이 붙지 않은 bio 는 소속을 말할 수 없으므로 NULL. */
	if (!bio || !bio->bi_blkg)
		return NULL;
	/* [한국어] bio → blkg → blkcg → css 로 두 단계 역참조한다.
	 * blkg 가 blkcg 를 가리키는 것은 blkg 생성 시 고정되며 이후 바뀌지 않는다. */
	return &bio->bi_blkg->blkcg->css;
}
EXPORT_SYMBOL_GPL(bio_blkcg_css);

/**
 * blkcg_parent - get the parent of a blkcg
 * @blkcg: blkcg of interest
 *
 * Return the parent blkcg of @blkcg.  Can be called anytime.
 */
/*
 * [한국어]
 * blkcg_parent - 부모 blkcg를 반환 (cgroup 트리 상향 탐색)
 *
 * @blkcg: 기준 blkcg
 * @return: 부모 blkcg. root cgroup이면 NULL.
 *
 * blkcg는 cgroup 코어의 css(cgroup_subsys_state)를 내장하고 있고, 트리 구조
 * 자체는 css가 관리한다. 따라서 부모를 찾으려면 css의 parent를 따라간 뒤
 * 다시 blkcg로 되돌리면 된다.
 *
 * 이 상향 탐색이 필요한 이유는 blk-cgroup의 여러 정책이 계층적이기 때문이다.
 * 자식 cgroup에 설정이 없으면 부모의 설정을 물려받고(상속), 통계는 자식에서
 * 부모로 합산되어 올라간다(recursive sum). 그 두 방향의 순회가 모두 이
 * 함수를 쓴다.
 *
 * root cgroup에서는 css.parent가 NULL이므로 이 함수도 NULL을 반환한다 —
 * 호출자는 그것을 "트리 꼭대기에 도달했다"는 종료 조건으로 쓴다.
 *
 * 실행 컨텍스트: 어디서든(단순 포인터 역참조). 다만 blkcg 자체의 생존은
 * 호출자가 참조나 RCU로 보장해야 한다.
 *
 * 호출 체인:
 *   blkg_alloc / blkcg_css_online / 정책별 상속 로직 → [blkcg_parent]
 */
static inline struct blkcg *blkcg_parent(struct blkcg *blkcg)
{
	/* [한국어] cgroup 계층은 css 가 관리하므로 css.parent 로 한 칸 올라간 뒤,
	 * css_to_blkcg() (container_of 래퍼)로 다시 blkcg 로 되돌린다.
	 * 루트에서는 css.parent 가 NULL 이고, css_to_blkcg(NULL) 은 NULL 을 준다. */
	return css_to_blkcg(blkcg->css.parent);
}

/**
 * blkg_alloc - allocate a blkg
 * @blkcg: block cgroup the new blkg is associated with
 * @disk: gendisk the new blkg is associated with
 * @gfp_mask: allocation mask to use
 *
 * Allocate a new blkg associating @blkcg and @disk.
 */
/*
 * [한국어]
 * blkg_alloc - (blkcg, disk->queue) 교차점을 나타내는 blkg 객체를 만들어 채운다
 *
 * @blkcg: 이 blkg 가 대표할 cgroup. 이후 blkg->blkcg 로 고정된다.
 * @disk:  이 blkg 가 대표할 블록 장치. 실제로 붙는 대상은 disk->queue 다.
 * @gfp_mask: 할당 플래그. 호출 문맥에 따라 GFP_KERNEL 또는 GFP_NOWAIT 이 온다
 *            (락을 쥔 채 부르는 경로가 있어 무조건 잠들 수 없다).
 * @return: 완성된 blkg, 실패 시 NULL. 반환된 blkg 는 아직 어떤 자료구조에도
 *          등록되지 않은 "고아" 상태다 — 등록은 blkg_create() 가 한다.
 *
 * === 왜 (blkcg, queue) 쌍마다 하나인가 ===
 * cgroup 은 여러 장치에 IO 를 낼 수 있고, 한 장치는 여러 cgroup 이 함께 쓴다.
 * 즉 "cgroup × 장치" 는 2차원 격자이고, 제한값·통계·정책 상태는 그 격자의
 * 칸마다 따로 있어야 한다(예: cgroup A 의 sda 제한과 sdb 제한은 별개).
 * 그 한 칸이 바로 blkg 다. 그래서 blkg 는 blkcg 쪽 리스트(blkcg->blkg_list)와
 * queue 쪽 리스트(q->blkg_list)에 동시에 매달리고, blkcg->blkg_tree 라는
 * radix tree 에 queue id 를 키로 색인된다.
 *
 * === 이 함수가 준비하는 것 ===
 * (1) blkg 구조체 자체와 percpu_ref 참조 카운터
 * (2) per-cpu IO 통계 영역(iostat_cpu)
 * (3) request_queue 참조 획득 — blkg 가 사는 동안 큐가 사라지면 안 된다
 * (4) 그 큐에서 활성화된 모든 정책의 pd(정책 데이터)를 할당해 pd[] 에 꽂기
 * 실패하면 goto 사슬을 역순으로 타고 내려가며 정확히 반대 순서로 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. gfp_mask 가 GFP_NOWAIT 인 경로
 * (blkg_lookup_create 가 queue_lock 을 쥔 채 부르는 경우)도 있으므로,
 * 이 함수는 잠드는 것을 전제해서는 안 된다.
 *
 * 호출 체인:
 *   blkcg_init_disk() / blkg_lookup_create() / blkg_conf_prep()
 *     → [blkg_alloc] → percpu_ref_init / alloc_percpu_gfp / blk_get_queue
 *                      / pol->pd_alloc_fn
 */
static struct blkcg_gq *blkg_alloc(struct blkcg *blkcg, struct gendisk *disk,
				   gfp_t gfp_mask)
{
	/* [한국어] 만들어 반환할 blkg. 실패 경로에서 부분 해제 대상이 된다. */
	struct blkcg_gq *blkg;
	/* [한국어] i 는 정책 슬롯(plid) 인덱스, cpu 는 per-cpu 통계 초기화 인덱스. */
	int i, cpu;

	/* alloc and init base part */
	/* [한국어] 큐가 속한 NUMA 노드에서 0 으로 초기화된 blkg 를 할당한다.
	 * _node 변형을 쓰는 이유는 이 blkg 를 주로 만지는 것이 그 큐에 IO 를 내는
	 * 경로이기 때문 — 같은 노드 메모리를 쓰면 원격 노드 접근을 줄일 수 있다.
	 * kzalloc 이라 pd[] 와 각종 포인터가 자동으로 NULL 이 되고, 이는 아래
	 * 실패 경로의 NULL 검사가 성립하는 전제이기도 하다. */
	blkg = kzalloc_node(sizeof(*blkg), gfp_mask, disk->queue->node);
	if (!blkg)
		/* [한국어] 첫 할당부터 실패 — 되돌릴 것이 없으므로 바로 NULL. */
		return NULL;
	/* [한국어] 참조 카운터를 per-cpu 모드로 초기화한다. 인자 순서는
	 * (ref, release 콜백, 초기 플래그, gfp). 초기 플래그 0 은 "처음부터
	 * per-cpu 모드로 시작" 을 뜻하고, 카운트는 1 에서 시작한다.
	 * 0 이 되면 blkg_release() 가 불린다(단, percpu_ref_kill 이후에만 판정됨). */
	if (percpu_ref_init(&blkg->refcnt, blkg_release, 0, gfp_mask))
		goto out_free_blkg;
	/* [한국어] CPU 마다 하나씩인 통계 묶음(blkg_iostat_set). IO 제출 핫패스가
	 * 락 없이 여기에 누적하고, flush 때 전역 iostat 으로 합산된다. */
	blkg->iostat_cpu = alloc_percpu_gfp(struct blkg_iostat_set, gfp_mask);
	if (!blkg->iostat_cpu)
		/* [한국어] percpu_ref 를 이미 초기화했으므로 그것부터 되돌린다. */
		goto out_exit_refcnt;
	/* [한국어] 큐 참조를 하나 얻는다. blkg 가 살아 있는 한 request_queue 가
	 * 해제되면 안 되기 때문이다. 큐가 이미 죽어가는 중이면 실패하며,
	 * 그때는 이 blkg 를 만들 이유도 없다. 짝이 되는 반납은 blkg_free_workfn(). */
	if (!blk_get_queue(disk->queue))
		goto out_free_iostat;

	/* [한국어] 이 blkg 가 붙는 큐를 고정한다. 이후 변경되지 않는 불변 필드. */
	blkg->q = disk->queue;
	/* [한국어] q->blkg_list 에 매달릴 링크를 빈 상태로 초기화. 실제 연결은
	 * blkg_create() 에서, 해제는 blkg_free_workfn() 에서 한다. */
	INIT_LIST_HEAD(&blkg->q_node);
	/* [한국어] 이 blkg 가 대표하는 cgroup 을 고정한다. 이것 역시 불변 필드이며,
	 * (blkcg, q) 쌍이 blkg 의 정체성이다. */
	blkg->blkcg = blkcg;
	/* [한국어] 전역 통계 묶음이 자기 주인 blkg 를 역참조하도록 백포인터를 건다.
	 * flush 코드가 iostat 포인터만 들고 blkg 를 찾아가야 하기 때문. */
	blkg->iostat.blkg = blkg;
#ifdef CONFIG_BLK_CGROUP_PUNT_BIO
	/* [한국어] 우회 제출 대기 리스트를 보호할 스핀락 초기화. */
	spin_lock_init(&blkg->async_bio_lock);
	/* [한국어] 우회 제출 대기 bio 리스트를 빈 상태로 초기화. */
	bio_list_init(&blkg->async_bios);
	/* [한국어] 그 리스트를 처리할 work 를 blkg_async_bio_workfn 으로 묶는다. */
	INIT_WORK(&blkg->async_bio_work, blkg_async_bio_workfn);
#endif

	/* [한국어] u64_stats_sync 초기화. 32비트 아키텍처에서 64비트 카운터를
	 * 찢김 없이 읽기 위한 seqcount 이며, 64비트에서는 사실상 no-op 이다. */
	u64_stats_init(&blkg->iostat.sync);
	/* [한국어] possible CPU 전부에 대해 per-cpu 통계 슬롯을 초기화한다.
	 * 나중에 핫플러그로 올라올 CPU 도 준비돼 있어야 하므로 online 이 아니라
	 * possible 을 돈다. */
	for_each_possible_cpu(cpu) {
		/* [한국어] 각 CPU 슬롯의 seqcount 초기화. */
		u64_stats_init(&per_cpu_ptr(blkg->iostat_cpu, cpu)->sync);
		/* [한국어] 각 CPU 슬롯도 자기 blkg 를 가리키게 한다. flush 시
		 * llist 에서 꺼낸 iostat_set 하나만으로 blkg 를 찾아야 하기 때문이다. */
		per_cpu_ptr(blkg->iostat_cpu, cpu)->blkg = blkg;
	}

	/* [한국어] 정책 슬롯을 0..BLKCG_MAX_POLS-1 까지 훑으며, 이 큐에서 켜져 있는
	 * 정책마다 전용 데이터를 할당해 pd[i] 에 꽂는다. i 가 곧 pol->plid 다. */
	for (i = 0; i < BLKCG_MAX_POLS; i++) {
		/* [한국어] 전역 테이블에서 i 번 정책을 꺼낸다. 등록된 정책이 없으면 NULL. */
		struct blkcg_policy *pol = blkcg_policy[i];
		/* [한국어] 이 정책이 이 blkg 에 붙일 데이터. 정책마다 실제 타입이 다르지만
		 * 공통 헤더(blkg_policy_data)를 앞에 두는 규약으로 통일돼 있다. */
		struct blkg_policy_data *pd;

		/* [한국어] pol 이 NULL 이거나, 등록은 됐어도 이 큐에서 활성화되지 않았으면
		 * 건너뛴다. blkcg_policy_enabled() 는 q->blkcg_pols 비트맵의 plid 비트를
		 * 확인하는 헬퍼다(blk-cgroup.h). */
		if (!blkcg_policy_enabled(disk->queue, pol))
			continue;

		/* alloc per-policy data and attach it to blkg */
		/* [한국어] 정책이 자기 구조체를 직접 할당한다(예: blk-throttle 의
		 * throtl_grp, iocost 의 ioc_gq, iolatency 의 iolatency_grp). */
		pd = pol->pd_alloc_fn(disk, blkcg, gfp_mask);
		if (!pd)
			/* [한국어] 여기까지 할당한 pd 들을 역순으로 되돌려야 한다. */
			goto out_free_pds;
		/* [한국어] blkg 의 i 번 슬롯에 꽂는다. 이후 정책 코드는 blkg->pd[plid] 만
		 * 보면 자기 데이터를 찾을 수 있다. */
		blkg->pd[i] = pd;
		/* [한국어] pd → blkg 역참조. 정책 콜백은 pd 만 받으므로 여기서 blkg 로
		 * 되돌아갈 수 있어야 한다(pd_to_blkg 헬퍼가 이 필드를 쓴다). */
		pd->blkg = blkg;
		/* [한국어] 자기 슬롯 번호를 기록. blkcg_policy[pd->plid] 로 정책을 되찾는다. */
		pd->plid = i;
		/* [한국어] 아직 pd_online_fn 이 불리기 전이므로 false.
		 * blkg_create() 가 등록을 마친 뒤 online 으로 뒤집는다. */
		pd->online = false;
	}

	/* [한국어] 모든 준비 완료 — 아직 등록되지 않은 blkg 를 호출자에게 넘긴다. */
	return blkg;

out_free_pds:
	/* [한국어] 실패한 i 번은 할당되지 않았으므로 --i 부터(즉 직전 슬롯부터)
	 * 0 까지 역순으로 되돌린다. */
	while (--i >= 0)
		/* [한국어] 활성화되지 않아 건너뛴 슬롯은 NULL 이므로 검사한다. */
		if (blkg->pd[i])
			/* [한국어] 정책이 자기 방식대로 해제하도록 콜백을 부른다. */
			blkcg_policy[i]->pd_free_fn(blkg->pd[i]);
	/* [한국어] 위에서 얻은 큐 참조 반납. */
	blk_put_queue(disk->queue);
out_free_iostat:
	/* [한국어] per-cpu 통계 영역 반납. */
	free_percpu(blkg->iostat_cpu);
out_exit_refcnt:
	/* [한국어] percpu_ref 내부 per-cpu 카운터 반납. 아직 kill 도 get 도 없었으므로
	 * 곧바로 exit 해도 안전하다. */
	percpu_ref_exit(&blkg->refcnt);
out_free_blkg:
	/* [한국어] 마지막으로 구조체 자체 해제 후 실패를 알린다. */
	kfree(blkg);
	return NULL;
}

/*
 * If @new_blkg is %NULL, this function tries to allocate a new one as
 * necessary using %GFP_NOWAIT.  @new_blkg is always consumed on return.
 */
/*
 * [한국어]
 * blkg_create - blkg 를 만들어 blkcg 와 queue 양쪽 자료구조에 등록한다
 *
 * @blkcg: 이 blkg 가 대표할 cgroup.
 * @disk:  이 blkg 가 대표할 장치(실제 대상은 disk->queue).
 * @new_blkg: 호출자가 미리 할당해 둔 blkg. NULL 이면 이 함수가 GFP_NOWAIT 로
 *            직접 할당한다. 위 영문 주석대로 성공/실패와 무관하게 항상
 *            "소비"된다 — 실패 시 이 함수가 해제하므로 호출자는 다시 free
 *            하면 안 된다.
 * @return: 등록된 blkg, 실패 시 ERR_PTR(-ENODEV/-ENOMEM 등).
 *
 * === 이 함수가 blkg 를 "보이게" 만드는 세 곳 ===
 * (1) blkcg->blkg_tree (radix tree, 키 = queue->id) — blkg_lookup() 의 주 색인
 * (2) blkcg->blkg_list (hlist, RCU) — 이 cgroup 이 가진 blkg 전체 열거용
 * (3) q->blkg_list (list) — 이 큐에 붙은 blkg 전체 열거용(정책 활성화/해제,
 *     디스크 제거 시 일괄 정리에 쓰인다)
 * 이 세 등록을 blkcg->lock 안에서 원자적으로 처리해, 어느 한쪽에만 있는
 * 중간 상태가 다른 CPU 에 보이지 않게 한다.
 *
 * === 계층 연결 ===
 * 루트가 아닌 cgroup 의 blkg 는 반드시 "같은 큐에 대한 부모 cgroup 의 blkg" 를
 * parent 로 가져야 한다. 통계는 자식→부모로 합산되고, 정책 제한도 계층적으로
 * 적용되기 때문이다. 그래서 호출자(blkg_lookup_create)가 루트부터 아래로
 * 내려오며 순서대로 만들어 주고, 여기서는 부모가 이미 있다고 가정한다.
 * 없으면 WARN_ON_ONCE 로 잡히는 논리 오류다.
 *
 * 실행 컨텍스트: disk->queue->queue_lock 을 쥔 상태여야 한다(lockdep 로 강제).
 * 그 락이 spin_lock_irqsave 로 잡혀 있을 수 있으므로 잠들 수 없고,
 * 그래서 내부 할당이 GFP_NOWAIT 이다.
 *
 * 호출 체인:
 *   blkg_lookup_create() / blkcg_init_disk()
 *     → [blkg_create] → blkg_alloc(), radix_tree_insert(), pd_init_fn/pd_online_fn
 */
static struct blkcg_gq *blkg_create(struct blkcg *blkcg, struct gendisk *disk,
				    struct blkcg_gq *new_blkg)
{
	/* [한국어] 실제로 등록할 blkg (new_blkg 를 그대로 쓴다). */
	struct blkcg_gq *blkg;
	/* [한국어] i 는 정책 슬롯 인덱스, ret 는 오류 코드/삽입 결과. */
	int i, ret;

	/* [한국어] 이 함수는 queue_lock 을 이미 쥐고 있어야 한다. blkg 등록과
	 * 큐 상태(dying 여부, blkg_list) 확인이 원자적이어야 하기 때문이다.
	 * lockdep 이 꺼진 커널에서는 아무 코드도 생성되지 않는다. */
	lockdep_assert_held(&disk->queue->queue_lock);

	/* request_queue is dying, do not create/recreate a blkg */
	/* [한국어] 큐가 제거 중이면 새 blkg 를 만들어 봐야 곧 지워질 뿐이고,
	 * blkg_destroy_all() 이 이미 지나간 뒤라면 영영 정리되지 않는 누수가 된다.
	 * 그래서 -ENODEV 로 거절한다. */
	if (blk_queue_dying(disk->queue)) {
		ret = -ENODEV;
		goto err_free_blkg;
	}

	/* blkg holds a reference to blkcg */
	/* [한국어] blkg 는 자기 cgroup 의 css 참조를 하나 들고 산다.
	 * tryget_"online" 인 이유: 이미 오프라인(rmdir 진행 중)인 cgroup 에
	 * 새 blkg 를 붙이면, 그 cgroup 의 정리 루틴(blkcg_destroy_blkgs)이
	 * 이미 지나가 버려 blkg 가 남을 수 있기 때문이다. */
	if (!css_tryget_online(&blkcg->css)) {
		ret = -ENODEV;
		goto err_free_blkg;
	}

	/* allocate */
	/* [한국어] 호출자가 미리 만들어 준 blkg 가 없으면 여기서 만든다. */
	if (!new_blkg) {
		/* [한국어] queue_lock 을 쥔 상태라 잠들 수 없으므로 GFP_NOWAIT.
		 * 메모리 압박 시 실패할 수 있고, 그 실패는 치명적이지 않다 —
		 * 호출자가 가장 가까운 조상 blkg 로 대체한다. */
		new_blkg = blkg_alloc(blkcg, disk, GFP_NOWAIT);
		if (unlikely(!new_blkg)) {
			ret = -ENOMEM;
			goto err_put_css;
		}
	}
	/* [한국어] 이후 코드는 blkg 라는 이름으로 다룬다. */
	blkg = new_blkg;

	/* link parent */
	/* [한국어] 부모 cgroup 이 있다 == 루트가 아니다. 루트 blkg 는 parent 가 NULL. */
	if (blkcg_parent(blkcg)) {
		/* [한국어] "부모 cgroup + 같은 큐" 의 blkg 를 찾는다. 호출자가 루트부터
		 * 차례로 만들어 내려왔다면 반드시 존재한다. */
		blkg->parent = blkg_lookup(blkcg_parent(blkcg), disk->queue);
		/* [한국어] 없다면 생성 순서 규약이 깨진 것이므로 커널 버그다.
		 * _ONCE 라 로그 폭주 없이 한 번만 경고하고 실패 처리한다. */
		if (WARN_ON_ONCE(!blkg->parent)) {
			ret = -ENODEV;
			goto err_put_css;
		}
		/* [한국어] 자식이 사는 동안 부모가 해제되면 안 되므로 참조를 잡는다.
		 * 반납은 blkg_free_workfn() 의 blkg_put(blkg->parent). */
		blkg_get(blkg->parent);
	}

	/* invoke per-policy init */
	/* [한국어] pd 는 blkg_alloc 에서 "할당"만 됐다. 여기서 정책별 초기화 콜백을
	 * 불러 기본 제한값·계층 상속 등을 세팅한다. 아직 등록 전이라 다른 CPU 가
	 * 이 blkg 를 볼 수 없는 시점이므로 락 없이 안전하다. */
	for (i = 0; i < BLKCG_MAX_POLS; i++) {
		/* [한국어] i 번 슬롯에 해당하는 정책. */
		struct blkcg_policy *pol = blkcg_policy[i];

		/* [한국어] 이 큐에서 켜진 정책만 pd 가 있고, init 콜백은 선택 사항이다. */
		if (blkg->pd[i] && pol->pd_init_fn)
			pol->pd_init_fn(blkg->pd[i]);
	}

	/* insert */
	/* [한국어] blkcg 쪽 자료구조(blkg_tree, blkg_list)를 보호하는 락.
	 * queue_lock 은 이미 쥐고 있으므로, 여기서 잠금 순서는
	 * queue_lock → blkcg->lock 이다. 이 순서를 다른 곳에서도 지켜야 데드락이 없다. */
	spin_lock(&blkcg->lock);
	/* [한국어] queue->id 를 키로 radix tree 에 넣는다. 이 id 는 큐마다 유일한
	 * 정수이며, 덕분에 blkg_lookup() 이 (blkcg, q) → blkg 를 빠르게 찾는다.
	 * 같은 키가 이미 있으면 -EEXIST 가 돌아온다(정상 흐름에서는 발생하지 않음). */
	ret = radix_tree_insert(&blkcg->blkg_tree, disk->queue->id, blkg);
	/* [한국어] 삽입 성공 경로 — 나머지 등록을 이어서 한다. */
	if (likely(!ret)) {
		/* [한국어] cgroup 쪽 열거 리스트에 RCU 안전하게 추가한다.
		 * _rcu 변형은 포인터 공개 전에 쓰기 배리어를 넣어, 리스트를 따라온
		 * RCU 독자가 초기화되지 않은 blkg 를 보지 않도록 보장한다. */
		hlist_add_head_rcu(&blkg->blkcg_node, &blkcg->blkg_list);
		/* [한국어] 큐 쪽 열거 리스트에 추가한다. 이쪽은 queue_lock 으로 보호되며
		 * RCU 순회 대상이 아니라 _rcu 변형을 쓰지 않는다. */
		list_add(&blkg->q_node, &disk->queue->blkg_list);

		/* [한국어] 자료구조 등록이 끝났으니 이제 정책들을 online 으로 전환한다.
		 * 순서가 중요하다 — 등록 전에 online 으로 만들면 아직 조회되지 않는
		 * blkg 에 대해 정책이 활동을 시작할 수 있다. */
		for (i = 0; i < BLKCG_MAX_POLS; i++) {
			/* [한국어] i 번 슬롯의 정책. */
			struct blkcg_policy *pol = blkcg_policy[i];

			/* [한국어] pd 가 붙어 있는 슬롯만 처리한다. */
			if (blkg->pd[i]) {
				/* [한국어] online 콜백은 선택 사항. 정책이 이 시점에
				 * 부모와의 연결(weight 재계산 등)을 마무리한다. */
				if (pol->pd_online_fn)
					pol->pd_online_fn(blkg->pd[i]);
				/* [한국어] "이 pd 는 이제 IO 경로에서 써도 된다" 는 표시.
				 * 통계 출력 등에서 online 인 pd 만 다루는 근거가 된다. */
				blkg->pd[i]->online = true;
			}
		}
	}
	/* [한국어] blkg 전체를 online 으로 표시한다. 삽입이 실패한 경우에도 이 값을
	 * 세팅하지만, 곧바로 blkg_put 으로 해제 경로를 타므로 문제되지 않는다. */
	blkg->online = true;
	spin_unlock(&blkcg->lock);

	/* [한국어] radix tree 삽입까지 성공했으면 완성된 blkg 를 돌려준다. */
	if (!ret)
		return blkg;

	/* @blkg failed fully initialized, use the usual release path */
	/* [한국어] 여기 도달했다는 것은 blkg 가 이미 "완전히 초기화된" 상태라는 뜻이므로
	 * (부모 참조·pd·css 참조를 모두 들고 있다) 부분 해제(blkg_free)가 아니라
	 * 정상 참조 반납 경로를 태워야 한다. blkg_put → percpu_ref 0 →
	 * blkg_release → RCU → __blkg_release → blkg_free 순으로 정리된다. */
	blkg_put(blkg);
	return ERR_PTR(ret);

err_put_css:
	/* [한국어] css_tryget_online 으로 얻은 참조를 되돌린다. 아직 blkg 가
	 * 그 참조를 넘겨받지 못한 시점의 실패이므로 여기서 직접 놓는다. */
	css_put(&blkcg->css);
err_free_blkg:
	/* [한국어] 호출자가 넘겨준 new_blkg 든 여기서 할당한 것이든, 항상 이 함수가
	 * 소비(해제)한다는 규약을 지킨다. 아직 아무 자료구조에도 없고 참조도
	 * 늘지 않았으므로 부분 해제 경로인 blkg_free 가 맞다. */
	if (new_blkg)
		blkg_free(new_blkg);
	return ERR_PTR(ret);
}

/**
 * blkg_lookup_create - lookup blkg, try to create one if not there
 * @blkcg: blkcg of interest
 * @disk: gendisk of interest
 *
 * Lookup blkg for the @blkcg - @disk pair.  If it doesn't exist, try to
 * create one.  blkg creation is performed recursively from blkcg_root such
 * that all non-root blkg's have access to the parent blkg.  This function
 * should be called under RCU read lock and takes @disk->queue->queue_lock.
 *
 * Returns the blkg or the closest blkg if blkg_create() fails as it walks
 * down from root.
 */
/*
 * [한국어]
 * blkg_lookup_create - (blkcg, disk) 쌍의 blkg 를 찾고, 없으면 만들어 돌려준다
 *
 * @blkcg: 대상 cgroup.
 * @disk:  대상 장치.
 * @return: 원하는 blkg. 생성이 실패하면 위 영문 주석대로 "가장 가까운"
 *          조상 blkg(최악의 경우 q->root_blkg)를 돌려준다 — NULL 이나 ERR_PTR 을
 *          돌려주지 않으므로 호출자는 항상 유효한 blkg 를 얻는다.
 *
 * === blkg 조회 3단계 ===
 * 이 파일의 조회 경로는 비용 순서대로 세 단계다.
 *   1) blkcg->blkg_hint : 직전에 쓴 blkg 하나를 캐시. 한 태스크가 같은 장치에
 *      연속으로 IO 를 내는 흔한 패턴에서 대부분 여기서 끝난다.
 *      (이 검사는 blk-cgroup.h 의 blkg_lookup() 안에 있다.)
 *   2) blkcg->blkg_tree : queue->id 를 키로 하는 radix tree 조회. hint 가
 *      빗나갔을 때 쓰이며, 성공하면 hint 를 갱신한다.
 *   3) 생성 : 위 둘이 모두 실패하면 이 함수가 queue_lock 을 잡고 blkg_create().
 * 1)과 2)는 RCU read-side 에서 락 없이 수행되고, 3)만 락을 잡는다.
 *
 * === 왜 루트부터 내려오며 만드는가 ===
 * blkg 는 부모 blkg 를 반드시 가져야 한다(계층 통계/제한 때문). 그래서 목표
 * cgroup 의 조상 중 blkg 가 없는 가장 위쪽을 찾아 거기부터 한 단계씩 만든다.
 * 도중에 실패하면, 그때까지 확인해 둔 "가장 가까운 조상 blkg" 로 되돌려
 * IO 가 최소한 상위 cgroup 몫으로는 계산되게 한다.
 *
 * 실행 컨텍스트: rcu_read_lock() 을 쥔 상태로 호출해야 하며(WARN 으로 확인),
 * 내부에서 q->queue_lock 을 irqsave 로 잡는다. 잠들 수 없다.
 *
 * 호출 체인:
 *   bio_associate_blkg_from_css() → [blkg_lookup_create]
 *     → blkg_lookup() / blkg_create()
 */
static struct blkcg_gq *blkg_lookup_create(struct blkcg *blkcg,
		struct gendisk *disk)
{
	/* [한국어] blkg 는 gendisk 가 아니라 request_queue 에 매달리므로 큐를 꺼낸다. */
	struct request_queue *q = disk->queue;
	/* [한국어] 조회 또는 생성 결과. */
	struct blkcg_gq *blkg;
	/* [한국어] spin_lock_irqsave 가 저장할 인터럽트 상태. */
	unsigned long flags;

	/* [한국어] blkg_lookup() 이 RCU 보호를 전제로 포인터를 읽으므로,
	 * 호출자가 rcu_read_lock 을 쥐고 들어왔는지 확인한다(디버그 커널 한정). */
	WARN_ON_ONCE(!rcu_read_lock_held());

	/* [한국어] 1·2단계(hint → radix tree)를 락 없이 시도한다. 대부분 여기서 끝난다. */
	blkg = blkg_lookup(blkcg, q);
	if (blkg)
		return blkg;

	/* [한국어] 생성이 필요하다. blkg 등록은 queue_lock 을 요구하고,
	 * 이 락은 IO 완료(IRQ) 경로에서도 잡히므로 irqsave 변형을 쓴다. */
	spin_lock_irqsave(&q->queue_lock, flags);
	/* [한국어] 락을 잡는 사이 다른 CPU 가 같은 blkg 를 만들었을 수 있으므로
	 * 다시 확인한다(전형적인 double-checked locking). */
	blkg = blkg_lookup(blkcg, q);
	if (blkg) {
		/* [한국어] 남이 만들어 둔 blkg 를 찾았다. 이번 조회가 hint 를 빗나갔다는
		 * 뜻이므로 hint 를 이 blkg 로 갱신해 다음 조회를 1단계에서 끝내게 한다.
		 * 루트는 어차피 별도 경로(q->root_blkg)로 빠르게 찾으므로 제외한다. */
		if (blkcg != &blkcg_root &&
		    blkg != rcu_dereference(blkcg->blkg_hint))
			/* [한국어] rcu_assign_pointer 는 쓰기 배리어를 포함해,
			 * hint 를 따라온 RCU 독자가 부분 초기화된 객체를 보지 않게 한다. */
			rcu_assign_pointer(blkcg->blkg_hint, blkg);
		goto found;
	}

	/*
	 * Create blkgs walking down from blkcg_root to @blkcg, so that all
	 * non-root blkgs have access to their parents.  Returns the closest
	 * blkg to the intended blkg should blkg_create() fail.
	 */
	/* [한국어] 바깥 루프는 "한 단계 만들기" 를 목표에 도달할 때까지 반복한다.
	 * 매 회차마다 다시 조상을 훑는 이유는, 방금 만든 blkg 덕분에 다음 회차의
	 * "가장 가까운 기존 blkg" 위치가 한 칸씩 내려오기 때문이다. */
	while (true) {
		/* [한국어] 이번 회차에 실제로 생성할 cgroup. 초기값은 목표 cgroup 이고,
		 * 아래 안쪽 루프가 조상 쪽으로 끌어올린다. */
		struct blkcg *pos = blkcg;
		/* [한국어] 조상 탐색 커서. 루트에 도달하면 NULL 이 되어 루프가 끝난다. */
		struct blkcg *parent = blkcg_parent(blkcg);
		/* [한국어] 생성 실패 시 대신 돌려줄 blkg. 최악의 경우가 루트 blkg 이며,
		 * 루트 blkg 는 디스크 초기화 때 미리 만들어져 항상 존재한다. */
		struct blkcg_gq *ret_blkg = q->root_blkg;

		/* [한국어] 위로 올라가며 blkg 가 이미 있는 가장 가까운 조상을 찾는다. */
		while (parent) {
			/* [한국어] 이 조상 cgroup 에 대한 blkg 가 있는지 조회. */
			blkg = blkg_lookup(parent, q);
			if (blkg) {
				/* remember closest blkg */
				/* [한국어] 찾았다. 실패 시 이 blkg 로 대체하면 되고,
				 * 지금 만들어야 할 것은 그 바로 아래인 pos 다. */
				ret_blkg = blkg;
				break;
			}
			/* [한국어] 이 조상도 blkg 가 없다 — 생성 지점을 한 칸 위로 올린다. */
			pos = parent;
			/* [한국어] 계속 위로. 루트까지 없으면 parent 가 NULL 이 되어 종료. */
			parent = blkcg_parent(parent);
		}

		/* [한국어] pos 의 부모 blkg 는 이제 확실히 존재하므로 안전하게 만들 수 있다.
		 * 세 번째 인자 NULL 은 "blkg 를 직접 할당하라(GFP_NOWAIT)" 는 뜻. */
		blkg = blkg_create(pos, disk, NULL);
		if (IS_ERR(blkg)) {
			/* [한국어] 메모리 부족이나 큐 소멸로 실패. 오류를 위로 던지지 않고
			 * 가장 가까운 조상 blkg 를 결과로 삼는다 — IO 를 실패시키는 것보다
			 * 상위 cgroup 몫으로 처리하는 편이 낫기 때문이다. */
			blkg = ret_blkg;
			break;
		}
		/* [한국어] 방금 만든 것이 목표 cgroup 이면 완료. 아니면 한 단계 아래를
		 * 만들기 위해 바깥 루프를 한 번 더 돈다. */
		if (pos == blkcg)
			break;
	}

found:
	/* [한국어] 락 해제 및 인터럽트 상태 복원. 이 시점 이후 blkg 는 RCU 독자와
	 * 다른 CPU 에게도 보인다. */
	spin_unlock_irqrestore(&q->queue_lock, flags);
	return blkg;
}

/*
 * [한국어]
 * blkg_destroy - blkg 를 조회 가능한 자료구조에서 떼어내고 참조를 kill 한다
 *
 * @blkg: 제거할 blkg.
 * @return: 없음. 실제 메모리 해제는 참조가 0 이 된 뒤 비동기로 일어난다.
 *
 * === "제거" 의 두 단계 ===
 * 이 함수는 blkg 를 "새로 찾을 수 없게" 만들 뿐, 메모리를 놓지는 않는다.
 *   1) 정책들을 offline 시키고(pd_offline_fn), online 플래그를 내린다.
 *   2) radix tree 와 blkcg_node 해시에서 뺀다 → 이제 blkg_lookup() 이 못 찾는다.
 *   3) hint 가 이 blkg 를 가리키면 지운다.
 *   4) percpu_ref_kill() — 생성 시 잡아 둔 초기 참조를 놓고 atomic 모드로 전환.
 * 이미 이 blkg 를 참조하고 있는 진행 중인 IO 는 그대로 끝까지 진행되고,
 * 마지막 참조가 반납되는 순간 blkg_release() 사슬이 시작된다.
 * 주의: q->blkg_list 에서는 여기서 빼지 않는다 — 위 영문 주석대로 그 제거는
 * pd 해제 순서를 지키기 위해 blkg_free_workfn() 까지 미뤄진다. 그래서 이
 * 함수는 같은 blkg 에 대해 두 번 불릴 수 있고(cgroup 삭제 경로와 디스크 제거
 * 경로가 겹칠 때), hlist_unhashed 검사로 두 번째 호출을 걸러낸다.
 *
 * 실행 컨텍스트: blkg->q->queue_lock 과 blkcg->lock 을 둘 다 쥔 상태여야 한다
 * (lockdep 으로 강제). 잠글 수 없는 스핀락 구간이다.
 *
 * 호출 체인:
 *   blkcg_destroy_blkgs()(cgroup 삭제) / blkg_destroy_all()(디스크 제거)
 *     → [blkg_destroy] → pd_offline_fn / radix_tree_delete / percpu_ref_kill
 */
static void blkg_destroy(struct blkcg_gq *blkg)
{
	/* [한국어] 이 blkg 의 소유 cgroup. blkg_tree/blkg_list 가 여기에 있다. */
	struct blkcg *blkcg = blkg->blkcg;
	/* [한국어] 정책 슬롯 순회 인덱스. */
	int i;

	/* [한국어] 두 락을 모두 요구한다. queue 쪽 자료구조와 blkcg 쪽 자료구조를
	 * 동시에 건드리기 때문이며, 잠금 순서는 queue_lock → blkcg->lock 이다. */
	lockdep_assert_held(&blkg->q->queue_lock);
	lockdep_assert_held(&blkcg->lock);

	/*
	 * blkg stays on the queue list until blkg_free_workfn(), see details in
	 * blkg_free_workfn(), hence this function can be called from
	 * blkcg_destroy_blkgs() first and again from blkg_destroy_all() before
	 * blkg_free_workfn().
	 */
	/* [한국어] blkcg_node 가 이미 해시에서 빠져 있다면 이 blkg 는 이미 한 번
	 * destroy 된 것이다(위 영문 주석의 이중 호출 시나리오). 조용히 돌아간다. */
	if (hlist_unhashed(&blkg->blkcg_node))
		return;

	/* [한국어] 붙어 있는 정책들을 먼저 offline 시킨다. 정책이 이 blkg 를 대상으로
	 * 진행 중이던 활동(타이머, 대기 큐 등)을 정리할 기회를 주는 단계다. */
	for (i = 0; i < BLKCG_MAX_POLS; i++) {
		/* [한국어] i 번 슬롯의 정책. */
		struct blkcg_policy *pol = blkcg_policy[i];

		/* [한국어] pd 가 있고 아직 online 인 것만 내린다 — 이중 offline 방지. */
		if (blkg->pd[i] && blkg->pd[i]->online) {
			/* [한국어] 먼저 플래그를 내려, 콜백이 도는 동안 다른 경로가
			 * 이 pd 를 online 으로 오인하지 않게 한다. */
			blkg->pd[i]->online = false;
			/* [한국어] offline 콜백은 선택 사항. */
			if (pol->pd_offline_fn)
				pol->pd_offline_fn(blkg->pd[i]);
		}
	}

	/* [한국어] blkg 자체도 offline 표시. 통계 출력 등이 이 플래그를 본다. */
	blkg->online = false;

	/* [한국어] radix tree 색인에서 제거 — 이 순간부터 blkg_lookup() 의 2단계가
	 * 이 blkg 를 찾지 못한다. 키는 생성 때와 같은 queue id. */
	radix_tree_delete(&blkcg->blkg_tree, blkg->q->id);
	/* [한국어] cgroup 의 blkg 해시에서도 제거한다. _rcu 변형이라 RCU 독자가
	 * 순회 중이어도 안전하고, _init 이라 노드가 "빠진 상태" 로 표시되어
	 * 위의 hlist_unhashed 재진입 검사가 성립한다. */
	hlist_del_init_rcu(&blkg->blkcg_node);

	/*
	 * Both setting lookup hint to and clearing it from @blkg are done
	 * under queue_lock.  If it's not pointing to @blkg now, it never
	 * will.  Hint assignment itself can race safely.
	 */
	/* [한국어] 캐시(hint)가 지금 지우는 blkg 를 가리키고 있으면 비운다.
	 * rcu_access_pointer 는 "역참조하지 않고 포인터 값만 비교" 할 때 쓰는
	 * 접근자라 RCU 락 없이도 lockdep 경고가 나지 않는다.
	 * 영문 주석의 요지: hint 설정도 해제도 모두 queue_lock 아래에서 일어나므로,
	 * 지금 이 blkg 를 가리키지 않는다면 앞으로도 가리킬 일이 없다. */
	if (rcu_access_pointer(blkcg->blkg_hint) == blkg)
		rcu_assign_pointer(blkcg->blkg_hint, NULL);

	/*
	 * Put the reference taken at the time of creation so that when all
	 * queues are gone, group can be destroyed.
	 */
	/* [한국어] 생성 시점에 잡혀 있던 초기 참조 1 을 반납한다. 동시에 percpu_ref 가
	 * per-cpu 모드에서 atomic 모드로 바뀌어, 이제부터 실제 0 도달을 판정할 수
	 * 있게 된다. 진행 중인 IO 가 들고 있는 참조가 모두 반납되면
	 * blkg_release() 가 호출된다. */
	percpu_ref_kill(&blkg->refcnt);
}

/*
 * [한국어]
 * blkg_destroy_all - 한 디스크에 붙어 있는 모든 blkg 를 정리한다
 *
 * @disk: 사라지는 디스크. 정리 대상은 disk->queue 의 blkg_list 전체.
 * @return: 없음.
 *
 * 왜 필요한가: 디스크가 제거되면 그 큐와 짝지어진 blkg 는 전부 의미를 잃는다.
 * blkg 는 (blkcg, queue) 2차원 격자의 한 칸이므로, 큐 하나가 사라진다는 것은
 * 그 큐에 해당하는 "열" 전체를 지운다는 뜻이다. cgroup 쪽에서 지우는 반대편
 * 함수가 blkcg_destroy_blkgs()(cgroup 하나의 "행" 을 지움)다.
 *
 * === 배치 처리와 재시작 ===
 * cgroup 이 수천 개인 시스템에서는 blkg 도 그만큼 많다. queue_lock 을 쥔 채
 * 전부 지우면 인터럽트가 오래 막혀 softlockup 경고가 뜬다. 그래서 64개
 * (BLKG_DESTROY_BATCH_SIZE)마다 락을 놓고 cond_resched() 로 양보한 뒤
 * restart 라벨로 돌아가 리스트를 처음부터 다시 훑는다. 이미 처리된 blkg 는
 * blkcg_node 가 unhashed 라 continue 로 건너뛰므로 중복 작업은 없다.
 * (리스트를 처음부터 다시 훑는 이유: 락을 놓은 사이 리스트가 바뀔 수 있어
 *  중단 지점 커서를 신뢰할 수 없기 때문이다.)
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(디스크 해제 경로). cond_resched() 를
 * 부르므로 아토믹 문맥에서 호출하면 안 된다.
 *
 * 호출 체인:
 *   del_gendisk() → blkcg_exit_disk() → [blkg_destroy_all] → blkg_destroy()
 */
static void blkg_destroy_all(struct gendisk *disk)
{
	/* [한국어] blkg 들이 매달려 있는 큐. */
	struct request_queue *q = disk->queue;
	/* [한국어] 리스트 순회 커서. */
	struct blkcg_gq *blkg;
	/* [한국어] 이번 배치에서 앞으로 몇 개 더 지울 수 있는지 남은 예산. */
	int count = BLKG_DESTROY_BATCH_SIZE;
	/* [한국어] 마지막에 정책 비트를 지울 때 쓰는 슬롯 인덱스. */
	int i;

restart:
	/* [한국어] blkg_list 순회와 blkg_destroy 는 queue_lock 을 요구한다.
	 * IO 완료 경로(IRQ)와도 경쟁하므로 _irq 변형으로 로컬 인터럽트를 막는다. */
	spin_lock_irq(&q->queue_lock);
	/* [한국어] 이 큐에 붙은 blkg 를 차례로 훑는다. blkg_destroy() 는 q_node 를
	 * 리스트에서 빼지 않으므로(해제는 blkg_free_workfn 에서) 순회 중 커서가
	 * 무효화되지 않는다 — 이것이 안전하게 list_for_each_entry 를 쓸 수 있는 이유다. */
	list_for_each_entry(blkg, &q->blkg_list, q_node) {
		/* [한국어] blkg 마다 소유 cgroup 이 다르므로 매번 꺼내 그 락을 잡는다. */
		struct blkcg *blkcg = blkg->blkcg;

		/* [한국어] 이미 지워진 blkg(직전 배치에서 처리했거나 cgroup 삭제 경로가
		 * 먼저 처리한 것)는 건너뛴다. */
		if (hlist_unhashed(&blkg->blkcg_node))
			continue;

		/* [한국어] 잠금 순서 규약대로 queue_lock 다음에 blkcg->lock 을 잡는다. */
		spin_lock(&blkcg->lock);
		/* [한국어] 실제 제거(정책 offline, 색인 제거, percpu_ref kill). */
		blkg_destroy(blkg);
		spin_unlock(&blkcg->lock);

		/*
		 * in order to avoid holding the spin lock for too long, release
		 * it when a batch of blkgs are destroyed.
		 */
		/* [한국어] 예산을 하나 소진하고 0 이 되면 배치 경계다. */
		if (!(--count)) {
			/* [한국어] 다음 배치 예산 재충전. */
			count = BLKG_DESTROY_BATCH_SIZE;
			/* [한국어] 락과 인터럽트를 풀어 다른 CPU/인터럽트가 진행할 틈을 준다. */
			spin_unlock_irq(&q->queue_lock);
			/* [한국어] 필요하면 스케줄러에 양보한다(선점 불가 커널에서도
			 * 여기서 다른 태스크가 돌 수 있게 된다). */
			cond_resched();
			/* [한국어] 락을 놓았으므로 리스트를 처음부터 다시 훑는다. */
			goto restart;
		}
	}

	/*
	 * Mark policy deactivated since policy offline has been done, and
	 * the free is scheduled, so future blkcg_deactivate_policy() can
	 * be bypassed
	 */
	/* [한국어] 모든 blkg 의 정책이 offline 되고 해제가 예약됐으므로,
	 * 이 큐의 "정책 활성" 비트맵을 통째로 비운다. 이후 누군가
	 * blkcg_deactivate_policy() 를 불러도 비트가 없어 곧바로 반환한다. */
	for (i = 0; i < BLKCG_MAX_POLS; i++) {
		/* [한국어] i 번 슬롯에 등록된 정책(없으면 NULL). */
		struct blkcg_policy *pol = blkcg_policy[i];

		/* [한국어] 등록된 정책에 대해서만 비트를 지운다. */
		if (pol)
			/* [한국어] __clear_bit 은 원자적이지 않은 비트 클리어다.
			 * queue_lock 을 쥐고 있어 배타성이 보장되므로 원자 버전이 불필요하다. */
			__clear_bit(pol->plid, q->blkcg_pols);
	}

	/* [한국어] 루트 blkg 포인터도 끊는다. 이것이 NULL 이 되었다는 것은
	 * "이 큐에는 더 이상 blkcg 구조가 없다" 는 신호다. */
	q->root_blkg = NULL;
	spin_unlock_irq(&q->queue_lock);

	/* [한국어] blkcg_init_disk() 가 같은 큐를 다시 초기화하려고
	 * root_blkg 가 NULL 이 되기를 기다리고 있을 수 있으므로 깨운다.
	 * wake_up_var 는 변수 주소를 키로 쓰는 대기/통지 메커니즘이다. */
	wake_up_var(&q->root_blkg);
}

/*
 * [한국어]
 * blkg_iostat_set - I/O 통계 구조체를 통째로 복사(대입)
 *
 * @dst: 복사 대상
 * @src: 복사 원본
 * @return: 없음
 *
 * blkg_iostat은 read/write/discard 세 방향의 bytes[]와 ios[] 배열을 담은
 * 구조체다. 구조체 대입(*dst = *src) 대신 필드를 하나씩 도는 이유는,
 * 이 구조체가 u64_stats_sync로 보호되는 seqlock 영역 안에서 다뤄지기 때문에
 * 컴파일러가 임의로 최적화하거나 재배치하지 않도록 명시적으로 쓰기 위해서다.
 *
 * 실행 컨텍스트: u64_stats_update_begin/end 구간 안(호출자가 보장).
 *
 * 호출 체인:
 *   __blkg_clear_stat / blkcg_iostat_update / blkg_iostat_add 등 → [blkg_iostat_set]
 */
static void blkg_iostat_set(struct blkg_iostat *dst, struct blkg_iostat *src)
{
	/* [한국어] 통계 분류 인덱스. BLKG_IOSTAT_NR 은 아래 세 분류의 개수(3). */
	int i;

	/* [한국어] read/write/discard 세 분류를 순회하며 복사한다. 이 분류는 같은
	 * 파일 아래쪽의 blk_cgroup_io_type() 이 bio->bi_opf 에서 결정한다:
	 * discard 면 BLKG_IOSTAT_DISCARD, 쓰기면 WRITE, 나머지는 READ. */
	for (i = 0; i < BLKG_IOSTAT_NR; i++) {
		/* [한국어] 해당 분류의 누적 바이트 수 복사. */
		dst->bytes[i] = src->bytes[i];
		/* [한국어] 해당 분류의 누적 IO 건수 복사. */
		dst->ios[i] = src->ios[i];
	}
}

/*
 * [한국어]
 * __blkg_clear_stat - per-CPU 통계 슬롯 하나를 0으로 초기화
 *
 * @bis: 초기화할 per-CPU 통계 집합(cur + last + seqlock)
 * @return: 없음
 *
 * blkg_iostat_set 구조체는 두 개의 통계를 갖는다:
 *   cur  - 지금까지 이 CPU에서 누적된 값
 *   last - 마지막으로 상위(부모 blkg)로 전파할 때의 스냅숏
 * 둘의 차이가 "아직 전파하지 않은 증분"이며, 그래서 초기화할 때 둘 다
 * 0으로 맞춰야 한다. cur만 지우면 last가 남아 다음 전파에서 음수 증분이
 * 계산된다.
 *
 * u64_stats_update_begin_irqsave/end_irqrestore로 감싸는 이유: 32비트
 * 아키텍처에서 u64 값은 두 번의 32비트 쓰기로 나뉘어, 그 사이에 읽는
 * 쪽이 앞뒤가 섞인 값(torn read)을 볼 수 있다. seqlock이 읽는 쪽에
 * 재시도를 시켜 그것을 막는다. 64비트에서는 이 매크로가 IRQ 비활성화만
 * 남기고 사실상 사라진다.
 *
 * 실행 컨텍스트: cgroup 통계 리셋 경로(프로세스 컨텍스트). IRQ를 끄고 진행.
 *
 * 호출 체인:
 *   blkg_clear_stat → [__blkg_clear_stat] → blkg_iostat_set
 */
static void __blkg_clear_stat(struct blkg_iostat_set *bis)
{
	/* [한국어] 전부 0 인 임시 구조체. 이것을 복사해 넣는 방식으로 초기화한다. */
	struct blkg_iostat cur = {0};
	/* [한국어] u64_stats_update_begin_irqsave 가 저장하는 인터럽트 상태. */
	unsigned long flags;

	/* [한국어] 쓰기 측 seqcount 구간 진입. 인터럽트도 함께 끄는 이유는
	 * 같은 CPU 의 IRQ 핸들러가 통계를 갱신하러 들어오면 seqcount 가 꼬이기 때문. */
	flags = u64_stats_update_begin_irqsave(&bis->sync);
	/* [한국어] 현재 누적치를 0 으로. */
	blkg_iostat_set(&bis->cur, &cur);
	/* [한국어] 마지막 전파 스냅숏도 0 으로. cur 만 지우면 (cur - last) 가 음수가 된다. */
	blkg_iostat_set(&bis->last, &cur);
	/* [한국어] seqcount 구간 종료 및 인터럽트 상태 복원. 이 시점에 독자는
	 * 일관된 0 값을 보게 된다. */
	u64_stats_update_end_irqrestore(&bis->sync, flags);
}

/*
 * [한국어]
 * blkg_clear_stat - 한 blkg의 모든 CPU 통계와 전역 통계를 초기화
 *
 * @blkg: 초기화할 blkcg_gq(= cgroup × 디스크 조합 하나)
 * @return: 없음
 *
 * 통계가 per-CPU에 흩어져 있으므로 모든 CPU의 슬롯을 순회해야 한다.
 * for_each_possible_cpu를 쓰는 이유가 중요하다 — online CPU만 돌면,
 * 지금 오프라인이지만 과거에 값을 쌓아 둔 CPU의 통계가 남는다. 그 CPU가
 * 나중에 온라인되면 지워졌어야 할 값이 되살아난다.
 *
 * 사용 시점: 사용자가 cgroup의 io.stat을 리셋하거나, blkg가 새로 만들어질 때
 * 이전 사용의 잔재를 없앤다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   blkcg_reset_stats(cgroupfs write) → [blkg_clear_stat] → __blkg_clear_stat
 */
static void blkg_clear_stat(struct blkcg_gq *blkg)
{
	/* [한국어] per-cpu 슬롯 순회 인덱스. */
	int cpu;

	/* [한국어] online 이 아니라 possible 을 도는 이유는 위 함수 주석 참조 —
	 * 지금 오프라인인 CPU 에 남은 값이 되살아나는 것을 막기 위해서다. */
	for_each_possible_cpu(cpu) {
		/* [한국어] cpu 번째 per-cpu 통계 슬롯 주소. */
		struct blkg_iostat_set *s = per_cpu_ptr(blkg->iostat_cpu, cpu);

		/* [한국어] 그 슬롯의 cur/last 를 모두 0 으로. */
		__blkg_clear_stat(s);
	}
	/* [한국어] 마지막으로 blkg 의 전역(합산) 통계도 0 으로. */
	__blkg_clear_stat(&blkg->iostat);
}

/*
 * [한국어]
 * blkcg_reset_stats - cgroupfs 의 blkio.reset_stats 쓰기 핸들러
 *
 * @css:    대상 cgroup 의 css. css_to_blkcg 로 blkcg 를 얻는다.
 * @cftype: 쓰기가 일어난 cgroupfs 파일 정보. 여기서는 deprecated 경고 메시지에
 *          파일 이름을 넣는 용도로만 쓴다.
 * @val:    사용자가 쓴 값. 이 함수는 값을 무시하고 "쓰기가 있었다" 는 사실만 본다.
 * @return: 항상 0(성공). cgroup 코어가 이 값을 write(2) 결과로 사용자에게 돌려준다.
 *
 * 왜 필요한가: cgroup v1(legacy) 인터페이스에는 통계를 0 으로 되돌리는 파일이
 * 있었다. v2 에는 대응 파일이 없어 deprecated 경고를 남기며, 실제로 이
 * cgroup 이 가진 모든 blkg 의 통계와 정책별 통계를 초기화한다.
 *
 * 경쟁 조건: 위 영문 주석이 명시하듯 이 초기화는 통계 갱신과 동기화되지 않는다.
 * 초기화 도중 들어온 IO 의 수치는 지워질 수도, 남을 수도 있다. 디버그 기능이라
 * 그 정도 부정확성을 감수한다.
 *
 * 실행 컨텍스트: cgroupfs write(프로세스 컨텍스트). blkcg_pol_mutex(잠들 수 있음)를
 * 먼저 잡고, 그 안에서 blkcg->lock 을 irq-safe 로 잡는다.
 *
 * 호출 체인:
 *   userspace write("blkio.reset_stats") → cgroup 코어 → [blkcg_reset_stats]
 *     → blkg_clear_stat() / pol->pd_reset_stats_fn()
 */
static int blkcg_reset_stats(struct cgroup_subsys_state *css,
			     struct cftype *cftype, u64 val)
{
	/* [한국어] css 를 감싸고 있는 blkcg 로 되돌린다(container_of 래퍼). */
	struct blkcg *blkcg = css_to_blkcg(css);
	/* [한국어] blkg_list 순회 커서. */
	struct blkcg_gq *blkg;
	/* [한국어] 정책 슬롯 인덱스. */
	int i;

	/* [한국어] 사용 중단 예정임을 부팅 후 한 번만 알린다(_once). */
	pr_info_once("blkio.%s is deprecated\n", cftype->name);
	/* [한국어] 순회 중 정책이 등록/해제되면 pd_reset_stats_fn 포인터가 흔들리므로
	 * 정책 테이블을 고정한다. 뮤텍스라 잠들 수 있는 문맥이어야 한다. */
	mutex_lock(&blkcg_pol_mutex);
	/* [한국어] 이 cgroup 의 blkg_list 를 순회하는 동안 목록이 바뀌지 않도록 보호.
	 * blkg 생성/삭제가 IRQ 를 끈 상태에서도 이 락을 잡으므로 _irq 변형을 쓴다. */
	spin_lock_irq(&blkcg->lock);

	/*
	 * Note that stat reset is racy - it doesn't synchronize against
	 * stat updates.  This is a debug feature which shouldn't exist
	 * anyway.  If you get hit by a race, retry.
	 */
	/* [한국어] 이 cgroup 이 쓰고 있는 모든 장치의 blkg 를 훑는다. 즉 2차원 격자에서
	 * 이 cgroup 에 해당하는 "행" 전체다. */
	hlist_for_each_entry(blkg, &blkcg->blkg_list, blkcg_node) {
		/* [한국어] 공통 IO 통계(bytes/ios)를 모든 CPU 슬롯까지 0 으로. */
		blkg_clear_stat(blkg);
		/* [한국어] 이어서 정책들이 따로 관리하는 통계도 초기화한다. */
		for (i = 0; i < BLKCG_MAX_POLS; i++) {
			/* [한국어] i 번 슬롯의 정책. */
			struct blkcg_policy *pol = blkcg_policy[i];

			/* [한국어] pd 가 붙어 있고 리셋 콜백을 제공하는 정책만 호출한다. */
			if (blkg->pd[i] && pol->pd_reset_stats_fn)
				pol->pd_reset_stats_fn(blkg->pd[i]);
		}
	}

	/* [한국어] 역순으로 락 해제. */
	spin_unlock_irq(&blkcg->lock);
	mutex_unlock(&blkcg_pol_mutex);
	/* [한국어] 항상 성공으로 처리한다 — 실패할 만한 자원 할당이 없다. */
	return 0;
}

/*
 * [한국어]
 * blkg_dev_name - blkg가 가리키는 블록 장치의 이름 문자열을 얻는다
 *
 * @blkg: 이름을 알고 싶은 blkcg_gq
 * @return: "nvme0n1" 같은 장치 이름. 디스크가 아직 없으면 NULL.
 *
 * cgroupfs의 io.stat, io.max 등은 "장치이름 값" 형식으로 출력되므로 blkg마다
 * 대응하는 장치 이름이 필요하다. bdi(backing_dev_info)의 이름을 쓰는 이유는
 * 그것이 파티션이 아닌 디스크 단위 이름을 주기 때문이다.
 *
 * NULL이 반환될 수 있는 상황: blkg는 request_queue 단위로 만들어지는데, 큐가
 * gendisk보다 먼저 존재할 수 있다(큐를 만든 뒤 디스크를 붙이는 순서).
 * 그 짧은 구간에 통계를 출력하려 하면 이름이 없고, 호출자는 그 blkg를
 * 출력에서 건너뛴다.
 *
 * 실행 컨텍스트: cgroupfs read(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   __blkg_prfill_u64 / blkcg_print_one_stat 등 → [blkg_dev_name]
 */
const char *blkg_dev_name(struct blkcg_gq *blkg)
{
	/* [한국어] 큐는 있지만 아직 gendisk 가 붙지 않은 구간이 존재한다.
	 * 그때는 이름을 만들 수 없으므로 NULL 을 돌려주고, 호출자가 이 blkg 를
	 * 출력에서 건너뛴다. */
	if (!blkg->q->disk)
		return NULL;
	/* [한국어] backing_dev_info 에 등록된 장치 이름을 돌려준다.
	 * 이 이름이 cgroupfs 출력의 "<장치이름> <값>" 앞부분이 된다. */
	return bdi_dev_name(blkg->q->disk->bdi);
}

/**
 * blkcg_print_blkgs - helper for printing per-blkg data
 * @sf: seq_file to print to
 * @blkcg: blkcg of interest
 * @prfill: fill function to print out a blkg
 * @pol: policy in question
 * @data: data to be passed to @prfill
 * @show_total: to print out sum of prfill return values or not
 *
 * This function invokes @prfill on each blkg of @blkcg if pd for the
 * policy specified by @pol exists.  @prfill is invoked with @sf, the
 * policy data and @data and the matching queue lock held.  If @show_total
 * is %true, the sum of the return values from @prfill is printed with
 * "Total" label at the end.
 *
 * This is to be used to construct print functions for
 * cftype->read_seq_string method.
 */
/*
 * [한국어] (위 영문 kernel-doc 참고)
 * blkcg_print_blkgs - 한 cgroup에 속한 모든 blkg를 순회하며 출력 콜백을 호출
 *
 * @sf:         출력 대상 seq_file
 * @blkcg:      순회 기준이 되는 cgroup
 * @prfill:     blkg마다 호출될 출력 콜백. 정책이 자기 형식에 맞게 제공한다.
 * @pol:        대상 정책. 이 정책의 policy_data가 없는 blkg는 건너뛴다.
 * @data:       prfill에 그대로 전달되는 값(보통 policy_data 안의 필드 오프셋)
 * @show_total: true면 모든 prfill 반환값의 합을 마지막에 "Total"로 출력
 * @return: 없음
 *
 * cgroupfs의 정책별 통계/설정 파일을 읽을 때 공통으로 쓰는 순회 골격이다.
 * 각 정책(blk-throttle, blk-iolatency, BFQ)은 출력 형식만 prfill 콜백으로
 * 제공하고, "어떤 blkg들을 어떤 순서로, 어떤 락 아래에서 도는가"라는 공통
 * 문제는 이 함수가 한 번에 해결한다.
 *
 * @pol의 policy_data가 없는 blkg를 건너뛰는 이유: 정책은 장치마다 개별적으로
 * 활성화되므로, 같은 cgroup 안에서도 어떤 디스크에는 blk-throttle이 붙어
 * 있고 어떤 디스크에는 없을 수 있다. 없는 쪽은 출력할 값 자체가 없다.
 *
 * 실행 컨텍스트: cgroupfs read(프로세스 컨텍스트). 각 blkg의 큐 락을 잡은
 * 상태로 prfill을 호출하므로, 콜백 안에서 잠들면 안 된다.
 *
 * 호출 체인:
 *   cgroupfs read → 정책의 seq_show 콜백 → [blkcg_print_blkgs]
 *     → prfill (예: __blkg_prfill_u64, blkg_prfill_rwstat)
 */
void blkcg_print_blkgs(struct seq_file *sf, struct blkcg *blkcg,
		       u64 (*prfill)(struct seq_file *,
				     struct blkg_policy_data *, int),
		       const struct blkcg_policy *pol, int data,
		       bool show_total)
{
	/* [한국어] blkg_list 순회 커서. 이 cgroup 이 쓰는 장치 수만큼 돈다. */
	struct blkcg_gq *blkg;
	/* [한국어] prfill 이 돌려준 값들의 누계. show_total 일 때만 출력된다. */
	u64 total = 0;

	/* [한국어] blkg_list 는 RCU 로 순회한다. 순회 중 blkg 가 destroy 되어도
	 * hlist_del_init_rcu 덕분에 커서가 무효화되지 않고, blkg 메모리도
	 * grace period 전에는 해제되지 않는다. */
	rcu_read_lock();
	/* [한국어] 이 cgroup 에 달린 모든 blkg — 즉 (이 cgroup) × (모든 장치) 조합. */
	hlist_for_each_entry_rcu(blkg, &blkcg->blkg_list, blkcg_node) {
		/* [한국어] 정책 데이터를 읽는 동안 그 장치의 상태가 바뀌지 않도록
		 * 해당 blkg 의 큐 락을 잡는다. blkg 마다 큐가 다르므로 매 반복마다
		 * 잡고 푼다. 이 때문에 prfill 콜백 안에서는 잠들 수 없다. */
		spin_lock_irq(&blkg->q->queue_lock);
		/* [한국어] 이 장치에서 해당 정책이 켜져 있어야 pd[pol->plid] 가 유효하다.
		 * 같은 cgroup 이라도 장치마다 활성 정책이 다를 수 있다. */
		if (blkcg_policy_enabled(blkg->q, pol))
			/* [한국어] 정책이 준 콜백에 자기 pd 를 넘겨 한 줄을 출력하게 하고,
			 * 반환값을 합계에 더한다. plid 가 pd[] 슬롯 번호라는 규약이
			 * 여기서 그대로 쓰인다. */
			total += prfill(sf, blkg->pd[pol->plid], data);
		spin_unlock_irq(&blkg->q->queue_lock);
	}
	rcu_read_unlock();

	/* [한국어] 호출자가 요청했으면 마지막 줄에 합계를 덧붙인다. */
	if (show_total)
		seq_printf(sf, "Total %llu\n", (unsigned long long)total);
}
EXPORT_SYMBOL_GPL(blkcg_print_blkgs);

/**
 * __blkg_prfill_u64 - prfill helper for a single u64 value
 * @sf: seq_file to print to
 * @pd: policy private data of interest
 * @v: value to print
 *
 * Print @v to @sf for the device associated with @pd.
 */
/*
 * [한국어]
 * __blkg_prfill_u64 - "장치이름 값" 한 줄을 seq_file에 출력하는 공용 prfill
 *
 * @sf: 출력 대상 seq_file(cgroupfs 파일 읽기 버퍼)
 * @pd: 출력 중인 blkg의 정책 데이터. 여기서 장치 이름을 얻는다.
 * @v:  출력할 값
 * @return: 출력한 값 v. 장치 이름이 없어 출력을 건너뛰었으면 0.
 *
 * blkcg_print_blkgs()가 blkg마다 호출하는 콜백의 가장 단순한 구현이다.
 * 정책들(blk-throttle, blk-iolatency 등)은 자기 값을 꺼내 이 함수에 넘기기만
 * 하면 되므로 출력 형식이 통일된다.
 *
 * 반환값이 "출력한 값"인 이유: blkcg_print_blkgs()가 show_total 옵션일 때
 * 모든 blkg의 반환값을 더해 마지막에 "Total"로 찍는다. 출력하지 않은
 * 경우 0을 반환해야 그 합계가 왜곡되지 않는다.
 *
 * 실행 컨텍스트: cgroupfs read. 호출자가 큐 락을 쥔 상태다.
 *
 * 호출 체인:
 *   blkcg_print_blkgs → (정책의 prfill 콜백) → [__blkg_prfill_u64]
 *     → blkg_dev_name → seq_printf
 */
u64 __blkg_prfill_u64(struct seq_file *sf, struct blkg_policy_data *pd, u64 v)
{
	/* [한국어] pd->blkg 로 blkg 를 되찾고, 거기서 장치 이름을 얻는다.
	 * pd 만 받는 콜백이 blkg 로 돌아갈 수 있는 것은 blkg_alloc 이 걸어 둔
	 * pd->blkg 백포인터 덕분이다. */
	const char *dname = blkg_dev_name(pd->blkg);

	/* [한국어] 아직 gendisk 가 붙지 않은 큐라면 출력할 이름이 없다.
	 * 0 을 반환해 합계(total)에 영향을 주지 않으면서 이 줄을 건너뛴다. */
	if (!dname)
		return 0;

	/* [한국어] cgroupfs 관례 형식 "<장치이름> <값>" 한 줄을 찍는다. */
	seq_printf(sf, "%s %llu\n", dname, (unsigned long long)v);
	/* [한국어] 출력한 값을 그대로 반환 — 호출자가 Total 합산에 쓴다. */
	return v;
}
EXPORT_SYMBOL_GPL(__blkg_prfill_u64);

/**
 * blkg_conf_init - initialize a blkg_conf_ctx
 * @ctx: blkg_conf_ctx to initialize
 * @input: input string
 *
 * Initialize @ctx which can be used to parse blkg config input string @input.
 * Once initialized, @ctx can be used with blkg_conf_open_bdev() and
 * blkg_conf_prep(), and must be cleaned up with blkg_conf_exit().
 */
/*
 * [한국어]
 * blkg_conf_init - cgroup 설정 문자열 파싱 컨텍스트를 초기화
 *
 * @ctx:   초기화할 컨텍스트(호출자 스택에 있는 경우가 많다)
 * @input: 사용자가 cgroupfs에 쓴 문자열. 예: "259:0 rbps=1048576 wbps=max"
 * @return: 없음
 *
 * === 왜 3단계(init → open_bdev → prep) 구조인가 ===
 * cgroup 설정 쓰기는 여러 자원을 순서대로 잡아야 한다: 문자열 파싱 →
 * 대상 블록 장치 열기 → 큐 락 획득 → blkg 찾기/생성. 이 과정에서 실패할
 * 수 있는 지점이 여러 곳이라, 어디서 실패하든 이미 잡은 것만 정확히
 * 되돌려야 한다.
 * ctx에 진행 상태를 모아 두고 blkg_conf_exit()이 "채워진 것만" 정리하는
 * 구조로 만들면, 각 단계가 실패 처리를 중복 구현하지 않아도 된다.
 *
 * 이 함수는 그 시작점으로, 구조체를 통째로 0으로 만들고 입력 문자열만
 * 심는다. 지정 초기화자(designated initializer)로 대입하면 나머지 필드가
 * 자동으로 0/NULL이 되어, 나중에 exit이 "NULL이면 건너뛴다" 규칙으로
 * 안전하게 정리할 수 있다.
 *
 * 실행 컨텍스트: cgroupfs write(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   정책의 write 핸들러(tg_set_limit, iolatency_set_limit 등)
 *     → [blkg_conf_init] → blkg_conf_open_bdev → blkg_conf_prep
 *     → ... → blkg_conf_exit
 */
void blkg_conf_init(struct blkg_conf_ctx *ctx, char *input)
{
	/* [한국어] 구조체 통째 대입. 지정 초기화자에서 언급하지 않은 필드
	 * (bdev, body, blkg 등)는 C 규칙에 따라 0/NULL 이 되므로,
	 * blkg_conf_exit() 이 "NULL 이면 건너뛴다" 로 안전하게 정리할 수 있다. */
	*ctx = (struct blkg_conf_ctx){ .input = input };
}
EXPORT_SYMBOL_GPL(blkg_conf_init);

/**
 * blkg_conf_open_bdev - parse and open bdev for per-blkg config update
 * @ctx: blkg_conf_ctx initialized with blkg_conf_init()
 *
 * Parse the device node prefix part, MAJ:MIN, of per-blkg config update from
 * @ctx->input and get and store the matching bdev in @ctx->bdev. @ctx->body is
 * set to point past the device node prefix.
 *
 * This function may be called multiple times on @ctx and the extra calls become
 * NOOPs. blkg_conf_prep() implicitly calls this function. Use this function
 * explicitly if bdev access is needed without resolving the blkcg / policy part
 * of @ctx->input. Returns -errno on error.
 */
/*
 * [한국어]
 * blkg_conf_open_bdev - 설정 문자열 앞머리의 "MAJ:MIN"을 파싱해 블록 장치를 연다
 *
 * @ctx: blkg_conf_init()으로 초기화된 컨텍스트. 성공 시 ctx->bdev가 채워지고
 *       ctx->body가 MAJ:MIN 뒤의 나머지 문자열을 가리킨다.
 * @return: 0 성공, 음수 errno(형식 오류 -EINVAL, 장치 없음 -ENODEV 등)
 *
 * cgroup의 I/O 설정은 항상 "어느 장치에 대한 설정인가"로 시작한다.
 * 예: "8:0 rbps=1048576"에서 앞의 "8:0"이 대상 블록 장치의 major:minor다.
 * 장치 이름이 아니라 번호를 쓰는 이유는 이름이 부팅마다 바뀔 수 있는 반면
 * major:minor는 커널 내부에서 장치를 유일하게 식별하는 값이기 때문이다.
 *
 * 위 영문 주석대로 이 함수는 여러 번 호출해도 안전하다(두 번째부터는 no-op).
 * blkg_conf_prep()이 내부적으로 이 함수를 부르므로, 정책이 blkg 조회 없이
 * 장치만 필요한 경우에만 따로 호출하면 된다.
 *
 * 실행 컨텍스트: cgroupfs write(프로세스 컨텍스트). 장치 열기가 잠들 수 있다.
 *
 * 에러 경로: ctx->bdev는 실패 시 NULL로 남으므로 blkg_conf_exit()이
 * 안전하게 건너뛴다.
 *
 * 호출 체인:
 *   정책의 write 핸들러 또는 blkg_conf_prep → [blkg_conf_open_bdev]
 *     → blkdev_get_by_dev
 */
int blkg_conf_open_bdev(struct blkg_conf_ctx *ctx)
{
	/* [한국어] 파싱 커서. 원본 ctx->input 은 건드리지 않고 지역 포인터를 옮긴다. */
	char *input = ctx->input;
	/* [한국어] 파싱해 낼 장치 번호. dev_t 로 조합해 장치를 찾는 데 쓴다. */
	unsigned int major, minor;
	/* [한국어] 찾아낸 블록 장치. 성공 시 ctx->bdev 에 보관되고
	 * blkg_conf_exit() 에서 참조가 반납된다. */
	struct block_device *bdev;
	/* [한국어] sscanf 의 %n 이 채워 줄 "여기까지 소비한 문자 수". */
	int key_len;

	/* [한국어] 이미 열려 있으면 아무 것도 하지 않는다. 위 영문 주석의
	 * "여러 번 불러도 no-op" 계약을 구현하는 부분이며, blkg_conf_prep() 이
	 * 무조건 이 함수를 부를 수 있게 해 준다. */
	if (ctx->bdev)
		return 0;

	/* [한국어] 입력 앞머리의 "MAJ:MIN"을 파싱한다. %n은 "여기까지 몇 글자를
	 * 읽었는가"를 key_len에 기록하는 지시자로, 파싱 후 나머지 문자열의
	 * 시작점을 알아내는 데 쓴다. 반환값 2는 major와 minor 두 개를 모두
	 * 성공적으로 읽었다는 뜻이다(%n은 변환 개수에 세지 않는다). */
	if (sscanf(input, "%u:%u%n", &major, &minor, &key_len) != 2)
		return -EINVAL;

	/* [한국어] 읽은 만큼 포인터를 전진시킨다. */
	input += key_len;
	/* [한국어] 형식 검증 — MAJ:MIN 뒤에는 반드시 공백이 와야 한다.
	 * 이 검사가 없으면 "259:01048576" 같은 입력에서 minor를 01048576으로
	 * 잘못 읽고도 통과해 엉뚱한 장치에 설정이 걸린다. */
	if (!isspace(*input))
		return -EINVAL;
	/* [한국어] 공백을 건너뛰어 정책별 값 부분("rbps=1048576" 등)의 시작을 얻는다. */
	input = skip_spaces(input);

	/* [한국어] major:minor를 dev_t로 조합해 block_device를 얻는다.
	 * _no_open 변형은 "장치를 실제로 여는" 절차(파티션 스캔, holder 등록,
	 * fops->open 호출)를 건너뛰고 참조만 얻는다. 설정을 바꾸려는 것뿐이라
	 * 전체 open 절차가 불필요하고, 그 절차가 잠들거나 다른 락을 잡으면
	 * 여기서 원하지 않는 부작용이 생기기 때문이다. */
	bdev = blkdev_get_no_open(MKDEV(major, minor), false);
	/* [한국어] 그런 번호의 블록 장치가 없다 — 사용자 입력 오류. */
	if (!bdev)
		return -ENODEV;
	/* [한국어] 파티션에는 cgroup I/O 설정을 걸 수 없다.
	 * cgroup 제한은 request_queue 단위로 동작하는데, 파티션들은 디스크 하나의
	 * 큐를 공유하므로 "이 파티션만 100MB/s"라는 제한이 성립하지 않는다.
	 * 사용자가 파티션을 지정하면 조용히 디스크 전체에 적용하는 대신
	 * 명시적으로 거부해, 의도와 다른 결과를 막는다. */
	if (bdev_is_partition(bdev)) {
		/* [한국어] 방금 얻은 참조를 되돌리고 거절한다. */
		blkdev_put_no_open(bdev);
		return -ENODEV;
	}

	/* [한국어] rq-qos 정책 목록을 보호하는 뮤텍스. 설정 적용 도중 다른
	 * 스레드가 정책을 붙이거나 떼면 자료구조가 꼬이므로 직렬화한다.
	 * 이 락은 blkg_conf_exit()이 해제한다 — 이 함수가 락을 쥔 채로
	 * 반환하는 계약이다. */
	mutex_lock(&bdev->bd_queue->rq_qos_mutex);
	/* [한국어] 디스크가 아직 살아 있는지(제거 중이 아닌지) 확인한다.
	 * 사라지는 장치에 설정을 걸어 봐야 곧 함께 사라진다.
	 * 락을 잡은 뒤에 확인하는 순서가 중요하다 — 락 밖에서 확인하면
	 * 확인과 사용 사이에 상태가 바뀔 수 있다. */
	if (!disk_live(bdev->bd_disk)) {
		/* [한국어] 실패 경로에서는 잡은 순서의 역순으로 모두 되돌린다. */
		blkdev_put_no_open(bdev);
		mutex_unlock(&bdev->bd_queue->rq_qos_mutex);
		return -ENODEV;
	}

	/* [한국어] 파싱 결과를 컨텍스트에 기록한다. body는 정책이 이어서 파싱할
	 * 값 부분이고, bdev는 대상 장치다. 이 둘이 채워졌다는 사실 자체가
	 * blkg_conf_exit()에게 "여기까지 진행됐으니 이만큼 정리하라"는 신호가 된다. */
	/* [한국어] 정책이 이어서 파싱할 값 부분의 시작 위치. */
	ctx->body = input;
	/* [한국어] 대상 장치와 그 참조의 소유권을 ctx 로 넘긴다. */
	ctx->bdev = bdev;
	/* [한국어] 성공. 주의: rq_qos_mutex 를 쥔 채로 반환한다(위 설명 참조). */
	return 0;
}
/*
 * Similar to blkg_conf_open_bdev, but additionally freezes the queue,
 * ensures the correct locking order between freeze queue and q->rq_qos_mutex.
 *
 * This function returns negative error on failure. On success it returns
 * memflags which must be saved and later passed to blkg_conf_exit_frozen
 * for restoring the memalloc scope.
 */
/*
 * [한국어]
 * blkg_conf_open_bdev_frozen - 장치를 열고 큐를 freeze한 상태로 만든다
 *
 * @ctx: blkg_conf_init()으로 초기화된 컨텍스트
 * @return: 성공 시 memflags(나중에 blkg_conf_exit_frozen()에 그대로 넘겨야 함),
 *          실패 시 음수 errno. __must_check이므로 반환값을 무시하면 컴파일 경고.
 *
 * 일부 정책 설정은 진행 중인 I/O가 없는 상태에서만 안전하게 바꿀 수 있다.
 * 예를 들어 rq-qos 정책을 큐에 붙이거나 떼는 작업은, 그 정책을 참조하는
 * request가 살아 있으면 해제된 자료구조를 건드리게 된다.
 *
 * === 락 순서가 이 함수의 핵심 ===
 * 큐 freeze는 진행 중인 I/O의 완료를 기다리는데, 그 I/O가 rq_qos_mutex를
 * 필요로 할 수 있다. 따라서 rq_qos_mutex를 쥔 채 freeze하면 데드락이다.
 * 그래서 이 함수는 "락 해제 → freeze → 락 재획득" 순서를 밟는다.
 *
 * memflags를 반환하는 이유: freeze 구간에서는 메모리 할당이 I/O를 유발하면
 * 안 되므로 PF_MEMALLOC_NOIO가 설정되는데, 그 이전 상태를 복원하려면
 * 저장해 두어야 한다. 반환값을 잃으면 태스크의 메모리 할당 컨텍스트가
 * 영구히 잘못된 상태로 남는다 — __must_check이 붙은 이유다.
 *
 * 실행 컨텍스트: cgroupfs write(프로세스 컨텍스트). freeze가 잠든다.
 *
 * 호출 체인:
 *   rq-qos 계열 정책의 write 핸들러 → [blkg_conf_open_bdev_frozen]
 *     → blkg_conf_open_bdev → blk_mq_freeze_queue
 *   해제: blkg_conf_exit_frozen(ctx, memflags)
 */
unsigned long __must_check blkg_conf_open_bdev_frozen(struct blkg_conf_ctx *ctx)
{
	/* [한국어] blkg_conf_open_bdev() 의 반환 코드. */
	int ret;
	/* [한국어] blk_mq_freeze_queue() 가 돌려주는, 복원해야 할 메모리 할당 범위 상태. */
	unsigned long memflags;

	/* [한국어] 이 변형은 반드시 "아직 열리지 않은" ctx 로 시작해야 한다.
	 * 이미 열려 있으면 아래의 unlock/freeze/lock 순서가 성립하지 않으므로 거절. */
	if (ctx->bdev)
		return -EINVAL;

	/* [한국어] 장치 열기 + 파티션/생존 검사. 성공하면 rq_qos_mutex 를 쥔 채 돌아온다. */
	ret = blkg_conf_open_bdev(ctx);
	if (ret < 0)
		/* [한국어] 실패 시 open_bdev 가 이미 모두 되돌렸으므로 그대로 전달. */
		return ret;
	/*
	 * At this point, we haven’t started protecting anything related to QoS,
	 * so we release q->rq_qos_mutex here, which was first acquired in blkg_
	 * conf_open_bdev. Later, we re-acquire q->rq_qos_mutex after freezing
	 * the queue to maintain the correct locking order.
	 */
	/* [한국어] freeze 전에 rq_qos_mutex 를 반드시 놓는다. freeze 는 진행 중인
	 * request 가 모두 끝나기를 기다리는데, 그 request 처리 경로가 이 뮤텍스를
	 * 필요로 할 수 있어 쥔 채로 freeze 하면 데드락이다(위 영문 주석). */
	mutex_unlock(&ctx->bdev->bd_queue->rq_qos_mutex);

	/* [한국어] 큐를 freeze 한다: 새 request 진입을 막고 이미 들어온 것들이
	 * 완료될 때까지 기다린다. 반환된 memflags 는 freeze 구간 동안 설정된
	 * memalloc 범위(회수 중 I/O 재진입 방지)를 되돌리는 데 필요하다. */
	memflags = blk_mq_freeze_queue(ctx->bdev->bd_queue);
	/* [한국어] freeze 가 끝난 뒤 올바른 순서(freeze → rq_qos_mutex)로 다시 잡는다.
	 * 이 뮤텍스는 blkg_conf_exit_frozen() 이 푼다. */
	mutex_lock(&ctx->bdev->bd_queue->rq_qos_mutex);

	/* [한국어] 성공. 반환값은 오류 코드가 아니라 memflags 이며, 호출자는 이것을
	 * blkg_conf_exit_frozen() 에 그대로 넘겨야 한다. */
	return memflags;
}

/**
 * blkg_conf_prep - parse and prepare for per-blkg config update
 * @blkcg: target block cgroup
 * @pol: target policy
 * @ctx: blkg_conf_ctx initialized with blkg_conf_init()
 *
 * Parse per-blkg config update from @ctx->input and initialize @ctx
 * accordingly. On success, @ctx->body points to the part of @ctx->input
 * following MAJ:MIN, @ctx->bdev points to the target block device and
 * @ctx->blkg to the blkg being configured.
 *
 * blkg_conf_open_bdev() may be called on @ctx beforehand. On success, this
 * function returns with queue lock held and must be followed by
 * blkg_conf_exit().
 */
/*
 * [한국어] (위 영문 kernel-doc 참고)
 * blkg_conf_prep - 설정 문자열을 끝까지 해석해 대상 blkg를 확보하고 락을 잡는다
 *
 * @blkcg: 설정을 적용할 cgroup
 * @pol:   대상 정책(blk-throttle, blk-iolatency 등)
 * @ctx:   blkg_conf_init()으로 초기화된 컨텍스트.
 *         성공 시 ctx->bdev(장치), ctx->blkg(대상 blkg), ctx->body(값 부분
 *         문자열)가 모두 채워진다.
 * @return: 0 성공, 음수 errno
 *
 * 3단계 설정 흐름(init → open_bdev → prep)의 마지막 단계다. 하는 일:
 *   1) 아직 장치를 열지 않았으면 blkg_conf_open_bdev()로 연다.
 *   2) 큐 락을 잡는다(이후 blkg 트리를 안전하게 조회하기 위해).
 *   3) (cgroup, 장치) 조합의 blkg를 찾고, 없으면 새로 만든다.
 *      blkg는 "이 cgroup이 이 장치에 대해 갖는 상태"이므로, 사용자가
 *      처음 설정하는 조합이면 이 시점에 생성된다.
 *
 * 락을 잡은 채로 반환하는 것이 이 함수의 계약이다(__acquires 주석). 호출자는
 * 설정을 적용한 뒤 반드시 blkg_conf_exit()으로 락을 풀어야 한다. 이렇게
 * 설계한 이유는 "blkg를 찾은 시점부터 설정을 적용할 때까지" 그 blkg가
 * 사라지지 않아야 하기 때문이다.
 *
 * 실행 컨텍스트: cgroupfs write(프로세스 컨텍스트). blkg 생성이 잠들 수
 * 있으므로 락을 잡기 전에 미리 할당(preload)하는 패턴을 쓴다.
 *
 * 호출 체인:
 *   정책의 write 핸들러 → [blkg_conf_prep]
 *     → blkg_conf_open_bdev → blkg_lookup_check / blkg_create
 */
int blkg_conf_prep(struct blkcg *blkcg, const struct blkcg_policy *pol,
		   struct blkg_conf_ctx *ctx)
	__acquires(&bdev->bd_queue->queue_lock)
{
	/* [한국어] 대상 장치의 gendisk. blkg_alloc/blkg_create 가 disk 를 요구한다. */
	struct gendisk *disk;
	/* [한국어] 그 디스크의 request_queue. blkg 가 실제로 매달리는 대상. */
	struct request_queue *q;
	/* [한국어] 설정을 적용할 blkg — 이 함수의 최종 산출물. */
	struct blkcg_gq *blkg;
	/* [한국어] 오류 코드 임시 저장. */
	int ret;

	/* [한국어] 아직 장치를 열지 않았다면 여기서 연다(이미 열렸으면 no-op).
	 * 성공하면 rq_qos_mutex 를 쥔 상태가 된다. */
	ret = blkg_conf_open_bdev(ctx);
	if (ret)
		return ret;

	/* [한국어] 열린 bdev 에서 디스크와 큐를 꺼낸다. */
	disk = ctx->bdev->bd_disk;
	q = disk->queue;

	/* Prevent concurrent with blkcg_deactivate_policy() */
	/* [한국어] 정책 비활성화와의 경쟁을 막는다. 이 락이 없으면 blkg 를 만든 직후
	 * 다른 스레드가 정책을 꺼서 pd 를 해제해 버릴 수 있다. */
	mutex_lock(&q->blkcg_mutex);
	/* [한국어] blkg 조회/생성은 queue_lock 을 요구한다. */
	spin_lock_irq(&q->queue_lock);

	/* [한국어] 이 장치에서 해당 정책이 켜져 있지 않으면 설정할 대상이 없다.
	 * -EOPNOTSUPP 는 "이 장치는 이 기능을 지원하지 않는다" 는 뜻으로
	 * 사용자에게 전달된다. */
	if (!blkcg_policy_enabled(q, pol)) {
		ret = -EOPNOTSUPP;
		goto fail_unlock;
	}

	/* [한국어] 목표 (cgroup, 큐) 조합의 blkg 가 이미 있으면 그대로 쓴다.
	 * 대부분의 재설정은 이 빠른 경로로 끝난다. */
	blkg = blkg_lookup(blkcg, q);
	if (blkg)
		goto success;

	/*
	 * Create blkgs walking down from blkcg_root to @blkcg, so that all
	 * non-root blkgs have access to their parents.
	 */
	/* [한국어] ★ 왜 루프인가: 부모 blkg가 먼저 존재해야 한다 ★
	 * blkg는 (cgroup × 디스크) 조합마다 하나씩 만들어지는데, 자식 blkg는
	 * 통계 전파와 설정 상속을 위해 부모 blkg를 참조한다. 그런데 사용자가
	 * 중간 cgroup을 건너뛰고 깊은 자식에만 설정을 걸 수 있어, 그 경로의
	 * 부모 blkg들이 아직 없을 수 있다.
	 * 그래서 root 쪽으로 거슬러 올라가 "가장 가까운 없는 조상"부터 하나씩
	 * 만들어 내려온다. 한 번에 하나씩 만드는 이유는 아래에서 보듯 할당을
	 * 위해 락을 놓았다 잡아야 하기 때문이다. */
	while (true) {
		/* [한국어] 이번 반복에서 만들 대상. 아래 탐색으로 조상 쪽으로 밀린다. */
		struct blkcg *pos = blkcg;
		/* [한국어] 조상 탐색 커서. NULL 이 되면 루트에 닿았다는 뜻. */
		struct blkcg *parent;
		/* [한국어] 락 밖에서 미리 할당해 둘 blkg. 락 안에서는 잠들 수 없으므로
		 * 이렇게 "미리 만들어 두고 락 안에서 등록만" 하는 패턴을 쓴다. */
		struct blkcg_gq *new_blkg;

		/* [한국어] 목표 cgroup 의 부모부터 위로 훑기 시작한다. */
		parent = blkcg_parent(blkcg);
		/* [한국어] blkg가 없는 가장 가까운 조상을 찾아 pos를 그쪽으로 옮긴다.
		 * 루프가 끝나면 pos는 "지금 만들어야 할 가장 위쪽 blkg"가 된다.
		 * parent가 NULL이면 root에 도달한 것이고, root blkg는 큐 생성 시
		 * 이미 만들어져 있으므로 탐색이 거기서 멈춘다. */
		while (parent && !blkg_lookup(parent, q)) {
			/* [한국어] 이 조상도 blkg 가 없다 — 생성 지점을 한 칸 위로 올린다. */
			pos = parent;
			/* [한국어] 다시 그 위를 본다. */
			parent = blkcg_parent(parent);
		}

		/* Drop locks to do new blkg allocation with GFP_KERNEL. */
		/* [한국어] 스핀락을 쥔 채로는 잠들 수 있는 할당을 할 수 없으므로
		 * 일시적으로 놓는다. 이 틈에 다른 스레드가 같은 blkg를 만들 수
		 * 있는데, 아래에서 다시 조회해 그 경쟁을 처리한다. */
		spin_unlock_irq(&q->queue_lock);

		/* [한국어] GFP_NOIO로 할당한다. GFP_KERNEL이 아닌 이유: 이 할당이
		 * 메모리 회수를 유발하고 그 회수가 이 디스크로의 write-back을
		 * 필요로 하면, 그 I/O가 다시 blkg를 찾으려다 교착에 빠질 수 있다. */
		new_blkg = blkg_alloc(pos, disk, GFP_NOIO);
		/* [한국어] 할당 실패. 락은 이미 놓은 상태이므로 fail_exit 로 간다
		 * (fail_unlock 으로 가면 잡지 않은 스핀락을 풀게 된다). */
		if (unlikely(!new_blkg)) {
			ret = -ENOMEM;
			goto fail_exit;
		}

		/* [한국어] blkg는 radix tree에 등록되는데, 그 삽입이 내부적으로
		 * 노드를 할당할 수 있다. 그런데 삽입은 스핀락 안에서 해야 하므로
		 * 그때는 할당이 불가능하다.
		 * radix_tree_preload()는 미리 per-CPU 캐시에 노드를 채워 두어,
		 * 락 안의 삽입이 할당 없이 성공하도록 보장한다. 이후
		 * radix_tree_preload_end()까지 preemption이 비활성화된다. */
		if (radix_tree_preload(GFP_KERNEL)) {
			/* [한국어] preload 실패 — 방금 만든 blkg 를 되돌리고 나간다. */
			blkg_free(new_blkg);
			ret = -ENOMEM;
			goto fail_exit;
		}

		/* [한국어] 등록을 위해 다시 락을 잡는다. 이 아래는 모두 "락을 놓은 사이
		 * 상황이 바뀌었을 수 있다" 를 전제로 재확인하는 코드다. */
		spin_lock_irq(&q->queue_lock);

		/* [한국어] 락을 놓은 사이에 상황이 변했을 수 있다. 정책이 이 큐에서
		 * 비활성화되었다면(다른 스레드가 blkcg_deactivate_policy 실행)
		 * 더 진행할 이유가 없다. 락을 놓았다 잡는 코드에서 이런 재확인은
		 * 선택이 아니라 필수다. */
		if (!blkcg_policy_enabled(q, pol)) {
			/* [한국어] 준비했던 blkg 를 버리고, preload 상태까지 정리하는
			 * fail_preloaded 경로로 간다. */
			blkg_free(new_blkg);
			ret = -EOPNOTSUPP;
			goto fail_preloaded;
		}

		/* [한국어] 같은 이유로 blkg도 다시 조회한다. 락을 놓은 틈에 다른
		 * 스레드가 같은 (cgroup, 디스크) 조합을 이미 만들었을 수 있다. */
		blkg = blkg_lookup(pos, q);
		if (blkg) {
			/* [한국어] 경쟁에서 졌다 — 상대가 만든 것을 쓰고 내가 준비한
			 * 것은 버린다. 오류가 아니라 정상적인 결과다. */
			blkg_free(new_blkg);
		} else {
			/* [한국어] 내가 만든다. blkg_create()가 radix tree와 blkcg의
			 * 리스트에 등록하고, 부모 blkg와의 연결도 맺는다.
			 * new_blkg 는 성공/실패와 무관하게 blkg_create 가 소비하므로
			 * 아래 실패 경로에서 다시 free 하지 않는다. */
			blkg = blkg_create(pos, disk, new_blkg);
			/* [한국어] 큐가 죽었거나 부모가 없는 등의 이유로 실패. */
			if (IS_ERR(blkg)) {
				ret = PTR_ERR(blkg);
				goto fail_preloaded;
			}
		}

		/* [한국어] preload 구간을 닫아 preemption을 다시 허용한다.
		 * 이 호출을 빠뜨리면 preemption이 영구히 비활성화되어 시스템이 멈춘다. */
		radix_tree_preload_end();

		/* [한국어] 목표 cgroup까지 도달했으면 완료. 아니면 다음 반복에서
		 * 그다음 자손을 만든다. 매 반복마다 조상이 하나씩 채워지므로
		 * 반드시 유한 횟수 안에 끝난다. */
		if (pos == blkcg)
			goto success;
	}
success:
	/* [한국어] blkg 확보 완료. 정책 비활성화 방지용 뮤텍스는 여기서 놓는다
	 * (queue_lock 은 계약대로 계속 쥔 채 반환한다). */
	mutex_unlock(&q->blkcg_mutex);
	/* [한국어] 호출자가 이어서 설정을 적용할 대상 blkg 를 컨텍스트에 심는다. */
	ctx->blkg = blkg;
	/* [한국어] 성공. queue_lock 은 blkg_conf_exit() 이 푼다. */
	return 0;

fail_preloaded:
	/* [한국어] radix_tree_preload() 가 껐던 preemption 을 반드시 되살린다. */
	radix_tree_preload_end();
fail_unlock:
	/* [한국어] queue_lock 해제. */
	spin_unlock_irq(&q->queue_lock);
fail_exit:
	mutex_unlock(&q->blkcg_mutex);
	/* [한국어] blkcg_mutex 해제 */
	/*
	 * If queue was bypassing, we should retry.  Do so after a
	 * short msleep().  It isn't strictly necessary but queue
	 * can be bypassing for some time and it's always nice to
	 * avoid busy looping.
	 */
	if (ret == -EBUSY) {
	/* [한국어] queue bypass 중이면 잠시 대기 후 재시도; 큐가 bypass 를 벗어나기를 기다린다 */
		msleep(10);
	/* [한국어] 시스템 콜 재시작 */
		ret = restart_syscall();
	}
	return ret;
}
EXPORT_SYMBOL_GPL(blkg_conf_prep);

/**
 * blkg_conf_exit - clean up per-blkg config update
 * @ctx: blkg_conf_ctx initialized with blkg_conf_init()
 *
 * Clean up after per-blkg config update. This function must be called on all
 * blkg_conf_ctx's initialized with blkg_conf_init().
 */
/*
 * [한국어]
 * blkg_conf_exit - 설정 파싱 과정에서 잡은 자원을 역순으로 모두 반납
 *
 * @ctx: 정리할 컨텍스트
 * @return: 없음
 *
 * blkg_conf_init/open_bdev/prep이 단계적으로 잡은 것들(큐 락, rq_qos_mutex,
 * blkg 참조, block_device 참조)을 반대 순서로 풀어 준다.
 *
 * 이 함수의 설계상 중요한 성질은 "부분적으로 진행된 상태에서도 안전하다"는
 * 것이다. blkg_conf_init()이 구조체를 전부 0으로 만들어 두었기 때문에, 각
 * 필드가 NULL인지 확인하는 것만으로 "그 단계까지 갔는지"를 알 수 있다.
 * 덕분에 어느 단계에서 실패하든 호출자는 이 함수 하나만 부르면 된다.
 *
 * __releases() 주석은 sparse 정적 분석기에게 "이 함수가 락을 해제한 채로
 * 반환한다"고 알리는 표시다. 이것이 없으면 sparse가 락 불균형으로 오인해
 * 경고를 낸다.
 *
 * 실행 컨텍스트: cgroupfs write의 마무리(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   정책의 write 핸들러(성공/실패 무관) → [blkg_conf_exit]
 *     → spin_unlock_irq(queue_lock) → mutex_unlock(rq_qos_mutex)
 *     → blkg_put → blkdev_put
 */
void blkg_conf_exit(struct blkg_conf_ctx *ctx)
	__releases(&ctx->bdev->bd_queue->queue_lock)
	__releases(&ctx->bdev->bd_queue->rq_qos_mutex)
{
	/* [한국어] ctx->blkg 가 채워졌다는 것은 blkg_conf_prep() 이 성공해
	 * queue_lock 을 쥔 채 반환했다는 뜻이다. 그러니 그 락을 여기서 푼다. */
	if (ctx->blkg) {
		/* [한국어] prep 이 잡은 그 큐의 락. bdev_get_queue 로 같은 큐를 다시 얻는다. */
		spin_unlock_irq(&bdev_get_queue(ctx->bdev)->queue_lock);
		/* [한국어] 이중 해제를 막기 위해 즉시 표식을 지운다. */
		ctx->blkg = NULL;
	}

	/* [한국어] ctx->bdev 가 채워졌다는 것은 blkg_conf_open_bdev() 가 성공해
	 * rq_qos_mutex 를 쥐고 bdev 참조를 들고 있다는 뜻이다. */
	if (ctx->bdev) {
		/* [한국어] open_bdev 가 잡은 채 반환했던 뮤텍스를 여기서 푼다. */
		mutex_unlock(&ctx->bdev->bd_queue->rq_qos_mutex);
		/* [한국어] blkdev_get_no_open 으로 얻은 참조를 짝 맞춰 반납한다. */
		blkdev_put_no_open(ctx->bdev);
		/* [한국어] body 는 input 문자열 내부를 가리키던 포인터라 해제 대상은
		 * 아니지만, 유효하지 않은 상태를 남기지 않도록 함께 지운다. */
		ctx->body = NULL;
		/* [한국어] 이중 해제 방지 표식. */
		ctx->bdev = NULL;
	}
}
EXPORT_SYMBOL_GPL(blkg_conf_exit);

/*
 * Similar to blkg_conf_exit, but also unfreezes the queue. Should be used
 * when blkg_conf_open_bdev_frozen is used to open the bdev.
 */
/*
 * [한국어]
 * blkg_conf_exit_frozen - freeze된 상태로 시작한 설정 작업의 뒷정리
 *
 * @ctx:      정리할 컨텍스트
 * @memflags: blkg_conf_open_bdev_frozen()이 반환했던 값. 반드시 그대로 전달해야 한다.
 * @return: 없음
 *
 * blkg_conf_exit()의 freeze 버전이다. 일반 정리에 더해 큐 unfreeze와
 * 메모리 할당 컨텍스트 복원을 수행한다.
 *
 * 순서가 중요하다: unfreeze를 먼저 해야 그 뒤에 오는 자원 해제 과정에서
 * 메모리 할당이 필요해져도 I/O가 막혀 있지 않다. 반대로 하면 해제 도중
 * 자기가 막아 놓은 큐를 기다리는 데드락이 될 수 있다.
 *
 * 실행 컨텍스트: cgroupfs write의 마무리(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   rq-qos 계열 정책의 write 핸들러 → [blkg_conf_exit_frozen]
 *     → blk_mq_unfreeze_queue(memflags) → blkg_conf_exit
 */
void blkg_conf_exit_frozen(struct blkg_conf_ctx *ctx, unsigned long memflags)
{
	/* [한국어] 장치를 열지 못했다면 freeze 도 없었으므로 할 일이 없다. */
	if (ctx->bdev) {
		/* [한국어] blkg_conf_exit() 이 ctx->bdev 를 NULL 로 만들기 전에
		 * 큐 포인터를 따로 보관해 둔다. 그 뒤에 unfreeze 해야 하기 때문. */
		struct request_queue *q = ctx->bdev->bd_queue;

		/* [한국어] 공통 정리(락 해제, 참조 반납)를 먼저 수행한다. */
		blkg_conf_exit(ctx);
		/* [한국어] 큐 freeze 해제 + memflags 로 메모리 할당 범위 복원.
		 * memflags 는 open_bdev_frozen 이 돌려준 값 그대로여야 한다. */
		blk_mq_unfreeze_queue(q, memflags);
	}
}

/*
 * [한국어]
 * blkg_iostat_add - 통계 구조체를 항목별로 더한다 (dst += src)
 *
 * @dst: 누적 대상
 * @src: 더할 값
 * @return: 없음
 *
 * 두 곳에서 쓰인다:
 *   1) per-CPU 통계를 blkg 전역 통계로 모을 때 — 각 CPU의 증분을 합산
 *   2) 자식 blkg의 통계를 부모로 전파할 때 — cgroup 트리 상향 누적
 * 둘 다 "지금까지의 합계에 새 값을 더한다"는 같은 연산이라 하나의 함수를 공유한다.
 *
 * 실행 컨텍스트: 통계 집계 경로. 호출자가 u64_stats seqlock 또는 적절한
 * 락으로 보호한다.
 *
 * 호출 체인:
 *   __blkcg_rstat_flush → [blkg_iostat_add]
 */
static void blkg_iostat_add(struct blkg_iostat *dst, struct blkg_iostat *src)
{
	/* [한국어] read/write/discard 세 분류 인덱스. */
	int i;

	/* [한국어] 분류별로 바이트와 건수를 각각 더한다. */
	for (i = 0; i < BLKG_IOSTAT_NR; i++) {
		/* [한국어] 누적 바이트 합산. u64 라 사실상 오버플로 걱정이 없다. */
		dst->bytes[i] += src->bytes[i];
		/* [한국어] 누적 IO 건수 합산. */
		dst->ios[i] += src->ios[i];
	}
}

/*
 * [한국어]
 * blkg_iostat_sub - 통계 구조체를 항목별로 뺀다 (dst -= src)
 *
 * @dst: 차감 대상
 * @src: 뺄 값
 * @return: 없음
 *
 * "증분"을 계산하는 데 쓴다. per-CPU 통계는 단조 증가하는 누적값이므로,
 * "지난번 전파 이후 얼마나 늘었는가"를 알려면 현재값(cur)에서 마지막
 * 스냅숏(last)을 빼야 한다:
 *   delta = cur - last;  부모에 delta를 더함;  last = cur;
 * 이 패턴 덕분에 같은 값을 두 번 전파하지 않으면서도 CPU별 카운터를
 * 초기화할 필요가 없다.
 *
 * 결과가 음수가 되면 안 되는데, cur >= last가 항상 성립하기 때문이다
 * (통계는 감소하지 않는다). 다만 blkg_clear_stat()으로 리셋할 때 둘을
 * 함께 0으로 맞추는 이유가 바로 이 불변식을 지키기 위해서다.
 *
 * 실행 컨텍스트: 통계 집계 경로.
 *
 * 호출 체인:
 *   __blkcg_rstat_flush → [blkg_iostat_sub]
 */
static void blkg_iostat_sub(struct blkg_iostat *dst, struct blkg_iostat *src)
{
	/* [한국어] read/write/discard 세 분류 인덱스. */
	int i;

	/* [한국어] 분류별로 바이트와 건수를 각각 뺀다. */
	for (i = 0; i < BLKG_IOSTAT_NR; i++) {
		/* [한국어] cur - last 로 "아직 전파하지 않은 증분" 을 얻는 용도. */
		dst->bytes[i] -= src->bytes[i];
		/* [한국어] 건수도 같은 방식. */
		dst->ios[i] -= src->ios[i];
	}
}

/*
 * [한국어]
 * blkcg_iostat_update - "증분만 더하고 스냅숏을 갱신" 하는 통계 전파의 핵심 연산
 *
 * @blkg: 갱신 대상 blkg. blkg->iostat.cur 에 증분이 더해진다.
 * @cur:  현재 누적값(단조 증가). per-cpu 슬롯의 cur 이거나, 부모로 전파할 때는
 *        자식 blkg 의 iostat.cur 이다.
 * @last: 지난번 전파 시점의 스냅숏. 이 함수가 cur 값으로 갱신한다.
 * @return: 없음.
 *
 * 왜 이런 방식인가: 통계 카운터를 flush 할 때마다 0 으로 초기화하면, 초기화와
 * 갱신 사이에 들어온 IO 를 잃는다. 그래서 카운터는 계속 단조 증가시키고,
 * "지난번에 어디까지 반영했는지" 를 last 에 기억한다.
 *   delta = cur - last;  대상에 delta 를 더함;  last = cur;
 * 이렇게 하면 갱신 측(핫패스)은 cur 을 더하기만 하면 되고 flush 와 경쟁하지 않는다.
 *
 * 실행 컨텍스트: __blkcg_rstat_flush() 안. 호출자가 blkg_stat_lock 을 쥔 상태이며,
 * 여기서는 추가로 대상 blkg 의 u64_stats seqcount 를 잡아 독자와 동기화한다.
 *
 * 호출 체인:
 *   __blkcg_rstat_flush() → [blkcg_iostat_update]
 *     → blkg_iostat_set/sub/add
 */
static void blkcg_iostat_update(struct blkcg_gq *blkg, struct blkg_iostat *cur,
				struct blkg_iostat *last)
{
	/* [한국어] cur - last 결과를 담을 지역 변수. 스택에 두어 다른 CPU 가
	 * 중간 상태를 볼 수 없게 한다. */
	struct blkg_iostat delta;
	/* [한국어] seqcount 진입 시 저장할 인터럽트 상태. */
	unsigned long flags;

	/* propagate percpu delta to global */
	/* [한국어] 목적지 통계의 쓰기 구간 진입. 독자(io.stat 읽기)는 이 구간 동안
	 * seq 값이 바뀌는 것을 보고 재시도한다. */
	flags = u64_stats_update_begin_irqsave(&blkg->iostat.sync);
	/* [한국어] delta = cur (일단 현재값을 통째로 복사). */
	blkg_iostat_set(&delta, cur);
	/* [한국어] delta -= last (이제 delta 는 지난 flush 이후의 증분). */
	blkg_iostat_sub(&delta, last);
	/* [한국어] 목적지 누적값에 증분을 더한다. */
	blkg_iostat_add(&blkg->iostat.cur, &delta);
	/* [한국어] last += delta, 즉 last 를 cur 과 같게 만든다. 다음 flush 의 기준점.
	 * (last = cur 대신 더하기를 쓰는 이유는 위 세 헬퍼만으로 표현하기 위해서다.) */
	blkg_iostat_add(last, &delta);
	/* [한국어] 쓰기 구간 종료 및 인터럽트 복원. */
	u64_stats_update_end_irqrestore(&blkg->iostat.sync, flags);
}

/*
 * [한국어] === 통계 관련 자료구조 정리 (blk-cgroup.h 정의를 이 파일 관점에서 요약) ===
 *
 * struct blkg_iostat
 *   bytes[3], ios[3] 두 배열뿐인 순수 카운터. 인덱스는 BLKG_IOSTAT_READ /
 *   WRITE / DISCARD 이며, 어느 분류인지는 blk_cgroup_io_type() 이 bio->bi_opf 로
 *   결정한다. 값은 단조 증가하며 flush 때 0 으로 되돌리지 않는다.
 *
 * struct blkg_iostat_set
 *   cur(현재 누적) + last(마지막 전파 시점 스냅숏) + sync(u64_stats seqcount)
 *   + blkg(주인 역참조) + lnode/lqueued(lockless list 연결). 두 벌로 쓰인다:
 *     - blkg->iostat_cpu : CPU 마다 하나. IO 제출 시 락 없이 갱신되는 쪽.
 *     - blkg->iostat     : blkg 당 하나. flush 로 합산된 전역 값이며 io.stat 출력원.
 *
 * 통계가 흐르는 길
 *   blk_cgroup_bio_start()  [갱신, 프로세스 컨텍스트, preempt off]
 *     → blkg->iostat_cpu[cpu].cur 증가, 아직 llist 에 없으면 blkcg->lhead 에 등록
 *   → (cgroup rstat 프레임워크가 적당한 때 flush 요청)
 *   → blkcg_rstat_flush() → __blkcg_rstat_flush()
 *     → llist 를 통째로 떼어 각 항목의 (cur - last) 를 blkg->iostat.cur 에 더하고,
 *       같은 증분을 부모 blkg 의 iostat.cur 에도 더한다(계층 누적)
 *   → blkcg_print_stat() 이 blkg->iostat.cur 을 읽어 io.stat 으로 출력
 */

/*
 * [한국어]
 * __blkcg_rstat_flush - 한 CPU 의 lockless list 에 쌓인 통계 갱신을 모두 반영한다
 *
 * @blkcg: flush 대상 cgroup. 이 cgroup 의 lhead[cpu] 를 비운다.
 * @cpu:   flush 할 CPU 번호. rstat 프레임워크가 "이 CPU 에서 갱신이 있었다" 고
 *         알려 준 값이거나, blkg 소멸 시 모든 CPU 를 도는 경우의 인덱스다.
 * @return: 없음.
 *
 * === 왜 lockless list 가 필요한가 ===
 * cgroup rstat 프레임워크는 "어느 (cgroup, CPU) 에서 갱신이 있었나" 까지만
 * 기억한다. 그런데 한 cgroup 에는 장치 수만큼 blkg 가 있으므로, 그것만으로는
 * 어느 blkg 를 봐야 할지 알 수 없어 전부 훑어야 한다. 그래서 갱신 측이
 * "내가 갱신한 iostat_cpu" 를 blkcg->lhead[cpu] 에 매달아 두고, 이 함수는
 * 그 리스트만 처리한다. 장치가 많아도 실제 IO 가 있었던 blkg 만 비용이 든다.
 *
 * === 두 방향의 반영 ===
 * 항목 하나마다 (cur - last) 증분을 구해 두 곳에 더한다.
 *   1) 자기 blkg 의 전역 iostat.cur  — io.stat 에 그대로 나오는 값
 *   2) 부모 blkg 의 iostat.cur       — cgroup 계층 누적(부모는 자식 합을 포함)
 * 부모로의 전파는 여기서 한 단계만 한다. 조부모 이상은 부모 blkg 가 다시
 * flush 될 때 같은 방식으로 올라간다.
 *
 * 실행 컨텍스트: rstat flush 경로(프로세스 컨텍스트) 또는 __blkg_release()
 * (RCU 콜백). 어느 쪽이든 잠들 수 없다. 전역 통계를 만지는 구간은
 * blkg_stat_lock(raw spinlock)으로 직렬화한다.
 *
 * 호출 체인:
 *   cgroup rstat flush → blkcg_rstat_flush() → [__blkcg_rstat_flush]
 *   __blkg_release() → [__blkcg_rstat_flush]
 *     → blkcg_iostat_update(), blkg_put()
 */

static void __blkcg_rstat_flush(struct blkcg *blkcg, int cpu)
{
	/* [한국어] 이 (blkcg, cpu) 조합의 lockless list 헤드. 갱신 측이 여기에
	 * iostat_set 을 매달아 둔다. */
	struct llist_head *lhead = per_cpu_ptr(blkcg->lhead, cpu);
	/* [한국어] 통째로 떼어 낸 리스트의 첫 노드. */
	struct llist_node *lnode;
	/* [한국어] 순회 커서와 다음 노드(safe 순회를 위해 미리 읽어 둔다). */
	struct blkg_iostat_set *bisc, *next_bisc;
	/* [한국어] raw_spin_lock_irqsave 가 저장할 인터럽트 상태. */
	unsigned long flags;

	/* [한국어] 리스트에 매달린 iostat_set 에서 blkg 와 그 부모 포인터를 따라가므로,
	 * 그 객체들이 해제되지 않도록 RCU read-side 로 감싼다. */
	rcu_read_lock();

	/* [한국어] 리스트 전체를 원자적으로 떼어 온다(헤드는 즉시 빈 상태가 된다).
	 * 이후 새로 들어오는 갱신은 빈 리스트에 쌓이므로, 이 함수가 처리하는
	 * 집합과 겹치지 않는다. llist 는 xchg 기반이라 락이 필요 없다. */
	lnode = llist_del_all(lhead);
	/* [한국어] 이 CPU 에서 갱신된 blkg 가 없다 — 할 일 없이 빠져나간다. */
	if (!lnode)
		goto out;

	/*
	 * For covering concurrent parent blkg update from blkg_release().
	 *
	 * When flushing from cgroup, the subsystem rstat lock is always held,
	 * so this lock won't cause contention most of time.
	 */
	/* [한국어] 전역/부모 통계를 만지는 구간을 직렬화한다. 같은 부모 blkg 를
	 * 서로 다른 CPU 가 동시에 갱신할 수 있고, __blkg_release() 도 여기에
	 * 끼어들 수 있기 때문이다(위 영문 주석). raw_ 인 이유는 PREEMPT_RT 에서도
	 * 잠들지 않아야 하기 때문이며, 영문 주석대로 rstat 경로에서는 상위 락이
	 * 이미 직렬화를 해 주므로 실제 경합은 드물다. */
	raw_spin_lock_irqsave(&blkg_stat_lock, flags);

	/*
	 * Iterate only the iostat_cpu's queued in the lockless list.
	 */
	/* [한국어] 떼어 온 리스트만 순회한다 — 이 cgroup 의 모든 blkg 가 아니라,
	 * 실제로 갱신이 있었던 것만. _safe 변형이라 순회 중 노드를 다시 리스트에
	 * 넣어도 커서가 깨지지 않는다. */
	llist_for_each_entry_safe(bisc, next_bisc, lnode, lnode) {
		/* [한국어] 이 통계 슬롯의 주인 blkg (blkg_alloc 이 심어 둔 백포인터). */
		struct blkcg_gq *blkg = bisc->blkg;
		/* [한국어] 증분을 한 단계 위로 전파할 대상. 루트 blkg 면 NULL. */
		struct blkcg_gq *parent = blkg->parent;
		/* [한국어] per-cpu 현재값의 일관된 스냅숏을 담을 지역 변수. */
		struct blkg_iostat cur;
		/* [한국어] u64_stats seqcount 재시도 루프용 시퀀스 값. */
		unsigned int seq;

		/*
		 * Order assignment of `next_bisc` from `bisc->lnode.next` in
		 * llist_for_each_entry_safe and clearing `bisc->lqueued` for
		 * avoiding to assign `next_bisc` with new next pointer added
		 * in blk_cgroup_bio_start() in case of re-ordering.
		 *
		 * The pair barrier is implied in llist_add() in blk_cgroup_bio_start().
		 */
		/* [한국어] 전체 메모리 배리어. 영문 주석이 설명하는 경쟁은 이렇다:
		 * 순회 매크로가 next_bisc 를 bisc->lnode.next 에서 읽는 것과,
		 * 바로 아래 lqueued=false 쓰기의 순서가 뒤바뀌면 위험하다.
		 * lqueued 가 먼저 false 로 보이면 blk_cgroup_bio_start() 가 이 노드를
		 * 리스트에 다시 넣으면서 lnode.next 를 새 값으로 덮어쓰고,
		 * 그러면 우리가 읽는 next_bisc 가 이미 처리한 리스트가 아니라
		 * 새 리스트를 가리키게 된다. 배리어로 "next 읽기 → lqueued 쓰기"
		 * 순서를 강제한다. 짝이 되는 배리어는 llist_add() 안에 들어 있다. */
		smp_mb();

		/* [한국어] "이 노드는 이제 리스트에 없다" 고 표시해, 다음 IO 가 다시
		 * 등록할 수 있게 한다. WRITE_ONCE 로 컴파일러 최적화(쪼개기/삭제)를 막는다. */
		WRITE_ONCE(bisc->lqueued, false);
		/* [한국어] 리스트에는 두 종류가 섞여 있다: per-cpu 슬롯(iostat_cpu)과,
		 * 자식이 부모를 대신 등록해 둔 전역 슬롯(&blkg->iostat).
		 * 후자는 이미 전역값이므로 per-cpu → 전역 합산 단계를 건너뛰고
		 * 곧바로 부모 전파만 한다. */
		if (bisc == &blkg->iostat)
			goto propagate_up; /* propagate up to parent only */

		/* fetch the current per-cpu values */
		/* [한국어] seqcount 재시도 루프. 갱신 측이 쓰는 도중이면 seq 가 홀수/변경되어
		 * 재시도하게 되고, 그 결과 찢기지 않은 스냅숏을 얻는다. */
		do {
			/* [한국어] 읽기 시작 시퀀스 확보. */
			seq = u64_stats_fetch_begin(&bisc->sync);
			/* [한국어] per-cpu 누적값을 지역 변수로 복사. */
			blkg_iostat_set(&cur, &bisc->cur);
		} while (u64_stats_fetch_retry(&bisc->sync, seq));
		/* [한국어] (cur - bisc->last) 증분을 blkg 전역 통계에 더하고 last 를 갱신. */
		blkcg_iostat_update(blkg, &cur, &bisc->last);

propagate_up:
		/* propagate global delta to parent (unless that's root) */
		/* [한국어] parent && parent->parent 조건: 부모가 있고, 그 부모도 루트가
		 * 아니어야 한다. 즉 "루트의 직계 자식" 까지만 올린다.
		 * 루트 cgroup 의 통계는 blkg 가 아니라 디스크 파티션 통계에서
		 * 직접 채우므로(blkcg_fill_root_iostats) 중복 집계를 피하는 것이다. */
		if (parent && parent->parent) {
			/* [한국어] 방금 갱신된 이 blkg 의 전역 누적값에서, 부모로 아직
			 * 올리지 않은 증분만큼을 부모 전역 통계에 더한다.
			 * 여기서 last 는 "부모로 전파한 지점" 을 기억하는 용도로 재사용된다. */
			blkcg_iostat_update(parent, &blkg->iostat.cur,
					    &blkg->iostat.last);
			/*
			 * Queue parent->iostat to its blkcg's lockless
			 * list to propagate up to the grandparent if the
			 * iostat hasn't been queued yet.
			 */
			/* [한국어] 조부모까지 올리려면 부모의 전역 슬롯도 flush 대상이
			 * 되어야 한다. 아직 등록돼 있지 않다면 부모 cgroup 의 같은 CPU
			 * 리스트에 넣어 둔다(위에서 bisc == &blkg->iostat 로 걸러지는 그 경우). */
			if (!parent->iostat.lqueued) {
				/* [한국어] 부모 cgroup 의 lockless list 헤드를 담을 지역 변수. */
				struct llist_head *plhead;

				/* [한국어] 부모의 blkcg 에서 같은 CPU 슬롯을 고른다.
				 * cgroup 이 다르므로 lhead 배열도 다르다. */
				plhead = per_cpu_ptr(parent->blkcg->lhead, cpu);
				/* [한국어] 원자적 리스트 추가(내부에 배리어 포함).
				 * 위 smp_mb() 와 짝이 되는 배리어가 여기 있다. */
				llist_add(&parent->iostat.lnode, plhead);
				/* [한국어] 중복 등록 방지 표시. blkg_stat_lock 아래이므로
				 * 평범한 대입으로 충분하다. */
				parent->iostat.lqueued = true;
			}
		}
	}
	/* [한국어] 전역 통계 갱신 구간 종료. */
	raw_spin_unlock_irqrestore(&blkg_stat_lock, flags);
out:
	/* [한국어] blkg 포인터 추적이 끝났으므로 RCU read-side 종료. */
	rcu_read_unlock();
}

/*
 * [한국어]
 * blkcg_rstat_flush - cgroup rstat 프레임워크가 부르는 flush 콜백
 *
 * @css: flush 대상 cgroup 의 css. blkcg 로 변환해 쓴다.
 * @cpu: 갱신이 있었다고 rstat 이 기록해 둔 CPU 번호.
 * @return: 없음.
 *
 * 왜 필요한가: cgroup 코어는 서브시스템마다 "이 (cgroup, CPU) 의 통계를
 * 정리하라" 는 콜백을 부른다. 이 파일은 그 콜백에서 lockless list 를 비운다.
 * blkcg 구조체의 css.ss->css_rstat_flush 로 등록되며, io.stat 을 읽거나
 * 주기적 flush 가 일어날 때 호출된다.
 *
 * 루트를 건너뛰는 이유: 루트 cgroup 의 io.stat 은 blkg 통계가 아니라
 * 디스크 전체 통계(part_stat)에서 직접 만든다(blkcg_fill_root_iostats).
 * 그래야 cgroup 을 하나도 만들지 않은 시스템에서 blkg 통계 유지 비용이
 * 들지 않는다. 그래서 루트에서는 flush 할 것이 없다.
 *
 * 실행 컨텍스트: rstat flush 경로. 상위에서 서브시스템 rstat 락을 쥔 상태다.
 *
 * 호출 체인:
 *   css_rstat_flush() → [blkcg_rstat_flush] → __blkcg_rstat_flush()
 */
static void blkcg_rstat_flush(struct cgroup_subsys_state *css, int cpu)
{
	/* Root-level stats are sourced from system-wide IO stats */
	/* [한국어] cgroup_parent() 가 NULL 이면 루트다. 루트가 아닐 때만 실제 flush. */
	if (cgroup_parent(css->cgroup))
		__blkcg_rstat_flush(css_to_blkcg(css), cpu);
}

/*
 * We source root cgroup stats from the system-wide stats to avoid
 * tracking the same information twice and incurring overhead when no
 * cgroups are defined. For that reason, css_rstat_flush in
 * blkcg_print_stat does not actually fill out the iostat in the root
 * cgroup's blkcg_gq.
 *
 * However, we would like to re-use the printing code between the root and
 * non-root cgroups to the extent possible. For that reason, we simulate
 * flushing the root cgroup's stats by explicitly filling in the iostat
 * with disk level statistics.
 */
/*
 * [한국어]
 * blkcg_fill_root_iostats - root cgroup 통계를 시스템 전체 disk_stats 로 채움
 *
 * 호출 경로: blkcg_print_stat() -> blkcg_fill_root_iostats()
 * root cgroup 은 모든 장치의 disk_stats 를
 *   집계해 read/write/discard 바이트/IO 수를 시뮬레이션한다. sector 단위를
 *   << 9 로 바이트로 변환한다.
 */

static void blkcg_fill_root_iostats(void)
{
	/* [한국어] block_class 에 등록된 장치를 안전하게 순회하기 위한 반복자.
	 * 순회 중 장치가 제거되지 않도록 내부에서 참조를 잡아 준다. */
	struct class_dev_iter iter;
	/* [한국어] 반복자가 돌려주는 struct device. dev_to_bdev 로 block_device 로 바꾼다. */
	struct device *dev;

	/* [한국어] block_class 의 장치 중 타입이 disk_type 인 것(= 파티션이 아닌 디스크)만
	 * 순회하도록 반복자를 초기화한다. 세 번째 인자 NULL 은 "처음부터" 라는 뜻. */
	class_dev_iter_init(&iter, &block_class, NULL, &disk_type);
	/* [한국어] 시스템의 모든 디스크를 하나씩 훑는다. 루트 통계는 장치 전체 합이므로
	 * 특정 cgroup 이 아니라 장치 목록을 도는 것이 맞다. */
	while ((dev = class_dev_iter_next(&iter))) {
		/* [한국어] device → block_device. 파티션 통계(bd_stats)가 여기에 있다. */
		struct block_device *bdev = dev_to_bdev(dev);
		/* [한국어] 이 디스크 큐의 루트 blkg. 여기에 합산 결과를 써 넣는다. */
		struct blkcg_gq *blkg = bdev->bd_disk->queue->root_blkg;
		/* [한국어] 모든 CPU 의 disk_stats 를 더해 담을 임시 버퍼. */
		struct blkg_iostat tmp;
		/* [한국어] per-cpu disk_stats 순회 인덱스. */
		int cpu;
		/* [한국어] seqcount 진입 시 저장할 인터럽트 상태. */
		unsigned long flags;

		/* [한국어] 누적 전에 0 으로 초기화. 이 함수는 "더하기" 가 아니라
		 * "현재 절대값으로 덮어쓰기" 이므로 매번 새로 계산한다. */
		memset(&tmp, 0, sizeof(tmp));
		/* [한국어] 디스크 통계도 per-cpu 로 흩어져 있어 전부 더해야 한다. */
		for_each_possible_cpu(cpu) {
			/* [한국어] 이 CPU 의 디스크 통계 슬롯. */
			struct disk_stats *cpu_dkstats;

			/* [한국어] bd_stats 는 block_device 의 per-cpu disk_stats 배열이다. */
			cpu_dkstats = per_cpu_ptr(bdev->bd_stats, cpu);
			/* [한국어] 읽기 건수 누적. STAT_READ 는 disk_stats 쪽 인덱스,
			 * BLKG_IOSTAT_READ 는 blkg 쪽 인덱스로 서로 다른 열거형이다. */
			tmp.ios[BLKG_IOSTAT_READ] +=
				cpu_dkstats->ios[STAT_READ];
			/* [한국어] 쓰기 건수 누적. */
			tmp.ios[BLKG_IOSTAT_WRITE] +=
				cpu_dkstats->ios[STAT_WRITE];
			/* [한국어] discard 건수 누적. */
			tmp.ios[BLKG_IOSTAT_DISCARD] +=
				cpu_dkstats->ios[STAT_DISCARD];
			// convert sectors to bytes
			/* [한국어] disk_stats 는 섹터 단위로 센다. blkg 통계는 바이트 단위라
			 * << 9 (즉 ×512)로 변환한다. 512 는 커널이 쓰는 논리 섹터 크기이며
			 * 장치의 실제 블록 크기와는 무관한 고정 환산 단위다. */
			tmp.bytes[BLKG_IOSTAT_READ] +=
				cpu_dkstats->sectors[STAT_READ] << 9;
			/* [한국어] 쓰기 바이트 누적(섹터→바이트 변환). */
			tmp.bytes[BLKG_IOSTAT_WRITE] +=
				cpu_dkstats->sectors[STAT_WRITE] << 9;
			/* [한국어] discard 바이트 누적(섹터→바이트 변환). */
			tmp.bytes[BLKG_IOSTAT_DISCARD] +=
				cpu_dkstats->sectors[STAT_DISCARD] << 9;
		}

		/* [한국어] 루트 blkg 의 전역 통계를 쓰는 구간. 읽는 쪽
		 * (blkcg_print_one_stat)이 찢긴 값을 보지 않도록 seqcount 로 감싼다. */
		flags = u64_stats_update_begin_irqsave(&blkg->iostat.sync);
		/* [한국어] 누적이 아니라 통째로 대입한다 — 이미 절대값을 계산했기 때문. */
		blkg_iostat_set(&blkg->iostat.cur, &tmp);
		u64_stats_update_end_irqrestore(&blkg->iostat.sync, flags);
	}
	/* [한국어] 반복자가 잡고 있던 참조를 반납한다. 빠뜨리면 장치가 해제되지 않는다. */
	class_dev_iter_exit(&iter);
}

/*
 * [한국어]
 * blkcg_print_one_stat - blkg 하나의 io.stat 한 줄을 출력
 *
 * @blkg: 출력할 blkcg_gq
 * @s:    출력 대상 seq_file
 * @return: 없음
 *
 * cgroup v2의 io.stat 파일에서 한 장치에 해당하는 줄을 만든다. 출력 형식:
 *   259:0 rbytes=... wbytes=... rios=... wios=... dbytes=... dios=...
 *
 * 값을 읽을 때 u64_stats_fetch_begin/retry 루프를 쓰는 이유: 통계는
 * per-CPU에서 갱신되는 u64 값이라, 32비트 아키텍처에서는 상위/하위 32비트가
 * 따로 쓰여 그 사이에 읽으면 앞뒤가 섞인 값을 본다. seqlock이 갱신 중임을
 * 감지하면 읽기를 재시도시킨다.
 *
 * 등록된 정책들의 pd_stat_fn 콜백도 함께 호출해, 정책별 추가 통계
 * (blk-iocost의 cost.stat 등)를 같은 줄에 이어 붙인다.
 *
 * 실행 컨텍스트: cgroupfs read(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   cgroupfs read(io.stat) → blkcg_print_stat → [blkcg_print_one_stat]
 *     → u64_stats_fetch_begin/retry → 정책의 pd_stat_fn
 */
static void blkcg_print_one_stat(struct blkcg_gq *blkg, struct seq_file *s)
{
	/* [한국어] 출력 대상은 per-cpu 가 아니라 이미 flush 된 전역 통계 묶음이다. */
	struct blkg_iostat_set *bis = &blkg->iostat;
	/* [한국어] seqcount 루프 안에서 한꺼번에 떠 오는 여섯 값의 스냅숏.
	 * 루프 밖에서 쓰려면 지역 변수에 복사해 두어야 한다. */
	u64 rbytes, wbytes, rios, wios, dbytes, dios;
	/* [한국어] 줄 앞머리에 찍을 장치 이름. */
	const char *dname;
	/* [한국어] u64_stats 재시도 루프의 시퀀스 값. */
	unsigned seq;
	/* [한국어] 정책별 추가 통계를 붙일 때 쓰는 슬롯 인덱스. */
	int i;

	/* [한국어] 소멸 중인 blkg 는 출력하지 않는다 — 곧 사라질 장치 줄을
	 * 사용자에게 보여 줄 이유가 없다. */
	if (!blkg->online)
		return;

	/* [한국어] 장치 이름 조회. */
	dname = blkg_dev_name(blkg);
	/* [한국어] 아직 gendisk 가 붙지 않은 큐라면 줄을 만들 수 없다. */
	if (!dname)
		return;

	/* [한국어] "<장치이름> " 으로 줄을 시작한다. 뒤에 key=value 들이 이어 붙는다. */
	seq_printf(s, "%s ", dname);

	/* [한국어] 여섯 값을 "같은 순간의 값" 으로 읽기 위한 재시도 루프.
	 * 도중에 갱신이 끼어들면 seq 가 달라져 통째로 다시 읽는다. */
	do {
		/* [한국어] 읽기 시작 시퀀스 확보. */
		seq = u64_stats_fetch_begin(&bis->sync);

		/* [한국어] 읽기 누적 바이트. */
		rbytes = bis->cur.bytes[BLKG_IOSTAT_READ];
		/* [한국어] 쓰기 누적 바이트. */
		wbytes = bis->cur.bytes[BLKG_IOSTAT_WRITE];
		/* [한국어] discard 누적 바이트. */
		dbytes = bis->cur.bytes[BLKG_IOSTAT_DISCARD];
		/* [한국어] 읽기 건수. */
		rios = bis->cur.ios[BLKG_IOSTAT_READ];
		/* [한국어] 쓰기 건수. */
		wios = bis->cur.ios[BLKG_IOSTAT_WRITE];
		/* [한국어] discard 건수. */
		dios = bis->cur.ios[BLKG_IOSTAT_DISCARD];
	} while (u64_stats_fetch_retry(&bis->sync, seq));

	/* [한국어] 모든 수치가 0 이면(= 이 cgroup 이 이 장치를 쓴 적이 없으면)
	 * 통계 부분을 생략한다. discard 만 있는 경우는 판정에서 빠져 있는데,
	 * 이는 상류 커널 코드 그대로다. */
	if (rbytes || wbytes || rios || wios) {
		/* [한국어] cgroup v2 io.stat 의 표준 키 이름들. */
		seq_printf(s, "rbytes=%llu wbytes=%llu rios=%llu wios=%llu dbytes=%llu dios=%llu",
			rbytes, wbytes, rios, wios,
			dbytes, dios);
	}

	/* [한국어] 디버그 모드이고 실제로 지연이 걸려 있는 blkg 에만 추가 정보를 붙인다.
	 * use_delay 는 "지연을 요구하는 정책 수" 격의 카운터라 0 이면 지연이 없다. */
	if (blkcg_debug_stats && atomic_read(&blkg->use_delay)) {
		/* [한국어] atomic 값들은 여기서 각각 따로 읽으므로 서로 완전히
		 * 일관된 스냅숏은 아니다 — 디버그 출력이라 허용된다. */
		seq_printf(s, " use_delay=%d delay_nsec=%llu",
			atomic_read(&blkg->use_delay),
			atomic64_read(&blkg->delay_nsec));
	}

	/* [한국어] 마지막으로 정책들이 자기 통계를 같은 줄에 이어 붙일 기회를 준다.
	 * 예: blk-iocost 는 cost.usage/cost.wait 같은 항목을 추가한다. */
	for (i = 0; i < BLKCG_MAX_POLS; i++) {
		/* [한국어] i 번 슬롯의 정책. */
		struct blkcg_policy *pol = blkcg_policy[i];

		/* [한국어] 이 blkg 에 pd 가 없거나(정책 비활성) 출력 콜백을 제공하지
		 * 않는 정책은 건너뛴다. */
		if (!blkg->pd[i] || !pol->pd_stat_fn)
			continue;

		/* [한국어] 정책이 자기 형식대로 seq_file 에 덧붙인다. */
		pol->pd_stat_fn(blkg->pd[i], s);
	}

	/* [한국어] 한 장치에 대한 줄을 끝맺는다. */
	seq_puts(s, "\n");
}

/*
 * [한국어]
 * blkcg_print_stat - cgroup v2 의 io.stat 파일 전체를 출력하는 seq_show 핸들러
 *
 * @sf: 출력 대상 seq_file. seq_css(sf) 로 어느 cgroup 의 파일인지 알아낸다.
 * @v:  seq_file 반복자 인자. 이 파일은 한 번에 전부 출력하므로 쓰지 않는다.
 * @return: 항상 0.
 *
 * 하는 일: 먼저 통계 원본을 최신화한 뒤, 이 cgroup 이 가진 blkg 마다
 * blkcg_print_one_stat() 으로 한 줄씩 찍는다. 즉 출력 줄 수 = 이 cgroup 이
 * 실제로 IO 를 낸 장치 수다.
 *
 * 실행 컨텍스트: cgroupfs read(프로세스 컨텍스트). css_rstat_flush() 가
 * 잠들 수 있으므로 락을 잡기 전에 부른다.
 *
 * 호출 체인:
 *   userspace read("io.stat") → cgroup 코어 → [blkcg_print_stat]
 *     → blkcg_fill_root_iostats() 또는 css_rstat_flush()
 *     → blkcg_print_one_stat()
 */
static int blkcg_print_stat(struct seq_file *sf, void *v)
{
	/* [한국어] 이 seq_file 이 속한 cgroup 의 blkcg. */
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));
	/* [한국어] blkg_list 순회 커서. */
	struct blkcg_gq *blkg;

	/* [한국어] ★ root와 non-root의 통계 출처가 다르다 ★
	 * root cgroup은 "cgroup에 속하지 않은 I/O를 포함한 전부"를 보여야 한다.
	 * 그런데 blkg 통계는 cgroup을 명시적으로 거친 I/O만 집계하므로, 커널
	 * 스레드가 낸 I/O 등이 빠진다. 그래서 root는 blkg 통계 대신
	 * /proc/diskstats와 같은 소스(디스크별 part_stat)에서 채운다. */
	/* [한국어] parent 가 NULL 이면 루트 cgroup 이다. */
	if (!seq_css(sf)->parent)
		/* [한국어] 루트는 blkg 통계 대신 디스크별 part_stat 합계로 채운다. */
		blkcg_fill_root_iostats();
	else
		/* [한국어] non-root는 per-CPU rstat에 흩어져 누적된 통계를 먼저
		 * 상위로 flush한다. cgroup의 rstat 인프라는 갱신 비용을 줄이려고
		 * CPU마다 값을 쌓아 두고, 읽을 때만 트리를 따라 합산한다.
		 * 이 호출이 없으면 방금 발생한 I/O가 통계에 반영되지 않는다. */
		css_rstat_flush(&blkcg->css);
	/* [한국어] 이 호출이 끝나면 blkg->iostat.cur 이 최신 상태가 된다. */

	/* [한국어] blkg 목록은 RCU로 보호된다. 순회 도중 다른 스레드가 blkg를
	 * 제거할 수 있는데, RCU 유예 해제 덕분에 이 구간에서는 안전하게 읽는다. */
	rcu_read_lock();
	/* [한국어] 이 cgroup에 속한 모든 blkg를 순회한다. blkg는 (cgroup × 디스크)
	 * 조합이므로, 시스템에 블록 장치가 여러 개면 각각에 대해
	 * 한 줄씩 출력된다. */
	hlist_for_each_entry_rcu(blkg, &blkcg->blkg_list, blkcg_node) {
		/* [한국어] 큐 락을 잡는다. blkcg_print_one_stat()이 정책별 통계
		 * (pd_stat_fn)까지 출력하는데, 그 정책 데이터가 큐 락으로 보호되기
		 * 때문이다. blkg마다 잡았다 놓아, 한 blkg 출력이 다른 디스크의
		 * I/O를 막지 않게 한다. */
		spin_lock_irq(&blkg->q->queue_lock);
		/* [한국어] 이 blkg 에 해당하는 한 줄을 출력한다. */
		blkcg_print_one_stat(blkg, sf);
		spin_unlock_irq(&blkg->q->queue_lock);
	}
	rcu_read_unlock();
	/* [한국어] seq_file 출력은 오류를 내지 않으므로 항상 성공. */
	return 0;
}

/* [한국어] cgroup v2(default hierarchy)에서 io 서브시스템이 만드는 파일 목록.
 * 여기서는 읽기 전용 "io.stat" 하나뿐이고, io.max/io.weight 같은 파일은
 * 각 정책(blk-throttle, iocost 등)이 자기 cftype 배열로 따로 등록한다.
 * 마지막 빈 원소 { } 가 배열의 끝 표식이다. */
static struct cftype blkcg_files[] = {
	{
		/* [한국어] cgroupfs 에 "io.stat" 으로 노출된다(접두사 io. 는 코어가 붙인다). */
		.name = "stat",
		/* [한국어] 읽기 핸들러. 위의 blkcg_print_stat 이 내용을 만든다. */
		.seq_show = blkcg_print_stat,
	},
	{ }	/* terminate */
};

/* [한국어] cgroup v1(legacy hierarchy) 전용 파일 목록. 현재는 통계 초기화용
 * "blkio.reset_stats" 하나만 남아 있으며 deprecated 경고를 낸다. */
static struct cftype blkcg_legacy_files[] = {
	{
		/* [한국어] cgroupfs 에 "blkio.reset_stats" 로 노출된다. */
		.name = "reset_stats",
		/* [한국어] 쓰기 핸들러. 값은 무시하고 통계를 0 으로 만든다. */
		.write_u64 = blkcg_reset_stats,
	},
	{ }	/* terminate */
};

#ifdef CONFIG_CGROUP_WRITEBACK
/*
 * [한국어]
 * blkcg_get_cgwb_list - 이 cgroup에 속한 cgroup writeback 구조체 목록을 반환
 *
 * @css: blkcg의 cgroup_subsys_state
 * @return: blkcg->cgwb_list의 주소(항상 유효)
 *
 * cgroup writeback(cgwb)은 "어느 cgroup의 더티 페이지를 어느 장치로
 * write-back할 것인가"를 cgroup별로 분리해 관리하는 메커니즘이다. 그
 * 구조체들은 blkcg마다 리스트로 매달려 있고, mm 계층(mm/backing-dev.c)이
 * 그 리스트를 순회해야 할 때가 있다.
 *
 * 이 함수가 존재하는 이유는 계층 분리다. mm 계층은 struct blkcg의 정의를
 * 알지 못하므로(블록 계층 내부 타입), css 포인터만 넘겨받아 리스트 주소를
 * 얻는 접근자를 블록 계층이 제공한다.
 *
 * 실행 컨텍스트: mm의 writeback 경로. 리스트 자체의 동기화는 호출자가
 * blkcg->lock 등으로 처리한다.
 *
 * 호출 체인:
 *   mm/backing-dev.c의 cgwb 정리 경로 → [blkcg_get_cgwb_list]
 */
struct list_head *blkcg_get_cgwb_list(struct cgroup_subsys_state *css)
{
	/* [한국어] css → blkcg 로 되돌린 뒤 그 안의 리스트 헤드 주소를 준다.
	 * 리스트 내용 자체는 넘겨주지 않고 주소만 노출하는 얇은 접근자다. */
	return &css_to_blkcg(css)->cgwb_list;
}
#endif

/*
 * blkcg destruction is a three-stage process.
 *
 * 1. Destruction starts.  The blkcg_css_offline() callback is invoked
 *    which offlines writeback.  Here we tie the next stage of blkg destruction
 *    to the completion of writeback associated with the blkcg.  This lets us
 *    avoid punting potentially large amounts of outstanding writeback to root
 *    while maintaining any ongoing policies.  The next stage is triggered when
 *    the nr_cgwbs count goes to zero.
 *
 * 2. When the nr_cgwbs count goes to zero, blkcg_destroy_blkgs() is called
 *    and handles the destruction of blkgs.  Here the css reference held by
 *    the blkg is put back eventually allowing blkcg_css_free() to be called.
 *    This work may occur in cgwb_release_workfn() on the cgwb_release
 *    workqueue.  Any submitted ios that fail to get the blkg ref will be
 *    punted to the root_blkg.
 *
 * 3. Once the blkcg ref count goes to zero, blkcg_css_free() is called.
 *    This finally frees the blkcg.
 */

/**
 * blkcg_destroy_blkgs - responsible for shooting down blkgs
 * @blkcg: blkcg of interest
 *
 * blkgs should be removed while holding both q and blkcg locks.  As blkcg lock
 * is nested inside q lock, this function performs reverse double lock dancing.
 * Destroying the blkgs releases the reference held on the blkcg's css allowing
 * blkcg_css_free to eventually be called.
 *
 * This is the blkcg counterpart of ioc_release_fn().
 */
/*
 * [한국어]
 * blkcg_destroy_blkgs - blkcg 의 모든 blkg 를 제거
 *
 * 호출 경로: blkcg_unpin_online() -> blkcg_destroy_blkgs()
 * cgroup 이 제거되면 그 cgroup 이 각 request_queue 에
 *   남긴 blkg 를 모두 정리한다. blkcg lock 과 queue lock 의 lock ordering 을
 *   맞추기 위해 역순으로 락을 잡는다.
 */

static void blkcg_destroy_blkgs(struct blkcg *blkcg)
{
	/* [한국어] 아래에서 cond_resched() 를 부르므로 이 함수는 잠들 수 있는
	 * 문맥에서만 호출돼야 한다. 디버그 커널에서 그 계약을 검사한다. */
	might_sleep();

	/* [한국어] 이 cgroup 의 blkg_list 를 보호한다. 잠금 순서 규약은
	 * queue_lock → blkcg->lock 인데, 여기서는 반대로 blkcg->lock 을 먼저
	 * 잡을 수밖에 없다(리스트가 blkcg 쪽에 있으므로). 그래서 아래에서
	 * queue_lock 은 trylock 으로만 잡는 "역순 이중 락 춤" 을 춘다
	 * (위 영문 주석의 reverse double lock dancing). */
	spin_lock_irq(&blkcg->lock);

	/* [한국어] 리스트가 빌 때까지 계속 첫 항목을 지운다. blkg_destroy() 가
	 * blkcg_node 를 해시에서 빼므로 반복마다 리스트가 줄어든다. */
	while (!hlist_empty(&blkcg->blkg_list)) {
		/* [한국어] 항상 첫 번째 항목을 대상으로 삼는다. 커서를 유지하지 않으므로
		 * 중간에 락을 놓았다 잡아도 안전하다. */
		struct blkcg_gq *blkg = hlist_entry(blkcg->blkg_list.first,
						struct blkcg_gq, blkcg_node);
		/* [한국어] 이 blkg 가 붙어 있는 큐. queue_lock 을 잡아야 지울 수 있다. */
		struct request_queue *q = blkg->q;

		/* [한국어] 두 가지 이유로 물러난다.
		 * (1) need_resched(): blkg 가 아주 많으면 락을 오래 쥐어 softlockup 이 난다.
		 * (2) trylock 실패: 잠금 순서를 어기고 있으므로 기다리면 데드락이다.
		 *     기다리는 대신 blkcg->lock 을 놓고 처음부터 다시 시도한다. */
		if (need_resched() || !spin_trylock(&q->queue_lock)) {
			/*
			 * Given that the system can accumulate a huge number
			 * of blkgs in pathological cases, check to see if we
			 * need to rescheduling to avoid softlockup.
			 */
			/* [한국어] 바깥 락을 놓아 상대가 진행할 수 있게 한다. */
			spin_unlock_irq(&blkcg->lock);
			/* [한국어] 필요하면 스케줄러에 양보. */
			cond_resched();
			/* [한국어] 다시 잡고 루프 조건부터 재평가한다. */
			spin_lock_irq(&blkcg->lock);
			continue;
		}

		/* [한국어] 두 락을 모두 쥔 상태 — 이제 안전하게 제거할 수 있다. */
		blkg_destroy(blkg);
		/* [한국어] trylock 으로 잡은 안쪽 락만 푼다(인터럽트 상태는
		 * 바깥 blkcg->lock 이 계속 들고 있으므로 _irq 변형이 아니다). */
		spin_unlock(&q->queue_lock);
	}

	/* [한국어] 리스트가 비었다 — 이 cgroup 은 더 이상 어떤 blkg 도 갖지 않는다.
	 * blkg 들이 들고 있던 css 참조가 반납되므로 blkcg_css_free() 로 가는 길이 열린다. */
	spin_unlock_irq(&blkcg->lock);
}

/**
 * blkcg_pin_online - pin online state
 * @blkcg_css: blkcg of interest
 *
 * While pinned, a blkcg is kept online.  This is primarily used to
 * impedance-match blkg and cgwb lifetimes so that blkg doesn't go offline
 * while an associated cgwb is still active.
 */
/*
 * [한국어]
 * blkcg_pin_online - blkcg가 offline 처리되지 않도록 참조를 건다
 *
 * @blkcg_css: 고정할 blkcg의 css
 * @return: 없음
 *
 * === online_pin이 필요한 이유 ===
 * 사용자가 cgroup 디렉터리를 지우면 cgroup 코어가 offline 절차를 시작한다.
 * 그런데 그 시점에 아직 그 cgroup에 속한 write-back I/O가 남아 있을 수 있다.
 * offline이 진행되면 정책 데이터가 해제되어, 뒤늦게 완료되는 I/O가 이미
 * 사라진 자료구조를 참조하게 된다.
 *
 * online_pin은 그것을 막는 별도의 참조 카운터다. 일반적인 css 참조와 달리
 * "구조체가 살아 있는가"가 아니라 "offline 콜백을 미룰 것인가"를 제어한다.
 * blkcg_unpin_online()으로 마지막 참조가 풀릴 때 비로소 실제 offline
 * 처리(정책 데이터 해제)가 진행된다.
 *
 * 주 사용처는 cgroup writeback으로, 더티 페이지가 남아 있는 동안 cgroup을
 * 붙잡아 둔다.
 *
 * 실행 컨텍스트: 어디서든(refcount 원자적 증가).
 *
 * 호출 체인:
 *   mm의 cgroup writeback 초기화 → [blkcg_pin_online]
 *   해제: blkcg_unpin_online → blkcg_css_offline 실제 수행
 */
void blkcg_pin_online(struct cgroup_subsys_state *blkcg_css)
{
	/* [한국어] online_pin 을 원자적으로 1 증가시킨다. refcount_t 는 오버플로/
	 * use-after-free 를 잡아내는 검사가 붙은 원자 카운터다.
	 * 짝이 되는 감소는 blkcg_unpin_online(). */
	refcount_inc(&css_to_blkcg(blkcg_css)->online_pin);
}

/**
 * blkcg_unpin_online - unpin online state
 * @blkcg_css: blkcg of interest
 *
 * This is primarily used to impedance-match blkg and cgwb lifetimes so
 * that blkg doesn't go offline while an associated cgwb is still active.
 * When this count goes to zero, all active cgwbs have finished so the
 * blkcg can continue destruction by calling blkcg_destroy_blkgs().
 */
/*
 * [한국어]
 * blkcg_unpin_online - online 고정을 풀고, 0 이 되면 blkg 일괄 제거를 시작한다
 *
 * @blkcg_css: 고정을 풀 blkcg 의 css.
 * @return: 없음.
 *
 * 왜 부모까지 거슬러 올라가는가: 자식 cgroup 은 살아 있는 동안 부모의
 * online_pin 을 하나 잡고 있다(blkcg_css_online 에서 blkcg_pin_online 호출).
 * 그래서 자식의 pin 이 0 이 되어 자식이 정리되면, 그 자식이 잡고 있던 부모의
 * pin 도 함께 풀어야 한다. 재귀 대신 루프로 부모를 따라 올라가며 처리하는데,
 * 어느 단계에서 카운트가 0 이 아니면(다른 자식이나 cgwb 가 아직 붙잡고 있으면)
 * 거기서 멈춘다. 재귀를 쓰지 않는 것은 계층이 깊을 때 스택 사용을 피하기 위함이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. blkcg_destroy_blkgs() 가 잠들 수 있으므로
 * 아토믹 문맥에서 부르면 안 된다(cgwb_release_workfn 같은 워커에서 불린다).
 *
 * 호출 체인:
 *   blkcg_css_offline() 또는 cgwb 해제 경로 → [blkcg_unpin_online]
 *     → blkcg_destroy_blkgs() → blkg_destroy()
 */
void blkcg_unpin_online(struct cgroup_subsys_state *blkcg_css)
{
	/* [한국어] css 를 blkcg 로 되돌린다. 루프 안에서 부모로 갱신된다. */
	struct blkcg *blkcg = css_to_blkcg(blkcg_css);

	/* [한국어] 자기 자신부터 시작해 루트 방향으로 올라간다. */
	do {
		/* [한국어] blkg 를 지우기 전에 부모 포인터를 미리 읽어 둔다 —
		 * blkcg_destroy_blkgs() 이후에도 blkcg 자체는 살아 있지만,
		 * 코드 흐름을 명확히 하기 위해 먼저 확보한다. */
		struct blkcg *parent;

		/* [한국어] 카운트를 1 줄이고 0 이 되었는지 본다. 0 이 아니면 아직
		 * 이 cgroup 을 붙잡고 있는 주체가 남았다는 뜻이라 여기서 멈춘다.
		 * refcount_dec_and_test 는 감소와 판정을 원자적으로 수행한다. */
		if (!refcount_dec_and_test(&blkcg->online_pin))
			break;

		/* [한국어] 다음 단계에서 처리할 부모. 루트면 NULL 이 되어 루프가 끝난다. */
		parent = blkcg_parent(blkcg);
		/* [한국어] 이 cgroup 이 가진 모든 blkg 를 제거한다(2단계 소멸의 핵심). */
		blkcg_destroy_blkgs(blkcg);
		/* [한국어] 부모의 pin 도 이 자식이 잡고 있었으므로 이어서 처리한다. */
		blkcg = parent;
	} while (blkcg);
}

/**
 * blkcg_css_offline - cgroup css_offline callback
 * @css: css of interest
 *
 * This function is called when @css is about to go away.  Here the cgwbs are
 * offlined first and only once writeback associated with the blkcg has
 * finished do we start step 2 (see above).
 */
/*
 * [한국어]
 * blkcg_css_offline - cgroup 이 사라지기 시작할 때 cgroup 코어가 부르는 콜백
 *
 * @css: 오프라인 처리할 cgroup 의 css.
 * @return: 없음.
 *
 * 이 파일 위쪽의 영문 주석이 설명하는 "3단계 소멸" 중 1단계다.
 *   1단계(여기): cgroup writeback 을 먼저 오프라인시키고, 기본 online_pin 을
 *                놓는다. 아직 cgwb 가 남아 있으면 카운트가 0 이 되지 않아
 *                2단계로 넘어가지 않는다.
 *   2단계: 카운트가 0 이 되는 순간 blkcg_destroy_blkgs() 가 불려 blkg 를 지우고,
 *          blkg 들이 들고 있던 css 참조가 반납된다.
 *   3단계: css 참조가 0 이 되면 blkcg_css_free() 가 blkcg 를 해제한다.
 * writeback 을 먼저 처리하는 이유는, 아직 내려쓰지 않은 대량의 더티 페이지를
 * 루트 cgroup 으로 떠넘기지 않으면서 진행 중인 정책은 유지하기 위해서다.
 *
 * 실행 컨텍스트: cgroup 소멸 경로(프로세스 컨텍스트, 잠들 수 있음).
 *
 * 호출 체인:
 *   rmdir(cgroup) → cgroup 코어 → [blkcg_css_offline]
 *     → wb_blkcg_offline() / blkcg_unpin_online()
 */
static void blkcg_css_offline(struct cgroup_subsys_state *css)
{
	/* this prevents anyone from attaching or migrating to this blkcg */
	/* [한국어] 이 cgroup 에 딸린 cgroup writeback 구조체들을 오프라인 표시한다.
	 * 각 cgwb 가 풀릴 때마다 blkcg_unpin_online() 이 불려 카운트가 줄어든다. */
	wb_blkcg_offline(css);

	/* put the base online pin allowing step 2 to be triggered */
	/* [한국어] blkcg_css_alloc() 에서 1 로 세팅했던 "기본" pin 을 놓는다.
	 * 남은 cgwb 가 없다면 여기서 카운트가 0 이 되어 곧바로 2단계
	 * (blkcg_destroy_blkgs)가 실행된다. */
	blkcg_unpin_online(css);
}

/*
 * [한국어]
 * blkcg_css_free - blkcg 소멸 3단계 중 마지막: 구조체와 부속 자원을 해제한다
 *
 * @css: 해제할 blkcg 의 css. 이 시점에는 css 참조가 0 이라 아무도 참조하지 않는다.
 * @return: 없음.
 *
 * 여기 도달했다는 것은 이 cgroup 의 blkg 가 모두 지워졌고(각 blkg 가 들고 있던
 * css 참조가 반납됐고), 다른 참조도 남지 않았다는 뜻이다. 그래서 이제
 * cgroup 단위 정책 데이터(cpd)와 per-cpu 통계 리스트를 놓을 수 있다.
 *
 * blkg 의 pd 와 blkcg 의 cpd 구분: pd 는 (cgroup, 장치) 조합마다 하나,
 * cpd 는 cgroup 하나에 정책마다 하나다. 예컨대 "이 cgroup 의 기본 weight" 같은
 * 장치 무관 설정이 cpd 에 들어간다.
 *
 * 실행 컨텍스트: cgroup 코어의 css 해제 경로(프로세스 컨텍스트).
 * blkcg_pol_mutex 를 잡으므로 잠들 수 있어야 한다.
 *
 * 호출 체인:
 *   css 참조 0 → cgroup 코어 → [blkcg_css_free] → cpd_free_fn() / kfree()
 */
static void blkcg_css_free(struct cgroup_subsys_state *css)
{
	/* [한국어] 해제 대상 blkcg. */
	struct blkcg *blkcg = css_to_blkcg(css);
	/* [한국어] 정책 슬롯 인덱스. */
	int i;

	/* [한국어] all_blkcgs 리스트와 cpd 해제를 정책 등록/해제와 직렬화한다.
	 * 이 락을 놓치면 blkcg_policy_register() 가 방금 해제 중인 blkcg 에
	 * cpd 를 할당하려 들 수 있다. */
	mutex_lock(&blkcg_pol_mutex);

	/* [한국어] 전역 blkcg 목록에서 뺀다. 이후 새로 등록되는 정책이
	 * 이 blkcg 를 대상으로 삼지 않는다. */
	list_del(&blkcg->all_blkcgs_node);

	/* [한국어] 정책별 cgroup 단위 데이터를 모두 해제한다. */
	for (i = 0; i < BLKCG_MAX_POLS; i++)
		/* [한국어] 해당 정책이 cpd 를 요구하지 않았다면 NULL 이다. */
		if (blkcg->cpd[i])
			/* [한국어] 정책이 스스로 정의한 해제 콜백. */
			blkcg_policy[i]->cpd_free_fn(blkcg->cpd[i]);

	mutex_unlock(&blkcg_pol_mutex);

	/* [한국어] init_blkcg_llists() 가 할당했던 per-cpu lockless list 반납.
	 * 락 밖에서 해도 되는 이유는 이 blkcg 를 참조하는 주체가 이미 없기 때문. */
	free_percpu(blkcg->lhead);
	/* [한국어] 마지막으로 blkcg 구조체 자체 해제.
	 * 주의: blkcg_root 는 정적 전역이라 이 경로로 오지 않는다. */
	kfree(blkcg);
}

static struct cgroup_subsys_state *
/*
 * [한국어]
 * blkcg_css_alloc - cgroup 이 만들어질 때 그 cgroup 의 블록 서브시스템 상태를 생성
 *
 * @parent_css: 부모 cgroup 의 css. NULL 이면 루트 cgroup 생성이다.
 * @return: 새 blkcg 의 css, 실패 시 ERR_PTR(-ENOMEM).
 *
 * 하는 일:
 *   1) blkcg 구조체 확보(루트는 정적 blkcg_root 재사용)
 *   2) per-cpu 통계 lockless list 준비(init_blkcg_llists)
 *   3) 이미 등록된 모든 정책에 대해 cpd(cgroup 단위 정책 데이터) 할당
 *   4) 조회 자료구조 초기화: blkg_tree(radix), blkg_list(hlist), 락, pin 카운트
 *   5) 전역 all_blkcgs 목록에 등록 — 나중에 새 정책이 등록될 때 이 blkcg 도
 *      cpd 를 소급 할당받을 수 있게 하기 위함
 * 이 시점에는 blkg 가 하나도 없다. blkg 는 실제로 IO 가 나거나 설정이 걸릴 때
 * blkg_lookup_create()/blkg_conf_prep() 이 만든다.
 *
 * 실행 컨텍스트: mkdir(cgroup) 경로(프로세스 컨텍스트). GFP_KERNEL 할당과
 * 뮤텍스를 쓰므로 잠들 수 있다.
 *
 * 호출 체인:
 *   mkdir(cgroup) → cgroup 코어 → [blkcg_css_alloc]
 *     → init_blkcg_llists() / pol->cpd_alloc_fn()
 */
blkcg_css_alloc(struct cgroup_subsys_state *parent_css)
{
	/* [한국어] 만들어 반환할 blkcg. 실패 경로에서 부분 해제 대상. */
	struct blkcg *blkcg;
	/* [한국어] 정책 슬롯 인덱스. 실패 시 이 값부터 역순으로 되돌린다. */
	int i;

	/* [한국어] 정책 테이블(blkcg_policy[])을 읽으며 cpd 를 만들어야 하므로,
	 * 그 사이 정책이 등록/해제되지 않도록 고정한다. all_blkcgs 리스트도 보호. */
	mutex_lock(&blkcg_pol_mutex);

	/* [한국어] parent_css 가 NULL 이면 루트 cgroup 이다. */
	if (!parent_css) {
		/* [한국어] 루트는 부팅 초기부터 필요하므로 정적 전역을 그대로 쓴다.
		 * 그래서 실패 경로에서도 kfree 하지 않는다(아래 free_blkcg 참조). */
		blkcg = &blkcg_root;
	} else {
		/* [한국어] 일반 cgroup 은 0 으로 초기화된 blkcg 를 새로 할당한다.
		 * kzalloc 이므로 cpd[] 가 전부 NULL 로 시작하고, 이는 실패 경로의
		 * NULL 검사가 성립하는 전제다. */
		blkcg = kzalloc_obj(*blkcg);
		if (!blkcg)
			/* [한국어] 아직 아무 것도 잡지 않았으므로 락만 풀고 나간다. */
			goto unlock;
	}

	/* [한국어] per-cpu 통계 lockless list 준비. 실패하면 blkcg 자체를 되돌린다. */
	if (init_blkcg_llists(blkcg))
		goto free_blkcg;

	/* [한국어] 지금 등록돼 있는 모든 정책에 대해 cpd 를 만든다.
	 * 나중에 등록되는 정책은 blkcg_policy_register() 가 all_blkcgs 를 훑으며
	 * 소급해서 채워 준다. */
	for (i = 0; i < BLKCG_MAX_POLS ; i++) {
		/* [한국어] i 번 슬롯의 정책(없으면 NULL). */
		struct blkcg_policy *pol = blkcg_policy[i];
		/* [한국어] 이 정책이 이 cgroup 에 붙일 cgroup 단위 데이터. */
		struct blkcg_policy_data *cpd;

		/*
		 * If the policy hasn't been attached yet, wait for it
		 * to be attached before doing anything else. Otherwise,
		 * check if the policy requires any specific per-cgroup
		 * data: if it does, allocate and initialize it.
		 */
		/* [한국어] 슬롯이 비었거나, 그 정책이 cgroup 단위 데이터를 쓰지 않으면
		 * (cpd_alloc_fn 이 NULL) 할 일이 없다. */
		if (!pol || !pol->cpd_alloc_fn)
			continue;

		/* [한국어] 정책이 자기 구조체를 할당한다. 여기서는 잠들 수 있으므로
		 * GFP_KERNEL 을 쓴다(뮤텍스만 쥐고 있고 스핀락은 없다). */
		cpd = pol->cpd_alloc_fn(GFP_KERNEL);
		if (!cpd)
			/* [한국어] 여기까지 만든 cpd 들을 역순으로 해제해야 한다. */
			goto free_pd_blkcg;

		/* [한국어] i 번 슬롯에 꽂는다. blkg->pd[] 와 같은 인덱스 규약이다. */
		blkcg->cpd[i] = cpd;
		/* [한국어] cpd → blkcg 역참조. 정책 콜백이 cpd 만 받기 때문에 필요하다. */
		cpd->blkcg = blkcg;
		/* [한국어] 자기 슬롯 번호 기록. */
		cpd->plid = i;
	}

	/* [한국어] blkg_tree/blkg_list 를 보호할 스핀락 초기화. */
	spin_lock_init(&blkcg->lock);
	/* [한국어] "기본" online pin 1 개. blkcg_css_offline() 이 이것을 놓는다.
	 * cgwb 가 추가로 잡는 pin 들과 합쳐져 0 이 될 때 blkg 정리가 시작된다. */
	refcount_set(&blkcg->online_pin, 1);
	/* [한국어] (queue id → blkg) radix tree 초기화. GFP_NOWAIT 는 이 트리가
	 * 내부 노드를 할당할 때 쓰는 플래그로, 스핀락 안에서 삽입되기 때문이다
	 * (그래서 호출자가 radix_tree_preload 로 미리 채워 둔다). */
	INIT_RADIX_TREE(&blkcg->blkg_tree, GFP_NOWAIT);
	/* [한국어] 이 cgroup 이 가진 blkg 들을 잇는 해시 리스트 헤드 초기화.
	 * RCU 순회 대상이라 hlist_add_head_rcu/hlist_del_init_rcu 로 조작된다. */
	INIT_HLIST_HEAD(&blkcg->blkg_list);
#ifdef CONFIG_CGROUP_WRITEBACK
	/* [한국어] cgroup writeback 구조체 목록. mm 계층이 blkcg_get_cgwb_list()
	 * 로 접근한다. 해당 기능이 꺼진 커널에는 필드 자체가 없다. */
	INIT_LIST_HEAD(&blkcg->cgwb_list);
#endif
	/* [한국어] 전역 목록에 등록. 이후 새 정책이 등록되면 이 blkcg 도
	 * cpd 를 받게 된다. */
	list_add_tail(&blkcg->all_blkcgs_node, &all_blkcgs);

	mutex_unlock(&blkcg_pol_mutex);
	/* [한국어] cgroup 코어에는 blkcg 가 아니라 그 안의 css 를 돌려준다. */
	return &blkcg->css;

free_pd_blkcg:
	/* [한국어] 실패한 i 번은 할당되지 않았으므로 i-1 부터 역순으로 해제. */
	for (i--; i >= 0; i--)
		/* [한국어] cpd 를 쓰지 않는 정책 슬롯은 NULL 이다. */
		if (blkcg->cpd[i])
			blkcg_policy[i]->cpd_free_fn(blkcg->cpd[i]);
	/* [한국어] 통계 리스트도 반납. */
	free_percpu(blkcg->lhead);
free_blkcg:
	/* [한국어] 루트는 정적 전역이므로 절대 kfree 하면 안 된다. */
	if (blkcg != &blkcg_root)
		kfree(blkcg);
unlock:
	mutex_unlock(&blkcg_pol_mutex);
	/* [한국어] 실패 원인은 모두 메모리 부족이므로 -ENOMEM 하나로 통일한다. */
	return ERR_PTR(-ENOMEM);
}

/*
 * [한국어]
 * blkcg_css_online - cgroup 이 사용 가능해질 때 불리는 콜백
 *
 * @css: 온라인 처리할 cgroup 의 css.
 * @return: 0(성공만 가능). 음수를 반환하면 cgroup 생성이 실패한다.
 *
 * 하는 일은 단 하나 — 부모의 online_pin 을 잡는 것이다.
 * 왜 필요한가: 소멸이 항상 잎에서 뿌리 방향으로 일어나야 한다. 부모가
 * 자식보다 먼저 오프라인되면, 자식 blkg 가 참조하는 부모 blkg 가 먼저
 * 사라져 계층 통계/제한이 깨진다. 자식이 부모의 pin 을 하나 잡고 있으면
 * 부모의 pin 카운트가 0 이 될 수 없어 그 순서가 강제된다.
 * 이 pin 은 자식이 정리될 때 blkcg_unpin_online() 의 부모 순회 루프가 놓는다.
 *
 * 실행 컨텍스트: mkdir(cgroup) 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   cgroup 코어 → [blkcg_css_online] → blkcg_pin_online(parent)
 */
static int blkcg_css_online(struct cgroup_subsys_state *css)
{
	/* [한국어] 이 cgroup 의 부모 blkcg. 루트라면 NULL 이다. */
	struct blkcg *parent = blkcg_parent(css_to_blkcg(css));

	/*
	 * blkcg_pin_online() is used to delay blkcg offline so that blkgs
	 * don't go offline while cgwbs are still active on them.  Pin the
	 * parent so that offline always happens towards the root.
	 */
	/* [한국어] 루트에는 부모가 없으므로 건너뛴다. */
	if (parent)
		blkcg_pin_online(&parent->css);
	/* [한국어] 실패할 수 있는 작업이 없으므로 항상 성공. */
	return 0;
}

/*
 * [한국어]
 * blkg_init_queue - request_queue 안의 blkcg 관련 필드를 초기 상태로 만든다
 *
 * @q: 갓 할당된 request_queue.
 * @return: 없음.
 *
 * blkg 는 (blkcg, queue) 쌍이므로 큐 쪽에도 blkg 를 매달 자리가 필요하다.
 * 이 함수는 그 자리(blkg_list)와, blkg 해제/정책 비활성화를 직렬화하는
 * 뮤텍스만 준비한다. 실제 blkg(루트 blkg 포함)는 gendisk 가 붙는
 * blkcg_init_disk() 시점에 만들어진다 — 큐가 디스크보다 먼저 존재할 수 있어
 * 두 단계로 나뉘어 있다.
 *
 * 실행 컨텍스트: 큐 할당 경로(프로세스 컨텍스트). 아직 아무도 이 큐를
 * 볼 수 없으므로 동기화가 필요 없다.
 *
 * 호출 체인:
 *   blk_alloc_queue() → [blkg_init_queue]
 */
void blkg_init_queue(struct request_queue *q)
{
	/* [한국어] 이 큐에 붙을 blkg 들을 잇는 리스트 헤드. blkg_create() 가 추가하고
	 * blkg_free_workfn() 이 제거한다. queue_lock 으로 보호된다. */
	INIT_LIST_HEAD(&q->blkg_list);
	/* [한국어] blkg_free_workfn() 의 pd 해제와 blkcg_deactivate_policy() 사이를
	 * 직렬화하는 뮤텍스. blkg_conf_prep() 도 이 락을 쓴다. */
	mutex_init(&q->blkcg_mutex);
}

/*
 * [한국어]
 * blkcg_init_disk - 디스크에 blkcg 기반을 세운다(= 루트 blkg 를 만든다)
 *
 * @disk: 새로 등장한 gendisk.
 * @return: 0 성공, 음수 errno(-ENOMEM, blkg_create 실패 코드).
 *
 * 왜 루트 blkg 를 미리 만드는가: blkg_lookup_create() 는 생성에 실패하면
 * "가장 가까운 조상 blkg" 로 되돌아가는데, 그 최후의 보루가 q->root_blkg 다.
 * 이것만은 메모리 압박 상황에서도 반드시 존재해야 하므로, 디스크가 등장하는
 * 시점(아직 IO 가 없어 GFP_KERNEL 을 쓸 수 있는 시점)에 미리 만들어 둔다.
 *
 * rebind 대기(wait_var_event)가 필요한 이유는 아래 영문 주석 그대로다:
 * SCSI 처럼 큐를 디스크 간에 재사용하는 경우, 이전 디스크의 blkg 정리가
 * 비동기(disk_release → blkcg_exit_disk)라 아직 끝나지 않았을 수 있다.
 * 그 상태에서 새로 만들면 같은 queue id 슬롯이 blkcg->blkg_tree 에 남아 있어
 * radix_tree_insert() 가 -EEXIST 로 실패한다. 그래서 root_blkg 가 NULL 이
 * 되기를(= 이전 정리 완료를) 기다린다.
 *
 * 실행 컨텍스트: 디스크 등록 경로(프로세스 컨텍스트). wait_var_event 와
 * GFP_KERNEL 때문에 잠들 수 있어야 한다.
 *
 * 호출 체인:
 *   add_disk()/디스크 초기화 → [blkcg_init_disk] → blkg_alloc() → blkg_create()
 */
int blkcg_init_disk(struct gendisk *disk)
{
	/* [한국어] blkg 가 실제로 붙는 대상. */
	struct request_queue *q = disk->queue;
	/* [한국어] new_blkg 는 미리 할당해 둘 blkg, blkg 는 등록 결과. */
	struct blkcg_gq *new_blkg, *blkg;
	/* [한국어] radix_tree_preload 를 성공적으로 했는지. 성공했을 때만
	 * 짝이 되는 preload_end 를 불러야 한다. */
	bool preloaded;

	/*
	 * If the queue is shared across disk rebind (e.g., SCSI), the
	 * previous disk's blkcg state is cleaned up asynchronously via
	 * disk_release() -> blkcg_exit_disk(). Wait for that cleanup to
	 * finish (indicated by root_blkg becoming NULL) before setting up
	 * new blkcg state. Otherwise, we may overwrite q->root_blkg while
	 * the old one is still alive, and radix_tree_insert() in
	 * blkg_create() will fail with -EEXIST because the old entries
	 * still occupy the same queue id slot in blkcg->blkg_tree.
	 */
	/* [한국어] 조건이 참이 될 때까지 잠들며 기다린다. 깨우는 쪽은
	 * blkg_destroy_all() 끝의 wake_up_var(&q->root_blkg) 다.
	 * READ_ONCE 로 읽는 이유는 다른 CPU 가 쓰는 값을 컴파일러가
	 * 루프 밖으로 끌어내 캐싱하지 못하게 하기 위함이다. */
	wait_var_event(&q->root_blkg, !READ_ONCE(q->root_blkg));

	/* [한국어] 루트 cgroup 용 blkg 를 미리 만든다. 아직 락 밖이라
	 * GFP_KERNEL 로 넉넉하게 할당할 수 있다. */
	new_blkg = blkg_alloc(&blkcg_root, disk, GFP_KERNEL);
	if (!new_blkg)
		/* [한국어] 루트 blkg 없이는 이 디스크에서 blkcg 를 쓸 수 없다. */
		return -ENOMEM;

	/* [한국어] radix tree 삽입은 스핀락 안에서 일어나 할당을 할 수 없으므로,
	 * 미리 per-CPU 캐시에 노드를 채워 둔다. 반환값 0 이 성공이라 ! 로 뒤집어
	 * preloaded 에 담는다. 성공 시 preemption 이 꺼진 상태가 된다. */
	preloaded = !radix_tree_preload(GFP_KERNEL);

	/* Make sure the root blkg exists. */
	/* spin_lock_irq can serve as RCU read-side critical section. */
	/* [한국어] blkg_create() 가 요구하는 락. 영문 주석대로 spin_lock_irq 구간은
	 * RCU read-side 로도 동작하므로 안에서 blkg_lookup() 을 불러도 안전하다. */
	spin_lock_irq(&q->queue_lock);
	/* [한국어] 미리 만든 new_blkg 를 넘겨 등록만 시킨다.
	 * 성공/실패와 무관하게 new_blkg 는 blkg_create 가 소비한다. */
	blkg = blkg_create(&blkcg_root, disk, new_blkg);
	if (IS_ERR(blkg))
		goto err_unlock;
	/* [한국어] 큐가 루트 blkg 를 직접 가리키게 한다. blkg_lookup() 이
	 * 루트를 radix tree 없이 곧바로 찾는 지름길이자, 생성 실패 시의 대체값이다. */
	q->root_blkg = blkg;
	spin_unlock_irq(&q->queue_lock);

	/* [한국어] preload 를 했다면 반드시 닫아 preemption 을 되살린다. */
	if (preloaded)
		radix_tree_preload_end();

	return 0;

err_unlock:
	/* [한국어] 실패 경로도 락과 preload 를 정확히 되돌린다. */
	spin_unlock_irq(&q->queue_lock);
	if (preloaded)
		radix_tree_preload_end();
	/* [한국어] blkg_create 가 포인터에 실어 보낸 오류 코드를 꺼내 반환한다. */
	return PTR_ERR(blkg);
}

/*
 * [한국어]
 * blkcg_exit_disk - 디스크가 사라질 때 blkcg 관련 자원을 모두 정리한다
 *
 * @disk: 제거되는 gendisk.
 * @return: 없음.
 *
 * blkcg_init_disk() 의 반대편이다. 이 디스크 큐에 붙어 있던 모든 blkg 를
 * 지우고(2차원 격자에서 이 장치의 "열" 전체), 내장 정책인 blk-throttle 의
 * 디스크 단위 자원도 정리한다.
 *
 * blkg_destroy_all() 이 끝나면서 q->root_blkg 가 NULL 이 되고
 * wake_up_var 가 호출되므로, 같은 큐를 재사용하려고 기다리던
 * blkcg_init_disk() 가 깨어난다.
 *
 * 실행 컨텍스트: 디스크 해제 경로(disk_release, 프로세스 컨텍스트).
 * blkg_destroy_all() 이 cond_resched() 를 부르므로 잠들 수 있어야 한다.
 *
 * 호출 체인:
 *   disk_release() → [blkcg_exit_disk] → blkg_destroy_all() / blk_throtl_exit()
 */
void blkcg_exit_disk(struct gendisk *disk)
{
	/* [한국어] 이 디스크 큐의 blkg 를 전부 제거하고 정책 활성 비트를 지운다. */
	blkg_destroy_all(disk);
	/* [한국어] blk-throttle 이 디스크 단위로 들고 있던 자원(타이머, 서비스 큐 등)
	 * 을 정리한다. blkg 의 pd 해제와는 별개인 디스크 레벨 정리다. */
	blk_throtl_exit(disk);
}

/*
 * [한국어]
 * blkcg_exit - 태스크가 종료될 때 남은 블록 계층 스로틀 상태를 정리
 *
 * @tsk: 종료 중인 태스크
 * @return: 없음
 *
 * cgroup_subsys의 exit 콜백으로 등록되어, 태스크가 사라질 때 호출된다.
 *
 * 정리 대상은 tsk->throttle_disk다. blk-throttle이나 blk-iocost가 이 태스크를
 * 스로틀하기로 결정하면, "스케줄 아웃될 때 지연을 부과하라"는 표시로 대상
 * 디스크를 태스크에 매달아 둔다(blkcg_schedule_throttle). 그런데 지연이
 * 부과되기 전에 태스크가 종료되면 그 참조가 남아 디스크가 해제되지 못한다.
 *
 * 여기서 참조를 놓고 포인터를 지워 그 누수를 막는다. 태스크가 이미 죽는
 * 중이므로 지연을 실제로 부과할 필요는 없다.
 *
 * 실행 컨텍스트: 태스크 종료 경로(do_exit). 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   do_exit → cgroup_exit → io_cgrp_subsys.exit == [blkcg_exit]
 *     → put_disk
 */
static void blkcg_exit(struct task_struct *tsk)
{
	/* [한국어] 스로틀 예약이 걸려 있었다면 그 디스크 참조를 반납한다.
	 * NULL이면 스로틀 대상이 아니었으므로 할 일이 없다. */
	if (tsk->throttle_disk)
		put_disk(tsk->throttle_disk);
	/* [한국어] 포인터를 지운다. 태스크 구조체가 재사용될 수 있으므로
	 * 해제된 주소를 남기면 안 된다. */
	tsk->throttle_disk = NULL;
}

/*
 * [한국어] blk-cgroup을 cgroup 코어에 등록하는 서브시스템 기술자.
 * cgroup 코어가 cgroup 생성/삭제/태스크 이동 시 여기 등록된 콜백을 호출한다.
 * 이 구조체가 곧 "cgroup의 io 컨트롤러"의 정의다.
 */
struct cgroup_subsys io_cgrp_subsys = {
	/* [한국어] cgroup 디렉터리가 만들어질 때 struct blkcg를 할당한다.
	 * 이 시점에는 아직 어떤 디스크와도 연결되지 않는다 — blkg는 실제로
	 * 그 cgroup의 I/O가 발생하거나 설정이 걸릴 때 만들어진다. */
	.css_alloc = blkcg_css_alloc,
	/* [한국어] 할당 후 cgroup을 실제로 사용 가능하게 만드는 단계.
	 * 정책별 cpd(cgroup policy data) 초기화가 여기서 이뤄진다. */
	.css_online = blkcg_css_online,
	/* [한국어] cgroup 디렉터리가 지워질 때 호출. 다만 online_pin 참조가
	 * 남아 있으면(cgroup writeback이 더티 페이지를 들고 있는 등) 실제
	 * 정리는 그 참조가 풀릴 때까지 미뤄진다(blkcg_pin_online 참고). */
	.css_offline = blkcg_css_offline,
	/* [한국어] 모든 참조가 사라진 뒤 struct blkcg를 해제한다. */
	.css_free = blkcg_css_free,
	/* [한국어] ★ 통계 집계의 핵심 ★
	 * cgroup의 rstat 인프라가 "이 cgroup의 per-CPU 통계를 상위로 올려라"고
	 * 요청할 때 호출된다. I/O 완료 경로는 per-CPU 카운터만 갱신하고(락 없음),
	 * 실제 트리 합산은 io.stat을 읽을 때 이 콜백으로 지연 수행된다.
	 * 이 분리 덕분에 완료 경로가 cgroup 트리 락 경합에서 자유롭다. */
	.css_rstat_flush = blkcg_rstat_flush,
	/* [한국어] cgroup v2에서 노출할 파일 목록(io.stat, io.max, io.weight 등). */
	.dfl_cftypes = blkcg_files,
	/* [한국어] cgroup v1에서 노출할 파일 목록. v1은 blkio.* 이름을 쓴다. */
	.legacy_cftypes = blkcg_legacy_files,
	/* [한국어] v1에서의 서브시스템 이름. v2에서는 "io"지만 v1 호환을 위해
	 * "blkio"라는 옛 이름을 유지한다. */
	.legacy_name = "blkio",
	/* [한국어] 태스크 종료 시 스로틀 예약 정리(위 blkcg_exit). */
	.exit = blkcg_exit,
#ifdef CONFIG_MEMCG
	/*
	 * This ensures that, if available, memcg is automatically enabled
	 * together on the default hierarchy so that the owner cgroup can
	 * be retrieved from writeback pages.
	 */
	.depends_on = 1 << memory_cgrp_id,
#endif
};
EXPORT_SYMBOL_GPL(io_cgrp_subsys);

/**
 * blkcg_activate_policy - activate a blkcg policy on a gendisk
 * @disk: gendisk of interest
 * @pol: blkcg policy to activate
 *
 * Activate @pol on @disk.  Requires %GFP_KERNEL context.  @disk goes through
 * bypass mode to populate its blkgs with policy_data for @pol.
 *
 * Activation happens with @disk bypassed, so nobody would be accessing blkgs
 * from IO path.  Update of each blkg is protected by both queue and blkcg
 * locks so that holding either lock and testing blkcg_policy_enabled() is
 * always enough for dereferencing policy data.
 *
 * The caller is responsible for synchronizing [de]activations and policy
 * [un]registerations.  Returns 0 on success, -errno on failure.
 */
/*
 * [한국어]
 * blkcg_activate_policy - 한 디스크의 모든 blkg 에 정책 데이터(pd)를 붙인다
 *
 * @disk: 정책을 켤 대상 디스크.
 * @pol:  켤 정책. 이미 blkcg_policy_register() 로 plid 를 배정받은 상태여야 한다.
 * @return: 0 성공, -EINVAL(콜백 누락), -ENOMEM(할당 실패).
 *
 * === 정책 등록 구조에서 이 함수의 위치 ===
 *   blkcg_policy_register(pol)   : 전역 등록. pol->plid 배정. (시스템에 1회)
 *   blkcg_activate_policy(disk,pol): 이 디스크의 blkg 들에 pd 부착. (디스크마다)
 * 즉 plid 는 "정책의 전역 번호" 이고, 그 번호가 곧 blkg->pd[] 의 슬롯 번호다.
 * 이 함수가 끝나면 q->blkcg_pols 의 plid 비트가 서고, 그때부터
 * blkcg_policy_enabled(q, pol) 이 참이 되어 정책이 blkg->pd[plid] 를 쓸 수 있다.
 *
 * === 왜 큐를 freeze 하는가 ===
 * 위 영문 주석대로, freeze 로 IO 경로가 blkg 에 접근하지 못하게 만든 상태에서
 * pd 를 채운다. 그래야 "pd 가 절반만 채워진 blkg" 를 IO 경로가 보지 않는다.
 * 각 blkg 갱신은 queue_lock 과 blkcg->lock 두 개로 보호되므로, 이후 어느 한쪽
 * 락만 쥐고 blkcg_policy_enabled() 를 확인하면 pd 역참조가 안전하다.
 *
 * === GFP_NOWAIT → GFP_KERNEL 재시도 패턴 ===
 * blkg 목록 순회는 스핀락 안에서 해야 하므로 우선 GFP_NOWAIT 로 시도한다.
 * 실패하면 그 blkg 의 참조를 잡아 두고(pinned_blkg) 락을 푼 뒤 GFP_KERNEL 로
 * 미리 할당(pd_prealloc)해 두고 retry 라벨로 되돌아간다. 다시 순회하다가
 * 그 blkg 를 만나면 준비해 둔 pd 를 쓴다. 참조를 잡는 이유는 락을 놓은 사이
 * 그 blkg 가 사라질 수 있기 때문이다.
 *
 * 실행 컨텍스트: GFP_KERNEL 문맥(프로세스 컨텍스트, 잠들 수 있음).
 * 호출자가 활성/비활성과 정책 등록/해제 사이의 직렬화를 책임진다(영문 주석).
 *
 * 호출 체인:
 *   blk_throtl_init() / iocost·iolatency 초기화 / elevator 전환
 *     → [blkcg_activate_policy] → pol->pd_alloc_fn/pd_init_fn/pd_online_fn
 */
int blkcg_activate_policy(struct gendisk *disk, const struct blkcg_policy *pol)
{
	/* [한국어] blkg 들이 매달린 큐. */
	struct request_queue *q = disk->queue;
	/* [한국어] 락 밖에서 GFP_KERNEL 로 미리 만들어 둔 pd. NULL 이면 없음. */
	struct blkg_policy_data *pd_prealloc = NULL;
	/* [한국어] blkg 는 순회 커서, pinned_blkg 는 "이 blkg 용으로 prealloc 했다" 는
	 * 표시이자 그 blkg 가 사라지지 않게 잡아 둔 참조. */
	struct blkcg_gq *blkg, *pinned_blkg = NULL;
	/* [한국어] freeze 가 돌려주는 memalloc 범위 상태. unfreeze 에 그대로 넘긴다. */
	unsigned int memflags;
	/* [한국어] 최종 반환값. */
	int ret;

	/* [한국어] 이미 이 디스크에서 켜져 있으면 할 일이 없다(멱등). */
	if (blkcg_policy_enabled(q, pol))
		return 0;

	/*
	 * Policy is allowed to be registered without pd_alloc_fn/pd_free_fn,
	 * for example, ioprio. Such policy will work on blkcg level, not disk
	 * level, and don't need to be activated.
	 */
	/* [한국어] 위 영문 주석대로 blk-ioprio 처럼 디스크 단위 pd 가 필요 없는 정책도
	 * 등록될 수 있다. 그런 정책은 애초에 이 함수를 부르면 안 되므로,
	 * alloc/free 콜백이 없는데 활성화를 시도하면 논리 오류로 잡는다.
	 * 둘 중 하나만 있는 경우도 누수/이중 해제를 부르므로 함께 검사한다. */
	if (WARN_ON_ONCE(!pol->pd_alloc_fn || !pol->pd_free_fn))
		return -EINVAL;

	/* [한국어] blk-mq 큐라면 freeze 해서 IO 경로가 blkg 를 만지지 못하게 한다.
	 * (bio 기반 드라이버 등 queue_is_mq 가 아닌 큐는 freeze 대상이 아니다.) */
	if (queue_is_mq(q))
		memflags = blk_mq_freeze_queue(q);
retry:
	/* [한국어] blkg_list 순회와 pd 부착은 queue_lock 아래에서. */
	spin_lock_irq(&q->queue_lock);

	/* blkg_list is pushed at the head, reverse walk to initialize parents first */
	/* [한국어] blkg_create() 가 list_add(head)로 앞에 넣으므로, 리스트 앞쪽이
	 * 나중에 만들어진(= 더 깊은 자손) blkg 다. 역순으로 돌면 부모가 먼저
	 * 처리되어, 정책 init 콜백이 부모 pd 를 참조할 수 있게 된다. */
	list_for_each_entry_reverse(blkg, &q->blkg_list, q_node) {
		/* [한국어] 이 blkg 에 붙일 정책 데이터. */
		struct blkg_policy_data *pd;

		/* [한국어] retry 로 되돌아온 경우 이미 처리한 blkg 가 있으므로 건너뛴다. */
		if (blkg->pd[pol->plid])
			continue;

		/* If prealloc matches, use it; otherwise try GFP_NOWAIT */
		/* [한국어] 직전 회차에 이 blkg 때문에 락을 풀고 준비해 둔 pd 가 있으면 그것을 쓴다. */
		if (blkg == pinned_blkg) {
			pd = pd_prealloc;
			/* [한국어] 소유권을 넘겼으므로 표식을 비운다(아래 out 에서 이중 해제 방지). */
			pd_prealloc = NULL;
		} else {
			/* [한국어] 스핀락 안이라 잠들 수 없다. 실패해도 되는 빠른 할당을 시도. */
			pd = pol->pd_alloc_fn(disk, blkg->blkcg,
					      GFP_NOWAIT);
		}

		/* [한국어] 빠른 할당이 실패했다 — 락을 풀고 제대로 할당해 오는 경로. */
		if (!pd) {
			/*
			 * GFP_NOWAIT failed.  Free the existing one and
			 * prealloc for @blkg w/ GFP_KERNEL.
			 */
			/* [한국어] 이전 회차에서 잡아 둔 다른 blkg 의 참조를 먼저 놓는다.
			 * 한 번에 하나만 pin 한다는 규칙을 유지하기 위함. */
			if (pinned_blkg)
				blkg_put(pinned_blkg);
			/* [한국어] 락을 놓는 동안 이 blkg 가 해제되지 않도록 참조를 잡는다. */
			blkg_get(blkg);
			pinned_blkg = blkg;

			/* [한국어] 이제 잠들 수 있는 할당을 하기 위해 락을 놓는다.
			 * 이 순간 blkg_list 가 바뀔 수 있어 아래에서 retry 로 처음부터 다시 돈다. */
			spin_unlock_irq(&q->queue_lock);

			/* [한국어] 쓰이지 못하고 남아 있던 prealloc 이 있으면 버린다
			 * (다른 blkg 용으로 만든 것이라 재사용할 수 없다). */
			if (pd_prealloc)
				pol->pd_free_fn(pd_prealloc);
			/* [한국어] 이번엔 GFP_KERNEL 로 확실하게 할당한다. */
			pd_prealloc = pol->pd_alloc_fn(disk, blkg->blkcg,
						       GFP_KERNEL);
			/* [한국어] 성공하면 락을 다시 잡고 처음부터 순회한다.
			 * 이미 pd 가 붙은 blkg 는 위의 continue 로 걸러진다. */
			if (pd_prealloc)
				goto retry;
			else
				/* [한국어] GFP_KERNEL 로도 실패 — 전부 되돌린다. */
				goto enomem;
		}

		/* [한국어] pd 를 blkg 에 붙이는 동안 blkcg 쪽 자료구조와도 배타적이어야 한다.
		 * 잠금 순서는 여기서도 queue_lock → blkcg->lock. */
		spin_lock(&blkg->blkcg->lock);

		/* [한국어] pd → blkg 역참조. */
		pd->blkg = blkg;
		/* [한국어] 자기 슬롯 번호 기록. */
		pd->plid = pol->plid;
		/* [한국어] 슬롯에 꽂는다. 이 대입 이후 정책 코드가 pd 를 볼 수 있다. */
		blkg->pd[pol->plid] = pd;

		/* [한국어] 정책이 기본값을 세팅할 기회(선택 콜백). */
		if (pol->pd_init_fn)
			pol->pd_init_fn(pd);

		/* [한국어] 정책이 실제 동작을 시작할 준비를 하는 단계(선택 콜백). */
		if (pol->pd_online_fn)
			pol->pd_online_fn(pd);
		/* [한국어] 사용 가능 표시. blkg_destroy() 가 이 플래그를 보고 offline 을 부른다. */
		pd->online = true;

		spin_unlock(&blkg->blkcg->lock);
	}

	/* [한국어] 모든 blkg 에 pd 가 붙었으므로 이 큐에서 정책을 "켜짐" 으로 표시한다.
	 * 이 비트가 서야 blkcg_policy_enabled() 가 참이 되고, 이후 새로 만들어지는
	 * blkg 도 blkg_alloc() 에서 이 정책의 pd 를 함께 할당받는다.
	 * __set_bit 은 비원자적 버전이며, queue_lock 이 배타성을 보장한다. */
	__set_bit(pol->plid, q->blkcg_pols);
	ret = 0;

	spin_unlock_irq(&q->queue_lock);
out:
	/* [한국어] freeze 했다면 반드시 짝을 맞춰 푼다. */
	if (queue_is_mq(q))
		blk_mq_unfreeze_queue(q, memflags);
	/* [한국어] 마지막까지 잡고 있던 blkg 참조 반납. */
	if (pinned_blkg)
		blkg_put(pinned_blkg);
	/* [한국어] 쓰이지 않고 남은 prealloc 이 있으면 해제(성공 경로에서도 남을 수 있다). */
	if (pd_prealloc)
		pol->pd_free_fn(pd_prealloc);
	return ret;

enomem:
	/* alloc failed, take down everything */
	/* [한국어] 부분 활성화 상태를 남기면 안 되므로, 이미 붙인 pd 를 전부 떼어낸다.
	 * q->blkcg_pols 비트는 아직 세우지 않았으므로 따로 지울 필요가 없다. */
	spin_lock_irq(&q->queue_lock);
	/* [한국어] 여기서는 순서가 중요하지 않아 정순으로 훑는다. */
	list_for_each_entry(blkg, &q->blkg_list, q_node) {
		/* [한국어] blkg 마다 소유 cgroup 의 락을 잡아야 한다. */
		struct blkcg *blkcg = blkg->blkcg;
		/* [한국어] 떼어낼 pd. */
		struct blkg_policy_data *pd;

		spin_lock(&blkcg->lock);
		/* [한국어] 이 blkg 에 이 정책의 pd 가 붙어 있는지 확인. */
		pd = blkg->pd[pol->plid];
		if (pd) {
			/* [한국어] online 까지 갔던 pd 만 offline 콜백을 부른다. */
			if (pd->online && pol->pd_offline_fn)
				pol->pd_offline_fn(pd);
			/* [한국어] 플래그를 내리고, */
			pd->online = false;
			/* [한국어] 정책이 할당한 메모리를 반납하고, */
			pol->pd_free_fn(pd);
			/* [한국어] 슬롯을 비운다. 이 순서를 지켜야 이중 해제가 없다. */
			blkg->pd[pol->plid] = NULL;
		}
		spin_unlock(&blkcg->lock);
	}
	spin_unlock_irq(&q->queue_lock);
	/* [한국어] 실패 원인을 기록하고 공통 정리(out)로 합류한다. */
	ret = -ENOMEM;
	goto out;
}
EXPORT_SYMBOL_GPL(blkcg_activate_policy);

/**
 * blkcg_deactivate_policy - deactivate a blkcg policy on a gendisk
 * @disk: gendisk of interest
 * @pol: blkcg policy to deactivate
 *
 * Deactivate @pol on @disk.  Follows the same synchronization rules as
 * blkcg_activate_policy().
 */
/*
 * [한국어]
 * blkcg_deactivate_policy - gendisk 에서 blkcg 정책 비활성화
 *
 * 호출 경로: 정책 제거/queue 종료 -> blkcg_deactivate_policy()
 * 이 큐에서 해당 cgroup 정책을 제거한다. q->blkcg_pols
 *   비트를 클리어하고 모든 blkg 의 pd[] 를 해제하여 이후 IO 경로에서
 *   더 이상 정책을 참조하지 않게 한다.
 */

void blkcg_deactivate_policy(struct gendisk *disk,
			     const struct blkcg_policy *pol)
{
	struct request_queue *q = disk->queue;
	/* [한국어] 정책을 비활성화할 request_queue */
	struct blkcg_gq *blkg;
	/* [한국어] 순회 중인 blkg */
	unsigned int memflags;

	if (!blkcg_policy_enabled(q, pol))
	/* [한국어] 이미 비활성화된 정책은 무시 */
	/* [한국어] deactivate 전 정책 활성화 여부 재확인 */
		return;

	if (queue_is_mq(q))
	/* [한국어] 큐 freeze — pd 를 해제하는 동안 그것을 참조하는 IO 가 없어야 한다 */
		memflags = blk_mq_freeze_queue(q);
		/* [한국어] blk-mq 큐 freeze — 새 진입을 막고 인플라이트가 끝나기를 기다린다 */

	mutex_lock(&q->blkcg_mutex);
	/* [한국어] blkg_free_workfn 과의 동기화 */
	spin_lock_irq(&q->queue_lock);
	/* [한국어] queue_lock 획득 */

	__clear_bit(pol->plid, q->blkcg_pols);
	/* [한국어] queue 의 정책 활성화 비트 클리어 */

	list_for_each_entry(blkg, &q->blkg_list, q_node) {
	/* [한국어] 이 큐의 모든 blkg 에서 해당 정책의 pd 를 해제한다 */
		struct blkcg *blkcg = blkg->blkcg;

		spin_lock(&blkcg->lock);
		/* [한국어] blkcg lock 획득 */
		if (blkg->pd[pol->plid]) {
		/* [한국어] pd 가 할당되어 있으면 해제 */
			if (blkg->pd[pol->plid]->online && pol->pd_offline_fn)
		/* [한국어] online 이면 offline 처리 */
				pol->pd_offline_fn(blkg->pd[pol->plid]);
			pol->pd_free_fn(blkg->pd[pol->plid]);
		/* [한국어] pd 메모리 해제 */
			blkg->pd[pol->plid] = NULL;
		/* [한국어] blkg 에서 pd 를 떼어 낸다 — 이후 IO 경로가 이 정책 데이터를 참조하지 못하게 된다 */
		}
		spin_unlock(&blkcg->lock);
	}

	spin_unlock_irq(&q->queue_lock);
	/* [한국어] queue_lock 해제 */
	mutex_unlock(&q->blkcg_mutex);
	/* [한국어] blkcg_mutex 해제 */

	if (queue_is_mq(q))
	/* [한국어] freeze 해제 — IO 재개 */
		blk_mq_unfreeze_queue(q, memflags);
}
EXPORT_SYMBOL_GPL(blkcg_deactivate_policy);

/*
 * [한국어]
 * blkcg_free_all_cpd - 모든 cgroup에서 이 정책의 cgroup-level 데이터를 해제
 *
 * @pol: 해제 대상 정책
 * @return: 없음
 *
 * blk-cgroup에는 두 종류의 정책 데이터가 있다:
 *   cpd (cgroup policy data) - cgroup마다 하나. 장치와 무관한 설정
 *                              (예: BFQ의 cgroup 가중치 기본값)
 *   pd  (policy data)        - (cgroup × 장치) 조합마다 하나. blkg에 붙는다.
 * 이 함수는 전자를 정리한다.
 *
 * 정책 등록 실패 시의 롤백과 정책 모듈 언로드 시에 호출된다. 모든 blkcg를
 * 순회해야 하므로 blkcg_pol_mutex 보호가 필요하며, 호출자가 이미 잡고 있다.
 *
 * 실행 컨텍스트: 정책 등록/해제 경로(프로세스 컨텍스트),
 * blkcg_pol_mutex 보유 상태.
 *
 * 호출 체인:
 *   blkcg_policy_register(실패 롤백) / blkcg_policy_unregister
 *     → [blkcg_free_all_cpd] → pol->cpd_free_fn
 */
static void blkcg_free_all_cpd(struct blkcg_policy *pol)
{
	struct blkcg *blkcg;
	/* [한국어] 순회 중인 cgroup */

	list_for_each_entry(blkcg, &all_blkcgs, all_blkcgs_node) {
	/* [한국어] 시스템 전체 blkcg 순회 */
		if (blkcg->cpd[pol->plid]) {
		/* [한국어] 해당 정책의 cpd 가 있으면 해제 */
			pol->cpd_free_fn(blkcg->cpd[pol->plid]);
			blkcg->cpd[pol->plid] = NULL;
		}
	}
}

/**
 * blkcg_policy_register - register a blkcg policy
 * @pol: blkcg policy to register
 *
 * Register @pol with blkcg core.  Might sleep and @pol may be modified on
 * successful registration.  Returns 0 on success and -errno on failure.
 */
/*
 * [한국어]
 * blkcg_policy_register - blkcg 정책 전역 등록
 *
 * 호출 경로: policy module init -> blkcg_policy_register()
 * throtl, BFQ, ioprio 등이 이 함수로 등록되며, 기존 모든 blkcg 의 cpd[]
 *   를 할당하고 sysfs cgroup 파일을 추가한다. 이후 각 큐는
 *   blkcg_activate_policy() 로 개별적으로 활성화해야 한다.
 */

int blkcg_policy_register(struct blkcg_policy *pol)
{
	struct blkcg *blkcg;
	/* [한국어] cpd 할당 시 순회 중인 cgroup */
	int i, ret;
	/* [한국어] 정책 슬롯 인덱스와 반환값 */

	/*
	 * Make sure cpd/pd_alloc_fn and cpd/pd_free_fn in pairs, and policy
	 * without pd_alloc_fn/pd_free_fn can't be activated.
	 */
	if ((!pol->cpd_alloc_fn ^ !pol->cpd_free_fn) ||
	/* [한국어] alloc/free 함수 쌍이 맞아야 메모리 누수/부패 방지 */
	    (!pol->pd_alloc_fn ^ !pol->pd_free_fn))
		return -EINVAL;

	mutex_lock(&blkcg_pol_register_mutex);
	/* [한국어] 정책 등록/해제 전역 직렬화 */
	mutex_lock(&blkcg_pol_mutex);
	/* [한국어] 정책 테이블 보호 */

	/* find an empty slot */
	for (i = 0; i < BLKCG_MAX_POLS; i++)
	/* [한국어] 빈 정책 슬롯 탐색 */
		if (!blkcg_policy[i])
			break;
	if (i >= BLKCG_MAX_POLS) {
	/* [한국어] plid 슬롯이 다 찼다 — 동시에 등록 가능한 정책 수(BLKCG_MAX_POLS)에 걸렸다 */
		pr_warn("blkcg_policy_register: BLKCG_MAX_POLS too small\n");
		ret = -ENOSPC;
		goto err_unlock;
	}

	/* register @pol */
	pol->plid = i;
	/* [한국어] 정책 id 할당 */
	blkcg_policy[pol->plid] = pol;
	/* [한국어] 전역 정책 테이블에 등록 */

	/* allocate and install cpd's */
	if (pol->cpd_alloc_fn) {
	/* [한국어] 기존 모든 cgroup 에 cpd 할당 */
		list_for_each_entry(blkcg, &all_blkcgs, all_blkcgs_node) {
		/* [한국어] 모든 cgroup 에 대해 cpd 할당 */
			struct blkcg_policy_data *cpd;

			cpd = pol->cpd_alloc_fn(GFP_KERNEL);
		/* [한국어] per-cgroup 정책 데이터 할당 */
			if (!cpd) {
		/* [한국어] cpd 할당 실패 시 롤백 */
				ret = -ENOMEM;
				goto err_free_cpds;
			}

			blkcg->cpd[pol->plid] = cpd;
		/* [한국어] cgroup 에 cpd 연결 */
			cpd->blkcg = blkcg;
		/* [한국어] cpd 가 역참조할 cgroup 설정 */
			cpd->plid = pol->plid;
		/* [한국어] policy id 설정 */
		}
	}

	mutex_unlock(&blkcg_pol_mutex);

	/* everything is in place, add intf files for the new policy */
	if (pol->dfl_cftypes == pol->legacy_cftypes) {
	/* [한국어] v2/v1 cgroup 파일이 동일하면 하나로 등록 */
		WARN_ON(cgroup_add_cftypes(&io_cgrp_subsys,
					   pol->dfl_cftypes));
	} else {
		WARN_ON(cgroup_add_dfl_cftypes(&io_cgrp_subsys,
		/* [한국어] cgroup v2 인터페이스 파일 추가 */
					       pol->dfl_cftypes));
		WARN_ON(cgroup_add_legacy_cftypes(&io_cgrp_subsys,
		/* [한국어] cgroup v1 파일 추가 */
						  pol->legacy_cftypes));
	}
	mutex_unlock(&blkcg_pol_register_mutex);
	return 0;

err_free_cpds:
	if (pol->cpd_free_fn)
	/* [한국어] 할당된 cpd 전부 해제 */
		blkcg_free_all_cpd(pol);

	blkcg_policy[pol->plid] = NULL;
	/* [한국어] 정책 테이블에서 등록 취소 */
err_unlock:
	mutex_unlock(&blkcg_pol_mutex);
	mutex_unlock(&blkcg_pol_register_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(blkcg_policy_register);

/**
 * blkcg_policy_unregister - unregister a blkcg policy
 * @pol: blkcg policy to unregister
 *
 * Undo blkcg_policy_register(@pol).  Might sleep.
 */
/*
 * [한국어]
 * blkcg_policy_unregister - blkcg 정책 전역 등록 해제
 *
 * 호출 경로: policy module exit -> blkcg_policy_unregister()
 * 해당 정책의 cgroup 인터페이스를 제거하고 plid 슬롯을 반납한다.
 *   blkcg_policy[] 슬롯을 NULL 로 만들고 cpd 를 해제한다.
 */

void blkcg_policy_unregister(struct blkcg_policy *pol)
{
	mutex_lock(&blkcg_pol_register_mutex);
	/* [한국어] 정책 등록/해제 직렬화 */

	if (WARN_ON(blkcg_policy[pol->plid] != pol))
	/* [한국어] 슬롯 불일치 시 방어 */
		goto out_unlock;

	/* kill the intf files first */
	if (pol->dfl_cftypes)
	/* [한국어] cgroup v2 파일 제거 */
		cgroup_rm_cftypes(pol->dfl_cftypes);
	if (pol->legacy_cftypes)
	/* [한국어] cgroup v1 파일 제거 */
		cgroup_rm_cftypes(pol->legacy_cftypes);

	/* remove cpds and unregister */
	mutex_lock(&blkcg_pol_mutex);
	/* [한국어] 정책 테이블 보호 */

	if (pol->cpd_free_fn)
	/* [한국어] 모든 cgroup 의 cpd 해제 */
		blkcg_free_all_cpd(pol);

	blkcg_policy[pol->plid] = NULL;
	/* [한국어] 전역 정책 테이블에서 제거 */

	mutex_unlock(&blkcg_pol_mutex);
out_unlock:
	mutex_unlock(&blkcg_pol_register_mutex);
}
EXPORT_SYMBOL_GPL(blkcg_policy_unregister);

/*
 * Scale the accumulated delay based on how long it has been since we updated
 * the delay.  We only call this when we are adding delay, in case it's been a
 * while since we added delay, and when we are checking to see if we need to
 * delay a task, to account for any delays that may have occurred.
 */
/*
 * [한국어]
 * blkcg_scale_delay - 누적된 cgroup IO 지연을 시간에 따라 감소
 *
 * 호출 경로: blkcg_add_delay() -> blkcg_scale_delay()
 *            blkcg_maybe_throttle_blkg() -> blkcg_scale_delay()
 * 장치의 IO 완료 지연이나 throttle 로 인해 쌓인
 *   delay_nsec 를 1초 단위로 decay 시킨다. queue depth 가 포화 상태일 때
 *   cgroup 별 제출 속도를 조절하는 데 사용된다.
 */

static void blkcg_scale_delay(struct blkcg_gq *blkg, u64 now)
{
	u64 old = atomic64_read(&blkg->delay_start);
	/* [한국어] 현재 delay_start 스냅샷; atomic read */

	/* negative use_delay means no scaling, see blkcg_set_delay() */
	if (atomic_read(&blkg->use_delay) < 0)
	/* [한국어] blkcg_set_delay() 모드에서는 decay 하지 않음 */
		return;

	/*
	 * We only want to scale down every second.  The idea here is that we
	 * want to delay people for min(delay_nsec, NSEC_PER_SEC) in a certain
	 * time window.  We only want to throttle tasks for recent delay that
	 * has occurred, in 1 second time windows since that's the maximum
	 * things can be throttled.  We save the current delay window in
	 * blkg->last_delay so we know what amount is still left to be charged
	 * to the blkg from this point onward.  blkg->last_use keeps track of
	 * the use_delay counter.  The idea is if we're unthrottling the blkg we
	 * are ok with whatever is happening now, and we can take away more of
	 * the accumulated delay as we've already throttled enough that
	 * everybody is happy with their IO latencies.
	 */
	if (time_before64(old + NSEC_PER_SEC, now) &&
	/* [한국어] 1초 이상 지난 지연 예산을 decay; atomic CAS 로 경쟁하는 CPU 중 하나만 갱신 */
	/* [한국어] 1초 이상 지난 지연 예산을 decay */
	    atomic64_try_cmpxchg(&blkg->delay_start, &old, now)) {
		u64 cur = atomic64_read(&blkg->delay_nsec);
		/* [한국어] 현재 누적 지연량; atomic read */
		u64 sub = min_t(u64, blkg->last_delay, now - old);
		/* [한국어] 감소시킬 지연량 산출 */
		int cur_use = atomic_read(&blkg->use_delay);
		/* [한국어] 현재 use_delay 카운터; throttle 활성 여부 */

		/*
		 * We've been unthrottled, subtract a larger chunk of our
		 * accumulated delay.
		 */
		if (cur_use < blkg->last_use)
		/* [한국어] throttle 이 해제되면 더 많은 지연 예산을 감소 */
			sub = max_t(u64, sub, blkg->last_delay >> 1);

		/*
		 * This shouldn't happen, but handle it anyway.  Our delay_nsec
		 * should only ever be growing except here where we subtract out
		 * min(last_delay, 1 second), but lord knows bugs happen and I'd
		 * rather not end up with negative numbers.
		 */
		if (unlikely(cur < sub)) {
		/* [한국어] 음수 방지; 지연 예산 0 으로 클리어 */
			atomic64_set(&blkg->delay_nsec, 0);
			blkg->last_delay = 0;
		} else {
			atomic64_sub(sub, &blkg->delay_nsec);
		/* [한국어] 지연 예산 감소; atomic 연산 */
			blkg->last_delay = cur - sub;
		}
		blkg->last_use = cur_use;
		/* [한국어] last_use 갱신; 다음 decay 계산 기준 */
	}
}

/*
 * This is called when we want to actually walk up the hierarchy and check to
 * see if we need to throttle, and then actually throttle if there is some
 * accumulated delay.  This should only be called upon return to user space so
 * we're not holding some lock that would induce a priority inversion.
 */
/*
 * [한국어]
 * blkcg_maybe_throttle_blkg - blkg 계층을 거슬러 올라가며 태스크 throttle
 *
 * 호출 경로: blkcg_maybe_throttle_current() -> blkcg_maybe_throttle_blkg()
 * IO 지연이 cgroup limit 을 초과하면 사용자 공간
 *   복귀 직전 태스크를 재워 새로운 IO 제출을 줄인다.
 *   clamp 시 최대 250ms 로 제한한다.
 */

static void blkcg_maybe_throttle_blkg(struct blkcg_gq *blkg, bool use_memdelay)
{
	unsigned long pflags;
	/* [한국어] PSI memstall 플래그 */
	bool clamp;
	/* [한국어] delay 를 250ms 로 clamp 할지 여부 */
	u64 now = blk_time_get_ns();
	/* [한국어] 현재 시간; 지연 예산 정규화 기준 */
	u64 exp;
	/* [한국어] 깨어날 시간 */
	u64 delay_nsec = 0;
	/* [한국어] 계층에서 발견한 최대 지연량 */
	int tok;
	/* [한국어] io_schedule_prepare 토큰 */

	while (blkg->parent) {
	/* [한국어] 현재 blkg 에서 root 까지 계층을 따라 최대 지연 탐색 */
		int use_delay = atomic_read(&blkg->use_delay);
		/* [한국어] 이 cgroup/blkg 의 지연 예산 활성화 상태; atomic read */

		if (use_delay) {
		/* [한국어] 이 cgroup/blkg 에 지연 예산이 쌓여 있으면 throttle 검사 */
			u64 this_delay;

			blkcg_scale_delay(blkg, now);
			/* [한국어] 지연 예산을 현재 시간 기준으로 정규화 */
			this_delay = atomic64_read(&blkg->delay_nsec);
			/* [한국어] 정규화된 지연 예산; atomic read */
			if (this_delay > delay_nsec) {
			/* [한국어] 최대 지연 갱신; 양수 use_delay 이면 clamp */
				delay_nsec = this_delay;
				clamp = use_delay > 0;
			}
		}
		blkg = blkg->parent;
		/* [한국어] 참조 획득 실패 시 상위 cgroup 의 blkg 로 fallback */
	}

	if (!delay_nsec)
	/* [한국어] 지연 예산이 없으면 throttle 없음 */
		return;

	/*
	 * Let's not sleep for all eternity if we've amassed a huge delay.
	 * Swapping or metadata IO can accumulate 10's of seconds worth of
	 * delay, and we want userspace to be able to do _something_ so cap the
	 * delays at 0.25s. If there's 10's of seconds worth of delay then the
	 * tasks will be delayed for 0.25 second for every syscall. If
	 * blkcg_set_delay() was used as indicated by negative use_delay, the
	 * caller is responsible for regulating the range.
	 */
	if (clamp)
	/* [한국어] 지나친 지연을 방지하기 위해 최대 250ms 로 clamp */
		delay_nsec = min_t(u64, delay_nsec, 250 * NSEC_PER_MSEC);

	if (use_memdelay)
	/* [한국어] PSI memory delay 기록 */
		psi_memstall_enter(&pflags);

	exp = ktime_add_ns(now, delay_nsec);
	/* [한국어] 깨어날 절대 시간 */
	tok = io_schedule_prepare();
	/* [한국어] IO 스케줄링 준비 */
	do {
		__set_current_state(TASK_KILLABLE);
		/* [한국어] TASK_KILLABLE 로 잔다 — 무한정 throttle 되더라도 SIGKILL 로는 빠져나올 수 있어야 한다 */
		if (!schedule_hrtimeout(&exp, HRTIMER_MODE_ABS))
		/* [한국어] 지정 시간까지 수면; 시간 만료 시 깨어남 */
			break;
	} while (!fatal_signal_pending(current));
	io_schedule_finish(tok);
	/* [한국어] IO 스케줄링 종료 처리 */

	if (use_memdelay)
		psi_memstall_leave(&pflags);
}

/**
 * blkcg_maybe_throttle_current - throttle the current task if it has been marked
 *
 * This is only called if we've been marked with set_notify_resume().  Obviously
 * we can be set_notify_resume() for reasons other than blkcg throttling, so we
 * check to see if current->throttle_disk is set and if not this doesn't do
 * anything.  This should only ever be called by the resume code, it's not meant
 * to be called by people willy-nilly as it will actually do the work to
 * throttle the task if it is setup for throttling.
 */
/*
 * [한국어]
 * blkcg_maybe_throttle_current - 현재 태스크의 blkcg throttle 조건 확인/수행
 *
 * 호출 경로: resume 코드 -> blkcg_maybe_throttle_current()
 * current->throttle_disk 에 저장된 디스크를 찾아 해당
 *   cgroup 의 blkg 를 lookup 한 후 지연을 적용한다. syscall 당 한 번만
 *   throttle 한다.
 */

void blkcg_maybe_throttle_current(void)
{
	struct gendisk *disk = current->throttle_disk;
	/* [한국어] throttle 대상 디스크 */
	struct blkcg *blkcg;
	/* [한국어] 현재 태스크의 cgroup */
	struct blkcg_gq *blkg;
	/* [한국어] throttle 대상 blkg */
	bool use_memdelay = current->use_memdelay;
	/* [한국어] PSI memdelay 사용 여부 */

	if (!disk)
	/* [한국어] throttle 예약이 없으면 무시 */
		return;

	current->throttle_disk = NULL;
	/* [한국어] throttle_disk 클리어; 한 syscall 당 한 번만 throttle */
	current->use_memdelay = false;
	/* [한국어] memdelay 플래그 클리어 */

	rcu_read_lock();
	/* [한국어] blkg_lookup 및 css 접근을 RCU 로 보호 */
	blkcg = css_to_blkcg(blkcg_css());
	/* [한국어] 현재 태스크의 blkcg 획득 */
	if (!blkcg)
	/* [한국어] blkcg 가 없으면 throttle 불가 */
		goto out;
	blkg = blkg_lookup(blkcg, disk->queue);
	/* [한국어] 이 디스크의 blkg 검색 */
	if (!blkg)
	/* [한국어] blkg 가 없으면 throttle 불가 */
		goto out;
	if (!blkg_tryget(blkg))
	/* [한국어] blkg 참조 획득 실패 시 throttle 불가; 제거 중일 수 있음 */
		goto out;
	rcu_read_unlock();
	/* [한국어] blkg_tryget 성공 후 RCU 종료; blkg 참조로 보호 */

	blkcg_maybe_throttle_blkg(blkg, use_memdelay);
	blkg_put(blkg);
	/* [한국어] throttle 완료 후 blkg 참조 반낑 */
	put_disk(disk);
	return;
out:
	rcu_read_unlock();
	put_disk(disk);
}

/**
 * blkcg_schedule_throttle - this task needs to check for throttling
 * @disk: disk to throttle
 * @use_memdelay: do we charge this to memory delay for PSI
 *
 * This is called by the IO controller when we know there's delay accumulated
 * for the blkg for this task.  We do not pass the blkg because there are places
 * we call this that may not have that information, the swapping code for
 * instance will only have a block_device at that point.  This set's the
 * notify_resume for the task to check and see if it requires throttling before
 * returning to user space.
 *
 * We will only schedule once per syscall.  You can call this over and over
 * again and it will only do the check once upon return to user space, and only
 * throttle once.  If the task needs to be throttled again it'll need to be
 * re-set at the next time we see the task.
 */
/*
 * [한국어]
 * blkcg_schedule_throttle - 현재 태스크가 user space 복귀 시 throttle 검사
 *
 * 호출 경로: throtl/bfq 등 -> blkcg_schedule_throttle()
 * IO 지연이 발생했음을 기록해 두고, 태스크가 user
 *   space 로 돌아갈 때 blkcg_maybe_throttle_current() 가 동작하도록
 *   set_notify_resume() 을 설정한다.
 */

void blkcg_schedule_throttle(struct gendisk *disk, bool use_memdelay)
{
	if (unlikely(current->flags & PF_KTHREAD))
	/* [한국어] kthread 는 user space 복귀가 없으므로 throttle 예약 안 함 */
		return;

	if (current->throttle_disk != disk) {
	/* [한국어] 다른 disk 를 가리키고 있거나 처음 설정 */
		if (test_bit(GD_DEAD, &disk->state))
		/* [한국어] 이미 죽은 디스크면 throttle 을 걸어 봐야 깨워 줄 IO 가 없다 */
			return;
		get_device(disk_to_dev(disk));
		/* [한국어] disk 장치 참조 획득 */

		if (current->throttle_disk)
		/* [한국어] 이전 disk 참조 반낑 */
			put_disk(current->throttle_disk);
		current->throttle_disk = disk;
		/* [한국어] throttle 할 disk 설정 */
	}

	if (use_memdelay)
	/* [한국어] memdelay 플래그 설정 */
		current->use_memdelay = use_memdelay;
	set_notify_resume(current);
	/* [한국어] user space 복귀 시 blkcg_maybe_throttle_current() 실행 예약 */
}

/**
 * blkcg_add_delay - add delay to this blkg
 * @blkg: blkg of interest
 * @now: the current time in nanoseconds
 * @delta: how many nanoseconds of delay to add
 *
 * Charge @delta to the blkg's current delay accumulation.  This is used to
 * throttle tasks if an IO controller thinks we need more throttling.
 */
/*
 * [한국어]
 * blkcg_add_delay - blkg 에 delta 만큼의 IO 지연을 누적
 *
 * 호출 경로: throtl/bfq -> blkcg_add_delay()
 * 큐의 지연이 목표를 초과하면 해당 cgroup 의
 *   delay_nsec 에 초과분을 축적한다. 이 값은 blkcg_maybe_throttle_blkg() 에서
 *   태스크 수면 시간으로 변환된다.
 */

void blkcg_add_delay(struct blkcg_gq *blkg, u64 now, u64 delta)
{
	if (WARN_ON_ONCE(atomic_read(&blkg->use_delay) < 0))
	/* [한국어] set_delay 모드와 혼용되면 안 되는 경고 */
		return;
	blkcg_scale_delay(blkg, now);
	/* [한국어] 먼저 지연 예산을 시간에 따라 정규화 */
	atomic64_add(delta, &blkg->delay_nsec);
	/* [한국어] delta 를 원자적으로 누적한다 — 여러 CPU 가 같은 blkg 에 동시에 기록한다 */
}

/**
 * blkg_tryget_closest - try and get a blkg ref on the closet blkg
 * @bio: target bio
 * @css: target css
 *
 * As the failure mode here is to walk up the blkg tree, this ensure that the
 * blkg->parent pointers are always valid.  This returns the blkg that it ended
 * up taking a reference on or %NULL if no reference was taken.
 */
/*
 * [한국어]
 * blkg_tryget_closest - 가장 가까운 살아있는 blkg 에 대한 참조 획득 시도
 *
 * 호출 경로: bio_associate_blkg_from_css() -> blkg_tryget_closest()
 * cgroup 이 소멸 중이면 그 IO 는 상위(부모) blkg 로 spill
 *   된다. blkg->parent 체인을 따라 올라가며 유효한 참조를 얻어 IO 완료까지
 *   blkg 가 유지되도록 한다.
 */

static inline struct blkcg_gq *blkg_tryget_closest(struct bio *bio,
		struct cgroup_subsys_state *css)
{
	struct blkcg_gq *blkg, *ret_blkg = NULL;
	/* [한국어] 검색 중인 blkg 와 결과 */

	rcu_read_lock();
	/* [한국어] blkg_lookup_create 및 parent 체인 접근 보호 */
	blkg = blkg_lookup_create(css_to_blkcg(css), bio->bi_bdev->bd_disk);
	/* [한국어] bio 가 향하는 디스크에 대한 blkg 검색/생성 */
	while (blkg) {
	/* [한국어] blkg->parent 체인을 따라 올라가며 살아있는 blkg 탐색 */
		if (blkg_tryget(blkg)) {
		/* [한국어] 참조 획득 성공; IO 수명 동안 blkg 유지 */
			ret_blkg = blkg;
			break;
		}
		blkg = blkg->parent;
	/* [한국어] 참조 획득 실패 시 상위 cgroup 의 blkg 로 fallback */
	}
	rcu_read_unlock();

	return ret_blkg;	/* [한국어] 요청한 그 blkg 이거나, 참조를 잡지 못해 거슬러 올라간 조상 blkg 다.
				 * 결코 NULL 이 아니다 — 최악의 경우 루트 blkg 가 반환되므로,
				 * 호출자는 NULL 검사 없이 곧장 이 포인터로 회계를 이어 갈 수 있다.
				 * 정확도를 조금 희생하고 IO 경로에 분기 하나를 없앤 절충이다. */
}

/**
 * bio_associate_blkg_from_css - associate a bio with a specified css
 * @bio: target bio
 * @css: target css
 *
 * Associate @bio with the blkg found by combining the css's blkg and the
 * request_queue of the @bio.  An association failure is handled by walking up
 * the blkg tree.  Therefore, the blkg associated can be anything between @blkg
 * and q->root_blkg.  This situation only happens when a cgroup is dying and
 * then the remaining bios will spill to the closest alive blkg.
 *
 * A reference will be taken on the blkg and will be released when @bio is
 * freed.
 */
/*
 * [한국어]
 * bio_associate_blkg_from_css - bio 를 지정한 css 의 blkg 에 연결
 *
 * 호출 경로: bio_associate_blkg() -> bio_associate_blkg_from_css()
 *            bio_clone_blkg_association() -> bio_associate_blkg_from_css()
 * bio->bi_blkg 를 설정한다. 이 포인터가 이후 rq-qos(throttle/iolatency/iocost)와
 *   통계 집계에서 "이 IO 는 누구 것인가"를 판정하는 근거가 된다. 귀속을
 *   고정한다. root cgroup 이면 q->root_blkg 를 사용한다.
 */

void bio_associate_blkg_from_css(struct bio *bio,
				 struct cgroup_subsys_state *css)
{
	if (bio->bi_blkg)
		/* [한국어] 기존 blkg 참조 반낑 후 재연결 */
	/* [한국어] 기존 blkg 가 있으면 참조 해제 후 재연결 */
		blkg_put(bio->bi_blkg);

	if (css && css->parent) {
	/* [한국어] root 가 아닌 cgroup 이면 가장 가까운 blkg 검색 */
		bio->bi_blkg = blkg_tryget_closest(bio, css);
	} else {
		blkg_get(bdev_get_queue(bio->bi_bdev)->root_blkg);
		/* [한국어] root cgroup 이면 그 큐의 root_blkg 를 그대로 쓴다 */
		bio->bi_blkg = bdev_get_queue(bio->bi_bdev)->root_blkg;
		/* [한국어] root_blkg 를 bio->bi_blkg 에 설정한다 (이 값은 큐 선택과 무관하며, 이후 throttle·통계의 귀속 대상만 정한다) */
	}
}
EXPORT_SYMBOL_GPL(bio_associate_blkg_from_css);

/**
 * bio_associate_blkg - associate a bio with a blkg
 * @bio: target bio
 *
 * Associate @bio with the blkg found from the bio's css and request_queue.
 * If one is not found, bio_lookup_blkg() creates the blkg.  If a blkg is
 * already associated, the css is reused and association redone as the
 * request_queue may have changed.
 */
/*
 * [한국어]
 * bio_associate_blkg - bio 의 cgroup 에 맞는 blkg 를 찾아 연결
 *
 * 호출 경로: submit_bio() -> bio_associate_blkg()
 * IO 제출의 시작점에서 이 bio 가 어느 cgroup 소유인지 확정한다.
 *   passthrough IO 는 제외한다. 이 함수 이후 bio 는
 *   submit_bio -> bio_associate_blkg -> blk_mq_submit_bio ->
 *   blk_mq_get_request -> mq_ops->queue_rq (간접 호출; NVMe PCIe 면 nvme_queue_rq -> nvme_sq_copy_cmd -> nvme_write_sq_db) 의
 *   경로를 타게 된다.
 */

void bio_associate_blkg(struct bio *bio)
{
	struct cgroup_subsys_state *css;
	/* [한국어] bio 의 cgroup css */

	if (blk_op_is_passthrough(bio->bi_opf))
	/* [한국어] passthrough/admin 명령은 cgroup 연결 제외 */
		return;

	rcu_read_lock();
	/* [한국어] blkcg_css() 및 blkg 연결의 RCU 보호 */

	if (bio->bi_blkg)
	/* [한국어] 기존 blkg 의 css 재사용 */
		css = bio_blkcg_css(bio);
	else
		css = blkcg_css();
	/* [한국어] 현재 태스크의 cgroup css 획득 */

	bio_associate_blkg_from_css(bio, css);
	/* [한국어] css 에 맞는 blkg 를 bio 에 연결한다 — 이후 rq-qos 와 통계가 이 포인터로 귀속을 판단한다 */

	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(bio_associate_blkg);

/**
 * bio_clone_blkg_association - clone blkg association from src to dst bio
 * @dst: destination bio
 * @src: source bio
 */
/*
 * [한국어]
 * bio_clone_blkg_association - src bio 의 blkg 연결을 dst bio 로 복제
 *
 * 호출 경로: bio_clone_* -> bio_clone_blkg_association()
 * split/clone 된 bio 가 원본과 동일한 cgroup 귀속을
 *   유지하도록 한다. CID/SQ 에 기록될 때 동일한 cgroup 정책이 적용된다.
 */

void bio_clone_blkg_association(struct bio *dst, struct bio *src)
{
	if (src->bi_blkg)
	/* [한국어] src bio 에 blkg 이 있을 때만 복제 */
		bio_associate_blkg_from_css(dst, bio_blkcg_css(src));
	/* [한국어] dst bio 에 동일한 cgroup css 적용; CID/SQ 기록 시 동일한 cgroup 정책 적용 */
}
EXPORT_SYMBOL_GPL(bio_clone_blkg_association);

/*
 * [한국어]
 * blk_cgroup_io_type - bio 를 read/write/discard 로 분류
 *
 * 호출 경로: blk_cgroup_bio_start() -> blk_cgroup_io_type()
 * bio 의 연산 종류(bi_opf)를 read/write/discard 통계 인덱스로 매핑한다.
 *   이 분류는 블록 계층 통계용이며 장치 프로토콜과 무관하다.
 *   op 코드만 본다.
 */

static int blk_cgroup_io_type(struct bio *bio)
{
	if (op_is_discard(bio->bi_opf))
	/* [한국어] discard/flush 등은 BLKG_IOSTAT_DISCARD 로 분류 */
		return BLKG_IOSTAT_DISCARD;
	if (op_is_write(bio->bi_opf))
	/* [한국어] write 관련 opcode 를 BLKG_IOSTAT_WRITE 로 분류 */
		return BLKG_IOSTAT_WRITE;
	/* [한국어] 나머지는 전부 read 로 분류한다 */
	return BLKG_IOSTAT_READ;
}

/*
 * [한국어]
 * blk_cgroup_bio_start - bio 의 cgroup IO 통계 및 상태 갱신
 *
 * 호출 경로: block layer IO 시작/완료 지점 -> blk_cgroup_bio_start()
 *            (rq_qos 또는 blk_account 경로를 통해 호출됨, 추정)
 * IO 가 서비스되거나 완료될 때 bio->bi_iter.bi_size 와
 *   ios[BLKG_IOSTAT_*] 를 per-cpu blkg_iostat_set 에 누적한다. BIO_CGROUP_ACCT
 *   플래그로 split bio 의 중복 집계를 방지하고, lockless list(lhead)에 등록해
 *   rstat flush 시점에 global 통계로 반영한다.
 */

void blk_cgroup_bio_start(struct bio *bio)
{
	struct blkcg *blkcg = bio->bi_blkg->blkcg;
	/* [한국어] bio 가 속한 cgroup */
	int rwd = blk_cgroup_io_type(bio), cpu;
	/* [한국어] IO 유형(read/write/discard)과 현재 CPU */
	struct blkg_iostat_set *bis;
	/* [한국어] per-cpu 통계 영역 포인터 */
	unsigned long flags;

	if (!cgroup_subsys_on_dfl(io_cgrp_subsys))
	/* [한국어] v1(hierarchy=legacy) cgroup 은 여기서 통계 집계 안 함 */
		return;

	/* Root-level stats are sourced from system-wide IO stats */
	if (!cgroup_parent(blkcg->css.cgroup))
	/* [한국어] root cgroup 통계는 시스템 전체 disk_stats 로 대체 */
		return;

	cpu = get_cpu();
	/* [한국어] 현재 CPU 의 per-cpu 통계 영역 사용; preempt disable 상태 */
	/* [한국어] 현재 CPU 의 per-cpu 통계 영역 사용 */
	bis = per_cpu_ptr(bio->bi_blkg->iostat_cpu, cpu);
	/* [한국어] bio 가 속한 blkg 의 per-cpu iostat_set 획득 */
	flags = u64_stats_update_begin_irqsave(&bis->sync);
	/* [한국어] per-cpu 통계 seqlock 진입. 완료 경로가 인터럽트 문맥에서 같은 카운터를 만질 수 있어 irqsave 로 막는다 */

	/*
	 * If the bio is flagged with BIO_CGROUP_ACCT it means this is a split
	 * bio and we would have already accounted for the size of the bio.
	 */
	if (!bio_flagged(bio, BIO_CGROUP_ACCT)) {
	/* [한국어] split bio 는 이미 크기를 집계했으므로 중복 방지 */
		bio_set_flag(bio, BIO_CGROUP_ACCT);
		/* [한국어] 중복 집계 방지 플래그 설정 */
		bis->cur.bytes[rwd] += bio->bi_iter.bi_size;
		/* [한국어] bio 크기(바이트)를 read/write/discard 별로 누적한다 */
		/* [한국어] bio 크기(바이트)를 read/write/discard 별로 누적 */
	}
	bis->cur.ios[rwd]++;
	/* [한국어] read/write/discard IO 건수 증가. 세는 단위는 bio 이지 장치 명령이 아니다 — 병합되면 여러 bio 가 명령 하나가 된다 */
	/* [한국어] read/write/discard IO 횟수 증가 */

	/*
	 * If the iostat_cpu isn't in a lockless list, put it into the
	 * list to indicate that a stat update is pending.
	 */
	if (!READ_ONCE(bis->lqueued)) {
	/* [한국어] 아직 lockless list 에 없으면 flush 대기열에 등록 */
		struct llist_head *lhead = this_cpu_ptr(blkcg->lhead);
		/* [한국어] 현재 CPU 의 cgroup lockless list 헤드 */

		llist_add(&bis->lnode, lhead);
		/* [한국어] per-cpu 통계 노드를 cgroup 의 lockless list 에 추가; 이후 rstat flush 시 global 로 반영 */
		/* [한국어] per-cpu 통계 노드를 cgroup 의 lockless list 에 추가 */
		WRITE_ONCE(bis->lqueued, true);
		/* [한국어] list 등록 상태를 배리어와 함께 기록; __blkcg_rstat_flush 의 llist_del_all 과 동기화 */
		/* [한국어] list 등록 상태를 배리어와 함께 기록 */
	}

	u64_stats_update_end_irqrestore(&bis->sync, flags);
	/* [한국어] per-cpu 통계 seqlock 해제 */
	css_rstat_updated(&blkcg->css, cpu);
	/* [한국어] cgroup rstat framework 에 통계 갱신 알림; lazy flush 트리거 */
	/* [한국어] cgroup rstat framework 에 통계 갱신 알림 */
	put_cpu();
	/* [한국어] preempt enable 복원 */
}

/*
 * [한국어]
 * blk_cgroup_congested - 현재 cgroup 계층에 IO 혼잡이 있는지 확인
 *
 * 호출 경로: writeback/congestion 판단 -> blk_cgroup_congested()
 * cgroup 의 congestion_count 가 0보다 크면 그 cgroup 이
 *   지연/스로틀 상태임을 나타낸다. writeback 등에서 추가 IO 제출을 억제하는
 *   데 활용된다.
 */

bool blk_cgroup_congested(void)
{
	struct blkcg *blkcg;
	/* [한국어] 현재 태스크의 cgroup */
	bool ret = false;
	/* [한국어] 혼잡 상태 반환값 */

	rcu_read_lock();
	/* [한국어] blkcg 계층 탐색의 RCU 보호 */
	for (blkcg = css_to_blkcg(blkcg_css()); blkcg;
	/* [한국어] 현재 cgroup 에서 root 까지 계층 순회 */
	     blkcg = blkcg_parent(blkcg)) {
		if (atomic_read(&blkcg->congestion_count)) {
		/* [한국어] congestion_count > 0 이면 이 cgroup 이 현재 스로틀되고 있다는 뜻 */
			ret = true;
			break;
		}
	}
	rcu_read_unlock();
	/* [한국어] RCU read-side 종료 */
	return ret;
}

module_param(blkcg_debug_stats, bool, 0644);
	/* [한국어] debug 통계 활성화 모듈 파라미터 */
MODULE_PARM_DESC(blkcg_debug_stats, "True if you want debug stats, false if not");

/* [한국어] 핵심 요약 — 그리고 NVMe 독자를 위한 경계 긋기
 *
 * 이 파일은 장치를 전혀 모른다. 파일 상단에도 적었듯 코드에 nvme 식별자가
 * 하나도 없고, 여기서 일어나는 모든 일은 NVMe·SATA·loop 에 똑같이 적용된다.
 * 그래서 아래 요약은 "NVMe 에서 이 파일이 무엇을 하는가"가 아니라
 * "이 파일이 하는 일과 NVMe 가 만나는 지점은 어디까지인가"로 적는다.
 *
 * - 이 파일이 하는 일: (blkcg, request_queue) 쌍마다 blkg 를 하나 두고,
 *   bio 가 들어올 때 bio->bi_blkg 를 채워 "이 IO 는 누구 것인가"를 확정한다.
 *   그 위에 정책(throtl / iocost / iolatency / bfq)이 blkg->pd[] 로 얹혀
 *   각자의 제한과 회계를 수행한다.
 *
 * - 시점이 중요하다. 이 파일과 그 위의 정책들은 **요청이 드라이버로 내려가기
 *   한참 전**, bio 단계에서 작동한다. mq_ops->queue_rq(NVMe 면 nvme_queue_rq)에
 *   닿을 무렵이면 throttle 판정은 이미 끝나 있다. cgroup 정책이
 *   "어느 하드웨어 큐를 쓸지"를 고르는 일은 **없다** — 큐 선택은 제출 CPU 와
 *   blk_mq_map_queue() 가 정하며 cgroup 과 무관하다.
 *
 * - 통계의 단위도 구분해야 한다. 여기서 세는 것은 bio 건수이지 장치 명령
 *   건수가 아니다. 병합되면 여러 bio 가 명령 하나가 되고, 분할되면 그 반대다.
 *   따라서 cgroup 의 ios 값과 NVMe 컨트롤러가 본 명령 수는 일치하지 않는다.
 *
 * - per-cpu blkg_iostat_set + lockless list(lhead) 구조는 완료가 여러 CPU 에서
 *   동시에 쏟아지는 고속 장치에서 카운터 한 줄을 두고 다투지 않기 위한 것이다.
 *   NVMe 처럼 큐가 CPU 수만큼 있는 장치에서 이 설계의 이득이 가장 크다 —
 *   이것이 이 파일에서 NVMe 와 가장 가까운 접점이다.
 *
 * - 관련 파일: 정책 구현은 blk-throttle.c / blk-iocost.c / blk-iolatency.c /
 *   bfq-cgroup.c 에 있고, 이 파일은 그것들이 공유하는 뼈대(blkg 트리, pd 슬롯,
 *   등록/활성화 절차)만 제공한다.
 */
