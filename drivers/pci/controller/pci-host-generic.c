// SPDX-License-Identifier: GPL-2.0
/*
 * Simple, generic PCI host controller driver targeting firmware-initialised
 * systems and virtual machines (e.g. the PCI emulation provided by kvmtool).
 *
 * Copyright (C) 2014 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

/*
 * [한국어 설명] ECAM 만 있으면 되는 가장 단순한 호스트 브리지 (pci-host-generic.c)
 *
 * === 파일의 역할 ===
 * PCIe 호스트 컨트롤러 드라이버 중 가장 짧다. 100줄 남짓이고, 하는 일은
 * "config space 가 ECAM 으로 통째로 메모리에 매핑되어 있다" 는 것 하나에
 * 기대어 나머지를 전부 공통 코드에 맡기는 것이다.
 *
 * 왜 이런 드라이버가 성립하는가. 링크 트레이닝, 클럭, 리셋, PHY 같은
 * 초기화를 이미 다른 누군가가 끝냈다고 전제하기 때문이다. 그런 상황이
 * 실제로 흔하다.
 *   - 가상 머신. QEMU 나 kvmtool 이 만든 가상 PCI 버스에는 물리 계층이
 *     없으므로 초기화할 것도 없다. 상류 주석이 kvmtool 을 예로 든 이유다.
 *   - 펌웨어가 이미 다 해 놓은 시스템. UEFI 가 링크까지 올려 두면 커널은
 *     config 를 읽기만 하면 된다.
 *
 * ECAM(Enhanced Configuration Access Mechanism)은 config space 를 메모리에
 * 통째로 펼치는 방식이다. 장치의 config 주소를
 *   base + (bus << 20) + (devfn << 12) + offset
 * 으로 계산해 그냥 읽고 쓴다. 예전 x86 의 포트 0xCF8/0xCFC 방식처럼
 * 주소를 쓰고 데이터를 읽는 두 단계를 거칠 필요가 없어 훨씬 빠르고,
 * 잠금도 필요 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디바이스 트리에 pci-host-ecam-generic 등이 있으면
 *   -> gen_pci_driver 가 바인딩되고 probe 로 pci_host_common_probe() 가 불린다.
 *      이 파일에는 probe 함수가 따로 없다 — 공통 probe 를 그대로 쓰고,
 *      필요한 차이는 of_match 에 실어 둔 ops 로만 표현한다. 그것이 이
 *      드라이버가 이토록 짧은 이유다.
 *      -> pci_host_common_probe() [pci-host-common.c]
 *         -> ecam.c 의 pci_ecam_create() 로 config 창 매핑
 *         -> pci_host_probe() -> PCI 코어 열거
 *
 * 실행 컨텍스트: probe 뿐이다. 이후 config 접근은 ecam.c 의 공통 함수가 처리한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 버스.
 * 아래쪽: pci-host-common.c(공통 probe), ecam.c(ECAM 매핑과 접근),
 *   그리고 PCI 코어.
 * 공유 상태: struct pci_ecam_ops — config 접근 방법을 담은 표.
 *   이 파일은 그중 몇 가지 변형(정렬 제약이 있는 하드웨어용)을 정의한다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * 다만 가상 머신에서 NVMe 를 테스트할 때 그 밑에 있는 것이 대개 이
 * 드라이버다. QEMU 의 -device nvme 로 만든 장치는 이 브리지를 통해
 * 열거된다. 물리 하드웨어 없이 NVMe 드라이버 동작을 확인할 수 있는
 * 이유가 여기 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * gen_pci_driver          : 이 드라이버의 전부라 할 수 있다. probe 로
 *                           pci_host_common_probe() 를, remove 로
 *                           pci_host_common_remove() 를 그대로 가리킨다.
 * gen_pci_of_match        : 지원 compatible 목록이자 이 파일의 핵심.
 *                           compatible 마다 어떤 config 접근 ops 를 쓸지
 *                           data 에 실어 둔다. 순수 ECAM 은 공통
 *                           pci_generic_ecam_ops 를, CAM 방식 하드웨어는
 *                           아래 것을 쓴다.
 * gen_pci_cfg_cam_bus_ops : CAM(구형 Configuration Access Mechanism) 방식.
 *                           ECAM 이 버스당 20비트를 쓰는 것과 달리 CAM 은
 *                           16비트라 주소 계산이 다르고, 그래서 별도 ops 가
 *                           필요하다.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci-ecam.h>
#include <linux/platform_device.h>

#include "pci-host-common.h"

/*
 * NVMe: CAM(legacy Configuration Address Mapping)용 ops.
 *      bus_shift=16은 16비트 CAM에서 bus/dev/fn을 조합하는 방식을 의미.
 *      이 ops가 등록되면 루트 브리지 아래 NVMe 장치의 config read/write가
 *      pci_generic_config_read()/write()를 통해 이루어짐.
 */
