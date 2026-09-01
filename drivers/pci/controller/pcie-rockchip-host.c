// SPDX-License-Identifier: GPL-2.0+
/*
 * Rockchip AXI PCIe host controller driver
 *
 * Copyright (c) 2016 Rockchip, Inc.
 *
 * Author: Shawn Lin <shawn.lin@rock-chips.com>
 *         Wenrui Li <wenrui.li@rock-chips.com>
 *
 * Bits taken from Synopsys DesignWare Host controller driver and
 * ARM PCI Host generic driver.
 */

/*
 * [한국어 설명] Rockchip AXI PCIe 컨트롤러를 루트 컴플렉스로 모는 드라이버 (pcie-rockchip-host.c)
 *
 * === 파일의 역할 ===
 * RK3399 계열 SoC 에 내장된 Rockchip AXI PCIe 컨트롤러를 **루트 컴플렉스(RC)**
 * 로 초기화하고, 커널 PCI 코어에 struct pci_ops 를 제공해 그 아래 버스를
 * 열거할 수 있게 하는 호스트 컨트롤러 드라이버다.
 *
 * 같은 IP 를 정반대 역할인 **엔드포인트(EP)** 로 모는 짝이
 * pcie-rockchip-ep.c 다. 두 파일은 레지스터 지도와 공통 초기화 코드를
 * 공유하며, 갈라지는 지점은 단 하나 — struct rockchip_pcie 의 is_rc 필드다.
 * 이 파일이 그것을 true 로 세우고(rockchip_pcie_probe 안), ep 쪽은 false 로
 * 세운다. 공용 코드(pcie-rockchip.c)가 그 값을 보고 세 곳에서 분기한다
 * (:35 axi-base vs mem-base, :90 PERST GPIO 방향, :160 링크 훈련 활성화와
 * MODE_RC vs MODE_EP).
 *
 * 이 파일이 맡는 일은 넷이다.
 *   1) config 접근 — 자기 자신(RC)의 config 는 APB 창에 매핑된
 *      PCIE_RC_CONFIG_NORMAL_BASE 로, 하위 장치의 config 는 AXI 창의
 *      ECAM 오프셋으로 접근한다. 접근 전에 outbound 영역 0 의 TLP 종류를
 *      Type 0/Type 1 로 바꿔 주는 것이 이 하드웨어의 특징이다.
 *   2) 링크 훈련 — Gen1 으로 훈련을 마친 뒤, 요청이 있으면 Gen2 로 재훈련한다.
 *      PERST GPIO 를 내렸다 올리는 전원 시퀀스도 여기서 지킨다.
 *   3) 주소 변환(ATU) — 메모리/IO 창과 메시지 영역을 outbound 영역에,
 *      DMA 대상 영역을 inbound 영역에 프로그램한다.
 *   4) 인터럽트 — 하위 장치의 INTx 넷을 IRQ 도메인으로 옮기고, 컨트롤러
 *      자신의 오류·상태 인터럽트를 두 핸들러로 나눠 받는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 빌드: Makefile:34 이 CONFIG_PCIE_ROCKCHIP_HOST 로 이 파일을,
 *       Makefile:32 가 CONFIG_PCIE_ROCKCHIP 로 공용 pcie-rockchip.c 를 넣는다.
 *       ep 판은 Makefile:33 이다. 셋 다 pcie-rockchip.h 를 공유한다.
 *
 * 등록: 장치 트리에 rockchip,rk3399-pcie 노드
 *         -> 플랫폼 버스가 rockchip_pcie_probe() 를 부른다
 *            -> devm_pci_alloc_host_bridge() 로 브리지와 struct rockchip_pcie 를
 *               한 번에 잡고 is_rc = true 로 모드를 정한다
 *            -> rockchip_pcie_parse_host_dt() — 공용 parse_dt 뒤에 레귤레이터 넷
 *            -> rockchip_pcie_enable_clocks() [공용]
 *            -> rockchip_pcie_set_vpcie() — 12V/3V3/1V8/0V9 순서로 켠다
 *            -> rockchip_pcie_host_init_port() — 공용 init_port 뒤에 RC 전용 설정과
 *               Gen1/Gen2 링크 훈련
 *            -> rockchip_pcie_init_irq_domain() — INTx 도메인
 *            -> rockchip_pcie_cfg_atu() — outbound/inbound 주소 변환
 *            -> rockchip_pcie_setup_irq() — sys/legacy/client 세 IRQ
 *            -> pci_host_probe() 로 PCI 코어에 넘긴다
 *
 * config 접근: PCI 코어
 *         -> pci_ops.read/write -> rockchip_pcie_rd_conf() / _wr_conf()
 *            -> 루트 버스면 rd_own_conf() / wr_own_conf()  (APB 창 직접)
 *            -> 그 아래면 rd_other_conf() / wr_other_conf() (AXI ECAM 창)
 *
 * 인터럽트: 하위 장치의 INTx
 *         -> rockchip_pcie_intx_handler()(체인 핸들러)
 *            -> generic_handle_domain_irq() -> 장치 드라이버
 *       컨트롤러 자신의 오류/상태
 *         -> rockchip_pcie_subsys_irq_handler() 와 _client_irq_handler()
 *
 * 실행 컨텍스트: probe 와 PM 콜백은 프로세스 컨텍스트이고 msleep 으로 잠든다.
 * config 접근은 PCI 코어가 pci_lock 스핀락을 쥔 채 부르므로 잠들 수 없다 —
 * 그래서 이 파일의 config 경로에는 대기가 전혀 없다.
 * 세 인터럽트 핸들러는 인터럽트 컨텍스트다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어 전체가 rockchip_pcie_ops(pci_ops) 를 통해서만 이 하드웨어에
 *   닿는다. 플랫폼 버스와 장치 트리가 진입점이다.
 * 옆쪽(짝이 되는 파일):
 *   pcie-rockchip.h  — 레지스터 지도와 struct rockchip_pcie, 그리고
 *     rockchip_pcie_read/write 인라인. 이 파일이 쓰는 모든 레지스터 이름이
 *     거기서 온다.
 *   pcie-rockchip.c  — 공용 초기화 일곱 함수. 이 파일이 쓰는 것은 여섯이다:
 *     parse_dt, init_port, deinit_phys, enable_clocks, disable_clocks,
 *     cfg_configuration_accesses. 나머지 하나인 get_phys 는 **직접 부르지
 *     않는다** — parse_dt 안(pcie-rockchip.c:55)에서 대신 불린다.
 *     ep 판은 그것을 직접 한 번 더 부른다(ep.c:732).
 *     반대로 cfg_configuration_accesses 는 이 파일만 쓴다(5회, ep 는 0회) —
 *     config 접근이 RC 쪽에만 있는 일이기 때문이다.
 *   pcie-rockchip-ep.c — 같은 IP 의 EP 판. 아래 "호스트와 엔드포인트의
 *     대칭" 절에서 견준다.
 * 아래쪽: readl/writel(APB 창과 AXI 창), regulator(전원 넷), gpiod(PERST),
 *   phy(레인별 PHY), irqdomain 과 chained_irq, "../pci.h"(PCIE_T_PVPERL_MS 와
 *   PCIE_RESET_CONFIG_WAIT_MS 가 거기 있다 — drivers/pci/pci.h:107 과 :137).
 * 공유 상태: struct rockchip_pcie 하나뿐이고, pci_host_bridge 의 private
 *   영역에 얹혀 있다(devm_pci_alloc_host_bridge + pci_host_bridge_priv).
 *   ep 판이 그 구조체를 자기 struct rockchip_pcie_ep 의 첫 멤버로 감싸는
 *   것과 대조된다 — 이쪽은 감싸지 않고 그대로 쓴다.
 *
 * === 주요 함수/구조체 요약 ===
 * rockchip_pcie_probe() / rockchip_pcie_remove()
 *                          : 플랫폼 드라이버 진입점과 정리.
 * rockchip_pcie_parse_host_dt() : 공용 parse_dt 에 레귤레이터 넷을 더한다.
 * rockchip_pcie_set_vpcie(): 12V/3V3/1V8/0V9 를 순서대로 켜고, 실패하면
 *                            역순으로 되돌린다.
 * rockchip_pcie_host_init_port() : RC 전용 하드웨어 초기화와 링크 훈련.
 *                            이 파일에서 가장 긴 함수다.
 * rockchip_pcie_set_power_limit() : 3V3 레귤레이터의 전류 한계를 읽어
 *                            Slot Power Limit 로 광고한다.
 * rockchip_pcie_rd_conf() / _wr_conf() : pci_ops 콜백. 대상 버스에 따라
 *                            own/other 로 갈린다.
 * rockchip_pcie_rd_own_conf() / _wr_own_conf()  : RC 자신의 config(APB 창).
 * rockchip_pcie_rd_other_conf() / _wr_other_conf(): 하위 장치의 config(AXI ECAM).
 * rockchip_pcie_valid_device() : 없는 장치를 미리 걸러 낸다.
 * rockchip_pcie_cfg_atu()  : 메모리/IO/메시지 창을 outbound 에, DMA 영역을
 *                            inbound 에 프로그램한다.
 * rockchip_pcie_prog_ob_atu() / _ib_atu() : 영역 하나를 프로그램하는 하위 함수.
 * rockchip_pcie_subsys_irq_handler() : 코어 내부 오류를 종류별로 로그한다.
 * rockchip_pcie_client_irq_handler() : 클라이언트 계층 상태 변화를 로그한다.
 * rockchip_pcie_intx_handler() : 하위 INTx 를 IRQ 도메인으로 넘긴다.
 * rockchip_pcie_setup_irq() : sys/legacy/client 세 IRQ 를 건다.
 * rockchip_pcie_init_irq_domain() : INTx 도메인을 만든다.
 * rockchip_pcie_enable_interrupts() : 인터럽트 마스크를 열고 대역폭 알림을 켠다.
 * rockchip_pcie_lane_map() : 실제로 붙은 레인을 알아낸다(역순 매핑 보정 포함).
 * rockchip_pcie_wait_l2() : 절전 전에 링크가 L2 로 내려가기를 기다린다.
 * rockchip_pcie_suspend_noirq() / _resume_noirq() : 시스템 절전 진입과 복귀.
 * rockchip_pcie_update_txcredit_mui() / _enable_bw_int() / _clr_bw_int()
 *                          : 크레딧 갱신 주기와 대역폭 변화 알림.
 *
 * 이 파일에는 자체 구조체가 없다. 상태는 전부 pcie-rockchip.h 의
 * struct rockchip_pcie 에 들어 있고, 그 필드 주석은 그 헤더에 있다.
 *
 * === 호스트와 엔드포인트의 대칭 ===
 * 같은 하드웨어를 반대 방향으로 쓰므로, 두 파일의 함수가 짝을 이룬다.
 * 이 절은 pcie-rockchip-ep.c 의 같은 이름 절과 짝이다.
 *
 *   모드 선택   host: is_rc = true  -> MODE_RC, 링크 훈련 즉시 활성화
 *               ep  : is_rc = false -> MODE_EP, CONF_DISABLE 로 시작
 *               (분기는 공용 pcie-rockchip.c:160)
 *
 *   PERST 신호  host: 출력. gpiod_set_value_cansleep 으로 상대를 리셋했다 푼다
 *                     (host_init_port 안, GPIOD_OUT_LOW 로 획득).
 *               ep  : 입력. 호스트가 거는 PERST 를 IRQ 로 받아 링크를
 *                     내렸다 올린다(GPIOD_IN 으로 획득, perst_irq).
 *               (획득 방향 분기는 공용 pcie-rockchip.c:90)
 *
 *   링크 훈련   host: host_init_port() 안에서 readl_poll_timeout 으로 동기 대기.
 *                     probe 가 끝나기 전에 링크가 서야 열거를 시작할 수 있다.
 *               ep  : delayed_work(link_training)로 비동기. 호스트가 언제
 *                     올지 모르므로 계속 재시도한다.
 *
 *   주소 변환   host: cfg_atu() 가 DT 의 메모리/IO 창을 outbound 에 미리
 *                     전부 깔아 둔다. inbound 는 영역 2 하나(DMA 대상).
 *               ep  : map_addr/unmap_addr 콜백이 요청이 올 때마다 outbound
 *                     영역을 하나씩 잡고 푼다. inbound 는 BAR 마다
 *                     set_bar/clear_bar 가 건다.
 *
 *   config      host: pci_ops 를 제공해 남의 config 를 읽고 쓴다.
 *               ep  : pci_epc_ops 의 write_header/set_bar 로 자기 config 를
 *                     남에게 보여 준다. 방향이 정확히 반대다.
 *
 *   인터럽트    host: 하위 장치의 INTx 를 받아 IRQ 도메인으로 올린다.
 *               ep  : raise_irq 콜백으로 INTx/MSI 를 호스트에게 보낸다.
 *
 *   구조체      host: struct rockchip_pcie 를 그대로 쓴다.
 *               ep  : struct rockchip_pcie_ep 의 첫 멤버로 감싸 EP 전용
 *                     필드(ob_region_map, irq_cpu_addr, link_training 등)를 더한다.
 *
 * === 값의 근거에 대하여 ===
 * 이 파일이 쓰는 레지스터 이름과 비트는 전부 pcie-rockchip.h 에 정의되어
 * 있고, 그 헤더에 이미 상세한 한국어 주석이 달려 있다(다른 작업자가 맡은
 * 파일이라 이 작업에서는 읽기만 했다). 따라서 아래 주석은 값을 다시
 * 설명하지 않고, **이 파일이 그 상수를 어떤 순서로 어떤 목적에 쓰는지**에
 * 집중한다. Rockchip 하드웨어 매뉴얼은 이 트리에 없으므로, 코드만으로
 * 뜻을 확정할 수 없는 것은 그렇게 밝혀 둔다.
 * PCIE_ECAM_OFFSET(include/linux/pci-ecam.h)과
 * PCI_VENDOR_ID_ROCKCHIP / PCI_CLASS_BRIDGE_PCI_NORMAL(include/linux/pci_ids.h),
 * 그리고 PCI_EXP_ 계열 표준 상수는 그 헤더들이 이 스파스 체크아웃에 없어
 * 값을 확인하지 못했다 — 코드의 사용 방식으로만 설명한다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일과 아무 접점이 없다(drivers/nvme 전수 grep 0건).
 * 이 파일은 특정 SoC 의 호스트 컨트롤러 드라이버라, 장치 종류와 무관하게
 * "PCI 버스를 제공" 하는 쪽이기 때문이다.
 *
 * 다만 RK3399 보드에 M.2 NVMe SSD 를 붙이는 구성이 실제로 흔하고, 그때
 * 그 SSD 를 열거하는 것이 이 드라이버다. 이 파일의 rockchip_pcie_cfg_atu()
 * 가 깔아 둔 inbound 영역이 곧 NVMe 컨트롤러가 DMA 로 호스트 메모리에
 * 닿는 통로이고, rockchip_pcie_rd_other_conf() 가 NVMe 의 BAR 와
 * capability 를 읽는 경로다. 다만 그 연결은 배치의 문제이지 코드의 의존
 * 관계가 아니므로, 이 파일 안에서 근거를 댈 수 있는 부분은 없다.
 */

/* [한국어] FIELD_PREP/FIELD_GET/FIELD_MAX — 비트 필드에 값을 넣고 빼는 매크로.
 * 전력 한계 계산과 PCIe capability 필드 조작에 쓰인다. */
#include <linux/bitfield.h>
/* [한국어] bitrev8() — 8비트를 뒤집는다. 레인 매핑이 역순으로 보고될 때 되돌리는 데 쓴다. */
#include <linux/bitrev.h>
/* [한국어] gpiod_set_value_cansleep() — PERST# 를 토글한다. */
#include <linux/gpio/consumer.h>
/* [한국어] irqreturn_t 와 인터럽트 관련 기본 정의. */
#include <linux/interrupt.h>
/* [한국어] readl_poll_timeout() — 조건이 참이 될 때까지 주기적으로 읽으며 기다리는 매크로.
 * 링크 훈련 완료 대기에 쓰인다. */
#include <linux/iopoll.h>
/* [한국어] irq_chip / irq_data 등 IRQ 도메인 구현에 필요한 정의. */
#include <linux/irq.h>
/* [한국어] chained_irq_enter/exit — INTx 를 GIC IRQ 하나로 모아 받는 체인 핸들러용. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] irq_domain_create_linear() 등 IRQ 도메인 API. */
#include <linux/irqdomain.h>
/* [한국어] MODULE_* 매크로와 module_platform_driver. */
#include <linux/module.h>
/* [한국어] of_property_read_bool() 등 디바이스 트리 접근. */
#include <linux/of.h>
/* [한국어] of_pci 계열 헬퍼. */
#include <linux/of_pci.h>
/* [한국어] phy_power_off()/phy_exit() — 쓰지 않는 레인의 PHY 를 끄는 데 쓴다. */
#include <linux/phy/phy.h>
/* [한국어] platform_driver 와 plat form_get_irq_byname 계열. */
#include <linux/platform_device.h>

/* [한국어] PCI 서브시스템 내부 헤더 — PCIE_T_PVPERL_MS 와 PCIE_RESET_CONFIG_WAIT_MS
 * 같은 PCIe 규격 지연 상수가 여기 있다. */
#include "../pci.h"
/* [한국어] 이 드라이버의 공용 헤더 — 레지스터 지도, struct rockchip_pcie,
 * APB 접근자 두 개, 그리고 공용 코드의 함수 선언. */
#include "pcie-rockchip.h"

/* [한국어]
 * rockchip_pcie_enable_bw_int - 링크 대역폭 변화 인터럽트를 켠다
 *
 * @rockchip: 컨트롤러 객체.
 *
 * RC 자신의 config 공간에 있는 Link Control 레지스터에서 두 비트를 세운다 —
 * LBMIE(Link Bandwidth Management Interrupt Enable)와 LABIE(Link Autonomous
 * Bandwidth Interrupt Enable). 전자는 소프트웨어가 요청한 링크 속도·폭 변경이
 * 끝났을 때, 후자는 하드웨어가 스스로 링크를 재협상했을 때 인터럽트를 올린다.
 *
 * 이 인터럽트가 필요한 이유는 링크 속도가 바뀌면 Tx 크레딧 갱신 주기를 다시
 * 계산해야 하기 때문이다. 실제로 subsys 핸들러가 PHY 링크 변화를 감지하면
 * rockchip_pcie_update_txcredit_mui() 를 다시 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_enable_interrupts() → [이 함수]
 *     → rockchip_pcie_read/write(PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL)
 */
static void rockchip_pcie_enable_bw_int(struct rockchip_pcie *rockchip)
{
	/* [한국어] 읽기-수정-쓰기에 쓸 임시 변수. */
	u32 status;

	/* [한국어] RC 자신의 config 공간에 있는 Link Control 레지스터를 읽는다.
	 * PCIE_RC_CONFIG_CR 은 그 config 블록의 시작 오프셋이고, 거기에
	 * PCI_EXP_LNKCTL 을 더해 표준 위치를 가리킨다. */
	status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);
	/* [한국어] 대역폭 관리 인터럽트 두 개를 켠다 — LBMIE(Link Bandwidth Management)와
	 * LABIE(Link Autonomous Bandwidth). 링크 속도나 폭이 바뀌면 인터럽트가 온다. */
	status |= (PCI_EXP_LNKCTL_LBMIE | PCI_EXP_LNKCTL_LABIE);
	/* [한국어] 되쓴다. */
	rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);
}

/* [한국어]
 * rockchip_pcie_clr_bw_int - 링크 대역폭 변화 인터럽트 상태를 지운다
 *
 * @rockchip: 컨트롤러 객체.
 *
 * enable 과 같은 레지스터를 다루지만 방식이 다르다. LBMS/LABS 는 RW1C 상태
 * 비트인데, 이 컨트롤러의 레지스터는 상위 16비트를 "어느 비트를 실제로 쓸지"의
 * 마스크로 해석하는 write-enable 방식이다. 그래서 상태 비트를 16비트 왼쪽으로
 * 밀어 마스크 자리에 놓는 것만으로 해당 비트를 지우는 효과가 난다.
 *
 * 읽은 값에 OR 로 얹기 때문에 하위 16비트(실제 값)는 원래 읽은 그대로 남고,
 * 상위 마스크만 새로 세워지는 구조다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_subsys_irq_handler()(PHY 링크 변화 갈래) → [이 함수]
 */
