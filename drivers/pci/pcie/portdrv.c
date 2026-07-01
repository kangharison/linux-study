// SPDX-License-Identifier: GPL-2.0
/*
 * Purpose:	PCI Express Port Bus Driver
 *
 * Copyright (C) 2004 Intel
 * Copyright (C) Tom Long Nguyen (tom.l.nguyen@intel.com)
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/pcie/portdrv.c)은 PCIe Root Port(RP), Upstream
 * Port(USP), Downstream Port(DSP), Root Complex Event Collector(RCEC)
 * 등 PCIe 포트에서 동작하는 서비스(PME/AER/HP/DPC/BWCTRL)를 탐지하고
 * 등록하는 PCIe Port Bus Driver의 핵심이다.
 *
 * NVMe SSD 입장에서 이 포트 서비스들은 다음과 같이 직접 관련된다.
 *   - AER(Advanced Error Reporting): NVMe 메모리/TLP/CRC/ECRC 오류
 *     보고 경로. NVMe 엔드포인트에서 발생한 Uncorrectable/Correctable
 *     Error는 상위 Root Port의 AER 서비스가 수신, 처리 후 NVMe
 *     드라이버의 pci_error_handlers(.error_detected/.slot_reset)로
 *     연결될 수 있다.
 *   - DPC(Downstream Port Containment): NVMe 장치의 서프라이즈 제거,
 *     링크 다운, UR/CA 등으로 인한 시스템 손상을 포트 단에서 차단.
 *     특히 NVMe CMB/P2P DMA 사용 시 데이터 무결성 보호에 중요하다.
 *   - PME(Power Management Event): NVMe 장치가 D3cold 등 저전력 상태에서
 *     Resume 요청 시 Root Port가 PME 메시지를 전달. NVMe APST/PCIE ASPM
 *     런타임 전환과 연결된다.
 *   - BWCTRL(Bandwidth Change Notification): PCIe 링크 속도/폭 변경
 *     (Gen3->Gen4->Gen5, x4->x2 등) 시 알림. NVMe 대역폭 및 QoS에
 *     영향을 준다.
 *   - Hotplug: NVMe SSD 물리적 핫 추가/제거 시 Slot/Link 이벤트 처리.
 *
 * 일반적인 NVMe 드라이버와의 호출/영향 경로:
 *   nvme_probe() -> pci_enable_device(pdev) -> ...
 *   pcie_portdrv_probe() [이 파일]
 *     -> pcie_port_device_register()
 *        -> get_port_device_capability()    /* RP/DSP 서비스 마스크 생성 */
 *        -> pcie_init_service_irqs()        /* 서비스용 MSI/MSI-X/INTx 할당 */
 *           -> pcie_port_enable_irq_vec()
 *              -> pcie_message_numbers()    /* PME/AER/DPC 벡터 번호 읽기 */
 *        -> pcie_device_init()              /* 서비스별 pcie_device 생성 */
 *   이후 AER/DPC 서비스 드라이버가 로드되어 NVMe 장치 오류 발생 시
 *   포트 서비스가 먼저 감지하고, 필요 시 NVMe의 .err_handler 콜백을
 *   통해 복구(retry/reset)를 수행한다.
 *
 * 추가적으로 NVMe CMB/P2PDMA는 PCIe 주소 공간 상에서 host bridge를
 * 경유하거나 peer 장치 간 직접 버스 주소를 사용하므로, 포트의 AER/DPC
 * 동작과 맞물려 있고, MSI/MSI-X 벡터 할당 정책은 엔드포인트(NVMe)와
 * 포트 간 인터럽트 자원 분배에 영향을 준다.
 * ===================================================================
 */

#include <linux/bitfield.h> /* NVMe: 비트 필드 매크로 사용(PCIe 캐퍼빌리티 레지스터 파싱) */
#include <linux/dmi.h> /* NVMe: DMI 시스템 테이블 기반 PME MSI 비활성화 결정 */
#include <linux/init.h> /* NVMe: __init/__initconst/__setup 등 초기화 섹션 */
#include <linux/module.h> /* NVMe: 모듈 관련 매크로(EXPORT_SYMBOL_GPL 등) */
#include <linux/pci.h> /* NVMe: PCI/PCIe 핵심 구조체와 함수 정의 */
#include <linux/kernel.h> /* NVMe: 커널 기본 매크로 및 함수 */
#include <linux/errno.h> /* NVMe: 에러 코드 상수 */
#include <linux/pm.h> /* NVMe: 전원 관리 관련 정의 */
#include <linux/pm_runtime.h> /* NVMe: 런타임 전원 관리 API */
#include <linux/string.h> /* NVMe: 문자열/메모리 비교 함수 */
#include <linux/slab.h> /* NVMe: 메모리 할당(kzalloc_obj 등) */
#include <linux/aer.h> /* NVMe: AER(Advanced Error Reporting) 관련 정의 */

#include "../pci.h" /* NVMe: PCI 코어 남장기 구조체 및 함수 선언 */
#include "portdrv.h" /* NVMe: PCIe 포트 서비스 드라이버 남장기 헤더 */

/*
 * The PCIe Capability Interrupt Message Number (PCIe r3.1, sec 7.8.2) must
 * be one of the first 32 MSI-X entries.  Per PCI r3.0, sec 6.8.3.1, MSI
 * supports a maximum of 32 vectors per function.
 */
#define PCIE_PORT_MAX_MSI_ENTRIES	32 /* NVMe: PCIe 포트 서비스당 최대 32개 MSI/MSI-X 엔트리(PCIe 규격 상 첫 32개) */

#define get_descriptor_id(type, service) (((type - 4) << 8) | service) /* NVMe: 포트 타입과 서비스 조합으로 pcie_device 이름의 고유 식별자 생성 */

struct portdrv_service_data { /* NVMe: 포트 서비스 검색 시 드라이버/장치/서비스 정보를 전달하기 위한 임시 구조체 */
	struct pcie_port_service_driver *drv; /* NVMe: 검색된 포트 서비스 드라이버 포인터 */
	struct device *dev; /* NVMe: 검색된 서비스 장치(device) 포인터 */
	u32 service; /* NVMe: 찾으려는 서비스 마스크(PME/AER/DPC 등) */
};

/**
 * release_pcie_device - free PCI Express port service device structure
 * @dev: Port service device to release
 *
 * Invoked automatically when device is being removed in response to
 * device_unregister(dev).  Release all resources being claimed.
 */
/*
 * release_pcie_device:
 *   pcie_device 구조체를 해제한다. NVMe와 연결된 포트 서비스(AER/DPC 등)
 *   장치가 제거될 때 호출된다.
 */
static void release_pcie_device(struct device *dev)
{
	kfree(to_pcie_device(dev)); /* NVMe: device 포인터를 pcie_device로 변환 후 동적 메모리 해제. */
}

/*
 * Fill in *pme, *aer, *dpc with the relevant Interrupt Message Numbers if
 * services are enabled in "mask".  Return the number of MSI/MSI-X vectors
 * required to accommodate the largest Message Number.
 */
/*
 * pcie_message_numbers:
 *   포트에서 활성화할 서비스(PME/AER/DPC)들의 Interrupt Message Number를
 *   읽어 필요한 MSI/MSI-X 벡터 수를 계산한다. NVMe 장치의 상위 Root Port
 *   에서 AER/DPC 이벤트를 NVMe로 연결할 때 사용할 인터럽트 벡터를
 *   결정한다.
 */
