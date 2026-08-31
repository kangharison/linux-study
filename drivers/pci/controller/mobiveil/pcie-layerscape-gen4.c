// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe Gen4 host controller driver for NXP Layerscape SoCs
 *
 * Copyright 2019-2020 NXP
 *
 * Author: Zhiqiang Hou <Zhiqiang.Hou@nxp.com>
 */

/*
 * [한국어 설명] NXP Layerscape Gen4 SoC 용 mobiveil 껍데기 드라이버
 *               (pcie-layerscape-gen4.c)
 *
 * === 파일의 역할 ===
 * 이 디렉터리에는 Mobiveil IP 를 쓰는 SoC 별 드라이버가 둘 있고, 그중
 * 하나가 이 파일이다(대상 compatible 은 fsl,lx2160a-pcie 하나).
 * 하는 일은 세 가지로 나뉜다.
 *
 * 1) 껍데기 노릇. probe 에서 호스트 브리지를 잡고 struct mobiveil_pcie 를
 *    채운 다음, 실제 초기화는 공통 mobiveil_pcie_host_probe() 에 넘긴다.
 *    자기 구조체 struct ls_g4_pcie 는 공통 구조체를 첫 필드로 감싼다.
 *
 * 2) SoC 차이 메우기. NXP 는 Mobiveil IP 위에 PF(Physical Function)라는
 *    자체 레지스터 블록을 얹었다(csr 베이스 + 0xc0000). 링크 상태도 IP 의
 *    LTSSM_STATUS 가 아니라 이 블록의 PCIE_PF_DBG 로 봐야 하고, 리셋 제어도
 *    여기에 있다. 그 차이를 공통 계층에 알리기 위해 두 개의 콜백 표
 *    (mobiveil_pab_ops, mobiveil_rp_ops)를 채워 건넨다.
 *
 *    이 트리에서 그 두 표를 실제로 채우는 파일은 이것 하나뿐이다.
 *    같은 디렉터리의 pcie-mobiveil-plat.c 는 채우지 않는다.
 *
 * 3) 링크 리셋 복구. 상대편이 링크 리셋을 걸면 PAB 내부 상태가 초기화되어
 *    probe 때 설정한 주소 창이 날아간다. 이 파일은 그 사건을 인터럽트로
 *    감지해 워크큐에서 하드웨어를 다시 세운다 — 이 디렉터리에서 유일하게
 *    런타임 복구 논리를 가진 파일이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 세 층으로 된 mobiveil 드라이버의 맨 위, SoC 층에 해당한다.
 *
 *   device_initcall
 *     -> platform_driver_probe() -> ls_g4_pcie_probe()   [이 파일]
 *          -> mobiveil_pcie_host_probe()                 (host.c)
 *               -> mobiveil_pcie_parse_dt()
 *               -> mobiveil_host_init()                  (host.c)
 *                    -> program_ob_windows() / program_ib_windows()
 *                                                        (pcie-mobiveil.c)
 *               -> mobiveil_pcie_interrupt_init()        (host.c)
 *                    -> rp->ops->interrupt_init
 *                         -> ls_g4_pcie_interrupt_init() [이 파일]
 *               -> mobiveil_bringup_link()               (pcie-mobiveil.c)
 *                    -> mobiveil_pcie_link_up()
 *                         -> ops->link_up
 *                              -> ls_g4_pcie_link_up()   [이 파일]
 *               -> pci_host_probe()                      (PCI 코어)
 *          -> ls_g4_pcie_enable_interrupt()               [이 파일]
 *
 * 런타임에는 다음 경로가 따로 돈다.
 *
 *   PCIe 링크 리셋
 *     -> ls_g4_pcie_isr()            [인터럽트 컨텍스트]
 *          -> ls_g4_pcie_disable_interrupt()
 *          -> schedule_delayed_work(1ms)
 *               -> ls_g4_pcie_reset() [워크큐, 프로세스 컨텍스트]
 *                    -> ls_g4_pcie_reinit_hw()
 *                         -> mobiveil_host_init(reinit=true)
 *
 * 실행 컨텍스트는 셋이 섞인다. probe 는 프로세스 컨텍스트이고 __init 이며,
 * ISR 은 인터럽트 컨텍스트, 복구는 잠들 수 있는 워크큐 컨텍스트다.
 * 복구가 usleep_range 를 쓰기 때문에 ISR 에서 직접 할 수 없고, 그것이
 * 워크큐를 끼워 넣은 이유다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽(이 파일을 부르는 쪽): platform 버스와 DT. compatible 이 맞으면
 *   커널이 ls_g4_pcie_probe() 를 부른다. 다만 builtin_platform_driver_probe
 *   를 쓰므로 등록 시점에 한 번만 매칭이 시도된다.
 * 아래쪽(이 파일이 부르는 쪽): pcie-mobiveil-host.c 의 공통 호스트 probe 와
 *   pcie-mobiveil.c 의 레지스터 접근자·창 설정·링크 대기.
 * 옆쪽: MSI 를 자체 처리하지 않고 DT 의 msi-parent 가 가리키는 외부
 *   인터럽트 컨트롤러에 맡긴다. probe 가 그 속성의 존재를 필수로 검사한다.
 *
 * 데이터 흐름. DT 의 csr_axi_slave 리소스가 공통 파싱을 거쳐
 * csr_axi_slave_base 에 담기고, 이 파일은 거기에 PCIE_PF_OFF 를 더해
 * NXP 전용 PF 레지스터에 닿는다. 반대 방향으로는 PAB 인터럽트 상태가
 * ls_g4_pcie_isr() 로 올라오고, 리셋 비트만 골라져 워크큐로 넘어간다.
 *
 * 공유 상태는 struct ls_g4_pcie 하나다. 브리지 private 영역에 놓이며
 * drvdata 에도 등록되어, 공통 계층이 넘겨준 struct mobiveil_pcie 포인터에서
 * to_ls_g4_pcie() 매크로로 되찾을 수 있다.
 *
 * === NVMe 관점 ===
 * drivers/nvme 는 이 파일의 어떤 심볼도 부르지 않는다(트리 전수 확인).
 * 관계는 토폴로지상의 것이다 — LX2160A 보드의 이 컨트롤러 아래에 NVMe SSD 를
 * 달면, 그 config 접근과 MMIO 가 여기서 켠 PIO 와 공통 계층이 설정한
 * 주소 창을 지난다. 완료 인터럽트(MSI)는 이 파일이 아니라 msi-parent 가
 * 가리키는 SoC 인터럽트 컨트롤러가 처리한다.
 *
 * 눈여겨볼 점은 리셋 복구다. 링크 리셋이 걸리면 이 파일이 주소 창을 다시
 * 세우는데, 그 사이 NVMe 큐의 진행 중인 I/O 는 하드웨어 관점에서 끊긴다.
 * 이 파일에는 상위 PCI/NVMe 계층에 그 사실을 알리는 코드가 없다 —
 * 복구 후 상태 정합을 누가 맞추는지는 이 트리에서 확인하지 못했다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct ls_g4_pcie          : 공통 struct mobiveil_pcie + 복구 워크 + IRQ 번호.
 * ls_g4_pcie_pf_readl/writel : NXP PF 블록 전용 32비트 접근자.
 *                              페이지 방식을 거치지 않는다.
 * ls_g4_pcie_link_up()       : PCIE_PF_DBG 의 LTSSM 으로 링크를 판정한다.
 *                              mobiveil_pab_ops.link_up 으로 등록된다.
 * ls_g4_pcie_interrupt_init(): "intr" IRQ 하나만 잡는다.
 *                              mobiveil_rp_ops.interrupt_init 으로 등록되어
 *                              공통 INTx/MSI 초기화를 통째로 대체한다.
 * ls_g4_pcie_isr()           : 리셋 비트만 보고 복구 워크를 예약한다.
 * ls_g4_pcie_reinit_hw()     : 리셋 완료를 폴링하고 창을 다시 세운 뒤
 *                              링크 재훈련을 기다린다.
 * ls_g4_pcie_reset()         : 워크큐 핸들러. 버스 리셋을 풀고 위 함수를 부른다.
 * ls_g4_pcie_probe()         : 준비를 마치고 공통 호스트 probe 에 넘긴다.
 *
 * === 이 파일을 읽을 때 알아 둘 점 ===
 * 코드에서 확인되는, 설명이 필요한 지점이 셋 있다. 각 함수 주석에 자세히
 * 적어 두었고 여기서는 위치만 밝힌다.
 *   - ls_g4_pcie_reset(): 재초기화 성공(0) 경로에서 인터럽트를 다시 켜지
 *     않고 실패 경로에서만 켠다. 반환값의 통상적 의미와 반대로 읽힌다.
 *   - ls_g4_pcie_reinit_hw(): 상류 영어 주석은 "clear PEX_RESET bit" 라고
 *     적혀 있으나 코드는 PF_DBG_PABR 비트를 세운다.
 *   - probe 의 두 ops 대입: 이 트리에서 ops 를 채우는 유일한 곳인데,
 *     읽는 쪽은 ops 자체의 NULL 검사 없이 역참조한다.
 * 셋 다 하드웨어 문서나 트리 밖 근거가 있어야 옳고 그름을 판단할 수 있어,
 * 여기서는 관찰된 사실만 적는다.
 */

