// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2014 Hauke Mehrtens <hauke@hauke-m.de>
 * Copyright (C) 2015 Broadcom Corporation
 */

/*
 * [한국어 설명] iProc PCIe 호스트 컨트롤러 공용 코어 (pcie-iproc.c)
 *
 * === 파일의 역할 ===
 * Broadcom iProc 계열 SoC 의 PCIe 호스트 컨트롤러를 실제로 다루는 본체다.
 * 레지스터 초기화, PERST 제어, 링크 학습 대기, config 공간 접근, 바깥·안쪽
 * 주소 창 설정, MSI 배선까지 하드웨어를 만지는 코드가 전부 여기 모여 있다.
 *
 * 이 파일이 홀로 쓰이는 일은 없다. 컨트롤러를 어느 버스에 붙일지는 두 개의
 * 결합 드라이버가 정한다 — DT 기반이면 pcie-iproc-platform.c, BCMA 버스면
 * pcie-iproc-bcma.c 다. 그 둘이 struct iproc_pcie 를 채워 이 파일의
 * iproc_pcie_setup() 에 넘기면 나머지를 이 파일이 맡는다. MSI 는 다시
 * 선택적 부속인 pcie-iproc-msi.c 로 넘어간다.
 *
 * 이 파일의 복잡성 대부분은 "변종 흡수" 에서 나온다. 같은 드라이버가 다섯
 * 가지 하드웨어 변종(enum iproc_pcie_type)을 다루는데, 변종마다 레지스터
 * 오프셋이 다르고 지원하는 창 개수와 크기도 다르며 심지어 config 읽기
 * 방식까지 다르다. 그 차이를 세 가지 장치로 흡수한다.
 *   1) 레지스터 오프셋 표 — 변종별 u16 배열 다섯 개(iproc_pcie_reg_paxb 등).
 *      enum iproc_pcie_reg 를 인덱스로 쓰며, 그 변종에 없는 레지스터는
 *      값이 0 이거나 IPROC_PCIE_REG_INVALID 로 표시된다.
 *   2) 창 능력 표 — paxb_ob_map / paxb_v2_ob_map / paxb_v2_ib_map.
 *      각 창이 지원하는 크기 목록을 담아, 요청한 크기를 표에서 찾는 방식으로
 *      하드웨어 제약을 검사한다.
 *   3) bool 플래그 모음 — iproc_pcie_rev_init() 이 변종에 따라
 *      iproc_cfg_read, has_apb_err_disable, ep_is_internal, need_msi_steer
 *      같은 스위치를 세우고, 이후 코드는 변종 이름 대신 그 플래그만 본다.
 *
 * PAXB 와 PAXC 의 구분이 특히 중요하다. pcie-iproc.h 의 enum 주석이 밝히듯
 * PAXB 는 바깥 엔드포인트를 붙이는 보통의 루트 컴플렉스 래퍼이고,
 * PAXC 는 SoC 내부에 에뮬레이트된 엔드포인트 전용 래퍼다. 그래서 PAXC 는
 * 주소 창도 링크 학습도 없고, 대신 잘못 만들어진 capability 목록을
 * 소프트웨어로 고쳐 주는 코드가 따로 붙는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 코어 → pci_host_bridge → [이 파일] → 결합 드라이버 → 버스 → 하드웨어
 * 순으로 놓이며, 이 파일은 위아래 양쪽에서 불린다.
 *
 * 정방향(설정):
 *   iproc_bcma_pcie_probe() [pcie-iproc-bcma.c:340] 또는
 *   iproc_pltfm_pcie_probe() [pcie-iproc-platform.c:247]
 *     -> [이 파일] iproc_pcie_setup():1445
 *        -> iproc_pcie_rev_init():1379   변종별 표와 플래그를 고른다
 *        -> iproc_pcie_check_link():766  PERST 를 풀고 링크 학습을 기다린다
 *        -> iproc_pcie_map_ranges():1008     바깥 창
 *        -> iproc_pcie_map_dma_ranges():1175 안쪽 창
 *        -> iproc_pcie_msi_enable():1339 -> iproc_msi_init():1367
 *        -> pci_host_probe()             PCI 코어에 열거를 넘긴다
 *
 * 역방향(열거 중 되불림): PCI 코어가 config 를 읽고 쓸 때마다
 *   iproc_pcie_config_read32() / _write32() 가 불린다. PAXB v2 는
 *   iproc_pcie_config_read() 를 거쳐 RRS 재시도 루프를 돈다.
 *
 * 해제: iproc_pcie_remove():1528 -> iproc_pcie_msi_disable():1374
 *   -> iproc_msi_exit():1376. 전원 차단은 iproc_pcie_shutdown():757 이
 *   PERST 를 다시 assert 한다.
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. setup 은 링크 학습(최대 100ms)과
 * config 재시도(최대 500ms)에서 잠들고, config 접근 콜백은 PCI 코어가
 * pci_lock 을 쥔 채 부르지만 이 파일에는 잠드는 경로가 그 안에 없다.
 * 인터럽트 핸들러는 이 파일에 없다 — MSI 수신은 pcie-iproc-msi.c 몫이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/probe.c 의 pci_host_probe() 가 열거를 시작하고,
 *   drivers/pci/access.c 가 config 접근 콜백을 되부른다.
 * 옆쪽(결합): pcie-iproc-platform.c 와 pcie-iproc-bcma.c 가
 *   iproc_pcie_setup()(:1526 에서 EXPORT), iproc_pcie_remove()(:1540),
 *   iproc_pcie_shutdown()(:764, GPL 한정) 세 심볼만 쓴다.
 * 옆쪽(MSI): pcie-iproc-msi.c 의 iproc_msi_init() / iproc_msi_exit().
 *   CONFIG_PCIE_IPROC_MSI 가 꺼지면 pcie-iproc.h:121,126 의 인라인 더미가
 *   -ENODEV 를 돌려주고 이 파일은 그 실패를 무시하고 진행한다.
 * 아래쪽: PHY 프레임워크(phy_init/phy_power_on), 클럭, DT(of_property_read_*,
 *   of_parse_phandle), GICv3 ITS 상수(GITS_TRANSLATER), pci-ecam.h.
 * 공유 상태: struct iproc_pcie(pcie-iproc.h) 하나가 전부다. 결합 드라이버가
 *   dev/type/base/base_addr/map_irq 를 채워 넘기고, 이 파일이 reg_offsets 와
 *   각종 플래그·창 표를 채워 완성한다. 인스턴스마다 독립적이라 이 파일에는
 *   전역 가변 상태가 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * iproc_pcie_setup()        : 진입점. 변종 판정부터 버스 스캔까지 전 과정.
 * iproc_pcie_rev_init()     : 다섯 변종의 차이를 표와 플래그로 흡수한다.
 *                             이 파일을 읽을 때 가장 먼저 볼 함수다.
 * iproc_pcie_check_link()   : PERST 해제 후 링크 학습을 기다리고, 링크가
 *                             섰는지 vendor ID 까지 읽어 확인한다.
 * iproc_pcie_cfg_retry()    : PAXB v2 의 RRS(Request Retry Status) 응답을
 *                             최대 500ms 동안 재시도한다.
 * iproc_pcie_fix_cap()      : PAXC 의 망가진 capability 목록을 읽기 시점에
 *                             소프트웨어로 고쳐 준다.
 * iproc_pcie_setup_ob() / _ib() : 요청한 크기를 변종 능력 표에서 찾아
 *                             바깥·안쪽 창을 연다.
 * iproc_pcie_msi_steer()    : MSI 쓰기를 어느 주소로 보낼지 배선한다.
 *                             내부 MSI 컨트롤러와 GICv3 ITS 두 갈래가 있다.
 * struct iproc_pcie_ob_map / _ib_map : 창 하나가 지원하는 크기 목록.
 * enum iproc_pcie_reg       : 레지스터 이름. 변종별 오프셋 표의 인덱스다.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 이 드라이버는 버스를 만드는 쪽이고 NVMe 는 그 위에 열거되는 장치라
 * 계층이 다르다. iProc 보드에 NVMe SSD 를 붙이면 iproc_pcie_setup() 이
 * 링크를 세우고 pci_host_probe() 가 그것을 발견하겠지만, 그 경로에
 * NVMe 에 특화된 처리는 한 줄도 없다 — 모든 PCIe 장치에 똑같이 적용된다.
 */

/* [한국어] kernel.h — ARRAY_SIZE, ALIGN_DOWN, IS_ALIGNED, ilog2 등 기본 매크로.
 * 창 크기·정렬 계산이 많은 파일이라 이 매크로들이 자주 쓰인다. */
#include <linux/kernel.h>
/* [한국어] pci.h — struct pci_bus/pci_dev, pci_ops, PCIBIOS_* 반환 코드,
 * pci_host_probe(), DECLARE_PCI_FIXUP_EARLY, PCI_EXP_* 표준 상수. */
#include <linux/pci.h>
/* [한국어] pci-ecam.h — PCIE_ECAM_OFFSET() 과 PCIE_ECAM_DEVFN(). 표준 ECAM 규칙으로
 * BDF 를 오프셋으로 바꾸는 데 쓴다. 이 컨트롤러에 ECAM 창이 있는 것은
 * 아니고, 주소 조립 규칙만 빌려 쓴다. */
#include <linux/pci-ecam.h>
/* [한국어] msi.h — MSI 관련 타입. 이 파일은 MSI 를 직접 처리하지 않고 배선만 하지만
 * pcie-iproc.h 를 통해 타입이 필요하다. */
#include <linux/msi.h>
/* [한국어] clk.h — 클럭 API. */
#include <linux/clk.h>
/* [한국어] module.h — EXPORT_SYMBOL / EXPORT_SYMBOL_GPL. 이 파일은 setup/remove/
 * shutdown 세 심볼을 결합 드라이버에 공개한다. */
#include <linux/module.h>
/* [한국어] mbus.h — Marvell 버스 정의. */
#include <linux/mbus.h>
/* [한국어] slab.h — devm_kcalloc(). 레지스터 오프셋 표 사본을 만드는 데 쓴다. */
#include <linux/slab.h>
/* [한국어] delay.h — udelay(), msleep(). PERST 지연과 config 재시도에 쓴다. */
#include <linux/delay.h>
/* [한국어] interrupt.h — 인터럽트 관련 정의. */
#include <linux/interrupt.h>
/* [한국어] irqchip/arm-gic-v3.h — GITS_TRANSLATER. PAXC v2 에서 MSI 를 GICv3 ITS 로
 * 보낼 때 그 레지스터의 오프셋이 필요하다. */
#include <linux/irqchip/arm-gic-v3.h>
/* [한국어] platform_device.h — 플랫폼 장치 정의. */
#include <linux/platform_device.h>
/* [한국어] of_address.h — of_address_to_resource(). MSI 수신 주소를 DT 에서 얻는다. */
#include <linux/of_address.h>
/* [한국어] of_irq.h — DT 인터럽트 헬퍼. */
#include <linux/of_irq.h>
/* [한국어] of_pci.h — DT PCI 헬퍼. */
#include <linux/of_pci.h>
/* [한국어] of_platform.h — of_parse_phandle(). msi-parent 를 따라간다. */
#include <linux/of_platform.h>
/* [한국어] phy/phy.h — 범용 PHY API. setup 이 phy_init/phy_power_on 을 부른다. */
#include <linux/phy/phy.h>

/* [한국어] pcie-iproc.h — struct iproc_pcie, enum iproc_pcie_type, 그리고
 * iproc_msi_init()/iproc_msi_exit() 선언(또는 그 인라인 더미). */
#include "pcie-iproc.h"

/* [한국어] PERST 소스 선택 비트의 자리(2). 아래 BIT() 로 감싸 쓴다.
 * SHIFT 상수와 비트 상수를 쌍으로 두는 것이 이 파일 전반의 관례다. */
#define EP_PERST_SOURCE_SELECT_SHIFT	2
/* [한국어] PERST 소스 선택 비트. iproc_pcie_perst_ctrl() 이 리셋을 걸 때 내린다. */
#define EP_PERST_SOURCE_SELECT		BIT(EP_PERST_SOURCE_SELECT_SHIFT)
/* [한국어] PERST 를 견디는 EP 모드 비트의 자리(1). */
#define EP_MODE_SURVIVE_PERST_SHIFT	1
/* [한국어] 그 비트. 역시 리셋을 걸 때 내린다. 엔드포인트가 PERST 를 무시하고
 * 살아남는 모드를 끄는 것으로 보이나, 정확한 의미는 이 트리에서 확인 못 함. */
#define EP_MODE_SURVIVE_PERST		BIT(EP_MODE_SURVIVE_PERST_SHIFT)
/* [한국어] 루트 컴플렉스의 리셋 출력 비트의 자리(0). */
#define RC_PCIE_RST_OUTPUT_SHIFT	0
/* [한국어] 그 비트. 이것이 실제로 PERST# 신호를 내보내는 비트다.
 * 리셋을 걸 때 내리고 풀 때 올린다 — 세 비트 중 유일하게 양방향으로 쓰인다. */
#define RC_PCIE_RST_OUTPUT		BIT(RC_PCIE_RST_OUTPUT_SHIFT)
/* [한국어] PAXC 리셋 마스크(0x7f). 이 파일 어디에서도 쓰지 않는다(전수 grep 확인). */
#define PAXC_RESET_MASK			0x7f

/* [한국어] GICv3 모드 비트의 자리(0). */
#define GIC_V3_CFG_SHIFT		0
/* [한국어] 그 비트. iproc_pcie_paxc_v2_msi_steer() 가 MSI_GIC_MODE 레지스터에 세워
 * MSI 를 외부 GICv3 ITS 로 넘길 수 있게 한다. */
#define GIC_V3_CFG			BIT(GIC_V3_CFG_SHIFT)

/* [한국어] MSI 활성 비트의 자리(0). */
#define MSI_ENABLE_CFG_SHIFT		0
/* [한국어] 그 비트. PAXC v2 의 MSI 개통 스위치이며, quirk_paxc_disable_msi_parsing()
 * 이 같은 비트를 0 으로 만들어 MSI 해석을 끈다. */
#define MSI_ENABLE_CFG			BIT(MSI_ENABLE_CFG_SHIFT)

/* [한국어] 간접 config 접근의 오프셋 마스크(0x00001ffc). 하위 2비트가 0 이라 워드
 * 정렬이 강제되고, 상위로는 8KB 범위만 표현된다. */
#define CFG_IND_ADDR_MASK		0x00001ffc

/* [한국어] config 주소에서 레지스터 번호 부분만 남기는 마스크(0x00000ffc).
 * iproc_pcie_config_read() 가 지금 읽는 것이 Vendor ID 인지 판정할 때 쓴다. */
#define CFG_ADDR_REG_NUM_MASK		0x00000ffc
/* [한국어] type 1 config 접근 표시(1). iproc_pcie_map_ep_cfg_reg() 가 4바이트 정렬로
 * 비워 둔 하위 2비트에 이 값을 얹는다. 엔드포인트 접근은 항상 type 1 이다. */
#define CFG_ADDR_CFG_TYPE_1		1

/* [한국어] INTx 활성 마스크(0xf). 하위 4비트가 INTA-INTD 에 대응하므로,
 * iproc_pcie_enable() 이 이 값을 써서 네 핀을 모두 켠다. */
#define SYS_RC_INTX_MASK		0xf

/* [한국어] PHY 링크업 비트의 자리(3). */
#define PCIE_PHYLINKUP_SHIFT		3
/* [한국어] 그 비트. iproc_pcie_check_link() 가 링크 확인의 첫 관문으로 본다. */
#define PCIE_PHYLINKUP			BIT(PCIE_PHYLINKUP_SHIFT)
/* [한국어] 데이터 링크 활성 비트의 자리(2). */
#define PCIE_DL_ACTIVE_SHIFT		2
/* [한국어] 그 비트. PHY 링크업과 함께 둘 다 서야 링크가 살아 있는 것으로 본다. */
#define PCIE_DL_ACTIVE			BIT(PCIE_DL_ACTIVE_SHIFT)

/* [한국어] APB 오류 전달 활성 비트의 자리(0). */
#define APB_ERR_EN_SHIFT		0
/* [한국어] 그 비트. iproc_pcie_apb_err_disable() 이 config 접근 전후로 내렸다 올린다.
 * Unsupported Request 가 시스템 예외로 번지는 것을 막기 위해서다. */
#define APB_ERR_EN			BIT(APB_ERR_EN_SHIFT)

/* [한국어] config 읽기 성공 상태값(0). CFG_RD_STATUS 레지스터가 이 값이면 정상이다.
 * 그 레지스터가 없는 변종에서는 읽기가 0 을 돌려주므로 자연히 이 값이 되어,
 * iproc_pcie_cfg_retry() 의 재시도 루프가 곧바로 빠져나온다. */
#define CFG_RD_SUCCESS			0
/* [한국어] Unsupported Request 상태값(1). 대상이 없다는 뜻이다. */
#define CFG_RD_UR			1
/* [한국어] RRS(Request Retry Status) 상태값(2). 엔드포인트가 아직 준비되지 않았으니
 * 다시 요청하라는 뜻이며, iproc_pcie_cfg_retry() 가 재시도할 유일한 근거다. */
#define CFG_RD_RRS			2
/* [한국어] Completer Abort 상태값(3). 이 파일에서 직접 판정에 쓰지는 않는다. */
#define CFG_RD_CA			3
/* [한국어] RRS 를 받았을 때 이 하드웨어가 데이터 대신 돌려주는 값(0xffff0001).
 * 어떤 레지스터의 진짜 값이 우연히 이것과 같으면 구분할 수 없다는 것이
 * iproc_pcie_cfg_retry() 상류 주석이 인정하는 한계다. */
#define CFG_RETRY_STATUS		0xffff0001
/* [한국어] 재시도 상한. 1us 간격이므로 500000회가 곧 500ms 다. */
#define CFG_RETRY_STATUS_TIMEOUT_US	500000 /* 500 milliseconds */

/* derive the enum index of the outbound/inbound mapping registers */
/* [한국어] 창 번호를 enum 오프셋으로 바꾼다. 2를 곱하는 것은 enum 에서 OARR 과 OMAP
 * (또는 IARR 과 IMAP)이 번갈아 배치돼 있어 창 하나가 두 칸을 차지하기 때문이다.
 * 예컨대 MAP_REG(IPROC_PCIE_OARR0, 1) 은 IPROC_PCIE_OARR1 이 된다. */
#define MAP_REG(base_reg, index)	((base_reg) + (index) * 2)

/*
 * Maximum number of outbound mapping window sizes that can be supported by any
 * OARR/OMAP mapping pair
 */
/* [한국어] 한 바깥 창이 가질 수 있는 크기 선택지의 최대 개수. 아래 paxb_v2_ob_map 의
 * 창 2·3 이 실제로 네 가지를 갖는다. */
#define MAX_NUM_OB_WINDOW_SIZES		4

/* [한국어] 바깥 창 유효 비트의 자리(0). */
#define OARR_VALID_SHIFT		0
/* [한국어] 그 비트. iproc_pcie_ob_is_valid() 가 창 사용 여부를 이것으로 판정하고,
 * iproc_pcie_ob_write() 가 창을 열 때 세운다. */
#define OARR_VALID			BIT(OARR_VALID_SHIFT)
/* [한국어] 바깥 창 크기 필드의 자리(1). 크기를 값이 아니라 지원 목록의 인덱스로
 * 표현해 유효 비트 바로 옆에 얹는다. */
#define OARR_SIZE_CFG_SHIFT		1

/*
 * Maximum number of inbound mapping region sizes that can be supported by an
 * IARR
 */
/* [한국어] 한 안쪽 영역이 가질 수 있는 크기 선택지의 최대 개수. 아래 paxb_v2_ib_map 의
 * IARR2 가 실제로 아홉 가지를 갖는다. */
#define MAX_NUM_IB_REGION_SIZES		9

/* [한국어] 안쪽 창 유효 비트의 자리(0). */
#define IMAP_VALID_SHIFT		0
/* [한국어] 그 비트. iproc_pcie_ib_write() 가 IMAP 하위 워드에 세운다.
 * 안쪽 영역의 사용 여부는 IARR 의 크기 비트로 판정하므로, 이 비트는
 * 판정이 아니라 개통에만 쓰인다. */
#define IMAP_VALID			BIT(IMAP_VALID_SHIFT)

/* [한국어] 이 컨트롤러의 전원 관리 capability 오프셋(0x48). 표준 상수가 아니라
 * 벤더 고유 배치라 직접 정의한다. iproc_pcie_fix_cap() 이 이 자리를 고친다. */
#define IPROC_PCI_PM_CAP		0x48
/* [한국어] 그 capability 워드에서 고칠 부분의 마스크(0xffff).
 * capability ID 와 다음 포인터가 들어가는 하위 16비트다. */
#define IPROC_PCI_PM_CAP_MASK		0xffff
/* [한국어] PCIe capability 오프셋(0xac). fix_cap 이 PM capability 의 다음 포인터를
 * 이 값으로 강제해 목록을 이어 붙인다. */
#define IPROC_PCI_EXP_CAP		0xac

/* [한국어] "이 변종에는 이 레지스터가 없다" 는 표시값(0xffff). iproc_pcie_rev_init() 이
 * 오프셋 표를 만들 때 값이 0 인 항목을 이것으로 바꿔 넣고,
 * iproc_pcie_reg_is_invalid() 가 그것을 판정한다. */
#define IPROC_PCIE_REG_INVALID		0xffff

/**
 * struct iproc_pcie_ob_map - iProc PCIe outbound mapping controller-specific
 * parameters
 * @window_sizes: list of supported outbound mapping window sizes in MB
 * @nr_sizes: number of supported outbound mapping window sizes
 */
