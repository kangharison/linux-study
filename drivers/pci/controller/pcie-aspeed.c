// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2025 Aspeed Technology Inc.
 */

/*
 * [한국어 설명] BMC SoC 안의 PCIe Root Complex 호스트 컨트롤러 (pcie-aspeed.c)
 *
 * === 파일의 역할 ===
 * Aspeed 의 BMC(Baseboard Management Controller) SoC — AST2600 과 AST2700 —
 * 에 들어 있는 PCIe Root Complex 의 호스트 컨트롤러 드라이버다.
 * 서버 메인보드에 얹힌 BMC 가 자기 PCIe 버스에 붙은 장치들을 열거하고
 * 쓸 수 있게 해 준다.
 *
 * 커널 PCI 코어가 요구하는 것은 struct pci_ops 의 read/write 이고, 이
 * 파일은 그것을 "H2X"(Host to PCIe) 컨트롤러 레지스터를 통해 구현한다.
 * pcie-altera.c 처럼 여기도 config 접근이 ECAM 같은 창이 아니라 레지스터
 * 창구를 통해 이뤄지지만, TLP 를 통째로 손으로 조립하는 대신 하드웨어가
 * 필드를 나눠 받는 형태다 — TLP 의 첫 DWORD(fmt/type 과 length)만 만들어
 * 넣고 나머지는 BDF·byte enable·데이터를 각각의 레지스터에 쓴다.
 *
 * 두 세대의 접근 방식이 다르다.
 *   AST2600 — Root Port 와 하위 장치를 같은 경로(aspeed_ast2600_conf)로
 *     처리하고, fmt/type 값만 Type 0/Type 1 로 갈아 끼운다. TX 디스크립터
 *     레지스터 넷(TX_DESC0~3)과 데이터 레지스터에 나눠 쓴 뒤 트리거를
 *     친다. 완료는 두 단계 폴링(TX idle 다음 RX done)으로 기다린다.
 *   AST2700 — Root Port 자신은 "CFGI"(Config Internal) 레지스터로 훨씬
 *     단순하게 접근하고(aspeed_ast2700_config), 하위 장치는 "CFGE"
 *     (Config External) 레지스터로 TLP 를 밀어 넣는다
 *     (aspeed_ast2700_child_config). 커널의 pci_host_bridge 가 제공하는
 *     ops/child_ops 두 벌 구조를 그대로 쓴다.
 *
 * config 접근 외에 이 파일이 맡는 일이 셋 더 있다.
 *   INTx  — 하위 장치의 INTA~INTD 를 받아 커널 IRQ 로 옮긴다.
 *           altera 와 달리 진짜 irq_chip(mask/unmask/ack)을 구현한다.
 *   MSI   — 최대 64개 벡터를 비트맵으로 관리하고, MSI 부모 도메인을 만든다.
 *           EP 가 특정 주소에 쓰면 컨트롤러가 그것을 인터럽트로 바꾼다.
 *   주소 변환 — AHB(SoC 내부 버스) 주소와 PCI 버스 주소 사이의 매핑을
 *           세운다(aspeed_pcie_map_ranges 와 세대별 구현).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록: 장치 트리에 aspeed,ast2600-pcie 또는 aspeed,ast2700-pcie 노드
 *         -> 플랫폼 버스가 aspeed_pcie_probe() 를 부른다
 *            -> of_device_get_match_data() 로 세대별
 *               aspeed_pcie_rc_platform 을 고른다
 *            -> devm_pci_alloc_host_bridge() 로 브리지와 이 드라이버의
 *               상태를 한 번에 잡는다
 *            -> platform->setup() (세대별) — 클록/경로/브리지를 켜고
 *               pci_ops 와 child_ops 를 건다
 *            -> aspeed_pcie_map_ranges() 로 주소 변환을 세운다
 *            -> aspeed_pcie_parse_dt() 로 포트마다 클록·PHY·PERST 를 잡고
 *               aspeed_pcie_port_init() 으로 링크를 올린다
 *            -> aspeed_pcie_init_irq_domain() 으로 INTx 와 MSI 도메인
 *            -> devm_request_irq() 로 인터럽트를 걸고
 *            -> pci_host_probe() 로 PCI 코어에 넘긴다
 *
 * config 접근: PCI 코어
 *         -> host->ops(루트 버스) 또는 host->child_ops(그 아래 버스)
 *            -> 세대별 rd_conf/wr_conf
 *               -> aspeed_ast2600_conf() 또는
 *                  aspeed_ast2700_config()/aspeed_ast2700_child_config()
 *
 * 인터럽트: 하위 장치의 INTx 또는 MSI
 *         -> aspeed_pcie_intr_handler()(공유 IRQ 핸들러)
 *            -> INTx 넷과 MSI 64개를 상태 레지스터에서 읽어
 *               generic_handle_domain_irq() 로 각 장치 핸들러에 넘긴다
 *
 * 실행 컨텍스트: probe 는 프로세스 컨텍스트이고 msleep/mdelay 로 잠든다.
 * config 접근은 PCI 코어가 스핀락을 쥔 채 부르므로 잠들 수 없고, 그래서
 * 완료 대기가 readl_poll_timeout 의 sleep_us=0(순수 스핀) 형태다.
 * aspeed_pcie_intr_handler() 는 인터럽트 컨텍스트다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어 전체가 host->ops 와 host->child_ops 를 통해서만 이
 *   하드웨어에 닿는다. 플랫폼 버스와 장치 트리가 진입점이다.
 * 아래쪽:
 *   readl/writel — "H2X" 컨트롤러 레지스터 창(devm_platform_ioremap_resource).
 *   regmap/syscon — AST2600 의 AHBC 와 AST2700 의 PCIECFG(SCU)를
 *     phandle 로 찾아 조작한다. 그 두 블록은 이 컨트롤러 밖에 있어
 *     레지스터 창을 직접 매핑하지 않고 syscon 을 거친다.
 *   clk / phy / reset — 포트마다 클록을 켜고 PHY 를 RC 모드로 세우고
 *     PERST 를 풀어 링크를 올린다.
 *   irqdomain 과 irq-msi-lib — INTx 도메인과 MSI 부모 도메인.
 *   "../pci.h" — PCI 코어 내부 헬퍼.
 * 공유 상태: struct aspeed_pcie 하나이며 pci_host_bridge 의 private
 *   영역에 얹혀 있다. 그 안의 msi_irq_in_use 비트맵만 mutex 로 보호된다.
 *
 * === 주요 함수/구조체 요약 ===
 * aspeed_pcie_probe()        : 플랫폼 드라이버 진입점. 위 등록 절차 전부.
 * aspeed_ast2600_setup() 와 aspeed_ast2700_setup()
 *                            : 세대별 하드웨어 초기화. AHBC/SCU 를 열고
 *                              브리지를 켜고 pci_ops 를 건다.
 * aspeed_pcie_parse_dt() / aspeed_pcie_parse_port() / aspeed_pcie_port_init()
 *                            : 장치 트리의 포트 노드마다 클록·PHY·PERST 를
 *                              잡고 링크를 올린다.
 * aspeed_pcie_get_bdf_offset(): BDF 와 오프셋을 TLP 셋째 DWORD 형태로 만든다.
 * aspeed_ast2600_conf()      : AST2600 의 config 접근 본체. TX 디스크립터를
 *                              채우고 트리거한 뒤 두 단계로 완료를 기다린다.
 * aspeed_ast2600_rd_conf() 와 그 형제 셋
 *                            : 위 본체에 fmt/type 과 방향을 넘기는 네 래퍼.
 * aspeed_ast2700_config()    : AST2700 의 Root Port 전용 경로(CFGI).
 * aspeed_ast2700_child_config(): AST2700 의 하위 장치 경로(CFGE).
 * aspeed_ast2700_rd_conf() 와 그 형제 셋
 *                            : 그 둘에 방향을 넘기는 네 래퍼.
 * aspeed_pcie_intr_handler() : INTx 넷과 MSI 64개를 함께 처리하는 IRQ 핸들러.
 * aspeed_pcie_intx_irq_ack/mask/unmask() 와 aspeed_pcie_intx_map()
 *                            : INTx irq_chip 구현.
 * aspeed_irq_compose_msi_msg(): EP 에 알려 줄 MSI 주소와 데이터를 만든다.
 * aspeed_irq_msi_domain_alloc() 와 aspeed_irq_msi_domain_free()
 *                            : MSI 벡터를 비트맵에서 떼고 돌려준다.
 * aspeed_pcie_msi_init() / aspeed_pcie_msi_free() / aspeed_pcie_init_irq_domain()
 *                            : 두 도메인의 생성과 해제.
 * aspeed_pcie_map_ranges() 와 세대별 map_ranges 콜백 둘
 *                            : AHB 와 PCI 주소 공간 사이의 변환을 세운다.
 * aspeed_host_reset()        : H2X 컨트롤러를 리셋한다.
 *
 * struct aspeed_pcie             : 이 컨트롤러 하나의 상태 전부.
 * struct aspeed_pcie_port        : 포트(슬롯) 하나의 클록·PHY·리셋.
 * struct aspeed_pcie_rc_platform : 세대별 콜백과 레지스터 오프셋 묶음.
 *
 * === config 접근이 어떻게 TLP 가 되는가 ===
 * 이 파일도 pcie-altera.c 처럼 Configuration Request 를 소프트웨어가
 * 만들어 내지만, 나누는 방식이 다르다.
 *
 * 공통 조립 도구가 파일 위쪽에 있다.
 *   ASPEED_TLP_FMT_TYPE(fmt, type) = ((fmt & 0x7) << 5) | (type & 0x1f)
 *     바로 위 원문 영어 주석대로 fmt 3비트와 type 5비트를 8비트 필드
 *     하나로 합친다. PCIe 스펙의 TLP 첫 바이트 배치 그대로다.
 *   CFG0_READ_FMTTYPE 등 네 상수는 그것을 FIELD_PREP 으로 DWORD 의
 *     최상위 바이트(ASPEED_TLP_COMMON_FIELDS = GENMASK(31,24)) 자리에
 *     올린 값이다. altera 가 0x04/0x44 같은 숫자를 직접 적어 둔 것과
 *     달리, 이쪽은 커널 공통 enum(PCIE_TLP_FMT_ 계열과 PCIE_TLP_TYPE_
 *     계열)에서 조립하므로 값이 코드에 드러나지 않는다.
 *   TLP_HEADER_BYTE_EN(size, where) = GENMASK(size-1, 0) << (where % 4)
 *     접근 크기만큼의 연속 비트를 DWORD 안의 바이트 위치로 민다.
 *     1바이트면 비트 하나, 2바이트면 둘, 4바이트면 넷이 선다.
 *   TLP_SET_VALUE(x, size, where) 와 TLP_GET_VALUE(x, size, where)
 *     쓸 값을 DWORD 안의 제자리로 밀어 넣고, 읽은 DWORD 에서 그 자리를
 *     잘라 낸다. altera 가 손으로 시프트·마스크하던 것을 매크로로 묶었다.
 *   CPL_STS(x) = FIELD_GET(GENMASK(15,13), x)
 *     완료 TLP 의 Completion Status 필드. altera 의 TLP_COMP_STATUS 와
 *     같은 비트 자리다.
 *
 * === 하드웨어 레지스터 값의 근거 ===
 * 이 파일의 레지스터 오프셋과 비트 상수(ASPEED_H2X_ 계열,
 * ASPEED_PCIE_ 계열, ASPEED_CFGI_ 계열, ASPEED_CFGE_ 계열,
 * ASPEED_SCU_ 계열, 그리고 세대별 platform 구조체의 reg_intx_en 이나
 * reg_msi_en 같은 값)는 모두 이 파일 안에 정의되어 있으나, 그 근거가
 * 되는 Aspeed 하드웨어 문서는 이 트리에 없다.
 * 따라서 아래 주석들은 값의 의미를 단정하지 않고 **코드가 그 상수를
 * 어떻게 쓰는지**(어느 레지스터의 마스크인지, 폴링 조건인지, 트리거
 * 비트인지, 매직 키인지)로 설명한다.
 * PCIE_TLP_FMT_ 계열과 PCIE_TLP_TYPE_ 계열, PCIE_CPL_STS_SUCCESS 는
 * 커널 공통 헤더에 있어야 하는데 이 스파스 체크아웃에 그 헤더가 없어
 * 값을 확인하지 못했다 — 이름이 뜻하는 바와 코드의 사용 방식으로만
 * 설명한다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일과 아무 접점이 없다(drivers/nvme 전수 grep 0건).
 * 이 파일은 특정 BMC SoC 의 호스트 컨트롤러 드라이버라, 장치 종류와
 * 무관하게 "PCI 버스를 제공" 하는 쪽이기 때문이다.
 *
 * 다만 실무에서 BMC 는 NVMe 와 자주 만난다. 서버의 NVMe 백플레인 상태를
 * BMC 가 읽어 관리 인터페이스로 보고하는 구성이 흔하고, 그때 BMC 쪽
 * PCIe 버스에 붙은 장치를 열거하는 것이 이 드라이버다. 다만 그 연결은
 * 배치의 문제이지 코드의 의존 관계가 아니므로, 이 파일 안에서 근거를
 * 댈 수 있는 부분은 없다.
 */

/* [한국어] FIELD_PREP/FIELD_GET 과 GENMASK — 이 파일의 거의 모든 레지스터 필드
 * 조작이 이 셋으로 이뤄진다. 시프트 값을 손으로 적지 않으므로 마스크
 * 상수 하나가 위치와 폭을 모두 담는다 */
#include <linux/bitfield.h>
/* [한국어] clk_prepare_enable — 포트마다 클록 게이트를 켠다 */
#include <linux/clk.h>
/* [한국어] irqreturn_t, IRQF_SHARED, devm_request_irq — 인터럽트 등록과 핸들러 */
#include <linux/interrupt.h>
/* [한국어] struct irq_chip, struct irq_data, irq_set_chip_and_handler,
 * handle_level_irq, handle_simple_irq, irq_set_status_flags */
#include <linux/irq.h>
/* [한국어] irq_domain 과 irq_domain_add_linear, irq_domain_set_info,
 * generic_handle_domain_irq, irq_domain_remove */
#include <linux/irqdomain.h>
/* [한국어] chained_irq 헬퍼. 다만 이 파일은 체인 핸들러가 아니라
 * devm_request_irq 로 보통의 핸들러를 등록하므로 직접 쓰는 심볼은
 * 확인되지 않았다. 상류 그대로 둔다 */
#include <linux/irqchip/chained_irq.h>
/* [한국어] msi_lib_init_dev_msi_info 와 MSI 부모 도메인 헬퍼. 장치별 MSI 도메인을
 * 이 컨트롤러의 부모 도메인 아래에 계층으로 붙여 주는 계층이다 */
#include <linux/irqchip/irq-msi-lib.h>
/* [한국어] ARRAY_SIZE 등 커널 공통 매크로 */
#include <linux/kernel.h>
/* [한국어] syscon_regmap_lookup_by_phandle — 이 컨트롤러 밖의 블록(AST2600 의
 * AHBC, AST2700 의 SCU)을 장치 트리 phandle 로 찾아 regmap 으로 다룬다 */
#include <linux/mfd/syscon.h>
/* [한국어] MODULE_AUTHOR/DESCRIPTION/LICENSE. 다만 등록은
 * builtin_platform_driver 로 하므로 모듈로 빠지지는 않는다 */
#include <linux/module.h>
/* [한국어] struct msi_msg, struct msi_parent_ops, MSI_FLAG_* — MSI 지원 선언 */
#include <linux/msi.h>
/* [한국어] struct mutex 와 devm_mutex_init, guard(mutex) — MSI 비트맵 보호 */
#include <linux/mutex.h>
/* [한국어] of_device_get_match_data, of_property_read_string,
 * for_each_available_child_of_node_scoped — 장치 트리 순회 */
#include <linux/of.h>
/* [한국어] 이 파일에서 직접 쓰는 심볼은 확인되지 않았다. 상류 그대로 둔다 */
#include <linux/of_address.h>
/* [한국어] of_pci_get_devfn — 포트 노드의 reg 속성에서 devfn 을 뽑는다 */
#include <linux/of_pci.h>
/* [한국어] struct pci_bus, struct pci_ops, struct pci_host_bridge,
 * PCIBIOS_* 반환 코드, PCI_SLOT/PCI_FUNC, PCI_NUM_INTX,
 * PCI_SET_ERROR_RESPONSE 등 PCI 코어 API */
#include <linux/pci.h>
/* [한국어] struct platform_device, platform_get_irq, platform_set_drvdata,
 * devm_platform_ioremap_resource */
#include <linux/platform_device.h>
/* [한국어] PHY_MODE_PCIE_RC — PHY 를 Root Complex 모드로 세울 때 쓰는 상수.
 * 같은 PHY 가 EP 모드로도 쓰일 수 있어 어느 쪽인지 알려 줘야 한다 */
#include <linux/phy/pcie.h>
/* [한국어] struct phy 와 phy_init, phy_set_mode_ext, devm_of_phy_get */
#include <linux/phy/phy.h>
/* [한국어] struct regmap 과 regmap_write, regmap_update_bits — syscon 으로 얻은
 * 외부 블록을 조작한다 */
#include <linux/regmap.h>
/* [한국어] struct reset_control 과 assert/deassert/put, 그리고 두 획득 함수
 * (devm_reset_control_get_exclusive 와 of_reset_control_get_exclusive) */
#include <linux/reset.h>

/* [한국어] PCI 코어 내부 헤더. devm_pci_alloc_host_bridge(), pci_host_bridge_priv(),
 * pci_host_probe(), PCIE_RESET_CONFIG_WAIT_MS 가 여기서 온다 */
#include "../pci.h"

/* [한국어] 이 컨트롤러가 가진 MSI 벡터 수. 32비트 상태 레지스터 두 개가 이만큼을
 * 덮으며, msi_irq_in_use 비트맵의 크기이기도 하다 */
#define MAX_MSI_HOST_IRQS	64
/* [한국어] H2X 컨트롤러 리셋을 유지할 시간(밀리초). aspeed_host_reset() 이
 * mdelay 로 이만큼 기다린다. 하드웨어가 리셋 신호를 인식하는 데 필요한
 * 최소 시간으로 보이나, 근거가 되는 Aspeed 문서는 이 트리에 없다 */
#define ASPEED_RESET_RC_WAIT_MS	10

/* AST2600 AHBC Registers */
/* [한국어] AHBC 의 잠금 키 레지스터. 아래 두 상수를 여기에 쓴다 */
#define ASPEED_AHBC_KEY			0x00
/* [한국어] AHBC 잠금을 푸는 매직 키. 실수로 이 블록을 건드리는 것을 막는 흔한
 * 설계다. 값의 근거가 되는 문서는 이 트리에 없다 */
#define  ASPEED_AHBC_UNLOCK_KEY			0xaeed1a03
/* [한국어] 잠금 키 레지스터에 쓰는 두 번째 값. aspeed_ast2600_setup() 이 설정을
 * 마친 뒤 이것을 써서 닫는다. "잠그기" 를 뜻하는지 다른 뜻인지는
 * 상수 이름만으로는 분명하지 않다 */
#define  ASPEED_AHBC_UNLOCK			0x01
/* [한국어] AHBC 의 주소 매핑 레지스터 */
#define ASPEED_AHBC_ADDR_MAPPING	0x8c
/* [한국어] 그 레지스터에서 RC 메모리 접근을 켜는 비트. regmap_update_bits 로
 * 이 비트만 세운다 */
#define  ASPEED_PCIE_RC_MEMORY_EN		BIT(5)

/* AST2600 H2X Controller Registers */
/* [한국어] AST2600 H2X 의 인터럽트 상태 레지스터 */
#define ASPEED_H2X_INT_STS		0x08
/* [한국어] 그 레지스터에서 TX idle 상태를 지우는 비트.
 * aspeed_ast2600_conf() 가 전송 완료 후 세운다 */
#define  ASPEED_PCIE_TX_IDLE_CLEAR		BIT(0)
/* [한국어] 같은 레지스터에서 INTx 넷을 나타내는 필드. GENMASK(3,0) 이므로
 * 하위 네 비트이고, aspeed_pcie_intr_handler() 가 FIELD_GET 으로 뽑는다 */
#define  ASPEED_PCIE_INTX_STS			GENMASK(3, 0)
/* [한국어] 호스트 쪽 RX 디스크립터 데이터. aspeed_ast2600_conf() 의 default
 * 갈래가 여기서 값을 읽는다 */
#define ASPEED_H2X_HOST_RX_DESC_DATA	0x0c
/* [한국어] TX 디스크립터 0 — TLP 첫 DWORD(fmt/type 과 길이)가 들어간다 */
#define ASPEED_H2X_TX_DESC0		0x10
/* [한국어] TX 디스크립터 1 — 세대 고정값과 태그와 byte enable */
#define ASPEED_H2X_TX_DESC1		0x14
/* [한국어] TX 디스크립터 2 — BDF 와 레지스터 오프셋 */
#define ASPEED_H2X_TX_DESC2		0x18
/* [한국어] TX 디스크립터 3 — 이 드라이버는 0 을 쓴다 */
#define ASPEED_H2X_TX_DESC3		0x1c
/* [한국어] TX 데이터 레지스터 — 쓰기 요청의 페이로드 */
#define ASPEED_H2X_TX_DESC_DATA		0x20
/* [한국어] H2X 상태 레지스터. 전송 트리거와 완료 판정이 모두 여기서 이뤄진다 */
#define ASPEED_H2X_STS			0x24
/* [한국어] 그 레지스터에서 TX 가 한가한지 나타내는 비트.
 * aspeed_ast2600_conf() 가 이것이 설 때까지 폴링한다 */
