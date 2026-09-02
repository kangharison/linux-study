// SPDX-License-Identifier: GPL-2.0
/*
 * Sophgo DesignWare based PCIe host controller driver
 */

/*
 * [한국어 설명] Sophgo SG2044 의 DesignWare PCIe 호스트 글루 (pcie-sophgo.c)
 *
 * === 파일의 역할 ===
 * Synopsys DesignWare PCIe 코어를 Sophgo SG2044 SoC 에 붙이는 얇은 글루
 * 드라이버다. config 접근, 링크 훈련, ATU 설정, MSI 도메인 같은 무거운 일은
 * 모두 pcie-designware-host.c 가 하고, 이 파일은 **DWC 코어가 다루지 못하는
 * SoC 고유의 것** 만 맡는다. 그것이 셋이다.
 *
 *   1) app 레지스터 창 — DWC 표준이 아닌 SoC 전용 레지스터 두 개
 *      (PCIE_INT_SIGNAL, PCIE_INT_EN)로 인터럽트를 보고 제어한다.
 *   2) INTx 도메인 — 이 SoC 는 INTx 넷을 요약 인터럽트 하나로 모아 올린다.
 *      그것을 넷으로 갈라 주는 도메인을 이 파일이 직접 만든다.
 *   3) ASPM L0s/L1 비활성화 — 링크 capability 레지스터에서 그 두 비트를
 *      지워, 소프트웨어가 ASPM 을 켜려 해도 켜지지 않게 한다.
 *
 * 이 파일에 없는 것이 있다는 점이 중요하다 — PHY 설정도, 리셋 시퀀스도,
 * 링크 대기도 없다. 그 하드웨어들이 이 SoC 에서는 펌웨어나 다른 드라이버가
 * 맡거나 필요 없다는 뜻이며, 그래서 이 파일이 275줄로 끝난다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   플랫폼 드라이버 코어 -> sophgo_pcie_probe()
 *     -> app 레지스터 창 매핑, 클럭 켜기
 *     -> dw_pcie_host_init()  [pcie-designware-host.c]
 *        -> 그 안에서 콜백 -> [이 파일] sophgo_pcie_host_init()
 *           -> INTx 도메인 생성 + 체인 핸들러 등록
 *           -> ASPM 비활성화
 *           -> MSI 인터럽트 허용
 *        -> DWC 코어가 이어서 링크·ATU·버스 스캔을 진행
 *
 * INTx 인터럽트가 올라오는 방향:
 *   장치가 INTx 어서션 -> SoC 가 요약 인터럽트 하나를 올림
 *     -> [이 파일] sophgo_pcie_intx_handler() (체인 핸들러)
 *        -> PCIE_INT_SIGNAL 을 읽어 어느 INTx 인지 가림
 *        -> generic_handle_domain_irq() -> 장치 드라이버의 핸들러
 *
 * MSI 는 이 파일을 거치지 않는다. DWC 코어가 자기 MSI 컨트롤러를 다루고,
 * 이 파일은 sophgo_pcie_msi_enable() 로 app 레지스터의 허용 비트 하나를
 * 켜 주기만 한다.
 *
 * 실행 컨텍스트: probe 와 host_init 은 프로세스 컨텍스트,
 * sophgo_pcie_intx_handler() 는 인터럽트 문맥, 마스크·언마스크는 인터럽트
 * 문맥일 수 있다. 그래서 그 셋이 공유하는 PCIE_INT_EN 레지스터를
 * raw_spin_lock_irqsave 로 지킨다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어가 dw_pcie_host_init() 을 거쳐 버스를 스캔한다.
 * 아래쪽: pcie-designware-host.c 와 pcie-designware.c 가 실제 컨트롤러
 *   동작을 맡는다. 접점은 두 가지다 — struct dw_pcie 를 이 파일의 구조체
 *   **맨 앞에** 두어 container_of 없이 변환되게 하는 것과,
 *   sophgo_pcie_host_ops 의 init 콜백 하나다.
 * 옆쪽: irqdomain·irqchip 코어. INTx 도메인이 그 위에 얹힌다.
 *
 * 데이터 흐름:
 *   디바이스 트리("app" 자원, 클럭, interrupt-controller 자식 노드)
 *     -> probe -> struct sophgo_pcie
 *   인터럽트: PCIE_INT_SIGNAL 의 비트 -> hwirq 번호 -> 도메인 -> 핸들러
 *   제어:     hwirq 번호 -> PCIE_INT_EN 의 비트
 *   두 레지스터가 **같은 INTx 를 서로 다른 비트 위치** 로 나타낸다는 점이
 *   이 파일에서 가장 헷갈리기 쉬운 대목이다(각 매크로 자리에 적어 두었다).
 *
 * 공유 상태: struct sophgo_pcie 하나. PCIE_INT_EN 레지스터만 잠금이 필요하고
 *   나머지는 probe 후 불변이다. 그 잠금으로 DWC 코어의 pp->lock 을 빌려 쓴다.
 *
 * === NVMe 관점 ===
 * 이 브리지 아래에 NVMe 컨트롤러가 붙으면 그 인터럽트는 MSI/MSI-X 로
 * 가므로 이 파일의 INTx 경로를 지나지 않는다. 이 파일이 관여하는 것은
 * sophgo_pcie_msi_enable() 이 app 레지스터의 MSI 허용 비트를 켜 주는
 * 한 지점뿐이며, 그것이 없으면 DWC 의 MSI 컨트롤러가 아무리 준비돼 있어도
 * 인터럽트가 CPU 까지 오지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * sophgo_pcie_host_init()      : DWC 코어가 부르는 유일한 콜백. 이 파일이
 *                                하는 일 셋이 여기 모여 있다.
 * sophgo_pcie_intx_handler()   : 요약 인터럽트를 INTx 넷으로 갈라 보낸다.
 * sophgo_intx_irq_mask/unmask(): PCIE_INT_EN 의 해당 비트를 끄고 켠다.
 * sophgo_pcie_init_irq_domain(): 디바이스 트리의 자식 노드에서 INTx 도메인을
 *                                만들고 그 요약 인터럽트 번호를 돌려준다.
 * sophgo_pcie_disable_l0s_l1() : 링크 capability 에서 ASPM 비트를 지운다.
 * struct sophgo_pcie           : dw_pcie 를 맨 앞에 둔 이 드라이버의 상태.
 */