/* [한국어] 바깥 창 하나가 지원하는 크기 목록. 변종별 표의 원소 타입이다. */
struct iproc_pcie_ob_map {
	/* [한국어] 지원 크기 목록(단위 MB). 오름차순으로 채워져 있고,
	 * iproc_pcie_setup_ob() 이 뒤에서부터(큰 것부터) 훑는다.
	 * 설정자: 아래 paxb_ob_map / paxb_v2_ob_map 의 정적 초기화.
	 * 읽는 자: iproc_pcie_setup_ob().
	 * 값 범위: MB 단위 크기. 쓰이지 않는 뒤쪽 칸은 0 이다.
	 * 동기화: const 정적 데이터라 필요 없다. */
	resource_size_t window_sizes[MAX_NUM_OB_WINDOW_SIZES];
	/* [한국어] 위 목록에서 실제로 유효한 개수.
	 * 설정자: 정적 초기화.
	 * 읽는 자: iproc_pcie_setup_ob() 의 루프 상한.
	 * 값 범위: 1..MAX_NUM_OB_WINDOW_SIZES(4).
	 * 동기화: const 정적 데이터. */
	unsigned int nr_sizes;
};

/* [한국어] PAXB 의 바깥 창 능력 표. 창이 둘뿐이고 각각 128/256MB 만 지원한다. */
static const struct iproc_pcie_ob_map paxb_ob_map[] = {
	{
		/* OARR0/OMAP0 */
		/* [한국어] OARR0/OMAP0 쌍이 지원하는 크기. */
		.window_sizes = { 128, 256 },
		/* [한국어] 두 가지. */
		.nr_sizes = 2,
	},
	{
		/* OARR1/OMAP1 */
		/* [한국어] OARR1/OMAP1 쌍. 0번과 능력이 같다. */
		.window_sizes = { 128, 256 },
		/* [한국어] 두 가지. */
		.nr_sizes = 2,
	},
};

/* [한국어] PAXB v2 의 바깥 창 능력 표. 창이 넷이고 뒤의 둘이 더 큰 크기를 지원한다. */
static const struct iproc_pcie_ob_map paxb_v2_ob_map[] = {
	{
		/* OARR0/OMAP0 */
		/* [한국어] OARR0/OMAP0. */
		.window_sizes = { 128, 256 },
		/* [한국어] 두 가지. */
		.nr_sizes = 2,
	},
	{
		/* OARR1/OMAP1 */
		/* [한국어] OARR1/OMAP1. */
		.window_sizes = { 128, 256 },
		/* [한국어] 두 가지. */
		.nr_sizes = 2,
	},
	{
		/* OARR2/OMAP2 */
		/* [한국어] OARR2/OMAP2 는 최대 1024MB 까지 지원한다. */
		.window_sizes = { 128, 256, 512, 1024 },
		/* [한국어] 네 가지. */
		.nr_sizes = 4,
	},
	{
		/* OARR3/OMAP3 */
		/* [한국어] OARR3/OMAP3 도 마찬가지다. setup_ob 이 창을 역순으로 훑으므로
		 * 큰 구간은 이 두 창부터 소비된다. */
		.window_sizes = { 128, 256, 512, 1024 },
		/* [한국어] 네 가지. */
		.nr_sizes = 4,
	},
};

/**
 * enum iproc_pcie_ib_map_type - iProc PCIe inbound mapping type
 * @IPROC_PCIE_IB_MAP_MEM: DDR memory
 * @IPROC_PCIE_IB_MAP_IO: device I/O memory
 * @IPROC_PCIE_IB_MAP_INVALID: invalid or unused
 */
/* [한국어] 안쪽 영역의 용도 구분. 영역마다 고정되어 있어 아무 영역이나
 * 골라 쓸 수 없다. */
enum iproc_pcie_ib_map_type {
	/* [한국어] DDR 메모리용(0). dma-ranges 로 오는 구간이 전부 이 종류다.
	 * 설정자/읽는 자: 아래 paxb_v2_ib_map 의 정적 초기화와
	 *   iproc_pcie_ib_check_type().
	 * 값 범위: 0.
	 * 동기화: 상수. */
	IPROC_PCIE_IB_MAP_MEM = 0,
	/* [한국어] 장치 I/O 메모리용. paxb_v2_ib_map 에서는 IARR0 만 이 종류이며,
	 * PAXB v2 의 MSI 배선이 그 영역을 노려 이 값을 지정한다.
	 * 설정자/읽는 자: 위와 같다.
	 * 값 범위: 1.
	 * 동기화: 상수. */
	IPROC_PCIE_IB_MAP_IO,
	/* [한국어] 유효하지 않음/미사용. 이 파일 어디에서도 쓰지 않는다(전수 grep 확인).
	 * 설정자/읽는 자: 없음.
	 * 값 범위: 2.
	 * 동기화: 상수. */
	IPROC_PCIE_IB_MAP_INVALID
};

/**
 * struct iproc_pcie_ib_map - iProc PCIe inbound mapping controller-specific
 * parameters
 * @type: inbound mapping region type
 * @size_unit: inbound mapping region size unit, could be SZ_1K, SZ_1M, or
 * SZ_1G
 * @region_sizes: list of supported inbound mapping region sizes in KB, MB, or
 * GB, depending on the size unit
 * @nr_sizes: number of supported inbound mapping region sizes
 * @nr_windows: number of supported inbound mapping windows for the region
 * @imap_addr_offset: register offset between the upper and lower 32-bit
 * IMAP address registers
 * @imap_window_offset: register offset between each IMAP window
 */
struct iproc_pcie_ib_map {
	/* [한국어] 이 인바운드 매핑 영역이 어떤 메모리를 대상으로 하는지.
	 * 설정자: 아래 paxb_v2_ib_map[] 의 정적 초기화.
	 * 읽는 자: iproc_pcie_setup_ib() 가 요청한 종류와 맞는 영역을 고를 때.
	 * 값 범위: IPROC_PCIE_IB_MAP_IO 또는 IPROC_PCIE_IB_MAP_MEM. 인바운드 매핑은
	 *   PCI 버스 주소를 시스템 메모리로 되돌리는 창이므로, 종류가 다르면 창의
	 *   크기 제약과 정렬 규칙도 달라진다.
	 * 동기화: const 정적 데이터. */
	enum iproc_pcie_ib_map_type type;
	/* [한국어] 크기 목록의 단위. SZ_1K / SZ_1M / SZ_1G 중 하나다. 영역마다 다루는 크기
	 * 규모가 크게 달라(32KB 부터 512GB 까지) 목록 값을 작게 유지하려고 단위를 뗐다.
	 * 설정자: 아래 paxb_v2_ib_map 의 정적 초기화.
	 * 읽는 자: iproc_pcie_setup_ib() 이 region_sizes[i] * size_unit 으로 실제 크기를 만든다.
	 * 값 범위: SZ_1K, SZ_1M, SZ_1G.
	 * 동기화: const 정적 데이터. */
	unsigned int size_unit;
	/* [한국어] 지원 크기 목록. 단위는 위 size_unit 이 정한다.
	 * 설정자: 정적 초기화.
	 * 읽는 자: iproc_pcie_setup_ib() 의 크기 일치 검사.
	 * 값 범위: 오름차순 정수. 쓰이지 않는 뒤쪽 칸은 0.
	 * 동기화: const 정적 데이터. */
	resource_size_t region_sizes[MAX_NUM_IB_REGION_SIZES];
	/* [한국어] 위 목록에서 유효한 개수. iproc_pcie_ib_is_in_use() 가 이 값으로
	 * BIT(nr_sizes) - 1 마스크를 만들어 사용 여부를 판정하기도 한다.
	 * 설정자: 정적 초기화.
	 * 읽는 자: setup_ib 의 루프 상한, ib_is_in_use 의 마스크 계산.
	 * 값 범위: 1..MAX_NUM_IB_REGION_SIZES(9).
	 * 동기화: const 정적 데이터. */
	unsigned int nr_sizes;
	/* [한국어] 이 영역이 가진 창 개수. 전체 크기를 이 개수로 나눠 각 창에 균등 배분한다.
	 * 설정자: 정적 초기화.
	 * 읽는 자: iproc_pcie_ib_write() 의 루프 상한과 크기 나눗셈.
	 * 값 범위: 표에 있는 값은 1 과 8 뿐이다. 시프트로 나누므로 2의 거듭제곱이어야 한다.
	 * 동기화: const 정적 데이터. */
	unsigned int nr_windows;
	/* [한국어] IMAP 주소의 하위 워드와 상위 워드 사이의 거리. 영역마다 레지스터 배치가
	 * 달라 IARR0 만 0x40 이고 나머지는 0x4 다.
	 * 설정자: 정적 초기화.
	 * 읽는 자: iproc_pcie_ib_write() 가 상위 32비트를 쓸 자리를 구할 때.
	 * 값 범위: 0x40 또는 0x4.
	 * 동기화: const 정적 데이터. */
	u16 imap_addr_offset;
	/* [한국어] IMAP 창 하나에서 다음 창까지의 거리. 여러 창에 나눠 쓸 때 오프셋을
	 * 이만큼씩 민다.
	 * 설정자: 정적 초기화.
	 * 읽는 자: iproc_pcie_ib_write() 의 루프.
	 * 값 범위: 0x4 또는 0x8.
	 * 동기화: const 정적 데이터. */
	u16 imap_window_offset;
/* [한국어] 구조체 끝. */
};

static const struct iproc_pcie_ib_map paxb_v2_ib_map[] = {
	/* [한국어] PAXB v2 의 안쪽 영역 능력 표. 다섯 영역이 각각 다른 용도와 크기를 갖는다.
	 * IARR0 만 I/O 종류(32KB)이고 나머지 넷은 DDR 용이며, 크기 규모가
	 * 8MB(IARR1)부터 512GB(IARR4)까지 넓게 퍼져 있다. */
	{
		/* IARR0/IMAP0 */
		.type = IPROC_PCIE_IB_MAP_IO,
		.size_unit = SZ_1K,
		.region_sizes = { 32 },
		.nr_sizes = 1,
		.nr_windows = 8,
		.imap_addr_offset = 0x40,
		.imap_window_offset = 0x4,
	},
	{
		/* IARR1/IMAP1 */
		.type = IPROC_PCIE_IB_MAP_MEM,
		.size_unit = SZ_1M,
		.region_sizes = { 8 },
		.nr_sizes = 1,
		.nr_windows = 8,
		.imap_addr_offset = 0x4,
		.imap_window_offset = 0x8,

	},
	{
		/* IARR2/IMAP2 */
		.type = IPROC_PCIE_IB_MAP_MEM,
		.size_unit = SZ_1M,
		.region_sizes = { 64, 128, 256, 512, 1024, 2048, 4096, 8192,
				  16384 },
		.nr_sizes = 9,
		.nr_windows = 1,
		.imap_addr_offset = 0x4,
		.imap_window_offset = 0x8,
	},
	{
		/* IARR3/IMAP3 */
		.type = IPROC_PCIE_IB_MAP_MEM,
		.size_unit = SZ_1G,
		.region_sizes = { 1, 2, 4, 8, 16, 32 },
		.nr_sizes = 6,
		.nr_windows = 8,
		.imap_addr_offset = 0x4,
		.imap_window_offset = 0x8,
	},
	{
		/* IARR4/IMAP4 */
		.type = IPROC_PCIE_IB_MAP_MEM,
		.size_unit = SZ_1G,
		.region_sizes = { 32, 64, 128, 256, 512 },
		.nr_sizes = 5,
		.nr_windows = 8,
		.imap_addr_offset = 0x4,
		.imap_window_offset = 0x8,
	},
};

/*
 * iProc PCIe host registers
 */
enum iproc_pcie_reg {
	/* clock/reset signal control */
	IPROC_PCIE_CLK_CTRL = 0,

	/*
	 * To allow MSI to be steered to an external MSI controller (e.g., ARM
	 * GICv3 ITS)
	 */
	IPROC_PCIE_MSI_GIC_MODE,

	/*
	 * IPROC_PCIE_MSI_BASE_ADDR and IPROC_PCIE_MSI_WINDOW_SIZE define the
	 * window where the MSI posted writes are written, for the writes to be
	 * interpreted as MSI writes.
	 */
	IPROC_PCIE_MSI_BASE_ADDR,
	IPROC_PCIE_MSI_WINDOW_SIZE,

	/*
	 * To hold the address of the register where the MSI writes are
	 * programmed.  When ARM GICv3 ITS is used, this should be programmed
	 * with the address of the GITS_TRANSLATER register.
	 */
	IPROC_PCIE_MSI_ADDR_LO,
	IPROC_PCIE_MSI_ADDR_HI,

	/* enable MSI */
	IPROC_PCIE_MSI_EN_CFG,

	/* allow access to root complex configuration space */
	IPROC_PCIE_CFG_IND_ADDR,
	IPROC_PCIE_CFG_IND_DATA,

	/* allow access to device configuration space */
	IPROC_PCIE_CFG_ADDR,
	IPROC_PCIE_CFG_DATA,

	/* enable INTx */
	IPROC_PCIE_INTX_EN,

	/* outbound address mapping */
	IPROC_PCIE_OARR0,
	IPROC_PCIE_OMAP0,
	IPROC_PCIE_OARR1,
	IPROC_PCIE_OMAP1,
	IPROC_PCIE_OARR2,
	IPROC_PCIE_OMAP2,
	IPROC_PCIE_OARR3,
	IPROC_PCIE_OMAP3,

	/* inbound address mapping */
	IPROC_PCIE_IARR0,
	IPROC_PCIE_IMAP0,
	IPROC_PCIE_IARR1,
	IPROC_PCIE_IMAP1,
	IPROC_PCIE_IARR2,
	IPROC_PCIE_IMAP2,
	IPROC_PCIE_IARR3,
	IPROC_PCIE_IMAP3,
	IPROC_PCIE_IARR4,
	IPROC_PCIE_IMAP4,

	/* config read status */
	IPROC_PCIE_CFG_RD_STATUS,

	/* link status */
	IPROC_PCIE_LINK_STATUS,

	/* enable APB error for unsupported requests */
	IPROC_PCIE_APB_ERR_EN,

	/* total number of core registers */
	IPROC_PCIE_MAX_NUM_REG,
};

/* iProc PCIe PAXB BCMA registers */
static const u16 iproc_pcie_reg_paxb_bcma[IPROC_PCIE_MAX_NUM_REG] = {
	[IPROC_PCIE_CLK_CTRL]		= 0x000,
	/* [한국어] PAXB BCMA 변종의 오프셋 표. 지정하지 않은 항목은 0 이 되고,
	 * iproc_pcie_rev_init() 이 그것을 IPROC_PCIE_REG_INVALID 로 바꿔 넣는다.
	 * 이 변종은 창도 MSI 도 없어 항목이 일곱 개뿐이다. */
	[IPROC_PCIE_CFG_IND_ADDR]	= 0x120,
	[IPROC_PCIE_CFG_IND_DATA]	= 0x124,
	[IPROC_PCIE_CFG_ADDR]		= 0x1f8,
	[IPROC_PCIE_CFG_DATA]		= 0x1fc,
	[IPROC_PCIE_INTX_EN]		= 0x330,
	[IPROC_PCIE_LINK_STATUS]	= 0xf0c,
};

/* iProc PCIe PAXB registers */
static const u16 iproc_pcie_reg_paxb[IPROC_PCIE_MAX_NUM_REG] = {
	[IPROC_PCIE_CLK_CTRL]		= 0x000,
	/* [한국어] PAXB 변종. BCMA 판에 바깥 창 두 쌍과 APB 오류 제어가 추가된다. */
	[IPROC_PCIE_CFG_IND_ADDR]	= 0x120,
	[IPROC_PCIE_CFG_IND_DATA]	= 0x124,
	[IPROC_PCIE_CFG_ADDR]		= 0x1f8,
	[IPROC_PCIE_CFG_DATA]		= 0x1fc,
	[IPROC_PCIE_INTX_EN]		= 0x330,
	[IPROC_PCIE_OARR0]		= 0xd20,
	[IPROC_PCIE_OMAP0]		= 0xd40,
	[IPROC_PCIE_OARR1]		= 0xd28,
	[IPROC_PCIE_OMAP1]		= 0xd48,
	[IPROC_PCIE_LINK_STATUS]	= 0xf0c,
	[IPROC_PCIE_APB_ERR_EN]		= 0xf40,
};

/* iProc PCIe PAXB v2 registers */
static const u16 iproc_pcie_reg_paxb_v2[IPROC_PCIE_MAX_NUM_REG] = {
	[IPROC_PCIE_CLK_CTRL]		= 0x000,
	/* [한국어] PAXB v2 변종. 이 파일에서 가장 많은 레지스터를 갖는다 — 바깥 창 네 쌍,
	 * 안쪽 영역 다섯 쌍, config 읽기 상태 레지스터까지 있다. */
	[IPROC_PCIE_CFG_IND_ADDR]	= 0x120,
	[IPROC_PCIE_CFG_IND_DATA]	= 0x124,
	[IPROC_PCIE_CFG_ADDR]		= 0x1f8,
	[IPROC_PCIE_CFG_DATA]		= 0x1fc,
	[IPROC_PCIE_INTX_EN]		= 0x330,
	[IPROC_PCIE_OARR0]		= 0xd20,
	[IPROC_PCIE_OMAP0]		= 0xd40,
	[IPROC_PCIE_OARR1]		= 0xd28,
	[IPROC_PCIE_OMAP1]		= 0xd48,
	[IPROC_PCIE_OARR2]		= 0xd60,
	[IPROC_PCIE_OMAP2]		= 0xd68,
	[IPROC_PCIE_OARR3]		= 0xdf0,
	[IPROC_PCIE_OMAP3]		= 0xdf8,
	[IPROC_PCIE_IARR0]		= 0xd00,
	[IPROC_PCIE_IMAP0]		= 0xc00,
	[IPROC_PCIE_IARR1]		= 0xd08,
	[IPROC_PCIE_IMAP1]		= 0xd70,
	[IPROC_PCIE_IARR2]		= 0xd10,
	[IPROC_PCIE_IMAP2]		= 0xcc0,
	[IPROC_PCIE_IARR3]		= 0xe00,
	[IPROC_PCIE_IMAP3]		= 0xe08,
	[IPROC_PCIE_IARR4]		= 0xe68,
	[IPROC_PCIE_IMAP4]		= 0xe70,
	[IPROC_PCIE_CFG_RD_STATUS]	= 0xee0,
	[IPROC_PCIE_LINK_STATUS]	= 0xf0c,
	[IPROC_PCIE_APB_ERR_EN]		= 0xf40,
};

/* iProc PCIe PAXC v1 registers */
static const u16 iproc_pcie_reg_paxc[IPROC_PCIE_MAX_NUM_REG] = {
	[IPROC_PCIE_CLK_CTRL]		= 0x000,
	/* [한국어] PAXC v1 변종. 창이 하나도 없고 config 접근 레지스터만 있다.
	 * 내부 에뮬레이트 엔드포인트 전용이라 주소 변환이 필요 없기 때문이다.
	 * 간접 접근 오프셋이 PAXB 계열의 0x120 이 아니라 0x1f0 인 점도 다르다. */
	[IPROC_PCIE_CFG_IND_ADDR]	= 0x1f0,
	[IPROC_PCIE_CFG_IND_DATA]	= 0x1f4,
	[IPROC_PCIE_CFG_ADDR]		= 0x1f8,
	[IPROC_PCIE_CFG_DATA]		= 0x1fc,
};

/* iProc PCIe PAXC v2 registers */
static const u16 iproc_pcie_reg_paxc_v2[IPROC_PCIE_MAX_NUM_REG] = {
	[IPROC_PCIE_MSI_GIC_MODE]	= 0x050,
	/* [한국어] PAXC v2 변종. PAXC v1 에 MSI 전용 레지스터 묶음이 더해진다.
	 * IPROC_PCIE_CLK_CTRL 이 없는 유일한 변종이라, rev_init 이 0번 항목을
	 * 따로 INVALID 로 지정해 준다. */
	[IPROC_PCIE_MSI_BASE_ADDR]	= 0x074,
	[IPROC_PCIE_MSI_WINDOW_SIZE]	= 0x078,
	[IPROC_PCIE_MSI_ADDR_LO]	= 0x07c,
	[IPROC_PCIE_MSI_ADDR_HI]	= 0x080,
	[IPROC_PCIE_MSI_EN_CFG]		= 0x09c,
	[IPROC_PCIE_CFG_IND_ADDR]	= 0x1f0,
	[IPROC_PCIE_CFG_IND_DATA]	= 0x1f4,
	[IPROC_PCIE_CFG_ADDR]		= 0x1f8,
	[IPROC_PCIE_CFG_DATA]		= 0x1fc,
};

/*
 * List of device IDs of controllers that have corrupted capability list that
 * require SW fixup
 */
static const u16 iproc_pcie_corrupt_cap_did[] = {
	0x16cd,
	0x16f0,
	0xd802,
	0xd804
};

/* [한국어]
 * iproc_data - pci_bus 에서 이 드라이버의 상태 구조체를 꺼낸다
 *
 * @bus: PCI 코어가 넘긴 버스.
 * @return: 그 버스를 만든 컨트롤러의 struct iproc_pcie.
 *
 * PCI 코어의 콜백은 전부 struct pci_bus 만 받으므로, 드라이버 상태로
 * 돌아올 통로가 필요하다. iproc_pcie_setup() 이 브리지를 등록할 때
 * sysdata 에 심어 둔 포인터를 여기서 꺼낸다.
 *
 * 지역 변수를 거쳐 돌려주는 형태지만 컴파일러가 지워 버리는 수준이고,
 * static inline 이라 호출 비용도 없다.
 *
 * 실행 컨텍스트: 제약 없음. 포인터 역참조 한 번이다.
 *
 * 호출 체인:  config 접근 콜백들 → [이 함수] → bus->sysdata
 */
