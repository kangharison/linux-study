/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Internal header to deal with irq_desc->status which will be renamed
 * to irq_desc->settings.
 */
/*
 * [한국어 설명] irq_desc 설정 비트의 내부 접근자 계층 (settings.h)
 *
 * === 파일의 역할 ===
 * 인터럽트 서술자(struct irq_desc)의 status_use_accessors 필드에 담긴 설정
 * 비트들을, 이름 있는 인라인 함수를 통해서만 읽고 쓰게 만드는 헤더다. 이
 * 필드는 "이 인터럽트가 CPU 마다 따로인가", "요청을 허용하는가", "스레드로
 * 돌릴 수 있는가", "엣지인가 레벨인가" 같은 성질을 한 워드에 모아 둔 것으로,
 * irq 코어 전체가 끊임없이 들여다보는 값이다.
 *
 * 이 파일의 진짜 목적은 접근자를 제공하는 것이 아니라 직접 접근을 막는 것이다.
 * 파일 중간의 #define IRQ_PER_CPU GOT_YOU_MORON 무리가 그 장치다 — 공개
 * 헤더(include/linux/irq.h)가 정의한 IRQ_* 플래그 이름들을 컴파일되지 않는
 * 토큰으로 덮어써, kernel/irq 안의 코드가 그 이름을 쓰면 곧바로 빌드가 깨진다.
 * 값 자체는 그 재정의 앞에서 _IRQ_* 라는 사본 enum 에 먼저 담아 두었으므로,
 * 이 파일의 접근자들만이 유일하게 그 값에 닿을 수 있다.
 *
 * 파일 상단의 영어 주석이 말하는 "status 를 settings 로 이름을 바꿀 것이다"는
 * 오래된 계획이고, 필드 이름 status_use_accessors 자체가 그 의도를 담고 있다 —
 * "이것은 status 이지만 접근자를 써라"는 뜻이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 리눅스 인터럽트 계층은 위에서부터 이렇게 쌓인다:
 *
 *   드라이버 (request_irq / free_irq / disable_irq)
 *     ↓
 *   kernel/irq/manage.c  — 요청·해제·친화도·스레드 관리
 *   kernel/irq/chip.c    — 흐름 제어 핸들러(handle_level_irq 등)
 *   kernel/irq/handle.c  — 핸들러 호출과 반환값 처리
 *     ↓
 *   struct irq_desc      — 인터럽트 번호 하나의 모든 상태 (irqdesc.c)
 *     ├─ irq_data        — 하드웨어 쪽 정보(hwirq, chip, 도메인, 친화도)
 *     ├─ action 목록     — 등록된 핸들러들
 *     └─ status_use_accessors ← **이 파일이 다루는 곳**
 *     ↓
 *   struct irq_chip      — 실제 인터럽트 컨트롤러 드라이버 (mask/unmask/ack)
 *
 * 이 파일은 그 서술자의 설정 비트에만 관여하는 가장 아래층의 얇은 계층이다.
 * 실행 컨텍스트를 가리지 않는다 — 모두 인라인 함수라 호출자의 문맥에서 돌며,
 * 잠들지도 락을 잡지도 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 곳: include/linux/irq.h 가 정의한 IRQ_* 상수와 struct irq_desc.
 * 이 헤더는 스스로 include 를 하지 않고, 이것을 포함하는 internals.h 가
 * 이미 필요한 것을 들여온 상태를 전제한다 — 그래서 단독으로는 컴파일되지 않는다.
 *
 * 이 파일에 의존하는 곳: kernel/irq 의 거의 모든 .c 파일.
 *   manage.c    — can_request/can_thread/is_per_cpu 로 요청 가능 여부를 판정
 *   chip.c      — is_level 로 흐름 제어 방식을 고르고, trigger_mask 를 읽음
 *   autoprobe.c — can_probe 로 자동 탐지 대상을 고름
 *   irqdesc.c   — 서술자를 만들 때 기본 플래그를 넣음
 *   spurious.c  — is_polled 로 폴링 대상인지 확인
 *   proc.c      — has_no_balance_set 으로 친화도 변경 가능 여부를 판정
 *
 * 데이터 흐름: irq_chip 드라이버나 아키텍처 코드가 irq_set_status_flags() 같은
 * 공개 API 로 비트를 세우면, 그 값이 status_use_accessors 에 남고, 이후 코어의
 * 판정은 모두 이 파일의 접근자를 거쳐 그 비트를 읽는다.
 *
 * === 주요 함수/구조체 요약 ===
 * enum { _IRQ_* }             — 공개 IRQ_* 플래그 값을 가려지기 전에 복사해 둔 사본.
 * #define IRQ_* GOT_YOU_MORON — 공개 이름을 컴파일 불가 토큰으로 덮어 직접 접근을 차단.
 * irq_settings_clr_and_set()  — IRQF_MODIFY_MASK 로 걸러 안전한 비트만 한 번에 바꾼다.
 * irq_settings_can_request()  — request_irq 가 이 인터럽트를 받아도 되는지.
 * irq_settings_is_level()     — 레벨 트리거인지. 흐름 제어 핸들러 선택의 근거.
 * irq_settings_get/set_trigger_mask() — 엣지/레벨과 극성을 담은 하위 비트 묶음.
 *
 * 접근자 이름의 규칙: is_/can_/has_ 로 시작하면 판정(bool 반환), set_ 은 세우기,
 * clr_ 은 지우기, get_ 은 값 꺼내기다. 이름만 보고 방향을 알 수 있게 되어 있다.
 */
/* [한국어] 공개 IRQ_* 플래그 값을 "가려지기 전에" 복사해 두는 사본 enum.
 *
 * 이 enum 이 반드시 아래 #define 무리보다 먼저 와야 한다. 아래에서 IRQ_PER_CPU
 * 같은 이름을 컴파일 불가 토큰으로 덮어쓰기 때문에, 그 뒤에서는 원래 값을 읽을
 * 방법이 없다. 여기서 미리 _IRQ_* 라는 밑줄 붙은 이름으로 복사해 두면, 이
 * 파일의 접근자들만이 그 값에 닿을 수 있게 된다.
 *
 * 즉 이 enum 과 아래 #define 무리는 한 쌍으로 "값은 남기고 이름은 봉인한다"는
 * 하나의 장치를 이룬다. */
enum {
	_IRQ_DEFAULT_INIT_FLAGS	= IRQ_DEFAULT_INIT_FLAGS,
	/* [한국어] 새로 만든 서술자에 기본으로 넣는 플래그 묶음.
	 * 설정자: 이 사본 정의뿐. 값은 아키텍처가 include/linux/irq.h 에서 정한다.
	 * 읽는 자: irqdesc.c 의 desc_set_defaults() 가 서술자를 초기화할 때.
	 * 값 범위: 대개 IRQ_NOREQUEST | IRQ_NOPROBE | IRQ_NOAUTOEN 의 조합이다.
	 *   기본이 "요청 불가, 탐지 불가" 인 것이 요점이다 — 인터럽트 컨트롤러
	 *   드라이버가 명시적으로 열어 주기 전에는 아무도 그 번호를 쓸 수 없다.
	 * 아키텍처가 이 값을 재정의할 수 있어, 플랫폼마다 기본 정책이 다르다. */

	_IRQ_PER_CPU		= IRQ_PER_CPU,
	/* [한국어] 이 인터럽트가 CPU 마다 따로 존재하는가.
	 * 설정자: irq_set_percpu_devid() 나 chip 드라이버의 초기화.
	 * 읽는 자: manage.c 가 친화도 설정을 거부할지 정할 때, chip.c 가
	 *   handle_percpu_irq 계열 흐름 제어를 고를 때.
	 * 값 범위: 서 있으면 로컬 타이머나 IPI 처럼 CPU 마다 독립된 인터럽트다.
	 * 무엇이 달라지는가: 친화도라는 개념이 성립하지 않고, 마스킹도 그 CPU
	 *   에서만 유효하며, 통계도 CPU 별로 따로 센다.
	 * 동기화: 서술자 락 아래에서 바뀌지만, 대개 초기화 때 한 번 정해진다. */