static void rockchip_pcie_clr_bw_int(struct rockchip_pcie *rockchip)
{
	/* [한국어] 임시 변수. */
	u32 status;

	/* [한국어] 같은 레지스터를 읽는다. */
	status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);
	/* [한국어] 상태 비트를 상위 16비트 자리에 놓아 쓴다. 이 컨트롤러의 여러 레지스터가
	 * 상위 16비트를 "어느 비트를 실제로 쓸지"의 마스크로 해석하는
	 * write-enable 방식이라, 하위에 값을 쓰는 대신 상위에 마스크를 세우는 것만으로
	 * 해당 비트를 지우는 효과를 낸다 — RW1C 상태 비트를 지우는 관용이다. */
	status |= (PCI_EXP_LNKSTA_LBMS | PCI_EXP_LNKSTA_LABS) << 16;
	/* [한국어] 되쓴다. */
	rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);
}

/* [한국어]
 * rockchip_pcie_update_txcredit_mui - Tx 크레딧 최대 갱신 주기를 설정한다
 *
 * @rockchip: 컨트롤러 객체.
 *
 * PCIe 의 흐름 제어는 크레딧 방식이다. 수신 측이 "버퍼가 이만큼 비었다"를
 * 주기적으로 알려 주고, 송신 측은 그 크레딧이 있을 때만 TLP 를 보낸다.
 * MUI(Maximum Update Interval)는 그 갱신을 얼마나 자주 보낼지의 상한이며,
 * 너무 길면 상대가 크레딧 고갈로 멈추고 너무 짧으면 링크에 관리 트래픽이 늘어난다.
 *
 * 24000ns 라는 값은 상류가 고른 절충값이다. 링크 속도가 바뀌면 같은 시간이
 * 다른 심볼 수에 해당하므로, PHY 링크 변화 인터럽트를 받을 때마다 이 함수를
 * 다시 부른다 — 그것이 subsys 핸들러가 이 함수를 호출하는 이유다.
 * resume 경로도 "L1 재진입을 위해" 이 설정을 다시 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_subsys_irq_handler() / rockchip_pcie_resume_noirq()
 *     → [이 함수] → rockchip_pcie_read/write(PCIE_CORE_TXCREDIT_CFG1)
 */
static void rockchip_pcie_update_txcredit_mui(struct rockchip_pcie *rockchip)
{
	/* [한국어] 임시 변수. */
	u32 val;

	/* Update Tx credit maximum update interval */
	/* [한국어] Tx 크레딧 갱신 주기 레지스터를 읽는다(옆의 상류 주석). */
	val = rockchip_pcie_read(rockchip, PCIE_CORE_TXCREDIT_CFG1);
	/* [한국어] MUI(Maximum Update Interval) 필드를 지운다. */
	val &= ~PCIE_CORE_TXCREDIT_CFG1_MUI_MASK;
	/* [한국어] 24000ns 를 인코딩해 넣는다. 옆의 상류 주석이 단위가 나노초임을 밝힌다.
	 * 이 주기가 너무 길면 상대가 크레딧 고갈로 멈추고, 너무 짧으면 링크에
	 * 관리 트래픽이 늘어난다. */
	val |= PCIE_CORE_TXCREDIT_CFG1_MUI_ENCODE(24000);	/* ns */
	/* [한국어] 되쓴다. */
	rockchip_pcie_write(rockchip, val, PCIE_CORE_TXCREDIT_CFG1);
}

/* [한국어]
 * rockchip_pcie_valid_device - 그 (버스, 장치) 조합에 config 접근을 해도 되는지 판정한다
 *
 * @rockchip: 컨트롤러 객체. 실제로는 쓰지 않지만 시그니처 일관성을 위해 받는다.
 * @bus: 접근 대상 버스.
 * @dev: 장치 번호(PCI_SLOT(devfn)).
 * @return: 1 = 접근 가능, 0 = 불가.
 *
 * 왜 필요한가: PCIe 는 점대점 링크라 루트 버스와 그 바로 아래 버스에는 장치가
 * 하나(번호 0)뿐이다. 그런데 PCI 코어는 열거할 때 버스마다 32개 장치 번호를
 * 모두 훑으므로, 걸러 내지 않으면 존재할 수 없는 devfn 에 config 요청이 나간다.
 * 그런 요청을 받으면 응답이 오지 않아 컨트롤러가 멈추거나 오류를 올리는
 * 하드웨어가 있다. 위 영어 주석이 그 제약을 명시한다.
 *
 * 동작: 루트 버스이거나 그 부모가 루트 버스면(= RC 바로 아래 버스면)
 * 장치 번호가 0 일 때만 참을 돌려준다. 더 깊은 버스는 스위치 아래일 수 있어
 * 여러 장치가 정상이므로 제한하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_rd_conf() / rockchip_pcie_wr_conf() → [이 함수]
 */
static int rockchip_pcie_valid_device(struct rockchip_pcie *rockchip,
				      struct pci_bus *bus, int dev)
{
	/*
	 * Access only one slot on each root port.
	 * Do not read more than one device on the bus directly attached
	 * to RC's downstream side.
	 */
	/* [한국어] 위 영어 주석이 설명하는 제약 — 루트 포트당 슬롯 하나만 접근하고,
	 * RC 바로 아래 버스에서는 장치 하나만 읽는다. PCIe 는 점대점 링크라
	 * 루트 버스와 그 바로 아래 버스에는 장치가 하나뿐인데, 그것을 확인하지 않으면
	 * 존재하지 않는 devfn 을 읽다가 하드웨어가 멈추는 컨트롤러가 있다. */
	if (pci_is_root_bus(bus) || pci_is_root_bus(bus->parent))
		/* [한국어] 장치 번호가 0 일 때만 유효하다고 답한다. */
		return dev == 0;

	/* [한국어] 더 깊은 버스(스위치 아래)에서는 여러 장치가 정상이므로 제한하지 않는다. */
	return 1;
}

/* [한국어]
 * rockchip_pcie_lane_map - 실제로 훈련된 레인의 비트맵을 구한다
 *
 * @rockchip: 컨트롤러 객체.
 * @return: 레인 비트맵(하위 MAX_LANE_NUM 비트). 비트 i 가 1 이면 레인 i 가 살아 있다.
 *
 * 왜 필요한가: x4 로 배선된 링크라도 실제로는 x1 이나 x2 로만 훈련될 수 있다.
 * 쓰이지 않는 레인의 SerDes 를 켜 둘 이유가 없으므로, 훈련이 끝난 뒤 이 비트맵을
 * 보고 나머지 PHY 의 전원을 내린다.
 *
 * 두 가지 경우를 다룬다.
 *   - 구형 PHY 바인딩(legacy_phy)에서는 레인 매핑 레지스터가 없으므로
 *     모든 레인이 유효하다고 가정한다.
 *   - 신형에서는 PCIE_CORE_LANE_MAP 을 읽는다. 그런데 위 영어 주석이 밝히듯
 *     역순 인덱싱을 쓰는 경우가 있어, REVERSE 비트가 서 있으면 bitrev8 로
 *     8비트를 뒤집은 뒤 4비트 오른쪽으로 민다. 뒤집으면 의미 있는 하위 4비트가
 *     상위로 올라가므로 그것을 다시 내리는 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_host_init_port() → [이 함수]
 *     → rockchip_pcie_read(PCIE_CORE_LANE_MAP) / bitrev8()
 */
static u8 rockchip_pcie_lane_map(struct rockchip_pcie *rockchip)
{
	/* [한국어] 레지스터 값. */
	u32 val;
	/* [한국어] 돌려줄 레인 비트맵. */
	u8 map;

	/* [한국어] 구형 PHY 바인딩 보드는 레인 매핑 레지스터가 없다. */
	if (rockchip->legacy_phy)
		/* [한국어] 그래서 모든 레인이 유효하다고 가정한다 — GENMASK(3,0) = 0xf. */
		return GENMASK(MAX_LANE_NUM - 1, 0);

	/* [한국어] 레인 매핑 레지스터를 읽는다. */
	val = rockchip_pcie_read(rockchip, PCIE_CORE_LANE_MAP);
	/* [한국어] 하위 비트에서 레인 비트맵을 뽑는다. */
	map = val & PCIE_CORE_LANE_MAP_MASK;

	/* The link may be using a reverse-indexed mapping. */
	/* [한국어] 위 영어 주석대로 역순 인덱싱을 쓰는 경우가 있다. */
	if (val & PCIE_CORE_LANE_MAP_REVERSE)
		/* [한국어] bitrev8 로 8비트를 뒤집은 뒤 4비트 오른쪽으로 밀어, 상위 니블에 온 결과를
		 * 하위 니블로 되돌린다. MAX_LANE_NUM 이 4 라 4비트만 의미가 있기 때문이다. */
		map = bitrev8(map) >> 4;

	/* [한국어] 정규화된 비트맵을 돌려준다. */
	return map;
}

/* [한국어]
 * rockchip_pcie_rd_own_conf - RC 자신의 config 공간을 읽는다
 *
 * @rockchip: 컨트롤러 객체.
 * @where: config space 안의 바이트 오프셋.
 * @size: 읽을 폭(1, 2, 4).
 * @val: 결과를 담을 출력 인자.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_BAD_REGISTER_NUMBER.
 *
 * 이 드라이버 config 접근의 핵심 구조가 여기 드러난다. RC 자신의 config 공간은
 * APB 창(apb_base) 안에 있고, 하위 장치의 config 는 ECAM 창(reg_base)에 있다.
 * 두 경로가 아예 다른 레지스터 창을 쓰기 때문에 이 드라이버는 map_bus 콜백
 * 하나로 통일할 수 없고, read/write 를 직접 구현한다.
 *
 * 정렬 검사를 먼저 하는 이유: 정렬되지 않은 MMIO 접근은 아키텍처에 따라 예외를
 * 일으키거나 조용히 잘못된 값을 준다. 실패 시 출력 버퍼를 0 으로 채워,
 * 호출자가 초기화되지 않은 스택 값을 보지 않게 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 * PCI 코어가 전역 pci_lock 을 쥔 채 부르므로 이 파일에는 락이 없다.
 *
 * 에러 경로: 정렬 위반과 잘못된 폭 두 가지 모두 PCIBIOS_BAD_REGISTER_NUMBER 다.
 *
 * 호출 체인:
 *   rockchip_pcie_rd_conf() → [이 함수] → readl/readw/readb(apb_base + ...)
 */