static inline struct iproc_pcie *iproc_data(struct pci_bus *bus)
{
	struct iproc_pcie *pcie = bus->sysdata;
	return pcie;
}

/* [한국어]
 * iproc_pcie_reg_is_invalid - 이 변종에 그 레지스터가 없는지 판정한다
 *
 * @reg_offset: 오프셋 표에서 꺼낸 값.
 * @return: IPROC_PCIE_REG_INVALID(0xffff)면 true.
 *
 * 변종별 오프셋 표는 그 변종에 없는 레지스터를 두 가지로 표시한다.
 * 지정하지 않아 0 이 되거나, 명시적으로 0xffff 가 들어 있는 경우다.
 * 이 함수는 후자만 본다.
 *
 * 두 표시를 구분해 쓰는 곳이 iproc_pcie_read_reg()/write_reg() 인데,
 * 그쪽은 0xffff 를 만나면 조용히 넘어가고 0 은 그대로 오프셋으로 쓴다.
 * 0x000 이 실제로 IPROC_PCIE_CLK_CTRL 의 유효한 오프셋이라 0 을
 * "없음" 으로 볼 수 없기 때문이다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  iproc_pcie_read_reg() / write_reg() / map_ep_cfg_reg() 등 → [이 함수]
 */
static inline bool iproc_pcie_reg_is_invalid(u16 reg_offset)
{
	return !!(reg_offset == IPROC_PCIE_REG_INVALID);
}

/* [한국어]
 * iproc_pcie_reg_offset - 레지스터 이름을 이 변종의 실제 오프셋으로 바꾼다
 *
 * @pcie: 컨트롤러.   @reg: enum iproc_pcie_reg 의 레지스터 이름.
 * @return: 그 변종의 오프셋 표에서 꺼낸 16비트 값.
 *
 * 이 파일이 다섯 변종을 하나의 코드로 다루는 핵심 장치다. 코드는 어디서나
 * IPROC_PCIE_CFG_ADDR 같은 이름으로 말하고, 실제 오프셋은
 * iproc_pcie_rev_init() 이 골라 둔 표(pcie->reg_offsets)가 정한다.
 *
 * 배열 첨자 하나라 검사가 없다. reg 가 IPROC_PCIE_MAX_NUM_REG 를 넘으면
 * 범위 밖을 읽지만, 인자가 enum 이고 호출자가 모두 상수를 넘기므로
 * 컴파일 시점에 값이 확정된다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  iproc_pcie_read_reg() / write_reg() 등 → [이 함수] → pcie->reg_offsets[]
 */
static inline u16 iproc_pcie_reg_offset(struct iproc_pcie *pcie,
					enum iproc_pcie_reg reg)
{
	return pcie->reg_offsets[reg];
}

/* [한국어]
 * iproc_pcie_read_reg - 이름으로 지정한 컨트롤러 레지스터를 읽는다
 *
 * @pcie: 컨트롤러.   @reg: 레지스터 이름.
 * @return: 읽은 32비트 값. 이 변종에 없는 레지스터면 0.
 *
 * 오프셋을 표에서 찾고, 없으면 0 을 돌려준다. 없는 레지스터를 읽는 것이
 * 오류가 아니라 0 인 이유는, 변종마다 지원 범위가 달라 호출자가 매번
 * 확인하게 하면 코드가 지저분해지기 때문이다. 대신 "없으면 0" 이라는
 * 규약을 호출자들이 알고 쓴다.
 *
 * 실행 컨텍스트: 제약 없음. readl 한 번이다.
 *
 * 호출 체인:  이 파일 전반 → [이 함수] → iproc_pcie_reg_offset() → readl()
 */
static inline u32 iproc_pcie_read_reg(struct iproc_pcie *pcie,
				      enum iproc_pcie_reg reg)
{
	u16 offset = iproc_pcie_reg_offset(pcie, reg);

	if (iproc_pcie_reg_is_invalid(offset))
		/* [한국어] 이 파일에서 유일하게 사용 중인지 판정하는 방식이 다른 곳이다 —
		 * 바깥 창은 valid 비트 하나, 안쪽 영역은 크기 비트 묶음을 본다. */
		return 0;

	return readl(pcie->base + offset);
}

/* [한국어]
 * iproc_pcie_write_reg - 이름으로 지정한 컨트롤러 레지스터에 쓴다
 *
 * @pcie: 컨트롤러.   @reg: 레지스터 이름.   @val: 쓸 값.
 * @return: 없음.
 *
 * 읽기 판의 짝이다. 이 변종에 없는 레지스터면 아무 일도 하지 않고 돌아간다.
 * 그 덕분에 호출자가 "이 변종에 이 레지스터가 있나" 를 묻지 않고 그냥 쓸 수
 * 있고, 없는 변종에서는 조용히 무시된다.
 *
 * 실행 컨텍스트: 제약 없음. writel 한 번이다.
 *
 * 호출 체인:  이 파일 전반 → [이 함수] → iproc_pcie_reg_offset() → writel()
 */
static inline void iproc_pcie_write_reg(struct iproc_pcie *pcie,
					enum iproc_pcie_reg reg, u32 val)
{
	u16 offset = iproc_pcie_reg_offset(pcie, reg);

	if (iproc_pcie_reg_is_invalid(offset))
		/* [한국어] 창이 아직 유효하면 건드리지 않고 다음으로 넘어간다. */
		return;

	writel(val, pcie->base + offset);
/* [한국어] 루프 끝. */
}

/*
 * APB error forwarding can be disabled during access of configuration
 * registers of the endpoint device, to prevent unsupported requests
 * (typically seen during enumeration with multi-function devices) from
 * triggering a system exception.
 */
/* [한국어]
 * iproc_pcie_apb_err_disable - config 접근 동안 APB 오류 전달을 잠시 끈다
 *
 * @bus:     접근 대상 버스.
 * @disable: true 면 끄고, false 면 되돌린다.
 * @return: 없음.
 *
 * 상류 주석이 이유를 밝힌다. 엔드포인트의 config 레지스터를 읽다 보면
 * Unsupported Request 가 나기 마련인데(특히 다기능 장치를 열거할 때
 * 없는 기능을 찔러 볼 때), 그것이 APB 오류로 전달되면 시스템 예외가 된다.
 * 그래서 접근 전후로 오류 전달을 껐다 켠다.
 *
 * 두 조건을 함께 본다. bus->number 가 0 이 아니어야 하고(루트 컴플렉스
 * 자신의 config 는 이 문제가 없다) has_apb_err_disable 플래그가 서 있어야
 * 한다. 그 플래그는 iproc_pcie_rev_init() 이 PAXB 와 PAXB v2 에만 세운다.
 *
 * 호출자인 iproc_pcie_config_read32()/write32() 가 접근을 이 함수 두 번으로
 * 감싸는 구조라, 중간에 어떤 경로로 빠져나가도 반드시 복구된다.
 *
 * 실행 컨텍스트: config 접근 경로. pci_lock 을 쥔 상태로 불린다.
 *
 * 호출 체인:  iproc_pcie_config_read32() / _write32() → [이 함수]
 */
static inline void iproc_pcie_apb_err_disable(struct pci_bus *bus,
					      bool disable)
{
	struct iproc_pcie *pcie = iproc_data(bus);
	u32 val;
/* [한국어] 버스 번호가 0 이 아니고(루트 컴플렉스 자신은 이 문제가 없다)
 * 이 변종이 APB 오류 제어를 지원할 때만 동작한다. */

	if (bus->number && pcie->has_apb_err_disable) {
		/* [한국어] 현재 값을 읽는다. */
		val = iproc_pcie_read_reg(pcie, IPROC_PCIE_APB_ERR_EN);
		/* [한국어] 끄라는 요청이면 */
		if (disable)
			/* [한국어] 활성 비트를 내린다. */
			val &= ~APB_ERR_EN;
		/* [한국어] 켜라는 요청이면 */
		else
			val |= APB_ERR_EN;
		/* [한국어] 고친 값을 되쓴다. 이 함수가 config 접근을 앞뒤로 감싸므로,
		 * 어떤 경로로 빠져나가도 반드시 원래대로 복구된다. */
		iproc_pcie_write_reg(pcie, IPROC_PCIE_APB_ERR_EN, val);
	}
}

/* [한국어]
 * iproc_pcie_map_ep_cfg_reg - 엔드포인트 config 접근용 주소를 세우고 데이터 창을 돌려준다
 *
 * @pcie:  컨트롤러.
 * @busno: 대상 버스 번호.   @devfn: 대상 devfn.   @where: config 오프셋.
 * @return: 데이터 레지스터의 가상 주소. 이 변종에 그 레지스터가 없으면 NULL.
 *
 * 이 컨트롤러의 config 접근은 두 단계다. 주소 레지스터에 "누구의 몇 번째
 * 워드" 를 써 두고, 데이터 레지스터를 읽거나 쓰면 그 대상에 닿는다.
 * 이 함수가 앞 단계를 하고 뒤 단계에 쓸 주소를 돌려준다.
 *
 * 주소 값을 조립하는 방식이 요령이다. PCIE_ECAM_OFFSET() 이 표준 ECAM
 * 규칙으로 (버스, devfn, 오프셋)을 하나의 오프셋으로 만들고, ALIGN_DOWN 으로
 * 4의 배수로 내린 뒤 CFG_ADDR_CFG_TYPE_1 을 OR 한다. 하위 2비트가 정렬로
 * 비어 있어 거기에 접근 종류를 얹을 수 있는 구조다.
 *
 * type 1 로 고정하는 것은 이 경로가 루트 버스가 아닌 곳에만 쓰이기 때문이다.
 * 루트 버스 접근은 iproc_pcie_map_cfg_bus() 가 다른 레지스터 쌍으로 처리한다.
 *
 * 실행 컨텍스트: config 접근 경로. 잠들지 않는다.
 *
 * 호출 체인:  iproc_pcie_config_read() / iproc_pcie_map_cfg_bus()
 *               → [이 함수] → iproc_pcie_write_reg()
 */
static void __iomem *iproc_pcie_map_ep_cfg_reg(struct iproc_pcie *pcie,
					       unsigned int busno,
					       unsigned int devfn,
					       int where)
{
	u16 offset;
	u32 val;
/* [한국어] ALIGN_DOWN 으로 4의 배수로 내리면 하위 2비트가 비므로,
 * 거기에 접근 종류를 얹을 수 있다. */

	/* EP device access */
	val = ALIGN_DOWN(PCIE_ECAM_OFFSET(busno, devfn, where), 4) |
		CFG_ADDR_CFG_TYPE_1;
/* [한국어] 주소를 먼저 세워 둔다. 그다음 데이터 레지스터에 접근하면 그 대상에 닿는다. */

	iproc_pcie_write_reg(pcie, IPROC_PCIE_CFG_ADDR, val);
	/* [한국어] 데이터 레지스터의 오프셋을 이 변종의 표에서 찾는다. */
	offset = iproc_pcie_reg_offset(pcie, IPROC_PCIE_CFG_DATA);

	if (iproc_pcie_reg_is_invalid(offset))
		/* [한국어] 이 변종에 데이터 레지스터가 없으면 접근할 방법이 없다. */
		return NULL;

	return (pcie->base + offset);
}

/* [한국어]
 * iproc_pcie_cfg_retry - RRS 응답이 오는 동안 config 읽기를 재시도한다
 *
 * @pcie:        컨트롤러.
 * @cfg_data_p:  읽을 데이터 레지스터의 가상 주소.
 * @return: 읽은 값. 500ms 안에 끝나지 않으면 0xffffffff.
 *
 * RRS(Request Retry Status)는 엔드포인트가 "아직 준비되지 않았으니 다시
 * 요청하라" 고 답하는 상태다. 보통은 하드웨어가 알아서 재시도하지만,
 * 이 컨트롤러는 RRS 를 받으면 CFG_RETRY_STATUS(0xffff0001)라는 특정 값을
 * 돌려주고 만다. 그래서 소프트웨어가 대신 재시도한다.
 *
 * 상류 주석이 이 우회의 한계까지 솔직히 적어 두었다. PCIe 규격상 RRS
 * 소프트웨어 가시성은 Vendor ID 읽기에만 적용되는데 이 하드웨어는 주소를
 * 가리지 않고 그 값을 돌려주며, 더 나쁘게는 어떤 레지스터의 진짜 값이
 * 우연히 CFG_RETRY_STATUS 와 같으면 그것을 RRS 로 오해해 재시도하다가
 * 결국 0xffffffff 를 돌려준다.
 *
 * 그 오해를 줄이는 장치가 루프 안의 상태 확인이다. 값이 CFG_RETRY_STATUS 와
 * 같더라도 CFG_RD_STATUS 레지스터가 CFG_RD_RRS 를 말하지 않으면 진짜 데이터로
 * 보고 그대로 돌려준다. 다만 그 레지스터가 없는 변종에서는 읽기가 0 을
 * 돌려주므로(iproc_pcie_read_reg 의 규약) CFG_RD_SUCCESS 와 같아져
 * 곧바로 빠져나온다.
 *
 * 1us 간격으로 최대 500000회, 즉 500ms 를 기다린다.
 *
 * 실행 컨텍스트: config 접근 경로. udelay 로 바쁘게 기다린다.
 *
 * 호출 체인:  iproc_pcie_config_read() → [이 함수] → readl() / iproc_pcie_read_reg()
 */
static unsigned int iproc_pcie_cfg_retry(struct iproc_pcie *pcie,
					 void __iomem *cfg_data_p)
{
	int timeout = CFG_RETRY_STATUS_TIMEOUT_US;
	unsigned int data;
	/* [한국어] CFG_RD_STATUS 레지스터에서 읽을 값. 재시도 여부의 근거다. */
	u32 status;

	/*
	 * As per PCIe r6.0, sec 2.3.2, Config RRS Software Visibility only
	 * affects config reads of the Vendor ID.  For config writes or any
	 * other config reads, the Root may automatically reissue the
	 * configuration request again as a new request.
	 *
	 * For config reads, this hardware returns CFG_RETRY_STATUS data
	 * when it receives a RRS completion, regardless of the address of
	 * the read or the RRS Software Visibility Enable bit.  As a
	 * partial workaround for this, we retry in software any read that
	 * returns CFG_RETRY_STATUS.
	 *
	 * Note that a non-Vendor ID config register may have a value of
	 * CFG_RETRY_STATUS.  If we read that, we can't distinguish it from
	 * a RRS completion, so we will incorrectly retry the read and
	 * eventually return the wrong data (0xffffffff).
	 */
	data = readl(cfg_data_p);
	while (data == CFG_RETRY_STATUS && timeout--) {
		/*
		 * RRS state is set in CFG_RD status register
		 * This will handle the case where CFG_RETRY_STATUS is
		 * valid config data.
		 */
		status = iproc_pcie_read_reg(pcie, IPROC_PCIE_CFG_RD_STATUS);
		if (status != CFG_RD_RRS)
			/* [한국어] 상태가 RRS 가 아니면 방금 읽은 값이 진짜 데이터라는 뜻이다.
			 * 값이 우연히 CFG_RETRY_STATUS 와 같았을 뿐이므로 그대로 돌려준다.
			 * 이 검사가 상류 주석이 말하는 오해를 줄이는 장치다. */
			return data;

		udelay(1);
		data = readl(cfg_data_p);
	/* [한국어] 1us 쉬고 다시 읽는다(윗줄과 함께). timeout 이 500000 이라 상한이 500ms 다. */
	}

	if (data == CFG_RETRY_STATUS)
		/* [한국어] 끝까지 CFG_RETRY_STATUS 였으면 진짜로 준비되지 않은 것이라,
		 * "없는 장치" 의 관례값인 전부 1 로 바꿔 돌려준다. */
		data = 0xffffffff;

	return data;
}

/* [한국어]
 * iproc_pcie_fix_cap - PAXC 의 망가진 capability 목록을 읽기 시점에 고쳐 준다
 *
 * @pcie:  컨트롤러.
 * @where: 읽고 있는 config 오프셋.
 * @val:   방금 읽은 값. 필요하면 이 함수가 고쳐 놓는다.
 * @return: 없음.
 *
 * 일부 PAXC 컨트롤러는 capability 목록이 깨진 채로 하드웨어에 박혀 있다.
 * 고칠 방법이 없으니 커널이 읽는 값을 가로채 올바른 모양으로 바꿔 준다.
 * 읽기 경로에만 개입하므로 하드웨어는 그대로다.
 *
 * 네 갈래로 나뉜다.
 *   PCI_VENDOR_ID   - 여기서는 고치지 않고 판정만 한다. device ID 가
 *                     iproc_pcie_corrupt_cap_did[] 목록에 있으면
 *                     fix_paxc_cap 을 세워 아래 갈래들이 동작하게 한다.
 *                     Vendor ID 읽기가 열거의 첫 접근이라 여기에 둔 것이다.
 *   IPROC_PCI_PM_CAP - 전원 관리 capability 가 있다고 알리고, 다음
 *                     capability 포인터를 PCIe capability 로 강제한다.
 *   IPROC_PCI_EXP_CAP - PCIe capability 를 루트 포트·버전 2 로 만들고
 *                     다음 포인터를 0 으로 두어 목록을 끝낸다.
 *   +PCI_EXP_RTCTL  - RRS 소프트웨어 가시성 지원 비트를 지운다. 위
 *                     iproc_pcie_cfg_retry() 가 그 기능이 제대로 동작하지
 *                     않는다고 말하는 하드웨어이므로, 커널이 그것을
 *                     쓰려 하지 않게 막는 것이다.
 *
 * switch 의 대상이 where & ~0x3 인 것은 config 읽기가 워드 단위라 오프셋의
 * 하위 2비트를 무시해야 하기 때문이다.
 *
 * 실행 컨텍스트: config 접근 경로.
 *
 * 호출 체인:  iproc_pcie_config_read() → [이 함수]
 */
static void iproc_pcie_fix_cap(struct iproc_pcie *pcie, int where, u32 *val)
{
	u32 i, dev_id;

	switch (where & ~0x3) {
	/* [한국어] Vendor ID 를 읽는 순간. 열거의 첫 접근이라 여기서 판정을 한다. */
	case PCI_VENDOR_ID:
		/* [한국어] 상위 16비트가 device ID 다. */
		dev_id = *val >> 16;

		/*
		 * Activate fixup for those controllers that have corrupted
		 * capability list registers
		 */
		for (i = 0; i < ARRAY_SIZE(iproc_pcie_corrupt_cap_did); i++)
			if (dev_id == iproc_pcie_corrupt_cap_did[i])
				/* [한국어] 망가진 목록을 가진 모델이면 표시를 세운다. 아래 갈래들이 이 표시를 본다. */
				pcie->fix_paxc_cap = true;
		/* [한국어] Vendor ID 자체는 고치지 않는다 — 판정만 하는 갈래다. */
		break;

	case IPROC_PCI_PM_CAP:
		/* [한국어] 표시가 서 있을 때만 고친다. 다른 컨트롤러의 값을 건드리면 안 된다. */
		if (pcie->fix_paxc_cap) {
			/* advertise PM, force next capability to PCIe */
			*val &= ~IPROC_PCI_PM_CAP_MASK;
			*val |= IPROC_PCI_EXP_CAP << 8 | PCI_CAP_ID_PM;
		}
		break;

	case IPROC_PCI_EXP_CAP:
		/* [한국어] PCIe capability 자리도 마찬가지다. */
		if (pcie->fix_paxc_cap) {
			/* advertise root port, version 2, terminate here */
			*val = (PCI_EXP_TYPE_ROOT_PORT << 4 | 2) << 16 |
				PCI_CAP_ID_EXP;
		}
		break;

	case IPROC_PCI_EXP_CAP + PCI_EXP_RTCTL:
		/* Don't advertise RRS SV support */
		*val &= ~(PCI_EXP_RTCAP_RRS_SV << 16);
		break;

	default:
		break;
	}
}

/* [한국어]
 * iproc_pcie_config_read - PAXB v2/PAXC 전용 config 읽기 경로
 *
 * @bus, @devfn: 대상 장치.   @where: 오프셋.   @size: 1, 2, 4 바이트.
 * @val: 결과를 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL / DEVICE_NOT_FOUND / FUNC_NOT_SUPPORTED.
 *
 * iproc_cfg_read 플래그가 선 변종에서만 쓰이는 특수 경로다. 평범한
 * pci_generic_config_read32() 대신 이 함수를 쓰는 이유는 RRS 재시도와
 * capability 고치기가 필요해서다.
 *
 * 루트 버스(번호 0)는 일반 경로로 읽고 iproc_pcie_fix_cap() 만 덧붙인다.
 * 그 아래 장치는 주소 창을 세우고 재시도 루프로 읽은 뒤, 요청 크기에 맞게
 * 잘라 낸다.
 *
 * 마지막 블록이 또 하나의 ASIC 결함 우회다. 상류 주석대로 PAXC 계열에서는
 * 펌웨어가 설정하지 않은 물리 기능(PF)을 루트 컴플렉스에서 숨길 수 없고,
 * 그런 PF 에 쓰기를 하면 내장 프로세서가 멈춰 버린다. 설정되지 않은 PF 는
 * device ID 가 0x168e 로 남아 있으므로, Vendor ID 를 읽는 시점에 그 값을
 * 잡아내 PCIBIOS_FUNC_NOT_SUPPORTED 로 답해 열거 자체를 막는다.
 *
 * 함수 한가운데에서 #define 두 개를 선언하는 것이 특이하지만 상류 코드
 * 그대로다.
 *
 * 실행 컨텍스트: config 접근 경로. udelay 로 최대 500ms 대기할 수 있다.
 *
 * 호출 체인:  iproc_pcie_config_read32() → [이 함수]
 *               → iproc_pcie_map_ep_cfg_reg() → iproc_pcie_cfg_retry()
 *               → iproc_pcie_fix_cap()
 */
