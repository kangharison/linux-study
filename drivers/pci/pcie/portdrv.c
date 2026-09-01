// SPDX-License-Identifier: GPL-2.0
/*
 * Purpose:	PCI Express Port Bus Driver
 *
 * Copyright (C) 2004 Intel
 * Copyright (C) Tom Long Nguyen (tom.l.nguyen@intel.com)
 */

/*
 * [한국어 설명] 포트 하나를 여러 서비스로 쪼개 각각 드라이버를 붙이는 버스 (portdrv.c)
 *
 * === 파일의 역할 ===
 * PCIe 포트(Root Port, 스위치의 상/하류 포트, RC Event Collector)는 여러
 * 기능을 동시에 갖는다 — 오류 보고(AER), 핫플러그(HP), 전원 이벤트(PME),
 * 링크 격리(DPC), 대역폭 알림(BWCTRL). 이 기능들은 서로 독립적이고
 * 담당하는 사람도 다르다.
 *
 * 그래서 커널은 포트 하나에 드라이버 하나를 붙이는 대신, 기능마다 가상
 * 장치(struct pcie_device)를 만들어 각각에 전용 드라이버를 바인딩한다.
 * 이 파일이 그 가상 버스(pcie_port_bus_type)를 만들고 관리한다.
 *
 * 이렇게 나눈 이득이 분명하다. AER 드라이버는 핫플러그를 몰라도 되고,
 * 각각 별도 모듈로 뺄 수 있으며, 커널 드라이버 모델의 probe/remove/PM
 * 체계를 그대로 재사용한다. 대가는 한 겹의 간접성뿐이다.
 *
 * 인터럽트 배분도 이 파일이 한다. 포트가 MSI/MSI-X 를 지원하면 서비스마다
 * 다른 벡터를 줄 수 있고, 아니면 하나를 공유한다. pcie_init_service_irqs()
 * 가 그 배분을 정해 각 pcie_device 의 irq 필드에 채워 넣는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 바인딩: pci-driver.c 가 포트 장치에 pcie_portdriver 를 바인딩
 *           -> [이 파일] pcie_portdrv_probe()
 *              -> get_port_device_capability() 로 이 포트가 어떤 서비스를
 *                 제공하는지 판정(_OSC 협상 결과와 capability 를 함께 본다)
 *              -> pcie_init_service_irqs() 로 인터럽트를 배분
 *              -> pcie_device_init() 으로 서비스마다 가상 장치를 만들어 등록
 *                 -> 드라이버 코어가 pcie_port_bus_match() 로 짝을 찾아
 *                    각 서비스 드라이버의 probe 를 부른다
 *
 * 전원/오류: PM 콜백과 err_handler 를 받아 각 서비스 드라이버에게 뿌린다.
 *           서비스 드라이버는 자기가 포트 위에 얹혀 있다는 것을 신경 쓰지
 *           않고 보통의 드라이버처럼 콜백만 구현하면 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. probe/remove 와 PM 콜백 경로다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci-driver.c 의 드라이버 모델이 pcie_portdriver 를 포트에 바인딩한다
 *   (등록은 아래 pcie_portdrv_init() 의 pci_register_driver).
 *   서비스 소유권을 나타내는 host->native_aer / native_pme /
 *   native_pcie_hotplug / native_dpc 는 probe.c:1754-1770 에서 1 로
 *   초기화된다("우리가 다룰 수 있다고 가정한다" 는 상류 주석과 함께).
 *   그것을 0 으로 내리는 _OSC 협상 코드 자체는 이 스파스 체크아웃에
 *   없다 — drivers/acpi/pci_root.c 에 있으며, pci-acpi.c 는 그 결과를
 *   읽어 쓸 뿐이다(pci-acpi.c:1906 이 그 사실을 적어 두었다).
 * 아래쪽: 각 서비스 드라이버 — pcie/aer.c, pcie/pme.c, pcie/dpc.c,
 *   pcie/bwctrl.c, hotplug/pciehp_core.c.
 * 옆쪽: pcie/portdrv.h 가 서비스 비트 정의와 struct pcie_device /
 *   pcie_port_service_driver 를 담는다.
 * 공유 상태: 포트의 struct pci_dev, 그리고 서비스마다 하나씩 만들어지는
 *   struct pcie_device(최대 PCIE_PORT_DEVICE_MAXSERVICES = 5개).
 *
 * === 주요 함수/구조체 요약 ===
 * pcie_portdrv_probe()        : 포트에 바인딩되어 서비스 가상 장치들을 만든다.
 * pcie_port_device_register() : 실제로 서비스를 조사하고 등록하는 본체.
 * get_port_device_capability(): 이 포트가 제공하는 서비스 비트를 모은다.
 *                               capability 존재 여부와 _OSC 소유권을 함께 본다.
 * pcie_init_service_irqs()    : MSI/MSI-X 벡터를 서비스별로 배분한다.
 * pcie_device_init()          : 서비스 하나에 대한 struct pcie_device 를 만들어
 *                               드라이버 모델에 등록한다.
 * pcie_port_service_register() / _unregister() : 서비스 드라이버가 자신을
 *                               이 버스에 등록한다.
 * pcie_port_device_remove()   : 서비스 장치들을 제거한다.
 * pcie_portdrv_err_handler    : 오류 복구 콜백. 각 서비스에게 전달한다.
 * pcie_portdrv_pm_ops         : 전원 관리 콜백. 마찬가지로 전달한다.
 * pcie_port_bus_type          : 이 파일이 만드는 가상 버스("pci_express").
 *                               match/probe/remove 세 콜백이 서비스 드라이버와
 *                               서비스 장치를 짝지어 준다.
 * pcie_port_find_device()     : 포트의 특정 서비스 장치를 찾아 준다. 이 트리에서
 *                               확인한 호출자는 pcie/aer_inject.c:1100 하나다.
 * pcie_message_numbers()      : 각 서비스가 쓸 Interrupt Message Number 를 읽어
 *                               필요한 벡터 수를 계산한다.
 * pcie_port_enable_irq_vec()  : MSI-X/MSI 를 실제로 잡는다. 필요한 만큼만 남기려
 *                               일부러 한 번 해제하고 다시 잡는 대목이 있다.
 * pcie_port_setup()           : "pcie_ports=" 부팅 인자를 해석해 아래 세 전역을
 *                               세운다(compat/native/dpc-native).
 * pcie_ports_disabled / pcie_ports_native / pcie_ports_dpc_native (전역 bool)
 *                             : 이 드라이버를 아예 끌지, _OSC 를 무시하고 강행할지,
 *                               DPC 만 강행할지를 정한다.
 * pcie_portdrv_init()         : device_initcall 진입점. 서비스들을 초기화하고
 *                               pcie_portdriver 를 PCI 버스에 등록한다.
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 * 하지만 NVMe SSD 가 꽂힌 슬롯의 상위 포트에서 이 드라이버가 동작하며,
 * 그것이 NVMe 의 여러 동작을 뒷받침한다.
 *
 *   AER 서비스   - NVMe 의 PCIe 오류를 받아 복구 절차를 시작한다.
 *                  결국 nvme_error_detected 등이 불린다.
 *   DPC 서비스   - NVMe 를 예고 없이 뽑았을 때 링크를 격리한다.
 *   HP 서비스    - U.2/EDSFF 백플레인의 핫스왑. 드라이브를 꽂으면
 *                  pciehp 가 재스캔해 nvme_probe 가 불리고, 뽑으면
 *                  nvme_remove 가 불린다.
 *   PME 서비스   - D3 로 내려간 NVMe 가 깨어나야 할 때 그 신호를 받는다.
 *   BWCTRL 서비스- 링크 속도가 떨어졌을 때 알림을 받는다.
 *
 * 이 드라이버가 붙지 못하면(예: 펌웨어가 소유권을 넘겨주지 않으면)
 * 위 기능이 전부 동작하지 않는다. NVMe 는 여전히 I/O 를 하지만,
 * 오류 복구도 핫플러그도 되지 않는 상태가 된다.
 *
 * (기존 주석은 DPC 가 "NVMe CMB/P2P DMA 사용 시 데이터 무결성 보호에
 *  중요하다" 고 적었으나 dpc.c 코드에서 그 연결의 근거를 찾을 수 없어
 *  삭제했다. DPC 는 링크 단위 격리이고 CMB 접근과는 별개다.)
 *
 */

/* [한국어] bitfield.h — FIELD_GET. Interrupt Message Number 처럼 레지스터 안에
 * 비트 필드로 박힌 값을 마스크/시프트 없이 꺼내는 데 쓴다. */
#include <linux/bitfield.h>
/* [한국어] dmi.h — dmi_check_system() 과 struct dmi_system_id. 메인보드 식별 정보로
 * 기종별 우회(PME 를 MSI 로 받지 않기)를 적용한다. */
#include <linux/dmi.h>
/* [한국어] init.h — __init / __initconst / device_initcall. 부팅 뒤 반납되는 섹션 지정. */
#include <linux/init.h>
/* [한국어] module.h — 모듈 관련 매크로. 이 파일은 EXPORT_SYMBOL_GPL 도 쓴다. */
#include <linux/module.h>
/* [한국어] pci.h — struct pci_dev, pci_pcie_type(), PCI_EXP_* 레지스터 정의,
 * pci_alloc_irq_vectors() 등 인터럽트 API. */
#include <linux/pci.h>
/* [한국어] kernel.h — max() 등 기본 매크로. */
#include <linux/kernel.h>
/* [한국어] errno.h — -ENODEV, -EIO, -EACCES 등 반환할 errno 상수. */
#include <linux/errno.h>
/* [한국어] pm.h — struct dev_pm_ops. 아래 pcie_portdrv_pm_ops 의 타입이다. */
#include <linux/pm.h>
/* [한국어] pm_runtime.h — pm_runtime_* 계열. 포트의 런타임 절전을 다룬다. */
#include <linux/pm_runtime.h>
/* [한국어] string.h — strncmp(). "pcie_ports=" 인자 해석에 쓴다. */
#include <linux/string.h>
/* [한국어] slab.h — kzalloc_obj / kfree. struct pcie_device 를 잡고 푼다. */
#include <linux/slab.h>
/* [한국어] aer.h — AER 관련 공개 정의. pci_aer_available() 등이 여기서 온다. */
#include <linux/aer.h>

/* [한국어] ../pci.h — PCI 코어 내부 선언. pcie_link_rcec(), pci_bridge_d3_possible()
 * 같은 코어 헬퍼가 들어 있다. */
#include "../pci.h"
/* [한국어] portdrv.h — 이 디렉터리의 사설 헤더. PCIE_PORT_SERVICE_* 비트,
 * struct pcie_device / pcie_port_service_driver, 서비스 init 스텁이 있다. */
#include "portdrv.h"

/*
 * The PCIe Capability Interrupt Message Number (PCIe r3.1, sec 7.8.2) must
 * be one of the first 32 MSI-X entries.  Per PCI r3.0, sec 6.8.3.1, MSI
 * supports a maximum of 32 vectors per function.
 */
/* [한국어] MSI/MSI-X 로 잡아 볼 최대 벡터 수. 상류 주석이 근거를 적어 두었다 —
 * PCIe Capability 의 Interrupt Message Number 는 MSI-X 앞쪽 32항목 안에
 * 있어야 하고, MSI 자체가 함수당 32벡터를 넘지 못한다. */
#define PCIE_PORT_MAX_MSI_ENTRIES	32

/* [한국어] 서비스 장치 이름의 뒷부분을 만드는 매크로. 포트 종류(type)에서 4를 빼
 * 8비트 밀고 서비스 비트를 얹는다. 같은 포트에 서비스 장치가 여럿 매달리므로
 * 이름이 겹치지 않게 하려는 것이다. 4를 빼는 이유는 포트로 쓰이는 종류
 * 번호가 4(Root Port) 이상이라 그만큼 당겨 값의 폭을 줄이려는 것이다. */
#define get_descriptor_id(type, service) (((type - 4) << 8) | service)

/* [한국어] find_service_iter() 가 device_for_each_child() 의 void 인자로 주고받는
 * 작업 묶음. 콜백이 int 하나만 돌려줄 수 있어 결과를 구조체에 담아 온다. */
struct portdrv_service_data {
	/* [한국어] 찾은 서비스 드라이버.
	 * 설정자: find_service_iter() 가 일치하는 장치를 만났을 때.
	 * 읽는 자: 현재 없다 — pcie_port_find_device() 는 dev 만 꺼내 쓴다.
	 * 값 범위: 유효한 포인터 또는 초기화되지 않은 값(찾지 못하면 손대지 않는다).
	 * 동기화: 지역 변수로만 쓰이므로 별도 락이 없다. */
	struct pcie_port_service_driver *drv;
	/* [한국어] 찾은 서비스 장치.
	 * 설정자: find_service_iter().
	 * 읽는 자: pcie_port_find_device() 가 이 값을 반환한다.
	 * 값 범위: 유효한 device 포인터 또는 NULL. 호출자가 NULL 로 초기화해 둔다.
	 * 동기화: 지역 변수. 순회는 device_for_each_child() 내부에서 직렬화된다. */
	struct device *dev;
	/* [한국어] 찾는 서비스 비트.
	 * 설정자: pcie_port_find_device() 가 순회 전에 채운다.
	 * 읽는 자: find_service_iter() 가 드라이버의 service 와 비교한다.
	 * 값 범위: PCIE_PORT_SERVICE_* 중 하나.
	 * 동기화: 순회 중 읽기 전용이라 필요 없다. */
	u32 service;
};

