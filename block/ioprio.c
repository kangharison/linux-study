// SPDX-License-Identifier: GPL-2.0
/*
 * fs/ioprio.c
 *
 * Copyright (C) 2004 Jens Axboe <axboe@kernel.dk>
 *
 * Helper functions for setting/querying io priorities of processes. The
 * system calls closely mimmick getpriority/setpriority, see the man page for
 * those. The prio argument is a composite of prio class and prio data, where
 * the data argument has meaning within that class. The standard scheduling
 * classes have 8 distinct prio levels, with 0 being the highest prio and 7
 * being the lowest.
 *
 * IOW, setting BE scheduling class with prio 2 is done ala:
 *
 * unsigned int prio = (IOPRIO_CLASS_BE << IOPRIO_CLASS_SHIFT) | 2;
 *
 * ioprio_set(PRIO_PROCESS, pid, prio);
 *
 * See also Documentation/block/ioprio.rst
 *
 */

/*
 * NVMe 관점 파일 요약:
 * 이 파일은 사용자공간에서 지정한 I/O 우선순위(ioprio)를 프로세스,
 * 프로세스 그룹, 사용자 단위로 커널 태스크의 io_context에 기록하는
 * ioprio_set/ioprio_get 시스템콜 구현체다.
 * 기록된 ioprio는 block/blk-ioc.c의 set_task_ioprio()를 통해
 * struct io_context::ioprio에 반영되며, 이후 bio 제출 경로에서
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 * blk_mq_sched_dispatch -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 * 로 전달되며, NVMe SQ 우선순위 및 스케줄러의 타임슬라이스 배분에
 * 힌트를 제공한다 (추정).
 * 이 파일은 block/blk-ioc.c, include/linux/ioprio.h,
 * Documentation/block/ioprio.rst와 논리적으로 연결된다.
 */
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/ioprio.h>
#include <linux/cred.h>
#include <linux/blkdev.h>
#include <linux/capability.h>
#include <linux/syscalls.h>
#include <linux/security.h>
#include <linux/pid_namespace.h>

/*
 * 이 파일에서 다루는 주요 데이터 구조와 필드 (NVMe 관점):
 *
 * struct task_struct::io_context
 *   - 태스크가 NVMe 장치로 보내는 모든 I/O가 참조하는 io_context를
 *     가리킨다. ioprio_set()은 이 필드가 없으면 block/blk-ioc.c의
 *     alloc_io_context()로 새로 할당한다.
 *
 * struct io_context::ioprio
 *   - 사용자가 지정한 I/O 우선순위가 저장된다. NVMe 스택에서는 이 값을
 *     바탕으로 request 구성 시 우선순위 힌트를 얻으며, BFQ 등의
 *     스케줄러가 SQ 선택이나 타임슬라이스에 반영할 수 있다 (추정).
 *
 * struct pid / struct user_struct
 *   - ioprio_set/get의 which 인자(PROCESS/PGRP/USER)에 따라 탐색 대상을
 *     식별하며, NVMe 입장에서는 동일한 ioprio를 공유할 태스크 집합을
 *     결정하는 메타데이터 역할을 한다.
 */

/*
 * ioprio_check_cap - 사용자가 지정한 ioprio의 클래스와 레벨을 검증하고
 *                    필요한 capability를 확인한다.
 *
 * 호출 경로:
 *   sys_ioprio_set -> SYSCALL_DEFINE3(ioprio_set) -> ioprio_check_cap
 *
 * NVMe 연결점:
 *   - IOPRIO_CLASS_RT는 NVMe Weighted Round Robin(WRR)의 Urgent/High
 *     우선순위 클래스에 대응될 수 있어서 CAP_SYS_ADMIN/CAP_SYS_NICE
 *     권한이 필요하다 (추정).
 *   - IOPRIO_CLASS_BE는 일반 NVMe I/O의 기본 우선순위이며,
 *     IOPRIO_CLASS_IDLE은 낮은 우선순위로 백그라운드 GC 등에 사용될 수 있다.
 *   - 검증을 통과한 ioprio만이 set_task_ioprio()를 거쳐
 *     task->io_context->ioprio에 기록된다.
 */