static const struct pci_ecam_ops gen_pci_cfg_cam_bus_ops = {
	.bus_shift	= 16,
	.pci_ops	= {
		.map_bus	= pci_ecam_map_bus,
		.read		= pci_generic_config_read,
		.write		= pci_generic_config_write,
	}
};

/*
 * NVMe: Synopsys DesignWare PCIe 컨트롤러의 ECAM 모드 특이점.
 *      루트 포트의 downstream port가 type 0 config TLP를 dev 1 이상으로도
 *      전달하여 버스 0에 동일 NVMe 장치가 여러 번 나타나는 문제를 방지.
 */
static bool pci_dw_valid_device(struct pci_bus *bus, unsigned int devfn)
{
	struct pci_config_window *cfg = bus->sysdata;

	/*
	 * The Synopsys DesignWare PCIe controller in ECAM mode will not filter
	 * type 0 config TLPs sent to devices 1 and up on its downstream port,
	 * resulting in devices appearing multiple times on bus 0 unless we
	 * filter out those accesses here.
	 */
	if (bus->number == cfg->busr.start && PCI_SLOT(devfn) > 0)
		return false;

	return true;
}

/*
 * NVMe: DesignWare ECAM용 map_bus 콜백.
 *      pci_ecam_map_bus()로 NVMe config 공간 MMIO 주소를 만들기 전
 *      유효성 검사를 수행.
 */
static void __iomem *pci_dw_ecam_map_bus(struct pci_bus *bus,
					 unsigned int devfn, int where)
{
	if (!pci_dw_valid_device(bus, devfn))
		return NULL;

	return pci_ecam_map_bus(bus, devfn, where);
}

/*
 * NVMe: DesignWare 기반 SoC(Armada8k, Synquacer, snps,dw-pcie-ecam)용 ops.
 *      map_bus에 위의 valid_device 필터를 적용해 중복 열거를 막음.
 */
static const struct pci_ecam_ops pci_dw_ecam_bus_ops = {
	.pci_ops	= {
		.map_bus	= pci_dw_ecam_map_bus,
		.read		= pci_generic_config_read,
		.write		= pci_generic_config_write,
	}
};

/*
 * NVMe: DT compatible 매핑 테이블.
 *      firmware(DT)가 "pci-host-ecam-generic" 등을 선언하면 이 드라이버가
 *      PCIe 루트 브리지로 등록되고, NVMe SSD가 연결되었을 때
 *      drivers/nvme/host/pci.c의 nvme_probe()로 이어지는 열거 과정이 시작됨.
 */
static const struct of_device_id gen_pci_of_match[] = {
	{ .compatible = "pci-host-cam-generic",
	  .data = &gen_pci_cfg_cam_bus_ops },

	{ .compatible = "pci-host-ecam-generic",
	  .data = &pci_generic_ecam_ops },

	{ .compatible = "marvell,armada8k-pcie-ecam",
	  .data = &pci_dw_ecam_bus_ops },

	{ .compatible = "socionext,synquacer-pcie-ecam",
	  .data = &pci_dw_ecam_bus_ops },

	{ .compatible = "snps,dw-pcie-ecam",
	  .data = &pci_dw_ecam_bus_ops },

	{ },
};
MODULE_DEVICE_TABLE(of, gen_pci_of_match);

/*
 * NVMe: generic PCI host platform_driver.
 *      .probe = pci_host_common_probe: 루트 브리지 생성 및 pci_host_probe() 호출.
 *      이후 PCI 버스 스캔이 NVMe 장치를 발견하면
 *      drivers/nvme/host/pci.c가 nvme_pci_driver로 바인딩함.
 */
static struct platform_driver gen_pci_driver = {
	.driver = {
		.name = "pci-host-generic",
		.of_match_table = gen_pci_of_match,
	},
	.probe = pci_host_common_probe,
	.remove = pci_host_common_remove,
};
module_platform_driver(gen_pci_driver);

MODULE_DESCRIPTION("Generic PCI host controller driver");
MODULE_LICENSE("GPL v2");