static int rockchip_pcie_rd_own_conf(struct rockchip_pcie *rockchip,
				     int where, int size, u32 *val)
{
	/* [한국어] 접근할 주소. */
	void __iomem *addr;

	/* [한국어] RC 자신의 config 공간은 APB 창 안에 있다. 하위 장치용 ECAM 창(reg_base)과
	 * 완전히 다른 경로라는 점이 이 드라이버 config 접근의 핵심 구조다. */
	addr = rockchip->apb_base + PCIE_RC_CONFIG_NORMAL_BASE + where;

	/* [한국어] 요청 폭에 맞게 주소가 정렬되어 있는지 확인한다. 정렬되지 않은 MMIO 접근은
	 * 아키텍처에 따라 예외를 일으키거나 조용히 잘못된 값을 준다. */
	if (!IS_ALIGNED((uintptr_t)addr, size)) {
		/* [한국어] 실패해도 호출자가 초기화되지 않은 값을 보지 않도록 0 으로 채운다. */
		*val = 0;
		/* [한국어] "그런 레지스터 번호는 쓸 수 없다"는 PCIBIOS 오류. */
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}

	/* [한국어] 4바이트 읽기. */
	if (size == 4) {
		/* [한국어] 32비트 그대로. */
		*val = readl(addr);
	/* [한국어] 2바이트 읽기. */
	} else if (size == 2) {
		/* [한국어] 16비트를 읽어 u32 에 담는다 — 상위는 0 으로 채워진다. */
		*val = readw(addr);
	/* [한국어] 1바이트 읽기. */
	} else if (size == 1) {
		/* [한국어] 8비트를 읽는다. */
		*val = readb(addr);
	/* [한국어] 그 밖의 폭은 PCI 규격에 없다. */
	} else {
		/* [한국어] 출력 버퍼를 0 으로. */
		*val = 0;
		/* [한국어] 오류 반환. */
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	/* [한국어] 성공. */
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * rockchip_pcie_wr_own_conf - RC 자신의 config 공간에 쓴다
 *
 * @rockchip: 컨트롤러 객체.
 * @where: config space 안의 바이트 오프셋.
 * @size: 쓸 폭(1, 2, 4).
 * @val: 쓸 값.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_BAD_REGISTER_NUMBER.
 *
 * 읽기 쪽과 달리 이 함수는 4바이트 미만 쓰기를 읽기-수정-쓰기로 흉내 낸다.
 * 하드웨어가 APB 창에서 바이트 단위 쓰기를 지원하지 않기 때문이다.
 *
 * 위 영어 주석이 그 위험을 스스로 인정한다 — 같은 워드 안에 RW1C(1 을 쓰면
 * 지워지는) 비트가 있으면, 읽은 1 을 그대로 되쓰면서 그 비트를 뜻하지 않게
 * 지운다. 하드웨어가 더 작은 쓰기를 지원하지 않아 다른 방법이 없다는 것이
 * 상류의 설명이다. 하위 장치용 wr_other_conf 는 ECAM 창을 쓰고 그쪽은 바이트
 * 쓰기를 지원해 이 문제가 없다는 점이 대비된다.
 *
 * 동작: 오프셋을 4바이트 경계로 내리고, 요청 폭·위치에 해당하는 비트만 지우는
 * 마스크를 만들어 읽은 값에 씌운 뒤, 요청 값을 그 자리로 밀어 넣어 되쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 잘못된 폭은 아래 계산에서 걸러지지 않고 그대로 진행된다 —
 * size 가 1/2/4 가 아닌 값으로 불릴 수 없다는 전제다(호출자가 PCI 코어이므로 성립한다).
 *
 * 호출 체인:
 *   rockchip_pcie_wr_conf() → [이 함수] → readl/writel(apb_base + ...)
 */
static int rockchip_pcie_wr_own_conf(struct rockchip_pcie *rockchip,
				     int where, int size, u32 val)
{
	/* [한국어] mask: 보존할 비트의 마스크. tmp: 읽기-수정-쓰기 중간값.
	 * offset: 4바이트 정렬로 내린 주소 오프셋. */
	u32 mask, tmp, offset;
	/* [한국어] 접근할 주소. */
	void __iomem *addr;

	/* [한국어] 하위 2비트를 지워 워드 경계로 내린다. 이 하드웨어가 4바이트 쓰기만 지원하기
	 * 때문에, 1~2바이트 요청도 워드 단위로 처리해야 한다. */
	offset = where & ~0x3;
	/* [한국어] RC 자신의 config 공간 안의 워드 주소. */
	addr = rockchip->apb_base + PCIE_RC_CONFIG_NORMAL_BASE + offset;

	/* [한국어] 4바이트 요청이면 읽기-수정-쓰기가 필요 없다. */
	if (size == 4) {
		/* [한국어] 그대로 쓴다. */
		writel(val, addr);
		/* [한국어] 성공. */
		return PCIBIOS_SUCCESSFUL;
	}

	/* [한국어] 요청한 폭만큼의 비트를 요청한 바이트 위치에 놓고 반전해, "그 자리만 빼고
	 * 나머지를 보존하는" 마스크를 만든다. */
	mask = ~(((1 << (size * 8)) - 1) << ((where & 0x3) * 8));

	/*
	 * N.B. This read/modify/write isn't safe in general because it can
	 * corrupt RW1C bits in adjacent registers.  But the hardware
	 * doesn't support smaller writes.
	 */
	/* [한국어] 위 영어 주석이 스스로 인정하는 위험 — 이 읽기-수정-쓰기는 일반적으로 안전하지
	 * 않다. 같은 워드 안에 RW1C(1 을 쓰면 지워지는) 비트가 있으면, 읽은 1 을
	 * 그대로 되쓰면서 그 비트를 뜻하지 않게 지운다. 하드웨어가 더 작은 쓰기를
	 * 지원하지 않아 다른 방법이 없다는 것이 상류의 설명이다. */
	tmp = readl(addr) & mask;
	/* [한국어] 요청 값을 해당 바이트 위치로 밀어 넣는다. */
	tmp |= val << ((where & 0x3) * 8);
	/* [한국어] 합친 워드를 쓴다. */
	writel(tmp, addr);

	/* [한국어] 성공. */
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * rockchip_pcie_rd_other_conf - 하위 장치의 config 공간을 읽는다
 *
 * @rockchip: 컨트롤러 객체.
 * @bus: 접근 대상 버스.
 * @devfn: 장치·함수 번호.
 * @where: config space 안의 바이트 오프셋.
 * @size: 읽을 폭.
 * @val: 결과를 담을 출력 인자.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_BAD_REGISTER_NUMBER.
 *
 * RC 자신을 읽는 rd_own_conf 와 달리 ECAM 창(reg_base)을 쓴다. 주소는
 * PCIE_ECAM_OFFSET(bus, devfn, where) 로 계산하는데, 이것이 표준 ECAM 배치
 * (버스 << 20 | devfn << 12 | 오프셋)를 그대로 따르는 매크로다.
 *
 * 이 함수의 특징은 접근 직전에 config 트랜잭션 종류를 매번 다시 설정한다는
 * 점이다. RC 바로 아래 버스면 Type 0 (버스 번호를 해석하지 않고 링크 상대에게
 * 직접), 더 깊으면 Type 1 (중간 브리지가 버스 번호를 보고 전달)이다.
 * 하드웨어에 그 구분을 알려 주는 레지스터가 하나뿐이라, 접근마다 갱신하는
 * 방식을 택했다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 정렬 위반과 잘못된 폭 모두 PCIBIOS_BAD_REGISTER_NUMBER 이고,
 * 출력 버퍼를 0 으로 채운다.
 *
 * 호출 체인:
 *   rockchip_pcie_rd_conf() → [이 함수]
 *     → rockchip_pcie_cfg_configuration_accesses() (공용 pcie-rockchip.c)
 *     → readl/readw/readb(reg_base + ECAM 오프셋)
 */
static int rockchip_pcie_rd_other_conf(struct rockchip_pcie *rockchip,
				       struct pci_bus *bus, u32 devfn,
				       int where, int size, u32 *val)
{
	/* [한국어] 접근할 주소. */
	void __iomem *addr;

	/* [한국어] 하위 장치용 ECAM 창(reg_base)에서 (버스, devfn, 오프셋)에 해당하는 주소를
	 * 계산한다. RC 자신의 config 가 APB 창에 있는 것과 대비된다. */
	addr = rockchip->reg_base + PCIE_ECAM_OFFSET(bus->number, devfn, where);

	/* [한국어] 정렬 검사. */
	if (!IS_ALIGNED((uintptr_t)addr, size)) {
		/* [한국어] 출력 버퍼를 0 으로. */
		*val = 0;
		/* [한국어] 오류 반환. */
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}

	/* [한국어] 접근 대상이 RC 바로 아래 버스면, */
	if (pci_is_root_bus(bus->parent))
		/* [한국어] Type 0 config 접근으로 설정한다 — 그 버스의 장치는 링크 상대편 자신이므로
		 * 버스 번호를 해석하지 않는 Type 0 를 쓴다. */
		rockchip_pcie_cfg_configuration_accesses(rockchip,
						AXI_WRAPPER_TYPE0_CFG);
	/* [한국어] 더 깊은 버스면, */
	else
		/* [한국어] Type 1 로 설정한다 — 중간 브리지가 버스 번호를 보고 전달해야 하기 때문이다.
		 * 이 설정을 config 접근마다 매번 바꾸는 것이 이 컨트롤러의 특징이다. */
		rockchip_pcie_cfg_configuration_accesses(rockchip,
						AXI_WRAPPER_TYPE1_CFG);

	/* [한국어] 4바이트 읽기. */
	if (size == 4) {
		/* [한국어] 32비트 그대로. */
		*val = readl(addr);
	/* [한국어] 2바이트. */
	} else if (size == 2) {
		/* [한국어] 16비트. */
		*val = readw(addr);
	/* [한국어] 1바이트. */
	} else if (size == 1) {
		/* [한국어] 8비트. */
		*val = readb(addr);
	/* [한국어] 그 밖의 폭. */
	} else {
		/* [한국어] 출력 버퍼를 0 으로. */
		*val = 0;
		/* [한국어] 오류 반환. */
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	/* [한국어] 성공. */
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * rockchip_pcie_wr_other_conf - 하위 장치의 config 공간에 쓴다
 *
 * @rockchip: 컨트롤러 객체.
 * @bus: 접근 대상 버스.
 * @devfn: 장치·함수 번호.
 * @where: config space 안의 바이트 오프셋.
 * @size: 쓸 폭.
 * @val: 쓸 값.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_BAD_REGISTER_NUMBER.
 *
 * 읽기 쪽과 대칭이며 같은 Type 0 / Type 1 설정을 매번 한다.
 * wr_own_conf 와 결정적으로 다른 점은 읽기-수정-쓰기가 필요 없다는 것이다 —
 * ECAM 창은 하드웨어가 바이트 단위 쓰기를 지원하므로 writeb/writew 를 그대로
 * 쓸 수 있고, 따라서 인접 RW1C 비트를 훼손할 위험도 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 정렬 위반과 잘못된 폭 모두 PCIBIOS_BAD_REGISTER_NUMBER.
 *
 * 호출 체인:
 *   rockchip_pcie_wr_conf() → [이 함수]
 *     → rockchip_pcie_cfg_configuration_accesses()
 *     → writel/writew/writeb(reg_base + ECAM 오프셋)
 */
static int rockchip_pcie_wr_other_conf(struct rockchip_pcie *rockchip,
				       struct pci_bus *bus, u32 devfn,
				       int where, int size, u32 val)
{
	/* [한국어] 접근할 주소. */
	void __iomem *addr;

	/* [한국어] 읽기 쪽과 같은 ECAM 주소 계산. */
	addr = rockchip->reg_base + PCIE_ECAM_OFFSET(bus->number, devfn, where);

	/* [한국어] 정렬 검사. 쓰기에는 출력 버퍼가 없어 0 으로 채울 것도 없다. */
	if (!IS_ALIGNED((uintptr_t)addr, size))
		/* [한국어] 오류 반환. */
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* [한국어] RC 바로 아래 버스면, */
	if (pci_is_root_bus(bus->parent))
		/* [한국어] Type 0. */
		rockchip_pcie_cfg_configuration_accesses(rockchip,
						AXI_WRAPPER_TYPE0_CFG);
	/* [한국어] 더 깊으면, */
	else
		/* [한국어] Type 1. */
		rockchip_pcie_cfg_configuration_accesses(rockchip,
						AXI_WRAPPER_TYPE1_CFG);

	/* [한국어] 4바이트 쓰기. */
	if (size == 4)
		/* [한국어] 그대로 쓴다. */
		writel(val, addr);
	/* [한국어] 2바이트. */
	else if (size == 2)
		/* [한국어] 16비트 쓰기. 읽기 쪽과 달리 여기서는 읽기-수정-쓰기가 필요 없다 —
		 * ECAM 창은 하드웨어가 바이트 단위 쓰기를 지원하기 때문이다
		 * (APB 창의 RC 자신 config 와 대비되는 점이다). */
		writew(val, addr);
	/* [한국어] 1바이트. */
	else if (size == 1)
		/* [한국어] 8비트 쓰기. */
		writeb(val, addr);
	/* [한국어] 그 밖의 폭. */
	else
		/* [한국어] 오류 반환. */
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* [한국어] 성공. */
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * rockchip_pcie_rd_conf - PCI 코어가 부르는 config 읽기 진입점
 *
 * @bus: 접근 대상 버스. sysdata 에 컨트롤러가 심어져 있다.
 * @devfn: 장치·함수 번호.
 * @where: config space 안의 바이트 오프셋.
 * @size: 읽을 폭.
 * @val: 결과를 담을 출력 인자.
 * @return: PCIBIOS_* 코드. 유효하지 않은 devfn 이면 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * 세 갈래로 나뉜다.
 *   1) valid_device() 로 그 자리에 장치가 있을 수 있는지 먼저 확인한다.
 *      없으면 DEVICE_NOT_FOUND 를 돌려주고, PCI 코어는 그 자리를 비어 있는
 *      것으로 처리한다.
 *   2) 루트 버스면 대상이 RC 자신이므로 APB 창 경로로 간다.
 *   3) 그 밖에는 ECAM 창 경로로 간다.
 *
 * 이 분기가 존재하는 이유는 RC 자신과 하위 장치의 config 가 물리적으로 다른
 * 레지스터 창에 있기 때문이며, 그래서 이 드라이버는 map_bus 콜백 대신
 * read/write 를 직접 구현한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 * PCI 코어가 전역 pci_lock 을 쥔 채 부른다.
 *
 * 에러 경로: 하위 함수의 PCIBIOS_* 를 그대로 전달한다.
 *
 * 호출 체인:
 *   pci_read_config_dword() 등 → bus->ops->read == [이 함수]
 *     → valid_device() → rd_own_conf() 또는 rd_other_conf()
 */
static int rockchip_pcie_rd_conf(struct pci_bus *bus, u32 devfn, int where,
				 int size, u32 *val)
{
	/* [한국어] PCI 코어가 bus->sysdata 에 심어 둔 컨트롤러를 꺼낸다. */
	struct rockchip_pcie *rockchip = bus->sysdata;

	/* [한국어] 위에서 본 슬롯 제약을 먼저 확인한다. */
	if (!rockchip_pcie_valid_device(rockchip, bus, PCI_SLOT(devfn)))
		/* [한국어] 유효하지 않은 devfn 이면 "장치 없음"으로 답한다. 코어는 이 값을 보고
		 * 그 자리를 비어 있는 것으로 처리한다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 루트 버스면 접근 대상이 RC 자신이다. */
	if (pci_is_root_bus(bus))
		/* [한국어] APB 창 경로로 간다. */
		return rockchip_pcie_rd_own_conf(rockchip, where, size, val);

	/* [한국어] 그 밖에는 ECAM 창 경로로 간다. */
	return rockchip_pcie_rd_other_conf(rockchip, bus, devfn, where, size,
					   val);
}

/* [한국어]
 * rockchip_pcie_wr_conf - PCI 코어가 부르는 config 쓰기 진입점
 *
 * @bus: 접근 대상 버스.
 * @devfn: 장치·함수 번호.
 * @where: config space 안의 바이트 오프셋.
 * @size: 쓸 폭.
 * @val: 쓸 값.
 * @return: PCIBIOS_* 코드.
 *
 * 읽기 진입점과 완전히 대칭이다 — 같은 valid_device() 검사를 하고,
 * 루트 버스면 APB 창 경로, 그 밖에는 ECAM 창 경로로 나눈다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 하위 함수의 PCIBIOS_* 를 그대로 전달한다.
 *
 * 호출 체인:
 *   pci_write_config_dword() 등 → bus->ops->write == [이 함수]
 *     → valid_device() → wr_own_conf() 또는 wr_other_conf()
 */
static int rockchip_pcie_wr_conf(struct pci_bus *bus, u32 devfn,
				 int where, int size, u32 val)
{
	/* [한국어] 컨트롤러를 꺼낸다. */
	struct rockchip_pcie *rockchip = bus->sysdata;

	/* [한국어] 같은 슬롯 제약 검사. */
	if (!rockchip_pcie_valid_device(rockchip, bus, PCI_SLOT(devfn)))
		/* [한국어] 장치 없음. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 루트 버스면, */
	if (pci_is_root_bus(bus))
		/* [한국어] APB 창 경로(읽기-수정-쓰기 포함). */
		return rockchip_pcie_wr_own_conf(rockchip, where, size, val);

	/* [한국어] 그 밖에는 ECAM 창 경로. */
	return rockchip_pcie_wr_other_conf(rockchip, bus, devfn, where, size,
					   val);
}

static struct pci_ops rockchip_pcie_ops = {
	/* [한국어] config 읽기 콜백. */
	.read = rockchip_pcie_rd_conf,
	/* [한국어] config 쓰기 콜백. map_bus 를 제공하지 않고 read/write 를 직접 구현하는 이유는,
	 * RC 자신과 하위 장치의 접근 경로가 아예 다른 창을 쓰기 때문이다 —
	 * 주소 하나로 통일할 수 없다. */
	.write = rockchip_pcie_wr_conf,
};

/* [한국어]
 * rockchip_pcie_set_power_limit - 슬롯 공급 전력을 RC 의 Device Capabilities 에 기록한다
 *
 * @rockchip: 컨트롤러 객체. vpcie3v3 레귤레이터에서 전류 한계를 얻는다.
 *
 * 왜 필요한가: PCIe 규격은 루트 포트가 "이 슬롯에 얼마나 전력을 줄 수 있는지"를
 * Device Capabilities 레지스터의 Slot Power Limit 필드에 광고하도록 한다.
 * 꽂힌 카드가 그 값을 읽어 자기 소비 전력을 조절한다. 위 영어 주석이 밝히듯
 * 하드웨어 기본값은 값·배율 모두 0 이라, 실제 전원 구성에 맞춰 소프트웨어가
 * 채워 넣어야 한다.
 *
 * 동작 과정:
 *   1) 3.3V 레귤레이터가 없으면(IS_ERR) 실제 공급 능력을 알 수 없어 그대로 돌아간다.
 *   2) 전류 한계를 마이크로암페어로 얻고, 0 이하면 한계가 정해지지 않은 것이라 포기한다.
 *   3) mA 로 바꾼 뒤 3.3V 를 곱해 밀리와트를 구한다.
 *   4) 값이 필드에 담기지 않으면 배율 코드를 한 단계 낮추고 값을 10 으로 나누기를
 *      반복한다. PCIe 는 전력을 (값 × 배율) 로 표현하므로, 같은 전력을 여러 조합으로
 *      쓸 수 있다는 성질을 이용한 정규화다. 배율을 더 낮출 수 없으면 값이 비정상이므로
 *      경고를 남기고 포기한다.
 *   5) 값과 배율을 각각의 필드에 넣어 되쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 * regulator_get_current_limit() 이 잠들 수 있으므로 probe 경로에서만 불려야 한다.
 *
 * 에러 경로: 세 곳에서 조용히 돌아간다. 반환값이 void 라 호출자는 설정 여부를
 * 알 수 없고, 실패해도 링크는 정상 동작한다.
 *
 * 호출 체인:
 *   rockchip_pcie_host_init_port() → [이 함수]
 *     → regulator_get_current_limit() → rockchip_pcie_read/write(PCI_EXP_DEVCAP)
 */
static void rockchip_pcie_set_power_limit(struct rockchip_pcie *rockchip)
{
	/* [한국어] 레귤레이터가 보고한 전류 한계(마이크로암페어). */
	int curr;
	/* [한국어] status: 레지스터 값. scale: 전력 단위 배율 코드. power: 계산한 전력 값. */
	u32 status, scale, power;

	/* [한국어] 3.3V 레귤레이터가 없으면 실제 공급 전력을 알 수 없다. */
	if (IS_ERR(rockchip->vpcie3v3))
		return;

	/*
	 * Set RC's captured slot power limit and scale if
	 * vpcie3v3 available. The default values are both zero
	 * which means the software should set these two according
	 * to the actual power supply.
	 */
	/* [한국어] 레귤레이터에서 전류 한계를 얻는다. 단위는 마이크로암페어다. */
	curr = regulator_get_current_limit(rockchip->vpcie3v3);
	/* [한국어] 0 이하면 한계가 정해지지 않은 것이라 계산할 수 없다. */
	if (curr <= 0)
		return;

	/* [한국어] 배율 코드 3 = 0.001배(옆의 상류 주석). PCIe 규격은 전력 값을
	 * (값 × 배율) 형태로 표현하며, 배율 코드가 클수록 작은 단위를 뜻한다. */
	scale = 3; /* 0.001x */
	/* [한국어] 마이크로암페어를 밀리암페어로. */
	curr = curr / 1000; /* convert to mA */
	/* [한국어] 3.3V 를 곱해 밀리와트를 구한다. (mA × 3300mV) / 1000 = mW 다. */
	power = (curr * 3300) / 1000; /* milliwatt */
	/* [한국어] 계산한 전력이 필드에 담을 수 있는 최대값을 넘으면, */
	while (power > FIELD_MAX(PCI_EXP_DEVCAP_PWR_VAL)) {
		/* [한국어] 더 낮출 배율이 없다면 값 자체가 이상한 것이다. */
		if (!scale) {
			/* [한국어] 경고를 남기고 설정을 포기한다. */
			dev_warn(rockchip->dev, "invalid power supply\n");
			return;
		}
		/* [한국어] 배율 코드를 한 단계 낮추고(= 단위를 10배 크게), */
		scale--;
		/* [한국어] 값을 10 으로 나눠 같은 전력을 다른 표현으로 만든다. */
		power = power / 10;
	}

	/* [한국어] Device Capabilities 레지스터를 읽는다. */
	status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_DEVCAP);
	/* [한국어] 전력 값 필드에 넣는다. */
	status |= FIELD_PREP(PCI_EXP_DEVCAP_PWR_VAL, power);
	/* [한국어] 배율 필드에 넣는다. */
	status |= FIELD_PREP(PCI_EXP_DEVCAP_PWR_SCL, scale);
	/* [한국어] 되쓴다. 이 값은 슬롯에 꽂힌 카드가 읽어 자기 소비 전력을 조절하는 데 쓴다. */
	rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_DEVCAP);
}

/**
 * rockchip_pcie_host_init_port - Initialize hardware
 * @rockchip: PCIe port information
 */
/* [한국어]
 * rockchip_pcie_host_init_port - RC 모드 하드웨어를 초기화하고 링크를 훈련시킨다
 *
 * @rockchip: 컨트롤러 객체.
 * @return: 0 = 성공(링크가 올라옴). 음수 = 공용 init_port 실패 또는 Gen1 훈련 시간 초과.
 *
 * 이 파일에서 가장 긴 함수이자 하드웨어 시퀀스의 중심이다. 위 커널독이 한 줄로
 * "Initialize hardware" 라고만 적어 둔 그 내용을 풀면 아홉 단계다.
 *
 *   1) PERST# 를 어서트해 엔드포인트를 리셋 상태에 둔다.
 *   2) 공용 init_port() — 리셋 시퀀스, PHY 초기화, MODE_RC 설정이 거기서 일어난다.
 *   3) L0s 탈출용 FTS 개수를 설정한다.
 *   4) 슬롯 공급 전력을 광고한다(set_power_limit).
 *   5) 공통 클럭 모드(SLC)와 RCB 128바이트를 설정한다. 공통 클럭은 RC 와
 *      엔드포인트가 같은 레퍼런스 클럭을 쓴다는 뜻이고, 그래야 상대가 공통 클럭
 *      모드로 훈련해 지연이 줄어든다.
 *   6) Gen1 훈련을 활성화하고, T_PVPERL 만큼 기다린 뒤 PERST# 를 해제한다.
 *      PCIe 규격이 요구하는 순서다 — 전원이 안정된 뒤에만 리셋을 풀 수 있다.
 *      이어서 RESET_CONFIG_WAIT 만큼 더 기다려야 config 접근이 가능해진다.
 *   7) 링크 업을 500ms 까지 폴링한다. 실패하면 PHY 를 되감고 오류를 낸다.
 *   8) link_gen 이 2 면 Gen2 재훈련을 요청한다. 위 영어 주석대로 Gen1 이 끝난
 *      뒤에만 설정해야 한다. 시간 초과여도 오류가 아니라 디버그 로그만 남기고
 *      Gen1 로 계속 간다 — 속도가 낮을 뿐 링크는 살아 있기 때문이다.
 *   9) 협상된 레인 수를 확인하고, 쓰이지 않는 레인의 PHY 전원을 내린다.
 *      그다음 Vendor ID 와 class code 를 채우고(그러지 않으면 PCI 코어가 루트
 *      포트를 브리지로 인식하지 못한다), THP capability 의 next 포인터를 지워
 *      L1 서브스테이트 capability 를 목록에서 끊어 내고, DT 가 요청하면 L0s 능력도
 *      숨기고, 마지막으로 MPS 를 256바이트로 고정한다.
 *
 * capability 를 "숨기는" 두 조작(THP next 지우기, L0s 비트 지우기)은 능력 자체를
 * 없애는 것이 아니라 상대가 볼 수 없게 만드는 방식이다. capability 가 연결
 * 리스트라 next 를 0 으로 만들면 그 뒤가 통째로 보이지 않는다.
 *
 * 실행 컨텍스트: probe 또는 resume 경로, 프로세스 컨텍스트.
 * msleep 과 폴링이 있어 최대 1초 남짓 걸릴 수 있다.
 *
 * 에러 경로: 라벨이 하나(err_power_off_phy)다. 진입 시 i 를 MAX_LANE_NUM 으로
 * 초기화해 두었기 때문에, 그 라벨이 while(i--) 두 번으로 모든 레인의
 * power_off 와 exit 를 짝 맞춰 되돌릴 수 있다. 다만 2)의 실패는 PHY 가 아직
 * 켜지지 않은 상태라 라벨을 거치지 않고 곧장 반환한다.
 *
 * 호출 체인:
 *   rockchip_pcie_probe() / rockchip_pcie_resume_noirq() → [이 함수]
 *     → rockchip_pcie_init_port() (공용 pcie-rockchip.c)
 *     → rockchip_pcie_set_power_limit() → readl_poll_timeout()
 *     → rockchip_pcie_lane_map() → phy_power_off()
 */
static int rockchip_pcie_host_init_port(struct rockchip_pcie *rockchip)
{
	/* [한국어] 로그 대상. */
	struct device *dev = rockchip->dev;
	/* [한국어] err: 각 단계 결과. i: 레인 인덱스이자 오류 되감기 카운터 —
	 * MAX_LANE_NUM 으로 초기화하는 이유는 아래 err_power_off_phy 라벨이
	 * 이 값을 그대로 써서 되감기 때문이다. */
	int err, i = MAX_LANE_NUM;
	/* [한국어] 레지스터 값. */
	u32 status;

	/* [한국어] PERST# 를 어서트한다(논리 0). 엔드포인트를 리셋 상태로 두고 초기화를 시작한다. */
	gpiod_set_value_cansleep(rockchip->perst_gpio, 0);

	/* [한국어] 공용 코드의 포트 초기화 — 리셋 시퀀스, PHY 초기화, 모드 설정이 거기서 일어난다. */
	err = rockchip_pcie_init_port(rockchip);
	/* [한국어] 실패 검사. */
	if (err)
		/* [한국어] PHY 가 아직 켜지지 않았으므로 되감을 것이 없다. */
		return err;

	/* Fix the transmitted FTS count desired to exit from L0s. */
	/* [한국어] L0s 탈출용 FTS 개수 레지스터를 읽는다(옆의 상류 주석). */
	status = rockchip_pcie_read(rockchip, PCIE_CORE_CTRL_PLC1);
	/* [한국어] 기존 FTS 필드를 지우고, */
	status = (status & ~PCIE_CORE_CTRL_PLC1_FTS_MASK) |
		 /* [한국어] 정해진 개수를 넣는다. */
		 (PCIE_CORE_CTRL_PLC1_FTS_CNT << PCIE_CORE_CTRL_PLC1_FTS_SHIFT);
	/* [한국어] 되쓴다. */
	rockchip_pcie_write(rockchip, status, PCIE_CORE_CTRL_PLC1);

	/* [한국어] 위에서 본 전력 한계를 설정한다. */
	rockchip_pcie_set_power_limit(rockchip);

	/* Set RC's clock architecture as common clock */
	/* [한국어] 공통 클럭 구조임을 알리기 위해 Link Control 을 읽는다(옆의 상류 주석). */
	status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);
	/* [한국어] SLC(Slot Clock Configuration) 비트를 상위 16비트 마스크 자리에 세워 쓴다.
	 * 이 비트는 "RC 와 엔드포인트가 같은 레퍼런스 클럭을 쓴다"는 뜻이고,
	 * 그래야 상대가 공통 클럭 모드로 훈련해 지연이 줄어든다. */
	status |= PCI_EXP_LNKSTA_SLC << 16;
	/* [한국어] 되쓴다. */
	rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);

	/* Set RC's RCB to 128 */
	/* [한국어] RCB(Read Completion Boundary)를 128바이트로 설정하기 위해 다시 읽는다(옆의 상류 주석). */
	status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);
	/* [한국어] RCB 비트를 세운다. */
	status |= PCI_EXP_LNKCTL_RCB;
	/* [한국어] 되쓴다. */
	rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);

	/* Enable Gen1 training */
	/* [한국어] 링크 훈련을 시작한다(옆의 상류 주석). 이 쓰기 전에는 링크가 올라오지 않는다. */
	rockchip_pcie_write(rockchip, PCIE_CLIENT_LINK_TRAIN_ENABLE,
			    PCIE_CLIENT_CONFIG);

	/* [한국어] PCIe 규격의 T_PVPERL — 전원이 안정된 뒤 PERST# 를 해제하기까지 기다려야 하는
	 * 최소 시간이다. 상수 정의는 drivers/pci/pci.h 에 있다. */
	msleep(PCIE_T_PVPERL_MS);
	/* [한국어] PERST# 를 해제한다(논리 1). 이 순간부터 엔드포인트가 훈련을 시작한다. */
	gpiod_set_value_cansleep(rockchip->perst_gpio, 1);

	/* [한국어] PERST# 해제 후 config 접근이 가능해지기까지 기다려야 하는 규격 시간. */
	msleep(PCIE_RESET_CONFIG_WAIT_MS);

	/* 500ms timeout value should be enough for Gen1/2 training */
	/* [한국어] 링크 업 비트가 설 때까지 20us 간격으로 폴링한다. */
	err = readl_poll_timeout(rockchip->apb_base + PCIE_CLIENT_BASIC_STATUS1,
				 status, PCIE_LINK_UP(status), 20,
				 /* [한국어] 위 영어 주석대로 Gen1/2 훈련에는 500ms 면 충분하다는 판단이다. */
				 500 * USEC_PER_MSEC);
	/* [한국어] 시간 초과. */
	if (err) {
		/* [한국어] 훈련 실패를 알린다. */
		dev_err(dev, "PCIe link training gen1 timeout!\n");
		/* [한국어] 켜 둔 PHY 를 끄는 되감기 구간으로. */
		goto err_power_off_phy;
	}

	/* [한국어] Gen2 를 목표로 하는 보드라면, */
	if (rockchip->link_gen == 2) {
		/*
		 * Enable retrain for gen2. This should be configured only after
		 * gen1 finished.
		 */
		/* [한국어] 위 영어 주석대로 Gen1 훈련이 끝난 뒤에만 Gen2 재훈련을 설정해야 한다.
		 * Link Control 2 를 읽어, */
		status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL2);
		/* [한국어] 목표 링크 속도 필드를 지우고, */
		status &= ~PCI_EXP_LNKCTL2_TLS;
		/* [한국어] 5.0GT/s(Gen2)를 넣는다. */
		status |= PCI_EXP_LNKCTL2_TLS_5_0GT;
		/* [한국어] 되쓴다. */
		rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL2);
		/* [한국어] Link Control 을 읽어, */
		status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);
		/* [한국어] Retrain Link 비트를 세운다 — 이 비트가 재훈련을 유발한다. */
		status |= PCI_EXP_LNKCTL_RL;
		/* [한국어] 되쓴다. */
		rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCTL);

		/* [한국어] Gen2 로 올라갔는지 같은 방식으로 폴링한다. */
		err = readl_poll_timeout(rockchip->apb_base + PCIE_CORE_CTRL,
					 status, PCIE_LINK_IS_GEN2(status), 20,
					 500 * USEC_PER_MSEC);
		/* [한국어] 시간 초과여도, */
		if (err)
			/* [한국어] 오류가 아니라 디버그 로그만 남기고 Gen1 로 계속 간다. 속도가 낮을 뿐
			 * 링크는 살아 있기 때문이다. */
			dev_dbg(dev, "PCIe link training gen2 timeout, fall back to gen1!\n");
	}

	/* Check the final link width from negotiated lane counter from MGMT */
	/* [한국어] 협상된 최종 레인 수를 읽는다(옆의 상류 주석). */
	status = rockchip_pcie_read(rockchip, PCIE_CORE_CTRL);
	/* [한국어] 레지스터의 코드 값을 실제 레인 수로 바꾼다 — 1 << 코드 이므로
	 * 코드 0 은 x1, 1 은 x2, 2 는 x4 를 뜻한다. */
	status = 0x1 << ((status & PCIE_CORE_PL_CONF_LANE_MASK) >>
			  PCIE_CORE_PL_CONF_LANE_SHIFT);
	/* [한국어] 결과를 디버그 로그로 남긴다. */
	dev_dbg(dev, "current link width is x%d\n", status);

	/* Power off unused lane(s) */
	/* [한국어] 실제로 훈련된 레인 비트맵을 구한다. */
	rockchip->lanes_map = rockchip_pcie_lane_map(rockchip);
	/* [한국어] 모든 레인을 순회하며, */
	for (i = 0; i < MAX_LANE_NUM; i++) {
		/* [한국어] 훈련되지 않은 레인은, */
		if (!(rockchip->lanes_map & BIT(i))) {
			/* [한국어] 어느 레인을 끄는지 알리고, */
			dev_dbg(dev, "idling lane %d\n", i);
			/* [한국어] 그 PHY 의 전원을 내린다(옆의 상류 주석). 쓰지 않는 레인의 SerDes 를
			 * 켜 둘 이유가 없다. */
			phy_power_off(rockchip->phys[i]);
		}
	}

	/* [한국어] RC 자신의 Vendor ID 를 Rockchip 으로 설정한다. 하드웨어 기본값이
	 * 올바르지 않아 소프트웨어가 채워 넣어야 한다. */
	rockchip_pcie_write(rockchip, PCI_VENDOR_ID_ROCKCHIP,
			    PCIE_CORE_CONFIG_VENDOR);
	/* [한국어] class code 도 채워 넣는다. */
	rockchip_pcie_write(rockchip,
			    /* [한국어] PCI-to-PCI 브리지 코드를 8비트 밀어 넣는다 — 하위 1바이트는 Revision ID 자리다.
			     * 이 값이 없으면 PCI 코어가 루트 포트를 브리지로 인식하지 못해
			     * 하위 버스를 열거하지 않는다. */
			    PCI_CLASS_BRIDGE_PCI_NORMAL << 8,
			    PCIE_RC_CONFIG_RID_CCR);

	/* Clear THP cap's next cap pointer to remove L1 substate cap */
	/* [한국어] THP capability 의 next 포인터를 지워 그 뒤에 이어진 L1 서브스테이트
	 * capability 를 목록에서 끊어 낸다(옆의 상류 주석). capability 는 연결
	 * 리스트라 next 를 0 으로 만들면 그 뒤가 통째로 보이지 않게 된다 —
	 * 지원하지 않는 기능을 감추는 흔한 수법이다. */
	status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_THP_CAP);
	/* [한국어] next 필드를 지운다. */
	status &= ~PCIE_RC_CONFIG_THP_CAP_NEXT_MASK;
	/* [한국어] 되쓴다. */
	rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_THP_CAP);

	/* Clear L0s from RC's link cap */
	/* [한국어] DT 가 L0s 를 쓰지 말라고 하면(옆의 상류 주석), */
	if (of_property_read_bool(dev->of_node, "aspm-no-l0s")) {
		/* [한국어] Link Capabilities 를 읽어, */
		status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCAP);
		/* [한국어] L0s 지원 비트를 지우고, */
		status &= ~PCI_EXP_LNKCAP_ASPM_L0S;
		/* [한국어] 되쓴다. 능력 자체를 숨겨 상대가 L0s 로 진입하지 않게 만든다. */
		rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_LNKCAP);
	}

	/* [한국어] Device Control 을 읽어, */
	status = rockchip_pcie_read(rockchip, PCIE_RC_CONFIG_CR + PCI_EXP_DEVCTL);
	/* [한국어] Max Payload Size 필드를 지우고, */
	status &= ~PCI_EXP_DEVCTL_PAYLOAD;
	/* [한국어] 256바이트를 넣는다. */
	status |= PCI_EXP_DEVCTL_PAYLOAD_256B;
	/* [한국어] 되쓴다. */
	rockchip_pcie_write(rockchip, status, PCIE_RC_CONFIG_CR + PCI_EXP_DEVCTL);

	/* [한국어] 초기화 성공. */
	return 0;
