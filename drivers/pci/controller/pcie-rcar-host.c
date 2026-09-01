// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe driver for Renesas R-Car SoCs
 *  Copyright (C) 2014-2020 Renesas Electronics Europe Ltd
 *
 * Based on:
 *  arch/sh/drivers/pci/pcie-sh7786.c
 *  arch/sh/drivers/pci/ops-sh7786.c
 *  Copyright (C) 2009 - 2011  Paul Mundt
 *
 * Author: Phil Edworthy <phil.edworthy@renesas.com>
 */

/*
 * [한국어 설명] R-Car SoC 의 PCIe 컨트롤러를 루트 컴플렉스로 모는 드라이버 (pcie-rcar-host.c)
 *
 * === 파일의 역할 ===
 * Renesas R-Car 계열 SoC 에 내장된 PCIe 컨트롤러를 "호스트"(루트 컴플렉스)
 * 로 동작시키는 플랫폼 드라이버다. x86 처럼 펌웨어가 버스를 미리 세워 주는
 * 환경이 아니라, 전원·클럭·PHY 를 켜고 링크를 훈련시키고 주소 창을 여는
 * 일까지 전부 이 드라이버가 한다.
 *
 * 하는 일이 크게 다섯이다.
 *   1) 자원 확보 - PHY, 레지스터 블록, 버스 클럭, MSI 용 IRQ 두 개를 DT 에서
 *      받아 온다(rcar_pcie_get_resources).
 *   2) 하드웨어 초기화 - 모드를 루트 컴플렉스로 잡고 PHY 가 준비되기를
 *      기다린 뒤, 자기 자신의 config space 를 PCI-to-PCI 브리지처럼 꾸미고
 *      링크 훈련을 시작한다(rcar_pcie_hw_init).
 *   3) config 접근 - 이 컨트롤러에는 ECAM 이 없다. 주소 레지스터에 BDF 를
 *      쓰고 데이터 레지스터로 한 워드씩 주고받는 간접 창 방식이라,
 *      rcar_pcie_config_access() 가 그 절차를 캡슐화한다.
 *   4) 주소 창 - 바깥 방향(CPU -> PCI) 최대 4개, 안쪽 방향(PCI -> 메모리)
 *      최대 6개의 창을 연다. 상수는 pcie-rcar.h 의 RCAR_PCI_MAX_RESOURCES(4),
 *      MAX_NR_INBOUND_MAPS(6) 다.
 *   5) MSI - 이 컨트롤러가 MSI 수신기를 직접 갖고 있어서, 커널 MSI 계층에
 *      붙을 irq_domain 을 스스로 만든다. 벡터 수는 INT_PCI_MSI_NR(32)이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * DT 에 "renesas,pcie-rcar-gen3" 같은 compatible 이 있으면 플랫폼 버스가
 * 이 드라이버를 붙인다.
 *
 *   platform_driver_register() / builtin_platform_driver()
 *     -> [이 파일] rcar_pcie_probe()
 *        -> devm_pci_alloc_host_bridge()    : pci_host_bridge + 이 드라이버의
 *                                             private 영역을 함께 할당
 *        -> rcar_pcie_get_resources()       : PHY/레지스터/클럭/IRQ
 *        -> rcar_pcie_parse_map_dma_ranges(): DT 의 dma-ranges -> 안쪽 창
 *        -> host->phy_init_fn()             : SoC 세대별 PHY 초기화
 *        -> rcar_pcie_hw_init()             : 모드 설정 + 링크 훈련
 *        -> rcar_pcie_enable_msi()          : irq_domain 생성 + IRQ 등록
 *        -> rcar_pcie_enable() -> pci_host_probe()  [drivers/pci/probe.c]
 *           -> 그 아래는 PCI 코어의 평범한 열거 경로다. 코어가 config 를
 *              읽을 때마다 rcar_pcie_ops 의 두 콜백이 불린다.
 *
 * 실행 컨텍스트: probe/resume 은 프로세스 컨텍스트다. config 접근 콜백은
 * PCI 코어가 pci_lock 을 잡은 상태에서 부르며(상류 주석이 그 점을 두 곳에
 * 적어 두었다), MSI 핸들러 rcar_pcie_msi_irq() 와 irq_chip 콜백들은
 * 인터럽트 컨텍스트에서 돈다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/probe.c 의 pci_host_probe() 가 열거를 시작하고,
 *   drivers/pci/access.c 가 pci_lock 을 쥔 채 rcar_pcie_read_conf() /
 *   rcar_pcie_write_conf() 를 부른다.
 * 아래쪽: pcie-rcar.c 가 이 파일과 ep 판이 공유하는 저수준 헬퍼를 갖고 있다 -
 *   rcar_pci_read_reg()/write_reg()(pcie-rcar.c:13,18), rcar_rmw32()(:23),
 *   rcar_pcie_wait_for_phyrdy()(:32), rcar_pcie_wait_for_dl()(:44),
 *   rcar_pcie_set_outbound()(:58), rcar_pcie_set_inbound()(:95).
 *   Makefile:11 이 pcie-rcar.o 와 pcie-rcar-host.o 를 함께 빌드한다.
 * 옆쪽: pcie-rcar-ep.c 가 같은 하드웨어를 엔드포인트로 모는 짝이다.
 *   Makefile:12 가 그쪽을 CONFIG_PCIE_RCAR_EP 로 묶는다. 두 드라이버는
 *   같은 레지스터 정의(pcie-rcar.h)를 쓰지만 PCIEMSR 에 쓰는 값이 달라
 *   (호스트는 1, 엔드포인트는 0) 하드웨어의 동작 모드가 갈린다.
 * 공유 상태: struct rcar_pcie(pcie-rcar.h) 가 dev 와 레지스터 기준 주소만
 *   담은 최소 구조체이고, 이 파일의 struct rcar_pcie_host 가 그것을 감싸
 *   PHY/클럭/MSI 상태를 덧붙인다.
 *
 * === 주요 함수/구조체 요약 ===
 * rcar_pcie_probe()        : 진입점. 자원 확보부터 열거 시작까지 전 과정을 엮는다.
 * rcar_pcie_hw_init()      : 모드 설정, 자기 config space 꾸미기, 링크 훈련.
 * rcar_pcie_config_access(): 간접 창(PCIECAR/PCIECCTLR/PCIECDR)으로 config 를
 *                            읽고 쓴다. 루트 버스는 창을 쓰지 않고 PCICONF() 로
 *                            직접 접근하는 우회가 들어 있다.
 * rcar_pcie_force_speedup(): 2.5GT/s 로 붙은 링크를 5.0GT/s 로 올린다.
 * rcar_pcie_inbound_ranges(): dma-ranges 한 항목을 정렬 제약에 맞춰 여러 개의
 *                            안쪽 창으로 쪼갠다.
 * rcar_pcie_msi_irq()      : MSI 수신 핸들러. PCIEMSIFR 의 비트를 훑어 넘긴다.
 * rcar_allocate_domains()  : 이 컨트롤러의 MSI irq_domain 을 만든다.
 * rcar_pcie_wakeup()       : L1 에 걸린 링크를 깨운다. config 접근마다 먼저 부른다.
 * struct rcar_pcie_host    : 이 드라이버의 장치별 상태 전부.
 * struct rcar_msi          : MSI 벡터 비트맵과 도메인, 두 개의 IRQ 번호.
 * rcar_pcie_of_match[]     : SoC 세대별 PHY 초기화 함수를 compatible 에 묶는 표.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 전체에서 이 파일(및 pcie-rcar 계열)의 심볼을 쓰는 곳은
 * 하나도 없다(전수 grep 확인). 방향이 반대이기 때문이다 - 이 드라이버는
 * 버스를 만들고, NVMe 는 그 버스 위에 열거되는 장치일 뿐이다.
 *
 * 굳이 접점을 찾자면 이 드라이버가 세운 것 위에서 NVMe 가 도는 관계다.
 * R-Car 보드에 NVMe SSD 를 붙이면 rcar_pcie_hw_init() 이 링크를 세우고,
 * pci_host_probe() 가 그 SSD 를 발견하고, 바깥 창이 BAR 접근을,
 * 안쪽 창이 SSD 의 DMA 를 각각 통과시킨다. 다만 그 어느 것도 NVMe 에
 * 특화된 처리가 아니라 모든 PCIe 장치에 똑같이 적용된다.
 *
 * (주의: 공유 헤더 pcie-rcar.h 에는 레지스터 정의마다 "NVMe endpoint config
 *  space", "NVMe queues/PRP buffers" 같은 문구가 붙은 한 줄 주석이 13개
 *  남아 있는데, 원본 스냅숏에는 그 자리에 주석이 아예 없다. 앞선 작업이
 *  넣은 여러 줄 주석의 꼬리만 남은 잔재이므로 근거로 삼지 않았다.
 *  이 파일의 레지스터 설명은 pcie-rcar.h 의 #define 값과 pcie-rcar.c 의
 *  실제 사용 코드만을 근거로 적었다.)
 */

/* [한국어] bitops.h — BIT(), find_first_bit(), order_base_2(). MSI 벡터 비트맵과
 * 레지스터 비트 조작에 쓴다. */
#include <linux/bitops.h>
/* [한국어] cleanup.h — scoped_guard(). 아래 MSI 마스킹에서 락을 블록 범위로 묶는다. */
#include <linux/cleanup.h>
/* [한국어] clk.h — clk_prepare_enable() / clk_disable_unprepare(). 버스 클럭을 켜고 끈다. */
#include <linux/clk.h>
/* [한국어] clk-provider.h — 클럭 제공자 쪽 정의. */
#include <linux/clk-provider.h>
/* [한국어] delay.h — udelay(), msleep(), usleep_range(). PHY ack 대기와 속도 변경 대기에 쓴다. */
#include <linux/delay.h>
/* [한국어] interrupt.h — devm_request_irq(), IRQF_SHARED, irqreturn_t. MSI 핸들러 등록. */
#include <linux/interrupt.h>
/* [한국어] irq.h — struct irq_data, struct irq_chip. MSI 의 irq_chip 콜백이 받는 타입. */
#include <linux/irq.h>
/* [한국어] irqchip/irq-msi-lib.h — msi_lib_init_dev_msi_info(). 아래 msi_parent_ops 가
 * 그 함수를 그대로 꽂아 쓴다. */
#include <linux/irqchip/irq-msi-lib.h>
/* [한국어] irqdomain.h — irq_domain 생성/삭제와 generic_handle_domain_irq(). */
#include <linux/irqdomain.h>
/* [한국어] kernel.h — min(), 기본 매크로. */
#include <linux/kernel.h>
/* [한국어] init.h — __init / __initconst / device_initcall. */
#include <linux/init.h>
/* [한국어] iopoll.h — readl_poll_timeout_atomic(). L1 깨우기에서 잠들지 않고 폴링한다. */
#include <linux/iopoll.h>
/* [한국어] msi.h — struct msi_msg, msi_create_parent_irq_domain(), msi_parent_ops. */
#include <linux/msi.h>
/* [한국어] of_address.h — of_address_to_resource(). DT 0번 자원에서 레지스터 블록을 얻는다. */
#include <linux/of_address.h>
/* [한국어] of_irq.h — irq_of_parse_and_map() / irq_dispose_mapping(). MSI 용 IRQ 두 개. */
#include <linux/of_irq.h>
/* [한국어] of_platform.h — of_device_get_match_data(). compatible 에 묶인 PHY 초기화 함수. */
#include <linux/of_platform.h>
/* [한국어] pci.h — PCI_SLOT/PCI_FUNC, PCIBIOS_* 반환 코드, PCI_EXP_* 레지스터 정의. */
#include <linux/pci.h>
/* [한국어] phy/phy.h — 범용 PHY API. 3세대에서만 실제로 쓴다. */
#include <linux/phy/phy.h>
/* [한국어] platform_device.h — platform_driver, platform_set_drvdata(). */
#include <linux/platform_device.h>
/* [한국어] pm_runtime.h — 런타임 PM. probe 에서 전원/클럭을 붙잡는다. */
#include <linux/pm_runtime.h>
/* [한국어] regulator/consumer.h — devm_regulator_get_enable_optional(). 보드 전원 레일. */
#include <linux/regulator/consumer.h>

/* [한국어] pcie-rcar.h — host 와 ep 가 공유하는 레지스터 정의와 저수준 헬퍼 선언.
 * struct rcar_pcie, INT_PCI_MSI_NR(32), MAX_NR_INBOUND_MAPS(6),
 * RCAR_PCI_MAX_RESOURCES(4)가 여기 있다. */
#include "pcie-rcar.h"

/* [한국어] 이 컨트롤러의 MSI 수신 상태를 한데 묶은 구조체. */
struct rcar_msi {
	/* [한국어] 할당된 MSI 벡터를 표시하는 비트맵(INT_PCI_MSI_NR = 32비트).
	 * 설정자: rcar_msi_domain_alloc() 이 자리를 잡고, rcar_msi_domain_free() 가 푼다.
	 * 읽는 자: 위 둘과 rcar_pcie_resume() 이 PCIEMSIIER 복원에 쓴다.
	 * 값 범위: 비트 0..31. 1 이면 그 벡터가 할당돼 있다.
	 * 동기화: 아래 map_lock 뮤텍스가 보호한다. */
	DECLARE_BITMAP(used, INT_PCI_MSI_NR);
	/* [한국어] 이 컨트롤러가 만든 MSI irq_domain.
	 * 설정자: rcar_allocate_domains().
	 * 읽는 자: rcar_pcie_msi_irq() 가 generic_handle_domain_irq() 에 넘기고,
	 *   rcar_free_domains() 가 해제한다.
	 * 값 범위: 유효한 포인터. 생성 실패 시 함수가 -ENOMEM 을 돌려주므로 NULL 이 남지 않는다.
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다. */
	struct irq_domain *domain;
	/* [한국어] used 비트맵을 보호하는 뮤텍스.
	 * 설정자/읽는 자: rcar_msi_domain_alloc() / _free().
	 * 값 범위: 뮤텍스.
	 * 동기화: 벡터 할당은 프로세스 컨텍스트에서만 일어나므로 잠들 수 있는 락으로 충분하다. */
	struct mutex map_lock;
	/* [한국어] PCIEMSIIER 갱신을 보호하는 raw spinlock.
	 * 설정자/읽는 자: rcar_msi_irq_mask() / _unmask().
	 * 값 범위: raw spinlock.
	 * 동기화: 이 경로는 인터럽트 컨텍스트에서 불려 잠들 수 없으므로 raw 계열이어야 한다.
	 *   위 map_lock 이 뮤텍스인 것과 대비된다 — 쓰이는 문맥이 다르다. */
	raw_spinlock_t mask_lock;
	/* [한국어] DT 인터럽트 목록 0번에서 매핑한 IRQ 번호.
	 * 설정자: rcar_pcie_get_resources().
	 * 읽는 자: rcar_pcie_enable_msi() 가 핸들러를 걸고, probe 의 에러 경로가 매핑을 푼다.
	 * 값 범위: 유효한 virq(0 이면 매핑 실패로 보고 probe 가 실패한다).
	 * 동기화: probe 에서만 설정된다. */
	int irq1;
	/* [한국어] DT 인터럽트 목록 1번에서 매핑한 IRQ 번호. irq1 과 같은 핸들러를 공유한다.
	 * 설정자/읽는 자/값 범위/동기화는 irq1 과 동일하다.
	 * 둘을 두는 이유는 하드웨어가 인터럽트 선을 두 개 내보내기 때문이다. */
	int irq2;
};

/* Structure representing the PCIe interface */
/* [한국어] 이 드라이버의 장치별 상태 전부. pci_host_bridge 의 private 영역에 얹힌다. */
struct rcar_pcie_host {
	/* [한국어] host 와 ep 가 공유하는 최소 구조체(pcie-rcar.h). dev 와 레지스터 기준 주소를 담는다.
	 * 설정자: rcar_pcie_probe() 가 dev 를, rcar_pcie_get_resources() 가 base 를 채운다.
	 * 읽는 자: 이 파일의 거의 모든 함수.
	 * 값 범위: base 는 ioremap 된 유효 주소.
	 * 동기화: probe 에서 설정된 뒤 읽기 전용이다.
	 * 첫 필드로 둔 덕분에 &host->pcie 와 host 의 주소가 같아, 두 타입 사이를 오가기 쉽다. */
	struct rcar_pcie	pcie;
	/* [한국어] 3세대에서만 쓰는 범용 PHY 핸들.
	 * 설정자: rcar_pcie_get_resources() 의 devm_phy_optional_get().
	 * 읽는 자: rcar_pcie_phy_init_gen3() 과 probe 의 에러 경로.
	 * 값 범위: 유효한 포인터 또는 NULL(1·2세대는 PHY 장치가 따로 없다).
	 * 동기화: probe 에서만 설정된다. */
	struct phy		*phy;
	/* [한국어] "pcie_bus" 클럭.
	 * 설정자: rcar_pcie_get_resources().
	 * 읽는 자: probe 가 clk_prepare_enable() 로 켜고 에러 경로가 끈다.
	 * 값 범위: 유효한 클럭 포인터.
	 * 동기화: probe 에서만 설정된다. */
	struct clk		*bus_clk;
	/* [한국어] MSI 수신 상태 묶음. 포인터가 아니라 값으로 박아 두어 container_of 로
	 * 되찾을 수 있게 했다(msi_to_host 참조).
	 * 설정자/읽는 자: MSI 관련 함수 전부.
	 * 값 범위: 위 struct rcar_msi 참조.
	 * 동기화: 필드마다 다르다 — 내부의 두 락이 나눠 맡는다. */
	struct			rcar_msi msi;
	/* [한국어] 이 SoC 세대에 맞는 PHY 초기화 함수.
	 * 설정자: rcar_pcie_probe() 가 of_device_get_match_data() 로 받아 꽂는다.
	 * 읽는 자: probe 와 rcar_pcie_resume().
	 * 값 범위: rcar_pcie_phy_init_h1 / _gen2 / _gen3 중 하나.
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다.
	 * 이 포인터 하나가 세대별 차이를 흡수해 나머지 코드를 공통으로 유지한다. */
	int			(*phy_init_fn)(struct rcar_pcie_host *host);
};