/* [한국어] GENMASK 과 BIT. 아래 레지스터 필드 정의가 이것을 쓴다. */
#include <linux/bits.h>
/* [한국어] devm_clk_bulk_get_all_enabled() 와 struct clk_bulk_data. */
#include <linux/clk.h>
/* [한국어] chained_irq_enter()/exit(). 이 파일의 INTx 핸들러가 체인 핸들러다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] irq_domain_create_linear() 와 irq_domain_ops. INTx 도메인의 뼈대다. */
#include <linux/irqdomain.h>
/* [한국어] MODULE_ 매크로들. 다만 이 드라이버는 builtin 이라 모듈로 빠지지 않는다. */
#include <linux/module.h>
/* [한국어] device_get_named_child_node() 와 fwnode_irq_get(). 디바이스 트리의
 * 자식 노드에서 INTx 컨트롤러를 찾는 데 쓴다. */
#include <linux/property.h>
/* [한국어] struct platform_device 와 devm_platform_ioremap_resource_byname(). */
#include <linux/platform_device.h>

/* [한국어] DWC 코어의 선언 전부 — struct dw_pcie, dw_pcie_host_init(),
 * dw_pcie_readl_dbi() 등. 이 파일이 그 위에 얹히는 글루라는 표시다. */
#include "pcie-designware.h"

/* [한국어] dw_pcie 포인터에서 이 드라이버의 상태를 되찾는 통로.
 * container_of 가 아니라 drvdata 를 거치는 것이 눈에 띄는데, probe 가
 * 같은 포인터를 drvdata 에 매달아 두기 때문에 성립한다. 그래서 probe 의
 * platform_set_drvdata() 가 다른 초기화보다 먼저여야 한다. */
#define to_sophgo_pcie(x)		dev_get_drvdata((x)->dev)

/* [한국어] 인터럽트 신호 레지스터 — 지금 어느 인터럽트가 올라와 있는지. */
#define PCIE_INT_SIGNAL			0xc48
/* [한국어] 인터럽트 허용 레지스터 — 어느 인터럽트를 CPU 까지 올릴지. */
#define PCIE_INT_EN			0xca0

/* [한국어] 신호 레지스터에서 INTx 넷이 차지하는 비트 5~8. */
#define PCIE_INT_SIGNAL_INTX		GENMASK(8, 5)

/* [한국어] 허용 레지스터에서 **같은 INTx 넷** 이 차지하는 비트 1~4.
 * 신호 쪽(5~8)과 위치가 달라, 이 파일에서 같은 INTx 가 세 가지 번호로
 * 나타난다 — hwirq 0~3, 신호 비트 5~8, 허용 비트 1~4.
 * 핸들러의 FIELD_GET 과 마스크·언마스크의 FIELD_PREP 이 그 세 번호 사이를 오간다. */
#define PCIE_INT_EN_INTX		GENMASK(4, 1)
/* [한국어] 허용 레지스터의 MSI 비트. 이 파일이 MSI 에 관여하는 유일한 자리다. */
#define PCIE_INT_EN_INT_MSI		BIT(5)

/* [한국어] 이 드라이버의 상태 전부. */
struct sophgo_pcie {
	/* [한국어] DWC 코어가 다루는 부분. **맨 앞에 두어** 이 구조체 포인터가 그대로
	 * struct dw_pcie 포인터로 쓰인다.
	 * 설정자: probe 가 dev 를 채우고, DWC 코어가 나머지를 채운다.
	 * 읽는 자: DWC 코어 전체와 이 파일의 to_dw_pcie_from_pp() 변환.
	 * 값 범위: DWC 코어가 정의한 구조체.
	 * 동기화: 코어가 관리한다. */
	struct dw_pcie		pci;
	/* [한국어] SoC 전용 app 레지스터 창의 가상 주소.
	 * 설정자: sophgo_pcie_resource_get() 의 devm_platform_ioremap_resource_byname("app").
	 * 읽는 자: sophgo_pcie_readl_app()/writel_app().
	 * 값 범위: 유효한 iomem 포인터. devres 가 관리한다.
	 * 동기화: probe 후 불변. 이 창 안의 PCIE_INT_EN 만 pp->lock 이 지킨다. */
	void __iomem		*app_base;
	/* [한국어] 이 컨트롤러가 쓰는 클럭들.
	 * 설정자: sophgo_pcie_clk_init() 의 devm_clk_bulk_get_all_enabled().
	 * 읽는 자: 이 파일에서 이 배열을 직접 훑는 곳은 없다 — devres 가 정리를 맡는다.
	 * 값 범위: 디바이스 트리에 적힌 클럭 수만큼. 개수가 트리에 달려 있어 코드에 고정할 수 없다.
	 * 동기화: probe 후 불변. */
	struct clk_bulk_data	*clks;
	/* [한국어] 그 클럭의 개수.
	 * 설정자: sophgo_pcie_clk_init() 이 _get_all_enabled 의 반환값을 담는다.
	 * 읽는 자: **없다.** 이 파일 전수 확인 결과 :210 의 대입 하나뿐이며,
	 * 원본(1f0e418bb6)에서도 같다. 코드는 고치지 않았다.
	 * 값 범위: 0 이상.
	 * 동기화: probe 후 불변. */
	unsigned int		clk_cnt;
	/* [한국어] 이 파일이 만든 INTx 도메인.
	 * 설정자: sophgo_pcie_init_irq_domain().
	 * 읽는 자: sophgo_pcie_intx_handler() 가 hwirq 로 핸들러를 찾는 데 쓴다.
	 * 값 범위: PCI_NUM_INTX(4) 크기의 선형 도메인.
	 * 동기화: probe 후 불변. */
	struct irq_domain	*irq_domain;
};