static int pcie_message_numbers(struct pci_dev *dev, int mask,
				u32 *pme, u32 *aer, u32 *dpc)
{
	u32 nvec = 0, pos; /* NVMe: nvec은 필요 벡터 수, pos는 확장 캐퍼빌리티 오프셋 */
	u16 reg16; /* NVMe: 16비트 PCIe 캐퍼빌리티/확장 레지스터 읽기 버퍼 */

	/*
	 * The Interrupt Message Number indicates which vector is used, i.e.,
	 * the MSI-X table entry or the MSI offset between the base Message
	 * Data and the generated interrupt message.  See PCIe r3.1, sec
	 * 7.8.2, 7.10.10, 7.31.2.
	 */

	if (mask & (PCIE_PORT_SERVICE_PME | PCIE_PORT_SERVICE_HP |
		    PCIE_PORT_SERVICE_BWCTRL)) { /* NVMe: PME/HP/BWCTRL 중 하나라도 활성화되면 공용 PCIe 캐퍼빌리티 IRQ 번호를 읽는다. */
		pcie_capability_read_word(dev, PCI_EXP_FLAGS, &reg16); /* NVMe: PCIe 캐퍼빌리티 레지스터(PCI_EXP_FLAGS)에서 IRQ 필드 읽기. */
		*pme = FIELD_GET(PCI_EXP_FLAGS_IRQ, reg16); /* NVMe: PCIe 캐퍼빌리티의 Interrupt Message Number 추출. */
		nvec = *pme + 1; /* NVMe: 0번부터 pme번까지 사용하므로 총 pme+1개 벡터 필요. */
	}

#ifdef CONFIG_PCIEAER
	if (mask & PCIE_PORT_SERVICE_AER) { /* NVMe: AER 서비스가 활성화된 경우(NVMe 오류 감지 경로). */
		u32 reg32; /* NVMe: AER 레지스터 32비트 읽기 버퍼 */

		pos = dev->aer_cap; /* NVMe: pci_dev에 캐시된 AER 확장 캐퍼빌리티 오프셋 획득. */
		if (pos) { /* NVMe: AER 캐퍼빌리티가 존재할 때만 진행. */
			pci_read_config_dword(dev, pos + PCI_ERR_ROOT_STATUS,
				      &reg32); /* NVMe: Root Error Status 레지스터에서 AER IRQ 필드 읽기. */
			*aer = FIELD_GET(PCI_ERR_ROOT_AER_IRQ, reg32); /* NVMe: AER Interrupt Message Number 추출. */
			nvec = max(nvec, *aer + 1); /* NVMe: PME 벡터 수와 AER 벡터 수 중 큰 값을 필요 벡터 수로 갱신. */
		}
	}
#endif

	if (mask & PCIE_PORT_SERVICE_DPC) { /* NVMe: DPC 서비스가 활성화된 경우(NVMe 링크 다운/서프라이즈 제거 보호). */
		pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_DPC); /* NVMe: DPC 확장 캐퍼빌리티 오프셋 탐색. */
		if (pos) { /* NVMe: DPC 캐퍼빌리티가 존재할 때만 진행. */
			pci_read_config_word(dev, pos + PCI_EXP_DPC_CAP,
				     &reg16); /* NVMe: DPC Capability 레지스터 읽기. */
			*dpc = FIELD_GET(PCI_EXP_DPC_IRQ, reg16); /* NVMe: DPC Interrupt Message Number 추출. */
			nvec = max(nvec, *dpc + 1); /* NVMe: 필요 벡터 수를 DPC 기준으로 갱신. */
		}
	}

	return nvec; /* NVMe: PME/AER/DPC 중 가장 큰 Message Number에 기반한 총 벡터 수 반환. */
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
/*
 * pcie_port_enable_irq_vec:
 *   NVMe 장치 상위 PCIe 포트에 대해 MSI-X 또는 MSI 벡터를 할당하고,
 *   PME/AER/DPC 서비스에 실제 IRQ 번호를 연결한다. NVMe와 포트는
 *   별도의 pci_dev이므로 각자의 MSI/MSI-X 공간을 사용하지만, 시스템
 *   전체 벡터 자원 부족 시 NVMe 할당에 간접 영향을 줄 수 있다.
 */