/* [한국어]
 * rcar_pcie_wakeup - L1 절전에 걸린 링크를 깨워 L0 로 돌아오게 한다
 *
 * @pcie_dev:  로그와 런타임 PM 상태 확인에 쓸 device.
 * @pcie_base: 레지스터 블록의 기준 가상 주소.
 * @return: 0 성공(깨울 필요가 없었던 경우 포함). -EINVAL 은 기준 주소가 없거나
 *          런타임 절전 중이라 손댈 수 없는 경우, -ETIMEDOUT 은 L1 진입이
 *          제 시간에 끝나지 않은 경우.
 *
 * 상류 주석이 설명하는 하드웨어 결함 우회다. 상대가 PM_ENTER_L1 DLLP 를
 * 보냈는데 컨트롤러가 아직 L1 링크 상태로 들어가지 못한 어중간한 상태가
 * 있고, 그대로 config 접근을 하면 응답이 오지 않는다. 그래서 강제로 L1
 * 진입을 지시해 상태를 정리하면, 하드웨어가 스스로 L0s/L0 로 돌아온다.
 *
 * 판정이 두 조건의 AND 다 - PMEL1RX(PM_ENTER_L1 을 받았음)가 서 있고,
 * PMSTATE 필드가 PMSTATE_L1 이 *아니어야* 한다. 둘 다여야 그 어중간한
 * 상태이기 때문이다.
 *
 * 조치는 PMCTLR 에 L1IATN(L1 진입 지시)을 쓰고, PMSR 의 L1FAEG 가 설 때까지
 * 10us 간격으로 최대 1ms 폴링한다. 시간이 초과돼도 진행은 하되 경고를
 * 남기며, 마지막에 L1FAEG 와 PMEL1RX 를 1 로 써서 지운다(이 하드웨어의
 * 상태 비트는 1 을 써서 지우는 방식이다).
 *
 * readl_poll_timeout_atomic 을 쓰는 이유는 이 함수가 config 접근 경로에서
 * 불리는데 그 경로는 pci_lock 을 쥔 채라 잠들 수 없기 때문이다.
 * 실행 컨텍스트: 아토믹. 잠들지 않는다.
 *
 * 호출 체인:  rcar_pcie_config_access() → [이 함수] → readl_poll_timeout_atomic()
 */
static int rcar_pcie_wakeup(struct device *pcie_dev, void __iomem *pcie_base)
{
	/* [한국어] pmsr 은 읽어 온 PMSR 값, val 은 폴링 매크로가 쓸 임시 변수. */
	u32 pmsr, val;
	/* [한국어] 기본값 0 — 깨울 필요가 없으면 그대로 성공으로 돌아간다. */
	int ret = 0;

	/* [한국어] 기준 주소가 없거나 이미 런타임 절전에 들어갔으면 레지스터를 만질 수 없다. */
	if (!pcie_base || pm_runtime_suspended(pcie_dev))
		/* [한국어] 인자/상태 오류. */
		return -EINVAL;

	/* [한국어] 전원 관리 상태 레지스터를 읽는다. */
	pmsr = readl(pcie_base + PMSR);

	/*
	 * Test if the PCIe controller received PM_ENTER_L1 DLLP and
	 * the PCIe controller is not in L1 link state. If true, apply
	 * fix, which will put the controller into L1 link state, from
	 * which it can return to L0s/L0 on its own.
	 */
	/* [한국어] PM_ENTER_L1 DLLP 를 받았는데(PMEL1RX) 아직 L1 상태가 아닌 어중간한 경우.
	 * 두 조건의 AND 여야 상류 주석이 말하는 그 상태다. */
	if ((pmsr & PMEL1RX) && ((pmsr & PMSTATE) != PMSTATE_L1)) {
		/* [한국어] PMCTLR 에 L1IATN 을 써서 L1 진입을 강제로 지시한다. */
		writel(L1IATN, pcie_base + PMCTLR);
		/* [한국어] PMSR 을 폴링하며 L1FAEG(L1 진입 완료)가 서기를 기다린다. */
		ret = readl_poll_timeout_atomic(pcie_base + PMSR, val,
						/* [한국어] 10us 간격, 최대 1000us. 아토믹 판이라 잠들지 않는다 — 이 경로는
						 * pci_lock 을 쥔 상태로 불리기 때문이다. */
						val & L1FAEG, 10, 1000);
		/* [한국어] 시간이 초과돼도 진행은 한다. */
		if (ret) {
			/* [한국어] 다만 경고를 남긴다. ratelimited 인 이유는 config 접근마다 불리는 경로라
			 * 같은 메시지가 쏟아질 수 있어서다. */
			dev_warn_ratelimited(pcie_dev,
					     "Timeout waiting for L1 link state, ret=%d\n",
					     ret);
		}
		/* [한국어] 두 상태 비트를 1 로 써서 지운다. 이 하드웨어는 1 을 쓰면 지워지는 방식이다. */
		writel(L1FAEG | PMEL1RX, pcie_base + PMSR);
	}

	/* [한국어] 폴링 결과를 그대로 전한다. 깨울 필요가 없었으면 0 이다. */
	return ret;
}

/* [한국어]
 * msi_to_host - struct rcar_msi 에서 그것을 품은 rcar_pcie_host 를 되찾는다
 *
 * @msi: 이 컨트롤러의 MSI 상태 묶음.
 * @return: 그 msi 를 필드로 가진 rcar_pcie_host.
 *
 * irq_domain 의 host_data 와 irq_chip 의 chip_data 에는 struct rcar_msi 만
 * 넣어 두는데, 실제로 레지스터를 만지려면 rcar_pcie(그 안의 base)가 필요하다.
 * container_of 로 바깥 구조체를 되찾는 그 변환을 한 곳에 모아 둔 것이다.
 *
 * 실행 컨텍스트: 제약 없음. 포인터 산술 한 번이라 인터럽트 문맥에서도 안전하다.
 *
 * 호출 체인:  rcar_msi_irq_ack/mask/unmask(), rcar_compose_msi_msg(),
 *             rcar_allocate_domains(), rcar_pcie_enable_msi() → [이 함수]
 */
static struct rcar_pcie_host *msi_to_host(struct rcar_msi *msi)
{
	/* [한국어] msi 필드의 주소에서 그것을 품은 rcar_pcie_host 의 주소를 역산한다.
	 * irq_domain 과 irq_chip 에는 msi 포인터만 넘겨 두지만, 실제 레지스터 접근에는
	 * pcie->base 가 필요해서 이 되찾기가 필요하다. */
	return container_of(msi, struct rcar_pcie_host, msi);
}

/* [한국어]
 * rcar_read_conf - 자기 자신의 config space 를 바이트 오프셋 기준으로 읽는다
 *
 * @pcie:  컨트롤러.
 * @where: 바이트 단위 오프셋. 4의 배수가 아니어도 된다.
 * @return: @where 위치부터 시작하는 값(상위 바이트는 남아 있을 수 있다).
 *
 * 레지스터 접근은 32비트 단위인데 호출자는 PCI_STATUS 처럼 4의 배수가 아닌
 * 오프셋을 쓰고 싶어 한다. 그래서 where & ~3 으로 워드를 읽고, 하위 2비트가
 * 가리키는 바이트 수만큼 오른쪽으로 밀어 원하는 필드를 최하위로 끌어내린다.
 *
 * 마스킹은 하지 않으므로 상위 쪽 쓰레기가 남는다 - 호출자가 필요한 비트만
 * 검사하는 것을 전제한 함수다. 실제로 유일한 호출자는 PCI_STATUS 의 두
 * abort 비트만 확인한다.
 *
 * 실행 컨텍스트: config 접근 경로. 잠들지 않는다.
 *
 * 호출 체인:  rcar_pcie_config_access() → [이 함수] → rcar_pci_read_reg()
 */
static u32 rcar_read_conf(struct rcar_pcie *pcie, int where)
{
	/* [한국어] 오프셋의 하위 2비트가 워드 안 바이트 위치이므로, 그만큼 비트로 환산해
	 * 오른쪽으로 밀 양을 구한다. */
	unsigned int shift = BITS_PER_BYTE * (where & 3);
	/* [한국어] 4의 배수로 내림한 주소에서 워드를 통째로 읽는다. */
	u32 val = rcar_pci_read_reg(pcie, where & ~3);

	/* [한국어] 원하는 필드를 최하위로 끌어내린다. 마스킹은 하지 않으므로 상위에
	 * 쓰레기가 남는다 — 호출자가 필요한 비트만 본다는 전제다. */
	return val >> shift;
}

/* [한국어] 아래 인라인 어셈블리 우회는 ARM 32비트에서만 필요하다. */
#ifdef CONFIG_ARM
/* [한국어] 없는 장치에 접근하면 나는 외부 abort 를 커널 oops 로 만들지 않기 위한 틀.
 * str/ldr 중 하나를 instr 로 받아 두 함수가 공유한다. */
#define __rcar_pci_rw_reg_workaround(instr)				\
		/* [한국어] 이 블록을 armv7-a 명령 집합으로 어셈블한다. */
		"	.arch armv7-a\n"				\
		/* [한국어] 실제 접근 명령. %1 이 값, %2 가 주소다. 1번 레이블이 예외 테이블의 키가 된다. */
		"1:	" instr " %1, [%2]\n"				\
		/* [한국어] 명령 동기화 장벽. abort 가 비동기라 다음 명령에서 잡힐 수 있어,
		 * 이 지점도 아래에서 예외 테이블에 함께 등록한다. */
		"2:	isb\n"						\
		/* [한국어] 정상 경로가 도달하는 지점이자, 아래 fixup 코드를 .text.fixup 섹션에 넣기 시작하는 곳. */
		"3:	.pushsection .text.fixup,\"ax\"\n"		\
		"	.align	2\n"					\
		/* [한국어] 예외가 나면 여기로 점프한다. %0(error)에 PCIBIOS_SET_FAILED 를 넣는다. */
		"4:	mov	%0, #" __stringify(PCIBIOS_SET_FAILED) "\n" \
		/* [한국어] 그리고 3번(정상 흐름 뒤)으로 돌아가 함수가 실패 코드를 돌려주게 한다. */
		"	b	3b\n"					\
		"	.popsection\n"					\
		/* [한국어] 여기부터 예외 테이블 항목을 __ex_table 섹션에 넣는다. */
		"	.pushsection __ex_table,\"a\"\n"		\
		"	.align	3\n"					\
		/* [한국어] 1번(접근 명령)에서 난 예외를 4번(fixup)으로 보낸다. */
		"	.long	1b, 4b\n"				\
		/* [한국어] 2번(isb)에서 난 예외도 같은 곳으로 보낸다. */
		"	.long	2b, 4b\n"				\
		"	.popsection\n"
#endif

/* [한국어]
 * rcar_pci_write_reg_workaround - 예외를 잡을 수 있는 형태로 config 데이터를 쓴다
 *
 * @pcie: 컨트롤러.   @val: 쓸 값.   @reg: 레지스터 오프셋.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_SET_FAILED.
 *
 * ARM 32비트에서는 존재하지 않는 장치에 접근하면 버스가 외부 abort 를
 * 일으킨다. 평범한 writel 로 쓰면 그 abort 가 그대로 커널 oops 가 되므로,
 * 인라인 어셈블리로 명령을 직접 내고 예외 테이블에 수정 항목을 등록해
 * abort 가 나면 error 변수에 실패 코드를 넣고 이어 가게 만든다.
 *
 * 위 __rcar_pci_rw_reg_workaround 매크로가 그 뼈대다. .pushsection 으로
 * .text.fixup 과 __ex_table 에 항목을 넣어, 1번(str)과 2번(isb) 레이블에서
 * 난 예외를 4번 레이블로 보낸다. isb 까지 함께 등록하는 이유는 abort 가
 * 비동기라 다음 명령에서 잡힐 수 있기 때문이다.
 *
 * ARM 이 아닌 아키텍처에서는 그런 우회가 필요 없어 평범한 쓰기로 컴파일된다.
 * 실행 컨텍스트: config 접근 경로. 잠들지 않는다.
 *
 * 호출 체인:  rcar_pcie_config_access() → [이 함수] → 인라인 asm 또는 rcar_pci_write_reg()
 */
static int rcar_pci_write_reg_workaround(struct rcar_pcie *pcie, u32 val,
					 unsigned int reg)
{
	/* [한국어] 기본값은 성공. 예외가 나면 위 fixup 이 이 변수를 실패로 바꾼다. */
	int error = PCIBIOS_SUCCESSFUL;
/* [한국어] ARM 이면 예외를 잡을 수 있는 인라인 어셈블리로, */
#ifdef CONFIG_ARM
	asm volatile(
		__rcar_pci_rw_reg_workaround("str")
	/* [한국어] "+r"(error) 는 입출력 겸용 — fixup 이 여기에 실패 코드를 써 넣는다.
	 * "memory" clobber 로 컴파일러가 이 접근의 순서를 바꾸지 못하게 막는다. */
	: "+r"(error):"r"(val), "r"(pcie->base + reg) : "memory");
/* [한국어] 다른 아키텍처는 그런 우회가 필요 없어 */
#else
	/* [한국어] 평범한 쓰기로 컴파일된다. */
	rcar_pci_write_reg(pcie, val, reg);
#endif
	/* [한국어] 성공 또는 fixup 이 넣은 실패 코드. */
	return error;
}

/* [한국어]
 * rcar_pci_read_reg_workaround - 예외를 잡을 수 있는 형태로 config 데이터를 읽는다
 *
 * @pcie: 컨트롤러.   @val: 읽은 값을 담을 곳.   @reg: 레지스터 오프셋.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_SET_FAILED.
 *
 * 쓰기 판과 같은 우회이며 명령만 ldr 로 바뀐다.
 *
 * 읽기에만 있는 처리가 하나 있다. 실패했으면 PCI_SET_ERROR_RESPONSE 로
*val 을 "전부 1" 에 해당하는 오류 응답 값으로 채운다. abort 가 난 뒤
 * 레지스터에 남은 쓰레기를 호출자가 실제 데이터로 오해하지 않게 하려는
 * 것이며, 없는 장치를 읽었을 때 PCI 가 원래 돌려주는 값과도 일치한다.
 *
 * 실행 컨텍스트: config 접근 경로. 잠들지 않는다.
 *
 * 호출 체인:  rcar_pcie_config_access() → [이 함수] → 인라인 asm 또는 rcar_pci_read_reg()
 */
static int rcar_pci_read_reg_workaround(struct rcar_pcie *pcie, u32 *val,
					unsigned int reg)
{
	/* [한국어] 기본값은 성공. */
	int error = PCIBIOS_SUCCESSFUL;
/* [한국어] ARM 이면 ldr 판 인라인 어셈블리. */
#ifdef CONFIG_ARM
	asm volatile(
		__rcar_pci_rw_reg_workaround("ldr")
	/* [한국어] "=r"(*val) 로 읽은 값을 받는다. */
	: "+r"(error), "=r"(*val) : "r"(pcie->base + reg) : "memory");

	/* [한국어] 예외가 나서 실패로 바뀌었으면 */
	if (error != PCIBIOS_SUCCESSFUL)
		/* [한국어] *val 을 "전부 1" 오류 응답으로 채운다. abort 뒤 레지스터에 남은 쓰레기를
		 * 호출자가 실제 데이터로 오해하지 않게 하려는 것이며, 없는 장치를 읽었을 때
		 * PCI 가 원래 돌려주는 값과도 일치한다. */
		PCI_SET_ERROR_RESPONSE(val);
#else
	/* [한국어] 다른 아키텍처는 평범한 읽기. */
	*val = rcar_pci_read_reg(pcie, reg);
#endif
	/* [한국어] 성공 또는 실패 코드. */
	return error;
}

/* Serialization is provided by 'pci_lock' in drivers/pci/access.c */
/* [한국어]
 * rcar_pcie_config_access - 간접 창으로 config space 한 워드를 읽거나 쓴다
 *
 * @pcie 계열 인자는 아래 참조.
 * @host:        컨트롤러 상태.
 * @access_type: RCAR_PCI_ACCESS_READ 또는 RCAR_PCI_ACCESS_WRITE.
 * @bus, @devfn: 대상 장치.
 * @where:       config 오프셋.
 * @data:        읽기면 결과를 담을 곳, 쓰기면 쓸 값.
 * @return: PCIBIOS_SUCCESSFUL / PCIBIOS_DEVICE_NOT_FOUND / PCIBIOS_SET_FAILED.
 *
 * 이 컨트롤러에는 ECAM(메모리 매핑 config)이 없다. 대신 세 레지스터로 된
 * 간접 창을 쓴다.
 *   PCIECAR    - 대상 BDF 와 오프셋을 조립해 써 넣는 주소 레지스터
 *   PCIECCTLR  - 접근을 여는 제어 레지스터. TYPE0 면 바로 아래 장치,
 *                TYPE1 이면 그 아래 브리지 너머를 뜻한다
 *   PCIECDR    - 실제 데이터가 오가는 레지스터
 *
 * 절차는 (0) 링크 깨우기 → (1) 오류 플래그 지우기 → (2) 주소 쓰기 →
 * (3) 접근 열기 → (4) 오류 확인 → (5) 데이터 주고받기 → (6) 접근 닫기 다.
 * 접근을 반드시 닫는 이유는 열린 채로 두면 다음 접근이 엉키기 때문이다.
 *
 * 루트 버스는 이 창을 쓰지 않는다. 상류 주석이 길게 설명하듯, 이 컨트롤러는
 * 루트 컴플렉스 모드에서 type 0 으로도 type 1 으로도 자기 자신을 지목할 수
 * 없고 시도하면 completer abort 가 난다. 그래서 자기 config space 는
 * PCICONF(index) 로 직접 읽고 쓰며, 채널당 장치가 하나뿐인 성질을 이용해
 * devfn 0 에 묶어 자기 열거가 가능하게 만든다. dev != 0 이면 없는 장치로 답한다.
 *
 * 오류 판정도 두 겹이다. PCIEERRFR 의 UNSUPPORTED_REQUEST 와, 자기
 * config space 의 PCI_STATUS 에 선 master/target abort 비트를 함께 본다.
 *
 * 직렬화는 이 함수가 하지 않는다 - 상류 주석대로 drivers/pci/access.c 의
 * pci_lock 이 이미 보장한다.
 * 실행 컨텍스트: 아토믹. pci_lock 을 쥔 상태로 불린다.
 *
 * 호출 체인:  PCI 코어(access.c) → rcar_pcie_read_conf()/write_conf()
 *               → [이 함수] → rcar_pcie_wakeup() → rcar_pci_*_reg_workaround()
 */