/* [한국어]
 * sophgo_pcie_readl_app - SoC 전용 app 레지스터를 읽는다
 *
 * @sophgo: 드라이버 상태.
 * @reg: app 창 안의 오프셋.
 * @return: 읽은 값.
 *
 * DWC 표준 레지스터(DBI)가 아니라 Sophgo 가 따로 둔 창을 읽는다. 인터럽트
 * 상태와 허용 비트가 그 창에 있다.
 *
 * 반환 타입이 int 인 것에 주의할 만하다. 읽는 값은 u32 이므로 최상위 비트가
 * 서 있으면 음수가 된다. 이 파일의 호출부는 모두 결과를 u32 나 unsigned long 에
 * 담고 비트만 보므로 실제 문제가 되지 않지만, 값을 그대로 비교하는 코드가
 * 생기면 부호 문제가 드러난다. 원본(1f0e418bb6)에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * _relaxed 판인 것은 이 읽기가 다른 메모리 접근과 순서를 맞출 필요가 없기
 * 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥과 프로세스 컨텍스트 양쪽. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   sophgo_pcie_intx_handler() / sophgo_intx_irq_mask() / _unmask()
 *   / sophgo_pcie_msi_enable() → [이 함수] → readl_relaxed()
 */
static int sophgo_pcie_readl_app(struct sophgo_pcie *sophgo, u32 reg)
{
	return readl_relaxed(sophgo->app_base + reg);
}

/* [한국어]
 * sophgo_pcie_writel_app - SoC 전용 app 레지스터에 쓴다
 *
 * @sophgo: 드라이버 상태.
 * @val: 쓸 값.
 * @reg: app 창 안의 오프셋.
 *
 * sophgo_pcie_readl_app() 의 짝이다.
 *
 * 이 파일에서 쓰는 곳은 PCIE_INT_EN 하나뿐이며, 언제나 읽기-수정-쓰기의
 * 마지막 단계다. 다른 인터럽트의 허용 비트를 보존해야 하기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥과 프로세스 컨텍스트 양쪽. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   sophgo_intx_irq_mask() / _unmask() / sophgo_pcie_msi_enable()
 *     → [이 함수] → writel_relaxed()
 */
static void sophgo_pcie_writel_app(struct sophgo_pcie *sophgo, u32 val, u32 reg)
{
	writel_relaxed(val, sophgo->app_base + reg);
}

/* [한국어]
 * sophgo_pcie_intx_handler - 요약 인터럽트를 INTx 넷으로 갈라 보낸다
 *
 * @desc: 이 체인 인터럽트의 서술자.
 *
 * 이 SoC 는 INTA~INTD 넷을 인터럽트 선 하나로 모아 올린다. 어느 것이었는지는
 * PCIE_INT_SIGNAL 의 비트가 알려 주므로, 그것을 읽어 갈라 보내는 것이 이
 * 함수의 일이다.
 *
 * 체인 핸들러라 자기 인터럽트를 직접 처리하지 않는다. chained_irq_enter/exit
 * 이 상위 컨트롤러의 마스킹과 EOI 를 대신한다.
 *
 * FIELD_GET 으로 비트를 **아래로 내리는** 것이 요점이다. 신호 레지스터에서
 * INTx 넷이 비트 5~8 에 있는데, 도메인의 hwirq 는 0~3 이어야 하므로 그
 * 위치 차이를 없애야 한다. 허용 레지스터 쪽은 같은 넷이 비트 1~4 에 있어
 * 또 다른 위치이며, 그 변환은 마스크·언마스크가 따로 한다.
 *
 * 인터럽트를 지우지 않는다. INTx 는 레벨 트리거라 장치가 신호를 내릴 때까지
 * 비트가 서 있고, 그것을 내리는 것은 장치 드라이버의 핸들러가 할 일이다.
 * 도메인의 흐름 처리기를 handle_level_irq 로 둔 것이 그 구조와 맞물린다.
 *
 * altera 판과 달리 바깥 while 루프가 없다. 레벨 트리거라 처리하지 못한
 * 인터럽트가 있으면 상위 컨트롤러가 다시 올려 주기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 잠들 수 없다.
 *
 * 에러 경로: 없다. generic_handle_domain_irq() 의 결과를 확인하지 않는다.
 *
 * 호출 체인:
 *   상위 인터럽트 컨트롤러 → [이 함수]
 *     → chained_irq_enter() → sophgo_pcie_readl_app(PCIE_INT_SIGNAL)
 *     → generic_handle_domain_irq() → chained_irq_exit()
 */
static void sophgo_pcie_intx_handler(struct irq_desc *desc)
{
	struct dw_pcie_rp *pp = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] DWC 문맥에서 그것을 품은 dw_pcie 를 되찾는다. */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] dw_pcie 가 이 구조체 맨 앞에 있어, drvdata 로 이 드라이버의 상태를 얻는다. */
	struct sophgo_pcie *sophgo = to_sophgo_pcie(pci);
	/* [한국어] hwirq 는 처리 중인 INTx 번호, reg 는 읽어 낸 신호 값이다.
	 * for_each_set_bit 이 unsigned long 을 요구해 둘 다 그 타입이다. */
	unsigned long hwirq, reg;

	chained_irq_enter(chip, desc);
