// SPDX-License-Identifier: GPL-2.0
/*
 * Block rq-qos policy for assigning an I/O priority class to requests.
 *
 * Using an rq-qos policy for assigning I/O priority class has two advantages
 * over using the ioprio_set() system call:
 *
 * - This policy is cgroup based so it has all the advantages of cgroups.
 * - While ioprio_set() does not affect page cache writeback I/O, this rq-qos
 *   controller affects page cache writeback I/O for filesystems that support
 *   assiociating a cgroup with writeback I/O. See also
 *   Documentation/admin-guide/cgroup-v2.rst.
 */
/*
 * [한국어 설명] blkcg 기반 I/O 우선순위(io.prio.class) 강제 정책의 실제 구현체 (blk-ioprio.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 block/blk-ioprio.h가 선언만 해 두는 blkcg_set_ioprio()의 유일한 구현과,
 * 그 뒤에서 동작하는 blk-cgroup(Block I/O Control Group) 정책 "io.prio.class"의
 * 전체 로직을 담고 있다. 이 정책은 cgroup 하나마다 POLICY_NO_CHANGE/
 * POLICY_PROMOTE_TO_RT(및 그 별칭 POLICY_NONE_TO_RT)/POLICY_RESTRICT_TO_BE/
 * POLICY_ALL_TO_IDLE 중 하나의 값을 저장해 두었다가, 그 cgroup 소속 bio가 제출될
 * 때마다 bio->bi_ioprio(IOPRIO_CLASS_RT/BE/IDLE로 인코딩된 값)를 정책에 맞게
 * 강제로 덮어쓴다. 파일 맨 위 원본 영어 주석이 설명하듯, 이 방식은 태스크 단위로만
 * 적용되는 ioprio_set(2) 시스템 콜과 달리 (1) cgroup 계층 구조를 그대로 활용할 수
 * 있고, (2) ioprio_set()이 건드리지 못하는 페이지 캐시 writeback I/O(커널 kworker가
 * 대신 제출하는 dirty page flush 등)에도 우선순위를 강제할 수 있다는 두 가지 이점이
 * 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 block/blk-ioprio.h가 선언하는 blkcg_set_ioprio()의 유일한 구현
 * 번역단위(translation unit)이며, CONFIG_BLK_CGROUP_IOPRIO=y일 때만 커널 빌드에
 * 포함된다(=n이면 이 .c 파일 자체가 빌드되지 않고, 헤더의 no-op 인라인이 대신
 * 쓰인다). bio 제출 경로에서의 호출 체인은 다음과 같다:
 *   (파일시스템/direct I/O/writeback 등에서 bio 생성)
 *   -> submit_bio_noacct() -> submit_bio_noacct_nocheck()
 *   -> blkcg_set_ioprio(bio)                              [본 파일, blkcg_set_ioprio()]
 *        -> blkcg_to_ioprio_blkcg(bio->bi_blkg->blkcg)     [본 파일, static 헬퍼]
 *   -> (bio->bi_ioprio가 cgroup 정책값으로 갱신될 수 있음)
 *   -> blk_mq_submit_bio() -> blk_mq_get_request() (request->ioprio에 복사)
 *   -> I/O 스케줄러(mq-deadline/bfq/kyber)가 디스패치 순서 결정 시 참고
 *   -> (NVMe 등 블록 드라이버) ->queue_rq() 콜백까지 ioprio가 전달될 수 있음.
 * 별도로, cgroup 관리자가 "io.prio.class" sysfs 파일을 열람/기록하는 흐름도 이
 * 파일에 구현되어 있다:
 *   cgroupfs read()  -> kernfs -> seq_show -> ioprio_show_prio_policy() [본 파일]
 *   cgroupfs write() -> kernfs -> cftype.write -> ioprio_set_prio_policy() [본 파일]
 * 실행 컨텍스트는 크게 두 갈래다 - (1) blkcg_set_ioprio()는 I/O를 제출하는
 * 프로세스(태스크) 컨텍스트에서 동기적으로 실행되고, (2) show/write 콜백은
 * cgroupfs 파일을 여닫는 사용자 프로세스의 시스템 콜 컨텍스트에서 kernfs 자체
 * 잠금(kernfs_open_file 단위 직렬화) 아래 실행된다. 두 경로 모두 인터럽트나
 * softirq 컨텍스트에서는 호출되지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * - block/blk-ioprio.h: 이 파일이 구현하는 blkcg_set_ioprio()의 유일한 공개
 *   선언부. CONFIG_BLK_CGROUP_IOPRIO=n일 때는 그 헤더의 no-op 인라인 함수가
 *   이 파일 전체를 대신한다.
 * - block/blk-cgroup.h, block/blk-cgroup.c: cgroup 계층 구조(struct blkcg),
 *   정책 등록 테이블(blkcg_policy_register()/blkcg_policy_unregister()),
 *   per-cgroup 정책 데이터 컨테이너(struct blkcg_policy_data, blkcg_to_cpd())를
 *   제공하는 상위 인프라. 이 파일의 모든 함수는 이 인프라가 요구하는 훅
 *   (cpd_alloc_fn/cpd_free_fn, cftype.seq_show/write)을 채워 넣는 형태로만
 *   blk-cgroup 코어와 연결된다 - 즉 이 파일은 blk-cgroup이 정의하는 "정책
 *   플러그인" 인터페이스의 구현체 중 하나일 뿐, cgroup 계층 순회나 css
 *   생명주기 관리 같은 공통 로직은 전혀 직접 구현하지 않는다.
 * - block/blk-rq-qos.h: 이 파일 맨 위 원본 주석이 "rq-qos policy"라고 표현하는
 *   근거가 되는 QoS(Quality of Service) 플러그인 프레임워크 헤더. 다만 이 파일은
 *   실제로는 rq_qos_ops(throttle/track/issue/done 등 콜백)를 구현해 등록하지
 *   않고, blkcg_set_ioprio()가 bio 제출 경로에서 직접 호출되는 더 단순한 형태로
 *   연결된다 - include는 하지만 이 파일 안에서 rq_qos_* 계열 심볼을 직접 쓰지는
 *   않는다.
 * - include/uapi/linux/ioprio.h, include/linux/ioprio.h: IOPRIO_CLASS_NONE/RT/
 *   BE/IDLE 상수와 IOPRIO_PRIO_CLASS()/IOPRIO_PRIO_VALUE() 인코딩 매크로를
 *   제공한다. bio->bi_ioprio는 상위 비트에 클래스(0~7 중 NONE=0/RT=1/BE=2/
 *   IDLE=3만 사용), 하위 비트에 클래스 내 우선순위 레벨을 담는 16비트 값이며,
 *   이 파일의 핵심 로직(blkcg_set_ioprio)은 전부 이 인코딩을 전제로 값을
 *   비교·조립한다. 뒤에서 보듯 enum prio_policy의 RESTRICT_TO_BE=2/ALL_TO_IDLE=3
 *   값은 이 헤더의 IOPRIO_CLASS_BE=2/IOPRIO_CLASS_IDLE=3과 의도적으로 동일한
 *   숫자로 맞춰져 있다.
 * - block/blk-mq.c 및 I/O 스케줄러(block/mq-deadline.c, block/bfq-iosched.c,
 *   block/kyber-iosched.c): bio->bi_ioprio가 request->ioprio로 전파된 뒤 이
 *   값을 디스패치 순서 결정에 참고하는 소비자들. 이 파일은 그 소비자들보다
 *   앞 단계에서 값을 "확정"짓는 생산자 역할만 하고, 스케줄링 자체에는 관여하지
 *   않는다.
 * 데이터 흐름 한 줄 요약: cgroupfs "io.prio.class" 파일 write() ->
 * ioprio_set_prio_policy()가 struct ioprio_blkcg.prio_policy 갱신 -> (다음
 * bio 제출부터) blkcg_set_ioprio()가 그 값을 읽어 bio->bi_ioprio에 반영 ->
 * request->ioprio로 전파 -> 스케줄러/드라이버가 소비.
 * 공유 핵심 자료구조: struct ioprio_blkcg(cgroup 1개당 1개, blk-cgroup 코어가
 * blkcg->cpd[plid] 배열에 보관)가 이 파일의 모든 함수가 공유하는 유일한 상태다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ioprio_blkcg: cgroup 하나에 대응하는 이 정책의 전용 데이터. 공통
 *   헤더 cpd와, 실제 정책 값 prio_policy 두 필드만 가진다. cpd가 구조체의 첫
 *   필드이므로 container_of(NULL, ...)이 그대로 NULL을 반환한다는 성질이
 *   blkcg_set_ioprio()의 NULL 체크 로직을 성립시키는 핵심 전제다.
 * - enum prio_policy / policy_name[]: "io.prio.class" sysfs 파일에 쓸 수 있는
 *   5가지 정책 값과, 그 값을 사용자에게 문자열로 보여주기 위한 이름 테이블.
 *   두 배열은 인덱스가 1:1로 대응해야 하므로 항상 함께 수정되어야 한다.
 * - blkcg_to_ioprio_blkcg()/ioprio_blkcg_from_css(): struct blkcg 또는
 *   cgroup_subsys_state로부터 이 정책 전용 데이터(struct ioprio_blkcg)를
 *   꺼내는 두 단계의 헬퍼 체인.
 * - ioprio_show_prio_policy()/ioprio_set_prio_policy(): "io.prio.class"
 *   cgroupfs 파일의 read/write 콜백. cftype.seq_show/write에 등록된다.
 * - ioprio_alloc_cpd()/ioprio_free_cpd(): cgroup이 생성/소멸될 때 struct
 *   ioprio_blkcg 메모리를 할당/해제하는 생명주기 콜백(cpd_alloc_fn/cpd_free_fn).
 * - blkcg_set_ioprio(): 이 파일의 핵심 진입점. bio 하나에 대해 cgroup 정책을
 *   조회하고 조건에 맞게 bio->bi_ioprio를 덮어쓴다.
 * - ioprio_init()/ioprio_exit(): 모듈 적재/제거 시 ioprio_policy를 blk-cgroup
 *   정책 테이블에 등록/해제하는 module_init/module_exit 훅.
 */