	_IRQ_LEVEL		= IRQ_LEVEL,
	/* [한국어] 레벨 트리거 인터럽트인가.
	 * 설정자: __irq_set_trigger() 가 하드웨어에 트리거 방식을 설정한 뒤 반영한다.
	 * 읽는 자: chip.c 의 흐름 제어 선택, proc.c 의 진단 출력.
	 * 값 범위: 서 있으면 레벨, 없으면 엣지다.
	 * 왜 중요한가: 두 방식은 핸들러가 끝난 뒤의 처리가 완전히 다르다. 레벨은
	 *   원인이 사라질 때까지 계속 어서션 상태라 핸들러 전에 마스크하고 후에
	 *   언마스크해야 하고, 엣지는 그럴 필요가 없는 대신 처리 중에 새로 온
	 *   엣지를 놓치지 않도록 재실행 표시를 다뤄야 한다. */

	_IRQ_NOPROBE		= IRQ_NOPROBE,
	/* [한국어] 자동 탐지(autoprobe)에서 제외하는가.
	 * 설정자: irq_set_probe()/irq_set_noprobe(), 그리고 위 기본 플래그.
	 * 읽는 자: autoprobe.c 의 probe_irq_on() 이 후보를 고를 때.
	 * 값 범위: 서 있으면 탐지 대상이 아니다. 기본값이 서 있는 상태다.
	 * 왜 기본이 제외인가: 자동 탐지는 인터럽트 선을 일부러 열어 두고 어떤
	 *   번호가 오는지 보는 방식이라, 아무 번호나 대상으로 삼으면 시스템에
	 *   위험하다. 오래된 ISA 장치를 위해서만 명시적으로 열어 준다. */

	_IRQ_NOREQUEST		= IRQ_NOREQUEST,
	/* [한국어] request_irq() 로 요청할 수 없는 인터럽트인가.
	 * 설정자: irq_set_norequest()/irq_modify_status(), 그리고 위 기본 플래그.
	 * 읽는 자: manage.c 의 __setup_irq() 가 요청을 받아들일지 판정할 때.
	 * 값 범위: 서 있으면 요청이 -EINVAL 로 거절된다. 기본값이 서 있는 상태다.
	 * 왜 기본이 거절인가: 인터럽트 번호 공간에는 하드웨어에 연결되지 않은
	 *   빈 번호가 많다. 컨트롤러 드라이버가 "이 번호는 실재한다"고 열어 주기
	 *   전에는 드라이버가 붙지 못하게 막는 것이 안전하다. */

	_IRQ_NOTHREAD		= IRQ_NOTHREAD,
	/* [한국어] 강제 스레드화(threadirqs)에서 제외하는가.
	 * 설정자: irq_set_nothread(), 그리고 chip 드라이버의 초기화.
	 * 읽는 자: manage.c 의 irq_setup_forced_threading() 이 이 핸들러를
	 *   스레드로 돌릴지 정할 때.
	 * 값 범위: 서 있으면 threadirqs 부트 옵션이나 PREEMPT_RT 에서도 하드
	 *   인터럽트 문맥에 그대로 남는다.
	 * 왜 필요한가: 타이머 틱이나 IPI 처럼 스레드로 미루면 시스템이 성립하지
	 *   않는 인터럽트가 있다. 그것들을 강제 스레드화의 예외로 표시한다. */

	_IRQ_NOAUTOEN		= IRQ_NOAUTOEN,
	/* [한국어] request_irq() 성공 직후 자동으로 켜지 말라는 표시.
	 * 설정자: irq_set_status_flags(IRQ_NOAUTOEN) 를 드라이버가 부른다.
	 * 읽는 자: manage.c 의 __setup_irq() 가 요청 끝에 enable 할지 정할 때.
	 * 값 범위: 서 있으면 드라이버가 나중에 직접 enable_irq() 를 불러야 한다.
	 * 언제 쓰는가: 핸들러를 등록해 두되 하드웨어 준비가 끝난 뒤에 열고 싶을 때다.
	 *   등록과 동시에 열리면, 아직 초기화되지 않은 장치의 인터럽트가 들어와
	 *   핸들러가 준비되지 않은 상태를 보게 된다. */

	_IRQ_NO_BALANCING	= IRQ_NO_BALANCING,
	/* [한국어] 친화도 자동 조정(irqbalance)의 대상에서 제외하는가.
	 * 설정자: irq_set_status_flags(IRQ_NO_BALANCING) 또는 chip 초기화.
	 * 읽는 자: proc.c 가 /proc/irq/N/affinity 쓰기를 허용할지, manage.c 의
	 *   irq_can_set_affinity 계열이 판정할 때.
	 * 값 범위: 서 있으면 사용자나 커널이 친화도를 바꿀 수 없다.
	 * 왜 필요한가: 특정 CPU 에 묶여야만 뜻이 통하는 인터럽트가 있다.
	 *   그것을 옮기면 동작이 깨지므로 아예 변경을 막는다. */

	_IRQ_NESTED_THREAD	= IRQ_NESTED_THREAD,
	/* [한국어] 이 인터럽트가 부모 스레드 핸들러 안에서 중첩 실행되는가.
	 * 설정자: irq_set_nested_thread() — 주로 GPIO 확장기나 I2C 인터럽트
	 *   컨트롤러의 자식 인터럽트에 붙인다.
	 * 읽는 자: manage.c 가 이 인터럽트에 자체 스레드를 만들지 정할 때,
	 *   chip.c 의 handle_nested_irq() 가 호출될 때.
	 * 값 범위: 서 있으면 자체 하드 인터럽트 핸들러도 자체 스레드도 없다.
	 * 무슨 구조인가: I2C 로 연결된 인터럽트 컨트롤러는 상태를 읽으려면
	 *   잠들어야 해서 하드 인터럽트 문맥에서 다룰 수 없다. 부모의 스레드
	 *   핸들러가 그 칩을 읽고, 발견한 자식 인터럽트를 이 방식으로 부른다. */

	_IRQ_PER_CPU_DEVID	= IRQ_PER_CPU_DEVID,
	/* [한국어] CPU 마다 다른 dev_id 를 갖는 per-CPU 인터럽트인가.
	 * 설정자: irq_set_percpu_devid().
	 * 읽는 자: manage.c 의 request_percpu_irq()/free_percpu_irq() 경로.
	 * 값 범위: 서 있으면 action->percpu_dev_id 가 __percpu 포인터로 해석된다.
	 * 위 _IRQ_PER_CPU 와의 차이: PER_CPU 는 "인터럽트가 CPU 마다 있다"이고,
	 *   이쪽은 그에 더해 "핸들러에 넘길 장치 문맥도 CPU 마다 다르다"이다.
	 *   로컬 타이머처럼 CPU 별 상태를 갖는 장치가 이 방식을 쓴다. */

	_IRQ_IS_POLLED		= IRQ_IS_POLLED,
	/* [한국어] 이 인터럽트가 폴링으로도 확인되는가.
	 * 설정자: irq_set_status_flags(IRQ_IS_POLLED).
	 * 읽는 자: spurious.c 의 오탐 감지 로직.
	 * 값 범위: 서 있으면 "처리하지 못한 인터럽트" 통계에서 제외된다.
	 * 왜 필요한가: 폴링을 병행하는 드라이버는 인터럽트가 왔을 때 이미 폴링이
	 *   일을 끝내 놓아 IRQ_NONE 을 돌려주는 일이 잦다. 그것을 오탐으로 세면
	 *   커널이 멀쩡한 인터럽트를 "nobody cared" 로 꺼 버린다. */

	_IRQ_DISABLE_UNLAZY	= IRQ_DISABLE_UNLAZY,
	/* [한국어] disable_irq() 를 게으르게(lazy) 처리하지 말라는 표시.
	 * 설정자: irq_set_status_flags(IRQ_DISABLE_UNLAZY).
	 * 읽는 자: chip.c 의 irq_disable() 이 하드웨어를 실제로 마스크할지 정할 때.
	 * 값 범위: 서 있으면 disable 시점에 곧바로 chip->irq_mask() 를 부른다.
	 * 기본이 게으른 이유: 마스킹은 컨트롤러 접근이라 비싸다. 그래서 보통은
	 *   소프트웨어 표시만 해 두고, 실제로 인터럽트가 왔을 때 그때 마스크한다.
	 * 게으르면 안 되는 경우: 인터럽트 선이 공유되거나 컨트롤러가 마스크되지
	 *   않은 인터럽트를 계속 재전송해 시스템을 멈추게 하는 하드웨어다. */

