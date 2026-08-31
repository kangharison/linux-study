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
