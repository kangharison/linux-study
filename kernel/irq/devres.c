// SPDX-License-Identifier: GPL-2.0
/*
 * [한국어 설명] 장치에 수명을 맡기는 인터럽트 요청 API (devres.c)
 *
 * === 파일의 역할 ===
 * request_irq() 와 짝이 되는 free_irq() 를 드라이버가 직접 부르지 않아도
 * 되게 해 준다. 장치가 떨어져 나갈 때(driver detach) 커널이 알아서 해제한다.
 *
 * devres(device resource management)가 무엇인가: 장치에 매단 자원 목록이다.
 * 드라이버가 devm_ 접두사가 붙은 함수로 자원을 잡으면, 그 자원과 해제
 * 방법이 장치에 기록된다. 장치가 사라질 때 커널이 그 목록을 역순으로
 * 훑으며 전부 반납한다.
 *
 * 왜 필요한가: 드라이버 프로브는 실패 경로가 많다. 인터럽트를 잡은 뒤
 * 메모리 할당이 실패하고, 그 뒤 DMA 설정이 실패하는 식이다. 각 실패마다
 * 그때까지 잡은 것을 역순으로 풀어야 하는데, 경로가 늘어날수록 빠뜨리기
 * 쉽다. devm_ 을 쓰면 그 되돌리기 코드가 통째로 사라진다.
 *
 * 이 파일의 함수들은 모두 같은 골격을 따른다:
 *   1. devres 항목을 먼저 잡는다.
 *   2. 실제 자원을 잡는다.
 *   3. 실패하면 devres 항목을 반납하고 물러난다.
 *   4. 성공하면 항목에 정보를 채우고 장치에 매단다.
 *
 * 순서가 중요하다. devres 항목을 먼저 잡는 이유는, 자원을 먼저 잡았다가
 * 항목 할당이 실패하면 그 자원을 손으로 되돌려야 하기 때문이다. 반대
 * 순서면 실패 처리가 devres_free() 한 줄로 끝난다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 드라이버와 인터럽트 코어 사이의 얇은 층이다:
 *
 *   드라이버 프로브
 *     ↓ devm_request_irq() 등
 *   이 파일                        ← devres 항목을 만들고
 *     ↓
 *   request_threaded_irq() (manage.c) — 실제 요청
 *
 *   장치 detach
 *     ↓ devres 목록을 역순으로
 *   devm_irq_release() 등           ← **이 파일**
 *     ↓
 *   free_irq() (manage.c)
 *
 * 실행 컨텍스트: 전부 프로세스 문맥. 할당과 해제로 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   drivers/base/devres.c — devres_alloc/add/free/release 기반.
 *   manage.c — 실제 인터럽트 요청과 해제.
 *   irqdesc.c — 서술자 범위 할당.
 *   generic-chip.c, irqdomain.c — 각각의 devm 판을 여기서 제공한다.
 *
 * 이 파일에 의존하는 곳: 수많은 장치 드라이버.
 *
 * === 주요 함수/구조체 요약 ===
 * struct irq_devres          — 해제할 인터럽트 번호와 dev_id 를 기억한다.
 * devm_irq_release()         — detach 때 free_irq() 를 부른다.
 * devm_irq_match()           — devm_free_irq 가 항목을 찾는 데 쓴다.
 * devm_request_result()      — 실패를 일관된 형식으로 로그에 남긴다.
 * devm_request_threaded_irq()— 가장 널리 쓰이는 진입점.
 * devm_free_irq()            — 미리 해제하고 싶을 때.
 * struct irq_desc_devres     — 해제할 서술자 범위.
 * struct irq_generic_chip_devres — 걷어 낼 generic chip 설정.
 * devm_irq_domain_instantiate() — 도메인의 devm 판.
 *
 * 오류 메시지를 이 계층에서 찍는 것이 최근의 방침이다. 드라이버마다
 * 제각각인 메시지 대신 일관된 형식을 쓰고, 드라이버는 메시지를 아예
 * 쓰지 않는다 — 아래 devm_request_result() 의 주석 참고.
 */
#include <linux/module.h>	/* [한국어] EXPORT_SYMBOL — 이 파일의 함수들은 대부분 모듈 드라이버가 쓴다 */
#include <linux/interrupt.h>	/* [한국어] request_threaded_irq, free_irq 등 실제 요청 API */
#include <linux/irqdomain.h>	/* [한국어] irq_domain_instantiate/remove — 아래 도메인 devm 판이 쓴다 */
#include <linux/device.h>	/* [한국어] struct device 와 devres 기반 함수들 */
#include <linux/gfp.h>	/* [한국어] GFP_KERNEL — devres 항목 할당 플래그 */
#include <linux/irq.h>	/* [한국어] struct irq_chip_generic 과 irq_gc_flags */

#include "internals.h"	/* [한국어] irq_init_generic_chip() — 코어 내부라 공개 헤더에 없다 */

/*
 * Device resource management aware IRQ request/free implementation.
 */
/* [한국어] (위 영어 주석에 이어) 해제할 인터럽트를 기억하는 devres 항목.
 *
 * free_irq() 가 요구하는 두 값만 담는다. 그 이상은 필요 없다 — detach 때
 * 하는 일이 free_irq() 를 부르는 것뿐이기 때문이다.
 *
 * 이 구조체가 작다는 것이 devres 방식의 특징이다. 자원마다 항목 하나씩
 * 할당하므로, 담는 내용이 많으면 그만큼 메모리를 쓴다. */
struct irq_devres {
	unsigned int irq;
	/* [한국어] 해제할 인터럽트 번호.
	 * 설정자: __devm_request_threaded_irq() 등이 요청에 성공한 뒤 채운다.
	 * 읽는 자: devm_irq_release() 가 free_irq() 에 넘기고, devm_irq_match()
	 *   가 항목을 찾을 때 비교한다.
	 * 값 범위: 유효한 리눅스 인터럽트 번호.
	 * 왜 요청 전이 아니라 후에 채우는가: 요청이 실패하면 이 항목은
	 *   devres_free() 로 그냥 버려진다. 미리 채워도 쓰이지 않는다. */

	void *dev_id;
	/* [한국어] free_irq() 에 넘길 장치 식별자.
	 * 설정자: 위 irq 와 함께 채운다.
	 * 읽는 자: devm_irq_release() 와 devm_irq_match().
	 * 왜 이것도 필요한가: 공유 인터럽트에서는 한 번호에 여러 핸들러가
	 *   붙어 있고, free_irq() 는 dev_id 로 어느 것을 뗄지 정한다. 번호만
	 *   으로는 남의 핸들러를 뗄 수 있다.
	 * 값 범위: 요청 때 준 값 그대로. 공유가 아니면 NULL 일 수 있다. */
};

