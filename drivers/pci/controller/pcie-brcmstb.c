// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2009 - 2019 Broadcom */

/*
 * [한국어 설명] Broadcom STB/BCM2711/BCM7xxx 계열 PCIe 호스트 컨트롤러 (pcie-brcmstb.c)
 *
 * === 파일의 역할 ===
 * Broadcom 의 셋톱박스(STB) 계열과 라즈베리파이 4/5 에 쓰이는 SoC 들의 PCIe
 * 컨트롤러를 루트 컴플렉스로 모는 플랫폼 드라이버다. 한 파일이 일곱 가지
 * SoC 변종(enum pcie_soc_base 의 GENERIC/BCM2711/BCM4908/BCM7278/BCM7425/
 * BCM7435/BCM7712)을 모두 다루며, 그 차이를 흡수하는 방식이 이 파일의 뼈대다.
 *
 * 변종 흡수 장치가 셋이다.
 *   1) struct pcie_cfg_data - compatible 마다 하나씩 있는 상수 표.
 *      레지스터 오프셋 배열, SoC 종류, PHY 유무, quirk 비트, 안쪽 창 개수,
 *      그리고 함수 포인터 셋(perst_set / bridge_sw_init_set / post_setup)을
 *      담는다. 변종별로 다른 동작이 전부 이 표를 통해 갈린다.
 *   2) offsets[] 배열 - 위 enum(RGR1_SW_INIT_1, EXT_CFG_INDEX, EXT_CFG_DATA,
 *      PCIE_HARD_DEBUG, PCIE_INTR2_CPU_BASE) 을 인덱스로 쓰는 오프셋 표.
 *      같은 이름의 레지스터가 변종마다 다른 자리에 있기 때문이다.
 *   3) is_bmips() 같은 판정 헬퍼 - BMIPS 계열(BCM7425/7435)은 SerDes 비트
 *      자리, 버스트 크기, 바깥 창 처리가 달라 그 세 곳에서 갈라진다.
 *      (BCM7425 의 config 접근이 다른 것은 이 헬퍼가 아니라 전용 pci_ops 로,
 *      MSI 의 legacy 배치는 hw_rev 비교로 갈린다.)
 *
 * 이 드라이버가 다른 호스트 드라이버보다 크게 다루는 것이 셋 있다.
 *   - 안쪽(inbound) 주소 창: DRAM 이 여러 memc 로 나뉘어 있어 창을 여러 개
 *     열어야 하고, 크기를 로그 인코딩으로 넣어야 한다.
 *   - CLKREQ# 신호 처리: L1 절전 하위 상태(L1SS)를 쓸지, CLKREQ 를 아예
 *     끌지를 보드와 장치 능력을 보고 런타임에 정한다(brcm_config_clkreq).
 *   - 오류 보고: 컨트롤러가 바깥으로 나간 요청의 오류를 자체 레지스터에
 *     기록해 두고, die/panic 알림 콜백에서 그것을 덤프한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계층은 PCI 코어 -> pci_host_bridge -> 이 파일 -> 레지스터 MMIO / PHY /
 * 레귤레이터 -> SoC 하드웨어 순이다.
 *
 *   platform_driver -> brcm_pcie_probe()
 *     -> devm_pci_alloc_host_bridge()   브리지와 private 를 함께 할당
 *     -> cfg 표를 compatible 에서 얻어 변종을 확정
 *     -> 클럭/리셋/레귤레이터를 잡고 PHY 를 켠다
 *     -> brcm_pcie_setup()              레지스터 초기화와 창 설정의 본체
 *     -> brcm_pcie_start_link()         PERST 를 풀고 링크를 기다린다
 *     -> brcm_pcie_enable_msi()         내장 MSI 컨트롤러를 세운다(선택)
 *     -> pci_host_probe()               PCI 코어에 열거를 넘긴다
 *
 * 열거 중 되불리는 것이 셋이다.
 *   config 접근  -> brcm_pcie_map_bus() 또는 brcm7425_pcie_map_bus()
 *                   (BMIPS 계열은 접근 방식이 달라 별도 함수를 쓴다)
 *   버스 추가    -> brcm_pcie_add_bus() / brcm_pcie_remove_bus()
 *                   여기서 자식 장치용 레귤레이터를 켜고 끄며,
 *                   CLKREQ 구성도 이 시점에 다시 정한다.
 *
 * 인터럽트: 내장 MSI 컨트롤러가 있으면 brcm_pcie_msi_isr() 이 체인 핸들러로
 * 상태 레지스터를 훑어 각 벡터를 도메인에 넘긴다. INTx 는 이 파일이 다루지
 * 않는다.
 *
 * 절전: brcm_pcie_suspend_noirq() 가 L23 진입 -> PHY 정지 -> 리셋 -> 클럭
 * 순으로 내리고, resume 이 그 역순으로 되살린 뒤 필요하면 링크를 다시 세운다.
 *
 * 실행 컨텍스트: probe/PM/config 접근은 프로세스 컨텍스트다. MSI 체인 핸들러와
 * irq_chip 콜백은 인터럽트 컨텍스트에서 돌고, die/panic 알림 콜백은 커널이
 * 죽는 도중 불리므로 잠들 수 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/probe.c 의 pci_host_probe(), drivers/pci/access.c 의
 *   config 접근 경로, 그리고 ../pci.h 의 코어 내부 선언.
 * 아래쪽: 클럭, 리셋 컨트롤러(rescal / perst / bridge / swinit 넷을 따로
 *   잡는다), 레귤레이터(자식 장치 전원), MSI 코어(msi_create_parent_irq_domain,
 *   msi_lib_init_dev_msi_info), DT 파서, 그리고 die/panic 알림 체인
 *   (linux/kdebug.h, linux/panic_notifier.h).
 * 공유 상태: struct brcm_pcie 가 컨트롤러 하나를, struct brcm_msi 가 내장 MSI
 *   컨트롤러를 담는다. 둘 다 인스턴스마다 독립적이며, 전역 가변 상태는 없다.
 *   bridge_lock 스핀락이 브리지 리셋 상태(bridge_in_reset)를 보호하는데,
 *   그 값을 인터럽트 문맥에서도 읽기 때문이다.
 *
 * === 주요 함수/구조체 요약 ===
 * brcm_pcie_probe()          : 진입점. 변종 확정부터 열거 시작까지 엮는다.
 * brcm_pcie_setup()          : 레지스터 초기화의 본체. 이 파일에서 가장 긴 함수다.
 * brcm_pcie_get_inbound_wins(): DRAM 배치를 읽어 안쪽 창 목록을 만든다.
 * set_inbound_win_registers(): 그 목록을 실제 레지스터에 써 넣는다.
 * brcm_pcie_set_outbound_win(): 바깥 창 하나를 연다.
 * brcm_config_clkreq()       : CLKREQ# 와 L1SS 를 보드·장치 능력에 맞춰 정한다.
 * brcm_pcie_start_link()     : PERST 를 풀고 링크가 서기를 기다린다.
 * brcm_pcie_enable_msi()     : 내장 MSI 컨트롤러와 irq_domain 을 세운다.
 * brcm_pcie_msi_isr()        : MSI 체인 핸들러.
 * brcm_pcie_dump_err()       : 컨트롤러가 기록해 둔 바깥 요청 오류를 덤프한다.
 * struct brcm_pcie           : 컨트롤러 하나의 모든 상태.
 * struct brcm_msi            : 내장 MSI 컨트롤러의 상태.
 * struct pcie_cfg_data       : 변종별 상수와 함수 포인터 표.
 * struct inbound_win         : 안쪽 창 하나의 크기·PCI 오프셋·CPU 주소.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 이 드라이버는 버스를 만드는 쪽이고 NVMe 는 그 위에 열거되는 장치라
 * 계층이 다르다. 다만 라즈베리파이 4/5 에 NVMe SSD 를 붙이는 구성이 흔하고,
 * 그 경우 이 파일의 안쪽 창 설정이 SSD 의 DMA 가 DRAM 에 닿는 통로가 되고
 * brcm_config_clkreq() 의 판단이 링크 절전 동작을 좌우한다. 그 경로에
 * NVMe 에 특화된 처리는 없고 모든 PCIe 장치에 똑같이 적용된다.
 */

#include <linux/bitfield.h> /* [한국어] FIELD_GET/FIELD_PREP/u32p_replace_bits — 레지스터 비트 필드 조작의 기본 도구 */
#include <linux/bitops.h> /* [한국어] BIT()/GENMASK()/HWEIGHT32() — 마스크 상수 정의와 마스크 폭 계산에 쓴다 */
#include <linux/clk.h> /* [한국어] clk_prepare_enable/clk_disable_unprepare — 컨트롤러 클록 "sw_pcie" 제어 */
#include <linux/compiler.h> /* [한국어] 컴파일러 속성/배리어 정의. 이 파일이 직접 쓰지는 않지만 다른 헤더가 기대한다 */
#include <linux/delay.h> /* [한국어] usleep_range/msleep/udelay — 리셋·SerDes·링크 대기에 쓰는 지연 */
#include <linux/init.h> /* [한국어] 초기화 섹션 매크로. 모듈 등록 경로가 기대하는 정의를 들여온다 */
#include <linux/interrupt.h> /* [한국어] 인터럽트 관련 기본 정의. MSI 체인 핸들러 등록 경로가 쓴다 */
#include <linux/io.h> /* [한국어] readl/writel/writel_relaxed — 컨트롤러 레지스터 접근의 핵심 */
#include <linux/iopoll.h> /* [한국어] readl_poll_timeout_atomic — MDIO 완료를 폴링으로 기다릴 때 쓴다 */
#include <linux/ioport.h> /* [한국어] struct resource 와 resource_size() — DT 창 자원을 다룬다 */
#include <linux/irqchip/chained_irq.h> /* [한국어] chained_irq_enter/exit — MSI 체인 핸들러가 상위 컨트롤러를 마스크할 때 */
#include <linux/irqchip/irq-msi-lib.h> /* [한국어] msi_lib_init_dev_msi_info 와 MSI_FLAG_ 계열 — 상위 MSI 계층에 붙는 규약 */
#include <linux/irqdomain.h> /* [한국어] irq_domain / irq_domain_set_info / msi_create_parent_irq_domain */
#include <linux/kdebug.h> /* [한국어] register_die_notifier/unregister_die_notifier — die 알림 체인 등록 */
#include <linux/kernel.h> /* [한국어] ARRAY_SIZE 등 공통 매크로 */
#include <linux/list.h> /* [한국어] 연결 리스트. resource_list_for_each_entry 순회의 바탕 */
#include <linux/log2.h> /* [한국어] ilog2/order_base_2/fls64 — 창 크기 인코딩과 MSI 벡터 정렬 계산 */
#include <linux/module.h> /* [한국어] MODULE_ 계열 매크로와 module_platform_driver() */
#include <linux/msi.h> /* [한국어] struct msi_msg 와 pci_msi_enabled() — MSI 주소/데이터 조립 */
#include <linux/notifier.h> /* [한국어] notifier_block 과 atomic_notifier_chain_register — panic 알림 등록 */
#include <linux/of_address.h> /* [한국어] DT 의 주소/크기 해석 도우미 */
#include <linux/of_irq.h> /* [한국어] irq_of_parse_and_map — DT 인터럽트 목록에서 MSI 선을 얻는다 */
#include <linux/of_pci.h> /* [한국어] of_pci_get_max_link_speed — DT 의 최대 링크 속도 속성을 읽는다 */
#include <linux/of_platform.h> /* [한국어] 플랫폼 디바이스와 DT 를 잇는 정의. devm_platform_ioremap_resource 경로 */
#include <linux/panic_notifier.h> /* [한국어] panic_notifier_list — 패닉 알림 체인의 헤드 */
#include <linux/pci.h> /* [한국어] PCI 코어 API 와 PCI_EXP_ 계열 스펙 상수 */
#include <linux/pci-ecam.h> /* [한국어] PCIE_ECAM_OFFSET/PCIE_ECAM_REG — config 접근 주소 계산 */
#include <linux/printk.h> /* [한국어] pr_err 등 로깅. probe 초기의 match data 실패에서 쓴다 */
#include <linux/regulator/consumer.h> /* [한국어] regulator_bulk_ 계열 — 하류 슬롯 전원 제어 */
#include <linux/reset.h> /* [한국어] reset_control_ 계열 — rescal/perst/bridge/swinit 리셋 조작 */
#include <linux/sizes.h> /* [한국어] SZ_1M/SZ_128M/SZ_2G/SZ_4G — 창 크기와 정렬 판정의 상수 */
#include <linux/slab.h> /* [한국어] devm_kzalloc — brcm_msi 와 subdev_regulators 할당 */
#include <linux/spinlock.h> /* [한국어] spinlock_t 와 spin_lock_irqsave — bridge_lock 의 정의 */
#include <linux/string.h> /* [한국어] strcmp — brcm,clkreq-mode 문자열 비교 */
#include <linux/string_choices.h> /* [한국어] str_read_write — 오류 보고에서 방향을 문자열로 바꾼다 */
#include <linux/types.h> /* [한국어] u32/u64/bool 등 기본 타입 */

#include "../pci.h" /* [한국어] PCI 서브시스템 내부 헤더. pcie_get_link_speed() 등 코어 비공개 도우미 */

/* BRCM_PCIE_CAP_REGS - Offset for the mandatory capability config regs */
#define BRCM_PCIE_CAP_REGS				0x00ac /* [한국어] RC 자신의 PCIe capability 레지스터가 시작하는 오프셋. start_link 가 여기에 PCI_EXP_LNKSTA/LNKCTL2 를 더해 표준 필드를 읽고 쓴다 */

/* Broadcom STB PCIe Register Offsets */
#define PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1				0x0188 /* [한국어] 벤더 전용 레지스터 1. 안쪽 창의 엔디언 모드가 들어 있다 */
#define  PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1_ENDIAN_MODE_BAR2_MASK	0xc /* [한국어] 그중 BAR2(주 안쪽 창)의 엔디언 모드 필드 */
#define  PCIE_RC_CFG_VENDOR_SPECIFIC_REG1_LITTLE_ENDIAN			0x0 /* [한국어] 그 필드에 넣을 리틀엔디언 값. setup 이 무조건 이 값으로 못박는다 */

#define PCIE_RC_CFG_PRIV1_ID_VAL3			0x043c /* [한국어] RC 의 config 공간에서 클래스 코드를 덮어쓰는 벤더 전용 레지스터 */
#define  PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_CODE_MASK	0xffffff /* [한국어] 그중 24비트 클래스 코드 필드. setup 이 0x060400(PCI-to-PCI 브리지)을 넣는다 */

#define PCIE_RC_CFG_PRIV1_LINK_CAPABILITY			0x04dc /* [한국어] RC 의 링크 능력 레지스터(벤더 경로). 광고할 능력을 바꿀 수 있다 */
#define  PCIE_RC_CFG_PRIV1_LINK_CAPABILITY_MAX_LINK_WIDTH_MASK	0x1f0 /* [한국어] 그중 최대 링크 폭 필드. DT 의 num-lanes 가 있으면 이 값을 덮는다 */

#define PCIE_RC_CFG_PRIV1_ROOT_CAP			0x4f8 /* [한국어] RC 의 루트 능력 레지스터. L1SS 광고 여부가 여기 있다 */
#define  PCIE_RC_CFG_PRIV1_ROOT_CAP_L1SS_MODE_MASK	0xf8 /* [한국어] 그중 L1SS 모드 필드. clkreq no-l1ss 모드에서 2 를 넣어 L1SS 를 광고하지 않게 한다 */

#define PCIE_RC_DL_MDIO_ADDR				0x1100 /* [한국어] MDIO 주소 레지스터. 포트/레지스터/명령을 조립한 패킷을 여기에 쓴다 */
#define PCIE_RC_DL_MDIO_WR_DATA				0x1104 /* [한국어] MDIO 쓰기 데이터 레지스터. 완료 비트도 이 레지스터에서 폴링한다 */
#define PCIE_RC_DL_MDIO_RD_DATA				0x1108 /* [한국어] MDIO 읽기 데이터 레지스터. 완료 비트와 데이터가 함께 온다 */

#define PCIE_RC_PL_REG_PHY_CTL_1			0x1804 /* [한국어] PHY 제어 레지스터 1 */
#define  PCIE_RC_PL_REG_PHY_CTL_1_REG_P2_POWERDOWN_ENA_NOSYNC_MASK	0x8 /* [한국어] 그중 P2 파워다운 활성 비트. num-lanes 를 덮어쓸 때 함께 켠다 */

#define PCIE_RC_PL_PHY_CTL_15				0x184c /* [한국어] PHY 제어 레지스터 15. L1SS 타이머의 기준 클록 주기가 여기 있다 */
#define  PCIE_RC_PL_PHY_CTL_15_DIS_PLL_PD_MASK		0x400000 /* [한국어] PLL 파워다운 금지 비트. [관찰] 이 트리에서는 정의만 있고 쓰이는 곳이 없다 */
#define  PCIE_RC_PL_PHY_CTL_15_PM_CLK_PERIOD_MASK	0xff /* [한국어] PM 클록 주기 필드. bcm2712 후처리가 18(=0x12)을 넣는다 — 54MHz 의 역수 18.52ns 내림 */

#define PCIE_MISC_MISC_CTRL				0x4008 /* [한국어] 컨트롤러 잡다 제어 레지스터. 버스트 크기, SCB 접근, memc 크기 등이 모여 있다 */
#define  PCIE_MISC_MISC_CTRL_PCIE_RCB_64B_MODE_MASK	0x80 /* [한국어] RCB 64바이트 모드. setup 이 켠다 */
#define  PCIE_MISC_MISC_CTRL_PCIE_RCB_MPS_MODE_MASK	0x400 /* [한국어] RCB 를 MPS 에 맞추는 모드. setup 이 켠다 */
#define  PCIE_MISC_MISC_CTRL_SCB_ACCESS_EN_MASK		0x1000 /* [한국어] SCB(내부 버스) 접근 허용. setup 이 켠다 */
#define  PCIE_MISC_MISC_CTRL_CFG_READ_UR_MODE_MASK	0x2000 /* [한국어] 실패한 config 읽기를 Unsupported Request 로 처리하는 모드. setup 이 켠다 */
#define  PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_MASK	0x300000 /* [한국어] SCB 최대 버스트 크기 필드. 인코딩이 변종마다 달라 setup 이 칩별로 값을 고른다 */

#define  PCIE_MISC_MISC_CTRL_SCB0_SIZE_MASK		0xf8000000 /* [한국어] memc 0 의 크기 필드 */
#define  PCIE_MISC_MISC_CTRL_SCB1_SIZE_MASK		0x07c00000 /* [한국어] memc 1 의 크기 필드 */
#define  PCIE_MISC_MISC_CTRL_SCB2_SIZE_MASK		0x0000001f /* [한국어] memc 2 의 크기 필드 */
#define  SCB_SIZE_MASK(x) PCIE_MISC_MISC_CTRL_SCB ## x ## _SIZE_MASK /* [한국어] 위 셋을 인덱스로 고르기 위한 토큰 붙이기 매크로. setup 의 memc 루프가 SCB_SIZE_MASK(0/1/2) 로 쓴다 */

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO		0x400c /* [한국어] 바깥 창 0 의 PCI 주소 하위 워드 */
/* [한국어] 바깥 창 win 의 PCI 주소 하위 워드 오프셋. 창 하나가 LO/HI 두
 * 워드를 쓰므로 8 바이트 간격이다. brcm_pcie_set_outbound_win() 이 쓴다. */
#define PCIE_MEM_WIN0_LO(win)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO + ((win) * 8)

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI		0x4010 /* [한국어] 바깥 창 0 의 PCI 주소 상위 워드 */
/* [한국어] 바깥 창 win 의 PCI 주소 상위 워드 오프셋. 위와 같은 8 바이트 간격. */
#define PCIE_MEM_WIN0_HI(win)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI + ((win) * 8)

/*
 * NOTE: You may see the term "BAR" in a number of register names used by
 *   this driver.  The term is an artifact of when the HW core was an
 *   endpoint device (EP).  Now it is a root complex (RC) and anywhere a
 *   register has the term "BAR" it is related to an inbound window.
 */

#define PCIE_BRCM_MAX_INBOUND_WINS			16 /* [한국어] inbound_win 배열의 최대 크기. cfg->num_inbound_wins 의 상한이기도 하다 */
#define PCIE_MISC_RC_BAR1_CONFIG_LO			0x402c /* [한국어] 안쪽 창 1 의 설정 레지스터 하위 워드. 창마다 8바이트 간격이라 brcm_bar_reg_offset() 이 여기서 센다 */
#define  PCIE_MISC_RC_BAR1_CONFIG_LO_SIZE_MASK		0x1f /* [한국어] 그 워드 안의 크기 필드. brcm_pcie_encode_ibar_size() 가 만든 비선형 값이 들어간다 */

#define PCIE_MISC_RC_BAR4_CONFIG_LO			0x40d4 /* [한국어] 안쪽 창 4 의 설정 레지스터. 1~3 과 자리가 끊겨 있어 4번부터 다시 기준을 잡는다 */


#define PCIE_MISC_MSI_BAR_CONFIG_LO			0x4044 /* [한국어] MSI 수신 주소 하위 워드. brcm_msi_set_regs() 가 여기에 목표 주소와 활성 비트를 쓴다 */
#define PCIE_MISC_MSI_BAR_CONFIG_HI			0x4048 /* [한국어] MSI 수신 주소 상위 워드 */

#define PCIE_MISC_MSI_DATA_CONFIG			0x404c /* [한국어] MSI 데이터 매칭 레지스터. 장치가 보낸 데이터가 이 값과 맞아야 MSI 로 인정한다 */
#define  PCIE_MISC_MSI_DATA_CONFIG_VAL_32		0xffe06540 /* [한국어] 32비트 MSI 판 매칭 값. compose_msi_msg 가 이 값의 하위 16비트를 장치에 알려 준다 */
#define  PCIE_MISC_MSI_DATA_CONFIG_VAL_8		0xfff86540 /* [한국어] legacy(8벡터) 판 매칭 값. set_regs 가 legacy 칩에서 이 값을 쓴다 */

#define PCIE_MISC_PCIE_CTRL				0x4064 /* [한국어] 컨트롤러 제어 레지스터. L23 요청과 (7278 계열의) PERST# 비트가 있다 */
#define  PCIE_MISC_PCIE_CTRL_PCIE_L23_REQUEST_MASK	0x1 /* [한국어] L23 진입 요청 비트. enter_l23 이 켜고 turn_off 가 되돌린다 */
#define PCIE_MISC_PCIE_CTRL_PCIE_PERSTB_MASK		0x4 /* [한국어] PERST# 비트(active-low). perst_set_7278 이 !val 로 쓰는 이유가 이 극성이다 */

#define PCIE_MISC_PCIE_STATUS				0x4068 /* [한국어] 컨트롤러 상태 레지스터. 포트 모드와 링크 상태가 모여 있다 */
#define  PCIE_MISC_PCIE_STATUS_PCIE_PORT_MASK		0x80 /* [한국어] RC 모드 비트. brcm_pcie_rc_mode() 가 이것 하나로 RC/EP 를 가른다 */
#define  PCIE_MISC_PCIE_STATUS_PCIE_DL_ACTIVE_MASK	0x20 /* [한국어] 데이터 링크 활성 비트 */
#define  PCIE_MISC_PCIE_STATUS_PCIE_PHYLINKUP_MASK	0x10 /* [한국어] PHY 링크업 비트. 위 비트와 둘 다여야 link_up() 이 참이다 */
#define  PCIE_MISC_PCIE_STATUS_PCIE_LINK_IN_L23_MASK	0x40 /* [한국어] 링크가 L23 에 들어갔는지. enter_l23 이 이 비트를 폴링한다 */

#define PCIE_MISC_REVISION				0x406c /* [한국어] 컨트롤러 리비전 레지스터 */
#define  BRCM_PCIE_HW_REV_33				0x0303 /* [한국어] MSI 배치가 바뀐 경계 리비전. 이 값 미만이면 legacy MSI 다 */
#define  BRCM_PCIE_HW_REV_3_20				0x0320 /* [한국어] BCM4908 에서 PERST# 배선이 바뀐 리비전. probe 가 이 이상을 거른다 */

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT		0x4070 /* [한국어] 바깥 창의 CPU 주소 base/limit(MB 단위 하위 비트). 창마다 4바이트 간격이다 */
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_MASK	0xfff00000 /* [한국어] 그중 limit 필드 */
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK	0xfff0 /* [한국어] 그중 base 필드. 이 필드 폭(HWEIGHT32)이 상위 비트를 밀어낼 shift 량이 된다 */
/* [한국어] 바깥 창 win 의 CPU base/limit 오프셋. 이 레지스터는 base 와 limit 를
 * 한 워드에 함께 담아 창마다 4 바이트 간격이다 — 위 LO/HI 쌍과 간격이 다른
 * 이유가 여기에 있다. */
#define PCIE_MEM_WIN0_BASE_LIMIT(win)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT + ((win) * 4)

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI			0x4080 /* [한국어] 바깥 창의 CPU base 상위 비트 */
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI_BASE_MASK	0xff /* [한국어] 그중 base 상위 필드 */
/* [한국어] 바깥 창 win 의 CPU base 상위 비트 오프셋. 다시 8 바이트 간격이다. */
#define PCIE_MEM_WIN0_BASE_HI(win)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI + ((win) * 8)

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI			0x4084 /* [한국어] 바깥 창의 CPU limit 상위 비트 */
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI_LIMIT_MASK	0xff /* [한국어] 그중 limit 상위 필드 */
/* [한국어] 바깥 창 win 의 CPU limit 상위 비트 오프셋. 8 바이트 간격. */
#define PCIE_MEM_WIN0_LIMIT_HI(win)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI + ((win) * 8)

#define  PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_MASK	0x2 /* [한국어] CLKREQ 디버그 활성 비트. clkreq no-l1ss 모드에서 켠다 */
#define  PCIE_MISC_HARD_PCIE_HARD_DEBUG_L1SS_ENABLE_MASK		0x200000 /* [한국어] L1SS 활성 비트. clkreq default 모드에서 켠다 */
#define  PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK		0x08000000 /* [한국어] SerDes IDDQ(전류 차단) 비트. setup 이 풀고 turn_off 가 건다 */
#define  PCIE_BMIPS_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK		0x00800000 /* [한국어] 같은 비트의 BMIPS 계열 자리. setup 이 is_bmips() 로 갈라 쓴다 */
/* [한국어] CLKREQ 동작을 정하는 두 모드 비트를 묶은 마스크.
 * brcm_config_clkreq() 가 이 마스크로 두 비트를 먼저 지워 "safe"(둘 다 0)
 * 상태에서 시작하고, 모드에 따라 둘 중 하나만 다시 켠다. */
#define  PCIE_CLKREQ_MASK \
	  (PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_MASK | \
	   PCIE_MISC_HARD_PCIE_HARD_DEBUG_L1SS_ENABLE_MASK)

#define PCIE_MISC_UBUS_BAR1_CONFIG_REMAP			0x40ac /* [한국어] 안쪽 창 1 의 UBUS remap 레지스터. BCM7712 만 쓰며 창마다 8바이트 간격이다 */
#define  PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_ACCESS_EN_MASK	BIT(0) /* [한국어] 그 레지스터의 접근 허용 비트. 하위 12비트 자리에 있어 주소와 겹치지 않는다 */
#define PCIE_MISC_UBUS_BAR4_CONFIG_REMAP			0x410c /* [한국어] 안쪽 창 4 의 UBUS remap 레지스터. 1~3 과 자리가 끊겨 4번부터 다시 센다 */

#define PCIE_MSI_INTR2_BASE		0x4500 /* [한국어] 전용 MSI 인터럽트 블록의 시작. HW_REV_33 이상 칩이 쓴다 */

/* Offsets from INTR2_CPU and MSI_INTR2 BASE offsets */
#define  MSI_INT_STATUS			0x0 /* [한국어] 대기 중인 MSI 비트. isr 이 읽는다 */
#define  MSI_INT_CLR			0x8 /* [한국어] MSI 대기 비트 지우기. ack 콜백과 set_regs 가 1 을 써서 지운다 */
#define  MSI_INT_MASK_SET		0x10 /* [한국어] MSI 마스크 걸기. [관찰] 이 트리에서는 정의만 있고 쓰이는 곳이 없다 */
#define  MSI_INT_MASK_CLR		0x14 /* [한국어] MSI 마스크 풀기. set_regs 가 쓴다 */

/* Error report registers */
#define PCIE_OUTB_ERR_TREAT				0x6000 /* [한국어] 오류를 어떻게 취급할지 정하는 레지스터. [관찰] 정의만 있고 쓰이는 곳이 없다 */
#define  PCIE_OUTB_ERR_TREAT_CONFIG		0x1 /* [한국어] config 오류 취급 비트. [관찰] 쓰이는 곳이 없다 */
#define  PCIE_OUTB_ERR_TREAT_MEM			0x2 /* [한국어] 메모리 오류 취급 비트. [관찰] 쓰이는 곳이 없다 */
#define PCIE_OUTB_ERR_VALID				0x6004 /* [한국어] 기록된 오류가 있는지 알리는 레지스터. dump_err 가 가장 먼저 본다 */
#define PCIE_OUTB_ERR_CLEAR				0x6008 /* [한국어] 오류 기록을 지우는 레지스터. dump_err 가 정보를 다 읽은 뒤 1 을 쓴다 */
#define PCIE_OUTB_ERR_ACC_INFO				0x600c /* [한국어] 오류가 난 접근의 성격(종류/폭/방향/바이트 레인)을 담은 레지스터 */
#define  PCIE_OUTB_ERR_ACC_INFO_CFG_ERR			BIT(0) /* [한국어] config 접근에서 난 오류임을 뜻하는 비트 */
#define  PCIE_OUTB_ERR_ACC_INFO_MEM_ERR			BIT(1) /* [한국어] 메모리 접근에서 난 오류임을 뜻하는 비트. 둘이 동시에 설 수도 있어 dump_err 가 각각 확인한다 */
#define  PCIE_OUTB_ERR_ACC_INFO_TYPE_64			BIT(2) /* [한국어] 64비트 접근이었음을 뜻하는 비트. 서면 "64bit", 아니면 "32bit" 로 찍는다 */
#define  PCIE_OUTB_ERR_ACC_INFO_DIR_WRITE		BIT(4) /* [한국어] 쓰기였음을 뜻하는 비트. dump_err 가 !(..) 로 뒤집어 str_read_write 에 넘긴다 */
#define  PCIE_OUTB_ERR_ACC_INFO_BYTE_LANES		0xff00 /* [한국어] 어느 바이트 레인이 유효했는지. dump_err 가 여덟 자리 0/1 문자열로 풀어 찍는다 */
#define PCIE_OUTB_ERR_ACC_ADDR				0x6010 /* [한국어] 오류가 난 config 접근의 대상 주소(BDF 와 레지스터 번호) */
#define PCIE_OUTB_ERR_ACC_ADDR_BUS			0xff00000 /* [한국어] 그중 버스 번호 필드 */
#define PCIE_OUTB_ERR_ACC_ADDR_DEV			0xf8000 /* [한국어] 그중 장치 번호 필드 */
#define PCIE_OUTB_ERR_ACC_ADDR_FUNC			0x7000 /* [한국어] 그중 함수 번호 필드 */
#define PCIE_OUTB_ERR_ACC_ADDR_REG			0xfff /* [한국어] 그중 config 레지스터 오프셋 필드 */
#define PCIE_OUTB_ERR_CFG_CAUSE				0x6014 /* [한국어] config 오류의 원인 비트 묶음 */
#define  PCIE_OUTB_ERR_CFG_CAUSE_TIMEOUT		BIT(6) /* [한국어] 응답 타임아웃 */
#define  PCIE_OUTB_ERR_CFG_CAUSE_ABORT			BIT(5) /* [한국어] 대상이 abort 로 응답 */
#define  PCIE_OUTB_ERR_CFG_CAUSE_UNSUPP_REQ		BIT(4) /* [한국어] Unsupported Request 응답 */
#define  PCIE_OUTB_ERR_CFG_CAUSE_ACC_TIMEOUT		BIT(2) /* [한국어] 접근 자체가 타임아웃 */
#define  PCIE_OUTB_ERR_CFG_CAUSE_ACC_DISABLED		BIT(1) /* [한국어] 접근이 금지된 상태였음 */
#define  PCIE_OUTB_ERR_CFG_CAUSE_ACC_64BIT		BIT(0) /* [한국어] 64비트 접근이 허용되지 않는 자리였음 */
#define PCIE_OUTB_ERR_MEM_ADDR_LO			0x6018 /* [한국어] 오류가 난 메모리 접근 주소의 하위 워드 */
#define PCIE_OUTB_ERR_MEM_ADDR_HI			0x601c /* [한국어] 같은 주소의 상위 워드. dump_err 가 둘을 합쳐 64비트 주소를 만든다 */
#define PCIE_OUTB_ERR_MEM_CAUSE				0x6020 /* [한국어] 메모리 오류의 원인 비트 묶음 */
#define  PCIE_OUTB_ERR_MEM_CAUSE_TIMEOUT		BIT(6) /* [한국어] 응답 타임아웃 */
#define  PCIE_OUTB_ERR_MEM_CAUSE_ABORT			BIT(5) /* [한국어] 대상이 abort 로 응답 */
#define  PCIE_OUTB_ERR_MEM_CAUSE_UNSUPP_REQ		BIT(4) /* [한국어] Unsupported Request 응답 */
#define  PCIE_OUTB_ERR_MEM_CAUSE_ACC_DISABLED		BIT(1) /* [한국어] 접근이 금지된 상태였음 */
#define  PCIE_OUTB_ERR_MEM_CAUSE_BAD_ADDR		BIT(0) /* [한국어] 주소가 어느 창에도 들지 않았음 */

#define  PCIE_RGR1_SW_INIT_1_PERST_MASK			0x1 /* [한국어] RGR1_SW_INIT_1 레지스터 안의 PERST# 비트. perst_set_generic 이 쓴다 */

#define RGR1_SW_INIT_1_INIT_GENERIC_MASK		0x2 /* [한국어] 같은 레지스터의 브리지 리셋 비트(일반 판) */
#define RGR1_SW_INIT_1_INIT_GENERIC_SHIFT		0x1 /* [한국어] 그 비트의 자리. 마스크와 짝을 이뤄 읽고-고쳐-쓰기에 쓰인다 */
#define RGR1_SW_INIT_1_INIT_7278_MASK			0x1 /* [한국어] 브리지 리셋 비트(BCM7278 판) — 자리가 달라 상수가 따로 있다 */
#define RGR1_SW_INIT_1_INIT_7278_SHIFT			0x0 /* [한국어] 그 비트의 자리 */

/* PCIe parameters */
#define BRCM_NUM_PCIE_OUT_WINS		0x4 /* [한국어] 바깥 창 개수. setup 이 이 수를 넘는 자원을 만나면 오류로 돌아가고, BMIPS 계열은 128MB 조각의 개수 상한으로도 쓴다 */
#define BRCM_INT_PCI_MSI_NR		32 /* [한국어] 일반 칩의 MSI 벡터 수. used 비트맵의 크기이기도 하다 */
#define BRCM_INT_PCI_MSI_LEGACY_NR	8 /* [한국어] legacy 칩의 MSI 벡터 수. enable_msi 의 BUILD_BUG_ON 이 이 값이 위 값을 넘지 않음을 못박는다 */
#define BRCM_INT_PCI_MSI_SHIFT		0 /* [한국어] [관찰] 정의만 있고 이 트리에서 쓰이는 곳이 없다 */
#define BRCM_INT_PCI_MSI_MASK		GENMASK(BRCM_INT_PCI_MSI_NR - 1, 0) /* [한국어] 일반 칩에서 마스크를 풀고 대기 비트를 지울 때 쓰는 전체 마스크 */
/* [한국어] legacy 칩에서 쓰는 MSI 마스크.
 * 그 칩들은 MSI 비트가 공유 레지스터의 상위 바이트([31..24])에 있어,
 * 위쪽부터 legacy 벡터 수만큼 잡는 GENMASK 가 된다. brcm_msi_set_regs() 가
 * 마스크를 풀고 대기 비트를 지울 때 이 값을 쓴다. */
#define BRCM_INT_PCI_MSI_LEGACY_MASK	GENMASK(31, \
						32 - BRCM_INT_PCI_MSI_LEGACY_NR)

/* MSI target addresses */
#define BRCM_MSI_TARGET_ADDR_LT_4GB	0x0fffffffcULL /* [한국어] 4GB 아래 MSI 목표 주소. 32비트만 다루는 장치를 위한 선택지다 */
#define BRCM_MSI_TARGET_ADDR_GT_4GB	0xffffffffcULL /* [한국어] 4GB 위 MSI 목표 주소. 안쪽 창이 낮은 자리를 이미 덮고 있을 때 쓴다 */

/* MDIO registers */
#define MDIO_PORT0			0x0 /* [한국어] MDIO 포트 0. 이 파일이 다루는 PHY 는 이 포트 하나뿐이다 */
#define MDIO_DATA_MASK			0x7fffffff /* [한국어] MDIO 읽기 결과에서 데이터만 남기는 마스크. 최상위 완료 비트를 뺀 나머지다 */
#define MDIO_PORT_MASK			0xf0000 /* [한국어] 조립 패킷의 포트 필드(하위 4비트분) */
#define MDIO_PORT_EXT_MASK		0x200000 /* [한국어] 포트 번호가 4비트를 넘을 때 쓰는 확장 비트. form_pkt 가 port >> 4 를 여기에 넣는다 */
#define MDIO_REGAD_MASK			0xffff /* [한국어] 조립 패킷의 레지스터 주소 필드 */
#define MDIO_CMD_MASK			0x00100000 /* [한국어] 조립 패킷의 명령 필드 */
#define MDIO_CMD_READ			0x1 /* [한국어] 읽기 명령 값 */
#define MDIO_CMD_WRITE			0x0 /* [한국어] 쓰기 명령 값 */
#define MDIO_DATA_DONE_MASK		0x80000000 /* [한국어] 완료 표시 비트. 읽기와 쓰기에서 뜻이 반대라 아래 두 매크로가 갈린다 */
#define MDIO_RD_DONE(x)			(((x) & MDIO_DATA_DONE_MASK) ? 1 : 0) /* [한국어] 읽기 완료 판정 — 비트가 서면 완료 */
#define MDIO_WT_DONE(x)			(((x) & MDIO_DATA_DONE_MASK) ? 0 : 1) /* [한국어] 쓰기 완료 판정 — 비트가 내려가면 완료. 폴링 조건이 읽기와 반대인 이유다 */
#define SSC_REGS_ADDR			0x1100 /* [한국어] SSC 관련 PHY 레지스터 뭉치의 주소. set_ssc 가 SET_ADDR_OFFSET 에 이 값을 써서 뭉치를 고른다 */
#define SET_ADDR_OFFSET			0x1f /* [한국어] 뭉치를 고르는 주소 설정 레지스터의 번호 */
#define SSC_CNTL_OFFSET			0x2 /* [한국어] 그 뭉치 안의 SSC 제어 레지스터 번호 */
#define SSC_CNTL_OVRD_EN_MASK		0x8000 /* [한국어] SSC 오버라이드 활성 비트 */
#define SSC_CNTL_OVRD_VAL_MASK		0x4000 /* [한국어] SSC 오버라이드 값 비트. 둘을 함께 세워야 SSC 가 켜진다 */
#define SSC_STATUS_OFFSET		0x1 /* [한국어] 그 뭉치 안의 SSC 상태 레지스터 번호 */
#define SSC_STATUS_SSC_MASK		0x400 /* [한국어] SSC 가 실제로 켜졌는지 */
#define SSC_STATUS_PLL_LOCK_MASK	0x800 /* [한국어] PLL 이 잠겼는지. 둘 다여야 set_ssc 가 성공으로 친다 */
#define PCIE_BRCM_MAX_MEMC		3 /* [한국어] memc_size[] 배열의 크기. DT 의 brcm,scb-sizes 에서 읽을 항목 수의 상한이다 */