static int pcie_port_enable_irq_vec(struct pci_dev *dev, int *irqs, int mask)
{
	int nr_entries, nvec, pcie_irq; /* NVMe: nr_entries=할당받은 벡터 수, nvec=실제 필요 수, pcie_irq=공유 IRQ 임시 변수 */
	u32 pme = 0, aer = 0, dpc = 0; /* NVMe: 각 서비스의 Interrupt Message Number 저장 변수 */

	/* Allocate the maximum possible number of MSI/MSI-X vectors */
	nr_entries = pci_alloc_irq_vectors(dev, 1, PCIE_PORT_MAX_MSI_ENTRIES,
			PCI_IRQ_MSIX | PCI_IRQ_MSI); /* NVMe: 포트에 대해 1~32개 MSI/MSI-X 벡터 우선 최대 할당 시도. */
	if (nr_entries < 0) /* NVMe: 벡터 할당 실패 시(음수는 에러 코드) */
		return nr_entries; /* NVMe: 에러 코드를 그대로 반환. */

	/* See how many and which Interrupt Message Numbers we actually use */
	nvec = pcie_message_numbers(dev, mask, &pme, &aer, &dpc); /* NVMe: 활성 서비스에 필요한 실제 벡터 수 계산. */
	if (nvec > nr_entries) { /* NVMe: 실제 필요 수가 할당받은 수보다 많으면 재할당 불가. */
		pci_free_irq_vectors(dev); /* NVMe: 기존 벡터 해제. */
		return -EIO; /* NVMe: I/O 오류 반환. */
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
	if (nvec != nr_entries) { /* NVMe: 초과 할당된 경우 줄여서 재할당. */
		pci_free_irq_vectors(dev); /* NVMe: 기존 벡터 해제. */

		nr_entries = pci_alloc_irq_vectors(dev, nvec, nvec,
				PCI_IRQ_MSIX | PCI_IRQ_MSI); /* NVMe: 정확히 nvec개만 재할당. */
		if (nr_entries < 0) /* NVMe: 재할당 실패 시 */
			return nr_entries; /* NVMe: 에러 코드 반환. */
	}

	/* PME, hotplug and bandwidth notification share an MSI/MSI-X vector */
	if (mask & (PCIE_PORT_SERVICE_PME | PCIE_PORT_SERVICE_HP |
		    PCIE_PORT_SERVICE_BWCTRL)) { /* NVMe: PME/HP/BWCTRL은 동일한 PCIe 캐퍼빌리티 IRQ를 공유. */
		pcie_irq = pci_irq_vector(dev, pme); /* NVMe: pme 번째 MSI/MSI-X 벡터의 Linux IRQ 번호 획득. */
		irqs[PCIE_PORT_SERVICE_PME_SHIFT] = pcie_irq; /* NVMe: PME 서비스 IRQ 배열에 저장. */
		irqs[PCIE_PORT_SERVICE_HP_SHIFT] = pcie_irq; /* NVMe: HP 서비스도 동일 IRQ 사용. */
		irqs[PCIE_PORT_SERVICE_BWCTRL_SHIFT] = pcie_irq; /* NVMe: BWCTRL 서비스도 동일 IRQ 사용. */
	}

	if (mask & PCIE_PORT_SERVICE_AER) /* NVMe: AER 서비스가 활성화된 경우 */
		irqs[PCIE_PORT_SERVICE_AER_SHIFT] = pci_irq_vector(dev, aer); /* NVMe: aer 번째 벡터의 IRQ 번호를 AER 서비스에 저장. */

	if (mask & PCIE_PORT_SERVICE_DPC) /* NVMe: DPC 서비스가 활성화된 경우 */
		irqs[PCIE_PORT_SERVICE_DPC_SHIFT] = pci_irq_vector(dev, dpc); /* NVMe: dpc 번째 벡터의 IRQ 번호를 DPC 서비스에 저장. */

	return 0; /* NVMe: MSI/MSI-X 벡터 설정 성공. */
}

/**
 * pcie_init_service_irqs - initialize irqs for PCI Express port services
 * @dev: PCI Express port to handle
 * @irqs: Array of irqs to populate
 * @mask: Bitmask of port capabilities returned by get_port_device_capability()
 *
 * Return value: Interrupt mode associated with the port
 */
/*
 * pcie_init_service_irqs:
 *   PCIe 포트 서비스의 IRQ 배열을 초기화하고, 우선 MSI/MSI-X를 시도한 뒤
 *   실패하면 INTx로 폴back한다. NVMe 장치의 상위 포트 인터럽트가
 *   MSI/MSI-X가 아닌 레거시 INTx로 동작하면 AER/DPC 지연/공유로 인해
 *   NVMe 오류 복구 응답 시간이 길어질 수 있다.
 */
static int pcie_init_service_irqs(struct pci_dev *dev, int *irqs, int mask)
{
	int ret, i; /* NVMe: ret는 INTx 할당 결과, i는 루프 인덱스 */

	for (i = 0; i < PCIE_PORT_DEVICE_MAXSERVICES; i++) /* NVMe: PME/AER/HP/DPC/BWCTRL 5개 서비스 IRQ를 -1(미할당)로 초기화. */
		irqs[i] = -1; /* NVMe: 각 서비스의 IRQ 값을 미할당 상태로 설정. */

	/*
	 * If we support PME but can't use MSI/MSI-X for it, we have to
	 * fall back to INTx or other interrupts, e.g., a system shared
	 * interrupt.
	 */
	if ((mask & PCIE_PORT_SERVICE_PME) && pcie_pme_no_msi()) /* NVMe: PME가 필요하고 DMI/커널 설정에서 MSI 사용 금지 시 */
		goto intx_irq; /* NVMe: INTx 폴back 경로로 이동. */

	/* Try to use MSI-X or MSI if supported */
	if (pcie_port_enable_irq_vec(dev, irqs, mask) == 0) /* NVMe: MSI/MSI-X 벡터 할당 시도. */
		return 0; /* NVMe: MSI/MSI-X 성공 시 0 반환. */

intx_irq:
	/* fall back to INTX IRQ */
	ret = pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_INTX); /* NVMe: 레거시 INTx 방식으로 1개 벡터만 할당. */
	if (ret < 0) /* NVMe: INTx 할당 실패 시 */
		return -ENODEV; /* NVMe: 장치 없음/불가 에러 반환. */

	for (i = 0; i < PCIE_PORT_DEVICE_MAXSERVICES; i++) /* NVMe: 모든 활성/비활성 서비스에 동일 INTx IRQ 번호 설정. */
		irqs[i] = pci_irq_vector(dev, 0); /* NVMe: INTx의 유일한 벡터(0번) IRQ 번호 저장. */

	return 0; /* NVMe: INTx 폴back 성공. */
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
/*
 * get_port_device_capability:
 *   PCIe 포트의 설정 레지스터를 읽어 지원하는 서비스(HP/AER/PME/DPC/BWCTRL)
 *   를 탐지한다. NVMe SSD가 연결된 Root Port/Downstream Port에서 어떤
 *   포트 서비스가 활성화될지 결정하므로, NVMe의 AER/DPC/HP/BWCTRL 지원
 *   여부와 직결된다.
 */
static int get_port_device_capability(struct pci_dev *dev)
{
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus); /* NVMe: 포트가 속한 host bridge(RC 일부) 획득. */
	int services = 0; /* NVMe: 활성화할 서비스 마스크 초기화. */

	if (dev->is_pciehp && /* NVMe: 포트가 PCIe native hotplug를 지원하는지 확인. */
	    (pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT || /* NVMe: Root Port일 때만 HP 이벤트 수신. */
	     pci_pcie_type(dev) == PCI_EXP_TYPE_DOWNSTREAM) && /* NVMe: Downstream Port(Switch 하위)도 HP 가능. */
	    (pcie_ports_native || host->native_pcie_hotplug)) { /* NVMe: 커널/플랫폼이 native hotplug를 허용하는지 확인. */
		services |= PCIE_PORT_SERVICE_HP; /* NVMe: NVMe SSD 핫 추가/제거 처리를 위한 HP 서비스 활성화. */

		/*
		 * Disable hot-plug interrupts in case they have been enabled
		 * by the BIOS and the hot-plug service driver won't be loaded
		 * to handle them.
		 */
		if (!IS_ENABLED(CONFIG_HOTPLUG_PCI_PCIE)) /* NVMe: HP 서비스 드라이버가 빌드되지 않은 경우 */
			pcie_capability_clear_word(dev, PCI_EXP_SLTCTL,
				PCI_EXP_SLTCTL_CCIE | PCI_EXP_SLTCTL_HPIE); /* NVMe: BIOS가 켠 HP 인터럽트를 미리 끔(드라이버 없이 폭증 방지). */
	}

#ifdef CONFIG_PCIEAER
	if ((pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT || /* NVMe: Root Port에서 AER 이벤트 수신. */
             pci_pcie_type(dev) == PCI_EXP_TYPE_RC_EC) && /* NVMe: Root Complex Event Collector도 AER 가능. */
	    dev->aer_cap && pci_aer_available() && /* NVMe: AER 캐퍼빌리티 존재 및 플랫폼 AER 사용 가능. */
	    (pcie_ports_native || host->native_aer)) /* NVMe: native AER 사용 정책 확인. */
		services |= PCIE_PORT_SERVICE_AER; /* NVMe: NVMe 장치 오류(UE/CE) 보고 경로인 AER 서비스 활성화. */
#endif

	/* Root Ports and Root Complex Event Collectors may generate PMEs */
	if ((pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT || /* NVMe: Root Port에서 PME 수신. */
	     pci_pcie_type(dev) == PCI_EXP_TYPE_RC_EC) && /* NVMe: RCEC에서도 PME 가능. */
	    (pcie_ports_native || host->native_pme)) { /* NVMe: native PME 사용 정책 확인. */
		services |= PCIE_PORT_SERVICE_PME; /* NVMe: NVMe 저전력 상태 복귀 요청을 위한 PME 서비스 활성화. */

		/*
		 * Disable PME interrupt on this port in case it's been enabled
		 * by the BIOS (the PME service driver will enable it when
		 * necessary).
		 */
		pcie_pme_interrupt_enable(dev, false); /* NVMe: PME 서비스 드라이버가 직접 제어하기 전까지 PME 인터럽트 끔. */
	}

	/*
	 * With dpc-native, allow Linux to use DPC even if it doesn't have
	 * permission to use AER.
	 */
	if (pci_find_ext_capability(dev, PCI_EXT_CAP_ID_DPC) && /* NVMe: DPC 확장 캐퍼빌리티 존재 여부 확인. */
	    pci_aer_available() && /* NVMe: AER 인프라 사용 가능(DPC와 연동). */
	    (pcie_ports_dpc_native || (services & PCIE_PORT_SERVICE_AER))) /* NVMe: dpc-native 옵션 또는 AER 서비스 활성화 시 DPC 사용. */
		services |= PCIE_PORT_SERVICE_DPC; /* NVMe: NVMe 장치 서프라이즈 제거/오류 억제를 위한 DPC 서비스 활성화. */

	/* Enable bandwidth control if more than one speed is supported. */
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_DOWNSTREAM || /* NVMe: Downstream Port에서 링크 폭/속도 변경 감지. */
	    pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT) { /* NVMe: Root Port에서도 링크 변경 감지. */
		u32 linkcap; /* NVMe: Link Capability 레지스터 값 저장. */

		pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &linkcap); /* NVMe: PCIe Link Capability 레지스터 읽기. */
		if (linkcap & PCI_EXP_LNKCAP_LBNC && /* NVMe: Link Bandwidth Notification Capability 지원 여부. */
		    hweight8(dev->supported_speeds) > 1) /* NVMe: 둘 이상의 링크 속도(Gen3/4/5 등)를 지원하면. */
			services |= PCIE_PORT_SERVICE_BWCTRL; /* NVMe: NVMe 대역폭 변경 알림을 위한 BWCTRL 서비스 활성화. */
	}

	return services; /* NVMe: 탐지된 포트 서비스 마스크 반환. */
}

/**
 * pcie_device_init - allocate and initialize PCI Express port service device
 * @pdev: PCI Express port to associate the service device with
 * @service: Type of service to associate with the service device
 * @irq: Interrupt vector to associate with the service device
 */
/*
 * pcie_device_init:
 *   주어진 PCIe 포트와 서비스 타입에 대한 pcie_device(service device)를
 *   할당/초기화하고 driver core에 등록한다. NVMe 상위 포트의 AER/DPC 등
 *   서비스 드라이버가 이 device에 bind되어 동작한다.
 */