static int iproc_pcie_config_read(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 *val)
{
	struct iproc_pcie *pcie = iproc_data(bus);
	unsigned int busno = bus->number;
	/* [한국어] 데이터 레지스터의 가상 주소. */
	void __iomem *cfg_data_p;
	/* [한국어] 재시도 루프가 읽어 온 값. */
	unsigned int data;
	/* [한국어] 하위 호출 결과. */
	int ret;

	/* root complex access */
	if (busno == 0) {
		ret = pci_generic_config_read32(bus, devfn, where, size, val);
		/* [한국어] 루트 버스 읽기가 성공했으면 */
		if (ret == PCIBIOS_SUCCESSFUL)
			/* [한국어] capability 를 고칠 기회를 준다. 아래 장치에는 이 처리를 하지 않는데,
			 * 망가진 것이 컨트롤러 자신의 config 이기 때문이다. */
			iproc_pcie_fix_cap(pcie, where, val);
/* [한국어] 루트 버스 처리는 여기서 끝난다. */

		return ret;
	}

	cfg_data_p = iproc_pcie_map_ep_cfg_reg(pcie, busno, devfn, where);
/* [한국어] 주소 창을 세우고 데이터 레지스터 주소를 얻는다. */

	if (!cfg_data_p)
		/* [한국어] 이 변종에 그 레지스터가 없으면 접근할 수 없다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	data = iproc_pcie_cfg_retry(pcie, cfg_data_p);
/* [한국어] RRS 재시도 루프로 읽는다(윗줄). */

	*val = data;
	if (size <= 2)
		*val = (data >> (8 * (where & 3))) & ((1 << (size * 8)) - 1);

	/*
	 * For PAXC and PAXCv2, the total number of PFs that one can enumerate
	 * depends on the firmware configuration. Unfortunately, due to an ASIC
	 * bug, unconfigured PFs cannot be properly hidden from the root
	 * complex. As a result, write access to these PFs will cause bus lock
	 * up on the embedded processor
	 *
	 * Since all unconfigured PFs are left with an incorrect, staled device
	 * ID of 0x168e (PCI_DEVICE_ID_NX2_57810), we try to catch those access
	 * early here and reject them all
	 */
#define DEVICE_ID_MASK     0xffff0000
#define DEVICE_ID_SHIFT    16
	if (pcie->rej_unconfig_pf &&
	    /* [한국어] 지금 읽는 것이 Vendor ID 자리인가. 마스크로 레지스터 번호만 남겨 비교한다. */
	    (where & CFG_ADDR_REG_NUM_MASK) == PCI_VENDOR_ID)
		if ((*val & DEVICE_ID_MASK) ==
		    /* [한국어] device ID 가 0x168e 면 펌웨어가 설정하지 않은 PF 다. 상류 주석대로
		     * 이런 PF 에 쓰기를 하면 내장 프로세서가 멈추므로, 열거 단계에서
		     * 미리 걸러 낸다. */
		    (PCI_DEVICE_ID_NX2_57810 << DEVICE_ID_SHIFT))
			return PCIBIOS_FUNC_NOT_SUPPORTED;

	return PCIBIOS_SUCCESSFUL;
}

/*
 * Note access to the configuration registers are protected at the higher layer
 * by 'pci_lock' in drivers/pci/access.c
 */
/* [한국어]
 * iproc_pcie_map_cfg_bus - 대상에 따라 두 가지 config 창 중 하나를 고른다
 *
 * @pcie:  컨트롤러.
 * @busno: 버스 번호.   @devfn: devfn.   @where: 오프셋.
 * @return: 접근할 데이터 레지스터의 가상 주소. 불가능하면 NULL.
 *
 * 루트 컴플렉스 자신과 그 아래 장치는 접근 방법이 다르다.
 *
 * 루트 버스(번호 0)는 "간접(indirect)" 레지스터 쌍을 쓴다. CFG_IND_ADDR 에
 * 오프셋을 쓰고 CFG_IND_DATA 로 주고받는다. 오프셋을 CFG_IND_ADDR_MASK
 * (0x00001ffc)로 자르는데, 하위 2비트가 0 이라 워드 정렬이 강제되고
 * 상위로는 8KB 범위만 표현된다.
 *
 * devfn 이 0 이 아니면 NULL 을 돌려준다. 루트 컴플렉스는 기능이 하나뿐이라
 * 그 외의 조회에는 장치가 없다고 답해야 한다.
 *
 * 그 아래 버스는 앞서 본 iproc_pcie_map_ep_cfg_reg() 로 넘긴다.
 *
 * 상류 주석이 이 경로의 직렬화가 상위 계층의 pci_lock 으로 보장된다고
 * 밝혀 두었다 - 주소를 쓰고 데이터를 읽는 사이에 다른 접근이 끼어들면
 * 안 되기 때문이다.
 *
 * 실행 컨텍스트: config 접근 경로. pci_lock 을 쥔 상태.
 *
 * 호출 체인:  iproc_pcie_bus_map_cfg_bus() / iproc_pci_raw_config_*()
 *               → [이 함수] → iproc_pcie_map_ep_cfg_reg()
 */
static void __iomem *iproc_pcie_map_cfg_bus(struct iproc_pcie *pcie,
					    int busno, unsigned int devfn,
					    int where)
{
	u16 offset;

	/* root complex access */
	if (busno == 0) {
		if (PCIE_ECAM_DEVFN(devfn) > 0)
			/* [한국어] 루트 컴플렉스는 기능이 하나뿐이라 devfn 0 이 아닌 조회에는 장치가 없다고 답한다. */
			return NULL;

		iproc_pcie_write_reg(pcie, IPROC_PCIE_CFG_IND_ADDR,
				     /* [한국어] 간접 접근용 주소 레지스터에 오프셋을 쓴다. 마스크로 워드 정렬을 강제하고
				      * 8KB 범위로 자른다. */
				     where & CFG_IND_ADDR_MASK);
		offset = iproc_pcie_reg_offset(pcie, IPROC_PCIE_CFG_IND_DATA);
		/* [한국어] 이 변종에 간접 데이터 레지스터가 없으면 */
		if (iproc_pcie_reg_is_invalid(offset))
			/* [한국어] 접근할 수 없다. */
			return NULL;
		else
			return (pcie->base + offset);
	/* [한국어] 루트 버스 갈래 끝. 그 아래 버스는 다음 줄에서 다른 경로로 넘긴다. */
	}

	return iproc_pcie_map_ep_cfg_reg(pcie, busno, devfn, where);
}

/* [한국어]
 * iproc_pcie_bus_map_cfg_bus - pci_ops 의 map_bus 콜백
 *
 * @bus: 대상 버스.   @devfn: devfn.   @where: 오프셋.
 * @return: 접근할 주소, 또는 NULL.
 *
 * pci_ops.map_bus 의 서명에 맞추려고 감싼 한 줄짜리다. PCI 코어는
 * struct pci_bus 를 넘기는데 본체는 struct iproc_pcie 와 버스 번호를
 * 따로 받으므로, iproc_data() 로 변환해 전달한다.
 *
 * 이 콜백이 있으면 pci_generic_config_read32()/write32() 를 그대로 쓸 수
 * 있다. 그 일반 함수들이 map_bus 로 주소를 얻어 readl/writel 하는 구조라,
 * 이 드라이버는 주소 계산만 제공하면 된다.
 *
 * 실행 컨텍스트: config 접근 경로.
 *
 * 호출 체인:  pci_generic_config_read32() / _write32() → [이 함수]
 *               → iproc_pcie_map_cfg_bus()
 */
static void __iomem *iproc_pcie_bus_map_cfg_bus(struct pci_bus *bus,
						unsigned int devfn,
						int where)
{
	return iproc_pcie_map_cfg_bus(iproc_data(bus), bus->number, devfn,
				      where);
}

/* [한국어]
 * iproc_pci_raw_config_read32 - PCI 코어를 거치지 않고 루트 config 를 읽는다
 *
 * @pcie: 컨트롤러.   @devfn: devfn.   @where: 오프셋.   @size: 1, 2, 4.
 * @val: 결과를 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * "raw" 인 이유는 struct pci_bus 없이 동작하기 때문이다. 이 드라이버는
 * 버스를 등록하기 *전에* 자기 config space 를 읽고 써야 하는데
 * (링크 확인, 클래스 코드 교정 등), 그 시점에는 pci_bus 가 아직 없다.
 *
 * 버스 번호를 0 으로 고정해 부르므로 항상 간접 레지스터 쌍을 쓴다.
 * 오프셋에서 하위 2비트를 미리 지우고 넘긴 뒤, 읽어 온 워드에서 원래
 * 오프셋의 하위 2비트로 필요한 바이트를 잘라 낸다.
 *
 * 실행 컨텍스트: probe 경로(프로세스 컨텍스트).
 *
 * 호출 체인:  iproc_pcie_check_link() → [이 함수] → iproc_pcie_map_cfg_bus() → readl()
 */