/*
 * [한국어]
 * devm_irq_release - 장치가 떨어져 나갈 때 인터럽트를 해제한다
 *
 * @dev: 그 장치. 이 구현은 쓰지 않는다 — 해제에 필요한 정보가 모두
 *       아래 res 안에 있기 때문이다.
 * @res: 위 struct irq_devres 항목.
 *
 * devres 코어가 detach 때 부르는 해제 콜백이다. 드라이버가 free_irq() 를
 * 부르지 않아도 되는 것은 이 함수 덕분이다.
 *
 * 반환값이 없는 이유: 해제 경로는 실패해도 물러설 곳이 없다. free_irq()
 * 자체도 void 다.
 *
 * 실행 컨텍스트: 장치 detach, 프로세스 문맥. free_irq() 가 진행 중인
 * 핸들러를 기다리므로 잠들 수 있다.
 *
 * 호출 체인:
 *   device_release_driver() → devres_release_all() → [이 함수] → free_irq()
 */
static void devm_irq_release(struct device *dev, void *res)
{
	struct irq_devres *this = res;	/* [한국어] devres 코어가 void * 로 넘기므로 원래 타입으로 되돌린다 */

	free_irq(this->irq, this->dev_id);	/* [한국어] 요청 때 기억해 둔 두 값으로 해제한다. 진행 중인 핸들러가 끝나기를 기다린다 */
}

/*
 * [한국어]
 * devm_irq_match - devres 항목이 찾는 인터럽트의 것인지 판정한다
 *
 * @dev:    그 장치. 쓰이지 않는다.
 * @res:    검사할 devres 항목.
 * @data:   찾는 조건. struct irq_devres 형태로 irq 와 dev_id 가 채워져 있다.
 * @return: 일치하면 0 이 아닌 값.
 *
 * 아래 devm_free_irq() 가 "이 인터럽트의 항목" 을 찾아 미리 해제할 때 쓴다.
 * 보통의 detach 경로는 목록 전체를 훑으므로 이 함수가 필요 없다.
 *
 * 두 값을 모두 비교하는 이유: 한 장치가 여러 인터럽트를 요청할 수 있고,
 * 같은 번호에 서로 다른 dev_id 로 여러 번 요청할 수도 있다(공유 인터럽트).
 * 번호만 비교하면 엉뚱한 항목을 해제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   devm_free_irq() → devres_release() → [이 함수]
 */
static int devm_irq_match(struct device *dev, void *res, void *data)
{
	struct irq_devres *this = res, *match = data;	/* [한국어] 검사 대상과 찾는 조건. 둘 다 같은 타입이다 */

	return this->irq == match->irq && this->dev_id == match->dev_id;	/* [한국어] 둘 다 같아야 한다. 공유 인터럽트에서 번호만 보면 남의 핸들러를 뗀다 */
}

/*
 * [한국어]
 * devm_request_result - 요청 실패를 일관된 형식으로 로그에 남긴다
 *
 * @dev:       요청한 장치.
 * @rc:        요청 결과. 0 이상이면 성공이다.
 * @irq:       요청한 인터럽트 번호.
 * @handler:   1차 핸들러. 메시지에 심볼 이름으로 찍는다.
 * @thread_fn: 스레드 핸들러. 없으면 NULL 이며 그대로 찍힌다.
 * @devname:   요청 때 준 이름. NULL 이면 빈 문자열로 찍는다.
 * @return:    rc 를 그대로 돌려준다. 성공이면 그 값, 실패면 오류 코드.
 *
 * 왜 이 계층에서 로그를 찍는가: 예전에는 드라이버마다 요청 실패를 자기
 * 형식으로 찍었다. 그러면 메시지가 제각각이고, 어떤 드라이버는 아예 찍지
 * 않아 원인을 알 수 없었다.
 *
 * 이 함수를 두어 형식을 통일하고, 아래 kernel-doc 들이 "호출부에서 추가
 * 오류 메시지를 쓰지 말라" 고 못 박는다. 드라이버는 반환값만 그대로
 * 올려 보내면 된다.
 *
 * dev_err_probe 를 쓰는 것이 요점이다. -EPROBE_DEFER 를 오류가 아니라
 * "나중에 다시 시도" 로 다뤄, 부팅 중 흔한 지연 재시도가 오류 로그를
 * 가득 채우지 않게 한다. 그러면서도 반환값은 그대로 전달한다.
 *
 * 0 이 아니라 0 이상을 성공으로 보는 이유: 아래
 * devm_request_any_context_irq() 가 성공 시 IRQC_IS_HARDIRQ(0) 또는
 * IRQC_IS_NESTED(1) 를 돌려주기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   devm_request_threaded_irq()/devm_request_any_context_irq() → [이 함수]
 */
static int devm_request_result(struct device *dev, int rc, unsigned int irq,
			       irq_handler_t handler, irq_handler_t thread_fn,
			       const char *devname)
{
	if (rc >= 0)	/* [한국어] 0 뿐 아니라 양수도 성공이다 — any_context 판이 IRQC_IS_NESTED(1) 을 돌려준다 */
		return rc;	/* [한국어] 그대로 전달한다 */

	return dev_err_probe(dev, rc, "request_irq(%u) %ps %ps %s\n",	/* [한국어] %ps 가 핸들러 주소를 심볼 이름으로 찍어 어느 함수인지 바로 드러난다 */
			     irq, handler, thread_fn, devname ? : "");	/* [한국어] 이름이 없으면 빈 문자열. GCC 확장인 ?: 로 조건을 짧게 쓴다 */
}

/*
 * [한국어]
 * __devm_request_threaded_irq - 인터럽트를 요청하고 장치에 수명을 매단다
 *
 * @dev:      요청하는 장치.
 * @irq:      요청할 인터럽트 번호.
 * @handler:  1차 핸들러. NULL 이면 커널이 기본 핸들러를 쓴다.
 * @thread_fn:스레드 핸들러. NULL 이면 1차 핸들러가 전부 처리한다.
 * @irqflags: IRQF_* 조합.
 * @devname:  요청자 이름. NULL 이면 장치 이름을 쓴다.
 * @dev_id:   핸들러에 넘길 문맥.
 * @return:   0 이면 성공, 음수면 오류.
 *
 * 이 파일의 표준 골격을 보여 주는 함수다. 네 단계로 이루어진다.
 *
 * devres 항목을 먼저 잡는 순서가 핵심이다. 인터럽트를 먼저 요청했다가
 * 항목 할당이 실패하면 free_irq() 로 되돌려야 하는데, 그 되돌리기가
 * 또 실패할 수 있고 코드도 길어진다. 항목을 먼저 잡으면 실패 처리가
 * devres_free() 한 줄로 끝난다.
 *
 * 밑줄 붙은 이름인 이유: 아래 devm_request_threaded_irq() 가 이것을 감싸며
 * 오류 로그를 더한다. 실제 일은 여기서 하고 로그는 바깥에서 하는 분리다.
 *
 * 실행 컨텍스트: 드라이버 프로브, 프로세스 문맥. 할당으로 잠들 수 있다.
 *
 * 호출 체인:
 *   devm_request_threaded_irq() → [이 함수] → request_threaded_irq()
 */