#define IDX_ADDR(pcie)			((pcie)->cfg->offsets[EXT_CFG_INDEX]) /* [한국어] 변종별 EXT_CFG_INDEX 오프셋 조회. map_bus 가 BDF 를 여기에 쓴다 */
#define DATA_ADDR(pcie)			((pcie)->cfg->offsets[EXT_CFG_DATA]) /* [한국어] 변종별 EXT_CFG_DATA 오프셋 조회. 인덱스가 가리키는 config 창이다 */
#define PCIE_RGR1_SW_INIT_1(pcie)	((pcie)->cfg->offsets[RGR1_SW_INIT_1]) /* [한국어] 변종별 RGR1_SW_INIT_1 오프셋 조회. 브리지 리셋/PERST# 비트가 있는 레지스터 */
#define HARD_DEBUG(pcie)		((pcie)->cfg->offsets[PCIE_HARD_DEBUG]) /* [한국어] 변종별 HARD_DEBUG 오프셋 조회. SerDes IDDQ 와 CLKREQ 비트가 있는 레지스터 */
#define INTR2_CPU_BASE(pcie)		((pcie)->cfg->offsets[PCIE_INTR2_CPU_BASE]) /* [한국어] 변종별 INTR2_CPU 블록 시작 조회. legacy MSI 가 이 블록을 나눠 쓴다 */

/* Rescal registers */
#define PCIE_DVT_PMU_PCIE_PHY_CTRL				0xc700 /* [한국어] PHY(rescal) 제어 레지스터. has_phy 인 칩에만 있다 */
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_NFLDS			0x3 /* [한국어] brcm_phy_cntl() 이 순서대로 다루는 필드 수(셋). shifts[]/masks[] 배열의 크기다 */
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_DIG_RESET_MASK		0x4 /* [한국어] 디지털 리셋 비트 — 시작 시 마지막, 정지 시 처음으로 다뤄진다 */
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_DIG_RESET_SHIFT	0x2 /* [한국어] 그 비트의 자리. start 일 때 BIT_MASK(shift) 로 값을 만든다 */
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_RESET_MASK		0x2 /* [한국어] 리셋 비트 */
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_RESET_SHIFT		0x1 /* [한국어] 그 비트의 자리 */
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_PWRDN_MASK		0x1 /* [한국어] 파워다운 비트 — 시작 시 처음, 정지 시 마지막으로 다뤄진다 */
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_PWRDN_SHIFT		0x0 /* [한국어] 그 비트의 자리 */

/* Forward declarations */
/* [한국어] struct brcm_pcie 의 전방 선언.
 * 아래 struct pcie_cfg_data 의 함수 포인터들이 brcm_pcie 를 인자로 받는데,
 * brcm_pcie 자신은 pcie_cfg_data 포인터를 필드로 갖는다. 서로를 참조하는
 * 관계라 한쪽을 먼저 이름만 알려 두어야 컴파일이 된다. */
struct brcm_pcie;

/* [한국어] 변종별 레지스터 오프셋 표(offsets[])의 인덱스로 쓰는 익명 enum.
 * 같은 이름의 레지스터가 SoC 마다 다른 자리에 있어, 코드는 이 이름으로
 * 가리키고 실제 오프셋은 pcie->cfg->offsets[이 값] 에서 꺼낸다.
 * 아래 매크로 PCIE_RGR1_SW_INIT_1() / EXT_CFG_INDEX() / HARD_DEBUG() /
 * INTR2_CPU_BASE() 가 그 조회를 감싼다.
 *
 * RGR1_SW_INIT_1      브리지 소프트 리셋과 (일반 판) PERST# 비트가 있는 레지스터.
 * EXT_CFG_INDEX       config 접근 시 대상 BDF 를 적어 창을 옮기는 인덱스 레지스터.
 * EXT_CFG_DATA        그 창을 통해 실제로 읽고 쓰는 데이터 레지스터.
 * PCIE_HARD_DEBUG     SerDes IDDQ 와 CLKREQ# 모드 비트가 모여 있는 레지스터.
 * PCIE_INTR2_CPU_BASE legacy MSI 가 얹혀 쓰는 인터럽트 블록의 시작.
 *
 * 설정자: 없음(컴파일 시점 상수). 읽는 자: 위 네 매크로.
 * 값 범위: 0부터 차례로. 표 배열의 크기와 순서가 이 enum 에 묶여 있다. */
enum {
	RGR1_SW_INIT_1,
	EXT_CFG_INDEX,
	EXT_CFG_DATA,
	PCIE_HARD_DEBUG,
	PCIE_INTR2_CPU_BASE,
};

/* [한국어] 이 드라이버가 다루는 SoC 계열 식별자.
 * pcie_cfg_data.soc_base 에 담기며, 코드 곳곳에서 == 비교로 분기한다.
 *
 * GENERIC  특별한 예외가 없는 STB 계열의 기본값.
 * BCM2711  라즈베리파이 4 계열. setup 에서 PERST# 를 명시적으로 걸고,
 *          버스트 크기 인코딩이 다르며, dma-ranges 검증의 배경이 된 칩.
 * BCM4908  PERST# 가 별도 리셋 컨트롤러에 있는 칩. 특정 리비전 이상은
 *          probe 가 지원하지 않는다고 거른다.
 * BCM7278  브리지 리셋 비트 자리와 버스트 인코딩이 모두 다른 칩.
 * BCM7425  BMIPS 계열. config 데이터 창이 레지스터 하나 크기라 pci_ops 를
 *          따로 쓴다.
 * BCM7435  BMIPS 계열. 7425 와 같은 MSI/버스트 특성을 공유한다.
 * BCM7712  안쪽 창을 여럿 쓰는 최신 계열. UBUS remap 설정이 필요하고,
 *          rbus 타임아웃 레지스터가 없으며, 첫 안쪽 창을 끄지 않는다.
 *
 * 설정자: 파일 끝의 pcie_cfg_data 상수들. 읽는 자: is_bmips() 와 setup /
 * get_inbound_wins / set_inbound_win_registers / probe 의 분기. */
enum pcie_soc_base {
	GENERIC,
	BCM2711,
	BCM4908,
	BCM7278,
	BCM7425,
	BCM7435,
	BCM7712,
};

/* [한국어] 안쪽(inbound) 창 하나를 나타내는 계산용 구조체.
 * 하드웨어 레지스터의 모양이 아니라, brcm_pcie_get_inbound_wins() 가
 * DT 의 dma-ranges 를 해석해 만든 중간 결과다. 그것을
 * set_inbound_win_registers() 가 받아 레지스터에 적는다.
 *
 * 배열로만 쓰이며, 인덱스 0 은 비워 둔다 — 하드웨어와 PCIe 가 BAR 를
 * 1번부터 세기 때문이다. */
struct inbound_win {
		/* [한국어] 이 창이 덮는 크기(바이트).
		 * 설정자: add_inbound_win() 을 통해 get_inbound_wins() 가 정한다.
		 * 읽는 자: set_inbound_win_registers() 가 brcm_pcie_encode_ibar_size()
		 *          로 비선형 인코딩으로 바꿔 레지스터의 SIZE 필드에 넣는다.
		 * 값 범위: 2의 거듭제곱. 0 은 "이 창을 쓰지 않음" 을 뜻하며, encode 가
		 *          0 을 돌려주어 창이 비활성이 된다.
		 * 동기화: probe/resume 안에서만 다루는 스택 배열이라 필요 없다. */
	u64 size;
		/* [한국어] 이 창이 대응하는 PCI 쪽 시작 주소.
		 * 설정자: add_inbound_win(). STB 계열은 dma-ranges 중 가장 낮은 PCI
		 *          주소를, 7712 계열은 각 dma-ranges 항목의 PCI 시작을 쓴다.
		 * 읽는 자: set_inbound_win_registers() 가 LO/HI 두 워드로 나눠 적는다.
		 * 값 범위: size 의 배수여야 한다 — get_inbound_wins() 가 그 정렬을
		 *          확인하고 어긋나면 -EINVAL 로 돌아간다.
		 * 동기화: 스택 지역 데이터. */
	u64 pci_offset;
		/* [한국어] 이 창의 CPU(시스템 메모리) 쪽 시작 주소.
		 * 설정자: add_inbound_win(). STB 계열은 늘 0 인데, 그 칩들은 BAR2 의
		 *          CPU 쪽 시작이 시스템 메모리 시작에 하드와이어되어 있기
		 *          때문이다. 7712 계열은 dma-ranges 의 CPU 시작을 쓴다.
		 * 읽는 자: set_inbound_win_registers() 가 7712 에서만 UBUS remap
		 *          레지스터에 적는다. 다른 칩에서는 읽히지 않는다.
		 * 값 범위: 7712 에서는 하위 12비트가 무시된다 — 그 자리가 플래그 자리라
		 *          코드가 & ~0xfff 로 지우고 ACCESS_EN 을 대신 넣는다.
		 * 동기화: 스택 지역 데이터. */
	u64 cpu_addr;
};

/*
 * The RESCAL block is tied to PCIe controller #1, regardless of the number of
 * controllers, and turning off PCIe controller #1 prevents access to the RESCAL
 * register blocks, therefore no other controller can access this register
 * space, and depending upon the bus fabric we may get a timeout (UBUS/GISB),
 * or a hang (AXI).
 */
#define CFG_QUIRK_AVOID_BRIDGE_SHUTDOWN		BIT(0) /* [한국어] 브리지를 끄면 안 되는 칩을 표시하는 quirk 비트. turn_off 가 이것을 보고 마지막 리셋 단계를 건너뛴다 */

/* [한국어] compatible 문자열 하나마다 하나씩 있는 상수 표.
 * 이 드라이버의 변종 흡수가 전부 이 구조체를 통해 이뤄진다. probe 가
 * of_device_get_match_data() 로 골라 pcie->cfg 에 꽂아 두면, 그 뒤로는
 * 코드가 칩 이름을 직접 알 필요 없이 이 표만 본다.
 *
 * 전부 const 정적 인스턴스라 런타임에 바뀌지 않는다. 그래서 어느 필드도
 * 동기화가 필요 없다. */
struct pcie_cfg_data {
		/* [한국어] 변종별 레지스터 오프셋 배열.
		 * 설정자: 파일 끝의 pcie_offsets 계열 배열 중 하나를 가리킨다.
		 * 읽는 자: PCIE_RGR1_SW_INIT_1() / EXT_CFG_INDEX() / EXT_CFG_DATA() /
		 *          HARD_DEBUG() / INTR2_CPU_BASE() 매크로.
		 * 값 범위: 위 익명 enum 의 원소 수만큼 채워진 const 배열. NULL 불가.
		 * 동기화: 읽기 전용. */
	const int *offsets;
		/* [한국어] 이 표가 어느 SoC 계열을 위한 것인지.
		 * 설정자: 각 pcie_cfg_data 상수. 읽는 자: is_bmips() 와 setup /
		 *          get_inbound_wins / set_inbound_win_registers /
		 *          extend_rbus_timeout / probe 의 == 비교 분기.
		 * 값 범위: enum pcie_soc_base 의 원소.
		 * 동기화: 읽기 전용. */
	const enum pcie_soc_base soc_base;
		/* [한국어] 이 칩이 소프트웨어로 제어하는 PHY 블록을 갖는지.
		 * 설정자: 각 pcie_cfg_data 상수. 읽는 자: brcm_phy_start() 와
		 *          brcm_phy_stop() 이 이 값으로 brcm_phy_cntl() 을 부를지 정한다.
		 * 값 범위: true/false. 거짓이면 PCIE_DVT_PMU_PCIE_PHY_CTRL 레지스터가
		 *          없다는 뜻이라 접근해서는 안 된다.
		 * 동기화: 읽기 전용. */
	const bool has_phy;
		/* [한국어] 이 칩에만 필요한 우회 동작 비트 묶음.
		 * 설정자: 각 pcie_cfg_data 상수. 읽는 자: brcm_pcie_turn_off() 가
		 *          CFG_QUIRK_AVOID_BRIDGE_SHUTDOWN 을 확인한다.
		 * 값 범위: 지금은 비트 0 하나뿐. 그 비트가 서면 정지 시 브리지 리셋을
		 *          걸지 않는데, 위 상류 주석대로 RESCAL 블록이 컨트롤러 #1 에
		 *          묶여 있어 그것을 끄면 다른 컨트롤러가 RESCAL 레지스터에
		 *          닿지 못하고 버스 종류에 따라 타임아웃이나 행이 나기 때문이다.
		 * 동기화: 읽기 전용. */
	const u32 quirks;
		/* [한국어] 이 칩이 쓸 수 있는 안쪽 창(BAR) 개수.
		 * 설정자: 각 pcie_cfg_data 상수. 대부분 3 이고 BCM7712 만 10 이다.
		 * 읽는 자: brcm_pcie_get_inbound_wins() 가 dma-ranges 를 훑다가 이
		 *          수를 넘으면 루프를 멈춘다.
		 * 값 범위: 1 이상 PCIE_BRCM_MAX_INBOUND_WINS 이하.
		 * 동기화: 읽기 전용. */
	u8 num_inbound_wins;
		/* [한국어] PERST# 를 걸고 푸는 변종별 구현.
		 * 설정자: 각 pcie_cfg_data 상수(perst_set_generic / _7278 / _4908).
		 * 읽는 자: brcm_pcie_setup() / brcm_pcie_start_link() /
		 *          brcm_pcie_turn_off() 가 직접 부른다.
		 * 값 범위: NULL 불가 — 모든 cfg 가 채운다. 인자 규약은 1=리셋,
		 *          0=해제로 통일되어 있고, 극성이 반대인 칩은 구현 안에서 뒤집는다.
		 * 동기화: 읽기 전용 포인터. */
	int (*perst_set)(struct brcm_pcie *pcie, u32 val);
		/* [한국어] 브리지 소프트 리셋을 걸고 푸는 변종별 구현.
		 * 설정자: 각 pcie_cfg_data 상수(bridge_sw_init_set_generic / _7278).
		 * 읽는 자: 래퍼 brcm_pcie_bridge_sw_init_set() 이 부른다. 다만
		 *          brcm_pcie_setup() 의 실패 되돌리기 한 곳에서는 래퍼를 거치지
		 *          않고 직접 부른다.
		 * 값 범위: NULL 불가.
		 * 동기화: 읽기 전용 포인터. 구현이 하는 읽고-고쳐-쓰기의 보호는
		 *          래퍼가 bridge_lock 으로 맡는다. */
	int (*bridge_sw_init_set)(struct brcm_pcie *pcie, u32 val);
		/* [한국어] setup 이 끝난 뒤 칩 전용으로 더 할 일이 있으면 그 구현.
		 * 설정자: BCM2712 용 표만 brcm_pcie_post_setup_bcm2712 를 넣는다.
		 * 읽는 자: brcm_pcie_setup() 의 마지막이 NULL 확인 후 부른다.
		 * 값 범위: NULL 이면 할 일이 없다는 뜻 — 대부분의 표가 그렇다.
		 * 동기화: 읽기 전용 포인터. */
	int (*post_setup)(struct brcm_pcie *pcie);
		/* [한국어] 이 칩이 바깥 접근 오류를 기록하는 레지스터를 갖는지.
		 * 설정자: 각 pcie_cfg_data 상수. 읽는 자: probe 가 die/panic 알림을
		 *          걸지 정할 때, remove 가 그것을 뗄 때, 그리고
		 *          brcm_pcie_bridge_sw_init_set() 이 스핀락을 잡을지 정할 때.
		 * 값 범위: true/false. 거짓이면 bridge_lock 이 초기화조차 되지 않으므로
		 *          그 락을 잡는 경로도 함께 막혀 있어야 한다 — 실제로 두 곳
		 *          모두 이 필드로 갈린다.
		 * 동기화: 읽기 전용. */
	bool has_err_report;
};

/* [한국어] 하류 슬롯 전원용 레귤레이터 묶음.
 * 구조체 끝에 가변 길이 배열이 붙는 배치라, alloc_subdev_regulators() 가
 * 크기를 직접 계산해 한 번에 잡는다.
 *
 * 수명은 하류 버스 device 에 devm 으로 묶인다. 소유자는 pcie->sr 이며,
 * add_bus 가 채우고 remove_bus 가 NULL 로 되돌린다. */
struct subdev_regulators {
		/* [한국어] supplies[] 에 실제로 들어 있는 항목 수.
		 * 설정자: alloc_subdev_regulators() 가 ARRAY_SIZE(supplies) 로 채운다.
		 * 읽는 자: regulator_bulk_get / _enable / _disable / _free 의 개수 인자.
		 * 값 범위: 지금은 늘 3(vpcie3v3, vpcie3v3aux, vpcie12v).
		 * 동기화: 버스 스캔/제거 경로에서만 다뤄 별도 락이 없다. */
	unsigned int num_supplies;
		/* [한국어] 레귤레이터 핸들과 이름을 담는 가변 길이 배열.
		 * 설정자: alloc_subdev_regulators() 가 .supply 에 이름 문자열을 꽂고,
		 *          regulator_bulk_get() 이 그 이름으로 찾은 핸들을 채운다.
		 * 읽는 자: regulator_bulk_enable/disable 이 켜고 끈다.
		 * 값 범위: 보드에 없는 전원은 코어가 더미 핸들로 채워 준다.
		 * 동기화: 위와 같다. */
	struct regulator_bulk_data supplies[];
};

/* [한국어] 내장 MSI 컨트롤러의 상태 전부.
 * brcm_pcie 와 따로 두는 이유는 MSI 가 선택적이기 때문이다 — 외부 MSI
 * 컨트롤러를 쓰는 보드에서는 이 구조체가 아예 만들어지지 않고
 * pcie->msi 가 NULL 로 남는다.
 *
 * devm_kzalloc 으로 잡혀 플랫폼 device 수명에 묶인다. */
struct brcm_msi {
		/* [한국어] 로그와 devm 할당에 쓸 device.
		 * 설정자: brcm_pcie_enable_msi() 가 pcie->dev 를 복사해 둔다.
		 * 읽는 자: brcm_pcie_msi_isr() 의 dev_dbg, brcm_allocate_domains() 의
		 *          dev_err.
		 * 값 범위: NULL 불가.
		 * 동기화: 만든 뒤 바뀌지 않는다. */
	struct device		*dev;
		/* [한국어] 컨트롤러 레지스터 창의 시작.
		 * 설정자: brcm_pcie_enable_msi() 가 pcie->base 를 복사해 둔다.
		 * 읽는 자: 같은 함수가 intr_base 를 계산할 때, 그리고
		 *          brcm_msi_set_regs() 가 MSI BAR/DATA 레지스터에 적을 때.
		 * 값 범위: ioremap 된 주소. NULL 불가.
		 * 동기화: 만든 뒤 바뀌지 않는다. */
	void __iomem		*base;
		/* [한국어] MSI 도메인의 fwnode 로 쓸 DT 노드.
		 * 설정자: brcm_pcie_enable_msi() 가 pcie->np 를 복사해 둔다.
		 * 읽는 자: brcm_allocate_domains() 가 of_fwnode_handle() 로 감싸
		 *          irq_domain_info.fwnode 에 넣는다.
		 * 값 범위: PCIe 노드 자신. probe 가 msi-parent 가 자기 자신을 가리킬
		 *          때만 이 경로로 들어오므로 그렇다.
		 * 동기화: 만든 뒤 바뀌지 않는다. */
	struct device_node	*np;
		/* [한국어] msi_create_parent_irq_domain() 이 만든 도메인.
		 * 설정자: brcm_allocate_domains().
		 * 읽는 자: brcm_pcie_msi_isr() 이 generic_handle_domain_irq() 에 넘기고,
		 *          brcm_free_domains() 가 없앤다.
		 * 값 범위: NULL 이면 도메인 생성 실패 — 그 경우 enable_msi 가 오류로
		 *          돌아가므로 이후 경로에서는 늘 유효하다.
		 * 동기화: probe 에서 한 번 만들고 remove 에서 없앤다. */
	struct irq_domain	*inner_domain;
		/* [한국어] used 비트맵을 보호하는 뮤텍스(상류 주석대로 alloc/free 를 지킨다).
		 * 설정자: brcm_pcie_enable_msi() 의 mutex_init().
		 * 읽는 자: brcm_msi_alloc() 과 brcm_msi_free().
		 * 값 범위: 잠들 수 있는 락. 벡터 할당이 프로세스 컨텍스트에서만
		 *          일어나므로 스핀락일 필요가 없다.
		 * 동기화: 이 락이 지키는 대상은 used 비트맵 하나뿐이다. */
	struct mutex		lock; /* guards the alloc/free operations */
		/* [한국어] 장치가 MSI 를 쓸 목적지 주소.
		 * 설정자: brcm_pcie_enable_msi() 가 pcie->msi_target_addr 를 복사한다.
		 *          그 값은 brcm_pcie_setup() 이 안쪽 창 배치를 보고 4GB 위/아래
		 *          중 하나로 정한 것이다.
		 * 읽는 자: brcm_msi_compose_msi_msg() 가 장치에 알려 주고,
		 *          brcm_msi_set_regs() 가 하드웨어 MSI BAR 에 적는다.
		 * 값 범위: BRCM_MSI_TARGET_ADDR_LT_4GB 또는 _GT_4GB.
		 * 동기화: 만든 뒤 바뀌지 않는다(resume 은 setup 을 다시 돌리지만
		 *          같은 규칙으로 같은 값을 얻는다). */
	u64			target_addr;
		/* [한국어] MSI 를 물어 오는 상위 인터럽트 번호.
		 * 설정자: brcm_pcie_enable_msi() 가 irq_of_parse_and_map(np, 1) 로 얻는다
		 *          — DT 인터럽트 목록의 두 번째가 MSI 용이기 때문이다.
		 * 읽는 자: irq_set_chained_handler_and_data() 로 핸들러를 걸 때와
		 *          brcm_msi_remove() 가 뗄 때.
		 * 값 범위: 0 보다 커야 한다. 아니면 enable_msi 가 -ENODEV 로 돌아간다.
		 * 동기화: 만든 뒤 바뀌지 않는다. */
	int			irq;
		/* [한국어] 어느 MSI 벡터가 쓰이고 있는지 표시하는 비트맵.
		 * 설정자/읽는 자: brcm_msi_alloc() 과 brcm_msi_free() 만 다룬다.
		 * 값 범위: 비트 수가 BRCM_INT_PCI_MSI_NR 로 고정이라, legacy 칩에서
		 *          nr 이 더 작아도 배열은 같은 크기다 — enable_msi 의
		 *          BUILD_BUG_ON 이 legacy 수가 이 크기를 넘지 않음을 못박는다.
		 * 동기화: 위의 lock 뮤텍스가 지킨다. */
	DECLARE_BITMAP(used, BRCM_INT_PCI_MSI_NR);
	bool			legacy;
	/* Some chips have MSIs in bits [31..24] of a shared register. */
	int			legacy_shift;
	int			nr; /* No. of MSI available, depends on chip */
	/* This is the base pointer for interrupt status/set/clr regs */
	void __iomem		*intr_base;
};

/* Internal PCIe Host Controller Information.*/
struct brcm_pcie {
		/* [한국어] 플랫폼 device. 로그와 devm 할당의 기준이다.
		 * 설정자: brcm_pcie_probe(). 읽는 자: 거의 모든 함수의 dev_err/dev_info.
		 * 값 범위: NULL 불가.
		 * 동기화: probe 에서 한 번 정하고 바뀌지 않는다. */
	struct device		*dev;
		/* [한국어] 컨트롤러 레지스터 창의 시작 주소.
		 * 설정자: brcm_pcie_probe() 의 devm_platform_ioremap_resource().
		 * 읽는 자: 이 파일의 모든 readl/writel 이 여기에 오프셋을 더한다.
		 * 값 범위: ioremap 된 주소. IS_ERR 면 probe 가 그 자리에서 돌아간다.
		 * 동기화: 바뀌지 않는다. */
	void __iomem		*base;
		/* [한국어] 컨트롤러 클록("sw_pcie").
		 * 설정자: brcm_pcie_probe() 의 devm_clk_get_optional().
		 * 읽는 자: probe/resume 이 켜고, suspend/remove 가 끈다.
		 * 값 범위: optional 이라 보드에 없으면 NULL 이고, 클록 API 가 NULL 을
		 *          무해하게 처리한다.
		 * 동기화: 켜고 끄는 순서를 PM 코어가 직렬화한다. */
	struct clk		*clk;
		/* [한국어] 이 컨트롤러의 DT 노드.
		 * 설정자: brcm_pcie_probe(). 읽는 자: DT 속성을 읽는 모든 곳 —
		 *          brcm,scb-sizes, brcm,clkreq-mode, aspm-no-l0s, num-lanes,
		 *          msi-parent.
		 * 값 범위: NULL 불가(플랫폼 디바이스가 DT 에서 만들어진다).
		 * 동기화: 바뀌지 않는다. */
	struct device_node	*np;
		/* [한국어] DT 가 SSC(대역 확산 클록)를 요청했는지.
		 * 설정자: brcm_pcie_probe() 가 "brcm,enable-ssc" 속성으로 채운다.
		 * 읽는 자: brcm_pcie_start_link() 가 링크가 선 뒤 brcm_pcie_set_ssc()
		 *          를 부를지 정한다.
		 * 값 범위: true/false. 실패해도 링크는 살아 있어 치명적이지 않다.
		 * 동기화: 바뀌지 않는다. */
	bool			ssc;
		/* [한국어] 링크 세대를 강제로 제한할 때 쓸 값.
		 * 읽는 자: brcm_pcie_start_link() 가 0 이 아니면 brcm_pcie_set_gen()
		 *          을 부른다.
		 * 값 범위: PCIe 세대 번호(1~).
		 * [관찰] 이 트리에서 이 필드에 대입하는 곳은 probe 의 `pcie->gen = 0;`
		 *        한 줄뿐이다(스냅숏 1f0e418bb6 도 같다). 브리지 private 영역이
		 *        0 으로 초기화되므로 실제로는 늘 0 이고, 따라서 세대 제한은
		 *        일어나지 않는다. 상류 코드 그대로 두었다.
		 * 동기화: 바뀌지 않는다. */
	int			gen;
		/* [한국어] 장치가 MSI 를 쓸 목적지 주소.
		 * 설정자: brcm_pcie_setup() 이 안쪽 창 배치를 보고 정한다.
		 * 읽는 자: brcm_pcie_enable_msi() 가 msi->target_addr 로 복사한다.
		 * 값 범위: BRCM_MSI_TARGET_ADDR_LT_4GB 또는 _GT_4GB.
		 * 동기화: setup 안에서만 쓰고, MSI 설정보다 먼저 정해진다. */
	u64			msi_target_addr;
		/* [한국어] 내장 MSI 컨트롤러 상태. 쓰지 않으면 NULL 이다.
		 * 설정자: brcm_pcie_enable_msi() 가 모든 준비를 마친 뒤 마지막에 채운다
		 *          — NULL 이 아닌 것이 곧 "완전히 준비됨" 의 표시다.
		 * 읽는 자: brcm_msi_remove() 와 brcm_pcie_resume_noirq() 가 NULL 확인
		 *          후 쓴다.
		 * 값 범위: 외부 MSI 컨트롤러를 쓰는 보드에서는 계속 NULL.
		 * 동기화: probe 에서 채우고 remove 까지 바뀌지 않는다. */
	struct brcm_msi		*msi;
		/* [한국어] "rescal" 리셋 — 아날로그 보정 블록을 위한 공유 리셋.
		 * 설정자: brcm_pcie_probe() 의 devm_reset_control_get_optional_shared().
		 *          shared 인 이유는 여러 컨트롤러가 한 RESCAL 블록을 나눠 쓰기
		 *          때문이다(위 CFG_QUIRK 주석의 배경과 같다).
		 * 읽는 자: probe/resume 이 reset_control_reset() 으로 쓰고,
		 *          suspend/remove 가 reset_control_rearm() 으로 되돌린다.
		 * 값 범위: optional 이라 없으면 NULL.
		 * 동기화: 리셋 프레임워크가 공유 카운트를 관리한다. */
	struct reset_control	*rescal;
		/* [한국어] "perst" 리셋 — BCM4908 에서 PERST# 를 조작하는 유일한 수단.
		 * 설정자: brcm_pcie_probe() 의 devm_reset_control_get_optional_exclusive().
		 * 읽는 자: brcm_pcie_perst_set_4908() 만 쓴다.
		 * 값 범위: 그 칩이 아니면 NULL 이고, 그 칩인데 NULL 이면
		 *          perst_set_4908 이 WARN_ONCE 후 -EINVAL 을 돌려준다.
		 * 동기화: exclusive 라 이 드라이버만 소유한다. */
	struct reset_control	*perst_reset;
		/* [한국어] "bridge" 리셋 — 브리지 소프트 리셋을 레지스터 대신 리셋
		 * 컨트롤러로 다루는 보드용.
		 * 설정자: brcm_pcie_probe(). 읽는 자: brcm_pcie_bridge_sw_init_set_generic()
		 *          이 NULL 이 아니면 이 경로를 쓰고 레지스터를 건드리지 않는다.
		 * 값 범위: 없으면 NULL — 그때는 레지스터 경로로 간다.
		 * 동기화: exclusive. */
	struct reset_control	*bridge_reset;
		/* [한국어] "swinit" 리셋 — probe 초기에 한 번 펄스로 주는 리셋.
		 * 설정자: brcm_pcie_probe(). 읽는 자: 같은 probe 가 assert → 1us 대기 →
		 *          deassert 로 한 번만 쓴다. 그 뒤로는 쓰이지 않는다.
		 * 값 범위: 없으면 NULL 이고 그 단계 전체를 건너뛴다.
		 * 동기화: exclusive. */
	struct reset_control	*swinit_reset;
		/* [한국어] 이 SoC 의 메모리 컨트롤러(memc) 개수.
		 * 설정자: brcm_pcie_get_inbound_wins() 가 DT 의 "brcm,scb-sizes" 항목
		 *          수로 정하고, 그 속성이 없으면 1 로 짐작한다.
		 * 읽는 자: 같은 함수가 크기 합을 낼 때, brcm_pcie_setup() 이 SCB_SIZE
		 *          필드를 채울 때.
		 * 값 범위: 1 이상 PCIE_BRCM_MAX_MEMC 이하. setup 의 분기가 셋까지만
		 *          있어 그 이상은 반영되지 않는다.
		 * 동기화: setup 경로 안에서만 쓴다. */
	int			num_memc;
		/* [한국어] 각 memc 가 보여 주는 메모리 크기.
		 * 설정자: brcm_pcie_get_inbound_wins() 가 DT 에서 읽거나, 없으면
		 *          dma-ranges 총합을 2의 거듭제곱으로 올려 [0] 에 넣는다.
		 * 읽는 자: 같은 함수의 합산과 brcm_pcie_setup() 의 SCB_SIZE 계산
		 *          (ilog2 에서 15 를 빼는 인코딩).
		 * 값 범위: 각 항목이 2의 거듭제곱이어야 인코딩이 성립한다.
		 * 동기화: setup 경로 안에서만 쓴다. */
	u64			memc_size[PCIE_BRCM_MAX_MEMC];
		/* [한국어] 컨트롤러 리비전 레지스터에서 읽은 값.
		 * 설정자: brcm_pcie_probe() 가 setup 직후 읽는다.
		 * 읽는 자: probe 가 BCM4908 의 지원 불가 리비전을 거를 때,
		 *          brcm_pcie_enable_msi() 가 legacy MSI 배치인지 판정할 때
		 *          (BRCM_PCIE_HW_REV_33 미만이면 legacy).
		 * 값 범위: 칩이 정하는 값. 그래서 MSI 설정이 probe 순서상 리비전을
		 *          읽은 뒤에 와야 한다.
		 * 동기화: 바뀌지 않는다. */
	u32			hw_rev;
		/* [한국어] 하류 슬롯 전원 레귤레이터 묶음. 없으면 NULL.
		 * 설정자: brcm_pcie_add_bus() 가 성공했을 때만 채우고, 어느 단계에서든
		 *          실패하면 다시 NULL 로 되돌린다.
		 * 읽는 자: brcm_pcie_remove_bus(), brcm_pcie_suspend_noirq(),
		 *          brcm_pcie_resume_noirq() 가 NULL 확인 후 쓴다.
		 * 값 범위: 레귤레이터를 쓰지 않는 보드에서는 계속 NULL.
		 * 동기화: 버스 스캔/제거와 PM 경로에서만 다루며, 둘이 겹치지 않는다. */
	struct subdev_regulators *sr;
		/* [한국어] 서스펜드 때 하류에 웨이크업 장치가 있어 전원을 끄지 않았는지.
		 * 설정자: brcm_pcie_suspend_noirq() 가 false 로 초기화한 뒤
		 *          pci_walk_bus(pci_dev_may_wakeup) 로 채운다.
		 * 읽는 자: brcm_pcie_resume_noirq() 가 이 값을 보고 레귤레이터를 다시
		 *          켤지 정한다. 켜지 않아야 사용 카운트가 어긋나지 않는다.
		 * 값 범위: true/false. resume 이 쓰고 나서 곧바로 false 로 되돌려
		 *          다음 주기를 깨끗하게 만든다.
		 * 동기화: suspend/resume 이 번갈아 실행되므로 경쟁이 없다. */
	bool			ep_wakeup_capable;
		/* [한국어] 이 칩의 변종 상수 표.
		 * 설정자: brcm_pcie_probe() 가 of_device_get_match_data() 결과를 넣는다.
		 * 읽는 자: 사실상 모든 함수 — 오프셋 조회, 계열 비교, 함수 포인터 호출.
		 * 값 범위: NULL 이면 probe 가 -EINVAL 로 돌아가므로 이후에는 늘 유효.
		 * 동기화: 읽기 전용 상수를 가리킨다. */
	const struct pcie_cfg_data	*cfg;
		/* [한국어] 브리지가 지금 리셋 상태인지 소프트웨어가 기억해 두는 값.
		 * 설정자: brcm_pcie_bridge_sw_init_set() 이 조작 결과에 따라 갱신한다.
		 *          실패하면 리셋(꺼짐)으로 간주해 true 로 둔다 — 안전한 쪽이다.
		 * 읽는 자: brcm_pcie_dump_err() 가 레지스터를 읽어도 되는지 판정한다.
		 *          리셋 상태에서 읽으면 또 다른 abort 가 나기 때문이다.
		 * 값 범위: true/false.
		 * 동기화: has_err_report 인 칩에서 bridge_lock 아래에서만 갱신·조회된다
		 *          — 그래야 die/panic 콜백이 보는 값과 실제 상태가 어긋나지 않는다. */
	bool			bridge_in_reset;
		/* [한국어] die 알림 체인에 걸어 둘 노드.
		 * 값으로 박아 두는 이유는 콜백이 container_of 로 바깥 brcm_pcie 를
		 * 되찾아야 하기 때문이다 — 알림 체인은 문맥 포인터를 따로 주지 않는다.
		 * 설정자/읽는 자: brcm_register_die_notifiers() 가 notifier_call 을
		 *          채워 등록하고, brcm_unregister_die_notifiers() 가 뗀다.
		 * 값 범위: has_err_report 인 칩에서만 등록된다.
		 * 동기화: 알림 체인 등록/해제는 커널이 관리한다. */
	struct notifier_block	die_notifier;
		/* [한국어] panic 알림 체인에 걸어 둘 노드.
		 * die 쪽과 같은 이유로 값으로 박혀 있다. 등록 API 만 다른데, panic
		 * 체인이 atomic notifier 이기 때문이다.
		 * 설정자/읽는 자: brcm_register_die_notifiers() 와 그 짝.
		 * 값 범위: has_err_report 인 칩에서만 등록된다.
		 * 동기화: 커널이 관리한다. */
	struct notifier_block	panic_notifier;
		/* [한국어] 오류 기록 레지스터와 bridge_in_reset 를 함께 지키는 스핀락.
		 * 설정자: brcm_pcie_probe() 가 has_err_report 인 칩에서만, 알림을 걸기
		 *          직전에 spin_lock_init() 한다.
		 * 읽는 자: brcm_pcie_bridge_sw_init_set() 과 brcm_pcie_dump_err().
		 * 값 범위: 스핀락이어야 하는 이유는 dump_err 가 die/panic 문맥에서
		 *          불려 잠들 수 없기 때문이다. irqsave 판을 쓰는 것도 그
		 *          문맥에서 인터럽트 상태를 보존해야 해서다.
		 * 동기화: has_err_report 가 거짓인 칩에서는 초기화되지 않으므로,
		 *          이 락을 잡는 두 곳 모두 같은 조건으로 갈린다. */
	spinlock_t		bridge_lock;
};

/* [한국어]
 * is_bmips - 이 SoC 가 BMIPS 계열인지 판정한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: BCM7435 또는 BCM7425 면 true.
 *
 * BMIPS 는 Broadcom 의 MIPS 기반 STB 계열이다. 이 판정이 실제로 갈라 놓는
 * 자리는 이 파일에서 네 곳뿐이다.
 *   - brcm_pcie_set_outbound_win(): 바깥 창의 상위 주소 레지스터가 없어
 *     하위 워드만 적고 그대로 끝낸다.
 *   - brcm_pcie_setup() 의 SerDes IDDQ: 그 비트의 자리가 다르다.
 *   - brcm_pcie_setup() 의 버스트 크기: 256 바이트를 고른다.
 *   - brcm_pcie_setup() 의 바깥 창 열기: 창 하나가 최대 128MB 라 자원을
 *     조각으로 나눠 담는다.
 * BCM7425 의 config 접근이 다른 것은 이 헬퍼가 아니라 별도의 pci_ops 로
 * 갈리며(BCM7435 는 일반 판을 쓴다), MSI 의 legacy 배치 판정도 이 헬퍼가
 * 아니라 hw_rev 비교로 이뤄진다.
 *
 * 변종 이름을 직접 비교하는 곳을 이 한 함수로 모아 두어, 나중에 BMIPS 계열이
 * 늘어도 여기만 고치면 되게 했다.
 *
 * 실행 컨텍스트: 제약 없음. 값 비교뿐이다.
 *
 * 호출 체인:  brcm_pcie_set_outbound_win() / brcm_pcie_setup() → [이 함수]
 */
static inline bool is_bmips(const struct brcm_pcie *pcie)
{
	return pcie->cfg->soc_base == BCM7435 || pcie->cfg->soc_base == BCM7425;
}

/* [한국어]
 * brcm_pcie_bridge_sw_init_set - 브리지 리셋을 걸거나 풀고 그 상태를 기억한다
 *
 * @pcie: 컨트롤러 상태.
 * @val:  1 이면 리셋을 걸고, 0 이면 푼다.
 * @return: 변종별 구현이 돌려준 값. 0 성공, 음수 errno.
 *
 * 변종마다 브리지 리셋 방법이 달라 cfg->bridge_sw_init_set 함수 포인터로
 * 위임한다. 이 함수는 그 위에 두 가지를 덧붙인다.
 *
 *   1) 결과를 pcie->bridge_in_reset 에 기억한다. 상류 주석대로 실패하면
 *      "리셋 상태(꺼짐)" 로 간주하는데, 상태를 모를 때 접근을 막는 쪽이
 *      안전하기 때문이다.
 *   2) 오류 보고 기능이 있는 변종에서만 spinlock 을 잡는다. 그 변종에서는
 *      die/panic 알림 콜백이 인터럽트가 꺼진 문맥에서 bridge_in_reset 을
 *      읽으므로, 갱신 도중의 값을 보면 안 된다.
 *
 * 락을 조건부로 잡는 점이 눈에 띈다 — 오류 보고를 쓰지 않는 변종에서는
 * 그 콜백이 등록되지 않아 경쟁 상대가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. irqsave 로 잡으므로 인터럽트 문맥과도 안전하다.
 *
 * 호출 체인:  brcm_pcie_setup() / brcm_pcie_turn_off() / probe·resume 경로
 *               → [이 함수] → cfg->bridge_sw_init_set()
 */