static int iproc_pci_raw_config_read32(struct iproc_pcie *pcie,
				       unsigned int devfn, int where,
				       int size, u32 *val)
{
	void __iomem *addr;

	addr = iproc_pcie_map_cfg_bus(pcie, 0, devfn, where & ~0x3);
	/* [한국어] 주소를 얻지 못했으면 */
	if (!addr)
		/* [한국어] 장치 없음으로 답한다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	*val = readl(addr);

	if (size <= 2)
		*val = (*val >> (8 * (where & 3))) & ((1 << (size * 8)) - 1);

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * iproc_pci_raw_config_write32 - PCI 코어를 거치지 않고 루트 config 에 쓴다
 *
 * @pcie: 컨트롤러.   @devfn: devfn.   @where: 오프셋.   @size: 1, 2, 4.
 * @val: 쓸 값.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * 읽기 판의 짝이다. 4바이트면 그대로 쓰고, 1·2바이트면 읽고-고쳐-쓰기를
 * 한다 - 하드웨어가 워드 단위로만 쓸 수 있기 때문이다.
 *
 * 마스크 계산이 한 줄에 압축돼 있다. (1 << (size*8)) - 1 로 그 폭의 마스크를
 * 만들고, (where & 0x3) * 8 만큼 밀어 자리를 잡은 뒤 반전해 그 자리를
 * 지울 마스크로 만든다. 그다음 새 값을 같은 자리로 밀어 얹는다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:  iproc_pcie_check_link() → [이 함수] → iproc_pcie_map_cfg_bus() → writel()
 */
static int iproc_pci_raw_config_write32(struct iproc_pcie *pcie,
					unsigned int devfn, int where,
					int size, u32 val)
{
	void __iomem *addr;
	u32 mask, tmp;
/* [한국어] 오프셋의 하위 2비트를 미리 지워 워드 단위로 접근한다. */

	addr = iproc_pcie_map_cfg_bus(pcie, 0, devfn, where & ~0x3);
	/* [한국어] 주소를 얻지 못했으면 */
	if (!addr)
		/* [한국어] 장치 없음으로 답한다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	if (size == 4) {
		/* [한국어] 4바이트면 읽고-고칠 필요 없이 그대로 쓴다. */
		writel(val, addr);
		/* [한국어] 성공. */
		return PCIBIOS_SUCCESSFUL;
	}

	mask = ~(((1 << (size * 8)) - 1) << ((where & 0x3) * 8));
	/* [한국어] 그 폭의 자리를 지운 워드를 읽어 온다(윗줄에서 마스크를 만들었다). */
	tmp = readl(addr) & mask;
	/* [한국어] 새 값을 같은 자리로 밀어 얹는다. */
	tmp |= val << ((where & 0x3) * 8);
	/* [한국어] 합친 워드를 되쓴다. */
	writel(tmp, addr);
/* [한국어] 부분 쓰기 완료. */

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * iproc_pcie_config_read32 - pci_ops 의 읽기 콜백
 *
 * @bus, @devfn: 대상.   @where: 오프셋.   @size: 1, 2, 4.   @val: 결과.
 * @return: PCIBIOS_* 코드.
 *
 * APB 오류 전달을 끄고, 변종에 맞는 읽기 함수를 고르고, 다시 켠다.
 * iproc_cfg_read 플래그가 선 변종(PAXB v2, PAXC, PAXC v2)은 RRS 재시도와
 * capability 고치기가 필요해 전용 경로를 타고, 나머지는 커널 일반 함수를 쓴다.
 *
 * APB 끄기와 켜기가 이 함수 안에서 짝을 이루므로, 어느 경로로 읽든
 * 반드시 복구된다.
 *
 * 실행 컨텍스트: pci_lock 을 쥔 상태. PAXB v2 경로는 최대 500ms 바쁘게 기다린다.
 *
 * 호출 체인:  PCI 코어(access.c) → [이 함수]
 *               → iproc_pcie_config_read() 또는 pci_generic_config_read32()
 */
static int iproc_pcie_config_read32(struct pci_bus *bus, unsigned int devfn,
				    int where, int size, u32 *val)
{
	int ret;
	struct iproc_pcie *pcie = iproc_data(bus);
/* [한국어] APB 오류 전달을 끈 상태에서 읽는다. */

	iproc_pcie_apb_err_disable(bus, true);
	/* [한국어] 전용 config 읽기가 필요한 변종이면 */
	if (pcie->iproc_cfg_read)
		/* [한국어] RRS 재시도와 capability 고치기가 들어간 경로를 쓴다. */
		ret = iproc_pcie_config_read(bus, devfn, where, size, val);
	/* [한국어] 아니면 */
	else
		ret = pci_generic_config_read32(bus, devfn, where, size, val);
	/* [한국어] 어느 경로였든 APB 오류 전달을 되돌린다. */
	iproc_pcie_apb_err_disable(bus, false);
/* [한국어] 읽기 결과를 전한다. */

	return ret;
}

/* [한국어]
 * iproc_pcie_config_write32 - pci_ops 의 쓰기 콜백
 *
 * @bus, @devfn: 대상.   @where: 오프셋.   @size: 1, 2, 4.   @val: 쓸 값.
 * @return: PCIBIOS_* 코드.
 *
 * 읽기 판과 달리 변종별 분기가 없다. 쓰기에는 RRS 재시도가 필요 없고
 * (상류 주석대로 하드웨어가 알아서 재발행한다) capability 를 고칠 이유도
 * 없기 때문이다. 그래서 APB 오류 전달만 껐다 켜고 커널 일반 함수에 맡긴다.
 *
 * 실행 컨텍스트: pci_lock 을 쥔 상태.
 *
 * 호출 체인:  PCI 코어(access.c) → [이 함수] → pci_generic_config_write32()
 */
static int iproc_pcie_config_write32(struct pci_bus *bus, unsigned int devfn,
				     int where, int size, u32 val)
{
	int ret;

	iproc_pcie_apb_err_disable(bus, true);
	/* [한국어] 쓰기에는 변종별 분기가 없다. RRS 재시도가 필요 없고(하드웨어가 알아서
	 * 재발행한다) capability 를 고칠 이유도 없기 때문이다. */
	ret = pci_generic_config_write32(bus, devfn, where, size, val);
	/* [한국어] APB 오류 전달을 되돌린다. */
	iproc_pcie_apb_err_disable(bus, false);
/* [한국어] 쓰기 결과를 전한다. */

	return ret;
}

static struct pci_ops iproc_pcie_ops = {
	/* [한국어] 주소 계산 콜백. 이것이 있으면 커널의 일반 config 함수를 그대로 쓸 수 있다. */
	.map_bus = iproc_pcie_bus_map_cfg_bus,
	/* [한국어] 읽기 콜백. 아래 쓰기 콜백과 함께 PCI 코어와 이 드라이버의 상시 접점이다. */
	.read = iproc_pcie_config_read32,
	.write = iproc_pcie_config_write32,
};

/* [한국어]
 * iproc_pcie_perst_ctrl - 엔드포인트 리셋 신호(PERST#)를 걸거나 푼다
 *
 * @pcie:   컨트롤러.
 * @assert: true 면 리셋을 걸고, false 면 푼다.
 * @return: 없음.
 *
 * PERST# 는 PCIe 슬롯에 나가는 물리 리셋 신호다. 링크를 세우기 전에 한 번
 * 걸었다 풀어 엔드포인트를 알려진 상태에서 출발시킨다.
 *
 * PAXC 는 통째로 건너뛴다. 상류 주석이 이유를 밝히는데, PAXC 아래에는
 * SoC 내부에 에뮬레이트된 엔드포인트가 있고 부팅 초기에 이미 펌웨어가
 * 올라가 있을 수 있어, 리셋하면 오히려 문제가 된다.
 *
 * 거는 쪽은 세 비트를 함께 내린다 - PERST 소스 선택, PERST 를 견디는
 * EP 모드, 그리고 리셋 출력이다. 푸는 쪽은 리셋 출력 비트만 올린다.
 * 비대칭인 이유는 앞의 두 비트가 모드 설정이라 한 번 정리하면 되고
 * 매번 되돌릴 필요가 없기 때문으로 보인다.
 *
 * 지연 시간이 다르다. 걸 때는 250us, 풀 때는 100ms 다. 뒤쪽이 훨씬 긴 것은
 * 엔드포인트가 리셋에서 깨어나 링크 학습을 시작할 시간을 줘야 해서다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 잠든다.
 *
 * 호출 체인:  iproc_pcie_setup() / iproc_pcie_shutdown() → [이 함수]
 */
static void iproc_pcie_perst_ctrl(struct iproc_pcie *pcie, bool assert)
{
	u32 val;

	/*
	 * PAXC and the internal emulated endpoint device downstream should not
	 * be reset.  If firmware has been loaded on the endpoint device at an
	 * earlier boot stage, reset here causes issues.
	 */
	if (pcie->ep_is_internal)
		return;

	if (assert) {
		/* [한국어] 현재 클럭/리셋 제어 값을 읽는다. */
		val = iproc_pcie_read_reg(pcie, IPROC_PCIE_CLK_CTRL);
		/* [한국어] PERST 소스 선택과 EP 생존 모드를 */
		val &= ~EP_PERST_SOURCE_SELECT & ~EP_MODE_SURVIVE_PERST &
			/* [한국어] 리셋 출력과 함께 내린다. 세 비트를 한 번에 지우는 형태다. */
			~RC_PCIE_RST_OUTPUT;
		/* [한국어] 고친 값을 쓴다 — 이 순간 PERST# 가 assert 된다. */
		iproc_pcie_write_reg(pcie, IPROC_PCIE_CLK_CTRL, val);
		/* [한국어] 250us 유지한다. 엔드포인트가 리셋 신호를 인식할 최소 시간이다. */
		udelay(250);
	} else {
		val = iproc_pcie_read_reg(pcie, IPROC_PCIE_CLK_CTRL);
		/* [한국어] 리셋 출력 비트만 올린다(윗줄에서 현재 값을 읽었다). 앞의 두 비트는
		 * 모드 설정이라 되돌릴 필요가 없어 비대칭이다. */
		val |= RC_PCIE_RST_OUTPUT;
		/* [한국어] 고친 값을 쓴다 — PERST# 가 해제된다. */
		iproc_pcie_write_reg(pcie, IPROC_PCIE_CLK_CTRL, val);
		/* [한국어] 100ms 기다린다. 엔드포인트가 리셋에서 깨어나 링크 학습을 시작할 시간이다. */
		msleep(100);
	}
}

/* [한국어]
 * iproc_pcie_shutdown - 시스템 종료 시 엔드포인트에 리셋을 걸어 둔다
 *
 * @pcie: 컨트롤러.
 * @return: 항상 0.
 *
 * PERST# 를 assert 한 뒤 500ms 기다린다. 다음 부팅이나 kexec 로 넘어갈 때
 * 엔드포인트가 어중간한 상태로 남아 있지 않게 하려는 것이다.
 *
 * 500ms 는 perst_ctrl() 안의 250us 와 별개로 더 기다리는 시간이다.
 * 리셋이 실제로 장치에 반영되고 안정될 여유를 주는 것으로 보이며,
 * 그 근거가 될 규격 조항은 이 트리에서 확인하지 못했다.
 *
 * 항상 0 을 돌려주므로 실패하지 않는다. 반환형이 int 인 것은 결합
 * 드라이버의 shutdown 콜백 서명에 맞추기 위한 것으로 보인다.
 *
 * EXPORT_SYMBOL_GPL 로 공개되며, 이 트리에서 확인한 호출자는
 * pcie-iproc-platform.c:128 하나다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(시스템 종료). 잠든다.
 *
 * 호출 체인:  iproc_pltfm_pcie_shutdown() [pcie-iproc-platform.c:452]
 *               → [이 함수] → iproc_pcie_perst_ctrl()
 */
int iproc_pcie_shutdown(struct iproc_pcie *pcie)
{
	iproc_pcie_perst_ctrl(pcie, true);
	msleep(500);

	return 0;
}
EXPORT_SYMBOL_GPL(iproc_pcie_shutdown);

/* [한국어]
 * iproc_pcie_check_link - 링크가 실제로 섰는지 확인하고, 필요하면 Gen1 로 낮춰 재시도한다
 *
 * @pcie: 컨트롤러.
 * @return: 0 이면 링크 정상. -ENODEV 는 PHY/링크 비활성 또는 링크 폭 0,
 *          -EFAULT 는 컨트롤러가 엔드포인트 모드로 잡혀 있는 경우.
 *
 * PAXC 는 Serdes 가 없고 내부 엔드포인트에 직결되므로 상류 주석대로
 * 이 검사를 통째로 건너뛰고 0 을 돌려준다.
 *
 * 나머지 변종에서는 네 단계를 밟는다.
 *   1) LINK_STATUS 레지스터에서 PHY 링크업과 데이터 링크 활성 두 비트를 본다.
 *   2) 자기 헤더 종류가 bridge 인지 확인한다. 아니면 이 컨트롤러가
 *      엔드포인트 모드로 잡혀 있다는 뜻이라 루트 컴플렉스로 쓸 수 없다.
 *   3) 클래스 코드를 PCI_CLASS_BRIDGE_PCI_NORMAL 로 강제한다. 하드웨어에
 *      잘못 박혀 있어 커널이 브리지로 인식하지 못하는 문제를 여기서 고친다.
 *      오프셋 0x43c 는 벤더 고유 자리라 표준 상수가 없어 함수 안에서
 *      #define 으로 선언해 두었다.
 *   4) 링크 상태의 Negotiated Link Width 가 0 이 아닌지 본다. 폭이 0 이면
 *      링크가 형식상 올라왔어도 실제로 쓸 수 없다.
 *
 * 마지막이 이 함수의 요점이다. 폭이 0 이면 목표 속도를 Gen2 에서 Gen1 로
 * 낮춰 다시 시도한다. 신호 품질이 나빠 Gen2 로는 못 붙는 보드에서
 * 속도를 낮추면 붙는 경우가 있기 때문이다. 낮춘 뒤 100ms 기다렸다가
 * 링크 폭을 다시 읽는다. 이미 Gen1 이었다면 재시도하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). msleep 으로 잠든다.
 *
 * 호출 체인:  iproc_pcie_setup() → [이 함수]
 *               → iproc_pci_raw_config_read32() / _write32()
 */
static int iproc_pcie_check_link(struct iproc_pcie *pcie)
{
	struct device *dev = pcie->dev;
	u32 hdr_type, link_ctrl, link_status, class, val;
	/* [한국어] 링크가 실제로 쓸 수 있는 상태인지. 링크 폭이 0 이 아니어야 참이 된다. */
	bool link_is_active = false;

	/*
	 * PAXC connects to emulated endpoint devices directly and does not
	 * have a Serdes.  Therefore skip the link detection logic here.
	 */
	if (pcie->ep_is_internal)
		return 0;

	val = iproc_pcie_read_reg(pcie, IPROC_PCIE_LINK_STATUS);
	/* [한국어] PHY 링크업과 데이터 링크 활성이 둘 다 서야 한다. */
	if (!(val & PCIE_PHYLINKUP) || !(val & PCIE_DL_ACTIVE)) {
		/* [한국어] 하나라도 없으면 물리 계층 문제다. */
		dev_err(dev, "PHY or data link is INACTIVE!\n");
		/* [한국어] 장치 없음으로 답한다. */
		return -ENODEV;
	}

	/* make sure we are not in EP mode */
	iproc_pci_raw_config_read32(pcie, 0, PCI_HEADER_TYPE, 1, &hdr_type);
	if ((hdr_type & PCI_HEADER_TYPE_MASK) != PCI_HEADER_TYPE_BRIDGE) {
		/* [한국어] 헤더 종류가 bridge 가 아니면 이 컨트롤러가 엔드포인트 모드로 잡혀 있다. */
		dev_err(dev, "in EP mode, hdr=%#02x\n", hdr_type);
		/* [한국어] 루트 컴플렉스로 쓸 수 없으므로 -EFAULT. */
		return -EFAULT;
	}

	/* force class to PCI_CLASS_BRIDGE_PCI_NORMAL (0x060400) */
#define PCI_BRIDGE_CTRL_REG_OFFSET	0x43c
#define PCI_BRIDGE_CTRL_REG_CLASS_MASK	0xffffff
	iproc_pci_raw_config_read32(pcie, 0, PCI_BRIDGE_CTRL_REG_OFFSET,
				    /* [한국어] 벤더 고유 자리의 브리지 제어 레지스터를 읽는다(윗줄에서 오프셋을 정의했다). */
				    4, &class);
	class &= ~PCI_BRIDGE_CTRL_REG_CLASS_MASK;
	/* [한국어] 클래스 코드를 브리지로 강제한다(윗줄에서 기존 값을 지웠다).
	 * 하드웨어에 잘못 박혀 있어 커널이 브리지로 인식하지 못하기 때문이다. */
	class |= PCI_CLASS_BRIDGE_PCI_NORMAL;
	/* [한국어] 고친 값을 되쓴다. */
	iproc_pci_raw_config_write32(pcie, 0, PCI_BRIDGE_CTRL_REG_OFFSET,
				     /* [한국어] 4바이트 통째로 쓴다. */
				     4, class);

	/* check link status to see if link is active */
	iproc_pci_raw_config_read32(pcie, 0, IPROC_PCI_EXP_CAP + PCI_EXP_LNKSTA,
				    2, &link_status);
	if (link_status & PCI_EXP_LNKSTA_NLW)
		/* [한국어] 협상된 링크 폭이 0 이 아니면 링크가 실제로 쓸 수 있는 상태다. */
		link_is_active = true;
/* [한국어] 아래에서 이 값이 거짓이면 Gen1 재시도로 들어간다. */

	if (!link_is_active) {
		/* try GEN 1 link speed */
#define PCI_TARGET_LINK_SPEED_MASK	0xf
#define PCI_TARGET_LINK_SPEED_GEN2	0x2
#define PCI_TARGET_LINK_SPEED_GEN1	0x1
		iproc_pci_raw_config_read32(pcie, 0,
					    /* [한국어] 링크 제어 2 레지스터를 읽는다. 목표 속도가 이 안에 있다. */
					    IPROC_PCI_EXP_CAP + PCI_EXP_LNKCTL2,
					    4, &link_ctrl);
		if ((link_ctrl & PCI_TARGET_LINK_SPEED_MASK) ==
		    /* [한국어] 목표가 Gen2 였다면 낮출 여지가 있다. 이미 Gen1 이면 재시도하지 않는다. */
		    PCI_TARGET_LINK_SPEED_GEN2) {
			link_ctrl &= ~PCI_TARGET_LINK_SPEED_MASK;
			/* [한국어] 목표를 Gen1 로 바꾼다(윗줄에서 기존 속도 필드를 지웠다).
			 * 신호 품질이 나빠 Gen2 로 못 붙는 보드에서 속도를 낮추면 붙는 경우가 있다. */
			link_ctrl |= PCI_TARGET_LINK_SPEED_GEN1;
			/* [한국어] 바꾼 값을 되쓴다. */
			iproc_pci_raw_config_write32(pcie, 0,
					/* [한국어] 같은 레지스터에 4바이트로. */
					IPROC_PCI_EXP_CAP + PCI_EXP_LNKCTL2,
					4, link_ctrl);
			msleep(100);

			iproc_pci_raw_config_read32(pcie, 0,
					/* [한국어] 100ms 기다린 뒤 링크 상태를 다시 읽는다(윗줄에서 잠들었다). */
					IPROC_PCI_EXP_CAP + PCI_EXP_LNKSTA,
					2, &link_status);
			if (link_status & PCI_EXP_LNKSTA_NLW)
				/* [한국어] 이번에 폭이 잡히면 링크가 살아난 것이다. */
				link_is_active = true;
		}
	}

	dev_info(dev, "link: %s\n", link_is_active ? "UP" : "DOWN");
/* [한국어] 링크 상태를 로그로 남긴다(윗줄). 링크가 없으면 -ENODEV 로 답해
 * probe 가 실패하고 "no PCIe EP device detected" 가 찍힌다. */

	return link_is_active ? 0 : -ENODEV;
}

/* [한국어]
 * iproc_pcie_enable - INTx 수신을 켠다
 *
 * @pcie: 컨트롤러.   @return: 없음.
 *
 * INTX_EN 레지스터에 SYS_RC_INTX_MASK(0xf)를 쓴다. 하위 4비트가
 * INTA-INTD 네 핀에 대응하므로, 네 개를 모두 켜는 셈이다.
 *
 * 이름이 "enable" 로 넓지만 실제로 하는 일은 INTx 하나뿐이다. MSI 는
 * 별도 경로(iproc_pcie_msi_enable)가 맡고, 링크는 이미 서 있는 상태에서
 * 불린다.
 *
 * 이 레지스터가 없는 변종(PAXC 계열)에서는 iproc_pcie_write_reg() 가
 * 조용히 무시하므로 별도 분기가 필요 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  iproc_pcie_setup() → [이 함수] → iproc_pcie_write_reg()
 */
static void iproc_pcie_enable(struct iproc_pcie *pcie)
{
	iproc_pcie_write_reg(pcie, IPROC_PCIE_INTX_EN, SYS_RC_INTX_MASK);
}

/* [한국어]
 * iproc_pcie_ob_is_valid - 바깥 창이 이미 쓰이고 있는지 본다
 *
 * @pcie:       컨트롤러.
 * @window_idx: 창 번호.
 * @return: 그 창의 valid 비트가 서 있으면 true.
 *
 * OARR 레지스터의 0번 비트가 창 활성 표시다. MAP_REG() 매크로로 창 번호를
 * 오프셋으로 바꾸는데, 그 매크로가 base + index*2 인 것은 enum 에서
 * OARR 과 OMAP 이 번갈아 배치돼 있어 창 하나가 enum 항목 두 칸을
 * 차지하기 때문이다.
 *
 * iproc_pcie_invalidate_mapping() 이 부팅 초기에 모든 창을 비우므로,
 * 설정 도중에는 보통 false 다. 실제 쓰임은 창을 지울 때 이미 비어 있는지
 * 확인하는 쪽이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  iproc_pcie_invalidate_mapping() → [이 함수] → iproc_pcie_read_reg()
 */
static inline bool iproc_pcie_ob_is_valid(struct iproc_pcie *pcie,
					  int window_idx)
{
	u32 val;

	val = iproc_pcie_read_reg(pcie, MAP_REG(IPROC_PCIE_OARR0, window_idx));
/* [한국어] 바깥 창은 valid 비트 하나로 사용 여부가 정해진다. */

	return !!(val & OARR_VALID);
}

/* [한국어]
 * iproc_pcie_ob_write - 바깥 창 하나에 주소와 크기를 써 넣는다
 *
 * @pcie:       컨트롤러.
 * @window_idx: 창 번호.
 * @size_idx:   그 창이 지원하는 크기 목록에서의 인덱스.
 * @axi_addr:   CPU(AXI) 쪽 시작 주소.
 * @pci_addr:   그에 대응하는 PCI 쪽 주소.
 * @return: 항상 0.
 *
 * 창 하나가 레지스터 두 쌍을 쓴다. OARR 이 AXI 주소와 크기·유효 비트를,
 * OMAP 이 PCI 주소를 담으며, 각각 하위·상위 32비트로 나뉜다.
 *
 * 쓰는 순서가 중요하다. OARR 하위에 유효 비트를 함께 쓰는 것이 마지막이
 * 아니라 중간인데, 그 뒤에 OARR 상위와 OMAP 을 마저 채운다. 창이 반쯤
 * 설정된 상태로 잠시 유효해지는 셈이지만, 이 시점은 아직 버스를 스캔하기
 * 전이라 그 창으로 트랜잭션이 나갈 일이 없다.
 *
 * 크기는 값이 아니라 인덱스로 표현된다. size_idx 를 OARR_SIZE_CFG_SHIFT(1)
 * 만큼 밀어 유효 비트 옆에 얹는데, 하드웨어가 지원 크기를 목록으로만
 * 받기 때문이다. 그 목록이 paxb_ob_map / paxb_v2_ob_map 이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  iproc_pcie_setup_ob() → [이 함수] → iproc_pcie_write_reg()
 */
static inline int iproc_pcie_ob_write(struct iproc_pcie *pcie, int window_idx,
				      int size_idx, u64 axi_addr, u64 pci_addr)
{
	struct device *dev = pcie->dev;
	u16 oarr_offset, omap_offset;
/* [한국어] 이 창의 OARR 오프셋을 표에서 찾는다. */

	/*
	 * Derive the OARR/OMAP offset from the first pair (OARR0/OMAP0) based
	 * on window index.
	 */
	oarr_offset = iproc_pcie_reg_offset(pcie, MAP_REG(IPROC_PCIE_OARR0,
							  window_idx));
	omap_offset = iproc_pcie_reg_offset(pcie, MAP_REG(IPROC_PCIE_OMAP0,
							  /* [한국어] OMAP 오프셋도 같은 방식으로 찾는다(윗줄에서 MAP_REG 로 창 번호를 변환했다). */
							  window_idx));
	if (iproc_pcie_reg_is_invalid(oarr_offset) ||
	    /* [한국어] 둘 중 하나라도 이 변종에 없으면 창을 쓸 수 없다. */
	    iproc_pcie_reg_is_invalid(omap_offset))
		return -EINVAL;

	/*
	 * Program the OARR registers.  The upper 32-bit OARR register is
	 * always right after the lower 32-bit OARR register.
	 */
	writel(lower_32_bits(axi_addr) | (size_idx << OARR_SIZE_CFG_SHIFT) |
	       OARR_VALID, pcie->base + oarr_offset);
	/* [한국어] AXI 주소의 상위 32비트를 OARR 다음 워드에 쓴다. */
	writel(upper_32_bits(axi_addr), pcie->base + oarr_offset + 4);
/* [한국어] 하위는 윗줄에서 크기·유효 비트와 함께 이미 썼다. */

	/* now program the OMAP registers */
	writel(lower_32_bits(pci_addr), pcie->base + omap_offset);
	writel(upper_32_bits(pci_addr), pcie->base + omap_offset + 4);
/* [한국어] OMAP 에 PCI 주소를 쓴다(윗줄과 함께 하위·상위). */

	dev_dbg(dev, "ob window [%d]: offset 0x%x axi %pap pci %pap\n",
		/* [한국어] 디버그 빌드에서 창 설정 내역을 남긴다. */
		window_idx, oarr_offset, &axi_addr, &pci_addr);
	dev_dbg(dev, "oarr lo 0x%x oarr hi 0x%x\n",
		/* [한국어] 방금 쓴 OARR 값을 되읽어 확인한다. */
		readl(pcie->base + oarr_offset),
		readl(pcie->base + oarr_offset + 4));
	dev_dbg(dev, "omap lo 0x%x omap hi 0x%x\n",
		/* [한국어] OMAP 도 마찬가지로 되읽어 남긴다. */
		readl(pcie->base + omap_offset),
		readl(pcie->base + omap_offset + 4));

	return 0;
}

/*
 * Some iProc SoCs require the SW to configure the outbound address mapping
 *
 * Outbound address translation:
 *
 * iproc_pcie_address = axi_address - axi_offset
 * OARR = iproc_pcie_address
 * OMAP = pci_addr
 *
 * axi_addr -> iproc_pcie_address -> OARR -> OMAP -> pci_address
 */
/* [한국어]
 * iproc_pcie_setup_ob - 요청한 구간을 지원되는 창 크기로 쪼개 바깥 창에 배정한다
 *
 * @pcie:     컨트롤러.
 * @axi_addr: CPU 쪽 시작 주소.
 * @pci_addr: PCI 쪽 시작 주소.
 * @size:     매핑할 크기.
 * @return: 0 성공. -EINVAL 은 정렬이 맞지 않는 경우,
 *          -ENOMEM 은 창이 모자란 경우.
 *
 * 바깥 창은 CPU 가 PCI 주소 공간에 접근하는 통로다. 문제는 하드웨어가
 * 임의 크기를 받지 않고 창마다 정해진 크기 목록 중에서만 고를 수 있다는
 * 점이다(paxb 는 128/256MB 두 가지, paxb_v2 의 창 2·3 은 최대 1024MB).
 *
 * 그래서 바깥 루프가 창을 큰 번호부터 하나씩 소비하고, 안쪽 루프가 그 창의
 * 크기 목록을 큰 것부터 훑는다. 둘 다 역순인 이유는 큰 창을 먼저 써서
 * 창 개수를 아끼기 위해서다. 이미 쓰이고 있는 창은 건너뛴다.
 *
 * 남은 크기가 창 크기보다 작을 때의 처리가 이 함수에서 가장 미묘하다.
 * 보통은 더 작은 크기를 찾아 continue 하지만, 마지막 선택지(size_idx 와
 * window_idx 가 모두 0)에 이르면 물러설 곳이 없으므로 주소를 창 크기로
 * 내림하고 크기 자체를 창 크기로 키워 버린다. 요청보다 넓게 매핑되는
 * 셈인데, 하드웨어가 그보다 작은 창을 표현할 수 없어 나온 타협이다.
 *
 * 정렬은 타협하지 않는다. AXI 주소나 PCI 주소가 고른 창 크기에 정렬돼
 * 있지 않으면 다음 크기를 보는 것이 아니라 그 자리에서 -EINVAL 로 실패한다.
 * 정렬이 어긋난 창은 하드웨어가 엉뚱한 주소로 해석하기 때문이다.
 *
 * 축소되는 값이 셋(size, axi_addr, pci_addr)이라 매 회차 함께 전진시킨다.
 * 창을 다 썼는데 크기가 남으면 -ENOMEM 으로 실패하고, 그때 남은 크기를
 * 로그에 찍어 DT 를 어떻게 고쳐야 하는지 알려 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  iproc_pcie_map_ranges() → [이 함수] → iproc_pcie_ob_write()
 */
static int iproc_pcie_setup_ob(struct iproc_pcie *pcie, u64 axi_addr,
			       u64 pci_addr, resource_size_t size)
{
	struct iproc_pcie_ob *ob = &pcie->ob;
	struct device *dev = pcie->dev;
	/* [한국어] ret 을 -EINVAL 로 시작한다. 창을 하나도 못 쓰고 루프를 빠져나오면
	 * 이 값이 그대로 err_ob 로 흘러 들어간다. */
	int ret = -EINVAL, window_idx, size_idx;

	if (axi_addr < ob->axi_offset) {
		/* [한국어] AXI 주소가 컨트롤러 내부 오프셋보다 작으면 변환이 불가능하다. */
		dev_err(dev, "axi address %pap less than offset %pap\n",
			/* [한국어] 어느 주소가 문제인지 남기고 거절한다. */
			&axi_addr, &ob->axi_offset);
		return -EINVAL;
	}

	/*
	 * Translate the AXI address to the internal address used by the iProc
	 * PCIe core before programming the OARR
	 */
	axi_addr -= ob->axi_offset;

	/* iterate through all OARR/OMAP mapping windows */
	for (window_idx = ob->nr_windows - 1; window_idx >= 0; window_idx--) {
		const struct iproc_pcie_ob_map *ob_map =
			/* [한국어] 이 창의 능력 서술을 가져온다. 창 번호를 역순으로 훑으므로 큰 창부터다. */
			&pcie->ob_map[window_idx];

		/*
		 * If current outbound window is already in use, move on to the
		 * next one.
		 */
		if (iproc_pcie_ob_is_valid(pcie, window_idx))
			continue;

		/*
		 * Iterate through all supported window sizes within the
		 * OARR/OMAP pair to find a match.  Go through the window sizes
		 * in a descending order.
		 */
		for (size_idx = ob_map->nr_sizes - 1; size_idx >= 0;
		     size_idx--) {
			/* [한국어] 이 크기 선택지의 실제 바이트 수를 구한다. */
			resource_size_t window_size =
				/* [한국어] 표의 값이 MB 단위라 SZ_1M 을 곱한다. */
				ob_map->window_sizes[size_idx] * SZ_1M;

			/*
			 * Keep iterating until we reach the last window and
			 * with the minimal window size at index zero. In this
			 * case, we take a compromise by mapping it using the
			 * minimum window size that can be supported
			 */
			if (size < window_size) {
				if (size_idx > 0 || window_idx > 0)
					/* [한국어] 남은 크기가 이 창 크기보다 작으면 보통 더 작은 선택지를 찾아 넘어간다.
					 * 다만 마지막 선택지(size_idx 와 window_idx 가 모두 0)면 물러설 곳이 없어
					 * 아래로 떨어진다. */
					continue;

				/*
				 * For the corner case of reaching the minimal
				 * window size that can be supported on the
				 * last window
				 */
				axi_addr = ALIGN_DOWN(axi_addr, window_size);
				pci_addr = ALIGN_DOWN(pci_addr, window_size);
				/* [한국어] 그 마지막 경우에는 주소를 창 크기로 내리고 크기 자체를 창 크기로 키운다.
				 * 요청보다 넓게 매핑되지만, 하드웨어가 더 작은 창을 표현할 수 없어 나온 타협이다. */
				size = window_size;
			/* [한국어] 크기 조정 블록 끝. */
			}

			if (!IS_ALIGNED(axi_addr, window_size) ||
			    /* [한국어] AXI 든 PCI 든 창 크기에 정렬돼 있지 않으면 하드웨어가 엉뚱한 주소로
			     * 해석한다. 여기서는 다음 크기를 보지 않고 즉시 실패한다. */
			    !IS_ALIGNED(pci_addr, window_size)) {
				dev_err(dev,
					"axi %pap or pci %pap not aligned\n",
					&axi_addr, &pci_addr);
				return -EINVAL;
			}

			/*
			 * Match found!  Program both OARR and OMAP and mark
			 * them as a valid entry.
			 */
			ret = iproc_pcie_ob_write(pcie, window_idx, size_idx,
						  axi_addr, pci_addr);
			if (ret)
				/* [한국어] 창 쓰기가 실패하면 공통 오류 경로로. */
				goto err_ob;

			size -= window_size;
			/* [한국어] 요청한 크기를 모두 덮었으면 */
			if (size == 0)
				/* [한국어] 성공이다. 남은 창은 그대로 둔다. */
				return 0;

			/*
			 * If we are here, we are done with the current window,
			 * but not yet finished all mappings.  Need to move on
			 * to the next window.
			 */
			axi_addr += window_size;
			pci_addr += window_size;
			/* [한국어] 이 창에 쓸 크기를 정했으므로 안쪽 루프를 끝내고 다음 창으로 넘어간다.
			 * (윗줄에서 주소들을 창 크기만큼 전진시켰다.) */
			break;
		}
	}

err_ob:
	dev_err(dev, "unable to configure outbound mapping\n");
	dev_err(dev,
		"axi %pap, axi offset %pap, pci %pap, res size %pap\n",
		&axi_addr, &ob->axi_offset, &pci_addr, &size);

	return ret;
}

/* [한국어]
 * iproc_pcie_map_ranges - 브리지의 메모리 창 목록을 바깥 창으로 옮긴다
 *
 * @pcie: 컨트롤러.
 * @resources: 결합 드라이버가 만든 자원 목록.
 * @return: 0 성공, 첫 실패의 errno.
 *
 * DT 나 결합 드라이버가 정한 메모리 창들을 훑으며 항목마다
 * iproc_pcie_setup_ob() 를 부른다.
 *
 * 메모리 자원만 처리한다. I/O 자원과 버스 번호 자원은 이 컨트롤러의
 * 바깥 창 대상이 아니다.
 *
 * AXI 주소를 계산할 때 window->offset 을 더하는 것이 아니라 빼는 방향인
 * 점을 눈여겨볼 만하다. 자원의 start 는 CPU 주소이고 offset 은 CPU 와 PCI
 * 주소의 차이라, start - offset 이 PCI 주소가 된다. 여기에 다시
 * pcie->ob.axi_offset 을 더해 컨트롤러 내부 주소로 바꾼다 - 그 오프셋의
 * 의미는 pcie-iproc.h 의 struct iproc_pcie_ob 주석이 밝힌다.
 *
 * 하나라도 실패하면 그 자리에서 멈추고 errno 를 전한다. 되돌리지 않는데,
 * 호출자가 probe 를 실패시켜 장치 전체가 정리되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  iproc_pcie_setup() → [이 함수] → iproc_pcie_setup_ob()
 */
static int iproc_pcie_map_ranges(struct iproc_pcie *pcie,
				 struct list_head *resources)
{
	struct device *dev = pcie->dev;
	struct resource_entry *window;
	/* [한국어] 하위 호출 결과. */
	int ret;

	resource_list_for_each_entry(window, resources) {
		/* [한국어] 이 항목이 가리키는 자원. */
		struct resource *res = window->res;
		/* [한국어] 자원 종류를 미리 꺼내 둔다. */
		u64 res_type = resource_type(res);
/* [한국어] 아래 switch 가 종류별로 갈린다. */

		switch (res_type) {
		/* [한국어] I/O 자원과 */
		case IORESOURCE_IO:
		/* [한국어] 버스 번호 자원은 바깥 창 대상이 아니라 그냥 넘어간다. */
		case IORESOURCE_BUS:
			break;
		case IORESOURCE_MEM:
			/* [한국어] 메모리 자원만 바깥 창으로 만든다. */
			ret = iproc_pcie_setup_ob(pcie, res->start,
						  res->start - window->offset,
						  resource_size(res));
			if (ret)
				/* [한국어] 하나라도 실패하면 그 자리에서 멈추고 errno 를 전한다.
				 * 되돌리지 않는 이유는 호출자가 probe 를 실패시켜 장치 전체가 정리되기 때문이다. */
				return ret;
			break;
		default:
			dev_err(dev, "invalid resource %pR\n", res);
			return -EINVAL;
		}
	}

	return 0;
}

/* [한국어]
 * iproc_pcie_ib_is_in_use - 안쪽 영역이 이미 쓰이고 있는지 본다
 *
 * @pcie:       컨트롤러.
 * @region_idx: 영역 번호.
 * @return: 그 영역의 크기 비트 중 하나라도 서 있으면 true.
 *
 * 바깥 창은 valid 비트 하나로 판정했지만 안쪽은 다르다. IARR 레지스터의
 * 하위 비트들이 "어느 크기로 쓰고 있는가" 를 표시하는 자리이고, 그중
 * 하나라도 서 있으면 그 영역이 사용 중이다.
 *
 * 그래서 마스크가 BIT(nr_sizes) - 1 로 만들어진다. 그 영역이 지원하는
 * 크기 개수만큼의 하위 비트를 모두 덮는 마스크다. 예컨대 IARR2 는
 * 지원 크기가 9개라 하위 9비트를 본다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  iproc_pcie_setup_ib() / iproc_pcie_invalidate_mapping()
 *               → [이 함수] → iproc_pcie_read_reg()
 */
static inline bool iproc_pcie_ib_is_in_use(struct iproc_pcie *pcie,
					   int region_idx)
{
	const struct iproc_pcie_ib_map *ib_map = &pcie->ib_map[region_idx];
	u32 val;
/* [한국어] IARR 의 크기 비트 묶음을 마스크로 만들어 사용 여부를 판정한다.
 * 지원 크기 개수만큼의 하위 비트를 덮는 BIT(nr_sizes) - 1 형태다. */

	val = iproc_pcie_read_reg(pcie, MAP_REG(IPROC_PCIE_IARR0, region_idx));
/* [한국어] 한 줄 비교지만 함수로 뗀 것은 setup_ib 의 조건문을 읽기 쉽게 하려는 것이다. */

	return !!(val & (BIT(ib_map->nr_sizes) - 1));
}

/* [한국어]
 * iproc_pcie_ib_check_type - 이 영역이 원하는 매핑 종류인지 본다
 *
 * @ib_map: 영역의 능력 서술.   @type: 원하는 종류(MEM 또는 IO).
 * @return: 같으면 true.
 *
 * 안쪽 영역마다 용도가 고정되어 있다. paxb_v2_ib_map 을 보면 IARR0 만
 * IPROC_PCIE_IB_MAP_IO 이고 나머지 넷은 모두 MEM 이다. DDR 메모리를
 * 매핑하려는데 I/O 전용 영역을 골라 쓸 수는 없으므로 이 검사가 필요하다.
 *
 * 한 줄 비교를 함수로 뗀 것은 iproc_pcie_setup_ib() 의 조건문을 읽기 쉽게
 * 하려는 것으로 보인다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  iproc_pcie_setup_ib() → [이 함수]
 */
static inline bool iproc_pcie_ib_check_type(const struct iproc_pcie_ib_map *ib_map,
					    enum iproc_pcie_ib_map_type type)
{
	return !!(ib_map->type == type);
}

/* [한국어]
 * iproc_pcie_ib_write - 안쪽 영역 하나에 주소와 크기를 써 넣는다
 *
 * @pcie:       컨트롤러.
 * @region_idx: 영역 번호.
 * @size_idx:   지원 크기 목록에서의 인덱스.
 * @nr_windows: 이 영역이 가진 창 개수.
 * @axi_addr:   보드 쪽(AXI) 시작 주소.
 * @pci_addr:   장치가 볼 PCI 주소.
 * @size:       전체 크기.
 * @return: 0 성공, -EINVAL 은 이 변종에 해당 레지스터가 없는 경우.
 *
 * 바깥 창과 방향이 반대다. IARR 이 PCI 쪽 주소를, IMAP 이 AXI 쪽 주소를
 * 담는다 - 장치가 낸 PCI 주소를 받아 보드 메모리 주소로 바꾸는 통로이기
 * 때문이다.
 *
 * 한 영역이 여러 창으로 나뉜다는 점이 바깥 창과 크게 다르다.
 * paxb_v2_ib_map 을 보면 IARR0/1/3/4 는 창이 8개, IARR2 만 1개다.
 * 전체 크기를 창 개수로 나눠(size >>= ilog2(nr_windows)) 각 창에 균등
 * 배분하고, IMAP 오프셋을 imap_window_offset 만큼 밀어 가며 채운다.
 *
 * 크기를 시프트로 나누는 것은 창 개수가 2의 거듭제곱이라는 전제다.
 * 실제 표의 값이 1 과 8 뿐이라 성립한다.
 *
 * IMAP 하위를 읽고-OR-쓰기 하는 것도 눈에 띈다. 상위 비트에 하드웨어가
 * 쓰는 다른 정보가 있을 수 있어 보존하려는 것으로 보이나, 그 비트들의
 * 의미는 이 트리에서 확인하지 못했다.
 *
 * 주소 상위 32비트는 imap_addr_offset 만큼 떨어진 자리에 쓴다. 이 값이
 * 영역마다 다른데(IARR0 만 0x40, 나머지는 0x4) 레지스터 배치가 영역별로
 * 다르기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  iproc_pcie_setup_ib() → [이 함수] → writel() / readl()
 */
static int iproc_pcie_ib_write(struct iproc_pcie *pcie, int region_idx,
			       int size_idx, int nr_windows, u64 axi_addr,
			       u64 pci_addr, resource_size_t size)
{
	struct device *dev = pcie->dev;
	const struct iproc_pcie_ib_map *ib_map = &pcie->ib_map[region_idx];
	/* [한국어] IARR 과 IMAP 각각의 오프셋. */
	u16 iarr_offset, imap_offset;
	/* [한국어] IMAP 하위 워드를 읽고-고쳐-쓸 임시 변수. */
	u32 val;
	/* [한국어] 창 반복자. */
	int window_idx;

	iarr_offset = iproc_pcie_reg_offset(pcie,
				/* [한국어] IARR 오프셋을 표에서 찾는다(윗줄). MAP_REG 로 영역 번호를 변환한다. */
				MAP_REG(IPROC_PCIE_IARR0, region_idx));
	imap_offset = iproc_pcie_reg_offset(pcie,
				/* [한국어] IMAP 오프셋도 같은 방식으로(윗줄). */
				MAP_REG(IPROC_PCIE_IMAP0, region_idx));
	if (iproc_pcie_reg_is_invalid(iarr_offset) ||
	    /* [한국어] 둘 중 하나라도 이 변종에 없으면 영역을 쓸 수 없다. */
	    iproc_pcie_reg_is_invalid(imap_offset))
		return -EINVAL;

	dev_dbg(dev, "ib region [%d]: offset 0x%x axi %pap pci %pap\n",
		/* [한국어] 디버그 빌드에서 어느 영역에 무엇을 매핑하는지 남긴다. */
		region_idx, iarr_offset, &axi_addr, &pci_addr);

	/*
	 * Program the IARR registers.  The upper 32-bit IARR register is
	 * always right after the lower 32-bit IARR register.
	 */
	writel(lower_32_bits(pci_addr) | BIT(size_idx),
	       pcie->base + iarr_offset);
	writel(upper_32_bits(pci_addr), pcie->base + iarr_offset + 4);
/* [한국어] IARR 상위 워드에 PCI 주소의 상위 32비트를 쓴다(윗줄).
 * 하위는 그 앞에서 크기 비트와 함께 이미 썼다. */

	dev_dbg(dev, "iarr lo 0x%x iarr hi 0x%x\n",
		/* [한국어] 방금 쓴 값을 되읽어 디버그 로그에 남긴다. */
		readl(pcie->base + iarr_offset),
		readl(pcie->base + iarr_offset + 4));

	/*
	 * Now program the IMAP registers.  Each IARR region may have one or
	 * more IMAP windows.
	 */
	size >>= ilog2(nr_windows);
	for (window_idx = 0; window_idx < nr_windows; window_idx++) {
		/* [한국어] IMAP 하위 워드의 현재 값을 읽는다. */
		val = readl(pcie->base + imap_offset);
		/* [한국어] AXI 주소 하위와 유효 비트를 얹는다. 읽고-OR-쓰기인 것은 상위 비트에
		 * 하드웨어가 쓰는 다른 정보를 보존하려는 것으로 보이나, 그 비트들의
		 * 의미는 이 트리에서 확인하지 못했다. */
		val |= lower_32_bits(axi_addr) | IMAP_VALID;
		/* [한국어] 고친 값을 쓴다. */
		writel(val, pcie->base + imap_offset);
		/* [한국어] AXI 주소 상위 32비트는 imap_addr_offset 만큼 떨어진 자리에 쓴다.
		 * 그 거리가 영역마다 달라(IARR0 만 0x40) 표에서 가져온다. */
		writel(upper_32_bits(axi_addr),
		       pcie->base + imap_offset + ib_map->imap_addr_offset);

		dev_dbg(dev, "imap window [%d] lo 0x%x hi 0x%x\n",
			/* [한국어] 창마다 설정 내역을 디버그 로그로 남긴다. */
			window_idx, readl(pcie->base + imap_offset),
			readl(pcie->base + imap_offset +
			      ib_map->imap_addr_offset));

		imap_offset += ib_map->imap_window_offset;
		/* [한국어] 다음 창은 그만큼 뒤쪽 주소를 맡는다(윗줄에서 IMAP 오프셋도 밀었다).
		 * 전체 크기를 창 개수로 나눈 값이라 균등 배분이 된다. */
		axi_addr += size;
	/* [한국어] 창 루프 끝. */
	}

	return 0;
}

/* [한국어]
 * iproc_pcie_setup_ib - 요청한 구간에 딱 맞는 안쪽 영역을 찾아 배정한다
 *
 * @pcie:  컨트롤러.
 * @entry: DT 의 dma-ranges 항목 하나.
 * @type:  필요한 매핑 종류(MEM 또는 IO).
 * @return: 0 성공, -EINVAL 은 맞는 영역이 없거나 정렬이 어긋난 경우.
 *
 * 바깥 창과 결정적으로 다른 점이 있다. 바깥은 큰 구간을 여러 창으로 쪼갤
 * 수 있었지만, 안쪽은 크기가 **정확히 일치**해야 한다(size != region_size 면
 * continue). 쪼개기도 내림도 하지 않는다.
 *
 * 그 결과 DT 의 dma-ranges 크기가 하드웨어가 지원하는 목록에 없으면
 * 그대로 실패한다. paxb_v2 기준으로 쓸 수 있는 크기는 IARR1 의 8MB,
 * IARR2 의 64MB~16GB 아홉 가지, IARR3 의 1~32GB 여섯 가지, IARR4 의
 * 32~512GB 다섯 가지다.
 *
 * 영역을 고르는 조건이 셋이다 - 아직 쓰이지 않았고, 종류가 맞고,
 * 크기가 정확히 같아야 한다. 셋을 모두 만족하면 정렬을 확인하고
 * (어긋나면 즉시 -EINVAL) 써 넣은 뒤 곧바로 돌아간다.
 *
 * 바깥 창과 달리 영역 번호를 0부터 오름차순으로 훑는다. 안쪽은 크기가
 * 정확히 맞아야 해서 큰 것을 먼저 볼 이유가 없기 때문이다.
 *
 * 루프를 다 돌고 나오면 ret 을 -EINVAL 로 두고 err_ib 로 흘러 들어간다.
 * 성공 경로가 루프 안에서 곧바로 return 하므로 가능한 구조다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  iproc_pcie_map_dma_ranges() → [이 함수]
 *               → iproc_pcie_ib_is_in_use() → iproc_pcie_ib_check_type()
 *               → iproc_pcie_ib_write()
 */
static int iproc_pcie_setup_ib(struct iproc_pcie *pcie,
			       struct resource_entry *entry,
			       enum iproc_pcie_ib_map_type type)
{
	struct device *dev = pcie->dev;
	struct iproc_pcie_ib *ib = &pcie->ib;
	/* [한국어] 하위 호출 결과. */
	int ret;
	/* [한국어] 영역과 크기 반복자. */
	unsigned int region_idx, size_idx;
	/* [한국어] 보드 쪽(AXI) 시작 주소. */
	u64 axi_addr = entry->res->start;
	/* [한국어] 장치가 볼 PCI 주소. CPU 주소에서 offset 을 빼 얻는다. */
	u64 pci_addr = entry->res->start - entry->offset;
	/* [한국어] 매핑할 전체 크기. 바깥 창과 달리 이 값이 줄지 않는다 — 쪼개지 않기 때문이다. */
	resource_size_t size = resource_size(entry->res);
/* [한국어] 아래 이중 루프가 조건에 맞는 영역을 찾는다. */

	/* iterate through all IARR mapping regions */
	for (region_idx = 0; region_idx < ib->nr_regions; region_idx++) {
		const struct iproc_pcie_ib_map *ib_map =
			/* [한국어] 이 영역의 능력 서술. 바깥 창과 달리 0부터 오름차순으로 훑는다 —
			 * 크기가 정확히 맞아야 해서 큰 것을 먼저 볼 이유가 없다. */
			&pcie->ib_map[region_idx];

		/*
		 * If current inbound region is already in use or not a
		 * compatible type, move on to the next.
		 */
		if (iproc_pcie_ib_is_in_use(pcie, region_idx) ||
		    !iproc_pcie_ib_check_type(ib_map, type))
			continue;

		/* iterate through all supported region sizes to find a match */
		for (size_idx = 0; size_idx < ib_map->nr_sizes; size_idx++) {
			resource_size_t region_size =
			/* [한국어] 이 크기 선택지의 실제 바이트 수. 단위가 영역마다 달라 size_unit 을 곱한다. */
			ib_map->region_sizes[size_idx] * ib_map->size_unit;

			if (size != region_size)
				/* [한국어] 바깥 창과 결정적으로 다른 점이다. 크기가 정확히 같지 않으면 그냥 넘어간다 —
				 * 쪼개기도 내림도 하지 않는다. 그래서 DT 의 dma-ranges 크기가 하드웨어
				 * 지원 목록에 없으면 그대로 실패한다. */
				continue;

			if (!IS_ALIGNED(axi_addr, region_size) ||
			    /* [한국어] 크기가 맞아도 정렬이 어긋나면 즉시 실패한다. */
			    !IS_ALIGNED(pci_addr, region_size)) {
				dev_err(dev,
					"axi %pap or pci %pap not aligned\n",
					&axi_addr, &pci_addr);
				return -EINVAL;
			}

			/* Match found!  Program IARR and all IMAP windows. */
			ret = iproc_pcie_ib_write(pcie, region_idx, size_idx,
						  ib_map->nr_windows, axi_addr,
						  pci_addr, size);
			if (ret)
				/* [한국어] 영역 쓰기가 실패하면 공통 오류 경로로. 성공하면 곧바로 0 을 돌려주므로
				 * 루프를 더 돌지 않는다. */
				goto err_ib;
			else
				return 0;

		}
	}
	ret = -EINVAL;

err_ib:
	dev_err(dev, "unable to configure inbound mapping\n");
	dev_err(dev, "axi %pap, pci %pap, res size %pap\n",
		/* [한국어] 어느 구간을 매핑하려다 실패했는지 남긴다. DT 를 어떻게 고쳐야 하는지
		 * 알려 주는 정보다. */
		&axi_addr, &pci_addr, &size);

	return ret;
}

/* [한국어]
 * iproc_pcie_map_dma_ranges - DT 의 dma-ranges 전체를 안쪽 영역으로 옮긴다
 *
 * @pcie: 컨트롤러.
 * @return: 0 성공, 첫 실패의 errno.
 *
 * 브리지의 dma_ranges 목록을 훑으며 항목마다 iproc_pcie_setup_ib() 를
 * IPROC_PCIE_IB_MAP_MEM 종류로 부른다.
 *
 * 종류를 MEM 으로 고정하는 것이 요점이다. dma-ranges 는 장치가 DMA 로
 * 접근할 시스템 메모리를 기술하는 속성이므로 I/O 영역일 수 없다.
 * 그래서 IARR0(I/O 전용)은 이 경로로는 절대 선택되지 않는다.
 *
 * pci_host_bridge_from_priv() 로 브리지를 역산하는데, iproc_pcie_setup() 이
 * private 영역에 이 구조체를 담아 브리지를 할당했기에 가능하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  iproc_pcie_setup() → [이 함수] → iproc_pcie_setup_ib()
 */
static int iproc_pcie_map_dma_ranges(struct iproc_pcie *pcie)
{
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie);
	struct resource_entry *entry;
	/* [한국어] 첫 실패를 담을 변수. 0 으로 시작해 항목이 하나도 없으면 성공으로 답한다. */
	int ret = 0;

	resource_list_for_each_entry(entry, &host->dma_ranges) {
		/* Each range entry corresponds to an inbound mapping region */
		ret = iproc_pcie_setup_ib(pcie, entry, IPROC_PCIE_IB_MAP_MEM);
		if (ret)
			/* [한국어] 하나라도 실패하면 멈춘다(윗줄에서 setup_ib 를 불렀다).
			 * 종류를 MEM 으로 고정하므로 I/O 전용인 IARR0 은 이 경로로 선택되지 않는다. */
			break;
	}

	return ret;
}