/* [한국어] container_of, ARRAY_SIZE 등 기본 커널 매크로. */
#include <linux/kernel.h>
/* [한국어] IRQF_SHARED, irqreturn_t, devm_request_irq — 이 드라이버는 체인 핸들러가
 * 아니라 공유 인터럽트 핸들러 하나만 등록하므로 이 헤더가 필요하다. */
#include <linux/interrupt.h>
/* [한국어] __init 섹션 지정자. 아래 probe 가 __init 으로 표시된다. */
#include <linux/init.h>
/* [한국어] of_parse_phandle 등 DT 의 PCI 관련 헬퍼. msi-parent 확인에 쓴다. */
#include <linux/of_pci.h>
/* [한국어] of_platform 계열 선언. 이 파일에서 직접 쓰는 심볼은 보이지 않으나
 * 상류가 넣어 둔 것을 그대로 둔다. */
#include <linux/of_platform.h>
/* [한국어] of_irq 계열 선언. 마찬가지로 이 파일에서 직접 쓰는 심볼은 확인하지 못했다. */
#include <linux/of_irq.h>
/* [한국어] of_address 계열 선언. 위와 같다. */
#include <linux/of_address.h>
/* [한국어] PCI_BRIDGE_CONTROL, PCI_BRIDGE_CTL_BUS_RESET 같은 config 공간 상수와
 * pci_host_bridge 관련 코어 API. */
#include <linux/pci.h>
/* [한국어] struct platform_device, platform_get_irq_byname, platform_set_drvdata,
 * 그리고 builtin_platform_driver_probe 매크로. */
#include <linux/platform_device.h>
/* [한국어] struct resource 정의. 리소스 타입을 다루는 코드가 이 헤더에 기댄다. */
#include <linux/resource.h>
/* [한국어] syscon 접근 선언. 이 파일에서 실제로 쓰는 심볼은 찾지 못했다 —
 * 예전 판에서 남은 포함으로 보이나 근거를 확인하지는 못했다. */
#include <linux/mfd/syscon.h>
/* [한국어] regmap 선언. 위와 같은 이유로 남아 있는 것으로 보인다. */
#include <linux/regmap.h>

/* [한국어] 이 디렉터리 공통 헤더 — struct mobiveil_pcie, 두 ops 구조체,
 * PAB_ 레지스터 상수와 mobiveil_csr_ 접근자 래퍼가 모두 여기서 온다. */
#include "pcie-mobiveil.h"

/* LUT and PF control registers */
/* [한국어] LUT(Look-Up Table) 블록이 csr_axi_slave 베이스에서 떨어진 거리.
 * 다만 이 파일 안에서 이 상수를 실제로 쓰는 코드는 찾지 못했다 — 정의만 있다. */
#define PCIE_LUT_OFF			0x80000
/* [한국어] PF(Physical Function) 제어 블록이 csr_axi_slave 베이스에서 떨어진 거리(0xc0000).
 * 아래 pf_readl / pf_writel 이 모든 접근에 이 값을 더한다.
 * 이 블록은 Mobiveil IP 의 PAB 레지스터가 아니라 NXP 가 덧붙인 래퍼 쪽이라,
 * 페이지 방식을 거치지 않고 ioread32 로 직접 접근한다. */
#define PCIE_PF_OFF			0xc0000
/* [한국어] PF 블록 안의 인터럽트 상태 레지스터 오프셋. */
#define PCIE_PF_INT_STAT		0x18
/* [한국어] 그 레지스터에서 'PAB 가 리셋 상태에 들어갔다'를 알리는 비트(31).
 * 읽는 자: ls_g4_pcie_reinit_hw() 가 리셋 진입을 기다리는 폴링 조건. */
#define PF_INT_STAT_PABRST		BIT(31)

/* [한국어] PF 블록 안의 디버그 레지스터 오프셋. LTSSM 상태와 리셋 제어 비트가
 * 한 레지스터에 같이 들어 있다. */
#define PCIE_PF_DBG			0x7fc
/* [한국어] 그 레지스터에서 LTSSM 상태 코드를 뽑는 마스크(하위 6비트).
 * 공통 헤더의 LTSSM_STATUS_L0_MASK 와 값이 같지만, 대상 레지스터가
 * 다르므로 여기서 따로 정의한다. */
#define PF_DBG_LTSSM_MASK		0x3f
/* [한국어] LTSSM 이 L0(정상 상태)일 때의 코드값(0x2d).
 * 이 SoC 는 공통 LTSSM_STATUS 대신 이 PF 레지스터로 링크를 판정한다. */
#define PF_DBG_LTSSM_L0			0x2d /* L0 state */
/* [한국어] PF_DBG 레지스터의 write-enable 비트(31).
 * 이 비트를 먼저 세워야 아래 PABR 비트에 대한 쓰기가 하드웨어에 먹는다 —
 * 그래서 리셋 해제 절차가 '켜고 → 쓰고 → 끄고' 세 단계가 된다. */
#define PF_DBG_WE			BIT(31)
/* [한국어] PF_DBG 레지스터의 PAB 리셋 제어 비트(27). */
#define PF_DBG_PABR			BIT(27)

/* [한국어] @x 로 받은 struct mobiveil_pcie 포인터에서 이 SoC 의 struct ls_g4_pcie 를
 * 되찾는 매크로. container_of 가 아니라 platform drvdata 를 경유한다.
 * 그래서 drvdata 가 설정되기 전에는 쓸 수 없다 — probe 가
 * platform_set_drvdata() 를 mobiveil_pcie_host_probe() 보다 먼저 부르는 이유다. */
#define to_ls_g4_pcie(x)		platform_get_drvdata((x)->pdev)

/* [한국어] Layerscape Gen4 컨트롤러 한 대의 상태.
 * 공통 struct mobiveil_pcie 를 첫 필드로 감싸는 형태라, 공통 코드에는
 * &pcie->pci 를 넘기고 되돌아올 때는 to_ls_g4_pcie() 로 복원한다.
 * 실체는 devm_pci_alloc_host_bridge() 가 잡아 준 브리지 private 영역에 있다. */