#define  ASPEED_PCIE_TX_IDLE			BIT(31)
/* [한국어] 전송 결과를 담는 필드. GENMASK(25,24) 이므로 두 비트다.
 * aspeed_ast2600_conf() 가 switch 로 이 값을 가른다 */
#define  ASPEED_PCIE_STATUS_OF_TX		GENMASK(25, 24)
/* [한국어] 그 필드에서 "정상 완료" 를 뜻하는 값(비트 25만 선 상태).
 * 필드 전체가 선 상태(= ASPEED_PCIE_STATUS_OF_TX)는 실패로 처리한다.
 * 이 줄만 들여쓰기 공백이 아니라 탭인데, 동작에는 영향이 없다 */
#define	ASPEED_PCIE_RC_H_TX_COMPLETE		BIT(25)
/* [한국어] 전송을 시작시키는 트리거 비트. 읽고-OR-쓰기로 세운다 */
#define  ASPEED_PCIE_TRIGGER_TX			BIT(0)
/* [한국어] AHB 주소 변환 설정 레지스터 0 — 하위 주소와 하위 마스크가 함께 들어간다 */
#define ASPEED_H2X_AHB_ADDR_CONFIG0	0x60
/* [한국어] 그 레지스터에서 하위 remap 주소가 놓이는 자리(GENMASK(15,4)).
 * 호출자가 값을 미리 16비트 오른쪽으로 밀어 넘기므로, 결과적으로
 * 원래 주소의 비트 31-20 이 여기 놓인다 */
#define  ASPEED_AHB_REMAP_LO_ADDR(x)		(x & GENMASK(15, 4))
/* [한국어] 같은 레지스터의 하위 마스크 자리(GENMASK(31,20)).
 * FIELD_PREP 이라 값을 그 위치로 밀어 넣는다 */
#define  ASPEED_AHB_MASK_LO_ADDR(x)		FIELD_PREP(GENMASK(31, 20), x)
/* [한국어] AHB 주소 변환 설정 레지스터 1 — 상위 remap 주소 */
#define ASPEED_H2X_AHB_ADDR_CONFIG1	0x64
/* [한국어] 그 레지스터에는 값을 그대로 쓴다. 위 두 매크로와 형태가 다른 것은
 * 각 레지스터의 필드 배치가 다르기 때문으로 보이며, 근거가 되는
 * 문서는 이 트리에 없다 */
#define  ASPEED_AHB_REMAP_HI_ADDR(x)		(x)
/* [한국어] AHB 주소 변환 설정 레지스터 2 — 상위 마스크 */
#define ASPEED_H2X_AHB_ADDR_CONFIG2	0x68
/* [한국어] 역시 값을 그대로 쓴다. aspeed_ast2600_pcie_map_ranges() 가 ~0 을 넣는다 */
#define  ASPEED_AHB_MASK_HI_ADDR(x)		(x)
/* [한국어] 장치 제어 레지스터. 아래 여덟 비트가 여기 모여 있다 */
#define ASPEED_H2X_DEV_CTRL		0xc0
/* [한국어] RX DMA 활성화 */
#define  ASPEED_PCIE_RX_DMA_EN			BIT(9)
/* [한국어] RX 선형 모드 */
#define  ASPEED_PCIE_RX_LINEAR			BIT(8)
/* [한국어] RX MSI 선택 */
#define  ASPEED_PCIE_RX_MSI_SEL			BIT(7)
/* [한국어] RX MSI 활성화. 위 둘과 함께 EP 가 보낸 MSI 를 받도록 설정한다 */
#define  ASPEED_PCIE_RX_MSI_EN			BIT(6)
/* [한국어] RX 버퍼 잠금 해제. aspeed_ast2600_conf() 가 config 접근의 앞과 뒤에서
 * 두 번 세운다 — 앞선 접근이 남긴 상태를 치우고, 다음 접근을 위해 푼다 */
#define  ASPEED_PCIE_UNLOCK_RX_BUFF		BIT(4)
/* [한국어] RX TLP 클리어 대기 */
#define  ASPEED_PCIE_WAIT_RX_TLP_CLR		BIT(2)
/* [한국어] RC RX 활성화 */
#define  ASPEED_PCIE_RC_RX_ENABLE		BIT(1)
/* [한국어] RC 활성화. aspeed_ast2600_setup() 이 위 비트들과 함께 한 번에 세운다 */
#define  ASPEED_PCIE_RC_ENABLE			BIT(0)
/* [한국어] 장치 상태 레지스터 */
#define ASPEED_H2X_DEV_STS		0xc8
/* [한국어] 그 레지스터에서 RX 완료를 나타내는 비트.
 * aspeed_ast2600_conf() 가 응답을 기다릴 때 폴링한다 */
#define  ASPEED_PCIE_RC_RX_DONE_ISR		BIT(4)
/* [한국어] 장치 쪽 RX 디스크립터 데이터 — 완료 TLP 의 페이로드 */
#define ASPEED_H2X_DEV_RX_DESC_DATA	0xcc
/* [한국어] 장치 쪽 RX 디스크립터 1 — Completion Status 가 여기 실려 온다.
 * CPL_STS 매크로가 이 값에서 그 필드를 뽑는다 */
#define ASPEED_H2X_DEV_RX_DESC1		0xd4
/* [한국어] TX 태그 개수 설정 레지스터 */
#define ASPEED_H2X_DEV_TX_TAG		0xfc
/* [한국어] 거기 쓰는 값. 의미는 Aspeed 문서 소관이라 이 트리에서 확인하지 못했다 */
#define  ASPEED_RC_TLP_TX_TAG_NUM		0x28

/* AST2700 H2X */
/* [한국어] AST2700 H2X 의 제어 레지스터. AST2600 의 같은 오프셋(0x00)이 AHBC 의
 * 키 레지스터인 것과 대비되는데, 두 세대가 서로 다른 레지스터 창을
 * 쓰기 때문이다 */
#define ASPEED_H2X_CTRL			0x00
/* [한국어] H2X 브리지 활성화 비트. 두 세대가 모두 쓴다 */
#define  ASPEED_H2X_BRIDGE_EN			BIT(0)
/* [한국어] direct 모드 활성화. AST2700 만 쓴다 */
#define  ASPEED_H2X_BRIDGE_DIRECT_EN		BIT(1)
/* [한국어] CFGE(외부 config) 인터럽트 상태 레지스터 */
#define ASPEED_H2X_CFGE_INT_STS		0x08
/* [한국어] 그 레지스터의 TX idle 비트. aspeed_ast2700_child_config() 의 1단계
 * 폴링 조건이다 */
#define  ASPEED_CFGE_TX_IDLE			BIT(0)
/* [한국어] 같은 레지스터의 RX busy 비트. 2단계 폴링 조건이며, 이름이 "busy" 인
 * 비트가 서기를 기다리는 형태다. 그 실제 의미는 Aspeed 문서 소관이라
 * 이 트리에서 확인하지 못했다 */
#define  ASPEED_CFGE_RX_BUSY			BIT(1)
/* [한국어] CFGI(내부 config) TLP 레지스터. Root Port 자신의 config 접근에 쓴다 */
#define ASPEED_H2X_CFGI_TLP		0x20
/* [한국어] 그 레지스터에서 byte enable 이 놓이는 자리(GENMASK(19,16)) */
#define  ASPEED_CFGI_BYTE_EN_MASK		GENMASK(19, 16)
/* [한국어] byte enable 값을 그 자리로 밀어 넣는 매크로 */
#define  ASPEED_CFGI_BYTE_EN(x) \
			FIELD_PREP(ASPEED_CFGI_BYTE_EN_MASK, (x))
/* [한국어] CFGI 쓰기 데이터 레지스터 */
#define ASPEED_H2X_CFGI_WR_DATA		0x24
/* [한국어] CFGI TLP 레지스터에서 "쓰기" 를 나타내는 비트. 오프셋과 byte enable 과
 * 같은 레지스터에 실린다 */
#define  ASPEED_CFGI_WRITE			BIT(20)
/* [한국어] CFGI 제어 레지스터 */
#define ASPEED_H2X_CFGI_CTRL		0x28
/* [한국어] 거기에 쓰면 CFGI 접근이 실행되는 트리거 비트 */
#define  ASPEED_CFGI_TLP_FIRE			BIT(0)
/* [한국어] CFGI 반환 데이터 레지스터. 읽기 결과가 여기 나온다 */
#define ASPEED_H2X_CFGI_RET_DATA	0x2c
/* [한국어] CFGE TLP 의 첫 DWORD 를 쓰는 레지스터 */
#define ASPEED_H2X_CFGE_TLP_1ST		0x30
/* [한국어] CFGE TLP 의 이후 DWORD 들을 쓰는 레지스터. 같은 주소에 연달아 쓰면
 * 하드웨어가 내부 포인터를 자동으로 전진시키는 FIFO 형태로 보인다 */
#define ASPEED_H2X_CFGE_TLP_NEXT	0x34
/* [한국어] CFGE 제어 레지스터 */
#define ASPEED_H2X_CFGE_CTRL		0x38
/* [한국어] 거기에 쓰면 CFGE 전송이 시작되는 트리거 비트 */
#define  ASPEED_CFGE_TLP_FIRE			BIT(0)
/* [한국어] CFGE 반환 데이터 레지스터 */
#define ASPEED_H2X_CFGE_RET_DATA	0x3c
/* [한국어] prefetchable 영역의 상위 주소 레지스터 */
#define ASPEED_H2X_REMAP_PREF_ADDR	0x70
/* [한국어] 거기에 값을 그대로 쓰는 매크로. aspeed_ast2700_setup() 이 0x3 을 넣어
 * 64비트 BAR 를 준비한다(그 자리의 원문 영어 주석 참조) */
#define  ASPEED_REMAP_PREF_ADDR_63_32(x)	(x)
/* [한국어] PCI 주소 변환의 상위 레지스터 */
#define ASPEED_H2X_REMAP_PCI_ADDR_HI	0x74
/* [한국어] 64비트 주소에서 상위 절반을 꺼내는 매크로 */
#define  ASPEED_REMAP_PCI_ADDR_63_32(x)		(((x) >> 32) & GENMASK(31, 0))
/* [한국어] PCI 주소 변환의 하위 레지스터 */
#define ASPEED_H2X_REMAP_PCI_ADDR_LO	0x78
/* [한국어] 하위 주소에서 비트 31-12 만 남기는 매크로. 4KB 단위 정렬을 전제한다 —
 * AST2600 이 1MB 단위인 것과 다르다 */
#define  ASPEED_REMAP_PCI_ADDR_31_12(x)		((x) & GENMASK(31, 12))

/* AST2700 SCU */
/* [한국어] AST2700 SCU 의 경로 제어 레지스터 */
#define ASPEED_SCU_60			0x60
/* [한국어] EP to Memory 경로 활성화 */
#define  ASPEED_RC_E2M_PATH_EN			BIT(0)
/* [한국어] Host to PCIe 의 S 갈래 */
#define  ASPEED_RC_H2XS_PATH_EN			BIT(16)
/* [한국어] Host to PCIe 의 D 갈래 */
#define  ASPEED_RC_H2XD_PATH_EN			BIT(17)
/* [한국어] Host to PCIe 의 X 갈래. 세 갈래의 정확한 구분은 Aspeed 문서 소관이라
 * 이 트리에서 확인하지 못했다 */
#define  ASPEED_RC_H2XX_PATH_EN			BIT(18)
/* [한국어] 상류 메모리 접근 활성화. aspeed_ast2700_setup() 이 위 넷과 함께
 * 한 번에 켠다 */
#define  ASPEED_RC_UPSTREAM_MEM_EN		BIT(19)
/* [한국어] SCU 의 DMA 디코딩 범위 레지스터. 이 SoC 에 RC 가 둘이라
 * 네 필드가 한 레지스터에 들어간다 */
#define ASPEED_SCU_64			0x64
/* [한국어] RC0 의 디코딩 하한(비트 7-0) */
#define  ASPEED_RC0_DECODE_DMA_BASE(x)		FIELD_PREP(GENMASK(7, 0), x)
/* [한국어] RC0 의 디코딩 상한(비트 15-8) */
#define  ASPEED_RC0_DECODE_DMA_LIMIT(x)		FIELD_PREP(GENMASK(15, 8), x)
/* [한국어] RC1 의 디코딩 하한(비트 23-16) */
#define  ASPEED_RC1_DECODE_DMA_BASE(x)		FIELD_PREP(GENMASK(23, 16), x)
/* [한국어] RC1 의 디코딩 상한(비트 31-24). setup 이 하한 0, 상한 0xff 로
 * 전 범위를 연다 */
#define  ASPEED_RC1_DECODE_DMA_LIMIT(x)		FIELD_PREP(GENMASK(31, 24), x)
/* [한국어] SCU 의 EP 기능 제어 레지스터 */
#define ASPEED_SCU_70			0x70
/* [한국어] 거기 쓰는 값. 이 SoC 는 RC 로도 EP 로도 동작할 수 있는데,
 * 이 드라이버는 RC 로 쓰므로 EP 기능을 끈다 */
#define  ASPEED_DISABLE_EP_FUNC			0

/* Macro to combine Fmt and Type into the 8-bit field */
/* [한국어] 바로 위 원문 영어 주석대로 fmt 3비트와 type 5비트를 8비트 필드 하나로
 * 합친다. PCIe 스펙의 TLP 첫 바이트 배치 그대로다 */
#define ASPEED_TLP_FMT_TYPE(fmt, type)	((((fmt) & 0x7) << 5) | ((type) & 0x1f))
/* [한국어] 그 8비트가 DWORD 안에서 놓이는 자리(GENMASK(31,24)) — 최상위 바이트다 */
#define ASPEED_TLP_COMMON_FIELDS	GENMASK(31, 24)

/* Completion status */
/* [한국어] 완료 TLP 에서 Completion Status 필드를 뽑는다.
 * pcie-altera.c 의 TLP_COMP_STATUS 와 같은 비트 자리(15-13)다 */
#define CPL_STS(x)	FIELD_GET(GENMASK(15, 13), (x))
/* TLP configuration type 0 and type 1 */
/* [한국어] Type 0 읽기의 fmt/type 을 DWORD 자리에 올린 값.
 * altera 가 0x04 같은 숫자를 직접 적어 둔 것과 달리 커널 공통 enum 에서
 * 조립하므로 값이 코드에 드러나지 않는다. 그 enum 의 정의는 이 스파스
 * 체크아웃에 없어 실제 값을 확인하지 못했다 */
#define CFG0_READ_FMTTYPE                                        \
	FIELD_PREP(ASPEED_TLP_COMMON_FIELDS,                     \
		   ASPEED_TLP_FMT_TYPE(PCIE_TLP_FMT_3DW_NO_DATA, \
				       PCIE_TLP_TYPE_CFG0_RD))
/* [한국어] Type 0 쓰기의 fmt/type. 읽기와 달리 3DW_DATA — 데이터가 따라온다는 뜻이다 */
#define CFG0_WRITE_FMTTYPE                                    \
	FIELD_PREP(ASPEED_TLP_COMMON_FIELDS,                  \
		   ASPEED_TLP_FMT_TYPE(PCIE_TLP_FMT_3DW_DATA, \
				       PCIE_TLP_TYPE_CFG0_WR))
/* [한국어] Type 1 읽기의 fmt/type. Type 1 은 브리지를 더 지나야 하는 접근이다 */
#define CFG1_READ_FMTTYPE                                        \
	FIELD_PREP(ASPEED_TLP_COMMON_FIELDS,                     \
		   ASPEED_TLP_FMT_TYPE(PCIE_TLP_FMT_3DW_NO_DATA, \
				       PCIE_TLP_TYPE_CFG1_RD))
/* [한국어] Type 1 쓰기의 fmt/type */
#define CFG1_WRITE_FMTTYPE                                    \
	FIELD_PREP(ASPEED_TLP_COMMON_FIELDS,                  \
		   ASPEED_TLP_FMT_TYPE(PCIE_TLP_FMT_3DW_DATA, \
				       PCIE_TLP_TYPE_CFG1_WR))
/* [한국어] TLP 의 Length 필드에 넣을 값. config 접근은 언제나 한 DWORD 이므로
 * 1 로 고정이다(원문 주석이 그것을 밝힌다) */
#define CFG_PAYLOAD_SIZE		0x01 /* 1 DWORD */
/* [한국어] 접근 크기만큼의 연속 비트를 DWORD 안의 바이트 위치로 미는 매크로.
 * 1바이트면 비트 하나, 2바이트면 둘, 4바이트면 넷이 선다.
 * GENMASK(x-1, 0) 이 크기를, << (y % 4) 가 위치를 담당한다 */
#define TLP_HEADER_BYTE_EN(x, y)	((GENMASK((x) - 1, 0) << ((y) % 4)))
/* [한국어] 읽은 DWORD 에서 요청한 크기와 위치의 값을 잘라 내는 매크로.
 * 오른쪽으로 밀어 자리를 맞춘 뒤 크기만큼 마스킹한다 */
#define TLP_GET_VALUE(x, y, z)	\
	(((x) >> ((((z) % 4)) * 8)) & GENMASK((8 * (y)) - 1, 0))
/* [한국어] 쓸 값을 DWORD 안의 제자리로 밀어 넣는 매크로. 위 GET 의 역방향이다 */
#define TLP_SET_VALUE(x, y, z)	\
	((((x) & GENMASK((8 * (y)) - 1, 0)) << ((((z) % 4)) * 8)))
/* [한국어] AST2600 의 TX 디스크립터 1 에 항상 들어가는 고정 비트들.
 * 태그와 byte enable 이 여기에 OR 된다. 그 값의 의미는 Aspeed 문서
 * 소관이라 이 트리에서 확인하지 못했다 */
#define AST2600_TX_DESC1_VALUE		0x00002000
/* [한국어] AST2700 의 같은 자리 고정값. 두 세대가 서로 다른 값을 쓴다 */
#define AST2700_TX_DESC1_VALUE		0x00401000

/**
 * struct aspeed_pcie_port - PCIe port information
 * @list: port list
 * @pcie: pointer to PCIe host info
 * @clk: pointer to the port clock gate
 * @phy: pointer to PCIe PHY
 * @perst: pointer to port reset control
 * @slot: port slot
 */
struct aspeed_pcie_port {
	/* [한국어] 이 포트를 컨트롤러의 ports 리스트에 매다는 고리.
	 * 설정자: aspeed_pcie_parse_port() 의 INIT_LIST_HEAD 와 list_add_tail.
	 * 읽는 자: aspeed_pcie_parse_dt() 가 list_empty() 로 포트가 하나라도
	 *   있는지 확인할 때. 그 밖에 리스트를 순회하는 코드는 이 파일에 없다.
	 * 동기화: probe 경로에서만 조작되므로 락이 없다 */
	struct list_head list;
	/* [한국어] 이 포트가 속한 컨트롤러로 되돌아가는 포인터.
	 * 설정자: aspeed_pcie_parse_port().
	 * 읽는 자: aspeed_pcie_port_init() 이 dev 를 얻어 로그를 찍을 때.
	 * 동기화: 설정 뒤 읽기 전용 */
	struct aspeed_pcie *pcie;
	/* [한국어] 이 포트의 클록 게이트.
	 * 설정자: aspeed_pcie_parse_port() 의 devm_get_clk_from_child().
	 * 읽는 자: aspeed_pcie_port_init() 의 clk_prepare_enable().
	 * 값 범위: 획득 실패 시 오류 포인터이며 그 경우 probe 가 중단된다.
	 * 동기화: 설정 뒤 읽기 전용. 해제는 devm 이 맡는다 */
	struct clk *clk;
	/* [한국어] 이 포트의 PCIe PHY.
	 * 설정자: aspeed_pcie_parse_port() 의 devm_of_phy_get().
	 * 읽는 자: aspeed_pcie_port_init() 이 phy_init() 과
	 *   phy_set_mode_ext(PHY_MODE_PCIE_RC) 로 RC 모드로 세운다.
	 * 동기화: 설정 뒤 읽기 전용 */
	struct phy *phy;
	/* [한국어] 이 포트의 PERST 리셋 컨트롤. 상대 장치를 리셋 상태로 붙들거나 푼다.
	 * 설정자: aspeed_pcie_parse_port() 의 of_reset_control_get_exclusive().
	 * 읽는 자: 같은 함수가 곧바로 assert 하고, aspeed_pcie_port_init() 이
	 *   deassert 해 링크 훈련을 시작시킨다.
	 * 값 범위: devm 판이 아니라서 aspeed_pcie_reset_release() 를 devm 액션으로
	 *   등록해 되돌린다 */
	struct reset_control *perst;
	/* [한국어] 이 포트의 슬롯 번호.
	 * 설정자: aspeed_pcie_parse_port() 가 장치 트리에서 얻은 값.
	 * 읽는 자: aspeed_pcie_port_init() 의 오류 메시지에만 쓰인다.
	 * 동기화: 설정 뒤 읽기 전용 */
	u32 slot;
};

/**
 * struct aspeed_pcie - PCIe RC information
 * @host: pointer to PCIe host bridge
 * @dev: pointer to device structure
 * @reg: PCIe host register base address
 * @ahbc: pointer to AHHC register map
 * @cfg: pointer to Aspeed PCIe configuration register map
 * @platform: platform specific information
 * @ports: list of PCIe ports
 * @tx_tag: current TX tag for the port
 * @root_bus_nr: bus number of the host bridge
 * @h2xrst: pointer to H2X reset control
 * @intx_domain: IRQ domain for INTx interrupts
 * @msi_domain: IRQ domain for MSI interrupts
 * @lock: mutex to protect MSI bitmap variable
 * @msi_irq_in_use: bitmap to track used MSI host IRQs
 * @clear_msi_twice: AST2700 workaround to clear MSI status twice
 */