static int pcie_device_init(struct pci_dev *pdev, int service, int irq)
{
	int retval; /* NVMe: device_register 결과 저장. */
	struct pcie_device *pcie; /* NVMe: 생성할 포트 서비스 장치 객체. */
	struct device *device; /* NVMe: pcie 남장기 device 객체 포인터. */

	pcie = kzalloc_obj(*pcie); /* NVMe: pcie_device 구조체 메모리 할당 및 0 초기화. */
	if (!pcie) /* NVMe: 메모리 할당 실패 시 */
		return -ENOMEM; /* NVMe: 메모리 부족 에러 반환. */
	pcie->port = pdev; /* NVMe: 서비스 장치가 속한 PCIe 포트(NVMe 상위 포트) 연결. */
	pcie->irq = irq; /* NVMe: 해당 서비스의 IRQ/MSI/MSI-X 벡터 번호 저장. */
	pcie->service = service; /* NVMe: PME/AER/HP/DPC/BWCTRL 중 하나를 지정. */

	/* Initialize generic device interface */
	device = &pcie->device; /* NVMe: pcie_device 내 device 구조체 포인터 획득. */
	device->bus = &pcie_port_bus_type; /* NVMe: "pci_express" 버스에 속하도록 설정. */
	device->release = release_pcie_device;	/* callback to free pcie dev */
	dev_set_name(device, "%s:pcie%03x",
		     pci_name(pdev),
		     get_descriptor_id(pci_pcie_type(pdev), service)); /* NVMe: "0000:00:01.0:pcie001" 형태의 고유 이름 생성. */
	device->parent = &pdev->dev; /* NVMe: 포트의 pci_dev.device를 부모로 설정해 sysfs 계층 구성. */
	device_enable_async_suspend(device); /* NVMe: 비동기 suspend 지원 설정. */

	retval = device_register(device); /* NVMe: driver core에 장치 등록(이제 pcie_port_bus_probe 등에서 match됨). */
	if (retval) { /* NVMe: 등록 실패 시 */
		put_device(device); /* NVMe: 참조 카운트 감소로 메모리 해제 유도. */
		return retval; /* NVMe: 에러 코드 반환. */
	}

	pm_runtime_no_callbacks(device); /* NVMe: 서비스 장치 자체의 런타임 콜백은 사용하지 않음. */

	return 0; /* NVMe: 서비스 장치 초기화 및 등록 성공. */
}

/**
 * pcie_port_device_register - register PCI Express port
 * @dev: PCI Express port to register
 *
 * Allocate the port extension structure and register services associated with
 * the port.
 */
/*
 * pcie_port_device_register:
 *   PCIe 포트를 활성화하고 지원하는 서비스를 탐지/할당/등록한다.
 *   NVMe SSD가 연결된 Root Port나 Switch Downstream Port에서 이 함수가
 *   호출되며, NVMe의 AER/DPC/HP/BWCTRL/PME 인프라가 여기서 준비된다.
 */
static int pcie_port_device_register(struct pci_dev *dev)
{
	int status, capabilities, i, nr_service; /* NVMe: status=결과, capabilities=서비스 마스크, i=루프, nr_service=등록 성공 수 */
	int irqs[PCIE_PORT_DEVICE_MAXSERVICES]; /* NVMe: PME/AER/HP/DPC/BWCTRL 각각의 IRQ 번호 배열. */

	/* Enable PCI Express port device */
	status = pci_enable_device(dev); /* NVMe: 포트의 I/O, MEM, BUS MASTER 등 리소스 활성화. */
	if (status) /* NVMe: 포트 활성화 실패 시 */
		return status; /* NVMe: 에러 코드 반환. */

	/* Get and check PCI Express port services */
	capabilities = get_port_device_capability(dev); /* NVMe: 포트가 지원하는 서비스 마스크 획득. */
	if (!capabilities) /* NVMe: 활성화할 서비스가 없으면 */
		return 0; /* NVMe: 추가 작업 없이 성공 처리. */

	pci_set_master(dev); /* NVMe: 포트가 버스 마스터링(DMA)을 할 수 있도록 설정. */
	/*
	 * Initialize service irqs. Don't use service devices that
	 * require interrupts if there is no way to generate them.
	 * However, some drivers may have a polling mode (e.g. pciehp_poll_mode)
	 * that can be used in the absence of irqs.  Allow them to determine
	 * if that is to be used.
	 */
	status = pcie_init_service_irqs(dev, irqs, capabilities); /* NVMe: 서비스별 IRQ(MSI/MSI-X/INTx) 초기화. */
	if (status) { /* NVMe: IRQ 초기화 실패 시 */
		capabilities &= PCIE_PORT_SERVICE_HP; /* NVMe: HP 서비스만 남기고 다른 서비스 마스크 제거. */
		if (!capabilities) /* NVMe: HP도 비활성화 상태면 */
			goto error_disable; /* NVMe: 포트 비활성화로 이동. */
	}

	/* Allocate child services if any */
	status = -ENODEV; /* NVMe: 아직 등록된 서비스가 없을 때 사용할 에러 코드 설정. */
	nr_service = 0; /* NVMe: 성공적으로 등록한 서비스 수 초기화. */
	for (i = 0; i < PCIE_PORT_DEVICE_MAXSERVICES; i++) { /* NVMe: 5개 서비스 각각에 대해. */
		int service = 1 << i; /* NVMe: i번째 서비스 마스크 생성(PME=1, AER=2, HP=4, ...). */
		if (!(capabilities & service)) /* NVMe: 현재 서비스가 활성화되지 않았으면 건다. */
			continue; /* NVMe: 다음 서비스로 진행. */
		if (!pcie_device_init(dev, service, irqs[i])) /* NVMe: 서비스 장치 생성/등록 시도. */
			nr_service++; /* NVMe: 성공 시 등록 카운트 증가. */
	}
	if (!nr_service) /* NVMe: 한 개의 서비스도 등록하지 못했으면 */
		goto error_cleanup_irqs; /* NVMe: IRQ 정리 후 비활성화. */

	return 0; /* NVMe: 포트 등록 및 서비스 초기화 완료. */

error_cleanup_irqs:
	pci_free_irq_vectors(dev); /* NVMe: 할당된 MSI/MSI-X/INTx 벡터 해제. */
error_disable:
	pci_disable_device(dev); /* NVMe: 포트 장치 비활성화. */
	return status; /* NVMe: 실패 코드 반환. */
}

typedef int (*pcie_callback_t)(struct pcie_device *); /* NVMe: pcie_device를 인자로 받는 서비스 드라이버 콜백 함수 포인터 타입. */

/*
 * pcie_port_device_iter:
 *   포트의 모든 자식 pcie_device를 순회하면서 등록된 서비스 드라이버의
 *   특정 콜백(suspend/resume/slot_reset 등)을 호출한다. NVMe 관련으로는
 *   slot_reset 콜백이 중요한데, AER/DPC 복구 과정에서 하위 서비스의
 *   slot_reset이 NVMe 엔드포인트 복구와 연동될 수 있다.
 */
static int pcie_port_device_iter(struct device *dev, void *data)
{
	struct pcie_port_service_driver *service_driver; /* NVMe: 현재 장치에 바인딩된 서비스 드라이버. */
	size_t offset = *(size_t *)data; /* NVMe: service_driver 구조체 내 콜백 함수 포인터 오프셋. */
	pcie_callback_t cb; /* NVMe: 호출할 실제 콜백 함수 포인터. */

	if ((dev->bus == &pcie_port_bus_type) && dev->driver) { /* NVMe: pcie_port_bus_type에 속하고 드라이버가 바인딩된 장치인지 확인. */
		service_driver = to_service_driver(dev->driver); /* NVMe: device_driver를 pcie_port_service_driver로 변환. */
		cb = *(pcie_callback_t *)((void *)service_driver + offset); /* NVMe: 오프셋 위치의 콜백 포인터 추출. */
		if (cb) /* NVMe: 콜백이 등록되어 있으면 */
			return cb(to_pcie_device(dev)); /* NVMe: pcie_device로 변환 후 콜백 호출. */
	}
	return 0; /* NVMe: 조건 미해당 또는 콜백 미등록 시 0 반환. */
}

#ifdef CONFIG_PM
/**
 * pcie_port_device_suspend - suspend port services associated with a PCIe port
 * @dev: PCI Express port to handle
 */
/*
 * pcie_port_device_suspend:
 *   포트 하위 서비스들의 suspend 콜백을 순회 호출한다. NVMe 장치가
 *   시스템 suspend 전환 시 상위 포트 서비스(AER/DPC/PME)도 같이
 *   suspend되어 전원 상태 전환이 일관되게 이루어진다.
 */
static int pcie_port_device_suspend(struct device *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, suspend); /* NVMe: service_driver->suspend 멤버의 오프셋 계산. */
	return device_for_each_child(dev, &off, pcie_port_device_iter); /* NVMe: 포트의 모든 자식 pcie_device에 대해 suspend 콜백 호출. */
}

