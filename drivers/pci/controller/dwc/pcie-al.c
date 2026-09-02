// SPDX-License-Identifier: GPL-2.0
/*
 * [한국어 설명] Amazon Annapurna Labs Alpine SoC 의 PCIe 지원 (pcie-al.c)
 *
 * === 파일의 역할 ===
 * 한 파일 안에 **서로 독립적인 두 드라이버가 들어 있다.** 그 둘이
 * #ifdef 로 나뉘어 있고 공유하는 코드가 하나도 없다는 점이 이 파일을
 * 읽을 때 가장 먼저 알아야 할 사실이다.
 *
 *   1) 앞쪽(CONFIG_ACPI && CONFIG_PCI_QUIRKS) — ACPI 로 부팅하는 시스템에서
 *      쓰는 ECAM 우회다. 이 하드웨어는 루트 포트의 config 공간이 표준 ECAM
 *      자리에 없고 별도의 DBI 창에 있어, 루트 버스 접근만 그쪽으로 돌린다.
 *      나머지는 일반 ECAM 그대로다. 진입점이 al_pcie_ops 라는 이름의
 *      pci_ecam_ops 표 하나이며, ACPI 코어가 그것을 찾아 쓴다.
 *   2) 뒤쪽(CONFIG_PCIE_AL) — 디바이스 트리로 부팅하는 시스템에서 쓰는
 *      온전한 DesignWare 글루 드라이버다. 컨트롤러 리비전을 읽어 레지스터
 *      배치를 정하고, ECAM 창과 대상 버스 레지스터를 맞물려 config 접근을
 *      성립시킨다.
 *
 * 두 갈래 모두가 다루는 근본 문제는 같다 — **ECAM 창이 256버스를 다 담을
 * 만큼 크지 않을 수 있다는 것** 이다. 그래서 뒤쪽 드라이버는 버스 번호를
 * 둘로 쪼갠다: 하위 비트는 ECAM 오프셋으로 쓰고, 상위 비트는 컨트롤러의
 * "대상 버스" 레지스터에 써 둔다. 접근할 버스가 바뀌어 상위 비트가 달라질
 * 때만 그 레지스터를 다시 쓴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * ACPI 경로:
 *   ACPI 코어가 MCFG 를 보고 이 세그먼트의 ops 로 al_pcie_ops 를 고른다
 *     -> al_pcie_init() 이 ACPI 자원에서 DBI 창을 얻어 매핑
 *     -> config 접근마다 al_pcie_map_bus() 가 루트 버스만 DBI 로 돌린다
 *
 * 디바이스 트리 경로:
 *   플랫폼 드라이버 코어 -> al_pcie_probe()
 *     -> "config"(ECAM)과 "controller" 자원을 얻는다
 *     -> dw_pcie_host_init()  [pcie-designware-host.c]
 *        -> 그 안에서 콜백 -> [이 파일] al_pcie_host_init()
 *           -> 자식 버스용 pci_ops 를 갈아 끼운다
 *           -> 리비전을 읽어 레지스터 오프셋을 정한다
 *           -> 대상 버스와 secondary/subordinate 를 설정한다
 *        -> 버스 스캔. 자식 버스 접근이 al_pcie_conf_addr_map_bus() 로 온다
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. 다만 config 접근 경로
 * (al_pcie_conf_addr_map_bus)는 PCI 코어가 잠금을 쥔 채 부르는 자리라
 * 잠들 수 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 앞쪽 갈래는 ACPI 의 PCI 루트 브리지 코드가, 뒤쪽 갈래는
 *   PCI 코어와 DWC 호스트 코어가 쓴다.
 * 아래쪽: 앞쪽은 pci-ecam 계층(pci_ecam_map_bus 등), 뒤쪽은
 *   pcie-designware-host.c 와 pcie-designware.c.
 * 옆쪽: 없다. 두 갈래가 서로를 부르지 않는다.
 *
 * 데이터 흐름:
 *   버스 번호 -> (ecam_mask 로 나눈) 하위 비트 -> ECAM 창 안의 오프셋
 *             -> (reg_mask 로 나눈) 상위 비트 -> 대상 버스 레지스터
 *   이 두 갈래가 만나 하나의 config 주소를 이룬다.
 *
 * 공유 상태: 뒤쪽 갈래의 struct al_pcie 하나. **target_bus_cfg.reg_val 만이
 *   런타임에 바뀌는 필드** 이며, config 접근 경로가 그것을 읽고 쓴다.
 *   그 경로가 PCI 코어의 잠금 아래에서만 불린다는 전제로 별도 잠금이 없다.
 *
 * === NVMe 관점 ===
 * 이 브리지 아래에 NVMe 컨트롤러가 붙으면, nvme_probe() 의 config 접근이
 * 모두 al_pcie_conf_addr_map_bus() 를 지난다. 그 장치가 루트 버스가 아닌
 * 하위 버스에 있으면 대상 버스 레지스터가 그 버스로 맞춰져 있어야 하고,
 * 다른 버스의 장치를 번갈아 접근하면 그 레지스터가 매번 다시 쓰인다.
 * 열거가 끝난 뒤의 BAR 접근은 이 경로를 지나지 않으므로 그 비용은
 * 열거 시점에만 든다.
 *
 * === 주요 함수/구조체 요약 ===
 * al_pcie_map_bus()             : ACPI 갈래. 루트 버스만 DBI 창으로 돌린다.
 * al_pcie_init()                : ACPI 갈래. ACPI 자원에서 DBI 창을 얻는다.
 * al_pcie_conf_addr_map_bus()   : DT 갈래. 버스 번호를 두 갈래로 쪼개
 *                                 필요할 때만 대상 버스 레지스터를 갱신한다.
 * al_pcie_config_prepare()      : 그 쪼갬의 마스크를 계산하고 초기값을 쓴다.
 * al_pcie_rev_id_get()          : 장치 ID 에서 컨트롤러 리비전을 알아낸다.
 * al_pcie_reg_offsets_set()     : 그 리비전으로 레지스터 배치를 정한다.
 * struct al_pcie                : DT 갈래의 상태. dw_pcie 를 포인터로 든다.
 * struct al_pcie_target_bus_cfg : 버스 번호 쪼갬의 마스크와 현재 값.
 */

/*
 * PCIe host controller driver for Amazon's Annapurna Labs IP (used in chips
 * such as Graviton and Alpine)
 *
 * Copyright 2019 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Author: Jonathan Chocron <jonnyc@amazon.com>
 */

/* [한국어] PCI 코어 타입들. */
#include <linux/pci.h>
/* [한국어] pci_ecam_ops 와 pci_ecam_map_bus(), PCIE_ECAM_OFFSET 매크로.
 * 이 파일의 두 갈래가 모두 ECAM 을 다룬다. */