/* [한국어]
 * iproc_pcie_invalidate_mapping - 부트로더가 남긴 주소 창 설정을 모두 지운다
 *
 * @pcie: 컨트롤러.   @return: 없음.
 *
 * 커널이 창을 설정하기 전에 백지 상태를 만든다. 부트로더나 이전 커널이
 * 남긴 창이 살아 있으면 커널이 만든 창과 겹쳐 엉뚱한 주소로 트랜잭션이
 * 나갈 수 있기 때문이다.
 *
 * 바깥 창은 OARR 에 0 을 써서 valid 비트를 지우고, 안쪽 영역은 IARR 에
 * 0 을 써서 크기 비트를 모두 지운다. 각각 이미 비어 있으면 건너뛴다.
 *
 * need_ob_cfg / need_ib_cfg 플래그를 먼저 보는 이유는, 창을 쓰지 않는
 * 변종(PAXC 계열)에서는 해당 레지스터 자체가 없어 훑을 표도 없기 때문이다.
 *
 * 주소 상위 워드나 IMAP 은 지우지 않는다. valid/크기 비트가 내려가면
 * 창이 동작하지 않으므로 나머지 값은 무의미해지기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  iproc_pcie_setup() → [이 함수] → iproc_pcie_write_reg()
 */
static void iproc_pcie_invalidate_mapping(struct iproc_pcie *pcie)
{
	struct iproc_pcie_ib *ib = &pcie->ib;
	struct iproc_pcie_ob *ob = &pcie->ob;
	/* [한국어] 창/영역 반복자. */
	int idx;

	if (pcie->ep_is_internal)
		/* [한국어] 창을 쓰지 않는 변종이면 지울 것도 없다. */
		return;

	if (pcie->need_ob_cfg) {
		/* iterate through all OARR mapping regions */
		for (idx = ob->nr_windows - 1; idx >= 0; idx--) {
			iproc_pcie_write_reg(pcie,
					     MAP_REG(IPROC_PCIE_OARR0, idx), 0);
		}
	}

	if (pcie->need_ib_cfg) {
		/* iterate through all IARR mapping regions */
		for (idx = 0; idx < ib->nr_regions; idx++) {
			iproc_pcie_write_reg(pcie,
					     MAP_REG(IPROC_PCIE_IARR0, idx), 0);
		}
	}
}