/* [한국어] 링크 훈련 실패 전용 되감기 라벨. */
err_power_off_phy:
	/* [한국어] i 는 진입 시 MAX_LANE_NUM 이므로, while(i--) 가 3,2,1,0 순으로 모든 레인을 돈다. */
	while (i--)
		/* [한국어] 각 레인의 PHY 전원을 내린다. */
		phy_power_off(rockchip->phys[i]);
	/* [한국어] 두 번째 순회를 위해 다시 초기화한다. */
	i = MAX_LANE_NUM;
	/* [한국어] 같은 방식으로, */
	while (i--)
		/* [한국어] 각 레인의 PHY 초기화를 되돌린다. power_off 와 exit 를 분리해 두 번 도는 것은
		 * PHY 프레임워크의 짝 규약을 지키기 위해서다. */
		phy_exit(rockchip->phys[i]);
	/* [한국어] 기록해 둔 오류를 전달한다. */
	return err;
}

/* [한국어]
 * rockchip_pcie_subsys_irq_handler - 코어 내부 오류와 PHY 링크 변화를 처리한다
 *
 * @irq: 인터럽트 번호. 사용하지 않는다.
 * @arg: devm_request_irq 에 넘긴 컨트롤러 객체.
 * @return: 언제나 IRQ_HANDLED.
 *
 * 이 컨트롤러는 인터럽트를 세 갈래로 나눠 받는다 — sys(이 핸들러),
 * client(다음 핸들러), legacy(INTx 체인 핸들러). 세 갈래가 같은
 * PCIE_CLIENT_INT_STATUS 레지스터를 보되 서로 다른 비트만 처리하고,
 * 자기 몫만 명시적으로 지운다.
 *
 * 이 핸들러가 맡는 것은 둘이다.
 *   - PCIE_CLIENT_INT_LOCAL: 코어 내부 오류. 세부 원인 레지스터를 읽어
 *     열네 가지 조건을 하나씩 확인하고 각각 디버그 로그를 남긴다. FIFO 패리티
 *     오류, 오버플로, 재전송 타이머 시간 초과, 형식이 잘못된 TLP, 기대하지 않은
 *     완료, 흐름 제어 광고 오류 등 링크 품질과 하드웨어 상태를 진단하는 정보다.
 *     처리 후 읽은 값을 그대로 되써서 지운다(RW1C).
 *   - PCIE_CLIENT_INT_PHY: PHY 링크 변화. 링크 속도가 바뀌었을 수 있으므로
 *     Tx 크레딧 갱신 주기를 다시 설정하고 대역폭 인터럽트 상태를 지운다.
 *
 * 로그가 전부 dev_dbg 인 이유는 이 조건들이 정상 동작 중에도 드물게 발생할 수
 * 있어, 기본 빌드에서 dmesg 를 채우면 곤란하기 때문이다.
 *
 * [상류 코드 관찰] 두 갈래가 else if 로 묶여 있어, 로컬 오류와 PHY 변화가
 * 동시에 올라오면 PHY 쪽이 이번 회차에 처리되지 않는다. 다만 마지막 쓰기가
 * LOCAL 비트만 지우므로 PHY 비트는 남아 다음 인터럽트에서 처리된다.
 * 또 IRQF_SHARED 로 등록되었는데도 항상 IRQ_HANDLED 를 돌려주어,
 * 다른 장치의 인터럽트까지 자기 것으로 주장한다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. 잠들 수 없고, 이 파일의 어떤 락도 잡지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   하드웨어 인터럽트 → devm_request_irq 로 등록된 [이 함수]
 *     → rockchip_pcie_read/write / update_txcredit_mui() / clr_bw_int()
 */
static irqreturn_t rockchip_pcie_subsys_irq_handler(int irq, void *arg)
{
	/* [한국어] 인터럽트 등록 시 넘긴 컨트롤러 객체. */
	struct rockchip_pcie *rockchip = arg;
	/* [한국어] 로그 대상. */
	struct device *dev = rockchip->dev;
	/* [한국어] 클라이언트 인터럽트 상태. */
	u32 reg;
	/* [한국어] 코어 인터럽트 세부 상태. */
	u32 sub_reg;

	/* [한국어] 어떤 종류의 인터럽트인지 먼저 확인한다. */
	reg = rockchip_pcie_read(rockchip, PCIE_CLIENT_INT_STATUS);
	/* [한국어] 로컬(코어 내부) 오류 계열이면, */
	if (reg & PCIE_CLIENT_INT_LOCAL) {
		/* [한국어] 그 사실을 알리고,이 계열 로그는 전부 dev_dbg 라 기본 빌드에서는 출력되지 않는다 —
		 * 정상 동작 중에도 드물게 발생할 수 있는 상태 보고이기 때문이다. */
		dev_dbg(dev, "local interrupt received\n");
		/* [한국어] 세부 원인 레지스터를 읽는다. */
		sub_reg = rockchip_pcie_read(rockchip, PCIE_CORE_INT_STATUS);
		/* [한국어] PNP 수신 FIFO RAM 읽기 중 패리티 오류. */
		if (sub_reg & PCIE_CORE_INT_PRFPE)
			dev_dbg(dev, "parity error detected while reading from the PNP receive FIFO RAM\n");

		/* [한국어] 완료(Completion) 수신 FIFO RAM 읽기 중 패리티 오류. */
		if (sub_reg & PCIE_CORE_INT_CRFPE)
			dev_dbg(dev, "parity error detected while reading from the Completion Receive FIFO RAM\n");

		/* [한국어] 재전송 버퍼 RAM 읽기 중 패리티 오류. */
		if (sub_reg & PCIE_CORE_INT_RRPE)
			dev_dbg(dev, "parity error detected while reading from replay buffer RAM\n");

		/* [한국어] PNP 수신 FIFO 오버플로. */
		if (sub_reg & PCIE_CORE_INT_PRFO)
			dev_dbg(dev, "overflow occurred in the PNP receive FIFO\n");

		/* [한국어] 완료 수신 FIFO 오버플로. */
		if (sub_reg & PCIE_CORE_INT_CRFO)
			dev_dbg(dev, "overflow occurred in the completion receive FIFO\n");

		/* [한국어] 재전송 타이머 시간 초과 — 상대가 ACK 를 보내지 않았다는 뜻이다. */
		if (sub_reg & PCIE_CORE_INT_RT)
			dev_dbg(dev, "replay timer timed out\n");

		/* [한국어] 같은 TLP 를 네 번 재전송했는데도 실패 — 링크 품질 문제를 시사한다. */
		if (sub_reg & PCIE_CORE_INT_RTR)
			dev_dbg(dev, "replay timer rolled over after 4 transmissions of the same TLP\n");

		/* [한국어] 수신 측 PHY 오류. */
		if (sub_reg & PCIE_CORE_INT_PE)
			dev_dbg(dev, "phy error detected on receive side\n");

		/* [한국어] 형식이 잘못된 TLP 수신. */
		if (sub_reg & PCIE_CORE_INT_MTR)
			dev_dbg(dev, "malformed TLP received from the link\n");

		/* [한국어] 기대하지 않은 완료 수신 — 요청하지 않은 응답이 왔다는 뜻이다. */
		if (sub_reg & PCIE_CORE_INT_UCR)
			dev_dbg(dev, "Unexpected Completion received from the link\n");

		/* [한국어] 흐름 제어 광고에서 오류 발견. */
		if (sub_reg & PCIE_CORE_INT_FCE)
			dev_dbg(dev, "an error was observed in the flow control advertisements from the other side\n");

		/* [한국어] 요청이 완료를 기다리다 시간 초과. */
		if (sub_reg & PCIE_CORE_INT_CT)
			dev_dbg(dev, "a request timed out waiting for completion\n");

		/* [한국어] 매핑되지 않은 TC(Traffic Class) 오류. */
		if (sub_reg & PCIE_CORE_INT_UTC)
			dev_dbg(dev, "unmapped TC error\n");

		/* [한국어] MSI 마스크 레지스터 변경. */
		if (sub_reg & PCIE_CORE_INT_MMVC)
			dev_dbg(dev, "MSI mask register changes\n");

		/* [한국어] 읽은 세부 상태를 그대로 되써서 지운다 — RW1C 관용이다. */
		rockchip_pcie_write(rockchip, sub_reg, PCIE_CORE_INT_STATUS);
	/* [한국어] PHY 링크 변화 인터럽트면, */
	} else if (reg & PCIE_CLIENT_INT_PHY) {
		/* [한국어] 그 사실을 알리고,이 계열 로그는 전부 dev_dbg 라 기본 빌드에서는 출력되지 않는다 —
		 * 정상 동작 중에도 드물게 발생할 수 있는 상태 보고이기 때문이다. */
		dev_dbg(dev, "phy link changes\n");
		/* [한국어] 링크 속도가 바뀌었을 수 있으므로 Tx 크레딧 갱신 주기를 다시 설정하고, */
		rockchip_pcie_update_txcredit_mui(rockchip);
		/* [한국어] 대역폭 관리 인터럽트 상태를 지운다. */
		rockchip_pcie_clr_bw_int(rockchip);
	}

	/* [한국어] 클라이언트 상태 레지스터에서 로컬 비트만 지운다. reg 전체가 아니라
	 * AND 로 걸러 쓰는 이유는, 이 핸들러가 처리한 원인만 지우고 다른 비트는
	 * 해당 핸들러가 지우도록 남겨 두기 위해서다. */
	rockchip_pcie_write(rockchip, reg & PCIE_CLIENT_INT_LOCAL,
			    PCIE_CLIENT_INT_STATUS);

	/* [한국어] 이 컨트롤러의 인터럽트이므로 처리했다고 답한다. */
	return IRQ_HANDLED;
}

/* [한국어]
 * rockchip_pcie_client_irq_handler - 클라이언트 계열 이벤트를 로그로 남긴다
 *
 * @irq: 인터럽트 번호. 사용하지 않는다.
 * @arg: 컨트롤러 객체.
 * @return: 언제나 IRQ_HANDLED.
 *
 * subsys 핸들러와 같은 상태 레지스터를 보지만 관심 비트가 다르다 — 이쪽은
 * 레거시 전달 완료, 메시지 전달 완료, 핫 리셋 수신, DPA, 치명적/비치명적/정정
 * 가능 오류, PHY 인터럽트 여덟 가지다.
 *
 * 하는 일은 각 비트에 대해 디버그 로그를 남기고, 마지막에 자기 몫의 비트들만
 * 골라 되써서 지우는 것이 전부다. 실제 복구 동작은 하지 않는다 — 오류 복구는
 * AER 계층이, 핫 리셋 대응은 PCI 코어가 담당하기 때문이다.
 *
 * 지울 비트를 하나하나 나열하는 것이 중요하다. subsys 핸들러가 지우는
 * PCIE_CLIENT_INT_LOCAL 이 그 목록에 없어, 두 핸들러가 같은 레지스터를
 * 공유하면서도 서로의 상태를 지우지 않는다.
 *
 * [상류 코드 관찰] subsys 쪽과 마찬가지로 IRQF_SHARED 인데 항상 IRQ_HANDLED 다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   하드웨어 인터럽트 → devm_request_irq 로 등록된 [이 함수]
 */