static int rcar_pcie_config_access(struct rcar_pcie_host *host,
		unsigned char access_type, struct pci_bus *bus,
		unsigned int devfn, int where, u32 *data)
{
	/* [한국어] host 의 첫 필드라 주소가 같지만, 타입을 맞추기 위해 명시적으로 얻는다. */
	struct rcar_pcie *pcie = &host->pcie;
	/* [한국어] devfn 에서 뽑을 장치/기능 번호, 4로 정렬한 오프셋과 그 워드 인덱스. */
	unsigned int dev, func, reg, index;
	/* [한국어] 하위 호출 결과. */
	int ret;

	/* Wake the bus up in case it is in L1 state. */
	/* [한국어] config 접근 전에 반드시 링크를 깨운다. L1 에 걸린 상태로 접근하면
	 * 응답이 오지 않는다. */
	ret = rcar_pcie_wakeup(pcie->dev, pcie->base);
	/* [한국어] 깨우기에 실패했으면 */
	if (ret) {
		/* [한국어] 읽기였을 수 있으므로 오류 응답 값을 채워 두고 */
		PCI_SET_ERROR_RESPONSE(data);
		/* [한국어] 실패로 돌린다. */
		return PCIBIOS_SET_FAILED;
	}

	/* [한국어] devfn 의 상위 5비트가 장치 번호. */
	dev = PCI_SLOT(devfn);
	/* [한국어] 하위 3비트가 기능 번호. */
	func = PCI_FUNC(devfn);
	/* [한국어] 4의 배수로 내림한 오프셋. 하드웨어는 워드 단위로만 다룬다. */
	reg = where & ~3;
	/* [한국어] 루트 버스 자기 접근에서 PCICONF(index) 로 쓸 워드 번호. */
	index = reg / 4;

	/*
	 * While each channel has its own memory-mapped extended config
	 * space, it's generally only accessible when in endpoint mode.
	 * When in root complex mode, the controller is unable to target
	 * itself with either type 0 or type 1 accesses, and indeed, any
	 * controller-initiated target transfer to its own config space
	 * results in a completer abort.
	 *
	 * Each channel effectively only supports a single device, but as
	 * the same channel <-> device access works for any PCI_SLOT()
	 * value, we cheat a bit here and bind the controller's config
	 * space to devfn 0 in order to enable self-enumeration. In this
	 * case the regular ECAR/ECDR path is sidelined and the mangled
	 * config access itself is initiated as an internal bus transaction.
	 */
	if (pci_is_root_bus(bus)) {
		/* [한국어] 채널당 장치가 하나뿐이라 0번 말고는 있을 수 없다. */
		if (dev != 0)
			/* [한국어] 없는 장치로 답한다. */
			return PCIBIOS_DEVICE_NOT_FOUND;

		/* [한국어] 루트 버스는 간접 창을 쓰지 않고 PCICONF() 로 자기 config 를 직접 만진다.
		 * 상류 주석대로 이 컨트롤러는 자기 자신을 type 0/1 로 지목할 수 없기 때문이다. */
		if (access_type == RCAR_PCI_ACCESS_READ)
			/* [한국어] 워드 인덱스로 자기 config space 를 읽는다. */
			*data = rcar_pci_read_reg(pcie, PCICONF(index));
		else
			/* [한국어] 또는 쓴다. */
			rcar_pci_write_reg(pcie, *data, PCICONF(index));

		/* [한국어] 자기 접근은 여기서 끝난다 — 아래 간접 창 절차를 타지 않는다. */
		return PCIBIOS_SUCCESSFUL;
	}

	/* Clear errors */
	/* [한국어] 읽은 값을 그대로 되써서 오류 플래그를 지운다. 이 하드웨어의 상태 비트는
	 * 1 을 쓰면 지워지므로, 지금 서 있는 비트만 정확히 지우는 관용구다. */
	rcar_pci_write_reg(pcie, rcar_pci_read_reg(pcie, PCIEERRFR), PCIEERRFR);

	/* Set the PIO address */
	/* [한국어] BDF 와 오프셋을 한 워드로 조립해 주소 레지스터에 쓴다.
	 * PCIE_CONF_BUS 는 24비트, _DEV 는 19비트, _FUNC 는 16비트 자리로 미는
	 * 매크로다(pcie-rcar.h). */
	rcar_pci_write_reg(pcie, PCIE_CONF_BUS(bus->number) |
		PCIE_CONF_DEV(dev) | PCIE_CONF_FUNC(func) | reg, PCIECAR);

	/* Enable the configuration access */
	/* [한국어] 대상의 부모가 루트 버스면 바로 아래에 붙은 장치다. */
	if (pci_is_root_bus(bus->parent))
		/* [한국어] TYPE0 로 연다 — 브리지를 건너지 않는 직접 접근이다. */
		rcar_pci_write_reg(pcie, PCIECCTLR_CCIE | TYPE0, PCIECCTLR);
	/* [한국어] 더 아래라면 */
	else
		/* [한국어] TYPE1 로 연다. 브리지가 목적지까지 전달해 준다.
		 * PCIECCTLR_CCIE 는 config 접근 활성 비트다. */
		rcar_pci_write_reg(pcie, PCIECCTLR_CCIE | TYPE1, PCIECCTLR);

	/* Check for errors */
	/* [한국어] 방금 접근에서 Unsupported Request 가 났는지 본다 — 그 자리에 장치가 없다는 뜻이다. */
	if (rcar_pci_read_reg(pcie, PCIEERRFR) & UNSUPPORTED_REQUEST)
		/* [한국어] 없는 장치로 답한다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* Check for master and target aborts */
	/* [한국어] 자기 config space 의 상태 레지스터도 확인한다. */
	if (rcar_read_conf(pcie, RCONF(PCI_STATUS)) &
		/* [한국어] master abort 나 target abort 가 섰으면 응답이 없었다는 뜻이다. */
		(PCI_STATUS_REC_MASTER_ABORT | PCI_STATUS_REC_TARGET_ABORT))
		/* [한국어] 역시 없는 장치로 답한다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 오류가 없으면 실제 데이터를 주고받는다. */
	if (access_type == RCAR_PCI_ACCESS_READ)
		/* [한국어] 예외를 잡을 수 있는 판으로 읽는다. */
		ret = rcar_pci_read_reg_workaround(pcie, data, PCIECDR);
	else
		/* [한국어] 또는 쓴다. */
		ret = rcar_pci_write_reg_workaround(pcie, *data, PCIECDR);

	/* Disable the configuration access */
	/* [한국어] 접근을 반드시 닫는다. 열린 채로 두면 다음 접근이 엉킨다. */
	rcar_pci_write_reg(pcie, 0, PCIECCTLR);

	/* [한국어] 데이터 접근의 결과를 그대로 전한다. */
	return ret;
}

/* [한국어]
 * rcar_pcie_read_conf - pci_ops 의 읽기 콜백. 워드를 읽어 요청 크기로 잘라 준다
 *
 * @bus, @devfn: 대상 장치.   @where: 오프셋.   @size: 1, 2, 4 바이트.
 * @val: 결과를 담을 곳.
 * @return: PCIBIOS_* 코드.
 *
 * 하드웨어는 32비트 단위로만 읽으므로, 워드를 통째로 읽어 온 뒤 요청 크기에
 * 맞게 잘라 낸다.
 *   size==1 : where 의 하위 2비트가 가리키는 바이트로 밀고 0xff 로 자른다.
 *   size==2 : where & 2 로 상·하위 반워드를 고르고 0xffff 로 자른다.
 *             2로 마스킹하는 이유는 16비트 접근이 짝수 오프셋만 허용되기 때문이다.
 *   size==4 : 그대로 둔다.
 *
 * 실패하면 자르는 단계를 건너뛰고 곧바로 돌려준다. 아래 계층이 이미
*val 에 오류 응답 값을 채워 두었기 때문이다.
 *
 * 실행 컨텍스트: 아토믹. pci_lock 을 쥔 상태.
 *
 * 호출 체인:  PCI 코어(access.c) → [이 함수] → rcar_pcie_config_access()
 */
static int rcar_pcie_read_conf(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 *val)
{
	/* [한국어] rcar_pcie_enable() 이 bridge->sysdata 에 꽂아 둔 이 드라이버 상태를 되찾는다. */
	struct rcar_pcie_host *host = bus->sysdata;
	/* [한국어] 하위 호출 결과. */
	int ret;

	/* [한국어] 크기와 무관하게 일단 32비트 워드로 읽어 온다. */
	ret = rcar_pcie_config_access(host, RCAR_PCI_ACCESS_READ,
				      bus, devfn, where, val);
	/* [한국어] 실패했으면 자르지 않고 그대로 전한다 — 아래 계층이 이미 *val 에
	 * 오류 응답 값을 채워 두었기 때문이다. */
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;

	/* [한국어] 1바이트 요청이면 */
	if (size == 1)
		/* [한국어] 오프셋 하위 2비트가 가리키는 바이트로 밀고 0xff 로 자른다. */
		*val = (*val >> (BITS_PER_BYTE * (where & 3))) & 0xff;
	/* [한국어] 2바이트 요청이면 */
	else if (size == 2)
		/* [한국어] where & 2 로 상·하위 반워드를 고른다. 2로 마스킹하는 이유는 16비트 접근이
		 * 짝수 오프셋만 허용되기 때문이다. 4바이트면 자르지 않고 그대로 둔다. */
		*val = (*val >> (BITS_PER_BYTE * (where & 2))) & 0xffff;

	/* [한국어] 디버그 빌드에서 접근 내역을 남긴다. */
	dev_dbg(&bus->dev, "pcie-config-read: bus=%3d devfn=0x%04x where=0x%04x size=%d val=0x%08x\n",
		bus->number, devfn, where, size, *val);

	/* [한국어] 여기까지 왔으면 성공이다. */
	return ret;
}

/* Serialization is provided by 'pci_lock' in drivers/pci/access.c */
/* [한국어]
 * rcar_pcie_write_conf - pci_ops 의 쓰기 콜백. 읽고-고쳐-쓰기로 부분 쓰기를 흉내 낸다
 *
 * @bus, @devfn: 대상 장치.   @where: 오프셋.   @size: 1, 2, 4 바이트.
 * @val: 쓸 값.
 * @return: PCIBIOS_* 코드.
 *
 * 하드웨어가 32비트 단위로만 쓸 수 있어서, 1바이트나 2바이트 요청은
 * 같은 워드를 먼저 읽어 해당 자리만 바꾼 뒤 통째로 되쓴다. 그래서 쓰기인데도
 * 맨 앞에 RCAR_PCI_ACCESS_READ 호출이 있다.
 *
 * 자리 계산은 읽기 쪽과 대칭이다 - 해당 폭의 마스크를 시프트해 지우고
 * (data &= ~(0xff << shift)) 새 값을 같은 자리에 얹는다. size==4 면 읽어 온
 * 값을 버리고 그대로 덮어쓴다.
 *
 * 이 방식에는 원자성이 없다. 읽기와 쓰기 사이에 장치가 스스로 다른 비트를
 * 바꾸면 그 변경이 사라진다. 다만 config space 에서 그런 일이 드물고
 * pci_lock 이 소프트웨어 쪽 경쟁은 막아 준다.
 *
 * 실행 컨텍스트: 아토믹. 상류 주석대로 직렬화는 pci_lock 이 제공한다.
 *
 * 호출 체인:  PCI 코어(access.c) → [이 함수] → rcar_pcie_config_access() (읽기 후 쓰기)
 */
static int rcar_pcie_write_conf(struct pci_bus *bus, unsigned int devfn,
				int where, int size, u32 val)
{
	/* [한국어] 드라이버 상태를 되찾는다. */
	struct rcar_pcie_host *host = bus->sysdata;
	/* [한국어] 부분 쓰기에서 자리로 밀 비트 수. */
	unsigned int shift;
	/* [한국어] 읽어 와서 고칠 워드. */
	u32 data;
	/* [한국어] 하위 호출 결과. */
	int ret;

	/* [한국어] 쓰기인데 읽기로 시작한다. 하드웨어가 32비트 단위로만 쓸 수 있어,
	 * 1·2바이트 요청은 같은 워드를 읽어 해당 자리만 바꿔야 하기 때문이다. */
	ret = rcar_pcie_config_access(host, RCAR_PCI_ACCESS_READ,
				      bus, devfn, where, &data);
	/* [한국어] 읽기가 실패하면 쓸 수 없다. */
	if (ret != PCIBIOS_SUCCESSFUL)
		/* [한국어] errno 를 전한다. */
		return ret;

	/* [한국어] 디버그 빌드에서 접근 내역을 남긴다. */
	dev_dbg(&bus->dev, "pcie-config-write: bus=%3d devfn=0x%04x where=0x%04x size=%d val=0x%08x\n",
		bus->number, devfn, where, size, val);

	/* [한국어] 1바이트 요청. */
	if (size == 1) {
		/* [한국어] 오프셋 하위 2비트로 자리를 구하고, */
		shift = BITS_PER_BYTE * (where & 3);
		/* [한국어] 그 자리의 8비트를 지운 뒤 */
		data &= ~(0xff << shift);
		/* [한국어] 새 값을 얹는다. */
		data |= ((val & 0xff) << shift);
	/* [한국어] 2바이트 요청. */
	} else if (size == 2) {
		/* [한국어] 짝수 오프셋 기준으로 자리를 구하고, */
		shift = BITS_PER_BYTE * (where & 2);
		/* [한국어] 그 자리의 16비트를 지운 뒤 */
		data &= ~(0xffff << shift);
		/* [한국어] 새 값을 얹는다. */
		data |= ((val & 0xffff) << shift);
	/* [한국어] 4바이트면 */
	} else
		/* [한국어] 읽어 온 값을 버리고 통째로 덮어쓴다. */
		data = val;

	/* [한국어] 고친 워드를 되쓴다. 읽기와 쓰기 사이에 원자성이 없지만, config space 에서
	 * 장치가 스스로 비트를 바꾸는 일이 드물고 pci_lock 이 소프트웨어 경쟁은 막는다. */
	ret = rcar_pcie_config_access(host, RCAR_PCI_ACCESS_WRITE,
				      bus, devfn, where, &data);

	/* [한국어] 쓰기 결과를 전한다. */
	return ret;
}

/* [한국어] PCI 코어가 config 를 읽고 쓸 때 부를 콜백 표.
 * rcar_pcie_enable() 이 bridge->ops 에 꽂는다. */
static struct pci_ops rcar_pcie_ops = {
	/* [한국어] 읽기 콜백. */
	.read	= rcar_pcie_read_conf,
	/* [한국어] 쓰기 콜백. 이 둘이 이 드라이버와 PCI 코어의 유일한 상시 접점이다. */
	.write	= rcar_pcie_write_conf,
};

/* [한국어]
 * rcar_pcie_force_speedup - 2.5GT/s 로 붙은 링크를 5.0GT/s 로 끌어올린다
 *
 * @pcie: 컨트롤러.   @return: 없음. 실패해도 로그만 남기고 진행한다.
 *
 * 링크 훈련은 보통 2.5GT/s(Gen1)로 먼저 붙고, 속도 변경은 별도로 지시해야
 * 한다. 이 함수가 그 지시를 넣고 결과를 기다린다.
 *
 * 먼저 두 가지를 확인해 할 일이 없으면 빠진다. MACS2R 의 LINK_SPEED 가
 * 5.0GT/s 를 지원한다고 말하지 않으면 올릴 수 없고, MACCTLR 에 SPEED_CHANGE
 * 가 이미 서 있으면 다른 주체가 진행 중이다. MACSR 이 이미 5.0GT/s 면
 * 곧바로 done 으로 가서 현재 속도만 찍는다.
 *
 * 절차는 넷이다.
 *   1) 목표 속도를 5.0GT/s 로 (EXPCAP(12) 의 Link Status 필드)
 *   2) 변경 사유를 "의도된 것" 으로 (MACCGSPSETR 의 SPCNGRSN 을 0 으로)
 *   3) 이전 결과 비트(SPCHGFIN/SPCHGSUC/SPCHGFAIL)를 지운다 - 1 을 써서 지우는
 *      방식이라 읽은 값을 그대로 되쓴다
 *   4) MACCTLR 의 SPEED_CHANGE 를 세워 시작
 *
 * 그다음 SPCHGFIN(완료)이 설 때까지 1ms 씩 최대 1000회 기다린다. 완료되면
 * 결과 비트를 지우고 SPCHGFAIL 여부로 성패를 판단한다. 실패하거나 시간이
 * 초과돼도 오류를 위로 전하지 않는다 - 2.5GT/s 로도 동작은 하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 잠든다.
 *
 * 호출 체인:  rcar_pcie_hw_enable() → [이 함수] → rcar_rmw32() / msleep()
 */
static void rcar_pcie_force_speedup(struct rcar_pcie *pcie)
{
	/* [한국어] 로그용 device. */
	struct device *dev = pcie->dev;
	/* [한국어] 완료를 기다릴 최대 반복 횟수. 1ms 씩이므로 최대 1초다. */
	unsigned int timeout = 1000;
	/* [한국어] MAC 상태 레지스터 값. */
	u32 macsr;

	/* [한국어] 하드웨어가 5.0GT/s 를 지원한다고 말하지 않으면 올릴 수 없다.
	 * MACS2R 의 LINK_SPEED 필드(16번 비트부터 4비트)를 본다. */
	if ((rcar_pci_read_reg(pcie, MACS2R) & LINK_SPEED) != LINK_SPEED_5_0GTS)
		/* [한국어] 할 일 없이 물러난다. */
		return;

	/* [한국어] 이미 속도 변경이 진행 중이면 끼어들면 안 된다. */
	if (rcar_pci_read_reg(pcie, MACCTLR) & SPEED_CHANGE) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "Speed change already in progress\n");
		/* [한국어] 물러난다. */
		return;
	}

	/* [한국어] 현재 링크 속도를 읽는다. */
	macsr = rcar_pci_read_reg(pcie, MACSR);
	/* [한국어] 이미 5.0GT/s 면 */
	if ((macsr & LINK_SPEED) == LINK_SPEED_5_0GTS)
		/* [한국어] 올릴 것이 없다. 아래 done 으로 가서 현재 속도만 찍는다. */
		goto done;

	/* Set target link speed to 5.0 GT/s */
	/* [한국어] 목표 속도를 5.0GT/s 로 지정한다. EXPCAP(12) 는 PCIe capability 의
	 * Link Status 워드 자리다. */
	rcar_rmw32(pcie, EXPCAP(12), PCI_EXP_LNKSTA_CLS,
		   PCI_EXP_LNKSTA_CLS_5_0GB);

	/* Set speed change reason as intentional factor */
	/* [한국어] 속도 변경 사유를 "의도된 것" 으로 표시한다. SPCNGRSN 을 0 으로 만든다. */
	rcar_rmw32(pcie, MACCGSPSETR, SPCNGRSN, 0);

	/* Clear SPCHGFIN, SPCHGSUC, and SPCHGFAIL */
	/* [한국어] 이전 시도의 결과 비트가 남아 있으면 */
	if (macsr & (SPCHGFIN | SPCHGSUC | SPCHGFAIL))
		/* [한국어] 읽은 값을 그대로 되써서 지운다(1 을 쓰면 지워지는 방식).
		 * 남겨 두면 이번 시도의 완료 판정이 즉시 참이 되어 버린다. */
		rcar_pci_write_reg(pcie, macsr, MACSR);

	/* Start link speed change */
	/* [한국어] MACCTLR 의 SPEED_CHANGE 를 세워 변경을 시작한다. */
	rcar_rmw32(pcie, MACCTLR, SPEED_CHANGE, SPEED_CHANGE);

	/* [한국어] 완료될 때까지 최대 1000회 돈다. */
	while (timeout--) {
		/* [한국어] 상태를 다시 읽는다. */
		macsr = rcar_pci_read_reg(pcie, MACSR);
		/* [한국어] 완료 비트가 섰으면 */
		if (macsr & SPCHGFIN) {
			/* Clear the interrupt bits */
			/* [한국어] 결과 비트들을 지우고, */
			rcar_pci_write_reg(pcie, macsr, MACSR);

			/* [한국어] 실패 비트가 함께 서 있으면 */
			if (macsr & SPCHGFAIL)
				/* [한국어] 그 사실을 남긴다. 다만 오류를 위로 전하지는 않는다 — 2.5GT/s 로도 동작한다. */
				dev_err(dev, "Speed change failed\n");

			/* [한국어] 성패와 무관하게 마무리로 간다. */
			goto done;
		}

		/* [한국어] 1ms 잠들고 다시 확인한다. */
		msleep(1);
	}

	/* [한국어] 1초 안에 끝나지 않았다. */
	dev_err(dev, "Speed change timed out\n");