/* [한국어] 요약 신호 레지스터를 읽는다. */

	reg = sophgo_pcie_readl_app(sophgo, PCIE_INT_SIGNAL);
	/* [한국어] INTx 넷이 비트 5~8 에 있는 것을 0~3 으로 **내린다.**
	 * 도메인의 hwirq 가 0 부터이기 때문이다. 허용 레지스터 쪽은 같은 넷이
	 * 비트 1~4 에 있어 또 다른 위치이며, 그 변환은 마스크·언마스크가 따로 한다. */
	reg = FIELD_GET(PCIE_INT_SIGNAL_INTX, reg);

	for_each_set_bit(hwirq, &reg, PCI_NUM_INTX)
		/* [한국어] 그 hwirq 에 등록된 장치 드라이버의 핸들러를 부른다.
		 * 결과를 확인하지 않는 것은 INTx 가 레벨 트리거라, 처리되지 않으면
		 * 상위 컨트롤러가 다시 올려 주기 때문이다. */
		generic_handle_domain_irq(sophgo->irq_domain, hwirq);

	chained_irq_exit(chip, desc);
}

/* [한국어]
 * sophgo_intx_irq_mask - INTx 하나를 막는다
 *
 * @d: 대상 인터럽트. hwirq 가 0~3 이다.
 *
 * PCIE_INT_EN 의 해당 비트를 지운다.
 *
 * FIELD_PREP 으로 비트를 **위로 올리는** 것이 핸들러의 FIELD_GET 과 대칭이다.
 * hwirq 0~3 을 허용 레지스터의 비트 1~4 자리로 옮긴다. 신호 레지스터의
 * 5~8 과는 또 다른 위치라, 이 파일에서 같은 INTx 가 세 가지 번호로 나타나는
 * 셈이다 — hwirq, 신호 비트, 허용 비트.
 *
 * 읽기-수정-쓰기라 다른 INTx 와 MSI 의 허용 비트가 보존된다.
 *
 * DWC 코어의 pp->lock 을 빌려 쓴다. 이 파일이 자기 잠금을 따로 두지 않는
 * 이유는 지켜야 할 것이 PCIE_INT_EN 하나뿐이고, 그것을 건드리는 세 함수가
 * 모두 이 잠금을 잡기 때문이다.
 *
 * raw_spin_lock_irqsave 인 것이 두 가지를 말해 준다 — 인터럽트 문맥에서
 * 잡을 수 있어야 하고(irqsave), PREEMPT_RT 에서도 잠들지 않는 진짜
 * 스핀락이어야 한다(raw).
 *
 * 실행 컨텍스트: 인터럽트 마스킹. 인터럽트 문맥일 수 있어 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 코어 → irq_chip.irq_mask == [이 함수]
 *     → sophgo_pcie_readl_app() → sophgo_pcie_writel_app()
 */
static void sophgo_intx_irq_mask(struct irq_data *d)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] drvdata 로 이 드라이버의 상태를 얻는다. */
	struct sophgo_pcie *sophgo = to_sophgo_pcie(pci);
	/* [한국어] 저장할 인터럽트 상태. */
	unsigned long flags;
	/* [한국어] 읽기-수정-쓰기 할 허용 값. */
	u32 val;

	raw_spin_lock_irqsave(&pp->lock, flags);
/* [한국어] 허용 레지스터를 읽는다. */

	val = sophgo_pcie_readl_app(sophgo, PCIE_INT_EN);
	/* [한국어] hwirq 0~3 을 허용 레지스터의 비트 1~4 자리로 **올려서** 지운다.
	 * 핸들러의 FIELD_GET 과 정확히 대칭인 변환이다. */
	val &= ~FIELD_PREP(PCIE_INT_EN_INTX, BIT(d->hwirq));
	/* [한국어] 되쓴다. 읽기-수정-쓰기라 다른 INTx 와 MSI 의 허용 비트가 보존된다. */
	sophgo_pcie_writel_app(sophgo, val, PCIE_INT_EN);

	raw_spin_unlock_irqrestore(&pp->lock, flags);
};

/* [한국어]
 * sophgo_intx_irq_unmask - INTx 하나를 다시 허용한다
 *
 * @d: 대상 인터럽트.
 *
 * sophgo_intx_irq_mask() 의 짝이며, 비트를 지우는 대신 세우는 것만 다르다.
 *
 * 잠금과 비트 위치 변환은 마스크 쪽과 같다.
 *
 * 실행 컨텍스트: 인터럽트 마스킹 해제. 인터럽트 문맥일 수 있어 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 코어 → irq_chip.irq_unmask == [이 함수]
 *     → sophgo_pcie_readl_app() → sophgo_pcie_writel_app()
 */
static void sophgo_intx_irq_unmask(struct irq_data *d)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] drvdata 로 상태를 얻는다. */
	struct sophgo_pcie *sophgo = to_sophgo_pcie(pci);
	/* [한국어] 저장할 인터럽트 상태. */
	unsigned long flags;
	/* [한국어] 읽기-수정-쓰기 할 허용 값. */
	u32 val;

	raw_spin_lock_irqsave(&pp->lock, flags);
/* [한국어] 허용 레지스터를 읽는다. */

	val = sophgo_pcie_readl_app(sophgo, PCIE_INT_EN);
	/* [한국어] 같은 자리 변환으로 비트를 세운다. */
	val |= FIELD_PREP(PCIE_INT_EN_INTX, BIT(d->hwirq));
	/* [한국어] 되쓴다. */
	sophgo_pcie_writel_app(sophgo, val, PCIE_INT_EN);

	raw_spin_unlock_irqrestore(&pp->lock, flags);
/* [한국어] 언마스크 끝. 세미콜론이 붙어 있으나 빈 문장이라 동작에는 영향이 없다. */
};

static struct irq_chip sophgo_intx_irq_chip = {
	/* [한국어] /proc/interrupts 에 나올 칩 이름. */
	.name			= "INTx",
	/* [한국어] 이 칩이 제공하는 것은 마스킹 한 쌍뿐이다. INTx 는 메시지가 없어
	 *  compose_msi_msg 가 필요 없고, EOI 도 레벨 트리거라 흐름 처리기가 맡는다. */
	.irq_mask		= sophgo_intx_irq_mask,
	.irq_unmask		= sophgo_intx_irq_unmask,
};