struct ls_g4_pcie {
	/* [한국어] 공통 컨트롤러 상태. 첫 필드지만 위 매크로가 drvdata 를 쓰므로
	 * 실제 복원에는 이 위치가 이용되지 않는다.
	 * 설정자: ls_g4_pcie_probe() 가 pdev, ops, rp.ops, rp.bridge 를 채운다.
	 * 읽는 자: 공통 계층 전부(host.c, pcie-mobiveil.c)와 이 파일의 모든 함수.
	 * 값 범위: 값 필드라 항상 유효하다.
	 * 동기화: probe 이후에는 대부분 읽기 전용이고, 창 카운터만 리셋 복구 때
	 * 다시 0 으로 돌아간다. */
	struct mobiveil_pcie pci;
	/* [한국어] PCIe 링크 리셋을 감지했을 때 하드웨어 재초기화를 미뤄 실행하는 워크.
	 * 설정자: ls_g4_pcie_probe() 의 INIT_DELAYED_WORK(핸들러는 ls_g4_pcie_reset).
	 * 읽는 자: ls_g4_pcie_isr() 이 schedule_delayed_work 로 1ms 뒤 실행을 건다.
	 * 값 범위: 커널 워크큐가 관리하는 구조체.
	 * 동기화: 재초기화는 usleep_range 로 잠들 수 있어 인터럽트 컨텍스트에서
	 * 할 수 없다. 그래서 ISR 은 인터럽트만 끄고 실제 작업을 이 워크로 넘긴다. */
	struct delayed_work dwork;
	/* [한국어] 이 드라이버가 등록한 인터럽트의 Linux IRQ 번호(virq).
	 * 설정자: ls_g4_pcie_interrupt_init() 의 platform_get_irq_byname(pdev, "intr").
	 * 읽는 자: 같은 함수의 devm_request_irq.
	 * 값 범위: 양수 virq. 음수면 오류로 그 자리에서 반환된다.
	 * 동기화: probe 때 한 번 정해지고 이후 바뀌지 않는다.
	 * 주의: 공통 struct mobiveil_root_port 에도 irq 필드가 있지만,
	 * 이 드라이버는 그쪽을 쓰지 않고 자기 필드를 따로 둔다. */
	int irq;
};

/* [한국어] ls_g4_pcie_pf_readl - PF 제어 블록의 32비트 레지스터를 읽는다.
 * 
 * @pcie: 대상 컨트롤러(이 SoC 전용 구조체).
 * @off: PF 블록 안에서의 오프셋(PCIE_PF_DBG 등).
 * @return: 읽은 32비트 값.
 * 
 * PF 블록은 Mobiveil 의 PAB 영역과 달리 페이지 방식을 쓰지 않는다.
 * 그래서 공통 mobiveil_csr_readl() 을 거치지 않고 베이스 + PCIE_PF_OFF + off
 * 로 곧장 ioread32 한다 — 페이지 선택 쓰기가 끼지 않으므로 접근이 한 번으로 끝난다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트(리셋 복구 워크)와 링크 판정 경로.
 * 호출자: ls_g4_pcie_link_up(), ls_g4_pcie_reinit_hw().
 * 피호출자: ioread32().
 * 에러 경로: 없다 — MMIO 읽기는 실패를 보고하지 않는다.
 * 
 * 호출 체인:
 *   ls_g4_pcie_link_up() / ls_g4_pcie_reinit_hw() → [이 함수] → ioread32() */
static inline u32 ls_g4_pcie_pf_readl(struct ls_g4_pcie *pcie, u32 off)
{
	/* [한국어] csr_axi_slave 가상 베이스에 PF 블록 오프셋(0xc0000)과 레지스터 오프셋을
	 * 더해 읽는다. csr_axi_slave_base 는 mobiveil_pcie_parse_dt() 가 매핑한 것이라,
	 * 이 함수는 공통 DT 파싱이 끝난 뒤에만 안전하다. */
	return ioread32(pcie->pci.csr_axi_slave_base + PCIE_PF_OFF + off);
}

/* [한국어] ls_g4_pcie_pf_writel - PF 제어 블록의 32비트 레지스터에 쓴다.
 * 
 * @pcie: 대상 컨트롤러.
 * @off: PF 블록 안에서의 오프셋.
 * @val: 쓸 값.
 * @return: 없음.
 * 
 * 인자 순서가 위 읽기 함수와 짝을 맞춰 (오프셋, 값) 순이다 —
 * 공통 mobiveil_csr_write() 의 (값, 오프셋) 순서와 반대라 헷갈리기 쉽다.
 * 
 * 실행 컨텍스트: 리셋 복구 워크(프로세스 컨텍스트)에서만 불린다.
 * 호출자: ls_g4_pcie_reinit_hw() 세 번(write enable 켜기, PABR 쓰기, 끄기).
 * 피호출자: iowrite32().
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   ls_g4_pcie_reinit_hw() → [이 함수] → iowrite32() */
static inline void ls_g4_pcie_pf_writel(struct ls_g4_pcie *pcie,
					u32 off, u32 val)
{
	/* [한국어] 값과 주소 순서가 iowrite32 관례(값, 주소)를 따른다.
	 * 주소 계산은 위 읽기 함수와 동일하다. */
	iowrite32(val, pcie->pci.csr_axi_slave_base + PCIE_PF_OFF + off);
}

/* [한국어] ls_g4_pcie_link_up - 이 SoC 방식으로 링크 상태를 판정한다.
 * 
 * @pci: 공통 계층이 넘겨 준 struct mobiveil_pcie 포인터.
 * @return: LTSSM 이 L0 이면 true, 아니면 false.
 * 
 * 공통 mobiveil_pcie_link_up() 은 PAB 영역의 LTSSM_STATUS 를 보지만,
 * Layerscape Gen4 는 NXP 가 덧붙인 PF 블록의 PCIE_PF_DBG 를 봐야 한다.
 * 그 차이를 흡수하려고 mobiveil_pab_ops.link_up 콜백으로 등록된다.
 * 
 * 동작: PF_DBG 를 읽어 하위 6비트를 뽑고 L0 코드(0x2d)와 비교한다.
 * 
 * 실행 컨텍스트: probe 중 링크 대기(프로세스 컨텍스트)와
 * 리셋 복구 워크. 인터럽트 컨텍스트에서는 불리지 않는다.
 * 호출자: pcie-mobiveil.c 의 mobiveil_pcie_link_up() 이 ops 를 통해,
 * 그리고 이 파일의 ls_g4_pcie_reinit_hw() 가 직접.
 * 피호출자: ls_g4_pcie_pf_readl().
 * 에러 경로: 없다 — 판정 결과만 돌려주고, 시간 초과 판단은 호출자 몫이다.
 * 
 * 호출 체인:
 *   mobiveil_bringup_link() → mobiveil_pcie_link_up() → ops->link_up → [이 함수] */
static bool ls_g4_pcie_link_up(struct mobiveil_pcie *pci)
{
	/* [한국어] 공통 포인터에서 이 SoC 구조체를 복원한다. drvdata 경유이므로
	 * probe 가 platform_set_drvdata() 를 먼저 부른 뒤여야 한다 —
	 * 실제로 probe 는 그것을 mobiveil_pcie_host_probe() 앞에서 한다. */
	struct ls_g4_pcie *pcie = to_ls_g4_pcie(pci);
	/* [한국어] PF_DBG 레지스터에서 읽은 값을 담을 지역 변수. */
	u32 state;

	/* [한국어] PF 디버그 레지스터를 읽는다. 여기에 LTSSM 상태가 하위 6비트로 들어 있다. */
	state = ls_g4_pcie_pf_readl(pcie, PCIE_PF_DBG);
	/* [한국어] 상태 코드만 뽑아 L0(0x2d)과 비교한다. L0 는 링크 훈련이 끝나고
	 * 정상적으로 데이터를 주고받는 상태를 뜻한다. */
	return (state & PF_DBG_LTSSM_MASK) == PF_DBG_LTSSM_L0;
}