static int brcm_pcie_bridge_sw_init_set(struct brcm_pcie *pcie, u32 val)
{
	unsigned long flags; /* [한국어] 커널이 이 CPU 의 인터럽트 상태를 저장할 자리 */
	int ret; /* [한국어] 하위 구현이 돌려준 결과. 그대로 호출자에게 올린다 */

	if (pcie->cfg->has_err_report) /* [한국어] 오류 보고 레지스터를 쓰는 칩에서만 락이 필요하다 — 그 칩만 die/panic 콜백이 같은 상태를 읽기 때문 */
		spin_lock_irqsave(&pcie->bridge_lock, flags); /* [한국어] 인터럽트를 끄고 잡는다. 잠들 수 없는 die/panic 문맥에서도 같은 락을 잡으므로 스핀락이어야 한다 */

	ret = pcie->cfg->bridge_sw_init_set(pcie, val); /* [한국어] 변종별 실제 구현을 부른다 */
	/* If we fail, assume the bridge is in reset (off) */
	pcie->bridge_in_reset = ret ? true : val; /* [한국어] 실패했으면 리셋(꺼짐)으로 간주하고, 성공했으면 요청한 값 그대로가 지금 상태다 */

	if (pcie->cfg->has_err_report) /* [한국어] 잡았던 칩에서만 푼다 */
		spin_unlock_irqrestore(&pcie->bridge_lock, flags); /* [한국어] 저장해 둔 인터럽트 상태를 복원하며 놓는다 */

	return ret; /* [한국어] 브리지 조작 결과를 그대로 올린다 */
}

/*
 * This is to convert the size of the inbound "BAR" region to the
 * non-linear values of PCIE_X_MISC_RC_BAR[123]_CONFIG_LO.SIZE
 */
/* [한국어]
 * brcm_pcie_encode_ibar_size - 안쪽 창 크기를 하드웨어의 비선형 인코딩으로 바꾼다
 *
 * @size: 창 크기(바이트).
 * @return: 레지스터에 넣을 인코딩 값. 표현할 수 없는 크기면 0(창 비활성).
 *
 * 상류 주석대로 안쪽 "BAR" 영역의 크기 필드는 값이 선형이 아니다.
 * 두 구간으로 나뉘어 있다.
 *
 *   4KB ~ 32KB   : log2 에서 12 를 빼고 0x1c 를 더한다(0x1c ~ 0x1f).
 *   64KB ~ 64GB  : log2 에서 15 를 뺀다(1 ~ 21).
 *
 * 두 구간이 인코딩 공간에서 떨어져 있어 하나의 식으로 표현할 수 없고,
 * 그래서 분기가 필요하다. 어느 구간에도 들지 않으면 0 을 돌려주는데,
 * 그 값이 곧 "이 창을 쓰지 않음" 을 뜻한다 — 상류 주석의 "disable" 이다.
 *
 * 크기가 2의 거듭제곱이라는 전제가 깔려 있다. ilog2 가 내림이라 그렇지
 * 않으면 조용히 작은 값으로 인코딩된다.
 *
 * 실행 컨텍스트: 제약 없음. 순수 계산이다.
 *
 * 호출 체인:  set_inbound_win_registers() → [이 함수]
 */
static int brcm_pcie_encode_ibar_size(u64 size)
{
	int log2_in = ilog2(size);

	if (log2_in >= 12 && log2_in <= 15)
		/* Covers 4KB to 32KB (inclusive) */
		return (log2_in - 12) + 0x1c;
	else if (log2_in >= 16 && log2_in <= 36)
		/* Covers 64KB to 64GB, (inclusive) */
		return log2_in - 15;
	/* Something is awry so disable */
	return 0;
}

/* [한국어]
 * brcm_pcie_mdio_form_pkt - MDIO 트랜잭션 한 건의 주소 워드를 조립한다
 *
 * @port:  MDIO 포트 번호.
 * @regad: 그 포트 안의 레지스터 주소.
 * @cmd:   MDIO_CMD_READ 또는 MDIO_CMD_WRITE.
 * @return: 주소 레지스터에 쓸 32비트 값.
 *
 * PHY 레지스터가 MDIO 라는 별도 직렬 버스 뒤에 있어, 접근하려면 포트·주소·
 * 명령을 한 워드로 조립해 주소 레지스터에 써야 한다.
 *
 * 포트 번호를 두 필드로 나눠 넣는 점이 요령이다. 상위 비트는
 * MDIO_PORT_EXT_MASK 에 (port >> 4) 로, 하위는 MDIO_PORT_MASK 에 그대로
 * 넣는다. 포트 번호가 한 필드에 들어가지 않아 하드웨어가 쪼개 둔 것이다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  brcm_pcie_mdio_read() / brcm_pcie_mdio_write() → [이 함수]
 */
static u32 brcm_pcie_mdio_form_pkt(int port, int regad, int cmd)
{
	u32 pkt = 0; /* [한국어] 조립할 주소 워드. 비트를 하나씩 얹어 나가므로 0 에서 시작한다 */

	pkt |= FIELD_PREP(MDIO_PORT_EXT_MASK, port >> 4); /* [한국어] 포트 번호가 4비트를 넘을 때의 상위 비트. port >> 4 가 그 초과분이다 */
	pkt |= FIELD_PREP(MDIO_PORT_MASK, port); /* [한국어] 포트 번호의 하위 4비트 */
	pkt |= FIELD_PREP(MDIO_REGAD_MASK, regad); /* [한국어] 접근할 PHY 레지스터 주소 */
	pkt |= FIELD_PREP(MDIO_CMD_MASK, cmd); /* [한국어] 읽기/쓰기 명령 비트 */

	return pkt; /* [한국어] 완성된 워드를 돌려준다. 호출자가 그대로 MDIO 주소 레지스터에 쓴다 */
}

/* negative return value indicates error */
/* [한국어]
 * brcm_pcie_mdio_read - MDIO 로 PHY 레지스터 하나를 읽는다
 *
 * @base:  컨트롤러 레지스터 기준 주소.
 * @port:  MDIO 포트.   @regad: 레지스터 주소.   @val: 읽은 값을 담을 곳.
 * @return: 0 성공, 음수는 완료 비트가 100us 안에 서지 않은 경우
 *          (상류 주석대로 음수가 오류를 뜻한다).
 *
 * 절차가 셋이다.
 *   1) 조립한 주소 워드를 MDIO 주소 레지스터에 쓴다.
 *   2) 그 레지스터를 곧바로 되읽는다. 값을 쓰지 않는 더미 읽기인데,
 *      PCIe 쓰기가 posted 라 실제로 하드웨어에 도달했음을 보장하려는
 *      flush 로 보인다.
 *   3) 읽기 데이터 레지스터의 완료 비트가 설 때까지 10us 간격으로
 *      폴링하고, 데이터 필드를 꺼낸다.
 *
 * 폴링 실패 시에도 *val 을 채운다는 점을 짚어 둔다. 그 값은 의미가 없으므로
 * 호출자가 반환값을 반드시 확인해야 하며, 실제로 모든 호출자가 확인한다.
 *
 * readl_poll_timeout_atomic 을 쓰므로 잠들지 않는다.
 *
 * 실행 컨텍스트: 아토믹. 바쁘게 기다린다.
 *
 * 호출 체인:  brcm_pcie_set_ssc() → [이 함수] → brcm_pcie_mdio_form_pkt()
 */
static int brcm_pcie_mdio_read(void __iomem *base, u8 port, u8 regad, u32 *val)
{
	u32 data; /* [한국어] 폴링이 읽어 담을 원시 워드. 완료 비트와 데이터가 함께 들어 있다 */
	int err; /* [한국어] 폴링 결과. 타임아웃이면 음수다 */

	writel(brcm_pcie_mdio_form_pkt(port, regad, MDIO_CMD_READ),
		   base + PCIE_RC_DL_MDIO_ADDR); /* [한국어] 조립한 주소 워드를 MDIO 주소 레지스터에 쓴다 — 이것이 요청 개시다 */
	readl(base + PCIE_RC_DL_MDIO_ADDR); /* [한국어] 같은 레지스터를 더미로 되읽어 쓰기를 하드웨어까지 밀어낸다(posted write flush) */
	err = readl_poll_timeout_atomic(base + PCIE_RC_DL_MDIO_RD_DATA, data,
					MDIO_RD_DONE(data), 10, 100); /* [한국어] 완료 비트가 설 때까지 10us 간격으로 최대 100us 폴링 */
	*val = FIELD_GET(MDIO_DATA_MASK, data); /* [한국어] 완료 워드에서 데이터 필드만 꺼내 호출자에게 준다 */

	return err; /* [한국어] 폴링 결과를 그대로 올린다. 음수면 위 *val 값은 의미가 없다 */
}

/* negative return value indicates error */
/* [한국어]
 * brcm_pcie_mdio_write - MDIO 로 PHY 레지스터 하나에 쓴다
 *
 * @base:   컨트롤러 레지스터 기준 주소.
 * @port:   MDIO 포트.   @regad: 레지스터 주소.   @wrdata: 쓸 16비트 값.
 * @return: 0 성공, 음수는 완료 비트가 100us 안에 서지 않은 경우.
 *
 * 읽기 판과 절차가 대칭이다. 주소를 쓰고 더미 읽기로 밀어낸 뒤, 데이터
 * 레지스터에 값을 쓰고 완료를 기다린다.
 *
 * 데이터를 쓸 때 MDIO_DATA_DONE_MASK 를 함께 얹는 것이 요점이다. 그 비트가
 * "이제 전송하라" 는 신호이면서, 동시에 완료 판정에도 쓰인다 — 하드웨어가
 * 전송을 마치면 그 비트를 내리고, 폴링이 그것을 본다.
 *
 * 실행 컨텍스트: 아토믹.
 *
 * 호출 체인:  brcm_pcie_set_ssc() → [이 함수] → brcm_pcie_mdio_form_pkt()
 */
static int brcm_pcie_mdio_write(void __iomem *base, u8 port,
				u8 regad, u16 wrdata)
{
	u32 data; /* [한국어] 폴링이 읽어 담을 원시 워드 */
	int err; /* [한국어] 폴링 결과 */

	writel(brcm_pcie_mdio_form_pkt(port, regad, MDIO_CMD_WRITE),
		   base + PCIE_RC_DL_MDIO_ADDR); /* [한국어] 쓰기 명령으로 조립한 주소 워드를 먼저 보낸다 */
	readl(base + PCIE_RC_DL_MDIO_ADDR); /* [한국어] 더미 읽기로 밀어낸다 */
	writel(MDIO_DATA_DONE_MASK | wrdata, base + PCIE_RC_DL_MDIO_WR_DATA); /* [한국어] DONE 비트를 얹어 데이터를 쓴다 — 그 비트가 "전송 개시" 신호를 겸한다 */

	err = readl_poll_timeout_atomic(base + PCIE_RC_DL_MDIO_WR_DATA, data,
					MDIO_WT_DONE(data), 10, 100); /* [한국어] DONE 비트가 내려갈 때까지 폴링. 쓰기는 완료 시 비트가 내려가므로 판정 매크로가 읽기와 반대다 */
	return err; /* [한국어] 폴링 결과를 그대로 올린다 */
}

/*
 * Configures device for Spread Spectrum Clocking (SSC) mode; a negative
 * return value indicates error.
 */
/* [한국어]
 * brcm_pcie_set_ssc - 확산 스펙트럼 클럭(SSC) 모드를 켠다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공(SSC 와 PLL 잠금이 모두 확인됨). -EIO 는 둘 중 하나가
 *          잡히지 않은 경우, 그 밖의 음수는 MDIO 접근 실패.
 *
 * SSC 는 클럭 주파수를 미세하게 흔들어 전자파 방출(EMI)의 첨두값을 낮추는
 * 기법이다. 규격이 허용하는 범위 안에서 흔들므로 링크 동작에는 지장이 없다.
 *
 * 절차는 MDIO 를 통한 PHY 레지스터 조작 넷이다.
 *   1) 주소 오프셋 레지스터에 SSC 레지스터 묶음의 기준을 써 창을 옮긴다.
 *   2) 제어 레지스터를 읽어 오버라이드 활성과 값 비트를 세워 되쓴다.
 *   3) 1~2ms 기다린다. PLL 이 새 설정으로 다시 잠기는 데 시간이 걸린다.
 *   4) 상태 레지스터에서 SSC 동작 여부와 PLL 잠금 여부를 함께 확인한다.
 *
 * 마지막 판정이 두 조건의 AND 인 것이 중요하다. SSC 가 켜졌어도 PLL 이
 * 잠기지 않았으면 클럭이 불안정하다는 뜻이라 실패로 봐야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 호출 체인:  brcm_pcie_start_link() → [이 함수]
 *               → brcm_pcie_mdio_write() / brcm_pcie_mdio_read()
 */
static int brcm_pcie_set_ssc(struct brcm_pcie *pcie)
{
	int pll, ssc; /* [한국어] 상태 레지스터에서 꺼낼 PLL 잠금 여부와 SSC 동작 여부 */
	int ret; /* [한국어] MDIO 접근 결과 */
	u32 tmp; /* [한국어] MDIO 로 읽고 고쳐 쓸 임시 값 */

	ret = brcm_pcie_mdio_write(pcie->base, MDIO_PORT0, SET_ADDR_OFFSET,
				   SSC_REGS_ADDR); /* [한국어] 주소 오프셋 레지스터에 SSC 레지스터 묶음의 기준을 써서 창을 옮긴다 */
	if (ret < 0) /* [한국어] MDIO 가 실패하면 더 진행할 수 없다 */
		return ret;

	ret = brcm_pcie_mdio_read(pcie->base, MDIO_PORT0,
				  SSC_CNTL_OFFSET, &tmp); /* [한국어] 옮긴 창에서 SSC 제어 레지스터를 읽는다 */
	if (ret < 0) /* [한국어] 읽기 실패도 그대로 올린다 */
		return ret;

	u32p_replace_bits(&tmp, 1, SSC_CNTL_OVRD_EN_MASK); /* [한국어] 오버라이드 활성 비트를 세운다 — 이것이 있어야 아래 값 비트가 반영된다 */
	u32p_replace_bits(&tmp, 1, SSC_CNTL_OVRD_VAL_MASK); /* [한국어] 오버라이드 값 비트를 세워 SSC 를 켠다 */
	ret = brcm_pcie_mdio_write(pcie->base, MDIO_PORT0,
				   SSC_CNTL_OFFSET, tmp); /* [한국어] 고친 값을 같은 제어 레지스터에 되쓴다 */
	if (ret < 0) /* [한국어] 쓰기 실패면 SSC 가 켜지지 않은 것이다 */
		return ret;

	usleep_range(1000, 2000); /* [한국어] PLL 이 새 설정으로 다시 잠길 시간을 준다 */
	ret = brcm_pcie_mdio_read(pcie->base, MDIO_PORT0,
				  SSC_STATUS_OFFSET, &tmp); /* [한국어] 상태 레지스터를 읽어 실제로 켜졌는지 확인한다 */
	if (ret < 0) /* [한국어] 확인조차 못 했으면 그 오류를 올린다 */
		return ret;

	ssc = FIELD_GET(SSC_STATUS_SSC_MASK, tmp); /* [한국어] SSC 가 실제로 동작 중인지 */
	pll = FIELD_GET(SSC_STATUS_PLL_LOCK_MASK, tmp); /* [한국어] PLL 이 잠겼는지 */

	return ssc && pll ? 0 : -EIO; /* [한국어] 둘 다여야 성공. 하나라도 아니면 클럭이 불안정하다는 뜻이라 -EIO */
}

/* Limits operation to a specific generation (1, 2, or 3) */
/* [한국어]
 * brcm_pcie_set_gen - 링크 세대를 특정 값으로 제한한다
 *
 * @pcie: 컨트롤러 상태.
 * @gen:  1, 2, 3 중 하나(상류 주석).
 * @return: 없음.
 *
 * 두 레지스터를 함께 고쳐야 실제로 제한이 걸린다.
 *   Link Capability 의 Supported Link Speeds - "내가 낼 수 있는 최고 속도"
 *   Link Control 2 의 Target Link Speed      - "이번에 협상할 목표 속도"
 *
 * 능력만 낮추면 이미 협상된 링크가 그대로 남고, 목표만 낮추면 상대가
 * 능력을 보고 더 높은 속도를 제안할 수 있다. 그래서 둘 다 고친다.
 *
 * 읽기 폭이 다른 점을 눈여겨볼 만하다. Link Capability 는 32비트(readl),
 * Link Control 2 는 16비트(readw)라 u32p_replace_bits 와 u16p_replace_bits 를
 * 각각 쓴다.
 *
 * 값 검증이 없다. gen 이 범위를 벗어나면 필드가 잘려 엉뚱한 속도가 되지만,
 * 호출자가 DT 에서 읽은 값을 그대로 넘기므로 DT 를 신뢰하는 구조다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  brcm_pcie_start_link() → [이 함수]
 */
static void brcm_pcie_set_gen(struct brcm_pcie *pcie, int gen)
{
	u16 lnkctl2 = readw(pcie->base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2); /* [한국어] 표준 config 공간의 링크 제어 2 레지스터. 목표 링크 속도 필드가 여기 있다 */
	u32 lnkcap = readl(pcie->base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY); /* [한국어] 벤더 전용 링크 능력 레지스터. 광고할 최대 속도가 여기 있다 */

	u32p_replace_bits(&lnkcap, gen, PCI_EXP_LNKCAP_SLS); /* [한국어] 능력 쪽의 지원 속도 필드를 요청 세대로 바꾼다 */
	writel(lnkcap, pcie->base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY); /* [한국어] 바꾼 능력 값을 되쓴다 — 이제 이 세대까지만 광고한다 */

	u16p_replace_bits(&lnkctl2, gen, PCI_EXP_LNKCTL2_TLS); /* [한국어] 제어 쪽의 목표 링크 속도 필드도 같은 세대로 맞춘다 */
	writew(lnkctl2, pcie->base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2); /* [한국어] 바꾼 제어 값을 되쓴다. 능력과 목표를 함께 맞춰야 협상이 그 세대로 수렴한다 */
}

/* [한국어]
 * brcm_pcie_set_outbound_win - 바깥 방향 주소 창 하나를 연다
 *
 * @pcie:      컨트롤러 상태.
 * @win:       창 번호.
 * @cpu_addr:  CPU 쪽 시작 주소.
 * @pcie_addr: 그에 대응하는 PCI 쪽 주소.
 * @size:      창 크기.
 * @return: 없음.
 *
 * CPU 가 PCI 주소 공간에 접근하는 통로를 만든다. 레지스터가 네 묶음으로
 * 나뉘어 있고, 그 배치가 이 함수의 구조를 정한다.
 *
 *   MEM_WIN0_LO/HI       - PCI 쪽 주소(하위/상위 32비트)
 *   MEM_WIN0_BASE_LIMIT  - CPU 쪽 base 와 limit 의 하위 부분. 한 워드에
 *                          두 필드가 들어 있어 읽고-고쳐-쓰기를 한다.
 *   MEM_WIN0_BASE_HI     - CPU base 의 상위 부분
 *   MEM_WIN0_LIMIT_HI    - CPU limit 의 상위 부분
 *
 * CPU 주소를 1MB 단위로 나눠 넣는 점이 요점이다. base/limit 필드가 MB
 * 단위라 창 정렬이 1MB 이며, limit 는 마지막 바이트 주소를 MB 로 내린 값이다.
 *
 * 상위 부분을 얼마나 밀어야 하는지를 HWEIGHT32 로 구하는 대목이 영리하다 —
 * 하위 필드 마스크에 선 비트 수가 곧 하위가 담는 비트 폭이므로, 그만큼
 * 오른쪽으로 밀면 상위 필드에 넣을 값이 나온다. 마스크가 바뀌어도 이
 * 계산은 그대로 맞는다.
 *
 * BMIPS 계열은 상위 레지스터가 없어 하위까지만 쓰고 돌아간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(초기화).
 *
 * 호출 체인:  brcm_pcie_setup() → [이 함수]
 */
static void brcm_pcie_set_outbound_win(struct brcm_pcie *pcie,
				       u8 win, u64 cpu_addr,
				       u64 pcie_addr, u64 size)
{
	u32 cpu_addr_mb_high, limit_addr_mb_high; /* [한국어] CPU base/limit 의 상위 비트(MB 단위). BMIPS 계열에서는 쓰이지 않는다 */
	phys_addr_t cpu_addr_mb, limit_addr_mb; /* [한국어] CPU base/limit 를 MB 단위로 나눈 값. 레지스터가 MB 단위를 받는다 */
	int high_addr_shift; /* [한국어] 하위 필드가 이미 담은 비트 수. 그만큼 밀어야 상위 비트가 나온다 */
	u32 tmp; /* [한국어] 레지스터를 읽고 고쳐 쓸 때 쓰는 임시 값 */

	/* Set the base of the pcie_addr window */
	writel(lower_32_bits(pcie_addr), pcie->base + PCIE_MEM_WIN0_LO(win)); /* [한국어] PCI 쪽 시작 주소의 하위 32비트 */
	writel(upper_32_bits(pcie_addr), pcie->base + PCIE_MEM_WIN0_HI(win)); /* [한국어] 같은 주소의 상위 32비트. 두 워드로 64비트 주소를 만든다 */

	/* Write the addr base & limit lower bits (in MBs) */
	cpu_addr_mb = cpu_addr / SZ_1M; /* [한국어] CPU 시작 주소를 MB 단위로 바꾼다 */
	limit_addr_mb = (cpu_addr + size - 1) / SZ_1M; /* [한국어] CPU 끝 주소를 MB 단위로. size-1 을 더하는 것은 끝이 포함 경계이기 때문이다 */

	tmp = readl(pcie->base + PCIE_MEM_WIN0_BASE_LIMIT(win)); /* [한국어] base 와 limit 가 한 워드를 나눠 쓰므로 먼저 읽어 온다 */
	u32p_replace_bits(&tmp, cpu_addr_mb,
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK); /* [한국어] 그 워드의 base 필드를 채운다 */
	u32p_replace_bits(&tmp, limit_addr_mb,
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_MASK); /* [한국어] 같은 워드의 limit 필드를 채운다 */
	writel(tmp, pcie->base + PCIE_MEM_WIN0_BASE_LIMIT(win)); /* [한국어] 두 필드를 한 번에 되쓴다 */

	if (is_bmips(pcie)) /* [한국어] BMIPS 계열은 상위 비트 레지스터가 없어 여기서 끝낸다 */
		return; /* [한국어] 창 크기가 128MB 로 제한되어 상위 비트가 필요 없다 */

	/* Write the cpu & limit addr upper bits */
	high_addr_shift =
		HWEIGHT32(PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK); /* [한국어] base 필드의 비트 수를 세어 shift 량으로 쓴다 — 하위 워드가 담은 만큼 밀어야 나머지가 나온다 */

	cpu_addr_mb_high = cpu_addr_mb >> high_addr_shift; /* [한국어] CPU base 의 상위 비트를 뽑는다 */
	tmp = readl(pcie->base + PCIE_MEM_WIN0_BASE_HI(win)); /* [한국어] 상위 비트 레지스터를 읽어 온다 */
	u32p_replace_bits(&tmp, cpu_addr_mb_high,
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI_BASE_MASK); /* [한국어] 그 워드의 base 상위 필드를 채운다 */
	writel(tmp, pcie->base + PCIE_MEM_WIN0_BASE_HI(win)); /* [한국어] 되쓴다 */

	limit_addr_mb_high = limit_addr_mb >> high_addr_shift; /* [한국어] CPU limit 의 상위 비트를 같은 방식으로 뽑는다 */
	tmp = readl(pcie->base + PCIE_MEM_WIN0_LIMIT_HI(win)); /* [한국어] limit 상위 레지스터를 읽어 온다 */
	u32p_replace_bits(&tmp, limit_addr_mb_high,
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI_LIMIT_MASK); /* [한국어] limit 상위 필드를 채운다 */
	writel(tmp, pcie->base + PCIE_MEM_WIN0_LIMIT_HI(win)); /* [한국어] 되쓴다. 이로써 이 창의 64비트 CPU 범위가 완성된다 */
}

/* [한국어] 이 MSI 도메인이 상위 계층에 "반드시 이렇게 다뤄 달라" 고 요구하는 플래그.
 * USE_DEF_DOM_OPS/USE_DEF_CHIP_OPS 는 도메인과 irq_chip 의 빈 자리를 코어의
 * 기본 구현으로 채워 달라는 뜻이고, NO_AFFINITY 는 이 하드웨어가 벡터별
 * CPU 지정(어피니티)을 지원하지 않음을 알린다 — 실제로 이 파일의 irq_chip 에
 * set_affinity 콜백이 없다. */
#define BRCM_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				 MSI_FLAG_USE_DEF_CHIP_OPS	| \
				 MSI_FLAG_NO_AFFINITY)

/* [한국어] 이 도메인이 지원할 수 있는 플래그의 상한.
 * MSI_GENERIC_FLAGS_MASK 로 일반 플래그를 모두 허용하고, 여기에
 * MULTI_PCI_MSI 를 더해 multi-MSI(한 장치가 연속 벡터 여럿을 쓰는 방식)를
 * 받아들인다 — brcm_msi_alloc() 이 bitmap_find_free_region 으로 연속·정렬된
 * 자리를 찾는 것이 그 지원의 실체다. */
#define BRCM_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK	| \
				  MSI_FLAG_MULTI_PCI_MSI)

/* [한국어] 상위 MSI 계층(irq-msi-lib)이 이 도메인을 다룰 때 쓰는 규약 묶음.
 * msi_create_parent_irq_domain() 에 함께 넘겨, 부모 도메인 위에 PCI/MSI 용
 * 자식 도메인을 만들 때 이 값들이 쓰인다. */
static const struct msi_parent_ops brcm_msi_parent_ops = {
	.required_flags		= BRCM_MSI_FLAGS_REQUIRED, /* [한국어] 반드시 적용할 플래그 */
	.supported_flags	= BRCM_MSI_FLAGS_SUPPORTED, /* [한국어] 허용 가능한 플래그의 상한 */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI, /* [한국어] 이 도메인이 PCI MSI 버스용임을 알리는 토큰. 코어가 도메인을 고를 때 이것으로 맞춘다 */
	.chip_flags		= MSI_CHIP_FLAG_SET_ACK, /* [한국어] 이 irq_chip 이 ack 콜백을 갖고 있으니 써 달라는 표시. brcm_msi_ack_irq 가 그 대상이다 */
	.prefix			= "BRCM-", /* [한국어] /proc/interrupts 등에 보일 이름 앞머리 */
	.init_dev_msi_info	= msi_lib_init_dev_msi_info, /* [한국어] 자식 도메인 정보를 채우는 표준 구현을 그대로 쓴다 */
};

/* [한국어]
 * brcm_pcie_msi_isr - 내장 MSI 컨트롤러의 체인 인터럽트 핸들러
 *
 * @desc: 상위 IRQ 의 descriptor. handler_data 로 brcm_msi 를 되찾는다.
 * @return: 없음.
 *
 * MSI 상태 레지스터의 선 비트를 훑어 각 벡터를 도메인으로 넘긴다.
 *
 * legacy_shift 만큼 오른쪽으로 미는 것이 요점이다. 구조체 주석이 밝히듯
 * 일부 칩은 MSI 비트가 공유 레지스터의 [31..24] 자리에 있어, 그 경우
 * shift 로 0번부터 시작하도록 맞춰 준다. 그렇지 않은 칩은 shift 가 0 이라
 * 아무 영향이 없다.
 *
 * for_each_set_bit 의 상한이 msi->nr 인 것도 같은 이유다 — 칩마다 실제
 * 쓸 수 있는 MSI 수가 달라 그 범위만 훑는다.
 *
 * 상태 비트를 여기서 지우지 않는다. 지우는 일은 irq_chip 의 ack 콜백
 * (brcm_msi_ack_irq)이 맡으며, handle_edge_irq 가 핸들러를 부르기 전에
 * 그것을 호출한다.
 *
 * 실행 컨텍스트: 인터럽트. chained_irq_enter/exit 사이에서 상위 컨트롤러가
 * 이 선을 마스크해 두므로 재진입이 없다.
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → generic_handle_domain_irq()
 *               → 장치 드라이버의 MSI 핸들러
 */
static void brcm_pcie_msi_isr(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc); /* [한국어] 상위 인터럽트 컨트롤러의 irq_chip. chained_irq_enter/exit 가 이것으로 선을 마스크한다 */
	unsigned long status; /* [한국어] 상태 레지스터에서 읽은 대기 비트들. for_each_set_bit 가 unsigned long 을 요구한다 */
	struct brcm_msi *msi; /* [한국어] 이 컨트롤러의 MSI 상태 */
	struct device *dev; /* [한국어] 로그용 device */
	u32 bit; /* [한국어] 훑는 중인 벡터 번호 */

	chained_irq_enter(chip, desc); /* [한국어] 상위 선을 마스크하고 ack 해 이 핸들러가 도는 동안 재진입이 없게 한다 */
	msi = irq_desc_get_handler_data(desc); /* [한국어] 등록 시 함께 넘긴 brcm_msi 를 되찾는다 */
	dev = msi->dev; /* [한국어] 로그에 쓸 device 를 꺼내 둔다 */

	status = readl(msi->intr_base + MSI_INT_STATUS); /* [한국어] 대기 중인 MSI 비트를 한 번에 읽는다 */
	status >>= msi->legacy_shift; /* [한국어] legacy 칩은 벡터가 상위 바이트에 있어, 0번부터 시작하도록 내린다 */

	for_each_set_bit(bit, &status, msi->nr) { /* [한국어] 선 비트만 골라 훑는다. 상한이 msi->nr 인 것은 칩마다 벡터 수가 다르기 때문 */
		int ret; /* [한국어] 도메인 전달 결과 */
		ret = generic_handle_domain_irq(msi->inner_domain, bit); /* [한국어] 해당 벡터의 가상 IRQ 핸들러를 부른다. 그 안에서 ack 콜백이 먼저 불려 비트가 지워진다 */
		if (ret) /* [한국어] 등록되지 않은 벡터가 올라온 경우 */
			dev_dbg(dev, "unexpected MSI\n"); /* [한국어] 디버그 로그만 남긴다 — 실제로 일어나면 하드웨어가 예상 밖 벡터를 올린 것이다 */
	}

	chained_irq_exit(chip, desc); /* [한국어] 상위 선의 마스크를 풀어 다음 인터럽트를 받게 한다 */
}

/* [한국어]
 * brcm_msi_compose_msi_msg - 장치가 MSI 를 보낼 주소와 데이터를 알려 준다
 *
 * @data: IRQ 코어가 넘기는 irq_data. hwirq 가 벡터 번호다.
 * @msg:  채워 줄 주소/데이터.
 * @return: 없음.
 *
 * 주소는 msi->target_addr 를 그대로 쓴다. 그 값은 brcm_pcie_enable_msi() 가
 * 정해 하드웨어의 MSI BAR 에도 함께 써 둔 것이라, 장치가 그 주소로 쓰면
 * 컨트롤러가 MSI 로 해석한다.
 *
 * 데이터가 두 부분으로 조립된다. 상위는 PCIE_MISC_MSI_DATA_CONFIG_VAL_32 의
 * 하위 16비트를 쓰고, 하위에 hwirq 를 얹는다. 그 상수의 상위 절반이
 * "이 값과 일치해야 MSI 로 인정한다" 는 매칭 패턴이고 하위 절반이 벡터
 * 번호가 들어갈 자리로 보이나, 그 비트 배치의 정확한 근거는 이 트리에서
 * 확인 못 함. 다만 brcm_msi_set_regs() 가 같은 상수를 하드웨어의
 * MSI_DATA_CONFIG 레지스터에 써 넣는다는 점은 확인했다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(벡터 할당 시). 잠들지 않는다.
 *
 * 호출 체인:  MSI 코어 → [이 함수]
 */
static void brcm_msi_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct brcm_msi *msi = irq_data_get_irq_chip_data(data); /* [한국어] irq_domain_set_info 가 chip_data 에 넣어 둔 brcm_msi 를 되찾는다 */

	msg->address_lo = lower_32_bits(msi->target_addr); /* [한국어] 장치가 쓸 목적지 주소의 하위 32비트 */
	msg->address_hi = upper_32_bits(msi->target_addr); /* [한국어] 같은 주소의 상위 32비트 */
	msg->data = (0xffff & PCIE_MISC_MSI_DATA_CONFIG_VAL_32) | data->hwirq; /* [한국어] 매칭 상수의 하위 16비트에 벡터 번호를 얹어 데이터 워드를 만든다 */
}

/* [한국어]
 * brcm_msi_ack_irq - 이 MSI 벡터의 대기 비트를 지운다
 *
 * @data: IRQ 코어가 넘기는 irq_data.
 * @return: 없음.
 *
 * MSI_INT_CLR 레지스터의 해당 비트에 1 을 써서 지운다. 쓰기만으로 지워지는
 * 방식이라 읽고-고쳐-쓰기가 필요 없고, 따라서 락도 필요 없다.
 *
 * 자리를 구할 때 legacy_shift 를 더하는 것이 isr 에서 뺀 것과 짝을 이룬다 —
 * isr 은 하드웨어 비트를 논리 번호로 내렸고, 여기서는 논리 번호를 다시
 * 하드웨어 비트로 올린다.
 *
 * msi_parent_ops 의 chip_flags 에 MSI_CHIP_FLAG_SET_ACK 가 있어 상위 MSI
 * 계층이 이 ack 를 쓰도록 표시되어 있고, 도메인이 handle_edge_irq 를
 * 지정하므로 핸들러 전에 불린다.
 *
 * 실행 컨텍스트: 인터럽트. 잠들지 않는다.
 *
 * 호출 체인:  IRQ 코어(handle_edge_irq) → [이 함수] → writel()
 */
static void brcm_msi_ack_irq(struct irq_data *data)
{
	struct brcm_msi *msi = irq_data_get_irq_chip_data(data); /* [한국어] chip_data 에서 이 컨트롤러의 MSI 상태를 되찾는다 */
	const int shift_amt = data->hwirq + msi->legacy_shift; /* [한국어] 논리 벡터 번호를 하드웨어 비트 자리로 올린다 — isr 이 내린 것과 짝을 이룬다 */

	writel(1 << shift_amt, msi->intr_base + MSI_INT_CLR); /* [한국어] 그 비트에 1 을 써서 지운다. 쓰기만으로 지워지므로 읽고-고쳐-쓰기가 필요 없고 락도 없다 */
}


/* [한국어] 이 MSI 벡터들의 irq_chip.
 * 콜백이 둘뿐인 것이 이 하드웨어의 성격을 그대로 보여 준다 — 벡터별
 * 마스크도, 어피니티 설정도 없고, 주소/데이터를 알려 주는 일과 대기 비트를
 * 지우는 일만 한다. 나머지 콜백은 MSI_FLAG_USE_DEF_CHIP_OPS 에 따라 코어의
 * 기본 구현으로 채워진다. */
static struct irq_chip brcm_msi_bottom_irq_chip = {
	.name			= "BRCM STB MSI", /* [한국어] /proc/interrupts 에 보일 이름 */
	.irq_compose_msi_msg	= brcm_msi_compose_msi_msg, /* [한국어] 장치에 알려 줄 MSI 주소/데이터를 만드는 콜백 */
	.irq_ack                = brcm_msi_ack_irq, /* [한국어] 핸들러 직전에 대기 비트를 지우는 콜백. handle_edge_irq 가 부른다 */
};

/* [한국어]
 * brcm_msi_alloc - 비트맵에서 연속된 MSI 벡터를 잡는다
 *
 * @msi:     MSI 컨트롤러 상태.
 * @nr_irqs: 요청 개수.
 * @return: 시작 hwirq. 자리가 없으면 음수.
 *
 * bitmap_find_free_region 에 order_base_2(nr_irqs) 를 넘긴다. multi-MSI 는
 * 벡터가 연속이고 개수가 2의 거듭제곱이며 시작이 그 크기에 정렬되어야
 * 하는데, 이 한 호출이 세 조건을 모두 만족하는 자리를 찾아 준다.
 *
 * msi->nr 을 상한으로 쓰므로 칩이 실제로 지원하는 벡터 수를 넘지 않는다.
 *
 * 뮤텍스로 감싼다. 벡터 할당은 프로세스 컨텍스트에서만 일어나므로 잠들 수
 * 있는 락으로 충분하며, 구조체 주석대로 이 락이 alloc/free 를 보호한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  brcm_irq_domain_alloc() → [이 함수] → bitmap_find_free_region()
 */
static int brcm_msi_alloc(struct brcm_msi *msi, unsigned int nr_irqs)
{
	int hwirq; /* [한국어] 찾은 시작 벡터 번호. 자리가 없으면 음수가 들어온다 */

	mutex_lock(&msi->lock); /* [한국어] 비트맵을 고치므로 잡는다. 프로세스 컨텍스트뿐이라 뮤텍스로 충분하다 */
	hwirq = bitmap_find_free_region(msi->used, msi->nr,
					order_base_2(nr_irqs)); /* [한국어] 2의 거듭제곱 크기로 정렬된 연속 자리를 찾아 표시한다 — multi-MSI 의 세 조건을 한 번에 만족시킨다 */
	mutex_unlock(&msi->lock); /* [한국어] 표시가 끝났으므로 곧바로 놓는다 */

	return hwirq; /* [한국어] 시작 벡터 번호 또는 음수 오류를 그대로 올린다 */
}

/* [한국어]
 * brcm_msi_free - 잡아 두었던 MSI 벡터를 비트맵에 되돌린다
 *
 * @msi:     MSI 컨트롤러 상태.
 * @hwirq:   반납할 시작 벡터 번호.
 * @nr_irqs: 개수.
 * @return: 없음.
 *
 * alloc 의 짝이다. order 계산이 alloc 과 같아야 정확히 같은 범위가 풀리며,
 * 그래서 호출자가 반드시 대표 hwirq(할당받은 시작 번호)로 불러야 한다.
 *
 * 같은 뮤텍스로 보호한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  brcm_irq_domain_free() → [이 함수] → bitmap_release_region()
 */
static void brcm_msi_free(struct brcm_msi *msi, unsigned long hwirq,
			  unsigned int nr_irqs)
{
	mutex_lock(&msi->lock); /* [한국어] 비트맵을 고치므로 잡는다 */
	bitmap_release_region(msi->used, hwirq, order_base_2(nr_irqs)); /* [한국어] alloc 과 같은 order 로 풀어야 정확히 같은 범위가 비워진다 */
	mutex_unlock(&msi->lock); /* [한국어] 놓는다 */
}