/* [한국어]
 * sophgo_pcie_intx_map - INTx 하나에 칩과 흐름 처리기를 붙인다
 *
 * @domain: INTx 도메인.
 * @irq: 배정된 가상 IRQ 번호.
 * @hwirq: 하드웨어 인터럽트 번호(0~3). 쓰지 않는다.
 * @return: 언제나 0.
 *
 * 도메인이 가상 IRQ 를 처음 만들 때 불려, 그 번호가 어떻게 동작할지를 정한다.
 *
 * handle_level_irq 를 고르는 것이 이 함수의 유일한 판단이다. INTx 가
 * 레벨 트리거이기 때문이며, 그 흐름 처리기는 핸들러를 부르기 전에 마스크하고
 * 끝난 뒤 언마스크한다 — 그것이 위 마스크·언마스크 콜백이 필요한 이유다.
 *
 * chip_data 로 dw_pcie_rp 를 넘긴다. 마스크·언마스크가 그것으로부터
 * 드라이버 상태를 되찾는다.
 *
 * 실행 컨텍스트: 인터럽트 매핑. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   irq_domain 코어 → irq_domain_ops.map == [이 함수]
 *     → irq_set_chip_and_handler() → irq_set_chip_data()
 */
static int sophgo_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &sophgo_intx_irq_chip, handle_level_irq);
	/* [한국어] chip_data 로 pp 를 넘긴다 — 마스크·언마스크가 그것으로부터
	 * 드라이버 상태를 되찾는다. */
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops intx_domain_ops = {
	/* [한국어] 매핑 콜백 하나뿐이다. 이 도메인은 선형이라 별도의 xlate 나 alloc 이 필요 없다. */
	.map = sophgo_pcie_intx_map,
};

/* [한국어]
 * sophgo_pcie_init_irq_domain - INTx 도메인을 만들고 요약 인터럽트 번호를 얻는다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 요약 인터럽트의 IRQ 번호, 또는 음수 오류.
 *
 * 반환값이 IRQ 번호라는 점이 이 함수의 특징이다. 도메인을 만드는 것과
 * 인터럽트 번호를 얻는 것을 함께 하는데, 둘 다 같은 디바이스 트리 자식
 * 노드에서 나오기 때문이다.
 *
 * 그 자식 노드(interrupt-controller)가 이 SoC 의 디바이스 트리 관용이다.
 * INTx 컨트롤러를 브리지 노드 안의 별도 노드로 서술하고, 그 노드가 요약
 * 인터럽트를 하나 갖는다.
 *
 * 노드 참조를 세 경로 모두에서 놓는 것에 주의할 만하다 — 인터럽트 번호를
 * 얻지 못한 경우, 도메인을 만든 경우, 그리고 도메인을 만들지 못한 경우.
 * 도메인 생성 뒤에 놓는 것은 irq_domain_create_linear() 가 필요하면 자기
 * 참조를 따로 올리기 때문이다.
 *
 * 선형 도메인을 쓰는 것은 hwirq 가 0~3 으로 조밀하기 때문이다. 배열 하나로
 * 매핑이 끝난다.
 *
 * host_data 로 pp 를 넘겨, 위 map 콜백이 그것을 chip_data 로 이어 준다.
 *
 * 실행 컨텍스트: host_init 콜백. 프로세스 컨텍스트.
 *
 * 에러 경로: 자식 노드가 없으면 -ENODEV, 인터럽트 번호를 못 얻으면 그 오류,
 * 도메인 생성 실패는 -EINVAL. 각각 기록을 남긴다.
 *
 * 호출 체인:
 *   sophgo_pcie_host_init() → [이 함수]
 *     → device_get_named_child_node() → fwnode_irq_get()
 *     → irq_domain_create_linear()
 */
static int sophgo_pcie_init_irq_domain(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct sophgo_pcie *sophgo = to_sophgo_pcie(pci);
	/* [한국어] 로그와 디바이스 트리 조회에 쓸 device. */
	struct device *dev = sophgo->pci.dev;
	/* [한국어] INTx 컨트롤러를 서술하는 자식 노드. */
	struct fwnode_handle *intc;
	/* [한국어] 그 노드가 갖는 요약 인터럽트의 번호. */
	int irq;

	intc = device_get_named_child_node(dev, "interrupt-controller");
	/* [한국어] 자식 노드가 없으면 — 이 SoC 의 디바이스 트리 관용을 따르지 않는 트리다. */
	if (!intc) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "missing child interrupt-controller node\n");
		/* [한국어] 장치 없음으로 답한다. */
		return -ENODEV;
	}

	irq = fwnode_irq_get(intc, 0);
	/* [한국어] 인터럽트 번호를 얻지 못하면, */
	if (irq < 0) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "failed to get INTx irq number\n");
		/* [한국어] 노드 참조를 놓은 뒤 — */
		fwnode_handle_put(intc);
		return irq;
	/* [한국어] 그 오류를 올려보낸다. */
	}

	sophgo->irq_domain = irq_domain_create_linear(intc, PCI_NUM_INTX,
						      /* [한국어] hwirq 가 0~3 으로 조밀해 선형 도메인이면 충분하다.
						       * host_data 로 pp 를 넘기면 map 콜백이 그것을 chip_data 로 이어 준다. */
						      &intx_domain_ops, pp);
	fwnode_handle_put(intc);
	if (!sophgo->irq_domain) {
		/* [한국어] 도메인을 만들지 못했으면 그 사실을 남기고, */
		dev_err(dev, "failed to get a INTx irq domain\n");
		/* [한국어] 잘못된 인자로 답한다. 여기서는 노드 참조를 이미 놓은 뒤다. */
		return -EINVAL;
	}

	return irq;
}

