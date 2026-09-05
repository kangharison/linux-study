/* SPDX-License-Identifier: GPL-2.0 */
/*
 * IRQ subsystem internal functions and variables:
 *
 * Do not ever include this file from anything else than
 * kernel/irq/. Do not even think about using any information outside
 * of this file for your non core code.
 */
/*
 * [한국어 설명] irq 서브시스템의 내부 계약 헤더 (internals.h)
 *
 * === 파일의 역할 ===
 * kernel/irq/ 안의 .c 파일들만 공유하는 선언을 모아 둔 헤더다. 코어가 서로
 * 부르는 함수의 원형, 서술자의 내부 상태 비트(IRQS_*)와 스레드 상태 비트
 * (IRQTF_*), 서술자 락을 안전하게 잡는 관용구, 그리고 CONFIG 조합에 따라
 * 실체가 갈리는 함수들의 빈 구현이 여기 있다.
 *
 * 상단 영어 주석의 경고가 이 파일의 성격을 말해 준다 — "kernel/irq 밖에서는
 * 절대 포함하지 말고, 여기 있는 정보를 코어 밖 코드에 쓸 생각도 하지 말라".
 * 즉 이것은 API 가 아니라 구현 세부다. 공개 API 는 include/linux/irq.h 와
 * include/linux/interrupt.h 에 있다.
 *
 * 파일 곳곳에 "직접 만지지 못하게 막는" 장치가 반복해서 나타난다. istate 를
 * core_internal_state__do_not_mess_with_it 으로 바꾸는 #define, irqd_to_state
 * 매크로를 쓰고 나서 곧바로 #undef 하는 것, 그리고 이 파일이 포함하는
 * settings.h 의 GOT_YOU_MORON 봉인이 모두 같은 취지다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 인터럽트 한 번이 처리되는 흐름과, 그 각 단계를 맡는 파일은 이렇다:
 *
 *   하드웨어 인터럽트 발생
 *     ↓ 아키텍처 진입점 (arch/<아키텍처>/kernel/irq.c)
 *   generic_handle_irq()            — irqdesc.c
 *     ↓ desc->handle_irq
 *   흐름 제어 핸들러                 — chip.c
 *     handle_level_irq / handle_edge_irq / handle_fasteoi_irq / handle_percpu_irq
 *     ↓ 마스크·ack 순서를 방식에 맞게 처리한 뒤
 *   handle_irq_event()              — handle.c
 *     ↓ action 목록을 순회하며
 *   드라이버 핸들러 (irqaction->handler)
 *     ↓ IRQ_WAKE_THREAD 를 돌려주면
 *   __irq_wake_thread() → irq_thread() — manage.c
 *     ↓ 반환값 집계
 *   note_interrupt()                — spurious.c (오탐 감지)
 *
 * 이 헤더는 그 모든 단계가 서로를 부르기 위한 선언을 담는다. 자기 코드는
 * 거의 없고(인라인 접근자뿐), 파일들 사이의 계약을 정의하는 자리다.
 *
 * === 타 모듈과의 연결 ===
 * 포함하는 것:
 *   linux/irqdesc.h    — struct irq_desc 의 정의. 이 파일의 모든 선언이 그것을 다룬다.
 *   linux/kernel_stat.h — 인터럽트 횟수 통계(kstat). 아래 kstat_incr_* 가 쓴다.
 *   linux/pm_runtime.h — 런타임 전원 관리. 인터럽트가 장치를 깨우는 경로에 필요하다.
 *   linux/sched/clock.h — sched_clock(). 인터럽트 타이밍 계측에 쓴다.
 *   "debug.h", "settings.h" — 같은 디렉터리의 내부 헤더. 순서가 중요하다
 *     (아래 그 include 지점의 주석 참고).
 *
 * 이 파일에 의존하는 곳: kernel/irq 의 모든 .c 파일이 예외 없이 포함한다.
 *
 * 데이터 흐름: 이 파일 자체는 데이터를 옮기지 않는다. 다만 struct irq_desc 의
 * 두 상태 워드에 대한 접근 규칙을 정한다 —
 *   desc->istate               (IRQS_*)  — 코어의 처리 상태. 이 파일이 이름을 봉인한다.
 *   desc->irq_data.common->state (IRQD_*) — 하드웨어 쪽 상태. irqd_* 접근자로만.
 *   desc->status_use_accessors (_IRQ_*)  — 설정. settings.h 의 접근자로만.
 * 셋이 각각 다른 규칙과 락을 갖고, 그 구분이 이 파일에 기록되어 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * enum { IRQTF_* }        — 인터럽트 스레드의 상태 비트. thread_flags 에 담긴다.
 * enum { IRQS_* }         — 서술자의 코어 내부 상태 비트. istate 에 담긴다.
 * chip_bus_lock()         — 느린 버스(I2C/SPI) 뒤의 칩을 만지기 전에 잠근다.
 * scoped_irqdesc_get_and_lock() — 서술자를 번호로 찾아 락을 잡고, 블록을 벗어나면 자동 해제.
 * irqd_set()/irqd_clear() — irq_data 의 IRQD_* 상태 비트를 다루는 유일한 통로.
 * kstat_incr_irqs_this_cpu() — 인터럽트 횟수를 CPU 별·전역 양쪽에 센다.
 * irq_can_move_pcntxt()   — 친화도 변경을 인터럽트 문맥에서 해도 되는지.
 *
 * 이 파일의 절반 가까이가 #ifdef 로 갈린 빈 구현이다. 그 목적은 언제나 같다 —
 * 호출부에 #ifdef 를 흩지 않고, 기능이 없는 빌드에서는 호출이 사라지게 하는 것.
 */
#include <linux/irqdesc.h>	/* [한국어] struct irq_desc 와 그 접근자들. 이 헤더의 거의 모든 선언이 이 타입을 다룬다 */
#include <linux/kernel_stat.h>	/* [한국어] kstat_irqs 통계. 아래 kstat_incr_irqs_this_cpu 가 쓴다 */
#include <linux/pm_runtime.h>	/* [한국어] 런타임 PM. 인터럽트가 잠든 장치를 깨우는 경로에서 필요하다 */
#include <linux/sched/clock.h>	/* [한국어] sched_clock(). 인터럽트 처리 시간을 재는 계측이 쓴다 */

/* [한국어] 인터럽트 번호 공간의 상한.
 *
 * SPARSE_IRQ 는 서술자를 배열이 아니라 radix tree 로 관리하는 구성이다.
 * 번호가 드문드문 흩어져 있어도 낭비가 없으므로 상한을 INT_MAX 로 열어 둔다 —
 * 실제 한계는 메모리이지 번호 공간이 아니다.
 *
 * 반대로 SPARSE_IRQ 가 없으면 서술자가 정적 배열이라, 배열 크기인 NR_IRQS 가
 * 그대로 상한이 된다. 오늘날 대부분의 아키텍처는 SPARSE_IRQ 를 쓴다. */
#ifdef CONFIG_SPARSE_IRQ	/* [한국어] 서술자를 트리로 관리하는 구성인가 */
# define MAX_SPARSE_IRQS	INT_MAX	/* [한국어] 번호 공간에 사실상 상한을 두지 않는다 */
#else
# define MAX_SPARSE_IRQS	NR_IRQS	/* [한국어] 정적 배열의 크기가 곧 상한이다 */
#endif

/* [한국어] desc->istate 를 직접 만지지 못하게 하는 장치.
 *
 * struct irq_desc 의 실제 필드 이름은 core_internal_state__do_not_mess_with_it
 * 이라는 긴 이름이고, 코어 안에서만 istate 라는 짧은 별명으로 부른다. 코어
 * 밖의 코드는 이 #define 을 볼 수 없으므로 긴 이름을 그대로 써야 하는데,
 * 그 이름 자체가 "건드리지 말라"는 경고가 된다.
 *
 * settings.h 의 GOT_YOU_MORON 봉인과 같은 발상이지만 방향이 반대다. 그쪽은
 * 코어 안에서 공개 이름을 막는 것이고, 이쪽은 코어 밖에서 내부 이름을 쓰기
 * 불편하게 만드는 것이다. */
#define istate core_internal_state__do_not_mess_with_it

extern bool noirqdebug;	/* [한국어] 오탐(spurious) 감지를 끄는 부트 옵션. spurious.c 가 정의하고 note_interrupt() 가 읽는다 */
extern int irq_poll_cpu;	/* [한국어] 지금 인터럽트 폴링을 수행 중인 CPU 번호. 폴링이 자기 자신을 오탐으로 세지 않도록 spurious.c 가 쓴다 */

extern struct irqaction chained_action;	/* [한국어] 연쇄(chained) 인터럽트임을 나타내는 표식용 action. 실제 핸들러가 아니라 "이 서술자는 상위 컨트롤러가 직접 부른다"는 표시이며, 아래 irq_desc_is_chained() 가 주소를 비교해 식별한다 */

/*
 * Bits used by threaded handlers:
 * IRQTF_RUNTHREAD - signals that the interrupt handler thread should run
 * IRQTF_WARNED    - warning "IRQ_WAKE_THREAD w/o thread_fn" has been printed
 * IRQTF_AFFINITY  - irq thread is requested to adjust affinity
 * IRQTF_FORCED_THREAD  - irq action is force threaded
 * IRQTF_READY     - signals that irq thread is ready
 */
/* [한국어] (위 영어 주석에 이어) 인터럽트 스레드의 상태 비트.
 *
 * 값이 아니라 비트 "번호"라는 점에 주의한다 — 아래 IRQS_* 가 0x1, 0x2 처럼
 * 마스크인 것과 달리, 이쪽은 0, 1, 2... 로 세어 set_bit()/test_bit() 계열에
 * 그대로 넘긴다. 스레드 상태는 여러 CPU 가 동시에 건드리므로 원자적 비트
 * 연산이 필요하고, 그 API 가 번호를 받기 때문이다.
 *
 * 담기는 곳: struct irqaction 의 thread_flags. */
enum {
	IRQTF_RUNTHREAD,
	/* [한국어] 이 스레드가 할 일이 생겼다는 신호.
	 * 설정자: __irq_wake_thread() — 1차 핸들러가 IRQ_WAKE_THREAD 를 돌려주면
	 *   이 비트를 세우고 스레드를 깨운다.
	 * 읽는 자: irq_thread() 의 루프가 test_and_clear_bit 으로 확인하고 지운다.
	 * 왜 플래그가 필요한가: 스레드를 깨우는 것만으로는 부족하다. 깨어난 스레드가
	 *   "정말 내 차례인가"를 확인할 수단이 있어야, 다른 이유로 깨어났을 때
	 *   핸들러를 헛돌리지 않는다.
	 * 동기화: 원자적 비트 연산. 하드 인터럽트 문맥에서 세우고 스레드 문맥에서 지운다. */

	IRQTF_WARNED,
	/* [한국어] "IRQ_WAKE_THREAD w/o thread_fn" 경고를 이미 출력했다는 표시.
	 * 설정자/읽는 자: __irq_wake_thread() 가 test_and_set_bit 으로 한 번만 찍는다.
	 * 무슨 상황인가: 드라이버의 1차 핸들러가 IRQ_WAKE_THREAD 를 돌려주었는데
	 *   등록된 스레드 핸들러가 없는 경우다. 드라이버 버그이지만, 그 인터럽트가
	 *   초당 수천 번 오면 경고가 로그를 가득 채워 시스템을 마비시킨다.
	 * 그래서 한 번만 찍고 이 비트로 봉한다 — 진단은 남기되 피해는 막는다.
	 * 동기화: 원자적 비트 연산. */

	IRQTF_AFFINITY,
	/* [한국어] 이 스레드의 CPU 친화도를 다시 맞춰야 한다는 요청.
	 * 설정자: irq_set_affinity 경로가 인터럽트의 친화도를 바꿀 때.
	 * 읽는 자: irq_thread() 의 루프가 매 반복마다 확인해, 서 있으면
	 *   irq_thread_check_affinity() 로 자기 친화도를 갱신한다.
	 * 왜 즉시 바꾸지 않는가: 친화도를 바꾸는 쪽은 인터럽트 문맥이거나 락을
	 *   쥔 상태일 수 있는데, set_cpus_allowed_ptr() 은 그런 곳에서 부를 수 없다.
	 *   그래서 요청만 남기고 스레드가 자기 문맥에서 처리하게 미룬다.
	 * 스레드를 인터럽트와 같은 CPU 에 두는 이유: 캐시 지역성 때문이다. */

	IRQTF_FORCED_THREAD,
	/* [한국어] 드라이버가 요청한 것이 아니라 커널이 강제로 스레드화한 action 인가.
	 * 설정자: irq_setup_forced_threading() — threadirqs 부트 옵션이나
	 *   PREEMPT_RT 에서 1차 핸들러를 스레드로 옮길 때.
	 * 읽는 자: irq_forced_thread_fn() 이 이 경우에만 필요한 처리를 할 때.
	 * 무엇이 다른가: 강제 스레드화된 핸들러는 원래 하드 인터럽트 문맥을
	 *   전제하고 쓰여 있다. 그래서 스레드에서 부를 때 local_bh_disable() 로
	 *   감싸, 소프트IRQ 가 끼어들지 않는 비슷한 환경을 만들어 준다.
	 * 자발적으로 스레드 핸들러를 등록한 드라이버에는 그 처리가 불필요하다. */

	IRQTF_READY,
	/* [한국어] 이 스레드가 준비를 마쳤다는 표시.
	 * 설정자: irq_thread() 가 시작하며 자기 자신에 대해 세운다.
	 * 읽는 자: __setup_irq() 가 wait_for_completion 대신 이 비트를 기다린다.
	 * 왜 필요한가: request_threaded_irq() 가 돌아왔을 때는 인터럽트가 언제든
	 *   들어올 수 있다. 그런데 스레드가 아직 스케줄되지 않았다면, 깨우기가
	 *   유실되어 첫 인터럽트의 스레드 처리가 통째로 사라질 수 있다.
	 * 그래서 요청 경로는 스레드가 실제로 돌기 시작할 때까지 기다린 뒤에야
	 *   인터럽트를 연다. */
};