/* [한국어]
 * brcm_irq_domain_alloc - MSI 벡터를 잡아 virq 들에 연결한다
 *
 * @domain:  MSI 도메인.
 * @virq:    시작 가상 IRQ 번호.
 * @nr_irqs: 요청 개수.
 * @args:    쓰지 않는다.
 * @return: 0 성공, 음수는 자리가 없는 경우.
 *
 * 비트맵에서 자리를 잡고, 요청 개수만큼 virq 와 hwirq 를 하나씩 짝지어
 * 등록한다.
 *
 * irq_domain_set_info 에 넘기는 인자들이 이 도메인의 성격을 정한다 —
 * irq_chip 은 brcm_msi_bottom_irq_chip(ack 와 compose 만 있다),
 * chip_data 는 host_data(즉 brcm_msi)라 콜백들이 문맥을 되찾고,
 * 흐름 제어는 handle_edge_irq 다. MSI 가 본질적으로 edge 이기 때문이다.
 *
 * 실패 시 되돌릴 것이 없다 — 비트맵 할당이 실패하면 아무것도 잡지 않은
 * 상태이므로 그대로 돌아가면 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  MSI 코어 → [이 함수] → brcm_msi_alloc() → irq_domain_set_info()
 */
static int brcm_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				 unsigned int nr_irqs, void *args)
{
	struct brcm_msi *msi = domain->host_data; /* [한국어] 도메인 생성 시 넣어 둔 brcm_msi 를 되찾는다 */
	int hwirq, i; /* [한국어] 잡은 시작 벡터 번호와 루프 인덱스 */

	hwirq = brcm_msi_alloc(msi, nr_irqs); /* [한국어] 비트맵에서 연속된 자리를 잡는다 */

	if (hwirq < 0) /* [한국어] 자리가 없으면 */
		return hwirq; /* [한국어] 아직 아무것도 잡지 않은 상태라 되돌릴 것 없이 오류를 올린다 */

	for (i = 0; i < nr_irqs; i++) /* [한국어] 요청 개수만큼 virq 와 hwirq 를 하나씩 짝짓는다 */
		irq_domain_set_info(domain, virq + i, (irq_hw_number_t)hwirq + i,
				    &brcm_msi_bottom_irq_chip, domain->host_data,
				    handle_edge_irq, NULL, NULL); /* [한국어] irq_chip 은 ack/compose 둘뿐인 이 파일의 chip, chip_data 는 콜백이 되찾을 msi, 흐름 제어는 MSI 의 성격에 맞는 edge 처리다 */
	return 0; /* [한국어] 전부 등록했다 */
}

/* [한국어]
 * brcm_irq_domain_free - 벡터를 반납한다
 *
 * @domain:  MSI 도메인.
 * @virq:    시작 가상 IRQ 번호.
 * @nr_irqs: 개수.
 * @return: 없음.
 *
 * virq 로 irq_data 를 얻어 hwirq 를 알아낸 뒤 비트맵에 되돌린다.
 * chip_data 에서 brcm_msi 를 꺼내는데, alloc 이 거기에 host_data 를
 * 넣어 두었기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  MSI 코어 → [이 함수] → brcm_msi_free()
 */
static void brcm_irq_domain_free(struct irq_domain *domain,
				 unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq); /* [한국어] 대표 virq 의 irq_data 를 얻어 hwirq 를 알아낸다 */
	struct brcm_msi *msi = irq_data_get_irq_chip_data(d); /* [한국어] alloc 이 chip_data 에 넣어 둔 brcm_msi 를 꺼낸다 */

	brcm_msi_free(msi, d->hwirq, nr_irqs); /* [한국어] 그 시작 번호로 비트맵을 되돌린다 */
}

/* [한국어] 이 MSI 도메인의 벡터 할당/반납 콜백 표.
 * brcm_allocate_domains() 가 irq_domain_info.ops 로 넘긴다. 두 콜백만 있는
 * 것은 이 도메인이 하는 일이 비트맵 관리와 virq↔hwirq 연결뿐이기 때문이다. */
static const struct irq_domain_ops msi_domain_ops = {
	.alloc	= brcm_irq_domain_alloc, /* [한국어] 벡터를 잡아 virq 에 연결하는 콜백 */
	.free	= brcm_irq_domain_free, /* [한국어] 그 반대 */
};

/* [한국어]
 * brcm_allocate_domains - 이 컨트롤러의 MSI irq_domain 을 만든다
 *
 * @msi: MSI 컨트롤러 상태.
 * @return: 0 성공, -ENOMEM 은 도메인 생성 실패.
 *
 * msi_create_parent_irq_domain() 이 부모 도메인을 만들고, 그 위에 PCI/MSI 용
 * 자식 도메인을 붙이는 일까지 brcm_msi_parent_ops 를 통해 처리한다.
 *
 * info 에 채우는 네 값의 뜻은 - fwnode 는 MSI 노드(도메인 식별자),
 * ops 는 alloc/free 콜백, host_data 는 콜백들이 되찾을 msi 포인터,
 * size 는 이 칩이 지원하는 벡터 수다.
 *
 * fwnode 로 msi->np 를 쓰는 점을 눈여겨볼 만하다. 일부 칩은 MSI 컨트롤러가
 * PCIe 노드와 별개의 DT 노드를 갖기 때문이며, 그 값은
 * brcm_pcie_enable_msi() 가 정한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  brcm_pcie_enable_msi() → [이 함수] → msi_create_parent_irq_domain()
 */
static int brcm_allocate_domains(struct brcm_msi *msi)
{
	struct device *dev = msi->dev; /* [한국어] 로그용 device 를 꺼내 둔다 */

	struct irq_domain_info info = { /* [한국어] 도메인을 만들 때 코어에 넘길 정보 묶음 */
		.fwnode		= of_fwnode_handle(msi->np), /* [한국어] 도메인 식별자로 MSI 노드를 쓴다. 별도 DT 노드를 갖는 구성을 위해 pcie->np 가 아닌 msi->np 다 */
		.ops		= &msi_domain_ops, /* [한국어] 위에서 정의한 alloc/free 콜백 표 */
		.host_data	= msi, /* [한국어] 콜백들이 domain->host_data 로 되찾을 문맥 */
		.size		= msi->nr, /* [한국어] 이 칩이 지원하는 벡터 수. legacy 칩이면 8, 아니면 32 다 */
	};

	msi->inner_domain = msi_create_parent_irq_domain(&info, &brcm_msi_parent_ops); /* [한국어] 부모 도메인을 만들고 그 위에 PCI/MSI 자식 도메인까지 붙인다 */
	if (!msi->inner_domain) { /* [한국어] 만들지 못했으면 */
		dev_err(dev, "failed to create MSI domain\n"); /* [한국어] 원인이 대개 메모리 부족이라 그렇게 알린다 */
		return -ENOMEM; /* [한국어] 호출자가 이 오류를 그대로 위로 올린다 */
	}

	return 0; /* [한국어] 도메인 준비 완료 */
}

/* [한국어]
 * brcm_free_domains - MSI irq_domain 을 없앤다
 *
 * @msi: MSI 컨트롤러 상태.
 * @return: 없음.
 *
 * irq_domain_remove() 한 줄이다. 계층으로 만든 자식 도메인까지 그 안에서
 * 함께 정리된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  brcm_msi_remove() → [이 함수] → irq_domain_remove()
 */
static void brcm_free_domains(struct brcm_msi *msi)
{
	irq_domain_remove(msi->inner_domain);
}

/* [한국어]
 * brcm_msi_remove - MSI 체인 핸들러를 떼고 도메인을 없앤다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 없음.
 *
 * MSI 를 쓰지 않는 구성이면 pcie->msi 가 NULL 이라 곧바로 돌아간다.
 *
 * 순서가 중요하다. 체인 핸들러를 먼저 떼야 그 뒤 도메인을 없애는 동안
 * 인터럽트가 들어와 사라진 도메인을 참조하는 일이 없다.
 *
 * 하드웨어의 MSI 수신을 끄지는 않는다 — 장치 제거 시점이라 위에서 이미
 * 링크가 내려가거나 곧 내려가기 때문으로 보인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(remove).
 *
 * 호출 체인:  __brcm_pcie_remove() → [이 함수] → brcm_free_domains()
 */
static void brcm_msi_remove(struct brcm_pcie *pcie)
{
	struct brcm_msi *msi = pcie->msi; /* [한국어] MSI 를 쓰는 구성이면 여기에 상태가 들어 있다 */

	if (!msi) /* [한국어] 외부 MSI 컨트롤러를 쓰거나 MSI 를 안 쓰는 보드 */
		return; /* [한국어] 세운 것이 없으니 할 일도 없다 */
	irq_set_chained_handler_and_data(msi->irq, NULL, NULL); /* [한국어] 체인 핸들러를 먼저 뗀다 — 도메인을 없애는 동안 인터럽트가 들어오면 안 된다 */
	brcm_free_domains(msi); /* [한국어] 그다음 도메인을 없앤다 */
}

/* [한국어]
 * brcm_msi_set_regs - MSI 수신 하드웨어를 설정한다
 *
 * @msi: MSI 컨트롤러 상태.
 * @return: 없음.
 *
 * 세 가지를 세운다.
 *
 *   1) 인터럽트 마스크를 풀고 대기 비트를 지운다. legacy 여부에 따라 쓰는
 *      마스크가 다른데, 구조체 주석대로 그런 칩은 MSI 비트가 공유
 *      레지스터의 상위 바이트에 있어 대상 비트가 다르기 때문이다.
 *   2) MSI 수신 주소를 MSI BAR 에 써 넣는다. 하위 워드에 0x1 을 OR 하는데,
 *      상류 주석이 그 이유를 밝힌다 — PCIE_MISC_MSI_BAR_CONFIG_LO 의 0번
 *      비트는 주소가 아니라 MSI 활성 비트로 전용되었고, 그것을 1 로 세운다.
 *      목표 주소가 4 바이트 정렬(BRCM_MSI_TARGET_ADDR_ 계열 상수가 모두
 *      ...fc 로 끝난다)이라 그 자리가 비어 있어 가능한 방식이다.
 *      compose_msi_msg 가 장치에 알려 줄 주소에는 이 비트를 얹지 않는다.
 *   3) MSI 데이터 매칭 값을 쓴다. legacy 면 8비트 판, 아니면 32비트 판이며,
 *      compose_msi_msg 가 장치에 알려 주는 데이터의 상위 절반과 같은
 *      상수다 — 그래서 장치가 보낸 값이 하드웨어의 기대와 맞는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 또는 resume).
 *
 * 호출 체인:  brcm_pcie_enable_msi() / brcm_pcie_resume_noirq() → [이 함수]
 */
static void brcm_msi_set_regs(struct brcm_msi *msi)
{
	u32 val = msi->legacy ? BRCM_INT_PCI_MSI_LEGACY_MASK : /* [한국어] legacy 칩은 벡터가 상위 바이트에 있어 마스크가 다르다 */
				BRCM_INT_PCI_MSI_MASK;

	writel(val, msi->intr_base + MSI_INT_MASK_CLR); /* [한국어] 해당 비트들의 인터럽트 마스크를 푼다 */
	writel(val, msi->intr_base + MSI_INT_CLR); /* [한국어] 혹시 남아 있던 대기 비트를 지워 깨끗한 상태에서 시작한다 */

	/*
	 * The 0 bit of PCIE_MISC_MSI_BAR_CONFIG_LO is repurposed to MSI
	 * enable, which we set to 1.
	 */
	writel(lower_32_bits(msi->target_addr) | 0x1,
	       msi->base + PCIE_MISC_MSI_BAR_CONFIG_LO); /* [한국어] 수신 주소 하위 워드에 활성 비트(0번)를 얹어 쓴다 */
	writel(upper_32_bits(msi->target_addr),
	       msi->base + PCIE_MISC_MSI_BAR_CONFIG_HI); /* [한국어] 같은 주소의 상위 워드 */

	val = msi->legacy ? PCIE_MISC_MSI_DATA_CONFIG_VAL_8 : PCIE_MISC_MSI_DATA_CONFIG_VAL_32; /* [한국어] 매칭 값도 벡터 수에 맞춰 8비트 판과 32비트 판이 갈린다 */
	writel(val, msi->base + PCIE_MISC_MSI_DATA_CONFIG); /* [한국어] 하드웨어가 이 값과 맞는 데이터만 MSI 로 받아들인다 — compose_msi_msg 가 장치에 알려 주는 값의 상위 절반과 같다 */
}

/* [한국어]
 * brcm_pcie_enable_msi - 내장 MSI 컨트롤러를 세운다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, -ENODEV 는 DT 에 MSI 인터럽트가 없는 경우,
 *          -ENOMEM 은 할당 실패, 그 밖에는 도메인 생성 실패.
 *
 * DT 의 두 번째 인터럽트(인덱스 1)를 MSI 선으로 쓴다. 인덱스 0 이 무엇인지는
 * DT 바인딩 문서에 있을 것으로 보이나 이 트리에서 확인 못 함 — 이 파일은
 * 인덱스 0 을 쓰지 않는다.
 *
 * 칩 리비전으로 두 갈래가 갈린다. HW_REV_33 미만이면 legacy 배치로,
 * 전용 MSI 레지스터 블록 대신 INTR2_CPU 블록을 나눠 쓰고 벡터가 상위
 * 바이트에 놓인다 — 그래서 intr_base 가 다르고, nr 이 적고, legacy_shift
 * 가 24 다. 그 이상이면 전용 PCIE_MSI_INTR2 블록에 벡터가 0번부터 놓여
 * shift 가 0 이다.
 *
 * BUILD_BUG_ON 이 legacy 벡터 수가 일반 벡터 수를 넘지 않음을 컴파일
 * 시점에 못박는다. 비트맵이 BRCM_INT_PCI_MSI_NR 크기로 잡혀 있어 legacy
 * 쪽이 더 크면 비트맵을 넘어서기 때문이다.
 *
 * 마지막 순서가 중요하다. 도메인을 먼저 만들고, 체인 핸들러를 걸고,
 * 그다음에 하드웨어 수신을 켠다. 반대로 하면 도메인이 없는 상태에서
 * 인터럽트가 들어올 수 있다.
 *
 * pcie->msi 를 마지막에 채우는 것도 같은 뜻이다 — 이 값이 NULL 이 아닌
 * 것이 곧 "MSI 가 완전히 준비됨" 의 표시이고, brcm_msi_remove() 와
 * resume 경로가 그것을 본다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). devm_kzalloc 이므로 실패 시
 * 따로 풀 것이 없다.
 *
 * 호출 체인:  brcm_pcie_probe() → [이 함수]
 *               → brcm_allocate_domains() → brcm_msi_set_regs()
 */
static int brcm_pcie_enable_msi(struct brcm_pcie *pcie)
{
	struct brcm_msi *msi; /* [한국어] 새로 잡을 MSI 상태 */
	int irq, ret; /* [한국어] DT 에서 얻은 MSI 인터럽트 번호와 하위 호출 결과 */
	struct device *dev = pcie->dev; /* [한국어] 로그와 devm 할당의 기준 */

	irq = irq_of_parse_and_map(dev->of_node, 1); /* [한국어] DT 인터럽트 목록의 두 번째(인덱스 1)를 MSI 선으로 쓴다. 인덱스 0 은 이 파일이 쓰지 않는다 */
	if (irq <= 0) { /* [한국어] DT 가 그 선을 주지 않았거나 매핑에 실패했다 */
		dev_err(dev, "cannot map MSI interrupt\n"); /* [한국어] MSI 를 쓸 수 없음을 알린다 */
		return -ENODEV; /* [한국어] probe 가 이 오류를 치명적으로 다뤄 그 자리에서 정리로 간다 */
	}

	msi = devm_kzalloc(dev, sizeof(struct brcm_msi), GFP_KERNEL); /* [한국어] device 수명에 묶어 잡는다 — 실패 경로에서 따로 풀 필요가 없다 */
	if (!msi) /* [한국어] 메모리가 없으면 */
		return -ENOMEM; /* [한국어] 할당 실패를 그대로 올린다 */

	mutex_init(&msi->lock); /* [한국어] 비트맵을 지킬 뮤텍스를 초기화한다. 도메인을 만들기 전이어야 한다 */
	msi->dev = dev; /* [한국어] 로그용 device 를 복사해 둔다 */
	msi->base = pcie->base; /* [한국어] 레지스터 기준 주소를 복사해 둔다 */
	msi->np = pcie->np; /* [한국어] MSI 도메인의 fwnode 로 쓸 DT 노드 */
	msi->target_addr = pcie->msi_target_addr; /* [한국어] setup 이 정해 둔 MSI 목표 주소를 가져온다 */
	msi->irq = irq; /* [한국어] 위에서 얻은 인터럽트 번호를 기억해 둔다 — remove 가 이 값으로 핸들러를 뗀다 */
	msi->legacy = pcie->hw_rev < BRCM_PCIE_HW_REV_33; /* [한국어] 리비전이 경계 미만이면 legacy MSI 배치다 */

	/*
	 * Sanity check to make sure that the 'used' bitmap in struct brcm_msi
	 * is large enough.
	 */
	BUILD_BUG_ON(BRCM_INT_PCI_MSI_LEGACY_NR > BRCM_INT_PCI_MSI_NR); /* [한국어] legacy 벡터 수가 비트맵 크기를 넘지 않음을 컴파일 시점에 못박는다 */

	if (msi->legacy) { /* [한국어] legacy 배치 */
		msi->intr_base = msi->base + INTR2_CPU_BASE(pcie); /* [한국어] 전용 블록이 없어 INTR2_CPU 블록을 나눠 쓴다 */
		msi->nr = BRCM_INT_PCI_MSI_LEGACY_NR; /* [한국어] 쓸 수 있는 벡터가 8개뿐 */
		msi->legacy_shift = 24; /* [한국어] 벡터가 [31..24] 에 있어 24 만큼 밀어야 0번부터 센다 */
	} else { /* [한국어] HW_REV_33 이상 */
		msi->intr_base = msi->base + PCIE_MSI_INTR2_BASE; /* [한국어] 전용 MSI 인터럽트 블록을 쓴다 */
		msi->nr = BRCM_INT_PCI_MSI_NR; /* [한국어] 벡터 32개 */
		msi->legacy_shift = 0; /* [한국어] 벡터가 0번부터라 밀 필요가 없다 */
	}

	ret = brcm_allocate_domains(msi); /* [한국어] 도메인을 먼저 만든다 — 핸들러보다 앞서야 인터럽트가 갈 곳이 있다 */
	if (ret) /* [한국어] 도메인 생성 실패 */
		return ret; /* [한국어] devm 할당이라 되돌릴 것 없이 오류만 올린다 */

	irq_set_chained_handler_and_data(msi->irq, brcm_pcie_msi_isr, msi); /* [한국어] 체인 핸들러를 건다. 이때부터 인터럽트가 이 파일로 들어올 수 있다 */

	brcm_msi_set_regs(msi); /* [한국어] 마지막에 하드웨어 수신을 켠다 */
	pcie->msi = msi; /* [한국어] 모든 준비가 끝난 뒤에 채운다 — 이 값이 NULL 이 아닌 것이 곧 "완전히 준비됨" 의 표시다 */

	return 0; /* [한국어] MSI 준비 완료 */
}

/* The controller is capable of serving in both RC and EP roles */
/* [한국어]
 * brcm_pcie_rc_mode - 이 컨트롤러가 RC 로 동작 중인지 본다
 *
 * @pcie: 컨트롤러 상태.
 * @return: true 면 루트 컴플렉스, false 면 엔드포인트.
 *
 * 상류 주석대로 이 하드웨어는 RC 와 EP 양쪽이 될 수 있고, 어느 쪽인지는
 * 보드 배선이 정한다. 이 드라이버는 RC 만 지원하므로 probe 가 이 값으로
 * 자신이 다룰 수 있는 상황인지 판정한다.
 *
 * STATUS 레지스터의 PORT 비트 하나를 읽는 것이 전부다. !! 로 접는 이유는
 * FIELD_GET 이 돌려주는 값이 0/1 이 아니라 필드 폭만큼의 정수일 수 있어서다.
 *
 * 실행 컨텍스트: 제약 없음. 레지스터 읽기 하나다.
 *
 * 호출 체인:  brcm_pcie_start_link() → [이 함수] → readl()
 */
static bool brcm_pcie_rc_mode(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base; /* [한국어] 레지스터 기준 주소 */
	u32 val = readl(base + PCIE_MISC_PCIE_STATUS); /* [한국어] 포트 모드와 링크 상태가 함께 들어 있는 상태 레지스터를 읽는다 */

	return !!FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_PORT_MASK, val); /* [한국어] RC 모드 비트 하나. FIELD_GET 이 폭만큼의 정수를 주므로 !! 로 0/1 로 접는다 */
}

/* [한국어]
 * brcm_pcie_link_up - 링크가 실제로 올라왔는지 본다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 두 조건이 모두 참일 때만 true.
 *
 * 같은 STATUS 레지스터에서 두 비트를 본다 — PHY 링크업과 데이터 링크
 * 활성이다. 물리 계층만 붙고 데이터 링크가 아직이면 config 접근이
 * 실패하므로, 둘 다여야 "올라왔다" 로 친다.
 *
 * 이 판정이 map_bus 에서도 쓰인다는 점이 중요하다. 상류 주석이 밝히듯
 * 링크 없이 하드웨어에 접근하면 CPU Abort 가 나기 때문이다.
 *
 * 실행 컨텍스트: 제약 없음. config 접근 경로에서도 불리므로 잠들면 안 된다.
 *
 * 호출 체인:  brcm_pcie_start_link() / brcm_pcie_map_bus() → [이 함수]
 */
static bool brcm_pcie_link_up(struct brcm_pcie *pcie)
{
	u32 val = readl(pcie->base + PCIE_MISC_PCIE_STATUS); /* [한국어] 같은 상태 레지스터를 한 번만 읽어 아래 두 필드를 함께 뽑는다 */
	u32 dla = FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_DL_ACTIVE_MASK, val); /* [한국어] 데이터 링크 계층이 활성인지 */
	u32 plu = FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_PHYLINKUP_MASK, val); /* [한국어] 물리 계층 링크가 올라왔는지 */

	return dla && plu; /* [한국어] 둘 다여야 config 접근이 성립한다 — PHY 만 붙은 상태로 접근하면 abort 가 난다 */
}

/* [한국어]
 * brcm_pcie_map_bus - config 공간 접근 주소를 만든다(일반 칩)
 *
 * @bus:   접근할 버스.
 * @devfn: 장치/함수 번호.
 * @where: config 공간 안의 오프셋.
 * @return: 읽고 쓸 주소. 접근하면 안 되는 상황이면 NULL.
 *
 * pci_ops 의 map_bus 콜백이다. PCI 코어의 pci_generic_config_read/write 가
 * 이 함수가 준 주소로 실제 접근을 한다.
 *
 * 세 갈래다.
 *
 *   1) 루트 버스이면서 devfn 이 0 - RC 자신의 config 를 보는 것이므로
 *      컨트롤러 레지스터에 직접 매핑한다. ECAM 인덱스를 거치지 않는다.
 *   2) 루트 버스인데 devfn 이 0 이 아님 - 루트 버스에는 RC 하나뿐이라
 *      그런 장치는 없다. NULL 을 돌려주면 코어가 0xffffffff 로 처리한다.
 *   3) 그 아래 버스 - 링크를 먼저 확인한다. 상류 주석대로 링크 없이
 *      접근하면 CPU Abort 가 난다. 그다음 인덱스 레지스터에 BDF 를 써서
 *      창을 옮기고, 데이터 창의 주소를 돌려준다.
 *
 * 3) 에서 인덱스에 넣는 오프셋이 0 이고 반환 주소에 where 를 더한다는
 * 점이 brcm7425 판과 갈리는 자리다. 이 칩은 데이터 창이 config 공간
 * 한 덩어리로 열려 있어 창 안에서 오프셋을 걸을 수 있다.
 *
 * 인덱스 레지스터가 컨트롤러 하나뿐인 공유 자원인데 여기서 락을 잡지
 * 않는다. PCI 코어가 config 접근을 pci_lock 으로 직렬화하기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트에서도 불릴 수 있다. 잠들지 않는다.
 *
 * 호출 체인:  PCI 코어(pci_generic_config_read/write) → [이 함수]
 */
static void __iomem *brcm_pcie_map_bus(struct pci_bus *bus,
				       unsigned int devfn, int where)
{
	struct brcm_pcie *pcie = bus->sysdata; /* [한국어] PCI 코어가 버스에 심어 둔 컨트롤러 상태를 되찾는다 */
	void __iomem *base = pcie->base; /* [한국어] 레지스터 기준 주소 */
	int idx; /* [한국어] config 창을 옮길 때 인덱스 레지스터에 쓸 값 */

	/* Accesses to the RC go right to the RC registers if !devfn */
	if (pci_is_root_bus(bus)) /* [한국어] 루트 버스는 RC 자신이다 */
		return devfn ? NULL : base + PCIE_ECAM_REG(where); /* [한국어] devfn 이 0 이면 RC 레지스터에 직접 매핑하고, 그 밖의 devfn 은 존재하지 않으므로 NULL(코어가 0xffffffff 로 처리) */

	/* An access to our HW w/o link-up will cause a CPU Abort */
	if (!brcm_pcie_link_up(pcie)) /* [한국어] 링크가 없으면 */
		return NULL; /* [한국어] 접근하면 CPU Abort 가 나므로 아예 주소를 주지 않는다 */

	/* For devices, write to the config space index register */
	idx = PCIE_ECAM_OFFSET(bus->number, devfn, 0); /* [한국어] 대상 BDF 로 인덱스 값을 만든다. 여기서 오프셋을 0 으로 두는 것이 7425 판과 갈리는 자리다 */
	writel(idx, base + IDX_ADDR(pcie)); /* [한국어] 인덱스 레지스터에 써서 config 창을 그 장치로 옮긴다. 직렬화는 PCI 코어의 config 락이 맡는다 */
	return base + DATA_ADDR(pcie) + PCIE_ECAM_REG(where); /* [한국어] 데이터 창 안에서 오프셋만큼 걸어간 주소를 준다 — 이 칩은 창이 config 공간 한 덩어리로 열린다 */
}

/* [한국어]
 * brcm7425_pcie_map_bus - config 공간 접근 주소를 만든다(BCM7425)
 *
 * @bus:   접근할 버스.
 * @devfn: 장치/함수 번호.
 * @where: config 공간 안의 오프셋.
 * @return: 읽고 쓸 주소. 접근하면 안 되는 상황이면 NULL.
 *
 * 일반 판과 갈리는 곳은 마지막 두 줄뿐이다. 이 칩은 데이터 창이 레지스터
 * 하나 크기라, 창 안에서 오프셋을 걸을 수 없다. 그래서 접근할 오프셋까지
 * 인덱스에 함께 넣어 창 자체를 그 자리로 옮기고, 창 주소를 그대로
 * 돌려준다.
 *
 * 루트 버스 처리와 링크 확인은 일반 판과 같다.
 *
 * 이 칩이 BMIPS 계열이라 is_bmips() 가 참인 쪽인데, 여기서는 그 헬퍼를
 * 쓰지 않는다 — 아예 별도의 pci_ops 로 갈아 끼우기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트에서도 불릴 수 있다.
 *
 * 호출 체인:  PCI 코어 → [이 함수]
 */
static void __iomem *brcm7425_pcie_map_bus(struct pci_bus *bus,
					   unsigned int devfn, int where)
{
	struct brcm_pcie *pcie = bus->sysdata; /* [한국어] PCI 코어가 버스에 심어 둔 컨트롤러 상태 */
	void __iomem *base = pcie->base; /* [한국어] 레지스터 기준 주소 */
	int idx; /* [한국어] 인덱스 레지스터에 쓸 값 */

	/* Accesses to the RC go right to the RC registers if !devfn */
	if (pci_is_root_bus(bus)) /* [한국어] 루트 버스는 RC 자신 */
		return devfn ? NULL : base + PCIE_ECAM_REG(where); /* [한국어] 일반 판과 같은 처리 */

	/* An access to our HW w/o link-up will cause a CPU Abort */
	if (!brcm_pcie_link_up(pcie)) /* [한국어] 링크가 없으면 */
		return NULL; /* [한국어] 접근 자체를 막는다 */

	/* For devices, write to the config space index register */
	idx = PCIE_ECAM_OFFSET(bus->number, devfn, where); /* [한국어] BDF 와 함께 접근할 오프셋까지 인덱스에 넣는다 — 이 칩은 데이터 창이 레지스터 하나 크기라 창 안에서 걸을 수 없다 */
	writel(idx, base + IDX_ADDR(pcie)); /* [한국어] 인덱스 레지스터에 써서 창을 그 자리로 옮긴다 */
	return base + DATA_ADDR(pcie); /* [한국어] 창 주소를 그대로 준다. 오프셋을 더하지 않는 것이 일반 판과의 유일한 차이다 */
}

/* [한국어]
 * brcm_pcie_bridge_sw_init_set_generic - 브리지 리셋 조작(일반 판)
 *
 * @pcie: 컨트롤러 상태.
 * @val:  1 이면 리셋을 걸고, 0 이면 푼다.
 * @return: 0 성공. reset controller 조작이 실패하면 그 오류.
 *
 * 두 가지 방식을 함께 다룬다.
 *
 * DT 가 "bridge" 리셋을 주었다면 리셋 컨트롤러 API 를 쓴다. 그 경우
 * 레지스터를 직접 건드리지 않는다 — 리셋 선이 다른 블록에 있어 이 드라이버가
 * 소유하지 않기 때문이다. 이 갈래에서만 실패가 나올 수 있다.
 *
 * 그렇지 않으면 RGR1_SW_INIT_1 레지스터의 INIT 비트를 읽고-고쳐-쓴다.
 * 마스크와 shift 가 일반 판 상수인 것이 7278 판과 갈리는 유일한 차이다.
 *
 * 읽고-고쳐-쓰기라 경쟁이 문제가 되는데, 그 보호는 호출자인
 * brcm_pcie_bridge_sw_init_set() 이 has_err_report 인 칩에 한해 스핀락으로
 * 해 준다. 그 칩만 감싸는 이유는 오류 보고 콜백이 인터럽트/패닉 문맥에서
 * 같은 레지스터를 건드릴 수 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. reset_control_ 계열은 잠들 수 있다.
 *
 * 호출 체인:  brcm_pcie_bridge_sw_init_set() → [이 함수]
 */
static int brcm_pcie_bridge_sw_init_set_generic(struct brcm_pcie *pcie, u32 val)
{
	u32 tmp, mask = RGR1_SW_INIT_1_INIT_GENERIC_MASK; /* [한국어] 읽고-고쳐-쓸 임시 값과 INIT 비트의 마스크(일반 판) */
	u32 shift = RGR1_SW_INIT_1_INIT_GENERIC_SHIFT; /* [한국어] 그 비트의 자리 */
	int ret = 0; /* [한국어] 레지스터 경로에서는 실패가 없으므로 0 으로 시작한다 */

	if (pcie->bridge_reset) { /* [한국어] DT 가 "bridge" 리셋을 주었으면 레지스터 대신 그쪽을 쓴다 */
		if (val) /* [한국어] 리셋을 걸라는 요청 */
			ret = reset_control_assert(pcie->bridge_reset); /* [한국어] 리셋 컨트롤러에 assert 를 건다 */
		else
			ret = reset_control_deassert(pcie->bridge_reset); /* [한국어] 풀라는 요청이면 deassert */

		if (ret) /* [한국어] 리셋 조작이 실패했으면 */
			dev_err(pcie->dev, "failed to %s 'bridge' reset, err=%d\n",
				val ? "assert" : "deassert", ret); /* [한국어] 어느 방향이 실패했는지 문자열로 구분해 알린다 */

		return ret; /* [한국어] 레지스터 경로로 내려가지 않고 여기서 끝낸다 */
	}

	tmp = readl(pcie->base + PCIE_RGR1_SW_INIT_1(pcie)); /* [한국어] 현재 값을 읽는다. 이 레지스터에는 PERST# 비트도 함께 있어 통째로 덮으면 안 된다 */
	tmp = (tmp & ~mask) | ((val << shift) & mask); /* [한국어] INIT 필드만 요청 값으로 갈아 끼운다 */
	writel(tmp, pcie->base + PCIE_RGR1_SW_INIT_1(pcie)); /* [한국어] 되쓴다. 이 읽고-고쳐-쓰기의 보호는 래퍼의 bridge_lock 이 맡는다 */

	return ret; /* [한국어] 레지스터 경로에서는 늘 0 이다 */
}

/* [한국어]
 * brcm_pcie_bridge_sw_init_set_7278 - 브리지 리셋 조작(BCM7278)
 *
 * @pcie: 컨트롤러 상태.
 * @val:  1 이면 리셋, 0 이면 해제.
 * @return: 늘 0. 실패할 일이 없다.
 *
 * 같은 RGR1_SW_INIT_1 레지스터를 쓰지만 INIT 비트의 자리가 달라 전용
 * 마스크/shift 상수를 쓴다. 그 자리 차이 하나 때문에 함수가 따로 있다.
 *
 * 일반 판과 달리 reset controller 갈래가 없다. 이 칩은 그 방식을 쓰지
 * 않기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  brcm_pcie_bridge_sw_init_set() → [이 함수]
 */
static int brcm_pcie_bridge_sw_init_set_7278(struct brcm_pcie *pcie, u32 val)
{
	u32 tmp, mask =  RGR1_SW_INIT_1_INIT_7278_MASK; /* [한국어] 임시 값과 INIT 비트의 마스크(BCM7278 판) — 자리가 달라 상수가 다르다 */
	u32 shift = RGR1_SW_INIT_1_INIT_7278_SHIFT; /* [한국어] 그 비트의 자리 */

	tmp = readl(pcie->base + PCIE_RGR1_SW_INIT_1(pcie)); /* [한국어] 현재 값을 읽는다 */
	tmp = (tmp & ~mask) | ((val << shift) & mask); /* [한국어] INIT 필드만 갈아 끼운다 */
	writel(tmp, pcie->base + PCIE_RGR1_SW_INIT_1(pcie)); /* [한국어] 되쓴다 */

	return 0; /* [한국어] 이 경로는 실패할 일이 없다 */
}

/* [한국어]
 * brcm_pcie_perst_set_4908 - PERST# 조작(BCM4908)
 *
 * @pcie: 컨트롤러 상태.
 * @val:  1 이면 PERST# 를 걸고(장치 리셋), 0 이면 푼다.
 * @return: 0 성공, -EINVAL 은 DT 에 perst 리셋이 없는 경우.
 *
 * 이 칩은 PERST# 가 컨트롤러 레지스터가 아니라 별도 리셋 컨트롤러에
 * 있어, reset_control_assert/deassert 로만 조작할 수 있다.
 *
 * 그래서 perst_reset 이 없으면 아무것도 할 수 없다. WARN_ONCE 로 한 번만
 * 경고하고 오류를 돌려주는데, DT 가 잘못된 구성이라 반복 경고가 의미
 * 없기 때문이다.
 *
 * PERST# 는 링크를 세우기 직전에 풀고 전원을 끄기 전에 거는 신호다.
 * 어느 쪽이든 실패하면 링크가 서지 않으므로 호출자가 오류를 그대로
 * 위로 올린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:  brcm_pcie_setup() / brcm_pcie_start_link() / brcm_pcie_turn_off()
 *               → cfg->perst_set → [이 함수]
 */
static int brcm_pcie_perst_set_4908(struct brcm_pcie *pcie, u32 val)
{
	int ret; /* [한국어] 리셋 조작 결과 */

	if (WARN_ONCE(!pcie->perst_reset, "missing PERST# reset controller\n")) /* [한국어] 이 칩은 PERST# 를 리셋 컨트롤러로만 만질 수 있어, 그것이 없으면 아무것도 못 한다. DT 구성 오류라 한 번만 경고한다 */
		return -EINVAL; /* [한국어] 호출자가 이 오류를 그대로 위로 올린다 */

	if (val) /* [한국어] PERST# 를 걸라는 요청 */
		ret = reset_control_assert(pcie->perst_reset); /* [한국어] assert 로 하류 장치를 리셋에 넣는다 */
	else
		ret = reset_control_deassert(pcie->perst_reset); /* [한국어] 풀라는 요청이면 deassert */

	if (ret) /* [한국어] 실패했으면 */
		dev_err(pcie->dev, "failed to %s 'perst' reset, err=%d\n",
			val ? "assert" : "deassert", ret); /* [한국어] 어느 방향이 실패했는지 알린다 */
	return ret; /* [한국어] 결과를 그대로 올린다 */
}

/* [한국어]
 * brcm_pcie_perst_set_7278 - PERST# 조작(BCM7278 계열)
 *
 * @pcie: 컨트롤러 상태.
 * @val:  1 이면 PERST# 를 건다.
 * @return: 늘 0.
 *
 * 상류 주석이 두 가지를 짚는다 — 이 칩은 PERST 비트가 다른 레지스터
 * (PCIE_MISC_PCIE_CTRL)로 옮겨 갔고, 극성이 반대다.
 *
 * 그래서 !val 을 쓴다. 레지스터 필드 이름이 PERSTB 로 끝나는 것이
 * active-low 임을 말해 준다. 이 함수의 @val 규약은 다른 판과 같게
 * "1 = 리셋" 으로 맞춰 두고, 극성 반전은 여기서 흡수한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  cfg->perst_set → [이 함수]
 */
static int brcm_pcie_perst_set_7278(struct brcm_pcie *pcie, u32 val)
{
	u32 tmp; /* [한국어] 읽고-고쳐-쓸 임시 값 */

	/* Perst bit has moved and assert value is 0 */
	tmp = readl(pcie->base + PCIE_MISC_PCIE_CTRL); /* [한국어] PERST 비트가 옮겨 간 레지스터를 읽는다 */
	u32p_replace_bits(&tmp, !val, PCIE_MISC_PCIE_CTRL_PCIE_PERSTB_MASK); /* [한국어] !val 을 넣는다 — 필드 이름이 PERSTB 로 끝나는 대로 active-low 라 극성이 반대다 */
	writel(tmp, pcie->base +  PCIE_MISC_PCIE_CTRL); /* [한국어] 되쓴다 */

	return 0; /* [한국어] 이 경로는 실패할 일이 없다 */
}

/* [한국어]
 * brcm_pcie_perst_set_generic - PERST# 조작(일반 판)
 *
 * @pcie: 컨트롤러 상태.
 * @val:  1 이면 PERST# 를 건다.
 * @return: 늘 0.
 *
 * 브리지 리셋과 같은 RGR1_SW_INIT_1 레지스터 안의 PERST 비트를 쓴다.
 * 극성이 그대로라 val 을 뒤집지 않는 것이 7278 판과 갈리는 자리다.
 *
 * 브리지 리셋 비트와 한 레지스터를 나눠 쓰므로, 읽고-고쳐-쓰기가 서로를
 * 덮어쓸 수 있다. 그 보호는 has_err_report 인 칩에 한해 호출 경로 위쪽의
 * bridge_lock 이 맡는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  cfg->perst_set → [이 함수]
 */
static int brcm_pcie_perst_set_generic(struct brcm_pcie *pcie, u32 val)
{
	u32 tmp; /* [한국어] 읽고-고쳐-쓸 임시 값 */

	tmp = readl(pcie->base + PCIE_RGR1_SW_INIT_1(pcie)); /* [한국어] 브리지 리셋 비트와 한 레지스터를 나눠 쓰므로 먼저 읽는다 */
	u32p_replace_bits(&tmp, val, PCIE_RGR1_SW_INIT_1_PERST_MASK); /* [한국어] PERST 필드만 갈아 끼운다. 극성이 그대로라 val 을 뒤집지 않는 것이 7278 판과의 차이다 */
	writel(tmp, pcie->base + PCIE_RGR1_SW_INIT_1(pcie)); /* [한국어] 되쓴다 */

	return 0; /* [한국어] 이 경로는 실패할 일이 없다 */
}