struct aspeed_pcie {
	/* [한국어] PCI 코어에 넘길 호스트 브리지.
	 * 설정자: aspeed_pcie_probe().
	 * 읽는 자: 세대별 setup() 이 ops 와 child_ops 를 걸 때,
	 *   aspeed_pcie_map_ranges() 가 windows 리스트를 훑을 때.
	 * 값 범위: 이 구조체 자신이 그 브리지의 private 영역에 얹혀 있다 —
	 *   pci_host_bridge_priv() 로 얻은 포인터가 곧 이 구조체다 */
	struct pci_host_bridge *host;
	/* [한국어] 이 컨트롤러의 struct device. 로그 출력과 devm 할당의 기준이다.
	 * 설정자: aspeed_pcie_probe().
	 * 읽는 자: 이 파일의 거의 모든 함수가 dev_err_probe 나 devm 계열에 쓴다 */
	struct device *dev;
	/* [한국어] "H2X" 컨트롤러 레지스터 창의 커널 가상 주소.
	 * 설정자: aspeed_pcie_probe() 의 devm_platform_ioremap_resource().
	 * 읽는 자: 이 파일의 모든 readl/writel. config 접근, INTx/MSI 레지스터,
	 *   주소 변환 레지스터가 전부 이 창 안에 있다.
	 * 동기화: probe 뒤 읽기 전용. 해제는 devm 이 맡는다 */
	void __iomem *reg;
	/* [한국어] AST2600 의 AHBC(AHB Controller) 레지스터 맵.
	 * 설정자: aspeed_ast2600_setup() 의 syscon_regmap_lookup_by_phandle().
	 * 읽는 자: 같은 함수가 매직 키로 잠금을 풀고 RC 메모리 접근을 켤 때.
	 * 값 범위: AST2700 경로에서는 채워지지 않는다.
	 * 왜 regmap 인가: AHBC 는 이 컨트롤러 밖의 블록이라 자기 레지스터 창으로
	 *   접근할 수 없고, 여러 드라이버가 공유하므로 syscon 을 거친다 */
	struct regmap *ahbc;
	/* [한국어] AST2700 의 PCIECFG(SCU) 레지스터 맵.
	 * 설정자: aspeed_ast2700_setup() 의 syscon_regmap_lookup_by_phandle().
	 * 읽는 자: 같은 함수가 경로 활성화, DMA 디코딩 범위, EP 기능 끄기를 할 때.
	 * 값 범위: AST2600 경로에서는 채워지지 않는다.
	 * 위 ahbc 와 같은 이유로 regmap 을 쓴다 */
	struct regmap *cfg;
	/* [한국어] of_device_get_match_data() 가 고른 세대별 콜백과 상수 묶음.
	 * 설정자: aspeed_pcie_probe().
	 * 읽는 자: 이 파일 전체 — setup/map_ranges 콜백, INTx/MSI 레지스터
	 *   오프셋, MSI 주소가 전부 여기서 나온다.
	 * 값 범위: pcie_rc_ast2600 또는 pcie_rc_ast2700 을 가리키며, 매칭에
	 *   실패하면 probe 가 -ENODEV 로 끝난다 */
	const struct aspeed_pcie_rc_platform *platform;
	/* [한국어] 이 컨트롤러에 딸린 포트들의 리스트 머리.
	 * 설정자: aspeed_pcie_probe() 의 INIT_LIST_HEAD,
	 *   aspeed_pcie_parse_port() 의 list_add_tail.
	 * 읽는 자: aspeed_pcie_parse_dt() 의 list_empty() 검사.
	 * 동기화: probe 경로에서만 조작된다 */
	struct list_head ports;

	/* [한국어] 다음 config 요청에 실을 TLP 태그.
	 * 설정자: aspeed_pcie_probe() 가 0 으로 두고, 두 config 본체가 요청마다
	 *   하나씩 증가시킨다(AST2600 은 % 0x8, AST2700 은 % 0xf 로 순환).
	 * 읽는 자: 두 config 본체가 FIELD_PREP(GENMASK(11,8), ...) 로 디스크립터에
	 *   넣는다.
	 * 왜 필요한가: 연속된 요청을 하드웨어가 구분할 수 있게 하려는 것으로
	 *   보이나, 근거가 되는 Aspeed 문서는 이 트리에 없다.
	 * 동기화: config 접근이 PCI 코어의 스핀락으로 직렬화되므로 별도 락이 없다 */
	u8 tx_tag;
	/* [한국어] 이 RC 의 루트 버스 번호.
	 * 설정자: aspeed_pcie_probe() 가 장치 트리의 bus-range 에서 한 번 얻는다.
	 *   pcie-altera.c 가 config 쓰기를 엿보아 갱신하는 것과 다르다.
	 * 읽는 자: aspeed_ast2700_child_config() 가 (root_bus_nr + 1) 과 비교해
	 *   Type 0/Type 1 을 가른다. AST2600 경로는 이 값을 쓰지 않는다
	 *   (그쪽은 ops/child_ops 로 이미 갈라져 있다).
	 * 값 범위: bus-range 항목이 없으면 0 이 그대로 남는다 */
	u8 root_bus_nr;

	/* [한국어] H2X 컨트롤러의 리셋 컨트롤.
	 * 설정자: aspeed_pcie_probe() 의 devm_reset_control_get_exclusive().
	 * 읽는 자: aspeed_host_reset() 이 걸었다 푼다.
	 * 동기화: probe 경로에서만 쓰인다 */
	struct reset_control *h2xrst;

	/* [한국어] 하위 장치의 INTx 를 커널 IRQ 로 옮기는 도메인.
	 * 설정자: aspeed_pcie_init_irq_domain().
	 * 읽는 자: aspeed_pcie_intr_handler() 의 generic_handle_domain_irq(),
	 *   aspeed_pcie_irq_domain_free() 의 제거.
	 * 값 범위: 해제 후 NULL 로 되돌려 두 번 해제되지 않게 한다 */
	struct irq_domain *intx_domain;
	/* [한국어] MSI 부모 도메인. 그 아래에 장치별 MSI 도메인이 계층으로 붙는다.
	 * 설정자: aspeed_pcie_msi_init() 의 msi_create_parent_irq_domain().
	 * 읽는 자: aspeed_pcie_intr_handler(), aspeed_pcie_msi_free().
	 * 값 범위: 해제 후 NULL */
	struct irq_domain *msi_domain;
	/* [한국어] 아래 MSI 비트맵을 보호하는 뮤텍스.
	 * 설정자: aspeed_pcie_probe() 의 devm_mutex_init().
	 * 읽는 자: aspeed_irq_msi_domain_alloc()/_free() 가 guard(mutex) 로 잡는다.
	 * 왜 필요한가: 여러 장치가 동시에 MSI 를 요청할 수 있고, 비트맵에서
	 *   빈자리를 찾는 일과 그것을 표시하는 일이 원자적이어야 한다.
	 * 왜 스핀락이 아닌가: 두 함수 모두 프로세스 컨텍스트에서만 불린다 */
	struct mutex lock;
	/* [한국어] 64개 MSI 벡터의 사용 여부 비트맵.
	 * 설정자/읽는 자: aspeed_irq_msi_domain_alloc() 의
	 *   bitmap_find_free_region() 과 _free() 의 bitmap_release_region().
	 * 값 범위: MAX_MSI_HOST_IRQS(64)비트. 하드웨어가 가진 벡터 수와 같다.
	 * 동기화: 위 lock 뮤텍스로 보호된다.
	 * 하드웨어 활성화 레지스터는 probe 때 전부 켜 두고, 개별 관리는 이
	 *   소프트웨어 비트맵으로만 한다 — INTx 가 하드웨어 비트로 개별 제어하는
	 *   것과 대조된다 */
	DECLARE_BITMAP(msi_irq_in_use, MAX_MSI_HOST_IRQS);

	/* [한국어] AST2700 에서 MSI 상태를 두 번 지워야 하는가.
	 * 설정자: aspeed_ast2700_setup() 이 true 로 세운다. AST2600 경로에서는
	 *   0 인 채로 남는다.
	 * 읽는 자: aspeed_pcie_intr_handler() — 참이면 상태 레지스터에 같은 값을
	 *   한 번 더 쓴다.
	 * 왜 필요한가: 그 함수 안의 원문 영어 주석이 workaround 라고 밝히고 있으며,
	 *   하드웨어가 한 번의 쓰기로 상태를 완전히 지우지 못하는 것으로 보인다.
	 *   근거가 되는 Aspeed 문서는 이 트리에 없다 */
	bool clear_msi_twice;		/* AST2700 workaround */
};

/**
 * struct aspeed_pcie_rc_platform - Platform information
 * @setup: initialization function
 * @pcie_map_ranges: function to map PCIe address ranges
 * @reg_intx_en: INTx enable register offset
 * @reg_intx_sts: INTx status register offset
 * @reg_msi_en: MSI enable register offset
 * @reg_msi_sts: MSI enable register offset
 * @msi_address: HW fixed MSI address
 */
struct aspeed_pcie_rc_platform {
	/* [한국어] 세대별 하드웨어 초기화 콜백.
	 * 설정자: pcie_rc_ast2600(aspeed_ast2600_setup)과
	 *   pcie_rc_ast2700(aspeed_ast2700_setup).
	 * 읽는 자: aspeed_pcie_probe() 가 한 번 부른다.
	 * 이 콜백 안에서 host->ops 와 host->child_ops 가 걸리므로, 이것이
	 *   config 접근 경로를 결정하는 지점이다 */
	int (*setup)(struct platform_device *pdev);
	/* [한국어] AHB 와 PCI 주소 공간 사이의 변환을 세우는 콜백.
	 * 설정자: 세대별 map_ranges 구현 둘.
	 * 읽는 자: aspeed_pcie_map_ranges() 가 첫 메모리 창을 찾아 넘긴다.
	 * 두 세대의 변환 레지스터 배치가 완전히 달라 콜백으로 나눠 두었다 */
	void (*pcie_map_ranges)(struct aspeed_pcie *pcie, u64 pci_addr);
	/* [한국어] INTx 활성화 레지스터의 오프셋.
	 * 값 범위: AST2600 은 0xc4, AST2700 은 0x40.
	 * 읽는 자: 세 irq_chip 콜백(ack/mask/unmask)과 aspeed_pcie_init_irq_domain().
	 * 그 값의 근거가 되는 Aspeed 문서는 이 트리에 없다 */
	int reg_intx_en;
	/* [한국어] INTx 상태 레지스터의 오프셋.
	 * 값 범위: AST2600 은 0xc8, AST2700 은 0x48.
	 * 읽는 자: aspeed_pcie_intr_handler() 가 읽고,
	 *   aspeed_pcie_init_irq_domain() 이 ~0 으로 묵은 상태를 지운다 */
	int reg_intx_sts;
	/* [한국어] MSI 활성화 레지스터의 오프셋. 여기서부터 4바이트 간격으로 두 개가
	 * 이어져 64 벡터를 덮는다.
	 * 값 범위: AST2600 은 0xe0, AST2700 은 0x50.
	 * 읽는 자: aspeed_pcie_msi_init() 이 둘 다 ~0 으로 켠다 */
	int reg_msi_en;
	/* [한국어] MSI 상태 레지스터의 오프셋. 역시 4바이트 간격으로 두 개다.
	 * 값 범위: AST2600 은 0xe8, AST2700 은 0x58.
	 * 읽는 자: aspeed_pcie_msi_init() 이 지우고,
	 *   aspeed_pcie_intr_handler() 가 읽고 되쓴다.
	 * (위 원문 kernel-doc 이 이 필드를 "MSI enable register offset" 이라
	 *  적었으나 실제로는 상태 레지스터다. 원문은 그대로 두고 이 사실만 적어 둔다.) */
	int reg_msi_sts;
	u32 msi_address;
};

/* [한국어]
 * aspeed_pcie_intx_irq_ack - INTx 인터럽트를 확인응답한다
 *
 * @d: 이 가상 IRQ 의 irq_data. hwirq 가 INTx 번호(0~3)다
 * @return: 없음
 *
 * irq_chip 의 .irq_ack 콜백이다. INTx 활성화 레지스터에서 이 hwirq 의
 * 비트를 세운다.
 *
 * 주목할 점: 이 함수의 본문이 aspeed_pcie_intx_irq_unmask() 와 완전히
 * 같다. 상태 레지스터가 아니라 활성화 레지스터를 건드리는 것도 "ack" 라는
 * 이름과 어긋나 보인다. 근거가 되는 Aspeed 하드웨어 문서는 이 트리에
 * 없어 그 이유를 확인하지 못했다 — 코드가 무엇을 하는지만 적어 둔다.
 * 코드는 고치지 않는다.
 *
 * handle_level_irq 는 핸들러를 부르기 전에 mask+ack 를, 부른 뒤 unmask 를
 * 하므로, 이 셋이 한 벌로 동작한다.
 *
 * 읽고-OR-쓰기 형태라 다른 INTx 의 비트는 보존된다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. irq_desc 의 락을 쥔 채 불린다.
 *
 * 호출 체인:
 *   (IRQ 코어의 handle_level_irq) → irq_chip.irq_ack → [이 함수] → writel()
 */
static void aspeed_pcie_intx_irq_ack(struct irq_data *d)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(d);
	/* [한국어] 세대별 INTx 활성화 레지스터 오프셋 */
	int intx_en = pcie->platform->reg_intx_en;
	/* [한국어] 읽고-고치고-쓰기에 쓸 지역 변수 */
	u32 en;

	/* [한국어] 현재 활성화 상태를 읽는다 */
	en = readl(pcie->reg + intx_en);
	/* [한국어] 이 hwirq 의 비트를 세운다. 상태 레지스터가 아니라 활성화 레지스터를
	 * 건드리는 점이 "ack" 라는 이름과 어긋나 보이나, 근거가 되는 문서는
	 * 이 트리에 없다 */
	en |= BIT(d->hwirq);
	writel(en, pcie->reg + intx_en);
}

/* [한국어]
 * aspeed_pcie_intx_irq_mask - INTx 인터럽트 하나를 막는다
 *
 * @d: 이 가상 IRQ 의 irq_data. hwirq 가 INTx 번호(0~3)다
 * @return: 없음
 *
 * irq_chip 의 .irq_mask 콜백이다. INTx 활성화 레지스터에서 이 hwirq 의
 * 비트를 AND-NOT 으로 지운다.
 *
 * pcie-altera.c 가 dummy_irq_chip 을 쓰는 것과 대비된다. 그쪽 하드웨어는
 * INTx 를 개별로 끄고 켤 수단이 없어 더미 chip 을 쓰지만, 이 하드웨어는
 * 비트마다 활성화 제어가 있어 진짜 irq_chip 을 구현할 수 있다. 덕분에
 * 드라이버가 disable_irq() 로 자기 INTx 만 막을 수 있다.
 *
 * 읽고-AND-NOT-쓰기라 다른 INTx 의 비트는 보존된다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트 또는 프로세스 컨텍스트.
 * irq_desc 의 락 아래에서 불린다.
 *
 * 호출 체인:
 *   (IRQ 코어) → irq_chip.irq_mask → [이 함수] → writel()
 */
static void aspeed_pcie_intx_irq_mask(struct irq_data *d)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(d);
	/* [한국어] 세대별 INTx 활성화 레지스터 오프셋 */
	int intx_en = pcie->platform->reg_intx_en;
	/* [한국어] 읽고-고치고-쓰기에 쓸 지역 변수 */
	u32 en;

	/* [한국어] 현재 활성화 상태를 읽는다 */
	en = readl(pcie->reg + intx_en);
	/* [한국어] 이 hwirq 의 비트만 지운다. 다른 INTx 는 그대로 둔다 */
	en &= ~BIT(d->hwirq);
	writel(en, pcie->reg + intx_en);
}

/* [한국어]
 * aspeed_pcie_intx_irq_unmask - INTx 인터럽트 하나를 다시 연다
 *
 * @d: 이 가상 IRQ 의 irq_data. hwirq 가 INTx 번호(0~3)다
 * @return: 없음
 *
 * aspeed_pcie_intx_irq_mask() 의 짝이다. 같은 비트를 OR 로 세운다.
 *
 * handle_level_irq 가 장치 핸들러를 부른 뒤 이것을 부른다. 레벨 트리거
 * 인터럽트이므로, 장치가 인터럽트 원인을 지우기 전에 unmask 하면
 * 인터럽트가 곧바로 다시 걸린다 — 그 순서를 IRQ 코어가 보장해 준다.
 *
 * 본문이 aspeed_pcie_intx_irq_ack() 와 같다(그쪽 주석 참조).
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. irq_desc 의 락 아래에서 불린다.
 *
 * 호출 체인:
 *   (IRQ 코어의 handle_level_irq) → irq_chip.irq_unmask → [이 함수] → writel()
 */
static void aspeed_pcie_intx_irq_unmask(struct irq_data *d)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(d);
	/* [한국어] 세대별 INTx 활성화 레지스터 오프셋 */
	int intx_en = pcie->platform->reg_intx_en;
	/* [한국어] 읽고-고치고-쓰기에 쓸 지역 변수 */
	u32 en;

	/* [한국어] 현재 활성화 상태를 읽는다 */
	en = readl(pcie->reg + intx_en);
	/* [한국어] 이 hwirq 의 비트를 세워 다시 연다 */
	en |= BIT(d->hwirq);
	/* [한국어] 되쓴다. handle_level_irq 가 장치 핸들러를 부른 뒤 여기에 온다 */
	writel(en, pcie->reg + intx_en);
}

/* [한국어] INTx 용 irq_chip. pcie-altera.c 가 dummy_irq_chip 을 쓰는 것과 달리
 * 이 하드웨어는 비트마다 활성화 제어가 있어 진짜 구현이 가능하다 */
static struct irq_chip aspeed_intx_irq_chip = {
	/* [한국어] /proc/interrupts 에 보이는 이름 */
	.name = "INTx",
	.irq_ack = aspeed_pcie_intx_irq_ack,
	.irq_mask = aspeed_pcie_intx_irq_mask,
	.irq_unmask = aspeed_pcie_intx_irq_unmask,
};

/* [한국어]
 * aspeed_pcie_intx_map - INTx 가상 IRQ 하나를 설정한다
 *
 * @domain: INTx 도메인.  @irq: 배정된 커널 가상 IRQ 번호
 * @hwirq: 하드웨어 IRQ 번호(0~3, INTA~INTD).  @return: 항상 0
 *
 * irq_domain_ops 의 .map 콜백이다. 하위 장치가 INTx 를 요청할 때 불린다.
 *
 * 셋을 설정한다.
 *   - aspeed_intx_irq_chip 과 handle_level_irq 를 건다. INTx 는 PCIe
 *     스펙상 레벨 트리거이므로 handle_level_irq 가 맞는 핸들러다.
 *     그것이 mask → ack → 장치 핸들러 → unmask 순서를 보장한다.
 *   - irq_set_chip_data 로 struct aspeed_pcie 를 붙인다. 위 세 irq_chip
 *     콜백이 irq_data_get_irq_chip_data() 로 그것을 되찾아 레지스터에
 *     접근한다 — pcie-altera.c 가 같은 일을 하고도 쓰지 않는 것과 달리
 *     여기서는 실제로 쓰인다.
 *   - IRQ_LEVEL 상태 플래그를 세운다. /proc/interrupts 표시와 IRQ 코어의
 *     처리에 쓰인다.
 *
 * 실행 컨텍스트: IRQ 매핑 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (irq_domain 코어) → irq_domain_ops.map → [이 함수]
 */
static int aspeed_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &aspeed_intx_irq_chip, handle_level_irq);
	/* [한국어] struct aspeed_pcie 를 이 IRQ 에 붙여 둔다. 위 세 irq_chip 콜백이
	 * irq_data_get_irq_chip_data() 로 되찾아 레지스터에 접근한다 */
	irq_set_chip_data(irq, domain->host_data);
	/* [한국어] 레벨 트리거 인터럽트임을 표시한다. /proc/interrupts 표시와
	 * IRQ 코어의 처리에 쓰인다 */
	irq_set_status_flags(irq, IRQ_LEVEL);

	return 0;
}

/* [한국어] INTx 도메인의 연산 표. xlate 를 지정하지 않아 기본 해석이 쓰인다 */
static const struct irq_domain_ops aspeed_intx_domain_ops = {
	.map = aspeed_pcie_intx_map,
};

/* [한국어]
 * aspeed_pcie_intr_handler - INTx 와 MSI 를 함께 처리하는 인터럽트 핸들러
 *
 * @irq: 이 컨트롤러의 IRQ 번호(쓰지 않는다).  @dev_id: struct aspeed_pcie
 * @return: 항상 IRQ_HANDLED
 *
 * 이 컨트롤러의 인터럽트 하나에 INTx 넷과 MSI 64개가 모두 모여 든다.
 * 그것을 풀어 각 장치의 핸들러로 나눠 주는 것이 이 함수다.
 *
 * pcie-altera.c 의 체인 핸들러와 달리 devm_request_irq(IRQF_SHARED) 로
 * 등록한 보통의 핸들러다. 그래서 chained_irq_enter/exit 로 감싸지 않는다.
 *
 * 두 부분이다.
 *
 *   1) INTx — 상태 레지스터에서 ASPEED_PCIE_INTX_STS(GENMASK(3,0))만
 *      뽑아 서 있는 비트마다 도메인 핸들러를 부른다.
 *      상태를 지우는 코드가 없는 점에 주의한다. INTx 는 레벨 트리거라
 *      장치가 원인을 지워야 내려가고, 그 사이의 재진입은 irq_chip 의
 *      mask/unmask 가 막아 준다.
 *
 *   2) MSI — 32비트 상태 레지스터 두 개(합쳐 64 벡터)를 훑는다.
 *      각 레지스터에 대해 읽고 → 그대로 되써서 지우고 → 서 있던 비트마다
 *      핸들러를 부른다. 지우기를 핸들러 호출보다 먼저 하므로, 처리 중
 *      도착한 MSI 를 잃지 않는다.
 *      bit += (i * 32) 로 두 번째 레지스터의 비트 번호를 32 만큼 밀어
 *      0~63 의 통합 hwirq 로 만든다.
 *
 *      AST2700 에서는 지우기를 한 번 더 한다. 바로 위 원문 영어 주석이
 *      그것을 workaround 라고 밝히고 있으며, 하드웨어가 한 번의 쓰기로
 *      상태를 완전히 지우지 못하는 것으로 보인다. 그 근거가 되는 문서는
 *      이 트리에 없다. clear_msi_twice 는 aspeed_ast2700_setup() 이
 *      세운다.
 *
 * 언제나 IRQ_HANDLED 를 돌려준다. IRQF_SHARED 로 등록했으므로 다른 장치의
 * 인터럽트에도 불릴 수 있는데, 그때 IRQ_NONE 을 돌려주지 않는다는 뜻이다.
 * 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. 잠들 수 없다.
 *
 * 호출 체인:
 *   (하드웨어 인터럽트) → [이 함수] → generic_handle_domain_irq()
 */