/*
 * Bit masks for desc->core_internal_state__do_not_mess_with_it
 *
 * IRQS_AUTODETECT		- autodetection in progress
 * IRQS_SPURIOUS_DISABLED	- was disabled due to spurious interrupt
 *				  detection
 * IRQS_POLL_INPROGRESS		- polling in progress
 * IRQS_ONESHOT			- irq is not unmasked in primary handler
 * IRQS_REPLAY			- irq has been resent and will not be resent
 * 				  again until the handler has run and cleared
 * 				  this flag.
 * IRQS_WAITING			- irq is waiting
 * IRQS_PENDING			- irq needs to be resent and should be resent
 * 				  at the next available opportunity.
 * IRQS_SUSPENDED		- irq is suspended
 * IRQS_NMI			- irq line is used to deliver NMIs
 * IRQS_SYSFS			- descriptor has been added to sysfs
 */
/* [한국어] (위 영어 주석에 이어) 서술자의 코어 내부 상태 비트.
 *
 * 위 IRQTF_* 와 달리 이쪽은 비트 마스크(0x1, 0x2, 0x8...)다. istate 는 서술자
 * 락 아래에서만 바뀌므로 원자적 비트 연산이 필요 없고, 여러 비트를 한 번에
 * 검사하는 일이 잦아 마스크가 편하다.
 *
 * 값이 듬성듬성한 것(0x4, 0x10, 0x100 등이 빠져 있다)은 역사의 흔적이다.
 * 쓰이지 않게 된 비트를 지우면서 남은 비트의 값을 옮기지 않았다 — 값을 옮기면
 * 디버그 출력이나 트레이스 도구와의 대응이 깨지기 때문이다.
 *
 * 이 워드가 담기는 필드 이름이 core_internal_state__do_not_mess_with_it 이고,
 * 코어 안에서만 위 #define 으로 istate 라는 별명을 쓴다. */
enum {
	IRQS_AUTODETECT		= 0x00000001,
	/* [한국어] 자동 탐지(autoprobe)가 이 인터럽트에 대해 진행 중이다.
	 * 설정자: probe_irq_on() 이 후보 인터럽트를 열면서 세운다.
	 * 읽는 자: 흐름 제어 핸들러들이 IRQS_WAITING 을 지울지 정할 때,
	 *   그리고 probe_irq_off() 가 결과를 수확할 때.
	 * 값의 뜻: 이 인터럽트는 지금 "누가 울리는지 보는 중"이라 실제 핸들러가
	 *   없다. 들어오면 아래 IRQS_WAITING 을 지우는 것으로 흔적만 남긴다.
	 * 동기화: 서술자 락. */

	IRQS_SPURIOUS_DISABLED	= 0x00000002,
	/* [한국어] 오탐이 너무 많아 커널이 이 인터럽트를 강제로 껐다.
	 * 설정자: spurious.c 의 __report_bad_irq() — "nobody cared" 메시지와 함께.
	 * 읽는 자: __enable_irq() 가 사용자가 다시 켤 때 이 표시를 지운다.
	 * 언제 일어나는가: 등록된 핸들러들이 연속 10만 번 IRQ_NONE 을 돌려주면
	 *   커널은 그 인터럽트를 고장으로 보고 꺼 버린다. 그러지 않으면 처리되지
	 *   않는 인터럽트가 CPU 를 무한히 점유해 시스템이 멈춘다.
	 * 이 비트가 서 있는 인터럽트는 로그를 확인해 원인을 찾아야 한다.
	 * 동기화: 서술자 락. */

	IRQS_POLL_INPROGRESS	= 0x00000008,
	/* [한국어] 이 인터럽트에 대한 폴링이 지금 진행 중이다.
	 * 설정자/읽는 자: spurious.c 의 misrouted_irq()/poll_spurious_irqs().
	 * 무엇을 막는가: 오탐 감지는 "핸들러를 직접 불러 보고 누가 응답하는지"
	 *   확인하는 폴링을 한다. 그 폴링 중에 진짜 인터럽트가 들어와 같은
	 *   핸들러를 부르면 재진입이 된다. 이 비트가 그것을 막는다.
	 * 값이 0x4 를 건너뛴 것은 위 enum 주석의 역사적 이유 때문이다.
	 * 동기화: 서술자 락. */

	IRQS_ONESHOT		= 0x00000020,
	/* [한국어] 스레드 핸들러가 끝날 때까지 인터럽트를 언마스크하지 않는다.
	 * 설정자: __setup_irq() 가 IRQF_ONESHOT 요청을 받았을 때.
	 * 읽는 자: chip.c 의 흐름 제어가 1차 핸들러 뒤에 언마스크할지 정할 때,
	 *   그리고 irq_finalize_oneshot() 이 스레드 종료 후 언마스크할 때.
	 * 왜 필요한가: 레벨 트리거 인터럽트에서 1차 핸들러가 원인을 지우지 않고
	 *   스레드에 미루면, 언마스크하는 순간 같은 인터럽트가 곧바로 다시 들어와
	 *   무한 루프가 된다. 스레드가 원인을 지울 때까지 마스크를 유지해야 한다.
	 * 1차 핸들러 없이 스레드 핸들러만 등록하는 경우 커널이 자동으로 요구한다.
	 * 동기화: 서술자 락. */

	IRQS_REPLAY		= 0x00000040,
	/* [한국어] (위 영어 주석 참고) 이 인터럽트를 소프트웨어로 재전송해 두었다.
	 * 설정자: check_irq_resend() 가 재전송을 실제로 발행할 때.
	 * 읽는 자: 같은 함수가 다음 재전송을 막을 때, 흐름 제어가 핸들러 실행 후 지운다.
	 * 무엇을 막는가: 영어 주석대로 "핸들러가 돌아 이 플래그를 지울 때까지
	 *   다시 재전송하지 않는다". 이 방어가 없으면 재전송이 재전송을 부르는
	 *   폭주가 생긴다.
	 * 재전송이 왜 필요한가: 인터럽트가 비활성화된 동안 들어온 것을, 다시
	 *   켤 때 잃지 않고 처리하기 위해서다. 하드웨어가 다시 보내 주지 않으므로
	 *   소프트웨어가 흉내 낸다.
	 * 동기화: 서술자 락. */

	IRQS_WAITING		= 0x00000080,
	/* [한국어] (위 영어 주석: irq is waiting) 자동 탐지 중이며 아직 울리지 않았다.
	 * 설정자: probe_irq_on() 이 후보를 열면서 IRQS_AUTODETECT 와 함께 세운다.
	 * 읽는 자: probe_irq_off() 가 수확할 때 — 아직 서 있으면 그 번호는
	 *   울리지 않은 것이고, 지워져 있으면 그 번호가 울린 것이다.
	 * 지우는 자: 흐름 제어 핸들러가 인터럽트를 받으면 지운다.
	 * 즉 "기다리는 중" 표시를 인터럽트가 지워 주는 방식으로 탐지가 이루어진다.
	 * 동기화: 서술자 락. */

	IRQS_PENDING		= 0x00000200,
	/* [한국어] (위 영어 주석 참고) 처리하지 못한 인터럽트가 있어 재전송이 필요하다.
	 * 설정자: 비활성화된 상태에서 인터럽트가 들어왔을 때, 또는 엣지 흐름 제어가
	 *   핸들러 실행 중에 새 엣지를 받았을 때.
	 * 읽는 자: __enable_irq()/irq_startup() 이 켜면서 재전송을 발행할지 정할 때,
	 *   handle_edge_irq() 의 재실행 루프.
	 * 영어 주석의 "다음 기회에 재전송한다"가 핵심이다. 인터럽트를 잃지 않는
	 *   것이 이 비트의 존재 이유다.
	 * 위 IRQS_REPLAY 와의 관계: PENDING 은 "보내야 한다", REPLAY 는 "보냈다"이다.
	 * 동기화: 서술자 락. */

	IRQS_SUSPENDED		= 0x00000800,
	/* [한국어] 시스템 절전으로 이 인터럽트가 중단된 상태다.
	 * 설정자: suspend_device_irqs() 가 절전 진입 시 세운다.
	 * 읽는 자: 인터럽트가 들어왔을 때 처리 대신 기록만 할지 정하는 곳,
	 *   그리고 resume_device_irqs() 가 되돌릴 때.
	 * 왜 그냥 끄지 않는가: 절전 중에 들어온 인터럽트가 깨우기(wakeup) 원인일
	 *   수 있다. 그것을 완전히 차단하면 시스템이 깨어나지 못한다. 그래서
	 *   처리는 미루되 도착 사실은 기록해, 깨어난 뒤 재전송한다.
	 * IRQF_NO_SUSPEND 로 요청된 인터럽트는 이 대상에서 제외된다.
	 * 동기화: 서술자 락. */

	IRQS_TIMINGS		= 0x00001000,
	/* [한국어] 이 인터럽트의 도착 시각을 기록해 예측에 쓴다.
	 * 설정자: __setup_irq() 가 IRQF_TIMINGS 요청을 받았을 때.
	 * 읽는 자: handle_irq_event_percpu() 가 타이밍을 남길지 정할 때.
	 * 무엇에 쓰는가: cpuidle 거버너가 "다음 인터럽트가 언제 올까"를 예측해
	 *   얼마나 깊은 절전 상태로 들어갈지 정한다. 곧 인터럽트가 올 것 같으면
	 *   깊이 자지 않는 편이 낫다 — 깨어나는 데 드는 시간이 아깝기 때문이다.
	 * 이 enum 에서 유일하게 위 영어 주석 목록에 설명이 없는 항목인데,
	 *   나중에 추가되면서 주석이 함께 갱신되지 않은 것이다.
	 * 동기화: 서술자 락. */

	IRQS_NMI		= 0x00002000,
	/* [한국어] (위 영어 주석 참고) 이 인터럽트 선이 NMI 전달에 쓰인다.
	 * 설정자: request_nmi()/prepare_percpu_nmi() 경로.
	 * 읽는 자: 아래 irq_is_nmi() 를 통해 여러 곳에서. 대표적으로 이 인터럽트를
	 *   보통의 방식으로 다루면 안 된다는 판정에 쓴다.
	 * 무엇이 다른가: NMI 는 마스크할 수 없고, 락을 잡을 수 없는 문맥에서
	 *   실행되며, 스레드화할 수도 없다. 그래서 요청 단계에서부터 제약이
	 *   훨씬 엄격하고, 처리 경로도 따로 있다.
	 * 왜 istate 에 두는가: 이것은 하드웨어의 성질이 아니라 "이 선을 NMI 로
	 *   쓰기로 했다"는 코어의 결정이라, 설정(status)이 아닌 상태에 속한다.
	 * 동기화: 서술자 락. */

	IRQS_SYSFS		= 0x00004000,
	/* [한국어] (위 영어 주석 참고) 이 서술자가 sysfs 에 등록되어 있다.
	 * 설정자: irq_sysfs_add() 가 등록에 성공했을 때.
	 * 읽는 자: 서술자를 해제할 때 sysfs 항목을 지울지 정하는 곳.
	 * 왜 기록해야 하는가: sysfs 등록은 실패할 수 있고, 또 sysfs 자체가
	 *   초기화되기 전에 만들어지는 서술자도 있다(부팅 초기의 인터럽트).
	 *   등록하지 않은 항목을 지우려 하면 경고가 나므로, 실제로 등록된 것만
	 *   지우도록 표시를 남긴다.
	 * 이 enum 의 마지막 값이며, 새 상태가 필요하면 0x00008000 부터 이어진다.
	 * 동기화: 서술자 락. */
};

/* [한국어] 같은 디렉터리의 내부 헤더 둘. 순서와 위치가 모두 의미가 있다.
 *
 * 파일 맨 위가 아니라 여기 있는 이유: settings.h 는 struct irq_desc 와 IRQ_*
 * 상수가 이미 보이는 상태를 전제하고, 위에서 그 조건이 갖춰졌다. 또 settings.h
 * 는 IRQ_* 이름들을 컴파일 불가 토큰으로 봉인하므로, 그 이름을 쓰는 코드가
 * 있다면 반드시 이 지점보다 위에 있어야 한다.
 *
 * debug.h 가 먼저인 것은 그쪽이 아무것도 봉인하지 않는 순수한 추가라, 순서에
 * 제약이 없기 때문이다. */
#include "debug.h"	/* [한국어] 인터럽트 상태를 사람이 읽는 형태로 찍는 진단 매크로 */
#include "settings.h"	/* [한국어] status_use_accessors 접근자. 이 시점부터 IRQ_* 공개 이름이 봉인된다 */

extern int __irq_set_trigger(struct irq_desc *desc, unsigned long flags);	/* [한국어] 트리거 방식(엣지/레벨과 극성)을 하드웨어에 설정하고 서술자 기록을 맞춘다. manage.c 구현 */
extern void __disable_irq(struct irq_desc *desc);	/* [한국어] 서술자 락을 이미 쥔 호출자를 위한 disable. 깊이(depth)를 올리고 필요하면 마스크한다 */
extern void __enable_irq(struct irq_desc *desc);	/* [한국어] 그 짝. 깊이를 내리고 0 이 되면 실제로 켠다 — 켤 때 IRQS_PENDING 을 보고 재전송을 발행한다 */

/* [한국어] irq_startup() 계열의 resend 인자에 이름을 붙인 상수 쌍.
 *
 * 왜 불리언에 이름을 붙이는가: irq_startup(desc, true, false) 같은 호출은
 * 어느 true 가 무엇인지 알 수 없다. irq_startup(desc, IRQ_RESEND,
 * IRQ_START_COND) 라고 쓰면 호출부만 보고 뜻이 통한다.
 *
 * IRQ_RESEND 쪽이 정하는 것: 인터럽트를 켤 때, 꺼져 있는 동안 도착해
 * IRQS_PENDING 으로 남아 있던 것을 소프트웨어로 재전송할지 여부다. */
