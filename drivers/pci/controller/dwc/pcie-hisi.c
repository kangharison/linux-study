// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for HiSilicon SoCs
 *
 * Copyright (C) 2015 HiSilicon Co., Ltd. http://www.hisilicon.com
 *
 * Authors: Zhou Wang <wangzhou1@hisilicon.com>
 *          Dacai Zhu <zhudacai@hisilicon.com>
 *          Gabriele Paoloni <gabriele.paoloni@huawei.com>
 */
/* [한국어] 이 파일에서 직접 쓰지는 않지만 상류가 남겨 둔 include 다. */
/*
 * [한국어 설명] HiSilicon HiP05/06/07 의 "거의 ECAM" PCIe 호스트 드라이버 (pcie-hisi.c)
 *
 * === 파일의 역할 ===
 * HiSilicon 서버 SoC 의 PCIe 호스트 컨트롤러가 표준 ECAM 에서 딱 한 군데
 * 벗어나는 것을 메우는 얇은 드라이버다. 벗어나는 지점은 이것이다 —
 * 루트 포트 자신의 config 공간이 ECAM 창 안에 있지 않고 별도의 레지스터
 * 창에 있으며, 그 창은 32비트 접근만 허용하고 슬롯도 하나뿐이다.
 * 그래서 이 파일이 하는 일은 세 가지로 요약된다. (1) 루트 버스 접근이면
 * 주소를 그 별도 창으로 돌리고, (2) 루트 버스에서 슬롯 0 이 아닌 접근은
 * 장치 없음으로 답해 유령 장치가 생기지 않게 하고, (3) 루트 버스 접근은
 * 폭과 무관하게 32비트 접근자를 쓴다. 그 아래 실제 장치들은 전부 표준
 * ECAM 경로로 흘려보낸다.
 * 파일 이름과 달리 DesignWare IP 코드를 전혀 쓰지 않는다는 점이 눈에 띈다.
 * dwc/ 디렉터리에 있지만 dw_pcie 도 dw_pcie_ops 도 등장하지 않고,
 * 대신 pci-ecam.h 와 pci-host-common.h 의 골격 위에 올라탄다.
 * 181줄, 함수 다섯 개가 전부다. probe 조차 직접 쓰지 않고
 * pci_host_common_probe() 를 그대로 빌려 쓴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 드라이버는 PCI 코어의 config 접근 경로 맨 아래에 끼어드는 어댑터다.
 * 위에서 pci_read_config_word() 같은 호출이 내려오면 코어가
 * bus->ops->read/write 를 부르고, 그것이 이 파일의 rd_conf/wr_conf 다.
 * 두 함수는 다시 pci_generic_config_read/write[32]() 를 부르고, 그 안에서
 * ops->map_bus 로 이 파일의 map_bus 가 불려 최종 주소가 정해진다.
 * 진입 경로가 둘이라는 것이 이 파일의 구조를 지배한다. ACPI 로 부팅하면
 * ACPI PCI 루트 브리지가 hisi_pcie_ops 를 써서 hisi_pcie_init() 을 부르고,
 * DT 로 부팅하면 builtin_platform_driver 가 pci_host_common_probe() 를 거쳐
 * hisi_pcie_platform_ops 의 hisi_pcie_platform_init() 을 부른다.
 * 두 경로가 다른 것은 init 하나뿐이고, 나머지 세 접근자는 완전히 공유한다.
 * 그래서 파일 전체가 바깥 #if 하나(둘 중 하나라도 켜졌는가)와 안쪽 #if 둘
 * (경로별 부분)로 이중 감싸여 있다.
 * 실행 컨텍스트는 두 갈래다. init 함수 둘은 probe 시점의 프로세스 컨텍스트로
 * 잠들 수 있고, 접근자 셋은 코어가 pci_lock 을 쥔 채 부르므로 잠들면 안 된다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어의 config 접근 경로(drivers/pci/access.c)와
 * pci-host-common.c 의 pci_host_common_probe(). probe 를 통째로 빌려 쓰므로
 * 자원 파싱, 버스 생성, 스캔은 이 파일에 한 줄도 없다.
 * 옆쪽: drivers/pci/ecam.c 의 pci_ecam_create() / pci_ecam_map_bus() 와
 * struct pci_config_window. cfg->priv 가 이 드라이버의 사설 상태를 매다는
 * 자리이고, cfg->busr.start 가 "루트 버스인가" 판정의 기준이다.
 * 아래쪽: access.c 의 pci_generic_config_read/write 와 그 32비트 전용 판.
 * read32 는 언제나 dword 를 읽어 바이트를 뽑아내고, write32 는 1/2바이트
 * 쓰기를 dword 읽기-수정-쓰기로 바꾼다 — 루트 포트의 32비트 제약을 이 둘이
 * 흡수해 주기 때문에 이 파일에는 그 처리가 없다.
 * 펌웨어 쪽: ACPI 경로는 acpi_get_rc_resources() 로 HISI0081 HID 장치를
 * 세그먼트 번호로 찾아 레지스터 창 주소를 얻고, DT 경로는 reg 속성의 두 번째
 * 항목에서 같은 정보를 얻는다. 표준 MCFG 가 알려 주지 않는 정보라 이런
 * 우회가 필요하며, 이 파일이 CONFIG_PCI_QUIRKS 조건 아래 놓인 이유다.
 * 데이터 흐름: 펌웨어(ACPI HISI0081 / DT reg[1]) → init 콜백이 매핑 →
 * cfg->priv->reg_base → map_bus 가 루트 버스 접근을 그리로 돌림 →
 * 32비트 접근자가 실제 읽고 씀.
 * 공유 상태: struct hisi_pcie 하나이며 필드도 reg_base 하나뿐이다.
 * 전역 변수는 ops 표 둘과 of_device_id 표 하나로 전부 상수다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct hisi_pcie: 필드가 reg_base 하나뿐인 사설 상태. init 이 채우고
 *   map_bus 가 읽으며, cfg->priv 에 매달려 컨트롤러 수명 동안 유지된다.
 * - hisi_pcie_rd_conf() / hisi_pcie_wr_conf(): 루트 버스면 슬롯 0 제한 +
 *   32비트 전용 접근자, 아니면 평범한 ECAM. 두 함수가 완전히 대칭이다.
 *   슬롯 제한이 없으면 스캔이 같은 레지스터를 31번 더 읽어 유령 장치를 만든다.
 * - hisi_pcie_map_bus(): 이 하드웨어의 비표준성이 응축된 함수. 루트 버스만
 *   reg_base 로 돌리고 나머지는 pci_ecam_map_bus() 에 맡긴다. devfn 을 무시해도
 *   되는 이유는 위 두 함수가 슬롯 0 이 아닌 접근을 이미 걸러 내기 때문이다.
 * - hisi_pcie_init(): ACPI 판. acpi_get_rc_resources() 로 HISI0081 을 세그먼트
 *   번호로 조회한다. 실패 세 지점이 모두 -ENOMEM 을 반환한다.
 * - hisi_pcie_platform_init(): DT 판. reg[1] 을 읽으면 끝이며, 없을 때는
 *   -EINVAL 을 쓴다. 같은 정보를 얻는 데 드는 비용이 펌웨어 인터페이스에 따라
 *   얼마나 달라지는지 보여 주는 대비다.
 * - hisi_pcie_ops / hisi_pcie_platform_ops: init 만 다르고 접근자 셋은 같은
 *   두 표. 전자는 ACPI 코드가 이름으로 참조하도록 const 전역으로 열려 있고,
 *   후자는 static 이며 of_device_id 표의 match data 로만 쓰인다.
 * - hisi_pcie_almost_ecam_driver: probe 가 pci_host_common_probe() 그대로다.
 *   suppress_bind_attrs 로 sysfs unbind 를 막고, builtin_platform_driver 로
 *   모듈이 아닌 내장 드라이버로 등록한다 — 루트 파일시스템이 이 버스 뒤에
 *   있을 수 있기 때문이다.
 */