#include <linux/pci-ecam.h>
/* [한국어] acpi_get_rc_resources() 와 acpi_pci_root. 앞쪽 갈래만 쓴다. */
#include <linux/pci-acpi.h>
/* [한국어] drivers/pci 안에서만 쓰는 선언들. */
#include "../../pci.h"

/* [한국어] **여기부터 ACPI 갈래다.** ACPI 로 부팅하고 쿼크가 켜진 빌드에서만
 * 컴파일된다 — 이 우회 자체가 하드웨어의 비표준 배치를 메우는 쿼크이기 때문이다. */
#if defined(CONFIG_ACPI) && defined(CONFIG_PCI_QUIRKS)

/* [한국어] ACPI 갈래가 ECAM 창 서술에 매달아 두는 사설 데이터. */
struct al_pcie_acpi  {
	/* [한국어] 루트 포트의 DBI 창 주소.
	 * 설정자: al_pcie_init() 이 ACPI 자원에서 얻어 매핑한 값을 담는다.
	 * 읽는 자: al_pcie_map_bus() 가 루트 버스 접근마다 꺼내 쓴다.
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: 초기화 후 불변. */
	void __iomem *dbi_base;
};

/* [한국어]
 * al_pcie_map_bus - ACPI 갈래에서 루트 버스 접근만 DBI 창으로 돌린다
 *
 * @bus: 대상 버스.
 * @devfn: 장치·기능 번호.
 * @where: 레지스터 오프셋.
 * @return: 접근할 주소, 대상이 아니면 NULL.
 *
 * 이 하드웨어의 루트 포트는 config 공간이 표준 ECAM 자리에 없고 별도의
 * DBI 창에 있다. ACPI 로 부팅하는 시스템에서는 그 사실을 알릴 방법이 없어,
 * 이 우회가 필요하다.
 *
 * 루트 버스가 아니면 일반 ECAM 계산에 그대로 넘긴다 — 하위 버스들은
 * 표준대로 동작하기 때문이다.
 *
 * 루트 버스에서 슬롯 0 이 아니면 NULL 이다. 루트 포트가 하나뿐이라
 * 그 자리에만 장치가 있고, 나머지는 없는 것으로 답해야 코어가 헛되이
 * 탐색하지 않는다.
 *
 * 실행 컨텍스트: config 접근 경로. PCI 코어가 잠금을 쥔 채 부르므로
 * 잠들지 않는다.
 *
 * 에러 경로: 없다. 대상이 아니면 NULL 이며 코어가 그것을 없는 장치로 읽는다.
 *
 * 호출 체인:
 *   PCI 코어의 config 접근 → pci_generic_config_read/write()
 *     → pci_ecam_ops.pci_ops.map_bus == [이 함수] → pci_ecam_map_bus()
 */
static void __iomem *al_pcie_map_bus(struct pci_bus *bus, unsigned int devfn,
				     int where)
{
	struct pci_config_window *cfg = bus->sysdata;
	struct al_pcie_acpi *pcie = cfg->priv;
	/* [한국어] ACPI 초기화 때 매달아 둔 DBI 창 주소를 꺼낸다. */
	void __iomem *dbi_base = pcie->dbi_base;
/* [한국어] 이제 어느 창으로 보낼지 정한다. */

	if (bus->number == cfg->busr.start) {
		/*
		 * The DW PCIe core doesn't filter out transactions to other
		 * devices/functions on the root bus num, so we do this here.
		 */
		if (PCI_SLOT(devfn) > 0)
			return NULL;
		else
			return dbi_base + where;
	/* [한국어] 루트 버스 처리 끝. 아래로 내려가면 일반 ECAM 경로다. */
	}

	return pci_ecam_map_bus(bus, devfn, where);
}

/* [한국어]
 * al_pcie_init - ACPI 갈래에서 DBI 창을 찾아 매핑한다
 *
 * @cfg: ECAM 창 서술. 여기에 사설 데이터를 매단다.
 * @return: 0 = 성공, -ENOMEM 또는 자원 조회 오류.
 *
 * ACPI 코어가 이 세그먼트의 ECAM 을 준비할 때 한 번 부른다.
 *
 * DBI 창의 주소를 ACPI 에서 얻는 것이 이 함수의 핵심이다.
 * acpi_get_rc_resources() 가 "AMZN0001" 이라는 ACPI HID 로 그 자원을
 * 찾는데, 그 HID 가 이 하드웨어의 루트 컴플렉스를 가리키는 약속이다.
 *
 * 세그먼트 번호를 함께 넘긴다 — 시스템에 루트 브리지가 여럿이면
 * 각각의 DBI 창이 따로 있기 때문이다.
 *
 * 매핑한 주소를 cfg->priv 에 매달아, 위 al_pcie_map_bus() 가 config
 * 접근마다 그것을 되찾아 쓴다.
 *
 * devm_pci_remap_cfg_resource() 를 쓰는 것이 요점이다. 일반 ioremap 이
 * 아니라 config 공간용 매핑이라, 아키텍처에 따라 다른 메모리 속성이 적용된다.
 *
 * 실행 컨텍스트: ACPI 의 PCI 루트 브리지 준비. 프로세스 컨텍스트.
 *
 * 에러 경로: 할당 실패는 -ENOMEM, ACPI 자원 조회 실패는 그 오류를
 * 어느 세그먼트였는지와 함께 기록하고 올려보낸다.
 *
 * 호출 체인:
 *   ACPI PCI 루트 브리지 코드 → pci_ecam_ops.init == [이 함수]
 *     → acpi_get_rc_resources() → devm_pci_remap_cfg_resource()
 */