#include <linux/blk-mq.h>
/* [한국어] blk-mq.h: struct request, request_queue 등 blk-mq 계층 선언을 제공한다.
 *         이 파일은 request를 직접 다루지 않지만, bio->bi_ioprio가
 *         request->ioprio로 전파되는 후속 단계(blk_mq_get_request())와의 연결
 *         고리를 이해하는 데 필요한 타입 선언들이 여기서 들어온다. */

#include <linux/blk_types.h>
/* [한국어] blk_types.h: struct bio와 bio->bi_ioprio, bio->bi_blkg 필드 등 이 파일이
 *         직접 조작하는 bio의 실제 레이아웃을 제공한다. blkcg_set_ioprio()가
 *         bio->bi_blkg->blkcg와 bio->bi_ioprio에 접근하려면 반드시 필요하다. */

#include <linux/kernel.h>
/* [한국어] kernel.h: max_t() 등 커널 전반의 공용 매크로를 제공한다.
 *         blkcg_set_ioprio()가 우선순위 비교에 사용하는 max_t(u16, ...)가
 *         여기서 온다. */

#include <linux/module.h>
/* [한국어] module.h: module_init()/module_exit() 매크로를 제공한다. 이 파일
 *         맨 아래에서 ioprio_init()/ioprio_exit()을 모듈 적재/제거 훅으로
 *         등록하는 데 쓰인다. */

#include "blk-cgroup.h"
/* [한국어] blk-cgroup.h: struct blkcg, struct blkcg_policy, struct
 *         blkcg_policy_data, blkcg_to_cpd()/css_to_blkcg() 등 이 파일이
 *         의존하는 cgroup 정책 프레임워크 전체를 제공한다. 이 파일의 모든
 *         함수는 이 헤더가 정의하는 인터페이스(cpd_alloc_fn/cpd_free_fn 등)를
 *         구현하는 형태로만 존재한다. */

#include "blk-ioprio.h"
/* [한국어] blk-ioprio.h: 이 파일이 구현하는 blkcg_set_ioprio(struct bio *)의
 *         선언이 담긴 자기 자신의 공개 헤더. CONFIG_BLK_CGROUP_IOPRIO=y일 때
 *         이 헤더의 선언과 이 파일의 정의가 링크되어야 하므로 반드시
 *         include한다. */

#include "blk-rq-qos.h"
/* [한국어] blk-rq-qos.h: rq-qos(Request Queue Quality of Service) 플러그인
 *         프레임워크 헤더. 파일 상단 원본 주석이 이 정책을 "rq-qos policy"라고
 *         부르는 근거가 되는 헤더이지만, 이 파일은 실제로 rq_qos_ops(throttle/
 *         track/issue/done 콜백)를 채워 등록하지는 않는다 - blkcg_set_ioprio()가
 *         bio 제출 경로에서 직접 호출되는 더 단순한 연결 방식을 쓴다. */

/**
 * enum prio_policy - I/O priority class policy.
 * @POLICY_NO_CHANGE: (default) do not modify the I/O priority class.
 * @POLICY_PROMOTE_TO_RT: modify no-IOPRIO_CLASS_RT to IOPRIO_CLASS_RT.
 * @POLICY_RESTRICT_TO_BE: modify IOPRIO_CLASS_NONE and IOPRIO_CLASS_RT into
 *		IOPRIO_CLASS_BE.
 * @POLICY_ALL_TO_IDLE: change the I/O priority class into IOPRIO_CLASS_IDLE.
 * @POLICY_NONE_TO_RT: an alias for POLICY_PROMOTE_TO_RT.
 *
 * See also <linux/ioprio.h>.
 */
/*
 * [한국어]
 * enum prio_policy - cgroup의 "io.prio.class" sysfs 파일에 쓸 수 있는 정책 값의
 * 집합. 이 값은 struct ioprio_blkcg.prio_policy 필드에 저장되며,
 * blkcg_set_ioprio()가 bio->bi_ioprio를 덮어쓸지 여부와 방식을 결정하는 유일한
 * 입력이다. 사용자는 이 enum 값을 숫자로 직접 쓰는 것이 아니라, 아래
 * policy_name[] 배열에 대응하는 문자열("no-change", "promote-to-rt",
 * "restrict-to-be", "idle", "none-to-rt")을 cgroupfs의 "io.prio.class"
 * 파일에 write()하여 간접적으로 설정한다(ioprio_set_prio_policy() 참고).
 * 중요한 설계 포인트: POLICY_RESTRICT_TO_BE(=2)와 POLICY_ALL_TO_IDLE(=3)의
 * 숫자값은 <linux/ioprio.h>의 IOPRIO_CLASS_BE(=2)/IOPRIO_CLASS_IDLE(=3)와
 * 의도적으로 동일하게 맞춰져 있다. blkcg_set_ioprio()의 max_t() 계산에서
 * blkcg->prio_policy 값을 별도의 매핑 테이블 없이 곧바로 IOPRIO_PRIO_VALUE()의
 * 클래스 인자로 재사용하기 때문이다. 반면 POLICY_NO_CHANGE(=0)/
 * POLICY_PROMOTE_TO_RT(=1)/POLICY_NONE_TO_RT(=4)는 blkcg_set_ioprio() 앞부분의
 * 전용 분기에서만 다뤄지며 IOPRIO_CLASS_* 값으로 재사용되지 않는다.
 */
enum prio_policy {
	POLICY_NO_CHANGE	= 0,
	/* [한국어] 기본값 - cgroup 생성 시 ioprio_alloc_cpd()가 이 값으로 초기화한다.
	 * 설정자: ioprio_alloc_cpd()(초기화), ioprio_set_prio_policy()가 사용자가
	 *   "echo no-change > io.prio.class"를 쓸 때 다시 이 값으로 되돌릴 수도 있다.
	 * 읽는 자: blkcg_set_ioprio()가 이 값을 보면 bio->bi_ioprio를 전혀 건드리지
	 *   않고 즉시 return한다 - 즉 태스크가 ioprio_set()으로 설정한 값이나 기본
	 *   IOPRIO_CLASS_NONE이 그대로 유지된다.
	 * 값 범위: 정수 0. policy_name[0] == "no-change"와 1:1 대응한다.
	 * 동기화: 단일 enum 값 대입/비교이므로 별도 락 없이 워드 단위 원자성에
	 *   의존한다(아래 prio_policy 필드 주석 참고). */

	POLICY_PROMOTE_TO_RT	= 1,
	/* [한국어] "이 cgroup의 모든 I/O는 최소 RT(Real Time) 클래스가 되어야 한다"는
	 * 정책. blkcg_set_ioprio()에서 POLICY_NONE_TO_RT(=4)와 완전히 동일한 코드
	 * 경로로 처리된다(값은 다르지만 취급은 동일) - 이미 IOPRIO_CLASS_RT인 bio는
	 * 건드리지 않고, 그렇지 않은 bio만 IOPRIO_PRIO_VALUE(IOPRIO_CLASS_RT, 4)로
	 * 승격시킨다.
	 * 설정자: ioprio_set_prio_policy()가 "promote-to-rt" 문자열을 받았을 때.
	 * 읽는 자: blkcg_set_ioprio()의 첫 번째 if 분기.
	 * 값 범위: 정수 1. IOPRIO_CLASS_RT(=1)와 우연히 같은 숫자이지만, 이 파일의
	 *   코드는 이 값을 IOPRIO_CLASS_*로 재사용하지 않고 오직 "==" 비교로만
	 *   사용하므로 두 상수의 일치는 의미상 우연이며 의존하지 않는다. */

	POLICY_RESTRICT_TO_BE	= 2,
	/* [한국어] "이 cgroup의 I/O는 최대 BE(Best Effort) 클래스까지만 허용한다"는
	 * 정책 - IOPRIO_CLASS_NONE과 IOPRIO_CLASS_RT를 IOPRIO_CLASS_BE로 낮추되(RT는
	 * BE로 강등, NONE은 BE로 승격), 이미 BE/IDLE인 bio는 그대로 둔다(단, IDLE인
	 * bio는 max_t()에 의해 여전히 IDLE로 남는다 - 아래 blkcg_set_ioprio 참고).
	 * 설정자: ioprio_set_prio_policy()가 "restrict-to-be" 문자열을 받았을 때.
	 * 읽는 자: blkcg_set_ioprio()의 max_t() 계산에서 IOPRIO_PRIO_VALUE()의
	 *   클래스 인자로 값 자체가 직접 재사용된다.
	 * 값 범위: 정수 2. IOPRIO_CLASS_BE(=2)와 의도적으로 동일한 숫자 - 이 일치가
	 *   깨지면 blkcg_set_ioprio()의 max_t() 계산이 잘못된 클래스를 적용하게
	 *   되므로 두 enum을 함께 수정해야 하는 강한 결합 관계다. */