#define IRQ_RESEND	true	/* [한국어] 켜면서 밀린 인터럽트를 재전송한다 */
#define IRQ_NORESEND	false	/* [한국어] 재전송하지 않는다 — 처음 켜는 경우처럼 밀린 것이 있을 수 없을 때 */

/* [한국어] irq_startup() 의 force 인자에 이름을 붙인 상수 쌍.
 *
 * IRQ_START_FORCE 는 조건을 따지지 않고 무조건 켜라는 뜻이고, IRQ_START_COND
 * 는 조건이 맞을 때만 켜라는 뜻이다.
 *
 * 어떤 조건인가: managed 인터럽트(커널이 친화도를 관리하는 것)는 대상 CPU 가
 * 모두 오프라인이면 켜서는 안 된다. 갈 곳이 없는 인터럽트를 켜면 오류가 된다.
 * 조건부 시작은 그 경우 켜지 않고 managed shutdown 상태로 남겨 둔다. */
#define IRQ_START_FORCE	true	/* [한국어] 조건을 무시하고 켠다 — 사용자가 명시적으로 요청한 경우 */
#define IRQ_START_COND	false	/* [한국어] 조건이 맞을 때만 켠다 — managed 인터럽트의 CPU 가 온라인일 때 */

extern int irq_activate(struct irq_desc *desc);	/* [한국어] 계층형 도메인에서 이 인터럽트에 실제 하드웨어 자원(벡터 등)을 배정한다. 켜기 전에 반드시 거쳐야 한다 */
extern int irq_activate_and_startup(struct irq_desc *desc, bool resend);	/* [한국어] 자원 배정과 시작을 한 번에. 요청 경로가 쓰는 흔한 조합이다 */
extern int irq_startup(struct irq_desc *desc, bool resend, bool force);	/* [한국어] 인터럽트를 실제로 켠다. 위 두 상수 쌍이 이 함수의 인자에 이름을 준다 */
extern void irq_startup_managed(struct irq_desc *desc);	/* [한국어] managed 인터럽트를 CPU 가 온라인이 되어 다시 켤 수 있게 됐을 때 되살린다 */

extern void irq_shutdown(struct irq_desc *desc);	/* [한국어] 인터럽트를 끄고 하드웨어를 마스크한다. startup 의 짝 */
extern void irq_shutdown_and_deactivate(struct irq_desc *desc);	/* [한국어] 끄고 나서 배정된 하드웨어 자원까지 반납한다. activate_and_startup 의 짝 */
extern void irq_disable(struct irq_desc *desc);	/* [한국어] 인터럽트를 비활성 상태로 만든다. DISABLE_UNLAZY 가 없으면 하드웨어는 건드리지 않고 표시만 남긴다 */
extern void irq_percpu_enable(struct irq_desc *desc, unsigned int cpu);	/* [한국어] per-CPU 인터럽트를 지정한 CPU 에서만 켠다 */
extern void irq_percpu_disable(struct irq_desc *desc, unsigned int cpu);	/* [한국어] 그 짝. per-CPU 는 CPU 마다 따로 켜고 꺼야 한다 */
extern void mask_irq(struct irq_desc *desc);	/* [한국어] chip->irq_mask() 를 부르고 IRQD_IRQ_MASKED 를 세운다. 이미 마스크되어 있으면 아무 일도 하지 않는다 */
extern void unmask_irq(struct irq_desc *desc);	/* [한국어] 그 짝. 하드웨어 접근을 줄이려고 상태를 확인한 뒤에만 호출한다 */
extern void unmask_threaded_irq(struct irq_desc *desc);	/* [한국어] ONESHOT 인터럽트를 스레드 종료 후 언마스크한다. 보통의 unmask 와 달리 버스 락까지 다뤄야 한다 */

/* [한국어] 서술자가 "존재하는 것으로 표시"되었음을 알리는 함수.
 *
 * SPARSE_IRQ 구성에서는 할 일이 없다 — 서술자가 트리에 삽입되는 순간이 곧
 * 존재의 선언이라 따로 표시할 것이 없기 때문이다. 그래서 빈 인라인이다.
 *
 * SPARSE_IRQ 가 없으면 서술자가 정적 배열이라 전부 미리 존재한다. 그중
 * 어느 것이 실제로 쓰이는지를 따로 표시해야 하고, 그 일을 irqdesc.c 의
 * 실제 구현이 맡는다. */
#ifdef CONFIG_SPARSE_IRQ	/* [한국어] 트리로 관리하는 구성 */
static inline void irq_mark_irq(unsigned int irq) { }	/* [한국어] 트리 삽입 자체가 표시이므로 할 일이 없다 */
#else
extern void irq_mark_irq(unsigned int irq);	/* [한국어] 정적 배열 구성에서는 실제로 쓰이는 항목을 표시해야 한다 */
#endif

irqreturn_t __handle_irq_event_percpu(struct irq_desc *desc);	/* [한국어] action 목록을 돌며 핸들러를 부르는 알맹이. 반환값들을 OR 로 모은다 */
irqreturn_t handle_irq_event_percpu(struct irq_desc *desc);	/* [한국어] 위 알맹이에 통계와 오탐 감지를 덧붙인 판 */
irqreturn_t handle_irq_event(struct irq_desc *desc);	/* [한국어] 거기에 더해 IRQS_PENDING 정리와 서술자 락 해제·재획득까지 감싼 판. 흐름 제어 핸들러가 부르는 진입점이다 */

/* Resending of interrupts :*/
/* [한국어] (위 영어 주석) 인터럽트 재전송 관련.
 *
 * 왜 재전송이 필요한가: 인터럽트가 비활성화된 동안 하드웨어가 보낸 것을
 * 커널이 처리하지 못하고 IRQS_PENDING 으로 표시만 해 두는 경우가 있다.
 * 하드웨어는 그것을 다시 보내 주지 않으므로, 다시 켤 때 소프트웨어가
 * 인터럽트를 흉내 내 잃지 않게 한다. */
int check_irq_resend(struct irq_desc *desc, bool inject);	/* [한국어] 밀린 인터럽트가 있으면 재전송한다. inject 면 조건을 무시하고 강제로 — irq_inject_interrupt() 가 그 경로다 */
void clear_irq_resend(struct irq_desc *desc);	/* [한국어] 재전송 대기 목록에서 이 서술자를 뺀다. 서술자가 사라지기 전에 반드시 불러야 한다 */
void irq_resend_init(struct irq_desc *desc);	/* [한국어] 재전송에 쓸 목록 고리를 초기화한다. 서술자 생성 시 한 번 */
void __irq_wake_thread(struct irq_desc *desc, struct irqaction *action);	/* [한국어] 1차 핸들러가 IRQ_WAKE_THREAD 를 돌려줬을 때 스레드를 깨운다. IRQTF_RUNTHREAD 를 세우고 wake_up_process() */

void wake_threads_waitq(struct irq_desc *desc);	/* [한국어] 마지막 스레드가 끝나기를 기다리는 쪽(주로 free_irq 나 synchronize_irq)을 깨운다 */

/* [한국어] /proc/interrupts 와 /proc/irq/N/ 항목의 등록·해제.
 *
 * PROC_FS 를 뺀 커널에서는 넷 모두 빈 인라인이 된다. 이 관용구가 이 파일에
 * 반복해서 나타나는데, 목적은 언제나 같다 — 호출부(irqdesc.c 의 서술자 생성과
 * manage.c 의 핸들러 등록)에 #ifdef 를 심지 않고, 기능이 없는 빌드에서는
 * 호출 자체가 컴파일 후 사라지게 하는 것이다. */
#ifdef CONFIG_PROC_FS	/* [한국어] procfs 를 켠 빌드 */
extern void register_irq_proc(unsigned int irq, struct irq_desc *desc);	/* [한국어] /proc/irq/N/ 디렉터리와 affinity 등의 파일을 만든다 */
extern void unregister_irq_proc(unsigned int irq, struct irq_desc *desc);	/* [한국어] 그 디렉터리를 지운다 */
extern void register_handler_proc(unsigned int irq, struct irqaction *action);	/* [한국어] /proc/irq/N/<핸들러이름>/ 을 만든다. 공유 인터럽트에서 누가 붙어 있는지 보여 준다 */
extern void unregister_handler_proc(unsigned int irq, struct irqaction *action);	/* [한국어] 그 항목을 지운다 */
#else
static inline void register_irq_proc(unsigned int irq, struct irq_desc *desc) { }	/* [한국어] procfs 가 없으면 만들 것이 없다 */
static inline void unregister_irq_proc(unsigned int irq, struct irq_desc *desc) { }	/* [한국어] 지울 것도 없다 */
static inline void register_handler_proc(unsigned int irq,	/* [한국어] 핸들러 항목도 마찬가지 */
					 struct irqaction *action) { }	/* [한국어] 시그니처만 맞춘다 */
static inline void unregister_handler_proc(unsigned int irq,	/* [한국어] 그 해제도 할 일이 없다 */
					   struct irqaction *action) { }	/* [한국어] 시그니처만 맞춘다 */
#endif

extern bool irq_can_set_affinity_usr(unsigned int irq);	/* [한국어] 사용자가 /proc 로 친화도를 바꿔도 되는지. NO_BALANCING 비트와 chip 의 능력을 함께 본다 */

extern int irq_do_set_affinity(struct irq_data *data,	/* [한국어] 실제로 친화도를 바꾼다. chip->irq_set_affinity() 를 부르고 결과에 따라 상태를 갱신한다 */
			       const struct cpumask *dest, bool force);	/* [한국어] dest 는 목표 CPU 집합, force 는 온라인 CPU 검사를 건너뛸지 */
extern void irq_affinity_schedule_notify_work(struct irq_desc *desc);	/* [한국어] 친화도가 바뀌었음을 등록된 알림 대상(주로 드라이버)에게 워크큐로 알린다 — 알림 콜백이 잠들 수 있어 인터럽트 문맥에서 직접 부를 수 없다 */

/* [한국어] 인터럽트를 어느 CPU 에 배정할지 초기값을 정한다.
 *
 * 단일 프로세서(!SMP) 커널에서는 고를 CPU 가 하나뿐이라 할 일이 없어 0 을
 * 돌려주는 빈 구현이 된다. 반환값 0 은 성공을 뜻한다. */
#ifdef CONFIG_SMP	/* [한국어] 다중 프로세서 커널 */
extern int irq_setup_affinity(struct irq_desc *desc);	/* [한국어] 기본 친화도 마스크와 NUMA 노드를 보고 초기 CPU 를 고른다 */
#else
static inline int irq_setup_affinity(struct irq_desc *desc) { return 0; }	/* [한국어] CPU 가 하나뿐이라 고를 것이 없다. 0 은 성공 */
#endif

/* [한국어] 서술자에 등록된 action 목록을 순회하는 관용구.
 *
 * 공유 인터럽트에서는 한 번호에 여러 드라이버의 핸들러가 붙을 수 있고,
 * 그것들이 next 로 이어진 단일 연결 목록을 이룬다. 이 매크로가 그 순회를
 * 한 줄로 만든다.
 *
 * 동기화 주의: 이 목록을 읽는 동안 다른 CPU 가 free_irq 로 항목을 뺄 수
 * 있다. 그래서 호출자는 서술자 락을 쥐고 있거나(설정 경로), 인터럽트가
 * 비활성화된 문맥에 있어야(처리 경로) 한다. */
#define for_each_action_of_desc(desc, act)			\
	for (act = desc->action; act; act = act->next)	/* [한국어] action 목록을 next 로 따라가는 단순 순회. 목록이 NULL 로 끝나므로 종료 조건이 act 자체다 */

/* Inline functions for support of irq chips on slow busses */
/* [한국어] (위 영어 주석에 이어) 느린 버스 뒤에 있는 인터럽트 칩을 위한 잠금.
 *
 * 어떤 하드웨어인가: I2C 나 SPI 로 연결된 GPIO 확장기, PMIC 같은 칩이
 * 인터럽트 컨트롤러 노릇을 하는 경우다. 그 칩의 레지스터를 읽고 쓰려면
 * 버스 트랜잭션이 필요하고, 그것은 잠들 수 있는 동작이다.
 *
 * 그래서 마스크·언마스크 같은 조작을 인터럽트 문맥에서 할 수 없고, 대신
 * chip 드라이버가 요청을 모아 두었다가 sync_unlock 시점에 한꺼번에 버스로
 * 내보낸다. 아래 두 함수가 그 구간의 시작과 끝을 표시한다.
 *
 * 짝을 반드시 맞춰야 한다 — bus_lock 만 하고 sync_unlock 을 빠뜨리면 모아
 * 둔 변경이 영원히 하드웨어에 도달하지 않는다. */
static inline void chip_bus_lock(struct irq_desc *desc)
{
	if (unlikely(desc->irq_data.chip->irq_bus_lock))	/* [한국어] 대부분의 칩은 이 콜백이 없다 — unlikely 로 분기 예측을 그쪽에 맞춘다 */
		desc->irq_data.chip->irq_bus_lock(&desc->irq_data);	/* [한국어] 있으면 버스 뮤텍스를 잡는다. 여기서 잠들 수 있으므로 호출자는 프로세스 문맥이어야 한다 */
}