static int al_pcie_init(struct pci_config_window *cfg)
{
	struct device *dev = cfg->parent;
	struct acpi_device *adev = to_acpi_device(dev);
	/* [한국어] 그 ACPI 장치에 매달린 PCI 루트 정보. 세그먼트 번호가 여기 있다. */
	struct acpi_pci_root *root = acpi_driver_data(adev);
	/* [한국어] DBI 창 주소를 담을 사설 구조. */
	struct al_pcie_acpi *al_pcie;
	/* [한국어] ACPI 에서 얻을 자원 서술. */
	struct resource *res;
	/* [한국어] 각 단계의 결과. */
	int ret;

	al_pcie = devm_kzalloc(dev, sizeof(*al_pcie), GFP_KERNEL);
	/* [한국어] 사설 구조를 잡지 못하면, */
	if (!al_pcie)
		/* [한국어] 메모리 부족으로 물러난다. */
		return -ENOMEM;

	res = devm_kzalloc(dev, sizeof(*res), GFP_KERNEL);
	/* [한국어] 자원 구조를 잡지 못하면, */
	if (!res)
		/* [한국어] 역시 메모리 부족이다. */
		return -ENOMEM;

	ret = acpi_get_rc_resources(dev, "AMZN0001", root->segment, res);
	/* [한국어] ACPI 자원 조회가 실패하면, */
	if (ret) {
		/* [한국어] 어느 세그먼트였는지와 함께 남기고 — 시스템에 루트 브리지가 여럿이면
		 * 각각의 DBI 창이 따로 있어 세그먼트를 밝혀야 추적이 된다. */
		dev_err(dev, "can't get rc dbi base address for SEG %d\n",
			/* [한국어] 그 세그먼트 번호를 함께 찍는다. */
			root->segment);
		return ret;
	}

	dev_dbg(dev, "Root port dbi res: %pR\n", res);
/* [한국어] 찾은 자원의 범위를 디버그 기록에 남긴다. */

	al_pcie->dbi_base = devm_pci_remap_cfg_resource(dev, res);
	/* [한국어] 매핑이 실패하면, */
	if (IS_ERR(al_pcie->dbi_base))
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(al_pcie->dbi_base);

	cfg->priv = al_pcie;
/* [한국어] **ECAM 창 서술에 매달아 둔다** — config 접근마다 al_pcie_map_bus() 가
 * 이것을 되찾아 쓴다. */

	return 0;
}

const struct pci_ecam_ops al_pcie_ops = {
	/* [한국어] 이 세그먼트의 ECAM 을 준비할 때 한 번 불린다. */
	.init         =  al_pcie_init,
	/* [한국어] config 접근 함수 표. */
	.pci_ops      = {
		.map_bus    = al_pcie_map_bus,
		/* [한국어] 읽기와 쓰기는 일반 ECAM 함수를 그대로 쓴다 — 주소 계산만 위에서 가로챈다. */
		.read       = pci_generic_config_read,
		.write      = pci_generic_config_write,
	}
};

/* [한국어] ACPI 갈래 끝. 아래 DT 갈래와 공유하는 코드가 하나도 없다. */
#endif /* defined(CONFIG_ACPI) && defined(CONFIG_PCI_QUIRKS) */

/* [한국어] **여기부터 DT 갈래다.** 위 ACPI 갈래와 독립적이며,
 * 한 빌드에 둘 다 들어갈 수도 있고 하나만 들어갈 수도 있다. */
#ifdef CONFIG_PCIE_AL

/* [한국어] of_pci 헬퍼. */
#include <linux/of_pci.h>
/* [한국어] DWC 코어의 선언 전부 — struct dw_pcie, dw_pcie_host_init() 등. */
#include "pcie-designware.h"

/* [한국어] Alpine v2 세대의 리비전 번호. */
#define AL_PCIE_REV_ID_2	2
/* [한국어] v3 세대. */
#define AL_PCIE_REV_ID_3	3
/* [한국어] v4 세대. 아래 장치 ID 값이 이 셋 중 하나로 대응된다. */
#define AL_PCIE_REV_ID_4	4

/* [한국어] AXI 레지스터 영역의 기준 오프셋. 0 이라 실질적으로는 컨트롤러 창의
 * 시작과 같지만, 계산식에 명시해 두어 구조를 드러낸다. */
#define AXI_BASE_OFFSET		0x0

/* [한국어] 장치 ID 블록의 오프셋. */
#define DEVICE_ID_OFFSET	0x16c

/* [한국어] 그 블록 안의 장치·리비전 레지스터. */
#define DEVICE_REV_ID			0x0
/* [한국어] 그 레지스터에서 장치 ID 가 차지하는 상위 16비트. */
#define DEVICE_REV_ID_DEV_ID_MASK	GENMASK(31, 16)

/* [한국어] x4 레인 — 리비전 2 에 대응한다. */
#define DEVICE_REV_ID_DEV_ID_X4		0
/* [한국어] x8 — 리비전 3. */
#define DEVICE_REV_ID_DEV_ID_X8		2
/* [한국어] x16 — 리비전 4. **장치 ID 값이 곧 레인 수를 뜻하며**,
 * 세대가 올라가며 레인 수도 함께 늘어난 내력이 그 대응에 남아 있다. */
#define DEVICE_REV_ID_DEV_ID_X16	4

/* [한국어] 리비전 1~2 의 outbound 제어 블록 오프셋. */
#define OB_CTRL_REV1_2_OFFSET	0x0040
/* [한국어] 리비전 3~5 의 오프셋. 세대가 달라도 3 과 4 는 같은 자리를 쓴다. */
#define OB_CTRL_REV3_5_OFFSET	0x0030

/* [한국어] outbound 블록 안의 대상 버스 레지스터. */
#define CFG_TARGET_BUS			0x0
/* [한국어] 그 안에서 마스크가 차지하는 하위 8비트. */
#define CFG_TARGET_BUS_MASK_MASK	GENMASK(7, 0)
/* [한국어] 버스 번호가 차지하는 다음 8비트. 컨트롤러가 그 마스크로
 * '번호의 어느 비트를 여기서 가져올지' 를 안다. */
#define CFG_TARGET_BUS_BUSNUM_MASK	GENMASK(15, 8)

/* [한국어] outbound 블록 안의 제어 레지스터. */
#define CFG_CONTROL			0x4
/* [한국어] subordinate 버스 번호가 차지하는 자리. */
#define CFG_CONTROL_SUBBUS_MASK		GENMASK(15, 8)
/* [한국어] secondary 버스 번호가 차지하는 자리. 두 필드가 이 브리지 아래
 * 버스 범위를 컨트롤러에 알린다. */
#define CFG_CONTROL_SEC_BUS_MASK	GENMASK(23, 16)

struct al_pcie_reg_offsets {
	/* [한국어] outbound 제어 블록의 오프셋.
	 * 설정자: al_pcie_reg_offsets_set() 이 리비전을 보고 정한다.
	 * 읽는 자: 대상 버스와 CFG_CONTROL 접근이 이 값을 기준으로 삼는다.
	 * 값 범위: 리비전 2 는 0x40, 3~4 는 0x30.
	 * 동기화: host_init 후 불변. */
	unsigned int ob_ctrl;
/* [한국어] 리비전에 따라 달라지는 레지스터 배치. */
};