static irqreturn_t aspeed_pcie_intr_handler(int irq, void *dev_id)
{
	struct aspeed_pcie *pcie = dev_id;
	/* [한국어] 세대별 레지스터 오프셋 묶음. 매번 pcie->platform-> 을 쓰지 않으려고
	 * 지역 변수에 담는다 */
	const struct aspeed_pcie_rc_platform *platform = pcie->platform;
	/* [한국어] MSI 상태. for_each_set_bit 이 unsigned long 을 요구한다 */
	unsigned long status;
	/* [한국어] INTx 상태. 같은 이유로 unsigned long 이다 */
	unsigned long intx;
	/* [한국어] 서 있는 비트 번호 */
	u32 bit;
	/* [한국어] MSI 상태 레지스터 두 개를 훑을 인덱스 */
	int i;

	/* [한국어] INTx 상태에서 하위 네 비트만 뽑는다. FIELD_GET 이 마스크 위치만큼
	 * 밀어 정규화하는데, 이 마스크는 GENMASK(3,0) 이라 시프트가 없다 */
	intx = FIELD_GET(ASPEED_PCIE_INTX_STS,
				 readl(pcie->reg + platform->reg_intx_sts));
	/* [한국어] 서 있는 INTx 마다 */
	for_each_set_bit(bit, &intx, PCI_NUM_INTX)
		/* [한국어] 그 hwirq 에 매핑된 장치 핸들러를 부른다. 상태를 지우지 않는 점에
		 * 주의 — INTx 는 레벨 트리거라 장치가 원인을 지워야 내려가고,
		 * 그 사이의 재진입은 irq_chip 의 mask/unmask 가 막아 준다 */
		generic_handle_domain_irq(pcie->intx_domain, bit);

	/* [한국어] 32비트 상태 레지스터 두 개로 64 벡터를 덮는다 */
	for (i = 0; i < 2; i++) {
		/* [한국어] 두 레지스터가 4바이트 간격으로 인접해 있다 */
		int msi_sts_reg = platform->reg_msi_sts + (i * 4);

		/* [한국어] 이번 32 벡터의 상태를 읽는다 */
		status = readl(pcie->reg + msi_sts_reg);
		/* [한국어] 읽은 값을 그대로 되써서 지운다(RW1C 로 보이는 사용 방식).
		 * 핸들러 호출보다 먼저 지우므로 처리 중 도착한 MSI 를 잃지 않는다 */
		writel(status, pcie->reg + msi_sts_reg);

		/*
		 * AST2700 workaround:
		 * The MSI status needs to clear one more time.
		 */
		if (pcie->clear_msi_twice)
			/* [한국어] AST2700 에서만 한 번 더 지운다. 바로 위 원문 영어 주석이 이것을
			 * workaround 라고 밝히고 있다 */
			writel(status, pcie->reg + msi_sts_reg);

		/* [한국어] 이 레지스터에서 서 있는 벡터마다 */
		for_each_set_bit(bit, &status, 32) {
			/* [한국어] 두 번째 레지스터면 32 를 더해 0~63 의 통합 번호로 만든다 */
			bit += (i * 32);
			/* [한국어] 그 벡터에 매핑된 장치 핸들러를 부른다 */
			generic_handle_domain_irq(pcie->msi_domain, bit);
		}
	}

	return IRQ_HANDLED;
}

/* [한국어]
 * aspeed_pcie_get_bdf_offset - BDF 와 오프셋을 TLP 셋째 DWORD 형태로 만든다
 *
 * @bus: 대상 버스.  @devfn: 대상 장치/기능.  @where: config 오프셋
 * @return: 조립된 32비트 값
 *
 * Configuration Request TLP 의 셋째 DWORD 배치를 그대로 만든다.
 *   비트 31-24 : 버스 번호
 *   비트 23-19 : 장치 번호
 *   비트 18-16 : 기능 번호
 *   비트 11-0  : 레지스터 오프셋(DWORD 정렬)
 *
 * pcie-altera.c 의 TLP_CFG_DW2 와 같은 배치이지만 표현이 다르다. 저쪽은
 * devfn 을 통째로 16비트 자리에 밀어 넣는 반면, 이쪽은 PCI_SLOT/PCI_FUNC 로
 * 쪼개 각각의 자리에 놓는다. devfn 이 이미 (slot << 3) | func 형태라
 * 결과는 같다.
 *
 * (where & ~3) 으로 하위 두 비트를 지우는 것이 중요하다. config 요청은
 * DWORD 단위로 나가야 하고, 그 안에서 어느 바이트가 유효한지는 별도의
 * byte enable 필드가 알린다.
 *
 * 세 호출자가 모두 이 값을 그대로 레지스터에 쓴다 —
 * aspeed_ast2600_conf() 는 TX_DESC2 에, aspeed_ast2700_child_config() 는
 * CFGE_TLP_NEXT 에.
 *
 * 실행 컨텍스트: config 접근 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   aspeed_ast2600_conf() / aspeed_ast2700_child_config() → [이 함수]
 */
static u32 aspeed_pcie_get_bdf_offset(struct pci_bus *bus, unsigned int devfn,
				      int where)
{
	return ((bus->number) << 24) | (PCI_SLOT(devfn) << 19) |
		(PCI_FUNC(devfn) << 16) | (where & ~3);
}

/* [한국어]
 * aspeed_ast2600_conf - AST2600 의 config 접근 본체
 *
 * @bus: 대상 버스.  @devfn: 대상 장치/기능.  @where: config 오프셋
 * @size: 1/2/4 바이트.  @val: 읽기면 결과를 담을 곳, 쓰기면 쓸 값
 * @fmt_type: 미리 조립된 fmt/type DWORD 상위 바이트(CFG0/CFG1, 읽기/쓰기)
 * @write: 참이면 쓰기
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_SET_FAILED
 *
 * AST2600 의 읽기·쓰기·Root Port·하위 장치 네 경로가 모두 이 함수로 모인다.
 * 차이는 @fmt_type 과 @write 두 인자뿐이다.
 *
 * 절차:
 *   1) RX 버퍼 잠금 해제. 원문 영어 주석대로 다음 TX 를 트리거하기 전에
 *      드라이버가 RX 버퍼를 풀어 둘 수 있다. 앞선 접근이 남긴 상태를
 *      치우는 셈이다.
 *   2) TX 디스크립터를 채운다.
 *      DESC0 — fmt/type 과 payload 길이(1 DWORD).
 *      DESC1 — 세대 고정값 + 태그 + byte enable. 태그는 pcie->tx_tag 를
 *              비트 11-8 자리에 넣는다.
 *      DESC2 — BDF 와 오프셋.
 *      DESC3 — 0.
 *      쓰기면 DESC_DATA 에 값을 자리에 맞춰 밀어 넣는다.
 *   3) 상태 레지스터의 트리거 비트를 세워 전송을 시작한다.
 *   4) TX idle 을 최대 50마이크로초 폴링한다. 시간 초과면 오류를 남기고
 *      PCI_SET_ERROR_RESPONSE 로 *val 을 0xffffffff 계열 값으로 채운 뒤
 *      out 으로 빠진다.
 *   5) TX idle 상태를 지운다.
 *   6) 전송 결과를 본다.
 *      RC_H_TX_COMPLETE — 정상. RX done 을 다시 폴링하고, 읽기면
 *        완료 디스크립터의 Completion Status 를 확인한 뒤 데이터를 꺼낸다.
 *        Status 가 성공이 아니면 실패로 처리한다 — 없는 장치를 찌른 경우가
 *        여기로 온다.
 *      STATUS_OF_TX(마스크 전체가 선 상태) — 실패.
 *      그 밖 — HOST_RX_DESC_DATA 를 그대로 읽어 값으로 쓴다.
 *   7) RX 버퍼를 다시 풀고, 읽은 DWORD 에서 요청한 크기만큼 잘라 낸다.
 *
 * out 라벨은 성공·실패 모두가 지난다. 장치 상태를 읽어 그대로 되쓰고
 * (RW1C 로 보이는 사용 방식), 태그를 하나 증가시킨다(0~7 순환).
 * 태그를 돌리는 이유는 연속된 요청을 하드웨어가 구분할 수 있게 하기
 * 위한 것으로 보이나, 근거가 되는 문서는 이 트리에 없다.
 *
*val = TLP_GET_VALUE(...) 가 실패 경로에서는 실행되지 않는 점에 주의한다.
 * 그 경우 PCI_SET_ERROR_RESPONSE 가 이미 값을 채워 두었다.
 *
 * 실행 컨텍스트: config 접근 경로. PCI 코어가 스핀락을 쥐고 있어 잠들 수
 * 없고, 그래서 폴링이 sleep_us=0 형태다.
 *
 * 호출 체인:
 *   (PCI 코어) → 네 래퍼 중 하나 → [이 함수]
 *     → aspeed_pcie_get_bdf_offset() → readl_poll_timeout()
 */
static int aspeed_ast2600_conf(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 *val, u32 fmt_type,
			       bool write)
{
	struct aspeed_pcie *pcie = bus->sysdata;
	/* [한국어] bdf_offset = TLP 셋째 DWORD, cfg_val = 여러 용도로 재사용하는 임시,
	 * isr = RX done 폴링에 쓸 상태 */
	u32 bdf_offset, cfg_val, isr;
	/* [한국어] 각 단계의 결과 */
	int ret;

	/* [한국어] BDF 와 오프셋을 TLP 셋째 DWORD 형태로 조립한다 */
	bdf_offset = aspeed_pcie_get_bdf_offset(bus, devfn, where);

	/* Driver may set unlock RX buffer before triggering next TX config */
	cfg_val = readl(pcie->reg + ASPEED_H2X_DEV_CTRL);
	/* [한국어] 바로 위 원문 영어 주석대로 다음 TX 를 트리거하기 전에 RX 버퍼를
	 * 풀어 둔다. 앞선 접근이 남긴 상태를 치우는 셈이다 */
	writel(ASPEED_PCIE_UNLOCK_RX_BUFF | cfg_val,
	       pcie->reg + ASPEED_H2X_DEV_CTRL);

	/* [한국어] TLP 첫 DWORD — fmt/type 과 payload 길이(1 DWORD) */
	cfg_val = fmt_type | CFG_PAYLOAD_SIZE;
	/* [한국어] TX 디스크립터 0 에 쓴다 */
	writel(cfg_val, pcie->reg + ASPEED_H2X_TX_DESC0);

	/* [한국어] 세대 고정값에 */
	cfg_val = AST2600_TX_DESC1_VALUE |
		  /* [한국어] 태그를 비트 11-8 자리에 넣고 */
		  FIELD_PREP(GENMASK(11, 8), pcie->tx_tag) |
		  /* [한국어] byte enable 을 더한다. 접근 크기와 오프셋으로 계산된 연속 비트다 */
		  TLP_HEADER_BYTE_EN(size, where);
	/* [한국어] TX 디스크립터 1 에 쓴다 */
	writel(cfg_val, pcie->reg + ASPEED_H2X_TX_DESC1);

	/* [한국어] TX 디스크립터 2 에 BDF 와 오프셋 */
	writel(bdf_offset, pcie->reg + ASPEED_H2X_TX_DESC2);
	/* [한국어] TX 디스크립터 3 은 쓰지 않으므로 0 */
	writel(0, pcie->reg + ASPEED_H2X_TX_DESC3);
	/* [한국어] 쓰기 요청이면 */
	if (write)
		/* [한국어] 데이터를 DWORD 안의 제자리로 밀어 넣어 데이터 레지스터에 쓴다 */
		writel(TLP_SET_VALUE(*val, size, where),
		       pcie->reg + ASPEED_H2X_TX_DESC_DATA);

	/* [한국어] 상태 레지스터를 읽어 */
	cfg_val = readl(pcie->reg + ASPEED_H2X_STS);
	/* [한국어] 전송 트리거 비트를 세우고 */
	cfg_val |= ASPEED_PCIE_TRIGGER_TX;
	/* [한국어] 되쓴다. 이 순간 TLP 가 나간다 */
	writel(cfg_val, pcie->reg + ASPEED_H2X_STS);

	/* [한국어] TX idle 이 될 때까지 최대 50마이크로초 폴링한다. sleep_us 가 0 이라
	 * 순수 스핀인데, config 접근이 PCI 코어의 스핀락 아래에서 돌아
	 * 잠들 수 없기 때문이다 */
	ret = readl_poll_timeout(pcie->reg + ASPEED_H2X_STS, cfg_val,
				 (cfg_val & ASPEED_PCIE_TX_IDLE), 0, 50);
	/* [한국어] 시간 초과 */
	if (ret) {
		dev_err(pcie->dev,
			"%02x:%02x.%d CR tx timeout sts: 0x%08x\n",
			bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), cfg_val);
		ret = PCIBIOS_SET_FAILED;
		PCI_SET_ERROR_RESPONSE(val);
		goto out;
	}

	/* [한국어] TX idle 인터럽트 상태를 읽어 */
	cfg_val = readl(pcie->reg + ASPEED_H2X_INT_STS);
	/* [한국어] 클리어 비트를 세우고 */
	cfg_val |= ASPEED_PCIE_TX_IDLE_CLEAR;
	/* [한국어] 되써서 지운다 */
	writel(cfg_val, pcie->reg + ASPEED_H2X_INT_STS);

	/* [한국어] 전송 결과를 확인한다 */
	cfg_val = readl(pcie->reg + ASPEED_H2X_STS);
	/* [한국어] 상태 필드의 값에 따라 갈린다 */
	switch (cfg_val & ASPEED_PCIE_STATUS_OF_TX) {
	/* [한국어] 정상 완료 — 이제 응답을 기다린다 */
	case ASPEED_PCIE_RC_H_TX_COMPLETE:
		ret = readl_poll_timeout(pcie->reg + ASPEED_H2X_DEV_STS, isr,
					 (isr & ASPEED_PCIE_RC_RX_DONE_ISR), 0,
					 50);
		/* [한국어] RX done 폴링이 시간 초과 */
		if (ret) {
			dev_err(pcie->dev,
				"%02x:%02x.%d CR rx timeout sts: 0x%08x\n",
				bus->number, PCI_SLOT(devfn),
				PCI_FUNC(devfn), isr);
			ret = PCIBIOS_SET_FAILED;
			PCI_SET_ERROR_RESPONSE(val);
			goto out;
		}
		/* [한국어] 읽기 요청이면 응답 데이터를 꺼내야 한다 */
		if (!write) {
			/* [한국어] 완료 디스크립터를 읽어 */
			cfg_val = readl(pcie->reg + ASPEED_H2X_DEV_RX_DESC1);
			/* [한국어] Completion Status 를 확인한다. 성공이 아니면 요청이 거부된 것이다 —
			 * 없는 장치를 찌른 경우가 여기로 온다 */
			if (CPL_STS(cfg_val) != PCIE_CPL_STS_SUCCESS) {
				ret = PCIBIOS_SET_FAILED;
				PCI_SET_ERROR_RESPONSE(val);
				goto out;
			} else {
				*val = readl(pcie->reg +
					     ASPEED_H2X_DEV_RX_DESC_DATA);
			}
		}
		break;
	/* [한국어] 상태 필드가 마스크 전체와 같은 경우. 전송 실패로 처리한다 */
	case ASPEED_PCIE_STATUS_OF_TX:
		ret = PCIBIOS_SET_FAILED;
		PCI_SET_ERROR_RESPONSE(val);
		goto out;
	default:
		*val = readl(pcie->reg + ASPEED_H2X_HOST_RX_DESC_DATA);
		break;
	}

	/* [한국어] 장치 제어 레지스터를 읽어 */
	cfg_val = readl(pcie->reg + ASPEED_H2X_DEV_CTRL);
	/* [한국어] RX 버퍼 잠금 해제 비트를 세우고 */
	cfg_val |= ASPEED_PCIE_UNLOCK_RX_BUFF;
	/* [한국어] 되쓴다. 다음 접근을 위해 버퍼를 풀어 두는 것이다 */
	writel(cfg_val, pcie->reg + ASPEED_H2X_DEV_CTRL);

	*val = TLP_GET_VALUE(*val, size, where);

	ret = PCIBIOS_SUCCESSFUL;
out:
	cfg_val = readl(pcie->reg + ASPEED_H2X_DEV_STS);
	/* [한국어] 장치 상태를 읽은 값 그대로 되써서 지운다(RW1C 로 보이는 사용 방식).
	 * 성공·실패 어느 경로로 왔든 지나간다 */
	writel(cfg_val, pcie->reg + ASPEED_H2X_DEV_STS);
	/* [한국어] 다음 요청을 위해 태그를 하나 증가시킨다. 0~7 을 순환한다 */
	pcie->tx_tag = (pcie->tx_tag + 1) % 0x8;
	return ret;
}

/* [한국어]
 * aspeed_ast2600_rd_conf - AST2600 루트 버스의 config 읽기
 *
 * @bus: 루트 버스.  @devfn: 대상 장치/기능.  @where: 오프셋.  @size: 크기
 * @val: 읽은 값을 담을 곳
 * @return: PCIBIOS_DEVICE_NOT_FOUND 또는 본체가 돌려준 값
 *
 * host->ops.read 로 등록되어 루트 버스 접근에만 쓰인다.
 *
 * 바로 안쪽 원문 영어 주석대로 AST2600 은 루트 버스에 Root Port 가
 * 하나뿐이다. 그 하나가 슬롯 8 에 있어, 그 밖의 슬롯을 찌르면 곧바로
 * "장치 없음" 을 돌려준다. 이 검사가 없으면 열거 중 32개 슬롯을 모두
 * 찔러 매번 50마이크로초 폴링을 두 번씩 하게 된다.
 *
 * Type 0 읽기 fmt/type 을 넘긴다 — 루트 버스의 장치는 이 링크 바로 아래에
 * 있으므로 Type 0 이다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유).
 *
 * 호출 체인:
 *   (PCI 코어) → host->ops.read → [이 함수] → aspeed_ast2600_conf()
 */
static int aspeed_ast2600_rd_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 *val)
{
	/*
	 * AST2600 has only one Root Port on the root bus.
	 */
	if (PCI_SLOT(devfn) != 8)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] Type 0 읽기 fmt/type 을 넘긴다 — 루트 버스의 장치는 이 링크 바로
	 * 아래에 있으므로 Type 0 이다 */
	return aspeed_ast2600_conf(bus, devfn, where, size, val,
				   CFG0_READ_FMTTYPE, false);
}

/* [한국어]
 * aspeed_ast2600_child_rd_conf - AST2600 하위 버스의 config 읽기
 *
 * @bus: 루트 버스 아래의 버스.  @devfn: 대상 장치/기능.  @where: 오프셋
 * @size: 크기.  @val: 읽은 값을 담을 곳
 * @return: 본체가 돌려준 값
 *
 * host->child_ops.read 로 등록된다. 커널의 pci_host_bridge 는 루트 버스와
 * 그 아래 버스에 서로 다른 ops 를 걸 수 있게 해 주는데, 이 드라이버가
 * 그 구조를 쓰는 이유는 Type 0 과 Type 1 을 가르기 위해서다.
 *
 * Type 1 읽기 fmt/type 을 넘긴다 — 하위 버스의 장치는 브리지를 한 번 더
 * 지나야 하므로 Type 1 이다.
 *
 * 슬롯 검사가 없는 점이 루트 버스 판과 다르다. 하위 버스에는 여러 장치가
 * 있을 수 있기 때문이다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유).
 *
 * 호출 체인:
 *   (PCI 코어) → host->child_ops.read → [이 함수] → aspeed_ast2600_conf()
 */
static int aspeed_ast2600_child_rd_conf(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 *val)
{
	return aspeed_ast2600_conf(bus, devfn, where, size, val,
				   CFG1_READ_FMTTYPE, false);
}

/* [한국어]
 * aspeed_ast2600_wr_conf - AST2600 루트 버스의 config 쓰기
 *
 * @bus: 루트 버스.  @devfn: 대상 장치/기능.  @where: 오프셋.  @size: 크기
 * @val: 쓸 값
 * @return: PCIBIOS_DEVICE_NOT_FOUND 또는 본체가 돌려준 값
 *
 * 읽기 판과 같은 슬롯 8 검사를 거친 뒤 Type 0 쓰기 fmt/type 으로 본체를
 * 부른다.
 *
 * &val 로 주소를 넘기는 점에 주의한다. 본체가 읽기·쓰기를 한 함수로
 * 처리하느라 u32 * 를 받기 때문이며, 쓰기 경로에서는 그 포인터가 입력으로만
 * 쓰인다 — 다만 실패 시 PCI_SET_ERROR_RESPONSE 가 그 지역 변수를 덮어쓰고,
 * 그 값은 호출자에게 전달되지 않는다. 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유).
 *
 * 호출 체인:
 *   (PCI 코어) → host->ops.write → [이 함수] → aspeed_ast2600_conf()
 */