/**
 * release_pcie_device - free PCI Express port service device structure
 * @dev: Port service device to release
 *
 * Invoked automatically when device is being removed in response to
 * device_unregister(dev).  Release all resources being claimed.
 */
/* [한국어]
 * release_pcie_device - 서비스 가상 장치의 메모리를 반납하는 콜백
 *
 * @dev: 해제될 struct device.   @return: 없음.
 *
 * device_unregister() 로 참조 계수가 0 이 되면 드라이버 코어가 자동으로
 * 부른다. pcie_device_init() 이 kzalloc 으로 잡은 struct pcie_device 를
 * 여기서 kfree 한다.
 *
 * 이 콜백을 반드시 채워야 하는 이유가 있다. 드라이버 코어는 struct device
 * 가 어떤 큰 구조체에 박혀 있는지 모르므로, 해제 방법을 소유자가 알려
 * 주어야 한다. to_pcie_device() 가 container_of 로 바깥 구조체를 되찾는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(마지막 put_device 시점).
 *
 * 호출 체인:  device_unregister() / put_device() → 드라이버 코어 → [이 함수]
 */
static void release_pcie_device(struct device *dev)
{
	/* [한국어] to_pcie_device() 가 container_of 로 바깥 struct pcie_device 를 되찾아
	 * kfree 한다. 드라이버 코어는 device 가 어떤 구조체에 박혀 있는지 모르므로
	 * 해제 방법을 소유자가 이 콜백으로 알려 주어야 한다. */
	kfree(to_pcie_device(dev));
}

/*
 * Fill in *pme, *aer, *dpc with the relevant Interrupt Message Numbers if
 * services are enabled in "mask".  Return the number of MSI/MSI-X vectors
 * required to accommodate the largest Message Number.
 */
/* [한국어]
 * pcie_message_numbers - 서비스별 Interrupt Message Number 를 읽고 필요한 벡터 수를 센다
 *
 * @dev:  대상 PCIe 포트.
 * @mask: 이 포트가 제공하는 서비스 비트(get_port_device_capability 결과).
 * @pme:  PME/HP/BWCTRL 이 쓸 메시지 번호를 받을 곳.
 * @aer:  AER 이 쓸 메시지 번호를 받을 곳.
 * @dpc:  DPC 가 쓸 메시지 번호를 받을 곳.
 * @return: 가장 큰 메시지 번호 + 1. 즉 최소한 이만큼의 벡터를 잡아야 한다.
 *
 * PCIe 는 포트의 각 기능이 "나는 몇 번째 벡터를 쓴다" 를 자기 capability
 * 안에 적어 두는 방식을 쓴다. 그 번호를 Interrupt Message Number 라 하고,
 * MSI-X 에서는 테이블 항목 번호, MSI 에서는 기준 Message Data 로부터의
 * 오프셋을 뜻한다. 상류 주석이 관련 스펙 절(7.8.2, 7.10.10, 7.31.2)을 적어 두었다.
 *
 * 세 곳에서 읽는다.
 *   PME/HP/BWCTRL - PCIe capability 의 Flags 레지스터. 셋이 한 번호를
 *     공유하므로 한 번만 읽는다.
 *   AER - AER capability 의 Root Error Status 레지스터 상위 필드.
 *   DPC - DPC capability 의 DPC Capability 레지스터.
 *
 * 반환값이 "개수" 인 이유는 번호가 0부터 시작하기 때문이다. 3번을 쓰겠다는
 * 장치에는 최소 4개의 벡터가 필요하다. 그래서 매번 +1 하고 max 로 누적한다.
 *
 * capability 가 없으면 그 서비스의 번호는 손대지 않는다. 호출자가 0 으로
 * 초기화해 넘기므로 결과적으로 0번 벡터를 쓰게 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(포트 probe).
 *
 * 호출 체인:  pcie_port_enable_irq_vec() → [이 함수]
 *               → pcie_capability_read_word() / pci_read_config_dword()
 */
static int pcie_message_numbers(struct pci_dev *dev, int mask,
				u32 *pme, u32 *aer, u32 *dpc)
{
	/* [한국어] nvec 은 지금까지 본 가장 큰 메시지 번호 + 1, pos 는 capability 오프셋. */
	u32 nvec = 0, pos;
	/* [한국어] 16비트 레지스터를 읽어 담을 곳. */
	u16 reg16;

	/*
	 * The Interrupt Message Number indicates which vector is used, i.e.,
	 * the MSI-X table entry or the MSI offset between the base Message
	 * Data and the generated interrupt message.  See PCIe r3.1, sec
	 * 7.8.2, 7.10.10, 7.31.2.
	 */

	/* [한국어] PME, 핫플러그, 대역폭 알림 셋 중 하나라도 쓰면 */
	if (mask & (PCIE_PORT_SERVICE_PME | PCIE_PORT_SERVICE_HP |
		    /* [한국어] 셋이 같은 번호를 공유하므로 한 번만 읽으면 된다. */
		    PCIE_PORT_SERVICE_BWCTRL)) {
		/* [한국어] PCIe Capability 의 Flags 레지스터. 여기 Interrupt Message Number 가 있다. */
		pcie_capability_read_word(dev, PCI_EXP_FLAGS, &reg16);
		/* [한국어] PCI_EXP_FLAGS_IRQ 필드를 꺼낸다. 이것이 "나는 몇 번째 벡터를 쓴다" 는 값이다. */
		*pme = FIELD_GET(PCI_EXP_FLAGS_IRQ, reg16);
		/* [한국어] 번호가 0부터 시작하므로 필요한 개수는 +1 이다. */
		nvec = *pme + 1;
	}

/* [한국어] AER 이 빌드에 포함될 때만 이 갈래를 컴파일한다. */
#ifdef CONFIG_PCIEAER
	/* [한국어] AER 서비스를 제공하는 포트라면 */
	if (mask & PCIE_PORT_SERVICE_AER) {
		/* [한국어] AER 쪽은 32비트 레지스터를 읽는다. */
		u32 reg32;

		/* [한국어] AER capability 오프셋. 열거 때 찾아 둔 값이다. */
		pos = dev->aer_cap;
		/* [한국어] capability 가 없으면 읽을 것이 없다. 그 경우 *aer 을 건드리지 않아
		 * 호출자가 초기화해 둔 0 이 남는다. */
		if (pos) {
			/* [한국어] Root Error Status 레지스터에 AER 의 메시지 번호가 들어 있다. */
			pci_read_config_dword(dev, pos + PCI_ERR_ROOT_STATUS,
				      &reg32);
			/* [한국어] PCI_ERR_ROOT_AER_IRQ 필드를 꺼낸다. */
			*aer = FIELD_GET(PCI_ERR_ROOT_AER_IRQ, reg32);
			/* [한국어] 지금까지의 최대와 비교해 큰 쪽을 남긴다. */
			nvec = max(nvec, *aer + 1);
		}
	}
/* [한국어] CONFIG_PCIEAER 갈래 끝. */
#endif

	/* [한국어] DPC 서비스를 제공하는 포트라면 */
	if (mask & PCIE_PORT_SERVICE_DPC) {
		/* [한국어] DPC 는 확장 capability 라 별도로 찾아야 한다. */
		pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_DPC);
		/* [한국어] 없으면 건너뛴다. */
		if (pos) {
			/* [한국어] DPC Capability 레지스터에 메시지 번호가 있다. */
			pci_read_config_word(dev, pos + PCI_EXP_DPC_CAP,
				     &reg16);
			/* [한국어] PCI_EXP_DPC_IRQ 필드를 꺼낸다. */
			*dpc = FIELD_GET(PCI_EXP_DPC_IRQ, reg16);
			/* [한국어] 역시 최대값을 갱신한다. */
			nvec = max(nvec, *dpc + 1);
		}
	}

	/* [한국어] 가장 큰 번호 + 1 — 최소한 이만큼의 벡터를 잡아야 모든 서비스가
	 * 자기 번호에 해당하는 벡터를 가질 수 있다. */
	return nvec;
}

/**
 * pcie_port_enable_irq_vec - try to set up MSI-X or MSI as interrupt mode
 * for given port
 * @dev: PCI Express port to handle
 * @irqs: Array of interrupt vectors to populate
 * @mask: Bitmask of port capabilities returned by get_port_device_capability()
 *
 * Return value: 0 on success, error code on failure
 */
/* [한국어]
 * pcie_port_enable_irq_vec - MSI-X 또는 MSI 를 잡아 서비스별 IRQ 를 채운다
 *
 * @dev:  대상 PCIe 포트.
 * @irqs: 서비스별 IRQ 번호를 채울 배열(PCIE_PORT_DEVICE_MAXSERVICES 칸).
 * @mask: 이 포트가 제공하는 서비스 비트.
 * @return: 0 성공. 음수면 벡터를 잡지 못했거나(-ENOSPC 등)
 *          장치가 알린 메시지 번호가 잡을 수 있는 벡터 수를 넘는다(-EIO).
 *
 * 두 번 잡는 구조가 이 함수의 특징이고, 그 이유가 상류 주석에 있다.
 *   1) 우선 최대치(32개)를 요청해 본다. 몇 개가 필요한지 알려면 메시지
 *      번호를 읽어야 하는데, 그러기 전에는 판단할 수 없기 때문이다.
 *   2) pcie_message_numbers() 로 실제 필요한 수를 센다.
 *   3) 필요한 수가 잡은 수보다 많으면 이 포트는 쓸 수 없다(-EIO).
 *   4) 남는다면 전부 놓고 필요한 만큼만 다시 잡는다. 벡터는 시스템 전체가
 *      나눠 쓰는 자원이라 놀리면 안 되기 때문이다.
 *
 * 다시 잡으면 구체적인 벡터가 바뀔 수 있으므로 pci_irq_vector() 호출은
 * 반드시 재할당 뒤에 해야 한다 - 상류 주석이 그 점을 못박아 두었다.
 * MSI 의 경우 하드웨어가 해제/재할당 사이에 메시지 번호를 바꾸는 것이
 * 스펙상 허용되지만, 가장 큰 번호를 감당할 만큼 잡았으므로 그러지 않으리라
 * 가정한다는 설명도 함께 적혀 있다.
 *
 * 마지막에 서비스별로 IRQ 를 배분한다. PME/HP/BWCTRL 은 한 벡터를 함께
 * 쓰고(상류 주석대로), AER 과 DPC 는 각자 번호를 갖는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pcie_init_service_irqs() → [이 함수]
 *               → pci_alloc_irq_vectors() [msi/api.c]
 *               → pcie_message_numbers() → pci_irq_vector()
 */
static int pcie_port_enable_irq_vec(struct pci_dev *dev, int *irqs, int mask)
{
	/* [한국어] nr_entries 는 실제로 잡힌 벡터 수, nvec 은 필요한 수, pcie_irq 는 공유 IRQ. */
	int nr_entries, nvec, pcie_irq;
	/* [한국어] 세 서비스의 메시지 번호. 0 으로 초기화해 두어 capability 가 없는 경우
	 * 아래에서 0번 벡터를 쓰게 된다. */
	u32 pme = 0, aer = 0, dpc = 0;

	/* Allocate the maximum possible number of MSI/MSI-X vectors */
	/* [한국어] 우선 최대치를 요청한다. 몇 개가 필요한지 알려면 메시지 번호를 읽어야 하는데,
	 * 그러려면 먼저 잡아 봐야 하기 때문이다. */
	nr_entries = pci_alloc_irq_vectors(dev, 1, PCIE_PORT_MAX_MSI_ENTRIES,
			/* [한국어] MSI-X 를 우선하고 안 되면 MSI 로 떨어진다. */
			PCI_IRQ_MSIX | PCI_IRQ_MSI);
	/* [한국어] 하나도 못 잡았다. */
	if (nr_entries < 0)
		/* [한국어] errno 를 그대로 전한다 — 호출자가 INTx 로 내려간다. */
		return nr_entries;

	/* See how many and which Interrupt Message Numbers we actually use */
	/* [한국어] 실제로 몇 개가 필요한지 센다. */
	nvec = pcie_message_numbers(dev, mask, &pme, &aer, &dpc);
	/* [한국어] 장치가 알린 번호가 잡을 수 있는 최대치를 넘는다. */
	if (nvec > nr_entries) {
		/* [한국어] 잡아 둔 것을 놓고 */
		pci_free_irq_vectors(dev);
		/* [한국어] 입출력 오류로 돌린다. 이 포트는 MSI 방식으로 쓸 수 없다. */
		return -EIO;
	}

	/*
	 * If we allocated more than we need, free them and reallocate fewer.
	 *
	 * Reallocating may change the specific vectors we get, so
	 * pci_irq_vector() must be done *after* the reallocation.
	 *
	 * If we're using MSI, hardware is *allowed* to change the Interrupt
	 * Message Numbers when we free and reallocate the vectors, but we
	 * assume it won't because we allocate enough vectors for the
	 * biggest Message Number we found.
	 */
	/* [한국어] 필요한 수와 잡은 수가 다르면 — 즉 남으면 */
	if (nvec != nr_entries) {
		/* [한국어] 전부 놓는다. 벡터는 시스템 전체가 나눠 쓰는 자원이라 놀리면 안 된다. */
		pci_free_irq_vectors(dev);

		/* [한국어] 필요한 만큼만 정확히 다시 잡는다(최소=최대=nvec). */
		nr_entries = pci_alloc_irq_vectors(dev, nvec, nvec,
				PCI_IRQ_MSIX | PCI_IRQ_MSI);
		/* [한국어] 재할당 실패. */
		if (nr_entries < 0)
			/* [한국어] errno 를 전한다. */
			return nr_entries;
	}

	/* PME, hotplug and bandwidth notification share an MSI/MSI-X vector */
	/* [한국어] PME/HP/BWCTRL 은 상류 주석대로 한 벡터를 함께 쓴다. */
	if (mask & (PCIE_PORT_SERVICE_PME | PCIE_PORT_SERVICE_HP |
		    PCIE_PORT_SERVICE_BWCTRL)) {
		/* [한국어] 메시지 번호를 실제 Linux IRQ 번호로 바꾼다. 이 호출이 재할당 *뒤* 여야
		 * 하는 이유가 상류 주석에 있다 — 다시 잡으면 구체적인 벡터가 바뀔 수 있다. */
		pcie_irq = pci_irq_vector(dev, pme);
		/* [한국어] PME 자리에 넣고 */
		irqs[PCIE_PORT_SERVICE_PME_SHIFT] = pcie_irq;
		/* [한국어] 핫플러그 자리에도 같은 IRQ 를, */
		irqs[PCIE_PORT_SERVICE_HP_SHIFT] = pcie_irq;
		/* [한국어] 대역폭 알림 자리에도 같은 IRQ 를 넣는다. */
		irqs[PCIE_PORT_SERVICE_BWCTRL_SHIFT] = pcie_irq;
	}

	/* [한국어] AER 을 제공하면 */
	if (mask & PCIE_PORT_SERVICE_AER)
		/* [한국어] AER 은 자기 번호에 해당하는 벡터를 따로 갖는다. */
		irqs[PCIE_PORT_SERVICE_AER_SHIFT] = pci_irq_vector(dev, aer);

	/* [한국어] DPC 를 제공하면 */
	if (mask & PCIE_PORT_SERVICE_DPC)
		/* [한국어] DPC 도 자기 번호의 벡터를 갖는다. */
		irqs[PCIE_PORT_SERVICE_DPC_SHIFT] = pci_irq_vector(dev, dpc);

	/* [한국어] 배분 완료. */
	return 0;
}