struct al_pcie_target_bus_cfg {
	/* [한국어] 대상 버스 레지스터에 **지금 들어 있는** 값.
	 * 설정자: al_pcie_config_prepare() 가 초기값을, al_pcie_conf_addr_map_bus()
	 * 가 버스가 바뀔 때마다 갱신한다.
	 * 읽는 자: 같은 두 함수.
	 * 값 범위: 버스 번호의 상위 비트.
	 * 동기화: **이 드라이버에서 런타임에 바뀌는 유일한 상태** 이며 별도 잠금이 없다 —
	 * config 접근을 PCI 코어가 직렬화한다는 전제다. */
	u8 reg_val;
	/* [한국어] 버스 번호에서 레지스터 쪽으로 갈 비트를 고르는 마스크.
	 * 설정자: al_pcie_config_prepare() 가 ECAM 마스크의 보수로 만든다.
	 * 읽는 자: al_pcie_conf_addr_map_bus().
	 * 값 범위: 아래 ecam_mask 와 합치면 8비트 전체가 된다.
	 * 동기화: host_init 후 불변. */
	u8 reg_mask;
	/* [한국어] 버스 번호에서 ECAM 오프셋으로 갈 비트를 고르는 마스크.
	 * 설정자: al_pcie_config_prepare() 가 창 크기에서 계산한다.
	 * 읽는 자: al_pcie_conf_addr_map_bus().
	 * 값 범위: 창이 담을 수 있는 버스 수 - 1.
	 * 동기화: host_init 후 불변. */
	u8 ecam_mask;
/* [한국어] 버스 번호 쪼갬의 마스크와 현재 값. 이 구조체 하나가 이 파일의 핵심 장치다. */
};

struct al_pcie {
	/* [한국어] DWC 코어가 다루는 부분. **포인터로 든다** — 값으로 품는 글루들과 달라,
	 * 이 구조체 포인터가 dw_pcie 포인터가 되지 않는다.
	 * 설정자: probe 가 따로 할당해 매단다.
	 * 읽는 자: 이 파일의 모든 DT 갈래 함수.
	 * 값 범위: 유효한 dw_pcie 포인터.
	 * 동기화: probe 후 불변. */
	struct dw_pcie *pci;
	/* [한국어] Annapurna 전용 컨트롤러 창(옆의 상류 주석이 DW 코어의 것이 아님을 밝힌다).
	 * 설정자: probe 의 devm_ioremap_resource("controller").
	 * 읽는 자: al_pcie_controller_readl()/writel().
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 후 불변. */
	void __iomem *controller_base; /* base of PCIe unit (not DW core) */
	/* [한국어] 로그에 쓸 device. pci->dev 와 같은 값이지만 따로 들고 있다.
	 * 설정자: probe.
	 * 읽는 자: 오류를 남기는 함수들.
	 * 값 범위: 유효한 device 포인터.
	 * 동기화: probe 후 불변. */
	struct device *dev;
	/* [한국어] ECAM 창의 크기.
	 * 설정자: probe 가 자원에서 읽는다. **창을 매핑하지는 않는다** — 매핑은
	 * DWC 코어가 하고, 이 파일은 크기만 쓴다.
	 * 읽는 자: al_pcie_config_prepare() 가 버스 마스크를 계산하는 근거로 쓴다.
	 * 값 범위: 유효한 크기. 256MB 를 넘으면 잘라 낸다.
	 * 동기화: probe 후 불변. */
	resource_size_t ecam_size;
	/* [한국어] 알아낸 컨트롤러 리비전.
	 * 설정자: al_pcie_rev_id_get().
	 * 읽는 자: al_pcie_reg_offsets_set().
	 * 값 범위: 2, 3, 4 중 하나.
	 * 동기화: host_init 후 불변. */
	unsigned int controller_rev_id;
	/* [한국어] 그 리비전에 맞는 레지스터 오프셋들.
	 * 설정자: al_pcie_reg_offsets_set().
	 * 읽는 자: 컨트롤러 창 접근 전부.
	 * 값 범위: 위 구조체 참조.
	 * 동기화: host_init 후 불변. */
	struct al_pcie_reg_offsets reg_offsets;
	/* [한국어] 버스 번호 쪼갬의 마스크와 현재 값.
	 * 설정자: al_pcie_config_prepare() 와 al_pcie_conf_addr_map_bus().
	 * 읽는 자: 같은 두 함수.
	 * 값 범위: 위 구조체 참조.
	 * 동기화: reg_val 만 런타임에 바뀌며 잠금이 없다. */
	struct al_pcie_target_bus_cfg target_bus_cfg;
/* [한국어] DT 갈래의 상태 전부. */
};

/* [한국어] dw_pcie 포인터에서 이 드라이버의 상태를 되찾는 통로.
 * container_of 가 아니라 drvdata 인 것은, 이 드라이버가 dw_pcie 를
 * **포인터로** 들고 있어 두 구조체가 한 덩어리가 아니기 때문이다.
 * 전제: probe 가 platform_set_drvdata 로 상태를 미리 심어 두어야 한다. */
#define to_al_pcie(x)		dev_get_drvdata((x)->dev)

/* [한국어]
 * al_pcie_controller_readl - 컨트롤러 전용 레지스터를 읽는다
 *
 * @pcie: 드라이버 상태.
 * @offset: 컨트롤러 창 안의 오프셋.
 * @return: 읽은 값.
 *
 * DWC 코어의 DBI 창이 아니라 **Annapurna 가 따로 둔 컨트롤러 창** 을
 * 읽는다(구조체 필드의 상류 주석이 그 구분을 밝힌다).
 *
 * 리비전 판별과 대상 버스 설정이 이 창을 통해 이뤄진다.
 *
 * _relaxed 판인 것은 이 읽기가 다른 메모리 접근과 순서를 맞출 필요가
 * 없기 때문이다.
 *
 * 실행 컨텍스트: host_init 과 config 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   al_pcie_rev_id_get() / al_pcie_config_prepare() → [이 함수]
 *     → readl_relaxed()
 */
static inline u32 al_pcie_controller_readl(struct al_pcie *pcie, u32 offset)
{
	return readl_relaxed(pcie->controller_base + offset);
}

/* [한국어]
 * al_pcie_controller_writel - 컨트롤러 전용 레지스터에 쓴다
 *
 * @pcie: 드라이버 상태.
 * @offset: 컨트롤러 창 안의 오프셋.
 * @val: 쓸 값.
 *
 * al_pcie_controller_readl() 의 짝이다.
 *
 * 인자 순서가 읽기 쪽과 뒤집혀 있다는 점에 주의할 만하다 — 오프셋이
 * 값보다 앞에 온다. 커널의 writel(값, 주소) 관용과 반대라 헷갈리기 쉽다.
 *
 * 실행 컨텍스트: host_init 과 config 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   al_pcie_target_bus_set() / al_pcie_config_prepare() → [이 함수]
 *     → writel_relaxed()
 */
static inline void al_pcie_controller_writel(struct al_pcie *pcie, u32 offset,
					     u32 val)
{
	writel_relaxed(val, pcie->controller_base + offset);
}