static int __devm_request_threaded_irq(struct device *dev, unsigned int irq,
				       irq_handler_t handler,
				       irq_handler_t thread_fn,
				       unsigned long irqflags,
				       const char *devname, void *dev_id)
{
	struct irq_devres *dr;	/* [한국어] 해제 정보를 담을 devres 항목 */
	int rc;	/* [한국어] 요청 결과 */

	dr = devres_alloc(devm_irq_release, sizeof(struct irq_devres),	/* [한국어] 항목을 먼저 잡는다. 해제 콜백을 함께 등록해, detach 때 그것이 불린다 */
			  GFP_KERNEL);	/* [한국어] 프로세스 문맥이라 잠들 수 있는 할당을 쓴다 */
	if (!dr)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 아직 인터럽트를 요청하지 않았으므로 되돌릴 것이 없다 — 이것이 순서를 이렇게 잡은 이유다 */

	if (!devname)	/* [한국어] 이름을 주지 않았으면 */
		devname = dev_name(dev);	/* [한국어] 장치 이름을 쓴다. /proc/interrupts 에 이 이름이 나타난다 */

	rc = request_threaded_irq(irq, handler, thread_fn, irqflags, devname,	/* [한국어] 실제 요청. manage.c 의 본체를 부른다 */
				  dev_id);	/* [한국어] 핸들러에 넘길 문맥 */
	if (rc) {	/* [한국어] 요청 실패 */
		devres_free(dr);	/* [한국어] 잡아 둔 항목만 반납하면 끝이다. 인터럽트는 잡히지 않았다 */
		return rc;	/* [한국어] 오류 코드를 그대로 올린다. 로그는 호출자가 찍는다 */
	}

	dr->irq = irq;	/* [한국어] 해제에 필요한 정보를 채운다 */
	dr->dev_id = dev_id;	/* [한국어] 공유 인터럽트에서 어느 핸들러인지 가르는 값 */
	devres_add(dev, dr);	/* [한국어] 장치의 자원 목록에 매단다. 이 순간부터 detach 때 자동으로 해제된다 */

	return 0;	/* [한국어] 성공 */
}

/**
 * devm_request_threaded_irq - allocate an interrupt line for a managed device with error logging
 * @dev:	Device to request interrupt for
 * @irq:	Interrupt line to allocate
 * @handler:	Function to be called when the interrupt occurs
 * @thread_fn:	Function to be called in a threaded interrupt context. NULL
 *		for devices which handle everything in @handler
 * @irqflags:	Interrupt type flags
 * @devname:	An ascii name for the claiming device, dev_name(dev) if NULL
 * @dev_id:	A cookie passed back to the handler function
 *
 * Except for the extra @dev argument, this function takes the same
 * arguments and performs the same function as request_threaded_irq().
 * Interrupts requested with this function will be automatically freed on
 * driver detach.
 *
 * If an interrupt allocated with this function needs to be freed
 * separately, devm_free_irq() must be used.
 *
 * When the request fails, an error message is printed with contextual
 * information (device name, interrupt number, handler functions and
 * error code). Don't add extra error messages at the call sites.
 *
 * Return: 0 on success or a negative error number.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * devm_request_threaded_irq - 장치 수명에 묶인 인터럽트 요청 (가장 널리 쓰인다)
 *
 * 이 파일에서 드라이버가 가장 많이 쓰는 함수다. devm_request_irq() 라는
 * 이름으로도 보이는데, 그것은 thread_fn 을 NULL 로 넘기는 인라인 래퍼다.
 *
 * 하는 일은 두 줄뿐이다 — 실제 요청은 위 밑줄 판이 하고, 여기서는 실패
 * 로그만 더한다. 그 분리가 이 파일의 구조다.
 *
 * kernel-doc 의 세 가지 약속이 이 API 의 계약이다:
 *   detach 때 자동으로 해제된다.
 *   미리 해제하려면 devm_free_irq() 를 써야 한다(free_irq() 가 아니다 —
 *     그러면 devres 항목이 남아 detach 때 두 번 해제된다).
 *   실패 메시지는 이 함수가 찍으므로 호출부에서 또 찍지 말 것.
 *
 * 실행 컨텍스트: 드라이버 프로브, 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 프로브 → [이 함수] → __devm_request_threaded_irq()
 *     → request_threaded_irq() (kernel/irq/manage.c)
 */
int devm_request_threaded_irq(struct device *dev, unsigned int irq,
			      irq_handler_t handler, irq_handler_t thread_fn,
			      unsigned long irqflags, const char *devname,
			      void *dev_id)
{
	int rc = __devm_request_threaded_irq(dev, irq, handler, thread_fn,	/* [한국어] 실제 요청과 devres 등록은 밑줄 판에 맡긴다 */
					     irqflags, devname, dev_id);	/* [한국어] 인자를 그대로 전달한다 */

	return devm_request_result(dev, rc, irq, handler, thread_fn, devname);	/* [한국어] 실패면 일관된 형식으로 로그를 찍고, 성공이든 실패든 rc 를 그대로 돌려준다 */
}
EXPORT_SYMBOL(devm_request_threaded_irq);	/* [한국어] 모듈 드라이버가 쓴다. GPL 제한이 없는 것은 오래된 API 이기 때문이다 */