	POLICY_ALL_TO_IDLE	= 3,
	/* [한국어] "이 cgroup의 모든 I/O를 IDLE 클래스로 낮춘다"는 가장 공격적인
	 * 정책 - 기존 클래스가 무엇이었든(NONE/RT/BE/IDLE) 상관없이 IDLE로
	 * 강등된다(디스크가 유휴 상태일 때만 서비스됨을 의미).
	 * 설정자: ioprio_set_prio_policy()가 "idle" 문자열을 받았을 때.
	 * 읽는 자: blkcg_set_ioprio()의 max_t() 계산에서 IOPRIO_PRIO_VALUE()의
	 *   클래스 인자로 값 자체가 직접 재사용된다.
	 * 값 범위: 정수 3. IOPRIO_CLASS_IDLE(=3)와 의도적으로 동일한 숫자 - POLICY_
	 *   RESTRICT_TO_BE와 마찬가지로 IOPRIO_CLASS_IDLE 값과 강하게 결합되어 있다. */

	POLICY_NONE_TO_RT	= 4,
	/* [한국어] POLICY_PROMOTE_TO_RT의 별칭(alias) - 커널 문서(위 kernel-doc)가
	 * 명시하듯 의미상 동일한 정책을 가리키는 또 다른 이름이며, 값 자체는
	 * 1이 아니라 4로 다르다(별칭이지만 별도의 enum 상수). blkcg_set_ioprio()는
	 * 두 값을 "||"로 나란히 검사하여 완전히 동일하게 처리하므로, 사용자가
	 * "promote-to-rt"와 "none-to-rt" 중 어느 이름으로 설정하든 실제 동작
	 * 차이는 없다(하위 호환을 위해 두 이름을 모두 남겨둔 것으로 보인다).
	 * 설정자: ioprio_set_prio_policy()가 "none-to-rt" 문자열을 받았을 때.
	 * 읽는 자: blkcg_set_ioprio()의 첫 번째 if 분기(POLICY_PROMOTE_TO_RT와
	 *   동일한 조건절에서 "||"로 함께 검사됨).
	 * 값 범위: 정수 4. IOPRIO_CLASS_* 어떤 상수와도 겹치지 않는 값이며, 이
	 *   파일에서 IOPRIO_CLASS_*로 재사용되지 않는다. */
};

static const char *policy_name[] = {
	/* [한국어] 전역 배열 - enum prio_policy 값(정수 인덱스)을 사용자에게 보여줄
	 * 문자열로 변환하는 테이블이자, 동시에 ioprio_set_prio_policy()가
	 * sysfs_match_string()으로 "문자열 -> 인덱스(enum 값)" 역방향 변환을 할 때도
	 * 쓰이는 유일한 정의 지점이다(양방향 변환에 이 배열 하나만 사용). 지정자
	 * 초기화([POLICY_NO_CHANGE] = ...)를 쓰기 때문에 enum 선언 순서와 무관하게
	 * 항상 올바른 인덱스에 문자열이 들어간다.
	 * 동기화: static const로 런타임에 절대 수정되지 않으므로 락이 필요 없다. */
	[POLICY_NO_CHANGE]	= "no-change",
	/* [한국어] POLICY_NO_CHANGE(=0)에 대응하는 사용자 표시 문자열.
	 * 읽는 자: ioprio_show_prio_policy()(seq_printf 출력), ioprio_set_prio_policy()
	 *   (sysfs_match_string()의 비교 대상). */

	[POLICY_PROMOTE_TO_RT]	= "promote-to-rt",
	/* [한국어] POLICY_PROMOTE_TO_RT(=1)에 대응하는 사용자 표시 문자열. */

	[POLICY_RESTRICT_TO_BE]	= "restrict-to-be",
	/* [한국어] POLICY_RESTRICT_TO_BE(=2)에 대응하는 사용자 표시 문자열. */

	[POLICY_ALL_TO_IDLE]	= "idle",
	/* [한국어] POLICY_ALL_TO_IDLE(=3)에 대응하는 사용자 표시 문자열 - 다른
	 * 항목과 달리 정책 이름과 IOPRIO_CLASS 이름이 그대로 "idle"로 일치한다. */

	[POLICY_NONE_TO_RT]	= "none-to-rt",
	/* [한국어] POLICY_NONE_TO_RT(=4)에 대응하는 사용자 표시 문자열 - "promote-to-rt"의
	 * 하위 호환용 별칭 이름. */
};

static struct blkcg_policy ioprio_policy;
/* [한국어] 전역 정책 객체의 잠정 정의(tentative definition) - 초기화식 없이
 * 선언만 해 두어, 아래 blkcg_to_ioprio_blkcg() 등 여러 함수가 &ioprio_policy를
 * "먼저" 참조할 수 있게 한다. C 언어 규칙상 파일 스코프 객체의 잠정 정의는
 * 나중에 나오는 초기화식이 있는 완전한 정의(이 파일 뒤쪽의
 * "static struct blkcg_policy ioprio_policy = { ... };")와 같은 저장 공간을
 * 공유하도록 병합된다 - 즉 이 줄과 뒤쪽의 정의는 서로 다른 두 객체가 아니라
 * 하나의 전역 변수를 가리킨다. 이렇게 앞에서 미리 선언해 두는 이유는, C에서는
 * 함수가 자신보다 뒤에 나오는 파일 스코프 변수를 참조할 수 없어서 구조체 본문
 * 정의(cftype 배열 등을 채우려면 그 배열을 정의하는 함수들이 먼저 나와야 함)와
 * 그 구조체를 참조하는 헬퍼 함수들의 선언 순서 문제를 풀기 위해서다.
 * 동기화: 초기화 이후에는 값이 바뀌지 않는(정책 등록 시 plid만 blk-cgroup 코어가
 * 채움) 정적 데이터이므로 별도 락이 필요 없다. */

/**
 * struct ioprio_blkcg - Per cgroup data.
 * @cpd: blkcg_policy_data structure.
 * @prio_policy: One of the IOPRIO_CLASS_* values. See also <linux/ioprio.h>.
 */
/*
 * [한국어]
 * struct ioprio_blkcg - "io.prio.class" 정책이 cgroup 하나당 보관하는 전용 데이터.
 * blk-cgroup 코어는 cgroup(blkcg)마다 정책 슬롯 배열 blkcg->cpd[plid]를 두고,
 * 이 정책이 등록될 때 배정받은 고유 ID(plid)를 인덱스로 이 구조체(정확히는 그
 * 첫 필드인 cpd)를 저장한다. 즉 이 구조체 자체는 cgroup 계층 구조나 참조 카운트
 * 같은 것을 직접 관리하지 않고, "이 cgroup에는 어떤 io.prio.class 정책이 설정돼
 * 있는가"라는 단 하나의 사실만 담는다.
 */
struct ioprio_blkcg {
	struct blkcg_policy_data cpd;
	/* [한국어] blk-cgroup 코어가 모든 정책 데이터에 공통으로 요구하는 헤더
	 * (역참조용 blkcg 포인터, plid 등을 담음). 반드시 구조체의 "첫 번째" 필드여야
	 * 한다 - offsetof(struct ioprio_blkcg, cpd) == 0이 성립해야, cpd 포인터가
	 * NULL일 때 container_of(NULL, struct ioprio_blkcg, cpd)의 결과도 정확히
	 * NULL이 되기 때문이다(포인터 산술상 NULL - 0 == NULL). 이 성질이
	 * blkcg_set_ioprio()의 "if (!blkcg ...)" 검사가 "이 cgroup에는 아직 정책
	 * 데이터가 없음(cpd == NULL)"을 올바르게 감지하게 해 주는 근거다.
	 * 설정자: ioprio_alloc_cpd()가 kzalloc_obj()로 이 구조체 전체를 할당한 뒤
	 *   &blkcg->cpd를 반환하면, 그 반환값을 받은 blk-cgroup 코어
	 *   (blkcg_css_alloc()/blkcg_policy_register())가 cpd.blkcg/cpd.plid
	 *   두 필드를 채운다 - 즉 이 필드 자체의 내용은 이 파일과 blk-cgroup 코어가
	 *   나누어 채우는 구조다.
	 * 읽는 자: blkcg_to_ioprio_blkcg()/ioprio_free_cpd()가 container_of()로
	 *   이 필드를 거쳐 바깥의 struct ioprio_blkcg 전체를 복원한다.
	 * 값 범위: 유효한 blkcg_policy_data(할당 성공 후) 또는 이 필드를 가리키는
	 *   포인터 자체가 NULL(정책 데이터가 아직 없음, 위 설명 참고).
	 * 동기화: blk-cgroup 코어의 blkcg_pol_mutex/cgroup 생명주기 규칙을 따르며,
	 *   이 파일은 별도의 락을 추가로 걸지 않는다. */