/**
 * pcie_init_service_irqs - initialize irqs for PCI Express port services
 * @dev: PCI Express port to handle
 * @irqs: Array of irqs to populate
 * @mask: Bitmask of port capabilities returned by get_port_device_capability()
 *
 * Return value: Interrupt mode associated with the port
 */
/* [한국어]
 * pcie_init_service_irqs - 서비스별 IRQ 배열을 채운다. 안 되면 INTx 로 내려간다
 *
 * @dev:  대상 PCIe 포트.
 * @irqs: 채울 IRQ 배열.
 * @mask: 이 포트가 제공하는 서비스 비트.
 * @return: 0 성공, -ENODEV 는 INTx 조차 잡지 못한 경우.
 *
 * 배열을 전부 -1 로 초기화해 두고 시작한다. 어떤 서비스에 IRQ 가 배정되지
 * 않았는지를 그 값으로 구분하기 위해서다.
 *
 * 그다음 두 갈래다.
 *   - PME 를 제공하는데 pcie_pme_no_msi() 가 참이면 곧바로 INTx 로 간다.
 *     특정 기종에서 PME 를 MSI 로 받으면 동작하지 않는다는 것이 알려져
 *     있고, 그 판정을 dmi_pcie_pme_disable_msi() 가 부팅 때 세워 둔다.
 *   - 아니면 MSI-X/MSI 를 시도하고, 실패하면 역시 INTx 로 내려간다.
 *
 * INTx 경로에서는 벡터가 하나뿐이므로 모든 서비스가 같은 IRQ 를 공유한다.
 * 배열을 통째로 그 번호로 채우는 것이 그 뜻이다. 상류 주석이 밝히듯
 * 시스템 공유 인터럽트일 수도 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pcie_port_device_register() → [이 함수]
 *               → pcie_port_enable_irq_vec() 또는 pci_alloc_irq_vectors(PCI_IRQ_INTX)
 */
static int pcie_init_service_irqs(struct pci_dev *dev, int *irqs, int mask)
{
	/* [한국어] ret 는 INTx 할당 결과, i 는 반복자. */
	int ret, i;

	/* [한국어] 배열을 모두 -1 로 채워 둔다. */
	for (i = 0; i < PCIE_PORT_DEVICE_MAXSERVICES; i++)
		/* [한국어] 어떤 서비스에 IRQ 가 배정되지 않았는지를 이 값으로 구분한다. */
		irqs[i] = -1;

	/*
	 * If we support PME but can't use MSI/MSI-X for it, we have to
	 * fall back to INTx or other interrupts, e.g., a system shared
	 * interrupt.
	 */
	/* [한국어] PME 를 제공하는데 이 기종에서는 PME 를 MSI 로 받으면 안 되는 경우.
	 * 그 판정은 부팅 때 dmi_pcie_pme_disable_msi() 가 세워 둔다. */
	if ((mask & PCIE_PORT_SERVICE_PME) && pcie_pme_no_msi())
		/* [한국어] MSI 를 시도하지도 않고 곧바로 INTx 로 간다. */
		goto intx_irq;

	/* Try to use MSI-X or MSI if supported */
	/* [한국어] MSI-X/MSI 를 시도한다. */
	if (pcie_port_enable_irq_vec(dev, irqs, mask) == 0)
		/* [한국어] 성공하면 배열이 이미 채워졌다. */
		return 0;

/* [한국어] MSI 를 쓸 수 없거나 실패했을 때 오는 지점. */
intx_irq:
	/* fall back to INTX IRQ */
	/* [한국어] INTx 를 하나 잡는다. 최소=최대=1 인 이유는 핀 인터럽트가 본래 하나뿐이기 때문이다. */
	ret = pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_INTX);
	/* [한국어] 그것마저 실패하면 이 포트에 인터럽트를 붙일 방법이 없다. */
	if (ret < 0)
		/* [한국어] 장치 없음으로 돌린다. */
		return -ENODEV;

	/* [한국어] 벡터가 하나뿐이므로 모든 서비스가 같은 IRQ 를 공유한다. */
	for (i = 0; i < PCIE_PORT_DEVICE_MAXSERVICES; i++)
		/* [한국어] 0번(유일한) 벡터의 IRQ 번호로 배열을 통째로 채운다.
		 * 상류 주석이 밝히듯 시스템 공유 인터럽트일 수도 있다. */
		irqs[i] = pci_irq_vector(dev, 0);

	/* [한국어] 배분 완료. */
	return 0;
}

/**
 * get_port_device_capability - discover capabilities of a PCI Express port
 * @dev: PCI Express port to examine
 *
 * The capabilities are read from the port's PCI Express configuration registers
 * as described in PCI Express Base Specification 1.0a sections 7.8.2, 7.8.9 and
 * 7.9 - 7.11.
 *
 * Return value: Bitmask of discovered port capabilities
 */
/* [한국어]
 * get_port_device_capability - 이 포트가 제공할 서비스 비트를 모은다
 *
 * @dev: 조사할 PCIe 포트.
 * @return: PCIE_PORT_SERVICE_* 비트들의 OR. 0 이면 붙일 서비스가 없다.
 *
 * 서비스마다 판정 조건이 두 겹이다. 하드웨어에 그 기능이 있는가, 그리고
 * OS 가 그것을 다뤄도 되는가. 두 번째가 소유권 문제이며, host->native_*
 * 비트로 표현된다. 그 비트는 probe.c:1754-1770 에서 1 로 시작하고,
 * ACPI 시스템에서는 _OSC 협상 결과에 따라 0 으로 내려갈 수 있다.
 * pcie_ports_native 는 "pcie_ports=native" 로 그 협상을 무시하겠다는 뜻이다.
 *
 * 서비스별로 보면
 *   HP     - dev->is_pciehp 이고 Root Port 또는 Downstream Port 여야 한다.
 *            CONFIG_HOTPLUG_PCI_PCIE 가 꺼져 있으면 서비스 드라이버가 뜨지
 *            않으므로, BIOS 가 켜 두었을 수 있는 핫플러그 인터럽트를 여기서
 *            직접 꺼 준다. 아무도 처리하지 않는 인터럽트가 계속 오는 것을
 *            막으려는 것이다.
 *   AER    - Root Port 또는 RC Event Collector 이고 aer_cap 이 있어야 한다.
 *   PME    - 역시 Root Port 또는 RC Event Collector. 상류 주석대로 그 둘만
 *            PME 를 생성한다. 여기서도 BIOS 가 켜 두었을 수 있는 PME
 *            인터럽트를 꺼 두는데, 필요해지면 PME 서비스 드라이버가 켠다.
 *   DPC    - DPC capability 가 있어야 하고, 보통은 AER 소유권이 함께
 *            필요하다. 상류 주석대로 "pcie_ports=dpc-native" 를 주면
 *            AER 권한 없이도 DPC 를 쓸 수 있다.
 *   BWCTRL - Downstream 또는 Root Port 이고, Link Capabilities 의 Link
 *            Bandwidth Notification 비트가 서 있으며, 지원 속도가 둘 이상
 *            이어야 한다. 속도가 하나뿐이면 알릴 변화 자체가 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(포트 probe).
 *
 * 호출 체인:  pcie_port_device_register() → [이 함수]
 *               → pci_find_host_bridge() / pcie_capability_read_dword()
 *               → pcie_pme_interrupt_enable()
 */
static int get_port_device_capability(struct pci_dev *dev)
{
	/* [한국어] 이 포트가 속한 호스트 브리지. 아래에서 native_* 소유권 비트를 본다. */
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);
	/* [한국어] 모아 나갈 서비스 비트. 0 이면 붙일 서비스가 없다는 뜻이다. */
	int services = 0;

	/* [한국어] 핫플러그 슬롯이 달린 포트인가(열거 때 세워 둔 값). */
	if (dev->is_pciehp &&
	    /* [한국어] Root Port 이거나 */
	    (pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT ||
	     /* [한국어] Downstream Port 여야 한다. 그 아래에만 슬롯이 있을 수 있다. */
	     pci_pcie_type(dev) == PCI_EXP_TYPE_DOWNSTREAM) &&
	    /* [한국어] 그리고 OS 가 핫플러그를 다뤄도 되어야 한다. pcie_ports_native 는
	     * "pcie_ports=native" 로 _OSC 협상을 무시하겠다는 뜻이다. */
	    (pcie_ports_native || host->native_pcie_hotplug)) {
		/* [한국어] 핫플러그 서비스를 제공 목록에 넣는다. */
		services |= PCIE_PORT_SERVICE_HP;

		/*
		 * Disable hot-plug interrupts in case they have been enabled
		 * by the BIOS and the hot-plug service driver won't be loaded
		 * to handle them.
		 */
		/* [한국어] pciehp 가 빌드에 없으면 서비스 드라이버가 뜨지 않는다. */
		if (!IS_ENABLED(CONFIG_HOTPLUG_PCI_PCIE))
			/* [한국어] 그러면 BIOS 가 켜 두었을 수 있는 핫플러그 인터럽트를 여기서 직접 끈다. */
			pcie_capability_clear_word(dev, PCI_EXP_SLTCTL,
				/* [한국어] Command Completed 와 Hot-Plug 인터럽트 활성 비트를 내린다.
				 * 아무도 처리하지 않는 인터럽트가 계속 오는 것을 막으려는 것이다. */
				PCI_EXP_SLTCTL_CCIE | PCI_EXP_SLTCTL_HPIE);
	}

/* [한국어] AER 이 빌드에 포함될 때만 이 갈래를 컴파일한다. */
#ifdef CONFIG_PCIEAER
	/* [한국어] AER 은 Root Port 이거나 */
	if ((pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT ||
             /* [한국어] RC Event Collector 여야 한다. 그 둘만 오류 메시지를 수집한다. */
             pci_pcie_type(dev) == PCI_EXP_TYPE_RC_EC) &&
	    /* [한국어] AER capability 가 있고, AER 이 전역으로 켜져 있어야 하며 */
	    dev->aer_cap && pci_aer_available() &&
	    /* [한국어] OS 가 AER 소유권을 가져야 한다. */
	    (pcie_ports_native || host->native_aer))
		/* [한국어] AER 서비스를 제공 목록에 넣는다. */
		services |= PCIE_PORT_SERVICE_AER;