#include <linux/interrupt.h>
/* [한국어] builtin_platform_driver() 매크로 계열의 초기화 섹션 정의. */
#include <linux/init.h>
/* [한국어] platform_get_resource() / to_platform_device() — DT 경로에서 reg[1] 을
 * 꺼내는 데 쓴다. */
#include <linux/platform_device.h>
/* [한국어] PCI_SLOT() 과 PCIBIOS_ 반환 코드. */
#include <linux/pci.h>
/* [한국어] acpi_get_rc_resources() / to_acpi_device() / acpi_driver_data() —
 * ACPI 경로에서 HISI0081 장치를 찾아 RC 레지스터 창을 얻는 통로다. */
#include <linux/pci-acpi.h>
/* [한국어] struct pci_config_window, struct pci_ecam_ops, pci_ecam_map_bus() —
 * 이 드라이버가 확장하는 ECAM 기반 골격. */
#include <linux/pci-ecam.h>
/* [한국어] devm_pci_remap_cfgspace() 선언. config 공간 전용 매핑 함수다. */
#include "../../pci.h"
/* [한국어] pci_host_common_probe() — DT 경로의 probe 를 통째로 빌려 온다. */
#include "../pci-host-common.h"

/* [한국어] 이 파일 전체를 감싸는 조건이다. DT 경로(CONFIG_PCI_HISI)나 ACPI 쿼크 경로
 * 둘 중 하나라도 켜져 있어야 컴파일된다. 두 경로가 아래 접근자 셋을 공유하기
 * 때문에, 공유 부분을 바깥 #if 로 묶고 경로별 부분만 안쪽 #if 로 나눈 구조다. */