/* [한국어] 현재 속도를 찍는 공통 마무리. */
done:
	/* [한국어] 마지막으로 읽은 macsr 로 판단한다. */
	dev_info(dev, "Current link speed is %s GT/s\n",
		 /* [한국어] 5.0GT/s 가 아니면 2.5GT/s 로 본다 — 이 하드웨어가 지원하는 두 속도뿐이다. */
		 (macsr & LINK_SPEED) == LINK_SPEED_5_0GTS ? "5" : "2.5");
}

/* [한국어]
 * rcar_pcie_hw_enable - 속도를 올리고 DT 의 창들을 하드웨어에 반영한다
 *
 * @host: 컨트롤러 상태.   @return: 없음.
 *
 * 링크가 이미 서 있는 상태에서 부르는 마무리 단계다. 두 가지를 한다.
 *   1) rcar_pcie_force_speedup() 으로 5.0GT/s 시도
 *   2) 브리지의 windows 목록을 훑어 I/O 와 MEM 창을 바깥 방향 창으로 연다
 *
 * 창 번호 i 는 실제로 연 창만 세어 올리므로, 목록에 flags 가 빈 항목이
 * 섞여 있어도 하드웨어 창 번호가 비지 않는다. 하드웨어 창은
 * RCAR_PCI_MAX_RESOURCES(4)개뿐이라 번호를 아껴 써야 한다.
 *
 * 다만 이 루프에는 i 가 4 를 넘는지 확인하는 검사가 없다. DT 가 창을 다섯 개
 * 이상 기술하면 rcar_pcie_set_outbound() 가 PCIEPALR(4) 처럼 범위를 벗어난
 * 오프셋에 쓰게 된다. 코드는 고치지 않고 관찰만 적어 둔다.
 *
 * probe 와 resume 양쪽에서 불린다 - 절전에서 돌아오면 창 설정이 날아가
 * 다시 써 넣어야 하기 때문이다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  rcar_pcie_enable() / rcar_pcie_resume() → [이 함수]
 *               → rcar_pcie_force_speedup() → rcar_pcie_set_outbound() [pcie-rcar.c:58]
 */
static void rcar_pcie_hw_enable(struct rcar_pcie_host *host)
{
	/* [한국어] 레지스터 접근에 쓸 공통 구조체. */
	struct rcar_pcie *pcie = &host->pcie;
	/* [한국어] private 영역 주소에서 그것을 품은 pci_host_bridge 를 역산한다.
	 * devm_pci_alloc_host_bridge() 가 둘을 한 덩어리로 할당했기에 가능하다. */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(host);
	/* [한국어] 창 목록을 훑을 커서. */
	struct resource_entry *win;
	/* [한국어] 선언만 되고 쓰이지 않는 지역 목록이다. 코드는 고치지 않고 관찰만 적어 둔다. */
	LIST_HEAD(res);
	/* [한국어] 실제로 연 하드웨어 창 번호. 건너뛴 항목은 세지 않는다. */
	int i = 0;

	/* Try setting 5 GT/s link speed */
	/* [한국어] 먼저 링크 속도를 5.0GT/s 로 올려 본다. */
	rcar_pcie_force_speedup(pcie);

	/* Setup PCI resources */
	/* [한국어] 브리지가 보유한 바깥 방향 창 목록을 훑는다. */
	resource_list_for_each_entry(win, &bridge->windows) {
		/* [한국어] 이 항목이 가리키는 자원. */
		struct resource *res = win->res;

		/* [한국어] flags 가 비어 있으면 유효한 창이 아니다. */
		if (!res->flags)
			/* [한국어] 건너뛴다. 이때 i 를 올리지 않으므로 하드웨어 창 번호가 비지 않는다. */
			continue;

		/* [한국어] 자원 종류로 갈린다. */
		switch (resource_type(res)) {
		/* [한국어] I/O 창과 */
		case IORESOURCE_IO:
		/* [한국어] 메모리 창만 하드웨어 창으로 연다. 버스 번호 자원 등은 대상이 아니다. */
		case IORESOURCE_MEM:
			/* [한국어] PCIEPALR/PAUR/PAMR/PTCTLR 네 레지스터를 채운다(pcie-rcar.c:58).
			 * 다만 여기에는 i 가 RCAR_PCI_MAX_RESOURCES(4)를 넘는지 확인하는 검사가 없다.
			 * DT 가 창을 다섯 개 이상 기술하면 범위를 벗어난 오프셋에 쓰게 된다.
			 * 코드는 고치지 않고 관찰만 적어 둔다. */
			rcar_pcie_set_outbound(pcie, i, win);
			/* [한국어] 다음 하드웨어 창 번호로. */
			i++;
			break;
		}
	}
}

/* [한국어]
 * rcar_pcie_enable - 창을 열고 PCI 코어에 열거를 넘긴다
 *
 * @host: 컨트롤러 상태.
 * @return: pci_host_probe() 의 결과. 0 성공, 음수 errno 실패.
 *
 * 하드웨어 준비를 마친 뒤 소프트웨어 쪽을 연결하는 마지막 단계다.
 *   1) rcar_pcie_hw_enable() 로 속도와 창을 반영
 *   2) PCI_REASSIGN_ALL_BUS 를 전역 플래그에 추가 - 펌웨어가 버스 번호를
 *      배정해 두지 않는 환경이므로 커널이 전부 다시 매기게 한다
 *   3) 브리지에 sysdata(이 드라이버 상태)와 ops(config 콜백)를 꽂는다.
 *      sysdata 로 넣어 둔 host 를 config 콜백이 bus->sysdata 로 되찾는다
 *   4) pci_host_probe() 로 버스 스캔을 시작
 *
 * 이 시점부터 PCI 코어가 주도권을 갖고, 이 드라이버는 콜백으로만 불린다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  rcar_pcie_probe() → [이 함수] → pci_host_probe() [drivers/pci/probe.c]
 */
static int rcar_pcie_enable(struct rcar_pcie_host *host)
{
	/* [한국어] private 영역에서 브리지를 역산한다. */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(host);

	/* [한국어] 속도를 올리고 창을 연다. */
	rcar_pcie_hw_enable(host);

	/* [한국어] 버스 번호를 커널이 전부 다시 매기게 한다. 펌웨어가 배정해 두지 않는
	 * 환경이라 기존 값을 신뢰할 수 없다. */
	pci_add_flags(PCI_REASSIGN_ALL_BUS);

	/* [한국어] config 콜백이 bus->sysdata 로 되찾을 드라이버 상태를 꽂는다. */
	bridge->sysdata = host;
	/* [한국어] config 접근 콜백 표를 꽂는다. 이 두 줄이 PCI 코어와 이 드라이버를 잇는다. */
	bridge->ops = &rcar_pcie_ops;

	/* [한국어] 버스 스캔을 시작한다. 이 시점부터 주도권이 PCI 코어로 넘어간다. */
	return pci_host_probe(bridge);
}

/* [한국어]
 * phy_wait_for_ack - H1 PHY 레지스터 접근이 받아들여지기를 기다린다
 *
 * @pcie: 컨트롤러.
 * @return: 0 성공, -ETIMEDOUT 은 10ms 안에 응답이 없는 경우.
 *
 * R-Car H1(1세대)의 PHY 는 별도 버스 뒤에 있어, 주소를 쓰면 하드웨어가
 * 처리를 마친 뒤 H1_PCIEPHYADRR 의 PHY_ACK 비트로 알린다. 그 비트를
 * 100us 간격으로 최대 100회 폴링한다.
 *
 * udelay 로 바쁘게 기다리는 이유는 PHY 초기화가 부팅 중 한 번뿐이고
 * 전체 대기가 10ms 를 넘지 않아 스케줄러를 부를 이유가 없기 때문이다.
 *
 * 시간이 초과되면 오류를 남기고 -ETIMEDOUT 을 돌려주지만, 유일한 호출자인
 * phy_write_reg() 가 그 값을 무시한다(그쪽 상류 주석이 이유를 밝힌다).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). 바쁜 대기라 잠들지는 않는다.
 *
 * 호출 체인:  phy_write_reg() → [이 함수] → rcar_pci_read_reg() / udelay()
 */
static int phy_wait_for_ack(struct rcar_pcie *pcie)
{
	/* [한국어] 로그용 device. */
	struct device *dev = pcie->dev;
	/* [한국어] 최대 100회. 100us 간격이므로 상한이 10ms 다. */
	unsigned int timeout = 100;

	/* [한국어] ack 가 올 때까지 돈다. */
	while (timeout--) {
		/* [한국어] H1 PHY 는 처리를 마치면 주소 레지스터의 PHY_ACK 비트로 알린다. */
		if (rcar_pci_read_reg(pcie, H1_PCIEPHYADRR) & PHY_ACK)
			/* [한국어] 받았으면 성공. */
			return 0;

		/* [한국어] 100us 바쁘게 기다린다. PHY 초기화는 부팅 중 한 번뿐이고 전체 대기가
		 * 10ms 를 넘지 않아 스케줄러를 부를 이유가 없다. */
		udelay(100);
	}

	/* [한국어] 시간이 초과됐음을 남긴다. */
	dev_err(dev, "Access to PCIe phy timed out\n");

	/* [한국어] 다만 유일한 호출자가 이 값을 무시한다(그쪽 상류 주석이 이유를 밝힌다). */
	return -ETIMEDOUT;
}

/* [한국어]
 * phy_write_reg - H1 PHY 의 레지스터 하나에 값을 써 넣는다
 *
 * @pcie: 컨트롤러.   @rate: 데이터 레이트 선택(1비트).   @addr: PHY 레지스터 주소(8비트).
 * @lane: 레인 번호(4비트).   @data: 쓸 32비트 값.
 * @return: 없음.
 *
 * 주소 워드를 비트 필드로 조립해 넣는 방식이다. pcie-rcar.h 의 정의를
 * 근거로 하면 WRITE_CMD 는 BIT(16), RATE_POS 는 12, LANE_POS 는 8,
 * ADR_POS 는 0 이다. 각 필드를 자기 폭으로 마스킹한 뒤 자리로 밀어 OR 한다.
 *
 * 절차가 두 번 반복된다. 먼저 데이터를 H1_PCIEPHYDOUTR 에 넣고 주소를
 * H1_PCIEPHYADRR 에 써서 명령을 내고 ack 를 기다린 뒤, 두 레지스터를 모두
 * 0 으로 되돌리고 다시 ack 를 기다린다. 뒤쪽이 명령을 거두는 절차이며,
 * 남겨 두면 다음 명령과 겹칠 수 있다.
 *
 * ack 실패를 무시하는 것이 의도적이다. 상류 주석이 두 번 다 "오류는
 * 데이터 링크가 내려가면 어차피 드러나므로 무시한다" 고 적어 두었다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  rcar_pcie_phy_init_h1() → [이 함수] → phy_wait_for_ack()
 */
static void phy_write_reg(struct rcar_pcie *pcie,
			  unsigned int rate, u32 addr,
			  unsigned int lane, u32 data)
{
	/* [한국어] 주소 워드를 비트 필드로 조립할 변수. */
	u32 phyaddr;

	/* [한국어] 쓰기 명령 비트(BIT(16)). */
	phyaddr = WRITE_CMD |
		/* [한국어] 데이터 레이트 1비트를 RATE_POS(12)로 민다. */
		((rate & 1) << RATE_POS) |
		/* [한국어] 레인 번호 4비트를 LANE_POS(8)로 민다. */
		((lane & 0xf) << LANE_POS) |
		/* [한국어] PHY 레지스터 주소 8비트를 ADR_POS(0)로 민다 — 시프트가 0 이지만
		 * 위 세 줄과 형식을 맞춘 표기다. */
		((addr & 0xff) << ADR_POS);

	/* Set write data */
	/* [한국어] 데이터를 먼저 넣고, */
	rcar_pci_write_reg(pcie, data, H1_PCIEPHYDOUTR);
	/* [한국어] 주소를 써서 명령을 낸다. 순서가 반대면 하드웨어가 옛 데이터를 쓴다. */
	rcar_pci_write_reg(pcie, phyaddr, H1_PCIEPHYADRR);

	/* Ignore errors as they will be dealt with if the data link is down */
	/* [한국어] 완료를 기다린다. 실패는 무시한다(상류 주석). */
	phy_wait_for_ack(pcie);

	/* Clear command */
	/* [한국어] 데이터 레지스터를 되돌리고, */
	rcar_pci_write_reg(pcie, 0, H1_PCIEPHYDOUTR);
	/* [한국어] 주소 레지스터도 되돌려 명령을 거둔다. 남겨 두면 다음 명령과 겹친다. */
	rcar_pci_write_reg(pcie, 0, H1_PCIEPHYADRR);

	/* Ignore errors as they will be dealt with if the data link is down */
	/* [한국어] 거두기가 반영되기를 기다린다. 역시 실패는 무시한다. */
	phy_wait_for_ack(pcie);
}

/* [한국어]
 * rcar_pcie_hw_init - 컨트롤러를 루트 컴플렉스로 세우고 링크를 훈련시킨다
 *
 * @pcie: 컨트롤러.
 * @return: 0 성공. -ETIMEDOUT 은 PHY 준비 또는 링크 확립 실패.
 *
 * 이 파일에서 하드웨어를 가장 많이 만지는 함수다. 순서 하나하나가 의미가 있다.
 *
 *   1) PCIETCTLR 에 0 - 진행 중인 것을 멈추고 초기화를 시작한다.
 *   2) PCIEMSR 에 1 - 동작 모드를 정한다. 엔드포인트 판(pcie-rcar-ep.c)이
 *      같은 자리에 0 을 쓰는 것과 대비된다. 이 한 줄이 호스트와 EP 를 가른다.
 *   3) PHY 가 준비되기를 기다린다(PCIEPHYSR 의 PHYRDY).
 *   4) 자기 config space 를 PCI-to-PCI 브리지처럼 꾸민다. IDSETR1 에 클래스
 *      코드를 쓰면 상류 주석대로 하드웨어가 알아서 전파하므로 quirk 가 필요 없다.
 *   5) Secondary/Subordinate 버스 번호를 1 로 채운다. 상류 주석대로 실제로
 *      쓰이지는 않지만, 비워 두면 코어가 이 브리지를 고장 난 것으로 판정한다.
 *   6) PCIe capability 를 만들어 넣는다 - capability ID, 포트 종류를
 *      Root Port 로, 헤더 종류를 bridge 로.
 *   7) DLLLARC(데이터 링크 활성 보고)를 켠다. 이것이 있어야 상위 계층이
 *      링크 상태를 읽을 수 있다.
 *   8) 물리 슬롯 번호 0, 완료 타이머 상한 50ms, capability 목록 종료 표시.
 *   9) MSI 를 쓰는 빌드면 PCIEMSITXR 에 0x801f0000 을 쓴다. 이 상수의 비트별
 *      의미는 pcie-rcar.h 에 정의가 없어 이 트리에서 근거를 확인하지 못했다.
 *  10) MACCTLR 에 MACCTLR_INIT_VAL(= LTSMDIS | MACCTLR_NFTS_MASK)을 쓴다.
 *  11) PCIETCTLR 에 CFINIT 을 써서 링크 훈련을 시작하고, 데이터 링크가
 *      활성화될 때까지 기다린다. 카드가 없으면 여기서 시간 초과가 난다.
 *  12) INTx 인터럽트를 켠다(PCIEINTXR 의 8번 비트부터 4비트). 이 필드의
 *      의미도 헤더에 정의가 없어 근거를 확인하지 못했다.
 *  13) wmb() 로 앞선 쓰기들이 순서대로 도달하도록 막는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). 안쪽에서 잠든다.
 *
 * 호출 체인:  rcar_pcie_probe() → [이 함수]
 *               → rcar_pcie_wait_for_phyrdy() [pcie-rcar.c:32]
 *               → rcar_pcie_wait_for_dl() [pcie-rcar.c:44]
 */