/* [한국어] CONFIG_PCIEAER 갈래 끝. */
#endif

	/* Root Ports and Root Complex Event Collectors may generate PMEs */
	/* [한국어] PME 도 Root Port 이거나 */
	if ((pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT ||
	     /* [한국어] RC Event Collector 여야 한다. 상류 주석대로 그 둘만 PME 를 생성한다. */
	     pci_pcie_type(dev) == PCI_EXP_TYPE_RC_EC) &&
	    /* [한국어] 그리고 OS 가 PME 소유권을 가져야 한다. */
	    (pcie_ports_native || host->native_pme)) {
		/* [한국어] PME 서비스를 제공 목록에 넣는다. */
		services |= PCIE_PORT_SERVICE_PME;

		/*
		 * Disable PME interrupt on this port in case it's been enabled
		 * by the BIOS (the PME service driver will enable it when
		 * necessary).
		 */
		/* [한국어] BIOS 가 켜 두었을 수 있는 PME 인터럽트를 일단 끈다. 상류 주석대로
		 * 필요해지면 PME 서비스 드라이버가 다시 켠다. HP 쪽과 달리 서비스가
		 * 뜰 예정인데도 끄는 이유는, 드라이버가 준비되기 전에 신호가 오면
		 * 처리할 주체가 없기 때문이다. */
		pcie_pme_interrupt_enable(dev, false);
	}

	/*
	 * With dpc-native, allow Linux to use DPC even if it doesn't have
	 * permission to use AER.
	 */
	/* [한국어] DPC 확장 capability 가 있고 */
	if (pci_find_ext_capability(dev, PCI_EXT_CAP_ID_DPC) &&
	    /* [한국어] AER 기반 구조가 쓸 수 있어야 하며 */
	    pci_aer_available() &&
	    /* [한국어] 보통은 AER 소유권이 함께 필요하다. 상류 주석대로 "pcie_ports=dpc-native"
	     * 를 주면 AER 권한 없이도 DPC 를 쓴다. */
	    (pcie_ports_dpc_native || (services & PCIE_PORT_SERVICE_AER)))
		/* [한국어] DPC 서비스를 제공 목록에 넣는다. */
		services |= PCIE_PORT_SERVICE_DPC;

	/* Enable bandwidth control if more than one speed is supported. */
	/* [한국어] 대역폭 알림은 Downstream 이거나 */
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_DOWNSTREAM ||
	    /* [한국어] Root Port 에서만 의미가 있다. 그 아래로 링크가 뻗기 때문이다. */
	    pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT) {
		/* [한국어] Link Capabilities 값을 담을 곳. */
		u32 linkcap;

		/* [한국어] Link Capabilities 레지스터를 읽는다. */
		pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &linkcap);
		/* [한국어] Link Bandwidth Notification 을 하드웨어가 지원하고 */
		if (linkcap & PCI_EXP_LNKCAP_LBNC &&
		    /* [한국어] 지원 속도가 둘 이상이어야 한다. 속도가 하나뿐이면 알릴 변화 자체가 없다.
		     * hweight8 은 세워진 비트 수를 세는 함수다. */
		    hweight8(dev->supported_speeds) > 1)
			/* [한국어] 대역폭 제어 서비스를 제공 목록에 넣는다. */
			services |= PCIE_PORT_SERVICE_BWCTRL;
	}

	/* [한국어] 모은 서비스 비트를 돌려준다. 0 이면 붙일 것이 없다는 뜻이다. */
	return services;
}

/**
 * pcie_device_init - allocate and initialize PCI Express port service device
 * @pdev: PCI Express port to associate the service device with
 * @service: Type of service to associate with the service device
 * @irq: Interrupt vector to associate with the service device
 */
/* [한국어]
 * pcie_device_init - 서비스 하나를 대표하는 가상 장치를 만들어 등록한다
 *
 * @pdev:    이 서비스가 얹힐 PCIe 포트.
 * @service: 이 장치가 담당할 서비스 비트 하나.
 * @irq:     이 서비스에 배정된 IRQ 번호.
 * @return: 0 성공, -ENOMEM 또는 device_register() 의 errno.
 *
 * 포트 하나를 서비스 여러 개로 쪼개는 설계가 실제로 구현되는 지점이다.
 * struct pcie_device 를 만들어 포트 포인터/IRQ/서비스 비트를 채우고,
 * 안에 박힌 struct device 를 pcie_port_bus_type 버스에 등록한다.
 * 그러면 드라이버 코어가 pcie_port_bus_match() 로 짝을 찾아 해당 서비스
 * 드라이버의 probe 를 불러 준다.
 *
 * 이름을 "<포트이름>:pcie<XXX>" 로 짓는데, 뒷부분은 get_descriptor_id()
 * 매크로가 포트 종류와 서비스 비트를 섞어 만든 값이다. 같은 포트에
 * 여러 서비스 장치가 매달리므로 이름이 겹치면 안 되기 때문이다.
 *
 * 실패 처리가 미묘하다. device_register() 가 실패해도 kfree 가 아니라
 * put_device() 를 부른다. 등록이 실패해도 device 는 이미 초기화되어
 * 있어서, 참조를 놓아 release 콜백(release_pcie_device)이 kfree 하게
 * 하는 것이 드라이버 모델의 규약이다.
 *
 * 마지막 두 줄도 의도가 분명하다. device_enable_async_suspend() 는 서비스
 * 장치들의 절전을 병렬로 처리해 재개 시간을 줄이고, pm_runtime_no_callbacks()
 * 는 이 가상 장치 자체에는 런타임 PM 콜백이 없음을 알린다 - 실제 전원은
 * 아래 포트가 관리하므로 가상 장치가 끼어들 이유가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pcie_port_device_register() → [이 함수] → device_register()
 */
static int pcie_device_init(struct pci_dev *pdev, int service, int irq)
{
	/* [한국어] device_register() 결과. */
	int retval;
	/* [한국어] 만들 서비스 가상 장치. */
	struct pcie_device *pcie;
	/* [한국어] 그 안에 박힌 struct device 를 가리킬 지역 포인터. */
	struct device *device;

	/* [한국어] 0 으로 채워 할당한다. 쓰지 않는 필드가 쓰레기값으로 남지 않게 한다. */
	pcie = kzalloc_obj(*pcie);
	/* [한국어] 할당 실패. */
	if (!pcie)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;
	/* [한국어] 이 서비스가 얹힐 포트를 기억해 둔다. 서비스 드라이버가 이 포인터로
	 * 실제 하드웨어에 접근한다. */
	pcie->port = pdev;
	/* [한국어] 배정받은 IRQ 번호. 서비스 드라이버가 여기에 핸들러를 건다. */
	pcie->irq = irq;
	/* [한국어] 이 장치가 담당할 서비스 비트. 아래 pcie_port_bus_match() 의 판정 기준이다. */
	pcie->service = service;

	/* Initialize generic device interface */
	/* [한국어] 드라이버 모델이 다룰 부분의 주소. */
	device = &pcie->device;
	/* [한국어] 이 파일이 만든 가상 버스에 소속시킨다. 이 한 줄이 드라이버 코어의
	 * 매칭 대상을 pcie_port_bus_type 으로 정한다. */
	device->bus = &pcie_port_bus_type;
	device->release = release_pcie_device;	/* callback to free pcie dev */
	/* [한국어] 이름을 "<포트이름>:pcie<XXX>" 로 짓는다. */
	dev_set_name(device, "%s:pcie%03x",
		     /* [한국어] 앞부분은 포트의 PCI 이름(예: 0000:00:1c.0). */
		     pci_name(pdev),
		     /* [한국어] 뒷부분은 포트 종류와 서비스 비트를 섞은 값. 같은 포트에 서비스 장치가
		      * 여럿 매달리므로 이름이 겹치면 안 된다. */
		     get_descriptor_id(pci_pcie_type(pdev), service));
	/* [한국어] 부모를 포트로 지정한다. 이 관계 덕분에 device_for_each_child() 로
	 * 포트의 서비스들을 훑을 수 있고, PM 순서도 자동으로 맞는다. */
	device->parent = &pdev->dev;
	/* [한국어] 서비스들의 절전을 병렬로 처리해 재개 시간을 줄인다. */
	device_enable_async_suspend(device);

	/* [한국어] 드라이버 코어에 등록한다. 이 안에서 매칭이 일어나 짝이 맞으면
	 * 서비스 드라이버의 probe 까지 이어진다. */
	retval = device_register(device);
	/* [한국어] 등록 실패. */
	if (retval) {
		/* [한국어] kfree 가 아니라 put_device 인 것이 요점이다. 등록이 실패해도 device 는
		 * 이미 초기화되어 있어서, 참조를 놓아 release_pcie_device() 가 kfree 하게
		 * 하는 것이 드라이버 모델의 규약이다. */
		put_device(device);
		/* [한국어] errno 를 전한다. */
		return retval;
	}

	/* [한국어] 이 가상 장치 자체에는 런타임 PM 콜백이 없음을 알린다. 실제 전원은
	 * 아래 포트가 관리하므로 가상 장치가 끼어들 이유가 없다. */
	pm_runtime_no_callbacks(device);

	/* [한국어] 성공. */
	return 0;
}

/**
 * pcie_port_device_register - register PCI Express port
 * @dev: PCI Express port to register
 *
 * Allocate the port extension structure and register services associated with
 * the port.
 */
/* [한국어]
 * pcie_port_device_register - 포트를 조사해 서비스 장치들을 만들어 붙인다
 *
 * @dev: 대상 PCIe 포트.
 * @return: 0 성공(서비스가 하나도 없어도 0), 음수 errno 실패.
 *
 * 이 파일의 본체다. 순서와 각 단계의 판단이 전부 의미가 있다.
 *   1) pci_enable_device() - config 접근과 인터럽트를 쓰려면 먼저 켜야 한다.
 *   2) get_port_device_capability() 로 서비스 비트를 모은다. 하나도 없으면
 *      0 을 돌려주고 끝낸다. 오류가 아니라 "할 일이 없다" 는 뜻이다.
 *   3) pci_set_master() - 서비스들이 MSI 를 받으려면 포트가 버스 마스터로
 *      동작해 메모리 쓰기를 낼 수 있어야 한다.
 *   4) pcie_init_service_irqs() 로 IRQ 를 배분한다. 실패하면 상류 주석대로
 *      HP 만 남긴다 - pciehp 에는 폴링 모드(pciehp_poll_mode)가 있어
 *      인터럽트 없이도 동작할 수 있기 때문이다. 다른 서비스는 인터럽트가
 *      없으면 의미가 없다.
 *   5) 서비스 비트를 하나씩 훑으며 pcie_device_init() 으로 가상 장치를 만든다.
 *      하나라도 성공하면 성공으로 본다.
 *
 * 실패 경로가 두 갈래인 이유는 되돌릴 것이 다르기 때문이다. 서비스 장치를
 * 하나도 못 만들었으면 벡터까지 반납해야 하고(error_cleanup_irqs),
 * IRQ 배분 자체가 실패했으면 반납할 벡터가 없다(error_disable).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(포트 probe).
 *
 * 호출 체인:  pcie_portdrv_probe() → [이 함수]
 *               → get_port_device_capability() → pcie_init_service_irqs()
 *               → pcie_device_init()
 */
static int pcie_port_device_register(struct pci_dev *dev)
{
	/* [한국어] status 는 하위 호출 결과, capabilities 는 서비스 비트, nr_service 는
	 * 실제로 만들어진 서비스 장치 수. */
	int status, capabilities, i, nr_service;
	/* [한국어] 서비스별 IRQ 번호를 담을 배열. 스택에 두는 이유는 pcie_device_init() 에
	 * 값을 넘기고 나면 더 필요 없기 때문이다. */
	int irqs[PCIE_PORT_DEVICE_MAXSERVICES];

	/* Enable PCI Express port device */
	/* [한국어] config 접근과 인터럽트를 쓰려면 먼저 장치를 켜야 한다. */
	status = pci_enable_device(dev);
	/* [한국어] 켜기 실패. */
	if (status)
		/* [한국어] errno 를 전한다. */
		return status;

	/* Get and check PCI Express port services */
	/* [한국어] 이 포트가 어떤 서비스를 제공하는지 조사한다. */
	capabilities = get_port_device_capability(dev);
	/* [한국어] 하나도 없으면 */
	if (!capabilities)
		/* [한국어] 0 을 돌려준다. 오류가 아니라 "할 일이 없다" 는 뜻이라 probe 는 성공한다. */
		return 0;

	/* [한국어] 서비스들이 MSI 를 받으려면 포트가 버스 마스터로 동작해 메모리 쓰기를
	 * 낼 수 있어야 한다. MSI 자체가 장치가 보내는 메모리 쓰기이기 때문이다. */
	pci_set_master(dev);
	/*
	 * Initialize service irqs. Don't use service devices that
	 * require interrupts if there is no way to generate them.
	 * However, some drivers may have a polling mode (e.g. pciehp_poll_mode)
	 * that can be used in the absence of irqs.  Allow them to determine
	 * if that is to be used.
	 */
	/* [한국어] 서비스별로 IRQ 를 배분한다. */
	status = pcie_init_service_irqs(dev, irqs, capabilities);
	/* [한국어] 배분에 실패했다면 */
	if (status) {
		/* [한국어] 핫플러그만 남긴다. 상류 주석대로 pciehp 에는 폴링 모드가 있어
		 * 인터럽트 없이도 동작할 수 있기 때문이다. 다른 서비스는 인터럽트가
		 * 없으면 의미가 없다. */
		capabilities &= PCIE_PORT_SERVICE_HP;
		/* [한국어] 핫플러그조차 없으면 */
		if (!capabilities)
			/* [한국어] 장치를 끄고 물러난다. 반납할 벡터가 없으므로 error_disable 로 간다. */
			goto error_disable;
	}

	/* Allocate child services if any */
	/* [한국어] 기본 반환값을 실패로 둔다. 아래에서 하나라도 성공하면 0 으로 바뀐다. */
	status = -ENODEV;
	/* [한국어] 만들어진 서비스 장치 수. */
	nr_service = 0;
	/* [한국어] 서비스 비트를 하나씩 훑는다. */
	for (i = 0; i < PCIE_PORT_DEVICE_MAXSERVICES; i++) {
		/* [한국어] i 번째 비트를 서비스 값으로 만든다. */
		int service = 1 << i;
		/* [한국어] 이 포트가 제공하지 않는 서비스면 */
		if (!(capabilities & service))
			/* [한국어] 건너뛴다. */
			continue;
		/* [한국어] 가상 장치를 만든다. 배열의 같은 인덱스가 그 서비스의 IRQ 다. */
		if (!pcie_device_init(dev, service, irqs[i]))
			/* [한국어] 성공한 개수를 센다. */
			nr_service++;
	}
	/* [한국어] 하나도 만들지 못했으면 */
	if (!nr_service)
		/* [한국어] 벡터까지 반납하는 경로로 간다. */
		goto error_cleanup_irqs;

	/* [한국어] 하나라도 만들었으면 성공이다. */
	return 0;

/* [한국어] 서비스 장치를 하나도 못 만든 경로 — 잡아 둔 벡터를 반납해야 한다. */
error_cleanup_irqs:
	/* [한국어] MSI-X/MSI 또는 INTx 벡터를 반납한다. */
	pci_free_irq_vectors(dev);
/* [한국어] IRQ 배분 자체가 실패한 경로 — 반납할 벡터가 없다. */
error_disable:
	/* [한국어] 위에서 켠 장치를 되돌린다. */
	pci_disable_device(dev);
	/* [한국어] 실패 원인을 그대로 전한다. */
	return status;
}