#if defined(CONFIG_PCI_HISI) || (defined(CONFIG_ACPI) && defined(CONFIG_PCI_QUIRKS))

struct hisi_pcie {
	/* [한국어] 이 컨트롤러의 루트 포트 config 공간이 매핑된 곳.
	 * 설정자: hisi_pcie_init()(ACPI) 또는 hisi_pcie_platform_init()(DT).
	 * 읽는 자: hisi_pcie_map_bus() 가 루트 버스 접근일 때만 이 주소를 쓴다.
	 * 값 범위: devm_pci_remap_cfgspace() 가 준 유효한 __iomem 포인터.
	 *   실패 시 두 init 함수가 -ENOMEM 을 반환하므로 NULL 이 저장될 일은 없다.
	 * 동기화: probe 때 한 번 쓰고 이후 읽기만 하므로 락이 필요 없다.
	 *   cfg->priv 에 매달려 컨트롤러 수명 동안 유지된다. */
	void __iomem	*reg_base;
};

/* [한국어]
 * hisi_pcie_rd_conf - config 읽기. 루트 버스만 특별 취급한다
 *
 * @bus: 대상 버스. sysdata 에 pci_config_window 가 들어 있다.
 * @devfn: 슬롯(상위 5비트) + 기능(하위 3비트).
 * @where: config 공간 오프셋.
 * @size: 요청 폭(1/2/4).
 * @val: 읽은 값을 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL, 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * 이 파일이 "almost ecam" 인 이유가 이 함수에 그대로 드러난다.
 * 루트 버스가 아니면 pci_generic_config_read() 로 넘겨 평범한 ECAM 접근을 하고,
 * 루트 버스면 두 가지 제약을 건다.
 *
 * 첫째, 슬롯 0 만 존재한다고 답한다. 루트 포트 하나에 슬롯 하나뿐인데
 * 그대로 두면 버스 스캔이 같은 레지스터를 31번 더 읽어 유령 장치를 만든다.
 * 둘째, read32 판을 쓴다. 루트 포트 config 공간이 32비트 접근만 허용하므로,
 * 1/2바이트 요청도 dword 로 읽어 소프트웨어로 바이트를 뽑아내야 한다.
 *
 * 실행 컨텍스트: PCI config 접근 경로. 코어가 pci_lock 을 쥔 상태로 부르므로
 * 잠들면 안 된다. 이 함수는 잠들지 않는다.
 *
 * 에러 경로: 없는 슬롯은 PCIBIOS_DEVICE_NOT_FOUND. 코어는 그것을 모든 비트가
 * 1인 값으로 바꿔 상위에 돌려준다.
 *
 * 호출 체인:
 *   pci_read_config_*() → PCI 코어 → [이 함수]
 *     → pci_generic_config_read32() 또는 pci_generic_config_read()
 *     → hisi_pcie_map_bus()
 */