/* [한국어] ls_g4_pcie_disable_interrupt - 이 컨트롤러의 모든 PAB 인터럽트를 끈다.
 * 
 * @pcie: 대상 컨트롤러.
 * @return: 없음.
 * 
 * 인터럽트 활성화 레지스터에 0 을 써서 모든 소스를 막는다.
 * 리셋을 감지한 직후 부르는데, 이유는 재초기화가 끝날 때까지 같은
 * 리셋 인터럽트가 계속 올라와 워크를 반복해서 거는 것을 막기 위함이다.
 * 
 * 실행 컨텍스트: 인터럽트 컨텍스트(ls_g4_pcie_isr 안에서 불린다).
 * 호출자: ls_g4_pcie_isr().
 * 피호출자: mobiveil_csr_writel().
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   ls_g4_pcie_isr() → [이 함수] → mobiveil_csr_writel() */
static void ls_g4_pcie_disable_interrupt(struct ls_g4_pcie *pcie)
{
	/* [한국어] 공통 구조체 주소를 꺼낸다. 첫 필드이므로 실제로는 pcie 와 같은 주소지만,
	 * 타입을 맞추려고 명시적으로 &pcie->pci 를 쓴다. */
	struct mobiveil_pcie *mv_pci = &pcie->pci;

	/* [한국어] 활성화 레지스터 전체를 0 으로 만든다 — 개별 비트를 지우는 것이 아니라
	 * 통째로 덮어쓰므로, 나중에 다시 켤 때는 ls_g4_pcie_enable_interrupt() 가
	 * 필요한 비트를 처음부터 다시 조립한다. */
	mobiveil_csr_writel(mv_pci, 0, PAB_INTP_AMBA_MISC_ENB);
}

/* [한국어] ls_g4_pcie_enable_interrupt - 이 컨트롤러가 볼 인터럽트 소스를 켠다.
 * 
 * @pcie: 대상 컨트롤러.
 * @return: 없음.
 * 
 * 먼저 상태 레지스터를 통째로 지운 뒤 활성화 비트를 세운다. 순서가 중요하다 —
 * 남아 있던 상태 비트를 먼저 지우지 않고 활성화하면, 켜자마자 과거의
 * 이벤트로 인터럽트가 올라온다.
 * 
 * 켜는 소스는 INTx 네 핀, MSI, 리셋, uncorrectable error, 그리고 PM/EC 계열이다.
 * 다만 이 드라이버의 ISR 이 실제로 처리하는 것은 리셋 비트뿐이고,
 * 나머지는 상태만 지우고 넘어간다. INTx 와 MSI 는 DT 의 msi-parent 가
 * 가리키는 외부 컨트롤러가 담당한다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트 — probe 끝부분과 리셋 복구 워크.
 * 호출자: ls_g4_pcie_probe(), ls_g4_pcie_reset().
 * 피호출자: mobiveil_csr_writel().
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   ls_g4_pcie_probe() / ls_g4_pcie_reset() → [이 함수] → mobiveil_csr_writel() */
static void ls_g4_pcie_enable_interrupt(struct ls_g4_pcie *pcie)
{
	/* [한국어] 공통 구조체 주소. */
	struct mobiveil_pcie *mv_pci = &pcie->pci;
	/* [한국어] 조립할 활성화 비트 묶음을 담을 지역 변수. */
	u32 val;

	/* Clear the interrupt status */
	/* [한국어] 상태 레지스터는 write-1-to-clear 라 0xffffffff 를 쓰면 세워진 비트가 모두 지워진다.
	 * 활성화보다 먼저 해야 묵은 이벤트가 곧바로 인터럽트로 튀지 않는다. */
	mobiveil_csr_writel(mv_pci, 0xffffffff, PAB_INTP_AMBA_MISC_STAT);

	/* [한국어] 켤 소스를 OR 로 모은다. INTx 네 핀과 MSI 는 상태만 보고 넘기는 소스이고,
	 * 이 드라이버가 실제로 반응하는 것은 PAB_INTP_RESET 하나다. */
	val = PAB_INTP_INTX_MASK | PAB_INTP_MSI | PAB_INTP_RESET |
	      PAB_INTP_PCIE_UE | PAB_INTP_IE_PMREDI | PAB_INTP_IE_EC;
	/* [한국어] 조립한 마스크를 활성화 레지스터에 통째로 기록한다.
	 * 읽고-고치고-쓰기가 아니라 덮어쓰기라, 이 함수가 곧 '켜야 할 소스의 전체 목록'이다. */
	mobiveil_csr_writel(mv_pci, val, PAB_INTP_AMBA_MISC_ENB);
}

/* [한국어] ls_g4_pcie_reinit_hw - 링크 리셋 뒤 컨트롤러 하드웨어를 다시 세운다.
 * 
 * @pcie: 대상 컨트롤러.
 * @return: 0 성공, -EIO 면 리셋 진입 대기나 링크 재훈련이 시간 안에 끝나지 않음.
 * 
 * 왜 필요한가: 상대편이 링크 리셋을 걸면 PAB 내부 상태가 초기화되어
 * probe 때 설정한 주소 창과 PIO 설정이 날아간다. 그래서 리셋이 끝나기를
 * 기다렸다가 mobiveil_host_init() 을 다시 돌려 창을 재설정해야 한다.
 * 
 * 동작 단계:
 *   1) PAB 가 리셋 상태에 들어가고(PABRST 비트) 진행 중인 트랜잭션이
 *      모두 빠지기를(PAB_ACTIVITY_STAT 가 0) 최대 100회 폴링한다.
 *   2) PF_DBG 의 write-enable 을 세우고 → PABR 비트를 쓰고 → write-enable 을
 *      내려 리셋 제어를 반영한다.
 *   3) mobiveil_host_init(reinit=true) 로 창과 PIO 를 다시 설정한다.
 *      reinit 이 true 라 버스 번호 재설정은 건너뛴다.
 *   4) 링크가 다시 L0 이 될 때까지 최대 100회 기다린다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트 전용이다 — usleep_range 로 잠들기 때문에
 * 인터럽트 컨텍스트에서는 부를 수 없다. 그래서 ISR 이 워크큐로 넘긴다.
 * 호출자: ls_g4_pcie_reset() (워크큐 핸들러).
 * 피호출자: ls_g4_pcie_pf_readl/pf_writel, mobiveil_csr_readl,
 *   mobiveil_host_init, ls_g4_pcie_link_up, usleep_range.
 * 에러 경로: 두 폴링 중 하나라도 시간 초과면 로그를 남기고 -EIO 를 돌려준다.
 *   호출자가 그 값을 어떻게 다루는지는 ls_g4_pcie_reset() 주석 참조.
 * 
 * 호출 체인:
 *   ls_g4_pcie_isr() → (워크큐) → ls_g4_pcie_reset() → [이 함수]
 *     → mobiveil_host_init() → program_ob_windows() 등 */