static int rcar_pcie_hw_init(struct rcar_pcie *pcie)
{
	/* [한국어] 하위 호출 결과. */
	int err;

	/* Begin initialization */
	/* [한국어] 진행 중인 것을 멈추고 초기화를 시작한다. */
	rcar_pci_write_reg(pcie, 0, PCIETCTLR);

	/* Set mode */
	/* [한국어] 동작 모드를 정한다. 엔드포인트 판(pcie-rcar-ep.c)이 같은 자리에 0 을 쓰는
	 * 것과 대비되며, 이 한 줄이 호스트와 EP 를 가른다. */
	rcar_pci_write_reg(pcie, 1, PCIEMSR);

	/* [한국어] PHY 가 준비될 때까지 기다린다(PCIEPHYSR 의 PHYRDY, pcie-rcar.c:32). */
	err = rcar_pcie_wait_for_phyrdy(pcie);
	/* [한국어] 준비되지 않으면 */
	if (err)
		/* [한국어] 더 진행할 수 없다. */
		return err;

	/*
	 * Initial header for port config space is type 1, set the device
	 * class to match. Hardware takes care of propagating the IDSETR
	 * settings, so there is no need to bother with a quirk.
	 */
	/* [한국어] 자기 config space 의 클래스 코드를 PCI-to-PCI 브리지로 만든다.
	 * 8비트 미는 이유는 IDSETR1 의 클래스 필드가 그 자리이기 때문이다.
	 * 상류 주석대로 하드웨어가 알아서 전파하므로 별도 quirk 가 필요 없다. */
	rcar_pci_write_reg(pcie, PCI_CLASS_BRIDGE_PCI_NORMAL << 8, IDSETR1);

	/*
	 * Setup Secondary Bus Number & Subordinate Bus Number, even though
	 * they aren't used, to avoid bridge being detected as broken.
	 */
	/* [한국어] Secondary/Subordinate 버스 번호를 1 로 채운다. 상류 주석대로 실제로
	 * 쓰이지는 않지만, 비워 두면 PCI 코어가 이 브리지를 고장 난 것으로 판정한다. */
	rcar_rmw32(pcie, RCONF(PCI_SECONDARY_BUS), 0xff, 1);
	rcar_rmw32(pcie, RCONF(PCI_SUBORDINATE_BUS), 0xff, 1);

	/* Initialize default capabilities. */
	/* [한국어] capability ID 를 PCIe 로 적어 capability 목록의 시작을 만든다. */
	rcar_rmw32(pcie, REXPCAP(0), 0xff, PCI_CAP_ID_EXP);
	rcar_rmw32(pcie, REXPCAP(PCI_EXP_FLAGS),
		/* [한국어] 포트 종류를 Root Port 로 적는다. 4비트 미는 것은 PCI_EXP_FLAGS 안에서
		 * Type 필드가 그 자리이기 때문이다. */
		PCI_EXP_FLAGS_TYPE, PCI_EXP_TYPE_ROOT_PORT << 4);
	rcar_rmw32(pcie, RCONF(PCI_HEADER_TYPE), PCI_HEADER_TYPE_MASK,
		/* [한국어] 헤더 종류를 bridge 로 바꾼다. 이래야 PCI 코어가 이 장치를 브리지로 다룬다. */
		PCI_HEADER_TYPE_BRIDGE);

	/* [한국어] 데이터 링크 활성 보고를 켠다. 이것이 있어야 상위 계층이 링크 상태를 읽는다. */
	/* Enable data link layer active state reporting */
	rcar_rmw32(pcie, REXPCAP(PCI_EXP_LNKCAP), PCI_EXP_LNKCAP_DLLLARC,
		PCI_EXP_LNKCAP_DLLLARC);

	/* [한국어] 물리 슬롯 번호를 0 으로. 슬롯이 하나뿐인 구성이다. */
	/* Write out the physical slot number = 0 */
	rcar_rmw32(pcie, REXPCAP(PCI_EXP_SLTCAP), PCI_EXP_SLTCAP_PSN, 0);

	/* [한국어] 완료 타이머 상한을 50ms 로. TLCTLR + 1 로 한 바이트 건너뛰어 접근하는데,
	 * rcar_rmw32() 가 오프셋 하위 2비트로 바이트 자리를 계산해 주기 때문이다. */
	/* Set the completion timer timeout to the maximum 50ms. */
	rcar_rmw32(pcie, TLCTLR + 1, 0x3f, 50);

	/* [한국어] capability 목록을 여기서 끝낸다(Next Capability Offset 을 0 으로). */
	/* Terminate list of capabilities (Next Capability Offset=0) */
	rcar_rmw32(pcie, RVCCAP(0), 0xfff00000, 0);

	/* Enable MSI */
	/* [한국어] MSI 를 쓰는 빌드에서만 이 레지스터를 세운다. */
	if (IS_ENABLED(CONFIG_PCI_MSI))
		/* [한국어] 이 상수의 비트별 의미는 pcie-rcar.h 에 정의가 없어 이 트리에서 근거를
		 * 확인하지 못했다. 이름과 위치로 보아 MSI 송신 설정으로 보인다. */
		rcar_pci_write_reg(pcie, 0x801f0000, PCIEMSITXR);

	/* [한국어] MACCTLR_INIT_VAL 은 LTSMDIS | MACCTLR_NFTS_MASK 다(pcie-rcar.h). */
	rcar_pci_write_reg(pcie, MACCTLR_INIT_VAL, MACCTLR);

	/* Finish initialization - establish a PCI Express link */
	rcar_pci_write_reg(pcie, CFINIT, PCIETCTLR);

	/* This will timeout if we don't have a link. */
	err = rcar_pcie_wait_for_dl(pcie);
	if (err)
		/* [한국어] 링크가 서지 않았다. 카드가 없을 수도 있어 호출자가 오류 수준을 낮춰 다룬다. */
		return err;

	/* Enable INTx interrupts */
	rcar_rmw32(pcie, PCIEINTXR, 0, 0xF << 8);

	wmb();

	return 0;
}

/* [한국어]
 * rcar_pcie_phy_init_h1 - R-Car H1(1세대) PHY 를 초기화한다
 *
 * @host: 컨트롤러 상태.   @return: 항상 0.
 *
 * PHY 레지스터에 벤더가 제공한 설정값을 순서대로 써 넣는다. 각 상수의
 * 의미는 Renesas 의 비공개 PHY 문서에 있고 이 트리에는 근거가 없다 -
 * 값 하나하나가 무엇을 뜻하는지는 확인하지 못했다.
 *
 * 읽을 수 있는 구조만 적으면, 인자는 (rate, addr, lane, data) 순이고
 * 같은 addr 에 rate 0 과 1 로 두 번 쓰는 쌍이 여럿 보인다. 데이터 레이트별로
 * 따로 설정해야 하는 항목들로 보인다.
 *
 * of_device_get_match_data() 로 얻어져 host->phy_init_fn 에 꽂히는 세 함수
 * 중 하나이며, "renesas,pcie-r8a7779" compatible 에 묶여 있다.
 * 항상 0 을 돌려주므로 이 경로에서는 PHY 초기화가 실패하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  rcar_pcie_probe() → host->phy_init_fn → [이 함수] → phy_write_reg()
 */
static int rcar_pcie_phy_init_h1(struct rcar_pcie_host *host)
{
	struct rcar_pcie *pcie = &host->pcie;

	/* Initialize the phy */
	phy_write_reg(pcie, 0, 0x42, 0x1, 0x0EC34191);
	phy_write_reg(pcie, 1, 0x42, 0x1, 0x0EC34180);
	/* [한국어] 이하 H1 PHY 설정값이 이어진다. 인자는 (rate, addr, lane, data) 순이다.
	 * 같은 addr 에 rate 0 과 1 로 두 번 쓰는 쌍이 여럿 보이는데, 데이터 레이트별로
	 * 따로 설정해야 하는 항목들로 보인다. 각 상수의 의미는 Renesas 의 비공개
	 * PHY 문서에 있고 이 트리에는 근거가 없어 확인하지 못했다. */
	phy_write_reg(pcie, 0, 0x43, 0x1, 0x00210188);
	phy_write_reg(pcie, 1, 0x43, 0x1, 0x00210188);
	phy_write_reg(pcie, 0, 0x44, 0x1, 0x015C0014);
	phy_write_reg(pcie, 1, 0x44, 0x1, 0x015C0014);
	/* [한국어] rate 1 전용 설정이 이어진다. */
	phy_write_reg(pcie, 1, 0x4C, 0x1, 0x786174A0);
	phy_write_reg(pcie, 1, 0x4D, 0x1, 0x048000BB);
	/* [한국어] 다시 rate 0 전용 설정. */
	phy_write_reg(pcie, 0, 0x51, 0x1, 0x079EC062);
	phy_write_reg(pcie, 0, 0x52, 0x1, 0x20000000);
	phy_write_reg(pcie, 1, 0x52, 0x1, 0x20000000);
	phy_write_reg(pcie, 1, 0x56, 0x1, 0x00003806);
/* [한국어] 여기서 한 줄 비어 있는 것은 상류가 설정 묶음을 나눈 자리다. */

	phy_write_reg(pcie, 0, 0x60, 0x1, 0x004B03A5);
	/* [한국어] 뒤쪽 세 줄은 rate 0 전용 설정이다. */
	phy_write_reg(pcie, 0, 0x64, 0x1, 0x3F0F1F0F);
	phy_write_reg(pcie, 0, 0x66, 0x1, 0x00008000);

	return 0;
}

/* [한국어]
 * rcar_pcie_phy_init_gen2 - R-Car 2세대 PHY 를 초기화한다
 *
 * @host: 컨트롤러 상태.   @return: 항상 0.
 *
 * H1 과 달리 전용 주소/데이터/제어 레지스터 세 개(GEN2_PCIEPHYADDR,
 * GEN2_PCIEPHYDATA, GEN2_PCIEPHYCTRL)로 쓴다. 상류 주석이 출처를 밝혀
 * 두었다 - "R-Car Series, 2nd Generation User's Manual" 의 50.3.1 절.
 *
 * 패턴이 네 줄씩 두 묶음이다. 주소를 쓰고, 데이터를 쓰고, 제어 레지스터에
 * 1 을 썼다가 6 을 쓴다. 뒤의 두 값이 각각 무엇을 지시하는지는 pcie-rcar.h 에
 * 정의가 없어 이 트리에서 근거를 확인하지 못했다.
 *
 * 두 번째 묶음의 데이터에는 상류 주석이 붙어 있다 - DC 연결이고 종단
 * 저항이 없는 구성을 위한 값이라는 뜻이다.
 *
 * 세 compatible("renesas,pcie-r8a7790", "r8a7791", "pcie-rcar-gen2")이
 * 이 함수를 가리킨다.
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  rcar_pcie_probe() → host->phy_init_fn → [이 함수] → rcar_pci_write_reg()
 */
static int rcar_pcie_phy_init_gen2(struct rcar_pcie_host *host)
{
	struct rcar_pcie *pcie = &host->pcie;

	/*
	 * These settings come from the R-Car Series, 2nd Generation User's
	 * Manual, section 50.3.1 (2) Initialization of the physical layer.
	 */
	rcar_pci_write_reg(pcie, 0x000f0030, GEN2_PCIEPHYADDR);
	rcar_pci_write_reg(pcie, 0x00381203, GEN2_PCIEPHYDATA);
	/* [한국어] 제어 레지스터에 1 을 썼다가 */
	rcar_pci_write_reg(pcie, 0x00000001, GEN2_PCIEPHYCTRL);
	/* [한국어] 6 을 쓰는 것이 이 PHY 의 쓰기 절차다. 두 값이 각각 무엇을 지시하는지는
	 * pcie-rcar.h 에 정의가 없어 이 트리에서 근거를 확인하지 못했다. */
	rcar_pci_write_reg(pcie, 0x00000006, GEN2_PCIEPHYCTRL);

	rcar_pci_write_reg(pcie, 0x000f0054, GEN2_PCIEPHYADDR);
	/* The following value is for DC connection, no termination resistor */
	rcar_pci_write_reg(pcie, 0x13802007, GEN2_PCIEPHYDATA);
	rcar_pci_write_reg(pcie, 0x00000001, GEN2_PCIEPHYCTRL);
	/* [한국어] 둘째 묶음도 같은 절차로 마무리한다. */
	rcar_pci_write_reg(pcie, 0x00000006, GEN2_PCIEPHYCTRL);

	return 0;
}

/* [한국어]
 * rcar_pcie_phy_init_gen3 - 3세대 PHY 를 범용 PHY 프레임워크로 초기화한다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공, phy_init()/phy_power_on() 의 음수 errno.
 *
 * 앞의 두 세대와 성격이 완전히 다르다. 3세대에서는 PHY 가 별도 드라이버로
 * 분리되어 있어서, 이 함수는 레지스터를 직접 만지지 않고 범용 PHY API 만
 * 부른다. host->phy 는 rcar_pcie_get_resources() 가 devm_phy_optional_get()
 * 으로 받아 둔 것이다.
 *
 * 되돌리기가 정확하다. phy_power_on() 이 실패하면 앞서 성공한 phy_init() 을
 * phy_exit() 으로 되돌린 뒤 오류를 전한다. 이렇게 해 두면 호출자는 실패 시
 * PHY 에 대해 아무것도 하지 않아도 된다.
 *
 * "renesas,pcie-r8a7795" 와 "renesas,pcie-rcar-gen3" 이 이 함수를 가리킨다.
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  rcar_pcie_probe() → host->phy_init_fn → [이 함수]
 *               → phy_init() → phy_power_on()
 */
static int rcar_pcie_phy_init_gen3(struct rcar_pcie_host *host)
{
	int err;

	err = phy_init(host->phy);
	/* [한국어] PHY 초기화 실패. */
	if (err)
		/* [한국어] 그대로 전한다. 아직 전원을 켜지 않았으므로 되돌릴 것이 없다. */
		return err;

	err = phy_power_on(host->phy);
	/* [한국어] 전원 켜기에 실패했으면 */
	if (err)
		/* [한국어] 앞서 성공한 phy_init() 을 되돌린다. 이렇게 해 두면 호출자는 실패 시
		 * PHY 에 대해 아무것도 하지 않아도 된다. */
		phy_exit(host->phy);

	return err;
}

/* [한국어]
 * rcar_pcie_msi_irq - MSI 수신 인터럽트 핸들러
 *
 * @irq:  울린 IRQ 번호. 쓰지 않는다(두 IRQ 가 같은 핸들러를 공유한다).
 * @data: 등록 때 넘긴 rcar_pcie_host.
 * @return: IRQ_HANDLED 는 처리했음, IRQ_NONE 은 내 것이 아님.
 *
 * 컨트롤러가 MSI 를 받으면 PCIEMSIFR 의 해당 비트를 세우고 인터럽트를 낸다.
 * 이 핸들러가 그 비트들을 훑어 커널 IRQ 계층으로 넘긴다.
 *
 * IRQ_NONE 을 돌려주는 갈래가 중요하다. 상류 주석대로 이 IRQ 선은 MSI 와
 * INTx 가 공유하므로, 플래그 레지스터가 비어 있으면 INTx 쪽 사건이라는
 * 뜻이라 "내 것이 아니다" 라고 답해야 공유 IRQ 체계가 다음 핸들러를 부른다.
 *
 * 바깥 루프가 있는 이유는 처리 도중 새 MSI 가 도착할 수 있기 때문이다.
 * 안쪽 루프를 다 돈 뒤 레지스터를 다시 읽어 비어 있을 때까지 반복하므로,
 * 인터럽트를 놓치지 않는다.
 *
 * generic_handle_domain_irq() 가 0 이 아닌 값을 주면 그 hwirq 에 매핑된
 * 핸들러가 없다는 뜻이다. 그런 "모르는 MSI" 는 플래그 비트를 직접 1 로 써서
 * 지운다 - 그러지 않으면 같은 비트가 계속 서 있어 인터럽트 폭풍이 된다.
 * 정상 경로에서 비트를 지우는 일은 irq_chip 의 ack 콜백(rcar_msi_irq_ack)이
 * 맡는다.
 *
 * find_first_bit 에 32 를 주는 것은 INT_PCI_MSI_NR 과 같은 값이다.
 * 실행 컨텍스트: 인터럽트(IRQF_NO_THREAD 로 등록되어 스레드화되지 않는다).
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → generic_handle_domain_irq() → 각 MSI 핸들러
 */
static irqreturn_t rcar_pcie_msi_irq(int irq, void *data)
{
	struct rcar_pcie_host *host = data;
	struct rcar_pcie *pcie = &host->pcie;
	/* [한국어] MSI 상태 묶음. */
	struct rcar_msi *msi = &host->msi;
	/* [한국어] 로그용 device. */
	struct device *dev = pcie->dev;
	/* [한국어] MSI 대기 플래그 레지스터 값. find_first_bit 에 넘기려고 unsigned long 이다. */
	unsigned long reg;

	reg = rcar_pci_read_reg(pcie, PCIEMSIFR);
/* [한국어] 대기 중인 MSI 비트맵을 읽는다. */

	/* MSI & INTx share an interrupt - we only handle MSI here */
	if (!reg)
		return IRQ_NONE;

	while (reg) {
		/* [한국어] 가장 낮은 번호의 대기 벡터를 찾는다. 32 는 INT_PCI_MSI_NR 과 같은 값이다. */
		unsigned int index = find_first_bit(&reg, 32);
		/* [한국어] 도메인 전달 결과. */
		int ret;

		ret = generic_handle_domain_irq(msi->domain, index);
		/* [한국어] 매핑된 핸들러가 없는 "모르는 MSI" 였다면 */
		if (ret) {
			/* Unknown MSI, just clear it */
			dev_dbg(dev, "unexpected MSI\n");
			rcar_pci_write_reg(pcie, BIT(index), PCIEMSIFR);
		/* [한국어] 플래그 비트를 직접 지운다(바로 위). 그러지 않으면 같은 비트가 계속 서 있어
		 * 인터럽트 폭풍이 된다. 정상 경로에서 비트를 지우는 일은 ack 콜백이 맡는다. */
		}

		/* see if there's any more pending in this vector */
		reg = rcar_pci_read_reg(pcie, PCIEMSIFR);
	}

	return IRQ_HANDLED;
}

/* [한국어]
 * rcar_msi_irq_ack - 이 MSI 벡터의 대기 플래그를 지운다
 *
 * @d: IRQ 코어가 넘기는 irq_data. hwirq 가 벡터 번호다.
 * @return: 없음.
 *
 * PCIEMSIFR 의 해당 비트에 1 을 써서 지운다. 이 하드웨어의 상태 비트는
 * 1 을 쓰면 지워지는 방식이라, 읽고-고쳐-쓰기가 필요 없고 다른 비트를
 * 건드릴 위험도 없다. 그래서 락 없이 안전하다 - 아래 mask/unmask 가
 * 락을 잡는 것과 대비된다.
 *
 * edge 방식 인터럽트라 핸들러를 부르기 전에 플래그를 지워야 처리 도중
 * 도착한 다음 인터럽트를 놓치지 않는다. 그 순서는 handle_edge_irq() 가
 * 보장하며, rcar_msi_domain_alloc() 이 그 핸들러를 지정한다.
 *
 * msi_parent_ops 의 chip_flags 에 MSI_CHIP_FLAG_SET_ACK 가 들어 있어
 * 상위 MSI 계층이 이 ack 를 쓰도록 표시되어 있다.
 * 실행 컨텍스트: 인터럽트. 잠들지 않는다.
 *
 * 호출 체인:  IRQ 코어(handle_edge_irq) → [이 함수] → rcar_pci_write_reg()
 */