/*
 * [한국어]
 * __devm_request_any_context_irq - 문맥을 커널이 고르는 인터럽트 요청
 *
 * @dev:      요청하는 장치.
 * @irq:      요청할 인터럽트 번호.
 * @handler:  핸들러. 하드 인터럽트 문맥에서 돌 수도, 스레드에서 돌 수도 있다.
 * @irqflags: IRQF_* 조합.
 * @devname:  요청자 이름. NULL 이면 장치 이름.
 * @dev_id:   핸들러에 넘길 문맥.
 * @return:   성공하면 IRQC_IS_HARDIRQ(0) 또는 IRQC_IS_NESTED(1), 실패하면 음수.
 *
 * 위 threaded 판과 골격이 같고 부르는 함수만 다르다.
 *
 * any_context 가 무엇인가: 이 인터럽트가 하드 인터럽트 문맥에서 처리될지
 * 스레드에서 처리될지를 커널이 정하게 맡긴다. 중첩 스레드 인터럽트
 * (I2C 뒤의 GPIO 확장기 등)에 붙는 드라이버는 자기가 어느 문맥에서 돌지
 * 미리 알 수 없어, 커널의 판단을 따르는 것이다.
 *
 * 반환값이 0 이 아닐 수 있다는 점이 위 판과 다르다. 어느 문맥으로
 * 정해졌는지를 알려 주므로, 드라이버는 그에 맞춰 락 종류를 고를 수 있다.
 *
 * 실패 판정이 rc < 0 인 것도 그래서다. 위 판은 rc 하나로 판정했지만
 * 여기서는 양수가 성공이다.
 *
 * 실행 컨텍스트: 드라이버 프로브, 프로세스 문맥.
 *
 * 호출 체인:
 *   devm_request_any_context_irq() → [이 함수] → request_any_context_irq()
 */
static int __devm_request_any_context_irq(struct device *dev, unsigned int irq,
					  irq_handler_t handler,
					  unsigned long irqflags,
					  const char *devname, void *dev_id)
{
	struct irq_devres *dr;	/* [한국어] 해제 정보를 담을 devres 항목 */
	int rc;	/* [한국어] 요청 결과. 성공해도 0 이 아닐 수 있다 */

	dr = devres_alloc(devm_irq_release, sizeof(struct irq_devres),	/* [한국어] 해제 콜백은 위 판과 같은 것을 쓴다 — 해제 방법이 같기 때문이다 */
			  GFP_KERNEL);	/* [한국어] 프로세스 문맥의 할당 */
	if (!dr)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 되돌릴 것이 없다 */

	if (!devname)	/* [한국어] 이름이 없으면 */
		devname = dev_name(dev);	/* [한국어] 장치 이름으로 대신한다 */

	rc = request_any_context_irq(irq, handler, irqflags, devname, dev_id);	/* [한국어] 커널이 문맥을 골라 요청한다 */
	if (rc < 0) {	/* [한국어] 음수만 실패다 — 양수는 어느 문맥으로 정해졌는지를 알리는 성공 값이다 */
		devres_free(dr);	/* [한국어] 항목만 반납한다 */
		return rc;	/* [한국어] 오류 코드를 올린다 */
	}

	dr->irq = irq;	/* [한국어] 해제 정보를 채운다 */
	dr->dev_id = dev_id;	/* [한국어] 공유 인터럽트를 위한 식별자 */
	devres_add(dev, dr);	/* [한국어] 장치에 매단다 */

	return rc;	/* [한국어] 0 이 아니라 rc 를 돌려준다 — 어느 문맥으로 정해졌는지를 호출자가 알아야 한다 */
}

/**
 * devm_request_any_context_irq - allocate an interrupt line for a managed device with error logging
 * @dev:	Device to request interrupt for
 * @irq:	Interrupt line to allocate
 * @handler:	Function to be called when the interrupt occurs
 * @irqflags:	Interrupt type flags
 * @devname:	An ascii name for the claiming device, dev_name(dev) if NULL
 * @dev_id:	A cookie passed back to the handler function
 *
 * Except for the extra @dev argument, this function takes the same
 * arguments and performs the same function as request_any_context_irq().
 * Interrupts requested with this function will be automatically freed on
 * driver detach.
 *
 * If an interrupt allocated with this function needs to be freed
 * separately, devm_free_irq() must be used.
 *
 * When the request fails, an error message is printed with contextual
 * information (device name, interrupt number, handler functions and
 * error code). Don't add extra error messages at the call sites.
 *
 * Return: IRQC_IS_HARDIRQ or IRQC_IS_NESTED on success, or a negative error
 * number.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * devm_request_any_context_irq - 위 밑줄 판에 실패 로그를 더한 공개 진입점
 *
 * 위 devm_request_threaded_irq() 와 대칭인 구조다. 실제 일은 밑줄 판이
 * 하고 여기서는 로그만 더한다.
 *
 * devm_request_result 에 thread_fn 자리로 NULL 을 넘기는 것에 주의한다.
 * 이 API 는 스레드 핸들러를 따로 받지 않으므로 넘길 것이 없고, 로그에는
 * "(null)" 로 찍힌다. 형식을 공유하기 위한 타협이다.
 *
 * kernel-doc 의 반환값 설명이 위 판과 다르다 — IRQC_IS_HARDIRQ 또는
 * IRQC_IS_NESTED 를 돌려준다. 드라이버는 그 값으로 자기 핸들러가 어느
 * 문맥에서 돌지 알고, 그에 맞는 락을 고른다.
 *
 * 실행 컨텍스트: 드라이버 프로브, 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 프로브 → [이 함수] → __devm_request_any_context_irq()
 */
int devm_request_any_context_irq(struct device *dev, unsigned int irq,
				 irq_handler_t handler, unsigned long irqflags,
				 const char *devname, void *dev_id)
{
	int rc = __devm_request_any_context_irq(dev, irq, handler, irqflags,	/* [한국어] 실제 요청은 밑줄 판에 맡긴다 */
						devname, dev_id);	/* [한국어] 인자를 그대로 전달 */

	return devm_request_result(dev, rc, irq, handler, NULL, devname);	/* [한국어] 스레드 핸들러가 없으므로 그 자리에 NULL 을 넘긴다. 로그에는 "(null)" 로 찍힌다 */
}
EXPORT_SYMBOL(devm_request_any_context_irq);	/* [한국어] 모듈 드라이버에 공개 */