static int aspeed_ast2600_wr_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 val)
{
	/*
	 * AST2600 has only one Root Port on the root bus.
	 */
	if (PCI_SLOT(devfn) != 8)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] Type 0 쓰기 fmt/type 을 넘긴다. &val 로 지역 변수의 주소를 넘기는 것은
	 * 본체가 읽기·쓰기를 한 함수로 처리하느라 u32 포인터를 받기 때문이다 */
	return aspeed_ast2600_conf(bus, devfn, where, size, &val,
				   CFG0_WRITE_FMTTYPE, true);
}

/* [한국어]
 * aspeed_ast2600_child_wr_conf - AST2600 하위 버스의 config 쓰기
 *
 * @bus: 루트 버스 아래의 버스.  @devfn: 대상 장치/기능.  @where: 오프셋
 * @size: 크기.  @val: 쓸 값
 * @return: 본체가 돌려준 값
 *
 * host->child_ops.write 로 등록되며, Type 1 쓰기 fmt/type 으로 본체를
 * 부른다. 네 래퍼 중 가장 단순하다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유).
 *
 * 호출 체인:
 *   (PCI 코어) → host->child_ops.write → [이 함수] → aspeed_ast2600_conf()
 */
static int aspeed_ast2600_child_wr_conf(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 val)
{
	return aspeed_ast2600_conf(bus, devfn, where, size, &val,
				   CFG1_WRITE_FMTTYPE, true);
}

/* [한국어]
 * aspeed_ast2700_config - AST2700 Root Port 자신의 config 접근(CFGI 경로)
 *
 * @bus: 루트 버스.  @devfn: 대상(언제나 0).  @where: 오프셋.  @size: 크기
 * @val: 읽기면 결과를 담을 곳, 쓰기면 쓸 값.  @write: 참이면 쓰기
 * @return: 언제나 PCIBIOS_SUCCESSFUL
 *
 * AST2600 의 본체와 비교하면 놀랄 만큼 단순하다. "CFGI"(Config Internal)는
 * Root Port 자신의 config 를 다루는 전용 창구라, TLP 를 만들어 링크로
 * 내보낼 필요가 없기 때문이다. BDF 도 필요 없다 — 대상이 자기 자신으로
 * 정해져 있다.
 *
 * 절차:
 *   1) CFGI_TLP 레지스터에 byte enable 과 오프셋을 함께 쓴다. 쓰기면
 *      ASPEED_CFGI_WRITE 비트를 덧붙여 방향을 알린다.
 *   2) 쓸 값을 CFGI_WR_DATA 에 자리에 맞춰 밀어 넣는다. 읽기에서도 이
 *      쓰기가 실행되는데, *val 이 그때 무엇이든 하드웨어가 무시하는
 *      것으로 보인다. 코드는 고치지 않고 이 관찰만 적어 둔다.
 *   3) CFGI_CTRL 에 FIRE 비트를 써서 실행한다.
 *   4) CFGI_RET_DATA 에서 결과를 읽고, 요청한 크기만큼 잘라 낸다.
 *
 * 폴링이 없다. 자기 자신에 대한 접근이라 즉시 끝난다는 전제이며, 그래서
 * 실패 경로도 없고 언제나 성공을 돌려준다. 근거가 되는 하드웨어 문서는
 * 이 트리에 없다.
 *
 * 쓰기 경로에서도 마지막 두 줄이 실행되어 *val 이 덮어써진다. 호출자
 * aspeed_ast2700_wr_conf() 가 지역 변수의 주소를 넘기므로 밖으로 새지는
 * 않는다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들지 않는다.
 *
 * 호출 체인:
 *   (PCI 코어) → host->ops.read/write → aspeed_ast2700_rd_conf()/_wr_conf()
 *     → [이 함수]
 */
static int aspeed_ast2700_config(struct pci_bus *bus, unsigned int devfn,
				 int where, int size, u32 *val, bool write)
{
	struct aspeed_pcie *pcie = bus->sysdata;
	/* [한국어] 레지스터에 쓸 값을 조립할 임시 변수 */
	u32 cfg_val;

	/* [한국어] byte enable 을 전용 필드 자리에 넣고 */
	cfg_val = ASPEED_CFGI_BYTE_EN(TLP_HEADER_BYTE_EN(size, where)) |
		  /* [한국어] DWORD 정렬된 오프셋을 더한다 */
		  (where & ~3);
	/* [한국어] 쓰기 요청이면 */
	if (write)
		/* [한국어] 방향 비트를 덧붙인다 */
		cfg_val |= ASPEED_CFGI_WRITE;
	/* [한국어] CFGI TLP 레지스터에 쓴다. BDF 가 없는 것이 이 경로의 특징 —
	 * 대상이 Root Port 자신으로 정해져 있기 때문이다 */
	writel(cfg_val, pcie->reg + ASPEED_H2X_CFGI_TLP);

	/* [한국어] 쓸 값을 자리에 맞춰 밀어 넣는다. 읽기에서도 이 쓰기가 실행되는데,
	 * 그때 *val 이 무엇이든 하드웨어가 무시하는 것으로 보인다 */
	writel(TLP_SET_VALUE(*val, size, where),
	       pcie->reg + ASPEED_H2X_CFGI_WR_DATA);
	writel(ASPEED_CFGI_TLP_FIRE, pcie->reg + ASPEED_H2X_CFGI_CTRL);
	*val = readl(pcie->reg + ASPEED_H2X_CFGI_RET_DATA);
	*val = TLP_GET_VALUE(*val, size, where);

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * aspeed_ast2700_child_config - AST2700 하위 장치의 config 접근(CFGE 경로)
 *
 * @bus: 루트 버스 아래의 버스.  @devfn: 대상 장치/기능.  @where: 오프셋
 * @size: 크기.  @val: 읽기면 결과를 담을 곳, 쓰기면 쓸 값.  @write: 방향
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_SET_FAILED
 *
 * "CFGE"(Config External)는 링크 너머의 장치를 향하는 창구라, 여기서는
 * 실제로 TLP 를 만들어 내보내야 한다. 구조가 AST2600 의 본체와 비슷하다.
 *
 * Type 0/Type 1 판정이 이 함수의 특징이다.
 *   bus->number == pcie->root_bus_nr + 1 이면 Type 0, 아니면 Type 1.
 * 즉 "루트 버스 바로 다음 버스" 가 Root Port 의 secondary bus 이고, 그
 * 버스의 장치는 링크 바로 아래에 있으므로 Type 0 이다. 그보다 먼 버스는
 * 브리지를 더 지나야 하므로 Type 1 이다.
 * pcie->root_bus_nr 은 probe 에서 장치 트리의 bus-range 로부터 한 번
 * 정해진다(AST2600 경로가 config 쓰기를 엿보아 갱신하는 것과 다르다).
 *
 * TLP 를 세 번(쓰기면 네 번)의 레지스터 쓰기로 밀어 넣는다. 같은
 * CFGE_TLP_NEXT 레지스터에 연달아 쓰는 것이 요점이다 — 하드웨어가 내부
 * 포인터를 자동으로 전진시키는 FIFO 형태로 보인다.
 *   TLP_1ST      — fmt/type 과 payload 길이
 *   TLP_NEXT (1) — 세대 고정값 + 태그 + byte enable
 *   TLP_NEXT (2) — BDF 와 오프셋
 *   TLP_NEXT (3) — 쓰기일 때만, 데이터
 *
 * 그 다음 상태 비트 둘을 지우고 FIRE 로 전송한다. 완료는 두 단계 폴링이다 —
 * TX idle 을 기다리고, 다시 RX busy 를 기다린다. 각각 50마이크로초 상한이며
 * 어느 쪽이든 시간 초과면 오류를 남기고 PCI_SET_ERROR_RESPONSE 로 값을
 * 채운 뒤 out 으로 빠진다.
 *
 * 두 번째 폴링의 조건이 (status & ASPEED_CFGE_RX_BUSY) 인 점에 주의한다.
 * 이름이 "busy" 인 비트가 서기를 기다리는 형태인데, 그 비트의 실제 의미는
 * Aspeed 문서 소관이라 이 트리에서 확인하지 못했다.
 *
 * AST2600 판과 달리 Completion Status 를 확인하지 않는다. 없는 장치를
 * 찌르면 RET_DATA 에 0xffffffff 계열 값이 읽혀 PCI 코어가 "장치 없음" 으로
 * 판단하는 것으로 보인다.
 *
 * out 라벨에서 상태를 되쓰고 태그를 증가시킨다. 태그의 순환 폭이 0xf 로
 * AST2600 의 0x8 과 다르다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들지 않는다.
 *
 * 호출 체인:
 *   (PCI 코어) → host->child_ops.read/write
 *     → aspeed_ast2700_child_rd_conf()/_wr_conf() → [이 함수]
 */
static int aspeed_ast2700_child_config(struct pci_bus *bus, unsigned int devfn,
				       int where, int size, u32 *val,
				       bool write)
{
	struct aspeed_pcie *pcie = bus->sysdata;
	/* [한국어] bdf_offset = TLP 셋째 DWORD, status = 폴링 결과, cfg_val = 임시 */
	u32 bdf_offset, status, cfg_val;
	/* [한국어] 각 단계의 결과 */
	int ret;

	/* [한국어] BDF 와 오프셋을 조립한다 */
	bdf_offset = aspeed_pcie_get_bdf_offset(bus, devfn, where);

	/* [한국어] payload 길이(1 DWORD)로 시작한다 */
	cfg_val = CFG_PAYLOAD_SIZE;
	/* [한국어] 쓰기 요청이면 */
	if (write)
		/* [한국어] 루트 버스 바로 다음 버스면 Type 0, 그보다 멀면 Type 1 쓰기.
		 * "바로 다음" 이 Root Port 의 secondary bus 이고, 그 버스의 장치는
		 * 링크 바로 아래에 있으므로 Type 0 이다 */
		cfg_val |= (bus->number == (pcie->root_bus_nr + 1)) ?
				   CFG0_WRITE_FMTTYPE :
				   CFG1_WRITE_FMTTYPE;
	else
		/* [한국어] 읽기 쪽도 같은 기준으로 Type 을 고른다 */
		cfg_val |= (bus->number == (pcie->root_bus_nr + 1)) ?
				   CFG0_READ_FMTTYPE :
				   CFG1_READ_FMTTYPE;
	/* [한국어] TLP 첫 DWORD 를 CFGE 레지스터에 쓴다 */
	writel(cfg_val, pcie->reg + ASPEED_H2X_CFGE_TLP_1ST);

	/* [한국어] 세대 고정값에 */
	cfg_val = AST2700_TX_DESC1_VALUE |
		  /* [한국어] 태그를 비트 11-8 자리에 넣고 */
		  FIELD_PREP(GENMASK(11, 8), pcie->tx_tag) |
		  /* [한국어] byte enable 을 더한다 */
		  TLP_HEADER_BYTE_EN(size, where);
	/* [한국어] 같은 NEXT 레지스터에 연달아 쓴다. 하드웨어가 내부 포인터를 자동으로
	 * 전진시키는 FIFO 형태로 보인다 */
	writel(cfg_val, pcie->reg + ASPEED_H2X_CFGE_TLP_NEXT);

	/* [한국어] 세 번째 DWORD — BDF 와 오프셋 */
	writel(bdf_offset, pcie->reg + ASPEED_H2X_CFGE_TLP_NEXT);
	/* [한국어] 쓰기 요청이면 */
	if (write)
		/* [한국어] 네 번째 DWORD 로 데이터를 밀어 넣는다 */
		writel(TLP_SET_VALUE(*val, size, where),
		       pcie->reg + ASPEED_H2X_CFGE_TLP_NEXT);
	/* [한국어] 전송 전에 두 상태 비트를 지운다 */
	writel(ASPEED_CFGE_TX_IDLE | ASPEED_CFGE_RX_BUSY,
	       pcie->reg + ASPEED_H2X_CFGE_INT_STS);
	/* [한국어] FIRE 비트로 전송을 시작한다 */
	writel(ASPEED_CFGE_TLP_FIRE, pcie->reg + ASPEED_H2X_CFGE_CTRL);

	/* [한국어] 1단계 — TX idle 을 최대 50마이크로초 폴링한다 */
	ret = readl_poll_timeout(pcie->reg + ASPEED_H2X_CFGE_INT_STS, status,
				 (status & ASPEED_CFGE_TX_IDLE), 0, 50);
	/* [한국어] 시간 초과 */
	if (ret) {
		dev_err(pcie->dev,
			"%02x:%02x.%d CR tx timeout sts: 0x%08x\n",
			bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), status);
		ret = PCIBIOS_SET_FAILED;
		PCI_SET_ERROR_RESPONSE(val);
		goto out;
	}

	/* [한국어] 2단계 — RX busy 를 기다린다. 이름이 "busy" 인 비트가 서기를
	 * 기다리는 형태인데, 그 실제 의미는 Aspeed 문서 소관이라 이 트리에서
	 * 확인하지 못했다 */
	ret = readl_poll_timeout(pcie->reg + ASPEED_H2X_CFGE_INT_STS, status,
				 (status & ASPEED_CFGE_RX_BUSY), 0, 50);
	/* [한국어] 시간 초과 */
	if (ret) {
		dev_err(pcie->dev,
			"%02x:%02x.%d CR rx timeout sts: 0x%08x\n",
			bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), status);
		ret = PCIBIOS_SET_FAILED;
		PCI_SET_ERROR_RESPONSE(val);
		goto out;
	}
	*val = readl(pcie->reg + ASPEED_H2X_CFGE_RET_DATA);
	*val = TLP_GET_VALUE(*val, size, where);

	ret = PCIBIOS_SUCCESSFUL;
out:
	writel(status, pcie->reg + ASPEED_H2X_CFGE_INT_STS);
	/* [한국어] 다음 요청을 위해 태그를 증가시킨다. AST2600 의 0x8 과 달리 0xf 로
	 * 순환한다 */
	pcie->tx_tag = (pcie->tx_tag + 1) % 0xf;
	return ret;
}

/* [한국어]
 * aspeed_ast2700_rd_conf - AST2700 루트 버스의 config 읽기
 *
 * @bus: 루트 버스.  @devfn: 대상 장치/기능.  @where: 오프셋.  @size: 크기
 * @val: 읽은 값을 담을 곳
 * @return: PCIBIOS_DEVICE_NOT_FOUND 또는 본체가 돌려준 값
 *
 * 바로 안쪽 원문 영어 주석대로 AST2700 도 루트 버스에 Root Port 가
 * 하나뿐이다. 다만 그 위치가 AST2600 과 다르다 — 이쪽은 devfn 0 이고,
 * AST2600 은 슬롯 8 이다. 그래서 검사식도 devfn != 0 이다.
 *
 * CFGI 경로(자기 자신 전용 창구)로 내려간다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유).
 *
 * 호출 체인:
 *   (PCI 코어) → host->ops.read → [이 함수] → aspeed_ast2700_config()
 */
static int aspeed_ast2700_rd_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 *val)
{
	/*
	 * AST2700 has only one Root Port on the root bus.
	 */
	if (devfn != 0)
		return PCIBIOS_DEVICE_NOT_FOUND;

	return aspeed_ast2700_config(bus, devfn, where, size, val, false);
}

/* [한국어]
 * aspeed_ast2700_child_rd_conf - AST2700 하위 버스의 config 읽기
 *
 * @bus: 루트 버스 아래의 버스.  @devfn: 대상 장치/기능.  @where: 오프셋
 * @size: 크기.  @val: 읽은 값을 담을 곳
 * @return: 본체가 돌려준 값
 *
 * host->child_ops.read 로 등록된다. CFGE 경로(링크 너머 전용 창구)로
 * 내려가며, Type 0/Type 1 판정은 본체가 버스 번호를 보고 한다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유).
 *
 * 호출 체인:
 *   (PCI 코어) → host->child_ops.read → [이 함수]
 *     → aspeed_ast2700_child_config()
 */
static int aspeed_ast2700_child_rd_conf(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 *val)
{
	return aspeed_ast2700_child_config(bus, devfn, where, size, val, false);
}

/* [한국어]
 * aspeed_ast2700_wr_conf - AST2700 루트 버스의 config 쓰기
 *
 * @bus: 루트 버스.  @devfn: 대상 장치/기능.  @where: 오프셋.  @size: 크기
 * @val: 쓸 값
 * @return: PCIBIOS_DEVICE_NOT_FOUND 또는 본체가 돌려준 값
 *
 * 읽기 판과 같은 devfn 0 검사를 거친 뒤 CFGI 경로로 내려간다.
 * &val 로 지역 변수의 주소를 넘기므로, 본체가 그 자리를 덮어써도 밖으로
 * 새지 않는다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유).
 *
 * 호출 체인:
 *   (PCI 코어) → host->ops.write → [이 함수] → aspeed_ast2700_config()
 */
static int aspeed_ast2700_wr_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 val)
{
	/*
	 * AST2700 has only one Root Port on the root bus.
	 */
	if (devfn != 0)
		return PCIBIOS_DEVICE_NOT_FOUND;

	return aspeed_ast2700_config(bus, devfn, where, size, &val, true);
}

/* [한국어]
 * aspeed_ast2700_child_wr_conf - AST2700 하위 버스의 config 쓰기
 *
 * @bus: 루트 버스 아래의 버스.  @devfn: 대상 장치/기능.  @where: 오프셋
 * @size: 크기.  @val: 쓸 값
 * @return: 본체가 돌려준 값
 *
 * host->child_ops.write 로 등록되며 CFGE 경로로 내려간다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유).
 *
 * 호출 체인:
 *   (PCI 코어) → host->child_ops.write → [이 함수]
 *     → aspeed_ast2700_child_config()
 */
static int aspeed_ast2700_child_wr_conf(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 val)
{
	return aspeed_ast2700_child_config(bus, devfn, where, size, &val, true);
}

/* [한국어] AST2600 의 루트 버스용 연산 표. Type 0 경로다 */
static struct pci_ops aspeed_ast2600_pcie_ops = {
	/* [한국어] 루트 버스 config 읽기 */
	.read = aspeed_ast2600_rd_conf,
	.write = aspeed_ast2600_wr_conf,
};

/* [한국어] AST2600 의 하위 버스용 연산 표. Type 1 경로다.
 * 커널의 pci_host_bridge 가 두 벌을 걸 수 있게 해 주는 덕분에
 * 버스 번호로 매번 판정할 필요가 없다 */
static struct pci_ops aspeed_ast2600_pcie_child_ops = {
	/* [한국어] 하위 버스 config 읽기 */
	.read = aspeed_ast2600_child_rd_conf,
	.write = aspeed_ast2600_child_wr_conf,
};

/* [한국어] AST2700 의 루트 버스용 연산 표. CFGI(자기 자신 전용) 경로다 */
static struct pci_ops aspeed_ast2700_pcie_ops = {
	/* [한국어] 루트 버스 config 읽기 */
	.read = aspeed_ast2700_rd_conf,
	.write = aspeed_ast2700_wr_conf,
};

/* [한국어] AST2700 의 하위 버스용 연산 표. CFGE(링크 너머) 경로다 */
static struct pci_ops aspeed_ast2700_pcie_child_ops = {
	/* [한국어] 하위 버스 config 읽기 */
	.read = aspeed_ast2700_child_rd_conf,
	.write = aspeed_ast2700_child_wr_conf,
};

/* [한국어]
 * aspeed_irq_compose_msi_msg - EP 에 알려 줄 MSI 주소와 데이터를 만든다
 *
 * @data: 이 MSI 벡터의 irq_data. hwirq 가 벡터 번호(0~63)다
 * @msg: 채울 MSI 메시지.  @return: 없음
 *
 * MSI 는 "EP 가 특정 주소에 특정 값을 쓰면 그것이 인터럽트가 된다" 는
 * 방식이다. 그 주소와 값을 정해 EP 의 MSI capability 에 프로그램하는 것이
 * 이 콜백의 일이고, 실제 프로그래밍은 PCI 코어가 한다.
 *
 * 주소는 세대별 고정값이다(platform->msi_address). AST2600 은 0x1e77005c,
 * AST2700 은 0x000000f0 이며, 그 값의 근거가 되는 Aspeed 문서는 이 트리에
 * 없다 — 코드가 그것을 "EP 가 쓰면 이 컨트롤러가 인터럽트로 바꾸는 주소"
 * 로 쓴다는 것만 알 수 있다.
 * address_hi 를 0 으로 두므로 32비트 주소만 쓴다.
 *
 * 데이터는 hwirq 를 그대로 쓴다. 즉 EP 가 벡터 번호를 값으로 써 보내고,
 * 컨트롤러가 그 번호에 해당하는 상태 비트를 세우며,
 * aspeed_pcie_intr_handler() 가 그 비트로 어느 벡터인지 알아낸다.
 *
 * 실행 컨텍스트: MSI 설정 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (PCI MSI 코어) → irq_chip.irq_compose_msi_msg → [이 함수]
 */
static void aspeed_irq_compose_msi_msg(struct irq_data *data,
				       struct msi_msg *msg)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(data);

	/* [한국어] 32비트 주소만 쓰므로 상위는 0 이다 */
	msg->address_hi = 0;
	/* [한국어] 세대별 고정 주소. EP 가 여기 쓰면 컨트롤러가 인터럽트로 바꾼다.
	 * 그 값의 근거가 되는 Aspeed 문서는 이 트리에 없다 */
	msg->address_lo = pcie->platform->msi_address;
	/* [한국어] 벡터 번호를 데이터로 쓴다. EP 가 그 번호를 써 보내면 컨트롤러가
	 * 해당 상태 비트를 세우고, 핸들러가 그 비트로 어느 벡터인지 알아낸다 */
	msg->data = data->hwirq;
}

/* [한국어] MSI 계층의 바닥 irq_chip. compose 콜백 하나만 구현한다 —
 * mask/unmask 는 상위 계층(irq-msi-lib)이 맡는다 */