int ioprio_check_cap(int ioprio)
{
	int class = IOPRIO_PRIO_CLASS(ioprio);	/* 상위 비트에서 클래스 추출 */
	int level = IOPRIO_PRIO_LEVEL(ioprio);	/* 하위 비트에서 클래스 내부 레벨 추출 */

	switch (class) {
		/* RT 클래스는 NVMe WRR의 High/Urgent 등급에 대응될 수 있어 권한 제한 (추정) */
		case IOPRIO_CLASS_RT:
			/*
			 * Originally this only checked for CAP_SYS_ADMIN,
			 * which was implicitly allowed for pid 0 by security
			 * modules such as SELinux. Make sure we check
			 * CAP_SYS_ADMIN first to avoid a denial/avc for
			 * possibly missing CAP_SYS_NICE permission.
			 */
			if (!capable(CAP_SYS_ADMIN) && !capable(CAP_SYS_NICE))
				return -EPERM;
			break;
		/* BE/IDLE은 일반/백그라운드 NVMe I/O에 해당, 별도 권한 불필요 */
		case IOPRIO_CLASS_BE:
		case IOPRIO_CLASS_IDLE:
			break;
		/* NONE 클래스는 level이 0이어야 함: io_context 미설정 상태 */
		case IOPRIO_CLASS_NONE:
			if (level)
				return -EINVAL;
			break;
		/* 잘못된 클래스: NVMe 경로로 전달되지 않도록 거부 */
		case IOPRIO_CLASS_INVALID:
		default:
			return -EINVAL;
	}

	return 0;
}

/*
 * SYSCALL_DEFINE3(ioprio_set, ...) - ioprio_set 시스템콜 본체
 *
 * 목적:
 *   지정한 프로세스/프로세스 그룹/사용자에게 속한 태스크들의 I/O
 *   우선순위를 설정한다.
 *
 * 호출 경로:
 *   사용자공간 ioprio_set(2) -> SYSCALL_DEFINE3(ioprio_set)
 *       -> ioprio_check_cap
 *       -> set_task_ioprio (block/blk-ioc.c)
 *           -> task->io_context->ioprio 갱신
 *           -> 이후 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *              -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *
 * NVMe 연결점:
 *   - 설정된 ioprio는 NVMe I/O 발행 시 blk-mq/스케줄러가 참조하는
 *     우선순위 힌트가 된다.
 *   - IOPRIO_WHO_USER로 다수 태스크를 일괄 변경하면, 동일 UID가
 *     사용하는 NVMe 큐들의 전반적인 서비스 품질이 변한다.
 *   - who == 0이면 current 태스크 자신의 NVMe I/O 우선순위를 변경한다.
 */
SYSCALL_DEFINE3(ioprio_set, int, which, int, who, int, ioprio)
{
	struct task_struct *p, *g;
	struct user_struct *user;
	struct pid *pgrp;
	kuid_t uid;
	int ret;

	ret = ioprio_check_cap(ioprio);	/* 클래스/레벨과 capability 검사 */
	if (ret)
		return ret;

	ret = -ESRCH;	/* 기본 오류: 대상 태스크를 찾을 수 없음 */
	rcu_read_lock();	/* which에 따라 태스크/pid 탐색 동안 보호 */
	switch (which) {
		case IOPRIO_WHO_PROCESS:
			if (!who)		/* who == 0이면 자기 자신의 우선순위 변경 */
				p = current;
			else
				p = find_task_by_vpid(who);	/* 지정 pid의 태스크 검색 */
			if (p)
				ret = set_task_ioprio(p, ioprio);	/* block/blk-ioc.c에서 io_context에 반영 */
			break;
		case IOPRIO_WHO_PGRP:
			if (!who)
				pgrp = task_pgrp(current);
			else
				pgrp = find_vpid(who);

			read_lock(&tasklist_lock);	/* 프로세스 그룹 스레드 순회 동안 tasklist 보호 */
			do_each_pid_thread(pgrp, PIDTYPE_PGID, p) {
				ret = set_task_ioprio(p, ioprio);	/* 그룹 내 각 태스크에 적용 */
				if (ret) {
					read_unlock(&tasklist_lock);
					goto out;	/* 하나라도 실패하면 즉시 중단 */
				}
			} while_each_pid_thread(pgrp, PIDTYPE_PGID, p);
			read_unlock(&tasklist_lock);

			break;
		case IOPRIO_WHO_USER:
			uid = make_kuid(current_user_ns(), who);	/* 사용자 namespace 기반 UID 변환 */
			if (!uid_valid(uid))
				break;
			if (!who)
				user = current_user();	/* who == 0이면 현재 사용자 */
			else
				user = find_user(uid);	/* 지정 UID의 user_struct 검색 */

			if (!user)
				break;

			for_each_process_thread(g, p) {	/* rcu_read_lock 아래 전체 태스크 순회 */
				if (!uid_eq(task_uid(p), uid) ||
				    !task_pid_vnr(p))		/* UID 불일치 또는 커널 스레드는 걸너뜀 */
					continue;
				ret = set_task_ioprio(p, ioprio);
				if (ret)
					goto free_uid;	/* 실패 시 find_user 참조 해제 필요 */
			}
free_uid:
			if (who)
				free_uid(user);	/* find_user()로 획득한 참조만 해제 */
			break;
		default:
			ret = -EINVAL;
	}

out:
	rcu_read_unlock();
	return ret;
}