	enum prio_policy	 prio_policy;
	/* [한국어] 이 cgroup에 실제로 설정된 io.prio.class 정책 값.
	 * 설정자: ioprio_alloc_cpd()가 cgroup 생성 시 POLICY_NO_CHANGE로 최초
	 *   초기화하고, 이후 ioprio_set_prio_policy()가 사용자의 sysfs write()에
	 *   맞춰 덮어쓴다.
	 * 읽는 자: ioprio_show_prio_policy()가 policy_name[]으로 변환해 사용자에게
	 *   보여주고, blkcg_set_ioprio()가 이 값을 근거로 bio->bi_ioprio를
	 *   덮어쓸지/어떻게 덮어쓸지 결정한다.
	 * 값 범위: enum prio_policy의 5개 값(0~4) 중 하나. sysfs_match_string()이
	 *   policy_name[] 범위를 벗어나는 값은 애초에 거부하므로(음수 반환) 항상
	 *   유효한 값만 저장된다.
	 * 동기화: 명시적인 락이 없다 - ioprio_set_prio_policy()의 write와
	 *   blkcg_set_ioprio()의 read가 다른 CPU에서 동시에 일어날 수 있지만,
	 *   enum(사실상 int) 크기의 정렬된 필드에 대한 단순 대입/읽기는 대부분의
	 *   아키텍처에서 원자적으로 관측되므로(찢어진 값이 보이지 않음) 한
	 *   순간에는 항상 write 이전 값 또는 이후 값 중 하나만 관측된다는
	 *   "best-effort" 수준의 일관성만 보장한다. kernfs 자체는 같은 파일에
	 *   대한 동시 write끼리는 of->mutex로 직렬화하지만, 그 사실이 이 필드를
	 *   읽는 blkcg_set_ioprio() 쪽 동시성까지 막아주지는 않는다. */
};

/*
 * [한국어]
 * blkcg_to_ioprio_blkcg - struct blkcg로부터 이 정책 전용 데이터(struct
 * ioprio_blkcg)를 꺼낸다.
 *
 * @blkcg: 조회 대상 cgroup을 표현하는 blk-cgroup 핵심 구조체. bio->bi_blkg->blkcg
 *         (blkcg_set_ioprio() 호출부) 또는 상위 cgroup 순회 코드에서 전달된다.
 * @return: blkcg에 연결된 struct ioprio_blkcg 포인터. blkcg 자체가 NULL이거나,
 *          blkcg->cpd[plid]가 아직 채워지지 않은 경우(이 정책이 그 cgroup에서
 *          활성화되지 않음) NULL이 반환된다(struct ioprio_blkcg.cpd 필드
 *          주석의 offsetof==0 성질 참고).
 *
 * blk-cgroup 코어는 정책마다 서로 다른 per-cgroup 데이터 타입을 다루므로,
 * blkcg->cpd[] 배열은 공통 타입인 struct blkcg_policy_data*의 배열로
 * 선언되어 있다. 이 함수는 그 공통 타입 포인터를 이 정책만의 구체적인 타입인
 * struct ioprio_blkcg*로 되돌리는 "타입 복원" 역할을 한다.
 * 동작 단계: (1) blkcg_to_cpd(blkcg, &ioprio_policy)로 이 정책(&ioprio_policy,
 * plid로 인덱싱)에 해당하는 blkcg_policy_data* 슬롯을 조회하고, (2)
 * container_of()로 그 포인터가 embed되어 있던 바깥 struct ioprio_blkcg 전체의
 * 시작 주소를 계산해 되돌린다.
 * 실행 컨텍스트: 호출자(ioprio_blkcg_from_css(), blkcg_set_ioprio())의
 * 컨텍스트를 그대로 물려받는다 - 이 함수 자체는 잠들거나 락을 걸지 않는
 * 순수 포인터 산술 함수다.
 * 에러 경로: 실패를 별도로 알리지 않고 NULL을 반환할 뿐이다 - 호출자가 반드시
 * NULL 체크를 해야 한다(blkcg_set_ioprio()의 "if (!blkcg ...)" 참고).
 *
 * 호출 체인:
 *   ioprio_blkcg_from_css() → [blkcg_to_ioprio_blkcg]
 *   blkcg_set_ioprio() → [blkcg_to_ioprio_blkcg]
 */
static struct ioprio_blkcg *blkcg_to_ioprio_blkcg(struct blkcg *blkcg)
{
	/* [한국어] blkcg_to_cpd()가 이 정책(&ioprio_policy, 내부적으로 ->plid 사용)에
	 * 해당하는 blkcg->cpd[] 슬롯을 반환하면(또는 blkcg가 NULL이거나 슬롯이
	 * 비어 있으면 NULL), container_of()로 그 slot 포인터가 가리키는 struct
	 * blkcg_policy_data가 embed된 struct ioprio_blkcg 전체의 주소를 계산한다.
	 * cpd가 구조체의 첫 필드(offset 0)이므로 이 계산은 실질적으로 포인터
	 * 값 자체를 그대로 반환하는 것과 같고, 입력이 NULL이면 결과도 NULL이다. */
	return container_of(blkcg_to_cpd(blkcg, &ioprio_policy),
			    struct ioprio_blkcg, cpd);
}

/*
 * [한국어]
 * ioprio_blkcg_from_css - cgroup_subsys_state(css)로부터 이 정책 전용 데이터를
 * 꺼내는 한 단계 더 상위의 헬퍼.
 *
 * @css: cgroup 서브시스템 상태 포인터. kernfs 파일 접근 경로(seq_css()/of_css())가
 *       "지금 열려 있는 sysfs 파일이 속한 cgroup"을 식별해 전달한다.
 * @return: 그 cgroup에 대응하는 struct ioprio_blkcg 포인터, 또는
 *          blkcg_to_ioprio_blkcg()와 동일한 조건에서 NULL.
 *
 * css_to_blkcg()가 cgroup_subsys_state를 container_of()로 감싸는 struct
 * blkcg로 먼저 되돌리고, 그 결과를 곧바로 blkcg_to_ioprio_blkcg()에 넘겨 최종
 * struct ioprio_blkcg를 얻는 2단계 변환을 한 번의 호출로 묶어 제공한다. cgroupfs
 * 파일 read/write 콜백은 blkcg 구조체가 아니라 cgroup_subsys_state를 인자로
 * 받으므로, 이 헬퍼가 없으면 show/write 콜백마다 두 함수를 중복 호출해야 한다.
 * 실행 컨텍스트: kernfs가 "io.prio.class" 파일의 read()/write() 시스템 콜을
 * 처리하는 프로세스 컨텍스트에서 호출된다.
 * 에러 경로: 별도 처리 없이 blkcg_to_ioprio_blkcg()의 NULL 전파를 그대로
 * 따른다.
 *
 * 호출 체인:
 *   ioprio_show_prio_policy() → [ioprio_blkcg_from_css] → blkcg_to_ioprio_blkcg()
 *   ioprio_set_prio_policy() → [ioprio_blkcg_from_css] → blkcg_to_ioprio_blkcg()
 */
static struct ioprio_blkcg *
ioprio_blkcg_from_css(struct cgroup_subsys_state *css)
{
	/* [한국어] css_to_blkcg(css)가 container_of()로 css를 감싸는 struct blkcg를
	 * 복원하면, 그 blkcg를 즉시 blkcg_to_ioprio_blkcg()에 넘겨 이 정책 전용
	 * 데이터까지 한 번에 얻는다. */
	return blkcg_to_ioprio_blkcg(css_to_blkcg(css));
}

/*
 * [한국어]
 * ioprio_show_prio_policy - "io.prio.class" cgroupfs 파일을 read()할 때 현재
 * 정책 문자열을 출력하는 seq_file show 콜백.
 *
 * @sf: 이 read() 요청에 대응하는 seq_file. sf->private에 kernfs_open_file이
 *      들어 있어 seq_css(sf)로 대상 cgroup의 css를 알아낼 수 있다.
 * @v: seq_file 반복자 프로토콜이 넘기는 현재 위치 포인터. 이 파일은 단일 값만
 *     출력하는 단순 콜백이라 실제로는 사용하지 않는다.
 * @return: 항상 0(성공). 이 콜백은 실패할 수 있는 동작(메모리 할당 등)을 하지
 *          않으므로 별도의 에러 반환 경로가 없다.
 *
 * cftype.seq_show에 등록되어, 사용자가 "cat io.prio.class" 등으로 이 파일을
 * 읽을 때 kernfs -> cgroup 코어를 거쳐 호출된다. 동작은 단순히 (1)
 * ioprio_blkcg_from_css()로 대상 cgroup의 struct ioprio_blkcg를 찾고, (2)
 * blkcg->prio_policy를 policy_name[] 배열로 문자열화해 seq_printf()로 출력하는
 * 두 단계뿐이다.
 * 실행 컨텍스트: read() 시스템 콜을 호출한 사용자 프로세스의 컨텍스트에서
 * kernfs의 파일 단위 직렬화 아래 실행된다 - 잠들 수 있으나(seq_printf 자체는
 * 잠들지 않음) 인터럽트 컨텍스트에서 호출되지 않는다.
 * 에러 경로: 해당 없음 - blkcg가 이론상 NULL일 수 있는 경우는 없다(이 콜백이
 * 걸려 있는 cgroup은 이미 이 정책이 활성화된 상태이므로 ioprio_alloc_cpd()가
 * 항상 먼저 실행되어 데이터가 존재함이 보장된다).
 *
 * 호출 체인:
 *   cgroupfs read() → kernfs_fop_read_iter() → seq_read_iter() →
 *   seq_ops->show() → [ioprio_show_prio_policy]
 */