/*
 * pcie_port_device_resume_noirq:
 *   IRQ 복구 전(noirq 단계)에 포트 서비스의 resume_noirq 콜백을 호출한다.
 *   NVMe 장치 복구 시 인터럽트가 아직 복원되지 않은 단계에서 포트 AER/DPC
 *   상태를 먼저 복구해야 한다.
 */
static int pcie_port_device_resume_noirq(struct device *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, resume_noirq); /* NVMe: resume_noirq 멤버 오프셋. */
	return device_for_each_child(dev, &off, pcie_port_device_iter); /* NVMe: 하위 서비스의 resume_noirq 콜백 순회 호출. */
}

/**
 * pcie_port_device_resume - resume port services associated with a PCIe port
 * @dev: PCI Express port to handle
 */
/*
 * pcie_port_device_resume:
 *   포트 하위 서비스들의 resume 콜백을 순회 호출한다. NVMe 장치가
 *   resume된 후 상위 포트의 PME/AER/DPC 서비스도 정상 동작 상태로
 *   복귀시킨다.
 */
static int pcie_port_device_resume(struct device *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, resume); /* NVMe: resume 멤버 오프셋. */
	return device_for_each_child(dev, &off, pcie_port_device_iter); /* NVMe: 하위 서비스의 resume 콜백 순회 호출. */
}

/**
 * pcie_port_device_runtime_suspend - runtime suspend port services
 * @dev: PCI Express port to handle
 */
/*
 * pcie_port_device_runtime_suspend:
 *   NVMe 장치가 런타임 D3로 진입할 때 상위 포트 서비스도 함께 런타임
 *   suspend 시킨다. 포트가 D3에 들어가면 AER/PME 이벤트 처리가
 *   일시적으로 중단될 수 있으므로 NVMe의 ASPM/runtime PM 정책과
 *   연동된다.
 */
static int pcie_port_device_runtime_suspend(struct device *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, runtime_suspend); /* NVMe: runtime_suspend 멤버 오프셋. */
	return device_for_each_child(dev, &off, pcie_port_device_iter); /* NVMe: 하위 서비스의 runtime_suspend 콜백 순회 호출. */
}

/**
 * pcie_port_device_runtime_resume - runtime resume port services
 * @dev: PCI Express port to handle
 */
/*
 * pcie_port_device_runtime_resume:
 *   NVMe 장치가 런타임 D3에서 깨어날 때 상위 포트 서비스를 런타임
 *   resume 시킨다. PME/AER/DPC 인터럽트 경로가 다시 활성화되어 NVMe
 *   이벤트 처리가 재개된다.
 */
static int pcie_port_device_runtime_resume(struct device *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, runtime_resume); /* NVMe: runtime_resume 멤버 오프셋. */
	return device_for_each_child(dev, &off, pcie_port_device_iter); /* NVMe: 하위 서비스의 runtime_resume 콜백 순회 호출. */
}
#endif /* PM */

/*
 * remove_iter:
 *   포트의 자식 pcie_device들을 unregister한다. NVMe 상위 포트가
 *   제거될 때 AER/DPC/HP 등 서비스 장치를 먼저 정리한다.
 */
static int remove_iter(struct device *dev, void *data)
{
	if (dev->bus == &pcie_port_bus_type) /* NVMe: pcie_port_bus_type에 속한 서비스 장치인지 확인. */
		device_unregister(dev); /* NVMe: 해당 서비스 장치를 driver core에서 제거. */
	return 0; /* NVMe: 순회 계속. */
}

/*
 * find_service_iter:
 *   포트 하위에서 특정 서비스 타입에 해당하는 pcie_device를 찾는다.
 *   NVMe 장치와 연결된 포트에서 AER/DPC/PME 서비스 장치를 검색할 때
 *   사용된다.
 */
static int find_service_iter(struct device *device, void *data)
{
	struct pcie_port_service_driver *service_driver; /* NVMe: 현재 장치에 바인딩된 서비스 드라이버. */
	struct portdrv_service_data *pdrvs; /* NVMe: 검색 조건/결과를 담은 콜백 데이터. */
	u32 service; /* NVMe: 찾으려는 서비스 마스크. */

	pdrvs = (struct portdrv_service_data *) data; /* NVMe: void*를 portdrv_service_data 포인터로 변환. */
	service = pdrvs->service; /* NVMe: 검색 대상 서비스 마스크 획득. */

	if (device->bus == &pcie_port_bus_type && device->driver) { /* NVMe: pcie_port_bus_type 장치이고 드라이버가 바인딩된 경우. */
		service_driver = to_service_driver(device->driver); /* NVMe: device_driver를 서비스 드라이버로 변환. */
		if (service_driver->service == service) { /* NVMe: 드라이버의 서비스 타입이 찾는 타입과 일치하면. */
			pdrvs->drv = service_driver; /* NVMe: 결과 구조체에 드라이버 저장. */
			pdrvs->dev = device; /* NVMe: 결과 구조체에 장치 저장. */
			return 1; /* NVMe: 검색 성공, 순회 중단. */
		}
	}

	return 0; /* NVMe: 일치하지 않으면 계속 순회. */
}

/**
 * pcie_port_find_device - find the struct device
 * @dev: PCI Express port the service is associated with
 * @service: For the service to find
 *
 * Find the struct device associated with given service on a pci_dev
 */
/*
 * pcie_port_find_device:
 *   NVMe 상위 PCIe 포트에서 지정한 서비스(AER/DPC/PME 등)에 해당하는
 *   struct device를 반환한다. 서비스 드라이버가 등록된 상태인지 확인하거나
 *   장치 간 참조를 맺을 때 사용된다.
 */
struct device *pcie_port_find_device(struct pci_dev *dev,
			      u32 service)
{
	struct device *device; /* NVMe: 검색 결과 장치 포인터. */
	struct portdrv_service_data pdrvs; /* NVMe: 검색용 임시 데이터 구조체. */

	pdrvs.dev = NULL; /* NVMe: 초기 검색 결과를 NULL로 설정. */
	pdrvs.service = service; /* NVMe: 찾을 서비스 마스크 설정. */
	device_for_each_child(&dev->dev, &pdrvs, find_service_iter); /* NVMe: 포트의 모든 자식 장치에서 find_service_iter 순회. */

	device = pdrvs.dev; /* NVMe: 검색된 장치 포인터 획득. */
	return device; /* NVMe: 일치하는 서비스 장치 반환(없으면 NULL). */
}
EXPORT_SYMBOL_GPL(pcie_port_find_device); /* NVMe: pcie_port_find_device를 GPL 모듈에 심볼 남장기. */

/**
 * pcie_port_device_remove - unregister PCI Express port service devices
 * @dev: PCI Express port the service devices to unregister are associated with
 *
 * Remove PCI Express port service devices associated with given port and
 * disable MSI-X or MSI for the port.
 */
/*
 * pcie_port_device_remove:
 *   포트에 등록된 모든 서비스 장치를 제거하고 IRQ 벡터를 해제한다.
 *   NVMe 장치가 제거되거나 상위 포트 드라이버가 unload될 때 AER/DPC/PME
 *   서비스를 정리한다.
 */
static void pcie_port_device_remove(struct pci_dev *dev)
{
	device_for_each_child(&dev->dev, NULL, remove_iter); /* NVMe: 포트의 모든 자식 pcie_device 제거. */
	pci_free_irq_vectors(dev); /* NVMe: 포트에 할당된 MSI/MSI-X/INTx 벡터 모두 해제. */
}

/*
 * pcie_port_bus_match:
 *   pcie_port_bus_type의 match 콜백. pcie_device의 서비스 타입과
 *   pcie_port_service_driver의 서비스/포트 타입이 일치하는지 검사한다.
 *   NVMe 상위 포트의 AER 서비스 장치는 AER 서비스 드라이버와만 매칭된다.
 */
static int pcie_port_bus_match(struct device *dev, const struct device_driver *drv)
{
	struct pcie_device *pciedev = to_pcie_device(dev); /* NVMe: device를 pcie_device로 변환. */
	const struct pcie_port_service_driver *driver = to_service_driver(drv); /* NVMe: device_driver를 서비스 드라이버로 변환. */

	if (driver->service != pciedev->service) /* NVMe: 드라이버와 장치의 서비스 타입이 다른면 매칭 실패. */
		return 0; /* NVMe: 매칭 실패. */

	if (driver->port_type != PCIE_ANY_PORT &&
	    driver->port_type != pci_pcie_type(pciedev->port)) /* NVMe: 드라이버가 특정 포트 타입 전용이고 현재 포트 타입과 다른면. */
		return 0; /* NVMe: 포트 타입 불일치로 매칭 실패. */

	return 1; /* NVMe: 서비스 및 포트 타입이 일치하면 매칭 성공. */
}