/* [한국어]
 * brcm_pcie_post_setup_bcm2712 - BCM2712 전용 PHY 후처리
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. MDIO 쓰기가 실패하면 그 오류.
 *
 * setup 이 끝난 뒤 cfg->post_setup 으로 불리는 유일한 구현이다.
 * 이 칩만 PHY 를 추가로 손봐야 해서 함수 포인터로 분리되어 있다.
 *
 * 두 부분이다.
 *
 *   1) MDIO 로 PHY 레지스터 일곱 개에 값을 써 넣는다. 상류 주석이
 *      "54MHz(xosc) 를 refclk 로 쓸 수 있게 한다" 고 밝힌다. 먼저
 *      SET_ADDR_OFFSET 에 0x1600 을 써서 접근할 레지스터 뭉치를 고르고,
 *      그다음 regs[]/data[] 짝을 차례로 쓴다. 각 값의 의미는 브로드컴
 *      PHY 문서에 있을 것으로 보이나 이 트리에서 확인 못 함.
 *      쓰고 나서 usleep_range 로 잠깐 기다리는데, PHY 가 새 설정을
 *      반영할 시간을 주기 위한 것으로 보인다.
 *   2) L1SS 서브스테이트 타이머의 기준 클록 주기를 적는다. 상류 주석이
 *      계산까지 적어 두었다 — 54MHz 의 역수 18.52ns 를 내림해 0x12(18)
 *      로 넣는다. 이 값이 틀리면 L1 서브스테이트 진입/복귀 시간이 어긋나
 *      상태 전이가 길어진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 호출 체인:  brcm_pcie_setup() → cfg->post_setup → [이 함수]
 *               → brcm_pcie_mdio_write()
 */
static int brcm_pcie_post_setup_bcm2712(struct brcm_pcie *pcie)
{
	static const u16 data[] = { 0x50b9, 0xbda1, 0x0094, 0x97b4, 0x5030,
				    0x5030, 0x0007 }; /* [한국어] PHY 레지스터에 써 넣을 값들. 각 값의 의미는 브로드컴 PHY 문서에 있을 것으로 보이나 이 트리에서 확인 못 함 */
	static const u8 regs[] = { 0x16, 0x17, 0x18, 0x19, 0x1b, 0x1c, 0x1e }; /* [한국어] 위 값들이 들어갈 PHY 레지스터 번호들. 두 배열이 인덱스로 짝을 이룬다 */
	int ret, i; /* [한국어] MDIO 결과와 루프 인덱스 */
	u32 tmp; /* [한국어] 읽고-고쳐-쓸 임시 값 */

	/* Allow a 54MHz (xosc) refclk source */
	ret = brcm_pcie_mdio_write(pcie->base, MDIO_PORT0, SET_ADDR_OFFSET, 0x1600); /* [한국어] 접근할 PHY 레지스터 뭉치를 0x1600 자리로 옮긴다 */
	if (ret < 0) /* [한국어] MDIO 가 안 되면 아래 쓰기도 무의미하다 */
		return ret; /* [한국어] 오류를 그대로 올린다 */

	for (i = 0; i < ARRAY_SIZE(regs); i++) { /* [한국어] 짝지어 둔 레지스터/값을 차례로 쓴다 */
		ret = brcm_pcie_mdio_write(pcie->base, MDIO_PORT0, regs[i], data[i]); /* [한국어] 한 쌍씩 MDIO 로 써 넣는다 */
		if (ret < 0) /* [한국어] 한 번이라도 실패하면 */
			return ret; /* [한국어] 그 자리에서 그만둔다 — 반쯤 쓴 상태로 두지만 setup 이 오류로 돌아가 이 칩을 쓰지 않는다 */
	}

	usleep_range(100, 200); /* [한국어] PHY 가 새 설정을 반영할 시간을 준다 */

	/*
	 * Set L1SS sub-state timers to avoid lengthy state transitions,
	 * PM clock period is 18.52ns (1/54MHz, round down).
	 */
	tmp = readl(pcie->base + PCIE_RC_PL_PHY_CTL_15); /* [한국어] L1SS 타이머의 기준 클록 주기 필드가 있는 레지스터를 읽는다 */
	tmp &= ~PCIE_RC_PL_PHY_CTL_15_PM_CLK_PERIOD_MASK; /* [한국어] 그 필드를 지운다 */
	tmp |= 0x12; /* [한국어] 54MHz 의 주기 18.52ns 를 내림한 18(0x12)을 넣는다 */
	writel(tmp, pcie->base + PCIE_RC_PL_PHY_CTL_15); /* [한국어] 되쓴다. 이 값이 틀리면 L1 서브스테이트 전이 시간이 어긋난다 */

	return 0; /* [한국어] 후처리 완료 */
}

/* [한국어]
 * add_inbound_win - 안쪽 창 배열에 항목 하나를 적고 개수를 늘린다
 *
 * @b:          채울 항목. 호출자가 이미 다음 빈 자리를 가리키게 해 둔다.
 * @count:      지금까지 채운 개수. 이 함수가 하나 올린다.
 * @size:       창 크기(바이트).
 * @cpu_addr:   CPU 쪽 시작 주소.
 * @pci_offset: 이 창이 대응하는 PCI 주소.
 *
 * 세 필드를 적고 개수를 올리는 것이 전부다. 별도 함수인 이유는
 * brcm_pcie_get_inbound_wins() 안에서 이 다섯 줄이 여러 갈래에서
 * 되풀이되기 때문이다.
 *
 * 배열 경계를 여기서 확인하지 않는다. 넘치지 않게 하는 책임은 호출자에게
 * 있고, 실제로 get_inbound_wins() 가 cfg->num_inbound_wins 와 대조해
 * 넘치면 오류로 돌아간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe/resume).
 *
 * 호출 체인:  brcm_pcie_get_inbound_wins() → [이 함수]
 */
static void add_inbound_win(struct inbound_win *b, u8 *count, u64 size,
			    u64 cpu_addr, u64 pci_offset)
{
	b->size = size; /* [한국어] 창 크기를 적는다 */
	b->cpu_addr = cpu_addr; /* [한국어] CPU 쪽 시작 주소를 적는다 */
	b->pci_offset = pci_offset; /* [한국어] PCI 쪽 시작 주소를 적는다 */
	(*count)++; /* [한국어] 채운 개수를 하나 올린다. 경계 확인은 호출자 책임이다 */
}

/* [한국어]
 * brcm_pcie_get_inbound_wins - DT 의 dma-ranges 를 안쪽 창 목록으로 바꾼다
 *
 * @pcie:         컨트롤러 상태.
 * @inbound_wins: 채울 배열. PCIE_BRCM_MAX_INBOUND_WINS 크기로 호출자가 잡아 둔다.
 * @return: 채운 창 개수(양수). dma-ranges 가 없거나 정렬/크기가 어긋나면 -EINVAL.
 *
 * 이 드라이버에서 가장 까다로운 함수다. "장치가 시스템 메모리를 어떻게
 * 보게 할 것인가" 를 정하며, 상류가 긴 주석 다섯 덩어리로 그 이유를
 * 남겨 두었다.
 *
 * 먼저 배열 인덱스 규약이 있다. 상류 주석대로 하드웨어와 PCIe 가 BAR 를
 * 1번부터 세므로 inbound_wins[0] 은 비워 두고 1번부터 쓴다. b_begin 이
 * 그 자리를 가리킨다.
 *
 * 이어서 두 갈래로 크게 갈린다.
 *
 *   [BCM7712 계열] dma-ranges 항목마다 창을 하나씩 만든다. 상류 주석대로
 *   이 칩은 BAR 가 많아 각각이 시스템 메모리의 겹치지 않는 조각을 볼 수
 *   있다(다만 각 크기는 여전히 2의 거듭제곱이어야 한다). 그래서 루프 안에서
 *   바로 창을 추가하고, 개수가 cfg->num_inbound_wins 를 넘으면 멈춘다.
 *   이 칩은 여러 메모리 컨트롤러를 하나로 합쳐 보여 주는 내부 장치가 없어
 *   (상류 주석), 여기서 곧바로 돌아간다.
 *
 *   [그 밖의 STB 칩] 창을 실제로 하나(BAR2)만 쓴다. 1번은 비활성으로,
 *   3번도 비활성으로 만든다.
 *     - 1번을 끄는 이유는 상류 주석에 있다. 기본값이 시스템 메모리가 아니라
 *       SoC 내부 레지스터를 가리키는데, 이 기능은 폐기되었고 보안상 문제가
 *       있으며 요즘 칩에는 없다.
 *     - 3번을 끄는 이유도 상류 주석에 있다. 어떤 칩에서는 2번과 같은 창을
 *       엔디언만 바꿔 보여 주는 것이라 쓸 일이 없다.
 *     - 2번의 크기는 memc(메모리 컨트롤러)들의 크기 합을 2의 거듭제곱으로
 *       올린 값이다. 그 크기는 DT 의 "brcm,scb-sizes" 에서 읽고, 없으면
 *       dma-ranges 총합으로 짐작한다("educated guess" — 상류 표현).
 *     - 2번의 CPU 주소는 0 이다. 상류 주석대로 이 칩은 BAR2 의 CPU 쪽
 *       시작이 시스템 메모리 시작에 하드와이어되어 있기 때문이다.
 *     - PCI 쪽 시작은 dma-ranges 중 가장 낮은 PCI 주소다.
 *
 * 마지막 검증 세 조건이 상류의 가장 긴 주석과 짝을 이룬다. 요약하면 안쪽
 * 창은 시스템 메모리 전체를 연속으로 덮어야 하고, 크기가 2의 거듭제곱이며,
 * 시작 PCI 주소가 그 크기의 배수여야 한다. 세 번째 조건(2GB 초과 4GB 미만
 * 금지)은 초기 라즈베리파이 4(bcm2711)의 하드웨어 문제 때문인데, 그 펌웨어가
 * dma-ranges 를 동적으로 고쳐 쓰기 때문에 DT 를 그대로 믿지 않고 여기서
 * 확인한다고 상류가 밝힌다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe/resume). DT 읽기가 잠들 수 있다.
 *
 * 호출 체인:  brcm_pcie_setup() → [이 함수]
 *               → of_property_read_variable_u64_array() / add_inbound_win()
 */
static int brcm_pcie_get_inbound_wins(struct brcm_pcie *pcie,
				      struct inbound_win inbound_wins[])
{
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie); /* [한국어] private 영역에서 바깥 호스트 브리지를 되찾는다. dma_ranges 가 거기 있다 */
	u64 pci_offset, cpu_addr, size = 0, tot_size = 0; /* [한국어] 창 하나의 PCI/CPU 시작과 크기, 그리고 dma-ranges 총합 */
	struct resource_entry *entry; /* [한국어] dma_ranges 순회용 항목 */
	struct device *dev = pcie->dev; /* [한국어] 로그용 device */
	u64 lowest_pcie_addr = ~(u64)0; /* [한국어] 가장 낮은 PCI 주소를 찾기 위해 최댓값으로 시작한다 */
	int ret, i = 0; /* [한국어] DT 읽기 결과와 memc 합산 루프 인덱스 */
	u8 n = 0; /* [한국어] 채운 창 개수. add_inbound_win 이 올린다 */

	/*
	 * The HW registers (and PCIe) use order-1 numbering for BARs.  As such,
	 * we have inbound_wins[0] unused and BAR1 starts at inbound_wins[1].
	 */
	struct inbound_win *b_begin = &inbound_wins[1]; /* [한국어] 하드웨어가 BAR 를 1번부터 세므로 0번을 비워 두고 1번부터 쓴다 */
	struct inbound_win *b = b_begin; /* [한국어] 다음에 채울 자리. add_inbound_win 을 부를 때마다 앞으로 간다 */

	/*
	 * STB chips beside 7712 disable the first inbound window default.
	 * Rather being mapped to system memory it is mapped to the
	 * internal registers of the SoC.  This feature is deprecated, has
	 * security considerations, and is not implemented in our modern
	 * SoCs.
	 */
	if (pcie->cfg->soc_base != BCM7712) /* [한국어] 7712 를 뺀 STB 칩들 */
		add_inbound_win(b++, &n, 0, 0, 0); /* [한국어] 창 1 을 크기 0 으로 만들어 끈다 — 기본값이 SoC 내부 레지스터를 가리켜 위험하기 때문 */

	resource_list_for_each_entry(entry, &bridge->dma_ranges) { /* [한국어] DT 의 dma-ranges 항목을 하나씩 훑는다 */
		u64 pcie_start = entry->res->start - entry->offset; /* [한국어] 이 항목의 PCI 쪽 시작. CPU 주소에서 오프셋을 빼서 얻는다 */
		u64 cpu_start = entry->res->start; /* [한국어] 이 항목의 CPU 쪽 시작 */

		size = resource_size(entry->res); /* [한국어] 이 항목이 덮는 크기 */
		tot_size += size; /* [한국어] STB 칩에서 창 크기를 짐작할 때 쓸 총합에 더한다 */
		if (pcie_start < lowest_pcie_addr) /* [한국어] 지금까지 본 것보다 낮으면 */
			lowest_pcie_addr = pcie_start; /* [한국어] 가장 낮은 PCI 주소를 갱신한다 — STB 창 2 의 시작이 될 값이다 */
		/*
		 * 7712 and newer chips may have many BARs, with each
		 * offering a non-overlapping viewport to system memory.
		 * That being said, each BARs size must still be a power of
		 * two.
		 */
		if (pcie->cfg->soc_base == BCM7712) /* [한국어] 7712 계열은 항목마다 창을 하나씩 갖는다 */
			add_inbound_win(b++, &n, size, cpu_start, pcie_start); /* [한국어] 그 자리에 이 항목을 그대로 적는다 */

		if (n > pcie->cfg->num_inbound_wins) /* [한국어] 이 칩이 쓸 수 있는 창 수를 넘었으면 */
			break; /* [한국어] 더 담을 자리가 없으므로 순회를 멈춘다 */
	}

	if (lowest_pcie_addr == ~(u64)0) { /* [한국어] 초깃값 그대로면 dma-ranges 항목이 하나도 없었다는 뜻 */
		dev_err(dev, "DT node has no dma-ranges\n"); /* [한국어] DT 가 필수 속성을 빠뜨린 경우다 */
		return -EINVAL; /* [한국어] 창을 정할 수 없으므로 setup 이 그대로 실패한다 */
	}

	/*
	 * 7712 and newer chips do not have an internal memory mapping system
	 * that enables multiple memory controllers.  As such, it can return
	 * now w/o doing special configuration.
	 */
	if (pcie->cfg->soc_base == BCM7712) /* [한국어] 7712 계열은 memc 를 합쳐 보여 주는 장치가 없어 */
		return n; /* [한국어] 위에서 채운 개수를 그대로 돌려주고 끝낸다 */

	ret = of_property_read_variable_u64_array(pcie->np, "brcm,scb-sizes", pcie->memc_size, 1,
						  PCIE_BRCM_MAX_MEMC); /* [한국어] DT 의 brcm,scb-sizes 에서 memc 별 크기를 최대 PCIE_BRCM_MAX_MEMC 개까지 읽는다 */
	if (ret <= 0) { /* [한국어] 속성이 없거나 읽기에 실패했으면 */
		/* Make an educated guess */
		pcie->num_memc = 1; /* [한국어] memc 가 하나뿐이라고 본다 */
		pcie->memc_size[0] = 1ULL << fls64(tot_size - 1); /* [한국어] dma-ranges 총합을 2의 거듭제곱으로 올려 그 크기로 삼는다 */
	} else {
		pcie->num_memc = ret; /* [한국어] 읽은 항목 수가 곧 memc 개수다 */
	}

	/* Each memc is viewed through a "port" that is a power of 2 */
	for (i = 0, size = 0; i < pcie->num_memc; i++) /* [한국어] 모든 memc 의 크기를 더한다 */
		size += pcie->memc_size[i]; /* [한국어] 합을 누적한다 */

	/* Our HW mandates that the window size must be a power of 2 */
	size = 1ULL << fls64(size - 1); /* [한국어] 합을 2의 거듭제곱으로 올린다 — 하드웨어가 그 크기만 받는다 */

	/*
	 * For STB chips, the BAR2 cpu_addr is hardwired to the start
	 * of system memory, so we set it to 0.
	 */
	cpu_addr = 0; /* [한국어] STB 칩은 BAR2 의 CPU 쪽 시작이 메모리 시작에 하드와이어되어 있어 0 이다 */
	pci_offset = lowest_pcie_addr; /* [한국어] PCI 쪽 시작은 dma-ranges 중 가장 낮은 주소 */

	/*
	 * We validate the inbound memory view even though we should trust
	 * whatever the device-tree provides. This is because of an HW issue on
	 * early Raspberry Pi 4's revisions (bcm2711). It turns out its
	 * firmware has to dynamically edit dma-ranges due to a bug on the
	 * PCIe controller integration, which prohibits any access above the
	 * lower 3GB of memory. Given this, we decided to keep the dma-ranges
	 * in check, avoiding hard to debug device-tree related issues in the
	 * future:
	 *
	 * The PCIe host controller by design must set the inbound viewport to
	 * be a contiguous arrangement of all of the system's memory.  In
	 * addition, its size must be a power of two.  To further complicate
	 * matters, the viewport must start on a pcie-address that is aligned
	 * on a multiple of its size.  If a portion of the viewport does not
	 * represent system memory -- e.g. 3GB of memory requires a 4GB
	 * viewport -- we can map the outbound memory in or after 3GB and even
	 * though the viewport will overlap the outbound memory the controller
	 * will know to send outbound memory downstream and everything else
	 * upstream.
	 *
	 * For example:
	 *
	 * - The best-case scenario, memory up to 3GB, is to place the inbound
	 *   region in the first 4GB of pcie-space, as some legacy devices can
	 *   only address 32bits. We would also like to put the MSI under 4GB
	 *   as well, since some devices require a 32bit MSI target address.
	 *
	 * - If the system memory is 4GB or larger we cannot start the inbound
	 *   region at location 0 (since we have to allow some space for
	 *   outbound memory @ 3GB). So instead it will  start at the 1x
	 *   multiple of its size
	 */
	if (!size || (pci_offset & (size - 1)) || /* [한국어] 크기가 0 이거나, 시작이 크기에 정렬되어 있지 않거나 */
	    (pci_offset < SZ_4G && pci_offset > SZ_2G)) { /* [한국어] 2GB 초과 4GB 미만 자리이면 안 된다 — 초기 bcm2711 의 하드웨어 문제 때문에 그 구간을 배제한다 */
		dev_err(dev, "Invalid inbound_win2_offset/size: size 0x%llx, off 0x%llx\n",
			size, pci_offset); /* [한국어] 어떤 값이 문제인지 그대로 찍어 준다 */
		return -EINVAL; /* [한국어] DT 가 잘못된 것이므로 setup 이 실패한다 */
	}

	/* Enable inbound window 2, the main inbound window for STB chips */
	add_inbound_win(b++, &n, size, cpu_addr, pci_offset); /* [한국어] 창 2 를 계산한 크기와 주소로 연다 — STB 칩의 유일한 실제 안쪽 창이다 */

	/*
	 * Disable inbound window 3.  On some chips presents the same
	 * window as #2 but the data appears in a settable endianness.
	 */
	add_inbound_win(b++, &n, 0, 0, 0); /* [한국어] 창 3 을 크기 0 으로 만들어 끈다 — 어떤 칩에서는 창 2 를 엔디언만 바꿔 보여 주는 것이라 쓸 일이 없다 */

	return n; /* [한국어] 채운 창 개수를 돌려준다. 호출자가 그 수만큼 레지스터에 적는다 */
}

/* [한국어]
 * brcm_bar_reg_offset - BAR 번호로 CONFIG 레지스터 오프셋을 구한다
 *
 * @bar: 1부터 시작하는 BAR 번호.
 * @return: 그 BAR 의 CONFIG_LO 레지스터 오프셋.
 *
 * BAR 하나가 LO/HI 두 워드를 쓰므로 8 바이트 간격이다.
 *
 * 1~3 과 4 이상이 갈리는 이유는 레지스터 배치가 그 사이에서 끊기기
 * 때문이다. BAR1~3 이 한 덩어리로 이어져 있고, BAR4 부터 다른 자리에서
 * 다시 이어진다. 그래서 기준점을 바꿔 각각 (bar-1), (bar-4) 를 곱한다.
 *
 * BAR 가 넷 이상인 것은 BCM7712 뿐이라, 아래 갈래는 그 칩에서만 쓰인다.
 *
 * 실행 컨텍스트: 제약 없음. 순수 계산이다.
 *
 * 호출 체인:  set_inbound_win_registers() → [이 함수]
 */
static u32 brcm_bar_reg_offset(int bar)
{
	if (bar <= 3) /* [한국어] BAR1~3 은 한 덩어리로 이어져 있다 */
		return PCIE_MISC_RC_BAR1_CONFIG_LO + 8 * (bar - 1); /* [한국어] BAR1 을 기준으로 8바이트 간격(LO/HI 두 워드)으로 센다 */
	else /* [한국어] BAR4 부터는 자리가 끊겨 있다 */
		return PCIE_MISC_RC_BAR4_CONFIG_LO + 8 * (bar - 4); /* [한국어] BAR4 를 새 기준으로 삼아 같은 간격으로 센다 */
}

/* [한국어]
 * brcm_ubus_reg_offset - BAR 번호로 UBUS remap 레지스터 오프셋을 구한다
 *
 * @bar: 1부터 시작하는 BAR 번호.
 * @return: 그 BAR 의 UBUS_BAR_CONFIG_REMAP 레지스터 오프셋.
 *
 * brcm_bar_reg_offset() 과 같은 모양이고, 대상 레지스터군만 다르다.
 * 1~3 과 4 이상에서 기준점이 갈리는 이유도 같다.
 *
 * UBUS 는 SoC 내부 버스로, 이 레지스터가 "그 BAR 로 들어온 PCI 주소를
 * 내부 버스의 어느 주소로 보낼 것인가" 를 정한다. BCM7712 만 이 설정이
 * 필요해 그 칩에서만 불린다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  set_inbound_win_registers() → [이 함수]
 */
static u32 brcm_ubus_reg_offset(int bar)
{
	if (bar <= 3) /* [한국어] BAR1~3 은 한 덩어리 */
		return PCIE_MISC_UBUS_BAR1_CONFIG_REMAP + 8 * (bar - 1); /* [한국어] BAR1 remap 을 기준으로 8바이트 간격 */
	else /* [한국어] BAR4 부터는 다른 자리 */
		return PCIE_MISC_UBUS_BAR4_CONFIG_REMAP + 8 * (bar - 4); /* [한국어] BAR4 remap 을 새 기준으로 삼는다 */
}

/* [한국어]
 * set_inbound_win_registers - 계산해 둔 안쪽 창을 하드웨어에 적는다
 *
 * @pcie:             컨트롤러 상태.
 * @inbound_wins:     get_inbound_wins() 가 채운 배열.
 * @num_inbound_wins: 그 함수가 돌려준 개수.
 *
 * 인덱스가 1부터인 것이 배열 규약과 맞는다 — 0번은 쓰지 않는다.
 *
 * 창 하나마다 두(또는 네) 워드를 적는다.
 *
 *   LO 워드에 PCI 주소의 하위 32비트를 넣고, 그 안의 SIZE 필드를
 *   brcm_pcie_encode_ibar_size() 가 만든 비선형 값으로 바꿔 넣는다.
 *   주소의 하위 비트와 크기 필드가 한 워드를 나눠 쓰는 배치라, 주소가
 *   창 크기에 정렬되어 있어야 겹치지 않는다 — get_inbound_wins() 가
 *   그 정렬을 미리 확인하는 이유다.
 *
 *   HI 워드에 상위 32비트를 넣는다.
 *
 *   BCM7712 라면 UBUS remap 레지스터도 함께 적는다. 상류 주석이 그 차이를
 *   "대부분의 STB 칩은 아무것도 하지 않고, 7712 는 모든 BAR 를 설정해야
 *   한다" 로 정리해 둔다. 하위 워드에서 하위 12비트를 지우는 것은 그 자리가
 *   주소가 아니라 플래그 자리이기 때문이고, 실제로 그 자리에
 *   ACCESS_EN 비트를 켜 넣는다.
 *
 * writel_relaxed 를 쓴다. 여기서 적는 값들 사이에 순서 제약이 없고,
 * 링크가 서기 전이라 다른 주체가 이 설정을 볼 일도 없기 때문이다.
 * 순서가 필요한 지점에서는 호출자 쪽이 일반 writel 을 쓴다.
 *
 * 크기 0 인 창은 encode 가 0 을 돌려주므로 그대로 비활성이 된다 —
 * get_inbound_wins() 가 "끄고 싶은 창" 을 size 0 으로 적어 두는 방식이
 * 여기서 성립한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe/resume).
 *
 * 호출 체인:  brcm_pcie_setup() → [이 함수] → brcm_pcie_encode_ibar_size()
 */
static void set_inbound_win_registers(struct brcm_pcie *pcie,
				      const struct inbound_win *inbound_wins,
				      u8 num_inbound_wins)
{
	void __iomem *base = pcie->base; /* [한국어] 레지스터 기준 주소 */
	int i; /* [한국어] 창 인덱스. 0번을 쓰지 않으므로 1 부터 센다 */

	for (i = 1; i <= num_inbound_wins; i++) { /* [한국어] 채워진 창을 차례로 하드웨어에 적는다 */
		u64 pci_offset = inbound_wins[i].pci_offset; /* [한국어] 이 창의 PCI 쪽 시작 */
		u64 cpu_addr = inbound_wins[i].cpu_addr; /* [한국어] 이 창의 CPU 쪽 시작. 7712 에서만 쓰인다 */
		u64 size = inbound_wins[i].size; /* [한국어] 이 창의 크기. 0 이면 아래 encode 가 0 을 돌려주어 창이 꺼진다 */
		u32 reg_offset = brcm_bar_reg_offset(i); /* [한국어] 이 창의 CONFIG 레지스터 자리를 구한다 */
		u32 tmp = lower_32_bits(pci_offset); /* [한국어] 하위 워드는 PCI 주소의 하위 32비트로 시작한다 */

		u32p_replace_bits(&tmp, brcm_pcie_encode_ibar_size(size),
				  PCIE_MISC_RC_BAR1_CONFIG_LO_SIZE_MASK); /* [한국어] 그 워드의 SIZE 필드를 비선형 인코딩 값으로 채운다. 주소가 창 크기에 정렬되어 있어야 두 값이 겹치지 않는다 */

		/* Write low */
		writel_relaxed(tmp, base + reg_offset); /* [한국어] 주소 하위 32비트와 크기 필드를 한 워드로 적는다. 순서 제약이 없어 relaxed 판을 쓴다 */
		/* Write high */
		writel_relaxed(upper_32_bits(pci_offset), base + reg_offset + 4); /* [한국어] 같은 창의 상위 워드를 4바이트 뒤에 적는다 */

		/*
		 * Most STB chips:
		 *     Do nothing.
		 * 7712:
		 *     All of their BARs need to be set.
		 */
		if (pcie->cfg->soc_base == BCM7712) { /* [한국어] 7712 계열만 UBUS remap 설정이 더 필요하다 */
			/* BUS remap register settings */
			reg_offset = brcm_ubus_reg_offset(i); /* [한국어] 같은 창 번호의 UBUS remap 레지스터 자리로 바꾼다 */
			tmp = lower_32_bits(cpu_addr) & ~0xfff; /* [한국어] CPU 주소 하위 32비트에서 하위 12비트를 지운다 — 그 자리는 주소가 아니라 플래그 자리다 */
			tmp |= PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_ACCESS_EN_MASK; /* [한국어] 비워 둔 자리에 접근 허용 비트를 넣는다 */
			writel_relaxed(tmp, base + reg_offset); /* [한국어] 하위 워드를 적는다 */
			tmp = upper_32_bits(cpu_addr); /* [한국어] CPU 주소의 상위 32비트 */
			writel_relaxed(tmp, base + reg_offset + 4); /* [한국어] 상위 워드를 적는다. 이로써 이 BAR 로 들어온 요청이 내부 버스의 어느 주소로 갈지 정해진다 */
		}
	}
}

/* [한국어]
 * brcm_pcie_setup - 링크를 세우기 전 컨트롤러 전체를 구성한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다(-EINVAL 등).
 *
 * probe 와 resume 이 공유하는 큰 초기화 함수다. 순서 자체가 요구사항이라
 * 단계별로 읽어야 한다.
 *
 *   1) 브리지 리셋을 건다. 이후 설정이 리셋 상태에서 이뤄져야 한다.
 *   2) BCM2711 에서만 PERST# 를 명시적으로 건다. 상류 주석대로 부트로더가
 *      이미 풀어 두었을 수 있기 때문이다. 이 단계가 실패하면 브리지 리셋을
 *      풀어 놓고 돌아간다 — 걸어 둔 리셋을 남기지 않기 위해서다.
 *   3) 브리지 리셋을 푼다.
 *   4) SerDes 의 IDDQ(전류 차단) 를 해제해 PHY 를 켠다. BMIPS 계열은 그
 *      비트 자리가 달라 마스크가 갈린다. 켜고 나서 안정될 때까지 기다린다.
 *   5) SCB 최대 버스트 크기를 정한다. 상류 주석이 인코딩이 칩마다 다름을
 *      밝힌다 — 일반 칩은 0=128/1=256/2=512, BCM7278 은 1=128/2=256/3=512
 *      라 같은 512 바이트를 뜻하는 값이 2 와 3 으로 갈린다.
 *   6) MISC_CTRL 의 다섯 필드를 한 번에 세운다(상류 주석이 이름을 나열).
 *      SCB_ACCESS_EN 은 안쪽 접근 허용, CFG_READ_UR_MODE 는 실패한 config
 *      읽기를 UR 로 처리, 나머지는 완료 패킷 경계 관련 설정이다.
 *   7) 안쪽 창을 계산해 하드웨어에 적는다.
 *   8) RC 모드인지 확인한다. EP 로 배선된 보드라면 이 드라이버가 다룰 수
 *      없으므로 여기서 그만둔다.
 *   9) memc 개수만큼 SCB_SIZE 필드를 채운다. ilog2 에서 15 를 빼는 것이
 *      encode_ibar_size 의 큰 구간과 같은 규칙이라, 64KB 를 1 로 세는
 *      같은 인코딩이다. memc 는 최대 셋이라 분기도 셋이다.
 *  10) MSI 목표 주소를 고른다. 상류 주석대로 32비트만 다룰 수 있는 장치가
 *      있어 4GB 아래가 낫지만, 안쪽 창이 그 자리를 이미 덮고 있으면 겹칠 수
 *      없다. 그래서 창이 4GB 위에 있거나 창 끝이 4GB 아래일 때만 낮은 쪽을
 *      쓰고, 그렇지 않으면 높은 쪽을 쓴다.
 *  11) DT 에 "aspm-no-l0s" 가 있으면 링크 능력에서 L0s 를 지운다. 그러면
 *      OS 가 L0s 를 켜려 하지 않는다.
 *  12) 링크 폭을 정한다. 상류 주석대로 기본은 하드웨어가 협상한 값을 쓰고,
 *      DT 에 "num-lanes" 가 있으면 칩의 기본 능력 정보가 틀렸다고 보고
 *      그 값으로 덮는다. 이때 PHY 의 P2 powerdown 설정도 함께 켠다.
 *  13) 클래스 코드를 0x060400(PCI-to-PCI 브리지)으로 바꾼다. 상류 주석대로
 *      기본값이 EP 모드라, 그대로 두면 config 공간에서 브리지로 보이지 않는다.
 *  14) 바깥 창을 연다. BMIPS 계열은 창 하나가 최대 128MB 라 큰 자원을
 *      128MB 조각으로 쪼개 여러 창에 나눠 담고, 그 한 자원만 처리한 뒤
 *      루프를 끝낸다. 다른 칩은 자원 하나에 창 하나다.
 *  15) 안쪽 창의 엔디언을 리틀엔디언으로 못박는다.
 *  16) 칩 전용 후처리가 있으면 부른다(BCM2712 뿐).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 호출 체인:  brcm_pcie_probe() / brcm_pcie_resume_noirq() → [이 함수]
 *               → brcm_pcie_get_inbound_wins() → set_inbound_win_registers()
 *               → brcm_pcie_set_outbound_win() → cfg->post_setup()
 */