/**
 *	devm_free_irq - free an interrupt
 *	@dev: device to free interrupt for
 *	@irq: Interrupt line to free
 *	@dev_id: Device identity to free
 *
 *	Except for the extra @dev argument, this function takes the
 *	same arguments and performs the same function as free_irq().
 *	This function instead of free_irq() should be used to manually
 *	free IRQs allocated with devm_request_irq().
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * devm_free_irq - devm 으로 잡은 인터럽트를 detach 전에 미리 해제한다
 *
 * @dev:    그 장치.
 * @irq:    해제할 인터럽트 번호.
 * @dev_id: 요청 때 준 식별자.
 *
 * 언제 필요한가: devm 의 요점은 해제를 잊어도 되게 하는 것이라, 보통은
 * 이 함수를 쓸 일이 없다. 다만 장치가 살아 있는 동안 인터럽트만 떼야 하는
 * 경우가 있다 — 예를 들어 펌웨어를 다시 올리려고 잠시 인터럽트를 꺼야 할 때다.
 *
 * free_irq() 를 직접 부르면 안 되는 이유가 중요하다. 그러면 인터럽트는
 * 해제되지만 devres 항목은 남는다. 나중에 detach 할 때 그 항목이 다시
 * free_irq() 를 불러, 이미 해제된 인터럽트를 또 해제하려 든다.
 *
 * WARN_ON 이 감싸는 이유: devres_release() 는 항목을 못 찾으면 -ENOENT 를
 * 돌려준다. 그것은 곧 이 인터럽트가 devm 으로 잡힌 것이 아니거나 이미
 * 해제되었다는 뜻이라, 드라이버의 논리에 문제가 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 안에서 free_irq() 를 부르므로 잠들 수 있다.
 *
 * 호출 체인:
 *   드라이버 → [이 함수] → devres_release() → devm_irq_release() → free_irq()
 */
void devm_free_irq(struct device *dev, unsigned int irq, void *dev_id)
{
	struct irq_devres match_data = { irq, dev_id };	/* [한국어] 찾을 조건. 지정 초기화가 아니라 순서 초기화라, 구조체의 필드 순서에 의존한다 */

	WARN_ON(devres_release(dev, devm_irq_release, devm_irq_match,	/* [한국어] 항목을 찾아 해제 콜백을 부르고 목록에서 뺀다 */
			       &match_data));	/* [한국어] 못 찾으면 -ENOENT 를 돌려주고, WARN 이 그것을 잡는다 — devm 으로 잡지 않았거나 이미 해제된 것이다 */
}
EXPORT_SYMBOL(devm_free_irq);	/* [한국어] 모듈 드라이버에 공개 */

/* [한국어] 해제할 서술자 범위를 기억하는 devres 항목.
 *
 * 위 irq_devres 와 달리 "번호 하나" 가 아니라 "번호 범위" 를 담는다.
 * 서술자 할당은 연속된 여러 개를 한 번에 잡기 때문이다 — MSI-X 벡터를
 * 여러 개 쓰는 장치가 대표적이다. */
struct irq_desc_devres {
	unsigned int from;
	/* [한국어] 할당된 범위의 첫 인터럽트 번호.
	 * 설정자: __devm_irq_alloc_descs() 가 할당에 성공한 뒤, 실제로 배정된
	 *   번호를 채운다. 요청한 번호와 다를 수 있다.
	 * 읽는 자: devm_irq_desc_release() 가 irq_free_descs() 에 넘긴다.
	 * 값 범위: 유효한 리눅스 인터럽트 번호. */

	unsigned int cnt;
	/* [한국어] 그 범위에 든 서술자의 개수.
	 * 설정자: 요청 때 준 개수를 그대로 담는다.
	 * 읽는 자: 위 from 과 함께 해제에 쓴다.
	 * 값 범위: 1 이상. 할당이 성공했다면 요청한 만큼 전부 받은 것이다 —
	 *   부분 할당은 일어나지 않는다. */
};

/*
 * [한국어]
 * devm_irq_desc_release - 장치가 떨어져 나갈 때 서술자 범위를 반납한다
 *
 * @dev: 그 장치. 쓰이지 않는다.
 * @res: 위 struct irq_desc_devres 항목.
 *
 * 위 devm_irq_release() 와 같은 자리의 함수인데, 이쪽은 인터럽트 요청이
 * 아니라 서술자 자체를 반납한다.
 *
 * 둘의 차이: 서술자는 "인터럽트 번호가 존재한다" 는 등록이고, 요청은
 * "그 번호에 핸들러를 붙인다" 는 것이다. 컨트롤러 드라이버가 전자를,
 * 장치 드라이버가 후자를 한다.
 *
 * 실행 컨텍스트: 장치 detach, 프로세스 문맥.
 *
 * 호출 체인:
 *   device_release_driver() → devres_release_all() → [이 함수]
 *     → irq_free_descs() (kernel/irq/irqdesc.c)
 */
static void devm_irq_desc_release(struct device *dev, void *res)
{
	struct irq_desc_devres *this = res;	/* [한국어] void * 를 원래 타입으로 되돌린다 */

	irq_free_descs(this->from, this->cnt);	/* [한국어] 범위 전체를 한 번에 반납한다 */
}

/**
 * __devm_irq_alloc_descs - Allocate and initialize a range of irq descriptors
 *			    for a managed device
 * @dev:	Device to allocate the descriptors for
 * @irq:	Allocate for specific irq number if irq >= 0
 * @from:	Start the search from this irq number
 * @cnt:	Number of consecutive irqs to allocate
 * @node:	Preferred node on which the irq descriptor should be allocated
 * @owner:	Owning module (can be NULL)
 * @affinity:	Optional pointer to an irq_affinity_desc array of size @cnt
 *		which hints where the irq descriptors should be allocated
 *		and which default affinities to use
 *
 * Returns the first irq number or error code.
 *
 * Note: Use the provided wrappers (devm_irq_alloc_desc*) for simplicity.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * __devm_irq_alloc_descs - 서술자 범위를 잡고 장치 수명에 매단다
 *
 * @irq:      0 이상이면 그 번호를 정확히 요구한다. 음수면 커널이 고른다.
 * @from:     커널이 고를 때 탐색을 시작할 번호.
 * @cnt:      연속으로 잡을 개수.
 * @node:     서술자 메모리를 잡을 NUMA 노드.
 * @owner:    이 서술자들을 소유한 모듈. 모듈이 언로드될 때 참조 검사에 쓴다.
 * @affinity: 각 서술자의 기본 친화도 힌트. affinity.c 가 만든 배열을 넘긴다.
 * @return:   성공하면 첫 인터럽트 번호(0 이상), 실패하면 음수.
 *
 * 인터럽트 컨트롤러 드라이버가 자기가 담당할 번호들을 등록할 때 쓴다.
 * 앞선 함수들이 "번호에 핸들러를 붙이는" 것이라면, 이쪽은 "번호가
 * 존재하게 만드는" 것이다.
 *
 * kernel-doc 의 Note 가 권하는 래퍼들(devm_irq_alloc_desc,
 * devm_irq_alloc_descs 등)은 이 함수의 인자 일부를 고정한 인라인이다.
 * 인자가 일곱 개나 되어 직접 부르면 읽기 어렵기 때문이다.
 *
 * 반환값이 0 일 수 있다는 점이 앞선 함수들과 다르다. 0 번 인터럽트가
 * 배정되면 성공인데도 0 이 나오므로, 호출자는 반드시 음수만 실패로 봐야 한다.
 *
 * 실행 컨텍스트: 컨트롤러 드라이버 프로브, 프로세스 문맥.
 *
 * 호출 체인:
 *   컨트롤러 드라이버 → devm_irq_alloc_descs() 래퍼 → [이 함수]
 *     → __irq_alloc_descs() (kernel/irq/irqdesc.c)
 */
