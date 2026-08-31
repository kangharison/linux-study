// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Mobiveil PCIe Host controller
 *
 * Copyright (c) 2018 Mobiveil Inc.
 * Copyright 2019 NXP
 *
 * Author: Subrahmanya Lingappa <l.subrahmanya@mobiveil.co.in>
 *	   Hou Zhiqiang <Zhiqiang.Hou@nxp.com>
 */

/*
 * [한국어 설명] Mobiveil IP 를 그대로 쓰는 보드용 드라이버 (pcie-mobiveil-plat.c)
 *
 * === 파일의 역할 ===
 * mobiveil/ 에서 가장 짧은 파일이다. 60줄뿐이고 함수도 하나다.
 * SoC 고유의 클럭·리셋·전원 처리가 필요 없는 보드 — Mobiveil 의 GPEX40
 * IP 를 그대로 얹은 경우 — 를 위한 최소 드라이버다.
 *
 * dwc/pcie-designware-plat.c 나 cadence/pcie-cadence-plat.c 와 같은
 * 자리이지만 그 둘보다도 얇다. 두 파일은 그래도 각각 EP 모드 지원이나
 * cpu_addr_fixup 콜백을 갖는데, 이 파일은 그런 것조차 없다 —
 * 브리지를 잡고 공통 초기화에 넘기는 것이 전부다.
 *
 * 같은 디렉터리의 pcie-layerscape-gen4.c 와 비교하면 이 파일의 성격이
 * 분명해진다. 그쪽은 NXP Layerscape 의 링크 판정과 리셋 처리를 위해
 * ops 를 채우지만, 이쪽은 ops 를 아예 설정하지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디바이스 트리에 mbvl,gpex40-pcie 가 있으면
 *   -> [이 파일] mobiveil_pcie_probe()
 *      -> 호스트 브리지와 struct mobiveil_pcie 를 함께 할당
 *      -> mobiveil_pcie_host_probe() [pcie-mobiveil-host.c]
 *         -> 레지스터 매핑, 주소 창 설정, 링크 대기, PCI 코어 열거
 *
 * 실행 컨텍스트: probe 뿐이며 프로세스 컨텍스트다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 버스.
 * 아래쪽: pcie-mobiveil-host.c 의 공통 초기화, 그리고 그 아래
 *   pcie-mobiveil.c 의 레지스터 접근과 주소 창.
 * 공유 상태: struct mobiveil_pcie 를 브리지의 private 영역에 둔다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(drivers/nvme 트리
 * 전수 확인 — mobiveil 관련 심볼 호출 0건). 관계는 토폴로지상의 것이다.
 *
 * === 주요 함수/구조체 요약 ===
 * mobiveil_pcie_probe()     : 브리지를 잡고 공통 초기화에 넘긴다.
 *                             이 파일의 유일한 함수다.
 * mobiveil_pcie_of_match    : 지원 compatible. GPEX40 하나뿐이다.
 * mobiveil_pcie_driver      : 플랫폼 드라이버 정의.
 *
 * === 눈여겨볼 점 ===
 * remove 함수가 없고 builtin_platform_driver 로 등록된다. 즉 이
 * 드라이버는 모듈로 뺄 수 없고 커널에 붙박이로 들어가며, 한 번 붙으면
 * 떨어지지 않는다. suppress_bind_attrs 로 sysfs 를 통한 수동 언바인드도
 * 막아 두었다 — 호스트 브리지를 떼면 그 아래 장치가 전부 사라지므로
 * 그럴 이유가 없다는 판단이다.
 * (MODULE_DESCRIPTION 과 MODULE_AUTHOR 는 남아 있지만 module_init 이
 * 아니라 builtin 이므로 모듈로 빌드되지는 않는다.)
 */

/* [한국어] 초기화 섹션 매크로. */
#include <linux/init.h>
/* [한국어] 기본 커널 매크로. */
#include <linux/kernel.h>
/* [한국어] MODULE_DEVICE_TABLE 과 MODULE_DESCRIPTION 등.
 * builtin 으로 빌드되더라도 디바이스 트리 매칭 표를 내보내려면 필요하다. */
#include <linux/module.h>
/* [한국어] PCI 관련 디바이스 트리 헬퍼. 다만 이 파일이 직접 쓰는
 * of_pci_* 심볼은 확인되지 않았다. */
#include <linux/of_pci.h>
/* [한국어] devm_pci_alloc_host_bridge 와 pci_host_bridge_priv. */
#include <linux/pci.h>
/* [한국어] 플랫폼 드라이버 등록과 struct platform_device. */
#include <linux/platform_device.h>
/* [한국어] 메모리 할당. 다만 이 파일은 devm_pci_alloc_host_bridge 만
 * 쓰고 직접 kmalloc 하지 않아, 이 헤더에서 오는 심볼 사용은 확인되지 않았다. */
#include <linux/slab.h>

/* [한국어] struct mobiveil_pcie 와 mobiveil_pcie_host_probe() 선언. */
#include "pcie-mobiveil.h"