/*
 * [한국어]
 * chip_bus_sync_unlock - 모아 둔 칩 조작을 실제 버스로 내보내고 잠금을 푼다
 *
 * @desc: 대상 서술자.
 *
 * 위 chip_bus_lock 의 짝이다. 이름에 sync 가 들어간 이유가 핵심이다 — 단순히
 * 잠금을 푸는 것이 아니라, lock 이후 쌓인 마스크·트리거 변경 요청을 이 시점에
 * 실제 버스 트랜잭션으로 내보낸다.
 *
 * 왜 모아서 내보내는가: I2C 트랜잭션 하나가 수백 마이크로초 걸린다. 마스크와
 * 언마스크를 매번 따로 보내면 인터럽트 처리가 그 시간에 묶인다. 여러 변경을
 * 한 트랜잭션에 합치면 왕복 횟수가 줄어든다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   __setup_irq()/__free_irq()/irq_set_type() 등 (kernel/irq/manage.c)
 *     → [이 함수] → chip->irq_bus_sync_unlock() → 실제 I2C/SPI 전송
 */
static inline void chip_bus_sync_unlock(struct irq_desc *desc)
{
	if (unlikely(desc->irq_data.chip->irq_bus_sync_unlock))	/* [한국어] lock 쪽과 짝이 맞아야 한다 — 한쪽만 있는 칩은 없다 */
		desc->irq_data.chip->irq_bus_sync_unlock(&desc->irq_data);	/* [한국어] 모아 둔 변경을 버스로 내보내고 뮤텍스를 푼다 */
}

/* [한국어] 서술자를 찾을 때 어떤 검사를 요구할지 나타내는 비트들.
 *
 * 아래 __irq_get_desc_lock() 의 check 인자에 쓰인다. 검사 종류를 비트로 둔
 * 이유는 조합이 필요하기 때문이다 — 아래 PERCPU 검사는 CHECK 를 포함한다. */
#define _IRQ_DESC_CHECK		(1 << 0)	/* [한국어] 이 번호에 드라이버가 요청할 수 있는 서술자인지 확인하라(can_request) */
#define _IRQ_DESC_PERCPU	(1 << 1)	/* [한국어] per-CPU devid 서술자인지 확인하라 */

/* [한국어] 위 비트들을 호출부에서 쓰기 좋게 묶은 이름.
 *
 * GLOBAL 은 보통의 인터럽트를 다루는 API 가 쓰고, PERCPU 는 request_percpu_irq
 * 계열이 쓴다. PERCPU 가 두 비트를 모두 갖는 것은 "요청 가능한 서술자이면서
 * per-CPU 여야 한다"는 뜻이다 — per-CPU API 에 보통 인터럽트 번호를 넘기는
 * 실수를 잡아내는 것이 목적이다. */
#define IRQ_GET_DESC_CHECK_GLOBAL	(_IRQ_DESC_CHECK)	/* [한국어] 보통의 인터럽트 API 용 검사 */
#define IRQ_GET_DESC_CHECK_PERCPU	(_IRQ_DESC_CHECK | _IRQ_DESC_PERCPU)	/* [한국어] per-CPU API 용 — 둘 다 만족해야 한다 */

struct irq_desc *__irq_get_desc_lock(unsigned int irq, unsigned long *flags, bool bus,	/* [한국어] 번호로 서술자를 찾아 락(과 필요하면 버스 락)까지 잡아 돌려준다. 실패하면 NULL */
				     unsigned int check);	/* [한국어] 위 IRQ_GET_DESC_CHECK_* 중 하나 */
void __irq_put_desc_unlock(struct irq_desc *desc, unsigned long flags, bool bus);	/* [한국어] 그 짝. 잡은 순서의 역순으로 푼다 */

/* [한국어] 아래 세 줄이 서술자 락을 "블록을 벗어나면 자동으로 푸는" 형태로 만든다.
 *
 * 커널의 cleanup 기반 guard 기능을 쓴 것이다. 세 매크로가 하는 일:
 *   __DEFINE_CLASS_IS_CONDITIONAL — 이 guard 는 획득이 실패할 수 있다고 선언한다.
 *     서술자를 못 찾으면 NULL 이 되므로, 컴파일러가 그 경우를 다룰 수 있게 한다.
 *   __DEFINE_UNLOCK_GUARD — 블록을 벗어날 때 부를 해제 코드와, guard 가 들고
 *     다닐 추가 상태(flags 와 bus)를 정의한다.
 *
 * 왜 이런 장치가 필요한가: 서술자 락을 잡는 함수는 실패·조기 반환 경로가 많다.
 * 그때마다 unlock 을 빠뜨리지 않고 적어야 하는데, 경로가 늘어날수록 실수가
 * 생긴다. guard 를 쓰면 return 이 어디에 있든 해제가 보장된다.
 *
 * 추가 상태로 flags 와 bus 를 함께 들고 다니는 이유: 해제할 때 그 둘이 필요한데,
 * 호출부가 따로 변수를 두면 guard 의 이점이 반감된다. */
__DEFINE_CLASS_IS_CONDITIONAL(irqdesc_lock, true);
__DEFINE_UNLOCK_GUARD(irqdesc_lock, struct irq_desc,	/* [한국어] guard 의 해제 코드와 추가 상태를 정의한다. _T 는 guard 객체를 가리키는 매크로 안의 이름이며, 여기서 flags 와 bus 를 꺼내 해제 함수에 넘긴다 */
		      __irq_put_desc_unlock(_T->lock, _T->flags, _T->bus),
		      unsigned long flags; bool bus);

/*
 * [한국어]
 * class_irqdesc_lock_constructor - irqdesc guard 의 획득부
 *
 * @irq:    찾을 인터럽트 번호.
 * @bus:    느린 버스 칩의 버스 락까지 함께 잡을지. 위 chip_bus_lock 참고.
 * @check:  IRQ_GET_DESC_CHECK_GLOBAL 또는 _PERCPU.
 * @return: guard 객체. 그 안의 lock 필드가 NULL 이면 서술자를 못 찾았거나
 *          검사에 실패한 것이다.
 *
 * 위 __DEFINE_UNLOCK_GUARD 가 정의한 guard 의 짝이 되는 생성자다. scoped_guard
 * 매크로가 블록에 들어갈 때 이것을 부르고, 나갈 때 __irq_put_desc_unlock 을 부른다.
 *
 * bus 를 먼저 구조체에 넣고 그 다음에 lock 을 잡는 순서에 주의한다. 획득이
 * 실패해 lock 이 NULL 이 되더라도 해제 코드는 실행되는데, 그때 bus 값이 이미
 * 들어 있어야 해제 쪽이 올바르게 동작한다.
 *
 * 실행 컨텍스트: 프로세스 문맥(bus 가 참이면 잠들 수 있다) 또는 인터럽트
 * 문맥(bus 가 거짓일 때). __irq_get_desc_lock 이 irqsave 로 잡는다.
 *
 * 호출 체인:
 *   scoped_irqdesc_get_and_lock() 매크로 → scoped_guard → [이 함수]
 */
static inline class_irqdesc_lock_t class_irqdesc_lock_constructor(unsigned int irq, bool bus,
								  unsigned int check)
{
	class_irqdesc_lock_t _t = { .bus = bus, };	/* [한국어] bus 를 먼저 채운다 — 획득이 실패해도 해제 코드가 이 값을 보기 때문이다 */

	_t.lock = __irq_get_desc_lock(irq, &_t.flags, bus, check);	/* [한국어] 서술자를 찾고 락을 잡는다. flags 는 여기서 채워져 해제 때 쓰인다 */

	return _t;	/* [한국어] lock 이 NULL 일 수 있다 — 호출부가 scoped_irqdesc 로 확인해야 한다 */
}

/* [한국어] 서술자를 번호로 찾아 락을 잡고, 블록을 벗어나면 자동으로 푸는 관용구.
 *
 * 쓰는 법:
 *   scoped_irqdesc_get_and_lock(irq, 0) {
 *           struct irq_desc *desc = scoped_irqdesc;
 *           if (!desc) return -EINVAL;
 *           ...
 *   }
 *
 * 블록 안 어디서 return 하든 락이 풀린다. 서술자 락은 실패 경로가 많은
 * 함수들에서 다뤄지므로 이 보장이 특히 유용하다. */
#define scoped_irqdesc_get_and_lock(_irq, _check)		\
	scoped_guard(irqdesc_lock, _irq, false, _check)

/* [한국어] 위와 같지만 느린 버스 칩의 버스 락까지 함께 잡는 판.
 *
 * 언제 이쪽을 쓰는가: 블록 안에서 chip 의 마스크·트리거 설정을 건드릴 때다.
 * I2C 뒤의 칩이라면 그 조작이 버스 트랜잭션을 일으키므로 버스 락이 필요하다.
 *
 * 대가: 버스 락은 잠들 수 있는 뮤텍스라, 이 판은 프로세스 문맥에서만 쓸 수 있다. */
#define scoped_irqdesc_get_and_buslock(_irq, _check)		\
	scoped_guard(irqdesc_lock, _irq, true, _check)

/* [한국어] 위 두 관용구 안에서 서술자 포인터를 꺼내는 이름.
 *
 * scoped_guard 가 만든 숨은 변수 scope 에서 guard 가 들고 있는 포인터를 꺼내
 * struct irq_desc * 로 돌려준다. NULL 일 수 있으므로 블록 첫머리에서 반드시
 * 확인해야 한다 — 그 확인을 빠뜨리는 것이 이 관용구의 유일한 함정이다. */
#define scoped_irqdesc		((struct irq_desc *)(__guard_ptr(irqdesc_lock)(&scope)))

/* [한국어] irq_data 의 상태 워드에 닿는 유일한 통로.
 *
 * 실제 필드 이름은 state_use_accessors 이고 ACCESS_PRIVATE 로 감싸여 있어,
 * 이 매크로 없이는 접근할 수 없다. 이름 자체가 "접근자를 써라"는 뜻이다.
 *
 * 중요한 점: 이 매크로는 아래 접근자들을 정의하는 데만 쓰이고, 그 정의가
 * 끝나면 곧바로 #undef 된다. 그래서 이 파일의 이 구간 밖에서는 존재하지
 * 않으며, 다른 곳의 코드는 irqd_set()/irqd_clear() 같은 접근자를 쓸 수밖에 없다.
 *
 * settings.h 의 GOT_YOU_MORON 봉인과 같은 취지이되, 수단이 다르다 — 그쪽은
 * 이름을 못 쓰게 덮어쓰고, 이쪽은 필요한 만큼만 쓰고 치운다. */
#define __irqd_to_state(d) ACCESS_PRIVATE((d)->common, state_use_accessors)

/*
 * [한국어]
 * irqd_get - irq_data 의 상태 워드 전체를 읽는다
 *
 * @d:      대상 irq_data.
 * @return: IRQD_* 비트들의 조합.
 *
 * 개별 비트를 묻는 아래 has_set 과 달리 워드 전체를 돌려준다. 진단 출력처럼
 * 여러 비트를 한꺼번에 봐야 하는 곳에서 쓴다.
 *
 * IRQD_* 상태에는 무엇이 있는가: 비활성 여부, 마스크 여부, 친화도 변경 대기,
 * activated 여부, managed shutdown, wakeup 설정 등 하드웨어 쪽 상태다.
 * 코어의 처리 상태(IRQS_*)와 설정(_IRQ_*)이 각각 다른 워드에 있다는 점이
 * 이 서브시스템을 읽을 때 가장 헷갈리는 부분이다.
 *
 * 실행 컨텍스트: 인라인. 락 없이 읽는다 — 진단 목적이라 순간의 값이면 충분하다.
 *
 * 호출 체인:
 *   irq_debug_show_data() (kernel/irq/debugfs.c) → [이 함수]
 */
static inline unsigned int irqd_get(struct irq_data *d)
{
	return __irqd_to_state(d);	/* [한국어] 위 매크로가 ACCESS_PRIVATE 를 풀어 실제 필드에 닿게 해 준다 */
}

/*
 * Manipulation functions for irq_data.state
 */
/* [한국어] (위 영어 주석) irq_data.state 를 다루는 함수들.
 *
 * 아래 접근자들이 모두 위 __irqd_to_state 매크로를 쓰고, 이 묶음이 끝나는
 * 지점에서 그 매크로가 #undef 된다. 즉 여기가 그 상태 워드에 직접 닿을 수
 * 있는 유일한 구간이다.
 *
 * 동기화 규칙: 이 워드는 서술자 락 아래에서 바꾼다. 접근자 자체는 락을 잡지
 * 않으므로 호출자가 책임진다. 읽기는 진단 목적이면 락 없이도 한다. */

/*
 * [한국어]
 * irqd_set_move_pending - 친화도 변경이 대기 중임을 표시한다
 *
 * @d: 대상 irq_data.
 *
 * 무슨 상황인가: 인터럽트의 친화도를 바꾸려는데, 지금 바로 바꾸면 안 되는
 * 하드웨어가 있다. 인터럽트가 이미 날아오는 중일 때 목적지를 바꾸면 그
 * 인터럽트가 유실되거나 두 CPU 에 중복으로 전달될 수 있기 때문이다.
 *
 * 그래서 그런 칩(IRQCHIP_MOVE_DEFERRED)에서는 새 친화도를 desc->pending_mask 에
 * 적어 두고 이 비트를 세운다. 실제 변경은 다음 인터럽트를 처리하고 ack 를
 * 보낸 직후, 즉 "지금은 확실히 날아오는 인터럽트가 없다"고 말할 수 있는
 * 순간에 이루어진다. 그 일을 irq_move_irq()/irq_move_masked_irq() 가 한다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락 아래.
 *
 * 호출 체인:
 *   irq_set_affinity_locked() → irq_do_set_affinity() (kernel/irq/manage.c) → [이 함수]
 *   나중에 handle_edge_irq() 등이 irq_move_irq() 로 실제 이동을 완료
 */
static inline void irqd_set_move_pending(struct irq_data *d)
{
	__irqd_to_state(d) |= IRQD_SETAFFINITY_PENDING;	/* [한국어] 실제 이동은 미루고 표시만 남긴다 — 목표 CPU 는 desc->pending_mask 에 있다 */
}