/* [한국어]
 * sophgo_pcie_msi_enable - app 레지스터에서 MSI 인터럽트를 허용한다
 *
 * @pp: DWC 의 루트 포트 문맥.
 *
 * MSI 자체는 DWC 코어가 다룬다. 이 함수는 그 인터럽트가 CPU 까지 올라오도록
 * SoC 쪽 관문을 여는 한 비트만 켠다.
 *
 * 이 한 줄이 없으면 DWC 의 MSI 컨트롤러가 아무리 준비돼 있어도 인터럽트가
 * 오지 않는다 — 이 파일이 MSI 에 관여하는 유일한 지점이다.
 *
 * INTx 와 달리 켜기만 하고 끄지 않는다. 끄는 경로가 없는 것은 이 드라이버가
 * 언바인드를 막아 두었기 때문이기도 하다(드라이버 구조체의
 * suppress_bind_attrs).
 *
 * INTx 마스크·언마스크와 같은 잠금과 읽기-수정-쓰기를 쓴다. 같은 레지스터를
 * 건드리므로 당연히 그래야 한다.
 *
 * 실행 컨텍스트: host_init 콜백. 프로세스 컨텍스트지만 잠금이 irqsave 판이다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   sophgo_pcie_host_init() → [이 함수]
 *     → sophgo_pcie_readl_app() → sophgo_pcie_writel_app()
 */
static void sophgo_pcie_msi_enable(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct sophgo_pcie *sophgo = to_sophgo_pcie(pci);
	/* [한국어] 저장할 인터럽트 상태. */
	unsigned long flags;
	/* [한국어] 읽기-수정-쓰기 할 허용 값. */
	u32 val;

	raw_spin_lock_irqsave(&pp->lock, flags);
/* [한국어] 허용 레지스터를 읽는다. */

	val = sophgo_pcie_readl_app(sophgo, PCIE_INT_EN);
	/* [한국어] MSI 허용 비트 하나를 세운다. 이 한 줄이 없으면 DWC 의 MSI 컨트롤러가
	 * 아무리 준비돼 있어도 인터럽트가 CPU 까지 오지 않는다. */
	val |= PCIE_INT_EN_INT_MSI;
	/* [한국어] 되쓴다. INTx 의 허용 비트가 보존된다. */
	sophgo_pcie_writel_app(sophgo, val, PCIE_INT_EN);

	raw_spin_unlock_irqrestore(&pp->lock, flags);
}

/* [한국어]
 * sophgo_pcie_disable_l0s_l1 - 링크 capability 에서 ASPM 지원 표시를 지운다
 *
 * @pp: DWC 의 루트 포트 문맥.
 *
 * ASPM 은 링크가 놀 때 자동으로 절전 상태로 내려가는 기능인데, 이 하드웨어에서
 * 쓸 수 없다는 뜻이다. 그 이유는 이 트리에서 확인 못 함.
 *
 * **capability 레지스터를 고치는 것** 이 이 함수의 방법이다. 소프트웨어가
 * ASPM 을 켜려 할 때 먼저 이 레지스터를 보고 "지원한다" 를 확인하는데,
 * 그 표시를 지워 두면 아무도 켜려 하지 않는다. 제어 레지스터를 끄는 것보다
 * 확실한데, 나중에 누가 켜도 다시 이 표시에 걸리기 때문이다.
 *
 * DBI 읽기 전용 쓰기 허용을 여는 것이 필수다. LNKCAP 은 하드웨어가 정하는
 * 읽기 전용 레지스터라, 그 보호를 풀지 않으면 쓰기가 무시된다. 그 창을
 * 연 뒤 반드시 닫는 것도 마찬가지로 중요하다 — 열어 두면 다른 경로의
 * 실수가 읽기 전용 레지스터를 망칠 수 있다.
 *
 * capability 위치를 매번 찾는 것에 주의할 만하다. 캐시해 두지 않는데,
 * 이 함수가 probe 에서 한 번만 불려 비용이 문제 되지 않기 때문이다.
 *
 * 실행 컨텍스트: host_init 콜백. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. capability 를 못 찾으면 offset 이 0 이 되어 엉뚱한 자리에
 * 쓰게 되지만, 그 경우를 검사하지 않는다.
 *
 * 호출 체인:
 *   sophgo_pcie_host_init() → [이 함수]
 *     → dw_pcie_find_capability() → dw_pcie_dbi_ro_wr_en()
 *     → dw_pcie_writel_dbi() → dw_pcie_dbi_ro_wr_dis()
 */
static void sophgo_pcie_disable_l0s_l1(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	u32 offset, val;

	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
/* [한국어] 읽기 전용 쓰기 보호를 푼다. LNKCAP 은 하드웨어가 정하는 읽기 전용
 * 레지스터라, 이것 없이는 아래 쓰기가 무시된다. */

	dw_pcie_dbi_ro_wr_en(pci);

	val = dw_pcie_readl_dbi(pci, PCI_EXP_LNKCAP + offset);
	/* [한국어] L0s 와 L1 지원 표시를 함께 지운다. 소프트웨어가 ASPM 을 켜려 할 때
	 * 먼저 이 표시를 보므로, 지워 두면 아무도 켜려 하지 않는다. */
	val &= ~(PCI_EXP_LNKCAP_ASPM_L0S | PCI_EXP_LNKCAP_ASPM_L1);
	/* [한국어] 되쓴다. */
	dw_pcie_writel_dbi(pci, PCI_EXP_LNKCAP + offset, val);

	dw_pcie_dbi_ro_wr_dis(pci);
}