static irqreturn_t rockchip_pcie_client_irq_handler(int irq, void *arg)
{
	/* [한국어] 컨트롤러 객체. */
	struct rockchip_pcie *rockchip = arg;
	/* [한국어] 로그 대상. */
	struct device *dev = rockchip->dev;
	/* [한국어] 상태 레지스터 값. */
	u32 reg;

	/* [한국어] 클라이언트 인터럽트 상태를 읽는다. 위 subsys 핸들러와 같은 레지스터를 보지만
	 * 관심 있는 비트가 다르다 — 그쪽은 로컬 오류와 PHY 변화, 이쪽은 그 밖의 전부다. */
	reg = rockchip_pcie_read(rockchip, PCIE_CLIENT_INT_STATUS);
	/* [한국어] 레거시 INTx 전달 완료. */
	if (reg & PCIE_CLIENT_INT_LEGACY_DONE)
		dev_dbg(dev, "legacy done interrupt received\n");

	/* [한국어] 메시지 전달 완료. */
	if (reg & PCIE_CLIENT_INT_MSG)
		dev_dbg(dev, "message done interrupt received\n");

	/* [한국어] 핫 리셋 수신 — 상대가 링크 리셋을 요청했다는 뜻이다. */
	if (reg & PCIE_CLIENT_INT_HOT_RST)
		dev_dbg(dev, "hot reset interrupt received\n");

	/* [한국어] DPA(Dynamic Power Allocation) 관련 인터럽트. */
	if (reg & PCIE_CLIENT_INT_DPA)
		dev_dbg(dev, "dpa interrupt received\n");

	/* [한국어] 치명적 오류 수신. */
	if (reg & PCIE_CLIENT_INT_FATAL_ERR)
		dev_dbg(dev, "fatal error interrupt received\n");

	/* [한국어] 비치명적 오류 수신. */
	if (reg & PCIE_CLIENT_INT_NFATAL_ERR)
		dev_dbg(dev, "non fatal error interrupt received\n");

	/* [한국어] 정정 가능한 오류 수신. */
	if (reg & PCIE_CLIENT_INT_CORR_ERR)
		dev_dbg(dev, "correctable error interrupt received\n");

	/* [한국어] PHY 인터럽트. */
	if (reg & PCIE_CLIENT_INT_PHY)
		dev_dbg(dev, "phy interrupt received\n");

	/* [한국어] 이 핸들러가 담당하는 비트들만 골라 되써서 지운다(RW1C).
	 * subsys 핸들러가 지우는 PCIE_CLIENT_INT_LOCAL 은 이 목록에 없다 —
	 * 두 핸들러가 같은 레지스터를 공유하면서 서로의 비트를 건드리지 않도록
	 * 각자 자기 몫만 명시적으로 나열하는 구조다. */
	rockchip_pcie_write(rockchip, reg & (PCIE_CLIENT_INT_LEGACY_DONE |
			      PCIE_CLIENT_INT_MSG | PCIE_CLIENT_INT_HOT_RST |
			      PCIE_CLIENT_INT_DPA | PCIE_CLIENT_INT_FATAL_ERR |
			      PCIE_CLIENT_INT_NFATAL_ERR |
			      PCIE_CLIENT_INT_CORR_ERR |
			      PCIE_CLIENT_INT_PHY),
		   PCIE_CLIENT_INT_STATUS);

	/* [한국어] 처리했다고 답한다. */
	return IRQ_HANDLED;
}

/* [한국어]
 * rockchip_pcie_intx_handler - INTx 하나를 받아 네 개의 하위 인터럽트로 분배한다
 *
 * @desc: 이 IRQ 의 irq_desc. handler_data 에 컨트롤러가 심어져 있다.
 *
 * 레거시 INTx 는 네 개의 선(INTA~INTD)인데 이 컨트롤러는 그것을 GIC 인터럽트
 * 하나로 모아 올린다. 그래서 일반 핸들러가 아니라 체인 핸들러로 등록되고,
 * 상위 컨트롤러의 흐름 제어를 직접 다루기 위해 chained_irq_enter/exit 로
 * 감싸야 한다.
 *
 * 동작 과정:
 *   1) 클라이언트 상태 레지스터를 읽어 INTx 비트들만 골라 하위로 내린다.
 *      마스크로 자르고 시프트해 0~3 을 그대로 표현하는 4비트 값으로 정규화한다.
 *   2) 서 있는 비트가 없어질 때까지 반복하며, ffs 로 가장 낮은 비트를 찾아
 *      0 기반 인덱스로 바꾸고, 처리한 비트를 지운다.
 *   3) generic_handle_domain_irq() 로 그 INTx 에 묶인 리눅스 IRQ 핸들러를 부른다.
 *      도메인이 hwirq → virq 사상을 알고 있으므로 여기서 장치 드라이버까지 이어진다.
 *   4) 등록되지 않은 INTx 가 어서트되면 오류 로그를 남긴다 — 흔히 하드웨어
 *      설정 오류를 뜻한다.
 *
 * [상류 코드 관찰] 이 핸들러는 상태 비트를 지우지 않는다. INTx 는 레벨 트리거라
 * 장치가 스스로 신호를 내려야 하기 때문이며, 그래서 하드웨어 상태를 쓰는 대신
 * 읽기만 한다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트(체인 핸들러). 잠들 수 없다.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   하드웨어 INTx → GIC → 체인 핸들러 == [이 함수]
 *     → generic_handle_domain_irq() → 장치 드라이버 핸들러
 */
static void rockchip_pcie_intx_handler(struct irq_desc *desc)
{
	/* [한국어] 이 IRQ 를 소유한 상위 인터럽트 칩. 체인 진입·이탈에 필요하다. */
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] irq_desc 에 심어 둔 컨트롤러 객체. */
	struct rockchip_pcie *rockchip = irq_desc_get_handler_data(desc);
	/* [한국어] 로그 대상. */
	struct device *dev = rockchip->dev;
	/* [한국어] 상태 레지스터 값. */
	u32 reg;
	/* [한국어] 처리할 INTx 번호(0~3). */
	u32 hwirq;
	/* [한국어] 도메인 디스패치 결과. */
	int ret;

	/* [한국어] 체인 핸들러 진입 — 상위 컨트롤러에 처리 시작을 알린다. */
	chained_irq_enter(chip, desc);

	/* [한국어] 클라이언트 인터럽트 상태를 읽는다. */
	reg = rockchip_pcie_read(rockchip, PCIE_CLIENT_INT_STATUS);
	/* [한국어] 그중 INTx 비트들만 골라 하위로 내린다. 마스크로 자르고 시프트해
	 * 0~3 을 그대로 표현하는 4비트 값으로 정규화한다. */
	reg = (reg & PCIE_CLIENT_INTR_MASK) >> PCIE_CLIENT_INTR_SHIFT;

	/* [한국어] 서 있는 비트가 없어질 때까지 반복한다 — 여러 INTx 가 동시에 어서트될 수 있다. */
	while (reg) {
		/* [한국어] ffs 는 가장 낮은 1 비트의 위치를 1부터 세어 돌려주므로, 1 을 빼면 0 기반
		 * 인덱스가 된다. */
		hwirq = ffs(reg) - 1;
		/* [한국어] 처리한 비트를 지워 다음 반복에서 다시 잡히지 않게 한다. */
		reg &= ~BIT(hwirq);

		/* [한국어] 그 INTx 에 묶인 리눅스 IRQ 핸들러를 부른다. 도메인이 hwirq → virq 사상을
		 * 알고 있으므로 여기서 장치 드라이버까지 이어진다. */
		ret = generic_handle_domain_irq(rockchip->irq_domain, hwirq);
		/* [한국어] 핸들러가 등록되지 않은 INTx 가 어서트되면, */
		if (ret)
			/* [한국어] 어느 선인지 알린다. 흔히 하드웨어 설정 오류를 뜻한다. */
			dev_err(dev, "unexpected IRQ, INT%d\n", hwirq);
	}

	/* [한국어] 체인 핸들러 이탈 — EOI 를 보낸다. */
	chained_irq_exit(chip, desc);
}

/* [한국어]
 * rockchip_pcie_setup_irq - 세 개의 인터럽트를 각각 알맞은 방식으로 등록한다
 *
 * @rockchip: 컨트롤러 객체.
 * @return: 0 = 성공. 음수 = IRQ 조회 실패(-EPROBE_DEFER 포함) 또는 등록 실패.
 *
 * 세 인터럽트를 DT 의 interrupt-names 로 찾는다. 이름으로 찾기 때문에 DT 에서
 * 순서가 바뀌어도 안전하다.
 *   - "sys": subsys 핸들러를 IRQF_SHARED 일반 인터럽트로 등록.
 *   - "legacy": INTx 체인 핸들러로 등록. 하나를 넷으로 분배해야 하므로
 *     일반 등록이 아니라 irq_set_chained_handler_and_data() 를 쓴다.
 *   - "client": client 핸들러를 IRQF_SHARED 일반 인터럽트로 등록.
 *
 * IRQF_SHARED 를 쓰는 이유는 이 IRQ 선을 SoC 의 다른 블록과 공유할 수 있기
 * 때문이다.
 *
 * [상류 코드 관찰] "legacy" 만 devm_ 이 아닌 방식으로 걸린다. 따라서 자동
 * 해제되지 않는데, probe 실패 경로도 remove 경로도 그것을 떼어 내지 않는다.
 * IRQ 도메인만 제거되고 체인 핸들러는 남는 셈이다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 세 지점 모두 곧장 return 한다. 앞서 등록한 devm 인터럽트는
 * 드라이버 코어가 회수하지만, 체인 핸들러는 위 관찰대로 남는다.
 *
 * 호출 체인:
 *   rockchip_pcie_probe() → [이 함수]
 *     → platform_get_irq_byname() ×3 → devm_request_irq() ×2
 *     → irq_set_chained_handler_and_data() ×1
 */
static int rockchip_pcie_setup_irq(struct rockchip_pcie *rockchip)
{
	/* [한국어] irq: 얻은 IRQ 번호. err: 등록 결과. */
	int irq, err;
	/* [한국어] 로그 대상. */
	struct device *dev = rockchip->dev;
	/* [한국어] platform_get_irq_byname 에 넘길 플랫폼 디바이스. */
	struct platform_device *pdev = to_platform_device(dev);

	/* [한국어] DT 의 interrupt-names 에서 "sys" 인터럽트를 찾는다. 이름으로 찾기 때문에
	 * DT 에서 순서가 바뀌어도 안전하다. */
	irq = platform_get_irq_byname(pdev, "sys");
	/* [한국어] 실패 검사. 반환값이 음수면 그 자체가 errno 다. */
	if (irq < 0)
		/* [한국어] 그대로 전달한다 — -EPROBE_DEFER 도 이 경로로 올라가 재시도가 성립한다. */
		return irq;

	/* [한국어] subsys 핸들러를 일반 인터럽트로 등록한다. IRQF_SHARED 인 이유는 이 IRQ 선을
	 * SoC 의 다른 블록과 공유할 수 있기 때문이다. */
	err = devm_request_irq(dev, irq, rockchip_pcie_subsys_irq_handler,
			       IRQF_SHARED, "pcie-sys", rockchip);
	/* [한국어] 등록 실패. */
	if (err) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "failed to request PCIe subsystem IRQ\n");
		/* [한국어] 오류 전달. */
		return err;
	}

	/* [한국어] "legacy" 인터럽트를 찾는다 — INTx 전용 선이다. */
	irq = platform_get_irq_byname(pdev, "legacy");
	/* [한국어] 실패 검사. */
	if (irq < 0)
		/* [한국어] 오류 전달. */
		return irq;

	/* [한국어] 이쪽은 devm_request_irq 가 아니라 체인 핸들러로 건다. INTx 하나를 받아
	 * 네 개의 하위 인터럽트로 분배해야 하기 때문이며, 그래서 IRQ 도메인이 필요하다.
	 * [상류 코드 관찰] devm 이 아니므로 자동 해제되지 않는다 —
	 * remove 경로가 명시적으로 떼어 내야 한다. */
	irq_set_chained_handler_and_data(irq,
					 rockchip_pcie_intx_handler,
					 rockchip);

	/* [한국어] "client" 인터럽트를 찾는다. */
	irq = platform_get_irq_byname(pdev, "client");
	/* [한국어] 실패 검사. */
	if (irq < 0)
		/* [한국어] 오류 전달. */
		return irq;

	/* [한국어] client 핸들러를 일반 인터럽트로 등록한다. 역시 공유 가능이다. */
	err = devm_request_irq(dev, irq, rockchip_pcie_client_irq_handler,
			       IRQF_SHARED, "pcie-client", rockchip);
	/* [한국어] 등록 실패. */
	if (err) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "failed to request PCIe client IRQ\n");
		/* [한국어] 오류 전달. */
		return err;
	}

	/* [한국어] 세 인터럽트 모두 설정 완료. */
	return 0;
}

/**
 * rockchip_pcie_parse_host_dt - Parse Device Tree
 * @rockchip: PCIe port information
 *
 * Return: '0' on success and error value on failure
 */
/* [한국어]
 * rockchip_pcie_parse_host_dt - 공용 DT 파싱에 더해 호스트 전용 레귤레이터 넷을 얻는다
 *
 * @rockchip: 컨트롤러 객체.
 * @return: 0 = 성공, 음수 = 실패(위 커널독 참조).
 *
 * 먼저 공용 rockchip_pcie_parse_dt() 를 부른다 — 레지스터 창 둘, 클럭, 리셋,
 * PERST# GPIO 가 거기서 채워진다. 이 함수는 그 위에 호스트에만 필요한
 * 전원 레귤레이터 넷을 더한다.
 *
 * 레귤레이터를 두 부류로 나눠 다루는 것이 이 함수의 핵심이다.
 *   - 12V 와 3.3V: optional 판을 쓴다. 보드에 따라 없을 수 있기 때문이다.
 *     -ENODEV(= DT 에 없음)는 정상으로 보고 정보 로그만 남긴 뒤 계속 진행한다.
 *     이때 필드에는 ERR_PTR(-ENODEV) 가 담긴 채로 남고, 이후 코드가 IS_ERR() 로
 *     "없음"을 판별한다 — NULL 이 아니라 오류 포인터를 부재 표시로 쓰는 관용이다.
 *     set_vpcie() 와 probe 의 되감기, set_power_limit() 이 모두 그 규약을 따른다.
 *   - 1.8V 와 0.9V: 필수다. 없으면 곧장 실패한다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트. 레귤레이터 조회가 잠들 수 있다.
 *
 * 에러 경로: 네 지점 모두 곧장 return 한다. devm 자원이라 정리할 것이 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_probe() → [이 함수]
 *     → rockchip_pcie_parse_dt() (공용 pcie-rockchip.c)
 *     → devm_regulator_get_optional() ×2 / devm_regulator_get() ×2
 */
static int rockchip_pcie_parse_host_dt(struct rockchip_pcie *rockchip)
{
	/* [한국어] 로그 대상. */
	struct device *dev = rockchip->dev;
	/* [한국어] 각 단계 결과. */
	int err;

	/* [한국어] 먼저 공용 코드의 DT 파싱을 부른다 — 레지스터 창, 클럭, 리셋, PERST# GPIO 가
	 * 거기서 채워진다. 이 함수는 그 위에 호스트 전용인 레귤레이터 넷을 더한다. */
	err = rockchip_pcie_parse_dt(rockchip);
	/* [한국어] 실패 전파. */
	if (err)
		return err;

	/* [한국어] 12V 보조 전원. optional 판을 쓰는 이유는 보드에 따라 없을 수 있기 때문이다. */
	rockchip->vpcie12v = devm_regulator_get_optional(dev, "vpcie12v");
	/* [한국어] 오류 포인터 검사. */
	if (IS_ERR(rockchip->vpcie12v)) {
		/* [한국어] -ENODEV 는 "그런 레귤레이터가 DT 에 없다"는 뜻이라 정상이다.
		 * 그 밖의 오류만 실패로 취급한다. */
		if (PTR_ERR(rockchip->vpcie12v) != -ENODEV)
			return PTR_ERR(rockchip->vpcie12v);
		/* [한국어] 없다는 사실만 정보 로그로 남기고 계속 진행한다. 이때 vpcie12v 에는
		 * ERR_PTR(-ENODEV) 가 담긴 채로 남으며, 이후 코드가 IS_ERR() 로 그것을 구분한다 —
		 * NULL 이 아니라 오류 포인터를 "없음" 표시로 쓰는 관용이다. */
		dev_info(dev, "no vpcie12v regulator found\n");
	}

	/* [한국어] 3.3V 슬롯 주 전원. 역시 optional 이다. */
	rockchip->vpcie3v3 = devm_regulator_get_optional(dev, "vpcie3v3");
	/* [한국어] 오류 검사. */
	if (IS_ERR(rockchip->vpcie3v3)) {
		/* [한국어] -ENODEV 가 아니면 진짜 실패. */
		if (PTR_ERR(rockchip->vpcie3v3) != -ENODEV)
			return PTR_ERR(rockchip->vpcie3v3);
		/* [한국어] 없으면 정보 로그만. 이 경우 set_power_limit() 이 전력 한계 계산을 건너뛴다. */
		dev_info(dev, "no vpcie3v3 regulator found\n");
	}

	/* [한국어] 1.8V 는 필수다 — optional 이 아닌 판을 쓴다. */
	rockchip->vpcie1v8 = devm_regulator_get(dev, "vpcie1v8");
	/* [한국어] 없거나 오류면, */
	if (IS_ERR(rockchip->vpcie1v8))
		/* [한국어] 곧장 실패로 전달한다. */
		return PTR_ERR(rockchip->vpcie1v8);

	/* [한국어] 0.9V 도 필수다. */
	rockchip->vpcie0v9 = devm_regulator_get(dev, "vpcie0v9");
	/* [한국어] 오류 검사. */
	if (IS_ERR(rockchip->vpcie0v9))
		/* [한국어] 실패 전달. */
		return PTR_ERR(rockchip->vpcie0v9);

	/* [한국어] 모든 자원 확보 완료. */
	return 0;
}

/* [한국어]
 * rockchip_pcie_set_vpcie - 네 전원을 순서대로 켜고, 실패하면 계단식으로 되돌린다
 *
 * @rockchip: 컨트롤러 객체.
 * @return: 0 = 네 전원 모두 인가됨. 음수 = 어느 하나의 인가 실패.
 *
 * 인가 순서는 12V → 3.3V → 1.8V → 0.9V 다. 12V 와 3.3V 는 보드에 없을 수
 * 있으므로 IS_ERR() 로 존재를 확인한 뒤에만 켠다 — parse_host_dt() 가 남겨 둔
 * ERR_PTR(-ENODEV) 를 그 표시로 쓴다.
 *
 * 되감기가 이 함수의 볼거리다. 라벨 네 개(err_disable_1v8, err_disable_3v3,
 * err_disable_12v, err_out)가 계단식으로 이어져, 각 단계의 실패가 자기보다
 * 앞서 성공한 것만 정확히 되돌린다. 되돌리는 쪽에서도 12V 와 3.3V 는 존재
 * 확인을 다시 하므로, 없는 레귤레이터를 끄려 시도하지 않는다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트. regulator_enable() 이 잠들 수 있다.
 *
 * 에러 경로: 위의 계단식 되감기. 이 함수가 실패해 돌아가면 켜 둔 전원이 하나도
 * 없으므로, 호출자는 전원에 대해 추가로 정리할 것이 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_probe() → [이 함수] → regulator_enable() ×4
 */
static int rockchip_pcie_set_vpcie(struct rockchip_pcie *rockchip)
{
	/* [한국어] 로그 대상. */
	struct device *dev = rockchip->dev;
	/* [한국어] 각 단계 결과. */
	int err;

	/* [한국어] 12V 가 있는 보드에서만 켠다. IS_ERR 로 "없음"을 구분하는 것이 위에서 본 관용이다. */
	if (!IS_ERR(rockchip->vpcie12v)) {
		/* [한국어] 레귤레이터를 켠다. */
		err = regulator_enable(rockchip->vpcie12v);
		/* [한국어] 실패 검사. */
		if (err) {
			/* [한국어] 실패 로그. */
			dev_err(dev, "fail to enable vpcie12v regulator\n");
			/* [한국어] 아직 아무것도 켜지 않았으므로 곧장 반환 구간으로. */
			goto err_out;
		}
	}

	/* [한국어] 3.3V 가 있으면 켠다. */
	if (!IS_ERR(rockchip->vpcie3v3)) {
		/* [한국어] 레귤레이터를 켠다. */
		err = regulator_enable(rockchip->vpcie3v3);
		/* [한국어] 실패 검사. */
		if (err) {
			/* [한국어] 실패 로그. */
			dev_err(dev, "fail to enable vpcie3v3 regulator\n");
			/* [한국어] 12V 를 되돌리는 구간으로. */
			goto err_disable_12v;
		}
	}

	/* [한국어] 1.8V 는 필수이므로 조건 없이 켠다. */
	err = regulator_enable(rockchip->vpcie1v8);
	/* [한국어] 실패 검사. */
	if (err) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "fail to enable vpcie1v8 regulator\n");
		/* [한국어] 3.3V 부터 되돌리는 구간으로. */
		goto err_disable_3v3;
	}

	/* [한국어] 0.9V 도 조건 없이 켠다. */
	err = regulator_enable(rockchip->vpcie0v9);
	/* [한국어] 실패 검사. */
	if (err) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "fail to enable vpcie0v9 regulator\n");
		/* [한국어] 1.8V 부터 되돌리는 구간으로. */
		goto err_disable_1v8;
	}

	/* [한국어] 네 전원 모두 인가 완료. */
	return 0;