static int brcm_pcie_setup(struct brcm_pcie *pcie)
{
	struct inbound_win inbound_wins[PCIE_BRCM_MAX_INBOUND_WINS];
	void __iomem *base = pcie->base;
	struct pci_host_bridge *bridge; /* [한국어] 바깥 창을 훑을 때 쓸 호스트 브리지 */
	struct resource_entry *entry; /* [한국어] 그 창 자원 순회용 항목 */
	u32 tmp, burst, num_lanes, num_lanes_cap; /* [한국어] 읽고-고쳐-쓸 임시 값, 버스트 인코딩, DT 가 요청한 레인 수, 하드웨어가 광고하는 레인 수 */
	u8 num_out_wins = 0; /* [한국어] 지금까지 연 바깥 창 개수 */
	int num_inbound_wins = 0; /* [한국어] get_inbound_wins 가 돌려준 안쪽 창 개수 */
	int memc, ret; /* [한국어] memc 루프 인덱스와 하위 호출 결과 */

	/* Reset the bridge */
	ret = brcm_pcie_bridge_sw_init_set(pcie, 1); /* [한국어] 이후 설정이 리셋 상태에서 이뤄져야 하므로 먼저 리셋을 건다 */
	if (ret) /* [한국어] 리셋조차 못 걸면 */
		return ret; /* [한국어] 아무것도 바꾸지 않은 상태라 그대로 돌아간다 */

	/* Ensure that PERST# is asserted; some bootloaders may deassert it. */
	if (pcie->cfg->soc_base == BCM2711) { /* [한국어] 이 칩만 부트로더가 PERST# 를 풀어 두는 일이 있다 */
		ret = pcie->cfg->perst_set(pcie, 1); /* [한국어] 확실히 걸어 둔다 */
		if (ret) { /* [한국어] 실패했으면 */
			pcie->cfg->bridge_sw_init_set(pcie, 0); /* [한국어] 방금 건 브리지 리셋을 되돌려 놓는다 — 걸어 둔 리셋을 남기지 않기 위해서다 */
			return ret; /* [한국어] 그다음 오류를 올린다 */
		}
	}

	usleep_range(100, 200); /* [한국어] 리셋이 전파될 시간을 준다 */

	/* Take the bridge out of reset */
	ret = brcm_pcie_bridge_sw_init_set(pcie, 0); /* [한국어] 설정을 시작할 수 있게 브리지를 리셋에서 꺼낸다 */
	if (ret) /* [한국어] 풀지 못했으면 */
		return ret; /* [한국어] 더 진행할 수 없다 */

	tmp = readl(base + HARD_DEBUG(pcie)); /* [한국어] SerDes IDDQ 비트가 있는 레지스터를 읽는다 */
	if (is_bmips(pcie)) /* [한국어] BMIPS 계열은 그 비트 자리가 다르다 */
		tmp &= ~PCIE_BMIPS_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK; /* [한국어] BMIPS 자리의 IDDQ 를 푼다 */
	else
		tmp &= ~PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK; /* [한국어] 일반 자리의 IDDQ 를 푼다 */
	writel(tmp, base + HARD_DEBUG(pcie)); /* [한국어] 되쓴다 — 이로써 PHY 전류가 흐른다 */
	/* Wait for SerDes to be stable */
	usleep_range(100, 200); /* [한국어] SerDes 가 안정될 시간을 준다 */

	/*
	 * SCB_MAX_BURST_SIZE is a two bit field.  For GENERIC chips it
	 * is encoded as 0=128, 1=256, 2=512, 3=Rsvd, for BCM7278 it
	 * is encoded as 0=Rsvd, 1=128, 2=256, 3=512.
	 */
	if (is_bmips(pcie)) /* [한국어] BMIPS 계열 */
		burst = 0x1; /* [한국어] 그 칩들의 인코딩에서 256 바이트 */
	else if (pcie->cfg->soc_base == BCM2711) /* [한국어] 라즈베리파이 4 계열 */
		burst = 0x0; /* [한국어] 128 바이트 */
	else if (pcie->cfg->soc_base == BCM7278) /* [한국어] BCM7278 은 인코딩이 한 칸 밀려 있다 */
		burst = 0x3; /* [한국어] 그 칩에서 512 바이트는 3 */
	else /* [한국어] 그 밖의 칩 */
		burst = 0x2; /* [한국어] 일반 인코딩에서 512 바이트는 2 */

	/*
	 * Set SCB_MAX_BURST_SIZE, CFG_READ_UR_MODE, SCB_ACCESS_EN,
	 * RCB_MPS_MODE, RCB_64B_MODE
	 */
	tmp = readl(base + PCIE_MISC_MISC_CTRL); /* [한국어] 여러 필드를 한 워드에서 고쳐야 하므로 먼저 읽는다 */
	u32p_replace_bits(&tmp, 1, PCIE_MISC_MISC_CTRL_SCB_ACCESS_EN_MASK); /* [한국어] 내부 버스 접근을 허용한다 */
	u32p_replace_bits(&tmp, 1, PCIE_MISC_MISC_CTRL_CFG_READ_UR_MODE_MASK); /* [한국어] 실패한 config 읽기를 Unsupported Request 로 처리하게 한다 */
	u32p_replace_bits(&tmp, burst, PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_MASK); /* [한국어] 위에서 고른 버스트 인코딩을 넣는다 */
	u32p_replace_bits(&tmp, 1, PCIE_MISC_MISC_CTRL_PCIE_RCB_MPS_MODE_MASK); /* [한국어] 완료 패킷 경계를 MPS 에 맞춘다 */
	u32p_replace_bits(&tmp, 1, PCIE_MISC_MISC_CTRL_PCIE_RCB_64B_MODE_MASK); /* [한국어] 완료 패킷 경계 64바이트 모드를 켠다 */
	writel(tmp, base + PCIE_MISC_MISC_CTRL); /* [한국어] 다섯 필드를 한 번에 되쓴다 */

	num_inbound_wins = brcm_pcie_get_inbound_wins(pcie, inbound_wins); /* [한국어] DT 의 dma-ranges 를 안쪽 창 목록으로 바꾼다 */
	if (num_inbound_wins < 0) /* [한국어] DT 가 잘못되었거나 정렬이 어긋났으면 */
		return num_inbound_wins; /* [한국어] 그 오류를 그대로 올린다 */

	set_inbound_win_registers(pcie, inbound_wins, num_inbound_wins); /* [한국어] 계산한 창들을 하드웨어에 적는다 */

	if (!brcm_pcie_rc_mode(pcie)) { /* [한국어] 보드가 이 컨트롤러를 EP 로 배선했으면 */
		dev_err(pcie->dev, "PCIe RC controller misconfigured as Endpoint\n"); /* [한국어] 이 드라이버는 RC 만 지원한다 */
		return -EINVAL; /* [한국어] 다룰 수 없으므로 그만둔다 */
	}

	tmp = readl(base + PCIE_MISC_MISC_CTRL); /* [한국어] memc 크기 필드가 위 다섯 필드와 같은 레지스터에 있어 다시 읽는다 */
	for (memc = 0; memc < pcie->num_memc; memc++) { /* [한국어] memc 마다 하나씩 채운다 */
		u32 scb_size_val = ilog2(pcie->memc_size[memc]) - 15; /* [한국어] 64KB 를 1 로 세는 인코딩. encode_ibar_size 의 큰 구간과 같은 규칙이다 */

		if (memc == 0) /* [한국어] 첫 번째 memc */
			u32p_replace_bits(&tmp, scb_size_val, SCB_SIZE_MASK(0)); /* [한국어] SCB0 크기 필드에 넣는다 */
		else if (memc == 1) /* [한국어] 두 번째 */
			u32p_replace_bits(&tmp, scb_size_val, SCB_SIZE_MASK(1)); /* [한국어] SCB1 크기 필드 */
		else if (memc == 2) /* [한국어] 세 번째 */
			u32p_replace_bits(&tmp, scb_size_val, SCB_SIZE_MASK(2)); /* [한국어] SCB2 크기 필드. 필드가 셋뿐이라 그 이상은 반영되지 않는다 */
	}
	writel(tmp, base + PCIE_MISC_MISC_CTRL); /* [한국어] 채운 크기 필드들을 되쓴다 */

	/*
	 * We ideally want the MSI target address to be located in the 32bit
	 * addressable memory area. Some devices might depend on it. This is
	 * possible either when the inbound window is located above the lower
	 * 4GB or when the inbound area is smaller than 4GB (taking into
	 * account the rounding-up we're forced to perform).
	 */
	if (inbound_wins[2].pci_offset >= SZ_4G || /* [한국어] 안쪽 창이 4GB 위에 있거나 */
	    (inbound_wins[2].size + inbound_wins[2].pci_offset) < SZ_4G) /* [한국어] 창 끝이 4GB 아래이면 낮은 자리가 비어 있다 */
		pcie->msi_target_addr = BRCM_MSI_TARGET_ADDR_LT_4GB; /* [한국어] 32비트 장치도 쓸 수 있는 4GB 아래 주소를 고른다 */
	else
		pcie->msi_target_addr = BRCM_MSI_TARGET_ADDR_GT_4GB; /* [한국어] 창이 낮은 자리를 덮고 있으면 겹칠 수 없어 4GB 위를 쓴다 */


	/* Don't advertise L0s capability if 'aspm-no-l0s' */
	tmp = readl(base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY); /* [한국어] 링크 능력 레지스터를 읽는다 */
	if (of_property_read_bool(pcie->np, "aspm-no-l0s")) /* [한국어] DT 가 L0s 를 쓰지 말라고 했으면 */
		tmp &= ~PCI_EXP_LNKCAP_ASPM_L0S; /* [한국어] 능력에서 L0s 비트를 지워 광고하지 않는다 */
	writel(tmp, base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY); /* [한국어] 되쓴다. 속성이 없으면 읽은 값을 그대로 되쓰는 셈이라 무해하다 */

	/* 'tmp' still holds the contents of PRIV1_LINK_CAPABILITY */
	num_lanes_cap = u32_get_bits(tmp, PCIE_RC_CFG_PRIV1_LINK_CAPABILITY_MAX_LINK_WIDTH_MASK); /* [한국어] 상류 주석대로 tmp 에 아직 그 레지스터 값이 있어, 하드웨어가 광고하는 최대 레인 수를 꺼낸다 */
	num_lanes = 0; /* [한국어] DT 속성이 없을 때를 대비해 0 으로 둔다 */

	/*
	 * Use hardware negotiated Max Link Width value by default.  If the
	 * "num-lanes" DT property is present, assume that the chip's default
	 * link width capability information is incorrect/undesired and use the
	 * specified value instead.
	 */
	if (!of_property_read_u32(pcie->np, "num-lanes", &num_lanes) && /* [한국어] DT 에 num-lanes 가 있고 */
	    num_lanes && num_lanes <= 4 && num_lanes_cap != num_lanes) { /* [한국어] 값이 1~4 범위이며 하드웨어 값과 다를 때만 덮는다 */
		u32p_replace_bits(&tmp, num_lanes,
			PCIE_RC_CFG_PRIV1_LINK_CAPABILITY_MAX_LINK_WIDTH_MASK); /* [한국어] 능력의 최대 레인 폭 필드를 그 값으로 바꾼다 */
		writel(tmp, base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY); /* [한국어] 되쓴다 */
		tmp = readl(base + PCIE_RC_PL_REG_PHY_CTL_1); /* [한국어] PHY 제어 레지스터를 읽는다 */
		u32p_replace_bits(&tmp, 1,
			PCIE_RC_PL_REG_PHY_CTL_1_REG_P2_POWERDOWN_ENA_NOSYNC_MASK); /* [한국어] P2 파워다운을 켠다 — 레인 수를 줄일 때 쓰지 않는 레인의 전력을 내리기 위한 것으로 보인다 */
		writel(tmp, base + PCIE_RC_PL_REG_PHY_CTL_1); /* [한국어] 되쓴다 */
	}

	/*
	 * For config space accesses on the RC, show the right class for
	 * a PCIe-PCIe bridge (the default setting is to be EP mode).
	 */
	tmp = readl(base + PCIE_RC_CFG_PRIV1_ID_VAL3); /* [한국어] ID/클래스 레지스터를 읽는다 */
	u32p_replace_bits(&tmp, 0x060400,
			  PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_CODE_MASK); /* [한국어] 클래스 코드를 0x060400 으로 바꾼다 — 기본값이 EP 모드라 그대로 두면 브리지로 보이지 않는다 */
	writel(tmp, base + PCIE_RC_CFG_PRIV1_ID_VAL3); /* [한국어] 되쓴다 */

	bridge = pci_host_bridge_from_priv(pcie); /* [한국어] 바깥 창 자원이 담긴 호스트 브리지를 되찾는다 */
	resource_list_for_each_entry(entry, &bridge->windows) { /* [한국어] DT 가 준 창 자원을 하나씩 훑는다 */
		struct resource *res = entry->res; /* [한국어] 이 항목의 자원 */

		if (resource_type(res) != IORESOURCE_MEM) /* [한국어] 메모리 창이 아니면 */
			continue; /* [한국어] 이 컨트롤러의 바깥 창은 메모리만 다룬다 */

		if (num_out_wins >= BRCM_NUM_PCIE_OUT_WINS) { /* [한국어] 하드웨어 창 수를 넘었으면 */
			dev_err(pcie->dev, "too many outbound wins\n"); /* [한국어] DT 가 감당 못 할 만큼 창을 요구한 것이다 */
			return -EINVAL; /* [한국어] 그만둔다 */
		}

		if (is_bmips(pcie)) { /* [한국어] BMIPS 계열 */
			u64 start = res->start; /* [한국어] 이 자원의 시작 주소 */
			unsigned int j, nwins = resource_size(res) / SZ_128M; /* [한국어] 128MB 조각으로 나눈 개수 */

			/* bmips PCIe outbound windows have a 128MB max size */
			if (nwins > BRCM_NUM_PCIE_OUT_WINS) /* [한국어] 조각 수가 창 수를 넘으면 */
				nwins = BRCM_NUM_PCIE_OUT_WINS; /* [한국어] 열 수 있는 만큼만 연다 */
			for (j = 0; j < nwins; j++, start += SZ_128M) /* [한국어] 128MB 씩 앞으로 가며 */
				brcm_pcie_set_outbound_win(pcie, j, start,
							   start - entry->offset,
							   SZ_128M); /* [한국어] 창을 하나씩 연다. PCI 주소는 CPU 주소에서 오프셋을 뺀 값이다 */
			break; /* [한국어] BMIPS 는 창을 이 자원 하나로 모두 써 버려 다음 자원을 볼 필요가 없다 */
		}
		brcm_pcie_set_outbound_win(pcie, num_out_wins, res->start,
					   res->start - entry->offset,
					   resource_size(res)); /* [한국어] 그 밖의 칩은 자원 하나에 창 하나를 그대로 연다 */
		num_out_wins++; /* [한국어] 다음 창 번호로 넘어간다 */
	}

	/* PCIe->SCB endian mode for inbound window */
	tmp = readl(base + PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1); /* [한국어] 엔디언 모드 필드가 있는 벤더 레지스터를 읽는다 */
	u32p_replace_bits(&tmp, PCIE_RC_CFG_VENDOR_SPECIFIC_REG1_LITTLE_ENDIAN,
		PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1_ENDIAN_MODE_BAR2_MASK); /* [한국어] 안쪽 창(BAR2)의 엔디언을 리틀엔디언으로 못박는다 */
	writel(tmp, base + PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1); /* [한국어] 되쓴다 */

	if (pcie->cfg->post_setup) { /* [한국어] 칩 전용 후처리가 있으면(BCM2712 뿐) */
		ret = pcie->cfg->post_setup(pcie); /* [한국어] 그것을 부른다 */
		if (ret < 0) /* [한국어] 후처리가 실패했으면 */
			return ret; /* [한국어] setup 전체가 실패한다 */
	}

	return 0; /* [한국어] 모든 구성이 끝났다 */
}

/*
 * This extends the timeout period for an access to an internal bus.  This
 * access timeout may occur during L1SS sleep periods, even without the
 * presence of a PCIe access.
 */
/* [한국어]
 * brcm_extend_rbus_timeout - 내부 버스 접근 타임아웃을 늘린다
 *
 * @pcie: 컨트롤러 상태.
 *
 * 상류 주석이 이유를 밝힌다 — 내부 버스 접근 타임아웃이 L1SS 수면 구간에
 * 걸릴 수 있고, PCIe 접근이 없어도 그렇다. 기본값이 짧으면 L1SS 를 켠
 * 채로는 정상 동작이 타임아웃으로 잘못 판정된다.
 *
 * 레지스터 자리를 상수로 두지 않고 RGR1_SW_INIT_1 에서 8 을 빼서 구한다.
 * 상류 주석대로 그 두 레지스터 앞에 있기 때문인데, RGR1 자리 자체가
 * 변종마다 달라 상대 위치로 구하는 편이 표를 하나 더 두는 것보다 낫다.
 *
 * 단위 변환도 상류 주석에 있다 — 한 눈금이 1/216,000,000 초라, 마이크로초
 * 값에 216 을 곱하면 눈금 수가 된다. 4초로 잡는 것은 L1SS 용 설정값이라고
 * 변수 주석이 밝힌다.
 *
 * BCM7712 에는 이 타이머 자체가 없어(상류 주석) 그냥 돌아간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. brcm_config_clkreq() 의 default 갈래에서만
 * 불린다 — L1SS 를 켜는 그 갈래에서만 필요하기 때문이다.
 *
 * 호출 체인:  brcm_config_clkreq() → [이 함수] → writel()
 */
static void brcm_extend_rbus_timeout(struct brcm_pcie *pcie)
{
	/* TIMEOUT register is two registers before RGR1_SW_INIT_1 */
	const unsigned int REG_OFFSET = PCIE_RGR1_SW_INIT_1(pcie) - 8; /* [한국어] 상류 주석대로 타임아웃 레지스터가 RGR1_SW_INIT_1 의 두 레지스터 앞(8바이트 앞)에 있다. RGR1 자리가 변종마다 달라 상대 위치로 구한다 */
	u32 timeout_us = 4000000; /* 4 seconds, our setting for L1SS */

	/* 7712 does not have this (RGR1) timer */
	if (pcie->cfg->soc_base == BCM7712) /* [한국어] 이 칩에는 RGR1 계열 타이머 자체가 없다 */
		return; /* [한국어] 건드릴 레지스터가 없으므로 그냥 돌아간다 */

	/* Each unit in timeout register is 1/216,000,000 seconds */
	writel(216 * timeout_us, pcie->base + REG_OFFSET); /* [한국어] 상류 주석대로 한 눈금이 1/216,000,000 초라, 마이크로초 값에 216 을 곱하면 눈금 수가 된다 */
}

/* [한국어]
 * brcm_config_clkreq - CLKREQ# 동작 방식을 DT 지시대로 고른다
 *
 * @pcie: 컨트롤러 상태.
 *
 * 이 드라이버가 다른 호스트 드라이버보다 눈에 띄게 공들이는 자리다.
 * CLKREQ# 신호를 어떻게 다루느냐에 따라 절전 깊이와 안정성이 맞바뀌는데,
 * 어느 쪽을 택할지는 보드가 알아야 해서 DT 의 "brcm,clkreq-mode" 로 받는다.
 *
 * 세 가지 값을 상류가 각각 긴 주석으로 설명해 두었다.
 *
 *   "no-l1ss"  클록 전원 관리와 L0s, L1 은 되지만 L1 서브스테이트는 안 된다.
 *              하류 장치가 L1SS 를 지원하고 OS 가 그것을 켜면 PCIe 통신이
 *              갑자기 멎어 시스템이 멈출 수 있다. 그래서 CLKREQ 디버그
 *              비트를 켜는 데 그치지 않고, ROOT_CAP 의 L1SS_MODE 필드를
 *              2 로 바꿔 L1 서브스테이트를 아예 광고하지 않는다 — 상류
 *              주석대로 OS 가 그것을 설정하려다 실패하거나 멈추는 것을
 *              막기 위해서다.
 *   "default"  L0s/L1/L1SS 는 되지만 클록 전원 관리 규격은 못 맞춘다.
 *              상류 주석이 구체적으로 짚는다 — PCIe 스펙 3.2.5.2.2
 *              "Dynamic Clock Control" 의 Tclron 최대 400ns 를 못 지킬 수
 *              있다. 옛 장치에서만 문제가 되는 드문 상황이라고 덧붙인다.
 *              이 갈래에서 L1SS 를 켜므로, 함께 rbus 타임아웃을 늘린다.
 *   "safe"     절전 없음. refclk 을 RC 가 무조건 구동한다.
 *
 * 기본값이 "default" 인 것에 주의. of_property_read_string 이 실패해도
 * mode 변수의 초기값이 그대로 남아 그 갈래로 간다. 다만 -EINVAL(속성
 * 없음)이 아닌 실패는 DT 문자열이 잘못된 것이므로 오류를 찍고 "safe" 로
 * 떨어뜨린다. 알 수 없는 문자열도 마찬가지로 "safe" 다 — 마지막 else 에서
 * 한 번 더 걸러진다.
 *
 * 두 모드 비트를 먼저 지우고 시작하는 것이 요령이다. 그러면 아무 비트도
 * 켜지 않은 상태가 곧 "safe" 라, else 갈래가 아무 일도 하지 않아도 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(링크가 선 직후).
 *
 * 호출 체인:  brcm_pcie_start_link() → [이 함수] → brcm_extend_rbus_timeout()
 */
static void brcm_config_clkreq(struct brcm_pcie *pcie)
{
	static const char err_msg[] = "invalid 'brcm,clkreq-mode' DT string\n"; /* [한국어] 잘못된 DT 문자열을 알릴 때 두 곳에서 같은 문구를 쓴다 */
	const char *mode = "default"; /* [한국어] DT 속성이 없으면 이 초깃값이 그대로 남아 default 갈래로 간다 */
	u32 clkreq_cntl; /* [한국어] HARD_DEBUG 레지스터를 읽고 고쳐 쓸 값 */
	int ret, tmp; /* [한국어] DT 읽기 결과와 ROOT_CAP 을 고칠 때 쓰는 임시 값 */

	ret = of_property_read_string(pcie->np, "brcm,clkreq-mode", &mode); /* [한국어] DT 에서 모드 문자열을 읽는다 */
	if (ret && ret != -EINVAL) { /* [한국어] -EINVAL(속성 없음)이 아닌 실패는 DT 가 잘못 적힌 경우다 */
		dev_err(pcie->dev, err_msg); /* [한국어] 무엇이 잘못인지 알린다 */
		mode = "safe"; /* [한국어] 믿을 수 없는 값이므로 절전 없는 쪽으로 떨어뜨린다 */
	}

	/* Start out assuming safe mode (both mode bits cleared) */
	clkreq_cntl = readl(pcie->base + HARD_DEBUG(pcie)); /* [한국어] 현재 값을 읽는다 */
	clkreq_cntl &= ~PCIE_CLKREQ_MASK; /* [한국어] 두 모드 비트를 지워 safe 상태에서 시작한다 — 그래야 아래 else 갈래가 아무 일도 하지 않아도 된다 */

	if (strcmp(mode, "no-l1ss") == 0) { /* [한국어] L1 서브스테이트를 쓰지 않는 모드 */
		/*
		 * "no-l1ss" -- Provides Clock Power Management, L0s, and
		 * L1, but cannot provide L1 substate (L1SS) power
		 * savings. If the downstream device connected to the RC is
		 * L1SS capable AND the OS enables L1SS, all PCIe traffic
		 * may abruptly halt, potentially hanging the system.
		 */
		clkreq_cntl |= PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_MASK; /* [한국어] CLKREQ 디버그 비트를 켠다 */
		/*
		 * We want to un-advertise L1 substates because if the OS
		 * tries to configure the controller into using L1 substate
		 * power savings it may fail or hang when the RC HW is in
		 * "no-l1ss" mode.
		 */
		tmp = readl(pcie->base + PCIE_RC_CFG_PRIV1_ROOT_CAP); /* [한국어] 루트 능력 레지스터를 읽는다 */
		u32p_replace_bits(&tmp, 2, PCIE_RC_CFG_PRIV1_ROOT_CAP_L1SS_MODE_MASK); /* [한국어] L1SS 모드 필드를 2 로 바꿔 L1 서브스테이트를 광고하지 않게 한다 */
		writel(tmp, pcie->base + PCIE_RC_CFG_PRIV1_ROOT_CAP); /* [한국어] 되쓴다. 이것이 없으면 OS 가 L1SS 를 켜려다 통신이 멎을 수 있다 */

	} else if (strcmp(mode, "default") == 0) { /* [한국어] L0s/L1/L1SS 를 모두 쓰는 기본 모드 */
		/*
		 * "default" -- Provides L0s, L1, and L1SS, but not
		 * compliant to provide Clock Power Management;
		 * specifically, may not be able to meet the Tclron max
		 * timing of 400ns as specified in "Dynamic Clock Control",
		 * section 3.2.5.2.2 of the PCIe spec.  This situation is
		 * atypical and should happen only with older devices.
		 */
		clkreq_cntl |= PCIE_MISC_HARD_PCIE_HARD_DEBUG_L1SS_ENABLE_MASK; /* [한국어] L1SS 활성 비트를 켠다 */
		brcm_extend_rbus_timeout(pcie); /* [한국어] L1SS 수면 중 내부 버스 타임아웃이 잘못 나지 않게 타이머를 늘린다 */

	} else { /* [한국어] 그 밖의 문자열은 모두 safe 로 본다 */
		/*
		 * "safe" -- No power savings; refclk is driven by RC
		 * unconditionally.
		 */
		if (strcmp(mode, "safe") != 0) /* [한국어] "safe" 도 아닌 알 수 없는 문자열이면 */
			dev_err(pcie->dev, err_msg); /* [한국어] DT 가 잘못 적힌 것이므로 알린다 */
		mode = "safe"; /* [한국어] 아래 로그에 실제 적용된 모드를 정확히 찍기 위해 값을 맞춰 둔다 */
	}
	writel(clkreq_cntl, pcie->base + HARD_DEBUG(pcie)); /* [한국어] 고른 모드 비트를 한 번에 되쓴다. 세 갈래가 모두 이 한 줄로 모인다 */

	dev_info(pcie->dev, "clkreq-mode set to %s\n", mode); /* [한국어] 어떤 모드로 정해졌는지 사용자에게 알린다 */
}

/* [한국어]
 * brcm_pcie_start_link - PERST# 를 풀고 링크가 설 때까지 기다린다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, -ENODEV 는 링크가 서지 않은 경우. PERST# 조작 실패는 그 오류.
 *
 * setup 이 끝난 뒤 실제로 링크를 세우는 단계다.
 *
 *   1) DT 가 세대를 지정했다면(pcie->gen) 그 세대로 제한한다. 링크가
 *      서기 전에 해야 협상에 반영된다.
 *   2) PERST# 를 풀어 하류 장치를 리셋에서 꺼낸다.
 *   3) PCIE_RESET_CONFIG_WAIT_MS 만큼 기다린다. 스펙이 정한 리셋 후
 *      대기 시간이다.
 *   4) 상류 주석대로 RC/EP 에 시간을 더 준다 — 5ms 씩 최대 100ms 동안
 *      링크 상태를 되풀이해 확인한다. 먼저 서면 바로 빠져나온다.
 *   5) 그래도 안 서면 -ENODEV. 슬롯이 비어 있는 흔한 경우라 probe 가
 *      이것을 치명적으로 다루지 않을 수 있다.
 *   6) 링크가 선 뒤에야 CLKREQ# 모드를 정한다. 링크가 없는 상태에서
 *      절전 설정을 건드릴 이유가 없기 때문이다.
 *   7) DT 가 SSC 를 요청했다면 켠다. 실패해도 링크는 살아 있으므로
 *      오류를 찍고 계속 간다 — ssc_good 이 아래 로그에만 쓰인다.
 *   8) 링크 상태 레지스터에서 실제 속도와 폭을 읽어 알린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 최대 100ms 이상 잠든다.
 *
 * 호출 체인:  brcm_pcie_probe() / brcm_pcie_resume_noirq() → [이 함수]
 *               → cfg->perst_set() → brcm_config_clkreq() → brcm_pcie_set_ssc()
 */
static int brcm_pcie_start_link(struct brcm_pcie *pcie)
{
	struct device *dev = pcie->dev; /* [한국어] 로그용 device */
	void __iomem *base = pcie->base; /* [한국어] 레지스터 기준 주소 */
	u16 nlw, cls, lnksta; /* [한국어] 링크 상태 레지스터에서 뽑을 레인 수, 속도 코드, 그리고 원본 워드 */
	bool ssc_good = false; /* [한국어] SSC 가 실제로 켜졌는지. 아래 로그에만 쓰인다 */
	int ret, i; /* [한국어] 하위 호출 결과와 링크 대기 루프 인덱스 */

	/* Limit the generation if specified */
	if (pcie->gen) /* [한국어] DT 가 세대를 지정했으면. [관찰] 이 트리에서 pcie->gen 에 0 이 아닌 값이 들어가는 곳이 없어 실제로는 이 분기에 들어가지 않는다 */
		brcm_pcie_set_gen(pcie, pcie->gen); /* [한국어] 링크가 서기 전에 세대를 제한해야 협상에 반영된다 */

	/* Unassert the fundamental reset */
	ret = pcie->cfg->perst_set(pcie, 0); /* [한국어] PERST# 를 풀어 하류 장치를 리셋에서 꺼낸다 */
	if (ret) /* [한국어] PERST# 조작이 실패했으면 */
		return ret; /* [한국어] 링크를 세울 수 없다 */

	msleep(PCIE_RESET_CONFIG_WAIT_MS); /* [한국어] 스펙이 정한 리셋 후 대기 시간만큼 기다린다 */

	/*
	 * Give the RC/EP even more time to wake up, before trying to
	 * configure RC.  Intermittently check status for link-up, up to a
	 * total of 100ms.
	 */
	for (i = 0; i < 100 && !brcm_pcie_link_up(pcie); i += 5) /* [한국어] 5ms 씩 최대 100ms 동안 링크를 확인한다. 먼저 서면 곧바로 빠져나온다 */
		msleep(5); /* [한국어] 한 번에 5ms 씩 잔다 */

	if (!brcm_pcie_link_up(pcie)) { /* [한국어] 그래도 서지 않았으면 */
		dev_err(dev, "link down\n"); /* [한국어] 슬롯이 비어 있는 흔한 경우다 */
		return -ENODEV; /* [한국어] probe 가 이 오류를 받아 정리로 간다 */
	}

	brcm_config_clkreq(pcie); /* [한국어] 링크가 선 뒤에야 절전 모드를 정한다 */

	if (pcie->ssc) { /* [한국어] DT 가 SSC 를 요청했으면 */
		ret = brcm_pcie_set_ssc(pcie); /* [한국어] 켜 본다 */
		if (ret == 0) /* [한국어] 성공했으면 */
			ssc_good = true; /* [한국어] 아래 로그에 표시한다 */
		else
			dev_err(dev, "failed attempt to enter ssc mode\n"); /* [한국어] 실패해도 링크는 살아 있으므로 알리고 계속 간다 */
	}

	lnksta = readw(base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKSTA); /* [한국어] 표준 링크 상태 레지스터를 읽는다 */
	cls = FIELD_GET(PCI_EXP_LNKSTA_CLS, lnksta); /* [한국어] 협상된 링크 속도 코드 */
	nlw = FIELD_GET(PCI_EXP_LNKSTA_NLW, lnksta); /* [한국어] 협상된 레인 수 */
	dev_info(dev, "link up, %s x%u %s\n",
		 pci_speed_string(pcie_get_link_speed(cls)), nlw,
		 ssc_good ? "(SSC)" : "(!SSC)"); /* [한국어] 실제 속도와 폭, 그리고 SSC 여부를 알린다 */

	return 0; /* [한국어] 링크가 섰다 */
}

/* [한국어] 하류 슬롯에 연결할 수 있는 전원 이름들.
 * PCIe 슬롯의 전원 규격 그대로 3.3V 주전원, 3.3V 보조, 12V 셋이다.
 * alloc_subdev_regulators() 가 이 문자열들을 regulator_bulk_data 에 꽂고,
 * regulator_bulk_get() 이 그 이름으로 DT 에서 실제 레귤레이터를 찾는다.
 * 보드가 일부만 가질 수 있으며, 없는 것은 코어가 더미로 채워 준다. */
static const char * const supplies[] = {
	"vpcie3v3", /* [한국어] 3.3V 주전원 */
	"vpcie3v3aux", /* [한국어] 3.3V 보조 전원 */
	"vpcie12v", /* [한국어] 12V 전원 */
};

/* [한국어]
 * alloc_subdev_regulators - 하류 장치용 레귤레이터 묶음을 잡는다
 *
 * @dev: 하류 버스의 device. devm 수명이 여기에 묶인다.
 * @return: 채워진 subdev_regulators. 할당 실패면 NULL.
 *
 * 구조체 뒤에 regulator_bulk_data 배열이 이어 붙는 가변 길이 배치라,
 * 크기를 직접 계산해 한 번에 잡는다.
 *
 * 잡은 뒤 supplies[] 의 이름 문자열을 각 항목에 꽂아 둔다.
 * regulator_bulk_get() 이 그 이름으로 DT 에서 실제 레귤레이터를 찾는다.
 *
 * 이름 셋이 PCIe 슬롯의 전원 규격 그대로다 — 3.3V 주전원, 3.3V 보조,
 * 12V. 보드가 그 중 일부만 가질 수 있고, 없는 것은 bulk_get 이 더미로
 * 채워 준다.
 *
 * devm 이라 dev 가 사라질 때 함께 풀린다. 그래서 실패 경로에서 따로
 * free 하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(버스 스캔 중).
 *
 * 호출 체인:  brcm_pcie_add_bus() → [이 함수] → devm_kzalloc()
 */
static void *alloc_subdev_regulators(struct device *dev)
{
	const size_t size = sizeof(struct subdev_regulators) +
		sizeof(struct regulator_bulk_data) * ARRAY_SIZE(supplies); /* [한국어] 구조체 뒤에 배열이 이어 붙는 가변 길이 배치라 크기를 직접 계산한다 */
	struct subdev_regulators *sr; /* [한국어] 잡을 묶음 */
	int i; /* [한국어] 이름을 꽂을 때 쓰는 루프 인덱스 */

	sr = devm_kzalloc(dev, size, GFP_KERNEL); /* [한국어] 하류 버스 device 수명에 묶어 한 번에 잡는다 */
	if (sr) { /* [한국어] 잡았으면 이름을 채운다. 실패하면 NULL 을 그대로 돌려준다 */
		sr->num_supplies = ARRAY_SIZE(supplies); /* [한국어] 항목 수를 기록해 둔다 — bulk 계열 API 가 이 값을 개수 인자로 받는다 */
		for (i = 0; i < ARRAY_SIZE(supplies); i++) /* [한국어] 이름을 하나씩 */
			sr->supplies[i].supply = supplies[i]; /* [한국어] 각 항목에 꽂는다. bulk_get 이 이 이름으로 DT 를 찾는다 */
	}

	return sr; /* [한국어] 채운 묶음 또는 NULL */
}

/* [한국어]
 * brcm_pcie_add_bus - 하류 버스가 생길 때 그 장치의 전원을 켜고 링크를 세운다
 *
 * @bus: 막 만들어진 버스.
 * @return: 늘 0. 실패해도 스캔을 막지 않는다.
 *
 * pci_ops 의 add_bus 콜백으로, PCI 코어가 버스를 새로 만들 때마다 부른다.
 *
 * 첫 조건이 이 함수의 범위를 정한다 — 부모가 있고 그 부모가 루트 버스인
 * 경우에만 일한다. 즉 RC 바로 아래 첫 버스에서만이다. 더 아래 버스는
 * 그 위 브리지가 이미 전원을 받고 있으므로 여기서 할 일이 없다.
 *
 * DT 노드가 있으면 그 노드에 적힌 레귤레이터를 잡아 켠다. 슬롯 전원을
 * 보드가 소프트웨어로 제어하는 구성을 위한 것이다. 실패는 모두 치명적이지
 * 않게 다룬다 — 할당 실패와 get 실패는 dev_info 로만 알리고 no_regulators
 * 로 건너뛰며, enable 실패는 dev_err 를 찍되 이미 잡은 것을 free 하고
 * pcie->sr 을 NULL 로 되돌린다. 레귤레이터가 없는 보드가 흔하기 때문이다.
 *
 * pcie->sr 을 NULL 로 되돌리는 것이 중요하다. remove_bus 와 suspend 가
 * 그 값으로 "레귤레이터를 쓰는 구성인가" 를 판단하므로, 반쯤 성공한 상태를
 * 남기면 안 된다.
 *
 * 마지막에 링크를 세운다. 전원을 켠 뒤여야 하류 장치가 응답하기 때문에
 * 순서가 이렇다. 실패해도 0 을 돌려주는데, 슬롯이 비어 있을 수 있고 그때
 * 스캔 자체를 막을 이유는 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(버스 스캔). 레귤레이터와 msleep 이
 * 잠들 수 있다.
 *
 * 호출 체인:  PCI 코어(버스 스캔) → [이 함수]
 *               → alloc_subdev_regulators() → brcm_pcie_start_link()
 */
static int brcm_pcie_add_bus(struct pci_bus *bus)
{
	struct brcm_pcie *pcie = bus->sysdata; /* [한국어] PCI 코어가 버스에 심어 둔 컨트롤러 상태 */
	struct device *dev = &bus->dev; /* [한국어] 이 버스의 device. DT 노드와 devm 수명의 기준이다 */
	struct subdev_regulators *sr; /* [한국어] 잡을 레귤레이터 묶음 */
	int ret; /* [한국어] 하위 호출 결과 */

	if (!bus->parent || !pci_is_root_bus(bus->parent)) /* [한국어] 부모가 없거나 부모가 루트 버스가 아니면 — RC 바로 아래 첫 버스가 아니다 */
		return 0; /* [한국어] 더 아래 버스는 위 브리지가 이미 전원을 받고 있어 할 일이 없다 */

	if (dev->of_node) { /* [한국어] 이 버스에 대응하는 DT 노드가 있어야 레귤레이터를 찾을 수 있다 */
		sr = alloc_subdev_regulators(dev); /* [한국어] 이름을 채운 묶음을 잡는다 */
		if (!sr) { /* [한국어] 메모리가 없으면 */
			dev_info(dev, "Can't allocate regulators for downstream device\n"); /* [한국어] 치명적이지 않으므로 정보 로그만 남긴다 */
			goto no_regulators; /* [한국어] 전원 없이 링크만 세워 본다 */
		}

		pcie->sr = sr; /* [한국어] remove_bus 와 PM 경로가 볼 수 있게 걸어 둔다 */

		ret = regulator_bulk_get(dev, sr->num_supplies, sr->supplies); /* [한국어] 꽂아 둔 이름으로 실제 레귤레이터를 찾는다 */
		if (ret) { /* [한국어] 못 찾았으면 */
			dev_info(dev, "Did not get regulators, err=%d\n", ret); /* [한국어] 레귤레이터가 없는 보드가 흔하므로 정보 로그만 남긴다 */
			pcie->sr = NULL; /* [한국어] 반쯤 성공한 상태를 남기지 않도록 되돌린다 */
			goto no_regulators; /* [한국어] 전원 없이 링크만 세워 본다 */
		}

		ret = regulator_bulk_enable(sr->num_supplies, sr->supplies); /* [한국어] 찾은 전원을 모두 켠다 */
		if (ret) { /* [한국어] 켜지 못했으면 */
			dev_err(dev, "Can't enable regulators for downstream device\n"); /* [한국어] 이쪽은 실제 오류이므로 dev_err 로 알린다 */
			regulator_bulk_free(sr->num_supplies, sr->supplies); /* [한국어] 잡은 것을 되돌려 놓는다 */
			pcie->sr = NULL; /* [한국어] PM 경로가 없는 것으로 보게 한다 */
		}
	}

no_regulators: /* [한국어] 전원을 못 잡았을 때도 여기로 모인다 */
	brcm_pcie_start_link(pcie); /* [한국어] 전원을 켠 뒤여야 하류 장치가 응답하므로 이 순서다 */
	return 0; /* [한국어] 링크가 서지 않아도 스캔을 막지 않는다 — 슬롯이 비어 있을 수 있다 */
}

/* [한국어]
 * brcm_pcie_remove_bus - 하류 버스가 사라질 때 레귤레이터를 끈다
 *
 * @bus: 없어지는 버스.
 *
 * add_bus 의 짝이다. 조건도 같아서, RC 바로 아래 첫 버스이면서 실제로
 * 레귤레이터를 잡아 둔 경우에만 일한다.
 *
 * disable 실패는 오류만 찍고 넘어간다. 이미 제거 경로라 되돌릴 방법이
 * 없고, free 는 어차피 해야 하기 때문이다.
 *
 * pcie->sr 을 NULL 로 되돌려 두 번 풀리는 일을 막는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  PCI 코어(버스 제거) → [이 함수] → regulator_bulk_free()
 */
static void brcm_pcie_remove_bus(struct pci_bus *bus)
{
	struct brcm_pcie *pcie = bus->sysdata; /* [한국어] PCI 코어가 버스에 심어 둔 컨트롤러 상태 */
	struct subdev_regulators *sr = pcie->sr; /* [한국어] add_bus 가 걸어 둔 레귤레이터 묶음 */
	struct device *dev = &bus->dev; /* [한국어] 로그용 device */

	if (!sr || !bus->parent || !pci_is_root_bus(bus->parent)) /* [한국어] 잡아 둔 것이 없거나 RC 바로 아래 첫 버스가 아니면 */
		return; /* [한국어] 끌 것이 없다 */

	if (regulator_bulk_disable(sr->num_supplies, sr->supplies)) /* [한국어] 전원을 끈다 */
		dev_err(dev, "Failed to disable regulators for downstream device\n"); /* [한국어] 실패해도 되돌릴 방법이 없어 알리기만 한다 */
	regulator_bulk_free(sr->num_supplies, sr->supplies); /* [한국어] 핸들을 놓는다. disable 이 실패했더라도 이것은 해야 한다 */
	pcie->sr = NULL; /* [한국어] 두 번 풀리는 일을 막는다 */
}

/* L23 is a low-power PCIe link state */
/* [한국어]
 * brcm_pcie_enter_l23 - 링크를 저전력 L23 상태로 넣는다
 *
 * @pcie: 컨트롤러 상태.
 *
 * 상류 주석이 L23 을 "저전력 PCIe 링크 상태" 로 소개한다. 서스펜드 전에
 * 링크를 정상적으로 재워야 하류 장치가 전원 상태를 알고 준비할 수 있다.
 *
 * 요청 비트를 켜고 상태 비트가 설 때까지 기다린다. 상류 주석대로 최대
 * 36ms 인데, 2~2.4ms 씩 15번 자는 계산이다.
 *
 * 첫 확인을 자기 전에 한 번 하는 것이 요령이다 — 이미 들어가 있으면
 * 한 번도 자지 않는다.
 *
 * 시간 안에 못 들어가도 오류만 찍고 돌아간다. 호출자인 turn_off 가
 * 어차피 PERST# 를 걸어 링크를 끊을 것이라, 여기서 멈출 이유가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(서스펜드/제거). usleep_range 로 잠든다.
 *
 * 호출 체인:  brcm_pcie_turn_off() → [이 함수]
 */
static void brcm_pcie_enter_l23(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base; /* [한국어] 레지스터 기준 주소 */
	int l23, i; /* [한국어] L23 진입 여부와 대기 루프 인덱스 */
	u32 tmp; /* [한국어] 읽고-고쳐-쓸 임시 값 */

	/* Assert request for L23 */
	tmp = readl(base + PCIE_MISC_PCIE_CTRL); /* [한국어] 제어 레지스터를 읽는다 */
	u32p_replace_bits(&tmp, 1, PCIE_MISC_PCIE_CTRL_PCIE_L23_REQUEST_MASK); /* [한국어] L23 요청 비트를 켠다 */
	writel(tmp, base + PCIE_MISC_PCIE_CTRL); /* [한국어] 되쓴다 — 이때부터 하드웨어가 링크를 재우기 시작한다 */

	/* Wait up to 36 msec for L23 */
	tmp = readl(base + PCIE_MISC_PCIE_STATUS); /* [한국어] 자기 전에 먼저 확인한다. 이미 들어가 있으면 한 번도 자지 않는다 */
	l23 = FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_LINK_IN_L23_MASK, tmp); /* [한국어] L23 진입 비트를 뽑는다 */
	for (i = 0; i < 15 && !l23; i++) { /* [한국어] 2~2.4ms 씩 최대 15번, 상류 주석대로 약 36ms 까지 기다린다 */
		usleep_range(2000, 2400); /* [한국어] 한 번 잔다 */
		tmp = readl(base + PCIE_MISC_PCIE_STATUS); /* [한국어] 상태를 다시 읽는다 */
		l23 = FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_LINK_IN_L23_MASK,
				tmp); /* [한국어] 진입 비트를 다시 확인한다 */
	}

	if (!l23) /* [한국어] 끝내 들어가지 못했으면 */
		dev_err(pcie->dev, "failed to enter low-power link state\n"); /* [한국어] 알리기만 한다 — 호출자가 어차피 PERST# 를 걸어 링크를 끊는다 */
}

/* [한국어]
 * brcm_phy_cntl - PHY 를 켜거나 끈다(순서가 정해진 세 필드 조작)
 *
 * @pcie:  컨트롤러 상태.
 * @start: 1 이면 시작, 0 이면 정지.
 * @return: 0 성공, -EIO 는 마지막 확인에서 값이 기대와 다른 경우.
 *
 * 세 필드(PWRDN, RESET, DIG_RESET)를 하나씩 순서대로 뒤집는다. 이 함수의
 * 핵심은 그 순서다.
 *
 * 시작할 때는 0 → 1 → 2 순으로, 멈출 때는 2 → 1 → 0 순으로 간다.
 * beg/end/증감이 start 로 갈리는 것이 그 뜻이다. 하드웨어 시퀀스가
 * 역순으로 풀려야 하는 종류이기 때문으로 보이며, 구체적인 근거 문서는
 * 이 트리에서 확인 못 함.
 *
 * 한 필드를 바꿀 때마다 50~200us 를 기다린다. 각 단계가 반영될 시간을
 * 주는 것이다.
 *
 * 값 계산이 눈에 띈다. start 면 BIT_MASK(shifts[i]) 를, 아니면 0 을
 * 쓴다. 마스크로 한 번 더 걸러 넣으므로 필드 밖 비트는 건드리지 않는다.
 *
 * 마지막에 세 비트를 한꺼번에 읽어 기대값과 대조한다. 시작이면 셋 다
 * 켜져 있어야 하고, 정지면 셋 다 꺼져 있어야 한다. 어긋나면 -EIO 다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 호출 체인:  brcm_phy_start() / brcm_phy_stop() → [이 함수]
 */