/*
 * [한국어]
 * irqd_clr_move_pending - 친화도 변경 대기 표시를 지운다
 *
 * @d: 대상 irq_data.
 *
 * 위 set 의 짝. 미뤄 둔 이동을 실제로 마쳤을 때, 또는 이동을 포기했을 때 부른다.
 *
 * 포기하는 경우가 있는가: 있다. 목표 CPU 가 그 사이 오프라인이 되면 그리로
 * 옮길 수 없다. irq_fixup_move_pending() 이 그런 상황을 정리하며 이 비트를 지운다.
 *
 * 이 비트를 제때 지우지 않으면 인터럽트를 처리할 때마다 이동을 시도하게 되어,
 * 핫패스에 쓸데없는 작업이 붙는다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락 아래, 또는 인터럽트 처리 중.
 *
 * 호출 체인:
 *   irq_move_masked_irq() (kernel/irq/migration.c) → [이 함수]
 */
static inline void irqd_clr_move_pending(struct irq_data *d)
{
	__irqd_to_state(d) &= ~IRQD_SETAFFINITY_PENDING;	/* [한국어] 이동을 마쳤거나 포기했다 — 그대로 두면 핫패스가 매번 이동을 시도한다 */
}

/*
 * [한국어]
 * irqd_set_managed_shutdown - managed 인터럽트가 CPU 부재로 꺼져 있음을 표시한다
 *
 * @d: 대상 irq_data.
 *
 * managed 인터럽트란: 커널이 친화도를 직접 관리하는 인터럽트다. 대표적으로
 * 멀티큐 장치(NVMe, 최신 NIC)의 큐마다 하나씩 있는 인터럽트로, 각 큐가
 * 특정 CPU 집합에 묶인다. 사용자가 친화도를 바꿀 수 없다.
 *
 * 무슨 문제를 푸는가: 그 CPU 집합이 전부 오프라인이 되면 인터럽트를 보낼
 * 곳이 없다. 보통의 인터럽트라면 다른 CPU 로 옮기면 되지만, managed 는
 * 그 큐가 특정 CPU 의 것이라 옮기는 것이 의미가 없다.
 *
 * 그래서 인터럽트를 끄되, "고장나서 끈 것이 아니라 CPU 가 없어서 끈 것"이라고
 * 표시해 둔다. 나중에 그 CPU 중 하나가 온라인이 되면 irq_startup_managed()
 * 가 이 표시를 보고 자동으로 되살린다.
 *
 * 실행 컨텍스트: 인라인. CPU 핫플러그 경로, 서술자 락 아래.
 *
 * 호출 체인:
 *   irq_shutdown()/migrate_one_irq() (kernel/irq/cpuhotplug.c) → [이 함수]
 */
static inline void irqd_set_managed_shutdown(struct irq_data *d)
{
	__irqd_to_state(d) |= IRQD_MANAGED_SHUTDOWN;	/* [한국어] "CPU 가 없어서 껐다" — CPU 가 돌아오면 자동으로 되살릴 근거가 된다 */
}

/*
 * [한국어]
 * irqd_clr_managed_shutdown - managed shutdown 표시를 지운다
 *
 * @d: 대상 irq_data.
 *
 * 위 set 의 짝. CPU 가 온라인이 되어 인터럽트를 되살렸을 때, 또는 그 인터럽트
 * 자체가 해제될 때 부른다.
 *
 * 이 표시가 남아 있으면 무슨 일이 생기는가: 다음 CPU 온라인 이벤트에서
 * 이미 켜져 있는 인터럽트를 또 켜려 하게 된다. startup 은 멱등하지 않아
 * 참조 계수나 하드웨어 상태가 어긋날 수 있다.
 *
 * 실행 컨텍스트: 인라인. CPU 핫플러그 또는 해제 경로, 서술자 락 아래.
 *
 * 호출 체인:
 *   irq_startup()/irq_startup_managed() (kernel/irq/chip.c) → [이 함수]
 */
static inline void irqd_clr_managed_shutdown(struct irq_data *d)
{
	__irqd_to_state(d) &= ~IRQD_MANAGED_SHUTDOWN;	/* [한국어] 되살렸거나 해제했다 — 남겨 두면 다음 온라인에서 중복 startup 이 일어난다 */
}

/*
 * [한국어]
 * irqd_clear - 상태 워드에서 지정한 비트들을 지운다
 *
 * @d:    대상 irq_data.
 * @mask: 지울 IRQD_* 비트들의 조합.
 *
 * 위의 이름 붙은 접근자들과 달리 임의의 비트를 다루는 일반형이다. 특정
 * 비트를 자주 다루는 곳은 전용 접근자를 만들고, 드물게 쓰거나 여러 비트를
 * 한 번에 다뤄야 하는 곳은 이 일반형을 쓴다.
 *
 * settings.h 의 clr_and_set 과 달리 안전 마스크로 거르지 않는다. 이 함수를
 * 부르는 것은 코어 자신뿐이라(공개 API 에 노출되지 않는다) 잘못된 비트가
 * 올 일이 없다고 보기 때문이다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락 아래.
 *
 * 호출 체인:
 *   irq_startup()/irq_shutdown()/irq_set_affinity 계열 → [이 함수]
 */
static inline void irqd_clear(struct irq_data *d, unsigned int mask)
{
	__irqd_to_state(d) &= ~mask;	/* [한국어] 안전 마스크로 거르지 않는다 — 코어 안에서만 쓰이므로 */
}

/*
 * [한국어]
 * irqd_set - 상태 워드에 지정한 비트들을 세운다
 *
 * @d:    대상 irq_data.
 * @mask: 세울 IRQD_* 비트들의 조합.
 *
 * 위 irqd_clear 의 짝이며, 같은 이유로 마스크 검사가 없다.
 *
 * 아래 irq_state_set_disabled/masked 가 이 함수를 감싼 얇은 래퍼인데, 그렇게
 * 이름을 따로 둔 이유는 호출부에서 IRQD_ 상수를 직접 쓰지 않게 하려는 것이다.
 * 자주 쓰이는 조작에 이름을 붙이면 오타로 엉뚱한 비트를 세우는 실수가 준다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락 아래.
 *
 * 호출 체인:
 *   irq_startup()/irq_shutdown()/irq_set_affinity 계열 → [이 함수]
 */
static inline void irqd_set(struct irq_data *d, unsigned int mask)
{
	__irqd_to_state(d) |= mask;	/* [한국어] 여러 비트를 한 번에 세울 수 있다 */
}

/*
 * [한국어]
 * irqd_has_set - 지정한 비트가 하나라도 서 있는지 묻는다
 *
 * @d:      대상 irq_data.
 * @mask:   확인할 IRQD_* 비트들.
 * @return: 마스크의 비트 중 하나라도 서 있으면 참.
 *
 * "모두 서 있는가"가 아니라 "하나라도 서 있는가"라는 점에 주의한다. 여러
 * 비트를 넘기면 OR 판정이 된다.
 *
 * 그래서 여러 비트를 넘기는 호출은 대개 "이 상태들 중 어느 하나라도 해당하면
 * 특별 처리" 라는 뜻이다. 예를 들어 인터럽트를 옮길 수 있는지 볼 때 여러
 * 금지 조건을 한 번에 검사한다.
 *
 * 실행 컨텍스트: 인라인. 락 없이 읽거나 서술자 락 아래에서.
 *
 * 호출 체인:
 *   irq_set_affinity 계열, chip.c 의 여러 판정 → [이 함수]
 */
static inline bool irqd_has_set(struct irq_data *d, unsigned int mask)
{
	return __irqd_to_state(d) & mask;	/* [한국어] AND 결과가 0 이 아니면 참 — "모두"가 아니라 "하나라도"다 */
}

/*
 * [한국어]
 * irq_state_set_disabled - 이 인터럽트를 비활성 상태로 표시한다
 *
 * @desc: 대상 서술자. irq_data 가 아니라 서술자를 받는 것에 주의.
 *
 * 위 irqd_set 을 감싼 래퍼다. 인자 타입이 다른 이유는 호출부의 편의다 —
 * 이 조작을 하는 곳은 대개 서술자를 들고 있어서, 매번 &desc->irq_data 를
 * 쓰는 것보다 이쪽이 읽기 좋다.
 *
 * IRQD_IRQ_DISABLED 의 뜻: 이 인터럽트는 논리적으로 꺼져 있다. 하드웨어가
 * 실제로 마스크되어 있는지는 별개이며(게으른 비활성화 참고), 그것은 아래
 * IRQD_IRQ_MASKED 가 나타낸다. 두 상태를 나누어 두는 것이 이 서브시스템의
 * 성능 설계의 핵심이다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락 아래.
 *
 * 호출 체인:
 *   irq_disable()/irq_shutdown() (kernel/irq/chip.c) → [이 함수]
 */
static inline void irq_state_set_disabled(struct irq_desc *desc)
{
	irqd_set(&desc->irq_data, IRQD_IRQ_DISABLED);	/* [한국어] 논리적 비활성. 하드웨어 마스크 여부는 아래 MASKED 가 따로 나타낸다 */
}

/*
 * [한국어]
 * irq_state_set_masked - 하드웨어가 실제로 마스크되었음을 표시한다
 *
 * @desc: 대상 서술자.
 *
 * 위 set_disabled 와 짝을 이루는 래퍼다. 두 상태의 차이가 게으른 비활성화의
 * 근거가 된다:
 *
 *   DISABLED 만 서 있음 — 논리적으로 꺼졌지만 하드웨어는 아직 열려 있다.
 *     인터럽트가 들어오면 그때 마스크하고 핸들러는 부르지 않는다.
 *   DISABLED + MASKED  — 하드웨어까지 실제로 마스크했다.
 *   MASKED 만 서 있음  — 레벨 인터럽트를 처리하는 동안의 정상 상태다.
 *
 * 이렇게 나누어 두면 disable_irq() 가 컨트롤러를 건드리지 않고 끝나, 대부분의
 * 경우 비용이 사라진다. 하드웨어 접근은 인터럽트가 실제로 들어왔을 때만 한다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락 아래.
 *
 * 호출 체인:
 *   mask_irq() (kernel/irq/chip.c) → [이 함수]
 */
static inline void irq_state_set_masked(struct irq_desc *desc)
{
	irqd_set(&desc->irq_data, IRQD_IRQ_MASKED);	/* [한국어] 하드웨어를 실제로 마스크했다는 기록. DISABLED 와 나뉘어 있는 것이 게으른 비활성화의 토대다 */
}

/* [한국어] 여기서 매크로를 치운다.
 *
 * 위 접근자들을 정의하는 데만 필요했고, 이 지점부터는 아무도 상태 워드에
 * 직접 닿지 못하게 한다. 이 파일의 아래쪽 코드도, 이 헤더를 포함하는 .c
 * 파일들도 모두 irqd_set()/irqd_clear()/irqd_has_set() 을 거쳐야 한다.
 *
 * settings.h 가 이름을 덮어써 봉인하는 것과 달리, 이쪽은 필요한 만큼만
 * 쓰고 없애는 방식이다. 결과는 같다 — 상태 워드를 다루는 규칙이 한 곳에 모인다. */
#undef __irqd_to_state

/*
 * [한국어]
 * __kstat_incr_irqs_this_cpu - 이 CPU 의 인터럽트 횟수를 센다 (밑줄 판)
 *
 * @desc: 대상 서술자.
 *
 * 두 곳을 함께 올린다. 서술자마다의 per-CPU 카운터와, 시스템 전체의 인터럽트
 * 총합이다. 앞은 /proc/interrupts 의 각 줄에, 뒤는 /proc/stat 의 intr 항목에 쓰인다.
 *
 * __this_cpu_inc 를 쓰는 이유: 이 함수는 인터럽트가 비활성화된 문맥에서만
 * 불린다. 그러면 선점이나 다른 인터럽트가 끼어들 수 없어, 선점 검사가 붙은
 * this_cpu_inc 대신 검사 없는 판을 쓸 수 있다. 인터럽트마다 불리는 핫패스라
 * 그 차이가 의미가 있다.
 *
 * 밑줄이 붙은 이유는 아래 kstat_incr_irqs_this_cpu 와 구분하기 위해서다.
 * 이쪽은 tot_count 를 올리지 않는다 — 그 이유는 아래 함수 주석에 있다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 인터럽트가 비활성화되어 있어야 한다.
 *
 * 호출 체인:
 *   handle_percpu_irq()/handle_percpu_devid_irq() (kernel/irq/chip.c) → [이 함수]
 */
static inline void __kstat_incr_irqs_this_cpu(struct irq_desc *desc)
{
	__this_cpu_inc(desc->kstat_irqs->cnt);	/* [한국어] 이 서술자의 이 CPU 몫. /proc/interrupts 의 CPU 별 열이 된다 */
	__this_cpu_inc(kstat.irqs_sum);	/* [한국어] 시스템 전체 총합. /proc/stat 의 intr 첫 숫자가 된다 */
}

/*
 * [한국어]
 * kstat_incr_irqs_this_cpu - 인터럽트 횟수를 세고 합계 캐시도 갱신한다
 *
 * @desc: 대상 서술자.
 *
 * 위 밑줄 판에 desc->tot_count 갱신을 더한 것이다. 이 차이가 성능 설계의 한 조각이다.
 *
 * tot_count 가 무엇인가: 이 서술자의 모든 CPU 카운터를 합친 값의 캐시다.
 * /proc/interrupts 를 읽을 때마다 CPU 수만큼 per-CPU 변수를 더하는 것은
 * CPU 가 수백 개인 기계에서 무시할 수 없는 비용이라, 세는 쪽에서 미리
 * 합계를 유지해 둔다.
 *
 * 왜 두 판으로 나뉘는가: tot_count 는 per-CPU 가 아닌 보통의 변수라, 여러
 * CPU 가 동시에 올리면 값이 어긋난다. per-CPU 인터럽트(로컬 타이머, IPI)는
 * 정의상 여러 CPU 에서 동시에 들어오므로 이 갱신을 하면 안 되고, 그래서
 * 위 밑줄 판을 쓴다. 그 대신 그런 인터럽트의 합계는 읽을 때 직접 더한다.
 *
 * 보통의 인터럽트는 한 번에 한 CPU 에서만 처리되므로 이 갱신이 안전하다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 인터럽트가 비활성화되어 있어야 한다.
 *
 * 호출 체인:
 *   handle_level_irq()/handle_edge_irq()/handle_fasteoi_irq() (kernel/irq/chip.c) → [이 함수]
 */