int __devm_irq_alloc_descs(struct device *dev, int irq, unsigned int from,
			   unsigned int cnt, int node, struct module *owner,
			   const struct irq_affinity_desc *affinity)
{
	struct irq_desc_devres *dr;	/* [한국어] 해제할 범위를 담을 devres 항목 */
	int base;	/* [한국어] 실제로 배정된 첫 번호. 요청한 것과 다를 수 있다 */

	dr = devres_alloc(devm_irq_desc_release, sizeof(*dr), GFP_KERNEL);	/* [한국어] 이 파일의 관례대로 항목을 먼저 잡는다 */
	if (!dr)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 아직 서술자를 잡지 않았으므로 되돌릴 것이 없다 */

	base = __irq_alloc_descs(irq, from, cnt, node, owner, affinity);	/* [한국어] 실제 할당. irqdesc.c 가 번호 공간에서 연속된 자리를 찾아 서술자를 만든다 */
	if (base < 0) {	/* [한국어] 음수만 실패다 — 0 번이 배정되어도 성공이다 */
		devres_free(dr);	/* [한국어] 항목만 반납 */
		return base;	/* [한국어] 오류 코드를 그대로 올린다. 이 API 는 로그를 찍지 않는다 */
	}

	dr->from = base;	/* [한국어] 요청한 번호가 아니라 실제 배정된 번호를 담는다 — 해제할 때 이것이 기준이다 */
	dr->cnt = cnt;	/* [한국어] 개수는 요청한 그대로다. 부분 할당은 일어나지 않는다 */
	devres_add(dev, dr);	/* [한국어] 장치에 매단다 */

	return base;	/* [한국어] 첫 번호를 돌려준다. 호출자는 여기서부터 cnt 개를 자기 것으로 쓴다 */
}
EXPORT_SYMBOL_GPL(__devm_irq_alloc_descs);	/* [한국어] 컨트롤러 드라이버가 모듈일 수 있어 공개한다. GPL 제한이 붙은 것은 비교적 최근 API 이기 때문이다 */

/* [한국어] generic irq chip 의 devm 판.
 *
 * generic chip 이 무엇인가: 많은 SoC 의 인터럽트 컨트롤러가 "마스크
 * 레지스터 하나, 상태 레지스터 하나" 같은 비슷한 구조를 갖는다. 그 공통
 * 패턴을 미리 구현해 두고 레지스터 오프셋만 채우면 되게 만든 것이
 * generic-chip.c 이며, 이 구간은 그것을 devm 으로 감싼다.
 *
 * 그 기능을 뺀 커널에서는 이 구간 전체가 컴파일되지 않는다. */
#ifdef CONFIG_GENERIC_IRQ_CHIP	/* [한국어] generic chip 기반을 켠 빌드 */
/**
 * devm_irq_alloc_generic_chip - Allocate and initialize a generic chip
 *                               for a managed device
 * @dev:	Device to allocate the generic chip for
 * @name:	Name of the irq chip
 * @num_ct:	Number of irq_chip_type instances associated with this
 * @irq_base:	Interrupt base nr for this chip
 * @reg_base:	Register base address (virtual)
 * @handler:	Default flow handler associated with this chip
 *
 * Returns an initialized irq_chip_generic structure. The chip defaults
 * to the primary (index 0) irq_chip_type and @handler
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * devm_irq_alloc_generic_chip - generic chip 을 잡고 초기화한다
 *
 * @dev:      소유할 장치.
 * @name:     이 chip 의 이름. /proc/interrupts 에 나타난다.
 * @num_ct:   딸린 irq_chip_type 의 개수. 아래 struct_size 계산에 쓴다.
 * @irq_base: 이 chip 이 담당할 첫 인터럽트 번호.
 * @reg_base: 레지스터 창의 가상 주소.
 * @handler:  기본 흐름 제어 핸들러.
 * @return:   초기화된 구조체, 또는 할당 실패 시 NULL.
 *
 * 이 파일의 다른 함수들과 골격이 다르다는 점이 눈에 띈다. devres 항목을
 * 따로 만들지 않고 devm_kzalloc() 하나로 끝낸다.
 *
 * 왜 그런가: 이 함수가 잡는 것은 메모리뿐이고, 해제도 그 메모리를 반납하는
 * 것으로 끝난다. devm_kzalloc 이 이미 그 일을 해 주므로 별도의 해제 콜백이
 * 필요 없다. 반면 아래 devm_irq_setup_generic_chip() 은 하드웨어 설정까지
 * 하므로 되돌릴 것이 있어 devres 항목을 쓴다.
 *
 * struct_size 를 쓰는 이유: irq_chip_type 배열이 구조체 끝에 가변 길이로
 * 붙어 있다(flexible array member). 그 크기를 손으로 계산하면 오버플로가
 * 날 수 있어, 커널이 제공하는 안전한 계산 매크로를 쓴다.
 *
 * 실행 컨텍스트: 컨트롤러 드라이버 프로브, 프로세스 문맥.
 *
 * 호출 체인:
 *   컨트롤러 드라이버 → [이 함수] → irq_init_generic_chip() (generic-chip.c)
 */
struct irq_chip_generic *
devm_irq_alloc_generic_chip(struct device *dev, const char *name, int num_ct,
			    unsigned int irq_base, void __iomem *reg_base,
			    irq_flow_handler_t handler)
{
	struct irq_chip_generic *gc;	/* [한국어] 잡을 구조체 */

	gc = devm_kzalloc(dev, struct_size(gc, chip_types, num_ct), GFP_KERNEL);	/* [한국어] 가변 길이 배열까지 포함한 크기를 안전하게 계산해 잡는다. devm_ 판이라 해제 콜백이 필요 없다 */
	if (gc)	/* [한국어] 할당에 성공했을 때만 초기화한다 */
		irq_init_generic_chip(gc, name, num_ct,	/* [한국어] 이름과 chip_type 개수를 채운다 */
				      irq_base, reg_base, handler);	/* [한국어] 담당 번호 범위와 레지스터 창, 기본 흐름 제어를 채운다 */

	return gc;	/* [한국어] 실패하면 NULL 이 그대로 나간다. 호출자가 확인해야 한다 */
}
EXPORT_SYMBOL_GPL(devm_irq_alloc_generic_chip);	/* [한국어] 컨트롤러 드라이버에 공개 */