/*
 * get_task_ioprio - 태스크의 유효한(effective) I/O 우선순위를 반환한다.
 *
 * 호출 경로:
 *   SYSCALL_DEFINE2(ioprio_get) -> get_task_ioprio
 *       -> security_task_getioprio
 *       -> __get_task_ioprio
 *
 * NVMe 연결점:
 *   - __get_task_ioprio는 io_context가 없거나 우선순위가 설정되지 않은
 *     경우 태스크의 nice 값에서 우선순위를 유도한다.
 *   - 반환값이 NVMe request 구성 시 실제로 사용되는 우선순위이며,
 *     block/blk-ioc.c에 저장된 raw 값과는 다를 수 있다.
 */
static int get_task_ioprio(struct task_struct *p)
{
	int ret;

	ret = security_task_getioprio(p);	/* LSM에 우선순위 조회 허용 여부 문의 */
	if (ret)
		goto out;
	task_lock(p);	/* task->io_context 접근 보호 */
	ret = __get_task_ioprio(p);	/* io_context/nice 기반 effective 우선순위 */
	task_unlock(p);
out:
	return ret;
}

/*
 * get_task_raw_ioprio - 사용자공간에서 마지막으로 설정한 raw ioprio를
 *                       반환한다(미설정 여부를 구분하기 위해).
 *
 * 호출 경로:
 *   SYSCALL_DEFINE2(ioprio_get, IOPRIO_WHO_PROCESS) -> get_task_raw_ioprio
 *
 * NVMe 연결점:
 *   - io_context가 없으면 IOPRIO_DEFAULT를 반환한다. 이 경우 NVMe 스택은
 *     nice 기반 effective 우선순위를 사용하게 된다.
 *   - p->io_context->ioprio가 사용자가 직접 지정한 값이며, NVMe 장치가
 *     이 값을 직접 해석하기보다는 block 스케줄러가 중간에서 해석한다.
 */

/*
 * Return raw IO priority value as set by userspace. We use this for
 * ioprio_get(pid, IOPRIO_WHO_PROCESS) so that we keep historical behavior and
 * also so that userspace can distinguish unset IO priority (which just gets
 * overriden based on task's nice value) from IO priority set to some value.
 */
static int get_task_raw_ioprio(struct task_struct *p)
{
	int ret;

	ret = security_task_getioprio(p);
	if (ret)
		goto out;
	task_lock(p);	/* task->io_context 접근 보호 */
	if (p->io_context)		/* io_context가 있으면 사용자가 설정한 raw 값 */
		ret = p->io_context->ioprio;
	else
		ret = IOPRIO_DEFAULT;	/* 미설정: NVMe 스택은 nice 기반값 사용 */
	task_unlock(p);
out:
	return ret;
}

/*
 * ioprio_best - 두 ioprio 값 중 더 높은 우선순위(숫자가 작은 값)를
 *               선택한다.
 *
 * 호출 경로:
 *   SYSCALL_DEFINE2(ioprio_get) -> ioprio_best
 *
 * NVMe 연결점:
 *   - 여러 태스크(PGRP/USER 단위 조회)의 우선순위를 집계할 때 사용되며,
 *     가장 높은 우선순위가 해당 태스크 집합의 NVMe 서비스 등급을
 *     대표하는 값으로 간주된다 (추정).
 */
static int ioprio_best(unsigned short aprio, unsigned short bprio)
{
	return min(aprio, bprio);	/* 숫자가 작을수록 우선순위 높음 */
}

/*
 * SYSCALL_DEFINE2(ioprio_get, ...) - ioprio_get 시스템콜 본체
 *
 * 목적:
 *   지정한 프로세스/프로세스 그룹/사용자에 대한 I/O 우선순위를 조회한다.
 *
 * 호출 경로:
 *   사용자공간 ioprio_get(2) -> SYSCALL_DEFINE2(ioprio_get)
 *       -> get_task_raw_ioprio (PROCESS 단위)
 *       -> get_task_ioprio / ioprio_best (PGRP/USER 단위)
 *
 * NVMe 연결점:
 *   - 조회된 값은 NVMe request 발행 경로에서 사용될 우선순위 힌트를
 *     확인하는 데 사용된다.
 *   - PGRP/USER 조회 시 ioprio_best로 가장 높은 우선순위를 선택하는
 *     이유는, 동일한 ioprio를 공유하는 태스크 집합이 동일한 NVMe SQ나
 *     스케줄러 엔티티로 묶일 가능성을 반영하기 위함이다 (추정).
 *   - ret == -ESRCH를 플래그처럼 사용하여 첫 번째 유효한 결과를
 *     초기화한다.
 */