static inline void kstat_incr_irqs_this_cpu(struct irq_desc *desc)
{
	__kstat_incr_irqs_this_cpu(desc);	/* [한국어] per-CPU 카운터와 전역 총합을 먼저 올린다 */
	desc->tot_count++;	/* [한국어] 합계 캐시. per-CPU 인터럽트에서는 여러 CPU 가 다투므로 이 판을 쓰면 안 된다 */
}

/*
 * [한국어]
 * irq_desc_get_node - 이 서술자가 속한 NUMA 노드를 알아낸다
 *
 * @desc:   대상 서술자.
 * @return: NUMA 노드 번호. NUMA 가 아닌 시스템에서는 0.
 *
 * 무엇에 쓰는가: 서술자에 딸린 메모리(per-CPU 카운터, pending 마스크 등)를
 * 할당할 때 그 노드에서 잡는다. 인터럽트를 처리하는 CPU 와 그 자료가 같은
 * 노드에 있으면 접근이 빠르다.
 *
 * 노드는 어떻게 정해지는가: 서술자를 만들 때 요청자가 지정한다. 보통은 그
 * 인터럽트를 낼 장치가 붙어 있는 PCI 버스의 노드를 쓴다 — 장치와 인터럽트
 * 처리를 같은 노드에 두는 것이 데이터가 오가는 거리를 줄인다.
 *
 * 실행 컨텍스트: 인라인. 어디서든.
 *
 * 호출 체인:
 *   alloc_masks()/irq_expand_nr_irqs() (kernel/irq/irqdesc.c) → [이 함수]
 */
static inline int irq_desc_get_node(struct irq_desc *desc)
{
	return irq_common_data_get_node(&desc->irq_common_data);	/* [한국어] 노드 번호는 irq_common_data 에 들어 있다 — irq_data 와 공유되는 부분이다 */
}

/*
 * [한국어]
 * irq_desc_is_chained - 이 서술자가 연쇄(chained) 인터럽트인지 묻는다
 *
 * @desc:   대상 서술자.
 * @return: 연쇄면 0 이 아닌 값, 아니면 0.
 *
 * 연쇄 인터럽트란: 인터럽트 컨트롤러가 다른 컨트롤러에 물려 있는 구조에서,
 * 상위 컨트롤러의 그 선을 가리킨다. 예를 들어 GPIO 컨트롤러 전체가 SoC
 * 인터럽트 컨트롤러의 한 선에 물려 있으면, 그 선이 연쇄 인터럽트다.
 *
 * 무엇이 다른가: 보통의 인터럽트는 action 목록의 드라이버 핸들러를 부르지만,
 * 연쇄는 흐름 제어 핸들러가 곧바로 하위 컨트롤러의 상태를 읽어 진짜 인터럽트를
 * 찾아 내려간다. 그래서 드라이버 핸들러가 없다.
 *
 * 어떻게 판별하는가: action 자리에 진짜 핸들러 대신 전역 chained_action 의
 * 주소를 넣어 두고, 그 주소를 비교한다. 내용이 아니라 주소를 비교하는 것이
 * 요점이다 — 이 표식은 단 하나뿐이므로 주소만으로 유일하게 식별된다.
 *
 * 왜 별도의 플래그 비트를 쓰지 않았는가: action 이 NULL 인지 아닌지를 보는
 * 코드가 이미 곳곳에 있는데, 연쇄 인터럽트에 NULL 을 두면 그 코드들이
 * "핸들러가 없다"로 오해한다. 더미 action 을 두면 그 코드들이 그대로 동작한다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락 아래에서 읽는 것이 안전하다.
 *
 * 호출 체인:
 *   irq_shutdown()/proc.c 의 출력 → [이 함수]
 */
static inline int irq_desc_is_chained(struct irq_desc *desc)
{
	return (desc->action && desc->action == &chained_action);	/* [한국어] 내용이 아니라 주소를 비교한다 — 이 표식은 전역에 하나뿐이다 */
}

/*
 * [한국어]
 * irq_is_nmi - 이 인터럽트 선이 NMI 전달에 쓰이는지 묻는다
 *
 * @desc:   대상 서술자.
 * @return: NMI 면 참.
 *
 * NMI(Non-Maskable Interrupt)는 마스크할 수 없는 인터럽트다. 워치독이나
 * 프로파일러처럼 시스템이 멈춘 상태에서도 반드시 들어와야 하는 것에 쓴다.
 *
 * 왜 특별 취급이 필요한가: NMI 핸들러는 다른 인터럽트가 락을 쥔 채 멈춘
 * 상황에서도 실행될 수 있다. 그래서 어떤 락도 잡을 수 없고, 스레드화할 수도
 * 없으며, 보통의 마스크·언마스크 절차도 쓸 수 없다. 요청 단계부터
 * request_nmi() 라는 별도 API 를 거치고, 제약을 만족하지 못하면 거절된다.
 *
 * istate 를 직접 읽는 몇 안 되는 인라인 함수 중 하나다. 위 #define istate 가
 * 그 긴 필드 이름을 짧게 만들어 주고 있어 이렇게 쓸 수 있다.
 *
 * 실행 컨텍스트: 인라인. 어디서든 — NMI 문맥에서도 불릴 수 있어 락을 잡지 않는다.
 *
 * 호출 체인:
 *   __free_irq()/irq_set_irq_type() 등 여러 제약 검사 → [이 함수]
 */
static inline bool irq_is_nmi(struct irq_desc *desc)
{
	return desc->istate & IRQS_NMI;	/* [한국어] 위 #define 이 core_internal_state__do_not_mess_with_it 을 istate 로 줄여 준다 */
}

/* [한국어] 절전(suspend/resume) 관련 훅 셋.
 *
 * PM_SLEEP 을 뺀 커널에서는 빈 인라인이 된다. 임베디드나 서버 구성에서
 * 시스템 절전을 아예 쓰지 않으면 이 코드가 통째로 사라진다.
 *
 * 세 함수가 하는 일:
 *   handle_wakeup   — 절전 중 들어온 인터럽트를 깨우기 원인으로 기록한다.
 *   install_action  — 핸들러를 등록할 때 그 인터럽트의 절전 정책을 갱신한다.
 *   remove_action   — 해제할 때 되돌린다.
 *
 * 정책이란: IRQF_NO_SUSPEND 로 요청된 핸들러가 하나라도 있으면 그 인터럽트는
 * 절전 중에도 살아 있어야 한다. 그래서 핸들러가 붙고 빠질 때마다 서술자의
 * 카운트를 다시 세어야 한다. */
#ifdef CONFIG_PM_SLEEP	/* [한국어] 시스템 절전을 지원하는 빌드 */
void irq_pm_handle_wakeup(struct irq_desc *desc);	/* [한국어] 절전 중 도착한 인터럽트를 깨우기 원인으로 기록하고 절전을 중단시킨다 */
void irq_pm_install_action(struct irq_desc *desc, struct irqaction *action);	/* [한국어] 핸들러 등록 시 NO_SUSPEND/FORCE_RESUME 카운트를 갱신한다 */
void irq_pm_remove_action(struct irq_desc *desc, struct irqaction *action);	/* [한국어] 해제 시 그 카운트를 되돌린다 */
#else
static inline void irq_pm_handle_wakeup(struct irq_desc *desc) { }	/* [한국어] 절전이 없으니 깨울 일도 없다 */
static inline void	/* [한국어] 절전 정책을 셀 필요가 없다 */
irq_pm_install_action(struct irq_desc *desc, struct irqaction *action) { }	/* [한국어] 시그니처만 맞춘다 */
static inline void	/* [한국어] 그 되돌리기도 마찬가지 */
irq_pm_remove_action(struct irq_desc *desc, struct irqaction *action) { }	/* [한국어] 시그니처만 맞춘다 */
#endif

/* [한국어] generic irq chip 을 초기화하는 함수.
 *
 * generic chip 이란: 많은 SoC 의 인터럽트 컨트롤러가 "마스크 레지스터 하나,
 * 상태 레지스터 하나" 같은 비슷한 구조를 갖는다. 그 공통 패턴을 미리 구현해
 * 두고 레지스터 오프셋만 채우면 되게 만든 것이 generic-chip.c 다.
 *
 * 이 기능을 뺀 커널에서는 빈 인라인이 된다. 인자가 여섯 개나 되는데도 전부
 * 적어 두는 이유는 시그니처가 정확히 같아야 호출부가 컴파일되기 때문이다. */
#ifdef CONFIG_GENERIC_IRQ_CHIP	/* [한국어] generic chip 기반을 켠 빌드 */
void irq_init_generic_chip(struct irq_chip_generic *gc, const char *name,	/* [한국어] gc 구조체를 이름과 함께 초기화한다 */
			   int num_ct, unsigned int irq_base,	/* [한국어] chip type 개수와 이 chip 이 담당할 첫 인터럽트 번호 */
			   void __iomem *reg_base, irq_flow_handler_t handler);	/* [한국어] 레지스터 창의 가상 주소와 기본 흐름 제어 핸들러 */
#else
static inline void	/* [한국어] generic chip 을 뺀 빌드의 빈 구현 */
irq_init_generic_chip(struct irq_chip_generic *gc, const char *name,	/* [한국어] 시그니처를 그대로 맞춘다 */
		      int num_ct, unsigned int irq_base,	/* [한국어] 인자를 모두 적어야 호출부가 컴파일된다 */
		      void __iomem *reg_base, irq_flow_handler_t handler) { }	/* [한국어] 본문은 비어 있다 */
#endif /* CONFIG_GENERIC_IRQ_CHIP */

/* [한국어] 친화도 변경을 미뤄야 하는 하드웨어를 위한 계층.
 *
 * 문제: 어떤 인터럽트 컨트롤러는 인터럽트가 날아오는 중에 목적지를 바꾸면
 * 그 인터럽트가 유실되거나 두 CPU 에 중복 전달된다. x86 의 IO-APIC 가
 * 대표적이다.
 *
 * 해법: 새 친화도를 desc->pending_mask 에 적어 두고 IRQD_SETAFFINITY_PENDING
 * 을 세운다. 실제 변경은 다음 인터럽트를 처리하고 ack 를 보낸 직후, 곧
 * "지금은 확실히 날아오는 인터럽트가 없다"고 말할 수 있는 순간에 한다.
 *
 * GENERIC_PENDING_IRQ 를 뺀 아키텍처(대부분의 ARM)에서는 아래 #else 쪽 빈
 * 구현이 쓰인다. 그쪽 하드웨어는 언제든 안전하게 옮길 수 있어, can_move_pcntxt
 * 가 항상 참이고 나머지는 아무 일도 하지 않는다.
 *
 * 이 #ifdef 짝의 대칭을 읽는 것이 요령이다 — 같은 이름의 함수가 위아래에
 * 하나씩 있고, 아래쪽은 "미룰 일이 없다"를 뜻하는 상수 구현이다. */
#ifdef CONFIG_GENERIC_PENDING_IRQ	/* [한국어] 친화도 변경을 미룰 수 있어야 하는 아키텍처 */
/*
 * [한국어]
 * irq_can_move_pcntxt - 지금 이 자리에서 곧바로 친화도를 바꿔도 되는지 묻는다
 *
 * @data:   대상 irq_data.
 * @return: 즉시 변경해도 되면 참, 미뤄야 하면 거짓.
 *
 * 이름의 pcntxt 는 "process context" 의 줄임이다. 원래는 "프로세스 문맥에서
 * 옮길 수 있는가"라는 뜻이었고, 지금은 "인터럽트 처리와 무관한 임의의
 * 시점에 옮길 수 있는가"로 읽는 편이 맞다.
 *
 * 판정 근거: chip 이 IRQCHIP_MOVE_DEFERRED 를 선언했는지 하나뿐이다. 그
 * 플래그가 있으면 그 컨트롤러는 이동을 미뤄야 하는 하드웨어라는 뜻이다.
 *
 * 거짓이면 호출자는 irqd_set_move_pending() 으로 표시만 남기고 돌아간다.
 *
 * 실행 컨텍스트: 인라인. 친화도 설정 경로.
 *
 * 호출 체인:
 *   irq_do_set_affinity() (kernel/irq/manage.c) → [이 함수]
 */
static inline bool irq_can_move_pcntxt(struct irq_data *data)
{
	return !(data->chip->flags & IRQCHIP_MOVE_DEFERRED);	/* [한국어] 플래그가 있으면 미뤄야 한다 — 그래서 부정한다 */
}
/*
 * [한국어]
 * irq_move_pending - 미뤄 둔 친화도 변경이 있는지 묻는다
 *
 * @data:   대상 irq_data.
 * @return: 대기 중인 변경이 있으면 참.
 *
 * 공개 접근자 irqd_is_setaffinity_pending() 을 그대로 감싼 얇은 래퍼다.
 * 굳이 이름을 하나 더 두는 이유는 아래 #else 쪽과 대칭을 맞추기 위해서다 —
 * 그쪽에서는 이 함수가 상수 false 가 되어 호출부의 코드가 통째로 사라진다.
 *
 * 호출부는 인터럽트를 처리한 직후 이 함수로 물어보고, 참이면 미뤄 둔 이동을
 * 마무리한다.
 *
 * 실행 컨텍스트: 인라인. 인터럽트 처리 문맥.
 *
 * 호출 체인:
 *   irq_move_irq() (kernel/irq/migration.c) → [이 함수]
 */