static int hisi_pcie_rd_conf(struct pci_bus *bus, u32 devfn, int where,
			     int size, u32 *val)
{
	/* [한국어] ECAM 골격이 bus->sysdata 에 넣어 둔 config 창 서술자.
	 * 여기서 필요한 것은 버스 번호 범위(busr)뿐이다. */
	struct pci_config_window *cfg = bus->sysdata;
	/* [한국어] devfn 상위 5비트가 슬롯 번호다. */
	int dev = PCI_SLOT(devfn);

	/* [한국어] 이 접근이 루트 버스(버스 범위의 첫 번호)를 향하는지 본다. */
	if (bus->number == cfg->busr.start) {
		/* access only one slot on each root port */
		/* [한국어] 위 영어 주석대로 루트 포트마다 슬롯이 하나뿐이므로, */
		if (dev > 0)
			/* [한국어] 0번이 아닌 슬롯을 물으면 장치 없음으로 답한다. 이렇게 하지 않으면
			 * 스캔이 같은 루트 포트 레지스터를 31번 더 읽어 유령 장치를 만들어 낸다. */
			return PCIBIOS_DEVICE_NOT_FOUND;
		else
			/* [한국어] 루트 포트 자신의 config 는 32비트 접근만 허용하므로 read32 판을 쓴다.
			 * 이 판은 언제나 dword 를 읽어 필요한 바이트를 소프트웨어로 뽑아낸다. */
			return pci_generic_config_read32(bus, devfn, where,
							 size, val);
	}

	/* [한국어] 루트 버스 아래의 실제 장치들은 평범한 ECAM 이라 폭 그대로 접근해도 된다.
	 * 이 파일의 이름이 "almost ecam" 인 이유가 이 한 줄과 위 분기의 차이다. */
	return pci_generic_config_read(bus, devfn, where, size, val);
}

/* [한국어]
 * hisi_pcie_wr_conf - config 쓰기. 읽기와 대칭인 제약을 건다
 *
 * @bus: 대상 버스.
 * @devfn: 슬롯 + 기능.
 * @where: config 공간 오프셋.
 * @size: 요청 폭(1/2/4).
 * @val: 쓸 값.
 * @return: PCIBIOS_SUCCESSFUL, 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * hisi_pcie_rd_conf() 와 구조가 완전히 같다. 루트 버스면 슬롯 0 으로 제한하고
 * 32비트 전용 write32 판을 쓰며, 그 밖에는 평범한 ECAM 쓰기로 넘긴다.
 *
 * write32 판은 1/2바이트 쓰기를 dword 읽기-수정-쓰기로 처리한다.
 * 읽기 쪽의 "읽어서 바이트를 뽑는다" 와 짝을 이루는 처리다.
 *
 * 실행 컨텍스트: PCI config 접근 경로. 잠들면 안 된다.
 *
 * 에러 경로: 없는 슬롯에 대한 쓰기는 조용히 버려진다.
 *
 * 호출 체인:
 *   pci_write_config_*() → PCI 코어 → [이 함수]
 *     → pci_generic_config_write32() 또는 pci_generic_config_write()
 *     → hisi_pcie_map_bus()
 */
static int hisi_pcie_wr_conf(struct pci_bus *bus, u32 devfn,
			     int where, int size, u32 val)
{
	/* [한국어] 위와 같은 방식으로 config 창 서술자를 꺼낸다. */
	struct pci_config_window *cfg = bus->sysdata;
	/* [한국어] 슬롯 번호. */
	int dev = PCI_SLOT(devfn);

	/* [한국어] 루트 버스 접근인지 판정. */
	if (bus->number == cfg->busr.start) {
		/* access only one slot on each root port */
		/* [한국어] 슬롯 0 만 존재한다. */
		if (dev > 0)
			/* [한국어] 그 밖의 슬롯에 대한 쓰기는 조용히 버린다 — 장치 없음으로 답하면
			 * 코어가 더 진행하지 않는다. */
			return PCIBIOS_DEVICE_NOT_FOUND;
		else
			/* [한국어] 루트 포트 config 는 32비트 전용이므로 write32 판. 이 판은 1/2바이트
			 * 요청을 dword 읽기-수정-쓰기로 처리한다. */
			return pci_generic_config_write32(bus, devfn, where,
							  size, val);
	}

	/* [한국어] 아래 장치들은 평범한 ECAM 쓰기. */
	return pci_generic_config_write(bus, devfn, where, size, val);
}

