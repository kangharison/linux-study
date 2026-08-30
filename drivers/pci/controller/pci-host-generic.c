// SPDX-License-Identifier: GPL-2.0
/*
 * Simple, generic PCI host controller driver targeting firmware-initialised
 * systems and virtual machines (e.g. the PCI emulation provided by kvmtool).
 *
 * Copyright (C) 2014 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#include <linux/kernel.h>	/* NVMe: 기본 커널 타입/매크로; NVMe PCIe host는 같은 커널 인프라 사용 */
#include <linux/init.h>		/* NVMe: 모듈 초기화/종료 매크로; host 드라이버 로딩 시 필요 */
#include <linux/module.h>	/* NVMe: module_platform_driver() 등 모듈 등록용; NVMe 디바이스 바인딩의 시작점 */
#include <linux/pci-ecam.h>	/* NVMe: ECAM(Configuration Space Access) 정의; NVMe BAR/CSR 탐색의 근간 */
#include <linux/platform_device.h>	/* NVMe: DT/ACPI 기반 platform_device; SoC PCIe 루트 컴플렉스 등록에 사용 */

#include "pci-host-common.h"	/* NVMe: 공통 host bridge probe/remove; NVMe 열거를 위한 pci_host_common_probe 선언 */

/*
 * NVMe: CAM(legacy Configuration Address Mapping)용 ops.
 *      bus_shift=16은 16비트 CAM에서 bus/dev/fn을 조합하는 방식을 의미.
 *      이 ops가 등록되면 루트 브리지 아래 NVMe 장치의 config read/write가
 *      pci_generic_config_read()/write()를 통해 이루어짐.
 */
static const struct pci_ecam_ops gen_pci_cfg_cam_bus_ops = {
	.bus_shift	= 16,		/* NVMe: CAM 주소 폭; NVMe config 접근 시 bus/dev/fn 인코딩 비트 수 */
	.pci_ops	= {		/* NVMe: PCI config space 접근 함수 테이블; NVMe BAR 탐색/MSI-X cap 읽기에 사용 */
		.map_bus	= pci_ecam_map_bus,	/* NVMe: bus/dev/fn/offset -> MMIO 주소 변환; NVMe Vendor/Device ID 읽기의 첫 단계 */
		.read		= pci_generic_config_read,	/* NVMe: config dword/word/byte 읽기; NVMe BAR0/1, CAP, STS 등 쿼리 */
		.write		= pci_generic_config_write,	/* NVMe: config 공간 쓰기; NVMe BAR 할당, MSI-X enable, MSE/IOSE/BME 설정 */
	}
};

/*
 * NVMe: Synopsys DesignWare PCIe 컨트롤러의 ECAM 모드 특이점.
 *      루트 포트의 downstream port가 type 0 config TLP를 dev 1 이상으로도
 *      전달하여 버스 0에 동일 NVMe 장치가 여러 번 나타나는 문제를 방지.
 */
static bool pci_dw_valid_device(struct pci_bus *bus, unsigned int devfn)
{
	struct pci_config_window *cfg = bus->sysdata;	/* NVMe: 현재 버스의 ECAM 윈도우; NVMe가 연결될 bus의 config 매핑 정보 */

	/*
	 * The Synopsys DesignWare PCIe controller in ECAM mode will not filter
	 * type 0 config TLPs sent to devices 1 and up on its downstream port,
	 * resulting in devices appearing multiple times on bus 0 unless we
	 * filter out those accesses here.
	 */
	if (bus->number == cfg->busr.start && PCI_SLOT(devfn) > 0)	/* NVMe: 루트 버스이고 slot > 0인 가상/중복 config 접근 필터 */
		return false;	/* NVMe: 잘못된 slot의 NVMe config 요청 차단; 열거 중복 방지 */

	return true;	/* NVMe: 유효한 slot의 NVMe/config 접근 허용 */
}

/*
 * NVMe: DesignWare ECAM용 map_bus 콜백.
 *      pci_ecam_map_bus()로 NVMe config 공간 MMIO 주소를 만들기 전
 *      유효성 검사를 수행.
 */
static void __iomem *pci_dw_ecam_map_bus(struct pci_bus *bus,
					 unsigned int devfn, int where)
{
	if (!pci_dw_valid_device(bus, devfn))	/* NVMe: DesignWare 버그 우회; 유효하지 않은 NVMe 접근은 NULL 반환 */
		return NULL;

	return pci_ecam_map_bus(bus, devfn, where);	/* NVMe: (bus,devfn,where) -> ECAM 매핑 주소; NVMe config TLP의 메모리 인터페이스 */
}