static int ls_g4_pcie_reinit_hw(struct ls_g4_pcie *pcie)
{
	/* [한국어] 공통 구조체 주소 — 공통 계층 함수에 넘기기 위한 것. */
	struct mobiveil_pcie *mv_pci = &pcie->pci;
	/* [한국어] 오류 로그용 device 포인터. pdev 는 probe 에서 채워져 있다. */
	struct device *dev = &mv_pci->pdev->dev;
	/* [한국어] val 은 PF 인터럽트 상태, act_stat 은 PAB 활동 상태를 담는다. */
	u32 val, act_stat;
	/* [한국어] 폴링 횟수 한도. 감소 연산의 결과로 -1 이 되는지를 보고 시간 초과를 판정하므로
	 * unsigned 가 아니라 int 여야 한다. */
	int to = 100;

	/* Poll for pab_csb_reset to set and PAB activity to clear */
	/* [한국어] 먼저 한 번 자고 나서 읽는 do-while 구조 — 리셋 신호가 반영될 시간을
	 * 최소 한 번은 주려는 것이다. */
	do {
		/* [한국어] 10~15us 를 자며 CPU 를 놓아 준다. 최대 100회이므로 이 폴링은 길어야 1.5ms 남짓이다. */
		usleep_range(10, 15);
		/* [한국어] PF 인터럽트 상태를 읽어 PAB 가 리셋 상태에 들어갔는지 확인할 재료를 얻는다. */
		val = ls_g4_pcie_pf_readl(pcie, PCIE_PF_INT_STAT);
		/* [한국어] PAB 활동 상태를 읽는다. 0 이 아니면 아직 처리 중인 트랜잭션이 남아 있다는 뜻이다.
		 * 이쪽은 PAB 영역이라 공통 접근자를 쓴다 — 즉 페이지 선택 쓰기가 함께 일어난다. */
		act_stat = mobiveil_csr_readl(mv_pci, PAB_ACTIVITY_STAT);
	/* [한국어] 둘 중 하나라도 만족되지 않으면(리셋 미진입 이거나 아직 활동 중) 계속 돈다.
	 * to-- 는 뒤에 놓여, 조건이 먼저 참일 때만 감소한다.
	 * 한도를 다 쓰면 to 가 -1 이 되면서 루프가 끝난다. */
	} while (((val & PF_INT_STAT_PABRST) == 0 || act_stat) && to--);
	/* [한국어] to 가 음수라는 것은 위 루프가 조건 만족이 아니라 횟수 소진으로 끝났다는 뜻이다. */
	if (to < 0) {
		/* [한국어] 무엇을 기다리다 실패했는지 남긴다 — PABRST 진입과 PAB 활동 정지 둘 다를 가리킨다. */
		dev_err(dev, "Poll PABRST&PABACT timeout\n");
		/* [한국어] 재초기화를 포기하고 I/O 오류로 보고한다. 호출자가 이 값을 보고
		 * 인터럽트를 다시 켤지 결정한다. */
		return -EIO;
	}

	/* clear PEX_RESET bit in PEX_PF0_DBG register */
	/* [한국어] PF_DBG 를 읽어 현재 값을 확보한다 — 다른 비트를 보존한 채 고치기 위함이다. */
	val = ls_g4_pcie_pf_readl(pcie, PCIE_PF_DBG);
	/* [한국어] write-enable 비트를 세운다. 이 비트 없이는 아래 PABR 쓰기가 먹지 않는다. */
	val |= PF_DBG_WE;
	/* [한국어] write-enable 을 먼저 하드웨어에 반영한다. 한 번의 쓰기로 WE 와 PABR 을
	 * 동시에 세우지 않고 나눈 것은, IP 가 WE 가 이미 서 있는 상태에서의
	 * 쓰기만 받아들이기 때문으로 보인다. */
	ls_g4_pcie_pf_writel(pcie, PCIE_PF_DBG, val);

	/* [한국어] WE 가 켜진 상태의 값을 다시 읽는다. */
	val = ls_g4_pcie_pf_readl(pcie, PCIE_PF_DBG);
	/* [한국어] PAB 리셋 제어 비트를 세운다.
	 * 주의: 바로 위 상류 주석은 'clear PEX_RESET bit' 라고 적혀 있지만,
	 * 코드는 |= 로 비트를 세운다. 둘 중 어느 쪽이 하드웨어의 실제 의미인지는
	 * 이 트리 안에서 확인하지 못했다 — IP 문서가 있어야 판단할 수 있다. */
	val |= PF_DBG_PABR;
	/* [한국어] PABR 을 반영한다. */
	ls_g4_pcie_pf_writel(pcie, PCIE_PF_DBG, val);

	/* [한국어] WE 를 내리기 위해 값을 다시 읽는다. */
	val = ls_g4_pcie_pf_readl(pcie, PCIE_PF_DBG);
	/* [한국어] write-enable 비트를 지운다 — 이후의 우발적인 쓰기가 리셋 제어를 건드리지 못하게 한다. */
	val &= ~PF_DBG_WE;
	/* [한국어] WE 를 내린 값을 기록해 세 단계 절차를 마친다. */
	ls_g4_pcie_pf_writel(pcie, PCIE_PF_DBG, val);

	/* [한국어] 창·PIO·클래스 코드 등 호스트 설정을 다시 세운다.
	 * reinit=true 라 버스 번호(PCI_PRIMARY_BUS) 재설정은 건너뛴다 —
	 * 열거된 버스 번호는 리셋 후에도 그대로 유지해야 하기 때문이다.
	 * 이 트리의 구현은 언제나 0 을 돌려주므로 반환값을 확인하지 않는다. */
	mobiveil_host_init(mv_pci, true);

	/* [한국어] 링크 재훈련 대기를 위해 한도를 다시 채운다. */
	to = 100;
	/* [한국어] 링크가 L0 이 될 때까지 기다린다. 조건이 먼저 평가되는 while 이므로,
	 * 이미 링크가 올라와 있으면 한 번도 자지 않고 빠져나간다. */
	while (!ls_g4_pcie_link_up(mv_pci) && to--)
		/* [한국어] 200~250us 를 잔다. 최대 100회이므로 이 대기는 길어야 25ms 정도다 —
		 * 공통 mobiveil_bringup_link() 의 약 1초와 비교하면 훨씬 짧다. */
		usleep_range(200, 250);
	/* [한국어] 횟수를 다 쓰고도 링크가 올라오지 않은 경우. */
	if (to < 0) {
		/* [한국어] 링크 훈련 시간 초과를 남긴다. */
		dev_err(dev, "PCIe link training timeout\n");
		/* [한국어] 창은 다시 세웠지만 링크가 없으므로 실패로 보고한다. */
		return -EIO;
	}

	/* [한국어] 리셋 진입 대기, 리셋 해제, 호스트 재설정, 링크 재훈련이 모두 성공했다. */
	return 0;
}

/* [한국어] ls_g4_pcie_isr - 이 컨트롤러의 인터럽트 핸들러.
 * 
 * @irq: 발생한 Linux IRQ 번호(이 함수는 쓰지 않는다).
 * @dev_id: devm_request_irq 에 넘겼던 struct ls_g4_pcie 포인터.
 * @return: 자기 인터럽트가 아니면 IRQ_NONE, 처리했으면 IRQ_HANDLED.
 * 
 * 공통 host.c 의 mobiveil_pcie_isr() 과 달리 이 핸들러는 INTx 나 MSI 를
 * 분배하지 않는다. 그 둘은 DT 의 msi-parent 가 가리키는 외부 인터럽트
 * 컨트롤러가 맡고, 이 핸들러는 오직 링크 리셋 이벤트만 본다.
 * 
 * 동작: 상태 레지스터를 읽어 비어 있으면 남의 인터럽트로 보고 물러난다.
 * 리셋 비트가 서 있으면 인터럽트를 모두 끄고 재초기화를 워크큐에 건다.
 * 마지막으로 읽은 상태 비트를 그대로 되써서 지운다.
 * 
 * 실행 컨텍스트: 인터럽트 컨텍스트. IRQF_SHARED 로 등록되므로
 * 다른 장치와 같은 선을 나눠 쓸 수 있고, 그래서 IRQ_NONE 반환이 의미를 가진다.
 * 호출자: 커널 인터럽트 처리부.
 * 피호출자: mobiveil_csr_readl/writel, ls_g4_pcie_disable_interrupt,
 *   schedule_delayed_work.
 * 에러 경로: 별도 오류 처리는 없다. 재초기화 실패는 워크 쪽에서 다뤄진다.
 * 
 * 호출 체인:
 *   커널 IRQ 처리부 → [이 함수] → schedule_delayed_work() → ls_g4_pcie_reset() */