/* [한국어] generic chip 설정을 되돌리기 위해 기억해 둘 값들.
 *
 * 위 alloc 판과 달리 이쪽은 devres 항목이 필요하다. 설정이 메모리 할당이
 * 아니라 하드웨어와 서술자 상태를 바꾸는 일이라, 되돌리려면 그때 쓴 값들을
 * 알아야 하기 때문이다.
 *
 * 네 필드가 그대로 irq_remove_generic_chip() 의 인자가 된다. */
struct irq_generic_chip_devres {
	struct irq_chip_generic *gc;
	/* [한국어] 설정한 generic chip.
	 * 설정자: devm_irq_setup_generic_chip() 이 설정에 성공한 뒤 채운다.
	 * 읽는 자: devm_irq_remove_generic_chip() 이 걷어 낼 때.
	 * 값 범위: NULL 이 아니다. 위 alloc 판이 돌려준 포인터다.
	 * 수명 주의: 이 chip 자체도 devm_kzalloc 으로 잡혔다면 같은 장치의
	 *   devres 목록에 있다. 목록이 역순으로 해제되므로, 나중에 등록된
	 *   이 항목이 먼저 실행되어 chip 이 살아 있는 동안 걷어 낸다. */

	u32 msk;
	/* [한국어] 어느 인터럽트들을 설정했는지 나타내는 비트마스크.
	 * 설정자: 설정 때 받은 값 그대로.
	 * 읽는 자: 걷어 낼 때 같은 값을 넘겨 정확히 그것들만 되돌린다.
	 * 값 범위: gc->irq_base 기준의 상대 비트. u32 라 한 chip 이 최대
	 *   32개의 인터럽트를 담당한다 — 아래 kernel-doc 의 "max. 32" 가 그것이다. */

	unsigned int clr;
	/* [한국어] 설정 때 지웠던 IRQ_* 설정 비트들.
	 * 설정자: 설정 때 받은 값 그대로.
	 * 읽는 자: 걷어 낼 때 그대로 넘긴다.
	 * 왜 기억하는가: 되돌리기가 대칭이어야 서술자가 원래 상태로 돌아간다.
	 *   무엇을 지웠는지 모르면 무엇을 되살려야 할지도 모른다. */

	unsigned int set;
	/* [한국어] 설정 때 세웠던 IRQ_* 설정 비트들.
	 * 설정자/읽는 자: 위 clr 과 같다.
	 * 위 clr 과 짝을 이룬다. 둘을 함께 기억해야 되돌리기가 완전해진다. */
};

/*
 * [한국어]
 * devm_irq_remove_generic_chip - 장치가 떨어져 나갈 때 generic chip 설정을 걷어 낸다
 *
 * @dev: 그 장치. 쓰이지 않는다.
 * @res: 위 struct irq_generic_chip_devres 항목.
 *
 * 이 파일의 다른 해제 콜백들과 같은 자리의 함수다. 기억해 둔 네 값을
 * 그대로 넘겨 설정을 되돌린다.
 *
 * 걷어 낸다는 것이 무슨 뜻인가: 그 인터럽트들의 chip 연결을 끊고, 흐름
 * 제어를 handle_bad_irq 로 되돌리고, 설정 비트를 원래대로 맞춘다. 그
 * 뒤로 그 번호에 인터럽트가 오면 잘못된 인터럽트로 처리된다.
 *
 * 실행 컨텍스트: 장치 detach, 프로세스 문맥.
 *
 * 호출 체인:
 *   device_release_driver() → devres_release_all() → [이 함수]
 *     → irq_remove_generic_chip() (kernel/irq/generic-chip.c)
 */
static void devm_irq_remove_generic_chip(struct device *dev, void *res)
{
	struct irq_generic_chip_devres *this = res;	/* [한국어] void * 를 원래 타입으로 */

	irq_remove_generic_chip(this->gc, this->msk, this->clr, this->set);	/* [한국어] 설정 때와 정확히 같은 인자로 되돌린다 */
}

/**
 * devm_irq_setup_generic_chip - Setup a range of interrupts with a generic
 *                               chip for a managed device
 *
 * @dev:	Device to setup the generic chip for
 * @gc:		Generic irq chip holding all data
 * @msk:	Bitmask holding the irqs to initialize relative to gc->irq_base
 * @flags:	Flags for initialization
 * @clr:	IRQ_* bits to clear
 * @set:	IRQ_* bits to set
 *
 * Set up max. 32 interrupts starting from gc->irq_base. Note, this
 * initializes all interrupts to the primary irq_chip_type and its
 * associated handler.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * devm_irq_setup_generic_chip - 인터럽트 범위에 generic chip 을 연결한다
 *
 * @dev:   소유할 장치.
 * @gc:    위 alloc 판이 만든 chip.
 * @msk:   gc->irq_base 기준으로 어느 인터럽트들을 설정할지.
 * @flags: 초기화 방식을 정하는 IRQ_GC_* 플래그.
 * @clr:   각 서술자에서 지울 IRQ_* 설정 비트.
 * @set:   세울 IRQ_* 설정 비트.
 * @return: 항상 0, 또는 devres 할당 실패 시 -ENOMEM.
 *
 * 이 파일의 표준 골격을 따르되 한 가지가 다르다 — 실제 설정 함수인
 * irq_setup_generic_chip() 이 void 라 실패할 수 없다. 그래서 실패 처리
 * 분기가 devres 할당 하나뿐이고, 설정 뒤에는 무조건 성공이다.
 *
 * kernel-doc 의 "max. 32" 는 msk 가 u32 이기 때문이다. 한 generic chip 이
 * 32개를 넘는 인터럽트를 담당하려면 chip 을 여러 개 두어야 한다.
 *
 * 그리고 kernel-doc 의 Note 가 중요하다: 모든 인터럽트가 첫 번째
 * irq_chip_type(index 0)과 그 핸들러로 초기화된다. 여러 chip_type 을
 * 쓰는 하드웨어라면 이 뒤에 각각을 다시 설정해야 한다.
 *
 * 실행 컨텍스트: 컨트롤러 드라이버 프로브, 프로세스 문맥.
 *
 * 호출 체인:
 *   컨트롤러 드라이버 → [이 함수] → irq_setup_generic_chip() (generic-chip.c)
 */