/* [한국어]
 * al_pcie_rev_id_get - 장치 ID 필드에서 컨트롤러 리비전을 알아낸다
 *
 * @pcie: 드라이버 상태.
 * @rev_id: 알아낸 리비전을 담을 자리.
 * @return: 0 = 성공, -EINVAL = 모르는 장치.
 *
 * 같은 드라이버가 Alpine v2/v3 를 모두 지원하는데, 그 세대마다 레지스터
 * 배치가 달라 먼저 어느 세대인지 알아야 한다.
 *
 * **장치 ID 값이 곧 레인 수를 뜻한다.** x4 가 리비전 2, x8 이 3, x16 이 4 로
 * 대응되는데, 세대가 올라가며 레인 수도 함께 늘어난 하드웨어의 내력이
 * 그 대응에 그대로 남아 있다.
 *
 * 모르는 값이면 -EINVAL 이다. 새 세대가 나왔는데 이 드라이버가 그것을
 * 모르는 경우이며, 그대로 진행하면 엉뚱한 오프셋을 쓰게 된다.
 *
 * 실행 컨텍스트: host_init 콜백. 프로세스 컨텍스트.
 *
 * 에러 경로: 모르는 장치 ID 는 그 값을 기록하고 -EINVAL.
 *
 * 호출 체인:
 *   al_pcie_host_init() → [이 함수] → al_pcie_controller_readl()
 */
static int al_pcie_rev_id_get(struct al_pcie *pcie, unsigned int *rev_id)
{
	u32 dev_rev_id_val;
	u32 dev_id_val;
/* [한국어] 장치·리비전 레지스터를 읽는다. */

	dev_rev_id_val = al_pcie_controller_readl(pcie, AXI_BASE_OFFSET +
						  /* [한국어] 세 오프셋을 더해 자리를 만든다 — AXI 기준, 장치 ID 블록, 그 안의 레지스터 순이다. */
						  DEVICE_ID_OFFSET +
						  DEVICE_REV_ID);
	dev_id_val = FIELD_GET(DEVICE_REV_ID_DEV_ID_MASK, dev_rev_id_val);
/* [한국어] 그 안에서 장치 ID 필드만 뽑는다. */

	switch (dev_id_val) {
	/* [한국어] x4 면 — */
	case DEVICE_REV_ID_DEV_ID_X4:
		*rev_id = AL_PCIE_REV_ID_2;
		break;
	case DEVICE_REV_ID_DEV_ID_X8:
		*rev_id = AL_PCIE_REV_ID_3;
		break;
	case DEVICE_REV_ID_DEV_ID_X16:
		*rev_id = AL_PCIE_REV_ID_4;
		break;
	default:
		dev_err(pcie->dev, "Unsupported dev_id_val (0x%x)\n",
			dev_id_val);
		return -EINVAL;
	}

	dev_dbg(pcie->dev, "dev_id_val: 0x%x\n", dev_id_val);
/* [한국어] 알아낸 값을 디버그 기록에 남긴다. */

	return 0;
}

/* [한국어]
 * al_pcie_reg_offsets_set - 리비전에 맞는 레지스터 오프셋을 정한다
 *
 * @pcie: 드라이버 상태.
 * @return: 0 = 성공, -EINVAL = 모르는 리비전.
 *
 * al_pcie_rev_id_get() 이 알아낸 리비전으로 outbound 제어 블록의 위치를
 * 정한다. 세대마다 그 블록이 다른 자리에 있어, 이후의 모든 대상 버스
 * 접근이 이 값을 기준으로 삼는다.
 *
 * 리비전 3 과 4 가 같은 오프셋을 쓴다 — 세대가 달라도 이 블록의 배치는
 * 바뀌지 않았다는 뜻이다.
 *
 * 기본 갈래가 있는 것이 방어적이다. 위 함수가 이미 리비전을 검증했으므로
 * 여기 오지 않아야 하지만, 두 함수가 따로 있어 어긋날 여지를 남겨 두지 않는다.
 *
 * 실행 컨텍스트: host_init 콜백. 프로세스 컨텍스트.
 *
 * 에러 경로: 모르는 리비전은 그 값을 기록하고 -EINVAL.
 *
 * 호출 체인:
 *   al_pcie_host_init() → [이 함수]
 */
static int al_pcie_reg_offsets_set(struct al_pcie *pcie)
{
	switch (pcie->controller_rev_id) {
	case AL_PCIE_REV_ID_2:
		/* [한국어] 리비전 2 는 0x40 자리를 쓴다. */
		pcie->reg_offsets.ob_ctrl = OB_CTRL_REV1_2_OFFSET;
		break;
	case AL_PCIE_REV_ID_3:
	/* [한국어] 리비전 4 도 — */
	case AL_PCIE_REV_ID_4:
		pcie->reg_offsets.ob_ctrl = OB_CTRL_REV3_5_OFFSET;
		break;
	default:
		dev_err(pcie->dev, "Unsupported controller rev_id: 0x%x\n",
			pcie->controller_rev_id);
		return -EINVAL;
	}

	return 0;
}

/* [한국어]
 * al_pcie_target_bus_set - 대상 버스 레지스터에 버스 번호와 마스크를 쓴다
 *
 * @pcie: 드라이버 상태.
 * @target_bus: 접근할 버스 번호의 상위 비트.
 * @mask_target_bus: 그 상위 비트를 고르는 마스크.
 *
 * 이 파일의 핵심 장치를 실제로 조작하는 함수다.
 *
 * ECAM 창이 256버스를 다 담을 만큼 크지 않을 수 있어, 버스 번호를 둘로
 * 쪼갠다. 하위 비트는 ECAM 창 안의 오프셋이 되고, 상위 비트는 이
 * 레지스터에 써 둔다 — 컨트롤러가 그 둘을 합쳐 실제 버스 번호를 만든다.
 *
 * 마스크를 함께 쓰는 것이 요점이다. 컨트롤러가 그 마스크로 "번호의 어느
 * 비트를 이 레지스터에서 가져올지" 를 알기 때문이다.
 *
 * 한 레지스터에 두 필드를 함께 쓰므로 읽기-수정-쓰기가 필요 없다 —
 * 그 레지스터의 다른 비트를 이 드라이버가 쓰지 않는다.
 *
 * 실행 컨텍스트: host_init 과 config 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   al_pcie_config_prepare() / al_pcie_conf_addr_map_bus() → [이 함수]
 *     → al_pcie_controller_writel()
 */