/* [한국어]
 * iproce_pcie_get_msi - DT 에서 MSI 수신 주소를 읽어 온다
 *
 * @pcie:     컨트롤러.
 * @msi_node: msi-parent 가 가리키는 DT 노드.
 * @msi_addr: 읽은 주소를 담을 곳.
 * @return: 0 성공, -ENODEV 는 노드가 이 컨트롤러 것이 아닌 경우,
 *          그 밖에는 of_address_to_resource() 의 errno.
 *
 * MSI 쓰기를 어느 주소로 보낼지 알아내는 함수다. 그 주소를 아래
 * iproc_pcie_msi_steer() 가 하드웨어에 배선한다.
 *
 * 먼저 msi_node 가 이 컨트롤러 자신의 노드인지 확인한다. iProc 의 내장
 * MSI 컨트롤러는 PCIe 컨트롤러와 같은 DT 노드를 공유하므로, 노드가
 * 다르면 외부 MSI 컨트롤러(GICv3 ITS 등)를 쓰는 구성이라는 뜻이다.
 *
 * 주소는 0번 자원에서 가져온다 - 즉 레지스터 블록의 물리 주소 자체가
 * MSI 수신 주소가 된다.
 *
 * 함수 이름의 "iproce" 는 오타로 보이지만 상류 코드 그대로다.
 * 코드는 고치지 않고 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  iproc_pcie_msi_steer() → [이 함수] → of_address_to_resource()
 */
static int iproce_pcie_get_msi(struct iproc_pcie *pcie,
			       struct device_node *msi_node,
			       u64 *msi_addr)
{
	struct device *dev = pcie->dev;
	int ret;
	/* [한국어] DT 0번 자원. 레지스터 블록의 물리 주소가 곧 MSI 수신 주소가 된다. */
	struct resource res;

	/*
	 * Check if 'msi-map' points to ARM GICv3 ITS, which is the only
	 * supported external MSI controller that requires steering.
	 */
	if (!of_device_is_compatible(msi_node, "arm,gic-v3-its")) {
		dev_err(dev, "unable to find compatible MSI controller\n");
		/* [한국어] msi_node 가 이 컨트롤러 자신의 노드가 아니면 외부 MSI 컨트롤러를 쓰는
		 * 구성이라는 뜻이다. iProc 내장 MSI 는 PCIe 컨트롤러와 DT 노드를 공유한다. */
		return -ENODEV;
	}

	/* derive GITS_TRANSLATER address from GICv3 */
	ret = of_address_to_resource(msi_node, 0, &res);
	if (ret < 0) {
		/* [한국어] 0번 자원을 얻지 못하면 수신 주소를 정할 수 없다. */
		dev_err(dev, "unable to obtain MSI controller resources\n");
		/* [한국어] errno 를 그대로 전한다. */
		return ret;
	}

	*msi_addr = res.start + GITS_TRANSLATER;
	return 0;
}

/* [한국어]
 * iproc_pcie_paxb_v2_msi_steer - PAXB v2 에서 MSI 쓰기가 통과할 안쪽 창을 연다
 *
 * @pcie:     컨트롤러.
 * @msi_addr: MSI 수신 주소.
 * @return: 0 성공, iproc_pcie_setup_ib() 의 errno.
 *
 * PAXB v2 에서는 MSI 도 결국 안쪽 방향 메모리 쓰기다. 그러므로 장치가
 * msi_addr 로 쓴 것이 컨트롤러를 통과하려면 그 주소를 덮는 안쪽 창이
 * 있어야 한다.
 *
 * 임시 resource 를 꾸며 iproc_pcie_setup_ib() 에 넘기는 방식이 요령이다.
 * 크기를 SZ_32K 로 잡는데, 이것이 IARR0 의 유일한 지원 크기(32KB)와
 * 일치한다 - 그래서 항상 IARR0 이 선택된다.
 *
 * 종류를 IPROC_PCIE_IB_MAP_IO 로 지정하는 것도 같은 이유다. IARR0 만
 * I/O 종류이므로, 이 지정이 곧 "IARR0 을 써라" 는 뜻이 된다. DDR 을
 * 매핑하는 dma-ranges 경로와 영역이 겹치지 않게 하는 장치인 셈이다.
 *
 * 주소를 ALIGN_DOWN 으로 32KB 경계에 맞추는 것은 setup_ib 의 정렬 검사를
 * 통과하기 위해서다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  iproc_pcie_msi_steer() → [이 함수] → iproc_pcie_setup_ib()
 */
static int iproc_pcie_paxb_v2_msi_steer(struct iproc_pcie *pcie, u64 msi_addr)
{
	int ret;
	struct resource_entry entry;
/* [한국어] 임시 자원 항목. setup_ib 가 resource_entry 를 받도록 되어 있어 형식을 맞춘다. */

	memset(&entry, 0, sizeof(entry));
	/* [한국어] 항목이 자기 안의 자원을 가리키게 연결한다. */
	entry.res = &entry.__res;
/* [한국어] offset 은 0 이라 PCI 주소와 AXI 주소가 같아진다. */

	msi_addr &= ~(SZ_32K - 1);
	/* [한국어] MSI 수신 주소를 32KB 경계로 내려 시작점으로 삼는다(윗줄에서 정렬했다). */
	entry.res->start = msi_addr;
	/* [한국어] 크기를 32KB 로 잡는다. 이것이 IARR0 의 유일한 지원 크기와 일치해
	 * 항상 그 영역이 선택된다. */
	entry.res->end = msi_addr + SZ_32K - 1;

	ret = iproc_pcie_setup_ib(pcie, &entry, IPROC_PCIE_IB_MAP_IO);
	/* [한국어] 종류를 IO 로 지정했으므로(윗줄) IARR0 만 후보가 된다.
	 * DDR 을 매핑하는 dma-ranges 경로와 영역이 겹치지 않게 하는 장치다. */
	return ret;
}

/* [한국어]
 * iproc_pcie_paxc_v2_msi_steer - PAXC v2 에서 MSI 를 내부 컨트롤러나 GICv3 ITS 로 보낸다
 *
 * @pcie:      컨트롤러.
 * @msi_addr:  MSI 수신 주소.
 * @enable:    true 면 켜고, false 면 끈다.
 * @return: 없음.
 *
 * PAXB v2 와 달리 PAXC v2 에는 MSI 전용 레지스터 묶음이 있어, 안쪽 창을
 * 쓰지 않고 그 레지스터들로 직접 배선한다.
 *
 * 끄는 경로는 단순하다. MSI_EN_CFG 와 MSI_GIC_MODE 를 0 으로 만들고 끝낸다.
 *
 * 켜는 경로는 다섯 단계다.
 *   1) GIC_V3_CFG 를 세워 GICv3 모드로 둔다. 이 비트를 항상 세우는데,
 *      enum 주석이 밝히듯 MSI 를 외부 컨트롤러로 넘기기 위한 설정이다.
 *   2) MSI 수신 창의 기준 주소를 msi_addr 로 잡는다.
 *   3) 창 크기를 SZ_32K 로 잡는다. 이 크기의 근거는 이 트리에서 확인하지
 *      못했다.
 *   4) 실제 쓰기가 향할 주소를 지정한다. GICv3 ITS 를 쓸 때는
 *      GITS_TRANSLATER 레지스터의 주소여야 하며, 상류 주석과 enum 정의가
 *      그 점을 밝힌다. msi_addr 에 GITS_TRANSLATER 오프셋을 더해 만든다.
 *   5) MSI_ENABLE_CFG 로 개통한다.
 *
 * 반환값이 없어 실패를 알릴 수 없다. 레지스터 쓰기만 하고 확인하지 않는
 * 구조라 실패할 지점 자체가 없다는 판단으로 보인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  iproc_pcie_msi_steer() → [이 함수] → iproc_pcie_write_reg()
 */
static void iproc_pcie_paxc_v2_msi_steer(struct iproc_pcie *pcie, u64 msi_addr,
					 bool enable)
{
	u32 val;

	if (!enable) {
		/*
		 * Disable PAXC MSI steering. All write transfers will be
		 * treated as non-MSI transfers
		 */
		val = iproc_pcie_read_reg(pcie, IPROC_PCIE_MSI_EN_CFG);
		val &= ~MSI_ENABLE_CFG;
		/* [한국어] MSI 활성 비트를 내려 개통을 끊는다(윗줄에서 비트를 지웠다). */
		iproc_pcie_write_reg(pcie, IPROC_PCIE_MSI_EN_CFG, val);
		/* [한국어] 끄는 경로는 여기서 끝난다 — GIC 모드도 함께 0 으로 만든 뒤다. */
		return;
	}

	/*
	 * Program bits [43:13] of address of GITS_TRANSLATER register into
	 * bits [30:0] of the MSI base address register.  In fact, in all iProc
	 * based SoCs, all I/O register bases are well below the 32-bit
	 * boundary, so we can safely assume bits [43:32] are always zeros.
	 */
	iproc_pcie_write_reg(pcie, IPROC_PCIE_MSI_BASE_ADDR,
			     (u32)(msi_addr >> 13));

	/* use a default 8K window size */
	iproc_pcie_write_reg(pcie, IPROC_PCIE_MSI_WINDOW_SIZE, 0);

	/* steering MSI to GICv3 ITS */
	val = iproc_pcie_read_reg(pcie, IPROC_PCIE_MSI_GIC_MODE);
	val |= GIC_V3_CFG;
	/* [한국어] GICv3 모드 비트를 세운다(윗줄에서 값을 만들었다).
	 * MSI 를 외부 GICv3 ITS 로 넘길 수 있게 하는 설정이다. */
	iproc_pcie_write_reg(pcie, IPROC_PCIE_MSI_GIC_MODE, val);
/* [한국어] 아래에서 수신 창과 목적지 주소를 차례로 지정한다. */

	/*
	 * Program bits [43:2] of address of GITS_TRANSLATER register into the
	 * iProc MSI address registers.
	 */
	msi_addr >>= 2;
	iproc_pcie_write_reg(pcie, IPROC_PCIE_MSI_ADDR_HI,
			     /* [한국어] MSI 수신 창의 기준 주소 상위 워드. */
			     upper_32_bits(msi_addr));
	iproc_pcie_write_reg(pcie, IPROC_PCIE_MSI_ADDR_LO,
			     /* [한국어] 하위 워드. 이 창에 들어오는 쓰기가 MSI 로 해석된다. */
			     lower_32_bits(msi_addr));

	/* enable MSI */
	val = iproc_pcie_read_reg(pcie, IPROC_PCIE_MSI_EN_CFG);
	val |= MSI_ENABLE_CFG;
	/* [한국어] 마지막으로 MSI_ENABLE_CFG 를 세워 개통한다(윗줄에서 값을 만들었다).
	 * 순서가 중요하다 — 주소를 다 정한 뒤에 켜야 엉뚱한 곳으로 가지 않는다. */
	iproc_pcie_write_reg(pcie, IPROC_PCIE_MSI_EN_CFG, val);
}

/* [한국어]
 * iproc_pcie_msi_steer - 변종에 맞는 방식으로 MSI 쓰기 경로를 배선한다
 *
 * @pcie:     컨트롤러.
 * @msi_node: msi-parent 가 가리키는 DT 노드.
 * @return: 0 성공, -EINVAL 은 배선 방법을 모르는 변종,
 *          그 밖에는 하위 호출의 errno.
 *
 * MSI 는 결국 특정 주소로의 메모리 쓰기이므로, 그 쓰기가 컨트롤러를
 * 통과해 올바른 곳에 닿도록 하드웨어를 설정해야 한다. 그 방식이 변종마다
 * 달라 이 함수가 갈림길 역할을 한다.
 *
 *   PAXB_V2  - 안쪽 창(IARR0)을 열어 MSI 쓰기를 통과시킨다.
 *   PAXC_V2  - 전용 MSI 레지스터로 목적지를 직접 지정한다.
 *   그 외    - 배선 방법이 정의돼 있지 않아 -EINVAL.
 *
 * 주소를 먼저 읽는 순서가 중요하다. iproce_pcie_get_msi() 가 -ENODEV 를
 * 돌려주면 외부 MSI 컨트롤러를 쓰는 구성이라는 뜻이므로 배선 없이
 * 그대로 실패를 전하고, 호출자가 그 뒤 iproc_msi_init() 으로 넘어간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  iproc_pcie_msi_enable() → [이 함수]
 *               → iproce_pcie_get_msi() → paxb_v2 / paxc_v2 배선 함수
 */
static int iproc_pcie_msi_steer(struct iproc_pcie *pcie,
				struct device_node *msi_node)
{
	struct device *dev = pcie->dev;
	int ret;
	/* [한국어] DT 에서 읽어 올 MSI 수신 주소. */
	u64 msi_addr;