	_IRQ_HIDDEN		= IRQ_HIDDEN,
	/* [한국어] /proc/interrupts 등 사용자에게 보이는 목록에서 감추는가.
	 * 설정자: irq_modify_status() 로 컨트롤러 드라이버가 붙인다.
	 * 읽는 자: proc.c 가 항목을 출력할지 정할 때.
	 * 값 범위: 서 있으면 통계와 sysfs 항목이 만들어지지 않는다.
	 * 왜 감추는가: 계층형 인터럽트 도메인에서 중간 단계의 가상 인터럽트는
	 *   사용자에게 의미가 없다. 실제로 카운트되는 것은 맨 아래 하나뿐이라,
	 *   중간 것들을 보여 주면 목록이 무의미하게 길어진다. */

	_IRQ_NO_DEBUG		= IRQ_NO_DEBUG,
	/* [한국어] 오탐(spurious) 감지와 디버그 계측에서 제외하는가.
	 * 설정자: irq_modify_status(), 아래 irq_settings_set_no_debug().
	 * 읽는 자: spurious.c 의 note_interrupt() 가 통계를 셀지 정할 때.
	 * 값 범위: 서 있으면 IRQ_NONE 반환이 쌓여도 "nobody cared" 로 꺼지지 않는다.
	 * 위 _IRQ_IS_POLLED 와 비슷하지만 더 강하다 — 폴링 예외가 아니라 감지
	 *   자체를 끄는 것이라, 정말로 오탐을 셀 수 없는 구조에만 쓴다. */

	_IRQF_MODIFY_MASK	= IRQF_MODIFY_MASK,
	/* [한국어] 바깥에서 고쳐도 안전한 비트들만 모은 마스크.
	 * 설정자: 이 사본 정의뿐.
	 * 읽는 자: 아래 irq_settings_clr_and_set() 이 유일하다.
	 * 왜 필요한가: status_use_accessors 에는 설정 비트뿐 아니라 IRQD_* 계열의
	 *   실행 중 상태(진행 중, 마스크됨, 대기 중)도 함께 들어 있다. 그 상태
	 *   비트를 바깥에서 건드리면 인터럽트 처리 상태 기계가 깨진다.
	 * 이 마스크가 그 경계다 — 통과한 비트만이 "설정"이고, 나머지는 코어의 것이다. */
};

/* [한국어] 여기서부터가 이 파일의 핵심 장치다.
 *
 * 위 enum 이 값을 _IRQ_* 사본으로 안전하게 옮겨 놓았으므로, 이제 공개 이름들을
 * 컴파일되지 않는 토큰(GOT_YOU_MORON)으로 덮어쓴다. 그러면 kernel/irq 안의
 * 어떤 코드가 desc->status_use_accessors & IRQ_PER_CPU 처럼 직접 비트를 만지려
 * 해도, 전처리기가 그 자리에 정의되지 않은 식별자를 넣어 빌드가 곧바로 깨진다.
 *
 * 왜 이렇게까지 하는가: 이 비트 워드에는 설정 비트와 실행 중 상태 비트가 섞여
 * 있고, 어떤 비트는 특정 락 아래에서만 바꿀 수 있으며, 어떤 조합은 서로
 * 모순된다. 직접 접근을 허용하면 그 규칙이 코드 곳곳에 흩어져 지켜지지 않는다.
 * 접근자만 남겨 두면 규칙이 이 파일 한 곳에 모인다.
 *
 * 이름을 GOT_YOU_MORON 으로 고른 것은 커널 소스의 오래된 농담이며, 컴파일
 * 오류 메시지에 이 토큰이 나타나면 "접근자를 쓰라"는 뜻으로 읽으면 된다.
 *
 * 이 봉인은 이 헤더를 포함한 번역 단위 안에서만 유효하다 — kernel/irq 밖의
 * 코드는 공개 헤더의 IRQ_* 를 정상적으로 계속 쓴다. */
#define IRQ_PER_CPU		GOT_YOU_MORON	/* [한국어] per-CPU 여부는 irq_settings_is_per_cpu() 로만 */
#define IRQ_NO_BALANCING	GOT_YOU_MORON	/* [한국어] 친화도 고정 여부는 irq_settings_has_no_balance_set() 로만 */
#define IRQ_LEVEL		GOT_YOU_MORON	/* [한국어] 레벨 여부는 irq_settings_is_level() 로만 */
#define IRQ_NOPROBE		GOT_YOU_MORON	/* [한국어] 탐지 가능 여부는 irq_settings_can_probe() 로만 */
#define IRQ_NOREQUEST		GOT_YOU_MORON	/* [한국어] 요청 가능 여부는 irq_settings_can_request() 로만 */
#define IRQ_NOTHREAD		GOT_YOU_MORON	/* [한국어] 스레드화 가능 여부는 irq_settings_can_thread() 로만 */
#define IRQ_NOAUTOEN		GOT_YOU_MORON	/* [한국어] 자동 활성화 여부는 irq_settings_can_autoenable() 로만 */
#define IRQ_NESTED_THREAD	GOT_YOU_MORON	/* [한국어] 중첩 스레드 여부는 irq_settings_is_nested_thread() 로만 */
#define IRQ_PER_CPU_DEVID	GOT_YOU_MORON	/* [한국어] per-CPU devid 여부는 irq_settings_is_per_cpu_devid() 로만 */
#define IRQ_IS_POLLED		GOT_YOU_MORON	/* [한국어] 폴링 병행 여부는 irq_settings_is_polled() 로만 */
#define IRQ_DISABLE_UNLAZY	GOT_YOU_MORON	/* [한국어] 즉시 마스크 여부는 irq_settings_disable_unlazy() 로만 */
#define IRQ_HIDDEN		GOT_YOU_MORON	/* [한국어] 목록 감춤 여부는 irq_settings_is_hidden() 로만 */
#define IRQ_NO_DEBUG		GOT_YOU_MORON	/* [한국어] 오탐 감지 제외 여부는 irq_settings_no_debug() 로만 */
/* [한국어] IRQF_MODIFY_MASK 만 #undef 를 먼저 하는 이유: 이것은 위 IRQ_* 들과
 * 달리 공개 헤더에서 이미 #define 으로 정의되어 있다. 그냥 다시 #define 하면
 * "매크로 재정의" 경고가 난다. 앞의 IRQ_* 들은 enum 상수라 그 문제가 없다. */
#undef IRQF_MODIFY_MASK
#define IRQF_MODIFY_MASK	GOT_YOU_MORON	/* [한국어] 안전 마스크는 irq_settings_clr_and_set() 안에서만 쓰인다 */

/*
 * [한국어]
 * irq_settings_clr_and_set - 설정 비트를 안전 마스크로 걸러 한 번에 지우고 세운다
 *
 * @desc: 대상 인터럽트 서술자. 호출자가 이미 서술자 락을 쥐고 있어야 한다.
 * @clr:  지울 비트들. IRQF_MODIFY_MASK 밖의 비트는 조용히 무시된다.
 * @set:  세울 비트들. 역시 마스크 밖은 무시된다.
 *
 * 이 파일에서 유일하게 여러 비트를 한꺼번에 바꾸는 함수이고, 유일하게
 * _IRQF_MODIFY_MASK 를 쓰는 함수다. 나머지 접근자들은 자기가 아는 비트 하나만
 * 다루므로 걸러 낼 필요가 없다.
 *
 * 왜 마스크로 거르는가: status_use_accessors 에는 설정 비트와 함께 IRQD_* 계열의
 * 실행 중 상태(처리 진행 중, 마스크됨, 대기 중)가 들어 있다. 공개 API 인
 * irq_modify_status() 는 드라이버가 부르는 것이라 어떤 값이 올지 알 수 없는데,
 * 그것이 상태 비트를 건드리면 인터럽트 처리 상태 기계가 조용히 깨진다.
 * 마스크가 그 경계를 강제한다 — 실수로 넘어온 비트는 오류가 아니라 무시로 처리된다.
 *
 * 지우기를 세우기보다 먼저 하는 순서도 의미가 있다. clr 과 set 에 같은 비트가
 * 있으면 결과는 "세움"이 된다 — 호출자가 "이 묶음을 지우고 이것들만 남겨라"
 * 라는 뜻으로 쓸 수 있게 하는 순서다.
 *
 * 실행 컨텍스트: 인라인 함수라 호출자의 문맥에서 돈다. 잠들지 않고 락도 잡지
 * 않으므로, 서술자 락은 호출자가 책임진다.
 *
 * 호출 체인:
 *   irq_modify_status() (kernel/irq/chip.c) → [이 함수]
 *   irq_set_status_flags()/irq_clear_status_flags() → irq_modify_status() → [이 함수]
 */