/* [한국어]
 * sophgo_pcie_host_init - DWC 코어가 부르는 SoC 초기화 콜백
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 파일이 DWC 코어와 만나는 **유일한 지점** 이다. dw_pcie_host_init() 이
 * 자기 초기화 도중에 이것을 불러, SoC 고유의 준비를 하게 한다.
 *
 * 세 가지를 순서대로 한다.
 * 1. INTx 도메인을 만들고 그 요약 인터럽트에 체인 핸들러를 건다.
 * 2. ASPM 을 못 쓰게 막는다.
 * 3. MSI 인터럽트를 허용한다.
 *
 * 1번이 먼저인 이유는 실패할 수 있는 유일한 단계이기 때문이다. 나머지 둘은
 * 반환값이 없어 되돌릴 것이 생기지 않는다.
 *
 * 체인 핸들러를 도메인 생성 **뒤** 에 거는 순서도 그래서다. 반대로 하면
 * 핸들러를 걸자마자 인터럽트가 올라왔을 때 갈라 보낼 도메인이 없다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 안. 프로세스 컨텍스트.
 *
 * 에러 경로: 도메인 생성이 실패하면 그 오류를 올려보내고, DWC 코어가
 * 그것을 probe 실패로 전한다.
 *
 * 호출 체인:
 *   sophgo_pcie_configure_rc() → dw_pcie_host_init()
 *     → dw_pcie_host_ops.init == [이 함수]
 *     → sophgo_pcie_init_irq_domain() → irq_set_chained_handler_and_data()
 *     → sophgo_pcie_disable_l0s_l1() → sophgo_pcie_msi_enable()
 */
static int sophgo_pcie_host_init(struct dw_pcie_rp *pp)
{
	int irq;

	irq = sophgo_pcie_init_irq_domain(pp);
	/* [한국어] 도메인 생성이 실패하면 — 이 함수에서 실패할 수 있는 유일한 단계다. */
	if (irq < 0)
		/* [한국어] 그 오류를 올려보내고, DWC 코어가 그것을 probe 실패로 전한다. */
		return irq;

	irq_set_chained_handler_and_data(irq, sophgo_pcie_intx_handler, pp);
/* [한국어] 요약 인터럽트에 체인 핸들러를 건다. 도메인을 만든 **뒤** 여야 하는데,
 * 반대로 하면 인터럽트가 올라왔을 때 갈라 보낼 도메인이 없다. */

	sophgo_pcie_disable_l0s_l1(pp);

	sophgo_pcie_msi_enable(pp);

	return 0;
}

static const struct dw_pcie_host_ops sophgo_pcie_host_ops = {
	/* [한국어] DWC 코어에 넘기는 콜백이 이 하나뿐이다 — 이 SoC 가 DWC 표준에서
	 * 벗어나는 부분이 그만큼 적다는 뜻이다. */
	.init = sophgo_pcie_host_init,
};

/* [한국어]
 * sophgo_pcie_clk_init - 이 컨트롤러가 쓰는 클럭을 모두 켠다
 *
 * @sophgo: 드라이버 상태.
 * @return: 0 = 성공, 음수 오류.
 *
 * 클럭이 없으면 컨트롤러 레지스터에 접근조차 되지 않으므로, 다른 초기화보다
 * 먼저 해야 한다.
 *
 * _get_all_enabled 판이 세 가지를 한 번에 한다 — 디바이스 트리에 적힌
 * 클럭을 **개수를 모른 채** 전부 얻고, 전부 켜고, devres 에 정리를 맡긴다.
 * 이 SoC 의 클럭 개수가 디바이스 트리에 달려 있어 코드에 고정할 수 없다.
 *
 * 반환값이 클럭 개수다. 그것을 clk_cnt 에 담아 두는데, [상류 코드 관찰]
 * 이 파일에서 그 값을 읽는 곳이 없다(전수 확인). 원본(1f0e418bb6)에서도
 * :210 의 대입 하나뿐이며, 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 클럭을 얻거나 켜지 못하면 dev_err_probe 로 기록하고 그 오류를
 * 올려보낸다.
 *
 * 호출 체인:
 *   sophgo_pcie_probe() → [이 함수] → devm_clk_bulk_get_all_enabled()
 */
static int sophgo_pcie_clk_init(struct sophgo_pcie *sophgo)
{
	struct device *dev = sophgo->pci.dev;
	int ret;

	ret = devm_clk_bulk_get_all_enabled(dev, &sophgo->clks);
	/* [한국어] 클럭을 얻거나 켜지 못했으면, */
	if (ret < 0)
		/* [한국어] 그 사실을 남기고 오류를 올려보낸다. 클럭이 없으면 컨트롤러 레지스터에
		 * 접근조차 되지 않는다. */
		return dev_err_probe(dev, ret, "failed to get clocks\n");

	sophgo->clk_cnt = ret;
/* [한국어] 반환값이 클럭 개수다. 담아 두지만 이 파일에서 읽는 곳은 없다. */

	return 0;
}

/* [한국어]
 * sophgo_pcie_resource_get - app 레지스터 창을 매핑한다
 *
 * @pdev: 플랫폼 장치.
 * @sophgo: 드라이버 상태.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 파일이 직접 다루는 레지스터 창은 하나뿐이다. DBI 창과 config 창은
 * DWC 코어가 자기 이름 규약으로 따로 얻는다.
 *
 * 이름으로 찾는 것이 그 구분의 방법이다 — 디바이스 트리의 reg-names 에
 * "app" 으로 적힌 자원만 이 파일의 것이다.
 *
 * devm 판이라 드라이버가 떨어질 때 자동으로 해제된다.
 *
 * 함수로 따로 뺀 이유는 probe 를 짧게 유지하기 위해서로 보인다. 지금은
 * 자원이 하나뿐이라 한 줄이지만, 늘어나면 이 자리에 모인다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 매핑 실패를 dev_err_probe 로 기록하고 올려보낸다.
 *
 * 호출 체인:
 *   sophgo_pcie_probe() → [이 함수]
 *     → devm_platform_ioremap_resource_byname()
 */