typedef int (*pcie_callback_t)(struct pcie_device *);

/* [한국어]
 * pcie_port_device_iter - 자식 서비스 장치 하나에서 지정한 콜백을 찾아 부른다
 *
 * @dev:  순회 중인 자식 device.
 * @data: 부를 콜백의 구조체 내 오프셋(size_t)을 가리키는 포인터.
 * @return: 콜백의 반환값. 콜백이 없거나 대상이 아니면 0.
 *
 * PM 과 오류 복구 콜백이 전부 "모든 자식 서비스에게 같은 종류의 콜백을
 * 전달한다" 는 같은 모양이라, 그 공통부를 하나로 묶은 함수다.
 *
 * 방식이 독특하다. 부를 콜백을 함수 포인터로 넘기는 대신
 * offsetof(struct pcie_port_service_driver, suspend) 같은 *오프셋* 을
 * 넘긴다. 그러면 여기서 드라이버 구조체 주소에 그 오프셋을 더해 원하는
 * 콜백 포인터를 꺼낼 수 있다. suspend/resume/runtime_suspend/... 마다
 * 똑같은 순회 함수를 따로 쓰지 않으려는 설계다.
 *
 * 자식 중에는 이 버스에 속하지 않은 것도 있을 수 있으므로 bus 를 확인하고,
 * 드라이버가 아직 바인딩되지 않았거나 그 콜백을 구현하지 않았으면
 * 조용히 0 을 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PM/오류 복구 경로).
 *
 * 호출 체인:  device_for_each_child() ← pcie_port_device_suspend() 등
 *               → [이 함수] → 서비스 드라이버의 해당 콜백
 */
static int pcie_port_device_iter(struct device *dev, void *data)
{
	struct pcie_port_service_driver *service_driver;
	size_t offset = *(size_t *)data;
	/* [한국어] 꺼내 부를 콜백 포인터. typedef 로 int (*)(struct pcie_device *) 를 미리 정해 두었다. */
	pcie_callback_t cb;

	if ((dev->bus == &pcie_port_bus_type) && dev->driver) {
		/* [한국어] struct device_driver 에서 바깥의 서비스 드라이버 구조체를 되찾는다. */
		service_driver = to_service_driver(dev->driver);
		/* [한국어] 구조체 주소에 오프셋을 더해 원하는 콜백 필드를 꺼낸다. 함수 포인터가
		 * 아니라 오프셋을 넘겨받는 이 방식 덕분에 suspend/resume/runtime_ 계열/slot_reset
		 * 마다 똑같은 순회 함수를 따로 쓰지 않아도 된다. */
		cb = *(pcie_callback_t *)((void *)service_driver + offset);
		/* [한국어] 그 드라이버가 이 콜백을 구현했으면 */
		if (cb)
			/* [한국어] 서비스 장치를 넘겨 부르고 결과를 그대로 전한다. 0 이 아니면
			 * device_for_each_child() 가 거기서 순회를 멈춘다. */
			return cb(to_pcie_device(dev));
	/* [한국어] 이 버스 소속이 아니거나 드라이버가 없으면 아래로 빠져 0 을 돌려준다. */
	}
	return 0;
}

#ifdef CONFIG_PM
/**
 * pcie_port_device_suspend - suspend port services associated with a PCIe port
 * @dev: PCI Express port to handle
 */
/* [한국어]
 * pcie_port_device_suspend - 절전 진입을 각 서비스 드라이버에게 전달한다
 *
 * @dev: PCIe 포트의 device.   @return: 자식 콜백 중 첫 실패의 errno, 없으면 0.
 *
 * 자기가 할 일은 없고 자식들에게 넘기기만 한다. 넘길 콜백을
 * offsetof 로 지정해 공통 순회 함수에 맡긴다.
 *
 * pm_ops 에서 .suspend 뿐 아니라 .freeze 와 .poweroff 자리에도 이 함수가
 * 쓰인다. 최대 절전 모드의 이미지 저장 직전과 전원 차단 직전이 서비스
 * 입장에서는 평범한 절전과 다를 바 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(시스템 절전).
 *
 * 호출 체인:  PM 코어 → [이 함수] → device_for_each_child()
 *               → pcie_port_device_iter() → 서비스의 suspend
 */
static int pcie_port_device_suspend(struct device *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, suspend);
	return device_for_each_child(dev, &off, pcie_port_device_iter);
}

/* [한국어]
 * pcie_port_device_resume_noirq - 인터럽트를 켜기 전 단계의 재개를 전달한다
 *
 * @dev: PCIe 포트의 device.   @return: 첫 실패의 errno, 없으면 0.
 *
 * noirq 단계는 인터럽트 처리가 아직 꺼져 있는 이른 재개 시점이다.
 * 서비스에 따라 인터럽트가 오기 전에 레지스터를 먼저 되돌려 놓아야
 * 하는 경우가 있어, 보통의 resume 과 별도로 이 단계가 필요하다.
 *
 * pm_ops 에서 .resume_noirq 와 .restore_noirq 자리에 함께 쓰인다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 다만 인터럽트가 비활성인 단계다.
 *
 * 호출 체인:  PM 코어 → [이 함수] → pcie_port_device_iter()
 *               → 서비스의 resume_noirq
 */
static int pcie_port_device_resume_noirq(struct device *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, resume_noirq);
	return device_for_each_child(dev, &off, pcie_port_device_iter);
/* [한국어] 넘길 콜백을 offsetof 로 지정해 공통 순회 함수에 맡긴다.
 * 이 함수는 pm_ops 의 .suspend, .freeze, .poweroff 세 자리에 함께 쓰인다. */
}

/**
 * pcie_port_device_resume - resume port services associated with a PCIe port
 * @dev: PCI Express port to handle
 */
/* [한국어]
 * pcie_port_device_resume - 재개를 각 서비스 드라이버에게 전달한다
 *
 * @dev: PCIe 포트의 device.   @return: 첫 실패의 errno, 없으면 0.
 *
 * pcie_port_device_suspend() 의 짝이며 구조가 같다. pm_ops 에서
 * .resume, .thaw, .restore 세 자리에 쓰인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(시스템 재개).
 *
 * 호출 체인:  PM 코어 → [이 함수] → pcie_port_device_iter()
 *               → 서비스의 resume
 */
static int pcie_port_device_resume(struct device *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, resume);
	return device_for_each_child(dev, &off, pcie_port_device_iter);
/* [한국어] noirq 단계용. 인터럽트가 아직 꺼진 이른 재개 시점에 불리며,
 * pm_ops 의 .resume_noirq 와 .restore_noirq 자리에 쓰인다. */
}

/**
 * pcie_port_device_runtime_suspend - runtime suspend port services
 * @dev: PCI Express port to handle
 */
/* [한국어]
 * pcie_port_device_runtime_suspend - 런타임 절전을 각 서비스에게 전달한다
 *
 * @dev: PCIe 포트의 device.   @return: 첫 실패의 errno, 없으면 0.
 *
 * 시스템 전체 절전이 아니라, 이 포트만 놀고 있어 D3 로 내려가는 경우다.
 * 서비스마다 별도 콜백을 두는 이유는 그때 해야 할 일이 시스템 절전과
 * 다를 수 있기 때문이다.
 *
 * 이 함수를 직접 pm_ops 에 꽂지 않고 pcie_port_runtime_suspend() 로
 * 한 겹 감싸는데, 그쪽에서 bridge_d3 를 먼저 확인한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(런타임 PM).
 *
 * 호출 체인:  pcie_port_runtime_suspend() → [이 함수]
 *               → pcie_port_device_iter() → 서비스의 runtime_suspend
 */
static int pcie_port_device_runtime_suspend(struct device *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, runtime_suspend);
	return device_for_each_child(dev, &off, pcie_port_device_iter);
/* [한국어] 보통의 재개. pm_ops 의 .resume, .thaw, .restore 세 자리에 쓰인다. */
}

/**
 * pcie_port_device_runtime_resume - runtime resume port services
 * @dev: PCI Express port to handle
 */
/* [한국어]
 * pcie_port_device_runtime_resume - 런타임 재개를 각 서비스에게 전달한다
 *
 * @dev: PCIe 포트의 device.   @return: 첫 실패의 errno, 없으면 0.
 *
 * 위 함수의 짝이다. 이쪽은 감싸는 겹 없이 pm_ops 의 .runtime_resume 에
 * 직접 꽂힌다 - 깨어나는 것은 조건을 따질 이유가 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(런타임 PM).
 *
 * 호출 체인:  PM 코어 → [이 함수] → pcie_port_device_iter()
 *               → 서비스의 runtime_resume
 */
static int pcie_port_device_runtime_resume(struct device *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, runtime_resume);
	return device_for_each_child(dev, &off, pcie_port_device_iter);
/* [한국어] 런타임 절전용. 시스템 전체가 아니라 이 포트만 놀아서 내려가는 경우다. */
}
#endif /* PM */

/* [한국어]
 * remove_iter - 자식이 서비스 장치면 등록을 해제한다
 *
 * @dev:  순회 중인 자식 device.
 * @data: 쓰지 않는다. device_for_each_child() 서명을 맞추기 위한 자리다.
 * @return: 항상 0 - 순회를 끝까지 계속하겠다는 뜻이다.
 *
 * device_for_each_child() 는 콜백이 0 이 아닌 값을 주면 거기서 순회를
 * 멈춘다. 여기서는 모든 서비스 장치를 지워야 하므로 항상 0 을 돌려준다.
 *
 * bus 검사가 필요한 이유는 포트의 자식에 서비스 장치가 아닌 것이 섞일 수
 * 있기 때문이다. device_unregister() 는 참조를 하나 놓을 뿐이고, 실제
 * 메모리 해제는 마지막 참조가 사라질 때 release_pcie_device() 가 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(포트 remove/shutdown).
 *
 * 호출 체인:  pcie_port_device_remove() → device_for_each_child()
 *               → [이 함수] → device_unregister()
 */
static int remove_iter(struct device *dev, void *data)
{
	if (dev->bus == &pcie_port_bus_type)
		device_unregister(dev);
	return 0;
}

/* [한국어]
 * find_service_iter - 자식 중 원하는 서비스의 장치를 찾으면 순회를 멈춘다
 *
 * @device: 순회 중인 자식 device.
 * @data:   struct portdrv_service_data 포인터. 찾는 서비스 번호가 들어 있고,
 *          결과도 여기에 담아 돌려준다.
 * @return: 1 이면 찾았다는 뜻이라 순회가 멈춘다. 0 이면 계속한다.
 *
 * remove_iter() 와 반대로, 찾는 즉시 1 을 돌려 순회를 끊는다.
 * device_for_each_child() 의 반환 규약을 이렇게 양쪽으로 활용한다.
 *
 * 결과를 반환값이 아니라 @data 구조체에 담는 이유는 콜백 서명이 int 만
 * 돌려줄 수 있기 때문이다. 드라이버와 device 를 둘 다 담아 두지만,
 * 호출자인 pcie_port_find_device() 는 device 만 꺼내 쓴다.
 *
 * 드라이버가 바인딩된 장치만 본다(device->driver 검사). 서비스 비트는
 * 장치가 아니라 드라이버 쪽에서 읽는데, 결국 같은 값이지만 바인딩
 * 여부까지 함께 확인하려는 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pcie_port_find_device() → device_for_each_child() → [이 함수]
 */
static int find_service_iter(struct device *device, void *data)
{
	struct pcie_port_service_driver *service_driver;
	struct portdrv_service_data *pdrvs;
	/* [한국어] 찾는 서비스 비트를 지역 변수로 꺼내 둔다. */
	u32 service;

	pdrvs = (struct portdrv_service_data *) data;
	/* [한국어] 호출자가 채워 넣은 값이다. */
	service = pdrvs->service;

	if (device->bus == &pcie_port_bus_type && device->driver) {
		/* [한국어] 바깥의 서비스 드라이버 구조체를 되찾는다. */
		service_driver = to_service_driver(device->driver);
		/* [한국어] 드라이버가 담당하는 서비스가 찾는 것과 같은가. 서비스 비트를 장치가
		 * 아니라 드라이버에서 읽는 이유는 바인딩 여부까지 함께 확인하려는 것이다. */
		if (service_driver->service == service) {
			/* [한국어] 찾은 드라이버를 담아 두고(현재 호출자는 쓰지 않는다), */
			pdrvs->drv = service_driver;
			/* [한국어] 장치를 담는다. 이것이 실제 결과다. */
			pdrvs->dev = device;
			/* [한국어] 1 을 돌려 순회를 멈춘다. remove_iter() 가 항상 0 을 돌려 끝까지 도는 것과
			 * 정확히 반대로, 같은 반환 규약을 다른 목적에 쓴다. */
			return 1;
		}
	}

	return 0;
}

/**
 * pcie_port_find_device - find the struct device
 * @dev: PCI Express port the service is associated with
 * @service: For the service to find
 *
 * Find the struct device associated with given service on a pci_dev
 */