/* [한국어]
 * hisi_pcie_map_bus - config 주소를 계산한다. 루트 버스만 딴 창으로 보낸다
 *
 * @bus: 대상 버스.
 * @devfn: 슬롯 + 기능. 루트 버스 경로에서는 쓰지 않는다.
 * @where: config 공간 오프셋.
 * @return: 접근할 __iomem 주소. 실패하면 NULL(pci_ecam_map_bus 가 판단).
 *
 * 이 하드웨어의 비표준성이 응축된 함수다. 표준 ECAM 이라면 버스·슬롯·기능·
 * 오프셋을 한 수식으로 접어 창 하나의 주소를 만든다. 그런데 이 컨트롤러는
 * 루트 포트 자신의 config 공간을 그 창 안에 두지 않고 별도의 레지스터 창에
 * 둔다. 그래서 루트 버스 접근만 init 콜백이 매핑해 둔 reg_base 로 돌리고,
 * 나머지는 pci_ecam_map_bus() 에 그대로 맡긴다.
 *
 * devfn 을 무시하는 루트 버스 경로가 성립하는 이유는, 위 rd_conf/wr_conf 가
 * 루트 버스에서 슬롯 0 이 아닌 접근을 이미 걸러 내기 때문이다. 세 함수가
 * 한 묶음으로 동작한다.
 *
 * 실행 컨텍스트: config 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 루트 버스 경로는 실패하지 않는다. reg_base 가 NULL 일 수 없기
 * 때문이다 — 두 init 함수가 매핑 실패 시 probe 를 중단시킨다.
 *
 * 호출 체인:
 *   pci_generic_config_read/write[32]() → [이 함수] → pci_ecam_map_bus()
 */
static void __iomem *hisi_pcie_map_bus(struct pci_bus *bus, unsigned int devfn,
				       int where)
{
	/* [한국어] config 창 서술자. */
	struct pci_config_window *cfg = bus->sysdata;
	/* [한국어] init 콜백이 cfg->priv 에 매달아 둔 이 드라이버의 사설 상태. */
	struct hisi_pcie *pcie = cfg->priv;

	/* [한국어] 루트 버스 접근이면, */
	if (bus->number == cfg->busr.start)
		/* [한국어] 별도로 매핑해 둔 RC 레지스터 창을 쓴다. 루트 포트의 config 공간이
		 * ECAM 창 안이 아니라 딴 곳에 있다는 것이 이 하드웨어의 비표준 지점이고,
		 * 이 파일이 존재하는 이유 그 자체다. */
		return pcie->reg_base + where;
	else
		/* [한국어] 그 아래 장치들은 표준 ECAM 주소 계산을 그대로 쓴다. */
		return pci_ecam_map_bus(bus, devfn, where);
}

/* [한국어] ACPI 경로에서만 쓰이는 부분. 쿼크로 분류되어 있는 것은 표준 MCFG 만으로는
 * 이 하드웨어를 다룰 수 없어 예외 처리가 필요하기 때문이다. */
#if defined(CONFIG_ACPI) && defined(CONFIG_PCI_QUIRKS)