static int sophgo_pcie_resource_get(struct platform_device *pdev,
				    struct sophgo_pcie *sophgo)
{
	sophgo->app_base = devm_platform_ioremap_resource_byname(pdev, "app");
	if (IS_ERR(sophgo->app_base))
		/* [한국어] 오류 포인터에서 코드를 꺼내, */
		return dev_err_probe(&pdev->dev, PTR_ERR(sophgo->app_base),
				     /* [한국어] 무엇이 실패했는지와 함께 올려보낸다. */
				     "failed to map app registers\n");

	return 0;
}

/* [한국어]
 * sophgo_pcie_configure_rc - 콜백 표를 걸고 DWC 코어에 나머지를 넘긴다
 *
 * @sophgo: 드라이버 상태.
 * @return: dw_pcie_host_init() 의 결과.
 *
 * 이 드라이버가 DWC 코어에 제어를 넘기는 지점이다.
 *
 * 하는 일이 두 줄뿐이다 — 콜백 표를 걸고 코어를 부른다. 그 표에 콜백이
 * 하나(init)뿐이라, 이 SoC 가 DWC 표준에서 벗어나는 부분이 그만큼 적다는
 * 뜻이다.
 *
 * 이 뒤로는 코어가 링크 훈련, ATU 설정, MSI 도메인 생성, 버스 스캔을 모두
 * 진행하며, 그 도중에 위에서 건 init 콜백이 한 번 불린다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트이며 링크 대기와 버스 스캔으로
 * 오래 걸린다.
 *
 * 에러 경로: 코어의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   sophgo_pcie_probe() → [이 함수] → dw_pcie_host_init()
 */
static int sophgo_pcie_configure_rc(struct sophgo_pcie *sophgo)
{
	struct dw_pcie_rp *pp;

	pp = &sophgo->pci.pp;
	/* [한국어] 콜백 표를 건다. 아래 코어 호출 도중에 이 표의 init 이 한 번 불린다. */
	pp->ops = &sophgo_pcie_host_ops;

	return dw_pcie_host_init(pp);
}

/* [한국어]
 * sophgo_pcie_probe - 자원과 클럭을 얻고 DWC 호스트를 초기화한다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이며, 순서가 전부다.
 *
 * 1. 상태 구조를 잡는다. dw_pcie 가 **맨 앞** 에 있어, 이 포인터가 곧
 *    struct dw_pcie 포인터로 쓰인다.
 * 2. drvdata 에 매단다. 파일 앞머리의 to_sophgo_pcie 매크로가 그것을
 *    되찾는 통로이며, 이 파일의 거의 모든 함수가 그 매크로를 거친다.
 *    **그래서 이 대입이 다른 초기화보다 먼저여야 한다** — 아래 host_init
 *    콜백이 이미 그 매크로를 쓴다.
 * 3. dev 를 채운다. DWC 코어가 이 값으로 디바이스 트리와 로그를 다룬다.
 * 4. app 창 매핑, 클럭 켜기, 그리고 코어에 넘기기.
 *
 * 되감기 코드가 없다. 잡는 자원이 모두 devm 판이라 실패하면 코어가 자동으로
 * 되돌리기 때문이다.
 *
 * builtin_platform_driver 로 등록되어 모듈로 뺄 수 없고, 드라이버 구조체의
 * suppress_bind_attrs 가 sysfs 로 언바인드하는 것도 막는다. 호스트 브리지를
 * 런타임에 떼는 것이 안전하지 않기 때문이다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 각 단계의 오류를 그대로 올려보내며, 되감기는 devres 가 맡는다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → sophgo_pcie_resource_get() → sophgo_pcie_clk_init()
 *     → sophgo_pcie_configure_rc()
 */
static int sophgo_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sophgo_pcie *sophgo;
	/* [한국어] 각 단계의 결과. */
	int ret;

	sophgo = devm_kzalloc(dev, sizeof(*sophgo), GFP_KERNEL);
	/* [한국어] 상태 구조를 잡지 못하면, */
	if (!sophgo)
		/* [한국어] 메모리 부족으로 물러난다. */
		return -ENOMEM;

	platform_set_drvdata(pdev, sophgo);
/* [한국어] **다른 초기화보다 먼저** 매단다. 파일 앞머리의 to_sophgo_pcie 매크로가
 * 이것을 되찾는 통로이며, 아래 host_init 콜백이 이미 그 매크로를 쓴다. */

	sophgo->pci.dev = dev;
/* [한국어] DWC 코어가 이 값으로 디바이스 트리와 로그를 다룬다. */

	ret = sophgo_pcie_resource_get(pdev, sophgo);
	/* [한국어] app 창 매핑이 실패하면, */
	if (ret)
		/* [한국어] 그 오류를 올려보낸다. 되감기가 없는 것은 잡는 자원이 모두 devm 판이기 때문이다. */
		return ret;

	ret = sophgo_pcie_clk_init(sophgo);
	/* [한국어] 클럭 초기화가 실패하면, */
	if (ret)
		/* [한국어] 그 오류를 올려보낸다. */
		return ret;

	return sophgo_pcie_configure_rc(sophgo);
/* [한국어] 이 뒤로는 DWC 코어가 링크 훈련, ATU 설정, MSI 도메인 생성, 버스 스캔을 진행한다. */
}

static const struct of_device_id sophgo_pcie_of_match[] = {
	/* [한국어] 디바이스 트리에서 이 문자열로 매칭한다. */
	{ .compatible = "sophgo,sg2044-pcie" },
	/* [한국어] 표의 끝 표시. */
	{ }
};
MODULE_DEVICE_TABLE(of, sophgo_pcie_of_match);

static struct platform_driver sophgo_pcie_driver = {
	/* [한국어] 플랫폼 드라이버로 등록된다 — 호스트 브리지 자체는 PCI 장치가 아니다. */
	.driver = {
		/* [한국어] sysfs 에 나올 이름. */
		.name = "sophgo-pcie",
		/* [한국어] 위 매칭 표. */
		.of_match_table = sophgo_pcie_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = sophgo_pcie_probe,
};
builtin_platform_driver(sophgo_pcie_driver);