static inline void
irq_settings_clr_and_set(struct irq_desc *desc, u32 clr, u32 set)
{
	desc->status_use_accessors &= ~(clr & _IRQF_MODIFY_MASK);	/* [한국어] 먼저 지운다. clr 을 마스크로 걸러 설정 비트만 남긴 뒤 그것들의 보수를 AND 한다 */
	desc->status_use_accessors |= (set & _IRQF_MODIFY_MASK);	/* [한국어] 그 다음 세운다. 같은 비트가 clr 과 set 에 모두 있으면 결과는 세움이 된다 */
}

/*
 * [한국어]
 * irq_settings_is_per_cpu - 이 인터럽트가 CPU 마다 따로 존재하는지 묻는다
 *
 * @desc:   대상 서술자.
 * @return: per-CPU 인터럽트면 참(0 이 아닌 값), 아니면 거짓.
 *
 * per-CPU 인터럽트는 하나의 번호가 CPU 마다 별개의 인터럽트 선을 뜻한다.
 * 로컬 타이머와 IPI 가 대표적이다. 그래서 "어느 CPU 로 보낼까" 라는 친화도
 * 개념이 성립하지 않고, 마스크와 통계도 CPU 별로 따로 다뤄야 한다.
 *
 * 반환형이 bool 인데 비트 연산 결과를 그대로 돌려주는 것에 주의 — C 가 0 이
 * 아닌 값을 참으로 변환해 주므로 동작에는 문제가 없다. 이 파일의 다른 판정
 * 접근자들도 모두 같은 형태다.
 *
 * 실행 컨텍스트: 인라인. 락 없이 읽는다 — 이 비트는 대개 초기화 때 한 번
 * 정해지고 이후 바뀌지 않아 경쟁이 없다.
 *
 * 호출 체인:
 *   irq_can_set_affinity()/__setup_irq() (kernel/irq/manage.c) → [이 함수]
 *   irq_percpu_enable()/irq_percpu_disable() (kernel/irq/chip.c) → [이 함수]
 */
static inline bool irq_settings_is_per_cpu(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_PER_CPU;	/* [한국어] 봉인된 IRQ_PER_CPU 대신 위에서 복사해 둔 사본을 쓴다 */
}

/*
 * [한국어]
 * irq_settings_is_per_cpu_devid - CPU 마다 다른 dev_id 를 갖는 인터럽트인지 묻는다
 *
 * @desc:   대상 서술자.
 * @return: per-CPU devid 인터럽트면 참.
 *
 * 위 is_per_cpu 보다 한 걸음 더 나아간 성질이다. PER_CPU 는 "인터럽트 선이
 * CPU 마다 있다"는 뜻이고, 이쪽은 그에 더해 "핸들러에 넘길 장치 문맥도
 * CPU 마다 다르다"는 뜻이다.
 *
 * 무엇이 달라지는가: 이 비트가 서 있으면 action 의 dev_id 자리를
 * percpu_dev_id 라는 __percpu 포인터로 해석한다. 핸들러가 불릴 때 커널이
 * 현재 CPU 의 몫을 꺼내 넘겨 주므로, 드라이버는 CPU 별 상태를 자연스럽게 쓸 수 있다.
 *
 * request_percpu_irq()/free_percpu_irq() 라는 별도의 API 쌍이 존재하는 이유가
 * 이것이다 — 보통의 request_irq() 로는 이 인터럽트를 다룰 수 없다.
 *
 * 실행 컨텍스트: 인라인. 락 없이 읽는다.
 *
 * 호출 체인:
 *   request_percpu_irq()/__free_percpu_irq() (kernel/irq/manage.c) → [이 함수]
 */
static inline bool irq_settings_is_per_cpu_devid(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_PER_CPU_DEVID;	/* [한국어] PER_CPU 와는 별개의 비트다 — 둘 다 서 있는 것이 보통이지만 뜻이 다르다 */
}

/*
 * [한국어]
 * irq_settings_set_per_cpu - 이 인터럽트를 per-CPU 로 표시한다
 *
 * @desc: 대상 서술자.
 *
 * 위 is_per_cpu 의 짝이 되는 설정자다. 지우는 함수가 없다는 점이 눈에 띄는데,
 * 이는 의도적이다 — per-CPU 성질은 하드웨어의 구조에서 오는 것이라 실행 중에
 * 사라질 수 없다. 한 번 표시하면 그 서술자가 없어질 때까지 유지된다.
 *
 * 언제 불리는가: irq_set_percpu_devid() 가 서술자를 per-CPU 로 전환할 때,
 * 그리고 아키텍처의 인터럽트 컨트롤러 드라이버가 IPI 나 로컬 타이머 번호를
 * 준비할 때다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락은 호출자가 쥔다.
 *
 * 호출 체인:
 *   irq_set_percpu_devid_flags()/irq_set_percpu_devid() (kernel/irq/irqdesc.c) → [이 함수]
 */
static inline void irq_settings_set_per_cpu(struct irq_desc *desc)
{
	desc->status_use_accessors |= _IRQ_PER_CPU;	/* [한국어] OR 로 세우기만 한다 — 되돌리는 접근자는 일부러 두지 않았다 */
}

/*
 * [한국어]
 * irq_settings_set_no_balancing - 이 인터럽트의 친화도를 고정한다
 *
 * @desc: 대상 서술자.
 *
 * 이 비트가 서면 사용자도 커널의 자동 조정도 이 인터럽트를 다른 CPU 로 옮길
 * 수 없다. /proc/irq/N/smp_affinity 쓰기가 거절되고, irqbalance 데몬도
 * 그 항목을 건너뛴다.
 *
 * 왜 고정해야 하는 인터럽트가 있는가: 특정 CPU 에 물리적으로 묶여 있거나
 * (per-CPU 인터럽트), 옮기는 순간 하드웨어 상태가 어긋나는 경우가 있다.
 * 또 실시간 구성에서 특정 CPU 를 인터럽트로부터 격리해 두었을 때, 그 격리를
 * 사용자가 실수로 깨지 못하게 막는 용도로도 쓴다.
 *
 * 짝이 되는 판정자는 아래 has_no_balance_set 이다. 이름이 is_ 가 아니라
 * has_..._set 인 이유는 그 함수 주석에 있다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락은 호출자가 쥔다.
 *
 * 호출 체인:
 *   irq_set_percpu_devid_flags() → [이 함수] (per-CPU 는 자동으로 고정된다)
 */
static inline void irq_settings_set_no_balancing(struct irq_desc *desc)
{
	desc->status_use_accessors |= _IRQ_NO_BALANCING;	/* [한국어] 세우기만 한다. 지우는 접근자가 없는 이유는 set_per_cpu 와 같다 */
}