static void rcar_msi_irq_ack(struct irq_data *d)
{
	struct rcar_msi *msi = irq_data_get_irq_chip_data(d);
	struct rcar_pcie *pcie = &msi_to_host(msi)->pcie;
/* [한국어] 루프를 다시 돌기 전에 레지스터를 다시 읽는다. 처리 도중 도착한 새 MSI 를
 * 놓치지 않기 위해서다. */

	/* clear the interrupt */
	rcar_pci_write_reg(pcie, BIT(d->hwirq), PCIEMSIFR);
}

/* [한국어]
 * rcar_msi_irq_mask - 이 MSI 벡터를 마스크한다
 *
 * @d: IRQ 코어가 넘기는 irq_data.   @return: 없음.
 *
 * PCIEMSIIER(인터럽트 활성 레지스터)에서 해당 비트를 내린다. ack 와 달리
 * 읽고-고쳐-쓰기가 필요하다 - 이 레지스터는 32개 벡터의 활성 비트가 한
 * 워드에 모여 있어, 통째로 쓰면 다른 벡터의 설정을 지우기 때문이다.
 *
 * 그래서 raw_spinlock 인 msi->mask_lock 을 irqsave 로 잡는다. 서로 다른
 * 벡터의 mask/unmask 가 동시에 같은 워드를 고치면 갱신 하나가 사라진다.
 * raw_ 계열인 이유는 이 경로가 인터럽트 문맥에서 불려 잠들 수 없기 때문이다.
 *
 * scoped_guard 를 쓰므로 블록을 벗어날 때 락이 자동으로 풀린다.
 * 실행 컨텍스트: 인터럽트 또는 프로세스. 잠들지 않는다.
 *
 * 호출 체인:  IRQ 코어(disable_irq 등) → [이 함수] → rcar_pci_read_reg()/write_reg()
 */
static void rcar_msi_irq_mask(struct irq_data *d)
{
	struct rcar_msi *msi = irq_data_get_irq_chip_data(d);
	struct rcar_pcie *pcie = &msi_to_host(msi)->pcie;
	/* [한국어] 읽고-고쳐-쓸 활성 레지스터 값. */
	u32 value;

	scoped_guard(raw_spinlock_irqsave, &msi->mask_lock) {
		/* [한국어] 현재 활성 비트맵을 읽는다. */
		value = rcar_pci_read_reg(pcie, PCIEMSIIER);
		/* [한국어] 이 벡터의 비트만 내린다. */
		value &= ~BIT(d->hwirq);
		/* [한국어] 통째로 되쓴다. 락이 필요한 이유가 여기 있다 — 32개 벡터의 활성 비트가
		 * 한 워드에 모여 있어 동시에 고치면 갱신 하나가 사라진다. */
		rcar_pci_write_reg(pcie, value, PCIEMSIIER);
	/* [한국어] scoped_guard 블록이 끝나면서 락이 자동으로 풀린다. */
	}
}

/* [한국어]
 * rcar_msi_irq_unmask - 이 MSI 벡터의 마스크를 푼다
 *
 * @d: IRQ 코어가 넘기는 irq_data.   @return: 없음.
 *
 * 위 mask 의 짝이다. 같은 레지스터의 같은 비트를 내리는 대신 올린다.
 * 락을 잡는 이유와 방식도 동일하다.
 *
 * 두 함수를 나란히 두는 것이 의도적이다 - 한쪽만 고치는 실수를 막는다.
 * 실행 컨텍스트: 인터럽트 또는 프로세스. 잠들지 않는다.
 *
 * 호출 체인:  IRQ 코어(enable_irq 등) → [이 함수] → rcar_pci_read_reg()/write_reg()
 */
static void rcar_msi_irq_unmask(struct irq_data *d)
{
	struct rcar_msi *msi = irq_data_get_irq_chip_data(d);
	struct rcar_pcie *pcie = &msi_to_host(msi)->pcie;
	/* [한국어] 읽고-고쳐-쓸 활성 레지스터 값. */
	u32 value;

	scoped_guard(raw_spinlock_irqsave, &msi->mask_lock) {
		/* [한국어] 현재 활성 비트맵을 읽는다. */
		value = rcar_pci_read_reg(pcie, PCIEMSIIER);
		/* [한국어] 이 벡터의 비트를 올린다. mask 와 정확히 대칭이다. */
		value |= BIT(d->hwirq);
		/* [한국어] 통째로 되쓴다. */
		rcar_pci_write_reg(pcie, value, PCIEMSIIER);
	/* [한국어] 락 자동 해제. */
	}
}

/* [한국어]
 * rcar_compose_msi_msg - 장치가 MSI 를 보낼 주소와 데이터를 알려 준다
 *
 * @data: IRQ 코어가 넘기는 irq_data. hwirq 가 벡터 번호다.
 * @msg:  채워 줄 주소/데이터.
 * @return: 없음.
 *
 * PCI 장치는 "특정 주소에 특정 값을 쓰는" 방식으로 MSI 를 보낸다. 그
 * 주소와 값을 정해 주는 것이 이 콜백이고, 결과가 장치의 MSI capability
 * 또는 MSI-X 테이블에 기록된다.
 *
 * 주소는 rcar_pcie_enable_msi() 가 PCIEMSIALR/PCIEMSIAUR 에 써 둔 값을
 * 그대로 읽어 온다. 하위 워드에서 MSIFE 비트를 지우는 것이 요점이다 -
 * 그 비트는 주소의 일부가 아니라 "주소 디코딩 활성" 플래그라, 장치에
 * 알려 줄 주소에는 들어가면 안 된다.
 *
 * 데이터는 hwirq 를 그대로 쓴다. 장치가 그 값을 써 보내면 컨트롤러가
 * PCIEMSIFR 의 그 번호 비트를 세우고, rcar_pcie_msi_irq() 가 같은 번호로
 * 되돌려 도메인에 넘긴다. 이렇게 벡터 번호가 데이터 값으로 왕복한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(벡터 할당 시). 잠들지 않는다.
 *
 * 호출 체인:  MSI 코어 → [이 함수] → rcar_pci_read_reg()
 */
static void rcar_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct rcar_msi *msi = irq_data_get_irq_chip_data(data);
	struct rcar_pcie *pcie = &msi_to_host(msi)->pcie;
/* [한국어] 수신 주소 하위 워드를 읽되 MSIFE 를 지운다. 그 비트는 주소의 일부가 아니라
 * "주소 디코딩 활성" 플래그라, 장치에 알려 줄 주소에 들어가면 안 된다. */

	msg->address_lo = rcar_pci_read_reg(pcie, PCIEMSIALR) & ~MSIFE;
	/* [한국어] 수신 주소 상위 워드는 그대로 쓴다. */
	msg->address_hi = rcar_pci_read_reg(pcie, PCIEMSIAUR);
	/* [한국어] 데이터는 벡터 번호 그 자체다. 장치가 이 값을 써 보내면 컨트롤러가
	 * PCIEMSIFR 의 그 번호 비트를 세우고, 핸들러가 같은 번호로 되돌려 넘긴다. */
	msg->data = data->hwirq;
}

static struct irq_chip rcar_msi_bottom_chip = {
	/* [한국어] /proc/interrupts 에 표시될 이름이자 IRQ 등록에도 쓰이는 문자열. */
	.name			= "R-Car MSI",
	/* [한국어] edge 인터럽트라 핸들러 전에 플래그를 지워야 한다. */
	.irq_ack		= rcar_msi_irq_ack,
	.irq_mask		= rcar_msi_irq_mask,
	.irq_unmask		= rcar_msi_irq_unmask,
	.irq_compose_msi_msg	= rcar_compose_msi_msg,
};

/* [한국어]
 * rcar_msi_domain_alloc - MSI 벡터를 비트맵에서 잡아 virq 에 연결한다
 *
 * @domain:  이 컨트롤러의 MSI 도메인.
 * @virq:    시작 가상 IRQ 번호.
 * @nr_irqs: 요청 개수.
 * @args:    쓰지 않는다.
 * @return: 0 성공, -ENOSPC 는 연속된 빈 자리가 없는 경우.
 *
 * irq_domain_ops 의 alloc 콜백이다. 32비트 비트맵 msi->used 에서 자리를
 * 찾아 각 virq 에 hwirq 와 irq_chip 을 묶는다.
 *
 * bitmap_find_free_region 에 order_base_2(nr_irqs) 를 넘기는 것이 핵심이다.
 * multi-MSI 는 벡터가 연속이고 개수가 2의 거듭제곱이며 시작이 그 크기에
 * 정렬되어야 하는데, 이 함수가 그 세 조건을 한 번에 만족하는 자리를 찾아 준다.
 * msi_parent_ops 의 supported_flags 에 MSI_FLAG_MULTI_PCI_MSI 가 있어
 * 실제로 여러 개를 요청받을 수 있다.
 *
 * 핸들러로 handle_edge_irq 를 지정한다. MSI 는 본질적으로 edge 이기 때문이다.
 *
 * 비트맵 조작을 뮤텍스로 감싼다. 벡터 할당은 프로세스 컨텍스트에서만
 * 일어나므로 잠들 수 있는 락으로 충분하다 - 인터럽트 경로에서 쓰는
 * mask_lock 이 raw_spinlock 인 것과 대비된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  MSI 코어(pci_msi_setup_msi_irqs 등) → [이 함수]
 *               → bitmap_find_free_region() → irq_domain_set_info()
 */
static int rcar_msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs, void *args)
{
	struct rcar_msi *msi = domain->host_data;
	unsigned int i;
	/* [한국어] 비트맵에서 잡은 시작 벡터 번호. */
	int hwirq;

	mutex_lock(&msi->map_lock);

	hwirq = bitmap_find_free_region(msi->used, INT_PCI_MSI_NR, order_base_2(nr_irqs));
/* [한국어] 연속되고 2의 거듭제곱 개수이며 그 크기에 정렬된 자리를 한 번에 찾는다.
 * multi-MSI 의 세 조건을 이 한 호출이 만족시킨다. */

	mutex_unlock(&msi->map_lock);

	if (hwirq < 0)
		/* [한국어] 빈 자리가 없다. */
		return -ENOSPC;

	for (i = 0; i < nr_irqs; i++)
		/* [한국어] 요청한 개수만큼 virq 와 hwirq 를 짝지어 등록한다. */
		irq_domain_set_info(domain, virq + i, hwirq + i,
				    /* [한국어] irq_chip 과 host_data 를 함께 꽂는다. 다음 인자의 handle_edge_irq 가
				     * MSI 에 맞는 흐름 제어 함수다. */
				    &rcar_msi_bottom_chip, domain->host_data,
				    handle_edge_irq, NULL, NULL);

	return 0;
}

/* [한국어]
 * rcar_msi_domain_free - 잡아 두었던 MSI 벡터를 비트맵에 되돌린다
 *
 * @domain:  이 컨트롤러의 MSI 도메인.
 * @virq:    시작 가상 IRQ 번호.
 * @nr_irqs: 반납할 개수.
 * @return: 없음.
 *
 * alloc 의 짝이다. virq 로 irq_data 를 얻어 hwirq 를 알아낸 뒤,
 * bitmap_release_region 으로 그 자리를 비운다. order 계산이 alloc 과
 * 같아야 정확히 같은 범위가 풀린다.
 *
 * 같은 뮤텍스로 보호한다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  MSI 코어 → [이 함수] → bitmap_release_region()
 */
static void rcar_msi_domain_free(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct rcar_msi *msi = domain->host_data;
/* [한국어] virq 로 irq_data 를 얻어 hwirq 를 알아낸다. */

	mutex_lock(&msi->map_lock);

	bitmap_release_region(msi->used, d->hwirq, order_base_2(nr_irqs));
/* [한국어] alloc 과 같은 order 로 풀어야 정확히 같은 범위가 비워진다. */

	mutex_unlock(&msi->map_lock);
}

static const struct irq_domain_ops rcar_msi_domain_ops = {
	/* [한국어] 벡터 할당 콜백. */
	.alloc	= rcar_msi_domain_alloc,
	/* [한국어] 반납 콜백. 이 둘만 있으면 커널 MSI 계층이 나머지를 처리한다. */
	.free	= rcar_msi_domain_free,
};

#define RCAR_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				 MSI_FLAG_USE_DEF_CHIP_OPS	| \
				 MSI_FLAG_PCI_MSI_MASK_PARENT	| \
				 MSI_FLAG_NO_AFFINITY)

#define RCAR_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK	| \
				  MSI_FLAG_MULTI_PCI_MSI)

static const struct msi_parent_ops rcar_msi_parent_ops = {
	/* [한국어] 이 도메인이 반드시 갖춰야 할 성질. NO_AFFINITY 가 들어 있는 것은
	 * 이 컨트롤러가 벡터별 CPU 지정을 지원하지 않기 때문이다. */
	.required_flags		= RCAR_MSI_FLAGS_REQUIRED,
	/* [한국어] 지원하는 성질. MULTI_PCI_MSI 가 있어 한 장치가 벡터를 여러 개 받을 수 있다. */
	.supported_flags	= RCAR_MSI_FLAGS_SUPPORTED,
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	.chip_flags		= MSI_CHIP_FLAG_SET_ACK,
	.prefix			= "RCAR-",
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};

/* [한국어]
 * rcar_allocate_domains - 이 컨트롤러의 MSI irq_domain 을 만든다
 *
 * @msi: MSI 상태 묶음.
 * @return: 0 성공, -ENOMEM 은 도메인 생성 실패.
 *
 * 커널의 계층형 MSI 구조에 이 컨트롤러를 끼워 넣는 작업이다.
 * msi_create_parent_irq_domain() 이 부모 도메인 하나를 만들고, 그 위에
 * PCI/MSI 용 자식 도메인을 붙이는 일까지 rcar_msi_parent_ops 를 통해
 * 알아서 처리한다.
 *
 * info 에 채우는 네 값의 뜻은 - fwnode 는 DT 노드(도메인 식별자),
 * ops 는 alloc/free 콜백, host_data 는 콜백들이 되찾을 msi 포인터,
 * size 는 벡터 수 INT_PCI_MSI_NR(32)이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  rcar_pcie_enable_msi() → [이 함수] → msi_create_parent_irq_domain()
 */
static int rcar_allocate_domains(struct rcar_msi *msi)
{
	struct rcar_pcie *pcie = &msi_to_host(msi)->pcie;
	struct irq_domain_info info = {
		/* [한국어] DT 노드를 도메인 식별자로 쓴다. */
		.fwnode		= dev_fwnode(pcie->dev),
		/* [한국어] 위에서 정의한 alloc/free 콜백. */
		.ops		= &rcar_msi_domain_ops,
		.host_data	= msi,
		.size		= INT_PCI_MSI_NR,
	};

	msi->domain = msi_create_parent_irq_domain(&info, &rcar_msi_parent_ops);
	/* [한국어] 도메인 생성 실패. */
	if (!msi->domain) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(pcie->dev, "failed to create IRQ domain\n");
		/* [한국어] 메모리 부족으로 돌린다. */
		return -ENOMEM;
	}

	return 0;
}

/* [한국어]
 * rcar_free_domains - MSI irq_domain 을 없앤다
 *
 * @msi: MSI 상태 묶음.   @return: 없음.
 *
 * rcar_allocate_domains() 의 역이며 irq_domain_remove() 한 줄이다.
 * 계층으로 만든 자식 도메인까지 그 안에서 함께 정리된다.
 *
 * probe 실패 경로와 teardown 양쪽에서 불린다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  rcar_pcie_enable_msi() 의 에러 경로 / rcar_pcie_teardown_msi()
 *               → [이 함수] → irq_domain_remove()
 */
static void rcar_free_domains(struct rcar_msi *msi)
{
	irq_domain_remove(msi->domain);
}

/* [한국어]
 * rcar_pcie_enable_msi - MSI 수신을 켠다. 도메인·IRQ·수신 주소를 모두 세운다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공, 음수 errno 실패(-ENOMEM, IRQ 등록 실패 등).
 *
 * 순서가 의미를 갖는다.
 *   1) 락 두 개를 초기화한다. map_lock 은 비트맵용 뮤텍스,
 *      mask_lock 은 PCIEMSIIER 갱신용 raw spinlock 이다.
 *   2) DT 의 0번 자원에서 레지스터 블록의 물리 주소를 얻는다. 이것이
 *      아래에서 MSI 수신 주소로 쓰인다.
 *   3) irq_domain 을 만든다.
 *   4) 두 IRQ 에 같은 핸들러를 건다. 상류 주석대로 이 둘은 MSI 전용이
 *      아니라 비-MSI 인터럽트와도 공유되므로 IRQF_SHARED 가 필요하고,
 *      그래서 핸들러가 IRQ_NONE 을 돌려줄 수 있어야 한다.
 *      IRQF_NO_THREAD 는 이 핸들러를 스레드화하지 않겠다는 뜻으로,
 *      MSI 를 상위 핸들러로 넘기는 일은 짧고 잠들지 않기 때문이다.
 *   5) 모든 MSI 를 일단 막는다(PCIEMSIIER 에 0). 켜자마자 엉뚱한 인터럽트가
 *      오는 것을 막는 순서다.
 *   6) 수신 주소를 등록한다. 상류 주석이 근거를 밝힌다 - 레지스터 블록의
 *      시작 주소를 쓰는데, R-Car 하드웨어에서는 그 주소가 반드시 하위
 *      32비트 범위 안에 있기 때문이다. 하위 워드에 MSIFE(주소 디코딩 활성)를
 *      함께 세운다.
 *
 * 에러 경로가 도메인 해제 하나뿐인 이유는 devm_request_irq 로 등록한 IRQ 가
 * 장치 해제 시 자동으로 반납되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  rcar_pcie_probe() → [이 함수] → rcar_allocate_domains()
 *               → devm_request_irq() → rcar_pci_write_reg()
 */