/* [한국어]
 * pcie_port_find_device - 포트에 매달린 특정 서비스 장치를 찾아 준다
 *
 * @dev:     대상 PCIe 포트.
 * @service: 찾는 서비스 비트(PCIE_PORT_SERVICE_AER 등).
 * @return: 그 서비스의 struct device. 없으면 NULL.
 *
 * 서비스 드라이버끼리 서로를 찾아야 할 때 쓰는 조회 함수다. 예컨대 AER
 * 오류 주입 기능은 "이 Root Port 의 AER 서비스 장치" 를 찾아 그 IRQ 를
 * 인위적으로 울려야 한다.
 *
 * pdrvs.dev 를 NULL 로 초기화해 두고 순회를 돌린 뒤 그 값을 그대로
 * 돌려준다. 찾지 못하면 콜백이 손대지 않으므로 NULL 이 남는다.
 * pdrvs.drv 는 콜백이 채우지만 여기서 쓰지 않는다.
 *
 * 참조 계수를 올리지 않고 포인터만 돌려주는 점을 유의해야 한다. 호출자가
 * 포트의 수명 안에서만 쓰는 것을 전제한다.
 *
 * EXPORT_SYMBOL_GPL 로 공개된다. 이 트리에서 확인한 호출자는
 * drivers/pci/pcie/aer_inject.c:1100 하나다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  aer_inject → [이 함수] → device_for_each_child()
 *               → find_service_iter()
 */
struct device *pcie_port_find_device(struct pci_dev *dev,
			      u32 service)
{
	struct device *device;
	struct portdrv_service_data pdrvs;
/* [한국어] 찾지 못하면 콜백이 손대지 않으므로 NULL 이 그대로 남는다. */

	pdrvs.dev = NULL;
	/* [한국어] 찾을 서비스를 지정한다. */
	pdrvs.service = service;
	/* [한국어] 포트의 자식들을 훑는다. 찾으면 콜백이 1 을 돌려 순회가 멈춘다. */
	device_for_each_child(&dev->dev, &pdrvs, find_service_iter);

	device = pdrvs.dev;
	/* [한국어] 참조 계수를 올리지 않고 포인터만 돌려준다 — 호출자가 포트의 수명 안에서만
	 * 쓰는 것을 전제한다. */
	return device;
}
EXPORT_SYMBOL_GPL(pcie_port_find_device);

/**
 * pcie_port_device_remove - unregister PCI Express port service devices
 * @dev: PCI Express port the service devices to unregister are associated with
 *
 * Remove PCI Express port service devices associated with given port and
 * disable MSI-X or MSI for the port.
 */
/* [한국어]
 * pcie_port_device_remove - 서비스 장치들을 모두 없애고 벡터를 반납한다
 *
 * @dev: 대상 PCIe 포트.   @return: 없음.
 *
 * pcie_port_device_register() 의 역이다. 자식 서비스 장치를 전부 등록
 * 해제하고, 포트가 잡고 있던 MSI-X/MSI(또는 INTx) 벡터를 반납한다.
 *
 * 순서가 중요하다. 서비스 장치를 먼저 없애야 그 드라이버들이 자기 IRQ
 * 핸들러를 떼어 낸다. 벡터를 먼저 반납하면 아직 핸들러가 걸린 IRQ 를
 * 해제하게 된다.
 *
 * pci_disable_device() 는 여기서 하지 않는다 - 호출자인
 * pcie_portdrv_remove() 가 하고, pcie_portdrv_shutdown() 은 하지 않는다.
 * shutdown 은 시스템이 곧 꺼지는 상황이라 포트를 굳이 끄지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pcie_portdrv_remove() / pcie_portdrv_shutdown() → [이 함수]
 *               → device_for_each_child() → remove_iter() → pci_free_irq_vectors()
 */
static void pcie_port_device_remove(struct pci_dev *dev)
{
	device_for_each_child(&dev->dev, NULL, remove_iter);
	pci_free_irq_vectors(dev);
}

/* [한국어]
 * pcie_port_bus_match - 서비스 장치와 서비스 드라이버가 짝인지 판정한다
 *
 * @dev: 후보 서비스 장치.   @drv: 후보 서비스 드라이버.
 * @return: 1 이면 짝, 0 이면 아니다.
 *
 * 가상 버스의 핵심 콜백이다. 드라이버 코어는 이 버스에 장치나 드라이버가
 * 등록될 때마다 모든 조합에 대해 이 함수를 불러 짝을 찾는다.
 *
 * 조건이 둘이다.
 *   1) 서비스 비트가 같아야 한다. AER 드라이버가 HP 장치에 붙으면 안 된다.
 *   2) 드라이버가 포트 종류를 특정했다면 그것도 맞아야 한다.
 *      PCIE_ANY_PORT 는 종류를 가리지 않겠다는 뜻이라 이 검사를 건너뛴다.
 *      Root Port 에서만 의미가 있는 서비스와 Downstream Port 에서도
 *      동작하는 서비스가 섞여 있기 때문에 필요한 구분이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(드라이버 코어의 매칭).
 *
 * 호출 체인:  드라이버 코어 → [이 함수]
 */
static int pcie_port_bus_match(struct device *dev, const struct device_driver *drv)
{
	struct pcie_device *pciedev = to_pcie_device(dev);
	const struct pcie_port_service_driver *driver = to_service_driver(drv);
/* [한국어] 담당 서비스가 다르면 */

	if (driver->service != pciedev->service)
		/* [한국어] 짝이 아니다. AER 드라이버가 HP 장치에 붙으면 안 된다. */
		return 0;

	if (driver->port_type != PCIE_ANY_PORT &&
	    /* [한국어] 드라이버가 포트 종류를 특정했는데 실제 종류와 다르면 역시 짝이 아니다.
	     * PCIE_ANY_PORT 는 종류를 가리지 않겠다는 뜻이라 이 검사를 건너뛴다. */
	    driver->port_type != pci_pcie_type(pciedev->port))
		return 0;

	return 1;
/* [한국어] 두 조건을 모두 통과하면 짝이다. */
}

/**
 * pcie_port_bus_probe - probe driver for given PCI Express port service
 * @dev: PCI Express port service device to probe against
 *
 * If PCI Express port service driver is registered with
 * pcie_port_service_register(), this function will be called by the driver core
 * whenever match is found between the driver and a port service device.
 */
/* [한국어]
 * pcie_port_bus_probe - 짝을 찾은 서비스 드라이버의 probe 를 부른다
 *
 * @dev: 바인딩할 서비스 장치.
 * @return: 0 성공. -ENODEV 는 드라이버가 없거나 probe 를 구현하지 않은 경우,
 *          그 밖에는 서비스 드라이버 probe 의 반환값.
 *
 * 버스의 .probe 콜백이다. 드라이버 코어가 넘긴 struct device_driver 를
 * struct pcie_port_service_driver 로 되돌려 그 probe 를 부른다.
 *
 * 성공했을 때 get_device() 로 참조를 하나 더 잡는 것이 요점이다. 짝이
 * 되어 있는 동안 장치가 사라지지 않게 붙잡아 두는 것이고, 아래
 * pcie_port_bus_remove() 의 put_device() 와 짝을 이룬다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  드라이버 코어 → [이 함수] → 서비스 드라이버의 probe
 */
static int pcie_port_bus_probe(struct device *dev)
{
	struct pcie_device *pciedev;
	struct pcie_port_service_driver *driver;
	/* [한국어] 서비스 드라이버 probe 의 반환값. */
	int status;

	driver = to_service_driver(dev->driver);
	/* [한국어] 드라이버가 없거나 probe 를 구현하지 않았으면 */
	if (!driver || !driver->probe)
		/* [한국어] 바인딩할 수 없다. */
		return -ENODEV;

	pciedev = to_pcie_device(dev);
	/* [한국어] 서비스 드라이버의 probe 를 부른다. 여기서부터 aer.c/pme.c 등의 코드가 돈다. */
	status = driver->probe(pciedev);
	/* [한국어] probe 가 실패했으면 */
	if (status)
		/* [한국어] 그 errno 를 전한다. 아래 get_device() 를 건너뛰므로 참조도 늘지 않는다. */
		return status;
/* [한국어] 성공했으니 참조를 하나 더 잡는다(다음 줄). 짝이 되어 있는 동안 장치가
 * 사라지지 않게 붙잡아 두는 것이고, pcie_port_bus_remove() 의 put_device() 와 짝이다. */

	get_device(dev);
	return 0;
}

/**
 * pcie_port_bus_remove - detach driver from given PCI Express port service
 * @dev: PCI Express port service device to handle
 *
 * If PCI Express port service driver is registered with
 * pcie_port_service_register(), this function will be called by the driver core
 * when device_unregister() is called for the port service device associated
 * with the driver.
 */
/* [한국어]
 * pcie_port_bus_remove - 서비스 드라이버를 장치에서 떼어 낸다
 *
 * @dev: 바인딩을 풀 서비스 장치.   @return: 없음.
 *
 * 버스의 .remove 콜백이다. 서비스 드라이버가 remove 를 구현했으면 부르고,
 * pcie_port_bus_probe() 가 잡아 둔 참조를 놓는다.
 *
 * 그 put_device() 가 마지막 참조라면 release_pcie_device() 가 이어서
 * 불려 struct pcie_device 가 해제된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  드라이버 코어(device_unregister 등) → [이 함수]
 *               → 서비스 드라이버의 remove → put_device()
 */
static void pcie_port_bus_remove(struct device *dev)
{
	struct pcie_device *pciedev;
	struct pcie_port_service_driver *driver;
/* [한국어] 바깥의 서비스 장치를 되찾아 아래 콜백에 넘긴다(윗줄에서 이미 얻었다). */

	pciedev = to_pcie_device(dev);
	/* [한국어] 떼어 낼 서비스 드라이버. */
	driver = to_service_driver(dev->driver);
	/* [한국어] 드라이버가 remove 를 구현했으면 */
	if (driver && driver->remove)
		/* [한국어] 부른다. 서비스 드라이버가 IRQ 핸들러를 떼고 자기 자원을 정리한다. */
		driver->remove(pciedev);

	put_device(dev);
}

const struct bus_type pcie_port_bus_type = {
	/* [한국어] 버스 이름. /sys/bus/pci_express/ 로 노출된다. */
	.name = "pci_express",
	/* [한국어] 장치와 드라이버를 짝짓는 판정 콜백. */
	.match = pcie_port_bus_match,
	.probe = pcie_port_bus_probe,
	.remove = pcie_port_bus_remove,
};

/**
 * pcie_port_service_register - register PCI Express port service driver
 * @new: PCI Express port service driver to register
 */
/* [한국어]
 * pcie_port_service_register - 서비스 드라이버를 이 가상 버스에 등록한다
 *
 * @new: 등록할 서비스 드라이버.
 * @return: 0 성공. -ENODEV 는 "pcie_ports=compat" 으로 이 체계가 통째로
 *          꺼져 있는 경우. 그 밖에는 driver_register() 의 errno.
 *
 * aer.c, pme.c, dpc.c, bwctrl.c, pciehp_core.c 가 자기 모듈 초기화에서
 * 부르는 진입점이다. 이름과 버스를 채워 넣고 드라이버 코어에 넘긴다.
 *
 * 맨 앞의 pcie_ports_disabled 검사가 안전장치다. 사용자가 이 체계를
 * 끄기로 했으면 서비스 드라이버가 아예 등록되지 않아야 한다. 등록만
 * 막으면 되는 이유는, 등록되지 않은 드라이버에는 매칭이 일어나지 않아
 * 어떤 서비스 장치에도 붙지 않기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 초기화).
 *
 * 호출 체인:  각 서비스 드라이버의 init → [이 함수] → driver_register()
 */
int pcie_port_service_register(struct pcie_port_service_driver *new)
{
	if (pcie_ports_disabled)
		return -ENODEV;

	new->driver.name = new->name;
	/* [한국어] 이 드라이버가 소속될 버스를 이 파일의 가상 버스로 지정한다.
	 * 이 한 줄이 서비스 드라이버를 PCI 버스가 아니라 pci_express 버스에 올린다. */
	new->driver.bus = &pcie_port_bus_type;

	return driver_register(&new->driver);
/* [한국어] 드라이버 코어에 등록한다. 등록되는 순간 이미 있는 서비스 장치들과
 * 매칭이 일어나 짝이 맞으면 probe 까지 이어진다. */
}

/**
 * pcie_port_service_unregister - unregister PCI Express port service driver
 * @drv: PCI Express port service driver to unregister
 */
/* [한국어]
 * pcie_port_service_unregister - 서비스 드라이버를 버스에서 뗀다
 *
 * @drv: 해제할 서비스 드라이버.   @return: 없음.
 *
 * 위 함수의 짝이며 driver_unregister() 로 넘기기만 한다. 그 안에서
 * 바인딩된 모든 장치에 대해 pcie_port_bus_remove() 가 불린다.
 *
 * 등록 때와 달리 pcie_ports_disabled 를 보지 않는데, 꺼져 있었다면
 * 애초에 등록되지 않았고 driver_unregister() 가 그런 드라이버에
 * 안전하게 동작하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 해제).
 *
 * 호출 체인:  각 서비스 드라이버의 exit → [이 함수] → driver_unregister()
 */
void pcie_port_service_unregister(struct pcie_port_service_driver *drv)
{
	driver_unregister(&drv->driver);
}

/* If this switch is set, PCIe port native services should not be enabled. */
bool pcie_ports_disabled;

/*
 * If the user specified "pcie_ports=native", use the PCIe services regardless
 * of whether the platform has given us permission.  On ACPI systems, this
 * means we ignore _OSC.
 */
bool pcie_ports_native;

/*
 * If the user specified "pcie_ports=dpc-native", use the Linux DPC PCIe
 * service even if the platform hasn't given us permission.
 */