/*
 * NVMe: DesignWare 기반 SoC(Armada8k, Synquacer, snps,dw-pcie-ecam)용 ops.
 *      map_bus에 위의 valid_device 필터를 적용해 중복 열거를 막음.
 */
static const struct pci_ecam_ops pci_dw_ecam_bus_ops = {
	.pci_ops	= {	/* NVMe: DesignWare 하드웨어를 통한 NVMe config 접근 함수 테이블 */
		.map_bus	= pci_dw_ecam_map_bus,	/* NVMe: DesignWare ECAM 주소 변환; NVMe config read/write의 MMIO 주소 계산 */
		.read		= pci_generic_config_read,	/* NVMe: NVMe PCIe capability, BAR, command/status 등 config read */
		.write		= pci_generic_config_write,	/* NVMe: NVMe BAR 설정, bus mastering/메모리 공간 enable 등 config write */
	}
};

/*
 * NVMe: DT compatible 매핑 테이블.
 *      firmware(DT)가 "pci-host-ecam-generic" 등을 선언하면 이 드라이버가
 *      PCIe 루트 브리지로 등록되고, NVMe SSD가 연결되었을 때
 *      drivers/nvme/host/pci.c의 nvme_probe()로 이어지는 열거 과정이 시작됨.
 */
static const struct of_device_id gen_pci_of_match[] = {
	/* NVMe: legacy CAM 방식 generic host; 오래된 ARM/VM 플랫폼의 NVMe 루트 브리지 */
	{ .compatible = "pci-host-cam-generic",
	  .data = &gen_pci_cfg_cam_bus_ops },

	/* NVMe: 표준 ECAM generic host; 대부분의 ARM64/VM 플랫폼에서 NVMe SSD를 탐색하는 경로 */
	{ .compatible = "pci-host-ecam-generic",
	  .data = &pci_generic_ecam_ops },

	/* NVMe: Marvell Armada8k SoC 내장 PCIe; NVMe SSD 연결 시 DesignWare 필터 적용 */
	{ .compatible = "marvell,armada8k-pcie-ecam",
	  .data = &pci_dw_ecam_bus_ops },

	/* NVMe: Socionext Synquacer SoC 내장 PCIe; NVMe 장치 중복 열거 방지 */
	{ .compatible = "socionext,synquacer-pcie-ecam",
	  .data = &pci_dw_ecam_bus_ops },

	/* NVMe: Synopsys DesignWare PCIe ECAM; 다양한 SoC의 NVMe 호스트 인터페이스 */
	{ .compatible = "snps,dw-pcie-ecam",
	  .data = &pci_dw_ecam_bus_ops },

	{ },	/* NVMe: of_device_id 테이블 종료 sentinel */
};
MODULE_DEVICE_TABLE(of, gen_pci_of_match);	/* NVMe: DT alias 매핑; 모듈 로드 시 compatible 기반 매칭을 위해 사용 */

/*
 * NVMe: generic PCI host platform_driver.
 *      .probe = pci_host_common_probe: 루트 브리지 생성 및 pci_host_probe() 호출.
 *      이후 PCI 버스 스캔이 NVMe 장치를 발견하면
 *      drivers/nvme/host/pci.c가 nvme_pci_driver로 바인딩함.
 */
static struct platform_driver gen_pci_driver = {
	.driver = {
		.name = "pci-host-generic",	/* NVMe: platform driver 이름; sysfs /sys/bus/platform/drivers/pci-host-generic */
		.of_match_table = gen_pci_of_match,	/* NVMe: 위 compatible 테이블 연결; NVMe 루트 브리지 후보 등록 */
	},
	.probe = pci_host_common_probe,		/* NVMe: 루트 브리지 초기화; NVMe가 속할 PCI 도메인/버스 생성 */
	.remove = pci_host_common_remove,	/* NVMe: 루트 버스 제거; 연결된 NVMe 장치 먼저 unbind 후 정리 */
};
module_platform_driver(gen_pci_driver);	/* NVMe: 모듈 로드/언로드 시 platform_driver 자동 등록; NVMe 사용 가능한 PCIe 도메인 생성/소멸 */

MODULE_DESCRIPTION("Generic PCI host controller driver");	/* NVMe: 범용 PCIe 루트 컴플렉스 드라이버; NVMe SSD의 PCIe 열거 기반 */
MODULE_LICENSE("GPL v2");	/* NVMe: GPL v2; NVMe PCIe host 드라이버와 동일한 라이선스 정책 */