static int rcar_pcie_enable_msi(struct rcar_pcie_host *host)
{
	struct rcar_pcie *pcie = &host->pcie;
	struct device *dev = pcie->dev;
	/* [한국어] MSI 상태 묶음. */
	struct rcar_msi *msi = &host->msi;
	/* [한국어] DT 0번 자원. 아래에서 MSI 수신 주소로 쓴다. */
	struct resource res;
	/* [한국어] 하위 호출 결과. */
	int err;

	mutex_init(&msi->map_lock);
	raw_spin_lock_init(&msi->mask_lock);

	err = of_address_to_resource(dev->of_node, 0, &res);
	/* [한국어] 레지스터 블록의 물리 주소를 얻지 못하면 */
	if (err)
		/* [한국어] MSI 수신 주소를 정할 수 없다. */
		return err;

	err = rcar_allocate_domains(msi);
	/* [한국어] 도메인 생성 실패면 */
	if (err)
		/* [한국어] 그대로 전한다. */
		return err;

	/* Two IRQs are for MSI, but they are also used for non-MSI IRQs */
	err = devm_request_irq(dev, msi->irq1, rcar_pcie_msi_irq,
			       IRQF_SHARED | IRQF_NO_THREAD,
			       rcar_msi_bottom_chip.name, host);
	if (err < 0) {
		/* [한국어] 첫 IRQ 등록 실패를 남기고 */
		dev_err(dev, "failed to request IRQ: %d\n", err);
		/* [한국어] 도메인을 되돌리는 경로로. */
		goto err;
	}

	err = devm_request_irq(dev, msi->irq2, rcar_pcie_msi_irq,
			       /* [한국어] IRQF_SHARED 는 이 선이 비-MSI 인터럽트와 공유되기 때문이고(상류 주석),
			        * IRQF_NO_THREAD 는 핸들러가 짧고 잠들지 않아 스레드화가 불필요해서다. */
			       IRQF_SHARED | IRQF_NO_THREAD,
			       rcar_msi_bottom_chip.name, host);
	if (err < 0) {
		/* [한국어] 둘째 IRQ 등록 실패를 남기고 */
		dev_err(dev, "failed to request IRQ: %d\n", err);
		/* [한국어] 같은 경로로. */
		goto err;
	}

	/* Disable all MSIs */
	rcar_pci_write_reg(pcie, 0, PCIEMSIIER);

	/*
	 * Setup MSI data target using RC base address, which is guaranteed
	 * to be in the low 32bit range on any R-Car HW.
	 */
	rcar_pci_write_reg(pcie, lower_32_bits(res.start) | MSIFE, PCIEMSIALR);
	rcar_pci_write_reg(pcie, upper_32_bits(res.start), PCIEMSIAUR);
/* [한국어] 수신 주소 상위 워드. 하위는 바로 위에서 MSIFE 와 함께 썼다. */

	return 0;

err:
	rcar_free_domains(msi);
	return err;
}

/* [한국어]
 * rcar_pcie_teardown_msi - MSI 수신을 끄고 도메인을 없앤다
 *
 * @host: 컨트롤러 상태.   @return: 없음.
 *
 * enable 의 역순이다. 모든 벡터를 막고(PCIEMSIIER 에 0), 수신 주소
 * 디코딩을 끄고(PCIEMSIALR 에 0 - MSIFE 를 포함해 통째로 지운다),
 * 도메인을 없앤다.
 *
 * 하드웨어를 먼저 끄고 소프트웨어를 나중에 정리하는 순서가 중요하다.
 * 도메인을 먼저 없애면 그사이 도착한 MSI 를 넘길 곳이 사라진다.
 *
 * IRQ 해제가 없는 이유는 devm 으로 등록해 장치 해제 시 자동 반납되기 때문이다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  rcar_pcie_probe() 의 에러 경로 → [이 함수] → rcar_free_domains()
 */
static void rcar_pcie_teardown_msi(struct rcar_pcie_host *host)
{
	struct rcar_pcie *pcie = &host->pcie;

	/* Disable all MSI interrupts */
	rcar_pci_write_reg(pcie, 0, PCIEMSIIER);

	/* Disable address decoding of the MSI interrupt, MSIFE */
	rcar_pci_write_reg(pcie, 0, PCIEMSIALR);

	rcar_free_domains(&host->msi);
}

/* [한국어]
 * rcar_pcie_get_resources - DT 에서 PHY·레지스터·클럭·IRQ 를 받아 온다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공, 음수 errno 실패.
 *
 * probe 가 필요한 자원을 한 곳에서 모아 온다.
 *   - PHY 는 optional 로 받는다. 1·2세대는 PHY 를 레지스터로 직접 다루므로
 *     별도 PHY 장치가 없고, 3세대만 실제 값이 들어온다. 없으면 NULL 이 되고
 *     그것이 정상이다.
 *   - 0번 자원을 ioremap 해 pcie->base 로 삼는다. 이 주소가 이후 모든
 *     레지스터 접근의 기준이며, MSI 수신 주소로도 쓰인다.
 *   - "pcie_bus" 클럭을 받는다. 여기서는 활성화하지 않고 probe 가 따로 켠다.
 *   - IRQ 두 개를 DT 인터럽트 목록의 0번과 1번에서 매핑한다.
 *
 * 에러 레이블 이름이 실제 동작과 어긋나 보이는 대목이 있다. err_irq1 은
 * 아무것도 되돌리지 않고 err 만 돌려주며, err_irq2 는 irq1 의 매핑만 푼다.
 * 자기 이름이 아니라 "그 단계에서 실패했을 때 되돌릴 것" 을 가리키는
 * 관례로 읽으면 동작 자체는 맞다. 코드는 고치지 않고 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  rcar_pcie_probe() → [이 함수] → devm_phy_optional_get()
 *               → devm_ioremap_resource() → devm_clk_get() → irq_of_parse_and_map()
 */
static int rcar_pcie_get_resources(struct rcar_pcie_host *host)
{
	struct rcar_pcie *pcie = &host->pcie;
	struct device *dev = pcie->dev;
	/* [한국어] DT 0번 자원. */
	struct resource res;
	/* [한국어] err 는 결과, i 는 IRQ 매핑 결과를 잠시 받는 변수. */
	int err, i;

	host->phy = devm_phy_optional_get(dev, "pcie");
	/* [한국어] PHY 획득에 실패했으면(없는 것과는 다르다) */
	if (IS_ERR(host->phy))
		/* [한국어] 그 오류를 전한다. optional 이라 없으면 NULL 이 오고 오류가 아니다. */
		return PTR_ERR(host->phy);

	err = of_address_to_resource(dev->of_node, 0, &res);
	/* [한국어] 레지스터 블록의 물리 주소를 얻지 못하면 */
	if (err)
		/* [한국어] 진행할 수 없다. */
		return err;

	pcie->base = devm_ioremap_resource(dev, &res);
	/* [한국어] 매핑 실패면 */
	if (IS_ERR(pcie->base))
		/* [한국어] 그 오류를 전한다. 이 주소가 이후 모든 레지스터 접근의 기준이다. */
		return PTR_ERR(pcie->base);

	host->bus_clk = devm_clk_get(dev, "pcie_bus");
	/* [한국어] 버스 클럭을 얻지 못했으면 */
	if (IS_ERR(host->bus_clk)) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "cannot get pcie bus clock\n");
		/* [한국어] 오류를 전한다. 여기서 켜지는 않고 probe 가 따로 켠다. */
		return PTR_ERR(host->bus_clk);
	}

	i = irq_of_parse_and_map(dev->of_node, 0);
	/* [한국어] 0 이면 매핑 실패다 — irq_of_parse_and_map 은 실패를 0 으로 알린다. */
	if (!i) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "cannot get platform resources for msi interrupt\n");
		/* [한국어] 자원 없음으로 기록해 */
		err = -ENOENT;
		goto err_irq1;
	}
	host->msi.irq1 = i;
/* [한국어] 되돌릴 것이 없는 경로로 간다. */

	i = irq_of_parse_and_map(dev->of_node, 1);
	/* [한국어] 둘째 IRQ 도 같은 방식으로 확인한다. */
	if (!i) {
		/* [한국어] 실패를 남기고 */
		dev_err(dev, "cannot get platform resources for msi interrupt\n");
		/* [한국어] 자원 없음으로 기록해 */
		err = -ENOENT;
		goto err_irq2;
	}
	host->msi.irq2 = i;
/* [한국어] 첫 IRQ 매핑을 되돌리는 경로로 간다. */

	return 0;

err_irq2:
	irq_dispose_mapping(host->msi.irq1);
err_irq1:
	return err;
}

/* [한국어]
 * rcar_pcie_inbound_ranges - dma-ranges 한 항목을 하드웨어 안쪽 창들로 쪼갠다
 *
 * @pcie:  컨트롤러.
 * @entry: DT 에서 온 안쪽 방향 구간 하나.
 * @index: 다음에 쓸 창 번호. 이 함수가 진행한 만큼 올려 돌려준다.
 * @return: 0 성공, -EINVAL 은 창이 모자란 경우.
 *
 * 안쪽 창은 장치의 DMA 가 시스템 메모리에 닿게 하는 통로다. 문제는 이
 * 하드웨어의 창이 "시작 주소 + 크기 마스크" 형태라, 임의의 구간을 창 하나로
 * 표현할 수 없다는 점이다.
 *
 * 그래서 루프를 돌며 쪼갠다. 매 회차의 크기 결정이 이 함수의 핵심이다.
 *   1) 시작 주소의 정렬이 상한이 된다. __ffs64(cpu_addr) 로 하위의 연속된
 *      0 비트 수를 세면 그것이 정렬 크기의 로그이고, 창 하나는 그보다 클 수
 *      없다. 상류 주석이 그 사정을 적어 두었다.
 *   2) 하드웨어 상한 4GiB 로 한 번 더 자른다(상류 주석).
 *   3) 그 크기를 2의 거듭제곱으로 올린 뒤 1 을 빼 마스크를 만들고,
 *      하위 4비트를 지운다. 그 자리에 LAM_PREFETCH/LAM_64BIT/LAR_ENABLE
 *      같은 플래그가 들어가기 때문이다(pcie-rcar.h 의 BIT(3)/BIT(2)/BIT(1)).
 *
 * 창 번호를 2씩 올리는 것도 이유가 있다. rcar_pcie_set_inbound()(pcie-rcar.c:95)
 * 가 idx 와 idx+1 을 한 쌍으로 써서 64비트 주소의 하위/상위를 나눠 담는다.
 * 그래서 MAX_NR_INBOUND_MAPS(6)은 실제로는 세 쌍이고, 검사도
 * idx >= MAX_NR_INBOUND_MAPS - 1 로 한 칸 여유를 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe/resume).
 *
 * 호출 체인:  rcar_pcie_parse_map_dma_ranges() → [이 함수]
 *               → rcar_pcie_set_inbound() [pcie-rcar.c:95]
 */
static int rcar_pcie_inbound_ranges(struct rcar_pcie *pcie,
				    struct resource_entry *entry,
				    int *index)
{
	u64 restype = entry->res->flags;
	u64 cpu_addr = entry->res->start;
	/* [한국어] 구간의 끝(포함). */
	u64 cpu_end = entry->res->end;
	/* [한국어] 장치가 볼 PCI 주소. CPU 주소에서 offset 을 빼 얻는다. */
	u64 pci_addr = entry->res->start - entry->offset;
	/* [한국어] 모든 창에 공통으로 붙는 플래그 — 64비트 주소이고 창을 활성화한다
	 * (pcie-rcar.h 의 BIT(2), BIT(1)). */
	u32 flags = LAM_64BIT | LAR_ENABLE;
	/* [한국어] 이번 창의 크기 마스크. */
	u64 mask;
	/* [한국어] 남은 크기. 루프를 돌며 줄어든다. */
	u64 size = resource_size(entry->res);
	/* [한국어] 시작 창 번호. 호출자가 이어서 쓰도록 참조로 받았다. */
	int idx = *index;

	if (restype & IORESOURCE_PREFETCH)
		/* [한국어] prefetch 가능 구간이면 그 플래그를 더한다(BIT(3)). */
		flags |= LAM_PREFETCH;

	while (cpu_addr < cpu_end) {
		/* [한국어] 창을 한 쌍(idx, idx+1)씩 쓰므로 마지막 한 칸을 남겨 두고 검사한다. */
		if (idx >= MAX_NR_INBOUND_MAPS - 1) {
			/* [한국어] 더 쪼갤 창이 없다. */
			dev_err(pcie->dev, "Failed to map inbound regions!\n");
			/* [한국어] 인자 오류로 돌린다. */
			return -EINVAL;
		}

		/*
		 * If the size of the range is larger than the alignment of
		 * the start address, we have to use multiple entries to
		 * perform the mapping.
		 */
		if (cpu_addr > 0) {
			unsigned long nr_zeros = __ffs64(cpu_addr);
			/* [한국어] 하위의 연속된 0 비트 수가 곧 정렬 크기의 로그다. */
			u64 alignment = 1ULL << nr_zeros;

			size = min(size, alignment);
		/* [한국어] 창 하나는 시작 주소의 정렬보다 클 수 없다(상류 주석). */
		}

		/* Hardware supports max 4GiB inbound region */
		size = min(size, 1ULL << 32);

		mask = roundup_pow_of_two(size) - 1;
		/* [한국어] 마스크의 하위 4비트를 지운다. 그 자리에 위 flags 가 들어가기 때문이다. */
		mask &= ~0xf;

		rcar_pcie_set_inbound(pcie, cpu_addr, pci_addr,
				      /* [한국어] 마스크와 플래그를 합쳐 넘긴다. 마지막 인자 true 가 "호스트 모드" 로,
				       * PCIEPRAR(장치가 볼 주소)까지 함께 쓰라는 뜻이다(pcie-rcar.c:95).
				       * EP 판은 여기에 false 를 넘겨 그 레지스터를 건드리지 않는다. */
				      lower_32_bits(mask) | flags, idx, true);

		pci_addr += size;
		/* [한국어] CPU 쪽도 같은 크기만큼 전진한다. */
		cpu_addr += size;
		/* [한국어] 창을 한 쌍 썼으므로 번호를 2 올린다. */
		idx += 2;
	}
	*index = idx;

	return 0;
}

/* [한국어]
 * rcar_pcie_parse_map_dma_ranges - DT 의 dma-ranges 전체를 안쪽 창으로 옮긴다
 *
 * @host: 컨트롤러 상태.
 * @return: 0 성공, 첫 실패의 errno.
 *
 * 브리지의 dma_ranges 목록을 훑으며 항목마다 rcar_pcie_inbound_ranges() 를
 * 부른다. index 를 참조로 넘겨 항목 사이에 창 번호가 이어지게 한다 -
 * 그러지 않으면 두 번째 항목이 첫 항목의 창을 덮어쓴다.
 *
 * dma_ranges 목록 자체는 PCI 코어가 DT 의 dma-ranges 속성을 파싱해
 * 채워 둔 것이다.
 *
 * 하나라도 실패하면 break 로 멈추고 그 errno 를 전한다. 중간까지 설정된
 * 창은 되돌리지 않는데, 호출자가 probe 를 실패시켜 장치 전체가 정리되기
 * 때문이다.
 *
 * probe 와 resume 양쪽에서 불린다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  rcar_pcie_probe() / rcar_pcie_resume() → [이 함수]
 *               → rcar_pcie_inbound_ranges()
 */
static int rcar_pcie_parse_map_dma_ranges(struct rcar_pcie_host *host)
{
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(host);
	struct resource_entry *entry;
	/* [한국어] index 는 창 번호 누적, err 는 첫 실패를 담는다. */
	int index = 0, err = 0;

	resource_list_for_each_entry(entry, &bridge->dma_ranges) {
		/* [한국어] 항목마다 창으로 옮긴다. index 를 참조로 넘겨 항목 사이에 번호가 이어지게 한다 —
		 * 그러지 않으면 두 번째 항목이 첫 항목의 창을 덮어쓴다. */
		err = rcar_pcie_inbound_ranges(&host->pcie, entry, &index);
		/* [한국어] 하나라도 실패하면 */
		if (err)
			/* [한국어] 멈춘다. 중간까지 설정된 창은 되돌리지 않는데, 호출자가 probe 를 실패시켜
			 * 장치 전체가 정리되기 때문이다. */
			break;
	}

	return err;
}

static const struct of_device_id rcar_pcie_of_match[] = {
	/* [한국어] 1세대 R-Car H1. */
	{ .compatible = "renesas,pcie-r8a7779",
	  /* [한국어] compatible 마다 PHY 초기화 함수를 data 로 묶어 둔다. probe 가 이 값을
	   * of_device_get_match_data() 로 받아 host->phy_init_fn 에 꽂는다. */
	  .data = rcar_pcie_phy_init_h1 },
	{ .compatible = "renesas,pcie-r8a7790",
	  .data = rcar_pcie_phy_init_gen2 },
	{ .compatible = "renesas,pcie-r8a7791",
	  .data = rcar_pcie_phy_init_gen2 },
	{ .compatible = "renesas,pcie-rcar-gen2",
	  .data = rcar_pcie_phy_init_gen2 },
	{ .compatible = "renesas,pcie-r8a7795",
	  .data = rcar_pcie_phy_init_gen3 },
	{ .compatible = "renesas,pcie-rcar-gen3",
	  .data = rcar_pcie_phy_init_gen3 },
	{},
};

/* Design note 346 from Linear Technology says order is not important. */
static const char * const rcar_pcie_supplies[] = {
	"vpcie1v5",
	/* [한국어] 3.3V 레일. 세 레일 모두 optional 이며, 상류 주석대로 켜는 순서는 무관하다. */
	"vpcie3v3",
	"vpcie12v",
};

/* [한국어]
 * rcar_pcie_probe - 플랫폼 드라이버 진입점. 자원 확보부터 열거까지 엮는다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 성공. -ENOMEM, -ENODEV(링크 없음), 그 밖의 하위 호출 errno.
 *
 * 이 파일의 전체 흐름이 이 함수에 담겨 있다.
 *   1) devm_pci_alloc_host_bridge() 로 pci_host_bridge 와 이 드라이버의
 *      private 영역(struct rcar_pcie_host)을 한 덩어리로 할당한다.
 *      pci_host_bridge_priv() 로 그 private 부분을 얻는다.
 *   2) 세 전원 레귤레이터를 켠다. 전부 optional 이라 -ENODEV(그 레일이 DT 에
 *      없음)는 정상으로 넘긴다.
 *   3) 런타임 PM 을 켜고 참조를 잡아 하드웨어에 전원과 클럭이 들어오게 한다.
 *   4) 자원을 받고 버스 클럭을 켠다.
 *   5) dma-ranges 를 안쪽 창으로 옮긴다.
 *   6) compatible 에 묶인 PHY 초기화 함수를 of_device_get_match_data() 로
 *      받아 부른다. 이 한 줄이 세대별 차이를 흡수한다.
 *   7) rcar_pcie_hw_init() 으로 링크를 세운다. 실패를 -ENODEV 로 바꾸면서
 *      남기는 로그가 "PCIe link down" 인 것이 상류의 판단이다 - 상류 주석대로
 *      카드가 안 꽂혀 있을 뿐일 수 있어 오류로 요란하게 알리지 않는다.
 *   8) MACSR 에서 링크 폭을 읽어 찍는다. (data >> 20) & 0x3f 의 필드 위치는
 *      pcie-rcar.h 에 정의가 없어 이 트리에서 근거를 확인하지 못했다.
 *   9) MSI 를 켜고, 마지막으로 rcar_pcie_enable() 로 PCI 코어에 넘긴다.
 *
 * 에러 경로가 다섯 단계로 나뉜 것은 어느 지점에서 실패했느냐에 따라
 * 되돌릴 것이 다르기 때문이다 - MSI 정리, PHY 끄기, 클럭 끄기, IRQ 매핑 해제,
 * 런타임 PM 되돌리기 순으로 쌓여 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(드라이버 바인딩).
 *
 * 호출 체인:  플랫폼 버스 → [이 함수] → rcar_pcie_get_resources()
 *               → rcar_pcie_parse_map_dma_ranges() → host->phy_init_fn()
 *               → rcar_pcie_hw_init() → rcar_pcie_enable_msi() → rcar_pcie_enable()
 */