	ret = iproce_pcie_get_msi(pcie, msi_node, &msi_addr);
	/* [한국어] 주소를 얻지 못했으면 */
	if (ret < 0) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "msi steering failed\n");
		/* [한국어] errno 를 전한다. -ENODEV 면 외부 MSI 컨트롤러를 쓰는 구성이라는 뜻이다. */
		return ret;
	}

	switch (pcie->type) {
	/* [한국어] PAXB v2 는 안쪽 창을 열어 MSI 쓰기를 통과시킨다. */
	case IPROC_PCIE_PAXB_V2:
		/* [한국어] IARR0 을 노려 32KB 창을 연다. */
		ret = iproc_pcie_paxb_v2_msi_steer(pcie, msi_addr);
		if (ret)
			/* [한국어] 실패하면 그대로 전한다. */
			return ret;
		break;
	case IPROC_PCIE_PAXC_V2:
		/* [한국어] PAXC v2 는 전용 레지스터로 목적지를 직접 지정한다. 반환값이 없어
		 * 실패를 알릴 수 없다 — 레지스터 쓰기만 하고 확인하지 않기 때문이다. */
		iproc_pcie_paxc_v2_msi_steer(pcie, msi_addr, true);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/* [한국어]
 * iproc_pcie_msi_enable - MSI 를 쓸 수 있게 준비한다
 *
 * @pcie: 컨트롤러.
 * @return: 0 성공. -ENODEV 는 msi-parent 가 없는 경우,
 *          그 밖에는 배선 또는 iproc_msi_init() 의 errno.
 *
 * 세 단계다.
 *   1) DT 에서 msi-parent phandle 을 따라가 MSI 컨트롤러 노드를 얻는다.
 *      없으면 이 시스템은 MSI 를 쓰지 않는다는 뜻이라 -ENODEV.
 *   2) need_msi_steer 가 선 변종이면 하드웨어 배선을 한다. 실패하면
 *      여기서 멈춘다.
 *   3) iproc_msi_init()(:1367) 으로 내장 MSI 컨트롤러 드라이버를 띄운다.
 *
 * 3번의 실패를 호출자가 어떻게 다루는지가 이 함수의 설계 의도를 보여 준다.
 * 상류 주석이 그 자리에 적어 두었듯, 다른 MSI 컨트롤러(GICv3 ITS 등)를
 * 쓰는 시스템에서는 이 호출이 실패하는 것이 정상이다. 그래서
 * iproc_pcie_setup() 은 이 함수의 실패를 치명적으로 다루지 않고 경고만
 * 남기고 진행한다.
 *
 * of_node_put 을 위한 out_put_node 라벨이 있어, 어느 경로로 빠져나가도
 * 노드 참조가 반납된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  iproc_pcie_setup() → [이 함수]
 *               → iproc_pcie_msi_steer() → iproc_msi_init() [pcie-iproc-msi.c]
 */
static int iproc_pcie_msi_enable(struct iproc_pcie *pcie)
{
	struct device_node *msi_node = NULL;
	int ret;
/* [한국어] msi-parent phandle 을 따라 MSI 컨트롤러 노드를 얻는다. */

	/*
	 * Either the "msi-parent" or the "msi-map" phandle needs to exist
	 * for us to obtain the MSI node.
	 */
	of_msi_xlate(pcie->dev, &msi_node, 0);
	if (!msi_node)
		/* [한국어] 없으면 이 시스템은 MSI 를 쓰지 않는다는 뜻이다. */
		return -ENODEV;

	/*
	 * Certain revisions of the iProc PCIe controller require additional
	 * configurations to steer the MSI writes towards an external MSI
	 * controller.
	 */
	if (pcie->need_msi_steer) {
		ret = iproc_pcie_msi_steer(pcie, msi_node);
		/* [한국어] 배선이 필요한 변종에서 배선이 실패했으면 */
		if (ret)
			/* [한국어] 노드 참조를 반납하고 물러난다. */
			goto out_put_node;
	}

	/*
	 * If another MSI controller is being used, the call below should fail
	 * but that is okay
	 */
	ret = iproc_msi_init(pcie, msi_node);

out_put_node:
	of_node_put(msi_node);
	return ret;
}

/* [한국어]
 * iproc_pcie_msi_disable - 내장 MSI 컨트롤러를 정리한다
 *
 * @pcie: 컨트롤러.   @return: 없음.
 *
 * iproc_msi_exit()(:1376) 을 부르는 한 줄이다. 감싸는 이유는 이름의 대칭
 * (enable/disable)을 맞추고, 나중에 배선 해제 같은 단계가 늘어나도
 * 호출부를 고치지 않게 하려는 것으로 보인다.
 *
 * 배선 자체는 되돌리지 않는다. iproc_pcie_msi_steer() 가 연 안쪽 창이나
 * PAXC v2 의 MSI 레지스터는 그대로 남는다. 장치가 제거되는 시점이라
 * 문제가 되지 않는다는 판단으로 보인다.
 *
 * MSI 가 애초에 켜지지 않았어도 안전하다 - iproc_msi_exit() 이
 * pcie->msi 가 NULL 인 경우를 처리한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(remove).
 *
 * 호출 체인:  iproc_pcie_remove() → [이 함수] → iproc_msi_exit() [pcie-iproc-msi.c]
 */
static void iproc_pcie_msi_disable(struct iproc_pcie *pcie)
{
	iproc_msi_exit(pcie);
}

/* [한국어]
 * iproc_pcie_rev_init - 다섯 변종의 차이를 표와 플래그로 흡수한다
 *
 * @pcie: 결합 드라이버가 type 까지 채워 넘긴 컨트롤러.
 * @return: 0 성공, -EINVAL 은 모르는 변종, -ENOMEM 은 표 할당 실패.
 *
 * 이 파일을 읽을 때 가장 먼저 볼 함수다. 이후의 모든 코드가 변종 이름을
 * 직접 보지 않고 여기서 세운 플래그와 표만 보기 때문에, 각 변종이 무엇을
 * 할 수 있는지가 이 switch 문에 다 드러난다.
 *
 * 변종별로 정해지는 것을 정리하면
 *   PAXB_BCMA - 오프셋 표만. 창도 MSI 배선도 없다. 가장 단순한 변종이다.
 *   PAXB      - APB 오류 끄기 가능. 바깥 창 2개.
 *   PAXB_V2   - 전용 config 읽기(RRS 재시도), APB 오류 끄기, 바깥 창 4개,
 *               안쪽 영역 5개, MSI 배선 필요. 가장 기능이 많다.
 *               CFG_RETRY_STATUS 값을 읽으면 잘못된 데이터가 돌아온다는
 *               경고를 여기서 한 번 찍어 둔다 - iproc_pcie_cfg_retry() 의
 *               한계를 사용자에게 알리는 것이다.
 *   PAXC      - 내부 엔드포인트(링크 학습·PERST 없음), 전용 config 읽기,
 *               설정되지 않은 PF 거부.
 *   PAXC_V2   - PAXC 와 같고 MSI 배선이 추가된다.
 *
 * 바깥 창 설정을 need_ob_cfg 로 감싸는 이유는, 그 플래그를 결합 드라이버가
 * DT 를 보고 미리 세워 두기 때문이다. 하드웨어가 창을 지원해도 DT 가
 * 쓰지 않겠다고 하면 표를 붙이지 않는다.
 *
 * 마지막 블록이 오프셋 표를 만드는 부분이다. 변종별 static 배열을 그대로
 * 쓰지 않고 devm 으로 사본을 만드는데, 값이 0 인 항목을
 * IPROC_PCIE_REG_INVALID 로 바꿔 넣기 위해서다. 그래야
 * iproc_pcie_read_reg()/write_reg() 가 "이 변종에 없는 레지스터" 를
 * 구분할 수 있다.
 *
 * 0번 항목만 따로 다루는 것이 요점이다. IPROC_PCIE_CLK_CTRL 의 오프셋이
 * 실제로 0x000 이라 0 을 "없음" 으로 바꾸는 규칙을 그대로 적용할 수 없다.
 * 그래서 PAXC_V2(이 레지스터가 없는 유일한 변종)만 명시적으로 INVALID 로
 * 두고, 나머지는 표의 값을 그대로 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  iproc_pcie_setup() → [이 함수] → devm_kcalloc()
 */
static int iproc_pcie_rev_init(struct iproc_pcie *pcie)
{
	struct device *dev = pcie->dev;
	unsigned int reg_idx;
	/* [한국어] 고를 오프셋 표의 포인터. 아래 switch 가 변종별로 정한다. */
	const u16 *regs;
/* [한국어] 이 switch 문이 다섯 변종의 능력 차이를 한눈에 보여 준다. */

	switch (pcie->type) {
	/* [한국어] 가장 단순한 변종. 창도 MSI 도 없다. */
	case IPROC_PCIE_PAXB_BCMA:
		/* [한국어] 오프셋 표만 고르고 끝낸다. */
		regs = iproc_pcie_reg_paxb_bcma;
		break;
	case IPROC_PCIE_PAXB:
		/* [한국어] PAXB 는 바깥 창 두 쌍과 APB 오류 제어를 갖는다. */
		regs = iproc_pcie_reg_paxb;
		pcie->has_apb_err_disable = true;
		/* [한국어] DT 가 바깥 창을 쓰겠다고 했을 때만 표를 붙인다. */
		if (pcie->need_ob_cfg) {
			/* [한국어] 창 두 개짜리 능력 표. */
			pcie->ob_map = paxb_ob_map;
			/* [한국어] 창 개수도 표 크기에서 가져온다. */
			pcie->ob.nr_windows = ARRAY_SIZE(paxb_ob_map);
		/* [한국어] 바깥 창 설정 끝. */
		}
		break;
	case IPROC_PCIE_PAXB_V2:
		/* [한국어] PAXB v2 가 기능이 가장 많다. */
		regs = iproc_pcie_reg_paxb_v2;
		pcie->iproc_cfg_read = true;
		/* [한국어] config 접근 시 APB 오류 전달을 끌 수 있다. */
		pcie->has_apb_err_disable = true;
		/* [한국어] 바깥 창을 쓰겠다고 했으면 */
		if (pcie->need_ob_cfg) {
			/* [한국어] 창 네 개짜리 능력 표를 붙인다. */
			pcie->ob_map = paxb_v2_ob_map;
			/* [한국어] 뒤의 두 창은 최대 1024MB 까지 지원한다. */
			pcie->ob.nr_windows = ARRAY_SIZE(paxb_v2_ob_map);
		/* [한국어] 바깥 창 설정 끝. */
		}
		pcie->ib.nr_regions = ARRAY_SIZE(paxb_v2_ib_map);
		/* [한국어] 안쪽 영역 표도 붙인다(윗줄에서 개수를 정했다). 이 변종만 안쪽 창을 쓴다. */
		pcie->ib_map = paxb_v2_ib_map;
		/* [한국어] MSI 를 안쪽 창으로 배선해야 하는 변종이다. */
		pcie->need_msi_steer = true;
		/* [한국어] config 읽기의 한계를 부팅 로그로 미리 알린다. */
		dev_warn(dev, "reads of config registers that contain %#x return incorrect data\n",
			 /* [한국어] 이 값을 읽으면 잘못된 데이터가 돌아온다는 경고다 —
			  * iproc_pcie_cfg_retry() 가 감당하지 못하는 경우를 사용자에게 알려 준다. */
			 CFG_RETRY_STATUS);
		break;
	case IPROC_PCIE_PAXC:
		/* [한국어] PAXC v1 은 내부 에뮬레이트 엔드포인트 전용이다. */
		regs = iproc_pcie_reg_paxc;
		pcie->ep_is_internal = true;
		/* [한국어] 전용 config 읽기 경로를 쓴다(capability 고치기 때문). */
		pcie->iproc_cfg_read = true;
		/* [한국어] 설정되지 않은 PF 를 거부한다. 그런 PF 에 쓰면 내장 프로세서가 멈춘다. */
		pcie->rej_unconfig_pf = true;
		/* [한국어] PAXC v1 설정 끝. 창이 하나도 없다 — 주소 변환이 필요 없기 때문이다. */
		break;
	case IPROC_PCIE_PAXC_V2:
		/* [한국어] PAXC v2 는 v1 에 MSI 배선이 더해진 형태다. */
		regs = iproc_pcie_reg_paxc_v2;
		pcie->ep_is_internal = true;
		/* [한국어] 전용 config 읽기. */
		pcie->iproc_cfg_read = true;
		/* [한국어] 설정되지 않은 PF 거부. */
		pcie->rej_unconfig_pf = true;
		/* [한국어] MSI 배선 필요. 다만 v1 과 달리 전용 MSI 레지스터를 쓴다. */
		pcie->need_msi_steer = true;
		/* [한국어] PAXC v2 설정 끝. */
		break;
	default:
		dev_err(dev, "incompatible iProc PCIe interface\n");
		return -EINVAL;
	}

	pcie->reg_offsets = devm_kcalloc(dev, IPROC_PCIE_MAX_NUM_REG,
					 /* [한국어] 항목 하나의 크기. */
					 sizeof(*pcie->reg_offsets),
					 GFP_KERNEL);
	if (!pcie->reg_offsets)
		/* [한국어] 표 사본을 만들 메모리가 없으면 진행할 수 없다. */
		return -ENOMEM;

	/* go through the register table and populate all valid registers */
	pcie->reg_offsets[0] = (pcie->type == IPROC_PCIE_PAXC_V2) ?
		IPROC_PCIE_REG_INVALID : regs[0];
	for (reg_idx = 1; reg_idx < IPROC_PCIE_MAX_NUM_REG; reg_idx++)
		/* [한국어] 1번 항목부터는 규칙이 단순하다 — 값이 0 이면 */
		pcie->reg_offsets[reg_idx] = regs[reg_idx] ?
			/* [한국어] "이 변종에 없음" 으로 바꿔 넣는다. 그래야 read_reg/write_reg 가
			 * 없는 레지스터를 구분할 수 있다. 0번만 따로 다루는 이유는
			 * IPROC_PCIE_CLK_CTRL 의 오프셋이 실제로 0x000 이라 이 규칙을 쓸 수 없어서다. */
			regs[reg_idx] : IPROC_PCIE_REG_INVALID;

	return 0;
}

/* [한국어]
 * iproc_pcie_setup - 이 파일의 진입점. 변종 판정부터 버스 스캔까지 엮는다
 *
 * @pcie: 결합 드라이버가 dev/type/base/base_addr/map_irq 를 채워 넘긴 구조체.
 * @res:  바깥 창으로 만들 자원 목록(브리지의 windows).
 * @return: 0 성공, 음수 errno 실패.
 *
 * 두 결합 드라이버(platform, bcma)가 각자 버스에서 자원을 긁어모은 뒤
 * 공통으로 부르는 함수다. 여기서부터는 버스 종류와 무관한 공통 경로다.
 *
 * 순서와 그 이유:
 *   1) rev_init - 변종별 표와 플래그를 세운다. 이것이 없으면 아래의
 *      레지스터 접근이 전부 무의미하므로 가장 먼저다.
 *   2) PHY 초기화와 전원 켜기. 링크를 세우려면 물리 계층이 살아 있어야 한다.
 *   3) PERST 를 걸었다 푼다. 엔드포인트를 알려진 상태에서 출발시킨다.
 *      PAXC 는 perst_ctrl 안에서 통째로 건너뛴다.
 *   4) 부트로더가 남긴 창 설정을 지운다.
 *   5) 바깥 창과 안쪽 영역을 연다. 각각 need_*_cfg 플래그가 설 때만 한다.
 *      안쪽만 -ENOENT 를 봐주는데, dma-ranges 가 DT 에 없는 것은 정상이기
 *      때문이다. 바깥 창은 없으면 장치에 접근할 수 없어 실패로 다룬다.
 *   6) 링크가 실제로 섰는지 확인한다. 여기서 실패하면 엔드포인트가
 *      없는 것이다.
 *   7) INTx 를 켠다.
 *   8) MSI 를 준비한다. 실패해도 진행한다 - 상류 코드가 오류가 아니라
 *      정보 수준 로그만 남기는 것이 그 뜻이며, 다른 MSI 컨트롤러를 쓰는
 *      시스템일 수 있기 때문이다.
 *   9) 브리지에 ops/sysdata/map_irq 를 꽂고 pci_host_probe() 로 넘긴다.
 *      sysdata 에 넣은 pcie 를 config 콜백이 iproc_data() 로 되찾는다.
 *  10) 열거가 끝난 뒤 루트 포트마다 링크 상태를 찍는다.
 *
 * 에러 경로가 PHY 두 단계뿐인 것은 나머지가 devm 할당이거나 되돌릴 필요가
 * 없는 하드웨어 설정이기 때문이다.
 *
 * EXPORT_SYMBOL 로 공개되며, 이 트리에서 확인한 호출자는
 * pcie-iproc-platform.c:107 과 pcie-iproc-bcma.c:340 둘이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). 링크 학습에서 잠든다.
 *
 * 호출 체인:  결합 드라이버의 probe → [이 함수] → iproc_pcie_rev_init()
 *               → iproc_pcie_perst_ctrl() → iproc_pcie_map_ranges()
 *               → iproc_pcie_check_link() → iproc_pcie_msi_enable()
 *               → pci_host_probe() [drivers/pci/probe.c]
 */
int iproc_pcie_setup(struct iproc_pcie *pcie, struct list_head *res)
{
	struct device *dev;
	int ret;
	/* [한국어] 열거가 끝난 뒤 링크 상태를 찍을 때 쓸 반복자. */
	struct pci_dev *pdev;
	/* [한국어] private 영역 주소에서 그것을 품은 브리지를 역산한다. */
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie);

	dev = pcie->dev;
/* [한국어] 변종별 표와 플래그를 먼저 세운다(윗줄). 이것이 없으면 아래의
 * 레지스터 접근이 전부 무의미하므로 가장 먼저다. */

	ret = iproc_pcie_rev_init(pcie);
	/* [한국어] 변종 판정에 실패했으면 */
	if (ret) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "unable to initialize controller parameters\n");
		/* [한국어] 물러난다. 아직 아무것도 켜지 않았으므로 되돌릴 것이 없다. */
		return ret;
	}

	ret = phy_init(pcie->phy);
	/* [한국어] PHY 초기화에 실패했으면 */
	if (ret) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "unable to initialize PCIe PHY\n");
		/* [한국어] 물러난다. */
		return ret;
	}

	ret = phy_power_on(pcie->phy);
	/* [한국어] PHY 전원 켜기에 실패했으면 */
	if (ret) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "unable to power on PCIe PHY\n");
		/* [한국어] phy_init 만 되돌리는 경로로. */
		goto err_exit_phy;
	}

	iproc_pcie_perst_ctrl(pcie, true);
	/* [한국어] 리셋을 풀어 엔드포인트가 링크 학습을 시작하게 한다(윗줄에서 걸었다).
	 * PAXC 는 perst_ctrl 안에서 통째로 건너뛴다. */
	iproc_pcie_perst_ctrl(pcie, false);

	iproc_pcie_invalidate_mapping(pcie);

	if (pcie->need_ob_cfg) {
		/* [한국어] DT 의 메모리 창들을 바깥 창으로 옮긴다. */
		ret = iproc_pcie_map_ranges(pcie, res);
		/* [한국어] 실패했으면 */
		if (ret) {
			/* [한국어] 그 사실을 남기고 */
			dev_err(dev, "map failed\n");
			/* [한국어] PHY 를 되돌리는 경로로. 바깥 창이 없으면 장치에 접근할 수 없다. */
			goto err_power_off_phy;
		}
	}

	if (pcie->need_ib_cfg) {
		/* [한국어] dma-ranges 를 안쪽 영역으로 옮긴다. */
		ret = iproc_pcie_map_dma_ranges(pcie);
		/* [한국어] -ENOENT 만 봐준다 — dma-ranges 가 DT 에 없는 것은 정상이기 때문이다. */
		if (ret && ret != -ENOENT)
			/* [한국어] 그 밖의 실패는 PHY 를 되돌리는 경로로. */
			goto err_power_off_phy;
	}

	ret = iproc_pcie_check_link(pcie);
	/* [한국어] 링크가 실제로 섰는지 확인해 실패했으면 */
	if (ret) {
		/* [한국어] 엔드포인트가 없다는 뜻이다. */
		dev_err(dev, "no PCIe EP device detected\n");
		/* [한국어] PHY 를 되돌리는 경로로. */
		goto err_power_off_phy;
	}

	iproc_pcie_enable(pcie);

	if (IS_ENABLED(CONFIG_PCI_MSI))
		/* [한국어] MSI 준비에 실패해도 */
		if (iproc_pcie_msi_enable(pcie))
			/* [한국어] 오류가 아니라 정보 수준 로그만 남기고 진행한다. 다른 MSI 컨트롤러를
			 * 쓰는 시스템일 수 있기 때문이다. */
			dev_info(dev, "not using iProc MSI\n");
/* [한국어] 아래에서 브리지에 콜백들을 꽂는다. */

	host->ops = &iproc_pcie_ops;
	/* [한국어] config 콜백이 iproc_data() 로 되찾을 드라이버 상태를 심는다. */
	host->sysdata = pcie;
	/* [한국어] 결합 드라이버가 준비한 INTx 사상 함수를 그대로 넘긴다. */
	host->map_irq = pcie->map_irq;
/* [한국어] 준비가 끝났으니 PCI 코어에 열거를 맡긴다. */

	ret = pci_host_probe(host);
	/* [한국어] 버스 스캔에 실패했으면 */
	if (ret < 0) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "failed to scan host: %d\n", ret);
		/* [한국어] PHY 를 되돌리는 경로로. */
		goto err_power_off_phy;
	}

	for_each_pci_bridge(pdev, host->bus) {
		/* [한국어] 루트 포트인 브리지마다(윗줄에서 순회를 시작했다) */
		if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT)
			/* [한국어] 협상된 링크 속도와 폭을 로그로 남긴다. 사용자가 링크가 기대대로
			 * 올라왔는지 확인할 수 있게 하는 정보다. */
			pcie_print_link_status(pdev);
	}

	return 0;

err_power_off_phy:
	phy_power_off(pcie->phy);
err_exit_phy:
	phy_exit(pcie->phy);
	return ret;
}
EXPORT_SYMBOL(iproc_pcie_setup);

/* [한국어]
 * iproc_pcie_remove - 버스를 걷어내고 PHY 를 끈다
 *
 * @pcie: 컨트롤러.   @return: 없음.
 *
 * setup 의 역순이다.
 *   1) pci_stop_root_bus() 로 아래 장치들의 드라이버를 떼어 내고,
 *   2) pci_remove_root_bus() 로 버스 자료구조를 없앤다.
 *      두 단계로 나뉜 것은 커널 PCI 코어의 규약이다 - 멈추는 것과
 *      제거하는 것을 분리해야 드라이버가 안전하게 정리된다.
 *   3) MSI 컨트롤러를 정리하고,
 *   4) PHY 전원을 끄고 종료한다.
 *
 * 순서가 중요하다. 장치들을 먼저 떼어 내야 그 드라이버들이 자기 MSI 벡터를
 * 반납하고, 그 뒤에 MSI 컨트롤러를 없앨 수 있다. 반대로 하면 아직 쓰이는
 * IRQ 도메인을 없애게 된다.
 *
 * setup 에서 연 주소 창들은 지우지 않는다. PHY 를 끄면 링크 자체가
 * 사라지므로 창이 남아 있어도 트랜잭션이 나갈 수 없기 때문이다.
 *
 * EXPORT_SYMBOL 로 공개되며, 호출자는 pcie-iproc-platform.c:121 과
 * pcie-iproc-bcma.c:377 이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(remove).
 *
 * 호출 체인:  결합 드라이버의 remove → [이 함수]
 *               → pci_stop_root_bus() → iproc_pcie_msi_disable() → phy_exit()
 */
void iproc_pcie_remove(struct iproc_pcie *pcie)
{
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie);

	pci_stop_root_bus(host->bus);
	pci_remove_root_bus(host->bus);

	iproc_pcie_msi_disable(pcie);

	phy_power_off(pcie->phy);
	phy_exit(pcie->phy);
}
EXPORT_SYMBOL(iproc_pcie_remove);

/*
 * The MSI parsing logic in certain revisions of Broadcom PAXC based root
 * complex does not work and needs to be disabled
 */
/* [한국어]
 * quirk_paxc_disable_msi_parsing - PAXC 아래 장치의 MSI 파싱을 끈다
 *
 * @pdev: quirk 대상 장치.   @return: 없음.
 *
 * PAXC 컨트롤러 아래에 붙은 내부 에뮬레이트 엔드포인트에 적용되는 fixup 이다.
 *
 * MSI_ENABLE_CFG 를 0 으로 만들어 컨트롤러가 MSI 쓰기를 해석하지 않게 한다.
 * 이 변종에서는 MSI 처리가 다른 경로로 이뤄지거나 아예 쓰이지 않는데,
 * 컨트롤러가 임의의 메모리 쓰기를 MSI 로 오인하면 문제가 되기 때문으로
 * 보인다. 다만 그 구체적 사정은 이 트리의 코드와 주석만으로는 확인하지
 * 못했다.
 *
 * 브리지가 아니라 그 아래 장치에 걸리는 fixup 이므로, 대상 장치의
 * 버스를 거슬러 올라가 컨트롤러를 찾아야 한다. iproc_data() 가 그 일을 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치 열거 중 fixup).
 *
 * 호출 체인:  pci_fixup_device(early) → [이 함수] → iproc_pcie_write_reg()
 */
static void quirk_paxc_disable_msi_parsing(struct pci_dev *pdev)
{
	struct iproc_pcie *pcie = iproc_data(pdev->bus);

	if (pdev->hdr_type == PCI_HEADER_TYPE_BRIDGE)
		/* [한국어] PAXC v2 면 MSI 배선까지 함께 끈다. v1 은 그 레지스터가 없어
		 * iproc_pcie_write_reg() 가 조용히 무시한다. */
		iproc_pcie_paxc_v2_msi_steer(pcie, 0, false);
/* [한국어] 함수 끝. */
}
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0x16f0,
			/* [한국어] Broadcom 0xd802 에 이 fixup 을 건다(윗줄이 벤더 ID). */
			quirk_paxc_disable_msi_parsing);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0xd802,
			/* [한국어] 0xd804 에도 같은 fixup 을 건다. 둘 다 PAXC 아래의 내부 엔드포인트다. */
			quirk_paxc_disable_msi_parsing);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0xd804,
			quirk_paxc_disable_msi_parsing);

/* [한국어]
 * quirk_paxc_bridge - PAXC 루트 포트의 클래스 코드와 버스 번호를 손본다
 *
 * @pdev: quirk 대상 브리지.   @return: 없음.
 *
 * PAXC 의 에뮬레이트된 루트 포트가 커널에 제대로 보이도록 두 가지를 고친다.
 *
 * 첫째, 클래스 코드를 PCI_CLASS_BRIDGE_PCI_NORMAL 로 강제한다. 하드웨어에
 * 잘못 박혀 있어 커널이 이 장치를 브리지로 인식하지 못하기 때문이다.
 * iproc_pcie_check_link() 가 PAXB 에 대해 같은 일을 하는 것과 짝을 이룬다 -
 * PAXC 는 링크 확인 경로를 건너뛰므로 여기서 따로 해 주는 것이다.
 *
 * 둘째, subordinate 버스 번호를 0xff 로 넓힌다. 상류 주석이 이유를 밝히는데,
 * PAXC 아래 장치들의 버스 번호가 펌웨어 구성에 따라 달라져 커널이 미리
 * 알 수 없으므로, 범위를 최대로 열어 두어 어떤 번호가 나와도 열거되게 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치 열거 중 fixup).
 *
 * 호출 체인:  pci_fixup_device(early) → [이 함수]
 */
static void quirk_paxc_bridge(struct pci_dev *pdev)
{
	/*
	 * The PCI config space is shared with the PAXC root port and the first
	 * Ethernet device.  So, we need to workaround this by telling the PCI
	 * code that the bridge is not an Ethernet device.
	 */
	if (pdev->hdr_type == PCI_HEADER_TYPE_BRIDGE)
		pdev->class = PCI_CLASS_BRIDGE_PCI_NORMAL;
/* [한국어] subordinate 버스 번호를 0xff 로 넓힌다. 상류 주석대로 PAXC 아래 장치의
 * 버스 번호가 펌웨어 구성에 따라 달라 커널이 미리 알 수 없으므로,
 * 범위를 최대로 열어 어떤 번호가 나와도 열거되게 한다. */

	/*
	 * MPSS is not being set properly (as it is currently 0).  This is
	 * because that area of the PCI config space is hard coded to zero, and
	 * is not modifiable by firmware.  Set this to 2 (e.g., 512 byte MPS)
	 * so that the MPS can be set to the real max value.
	 */
	pdev->pcie_mpss = 2;
}
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0x16cd, quirk_paxc_bridge);
/* [한국어] 아래 넷이 이 fixup 의 대상이다. 0x16f0 부터 시작한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0x16f0, quirk_paxc_bridge);
/* [한국어] 0xd750. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0xd750, quirk_paxc_bridge);
/* [한국어] 0xd802. 위 disable_msi_parsing fixup 과 대상이 겹치는데,
 * 그쪽은 브리지 아래 장치이고 이쪽은 브리지 자신이라 역할이 다르다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0xd802, quirk_paxc_bridge);
/* [한국어] 0xd804. EARLY 단계여야 하는 이유는 PCI 코어가 클래스 코드를 보고
 * 이 장치를 브리지로 다룰지 정하기 전에 고쳐야 하기 때문이다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0xd804, quirk_paxc_bridge);

MODULE_AUTHOR("Ray Jui <rjui@broadcom.com>");
MODULE_DESCRIPTION("Broadcom iPROC PCIe common driver");
MODULE_LICENSE("GPL v2");