/*
 * [한국어]
 * irq_settings_has_no_balance_set - 친화도가 고정되어 있는지 묻는다
 *
 * @desc:   대상 서술자.
 * @return: 고정되어 있으면 참, 옮길 수 있으면 거짓.
 *
 * 이름이 is_no_balancing 이 아니라 has_no_balance_set 인 것에 의미가 있다.
 * 이 함수는 "균형 조정을 하지 않는 상태인가"를 묻는 것이 아니라 "NO_BALANCING
 * 비트가 세워져 있는가"를 묻는다. 실제로 친화도를 바꿀 수 있는지는 이 비트
 * 말고도 chip 이 irq_set_affinity 를 제공하는지 등 여러 조건이 걸려 있어,
 * 이 함수 하나로 결론이 나지 않는다.
 *
 * 그래서 호출자인 irq_can_set_affinity_usr() 같은 함수는 이 결과를 여러
 * 조건 중 하나로만 쓴다.
 *
 * 실행 컨텍스트: 인라인. 락 없이 읽는다.
 *
 * 호출 체인:
 *   irq_can_set_affinity_usr() (kernel/irq/manage.c) → [이 함수]
 *   → /proc/irq/N/smp_affinity 쓰기 허용 여부
 */
static inline bool irq_settings_has_no_balance_set(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_NO_BALANCING;	/* [한국어] 비트만 본다 — 실제 변경 가능 여부는 호출자가 다른 조건과 합쳐 판정한다 */
}

/*
 * [한국어]
 * irq_settings_get_trigger_mask - 트리거 방식과 극성을 담은 하위 비트를 꺼낸다
 *
 * @desc:   대상 서술자.
 * @return: IRQ_TYPE_* 값 하나. 예: IRQ_TYPE_EDGE_RISING, IRQ_TYPE_LEVEL_HIGH.
 *
 * 이 함수만 _IRQ_* 사본이 아니라 IRQ_TYPE_SENSE_MASK 를 그대로 쓴다. 위
 * #define 무리가 봉인한 것은 IRQ_PER_CPU 같은 성질 플래그들뿐이고, 트리거
 * 종류를 나타내는 IRQ_TYPE_* 계열은 봉인 대상이 아니기 때문이다. 이 값들은
 * 장치 트리와 ACPI 에서 그대로 넘어와 chip 드라이버까지 전달되어야 해서,
 * 내부에서 가려 버리면 오히려 불편하다.
 *
 * 무엇이 담기는가: 엣지인지 레벨인지, 그리고 상승/하강 또는 High/Low 인지가
 * 한 값으로 인코딩되어 있다. 위 _IRQ_LEVEL 비트가 "레벨인가"라는 요약이라면,
 * 이쪽은 그 원본 정보다.
 *
 * 왜 둘 다 필요한가: 흐름 제어 핸들러를 고를 때는 레벨/엣지 구분만 있으면
 * 되지만(_IRQ_LEVEL), chip 드라이버에 방식을 설정할 때는 극성까지 알아야 한다.
 *
 * 실행 컨텍스트: 인라인. 락 없이 읽는다.
 *
 * 호출 체인:
 *   __irq_set_trigger() (kernel/irq/manage.c) → [이 함수]
 *   → 요청한 트리거와 현재 설정이 같은지 비교
 */
static inline u32 irq_settings_get_trigger_mask(struct irq_desc *desc)
{
	return desc->status_use_accessors & IRQ_TYPE_SENSE_MASK;	/* [한국어] 트리거 종류는 봉인 대상이 아니라 공개 이름을 그대로 쓴다 */
}

/*
 * [한국어]
 * irq_settings_set_trigger_mask - 트리거 방식과 극성을 기록한다
 *
 * @desc: 대상 서술자.
 * @mask: 새 IRQ_TYPE_* 값.
 *
 * 지우고 세우는 두 단계로 되어 있다. 트리거 종류는 여러 비트로 인코딩된
 * 하나의 값이라, 새 값을 OR 로만 얹으면 옛 비트가 남아 정의되지 않은 조합이
 * 된다. 그래서 먼저 그 자리를 통째로 비운 뒤 새 값을 넣는다.
 *
 * 위 clr_and_set 과 달리 _IRQF_MODIFY_MASK 로 거르지 않고 IRQ_TYPE_SENSE_MASK
 * 로 거른다. 이 함수는 트리거 필드만 다루도록 좁게 정해져 있어, 그 자리를
 * 벗어난 비트가 들어오면 무시하는 편이 맞다.
 *
 * 언제 불리는가: __irq_set_trigger() 가 chip->irq_set_type() 을 불러 하드웨어
 * 설정에 성공한 뒤에만 부른다 — 하드웨어와 서술자의 기록이 어긋나지 않게
 * 하드웨어를 먼저 바꾸고 기록을 뒤에 남기는 순서다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락은 호출자가 쥔다.
 *
 * 호출 체인:
 *   __irq_set_trigger() (kernel/irq/manage.c) → [이 함수]
 */
static inline void
irq_settings_set_trigger_mask(struct irq_desc *desc, u32 mask)
{
	desc->status_use_accessors &= ~IRQ_TYPE_SENSE_MASK;	/* [한국어] 트리거 자리를 통째로 비운다 — 여러 비트 인코딩이라 남은 비트가 있으면 뜻이 깨진다 */
	desc->status_use_accessors |= mask & IRQ_TYPE_SENSE_MASK;	/* [한국어] 새 값을 그 자리에만 넣는다. 범위 밖 비트는 무시한다 */
}

/*
 * [한국어]
 * irq_settings_is_level - 레벨 트리거 인터럽트인지 묻는다
 *
 * @desc:   대상 서술자.
 * @return: 레벨이면 참, 엣지면 거짓.
 *
 * 이 판정이 인터럽트 처리 방식 전체를 가른다.
 *
 * 레벨 트리거: 원인이 사라질 때까지 신호가 계속 어서션 상태다. 그래서 핸들러를
 * 부르기 전에 반드시 마스크해야 하고(안 하면 같은 인터럽트가 무한히 재진입한다),
 * 핸들러가 장치의 원인을 지운 뒤에 언마스크해야 한다. handle_level_irq() 가
 * 그 순서를 구현한다.
 *
 * 엣지 트리거: 신호의 변화 순간만 잡히므로 마스크할 필요가 없다. 대신 핸들러가
 * 도는 동안 새 엣지가 오면 그것을 놓치지 않도록 "다시 실행해야 함" 표시를
 * 남겨야 한다. handle_edge_irq() 가 그 재실행 루프를 구현한다.
 *
 * 위 get_trigger_mask 와의 관계: 그쪽은 극성까지 담은 원본 값이고, 이쪽은
 * 흐름 제어를 고르는 데 필요한 요약이다. 요약을 따로 두면 판정이 비트 하나로 끝난다.
 *
 * 실행 컨텍스트: 인라인. 락 없이 읽는다.
 *
 * 호출 체인:
 *   irq_set_handler()/__irq_set_trigger() → [이 함수]
 *   irq_pm 과 proc.c 의 진단 출력 → [이 함수]
 */
static inline bool irq_settings_is_level(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_LEVEL;	/* [한국어] 흐름 제어 핸들러 선택의 근거가 되는 한 비트 */
}

/*
 * [한국어]
 * irq_settings_clr_level - 레벨 표시를 지운다 (= 엣지로 만든다)
 *
 * @desc: 대상 서술자.
 *
 * 아래 set_level 과 짝을 이루며, __irq_set_trigger() 가 새 트리거 방식을
 * 하드웨어에 설정한 뒤 서술자의 요약 비트를 맞추는 데 쓴다.
 *
 * 왜 clr 과 set 이 따로 있는가: 트리거를 바꾸는 코드는 새 방식이 레벨인지
 * 엣지인지에 따라 둘 중 하나를 부른다. 한 함수에 불리언을 받게 만들 수도
 * 있었겠지만, 호출부에서 어느 쪽으로 가는지가 이름으로 드러나는 편이 낫다.
 *
 * 순서 주의: 이 비트는 하드웨어 설정이 성공한 뒤에만 바꿔야 한다. 미리
 * 바꿔 두고 하드웨어 설정이 실패하면 기록과 실제가 어긋나, 이후의 흐름 제어가
 * 잘못된 방식으로 동작한다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락은 호출자가 쥔다.
 *
 * 호출 체인:
 *   __irq_set_trigger() (kernel/irq/manage.c) → [이 함수]
 */
static inline void irq_settings_clr_level(struct irq_desc *desc)
{
	desc->status_use_accessors &= ~_IRQ_LEVEL;	/* [한국어] 레벨 비트를 내린다 — 이제 엣지로 취급된다 */
}