static int rcar_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *bridge;
	/* [한국어] 이 드라이버의 상태(브리지 private 영역에 얹힌다). */
	struct rcar_pcie_host *host;
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct rcar_pcie *pcie;
	/* [한국어] 레귤레이터 목록 반복자. */
	unsigned int i;
	/* [한국어] MACSR 에서 읽을 링크 폭. */
	u32 data;
	/* [한국어] 하위 호출 결과. */
	int err;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*host));
	/* [한국어] 할당 실패면 */
	if (!bridge)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	host = pci_host_bridge_priv(bridge);
	/* [한국어] private 영역 안의 공통 구조체를 가리킨다. */
	pcie = &host->pcie;
	/* [한국어] 이후 모든 로그와 devm 할당의 주인이 된다. */
	pcie->dev = dev;
	/* [한국어] resume 콜백이 dev_get_drvdata() 로 되찾을 수 있게 저장해 둔다. */
	platform_set_drvdata(pdev, host);

	for (i = 0; i < ARRAY_SIZE(rcar_pcie_supplies); i++) {
		/* [한국어] 레일을 하나씩 켠다. optional 판이라 DT 에 없으면 -ENODEV 가 온다. */
		err = devm_regulator_get_enable_optional(dev, rcar_pcie_supplies[i]);
		/* [한국어] 실패이면서 "없음" 도 아니면 진짜 오류다. */
		if (err < 0 && err != -ENODEV)
			/* [한국어] 어느 레일인지 남기며 probe 를 실패시킨다. */
			return dev_err_probe(dev, err, "failed to enable regulator: %s\n",
					     rcar_pcie_supplies[i]);
	}

	pm_runtime_enable(pcie->dev);
	err = pm_runtime_get_sync(pcie->dev);
	/* [한국어] 참조 획득에 실패했으면 */
	if (err < 0) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(pcie->dev, "pm_runtime_get_sync failed\n");
		/* [한국어] 런타임 PM 을 되돌리는 경로로. */
		goto err_pm_put;
	}

	err = rcar_pcie_get_resources(host);
	/* [한국어] 자원 확보에 실패했으면 */
	if (err < 0) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "failed to request resources: %d\n", err);
		/* [한국어] 같은 경로로. */
		goto err_pm_put;
	}

	err = clk_prepare_enable(host->bus_clk);
	/* [한국어] 버스 클럭 켜기에 실패했으면 */
	if (err) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "failed to enable bus clock: %d\n", err);
		/* [한국어] IRQ 매핑까지 되돌리는 경로로. */
		goto err_unmap_msi_irqs;
	}

	err = rcar_pcie_parse_map_dma_ranges(host);
	/* [한국어] 안쪽 창 설정에 실패했으면 */
	if (err)
		/* [한국어] 클럭까지 되돌리는 경로로. */
		goto err_clk_disable;

	host->phy_init_fn = of_device_get_match_data(dev);
	/* [한국어] 세대별 PHY 초기화 함수를 부른다. 이 한 줄이 세대 차이를 흡수한다. */
	err = host->phy_init_fn(host);
	/* [한국어] 실패했으면 */
	if (err) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "failed to init PCIe PHY\n");
		/* [한국어] 클럭을 되돌리는 경로로. */
		goto err_clk_disable;
	}

	/* Failure to get a link might just be that no cards are inserted */
	if (rcar_pcie_hw_init(pcie)) {
		dev_info(dev, "PCIe link down\n");
		/* [한국어] 링크 실패를 -ENODEV 로 바꾼다. 상류 주석대로 카드가 안 꽂혔을 뿐일 수 있어
		 * 로그도 오류가 아닌 정보 수준이다. */
		err = -ENODEV;
		goto err_phy_shutdown;
	}

	data = rcar_pci_read_reg(pcie, MACSR);
	/* [한국어] 링크 폭을 찍는다. (data >> 20) & 0x3f 의 필드 위치는 pcie-rcar.h 에 정의가
	 * 없어 이 트리에서 근거를 확인하지 못했다. */
	dev_info(dev, "PCIe x%d: link up\n", (data >> 20) & 0x3f);

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		/* [한국어] MSI 를 쓰는 빌드면 수신 체계를 켠다. */
		err = rcar_pcie_enable_msi(host);
		/* [한국어] 실패했으면 */
		if (err < 0) {
			/* [한국어] 그 사실을 남기고 PHY 를 되돌리는 경로로 간다. */
			dev_err(dev,
				"failed to enable MSI support: %d\n",
				err);
			goto err_phy_shutdown;
		}
	}

	err = rcar_pcie_enable(host);
	/* [한국어] 열거 시작에 실패했으면 */
	if (err)
		/* [한국어] MSI 부터 되돌리는 경로로. */
		goto err_msi_teardown;

	return 0;

err_msi_teardown:
	if (IS_ENABLED(CONFIG_PCI_MSI))
		rcar_pcie_teardown_msi(host);

err_phy_shutdown:
	if (host->phy) {
		phy_power_off(host->phy);
		phy_exit(host->phy);
	}

err_clk_disable:
	clk_disable_unprepare(host->bus_clk);

err_unmap_msi_irqs:
	irq_dispose_mapping(host->msi.irq2);
	irq_dispose_mapping(host->msi.irq1);

err_pm_put:
	pm_runtime_put(dev);
	pm_runtime_disable(dev);

	return err;
}

/* [한국어]
 * rcar_pcie_resume - 시스템 절전에서 돌아와 컨트롤러를 다시 세운다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 항상 0. 실패해도 오류로 만들지 않는다.
 *
 * 절전 중에 컨트롤러의 설정이 전부 날아가므로 probe 가 했던 일 중
 * 하드웨어 설정 부분을 다시 한다 - 안쪽 창, PHY, MSI 수신 주소, 바깥 창.
 *
 * 항상 0 을 돌려주는 것이 눈에 띈다. dma-ranges 파싱이 실패하거나 PHY
 * 초기화가 실패해도 0 이다. 링크가 없는 것은 카드가 빠진 상황일 수 있어
 * 시스템 재개 자체를 실패시키지 않겠다는 판단으로 보인다.
 *
 * MSI 복원에서 비트맵을 하드웨어로 되돌리는 대목이 요점이다.
 * bitmap_to_arr32() 로 msi.used(현재 할당된 벡터들)를 32비트 값으로 만들어
 * PCIEMSIIER 에 그대로 쓴다. 절전 전에 켜져 있던 벡터만 정확히 다시
 * 켜지는 셈이다. 다만 이 방식은 개별 벡터의 마스크 상태(rcar_msi_irq_mask 가
 * 꺼 둔 것)를 구분하지 않고 할당 여부만 반영한다. 코드는 고치지 않고
 * 관찰만 적어 둔다.
 *
 * 주소 레지스터를 쓰는 순서가 probe 와 반대다 - 여기서는 상위(AUR)를 먼저
 * 쓰고 하위(ALR)를 나중에 쓴다. 하위에 MSIFE 활성 비트가 함께 들어가므로
 * 상위가 먼저 자리를 잡은 뒤 켜지는 순서가 되어, 오히려 이쪽이 안전하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(시스템 재개).
 *
 * 호출 체인:  PM 코어(SYSTEM_SLEEP_PM_OPS) → [이 함수]
 *               → rcar_pcie_parse_map_dma_ranges() → host->phy_init_fn()
 *               → rcar_pcie_hw_enable()
 */
static int rcar_pcie_resume(struct device *dev)
{
	struct rcar_pcie_host *host = dev_get_drvdata(dev);
	struct rcar_pcie *pcie = &host->pcie;
	/* [한국어] MACSR 에서 읽을 링크 폭. */
	unsigned int data;
	/* [한국어] 하위 호출 결과. */
	int err;

	err = rcar_pcie_parse_map_dma_ranges(host);
	/* [한국어] 안쪽 창 복원에 실패해도 */
	if (err)
		/* [한국어] 0 을 돌려준다. 시스템 재개 자체를 실패시키지 않겠다는 판단이다. */
		return 0;

	/* Failure to get a link might just be that no cards are inserted */
	err = host->phy_init_fn(host);
	if (err) {
		/* [한국어] PHY 초기화에 실패하면 링크가 없다는 뜻이다. */
		dev_info(dev, "PCIe link down\n");
		/* [한국어] 역시 0 을 돌려주고 조용히 물러난다. */
		return 0;
	}

	data = rcar_pci_read_reg(pcie, MACSR);
	/* [한국어] 링크 폭을 찍는다. probe 와 같은 필드 위치를 쓴다. */
	dev_info(dev, "PCIe x%d: link up\n", (data >> 20) & 0x3f);

	/* Enable MSI */
	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		struct resource res;
		/* [한국어] 비트맵을 담을 32비트 값. */
		u32 val;

		of_address_to_resource(dev->of_node, 0, &res);
		/* [한국어] 상위 워드를 먼저 쓰고, */
		rcar_pci_write_reg(pcie, upper_32_bits(res.start), PCIEMSIAUR);
		/* [한국어] 하위 워드를 MSIFE 와 함께 쓴다. probe 와 순서가 반대인데, 활성 비트가
		 * 하위에 있으므로 상위가 자리를 잡은 뒤 켜지는 이쪽이 오히려 안전하다. */
		rcar_pci_write_reg(pcie, lower_32_bits(res.start) | MSIFE, PCIEMSIALR);

		bitmap_to_arr32(&val, host->msi.used, INT_PCI_MSI_NR);
		/* [한국어] 할당돼 있던 벡터만 정확히 다시 켠다(바로 위에서 비트맵을 32비트로 변환).
		 * 다만 이 방식은 개별 벡터의 마스크 상태를 구분하지 않고 할당 여부만
		 * 반영한다. 코드는 고치지 않고 관찰만 적어 둔다. */
		rcar_pci_write_reg(pcie, val, PCIEMSIIER);
	}

	rcar_pcie_hw_enable(host);

	return 0;
}

/* [한국어]
 * rcar_pcie_resume_noirq - 인터럽트를 켜기 전 단계에서 링크를 되살린다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 0 이면 링크가 살아 있거나 다시 세웠다. -ETIMEDOUT 은 실패.
 *
 * noirq 단계는 인터럽트 처리가 아직 꺼져 있는 이른 재개 시점이다. 링크가
 * 없는 상태로 인터럽트가 열리면 곤란하므로 여기서 먼저 확인한다.
 *
 * 앞의 조건이 "다시 세울 필요가 없는 경우" 를 걸러 낸다. PMSR 이 0 이 아니고
 * (전원 관리 상태가 살아 있고) PCIETCTLR 에 DL_DOWN 이 서 있지 않으면
 * (데이터 링크가 내려가지 않았으면) 그대로 0 을 돌려준다.
 *
 * 그렇지 않으면 MACCTLR 을 초기값으로 되돌리고 PCIETCTLR 에 CFINIT 을 써서
 * 링크 훈련을 다시 시작한 뒤, 데이터 링크가 활성화되기를 기다린다.
 * rcar_pcie_hw_init() 의 마지막 두 단계만 떼어 낸 셈이다.
 *
 * rcar_pcie_resume() 과 달리 여기서는 실패를 그대로 전한다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 다만 인터럽트가 비활성인 단계다.
 *
 * 호출 체인:  PM 코어(.resume_noirq) → [이 함수] → rcar_pcie_wait_for_dl() [pcie-rcar.c:44]
 */
static int rcar_pcie_resume_noirq(struct device *dev)
{
	struct rcar_pcie_host *host = dev_get_drvdata(dev);
	struct rcar_pcie *pcie = &host->pcie;
/* [한국어] 전원 관리 상태가 살아 있고 */

	if (rcar_pci_read_reg(pcie, PMSR) &&
	    /* [한국어] 데이터 링크가 내려가지 않았으면 다시 세울 필요가 없다. */
	    !(rcar_pci_read_reg(pcie, PCIETCTLR) & DL_DOWN))
		return 0;

	/* Re-establish the PCIe link */
	rcar_pci_write_reg(pcie, MACCTLR_INIT_VAL, MACCTLR);
	rcar_pci_write_reg(pcie, CFINIT, PCIETCTLR);
	/* [한국어] 데이터 링크가 활성화될 때까지 기다린다(pcie-rcar.c:44).
	 * rcar_pcie_resume() 과 달리 여기서는 실패를 그대로 전한다. */
	return rcar_pcie_wait_for_dl(pcie);
}

static const struct dev_pm_ops rcar_pcie_pm_ops = {
	/* [한국어] 시스템 절전용. suspend 는 NULL 이라 내려갈 때 할 일이 없고, 복원만 한다. */
	SYSTEM_SLEEP_PM_OPS(NULL, rcar_pcie_resume)
	/* [한국어] 인터럽트를 켜기 전 단계에서 링크를 먼저 되살린다. */
	.resume_noirq = rcar_pcie_resume_noirq,
/* [한국어] 이 두 콜백이 절전/재개에서 이 드라이버가 하는 일의 전부다. */
};

static struct platform_driver rcar_pcie_driver = {
	/* [한국어] 플랫폼 드라이버 서술자. */
	.driver = {
		/* [한국어] 드라이버 이름. */
		.name = "rcar-pcie",
		/* [한국어] 위에서 정의한 compatible 표. 이 표에 없는 노드에는 붙지 않는다. */
		.of_match_table = rcar_pcie_of_match,
		.pm = &rcar_pcie_pm_ops,
		.suppress_bind_attrs = true,
	},
	.probe = rcar_pcie_probe,
};

#ifdef CONFIG_ARM
/* [한국어]
 * rcar_pcie_aarch32_abort_handler - ARM 외부 abort 를 예외 테이블로 넘긴다
 *
 * @addr: 문제가 난 주소. 쓰지 않는다.
 * @fsr:  폴트 상태 레지스터. 쓰지 않는다.
 * @regs: 폴트 시점의 레지스터 묶음.
 * @return: 0 이면 처리했으니 계속 진행, 0 이 아니면 처리하지 못했다는 뜻.
 *
 * 없는 장치의 config 를 읽으면 ARM 32비트에서 외부 abort 가 난다. 그대로
 * 두면 커널이 죽으므로, rcar_pci_*_reg_workaround() 가 등록해 둔 예외 테이블
 * 항목을 찾아 그리로 점프시킨다.
 *
 * fixup_exception() 은 항목을 찾아 처리했으면 참을 돌려준다. 이 핸들러의
 * 반환 규약은 반대(0 이 성공)라서 ! 로 뒤집는다. 예외 테이블에 없는 주소면
 * 0 이 아닌 값이 되어 평소의 abort 처리로 넘어간다.
 *
 * CONFIG_ARM 일 때만 컴파일된다.
 * 실행 컨텍스트: 예외 처리 컨텍스트.
 *
 * 호출 체인:  ARM abort 처리 → hook_fault_code 로 등록된 [이 함수] → fixup_exception()
 */
static int rcar_pcie_aarch32_abort_handler(unsigned long addr,
		unsigned int fsr, struct pt_regs *regs)
{
	return !fixup_exception(regs);
}

static const struct of_device_id rcar_pcie_abort_handler_of_match[] __initconst = {
	/* [한국어] 1세대. abort 후킹이 필요한 것은 이 세대들뿐이다. */
	{ .compatible = "renesas,pcie-r8a7779" },
	/* [한국어] 2세대 계열이 이어진다. 3세대는 목록에 없다 — 그쪽은 이 우회가 필요 없다. */
	{ .compatible = "renesas,pcie-r8a7790" },
	{ .compatible = "renesas,pcie-r8a7791" },
	{ .compatible = "renesas,pcie-rcar-gen2" },
	{},
};

/* [한국어]
 * rcar_pcie_init - ARM 에서 abort 후킹을 걸고 드라이버를 등록한다
 *
 * @return: platform_driver_register() 의 결과.
 *
 * CONFIG_ARM 빌드에서만 쓰이는 진입점이다. 평범한
 * builtin_platform_driver() 대신 직접 initcall 을 두는 이유는, 드라이버를
 * 등록하기 전에 abort 핸들러를 걸어야 하기 때문이다.
 *
 * 먼저 DT 에 1·2세대 compatible 이 있는지 확인한다. 이 우회가 필요한 것은
 * 그 세대들뿐이라, 해당 없는 보드에서는 전역 abort 처리를 건드리지 않는다.
 *
 * 폴트 코드가 LPAE 여부로 갈린다 - LPAE 면 17("asynchronous external abort"),
 * 아니면 22("imprecise external abort")다. 페이지 테이블 형식에 따라 같은
 * 사건에 다른 코드가 붙기 때문이다.
 *
 * CONFIG_ARM 이 아닌 빌드에서는 파일 끝의 #else 가지가 평범한
 * builtin_platform_driver() 를 쓴다 - 후킹이 필요 없기 때문이다.
 *
 * 실행 컨텍스트: 부팅 초기(__init, 프로세스 컨텍스트).
 *
 * 호출 체인:  device_initcall → [이 함수] → of_find_matching_node()
 *               → hook_fault_code() → platform_driver_register()
 */
static int __init rcar_pcie_init(void)
{
	if (of_find_matching_node(NULL, rcar_pcie_abort_handler_of_match)) {
#ifdef CONFIG_ARM_LPAE
		hook_fault_code(17, rcar_pcie_aarch32_abort_handler, SIGBUS, 0,
				/* [한국어] LPAE 페이지 테이블에서는 폴트 코드 17 이 이 사건에 해당한다. */
				"asynchronous external abort");
#else
		hook_fault_code(22, rcar_pcie_aarch32_abort_handler, SIGBUS, 0,
				/* [한국어] LPAE 가 아니면 코드 22 다. 같은 사건에 형식에 따라 다른 코드가 붙는다. */
				"imprecise external abort");
#endif
	}

	return platform_driver_register(&rcar_pcie_driver);
/* [한국어] 드라이버를 등록한다(바로 위). 후킹을 먼저 건 뒤여야 첫 config 접근이
 * 안전하게 보호된다. */
}
device_initcall(rcar_pcie_init);
#else
builtin_platform_driver(rcar_pcie_driver);
#endif