static int brcm_phy_cntl(struct brcm_pcie *pcie, const int start)
{
	static const u32 shifts[PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_NFLDS] = { /* [한국어] 세 필드의 비트 자리를 순서대로 담은 표 */
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_PWRDN_SHIFT, /* [한국어] 0번 - 파워다운 */
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_RESET_SHIFT, /* [한국어] 1번 - 리셋 */
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_DIG_RESET_SHIFT,}; /* [한국어] 2번 - 디지털 리셋. 이 배열 순서가 곧 시작 시 다루는 순서다 */
	static const u32 masks[PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_NFLDS] = { /* [한국어] 같은 세 필드의 마스크. 위 배열과 인덱스가 짝을 이룬다 */
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_PWRDN_MASK, /* [한국어] 파워다운 마스크 */
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_RESET_MASK, /* [한국어] 리셋 마스크 */
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_DIG_RESET_MASK,}; /* [한국어] 디지털 리셋 마스크 */
	const int beg = start ? 0 : PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_NFLDS - 1; /* [한국어] 시작이면 0번부터, 정지면 마지막 번호부터 — 정지는 역순이어야 한다 */
	const int end = start ? PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_NFLDS : -1; /* [한국어] 루프의 끝 값. 시작이면 개수, 정지면 -1 이라 0번까지 돈다 */
	u32 tmp, combined_mask = 0; /* [한국어] 읽고-고쳐-쓸 임시 값과, 마지막 확인에 쓸 세 마스크의 합 */
	u32 val; /* [한국어] 이번에 넣을 필드 값 */
	void __iomem *base = pcie->base; /* [한국어] 레지스터 기준 주소 */
	int i, ret; /* [한국어] 루프 인덱스와 반환값 */

	for (i = beg; i != end; start ? i++ : i--) { /* [한국어] 시작이면 오름차순, 정지면 내림차순으로 한 필드씩 다룬다 */
		val = start ? BIT_MASK(shifts[i]) : 0; /* [한국어] 시작이면 그 비트를 세우고, 정지면 0 으로 내린다 */
		tmp = readl(base + PCIE_DVT_PMU_PCIE_PHY_CTRL); /* [한국어] 다른 필드를 건드리지 않으려면 먼저 읽어야 한다 */
		tmp = (tmp & ~masks[i]) | (val & masks[i]); /* [한국어] 이번 필드만 갈아 끼운다 */
		writel(tmp, base + PCIE_DVT_PMU_PCIE_PHY_CTRL); /* [한국어] 되쓴다 */
		usleep_range(50, 200); /* [한국어] 이 단계가 하드웨어에 반영될 시간을 준다 */
		combined_mask |= masks[i]; /* [한국어] 마지막 확인에 쓸 마스크를 모아 둔다 */
	}

	tmp = readl(base + PCIE_DVT_PMU_PCIE_PHY_CTRL); /* [한국어] 세 필드를 한꺼번에 다시 읽는다 */
	val = start ? combined_mask : 0; /* [한국어] 시작이면 셋 다 서 있어야 하고, 정지면 셋 다 내려가 있어야 한다 */

	ret = (tmp & combined_mask) == val ? 0 : -EIO; /* [한국어] 기대와 다르면 하드웨어가 지시를 따르지 않은 것이다 */
	if (ret) /* [한국어] 어긋났으면 */
		dev_err(pcie->dev, "failed to %s phy\n", (start ? "start" : "stop")); /* [한국어] 어느 방향이 실패했는지 알린다 */

	return ret; /* [한국어] 호출자(probe/resume/suspend)가 이 값으로 다음 단계를 정한다 */
}

/* [한국어]
 * brcm_phy_start - PHY 가 있는 칩에서만 PHY 를 켠다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. PHY 가 없는 칩이면 아무 일도 하지 않고 0.
 *
 * cfg->has_phy 로 갈린다. 그 필드가 거짓인 칩은 이 레지스터 블록 자체가
 * 없어 접근하면 안 된다.
 *
 * 이 얇은 래퍼 덕분에 호출자(resume 경로)가 칩 종류를 몰라도 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  brcm_pcie_resume_noirq() / brcm_pcie_probe() → [이 함수]
 *               → brcm_phy_cntl(pcie, 1)
 */
static inline int brcm_phy_start(struct brcm_pcie *pcie)
{
	return pcie->cfg->has_phy ? brcm_phy_cntl(pcie, 1) : 0; /* [한국어] PHY 가 없는 칩에서는 이 레지스터 블록 자체가 없어 접근하면 안 되므로 그냥 0 */
}

/* [한국어]
 * brcm_phy_stop - PHY 가 있는 칩에서만 PHY 를 끈다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. PHY 가 없는 칩이면 그냥 0.
 *
 * brcm_phy_start() 의 짝이며 인자만 0 이다. 그러면 brcm_phy_cntl() 이
 * 세 필드를 역순으로 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  brcm_pcie_suspend_noirq() / __brcm_pcie_remove() → [이 함수]
 *               → brcm_phy_cntl(pcie, 0)
 */
static inline int brcm_phy_stop(struct brcm_pcie *pcie)
{
	return pcie->cfg->has_phy ? brcm_phy_cntl(pcie, 0) : 0; /* [한국어] 같은 이유로 PHY 가 없으면 아무 일도 하지 않는다. 인자만 0 이라 세 필드가 역순으로 되돌려진다 */
}

/* [한국어]
 * brcm_pcie_turn_off - 링크를 재우고 컨트롤러를 끈다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. PERST# 조작이나 브리지 리셋이 실패하면 그 오류.
 *
 * 서스펜드와 제거가 공유하는 정지 절차다. 순서가 요구사항이다.
 *
 *   1) 링크가 살아 있으면 먼저 L23 으로 넣는다. 링크가 이미 죽었으면
 *      그 단계를 건너뛴다 — 들어갈 링크가 없기 때문이다.
 *   2) PERST# 를 걸어 하류 장치를 리셋에 넣는다.
 *   3) L23 요청 비트를 되돌린다. 상류 주석이 "걸어 두었다면" 이라고
 *      조건을 달아 두는데, 1) 을 건너뛴 경우에도 무조건 지우는 편이
 *      상태를 확실히 하기 때문이다.
 *   4) SerDes 를 IDDQ 로 넣어 PHY 전류를 끊는다. setup 4단계에서 푼 것을
 *      되돌리는 것이다.
 *   5) CFG_QUIRK_AVOID_BRIDGE_SHUTDOWN 이 없는 칩에서만 브리지 리셋을
 *      건다. 그 quirk 가 붙은 칩(BCM2712)은 브리지를 끄면 문제가 생겨
 *      이 마지막 단계를 건너뛴다.
 *
 * ret 의 흐름에 주의. 5) 를 건너뛰면 ret 는 2) 가 남긴 0 이라 성공으로
 * 돌아간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(서스펜드/제거).
 *
 * 호출 체인:  brcm_pcie_suspend_noirq() / __brcm_pcie_remove() → [이 함수]
 *               → brcm_pcie_enter_l23() → cfg->perst_set()
 *               → brcm_pcie_bridge_sw_init_set()
 */
static int brcm_pcie_turn_off(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base; /* [한국어] 레지스터 기준 주소 */
	int tmp, ret; /* [한국어] 읽고-고쳐-쓸 임시 값과 하위 호출 결과 */

	if (brcm_pcie_link_up(pcie)) /* [한국어] 링크가 살아 있을 때만 */
		brcm_pcie_enter_l23(pcie); /* [한국어] 정상적으로 저전력 상태로 넣는다. 이미 죽었으면 들어갈 링크가 없다 */
	/* Assert fundamental reset */
	ret = pcie->cfg->perst_set(pcie, 1); /* [한국어] PERST# 를 걸어 하류 장치를 리셋에 넣는다 */
	if (ret) /* [한국어] 실패했으면 */
		return ret; /* [한국어] 아래 단계를 건너뛰고 오류를 올린다 */

	/* Deassert request for L23 in case it was asserted */
	tmp = readl(base + PCIE_MISC_PCIE_CTRL); /* [한국어] 제어 레지스터를 읽는다 */
	u32p_replace_bits(&tmp, 0, PCIE_MISC_PCIE_CTRL_PCIE_L23_REQUEST_MASK); /* [한국어] L23 요청 비트를 내린다. 위에서 건너뛴 경우에도 무조건 지워 상태를 확실히 한다 */
	writel(tmp, base + PCIE_MISC_PCIE_CTRL); /* [한국어] 되쓴다 */

	/* Turn off SerDes */
	tmp = readl(base + HARD_DEBUG(pcie)); /* [한국어] IDDQ 비트가 있는 레지스터를 읽는다 */
	u32p_replace_bits(&tmp, 1, PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK); /* [한국어] IDDQ 를 걸어 PHY 전류를 끊는다 — setup 이 푼 것을 되돌리는 것이다 */
	writel(tmp, base + HARD_DEBUG(pcie)); /* [한국어] 되쓴다 */

	if (!(pcie->cfg->quirks & CFG_QUIRK_AVOID_BRIDGE_SHUTDOWN)) /* [한국어] 브리지를 꺼도 되는 칩에서만 */
		/* Shutdown PCIe bridge */
		ret = brcm_pcie_bridge_sw_init_set(pcie, 1); /* [한국어] 마지막으로 브리지를 리셋에 넣는다. quirk 가 붙은 칩은 이 단계를 건너뛰고 ret 는 위에서 남긴 0 이 된다 */

	return ret; /* [한국어] 마지막 조작 결과를 올린다 */
}

/* [한국어]
 * pci_dev_may_wakeup - 이 장치가 웨이크업 소스인지 확인하는 순회 콜백
 *
 * @dev:  순회 중 만난 PCI 장치.
 * @data: bool 포인터. 하나라도 참이면 true 로 바뀐다.
 * @return: 0 이면 순회를 계속, 0 이 아니면 멈춘다.
 *
 * pci_walk_bus() 에 넘기는 콜백이다. 버스 아래 장치를 훑다가 웨이크업
 * 가능한 것을 하나 찾으면 그 사실을 *data 에 적고, 사용자에게도 알린다 —
 * 레귤레이터를 끄지 않을 것이라는 안내다.
 *
 * 반환값이 *ret 를 그대로 캐스트한 것이라, 참을 찾은 순간 순회가 멈춘다.
 * 하나만 찾으면 되므로 더 볼 이유가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(서스펜드). pci_walk_bus 가 버스 세마포어를
 * 잡은 상태로 부른다.
 *
 * 호출 체인:  brcm_pcie_suspend_noirq() → pci_walk_bus() → [이 함수]
 */
static int pci_dev_may_wakeup(struct pci_dev *dev, void *data)
{
	bool *ret = data; /* [한국어] 호출자가 넘긴 bool 포인터. 하나라도 찾으면 여기에 참을 적는다 */

	if (device_may_wakeup(&dev->dev)) { /* [한국어] 이 장치가 웨이크업 소스로 켜져 있으면 */
		*ret = true; /* [한국어] 호출자에게 그 사실을 알린다 */
		dev_info(&dev->dev, "Possible wake-up device; regulators will not be disabled\n"); /* [한국어] 전원을 끄지 않을 것임을 사용자에게 알린다 */
	}
	return (int) *ret; /* [한국어] 0 이 아니면 pci_walk_bus 가 순회를 멈춘다 — 하나만 찾으면 되므로 더 볼 이유가 없다 */
}

/* [한국어]
 * brcm_pcie_suspend_noirq - 서스펜드 직전 컨트롤러를 끈다
 *
 * @dev: 플랫폼 디바이스.
 * @return: 0 성공. 되돌릴 수 없는 실패는 그 오류를 올려 서스펜드를 막는다.
 *
 * noirq 단계라 인터럽트가 이미 꺼진 뒤에 불린다. 그래서 여기서는 MSI 나
 * 링크 이벤트가 들어올 걱정을 하지 않는다.
 *
 *   1) turn_off 로 링크를 재우고 PHY 전류를 끊고 브리지를 리셋에 넣는다.
 *      실패하면 서스펜드를 막는다 — 하드웨어를 어중간한 상태로 두고
 *      잠들면 복귀가 위험하기 때문이다.
 *   2) PHY 를 멈춘다. 상류 주석이 실패를 삼키는 이유를 밝힌다 — 오류를
 *      올리면 서스펜드가 실패하는데, 이 정도는 봐줄 만하고 resume 에서
 *      아마 지워질 문제이기 때문이다.
 *   3) rescal 리셋을 rearm 한다. 한 번 쓴 리셋을 다시 쓸 수 있게 되돌리는
 *      조작으로, resume 이 reset_control_reset() 을 부를 수 있게 한다.
 *      이것이 실패하면 resume 이 리셋을 못 걸어 복구가 안 되므로 막는다.
 *   4) 레귤레이터를 끈다. 다만 상류 주석대로, 하류 장치 중 하나라도
 *      웨이크업 소스로 켜져 있으면 끄지 않는다 — 전원이 없으면 그 장치가
 *      깨울 수 없기 때문이다. 판정은 pci_walk_bus 순회로 한다.
 *      ep_wakeup_capable 를 여기서 false 로 초기화한 뒤 순회로 채우고,
 *      그 값이 resume 까지 남아 "서스펜드 때 안 껐다" 는 표시가 된다.
 *      끄기가 실패하면 3) 에서 rearm 한 리셋을 다시 걸어 되돌린 다음
 *      오류를 올린다.
 *   5) 클록을 끈다. 마지막이어야 위 조작들이 모두 유효하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 인터럽트 비활성(noirq) 단계.
 *
 * 호출 체인:  PM 코어 → [이 함수] → brcm_pcie_turn_off() → brcm_phy_stop()
 *               → pci_walk_bus(pci_dev_may_wakeup)
 */
static int brcm_pcie_suspend_noirq(struct device *dev)
{
	struct brcm_pcie *pcie = dev_get_drvdata(dev); /* [한국어] PM 코어가 넘긴 device 에서 컨트롤러 상태를 되찾는다 */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie); /* [한국어] pci_walk_bus 에 넘길 버스를 얻기 위해 바깥 브리지를 되찾는다 */
	int ret, rret; /* [한국어] 주 결과와, 되돌리기 중 쓰는 보조 결과 */

	ret = brcm_pcie_turn_off(pcie); /* [한국어] 링크를 재우고 PERST# 를 걸고 PHY 전류를 끊고 브리지를 리셋에 넣는다 */
	if (ret) /* [한국어] 실패했으면 */
		return ret; /* [한국어] 어중간한 상태로 잠들면 복귀가 위험하므로 서스펜드를 막는다 */

	/*
	 * If brcm_phy_stop() returns an error, just dev_err(). If we
	 * return the error it will cause the suspend to fail and this is a
	 * forgivable offense that will probably be erased on resume.
	 */
	if (brcm_phy_stop(pcie)) /* [한국어] PHY 를 멈춘다. 상류 주석대로 실패해도 서스펜드를 막지 않는다 */
		dev_err(dev, "Could not stop phy for suspend\n"); /* [한국어] 알리기만 한다 */

	ret = reset_control_rearm(pcie->rescal); /* [한국어] 한 번 쓴 리셋을 다시 쓸 수 있게 되돌린다 — resume 이 reset 을 걸 수 있어야 한다 */
	if (ret) { /* [한국어] 되돌리지 못했으면 */
		dev_err(dev, "Could not rearm rescal reset\n"); /* [한국어] 무엇이 문제인지 알리고 */
		return ret; /* [한국어] 복귀가 불가능해지므로 서스펜드를 막는다 */
	}

	if (pcie->sr) { /* [한국어] 레귤레이터를 쓰는 구성일 때만 */
		/*
		 * Now turn off the regulators, but if at least one
		 * downstream device is enabled as a wake-up source, do not
		 * turn off regulators.
		 */
		pcie->ep_wakeup_capable = false; /* [한국어] 순회 전에 초기화한다 */
		pci_walk_bus(bridge->bus, pci_dev_may_wakeup,
			     &pcie->ep_wakeup_capable); /* [한국어] 버스 아래를 훑어 웨이크업 소스가 있는지 본다 */
		if (!pcie->ep_wakeup_capable) { /* [한국어] 하나도 없으면 */
			ret = regulator_bulk_disable(pcie->sr->num_supplies,
						     pcie->sr->supplies); /* [한국어] 전원을 꺼도 안전하다 */
			if (ret) { /* [한국어] 끄지 못했으면 */
				dev_err(dev, "Could not turn off regulators\n"); /* [한국어] 알리고 */
				rret = reset_control_reset(pcie->rescal); /* [한국어] 위에서 rearm 한 리셋을 다시 걸어 되돌린다 — 서스펜드를 접을 것이므로 상태를 되돌려야 한다 */
				if (rret) /* [한국어] 그 되돌리기마저 실패하면 */
					dev_err(dev, "failed to reset 'rascal' controller ret=%d\n",
						rret); /* [한국어] 알리기만 한다. 더 할 수 있는 일이 없다 */
				return ret; /* [한국어] 원래 오류를 올려 서스펜드를 막는다 */
			}
		}
	}
	clk_disable_unprepare(pcie->clk); /* [한국어] 마지막으로 클록을 끈다. 위 조작들이 모두 끝난 뒤여야 한다 */

	return 0; /* [한국어] 서스펜드 준비 완료 */
}

/* [한국어]
 * brcm_pcie_resume_noirq - 서스펜드에서 깨어나 컨트롤러를 되살린다
 *
 * @dev: 플랫폼 디바이스.
 * @return: 0 성공. 실패하면 되돌린 뒤 그 오류.
 *
 * suspend 의 역순이며, goto 라벨 셋으로 단계별 되돌리기를 갖춘다.
 *
 *   1) 클록을 켠다. 이것이 없으면 레지스터 접근 자체가 안 된다.
 *   2) rescal 리셋을 건다. suspend 에서 rearm 해 두어 여기서 다시 쓸 수 있다.
 *   3) PHY 를 켠다.
 *   4) 브리지 리셋을 푼다. 상류 주석대로 SERDES 레지스터에 닿기 위해서다.
 *   5) SERDES IDDQ 를 풀고 100us 기다린다. 상류 주석대로 안정될 시간이다.
 *      여기서는 udelay 를 쓴다 — noirq 단계라 잠들 수 없는 문맥일 수 있다.
 *   6) setup 을 통째로 다시 돌린다. 서스펜드로 레지스터가 다 날아갔기
 *      때문에, 안쪽/바깥쪽 창부터 클래스 코드까지 전부 다시 세운다.
 *   7) 레귤레이터를 켠다. 다만 상류 주석대로, suspend 에서 웨이크업 장치
 *      때문에 끄지 않았다면 켜지 않는다 — 켜면 사용 카운트가 잘못 올라간다.
 *      그 표시(ep_wakeup_capable)를 여기서 false 로 되돌려 다음 주기를
 *      깨끗하게 만든다.
 *   8) 링크를 다시 세운다.
 *   9) MSI 를 쓰는 구성이면 MSI 레지스터를 다시 적는다. 도메인과 비트맵은
 *      메모리에 남아 있어 다시 만들 필요가 없고, 하드웨어 쪽만 복원하면 된다.
 *
 * 되돌리기 순서가 라벨 배치로 드러난다. err_regulator 는 레귤레이터를 끄고
 * 아래로 흘러 err_reset 으로, 거기서 rescal 을 rearm 하고 err_disable_clk
 * 으로, 마지막에 클록을 끈다 — 잡은 역순 그대로다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, noirq 단계.
 *
 * 호출 체인:  PM 코어 → [이 함수] → brcm_pcie_setup() → brcm_pcie_start_link()
 *               → brcm_msi_set_regs()
 */
static int brcm_pcie_resume_noirq(struct device *dev)
{
	struct brcm_pcie *pcie = dev_get_drvdata(dev); /* [한국어] PM 코어가 넘긴 device 에서 컨트롤러 상태를 되찾는다 */
	void __iomem *base; /* [한국어] 레지스터 기준 주소 */
	u32 tmp; /* [한국어] 읽고-고쳐-쓸 임시 값 */
	int ret, rret; /* [한국어] 주 결과와, 되돌리기 중 쓰는 보조 결과 */

	base = pcie->base; /* [한국어] 레지스터 기준 주소를 꺼내 둔다 */
	ret = clk_prepare_enable(pcie->clk); /* [한국어] 클록부터 켠다. 이것이 없으면 레지스터 접근 자체가 안 된다 */
	if (ret) /* [한국어] 못 켰으면 */
		return ret; /* [한국어] 아직 잡은 것이 없어 되돌릴 것도 없다 */

	ret = reset_control_reset(pcie->rescal); /* [한국어] suspend 에서 rearm 해 둔 리셋을 다시 건다 */
	if (ret) /* [한국어] 실패했으면 */
		goto err_disable_clk; /* [한국어] 켠 클록만 되돌린다 */

	ret = brcm_phy_start(pcie); /* [한국어] PHY 를 켠다 */
	if (ret) /* [한국어] 실패했으면 */
		goto err_reset; /* [한국어] 리셋 rearm 과 클록 끄기까지 되돌린다 */

	/* Take bridge out of reset so we can access the SERDES reg */
	ret = brcm_pcie_bridge_sw_init_set(pcie, 0); /* [한국어] 상류 주석대로 SERDES 레지스터에 닿기 위해 브리지를 리셋에서 꺼낸다 */
	if (ret) /* [한국어] 실패했으면 */
		goto err_reset; /* [한국어] 같은 곳으로 되돌린다 */

	/* SERDES_IDDQ = 0 */
	tmp = readl(base + HARD_DEBUG(pcie)); /* [한국어] IDDQ 비트가 있는 레지스터를 읽는다 */
	u32p_replace_bits(&tmp, 0, PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK); /* [한국어] IDDQ 를 풀어 PHY 전류를 흘린다 */
	writel(tmp, base + HARD_DEBUG(pcie)); /* [한국어] 되쓴다 */

	/* wait for serdes to be stable */
	udelay(100); /* [한국어] SerDes 가 안정될 때까지 기다린다. noirq 단계라 잠들지 않는 udelay 를 쓴다 */

	ret = brcm_pcie_setup(pcie); /* [한국어] 서스펜드로 레지스터가 다 날아갔으므로 setup 을 통째로 다시 돌린다 */
	if (ret) /* [한국어] 실패했으면 */
		goto err_reset; /* [한국어] 되돌린다 */

	if (pcie->sr) { /* [한국어] 레귤레이터를 쓰는 구성일 때만 */
		if (pcie->ep_wakeup_capable) { /* [한국어] suspend 가 웨이크업 장치 때문에 전원을 끄지 않았다면 */
			/*
			 * We are resuming from a suspend.  In the suspend we
			 * did not disable the power supplies, so there is
			 * no need to enable them (and falsely increase their
			 * usage count).
			 */
			pcie->ep_wakeup_capable = false; /* [한국어] 켤 필요가 없다. 표시만 되돌려 다음 주기를 깨끗하게 만든다 */
		} else { /* [한국어] 꺼 두었다면 */
			ret = regulator_bulk_enable(pcie->sr->num_supplies,
						    pcie->sr->supplies); /* [한국어] 다시 켠다 */
			if (ret) { /* [한국어] 켜지 못했으면 */
				dev_err(dev, "Could not turn on regulators\n"); /* [한국어] 알리고 */
				goto err_reset; /* [한국어] 되돌린다 */
			}
		}
	}

	ret = brcm_pcie_start_link(pcie); /* [한국어] 링크를 다시 세운다 */
	if (ret) /* [한국어] 서지 않았으면 */
		goto err_regulator; /* [한국어] 레귤레이터부터 되돌린다 */

	if (pcie->msi) /* [한국어] MSI 를 쓰는 구성이면 */
		brcm_msi_set_regs(pcie->msi); /* [한국어] 도메인과 비트맵은 메모리에 남아 있으므로 하드웨어 쪽만 다시 적는다 */

	return 0; /* [한국어] 복귀 완료 */

err_regulator: /* [한국어] 링크 실패 경로 — 켠 전원을 되돌린다 */
	if (pcie->sr) /* [한국어] 전원을 쓰는 구성일 때만 */
		regulator_bulk_disable(pcie->sr->num_supplies, pcie->sr->supplies); /* [한국어] 끈다. 실패해도 아래 단계를 계속해야 하므로 결과를 보지 않는다 */
err_reset: /* [한국어] PHY/브리지/setup 실패 경로 — 리셋을 되돌린다 */
	rret = reset_control_rearm(pcie->rescal); /* [한국어] 다음 서스펜드가 이 리셋을 다시 쓸 수 있게 rearm 한다 */
	if (rret) /* [한국어] 그것마저 실패하면 */
		dev_err(pcie->dev, "failed to rearm 'rescal' reset, err=%d\n", rret); /* [한국어] 알리기만 한다 */
err_disable_clk: /* [한국어] 클록 실패 이후의 모든 경로가 여기로 모인다 */
	clk_disable_unprepare(pcie->clk); /* [한국어] 마지막으로 클록을 끈다 — 잡은 역순 그대로다 */
	return ret; /* [한국어] 원래 실패 원인을 올린다 */
}

/* Dump out PCIe errors on die or panic */
/* [한국어]
 * brcm_pcie_dump_err - die/panic 시점에 컨트롤러가 기록한 오류를 찍는다
 *
 * @pcie: 컨트롤러 상태.
 * @type: "Die" 또는 "Panic" — 어느 알림에서 왔는지 로그에 남긴다.
 * @return: 늘 NOTIFY_DONE. 알림 체인을 멈추지 않는다.
 *
 * 이 드라이버의 특징적인 기능이다. 시스템이 죽는 순간, 그 원인이 PCIe
 * 접근 실패였는지 알려 줄 정보가 컨트롤러 안에 남아 있다. 그것을 마지막에
 * 콘솔로 꺼내 준다.
 *
 * 첫 관문이 중요하다. 브리지가 리셋 상태이면 레지스터를 읽으면 안 된다 —
 * 읽는 순간 또 다른 abort 가 나 죽는 경로가 더 나빠진다. 그래서
 * bridge_in_reset 플래그를 먼저 본다. 그 플래그는
 * brcm_pcie_bridge_sw_init_set() 이 같은 스핀락 아래에서 갱신하므로,
 * 여기서 보는 값과 실제 하드웨어 상태가 어긋나지 않는다. 유효 비트가
 * 0 이면 기록된 오류가 없다는 뜻이라 역시 그냥 돌아간다.
 *
 * 상류 주석이 밝히듯 필요한 레지스터를 먼저 다 읽고 락을 빨리 놓는다.
 * 패닉/die 문맥에서 락을 오래 쥐면 다른 CPU 가 막히기 때문이다. 그래서
 * 읽기와 출력이 분리되어 있고, 출력은 락 밖에서 한다.
 *
 * 오류 비트를 지우는 것도 락 안에서 한다. 다음 번 알림이 같은 오류를
 * 다시 찍지 않게 하기 위해서다.
 *
 * 정보는 두 갈래로 나뉜다.
 *   CFG 오류 — 어느 BDF 의 어느 레지스터에 접근하다 났는지까지 알려 준다.
 *              원인 비트가 여섯 가지다(타임아웃, abort, Unsupported
 *              Request, 접근 타임아웃, 접근 금지, 64비트 접근).
 *   MEM 오류 — 어느 주소에 접근하다 났는지 알려 준다. 하위/상위를 합쳐
 *              64비트 주소를 만든다. 원인 비트가 다섯 가지다.
 *
 * 공통 정보로 접근 폭(32/64비트), 방향(읽기/쓰기), 바이트 레인 마스크를
 * 찍는다. 레인은 문자열로 풀어 여덟 자리 0/1 로 보여 준다 — lanes_str[8] 에
 * 0 을 미리 넣어 종결하고 나머지 여덟 칸을 채운다.
 *
 * 실행 컨텍스트: die 또는 panic 알림 문맥. 인터럽트가 꺼져 있을 수 있고
 * 다른 CPU 가 멈춰 있을 수 있다. 잠들면 안 된다 — spin_lock_irqsave 와
 * readl/dev_err 만 쓰는 이유다.
 *
 * 호출 체인:  die/panic 알림 체인 → brcm_pcie_die_notify_cb() /
 *               brcm_pcie_panic_notify_cb() → [이 함수]
 */
static int brcm_pcie_dump_err(struct brcm_pcie *pcie,
			       const char *type)
{
	void __iomem *base = pcie->base; /* [한국어] 레지스터 기준 주소 */
	int i, is_cfg_err, is_mem_err, lanes; /* [한국어] 문자열 조립 인덱스, 두 오류 종류의 판정 결과, 바이트 레인 마스크 */
	const char *width_str, *direction_str; /* [한국어] 접근 폭과 방향을 나타낼 문자열 */
	u32 info, cfg_addr, cfg_cause, mem_cause, lo, hi; /* [한국어] 락 안에서 읽어 둘 레지스터 값들. 락 밖에서 이 값들만으로 출력한다 */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie); /* [한국어] 로그에 도메인 번호를 찍기 위해 바깥 브리지를 되찾는다 */
	unsigned long flags; /* [한국어] 인터럽트 상태 저장 자리 */
	char lanes_str[9]; /* [한국어] 여덟 자리 0/1 문자열과 종결 문자를 담을 버퍼 */

	spin_lock_irqsave(&pcie->bridge_lock, flags); /* [한국어] bridge_in_reset 과 오류 레지스터를 함께 지키는 락을 잡는다 */
	/* Don't access registers when the bridge is off */
	if (pcie->bridge_in_reset || readl(base + PCIE_OUTB_ERR_VALID) == 0) { /* [한국어] 브리지가 리셋 중이면 레지스터를 읽는 것만으로도 또 다른 abort 가 나고, 유효 비트가 0 이면 기록된 오류가 없다 */
		spin_unlock_irqrestore(&pcie->bridge_lock, flags); /* [한국어] 어느 쪽이든 읽지 않고 */
		return NOTIFY_DONE; /* [한국어] 알림 체인을 막지 않고 물러난다 */
	}

	/* Read all necessary registers so we can release the spinlock ASAP */
	info = readl(base + PCIE_OUTB_ERR_ACC_INFO); /* [한국어] 접근의 성격(종류/폭/방향/레인)을 담은 워드 */
	is_cfg_err = !!(info & PCIE_OUTB_ERR_ACC_INFO_CFG_ERR); /* [한국어] config 접근에서 난 오류인지 */
	is_mem_err = !!(info & PCIE_OUTB_ERR_ACC_INFO_MEM_ERR); /* [한국어] 메모리 접근에서 난 오류인지. 둘이 동시에 설 수도 있다 */
	if (is_cfg_err) { /* [한국어] config 오류이면 */
		cfg_addr = readl(base + PCIE_OUTB_ERR_ACC_ADDR); /* [한국어] 대상 BDF 와 레지스터 번호 */
		cfg_cause = readl(base + PCIE_OUTB_ERR_CFG_CAUSE); /* [한국어] 원인 비트 묶음 */
	}
	if (is_mem_err) { /* [한국어] 메모리 오류이면 */
		mem_cause = readl(base + PCIE_OUTB_ERR_MEM_CAUSE); /* [한국어] 원인 비트 묶음 */
		lo = readl(base + PCIE_OUTB_ERR_MEM_ADDR_LO); /* [한국어] 주소 하위 워드 */
		hi = readl(base + PCIE_OUTB_ERR_MEM_ADDR_HI); /* [한국어] 주소 상위 워드 */
	}
	/* We've got all of the info, clear the error */
	writel(1, base + PCIE_OUTB_ERR_CLEAR); /* [한국어] 다 읽었으므로 기록을 지운다 — 다음 알림이 같은 오류를 다시 찍지 않게 한다 */
	spin_unlock_irqrestore(&pcie->bridge_lock, flags); /* [한국어] 락을 놓는다. 패닉 문맥에서 오래 쥐면 다른 CPU 가 막힌다 */

	dev_err(pcie->dev, "reporting PCIe info which may be related to %s error\n",
		type); /* [한국어] 어느 알림에서 온 보고인지 먼저 밝힌다 */
	width_str = (info & PCIE_OUTB_ERR_ACC_INFO_TYPE_64) ? "64bit" : "32bit"; /* [한국어] 64비트 접근이었는지 */
	direction_str = str_read_write(!(info & PCIE_OUTB_ERR_ACC_INFO_DIR_WRITE)); /* [한국어] 쓰기 비트를 뒤집어 넘긴다 — str_read_write 가 참일 때 "read" 를 준다 */
	lanes = FIELD_GET(PCIE_OUTB_ERR_ACC_INFO_BYTE_LANES, info); /* [한국어] 유효했던 바이트 레인 마스크 */
	for (i = 0, lanes_str[8] = 0; i < 8; i++) /* [한국어] 끝에 종결 문자를 먼저 넣고 여덟 칸을 채운다 */
		lanes_str[i] = (lanes & (1 << i)) ? '1' : '0'; /* [한국어] 각 레인 비트를 0/1 문자로 바꾼다 */

	if (is_cfg_err) { /* [한국어] config 오류가 기록되어 있으면 */
		int bus = FIELD_GET(PCIE_OUTB_ERR_ACC_ADDR_BUS, cfg_addr); /* [한국어] 버스 번호 */
		int dev = FIELD_GET(PCIE_OUTB_ERR_ACC_ADDR_DEV, cfg_addr); /* [한국어] 장치 번호 */
		int func = FIELD_GET(PCIE_OUTB_ERR_ACC_ADDR_FUNC, cfg_addr); /* [한국어] 함수 번호 */
		int reg = FIELD_GET(PCIE_OUTB_ERR_ACC_ADDR_REG, cfg_addr); /* [한국어] config 레지스터 오프셋 */

		dev_err(pcie->dev, "Error: CFG Acc, %s, %s (%04x:%02x:%02x.%d) reg=0x%x, lanes=%s\n",
			width_str, direction_str, bridge->domain_nr, bus, dev,
			func, reg, lanes_str); /* [한국어] 어느 장치의 어느 레지스터에 접근하다 났는지 BDF 형식으로 찍는다 */
		dev_err(pcie->dev, " Type: TO=%d Abt=%d UnsupReq=%d AccTO=%d AccDsbld=%d Acc64bit=%d\n",
			!!(cfg_cause & PCIE_OUTB_ERR_CFG_CAUSE_TIMEOUT),
			!!(cfg_cause & PCIE_OUTB_ERR_CFG_CAUSE_ABORT),
			!!(cfg_cause & PCIE_OUTB_ERR_CFG_CAUSE_UNSUPP_REQ),
			!!(cfg_cause & PCIE_OUTB_ERR_CFG_CAUSE_ACC_TIMEOUT),
			!!(cfg_cause & PCIE_OUTB_ERR_CFG_CAUSE_ACC_DISABLED),
			!!(cfg_cause & PCIE_OUTB_ERR_CFG_CAUSE_ACC_64BIT)); /* [한국어] 원인 비트 여섯 가지를 각각 0/1 로 풀어 찍는다 */
	}

	if (is_mem_err) { /* [한국어] 메모리 오류가 기록되어 있으면 */
		u64 addr = ((u64)hi << 32) | (u64)lo; /* [한국어] 하위/상위 워드를 합쳐 64비트 주소를 만든다 */

		dev_err(pcie->dev, "Error: Mem Acc, %s, %s, @0x%llx, lanes=%s\n",
			width_str, direction_str, addr, lanes_str); /* [한국어] 어느 주소에 접근하다 났는지 찍는다 */
		dev_err(pcie->dev, " Type: TO=%d Abt=%d UnsupReq=%d AccDsble=%d BadAddr=%d\n",
			!!(mem_cause & PCIE_OUTB_ERR_MEM_CAUSE_TIMEOUT),
			!!(mem_cause & PCIE_OUTB_ERR_MEM_CAUSE_ABORT),
			!!(mem_cause & PCIE_OUTB_ERR_MEM_CAUSE_UNSUPP_REQ),
			!!(mem_cause & PCIE_OUTB_ERR_MEM_CAUSE_ACC_DISABLED),
			!!(mem_cause & PCIE_OUTB_ERR_MEM_CAUSE_BAD_ADDR)); /* [한국어] 원인 비트 다섯 가지를 각각 0/1 로 풀어 찍는다 */
	}

	return NOTIFY_DONE; /* [한국어] 알림 체인을 멈추지 않는다 — 다른 등록자도 자기 일을 해야 한다 */
}

/* [한국어]
 * brcm_pcie_die_notify_cb - die 알림 체인 콜백
 *
 * @self: 등록해 둔 notifier_block. 이 구조체가 brcm_pcie 안에 박혀 있다.
 * @v:    die 이유 코드. 쓰지 않는다.
 * @p:    die 인자. 쓰지 않는다.
 * @return: brcm_pcie_dump_err() 의 반환값, 즉 NOTIFY_DONE.
 *
 * container_of 로 notifier_block 에서 바깥 brcm_pcie 를 되찾는다. 알림
 * 체인은 컨텍스트 포인터를 따로 주지 않으므로 이 방식이 유일하다.
 * 그래서 die_notifier 필드가 brcm_pcie 안에 값으로 들어 있다.
 *
 * panic 판과 갈리는 것은 로그에 남길 문자열뿐이다.
 *
 * 실행 컨텍스트: die 알림 문맥. 잠들면 안 된다.
 *
 * 호출 체인:  register_die_notifier 체인 → [이 함수] → brcm_pcie_dump_err()
 */
static int brcm_pcie_die_notify_cb(struct notifier_block *self,
				   unsigned long v, void *p)
{
	struct brcm_pcie *pcie =
		container_of(self, struct brcm_pcie, die_notifier); /* [한국어] 알림 체인은 문맥 포인터를 주지 않으므로, 구조체에 박힌 필드 주소에서 바깥을 되찾는다 */

	return brcm_pcie_dump_err(pcie, "Die"); /* [한국어] die 로 죽는 중임을 문자열로 알리며 덤프한다 */
}

/* [한국어]
 * brcm_pcie_panic_notify_cb - panic 알림 체인 콜백
 *
 * @self: 등록해 둔 notifier_block(brcm_pcie 안의 panic_notifier).
 * @v:    쓰지 않는다.
 * @p:    패닉 메시지 포인터. 쓰지 않는다.
 * @return: NOTIFY_DONE.
 *
 * die 판과 같은 구조이며 문자열만 "Panic" 이다. 두 경로를 모두 거는 이유는
 * 죽는 방식이 둘 다 있어서다 — oops/die 로 죽을 수도, panic 으로 바로
 * 죽을 수도 있다.
 *
 * 실행 컨텍스트: panic 문맥. 다른 CPU 가 이미 멈춰 있을 수 있다.
 *
 * 호출 체인:  panic_notifier_list → [이 함수] → brcm_pcie_dump_err()
 */
static int brcm_pcie_panic_notify_cb(struct notifier_block *self,
				     unsigned long v, void *p)
{
	struct brcm_pcie *pcie =
		container_of(self, struct brcm_pcie, panic_notifier); /* [한국어] panic 쪽 필드에서 같은 방식으로 바깥을 되찾는다 */

	return brcm_pcie_dump_err(pcie, "Panic"); /* [한국어] panic 으로 죽는 중임을 알리며 덤프한다 */
}

/* [한국어]
 * brcm_register_die_notifiers - 오류 보고 콜백 둘을 알림 체인에 건다
 *
 * @pcie: 컨트롤러 상태.
 *
 * panic 체인은 atomic notifier 라 atomic_notifier_chain_register 로,
 * die 체인은 전용 API 인 register_die_notifier 로 건다. 종류가 달라
 * 등록 함수도 다르다.
 *
 * has_err_report 인 칩에서만 불린다. 다른 칩에는 이 오류 기록 레지스터가
 * 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  brcm_pcie_probe() → [이 함수]
 */