/* [한국어]
 * hisi_pcie_init - ACPI 경로에서 루트 포트 레지스터 창을 찾아 매핑한다
 *
 * @cfg: ECAM 골격이 만든 config 창 서술자. parent 가 ACPI 장치다.
 * @return: 0 = 성공, -ENOMEM = 할당·조회·매핑 실패.
 *
 * ACPI 로 부팅한 시스템에서 이 드라이버가 하는 유일한 준비 작업이다.
 *
 * 핵심은 acpi_get_rc_resources() 호출이다. 위 영어 주석이 밝히듯, HISI0081 이라는
 * ACPI HID 를 가진 장치들 중 _UID 가 우리 PCI 세그먼트 번호와 맞는 것을 찾아
 * 그 자원을 가져온다. 표준 MCFG 테이블은 ECAM 창만 알려 주지 이 컨트롤러가
 * 루트 포트 config 를 따로 둔 위치는 알려 주지 않기 때문에, 펌웨어가 별도
 * 장치로 심어 둔 정보를 이렇게 찾아내야 한다. 이 파일이 PCI_QUIRKS 조건 아래
 * 놓인 이유가 그것이다.
 *
 * 매핑에 ioremap 이 아니라 devm_pci_remap_cfgspace() 를 쓰는 것도 중요하다.
 * config 공간은 아키텍처에 따라 쓰기 결합이나 추측 실행을 금지하는 메모리
 * 속성을 요구하며, 그 차이를 이 함수가 처리한다.
 *
 * 결과는 cfg->priv 에 매달아 두고, 이후 hisi_pcie_map_bus() 가 꺼내 쓴다.
 *
 * 실행 컨텍스트: ACPI PCI 루트 브리지 probe, 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 에러 경로: 세 실패 지점 모두 -ENOMEM 을 반환한다. 자원 조회 실패도 마찬가지라
 * 원인이 반환값만으로는 구분되지 않지만, 그 경로에만 dev_err 로그가 있다.
 * 할당은 모두 devm_ 이라 되돌릴 것이 없다.
 *
 * 호출 체인:
 *   ACPI PCI 루트 브리지 probe → pci_ecam_create() → ops->init → [이 함수]
 *     → acpi_get_rc_resources() → devm_pci_remap_cfgspace()
 */
static int hisi_pcie_init(struct pci_config_window *cfg)
{
	/* [한국어] ECAM 골격이 넘겨 준 부모 장치. ACPI 경로에서는 ACPI 장치다. */
	struct device *dev = cfg->parent;
	/* [한국어] 할당할 사설 상태. */
	struct hisi_pcie *pcie;
	/* [한국어] 부모 device 를 ACPI 장치로 되돌린다. */
	struct acpi_device *adev = to_acpi_device(dev);
	/* [한국어] 그 ACPI 장치에 매달린 PCI 루트 정보. 아래에서 세그먼트 번호를 쓴다. */
	struct acpi_pci_root *root = acpi_driver_data(adev);
	/* [한국어] RC 레지스터 창을 받을 자원. */
	struct resource *res;
	/* [한국어] 오류 코드. */
	int ret;

	/* [한국어] 사설 상태를 할당한다. devm_ 이라 장치 해제 시 자동으로 풀린다. */
	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* [한국어] 할당 실패면, */
	if (!pcie)
		/* [한국어] 메모리 부족으로 나간다. */
		return -ENOMEM;

	/*
	 * Retrieve RC base and size from a HISI0081 device with _UID
	 * matching our segment.
	 */
	/* [한국어] 자원 서술자도 devm_ 으로 할당한다. 스택에 두지 않는 이유는
	 * acpi_get_rc_resources() 가 채워 준 뒤에도 정보를 남겨 두기 위함이다. */
	res = devm_kzalloc(dev, sizeof(*res), GFP_KERNEL);
	if (!res)
		/* [한국어] 할당 실패. */
		return -ENOMEM;

	/* [한국어] 위 영어 주석이 설명하듯, HISI0081 이라는 ACPI HID 를 가진 장치 중
	 * 우리 세그먼트 번호와 _UID 가 맞는 것을 찾아 그 자원을 가져온다.
	 * 루트 포트 레지스터 창의 주소를 펌웨어에서 얻는 경로다. */
	ret = acpi_get_rc_resources(dev, "HISI0081", root->segment, res);
	/* [한국어] 찾지 못했으면, */
	if (ret) {
		/* [한국어] 진단 메시지를 남기고, */
		dev_err(dev, "can't get rc base address\n");
		/* [한국어] -ENOMEM 을 반환한다. 실제 원인은 메모리 부족이 아니라 자원 조회 실패지만,
		 * 상류 코드가 ret 대신 이 값을 쓴다. */
		return -ENOMEM;
	}

	/* [한국어] 얻은 물리 주소 구간을 매핑한다. ioremap 이 아니라 전용 함수를 쓰는 이유는
	 * config 공간이 아키텍처에 따라 다른 메모리 속성(쓰기 결합 금지 등)을
	 * 요구하기 때문이다. */
	pcie->reg_base = devm_pci_remap_cfgspace(dev, res->start, resource_size(res));
	/* [한국어] 매핑 실패면, */
	if (!pcie->reg_base)
		/* [한국어] 메모리 부족으로 나간다. */
		return -ENOMEM;

	/* [한국어] 완성된 사설 상태를 config 창에 매단다. 이후 map_bus 가 이것을 꺼내 쓴다. */
	cfg->priv = pcie;
	/* [한국어] 성공. */
	return 0;
}