/* [한국어]
 * mobiveil_pcie_probe - 호스트 브리지를 잡고 공통 초기화에 넘긴다
 *
 * @pdev: 디바이스 트리에서 만들어진 플랫폼 장치.
 * @return: mobiveil_pcie_host_probe() 의 결과, 또는 -ENOMEM.
 *
 * 이 파일의 유일한 함수이자 전부다. 하는 일이 셋뿐이다.
 *   브리지와 그 private 영역을 한 번에 잡고,
 *   서로를 가리키게 연결한 뒤,
 *   공통 초기화에 넘긴다.
 *
 * ops 를 설정하지 않는 점이 이 드라이버의 성격을 보여 준다. 같은
 * 디렉터리의 pcie-layerscape-gen4.c 는 링크 판정과 리셋을 위해 ops 를
 * 채우지만, 이 보드는 IP 의 기본 동작으로 충분하다는 판단이다.
 *
 * 다만 그 결과에 대해 코드를 확인한 내용을 남겨 둔다(수정하지 않음).
 * mobiveil 트리 전체를 주석 제거 후 훑으면 mv_pci->ops 와 rp->ops 를
 * 대입하는 곳은 pcie-layerscape-gen4.c:217-218 뿐이다. 그런데 두
 * 포인터는 각각 아래에서 ops 자체의 NULL 검사 없이 역참조된다.
 *   pcie-mobiveil.c 의 mobiveil_pcie_link_up(): pcie->ops->link_up
 *   pcie-mobiveil-host.c 의 인터럽트 초기화: rp->ops->interrupt_init
 * 그리고 두 경로 모두 아래 mobiveil_pcie_host_probe() 에서 도달한다 —
 * 그 함수가 mobiveil_pcie_interrupt_init() 과 mobiveil_bringup_link() 를
 * 차례로 부른다.
 * 브리지 private 영역은 0 으로 초기화되므로 이 드라이버 경로에서는
 * 두 ops 가 NULL 이다. 이것이 의도인지, 이 compatible 이 실제로 쓰이지
 * 않아 드러나지 않는 것인지는 이 트리에서 판단할 근거를 찾지 못했다.
 *
 * 실행 컨텍스트: probe — 프로세스 컨텍스트. 아래 host_probe 안에서
 *   링크 대기와 열거가 일어나 잠들 수 있다.
 *
 * 에러 경로: 브리지 할당 실패만 이 함수가 직접 다룬다. 나머지는
 *   host_probe 의 결과를 그대로 전한다. 브리지는 devm 이라 실패 시
 *   자동 해제된다.
 *
 * 호출 체인:
 *   플랫폼 버스 매칭 → [이 함수]
 *     → devm_pci_alloc_host_bridge() → mobiveil_pcie_host_probe()
 */
static int mobiveil_pcie_probe(struct platform_device *pdev)
{
	struct mobiveil_pcie *pcie;
	/* [한국어] PCI 코어가 다룰 호스트 브리지. 아래에서 private 영역과 함께 잡는다. */
	struct pci_host_bridge *bridge;
	struct device *dev = &pdev->dev;

	/* allocate the PCIe port */
	/* [한국어] 브리지와 struct mobiveil_pcie 를 한 덩어리로 잡는다.
	 * devm 이라 드라이버가 떨어질 때 자동 해제되는데, 이 드라이버는
	 * builtin 이고 remove 도 없어 실제로 떨어질 일은 없다. */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	if (!bridge)
		return -ENOMEM;

	/* [한국어] 브리지의 private 영역이 곧 우리 구조체다. */
	pcie = pci_host_bridge_priv(bridge);
	/* [한국어] 반대 방향 연결. 공통 코드가 rp.bridge 를 통해 브리지에
	 * 닿아야 하기 때문이다. */
	pcie->rp.bridge = bridge;

	/* [한국어] 오류 메시지와 자원 조회에 쓸 플랫폼 장치를 기록한다.
	 * pcie-mobiveil.c 의 dev_err 들이 pcie->pdev->dev 를 쓴다. */
	pcie->pdev = pdev;

	/* [한국어] 나머지는 전부 공통 코드가 한다 — 레지스터 매핑,
	 * 주소 창 설정, 인터럽트 도메인, 링크 대기, PCI 코어 열거. */
	return mobiveil_pcie_host_probe(pcie);
}

/* [한국어] 이 드라이버가 지원하는 디바이스 트리 compatible 목록. */
static const struct of_device_id mobiveil_pcie_of_match[] = {
	/* [한국어] Mobiveil GPEX40 IP. 이 하나뿐이라, 다른 SoC 는 자기 전용 드라이버
	 * (같은 디렉터리의 pcie-layerscape-gen4.c 등)를 쓴다. */
	{.compatible = "mbvl,gpex40-pcie",},
	{},
};

MODULE_DEVICE_TABLE(of, mobiveil_pcie_of_match);

/* [한국어] 플랫폼 드라이버 정의. remove 가 없다 — 아래 builtin 등록과 함께
 * 이 드라이버가 한 번 붙으면 떨어지지 않음을 뜻한다. */
static struct platform_driver mobiveil_pcie_driver = {
	/* [한국어] 유일한 콜백. */
	.probe = mobiveil_pcie_probe,
	.driver = {
		/* [한국어] sysfs 와 로그에 나타날 이름. */
		.name = "mobiveil-pcie",
		.of_match_table = mobiveil_pcie_of_match,
		.suppress_bind_attrs = true,
	},
};

builtin_platform_driver(mobiveil_pcie_driver);

MODULE_DESCRIPTION("Mobiveil PCIe host controller driver");
MODULE_AUTHOR("Subrahmanya Lingappa <l.subrahmanya@mobiveil.co.in>");