static int ioprio_show_prio_policy(struct seq_file *sf, void *v)
{
	struct ioprio_blkcg *blkcg = ioprio_blkcg_from_css(seq_css(sf));
	/* [한국어] seq_css(sf)가 sf->private(kernfs_open_file)로부터 of_css()를 거쳐
	 * "이 파일이 속한 cgroup"의 css를 얻고, ioprio_blkcg_from_css()가 그 css를
	 * 이 정책 전용 데이터(struct ioprio_blkcg)로 변환한다. */

	seq_printf(sf, "%s\n", policy_name[blkcg->prio_policy]);
	/* [한국어] blkcg->prio_policy(0~4)를 policy_name[] 배열의 인덱스로 사용해
	 * 대응하는 문자열("no-change" 등)을 얻고, 줄바꿈을 붙여 seq_file 버퍼에
	 * 기록한다 - 이 버퍼는 이후 read() 시스템 콜의 사용자 버퍼로 복사된다. */
	return 0;
	/* [한국어] show 콜백의 성공 반환값 - seq_file 프레임워크는 0을 "계속 진행/
	 * 성공"으로 해석한다. */
}

/*
 * [한국어]
 * ioprio_set_prio_policy - "io.prio.class" cgroupfs 파일에 write()할 때 새
 * 정책 값을 적용하는 kernfs write 콜백.
 *
 * @of: 이 write() 요청에 대응하는 kernfs_open_file. of_css(of)로 대상 cgroup의
 *      css를 얻는다.
 * @buf: 사용자가 write()한 문자열 버퍼. kernfs_fop_write_iter()가 미리 이
 *       버퍼를 '\0'로 끝맺어 두므로 이 함수는 buf를 그대로 C 문자열로 다룰 수
 *       있다(원본 영어 인라인 주석 참고).
 * @nbytes: 사용자가 write()에 넘긴 바이트 수. 성공 시 그대로 반환해 "전부
 *          받아들였다"는 뜻을 write(2) 시스템 콜 호출자에게 전달한다.
 * @off: write()의 파일 오프셋. 이 파일은 seek 후 이어쓰기를 지원하지 않는
 *       "단일 값 전체 교체" 방식의 cgroup 컨트롤 파일이라 0이 아니면 즉시
 *       거부한다.
 * @return: 성공 시 nbytes(사용자가 요청한 바이트 수 그대로), 실패 시 음수
 *          errno(-EIO 또는 sysfs_match_string()이 반환하는 -EINVAL 계열 값).
 *
 * cftype.write에 등록되어, 관리자가 "echo restrict-to-be > io.prio.class"
 * 등으로 정책을 바꿀 때 호출된다. 새 값은 sysfs_match_string()으로
 * policy_name[] 배열 안에서 문자열을 찾아 그 인덱스를 그대로 enum prio_policy
 * 값으로 사용한다 - 즉 이 배열이 "허용되는 정책 이름의 화이트리스트" 겸
 * "이름 -> enum 값 변환표" 역할을 동시에 한다.
 * 실행 컨텍스트: write() 시스템 콜을 호출한 사용자 프로세스 컨텍스트에서
 * kernfs가 제공하는 of->mutex(같은 파일에 대한 동시 write 직렬화) 아래
 * 실행된다.
 * 에러 경로: (1) off != 0이면 -EIO(부분 쓰기/이어쓰기를 지원하지 않는다는
 * 뜻), (2) sysfs_match_string()이 policy_name[] 안에서 일치하는 문자열을
 * 찾지 못하면 그 함수가 반환하는 음수 값을 그대로 상위(write(2) 호출자)에
 * 전달한다 - 두 경우 모두 blkcg->prio_policy는 변경되지 않는다.
 *
 * 호출 체인:
 *   cgroupfs write() → kernfs_fop_write_iter() → cftype.write() →
 *   [ioprio_set_prio_policy]
 */
static ssize_t ioprio_set_prio_policy(struct kernfs_open_file *of, char *buf,
				      size_t nbytes, loff_t off)
{
	struct ioprio_blkcg *blkcg = ioprio_blkcg_from_css(of_css(of));
	/* [한국어] of_css(of)가 "이 write() 요청이 열어 둔 kernfs 파일"이 속한
	 * cgroup의 css를 반환하고, ioprio_blkcg_from_css()가 그 css를 이 정책
	 * 전용 데이터로 변환한다. */
	int ret;
	/* [한국어] sysfs_match_string()의 반환값(성공 시 배열 인덱스, 실패 시 음수
	 * errno)을 임시로 담을 지역 변수. */

	if (off != 0)
	/* [한국어] 오프셋이 0이 아니라는 것은 사용자가 seek 후 이어쓰기를 시도했다는
	 * 뜻 - 이 컨트롤 파일은 "한 번에 전체 값을 새로 쓰는" 의미론만 지원하므로
	 * 즉시 거부한다. */
		return -EIO;
		/* [한국어] 표준 에러코드 -EIO(입출력 오류)로 write(2) 호출자에게
		 * "이 오프셋에서는 쓸 수 없다"를 알린다. */
	/* kernfs_fop_write_iter() terminates 'buf' with '\0'. */
	/* [한국어] 원본 주석 번역: kernfs_fop_write_iter()가 buf를 '\0'로 끝맺어
	 * 준다는 뜻 - 그래서 바로 아래 sysfs_match_string()이 strlen() 기반
	 * 문자열 비교를 안전하게 수행할 수 있다(버퍼 오버런 걱정 없이). */
	ret = sysfs_match_string(policy_name, buf);
	/* [한국어] buf에 담긴 문자열을 policy_name[] 배열(ARRAY_SIZE로 크기 자동
	 * 계산)과 순서대로 비교해, 일치하는 원소의 인덱스를 반환한다 - 그 인덱스
	 * 값이 곧 목표 enum prio_policy 값이다. 일치하는 항목이 없으면 음수를
	 * 반환한다. */
	if (ret < 0)
	/* [한국어] 사용자가 policy_name[] 어디에도 없는 문자열(오타 등)을 썼다는
	 * 뜻 - 새 정책을 적용하지 않고 그대로 에러를 전달한다. */
		return ret;
		/* [한국어] sysfs_match_string()이 반환한 음수 errno를 그대로 write(2)
		 * 호출자에게 전달한다. */
	blkcg->prio_policy = ret;
	/* [한국어] 유효성이 확인된 인덱스(ret)를 이 cgroup의 정책 값으로 확정
	 * 반영한다 - 이 대입 이후 제출되는 bio부터 blkcg_set_ioprio()가 새 정책을
	 * 적용한다. */
	return nbytes;
	/* [한국어] write(2) 관례에 따라 "요청받은 바이트 수를 전부 처리했다"는
	 * 뜻으로 nbytes를 그대로 반환한다 - 커널 write 콜백이 부분 성공을
	 * 표현하려면 더 작은 값을 반환할 수도 있지만, 이 콜백은 항상 전부-아니면-
	 * 전무(all-or-nothing) 방식이다. */
}

/*
 * [한국어]
 * ioprio_alloc_cpd - cgroup이 새로 생성될 때 이 정책의 per-cgroup 데이터(struct
 * ioprio_blkcg)를 할당하고 기본값으로 초기화하는 cpd_alloc_fn 콜백.
 *
 * @gfp: 할당에 사용할 GFP 플래그. 호출자(blk-cgroup 코어)가 상황에 맞는 플래그
 *       (통상 GFP_KERNEL)를 넘긴다.
 * @return: 새로 할당된 struct ioprio_blkcg의 cpd 필드 주소, 또는 메모리 부족 시
 *          NULL.
 *
 * blkcg_policy.cpd_alloc_fn에 등록되어, (1) blkcg_policy_register() 시점에
 * 이미 존재하는 모든 cgroup에 대해, 그리고 (2) 그 이후 새 cgroup이 생성될
 * 때마다(blkcg_css_alloc()) blk-cgroup 코어가 호출한다. 이 함수는 struct
 * ioprio_blkcg 전체를 kzalloc_obj()로 0-채움 할당한 뒤, prio_policy 필드만
 * POLICY_NO_CHANGE로 명시적으로 설정한다(POLICY_NO_CHANGE == 0이라 kzalloc의
 * 0-채움만으로도 이미 같은 값이지만, enum 값이 나중에 재배치되더라도 의도가
 * 코드에 분명히 드러나도록 명시적으로 대입한다). 반환값은 구조체 전체가 아니라
 * 그 첫 필드인 &blkcg->cpd인데, 이는 blkcg_policy_data* 반환 타입 규약을
 * 맞추기 위함이며, cpd가 첫 필드이므로 이 포인터로부터 container_of()를 쓰면
 * 언제든 struct ioprio_blkcg 전체를 되돌릴 수 있다.
 * 주의: 이 함수는 cpd.blkcg/cpd.plid 두 필드를 채우지 않는다 - 그 두 필드는
 * 이 함수가 반환한 직후 blk-cgroup 코어(blkcg_css_alloc()/
 * blkcg_policy_register())가 채운다. 즉 이 함수가 끝난 시점에는 cpd.blkcg/
 * cpd.plid가 아직 유효하지 않을 수 있다.
 * 실행 컨텍스트: cgroup 생성 시스템 콜(mkdir 등)을 처리하는 프로세스
 * 컨텍스트, 또는 blkcg_policy_register() 호출 스레드의 컨텍스트.
 * 에러 경로: kzalloc_obj() 실패 시 NULL을 그대로 반환하며, 호출자(blk-cgroup
 * 코어)가 이를 -ENOMEM으로 변환해 cgroup 생성 자체를 실패시킨다.
 *
 * 호출 체인:
 *   blkcg_css_alloc() → pol->cpd_alloc_fn → [ioprio_alloc_cpd]
 *   blkcg_policy_register() → list_for_each_entry(all_blkcgs) →
 *     pol->cpd_alloc_fn → [ioprio_alloc_cpd]
 */