static irqreturn_t ls_g4_pcie_isr(int irq, void *dev_id)
{
	/* [한국어] devm_request_irq 에 넘겼던 컨텍스트를 되찾는다.
	 * 여기서는 to_ls_g4_pcie 매크로를 쓰지 않는데, 넘겨받은 것이 이미
	 * SoC 구조체 포인터이기 때문이다. */
	struct ls_g4_pcie *pcie = (struct ls_g4_pcie *)dev_id;
	/* [한국어] 공통 구조체 주소 — 상태 레지스터 접근에 쓴다. */
	struct mobiveil_pcie *mv_pci = &pcie->pci;
	/* [한국어] 읽은 인터럽트 상태를 담을 변수. */
	u32 val;

	/* [한국어] 인터럽트 상태 레지스터를 읽는다. 이 접근은 PAB 영역이라 내부적으로
	 * 페이지 선택 쓰기를 동반한다 — 인터럽트 컨텍스트에서도 마찬가지다. */
	val = mobiveil_csr_readl(mv_pci, PAB_INTP_AMBA_MISC_STAT);
	/* [한국어] 세워진 비트가 하나도 없다는 것은 이 인터럽트가 우리 것이 아니라는 뜻이다. */
	if (!val)
		/* [한국어] IRQF_SHARED 로 선을 공유하므로, 자기 것이 아님을 IRQ_NONE 으로 알려
		 * 커널이 다음 핸들러를 부르게 한다. */
		return IRQ_NONE;

	/* [한국어] 링크 쪽에서 리셋이 관측된 경우. 이 드라이버가 실제로 처리하는 유일한 이벤트다. */
	if (val & PAB_INTP_RESET) {
		/* [한국어] 재초기화가 끝날 때까지 같은 인터럽트가 계속 올라와 워크를 거듭 거는 것을
		 * 막으려고 모든 소스를 먼저 끈다. */
		ls_g4_pcie_disable_interrupt(pcie);
		/* [한국어] 실제 복구는 잠들 수 있는 작업이라 인터럽트 컨텍스트에서 할 수 없다.
		 * 1ms 뒤에 실행되도록 워크큐에 넘긴다 — 지연을 두는 것은 리셋 신호가
		 * 안정될 시간을 주려는 것으로 보인다. */
		schedule_delayed_work(&pcie->dwork, msecs_to_jiffies(1));
	}

	/* [한국어] 읽었던 상태 비트를 그대로 되써서 지운다(write-1-to-clear).
	 * 리셋이 아닌 소스들도 여기서 함께 지워지므로, 이 드라이버는 그것들을
	 * '켜 두되 상태만 비우는' 방식으로 다룬다. */
	mobiveil_csr_writel(mv_pci, val, PAB_INTP_AMBA_MISC_STAT);

	/* [한국어] 우리 인터럽트였고 처리를 마쳤다. */
	return IRQ_HANDLED;
}

/* [한국어] ls_g4_pcie_interrupt_init - 이 SoC 의 인터럽트 초기화(공통 경로 대체).
 * 
 * @mv_pci: 공통 계층이 넘겨 준 컨트롤러 포인터.
 * @return: 0 성공, 음수 errno 실패.
 * 
 * 왜 필요한가: 공통 host.c 의 기본 경로는 자체 INTx/MSI 도메인을 만들고
 * apb_csr 리소스를 매핑한다. 하지만 Layerscape Gen4 는 MSI 를 SoC 의
 * 외부 인터럽트 컨트롤러(DT 의 msi-parent)에 맡기므로 그 초기화가 필요 없다.
 * 그래서 mobiveil_rp_ops.interrupt_init 콜백으로 등록해 기본 경로를 통째로 대체한다.
 * 이 함수가 하는 일은 'intr' 이라는 이름의 IRQ 하나를 잡아 자기 핸들러를 다는 것뿐이다.
 * 
 * 실행 컨텍스트: probe 중 프로세스 컨텍스트.
 * 호출자: host.c 의 mobiveil_pcie_interrupt_init() 이 rp->ops->interrupt_init 을 통해.
 * 피호출자: platform_get_irq_byname(), devm_request_irq().
 * 에러 경로: IRQ 를 못 찾으면 그 음수값을, 등록에 실패하면 그 errno 를 돌려준다.
 *   어느 쪽이든 mobiveil_pcie_host_probe() 가 'Interrupt init failed' 를 찍고 probe 를 접는다.
 * 
 * 호출 체인:
 *   ls_g4_pcie_probe() → mobiveil_pcie_host_probe()
 *     → mobiveil_pcie_interrupt_init() → rp->ops->interrupt_init → [이 함수] */
static int ls_g4_pcie_interrupt_init(struct mobiveil_pcie *mv_pci)
{
	/* [한국어] 공통 포인터에서 SoC 구조체를 복원한다. probe 가 platform_set_drvdata 를
	 * mobiveil_pcie_host_probe 보다 먼저 불렀기에 여기서 유효하다. */
	struct ls_g4_pcie *pcie = to_ls_g4_pcie(mv_pci);
	/* [한국어] 리소스와 IRQ 를 찾을 platform 장치. */
	struct platform_device *pdev = mv_pci->pdev;
	/* [한국어] devm_request_irq 의 소유자이자 로그 대상이 될 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] devm_request_irq 결과를 담을 변수. */
	int ret;

	/* [한국어] DT 의 interrupt-names 에서 "intr" 로 이름 붙은 인터럽트를 찾는다.
	 * 번호가 아니라 이름으로 찾으므로, DT 에 인터럽트가 여러 개 나열돼 있어도
	 * 순서에 의존하지 않는다. */
	pcie->irq = platform_get_irq_byname(pdev, "intr");
	/* [한국어] 이름을 못 찾았거나 매핑에 실패한 경우 음수가 돌아온다. */
	if (pcie->irq < 0)
		/* [한국어] -EPROBE_DEFER 를 포함할 수 있으므로 값을 그대로 전달해야 한다 —
		 * 임의의 errno 로 바꾸면 지연 재시도 기회를 잃는다. */
		return pcie->irq;

	/* [한국어] 핸들러를 등록한다. devm_ 판이라 드라이버가 떨어질 때 자동으로 해제된다.
	 * IRQF_SHARED 는 이 인터럽트 선을 다른 장치와 나눠 쓸 수 있다는 뜻이고,
	 * 마지막 인자 pcie 가 ISR 의 dev_id 로 되돌아온다. */
	ret = devm_request_irq(dev, pcie->irq, ls_g4_pcie_isr,
			       IRQF_SHARED, pdev->name, pcie);
	/* [한국어] 등록 실패 — 이미 다른 방식(비공유 등)으로 잡혀 있는 경우 등이 여기 걸린다. */
	if (ret) {
		/* [한국어] 실패 원인을 errno 와 함께 남긴다. */
		dev_err(dev, "Can't register PCIe IRQ, errno = %d\n", ret);
		/* [한국어] 실패를 그대로 위로 올려 probe 를 접게 한다. */
		return  ret;
	}

	/* [한국어] 인터럽트 준비 완료. 실제로 인터럽트를 '켜는' 것은 probe 끝부분의
	 * ls_g4_pcie_enable_interrupt() 이다 — 핸들러가 준비되기 전에 인터럽트가
	 * 올라오는 일을 피하려는 순서다. */
	return 0;
}