static struct irq_chip aspeed_msi_bottom_irq_chip = {
	/* [한국어] /proc/interrupts 에 보이는 이름 */
	.name = "ASPEED MSI",
	.irq_compose_msi_msg = aspeed_irq_compose_msi_msg,
};

/* [한국어]
 * aspeed_irq_msi_domain_alloc - MSI 벡터를 비트맵에서 떼어 준다
 *
 * @domain: MSI 도메인.  @virq: 배정된 첫 가상 IRQ 번호
 * @nr_irqs: 요청한 벡터 개수.  @args: 쓰지 않는다
 * @return: 0 = 성공, -ENOSPC = 남은 벡터가 없음
 *
 * irq_domain_ops 의 .alloc 콜백이다. 장치가 MSI 를 요청하면 불린다.
 *
 * 이 컨트롤러는 64개 벡터를 하드웨어로 갖고 있고, 그것을 비트맵으로
 * 관리한다. bitmap_find_free_region() 이 연속된 자리를 찾아 주는데,
 * 2의 거듭제곱 크기 단위로만 찾으므로 get_count_order(nr_irqs) 로
 * 요청 개수를 그 지수로 바꿔 넘긴다.
 * 연속이어야 하는 이유는 MSI(MSI-X 가 아닌) 규격이 그렇게 요구하기
 * 때문이다 — 여러 벡터를 쓰는 장치는 시작 번호 하나만 받고 나머지는
 * 연속이라고 가정한다.
 *
 * 찾은 뒤 vector 마다 irq_domain_set_info() 로 hwirq 와 chip 과 핸들러를
 * 건다. handle_simple_irq 를 쓰는 것은 MSI 가 에지 트리거라 mask/ack 가
 * 필요 없기 때문이다.
 *
 * guard(mutex)(&pcie->lock) 이 비트맵을 보호한다. 여러 장치가 동시에
 * MSI 를 요청할 수 있고, 찾기와 표시하기가 원자적이어야 한다.
 * guard() 는 범위를 벗어날 때 자동으로 풀어 주는 커널 관용구라
 * 실패 경로마다 unlock 을 적을 필요가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   (PCI MSI 코어) → irq_domain_ops.alloc → [이 함수]
 *     → bitmap_find_free_region() → irq_domain_set_info()
 */
static int aspeed_irq_msi_domain_alloc(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs,
				       void *args)
{
	struct aspeed_pcie *pcie = domain->host_data;
	/* [한국어] 비트맵에서 찾은 시작 벡터 번호 */
	int bit;
	/* [한국어] 벡터마다 도는 인덱스 */
	int i;

	guard(mutex)(&pcie->lock);

	/* [한국어] 연속된 빈자리를 찾는다. 2의 거듭제곱 크기 단위로만 찾으므로
	 * get_count_order 로 요청 개수를 그 지수로 바꿔 넘긴다.
	 * 연속이어야 하는 것은 MSI 규격이 시작 번호 하나만 주고 나머지는
	 * 연속이라고 가정하기 때문이다 */
	bit = bitmap_find_free_region(pcie->msi_irq_in_use, MAX_MSI_HOST_IRQS,
				      get_count_order(nr_irqs));

	/* [한국어] 남은 자리가 없다 */
	if (bit < 0)
		return -ENOSPC;

	/* [한국어] 찾은 자리부터 벡터마다 */
	for (i = 0; i < nr_irqs; i++) {
		/* [한국어] hwirq 와 chip 과 핸들러를 건다. handle_simple_irq 를 쓰는 것은
		 * MSI 가 에지 트리거라 mask/ack 가 필요 없기 때문이다 */
		irq_domain_set_info(domain, virq + i, bit + i,
				    &aspeed_msi_bottom_irq_chip,
				    domain->host_data, handle_simple_irq, NULL,
				    NULL);
	}

	return 0;
}

/* [한국어]
 * aspeed_irq_msi_domain_free - MSI 벡터를 비트맵에 돌려준다
 *
 * @domain: MSI 도메인.  @virq: 해제할 첫 가상 IRQ 번호
 * @nr_irqs: 해제할 벡터 개수.  @return: 없음
 *
 * aspeed_irq_msi_domain_alloc() 의 짝이다. irq_data 에서 hwirq 를 되찾아
 * 그 자리부터 비트맵을 비운다.
 *
 * get_count_order(nr_irqs) 로 같은 지수를 계산해 넘기는 것이 중요하다 —
 * 할당 때와 같은 크기 단위로 풀어야 비트맵이 어긋나지 않는다.
 *
 * chip_data 에서 struct aspeed_pcie 를 되찾는 경로가 alloc 과 다르다.
 * alloc 은 domain->host_data 를 쓰고 free 는 irq_data 를 거치는데,
 * 두 값이 같은 포인터를 가리킨다(irq_domain_set_info 의 chip_data 인자로
 * domain->host_data 를 넘겼기 때문이다).
 *
 * 역시 guard(mutex) 로 비트맵을 보호한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   (PCI MSI 코어) → irq_domain_ops.free → [이 함수]
 *     → bitmap_release_region()
 */
static void aspeed_irq_msi_domain_free(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *data = irq_domain_get_irq_data(domain, virq);
	/* [한국어] irq_data 를 거쳐 컨트롤러를 되찾는다. alloc 이 domain->host_data 를
	 * 쓰는 것과 경로가 다르지만 같은 포인터를 가리킨다 */
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(data);

	guard(mutex)(&pcie->lock);

	/* [한국어] 할당 때와 같은 크기 단위로 비운다. 어긋나면 비트맵이 망가진다 */
	bitmap_release_region(pcie->msi_irq_in_use, data->hwirq,
			      get_count_order(nr_irqs));
}

/* [한국어] MSI 도메인의 연산 표 */
static const struct irq_domain_ops aspeed_msi_domain_ops = {
	/* [한국어] 벡터 할당 콜백 */
	.alloc = aspeed_irq_msi_domain_alloc,
	.free = aspeed_irq_msi_domain_free,
};

#define ASPEED_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				  MSI_FLAG_USE_DEF_CHIP_OPS	| \
				  MSI_FLAG_NO_AFFINITY)

#define ASPEED_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK	| \
				   MSI_FLAG_MULTI_PCI_MSI	| \
				   MSI_FLAG_PCI_MSIX)

/* [한국어] 이 컨트롤러가 어떤 MSI 기능을 지원하는지 상위 계층에 알린다.
 * irq-msi-lib 가 이것을 보고 장치별 MSI 도메인을 만들어 준다 */
static const struct msi_parent_ops aspeed_msi_parent_ops = {
	/* [한국어] 반드시 필요한 플래그 묶음 */
	.required_flags		= ASPEED_MSI_FLAGS_REQUIRED,
	.supported_flags	= ASPEED_MSI_FLAGS_SUPPORTED,
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	.chip_flags		= MSI_CHIP_FLAG_SET_ACK,
	.prefix			= "ASPEED-",
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};

/* [한국어]
 * aspeed_pcie_msi_init - MSI 하드웨어를 켜고 부모 도메인을 만든다
 *
 * @pcie: 이 컨트롤러.  @return: 0 = 성공, -ENOMEM = 도메인 생성 실패
 *
 * 두 부분이다.
 *
 *   1) 하드웨어 준비. 활성화 레지스터 두 개(32비트씩 = 64 벡터)에 ~0 을
 *      써서 전부 켜고, 상태 레지스터 두 개에도 ~0 을 써서 묵은 상태를
 *      지운다(RW1C 로 보이는 사용 방식). + 0x04 로 두 번째 레지스터에
 *      접근하는 것으로 보아 두 레지스터가 인접해 있다.
 *      벡터를 전부 켜 두고 개별 제어는 소프트웨어 비트맵으로 하는 방식이다.
 *
 *   2) msi_create_parent_irq_domain() 으로 MSI 부모 도메인을 만든다.
 *      "부모" 도메인이라는 것은 그 아래에 장치별 MSI 도메인이 계층으로
 *      붙는다는 뜻이고, 그 계층 구성은 커널의 irq-msi-lib 가 맡는다.
 *      aspeed_msi_parent_ops 가 어떤 MSI 기능을 지원하는지 알린다.
 *
 * struct irq_domain_info 를 함수 중간에 선언하는 점이 눈에 띈다. C99
 * 이후 허용되는 형태이며, 커널도 근래에는 허용한다.
 *
 * 에러 경로: 도메인 생성 실패는 dev_err_probe 로 로그와 함께 -ENOMEM 을
 * 돌려준다. 하드웨어 레지스터 쓰기는 되돌리지 않는데, 실패하면 probe
 * 전체가 중단되므로 문제가 되지 않는다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   aspeed_pcie_init_irq_domain() → [이 함수]
 *     → msi_create_parent_irq_domain()
 */
static int aspeed_pcie_msi_init(struct aspeed_pcie *pcie)
{
	writel(~0, pcie->reg + pcie->platform->reg_msi_en);
	/* [한국어] 두 번째 32 벡터의 활성화 레지스터. 4바이트 간격으로 인접해 있다 */
	writel(~0, pcie->reg + pcie->platform->reg_msi_en + 0x04);
	/* [한국어] 첫 32 벡터의 묵은 상태를 지운다 */
	writel(~0, pcie->reg + pcie->platform->reg_msi_sts);
	/* [한국어] 두 번째 32 벡터의 묵은 상태를 지운다. 켜기 전에 지워야 켜자마자
	 * 가짜 인터럽트가 들어오지 않는다 */
	writel(~0, pcie->reg + pcie->platform->reg_msi_sts + 0x04);

	/* [한국어] 도메인 생성에 넘길 정보. C99 이후 허용되는 함수 중간 선언이다 */
	struct irq_domain_info info = {
		/* [한국어] 장치 트리 노드를 도메인에 연결한다 */
		.fwnode		= dev_fwnode(pcie->dev),
		.ops		= &aspeed_msi_domain_ops,
		.host_data	= pcie,
		.size		= MAX_MSI_HOST_IRQS,
	};

	/* [한국어] MSI 부모 도메인을 만든다. "부모" 는 그 아래에 장치별 MSI 도메인이
	 * 계층으로 붙는다는 뜻이고, 그 구성은 irq-msi-lib 가 맡는다 */
	pcie->msi_domain = msi_create_parent_irq_domain(&info,
							&aspeed_msi_parent_ops);
	/* [한국어] 생성 실패 */
	if (!pcie->msi_domain)
		/* [한국어] 로그와 함께 -ENOMEM 을 돌려준다. 위에서 켠 레지스터는 되돌리지
		 * 않는데, 실패하면 probe 전체가 중단되므로 문제가 되지 않는다 */
		return dev_err_probe(pcie->dev, -ENOMEM,
				     "failed to create MSI domain\n");

	return 0;
}

/* [한국어]
 * aspeed_pcie_msi_free - MSI 도메인을 없앤다
 *
 * @pcie: 이 컨트롤러.  @return: 없음
 *
 * NULL 검사를 먼저 하므로 도메인이 만들어지지 않은 상태에서도 안전하다 —
 * aspeed_pcie_init_irq_domain() 의 실패 경로가 그것에 기댄다.
 *
 * 없앤 뒤 포인터를 NULL 로 되돌린다. 두 번 불려도 안전해지며, 실제로
 * 그럴 수 있다 — 실패 경로에서 한 번, devm 정리에서 또 한 번 불릴 수
 * 있는 구조다.
 *
 * MSI 하드웨어를 끄지는 않는다(활성화 레지스터를 되돌리지 않는다).
 * 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: probe 실패 경로 또는 devm 정리의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   aspeed_pcie_irq_domain_free() → [이 함수] → irq_domain_remove()
 */
static void aspeed_pcie_msi_free(struct aspeed_pcie *pcie)
{
	if (pcie->msi_domain) {
		irq_domain_remove(pcie->msi_domain);
		/* [한국어] 두 번 불려도 안전하도록 NULL 로 되돌린다. 실패 경로에서 한 번,
		 * devm 정리에서 또 한 번 불릴 수 있는 구조다 */
		pcie->msi_domain = NULL;
	}
}

/* [한국어]
 * aspeed_pcie_irq_domain_free - INTx 와 MSI 도메인을 모두 없앤다
 *
 * @d: devm 액션에 넘긴 struct aspeed_pcie.  @return: 없음
 *
 * 두 곳에서 불린다 — aspeed_pcie_init_irq_domain() 의 실패 경로(손으로)와
 * devm 액션(장치 제거 시 자동으로). 그래서 두 번 불려도 안전해야 하고,
 * 그것을 NULL 검사와 NULL 대입으로 보장한다.
 *
 * INTx 도메인을 먼저 없애고 MSI 를 나중에 없앤다. 두 도메인이 서로
 * 의존하지 않으므로 순서에 특별한 의미는 없어 보인다.
 *
 * 인자 타입이 void * 인 것은 devm_add_action_or_reset() 의 콜백 서명을
 * 맞추기 위해서다.
 *
 * 실행 컨텍스트: probe 실패 경로 또는 장치 제거 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   aspeed_pcie_init_irq_domain()의 실패 경로 또는 (devm 정리) → [이 함수]
 *     → irq_domain_remove() → aspeed_pcie_msi_free()
 */
static void aspeed_pcie_irq_domain_free(void *d)
{
	struct aspeed_pcie *pcie = d;

	/* [한국어] 만들어진 적이 있으면 */
	if (pcie->intx_domain) {
		irq_domain_remove(pcie->intx_domain);
		/* [한국어] 역시 두 번 해제를 막기 위해 NULL 로 되돌린다 */
		pcie->intx_domain = NULL;
	}
	aspeed_pcie_msi_free(pcie);
}

/* [한국어]
 * aspeed_pcie_init_irq_domain - INTx 와 MSI 도메인을 만든다
 *
 * @pcie: 이 컨트롤러.  @return: 0 = 성공, 음수 errno = 실패
 *
 * 셋을 순서대로 한다.
 *
 *   1) INTx 선형 도메인(PCI_NUM_INTX = 4)을 만든다. 장치 트리 노드를
 *      함께 넘겨, 하위 장치의 interrupt-map 이 이 컨트롤러를 가리킬 때
 *      이 도메인을 찾을 수 있게 한다.
 *
 *   2) INTx 하드웨어를 초기 상태로 둔다 — 활성화 레지스터에 0 을 써서
 *      전부 끄고, 상태 레지스터에 ~0 을 써서 묵은 상태를 지운다.
 *      MSI 와 정반대인 점에 주의한다. MSI 는 전부 켜 두고 소프트웨어
 *      비트맵으로 관리하는 반면, INTx 는 전부 꺼 두고 장치가 요청할 때
 *      irq_chip 의 unmask 가 하나씩 켠다.
 *
 *   3) aspeed_pcie_msi_init() 으로 MSI 쪽을 준비한다.
 *
 * 에러 경로: 어느 단계가 실패하든 err 라벨로 가서
 * aspeed_pcie_irq_domain_free() 로 지금까지 만든 것을 되돌린다.
 * 그 함수가 NULL 을 안전하게 다루므로 부분적으로 만들어진 상태에서도
 * 동작한다.
 *
 * 이 함수 자체는 devm 액션을 등록하지 않는다. 호출자
 * aspeed_pcie_probe() 가 성공 직후에 등록한다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   aspeed_pcie_probe() → [이 함수]
 *     → irq_domain_add_linear() → aspeed_pcie_msi_init()
 */
static int aspeed_pcie_init_irq_domain(struct aspeed_pcie *pcie)
{
	int ret;

	/* [한국어] INTx 선형 도메인을 만든다. 장치 트리 노드를 함께 넘겨,
	 * 하위 장치의 interrupt-map 이 이 컨트롤러를 가리킬 때 찾을 수 있게 한다 */
	pcie->intx_domain = irq_domain_add_linear(pcie->dev->of_node,
						  PCI_NUM_INTX,
						  &aspeed_intx_domain_ops,
						  pcie);
	/* [한국어] 생성 실패 */
	if (!pcie->intx_domain) {
		/* [한국어] 로그와 함께 -ENOMEM 을 준비하고 아래 정리 경로로 간다 */
		ret = dev_err_probe(pcie->dev, -ENOMEM,
				    "failed to get INTx IRQ domain\n");
		goto err;
	}

	/* [한국어] INTx 를 전부 끈다. MSI 와 정반대인 점에 주의 — MSI 는 전부 켜 두고
	 * 소프트웨어 비트맵으로 관리하지만, INTx 는 꺼 두고 장치가 요청할 때
	 * irq_chip 의 unmask 가 하나씩 켠다 */
	writel(0, pcie->reg + pcie->platform->reg_intx_en);
	/* [한국어] 묵은 INTx 상태를 지운다 */
	writel(~0, pcie->reg + pcie->platform->reg_intx_sts);

	/* [한국어] MSI 쪽을 준비한다 */
	ret = aspeed_pcie_msi_init(pcie);
	/* [한국어] 실패하면 아래 정리 경로로 */
	if (ret)
		goto err;

	return 0;
err:
	aspeed_pcie_irq_domain_free(pcie);
	return ret;
}

/* [한국어]
 * aspeed_pcie_port_init - 포트 하나의 링크를 올린다
 *
 * @port: 이 포트의 클록·PHY·리셋.  @return: 0 = 성공, 음수 errno = 실패
 *
 * PCIe 링크가 서려면 클록이 돌고 PHY 가 설정되고 PERST 가 풀려야 한다.
 * 그 셋을 순서대로 한다.
 *
 *   1) 클록을 켠다.
 *   2) PHY 를 초기화한다.
 *   3) PHY 를 PCIe RC 모드로 세운다. 같은 PHY 가 EP 모드로도 쓰일 수
 *      있으므로 어느 쪽인지 알려 줘야 한다.
 *   4) PERST 를 푼다(deassert). 이 순간부터 상대 장치가 리셋에서 벗어나
 *      링크 훈련을 시작한다.
 *   5) PCIE_RESET_CONFIG_WAIT_MS 만큼 잠든다. PCIe 스펙이 정한, 리셋을
 *      푼 뒤 config 접근을 시작하기까지의 대기 시간이다. 이 대기가 없으면
 *      아직 준비되지 않은 장치를 찔러 열거가 실패한다.
 *
 * msleep 을 쓰므로 잠든다. probe 경로라 문제가 없다.
 *
 * 에러 경로: 각 단계가 실패하면 dev_err_probe 로 로그와 함께 그 오류를
 * 올린다. 앞 단계에서 켠 클록이나 초기화한 PHY 를 되돌리지 않는데,
 * 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트. 잠든다.
 *
 * 호출 체인:
 *   aspeed_pcie_parse_dt() → aspeed_pcie_parse_port() → [이 함수]
 *     → clk_prepare_enable() → phy_init() → phy_set_mode_ext()
 *     → reset_control_deassert() → msleep()
 */
static int aspeed_pcie_port_init(struct aspeed_pcie_port *port)
{
	struct aspeed_pcie *pcie = port->pcie;
	/* [한국어] 로그 출력 대상 */
	struct device *dev = pcie->dev;
	/* [한국어] 각 단계의 결과 */
	int ret;

	/* [한국어] 포트 클록을 켠다. 링크가 서려면 클록이 먼저 돌아야 한다 */
	ret = clk_prepare_enable(port->clk);
	/* [한국어] 실패 */
	if (ret)
		/* [한국어] 어느 슬롯에서 무엇이 실패했는지 남긴다. dev_err_probe 는
		 * -EPROBE_DEFER 일 때 로그를 줄여 준다 */
		return dev_err_probe(dev, ret,
				     "failed to set clock for slot (%d)\n",
				     port->slot);

	/* [한국어] PHY 를 초기화한다 */
	ret = phy_init(port->phy);
	/* [한국어] 실패 */
	if (ret)
		/* [한국어] 슬롯 번호와 함께 남긴다 */
		return dev_err_probe(dev, ret,
				     "failed to init phy pcie for slot (%d)\n",
				     port->slot);

	/* [한국어] PHY 를 PCIe RC 모드로 세운다. 같은 PHY 가 EP 모드로도 쓰일 수 있어
	 * 어느 쪽인지 알려 줘야 한다 */
	ret = phy_set_mode_ext(port->phy, PHY_MODE_PCIE, PHY_MODE_PCIE_RC);
	/* [한국어] 실패 */
	if (ret)
		/* [한국어] 슬롯 번호와 함께 남긴다 */
		return dev_err_probe(dev, ret,
				     "failed to set phy mode for slot (%d)\n",
				     port->slot);

	reset_control_deassert(port->perst);
	msleep(PCIE_RESET_CONFIG_WAIT_MS);

	return 0;
}

/* [한국어]
 * aspeed_host_reset - H2X 컨트롤러를 리셋한다
 *
 * @pcie: 이 컨트롤러.  @return: 없음
 *
 * 리셋을 걸고, 정해진 시간만큼 유지한 뒤, 푼다. 하드웨어가 리셋 신호를
 * 인식하려면 일정 시간 유지되어야 하므로 그 사이에 대기가 들어간다.
 *
 * mdelay 를 쓰는 점이 눈에 띈다 — msleep 과 달리 CPU 를 붙들고 도는
 * 바쁜 대기다. 10밀리초 동안 CPU 를 점유한다. probe 경로라 문제가 크지
 * 않지만, 잠들 수 있는 문맥이므로 msleep 도 가능했을 자리다. 코드는
 * 고치지 않고 이 관찰만 적어 둔다.
 *
 * 두 세대의 setup 함수가 모두 이것을 부른다. 다만 부르는 시점이 다르다 —
 * AST2600 은 AHBC 를 열기 전에, AST2700 은 SCU 설정을 마친 뒤다.
 * 그 차이의 근거가 되는 하드웨어 문서는 이 트리에 없다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   aspeed_ast2600_setup() / aspeed_ast2700_setup() → [이 함수]
 */