static struct blkcg_policy_data *ioprio_alloc_cpd(gfp_t gfp)
{
	struct ioprio_blkcg *blkcg;
	/* [한국어] 새로 할당할 이 정책 전용 데이터를 담을 지역 포인터. */

	blkcg = kzalloc_obj(*blkcg, gfp);
	/* [한국어] kzalloc_obj(*blkcg, gfp)는 typeof(*blkcg)(=struct ioprio_blkcg)
	 * 크기만큼을 gfp 플래그로 0-채움 할당한다(내부적으로 kzalloc_noprof를
	 * 감싸는 헬퍼) - 반환된 메모리는 모든 바이트가 0이므로 cpd/prio_policy
	 * 모두 일단 0으로 시작한다. */
	if (!blkcg)
	/* [한국어] 메모리 부족 등으로 할당이 실패한 경우 - 이 cgroup에는 정책
	 * 데이터가 생기지 않는다. */
		return NULL;
		/* [한국어] 실패를 그대로 상위(blk-cgroup 코어)에 알려 cgroup 생성
		 * 전체를 되돌리게 한다. */
	blkcg->prio_policy = POLICY_NO_CHANGE;
	/* [한국어] kzalloc의 0-채움으로 이미 POLICY_NO_CHANGE(=0)와 같은 값이지만,
	 * "기본 정책은 변경 없음"이라는 의도를 코드에 명시적으로 드러내기 위해
	 * 다시 대입한다. */
	return &blkcg->cpd;
	/* [한국어] 구조체 전체가 아니라 그 첫 필드인 cpd의 주소를 반환한다 -
	 * blkcg_policy_data* 반환 타입 규약을 맞추면서도, cpd가 첫 필드이므로
	 * 이 값과 &blkcg는 실질적으로 같은 주소다. */
}

/*
 * [한국어]
 * ioprio_free_cpd - cgroup이 소멸될 때(또는 정책 등록 해제 시) 이 정책의
 * per-cgroup 데이터를 해제하는 cpd_free_fn 콜백.
 *
 * @cpd: 해제할 blkcg_policy_data 포인터. blk-cgroup 코어가 blkcg->cpd[plid]에
 *       보관해 두었던 값을 그대로 넘겨준다(ioprio_alloc_cpd()가 반환했던 바로
 *       그 포인터).
 * @return: 없음(void). 메모리 해제는 실패할 수 있는 연산이 아니다.
 *
 * blkcg_policy.cpd_free_fn에 등록되어, cgroup 소멸(blkcg_css_free()) 또는
 * 정책 자체의 등록 해제(blkcg_policy_unregister() -> blkcg_free_all_cpd())
 * 경로에서 호출된다. container_of()로 cpd 포인터가 embed되어 있던 바깥 struct
 * ioprio_blkcg 전체의 시작 주소를 계산한 뒤, 그 전체를 kfree()한다 - cpd만
 * 따로 해제하는 것이 아니라 ioprio_alloc_cpd()가 kzalloc_obj()로 할당했던
 * 블록 전체를 한 번에 반환하는 것이다.
 * 실행 컨텍스트: cgroup 소멸 경로(rmdir 등)의 프로세스 컨텍스트, 또는
 * blkcg_policy_unregister() 호출 스레드의 컨텍스트.
 * 에러 경로: 해당 없음 - kfree()는 실패를 반환하지 않는다.
 *
 * 호출 체인:
 *   blkcg_css_free() → pol->cpd_free_fn → [ioprio_free_cpd]
 *   blkcg_policy_unregister() → blkcg_free_all_cpd() → pol->cpd_free_fn →
 *     [ioprio_free_cpd]
 */
static void ioprio_free_cpd(struct blkcg_policy_data *cpd)
{
	struct ioprio_blkcg *blkcg = container_of(cpd, typeof(*blkcg), cpd);
	/* [한국어] container_of(cpd, typeof(*blkcg), cpd)로 cpd 포인터가 가리키는
	 * blkcg_policy_data가 embed되어 있던 바깥 struct ioprio_blkcg 전체의 주소를
	 * 복원한다 - typeof(*blkcg)를 쓴 이유는 blkcg 변수 자신의 선언과 타입을
	 * 어긋나지 않게 맞추기 위한 관용구다. */

	kfree(blkcg);
	/* [한국어] ioprio_alloc_cpd()가 kzalloc_obj()로 할당했던 struct
	 * ioprio_blkcg 전체(cpd 필드와 prio_policy 필드를 포함한 블록 전체)를
	 * 한 번에 해제한다 - cpd 필드만 따로 해제하는 게 아니라 바깥 구조체
	 * 전체를 해제해야 함에 유의(그래서 위에서 container_of로 전체 주소를
	 * 먼저 복원했다). */
}

static struct cftype ioprio_files[] = {
	/* [한국어] cgroup v1/v2 공통으로 등록되는 이 정책의 sysfs 파일 목록.
	 * blkcg_policy.dfl_cftypes/legacy_cftypes 양쪽이 모두 이 배열을 가리키므로
	 * (아래 static struct blkcg_policy ioprio_policy 정의 참고), 결과적으로
	 * cgroup v1("blkio" 계층)과 v2(통합 "io" 계층) 모두에서 파일 이름/동작이
	 * 완전히 동일하다. */
	{
		.name		= "prio.class",
		/* [한국어] cgroup 코어가 이 이름 앞에 서브시스템 접두사("io.")를 붙여
		 * 실제 sysfs 파일명 "io.prio.class"를 만든다 - 사용자가 실제로 보는
		 * 파일 경로는 /sys/fs/cgroup/<cgroup>/io.prio.class 형태가 된다. */
		.seq_show	= ioprio_show_prio_policy,
		/* [한국어] 이 파일을 read()할 때 호출될 seq_file show 콜백 - 위에서
		 * 정의한 ioprio_show_prio_policy()를 그대로 연결한다. */
		.write		= ioprio_set_prio_policy,
		/* [한국어] 이 파일에 write()할 때 호출될 콜백 - 위에서 정의한
		 * ioprio_set_prio_policy()를 그대로 연결한다. */
	},
	{ } /* sentinel */
	/* [한국어] 배열의 끝을 표시하는 종결자(sentinel) 원소 - name 필드가
	 * '\0'(빈 문자열)인 원소를 만나면 cgroup_add_cftypes()/
	 * cgroup_add_dfl_cftypes() 등이 순회를 멈춘다는 커널 전역 관례를 따른다. */
};

/*
 * [한국어]
 * ioprio_policy - 이 파일이 구현하는 "io.prio.class" 정책 전체를 blk-cgroup
 * 코어에 노출하는 실제(초기화식 있는) 정의. 이 파일 앞부분의
 * "static struct blkcg_policy ioprio_policy;"(잠정 정의)와 같은 저장 공간을
 * 공유하는 동일 객체다.
 * dfl_cftypes와 legacy_cftypes가 둘 다 같은 ioprio_files 배열을 가리키므로,
 * blkcg_policy_register()는 "pol->dfl_cftypes == pol->legacy_cftypes"
 * 특수 경로를 타서 cgroup_add_cftypes() 한 번만 호출한다 - 즉 v1/v2 파일을
 * 따로 등록하는 대신 하나의 등록으로 양쪽 계층에 동일한 파일을 노출한다.
 * pd_alloc_fn/pd_init_fn 등 "장치(request_queue)별" 데이터를 다루는 콜백들은
 * 일부러 채우지 않았다 - 이 정책의 상태(prio_policy)는 cgroup 단위로만
 * 존재하고, 그 cgroup이 어떤 블록 장치에 I/O를 하든 동일하게 적용되기
 * 때문이다(장치별로 다른 정책 값을 가질 필요가 없는 설계).
 */
static struct blkcg_policy ioprio_policy = {
	.dfl_cftypes	= ioprio_files,
	/* [한국어] cgroup v2(통합 계층)에 노출할 파일 배열 - 위에서 정의한
	 * ioprio_files를 그대로 가리킨다. */
	.legacy_cftypes = ioprio_files,
	/* [한국어] cgroup v1("blkio" 계층)에 노출할 파일 배열 - dfl_cftypes와 완전히
	 * 같은 배열을 가리키므로, blkcg_policy_register()가 두 계층을 위한 파일을
	 * 별도로 등록하지 않고 한 번에 등록하는 최적화 경로를 타게 된다. */