/* [한국어] 0.9V 실패 전용 라벨. */
err_disable_1v8:
	/* [한국어] 1.8V 를 끈다. */
	regulator_disable(rockchip->vpcie1v8);
/* [한국어] 1.8V 실패가 함께 도달하는 라벨. */
err_disable_3v3:
	/* [한국어] 3.3V 가 있는 보드에서만, */
	if (!IS_ERR(rockchip->vpcie3v3))
		/* [한국어] 끈다. */
		regulator_disable(rockchip->vpcie3v3);
/* [한국어] 3.3V 실패가 도달하는 라벨. */
err_disable_12v:
	/* [한국어] 12V 가 있는 보드에서만, */
	if (!IS_ERR(rockchip->vpcie12v))
		/* [한국어] 끈다. */
		regulator_disable(rockchip->vpcie12v);
/* [한국어] 12V 실패와 위 되감기가 모두 모이는 라벨. 계단식 되감기의 전형적인 형태로,
 * 각 단계가 자기보다 앞서 성공한 것만 정확히 되돌린다. */
err_out:
	/* [한국어] 기록해 둔 오류를 전달한다. */
	return err;
}

/* [한국어]
 * rockchip_pcie_enable_interrupts - 인터럽트 마스크를 풀고 대역폭 인터럽트를 켠다
 *
 * @rockchip: 컨트롤러 객체.
 *
 * 두 개의 마스크 레지스터를 서로 다른 방식으로 다룬다.
 *   - PCIE_CLIENT_INT_MASK: 상위 16비트를 write-enable 마스크로 쓰는 레지스터다.
 *     상위에 대상 비트를 세우고 하위에 ~비트(= 0)를 놓아, 그 비트만 0 으로
 *     만든다. 이 하드웨어에서 마스크 0 이 "허용"을 뜻한다.
 *   - PCIE_CORE_INT_MASK: 그런 장치가 없는 일반 레지스터라 ~PCIE_CORE_INT 를
 *     그대로 쓴다.
 * 그다음 대역폭 관리 인터럽트를 켠다.
 *
 * 이 함수가 불리는 시점이 중요하다. probe 에서는 세 핸들러를 모두 등록한
 * 뒤에 불려, 핸들러 없는 인터럽트가 올라오는 일이 없다. suspend 실패 경로에서는
 * 방금 끈 인터럽트를 되살리는 되감기로 쓰인다.
 *
 * 실행 컨텍스트: probe / resume / suspend 되감기, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_probe() / rockchip_pcie_resume_noirq() /
 *   rockchip_pcie_suspend_noirq()(실패 되감기) → [이 함수]
 *     → rockchip_pcie_write() ×2 → rockchip_pcie_enable_bw_int()
 */
static void rockchip_pcie_enable_interrupts(struct rockchip_pcie *rockchip)
{
	/* [한국어] 클라이언트 인터럽트 마스크를 설정한다. 상위 16비트에 "이 비트들을 쓰겠다"는
	 * 마스크를 놓고 하위 16비트에 값을 놓는 write-enable 방식이다.
	 * 값 쪽이 ~PCIE_CLIENT_INT_CLI 이므로 해당 비트를 0 으로 만들어 마스크를 해제한다 —
	 * 이 하드웨어에서 마스크 비트 0 이 "허용"을 뜻한다. */
	rockchip_pcie_write(rockchip, (PCIE_CLIENT_INT_CLI << 16) &
			    (~PCIE_CLIENT_INT_CLI), PCIE_CLIENT_INT_MASK);
	/* [한국어] 코어 인터럽트 마스크도 같은 방식으로 해제한다. 이쪽은 상위 마스크가 없는
	 * 일반 레지스터라 ~PCIE_CORE_INT 를 그대로 쓴다. */
	rockchip_pcie_write(rockchip, (u32)(~PCIE_CORE_INT),
			    PCIE_CORE_INT_MASK);

	/* [한국어] 대역폭 관리 인터럽트를 켠다 — 링크 속도·폭 변화를 감지하기 위해서다. */
	rockchip_pcie_enable_bw_int(rockchip);
}

/* [한국어]
 * rockchip_pcie_intx_map - INTx 하나를 IRQ 도메인에 처음 매핑할 때 불린다
 *
 * @domain: 이 컨트롤러의 INTx 도메인.
 * @irq: 코어가 배정한 리눅스 IRQ 번호.
 * @hwirq: 하드웨어 인터럽트 번호(0~3, 곧 INTA~INTD).
 * @return: 언제나 0.
 *
 * 선형 도메인은 hwirq 를 배열 인덱스로 그대로 쓰는 가장 단순한 형태이고,
 * alloc/free 대신 map 콜백 하나만 요구한다.
 *
 * dummy_irq_chip 을 쓰는 것이 핵심이다. 이 컨트롤러에는 개별 INTx 를
 * 마스킹하는 하드웨어 수단이 없어 mask/unmask 로 할 일이 없기 때문이다.
 * 흐름 핸들러도 마스킹·EOI 가 필요 없는 handle_simple_irq 를 쓴다.
 *
 * [상류 코드 관찰] irq_set_chip_data() 로 도메인의 host_data(= 컨트롤러)를
 * 붙여 두지만, 그 값을 irq_get_chip_data 로 꺼내 쓰는 코드가 이 파일에 없다.
 * 관례적으로 넣어 둔 것으로 보인다.
 *
 * 실행 컨텍스트: 첫 INTx 매핑 시, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   irq_create_mapping() / generic_handle_domain_irq() 의 첫 호출
 *     → irq_domain_ops.map == [이 함수]
 */
static int rockchip_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				  irq_hw_number_t hwirq)
{
	/* [한국어] 이 INTx 를 dummy_irq_chip 과 handle_simple_irq 에 묶는다.
	 * dummy 칩을 쓰는 이유는 개별 INTx 를 마스킹하는 하드웨어 수단이 없기 때문이고,
	 * handle_simple_irq 는 마스킹·EOI 가 필요 없는 가장 단순한 흐름 핸들러다. */
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);
	/* [한국어] 도메인의 host_data(= 컨트롤러)를 이 IRQ 에 붙여 둔다.
	 * [상류 코드 관찰] 이 값을 나중에 irq_get_chip_data 로 꺼내 쓰는 코드가
	 * 이 파일에 없다 — 관례적으로 넣어 둔 것으로 보인다. */
	irq_set_chip_data(irq, domain->host_data);

	/* [한국어] 매핑 성공. */
	return 0;
}

static const struct irq_domain_ops intx_domain_ops = {
	/* [한국어] 도메인이 새 hwirq 를 처음 만났을 때 불릴 콜백. alloc/free 가 아니라 map 만
	 * 제공하는 것은 선형 도메인의 단순한 형태다. */
	.map = rockchip_pcie_intx_map,
};

/* [한국어]
 * rockchip_pcie_init_irq_domain - INTx 네 개를 담을 선형 IRQ 도메인을 만든다
 *
 * @rockchip: 컨트롤러 객체.
 * @return: 0 = 성공. -EINVAL = DT 에 인터럽트 컨트롤러 자식 노드가 없거나
 *       도메인 생성 실패.
 *
 * DT 바인딩상 이 컨트롤러 노드 아래에 인터럽트 컨트롤러를 나타내는 자식 노드가
 * 하나 있어야 한다. 그 노드의 fwnode 로 도메인을 만들어야, 하위 장치의 DT 가
 * interrupt-parent 로 그것을 가리켰을 때 사상이 성립한다.
 *
 * of_get_next_child() 는 참조 카운트를 올리므로 반드시 내려야 한다. 이 함수는
 * 도메인 생성 직후, 성공 여부와 무관하게 of_node_put() 을 부른다 — 도메인이
 * fwnode 를 자체적으로 붙잡기 때문에 여기서 더 들고 있을 이유가 없다.
 *
 * 선형 도메인을 고른 이유는 hwirq 가 0~3 으로 적고 고정이기 때문이다.
 * 이런 경우 배열 하나로 충분해 트리나 해시 도메인의 부담이 필요 없다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 두 지점 모두 곧장 return 한다. 자식 노드 부재는 참조를 얻기 전이라
 * 정리할 것이 없고, 도메인 생성 실패는 이미 of_node_put() 을 마친 뒤다.
 *
 * 호출 체인:
 *   rockchip_pcie_probe() → [이 함수]
 *     → of_get_next_child() → irq_domain_create_linear() → of_node_put()
 */
static int rockchip_pcie_init_irq_domain(struct rockchip_pcie *rockchip)
{
	/* [한국어] 로그 대상. */
	struct device *dev = rockchip->dev;
	/* [한국어] 인터럽트 컨트롤러를 나타내는 DT 자식 노드를 얻는다. 참조 카운트가 올라가므로
	 * 아래에서 반드시 내려야 한다. */
	struct device_node *intc = of_get_next_child(dev->of_node, NULL);

	/* [한국어] 그런 자식 노드가 없으면 DT 가 이 드라이버의 바인딩을 만족하지 못한 것이다. */
	if (!intc) {
		/* [한국어] 무엇이 빠졌는지 알린다. */
		dev_err(dev, "missing child interrupt-controller node\n");
		/* [한국어] -EINVAL 로 중단한다. */
		return -EINVAL;
	}

	/* [한국어] INTx 네 개(PCI_NUM_INTX)를 담을 선형 도메인을 만든다. 선형 도메인은
	 * hwirq 를 배열 인덱스로 그대로 쓰는 가장 단순한 형태로, 개수가 적고
	 * 고정일 때 적합하다. rockchip 을 host_data 로 넘겨 콜백이 되찾게 한다. */
	rockchip->irq_domain = irq_domain_create_linear(of_fwnode_handle(intc), PCI_NUM_INTX,
							&intx_domain_ops, rockchip);
	/* [한국어] 도메인 생성 성공 여부와 무관하게 노드 참조를 곧바로 내린다 —
	 * 도메인이 fwnode 를 자체적으로 붙잡기 때문에 여기서 더 들고 있을 이유가 없다. */
	of_node_put(intc);
	/* [한국어] 생성 실패 검사. */
	if (!rockchip->irq_domain) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "failed to get a INTx IRQ domain\n");
		/* [한국어] -EINVAL 로 중단. */
		return -EINVAL;
	}

	/* [한국어] 도메인 준비 완료. */
	return 0;
}

/* [한국어]
 * rockchip_pcie_prog_ob_atu - 아웃바운드 주소 변환 창 하나를 설정한다
 *
 * @rockchip: 컨트롤러 객체.
 * @region_no: 창 번호. 0번은 config 접근용으로 예약되어 있어 실제 데이터 창은 1번부터다.
 * @type: 창의 종류(AXI_WRAPPER_MEM_WRITE / IO_WRITE / NOR_MSG).
 * @num_pass_bits: 그대로 통과시킬 하위 주소 비트 수. 창 크기는 2^(값+1) 바이트다.
 * @lower_addr: 이 창이 대응할 PCI 주소의 하위 32비트.
 * @upper_addr: 그 상위 32비트.
 * @return: 0 = 성공. -EINVAL = 창 번호나 크기가 하드웨어 제약을 벗어남.
 *
 * 아웃바운드 창은 CPU 가 낸 주소를 PCI 버스 주소로 바꿔 링크 너머로 내보내는
 * 장치다. 창마다 "어느 CPU 주소 범위를 어느 PCI 주소로, 어떤 종류의 트랜잭션으로
 * 보낼지"를 정한다.
 *
 * 크기를 통과 비트 수로 표현하는 방식이 이 하드웨어의 특징이다. n 비트를
 * 통과시킨다는 것은 하위 n+1 비트가 창 안의 오프셋이라는 뜻이므로 창 크기가
 * 2^(n+1) 이 된다. 그래서 1MB 창을 만들려면 19 를 넘긴다.
 *
 * 네 가지 유효성 검사를 한다 — 창 번호 상한, 최소 크기(256바이트), 64비트
 * 주소 범위, 그리고 0번 창과 그 밖의 창에 대해 서로 다른 최대 크기.
 *
 * [값의 근거] 서술자에 함께 세우는 비트 23 의 의미는 이 트리의 헤더에도
 * 코드에도 설명이 없어 확인할 수 없다. 모든 창에 무조건 세우는 것으로 보아
 * "창 활성화" 계열로 추정되지만 단정하지 않는다.
 *
 * 실행 컨텍스트: probe 또는 resume 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 다섯 개의 검사가 모두 -EINVAL 을 돌려준다. 레지스터를 쓰기 전에
 * 걸러지므로 되돌릴 것이 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_cfg_atu() → [이 함수] → rockchip_pcie_write() ×4
 */
static int rockchip_pcie_prog_ob_atu(struct rockchip_pcie *rockchip,
				     int region_no, int type, u8 num_pass_bits,
				     u32 lower_addr, u32 upper_addr)
{
	/* [한국어] 창의 하위 주소와 통과 비트 수를 담을 레지스터 값. */
	u32 ob_addr_0;
	/* [한국어] 창의 상위 주소. */
	u32 ob_addr_1;
	/* [한국어] 창의 종류(메모리 쓰기 / IO 쓰기 / 메시지)를 담을 서술자. */
	u32 ob_desc_0;
	/* [한국어] 이 창의 레지스터 블록 오프셋. */
	u32 aw_offset;

	/* [한국어] 창 번호가 하드웨어가 가진 개수를 넘으면, */
	if (region_no >= MAX_AXI_WRAPPER_REGION_NUM)
		/* [한국어] -EINVAL. */
		return -EINVAL;
	/* [한국어] 통과 비트 수 + 1 이 8 미만이면 창이 256바이트보다 작다는 뜻이라 허용하지 않는다. */
	if (num_pass_bits + 1 < 8)
		/* [한국어] -EINVAL. */
		return -EINVAL;
	/* [한국어] 63 을 넘으면 64비트 주소 공간을 벗어난다. */
	if (num_pass_bits > 63)
		/* [한국어] -EINVAL. */
		return -EINVAL;
	/* [한국어] 0번 창은 크기 상한이 다르다. */
	if (region_no == 0) {
		/* [한국어] 요청 크기(2 << num_pass_bits)가 0번 창의 최대 크기를 넘으면, */
		if (AXI_REGION_0_SIZE < (2ULL << num_pass_bits))
			/* [한국어] -EINVAL. */
			return -EINVAL;
	}
	/* [한국어] 그 밖의 창은, */
	if (region_no != 0) {
		/* [한국어] 일반 창 최대 크기와 비교한다. */
		if (AXI_REGION_SIZE < (2ULL << num_pass_bits))
			/* [한국어] -EINVAL. */
			return -EINVAL;
	}

	/* [한국어] 창 번호를 레지스터 블록 크기만큼 시프트해 오프셋을 만든다. 창마다
	 * 같은 배치의 레지스터 묶음이 연속으로 놓여 있다는 뜻이다. */
	aw_offset = (region_no << OB_REG_SIZE_SHIFT);

	/* [한국어] 통과 비트 수를 해당 필드에 넣는다. "몇 개의 하위 주소 비트를 그대로
	 * 통과시킬 것인가"가 곧 창 크기를 정한다 — n 이면 2^(n+1) 바이트다. */
	ob_addr_0 = num_pass_bits & PCIE_CORE_OB_REGION_ADDR0_NUM_BITS;
	/* [한국어] 하위 주소를 같은 워드의 다른 필드에 OR 로 넣는다. */
	ob_addr_0 |= lower_addr & PCIE_CORE_OB_REGION_ADDR0_LO_ADDR;
	/* [한국어] 상위 주소는 별도 레지스터에 그대로 들어간다. */
	ob_addr_1 = upper_addr;
	/* [한국어] 서술자에 종류 코드와 함께 비트 23 을 세운다.
	 * [값의 근거] 비트 23 의 의미는 이 트리의 헤더에도 코드에도 설명이 없어
	 * 확인할 수 없다. 모든 창에 무조건 세우는 것으로 보아 "창 활성화" 계열로
	 * 추정되지만 단정하지 않는다. */
	ob_desc_0 = (1 << 23 | type);

	/* [한국어] 하위 주소 + 통과 비트 수를 쓴다. */
	rockchip_pcie_write(rockchip, ob_addr_0,
			    PCIE_CORE_OB_REGION_ADDR0 + aw_offset);
	/* [한국어] 상위 주소를 쓴다. */
	rockchip_pcie_write(rockchip, ob_addr_1,
			    PCIE_CORE_OB_REGION_ADDR1 + aw_offset);
	/* [한국어] 서술자를 쓴다. */
	rockchip_pcie_write(rockchip, ob_desc_0,
			    PCIE_CORE_OB_REGION_DESC0 + aw_offset);
	/* [한국어] 서술자 두 번째 워드는 0 으로 지운다. 이전 설정이 남아 있으면 창이
	 * 엉뚱하게 동작하므로 명시적으로 비운다. */
	rockchip_pcie_write(rockchip, 0,
			    PCIE_CORE_OB_REGION_DESC1 + aw_offset);

	/* [한국어] 창 설정 완료. */
	return 0;
}

/* [한국어]
 * rockchip_pcie_prog_ib_atu - 인바운드 주소 변환 창 하나를 설정한다
 *
 * @rockchip: 컨트롤러 객체.
 * @region_no: 창 번호.
 * @num_pass_bits: 통과시킬 하위 주소 비트 수.
 * @lower_addr: 이 창이 대응할 CPU 주소의 하위 부분.
 * @upper_addr: 그 상위 32비트.
 * @return: 0 = 성공, -EINVAL = 제약 위반.
 *
 * 아웃바운드의 반대 방향이다. 엔드포인트가 DMA 로 낸 주소를 CPU 메모리 주소로
 * 바꿔 시스템 메모리에 닿게 한다. 이 창을 열지 않으면 어떤 DMA 도 동작하지 않는다.
 *
 * 아웃바운드와 세 가지가 다르다.
 *   1) 창 번호 검사가 >= 가 아니라 > 다. MAX_AXI_IB_ROOTPORT_REGION_NUM 이
 *      "개수"가 아니라 "최대 번호"이기 때문이다.
 *   2) 레지스터 블록 시프트 값이 다르다 — 인바운드 창의 레지스터 묶음이 더 작다.
 *   3) 하위 주소를 8비트 왼쪽으로 밀어 넣는다. 이 레지스터에서 주소 필드가
 *      비트 8 부터 시작하기 때문이며, 뒤집어 말하면 하위 8비트(256바이트)는
 *      항상 통과시킨다는 뜻이다.
 * 또 종류를 고를 것이 없어 서술자 레지스터가 아예 없다.
 *
 * 실행 컨텍스트: probe 또는 resume 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 세 개의 검사가 -EINVAL 을 돌려준다.
 *
 * 호출 체인:
 *   rockchip_pcie_cfg_atu() → [이 함수] → rockchip_pcie_write() ×2
 */
static int rockchip_pcie_prog_ib_atu(struct rockchip_pcie *rockchip,
				     int region_no, u8 num_pass_bits,
				     u32 lower_addr, u32 upper_addr)
{
	/* [한국어] 창의 하위 주소와 통과 비트 수. */
	u32 ib_addr_0;
	/* [한국어] 창의 상위 주소. */
	u32 ib_addr_1;
	/* [한국어] 레지스터 블록 오프셋. */
	u32 aw_offset;

	/* [한국어] 인바운드 창 번호 범위 검사. 아웃바운드와 달리 >= 가 아니라 > 인데,
	 * MAX_AXI_IB_ROOTPORT_REGION_NUM 이 "최대 번호"이지 "개수"가 아니기 때문이다. */
	if (region_no > MAX_AXI_IB_ROOTPORT_REGION_NUM)
		/* [한국어] -EINVAL. */
		return -EINVAL;
	/* [한국어] 최소 통과 비트 수 검사. 인바운드는 별도 상수를 쓴다. */
	if (num_pass_bits + 1 < MIN_AXI_ADDR_BITS_PASSED)
		/* [한국어] -EINVAL. */
		return -EINVAL;
	/* [한국어] 64비트 범위 검사. */
	if (num_pass_bits > 63)
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* [한국어] 창 번호로 레지스터 블록 오프셋을 만든다. 아웃바운드와 시프트 값이 다르다 —
	 * 인바운드 창의 레지스터 묶음이 더 작기 때문이다. */
	aw_offset = (region_no << IB_ROOT_PORT_REG_SIZE_SHIFT);

	/* [한국어] 통과 비트 수를 필드에 넣는다. */
	ib_addr_0 = num_pass_bits & PCIE_CORE_IB_REGION_ADDR0_NUM_BITS;
	/* [한국어] 하위 주소를 8비트 왼쪽으로 밀어 넣는다. 아웃바운드와 달리 시프트가 필요한 것은
	 * 이 레지스터에서 주소 필드가 비트 8 부터 시작하기 때문이다 —
	 * 즉 하위 8비트(256바이트)는 항상 통과시킨다는 뜻이기도 하다. */
	ib_addr_0 |= (lower_addr << 8) & PCIE_CORE_IB_REGION_ADDR0_LO_ADDR;
	/* [한국어] 상위 주소. */
	ib_addr_1 = upper_addr;

	/* [한국어] 두 값을 각각의 레지스터에 쓴다. 아웃바운드와 달리 서술자가 없다 —
	 * 인바운드는 종류를 고를 것이 없기 때문이다. */
	rockchip_pcie_write(rockchip, ib_addr_0, PCIE_RP_IB_ADDR0 + aw_offset);
	/* [한국어] 상위 주소 쓰기. */
	rockchip_pcie_write(rockchip, ib_addr_1, PCIE_RP_IB_ADDR1 + aw_offset);

	/* [한국어] 창 설정 완료. */
	return 0;
}