/* [한국어] ls_g4_pcie_reset - 링크 리셋 뒤 복구를 수행하는 워크큐 핸들러.
 * 
 * @work: 워크큐가 넘겨 준 work_struct. 여기서 delayed_work 를 거쳐
 *   struct ls_g4_pcie 를 복원한다.
 * @return: 없음.
 * 
 * 왜 필요한가: 복구 절차가 usleep_range 로 잠들기 때문에 ISR 안에서
 * 직접 할 수 없다. ISR 은 인터럽트만 끄고 이 워크에 실제 작업을 넘긴다.
 * 
 * 동작 단계:
 *   1) 브리지 제어 레지스터의 secondary bus reset 비트를 내려
 *      하위 버스를 리셋 상태에서 풀어 준다.
 *   2) ls_g4_pcie_reinit_hw() 로 리셋 완료를 기다리고 창을 다시 세운다.
 *   3) 그 결과에 따라 인터럽트를 다시 켠다.
 * 
 * 실행 컨텍스트: 워크큐(프로세스 컨텍스트). 잠들 수 있다.
 * 호출자: 워크큐 코어. 예약은 ls_g4_pcie_isr() 이 한다.
 * 피호출자: mobiveil_csr_readw/writew, ls_g4_pcie_reinit_hw,
 *   ls_g4_pcie_enable_interrupt.
 * 에러 경로: 재초기화 실패 시 로그는 하위 함수가 남긴다.
 * 
 * 주의(코드에서 확인되는 사실): 3)의 분기가 반환값의 통상적 의미와
 * 반대 방향으로 읽힌다. ls_g4_pcie_reinit_hw() 는 성공에 0 을 돌려주는데,
 * 이 함수는 !0 이 참이 되는 성공 경로에서 곧바로 return 하고,
 * 실패(-EIO) 경로에서만 ls_g4_pcie_enable_interrupt() 를 부른다.
 * 이 트리 안에서 인터럽트를 다시 켜는 곳은 여기와 probe 끝 두 군데뿐이므로,
 * 성공적으로 복구된 뒤에는 이 컨트롤러의 PAB 인터럽트가 꺼진 채 남는다.
 * 그것이 의도인지 실수인지는 이 트리만으로 판단할 근거를 찾지 못했다.
 * 
 * 호출 체인:
 *   ls_g4_pcie_isr() → schedule_delayed_work() → 워크큐 → [이 함수]
 *     → ls_g4_pcie_reinit_hw() → mobiveil_host_init() */
static void ls_g4_pcie_reset(struct work_struct *work)
{
	/* [한국어] 받은 work_struct 를 감싸고 있는 delayed_work 로 되돌린다. */
	struct delayed_work *dwork = to_delayed_work(work);
	/* [한국어] 다시 그 delayed_work 를 품고 있는 SoC 구조체로 되돌린다.
	 * 여기서는 drvdata 가 아니라 container_of 를 쓴다 — 워크 구조체가
	 * SoC 구조체 안에 값으로 박혀 있어 오프셋 계산이 가능하기 때문이다. */
	struct ls_g4_pcie *pcie = container_of(dwork, struct ls_g4_pcie, dwork);
	/* [한국어] 공통 구조체 주소 — config 헤더 접근에 쓴다. */
	struct mobiveil_pcie *mv_pci = &pcie->pci;
	/* [한국어] 브리지 제어 레지스터 값(16비트)을 담을 변수. */
	u16 ctrl;

	/* [한국어] RC 자신의 config 헤더에서 브리지 제어 레지스터를 읽는다.
	 * 루트 버스 접근이므로 csr_axi_slave 영역이 곧 config 공간 역할을 한다. */
	ctrl = mobiveil_csr_readw(mv_pci, PCI_BRIDGE_CONTROL);
	/* [한국어] secondary bus reset 비트를 내린다 — 하위 버스에 걸려 있던 리셋을 푼다.
	 * 다른 비트(VGA 포워딩, 오류 보고 등)는 보존해야 하므로 통째로 쓰지 않는다. */
	ctrl &= ~PCI_BRIDGE_CTL_BUS_RESET;
	/* [한국어] 고친 값을 되쓴다. 이 시점부터 하위 링크가 다시 훈련을 시작할 수 있다. */
	mobiveil_csr_writew(mv_pci, ctrl, PCI_BRIDGE_CONTROL);

	/* [한국어] 재초기화를 시도한다. 위 함수 주석에 적어 둔 대로, 여기서 참이 되는 것은
	 * 반환값 0 — 즉 재초기화가 성공한 경우다. */
	if (!ls_g4_pcie_reinit_hw(pcie))
		/* [한국어] 그 경우 인터럽트를 다시 켜지 않고 그대로 끝낸다. */
		return;

	/* [한국어] 재초기화가 -EIO 로 실패한 경우에만 도달한다. 인터럽트를 다시 켜므로
	 * 다음 리셋 이벤트는 다시 감지된다. */
	ls_g4_pcie_enable_interrupt(pcie);
}

/* [한국어] 이 SoC 의 루트 포트 계층 콜백 표. 공통 host.c 가 기본 인터럽트 초기화
 * 대신 이 표를 보게 된다.
 * mobiveil 트리에서 mobiveil_rp_ops 를 실제로 채우는 것은 이 정의뿐이다. */
static const struct mobiveil_rp_ops ls_g4_pcie_rp_ops = {
	/* [한국어] 인터럽트 초기화를 위 ls_g4_pcie_interrupt_init 으로 대체한다.
	 * 이 필드가 채워졌기 때문에 host.c 는 자체 INTx/MSI 도메인을 만들지 않는다. */
	.interrupt_init = ls_g4_pcie_interrupt_init,
};

/* [한국어] 이 SoC 의 PAB 계층 콜백 표. 공통 pcie-mobiveil.c 가 링크 판정을
 * 이 표를 통해 위임한다.
 * mobiveil 트리에서 mobiveil_pab_ops 를 실제로 채우는 것도 이 정의뿐이다. */
static const struct mobiveil_pab_ops ls_g4_pcie_pab_ops = {
	/* [한국어] 링크 판정을 위 ls_g4_pcie_link_up 으로 대체한다 —
	 * 공통 LTSSM_STATUS 대신 PF 블록의 PCIE_PF_DBG 를 보게 된다. */
	.link_up = ls_g4_pcie_link_up,
};

/* [한국어] ls_g4_pcie_probe - fsl,lx2160a-pcie 컨트롤러의 probe.
 * 
 * @pdev: 이 컨트롤러에 대응하는 platform 장치.
 * @return: 0 성공, 음수 errno 실패.
 * 
 * 왜 필요한가: 공통 계층이 동작하려면 struct mobiveil_pcie 의 pdev,
 * bridge, 그리고 두 ops 표가 미리 채워져 있어야 한다. 이 함수가 그
 * 준비를 하고 나머지를 mobiveil_pcie_host_probe() 에 넘긴다.
 * 
 * 동작 단계:
 *   1) DT 에 msi-parent 가 있는지 확인한다. 이 SoC 는 MSI 를 외부
 *      컨트롤러에 맡기므로, 없으면 아예 진행하지 않는다.
 *   2) 호스트 브리지를 할당하고 그 private 영역을 struct ls_g4_pcie 로 쓴다.
 *   3) pdev, 두 ops, bridge 를 채운다.
 *   4) drvdata 를 설정한다 — to_ls_g4_pcie() 가 drvdata 를 경유하므로
 *      공통 계층을 부르기 전에 반드시 해 두어야 한다.
 *   5) 리셋 복구 워크를 초기화한다 — 인터럽트를 켜기 전에 해야
 *      첫 인터럽트가 초기화되지 않은 워크를 예약하는 일이 없다.
 *   6) 공통 호스트 probe 를 부르고, 성공하면 인터럽트를 켠다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. __init 이라 부팅 초기화 이후
 * 메모리가 회수된다 — 그래서 이 드라이버는 나중에 바인딩될 수 없고,
 * driver 구조체도 .probe 대신 builtin_platform_driver_probe 를 쓴다.
 * 호출자: platform_driver_probe() (파일 끝의 매크로가 만든 initcall 경유).
 * 피호출자: of_parse_phandle, devm_pci_alloc_host_bridge,
 *   platform_set_drvdata, INIT_DELAYED_WORK, mobiveil_pcie_host_probe,
 *   ls_g4_pcie_enable_interrupt.
 * 에러 경로: 각 단계에서 바로 반환한다. 브리지와 IRQ 는 devm_ 로 잡았으므로
 *   별도 해제 코드가 없다.
 * 
 * 호출 체인:
 *   device_initcall → platform_driver_probe() → [이 함수]
 *     → mobiveil_pcie_host_probe() → mobiveil_host_init() / pci_host_probe() */