	.cpd_alloc_fn	= ioprio_alloc_cpd,
	/* [한국어] cgroup 생성 시 이 정책의 per-cgroup 데이터를 할당하는 콜백 -
	 * 위에서 정의한 ioprio_alloc_cpd()를 연결한다. */
	.cpd_free_fn	= ioprio_free_cpd,
	/* [한국어] cgroup 소멸 시 그 데이터를 해제하는 콜백 - cpd_alloc_fn과 반드시
	 * 쌍으로 존재해야 하며(blkcg_policy_register()가 XOR로 짝을 검사), 위에서
	 * 정의한 ioprio_free_cpd()를 연결한다. */
};

/*
 * [한국어]
 * blkcg_set_ioprio - bio가 속한 cgroup의 io.prio.class 정책을 읽어
 * bio->bi_ioprio에 반영(강제 기록)한다. 이 파일의 핵심 진입점이며,
 * block/blk-ioprio.h가 선언하는 유일한 공개 함수의 실제 구현이다.
 *
 * @bio: 우선순위를 기록할 대상 bio. 아직 request로 승격되지 않은 상태여야
 *       하며, bio->bi_blkg가 이미 설정되어 있어야 한다(즉 bio_associate_blkg()
 *       등으로 소속 cgroup이 먼저 확정된 뒤에 호출되어야 한다). 이 함수는
 *       bio 자체나 bio->bi_blkg의 NULL 여부를 검사하지 않으므로, 호출자가
 *       유효한 상태의 bio를 넘기는 것을 전제로 한다.
 * @return: 없음(void). 성공/실패를 구분해 반환하지 않는 이유는, 이 함수가
 *          "cgroup에 정책이 설정돼 있으면 반영하고, 없으면 아무 것도 하지
 *          않는" fail-open(best-effort) 방식으로 설계되어 있기 때문이다 -
 *          정책 부재는 에러가 아니라 정상적인 "기본값 유지" 경로다.
 *
 * 배경(Why): io.prio.class 정책이 cgroup 설정 파일 수준에만 머물러 있으면
 * I/O 스케줄러나 하위 드라이버는 그 존재를 알 방법이 없다. 이 함수는 그
 * 정책을 실제 bio 데이터(bi_ioprio 필드)로 "번역"해 넣는 유일한 지점이며,
 * 이 번역이 없으면 io.prio.class는 아무 효과도 내지 못한다.
 * 동작 단계(What):
 *   1) blkcg_to_ioprio_blkcg(bio->bi_blkg->blkcg)로 이 bio가 속한 cgroup의
 *      정책 데이터를 조회한다.
 *   2) blkcg가 NULL이거나(이 cgroup에서 정책이 활성화되지 않음) 정책이
 *      POLICY_NO_CHANGE면 아무 것도 하지 않고 즉시 반환한다.
 *   3) 정책이 POLICY_PROMOTE_TO_RT/POLICY_NONE_TO_RT(둘 다 동일하게 취급)면,
 *      bio의 클래스가 이미 IOPRIO_CLASS_RT가 아닐 때만 IOPRIO_CLASS_RT/
 *      레벨 4로 강제 승격하고 반환한다.
 *   4) 그 외(POLICY_RESTRICT_TO_BE 또는 POLICY_ALL_TO_IDLE)의 경우, 기존
 *      bio 우선순위와 정책이 요구하는 우선순위 중 "더 낮은" 쪽(raw 값으로는
 *      더 큰 쪽)을 선택해 필요할 때만 bio->bi_ioprio를 갱신한다.
 * 실행 컨텍스트(Execution context): I/O를 제출한 프로세스(태스크) 컨텍스트에서
 * 동기적으로 실행되며, 인터럽트 컨텍스트나 별도 커널 스레드에서 호출되지
 * 않는다. 하나의 bio는 단일 제출 경로에서만 다뤄지므로 이 함수 자체가 bio를
 * 놓고 다른 스레드와 경쟁할 일은 없다.
 * 호출자(Who calls): submit_bio_noacct_nocheck() 계열의 bio 제출 초기 경로에서
 * request로 변환되기 전에 호출된다(정확한 호출 지점은 block/blk-ioprio.h의
 * 문서 참고).
 * 피호출자(Who is called): blkcg_to_ioprio_blkcg()만을 호출한다.
 * 에러 경로(Error path): 실패를 반환하지 않는 fail-open 설계다 - cgroup/정책
 * 조회에 실패하거나 정책 자체가 없으면 그냥 아무 것도 하지 않고 반환한다.
 * 공유 상태(Shared state): bio->bi_ioprio(수정 대상)와, blkcg가 보유한
 * prio_policy(읽기 대상)가 이 함수를 통해 연결되는 공유 상태다.
 *
 * 호출 체인:
 *   submit_bio_noacct_nocheck() → [blkcg_set_ioprio] → (bio->bi_ioprio 갱신)
 *   → blk_mq_submit_bio() → blk_mq_get_request() (request->ioprio로 전파)
 *   → I/O 스케줄러(mq-deadline/bfq/kyber) 디스패치 순서 결정
 */
void blkcg_set_ioprio(struct bio *bio)
{
	struct ioprio_blkcg *blkcg = blkcg_to_ioprio_blkcg(bio->bi_blkg->blkcg);
	/* [한국어] bio->bi_blkg->blkcg(이 bio가 이미 연결된 cgroup)를 이 정책 전용
	 * 데이터로 변환한다 - bio->bi_blkg가 아직 설정되지 않았다면(호출 시점이
	 * 잘못됐다면) 이 역참조 자체가 문제가 되므로, 호출자는 반드시 bio_associate_
	 * blkg() 이후에만 이 함수를 호출해야 한다. */
	u16 prio;
	/* [한국어] 아래 max_t() 계산 결과를 담을 임시 변수 - bio->bi_ioprio와 같은
	 * u16(IOPRIO_PRIO_VALUE()의 반환 타입과 동일한 폭)로 선언되어 있다. */

	if (!blkcg || blkcg->prio_policy == POLICY_NO_CHANGE)
	/* [한국어] blkcg가 NULL이면 "이 cgroup에는 io.prio.class 정책 데이터 자체가
	 * 없다"(정책이 활성화되지 않았거나 cpd 슬롯이 비어 있음)는 뜻이고,
	 * POLICY_NO_CHANGE면 "정책은 있지만 아무 것도 바꾸지 말라"는 명시적
	 * 설정이다 - 두 경우 모두 bio를 있는 그대로 둔다. */
		return;
		/* [한국어] bio->bi_ioprio를 전혀 건드리지 않고 즉시 반환 - 태스크가
		 * ioprio_set()으로 설정했던 값이나 기본 IOPRIO_CLASS_NONE이 그대로
		 * 유지된다. */

	if (blkcg->prio_policy == POLICY_PROMOTE_TO_RT ||
	    blkcg->prio_policy == POLICY_NONE_TO_RT) {
	/* [한국어] 두 enum 값(1과 4)은 숫자는 다르지만 커널 문서상 서로 별칭
	 * 관계이며, 이 조건절에서 "||"로 나란히 검사되어 완전히 동일한 승격 로직을
	 * 공유한다 - 사용자가 어느 이름으로 정책을 설정했든 동작은 같다. */
		/*
		 * For RT threads, the default priority level is 4 because
		 * task_nice is 0. By promoting non-RT io-priority to RT-class
		 * and default level 4, those requests that are already
		 * RT-class but need a higher io-priority can use ioprio_set()
		 * to achieve this.
		 */
		/* [한국어] 원본 주석 번역: RT(Real Time) 스레드의 기본 우선순위
		 * 레벨이 4인 이유는 task_nice()가 0일 때 task_nice_ioprio()가
		 * (0+20)/5=4를 계산하기 때문이다. non-RT bio를 RT 클래스 +
		 * 기본 레벨 4로 승격시켜 두면, 이미 RT 클래스이면서 더 높은
		 * (숫자가 더 작은) I/O 우선순위가 필요한 요청은 태스크가 직접
		 * ioprio_set()을 호출해 레벨을 낮추는 방식으로 여전히 세밀하게
		 * 조정할 수 있다 - 즉 이 승격 로직은 "클래스"만 강제로 올릴 뿐,
		 * 이미 RT인 bio의 "레벨"은 절대 건드리지 않는다(아래 IOPRIO_PRIO_CLASS
		 * 비교가 클래스만 보는 이유). */
		if (IOPRIO_PRIO_CLASS(bio->bi_ioprio) != IOPRIO_CLASS_RT)
		/* [한국어] bio->bi_ioprio 상위 비트(클래스 필드)만 추출해 이미
		 * IOPRIO_CLASS_RT인지 확인한다 - 이미 RT라면 레벨 값을 그대로
		 * 존중하고 아래 대입을 건너뛴다(태스크가 ioprio_set()으로 세밀하게
		 * 맞춰 둔 레벨을 이 정책이 뭉개지 않도록). */
			bio->bi_ioprio = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_RT, 4);
			/* [한국어] 아직 RT가 아니었던 bio를 IOPRIO_CLASS_RT + 레벨
			 * 4(IOPRIO_NORM과 동일한 기본 레벨)로 완전히 덮어쓴다 - 기존
			 * 클래스/레벨 정보는 승격 이후 의미가 없으므로 보존하지
			 * 않는다. */
		return;
		/* [한국어] RT 승격 분기는 여기서 항상 반환한다 - 아래 RESTRICT_TO_BE/
		 * ALL_TO_IDLE 전용 계산(max_t 등)까지 내려가지 않도록 막는
		 * 역할이다. */
	}

	/*
	 * Except for IOPRIO_CLASS_NONE, higher I/O priority numbers
	 * correspond to a lower priority. Hence, the max_t() below selects
	 * the lower priority of bi_ioprio and the cgroup I/O priority class.
	 * If the bio I/O priority equals IOPRIO_CLASS_NONE, the cgroup I/O
	 * priority is assigned to the bio.
	 */
	/* [한국어] 원본 주석 번역: IOPRIO_CLASS_NONE(=0)을 예외로 하면, I/O
	 * 우선순위 값이 클수록(클래스 번호가 클수록: RT=1 < BE=2 < IDLE=3)
	 * 오히려 더 낮은 우선순위를 뜻한다. bio->bi_ioprio와 cgroup 정책이
	 * 요구하는 클래스 값을 16비트 raw 값으로 비교하면, 상위 비트에 있는
	 * 클래스 필드가 비교를 지배하므로 "값이 큰 쪽 = 우선순위가 더 낮은
	 * 쪽"이 된다. 이 지점에 도달했다는 것은 이미 위에서 POLICY_NO_CHANGE/
	 * PROMOTE_TO_RT/NONE_TO_RT가 모두 걸러졌다는 뜻이므로, 남은 정책 값은
	 * POLICY_RESTRICT_TO_BE(=2, IOPRIO_CLASS_BE와 동일값)와
	 * POLICY_ALL_TO_IDLE(=3, IOPRIO_CLASS_IDLE와 동일값)뿐이다 - 그래서
	 * blkcg->prio_policy를 별도 매핑 없이 그대로 IOPRIO_PRIO_VALUE()의
	 * 클래스 인자로 재사용할 수 있다. bio가 IOPRIO_CLASS_NONE(태스크가
	 * ioprio_set()을 한 번도 안 한 상태, raw 값이 가장 작음)이었다면
	 * max_t()는 사실상 항상 정책 쪽 값을 고르게 되어 "정책 값을 그대로
	 * bio에 대입"한 것과 같은 효과를 낸다. */
	prio = max_t(u16, bio->bi_ioprio,
			IOPRIO_PRIO_VALUE(blkcg->prio_policy, 0));
	/* [한국어] IOPRIO_PRIO_VALUE(blkcg->prio_policy, 0)은 정책이 요구하는
	 * 클래스를 "그 클래스 안에서 가장 높은(레벨 0)" 우선순위로 인코딩한
	 * 값이다 - 이는 "이 정책 클래스로 허용되는 범위 안에서 가장 관대한
	 * 하한선"을 의미하며, max_t(u16, ...)는 이 값과 bio의 기존 raw 값 중
	 * 더 큰 쪽(=더 낮은 우선순위 쪽)을 prio에 담는다. */
	if (prio > bio->bi_ioprio)
	/* [한국어] 계산된 prio가 bio의 기존 값보다 실제로 클 때만(즉 정책이 bio를
	 * 더 낮은 우선순위로 "제한"해야 할 때만) 아래 대입을 수행한다 - prio가
	 * bio->bi_ioprio와 같다면(이미 정책보다 낮거나 같은 우선순위였다면)
	 * 굳이 같은 값을 다시 쓰는 무의미한 대입을 건너뛰는 최적화다(기능적으로는
	 * 항상 대입해도 결과는 동일하다). */
		bio->bi_ioprio = prio;
		/* [한국어] bio의 최종 우선순위를 "기존 값과 정책값 중 더 낮은 쪽"으로
		 * 확정한다 - 이후 blk_mq_get_request()가 이 값을 request->ioprio로
		 * 복사해 스케줄러/드라이버에 전달한다. */
}