static void aspeed_host_reset(struct aspeed_pcie *pcie)
{
	reset_control_assert(pcie->h2xrst);
	mdelay(ASPEED_RESET_RC_WAIT_MS);
	reset_control_deassert(pcie->h2xrst);
}

/* [한국어]
 * aspeed_pcie_map_ranges - 첫 메모리 창을 찾아 주소 변환을 세운다
 *
 * @pcie: 이 컨트롤러.  @return: 없음
 *
 * 장치 트리의 ranges 속성이 "SoC 의 어느 주소 범위가 PCI 의 어느 범위에
 * 대응하는가" 를 정하고, PCI 코어가 그것을 host->windows 리스트로 만들어
 * 둔다. 이 함수는 그중 첫 메모리 창을 찾아 하드웨어에 알린다.
 *
 * pci_addr = window->res->start - window->offset 이 변환의 핵심이다.
 * res->start 는 CPU/AHB 쪽에서 본 주소이고 offset 은 두 주소 공간의 차이라,
 * 빼면 PCI 버스 쪽에서 본 주소가 나온다.
 *
 * 찾은 첫 창 하나만 처리하고 break 한다. 이 하드웨어의 변환 레지스터가
 * 한 벌뿐이라 여러 창을 다룰 수 없기 때문으로 보이며, 근거가 되는 문서는
 * 이 트리에 없다.
 *
 * 실제 레지스터 쓰기는 세대별 콜백에 위임한다 — 두 세대의 변환 레지스터
 * 배치가 완전히 다르기 때문이다.
 *
 * 메모리 창이 하나도 없으면 아무 일도 하지 않는다. 반환값이 없어 호출자가
 * 그것을 알 방법도 없다. 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   aspeed_pcie_probe() → [이 함수] → (platform 콜백) pcie_map_ranges
 */
static void aspeed_pcie_map_ranges(struct aspeed_pcie *pcie)
{
	struct pci_host_bridge *bridge = pcie->host;
	/* [한국어] windows 리스트를 훑을 반복자 */
	struct resource_entry *window;

	/* [한국어] 장치 트리의 ranges 로부터 PCI 코어가 만들어 둔 창 목록 */
	resource_list_for_each_entry(window, &bridge->windows) {
		/* [한국어] 이 창의 PCI 쪽 시작 주소 */
		u64 pci_addr;

		/* [한국어] 메모리 창이 아니면 건너뛴다 */
		if (resource_type(window->res) != IORESOURCE_MEM)
			continue;

		/* [한국어] CPU/AHB 쪽 주소에서 두 주소 공간의 차이를 빼면 PCI 쪽 주소가 나온다 */
		pci_addr = window->res->start - window->offset;
		/* [한국어] 세대별 콜백에 위임한다. 두 세대의 변환 레지스터 배치가 다르기 때문이다 */
		pcie->platform->pcie_map_ranges(pcie, pci_addr);
		break;
	}
}

/* [한국어]
 * aspeed_ast2600_pcie_map_ranges - AST2600 의 AHB-PCI 주소 변환을 세운다
 *
 * @pcie: 이 컨트롤러.  @pci_addr: PCI 버스 쪽에서 본 창의 시작 주소
 * @return: 없음
 *
 * 64비트 주소를 상하위로 쪼개 세 레지스터에 나눠 쓴다.
 *
 *   하위 32비트는 먼저 16비트 오른쪽으로 민다. 그 뒤
 *   ASPEED_AHB_REMAP_LO_ADDR 이 GENMASK(15,4) 로 걸러 내므로, 결과적으로
 *   원래 주소의 비트 31-20 이 레지스터의 비트 15-4 자리에 놓인다.
 *   즉 1MB 단위 정렬을 전제한 배치다.
 *
 *   ASPEED_AHB_MASK_LO_ADDR(0xe00) 이 같은 레지스터의 비트 31-20 에
 *   0xe00 을 넣는다. 마스크 값이 하드코딩된 상수인데, 그것이 어떤 크기의
 *   창을 뜻하는지는 Aspeed 문서 소관이라 이 트리에서 확인하지 못했다.
 *
 *   상위 32비트는 CONFIG1 에 그대로 쓰고, CONFIG2 에는 마스크로 ~0 을 쓴다.
 *
 * 세 매크로(REMAP_LO/MASK_LO/REMAP_HI/MASK_HI)의 정의가 서로 다른 형태인
 * 점에 주의한다 — 어떤 것은 GENMASK 로 걸러 내고 어떤 것은 FIELD_PREP 으로
 * 밀어 넣고 어떤 것은 값을 그대로 쓴다. 각 레지스터의 필드 배치가 다르기
 * 때문으로 보이며, 근거가 되는 문서는 이 트리에 없다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   aspeed_pcie_map_ranges() → (platform 콜백) → [이 함수] → writel()
 */
static void aspeed_ast2600_pcie_map_ranges(struct aspeed_pcie *pcie,
					  u64 pci_addr)
{
	u32 pci_addr_lo = pci_addr & GENMASK(31, 0);
	/* [한국어] 상위 32비트를 꺼낸다 */
	u32 pci_addr_hi = (pci_addr >> 32) & GENMASK(31, 0);

	/* [한국어] 하위를 16비트 오른쪽으로 민다. 아래 매크로가 GENMASK(15,4) 로 걸러
	 * 내므로, 결과적으로 원래 주소의 비트 31-20 이 레지스터의 비트 15-4 에
	 * 놓인다 — 1MB 단위 정렬을 전제한 배치다 */
	pci_addr_lo >>= 16;
	/* [한국어] 밀어 둔 하위 주소와 마스크를 한 레지스터에 함께 쓴다.
	 * 마스크 값 0xe00 이 어떤 크기의 창을 뜻하는지는 Aspeed 문서 소관이라
	 * 이 트리에서 확인하지 못했다 */
	writel(ASPEED_AHB_REMAP_LO_ADDR(pci_addr_lo) |
	       ASPEED_AHB_MASK_LO_ADDR(0xe00),
	       pcie->reg + ASPEED_H2X_AHB_ADDR_CONFIG0);
	writel(ASPEED_AHB_REMAP_HI_ADDR(pci_addr_hi),
	       pcie->reg + ASPEED_H2X_AHB_ADDR_CONFIG1);
	/* [한국어] 상위 마스크에는 ~0 을 쓴다 */
	writel(ASPEED_AHB_MASK_HI_ADDR(~0),
	       pcie->reg + ASPEED_H2X_AHB_ADDR_CONFIG2);
}

/* [한국어]
 * aspeed_ast2600_setup - AST2600 의 하드웨어 초기화
 *
 * @pdev: 이 플랫폼 장치.  @return: 0 = 성공, 음수 errno = 실패
 *
 * platform->setup 콜백이며, probe 가 부르는 세대별 초기화의 전부다.
 *
 * 절차:
 *   1) 장치 트리의 "aspeed,ahbc" phandle 로 AHBC 레지스터 맵을 얻는다.
 *      AHBC 는 이 컨트롤러 밖의 블록이라 자기 레지스터 창으로 접근할 수
 *      없고, syscon 을 거쳐 regmap 으로 다룬다.
 *
 *   2) H2X 컨트롤러를 리셋한다.
 *
 *   3) AHBC 를 열어 RC 메모리 접근을 켠다. 순서가 셋이다 —
 *      매직 키(ASPEED_AHBC_UNLOCK_KEY = 0xaeed1a03)를 써서 잠금을 풀고,
 *      주소 매핑 레지스터에서 RC 메모리 활성 비트를 세우고,
 *      다시 키 레지스터에 ASPEED_AHBC_UNLOCK(0x01)을 써서 닫는다.
 *      매직 키 방식은 실수로 이 레지스터를 건드리는 것을 막는 흔한 설계다.
 *      마지막 쓰기가 "잠그기" 인지 다른 뜻인지는 상수 이름만으로는
 *      분명하지 않고, 근거가 되는 문서는 이 트리에 없다.
 *
 *   4) H2X 브리지를 켠다.
 *
 *   5) 장치 제어 레지스터에 여러 비트를 한 번에 세운다 — RX DMA,
 *      선형 RX, MSI 선택과 활성화, RX TLP 클리어 대기, RC RX 활성화,
 *      RC 활성화. 읽고-OR-쓰기가 아니라 통째로 쓰는 점에 주의한다.
 *      초기화 시점이라 다른 비트를 보존할 이유가 없기 때문으로 보인다.
 *
 *   6) TX 태그 개수를 설정한다(ASPEED_RC_TLP_TX_TAG_NUM = 0x28).
 *
 *   7) 이 세대의 pci_ops 두 벌을 브리지에 건다. 루트 버스용과 하위
 *      버스용을 나누는 것이 Type 0/Type 1 을 가르는 방식이다.
 *
 * 에러 경로: AHBC 맵 획득 실패만 확인하고 나머지 레지스터 쓰기는 실패를
 * 알릴 방법이 없다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트. 리셋 대기로 잠든다.
 *
 * 호출 체인:
 *   aspeed_pcie_probe() → (platform 콜백) setup → [이 함수]
 *     → syscon_regmap_lookup_by_phandle() → aspeed_host_reset() → writel()
 */
static int aspeed_ast2600_setup(struct platform_device *pdev)
{
	struct aspeed_pcie *pcie = platform_get_drvdata(pdev);
	/* [한국어] 로그 출력 대상 */
	struct device *dev = pcie->dev;

	/* [한국어] 장치 트리의 phandle 로 AHBC 레지스터 맵을 얻는다.
	 * AHBC 는 이 컨트롤러 밖의 블록이라 syscon 을 거친다 */
	pcie->ahbc = syscon_regmap_lookup_by_phandle(dev->of_node,
						     "aspeed,ahbc");
	/* [한국어] 획득 실패 */
	if (IS_ERR(pcie->ahbc))
		/* [한국어] 로그와 함께 오류를 올린다 */
		return dev_err_probe(dev, PTR_ERR(pcie->ahbc),
				     "failed to map ahbc base\n");

	aspeed_host_reset(pcie);

	/* [한국어] 매직 키를 써서 AHBC 의 잠금을 푼다. 실수로 이 레지스터를 건드리는
	 * 것을 막는 흔한 설계다 */
	regmap_write(pcie->ahbc, ASPEED_AHBC_KEY, ASPEED_AHBC_UNLOCK_KEY);
	/* [한국어] 주소 매핑 레지스터에서 RC 메모리 접근 비트를 세운다 */
	regmap_update_bits(pcie->ahbc, ASPEED_AHBC_ADDR_MAPPING,
			   ASPEED_PCIE_RC_MEMORY_EN, ASPEED_PCIE_RC_MEMORY_EN);
	/* [한국어] 다시 키 레지스터에 다른 값을 써서 닫는다. 그 값이 "잠그기" 를
	 * 뜻하는지 다른 뜻인지는 상수 이름만으로는 분명하지 않고, 근거가 되는
	 * 문서는 이 트리에 없다 */
	regmap_write(pcie->ahbc, ASPEED_AHBC_KEY, ASPEED_AHBC_UNLOCK);

	/* [한국어] H2X 브리지를 켠다 */
	writel(ASPEED_H2X_BRIDGE_EN, pcie->reg + ASPEED_H2X_CTRL);

	/* [한국어] 장치 제어 레지스터에 여러 비트를 한 번에 세운다. 읽고-OR-쓰기가
	 * 아니라 통째로 쓰는 점에 주의 — 초기화 시점이라 보존할 값이 없다 */
	writel(ASPEED_PCIE_RX_DMA_EN | ASPEED_PCIE_RX_LINEAR |
	       /* [한국어] MSI 선택과 활성화 */
	       ASPEED_PCIE_RX_MSI_SEL | ASPEED_PCIE_RX_MSI_EN |
	       /* [한국어] RX TLP 클리어 대기와 RC RX 활성화 */
	       ASPEED_PCIE_WAIT_RX_TLP_CLR | ASPEED_PCIE_RC_RX_ENABLE |
	       ASPEED_PCIE_RC_ENABLE,
	       pcie->reg + ASPEED_H2X_DEV_CTRL);

	/* [한국어] TX 태그 개수를 설정한다. 그 값의 의미는 Aspeed 문서 소관이라
	 * 이 트리에서 확인하지 못했다 */
	writel(ASPEED_RC_TLP_TX_TAG_NUM, pcie->reg + ASPEED_H2X_DEV_TX_TAG);

	/* [한국어] 루트 버스용 연산 표를 건다 */
	pcie->host->ops = &aspeed_ast2600_pcie_ops;
	/* [한국어] 하위 버스용 연산 표를 건다. 이 두 줄이 Type 0/Type 1 을 가르는 장치다 */
	pcie->host->child_ops = &aspeed_ast2600_pcie_child_ops;

	return 0;
}

/* [한국어]
 * aspeed_ast2700_pcie_map_ranges - AST2700 의 PCI 주소 변환을 세운다
 *
 * @pcie: 이 컨트롤러.  @pci_addr: PCI 버스 쪽에서 본 창의 시작 주소
 * @return: 없음
 *
 * AST2600 판보다 훨씬 단순하다. 두 레지스터에 하위/상위를 나눠 쓴다.
 *
 *   하위 — ASPEED_REMAP_PCI_ADDR_31_12 가 GENMASK(31,12) 로 걸러 내므로
 *     4KB 단위 정렬을 전제한다. AST2600 이 1MB 단위인 것과 다르다.
 *   상위 — ASPEED_REMAP_PCI_ADDR_63_32 가 32비트 오른쪽으로 밀어 상위
 *     절반을 꺼낸다.
 *
 * 마스크 레지스터를 쓰지 않는다. 창 크기를 다른 곳에서 정하거나
 * 하드웨어가 다르게 처리하는 것으로 보이며, 근거가 되는 문서는 이 트리에
 * 없다.
 *
 * prefetchable 영역의 상위 주소는 이 함수가 아니라
 * aspeed_ast2700_setup() 이 따로 세운다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   aspeed_pcie_map_ranges() → (platform 콜백) → [이 함수] → writel()
 */
static void aspeed_ast2700_pcie_map_ranges(struct aspeed_pcie *pcie,
					  u64 pci_addr)
{
	writel(ASPEED_REMAP_PCI_ADDR_31_12(pci_addr),
		pcie->reg + ASPEED_H2X_REMAP_PCI_ADDR_LO);
	writel(ASPEED_REMAP_PCI_ADDR_63_32(pci_addr),
		pcie->reg + ASPEED_H2X_REMAP_PCI_ADDR_HI);
}

/* [한국어]
 * aspeed_ast2700_setup - AST2700 의 하드웨어 초기화
 *
 * @pdev: 이 플랫폼 장치.  @return: 0 = 성공, 음수 errno = 실패
 *
 * AST2600 판과 같은 자리를 맡지만 다루는 블록이 다르다.
 *
 * 절차:
 *   1) 장치 트리의 "aspeed,pciecfg" phandle 로 SCU 레지스터 맵을 얻는다.
 *      AST2600 이 AHBC 를 쓰는 자리에 이쪽은 SCU 를 쓴다.
 *
 *   2) SCU_60 에서 다섯 경로 비트를 한꺼번에 켠다 — E2M(EP to Memory),
 *      H2XS/H2XD/H2XX(Host to PCIe 의 세 갈래), 그리고 상류 메모리 접근.
 *      regmap_update_bits 로 마스크와 값을 같게 넘겨 그 비트들만 세운다.
 *      각 경로의 정확한 의미는 Aspeed 문서 소관이라 확인하지 못했다.
 *
 *   3) SCU_64 에 DMA 디코딩 범위를 쓴다. RC0 과 RC1 각각에 base 0,
 *      limit 0xff 를 넣어 전 범위를 연다. 두 RC 가 있는 SoC 라는 뜻이다.
 *
 *   4) SCU_70 에 ASPEED_DISABLE_EP_FUNC(0)를 써서 EP 기능을 끈다.
 *      이 SoC 는 RC 로도 EP 로도 동작할 수 있는데, 여기서는 RC 로 쓴다.
 *
 *   5) H2X 컨트롤러를 리셋한다. AST2600 이 SCU/AHBC 설정보다 먼저
 *      리셋하는 것과 순서가 반대다.
 *
 *   6) H2X 제어 레지스터를 0 으로 만든 뒤 브리지와 direct 모드를 켠다.
 *      0 을 먼저 쓰는 것은 확실한 초기 상태를 만들기 위한 것으로 보인다.
 *
 *   7) 원문 영어 주석 "Prepare for 64-bit BAR pref" 대로, prefetchable
 *      영역의 상위 32비트 주소를 0x3 으로 세운다. 64비트 BAR 를 쓰는
 *      장치를 위한 준비다.
 *
 *   8) 이 세대의 pci_ops 두 벌을 걸고, clear_msi_twice 를 세운다.
 *      그 플래그가 aspeed_pcie_intr_handler() 의 MSI 상태 지우기를 두 번
 *      하게 만든다(그쪽 원문 주석이 workaround 라고 밝힌다).
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트. 리셋 대기로 잠든다.
 *
 * 호출 체인:
 *   aspeed_pcie_probe() → (platform 콜백) setup → [이 함수]
 *     → syscon_regmap_lookup_by_phandle() → regmap_update_bits()
 *     → aspeed_host_reset() → writel()
 */
static int aspeed_ast2700_setup(struct platform_device *pdev)
{
	struct aspeed_pcie *pcie = platform_get_drvdata(pdev);
	/* [한국어] 로그 출력 대상 */
	struct device *dev = pcie->dev;

	/* [한국어] 장치 트리의 phandle 로 SCU 레지스터 맵을 얻는다.
	 * AST2600 이 AHBC 를 쓰는 자리에 이쪽은 SCU 를 쓴다 */
	pcie->cfg = syscon_regmap_lookup_by_phandle(dev->of_node,
						    "aspeed,pciecfg");
	/* [한국어] 획득 실패 */
	if (IS_ERR(pcie->cfg))
		/* [한국어] 로그와 함께 오류를 올린다 */
		return dev_err_probe(dev, PTR_ERR(pcie->cfg),
				     "failed to map pciecfg base\n");

	/* [한국어] SCU_60 에서 다섯 경로 비트를 한꺼번에 켠다. 마스크와 값을 같게 넘겨
	 * 그 비트들만 세운다 */
	regmap_update_bits(pcie->cfg, ASPEED_SCU_60,
			   ASPEED_RC_E2M_PATH_EN | ASPEED_RC_H2XS_PATH_EN |
			   /* [한국어] Host to PCIe 의 세 갈래 */
			   ASPEED_RC_H2XD_PATH_EN | ASPEED_RC_H2XX_PATH_EN |
			   ASPEED_RC_UPSTREAM_MEM_EN,
			   ASPEED_RC_E2M_PATH_EN | ASPEED_RC_H2XS_PATH_EN |
			   /* [한국어] 값 쪽에도 같은 비트 묶음을 넘긴다 */
			   ASPEED_RC_H2XD_PATH_EN | ASPEED_RC_H2XX_PATH_EN |
			   ASPEED_RC_UPSTREAM_MEM_EN);
	/* [한국어] DMA 디코딩 범위를 쓴다 */
	regmap_write(pcie->cfg, ASPEED_SCU_64,
		     ASPEED_RC0_DECODE_DMA_BASE(0) |
		     /* [한국어] RC0 의 상한을 0xff 로 열어 전 범위를 허용한다 */
		     ASPEED_RC0_DECODE_DMA_LIMIT(0xff) |
		     /* [한국어] RC1 의 하한. 이 SoC 에 RC 가 둘이라는 뜻이다 */
		     ASPEED_RC1_DECODE_DMA_BASE(0) |
		     ASPEED_RC1_DECODE_DMA_LIMIT(0xff));
	/* [한국어] EP 기능을 끈다. 이 SoC 는 RC 로도 EP 로도 동작할 수 있는데
	 * 여기서는 RC 로 쓴다 */
	regmap_write(pcie->cfg, ASPEED_SCU_70, ASPEED_DISABLE_EP_FUNC);

	aspeed_host_reset(pcie);

	/* [한국어] 제어 레지스터를 0 으로 만들어 확실한 초기 상태를 만든 뒤 */
	writel(0, pcie->reg + ASPEED_H2X_CTRL);
	/* [한국어] 브리지와 direct 모드를 켠다 */
	writel(ASPEED_H2X_BRIDGE_EN | ASPEED_H2X_BRIDGE_DIRECT_EN,
	       pcie->reg + ASPEED_H2X_CTRL);

	/* Prepare for 64-bit BAR pref */
	writel(ASPEED_REMAP_PREF_ADDR_63_32(0x3),
	       pcie->reg + ASPEED_H2X_REMAP_PREF_ADDR);

	/* [한국어] 루트 버스용 연산 표를 건다 */
	pcie->host->ops = &aspeed_ast2700_pcie_ops;
	/* [한국어] 하위 버스용 연산 표를 건다 */
	pcie->host->child_ops = &aspeed_ast2700_pcie_child_ops;
	/* [한국어] MSI 상태를 두 번 지우게 한다. 바로 그 처리가
	 * aspeed_pcie_intr_handler() 안에 있고 원문 주석이 workaround 라고 밝힌다 */
	pcie->clear_msi_twice = true;

	return 0;
}

/* [한국어]
 * aspeed_pcie_reset_release - PERST 리셋 컨트롤 참조를 놓는다
 *
 * @d: devm 액션에 넘긴 struct reset_control.  @return: 없음
 *
 * aspeed_pcie_parse_port() 가 of_reset_control_get_exclusive() 로 얻은
 * 참조를 devm 액션으로 되돌리기 위한 함수다.
 *
 * 그 획득 함수가 devm 판이 아니라서 손으로 놓아야 하는데, 실패 경로마다
 * 적는 대신 devm_add_action_or_reset() 으로 등록해 두는 방식을 썼다.
 * 그러면 이후 어느 단계가 실패해도, 그리고 장치가 제거될 때도 devm 이
 * 알아서 부른다.
 *
 * NULL 검사가 있지만 호출자가 NULL 을 넘길 일은 없다 — 그 앞에서
 * IS_ERR 로 이미 걸렀기 때문이다. 방어적인 처리다.
 *
 * 인자 타입이 void * 인 것은 devm 콜백 서명을 맞추기 위해서다.
 *
 * 실행 컨텍스트: probe 실패 경로 또는 장치 제거 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (devm 정리) → [이 함수] → reset_control_put()
 */