SYSCALL_DEFINE2(ioprio_get, int, which, int, who)
{
	struct task_struct *g, *p;
	struct user_struct *user;
	struct pid *pgrp;
	kuid_t uid;
	int ret = -ESRCH;	/* 태스크를 찾지 못한 경우의 기본 오류 */
	int tmpio;		/* 개별 태스크의 effective 우선순위 임시 저장 */

	rcu_read_lock();	/* 태스크/pid 탐색 동안 객체 보호 */
	switch (which) {
		case IOPRIO_WHO_PROCESS:
			if (!who)		/* who == 0이면 자기 자신 */
				p = current;
			else
				p = find_task_by_vpid(who);	/* 지정 pid의 태스크 검색 */
			if (p)
				ret = get_task_raw_ioprio(p);	/* PROCESS 단위는 raw 값 반환 */
			break;
		case IOPRIO_WHO_PGRP:
			if (!who)
				pgrp = task_pgrp(current);
			else
				pgrp = find_vpid(who);
			read_lock(&tasklist_lock);	/* 프로세스 그룹 스레드 순회 보호 */
			do_each_pid_thread(pgrp, PIDTYPE_PGID, p) {
				tmpio = get_task_ioprio(p);	/* effective 우선순위 조회 */
				if (tmpio < 0)
					continue;	/* 권한 거부 등으로 조회 불가 시 무시 */
				if (ret == -ESRCH)
					ret = tmpio;	/* 첫 번째 유효 결과로 초기화 */
				else
					ret = ioprio_best(ret, tmpio);	/* 가장 높은 우선순위 선택 */
			} while_each_pid_thread(pgrp, PIDTYPE_PGID, p);
			read_unlock(&tasklist_lock);

			break;
		case IOPRIO_WHO_USER:
			uid = make_kuid(current_user_ns(), who);	/* 사용자 namespace 기반 UID 변환 */
			if (!who)
				user = current_user();	/* who == 0이면 현재 사용자 */
			else
				user = find_user(uid);	/* 지정 UID의 user_struct 검색 */

			if (!user)
				break;

			for_each_process_thread(g, p) {	/* rcu_read_lock 아래 전체 태스크 순회 */
				if (!uid_eq(task_uid(p), user->uid) ||
				    !task_pid_vnr(p))		/* UID 불일치 또는 커널 스레드는 걸너뜀 */
					continue;
				tmpio = get_task_ioprio(p);	/* effective 우선순위 조회 */
				if (tmpio < 0)
					continue;	/* 권한 거부 등으로 조회 불가 시 무시 */
				if (ret == -ESRCH)
					ret = tmpio;	/* 첫 번째 유효 결과로 초기화 */
				else
					ret = ioprio_best(ret, tmpio);	/* 가장 높은 우선순위 선택 */
			}

			if (who)
				free_uid(user);	/* find_user()로 획득한 참조만 해제 */
			break;
		default:
			ret = -EINVAL;
	}

	rcu_read_unlock();
	return ret;
}

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 사용자공간 ioprio를 커널 태스크의 io_context에 반영하는
 *   시스템콜 진입점이며, 실제 우선순위 저장은 block/blk-ioc.c에서 수행된다.
 * - 설정된 ioprio는 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *   -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로에서 스케줄러의
 *   타임슬라이스 및 NVMe SQ 우선순위 결정에 힌트를 제공한다 (추정).
 * - IOPRIO_CLASS_RT/BE/IDLE은 NVMe Weighted Round Robin의 서비스 등급과
 *   개념적으로 대응되며, RT 클래스는 높은 권한이 필요하다.
 * - ioprio_get은 PROCESS 단위로 raw 값을, PGRP/USER 단위로는 집계된
 *   effective 값을 반환하여 NVMe I/O 특성 진단에 활용될 수 있다.
 * - include/linux/ioprio.h의 매크로(IOPRIO_PRIO_CLASS/LEVEL)와
 *   Documentation/block/ioprio.rst가 이 파일의 의미 체계를 정의한다.
 */