/*
 * [한국어]
 * ioprio_init - 모듈(또는 커널) 초기화 시 ioprio_policy를 blk-cgroup 정책
 * 테이블에 등록하는 module_init 훅.
 *
 * @return: blkcg_policy_register()의 반환값을 그대로 전달한다 - 0이면 성공,
 *          음수 errno(-EINVAL, -ENOSPC, -ENOMEM 등)면 등록 실패. module_init
 *          규약상 이 값이 0이 아니면 모듈 적재(또는 initcall)가 실패로 처리될
 *          수 있다.
 *
 * blkcg_policy_register(&ioprio_policy) 한 번의 호출이 (1) 전역 정책 슬롯
 * 테이블(blkcg_policy[])에서 빈 자리를 찾아 &ioprio_policy를 등록하고 plid를
 * 배정하며, (2) 이미 존재하는 모든 cgroup을 순회하며 ioprio_alloc_cpd()를
 * 호출해 cpd를 채우고, (3) dfl_cftypes == legacy_cftypes 특수 경로를 통해
 * "io.prio.class" 파일을 cgroup v1/v2 양쪽에 한 번에 노출하는 세 가지 일을
 * 모두 수행한다 - 이 파일은 그 결과만 그대로 반환할 뿐, 실패 시 롤백 같은
 * 추가 로직을 직접 구현하지 않는다(전부 blkcg_policy_register() 내부에서
 * 처리됨).
 * 실행 컨텍스트: 모듈 적재(insmod/modprobe) 또는 커널 부팅 중 initcall
 * 단계의 프로세스 컨텍스트.
 * 호출자(Who calls): module_init(ioprio_init) 매크로가 등록한 초기화
 * 엔트리포인트를 통해 모듈 서브시스템이 호출한다.
 * 피호출자(Who is called): blkcg_policy_register() 하나만 호출한다.
 * 에러 경로: blkcg_policy_register()가 실패하면(-EINVAL: cpd/pd 함수 쌍
 * 불일치, -ENOSPC: BLKCG_MAX_POLS 슬롯 부족, -ENOMEM: cpd 할당 실패) 그
 * 값을 그대로 반환하며, 이 정책은 등록되지 않은 채로 남는다(등록 실패 시
 * blkcg_policy_register() 내부에서 이미 부분 상태를 롤백해 둔다).
 *
 * 호출 체인:
 *   module_init(ioprio_init) → [ioprio_init] → blkcg_policy_register()
 */
static int __init ioprio_init(void)
{
	return blkcg_policy_register(&ioprio_policy);
	/* [한국어] 전역 정책 객체 &ioprio_policy를 blk-cgroup 코어의 정책
	 * 테이블에 등록하고, 그 성공/실패 결과(0 또는 음수 errno)를 그대로
	 * module_init 규약에 맞춰 반환한다. */
}

/*
 * [한국어]
 * ioprio_exit - 모듈 제거 시 ioprio_policy를 blk-cgroup 정책 테이블에서
 * 해제하는 module_exit 훅.
 *
 * @return: 없음(void). module_exit 콜백은 실패를 표현할 수 없는 규약이다.
 *
 * blkcg_policy_unregister(&ioprio_policy) 한 번의 호출이 (1) "io.prio.class"
 * cftype 파일들을 cgroup_rm_cftypes()로 제거하고, (2) 존재하는 모든 cgroup에
 * 대해 ioprio_free_cpd()를 호출해 struct ioprio_blkcg 메모리를 회수하며,
 * (3) 전역 정책 슬롯 테이블(blkcg_policy[plid])에서 이 정책을 제거하는 세
 * 가지 일을 모두 수행한다 - ioprio_init()과 정확히 대칭을 이루는 정리 경로다.
 * 실행 컨텍스트: 모듈 제거(rmmod) 프로세스 컨텍스트.
 * 호출자(Who calls): module_exit(ioprio_exit) 매크로가 등록한 종료
 * 엔트리포인트를 통해 모듈 서브시스템이 호출한다.
 * 피호출자(Who is called): blkcg_policy_unregister() 하나만 호출한다.
 * 에러 경로: blkcg_policy_unregister() 자체가 void를 반환하므로(내부에서
 * WARN_ON으로 슬롯 불일치만 방어) 이 함수에도 별도의 에러 반환 경로가 없다.
 *
 * 호출 체인:
 *   module_exit(ioprio_exit) → [ioprio_exit] → blkcg_policy_unregister()
 */
static void __exit ioprio_exit(void)
{
	blkcg_policy_unregister(&ioprio_policy);
	/* [한국어] 전역 정책 객체 &ioprio_policy를 blk-cgroup 코어에서 등록
	 * 해제한다 - cftype 파일 제거, 모든 cgroup의 cpd 해제, 정책 테이블
	 * 슬롯 반환까지 전부 이 한 호출 안에서 처리된다. */
}

module_init(ioprio_init);
/* [한국어] ioprio_init을 이 모듈(또는 내장 서브시스템)의 초기화 엔트리포인트로
 * 등록하는 매크로 - CONFIG_BLK_CGROUP_IOPRIO가 tristate이고 모듈로 빌드되면
 * insmod 시점에, 내장(built-in)으로 빌드되면 커널 부팅 중 대응하는 initcall
 * 단계에서 자동 호출된다. */
module_exit(ioprio_exit);
/* [한국어] ioprio_exit을 이 모듈의 종료 엔트리포인트로 등록하는 매크로 -
 * 모듈로 빌드되어 rmmod될 때만 실제로 호출되며, 내장으로 빌드된 경우 이
 * 함수는 링크는 되지만 커널 종료 시나리오가 없어 사실상 호출되지 않는다. */
