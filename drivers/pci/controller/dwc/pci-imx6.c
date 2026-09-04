// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Freescale i.MX6 SoCs
 *
 * Copyright (C) 2013 Kosagi
 *		https://www.kosagi.com
 *
 * Author: Sean Cross <xobs@kosagi.com>
 */

/*
 * [한국어 설명] i.MX 계열 SoC 용 DesignWare PCIe 글루 드라이버 (pci-imx6.c)
 *
 * === 파일의 역할 ===
 * NXP(구 Freescale) i.MX SoC 에 들어 있는 DesignWare PCIe 코어를 리눅스에
 * 물리는 글루 드라이버다. PCIe 링크를 세우고 config 공간을 읽고 쓰는 일
 * 자체는 이 파일이 하지 않는다 — 그것은 같은 디렉터리의 공용 코어인
 * pcie-designware.c / pcie-designware-host.c / pcie-designware-ep.c 가
 * 맡는다. 이 파일이 맡는 것은 그 코어를 실제 칩 위에서 켜기 위해 필요한
 * SoC 쪽 잡일이다 — 클럭 켜기, 전원 도메인 붙이기, 리셋 풀기, PHY 초기화,
 * IOMUXC-GPR 레지스터를 통한 모드·LTSSM 제어, 그리고 세대별 하드웨어
 * 오류(errata) 우회다.
 * 파일 이름은 i.MX6 이지만 실제로는 6Q/6SX/6QP/7D/8MQ/8MM/8MP/8Q/95 까지,
 * 그리고 각 세대의 엔드포인트(EP) 판까지 **한 파일이 모두 담당한다.**
 * 세대 차이는 코드 분기가 아니라 뒤쪽의 drvdata[] 표 하나로 갈라낸다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * DesignWare 계열 드라이버는 늘 두 층이다.
 *
 *   [PCI 코어]  drivers/pci/probe.c, setup-bus.c ...
 *        ^
 *   [DWC 공용]  pcie-designware-host.c  (RC: config 접근, MSI, iATU)
 *               pcie-designware-ep.c    (EP: BAR, 인바운드 창)
 *               pcie-designware.c       (dbi 접근, 링크 대기, iATU 프로그래밍)
 *        ^  struct dw_pcie / dw_pcie_rp / dw_pcie_ep 를 통해
 *   [SoC 글루]  **이 파일**  struct imx_pcie 가 struct dw_pcie 를 품는다
 *        ^
 *   [SoC 하드웨어] IOMUXC-GPR(syscon), 클럭, 리셋, 전원 도메인, PHY
 *
 * 위층에서 아래로 내려오는 통로는 콜백 세 묶음이다.
 *   struct dw_pcie_ops       — start_link / stop_link. 코어가 링크를
 *                              세우거나 내릴 때 이 파일을 부른다.
 *   struct dw_pcie_host_ops  — init / deinit / post_init / pme_turn_off.
 *                              RC 초기화의 각 단계에서 불린다.
 *   struct dw_pcie_ep_ops    — raise_irq / get_features. EP 모드에서
 *                              쓰인다.
 * 아래로 나가는 통로는 regmap(syscon)·clk·reset·regulator·phy 서브시스템과,
 * 일부 세대에서는 직접 ioremap 한 PHY 레지스터 창(phy_base)이다.
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. probe/suspend/resume 는 드라이버
 * 코어가 부르고, host_ops 콜백은 dw_pcie_host_init() 안에서 불린다. 이
 * 파일에는 인터럽트 핸들러가 없다 — MSI 와 INTx 처리는 DWC 공용 코어와
 * GIC 쪽이 맡는다. 예외가 하나 있는데, CONFIG_ARM 에서만 등록되는
 * imx6q_pcie_abort_handler() 는 **데이터 어보트 예외 컨텍스트**에서 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 것:
 *   - pcie-designware.h — struct dw_pcie 와 dw_pcie_readl_dbi() 계열,
 *     dw_pcie_host_init()/dw_pcie_ep_init(), GEN3_RELATED_OFF 같은 DWC
 *     레지스터 정의. 이 파일이 DWC 코어를 다루는 유일한 통로다.
 *   - ../../pci.h — PCI 코어 내부 전용 헤더. PCIE_LINK_WAIT_* 같은 값과
 *     코어 밖에 공개되지 않은 함수를 쓰기 위해 필요하다.
 *   - linux/mfd/syscon/imx6q-iomuxc-gpr.h, imx7-iomuxc-gpr.h — **IOMUXC-GPR
 *     레지스터 정의.** i.MX 에서 PCIe 컨트롤러의 동작 모드, LTSSM 켜기,
 *     레퍼런스 클럭 경로, PHY 세부 설정이 모두 이 syscon 블록의 비트로
 *     제어된다. 이 파일이 SoC 를 만지는 주된 수단이다.
 *   - clk / reset / regulator / phy / pm_domain 서브시스템.
 * 이 파일에 의존하는 것: 없다. 이 파일은 심볼을 내보내지 않는 말단
 * 플랫폼 드라이버다(전수 grep 확인).
 * 데이터 흐름: 장치 트리의 compatible 문자열 -> imx_pcie_of_match[] ->
 * drvdata[] 항목 하나 -> imx_pcie->drvdata -> 이후 모든 세대 분기.
 *
 * === 주요 함수/구조체 요약 ===
 * imx_pcie_probe()        : 진입점. 자원을 모으고 RC 또는 EP 로 갈린다.
 * imx_pcie_host_init()    : RC 초기화 본체. 리셋·PHY·클럭 순서를 밟는다.
 * imx_pcie_start_link()   : LTSSM 을 켜고 링크를 기다리며 속도를 올린다.
 * imx_pcie_ltssm_enable() : GPR 비트 또는 apps_reset 으로 LTSSM 을 켠다.
 * pcie_phy_read/write()   : 메모리 맵이 아닌 i.MX6 PHY 를 한 워드씩 읽고 쓴다.
 * imx_pcie_add_lut()      : i.MX95 의 RID -> StreamID 변환표 한 칸을 채운다.
 * imx_pcie_suspend/resume_noirq() : L2 진입·복귀와 상태 저장·복원.
 * struct imx_pcie         : 이 드라이버의 인스턴스 상태 전부.
 * struct imx_pcie_drvdata : **세대별 차이를 담은 표의 한 항목.**
 * drvdata[]               : 그 표 자체. 세대 하나가 배열 원소 하나다.
 *
 * === 세대 차이를 다루는 방식: drvdata 표 ===
 * 이 파일의 뼈대다. 세대가 열넷(RC 아홉 + EP 다섯)인데도 코드에
 * "if (variant == IMX7D)" 같은 분기가 거의 없는 이유는, 차이를 전부
 * struct imx_pcie_drvdata 의 필드로 빼 두었기 때문이다. 갈라내는 축이 넷이다.
 *
 *   1. **함수 포인터** — 세대마다 절차 자체가 다른 다섯 가지를 갈아 끼운다.
 *      init_phy / enable_ref_clk / core_reset / wait_pll_lock /
 *      clr_clkreq_override. 예컨대 core_reset 은 6SX·6QP·6Q·7D·95 가
 *      각각 다른 함수를 쓰고, 8M 계열은 아예 NULL 이라 generic PHY
 *      드라이버가 대신한다.
 *   2. **플래그 비트** — 있으면 하고 없으면 건너뛰는 일들.
 *      IMX_PCIE_FLAG_IMX_PHY(자체 PHY 를 이 파일이 직접 다룸),
 *      HAS_PHYDRV(대신 generic PHY 드라이버 사용), HAS_APP_RESET,
 *      HAS_PHY_RESET, HAS_SERDES, HAS_LUT, SUPPORT_64BIT,
 *      CPU_ADDR_FIXUP, KEEP_MSI_CAP, SUPPORTS_SUSPEND 등.
 *      errata 우회도 플래그로 붙는다 — BROKEN_SUSPEND(ERR005723),
 *      8GT_ECN_ERR051586, SPEED_CHANGE_WORKAROUND, SKIP_L23_READY.
 *   3. **레지스터 좌표** — gpr(syscon 이름), ltssm_off/ltssm_mask,
 *      mode_off[]/mode_mask[]. 같은 일을 하는 비트가 세대마다 다른
 *      레지스터의 다른 자리에 있으므로 그 좌표를 표에 적어 둔다.
 *      배열인 이유는 컨트롤러가 둘인 칩(IMX_PCIE_MAX_INSTANCES 2)이
 *      있어서이고, mode_mask[id] 가 0 이면 두 컨트롤러가 같은 GPR 을
 *      공유한다는 뜻으로 읽어 id 를 0 으로 접는다.
 *   4. **ops 묶음** — dw_pcie_host_ops 를 두 벌 두었다.
 *      imx_pcie_host_ops 는 pme_turn_off 를 자체 구현으로 채우고,
 *      imx_pcie_host_dw_pme_ops 는 그것을 DWC 공용 구현에 맡기는 대신
 *      post_init 을 채운다. 바로 위 상류 주석이 이유를 밝힌다 — 옛
 *      DWC 구현에서는 iATU Ctrl2 의 PCIE_ATU_INHIBIT_PAYLOAD 가 예약
 *      비트라 공용 방식(더미 MMIO 쓰기)을 쓸 수 없다.
 *
 * === i.MX6 자체 PHY 를 다루는 방식 ===
 * 초기 세대(IMX_PCIE_FLAG_IMX_PHY)의 PHY 레지스터는 **메모리 맵이 아니다.**
 * 그래서 dbi 창의 PCIE_PHY_CTRL(0x700+0x114)과 PCIE_PHY_STAT(0x700+0x110)
 * 두 레지스터를 통해 한 워드씩 주고받는다. 절차는 주소 걸기 -> ack 대기 ->
 * 읽기/쓰기 스트로브 -> ack 대기 -> 스트로브 내리기 -> ack 내려가기 대기다.
 * pcie_phy_wait_ack() / pcie_phy_read() / pcie_phy_write() 세 함수가 그
 * 핸드셰이크를 구현하며, pcie_phy_poll_ack() 이 매 단계의 대기를 맡는다.
 * 뒤 세대(HAS_PHYDRV)는 이 방식을 버리고 drivers/phy 아래의 generic PHY
 * 드라이버에 맡긴다 — 그래서 그 세대들은 init_phy 가 NULL 이다.
 *
 * === i.MX95 의 LUT ===
 * i.MX95 에만 있는 IMX_PCIE_FLAG_HAS_LUT 는 **RID(Requester ID)를
 * StreamID 로 바꾸는 변환표**를 뜻한다. IOMMU/SMMU 가 트랜잭션의 주인을
 * 가릴 때 쓰는 값이며, 표가 32칸(IMX95_MAX_LUT)뿐이라 장치가 붙고 떨어질
 * 때마다 칸을 잡고 놓아야 한다. 그래서 이 세대만 host bridge 의
 * enable_device/disable_device 콜백을 채워 장치별로 칸을 관리한다.
 * 표 접근은 ACSCTRL 에 인덱스와 읽기/쓰기 방향을 쓰고 DATA1/DATA2 를
 * 주고받는 간접 방식이며, 인스턴스가 둘이어도 표는 하나이므로
 * imx_pcie->lock 뮤텍스로 직렬화한다.
 *
 * === 이 파일에서 눈에 띄는 것들 ===
 * 상류 주석과 코드가 스스로 밝히는 것만 적는다.
 *   - **ERR005723** — PCIe 가 L2 전원 차단을 지원하지 않는다. 영향을 받는
 *     세대는 IMX_PCIE_FLAG_BROKEN_SUSPEND 로 표시되고, 서스펜드 때
 *     정상적인 L2 진입 대신 다른 경로를 밟는다.
 *   - **ERR051624**(i.MX95) — 보조 전원이 없으면 컨트롤러가 beacon 이나
 *     PERST# 해제로 L23 Ready 에서 빠져나오지 못한다. 우회는
 *     SS_RW_REG_1[SYS_AUX_PWR_DET] 를 1 로 두는 것이다.
 *   - **ERR051586** — GEN3_RELATED_OFF[GEN3_ZRXDC_NONCOMPL] 의 기본값 1 이
 *     8GT/s 이상에서 수신기를 ZRX-DC 규격에 어긋나게 만들어 L1 에서
 *     불필요한 타임아웃을 낸다. 우회는 그 비트를 0 으로 쓰는 것이다.
 *   - **imx6q_pcie_abort_handler()** — CONFIG_ARM 에서만 쓰이며, PCIe
 *     응답이 없을 때 나는 외부 어보트를 삼켜 "전부 1 을 읽은 것" 처럼
 *     보이게 한다. 명령어를 직접 해독해 목적 레지스터에 값을 넣고 PC 를
 *     4 만큼 미는 방식이라, ARM 명령 인코딩에 직접 의존한다.
 *   - **imx_pcie_quirk()** — i.MX 6Quad 에서 커널이 레지스터 집합 너머를
 *     읽어 어보트가 나는 것을 막으려고 루트 버스 장치의 cfg_size 를
 *     drvdata 의 dbi_length 로 줄인다.
 *   - drvdata 표에서 EP 항목들은 이름이 _EP 로 끝나고 mode 가
 *     DW_PCIE_EP_TYPE 이며 epc_features 를 가진다. of_match 표도
 *     "fsl,imx8mq-pcie-ep" 처럼 compatible 문자열이 따로다.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 다만 i.MX 보드에 M.2 NVMe SSD 를 물리면 그 SSD 가 보이기까지의 경로가
 * 정확히 이 파일이다 — 여기서 클럭과 전원을 켜고 PHY 를 올리고
 * imx_pcie_start_link() 가 LTSSM 을 돌려 링크가 서야, DWC 코어가 config
 * 공간을 읽고 PCI 코어가 nvme 드라이버를 붙일 수 있다. 링크 속도를
 * Gen2 이상으로 올리는 것도 여기(imx_pcie_start_link() 의 속도 변경
 * 단계)라, NVMe 성능이 링크 속도에서 막히는지 볼 때 먼저 보게 되는
 * 자리다. i.MX95 의 LUT 는 SMMU 를 켠 환경에서 SSD 의 DMA 가 어떤
 * StreamID 로 보이는지를 정하므로, IOMMU 관련 문제에서도 이 파일이
 * 관여한다.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>	/* [한국어] FIELD_PREP/FIELD_GET/GENMASK — 이 파일의 레지스터 필드 조작이 전부 이 매크로로 이루어진다 */
#include <linux/mfd/syscon/imx6q-iomuxc-gpr.h>	/* [한국어] clk_bulk_prepare_enable 등 클럭 API. 세대마다 클럭 개수가 달라 bulk 판을 쓴다 */
#include <linux/mfd/syscon/imx7-iomuxc-gpr.h>	/* [한국어] udelay/usleep_range/msleep — PHY 핸드셰이크와 리셋 규격 대기에 쓴다 */
#include <linux/module.h>	/* [한국어] gpiod_* — PERST# 리셋 신호를 GPIO 로 다룬다 */
#include <linux/of.h>	/* [한국어] 커널 일반 정의 */
#include <linux/of_address.h>	/* [한국어] syscon_regmap_lookup_by_compatible — IOMUXC-GPR 블록을 찾는다 */
#include <linux/pci.h>	/* [한국어] i.MX6/7 공용 GPR 레지스터와 비트 정의. IOMUXC_GPR1/8/12 등이 여기 있다 */
#include <linux/platform_device.h>	/* [한국어] i.MX7 전용 GPR 정의. IOMUXC_GPR22 의 PLL_LOCKED 비트가 여기 있다 */
#include <linux/regmap.h>	/* [한국어] module_param 등 모듈 뼈대. 이 파일은 device_initcall 을 쓰지만 헤더는 필요하다 */
#include <linux/regulator/consumer.h>	/* [한국어] of_property_read_u32, of_device_get_match_data 등 장치 트리 API */
#include <linux/resource.h>	/* [한국어] of_address_to_resource — i.MX7D 의 PHY 레지스터 창을 얻는 데 쓴다 */
#include <linux/signal.h>	/* [한국어] PCI_EXP_LNKCAP 등 PCIe 규격 상수와 struct pci_dev */
#include <linux/types.h>	/* [한국어] 플랫폼 드라이버 뼈대 */
#include <linux/interrupt.h>	/* [한국어] regmap_update_bits/read_poll_timeout — GPR 접근의 유일한 통로다 */
#include <linux/reset.h>	/* [한국어] devm_regulator_get_optional 등. vpcie/vph 전원을 다룬다 */
#include <linux/phy/pcie.h>	/* [한국어] PHY_MODE_PCIE_RC/_EP — generic PHY 에 RC 인지 EP 인지 알릴 때 쓴다 */
#include <linux/phy/phy.h>	/* [한국어] phy_init/phy_power_on/phy_set_speed 등 generic PHY API */
#include <linux/pm_domain.h>	/* [한국어] dev_pm_domain_attach_by_name — 전원 도메인을 붙인다 */
#include <linux/pm_runtime.h>	/* [한국어] 런타임 PM. device_link 의 DL_FLAG_PM_RUNTIME 과 짝이다 */

#include "../../pci.h"	/* [한국어] **drivers/pci 안쪽 전용 헤더.** pci_hp_add_bridge 나 PCIE_T_PVPERL_MS 처럼 코어 밖에 공개되지 않은 것을 쓰기 위해 필요하다 */
#include "pcie-designware.h"	/* [한국어] DWC 공용 층의 헤더. struct dw_pcie 와 dw_pcie_readl_dbi() 계열, GEN3_RELATED_OFF 등이 여기 있다 */

#define IMX8MQ_GPR_PCIE_REF_USE_PAD		BIT(9)	/* [한국어] 8M 계열 GPR14/16 의 비트 — 외부 패드에서 레퍼런스 클럭을 받는다 */
#define IMX8MQ_GPR_PCIE_CLK_REQ_OVERRIDE_EN	BIT(10)	/* [한국어] CLKREQ# 오버라이드를 켜는 스위치 비트 */
#define IMX8MQ_GPR_PCIE_CLK_REQ_OVERRIDE	BIT(11)	/* [한국어] 덮어쓸 CLKREQ# 값. **0 이 어서트(요청 있음)** 라 스위치와 극성이 반대다 */
#define IMX8MQ_GPR_PCIE_VREG_BYPASS		BIT(12)	/* [한국어] PHY 내부 레귤레이터 우회. VPH 가 3.3V 일 때 지운다 */
#define IMX8MQ_GPR12_PCIE2_CTRL_DEVICE_TYPE	GENMASK(11, 8)	/* [한국어] 8MQ 의 두 번째 컨트롤러가 쓰는 device type 필드 */

#define IMX95_PCIE_PHY_GEN_CTRL			0x0	/* [한국어] i.MX95 PHY 일반 제어 레지스터 */
#define IMX95_PCIE_REF_USE_PAD			BIT(17)	/* [한국어] 외부 패드에서 레퍼런스 클럭을 받게 하는 비트 */

#define IMX95_PCIE_PHY_MPLLA_CTRL		0x10	/* [한국어] i.MX95 MPLLA 제어 레지스터 */
#define IMX95_PCIE_PHY_MPLL_STATE		BIT(30)	/* [한국어] MPLL 이 잠겼는지 알리는 상태 비트. imx95_pcie_wait_for_phy_pll_lock() 이 이것을 본다 */

#define IMX95_PCIE_SS_RW_REG_0			0xf0	/* [한국어] i.MX95 서브시스템 읽기·쓰기 레지스터 0 */
#define IMX95_PCIE_REF_CLKEN			BIT(23)	/* [한국어] 내부 레퍼런스 클럭 공급을 켜는 비트. REF_USE_PAD 와 배타적이다 */
#define IMX95_PCIE_PHY_CR_PARA_SEL		BIT(9)	/* [한국어] PHY 제어 레지스터를 병렬 인터페이스로 접근하게 하는 비트 */
#define IMX95_PCIE_SS_RW_REG_1			0xf4	/* [한국어] i.MX95 서브시스템 읽기·쓰기 레지스터 1 */
#define IMX95_PCIE_CLKREQ_OVERRIDE_EN		BIT(8)	/* [한국어] CLKREQ# 오버라이드 스위치 */
#define IMX95_PCIE_CLKREQ_OVERRIDE_VAL		BIT(9)	/* [한국어] 덮어쓸 CLKREQ# 값. 8M 계열과 달리 스위치와 같은 방향이다 */
#define IMX95_PCIE_SYS_AUX_PWR_DET		BIT(31)	/* [한국어] **ERR051624 우회 비트** — 보조 전원이 있는 것처럼 보이게 해 L23 Ready 에서 빠져나올 수 있게 한다 */

#define IMX95_PE0_GEN_CTRL_1			0x1050	/* [한국어] i.MX95 포트 0 일반 제어 레지스터 1 */
#define IMX95_PCIE_DEVICE_TYPE			GENMASK(3, 0)	/* [한국어] 그 안의 device type 필드. RC 인지 EP 인지를 여기 적는다 */

#define IMX95_PE0_GEN_CTRL_3			0x1058	/* [한국어] i.MX95 포트 0 일반 제어 레지스터 3 */
#define IMX95_PCIE_LTSSM_EN			BIT(0)	/* [한국어] **LTSSM 활성화 비트.** drvdata[IMX95].ltssm_mask 가 이 값이다 */

#define IMX95_PE0_LUT_ACSCTRL			0x1008	/* [한국어] **LUT 접근 제어 레지스터.** 인덱스와 방향을 여기 써서 한 칸을 고른다 */
#define IMX95_PEO_LUT_RWA			BIT(16)	/* [한국어] 읽기 방향 비트. 세우면 그 칸의 내용이 DATA1/DATA2 에 나타나고, 지운 채 쓰면 DATA1/DATA2 의 값이 그 칸에 들어간다 */
#define IMX95_PE0_LUT_ENLOC			GENMASK(4, 0)	/* [한국어] 인덱스 필드. 32칸이라 5비트다 */

#define IMX95_PE0_LUT_DATA1			0x100c	/* [한국어] LUT 한 칸의 첫 워드 */
#define IMX95_PE0_LUT_VLD			BIT(31)	/* [한국어] 그 칸이 유효한지 알리는 비트. 지우면 빈 칸이 된다 */
#define IMX95_PE0_LUT_DAC_ID			GENMASK(10, 8)	/* [한국어] DAC ID 필드. 이 파일은 늘 0 을 넣는다 */
#define IMX95_PE0_LUT_STREAM_ID			GENMASK(5, 0)	/* [한국어] **StreamID 필드. 6비트뿐이라 64 이상은 넣을 수 없다** — imx_pcie_add_lut() 의 첫 검사가 그 때문이다 */

#define IMX95_PE0_LUT_DATA2			0x1010	/* [한국어] LUT 한 칸의 둘째 워드 */
#define IMX95_PE0_LUT_REQID			GENMASK(31, 16)	/* [한국어] Requester ID 필드. 어느 장치의 트랜잭션인지를 가린다 */
#define IMX95_PE0_LUT_MASK			GENMASK(15, 0)	/* [한국어] 비교 마스크 필드. RC 모드에서는 전 비트를, EP 모드에서는 0x7 만 비교한다 */

#define IMX95_SID_MASK				GENMASK(5, 0)	/* [한국어] msi-map 에서 얻은 값에서 StreamID 만 떼어 내는 마스크. MSI 글루가 앞에 붙인 2비트 컨트롤러 ID 를 지운다 */
#define IMX95_MAX_LUT				32	/* [한국어] LUT 칸 수. 이 값이 곧 동시에 다룰 수 있는 장치 수의 상한이다 */

#define IMX95_PCIE_RST_CTRL			0x3010	/* [한국어] i.MX95 리셋 제어 레지스터 */
#define IMX95_PCIE_COLD_RST			BIT(0)	/* [한국어] COLD 리셋 비트. imx95_pcie_core_reset() 이 파형에 맞춰 토글한다 */

#define to_imx_pcie(x)	dev_get_drvdata((x)->dev)	/* [한국어] DWC 쪽 구조체에서 이 드라이버 인스턴스를 되찾는 매크로. probe 가 platform_set_drvdata() 로 심어 둔 값을 꺼낸다 */

enum imx_pcie_variants {
	/* [한국어]
	 * IMX6Q - i.MX6 Quad. 이 드라이버의 출발점이 된 세대다.
	 * 설정자: of_match 표의 "fsl,imx6q-pcie" 항목이 drvdata[IMX6Q] 를 가리킨다.
	 * 읽는 자: imx_pcie_probe() 의 controller_id 결정 switch 와 WARN_ON 검사들.
	 * 특징: 자체 PHY(IMX_PHY), 속도 변경 우회, ERR005723 로 서스펜드가 깨진
	 * 세대(BROKEN_SUSPEND), dbi_length 0x200 으로 config 길이 제한 필요.
	 */
	IMX6Q,
	/* [한국어]
	 * IMX6SX - i.MX6 SoloX.
	 * 6Q 와 같은 GPR 을 쓰지만 PHY 초기화에 RX_EQ 설정이 하나 더 붙고,
	 * 코어 리셋과 레퍼런스 클럭 제어가 TEST_POWERDOWN 비트 하나로 묶여 있다.
	 * BROKEN_SUSPEND 가 아니라 SKIP_L23_READY 를 쓴다.
	 */
	IMX6SX,
	/* [한국어]
	 * IMX6QP - i.MX6 QuadPlus.
	 * 6Q 와 거의 같으나 GPR1 에 전용 소프트웨어 리셋 비트(PCIE_SW_RST)가
	 * 있어 코어 리셋을 깔끔하게 걸고 풀 수 있다. dbi_length 는 6Q 와 같다.
	 */
	IMX6QP,
	/* [한국어]
	 * IMX7D - i.MX7 Dual.
	 * PHY 레지스터가 메모리 맵으로 바뀌어(fsl,imx7d-pcie-phy phandle)
	 * ERR010728(저온에서 PLL 잠금 실패) 우회를 writel 로 직접 수행한다.
	 * PLL 잠금 대기도 이 세대부터 생긴다.
	 */
	IMX7D,
	/* [한국어]
	 * IMX8MQ - i.MX8M Quad.
	 * **컨트롤러가 둘인 첫 세대**라 controller_id 가 의미를 갖는다.
	 * probe 가 linux,pci-domain 으로 그 번호를 정하고,
	 * imx_pcie_grp_offset() 이 그것으로 GPR14/GPR16 을 고른다.
	 * 자체 PHY 를 버리고 generic PHY 드라이버(HAS_PHYDRV)를 쓴다.
	 */
	IMX8MQ,
	/* [한국어]
	 * IMX8MM - i.MX8M Mini.
	 * 8MQ 와 같은 GPR 비트 체계를 쓰지만 컨트롤러가 하나라 도메인 번호를
	 * 읽지 않는다. 레퍼런스 클럭 제어가 CLKREQ# 오버라이드로 바뀐 세대다.
	 */
	IMX8MM,
	/* [한국어]
	 * IMX8MP - i.MX8M Plus.
	 * 8MM 과 같은 계열로 다뤄지며 같은 함수 포인터 묶음을 쓴다.
	 */
	IMX8MP,
	/* [한국어]
	 * IMX8Q - i.MX8QuadMax/QuadXPlus 계열.
	 * GPR 로 모드를 정하지 않아(mode_mask 가 0) generic PHY 드라이버가
	 * RC/EP 모드까지 맡는다. EP 능력표가 8M 계열과 따로 있다.
	 */
	IMX8Q,
	/* [한국어]
	 * IMX95 - i.MX95.
	 * GPR 이 아니라 컨트롤러 자기 레지스터 창(HAS_SERDES, "app" 자원)에
	 * regmap 을 만들어 쓰고, **RID -> StreamID 변환표(HAS_LUT)** 가 있는
	 * 유일한 세대다. ERR051624 와 ERR051586 우회도 이 세대에 붙는다.
	 */
	IMX95,
	/* [한국어]
	 * IMX8MQ_EP - 8MQ 를 엔드포인트로 쓰는 항목.
	 * 아래 네 _EP 항목은 모두 같은 하드웨어를 mode = DW_PCIE_EP_TYPE 으로
	 * 쓰는 경우이며, of_match 에서 "-ep" 로 끝나는 별도 compatible 문자열에
	 * 대응한다. RC 항목과 달리 epc_features 를 가진다.
	 */
	IMX8MQ_EP,
	/* [한국어]
	 * IMX8MM_EP - 8MM 의 엔드포인트 항목.
	 */
	IMX8MM_EP,
	/* [한국어]
	 * IMX8MP_EP - 8MP 의 엔드포인트 항목.
	 */
	IMX8MP_EP,
	/* [한국어]
	 * IMX8Q_EP - 8Q 의 엔드포인트 항목. 능력표가 imx8q_pcie_epc_features 다.
	 */
	IMX8Q_EP,
	/* [한국어]
	 * IMX95_EP - i.MX95 의 엔드포인트 항목.
	 * LUT 를 쓰지만 EP 모드에서는 RID 0 하나만 등록하며, LUT 마스크도
	 * RC 와 달리 0x7 로 두어 Device ID 만 비교한다.
	 */
	IMX95_EP,
};

#define IMX_PCIE_FLAG_IMX_PHY			BIT(0)	/* [한국어] 자체 PHY 를 이 파일이 직접 다루는 세대. pcie_phy_read/write 핸드셰이크와 imx_setup_phy_mpll() 이 이 플래그에 걸려 있다 */
#define IMX_PCIE_FLAG_SPEED_CHANGE_WORKAROUND	BIT(1)	/* [한국어] Gen1 으로 먼저 링크를 세운 뒤 속도를 올리는 우회가 필요한 세대 */
#define IMX_PCIE_FLAG_SUPPORTS_SUSPEND		BIT(2)	/* [한국어] 서스펜드를 지원하는 세대. 없으면 suspend/resume 이 곧바로 나간다 */
#define IMX_PCIE_FLAG_HAS_PHYDRV		BIT(3)	/* [한국어] generic PHY 드라이버를 쓰는 세대. IMX_PHY 와 배타적이다 */
#define IMX_PCIE_FLAG_HAS_APP_RESET		BIT(4)	/* [한국어] LTSSM 을 "apps" 리셋 신호로 제어하는 세대 */
#define IMX_PCIE_FLAG_HAS_PHY_RESET		BIT(5)	/* [한국어] PHY 리셋을 표준 리셋 서브시스템으로 다루는 세대 */
#define IMX_PCIE_FLAG_HAS_SERDES		BIT(6)	/* [한국어] GPR syscon 대신 컨트롤러 자기 레지스터 창("app" 자원)을 쓰는 세대 */
#define IMX_PCIE_FLAG_SUPPORT_64BIT		BIT(7)	/* [한국어] 64비트 DMA 를 지원하는 세대. EP 모드에서 DMA 마스크를 넓힌다 */
#define IMX_PCIE_FLAG_CPU_ADDR_FIXUP		BIT(8)	/* [한국어] CPU 주소와 PCI 주소가 달라 보정이 필요한 세대. 이 파일에서 이 비트를 읽는 곳은 없다(전수 grep 확인) */
/*
 * Because of ERR005723 (PCIe does not support L2 power down) we need to
 * workaround suspend resume on some devices which are affected by this errata.
 */
#define IMX_PCIE_FLAG_BROKEN_SUSPEND		BIT(9)	/* [한국어] 바로 위 상류 주석이 밝히는 ERR005723 의 영향을 받는 세대 — 정상적인 L2 진입 대신 리셋과 클럭 끄기로 대신한다 */
#define IMX_PCIE_FLAG_HAS_LUT			BIT(10)	/* [한국어] RID -> StreamID 변환표가 있는 세대(i.MX95) */
#define IMX_PCIE_FLAG_8GT_ECN_ERR051586		BIT(11)	/* [한국어] ERR051586 우회가 필요한 세대 — 8GT/s 이상에서 ZRX-DC 비호환 비트를 지운다 */
#define IMX_PCIE_FLAG_SKIP_L23_READY		BIT(12)	/* [한국어] L23 Ready 대기를 건너뛰는 세대. DWC 코어 쪽 skip_l23_ready 로 전달된다 */
/* Preserve MSI capability for platforms that require it */
#define IMX_PCIE_FLAG_KEEP_MSI_CAP		BIT(13)	/* [한국어] 바로 위 상류 주석대로 MSI capability 를 보존해야 하는 플랫폼. DWC 코어 쪽 keep_rp_msi_en 으로 전달된다 */

#define imx_check_flag(pci, val)	(pci->drvdata->flags & val)	/* [한국어] 세대 플래그를 확인하는 축약 매크로. 이 파일 전체가 이것으로 세대를 가른다 */

#define IMX_PCIE_MAX_INSTANCES	2	/* [한국어] 한 SoC 안의 최대 컨트롤러 수. mode_off/mode_mask 배열의 크기다 */

struct imx_pcie;	/* [한국어] 아래 함수 포인터들의 인자에 쓰이므로 미리 선언한다 */

struct imx_pcie_drvdata {
	/* [한국어]
	 * enum imx_pcie_variants variant;
	 * 이 항목이 어느 세대인지. 배열 인덱스와 같은 값이 들어간다.
	 * 설정자: drvdata[] 표의 각 항목이 자기 인덱스를 그대로 적어 둔다.
	 * 읽는 자: imx_pcie_probe() 의 switch 하나(IMX8MQ 계열에서 도메인 번호를
	 * 읽을지 정하는 자리)와 imx_pcie_grp_offset() 의 WARN_ON 검사.
	 * 값 범위: enum imx_pcie_variants 의 열넷.
	 * 동기화: 표가 const 라 읽기 전용이며 락이 없다.
	 */
	enum imx_pcie_variants variant;
	/* [한국어]
	 * enum dw_pcie_device_mode mode;
	 * 이 항목을 루트 컴플렉스로 쓸지 엔드포인트로 쓸지.
	 * 설정자: EP 항목만 DW_PCIE_EP_TYPE 을 적고, RC 항목은 0(미지정)으로 둔다.
	 * 읽는 자: imx_pcie_probe() 가 EP 경로로 갈지 정할 때,
	 * imx_pcie_configure_type() 이 GPR 에 적을 값을 정할 때,
	 * imx_pcie_host_init() 이 PHY 모드를 고를 때,
	 * imx_pcie_add_lut() 이 LUT 마스크를 정할 때.
	 * 동기화: 읽기 전용.
	 */
	enum dw_pcie_device_mode mode;
	/* [한국어]
	 * u32 flags;
	 * 이 세대가 무엇을 하고 무엇을 건너뛰는지를 담은 비트 묶음.
	 * 설정자: drvdata[] 표. 읽는 자: imx_check_flag() 매크로를 통해 파일 전역.
	 * 값 범위: IMX_PCIE_FLAG_* 열넷의 OR. 크게 세 갈래다 —
	 *   하드웨어 구성(IMX_PHY, HAS_PHYDRV, HAS_APP_RESET, HAS_PHY_RESET,
	 *   HAS_SERDES, HAS_LUT, SUPPORT_64BIT, CPU_ADDR_FIXUP),
	 *   기능 지원(SUPPORTS_SUSPEND, KEEP_MSI_CAP, SKIP_L23_READY),
	 *   errata 우회(SPEED_CHANGE_WORKAROUND, BROKEN_SUSPEND,
	 *   8GT_ECN_ERR051586).
	 * 동기화: 읽기 전용.
	 */
	u32 flags;
	/* [한국어]
	 * int dbi_length;
	 * 이 세대에서 안전하게 읽을 수 있는 config 공간의 길이.
	 * 설정자: 6Q 와 6QP 만 0x200 을 적고 나머지는 0 이다.
	 * 읽는 자: imx_pcie_quirk() 이 루트 버스 장치의 cfg_size 를 이 값으로
	 * 줄인다. 상류 주석대로 i.MX 6Quad 에서 레지스터 집합 너머를 읽으면
	 * 어보트가 나기 때문이다.
	 * 값 범위: 0 이면 제한하지 않는다.
	 */
	int dbi_length;
	/* [한국어]
	 * const char *gpr;
	 * IOMUXC-GPR syscon 노드를 찾을 때 쓰는 compatible 문자열.
	 * 설정자: 6Q 계열은 "fsl,imx6q-iomuxc-gpr", 7D 는 "fsl,imx7d-iomuxc-gpr"
	 * 같은 식이며, HAS_SERDES 세대는 NULL 이다.
	 * 읽는 자: imx_pcie_probe() 가 syscon_regmap_lookup_by_compatible() 에
	 * 넘긴다. NULL 이면 그 경로를 건너뛰고 "app" MMIO 자원으로 regmap 을
	 * 직접 만든다.
	 */
	const char *gpr;
	/* [한국어]
	 * const u32 ltssm_off;
	 * LTSSM 활성화 비트가 들어 있는 GPR 레지스터의 오프셋.
	 * 설정자: 6Q 계열은 IOMUXC_GPR12, i.MX95 는 IMX95_PE0_GEN_CTRL_3 처럼
	 * 세대마다 다르다.
	 * 읽는 자: imx_pcie_ltssm_enable()/disable() 이 regmap_update_bits() 의
	 * 주소로 쓴다. ltssm_mask 가 0 이면 이 값은 쓰이지 않는다.
	 */
	const u32 ltssm_off;
	/* [한국어]
	 * const u32 ltssm_mask;
	 * 그 레지스터 안에서 LTSSM 을 켜는 비트의 마스크.
	 * 설정자: 6Q 계열은 IMX6Q_GPR12_PCIE_CTL_2, i.MX95 는 IMX95_PCIE_LTSSM_EN.
	 * 읽는 자: imx_pcie_ltssm_enable()/disable().
	 * 값 범위: **0 이면 GPR 이 아니라 apps_reset 신호로 LTSSM 을 제어한다는
	 * 뜻**이며, 그 세대는 HAS_APP_RESET 플래그를 함께 가진다.
	 */
	const u32 ltssm_mask;
	/* [한국어]
	 * const u32 mode_off[IMX_PCIE_MAX_INSTANCES];
	 * RC/EP 모드 값을 적을 GPR 레지스터의 오프셋. 컨트롤러별로 하나씩.
	 * 설정자: drvdata[] 표. 컨트롤러가 하나뿐인 세대는 [0] 만 채운다.
	 * 읽는 자: imx_pcie_configure_type().
	 * 배열인 이유: 컨트롤러가 둘인 칩에서 두 컨트롤러의 모드 비트가 서로
	 * 다른 레지스터에 있을 수 있기 때문이다.
	 */
	const u32 mode_off[IMX_PCIE_MAX_INSTANCES];
	/* [한국어]
	 * const u32 mode_mask[IMX_PCIE_MAX_INSTANCES];
	 * 그 레지스터 안에서 모드 값이 들어갈 필드의 마스크.
	 * 설정자: drvdata[] 표.
	 * 읽는 자: imx_pcie_configure_type() 이 세 가지로 해석한다 —
	 *   mode_mask[0] 이 0 이면 이 세대는 GPR 로 모드를 정하지 않는다(generic
	 *   PHY 드라이버가 대신한다). mode_mask[id] 가 0 이면 두 컨트롤러가 GPR 을
	 *   공유한다는 뜻이라 id 를 0 으로 접는다. 그 밖에는 그대로 쓴다.
	 * 값을 넣을 때 ffs(mask) - 1 로 시프트 폭을 구하므로, 마스크가 어느
	 * 자리에 있어도 같은 코드가 동작한다.
	 */
	const u32 mode_mask[IMX_PCIE_MAX_INSTANCES];
	/* [한국어]
	 * const struct pci_epc_features *epc_features;
	 * 엔드포인트 모드에서 프레임워크에 알릴 능력표.
	 * 설정자: _EP 항목만 채운다. RC 항목은 NULL 이다.
	 * 읽는 자: imx_pcie_ep_get_features() 가 그대로 돌려주고,
	 * imx_add_pcie_ep() 이 align 값을 ep->page_size 로 쓴다.
	 * 값 범위: 이 파일의 imx8m/imx8q/imx95 세 능력표 중 하나.
	 */
	const struct pci_epc_features *epc_features;
	/* [한국어]
	 * int (*init_phy)(struct imx_pcie *pcie);
	 * PHY 파라미터와 레퍼런스 클럭 경로를 정하는 세대별 함수.
	 * 설정자: drvdata[] 표가 imx_pcie_init_phy / imx6sx_pcie_init_phy /
	 * imx8mq_pcie_init_phy / imx95_pcie_init_phy 중 하나를 적거나, generic
	 * PHY 드라이버에 맡기는 세대는 NULL 로 둔다.
	 * 읽는 자: imx_pcie_host_init() 이 리셋을 건 직후 부른다. NULL 이면
	 * 건너뛴다.
	 */
	int (*init_phy)(struct imx_pcie *pcie);
	/* [한국어]
	 * int (*enable_ref_clk)(struct imx_pcie *pcie, bool enable);
	 * 레퍼런스 클럭을 켜고 끄는 세대별 함수.
	 * 설정자: 6SX 는 TEST_POWERDOWN 비트, 6Q 는 전원+클럭 순서, 7D 는 소스
	 * 선택, 8M/95 는 CLKREQ# 오버라이드로 서로 전혀 다른 방식을 쓴다.
	 * 읽는 자: imx_pcie_clk_enable()/imx_pcie_clk_disable(), 그리고
	 * BROKEN_SUSPEND 세대의 서스펜드·리줌 경로가 직접 부른다.
	 * [관찰] 서스펜드·리줌 경로는 NULL 검사 없이 부른다 — BROKEN_SUSPEND
	 * 플래그를 가진 세대는 모두 이 포인터를 채우고 있기 때문이다.
	 */
	int (*enable_ref_clk)(struct imx_pcie *pcie, bool enable);
	/* [한국어]
	 * int (*core_reset)(struct imx_pcie *pcie, bool assert);
	 * 코어·PHY 리셋을 걸고 푸는 세대별 함수.
	 * 설정자: 6SX/6QP/6Q/7D/95 가 각각 다른 함수를 적고, 8M 계열은 NULL 로
	 * 두어 표준 리셋 서브시스템에만 맡긴다.
	 * 읽는 자: imx_pcie_assert_core_reset()/deassert_core_reset().
	 * [관찰] 세대에 따라 assert 쪽과 deassert 쪽 중 한쪽만 실제로 동작하는
	 * 구현이 있다 — 6Q 는 assert 만, 7D 와 95 는 각각 deassert 와 assert 만.
	 */
	int (*core_reset)(struct imx_pcie *pcie, bool assert);
	/* [한국어]
	 * int (*wait_pll_lock)(struct imx_pcie *pcie);
	 * PHY PLL 이 잠기기를 기다리는 세대별 함수.
	 * 설정자: i.MX95 만 imx95_pcie_wait_for_phy_pll_lock() 을 적는다.
	 * 읽는 자: imx_pcie_host_init() 이 리셋을 풀고 PERST# 를 해제한 직후
	 * 부르며, **반환값을 확인해 실패하면 초기화를 접는다.**
	 * [관찰] i.MX7D 도 PLL 잠금을 기다리지만 이 포인터를 쓰지 않는다 —
	 * imx7d_pcie_core_reset() 안에서 직접 부르고 결과를 무시한다.
	 */
	int (*wait_pll_lock)(struct imx_pcie *pcie);
	/* [한국어]
	 * void (*clr_clkreq_override)(struct imx_pcie *pcie);
	 * 링크가 선 뒤 CLKREQ# 오버라이드를 푸는 세대별 함수.
	 * 설정자: 8M 계열과 i.MX95 가 각자의 함수를 적는다.
	 * 읽는 자: imx_pcie_host_post_init() 이 **링크가 서 있고 보드가
	 * supports-clkreq 를 밝혔을 때만** 부른다. 그때부터 하드웨어 신호를
	 * 따르게 되어 전력 절감이 가능해진다.
	 */
	void (*clr_clkreq_override)(struct imx_pcie *pcie);
	/* [한국어]
	 * const struct dw_pcie_host_ops *ops;
	 * 이 세대가 쓸 dw_pcie_host_ops 묶음.
	 * 설정자: pme_turn_off 를 자체 구현으로 해야 하는 세대만
	 * imx_pcie_host_ops 를 적고, 나머지는 비워 둔다.
	 * 읽는 자: imx_pcie_probe() 가 NULL 이면 imx_pcie_host_dw_pme_ops 를
	 * 대신 쓴다. 두 묶음의 차이는 pme_turn_off 자리를 자체 구현으로 채우느냐
	 * post_init 으로 채우느냐다.
	 */
	const struct dw_pcie_host_ops *ops;
};

struct imx_lut_data {
	/* [한국어]
	 * u32 data1;
	 * LUT 한 칸의 DATA1 레지스터 사본. 유효 비트(VLD), DAC_ID, StreamID 가
	 * 이 워드에 들어 있다.
	 * 설정자: imx_pcie_lut_save() 가 서스펜드 직전에 하드웨어에서 읽어 담는다.
	 * 칸이 비어 있으면(VLD 가 없으면) 0 을 넣는다.
	 * 읽는 자: imx_pcie_lut_restore() 가 리줌 뒤 그대로 되쓴다. 값이 0 이면
	 * 그 칸을 건너뛴다 — 0 에는 VLD 가 없기 때문이다.
	 * 동기화: noirq 단계에서만 다뤄지므로 락이 없다.
	 */
	u32 data1;
	/* [한국어]
	 * u32 data2;
	 * 같은 칸의 DATA2 레지스터 사본. Requester ID 와 비교 마스크가 들어 있다.
	 * 설정자/읽는 자와 동기화는 data1 과 같다. 두 워드가 한 칸을 이루므로
	 * 언제나 함께 저장되고 함께 복원된다.
	 */
	u32 data2;
};

struct imx_pcie {
	/* [한국어]
	 * struct dw_pcie *pci;
	 * DWC 공용 코어가 쓰는 장치 구조체. 이 글루 층과 공용 층을 잇는 통로다.
	 * 설정자: imx_pcie_probe() 가 따로 devm_kzalloc 으로 잡아 서로 잇고,
	 * pci->ops 에 dw_pcie_ops 를, pci->pp.ops 에 host_ops 를 매단다.
	 * 읽는 자: dbi 레지스터에 닿아야 하는 거의 모든 함수. 반대 방향으로는
	 * to_imx_pcie(pci) 매크로가 dev_get_drvdata() 를 거쳐 이 구조체를
	 * 되찾는다.
	 * 동기화: 필드별로 DWC 코어가 관리하며 이 파일은 락을 잡지 않는다.
	 */
	struct dw_pcie		*pci;
	/* [한국어]
	 * struct gpio_desc *reset_gpiod;
	 * PERST# 리셋 신호에 연결된 GPIO.
	 * 설정자: imx_pcie_probe() 가 devm_gpiod_get_optional() 로 얻는다.
	 * 보드에 그 배선이 없으면 NULL 이다.
	 * 읽는 자: imx_pcie_assert_perst() 하나뿐이다.
	 * 값 범위: NULL 가능. NULL 이면 어서트는 조용히 넘어가고 해제 쪽은
	 * 규격 대기까지 통째로 건너뛴다.
	 */
	struct gpio_desc	*reset_gpiod;
	/* [한국어]
	 * struct clk_bulk_data *clks;
	 * 이 컨트롤러가 쓰는 클럭 전부의 배열.
	 * 설정자: imx_pcie_probe() 의 devm_clk_bulk_get_all() 이 장치 트리에
	 * 적힌 클럭을 모두 모아 채운다. 개수와 이름이 세대마다 다르다.
	 * 읽는 자: imx_pcie_clk_enable()/disable() 이 한꺼번에 켜고 끄고,
	 * imx_setup_phy_mpll() 이 이름으로 "pcie_phy" 를 찾아 주파수를 묻고,
	 * probe 가 "extref" 로 시작하는 클럭이 있는지 본다.
	 * 동기화: 클럭 서브시스템이 자체 락을 갖는다.
	 */
	struct clk_bulk_data	*clks;
	/* [한국어]
	 * int num_clks;
	 * 위 배열의 원소 수.
	 * 설정자: devm_clk_bulk_get_all() 의 반환값. **음수이면 오류**라
	 * probe 가 그 자리에서 실패한다.
	 * 읽는 자: 클럭을 켜고 끄는 두 함수와 imx_setup_phy_mpll() 의 순회 상한.
	 */
	int			num_clks;
	/* [한국어]
	 * bool supports_clkreq;
	 * 보드가 CLKREQ# 신호를 실제로 배선했는지.
	 * 설정자: imx_pcie_probe() 가 장치 트리의 supports-clkreq 불리언을 읽는다.
	 * 읽는 자: imx_pcie_host_post_init() 이 **링크가 서 있고 이 값이 참일
	 * 때만** CLKREQ# 오버라이드를 푼다. 배선이 없는 보드에서 오버라이드를
	 * 풀면 레퍼런스 클럭이 끊겨 링크가 죽을 수 있기 때문이다.
	 */
	bool			supports_clkreq;
	/* [한국어]
	 * bool enable_ext_refclk;
	 * 외부 레퍼런스 클럭을 쓰는 구성인지.
	 * 설정자: imx_pcie_probe() 가 클럭 이름 중 "extref" 로 시작하는 것이
	 * 있는지 보고 정한다. 장치 트리의 별도 속성이 아니라 **클럭 이름이
	 * 근거**라는 점이 특이하다.
	 * 읽는 자: imx95_pcie_init_phy() 가 REF_USE_PAD 와 REF_CLKEN 중 어느
	 * 경로를 켤지 정할 때.
	 */
	bool			enable_ext_refclk;
	/* [한국어]
	 * struct regmap *iomuxc_gpr;
	 * SoC 쪽 제어 레지스터에 닿는 통로. 이 드라이버가 하드웨어를 만지는
	 * 주된 수단이다.
	 * 설정자: imx_pcie_probe() 가 두 방법 중 하나로 만든다 — drvdata 의
	 * gpr 문자열로 syscon 노드를 찾거나, HAS_SERDES 세대이면 "app" 이라는
	 * 이름의 MMIO 자원을 직접 매핑해 그 위에 regmap 을 만든다.
	 * 읽는 자: 세대별 init_phy / enable_ref_clk / core_reset / LTSSM 제어 /
	 * 모드 설정 / LUT 접근 등 거의 모든 SoC 조작.
	 * 값 범위: 이름은 IOMUXC-GPR 이지만 i.MX95 에서는 컨트롤러 자기
	 * 레지스터 창을 가리킨다.
	 * 동기화: regmap 이 자체 락을 갖는다. 다만 LUT 처럼 여러 레지스터를
	 * 묶어 하나의 동작을 이루는 곳은 이 파일의 뮤텍스가 따로 필요하다.
	 */
	struct regmap		*iomuxc_gpr;
	/* [한국어]
	 * u16 msi_ctrl;
	 * 서스펜드 전에 저장해 둔 MSI capability 의 MSI_FLAGS 값.
	 * 설정자: imx_pcie_msi_save_restore(imx_pcie, true).
	 * 읽는 자: 같은 함수의 복원 쪽(false).
	 * 왜 필요한가: 서스펜드로 컨트롤러 전원이 내려가면 루트 포트 자신의
	 * config 공간이 초기값으로 돌아가 MSI 가 꺼지기 때문이다.
	 * 동기화: noirq 단계에서만 다뤄진다.
	 */
	u16			msi_ctrl;
	/* [한국어]
	 * u32 controller_id;
	 * 같은 SoC 안에서 이 컨트롤러가 몇 번째인지(0 또는 1).
	 * 설정자: imx_pcie_probe() 가 IMX8MQ 계열에서만 장치 트리의
	 * linux,pci-domain 을 읽어 넣는다. 다른 세대에서는 0 인 채로 남는다.
	 * 읽는 자: imx_pcie_grp_offset() 이 GPR14/GPR16 을 고를 때,
	 * imx_pcie_configure_type() 이 mode_off/mode_mask 배열의 첨자로 쓸 때.
	 * 값 범위: 0~1. 그 밖의 값이면 probe 가 실패한다.
	 */
	u32			controller_id;
	/* [한국어]
	 * struct reset_control *pciephy_reset;
	 * 표준 리셋 서브시스템을 통한 PHY 리셋 핸들.
	 * 설정자: imx_pcie_probe() 가 IMX_PCIE_FLAG_HAS_PHY_RESET 세대에서만
	 * "pciephy" 라는 이름으로 얻는다.
	 * 읽는 자: imx_pcie_assert_core_reset()/deassert_core_reset().
	 * 값 범위: NULL 가능. reset_control_assert(NULL) 은 조용히 넘어가므로
	 * 없는 세대에서도 같은 코드가 돈다.
	 */
	struct reset_control	*pciephy_reset;
	/* [한국어]
	 * struct reset_control *apps_reset;
	 * LTSSM 을 제어하는 "apps" 리셋 신호.
	 * 설정자: imx_pcie_probe() 가 IMX_PCIE_FLAG_HAS_APP_RESET 세대에서만 얻는다.
	 * 읽는 자: imx_pcie_ltssm_enable() 이 풀고 imx_pcie_ltssm_disable() 이 건다.
	 * **이 신호가 GPR 의 ltssm_mask 비트를 대신하는 세대가 있다** — 그 세대는
	 * ltssm_mask 가 0 이라 GPR 쓰기를 건너뛰고 이 리셋만으로 LTSSM 을 켜고 끈다.
	 */
	struct reset_control	*apps_reset;
	/* [한국어]
	 * u32 tx_deemph_gen1;
	 * Gen1 송신 디엠퍼시스 값.
	 * 설정자: imx_pcie_probe() 가 장치 트리의 fsl,tx-deemph-gen1 을 읽고,
	 * 없으면 0 으로 둔다.
	 * 읽는 자: imx_pcie_init_phy() 가 GPR8 의 해당 필드(시프트 0)에 쓴다.
	 * 값 범위: GPR8 의 필드 폭 안. 보드의 배선 길이와 손실에 맞춰 조정한다.
	 */
	u32			tx_deemph_gen1;
	/* [한국어]
	 * u32 tx_deemph_gen2_3p5db;
	 * Gen2 의 3.5dB 디엠퍼시스 값. 기본값 0.
	 * 설정자/읽는 자는 tx_deemph_gen1 과 같으며 GPR8 의 시프트 6 자리에 들어간다.
	 */
	u32			tx_deemph_gen2_3p5db;
	/* [한국어]
	 * u32 tx_deemph_gen2_6db;
	 * Gen2 의 6dB 디엠퍼시스 값. **기본값이 20** 으로 다른 셋과 다르다.
	 * 설정자/읽는 자는 위와 같으며 GPR8 의 시프트 12 자리에 들어간다.
	 */
	u32			tx_deemph_gen2_6db;
	/* [한국어]
	 * u32 tx_swing_full;
	 * 송신 스윙의 full 값. 기본값 127.
	 * 설정자/읽는 자는 위와 같으며 GPR8 의 시프트 18 자리에 들어간다.
	 */
	u32			tx_swing_full;
	/* [한국어]
	 * u32 tx_swing_low;
	 * 송신 스윙의 low 값. 기본값 127.
	 * 설정자/읽는 자는 위와 같으며 GPR8 의 시프트 25 자리에 들어간다.
	 */
	u32			tx_swing_low;
	/* [한국어]
	 * struct regulator *vpcie;
	 * PCIe 슬롯에 전원을 공급하는 레귤레이터.
	 * 설정자: imx_pcie_probe() 가 devm_regulator_get_optional() 로 얻고,
	 * 없으면(-ENODEV) NULL 로 두어 없는 것으로 다룬다.
	 * 읽는 자: imx_pcie_host_init() 이 가장 먼저 켜고,
	 * imx_pcie_host_exit() 과 host_init 의 실패 경로가 끈다.
	 * 값 범위: NULL 가능. 모든 접근이 NULL 검사로 감싸여 있다.
	 */
	struct regulator	*vpcie;
	/* [한국어]
	 * struct regulator *vph;
	 * PCIe PHY 의 전원(PCIE_VPH) 레귤레이터.
	 * 설정자: vpcie 와 같은 방식으로 얻는다.
	 * 읽는 자: imx8mq_pcie_init_phy() 하나뿐이며, **전압을 읽어 3V 를 넘으면
	 * VREG_BYPASS 를 지운다.** 데이터시트가 1.8V 를 권장하는데 3.3V 로
	 * 공급되는 보드에서 필요한 조정이다.
	 * 값 범위: NULL 가능. 켜고 끄지는 않고 전압만 묻는다.
	 */
	struct regulator	*vph;
	/* [한국어]
	 * void __iomem *phy_base;
	 * 메모리 맵된 PHY 레지스터 창의 커널 가상 주소.
	 * 설정자: imx_pcie_probe() 가 장치 트리의 fsl,imx7d-pcie-phy phandle 을
	 * 따라가 그 자원을 매핑한다. 그 phandle 이 없으면 NULL 이다.
	 * 읽는 자: imx7d_pcie_core_reset() 이 ERR010728 우회를 위해 CMN_REG4 /
	 * CMN_REG24 / CMN_REG26 에 직접 writel 한다.
	 * 값 범위: NULL 이면 우회를 못 하고 경고만 남긴다.
	 * [대비] i.MX6 자체 PHY 는 메모리 맵이 아니라 pcie_phy_read/write 의
	 * 핸드셰이크로 접근하며 이 필드를 쓰지 않는다.
	 */
	void __iomem		*phy_base;

	/* LUT data for pcie */
	/* [한국어]
	 * struct imx_lut_data luts[IMX95_MAX_LUT];
	 * 서스펜드 동안 LUT 32칸의 내용을 보관하는 자리.
	 * 설정자: imx_pcie_lut_save(). 읽는 자: imx_pcie_lut_restore().
	 * 왜 필요한가: LUT 는 GPR 블록에 있어 전원이 내려가면 사라지는데,
	 * 그 내용은 PCI 열거 중 장치별로 채워진 것이라 다시 만들 방법이 없다.
	 * 값 범위: 빈 칸은 두 워드가 모두 0 이며, 복원 때 건너뛴다.
	 * 동기화: noirq 단계에서만 다뤄진다.
	 */
	struct imx_lut_data	luts[IMX95_MAX_LUT];
	/* power domain for pcie */
	/* [한국어]
	 * struct device *pd_pcie;
	 * PCIe 컨트롤러가 속한 전원 도메인의 가상 장치.
	 * 설정자: imx_pcie_attach_pd() 가 dev_pm_domain_attach_by_name(dev,
	 * "pcie") 로 얻고 device_link_add() 로 의존 관계를 건다.
	 * 읽는 자: 이 파일에서 다시 읽는 곳은 없다 — 붙여 두는 것 자체가 목적이며,
	 * 이후 순서 보장은 런타임 PM 코어가 링크를 보고 처리한다.
	 * 값 범위: 도메인이 하나뿐인 장치 트리에서는 NULL 이고 그때는 커널이
	 * 이미 알아서 붙여 준 상태다.
	 */
	struct device		*pd_pcie;
	/* power domain for pcie phy */
	/* [한국어]
	 * struct device *pd_pcie_phy;
	 * PCIe PHY 가 속한 전원 도메인의 가상 장치.
	 * 설정자/읽는 자는 pd_pcie 와 같다. 컨트롤러와 PHY 가 서로 다른 도메인에
	 * 있는 세대에서 둘을 각각 켜기 위해 필요하다.
	 */
	struct device		*pd_pcie_phy;
	/* [한국어]
	 * struct phy *phy;
	 * generic PHY 드라이버 핸들.
	 * 설정자: imx_pcie_probe() 가 IMX_PCIE_FLAG_HAS_PHYDRV 세대에서만
	 * "pcie-phy" 라는 이름으로 얻는다.
	 * 읽는 자: imx_pcie_host_init() 이 phy_init -> phy_set_mode_ext ->
	 * phy_power_on 순으로 켜고, host_exit 가 끈다. LTSSM 을 켜고 끌 때
	 * phy_set_speed() 로 목표 속도도 알린다.
	 * 값 범위: NULL 가능. **NULL 인 세대는 자체 PHY(IMX_PHY)를 이 파일이
	 * 직접 다루는 초기 세대**다. 다만 phy_set_speed(NULL, ...) 은 LTSSM
	 * 제어에서 NULL 검사 없이 불린다.
	 */
	struct phy		*phy;
	/* [한국어]
	 * const struct imx_pcie_drvdata *drvdata;
	 * **이 인스턴스가 어느 세대인지를 가리키는 표 항목.**
	 * 설정자: imx_pcie_probe() 의 of_device_get_match_data(dev) 한 줄.
	 * 읽는 자: 이 파일 거의 전부. imx_check_flag() 매크로도 이 포인터를
	 * 거쳐 flags 를 본다.
	 * 값 범위: drvdata[] 배열의 원소 하나를 가리키며 const 라 바뀌지 않는다.
	 * 동기화: 읽기 전용이라 락이 없다.
	 */
	const struct imx_pcie_drvdata *drvdata;

	/* Ensure that only one device's LUT is configured at any given time */
	/* [한국어]
	 * struct mutex lock;
	 * LUT 접근을 직렬화하는 뮤텍스. 바로 위 상류 주석이 목적을 밝힌다 —
	 * 어느 순간에도 한 장치의 LUT 만 설정되도록 하기 위해서다.
	 * 설정자: imx_pcie_probe() 의 mutex_init().
	 * 읽는 자: imx_pcie_add_lut() 과 imx_pcie_remove_lut() 이 guard(mutex) 로
	 * 함수 전체를 잠근다.
	 * 왜 필요한가: LUT 접근은 ACSCTRL 에 인덱스를 쓴 뒤 DATA1/DATA2 를
	 * 주고받는 두 단계 동작이라, 그 사이에 다른 흐름이 끼어들면 엉뚱한 칸을
	 * 읽거나 쓰게 된다. 컨트롤러가 둘이어도 표는 하나다.
	 * [관찰] imx_pcie_lut_save()/restore() 는 이 뮤텍스를 잡지 않는다.
	 * noirq 단계라 경쟁이 없기 때문으로 보이나 코드에 근거가 적혀 있지는 않다.
	 */
	struct mutex		lock;
};

/* Parameters for the waiting for PCIe PHY PLL to lock on i.MX7 */
#define PHY_PLL_LOCK_WAIT_USLEEP_MAX	200
#define PHY_PLL_LOCK_WAIT_TIMEOUT	(2000 * PHY_PLL_LOCK_WAIT_USLEEP_MAX)

/* PCIe Port Logic registers (memory-mapped) */
#define PL_OFFSET 0x700

#define PCIE_PHY_CTRL (PL_OFFSET + 0x114)
#define PCIE_PHY_CTRL_DATA(x)		FIELD_PREP(GENMASK(15, 0), (x))
#define PCIE_PHY_CTRL_CAP_ADR		BIT(16)
#define PCIE_PHY_CTRL_CAP_DAT		BIT(17)
#define PCIE_PHY_CTRL_WR		BIT(18)
#define PCIE_PHY_CTRL_RD		BIT(19)

#define PCIE_PHY_STAT (PL_OFFSET + 0x110)
#define PCIE_PHY_STAT_ACK		BIT(16)

/* PHY registers (not memory-mapped) */
#define PCIE_PHY_ATEOVRD			0x10
#define  PCIE_PHY_ATEOVRD_EN			BIT(2)
#define  PCIE_PHY_ATEOVRD_REF_CLKDIV_SHIFT	0
#define  PCIE_PHY_ATEOVRD_REF_CLKDIV_MASK	0x1

#define PCIE_PHY_MPLL_OVRD_IN_LO		0x11
#define  PCIE_PHY_MPLL_MULTIPLIER_SHIFT		2
#define  PCIE_PHY_MPLL_MULTIPLIER_MASK		0x7f
#define  PCIE_PHY_MPLL_MULTIPLIER_OVRD		BIT(9)

#define PCIE_PHY_RX_ASIC_OUT 0x100D
#define PCIE_PHY_RX_ASIC_OUT_VALID	(1 << 0)

/* iMX7 PCIe PHY registers */
#define PCIE_PHY_CMN_REG4		0x14
/* These are probably the bits that *aren't* DCC_FB_EN */
#define PCIE_PHY_CMN_REG4_DCC_FB_EN	0x29

#define PCIE_PHY_CMN_REG15	        0x54
#define PCIE_PHY_CMN_REG15_DLY_4	BIT(2)
#define PCIE_PHY_CMN_REG15_PLL_PD	BIT(5)
#define PCIE_PHY_CMN_REG15_OVRD_PLL_PD	BIT(7)

#define PCIE_PHY_CMN_REG24		0x90
#define PCIE_PHY_CMN_REG24_RX_EQ	BIT(6)
#define PCIE_PHY_CMN_REG24_RX_EQ_SEL	BIT(3)

#define PCIE_PHY_CMN_REG26		0x98
#define PCIE_PHY_CMN_REG26_ATT_MODE	0xBC

#define PHY_RX_OVRD_IN_LO 0x1005
#define PHY_RX_OVRD_IN_LO_RX_DATA_EN		BIT(5)
#define PHY_RX_OVRD_IN_LO_RX_PLL_EN		BIT(3)

/* [한국어]
 * imx_pcie_grp_offset - 8M 계열에서 이 컨트롤러가 쓰는 GPR 레지스터 번호를 고른다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @return: IOMUXC_GPR14 또는 IOMUXC_GPR16.
 *
 * i.MX8MQ/8MM/8MP 는 PCIe 컨트롤러가 둘일 수 있고, PHY 관련 비트가 컨트롤러
 * 마다 다른 GPR 레지스터에 놓여 있다. 0번은 GPR14, 1번은 GPR16 이다.
 * 그래서 이 계산을 한 곳에 모아 두고 REF_USE_PAD, VREG_BYPASS,
 * CLK_REQ_OVERRIDE 같은 비트를 만질 때마다 부른다.
 *
 * 맨 앞의 WARN_ON 은 이 함수를 8M 계열이 아닌 세대에서 부르면 알린다 —
 * 다른 세대에는 GPR14/GPR16 에 같은 뜻의 비트가 없기 때문이다. 값을
 * 돌려주기는 하므로 치명적이지는 않지만, 그 값은 의미가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. probe 와 서스펜드·리줌 경로에서 불린다.
 *
 * 호출 체인:
 *   imx8mq_pcie_init_phy() / imx8mm_pcie_clkreq_override() -> [이 함수]
 */
static unsigned int imx_pcie_grp_offset(const struct imx_pcie *imx_pcie)
{
	WARN_ON(imx_pcie->drvdata->variant != IMX8MQ &&	/* [한국어] **8M 계열이 아닌 세대에서 부르면 알린다** — 다른 세대에는 GPR14/GPR16 에 같은 뜻의 비트가 없기 때문이다 */
		imx_pcie->drvdata->variant != IMX8MQ_EP &&	/* [한국어] 8MQ 의 엔드포인트 판도 같은 계열이다 */
		imx_pcie->drvdata->variant != IMX8MM &&	/* [한국어] 8MM */
		imx_pcie->drvdata->variant != IMX8MM_EP &&	/* [한국어] 8MM 의 엔드포인트 판 */
		imx_pcie->drvdata->variant != IMX8MP &&	/* [한국어] 8MP */
		imx_pcie->drvdata->variant != IMX8MP_EP);	/* [한국어] 8MP 의 엔드포인트 판. 여섯 중 어느 것도 아니면 경고가 찍힌다 */
	return imx_pcie->controller_id == 1 ? IOMUXC_GPR16 : IOMUXC_GPR14;	/* [한국어] **컨트롤러 1번은 GPR16, 0번은 GPR14** 를 쓴다. 그 번호는 probe 가 linux,pci-domain 에서 읽어 둔 값이다 */
}

/* [한국어]
 * imx95_pcie_init_phy - i.MX95 의 PHY 를 초기화한다(errata 우회 포함)
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @return: 항상 0. 실패 경로가 없다.
 *
 * drvdata[IMX95].init_phy 로 등록되어 imx_pcie_host_init() 이 부른다.
 * regmap 쓰기 넷이 전부이며 각각 뜻이 다르다.
 *
 *   1. **ERR051624 우회** — 바로 아래 상류 주석이 밝힌다. 보조 전원이 없는
 *      컨트롤러는 beacon 이나 PERST# 해제로 L23 Ready 에서 빠져나오지
 *      못하므로, SS_RW_REG_1[SYS_AUX_PWR_DET] 를 1 로 세워 보조 전원이
 *      있는 것처럼 보이게 한다.
 *   2. PHY 제어 레지스터를 병렬 인터페이스로 접근하도록
 *      SS_RW_REG_0[PHY_CR_PARA_SEL] 을 세운다.
 *   3~4. **레퍼런스 클럭 경로를 고른다.** 장치 트리가 외부 클럭을
 *      쓰라고 했으면(enable_ext_refclk) PHY_GEN_CTRL[REF_USE_PAD] 를 세우고
 *      내부 클럭 공급(SS_RW_REG_0[REF_CLKEN])은 끈다. 아니면 반대로 한다.
 *      둘이 서로 배타적이라 삼항 연산자가 엇갈려 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe/resume). iomuxc_gpr regmap 을 통해
 * syscon 블록에 쓴다.
 *
 * 호출 체인:
 *   imx_pcie_host_init() -> drvdata->init_phy -> [이 함수] -> regmap_*_bits()
 */
static int imx95_pcie_init_phy(struct imx_pcie *imx_pcie)
{
	bool ext = imx_pcie->enable_ext_refclk;	/* [한국어] 장치 트리가 외부 레퍼런스 클럭을 쓰라고 했는지. probe 가 클럭 이름 "extref" 로 판별해 둔 값이다 */

	/*
	 * ERR051624: The Controller Without Vaux Cannot Exit L23 Ready
	 * Through Beacon or PERST# De-assertion
	 *
	 * When the auxiliary power is not available, the controller
	 * cannot exit from L23 Ready with beacon or PERST# de-assertion
	 * when main power is not removed.
	 *
	 * Workaround: Set SS_RW_REG_1[SYS_AUX_PWR_DET] to 1.
	 */
	regmap_set_bits(imx_pcie->iomuxc_gpr, IMX95_PCIE_SS_RW_REG_1,	/* [한국어] **ERR051624 우회** — 바로 위 상류 주석이 인용한다. 보조 전원이 없으면 컨트롤러가 beacon 이나 PERST# 해제로 L23 Ready 에서 못 빠져나오므로, 보조 전원이 감지된 것처럼 보이게 한다 */
			IMX95_PCIE_SYS_AUX_PWR_DET);	/* [한국어] 세울 비트 */

	regmap_update_bits(imx_pcie->iomuxc_gpr,	/* [한국어] PHY 제어 레지스터 접근 방식을 정한다 */
			IMX95_PCIE_SS_RW_REG_0,	/* [한국어] 서브시스템 읽기·쓰기 레지스터 0 */
			IMX95_PCIE_PHY_CR_PARA_SEL,	/* [한국어] 병렬 인터페이스 선택 비트를 */
			IMX95_PCIE_PHY_CR_PARA_SEL);	/* [한국어] 세운다 */

	regmap_update_bits(imx_pcie->iomuxc_gpr, IMX95_PCIE_PHY_GEN_CTRL,	/* [한국어] **레퍼런스 클럭 경로 그 하나** — 외부 패드에서 받을지 */
			   IMX95_PCIE_REF_USE_PAD,	/* [한국어] 외부 패드 사용 비트를 */
			   ext ? IMX95_PCIE_REF_USE_PAD : 0);	/* [한국어] 외부 클럭이면 세우고 아니면 지운다 */
	regmap_update_bits(imx_pcie->iomuxc_gpr, IMX95_PCIE_SS_RW_REG_0,	/* [한국어] **레퍼런스 클럭 경로 그 둘** — 내부 클럭 공급을 켤지 */
			   IMX95_PCIE_REF_CLKEN,	/* [한국어] 내부 클럭 활성화 비트를 */
			   ext ? 0 : IMX95_PCIE_REF_CLKEN);	/* [한국어] 외부 클럭이면 **지우고** 아니면 세운다. 위 삼항 연산자와 방향이 반대인 이유는 두 경로가 서로 배타적이기 때문이다 */

	return 0;	/* [한국어] 실패 경로가 없어 늘 성공이다 */
}

/* [한국어]
 * imx_pcie_configure_type - 컨트롤러를 루트 포트로 쓸지 엔드포인트로 쓸지 GPR 에 적는다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 *
 * 같은 하드웨어가 RC 로도 EP 로도 동작하므로, 링크를 세우기 전에 어느
 * 쪽인지 알려야 한다. 그 값은 PCIe 규격의 device/port type 값
 * (PCI_EXP_TYPE_ROOT_PORT 또는 PCI_EXP_TYPE_ENDPOINT)이고, 넣을 자리는
 * 세대마다 다른 GPR 레지스터의 다른 필드다.
 *
 * 갈래가 셋이다.
 *   - mode_mask[0] 이 0 이면 이 세대는 GPR 로 모드를 정하지 않는다.
 *     바로 위 상류 주석대로 generic PHY 드라이버가 대신하므로 그냥 나간다.
 *   - mode_mask[id] 가 0 이면 두 컨트롤러가 GPR 을 공유한다는 뜻이라
 *     상류 주석대로 id 를 0 으로 접는다.
 *   - 그 밖에는 id 번째 좌표를 그대로 쓴다.
 *
 * 값을 만들 때 ffs(mask) - 1 로 마스크의 최하위 세워진 비트 위치를 구해
 * 그만큼 왼쪽으로 민다 — 마스크가 세대마다 다른 자리에 있어 시프트 폭을
 * 상수로 둘 수 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. imx_pcie_host_init() 이 리셋을 걸고
 * PHY 를 초기화한 직후 부른다.
 *
 * 호출 체인:  imx_pcie_host_init() -> [이 함수] -> regmap_update_bits()
 */
static void imx_pcie_configure_type(struct imx_pcie *imx_pcie)
{
	const struct imx_pcie_drvdata *drvdata = imx_pcie->drvdata;	/* [한국어] 세대별 표 항목을 지역 변수로 꺼내 둔다 */
	unsigned int mask, val, mode, id;	/* [한국어] mask 는 모드 필드의 마스크, val 은 그 자리에 넣을 값, mode 는 PCIe 규격의 포트 종류 값, id 는 컨트롤러 번호 */

	if (drvdata->mode == DW_PCIE_EP_TYPE)	/* [한국어] 표가 이 항목을 엔드포인트로 지정했으면 */
		mode = PCI_EXP_TYPE_ENDPOINT;	/* [한국어] 규격의 엔드포인트 종류 값을 쓰고 */
	else
		mode = PCI_EXP_TYPE_ROOT_PORT;	/* [한국어] 루트 포트 종류 값을 쓴다 */

	id = imx_pcie->controller_id;	/* [한국어] 컨트롤러 번호를 첨자로 삼는다 */

	/* If mode_mask is 0, generic PHY driver is used to set the mode */
	if (!drvdata->mode_mask[0])	/* [한국어] **0번 좌표가 비어 있으면 이 세대는 GPR 로 모드를 정하지 않는다** — 바로 위 상류 주석대로 generic PHY 드라이버가 대신한다 */
		return;	/* [한국어] 그대로 나간다 */

	/* If mode_mask[id] is 0, each controller has its individual GPR */
	if (!drvdata->mode_mask[id])	/* [한국어] **id 번 좌표가 비어 있으면** 바로 위 상류 주석대로 두 컨트롤러가 GPR 을 공유한다는 뜻이므로 */
		id = 0;	/* [한국어] 0번 좌표로 접는다 */

	mask = drvdata->mode_mask[id];	/* [한국어] 쓸 필드의 마스크를 꺼내고 */
	val = mode << (ffs(mask) - 1);	/* [한국어] **ffs(mask) - 1 로 최하위 세워진 비트 위치를 구해** 그만큼 왼쪽으로 민다. 마스크가 세대마다 다른 자리에 있어 시프트 폭을 상수로 둘 수 없다 */

	regmap_update_bits(imx_pcie->iomuxc_gpr, drvdata->mode_off[id], mask, val);	/* [한국어] 해당 GPR 레지스터의 그 필드만 바꾼다 */
}

/* [한국어]
 * pcie_phy_poll_ack - i.MX6 자체 PHY 의 ack 비트가 원하는 값이 될 때까지 기다린다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @exp_val:  기다리는 ack 값. true 면 올라가기를, false 면 내려가기를 기다린다.
 * @return: 0 이면 그 값이 되었다. 10회 안에 안 되면 -ETIMEDOUT.
 *
 * i.MX6 자체 PHY 는 메모리 맵이 아니라 PCIE_PHY_CTRL/PCIE_PHY_STAT 두
 * 레지스터를 통한 핸드셰이크로 접근한다. 이 함수는 그 핸드셰이크의 매
 * 단계마다 상대가 응답했는지 확인하는 자리다.
 *
 * 1us 간격으로 최대 10회 본다. 상한이 상수로 박혀 있어 그보다 느린 PHY 는
 * 지원하지 못한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. udelay 로 바쁜 대기를 하므로 최악에도
 * 10us 다.
 *
 * 호출 체인:
 *   pcie_phy_wait_ack() / pcie_phy_read() / pcie_phy_write()
 *     -> [이 함수] -> dw_pcie_readl_dbi()
 */
static int pcie_phy_poll_ack(struct imx_pcie *imx_pcie, bool exp_val)
{
	struct dw_pcie *pci = imx_pcie->pci;	/* [한국어] dbi 접근에 쓸 DWC 장치 구조체 */
	bool val;	/* [한국어] 읽어 낸 ack 비트 상태 */
	u32 max_iterations = 10;	/* [한국어] 최대 반복 횟수. 상수로 박혀 있어 이보다 느린 PHY 는 지원하지 못한다 */
	u32 wait_counter = 0;	/* [한국어] 지금까지 돈 횟수 */

	do {	/* [한국어] 적어도 한 번은 읽어 본다 */
		val = dw_pcie_readl_dbi(pci, PCIE_PHY_STAT) &	/* [한국어] **PHY 상태 레지스터를 읽어** */
			PCIE_PHY_STAT_ACK;	/* [한국어] ack 비트만 뗀다 */
		wait_counter++;	/* [한국어] 횟수를 센다 */

		if (val == exp_val)	/* [한국어] 기다리던 값이 되었으면 */
			return 0;	/* [한국어] 성공으로 돌아간다 */

		udelay(1);	/* [한국어] 1us 쉬고 다시 본다. 바쁜 대기라 최악에도 10us 다 */
	} while (wait_counter < max_iterations);	/* [한국어] 상한까지 반복한다 */

	return -ETIMEDOUT;	/* [한국어] 상한 안에 응답이 없으면 시간 초과다 */
}

/* [한국어]
 * pcie_phy_wait_ack - PHY 접근의 첫 단계인 "주소 걸기" 를 수행한다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @addr: 접근할 PHY 레지스터 주소(16비트).
 * @return: 0 이면 성공, 상대가 응답하지 않으면 -ETIMEDOUT.
 *
 * 읽기와 쓰기가 공통으로 먼저 하는 일이다. 절차가 넷이다.
 *   1. CTRL 의 데이터 필드에 주소를 실어 쓴다.
 *   2. CAP_ADR 비트를 얹어 "이 값이 주소다" 라고 알린다.
 *   3. ack 가 올라오기를 기다린다.
 *   4. CAP_ADR 를 내리고 ack 가 내려가기를 기다린다.
 * 스트로브를 올렸다 내리고 그때마다 ack 를 확인하는 것이 이 PHY
 * 인터페이스의 기본 규약이며, 아래 read/write 도 같은 모양을 반복한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pcie_phy_read() / pcie_phy_write() -> [이 함수]
 *     -> dw_pcie_writel_dbi() -> pcie_phy_poll_ack()
 */
static int pcie_phy_wait_ack(struct imx_pcie *imx_pcie, int addr)
{
	struct dw_pcie *pci = imx_pcie->pci;	/* [한국어] dbi 접근에 쓸 DWC 장치 구조체 */
	u32 val;	/* [한국어] CTRL 레지스터에 쓸 값 */
	int ret;	/* [한국어] 하위 호출 결과 */

	val = PCIE_PHY_CTRL_DATA(addr);	/* [한국어] **주소를 CTRL 의 데이터 필드에 싣는다** */
	dw_pcie_writel_dbi(pci, PCIE_PHY_CTRL, val);	/* [한국어] 그대로 쓴다. 아직 스트로브는 없다 */

	val |= PCIE_PHY_CTRL_CAP_ADR;	/* [한국어] **주소 캡처 스트로브를 얹는다** — 이 값이 주소라고 알리는 것이다 */
	dw_pcie_writel_dbi(pci, PCIE_PHY_CTRL, val);	/* [한국어] 스트로브와 함께 다시 쓴다 */

	ret = pcie_phy_poll_ack(imx_pcie, true);	/* [한국어] 상대가 ack 를 올릴 때까지 기다린다 */
	if (ret)	/* [한국어] 응답이 없으면 */
		return ret;	/* [한국어] 그 코드를 그대로 돌려준다 */

	val = PCIE_PHY_CTRL_DATA(addr);	/* [한국어] **스트로브를 뺀 값으로 되돌린다** — 캡처를 내리는 것이다 */
	dw_pcie_writel_dbi(pci, PCIE_PHY_CTRL, val);	/* [한국어] 그대로 쓴다 */

	return pcie_phy_poll_ack(imx_pcie, false);	/* [한국어] ack 가 내려가기를 기다린 결과를 그대로 돌려준다. 이 올렸다 내리기와 ack 확인이 이 PHY 인터페이스의 기본 규약이다 */
}

/* Read from the 16-bit PCIe PHY control registers (not memory-mapped) */
/* [한국어]
 * pcie_phy_read - 메모리 맵이 아닌 i.MX6 PHY 레지스터에서 16비트를 읽는다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @addr: 읽을 PHY 레지스터 주소.
 * @data: 읽은 값을 담을 자리.
 * @return: 0 이면 성공, 어느 단계든 응답이 없으면 -ETIMEDOUT.
 *
 * 바로 위 상류 주석이 밝히듯 이 PHY 의 16비트 제어 레지스터는 메모리에
 * 매핑되어 있지 않다. 그래서 dbi 창의 두 레지스터를 통한 핸드셰이크로
 * 읽는다.
 *
 * 절차는 주소 걸기(pcie_phy_wait_ack) -> RD 스트로브 올리기 -> ack 대기 ->
 * **STAT 레지스터에서 값 읽기** -> CTRL 을 0 으로 지워 스트로브 내리기 ->
 * ack 내려가기 대기다. 값이 CTRL 이 아니라 STAT 에서 나온다는 점이
 * 쓰기와 다르다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx_setup_phy_mpll() / imx_pcie_reset_phy() -> [이 함수]
 *     -> pcie_phy_wait_ack() -> pcie_phy_poll_ack()
 */
static int pcie_phy_read(struct imx_pcie *imx_pcie, int addr, u16 *data)
{
	struct dw_pcie *pci = imx_pcie->pci;	/* [한국어] dbi 접근에 쓸 DWC 장치 구조체 */
	u32 phy_ctl;	/* [한국어] CTRL 레지스터에 쓸 값 */
	int ret;	/* [한국어] 하위 호출 결과 */

	ret = pcie_phy_wait_ack(imx_pcie, addr);	/* [한국어] **먼저 주소를 건다** */
	if (ret)	/* [한국어] 실패하면 */
		return ret;	/* [한국어] 그 코드를 그대로 돌려준다 */

	/* assert Read signal */
	phy_ctl = PCIE_PHY_CTRL_RD;	/* [한국어] **읽기 스트로브만 세운 값을 만든다** — 데이터 필드는 0 이다 */
	dw_pcie_writel_dbi(pci, PCIE_PHY_CTRL, phy_ctl);	/* [한국어] 그대로 써서 읽기를 시작시킨다 */

	ret = pcie_phy_poll_ack(imx_pcie, true);	/* [한국어] 상대가 ack 를 올릴 때까지 기다린다 */
	if (ret)	/* [한국어] 응답이 없으면 */
		return ret;	/* [한국어] 그 코드를 그대로 돌려준다 */

	*data = dw_pcie_readl_dbi(pci, PCIE_PHY_STAT);	/* [한국어] **값은 CTRL 이 아니라 STAT 레지스터에서 나온다.** 쓰기와 다른 점이다 */

	/* deassert Read signal */
	dw_pcie_writel_dbi(pci, PCIE_PHY_CTRL, 0x00);	/* [한국어] CTRL 을 0 으로 지워 읽기 스트로브를 내린다 */

	return pcie_phy_poll_ack(imx_pcie, false);	/* [한국어] ack 가 내려가기를 기다린 결과를 그대로 돌려준다 */
}

/* [한국어]
 * pcie_phy_write - 메모리 맵이 아닌 i.MX6 PHY 레지스터에 16비트를 쓴다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @addr: 쓸 PHY 레지스터 주소.
 * @data: 쓸 값.
 * @return: 0 이면 성공, 어느 단계든 응답이 없으면 -ETIMEDOUT.
 *
 * 읽기보다 단계가 많다. **데이터를 걸고 확정한 뒤, 다시 쓰기 스트로브를
 * 따로 올려야** 하기 때문이다.
 *   1. 주소 걸기(pcie_phy_wait_ack).
 *   2. CTRL 데이터 필드에 쓸 값을 싣는다.
 *   3. CAP_DAT 를 얹어 "이 값이 데이터다" 라고 알리고 ack 를 기다린다.
 *   4. CAP_DAT 를 내리고 ack 가 내려가기를 기다린다.
 *   5. WR 스트로브를 올리고 ack 를 기다린다.
 *   6. WR 를 내리고 ack 가 내려가기를 기다린다.
 *   7. CTRL 을 0 으로 지워 마무리한다.
 * 각 단계의 상류 영어 주석이 그 순서를 그대로 적어 두었다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx_setup_phy_mpll() / imx_pcie_reset_phy() -> [이 함수]
 *     -> pcie_phy_wait_ack() -> pcie_phy_poll_ack()
 */
static int pcie_phy_write(struct imx_pcie *imx_pcie, int addr, u16 data)
{
	struct dw_pcie *pci = imx_pcie->pci;	/* [한국어] dbi 접근에 쓸 DWC 장치 구조체 */
	u32 var;	/* [한국어] CTRL 레지스터에 쓸 값 */
	int ret;	/* [한국어] 하위 호출 결과 */

	/* write addr */
	/* cap addr */
	ret = pcie_phy_wait_ack(imx_pcie, addr);	/* [한국어] **1단계 주소 걸기.** 바로 위 두 줄의 상류 주석이 그 뜻을 적어 두었다 */
	if (ret)	/* [한국어] 실패하면 */
		return ret;	/* [한국어] 그 코드를 그대로 돌려준다 */

	var = PCIE_PHY_CTRL_DATA(data);	/* [한국어] **2단계** 쓸 값을 CTRL 의 데이터 필드에 싣는다 */
	dw_pcie_writel_dbi(pci, PCIE_PHY_CTRL, var);	/* [한국어] 스트로브 없이 먼저 쓴다 */

	/* capture data */
	var |= PCIE_PHY_CTRL_CAP_DAT;	/* [한국어] **3단계** 데이터 캡처 스트로브를 얹는다 */
	dw_pcie_writel_dbi(pci, PCIE_PHY_CTRL, var);	/* [한국어] 스트로브와 함께 다시 쓴다 */

	ret = pcie_phy_poll_ack(imx_pcie, true);	/* [한국어] ack 가 올라오기를 기다린다 */
	if (ret)	/* [한국어] 응답이 없으면 */
		return ret;	/* [한국어] 그 코드를 그대로 돌려준다 */

	/* deassert cap data */
	var = PCIE_PHY_CTRL_DATA(data);	/* [한국어] **4단계** 스트로브를 뺀 값으로 되돌려 캡처를 내린다 */
	dw_pcie_writel_dbi(pci, PCIE_PHY_CTRL, var);	/* [한국어] 그대로 쓴다 */

	/* wait for ack de-assertion */
	ret = pcie_phy_poll_ack(imx_pcie, false);	/* [한국어] ack 가 내려가기를 기다린다 */
	if (ret)	/* [한국어] 응답이 없으면 */
		return ret;	/* [한국어] 그 코드를 그대로 돌려준다 */

	/* assert wr signal */
	var = PCIE_PHY_CTRL_WR;	/* [한국어] **5단계 쓰기 스트로브를 따로 올린다.** 데이터 캡처와 실제 쓰기가 나뉘어 있는 것이 읽기와 다른 점이다 */
	dw_pcie_writel_dbi(pci, PCIE_PHY_CTRL, var);	/* [한국어] 데이터 필드 없이 스트로브만 쓴다 */

	/* wait for ack */
	ret = pcie_phy_poll_ack(imx_pcie, true);	/* [한국어] ack 가 올라오기를 기다린다 */
	if (ret)	/* [한국어] 응답이 없으면 */
		return ret;	/* [한국어] 그 코드를 그대로 돌려준다 */

	/* deassert wr signal */
	var = PCIE_PHY_CTRL_DATA(data);	/* [한국어] **6단계** 다시 데이터 값으로 되돌려 쓰기 스트로브를 내린다 */
	dw_pcie_writel_dbi(pci, PCIE_PHY_CTRL, var);	/* [한국어] 그대로 쓴다 */

	/* wait for ack de-assertion */
	ret = pcie_phy_poll_ack(imx_pcie, false);	/* [한국어] ack 가 내려가기를 기다린다 */
	if (ret)	/* [한국어] 응답이 없으면 */
		return ret;	/* [한국어] 그 코드를 그대로 돌려준다 */

	dw_pcie_writel_dbi(pci, PCIE_PHY_CTRL, 0x0);	/* [한국어] **7단계** CTRL 을 0 으로 지워 마무리한다 */

	return 0;	/* [한국어] 일곱 단계를 모두 지났으면 성공 */
}

/* [한국어]
 * imx8mq_pcie_init_phy - i.MX8M 계열의 PHY 레퍼런스 클럭과 전원 설정을 정한다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @return: 항상 0.
 *
 * drvdata 의 8MQ/8MM/8MP(및 각 EP 판) 항목에서 init_phy 로 등록된다.
 * 하는 일이 둘이다.
 *
 *   1. REF_USE_PAD 를 세워 **외부 오실레이터를 레퍼런스 클럭으로 쓰게**
 *      한다. 바로 위 상류 주석이 이것을 TODO 로 적어 두었다 — 외부
 *      오실레이터를 쓴다고 가정한 코드라는 뜻이다.
 *   2. **PCIE_VPH 전압에 따라 VREG_BYPASS 를 조정한다.** 상류 주석이
 *      데이터시트를 인용해 밝힌다 — VPH 는 1.8V 를 권장하며, 3.3V 로
 *      공급되면 VREG_BYPASS 를 0 으로 지워야 한다. 그래서 레귤레이터가
 *      있고 실제 전압이 3V 를 넘을 때만 그 비트를 지운다.
 *
 * 두 쓰기 모두 imx_pcie_grp_offset() 이 고른 GPR14/GPR16 에 들어간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe/resume).
 *
 * 호출 체인:
 *   imx_pcie_host_init() -> drvdata->init_phy -> [이 함수]
 *     -> imx_pcie_grp_offset() -> regulator_get_voltage()
 */
static int imx8mq_pcie_init_phy(struct imx_pcie *imx_pcie)
{
	/* TODO: This code assumes external oscillator is being used */
	regmap_update_bits(imx_pcie->iomuxc_gpr,	/* [한국어] **외부 오실레이터를 레퍼런스 클럭으로 쓰게 한다.** 바로 위 상류 주석이 이것을 TODO 로 적어 두었다 — 외부 오실레이터를 쓴다고 가정한 코드다 */
			   imx_pcie_grp_offset(imx_pcie),	/* [한국어] 컨트롤러 번호에 따라 GPR14 또는 GPR16 을 고른다 */
			   IMX8MQ_GPR_PCIE_REF_USE_PAD,	/* [한국어] 외부 패드 사용 비트를 */
			   IMX8MQ_GPR_PCIE_REF_USE_PAD);	/* [한국어] 세운다 */
	/*
	 * Per the datasheet, the PCIE_VPH is suggested to be 1.8V.  If the
	 * PCIE_VPH is supplied by 3.3V, the VREG_BYPASS should be cleared
	 * to zero.
	 */
	if (imx_pcie->vph && regulator_get_voltage(imx_pcie->vph) > 3000000)	/* [한국어] **레귤레이터가 있고 실제 전압이 3V 를 넘으면** — 바로 위 상류 주석이 데이터시트를 인용한다. VPH 는 1.8V 를 권장하며 3.3V 로 공급되면 VREG_BYPASS 를 0 으로 지워야 한다 */
		regmap_update_bits(imx_pcie->iomuxc_gpr,	/* [한국어] 같은 GPR 에 */
				   imx_pcie_grp_offset(imx_pcie),	/* [한국어] 같은 방식으로 레지스터를 고르고 */
				   IMX8MQ_GPR_PCIE_VREG_BYPASS,	/* [한국어] PHY 내부 레귤레이터 우회 비트를 */
				   0);	/* [한국어] 지운다 */

	return 0;	/* [한국어] 실패 경로가 없어 늘 성공이다 */
}

/* [한국어]
 * imx_pcie_init_phy - i.MX6Q/6QP 자체 PHY 의 전기적 파라미터를 GPR 에 적는다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @return: 항상 0.
 *
 * 이 세대의 PHY 는 송신 파형과 수신 감도를 GPR8/GPR12 의 필드로 조정한다.
 * 그 값들은 보드마다 다를 수 있어 장치 트리에서 읽어 imx_pcie 에 담아 두고
 * (fsl,tx-deemph-gen1 등), 여기서 레지스터에 옮긴다.
 *
 * 쓰는 항목은 여섯이다.
 *   - GPR12[PCIE_CTL_2] 를 0 으로 — LTSSM 이 꺼진 상태에서 시작하게 한다.
 *   - GPR12[LOS_LEVEL] 에 9 — 상류 주석대로 컨트롤러와 PHY 에 넣는 고정
 *     입력 신호로, 수신 신호 상실(Loss Of Signal) 판정 문턱이다.
 *   - GPR8 의 네 필드 — Gen1 디엠퍼시스, Gen2 의 3.5dB 와 6dB 디엠퍼시스,
 *     송신 스윙의 full 과 low 값. 시프트 폭(0/6/12/18/25)이 각 필드의
 *     비트 위치다.
 *
 * 6SX 는 이 함수를 그대로 부르기 전에 RX_EQ 만 따로 정한다
 * (imx6sx_pcie_init_phy).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe/resume).
 *
 * 호출 체인:
 *   imx_pcie_host_init() -> drvdata->init_phy -> [이 함수]
 *   imx6sx_pcie_init_phy() -> [이 함수]
 */
static int imx_pcie_init_phy(struct imx_pcie *imx_pcie)
{
	regmap_update_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR12,	/* [한국어] **LTSSM 이 꺼진 상태에서 시작하게 한다** — 이후 리셋과 PHY 설정을 안전하게 하기 위해서다 */
				   IMX6Q_GPR12_PCIE_CTL_2, 0 << 10);	/* [한국어] CTL_2 비트를 0 으로 쓴다 */

	/* configure constant input signal to the pcie ctrl and phy */
	regmap_update_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR12,	/* [한국어] **수신 신호 상실 판정 문턱.** 바로 위 상류 주석대로 컨트롤러와 PHY 에 넣는 고정 입력 신호다 */
			   IMX6Q_GPR12_LOS_LEVEL, 9 << 4);	/* [한국어] LOS_LEVEL 필드에 9 를 넣는다 */

	regmap_update_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR8,	/* [한국어] **여기부터 다섯은 송신 파형 파라미터**이며 모두 GPR8 에 들어간다 */
			   IMX6Q_GPR8_TX_DEEMPH_GEN1,	/* [한국어] Gen1 디엠퍼시스 필드 */
			   imx_pcie->tx_deemph_gen1 << 0);	/* [한국어] 장치 트리 값(기본 0)을 시프트 0 자리에 */
	regmap_update_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR8,	/* [한국어] 다음 필드 */
			   IMX6Q_GPR8_TX_DEEMPH_GEN2_3P5DB,	/* [한국어] Gen2 의 3.5dB 디엠퍼시스 필드 */
			   imx_pcie->tx_deemph_gen2_3p5db << 6);	/* [한국어] 장치 트리 값(기본 0)을 시프트 6 자리에 */
	regmap_update_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR8,	/* [한국어] 다음 필드 */
			   IMX6Q_GPR8_TX_DEEMPH_GEN2_6DB,	/* [한국어] Gen2 의 6dB 디엠퍼시스 필드 */
			   imx_pcie->tx_deemph_gen2_6db << 12);	/* [한국어] 장치 트리 값(기본 20)을 시프트 12 자리에 */
	regmap_update_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR8,	/* [한국어] 다음 필드 */
			   IMX6Q_GPR8_TX_SWING_FULL,	/* [한국어] 송신 스윙 full 필드 */
			   imx_pcie->tx_swing_full << 18);	/* [한국어] 장치 트리 값(기본 127)을 시프트 18 자리에 */
	regmap_update_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR8,	/* [한국어] 마지막 필드 */
			   IMX6Q_GPR8_TX_SWING_LOW,	/* [한국어] 송신 스윙 low 필드 */
			   imx_pcie->tx_swing_low << 25);	/* [한국어] 장치 트리 값(기본 127)을 시프트 25 자리에 */
	return 0;	/* [한국어] 다섯 파라미터를 모두 넣었으면 성공 */
}

/* [한국어]
 * imx6sx_pcie_init_phy - i.MX6SX 의 수신 이퀄라이저를 정한 뒤 공통 초기화로 넘긴다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @return: imx_pcie_init_phy() 의 반환값(항상 0).
 *
 * 6SX 만 GPR12 에 RX_EQ 필드가 따로 있어 그 값을 IMX6SX_GPR12_PCIE_RX_EQ_2
 * 로 정한다. 그 밖의 파라미터는 6Q 와 같으므로 imx_pcie_init_phy() 를
 * 그대로 부른다.
 *
 * 이 "한 줄 덧붙이고 공통 함수 호출" 은 drvdata 함수 포인터 방식에서
 * 세대별 차이를 표현하는 전형적인 모양이다 — 표에는 이 함수 하나만 적고,
 * 공통 부분은 코드 재사용으로 처리한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe/resume).
 *
 * 호출 체인:
 *   imx_pcie_host_init() -> drvdata->init_phy -> [이 함수]
 *     -> imx_pcie_init_phy()
 */
static int imx6sx_pcie_init_phy(struct imx_pcie *imx_pcie)
{
	regmap_update_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR12,	/* [한국어] **6SX 에만 있는 수신 이퀄라이저 설정** */
			   IMX6SX_GPR12_PCIE_RX_EQ_MASK, IMX6SX_GPR12_PCIE_RX_EQ_2);	/* [한국어] RX_EQ 필드를 값 2 로 정한다 */

	return imx_pcie_init_phy(imx_pcie);	/* [한국어] 나머지 파라미터는 6Q 와 같으므로 공통 함수에 맡기고 그 결과를 그대로 돌려준다 */
}

/* [한국어]
 * imx7d_pcie_wait_for_phy_pll_lock - i.MX7D 의 PHY PLL 이 잠길 때까지 기다린다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 *
 * PLL 이 잠기지 않은 상태에서 링크를 세우려 하면 실패하므로, 리셋을 푼 뒤
 * GPR22 의 PLL_LOCKED 비트가 설 때까지 기다린다.
 *
 * regmap_read_poll_timeout() 이 200us 간격으로 최대
 * PHY_PLL_LOCK_WAIT_TIMEOUT(2000 * 200us = 0.4초)까지 본다.
 *
 * **반환값이 없다는 점이 아래 i.MX95 판과 다르다.** 시간이 지나도 오류를
 * 남길 뿐 호출자에게 알리지 않으므로, 실패해도 초기화가 계속 진행된다.
 * 같은 일을 하는 imx95_pcie_wait_for_phy_pll_lock() 은 -ETIMEDOUT 을
 * 돌려주고 drvdata 의 wait_pll_lock 으로 등록되어 host_init 이 결과를
 * 확인한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep 으로 잠든다.
 *
 * 호출 체인:
 *   imx7d_pcie_core_reset() -> [이 함수] -> regmap_read_poll_timeout()
 */
static void imx7d_pcie_wait_for_phy_pll_lock(struct imx_pcie *imx_pcie)
{
	u32 val;	/* [한국어] 폴링이 읽은 값을 담을 자리 */
	struct device *dev = imx_pcie->pci->dev;	/* [한국어] 오류 메시지를 낼 장치 */

	if (regmap_read_poll_timeout(imx_pcie->iomuxc_gpr,	/* [한국어] **PLL 이 잠길 때까지 주기적으로 읽는다** */
				     IOMUXC_GPR22, val,	/* [한국어] i.MX7D 는 GPR22 에 잠금 상태가 있다 */
				     val & IMX7D_GPR22_PCIE_PHY_PLL_LOCKED,	/* [한국어] 그 안의 PLL_LOCKED 비트가 서면 끝난다 */
				     PHY_PLL_LOCK_WAIT_USLEEP_MAX,	/* [한국어] 200us 간격으로 */
				     PHY_PLL_LOCK_WAIT_TIMEOUT))	/* [한국어] 0.4초(2000 x 200us)까지 본다. 그 안에 안 되면 참을 돌려준다 */
		dev_err(dev, "PCIe PLL lock timeout\n");	/* [한국어] **시간이 지나도 오류만 남긴다** — 반환값이 없어 호출자에게 알리지 않으므로 초기화가 계속 진행된다 */
}

/* [한국어]
 * imx95_pcie_wait_for_phy_pll_lock - i.MX95 의 MPLL 이 잠길 때까지 기다린다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 잠겼다. 시간이 지나면 -ETIMEDOUT.
 *
 * drvdata[IMX95].wait_pll_lock 으로 등록되어 imx_pcie_host_init() 이
 * 리셋을 풀고 PERST# 를 해제한 직후 부른다. 결과를 확인하므로, 잠기지
 * 않으면 초기화가 그 자리에서 실패한다.
 *
 * 보는 비트는 PHY_MPLLA_CTRL[MPLL_STATE] 이고, 간격과 상한은 i.MX7D 판과
 * 같은 상수를 쓴다(200us 간격, 0.4초 상한).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep 으로 잠든다.
 *
 * 호출 체인:
 *   imx_pcie_host_init() -> drvdata->wait_pll_lock -> [이 함수]
 *     -> regmap_read_poll_timeout()
 */
static int imx95_pcie_wait_for_phy_pll_lock(struct imx_pcie *imx_pcie)
{
	u32 val;	/* [한국어] 폴링이 읽은 값을 담을 자리 */
	struct device *dev = imx_pcie->pci->dev;	/* [한국어] 오류 메시지를 낼 장치 */

	if (regmap_read_poll_timeout(imx_pcie->iomuxc_gpr,	/* [한국어] 같은 방식으로 폴링한다 */
				     IMX95_PCIE_PHY_MPLLA_CTRL, val,	/* [한국어] i.MX95 는 MPLLA 제어 레지스터에 상태가 있다 */
				     val & IMX95_PCIE_PHY_MPLL_STATE,	/* [한국어] 그 안의 MPLL_STATE 비트가 서면 끝난다 */
				     PHY_PLL_LOCK_WAIT_USLEEP_MAX,	/* [한국어] i.MX7D 판과 같은 간격과 */
				     PHY_PLL_LOCK_WAIT_TIMEOUT)) {	/* [한국어] 같은 상한을 쓴다 */
		dev_err(dev, "PCIe PLL lock timeout\n");	/* [한국어] 시간이 지나면 알리고 */
		return -ETIMEDOUT;	/* [한국어] **-ETIMEDOUT 을 돌려준다.** i.MX7D 판과 달리 호출자가 이 값을 확인해 초기화를 접는다 */
	}

	return 0;	/* [한국어] 잠겼으면 성공 */
}

/* [한국어]
 * imx_setup_phy_mpll - PHY 레퍼런스 클럭 주파수에 맞게 MPLL 체배·분주를 조정한다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 성공(조정할 필요가 없는 경우 포함), 지원하지 않는
 *          주파수면 -EINVAL.
 *
 * i.MX6 자체 PHY(IMX_PCIE_FLAG_IMX_PHY)에만 해당한다. 플래그가 없으면
 * 곧바로 0 으로 나간다.
 *
 * PHY 는 내부적으로 2.5GHz 대의 클럭을 만들어야 하는데, 보드가 넣어 주는
 * 레퍼런스 클럭 주파수가 다르면 체배 비율을 바꿔야 한다. 그래서 클럭
 * 묶음에서 이름이 "pcie_phy" 로 시작하는 클럭의 실제 주파수를 물어 세
 * 경우로 가른다.
 *   125MHz — 상류 주석대로 MPLL 의 기본값이 이 주파수 기준이라 손댈 것이 없다.
 *   100MHz — 체배 25, 분주 0.
 *   200MHz — 체배 25, 분주 1(즉 2분주해 100MHz 로 만든 뒤 25배).
 *   그 밖  — 지원하지 않는다고 알리고 -EINVAL.
 *
 * 조정은 PHY 레지스터 둘에 쓴다. MPLL_OVRD_IN_LO 에 체배 값과 오버라이드
 * 비트를, ATEOVRD 에 분주 값과 오버라이드 활성 비트를 넣는다. 두 레지스터
 * 모두 메모리 맵이 아니므로 pcie_phy_read/write 핸드셰이크를 거친다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. imx_pcie_host_init() 의 마지막 단계다.
 *
 * 호출 체인:
 *   imx_pcie_host_init() -> [이 함수] -> clk_get_rate()
 *     -> pcie_phy_read() -> pcie_phy_write()
 */
static int imx_setup_phy_mpll(struct imx_pcie *imx_pcie)
{
	unsigned long phy_rate = 0;	/* [한국어] PHY 레퍼런스 클럭의 실제 주파수. 못 찾으면 0 인 채로 남아 default 갈래로 간다 */
	int mult, div;	/* [한국어] MPLL 체배와 분주 값 */
	u16 val;	/* [한국어] PHY 레지스터를 읽고 고칠 임시 변수 */
	int i;	/* [한국어] 클럭 배열 순회 인덱스 */
	struct clk_bulk_data *clks = imx_pcie->clks;	/* [한국어] 클럭 묶음을 지역 변수로 꺼내 둔다 */

	if (!(imx_pcie->drvdata->flags & IMX_PCIE_FLAG_IMX_PHY))	/* [한국어] **자체 PHY 세대가 아니면** 이 조정이 의미가 없다 */
		return 0;	/* [한국어] 그대로 성공으로 나간다 */

	for (i = 0; i < imx_pcie->num_clks; i++)	/* [한국어] 클럭 묶음을 훑으며 */
		if (strncmp(clks[i].id, "pcie_phy", 8) == 0)	/* [한국어] 이름이 "pcie_phy" 로 시작하는 것을 찾아 */
			phy_rate = clk_get_rate(clks[i].clk);	/* [한국어] 그 실제 주파수를 묻는다 */

	switch (phy_rate) {	/* [한국어] 주파수별로 체배·분주를 정한다 */
	case 125000000:	/* [한국어] 125MHz 이면 */
		/*
		 * The default settings of the MPLL are for a 125MHz input
		 * clock, so no need to reconfigure anything in that case.
		 */
		return 0;	/* [한국어] 바로 위 상류 주석대로 MPLL 의 기본값이 이 주파수 기준이라 손댈 것이 없다 */
	case 100000000:	/* [한국어] 100MHz 이면 */
		mult = 25;	/* [한국어] 25배 체배하고 */
		div = 0;	/* [한국어] 분주는 하지 않는다 */
		break;	/* [한국어] 갈래를 벗어난다 */
	case 200000000:	/* [한국어] 200MHz 이면 */
		mult = 25;	/* [한국어] 같은 25배 체배에 */
		div = 1;	/* [한국어] 2분주를 걸어 100MHz 로 맞춘다 */
		break;	/* [한국어] 갈래를 벗어난다 */
	default:	/* [한국어] 그 밖의 주파수는 */
		dev_err(imx_pcie->pci->dev,	/* [한국어] 지원하지 않는다고 알리고 */
			"Unsupported PHY reference clock rate %lu\n", phy_rate);	/* [한국어] 어떤 주파수였는지 함께 남긴다 */
		return -EINVAL;	/* [한국어] 인자 오류로 돌아간다 */
	}

	pcie_phy_read(imx_pcie, PCIE_PHY_MPLL_OVRD_IN_LO, &val);	/* [한국어] **체배 값을 넣을 PHY 레지스터를 먼저 읽는다** — 다른 비트를 보존해야 하기 때문이다 */
	val &= ~(PCIE_PHY_MPLL_MULTIPLIER_MASK <<	/* [한국어] 기존 체배 필드를 지운다 */
		 PCIE_PHY_MPLL_MULTIPLIER_SHIFT);	/* [한국어] 필드의 비트 위치 */
	val |= mult << PCIE_PHY_MPLL_MULTIPLIER_SHIFT;	/* [한국어] 구한 체배 값을 그 자리에 넣고 */
	val |= PCIE_PHY_MPLL_MULTIPLIER_OVRD;	/* [한국어] **오버라이드 비트를 세워** 하드웨어 기본값 대신 이 값을 쓰게 한다 */
	pcie_phy_write(imx_pcie, PCIE_PHY_MPLL_OVRD_IN_LO, val);	/* [한국어] 고친 값을 되쓴다 */

	pcie_phy_read(imx_pcie, PCIE_PHY_ATEOVRD, &val);	/* [한국어] **분주 값을 넣을 레지스터도 같은 방식으로 읽는다** */
	val &= ~(PCIE_PHY_ATEOVRD_REF_CLKDIV_MASK <<	/* [한국어] 기존 분주 필드를 지운다 */
		 PCIE_PHY_ATEOVRD_REF_CLKDIV_SHIFT);	/* [한국어] 필드의 비트 위치 */
	val |= div << PCIE_PHY_ATEOVRD_REF_CLKDIV_SHIFT;	/* [한국어] 구한 분주 값을 그 자리에 넣고 */
	val |= PCIE_PHY_ATEOVRD_EN;	/* [한국어] 오버라이드를 켜는 비트를 세운다 */
	pcie_phy_write(imx_pcie, PCIE_PHY_ATEOVRD, val);	/* [한국어] 고친 값을 되쓴다 */

	return 0;	/* [한국어] 두 레지스터를 모두 맞췄으면 성공 */
}

/* [한국어]
 * imx_pcie_reset_phy - 링크 세우기가 실패했을 때 PHY 수신단을 흔들어 되살린다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 *
 * i.MX6 자체 PHY 에만 해당한다. 플래그가 없으면 곧바로 나간다.
 *
 * PHY_RX_OVRD_IN_LO 의 RX_DATA_EN 과 RX_PLL_EN 두 비트를 세웠다가
 * 2~3ms 뒤 다시 지운다. 오버라이드 비트를 잠시 걸어 수신단을 강제로
 * 껐다 켜는 셈이다.
 *
 * 부르는 곳은 imx_pcie_start_link() 의 err_reset_phy 경로 하나뿐이다 —
 * 링크가 서지 않았거나 속도 변경이 끝나지 않았을 때 마지막으로 시도해
 * 보는 조치다. 그 경로가 0 을 돌려주므로 이 조치의 성패는 호출자에게
 * 전해지지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep 으로 잠든다.
 *
 * 호출 체인:
 *   imx_pcie_start_link() 실패 경로 -> [이 함수]
 *     -> pcie_phy_read() -> pcie_phy_write()
 */
static void imx_pcie_reset_phy(struct imx_pcie *imx_pcie)
{
	u16 tmp;	/* [한국어] PHY 레지스터를 읽고 고칠 임시 변수 */

	if (!(imx_pcie->drvdata->flags & IMX_PCIE_FLAG_IMX_PHY))	/* [한국어] **자체 PHY 세대가 아니면** 이 조치가 의미가 없다 */
		return;	/* [한국어] 그대로 나간다 */

	pcie_phy_read(imx_pcie, PHY_RX_OVRD_IN_LO, &tmp);	/* [한국어] 수신 오버라이드 레지스터를 읽는다 */
	tmp |= (PHY_RX_OVRD_IN_LO_RX_DATA_EN |	/* [한국어] 수신 데이터 활성화와 */
		PHY_RX_OVRD_IN_LO_RX_PLL_EN);	/* [한국어] 수신 PLL 활성화 비트를 세워 */
	pcie_phy_write(imx_pcie, PHY_RX_OVRD_IN_LO, tmp);	/* [한국어] 오버라이드를 건다 — 수신단을 강제로 켜는 셈이다 */

	usleep_range(2000, 3000);	/* [한국어] 2~3ms 유지한다 */

	pcie_phy_read(imx_pcie, PHY_RX_OVRD_IN_LO, &tmp);	/* [한국어] 같은 레지스터를 다시 읽어 */
	tmp &= ~(PHY_RX_OVRD_IN_LO_RX_DATA_EN |	/* [한국어] 두 비트를 */
		  PHY_RX_OVRD_IN_LO_RX_PLL_EN);	/* [한국어] 지우고 */
	pcie_phy_write(imx_pcie, PHY_RX_OVRD_IN_LO, tmp);	/* [한국어] 되쓴다. 오버라이드를 풀어 수신단을 원래 제어로 되돌리는 것이며, 이 껐다 켜기가 링크 실패 뒤의 마지막 시도다 */
}

#ifdef CONFIG_ARM	/* [한국어] **32비트 ARM 에서만 이 핸들러가 존재한다.** 64비트에는 이런 어보트 훅이 없다 */
/*  Added for PCI abort handling */
/* [한국어]
 * imx6q_pcie_abort_handler - 응답 없는 PCIe 접근이 낸 ARM 데이터 어보트를 삼킨다
 *
 * @addr: 어보트를 낸 주소. 쓰지 않는다.
 * @fsr:  폴트 상태 레지스터. 쓰지 않는다.
 * @regs: 어보트 시점의 레지스터 상태. 여기를 고쳐 예외에서 복귀시킨다.
 * @return: 0 이면 처리했으니 그대로 이어 가라는 뜻, 1 이면 처리하지
 *          못했으니 커널이 원래대로 죽으라는 뜻이다.
 *
 * 바로 위 상류 주석이 목적을 밝힌다 — PCI 어보트 처리를 위해 넣은 것이다.
 * PCIe 장치가 없는 주소를 읽으면 버스가 응답하지 않아 ARM 에서 외부
 * 어보트가 나는데, PCI 열거는 없는 장치를 읽어 보는 것이 정상 동작이므로
 * 그때마다 죽으면 안 된다. 그래서 **읽기였으면 전부 1 을 읽은 것처럼
 * 꾸미고 다음 명령으로 넘어간다.**
 *
 * 명령어를 직접 해독한다. 명령 워드에서 비트 12~15 를 목적 레지스터
 * 번호로 꺼내고, 두 가지 인코딩을 알아본다.
 *   - (instr & 0x0c100000) == 0x04100000 — LDR 계열 적재. 바이트
 *     적재(0x00400000)면 255 를, 아니면 -1(전부 1)을 목적 레지스터에 넣는다.
 *   - (instr & 0x0e100090) == 0x00100090 — 하프워드/부호 확장 적재 계열.
 *     -1 을 넣는다.
 * 어느 쪽이든 PC 를 4 만큼 밀어 그 명령을 건너뛴다.
 *
 * 읽기가 아니면(즉 쓰기이면) 1 을 돌려주어 원래의 어보트 처리로 넘긴다.
 *
 * **#ifdef CONFIG_ARM 안에서만 존재한다.** 64비트 ARM 에는 이 훅이 없고,
 * 등록도 imx_pcie_init() 안의 같은 조건부에서만 한다.
 *
 * 실행 컨텍스트: **데이터 어보트 예외 컨텍스트.** 잠들 수 없고, 여기서
 * 드라이버 상태를 건드려서도 안 된다. imx_pcie_init() 의 상류 주석이
 * 그 점을 짚는다 — 이 핸들러가 아무 상태도 만지지 않으므로 probe 가
 * 미뤄지더라도 미리 등록해 둘 수 있다는 것이다.
 *
 * 호출 체인:  ARM 어보트 처리부 -> [이 함수]
 */
static int imx6q_pcie_abort_handler(unsigned long addr,
		unsigned int fsr, struct pt_regs *regs)
{
	unsigned long pc = instruction_pointer(regs);	/* [한국어] 어보트가 난 시점의 명령 포인터 */
	unsigned long instr = *(unsigned long *)pc;	/* [한국어] **그 주소의 명령어 워드를 직접 읽는다.** 명령 인코딩을 해독하기 위해서다 */
	int reg = (instr >> 12) & 15;	/* [한국어] ARM 명령의 비트 12~15 가 목적 레지스터 번호다 */

	/*
	 * If the instruction being executed was a read,
	 * make it look like it read all-ones.
	 */
	if ((instr & 0x0c100000) == 0x04100000) {	/* [한국어] **LDR 계열 적재인지 본다.** 이 마스크와 값이 그 인코딩을 가른다 */
		unsigned long val;	/* [한국어] 꾸며 넣을 값 */

		if (instr & 0x00400000)	/* [한국어] 바이트 적재 비트가 서 있으면 */
			val = 255;	/* [한국어] 바이트 폭의 전부 1 인 255 를, */
		else
			val = -1;	/* [한국어] 워드 폭의 전부 1 인 -1 을 쓴다 */

		regs->uregs[reg] = val;	/* [한국어] **목적 레지스터에 그 값을 넣는다** — 읽기가 성공해 전부 1 을 읽은 것처럼 보이게 하는 것이다 */
		regs->ARM_pc += 4;	/* [한국어] PC 를 4 만큼 밀어 그 명령을 건너뛴다 */
		return 0;	/* [한국어] 처리했으니 그대로 이어 가라고 알린다 */
	}

	if ((instr & 0x0e100090) == 0x00100090) {	/* [한국어] **하프워드/부호 확장 적재 계열인지 본다** */
		regs->uregs[reg] = -1;	/* [한국어] 목적 레지스터에 전부 1 을 넣고 */
		regs->ARM_pc += 4;	/* [한국어] PC 를 밀어 건너뛰고 */
		return 0;	/* [한국어] 처리했다고 알린다 */
	}

	return 1;	/* [한국어] 읽기가 아니면(즉 쓰기이면) 처리하지 못했다고 알려 원래의 어보트 처리로 넘긴다 */
}
#endif

/* [한국어]
 * imx_pcie_attach_pd - PCIe 와 PCIe PHY 전원 도메인을 이 장치에 붙인다
 *
 * @dev: 플랫폼 장치.
 * @return: 0 이면 성공(붙일 도메인이 없는 경우 포함), 실패면 오류 코드.
 *
 * 일부 i.MX 세대는 PCIe 컨트롤러와 PHY 가 서로 다른 전원 도메인에 들어
 * 있어, 둘을 각각 켜 주지 않으면 레지스터 접근조차 되지 않는다. 장치
 * 트리의 power-domains 항목이 하나뿐이면 커널이 알아서 붙여 주므로 아무
 * 일도 하지 않고, 둘 이상일 때만 이름("pcie", "pcie_phy")으로 각각 붙인다.
 *
 * 붙인 뒤 device_link_add() 로 의존 관계를 건다. 플래그 셋의 뜻은
 *   DL_FLAG_STATELESS   — 링크의 수명을 커널이 자동 관리하지 않는다.
 *   DL_FLAG_PM_RUNTIME  — 런타임 PM 순서를 이 링크가 강제한다.
 *   DL_FLAG_RPM_ACTIVE  — 링크를 걸 때 공급자를 곧바로 활성 상태로 만든다.
 * 즉 이 장치가 살아 있는 동안 두 전원 도메인이 먼저 켜지고 나중에 꺼지도록
 * 순서를 못 박는 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   imx_pcie_probe() -> [이 함수] -> dev_pm_domain_attach_by_name()
 *                                 -> device_link_add()
 */
static int imx_pcie_attach_pd(struct device *dev)
{
	struct imx_pcie *imx_pcie = dev_get_drvdata(dev);	/* [한국어] probe 가 심어 둔 이 드라이버 인스턴스 */
	struct device_link *link;	/* [한국어] device_link_add() 가 돌려줄 링크 */

	/* Do nothing when in a single power domain */
	if (dev->pm_domain)	/* [한국어] **전원 도메인이 하나뿐이면 커널이 이미 붙여 주었다** */
		return 0;	/* [한국어] 할 일이 없다 */

	imx_pcie->pd_pcie = dev_pm_domain_attach_by_name(dev, "pcie");	/* [한국어] "pcie" 라는 이름의 도메인을 붙인다 */
	if (IS_ERR(imx_pcie->pd_pcie))	/* [한국어] 실패하면 */
		return PTR_ERR(imx_pcie->pd_pcie);	/* [한국어] 그 오류를 그대로 돌려준다 */
	/* Do nothing when power domain missing */
	if (!imx_pcie->pd_pcie)	/* [한국어] 그 이름의 도메인이 없으면 */
		return 0;	/* [한국어] 붙일 것이 없으므로 성공으로 나간다 */
	link = device_link_add(dev, imx_pcie->pd_pcie,	/* [한국어] **의존 관계를 건다** — 이 장치가 사는 동안 그 도메인이 먼저 켜지고 나중에 꺼지게 한다 */
			DL_FLAG_STATELESS |	/* [한국어] 링크의 수명을 커널이 자동 관리하지 않는다 */
			DL_FLAG_PM_RUNTIME |	/* [한국어] 런타임 PM 순서를 이 링크가 강제한다 */
			DL_FLAG_RPM_ACTIVE);	/* [한국어] 링크를 걸 때 공급자를 곧바로 활성 상태로 만든다 */
	if (!link) {	/* [한국어] 링크를 못 걸면 */
		dev_err(dev, "Failed to add device_link to pcie pd\n");	/* [한국어] 그 사실을 알리고 */
		return -EINVAL;	/* [한국어] 인자 오류로 돌아간다 */
	}

	imx_pcie->pd_pcie_phy = dev_pm_domain_attach_by_name(dev, "pcie_phy");	/* [한국어] **PHY 쪽 도메인도 따로 붙인다** — 컨트롤러와 PHY 가 다른 도메인에 있는 세대 때문이다 */
	if (IS_ERR(imx_pcie->pd_pcie_phy))	/* [한국어] 실패하면 */
		return PTR_ERR(imx_pcie->pd_pcie_phy);	/* [한국어] 그 오류를 그대로 돌려준다 */

	link = device_link_add(dev, imx_pcie->pd_pcie_phy,	/* [한국어] 같은 방식으로 의존 관계를 건다 */
			DL_FLAG_STATELESS |	/* [한국어] 같은 세 플래그 */
			DL_FLAG_PM_RUNTIME |
			DL_FLAG_RPM_ACTIVE);
	if (!link) {	/* [한국어] 링크를 못 걸면 */
		dev_err(dev, "Failed to add device_link to pcie_phy pd\n");	/* [한국어] 그 사실을 알리고 */
		return -EINVAL;	/* [한국어] 인자 오류로 돌아간다 */
	}

	return 0;	/* [한국어] 두 도메인을 모두 붙였으면 성공 */
}

/* [한국어]
 * imx6sx_pcie_enable_ref_clk - i.MX6SX 의 PHY 테스트 전원차단 비트로 레퍼런스 클럭을 켜고 끈다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @enable:   true 면 켜기, false 면 끄기.
 * @return: 항상 0.
 *
 * drvdata[IMX6SX].enable_ref_clk 로 등록된다. 6SX 에서는 레퍼런스 클럭을
 * 따로 켜는 비트가 아니라 **PHY 를 테스트 전원차단 상태에서 꺼내는 비트**
 * 가 그 역할을 한다. 그래서 켤 때는 TEST_POWERDOWN 을 0 으로, 끌 때는
 * 1 로 쓴다 — 삼항 연산자가 뒤집혀 보이는 이유다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx_pcie_clk_enable()/imx_pcie_clk_disable()
 *     -> drvdata->enable_ref_clk -> [이 함수] -> regmap_update_bits()
 */
static int imx6sx_pcie_enable_ref_clk(struct imx_pcie *imx_pcie, bool enable)
{
	regmap_update_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR12,	/* [한국어] **6SX 에서는 PHY 를 테스트 전원차단에서 꺼내는 비트가 레퍼런스 클럭 역할을 한다** */
			   IMX6SX_GPR12_PCIE_TEST_POWERDOWN,	/* [한국어] 그 비트를 */
			   enable ? 0 : IMX6SX_GPR12_PCIE_TEST_POWERDOWN);	/* [한국어] **켤 때 0 을 쓴다** — 전원차단을 푸는 것이 곧 켜는 것이기 때문이다 */
	return 0;	/* [한국어] 실패 경로가 없어 늘 성공이다 */
}

/* [한국어]
 * imx6q_pcie_enable_ref_clk - i.MX6Q/6QP 의 PHY 전원과 레퍼런스 클럭을 순서대로 켜고 끈다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @enable:   true 면 켜기, false 면 끄기.
 * @return: 항상 0.
 *
 * **순서가 중요하다.** 켤 때는 상류 주석대로 PHY 전원을 먼저 넣고
 * (TEST_PD 를 0 으로), 약 10us 기다린 뒤 레퍼런스 클럭을 켠다. 주석이
 * 그 지연의 이유를 밝힌다 — 비동기 리셋 입력이 내부에서 동기화되려면
 * 레퍼런스 클럭이 필요한데, 클럭이 리셋보다 늦게 오면 내부에서 동기화된
 * 리셋 시간이 너무 짧아져 요구 조건을 못 맞춘다.
 *
 * 끌 때는 반대로 클럭을 먼저 끄고 전원을 내린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep 으로 잠든다.
 *
 * 호출 체인:
 *   imx_pcie_clk_enable()/imx_pcie_clk_disable()
 *     -> drvdata->enable_ref_clk -> [이 함수]
 */
static int imx6q_pcie_enable_ref_clk(struct imx_pcie *imx_pcie, bool enable)
{
	if (enable) {	/* [한국어] 켤 때는 순서가 있다 */
		/* power up core phy and enable ref clock */
		regmap_clear_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR1, IMX6Q_GPR1_PCIE_TEST_PD);	/* [한국어] **먼저 PHY 전원을 넣는다** — 테스트 전원차단 비트를 지우는 것이다. 바로 위 상류 주석이 그렇게 적고 있다 */
		/*
		 * The async reset input need ref clock to sync internally,
		 * when the ref clock comes after reset, internal synced
		 * reset time is too short, cannot meet the requirement.
		 * Add a ~10us delay here.
		 */
		usleep_range(10, 100);	/* [한국어] 바로 위 상류 주석이 이 지연의 이유를 밝힌다 — 비동기 리셋 입력이 내부에서 동기화되려면 레퍼런스 클럭이 필요한데, 클럭이 리셋보다 늦게 오면 동기화된 리셋 시간이 요구 조건에 못 미친다 */
		regmap_set_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR1, IMX6Q_GPR1_PCIE_REF_CLK_EN);	/* [한국어] **그 뒤에 레퍼런스 클럭을 켠다** */
	} else {
		regmap_clear_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR1, IMX6Q_GPR1_PCIE_REF_CLK_EN);	/* [한국어] 클럭을 먼저 끄고 */
		regmap_set_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR1, IMX6Q_GPR1_PCIE_TEST_PD);	/* [한국어] PHY 전원을 내린다 */
	}

	return 0;	/* [한국어] 실패 경로가 없어 늘 성공이다 */
}

/* [한국어]
 * imx8mm_pcie_clkreq_override - 8M 계열에서 CLKREQ# 신호를 소프트웨어로 덮어쓴다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @enable:   true 면 오버라이드를 걸어 CLKREQ# 를 강제로 어서트한 상태로
 *            둔다. false 면 오버라이드를 풀어 실제 핀 신호를 따르게 한다.
 *
 * CLKREQ# 는 엔드포인트가 레퍼런스 클럭을 계속 달라고 요청하는 신호다.
 * 보드에 그 배선이 없거나 링크가 아직 서기 전이면 신호를 믿을 수 없으므로,
 * 소프트웨어로 "요청이 있는 것" 처럼 덮어써 클럭이 끊기지 않게 한다.
 *
 * 비트 둘을 함께 만진다 — OVERRIDE 는 덮어쓸 값이고 OVERRIDE_EN 은 그
 * 덮어쓰기를 켜는 스위치다. **enable 이 true 일 때 값 비트를 0 으로 쓰는
 * 것이 CLKREQ# 를 어서트하는 것**이다(낮은 값이 요청이라는 규격상 극성).
 * 두 삼항 연산자가 서로 반대 방향인 이유가 그것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx8mm_pcie_enable_ref_clk() / imx8mm_pcie_clr_clkreq_override()
 *     -> [이 함수] -> imx_pcie_grp_offset() -> regmap_update_bits()
 */
static void imx8mm_pcie_clkreq_override(struct imx_pcie *imx_pcie, bool enable)
{
	int offset = imx_pcie_grp_offset(imx_pcie);	/* [한국어] 컨트롤러 번호에 따라 GPR14 또는 GPR16 을 고른다 */

	regmap_update_bits(imx_pcie->iomuxc_gpr, offset,	/* [한국어] **덮어쓸 CLKREQ# 값을 정한다** */
			   IMX8MQ_GPR_PCIE_CLK_REQ_OVERRIDE,	/* [한국어] 값 비트를 */
			   enable ? 0 : IMX8MQ_GPR_PCIE_CLK_REQ_OVERRIDE);	/* [한국어] **켤 때 0 을 쓴다** — CLKREQ# 는 낮은 값이 요청이라는 규격상 극성 때문이다 */
	regmap_update_bits(imx_pcie->iomuxc_gpr, offset,	/* [한국어] **그 덮어쓰기를 켜는 스위치를 정한다** */
			   IMX8MQ_GPR_PCIE_CLK_REQ_OVERRIDE_EN,	/* [한국어] 스위치 비트를 */
			   enable ? IMX8MQ_GPR_PCIE_CLK_REQ_OVERRIDE_EN : 0);	/* [한국어] 켤 때 1 을 쓴다. 위 삼항 연산자와 방향이 반대인 이유가 극성 차이다 */
}

/* [한국어]
 * imx8mm_pcie_enable_ref_clk - 8M 계열의 레퍼런스 클럭 활성화는 CLKREQ# 오버라이드로 대신한다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @enable:   true 면 켜기, false 면 끄기.
 * @return: 항상 0.
 *
 * drvdata 의 8MM/8MP(및 EP 판) 항목에서 enable_ref_clk 로 등록된다.
 * 이 세대에는 레퍼런스 클럭을 직접 켜고 끄는 비트가 없고, 대신 CLKREQ#
 * 오버라이드를 걸어 클럭 공급이 계속되게 만든다. 그래서 이 함수는
 * imx8mm_pcie_clkreq_override() 로 그대로 넘긴다.
 *
 * 링크가 선 뒤에는 오버라이드를 풀어 하드웨어 신호를 따르게 하는데,
 * 그 자리가 imx_pcie_host_post_init() 이 부르는 clr_clkreq_override 다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx_pcie_clk_enable()/imx_pcie_clk_disable()
 *     -> drvdata->enable_ref_clk -> [이 함수]
 *     -> imx8mm_pcie_clkreq_override()
 */
static int imx8mm_pcie_enable_ref_clk(struct imx_pcie *imx_pcie, bool enable)
{
	imx8mm_pcie_clkreq_override(imx_pcie, enable);	/* [한국어] **이 세대에는 레퍼런스 클럭을 직접 켜는 비트가 없어** CLKREQ# 오버라이드로 클럭 공급이 이어지게 만든다 */
	return 0;	/* [한국어] 실패 경로가 없어 늘 성공이다 */
}

/* [한국어]
 * imx7d_pcie_enable_ref_clk - i.MX7D 의 PHY 레퍼런스 클럭 소스를 고른다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @enable:   true 면 켜기, false 면 끄기.
 * @return: 항상 0.
 *
 * GPR12[PCIE_PHY_REFCLK_SEL] 하나만 만진다. **켤 때 0 을 쓴다** — 이
 * 비트가 서 있으면 다른 소스를 고른다는 뜻이라, 정상 동작에서는 지워
 * 두어야 하기 때문이다. 6SX 의 TEST_POWERDOWN 과 마찬가지로 삼항
 * 연산자가 뒤집혀 보이는 이유가 그것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx_pcie_clk_enable()/imx_pcie_clk_disable()
 *     -> drvdata->enable_ref_clk -> [이 함수] -> regmap_update_bits()
 */
static int imx7d_pcie_enable_ref_clk(struct imx_pcie *imx_pcie, bool enable)
{
	regmap_update_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR12,	/* [한국어] **7D 는 레퍼런스 클럭 소스를 고르는 비트를 쓴다** */
			   IMX7D_GPR12_PCIE_PHY_REFCLK_SEL,	/* [한국어] 소스 선택 비트를 */
			   enable ? 0 : IMX7D_GPR12_PCIE_PHY_REFCLK_SEL);	/* [한국어] **켤 때 0 을 쓴다** — 이 비트가 서 있으면 다른 소스를 고른다는 뜻이라 정상 동작에서는 지워 두어야 한다 */
	return 0;	/* [한국어] 실패 경로가 없어 늘 성공이다 */
}

/* [한국어]
 * imx95_pcie_clkreq_override - i.MX95 에서 CLKREQ# 신호를 소프트웨어로 덮어쓴다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @enable:   true 면 오버라이드를 걸고, false 면 푼다.
 *
 * 8M 계열의 같은 이름 함수와 목적이 같지만 레지스터와 극성이 다르다.
 * i.MX95 는 SS_RW_REG_1 의 CLKREQ_OVERRIDE_EN(스위치)과
 * CLKREQ_OVERRIDE_VAL(덮어쓸 값)을 쓰며, **두 삼항 연산자가 같은
 * 방향**이다 — enable 이 true 면 둘 다 세운다. 8M 판에서 값 비트만
 * 반대였던 것과 대비된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx95_pcie_enable_ref_clk() / imx95_pcie_clr_clkreq_override()
 *     -> [이 함수] -> regmap_update_bits()
 */
static void imx95_pcie_clkreq_override(struct imx_pcie *imx_pcie, bool enable)
{
	regmap_update_bits(imx_pcie->iomuxc_gpr, IMX95_PCIE_SS_RW_REG_1,	/* [한국어] **덮어쓰기를 켜는 스위치를 정한다** */
			   IMX95_PCIE_CLKREQ_OVERRIDE_EN,	/* [한국어] 스위치 비트를 */
			   enable ? IMX95_PCIE_CLKREQ_OVERRIDE_EN : 0);	/* [한국어] 켤 때 세운다 */
	regmap_update_bits(imx_pcie->iomuxc_gpr, IMX95_PCIE_SS_RW_REG_1,	/* [한국어] **덮어쓸 값을 정한다** */
			   IMX95_PCIE_CLKREQ_OVERRIDE_VAL,	/* [한국어] 값 비트를 */
			   enable ? IMX95_PCIE_CLKREQ_OVERRIDE_VAL : 0);	/* [한국어] 켤 때 함께 세운다. **8M 판과 달리 두 삼항 연산자가 같은 방향**이다 */
}

/* [한국어]
 * imx95_pcie_enable_ref_clk - i.MX95 의 레퍼런스 클럭 활성화도 CLKREQ# 오버라이드로 대신한다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @enable:   true 면 켜기, false 면 끄기.
 * @return: 항상 0.
 *
 * drvdata[IMX95] 와 [IMX95_EP] 에서 enable_ref_clk 로 등록된다. 8M 계열과
 * 같은 구조로, imx95_pcie_clkreq_override() 에 그대로 넘긴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx_pcie_clk_enable()/imx_pcie_clk_disable()
 *     -> drvdata->enable_ref_clk -> [이 함수]
 *     -> imx95_pcie_clkreq_override()
 */
static int imx95_pcie_enable_ref_clk(struct imx_pcie *imx_pcie, bool enable)
{
	imx95_pcie_clkreq_override(imx_pcie, enable);	/* [한국어] i.MX95 도 CLKREQ# 오버라이드로 레퍼런스 클럭 공급을 이어 간다 */
	return 0;	/* [한국어] 실패 경로가 없어 늘 성공이다 */
}

/* [한국어]
 * imx8mm_pcie_clr_clkreq_override - 링크가 선 뒤 8M 계열의 CLKREQ# 오버라이드를 푼다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 *
 * drvdata 의 8M 계열 항목에서 clr_clkreq_override 로 등록된다.
 * imx_pcie_host_post_init() 이 **링크가 서 있고 보드가 CLKREQ# 배선을
 * 지원할 때만**(supports_clkreq) 부른다. 그때부터는 실제 핀 신호를
 * 따르게 되어, 엔드포인트가 요청하지 않으면 레퍼런스 클럭을 끌 수 있다 —
 * 즉 전력 절감이 가능해진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx_pcie_host_post_init() -> drvdata->clr_clkreq_override -> [이 함수]
 *     -> imx8mm_pcie_clkreq_override(false)
 */
static void imx8mm_pcie_clr_clkreq_override(struct imx_pcie *imx_pcie)
{
	imx8mm_pcie_clkreq_override(imx_pcie, false);	/* [한국어] 오버라이드를 풀어 실제 CLKREQ# 핀 신호를 따르게 한다. 그때부터 엔드포인트가 요청하지 않으면 클럭을 끌 수 있다 */
}

/* [한국어]
 * imx95_pcie_clr_clkreq_override - 링크가 선 뒤 i.MX95 의 CLKREQ# 오버라이드를 푼다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 *
 * 8M 계열의 같은 이름 함수와 목적이 같고, 레지스터만 i.MX95 쪽
 * (SS_RW_REG_1)을 쓴다. drvdata[IMX95] 계열에서 clr_clkreq_override 로
 * 등록된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx_pcie_host_post_init() -> drvdata->clr_clkreq_override -> [이 함수]
 *     -> imx95_pcie_clkreq_override(false)
 */
static void imx95_pcie_clr_clkreq_override(struct imx_pcie *imx_pcie)
{
	imx95_pcie_clkreq_override(imx_pcie, false);	/* [한국어] i.MX95 쪽 레지스터로 같은 일을 한다 */
}

/* [한국어]
 * imx_pcie_clk_enable - 장치 트리가 준 클럭 묶음을 켜고 레퍼런스 클럭까지 손본다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 성공. 클럭 켜기가 실패하면 그 코드, 레퍼런스 클럭
 *          설정이 실패하면 앞서 켠 클럭을 되돌리고 그 코드.
 *
 * 두 단계다.
 *   1. clk_bulk_prepare_enable() 로 장치 트리에 적힌 클럭을 한꺼번에 켠다.
 *      개수와 목록은 probe 에서 devm_clk_bulk_get_all() 로 모아 두었다.
 *   2. 세대별 enable_ref_clk 가 있으면 부른다. 세대마다 레퍼런스 클럭을
 *      켜는 방법이 전혀 달라(전원차단 해제, CLKREQ# 오버라이드, 소스
 *      선택) 표의 함수 포인터로 갈라 둔 부분이다.
 *
 * 마지막의 200~500us 대기는 상류 주석대로 클럭이 안정될 시간을 주는 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep 으로 잠든다.
 *
 * 에러 경로: 2단계가 실패하면 err_ref_clk 로 가서 1단계를 되돌린다.
 *
 * 호출 체인:
 *   imx_pcie_host_init() / imx_pcie_resume_noirq() -> [이 함수]
 *     -> clk_bulk_prepare_enable() -> drvdata->enable_ref_clk()
 */
static int imx_pcie_clk_enable(struct imx_pcie *imx_pcie)
{
	struct dw_pcie *pci = imx_pcie->pci;	/* [한국어] 오류 메시지를 낼 장치를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	int ret;	/* [한국어] 하위 호출 결과 */

	ret = clk_bulk_prepare_enable(imx_pcie->num_clks, imx_pcie->clks);	/* [한국어] **장치 트리에 적힌 클럭을 한꺼번에 켠다.** 개수와 목록은 probe 가 모아 두었다 */
	if (ret)	/* [한국어] 실패하면 */
		return ret;	/* [한국어] 그 코드를 그대로 돌려준다. 아직 되돌릴 것이 없다 */

	if (imx_pcie->drvdata->enable_ref_clk) {	/* [한국어] **세대별 레퍼런스 클럭 처리가 있으면** */
		ret = imx_pcie->drvdata->enable_ref_clk(imx_pcie, true);	/* [한국어] 켜라고 부른다 */
		if (ret) {	/* [한국어] 실패하면 */
			dev_err(dev, "Failed to enable PCIe REFCLK\n");	/* [한국어] 그 사실을 알리고 */
			goto err_ref_clk;	/* [한국어] 앞서 켠 클럭을 되돌리는 라벨로 간다 */
		}
	}

	/* allow the clocks to stabilize */
	usleep_range(200, 500);	/* [한국어] 바로 위 상류 주석대로 클럭이 안정될 시간을 준다 */
	return 0;	/* [한국어] 두 단계를 모두 마쳤으면 성공 */

err_ref_clk:	/* [한국어] **되돌리기 경로** — 레퍼런스 클럭 설정만 실패한 경우다 */
	clk_bulk_disable_unprepare(imx_pcie->num_clks, imx_pcie->clks);	/* [한국어] 앞서 켠 클럭 묶음을 내린다 */

	return ret;	/* [한국어] 담아 둔 오류 코드를 돌려준다 */
}

/* [한국어]
 * imx_pcie_clk_disable - 레퍼런스 클럭을 먼저 끄고 클럭 묶음을 내린다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 *
 * imx_pcie_clk_enable() 의 짝이며 **순서가 반대**다. 세대별
 * enable_ref_clk(..., false) 를 먼저 부르고 그 다음 클럭 묶음을 내린다 —
 * 레퍼런스 클럭 제어가 GPR 레지스터 접근을 필요로 할 수 있어, 클럭이
 * 살아 있는 동안 해야 하기 때문이다.
 *
 * 반환값이 없어 실패를 알리지 않는다. 정리 경로에서만 쓰이므로 되돌릴
 * 것이 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx_pcie_host_init() 실패 경로 / imx_pcie_host_exit() /
 *   imx_pcie_suspend_noirq() -> [이 함수]
 *     -> drvdata->enable_ref_clk() -> clk_bulk_disable_unprepare()
 */
static void imx_pcie_clk_disable(struct imx_pcie *imx_pcie)
{
	if (imx_pcie->drvdata->enable_ref_clk)	/* [한국어] **세대별 레퍼런스 클럭 처리를 먼저 끈다** — 그 처리가 GPR 접근을 필요로 할 수 있어 클럭이 살아 있는 동안 해야 한다 */
		imx_pcie->drvdata->enable_ref_clk(imx_pcie, false);	/* [한국어] 끄라고 부른다. 반환값은 보지 않는다 */
	clk_bulk_disable_unprepare(imx_pcie->num_clks, imx_pcie->clks);	/* [한국어] 그 다음 클럭 묶음을 내린다 */
}

/* [한국어]
 * imx6sx_pcie_core_reset - i.MX6SX 의 코어 리셋을 걸고 푼다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @assert:   true 면 리셋을 걸고, false 면 푼다.
 * @return: 항상 0.
 *
 * drvdata[IMX6SX].core_reset 로 등록된다. 두 가지를 한다.
 *   - 리셋을 걸 때만 GPR12[PCIE_TEST_POWERDOWN] 을 세워 PHY 를 전원차단
 *     상태로 넣는다. 풀 때는 이 비트를 건드리지 않는데, 그것은
 *     imx6sx_pcie_enable_ref_clk() 이 클럭을 켜면서 함께 푸는 몫이다.
 *   - 상류 주석대로 GPR5[PCIE_BTNRST_RESET] 로 PHY 리셋을 강제한다.
 *     이쪽은 assert 값을 그대로 따라 걸고 푼다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx_pcie_assert_core_reset()/imx_pcie_deassert_core_reset()
 *     -> drvdata->core_reset -> [이 함수] -> regmap_*_bits()
 */
static int imx6sx_pcie_core_reset(struct imx_pcie *imx_pcie, bool assert)
{
	if (assert)	/* [한국어] **리셋을 걸 때만** PHY 를 전원차단에 넣는다 */
		regmap_set_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR12,	/* [한국어] 테스트 전원차단 비트를 */
				IMX6SX_GPR12_PCIE_TEST_POWERDOWN);	/* [한국어] 세운다. 푸는 쪽은 imx6sx_pcie_enable_ref_clk() 이 클럭을 켜면서 함께 한다 */

	/* Force PCIe PHY reset */
	regmap_update_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR5, IMX6SX_GPR5_PCIE_BTNRST_RESET,	/* [한국어] 바로 위 상류 주석대로 PHY 리셋을 강제한다. 이쪽은 assert 값을 그대로 따른다 */
			   assert ? IMX6SX_GPR5_PCIE_BTNRST_RESET : 0);	/* [한국어] 걸면 세우고 풀면 지운다 */
	return 0;	/* [한국어] 실패 경로가 없어 늘 성공이다 */
}

/* [한국어]
 * imx6qp_pcie_core_reset - i.MX6QP 의 소프트웨어 리셋 비트를 걸고 푼다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @assert:   true 면 리셋을 걸고, false 면 푼다.
 * @return: 항상 0.
 *
 * drvdata[IMX6QP].core_reset 로 등록된다. 6QP 에는 6Q 에 없는
 * GPR1[PCIE_SW_RST] 비트가 있어 리셋을 깔끔하게 걸고 풀 수 있다.
 *
 * 푼 뒤에만 200~500us 기다린다 — 리셋에서 나온 블록이 안정될 시간을
 * 주는 것이며, 걸 때는 기다릴 이유가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep 으로 잠든다.
 *
 * 호출 체인:
 *   imx_pcie_assert_core_reset()/imx_pcie_deassert_core_reset()
 *     -> drvdata->core_reset -> [이 함수]
 */
static int imx6qp_pcie_core_reset(struct imx_pcie *imx_pcie, bool assert)
{
	regmap_update_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR1, IMX6Q_GPR1_PCIE_SW_RST,	/* [한국어] **6QP 에만 있는 전용 소프트웨어 리셋 비트**를 쓴다 */
			   assert ? IMX6Q_GPR1_PCIE_SW_RST : 0);	/* [한국어] 걸면 세우고 풀면 지운다 */
	if (!assert)	/* [한국어] **푼 뒤에만** */
		usleep_range(200, 500);	/* [한국어] 200~500us 기다린다 — 리셋에서 나온 블록이 안정될 시간이다. 걸 때는 기다릴 이유가 없다 */

	return 0;	/* [한국어] 실패 경로가 없어 늘 성공이다 */
}

/* [한국어]
 * imx6q_pcie_core_reset - i.MX6Q 는 리셋을 걸 때만 PHY 전원과 클럭을 손댄다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @assert:   true 면 리셋을 걸고, false 면 아무 일도 하지 않는다.
 * @return: 항상 0.
 *
 * drvdata[IMX6Q].core_reset 로 등록된다. **6Q 에는 전용 소프트웨어 리셋
 * 비트가 없다.** 그래서 리셋을 거는 대신 PHY 를 테스트 전원차단
 * (GPR1[PCIE_TEST_PD])에 넣고 레퍼런스 클럭 활성화 비트를 세워 둔다.
 *
 * 푸는 쪽은 곧바로 0 으로 나간다 — 되돌리는 일은
 * imx6q_pcie_enable_ref_clk() 이 클럭을 켜면서 순서를 맞춰 하기 때문이다.
 * 6QP 가 같은 세대인데도 별도 함수를 쓰는 이유가 이 차이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx_pcie_assert_core_reset() -> drvdata->core_reset -> [이 함수]
 */
static int imx6q_pcie_core_reset(struct imx_pcie *imx_pcie, bool assert)
{
	if (!assert)	/* [한국어] **푸는 쪽은 할 일이 없다** — 되돌리기는 imx6q_pcie_enable_ref_clk() 이 순서를 맞춰 한다 */
		return 0;	/* [한국어] 그대로 나간다 */

	regmap_set_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR1, IMX6Q_GPR1_PCIE_TEST_PD);	/* [한국어] **6Q 에는 전용 리셋 비트가 없어** PHY 를 테스트 전원차단에 넣는 것으로 대신한다 */
	regmap_set_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR1, IMX6Q_GPR1_PCIE_REF_CLK_EN);	/* [한국어] 레퍼런스 클럭 활성화 비트도 함께 세워 둔다 */

	return 0;	/* [한국어] 실패 경로가 없어 늘 성공이다 */
}

/* [한국어]
 * imx7d_pcie_core_reset - i.MX7D 의 리셋 해제 시 PLL 잠금 실패 errata 를 우회한다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @assert:   true 면 아무 일도 하지 않고, false(리셋 해제)일 때만 동작한다.
 * @return: 항상 0.
 *
 * drvdata[IMX7D].core_reset 로 등록된다. 리셋을 거는 쪽은 곧바로 나가고,
 * **푸는 쪽에서만 ERR010728 우회를 수행한다.**
 *
 * 바로 아래 상류 주석이 errata 를 그대로 인용한다 — 저온 같은 코너
 * 조건에서 초기 VCO 발진이 실패해 PCIe PLL 이 초기화 단계에서 잠기지
 * 못한다. 우회는 Duty-cycle Corrector 보정을 끄는 것이고, 주석이 다섯
 * 단계를 적어 두었다. 이 함수가 맡는 것은 그중 2~4단계로, PHY 레지스터
 * 셋에 값을 직접 쓴다.
 *   CMN_REG4  <- 0x29  (DCC_FB_EN 해제)
 *   CMN_REG24 <- RX_EQ | RX_EQ_SEL (어서트)
 *   CMN_REG26 <- 0xBC  (ATT_MODE 어서트)
 * 1단계와 5단계의 리셋 신호 조작은 reset_control 쪽에서 이미 이루어진다.
 *
 * **이 PHY 레지스터는 메모리 맵이다** — i.MX6 자체 PHY 와 달리 장치
 * 트리의 fsl,imx7d-pcie-phy phandle 로 얻은 phy_base 에 직접 writel 한다.
 * 그 phandle 이 없으면 phy_base 가 NULL 이라 우회를 못 하고, 그 사실을
 * 경고로 남긴다.
 *
 * 마지막으로 PLL 이 잠기기를 기다린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx_pcie_deassert_core_reset() -> drvdata->core_reset -> [이 함수]
 *     -> writel() -> imx7d_pcie_wait_for_phy_pll_lock()
 */
static int imx7d_pcie_core_reset(struct imx_pcie *imx_pcie, bool assert)
{
	struct dw_pcie *pci = imx_pcie->pci;	/* [한국어] 경고 메시지를 낼 장치를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev;	/* [한국어] 경고 메시지를 낼 장치 */

	if (assert)	/* [한국어] **거는 쪽은 할 일이 없다** — 아래 우회는 리셋을 푸는 시점에 해야 한다 */
		return 0;	/* [한국어] 그대로 나간다 */

	/*
	 * Workaround for ERR010728 (IMX7DS_2N09P, Rev. 1.1, 4/2023):
	 *
	 * PCIe: PLL may fail to lock under corner conditions.
	 *
	 * Initial VCO oscillation may fail under corner conditions such as
	 * cold temperature which will cause the PCIe PLL fail to lock in the
	 * initialization phase.
	 *
	 * The Duty-cycle Corrector calibration must be disabled.
	 *
	 * 1. De-assert the G_RST signal by clearing
	 *    SRC_PCIEPHY_RCR[PCIEPHY_G_RST].
	 * 2. De-assert DCC_FB_EN by writing data “0x29” to the register
	 *    address 0x306d0014 (PCIE_PHY_CMN_REG4).
	 * 3. Assert RX_EQS, RX_EQ_SEL by writing data “0x48” to the register
	 *    address 0x306d0090 (PCIE_PHY_CMN_REG24).
	 * 4. Assert ATT_MODE by writing data “0xbc” to the register
	 *    address 0x306d0098 (PCIE_PHY_CMN_REG26).
	 * 5. De-assert the CMN_RST signal by clearing register bit
	 *    SRC_PCIEPHY_RCR[PCIEPHY_BTN]
	 */

	if (likely(imx_pcie->phy_base)) {	/* [한국어] **PHY 레지스터 창이 매핑되어 있으면** — 장치 트리에 fsl,imx7d-pcie-phy phandle 이 있어야 한다 */
		/* De-assert DCC_FB_EN */
		writel(PCIE_PHY_CMN_REG4_DCC_FB_EN, imx_pcie->phy_base + PCIE_PHY_CMN_REG4);	/* [한국어] **ERR010728 우회 2단계** — DCC_FB_EN 을 해제한다(값 0x29). 바로 위 상류 주석의 다섯 단계 중 둘째다 */
		/* Assert RX_EQS and RX_EQS_SEL */
		writel(PCIE_PHY_CMN_REG24_RX_EQ_SEL | PCIE_PHY_CMN_REG24_RX_EQ,	/* [한국어] **3단계** — RX_EQS 와 RX_EQ_SEL 을 어서트한다 */
		       imx_pcie->phy_base + PCIE_PHY_CMN_REG24);	/* [한국어] phy_base 기준의 CMN_REG24 에 쓴다. 이 PHY 는 i.MX6 자체 PHY 와 달리 메모리 맵이라 writel 로 직접 쓴다 */
		/* Assert ATT_MODE */
		writel(PCIE_PHY_CMN_REG26_ATT_MODE, imx_pcie->phy_base + PCIE_PHY_CMN_REG26);	/* [한국어] **4단계** — ATT_MODE 를 어서트한다(값 0xBC) */
	} else {
		dev_warn(dev, "Unable to apply ERR010728 workaround. DT missing fsl,imx7d-pcie-phy phandle ?\n");	/* [한국어] 우회를 적용할 수 없다고 경고만 남긴다. 그래도 아래 PLL 대기는 진행한다 */
	}
	imx7d_pcie_wait_for_phy_pll_lock(imx_pcie);	/* [한국어] PLL 이 잠기기를 기다린다. **결과를 확인하지 않는다** — 그 함수가 반환값이 없다 */
	return 0;	/* [한국어] 늘 성공으로 돌아간다 */
}

/* [한국어]
 * imx95_pcie_core_reset - i.MX95 의 COLD 리셋을 데이터시트 파형에 맞춰 토글한다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @assert:   true 일 때만 동작한다. false 면 아무 일도 하지 않는다.
 * @return: 항상 0.
 *
 * drvdata[IMX95] 계열의 core_reset 으로 등록된다. 바로 아래 상류 주석에
 * 파형 그림이 있다 — phy_reset 이 15us 정도 높게 유지된 뒤 내려가고,
 * 그 다음 ref_clk_en 이 올라오는 순서다. 전원 인가 시에는 10us 이상,
 * 웜 리셋에서는 10ns 이상이 요구된다.
 *
 * 그래서 COLD_RST 를 세우고 15us, 지운 뒤 10us 를 기다린다.
 *
 * **두 번의 regmap_read_bypassed() 가 핵심**이다. 상류 주석이 이유를
 * 밝힌다 — regmap 캐시나 버퍼 때문에 쓰기가 실제 하드웨어에 닿기 전에
 * udelay 가 시작될 수 있으므로, 읽기를 한 번 끼워 넣어 쓰기가 하드웨어에
 * 도달했음을 보장한다. bypassed 판을 쓰는 것은 캐시를 우회해 진짜
 * 레지스터를 읽기 위해서다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. udelay 로 바쁜 대기를 한다.
 *
 * 호출 체인:
 *   imx_pcie_assert_core_reset() -> drvdata->core_reset -> [이 함수]
 */
static int imx95_pcie_core_reset(struct imx_pcie *imx_pcie, bool assert)
{
	u32 val;	/* [한국어] regmap_read_bypassed() 가 값을 담을 자리. 그 값 자체는 쓰지 않는다 */

	if (assert) {	/* [한국어] **거는 쪽에서만 동작한다.** 푸는 쪽은 할 일이 없다 */
		/*
		 * From i.MX95 PCIe PHY perspective, the COLD reset toggle
		 * should be complete after power-up by the following sequence.
		 *                 > 10us(at power-up)
		 *                 > 10ns(warm reset)
		 *               |<------------>|
		 *                ______________
		 * phy_reset ____/              \________________
		 *                                   ____________
		 * ref_clk_en_______________________/
		 * Toggle COLD reset aligned with this sequence for i.MX95 PCIe.
		 */
		regmap_set_bits(imx_pcie->iomuxc_gpr, IMX95_PCIE_RST_CTRL,	/* [한국어] **COLD 리셋을 건다.** 바로 위 상류 주석의 파형 그림대로 이 신호를 먼저 올린다 */
				IMX95_PCIE_COLD_RST);	/* [한국어] 세울 비트 */
		/*
		 * Make sure the write to IMX95_PCIE_RST_CTRL is flushed to the
		 * hardware by doing a read. Otherwise, there is no guarantee
		 * that the write has reached the hardware before udelay().
		 */
		regmap_read_bypassed(imx_pcie->iomuxc_gpr, IMX95_PCIE_RST_CTRL,	/* [한국어] **읽기를 끼워 쓰기가 하드웨어에 도달했음을 보장한다.** 바로 위 상류 주석이 이유를 밝힌다 — 그러지 않으면 쓰기가 닿기 전에 udelay 가 시작될 수 있다. bypassed 판이라 캐시를 우회해 진짜 레지스터를 읽는다 */
				     &val);	/* [한국어] 값을 담을 자리(쓰지 않는다) */
		udelay(15);	/* [한국어] 전원 인가 시 요구되는 10us 이상을 넉넉히 지킨다 */
		regmap_clear_bits(imx_pcie->iomuxc_gpr, IMX95_PCIE_RST_CTRL,	/* [한국어] **COLD 리셋을 푼다** */
				  IMX95_PCIE_COLD_RST);	/* [한국어] 지울 비트 */
		regmap_read_bypassed(imx_pcie->iomuxc_gpr, IMX95_PCIE_RST_CTRL,	/* [한국어] 같은 이유로 읽기를 한 번 더 끼운다 */
				     &val);	/* [한국어] 값을 담을 자리 */
		udelay(10);	/* [한국어] 푼 뒤 10us 기다린 다음 레퍼런스 클럭이 올라오게 한다 */
	}

	return 0;	/* [한국어] 실패 경로가 없어 늘 성공이다 */
}

/* [한국어]
 * imx_pcie_assert_core_reset - 코어 리셋을 건다(공통 부분 + 세대별 부분)
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 *
 * 두 층으로 나뉜다.
 *   1. reset_control_assert(pciephy_reset) — 장치 트리에 리셋 컨트롤러가
 *      적혀 있는 세대(IMX_PCIE_FLAG_HAS_PHY_RESET)에서 표준 리셋
 *      서브시스템을 통해 PHY 리셋을 건다. 없는 세대에서는 이 포인터가
 *      NULL 이고 reset_control_assert() 가 조용히 넘어간다.
 *   2. drvdata->core_reset 이 있으면 세대별 절차를 수행한다. 세대마다
 *      쓰는 GPR 비트와 순서가 완전히 달라 표의 함수 포인터로 갈라 둔
 *      부분이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. imx_pcie_host_init() 의 가장 앞
 * 단계이며, 여기서 리셋을 걸어 놓고 PHY 초기화와 클럭 켜기를 한 뒤
 * imx_pcie_deassert_core_reset() 으로 푼다.
 *
 * 호출 체인:
 *   imx_pcie_host_init() -> [이 함수] -> reset_control_assert()
 *                                     -> drvdata->core_reset(true)
 */
static void imx_pcie_assert_core_reset(struct imx_pcie *imx_pcie)
{
	reset_control_assert(imx_pcie->pciephy_reset);	/* [한국어] **표준 리셋 서브시스템으로 PHY 리셋을 건다.** 그 핸들이 없는 세대에서는 NULL 이라 조용히 넘어간다 */

	if (imx_pcie->drvdata->core_reset)	/* [한국어] **세대별 리셋 절차가 있으면** */
		imx_pcie->drvdata->core_reset(imx_pcie, true);	/* [한국어] 걸라고 부른다. 세대마다 쓰는 GPR 비트와 순서가 전혀 달라 표의 함수 포인터로 갈라 두었다 */
}

/* [한국어]
 * imx_pcie_deassert_core_reset - 코어 리셋을 푼다(공통 부분 + 세대별 부분)
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 *
 * imx_pcie_assert_core_reset() 의 짝이며 구조가 같다. 표준 리셋
 * 서브시스템으로 PHY 리셋을 풀고, 세대별 core_reset(false) 를 부른다.
 *
 * **여기서 i.MX7D 의 ERR010728 우회가 실제로 수행된다** —
 * imx7d_pcie_core_reset() 이 assert 가 false 일 때만 동작하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. imx_pcie_host_init() 이 PHY 를
 * 켜고 LTSSM 을 끈 직후 부른다.
 *
 * 호출 체인:
 *   imx_pcie_host_init() -> [이 함수] -> reset_control_deassert()
 *                                     -> drvdata->core_reset(false)
 */
static void imx_pcie_deassert_core_reset(struct imx_pcie *imx_pcie)
{
	reset_control_deassert(imx_pcie->pciephy_reset);	/* [한국어] 표준 리셋 서브시스템으로 PHY 리셋을 푼다 */

	if (imx_pcie->drvdata->core_reset)	/* [한국어] 세대별 리셋 절차가 있으면 */
		imx_pcie->drvdata->core_reset(imx_pcie, false);	/* [한국어] 풀라고 부른다. **여기서 i.MX7D 의 ERR010728 우회가 실제로 수행된다** — 그 함수가 assert 가 false 일 때만 동작하기 때문이다 */
}

/* [한국어]
 * imx_pcie_wait_for_speed_change - 링크 속도 변경(Directed Speed Change)이 끝나기를 기다린다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 끝났다. 200회 안에 안 끝나면 -ETIMEDOUT.
 *
 * imx_pcie_start_link() 가 Gen1 으로 링크를 세운 뒤 더 빠른 속도로
 * 올리라고 지시했을 때, 그 협상이 끝났는지 확인하는 자리다.
 *
 * 보는 비트는 DWC 의 PORT_LOGIC_SPEED_CHANGE 로, 소프트웨어가 세우면
 * 하드웨어가 속도 협상을 시작하고 끝나면 **스스로 지운다.** 그래서
 * 비트가 지워진 것이 완료 신호다.
 *
 * 100~1000us 간격으로 200회 보므로 최악에도 0.2초 안팎이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep 으로 잠든다.
 *
 * 호출 체인:
 *   imx_pcie_start_link() -> [이 함수] -> dw_pcie_readl_dbi()
 */
static int imx_pcie_wait_for_speed_change(struct imx_pcie *imx_pcie)
{
	struct dw_pcie *pci = imx_pcie->pci;	/* [한국어] dbi 접근에 쓸 DWC 장치 구조체 */
	struct device *dev = pci->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	u32 tmp;	/* [한국어] 읽은 레지스터 값 */
	unsigned int retries;	/* [한국어] 반복 횟수 */

	for (retries = 0; retries < 200; retries++) {	/* [한국어] 최대 200회 본다 */
		tmp = dw_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);	/* [한국어] DWC 의 링크 폭·속도 제어 레지스터를 읽는다 */
		/* Test if the speed change finished. */
		if (!(tmp & PORT_LOGIC_SPEED_CHANGE))	/* [한국어] **속도 변경 비트가 지워졌으면 협상이 끝난 것이다** — 소프트웨어가 세우면 하드웨어가 시작하고 끝나면 스스로 지운다 */
			return 0;	/* [한국어] 성공으로 돌아간다 */
		usleep_range(100, 1000);	/* [한국어] 100~1000us 쉬고 다시 본다. 최악에도 0.2초 안팎이다 */
	}

	dev_err(dev, "Speed change timeout\n");	/* [한국어] 상한 안에 안 끝나면 알리고 */
	return -ETIMEDOUT;	/* [한국어] 시간 초과로 돌아간다 */
}

/* [한국어]
 * imx_pcie_ltssm_enable - LTSSM 을 켜 링크 훈련을 시작시킨다
 *
 * @dev: 플랫폼 장치. drvdata 에서 imx_pcie 를 되찾는 데 쓴다.
 *
 * LTSSM(Link Training and Status State Machine)은 PCIe 링크를 실제로
 * 세우는 상태 기계다. 이것이 돌기 시작해야 링크가 선다.
 *
 * 하는 일이 셋이다.
 *   1. **PHY 에 최대 속도를 알린다.** dbi 의 PCI Express capability 에서
 *      LNKCAP 의 최대 링크 속도 필드를 읽어 phy_set_speed() 로 넘긴다.
 *      generic PHY 드라이버를 쓰는 세대에서 PHY 가 그 속도에 맞게
 *      설정되도록 하는 것이다.
 *   2. drvdata 에 ltssm_mask 가 있으면 그 GPR 비트를 세운다. 세대마다
 *      LTSSM 활성화 비트의 레지스터와 자리가 달라 표에 좌표를 두었다.
 *   3. **apps_reset 을 푼다.** IMX_PCIE_FLAG_HAS_APP_RESET 세대는 GPR
 *      비트가 아니라 별도의 리셋 신호로 LTSSM 을 제어한다. 없는
 *      세대에서는 포인터가 NULL 이라 조용히 넘어간다.
 * 즉 세대에 따라 2번이나 3번 중 하나가 실제 스위치 역할을 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx_pcie_start_link() -> [이 함수] -> dw_pcie_find_capability()
 *     -> phy_set_speed() -> regmap_update_bits() -> reset_control_deassert()
 */
static void imx_pcie_ltssm_enable(struct device *dev)
{
	struct imx_pcie *imx_pcie = dev_get_drvdata(dev);	/* [한국어] 플랫폼 장치에 심어 둔 이 드라이버 인스턴스 */
	const struct imx_pcie_drvdata *drvdata = imx_pcie->drvdata;	/* [한국어] 세대별 표 항목 */
	u8 offset = dw_pcie_find_capability(imx_pcie->pci, PCI_CAP_ID_EXP);	/* [한국어] **PCI Express capability 의 위치를 찾는다** — LNKCAP 이 그 안에 있다 */
	u32 tmp;	/* [한국어] 읽은 레지스터 값 */

	tmp = dw_pcie_readl_dbi(imx_pcie->pci, offset + PCI_EXP_LNKCAP);	/* [한국어] 링크 능력 레지스터를 읽고 */
	phy_set_speed(imx_pcie->phy, FIELD_GET(PCI_EXP_LNKCAP_SLS, tmp));	/* [한국어] **최대 링크 속도 필드를 떼어 PHY 에 알린다.** generic PHY 드라이버가 그 속도에 맞게 설정하도록 하는 것이다 */
	if (drvdata->ltssm_mask)	/* [한국어] **GPR 로 LTSSM 을 켜는 세대이면** */
		regmap_update_bits(imx_pcie->iomuxc_gpr, drvdata->ltssm_off, drvdata->ltssm_mask,	/* [한국어] 그 비트를 세운다 */
				   drvdata->ltssm_mask);	/* [한국어] 세울 값이 마스크와 같다 — 비트를 켜는 것이다 */

	reset_control_deassert(imx_pcie->apps_reset);	/* [한국어] **"apps" 리셋으로 LTSSM 을 켜는 세대이면** 여기서 풀린다. 없는 세대에서는 NULL 이라 조용히 넘어간다. 세대에 따라 위 GPR 쓰기와 이 줄 중 하나가 실제 스위치다 */
}

/* [한국어]
 * imx_pcie_ltssm_disable - LTSSM 을 꺼 링크를 내린다
 *
 * @dev: 플랫폼 장치. drvdata 에서 imx_pcie 를 되찾는 데 쓴다.
 *
 * imx_pcie_ltssm_enable() 의 짝이며, 같은 세 축을 반대로 조작한다 —
 * PHY 속도를 0 으로 알리고, ltssm_mask 비트를 지우고, apps_reset 을
 * 건다.
 *
 * 부르는 곳이 둘이다. imx_pcie_stop_link() 는 DWC 코어가 링크를 내리라고
 * 할 때 부르고, imx_pcie_host_init() 은 리셋을 풀기 직전에 **LTSSM 이
 * 확실히 꺼진 상태에서 시작하도록** 부른다(그 자리의 상류 주석이 그렇게
 * 적고 있다).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx_pcie_stop_link() / imx_pcie_host_init() -> [이 함수]
 *     -> phy_set_speed() -> regmap_update_bits() -> reset_control_assert()
 */
static void imx_pcie_ltssm_disable(struct device *dev)
{
	struct imx_pcie *imx_pcie = dev_get_drvdata(dev);	/* [한국어] 플랫폼 장치에 심어 둔 이 드라이버 인스턴스 */
	const struct imx_pcie_drvdata *drvdata = imx_pcie->drvdata;	/* [한국어] 세대별 표 항목 */

	phy_set_speed(imx_pcie->phy, 0);	/* [한국어] PHY 에 속도 0 을 알려 링크를 내릴 준비를 시킨다 */
	if (drvdata->ltssm_mask)	/* [한국어] GPR 로 제어하는 세대이면 */
		regmap_update_bits(imx_pcie->iomuxc_gpr, drvdata->ltssm_off,	/* [한국어] 그 비트를 */
				   drvdata->ltssm_mask, 0);	/* [한국어] 지운다 */

	reset_control_assert(imx_pcie->apps_reset);	/* [한국어] "apps" 리셋을 걸어 LTSSM 을 멈춘다 */
}

/* [한국어]
 * imx_pcie_start_link - LTSSM 을 켜 링크를 세우고, 필요하면 속도를 올린다
 *
 * @pci: DWC 코어의 장치 구조체. to_imx_pcie() 로 이 드라이버 인스턴스를
 *       되찾는다.
 * @return: 항상 0. 실패해도 0 을 돌려준다.
 *
 * struct dw_pcie_ops 의 start_link 로 등록되어 DWC 코어가 부른다.
 * 링크를 세우는 방식이 세대에 따라 둘로 갈린다.
 *
 * **IMX_PCIE_FLAG_SPEED_CHANGE_WORKAROUND 가 없는 세대**는 LTSSM 만
 * 켜고 곧바로 돌아간다. 링크 대기와 속도 협상은 DWC 코어가 맡는다.
 *
 * **있는 세대**는 두 단계로 나눠 올린다.
 *   1. 상류 주석이 이유를 밝힌다 — Gen2 로 시작하면 버스의 장치가 아예
 *      검출되지 않을 수 있고, 특히 PCIe 스위치에서 그렇다. 그래서
 *      LNKCAP 의 최대 속도를 강제로 2.5GT/s(Gen1)로 낮춰 쓰고 LTSSM 을
 *      켠다. dbi 의 읽기 전용 필드를 고치는 것이라
 *      dw_pcie_dbi_ro_wr_en/dis 로 감싼다.
 *   2. 장치 트리가 Gen2 이상을 요구했으면(max_link_speed > 1) 링크가
 *      서기를 기다린 뒤, LNKCAP 을 원래 최대 속도로 되돌리고
 *      PORT_LOGIC_SPEED_CHANGE 를 세워 **Directed Speed Change** 를
 *      시작한다. 그러면 양쪽이 지원하는 최선의 속도로 재협상한다.
 *      그 협상이 끝나기를 imx_pcie_wait_for_speed_change() 로 기다린다.
 *
 * 에러 경로: 링크가 안 서거나 속도 변경이 안 끝나면 err_reset_phy 로
 * 가서 PORT_DEBUG0/1 을 디버그 로그로 남기고 PHY 수신단을 흔들어 본 뒤
 * **0 을 돌려준다.** 즉 이 함수는 실패를 호출자에게 알리지 않으며,
 * 링크 유무는 DWC 코어가 따로 확인한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe/resume). 링크 대기와 속도
 * 협상에서 잠든다.
 *
 * 호출 체인:
 *   dw_pcie_host_init()/dw_pcie_ep_init() -> pci->ops->start_link -> [이 함수]
 *     -> imx_pcie_ltssm_enable() -> dw_pcie_wait_for_link()
 *     -> imx_pcie_wait_for_speed_change() -> imx_pcie_reset_phy()
 */
static int imx_pcie_start_link(struct dw_pcie *pci)
{
	struct imx_pcie *imx_pcie = to_imx_pcie(pci);	/* [한국어] DWC 구조체에서 이 드라이버 인스턴스를 되찾는다 */
	struct device *dev = pci->dev;	/* [한국어] 메시지를 낼 장치 */
	u8 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);	/* [한국어] PCI Express capability 의 위치 */
	u32 tmp;	/* [한국어] 읽은 레지스터 값 */
	int ret;	/* [한국어] 하위 호출 결과 */

	if (!(imx_pcie->drvdata->flags &	/* [한국어] **속도 변경 우회가 필요 없는 세대이면** */
	    IMX_PCIE_FLAG_SPEED_CHANGE_WORKAROUND)) {	/* [한국어] 플래그를 확인해 */
		imx_pcie_ltssm_enable(dev);	/* [한국어] LTSSM 만 켜고 */
		return 0;	/* [한국어] 곧바로 나간다. 링크 대기와 속도 협상은 DWC 코어가 맡는다 */
	}

	/*
	 * Force Gen1 operation when starting the link.  In case the link is
	 * started in Gen2 mode, there is a possibility the devices on the
	 * bus will not be detected at all.  This happens with PCIe switches.
	 */
	dw_pcie_dbi_ro_wr_en(pci);	/* [한국어] **LNKCAP 은 읽기 전용 필드라** 쓰기를 허용해야 고칠 수 있다 */
	tmp = dw_pcie_readl_dbi(pci, offset + PCI_EXP_LNKCAP);	/* [한국어] 링크 능력 레지스터를 읽어 */
	tmp &= ~PCI_EXP_LNKCAP_SLS;	/* [한국어] 최대 속도 필드를 지우고 */
	tmp |= PCI_EXP_LNKCAP_SLS_2_5GB;	/* [한국어] **Gen1(2.5GT/s)로 강제한다.** 바로 위 상류 주석이 이유를 밝힌다 — Gen2 로 시작하면 버스의 장치가 아예 검출되지 않을 수 있고 특히 PCIe 스위치에서 그렇다 */
	dw_pcie_writel_dbi(pci, offset + PCI_EXP_LNKCAP, tmp);	/* [한국어] 고친 값을 되쓴다 */
	dw_pcie_dbi_ro_wr_dis(pci);	/* [한국어] 읽기 전용 보호를 되돌린다 */

	/* Start LTSSM. */
	imx_pcie_ltssm_enable(dev);	/* [한국어] **Gen1 상태로 링크 훈련을 시작시킨다** */

	if (pci->max_link_speed > 1) {	/* [한국어] **장치 트리가 Gen2 이상을 요구했으면** 두 번째 단계로 올린다 */
		ret = dw_pcie_wait_for_link(pci);	/* [한국어] 먼저 링크가 서기를 기다린다 */
		if (ret)	/* [한국어] 안 서면 */
			goto err_reset_phy;	/* [한국어] PHY 를 흔들어 보는 경로로 간다 */

		/* Allow faster modes after the link is up */
		dw_pcie_dbi_ro_wr_en(pci);	/* [한국어] 다시 쓰기를 허용하고 */
		tmp = dw_pcie_readl_dbi(pci, offset + PCI_EXP_LNKCAP);	/* [한국어] 링크 능력 레지스터를 읽어 */
		tmp &= ~PCI_EXP_LNKCAP_SLS;	/* [한국어] 최대 속도 필드를 지우고 */
		tmp |= pci->max_link_speed;	/* [한국어] **원래 요구한 최대 속도로 되돌린다** */
		dw_pcie_writel_dbi(pci, offset + PCI_EXP_LNKCAP, tmp);	/* [한국어] 고친 값을 되쓴다 */

		/*
		 * Start Directed Speed Change so the best possible
		 * speed both link partners support can be negotiated.
		 */
		tmp = dw_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);	/* [한국어] 링크 폭·속도 제어 레지스터를 읽어 */
		tmp |= PORT_LOGIC_SPEED_CHANGE;	/* [한국어] **속도 변경 비트를 세운다** — 바로 위 상류 주석대로 양쪽이 지원하는 최선의 속도로 재협상시키는 것이다 */
		dw_pcie_writel_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, tmp);	/* [한국어] 그 값을 되쓴다 */
		dw_pcie_dbi_ro_wr_dis(pci);	/* [한국어] 읽기 전용 보호를 되돌린다 */

		ret = imx_pcie_wait_for_speed_change(imx_pcie);	/* [한국어] 협상이 끝나기를 기다린다 */
		if (ret) {	/* [한국어] 안 끝나면 */
			dev_err(dev, "Failed to bring link up!\n");	/* [한국어] 그 사실을 알리고 */
			goto err_reset_phy;	/* [한국어] PHY 를 흔들어 보는 경로로 간다 */
		}
	} else {
		dev_info(dev, "Link: Only Gen1 is enabled\n");	/* [한국어] 그 사실을 알리고 그대로 둔다 */
	}

	return 0;	/* [한국어] 여기까지 왔으면 성공 */

err_reset_phy:	/* [한국어] **PHY 되살리기 경로** — 링크가 안 서거나 속도 변경이 안 끝났을 때다 */
	dev_dbg(dev, "PHY DEBUG_R0=0x%08x DEBUG_R1=0x%08x\n",	/* [한국어] 포트 디버그 레지스터 둘을 남긴다 */
		dw_pcie_readl_dbi(pci, PCIE_PORT_DEBUG0),	/* [한국어] DEBUG0 */
		dw_pcie_readl_dbi(pci, PCIE_PORT_DEBUG1));	/* [한국어] DEBUG1. LTSSM 이 어느 상태에서 멈췄는지 알 수 있는 값이다 */
	imx_pcie_reset_phy(imx_pcie);	/* [한국어] PHY 수신단을 흔들어 본다 */
	return 0;	/* [한국어] **실패했어도 0 을 돌려준다.** 링크 유무는 DWC 코어가 따로 확인한다 */
}

/* [한국어]
 * imx_pcie_stop_link - 링크를 내린다
 *
 * @pci: DWC 코어의 장치 구조체.
 *
 * struct dw_pcie_ops 의 stop_link 로 등록되어 DWC 코어가 부른다.
 * 상류 주석대로 LTSSM 을 끄는 것이 전부다 — 클럭이나 전원은 건드리지
 * 않는다. 그것은 host_exit 나 서스펜드 경로의 몫이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   DWC 코어 -> pci->ops->stop_link -> [이 함수] -> imx_pcie_ltssm_disable()
 */
static void imx_pcie_stop_link(struct dw_pcie *pci)
{
	struct device *dev = pci->dev;	/* [한국어] LTSSM 제어 함수가 device 를 받으므로 꺼내 둔다 */

	/* Turn off PCIe LTSSM */
	imx_pcie_ltssm_disable(dev);	/* [한국어] 바로 위 상류 주석대로 LTSSM 만 끈다. 클럭이나 전원은 건드리지 않는다 */
}

/* [한국어]
 * imx_pcie_add_lut - i.MX95 의 RID -> StreamID 변환표에 한 칸을 채운다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @rid: 이 장치의 Requester ID(버스·장치·함수를 합친 16비트 식별자).
 * @sid: 그 RID 에 대응시킬 StreamID(6비트).
 * @return: 0 이면 성공(이미 같은 RID 항목이 있어 아무 일도 하지 않은 경우
 *          포함), sid 가 범위를 넘으면 -EINVAL, 빈 칸이 없으면 -ENOSPC.
 *
 * IMX_PCIE_FLAG_HAS_LUT 세대(i.MX95)에만 있다. SMMU/MSI 글루가 트랜잭션의
 * 주인을 가릴 때 쓰는 StreamID 를 RID 별로 정해 주는 표이며, 칸이
 * IMX95_MAX_LUT(32)개뿐이라 장치가 붙을 때 잡고 떨어질 때 놓아야 한다.
 *
 * StreamID 는 6비트라 64 이상은 넣을 수 없다 — 그것이 첫 검사다.
 *
 * **표 접근은 간접 방식**이다. ACSCTRL 에 (읽기 방향 비트 | 인덱스) 를 쓰면
 * 그 칸의 내용이 DATA1/DATA2 에 나타나고, 쓸 때는 DATA1/DATA2 를 먼저
 * 채운 뒤 ACSCTRL 에 인덱스만(RWA 없이) 쓴다.
 *
 * 한 번의 순회로 두 가지를 함께 한다 — 바로 위 상류 주석이 그 의도를
 * 밝힌다. 유효하지 않은 칸을 만나면 첫 번째 것을 빈 칸 후보로 기억해 두고,
 * 유효한 칸이면 RID 가 겹치는지 본다. 겹치면 경고만 남기고 0 으로 나간다.
 *
 * 채울 값의 구성:
 *   DATA1 — DAC_ID 0, STREAM_ID 에 sid, 그리고 유효 비트(VLD).
 *   DATA2 — REQID 에 rid, 그리고 마스크. **마스크가 모드에 따라 다르다** —
 *           EP 모드에서는 0x7 로 두어 상류 주석대로 Device ID 만 비교하고,
 *           RC 모드에서는 IMX95_PE0_LUT_MASK 로 두어 RID 전 비트를 비교한다.
 *
 * 동기화: guard(mutex)(&imx_pcie->lock) 로 함수 전체를 잠근다. 상류 주석이
 * 구조체 정의 자리에서 이유를 밝힌다 — 어느 순간에도 한 장치의 LUT 만
 * 설정되도록 하기 위해서다. 컨트롤러가 둘이어도 표는 하나이고, ACSCTRL 에
 * 인덱스를 쓴 뒤 DATA 를 읽는 두 단계 사이에 다른 흐름이 끼어들면 엉뚱한
 * 칸을 읽게 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. PCI 열거 중 장치가 나타날 때 불린다.
 *
 * 호출 체인:
 *   imx_pcie_add_lut_by_rid() -> [이 함수] -> regmap_write()/regmap_read()
 */
static int imx_pcie_add_lut(struct imx_pcie *imx_pcie, u16 rid, u8 sid)
{
	struct dw_pcie *pci = imx_pcie->pci;	/* [한국어] 오류 메시지를 낼 장치를 얻기 위한 DWC 구조체 */
	struct device *dev = pci->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	u32 data1, data2;	/* [한국어] LUT 한 칸의 두 워드 */
	int free = -1;	/* [한국어] 찾아낸 빈 칸의 인덱스. -1 은 아직 못 찾았다는 뜻이다 */
	int i;	/* [한국어] 칸 순회 인덱스 */

	if (sid >= 64) {	/* [한국어] **StreamID 는 6비트뿐이라 64 이상은 넣을 수 없다** */
		dev_err(dev, "Invalid SID for index %d\n", sid);	/* [한국어] 그 사실을 알리고 */
		return -EINVAL;	/* [한국어] 인자 오류로 돌아간다 */
	}

	guard(mutex)(&imx_pcie->lock);	/* [한국어] **함수 전체를 뮤텍스로 잠근다.** ACSCTRL 에 인덱스를 쓴 뒤 DATA 를 읽는 두 단계 사이에 다른 흐름이 끼어들면 엉뚱한 칸을 다루게 된다 */

	/*
	 * Iterate through all LUT entries to check for duplicate RID and
	 * identify the first available entry. Configure this available entry
	 * immediately after verification to avoid rescanning it.
	 */
	for (i = 0; i < IMX95_MAX_LUT; i++) {	/* [한국어] 32칸을 모두 훑는다. 바로 위 상류 주석대로 중복 RID 확인과 빈 칸 찾기를 한 번의 순회로 함께 한다 */
		regmap_write(imx_pcie->iomuxc_gpr,	/* [한국어] **읽기 방향 비트와 인덱스를 써서 그 칸을 고른다** */
			     IMX95_PE0_LUT_ACSCTRL, IMX95_PEO_LUT_RWA | i);	/* [한국어] RWA 가 읽기 방향이고 i 가 칸 번호다 */
		regmap_read(imx_pcie->iomuxc_gpr, IMX95_PE0_LUT_DATA1, &data1);	/* [한국어] 그 칸의 첫 워드를 읽는다 */

		if (!(data1 & IMX95_PE0_LUT_VLD)) {	/* [한국어] 유효 비트가 없으면 빈 칸이다 */
			if (free < 0)	/* [한국어] 아직 빈 칸을 못 찾았으면 */
				free = i;	/* [한국어] 이 칸을 후보로 기억한다 — 첫 번째 빈 칸을 쓴다 */
			continue;	/* [한국어] 빈 칸은 중복 검사를 할 것이 없으므로 넘어간다 */
		}

		regmap_read(imx_pcie->iomuxc_gpr, IMX95_PE0_LUT_DATA2, &data2);	/* [한국어] 유효한 칸이면 둘째 워드도 읽는다 */

		/* Do not add duplicate RID */
		if (rid == FIELD_GET(IMX95_PE0_LUT_REQID, data2)) {	/* [한국어] **같은 RID 가 이미 있으면** */
			dev_warn(dev, "Existing LUT entry available for RID (%d)", rid);	/* [한국어] 그 사실을 경고로 남기고 */
			return 0;	/* [한국어] 아무것도 하지 않고 성공으로 나간다 */
		}
	}

	if (free < 0) {	/* [한국어] **빈 칸을 하나도 못 찾았으면** */
		dev_err(dev, "LUT entry is not available\n");	/* [한국어] 표가 꽉 찼다고 알리고 */
		return -ENOSPC;	/* [한국어] 공간 없음으로 돌아간다. 표가 32칸뿐이라 실제로 일어날 수 있는 일이다 */
	}

	data1 = FIELD_PREP(IMX95_PE0_LUT_DAC_ID, 0);	/* [한국어] DAC ID 는 늘 0 을 넣는다 */
	data1 |= FIELD_PREP(IMX95_PE0_LUT_STREAM_ID, sid);	/* [한국어] **StreamID 필드에 sid 를 넣고** */
	data1 |= IMX95_PE0_LUT_VLD;	/* [한국어] 유효 비트를 세운다 */
	regmap_write(imx_pcie->iomuxc_gpr, IMX95_PE0_LUT_DATA1, data1);	/* [한국어] 첫 워드를 쓴다 */

	if (imx_pcie->drvdata->mode == DW_PCIE_EP_TYPE)	/* [한국어] **엔드포인트 모드이면** */
		data2 = 0x7;	/* In the EP mode, only 'Device ID' is required */ /* [한국어] 옆 상류 주석대로 Device ID 만 비교하도록 마스크를 0x7 로 둔다 */
	else
		data2 = IMX95_PE0_LUT_MASK;	/* Match all bits of RID */ /* [한국어] 옆 상류 주석대로 RID 전 비트를 비교하도록 전체 마스크를 둔다 */
	data2 |= FIELD_PREP(IMX95_PE0_LUT_REQID, rid);	/* [한국어] 그 위에 RID 를 얹는다 */
	regmap_write(imx_pcie->iomuxc_gpr, IMX95_PE0_LUT_DATA2, data2);	/* [한국어] 둘째 워드를 쓴다 */

	regmap_write(imx_pcie->iomuxc_gpr, IMX95_PE0_LUT_ACSCTRL, free);	/* [한국어] **읽기 방향 비트 없이 인덱스만 써서 두 워드를 그 칸에 반영한다** */

	return 0;	/* [한국어] 칸을 채웠으면 성공 */
}

/* [한국어]
 * imx_pcie_remove_lut - RID 로 LUT 항목을 찾아 지운다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @rid: 지울 항목의 Requester ID.
 *
 * 장치가 떨어질 때 그 장치가 차지하던 LUT 칸을 놓아 준다. 표가 32칸뿐이라
 * 놓지 않으면 곧 -ENOSPC 가 난다.
 *
 * 32칸을 훑으며 DATA2 의 REQID 필드가 rid 와 같은 칸을 찾는다. 찾으면
 * DATA1 과 DATA2 를 모두 0 으로 쓰고 ACSCTRL 에 인덱스를 써서 반영한 뒤
 * 곧바로 멈춘다 — 같은 RID 가 둘 있을 수 없기 때문이다(추가 쪽에서
 * 중복을 막는다).
 *
 * **DATA1 을 0 으로 쓰면 유효 비트도 함께 지워진다.** 그것이 이 칸이
 * 비었다는 표시가 된다.
 *
 * [관찰] 유효 비트를 보지 않고 REQID 만 비교한다. 무효한 칸에 남아 있는
 * 쓰레기 값이 우연히 rid 와 같으면 그 칸을 지우게 되는데, 지우는 것이
 * 0 으로 쓰는 것이라 결과는 같다.
 *
 * 동기화: imx_pcie_add_lut() 과 같은 뮤텍스로 함수 전체를 잠근다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. PCI 장치가 제거될 때 불린다.
 *
 * 호출 체인:
 *   imx_pcie_disable_device() -> [이 함수] -> regmap_write()/regmap_read()
 */
static void imx_pcie_remove_lut(struct imx_pcie *imx_pcie, u16 rid)
{
	u32 data2;	/* [한국어] 읽어 볼 둘째 워드 */
	int i;	/* [한국어] 칸 순회 인덱스 */

	guard(mutex)(&imx_pcie->lock);	/* [한국어] 추가 쪽과 같은 뮤텍스로 함수 전체를 잠근다 */

	for (i = 0; i < IMX95_MAX_LUT; i++) {	/* [한국어] 32칸을 훑는다 */
		regmap_write(imx_pcie->iomuxc_gpr,	/* [한국어] **읽기 방향 비트와 인덱스로 그 칸을 고른다** */
			     IMX95_PE0_LUT_ACSCTRL, IMX95_PEO_LUT_RWA | i);	/* [한국어] RWA 가 읽기 방향이고 i 가 칸 번호다 */
		regmap_read(imx_pcie->iomuxc_gpr, IMX95_PE0_LUT_DATA2, &data2);	/* [한국어] 둘째 워드를 읽는다 — RID 가 그 안에 있다 */
		if (FIELD_GET(IMX95_PE0_LUT_REQID, data2) == rid) {	/* [한국어] **RID 가 맞으면 이 칸이다.** 유효 비트는 보지 않지만, 지우는 것이 0 으로 쓰는 것이라 결과는 같다 */
			regmap_write(imx_pcie->iomuxc_gpr,	/* [한국어] 첫 워드를 */
				     IMX95_PE0_LUT_DATA1, 0);	/* [한국어] 0 으로 쓴다 — **유효 비트도 함께 지워져 빈 칸이 된다** */
			regmap_write(imx_pcie->iomuxc_gpr,	/* [한국어] 둘째 워드도 */
				     IMX95_PE0_LUT_DATA2, 0);	/* [한국어] 0 으로 쓰고 */
			regmap_write(imx_pcie->iomuxc_gpr,	/* [한국어] 인덱스만 써서 */
				     IMX95_PE0_LUT_ACSCTRL, i);	/* [한국어] 두 워드를 그 칸에 반영한다 */

			break;	/* [한국어] 같은 RID 가 둘 있을 수 없으므로 곧바로 멈춘다 */
		}
	}
}

/* [한국어]
 * imx_pcie_add_lut_by_rid - 장치 트리의 iommu-map/msi-map 을 보고 StreamID 를 정한 뒤 LUT 에 넣는다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @rid: 이 장치의 Requester ID.
 * @return: 0 이면 성공(넣을 필요가 없는 경우 포함), 짝이 맞지 않거나
 *          하드웨어가 감당 못 하는 구성이면 -EINVAL, 그 밖은 하위 호출의 코드.
 *
 * StreamID 를 어디서 얻을지 정하는 것이 이 함수의 일이다. 출처가 둘 있다 —
 * 장치 트리의 iommu-map(SMMU 용)과 msi-map(ITS 용)이며, of_map_id() 로
 * 각각 조회한다.
 *
 * **of_map_id() 의 결과 해석이 까다롭다.** 상류 주석이 표로 정리해 두었다.
 * target 이 NULL 인데 오류도 없으면 RID 가 맵 범위 밖이라는 뜻이고, 그
 * 경우 1:1 매핑이 되어야 하는데 StreamID 가 6비트뿐이라 하드웨어가
 * 감당할 수 없다. 그래서 iommu-map 쪽은 그 경우를 -EINVAL 로 바꿔 두고,
 * msi-map 쪽은 그 조합을 만나면 곧바로 -EINVAL 로 나간다.
 *
 * 두 맵의 유무 조합에 따라 하는 일이 갈린다 — 이것도 상류 주석에 표로
 * 있다. 둘 다 없으면 DWC 내장 MSI 컨트롤러를 쓰고 SMMU 도 없으므로
 * LUT 가 필요 없어 0 으로 나간다. 둘 다 있으면 ITS 와 SMMU 를 함께 쓰는
 * 구성이라 **두 StreamID 가 같아야 한다.**
 *
 * 같은지 비교할 때 마스크를 씌우는 이유를 상류 주석의 그림이 밝힌다 —
 * MSI 글루 층이 StreamID 앞에 2비트 컨트롤러 ID(00 PCIe0, 01 ENETC,
 * 10 PCIe1)를 자동으로 붙이는 반면 IOMMU 글루 층은 붙이지 않는다.
 * 그래서 msi-map 쪽 값에서 IMX95_SID_MASK 로 하위 6비트만 떼어 비교한다.
 *
 * 마지막으로 있는 쪽의 값을 sid 로 골라 imx_pcie_add_lut() 에 넘긴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   imx_pcie_enable_device() / imx_pcie_probe()(EP 경로) -> [이 함수]
 *     -> of_map_id() -> imx_pcie_add_lut()
 */
static int imx_pcie_add_lut_by_rid(struct imx_pcie *imx_pcie, u32 rid)
{
	struct device *dev = imx_pcie->pci->dev;	/* [한국어] 오류 메시지를 낼 장치이자 of_node 의 출처 */
	struct device_node *target;	/* [한국어] of_map_id() 가 채워 줄 대상 노드(SMMU 또는 ITS) */
	u32 sid_i, sid_m;	/* [한국어] iommu-map 과 msi-map 에서 각각 얻은 StreamID */
	int err_i, err_m;	/* [한국어] 두 조회의 결과 코드 */
	u32 sid = 0;	/* [한국어] 최종적으로 LUT 에 넣을 StreamID */

	target = NULL;	/* [한국어] 조회 전에 비워 둔다 — 채워졌는지로 결과를 가르기 때문이다 */
	err_i = of_map_id(dev->of_node, rid, "iommu-map", "iommu-map-mask",	/* [한국어] **iommu-map 에서 이 RID 에 대응하는 StreamID 를 찾는다** */
			  &target, &sid_i);	/* [한국어] 대상 노드와 값을 받을 자리 */
	if (target) {	/* [한국어] 대상 노드가 채워졌으면 유효한 매핑이다 */
		of_node_put(target);	/* [한국어] 참조를 놓아 준다 */
	} else {
		/*
		 * "target == NULL && err_i == 0" means RID out of map range.
		 * Use 1:1 map RID to streamID. Hardware can't support this
		 * because the streamID is only 6 bits
		 */
		err_i = -EINVAL;	/* [한국어] **바로 위 상류 주석대로** target 이 NULL 인데 오류도 없으면 RID 가 맵 범위 밖이라는 뜻이고, 1:1 매핑이 되어야 하는데 StreamID 가 6비트뿐이라 하드웨어가 감당할 수 없다. 그래서 오류로 바꿔 둔다 */
	}

	target = NULL;	/* [한국어] 다시 비우고 */
	err_m = of_map_id(dev->of_node, rid, "msi-map", "msi-map-mask",	/* [한국어] **msi-map 에서도 찾는다** */
			  &target, &sid_m);	/* [한국어] 대상 노드와 값을 받을 자리 */

	/*
	 *   err_m      target
	 *	0	NULL		RID out of range. Use 1:1 map RID to
	 *				streamID, Current hardware can't
	 *				support it, so return -EINVAL.
	 *      != 0    NULL		msi-map does not exist, use built-in MSI
	 *	0	!= NULL		Get correct streamID from RID
	 *	!= 0	!= NULL		Invalid combination
	 */
	if (!err_m && !target)	/* [한국어] **바로 위 상류 주석의 표대로** 오류가 없는데 대상도 없으면 RID 가 범위 밖이라 1:1 매핑이 필요한 경우다 */
		return -EINVAL;	/* [한국어] 하드웨어가 감당 못 하므로 인자 오류로 돌아간다 */
	else if (target)	/* [한국어] 대상이 채워졌으면 유효한 매핑이다 */
		of_node_put(target);	/* Find streamID map entry for RID in msi-map */ /* [한국어] 참조를 놓아 준다 */

	/*
	 * msi-map        iommu-map
	 *   N                N            DWC MSI Ctrl
	 *   Y                Y            ITS + SMMU, require the same SID
	 *   Y                N            ITS
	 *   N                Y            DWC MSI Ctrl + SMMU
	 */
	if (err_i && err_m)	/* [한국어] **둘 다 없으면** 바로 위 상류 주석의 표대로 DWC 내장 MSI 컨트롤러를 쓰고 SMMU 도 없는 구성이라 LUT 가 필요 없다 */
		return 0;	/* [한국어] 아무것도 하지 않고 성공으로 나간다 */

	if (!err_i && !err_m) {	/* [한국어] **둘 다 있으면** ITS 와 SMMU 를 함께 쓰는 구성이라 두 StreamID 가 같아야 한다 */
		/*
		 *	    Glue Layer
		 *          <==========>
		 * ┌─────┐                  ┌──────────┐
		 * │ LUT │ 6-bit streamID   │          │
		 * │     │─────────────────►│  MSI     │
		 * └─────┘   2-bit ctrl ID  │          │
		 *             ┌───────────►│          │
		 *  (i.MX95)   │            │          │
		 *  00 PCIe0   │            │          │
		 *  01 ENETC   │            │          │
		 *  10 PCIe1   │            │          │
		 *             │            └──────────┘
		 * The MSI glue layer auto adds 2 bits controller ID ahead of
		 * streamID, so mask these 2 bits to get streamID. The
		 * IOMMU glue layer doesn't do that.
		 */
		if (sid_i != (sid_m & IMX95_SID_MASK)) {	/* [한국어] **마스크를 씌워 비교한다.** 바로 위 상류 주석의 그림대로 MSI 글루 층이 StreamID 앞에 2비트 컨트롤러 ID(00 PCIe0, 01 ENETC, 10 PCIe1)를 자동으로 붙이는 반면 IOMMU 글루 층은 붙이지 않기 때문이다 */
			dev_err(dev, "iommu-map and msi-map entries mismatch!\n");	/* [한국어] 어긋나면 장치 트리가 잘못된 것이라 알리고 */
			return -EINVAL;	/* [한국어] 인자 오류로 돌아간다 */
		}
	}

	if (!err_i)	/* [한국어] iommu-map 쪽이 유효하면 */
		sid = sid_i;	/* [한국어] 그 값을 쓰고 */
	else if (!err_m)	/* [한국어] 아니고 msi-map 쪽이 유효하면 */
		sid = sid_m & IMX95_SID_MASK;	/* [한국어] 컨트롤러 ID 를 떼어 낸 값을 쓴다 */

	return imx_pcie_add_lut(imx_pcie, rid, sid);	/* [한국어] 정한 StreamID 로 LUT 에 한 칸을 채우고 그 결과를 그대로 돌려준다 */
}

/* [한국어]
 * imx_pcie_enable_device - PCI 코어가 장치를 켤 때 그 장치의 LUT 칸을 잡는다
 *
 * @bridge: 호스트 브리지. sysdata 를 거슬러 이 드라이버 인스턴스를 얻는다.
 * @pdev:   방금 나타난 PCI 장치.
 * @return: 0 이면 성공, 실패면 imx_pcie_add_lut_by_rid() 의 오류 코드.
 *
 * struct pci_host_bridge 의 enable_device 콜백이다. imx_pcie_host_init()
 * 이 IMX_PCIE_FLAG_HAS_LUT 세대에서만 이 콜백을 매단다 — 다른 세대에는
 * LUT 가 없어 잡을 것이 없기 때문이다.
 *
 * pci_dev_id() 가 (버스 << 8 | devfn) 형태의 RID 를 만들어 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 열거).
 *
 * 호출 체인:
 *   PCI 코어 -> bridge->enable_device -> [이 함수]
 *     -> imx_pcie_add_lut_by_rid()
 */
static int imx_pcie_enable_device(struct pci_host_bridge *bridge, struct pci_dev *pdev)
{
	struct imx_pcie *imx_pcie = to_imx_pcie(to_dw_pcie_from_pp(bridge->sysdata));	/* [한국어] **호스트 브리지의 sysdata 를 거슬러 이 드라이버 인스턴스를 되찾는다** — sysdata 는 dw_pcie_rp 이고 거기서 dw_pcie 로, 다시 imx_pcie 로 간다 */

	return imx_pcie_add_lut_by_rid(imx_pcie, pci_dev_id(pdev));	/* [한국어] pci_dev_id() 가 (버스 << 8 | devfn) 형태의 RID 를 만들어 준다 */
}

/* [한국어]
 * imx_pcie_disable_device - PCI 코어가 장치를 끌 때 그 장치의 LUT 칸을 놓는다
 *
 * @bridge: 호스트 브리지.
 * @pdev:   사라지는 PCI 장치.
 *
 * imx_pcie_enable_device() 의 짝이며 struct pci_host_bridge 의
 * disable_device 콜백이다. 반환값이 없어 실패를 알릴 수 없는데, 지우는
 * 쪽은 실패할 일이 없다(항목을 못 찾으면 조용히 넘어간다).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 장치 제거).
 *
 * 호출 체인:
 *   PCI 코어 -> bridge->disable_device -> [이 함수] -> imx_pcie_remove_lut()
 */
static void imx_pcie_disable_device(struct pci_host_bridge *bridge,
				    struct pci_dev *pdev)
{
	struct imx_pcie *imx_pcie;	/* [한국어] 되찾을 이 드라이버 인스턴스 */

	imx_pcie = to_imx_pcie(to_dw_pcie_from_pp(bridge->sysdata));	/* [한국어] enable 쪽과 같은 방식으로 sysdata 를 거슬러 올라간다 */
	imx_pcie_remove_lut(imx_pcie, pci_dev_id(pdev));	/* [한국어] 같은 RID 로 LUT 칸을 놓는다. 반환값이 없어 실패를 알릴 수 없지만 지우는 쪽은 실패할 일이 없다 */
}

/* [한국어]
 * imx_pcie_assert_perst - PERST# 리셋 GPIO 를 걸거나 풀며 규격이 정한 시간을 지킨다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @assert:   true 면 리셋을 걸고, false 면 푼다.
 *
 * PERST# 는 슬롯의 카드를 리셋하는 신호다. 보드에 그 GPIO 가 없으면
 * reset_gpiod 가 NULL 이고, 걸 때는 gpiod_set_value_cansleep() 이 조용히
 * 넘어가며 풀 때는 아예 아무 일도 하지 않는다.
 *
 * **푸는 쪽에만 두 번의 대기가 있다.**
 *   PCIE_T_PVPERL_MS  — 규격이 정한, 전원이 안정된 뒤 PERST# 를 풀기까지
 *                       기다려야 하는 시간. 풀기 전에 지킨다.
 *   PCIE_RESET_CONFIG_WAIT_MS — PERST# 를 푼 뒤 카드가 config 접근에
 *                       응답할 수 있게 되기까지 기다리는 시간.
 * 두 상수 모두 ../../pci.h 에 있다. 이 대기를 건너뛰면 곧바로 이어지는
 * 링크 훈련이나 config 읽기가 실패할 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 잠들며, GPIO 조작도
 * 잠들 수 있는 판(cansleep)을 쓴다 — I2C 확장기에 물린 GPIO 일 수 있기
 * 때문이다.
 *
 * 호출 체인:
 *   imx_pcie_host_init() / imx_pcie_suspend_noirq() /
 *   imx_pcie_resume_noirq() / imx_pcie_shutdown() -> [이 함수]
 */
static void imx_pcie_assert_perst(struct imx_pcie *imx_pcie, bool assert)
{
	if (assert) {	/* [한국어] 리셋을 거는 경우 */
		gpiod_set_value_cansleep(imx_pcie->reset_gpiod, 1);	/* [한국어] **GPIO 를 1 로 만들어 PERST# 를 어서트한다.** GPIO 가 없으면 이 호출이 조용히 넘어간다 */
	} else {
		if (imx_pcie->reset_gpiod) {	/* [한국어] **GPIO 가 실제로 있을 때만** 아래 대기를 지킨다 */
			msleep(PCIE_T_PVPERL_MS);	/* [한국어] 규격이 정한, 전원이 안정된 뒤 PERST# 를 풀기까지 기다려야 하는 시간 */
			gpiod_set_value_cansleep(imx_pcie->reset_gpiod, 0);	/* [한국어] PERST# 를 해제한다 */
			msleep(PCIE_RESET_CONFIG_WAIT_MS);	/* [한국어] 푼 뒤 카드가 config 접근에 응답할 수 있게 되기까지 기다린다. 이 대기를 건너뛰면 곧 이어지는 링크 훈련이나 config 읽기가 실패할 수 있다 */
		}
	}
}

/* [한국어]
 * imx_pcie_host_init - RC 초기화 본체. 전원·리셋·PHY·클럭 순서를 밟는다
 *
 * @pp: DWC 코어의 루트 포트 구조체.
 * @return: 0 이면 성공. 단계마다 다른 오류 코드를 돌려주며, 실패 시
 *          그때까지 켠 것을 역순으로 되돌린다.
 *
 * struct dw_pcie_host_ops 의 init 로 등록되어 dw_pcie_host_init() 이
 * 부른다. EP 경로에서도 imx_add_pcie_ep() 가 직접 부른다 — 하드웨어를
 * 켜는 절차는 RC 든 EP 든 같기 때문이다.
 *
 * **순서 자체가 이 함수의 내용**이다.
 *   1. vpcie 레귤레이터를 켠다(있으면). 전원이 먼저다.
 *   2. LUT 세대이면 호스트 브리지에 enable/disable_device 콜백을 매단다.
 *   3. **코어 리셋과 PERST# 를 건다.** 아래 설정을 리셋 상태에서 한다.
 *   4. 세대별 init_phy 로 PHY 파라미터와 클럭 경로를 정한다.
 *   5. RC/EP 모드를 GPR 에 적는다.
 *   6. 클럭을 켠다. 여기서 세대별 레퍼런스 클럭 활성화도 함께 일어난다.
 *   7. generic PHY 가 있으면 phy_init -> phy_set_mode_ext -> phy_power_on.
 *      모드는 drvdata->mode 를 보고 PHY_MODE_PCIE_EP 또는 _RC 로 넘긴다.
 *   8. **LTSSM 이 꺼져 있는지 확실히 한다**(그 자리의 상류 주석이 밝힌다).
 *   9. 코어 리셋과 PERST# 를 푼다. 여기서 카드가 깨어난다.
 *  10. 세대별 wait_pll_lock 이 있으면 PLL 이 잠기기를 기다린다.
 *  11. imx_setup_phy_mpll() 로 레퍼런스 클럭 주파수에 맞춰 MPLL 을 맞춘다.
 *
 * 에러 경로가 네 라벨로 계단을 이룬다 — err_phy_off -> err_phy_exit ->
 * err_clk_disable -> err_reg_disable. 실패 지점에 따라 되돌릴 깊이가
 * 다르기 때문이며, 각 라벨은 자기 것만 되돌리고 아래로 떨어진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe/resume). 여러 단계에서 잠든다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() -> pp->ops->init -> [이 함수]
 *   imx_add_pcie_ep() -> [이 함수]
 *     -> imx_pcie_assert_core_reset() -> drvdata->init_phy()
 *     -> imx_pcie_configure_type() -> imx_pcie_clk_enable() -> phy_power_on()
 *     -> imx_pcie_deassert_core_reset() -> imx_setup_phy_mpll()
 */
static int imx_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* [한국어] DWC 루트 포트에서 장치 구조체를 얻는다 */
	struct device *dev = pci->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	struct imx_pcie *imx_pcie = to_imx_pcie(pci);	/* [한국어] 이 드라이버 인스턴스 */
	int ret;	/* [한국어] 하위 호출 결과 */

	if (imx_pcie->vpcie) {	/* [한국어] **1단계 전원.** 슬롯 레귤레이터가 있으면 */
		ret = regulator_enable(imx_pcie->vpcie);	/* [한국어] 켠다 */
		if (ret) {	/* [한국어] 실패하면 */
			dev_err(dev, "failed to enable vpcie regulator: %d\n",	/* [한국어] 그 사실을 알리고 */
				ret);
			return ret;	/* [한국어] 그 코드를 돌려준다. 아직 되돌릴 것이 없다 */
		}
	}

	if (pp->bridge && imx_check_flag(imx_pcie, IMX_PCIE_FLAG_HAS_LUT)) {	/* [한국어] **2단계.** 브리지가 있고 LUT 세대이면 */
		pp->bridge->enable_device = imx_pcie_enable_device;	/* [한국어] 장치가 나타날 때 LUT 칸을 잡는 콜백과 */
		pp->bridge->disable_device = imx_pcie_disable_device;	/* [한국어] 사라질 때 놓는 콜백을 매단다 */
	}

	imx_pcie_assert_core_reset(imx_pcie);	/* [한국어] **3단계 리셋.** 아래 설정을 리셋 상태에서 하기 위해 먼저 건다 */
	imx_pcie_assert_perst(imx_pcie, true);	/* [한국어] PERST# 도 어서트해 카드를 리셋 상태로 둔다 */

	if (imx_pcie->drvdata->init_phy)	/* [한국어] **4단계 PHY.** 세대별 초기화가 있으면 */
		imx_pcie->drvdata->init_phy(imx_pcie);	/* [한국어] 부른다. generic PHY 드라이버에 맡기는 세대는 NULL 이라 건너뛴다 */

	imx_pcie_configure_type(imx_pcie);	/* [한국어] **5단계** RC 인지 EP 인지를 GPR 에 적는다 */

	ret = imx_pcie_clk_enable(imx_pcie);	/* [한국어] **6단계 클럭.** 여기서 세대별 레퍼런스 클럭 활성화도 함께 일어난다 */
	if (ret) {	/* [한국어] 실패하면 */
		dev_err(dev, "unable to enable pcie clocks: %d\n", ret);	/* [한국어] 그 사실을 알리고 */
		goto err_reg_disable;	/* [한국어] 레귤레이터를 되돌리는 라벨로 간다 */
	}

	if (imx_pcie->phy) {	/* [한국어] **7단계 generic PHY.** 있는 세대에서만 */
		ret = phy_init(imx_pcie->phy);	/* [한국어] PHY 를 초기화하고 */
		if (ret) {	/* [한국어] 실패하면 */
			dev_err(dev, "pcie PHY power up failed\n");	/* [한국어] 그 사실을 알리고 */
			goto err_clk_disable;	/* [한국어] 클럭까지 되돌리는 라벨로 간다 */
		}

		ret = phy_set_mode_ext(imx_pcie->phy, PHY_MODE_PCIE,	/* [한국어] **PHY 에 PCIe 모드를 알린다** */
				       imx_pcie->drvdata->mode == DW_PCIE_EP_TYPE ?	/* [한국어] 표가 이 항목을 엔드포인트로 지정했으면 */
						PHY_MODE_PCIE_EP : PHY_MODE_PCIE_RC);	/* [한국어] EP 모드를, 아니면 RC 모드를 넘긴다 */
		if (ret) {	/* [한국어] 실패하면 */
			dev_err(dev, "unable to set PCIe PHY mode\n");	/* [한국어] 그 사실을 알리고 */
			goto err_phy_exit;	/* [한국어] PHY 초기화까지 되돌리는 라벨로 간다 */
		}

		ret = phy_power_on(imx_pcie->phy);	/* [한국어] PHY 전원을 넣는다 */
		if (ret) {	/* [한국어] 실패하면 */
			dev_err(dev, "waiting for PHY ready timeout!\n");	/* [한국어] 그 사실을 알리고 */
			goto err_phy_exit;	/* [한국어] PHY 초기화까지 되돌리는 라벨로 간다 */
		}
	}

	/* Make sure that PCIe LTSSM is cleared */
	imx_pcie_ltssm_disable(dev);	/* [한국어] **8단계.** 바로 위 상류 주석대로 LTSSM 이 확실히 꺼진 상태에서 시작하게 한다 */

	imx_pcie_deassert_core_reset(imx_pcie);	/* [한국어] **9단계 리셋 해제.** 여기서 하드웨어가 깨어난다 */
	imx_pcie_assert_perst(imx_pcie, false);	/* [한국어] PERST# 도 해제한다. 그 안에서 규격 대기가 지켜진다 */

	if (imx_pcie->drvdata->wait_pll_lock) {	/* [한국어] **10단계.** PLL 잠금을 기다려야 하는 세대이면 */
		ret = imx_pcie->drvdata->wait_pll_lock(imx_pcie);	/* [한국어] 기다린다 */
		if (ret < 0)	/* [한국어] 못 잠기면 */
			goto err_phy_off;	/* [한국어] PHY 전원까지 되돌리는 라벨로 간다 */
	}

	imx_setup_phy_mpll(imx_pcie);	/* [한국어] **11단계** 레퍼런스 클럭 주파수에 맞춰 MPLL 을 맞춘다. 반환값은 보지 않는다 */

	return 0;	/* [한국어] 열한 단계를 모두 지났으면 성공 */

err_phy_off:	/* [한국어] **PHY 전원까지 되돌리는 경로** */
	phy_power_off(imx_pcie->phy);	/* [한국어] PHY 전원을 내리고 아래로 이어진다 */
err_phy_exit:	/* [한국어] **PHY 초기화까지 되돌리는 경로** */
	phy_exit(imx_pcie->phy);	/* [한국어] PHY 를 해제하고 아래로 이어진다 */
err_clk_disable:	/* [한국어] **클럭까지 되돌리는 경로** */
	imx_pcie_clk_disable(imx_pcie);	/* [한국어] 클럭을 내리고 아래로 이어진다 */
err_reg_disable:	/* [한국어] **레귤레이터까지 되돌리는 경로** */
	if (imx_pcie->vpcie)	/* [한국어] 슬롯 레귤레이터가 있으면 */
		regulator_disable(imx_pcie->vpcie);	/* [한국어] 끈다 */
	return ret;	/* [한국어] 담아 둔 오류 코드를 돌려준다 */
}

/* [한국어]
 * imx_pcie_host_exit - RC 초기화가 켠 것을 역순으로 끈다
 *
 * @pp: DWC 코어의 루트 포트 구조체.
 *
 * struct dw_pcie_host_ops 의 deinit 로 등록된다. imx_pcie_host_init() 의
 * 짝이며 순서가 반대다 — PHY 를 내리고, 클럭을 끄고, 마지막에 레귤레이터를
 * 끈다.
 *
 * phy_power_off() 의 실패만 오류로 남기고 계속 진행한다. 정리 경로라
 * 중간에 멈출 이유가 없기 때문이다.
 *
 * **리셋은 다시 걸지 않는다.** 그것은 imx_pcie_shutdown() 이 재부팅 대비로
 * 따로 하는 일이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(초기화 실패 경로, 서스펜드).
 *
 * 호출 체인:
 *   dw_pcie_host_init() 실패 경로 / dw_pcie_suspend_noirq()
 *     -> pp->ops->deinit -> [이 함수]
 *     -> phy_power_off() -> phy_exit() -> imx_pcie_clk_disable()
 */
static void imx_pcie_host_exit(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* [한국어] DWC 루트 포트에서 장치 구조체를 얻는다 */
	struct imx_pcie *imx_pcie = to_imx_pcie(pci);	/* [한국어] 이 드라이버 인스턴스 */

	if (imx_pcie->phy) {	/* [한국어] generic PHY 가 있는 세대이면 */
		if (phy_power_off(imx_pcie->phy))	/* [한국어] 전원을 내리고, 실패하면 */
			dev_err(pci->dev, "unable to power off PHY\n");	/* [한국어] 알리기만 하고 계속 진행한다 — 정리 경로라 멈출 이유가 없다 */
		phy_exit(imx_pcie->phy);	/* [한국어] PHY 를 해제한다 */
	}
	imx_pcie_clk_disable(imx_pcie);	/* [한국어] 클럭을 내린다 */

	if (imx_pcie->vpcie)	/* [한국어] 슬롯 레귤레이터가 있으면 */
		regulator_disable(imx_pcie->vpcie);	/* [한국어] 마지막으로 끈다. **host_init 과 정확히 반대 순서다** */
}

/* [한국어]
 * imx_pcie_host_post_init - 링크가 선 뒤에 해야 하는 두 가지를 처리한다
 *
 * @pp: DWC 코어의 루트 포트 구조체.
 *
 * struct dw_pcie_host_ops 의 post_init 로 등록된다. 등록되는 것은
 * imx_pcie_host_dw_pme_ops 쪽뿐이며, imx_pcie_host_ops 는 그 자리를
 * pme_turn_off 로 대신 채운다.
 *
 * 하는 일이 둘이다.
 *   1. **ERR051586 우회**(IMX_PCIE_FLAG_8GT_ECN_ERR051586 세대). 상류
 *      주석이 errata 를 인용한다 — GEN3_RELATED_OFF[GEN3_ZRXDC_NONCOMPL]
 *      의 기본값 1 이 8GT/s 이상에서 동작할 때 수신기를 2.5GT/s 의 ZRX-DC
 *      규격에 어긋나게 만들어 L1 에서 불필요한 타임아웃을 낸다. 그래서
 *      그 비트를 0 으로 쓴다. 읽기 전용 필드라
 *      dw_pcie_dbi_ro_wr_en/dis 로 감싼다.
 *   2. **CLKREQ# 오버라이드를 푼다.** 링크가 실제로 서 있고 보드가
 *      CLKREQ# 배선을 지원한다고 장치 트리가 밝혔을 때만 한다. 그때부터는
 *      하드웨어 신호를 따르게 되어 전력 절감이 가능해진다.
 *
 * **링크가 선 뒤라야 하는 이유**는 둘 다 링크 상태에 의존하기 때문이다 —
 * 1번은 링크 속도가 정해진 뒤에 의미가 있고, 2번은 상대가 실제로
 * CLKREQ# 를 구동하고 있어야 안전하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_host_init() -> pp->ops->post_init -> [이 함수]
 *   imx_add_pcie_ep() -> [이 함수]
 *     -> dw_pcie_readl_dbi()/writel_dbi() -> drvdata->clr_clkreq_override()
 */
static void imx_pcie_host_post_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* [한국어] DWC 루트 포트에서 장치 구조체를 얻는다 */
	struct imx_pcie *imx_pcie = to_imx_pcie(pci);	/* [한국어] 이 드라이버 인스턴스 */
	u32 val;	/* [한국어] 읽고 고칠 레지스터 값 */

	if (imx_pcie->drvdata->flags & IMX_PCIE_FLAG_8GT_ECN_ERR051586) {	/* [한국어] **ERR051586 우회가 필요한 세대이면** — 바로 아래 상류 주석이 errata 를 인용한다 */
		/*
		 * ERR051586: Compliance with 8GT/s Receiver Impedance ECN
		 *
		 * The default value of GEN3_RELATED_OFF[GEN3_ZRXDC_NONCOMPL]
		 * is 1 which makes receiver non-compliant with the ZRX-DC
		 * parameter for 2.5 GT/s when operating at 8 GT/s or higher.
		 * It causes unnecessary timeout in L1.
		 *
		 * Workaround: Program GEN3_RELATED_OFF[GEN3_ZRXDC_NONCOMPL]
		 * to 0.
		 */
		dw_pcie_dbi_ro_wr_en(pci);	/* [한국어] **GEN3_RELATED_OFF 는 읽기 전용 필드를 포함해** 쓰기를 허용해야 고칠 수 있다 */
		val = dw_pcie_readl_dbi(pci, GEN3_RELATED_OFF);	/* [한국어] 현재 값을 읽고 */
		val &= ~GEN3_RELATED_OFF_GEN3_ZRXDC_NONCOMPL;	/* [한국어] **ZRXDC_NONCOMPL 비트를 지운다** — 기본값 1 이 8GT/s 이상에서 수신기를 2.5GT/s 의 ZRX-DC 규격에 어긋나게 만들어 L1 에서 불필요한 타임아웃을 낸다 */
		dw_pcie_writel_dbi(pci, GEN3_RELATED_OFF, val);	/* [한국어] 고친 값을 되쓴다 */
		dw_pcie_dbi_ro_wr_dis(pci);	/* [한국어] 읽기 전용 보호를 되돌린다 */
	}

	/* Clear CLKREQ# override if supports_clkreq is true and link is up */
	if (dw_pcie_link_up(pci) && imx_pcie->supports_clkreq) {	/* [한국어] **링크가 실제로 서 있고 보드가 CLKREQ# 배선을 지원한다고 밝혔을 때만** */
		if (imx_pcie->drvdata->clr_clkreq_override)	/* [한국어] 세대별 해제 함수가 있으면 */
			imx_pcie->drvdata->clr_clkreq_override(imx_pcie);	/* [한국어] 오버라이드를 푼다. 그때부터 하드웨어 신호를 따르게 되어 전력 절감이 가능해진다 */
	}
}

/*
 * In old DWC implementations, PCIE_ATU_INHIBIT_PAYLOAD in iATU Ctrl2
 * register is reserved, so the generic DWC implementation of sending the
 * PME_Turn_Off message using a dummy MMIO write cannot be used.
 */
/* [한국어]
 * imx_pcie_pme_turn_off - PME_Turn_Off 메시지를 GPR 비트로 보낸다
 *
 * @pp: DWC 코어의 루트 포트 구조체.
 *
 * struct dw_pcie_host_ops 의 pme_turn_off 로 등록된다. 서스펜드 때
 * 링크를 L2 로 내리기 전에 상대에게 "전원을 끌 테니 준비하라" 고 알리는
 * 규격상 메시지다.
 *
 * 바로 위 상류 주석이 자체 구현이 필요한 이유를 밝힌다 — 옛 DWC
 * 구현에서는 iATU Ctrl2 의 PCIE_ATU_INHIBIT_PAYLOAD 가 예약 비트라,
 * 공용 코드가 쓰는 방식(더미 MMIO 쓰기로 메시지를 만드는 것)을 쓸 수
 * 없다. 대신 i.MX 는 GPR12 에 PM_TURN_OFF 비트를 두어 그 비트를 세웠다
 * 지우면 하드웨어가 메시지를 보낸다.
 *
 * 메시지를 보낸 뒤 PCIE_PME_TO_L2_TIMEOUT_US 의 10분의 1 에서 그 값까지
 * 기다린다 — 상대가 응답하고 링크가 L2 로 내려갈 시간을 주는 것이다.
 *
 * 이 방식을 쓰는 세대는 drvdata 에서 ops 를 imx_pcie_host_ops 로 지정한
 * 쪽이고, 나머지는 imx_pcie_host_dw_pme_ops 를 써서 DWC 공용 구현에
 * 맡긴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(서스펜드). usleep 으로 잠든다.
 *
 * 호출 체인:
 *   dw_pcie_suspend_noirq() -> pp->ops->pme_turn_off -> [이 함수]
 *     -> regmap_set_bits()/regmap_clear_bits()
 */
static void imx_pcie_pme_turn_off(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* [한국어] DWC 루트 포트에서 장치 구조체를 얻는다 */
	struct imx_pcie *imx_pcie = to_imx_pcie(pci);	/* [한국어] 이 드라이버 인스턴스 */

	regmap_set_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR12, IMX6SX_GPR12_PCIE_PM_TURN_OFF);	/* [한국어] **PM_TURN_OFF 비트를 세웠다가** */
	regmap_clear_bits(imx_pcie->iomuxc_gpr, IOMUXC_GPR12, IMX6SX_GPR12_PCIE_PM_TURN_OFF);	/* [한국어] 곧바로 지운다 — 이 토글이 하드웨어에 PME_Turn_Off 메시지를 보내라고 알리는 것이다 */

	usleep_range(PCIE_PME_TO_L2_TIMEOUT_US/10, PCIE_PME_TO_L2_TIMEOUT_US);	/* [한국어] 상대가 응답하고 링크가 L2 로 내려갈 시간을 준다. 하한을 상한의 10분의 1 로 두어 대개는 짧게 끝난다 */
}

static const struct dw_pcie_host_ops imx_pcie_host_ops = {	/* [한국어] **pme_turn_off 를 자체 구현으로 채우는 묶음.** 옛 DWC 구현에서 공용 방식을 쓸 수 없는 세대가 쓴다 */
	.init = imx_pcie_host_init,	/* [한국어] RC 초기화 */
	.deinit = imx_pcie_host_exit,	/* [한국어] 정리 */
	.pme_turn_off = imx_pcie_pme_turn_off,	/* [한국어] PME_Turn_Off 를 GPR 비트로 보낸다 */
};

static const struct dw_pcie_host_ops imx_pcie_host_dw_pme_ops = {	/* [한국어] **post_init 을 채우고 pme_turn_off 는 DWC 공용 구현에 맡기는 묶음.** 표에 ops 를 적지 않은 세대가 기본으로 쓴다 */
	.init = imx_pcie_host_init,	/* [한국어] RC 초기화 */
	.deinit = imx_pcie_host_exit,	/* [한국어] 정리 */
	.post_init = imx_pcie_host_post_init,	/* [한국어] 링크가 선 뒤의 errata 우회와 CLKREQ# 정리 */
};

static const struct dw_pcie_ops dw_pcie_ops = {	/* [한국어] **DWC 코어가 링크를 세우고 내릴 때 부를 콜백 묶음** */
	.start_link = imx_pcie_start_link,	/* [한국어] LTSSM 을 켜고 속도를 올린다 */
	.stop_link = imx_pcie_stop_link,	/* [한국어] LTSSM 을 끈다 */
};

/* [한국어]
 * imx_pcie_ep_raise_irq - EP 모드에서 호스트로 인터럽트를 올린다
 *
 * @ep: DWC 코어의 엔드포인트 구조체.
 * @func_no: 어느 물리 함수가 올리는지.
 * @type: PCI_IRQ_INTX / PCI_IRQ_MSI / PCI_IRQ_MSIX 중 하나.
 * @interrupt_num: MSI/MSI-X 에서 몇 번째 벡터인지. INTx 에서는 쓰지 않는다.
 * @return: 하위 DWC 헬퍼의 반환값. 아는 종류가 아니면 -EINVAL.
 *
 * struct dw_pcie_ep_ops 의 raise_irq 로 등록되어 엔드포인트 프레임워크가
 * 부른다. i.MX 에 특별한 것이 없어 종류별로 DWC 공용 헬퍼에 그대로
 * 넘기는 얇은 분배기다.
 *
 * [관찰] switch 의 모든 갈래가 return 으로 끝나므로 그 아래의 return 0 은
 * 닿지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 엔드포인트 기능 드라이버가 호스트에
 * 알릴 일이 있을 때 부른다.
 *
 * 호출 체인:
 *   엔드포인트 프레임워크 -> ep->ops->raise_irq -> [이 함수]
 *     -> dw_pcie_ep_raise_intx_irq()/_msi_irq()/_msix_irq()
 */
static int imx_pcie_ep_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				  unsigned int type, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);	/* [한국어] 오류 메시지를 낼 장치를 얻기 위한 DWC 구조체 */

	switch (type) {	/* [한국어] 인터럽트 종류에 따라 갈린다 */
	case PCI_IRQ_INTX:	/* [한국어] 레거시 INTx 이면 */
		return dw_pcie_ep_raise_intx_irq(ep, func_no);	/* [한국어] DWC 공용 헬퍼에 맡긴다 */
	case PCI_IRQ_MSI:	/* [한국어] MSI 이면 */
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);	/* [한국어] 벡터 번호와 함께 공용 헬퍼에 맡긴다 */
	case PCI_IRQ_MSIX:	/* [한국어] MSI-X 이면 */
		return dw_pcie_ep_raise_msix_irq(ep, func_no, interrupt_num);	/* [한국어] 같은 방식으로 맡긴다 */
	default:	/* [한국어] 아는 종류가 아니면 */
		dev_err(pci->dev, "UNKNOWN IRQ type\n");	/* [한국어] 그 사실을 알리고 */
		return -EINVAL;	/* [한국어] 인자 오류로 돌아간다 */
	}

	return 0;	/* [한국어] **switch 의 모든 갈래가 return 으로 끝나므로 이 줄에는 닿지 않는다** */
}

static const struct pci_epc_features imx8m_pcie_epc_features = {	/* [한국어] **i.MX8M 계열의 엔드포인트 능력표** */
	DWC_EPC_COMMON_FEATURES,	/* [한국어] DWC 공통 능력을 그대로 가져온다 */
	.msi_capable = true,	/* [한국어] MSI 를 낼 수 있다 */
	.bar[BAR_1] = { .type = BAR_DISABLED, },	/* [한국어] BAR1 은 쓰지 않는다 — BAR0 가 64비트이면 BAR1 이 그 상위 절반이 되기 때문이다 */
	.bar[BAR_3] = { .type = BAR_DISABLED, },	/* [한국어] BAR3 도 쓰지 않는다 */
	.bar[BAR_4] = { .type = BAR_FIXED, .fixed_size = SZ_256, },	/* [한국어] BAR4 는 256바이트 고정 크기다 */
	.bar[BAR_5] = { .type = BAR_DISABLED, },	/* [한국어] BAR5 도 쓰지 않는다 */
	.align = SZ_64K,	/* [한국어] 인바운드 창을 나눌 단위가 64KB 다. ep->page_size 가 이 값이 된다 */
};

static const struct pci_epc_features imx8q_pcie_epc_features = {	/* [한국어] **i.MX8Q 의 엔드포인트 능력표** */
	DWC_EPC_COMMON_FEATURES,	/* [한국어] DWC 공통 능력 */
	.msi_capable = true,	/* [한국어] MSI 를 낼 수 있다 */
	.bar[BAR_1] = { .type = BAR_DISABLED, },	/* [한국어] BAR1 은 쓰지 않는다 */
	.bar[BAR_3] = { .type = BAR_DISABLED, },	/* [한국어] BAR3 도 쓰지 않는다 */
	.bar[BAR_5] = { .type = BAR_DISABLED, },	/* [한국어] BAR5 도 쓰지 않는다. **8M 판과 달리 BAR4 는 고정 크기가 아니다** */
	.align = SZ_64K,	/* [한국어] 정렬 단위 64KB */
};

/*
 *     	| Default  | Default | Default | BAR Sizing
 * BAR#	| Enable?  | Type    | Size    | Scheme
 * =======================================================
 * BAR0	| Enable   | 64-bit  |  1 MB   | Programmable Size
 * BAR1	| Disable  | 32-bit  | 64 KB   | Fixed Size
 *       (BAR1 should be disabled if BAR0 is 64-bit)
 * BAR2	| Enable   | 32-bit  |  1 MB   | Programmable Size
 * BAR3	| Enable   | 32-bit  | 64 KB   | Programmable Size
 * BAR4	| Enable   | 32-bit  |  1 MB   | Programmable Size
 * BAR5	| Enable   | 32-bit  | 64 KB   | Programmable Size
 */
static const struct pci_epc_features imx95_pcie_epc_features = {	/* [한국어] **i.MX95 의 엔드포인트 능력표.** 바로 위 상류 주석의 표가 BAR0~5 의 기본 활성 여부·폭·크기·사이징 방식을 적어 두었다 */
	DWC_EPC_COMMON_FEATURES,	/* [한국어] DWC 공통 능력 */
	.msi_capable = true,	/* [한국어] MSI 를 낼 수 있다 */
	.bar[BAR_1] = { .type = BAR_FIXED, .fixed_size = SZ_64K, },	/* [한국어] **BAR1 은 비활성이 아니라 64KB 고정**이다 — 위 표대로 이 세대의 BAR1 은 기본 비활성 32비트 64KB 고정 크기다 */
	.align = SZ_4K,	/* [한국어] **정렬 단위가 4KB** 로 다른 두 세대보다 잘다 */
};

static const struct pci_epc_features*	/* [한국어] 반환형이 앞 줄에 따로 있는 정의다 */
/* [한국어]
 * imx_pcie_ep_get_features - 이 세대의 엔드포인트 능력표를 돌려준다
 *
 * @ep: DWC 코어의 엔드포인트 구조체.
 * @return: drvdata 에 적힌 struct pci_epc_features 포인터.
 *
 * struct dw_pcie_ep_ops 의 get_features 로 등록된다. 엔드포인트
 * 프레임워크가 "이 컨트롤러는 BAR 를 몇 개 쓸 수 있고 크기 제약이
 * 무엇인가" 를 물을 때 부른다.
 *
 * 세대마다 답이 달라 drvdata 의 epc_features 필드로 갈라 두었다.
 * 이 파일에 있는 능력표는 셋이다.
 *   imx8m_pcie_epc_features  — BAR1/3/5 비활성, BAR4 는 256바이트 고정,
 *                              정렬 64KB.
 *   imx8q_pcie_epc_features  — BAR1/3/5 비활성, 정렬 64KB.
 *   imx95_pcie_epc_features  — BAR1 은 64KB 고정, 정렬 4KB.
 *                              그 위의 상류 주석에 BAR0~5 의 기본 활성
 *                              여부·폭·크기·사이징 방식이 표로 있다.
 *                              BAR0 가 64비트이면 BAR1 은 그 상위 절반이
 *                              되므로 비활성이어야 한다는 설명도 그 표에 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   엔드포인트 프레임워크 -> ep->ops->get_features -> [이 함수]
 */
imx_pcie_ep_get_features(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);	/* [한국어] 엔드포인트 구조체에서 DWC 장치 구조체를 얻는다 */
	struct imx_pcie *imx_pcie = to_imx_pcie(pci);	/* [한국어] 거기서 다시 이 드라이버 인스턴스를 얻는다 */

	return imx_pcie->drvdata->epc_features;	/* [한국어] **세대별 표에 적힌 능력표를 그대로 돌려준다.** 이 파일에는 8M/8Q/95 세 벌이 있다 */
}

static const struct dw_pcie_ep_ops pcie_ep_ops = {	/* [한국어] **엔드포인트 프레임워크가 부를 콜백 묶음** */
	.raise_irq = imx_pcie_ep_raise_irq,	/* [한국어] 호스트로 인터럽트를 올린다 */
	.get_features = imx_pcie_ep_get_features,	/* [한국어] 이 세대의 능력표를 알린다 */
};

/* [한국어]
 * imx_add_pcie_ep - 이 컨트롤러를 엔드포인트로 세운다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @pdev: 플랫폼 장치.
 * @return: 0 이면 성공, 실패면 DWC 헬퍼의 오류 코드.
 *
 * probe 가 drvdata->mode 를 보고 EP 세대일 때 부르는 경로다. RC 쪽
 * dw_pcie_host_init() 에 대응한다.
 *
 * 순서가 이렇다.
 *   1. **imx_pcie_host_init(pp) 을 직접 부른다.** 이름은 host 이지만
 *      하드웨어를 켜는 절차(전원·리셋·PHY·클럭)는 RC 든 EP 든 같기
 *      때문이다. RC 경로에서는 DWC 코어가 콜백으로 부르는 함수를 여기서는
 *      직접 부르는 셈이다.
 *   2. ep->ops 에 pcie_ep_ops 를 매단다.
 *   3. 64비트를 지원하는 세대이면 DMA 마스크를 64비트로 넓힌다.
 *   4. ep->page_size 를 능력표의 정렬 값으로 정한다 — 인바운드 창을
 *      나눌 단위다.
 *   5. dw_pcie_ep_init() 으로 엔드포인트를 만든다.
 *   6. **imx_pcie_host_post_init(pp) 도 직접 부른다.** RC 경로에서
 *      post_init 콜백이 하던 errata 우회와 CLKREQ# 정리를 여기서 한다.
 *   7. dw_pcie_ep_init_registers() 로 BAR 와 config 공간을 프로그래밍한다.
 *      실패하면 5번을 되돌린다.
 *   8. pci_epc_init_notify() 로 기능 드라이버들에게 준비되었음을 알린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   imx_pcie_probe() -> [이 함수] -> imx_pcie_host_init()
 *     -> dw_pcie_ep_init() -> imx_pcie_host_post_init()
 *     -> dw_pcie_ep_init_registers() -> pci_epc_init_notify()
 */
static int imx_add_pcie_ep(struct imx_pcie *imx_pcie,
			   struct platform_device *pdev)
{
	int ret;	/* [한국어] 하위 호출 결과 */
	struct dw_pcie_ep *ep;	/* [한국어] DWC 엔드포인트 구조체 */
	struct dw_pcie *pci = imx_pcie->pci;	/* [한국어] DWC 장치 구조체 */
	struct dw_pcie_rp *pp = &pci->pp;	/* [한국어] 루트 포트 구조체. **EP 모드에서도 하드웨어를 켜는 함수가 이것을 인자로 받기 때문에 필요하다** */
	struct device *dev = pci->dev;	/* [한국어] 오류 메시지를 낼 장치 */

	imx_pcie_host_init(pp);	/* [한국어] **이름은 host 이지만 하드웨어를 켜는 절차는 RC 든 EP 든 같다.** RC 경로에서 DWC 코어가 콜백으로 부르는 함수를 여기서는 직접 부른다 */
	ep = &pci->ep;	/* [한국어] 엔드포인트 구조체의 위치 */
	ep->ops = &pcie_ep_ops;	/* [한국어] 콜백 묶음을 매단다 */

	if (imx_check_flag(imx_pcie, IMX_PCIE_FLAG_SUPPORT_64BIT))	/* [한국어] 64비트를 지원하는 세대이면 */
		dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));	/* [한국어] DMA 마스크를 64비트로 넓힌다. 반환값은 보지 않는다 */

	ep->page_size = imx_pcie->drvdata->epc_features->align;	/* [한국어] **인바운드 창을 나눌 단위를 능력표의 정렬 값으로 정한다** */

	ret = dw_pcie_ep_init(ep);	/* [한국어] DWC 엔드포인트를 만든다 */
	if (ret) {	/* [한국어] 실패하면 */
		dev_err(dev, "failed to initialize endpoint\n");	/* [한국어] 그 사실을 알리고 */
		return ret;	/* [한국어] 그 코드를 돌려준다 */
	}
	imx_pcie_host_post_init(pp);	/* [한국어] **post_init 도 직접 부른다** — RC 경로에서 콜백이 하던 errata 우회와 CLKREQ# 정리를 여기서 한다 */

	ret = dw_pcie_ep_init_registers(ep);	/* [한국어] BAR 와 config 공간을 프로그래밍한다 */
	if (ret) {	/* [한국어] 실패하면 */
		dev_err(dev, "Failed to initialize DWC endpoint registers\n");	/* [한국어] 그 사실을 알리고 */
		dw_pcie_ep_deinit(ep);	/* [한국어] 앞서 만든 엔드포인트를 되돌린 뒤 */
		return ret;	/* [한국어] 그 코드를 돌려준다 */
	}

	pci_epc_init_notify(ep->epc);	/* [한국어] **기능 드라이버들에게 준비되었음을 알린다** */

	return 0;	/* [한국어] 모든 단계를 지났으면 성공 */
}

/* [한국어]
 * imx_pcie_msi_save_restore - 서스펜드 전후로 MSI 활성화 비트를 저장하고 되돌린다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 * @save: true 면 저장, false 면 복원.
 *
 * 서스펜드에서 컨트롤러 전원이 내려가면 루트 포트 자신의 config 공간에
 * 있는 MSI capability 의 MSI_FLAGS 가 초기값으로 돌아간다. 그러면 깨어난
 * 뒤 MSI 가 동작하지 않으므로 값을 imx_pcie->msi_ctrl 에 담아 두었다가
 * 되돌린다.
 *
 * pci_msi_enabled() 가 거짓이면(커널이 MSI 를 끈 경우) 아무 일도 하지
 * 않는다.
 *
 * 복원 쪽만 dw_pcie_dbi_ro_wr_en/dis 로 감싼다 — MSI_FLAGS 의 일부가
 * 읽기 전용이라 그냥은 써지지 않기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(noirq 단계의 서스펜드·리줌).
 *
 * 호출 체인:
 *   imx_pcie_suspend_noirq()/imx_pcie_resume_noirq() -> [이 함수]
 *     -> dw_pcie_readw_dbi()/dw_pcie_writew_dbi()
 */
static void imx_pcie_msi_save_restore(struct imx_pcie *imx_pcie, bool save)
{
	u8 offset;	/* [한국어] MSI capability 의 위치 */
	u16 val;	/* [한국어] 읽고 쓸 값 */
	struct dw_pcie *pci = imx_pcie->pci;	/* [한국어] dbi 접근에 쓸 DWC 장치 구조체 */

	if (pci_msi_enabled()) {	/* [한국어] **커널이 MSI 를 끈 상태이면 아무 일도 하지 않는다** */
		offset = dw_pcie_find_capability(pci, PCI_CAP_ID_MSI);	/* [한국어] 루트 포트 자신의 MSI capability 위치를 찾는다 */
		if (save) {	/* [한국어] 저장하는 경우 */
			val = dw_pcie_readw_dbi(pci, offset + PCI_MSI_FLAGS);	/* [한국어] 현재 MSI_FLAGS 를 읽어 */
			imx_pcie->msi_ctrl = val;	/* [한국어] 인스턴스에 담아 둔다 */
		} else {
			dw_pcie_dbi_ro_wr_en(pci);	/* [한국어] **MSI_FLAGS 의 일부가 읽기 전용이라** 쓰기를 허용해야 한다 */
			val = imx_pcie->msi_ctrl;	/* [한국어] 담아 둔 값을 꺼내 */
			dw_pcie_writew_dbi(pci, offset + PCI_MSI_FLAGS, val);	/* [한국어] 되쓴다 */
			dw_pcie_dbi_ro_wr_dis(pci);	/* [한국어] 읽기 전용 보호를 되돌린다 */
		}
	}
}

/* [한국어]
 * imx_pcie_lut_save - 서스펜드 전에 LUT 32칸을 전부 읽어 둔다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 *
 * i.MX95 의 LUT 는 GPR 블록에 있어 전원이 내려가면 사라진다. 그런데 그
 * 내용은 PCI 열거 중에 장치별로 채워진 것이라 다시 만들 방법이 없으므로,
 * 서스펜드 전에 통째로 imx_pcie->luts[] 에 베껴 둔다.
 *
 * 32칸을 모두 훑되, 유효 비트가 없는 칸은 0 으로 채워 둔다 — 복원 때
 * 그 칸을 건너뛰게 하기 위해서다.
 *
 * [관찰] imx_pcie_add_lut()/remove_lut() 과 달리 뮤텍스를 잡지 않는다.
 * noirq 단계라 다른 흐름이 LUT 를 건드릴 수 없기 때문으로 보이나, 코드에
 * 그 근거가 적혀 있지는 않다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(noirq 단계의 서스펜드).
 *
 * 호출 체인:  imx_pcie_suspend_noirq() -> [이 함수] -> regmap_read()
 */
static void imx_pcie_lut_save(struct imx_pcie *imx_pcie)
{
	u32 data1, data2;	/* [한국어] 읽어 낼 두 워드 */
	int i;	/* [한국어] 칸 순회 인덱스 */

	for (i = 0; i < IMX95_MAX_LUT; i++) {	/* [한국어] 32칸을 모두 훑는다 */
		regmap_write(imx_pcie->iomuxc_gpr, IMX95_PE0_LUT_ACSCTRL,	/* [한국어] **읽기 방향 비트와 인덱스로 그 칸을 고른다** */
			     IMX95_PEO_LUT_RWA | i);	/* [한국어] RWA 가 읽기 방향이고 i 가 칸 번호다 */
		regmap_read(imx_pcie->iomuxc_gpr, IMX95_PE0_LUT_DATA1, &data1);	/* [한국어] 첫 워드를 읽고 */
		regmap_read(imx_pcie->iomuxc_gpr, IMX95_PE0_LUT_DATA2, &data2);	/* [한국어] 둘째 워드도 읽는다 */
		if (data1 & IMX95_PE0_LUT_VLD) {	/* [한국어] **유효 비트가 있으면 실제로 쓰이던 칸이다** */
			imx_pcie->luts[i].data1 = data1;	/* [한국어] 첫 워드를 보관하고 */
			imx_pcie->luts[i].data2 = data2;	/* [한국어] 둘째 워드도 보관한다 */
		} else {
			imx_pcie->luts[i].data1 = 0;	/* [한국어] 첫 워드를 0 으로 두고 */
			imx_pcie->luts[i].data2 = 0;	/* [한국어] 둘째 워드도 0 으로 둔다 — 복원 때 이 칸을 건너뛰게 하는 표시다 */
		}
	}
}

/* [한국어]
 * imx_pcie_lut_restore - 리줌 뒤 저장해 둔 LUT 내용을 다시 써 넣는다
 *
 * @imx_pcie: 이 드라이버 인스턴스.
 *
 * imx_pcie_lut_save() 의 짝이다. 저장할 때 무효한 칸을 0 으로 채워
 * 두었으므로, 유효 비트가 없는 칸은 건너뛰고 있던 칸만 되돌린다.
 *
 * 쓰는 순서는 추가 때와 같다 — DATA1 과 DATA2 를 채운 뒤 ACSCTRL 에
 * 인덱스를 써서 반영한다.
 *
 * **칸 번호를 그대로 유지한다.** 어느 칸에 있었는지가 기능상 의미는
 * 없지만, 저장·복원을 인덱스 단위로 맞추면 로직이 단순해진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(noirq 단계의 리줌).
 *
 * 호출 체인:  imx_pcie_resume_noirq() -> [이 함수] -> regmap_write()
 */
static void imx_pcie_lut_restore(struct imx_pcie *imx_pcie)
{
	int i;	/* [한국어] 칸 순회 인덱스 */

	for (i = 0; i < IMX95_MAX_LUT; i++) {	/* [한국어] 32칸을 모두 훑는다 */
		if ((imx_pcie->luts[i].data1 & IMX95_PE0_LUT_VLD) == 0)	/* [한국어] **보관한 값에 유효 비트가 없으면 원래 빈 칸이었다** */
			continue;	/* [한국어] 되돌릴 것이 없으므로 넘어간다 */

		regmap_write(imx_pcie->iomuxc_gpr, IMX95_PE0_LUT_DATA1,	/* [한국어] 첫 워드를 */
			     imx_pcie->luts[i].data1);	/* [한국어] 보관해 둔 값으로 되쓰고 */
		regmap_write(imx_pcie->iomuxc_gpr, IMX95_PE0_LUT_DATA2,	/* [한국어] 둘째 워드도 */
			     imx_pcie->luts[i].data2);	/* [한국어] 되쓴 뒤 */
		regmap_write(imx_pcie->iomuxc_gpr, IMX95_PE0_LUT_ACSCTRL, i);	/* [한국어] **읽기 방향 비트 없이 인덱스만 써서 그 칸에 반영한다.** 칸 번호를 그대로 유지한다 */
	}
}

/* [한국어]
 * imx_pcie_suspend_noirq - 서스펜드. 세대에 따라 정상 L2 진입과 errata 우회로 갈린다
 *
 * @dev: 플랫폼 장치.
 * @return: 0 이면 성공, 실패면 DWC 헬퍼의 오류 코드.
 *
 * IMX_PCIE_FLAG_SUPPORTS_SUSPEND 세대가 아니면 곧바로 0 으로 나간다 —
 * 서스펜드를 지원하지 않는 세대는 아무것도 하지 않는 편이 안전하다.
 *
 * 먼저 상태를 저장한다. MSI 활성화 비트는 모든 세대에서, LUT 는
 * IMX_PCIE_FLAG_HAS_LUT 세대에서만.
 *
 * 그 다음이 갈림길이다.
 *   - **IMX_PCIE_FLAG_BROKEN_SUSPEND**(ERR005723 의 영향을 받는 세대,
 *     파일 위쪽 플래그 정의의 상류 주석이 그 errata 를 밝힌다: PCIe 가
 *     L2 전원 차단을 지원하지 않는다) — 정상적인 L2 진입을 쓸 수 없다.
 *     대신 코어 리셋을 걸고 PERST# 를 어서트하고 레퍼런스 클럭을 끈다.
 *     그 자리의 상류 주석이 최소한의 우회는 PERST# 와 PCIE_TEST_PD 를
 *     세우는 것이지만 클럭까지 끄면 전력을 더 아낄 수 있다고 적는다.
 *   - 그 밖의 세대 — dw_pcie_suspend_noirq() 에 맡긴다. 그쪽이
 *     PME_Turn_Off 를 보내고 L2 로 내려가는 정규 절차를 밟는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, **noirq 단계**. 인터럽트가 이미
 * 꺼져 있어 이 시점에는 장치 인터럽트가 오지 않는다.
 *
 * 호출 체인:
 *   PM 코어 -> imx_pcie_pm_ops.suspend_noirq -> [이 함수]
 *     -> imx_pcie_msi_save_restore() -> imx_pcie_lut_save()
 *     -> imx_pcie_assert_core_reset() / dw_pcie_suspend_noirq()
 */
static int imx_pcie_suspend_noirq(struct device *dev)
{
	struct imx_pcie *imx_pcie = dev_get_drvdata(dev);	/* [한국어] 플랫폼 장치에 심어 둔 이 드라이버 인스턴스 */

	if (!(imx_pcie->drvdata->flags & IMX_PCIE_FLAG_SUPPORTS_SUSPEND))	/* [한국어] **서스펜드를 지원하지 않는 세대이면** */
		return 0;	/* [한국어] 아무것도 하지 않는 편이 안전하다 */

	imx_pcie_msi_save_restore(imx_pcie, true);	/* [한국어] **MSI 활성화 비트를 저장한다** — 모든 세대에서 한다 */
	if (imx_check_flag(imx_pcie, IMX_PCIE_FLAG_HAS_LUT))	/* [한국어] LUT 세대이면 */
		imx_pcie_lut_save(imx_pcie);	/* [한국어] 표 32칸도 통째로 저장한다 */
	if (imx_check_flag(imx_pcie, IMX_PCIE_FLAG_BROKEN_SUSPEND)) {	/* [한국어] **ERR005723 의 영향을 받는 세대이면** — 파일 위쪽 플래그 정의의 상류 주석대로 PCIe 가 L2 전원 차단을 지원하지 않는다 */
		/*
		 * The minimum for a workaround would be to set PERST# and to
		 * set the PCIE_TEST_PD flag. However, we can also disable the
		 * clock which saves some power.
		 */
		imx_pcie_assert_core_reset(imx_pcie);	/* [한국어] 정상적인 L2 진입 대신 코어 리셋을 걸고 */
		imx_pcie_assert_perst(imx_pcie, true);	/* [한국어] PERST# 를 어서트하고 */
		imx_pcie->drvdata->enable_ref_clk(imx_pcie, false);	/* [한국어] 레퍼런스 클럭을 끈다. 바로 위 상류 주석대로 최소한의 우회는 PERST# 와 PCIE_TEST_PD 를 세우는 것이지만 클럭까지 끄면 전력을 더 아낄 수 있다 */
	} else {
		return dw_pcie_suspend_noirq(imx_pcie->pci);	/* [한국어] **DWC 공용 구현에 맡긴다** — PME_Turn_Off 를 보내고 L2 로 내려가는 정규 절차다 */
	}

	return 0;	/* [한국어] 우회 경로를 밟았으면 성공으로 나간다 */
}

/* [한국어]
 * imx_pcie_resume_noirq - 리줌. 서스펜드와 대칭으로 갈리고 저장한 상태를 되돌린다
 *
 * @dev: 플랫폼 장치.
 * @return: 0 이면 성공, 실패면 하위 호출의 오류 코드.
 *
 * imx_pcie_suspend_noirq() 의 짝이며 같은 조건으로 갈린다.
 *
 *   - **BROKEN_SUSPEND 세대** — 레퍼런스 클럭을 켜고 코어 리셋과 PERST# 를
 *     푼 뒤 **dw_pcie_setup_rc() 로 루트 컴플렉스를 다시 세운다.**
 *     그 자리의 상류 주석이 이유를 밝힌다 — PCIE_TEST_PD 를 쓰면 MSI 가
 *     꺼지고 루트 컴플렉스의 전원이 내려가는 것으로 보이므로, RC 설정을
 *     다시 하고 MSI 레지스터도 되돌려야 한다.
 *   - 그 밖의 세대 — dw_pcie_resume_noirq() 가 L2 에서 복귀시킨다.
 *
 * 그 뒤 저장해 둔 것을 되돌린다 — LUT(해당 세대만)와 MSI 활성화 비트.
 * **되돌리는 순서가 서스펜드의 저장 순서와 반대**다(저장은 MSI -> LUT,
 * 복원은 LUT -> MSI).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, noirq 단계.
 *
 * 호출 체인:
 *   PM 코어 -> imx_pcie_pm_ops.resume_noirq -> [이 함수]
 *     -> drvdata->enable_ref_clk() -> imx_pcie_deassert_core_reset()
 *     -> dw_pcie_setup_rc() / dw_pcie_resume_noirq()
 *     -> imx_pcie_lut_restore() -> imx_pcie_msi_save_restore()
 */
static int imx_pcie_resume_noirq(struct device *dev)
{
	int ret;	/* [한국어] 하위 호출 결과 */
	struct imx_pcie *imx_pcie = dev_get_drvdata(dev);	/* [한국어] 플랫폼 장치에 심어 둔 이 드라이버 인스턴스 */

	if (!(imx_pcie->drvdata->flags & IMX_PCIE_FLAG_SUPPORTS_SUSPEND))	/* [한국어] 서스펜드를 지원하지 않는 세대이면 */
		return 0;	/* [한국어] 되돌릴 것도 없다 */

	if (imx_check_flag(imx_pcie, IMX_PCIE_FLAG_BROKEN_SUSPEND)) {	/* [한국어] **서스펜드 때 우회 경로를 밟았던 세대이면** 대칭으로 되돌린다 */
		ret = imx_pcie->drvdata->enable_ref_clk(imx_pcie, true);	/* [한국어] 레퍼런스 클럭을 켜고 */
		if (ret)	/* [한국어] 실패하면 */
			return ret;	/* [한국어] 그 코드를 돌려준다 */
		imx_pcie_deassert_core_reset(imx_pcie);	/* [한국어] 코어 리셋을 풀고 */
		imx_pcie_assert_perst(imx_pcie, false);	/* [한국어] PERST# 도 해제한다 */

		/*
		 * Using PCIE_TEST_PD seems to disable MSI and powers down the
		 * root complex. This is why we have to setup the rc again and
		 * why we have to restore the MSI register.
		 */
		ret = dw_pcie_setup_rc(&imx_pcie->pci->pp);	/* [한국어] **루트 컴플렉스를 다시 세운다.** 바로 위 상류 주석이 이유를 밝힌다 — PCIE_TEST_PD 를 쓰면 MSI 가 꺼지고 루트 컴플렉스의 전원이 내려가는 것으로 보이므로 RC 설정을 다시 해야 한다 */
		if (ret)	/* [한국어] 실패하면 */
			return ret;	/* [한국어] 그 코드를 돌려준다 */
	} else {
		ret = dw_pcie_resume_noirq(imx_pcie->pci);	/* [한국어] DWC 공용 구현이 L2 에서 복귀시킨다 */
		if (ret)	/* [한국어] 실패하면 */
			return ret;	/* [한국어] 그 코드를 돌려준다 */
	}
	if (imx_check_flag(imx_pcie, IMX_PCIE_FLAG_HAS_LUT))	/* [한국어] LUT 세대이면 */
		imx_pcie_lut_restore(imx_pcie);	/* [한국어] 저장해 둔 표를 되돌린다 */
	imx_pcie_msi_save_restore(imx_pcie, false);	/* [한국어] **MSI 활성화 비트를 되돌린다.** 저장 순서와 반대로 LUT 를 먼저 되돌린다 */

	return 0;	/* [한국어] 모두 되돌렸으면 성공 */
}

static const struct dev_pm_ops imx_pcie_pm_ops = {	/* [한국어] **PM 코어가 부를 콜백 묶음** */
	NOIRQ_SYSTEM_SLEEP_PM_OPS(imx_pcie_suspend_noirq,	/* [한국어] noirq 단계의 서스펜드와 */
				  imx_pcie_resume_noirq)	/* [한국어] 리줌만 채운다. 일반 suspend/resume 단계에는 할 일이 없다 */
};

/* [한국어]
 * imx_pcie_probe - 진입점. 자원을 모으고 RC 또는 EP 로 갈린다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 이면 성공, 실패면 그 단계의 오류 코드.
 *
 * 이 파일에서 가장 긴 함수이며 하는 일이 셋으로 나뉜다.
 *
 * **1. 두 구조체를 잡고 표를 고른다.**
 *   struct imx_pcie(이 드라이버의 상태)와 struct dw_pcie(DWC 코어가 쓰는
 *   구조체)를 각각 devm_kzalloc 으로 잡고 서로 잇는다.
 *   of_device_get_match_data() 가 compatible 문자열에 대응하는 drvdata[]
 *   항목을 돌려주며, **이 한 줄이 이후 모든 세대 분기의 근거**가 된다.
 *   host_ops 도 여기서 정한다 — 표에 ops 가 적혀 있으면 그것을, 없으면
 *   imx_pcie_host_dw_pme_ops 를 쓴다.
 *
 * **2. 장치 트리에서 자원을 모은다.** 모두 있으면 쓰고 없으면 넘어가거나
 *   실패하는 형태이며, 세대별 플래그가 필요 여부를 정한다.
 *   - fsl,imx7d-pcie-phy phandle -> phy_base(i.MX7D 의 ERR010728 우회용).
 *     상류 주석대로 이것을 쓰는 것은 i.MX7D 뿐이다.
 *   - reset GPIO -> PERST# 신호.
 *   - 클럭 묶음 전부. 이름이 "extref" 로 시작하는 클럭이 있으면
 *     **외부 레퍼런스 클럭을 쓴다고 표시한다**(enable_ext_refclk) —
 *     그 표시를 imx95_pcie_init_phy() 가 읽어 클럭 경로를 고른다.
 *   - HAS_PHYDRV 세대이면 generic PHY, HAS_APP_RESET 이면 "apps" 리셋,
 *     HAS_PHY_RESET 이면 "pciephy" 리셋.
 *   - IMX8MQ 계열은 linux,pci-domain 으로 controller_id 를 정한다 —
 *     컨트롤러가 둘이라 어느 쪽인지 알아야 GPR14/GPR16 을 고를 수 있다.
 *   - **iomuxc_gpr 을 얻는 방법이 둘이다.** 표에 gpr 문자열이 있으면
 *     syscon 을 compatible 로 찾고, HAS_SERDES 세대이면 "app" 이라는
 *     이름의 MMIO 자원을 직접 매핑해 그 위에 regmap 을 만든다. 뒤쪽은
 *     별도의 syscon 노드 없이 컨트롤러 자기 레지스터 창을 쓰는 방식이다.
 *   - fsl,tx-* 네 가지 PHY 송신 파라미터. 없으면 기본값(0, 0, 20, 127,
 *     127)을 쓴다.
 *   - fsl,max-link-speed 로 최대 링크 속도. **기본값이 1(Gen1)** 이라
 *     장치 트리가 말하지 않으면 Gen1 로 제한된다.
 *   - supports-clkreq 불리언, 그리고 레귤레이터 셋(vpcie3v3aux 는
 *     얻자마자 켜고, vpcie 와 vph 는 나중에 쓰려고 붙잡아만 둔다).
 *   - 전원 도메인 붙이기.
 *
 * **3. 모드에 따라 갈린다.**
 *   - EP 세대이면 imx_add_pcie_ep() 로 엔드포인트를 세운 뒤 RID 0 으로
 *     LUT 를 하나 넣는다. 그 자리의 상류 주석이 FIXME 로 이유를 적는다 —
 *     엔드포인트 프레임워크의 제약 때문에 EPF 가 하나만 지원된다.
 *   - RC 세대이면 세대별 플래그 둘(skip_l23_ready, keep_rp_msi_en)을
 *     DWC 코어 쪽 구조체에 옮기고 use_atu_msg 를 켠 뒤
 *     dw_pcie_host_init() 을 부른다. 그 뒤 MSI 가 켜져 있으면 루트 포트
 *     자신의 MSI capability 에 ENABLE 비트를 세운다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. PROBE_PREFER_ASYNCHRONOUS 로
 * 등록되어 있어 다른 probe 와 병렬로 돌 수 있다.
 *
 * 에러 경로: 대부분 dev_err_probe() 로 오류를 남기며 곧바로 돌아간다.
 * devm_ 계열로 잡은 자원은 커널이 자동으로 되돌린다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 -> [이 함수] -> of_device_get_match_data()
 *     -> devm_clk_bulk_get_all() -> syscon_regmap_lookup_by_compatible()
 *     -> imx_pcie_attach_pd()
 *     -> imx_add_pcie_ep() 또는 dw_pcie_host_init()
 */
static int imx_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;	/* [한국어] 플랫폼 장치의 device 구조체 */
	struct dw_pcie *pci;	/* [한국어] DWC 코어가 쓸 장치 구조체 */
	struct imx_pcie *imx_pcie;	/* [한국어] 이 드라이버의 인스턴스 상태 */
	struct device_node *node = dev->of_node;	/* [한국어] 장치 트리 노드 */
	int i, ret, domain;	/* [한국어] i 는 클럭 순회 인덱스, ret 은 하위 호출 결과, domain 은 PCI 도메인 번호 */
	u16 val;	/* [한국어] MSI_FLAGS 를 읽고 쓸 값 */

	imx_pcie = devm_kzalloc(dev, sizeof(*imx_pcie), GFP_KERNEL);	/* [한국어] **이 드라이버의 상태 구조체를 잡는다.** devm_ 이라 실패 시 커널이 자동으로 되돌린다 */
	if (!imx_pcie)	/* [한국어] 못 잡으면 */
		return -ENOMEM;	/* [한국어] 메모리 부족으로 돌아간다 */

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);	/* [한국어] **DWC 코어가 쓸 구조체는 따로 잡는다** — 두 층이 각자의 상태를 갖는다 */
	if (!pci)	/* [한국어] 못 잡으면 */
		return -ENOMEM;	/* [한국어] 메모리 부족으로 돌아간다 */

	pci->dev = dev;	/* [한국어] DWC 구조체에 장치를 매단다 */
	pci->ops = &dw_pcie_ops;	/* [한국어] **링크를 세우고 내리는 콜백 묶음을 매단다** */

	imx_pcie->pci = pci;	/* [한국어] 두 구조체를 잇는다. 반대 방향은 to_imx_pcie() 매크로가 drvdata 를 거쳐 되찾는다 */
	imx_pcie->drvdata = of_device_get_match_data(dev);	/* [한국어] **compatible 문자열에 대응하는 drvdata[] 항목을 고른다. 이 한 줄이 이후 모든 세대 분기의 근거다** */

	mutex_init(&imx_pcie->lock);	/* [한국어] LUT 접근을 직렬화할 뮤텍스를 초기화한다 */

	if (imx_pcie->drvdata->ops)	/* [한국어] 표에 host_ops 가 적혀 있으면 */
		pci->pp.ops = imx_pcie->drvdata->ops;	/* [한국어] 그것을 쓰고 */
	else
		pci->pp.ops = &imx_pcie_host_dw_pme_ops;	/* [한국어] post_init 을 채운 기본 묶음을 쓴다 */

	/* Find the PHY if one is defined, only imx7d uses it */
	struct device_node *np __free(device_node) =	/* [한국어] **i.MX7D 의 PHY 레지스터 창을 가리키는 phandle 을 찾는다.** 바로 위 상류 주석대로 이것을 쓰는 것은 i.MX7D 뿐이다 */
		of_parse_phandle(node, "fsl,imx7d-pcie-phy", 0);	/* [한국어] __free(device_node) 로 선언해 함수를 벗어날 때 참조가 자동으로 풀린다 */
	if (np) {	/* [한국어] phandle 이 있으면 */
		struct resource res;	/* [한국어] 그 노드의 자원을 담을 자리 */

		ret = of_address_to_resource(np, 0, &res);	/* [한국어] 노드에서 물리 주소 범위를 꺼내 */
		if (ret) {	/* [한국어] 실패하면 */
			dev_err(dev, "Unable to map PCIe PHY\n");	/* [한국어] 그 사실을 알리고 */
			return ret;	/* [한국어] 그 코드를 돌려준다 */
		}
		imx_pcie->phy_base = devm_ioremap_resource(dev, &res);	/* [한국어] 그 범위를 매핑한다. **이 주소가 ERR010728 우회에서 writel 의 대상이 된다** */
		if (IS_ERR(imx_pcie->phy_base))	/* [한국어] 매핑에 실패하면 */
			return PTR_ERR(imx_pcie->phy_base);	/* [한국어] 그 오류를 돌려준다 */
	}

	/* Fetch GPIOs */
	imx_pcie->reset_gpiod = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);	/* [한국어] **PERST# 리셋 GPIO 를 얻는다.** GPIOD_OUT_HIGH 로 잡아 처음부터 리셋을 건 상태로 둔다 */
	if (IS_ERR(imx_pcie->reset_gpiod))	/* [한국어] 오류이면 */
		return dev_err_probe(dev, PTR_ERR(imx_pcie->reset_gpiod),	/* [한국어] 그 사실을 알리며 */
				     "unable to get reset gpio\n");	/* [한국어] 돌아간다. optional 이라 없는 것은 오류가 아니다 */
	gpiod_set_consumer_name(imx_pcie->reset_gpiod, "PCIe reset");	/* [한국어] 디버깅에서 알아보기 쉽도록 이름을 붙인다 */

	/* Fetch clocks */
	imx_pcie->num_clks = devm_clk_bulk_get_all(dev, &imx_pcie->clks);	/* [한국어] **장치 트리에 적힌 클럭을 모두 모은다.** 개수와 이름이 세대마다 달라 bulk 판을 쓴다 */
	if (imx_pcie->num_clks < 0)	/* [한국어] 음수이면 오류다 */
		return dev_err_probe(dev, imx_pcie->num_clks,	/* [한국어] 그 사실을 알리며 */
				     "failed to get clocks\n");	/* [한국어] 돌아간다 */
	for (i = 0; i < imx_pcie->num_clks; i++)	/* [한국어] 모은 클럭을 훑으며 */
		if (strncmp(imx_pcie->clks[i].id, "extref", 6) == 0)	/* [한국어] **이름이 "extref" 로 시작하는 것이 있으면** */
			imx_pcie->enable_ext_refclk = true;	/* [한국어] 외부 레퍼런스 클럭 구성으로 표시한다. 별도 속성이 아니라 클럭 이름이 근거라는 점이 특이하다 */

	if (imx_check_flag(imx_pcie, IMX_PCIE_FLAG_HAS_PHYDRV)) {	/* [한국어] generic PHY 드라이버를 쓰는 세대이면 */
		imx_pcie->phy = devm_phy_get(dev, "pcie-phy");	/* [한국어] "pcie-phy" 라는 이름으로 얻는다 */
		if (IS_ERR(imx_pcie->phy))	/* [한국어] 오류이면 */
			return dev_err_probe(dev, PTR_ERR(imx_pcie->phy),	/* [한국어] 그 사실을 알리며 */
					     "failed to get pcie phy\n");	/* [한국어] 돌아간다 */
	}

	if (imx_check_flag(imx_pcie, IMX_PCIE_FLAG_HAS_APP_RESET)) {	/* [한국어] LTSSM 을 "apps" 리셋으로 제어하는 세대이면 */
		imx_pcie->apps_reset = devm_reset_control_get_exclusive(dev, "apps");	/* [한국어] 그 리셋 핸들을 얻는다 */
		if (IS_ERR(imx_pcie->apps_reset))	/* [한국어] 오류이면 */
			return dev_err_probe(dev, PTR_ERR(imx_pcie->apps_reset),	/* [한국어] 그 사실을 알리며 */
					     "failed to get pcie apps reset control\n");	/* [한국어] 돌아간다 */
	}

	if (imx_check_flag(imx_pcie, IMX_PCIE_FLAG_HAS_PHY_RESET)) {	/* [한국어] PHY 리셋을 표준 리셋 서브시스템으로 다루는 세대이면 */
		imx_pcie->pciephy_reset = devm_reset_control_get_exclusive(dev, "pciephy");	/* [한국어] "pciephy" 리셋 핸들을 얻는다 */
		if (IS_ERR(imx_pcie->pciephy_reset))	/* [한국어] 오류이면 */
			return dev_err_probe(dev, PTR_ERR(imx_pcie->pciephy_reset),	/* [한국어] 그 사실을 알리며 */
					     "Failed to get PCIEPHY reset control\n");	/* [한국어] 돌아간다 */
	}

	switch (imx_pcie->drvdata->variant) {	/* [한국어] **컨트롤러가 둘인 세대에서만 도메인 번호를 읽는다** */
	case IMX8MQ:	/* [한국어] 8MQ 와 */
	case IMX8MQ_EP:	/* [한국어] 그 엔드포인트 판이 해당한다 */
		domain = of_get_pci_domain_nr(node);	/* [한국어] 장치 트리의 linux,pci-domain 을 읽는다 */
		if (domain < 0 || domain > 1)	/* [한국어] 0 이나 1 이 아니면 이 SoC 에서 있을 수 없는 값이다 */
			return dev_err_probe(dev, -ENODEV, "no \"linux,pci-domain\" property in devicetree\n");	/* [한국어] 그 속성이 없다고 알리며 돌아간다 */

		imx_pcie->controller_id = domain;	/* [한국어] **읽은 번호를 컨트롤러 번호로 삼는다.** imx_pcie_grp_offset() 이 이 값으로 GPR14/GPR16 을 고른다 */
		break;	/* [한국어] 갈래를 벗어난다 */
	default:	/* [한국어] 그 밖의 세대는 */
		break;	/* [한국어] 컨트롤러가 하나뿐이라 번호가 0 인 채로 남는다 */
	}

	if (imx_pcie->drvdata->gpr) {	/* [한국어] **표에 syscon compatible 문자열이 있으면** */
	/* Grab GPR config register range */
		imx_pcie->iomuxc_gpr =	/* [한국어] 그 문자열로 IOMUXC-GPR 노드를 찾아 */
			 syscon_regmap_lookup_by_compatible(imx_pcie->drvdata->gpr);	/* [한국어] regmap 을 얻는다 */
		if (IS_ERR(imx_pcie->iomuxc_gpr))	/* [한국어] 실패하면 */
			return dev_err_probe(dev, PTR_ERR(imx_pcie->iomuxc_gpr),	/* [한국어] 그 사실을 알리며 */
					     "unable to find iomuxc registers\n");	/* [한국어] 돌아간다 */
	}

	if (imx_check_flag(imx_pcie, IMX_PCIE_FLAG_HAS_SERDES)) {	/* [한국어] **GPR syscon 대신 컨트롤러 자기 레지스터 창을 쓰는 세대이면** */
		void __iomem *off = devm_platform_ioremap_resource_byname(pdev, "app");	/* [한국어] "app" 이라는 이름의 MMIO 자원을 직접 매핑한다 */

		if (IS_ERR(off))	/* [한국어] 실패하면 */
			return dev_err_probe(dev, PTR_ERR(off),	/* [한국어] 그 사실을 알리며 */
					     "unable to find serdes registers\n");	/* [한국어] 돌아간다 */

		static const struct regmap_config regmap_config = {	/* [한국어] 그 창 위에 씌울 regmap 설정 */
			.reg_bits = 32,	/* [한국어] 주소가 32비트 */
			.val_bits = 32,	/* [한국어] 값도 32비트 */
			.reg_stride = 4,	/* [한국어] 레지스터 간격이 4바이트 */
		};

		imx_pcie->iomuxc_gpr = devm_regmap_init_mmio(dev, off, &regmap_config);	/* [한국어] **매핑한 창 위에 regmap 을 만들어 같은 인터페이스로 쓴다.** 이렇게 하면 세대에 상관없이 regmap_update_bits() 한 가지로 접근할 수 있다 */
		if (IS_ERR(imx_pcie->iomuxc_gpr))	/* [한국어] 실패하면 */
			return dev_err_probe(dev, PTR_ERR(imx_pcie->iomuxc_gpr),	/* [한국어] 그 사실을 알리며 */
					     "unable to find iomuxc registers\n");	/* [한국어] 돌아간다 */
	}

	/* Grab PCIe PHY Tx Settings */
	if (of_property_read_u32(node, "fsl,tx-deemph-gen1",	/* [한국어] **PHY 송신 파라미터 다섯을 장치 트리에서 읽는다**. Gen1 디엠퍼시스 */
				 &imx_pcie->tx_deemph_gen1))	/* [한국어] 값을 받을 자리. 속성이 없으면 참을 돌려준다 */
		imx_pcie->tx_deemph_gen1 = 0;	/* [한국어] 없으면 0 */

	if (of_property_read_u32(node, "fsl,tx-deemph-gen2-3p5db",	/* [한국어] Gen2 의 3.5dB 디엠퍼시스 */
				 &imx_pcie->tx_deemph_gen2_3p5db))	/* [한국어] 값을 받을 자리 */
		imx_pcie->tx_deemph_gen2_3p5db = 0;	/* [한국어] 없으면 0 */

	if (of_property_read_u32(node, "fsl,tx-deemph-gen2-6db",	/* [한국어] Gen2 의 6dB 디엠퍼시스 */
				 &imx_pcie->tx_deemph_gen2_6db))	/* [한국어] 값을 받을 자리 */
		imx_pcie->tx_deemph_gen2_6db = 20;	/* [한국어] **없으면 20** — 이것만 기본값이 0 이 아니다 */

	if (of_property_read_u32(node, "fsl,tx-swing-full",	/* [한국어] 송신 스윙 full */
				 &imx_pcie->tx_swing_full))	/* [한국어] 값을 받을 자리 */
		imx_pcie->tx_swing_full = 127;	/* [한국어] 없으면 127 */

	if (of_property_read_u32(node, "fsl,tx-swing-low",	/* [한국어] 송신 스윙 low */
				 &imx_pcie->tx_swing_low))	/* [한국어] 값을 받을 자리 */
		imx_pcie->tx_swing_low = 127;	/* [한국어] 없으면 127 */

	/* Limit link speed */
	pci->max_link_speed = 1;	/* [한국어] **기본값을 Gen1 로 둔다** — 장치 트리가 말하지 않으면 Gen1 로 제한된다 */
	of_property_read_u32(node, "fsl,max-link-speed", &pci->max_link_speed);	/* [한국어] 속성이 있으면 그 값으로 덮어쓴다. 반환값을 보지 않으므로 없으면 위 기본값이 남는다 */
	imx_pcie->supports_clkreq = of_property_read_bool(node, "supports-clkreq");	/* [한국어] 보드가 CLKREQ# 를 배선했는지 읽는다 */

	ret = devm_regulator_get_enable_optional(&pdev->dev, "vpcie3v3aux");	/* [한국어] **보조 3.3V 전원은 얻자마자 켜 둔다** — 나중에 켜고 끌 일이 없기 때문이다 */
	if (ret < 0 && ret != -ENODEV)	/* [한국어] 없는 것(-ENODEV)은 오류가 아니지만 그 밖의 오류이면 */
		return dev_err_probe(dev, ret, "failed to enable Vaux supply\n");	/* [한국어] 그 사실을 알리며 돌아간다 */

	imx_pcie->vpcie = devm_regulator_get_optional(&pdev->dev, "vpcie");	/* [한국어] 슬롯 전원 레귤레이터는 붙잡아만 둔다 */
	if (IS_ERR(imx_pcie->vpcie)) {	/* [한국어] 오류이면 */
		if (PTR_ERR(imx_pcie->vpcie) != -ENODEV)	/* [한국어] 없는 것 말고 진짜 오류이면 */
			return PTR_ERR(imx_pcie->vpcie);	/* [한국어] 그 코드를 돌려주고 */
		imx_pcie->vpcie = NULL;	/* [한국어] 없는 것이면 NULL 로 두어 없는 것으로 다룬다 */
	}

	imx_pcie->vph = devm_regulator_get_optional(&pdev->dev, "vph");	/* [한국어] PHY 전원 레귤레이터도 같은 방식으로 얻는다 */
	if (IS_ERR(imx_pcie->vph)) {	/* [한국어] 오류이면 */
		if (PTR_ERR(imx_pcie->vph) != -ENODEV)	/* [한국어] 없는 것 말고 진짜 오류이면 */
			return PTR_ERR(imx_pcie->vph);	/* [한국어] 그 코드를 돌려주고 */
		imx_pcie->vph = NULL;	/* [한국어] 없는 것이면 NULL 로 둔다 */
	}

	platform_set_drvdata(pdev, imx_pcie);	/* [한국어] **이 인스턴스를 플랫폼 장치에 심는다.** to_imx_pcie() 와 LTSSM 제어 함수들이 이 값을 꺼내 쓴다 */

	ret = imx_pcie_attach_pd(dev);	/* [한국어] 전원 도메인을 붙인다 */
	if (ret)	/* [한국어] 실패하면 */
		return ret;	/* [한국어] 그 코드를 돌려준다 */

	pci->use_parent_dt_ranges = true;	/* [한국어] 주소 범위를 부모 노드의 ranges 에서 가져오게 한다 */
	if (imx_pcie->drvdata->mode == DW_PCIE_EP_TYPE) {	/* [한국어] **표가 이 항목을 엔드포인트로 지정했으면** */
		ret = imx_add_pcie_ep(imx_pcie, pdev);	/* [한국어] 엔드포인트로 세운다 */
		if (ret < 0)	/* [한국어] 실패하면 */
			return ret;	/* [한국어] 그 코드를 돌려준다 */

		/*
		 * FIXME: Only single Device (EPF) is supported due to the
		 * Endpoint framework limitation.
		 */
		imx_pcie_add_lut_by_rid(imx_pcie, 0);	/* [한국어] RID 0 으로 LUT 를 하나 넣는다. 바로 위 상류 주석이 FIXME 로 이유를 적는다 — 엔드포인트 프레임워크의 제약 때문에 EPF 가 하나만 지원된다 */
	} else {
		if (imx_check_flag(imx_pcie, IMX_PCIE_FLAG_SKIP_L23_READY))	/* [한국어] L23 Ready 대기를 건너뛰는 세대이면 */
			pci->pp.skip_l23_ready = true;	/* [한국어] DWC 코어 쪽에 그 표시를 전한다 */
		if (imx_check_flag(imx_pcie, IMX_PCIE_FLAG_KEEP_MSI_CAP))	/* [한국어] MSI capability 를 보존해야 하는 세대이면 */
			pci->pp.keep_rp_msi_en = true;	/* [한국어] DWC 코어 쪽에 그 표시를 전한다 */
		pci->pp.use_atu_msg = true;	/* [한국어] PME_Turn_Off 를 iATU 메시지로 보내게 한다 */
		ret = dw_pcie_host_init(&pci->pp);	/* [한국어] **DWC 코어의 RC 초기화를 부른다.** 그 안에서 host_ops 의 init 와 post_init 가 불린다 */
		if (ret < 0)	/* [한국어] 실패하면 */
			return ret;	/* [한국어] 그 코드를 돌려준다 */

		if (pci_msi_enabled()) {	/* [한국어] 커널이 MSI 를 켠 상태이면 */
			u8 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_MSI);	/* [한국어] 루트 포트 자신의 MSI capability 위치를 찾아 */

			val = dw_pcie_readw_dbi(pci, offset + PCI_MSI_FLAGS);	/* [한국어] 현재 값을 읽고 */
			val |= PCI_MSI_FLAGS_ENABLE;	/* [한국어] 활성화 비트를 세워 */
			dw_pcie_writew_dbi(pci, offset + PCI_MSI_FLAGS, val);	/* [한국어] 되쓴다 */
		}
	}

	return 0;	/* [한국어] 모든 단계를 지났으면 성공 */
}

/* [한국어]
 * imx_pcie_shutdown - 재부팅 전에 링크를 내려 부트로더에 깨끗한 상태를 넘긴다
 *
 * @pdev: 플랫폼 장치.
 *
 * 상류 주석이 목적을 밝힌다 — 재부팅할 경우 부트로더가 깨끗한 상태를
 * 받도록 링크를 내린다. 코어 리셋을 걸고 PERST# 를 어서트하는 두 줄이
 * 전부다.
 *
 * 이것이 없으면 부트로더가 이미 링크가 서 있는 컨트롤러를 만나 초기화에
 * 실패하거나 예상 밖 상태에서 시작할 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(시스템 재부팅·전원 종료).
 *
 * 호출 체인:
 *   커널 재부팅 경로 -> platform_driver.shutdown -> [이 함수]
 *     -> imx_pcie_assert_core_reset() -> imx_pcie_assert_perst()
 */
static void imx_pcie_shutdown(struct platform_device *pdev)
{
	struct imx_pcie *imx_pcie = platform_get_drvdata(pdev);	/* [한국어] 플랫폼 장치에 심어 둔 이 드라이버 인스턴스 */

	/* bring down link, so bootloader gets clean state in case of reboot */
	imx_pcie_assert_core_reset(imx_pcie);	/* [한국어] **코어 리셋을 걸어 링크를 내린다.** 바로 위 상류 주석대로 재부팅할 경우 부트로더가 깨끗한 상태를 받게 하려는 것이다 */
	imx_pcie_assert_perst(imx_pcie, true);	/* [한국어] PERST# 도 어서트해 카드를 리셋 상태로 둔다 */
}

static const struct imx_pcie_drvdata drvdata[] = {	/* [한국어] **세대별 차이를 담은 표. 이 파일의 뼈대다.** 배열 인덱스가 곧 세대이며, of_match 표가 compatible 문자열을 이 인덱스로 잇는다 */
	[IMX6Q] = {	/* [한국어] **i.MX6 Quad 항목.** 자체 PHY 와 속도 변경 우회를 쓰고 ERR005723 로 서스펜드가 깨진 세대다 */
		.variant = IMX6Q,	/* [한국어] 이 항목이 어느 세대인지. 배열 인덱스와 같은 값이다 */
		.flags = IMX_PCIE_FLAG_IMX_PHY |	/* [한국어] **이 세대가 무엇을 하고 무엇을 건너뛰는지** — 아래 비트들의 OR 이다 / 자체 PHY 를 이 파일이 직접 다룬다 */
			 IMX_PCIE_FLAG_SPEED_CHANGE_WORKAROUND |	/* [한국어] Gen1 으로 먼저 링크를 세운 뒤 속도를 올린다 */
			 IMX_PCIE_FLAG_BROKEN_SUSPEND |	/* [한국어] ERR005723 — L2 전원 차단을 못 해 리셋으로 대신한다 */
			 IMX_PCIE_FLAG_SUPPORTS_SUSPEND,	/* [한국어] 서스펜드를 지원한다 */
		.dbi_length = 0x200,	/* [한국어] imx_pcie_quirk() 이 루트 버스 장치의 cfg_size 를 이 값으로 제한한다 */
		.gpr = "fsl,imx6q-iomuxc-gpr",	/* [한국어] IOMUXC-GPR syscon 노드를 찾을 compatible 문자열 */
		.ltssm_off = IOMUXC_GPR12,	/* [한국어] LTSSM 활성화 비트가 있는 레지스터의 오프셋 */
		.ltssm_mask = IMX6Q_GPR12_PCIE_CTL_2,	/* [한국어] 그 안에서 LTSSM 을 켜는 비트의 마스크 */
		.mode_off[0] = IOMUXC_GPR12,	/* [한국어] 0번 컨트롤러의 RC/EP 모드 값을 적을 레지스터 오프셋 */
		.mode_mask[0] = IMX6Q_GPR12_DEVICE_TYPE,	/* [한국어] 그 레지스터 안에서 모드 값이 들어갈 필드의 마스크(0번 컨트롤러) */
		.init_phy = imx_pcie_init_phy,	/* [한국어] PHY 파라미터와 클럭 경로를 정하는 세대별 함수 */
		.enable_ref_clk = imx6q_pcie_enable_ref_clk,	/* [한국어] 레퍼런스 클럭을 켜고 끄는 세대별 함수 */
		.core_reset = imx6q_pcie_core_reset,	/* [한국어] 코어·PHY 리셋을 걸고 푸는 세대별 함수 */
	},
	[IMX6SX] = {	/* [한국어] **i.MX6 SoloX 항목.** 6Q 와 달리 BROKEN_SUSPEND 대신 SKIP_L23_READY 를 쓰고 dbi_length 제한이 없다 */
		.variant = IMX6SX,	/* [한국어] 이 항목이 어느 세대인지. 배열 인덱스와 같은 값이다 */
		.flags = IMX_PCIE_FLAG_IMX_PHY |	/* [한국어] **이 세대가 무엇을 하고 무엇을 건너뛰는지** — 아래 비트들의 OR 이다 / 자체 PHY 를 이 파일이 직접 다룬다 */
			 IMX_PCIE_FLAG_SPEED_CHANGE_WORKAROUND |	/* [한국어] Gen1 으로 먼저 링크를 세운 뒤 속도를 올린다 */
			 IMX_PCIE_FLAG_SKIP_L23_READY |	/* [한국어] L23 Ready 대기를 건너뛴다 */
			 IMX_PCIE_FLAG_SUPPORTS_SUSPEND,	/* [한국어] 서스펜드를 지원한다 */
		.gpr = "fsl,imx6q-iomuxc-gpr",	/* [한국어] IOMUXC-GPR syscon 노드를 찾을 compatible 문자열 */
		.ltssm_off = IOMUXC_GPR12,	/* [한국어] LTSSM 활성화 비트가 있는 레지스터의 오프셋 */
		.ltssm_mask = IMX6Q_GPR12_PCIE_CTL_2,	/* [한국어] 그 안에서 LTSSM 을 켜는 비트의 마스크 */
		.mode_off[0] = IOMUXC_GPR12,	/* [한국어] 0번 컨트롤러의 RC/EP 모드 값을 적을 레지스터 오프셋 */
		.mode_mask[0] = IMX6Q_GPR12_DEVICE_TYPE,	/* [한국어] 그 레지스터 안에서 모드 값이 들어갈 필드의 마스크(0번 컨트롤러) */
		.init_phy = imx6sx_pcie_init_phy,	/* [한국어] PHY 파라미터와 클럭 경로를 정하는 세대별 함수 */
		.enable_ref_clk = imx6sx_pcie_enable_ref_clk,	/* [한국어] 레퍼런스 클럭을 켜고 끄는 세대별 함수 */
		.core_reset = imx6sx_pcie_core_reset,	/* [한국어] 코어·PHY 리셋을 걸고 푸는 세대별 함수 */
		.ops = &imx_pcie_host_ops,	/* [한국어] pme_turn_off 를 자체 구현으로 채운 host_ops 묶음을 쓴다 */
	},
	[IMX6QP] = {	/* [한국어] **i.MX6 QuadPlus 항목.** 6SX 와 플래그가 같고 전용 소프트웨어 리셋 비트를 쓴다 */
		.variant = IMX6QP,	/* [한국어] 이 항목이 어느 세대인지. 배열 인덱스와 같은 값이다 */
		.flags = IMX_PCIE_FLAG_IMX_PHY |	/* [한국어] **이 세대가 무엇을 하고 무엇을 건너뛰는지** — 아래 비트들의 OR 이다 / 자체 PHY 를 이 파일이 직접 다룬다 */
			 IMX_PCIE_FLAG_SPEED_CHANGE_WORKAROUND |	/* [한국어] Gen1 으로 먼저 링크를 세운 뒤 속도를 올린다 */
			 IMX_PCIE_FLAG_SKIP_L23_READY |	/* [한국어] L23 Ready 대기를 건너뛴다 */
			 IMX_PCIE_FLAG_SUPPORTS_SUSPEND,	/* [한국어] 서스펜드를 지원한다 */
		.dbi_length = 0x200,	/* [한국어] imx_pcie_quirk() 이 루트 버스 장치의 cfg_size 를 이 값으로 제한한다 */
		.gpr = "fsl,imx6q-iomuxc-gpr",	/* [한국어] IOMUXC-GPR syscon 노드를 찾을 compatible 문자열 */
		.ltssm_off = IOMUXC_GPR12,	/* [한국어] LTSSM 활성화 비트가 있는 레지스터의 오프셋 */
		.ltssm_mask = IMX6Q_GPR12_PCIE_CTL_2,	/* [한국어] 그 안에서 LTSSM 을 켜는 비트의 마스크 */
		.mode_off[0] = IOMUXC_GPR12,	/* [한국어] 0번 컨트롤러의 RC/EP 모드 값을 적을 레지스터 오프셋 */
		.mode_mask[0] = IMX6Q_GPR12_DEVICE_TYPE,	/* [한국어] 그 레지스터 안에서 모드 값이 들어갈 필드의 마스크(0번 컨트롤러) */
		.init_phy = imx_pcie_init_phy,	/* [한국어] PHY 파라미터와 클럭 경로를 정하는 세대별 함수 */
		.enable_ref_clk = imx6q_pcie_enable_ref_clk,	/* [한국어] 레퍼런스 클럭을 켜고 끄는 세대별 함수 */
		.core_reset = imx6qp_pcie_core_reset,	/* [한국어] 코어·PHY 리셋을 걸고 푸는 세대별 함수 */
		.ops = &imx_pcie_host_ops,	/* [한국어] pme_turn_off 를 자체 구현으로 채운 host_ops 묶음을 쓴다 */
	},
	[IMX7D] = {	/* [한국어] **i.MX7 Dual 항목.** 여기서부터 표준 리셋 서브시스템(HAS_APP_RESET/HAS_PHY_RESET)을 쓴다 */
		.variant = IMX7D,	/* [한국어] 이 항목이 어느 세대인지. 배열 인덱스와 같은 값이다 */
		.flags = IMX_PCIE_FLAG_SUPPORTS_SUSPEND |	/* [한국어] **이 세대가 무엇을 하고 무엇을 건너뛰는지** — 아래 비트들의 OR 이다 / 서스펜드를 지원한다 */
			 IMX_PCIE_FLAG_KEEP_MSI_CAP |	/* [한국어] MSI capability 를 보존해야 한다 */
			 IMX_PCIE_FLAG_HAS_APP_RESET |	/* [한국어] LTSSM 을 apps 리셋으로 제어한다 */
			 IMX_PCIE_FLAG_SKIP_L23_READY |	/* [한국어] L23 Ready 대기를 건너뛴다 */
			 IMX_PCIE_FLAG_HAS_PHY_RESET,	/* [한국어] PHY 리셋을 표준 리셋 서브시스템으로 다룬다 */
		.gpr = "fsl,imx7d-iomuxc-gpr",	/* [한국어] IOMUXC-GPR syscon 노드를 찾을 compatible 문자열 */
		.mode_off[0] = IOMUXC_GPR12,	/* [한국어] 0번 컨트롤러의 RC/EP 모드 값을 적을 레지스터 오프셋 */
		.mode_mask[0] = IMX6Q_GPR12_DEVICE_TYPE,	/* [한국어] 그 레지스터 안에서 모드 값이 들어갈 필드의 마스크(0번 컨트롤러) */
		.enable_ref_clk = imx7d_pcie_enable_ref_clk,	/* [한국어] 레퍼런스 클럭을 켜고 끄는 세대별 함수 */
		.core_reset = imx7d_pcie_core_reset,	/* [한국어] 코어·PHY 리셋을 걸고 푸는 세대별 함수 */
	},
	[IMX8MQ] = {	/* [한국어] **i.MX8M Quad 항목.** 컨트롤러가 둘이라 mode_off/mode_mask 를 두 칸 다 채우고 generic PHY 를 쓴다 */
		.variant = IMX8MQ,	/* [한국어] 이 항목이 어느 세대인지. 배열 인덱스와 같은 값이다 */
		.flags = IMX_PCIE_FLAG_HAS_APP_RESET |	/* [한국어] **이 세대가 무엇을 하고 무엇을 건너뛰는지** — 아래 비트들의 OR 이다 / LTSSM 을 apps 리셋으로 제어한다 */
			 IMX_PCIE_FLAG_KEEP_MSI_CAP |	/* [한국어] MSI capability 를 보존해야 한다 */
			 IMX_PCIE_FLAG_HAS_PHY_RESET |	/* [한국어] PHY 리셋을 표준 리셋 서브시스템으로 다룬다 */
			 IMX_PCIE_FLAG_SUPPORTS_SUSPEND,	/* [한국어] 서스펜드를 지원한다 */
		.gpr = "fsl,imx8mq-iomuxc-gpr",	/* [한국어] IOMUXC-GPR syscon 노드를 찾을 compatible 문자열 */
		.mode_off[0] = IOMUXC_GPR12,	/* [한국어] 0번 컨트롤러의 RC/EP 모드 값을 적을 레지스터 오프셋 */
		.mode_mask[0] = IMX6Q_GPR12_DEVICE_TYPE,	/* [한국어] 그 레지스터 안에서 모드 값이 들어갈 필드의 마스크(0번 컨트롤러) */
		.mode_off[1] = IOMUXC_GPR12,	/* [한국어] 1번 컨트롤러의 RC/EP 모드 값을 적을 레지스터 오프셋 */
		.mode_mask[1] = IMX8MQ_GPR12_PCIE2_CTRL_DEVICE_TYPE,	/* [한국어] 그 레지스터 안에서 모드 값이 들어갈 필드의 마스크(1번 컨트롤러) */
		.init_phy = imx8mq_pcie_init_phy,	/* [한국어] PHY 파라미터와 클럭 경로를 정하는 세대별 함수 */
		.enable_ref_clk = imx8mm_pcie_enable_ref_clk,	/* [한국어] 레퍼런스 클럭을 켜고 끄는 세대별 함수 */
		.clr_clkreq_override = imx8mm_pcie_clr_clkreq_override,	/* [한국어] 링크가 선 뒤 CLKREQ# 오버라이드를 푸는 세대별 함수 */
	},
	[IMX8MM] = {	/* [한국어] **i.MX8M Mini 항목.** GPR 로 LTSSM 을 켜지 않고 apps 리셋만 쓴다(ltssm_mask 가 없다) */
		.variant = IMX8MM,	/* [한국어] 이 항목이 어느 세대인지. 배열 인덱스와 같은 값이다 */
		.flags = IMX_PCIE_FLAG_SUPPORTS_SUSPEND |	/* [한국어] **이 세대가 무엇을 하고 무엇을 건너뛰는지** — 아래 비트들의 OR 이다 / 서스펜드를 지원한다 */
			 IMX_PCIE_FLAG_KEEP_MSI_CAP |	/* [한국어] MSI capability 를 보존해야 한다 */
			 IMX_PCIE_FLAG_HAS_PHYDRV |	/* [한국어] generic PHY 드라이버를 쓴다 */
			 IMX_PCIE_FLAG_HAS_APP_RESET,	/* [한국어] LTSSM 을 apps 리셋으로 제어한다 */
		.gpr = "fsl,imx8mm-iomuxc-gpr",	/* [한국어] IOMUXC-GPR syscon 노드를 찾을 compatible 문자열 */
		.mode_off[0] = IOMUXC_GPR12,	/* [한국어] 0번 컨트롤러의 RC/EP 모드 값을 적을 레지스터 오프셋 */
		.mode_mask[0] = IMX6Q_GPR12_DEVICE_TYPE,	/* [한국어] 그 레지스터 안에서 모드 값이 들어갈 필드의 마스크(0번 컨트롤러) */
		.enable_ref_clk = imx8mm_pcie_enable_ref_clk,	/* [한국어] 레퍼런스 클럭을 켜고 끄는 세대별 함수 */
		.clr_clkreq_override = imx8mm_pcie_clr_clkreq_override,	/* [한국어] 링크가 선 뒤 CLKREQ# 오버라이드를 푸는 세대별 함수 */
	},
	[IMX8MP] = {	/* [한국어] **i.MX8M Plus 항목.** 8MM 과 같은 함수 묶음을 쓰고 mode 좌표만 다르다 */
		.variant = IMX8MP,	/* [한국어] 이 항목이 어느 세대인지. 배열 인덱스와 같은 값이다 */
		.flags = IMX_PCIE_FLAG_SUPPORTS_SUSPEND |	/* [한국어] **이 세대가 무엇을 하고 무엇을 건너뛰는지** — 아래 비트들의 OR 이다 / 서스펜드를 지원한다 */
			 IMX_PCIE_FLAG_HAS_PHYDRV |	/* [한국어] generic PHY 드라이버를 쓴다 */
			 IMX_PCIE_FLAG_HAS_APP_RESET,	/* [한국어] LTSSM 을 apps 리셋으로 제어한다 */
		.gpr = "fsl,imx8mp-iomuxc-gpr",	/* [한국어] IOMUXC-GPR syscon 노드를 찾을 compatible 문자열 */
		.mode_off[0] = IOMUXC_GPR12,	/* [한국어] 0번 컨트롤러의 RC/EP 모드 값을 적을 레지스터 오프셋 */
		.mode_mask[0] = IMX6Q_GPR12_DEVICE_TYPE,	/* [한국어] 그 레지스터 안에서 모드 값이 들어갈 필드의 마스크(0번 컨트롤러) */
		.enable_ref_clk = imx8mm_pcie_enable_ref_clk,	/* [한국어] 레퍼런스 클럭을 켜고 끄는 세대별 함수 */
		.clr_clkreq_override = imx8mm_pcie_clr_clkreq_override,	/* [한국어] 링크가 선 뒤 CLKREQ# 오버라이드를 푸는 세대별 함수 */
	},
	[IMX8Q] = {	/* [한국어] **i.MX8Q 항목.** mode_mask 를 아예 두지 않아 generic PHY 드라이버가 RC/EP 모드까지 맡는다 */
		.variant = IMX8Q,	/* [한국어] 이 항목이 어느 세대인지. 배열 인덱스와 같은 값이다 */
		.flags = IMX_PCIE_FLAG_HAS_PHYDRV |	/* [한국어] **이 세대가 무엇을 하고 무엇을 건너뛰는지** — 아래 비트들의 OR 이다 / generic PHY 드라이버를 쓴다 */
			 IMX_PCIE_FLAG_CPU_ADDR_FIXUP |	/* [한국어] CPU 주소 보정이 필요하다(이 파일에서 읽는 곳은 없다) */
			 IMX_PCIE_FLAG_SUPPORTS_SUSPEND,	/* [한국어] 서스펜드를 지원한다 */
	},
	[IMX95] = {	/* [한국어] **i.MX95 항목.** HAS_SERDES 로 컨트롤러 자기 레지스터 창을 쓰고 HAS_LUT 로 RID 변환표를 가진다 */
		.variant = IMX95,	/* [한국어] 이 항목이 어느 세대인지. 배열 인덱스와 같은 값이다 */
		.flags = IMX_PCIE_FLAG_HAS_SERDES |	/* [한국어] **이 세대가 무엇을 하고 무엇을 건너뛰는지** — 아래 비트들의 OR 이다 / GPR syscon 대신 컨트롤러 자기 레지스터 창을 쓴다 */
			 IMX_PCIE_FLAG_HAS_LUT |	/* [한국어] RID -> StreamID 변환표가 있다 */
			 IMX_PCIE_FLAG_8GT_ECN_ERR051586 |	/* [한국어] ERR051586 — ZRX-DC 비호환 비트를 지워야 한다 */
			 IMX_PCIE_FLAG_SUPPORTS_SUSPEND,	/* [한국어] 서스펜드를 지원한다 */
		.ltssm_off = IMX95_PE0_GEN_CTRL_3,	/* [한국어] LTSSM 활성화 비트가 있는 레지스터의 오프셋 */
		.ltssm_mask = IMX95_PCIE_LTSSM_EN,	/* [한국어] 그 안에서 LTSSM 을 켜는 비트의 마스크 */
		.mode_off[0]  = IMX95_PE0_GEN_CTRL_1,	/* [한국어] 0번 컨트롤러의 RC/EP 모드 값을 적을 레지스터 오프셋 */
		.mode_mask[0] = IMX95_PCIE_DEVICE_TYPE,	/* [한국어] 그 레지스터 안에서 모드 값이 들어갈 필드의 마스크(0번 컨트롤러) */
		.core_reset = imx95_pcie_core_reset,	/* [한국어] 코어·PHY 리셋을 걸고 푸는 세대별 함수 */
		.init_phy = imx95_pcie_init_phy,	/* [한국어] PHY 파라미터와 클럭 경로를 정하는 세대별 함수 */
		.wait_pll_lock = imx95_pcie_wait_for_phy_pll_lock,	/* [한국어] PLL 잠금을 기다리는 세대별 함수. 결과를 host_init 이 확인한다 */
		.enable_ref_clk = imx95_pcie_enable_ref_clk,	/* [한국어] 레퍼런스 클럭을 켜고 끄는 세대별 함수 */
		.clr_clkreq_override = imx95_pcie_clr_clkreq_override,	/* [한국어] 링크가 선 뒤 CLKREQ# 오버라이드를 푸는 세대별 함수 */
	},
	[IMX8MQ_EP] = {	/* [한국어] **8MQ 엔드포인트 항목.** mode 가 EP 이고 epc_features 를 가진다 */
		.variant = IMX8MQ_EP,	/* [한국어] 이 항목이 어느 세대인지. 배열 인덱스와 같은 값이다 */
		.flags = IMX_PCIE_FLAG_HAS_APP_RESET |	/* [한국어] **이 세대가 무엇을 하고 무엇을 건너뛰는지** — 아래 비트들의 OR 이다 / LTSSM 을 apps 리셋으로 제어한다 */
			 IMX_PCIE_FLAG_HAS_PHY_RESET,	/* [한국어] PHY 리셋을 표준 리셋 서브시스템으로 다룬다 */
		.mode = DW_PCIE_EP_TYPE,	/* [한국어] 이 항목을 엔드포인트로 쓴다는 표시 */
		.gpr = "fsl,imx8mq-iomuxc-gpr",	/* [한국어] IOMUXC-GPR syscon 노드를 찾을 compatible 문자열 */
		.mode_off[0] = IOMUXC_GPR12,	/* [한국어] 0번 컨트롤러의 RC/EP 모드 값을 적을 레지스터 오프셋 */
		.mode_mask[0] = IMX6Q_GPR12_DEVICE_TYPE,	/* [한국어] 그 레지스터 안에서 모드 값이 들어갈 필드의 마스크(0번 컨트롤러) */
		.mode_off[1] = IOMUXC_GPR12,	/* [한국어] 1번 컨트롤러의 RC/EP 모드 값을 적을 레지스터 오프셋 */
		.mode_mask[1] = IMX8MQ_GPR12_PCIE2_CTRL_DEVICE_TYPE,	/* [한국어] 그 레지스터 안에서 모드 값이 들어갈 필드의 마스크(1번 컨트롤러) */
		.epc_features = &imx8q_pcie_epc_features,	/* [한국어] 엔드포인트 프레임워크에 알릴 능력표 */
		.init_phy = imx8mq_pcie_init_phy,	/* [한국어] PHY 파라미터와 클럭 경로를 정하는 세대별 함수 */
		.enable_ref_clk = imx8mm_pcie_enable_ref_clk,	/* [한국어] 레퍼런스 클럭을 켜고 끄는 세대별 함수 */
	},
	[IMX8MM_EP] = {	/* [한국어] **8MM 엔드포인트 항목** */
		.variant = IMX8MM_EP,	/* [한국어] 이 항목이 어느 세대인지. 배열 인덱스와 같은 값이다 */
		.flags = IMX_PCIE_FLAG_HAS_APP_RESET |	/* [한국어] **이 세대가 무엇을 하고 무엇을 건너뛰는지** — 아래 비트들의 OR 이다 / LTSSM 을 apps 리셋으로 제어한다 */
			 IMX_PCIE_FLAG_HAS_PHYDRV,	/* [한국어] generic PHY 드라이버를 쓴다 */
		.mode = DW_PCIE_EP_TYPE,	/* [한국어] 이 항목을 엔드포인트로 쓴다는 표시 */
		.gpr = "fsl,imx8mm-iomuxc-gpr",	/* [한국어] IOMUXC-GPR syscon 노드를 찾을 compatible 문자열 */
		.mode_off[0] = IOMUXC_GPR12,	/* [한국어] 0번 컨트롤러의 RC/EP 모드 값을 적을 레지스터 오프셋 */
		.mode_mask[0] = IMX6Q_GPR12_DEVICE_TYPE,	/* [한국어] 그 레지스터 안에서 모드 값이 들어갈 필드의 마스크(0번 컨트롤러) */
		.epc_features = &imx8m_pcie_epc_features,	/* [한국어] 엔드포인트 프레임워크에 알릴 능력표 */
		.enable_ref_clk = imx8mm_pcie_enable_ref_clk,	/* [한국어] 레퍼런스 클럭을 켜고 끄는 세대별 함수 */
	},
	[IMX8MP_EP] = {	/* [한국어] **8MP 엔드포인트 항목** */
		.variant = IMX8MP_EP,	/* [한국어] 이 항목이 어느 세대인지. 배열 인덱스와 같은 값이다 */
		.flags = IMX_PCIE_FLAG_HAS_APP_RESET |	/* [한국어] **이 세대가 무엇을 하고 무엇을 건너뛰는지** — 아래 비트들의 OR 이다 / LTSSM 을 apps 리셋으로 제어한다 */
			 IMX_PCIE_FLAG_HAS_PHYDRV,	/* [한국어] generic PHY 드라이버를 쓴다 */
		.mode = DW_PCIE_EP_TYPE,	/* [한국어] 이 항목을 엔드포인트로 쓴다는 표시 */
		.gpr = "fsl,imx8mp-iomuxc-gpr",	/* [한국어] IOMUXC-GPR syscon 노드를 찾을 compatible 문자열 */
		.mode_off[0] = IOMUXC_GPR12,	/* [한국어] 0번 컨트롤러의 RC/EP 모드 값을 적을 레지스터 오프셋 */
		.mode_mask[0] = IMX6Q_GPR12_DEVICE_TYPE,	/* [한국어] 그 레지스터 안에서 모드 값이 들어갈 필드의 마스크(0번 컨트롤러) */
		.epc_features = &imx8m_pcie_epc_features,	/* [한국어] 엔드포인트 프레임워크에 알릴 능력표 */
		.enable_ref_clk = imx8mm_pcie_enable_ref_clk,	/* [한국어] 레퍼런스 클럭을 켜고 끄는 세대별 함수 */
	},
	[IMX8Q_EP] = {	/* [한국어] **8Q 엔드포인트 항목.** 능력표가 8M 계열과 따로다 */
		.variant = IMX8Q_EP,	/* [한국어] 이 항목이 어느 세대인지. 배열 인덱스와 같은 값이다 */
		.flags = IMX_PCIE_FLAG_HAS_PHYDRV,	/* [한국어] **이 세대가 무엇을 하고 무엇을 건너뛰는지** — 아래 비트들의 OR 이다 / generic PHY 드라이버를 쓴다 */
		.mode = DW_PCIE_EP_TYPE,	/* [한국어] 이 항목을 엔드포인트로 쓴다는 표시 */
		.epc_features = &imx8q_pcie_epc_features,	/* [한국어] 엔드포인트 프레임워크에 알릴 능력표 */
	},
	[IMX95_EP] = {	/* [한국어] **i.MX95 엔드포인트 항목.** RC 판과 달리 LUT 마스크를 Device ID 만 비교하도록 쓴다 */
		.variant = IMX95_EP,	/* [한국어] 이 항목이 어느 세대인지. 배열 인덱스와 같은 값이다 */
		.flags = IMX_PCIE_FLAG_HAS_SERDES |	/* [한국어] **이 세대가 무엇을 하고 무엇을 건너뛰는지** — 아래 비트들의 OR 이다 / GPR syscon 대신 컨트롤러 자기 레지스터 창을 쓴다 */
			 IMX_PCIE_FLAG_8GT_ECN_ERR051586 |	/* [한국어] ERR051586 — ZRX-DC 비호환 비트를 지워야 한다 */
			 IMX_PCIE_FLAG_SUPPORT_64BIT,	/* [한국어] 64비트 DMA 를 지원한다 */
		.ltssm_off = IMX95_PE0_GEN_CTRL_3,	/* [한국어] LTSSM 활성화 비트가 있는 레지스터의 오프셋 */
		.ltssm_mask = IMX95_PCIE_LTSSM_EN,	/* [한국어] 그 안에서 LTSSM 을 켜는 비트의 마스크 */
		.mode_off[0]  = IMX95_PE0_GEN_CTRL_1,	/* [한국어] 0번 컨트롤러의 RC/EP 모드 값을 적을 레지스터 오프셋 */
		.mode_mask[0] = IMX95_PCIE_DEVICE_TYPE,	/* [한국어] 그 레지스터 안에서 모드 값이 들어갈 필드의 마스크(0번 컨트롤러) */
		.init_phy = imx95_pcie_init_phy,	/* [한국어] PHY 파라미터와 클럭 경로를 정하는 세대별 함수 */
		.core_reset = imx95_pcie_core_reset,	/* [한국어] 코어·PHY 리셋을 걸고 푸는 세대별 함수 */
		.wait_pll_lock = imx95_pcie_wait_for_phy_pll_lock,	/* [한국어] PLL 잠금을 기다리는 세대별 함수. 결과를 host_init 이 확인한다 */
		.epc_features = &imx95_pcie_epc_features,	/* [한국어] 엔드포인트 프레임워크에 알릴 능력표 */
		.enable_ref_clk = imx95_pcie_enable_ref_clk,	/* [한국어] 레퍼런스 클럭을 켜고 끄는 세대별 함수 */
		.mode = DW_PCIE_EP_TYPE,	/* [한국어] 이 항목을 엔드포인트로 쓴다는 표시 */
	},
};

static const struct of_device_id imx_pcie_of_match[] = {	/* [한국어] **장치 트리의 compatible 문자열을 drvdata[] 항목으로 잇는 표.** probe 의 of_device_get_match_data() 가 이 표를 보고 세대를 정한다 */
	{ .compatible = "fsl,imx6q-pcie",  .data = &drvdata[IMX6Q],  },	/* [한국어] i.MX6 Quad */
	{ .compatible = "fsl,imx6sx-pcie", .data = &drvdata[IMX6SX], },	/* [한국어] i.MX6 SoloX */
	{ .compatible = "fsl,imx6qp-pcie", .data = &drvdata[IMX6QP], },	/* [한국어] i.MX6 QuadPlus */
	{ .compatible = "fsl,imx7d-pcie",  .data = &drvdata[IMX7D],  },	/* [한국어] i.MX7 Dual */
	{ .compatible = "fsl,imx8mq-pcie", .data = &drvdata[IMX8MQ], },	/* [한국어] i.MX8M Quad */
	{ .compatible = "fsl,imx8mm-pcie", .data = &drvdata[IMX8MM], },	/* [한국어] i.MX8M Mini */
	{ .compatible = "fsl,imx8mp-pcie", .data = &drvdata[IMX8MP], },	/* [한국어] i.MX8M Plus */
	{ .compatible = "fsl,imx8q-pcie", .data = &drvdata[IMX8Q], },	/* [한국어] i.MX8Q */
	{ .compatible = "fsl,imx95-pcie", .data = &drvdata[IMX95], },	/* [한국어] i.MX95 */
	{ .compatible = "fsl,imx8mq-pcie-ep", .data = &drvdata[IMX8MQ_EP], },	/* [한국어] **여기부터가 엔드포인트 판** — compatible 문자열이 "-ep" 로 끝난다. 8MQ EP */
	{ .compatible = "fsl,imx8mm-pcie-ep", .data = &drvdata[IMX8MM_EP], },	/* [한국어] 8MM EP */
	{ .compatible = "fsl,imx8mp-pcie-ep", .data = &drvdata[IMX8MP_EP], },	/* [한국어] 8MP EP */
	{ .compatible = "fsl,imx8q-pcie-ep", .data = &drvdata[IMX8Q_EP], },	/* [한국어] 8Q EP */
	{ .compatible = "fsl,imx95-pcie-ep", .data = &drvdata[IMX95_EP], },	/* [한국어] i.MX95 EP */
	{},	/* [한국어] 표의 끝을 알리는 빈 항목 */
};

static struct platform_driver imx_pcie_driver = {	/* [한국어] **플랫폼 드라이버 등록 구조체** */
	.driver = {	/* [한국어] 드라이버 코어 쪽 정보 */
		.name	= "imx6q-pcie",	/* [한국어] 드라이버 이름. 파일 이름과 마찬가지로 6Q 시절 이름이 남아 있다 */
		.of_match_table = imx_pcie_of_match,	/* [한국어] 위 compatible 표 */
		.suppress_bind_attrs = true,	/* [한국어] sysfs 로 수동 바인드·언바인드를 막는다 — PCIe 호스트 브리지를 임의로 떼면 안 되기 때문이다 */
		.pm = &imx_pcie_pm_ops,	/* [한국어] 서스펜드·리줌 콜백 묶음 */
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,	/* [한국어] **다른 probe 와 병렬로 돌아도 된다고 알린다.** 링크 대기에 수백 ms 가 걸릴 수 있어 부팅 시간에 영향이 크다 */
	},
	.probe    = imx_pcie_probe,	/* [한국어] 진입점 */
	.shutdown = imx_pcie_shutdown,	/* [한국어] 재부팅 전에 링크를 내린다 */
};

/* [한국어]
 * imx_pcie_quirk - i.MX 6Quad 에서 config 공간을 읽는 길이를 제한한다
 *
 * @dev: 방금 발견된 PCI 장치.
 *
 * DECLARE_PCI_FIXUP_CLASS_HEADER 로 등록된 PCI 코어 quirk 이며, 벤더가
 * Synopsys(0xabcd 장치, PCI-to-PCI 브리지 클래스)인 장치를 발견했을 때
 * 불린다 — DWC 루트 포트가 그 정체로 나타나기 때문이다.
 *
 * **엉뚱한 컨트롤러를 건드리지 않도록 두 겹으로 확인한다.**
 *   1. 버스의 부모(PCI 브리지)와 그 부모(플랫폼 장치)가 모두 있어야 한다.
 *   2. 그 플랫폼 장치의 드라이버가 imx_pcie_driver 여야 한다.
 *      상류 주석이 두 검사의 뜻을 각각 적어 두었다.
 *
 * 그 다음 루트 버스의 장치일 때만 cfg_size 를 제한한다. 상류 주석이
 * 이유를 밝힌다 — i.MX 6Quad 에서 커널이 레지스터 집합 너머를 읽으면
 * 어보트가 나므로 그것을 막는 것이다. 제한 값은 drvdata 의 dbi_length
 * (6Q/6QP 는 0x200)이며, 0 인 세대에서는 아무 일도 하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 열거).
 *
 * 호출 체인:  PCI 코어의 fixup 처리부 -> [이 함수]
 */
static void imx_pcie_quirk(struct pci_dev *dev)
{
	struct pci_bus *bus = dev->bus;	/* [한국어] 이 장치가 붙은 버스 */
	struct dw_pcie_rp *pp = bus->sysdata;	/* [한국어] 그 버스의 sysdata 는 DWC 루트 포트 구조체다 */

	/* Bus parent is the PCI bridge, its parent is this platform driver */
	if (!bus->dev.parent || !bus->dev.parent->parent)	/* [한국어] **바로 위 상류 주석대로 버스의 부모는 PCI 브리지이고 그 부모가 이 플랫폼 드라이버**다. 둘 중 하나라도 없으면 우리가 다룰 장치가 아니다 */
		return;	/* [한국어] 그대로 나간다 */

	/* Make sure we only quirk devices associated with this driver */
	if (bus->dev.parent->parent->driver != &imx_pcie_driver.driver)	/* [한국어] **바로 위 상류 주석대로 이 드라이버가 관리하는 장치인지 확인한다.** 같은 Synopsys 정체를 가진 다른 컨트롤러를 건드리지 않기 위해서다 */
		return;	/* [한국어] 그대로 나간다 */

	if (pci_is_root_bus(bus)) {	/* [한국어] **루트 버스의 장치일 때만** 제한한다 */
		struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* [한국어] 루트 포트에서 DWC 장치 구조체를 얻고 */
		struct imx_pcie *imx_pcie = to_imx_pcie(pci);	/* [한국어] 거기서 이 드라이버 인스턴스를 얻는다 */

		/*
		 * Limit config length to avoid the kernel reading beyond
		 * the register set and causing an abort on i.MX 6Quad
		 */
		if (imx_pcie->drvdata->dbi_length) {	/* [한국어] 표에 dbi_length 가 적힌 세대이면 — 6Q 와 6QP 만 0x200 을 가진다 */
			dev->cfg_size = imx_pcie->drvdata->dbi_length;	/* [한국어] **config 공간을 읽을 수 있는 길이를 그 값으로 줄인다.** 바로 위 상류 주석대로 i.MX 6Quad 에서 레지스터 집합 너머를 읽으면 어보트가 나기 때문이다 */
			dev_info(&dev->dev, "Limiting cfg_size to %d\n",	/* [한국어] 제한했음을 알린다 */
					dev->cfg_size);	/* [한국어] 제한한 값 */
		}
	}
}
DECLARE_PCI_FIXUP_CLASS_HEADER(PCI_VENDOR_ID_SYNOPSYS, 0xabcd,	/* [한국어] **PCI 코어가 이 정체의 장치를 발견했을 때 위 quirk 을 부르게 한다.** DWC 루트 포트가 Synopsys 벤더의 0xabcd 장치, PCI-PCI 브리지 클래스로 나타난다 */
			PCI_CLASS_BRIDGE_PCI, 8, imx_pcie_quirk);	/* [한국어] 클래스 비교 자릿수 8 은 상위 16비트(클래스·서브클래스)까지만 본다는 뜻이다 */

/* [한국어]
 * imx_pcie_init - 모듈 진입점. ARM 에서는 어보트 훅을 먼저 걸고 드라이버를 등록한다
 *
 * @return: platform_driver_register() 의 반환값. CONFIG_ARM 에서 이
 *          드라이버가 쓰이는 노드가 하나도 없으면 -ENODEV.
 *
 * module_init 이 아니라 **device_initcall** 로 등록된다.
 *
 * CONFIG_ARM 에서만 도는 부분이 앞에 있다. 장치 트리에 이 드라이버가
 * 맡을 노드가 있는지 먼저 확인하고, 없으면 -ENODEV 로 나간다 — 훅을 걸
 * 이유가 없기 때문이다. 있으면 hook_fault_code() 로 fsr 8(비-라인페치
 * 외부 어보트)에 imx6q_pcie_abort_handler() 를 건다.
 *
 * **훅을 probe 가 아니라 여기서 거는 이유**를 상류 주석이 밝힌다.
 * probe 는 미뤄질 수 있는데(deferred probe), 그 사이에 __init 메모리가
 * 해제되면 hook_fault_code() 를 부를 수 없게 된다. 그리고
 * imx6q_pcie_abort_handler() 는 드라이버 상태를 전혀 만지지 않는
 * 무해한 함수이므로, 초기화되지 않은 상태를 건드릴 위험 없이 미리
 * 걸어 두어도 된다.
 *
 * 실행 컨텍스트: 커널 초기화(프로세스 컨텍스트), __init 이다.
 *
 * 호출 체인:
 *   device_initcall -> [이 함수] -> of_find_matching_node()
 *     -> hook_fault_code() -> platform_driver_register()
 */
static int __init imx_pcie_init(void)
{
#ifdef CONFIG_ARM	/* [한국어] **어보트 훅은 32비트 ARM 에서만 건다** */
	struct device_node *np;	/* [한국어] 장치 트리 노드를 담을 자리 */

	np = of_find_matching_node(NULL, imx_pcie_of_match);	/* [한국어] **이 드라이버가 맡을 노드가 있는지 먼저 본다** */
	if (!np)	/* [한국어] 없으면 */
		return -ENODEV;	/* [한국어] 훅을 걸 이유가 없으므로 장치 없음으로 나간다 */
	of_node_put(np);	/* [한국어] 찾은 노드의 참조를 놓아 준다 */

	/*
	 * Since probe() can be deferred we need to make sure that
	 * hook_fault_code is not called after __init memory is freed
	 * by kernel and since imx6q_pcie_abort_handler() is a no-op,
	 * we can install the handler here without risking it
	 * accessing some uninitialized driver state.
	 */
	hook_fault_code(8, imx6q_pcie_abort_handler, SIGBUS, 0,	/* [한국어] **fsr 8(비-라인페치 외부 어보트)에 핸들러를 건다.** 바로 위 상류 주석이 probe 가 아니라 여기서 거는 이유를 밝힌다 — probe 는 미뤄질 수 있는데 그 사이 __init 메모리가 해제되면 이 함수를 부를 수 없고, 핸들러 자신이 드라이버 상태를 만지지 않아 미리 걸어도 안전하다 */
			"external abort on non-linefetch");	/* [한국어] 어보트 이름 */
#endif

	return platform_driver_register(&imx_pcie_driver);	/* [한국어] **플랫폼 드라이버를 등록한다.** 이 뒤로 커널이 맞는 노드를 찾아 probe 를 부른다 */
}
device_initcall(imx_pcie_init);	/* [한국어] module_init 이 아니라 device_initcall 로 등록한다 */