static void aspeed_pcie_reset_release(void *d)
{
	struct reset_control *perst = d;

	/* [한국어] 호출자가 NULL 을 넘길 일은 없다 — 그 앞에서 IS_ERR 로 이미 걸렀다.
	 * 방어적인 처리다 */
	if (!perst)
		return;

	reset_control_put(perst);
}

/* [한국어]
 * aspeed_pcie_parse_port - 장치 트리의 포트 노드 하나를 자원과 함께 잡는다
 *
 * @pcie: 이 컨트롤러.  @node: 포트를 나타내는 장치 트리 자식 노드
 * @slot: 그 포트의 슬롯 번호
 * @return: 0 = 성공, 음수 errno = 실패
 *
 * 포트 하나에 필요한 자원 셋(클록, PHY, PERST 리셋)을 모으고 링크를
 * 올린다.
 *
 *   1) struct aspeed_pcie_port 를 devm 으로 잡는다.
 *   2) 클록을 얻는다. devm_get_clk_from_child 는 자식 노드에서 찾는
 *      판이라, 포트마다 다른 클록을 가질 수 있다.
 *   3) PHY 를 얻는다.
 *   4) PERST 리셋을 얻는다. exclusive 판이라 다른 드라이버와 공유하지
 *      않으며, devm 판이 아니라서 곧바로 devm 액션을 등록해 되돌릴
 *      길을 만들어 둔다.
 *   5) reset_control_assert() 로 PERST 를 건다. 아직 푸는 것이 아니라
 *      거는 것이 요점 — 링크를 올리기 전에 상대 장치를 확실히 리셋
 *      상태로 두려는 것이다. 푸는 것은 아래 port_init 이 한다.
 *   6) 포트를 컨트롤러의 리스트에 매단다.
 *   7) aspeed_pcie_port_init() 으로 클록을 켜고 PHY 를 세우고 PERST 를
 *      풀어 링크를 올린다.
 *
 * 에러 경로: 각 단계가 dev_err_probe 로 로그와 함께 오류를 올린다.
 * 그 함수는 -EPROBE_DEFER 일 때 로그를 줄여 주므로, 클록이나 PHY 드라이버가
 * 아직 준비되지 않아 재시도하는 흔한 경우에 로그가 지저분해지지 않는다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트. 잠든다.
 *
 * 호출 체인:
 *   aspeed_pcie_parse_dt() → [이 함수]
 *     → devm_get_clk_from_child() → devm_of_phy_get()
 *     → of_reset_control_get_exclusive() → aspeed_pcie_port_init()
 */
static int aspeed_pcie_parse_port(struct aspeed_pcie *pcie,
				  struct device_node *node,
				  int slot)
{
	struct aspeed_pcie_port *port;
	/* [한국어] devm 할당과 로그의 기준 */
	struct device *dev = pcie->dev;
	/* [한국어] 각 단계의 결과 */
	int ret;

	/* [한국어] 포트 상태를 devm 으로 잡는다 */
	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	/* [한국어] 메모리 부족 */
	if (!port)
		return -ENOMEM;

	/* [한국어] 자식 노드에서 클록을 얻는다. 포트마다 다른 클록을 가질 수 있다 */
	port->clk = devm_get_clk_from_child(dev, node, NULL);
	/* [한국어] 획득 실패 */
	if (IS_ERR(port->clk))
		/* [한국어] 슬롯 번호와 함께 남긴다 */
		return dev_err_probe(dev, PTR_ERR(port->clk),
				     "failed to get pcie%d clock\n", slot);

	/* [한국어] PHY 를 얻는다 */
	port->phy = devm_of_phy_get(dev, node, NULL);
	/* [한국어] 획득 실패 */
	if (IS_ERR(port->phy))
		/* [한국어] 슬롯 번호와 함께 남긴다 */
		return dev_err_probe(dev, PTR_ERR(port->phy),
				     "failed to get phy pcie%d\n", slot);

	/* [한국어] PERST 리셋을 얻는다. exclusive 판이라 다른 드라이버와 공유하지 않는다 */
	port->perst = of_reset_control_get_exclusive(node, "perst");
	/* [한국어] 획득 실패 */
	if (IS_ERR(port->perst))
		/* [한국어] 슬롯 번호와 함께 남긴다 */
		return dev_err_probe(dev, PTR_ERR(port->perst),
				     "failed to get pcie%d reset control\n",
				     slot);
	/* [한국어] devm 판이 아니라서 되돌릴 액션을 곧바로 등록해 둔다.
	 * 그러면 이후 어느 단계가 실패해도, 장치가 제거될 때도 devm 이 부른다 */
	ret = devm_add_action_or_reset(dev, aspeed_pcie_reset_release,
				       port->perst);
	/* [한국어] 등록 실패 */
	if (ret)
		return ret;
	reset_control_assert(port->perst);

	/* [한국어] 슬롯 번호를 기억해 둔다. 오류 메시지에만 쓰인다 */
	port->slot = slot;
	/* [한국어] 컨트롤러로 되돌아갈 포인터 */
	port->pcie = pcie;

	INIT_LIST_HEAD(&port->list);
	/* [한국어] 컨트롤러의 포트 리스트에 매단다 */
	list_add_tail(&port->list, &pcie->ports);

	/* [한국어] 클록을 켜고 PHY 를 세우고 PERST 를 풀어 링크를 올린다 */
	ret = aspeed_pcie_port_init(port);
	/* [한국어] 실패 */
	if (ret)
		return ret;

	return 0;
}

/* [한국어]
 * aspeed_pcie_parse_dt - 장치 트리의 포트 노드들을 모두 훑는다
 *
 * @pcie: 이 컨트롤러.  @return: 0 = 성공, 음수 errno = 실패
 *
 * 컨트롤러 노드의 자식 중 device_type 이 "pci" 인 것들이 포트다. 그것을
 * 하나씩 찾아 aspeed_pcie_parse_port() 로 넘긴다.
 *
 * for_each_available_child_of_node_scoped 는 두 가지를 해 준다 —
 * status 가 "disabled" 인 노드를 건너뛰고, 반복자의 참조 계수를 범위
 * 종료 시 자동으로 놓는다. 후자 덕분에 중간에 return 해도 노드 참조가
 * 새지 않는다.
 *
 * device_type 검사가 필요한 이유는 컨트롤러 노드 아래에 포트가 아닌
 * 자식(예: 인터럽트 컨트롤러 노드)이 있을 수 있어서다. 읽기에 실패하거나
 * "pci" 가 아니면 조용히 건너뛴다.
 *
 * of_pci_get_devfn() 이 장치 트리의 reg 속성에서 devfn 을 뽑아 주고,
 * PCI_SLOT 으로 슬롯 번호를 얻는다.
 *
 * 포트를 하나도 못 찾으면 -ENODEV 로 실패한다. 포트 없는 RC 는 쓸모가
 * 없기 때문이다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   aspeed_pcie_probe() → [이 함수]
 *     → of_pci_get_devfn() → aspeed_pcie_parse_port()
 */
static int aspeed_pcie_parse_dt(struct aspeed_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] 컨트롤러의 장치 트리 노드. 포트들이 그 자식으로 있다 */
	struct device_node *node = dev->of_node;
	/* [한국어] 각 단계의 결과 */
	int ret;

	/* [한국어] status 가 disabled 인 노드를 건너뛰고, 반복자의 참조를 범위 종료 시
	 * 자동으로 놓는 매크로다. 덕분에 중간에 return 해도 노드 참조가 새지 않는다 */
	for_each_available_child_of_node_scoped(node, child) {
		/* [한국어] 이 포트의 슬롯 번호 */
		int slot;
		/* [한국어] device_type 속성 값 */
		const char *type;

		/* [한국어] 이 자식이 포트인지 확인한다 */
		ret = of_property_read_string(child, "device_type", &type);
		/* [한국어] 읽기에 실패했거나 "pci" 가 아니면 포트가 아니다 — 컨트롤러 노드 아래에
		 * 인터럽트 컨트롤러 같은 다른 자식이 있을 수 있다 */
		if (ret || strcmp(type, "pci"))
			continue;

		/* [한국어] 장치 트리의 reg 속성에서 devfn 을 뽑는다 */
		ret = of_pci_get_devfn(child);
		/* [한국어] 파싱 실패 */
		if (ret < 0)
			/* [한국어] 로그와 함께 오류를 올린다 */
			return dev_err_probe(dev, ret,
					     "failed to parse devfn\n");

		/* [한국어] devfn 에서 슬롯 번호만 꺼낸다 */
		slot = PCI_SLOT(ret);

		/* [한국어] 이 포트의 자원을 잡고 링크를 올린다 */
		ret = aspeed_pcie_parse_port(pcie, child, slot);
		/* [한국어] 하나라도 실패하면 probe 전체를 중단한다 */
		if (ret)
			return ret;
	}

	/* [한국어] 포트를 하나도 못 찾았다 */
	if (list_empty(&pcie->ports))
		/* [한국어] 포트 없는 RC 는 쓸모가 없으므로 -ENODEV 로 실패한다 */
		return dev_err_probe(dev, -ENODEV,
				     "No PCIe port found in DT\n");

	return 0;
}

/* [한국어]
 * aspeed_pcie_probe - 플랫폼 장치를 잡아 PCIe RC 를 세운다
 *
 * @pdev: 장치 트리가 만든 플랫폼 장치
 * @return: 0 = 성공, 음수 errno = 실패
 *
 * 이 드라이버의 진입점이다. 순서에 각각 이유가 있다.
 *
 *   1) of_device_get_match_data() 로 세대별 platform 구조체를 고른다.
 *      이후의 모든 세대 분기(setup 콜백, map_ranges 콜백, 레지스터
 *      오프셋, MSI 주소)가 이 한 줄에서 갈린다. 없으면 -ENODEV.
 *
 *   2) devm_pci_alloc_host_bridge() 로 브리지와 이 드라이버의 상태를
 *      한 번에 잡는다. struct aspeed_pcie 를 따로 할당하지 않는 이유다.
 *
 *   3) 기본 필드를 채우고 포트 리스트를 초기화한다.
 *
 *   4) 원문 영어 주석대로, config 명령이 TLP type 0 인지 1 인지 정하는
 *      데 쓸 루트 버스 번호를 windows 리스트의 IORESOURCE_BUS 항목에서
 *      얻는다. AST2700 의 child_config 가 (root_bus_nr + 1) 과 비교하는
 *      그 값이다. AST2600 경로는 이 값을 쓰지 않는다.
 *      항목이 없으면 0 이 그대로 남는다.
 *
 *   5) 레지스터 창을 매핑하고, H2X 리셋 컨트롤을 얻고, MSI 비트맵을
 *      보호할 뮤텍스를 초기화한다.
 *
 *   6) platform->setup() — 세대별 하드웨어 초기화. 여기서 pci_ops 두
 *      벌이 브리지에 걸린다.
 *
 *   7) aspeed_pcie_map_ranges() 로 주소 변환을 세운다. setup 다음이어야
 *      하는 이유는 그 안에서 세대별 콜백을 부르기 때문이다.
 *
 *   8) aspeed_pcie_parse_dt() 로 포트마다 링크를 올린다.
 *
 *   9) host->sysdata 를 세운다. config 콜백들이 bus->sysdata 로 이 값을
 *      되찾는다.
 *
 *   10) IRQ 도메인을 만들고, 그 정리를 devm 액션으로 등록한다.
 *       등록이 도메인 생성 다음인 것이 중요하다 — 만들기 전에 등록하면
 *       정리할 것이 없는데 부르게 되고, 그 반대면 실패 시 새어 나간다.
 *
 *   11) IRQ 를 얻어 devm_request_irq(IRQF_SHARED) 로 핸들러를 건다.
 *       IRQF_SHARED 는 이 인터럽트 선을 다른 장치와 공유할 수 있다는 뜻이다.
 *
 *   12) pci_host_probe() 로 PCI 코어에 넘긴다. 그때부터 코어가 버스를
 *       열거하며 config 콜백들을 부르기 시작한다.
 *
 * 에러 경로: 거의 모든 단계가 dev_err_probe 로 로그와 함께 오류를 올리고,
 * 자원 해제는 devm 이 맡는다. 그래서 goto 사슬 없이 곧바로 return 하는
 * 형태가 가능하다.
 *
 * remove 함수가 없는 점에 주의한다. 아래 드라이버 서술자가
 * suppress_bind_attrs 를 세워 sysfs 를 통한 언바인드를 막고
 * builtin_platform_driver 로 등록하므로, 이 드라이버는 한 번 붙으면
 * 떨어지지 않는다.
 *
 * 실행 컨텍스트: 드라이버 바인드 경로의 프로세스 컨텍스트. 잠든다.
 *
 * 호출 체인:
 *   (플랫폼 버스 바인드) → [이 함수]
 *     → (platform) setup → aspeed_pcie_map_ranges() → aspeed_pcie_parse_dt()
 *     → aspeed_pcie_init_irq_domain() → devm_request_irq() → pci_host_probe()
 */
static int aspeed_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	/* [한국어] PCI 코어에 넘길 호스트 브리지 */
	struct pci_host_bridge *host;
	/* [한국어] 이 컨트롤러의 상태. 위 브리지의 private 영역에 얹힌다 */
	struct aspeed_pcie *pcie;
	/* [한국어] windows 리스트에서 버스 범위 항목을 찾을 반복자 */
	struct resource_entry *entry;
	/* [한국어] of_device_get_match_data 가 돌려줄 세대별 묶음 */
	const struct aspeed_pcie_rc_platform *md;
	/* [한국어] IRQ 번호와 각 단계의 결과 */
	int irq, ret;

	/* [한국어] 어느 compatible 로 매칭됐는지에 따라 세대별 묶음을 얻는다.
	 * 이후의 모든 세대 분기가 이 한 줄에서 갈린다 */
	md = of_device_get_match_data(dev);
	/* [한국어] 지원하지 않는 하드웨어 */
	if (!md)
		return -ENODEV;

	/* [한국어] 브리지와 이 드라이버의 상태를 한 번에 잡는다 */
	host = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	/* [한국어] 메모리 부족 */
	if (!host)
		return -ENOMEM;

	/* [한국어] 방금 잡은 private 영역을 얻는다 */
	pcie = pci_host_bridge_priv(host);
	/* [한국어] 로그와 devm 의 기준으로 쓴다 */
	pcie->dev = dev;
	/* [한국어] TLP 태그를 0 에서 시작한다 */
	pcie->tx_tag = 0;
	/* [한국어] 이후 세대별 setup 이 platform_get_drvdata 로 되찾는다 */
	platform_set_drvdata(pdev, pcie);

	/* [한국어] 세대별 콜백과 상수 묶음 */
	pcie->platform = md;
	/* [한국어] setup 콜백이 host->ops 를 걸 수 있도록 기억해 둔다 */
	pcie->host = host;
	INIT_LIST_HEAD(&pcie->ports);

	/* Get root bus num for cfg command to decide tlp type 0 or type 1 */
	entry = resource_list_first_type(&host->windows, IORESOURCE_BUS);
	/* [한국어] 버스 범위 항목이 있으면 */
	if (entry)
		/* [한국어] 그 시작 번호를 루트 버스 번호로 삼는다. AST2700 의 child_config 가
		 * (root_bus_nr + 1) 과 비교해 Type 0/1 을 가르는 그 값이다 */
		pcie->root_bus_nr = entry->res->start;

	/* [한국어] "H2X" 컨트롤러 레지스터 창을 매핑한다 */
	pcie->reg = devm_platform_ioremap_resource(pdev, 0);
	/* [한국어] 매핑 실패 */
	if (IS_ERR(pcie->reg))
		/* [한국어] 오류 포인터에서 errno 를 꺼내 올린다 */
		return PTR_ERR(pcie->reg);

	/* [한국어] H2X 리셋 컨트롤을 얻는다 */
	pcie->h2xrst = devm_reset_control_get_exclusive(dev, "h2x");
	/* [한국어] 획득 실패 */
	if (IS_ERR(pcie->h2xrst))
		/* [한국어] 로그와 함께 오류를 올린다 */
		return dev_err_probe(dev, PTR_ERR(pcie->h2xrst),
				     "failed to get h2x reset\n");

	/* [한국어] MSI 비트맵을 보호할 뮤텍스를 초기화한다. devm 판이라 해제가 자동이다 */
	ret = devm_mutex_init(dev, &pcie->lock);
	/* [한국어] 초기화 실패 */
	if (ret)
		/* [한국어] 로그와 함께 오류를 올린다 */
		return dev_err_probe(dev, ret, "failed to init mutex\n");

	/* [한국어] 세대별 하드웨어 초기화. 여기서 pci_ops 두 벌이 브리지에 걸린다 */
	ret = pcie->platform->setup(pdev);
	/* [한국어] 실패 */
	if (ret)
		/* [한국어] 로그와 함께 오류를 올린다 */
		return dev_err_probe(dev, ret, "failed to setup PCIe RC\n");

	aspeed_pcie_map_ranges(pcie);

	/* [한국어] 포트마다 클록·PHY·PERST 를 잡고 링크를 올린다 */
	ret = aspeed_pcie_parse_dt(pcie);
	/* [한국어] 실패 */
	if (ret)
		return ret;

	/* [한국어] config 콜백들이 bus->sysdata 로 이 값을 되찾는다 */
	host->sysdata = pcie;

	/* [한국어] INTx 와 MSI 도메인을 만든다 */
	ret = aspeed_pcie_init_irq_domain(pcie);
	/* [한국어] 실패 */
	if (ret)
		return ret;

	/* [한국어] 정리를 devm 액션으로 등록한다. 도메인 생성 다음이어야 한다 —
	 * 만들기 전에 등록하면 정리할 것이 없는데 부르게 되고, 그 반대면
	 * 실패 시 새어 나간다 */
	ret = devm_add_action_or_reset(dev, aspeed_pcie_irq_domain_free, pcie);
	/* [한국어] 등록 실패 */
	if (ret)
		return ret;

	/* [한국어] 이 컨트롤러의 IRQ 를 얻는다 */
	irq = platform_get_irq(pdev, 0);
	/* [한국어] 획득 실패 */
	if (irq < 0)
		/* [한국어] 그대로 올린다 */
		return irq;

	/* [한국어] 인터럽트 핸들러를 건다. IRQF_SHARED 는 이 인터럽트 선을 다른 장치와
	 * 공유할 수 있다는 뜻이고, devm 판이라 해제가 자동이다 */
	ret = devm_request_irq(dev, irq, aspeed_pcie_intr_handler, IRQF_SHARED,
			       dev_name(dev), pcie);
	/* [한국어] 등록 실패 */
	if (ret)
		return ret;

	/* [한국어] PCI 코어에 넘긴다. 그때부터 코어가 버스를 열거하며 config 콜백들을
	 * 부르기 시작한다 */
	return pci_host_probe(host);
}

/* [한국어] AST2600 의 세대별 묶음 */
static const struct aspeed_pcie_rc_platform pcie_rc_ast2600 = {
	/* [한국어] 이 세대의 초기화 함수 */
	.setup = aspeed_ast2600_setup,
	.pcie_map_ranges = aspeed_ast2600_pcie_map_ranges,
	.reg_intx_en = 0xc4,
	.reg_intx_sts = 0xc8,
	.reg_msi_en = 0xe0,
	.reg_msi_sts = 0xe8,
	.msi_address = 0x1e77005c,
};

/* [한국어] AST2700 의 세대별 묶음. 레지스터 오프셋과 MSI 주소가 모두 다르다 */
static const struct aspeed_pcie_rc_platform pcie_rc_ast2700 = {
	/* [한국어] 이 세대의 초기화 함수 */
	.setup = aspeed_ast2700_setup,
	.pcie_map_ranges = aspeed_ast2700_pcie_map_ranges,
	.reg_intx_en = 0x40,
	.reg_intx_sts = 0x48,
	.reg_msi_en = 0x50,
	.reg_msi_sts = 0x58,
	.msi_address = 0x000000f0,
};

/* [한국어] 장치 트리 compatible 문자열과 세대별 묶음의 대응표 */
static const struct of_device_id aspeed_pcie_of_match[] = {
	/* [한국어] AST2600 하드웨어 */
	{ .compatible = "aspeed,ast2600-pcie", .data = &pcie_rc_ast2600 },
	{ .compatible = "aspeed,ast2700-pcie", .data = &pcie_rc_ast2700 },
	{}
};

/* [한국어] 플랫폼 드라이버 서술자. remove 함수가 없는 점에 주의 —
 * 아래 suppress_bind_attrs 와 builtin_platform_driver 로 인해
 * 이 드라이버는 한 번 붙으면 떨어지지 않는다 */
static struct platform_driver aspeed_pcie_driver = {
	/* [한국어] 드라이버 속성 묶음 */
	.driver = {
		/* [한국어] sysfs 와 로그에 보이는 이름 */
		.name = "aspeed-pcie",
		.of_match_table = aspeed_pcie_of_match,
		.suppress_bind_attrs = true,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = aspeed_pcie_probe,
};

builtin_platform_driver(aspeed_pcie_driver);

MODULE_AUTHOR("Jacky Chou <jacky_chou@aspeedtech.com>");
MODULE_DESCRIPTION("ASPEED PCIe Root Complex");
MODULE_LICENSE("GPL");