/*
 * [한국어]
 * irq_settings_set_level - 레벨 표시를 세운다
 *
 * @desc: 대상 서술자.
 *
 * 위 clr_level 의 짝. 하드웨어를 레벨 트리거로 설정하는 데 성공한 뒤에 부른다.
 *
 * 이 비트가 세워지면 이후 이 인터럽트는 handle_level_irq() 계열의 흐름 제어를
 * 따르게 된다 — 핸들러 전에 마스크, 후에 언마스크하는 방식이다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락은 호출자가 쥔다.
 *
 * 호출 체인:
 *   __irq_set_trigger() (kernel/irq/manage.c) → [이 함수]
 */
static inline void irq_settings_set_level(struct irq_desc *desc)
{
	desc->status_use_accessors |= _IRQ_LEVEL;	/* [한국어] 레벨 비트를 세운다 */
}

/*
 * [한국어]
 * irq_settings_can_request - request_irq() 로 이 인터럽트를 요청해도 되는지 묻는다
 *
 * @desc:   대상 서술자.
 * @return: 요청할 수 있으면 참, 막혀 있으면 거짓.
 *
 * 반환값이 비트의 부정이라는 점에 주의한다. 저장된 비트는 NOREQUEST("요청
 * 금지")인데 함수 이름은 can_request("요청 가능")이라, 안에서 ! 로 뒤집는다.
 * 이 파일의 can_ 계열(can_request, can_thread, can_probe, can_autoenable)이
 * 모두 같은 형태다 — 비트는 금지를 나타내고 함수는 허용을 묻는다.
 *
 * 왜 비트를 금지로 두는가: 새로 만든 서술자를 0 으로 채우면 모든 것이
 * 허용되어 버린다. 금지를 기본값으로 하려면 비트가 금지를 뜻해야 하고,
 * 실제로 _IRQ_DEFAULT_INIT_FLAGS 가 NOREQUEST 를 포함한다. 컨트롤러
 * 드라이버가 명시적으로 열어 주기 전에는 아무도 그 번호를 못 쓴다.
 *
 * 이 판정이 실패하면 request_irq() 는 -EINVAL 을 돌려준다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락 아래에서 읽는다.
 *
 * 호출 체인:
 *   request_threaded_irq() → __setup_irq() (kernel/irq/manage.c) → [이 함수]
 */
static inline bool irq_settings_can_request(struct irq_desc *desc)
{
	return !(desc->status_use_accessors & _IRQ_NOREQUEST);	/* [한국어] 비트는 "금지", 함수는 "가능" — 그래서 부정한다 */
}

/*
 * [한국어]
 * irq_settings_clr_norequest - 요청 금지를 풀어 이 인터럽트를 열어 준다
 *
 * @desc: 대상 서술자.
 *
 * 인터럽트 컨트롤러 드라이버가 "이 번호는 실재하며 드라이버가 붙어도 된다"고
 * 선언하는 방법이다. 서술자는 기본적으로 금지 상태로 태어나므로, 이 호출이
 * 없으면 그 번호에는 아무도 request_irq() 를 할 수 없다.
 *
 * 언제 불리는가: irq_domain 이 인터럽트를 매핑할 때, 또는 아키텍처 코드가
 * irq_set_probe()/irq_modify_status() 를 통해 부를 때다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락은 호출자가 쥔다.
 *
 * 호출 체인:
 *   irq_modify_status() (kernel/irq/chip.c) → [이 함수]
 */
static inline void irq_settings_clr_norequest(struct irq_desc *desc)
{
	desc->status_use_accessors &= ~_IRQ_NOREQUEST;	/* [한국어] 금지 비트를 내려 요청을 허용한다 */
}

/*
 * [한국어]
 * irq_settings_set_norequest - 요청을 다시 막는다
 *
 * @desc: 대상 서술자.
 *
 * 위 clr_norequest 의 짝. 인터럽트를 회수하거나 도메인 매핑을 해제할 때,
 * 그 번호가 다시 쓰이기 전에 잠가 두는 데 쓴다.
 *
 * 왜 필요한가: 번호를 반납했는데 잠그지 않으면, 다음에 그 번호를 요청한
 * 드라이버가 아직 정리되지 않은 서술자에 핸들러를 붙일 수 있다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락은 호출자가 쥔다.
 *
 * 호출 체인:
 *   irq_modify_status()/irq_domain_disassociate() → [이 함수]
 */
static inline void irq_settings_set_norequest(struct irq_desc *desc)
{
	desc->status_use_accessors |= _IRQ_NOREQUEST;	/* [한국어] 금지 비트를 세워 요청을 막는다 */
}

/*
 * [한국어]
 * irq_settings_can_thread - 이 핸들러를 강제로 스레드화해도 되는지 묻는다
 *
 * @desc:   대상 서술자.
 * @return: 스레드로 돌려도 되면 참, 하드 인터럽트 문맥에 남겨야 하면 거짓.
 *
 * 위 can_request 와 같은 형태로, 저장된 비트는 NOTHREAD(금지)이고 함수는
 * 허용을 묻는다.
 *
 * 무엇을 위한 판정인가: threadirqs 부트 옵션이나 PREEMPT_RT 커널에서는
 * 거의 모든 인터럽트 핸들러를 커널 스레드로 옮겨, 하드 인터럽트 문맥의
 * 지연 시간을 줄인다. 그런데 그렇게 하면 안 되는 인터럽트가 있다.
 *
 * 옮기면 안 되는 것들: 타이머 틱(스케줄러 자신이 그것에 의존한다), IPI
 * (스레드를 깨우는 데 IPI 가 필요한데 그 IPI 를 스레드로 미루면 순환한다),
 * 그리고 per-CPU 인터럽트 일반이다. 그것들에 NOTHREAD 를 붙여 예외로 둔다.
 *
 * 실행 컨텍스트: 인라인. 요청 경로에서 서술자 락 아래에 읽는다.
 *
 * 호출 체인:
 *   __setup_irq() → irq_setup_forced_threading() (kernel/irq/manage.c) → [이 함수]
 */
static inline bool irq_settings_can_thread(struct irq_desc *desc)
{
	return !(desc->status_use_accessors & _IRQ_NOTHREAD);	/* [한국어] 비트는 금지, 함수는 허용 — can_request 와 같은 규칙 */
}

/*
 * [한국어]
 * irq_settings_clr_nothread - 강제 스레드화 예외를 푼다
 *
 * @desc: 대상 서술자.
 *
 * 아래 set_nothread 의 짝이다. 예외로 두었던 인터럽트를 다시 일반 규칙으로
 * 되돌릴 때 쓰지만, 실제로 불리는 경우는 드물다 — 스레드화 예외는 대개
 * 하드웨어의 성질에서 오는 것이라 실행 중에 바뀌지 않는다.
 *
 * 대칭성을 위해 존재하는 접근자에 가깝다. irq_modify_status() 가 임의의
 * 플래그 조합을 받을 수 있어, 지우는 쪽도 갖춰 두어야 그 API 가 완결된다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락은 호출자가 쥔다.
 *
 * 호출 체인:
 *   irq_modify_status() (kernel/irq/chip.c) → [이 함수]
 */
static inline void irq_settings_clr_nothread(struct irq_desc *desc)
{
	desc->status_use_accessors &= ~_IRQ_NOTHREAD;	/* [한국어] 예외 표시를 내려 일반 규칙을 따르게 한다 */
}

/*
 * [한국어]
 * irq_settings_set_nothread - 이 인터럽트를 강제 스레드화에서 제외한다
 *
 * @desc: 대상 서술자.
 *
 * 위 can_thread 설명의 "옮기면 안 되는 것들"에 이 표시를 붙인다. 아키텍처의
 * 타이머와 IPI 초기화 코드가 대표적인 호출자다.
 *
 * 이 표시를 빠뜨리면 무슨 일이 생기는가: threadirqs 커널에서 타이머 틱이
 * 스레드로 옮겨지고, 그 스레드를 깨우려면 스케줄러가 돌아야 하는데 스케줄러는
 * 타이머 틱에 의존한다. 부팅이 그 자리에서 멈춘다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락은 호출자가 쥔다.
 *
 * 호출 체인:
 *   irq_set_percpu_devid_flags()/아키텍처 타이머 초기화 → [이 함수]
 */