static inline void al_pcie_target_bus_set(struct al_pcie *pcie,
					  u8 target_bus,
					  u8 mask_target_bus)
{
	u32 reg;

	reg = FIELD_PREP(CFG_TARGET_BUS_MASK_MASK, mask_target_bus) |
	      /* [한국어] 버스 번호를 그 자리에 넣는다. 두 필드를 한 값으로 합쳐 한 번에 쓴다. */
	      FIELD_PREP(CFG_TARGET_BUS_BUSNUM_MASK, target_bus);
/* [한국어] 이제 대상 버스 레지스터에 쓴다. */

	al_pcie_controller_writel(pcie, AXI_BASE_OFFSET +
				  /* [한국어] 리비전에 따라 정해진 outbound 블록 오프셋을 기준으로 삼는다. */
				  pcie->reg_offsets.ob_ctrl + CFG_TARGET_BUS,
				  reg);
}

/* [한국어]
 * al_pcie_conf_addr_map_bus - 자식 버스 config 접근의 주소를 만든다
 *
 * @bus: 대상 버스.
 * @devfn: 장치·기능 번호.
 * @where: 레지스터 오프셋.
 * @return: 접근할 주소.
 *
 * 이 파일에서 가장 자주 불리는 함수이며, 버스 번호 쪼개기가 실제로
 * 작동하는 자리다.
 *
 * 버스 번호를 두 마스크로 나눈다 — ECAM 쪽 하위 비트와 레지스터 쪽 상위
 * 비트다. 그리고 **상위 비트가 지금 레지스터에 있는 값과 다를 때만**
 * 그 레지스터를 다시 쓴다. 대부분의 접근이 같은 버스를 연달아 향하므로,
 * 이 비교 덕분에 레지스터 쓰기가 드물게 일어난다.
 *
 * reg_val 을 갱신하는 것이 그 캐시다. 이 필드가 이 드라이버에서 런타임에
 * 바뀌는 유일한 상태이며, 별도 잠금이 없다 — PCI 코어가 config 접근을
 * 직렬화한다는 전제다.
 *
 * 주소 계산은 표준 ECAM 공식이되 **마스크를 씌운 버스 번호** 를 쓴다.
 * 그래야 창 크기를 넘지 않는다.
 *
 * 실행 컨텍스트: config 접근 경로. PCI 코어가 잠금을 쥔 채 부르므로
 * 잠들지 않는다.
 *
 * 에러 경로: 없다. 언제나 주소를 돌려준다.
 *
 * 호출 체인:
 *   PCI 코어의 config 접근 → pci_generic_config_read/write()
 *     → pci_ops.map_bus == [이 함수] → al_pcie_target_bus_set()
 */
static void __iomem *al_pcie_conf_addr_map_bus(struct pci_bus *bus,
					       unsigned int devfn, int where)
{
	struct dw_pcie_rp *pp = bus->sysdata;
	struct al_pcie *pcie = to_al_pcie(to_dw_pcie_from_pp(pp));
	/* [한국어] 지금 접근하려는 버스 번호. */
	unsigned int busnr = bus->number;
	/* [한국어] 쪼갬의 마스크와 현재 값을 담은 구조. */
	struct al_pcie_target_bus_cfg *target_bus_cfg = &pcie->target_bus_cfg;
	/* [한국어] **하위 비트** — ECAM 창 안의 오프셋 계산에 쓴다. */
	unsigned int busnr_ecam = busnr & target_bus_cfg->ecam_mask;
	/* [한국어] **상위 비트** — 대상 버스 레지스터에 들어갈 값이다. */
	unsigned int busnr_reg = busnr & target_bus_cfg->reg_mask;
/* [한국어] 이제 그 상위 비트가 바뀌었는지 본다. */

	if (busnr_reg != target_bus_cfg->reg_val) {
		/* [한국어] 바뀌었으면 무엇에서 무엇으로 바뀌는지 남긴다 — 이 로그가 자주 나오면
		 * 버스를 번갈아 접근하고 있다는 신호다. */
		dev_dbg(pcie->pci->dev, "Changing target bus busnum val from 0x%x to 0x%x\n",
			/* [한국어] 이전 값과 새 값을 함께 찍는다. */
			target_bus_cfg->reg_val, busnr_reg);
		target_bus_cfg->reg_val = busnr_reg;
		/* [한국어] 레지스터를 갱신한다. **바뀌었을 때만** 쓰므로, 같은 버스를 연달아
		 * 접근하는 흔한 경우에는 레지스터 쓰기가 일어나지 않는다. */
		al_pcie_target_bus_set(pcie,
				       target_bus_cfg->reg_val,
				       target_bus_cfg->reg_mask);
	}

	return pp->va_cfg0_base + PCIE_ECAM_OFFSET(busnr_ecam, devfn, where);
/* [한국어] 주소 계산 끝. 마스크를 씌운 버스 번호를 쓰므로 창 크기를 넘지 않는다. */
}

static struct pci_ops al_child_pci_ops = {
	/* [한국어] 자식 버스 접근만 이 함수로 온다 — 루트 버스는 DWC 코어의 기본 접근을 쓴다. */
	.map_bus = al_pcie_conf_addr_map_bus,
	/* [한국어] 읽기와 쓰기는 일반 함수를 그대로 쓴다. */
	.read = pci_generic_config_read,
	.write = pci_generic_config_write,
};

/* [한국어]
 * al_pcie_config_prepare - 버스 번호 쪼갬의 마스크를 계산하고 초기값을 쓴다
 *
 * @pcie: 드라이버 상태.
 * @return: 0 = 성공, -ENODEV = 버스 범위를 못 찾음.
 *
 * 위 al_pcie_conf_addr_map_bus() 가 쓸 마스크 한 벌을 여기서 만든다.
 *
 * ECAM 마스크 계산이 이 함수의 핵심이다. 창 크기를 버스당 크기로 나누면
 * 담을 수 있는 버스 수가 나오고, 거기서 1 을 빼면 그것이 곧 하위 비트
 * 마스크가 된다 — 버스당 크기가 2 의 거듭제곱이라 성립하는 계산이다.
 *
 * 256 을 넘으면 잘라 낸다. PCI 의 버스 번호가 8비트뿐이라 그보다 큰
 * 창은 남는 부분을 쓸 수 없고, 그 사실을 경고로 알린다.
 *
 * 레지스터 마스크는 ECAM 마스크의 보수다 — 두 마스크가 버스 번호 8비트를
 * 정확히 둘로 나눈다.
 *
 * secondary 를 버스 시작 + 1 로, subordinate 를 버스 끝으로 두는 것이
 * 브리지 설정의 관용이다. 루트 버스가 시작이고 그 아래가 하나씩이다.
 *
 * 그 둘을 쓰는 자리가 읽기-수정-쓰기인 것에 주의할 만하다 — 같은
 * 레지스터에 다른 제어 비트가 있어 통째로 쓸 수 없다.
 *
 * 실행 컨텍스트: host_init 콜백. 프로세스 컨텍스트.
 *
 * 에러 경로: 버스 자원을 못 찾으면 -ENODEV.
 *
 * 호출 체인:
 *   al_pcie_host_init() → [이 함수]
 *     → resource_list_first_type() → al_pcie_target_bus_set()
 *     → al_pcie_controller_readl() → al_pcie_controller_writel()
 */