static inline bool irq_move_pending(struct irq_data *data)
{
	return irqd_is_setaffinity_pending(data);	/* [한국어] 공개 접근자를 감싼다. 대칭을 위해 이름을 하나 더 두었다 */
}
/*
 * [한국어]
 * irq_copy_pending - 미뤄 둘 목표 CPU 마스크를 서술자에 적어 둔다
 *
 * @desc: 대상 서술자.
 * @mask: 옮기고 싶은 목표 CPU 집합.
 *
 * 위 can_move_pcntxt 가 거짓일 때, 지금 옮기는 대신 목표를 여기 적어 둔다.
 * 나중에 안전한 순간이 오면 아래 irq_get_pending 이 이 값을 꺼내 실제 이동에 쓴다.
 *
 * pending_mask 는 서술자가 만들어질 때 함께 할당된다 — 이 기능을 켠 커널에서만
 * 그 메모리가 존재하고, 아래 #else 쪽에서는 아예 필드가 없다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락 아래.
 *
 * 호출 체인:
 *   irq_do_set_affinity() (kernel/irq/manage.c) → [이 함수]
 */
static inline void
irq_copy_pending(struct irq_desc *desc, const struct cpumask *mask)
{
	cpumask_copy(desc->pending_mask, mask);	/* [한국어] 목표를 적어 둔다. 호출자가 준 mask 는 이 호출 뒤 사라질 수 있어 값을 복사한다 */
}
/*
 * [한국어]
 * irq_get_pending - 미뤄 둔 목표 CPU 마스크를 꺼낸다
 *
 * @mask: 결과를 담을 곳.
 * @desc: 대상 서술자.
 *
 * 위 copy_pending 의 짝. 인자 순서가 반대인 것에 주의한다 — 이쪽은 목적지가
 * 먼저 온다(memcpy 관례를 따랐다).
 *
 * 왜 값을 복사해 꺼내는가: 실제 이동은 chip->irq_set_affinity() 를 부르는
 * 것이고, 그 호출 중에 서술자 락을 놓아야 할 수도 있다. pending_mask 를
 * 직접 넘기면 그 사이 다른 CPU 가 새 목표를 덮어쓸 수 있어, 지역 사본을 쓴다.
 *
 * 실행 컨텍스트: 인라인. 인터럽트 처리 직후, 서술자 락 아래.
 *
 * 호출 체인:
 *   irq_move_masked_irq() (kernel/irq/migration.c) → [이 함수]
 */
static inline void
irq_get_pending(struct cpumask *mask, struct irq_desc *desc)
{
	cpumask_copy(mask, desc->pending_mask);	/* [한국어] 인자 순서가 copy_pending 과 반대다 — 목적지가 먼저 온다 */
}
/*
 * [한국어]
 * irq_desc_get_pending_mask - 미뤄 둔 목표 마스크의 주소를 그대로 돌려준다
 *
 * @desc:   대상 서술자.
 * @return: pending_mask 포인터.
 *
 * 위 두 함수가 값을 복사하는 것과 달리, 이쪽은 포인터를 그대로 준다.
 * 마스크를 읽기만 하는 진단 경로가 쓴다.
 *
 * 아래 #else 쪽에서 이 함수는 NULL 을 돌려준다. 그래서 호출부는 반드시
 * NULL 을 확인해야 하며, 그것이 이 함수와 위 두 함수의 결정적 차이다.
 *
 * 실행 컨텍스트: 인라인. 진단 경로.
 *
 * 호출 체인:
 *   irq_debug_show_masks() (kernel/irq/debugfs.c) → [이 함수]
 */
static inline struct cpumask *irq_desc_get_pending_mask(struct irq_desc *desc)
{
	return desc->pending_mask;	/* [한국어] 값이 아니라 포인터. 아래 빈 구현은 NULL 을 주므로 호출부가 확인해야 한다 */
}
bool irq_fixup_move_pending(struct irq_desc *desc, bool force_clear);	/* [한국어] 미뤄 둔 목표에서 오프라인 CPU 를 걸러 낸다. 남는 것이 없으면 이동을 포기한다 */
void irq_force_complete_move(struct irq_desc *desc);	/* [한국어] CPU 가 내려가기 전에 미뤄 둔 이동을 강제로 끝낸다 — 그러지 않으면 사라질 CPU 를 가리킨 채 남는다 */
#else /* CONFIG_GENERIC_PENDING_IRQ */
/*
 * [한국어]
 * irq_can_move_pcntxt - (미룸 기능 없음) 언제나 즉시 옮겨도 된다
 *
 * @data:   대상 irq_data. 여기서는 쓰이지 않는다.
 * @return: 항상 참.
 *
 * 이 아키텍처의 인터럽트 컨트롤러는 언제 옮겨도 인터럽트를 잃지 않는다.
 * 그래서 미룰 이유가 없고, 아래 함수들도 모두 빈 구현이 된다.
 *
 * 상수 참을 돌려주면 컴파일러가 호출부의 "미루는 쪽" 분기를 통째로 지운다 —
 * 이 #ifdef 짝의 목적이 그것이다.
 *
 * 실행 컨텍스트: 인라인.
 *
 * 호출 체인:
 *   irq_do_set_affinity() (kernel/irq/manage.c) → [이 빈 구현]
 */
static inline bool irq_can_move_pcntxt(struct irq_data *data)
{
	return true;	/* [한국어] 상수 참 — 호출부의 미루기 분기가 컴파일 후 사라진다 */
}
/*
 * [한국어]
 * irq_move_pending - (미룸 기능 없음) 대기 중인 이동은 없다
 *
 * @data:   대상 irq_data. 쓰이지 않는다.
 * @return: 항상 거짓.
 *
 * 미루는 일이 없으므로 대기 중인 것도 없다. 상수 거짓이라 호출부의
 * "미뤄 둔 이동을 마무리하는" 코드가 통째로 사라진다.
 *
 * 이것이 성능에 의미가 있는 이유: 그 호출부는 인터럽트 처리 핫패스인
 * irq_move_irq() 다. 아키텍처가 미룸을 쓰지 않으면 그 경로에 아무 비용도
 * 남지 않아야 한다.
 *
 * 실행 컨텍스트: 인라인. 인터럽트 처리 핫패스.
 *
 * 호출 체인:
 *   irq_move_irq() → [이 빈 구현]
 */
static inline bool irq_move_pending(struct irq_data *data)
{
	return false;	/* [한국어] 상수 거짓 — 인터럽트 핫패스에서 이동 처리가 통째로 사라진다 */
}
/*
 * [한국어]
 * irq_copy_pending - (미룸 기능 없음) 적어 둘 곳이 없다
 *
 * @desc: 대상 서술자. 쓰이지 않는다.
 * @mask: 목표 CPU 집합. 버려진다.
 *
 * 이 구성에서는 struct irq_desc 에 pending_mask 필드 자체가 없다. 그래서
 * 적어 둘 곳이 없고, 애초에 위 can_move_pcntxt 가 항상 참이라 이 함수가
 * 불릴 일도 없다.
 *
 * 그래도 정의해 두어야 호출부가 #ifdef 없이 컴파일된다.
 *
 * 실행 컨텍스트: 인라인.
 *
 * 호출 체인:
 *   irq_do_set_affinity() → [이 빈 구현] (실제로는 도달하지 않는다)
 */
static inline void
irq_copy_pending(struct irq_desc *desc, const struct cpumask *mask)
{
}
/*
 * [한국어]
 * irq_get_pending - (미룸 기능 없음) 꺼낼 것이 없다
 *
 * @mask: 결과를 담을 곳. 건드리지 않는다 — 호출자가 초기화한 값이 그대로 남는다.
 * @desc: 대상 서술자. 쓰이지 않는다.
 *
 * 위 copy_pending 과 짝을 이루는 빈 구현이다. mask 를 건드리지 않는다는
 * 점에 주의 — 호출자가 쓰레기 값을 그대로 쓰지 않도록 미리 초기화해야 하지만,
 * 실제로는 이 함수가 불리는 경로 자체가 컴파일 후 사라진다.
 *
 * 실행 컨텍스트: 인라인.
 *
 * 호출 체인:
 *   irq_move_masked_irq() → [이 빈 구현] (실제로는 도달하지 않는다)
 */
static inline void
irq_get_pending(struct cpumask *mask, struct irq_desc *desc)
{
}
/*
 * [한국어]
 * irq_desc_get_pending_mask - (미룸 기능 없음) 마스크가 존재하지 않는다
 *
 * @desc:   대상 서술자. 쓰이지 않는다.
 * @return: 항상 NULL.
 *
 * 위쪽 판이 포인터를 돌려주는 것과 달리 여기서는 NULL 이다. 그래서 이 함수를
 * 쓰는 호출부는 반드시 NULL 을 확인해야 한다 — 그러지 않으면 이 구성의
 * 커널에서만 널 역참조가 난다.
 *
 * 이 #ifdef 짝 중에서 호출부에 실제로 부담을 주는 유일한 함수다. 나머지는
 * 상수나 빈 본문이라 호출부가 신경 쓸 것이 없다.
 *
 * 실행 컨텍스트: 인라인. 진단 경로.
 *
 * 호출 체인:
 *   irq_debug_show_masks() (kernel/irq/debugfs.c) → [이 빈 구현]
 */
static inline struct cpumask *irq_desc_get_pending_mask(struct irq_desc *desc)
{
	return NULL;	/* [한국어] 필드 자체가 없다. 호출부가 NULL 검사를 해야 하는 유일한 함수다 */
}
/*
 * [한국어]
 * irq_fixup_move_pending - (미룸 기능 없음) 정리할 대기 이동이 없다
 *
 * @desc:   대상 서술자. 쓰이지 않는다.
 * @fclear: 강제로 지울지 여부. 쓰이지 않는다.
 * @return: 항상 거짓 — "정리한 것이 없다".
 *
 * 위쪽 판은 미뤄 둔 목표 CPU 집합에서 오프라인이 된 CPU 를 걸러 내고, 남는
 * 것이 없으면 이동을 포기한다. 이 구성에는 미뤄 둔 것 자체가 없다.
 *
 * 인자 이름이 위쪽 선언의 force_clear 와 다른 fclear 인데, 선언과 정의에서
 * 인자 이름이 달라도 되는 C 의 규칙을 따른 것이다. 줄 길이를 맞추려는 흔적이다.
 *
 * 실행 컨텍스트: 인라인. CPU 핫플러그 경로.
 *
 * 호출 체인:
 *   irq_needs_fixup() (kernel/irq/cpuhotplug.c) → [이 빈 구현]
 */
static inline bool irq_fixup_move_pending(struct irq_desc *desc, bool fclear)
{
	return false;	/* [한국어] 정리할 것이 없다. 인자 이름이 위 선언과 다른 것은 C 가 허용하는 범위다 */
}
/*
 * [한국어]
 * irq_force_complete_move - (미룸 기능 없음) 끝낼 이동이 없다
 *
 * @desc: 대상 서술자. 쓰이지 않는다.
 *
 * 위쪽 판은 CPU 가 내려가기 전에 미뤄 둔 이동을 강제로 마무리한다 — 그러지
 * 않으면 사라질 CPU 를 목표로 삼은 채 남아 이후의 인터럽트가 갈 곳을 잃는다.
 *
 * 이 구성에는 미뤄 둔 이동이 없으므로 할 일이 없다.
 *
 * 실행 컨텍스트: 인라인. CPU 오프라인 경로.
 *
 * 호출 체인:
 *   irq_migrate_all_off_this_cpu() (kernel/irq/cpuhotplug.c) → [이 빈 구현]
 */
static inline void irq_force_complete_move(struct irq_desc *desc) { }
#endif /* !CONFIG_GENERIC_PENDING_IRQ */

/* [한국어] 계층형 도메인이 없는 빌드를 위한 activate/deactivate 의 빈 구현.
 *
 * activate 란: 계층형 인터럽트 도메인에서, 인터럽트에 실제 하드웨어 자원을
 * 배정하는 단계다. 예를 들어 PCI MSI 인터럽트는 도메인 계층을 따라 내려가며
 * CPU 벡터를 하나 받아 오는데, 그것이 activate 다. 그 전까지는 인터럽트가
 * "예약만 된" 상태다.
 *
 * 왜 매핑과 activate 를 나누는가: 벡터는 유한한 자원이라, 인터럽트를 만들
 * 때마다 잡으면 금세 고갈된다. 실제로 쓰기 직전(request_irq 시점)에 잡으면
 * 훨씬 많은 인터럽트를 만들어 둘 수 있다.
 *
 * 도메인 계층이 없는 빌드에는 배정할 자원 계층이 없으므로, 상태 비트만
 * 세우고 0(성공)을 돌려주면 된다. */
#if !defined(CONFIG_IRQ_DOMAIN) || !defined(CONFIG_IRQ_DOMAIN_HIERARCHY)	/* [한국어] 도메인이 없거나 계층형이 아닌 빌드 */
/*
 * [한국어]
 * irq_domain_activate_irq - (계층형 도메인 없음) 상태만 표시하고 성공을 답한다
 *
 * @data:    대상 irq_data.
 * @reserve: 자원을 예약만 할지 실제로 배정할지. 여기서는 쓰이지 않는다.
 * @return:  항상 0(성공).
 *
 * 계층형 도메인이 있는 빌드에서는 irqdomain.c 의 실제 구현이 도메인 계층을
 * 따라 내려가며 각 단계의 activate 콜백을 부르고, 맨 아래에서 CPU 벡터 같은
 * 실제 자원을 배정한다.
 *
 * 이 빌드에는 그 계층이 없다. 그래도 activated 상태 비트는 세워야 하는데,
 * 다른 코드가 그 비트로 "이 인터럽트를 쓸 준비가 되었는가"를 판정하기 때문이다.
 *
 * 0 을 돌려주는 것이 중요하다 — 실패로 답하면 request_irq() 가 거절된다.
 *
 * 실행 컨텍스트: 인라인. 요청 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_activate() (kernel/irq/chip.c) → [이 빈 구현]
 */