static inline void irq_settings_set_nothread(struct irq_desc *desc)
{
	desc->status_use_accessors |= _IRQ_NOTHREAD;	/* [한국어] 스레드화 예외로 표시한다 */
}

/*
 * [한국어]
 * irq_settings_can_probe - 자동 탐지(autoprobe)의 후보로 삼아도 되는지 묻는다
 *
 * @desc:   대상 서술자.
 * @return: 탐지 대상이면 참, 제외면 거짓.
 *
 * 자동 탐지란: 어떤 인터럽트 번호를 쓰는지 스스로 알려 주지 않는 오래된
 * ISA 장치를 위해, 커널이 후보 번호들을 모두 열어 두고 장치를 자극한 뒤
 * 어느 번호가 울리는지 보는 방식이다. probe_irq_on()/probe_irq_off() 쌍이
 * 그 절차이며 autoprobe.c 에 있다.
 *
 * 왜 대부분 제외인가: 기본 플래그에 NOPROBE 가 들어 있어 서술자는 제외
 * 상태로 태어난다. 아무 번호나 열어 두면 그 사이 들어온 진짜 인터럽트가
 * 핸들러 없이 처리되어 시스템이 불안정해진다. 오래된 ISA 구간의 번호들만
 * 아키텍처 코드가 명시적으로 열어 준다.
 *
 * 오늘날에는 거의 쓰이지 않는 기능이지만, 지원하는 하드웨어가 남아 있어
 * 코드가 유지된다.
 *
 * 실행 컨텍스트: 인라인. 탐지 경로에서 서술자 락 아래에 읽는다.
 *
 * 호출 체인:
 *   probe_irq_on() (kernel/irq/autoprobe.c) → [이 함수]
 */
static inline bool irq_settings_can_probe(struct irq_desc *desc)
{
	return !(desc->status_use_accessors & _IRQ_NOPROBE);	/* [한국어] 비트는 금지, 함수는 허용 */
}

/*
 * [한국어]
 * irq_settings_clr_noprobe - 자동 탐지 대상으로 열어 준다
 *
 * @desc: 대상 서술자.
 *
 * 아키텍처 코드가 "이 번호는 자동 탐지에 써도 안전하다"고 표시하는 방법이다.
 * x86 의 레거시 ISA 인터럽트(0~15) 초기화가 대표적인 호출자다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락은 호출자가 쥔다.
 *
 * 호출 체인:
 *   irq_set_probe() (kernel/irq/chip.c) → irq_modify_status() → [이 함수]
 */
static inline void irq_settings_clr_noprobe(struct irq_desc *desc)
{
	desc->status_use_accessors &= ~_IRQ_NOPROBE;	/* [한국어] 제외 표시를 내려 탐지 후보로 만든다 */
}

/*
 * [한국어]
 * irq_settings_set_noprobe - 자동 탐지에서 다시 제외한다
 *
 * @desc: 대상 서술자.
 *
 * 위 clr_noprobe 의 짝. 서술자의 기본 상태가 이미 제외이므로, 이 함수는
 * 한 번 열어 준 번호를 다시 잠글 때만 필요하다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락은 호출자가 쥔다.
 *
 * 호출 체인:
 *   irq_set_noprobe() (kernel/irq/chip.c) → irq_modify_status() → [이 함수]
 */
static inline void irq_settings_set_noprobe(struct irq_desc *desc)
{
	desc->status_use_accessors |= _IRQ_NOPROBE;	/* [한국어] 탐지 대상에서 제외한다 */
}

/*
 * [한국어]
 * irq_settings_can_autoenable - request_irq() 직후 자동으로 켜도 되는지 묻는다
 *
 * @desc:   대상 서술자.
 * @return: 자동으로 켜도 되면 참, 드라이버가 직접 켜야 하면 거짓.
 *
 * can_ 계열의 마지막이며, 역시 저장된 비트는 NOAUTOEN(금지)이다. 다만 앞의
 * 셋과 달리 짝이 되는 set_/clr_ 접근자가 없다 — 이 비트는 드라이버가 요청
 * 전에 irq_set_status_flags() 로 붙이고, 코어는 읽기만 하기 때문이다.
 *
 * 무엇을 정하는가: 보통 request_irq() 는 핸들러 등록을 마치면 그 인터럽트를
 * 곧바로 켠다. 그런데 하드웨어 초기화가 아직 끝나지 않았는데 인터럽트가
 * 들어오면, 핸들러가 준비되지 않은 장치 상태를 보게 된다. 이 비트가 서 있으면
 * 코어는 켜지 않고 두고, 드라이버가 준비를 마친 뒤 enable_irq() 를 부른다.
 *
 * 주의할 점: 이 방식을 쓰면 disable 깊이(depth)가 1 에서 시작하므로,
 * 드라이버는 반드시 enable_irq() 를 한 번 불러야 인터럽트가 열린다.
 *
 * 실행 컨텍스트: 인라인. 요청 경로에서 서술자 락 아래에 읽는다.
 *
 * 호출 체인:
 *   __setup_irq() (kernel/irq/manage.c) → [이 함수]
 */
static inline bool irq_settings_can_autoenable(struct irq_desc *desc)
{
	return !(desc->status_use_accessors & _IRQ_NOAUTOEN);	/* [한국어] 비트는 금지, 함수는 허용. set/clr 접근자가 없는 유일한 can_ 계열이다 */
}

/*
 * [한국어]
 * irq_settings_is_nested_thread - 부모 스레드 안에서 중첩 실행되는 인터럽트인지 묻는다
 *
 * @desc:   대상 서술자.
 * @return: 중첩 방식이면 참.
 *
 * 어떤 구조를 위한 것인가: I2C 나 SPI 로 연결된 GPIO 확장기, PMIC 같은 칩은
 * 자기 인터럽트 상태 레지스터를 읽으려면 버스 트랜잭션을 해야 하고, 그것은
 * 잠들 수 있는 동작이라 하드 인터럽트 문맥에서 할 수 없다.
 *
 * 그래서 이렇게 한다: 부모(그 칩이 물린 실제 인터럽트 선)에 스레드 핸들러를
 * 등록하고, 그 스레드가 버스로 칩의 상태를 읽는다. 어느 자식 인터럽트가
 * 울렸는지 알아내면 handle_nested_irq() 로 그 자식의 핸들러를 같은 스레드
 * 문맥에서 직접 부른다.
 *
 * 이 비트가 서 있으면 코어는 그 자식에게 자체 하드 인터럽트 핸들러도, 자체
 * 스레드도 만들지 않는다 — 부모가 대신 불러 주기 때문이다.
 *
 * 실행 컨텍스트: 인라인. 요청 경로에서 서술자 락 아래에 읽는다.
 *
 * 호출 체인:
 *   __setup_irq() (kernel/irq/manage.c) → [이 함수]
 *   handle_nested_irq() (kernel/irq/chip.c) 가 실제 호출을 수행
 */
static inline bool irq_settings_is_nested_thread(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_NESTED_THREAD;	/* [한국어] 자체 핸들러·스레드를 만들지 말라는 표시 */
}

/*
 * [한국어]
 * irq_settings_is_polled - 드라이버가 폴링을 병행하는 인터럽트인지 묻는다
 *
 * @desc:   대상 서술자.
 * @return: 폴링 병행이면 참.
 *
 * 무엇을 막기 위한 표시인가: 커널은 IRQ_NONE("내 것이 아니다")만 계속
 * 돌려주는 인터럽트를 오작동으로 보고, 일정 횟수가 넘으면 "nobody cared"
 * 라는 메시지와 함께 그 인터럽트를 꺼 버린다. 고장난 하드웨어가 인터럽트를
 * 무한히 쏘아 시스템을 멈추게 하는 것을 막는 안전 장치다.
 *
 * 그런데 폴링을 병행하는 드라이버는 정상 동작 중에도 IRQ_NONE 을 자주
 * 돌려준다. 인터럽트가 도착했을 때 이미 폴링 타이머가 일을 끝내 놓아 처리할
 * 것이 남아 있지 않기 때문이다. 그 정상 동작이 오작동으로 오해받아 인터럽트가
 * 꺼지면, 폴링만 남아 성능이 급격히 떨어진다.
 *
 * 이 비트가 서 있으면 spurious.c 가 그 계산에서 이 인터럽트를 빼 준다.
 *
 * 실행 컨텍스트: 인라인. 인터럽트 처리 직후 note_interrupt() 에서 읽힌다.
 *
 * 호출 체인:
 *   handle_irq_event_percpu() → note_interrupt() (kernel/irq/spurious.c) → [이 함수]
 */