static int al_pcie_config_prepare(struct al_pcie *pcie)
{
	struct al_pcie_target_bus_cfg *target_bus_cfg;
	struct dw_pcie_rp *pp = &pcie->pci->pp;
	/* [한국어] 창 크기에서 계산할 ECAM 마스크. */
	unsigned int ecam_bus_mask;
	/* [한국어] 버스 번호 범위를 담은 자원 항목. */
	struct resource_entry *ft;
	/* [한국어] CFG_CONTROL 레지스터의 오프셋. */
	u32 cfg_control_offset;
	/* [한국어] 그 자원. */
	struct resource *bus;
	/* [한국어] 이 브리지 아래의 마지막 버스 번호. */
	u8 subordinate_bus;
	/* [한국어] 바로 아래 버스 번호. */
	u8 secondary_bus;
	/* [한국어] 읽어 온 제어 값. */
	u32 cfg_control;
	/* [한국어] 고쳐 쓸 값. */
	u32 reg;
/* [한국어] 먼저 버스 번호 범위를 찾는다. */

	ft = resource_list_first_type(&pp->bridge->windows, IORESOURCE_BUS);
	/* [한국어] 찾지 못하면 — */
	if (!ft)
		/* [한국어] 쪼갬의 기준을 정할 수 없으므로 물러난다. */
		return -ENODEV;

	bus = ft->res;
	/* [한국어] 쪼갬 설정을 담을 구조를 가리킨다. */
	target_bus_cfg = &pcie->target_bus_cfg;
/* [한국어] 이제 마스크를 계산한다. */

	ecam_bus_mask = (pcie->ecam_size >> PCIE_ECAM_BUS_SHIFT) - 1;
	/* [한국어] 창이 256버스보다 크면 — */
	if (ecam_bus_mask > 255) {
		/* [한국어] PCI 의 버스 번호가 8비트뿐이라 남는 부분을 쓸 수 없다는 사실을 알리고, */
		dev_warn(pcie->dev, "ECAM window size is larger than 256MB. Cutting off at 256\n");
		/* [한국어] 8비트에 맞춰 잘라 낸다. */
		ecam_bus_mask = 255;
	}

	/* This portion is taken from the transaction address */
	target_bus_cfg->ecam_mask = ecam_bus_mask;
	/* This portion is taken from the cfg_target_bus reg */
	target_bus_cfg->reg_mask = ~target_bus_cfg->ecam_mask;
	target_bus_cfg->reg_val = bus->start & target_bus_cfg->reg_mask;
/* [한국어] 레지스터 마스크는 ECAM 마스크의 보수다 — 둘이 8비트를 정확히 나눈다. */

	al_pcie_target_bus_set(pcie, target_bus_cfg->reg_val,
			       /* [한국어] 초기값을 레지스터에 써 둔다. */
			       target_bus_cfg->reg_mask);

	secondary_bus = bus->start + 1;
	/* [한국어] 마지막 버스는 자원 범위의 끝이다. */
	subordinate_bus = bus->end;
/* [한국어] 이제 그 둘을 CFG_CONTROL 에 쓴다. */

	/* Set the valid values of secondary and subordinate buses */
	cfg_control_offset = AXI_BASE_OFFSET + pcie->reg_offsets.ob_ctrl +
			     CFG_CONTROL;

	cfg_control = al_pcie_controller_readl(pcie, cfg_control_offset);
/* [한국어] 현재 값을 읽는다 — 같은 레지스터에 다른 제어 비트가 있어 통째로 쓸 수 없다. */

	reg = cfg_control &
	      /* [한국어] 두 버스 번호 필드만 지운다. */
	      ~(CFG_CONTROL_SEC_BUS_MASK | CFG_CONTROL_SUBBUS_MASK);
/* [한국어] 그 자리에 새 값을 넣는다. */

	reg |= FIELD_PREP(CFG_CONTROL_SUBBUS_MASK, subordinate_bus) |
	       /* [한국어] secondary 도 함께 넣어 한 번에 쓴다. */
	       FIELD_PREP(CFG_CONTROL_SEC_BUS_MASK, secondary_bus);
/* [한국어] 이제 되쓴다. */

	al_pcie_controller_writel(pcie, cfg_control_offset, reg);
/* [한국어] 쪼갬 설정과 브리지 버스 번호가 모두 준비됐다. */

	return 0;
}

/* [한국어]
 * al_pcie_host_init - 자식 ops 를 갈아 끼우고 리비전과 버스 설정을 마친다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 0 = 성공, 음수 오류.
 *
 * DWC 코어가 호스트 초기화 도중에 부르는 콜백이며, 이 파일 DT 갈래의
 * 준비가 모두 여기 모여 있다.
 *
 * **자식 버스용 pci_ops 를 갈아 끼우는 것** 이 첫 줄이자 가장 중요한 부분이다.
 * 루트 버스는 DWC 코어의 기본 접근을 쓰고, 그 아래 버스만 이 파일의
 * 버스 번호 쪼개기를 거치게 만든다.
 *
 * 그 다음 셋이 순서대로 의존한다 — 리비전을 알아야 오프셋을 정할 수 있고,
 * 오프셋을 알아야 대상 버스 레지스터를 쓸 수 있다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 안. 프로세스 컨텍스트.
 *
 * 에러 경로: 각 단계의 오류를 그대로 올려보내며, 되감기가 없다 —
 * 설정만 했을 뿐 잡은 자원이 없기 때문이다.
 *
 * 호출 체인:
 *   al_pcie_probe() → dw_pcie_host_init()
 *     → dw_pcie_host_ops.init == [이 함수]
 *     → al_pcie_rev_id_get() → al_pcie_reg_offsets_set()
 *     → al_pcie_config_prepare()
 */
static int al_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct al_pcie *pcie = to_al_pcie(pci);
	/* [한국어] 각 단계의 결과. */
	int rc;
/* [한국어] 먼저 자식 버스용 접근 함수를 갈아 끼운다. */

	pp->bridge->child_ops = &al_child_pci_ops;
