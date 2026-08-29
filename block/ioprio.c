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
 * [한국어 설명] ioprio_set(2)/ioprio_get(2) 시스템 콜을 구현하는 프로세스별
 * I/O 우선순위 설정/조회 인터페이스 (ioprio.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 사용자공간이 ioprio_set(2)/ioprio_get(2) 시스템 콜을 통해 특정
 * 프로세스(PRIO_PROCESS)/프로세스 그룹(PRIO_PGRP)/사용자(PRIO_USER) 단위로
 * I/O 우선순위(ioprio)를 설정하거나 조회할 수 있게 해 주는 커널 진입점이다.
 * ioprio는 상위 비트의 클래스(IOPRIO_CLASS_RT=실시간(Real Time)/BE=Best
 * Effort/IDLE=유휴)와 하위 비트의 클래스 내부 레벨(0이 가장 높고 7이 가장
 * 낮음)로 구성된 값이며, 이 파일은 그 값을 검증(capability 확인 포함)한 뒤
 * 대상 태스크(task_struct)의 io_context->ioprio 필드에 기록하거나, 반대로
 * 그 필드(또는 미설정 시 nice 기반 유도값)를 읽어 사용자에게 돌려준다. 이
 * 파일 자체는 io_context의 할당/락 규칙이나 실제 저장 로직을 직접 구현하지
 * 않고, 그 부분은 전부 block/blk-ioc.c(set_task_ioprio(), __get_task_ioprio())
 * 에 위임한다 - 이 파일은 "누구에게(which/who) 적용할지 찾아내고 반복하는"
 * 상위 순회 로직과 "값이 유효한지, 권한이 있는지" 검증 로직만 담당한다.
 *
 * *** 매우 중요 - block/blk-ioprio.c(cgroup 강제 정책)와의 구분 ***
 * 이 파일(ioprio.c)의 ioprio_set(2)는 "태스크(프로세스) 단위"로 ioprio를
 * 사용자가 명시적으로 지정하는 전통적인 syscall 인터페이스다. 이는
 * block/blk-ioprio.c가 구현하는 blkcg(Block I/O Control Group) 기반
 * "io.prio.class" cgroup 강제 정책과는 계층과 적용 범위가 완전히 다르다:
 *   - ioprio.c(본 파일): task_struct 단위. 태스크가 만든 bio가 제출될 때
 *     submit_bio() 계열이 태스크의 io_context->ioprio 값을 bio->bi_ioprio에
 *     초기값으로 채워 넣는다(구현은 block/blk-core.c 부근, 이 파일 범위 밖).
 *     ioprio_set()이 건드리지 못하는 대표적 사각지대는 커널 워커(kworker)가
 *     대신 제출하는 페이지 캐시 writeback I/O다 - 그 bio는 어떤 태스크의
 *     io_context와도 직접 연결되지 않기 때문이다.
 *   - block/blk-ioprio.c: bio가 소속된 cgroup(blkg/blkcg) 단위. cgroup
 *     관리자가 "io.prio.class" 파일에 POLICY_PROMOTE_TO_RT/RESTRICT_TO_BE/
 *     ALL_TO_IDLE 중 하나를 써 두면, blkcg_set_ioprio()가 매 bio 제출마다
 *     그 cgroup 정책값으로 bio->bi_ioprio를 "강제 덮어쓰기"한다. 이 강제는
 *     writeback I/O에도 적용되므로 ioprio_set()의 사각지대를 메울 수 있다.
 * 두 메커니즘은 동일한 목적지(bio->bi_ioprio, 이후 request->ioprio)를
 * 공유하지만 서로 완전히 독립된 코드 경로이며, 이 파일은 blk-ioprio.c의
 * 어떤 심볼도 참조하지 않는다. 실제 bio 제출 시점에는 두 값 중 나중에
 * 적용되는 쪽(통상 blkcg_set_ioprio()가 submit_bio_noacct_nocheck()에서
 * 더 늦게 호출됨)이 최종값을 결정한다(추정 - 정확한 순서는 blk-core.c 참고).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일의 두 시스템 콜은 I/O 제출 경로 자체보다 훨씬 앞선, "정책을
 * 미리 설정/조회해 두는" 준비 단계에 위치한다 - 즉 실제 bio/request가
 * 만들어지는 매 I/O마다 호출되는 것이 아니라, 사용자공간 도구(ionice(1)
 * 등)가 한 번(또는 드물게) 호출해 두면 그 뒤로 발생하는 모든 I/O에 영향을
 * 미치는 구조다. 대략적인 두 호출 체인은 다음과 같다:
 *
 *   (설정 경로)
 *   사용자공간 ionice(1) -> ioprio_set(2) -> SYSCALL_DEFINE3(ioprio_set)
 *     -> ioprio_check_cap() (클래스/레벨 검증 + capability 확인)
 *     -> set_task_ioprio() (block/blk-ioc.c, task->io_context에 실제 반영)
 *   (그 이후, 별도 시점에 발생하는 모든 I/O 제출마다)
 *     -> submit_bio() -> ... -> (task->io_context->ioprio 값이 bio->bi_ioprio
 *        초기값으로 반영, 구현은 이 파일 범위 밖)
 *     -> blk_mq_submit_bio() -> blk_mq_get_request() (request->ioprio로 전파)
 *     -> I/O 스케줄러(mq-deadline/bfq/kyber)가 디스패치 순서 결정에 참고
 *     -> (NVMe 드라이버 경로라면) nvme_queue_rq() -> nvme_sq_copy_cmd/nvme_write_sq_db()로
 *        SQ(Submission Queue)에 커맨드가 올라가고 도어벨(doorbell)이 눌림
 *        (NVMe 컨트롤러가 WRR(Weighted Round Robin) Arbitration을 지원하는
 *        경우의 이야기이며, 정확한 매핑 여부는 개별 드라이버 구현에 달려
 *        있다 - 추정)
 *
 *   (조회 경로)
 *   사용자공간 ionice(1) -p/-P -> ioprio_get(2) -> SYSCALL_DEFINE2(ioprio_get)
 *     -> get_task_raw_ioprio()/get_task_ioprio() (task->io_context를 직접
 *        읽거나, 미설정 시 nice 기반 유도값 계산)
 *     -> ioprio_best() (PGRP/USER처럼 여러 태스크를 집계할 때 대표값 선택)
 *
 * 실행 컨텍스트는 항상 시스템 콜을 호출한 프로세스 자신의 컨텍스트이며
 * (인터럽트/softirq 컨텍스트에서 호출될 수 없음), rcu_read_lock()과
 * tasklist_lock으로 대상 태스크 탐색 동안만 보호될 뿐 이 파일 자체가
 * 별도의 커널 스레드나 워크큐를 띄우지는 않는다.
 *
 * === 타 모듈과의 연결 ===
 * - block/blk-ioc.c: set_task_ioprio()(EXPORT_SYMBOL_GPL)가 이 파일이
 *   호출하는 유일한 "쓰기" 진입점이다. io_context가 아직 없으면
 *   alloc_io_context()로 새로 만든 뒤 ioprio 필드를 채운다. 이 파일은
 *   io_context의 락 규칙(task_lock)이나 참조카운트 관리에는 관여하지 않고
 *   전부 set_task_ioprio()에 위임한다.
 * - include/linux/ioprio.h: __get_task_ioprio()(task->io_context가 없거나
 *   클래스가 IOPRIO_CLASS_NONE이면 task_nice_ioprio()/task_nice_ioclass()로
 *   CFS(Completely Fair Scheduler) nice 값을 ioprio로 변환), ioprio_check_cap()
 *   /set_task_ioprio()의 extern 선언, IOPRIO_DEFAULT 매크로를 제공하는 상위
 *   헤더.
 * - include/uapi/linux/ioprio.h: IOPRIO_CLASS_RT/BE/IDLE/NONE/INVALID enum과
 *   IOPRIO_PRIO_CLASS()/IOPRIO_PRIO_LEVEL() 비트 추출 매크로, IOPRIO_WHO_
 *   PROCESS/PGRP/USER enum을 정의하는 사용자공간 공유 UAPI 헤더. ioprio 값의
 *   비트 레이아웃(상위 비트=클래스, 하위 비트=레벨)에 대한 유일한 근거다.
 * - kernel/pid.c, kernel/user.c(이 저장소 sparse checkout 범위 밖): PID/UID
 *   네임스페이스를 인식하는 find_task_by_vpid()/find_vpid()/find_user() 등을
 *   제공하며, 이 파일의 PGRP/USER 순회 로직이 이들에 의존한다.
 * - block/blk-ioprio.c(+ blk-ioprio.h): 별도 계층인 blkcg 기반 강제 정책
 *   구현체. 위 "매우 중요" 단락에서 설명했듯 이 파일과는 독립적이지만
 *   최종적으로 같은 필드(bio->bi_ioprio)를 놓고 순차 적용된다.
 * - Documentation/block/ioprio.rst: 이 시스템 콜들의 사용자공간 계약(값
 *   인코딩, ionice(1) 사용 예)을 설명하는 문서.
 * - 데이터 흐름: ionice(1) 등 사용자공간 -> ioprio_set(2) 인자(which/who/
 *   ioprio) -> 이 파일이 which로 대상(task_struct 하나 또는 집합)을 찾음 ->
 *   set_task_ioprio() -> task->io_context->ioprio 갱신 -> (이후) bio 제출 시
 *   bio->bi_ioprio로 복사 -> request->ioprio로 전파 -> 스케줄러/드라이버 소비.
 *
 * === 주요 함수/구조체 요약 ===
 * - ioprio_check_cap(int ioprio): 클래스(RT/BE/IDLE/NONE/INVALID)와 레벨의
 *   유효성, 그리고 RT 클래스에 필요한 CAP_SYS_ADMIN/CAP_SYS_NICE capability를
 *   검사한다. include/linux/ioprio.h에 extern 선언되어 있어 이 파일 밖(예:
 *   io_uring의 SQE 단위 ioprio 힌트 검증 등)에서도 재사용될 수 있다(추정).
 * - SYSCALL_DEFINE3(ioprio_set, which, who, ioprio): ioprio_set(2)의 본체.
 *   which(PROCESS/PGRP/USER)에 따라 대상 태스크 하나 또는 집합을 찾아
 *   set_task_ioprio()를 반복 호출한다.
 * - get_task_ioprio(p) / get_task_raw_ioprio(p): 전자는 LSM 검사 + nice 기반
 *   유도까지 포함한 "유효(effective)" 우선순위를, 후자는 사용자가 마지막으로
 *   설정한 "원본(raw)" 값(미설정 시 IOPRIO_DEFAULT)만을 반환한다 - ioprio_get의
 *   PROCESS 케이스는 역사적 호환성을 위해 raw 값을 쓴다.
 * - ioprio_best(aprio, bprio): 두 ioprio 값 중 숫자가 더 작은(=우선순위가
 *   더 높은) 쪽을 고르는 비교기. PGRP/USER처럼 여러 태스크의 값을 하나로
 *   집계할 때 "그룹 내 가장 급한 태스크"를 대표값으로 삼기 위해 쓰인다.
 * - SYSCALL_DEFINE2(ioprio_get, which, who): ioprio_get(2)의 본체.
 *   PROCESS는 raw 값 하나를, PGRP/USER는 대상 집합을 순회하며 ioprio_best()로
 *   집계한 값을 반환한다.
 * - 공유 핵심 자료구조: struct task_struct::io_context(설정/조회 대상),
 *   struct io_context::ioprio(실제 값이 저장되는 필드, block/blk-ioc.c가
 *   정의), struct pid(PGRP 탐색 키), struct user_struct(USER 탐색 키).
 */
#include <linux/gfp.h>	/* [한국어] GFP_* 메모리 할당 플래그 매크로 정의 - 이 파일 자체가 직접 커널 메모리를 할당하지는 않으나(할당은 set_task_ioprio()가 위임하는 alloc_io_context()가 담당), 원본 fs/ioprio.c 시절부터 유지되어 온 포함으로 보인다(추정) */
#include <linux/kernel.h>	/* [한국어] 커널 기본 매크로/타입 - ioprio_best()의 min() 매크로 등 커널 공통 헬퍼를 쓰기 위해 포함 */
#include <linux/ioprio.h>	/* [한국어] IOPRIO_PRIO_CLASS()/IOPRIO_PRIO_LEVEL()/IOPRIO_DEFAULT 매크로와 __get_task_ioprio()/set_task_ioprio()/ioprio_check_cap()의 선언을 제공 - 이 파일이 구현하는 두 시스템 콜의 핵심 타입/함수 계약 */
#include <linux/cred.h>	/* [한국어] struct cred, current_user()/current_user_ns() 등 현재 태스크의 자격증명(credential) 접근 API - IOPRIO_WHO_USER 처리에서 사용 */
#include <linux/blkdev.h>	/* [한국어] 블록 계층 핵심 타입(struct request_queue 등) 선언 - 이 파일 범위 안에서 직접 참조하는 심볼은 보이지 않으나, ioprio 개념이 블록 계층 것이므로 관례적으로 포함된 것으로 보인다(추정) */
#include <linux/capability.h>	/* [한국어] capable(), CAP_SYS_ADMIN/CAP_SYS_NICE 등 capability 상수 - ioprio_check_cap()의 RT 클래스 권한 검사에 필수 */
#include <linux/syscalls.h>	/* [한국어] SYSCALL_DEFINE2()/SYSCALL_DEFINE3() 매크로 - 이 파일의 두 시스템 콜 진입점(ioprio_set/ioprio_get)을 선언하는 데 필수 */
#include <linux/security.h>	/* [한국어] security_task_getioprio() LSM(Linux Security Module, 예: SELinux/AppArmor) 훅 선언 - get_task_ioprio()/get_task_raw_ioprio()가 조회 권한을 확인할 때 사용 */
#include <linux/pid_namespace.h>	/* [한국어] find_task_by_vpid()/find_vpid() 등 pid 네임스페이스 인식 탐색 함수 - IOPRIO_WHO_PROCESS/PGRP 처리에서 사용 */

/*
 * [한국어]
 * ioprio_check_cap() - 사용자가 지정한 ioprio 값의 클래스/레벨이 유효한지,
 * 그리고 해당 클래스를 요청할 capability(권한)가 있는지를 검증한다.
 *
 * @ioprio: 검증할 원본 ioprio 값. 상위 비트에 IOPRIO_PRIO_CLASS()로 추출되는
 *          클래스(RT=1/BE=2/IDLE=3/NONE=0/INVALID=7), 하위 비트에
 *          IOPRIO_PRIO_LEVEL()로 추출되는 클래스 내부 레벨(0~7, 낮을수록
 *          우선순위 높음)이 인코딩되어 있다. 사용자공간이 ioprio_set(2)
 *          syscall 인자로 넘긴 값이 SYSCALL_DEFINE3(ioprio_set)를 거쳐
 *          그대로 전달된다.
 * @return: 0이면 검증 통과. -EPERM이면 RT 클래스를 요청했으나 필요한
 *          capability(CAP_SYS_ADMIN 또는 CAP_SYS_NICE)가 없는 경우.
 *          -EINVAL이면 클래스 자체가 잘못됐거나(IOPRIO_CLASS_INVALID/정의되지
 *          않은 값), NONE 클래스인데 레벨이 0이 아닌 경우(그런 조합은 의미가
 *          없으므로 거부).
 *
 * 배경(Why): ioprio 값은 커널 내부적으로 스케줄러(BFQ/mq-deadline)의 디스패치
 * 우선순위를 좌우하므로, 아무 프로세스나 RT(Real Time) 클래스를 자칭해
 * 다른 프로세스의 I/O를 굶길 수 있으면 안 된다. 이 함수는 그 최소한의
 * 게이트키퍼 역할을 한다.
 * 동작(What): switch (class)로 분기해 (1) RT면 CAP_SYS_ADMIN 또는
 * CAP_SYS_NICE 중 하나라도 있어야 통과, (2) BE/IDLE은 누구나 허용, (3) NONE은
 * level이 반드시 0이어야 함(NONE은 "우선순위 미지정"을 뜻하므로 레벨 값
 * 자체가 무의미), (4) INVALID/그 외 정의되지 않은 클래스는 무조건 거부한다.
 * 실행 컨텍스트: 시스템 콜을 요청한 프로세스 컨텍스트에서 동기 실행되며,
 * capable()은 현재 태스크(current)의 자격증명만 확인하므로 재진입/동시성
 * 이슈가 없다.
 * 호출자(Who calls): SYSCALL_DEFINE3(ioprio_set)이 값을 실제로 반영하기 전에
 * 가장 먼저 호출한다. include/linux/ioprio.h에 extern 선언되어 있어 이 파일
 * 밖의 다른 서브시스템(예: io_uring의 SQE 단위 ioprio 힌트 검증 등)에서도
 * 재사용될 수 있다(추정 - 이 저장소 트리 범위에서는 다른 호출자를 확인하지
 * 못함).
 * 피호출자(Who is called): capable() (커널 capability 검사 코어 함수)만
 * 호출한다.
 * 에러 경로(Error path): 위 @return 설명대로 -EPERM/-EINVAL을 즉시 반환하며,
 * 호출자(ioprio_set)는 이 값을 그대로 시스템 콜 반환값으로 사용자공간에
 * 전달한다(추가 처리 없음).
 *
 * 호출 체인:
 *   사용자공간 ioprio_set(2) -> SYSCALL_DEFINE3(ioprio_set) -> [ioprio_check_cap]
 *   -> capable(CAP_SYS_ADMIN/CAP_SYS_NICE)
 */
int ioprio_check_cap(int ioprio)
{
	int class = IOPRIO_PRIO_CLASS(ioprio);	/* [한국어] ioprio 값의 상위 비트(비트 13~15, IOPRIO_CLASS_SHIFT=13 기준)에서 클래스 필드를 추출 - RT=1/BE=2/IDLE=3/NONE=0/INVALID=7 중 하나가 됨 */
	int level = IOPRIO_PRIO_LEVEL(ioprio);	/* [한국어] ioprio 값의 하위 3비트(IOPRIO_LEVEL_NR_BITS=3)에서 클래스 내부 우선순위 레벨(0~7)을 추출 - 값이 작을수록 우선순위가 높다 */

	switch (class) {	/* [한국어] 추출한 클래스 값에 따라 필요한 권한/유효성 검사가 서로 다르므로 분기 */
		/* [한국어] RT(Real Time) 클래스 요청 - BFQ/mq-deadline 스케줄러에서 가장 높은 서비스 등급이며 다른 프로세스의 I/O를 상시 선점할 수 있으므로, 아무나 자칭할 수 없도록 별도 capability 검사가 필요하다. NVMe 컨트롤러가 WRR(Weighted Round Robin) Arbitration을 지원한다면 궁극적으로 Urgent/High 큐 힌트로 이어질 가능성이 있다(추정) */
		case IOPRIO_CLASS_RT:
			/*
			 * Originally this only checked for CAP_SYS_ADMIN,
			 * which was implicitly allowed for pid 0 by security
			 * modules such as SELinux. Make sure we check
			 * CAP_SYS_ADMIN first to avoid a denial/avc for
			 * possibly missing CAP_SYS_NICE permission.
			 */
			if (!capable(CAP_SYS_ADMIN) && !capable(CAP_SYS_NICE))	/* [한국어] CAP_SYS_ADMIN 또는 CAP_SYS_NICE 둘 중 하나라도 있으면 통과 - capable()은 현재 태스크(current)의 유효 capability 집합을 확인하는 커널 API. 원본 영어 주석대로 SELinux 등 LSM의 avc(access vector cache) 거부 로그를 줄이기 위해 CAP_SYS_ADMIN을 먼저 검사한다 */
				return -EPERM;	/* [한국어] 두 capability 모두 없으면 RT 클래스 요청을 거부 - 호출자(ioprio_set)는 이 값을 그대로 시스템 콜 반환값으로 사용자공간에 전달 */
			break;	/* [한국어] capability 검사를 통과한 RT 요청 - switch 종료 후 함수 끝의 return 0으로 진행 */
		/* [한국어] BE(Best Effort)와 IDLE 클래스 요청 - BE는 스케줄러 기본(default) 등급이라 모든 프로세스가 자유롭게 지정 가능하고, IDLE은 그보다 더 낮아 디스크가 유휴 상태일 때만 서비스되는 등급이라 마찬가지로 특별한 권한이 필요 없다 */
		case IOPRIO_CLASS_BE:
		case IOPRIO_CLASS_IDLE:
			break;	/* [한국어] BE/IDLE은 별도 권한 검사 없이 통과 */
		/* [한국어] NONE 클래스 요청 - "우선순위 미지정"을 의미하며 그 자체로는 유효하지만, level 필드는 반드시 0이어야 한다(NONE과 결합된 0이 아닌 레벨 값은 의미가 없는 조합이므로) */
		case IOPRIO_CLASS_NONE:
			if (level)	/* [한국어] NONE인데 level이 0이 아니면 잘못된 조합 - 사용자가 실수로 레벨만 채운 경우를 걸러낸다 */
				return -EINVAL;	/* [한국어] 잘못된 (NONE, level!=0) 조합을 거부 */
			break;	/* [한국어] level==0인 정상적인 NONE 요청은 통과 */
		/* [한국어] IOPRIO_CLASS_INVALID(=7, "잘못된 ioprio 값"임을 나타내는 전용 상수)이거나 위 어떤 case에도 해당하지 않는 정의되지 않은 클래스 값 - 둘 다 무조건 거부 */
		case IOPRIO_CLASS_INVALID:
		default:
			return -EINVAL;	/* [한국어] 정의되지 않은/명시적으로 무효한 클래스는 어떤 경우에도 커널에 반영되지 않도록 여기서 차단 */
	}

	return 0;	/* [한국어] 모든 검사를 통과 - ioprio_set()이 이 반환값(0)을 확인한 뒤 실제 값 반영 단계로 진행 */
}

/*
 * [한국어]
 * SYSCALL_DEFINE3(ioprio_set, which, who, ioprio) - ioprio_set(2) 시스템 콜
 * 본체. 지정한 프로세스/프로세스 그룹/사용자에게 속한 태스크(들)의 I/O
 * 우선순위를 설정한다.
 *
 * @which: 대상의 종류. IOPRIO_WHO_PROCESS(단일 프로세스)/IOPRIO_WHO_PGRP
 *         (프로세스 그룹)/IOPRIO_WHO_USER(사용자) 중 하나. 사용자공간의
 *         PRIO_PROCESS/PRIO_PGRP/PRIO_USER(getpriority(2)와 동일한 관례)에
 *         대응한다.
 * @who: which의 종류에 따라 해석되는 대상 식별자(pid, pgid, 또는 uid).
 *       0이면 "호출자 자신(또는 자신이 속한 그룹/사용자)"을 의미한다.
 * @ioprio: 설정할 ioprio 값. IOPRIO_PRIO_VALUE(class, level) 형태로 조립된
 *          값이며, ioprio_check_cap()의 검증을 통과해야 한다.
 * @return: 성공 시 0. 실패 시 -ESRCH(대상을 하나도 찾지 못함) / -EPERM(RT
 *          클래스에 필요한 capability 없음) / -EINVAL(잘못된 which 또는
 *          ioprio 클래스/레벨 조합) 중 하나.
 *
 * 배경(Why): ionice(1) 등 사용자공간 도구가 "이 프로세스(또는 그룹/사용자)가
 * 만드는 I/O는 앞으로 이런 우선순위로 처리해 달라"고 커널에 지시하는 유일한
 * 표준 인터페이스다. 설정된 값은 즉시 효과가 나는 것이 아니라, 대상
 * task_struct의 io_context->ioprio 필드에 저장되어 두었다가 그 태스크가
 * 이후 I/O를 제출할 때마다 참고된다.
 * 동작(What) 단계별 설명:
 *   1) ioprio_check_cap()으로 값 자체의 유효성과 권한을 먼저 검증한다(대상을
 *      찾기 전에 값부터 검증하므로, 잘못된 값이면 대상 탐색 비용조차 들이지
 *      않는다).
 *   2) rcu_read_lock()으로 태스크 탐색 구간을 보호하며 which로 분기:
 *      - PROCESS: find_task_by_vpid()로 단일 태스크를 찾아 set_task_ioprio()
 *        한 번 호출.
 *      - PGRP: task_pgrp()/find_vpid()로 그룹을 찾고, tasklist_lock 아래
 *        do_each_pid_thread()로 그룹 내 모든 스레드를 순회하며 각각
 *        set_task_ioprio() 호출. 중간에 하나라도 실패하면 즉시 중단(그
 *        시점까지 반영된 태스크는 되돌리지 않는 부분 적용 상태로 남을 수
 *        있다 - 원자적 트랜잭션이 아니다).
 *      - USER: make_kuid()로 uid를 변환하고, find_user()/current_user()로
 *        user_struct를 찾은 뒤 for_each_process_thread()로 시스템 전체
 *        태스크를 순회하며 UID가 일치하는 것만 set_task_ioprio() 호출.
 *   3) 실제 저장은 매번 block/blk-ioc.c의 set_task_ioprio()에 위임한다 -
 *      이 함수는 io_context 할당/락/LSM 재검사(security_task_setioprio())를
 *      전부 그쪽에 맡긴다.
 * 실행 컨텍스트: 시스템 콜을 호출한 프로세스 컨텍스트에서 동기 실행. PGRP
 * 케이스는 tasklist_lock(rwlock)까지 함께 걸어 두 단계 잠금(RCU + rwlock)이
 * 필요한 반면, USER 케이스는 for_each_process_thread()가 RCU만으로 충분하다고
 * 가정하는 인터페이스라 tasklist_lock을 걸지 않는다(이 파일의 기존 설계).
 * 호출자(Who calls): 사용자공간에서 ioprio_set(2) libc 래퍼를 통해 진입.
 * 대표적 사용자는 ionice(1) 커맨드나 io_uring/스케줄러 튜닝 도구.
 * 피호출자(Who is called): ioprio_check_cap(), set_task_ioprio()(block/
 * blk-ioc.c), 그리고 태스크/pid/uid 탐색을 위한 find_task_by_vpid()/
 * task_pgrp()/find_vpid()/make_kuid()/find_user()/current_user().
 * 에러 경로(Error path): PGRP 케이스 중간 실패는 goto out으로 tasklist_lock을
 * 먼저 풀고 즉시 반환한다(부분 적용된 상태일 수 있음을 호출자가 알 방법은
 * 없다). USER 케이스 중간 실패는 goto free_uid로 이동해 find_user()가 잡은
 * 참조만 정리한 뒤 반환한다.
 *
 * 호출 체인:
 *   사용자공간 ionice(1) -> ioprio_set(2) -> [SYSCALL_DEFINE3(ioprio_set)]
 *   -> ioprio_check_cap() -> set_task_ioprio() (block/blk-ioc.c)
 *   -> (대상 태스크의) task->io_context->ioprio 갱신
 *   -> (이후 별도 시점) submit_bio() 계열이 bio->bi_ioprio 초기값으로 반영
 *   -> blk_mq_submit_bio() -> I/O 스케줄러 디스패치 -> (NVMe 경로) doorbell
 */
SYSCALL_DEFINE3(ioprio_set, int, which, int, who, int, ioprio)	/* [한국어] ioprio_set(2) 시스템 콜 진입점 매크로 - 확장되면 asmlinkage long sys_ioprio_set(int which, int who, int ioprio) 함수 정의가 된다 */
{
	struct task_struct *p, *g;	/* [한국어] p는 PROCESS 케이스의 대상 태스크이자 PGRP 케이스의 순회 변수로 재사용. g는 USER 케이스에서 for_each_process_thread()의 스레드 그룹 리더 순회 변수로만 쓰인다 */
	struct user_struct *user;	/* [한국어] IOPRIO_WHO_USER 케이스에서 대상 UID의 user_struct를 담는다 - find_user()/current_user()로 획득하며, find_user()로 얻은 경우에만 함수 끝에서 free_uid()로 참조 해제가 필요하다 */
	struct pid *pgrp;	/* [한국어] IOPRIO_WHO_PGRP 케이스에서 대상 프로세스 그룹의 struct pid를 담는다 - task_pgrp()/find_vpid()로 획득 */
	kuid_t uid;	/* [한국어] IOPRIO_WHO_USER 케이스에서 who(원시 uid 값)를 현재 사용자 네임스페이스 기준으로 변환한 커널 내부 kuid_t 값 */
	int ret;	/* [한국어] 이 시스템 콜의 최종 반환값 - 아래에서 -ESRCH로 초기화된 뒤 각 case에서 갱신된다 */

	ret = ioprio_check_cap(ioprio);	/* [한국어] 값을 실제로 반영하기 전에 먼저 클래스/레벨 유효성과 capability를 검증 */
	/* [한국어] 검증 실패(0이 아닌 반환값)면 즉시 종료 - 어떤 태스크의 io_context도 건드리지 않는다 */
	if (ret)
		return ret;	/* [한국어] -EPERM 또는 -EINVAL을 그대로 사용자공간에 전달 */

	ret = -ESRCH;	/* [한국어] 기본값을 "대상 없음" 오류로 초기화 - PROCESS 케이스에서 p가 NULL이거나 PGRP/USER 케이스에서 대상이 하나도 없으면 이 값이 그대로 반환된다 */
	rcu_read_lock();	/* [한국어] find_task_by_vpid()/for_each_process_thread() 등 태스크 목록 탐색 동안 task_struct가 RCU(Read-Copy-Update) 유예 기간 없이 해제되지 않도록 보호 시작 */
	switch (which) {	/* [한국어] ioprio_set(2)의 첫 인자(PRIO_PROCESS/PRIO_PGRP/PRIO_USER에 대응하는 IOPRIO_WHO_* 값)에 따라 대상 탐색 방식이 완전히 다르므로 분기 */
		case IOPRIO_WHO_PROCESS:	/* [한국어] 단일 프로세스(스레드) 대상 */
			/* [한국어] who==0은 "자기 자신"을 가리키는 관례(getpriority/setpriority와 동일) */
			if (!who)
				p = current;	/* [한국어] 호출한 태스크 자신을 대상으로 지정 */
			else
				p = find_task_by_vpid(who);	/* [한국어] who를 호출자의 pid 네임스페이스 기준 vpid로 해석해 대상 task_struct를 찾음 - 없으면 NULL */
			/* [한국어] 대상 태스크를 찾은 경우에만 반영 시도 - 못 찾으면 ret은 위에서 설정한 -ESRCH 그대로 유지된다 */
			if (p)
				ret = set_task_ioprio(p, ioprio);	/* [한국어] block/blk-ioc.c의 set_task_ioprio()에 위임 - 필요하면 io_context를 새로 할당하고 ioprio 필드를 갱신 */
			break;	/* [한국어] PROCESS 케이스 종료 */
		case IOPRIO_WHO_PGRP:	/* [한국어] 프로세스 그룹(PGID) 소속 모든 스레드 대상 */
			/* [한국어] who==0이면 호출자 자신이 속한 프로세스 그룹을 대상으로 */
			if (!who)
				pgrp = task_pgrp(current);	/* [한국어] 현재 태스크가 속한 프로세스 그룹의 struct pid 획득 */
			else
				pgrp = find_vpid(who);	/* [한국어] who를 vpid로 해석해 해당 프로세스 그룹의 struct pid를 조회 - 존재하지 않으면 NULL */

			read_lock(&tasklist_lock);	/* [한국어] do_each_pid_thread()로 그룹 내 태스크 리스트를 순회하는 동안 tasklist_lock(전역 rwlock)으로 태스크 리스트 변경(fork/exit)을 막는다 - rcu_read_lock만으로는 pid에 연결된 태스크 리스트 전체 순회의 일관성을 보장하지 못하기 때문 */
			do_each_pid_thread(pgrp, PIDTYPE_PGID, p) {	/* [한국어] pgrp에 연결된 PIDTYPE_PGID(프로세스 그룹) 타입의 모든 태스크를 p로 순회하는 매크로 - while_each_pid_thread와 짝을 이루는 do/while 순회 루프 시작부 */
				ret = set_task_ioprio(p, ioprio);	/* [한국어] 그룹 내 각 태스크에 동일한 ioprio를 반영 */
				if (ret) {	/* [한국어] 그룹 중 한 태스크라도 실패하면(예: 재검증 실패) 나머지는 처리하지 않고 즉시 중단 */
					read_unlock(&tasklist_lock);	/* [한국어] goto로 루프를 벗어나기 전에 보유 중이던 tasklist_lock을 반드시 먼저 해제 - 그렇지 않으면 락 불균형으로 데드락/경고가 발생한다 */
					goto out;	/* [한국어] 함수 끝의 out 레이블로 점프해 rcu_read_unlock() 후 ret(실패 코드)을 반환 */
				}
			} while_each_pid_thread(pgrp, PIDTYPE_PGID, p);	/* [한국어] do_each_pid_thread와 짝을 이루는 순회 종료 매크로 - 모든 그룹 스레드를 성공적으로 처리했으면 이 지점까지 도달한다 */
			read_unlock(&tasklist_lock);	/* [한국어] 정상적으로 순회를 마쳤으면 tasklist_lock 해제 */

			break;	/* [한국어] PGRP 케이스 종료 */
		case IOPRIO_WHO_USER:	/* [한국어] 특정 UID(사용자)에 속한 모든 태스크 대상 */
			uid = make_kuid(current_user_ns(), who);	/* [한국어] who(원시 uid 값)를 호출자의 사용자 네임스페이스 기준 kuid_t로 변환 - 네임스페이스 간 UID 매핑을 올바르게 처리하기 위함 */
			if (!uid_valid(uid))	/* [한국어] 변환 실패(해당 네임스페이스에 매핑되지 않는 UID) 시 처리 중단 */
				break;	/* [한국어] ret은 위의 -ESRCH를 유지한 채 switch를 빠져나간다 */
			/* [한국어] who==0이면 호출자 자신의 사용자를 대상으로 */
			if (!who)
				user = current_user();	/* [한국어] 현재 태스크의 cred에서 user_struct를 얻음 - 참조카운트를 새로 증가시키지 않는 차용(borrow) 성격의 접근 */
			else
				user = find_user(uid);	/* [한국어] uid에 해당하는 user_struct를 사용자 해시 테이블에서 검색하며 참조카운트를 증가시켜 반환 - 없으면 NULL. 이 경우는 아래에서 free_uid()로 반드시 해제해야 한다 */

			/* [한국어] 해당 UID의 user_struct가 없으면(예: 그 UID로 로그인/활동 이력이 전혀 없음) 처리할 대상이 없다 */
			if (!user)
				break;	/* [한국어] ret은 -ESRCH 유지 */

			for_each_process_thread(g, p) {	/* [한국어] rcu_read_lock() 보호 아래 시스템의 모든 스레드 그룹 리더(g)와 그 안의 모든 스레드(p)를 순회하는 매크로 - PGRP 케이스와 달리 tasklist_lock 없이 RCU만으로 순회한다(이 API의 설계) */
				if (!uid_eq(task_uid(p), uid) ||	/* [한국어] 순회 중인 태스크의 실효 UID가 대상 uid와 다르면 건너뜀 */
				    !task_pid_vnr(p))	/* [한국어] 또는 호출자의 pid 네임스페이스에서 보이지 않는 태스크(예: 다른 네임스페이스의 커널 스레드)면 건너뜀 - task_pid_vnr()이 0을 반환하는 경우 */
					continue;	/* [한국어] 조건에 맞지 않는 태스크는 반영하지 않고 다음 태스크로 */
				ret = set_task_ioprio(p, ioprio);	/* [한국어] 조건에 맞는 태스크에 ioprio 반영 */
				/* [한국어] 한 태스크라도 실패하면 나머지 순회를 중단 */
				if (ret)
					goto free_uid;	/* [한국어] find_user()로 얻은 참조가 있다면 해제해야 하므로 순회를 중단하고 free_uid 레이블로 점프 */
			}
free_uid:	/* [한국어] 성공/실패와 무관하게 USER 케이스가 끝나면 반드시 거치는 정리 지점 - find_user()로 얻은 참조 해제를 담당한다 */
			/* [한국어] who==0(current_user 경로)이었다면 user는 current_user()의 차용 포인터이므로 해제하면 안 됨 - who!=0(find_user 경로)일 때만 해제한다 */
			if (who)
				free_uid(user);	/* [한국어] find_user()가 증가시킨 참조카운트를 감소 */
			break;	/* [한국어] USER 케이스 종료 */
		default:	/* [한국어] PROCESS/PGRP/USER 외의 정의되지 않은 which 값 */
			ret = -EINVAL;	/* [한국어] 잘못된 which 인자에 대해 -EINVAL 반환 준비 */
	}

out:	/* [한국어] PGRP 케이스의 부분 실패(goto out)와 정상 종료 경로가 모두 합류하는 지점 */
	rcu_read_unlock();	/* [한국어] 함수 시작부에서 건 RCU 보호 구간 종료 */
	return ret;	/* [한국어] 최종 결과(0 성공, 또는 -ESRCH/-EPERM/-EINVAL 등 오류)를 시스템 콜 반환값으로 사용자공간에 전달 */
}

/*
 * [한국어]
 * get_task_ioprio() - 태스크 p의 "유효(effective)" I/O 우선순위를 조회한다.
 *
 * @p: 조회 대상 태스크. 호출자가 이미 유효성을 확인한 task_struct 포인터여야
 *     하며(NULL 불가), rcu_read_lock() 보호 구간 안에서 호출되는 것을 전제로
 *     한다(PGRP/USER 순회 중 호출되므로).
 * @return: 0 이상이면 유효한 ioprio 값(클래스/레벨 인코딩). 음수면
 *          security_task_getioprio()가 거부한 LSM 오류 코드(예: -EACCES 계열).
 *
 * 배경(Why): ioprio_get(2)의 PGRP/USER 케이스는 "그룹/사용자 전체의 대표
 * 우선순위"를 계산해야 하는데, 이때는 사용자가 명시적으로 설정했는지 여부와
 * 무관하게 "실제로 스케줄러가 적용할 값"이 필요하다. 이 함수는 그 실제
 * 적용값(설정 안 됐으면 nice 기반 유도값까지 포함)을 반환한다 - PROCESS
 * 케이스가 쓰는 get_task_raw_ioprio()(원본 값만 반환)와 대비된다.
 * 동작(What): 먼저 security_task_getioprio()로 LSM에 조회 허용 여부를 묻고,
 * 허용되면 task_lock(p)으로 p->io_context 포인터를 안정시킨 뒤
 * __get_task_ioprio(p)(include/linux/ioprio.h의 인라인 함수)를 호출한다.
 * __get_task_ioprio()는 io_context가 없거나 클래스가 IOPRIO_CLASS_NONE이면
 * task_nice_ioclass()/task_nice_ioprio()로 CFS(Completely Fair Scheduler)
 * nice 값을 ioprio로 변환하고, 그렇지 않으면 io_context->ioprio를 그대로
 * 돌려준다.
 * 실행 컨텍스트: 호출자(ioprio_get)가 이미 rcu_read_lock()을 잡은 상태에서
 * 호출되며, 이 함수 자신은 p->alloc_lock(task_lock)만 추가로 잡는다 - RCU
 * 읽기 락 보유 중에 task_lock(스핀락 기반)을 잡는 것은 허용되는 락 순서다.
 * 호출자(Who calls): SYSCALL_DEFINE2(ioprio_get)의 PGRP/USER 케이스가 그룹/
 * 사용자 내 각 태스크에 대해 반복 호출한다.
 * 피호출자(Who is called): security_task_getioprio(), task_lock()/
 * task_unlock(), __get_task_ioprio().
 * 에러 경로(Error path): LSM이 거부하면 goto out으로 task_lock을 아예 잡지
 * 않고 바로 반환한다(불필요한 락 획득을 피함).
 *
 * 호출 체인:
 *   SYSCALL_DEFINE2(ioprio_get) -> [get_task_ioprio] -> security_task_getioprio()
 *   -> task_lock() -> __get_task_ioprio() -> task_unlock()
 */
static int get_task_ioprio(struct task_struct *p)
{
	int ret;	/* [한국어] LSM 검사 결과 또는 최종 effective ioprio를 담을 반환값 */

	ret = security_task_getioprio(p);	/* [한국어] LSM(Linux Security Module, 예: SELinux/AppArmor) 훅을 호출해 호출자가 p의 ioprio를 조회할 권한이 있는지 확인 - 기본 정책은 대체로 허용이지만 강화된 LSM 정책에서 거부될 수 있다 */
	/* [한국어] LSM이 거부(0이 아닌 값, 보통 -EACCES 계열)했으면 io_context 조회 없이 즉시 종료 */
	if (ret)
		goto out;	/* [한국어] 아래 task_lock/__get_task_ioprio를 건너뛰고 곧장 반환 경로로 이동 */
	task_lock(p);	/* [한국어] p->io_context 포인터 자체가 동시에 다른 스레드(exit_io_context, set_task_ioprio 등)에 의해 바뀌지 않도록 태스크 락(p->alloc_lock) 획득 */
	ret = __get_task_ioprio(p);	/* [한국어] include/linux/ioprio.h의 인라인 함수 - io_context가 없거나 클래스가 NONE이면 task_nice_ioclass()/task_nice_ioprio()로 스케줄러 nice 값에서 ioprio를 유도하고, 그렇지 않으면 io_context->ioprio(사용자가 설정한 raw 값)를 그대로 반환 */
	task_unlock(p);	/* [한국어] 태스크 락 해제 */
out:	/* [한국어] LSM 거부 경로와 정상 조회 경로가 합류하는 지점 */
	return ret;	/* [한국어] LSM 오류 코드 또는 effective ioprio 값을 호출자(ioprio_get 계열)에 반환 */
}

/*
 * [한국어]
 * get_task_raw_ioprio() - 태스크 p에 사용자공간이 마지막으로 설정한 "원본
 * (raw)" ioprio 값을 조회한다(설정 여부 자체를 구분하기 위해 유도값으로
 * 대체하지 않음).
 *
 * @p: 조회 대상 태스크. get_task_ioprio()와 동일하게 rcu_read_lock() 보호
 *     구간 안에서 호출됨을 전제로 한다.
 * @return: 0 이상이면 raw ioprio 값 - io_context가 없으면 IOPRIO_DEFAULT
 *          (클래스 NONE, 레벨 0)를 반환한다. 음수면 security_task_getioprio()
 *          가 거부한 LSM 오류 코드.
 *
 * 배경(Why, 바로 아래 원본 영어 주석 참고): ioprio_get(pid, IOPRIO_WHO_PROCESS)
 * 에서만 이 함수를 쓰는 이유는 하위 호환성 때문이다 - 사용자공간이
 * "우선순위가 아예 설정된 적이 없음"(IOPRIO_DEFAULT)과 "명시적으로 어떤
 * 값으로 설정됨"을 구분할 수 있어야 하는데, get_task_ioprio()처럼 nice 기반
 * 유도값으로 치환해 버리면 그 구분이 사라진다.
 * 동작(What): get_task_ioprio()와 마찬가지로 LSM 검사 -> task_lock 순으로
 * 진행하되, __get_task_ioprio()를 호출하는 대신 p->io_context가 존재하면
 * 그 ioprio 필드를 직접 읽고, 없으면 IOPRIO_DEFAULT를 반환한다 - nice 기반
 * 유도 로직을 절대 거치지 않는다.
 * 실행 컨텍스트: get_task_ioprio()와 동일 - 호출자의 RCU 보호 구간 안에서
 * task_lock만 추가로 획득한다.
 * 호출자(Who calls): SYSCALL_DEFINE2(ioprio_get)의 IOPRIO_WHO_PROCESS
 * 케이스에서만 호출된다(PGRP/USER는 get_task_ioprio()를 사용).
 * 피호출자(Who is called): security_task_getioprio(), task_lock()/
 * task_unlock(). __get_task_ioprio()는 호출하지 않는다(핵심 차이점).
 * 에러 경로(Error path): get_task_ioprio()와 동일하게 LSM 거부 시 task_lock
 * 없이 바로 반환한다.
 *
 * 호출 체인:
 *   SYSCALL_DEFINE2(ioprio_get, IOPRIO_WHO_PROCESS) -> [get_task_raw_ioprio]
 *   -> security_task_getioprio() -> task_lock() -> (p->io_context 직접 읽기)
 *   -> task_unlock()
 */
/*
 * Return raw IO priority value as set by userspace. We use this for
 * ioprio_get(pid, IOPRIO_WHO_PROCESS) so that we keep historical behavior and
 * also so that userspace can distinguish unset IO priority (which just gets
 * overriden based on task's nice value) from IO priority set to some value.
 */
static int get_task_raw_ioprio(struct task_struct *p)
{
	int ret;	/* [한국어] LSM 검사 결과 또는 최종 raw ioprio 값을 담을 반환값 */

	ret = security_task_getioprio(p);	/* [한국어] LSM 훅으로 조회 권한 확인 - get_task_ioprio()와 동일한 검사 */
	/* [한국어] LSM이 거부했으면 io_context 조회 없이 즉시 종료 */
	if (ret)
		goto out;	/* [한국어] 반환 경로로 직행 */
	task_lock(p);	/* [한국어] p->io_context 포인터 접근을 보호하기 위해 태스크 락 획득 - get_task_ioprio()와 동일한 이유 */
	/* [한국어] io_context가 이미 할당되어 있다면(과거에 ioprio_set() 등으로 한 번이라도 초기화된 적 있음) 그 안의 raw 값을 사용 */
	if (p->io_context)
		ret = p->io_context->ioprio;	/* [한국어] 사용자가 마지막으로 ioprio_set()에 지정한 원본 값을 그대로 반환 - __get_task_ioprio()와 달리 nice 기반 유도값으로 대체하지 않는다 */
	else
		ret = IOPRIO_DEFAULT;	/* [한국어] io_context가 아직 할당된 적이 없으면 "미설정" 상태 - IOPRIO_DEFAULT(=IOPRIO_PRIO_VALUE(IOPRIO_CLASS_NONE,0), 즉 클래스 NONE/레벨 0)를 반환해 사용자공간이 "설정된 적 없음"을 구분할 수 있게 한다 */
	task_unlock(p);	/* [한국어] 태스크 락 해제 */
out:	/* [한국어] LSM 거부 경로와 정상 조회 경로가 합류하는 지점 */
	return ret;	/* [한국어] raw ioprio 또는 LSM 오류 코드를 반환 - SYSCALL_DEFINE2(ioprio_get)의 PROCESS 케이스가 그대로 사용자공간에 전달한다 */
}

/*
 * [한국어]
 * ioprio_best() - 두 ioprio 값 중 더 높은 우선순위(숫자가 더 작은 값)를
 * 고르는 비교기.
 *
 * @aprio: 비교할 첫 번째 ioprio 값(보통 지금까지 집계된 최고 우선순위).
 * @bprio: 비교할 두 번째 ioprio 값(보통 방금 조회한 개별 태스크의 값).
 * @return: aprio와 bprio 중 숫자가 더 작은 쪽(=더 높은 우선순위).
 *
 * 배경(Why): ioprio 인코딩(IOPRIO_PRIO_VALUE)은 상위 비트의 클래스와 하위
 * 비트의 레벨을 이어붙인 값이며, RT(1) < BE(2) < IDLE(3) 순으로 클래스 값이
 * 커지고, 같은 클래스 안에서도 레벨이 작을수록 우선순위가 높다. 따라서 두
 * 값을 대소 비교하는 것만으로 "더 급한 쪽"을 정확히 가려낼 수 있어, 별도의
 * 클래스/레벨 분해 로직 없이 min() 하나로 충분하다.
 * 동작(What): unsigned short로 받은 두 값을 min()(include/linux/kernel.h 등
 * 매크로/인라인)으로 비교해 그대로 반환한다.
 * 실행 컨텍스트: 부작용이 전혀 없는 순수 비교 함수 - 어떤 락도 필요 없다.
 * 호출자(Who calls): SYSCALL_DEFINE2(ioprio_get)의 PGRP/USER 케이스가 그룹/
 * 사용자에 속한 여러 태스크의 값을 하나의 대표값으로 접을 때마다 호출한다.
 * 피호출자(Who is called): min() 매크로/인라인 하나뿐.
 * 에러 경로(Error path): 없음 - 입력이 이미 유효한 ioprio 값이라고 가정한다
 * (호출자가 tmpio < 0인 오류값은 걸러내고 넘겨준다).
 *
 * 호출 체인:
 *   SYSCALL_DEFINE2(ioprio_get) -> [ioprio_best] -> min()
 */
static int ioprio_best(unsigned short aprio, unsigned short bprio)
{
	return min(aprio, bprio);	/* [한국어] ioprio 인코딩은 값이 작을수록 우선순위가 높으므로, 두 값 중 작은 쪽(min)이 곧 "더 높은 우선순위"가 된다 */
}

/*
 * [한국어]
 * SYSCALL_DEFINE2(ioprio_get, which, who) - ioprio_get(2) 시스템 콜 본체.
 * 지정한 프로세스/프로세스 그룹/사용자에 대한 I/O 우선순위를 조회한다.
 *
 * @which: 대상의 종류. IOPRIO_WHO_PROCESS/PGRP/USER 중 하나 - ioprio_set과
 *         동일한 enum을 공유한다.
 * @who: which에 따라 해석되는 대상 식별자. 0이면 호출자 자신(또는 자신이
 *       속한 그룹/사용자).
 * @return: 0 이상이면 조회된 ioprio 값(PROCESS는 raw 값, PGRP/USER는 집계된
 *          effective 대표값). 음수면 -ESRCH(대상 없음)/-EINVAL(잘못된
 *          which)/LSM이 거부한 오류 코드 중 하나.
 *
 * 배경(Why): ionice(1) -p/-P 등으로 현재 설정을 조회하거나, 스케줄링 진단
 * 도구가 특정 프로세스(군)의 I/O 서비스 등급을 확인할 때 쓰는 조회 전용
 * 인터페이스다. ioprio_set()과 대칭을 이루는 구조이지만, PROCESS와
 * PGRP/USER가 서로 다른 조회 함수(get_task_raw_ioprio vs get_task_ioprio)를
 * 쓴다는 점이 ioprio_set()과의 핵심 차이다.
 * 동작(What) 단계별 설명:
 *   1) rcu_read_lock()으로 탐색 구간을 보호하며 which로 분기.
 *   2) PROCESS: find_task_by_vpid()로 단일 태스크를 찾아
 *      get_task_raw_ioprio() 한 번 호출 - 역사적 호환성을 위해 raw 값을
 *      그대로 반환(설정 안 했으면 IOPRIO_DEFAULT).
 *   3) PGRP: tasklist_lock 아래 do_each_pid_thread()로 그룹을 순회하며
 *      각 태스크의 get_task_ioprio()(effective 값)를 얻고, ret==-ESRCH
 *      (아직 결과 없음 센티널)이면 첫 값으로 초기화, 아니면 ioprio_best()로
 *      기존 값과 비교해 더 높은 우선순위를 유지한다. 개별 태스크 조회가
 *      음수(LSM 거부 등)면 그 태스크만 건너뛰고 나머지는 계속 집계한다
 *      (best-effort).
 *   4) USER: for_each_process_thread()로 시스템 전체를 순회하며 UID가
 *      일치하는 태스크만 PGRP와 동일한 방식으로 집계한다.
 * 실행 컨텍스트: 시스템 콜 호출 프로세스 컨텍스트에서 동기 실행. ioprio_set()
 * 과 달리 이 함수에는 goto로 락을 건너뛰는 조기 종료 경로가 없어(모든 case가
 * 정상적으로 break로 빠져나옴) 별도의 out: 레이블이 없다.
 * 호출자(Who calls): 사용자공간 ioprio_get(2) libc 래퍼.
 * 피호출자(Who is called): get_task_raw_ioprio(), get_task_ioprio(),
 * ioprio_best(), 그리고 find_task_by_vpid()/task_pgrp()/find_vpid()/
 * make_kuid()/find_user()/current_user() 등 탐색 함수.
 * 에러 경로(Error path): 각 case에서 대상을 못 찾으면 ret은 초기값 -ESRCH를
 * 유지한 채 switch를 빠져나가고, 함수 끝에서 그대로 반환된다 - ioprio_set()
 * 처럼 중간에 goto로 튀어나가는 실패 경로는 없다(조회는 실패해도 다른
 * 자원을 정리할 필요가 없기 때문).
 *
 * 호출 체인:
 *   사용자공간 ionice(1) -p -> ioprio_get(2) -> [SYSCALL_DEFINE2(ioprio_get)]
 *   -> get_task_raw_ioprio() / (get_task_ioprio() + ioprio_best()) 반복
 *   -> (결과) 사용자공간에 ioprio 값 반환
 */
SYSCALL_DEFINE2(ioprio_get, int, which, int, who)	/* [한국어] ioprio_get(2) 시스템 콜 진입점 매크로 - 확장되면 asmlinkage long sys_ioprio_get(int which, int who)가 된다 */
{
	struct task_struct *g, *p;	/* [한국어] g는 USER 케이스의 스레드 그룹 리더 순회 변수, p는 PROCESS/PGRP/USER 공통으로 쓰이는 대상 태스크(또는 순회) 변수 */
	struct user_struct *user;	/* [한국어] IOPRIO_WHO_USER 케이스의 대상 user_struct - find_user() 경로일 때만 함수 끝에서 free_uid() 필요 */
	struct pid *pgrp;	/* [한국어] IOPRIO_WHO_PGRP 케이스의 대상 프로세스 그룹 */
	kuid_t uid;	/* [한국어] IOPRIO_WHO_USER 케이스에서 who를 변환한 kuid_t 값 */
	int ret = -ESRCH;	/* [한국어] 기본값 "대상 없음" 오류로 초기화 - PGRP/USER 케이스에서는 이후 이 값이 "아직 유효한 결과를 하나도 못 얻음"의 센티널로도 재사용된다(아래 tmpio 비교부 참고) */
	int tmpio;	/* [한국어] PGRP/USER 케이스에서 개별 태스크의 effective ioprio를 임시로 담아 ioprio_best()로 집계하기 위한 변수 */

	rcu_read_lock();	/* [한국어] 태스크 탐색/순회 동안 RCU 보호 시작 - ioprio_set()과 동일한 이유 */
	switch (which) {	/* [한국어] which(PROCESS/PGRP/USER)에 따라 조회 대상과 집계 방식이 다르므로 분기 */
		case IOPRIO_WHO_PROCESS:	/* [한국어] 단일 태스크의 raw ioprio 조회 */
			/* [한국어] who==0이면 호출자 자신 */
			if (!who)
				p = current;	/* [한국어] 현재 태스크를 대상으로 */
			else
				p = find_task_by_vpid(who);	/* [한국어] who를 vpid로 해석해 대상 태스크 검색 */
			/* [한국어] 태스크를 찾은 경우에만 조회 - 못 찾으면 ret은 -ESRCH 유지 */
			if (p)
				ret = get_task_raw_ioprio(p);	/* [한국어] PROCESS 케이스는 역사적 호환성을 위해 effective 값이 아니라 raw 값(get_task_ioprio가 아님)을 반환한다 - 미설정과 설정된 기본값을 사용자공간이 구분할 수 있도록 */
			break;	/* [한국어] PROCESS 케이스 종료 */
		case IOPRIO_WHO_PGRP:	/* [한국어] 프로세스 그룹 전체를 조회해 대표값(최고 우선순위)으로 집계 */
			/* [한국어] who==0이면 호출자가 속한 그룹 */
			if (!who)
				pgrp = task_pgrp(current);	/* [한국어] 현재 태스크의 프로세스 그룹 획득 */
			else
				pgrp = find_vpid(who);	/* [한국어] who를 vpid로 해석해 그룹 조회 */
			read_lock(&tasklist_lock);	/* [한국어] do_each_pid_thread 순회 동안 tasklist_lock으로 보호 - ioprio_set()의 PGRP 케이스와 동일한 이유 */
			do_each_pid_thread(pgrp, PIDTYPE_PGID, p) {	/* [한국어] 그룹 내 모든 태스크를 p로 순회 */
				tmpio = get_task_ioprio(p);	/* [한국어] 각 태스크의 effective ioprio 조회 - get_task_raw_ioprio가 아니라 nice 기반 유도까지 포함하는 get_task_ioprio를 사용한다(집계 시에는 raw/미설정 구분보다 실제 서비스 등급이 더 의미있기 때문으로 추정) */
				if (tmpio < 0)	/* [한국어] 개별 태스크 조회가 LSM 등에 의해 거부(음수 오류코드)된 경우 */
					continue;	/* [한국어] 집계에서 제외하고 다음 태스크로 - 그룹 전체가 실패 처리되지 않도록 관대하게(best-effort) 넘어간다 */
				if (ret == -ESRCH)	/* [한국어] 아직 유효한 결과를 하나도 못 얻은 첫 태스크인지 확인 - -ESRCH를 "결과 없음" 센티널로 재사용 */
					ret = tmpio;	/* [한국어] 첫 유효 결과로 초기화 */
				else
					ret = ioprio_best(ret, tmpio);	/* [한국어] 기존 최고 우선순위와 비교해 더 높은(숫자가 작은) 쪽을 유지 */
			} while_each_pid_thread(pgrp, PIDTYPE_PGID, p);	/* [한국어] 순회 종료 매크로 */
			read_unlock(&tasklist_lock);	/* [한국어] tasklist_lock 해제 */

			break;	/* [한국어] PGRP 케이스 종료 */
		case IOPRIO_WHO_USER:	/* [한국어] 특정 UID 소속 모든 태스크를 조회해 대표값으로 집계 */
			uid = make_kuid(current_user_ns(), who);	/* [한국어] who를 현재 네임스페이스 기준 kuid_t로 변환 - ioprio_set()과 달리 여기서는 uid_valid() 검사를 별도로 하지 않고 바로 아래 find_user()/current_user() 결과(존재 여부)로 유효성을 판단한다 */
			/* [한국어] who==0이면 호출자 자신의 사용자 */
			if (!who)
				user = current_user();	/* [한국어] 현재 cred의 user_struct를 차용(참조카운트 증가 없음) */
			else
				user = find_user(uid);	/* [한국어] uid에 해당하는 user_struct 검색(참조카운트 증가) - 없으면 NULL, 있으면 아래에서 반드시 free_uid() 필요 */

			/* [한국어] 해당 UID의 user_struct가 없으면 조회할 대상이 없다 */
			if (!user)
				break;	/* [한국어] ret은 -ESRCH 유지 */

			for_each_process_thread(g, p) {	/* [한국어] 시스템 전체 태스크를 RCU 보호 아래 순회 - ioprio_set()의 USER 케이스와 동일한 패턴 */
				if (!uid_eq(task_uid(p), user->uid) ||	/* [한국어] 태스크의 실효 UID가 대상과 다르면 건너뜀 - ioprio_set()과 달리 앞서 만든 uid 변수 대신 user->uid와 비교한다(동일한 값이어야 함) */
				    !task_pid_vnr(p))	/* [한국어] 또는 호출자 네임스페이스에서 보이지 않는 태스크면 건너뜀 */
					continue;	/* [한국어] 조건 불일치 시 다음 태스크로 */
				tmpio = get_task_ioprio(p);	/* [한국어] effective ioprio 조회 */
				if (tmpio < 0)	/* [한국어] 조회 실패(LSM 거부 등) 시 */
					continue;	/* [한국어] 집계 대상에서 제외 */
				if (ret == -ESRCH)	/* [한국어] 첫 유효 결과인지 확인 */
					ret = tmpio;	/* [한국어] 첫 유효 결과로 초기화 */
				else
					ret = ioprio_best(ret, tmpio);	/* [한국어] 기존 최고 우선순위와 비교해 갱신 */
			}

			/* [한국어] who==0(current_user 경로)이면 해제하면 안 되므로, find_user() 경로(who!=0)일 때만 해제한다 */
			if (who)
				free_uid(user);	/* [한국어] find_user()가 증가시킨 참조카운트 해제 */
			break;	/* [한국어] USER 케이스 종료 */
		default:	/* [한국어] 정의되지 않은 which 값 */
			ret = -EINVAL;	/* [한국어] 잘못된 which 인자 오류 */
	}

	rcu_read_unlock();	/* [한국어] RCU 보호 구간 종료 - ioprio_set()과 달리 이 함수는 PGRP 케이스에서도 goto로 락을 건너뛰는 경로가 없어 별도의 out: 레이블이 없다 */
	return ret;	/* [한국어] 최종 결과(단일 raw 값 또는 집계된 effective 값, 혹은 오류코드)를 사용자공간에 반환 */
}