const struct pci_ecam_ops hisi_pcie_ops = {
	/* [한국어] ACPI 경로의 초기화 콜백. */
	.init         =  hisi_pcie_init,
	/* [한국어] 아래 세 접근자가 ACPI 경로와 DT 경로에서 완전히 같다 —
	 * 달라지는 것은 init 뿐이라 표를 둘로 나눈 것이다. */
	.pci_ops      = {
		/* [한국어] 루트 버스만 딴 창으로 보내는 주소 계산. */
		.map_bus    = hisi_pcie_map_bus,
		/* [한국어] 루트 버스 슬롯 0 제한 + 32비트 전용 읽기. */
		.read       = hisi_pcie_rd_conf,
		/* [한국어] 같은 제한의 쓰기. */
		.write      = hisi_pcie_wr_conf,
	}
};

/* [한국어] ACPI 쿼크 경로 끝. */
#endif

/* [한국어] 여기서부터 DT 경로. */
#ifdef CONFIG_PCI_HISI

/* [한국어]
 * hisi_pcie_platform_init - DT 경로에서 루트 포트 레지스터 창을 찾아 매핑한다
 *
 * @cfg: config 창 서술자. parent 가 플랫폼 장치다.
 * @return: 0 = 성공, -ENOMEM = 할당·매핑 실패, -EINVAL = reg[1] 이 없음.
 *
 * hisi_pcie_init() 의 DT 판이다. 하는 일은 같고 정보를 얻는 방법만 다르다.
 *
 * ACPI 판이 HISI0081 장치를 조회해야 했던 것과 달리, DT 판은 reg 속성의
 * 두 번째 항목을 읽으면 끝이다. 첫 번째가 ECAM 창이고 두 번째가 루트 포트
 * 레지스터 창이라고 바인딩이 정해 두었기 때문이다. 펌웨어 인터페이스가
 * 다르면 같은 정보를 얻는 비용도 이렇게 달라진다.
 *
 * reg[1] 이 없을 때 -EINVAL 을 반환하는 것도 ACPI 판과 다르다. 그쪽은 조회
 * 실패에도 -ENOMEM 을 썼는데, 여기서는 DT 가 잘못 쓰였다는 뜻이므로
 * 잘못된 인자가 더 정확하다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe, 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 에러 경로: 모두 devm_ 할당이라 되돌릴 것이 없다. probe 가 실패하면
 * 골격이 알아서 정리한다.
 *
 * 호출 체인:
 *   builtin_platform_driver → pci_host_common_probe() → pci_ecam_create()
 *     → ops->init → [이 함수] → platform_get_resource() → devm_pci_remap_cfgspace()
 */