/* [한국어]
 * rockchip_pcie_cfg_atu - 메모리·I/O·메시지 창과 인바운드 창을 모두 설정한다
 *
 * @rockchip: 컨트롤러 객체. 브리지의 windows 목록에서 자원을 읽는다.
 * @return: 0 = 성공. -ENODEV = 메모리나 I/O 자원이 없음.
 *       그 밖의 음수 = 창 설정 실패.
 *
 * 이 컨트롤러의 아웃바운드 창은 하나가 1MB 로 고정이다. 그래서 큰 자원을 덮으려면
 * 창을 여러 개 이어 붙여야 하고, 이 함수가 그 배치를 담당한다.
 *
 * 동작 과정:
 *   1) config 접근을 Type 0 로 초기화한다.
 *   2) 메모리 자원을 찾아 그 PCI 주소를 msg_bus_addr 의 기준으로 기억해 둔다.
 *   3) 크기를 1MB 로 나눈 개수만큼 창을 채운다. 창 번호는 1 부터 시작하는데,
 *      0번이 config 접근용으로 예약되어 있기 때문이다.
 *   4) 인바운드 창 2번에 32비트 전체를 통과시키는 설정을 건다 — 엔드포인트가
 *      DMA 로 시스템 메모리 하위 4GB 전체에 닿을 수 있게 여는 것이다.
 *   5) I/O 자원에 대해 같은 일을 반복한다. 창 번호는 메모리 창 다음부터 이어진다.
 *      그 이어붙임을 위해 메모리 창 개수를 offset 에 기억해 두는데, size 가
 *      다음 줄에서 덮어써지므로 계산 순서가 중요하다.
 *   6) 마지막 창 하나를 메시지 전용(NOR_MSG)으로 배정하고, 그 위치만큼
 *      msg_bus_addr 에 더해 최종 주소를 완성한다. 그 주소를 probe 가 ioremap 해
 *      msg_region 으로 만들고, wait_l2() 가 거기에 써서 PME_TURN_OFF 를 보낸다.
 *
 * [상류 코드 관찰, 수정하지 않음] 6)의 prog_ob_atu() 호출만 반환값을 검사하지
 * 않는다. 그리고 마지막 return err 이 돌려주는 것은 그 호출의 결과가 아니라
 * 직전 I/O 루프의 마지막 결과다 — 루프가 한 번도 돌지 않았다면 4)의 인바운드
 * 설정 결과가 남아 있으며, 어느 경우든 그 시점에 err 은 0 이다.
 *
 * 실행 컨텍스트: probe 또는 resume 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 다섯 지점이 곧장 return 한다. 이미 설정한 창을 되돌리지 않지만,
 * 호출자가 실패 시 컨트롤러 전체를 리셋하므로 문제가 되지 않는다.
 *
 * 호출 체인:
 *   rockchip_pcie_probe() / rockchip_pcie_resume_noirq() → [이 함수]
 *     → resource_list_first_type() → rockchip_pcie_prog_ob_atu() ×N
 *     → rockchip_pcie_prog_ib_atu()
 */
static int rockchip_pcie_cfg_atu(struct rockchip_pcie *rockchip)
{
	/* [한국어] 로그 대상. */
	struct device *dev = rockchip->dev;
	/* [한국어] 이 컨트롤러를 private 로 품고 있는 호스트 브리지. windows 목록이 필요하다. */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(rockchip);
	/* [한국어] 자원 목록 항목. */
	struct resource_entry *entry;
	/* [한국어] pci_addr: 창이 대응할 PCI 버스 주소. size: 창 크기. */
	u64 pci_addr, size;
	/* [한국어] 메모리 창이 소비한 창 번호 개수를 IO 창 번호 계산에 이어 쓰기 위한 변수. */
	int offset;
	/* [한국어] 각 단계 결과. */
	int err;
	/* [한국어] 창 번호 순회 인덱스. */
	int reg_no;

	/* [한국어] config 접근을 Type 0 로 초기 설정한다. 실제 접근 때마다 다시 바뀌지만,
	 * 알려진 상태에서 출발하기 위한 초기화다. */
	rockchip_pcie_cfg_configuration_accesses(rockchip,
						 AXI_WRAPPER_TYPE0_CFG);
	/* [한국어] 브리지 윈도 목록에서 첫 메모리 자원을 찾는다. */
	entry = resource_list_first_type(&bridge->windows, IORESOURCE_MEM);
	/* [한국어] 메모리 창이 없으면 아무것도 매핑할 수 없다. */
	if (!entry)
		/* [한국어] -ENODEV. */
		return -ENODEV;

	/* [한국어] 창 크기. */
	size = resource_size(entry->res);
	/* [한국어] CPU 주소에서 offset 을 빼 PCI 버스 주소를 얻는다. */
	pci_addr = entry->res->start - entry->offset;
	/* [한국어] PME 메시지 창의 기준 주소로 이 값을 기억해 둔다. 아래에서 실제 창 위치만큼
	 * 더해 최종 주소가 된다. */
	rockchip->msg_bus_addr = pci_addr;

	/* [한국어] 창 하나가 1MB(20비트)이므로 크기를 20비트 시프트한 만큼 창이 필요하다. */
	for (reg_no = 0; reg_no < (size >> 20); reg_no++) {
		/* [한국어] 창 번호 1 부터 쓴다 — 0번은 config 접근용으로 예약되어 있기 때문이다. */
		err = rockchip_pcie_prog_ob_atu(rockchip, reg_no + 1,
						/* [한국어] 메모리 쓰기 종류. */
						AXI_WRAPPER_MEM_WRITE,
						/* [한국어] 통과 비트 수 19 = 창 크기 2^20 = 1MB. */
						20 - 1,
						/* [한국어] 이 창이 대응할 PCI 주소는 기준에서 창 번호만큼 1MB 씩 떨어진 곳이다. */
						pci_addr + (reg_no << 20),
						0);
		/* [한국어] 창 설정 실패. */
		if (err) {
			/* [한국어] 실패 로그. */
			dev_err(dev, "program RC mem outbound ATU failed\n");
			/* [한국어] 오류 전달. */
			return err;
		}
	}

	/* [한국어] 인바운드 창 2번에 32비트 전체를 통과시키는 설정을 건다 —
	 * 엔드포인트가 DMA 로 시스템 메모리 하위 4GB 전체에 닿을 수 있게 여는 것이다.
	 * 이것이 없으면 어떤 DMA 도 동작하지 않는다. */
	err = rockchip_pcie_prog_ib_atu(rockchip, 2, 32 - 1, 0x0, 0);
	/* [한국어] 실패 검사. */
	if (err) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "program RC mem inbound ATU failed\n");
		/* [한국어] 오류 전달. */
		return err;
	}

	/* [한국어] 이번에는 I/O 자원을 찾는다. */
	entry = resource_list_first_type(&bridge->windows, IORESOURCE_IO);
	/* [한국어] 없으면, */
	if (!entry)
		/* [한국어] -ENODEV. */
		return -ENODEV;

	/* store the register number offset to program RC io outbound ATU */
	/* [한국어] 메모리 창이 소비한 창 개수를 기억해 둔다(옆의 상류 주석).
	 * 이 시점의 size 는 아직 메모리 창 크기이므로 그 값으로 계산한다 —
	 * 다음 줄에서 size 가 덮어써지기 때문에 순서가 중요하다. */
	offset = size >> 20;

	/* [한국어] 이제 I/O 창 크기로 갱신한다. */
	size = resource_size(entry->res);
	/* [한국어] I/O 창의 PCI 주소. */
	pci_addr = entry->res->start - entry->offset;

	/* [한국어] 같은 방식으로 1MB 단위 창을 채운다. */
	for (reg_no = 0; reg_no < (size >> 20); reg_no++) {
		/* [한국어] 창 번호는 메모리 창 다음부터 이어진다. */
		err = rockchip_pcie_prog_ob_atu(rockchip,
						reg_no + 1 + offset,
						/* [한국어] I/O 쓰기 종류. */
						AXI_WRAPPER_IO_WRITE,
						/* [한국어] 역시 1MB 창. */
						20 - 1,
						/* [한국어] 기준에서 1MB 씩 떨어진 PCI 주소. */
						pci_addr + (reg_no << 20),
						0);
		/* [한국어] 실패 검사. */
		if (err) {
			/* [한국어] 실패 로그. */
			dev_err(dev, "program RC io outbound ATU failed\n");
			/* [한국어] 오류 전달. */
			return err;
		}
	}

	/* assign message regions */
	/* [한국어] 마지막 창 하나를 메시지 전용으로 배정한다(옆의 상류 주석).
	 * [상류 코드 관찰] 이 호출만 반환값을 검사하지 않는다. 아래 return err 이
	 * 돌려주는 것은 이 호출의 결과가 아니라 직전 I/O 루프의 마지막 결과다. */
	rockchip_pcie_prog_ob_atu(rockchip, reg_no + 1 + offset,
				  AXI_WRAPPER_NOR_MSG,
				  20 - 1, 0, 0);

	/* [한국어] 메시지 창의 실제 위치만큼 기준 주소에 더해 최종 버스 주소를 완성한다.
	 * 이 값이 나중에 devm_ioremap 되어 msg_region 이 된다. */
	rockchip->msg_bus_addr += ((reg_no + offset) << 20);
	/* [한국어] [상류 코드 관찰] err 은 위 I/O 루프에서 마지막으로 대입된 값이다.
	 * 루프가 한 번도 돌지 않았다면(I/O 창 크기가 1MB 미만) 그보다 앞선
	 * 인바운드 설정의 결과가 남아 있으며, 어느 경우든 0 이다. */
	return err;
}

/* [한국어]
 * rockchip_pcie_wait_l2 - PME_TURN_OFF 를 보내고 링크가 L2 로 떨어지기를 기다린다
 *
 * @rockchip: 컨트롤러 객체. msg_region 매핑이 준비되어 있어야 한다.
 * @return: 0 = L2 진입 성공, 음수 = 5초 시간 초과.
 *
 * 시스템 서스펜드 전에 링크를 안전하게 내리는 절차다. PCIe 규격상 호스트가
 * PME_TURN_OFF 메시지를 보내면 엔드포인트가 PME_TO_Ack 로 답하고 링크가
 * L2/L3 Ready 로 전이한다. 그 절차를 밟지 않고 전원을 내리면 엔드포인트가
 * 비정상 상태로 남을 수 있다.
 *
 * 메시지를 보내는 방법이 특이하다. cfg_atu() 가 마지막 아웃바운드 창을 메시지
 * 전용(NOR_MSG)으로 배정해 두었고 probe 가 그것을 ioremap 해 두었으므로,
 * 그 창에 대한 MMIO 쓰기 한 번이 링크 너머로 PME_TURN_OFF TLP 를 내보낸다.
 * 쓰는 값(0x0)은 의미가 없고 쓰기 행위 자체가 트리거다.
 *
 * 그다음 LTSSM 상태를 5초까지 20us 간격으로 폴링한다. 엔드포인트가 응답해
 * 링크를 내리기까지 걸리는 시간이 장치마다 크게 달라 넉넉히 잡았다.
 *
 * 실행 컨텍스트: NOIRQ 서스펜드 경로, 프로세스 컨텍스트. 최대 5초 잠들 수 있다.
 *
 * 에러 경로: 시간 초과만 오류다. 호출자가 그것을 받아 인터럽트를 되살리고
 * 서스펜드를 취소한다.
 *
 * 호출 체인:
 *   rockchip_pcie_suspend_noirq() → [이 함수]
 *     → writel(msg_region + PCIE_RC_SEND_PME_OFF) → readl_poll_timeout()
 */
static int rockchip_pcie_wait_l2(struct rockchip_pcie *rockchip)
{
	/* [한국어] 폴링에 쓸 레지스터 값. */
	u32 value;
	/* [한국어] 폴링 결과. */
	int err;

	/* send PME_TURN_OFF message */
	/* [한국어] PME_TURN_OFF 메시지를 보낸다(옆의 상류 주석). 값이 아니라 쓰기 행위 자체가
	 * 트리거이므로 0 을 쓴다. cfg_atu() 가 마지막 창을 메시지 전용으로 배정하고
	 * probe 가 그것을 ioremap 해 둔 덕분에, 이 한 줄의 MMIO 쓰기가 링크 너머로
	 * PME_TURN_OFF TLP 를 내보낸다. */
	writel(0x0, rockchip->msg_region + PCIE_RC_SEND_PME_OFF);

	/* read LTSSM and wait for falling into L2 link state */
	/* [한국어] LTSSM 이 L2 상태로 떨어질 때까지 20us 간격으로 폴링한다(옆의 상류 주석). */
	err = readl_poll_timeout(rockchip->apb_base + PCIE_CLIENT_DEBUG_OUT_0,
				 value, PCIE_LINK_IS_L2(value), 20,
				 /* [한국어] 5초를 마이크로초로 바꿔 상한으로 삼는다. 엔드포인트가 PME_TURN_OFF 에
				  * 응답해 링크를 내리기까지 걸리는 시간이 장치마다 크게 달라 넉넉히 잡았다. */
				 jiffies_to_usecs(5 * HZ));
	/* [한국어] 시간 초과. */
	if (err) {
		/* [한국어] L2 진입 실패를 알린다. */
		dev_err(rockchip->dev, "PCIe link enter L2 timeout!\n");
		/* [한국어] 오류 전달 — 호출자가 인터럽트를 다시 켜고 서스펜드를 포기한다. */
		return err;
	}

	/* [한국어] L2 진입 성공. */
	return 0;
}

/* [한국어]
 * rockchip_pcie_suspend_noirq - 링크를 L2 로 내리고 PHY·클럭·0.9V 를 끈다
 *
 * @dev: 컨트롤러의 struct device. drvdata 에서 rockchip 을 꺼낸다.
 * @return: 0 = 서스펜드 준비 완료, 음수 = L2 진입 실패.
 *
 * NOIRQ 단계에서 불리는 이유: 이 시점에는 인터럽트가 이미 꺼져 있어, PME 처리와
 * 링크 상태 전이가 다른 인터럽트에 방해받지 않는다.
 *
 * 동작 과정:
 *   1) 클라이언트와 코어 인터럽트를 마스크한다. 위 영어 주석이 그 이유를 밝힌다 —
 *      PME_ACK 를 확인할 필요가 없으므로 아예 받지 않는다.
 *   2) PME_TURN_OFF 를 보내고 L2 진입을 기다린다.
 *   3) 실패하면 방금 끈 인터럽트를 되살리고 오류를 전달해 PM 코어가 서스펜드를
 *      취소하게 한다 — 되감기가 그 한 줄이다.
 *   4) 성공하면 PHY 를 내리고, 클럭을 끄고, 0.9V 만 끈다.
 *
 * 전원을 하나만 끄는 것이 특징이다. 12V/3.3V/1.8V 는 켜 둔 채로 서스펜드하는데,
 * resume 이 0.9V 만 다시 켜는 것과 정확히 대칭이다. 나머지를 유지하는 이유는
 * 이 트리의 코드만으로는 확인할 수 없다.
 *
 * 실행 컨텍스트: 시스템 서스펜드의 NOIRQ 단계, 프로세스 컨텍스트.
 * wait_l2() 때문에 최대 5초 걸릴 수 있다.
 *
 * 에러 경로: L2 실패만 오류이고, 그 경우 인터럽트를 되살린 뒤 반환한다.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.suspend_noirq == [이 함수]
 *     → rockchip_pcie_wait_l2() → deinit_phys() → disable_clocks()
 *     → regulator_disable(vpcie0v9)
 */
static int rockchip_pcie_suspend_noirq(struct device *dev)
{
	/* [한국어] drvdata 에 심어 둔 컨트롤러를 꺼낸다. */
	struct rockchip_pcie *rockchip = dev_get_drvdata(dev);
	/* [한국어] 각 단계 결과. */
	int ret;

	/* disable core and cli int since we don't need to ack PME_ACK */
	/* [한국어] 클라이언트 인터럽트를 다시 마스크한다(옆의 상류 주석 — PME_ACK 를 확인할
	 * 필요가 없으므로 끈다). enable 쪽과 달리 값 자리에도 비트를 세워 1(차단)로 만든다. */
	rockchip_pcie_write(rockchip, (PCIE_CLIENT_INT_CLI << 16) |
			    PCIE_CLIENT_INT_CLI, PCIE_CLIENT_INT_MASK);
	/* [한국어] 코어 인터럽트도 마스크한다. 이쪽은 반전 없이 그대로 쓴다. */
	rockchip_pcie_write(rockchip, (u32)PCIE_CORE_INT, PCIE_CORE_INT_MASK);

	/* [한국어] PME_TURN_OFF 를 보내고 L2 진입을 기다린다. */
	ret = rockchip_pcie_wait_l2(rockchip);
	/* [한국어] 실패했다면 서스펜드를 진행할 수 없다. */
	if (ret) {
		/* [한국어] 방금 끈 인터럽트를 되살려 원래 상태로 돌린다 — 되감기가 이 한 줄이다. */
		rockchip_pcie_enable_interrupts(rockchip);
		/* [한국어] 오류를 전달해 PM 코어가 서스펜드를 취소하게 한다. */
		return ret;
	}

	/* [한국어] PHY 를 내린다. 링크가 이미 L2 이므로 안전하다. */
	rockchip_pcie_deinit_phys(rockchip);

	/* [한국어] 클럭을 끈다. */
	rockchip_pcie_disable_clocks(rockchip);

	/* [한국어] 0.9V 만 끈다. 나머지 세 전원은 켜 둔 채로 서스펜드하는데,
	 * resume 경로가 0.9V 만 다시 켜는 것과 대칭이다. */
	regulator_disable(rockchip->vpcie0v9);

	/* [한국어] 여기서 ret 은 반드시 0 이다(0 이 아니면 위에서 이미 반환했다). */
	return ret;
}

/* [한국어]
 * rockchip_pcie_resume_noirq - 0.9V·클럭을 되살리고 포트와 주소 창을 다시 초기화한다
 *
 * @dev: 컨트롤러의 struct device.
 * @return: 0 = 재개 완료, 음수 = 어느 단계의 실패.
 *
 * suspend 의 역순이되, 단순히 되돌리는 것이 아니라 하드웨어를 처음부터 다시
 * 초기화한다는 점이 중요하다. L2 에서 깨어난 링크는 훈련을 새로 해야 하고,
 * 리셋으로 레지스터가 초기화되므로 주소 변환 창도 다시 프로그래밍해야 한다.
 *
 * 동작 과정:
 *   1) 0.9V 를 다시 켠다(suspend 가 끈 유일한 전원).
 *   2) 클럭을 켠다.
 *   3) host_init_port() — 리셋, PHY, 링크 훈련을 전부 다시 한다.
 *   4) cfg_atu() — 아웃바운드·인바운드 창을 다시 설정한다.
 *   5) L1 재진입을 위해 Tx 크레딧 갱신 주기를 다시 설정하고(위 영어 주석),
 *      인터럽트를 되살린다.
 *
 * 실행 컨텍스트: 시스템 재개의 NOIRQ 단계, 프로세스 컨텍스트.
 * 링크 훈련 때문에 최대 1초 남짓 걸릴 수 있다.
 *
 * 에러 경로: 세 개의 라벨이 계단식으로 이어져 잡은 것만 되돌린다.
 * 첫 라벨 이름 err_err_deinit_port 에 err 이 두 번 들어간 것은 상류의 오타로
 * 보이나 동작에는 영향이 없다.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.resume_noirq == [이 함수]
 *     → regulator_enable(vpcie0v9) → enable_clocks()
 *     → host_init_port() → cfg_atu()
 *     → update_txcredit_mui() → enable_interrupts()
 */