/**
 * pcie_port_bus_probe - probe driver for given PCI Express port service
 * @dev: PCI Express port service device to probe against
 *
 * If PCI Express port service driver is registered with
 * pcie_port_service_register(), this function will be called by the driver core
 * whenever match is found between the driver and a port service device.
 */
/*
 * pcie_port_bus_probe:
 *   매칭된 포트 서비스 드라이버의 probe 콜백을 호출한다. NVMe 상위
 *   포트에서 AER/DPC/PME/HP/BWCTRL 서비스 드라이버가 로드될 때 이
 *   함수를 통해 초기화된다.
 */
static int pcie_port_bus_probe(struct device *dev)
{
	struct pcie_device *pciedev; /* NVMe: probe 대상 pcie_device. */
	struct pcie_port_service_driver *driver; /* NVMe: 매칭된 서비스 드라이버. */
	int status; /* NVMe: probe 콜백 결과. */

	driver = to_service_driver(dev->driver); /* NVMe: 장치에 매칭된 드라이버 획득. */
	if (!driver || !driver->probe) /* NVMe: 드라이버가 없거나 probe 콜백이 없으면. */
		return -ENODEV; /* NVMe: 장치 없음 에러 반환. */

	pciedev = to_pcie_device(dev); /* NVMe: device를 pcie_device로 변환. */
	status = driver->probe(pciedev); /* NVMe: 서비스 드라이버의 probe 호출(AER/DPC/PME 등 초기화). */
	if (status) /* NVMe: probe 실패 시 */
		return status; /* NVMe: 에러 코드 반환. */

	get_device(dev); /* NVMe: probe 성공 시 장치 참조 카운트 증가(드라이버가 사용 중임을 표시). */
	return 0; /* NVMe: probe 성공. */
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
/*
 * pcie_port_bus_remove:
 *   NVMe 상위 포트 서비스 드라이버가 제거될 때 호출된다. AER/DPC/PME
 *   등의 remove 콜백을 통해 인터럽트/상태 머신을 정리한다.
 */
static void pcie_port_bus_remove(struct device *dev)
{
	struct pcie_device *pciedev; /* NVMe: 제거 대상 pcie_device. */
	struct pcie_port_service_driver *driver; /* NVMe: 바인딩된 서비스 드라이버. */

	pciedev = to_pcie_device(dev); /* NVMe: device를 pcie_device로 변환. */
	driver = to_service_driver(dev->driver); /* NVMe: 드라이버 획득. */
	if (driver && driver->remove) /* NVMe: 드라이버와 remove 콜백이 존재하면. */
		driver->remove(pciedev); /* NVMe: 서비스별 remove 콜백 호출. */

	put_device(dev); /* NVMe: probe 때 증가시킨 참조 카운트 감소. */
}

const struct bus_type pcie_port_bus_type = { /* NVMe: "pci_express" 버스 타입 정의. */
	.name = "pci_express", /* NVMe: sysfs에서 보이는 버스 이름. */
	.match = pcie_port_bus_match, /* NVMe: 서비스/포트 타입 기반 match 함수. */
	.probe = pcie_port_bus_probe, /* NVMe: 드라이버 probe 진입점. */
	.remove = pcie_port_bus_remove, /* NVMe: 드라이버 remove 진입점. */
};

/**
 * pcie_port_service_register - register PCI Express port service driver
 * @new: PCI Express port service driver to register
 */
/*
 * pcie_port_service_register:
 *   AER/DPC/PME/HP/BWCTRL 서비스 드라이버를 pcie_port_bus_type에
 *   등록한다. NVMe 엔드포인트의 오류 처리/전원 관리/핫플러그를 담당할
 *   포트 서비스 드라이버들이 이 함수를 통해 등록된다.
 */
int pcie_port_service_register(struct pcie_port_service_driver *new)
{
	if (pcie_ports_disabled) /* NVMe: 커널 옵션 "pcie_ports=compat"로 포트 서비스가 비활성화된 경우. */
		return -ENODEV; /* NVMe: 등록 거부. */

	new->driver.name = new->name; /* NVMe: 남장기 device_driver 이름에 서비스 드라이버 이름 설정. */
	new->driver.bus = &pcie_port_bus_type; /* NVMe: 등록 대상 버스를 pcie_port_bus_type으로 지정. */

	return driver_register(&new->driver); /* NVMe: driver core에 서비스 드라이버 등록. */
}

/**
 * pcie_port_service_unregister - unregister PCI Express port service driver
 * @drv: PCI Express port service driver to unregister
 */
/*
 * pcie_port_service_unregister:
 *   등록된 포트 서비스 드라이버를 해제한다. NVMe 상위 포트의 AER/DPC
 *   처리 능력이 제거될 때 사용된다.
 */
void pcie_port_service_unregister(struct pcie_port_service_driver *drv)
{
	driver_unregister(&drv->driver); /* NVMe: driver core에서 서비스 드라이버 등록 해제. */
}

/* If this switch is set, PCIe port native services should not be enabled. */
bool pcie_ports_disabled; /* NVMe: "pcie_ports=compat" 시 true, 모든 포트 native 서비스 비활성화. */

/*
 * If the user specified "pcie_ports=native", use the PCIe services regardless
 * of whether the platform has given us permission.  On ACPI systems, this
 * means we ignore _OSC.
 */
bool pcie_ports_native; /* NVMe: "pcie_ports=native" 시 true, 플랫폼 _OSC 무시하고 native 서비스 사용. */

/*
 * If the user specified "pcie_ports=dpc-native", use the Linux DPC PCIe
 * service even if the platform hasn't given us permission.
 */
bool pcie_ports_dpc_native; /* NVMe: "pcie_ports=dpc-native" 시 true, DPC 서비스 강제 사용. */

/*
 * pcie_port_setup:
 *   커널 부팅 파라미터 "pcie_ports="를 파싱하여 포트 서비스 정책을
 *   설정한다. NVMe 시스템에서 AER/DPC/PME 동작 방식을 사용자가 제어할
 *   수 있는 진입점이다.
 */
static int __init pcie_port_setup(char *str)
{
	if (!strncmp(str, "compat", 6)) /* NVMe: "compat" 옵션 시. */
		pcie_ports_disabled = true; /* NVMe: 모든 native 포트 서비스 비활성화. */
	else if (!strncmp(str, "native", 6)) /* NVMe: "native" 옵션 시. */
		pcie_ports_native = true; /* NVMe: _OSC 무시하고 native 서비스(AER/DPC/PME/HP/BWCTRL) 사용. */
	else if (!strncmp(str, "dpc-native", 10)) /* NVMe: "dpc-native" 옵션 시. */
		pcie_ports_dpc_native = true; /* NVMe: DPC 서비스를 플랫폼 권한 없이도 사용. */

	return 1; /* NVMe: __setup 핸들러 성공 반환. */
}
__setup("pcie_ports=", pcie_port_setup); /* NVMe: "pcie_ports=" 커널 파라미터 등록. */

/* global data */

#ifdef CONFIG_PM
/*
 * pcie_port_runtime_suspend:
 *   포트의 런타임 suspend 조건을 확인하고 하위 서비스의 runtime_suspend
 *   를 호출한다. NVMe 장치가 D3cold로 들어갈 때 상위 포트도 D3로
 *   진입 가능한지 판단한다.
 */
static int pcie_port_runtime_suspend(struct device *dev)
{
	if (!to_pci_dev(dev)->bridge_d3) /* NVMe: 포트가 D3 상태로 전환 가능한 bridge_d3 플래그가 꺼져 있으면. */
		return -EBUSY; /* NVMe: 런타임 suspend 불가. */

	return pcie_port_device_runtime_suspend(dev); /* NVMe: 하위 서비스 런타임 suspend 수행. */
}

/*
 * pcie_port_runtime_idle:
 *   런타임 PM idle 콜백. bridge_d3가 true일 때만 idle 허용. NVMe의
 *   ASPM/런타임 전원 관리와 연동된다.
 */
static int pcie_port_runtime_idle(struct device *dev)
{
	/*
	 * Assume the PCI core has set bridge_d3 whenever it thinks the port
	 * should be good to go to D3.  Everything else, including moving
	 * the port to D3, is handled by the PCI core.
	 */
	return to_pci_dev(dev)->bridge_d3 ? 0 : -EBUSY; /* NVMe: bridge_d3가 설정되어 있으면 idle 진입 허용, 아니면 거부. */
}

static const struct dev_pm_ops pcie_portdrv_pm_ops = { /* NVMe: PCIe 포트 드라이버의 전원 관리 ops 구조체. */
	.suspend	= pcie_port_device_suspend, /* NVMe: 시스템 suspend 시 하위 서비스 suspend 호출. */
	.resume_noirq	= pcie_port_device_resume_noirq, /* NVMe: IRQ 복구 전 하위 서비스 resume_noirq 호출. */
	.resume		= pcie_port_device_resume, /* NVMe: 시스템 resume 시 하위 서비스 resume 호출. */
	.freeze		= pcie_port_device_suspend, /* NVMe: hibernation freeze 단계에서 suspend 재사용. */
	.thaw		= pcie_port_device_resume, /* NVMe: hibernation thaw 단계에서 resume 재사용. */
	.poweroff	= pcie_port_device_suspend, /* NVMe: 시스템 종료 시 suspend 재사용. */
	.restore_noirq	= pcie_port_device_resume_noirq, /* NVMe: hibernation 복구 시 resume_noirq 재사용. */
	.restore	= pcie_port_device_resume, /* NVMe: hibernation 복구 시 resume 재사용. */
	.runtime_suspend = pcie_port_runtime_suspend, /* NVMe: 런타임 suspend 진입. */
	.runtime_resume	= pcie_port_device_runtime_resume, /* NVMe: 런타임 resume 진입. */
	.runtime_idle	= pcie_port_runtime_idle, /* NVMe: 런타임 idle 판단. */
};

#define PCIE_PORTDRV_PM_OPS	(&pcie_portdrv_pm_ops) /* NVMe: PM ops 매크로 정의. */

#else /* !PM */

#define PCIE_PORTDRV_PM_OPS	NULL /* NVMe: PM 미지원 시 NULL. */
#endif /* !PM */

/*
 * pcie_portdrv_probe - Probe PCI-Express port devices
 * @dev: PCI-Express port device being probed
 *
 * If detected invokes the pcie_port_device_register() method for
 * this port device.
 *
 */
/*
 * pcie_portdrv_probe:
 *   PCIe 포트(RP/USP/DSP/RCEC)에 대해 portdrv를 probe한다. NVMe SSD가
 *   연결된 포트에서는 이 함수를 통해 AER/DPC/PME/HP/BWCTRL 서비스가
 *   활성화되고, NVMe의 오류 처리 및 전원/핫플러그/대역폭 관리가
 *   가능해진다.
 */
static int pcie_portdrv_probe(struct pci_dev *dev,
				const struct pci_device_id *id)
{
	int type = pci_pcie_type(dev); /* NVMe: 현재 포트의 PCIe 포트 타입(RP/USP/DSP/RCEC) 획득. */
	int status; /* NVMe: 등록 결과 저장. */

	if (!pci_is_pcie(dev) || /* NVMe: 장치가 PCIe가 아니거나. */
	    ((type != PCI_EXP_TYPE_ROOT_PORT) && /* NVMe: Root Port가 아니고. */
	     (type != PCI_EXP_TYPE_UPSTREAM) && /* NVMe: Upstream Port가 아니고. */
	     (type != PCI_EXP_TYPE_DOWNSTREAM) && /* NVMe: Downstream Port가 아니고. */
	     (type != PCI_EXP_TYPE_RC_EC))) /* NVMe: RCEC도 아니면. */
		return -ENODEV; /* NVMe: 이 드라이버가 처리하지 않는 포트이므로 probe 실패. */

	if (type == PCI_EXP_TYPE_RC_EC) /* NVMe: Root Complex Event Collector인 경우. */
		pcie_link_rcec(dev); /* NVMe: RCEC를 PCIe 계층 구조에 연결(오류/이벤트 통지 경로 구성). */

	status = pcie_port_device_register(dev); /* NVMe: 포트 활성화 및 서비스(AER/DPC/PME/HP/BWCTRL) 등록. */
	if (status) /* NVMe: 등록 실패 시 */
		return status; /* NVMe: 에러 코드 반환. */

	pci_save_state(dev); /* NVMe: 포트의 현재 PCI 설정 상태 저장(suspend/resume 복구용). */

	dev_pm_set_driver_flags(&dev->dev, DPM_FLAG_NO_DIRECT_COMPLETE |
					   DPM_FLAG_SMART_SUSPEND); /* NVMe: 런타임 PM 플래그 설정(직접 complete 금지, smart suspend 사용). */

	if (pci_bridge_d3_possible(dev)) { /* NVMe: 포트가 D3 상태 전환을 지원하면. */
		/*
		 * Keep the port resumed 100ms to make sure things like
		 * config space accesses from userspace (lspci) will not
		 * cause the port to repeatedly suspend and resume.
		 */
		pm_runtime_set_autosuspend_delay(&dev->dev, 100); /* NVMe: 100ms 자동 suspend 지연 설정. */
		pm_runtime_use_autosuspend(&dev->dev); /* NVMe: autosuspend 메커니즘 활성화. */
		pm_runtime_mark_last_busy(&dev->dev); /* NVMe: 마지막 활동 시점 갱신. */
		pm_runtime_put_autosuspend(&dev->dev); /* NVMe: autosuspend 참조 카운트 반납. */
		pm_runtime_allow(&dev->dev); /* NVMe: 런타임 PM 허용. */
	}

	return 0; /* NVMe: PCIe 포트 probe 성공. */
}

/*
 * pcie_portdrv_remove:
 *   PCIe 포트 드라이버가 제거될 때 호출된다. NVMe 상위 포트에서
 *   AER/DPC/PME/HP/BWCTRL 서비스를 정리하고 포트를 비활성화한다.
 */
static void pcie_portdrv_remove(struct pci_dev *dev)
{
	if (pci_bridge_d3_possible(dev)) { /* NVMe: 런타임 PM이 활성화된 포트인 경우. */
		pm_runtime_forbid(&dev->dev); /* NVMe: 추가 런타임 suspend 금지. */
		pm_runtime_get_noresume(&dev->dev); /* NVMe: 런타임 resume 없이 참조 획득. */
		pm_runtime_dont_use_autosuspend(&dev->dev); /* NVMe: autosuspend 비활성화. */
	}

	pcie_port_device_remove(dev); /* NVMe: 하위 서비스 장치 제거 및 IRQ 벡터 해제. */

	pci_disable_device(dev); /* NVMe: 포트 장치 비활성화. */
}

/*
 * pcie_portdrv_shutdown:
 *   시스템 종료 시 PCIe 포트의 서비스들을 정리한다. NVMe 장치가
 *   종료 중에도 상위 포트의 AER/DPC 등이 안전하게 정리되어야 한다.
 */
static void pcie_portdrv_shutdown(struct pci_dev *dev)
{
	if (pci_bridge_d3_possible(dev)) { /* NVMe: 런타임 PM이 활성화된 포트인 경우. */
		pm_runtime_forbid(&dev->dev); /* NVMe: 런타임 suspend 금지. */
		pm_runtime_get_noresume(&dev->dev); /* NVMe: 참조 획득. */
		pm_runtime_dont_use_autosuspend(&dev->dev); /* NVMe: autosuspend 중지. */
	}

	pcie_port_device_remove(dev); /* NVMe: 포트 서비스 장치 및 IRQ 정리. */
}

/*
 * pcie_portdrv_error_detected:
 *   PCIe 포트 자체의 AER/ERR 콜백. NVMe 엔드포인트에서 발생한 오류가
 *   상위 포트로 전파되어 채널 상태가 frozen이면 reset이 필요하다고
 *   판단한다. 이 결과는 PCI core의 error recovery 흐름을 타고 NVMe의
 *   .error_detected 콜백으로 연결될 수 있다.
 */
static pci_ers_result_t pcie_portdrv_error_detected(struct pci_dev *dev,
					pci_channel_state_t error)
{
	if (error == pci_channel_io_frozen) /* NVMe: PCIe 채널이 frozen 상태이면. */
		return PCI_ERS_RESULT_NEED_RESET; /* NVMe: slot reset 필요를 알림. */
	return PCI_ERS_RESULT_CAN_RECOVER; /* NVMe: 그 외에는 복구 가능으로 판단. */
}

/*
 * pcie_portdrv_slot_reset:
 *   PCIe 포트가 slot reset 후 복구될 때 하위 서비스 드라이버의
 *   slot_reset 콜백을 순회 호출하고 포트 상태를 복원한다. NVMe 장치의
 *   AER/DPC 복구 과정에서 상위 포트가 먼저 reset되고 이후 NVMe의
 *   .slot_reset가 호출될 수 있다.
 */
static pci_ers_result_t pcie_portdrv_slot_reset(struct pci_dev *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, slot_reset); /* NVMe: slot_reset 콜백 오프셋 계산. */
	device_for_each_child(&dev->dev, &off, pcie_port_device_iter); /* NVMe: 하위 AER/DPC/PME 등 서비스의 slot_reset 호출. */

	pci_restore_state(dev); /* NVMe: 포트의 PCI 설정 상태 복원. */
	return PCI_ERS_RESULT_RECOVERED; /* NVMe: 포트 복구 완료 보고. */
}