/* [한국어] **루트 버스는 그대로 두고 자식만 바꾼다** — 루트 버스는 DWC 코어의
 * 기본 접근으로 충분하고, 버스 번호 쪼개기는 그 아래에서만 필요하다. */

	rc = al_pcie_rev_id_get(pcie, &pcie->controller_rev_id);
	/* [한국어] 리비전을 알아내지 못하면, */
	if (rc)
		/* [한국어] 그 오류를 올려보낸다. */
		return rc;

	rc = al_pcie_reg_offsets_set(pcie);
	/* [한국어] 레지스터 오프셋을 정하지 못하면, */
	if (rc)
		/* [한국어] 그 오류를 올려보낸다. */
		return rc;

	rc = al_pcie_config_prepare(pcie);
	/* [한국어] 버스 설정이 실패하면, */
	if (rc)
		/* [한국어] 그 오류를 올려보낸다. */
		return rc;
/* [한국어] 세 단계가 순서대로 의존한다 — 리비전 → 오프셋 → 버스 설정. */

	return 0;
}

static const struct dw_pcie_host_ops al_pcie_host_ops = {
	/* [한국어] 이 SoC 가 DWC 표준에서 벗어나는 부분이 초기화 하나에 모여 있다. */
	.init = al_pcie_host_init,
};

/* [한국어]
 * al_pcie_probe - ECAM 창과 컨트롤러 창을 얻어 DWC 호스트를 초기화한다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * DT 갈래의 진입점이다.
 *
 * **dw_pcie 를 따로 할당한다** — 이 드라이버가 그것을 포인터로 들기 때문이며,
 * 그래서 to_al_pcie() 매크로도 drvdata 를 거친다.
 *
 * 자원 둘을 이름으로 얻는다.
 * - "config" — ECAM 창. **크기만 쓰고 매핑하지 않는다.** 매핑은 DWC 코어가
 *   하며, 이 파일은 그 크기로 버스 번호 쪼갬의 마스크를 계산한다.
 * - "controller" — Annapurna 전용 창. 이쪽은 직접 매핑한다.
 *
 * native_ecam 을 세우는 것이 DWC 코어에 "config 접근을 ECAM 방식으로
 * 하라" 고 알리는 표시다. 그래야 코어가 ECAM 창을 매핑하고 이 파일의
 * map_bus 가 그 안의 오프셋을 계산하는 구조가 성립한다.
 *
 * controller 자원의 NULL 검사가 없는 것에 주의할 만하다 —
 * devm_ioremap_resource() 가 NULL 을 오류 포인터로 답하므로 아래 검사에서
 * 함께 걸린다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트이며 버스 스캔으로
 * 오래 걸린다.
 *
 * 에러 경로: 할당 실패는 -ENOMEM, ECAM 자원 부재는 -ENOENT, 컨트롤러 창
 * 매핑 실패는 그 오류. 되감기가 없는 것은 잡는 자원이 모두 devm 판이기 때문이다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → platform_get_resource_byname() → devm_ioremap_resource()
 *     → dw_pcie_host_init()
 */
static int al_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *controller_res;
	/* [한국어] ECAM 창 자원. */
	struct resource *ecam_res;
	/* [한국어] 이 드라이버의 상태. */
	struct al_pcie *al_pcie;
	/* [한국어] DWC 코어가 다룰 부분. */
	struct dw_pcie *pci;
/* [한국어] 먼저 상태 구조를 잡는다. */

	al_pcie = devm_kzalloc(dev, sizeof(*al_pcie), GFP_KERNEL);
	/* [한국어] 잡지 못하면, */
	if (!al_pcie)
		/* [한국어] 메모리 부족으로 물러난다. */
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	/* [한국어] dw_pcie 를 따로 잡지 못하면, */
	if (!pci)
		/* [한국어] 역시 메모리 부족이다. */
		return -ENOMEM;

	pci->dev = dev;
	/* [한국어] 호스트 콜백 표를 건다. */
	pci->pp.ops = &al_pcie_host_ops;
/* [한국어] 두 구조를 이어 둔다. */

	al_pcie->pci = pci;
	/* [한국어] 로그용 device 도 따로 들고 있는다. */
	al_pcie->dev = dev;
/* [한국어] 이제 자원을 얻는다. */

	ecam_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "config");
	/* [한국어] ECAM 자원이 없으면 — */
	if (!ecam_res) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "couldn't find 'config' reg in DT\n");
		/* [한국어] 장치 없음으로 물러난다. */
		return -ENOENT;
	}
	al_pcie->ecam_size = resource_size(ecam_res);
	/* [한국어] **config 접근을 ECAM 방식으로 하라는 표시다** — 그래야 코어가 창을
	 * 매핑하고 이 파일의 map_bus 가 그 안의 오프셋을 계산하는 구조가 성립한다. */
	pci->pp.native_ecam = true;
/* [한국어] 이제 컨트롤러 창을 얻는다. */

	controller_res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						      /* [한국어] 이름으로 찾는다 — ECAM 창과 구분해야 한다. */
						      "controller");
	al_pcie->controller_base = devm_ioremap_resource(dev, controller_res);
	/* [한국어] 매핑이 실패하면 — 자원이 NULL 이어도 여기서 함께 걸린다. */
	if (IS_ERR(al_pcie->controller_base)) {
		/* [한국어] 어느 범위였는지와 함께 남기고, */
		dev_err(dev, "couldn't remap controller base %pR\n",
			/* [한국어] 그 자원을 찍는다. */
			controller_res);
		return PTR_ERR(al_pcie->controller_base);
	/* [한국어] 컨트롤러 창 처리 끝. */
	}

	dev_dbg(dev, "From DT: controller_base: %pR\n", controller_res);
/* [한국어] 찾은 자원을 디버그 기록에 남긴다. */

	platform_set_drvdata(pdev, al_pcie);
/* [한국어] 이 파일의 변환 매크로가 drvdata 를 거치므로 아래 호출보다 먼저 매달아야 한다. */

	return dw_pcie_host_init(&pci->pp);
/* [한국어] 이 뒤로는 코어가 host_init 을 부르고 버스를 스캔한다. */
}

static const struct of_device_id al_pcie_of_match[] = {
	/* [한국어] Alpine v2 판. */
	{ .compatible = "amazon,al-alpine-v2-pcie",
	/* [한국어] 같은 드라이버가 두 세대를 다루며, 세대 차이는 런타임에 리비전을 읽어 가른다. */
	},
	{ .compatible = "amazon,al-alpine-v3-pcie",
	},
	{},
};

static struct platform_driver al_pcie_driver = {
	/* [한국어] 드라이버 정보. */
	.driver = {
		/* [한국어] sysfs 에 나올 이름. */
		.name	= "al-pcie",
		/* [한국어] 위 매칭 표. */
		.of_match_table = al_pcie_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = al_pcie_probe,
};
builtin_platform_driver(al_pcie_driver);

/* [한국어] DT 갈래 끝. */
#endif /* CONFIG_PCIE_AL*/