static int hisi_pcie_platform_init(struct pci_config_window *cfg)
{
	/* [한국어] 부모 장치. DT 경로에서는 플랫폼 장치다. */
	struct device *dev = cfg->parent;
	/* [한국어] 할당할 사설 상태. */
	struct hisi_pcie *pcie;
	/* [한국어] 부모 device 를 플랫폼 장치로 되돌린다. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] RC 레지스터 창 자원. */
	struct resource *res;

	/* [한국어] 사설 상태 할당. */
	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		/* [한국어] 실패면 메모리 부족. */
		return -ENOMEM;

	/* [한국어] DT 의 reg 속성 중 **두 번째** 항목을 가져온다. 첫 번째는 ECAM 창이고
	 * 두 번째가 루트 포트 레지스터 창이다 — ACPI 경로가 HISI0081 조회로 얻는
	 * 것과 같은 정보를, DT 경로에서는 이 인덱스 하나로 얻는다. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	/* [한국어] 없으면, */
	if (!res) {
		/* [한국어] 어느 속성이 빠졌는지 알려 주고, */
		dev_err(dev, "missing \"reg[1]\"property\n");
		/* [한국어] 잘못된 설정으로 거절한다. 여기서는 ACPI 판과 달리 -EINVAL 을 쓴다. */
		return -EINVAL;
	}

	/* [한국어] ACPI 판과 동일하게 config 전용 매핑. */
	pcie->reg_base = devm_pci_remap_cfgspace(dev, res->start, resource_size(res));
	/* [한국어] 매핑 실패면, */
	if (!pcie->reg_base)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	/* [한국어] config 창에 매단다. */
	cfg->priv = pcie;
	/* [한국어] 성공. */
	return 0;
}

static const struct pci_ecam_ops hisi_pcie_platform_ops = {
	/* [한국어] DT 경로의 초기화 콜백. 이 한 줄만 ACPI 판과 다르다. */
	.init         =  hisi_pcie_platform_init,
	/* [한국어] 나머지 세 접근자는 ACPI 판과 완전히 같은 함수를 가리킨다. */
	.pci_ops      = {
		/* [한국어] 루트 버스 우회 주소 계산. */
		.map_bus    = hisi_pcie_map_bus,
		/* [한국어] 32비트 전용 읽기. */
		.read       = hisi_pcie_rd_conf,
		/* [한국어] 32비트 전용 쓰기. */
		.write      = hisi_pcie_wr_conf,
	}
};

static const struct of_device_id hisi_pcie_almost_ecam_of_match[] = {
	{
		/* [한국어] HiP06 SoC 의 컨트롤러. */
		.compatible =  "hisilicon,hip06-pcie-ecam",
		/* [한국어] 위 DT 용 ops 표를 매치 데이터로 붙인다. pci_host_common_probe() 가
		 * 이 데이터를 꺼내 골격에 넘긴다. */
		.data	    =  &hisi_pcie_platform_ops,
	},
	{
		/* [한국어] HiP07 SoC. 컨트롤러 동작이 같아 같은 ops 를 쓴다. */
		.compatible =  "hisilicon,hip07-pcie-ecam",
		/* [한국어] 동일한 표. */
		.data       =  &hisi_pcie_platform_ops,
	},
	/* [한국어] 배열 끝 표시. */
	{},
};

static struct platform_driver hisi_pcie_almost_ecam_driver = {
	/* [한국어] probe 를 직접 쓰지 않고 공용 골격 함수를 그대로 쓴다. 이 드라이버가
	 * 고유하게 할 일은 위 ops 표에 담긴 네 콜백뿐이고, 자원 파싱·버스 생성·
	 * 스캔은 모두 pci-host-common.c 가 처리한다. */
	.probe  = pci_host_common_probe,
	.driver = {
		   /* [한국어] sysfs 와 로그에 보일 드라이버 이름. */
		   .name = "hisi-pcie-almost-ecam",
		   /* [한국어] 위 compatible 표. */
		   .of_match_table = hisi_pcie_almost_ecam_of_match,
		   /* [한국어] sysfs 로 bind/unbind 를 막는다. 호스트 브리지를 런타임에 떼면 그 아래
		    * 모든 장치가 사라지므로 허용하지 않는다. */
		   .suppress_bind_attrs = true,
	},
};
/* [한국어] 모듈이 아니라 커널 내장 드라이버로 등록한다. 부팅 초기에 PCI 버스가
 * 있어야 루트 파일시스템의 저장 장치를 찾을 수 있기 때문이다. */
builtin_platform_driver(hisi_pcie_almost_ecam_driver);

/* [한국어] DT 경로 끝. */
#endif
/* [한국어] 파일 전체를 감싼 바깥 조건 끝. */
#endif