/*
 * pcie_portdrv_mmio_enabled:
 *   MMIO 접근이 다시 허용되었을 때 포트 측 복구 상태를 보고한다.
 *   NVMe의 MMIO(bar/doorbell) 접근이 재개되기 전 포트가 먼저
 *   복구되었음을 나타낸다.
 */
static pci_ers_result_t pcie_portdrv_mmio_enabled(struct pci_dev *dev)
{
	return PCI_ERS_RESULT_RECOVERED; /* NVMe: MMIO 활성화 후 복구된 것으로 간주. */
}

/*
 * LINUX Device Driver Model
 */
static const struct pci_device_id port_pci_ids[] = { /* NVMe: pcie_portdriver가 매칭할 PCI 클래스 ID 테이블. */
	/* handle any PCI-Express port */
	{ PCI_DEVICE_CLASS(PCI_CLASS_BRIDGE_PCI_NORMAL, ~0) }, /* NVMe: 일반 PCIe 브리지(06:04) 매칭. */
	/* subtractive decode PCI-to-PCI bridge, class type is 060401h */
	{ PCI_DEVICE_CLASS(PCI_CLASS_BRIDGE_PCI_SUBTRACTIVE, ~0) }, /* NVMe: subtractive 디코딩 P2P 브리지(06:01) 매칭. */
	/* handle any Root Complex Event Collector */
	{ PCI_DEVICE_CLASS(((PCI_CLASS_SYSTEM_RCEC << 8) | 0x00), ~0) }, /* NVMe: RCEC(08:07) 매칭. */
	{ }, /* NVMe: 테이블 종료 마커. */
};