static int rockchip_pcie_resume_noirq(struct device *dev)
{
	/* [한국어] 컨트롤러를 꺼낸다. */
	struct rockchip_pcie *rockchip = dev_get_drvdata(dev);
	/* [한국어] 각 단계 결과. */
	int err;

	/* [한국어] suspend 가 끈 0.9V 를 다시 켠다. */
	err = regulator_enable(rockchip->vpcie0v9);
	/* [한국어] 실패 검사. */
	if (err) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "fail to enable vpcie0v9 regulator\n");
		/* [한국어] 오류 전달. */
		return err;
	}

	/* [한국어] 클럭을 다시 켠다. */
	err = rockchip_pcie_enable_clocks(rockchip);
	/* [한국어] 실패하면, */
	if (err)
		/* [한국어] 0.9V 를 되돌리는 구간으로. */
		goto err_disable_0v9;

	/* [한국어] 포트를 처음부터 다시 초기화한다 — 리셋, PHY, 링크 훈련을 모두 다시 한다.
	 * L2 에서 깨어난 링크는 훈련을 새로 해야 하기 때문이다. */
	err = rockchip_pcie_host_init_port(rockchip);
	/* [한국어] 실패하면, */
	if (err)
		/* [한국어] 클럭까지 되돌리는 구간으로. */
		goto err_pcie_resume;

	/* [한국어] 주소 변환 창도 다시 설정한다. 리셋으로 레지스터가 초기화되었기 때문이다. */
	err = rockchip_pcie_cfg_atu(rockchip);
	/* [한국어] 실패하면, */
	if (err)
		/* [한국어] PHY 까지 되돌리는 구간으로. */
		goto err_err_deinit_port;

	/* Need this to enter L1 again */
	/* [한국어] L1 재진입을 위해 Tx 크레딧 갱신 주기를 다시 설정한다(옆의 상류 주석). */
	rockchip_pcie_update_txcredit_mui(rockchip);
	/* [한국어] 인터럽트를 다시 켠다. */
	rockchip_pcie_enable_interrupts(rockchip);

	/* [한국어] 재개 완료. */
	return 0;

/* [한국어] cfg_atu 실패 전용 라벨. 이름에 err 이 두 번 들어간 것은 상류의 오타로 보이나
 * 그대로 둔다. */
err_err_deinit_port:
	/* [한국어] PHY 를 내린다. */
	rockchip_pcie_deinit_phys(rockchip);
/* [한국어] host_init_port 실패가 도달하는 라벨. */
err_pcie_resume:
	/* [한국어] 클럭을 끈다. */
	rockchip_pcie_disable_clocks(rockchip);
/* [한국어] 클럭 활성화 실패가 도달하는 라벨. */
err_disable_0v9:
	/* [한국어] 0.9V 를 끈다. */
	regulator_disable(rockchip->vpcie0v9);
	/* [한국어] 기록해 둔 오류를 전달한다. */
	return err;
}

/* [한국어]
 * rockchip_pcie_probe - RK3399 PCIe 컨트롤러를 루트 컴플렉스로 올린다
 *
 * @pdev: DT 에서 "rockchip,rk3399-pcie" 로 매칭된 플랫폼 디바이스.
 * @return: 0 = 성공. -ENODEV = DT 노드 없음. -ENOMEM = 할당 또는 매핑 실패.
 *       그 밖의 음수 = 각 초기화 단계의 실패.
 *
 * 동작 과정은 열두 단계다.
 *   1) DT 노드를 확인하고, 브리지와 컨트롤러를 한 번에 할당한다.
 *      struct rockchip_pcie 가 브리지의 private 영역에 놓이기 때문에
 *      cfg_atu() 가 pci_host_bridge_from_priv() 로 역변환할 수 있다.
 *   2) drvdata 를 심고 dev 를 기록한 뒤 is_rc = true 로 모드를 정한다.
 *      이 한 줄이 host 와 ep 를 가르는 유일한 지점이며, 공용 코드가 이 값으로
 *      세 곳에서 분기한다.
 *   3) DT 파싱(공용 + 레귤레이터 넷) → 클럭 켜기 → 전원 넷 켜기.
 *   4) host_init_port() 로 리셋·PHY·링크 훈련을 수행한다.
 *   5) INTx IRQ 도메인을 만든다.
 *   6) cfg_atu() 로 주소 변환 창을 설정한다. 이 안에서 msg_bus_addr 이 정해진다.
 *   7) 그 주소를 1MB 매핑해 msg_region 으로 삼는다 — wait_l2() 가 PME_TURN_OFF 를
 *      보낼 통로다.
 *   8) 브리지에 sysdata 와 ops 를 걸고, 세 인터럽트를 등록한다.
 *   9) 인터럽트 마스크를 푼다. 핸들러 등록보다 뒤에 오는 순서가 중요하다 —
 *      반대면 핸들러 없는 인터럽트가 올라올 수 있다.
 *  10) pci_host_probe() 로 버스를 스캔하고 장치를 등록한다.
 *
 * 실행 컨텍스트: 드라이버 코어의 probe 경로, 프로세스 컨텍스트.
 * 링크 훈련과 전원 인가로 1초 남짓 걸릴 수 있다.
 *
 * 에러 경로: 네 개의 라벨(err_remove_irq_domain → err_deinit_port → err_vpcie
 * → err_set_vpcie)이 계단식으로 이어진다.
 * [상류 코드 관찰, 수정하지 않음] setup_irq() 가 건 INTx 체인 핸들러는 devm 이
 * 아닌데도 어느 라벨에서도 떼어 내지 않는다. IRQ 도메인만 제거되고 핸들러는
 * 남으므로, probe 실패 직후 그 IRQ 가 올라오면 사라진 도메인을 참조하게 된다.
 *
 * 호출 체인:
 *   DT 매칭 → platform_driver.probe == [이 함수]
 *     → devm_pci_alloc_host_bridge() → parse_host_dt() → enable_clocks()
 *     → set_vpcie() → host_init_port() → init_irq_domain() → cfg_atu()
 *     → devm_ioremap() → setup_irq() → enable_interrupts() → pci_host_probe()
 */
static int rockchip_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 이 컨트롤러 객체. */
	struct rockchip_pcie *rockchip;
	/* [한국어] 로그와 devm 의 기준 디바이스. */
	struct device *dev = &pdev->dev;
	/* [한국어] PCI 호스트 브리지. */
	struct pci_host_bridge *bridge;
	/* [한국어] 각 단계 결과. */
	int err;

	/* [한국어] DT 노드 없이는 아무 정보도 얻을 수 없다. */
	if (!dev->of_node)
		/* [한국어] -ENODEV. */
		return -ENODEV;

	/* [한국어] 브리지와 컨트롤러를 한 번에 할당한다. devm 이라 자동 해제된다. */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*rockchip));
	/* [한국어] 메모리 부족. */
	if (!bridge)
		/* [한국어] -ENOMEM. */
		return -ENOMEM;

	/* [한국어] 브리지 뒤에 붙은 private 영역이 곧 struct rockchip_pcie 다.
	 * 그래서 cfg_atu() 가 pci_host_bridge_from_priv() 로 역변환할 수 있다. */
	rockchip = pci_host_bridge_priv(bridge);

	/* [한국어] suspend/resume/remove 가 되찾을 수 있도록 심어 둔다. */
	platform_set_drvdata(pdev, rockchip);
	/* [한국어] 로그와 devm 의 기준 디바이스를 기록한다. */
	rockchip->dev = dev;
	/* [한국어] 이 인스턴스가 루트 컴플렉스임을 표시한다. 공용 코드(pcie-rockchip.c)가
	 * 이 값 하나로 세 곳에서 분기하며, ep 판은 이 대입을 하지 않아
	 * 0 초기화된 false 가 그대로 남는다. */
	rockchip->is_rc = true;

	/* [한국어] DT 에서 레지스터 창·클럭·리셋·GPIO(공용)와 레귤레이터 넷(호스트 전용)을 읽는다. */
	err = rockchip_pcie_parse_host_dt(rockchip);
	/* [한국어] 실패하면, */
	if (err)
		/* [한국어] 곧장 반환한다 — 아직 devm 이 아닌 자원을 잡은 것이 없다. */
		return err;

	/* [한국어] 클럭을 켠다. */
	err = rockchip_pcie_enable_clocks(rockchip);
	/* [한국어] 실패하면, */
	if (err)
		/* [한국어] 곧장 반환. 클럭은 실패 시 자체적으로 정리된다. */
		return err;

	/* [한국어] 네 전원을 순서대로 켠다. */
	err = rockchip_pcie_set_vpcie(rockchip);
	/* [한국어] 실패 검사. */
	if (err) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "failed to set vpcie regulator\n");
		/* [한국어] 클럭만 되돌리는 구간으로 — set_vpcie 는 자체 되감기를 이미 마쳤다. */
		goto err_set_vpcie;
	}

	/* [한국어] 리셋·PHY·링크 훈련을 수행한다. 이 함수가 돌아오면 링크가 올라와 있다. */
	err = rockchip_pcie_host_init_port(rockchip);
	/* [한국어] 실패하면, */
	if (err)
		/* [한국어] 전원부터 되돌리는 구간으로. */
		goto err_vpcie;

	/* [한국어] INTx IRQ 도메인을 만든다. */
	err = rockchip_pcie_init_irq_domain(rockchip);
	/* [한국어] 실패하면, */
	if (err < 0)
		/* [한국어] PHY 부터 되돌리는 구간으로. */
		goto err_deinit_port;

	/* [한국어] 아웃바운드·인바운드 주소 변환 창을 설정한다. 이 안에서 msg_bus_addr 도 정해진다. */
	err = rockchip_pcie_cfg_atu(rockchip);
	/* [한국어] 실패하면, */
	if (err)
		/* [한국어] IRQ 도메인부터 되돌리는 구간으로. */
		goto err_remove_irq_domain;

	/* [한국어] 방금 정해진 메시지 창 주소를 1MB 매핑한다. 이 매핑이 있어야
	 * wait_l2() 가 PME_TURN_OFF 를 보낼 수 있다. */
	rockchip->msg_region = devm_ioremap(dev, rockchip->msg_bus_addr, SZ_1M);
	/* [한국어] 매핑 실패. */
	if (!rockchip->msg_region) {
		/* [한국어] -ENOMEM 을 기록하고, */
		err = -ENOMEM;
		/* [한국어] 같은 되감기 구간으로. */
		goto err_remove_irq_domain;
	}

	/* [한국어] config 접근 콜백이 bus->sysdata 로 되찾을 컨트롤러를 심는다. */
	bridge->sysdata = rockchip;
	/* [한국어] 이 파일이 구현한 read/write 콜백 테이블을 건다. */
	bridge->ops = &rockchip_pcie_ops;

	/* [한국어] 세 인터럽트(sys / legacy 체인 / client)를 등록한다. */
	err = rockchip_pcie_setup_irq(rockchip);
	/* [한국어] 실패하면, */
	if (err)
		/* [한국어] 같은 되감기 구간으로. */
		goto err_remove_irq_domain;

	/* [한국어] 인터럽트 마스크를 풀어 실제로 받기 시작한다. 핸들러 등록보다 뒤에 오는
	 * 순서가 중요하다 — 반대면 핸들러 없는 인터럽트가 올라올 수 있다. */
	rockchip_pcie_enable_interrupts(rockchip);

	/* [한국어] PCI 코어에 브리지를 넘겨 버스를 스캔하고 장치를 등록한다. */
	err = pci_host_probe(bridge);
	/* [한국어] 실패하면, */
	if (err < 0)
		/* [한국어] 같은 되감기 구간으로. */
		goto err_remove_irq_domain;

	/* [한국어] probe 성공. */
	return 0;

/* [한국어] IRQ 도메인 생성 이후의 실패가 모이는 라벨. */
err_remove_irq_domain:
	/* [한국어] 도메인을 제거한다.
	 * [상류 코드 관찰] setup_irq() 가 건 체인 핸들러(devm 이 아니다)는 여기서
	 * 떼어 내지 않는다. 그 IRQ 가 남아 있는 상태에서 도메인이 사라지므로,
	 * probe 실패 직후 인터럽트가 올라오면 문제가 될 수 있다. */
	irq_domain_remove(rockchip->irq_domain);
/* [한국어] host_init_port 이후의 실패가 도달하는 라벨. */
err_deinit_port:
	/* [한국어] PHY 를 내린다. */
	rockchip_pcie_deinit_phys(rockchip);
/* [한국어] set_vpcie 이후의 실패가 도달하는 라벨. */
err_vpcie:
	/* [한국어] 12V 가 있는 보드에서만, */
	if (!IS_ERR(rockchip->vpcie12v))
		/* [한국어] 끈다. */
		regulator_disable(rockchip->vpcie12v);
	/* [한국어] 3.3V 가 있는 보드에서만, */
	if (!IS_ERR(rockchip->vpcie3v3))
		/* [한국어] 끈다. */
		regulator_disable(rockchip->vpcie3v3);
	/* [한국어] 1.8V 는 항상 있으므로 조건 없이 끈다. */
	regulator_disable(rockchip->vpcie1v8);
	/* [한국어] 0.9V 도 마찬가지. */
	regulator_disable(rockchip->vpcie0v9);
/* [한국어] 클럭 이후의 실패가 도달하는 라벨. */
err_set_vpcie:
	/* [한국어] 클럭을 끈다. */
	rockchip_pcie_disable_clocks(rockchip);
	/* [한국어] 기록해 둔 오류를 전달한다. */
	return err;
}

/* [한국어]
 * rockchip_pcie_remove - 버스를 제거하고 PHY·클럭·전원을 모두 내린다
 *
 * @pdev: 제거되는 플랫폼 디바이스.
 *
 * probe 의 역순으로 정리한다.
 *   1) pci_stop_root_bus() 로 버스의 장치들을 정지시키고(드라이버를 떼고),
 *      pci_remove_root_bus() 로 버스 객체를 없앤다. 두 단계로 나뉜 것은
 *      정지와 해제의 의미가 다르기 때문이다.
 *   2) IRQ 도메인을 제거한다.
 *   3) PHY 를 내리고 클럭을 끈다.
 *   4) 전원 넷을 끈다. 12V 와 3.3V 는 존재 확인 후에만 끈다.
 *
 * 같은 계열의 다른 드라이버들(pcie-artpec6.c, pci-meson.c)이 remove 를 아예
 * 두지 않는 것과 달리, 이 드라이버는 버스 제거까지 제대로 수행한다.
 *
 * [상류 코드 관찰, 수정하지 않음] probe 실패 경로와 마찬가지로 setup_irq() 가
 * 건 INTx 체인 핸들러를 떼어 내지 않는다. 또 전원 차단 순서가 인가 순서의
 * 역순이 아니라 같은 순서인데, 전원 차단에는 순서 의존성이 없다는 판단으로 보인다.
 *
 * 실행 컨텍스트: 드라이버 언바인드 경로, 프로세스 컨텍스트.
 * 하위 장치들의 remove 콜백이 연쇄로 불리므로 잠들 수 있다.
 *
 * 에러 경로: 없다. 반환값이 void 다.
 *
 * 호출 체인:
 *   드라이버 코어의 언바인드 → platform_driver.remove == [이 함수]
 *     → pci_stop_root_bus() → pci_remove_root_bus() → irq_domain_remove()
 *     → deinit_phys() → disable_clocks() → regulator_disable() ×4
 */
static void rockchip_pcie_remove(struct platform_device *pdev)
{
	/* [한국어] 로그 대상. */
	struct device *dev = &pdev->dev;
	/* [한국어] probe 가 심어 둔 컨트롤러. */
	struct rockchip_pcie *rockchip = dev_get_drvdata(dev);
	/* [한국어] private 포인터에서 브리지로 되돌아간다 — probe 에서 둘을 함께 할당했기에 성립한다. */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(rockchip);

	/* [한국어] 먼저 버스의 장치들을 정지시킨다. */
	pci_stop_root_bus(bridge->bus);
	/* [한국어] 그다음 버스 자체를 제거한다. 두 단계로 나뉘어 있는 것은 정지와 해제의
	 * 의미가 달라서다 — 정지는 드라이버를 떼고, 제거는 객체를 없앤다. */
	pci_remove_root_bus(bridge->bus);
	/* [한국어] IRQ 도메인을 제거한다.
	 * [상류 코드 관찰] probe 실패 경로와 마찬가지로 체인 핸들러를 떼어 내지 않는다. */
	irq_domain_remove(rockchip->irq_domain);

	/* [한국어] PHY 를 내린다. */
	rockchip_pcie_deinit_phys(rockchip);

	/* [한국어] 클럭을 끈다. */
	rockchip_pcie_disable_clocks(rockchip);

	/* [한국어] 12V 가 있으면, */
	if (!IS_ERR(rockchip->vpcie12v))
		/* [한국어] 끈다. */
		regulator_disable(rockchip->vpcie12v);
	/* [한국어] 3.3V 가 있으면, */
	if (!IS_ERR(rockchip->vpcie3v3))
		/* [한국어] 끈다. */
		regulator_disable(rockchip->vpcie3v3);
	/* [한국어] 1.8V 를 끈다. */
	regulator_disable(rockchip->vpcie1v8);
	/* [한국어] 0.9V 를 끈다. 순서가 probe 의 인가 순서와 역순이 아니라 같은 순서인데,
	 * 전원 차단은 순서 의존성이 없다는 판단으로 보인다. */
	regulator_disable(rockchip->vpcie0v9);
}

static const struct dev_pm_ops rockchip_pcie_pm_ops = {
	/* [한국어] NOIRQ 단계의 서스펜드/재개만 등록한다. 인터럽트가 이미 꺼진 뒤에 불리므로
	 * PME 처리와 링크 상태 전이를 방해받지 않고 진행할 수 있다. */
	NOIRQ_SYSTEM_SLEEP_PM_OPS(rockchip_pcie_suspend_noirq,
				  rockchip_pcie_resume_noirq)
};

static const struct of_device_id rockchip_pcie_of_match[] = {
	/* [한국어] RK3399 SoC 의 PCIe 컨트롤러. */
	{ .compatible = "rockchip,rk3399-pcie", },
	/* [한국어] 테이블 끝을 알리는 빈 항목. */
	{}
};
/* [한국어] 모듈 자동 로딩을 위해 매칭 테이블을 내보낸다. */
MODULE_DEVICE_TABLE(of, rockchip_pcie_of_match);

static struct platform_driver rockchip_pcie_driver = {
	.driver = {
		/* [한국어] 드라이버 이름. */
		.name = "rockchip-pcie",
		/* [한국어] 위에서 정의한 DT 매칭 테이블. */
		.of_match_table = rockchip_pcie_of_match,
		/* [한국어] 서스펜드/재개 콜백 테이블. */
		.pm = &rockchip_pcie_pm_ops,
	},
	/* [한국어] 장치가 나타났을 때 불릴 진입점. */
	.probe = rockchip_pcie_probe,
	/* [한국어] 장치가 사라질 때 불릴 정리 함수. artpec6 나 meson 과 달리 이 드라이버는
	 * remove 를 제대로 구현해 버스 제거까지 수행한다. */
	.remove = rockchip_pcie_remove,
};
/* [한국어] module_init/module_exit 보일러플레이트. builtin 이 아니라 모듈로 빌드될 수 있다. */
module_platform_driver(rockchip_pcie_driver);

/* [한국어] modinfo 에 표시될 작성자. */
MODULE_AUTHOR("Rockchip Inc");
/* [한국어] modinfo 에 표시될 설명. */
MODULE_DESCRIPTION("Rockchip AXI PCIe driver");
/* [한국어] 라이선스 선언. */
MODULE_LICENSE("GPL v2");