bool pcie_ports_dpc_native;

/* [한국어]
 * pcie_port_setup - "pcie_ports=" 부팅 인자를 해석한다
 *
 * @str: '=' 뒤의 문자열.   @return: 항상 1 - 인자를 처리했다는 뜻이다.
 *
 * 세 가지 값을 받는다.
 *   compat     - 이 포트 버스 체계를 통째로 끈다. 서비스 드라이버가
 *                등록되지 않아 AER/HP/PME/DPC/BWCTRL 이 모두 동작하지 않는다.
 *   native     - 플랫폼(_OSC)이 권한을 주지 않아도 서비스를 쓴다.
 *                ACPI 시스템에서 _OSC 협상 결과를 무시하겠다는 뜻이며,
 *                상류 주석이 그 점을 명시한다.
 *   dpc-native - DPC 만 강행한다. AER 권한이 없어도 DPC 는 쓰겠다는 뜻이다.
 *
 * strncmp 의 비교 길이가 각 문자열 길이와 같아 접두어만 맞으면 걸린다.
 * "native" 검사가 "dpc-native" 보다 앞에 있지만, "dpc-native" 는 'd' 로
 * 시작해 "native" 와 앞 6글자가 다르므로 서로 가로채지 않는다.
 *
 * __setup 매크로가 이 함수를 커널 커맨드라인 처리기로 등록한다.
 * 실행 컨텍스트: 부팅 초기(프로세스 컨텍스트).
 *
 * 호출 체인:  커널 커맨드라인 파서 → [이 함수]
 */
static int __init pcie_port_setup(char *str)
{
	if (!strncmp(str, "compat", 6))
		pcie_ports_disabled = true;
	/* [한국어] "native" — _OSC 협상 결과를 무시하고 서비스를 쓴다. */
	else if (!strncmp(str, "native", 6))
		/* [한국어] 전역 플래그를 세운다. get_port_device_capability() 가 이 값을 보고
		 * host->native_* 검사를 건너뛴다. */
		pcie_ports_native = true;
	/* [한국어] "dpc-native" — DPC 만 강행한다. 앞의 "native" 검사와 앞 6글자가 달라
	 * 서로 가로채지 않는다. */
	else if (!strncmp(str, "dpc-native", 10))
		/* [한국어] DPC 전용 플래그를 세운다. */
		pcie_ports_dpc_native = true;

	return 1;
/* [한국어] __setup 규약상 1 은 "이 인자를 처리했다" 는 뜻이다. */
}
__setup("pcie_ports=", pcie_port_setup);

/* global data */

#ifdef CONFIG_PM
/* [한국어]
 * pcie_port_runtime_suspend - 포트가 D3 로 갈 수 있을 때만 런타임 절전을 진행한다
 *
 * @dev: PCIe 포트의 device.
 * @return: 0 성공. -EBUSY 는 이 포트를 D3 로 내리면 안 된다는 뜻이라
 *          PM 코어가 절전을 포기한다.
 *
 * pcie_port_device_runtime_suspend() 앞에 조건 하나를 덧붙인 겹이다.
 * pci_dev->bridge_d3 는 PCI 코어가 "이 브리지는 D3 로 내려도 안전하다" 고
 * 판단해 세워 두는 값이며, pci.c 의 화이트리스트와 "pcie_port_pm=" 부팅
 * 인자가 그 판단에 관여한다(그 파라미터 처리는 이 파일이 아니라
 * pci.c:342 의 pcie_port_pm_setup() 에 있다).
 *
 * 그 값이 서 있지 않은데 절전을 진행하면 아래 장치들이 깨어나지 못하는
 * 상황이 생길 수 있어, 아예 -EBUSY 로 거절한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(런타임 PM).
 *
 * 호출 체인:  PM 코어 → [이 함수] → pcie_port_device_runtime_suspend()
 */
static int pcie_port_runtime_suspend(struct device *dev)
{
	if (!to_pci_dev(dev)->bridge_d3)
		return -EBUSY;

	return pcie_port_device_runtime_suspend(dev);
}

/* [한국어]
 * pcie_port_runtime_idle - 이 포트를 지금 절전으로 내려도 되는지 알려 준다
 *
 * @dev: PCIe 포트의 device.
 * @return: 0 이면 내려도 좋다. -EBUSY 면 안 된다.
 *
 * PM 코어는 장치가 놀기 시작하면 이 콜백으로 의사를 묻는다. 여기서는
 * 판단을 직접 하지 않고 PCI 코어가 세워 둔 bridge_d3 를 그대로 전한다.
 *
 * 상류 주석이 그 이유를 밝힌다 - 포트를 실제로 D3 로 옮기는 일을 포함해
 * 나머지는 전부 PCI 코어가 처리하므로, 이 드라이버는 코어의 판단을
 * 따르기만 하면 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(런타임 PM).
 *
 * 호출 체인:  PM 코어 → [이 함수]
 */
static int pcie_port_runtime_idle(struct device *dev)
{
	/*
	 * Assume the PCI core has set bridge_d3 whenever it thinks the port
	 * should be good to go to D3.  Everything else, including moving
	 * the port to D3, is handled by the PCI core.
	 */
	return to_pci_dev(dev)->bridge_d3 ? 0 : -EBUSY;
}

static const struct dev_pm_ops pcie_portdrv_pm_ops = {
	/* [한국어] 시스템 절전 진입. 아래 freeze/poweroff 와 같은 함수를 쓰는데,
	 * 서비스 입장에서는 세 경우가 다를 바 없기 때문이다. */
	.suspend	= pcie_port_device_suspend,
	/* [한국어] 인터럽트를 켜기 전 단계의 재개. restore_noirq 와 공유한다. */
	.resume_noirq	= pcie_port_device_resume_noirq,
	.resume		= pcie_port_device_resume,
	.freeze		= pcie_port_device_suspend,
	.thaw		= pcie_port_device_resume,
	.poweroff	= pcie_port_device_suspend,
	.restore_noirq	= pcie_port_device_resume_noirq,
	.restore	= pcie_port_device_resume,
	.runtime_suspend = pcie_port_runtime_suspend,
	.runtime_resume	= pcie_port_device_runtime_resume,
	.runtime_idle	= pcie_port_runtime_idle,
};

/* [한국어] 아래 struct pci_driver 의 .driver.pm 자리에 넣을 값.
 * CONFIG_PM 이 켜져 있으면 위에서 정의한 콜백 묶음의 주소이고,
 * 꺼져 있으면 아래 #else 에서 NULL 로 정의된다.
 * 이렇게 매크로로 감싸 두면 드라이버 구조체 초기화 부분을 #ifdef 로
 * 두 번 쓰지 않아도 된다. */
#define PCIE_PORTDRV_PM_OPS	(&pcie_portdrv_pm_ops)

#else /* !PM */

#define PCIE_PORTDRV_PM_OPS	NULL
#endif /* !PM */

/*
 * pcie_portdrv_probe - Probe PCI-Express port devices
 * @dev: PCI-Express port device being probed
 *
 * If detected invokes the pcie_port_device_register() method for
 * this port device.
 *
 */
/* [한국어]
 * pcie_portdrv_probe - PCIe 포트에 바인딩되어 서비스 체계를 세운다
 *
 * @dev: 바인딩 대상 PCI 장치.
 * @id:  매칭에 쓰인 id 항목. 여기서는 쓰지 않는다.
 * @return: 0 성공, -ENODEV 는 포트가 아니라 이 드라이버가 맡을 대상이 아닌
 *          경우, 그 밖에는 pcie_port_device_register() 의 errno.
 *
 * id_table 이 클래스 기준이라 PCI-to-PCI 브리지가 전부 걸려 든다. 그래서
 * 맨 먼저 진짜 대상인지 걸러 낸다 - PCIe 여야 하고, 종류가 Root Port /
 * Upstream / Downstream / RC Event Collector 중 하나여야 한다. 옛 PCI
 * 브리지는 여기서 -ENODEV 로 물러난다.
 *
 * RC Event Collector 라면 pcie_link_rcec() 로 자기가 담당할 RCiEP 들과
 * 연결해 둔다. RCEC 는 자기 아래 버스가 없고 별도로 지정된 장치들의
 * 오류를 대신 보고하는 구조라, 그 관계를 미리 맺어 두어야 한다.
 *
 * 그다음 pcie_port_device_register() 로 서비스 장치들을 만들고,
 * pci_save_state() 로 현재 config 를 저장한다. 저장이 필요한 이유는
 * 아래 pcie_portdrv_slot_reset() 이 리셋 후 이 값을 되돌리기 때문이다.
 *
 * 드라이버 PM 플래그 둘도 의미가 있다. NO_DIRECT_COMPLETE 는 자식이
 * 있는 브리지라 절전을 건너뛰면 안 된다는 뜻이고, SMART_SUSPEND 는
 * 이미 런타임 절전 중이면 시스템 절전 때 굳이 깨우지 말라는 뜻이다.
 *
 * 마지막으로 D3 가 가능한 포트라면 런타임 PM 자동 절전을 켜되 100ms 의
 * 지연을 둔다. 상류 주석대로 lspci 같은 도구가 config 를 훑을 때
 * 포트가 잠들었다 깨기를 반복하는 것을 막기 위해서다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(드라이버 바인딩).
 *
 * 호출 체인:  pci-driver.c 의 바인딩 → [이 함수]
 *               → pcie_link_rcec() → pcie_port_device_register()
 *               → pci_save_state() → pm_runtime_allow()
 */
static int pcie_portdrv_probe(struct pci_dev *dev,
				const struct pci_device_id *id)
{
	int type = pci_pcie_type(dev);
	int status;
/* [한국어] PCIe 장치가 아니거나 */

	if (!pci_is_pcie(dev) ||
	    /* [한국어] 종류가 Root Port 도 */
	    ((type != PCI_EXP_TYPE_ROOT_PORT) &&
	     (type != PCI_EXP_TYPE_UPSTREAM) &&
	     (type != PCI_EXP_TYPE_DOWNSTREAM) &&
	     (type != PCI_EXP_TYPE_RC_EC)))
		return -ENODEV;

	if (type == PCI_EXP_TYPE_RC_EC)
		/* [한국어] RC Event Collector 는 자기 아래 버스가 없고 별도로 지정된 RCiEP 들의
		 * 오류를 대신 보고하는 구조라, 그 담당 관계를 미리 맺어 둔다. */
		pcie_link_rcec(dev);

	status = pcie_port_device_register(dev);
	/* [한국어] 서비스 등록이 실패했으면 */
	if (status)
		/* [한국어] errno 를 전해 바인딩을 포기한다. */
		return status;
/* [한국어] 성공했으므로 아래에서 config 상태를 저장한다. */

	pci_save_state(dev);

	dev_pm_set_driver_flags(&dev->dev, DPM_FLAG_NO_DIRECT_COMPLETE |
					   /* [한국어] SMART_SUSPEND — 이미 런타임 절전 중이면 시스템 절전 때 굳이 깨우지 말라는 뜻.
					    * 윗줄의 NO_DIRECT_COMPLETE 는 자식이 있는 브리지라 절전을 건너뛰면 안 된다는 뜻이다. */
					   DPM_FLAG_SMART_SUSPEND);

	if (pci_bridge_d3_possible(dev)) {
		/*
		 * Keep the port resumed 100ms to make sure things like
		 * config space accesses from userspace (lspci) will not
		 * cause the port to repeatedly suspend and resume.
		 */
		pm_runtime_set_autosuspend_delay(&dev->dev, 100);
		pm_runtime_use_autosuspend(&dev->dev);
		pm_runtime_mark_last_busy(&dev->dev);
		pm_runtime_put_autosuspend(&dev->dev);
		pm_runtime_allow(&dev->dev);
	}

	return 0;
}

/* [한국어]
 * pcie_portdrv_remove - 포트에서 이 드라이버를 떼어 낸다
 *
 * @dev: 대상 PCIe 포트.   @return: 없음.
 *
 * probe 의 역순이다. D3 가 가능한 포트였다면 런타임 PM 설정부터 되돌린다.
 *   pm_runtime_forbid()            - 사용자 공간이 허용해 둔 것을 막고
 *   pm_runtime_get_noresume()      - 참조를 잡아 절전으로 내려가지 못하게 하고
 *   pm_runtime_dont_use_autosuspend() - 자동 절전을 끈다
 * 이 셋을 먼저 하는 이유는 제거 도중 포트가 절전으로 내려가면 아래
 * 서비스 장치를 정리하는 config 접근이 실패하기 때문이다.
 *
 * 그다음 서비스 장치와 벡터를 정리하고 마지막에 포트를 끈다.
 * pci_disable_device() 가 probe 의 pci_enable_device() 와 짝이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pci-driver.c 의 언바인딩 → [이 함수]
 *               → pcie_port_device_remove() → pci_disable_device()
 */
static void pcie_portdrv_remove(struct pci_dev *dev)
{
	if (pci_bridge_d3_possible(dev)) {
		pm_runtime_forbid(&dev->dev);
		pm_runtime_get_noresume(&dev->dev);
		pm_runtime_dont_use_autosuspend(&dev->dev);
	}

	pcie_port_device_remove(dev);

	pci_disable_device(dev);
}

/* [한국어]
 * pcie_portdrv_shutdown - 시스템 종료/재부팅 직전 정리
 *
 * @dev: 대상 PCIe 포트.   @return: 없음.
 *
 * pcie_portdrv_remove() 와 거의 같지만 마지막 pci_disable_device() 가
 * 없다. 시스템이 곧 꺼지거나 재부팅되는 상황이라 포트를 굳이 끌 이유가
 * 없고, 끄는 도중 문제가 생기면 오히려 종료가 막힐 수 있기 때문이다.
 *
 * 서비스 장치를 정리하고 벡터를 반납하는 것까지는 하는데, kexec 로
 * 다음 커널이 뜰 때 정리되지 않은 인터럽트가 남아 있으면 곤란해서다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(시스템 종료).
 *
 * 호출 체인:  커널 종료 경로 → [이 함수] → pcie_port_device_remove()
 */