static const struct pci_error_handlers pcie_portdrv_err_handler = { /* NVMe: 포트 드라이버의 AER/ERR 핸들러 등록. */
	.error_detected = pcie_portdrv_error_detected, /* NVMe: 오류 감지 콜백. */
	.slot_reset = pcie_portdrv_slot_reset, /* NVMe: 슬롯 리셋 콜백. */
	.mmio_enabled = pcie_portdrv_mmio_enabled, /* NVMe: MMIO 재활성화 콜백. */
};

static struct pci_driver pcie_portdriver = { /* NVMe: PCIe 포트 드라이버 구조체. */
	.name		= "pcieport", /* NVMe: 드라이버 이름("pcieport"). */
	.id_table	= port_pci_ids, /* NVMe: 매칭할 PCI 클래스 ID 테이블. */

	.probe		= pcie_portdrv_probe, /* NVMe: 포트 발견 시 probe 함수. */
	.remove		= pcie_portdrv_remove, /* NVMe: 포트 제거 시 remove 함수. */
	.shutdown	= pcie_portdrv_shutdown, /* NVMe: 시스템 종료 시 shutdown 함수. */

	.err_handler	= &pcie_portdrv_err_handler, /* NVMe: AER/ERR 복구 핸들러. */

	.driver_managed_dma = true, /* NVMe: 드라이버가 직접 DMA 일관성을 관리. */

	.driver.pm	= PCIE_PORTDRV_PM_OPS, /* NVMe: 전원 관리 ops 연결. */
};

/*
 * dmi_pcie_pme_disable_msi:
 *   DMI로 특정 시스템이 매칭되면 PME MSI 사용을 비활성화한다. 일부
 *   시스템에서 PME MSI가 buggy하여 NVMe resume 이벤트가 누락될 수
 *   있으므로 INTx 폴back이 필요하다.
 */
static int __init dmi_pcie_pme_disable_msi(const struct dmi_system_id *d)
{
	pr_notice("%s detected: will not use MSI for PCIe PME signaling\n",
		  d->ident); /* NVMe: DMI 식별 문자열과 함께 PME MSI 비활성화 알림 출력. */
	pcie_pme_disable_msi(); /* NVMe: PME MSI 비활성화 플래그 설정. */
	return 0; /* NVMe: DMI 콜백 성공 반환. */
}

static const struct dmi_system_id pcie_portdrv_dmi_table[] __initconst = { /* NVMe: DMI 기반 quirks 테이블. */
	/*
	 * Boxes that should not use MSI for PCIe PME signaling.
	 */
	{
	 .callback = dmi_pcie_pme_disable_msi, /* NVMe: 매칭 시 PME MSI 비활성화 콜백 실행. */
	 .ident = "MSI Wind U-100", /* NVMe: 시스템 식별 이름. */
	 .matches = {
		     DMI_MATCH(DMI_SYS_VENDOR,
				"MICRO-STAR INTERNATIONAL CO., LTD"), /* NVMe: 제조사 매칭 조건. */
		     DMI_MATCH(DMI_PRODUCT_NAME, "U-100"), /* NVMe: 제품명 매칭 조건. */
		     },
	 },
	 {} /* NVMe: 테이블 종료 마커. */
};

/*
 * pcie_init_services:
 *   PCIe 포트 서비스(AER/PME/DPC/BWCTRL/HP) 하위 드라이버들을 초기화한다.
 *   NVMe 엔드포인트의 오류/전원/핫플러그/대역폭 처리를 담당할
 *   인프라가 여기서 준비된다.
 */
static void __init pcie_init_services(void)
{
	pcie_aer_init(); /* NVMe: AER 서비스 초기화(NVMe Uncorrectable/Correctable Error 처리 준비). */
	pcie_pme_init(); /* NVMe: PME 서비스 초기화(NVMe 저전력 resume 이벤트 처리 준비). */
	pcie_dpc_init(); /* NVMe: DPC 서비스 초기화(NVMe 서프라이즈 제거/오류 억제 준비). */
	pcie_bwctrl_init(); /* NVMe: BWCTRL 서비스 초기화(NVMe 링크 대역폭 변경 알림 준비). */
	pcie_hp_init(); /* NVMe: HP 서비스 초기화(NVMe 핫 추가/제거 준비). */
}

/*
 * pcie_portdrv_init:
 *   PCIe 포트 버스 드라이버를 초기화하고 PCI 코어에 등록한다. NVMe
 *   장치가 연결될 PCIe 포트들을 발견하고, 각 포트의 서비스(AER/DPC/PME
 *   /HP/BWCTRL)를 활성화하는 전체 흐름의 시작점이다.
 */
static int __init pcie_portdrv_init(void)
{
	if (pcie_ports_disabled) /* NVMe: "pcie_ports=compat"로 포트 서비스가 비활성화된 경우. */
		return -EACCES; /* NVMe: 접근 거부 에러로 초기화 중단. */

	pcie_init_services(); /* NVMe: AER/PME/DPC/BWCTRL/HP 서비스 초기화. */
	dmi_check_system(pcie_portdrv_dmi_table); /* NVMe: DMI quirks 적용(필요 시 PME MSI 비활성화). */

	return pci_register_driver(&pcie_portdriver); /* NVMe: pcie_portdriver를 PCI 코어에 등록, 포트 탐색 시작. */
}
device_initcall(pcie_portdrv_init); /* NVMe: 장치 초기화 단계에서 pcie_portdrv_init 자동 실행. */