static void brcm_register_die_notifiers(struct brcm_pcie *pcie)
{
	pcie->panic_notifier.notifier_call = brcm_pcie_panic_notify_cb; /* [한국어] panic 체인이 부를 함수를 꽂는다 */
	atomic_notifier_chain_register(&panic_notifier_list,
				       &pcie->panic_notifier); /* [한국어] panic 체인은 atomic notifier 라 전용 등록 함수를 쓴다 */

	pcie->die_notifier.notifier_call = brcm_pcie_die_notify_cb; /* [한국어] die 체인이 부를 함수를 꽂는다 */
	register_die_notifier(&pcie->die_notifier); /* [한국어] die 체인은 별도의 전용 API 로 등록한다 */
}

/* [한국어]
 * brcm_unregister_die_notifiers - 걸어 둔 오류 보고 콜백을 뗀다
 *
 * @pcie: 컨트롤러 상태.
 *
 * 등록의 역순이다. 드라이버가 사라진 뒤에 체인이 이 콜백을 부르면
 * 해제된 메모리를 건드리게 되므로, 제거 경로에서 반드시 떼야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(remove).
 *
 * 호출 체인:  brcm_pcie_remove() → [이 함수]
 */
static void brcm_unregister_die_notifiers(struct brcm_pcie *pcie)
{
	unregister_die_notifier(&pcie->die_notifier); /* [한국어] die 체인에서 먼저 뗀다 */
	atomic_notifier_chain_unregister(&panic_notifier_list,
					 &pcie->panic_notifier); /* [한국어] panic 체인에서도 뗀다. 드라이버가 사라진 뒤 체인이 이 콜백을 부르면 해제된 메모리를 건드리게 된다 */
}

/* [한국어]
 * __brcm_pcie_remove - 하드웨어 자원을 잡은 역순으로 놓는다
 *
 * @pcie: 컨트롤러 상태.
 *
 * probe 실패 경로와 remove 가 함께 쓰는 정리 함수라 이름에 밑줄이 붙어 있다.
 *
 * 순서가 잡은 역순이다 — MSI 를 먼저 떼고(인터럽트가 더 들어오지 않게),
 * 컨트롤러를 끄고, PHY 를 멈추고, rescal 을 rearm 하고, 마지막에 클록을
 * 끈다.
 *
 * PHY 정지와 rescal rearm 의 실패는 오류만 찍고 계속 간다. 이미 정리
 * 경로라 되돌릴 방법이 없고, 남은 단계를 건너뛰면 자원이 더 새기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  brcm_pcie_probe()(실패 경로) / brcm_pcie_remove() → [이 함수]
 */
static void __brcm_pcie_remove(struct brcm_pcie *pcie)
{
	brcm_msi_remove(pcie); /* [한국어] MSI 를 먼저 뗀다 — 인터럽트가 더 들어오지 않게 하는 것이 우선이다 */
	brcm_pcie_turn_off(pcie); /* [한국어] 링크를 재우고 PHY 전류를 끊고 브리지를 리셋에 넣는다 */
	if (brcm_phy_stop(pcie)) /* [한국어] PHY 를 멈춘다 */
		dev_err(pcie->dev, "Could not stop phy\n"); /* [한국어] 실패해도 정리를 계속해야 하므로 알리기만 한다 */
	if (reset_control_rearm(pcie->rescal)) /* [한국어] 리셋을 다시 쓸 수 있게 되돌린다 */
		dev_err(pcie->dev, "Could not rearm rescal reset\n"); /* [한국어] 실패해도 알리기만 한다 */
	clk_disable_unprepare(pcie->clk); /* [한국어] 마지막에 클록을 끈다 — 잡은 역순이다 */
}

/* [한국어]
 * brcm_pcie_remove - 플랫폼 드라이버의 remove 콜백
 *
 * @pdev: 플랫폼 디바이스.
 *
 * 순서가 요점이다. PCI 버스를 먼저 접어야(stop → remove) 하류 장치
 * 드라이버들이 정리되고, 그 뒤에야 컨트롤러를 꺼도 안전하다. 반대로 하면
 * 살아 있는 장치 드라이버가 죽은 컨트롤러에 접근한다.
 *
 * stop 과 remove 가 나뉜 것은 PCI 코어의 규약이다 — stop 이 드라이버를
 * 떼고, remove 가 장치 구조체를 없앤다.
 *
 * 오류 보고 알림은 has_err_report 인 칩에서만 걸어 두었으므로, 뗄 때도
 * 같은 조건을 본다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  드라이버 코어 → [이 함수] → __brcm_pcie_remove()
 */
static void brcm_pcie_remove(struct platform_device *pdev)
{
	struct brcm_pcie *pcie = platform_get_drvdata(pdev); /* [한국어] 플랫폼 디바이스에 걸어 둔 컨트롤러 상태를 되찾는다 */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie); /* [한국어] PCI 버스를 접기 위해 바깥 브리지를 되찾는다 */

	pci_stop_root_bus(bridge->bus); /* [한국어] 하류 장치 드라이버들을 먼저 뗀다 */
	pci_remove_root_bus(bridge->bus); /* [한국어] 그다음 장치 구조체들을 없앤다. 컨트롤러를 끄기 전에 이 둘이 끝나야 안전하다 */
	if (pcie->cfg->has_err_report) /* [한국어] 알림을 걸어 둔 칩에서만 */
		brcm_unregister_die_notifiers(pcie); /* [한국어] 뗀다 */

	__brcm_pcie_remove(pcie); /* [한국어] 하드웨어 자원을 잡은 역순으로 놓는다 */
}

/* [한국어] 일반 STB 칩의 레지스터 오프셋 표.
 * 위 익명 enum 을 인덱스로 쓰는 지정 초기화라, 값의 순서가 아니라 이름으로
 * 자리가 정해진다. 이 표를 쓰는 칩이 GENERIC/BCM2711/BCM4908/BCM7435 다. */
static const int pcie_offsets[] = {
	[RGR1_SW_INIT_1]	= 0x9210, /* [한국어] 브리지 리셋과 PERST# 비트가 있는 레지스터 */
	[EXT_CFG_INDEX]		= 0x9000, /* [한국어] config 접근 시 BDF 를 쓰는 인덱스 레지스터 */
	[EXT_CFG_DATA]		= 0x8000, /* [한국어] 그 창을 통해 읽고 쓰는 데이터 레지스터 */
	[PCIE_HARD_DEBUG]	= 0x4204, /* [한국어] SerDes IDDQ 와 CLKREQ 비트가 있는 레지스터 */
	[PCIE_INTR2_CPU_BASE]	= 0x4300, /* [한국어] legacy MSI 가 나눠 쓰는 인터럽트 블록의 시작 */
};

/* [한국어] BCM7278 계열의 오프셋 표.
 * 일반 판과 다른 것은 RGR1_SW_INIT_1 자리 하나뿐이다(0x9210 → 0xc010).
 * 그 한 값 때문에 표가 따로 있다. */
static const int pcie_offsets_bcm7278[] = {
	[RGR1_SW_INIT_1]	= 0xc010, /* [한국어] 이 칩만 브리지 리셋 레지스터가 다른 자리에 있다 */
	[EXT_CFG_INDEX]		= 0x9000, /* [한국어] 나머지는 일반 판과 같다 */
	[EXT_CFG_DATA]		= 0x8000, /* [한국어] 같음 */
	[PCIE_HARD_DEBUG]	= 0x4204, /* [한국어] 같음 */
	[PCIE_INTR2_CPU_BASE]	= 0x4300, /* [한국어] 같음 */
};

/* [한국어] BCM7425(BMIPS 계열)의 오프셋 표.
 * RGR1 자리가 다르고, config 인덱스/데이터가 4바이트 간격으로 나란히 있다.
 * 데이터 창이 레지스터 하나 크기라는 사실이 그 배치에 드러나며, 그래서
 * 이 칩만 brcm7425_pcie_map_bus() 를 쓴다. */
static const int pcie_offsets_bcm7425[] = {
	[RGR1_SW_INIT_1]	= 0x8010, /* [한국어] 이 칩의 브리지 리셋 레지스터 자리 */
	[EXT_CFG_INDEX]		= 0x8300, /* [한국어] config 인덱스 */
	[EXT_CFG_DATA]		= 0x8304, /* [한국어] 바로 4바이트 뒤가 데이터 — 창이 한 레지스터 크기임을 보여 준다 */
	[PCIE_HARD_DEBUG]	= 0x4204, /* [한국어] 일반 판과 같음 */
	[PCIE_INTR2_CPU_BASE]	= 0x4300, /* [한국어] 일반 판과 같음 */
};

/* [한국어] BCM7712 계열(BCM2712 포함)의 오프셋 표.
 * HARD_DEBUG 와 INTR2_CPU 자리가 옮겨 간 것이 일반 판과의 차이다. */
static const int pcie_offsets_bcm7712[] = {
	[RGR1_SW_INIT_1]	= 0x9210, /* [한국어] 일반 판과 같음 */
	[EXT_CFG_INDEX]		= 0x9000, /* [한국어] 일반 판과 같음 */
	[EXT_CFG_DATA]		= 0x8000, /* [한국어] 일반 판과 같음 */
	[PCIE_HARD_DEBUG]	= 0x4304, /* [한국어] 이 계열만 HARD_DEBUG 가 0x100 뒤로 옮겨 갔다 */
	[PCIE_INTR2_CPU_BASE]	= 0x4400, /* [한국어] 인터럽트 블록도 0x100 뒤로 옮겨 갔다 */
};

/* [한국어] 특별한 예외가 없는 STB 칩의 기본 표. bcm7211/bcm7445 가 이것을 쓴다. */
static const struct pcie_cfg_data generic_cfg = {
	.offsets	= pcie_offsets, /* [한국어] 일반 오프셋 표 */
	.soc_base	= GENERIC, /* [한국어] 특별한 분기가 없는 기본 계열 */
	.perst_set	= brcm_pcie_perst_set_generic, /* [한국어] RGR1 안의 PERST# 비트를 쓰는 방식 */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic, /* [한국어] RGR1 안의 INIT 비트를 쓰는 방식 */
	.num_inbound_wins = 3, /* [한국어] 안쪽 창 셋(1 비활성, 2 사용, 3 비활성) */
};

/* [한국어] BCM2711(라즈베리파이 4)의 표.
 * 함수 포인터와 창 수는 일반 판과 같고, soc_base 만 다르다. 그 값이
 * setup 에서 PERST# 를 명시적으로 거는 분기와 버스트 인코딩 분기를 만든다. */
static const struct pcie_cfg_data bcm2711_cfg = {
	.offsets	= pcie_offsets, /* [한국어] 일반 오프셋 표 */
	.soc_base	= BCM2711, /* [한국어] setup/버스트 분기의 근거가 되는 값 */
	.perst_set	= brcm_pcie_perst_set_generic, /* [한국어] 일반 판과 같음 */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic, /* [한국어] 일반 판과 같음 */
	.num_inbound_wins = 3, /* [한국어] 일반 판과 같음 */
};

/* [한국어] BCM2712(라즈베리파이 5)의 표.
 * soc_base 로 BCM7712 를 쓴다 — 안쪽 창과 UBUS 설정이 그 계열과 같기
 * 때문이다. 이 칩만의 것은 post_setup(PHY 후처리)과 브리지를 끄지 말라는
 * quirk 다. */
static const struct pcie_cfg_data bcm2712_cfg = {
	.offsets	= pcie_offsets_bcm7712, /* [한국어] 7712 계열 오프셋 표 */
	.soc_base	= BCM7712, /* [한국어] 창/UBUS 동작이 7712 와 같아 같은 값을 쓴다 */
	.perst_set	= brcm_pcie_perst_set_7278, /* [한국어] PERST 비트가 옮겨 간 방식 */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic, /* [한국어] 브리지 리셋은 일반 판 방식 */
	.post_setup	= brcm_pcie_post_setup_bcm2712, /* [한국어] 이 칩만 PHY 후처리가 필요하다 */
	.quirks		= CFG_QUIRK_AVOID_BRIDGE_SHUTDOWN, /* [한국어] 브리지를 끄면 RESCAL 접근이 막혀 문제가 되므로 끄지 않는다 */
	.num_inbound_wins = 10, /* [한국어] 안쪽 창을 열 개까지 쓴다 */
};

/* [한국어] BCM4908 의 표. PERST# 를 리셋 컨트롤러로만 만질 수 있는 것이 유일한 차이다. */
static const struct pcie_cfg_data bcm4908_cfg = {
	.offsets	= pcie_offsets, /* [한국어] 일반 오프셋 표 */
	.soc_base	= BCM4908, /* [한국어] probe 의 리비전 검사 분기 근거 */
	.perst_set	= brcm_pcie_perst_set_4908, /* [한국어] 리셋 컨트롤러 전용 구현 */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic, /* [한국어] 브리지 리셋은 일반 판 방식 */
	.num_inbound_wins = 3, /* [한국어] 안쪽 창 셋 */
};

/* [한국어] BCM7278 의 표. 오프셋 표와 브리지 리셋 방식이 모두 전용이다. */
static const struct pcie_cfg_data bcm7278_cfg = {
	.offsets	= pcie_offsets_bcm7278, /* [한국어] RGR1 자리가 다른 전용 표 */
	.soc_base	= BCM7278, /* [한국어] 버스트 인코딩 분기의 근거 */
	.perst_set	= brcm_pcie_perst_set_7278, /* [한국어] PERST 비트가 옮겨 간 방식 */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_7278, /* [한국어] INIT 비트 자리가 다른 전용 구현 */
	.num_inbound_wins = 3, /* [한국어] 안쪽 창 셋 */
};

/* [한국어] BCM7425 의 표. BMIPS 계열이라 config 접근과 MSI 배치가 갈린다. */
static const struct pcie_cfg_data bcm7425_cfg = {
	.offsets	= pcie_offsets_bcm7425, /* [한국어] 인덱스/데이터가 나란히 붙은 전용 표 */
	.soc_base	= BCM7425, /* [한국어] is_bmips() 가 참이 되는 값 */
	.perst_set	= brcm_pcie_perst_set_generic, /* [한국어] 일반 판과 같음 */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic, /* [한국어] 일반 판과 같음 */
	.num_inbound_wins = 3, /* [한국어] 안쪽 창 셋 */
};

/* [한국어] BCM7435 의 표. 같은 BMIPS 계열이지만 오프셋은 일반 판을 쓴다. */
static const struct pcie_cfg_data bcm7435_cfg = {
	.offsets	= pcie_offsets, /* [한국어] 일반 오프셋 표 — 7425 와 달리 config 창 배치가 일반적이다 */
	.soc_base	= BCM7435, /* [한국어] is_bmips() 가 참이 되는 값 */
	.perst_set	= brcm_pcie_perst_set_generic, /* [한국어] 일반 판과 같음 */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic, /* [한국어] 일반 판과 같음 */
	.num_inbound_wins = 3, /* [한국어] 안쪽 창 셋 */
};

/* [한국어] BCM7216 의 표. 이 파일에서 오류 보고와 PHY 제어를 모두 켜는
 * 유일한 칩이라, die/panic 알림과 bridge_lock 이 이 칩에서만 살아 있다. */
static const struct pcie_cfg_data bcm7216_cfg = {
	.offsets	= pcie_offsets_bcm7278, /* [한국어] 7278 과 같은 오프셋 표 */
	.soc_base	= BCM7278, /* [한국어] 7278 계열로 취급한다 */
	.perst_set	= brcm_pcie_perst_set_7278, /* [한국어] PERST 비트가 옮겨 간 방식 */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_7278, /* [한국어] INIT 비트 자리가 다른 전용 구현 */
	.has_phy	= true, /* [한국어] PHY 제어 레지스터가 있어 brcm_phy_start/stop 이 실제로 동작한다 */
	.num_inbound_wins = 3, /* [한국어] 안쪽 창 셋 */
	.has_err_report = true, /* [한국어] 오류 기록 레지스터가 있어 die/panic 덤프를 켠다 */
};

/* [한국어] BCM7712 의 표. 안쪽 창을 여럿 쓰고 UBUS remap 설정이 필요한 계열이다. */
static const struct pcie_cfg_data bcm7712_cfg = {
	.offsets	= pcie_offsets_bcm7712, /* [한국어] 7712 전용 오프셋 표 */
	.perst_set	= brcm_pcie_perst_set_7278, /* [한국어] PERST 비트가 옮겨 간 방식 */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic, /* [한국어] 브리지 리셋은 일반 판 방식 */
	.soc_base	= BCM7712, /* [한국어] 창/UBUS 분기의 근거 */
	.num_inbound_wins = 10, /* [한국어] 안쪽 창을 열 개까지 쓴다 */
};

/* [한국어] DT compatible 문자열과 위 표를 잇는 매칭 표.
 * probe 가 of_device_get_match_data() 로 여기서 표를 꺼내며, 그 한 번의
 * 조회가 이 드라이버의 모든 변종 분기의 출발점이 된다. */
static const struct of_device_id brcm_pcie_match[] = {
	{ .compatible = "brcm,bcm2711-pcie", .data = &bcm2711_cfg }, /* [한국어] 라즈베리파이 4 */
	{ .compatible = "brcm,bcm2712-pcie", .data = &bcm2712_cfg }, /* [한국어] 라즈베리파이 5 */
	{ .compatible = "brcm,bcm4908-pcie", .data = &bcm4908_cfg }, /* [한국어] BCM4908 */
	{ .compatible = "brcm,bcm7211-pcie", .data = &generic_cfg }, /* [한국어] BCM7211 은 예외가 없어 기본 표를 쓴다 */
	{ .compatible = "brcm,bcm7216-pcie", .data = &bcm7216_cfg }, /* [한국어] 오류 보고와 PHY 를 켜는 칩 */
	{ .compatible = "brcm,bcm7278-pcie", .data = &bcm7278_cfg }, /* [한국어] BCM7278 */
	{ .compatible = "brcm,bcm7425-pcie", .data = &bcm7425_cfg }, /* [한국어] BMIPS, config 창이 좁은 칩 */
	{ .compatible = "brcm,bcm7435-pcie", .data = &bcm7435_cfg }, /* [한국어] BMIPS */
	{ .compatible = "brcm,bcm7445-pcie", .data = &generic_cfg }, /* [한국어] BCM7445 도 예외가 없어 기본 표 */
	{ .compatible = "brcm,bcm7712-pcie", .data = &bcm7712_cfg }, /* [한국어] BCM7712 */
	{}, /* [한국어] 표의 끝을 알리는 빈 항목 */
};

/* [한국어] 일반 칩용 pci_ops.
 * config 읽기/쓰기는 코어의 범용 구현을 그대로 쓰고, 주소를 만드는 map_bus
 * 만 이 파일이 제공한다. add_bus/remove_bus 를 두는 것이 이 드라이버의
 * 특징인데, RC 바로 아래 첫 버스가 생길 때 슬롯 전원을 켜고 링크를 세워야
 * 하기 때문이다. */
static struct pci_ops brcm_pcie_ops = {
	.map_bus = brcm_pcie_map_bus, /* [한국어] config 접근 주소를 만든다 */
	.read = pci_generic_config_read, /* [한국어] 바이트/워드 폭을 가리지 않는 범용 읽기 */
	.write = pci_generic_config_write, /* [한국어] 범용 쓰기 */
	.add_bus = brcm_pcie_add_bus, /* [한국어] 버스가 생길 때 전원을 켜고 링크를 세운다 */
	.remove_bus = brcm_pcie_remove_bus, /* [한국어] 버스가 사라질 때 전원을 끈다 */
};

/* [한국어] BCM7425 전용 pci_ops.
 * 두 곳이 다르다 — map_bus 가 전용 구현이고, 읽기/쓰기가 32비트 전용
 * 판이다. 이 칩의 config 데이터 창이 32비트 레지스터 하나라 그보다 좁은
 * 폭의 접근을 그대로 낼 수 없어, 코어가 32비트로 읽어 잘라 주는 판을 쓴다. */
static struct pci_ops brcm7425_pcie_ops = {
	.map_bus = brcm7425_pcie_map_bus, /* [한국어] 오프셋까지 인덱스에 넣는 전용 구현 */
	.read = pci_generic_config_read32, /* [한국어] 32비트 단위로만 접근하는 범용 읽기 */
	.write = pci_generic_config_write32, /* [한국어] 32비트 단위 읽고-고쳐-쓰기로 처리하는 범용 쓰기 */
	.add_bus = brcm_pcie_add_bus, /* [한국어] 일반 판과 같음 */
	.remove_bus = brcm_pcie_remove_bus, /* [한국어] 일반 판과 같음 */
};

/* [한국어]
 * brcm_pcie_probe - 플랫폼 디바이스를 받아 PCIe 호스트를 세운다
 *
 * @pdev: DT 가 만든 플랫폼 디바이스.
 * @return: 0 성공. 각 단계의 실패 이유를 그대로 올린다.
 *
 * 이 파일의 입구다. 크게 [자원 확보] → [하드웨어 기동] → [PCI 등록] 세
 * 토막으로 읽으면 된다.
 *
 * [자원 확보]
 *   호스트 브리지를 잡을 때 sizeof(*pcie) 를 함께 달라고 해서, brcm_pcie 를
 *   브리지의 private 영역에 얹는다. 그래서 이 드라이버는 어디서든
 *   pci_host_bridge_from_priv() 로 브리지를, bus->sysdata 로 pcie 를
 *   되찾을 수 있다.
 *
 *   cfg 표를 of_device_get_match_data 로 가져온다. 이 한 줄이 아래 모든
 *   변종 분기의 출발점이다.
 *
 *   레지스터 창, 클록, 리셋 넷("rescal"/"perst"/"bridge"/"swinit")을 잡는다.
 *   전부 optional 이라 보드가 갖지 않은 것은 NULL 이 되고, 각 사용처가
 *   NULL 을 확인한다.
 *
 *   [관찰] of_pci_get_max_link_speed() 의 결과를 ret 에 받은 뒤,
 *   알 수 없는 속도일 때만 pcie->gen 을 0 으로 놓는다. 그런데 이 트리
 *   전체에서 pcie->gen 에 0 이 아닌 값이 들어가는 곳이 없다
 *   (스냅숏 1f0e418bb6 기준으로도 대입은 이 한 줄뿐이다). 브리지 private
 *   영역은 0 으로 초기화되므로 pcie->gen 은 항상 0 이고, 따라서
 *   brcm_pcie_start_link() 의 세대 제한(brcm_pcie_set_gen 호출)은 실제로는
 *   일어나지 않는다. 상류 코드 그대로이며 여기서는 고치지 않는다.
 *
 * [하드웨어 기동]
 *   클록 → 브리지 리셋 해제 → swinit 리셋 펄스 → rescal 리셋 → PHY 시작
 *   → setup 순이다. swinit 펄스 사이의 1us 대기는 상류 주석대로 하드웨어
 *   팀이 권고한 값으로, 리셋이 제대로 동기화되고 전파될 시간이다.
 *
 *   이 구간의 실패 처리 방식이 두 가지로 갈린다. setup 이전에는 잡은 것을
 *   손으로 하나씩 되돌리고(클록 끄기, rescal rearm), setup 이후에는
 *   fail 라벨로 가 __brcm_pcie_remove() 가 한꺼번에 정리한다. 경계가
 *   setup 인 이유는 그 시점부터 정리할 것이 여러 개로 늘기 때문이다.
 *
 *   칩 리비전을 읽어 BCM4908 의 특정 리비전 이상을 거른다. 그 리비전은
 *   PERST# 배선이 이 드라이버가 다루는 방식과 달라 지원하지 않는다.
 *
 * [PCI 등록]
 *   MSI 는 조건이 두 겹이다. 커널 전체에서 MSI 가 켜져 있어야 하고,
 *   DT 의 msi-parent 가 자기 자신을 가리켜야 한다. 다른 노드를 가리키면
 *   외부 MSI 컨트롤러(예: bcm2712 의 MIP)를 쓰는 구성이므로 내장 MSI 를
 *   세우지 않는다 — 파일 끝의 MODULE_SOFTDEP 가 그 외부 드라이버를 먼저
 *   올리라고 알리는 것도 같은 맥락이다.
 *
 *   pci_ops 를 BCM7425 만 따로 갈아 끼운다. 그 칩의 config 데이터 창이
 *   레지스터 하나 크기라 map_bus 구현이 다르기 때문이다.
 *
 *   pci_host_probe() 가 버스를 스캔한다. 이때 add_bus 콜백이 불려 링크가
 *   서므로, 스캔이 끝난 뒤 링크를 다시 확인해 서지 않았으면 -ENODEV 로
 *   친다. 실패하면 brcm_pcie_remove(pdev) 를 직접 불러 정리하는데,
 *   이미 platform_set_drvdata 까지 끝난 뒤라 remove 경로가 그대로 성립한다.
 *
 *   마지막으로 오류 보고를 쓰는 칩이면 스핀락을 초기화하고 die/panic
 *   알림을 건다. 락 초기화가 알림 등록보다 먼저여야 한다 — 등록 직후
 *   알림이 오면 그 콜백이 이 락을 잡기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:  드라이버 코어 → [이 함수] → brcm_pcie_setup()
 *               → brcm_pcie_enable_msi() → pci_host_probe()
 *                 → (버스 스캔) → brcm_pcie_add_bus() → brcm_pcie_start_link()
 */
static int brcm_pcie_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node; /* [한국어] 이 컨트롤러의 DT 노드. 아래에서 여러 속성을 여기서 읽는다 */
	struct pci_host_bridge *bridge; /* [한국어] PCI 코어에 넘길 호스트 브리지 */
	const struct pcie_cfg_data *data; /* [한국어] compatible 로 고른 변종 상수 표 */
	struct brcm_pcie *pcie; /* [한국어] 브리지 private 영역에 얹을 이 드라이버의 상태 */
	int ret; /* [한국어] 각 단계의 결과 */

	bridge = devm_pci_alloc_host_bridge(&pdev->dev, sizeof(*pcie)); /* [한국어] brcm_pcie 를 담을 자리까지 함께 달라고 해서 한 번에 잡는다 — 그래서 어디서든 pci_host_bridge_from_priv() 로 서로를 되찾을 수 있다 */
	if (!bridge) /* [한국어] 메모리가 없으면 */
		return -ENOMEM; /* [한국어] 아직 잡은 것이 없다 */

	data = of_device_get_match_data(&pdev->dev); /* [한국어] DT compatible 에 대응하는 변종 표를 꺼낸다 */
	if (!data) { /* [한국어] 매칭 표에 없는 compatible 이면 */
		pr_err("failed to look up compatible string\n"); /* [한국어] 아직 device 로 로그를 찍을 준비가 안 되어 pr_err 를 쓴다 */
		return -EINVAL; /* [한국어] 변종을 모르면 아무것도 할 수 없다 */
	}

	pcie = pci_host_bridge_priv(bridge); /* [한국어] 브리지의 private 영역이 곧 이 드라이버의 상태다 */
	pcie->dev = &pdev->dev; /* [한국어] 로그와 devm 의 기준 */
	pcie->np = np; /* [한국어] DT 노드를 기억해 둔다 */
	pcie->cfg = data; /* [한국어] 이 한 줄이 아래 모든 변종 분기의 출발점이다 */

	pcie->base = devm_platform_ioremap_resource(pdev, 0); /* [한국어] DT 의 첫 번째 레지스터 자원을 매핑한다 */
	if (IS_ERR(pcie->base)) /* [한국어] 매핑 실패면 */
		return PTR_ERR(pcie->base); /* [한국어] 그 오류를 그대로 올린다 */

	pcie->clk = devm_clk_get_optional(&pdev->dev, "sw_pcie"); /* [한국어] 컨트롤러 클록. optional 이라 보드에 없으면 NULL 이 된다 */
	if (IS_ERR(pcie->clk)) /* [한국어] 실제 오류(예: 이름은 있는데 아직 준비 안 됨)면 */
		return PTR_ERR(pcie->clk); /* [한국어] 그대로 올린다. -EPROBE_DEFER 이면 코어가 나중에 다시 부른다 */

	ret = of_pci_get_max_link_speed(np); /* [한국어] DT 의 max-link-speed 를 읽는다 */
	if (pcie_get_link_speed(ret) == PCI_SPEED_UNKNOWN) /* [한국어] 알 수 없는 속도이면 */
		pcie->gen = 0; /* [한국어] 제한 없음으로 둔다. [관찰] 이 트리에서 pcie->gen 에 대입하는 곳은 이 한 줄뿐이고 private 영역이 0 으로 초기화되므로, 결과적으로 세대 제한은 일어나지 않는다 */

	pcie->ssc = of_property_read_bool(np, "brcm,enable-ssc"); /* [한국어] DT 가 SSC 를 요청했는지 기억해 둔다. start_link 가 이 값을 본다 */

	pcie->rescal = devm_reset_control_get_optional_shared(&pdev->dev, "rescal"); /* [한국어] 아날로그 보정 블록의 공유 리셋. 여러 컨트롤러가 한 블록을 나눠 쓰므로 shared 다 */
	if (IS_ERR(pcie->rescal)) /* [한국어] 오류면 */
		return PTR_ERR(pcie->rescal); /* [한국어] 그대로 올린다 */

	pcie->perst_reset = devm_reset_control_get_optional_exclusive(&pdev->dev, "perst"); /* [한국어] BCM4908 에서 PERST# 를 만드는 리셋. 이 드라이버만 소유하므로 exclusive 다 */
	if (IS_ERR(pcie->perst_reset)) /* [한국어] 오류면 */
		return PTR_ERR(pcie->perst_reset); /* [한국어] 그대로 올린다 */

	pcie->bridge_reset = devm_reset_control_get_optional_exclusive(&pdev->dev, "bridge"); /* [한국어] 브리지 리셋을 레지스터 대신 리셋 컨트롤러로 다루는 보드용 */
	if (IS_ERR(pcie->bridge_reset)) /* [한국어] 오류면 */
		return PTR_ERR(pcie->bridge_reset); /* [한국어] 그대로 올린다 */

	pcie->swinit_reset = devm_reset_control_get_optional_exclusive(&pdev->dev, "swinit"); /* [한국어] probe 초기에 한 번 펄스로 줄 리셋 */
	if (IS_ERR(pcie->swinit_reset)) /* [한국어] 오류면 */
		return PTR_ERR(pcie->swinit_reset); /* [한국어] 그대로 올린다 */

	ret = clk_prepare_enable(pcie->clk); /* [한국어] 여기부터 하드웨어를 만지므로 클록을 켠다 */
	if (ret) /* [한국어] 못 켰으면 */
		return dev_err_probe(&pdev->dev, ret, "could not enable clock\n"); /* [한국어] dev_err_probe 는 -EPROBE_DEFER 를 조용히 처리해 준다 */

	ret = brcm_pcie_bridge_sw_init_set(pcie, 0); /* [한국어] 브리지를 리셋에서 꺼내 레지스터에 닿을 수 있게 한다 */
	if (ret) /* [한국어] 실패했으면 */
		return dev_err_probe(&pdev->dev, ret,
				     "could not de-assert bridge reset\n"); /* [한국어] 클록을 끄지 않고 돌아가는데, 이 지점의 상류 코드가 그렇다 */

	if (pcie->swinit_reset) { /* [한국어] DT 가 swinit 리셋을 주었으면 */
		ret = reset_control_assert(pcie->swinit_reset); /* [한국어] 먼저 건다 */
		if (ret) { /* [한국어] 실패했으면 */
			clk_disable_unprepare(pcie->clk); /* [한국어] 켠 클록을 되돌리고 */
			return dev_err_probe(&pdev->dev, ret,
					     "could not assert reset 'swinit'\n"); /* [한국어] 오류를 올린다 */
		}

		/* HW team recommends 1us for proper sync and propagation of reset */
		udelay(1); /* [한국어] 상류 주석대로 하드웨어 팀이 권고한 1us — 리셋이 동기화되고 전파될 시간이다 */

		ret = reset_control_deassert(pcie->swinit_reset); /* [한국어] 다시 푼다. 이 펄스가 swinit 리셋의 전부다 */
		if (ret) { /* [한국어] 실패했으면 */
			clk_disable_unprepare(pcie->clk); /* [한국어] 클록을 되돌리고 */
			return dev_err_probe(&pdev->dev, ret,
					     "could not de-assert reset 'swinit'\n"); /* [한국어] 오류를 올린다 */
		}
	}

	ret = reset_control_reset(pcie->rescal); /* [한국어] 아날로그 보정 리셋을 건다 */
	if (ret) { /* [한국어] 실패했으면 */
		clk_disable_unprepare(pcie->clk); /* [한국어] 클록을 되돌리고 */
		return dev_err_probe(&pdev->dev, ret, "failed to deassert 'rescal'\n"); /* [한국어] 오류를 올린다 */
	}

	ret = brcm_phy_start(pcie); /* [한국어] PHY 를 켠다. 이 칩에 PHY 제어가 없으면 아무 일도 하지 않고 0 이다 */
	if (ret) { /* [한국어] 실패했으면 */
		reset_control_rearm(pcie->rescal); /* [한국어] 방금 쓴 rescal 리셋을 되돌리고 */
		clk_disable_unprepare(pcie->clk); /* [한국어] 클록도 되돌린다 */
		return ret; /* [한국어] brcm_phy_cntl 이 이미 로그를 찍었으므로 오류만 올린다 */
	}

	ret = brcm_pcie_setup(pcie); /* [한국어] 레지스터 초기화와 창 설정의 본체 */
	if (ret) /* [한국어] 실패했으면 */
		goto fail; /* [한국어] 여기부터는 정리할 것이 여러 개라 공통 정리 경로로 간다 */

	pcie->hw_rev = readl(pcie->base + PCIE_MISC_REVISION); /* [한국어] 칩 리비전을 읽는다. MSI 배치 판정이 이 값에 달려 있어 아래 MSI 설정보다 먼저여야 한다 */
	if (pcie->cfg->soc_base == BCM4908 &&
	    pcie->hw_rev >= BRCM_PCIE_HW_REV_3_20) { /* [한국어] BCM4908 이면서 PERST# 배선이 바뀐 리비전 이상이면 */
		dev_err(pcie->dev, "hardware revision with unsupported PERST# setup\n"); /* [한국어] 이 드라이버가 다루는 방식과 달라 지원하지 않는다 */
		ret = -ENODEV; /* [한국어] 장치가 없는 것으로 처리한다 */
		goto fail; /* [한국어] 정리 경로로 간다 */
	}

	if (pci_msi_enabled()) { /* [한국어] 커널 전체에서 MSI 가 켜져 있을 때만 */
		struct device_node *msi_np = of_parse_phandle(pcie->np, "msi-parent", 0); /* [한국어] DT 의 msi-parent 가 가리키는 노드를 얻는다 */

		if (msi_np == pcie->np) /* [한국어] 자기 자신을 가리키면 내장 MSI 를 쓰라는 뜻이다 */
			ret = brcm_pcie_enable_msi(pcie); /* [한국어] 내장 MSI 컨트롤러를 세운다. 다른 노드를 가리키면 외부 컨트롤러를 쓰는 구성이라 건너뛴다 */

		of_node_put(msi_np); /* [한국어] phandle 참조 카운트를 놓는다 */

		if (ret) { /* [한국어] MSI 설정이 실패했으면 */
			dev_err(pcie->dev, "probe of internal MSI failed"); /* [한국어] 알리고 */
			goto fail; /* [한국어] 정리 경로로 간다 */
		}
	}

	bridge->ops = pcie->cfg->soc_base == BCM7425 ?
				&brcm7425_pcie_ops : &brcm_pcie_ops; /* [한국어] BCM7425 만 config 데이터 창이 좁아 전용 ops 를 쓴다 */
	bridge->sysdata = pcie; /* [한국어] map_bus 와 add_bus 콜백이 bus->sysdata 로 되찾을 값 */

	platform_set_drvdata(pdev, pcie); /* [한국어] remove 와 PM 콜백이 dev_get_drvdata 로 되찾을 값 */

	ret = pci_host_probe(bridge); /* [한국어] PCI 코어에 열거를 넘긴다. 이 안에서 add_bus 가 불려 링크가 선다 */
	if (!ret && !brcm_pcie_link_up(pcie)) /* [한국어] 열거는 성공했는데 링크가 서지 않았으면 */
		ret = -ENODEV; /* [한국어] 쓸 수 있는 장치가 없는 것이다 */

	if (ret) { /* [한국어] 둘 중 어느 쪽이든 실패면 */
		brcm_pcie_remove(pdev); /* [한국어] 이미 drvdata 까지 설정된 뒤라 remove 경로를 그대로 쓸 수 있다 */
		return ret; /* [한국어] 실패 원인을 올린다 */
	}

	if (pcie->cfg->has_err_report) { /* [한국어] 오류 기록 레지스터가 있는 칩이면 */
		spin_lock_init(&pcie->bridge_lock); /* [한국어] 락을 먼저 초기화한다 — 등록 직후 알림이 오면 콜백이 이 락을 잡기 때문이다 */
		brcm_register_die_notifiers(pcie); /* [한국어] 그다음 die/panic 알림을 건다 */
	}

	return 0; /* [한국어] 컨트롤러가 완전히 준비되었다 */

fail: /* [한국어] setup 이후의 모든 실패가 여기로 모인다 */
	__brcm_pcie_remove(pcie); /* [한국어] MSI·링크·PHY·리셋·클록을 잡은 역순으로 놓는다 */

	return ret; /* [한국어] 원래 실패 원인을 올린다 */
}

MODULE_DEVICE_TABLE(of, brcm_pcie_match); /* [한국어] 이 매칭 표를 모듈 별칭으로 내보내, DT 에 맞는 compatible 이 있으면 커널이 이 모듈을 자동으로 올리게 한다 */

/* [한국어] 이 드라이버의 절전 콜백 표.
 * noirq 단계에 다는 것이 요점이다 — 인터럽트가 이미 꺼진 뒤에 불려야
 * MSI 나 링크 이벤트가 들어오는 도중에 컨트롤러를 끄는 일이 없다. */
static const struct dev_pm_ops brcm_pcie_pm_ops = {
	.suspend_noirq = brcm_pcie_suspend_noirq, /* [한국어] 인터럽트가 꺼진 뒤 컨트롤러를 끈다 */
	.resume_noirq = brcm_pcie_resume_noirq, /* [한국어] 인터럽트가 켜지기 전에 되살린다 */
};

/* [한국어] 플랫폼 드라이버 등록 정보. */
static struct platform_driver brcm_pcie_driver = {
	.probe = brcm_pcie_probe, /* [한국어] DT 매칭이 성사되면 불린다 */
	.remove = brcm_pcie_remove, /* [한국어] 장치가 사라질 때 불린다 */
	.driver = {
		.name = "brcm-pcie", /* [한국어] sysfs 등에 보일 드라이버 이름 */
		.of_match_table = brcm_pcie_match, /* [한국어] 위에서 정의한 compatible 매칭 표 */
		.pm = &brcm_pcie_pm_ops, /* [한국어] 위에서 정의한 절전 콜백 표 */
	},
};
module_platform_driver(brcm_pcie_driver); /* [한국어] module_init/exit 를 만들어 이 드라이버를 등록·해제한다 */

MODULE_LICENSE("GPL"); /* [한국어] GPL 라이선스임을 커널에 알린다 — GPL 전용 심볼을 쓰려면 필요하다 */
MODULE_DESCRIPTION("Broadcom STB PCIe RC driver"); /* [한국어] modinfo 에 보일 설명 */
MODULE_AUTHOR("Broadcom"); /* [한국어] modinfo 에 보일 작성자 */
MODULE_SOFTDEP("pre: irq_bcm2712_mip"); /* [한국어] BCM2712 의 외부 MSI 컨트롤러(MIP) 드라이버를 먼저 올리라고 알린다 — 그 구성에서는 msi-parent 가 이 노드가 아니라 그쪽을 가리키므로, 그 드라이버가 없으면 MSI 를 쓸 수 없다 */