static inline int irq_domain_activate_irq(struct irq_data *data, bool reserve)
{
	irqd_set_activated(data);	/* [한국어] 배정할 자원은 없지만 상태 비트는 세워야 한다 — 다른 코드가 이 비트로 준비 여부를 본다 */
	return 0;	/* [한국어] 성공. 실패로 답하면 request_irq 가 거절된다 */
}
/*
 * [한국어]
 * irq_domain_deactivate_irq - (계층형 도메인 없음) 상태 표시만 지운다
 *
 * @data: 대상 irq_data.
 *
 * 위 activate 의 짝. 반납할 자원이 없으므로 비트만 지운다.
 *
 * 반환값이 없는 이유: 자원을 되돌리는 경로는 실패해도 물러설 곳이 없다.
 * 계층형 구현도 마찬가지로 void 다.
 *
 * 실행 컨텍스트: 인라인. 해제 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_shutdown_and_deactivate() (kernel/irq/chip.c) → [이 빈 구현]
 */
static inline void irq_domain_deactivate_irq(struct irq_data *data)
{
	irqd_clr_activated(data);	/* [한국어] 반납할 자원은 없고 표시만 되돌린다 */
}
#endif

/*
 * [한국어]
 * irqd_get_parent_data - 계층형 도메인에서 한 단계 위의 irq_data 를 얻는다
 *
 * @irqd:   대상 irq_data.
 * @return: 부모 단계의 irq_data. 계층이 없거나 최상위면 NULL.
 *
 * 계층형 도메인의 구조: 하나의 인터럽트가 여러 단계로 표현된다. 예를 들어
 * PCI MSI 는 이렇게 겹친다.
 *
 *   MSI 도메인      — 장치가 보는 MSI 메시지
 *     ↑ parent_data
 *   상위 컨트롤러    — 예: ARM GIC-ITS, x86 remapping
 *     ↑ parent_data
 *   CPU 벡터 도메인  — 최종적으로 CPU 를 깨우는 자원
 *
 * 각 단계가 자기 irq_data 를 갖고 parent_data 로 이어져 있다. 마스크나
 * 친화도 설정 같은 조작은 그 사슬을 따라 올라가며 각 단계가 자기 몫을 처리한다.
 *
 * 이 함수가 #ifdef 를 함수 안에 넣은 것에 주의한다. 위쪽의 다른 관용구들은
 * 함수 자체를 두 벌 정의하는데, 여기서는 본문만 갈린다. 반환형과 인자가 같고
 * 본문이 한 줄씩이라, 함수를 두 번 쓰는 것보다 짧기 때문이다.
 *
 * 실행 컨텍스트: 인라인. 어디서든.
 *
 * 호출 체인:
 *   irq_chip_set_affinity_parent()/irq_chip_mask_parent 계열 (kernel/irq/chip.c) → [이 함수]
 */
static inline struct irq_data *irqd_get_parent_data(struct irq_data *irqd)
{
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY	/* [한국어] 계층형 도메인을 켠 빌드 */
	return irqd->parent_data;	/* [한국어] 한 단계 위. 최상위면 이 필드가 NULL 이다 */
#else
	return NULL;	/* [한국어] 계층이 없으면 부모도 없다 */
#endif
}

/* [한국어] debugfs 로 인터럽트 내부 상태를 들여다보는 계층.
 *
 * 무엇을 보여 주는가: /sys/kernel/debug/irq/ 아래에 인터럽트마다 파일이 생기고,
 * 그 안에 chip 이름, 도메인 계층, 하드웨어 인터럽트 번호, 상태 비트들,
 * 친화도 마스크가 사람이 읽는 형태로 나온다.
 *
 * 왜 /proc/interrupts 로 부족한가: 그쪽은 횟수만 보여 준다. 인터럽트가
 * 오지 않는 문제를 진단하려면 마스크되었는지, activate 되었는지, 어느
 * 도메인 계층에 어떻게 매핑되었는지를 봐야 하는데 그 정보가 없다.
 *
 * 이 기능을 뺀 빌드에서는 아래 #else 쪽 빈 구현이 되어 호출이 사라진다. */
#ifdef CONFIG_GENERIC_IRQ_DEBUGFS	/* [한국어] irq debugfs 를 켠 빌드 */
#include <linux/debugfs.h>	/* [한국어] debugfs_remove() 등. 아래 remove_debugfs_entry 가 인라인이라 여기서 필요하다 */

/* [한국어] 상태 비트 하나를 사람이 읽는 이름과 짝지은 항목.
 *
 * 상태 워드를 그대로 16진수로 찍으면 사람이 해석할 수 없다. 이 표를 두고
 * 비트가 서 있는 것마다 이름을 출력하면 곧바로 읽힌다. */
struct irq_bit_descr {
	unsigned int	mask;
	/* [한국어] 이 항목이 나타내는 비트 마스크.
	 * 설정자: 아래 BIT_MASK_DESCR 매크로가 정적으로 채운다.
	 * 읽는 자: irq_debug_show_bits() 가 상태 워드와 AND 해 본다.
	 * 값 범위: IRQD_* 또는 IRQS_* 상수 하나. 여러 비트를 담을 수도 있지만
	 *   그러면 이름 하나가 여러 비트를 뜻하게 되어 출력이 애매해진다. */

	char		*name;
	/* [한국어] 그 비트를 나타낼 문자열.
	 * 설정자: BIT_MASK_DESCR 매크로가 상수 이름을 문자열화(#m)해서 넣는다.
	 * 읽는 자: 비트가 서 있을 때 그대로 출력된다.
	 * 왜 const 가 아닌가: 상수 문자열을 가리키므로 const char * 여야 맞지만,
	 *   오래된 코드라 그대로 남아 있다. 실제로 고치는 곳은 없다. */
};

/* [한국어] 상태 비트 표의 항목 하나를 만드는 매크로.
 *
 * BIT_MASK_DESCR(IRQD_IRQ_MASKED) 라고 쓰면
 * { .mask = IRQD_IRQ_MASKED, .name = "IRQD_IRQ_MASKED" } 가 된다.
 *
 * #m 이 전처리기의 문자열화 연산자다. 상수 이름을 그대로 문자열로 만들어
 * 주므로, 이름과 문자열이 어긋날 수 없다 — 손으로 둘을 적으면 오타나
 * 이름 변경 때 어긋나지만 이 방식은 언제나 일치한다. */
#define BIT_MASK_DESCR(m)	{ .mask = m, .name = #m }

void irq_debug_show_bits(struct seq_file *m, int ind, unsigned int state,	/* [한국어] 상태 워드에서 서 있는 비트들의 이름을 출력한다. ind 는 들여쓰기 칸 수 */
			 const struct irq_bit_descr *sd, int size);	/* [한국어] 위 표와 그 항목 수 */

void irq_add_debugfs_entry(unsigned int irq, struct irq_desc *desc);	/* [한국어] 이 인터럽트의 debugfs 파일을 만든다. 서술자 생성 시 호출 */
/*
 * [한국어]
 * irq_remove_debugfs_entry - 이 인터럽트의 debugfs 파일과 이름 사본을 지운다
 *
 * @desc: 대상 서술자.
 *
 * 위 add 의 짝인데, add 가 .c 파일의 실제 함수인 것과 달리 이쪽은 인라인이다.
 * 하는 일이 두 줄뿐이라 함수를 만들 이유가 없기 때문이다.
 *
 * 두 가지를 함께 정리한다:
 *   debugfs_file — debugfs 항목 자체.
 *   dev_name     — 이 인터럽트를 요청한 장치의 이름 사본. debugfs 출력에
 *     쓰려고 복사해 둔 것이라, 항목과 수명을 함께한다.
 *
 * debugfs_remove() 는 NULL 을 받아도 안전하고 kfree() 도 그렇다. 그래서
 * 항목을 만든 적이 없는 서술자에 이 함수를 불러도 문제가 없다 — 해제
 * 경로에서 조건 검사를 줄이는 흔한 방식이다.
 *
 * 실행 컨텍스트: 인라인. 서술자 해제 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   free_desc() (kernel/irq/irqdesc.c) → [이 함수]
 */
static inline void irq_remove_debugfs_entry(struct irq_desc *desc)
{
	debugfs_remove(desc->debugfs_file);	/* [한국어] NULL 을 받아도 안전하다 — 만든 적 없는 서술자에도 그냥 부를 수 있다 */
	kfree(desc->dev_name);	/* [한국어] debugfs 출력용으로 복사해 둔 장치 이름. 역시 NULL 이면 아무 일도 하지 않는다 */
}
void irq_debugfs_copy_devname(int irq, struct device *dev);	/* [한국어] 요청한 장치의 이름을 복사해 둔다. 장치가 먼저 사라져도 debugfs 출력이 유효하도록 */
/* [한국어] 도메인 자체의 debugfs 항목. irq 별 항목과 별개로 도메인 계층을 보여 준다. */
# ifdef CONFIG_IRQ_DOMAIN	/* [한국어] 도메인을 쓰는 빌드에만 */
void irq_domain_debugfs_init(struct dentry *root);	/* [한국어] /sys/kernel/debug/irq/domains/ 를 만든다 */
# else	/* [한국어] 도메인을 쓰지 않는 빌드 */
/*
 * [한국어]
 * irq_domain_debugfs_init - (도메인 없음) 보여 줄 도메인 계층이 없다
 *
 * @root: irq debugfs 의 최상위 디렉터리. 여기서는 쓰이지 않는다.
 *
 * 도메인을 쓰는 빌드에서는 이 호출이 그 아래에 domains/ 디렉터리를 만들고,
 * 등록된 인터럽트 도메인마다 파일을 하나씩 둔다. 그 파일에는 도메인의 이름,
 * 부모 도메인, 매핑된 인터럽트 수가 나온다.
 *
 * 도메인이 없는 빌드는 인터럽트 번호가 곧 하드웨어 번호라 계층이라는 개념
 * 자체가 없다. 그래서 보여 줄 것이 없다.
 *
 * irq debugfs 자체는 켜져 있는 구성이라는 점에 주의한다 — 이 함수는 바깥
 * #ifdef CONFIG_GENERIC_IRQ_DEBUGFS 안쪽에 있고, 안쪽 #ifdef 만 도메인
 * 유무를 가른다. 즉 인터럽트별 debugfs 항목은 여전히 만들어진다.
 *
 * 실행 컨텍스트: 인라인. 부팅 중 debugfs 초기화, 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_debugfs_init() (kernel/irq/debugfs.c) → [이 빈 구현]
 */
static inline void irq_domain_debugfs_init(struct dentry *root)	/* [한국어] 도메인이 없으면 보여 줄 계층도 없다 */
{
}
# endif	/* [한국어] 도메인 유무 분기의 끝 */
#else /* CONFIG_GENERIC_IRQ_DEBUGFS */
/*
 * [한국어]
 * irq_add_debugfs_entry - (irq debugfs 없음) 만들 항목이 없다
 *
 * @irq: 인터럽트 번호. 쓰이지 않는다.
 * @d:   대상 서술자. 쓰이지 않는다.
 *
 * 이 기능을 뺀 빌드에서는 서술자에 debugfs_file 필드 자체가 없다. 그래서
 * 만들 항목도 저장할 곳도 없다.
 *
 * 인자 이름이 위쪽 선언의 desc 가 아니라 d 인데, 줄 길이를 맞추려는 흔적이며
 * C 가 허용하는 범위다.
 *
 * 실행 컨텍스트: 인라인. 서술자 생성 경로.
 *
 * 호출 체인:
 *   alloc_desc() (kernel/irq/irqdesc.c) → [이 빈 구현]
 */
static inline void irq_add_debugfs_entry(unsigned int irq, struct irq_desc *d)
{
}
/*
 * [한국어]
 * irq_remove_debugfs_entry - (irq debugfs 없음) 지울 항목이 없다
 *
 * @d: 대상 서술자. 쓰이지 않는다.
 *
 * 위 add 의 짝. 만든 적이 없으므로 지울 것도 없다.
 *
 * 위쪽 판이 debugfs_remove 와 kfree 를 부르는 것과 달리 완전히 비어 있다 —
 * 그 필드들이 이 빌드에는 존재하지 않기 때문이다.
 *
 * 실행 컨텍스트: 인라인. 서술자 해제 경로.
 *
 * 호출 체인:
 *   free_desc() (kernel/irq/irqdesc.c) → [이 빈 구현]
 */
static inline void irq_remove_debugfs_entry(struct irq_desc *d)
{
}
/*
 * [한국어]
 * irq_debugfs_copy_devname - (irq debugfs 없음) 복사해 둘 이름이 없다
 *
 * @irq: 인터럽트 번호. 쓰이지 않는다.
 * @dev: 요청한 장치. 쓰이지 않는다.
 *
 * 위쪽 판은 장치 이름을 kstrdup 으로 복사해 서술자에 붙여 둔다. 장치가
 * 먼저 사라져도 debugfs 출력이 유효하도록 사본을 갖는 것이다.
 *
 * 이 빌드에는 보여 줄 곳이 없으므로 복사도 하지 않는다 — 그만큼 요청
 * 경로에서 할당 하나가 사라진다.
 *
 * 실행 컨텍스트: 인라인. 인터럽트 요청 경로.
 *
 * 호출 체인:
 *   __setup_irq() (kernel/irq/manage.c) → [이 빈 구현]
 */
static inline void irq_debugfs_copy_devname(int irq, struct device *dev)
{
}
#endif /* CONFIG_GENERIC_IRQ_DEBUGFS */