int devm_irq_setup_generic_chip(struct device *dev, struct irq_chip_generic *gc,
				u32 msk, enum irq_gc_flags flags,
				unsigned int clr, unsigned int set)
{
	struct irq_generic_chip_devres *dr;	/* [한국어] 되돌리기 정보를 담을 항목 */

	dr = devres_alloc(devm_irq_remove_generic_chip,	/* [한국어] 관례대로 항목을 먼저 잡는다 */
			  sizeof(*dr), GFP_KERNEL);	/* [한국어] 프로세스 문맥의 할당 */
	if (!dr)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 아직 설정하지 않았으므로 되돌릴 것이 없다 */

	irq_setup_generic_chip(gc, msk, flags, clr, set);	/* [한국어] 실제 설정. void 라 실패할 수 없어 아래에 실패 분기가 없다 */

	dr->gc = gc;	/* [한국어] 되돌릴 때 필요한 네 값을 그대로 기억한다 */
	dr->msk = msk;	/* [한국어] 어느 인터럽트들이었는지 */
	dr->clr = clr;	/* [한국어] 무엇을 지웠는지 */
	dr->set = set;	/* [한국어] 무엇을 세웠는지 */
	devres_add(dev, dr);	/* [한국어] 장치에 매단다 */

	return 0;	/* [한국어] 설정 자체는 실패할 수 없으므로 여기 도달하면 성공이다 */
}
EXPORT_SYMBOL_GPL(devm_irq_setup_generic_chip);	/* [한국어] 컨트롤러 드라이버에 공개 */
#endif /* CONFIG_GENERIC_IRQ_CHIP */

/* [한국어] 인터럽트 도메인의 devm 판.
 *
 * 도메인이 무엇인가: 하드웨어 인터럽트 번호(hwirq)를 리눅스 인터럽트
 * 번호(virq)로 옮기는 대응표다. 컨트롤러마다 자기 hwirq 공간이 있고
 * 그것들이 겹치므로, 커널 전역에서 유일한 번호로 옮겨야 한다.
 *
 * 도메인을 쓰지 않는 빌드에서는 이 구간이 컴파일되지 않는다. */
#ifdef CONFIG_IRQ_DOMAIN	/* [한국어] 도메인을 쓰는 빌드 */
/*
 * [한국어]
 * devm_irq_domain_remove - 장치가 떨어져 나갈 때 도메인을 없앤다
 *
 * @dev: 그 장치. 쓰이지 않는다.
 * @res: 도메인 포인터를 담은 devres 항목.
 *
 * 이 파일의 다른 해제 콜백들과 달리 항목이 구조체가 아니라 포인터
 * 하나다. 도메인을 없애는 데 필요한 것이 그것뿐이기 때문이다.
 *
 * 그래서 res 를 struct irq_domain ** 로 해석한다 — devres 항목의 내용이
 * "도메인 포인터" 이고, res 는 그 항목을 가리키므로 이중 포인터가 된다.
 *
 * 실행 컨텍스트: 장치 detach, 프로세스 문맥.
 *
 * 호출 체인:
 *   device_release_driver() → devres_release_all() → [이 함수]
 *     → irq_domain_remove() (kernel/irq/irqdomain.c)
 */
static void devm_irq_domain_remove(struct device *dev, void *res)
{
	struct irq_domain **domain = res;	/* [한국어] 항목의 내용이 포인터 하나라 이중 포인터가 된다 */

	irq_domain_remove(*domain);	/* [한국어] 도메인을 없앤다. 매핑된 인터럽트가 남아 있으면 그쪽에서 경고한다 */
}

/**
 * devm_irq_domain_instantiate() - Instantiate a new irq domain data for a
 *                                 managed device.
 * @dev:	Device to instantiate the domain for
 * @info:	Domain information pointer pointing to the information for this
 *		domain
 *
 * Return: A pointer to the instantiated irq domain or an ERR_PTR value.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * devm_irq_domain_instantiate - 도메인을 만들고 장치 수명에 매단다
 *
 * @dev:    소유할 장치.
 * @info:   도메인 설정. 크기, 연산표, 부모 도메인 등이 들어 있다.
 * @return: 만들어진 도메인, 또는 ERR_PTR 로 감싼 오류.
 *
 * 이 파일의 다른 함수들과 오류 표현이 다르다. NULL 이나 음수가 아니라
 * ERR_PTR 을 쓴다 — 포인터를 돌려주면서 오류 코드도 전하기 위한 커널의
 * 관용구이며, 호출자는 IS_ERR() 로 검사해야 한다.
 *
 * 그래서 실패 판정이 !IS_ERR(domain) 의 부정으로 표현된다. 성공 경로를
 * if 의 참 쪽에 두어 읽기 좋게 만든 것이다.
 *
 * 골격 자체는 이 파일의 관례를 따른다 — 항목을 먼저 잡고, 실제 자원을
 * 잡고, 실패하면 항목만 반납한다.
 *
 * 실행 컨텍스트: 컨트롤러 드라이버 프로브, 프로세스 문맥.
 *
 * 호출 체인:
 *   컨트롤러 드라이버 → [이 함수] → irq_domain_instantiate() (irqdomain.c)
 */
struct irq_domain *devm_irq_domain_instantiate(struct device *dev,
					       const struct irq_domain_info *info)
{
	struct irq_domain *domain;	/* [한국어] 만들어진 도메인 */
	struct irq_domain **dr;	/* [한국어] devres 항목. 내용이 포인터 하나라 이중 포인터다 */

	dr = devres_alloc(devm_irq_domain_remove, sizeof(*dr), GFP_KERNEL);	/* [한국어] 관례대로 항목을 먼저 잡는다. 크기는 포인터 하나 */
	if (!dr)	/* [한국어] 할당 실패 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 포인터를 돌려주는 API 라 오류도 ERR_PTR 로 감싼다 */

	domain = irq_domain_instantiate(info);	/* [한국어] 실제 도메인 생성 */
	if (!IS_ERR(domain)) {	/* [한국어] 성공 경로를 참 쪽에 두어 읽기 좋게 했다 */
		*dr = domain;	/* [한국어] 항목에 도메인 포인터를 담는다 */
		devres_add(dev, dr);	/* [한국어] 장치에 매단다. 이제 detach 때 자동으로 없어진다 */
	} else {
		devres_free(dr);	/* [한국어] 실패했으므로 항목만 반납한다 */
	}

	return domain;	/* [한국어] 성공이면 도메인, 실패면 ERR_PTR. 호출자가 IS_ERR 로 검사해야 한다 */
}
EXPORT_SYMBOL_GPL(devm_irq_domain_instantiate);	/* [한국어] 컨트롤러 드라이버에 공개 */
#endif /* CONFIG_IRQ_DOMAIN */