static void pcie_portdrv_shutdown(struct pci_dev *dev)
{
	if (pci_bridge_d3_possible(dev)) {
		pm_runtime_forbid(&dev->dev);
		pm_runtime_get_noresume(&dev->dev);
		pm_runtime_dont_use_autosuspend(&dev->dev);
	}

	pcie_port_device_remove(dev);
}

/* [한국어]
 * pcie_portdrv_error_detected - 오류가 감지됐을 때 포트가 낼 첫 의견
 *
 * @dev:   오류가 난 포트.
 * @error: 채널 상태. pci_channel_io_frozen 이면 링크가 얼어붙어 config
 *         접근조차 되지 않는 상태다.
 * @return: PCI_ERS_RESULT_NEED_RESET 또는 PCI_ERS_RESULT_CAN_RECOVER.
 *
 * PCI 오류 복구는 관련된 모든 드라이버의 의견을 모아 다음 단계를 정한다.
 * 이 함수는 포트 드라이버의 의견이다.
 *
 * 얼어붙은 채널이면 슬롯 리셋을 요구한다 - 그 상태에서는 소프트웨어가
 * 할 수 있는 일이 없고 링크를 다시 세우는 수밖에 없기 때문이다.
 * 그 밖에는 복구 가능하다고 답해, 실제 판단을 아래 장치 드라이버들의
 * 의견에 맡긴다.
 *
 * 자식 서비스에게 전달하지 않는 점이 slot_reset 과 다르다. 이 단계는
 * 포트 자신의 상태 판단이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(AER 복구 워커).
 *
 * 호출 체인:  AER 복구 → err_handler->error_detected → [이 함수]
 */
static pci_ers_result_t pcie_portdrv_error_detected(struct pci_dev *dev,
					pci_channel_state_t error)
{
	if (error == pci_channel_io_frozen)
		return PCI_ERS_RESULT_NEED_RESET;
	return PCI_ERS_RESULT_CAN_RECOVER;
}

/* [한국어]
 * pcie_portdrv_slot_reset - 슬롯 리셋 후 포트와 서비스들을 되돌린다
 *
 * @dev: 리셋을 겪은 포트.
 * @return: 항상 PCI_ERS_RESULT_RECOVERED.
 *
 * 리셋은 config space 를 초기값으로 되돌리므로, 커널이 저장해 둔 상태를
 * 다시 써 넣어야 한다. 순서가 중요하다.
 *   1) 자식 서비스들의 slot_reset 콜백을 먼저 부른다. AER 이나 DPC 가
 *      자기 capability 를 다시 세팅한다.
 *   2) pci_restore_state() 로 포트의 config 를 되돌린다. 저장은
 *      pcie_portdrv_probe() 의 pci_save_state() 가 해 두었다.
 *
 * 항상 RECOVERED 를 돌려주는 것은 포트 자신은 리셋으로 되살아난다고
 * 보기 때문이다. 아래 장치가 살아났는지는 그 드라이버들이 따로 답한다.
 *
 * 콜백 전달에 pcie_port_device_iter() 를 쓰므로 PM 경로와 같은 방식이다.
 * 실행 컨텍스트: 프로세스 컨텍스트(AER 복구 워커).
 *
 * 호출 체인:  AER 복구 → err_handler->slot_reset → [이 함수]
 *               → pcie_port_device_iter() → pci_restore_state()
 */
static pci_ers_result_t pcie_portdrv_slot_reset(struct pci_dev *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, slot_reset);
	device_for_each_child(&dev->dev, &off, pcie_port_device_iter);
/* [한국어] 자식 서비스들의 slot_reset 을 먼저 부른 뒤(윗줄) 포트의 config 를 되돌린다.
 * 순서가 반대면 서비스가 되살리는 설정을 pci_restore_state() 가 덮어쓴다. */

	pci_restore_state(dev);
	return PCI_ERS_RESULT_RECOVERED;
}

/* [한국어]
 * pcie_portdrv_mmio_enabled - MMIO 가 다시 켜졌음을 알리는 단계
 *
 * @dev: 대상 포트.   @return: 항상 PCI_ERS_RESULT_RECOVERED.
 *
 * 오류 복구 중 MMIO 접근이 복구된 시점에 불린다. 포트 드라이버는 이
 * 단계에서 할 일이 없어 "괜찮다" 고만 답한다.
 *
 * 콜백을 아예 비워 두지 않고 두는 이유는, err_handler 에 이 항목이
 * 없으면 복구 코어가 다른 기본 동작을 고를 수 있기 때문이다. 명시적으로
 * RECOVERED 를 돌려주어 복구 흐름이 다음 단계로 넘어가게 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(AER 복구 워커).
 *
 * 호출 체인:  AER 복구 → err_handler->mmio_enabled → [이 함수]
 */
static pci_ers_result_t pcie_portdrv_mmio_enabled(struct pci_dev *dev)
{
	return PCI_ERS_RESULT_RECOVERED;
}

/*
 * LINUX Device Driver Model
 */
static const struct pci_device_id port_pci_ids[] = {
	/* handle any PCI-Express port */
	{ PCI_DEVICE_CLASS(PCI_CLASS_BRIDGE_PCI_NORMAL, ~0) },
	/* subtractive decode PCI-to-PCI bridge, class type is 060401h */
	{ PCI_DEVICE_CLASS(PCI_CLASS_BRIDGE_PCI_SUBTRACTIVE, ~0) },
	/* handle any Root Complex Event Collector */
	{ PCI_DEVICE_CLASS(((PCI_CLASS_SYSTEM_RCEC << 8) | 0x00), ~0) },
	{ },
};

static const struct pci_error_handlers pcie_portdrv_err_handler = {
	/* [한국어] 오류 감지 단계 — 포트 자신의 의견을 낸다. */
	.error_detected = pcie_portdrv_error_detected,
	/* [한국어] 슬롯 리셋 후 단계 — 자식 서비스에게 전달하고 config 를 되돌린다. */
	.slot_reset = pcie_portdrv_slot_reset,
	.mmio_enabled = pcie_portdrv_mmio_enabled,
};

static struct pci_driver pcie_portdriver = {
	/* [한국어] 드라이버 이름. lspci -k 의 "Kernel driver in use: pcieport" 가 이 값이다. */
	.name		= "pcieport",
	/* [한국어] 매칭할 장치 목록. 클래스 기준이라 PCI-to-PCI 브리지가 전부 걸려 들고,
	 * 진짜 대상인지는 pcie_portdrv_probe() 가 다시 걸러 낸다. */
	.id_table	= port_pci_ids,

	.probe		= pcie_portdrv_probe,
	/* [한국어] 언바인딩 시 호출. 아래 shutdown 과 달리 마지막에 장치를 끈다. */
	.remove		= pcie_portdrv_remove,
	.shutdown	= pcie_portdrv_shutdown,

	.err_handler	= &pcie_portdrv_err_handler,
/* [한국어] 이 드라이버가 DMA 매핑을 직접 관리한다고 알린다. 포트는 자신이 DMA 를
 * 하지 않고 아래 장치의 트래픽을 중계할 뿐이라, 코어가 IOMMU 도메인을
 * 임의로 떼어 내지 않게 하는 표시다. */

	.driver_managed_dma = true,
/* [한국어] CONFIG_PM 여부에 따라 위 매크로가 콜백 묶음 주소 또는 NULL 이 된다. */

	.driver.pm	= PCIE_PORTDRV_PM_OPS,
};

/* [한국어]
 * dmi_pcie_pme_disable_msi - 특정 기종에서 PME 를 MSI 로 받지 않게 한다
 *
 * @d: 매칭된 DMI 항목. 로그에 기종 이름을 찍는 데 쓴다.
 * @return: 항상 0.
 *
 * 아래 pcie_portdrv_dmi_table 의 콜백이다. 메인보드 식별 정보(DMI)가
 * 목록과 맞으면 pcie_pme_disable_msi() 를 불러 전역 플래그를 세우고,
 * 그러면 pcie_init_service_irqs() 가 PME 를 MSI 로 받지 않고 INTx 로
 * 내려간다.
 *
 * 특정 기종에서 PME 를 MSI 로 받으면 신호가 오지 않는 것이 알려져 있어
 * 만들어진 우회다. 현재 목록에는 MSI Wind U-100 한 기종만 있다.
 *
 * 어떤 기종 때문에 동작이 바뀌었는지 로그로 남기는 것이 중요하다 -
 * 나중에 인터럽트 방식이 다른 이유를 추적할 단서가 된다.
 *
 * 실행 컨텍스트: 부팅 초기(__init).
 *
 * 호출 체인:  dmi_check_system() ← pcie_portdrv_init() → [이 함수]
 *               → pcie_pme_disable_msi()
 */
static int __init dmi_pcie_pme_disable_msi(const struct dmi_system_id *d)
{
	pr_notice("%s detected: will not use MSI for PCIe PME signaling\n",
		  d->ident);
	pcie_pme_disable_msi();
	return 0;
}

static const struct dmi_system_id pcie_portdrv_dmi_table[] __initconst = {
	/*
	 * Boxes that should not use MSI for PCIe PME signaling.
	 */
	{
	 .callback = dmi_pcie_pme_disable_msi,
	 /* [한국어] 현재 목록에는 이 한 기종만 있다. 두 DMI 항목(제조사와 제품명)이
	  * 모두 맞아야 콜백이 불린다. */
	 .ident = "MSI Wind U-100",
	 .matches = {
		     DMI_MATCH(DMI_SYS_VENDOR,
				"MICRO-STAR INTERNATIONAL CO., LTD"),
		     DMI_MATCH(DMI_PRODUCT_NAME, "U-100"),
		     },
	 },
	 {}
};

/* [한국어]
 * pcie_init_services - 다섯 서비스 드라이버를 차례로 초기화한다
 *
 * @return: 없음.
 *
 * AER, PME, DPC, BWCTRL, HP 각각의 초기화 함수를 부른다. 이들이 안에서
 * pcie_port_service_register() 로 자신을 이 버스에 등록한다.
 *
 * 반환값을 보지 않는 것이 의도적이다. 어떤 서비스가 뜨지 못해도 나머지는
 * 동작해야 하기 때문이다 - AER 이 없다고 핫플러그까지 포기할 이유가 없다.
 * 해당 CONFIG_* 가 꺼져 있으면 이 함수들은 portdrv.h 의 빈 인라인 스텁이
 * 되어 아무 일도 하지 않는다.
 *
 * 포트 드라이버를 등록하기 *전에* 부르는 순서가 중요하다. 서비스
 * 드라이버가 먼저 등록되어 있어야, 포트가 바인딩되며 서비스 장치를
 * 만들 때 곧바로 짝이 맞아 probe 가 이어진다.
 *
 * 실행 컨텍스트: 부팅 초기(__init).
 *
 * 호출 체인:  pcie_portdrv_init() → [이 함수]
 *               → pcie_aer_init() / pcie_pme_init() / pcie_dpc_init()
 *               → pcie_bwctrl_init() / pcie_hp_init()
 */
static void __init pcie_init_services(void)
{
	pcie_aer_init();
	pcie_pme_init();
	pcie_dpc_init();
	pcie_bwctrl_init();
	pcie_hp_init();
}

/* [한국어]
 * pcie_portdrv_init - 이 파일의 진입점. 서비스를 세우고 포트 드라이버를 등록한다
 *
 * @return: 0 성공. -EACCES 는 "pcie_ports=compat" 으로 꺼 둔 경우,
 *          그 밖에는 pci_register_driver() 의 errno.
 *
 * device_initcall 로 등록되어 부팅 중 한 번 불린다.
 *
 * 순서가 셋이다.
 *   1) pcie_ports_disabled 면 아무것도 하지 않고 -EACCES 로 물러난다.
 *      서비스 드라이버 등록도, 포트 드라이버 등록도 하지 않으므로
 *      이 체계 전체가 존재하지 않는 것과 같아진다.
 *   2) pcie_init_services() 로 서비스 드라이버들을 먼저 등록한다.
 *   3) dmi_check_system() 으로 기종별 우회를 적용한 뒤
 *      pci_register_driver() 로 포트 드라이버를 PCI 버스에 올린다.
 *      이 시점부터 조건에 맞는 포트마다 pcie_portdrv_probe() 가 불린다.
 *
 * DMI 검사가 드라이버 등록보다 앞선 것도 의도적이다. 첫 포트가 probe 되기
 * 전에 PME 의 MSI 사용 여부가 정해져 있어야 하기 때문이다.
 *
 * 실행 컨텍스트: 부팅 초기(__init, 프로세스 컨텍스트).
 *
 * 호출 체인:  device_initcall → [이 함수] → pcie_init_services()
 *               → dmi_check_system() → pci_register_driver()
 */
static int __init pcie_portdrv_init(void)
{
	if (pcie_ports_disabled)
		return -EACCES;

	pcie_init_services();
	dmi_check_system(pcie_portdrv_dmi_table);

	return pci_register_driver(&pcie_portdriver);
/* [한국어] device_initcall 로 등록해 부팅 중 한 번 불리게 한다(다음 줄).
 * module_init 이 아닌 이유는 이 드라이버가 내장 전용이기 때문이다. */
}
device_initcall(pcie_portdrv_init);