static inline bool irq_settings_is_polled(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_IS_POLLED;	/* [한국어] 오탐 계산에서 빼 달라는 표시 */
}

/*
 * [한국어]
 * irq_settings_disable_unlazy - disable_irq() 에서 즉시 마스크해야 하는지 묻는다
 *
 * @desc:   대상 서술자.
 * @return: 즉시 마스크해야 하면 참, 게으르게 미뤄도 되면 거짓.
 *
 * 게으른 비활성화란: disable_irq() 는 기본적으로 하드웨어를 건드리지 않는다.
 * 소프트웨어 표시만 남겨 두었다가, 실제로 인터럽트가 들어왔을 때 그때 마스크하고
 * 핸들러를 부르지 않는다. 컨트롤러 접근이 비싸고, 대부분의 경우 비활성화 구간에
 * 인터럽트가 아예 오지 않기 때문에 이 방식이 훨씬 싸다.
 *
 * 게으르면 안 되는 경우: 인터럽트 선을 여러 장치가 공유하는데 그중 하나가
 * 계속 어서션하고 있으면, 마스크되지 않은 그 선이 CPU 를 무한히 방해한다.
 * 또 어떤 컨트롤러는 처리되지 않은 인터럽트를 끈질기게 재전송한다. 그런
 * 하드웨어에서는 disable 시점에 실제로 마스크를 걸어야 한다.
 *
 * 드라이버가 그런 사정을 알고 있을 때 IRQ_DISABLE_UNLAZY 를 붙인다.
 *
 * 실행 컨텍스트: 인라인. 비활성화 경로에서 서술자 락 아래에 읽는다.
 *
 * 호출 체인:
 *   irq_disable() (kernel/irq/chip.c) → [이 함수]
 *   → 참이면 chip->irq_mask() 를 곧바로 호출
 */
static inline bool irq_settings_disable_unlazy(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_DISABLE_UNLAZY;	/* [한국어] 참이면 disable 시점에 하드웨어를 실제로 마스크한다 */
}

/*
 * [한국어]
 * irq_settings_clr_disable_unlazy - 즉시 마스크 요구를 푼다
 *
 * @desc: 대상 서술자.
 *
 * 위 disable_unlazy 판정의 짝인데, 세우는 접근자는 이 파일에 없다 — 드라이버가
 * irq_set_status_flags(IRQ_DISABLE_UNLAZY) 로 붙이고, 코어는 지우기만 한다.
 *
 * 언제 지우는가: 그 인터럽트에 등록된 마지막 핸들러가 해제될 때다. 요청이
 * 모두 사라진 서술자는 다음에 다른 드라이버가 물려받을 수 있는데, 이전
 * 드라이버의 특수 요구가 남아 있으면 안 된다. 서술자를 중립 상태로 되돌리는
 * 정리 작업의 일부다.
 *
 * 실행 컨텍스트: 인라인. free_irq 경로에서 서술자 락 아래에 실행된다.
 *
 * 호출 체인:
 *   __free_irq() (kernel/irq/manage.c) → [이 함수]
 */
static inline void irq_settings_clr_disable_unlazy(struct irq_desc *desc)
{
	desc->status_use_accessors &= ~_IRQ_DISABLE_UNLAZY;	/* [한국어] 마지막 핸들러가 빠질 때 서술자를 중립 상태로 되돌린다 */
}

/*
 * [한국어]
 * irq_settings_is_hidden - 사용자에게 보이는 목록에서 감춰야 하는지 묻는다
 *
 * @desc:   대상 서술자.
 * @return: 감춰야 하면 참.
 *
 * 무엇을 감추는가: /proc/interrupts 의 줄과 /proc/irq/N/ 디렉터리, 그리고
 * sysfs 항목이다.
 *
 * 왜 감추는 인터럽트가 있는가: 계층형 인터럽트 도메인에서는 하나의 물리
 * 인터럽트가 여러 단계의 가상 인터럽트로 표현된다. 예를 들어 PCI MSI 는
 * MSI 도메인 → 상위 컨트롤러 도메인 → CPU 벡터 순으로 겹쳐 있고, 각 단계마다
 * 서술자가 하나씩 생긴다. 실제로 통계가 쌓이는 것은 맨 아래 하나뿐이고
 * 중간 것들은 항상 0 이라, 모두 보여 주면 목록이 무의미하게 길어진다.
 *
 * 실행 컨텍스트: 인라인. /proc 출력 경로에서 읽는다.
 *
 * 호출 체인:
 *   show_interrupts() (kernel/irq/proc.c) → [이 함수]
 *   register_irq_proc() → [이 함수] (디렉터리를 만들지 정할 때)
 */
static inline bool irq_settings_is_hidden(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_HIDDEN;	/* [한국어] 계층형 도메인의 중간 단계 인터럽트를 사용자 목록에서 뺀다 */
}

/*
 * [한국어]
 * irq_settings_set_no_debug - 오탐 감지와 디버그 계측에서 제외한다
 *
 * @desc: 대상 서술자.
 *
 * 위 is_polled 가 오탐 계산에서 "이 인터럽트는 IRQ_NONE 이 잦은 것이 정상"
 * 이라고 예외를 두는 것이라면, 이쪽은 감지 자체를 끄는 더 강한 조치다.
 *
 * 언제 필요한가: 오탐 감지 로직이 성립하지 않는 구조가 있다. 예를 들어
 * 인터럽트가 실제로 처리되었는지를 커널이 알 수 없는 전달 방식이거나,
 * 핸들러가 반환값으로 처리 여부를 표현하지 않는 특수한 경로다. 그런 곳에서
 * 감지를 켜 두면 정상 동작이 계속 오작동으로 집계된다.
 *
 * 짝이 되는 판정자는 바로 아래 no_debug 이며, 지우는 접근자는 없다 — 이
 * 성질은 인터럽트의 구조에서 오는 것이라 실행 중에 사라지지 않는다.
 *
 * 실행 컨텍스트: 인라인. 서술자 락은 호출자가 쥔다.
 *
 * 호출 체인:
 *   irq_modify_status()/도메인 초기화 코드 → [이 함수]
 */
static inline void irq_settings_set_no_debug(struct irq_desc *desc)
{
	desc->status_use_accessors |= _IRQ_NO_DEBUG;	/* [한국어] 오탐 감지 대상에서 뺀다. 되돌리는 접근자는 두지 않았다 */
}

/*
 * [한국어]
 * irq_settings_no_debug - 오탐 감지에서 제외된 인터럽트인지 묻는다
 *
 * @desc:   대상 서술자.
 * @return: 제외 대상이면 참.
 *
 * 위 set_no_debug 의 짝. 이름에 is_ 나 can_ 가 붙지 않은 유일한 판정자인데,
 * 비트 이름(NO_DEBUG)을 그대로 함수 이름으로 쓴 결과다. can_ 계열처럼
 * 부정하지 않고 비트를 그대로 돌려주므로, 참이면 "디버그하지 않는다"는 뜻이다.
 *
 * 이 함수가 참을 주면 spurious.c 는 그 인터럽트의 IRQ_NONE 반환을 세지 않고,
 * 따라서 "nobody cared" 로 꺼지는 일도 없다.
 *
 * 실행 컨텍스트: 인라인. 인터럽트 처리 직후 호출된다.
 *
 * 호출 체인:
 *   note_interrupt() (kernel/irq/spurious.c) → [이 함수]
 */
static inline bool irq_settings_no_debug(struct irq_desc *desc)
{
	return desc->status_use_accessors & _IRQ_NO_DEBUG;	/* [한국어] 부정하지 않는다 — 참이면 "감지하지 않음" 이다 */
}