static int __init ls_g4_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 로그와 devm 할당의 주인이 될 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 이 컨트롤러의 PCI 호스트 브리지 객체. */
	struct pci_host_bridge *bridge;
	/* [한국어] 공통 계층에 넘길 포인터. */
	struct mobiveil_pcie *mv_pci;
	/* [한국어] 이 SoC 구조체 — 브리지 private 영역을 가리키게 된다. */
	struct ls_g4_pcie *pcie;
	/* [한국어] DT 노드. msi-parent 확인에 쓴다. */
	struct device_node *np = dev->of_node;
	/* [한국어] 공통 probe 결과를 담을 변수. */
	int ret;

	/* [한국어] 이 SoC 는 MSI 를 자체 처리하지 않고 외부 컨트롤러에 맡기므로,
	 * DT 에 msi-parent 가 없으면 MSI 를 쓸 방법이 없다.
	 * of_parse_phandle 은 참조 카운트를 올린 device_node 를 돌려주는데,
	 * 여기서는 존재 여부만 보고 of_node_put 을 부르지 않는다 —
	 * 이 트리에서 그것을 되돌리는 코드는 찾지 못했다. */
	if (!of_parse_phandle(np, "msi-parent", 0)) {
		/* [한국어] 필수 DT 속성이 빠졌음을 알린다. */
		dev_err(dev, "Failed to find msi-parent\n");
		/* [한국어] 설정 오류이므로 재시도 여지가 없는 -EINVAL 로 접는다. */
		return -EINVAL;
	}

	/* [한국어] 호스트 브리지를 할당하면서 뒤에 sizeof(*pcie) 만큼의 private 공간을 함께 잡는다.
	 * devm_ 판이라 별도 해제가 필요 없고, 할당은 0 으로 채워지므로
	 * 채우지 않은 필드는 NULL 로 남는다. */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	/* [한국어] 메모리 부족으로 할당에 실패한 경우. */
	if (!bridge)
		/* [한국어] 메모리 부족을 알린다. */
		return -ENOMEM;

	/* [한국어] 브리지 뒤에 딸린 private 영역의 주소를 받아 이 SoC 구조체로 쓴다.
	 * 즉 struct ls_g4_pcie 는 따로 kmalloc 되지 않고 브리지와 수명을 같이한다. */
	pcie = pci_host_bridge_priv(bridge);
	/* [한국어] 공통 구조체는 그 첫 필드다 — 이후 공통 계층에는 이 주소를 넘긴다. */
	mv_pci = &pcie->pci;

	/* [한국어] 공통 계층이 리소스 획득과 로그에 쓸 platform 장치를 알려 준다. */
	mv_pci->pdev = pdev;
	/* [한국어] PAB 계층 콜백 표를 건다. 이 대입이 있어야 pcie-mobiveil.c 의
	 * mobiveil_pcie_link_up() 이 PF 블록 기반 판정을 쓰게 된다.
	 * 참고: mobiveil 트리에서 mobiveil_pcie 의 ops 를 대입하는 곳은 여기뿐이고,
	 * 읽는 쪽은 ops 자체의 NULL 검사 없이 ops->link_up 을 본다. */
	mv_pci->ops = &ls_g4_pcie_pab_ops;
	/* [한국어] 루트 포트 계층 콜백 표를 건다. 이 대입이 있어야 host.c 가
	 * 자체 INTx/MSI 초기화 대신 ls_g4_pcie_interrupt_init() 을 부른다.
	 * 참고: rp.ops 를 대입하는 곳도 mobiveil 트리에서 여기뿐이며,
	 * 읽는 쪽은 rp->ops 자체의 NULL 검사 없이 rp->ops->interrupt_init 을 본다. */
	mv_pci->rp.ops = &ls_g4_pcie_rp_ops;
	/* [한국어] 공통 계층이 브리지에 닿을 수 있도록 반대 방향 연결을 만든다.
	 * mobiveil_host_init() 이 bridge->windows 로 DT 의 ranges 를 훑는다. */
	mv_pci->rp.bridge = bridge;

	/* [한국어] to_ls_g4_pcie() 매크로가 drvdata 를 경유하므로, 공통 계층을 부르기 전에
	 * 반드시 여기서 설정해야 한다. 공통 probe 안에서 링크 판정 콜백이
	 * 불리는데 그때 이 값이 없으면 NULL 을 역참조하게 된다. */
	platform_set_drvdata(pdev, pcie);

	/* [한국어] 리셋 복구 워크를 초기화하고 핸들러를 ls_g4_pcie_reset 으로 지정한다.
	 * 인터럽트를 켜기 전에 해 두어야 첫 리셋 인터럽트가 초기화되지 않은
	 * 워크를 예약하는 일이 없다. */
	INIT_DELAYED_WORK(&pcie->dwork, ls_g4_pcie_reset);

	/* [한국어] 여기서부터는 공통 계층이 맡는다 — DT 파싱, 창 설정, 인터럽트 초기화
	 * (위에서 건 콜백 경유), 링크 대기, 그리고 PCI 버스 열거까지. */
	ret = mobiveil_pcie_host_probe(mv_pci);
	/* [한국어] 공통 probe 가 실패한 경우. */
	if (ret) {
		/* [한국어] 실패를 남긴다. 구체적인 원인은 공통 계층이 이미 별도로 찍었다. */
		dev_err(dev, "Fail to probe\n");
		/* [한국어] 실패를 그대로 올린다. */
		return  ret;
	}

	/* [한국어] 버스 열거까지 끝난 뒤에야 인터럽트를 켠다.
	 * 순서가 중요하다 — 열거 도중 리셋 인터럽트가 올라와 재초기화 워크가
	 * 돌면 방금 설정한 창이 다시 뒤집힐 수 있다. */
	ls_g4_pcie_enable_interrupt(pcie);

	/* [한국어] probe 성공. */
	return 0;
}

/* [한국어] 이 드라이버가 담당하는 DT compatible 목록.
 * MODULE_DEVICE_TABLE 이 없는 것은 이 드라이버가 모듈이 아니라
 * 빌트인 전용(Kconfig 가 bool)이기 때문이다. */
static const struct of_device_id ls_g4_pcie_of_match[] = {
	/* [한국어] NXP LX2160A 의 Gen4 PCIe 컨트롤러.
	 * mobiveil 트리에서 ops 를 채우는 유일한 compatible 이기도 하다. */
	{ .compatible = "fsl,lx2160a-pcie", },
	/* [한국어] 목록의 끝을 알리는 빈 항목. 이것이 없으면 매칭 루프가 배열을 넘어간다. */
	{ },
};

/* [한국어] platform 버스에 등록할 드라이버 서술자.
 * .probe 필드가 없는 점이 특이한데, 파일 끝의
 * builtin_platform_driver_probe 매크로가 probe 함수를 따로 넘겨주기 때문이다. */
static struct platform_driver ls_g4_pcie_driver = {
	/* [한국어] 드라이버 공통 속성 묶음. */
	.driver = {
		/* [한국어] sysfs 에 보이는 드라이버 이름. */
		.name = "layerscape-pcie-gen4",
		/* [한국어] 위 compatible 목록을 연결해 DT 매칭이 이루어지게 한다. */
		.of_match_table = ls_g4_pcie_of_match,
		/* [한국어] sysfs 의 bind/unbind 속성을 만들지 않는다.
		 * probe 가 __init 이라 부팅 후에는 그 코드가 사라지므로, 나중에 수동으로
		 * 바인딩을 시도하면 안 되기 때문이다. */
		.suppress_bind_attrs = true,
	},
};

/* [한국어] 드라이버 등록 initcall 을 만든다.
 * 일반적인 builtin_platform_driver 와 달리 probe 함수를 인자로 따로 받는데,
 * 그 형태라야 probe 를 __init 에 둘 수 있다(platform_driver_probe 는
 * 등록 시점에 한 번만 매칭을 시도하고 이후 재시도하지 않는다).
 * 부작용으로 이 드라이버는 -EPROBE_DEFER 재시도의 이득을 보지 못한다. */
builtin_platform_driver_probe(ls_g4_pcie_driver, ls_g4_pcie_probe);
