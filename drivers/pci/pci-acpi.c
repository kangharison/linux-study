// SPDX-License-Identifier: GPL-2.0
/*
 * PCI support in ACPI
 *
 * Copyright (C) 2005 David Shaohua Li <shaohua.li@intel.com>
 * Copyright (C) 2004 Tom Long Nguyen <tom.l.nguyen@intel.com>
 * Copyright (C) 2004 Intel Corp.
 */

/*
 * ==================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * ------------------------------------------------------------------
 * 본 파일(drivers/pci/pci-acpi.c)은 ACPI와 PCI의 연동 레이어로서,
 * NVMe SSD가 탑재된 PCIe 루트/엔드포인트의 초기화, 전원 관리,
 * MSI/MSI-X IRQ 도메인, ASPM, config space 접근, hotplug, wake 등을
 * ACPI 네임스페이스 및 _DSM/_HPX/_SxD/_PRW 등의 메서드를 통해
 * 제어한다.
 * NVMe 드라이버(drivers/nvme/host/pci.c) 입장에서 본 파일의 주요
 * 관여 지점은 다음과 같다.
 *   - pci_acpi_init: FADT의 NO_MSI/NO_ASPM 플래그를 해석해 전역 MSI,
 *     ASPM을 끈다. NVMe MSI-X 기반 큐와 ASPM 절전 링크 상태에 직접
 *     영향을 준다.
 *   - pci_host_bridge_acpi_msi_domain: NVMe 장치가 연결된 host bridge의
 *     MSI irq_domain을 조회한다. MSI-X vector 할당 시 사용된다.
 *   - acpi_pci_choose_state / acpi_pci_set_power_state: NVMe 디바이스의
 *     D0/D3hot/D3cold 전환을 ACPI _PSx 메서드로 수행한다. NVMe
 *     suspend/resume/reset 흐름에서 핵심 경로다.
 *   - acpi_pci_irq_enable (ARM64/RISCV): NVMe INT#x/MSI 라인을 ACPI
 *     _PRT에서 찾아 할당한다.
 *   - acpi_pci_setup / pci_acpi_optimize_delay: NVMe pci_dev에 대한
 *     wakeup, D3 delay, ExternalFacingPort 등의 ACPI 기반 설정을
 *     수행한다.
 *   - pci_acpi_scan_root: ACPI MCFG/ECAM을 통해 PCI config space를
 *     매핑하고 root bus를 생성한다. NVMe의 BAR0(BAR), PCIe capability,
 *     MSI-X capability 탐색이 이 config space를 통해 이뤄진다.
 *   - _HPX/_HPP Type 0/1/2/3: NVMe 장치의 PCIe capability, AER,
 *     DEVCTL/LNKCTL 등을 ACPI가 제어할 수 있게 한다.
 * 일반적인 NVMe 드라이버 호출 경로:
 *   nvme_probe -> pci_enable_device -> pcibios_add_bus ->
 *   acpi_pci_add_bus -> pci_acpi_setup -> pci_acpi_optimize_delay
 *   nvme_reset_work -> pci_set_power_state ->
 *   acpi_pci_set_power_state(_PS3/_PS0)
 *   nvme_setup_io_queues -> pci_alloc_irq_vectors(MSI-X) ->
 *   pci_host_bridge_acpi_msi_domain
 * ==================================================================
 */

#include <linux/delay.h>        /* NVMe: D3cold->D0 복귀 지연 등에 사용 */
#include <linux/init.h>
#include <linux/iommu.h>        /* NVMe: DMA/IOMMU 연동, reset 시 detach */
#include <linux/irqdomain.h>    /* NVMe: MSI-X vector 할당용 irq_domain */
#include <linux/pci.h>          /* NVMe: PCIe config space, BAR, capability */
#include <linux/msi.h>          /* NVMe: MSI/MSI-X 메시지 signaled interrupts */
#include <linux/pci_hotplug.h>  /* NVMe: NVMe SSD hotplug 이벤트 처리 */
#include <linux/module.h>
#include <linux/pci-acpi.h>     /* NVMe: ACPI-PCI 연동 공개 인터페이스 */
#include <linux/pci-ecam.h>   /* NVMe: ECAM 기반 PCIe config space 접근 */
#include <linux/pm_runtime.h> /* NVMe: NVMe 디바이스 런타임 PM 정책 */
#include <linux/pm_qos.h>     /* NVMe: NO_POWER_OFF QoS 제약 해석 */
#include <linux/rwsem.h>
#include "pci.h"              /* NVMe: 남장기 낸부 PCI 구조체/함수 */

/*
 * pci_acpi_dsm_guid:
 *   ACPI _DSM(Device Specific Method) 호출 시 사용하는 PCI Firmware
 *   Specification GUID. NVMe 장치/루트 브리지의 preserve boot config,
 *   reset delay, device readiness durations 등의 _DSM 함수 식별에
 *   사용된다.
 * The GUID is defined in the PCI Firmware Specification available
 * here to PCI-SIG members:
 * https://members.pcisig.com/wg/PCI-SIG/document/15350
 */
const guid_t pci_acpi_dsm_guid =
	GUID_INIT(0xe5c937d0, 0x3553, 0x4d7a,
		  0x91, 0x17, 0xea, 0x4d, 0x19, 0xc3, 0x43, 0x4d); /* NVMe: PCI _DSM GUID 초기화 */

#if defined(CONFIG_PCI_QUIRKS) && defined(CONFIG_ARM64)
/*
 * acpi_get_rc_addr:
 *   ACPI _CRS(Current Resource Settings)에서 Root Complex의 메모리
 *   리소스를 파싱한다. NVMe BAR(DMA/MMIO window)가 속한 root bridge
 *   window를 결정하는 데 쓰인다.
 */
static int acpi_get_rc_addr(struct acpi_device *adev, struct resource *res)
{
	struct device *dev = &adev->dev; /* NVMe: ACPI companion device 획득 */
	struct resource_entry *entry;    /* NVMe: _CRS로부터 얻은 리소스 엔트리 포인터 */
	struct list_head list;           /* NVMe: _CRS 리소스 리스트 헤드 */
	unsigned long flags;             /* NVMe: 리소스 타입 플래그(IORESOURCE_MEM) */
	int ret;                         /* NVMe: 반환 코드 */

	INIT_LIST_HEAD(&list);           /* NVMe: 리소스 리스트 초기화 */
	flags = IORESOURCE_MEM;          /* NVMe: 메모리 타입 리소스만 필터링 */
	ret = acpi_dev_get_resources(adev, &list,
				     acpi_dev_filter_resource_type_cb,
				     (void *) flags); /* NVMe: _CRS에서 MEM 리소스 추출 */
	if (ret < 0) { /* NVMe: _CRS 파싱 실패 시 */
		dev_err(dev, "failed to parse _CRS method, error code %d\n",
			ret); /* NVMe: 오류 메시지 출력 */
		return ret; /* NVMe: 파싱 오류 반환 */
	}

	if (ret == 0) { /* NVMe: _CRS에 MEM 리소스가 없으면 */
		dev_err(dev, "no IO and memory resources present in _CRS\n");
		return -EINVAL; /* NVMe: 유효하지 않은 리소스 상태 반환 */
	}

	entry = list_first_entry(&list, struct resource_entry, node); /* NVMe: 첫 번째 MEM 리소스 선택 */
	*res = *entry->res;        /* NVMe: 호출자에게 Root Complex 주소 범위 복사 */
	acpi_dev_free_resource_list(&list); /* NVMe: _CRS 리소스 리스트 메모리 해제 */
	return 0;                   /* NVMe: 성공 반환 */
}

/*
 * acpi_match_rc:
 *   ACPI namespace를 순회하며 주어진 PCI segment 번호와 _UID가
 *   일치하는 Root Complex 객체를 찾는다. NVMe 장치가 속한 PCIe segment를
 *   올바른 ACPI Root Complex에 매핑할 때 사용된다.
 */
static acpi_status acpi_match_rc(acpi_handle handle, u32 lvl, void *context,
				 void **retval)
{
	u16 *segment = context;        /* NVMe: 찾으려는 PCI segment 번호 */
	unsigned long long uid;        /* NVMe: ACPI 객체의 _UID 값 */
	acpi_status status;            /* NVMe: ACPI 평가 상태 */

	status = acpi_evaluate_integer(handle, METHOD_NAME__UID, NULL, &uid); /* NVMe: _UID 메서드 평가 */
	if (ACPI_FAILURE(status) || uid != *segment) /* NVMe: _UID 실패 또는 segment 불일치 시 */
		return AE_CTRL_DEPTH; /* NVMe: 더 깊은 트리 탐색 제어 */

	*(acpi_handle *)retval = handle; /* NVMe: 일치하는 Root Complex 핸들 저장 */
	return AE_CTRL_TERMINATE;        /* NVMe: 일치 항목 발견, 탐색 종료 */
}

/*
 * acpi_get_rc_resources:
 *   지정된 ACPI _HID와 segment 번호로 Root Complex를 찾아 _CRS
 *   리소스를 반환한다. NVMe SSD가 연결된 Root Complex의 ECAM/Base
 *   주소를 초기화할 때 활용된다.
 */
int acpi_get_rc_resources(struct device *dev, const char *hid, u16 segment,
			  struct resource *res)
{
	struct acpi_device *adev;  /* NVMe: 찾은 ACPI Root Complex 장치 */
	acpi_status status;        /* NVMe: ACPI 평가 상태 */
	acpi_handle handle;        /* NVMe: 일치한 ACPI 핸들 */
	int ret;                   /* NVMe: 반환 코드 */

	status = acpi_get_devices(hid, acpi_match_rc, &segment, &handle); /* NVMe: _HID/_UID로 Root Complex 검색 */
	if (ACPI_FAILURE(status)) { /* NVMe: Root Complex를 찾지 못하면 */
		dev_err(dev, "can't find _HID %s device to locate resources\n",
			hid); /* NVMe: 오류 메시지 출력 */
		return -ENODEV; /* NVMe: 장치 없음 반환 */
	}

	adev = acpi_fetch_acpi_dev(handle); /* NVMe: 핸들에서 ACPI device 구조체 획득 */
	if (!adev) /* NVMe: ACPI device 변환 실패 시 */
		return -ENODEV; /* NVMe: 장치 없음 반환 */

	ret = acpi_get_rc_addr(adev, res); /* NVMe: Root Complex _CRS 리소스 획득 */
	if (ret) { /* NVMe: 리소스 획득 실패 시 */
		dev_err(dev, "can't get resource from %s\n",
			dev_name(&adev->dev)); /* NVMe: 실패한 ACPI device 이름 출력 */
		return ret; /* NVMe: 오류 반환 */
	}

	return 0; /* NVMe: 성공 반환 */
}
#endif

/*
 * acpi_pci_root_get_mcfg_addr:
 *   ACPI Root Complex의 _CBA(Configuration Base Address) 메서드를
 *   평가하여 ECAM(Configuration Space) 물리 주소를 반환한다. NVMe
 *   디바이스의 BAR, MSI-X capability, PCIe capability를 읽으려면 이
 *   ECAM 기반 config space 매핑이 선행되어야 한다.
 */
phys_addr_t acpi_pci_root_get_mcfg_addr(acpi_handle handle)
{
	acpi_status status = AE_NOT_EXIST; /* NVMe: 기본값은 _CBA가 없음을 의미 */
	unsigned long long mcfg_addr;      /* NVMe: _CBA로부터 얻은 ECAM 물리 주소 */

	if (handle) /* NVMe: 유효한 ACPI 핸들이 주어지면 */
		status = acpi_evaluate_integer(handle, METHOD_NAME__CBA,
					       NULL, &mcfg_addr); /* NVMe: _CBA 평가 시도 */
	if (ACPI_FAILURE(status)) /* NVMe: _CBA 평가 실패 시 */
		return 0; /* NVMe: ECAM 주소를 0으로 반환(매핑 불가) */

	return (phys_addr_t)mcfg_addr; /* NVMe: ECAM 물리 주소 반환 */
}

/*
 * pci_acpi_preserve_config:
 *   PCI Firmware _DSM Function 0("PCI Boot Configuration")를 평가하여
 *   firmware가 할당한 NVMe BAR, bus 리소스를 운영체제가 재할당하지
 *   않고 보존해야 하는지 결정한다. NVMe BAR0의 doorbell/register 주소가
 *   firmware 설정 그대로 유지되어야 할 때 중요하다.
 */
bool pci_acpi_preserve_config(struct pci_host_bridge *host_bridge)
{
	bool ret = false; /* NVMe: 기본값은 리소스 보존 불필요 */

	if (ACPI_HANDLE(&host_bridge->dev)) { /* NVMe: host bridge에 ACPI 핸들이 있으면 */
		union acpi_object *obj; /* NVMe: _DSM 반환 객체 포인터 */

		/*
		 * Evaluate the "PCI Boot Configuration" _DSM Function.  If it
		 * exists and returns 0, we must preserve any PCI resource
		 * assignments made by firmware for this host bridge.
		 * NVMe: firmware가 할당한 NVMe BAR/resource를 OS가 덮어쓰지 않음.
		 */
		obj = acpi_evaluate_dsm_typed(ACPI_HANDLE(&host_bridge->dev),
					      &pci_acpi_dsm_guid,
					      1, DSM_PCI_PRESERVE_BOOT_CONFIG,
					      NULL, ACPI_TYPE_INTEGER); /* NVMe: _DSM func 0 평가 */
		if (obj && obj->integer.value == 0) /* NVMe: 반환값이 0이면 보존 필요 */
			ret = true; /* NVMe: preserve_config 플래그 설정 */
		ACPI_FREE(obj); /* NVMe: _DSM 반환 객체 메모리 해제 */
	}

	return ret; /* NVMe: preserve_config 여부 반환 */
}

/* _HPX PCI Setting Record (Type 0); same as _HPP */
/* NVMe: _HPX Type 0은 Legacy PCI 설정(cache line, latency timer, SERR/PERR) */
struct hpx_type0 {
	u32 revision;		/* Not present in _HPP */ /* NVMe: _HPP/_HPX revision */
	u8  cache_line_size;	/* Not applicable to PCIe */ /* NVMe: PCI cache line size */
	u8  latency_timer;	/* Not applicable to PCIe */ /* NVMe: PCI latency timer */
	u8  enable_serr;
	u8  enable_perr;
};

static struct hpx_type0 pci_default_type0 = {
	.revision = 1,           /* NVMe: 기본 revision 1 */
	.cache_line_size = 8,    /* NVMe: 기본 cache line size(32바이트 단위 시 256B) */
	.latency_timer = 0x40,   /* NVMe: 기본 latency timer */
	.enable_serr = 0,        /* NVMe: SERR 기본 비활성 */
	.enable_perr = 0,        /* NVMe: PERR 기본 비활성 */
};

/*
 * program_hpx_type0:
 *   _HPX/_HPP Type 0 레코드를 NVMe/PCI 디바이스의 PCI config space에
 *   기록한다. NVMe의 PCI_COMMAND, CACHE_LINE_SIZE 등이 여기서 설정될
 *   수 있다.
 */
static void program_hpx_type0(struct pci_dev *dev, struct hpx_type0 *hpx)
{
	u16 pci_cmd, pci_bctl; /* NVMe: PCI COMMAND/BRIDGE CONTROL 레지스터 임시값 */

	if (!hpx) /* NVMe: _HPX 레코드가 없으면 */
		hpx = &pci_default_type0; /* NVMe: 기본값 사용 */

	if (hpx->revision > 1) { /* NVMe: 지원하지 않는 revision이면 */
		pci_warn(dev, "PCI settings rev %d not supported; using defaults\n",
			 hpx->revision); /* NVMe: 경고 후 기본값 적용 */
		hpx = &pci_default_type0; /* NVMe: 기본값 포인터로 대체 */
	}

	pci_write_config_byte(dev, PCI_CACHE_LINE_SIZE, hpx->cache_line_size); /* NVMe: Cache Line Size 쓰기 */
	pci_write_config_byte(dev, PCI_LATENCY_TIMER, hpx->latency_timer);     /* NVMe: Latency Timer 쓰기 */
	pci_read_config_word(dev, PCI_COMMAND, &pci_cmd);                      /* NVMe: 현재 COMMAND 레지스터 읽기 */
	if (hpx->enable_serr) /* NVMe: SERR 활성화 요청 시 */
		pci_cmd |= PCI_COMMAND_SERR; /* NVMe: SERR 비트 설정 */
	if (hpx->enable_perr) /* NVMe: PERR 활성화 요청 시 */
		pci_cmd |= PCI_COMMAND_PARITY; /* NVMe: Parity Error 비트 설정 */
	pci_write_config_word(dev, PCI_COMMAND, pci_cmd); /* NVMe: 갱신된 COMMAND 레지스터 쓰기 */

	/* Program bridge control value */
	if ((dev->class >> 8) == PCI_CLASS_BRIDGE_PCI) { /* NVMe: 대상이 PCI bridge이면 */
		pci_write_config_byte(dev, PCI_SEC_LATENCY_TIMER,
				      hpx->latency_timer); /* NVMe: secondary latency timer 쓰기 */
		pci_read_config_word(dev, PCI_BRIDGE_CONTROL, &pci_bctl); /* NVMe: bridge control 읽기 */
		if (hpx->enable_perr) /* NVMe: PERR 요청 시 bridge control에도 설정 */
			pci_bctl |= PCI_BRIDGE_CTL_PARITY; /* NVMe: bridge parity 비트 설정 */
		pci_write_config_word(dev, PCI_BRIDGE_CONTROL, pci_bctl); /* NVMe: bridge control 쓰기 */
	}
}

/*
 * decode_type0_hpx_record:
 *   ACPI _HPX Type 0 패키지를 host 구조체 hpx_type0로 디코딩한다.
 *   NVMe 디바이스의 _HPX 설정을 파싱하는 단계다.
 */
static acpi_status decode_type0_hpx_record(union acpi_object *record,
					   struct hpx_type0 *hpx0)
{
	int i; /* NVMe: 패키지 요소 순회 인덱스 */
	union acpi_object *fields = record->package.elements; /* NVMe: _HPX 패키지 요소 배열 */
	u32 revision = fields[1].integer.value; /* NVMe: 두 번째 요소가 revision */

	switch (revision) { /* NVMe: revision별 분기 */
	case 1:
		if (record->package.count != 6) /* NVMe: Type 0 revision 1은 6개 요소여야 함 */
			return AE_ERROR; /* NVMe: 형식 오류 반환 */
		for (i = 2; i < 6; i++) /* NVMe: 나머지 4개 정수 필드 검증 */
			if (fields[i].type != ACPI_TYPE_INTEGER) /* NVMe: 정수 타입 아니면 오류 */
				return AE_ERROR; /* NVMe: 타입 오류 반환 */
		hpx0->revision        = revision;        /* NVMe: revision 저장 */
		hpx0->cache_line_size = fields[2].integer.value; /* NVMe: cache line size 저장 */
		hpx0->latency_timer   = fields[3].integer.value; /* NVMe: latency timer 저장 */
		hpx0->enable_serr     = fields[4].integer.value; /* NVMe: SERR 활성화 저장 */
		hpx0->enable_perr     = fields[5].integer.value; /* NVMe: PERR 활성화 저장 */
		break; /* NVMe: revision 1 처리 완료 */
	default:
		pr_warn("%s: Type 0 Revision %d record not supported\n",
		       __func__, revision); /* NVMe: 미지원 revision 경고 */
		return AE_ERROR; /* NVMe: 오류 반환 */
	}
	return AE_OK; /* NVMe: 디코딩 성공 */
}

/* _HPX PCI-X Setting Record (Type 1) */
/* NVMe: PCI-X 전용 설정(현대 NVMe는 PCI Express이므로 거의 미사용) */
struct hpx_type1 {
	u32 revision;
	u8  max_mem_read;
	u8  avg_max_split;
	u16 tot_max_split;
};

/*
 * program_hpx_type1:
 *   _HPX Type 1 PCI-X 레코드를 적용한다. NVMe 장치는 PCIe이므로
 *   실제로는 경고만 출력하고 리턴한다.
 */
static void program_hpx_type1(struct pci_dev *dev, struct hpx_type1 *hpx)
{
	int pos; /* NVMe: PCI-X capability 위치 */

	if (!hpx) /* NVMe: Type 1 레코드 없으면 즉시 리턴 */
		return; /* NVMe: 적용 불필요 */

	pos = pci_find_capability(dev, PCI_CAP_ID_PCIX); /* NVMe: PCI-X capability 탐색 */
	if (!pos) /* NVMe: PCI-X capability가 없으면 */
		return; /* NVMe: PCIe NVMe이므로 보통 없음 */

	pci_warn(dev, "PCI-X settings not supported\n"); /* NVMe: PCI-X 설정은 NVMe에 미적용 */
}

/*
 * decode_type1_hpx_record:
 *   _HPX Type 1 PCI-X 레코드를 디코딩한다. NVMe에는 직접 사용되지
 *   않으나 구조체 형식을 맞추기 위해 파싱한다.
 */
static acpi_status decode_type1_hpx_record(union acpi_object *record,
					   struct hpx_type1 *hpx1)
{
	int i; /* NVMe: 패키지 요소 인덱스 */
	union acpi_object *fields = record->package.elements; /* NVMe: _HPX 패키지 배열 */
	u32 revision = fields[1].integer.value; /* NVMe: revision 필드 */

	switch (revision) { /* NVMe: revision별 분기 */
	case 1:
		if (record->package.count != 5) /* NVMe: Type 1 rev1은 5개 요소 */
			return AE_ERROR; /* NVMe: 형식 오류 */
		for (i = 2; i < 5; i++) /* NVMe: 정수 필드 3개 검증 */
			if (fields[i].type != ACPI_TYPE_INTEGER) /* NVMe: 정수 타입 검사 */
				return AE_ERROR; /* NVMe: 타입 오류 */
		hpx1->revision      = revision;        /* NVMe: revision 저장 */
		hpx1->max_mem_read  = fields[2].integer.value; /* NVMe: max mem read 저장 */
		hpx1->avg_max_split = fields[3].integer.value; /* NVMe: avg max split 저장 */
		hpx1->tot_max_split = fields[4].integer.value; /* NVMe: tot max split 저장 */
		break; /* NVMe: 처리 완료 */
	default:
		pr_warn("%s: Type 1 Revision %d record not supported\n",
		       __func__, revision); /* NVMe: 미지원 revision 경고 */
		return AE_ERROR; /* NVMe: 오류 반환 */
	}
	return AE_OK; /* NVMe: 디코딩 성공 */
}

/* _HPX PCI Express Setting Record (Type 2) */
/* NVMe: PCIe AER(Advanced Error Reporting), DEVCTL, LNKCTL 등에 관여 */
struct hpx_type2 {
	u32 revision;
	u32 unc_err_mask_and;
	u32 unc_err_mask_or;
	u32 unc_err_sever_and;
	u32 unc_err_sever_or;
	u32 cor_err_mask_and;
	u32 cor_err_mask_or;
	u32 adv_err_cap_and;
	u32 adv_err_cap_or;
	u16 pci_exp_devctl_and;
	u16 pci_exp_devctl_or;
	u16 pci_exp_lnkctl_and;
	u16 pci_exp_lnkctl_or;
	u32 sec_unc_err_sever_and;
	u32 sec_unc_err_sever_or;
	u32 sec_unc_err_mask_and;
	u32 sec_unc_err_mask_or;
};

/*
 * program_hpx_type2:
 *   _HPX Type 2 PCIe 설정 레코드를 NVMe 장치의 PCIe capability와
 *   AER(Advanced Error Reporting) 확장 capability에 적용한다. NVMe의
 *   correctable/uncorrectable error mask, error severity, ECRC 등이
 *   여기서 제어될 수 있다.
 */
static void program_hpx_type2(struct pci_dev *dev, struct hpx_type2 *hpx)
{
	int pos;              /* NVMe: AER 확장 capability 오프셋 */
	u32 reg32;            /* NVMe: 32비트 레지스터 임시값 */
	const struct pci_host_bridge *host; /* NVMe: NVMe가 연결된 host bridge */

	if (!hpx) /* NVMe: Type 2 레코드 없으면 리턴 */
		return; /* NVMe: 적용 불필요 */

	if (!pci_is_pcie(dev)) /* NVMe: 대상이 PCIe 디바이스가 아니면 */
		return; /* NVMe: NVMe는 PCIe이어야 함 */

	host = pci_find_host_bridge(dev->bus); /* NVMe: NVMe bus의 host bridge 획득 */

	/*
	 * Only do the _HPX Type 2 programming if OS owns PCIe native
	 * hotplug but not AER.
	 * NVMe: OS가 native hotplug을, AER은 firmware가 관리할 때만 적용.
	 */
	if (!host->native_pcie_hotplug || host->native_aer) /* NVMe: 조건 불만족 시 */
		return; /* NVMe: _HPX Type 2 적용 안 함 */

	if (hpx->revision > 1) { /* NVMe: revision 1 초과는 미지원 */
		pci_warn(dev, "PCIe settings rev %d not supported\n",
			 hpx->revision); /* NVMe: 경고 출력 */
		return; /* NVMe: 적용 중단 */
	}

	/*
	 * We only allow _HPX to program DEVCTL bits related to AER, namely
	 * PCI_EXP_DEVCTL_CERE, PCI_EXP_DEVCTL_NFERE, PCI_EXP_DEVCTL_FERE,
	 * and PCI_EXP_DEVCTL_URRE.
	 *
	 * The rest of DEVCTL is managed by the OS to make sure it's
	 * consistent with the rest of the platform.
	 * NVMe: DEVCTL의 AER 관련 비트만 _HPX가 덮어쓸 수 있도록 마스크 조정.
	 */
	hpx->pci_exp_devctl_and |= ~PCI_EXP_AER_FLAGS; /* NVMe: AER 비트 외에는 AND 마스크로 보존 */
	hpx->pci_exp_devctl_or &= PCI_EXP_AER_FLAGS;   /* NVMe: AER 비트만 OR로 설정 가능 */

	/* Initialize Device Control Register */
	pcie_capability_clear_and_set_word(dev, PCI_EXP_DEVCTL,
			~hpx->pci_exp_devctl_and, hpx->pci_exp_devctl_or); /* NVMe: PCIe DEVCTL 갱신 */

	/* Log if _HPX attempts to modify Link Control Register */
	if (pcie_cap_has_lnkctl(dev)) { /* NVMe: LNKCTL capability가 있으면 */
		if (hpx->pci_exp_lnkctl_and != 0xffff ||
		    hpx->pci_exp_lnkctl_or != 0) /* NVMe: _HPX가 LNKCTL을 변경하려 하면 */
			pci_info(dev, "_HPX attempts Link Control setting (AND %#06x OR %#06x)\n",
				 hpx->pci_exp_lnkctl_and,
				 hpx->pci_exp_lnkctl_or); /* NVMe: 정보 로깅 */
	}

	/* Find Advanced Error Reporting Enhanced Capability */
	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ERR); /* NVMe: AER 확장 capability 위치 탐색 */
	if (!pos) /* NVMe: AER capability가 없으면 */
		return; /* NVMe: AER 레지스터 프로그래밍 불가 */

	/* Initialize Uncorrectable Error Mask Register */
	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_MASK, &reg32); /* NVMe: AER Uncorrectable Error Mask 읽기 */
	reg32 = (reg32 & hpx->unc_err_mask_and) | hpx->unc_err_mask_or; /* NVMe: AND/OR 마스크 적용 */
	pci_write_config_dword(dev, pos + PCI_ERR_UNCOR_MASK, reg32); /* NVMe: 갱신된 마스크 쓰기 */

	/* Initialize Uncorrectable Error Severity Register */
	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_SEVER, &reg32); /* NVMe: Uncorrectable Error Severity 읽기 */
	reg32 = (reg32 & hpx->unc_err_sever_and) | hpx->unc_err_sever_or; /* NVMe: severity 마스크 적용 */
	pci_write_config_dword(dev, pos + PCI_ERR_UNCOR_SEVER, reg32); /* NVMe: severity 쓰기 */

	/* Initialize Correctable Error Mask Register */
	pci_read_config_dword(dev, pos + PCI_ERR_COR_MASK, &reg32); /* NVMe: Correctable Error Mask 읽기 */
	reg32 = (reg32 & hpx->cor_err_mask_and) | hpx->cor_err_mask_or; /* NVMe: correctable 마스크 적용 */
	pci_write_config_dword(dev, pos + PCI_ERR_COR_MASK, reg32); /* NVMe: correctable mask 쓰기 */

	/* Initialize Advanced Error Capabilities and Control Register */
	pci_read_config_dword(dev, pos + PCI_ERR_CAP, &reg32); /* NVMe: AER Cap/Control 읽기 */
	reg32 = (reg32 & hpx->adv_err_cap_and) | hpx->adv_err_cap_or; /* NVMe: AER cap 마스크 적용 */

	/* Don't enable ECRC generation or checking if unsupported */
	if (!(reg32 & PCI_ERR_CAP_ECRC_GENC)) /* NVMe: ECRC 생성 가능 비트가 없으면 */
		reg32 &= ~PCI_ERR_CAP_ECRC_GENE; /* NVMe: ECRC 생성 활성화 비트 해제 */
	if (!(reg32 & PCI_ERR_CAP_ECRC_CHKC)) /* NVMe: ECRC 체크 가능 비트가 없으면 */
		reg32 &= ~PCI_ERR_CAP_ECRC_CHKE; /* NVMe: ECRC 체크 활성화 비트 해제 */
	pci_write_config_dword(dev, pos + PCI_ERR_CAP, reg32); /* NVMe: AER Cap/Control 쓰기 */

	/*
	 * FIXME: The following two registers are not supported yet.
	 *
	 *   o Secondary Uncorrectable Error Severity Register
	 *   o Secondary Uncorrectable Error Mask Register
	 * NVMe: secondary AER 레지스터는 아직 미지원.
	 */
}

/*
 * decode_type2_hpx_record:
 *   ACPI _HPX Type 2 PCIe 레코드를 hpx_type2 구조체로 디코딩한다.
 *   NVMe AER/DEVCTL/LNKCTL 설정값을 ACPI에서 추출하는 단계.
 */
static acpi_status decode_type2_hpx_record(union acpi_object *record,
					   struct hpx_type2 *hpx2)
{
	int i; /* NVMe: 패키지 요소 인덱스 */
	union acpi_object *fields = record->package.elements; /* NVMe: _HPX 패키지 배열 */
	u32 revision = fields[1].integer.value; /* NVMe: revision 필드 */

	switch (revision) { /* NVMe: revision별 분기 */
	case 1:
		if (record->package.count != 18) /* NVMe: Type 2 rev1은 18개 요소 */
			return AE_ERROR; /* NVMe: 형식 오류 */
		for (i = 2; i < 18; i++) /* NVMe: 16개 정수 필드 검증 */
			if (fields[i].type != ACPI_TYPE_INTEGER) /* NVMe: 정수 타입 검사 */
				return AE_ERROR; /* NVMe: 타입 오류 */
		hpx2->revision      = revision;        /* NVMe: revision 저장 */
		hpx2->unc_err_mask_and      = fields[2].integer.value;  /* NVMe: uncorrectable mask AND */
		hpx2->unc_err_mask_or       = fields[3].integer.value;  /* NVMe: uncorrectable mask OR */
		hpx2->unc_err_sever_and     = fields[4].integer.value;  /* NVMe: uncorrectable severity AND */
		hpx2->unc_err_sever_or      = fields[5].integer.value;  /* NVMe: uncorrectable severity OR */
		hpx2->cor_err_mask_and      = fields[6].integer.value;  /* NVMe: correctable mask AND */
		hpx2->cor_err_mask_or       = fields[7].integer.value;  /* NVMe: correctable mask OR */
		hpx2->adv_err_cap_and       = fields[8].integer.value;  /* NVMe: AER cap AND */
		hpx2->adv_err_cap_or        = fields[9].integer.value;  /* NVMe: AER cap OR */
		hpx2->pci_exp_devctl_and    = fields[10].integer.value; /* NVMe: PCIe DEVCTL AND */
		hpx2->pci_exp_devctl_or     = fields[11].integer.value; /* NVMe: PCIe DEVCTL OR */
		hpx2->pci_exp_lnkctl_and    = fields[12].integer.value; /* NVMe: PCIe LNKCTL AND */
		hpx2->pci_exp_lnkctl_or     = fields[13].integer.value; /* NVMe: PCIe LNKCTL OR */
		hpx2->sec_unc_err_sever_and = fields[14].integer.value; /* NVMe: secondary unc severity AND */
		hpx2->sec_unc_err_sever_or  = fields[15].integer.value; /* NVMe: secondary unc severity OR */
		hpx2->sec_unc_err_mask_and  = fields[16].integer.value; /* NVMe: secondary unc mask AND */
		hpx2->sec_unc_err_mask_or   = fields[17].integer.value; /* NVMe: secondary unc mask OR */
		break; /* NVMe: 처리 완료 */
	default:
		pr_warn("%s: Type 2 Revision %d record not supported\n",
		       __func__, revision); /* NVMe: 미지원 revision 경고 */
		return AE_ERROR; /* NVMe: 오류 반환 */
	}
	return AE_OK; /* NVMe: 디코딩 성공 */
}

/* _HPX PCI Express Setting Record (Type 3) */
/* NVMe: PCIe DVSEC/Vendor specific capability 등 유연한 레지스터 패치 */
struct hpx_type3 {
	u16 device_type;
	u16 function_type;
	u16 config_space_location;
	u16 pci_exp_cap_id;
	u16 pci_exp_cap_ver;
	u16 pci_exp_vendor_id;
	u16 dvsec_id;
	u16 dvsec_rev;
	u16 match_offset;
	u32 match_mask_and;
	u32 match_value;
	u16 reg_offset;
	u32 reg_mask_and;
	u32 reg_mask_or;
};

enum hpx_type3_dev_type {
	HPX_TYPE_ENDPOINT	= BIT(0),  /* NVMe: PCIe endpoint, 일반 NVMe SSD */
	HPX_TYPE_LEG_END	= BIT(1),  /* NVMe: legacy endpoint */
	HPX_TYPE_RC_END		= BIT(2),  /* NVMe: root complex integrated endpoint */
	HPX_TYPE_RC_EC		= BIT(3),  /* NVMe: root complex event collector */
	HPX_TYPE_ROOT_PORT	= BIT(4),  /* NVMe: NVMe가 연결된 root port */
	HPX_TYPE_UPSTREAM	= BIT(5),  /* NVMe: upstream port of switch */
	HPX_TYPE_DOWNSTREAM	= BIT(6),  /* NVMe: downstream port of switch */
	HPX_TYPE_PCI_BRIDGE	= BIT(7),  /* NVMe: PCI bridge */
	HPX_TYPE_PCIE_BRIDGE	= BIT(8),  /* NVMe: PCIe bridge */
};

/*
 * hpx3_device_type:
 *   pci_dev의 PCIe device/port type을 _HPX Type 3의 device_type
 *   비트마스크로 변환한다. NVMe endpoint인지 Root Port인지 판별할 때
 *   사용된다.
 */
static u16 hpx3_device_type(struct pci_dev *dev)
{
	u16 pcie_type = pci_pcie_type(dev); /* NVMe: 디바이스의 PCIe 타입 획득 */
	static const int pcie_to_hpx3_type[] = { /* NVMe: PCIe 타입 -> HPX3 타입 매핑 테이블 */
		[PCI_EXP_TYPE_ENDPOINT]    = HPX_TYPE_ENDPOINT,
		[PCI_EXP_TYPE_LEG_END]     = HPX_TYPE_LEG_END,
		[PCI_EXP_TYPE_RC_END]      = HPX_TYPE_RC_END,
		[PCI_EXP_TYPE_RC_EC]       = HPX_TYPE_RC_EC,
		[PCI_EXP_TYPE_ROOT_PORT]   = HPX_TYPE_ROOT_PORT,
		[PCI_EXP_TYPE_UPSTREAM]    = HPX_TYPE_UPSTREAM,
		[PCI_EXP_TYPE_DOWNSTREAM]  = HPX_TYPE_DOWNSTREAM,
		[PCI_EXP_TYPE_PCI_BRIDGE]  = HPX_TYPE_PCI_BRIDGE,
		[PCI_EXP_TYPE_PCIE_BRIDGE] = HPX_TYPE_PCIE_BRIDGE,
	};

	if (pcie_type >= ARRAY_SIZE(pcie_to_hpx3_type)) /* NVMe: 매핑 테이블 범위 밖이면 */
		return 0; /* NVMe: 일치하는 HPX3 타입 없음 */

	return pcie_to_hpx3_type[pcie_type]; /* NVMe: HPX3 device_type 비트마스크 반환 */
}

enum hpx_type3_fn_type {
	HPX_FN_NORMAL		= BIT(0),  /* NVMe: 일반 PF */
	HPX_FN_SRIOV_PHYS	= BIT(1),  /* NVMe: SR-IOV physical function */
	HPX_FN_SRIOV_VIRT	= BIT(2),  /* NVMe: SR-IOV virtual function */
};

/*
 * hpx3_function_type:
 *   pci_dev가 일반 PF, SR-IOV PF, VF 중 어느 것인지 _HPX Type 3의
 *   function_type 비트마스크로 반환한다. NVMe SR-IOV 환경에서 VF에
 *   대한 레지스터 패치 적용 여부를 결정한다.
 */
static u8 hpx3_function_type(struct pci_dev *dev)
{
	if (dev->is_virtfn) /* NVMe: 가상 function이면 */
		return HPX_FN_SRIOV_VIRT; /* NVMe: VF 타입 반환 */
	else if (pci_find_ext_capability(dev, PCI_EXT_CAP_ID_SRIOV) > 0) /* NVMe: SR-IOV capability가 있으면 */
		return HPX_FN_SRIOV_PHYS; /* NVMe: SR-IOV PF 타입 반환 */
	else /* NVMe: 그 외 */
		return HPX_FN_NORMAL; /* NVMe: 일반 PF 타입 반환 */
}

/*
 * hpx3_cap_ver_matches:
 *   _HPX Type 3에 지정된 capability version이 실제 PCIe capability
 *   version과 일치하는지 검사한다. NVMe의 다양한 PCIe/DVSEC capability
 *   버전 호환성 판단에 사용된다.
 */
static bool hpx3_cap_ver_matches(u8 pcie_cap_id, u8 hpx3_cap_id)
{
	u8 cap_ver = hpx3_cap_id & 0xf; /* NVMe: 하위 4비트가 capability version */

	if ((hpx3_cap_id & BIT(4)) && cap_ver >= pcie_cap_id) /* NVMe: major 비트가 켜져 있고 버전 >= 실제 버전 */
		return true; /* NVMe: 버전 조건 만족 */
	else if (cap_ver == pcie_cap_id) /* NVMe: 정확히 같은 버전이면 */
		return true; /* NVMe: 버전 일치 */

	return false; /* NVMe: 버전 불일치 */
}

enum hpx_type3_cfg_loc {
	HPX_CFG_PCICFG		= 0,  /* NVMe: 일반 PCI config space */
	HPX_CFG_PCIE_CAP	= 1,  /* NVMe: PCIe capability 영역 */
	HPX_CFG_PCIE_CAP_EXT	= 2,  /* NVMe: PCIe extended capability 영역 */
	HPX_CFG_VEND_CAP	= 3,  /* NVMe: vendor specific capability */
	HPX_CFG_DVSEC		= 4,  /* NVMe: Designated Vendor Specific Extended Capability */
	HPX_CFG_MAX,
};

/*
 * program_hpx_type3_register:
 *   _HPX Type 3에 기술된 한 개의 레지스터 패치를 NVMe/PCI 디바이스
 *   config space에 적용한다. device_type, function_type, capability
 *   위치, match 조건, AND/OR 마스크를 모두 고려한다.
 */
static void program_hpx_type3_register(struct pci_dev *dev,
				       const struct hpx_type3 *reg)
{
	u32 match_reg, write_reg, header, orig_value; /* NVMe: match/원본/쓸 값, capability header */
	u16 pos; /* NVMe: config space 내 capability/레지스터 오프셋 */

	if (!(hpx3_device_type(dev) & reg->device_type)) /* NVMe: 디바이스 타입이 _HPX 조건과 맞지 않으면 */
		return; /* NVMe: 이 레지스터 패치 스킵 */

	if (!(hpx3_function_type(dev) & reg->function_type)) /* NVMe: function 타입이 맞지 않으면 */
		return; /* NVMe: 이 레지스터 패치 스킵 */

	switch (reg->config_space_location) { /* NVMe: config space 위치별 분기 */
	case HPX_CFG_PCICFG:
		pos = 0; /* NVMe: 일반 PCI config space base */
		break; /* NVMe: 위치 결정 완료 */
	case HPX_CFG_PCIE_CAP:
		pos = pci_find_capability(dev, reg->pci_exp_cap_id); /* NVMe: PCIe capability 오프셋 탐색 */
		if (pos == 0) /* NVMe: capability가 없으면 */
			return; /* NVMe: 패치 불가, 리턴 */

		break; /* NVMe: 위치 결정 완료 */
	case HPX_CFG_PCIE_CAP_EXT:
		pos = pci_find_ext_capability(dev, reg->pci_exp_cap_id); /* NVMe: PCIe extended capability 오프셋 탐색 */
		if (pos == 0) /* NVMe: extended capability가 없으면 */
			return; /* NVMe: 패치 불가, 리턴 */

		pci_read_config_dword(dev, pos, &header); /* NVMe: capability header 읽기 */
		if (!hpx3_cap_ver_matches(PCI_EXT_CAP_VER(header),
					  reg->pci_exp_cap_ver)) /* NVMe: capability version 비교 */
			return; /* NVMe: 버전 불일치 시 패치 스킵 */

		break; /* NVMe: 위치 결정 완료 */
	case HPX_CFG_VEND_CAP:
	case HPX_CFG_DVSEC:
	default:
		pci_warn(dev, "Encountered _HPX type 3 with unsupported config space location"); /* NVMe: 미지원 위치 경고 */
		return; /* NVMe: 패치 불가, 리턴 */
	}

	pci_read_config_dword(dev, pos + reg->match_offset, &match_reg); /* NVMe: match_offset에서 비교값 읽기 */

	if ((match_reg & reg->match_mask_and) != reg->match_value) /* NVMe: match 조건 불만족 시 */
		return; /* NVMe: 이 레지스터 패치 스킵 */

	pci_read_config_dword(dev, pos + reg->reg_offset, &write_reg); /* NVMe: 실제로 쓸 레지스터 읽기 */
	orig_value = write_reg;          /* NVMe: 원본값 백업(디버그용) */
	write_reg &= reg->reg_mask_and;  /* NVMe: AND 마스크 적용(비트 클리어) */
	write_reg |= reg->reg_mask_or;   /* NVMe: OR 마스크 적용(비트 설정) */

	if (orig_value == write_reg) /* NVMe: 값이 달라지지 않으면 */
		return; /* NVMe: 불필요한 쓰기 방지 */

	pci_write_config_dword(dev, pos + reg->reg_offset, write_reg); /* NVMe: 변경된 값을 config space에 쓰기 */

	pci_dbg(dev, "Applied _HPX3 at [0x%x]: 0x%08x -> 0x%08x",
		pos, orig_value, write_reg); /* NVMe: 적용 내역 디버그 로깅 */
}

/*
 * program_hpx_type3:
 *   _HPX Type 3 레코드를 NVMe PCIe 디바이스에 적용한다. Type 3은
 *   DVSEC, vendor capability 등 다양한 config space 레지스터를
 *   유연하게 패치할 수 있다.
 */
static void program_hpx_type3(struct pci_dev *dev, struct hpx_type3 *hpx)
{
	if (!hpx) /* NVMe: Type 3 레코드 없으면 */
		return; /* NVMe: 적용 불필요 */

	if (!pci_is_pcie(dev)) /* NVMe: 대상이 PCIe가 아니면 */
		return; /* NVMe: NVMe는 PCIe이어야 함 */

	program_hpx_type3_register(dev, hpx); /* NVMe: 단일 레지스터 패치 수행 */
}

/*
 * parse_hpx3_register:
 *   _HPX Type 3 패키지의 한 레지스터 설명을 hpx_type3 구조체로
 *   파싱한다. NVMe의 특정 capability 레지스터를 선택적으로 패치하기
 *   위한 정보를 추출한다.
 */
static void parse_hpx3_register(struct hpx_type3 *hpx3_reg,
				union acpi_object *reg_fields)
{
	hpx3_reg->device_type            = reg_fields[0].integer.value; /* NVMe: device type 필드 */
	hpx3_reg->function_type          = reg_fields[1].integer.value; /* NVMe: function type 필드 */
	hpx3_reg->config_space_location  = reg_fields[2].integer.value; /* NVMe: config space 위치 */
	hpx3_reg->pci_exp_cap_id         = reg_fields[3].integer.value; /* NVMe: PCIe capability ID */
	hpx3_reg->pci_exp_cap_ver        = reg_fields[4].integer.value; /* NVMe: capability version */
	hpx3_reg->pci_exp_vendor_id      = reg_fields[5].integer.value; /* NVMe: vendor ID */
	hpx3_reg->dvsec_id               = reg_fields[6].integer.value; /* NVMe: DVSEC ID */
	hpx3_reg->dvsec_rev              = reg_fields[7].integer.value; /* NVMe: DVSEC revision */
	hpx3_reg->match_offset           = reg_fields[8].integer.value; /* NVMe: match 레지스터 오프셋 */
	hpx3_reg->match_mask_and         = reg_fields[9].integer.value; /* NVMe: match AND 마스크 */
	hpx3_reg->match_value            = reg_fields[10].integer.value; /* NVMe: match 기대값 */
	hpx3_reg->reg_offset             = reg_fields[11].integer.value; /* NVMe: 수정할 레지스터 오프셋 */
	hpx3_reg->reg_mask_and           = reg_fields[12].integer.value; /* NVMe: 수정 AND 마스크 */
	hpx3_reg->reg_mask_or            = reg_fields[13].integer.value; /* NVMe: 수정 OR 마스크 */
}

/*
 * program_type3_hpx_record:
 *   _HPX Type 3 패키지 전체를 순회하며 포함된 모든 레지스터 패치를
 *   NVMe/PCI 디바이스에 적용한다.
 */
static acpi_status program_type3_hpx_record(struct pci_dev *dev,
					   union acpi_object *record)
{
	union acpi_object *fields = record->package.elements; /* NVMe: _HPX 패키지 배열 */
	u32 desc_count, expected_length, revision; /* NVMe: descriptor 개수, 예상 길이, revision */
	union acpi_object *reg_fields; /* NVMe: 개별 레지스터 패치 필드 포인터 */
	struct hpx_type3 hpx3;         /* NVMe: 파싱된 Type 3 레지스터 정보 */
	int i;                         /* NVMe: 루프 인덱스 */

	revision = fields[1].integer.value; /* NVMe: revision 필드 추출 */
	switch (revision) { /* NVMe: revision별 분기 */
	case 1:
		desc_count = fields[2].integer.value; /* NVMe: descriptor 개수 추출 */
		expected_length = 3 + desc_count * 14; /* NVMe: revision1은 descriptor당 14개 필드 */

		if (record->package.count != expected_length) /* NVMe: 패키지 길이 검증 */
			return AE_ERROR; /* NVMe: 길이 오류 */

		for (i = 2; i < expected_length; i++) /* NVMe: 모든 필드가 정수인지 검증 */
			if (fields[i].type != ACPI_TYPE_INTEGER) /* NVMe: 정수 타입 검사 */
				return AE_ERROR; /* NVMe: 타입 오류 */

		for (i = 0; i < desc_count; i++) { /* NVMe: descriptor 개수만큼 반복 */
			reg_fields = fields + 3 + i * 14; /* NVMe: i번째 descriptor 필드 시작 주소 */
			parse_hpx3_register(&hpx3, reg_fields); /* NVMe: descriptor 파싱 */
			program_hpx_type3(dev, &hpx3); /* NVMe: NVMe 디바이스에 적용 */
		}

		break; /* NVMe: revision 1 처리 완료 */
	default:
		printk(KERN_WARNING
			"%s: Type 3 Revision %d record not supported\n",
			__func__, revision); /* NVMe: 미지원 revision 경고 */
		return AE_ERROR; /* NVMe: 오류 반환 */
	}
	return AE_OK; /* NVMe: 적용 성공 */
}

/*
 * acpi_run_hpx:
 *   주어진 ACPI 핸들의 _HPX 메서드를 평가하여 Type 0/1/2/3 설정을
 *   NVMe/PCI 디바이스에 차례로 적용한다. NVMe 초기화 시 PCIe
 *   capability, AER, DVSEC 등에 platform quirk를 적용하는 통로다.
 */
static acpi_status acpi_run_hpx(struct pci_dev *dev, acpi_handle handle)
{
	acpi_status status; /* NVMe: ACPI 평가 상태 */
	struct acpi_buffer buffer = {ACPI_ALLOCATE_BUFFER, NULL}; /* NVMe: _HPX 반환값 버퍼 */
	union acpi_object *package, *record, *fields; /* NVMe: _HPX 패키지/레코드/필드 포인터 */
	struct hpx_type0 hpx0; /* NVMe: Type 0 설정 구조체 */
	struct hpx_type1 hpx1; /* NVMe: Type 1 설정 구조체 */
	struct hpx_type2 hpx2; /* NVMe: Type 2 PCIe/AER 설정 구조체 */
	u32 type;              /* NVMe: _HPX 레코드 타입 */
	int i;                 /* NVMe: 루프 인덱스 */

	status = acpi_evaluate_object(handle, "_HPX", NULL, &buffer); /* NVMe: _HPX 메서드 평가 */
	if (ACPI_FAILURE(status)) /* NVMe: _HPX 평가 실패 시 */
		return status; /* NVMe: 실패 상태 반환(_HPP 평가로 대체 가능) */

	package = (union acpi_object *)buffer.pointer; /* NVMe: _HPX 반환 객체 */
	if (package->type != ACPI_TYPE_PACKAGE) { /* NVMe: 반환값이 package가 아니면 */
		status = AE_ERROR; /* NVMe: 형식 오류 설정 */
		goto exit; /* NVMe: 정리 후 종료 */
	}

	for (i = 0; i < package->package.count; i++) { /* NVMe: _HPX 내 각 레코드 순회 */
		record = &package->package.elements[i]; /* NVMe: i번째 레코드 */
		if (record->type != ACPI_TYPE_PACKAGE) { /* NVMe: 레코드도 package여야 함 */
			status = AE_ERROR; /* NVMe: 형식 오류 */
			goto exit; /* NVMe: 정리 후 종료 */
		}

		fields = record->package.elements; /* NVMe: 레코드 필드 배열 */
		if (fields[0].type != ACPI_TYPE_INTEGER ||
		    fields[1].type != ACPI_TYPE_INTEGER) { /* NVMe: 타입/revision은 정수여야 함 */
			status = AE_ERROR; /* NVMe: 형식 오류 */
			goto exit; /* NVMe: 정리 후 종료 */
		}

		type = fields[0].integer.value; /* NVMe: 레코드 타입 추출 */
		switch (type) { /* NVMe: 레코드 타입별 분기 */
		case 0:
			memset(&hpx0, 0, sizeof(hpx0)); /* NVMe: Type 0 구조체 초기화 */
			status = decode_type0_hpx_record(record, &hpx0); /* NVMe: Type 0 디코딩 */
			if (ACPI_FAILURE(status)) /* NVMe: 디코딩 실패 시 */
				goto exit; /* NVMe: 정리 후 종료 */
			program_hpx_type0(dev, &hpx0); /* NVMe: Type 0 설정 적용 */
			break; /* NVMe: Type 0 처리 완료 */
		case 1:
			memset(&hpx1, 0, sizeof(hpx1)); /* NVMe: Type 1 구조체 초기화 */
			status = decode_type1_hpx_record(record, &hpx1); /* NVMe: Type 1 디코딩 */
			if (ACPI_FAILURE(status)) /* NVMe: 디코딩 실패 시 */
				goto exit; /* NVMe: 정리 후 종료 */
			program_hpx_type1(dev, &hpx1); /* NVMe: Type 1 설정 적용(PCI-X) */
			break; /* NVMe: Type 1 처리 완료 */
		case 2:
			memset(&hpx2, 0, sizeof(hpx2)); /* NVMe: Type 2 구조체 초기화 */
			status = decode_type2_hpx_record(record, &hpx2); /* NVMe: Type 2 디코딩 */
			if (ACPI_FAILURE(status)) /* NVMe: 디코딩 실패 시 */
				goto exit; /* NVMe: 정리 후 종료 */
			program_hpx_type2(dev, &hpx2); /* NVMe: Type 2 PCIe/AER 설정 적용 */
			break; /* NVMe: Type 2 처리 완료 */
		case 3:
			status = program_type3_hpx_record(dev, record); /* NVMe: Type 3 레지스터 패치 적용 */
			if (ACPI_FAILURE(status)) /* NVMe: 적용 실패 시 */
				goto exit; /* NVMe: 정리 후 종료 */
			break; /* NVMe: Type 3 처리 완료 */
		default:
			pr_err("%s: Type %d record not supported\n",
			       __func__, type); /* NVMe: 미지원 타입 오류 로깅 */
			status = AE_ERROR; /* NVMe: 오류 설정 */
			goto exit; /* NVMe: 정리 후 종료 */
		}
	}
 exit:
	kfree(buffer.pointer); /* NVMe: _HPX 반환 버퍼 메모리 해제 */
	return status;         /* NVMe: 최종 상태 반환 */
}

/*
 * acpi_run_hpp:
 *   ACPI _HPP(Hot Plug Parameters) 메서드를 평가하여 Type 0와
 *   동일한 설정을 NVMe/PCI 디바이스에 적용한다. _HPX가 없는 플랫폼의
 *   fallback 경로다.
 */
static acpi_status acpi_run_hpp(struct pci_dev *dev, acpi_handle handle)
{
	acpi_status status; /* NVMe: ACPI 평가 상태 */
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL }; /* NVMe: _HPP 반환 버퍼 */
	union acpi_object *package, *fields; /* NVMe: _HPP 패키지/필드 포인터 */
	struct hpx_type0 hpx0; /* NVMe: Type 0 설정 구조체(_HPP는 Type 0만 해당) */
	int i; /* NVMe: 루프 인덱스 */

	memset(&hpx0, 0, sizeof(hpx0)); /* NVMe: Type 0 구조체 초기화 */

	status = acpi_evaluate_object(handle, "_HPP", NULL, &buffer); /* NVMe: _HPP 메서드 평가 */
	if (ACPI_FAILURE(status)) /* NVMe: _HPP 평가 실패 시 */
		return status; /* NVMe: 실패 반환 */

	package = (union acpi_object *) buffer.pointer; /* NVMe: _HPP 반환 객체 */
	if (package->type != ACPI_TYPE_PACKAGE ||
	    package->package.count != 4) { /* NVMe: _HPP는 4개 정수의 package여야 함 */
		status = AE_ERROR; /* NVMe: 형식 오류 */
		goto exit; /* NVMe: 정리 후 종료 */
	}

	fields = package->package.elements; /* NVMe: _HPP 필드 배열 */
	for (i = 0; i < 4; i++) { /* NVMe: 4개 필드 타입 검증 */
		if (fields[i].type != ACPI_TYPE_INTEGER) { /* NVMe: 정수 타입 아니면 */
			status = AE_ERROR; /* NVMe: 형식 오류 */
			goto exit; /* NVMe: 정리 후 종료 */
		}
	}

	hpx0.revision        = 1;                       /* NVMe: _HPP는 revision 1 고정 */
	hpx0.cache_line_size = fields[0].integer.value; /* NVMe: cache line size */
	hpx0.latency_timer   = fields[1].integer.value; /* NVMe: latency timer */
	hpx0.enable_serr     = fields[2].integer.value; /* NVMe: SERR 활성화 */
	hpx0.enable_perr     = fields[3].integer.value; /* NVMe: PERR 활성화 */

	program_hpx_type0(dev, &hpx0); /* NVMe: _HPP 설정을 NVMe 디바이스에 적용 */

exit:
	kfree(buffer.pointer); /* NVMe: _HPP 반환 버퍼 메모리 해제 */
	return status;         /* NVMe: 최종 상태 반환 */
}

/*
 * pci_acpi_program_hp_params:
 *   NVMe/PCI 디바이스에 대해 상위 bridge 범위에서 _HPX 또는 _HPP를
 *   찾아 평가하고 적용한다. NVMe SSD probe 시 호출되어 PCIe capability,
 *   cache line, error handling 등의 platform 권장값을 반영한다.
 */
int pci_acpi_program_hp_params(struct pci_dev *dev)
{
	acpi_status status;  /* NVMe: ACPI 평가 상태 */
	acpi_handle handle, phandle; /* NVMe: 현재/부모 ACPI 핸들 */
	struct pci_bus *pbus; /* NVMe: NVMe 디바이스가 속한 bus를 따라 상위로 이동 */

	if (acpi_pci_disabled) /* NVMe: ACPI PCI가 비활성이면 */
		return -ENODEV; /* NVMe: ACPI 기반 설정 불가 */

	handle = NULL; /* NVMe: 초기 핸들 null */
	for (pbus = dev->bus; pbus; pbus = pbus->parent) { /* NVMe: NVMe bus에서 root bus 방향으로 순회 */
		handle = acpi_pci_get_bridge_handle(pbus); /* NVMe: 각 bridge의 ACPI 핸들 획득 시도 */
		if (handle) /* NVMe: 핸들을 찾으면 */
			break; /* NVMe: 순회 종료 */
	}

	/*
	 * _HPP settings apply to all child buses, until another _HPP is
	 * encountered. If we don't find an _HPP for the input pci dev,
	 * look for it in the parent device scope since that would apply to
	 * this pci dev.
	 * NVMe: _HPP/_HPX는 하위 bus에 상속되므로, NVMe 핸들에서 못 찾으면
	 *       부모 bridge 범위까지 올라가며 검색한다.
	 */
	while (handle) { /* NVMe: 유효한 ACPI 핸들이 있는 동안 */
		status = acpi_run_hpx(dev, handle); /* NVMe: 우선 _HPX 시도 */
		if (ACPI_SUCCESS(status)) /* NVMe: _HPX 성공 시 */
			return 0; /* NVMe: 적용 완료 */
		status = acpi_run_hpp(dev, handle); /* NVMe: _HPX 없으면 _HPP 시도 */
		if (ACPI_SUCCESS(status)) /* NVMe: _HPP 성공 시 */
			return 0; /* NVMe: 적용 완료 */
		if (acpi_is_root_bridge(handle)) /* NVMe: root bridge에 도달하면 */
			break; /* NVMe: 더 이상 부모 없음 */
		status = acpi_get_parent(handle, &phandle); /* NVMe: 부모 ACPI 핸들 획득 */
		if (ACPI_FAILURE(status)) /* NVMe: 부모 획득 실패 시 */
			break; /* NVMe: 검색 종료 */
		handle = phandle; /* NVMe: 부모 핸들로 이동 */
	}
	return -ENODEV; /* NVMe: _HPX/_HPP 둘 다 없음 */
}

/**
 * pciehp_is_native - Check whether a hotplug port is handled by the OS
 * @bridge: Hotplug port to check
 *
 * Returns true if the given @bridge is handled by the native PCIe hotplug
 * driver.
 * NVMe: NVMe SSD가 연결된 Root Port의 native hotplug 처리 여부를 확인.
 *       native hotplug가 활성이면 NVMe 장치의 surprise removal 등을 OS가 직접 처리.
 */
bool pciehp_is_native(struct pci_dev *bridge)
{
	const struct pci_host_bridge *host; /* NVMe: bridge가 속한 host bridge */

	if (!IS_ENABLED(CONFIG_HOTPLUG_PCI_PCIE)) /* NVMe: pciehp 커널 설정이 꺼져 있으면 */
		return false; /* NVMe: native hotplug 불가 */

	if (pcie_ports_native) /* NVMe: 커널 파라미터로 native 모드 강제 시 */
		return true; /* NVMe: native hotplug 활성 */

	host = pci_find_host_bridge(bridge->bus); /* NVMe: host bridge 메타정보 획득 */
	return host->native_pcie_hotplug; /* NVMe: host bridge의 native hotplug 플래그 반환 */
}

/**
 * shpchp_is_native - Check whether a hotplug port is handled by the OS
 * @bridge: Hotplug port to check
 *
 * Returns true if the given @bridge is handled by the native SHPC hotplug
 * driver.
 * NVMe: legacy SHPC hotplug 여부 확인. NVMe는 PCIe이므로 주로 pciehp 사용.
 */
bool shpchp_is_native(struct pci_dev *bridge)
{
	return bridge->shpc_managed; /* NVMe: bridge의 SHPC 관리 플래그 반환 */
}

/**
 * pci_acpi_wake_bus - Root bus wakeup notification fork function.
 * @context: Device wakeup context.
 * NVMe: ACPI wake 이벤트 발생 시 NVMe가 속한 root bus의 PME 처리를
 *       fork하는 콜백.
 */
static void pci_acpi_wake_bus(struct acpi_device_wakeup_context *context)
{
	pci_pme_wakeup_bus(to_pci_host_bridge(context->dev)->bus); /* NVMe: host bridge의 root bus로 PME wake 전파 */
}

/**
 * pci_acpi_wake_dev - PCI device wakeup notification work function.
 * @context: Device wakeup context.
 * NVMe: ACPI wake 이벤트 발생 시 NVMe endpoint의 PME status를 클리어하고
 *       resume를 요청하는 work 함수.
 */
static void pci_acpi_wake_dev(struct acpi_device_wakeup_context *context)
{
	struct pci_dev *pci_dev; /* NVMe: wake 이벤트를 받은 PCI 디바이스 */

	pci_dev = to_pci_dev(context->dev); /* NVMe: ACPI context에서 pci_dev 추출 */

	if (pci_dev->pme_poll) /* NVMe: PME 폴링 중이면 */
		pci_dev->pme_poll = false; /* NVMe: 폴링 중지 */

	if (pci_dev->current_state == PCI_D3cold) { /* NVMe: D3cold에서 wake되면 */
		pci_wakeup_event(pci_dev);          /* NVMe: wake 이벤트 기록 */
		pm_request_resume(&pci_dev->dev);   /* NVMe: 디바이스 resume 요청 */
		return; /* NVMe: D3cold wake 처리 완료 */
	}

	/* Clear PME Status if set. */
	if (pci_dev->pme_support) /* NVMe: 디바이스가 PME를 지원하면 */
		pci_check_pme_status(pci_dev); /* NVMe: PME status 클리어 */

	pci_wakeup_event(pci_dev);        /* NVMe: wake 이벤트 기록 */
	pm_request_resume(&pci_dev->dev); /* NVMe: resume 요청 */

	pci_pme_wakeup_bus(pci_dev->subordinate); /* NVMe: 하위 bus에도 PME wake 전파 */
}

/**
 * pci_acpi_add_root_pm_notifier - Register PM notifier for root PCI bus.
 * @dev: PCI root bridge ACPI device.
 * @root: PCI root corresponding to @dev.
 * NVMe: NVMe가 속한 root bridge에 ACPI PM notifier를 등록하여 시스템
 *       wake 이벤트를 처리할 수 있게 한다.
 */
acpi_status pci_acpi_add_root_pm_notifier(struct acpi_device *dev,
					  struct acpi_pci_root *root)
{
	return acpi_add_pm_notifier(dev, root->bus->bridge, pci_acpi_wake_bus); /* NVMe: root bridge에 wake notifier 등록 */
}

/**
 * pci_acpi_add_pm_notifier - Register PM notifier for given PCI device.
 * @dev: ACPI device to add the notifier for.
 * @pci_dev: PCI device to check for the PME status if an event is signaled.
 * NVMe: 개별 NVMe endpoint에 ACPI PM notifier를 등록한다.
 */
acpi_status pci_acpi_add_pm_notifier(struct acpi_device *dev,
				     struct pci_dev *pci_dev)
{
	return acpi_add_pm_notifier(dev, &pci_dev->dev, pci_acpi_wake_dev); /* NVMe: NVMe 디바이스에 wake notifier 등록 */
}

/*
 * _SxD returns the D-state with the highest power
 * (lowest D-state number) supported in the S-state "x".
 *
 * If the devices does not have a _PRW
 * (Power Resources for Wake) supporting system wakeup from "x"
 * then the OS is free to choose a lower power (higher number
 * D-state) than the return value from _SxD.
 *
 * But if _PRW is enabled at S-state "x", the OS
 * must not choose a power lower than _SxD --
 * unless the device has an _SxW method specifying
 * the lowest power (highest D-state number) the device
 * may enter while still able to wake the system.
 *
 * ie. depending on global OS policy:
 *
 * if (_PRW at S-state x)
 *	choose from highest power _SxD to lowest power _SxW
 * else // no _PRW at S-state x
 *	choose highest power _SxD or any lower power
 * NVMe: _SxD/_SxW/_PRW를 통해 NVMe가 시스템 수면 상태에서 어느 D-state까지
 *       진입할 수 있는지 결정.
 */

/*
 * acpi_pci_choose_state:
 *   NVMe 디바이스가 진입할 수 있는 가장 낮은 전력 ACPI D-state를
 *   선택한다. NVMe suspend/resume에서 pci_set_power_state()로 전달된다.
 */
pci_power_t acpi_pci_choose_state(struct pci_dev *pdev)
{
	int acpi_state, d_max; /* NVMe: ACPI 상태, 허용 최대 D-state */

	if (pdev->no_d3cold || !pdev->d3cold_allowed) /* NVMe: D3cold 금지 시 */
		d_max = ACPI_STATE_D3_HOT; /* NVMe: D3hot까지만 허용 */
	else /* NVMe: D3cold 허용 시 */
		d_max = ACPI_STATE_D3_COLD; /* NVMe: D3cold까지 허용 */
	acpi_state = acpi_pm_device_sleep_state(&pdev->dev, NULL, d_max); /* NVMe: ACPI _SxD/_SxW 평가 */
	if (acpi_state < 0) /* NVMe: ACPI 상태 결정 실패 시 */
		return PCI_POWER_ERROR; /* NVMe: 전원 상태 오류 반환 */

	switch (acpi_state) { /* NVMe: ACPI 상태 -> PCI D-state 변환 */
	case ACPI_STATE_D0:
		return PCI_D0; /* NVMe: 완전 활성 상태 */
	case ACPI_STATE_D1:
		return PCI_D1; /* NVMe: D1 상태 */
	case ACPI_STATE_D2:
		return PCI_D2; /* NVMe: D2 상태 */
	case ACPI_STATE_D3_HOT:
		return PCI_D3hot; /* NVMe: D3hot 상태 */
	case ACPI_STATE_D3_COLD:
		return PCI_D3cold; /* NVMe: D3cold 상태 */
	}
	return PCI_POWER_ERROR; /* NVMe: 매핑되지 않는 상태는 오류 */
}

/*
 * pci_set_acpi_fwnode:
 *   NVMe pci_dev의 firmware node가 없고 아직 추가되지 않은 경우
 *   ACPI companion을 연결한다. 이후 ACPI 기반 속성(_DSD, _PRW 등)이
 *   NVMe 드라이버에서 조회 가능해진다.
 */
static struct acpi_device *acpi_pci_find_companion(struct device *dev);

void pci_set_acpi_fwnode(struct pci_dev *dev)
{
	if (!dev_fwnode(&dev->dev) && !pci_dev_is_added(dev)) /* NVMe: fwnode 없고 미등록 상태면 */
		ACPI_COMPANION_SET(&dev->dev,
				   acpi_pci_find_companion(&dev->dev)); /* NVMe: ACPI companion 설정 */
}

/**
 * pci_dev_acpi_reset - do a function level reset using _RST method
 * @dev: device to reset
 * @probe: if true, return 0 if device supports _RST
 * NVMe: ACPI _RST 메서드를 이용한 NVMe function-level reset. NVMe
 *       드라이버의 controller reset 경로에서 사용될 수 있다.
 */
int pci_dev_acpi_reset(struct pci_dev *dev, bool probe)
{
	acpi_handle handle = ACPI_HANDLE(&dev->dev); /* NVMe: NVMe 디바이스의 ACPI 핸들 */
	int ret; /* NVMe: 반환 코드 */

	if (!handle || !acpi_has_method(handle, "_RST")) /* NVMe: _RST 메서드 없으면 */
		return -ENOTTY; /* NVMe: reset 불가 반환 */

	if (probe) /* NVMe: probe 모드면 _RST 지원 여부만 확인 */
		return 0; /* NVMe: _RST 지원함 */

	ret = pci_dev_reset_iommu_prepare(dev); /* NVMe: reset 전 IOMMU 안전 분리 준비 */
	if (ret) { /* NVMe: IOMMU 준비 실패 시 */
		pci_err(dev, "failed to stop IOMMU for a PCI reset: %d\n", ret); /* NVMe: 오류 로깅 */
		return ret; /* NVMe: IOMMU 오류 반환 */
	}

	if (ACPI_FAILURE(acpi_evaluate_object(handle, "_RST", NULL, NULL))) { /* NVMe: _RST 메서드 실행 */
		pci_warn(dev, "ACPI _RST failed\n"); /* NVMe: _RST 실패 경고 */
		ret = -ENOTTY; /* NVMe: 실패 코드 설정 */
	}

	pci_dev_reset_iommu_done(dev); /* NVMe: reset 후 IOMMU 복원 */
	return ret; /* NVMe: reset 결과 반환 */
}

/*
 * acpi_pci_power_manageable:
 *   NVMe 디바이스가 ACPI를 통해 전원 관리 가능한지 확인한다.
 *   _PSx 메서드가 있으면 true.
 */
bool acpi_pci_power_manageable(struct pci_dev *dev)
{
	struct acpi_device *adev = ACPI_COMPANION(&dev->dev); /* NVMe: NVMe의 ACPI companion */

	return adev && acpi_device_power_manageable(adev); /* NVMe: companion 있고 전원 관리 가능하면 true */
}

/*
 * acpi_pci_bridge_d3:
 *   NVMe가 연결된 PCIe bridge(또는 Root Port)가 D3 상태에서 wake를
 *   유지하며 hotplug 이벤트를 처리할 수 있는지 판단한다. NVMe
 *   hotplug/surprise removal 시 bridge 전원 정책에 영향을 준다.
 */
bool acpi_pci_bridge_d3(struct pci_dev *dev)
{
	struct pci_dev *rpdev; /* NVMe: 연결된 Root Port 디바이스 */
	struct acpi_device *adev, *rpadev; /* NVMe: bridge 및 Root Port ACPI companion */
	const union acpi_object *obj; /* NVMe: _DSD 속성 객체 */

	if (acpi_pci_disabled || !dev->is_pciehp) /* NVMe: ACPI PCI 꺼져 있거나 PCIe hotplug 아니면 */
		return false; /* NVMe: D3 hotplug 불가 */

	adev = ACPI_COMPANION(&dev->dev); /* NVMe: bridge의 ACPI companion */

	if (adev) {
		/*
		 * If the bridge has _S0W, whether or not it can go into D3
		 * depends on what is returned by that object.  In particular,
		 * if the power state returned by _S0W is D2 or shallower,
		 * entering D3 should not be allowed.
		 * NVMe: bridge의 _S0W가 D2 이하면 D3 진입 불가.
		 */
		if (acpi_dev_power_state_for_wake(adev) <= ACPI_STATE_D2) /* NVMe: wake 가능 상태가 D2 이하면 */
			return false; /* NVMe: D3 진입 금지 */

		/*
		 * Otherwise, assume that the bridge can enter D3 so long as it
		 * is power-manageable via ACPI.
		 * NVMe: ACPI 전원 관리 가능하면 D3 진입 가능.
		 */
		if (acpi_device_power_manageable(adev)) /* NVMe: ACPI로 전원 관리 가능하면 */
			return true; /* NVMe: D3 허용 */
	}

	rpdev = pcie_find_root_port(dev); /* NVMe: NVMe 쪽 Root Port 찾기 */
	if (!rpdev) /* NVMe: Root Port를 찾지 못하면 */
		return false; /* NVMe: D3 불가 */

	if (rpdev == dev) /* NVMe: 대상이 자신이 Root Port이면 */
		rpadev = adev; /* NVMe: 이미 획득한 ACPI companion 사용 */
	else /* NVMe: 그 외 bridge이면 */
		rpadev = ACPI_COMPANION(&rpdev->dev); /* NVMe: Root Port의 ACPI companion 획득 */

	if (!rpadev) /* NVMe: Root Port에 ACPI companion이 없으면 */
		return false; /* NVMe: D3 불가 */

	/*
	 * If the Root Port cannot signal wakeup signals at all, i.e., it
	 * doesn't supply a wakeup GPE via _PRW, it cannot signal hotplug
	 * events from low-power states including D3hot and D3cold.
	 * NVMe: Root Port에 _PRW 기반 wake GPE가 없으면 D3에서 hotplug 이벤트 처리 불가.
	 */
	if (!rpadev->wakeup.flags.valid) /* NVMe: wake 플래그가 유효하지 않으면 */
		return false; /* NVMe: D3 불가 */

	/*
	 * In the bridge-below-a-Root-Port case, evaluate _S0W for the Root Port
	 * to verify whether or not it can signal wakeup from D3.
	 * NVMe: Root Port 아래 bridge인 경우 Root Port의 _S0W도 확인.
	 */
	if (rpadev != adev &&
	    acpi_dev_power_state_for_wake(rpadev) <= ACPI_STATE_D2) /* NVMe: Root Port _S0W가 D2 이하면 */
		return false; /* NVMe: D3 불가 */

	/*
	 * The "HotPlugSupportInD3" property in a Root Port _DSD indicates
	 * the Port can signal hotplug events while in D3.  We assume any
	 * bridges *below* that Root Port can also signal hotplug events
	 * while in D3.
	 * NVMe: Root Port _DSD에 HotPlugSupportInD3=1이면 D3에서 hotplug 지원.
	 */
	if (!acpi_dev_get_property(rpadev, "HotPlugSupportInD3",
				   ACPI_TYPE_INTEGER, &obj) &&
	    obj->integer.value == 1) /* NVMe: 속성이 존재하고 값이 1이면 */
		return true; /* NVMe: D3 hotplug 허용 */

	return false; /* NVMe: 위 조건을 모두 만족하지 못하면 D3 불가 */
}

/*
 * acpi_pci_config_space_access:
 *   ACPI _REG 메서드를 호출해 NVMe 디바이스의 PCI config space 접근
 *   가능/불가를 AML에 통지한다. D3cold 진입/복귀 시 호출되어 config
 *   space 접근성을 동기화한다.
 */
static void acpi_pci_config_space_access(struct pci_dev *dev, bool enable)
{
	int val = enable ? ACPI_REG_CONNECT : ACPI_REG_DISCONNECT; /* NVMe: connect/disconnect 값 설정 */
	int ret = acpi_evaluate_reg(ACPI_HANDLE(&dev->dev),
				    ACPI_ADR_SPACE_PCI_CONFIG, val); /* NVMe: _REG(PCIFG, connect/disconnect) 평가 */
	if (ret) /* NVMe: _REG 평가 실패 시 */
		pci_dbg(dev, "ACPI _REG %s evaluation failed (%d)\n",
			enable ? "connect" : "disconnect", ret); /* NVMe: 디버그 로깅 */
}

/*
 * acpi_pci_set_power_state:
 *   NVMe 디바이스의 전원 상태를 PCI_D0/PCI_D3hot/PCI_D3cold 등으로
 *   ACPI _PSx 메서드를 통해 전환한다. NVMe reset, suspend, resume 시
 *   pci_set_power_state() 아래에서 호출된다.
 */
int acpi_pci_set_power_state(struct pci_dev *dev, pci_power_t state)
{
	struct acpi_device *adev = ACPI_COMPANION(&dev->dev); /* NVMe: NVMe의 ACPI companion */
	static const u8 state_conv[] = { /* NVMe: PCI D-state -> ACPI D-state 변환 테이블 */
		[PCI_D0] = ACPI_STATE_D0,
		[PCI_D1] = ACPI_STATE_D1,
		[PCI_D2] = ACPI_STATE_D2,
		[PCI_D3hot] = ACPI_STATE_D3_HOT,
		[PCI_D3cold] = ACPI_STATE_D3_COLD,
	};
	int error; /* NVMe: ACPI 전환 오류 코드 */

	/* If the ACPI device has _EJ0, ignore the device */
	if (!adev || acpi_has_method(adev->handle, "_EJ0")) /* NVMe: ACPI companion 없거나 _EJ0(eject) 있으면 */
		return -ENODEV; /* NVMe: 전원 상태 전환 불가 */

	switch (state) { /* NVMe: 전달된 상태값 검증 */
	case PCI_D0:
	case PCI_D1:
	case PCI_D2:
	case PCI_D3hot:
	case PCI_D3cold:
		break; /* NVMe: 유효한 상태 */
	default:
		return -EINVAL; /* NVMe: 잘못된 상태 */
	}

	if (state == PCI_D3cold) { /* NVMe: D3cold로 진입 시 */
		if (dev_pm_qos_flags(&dev->dev, PM_QOS_FLAG_NO_POWER_OFF) ==
				PM_QOS_FLAGS_ALL) /* NVMe: NO_POWER_OFF QoS 제약이 모두 설정되면 */
			return -EBUSY; /* NVMe: 전원 차단 거부 */

		/* Notify AML lack of PCI config space availability */
		acpi_pci_config_space_access(dev, false); /* NVMe: AML에 config space 접근 불가 통지 */
	}

	error = acpi_device_set_power(adev, state_conv[state]); /* NVMe: ACPI _PSx 메서드 실행 */
	if (error) /* NVMe: ACPI 상태 전환 실패 시 */
		return error; /* NVMe: ACPI 오류 반환 */

	pci_dbg(dev, "power state changed by ACPI to %s\n",
	        acpi_power_state_string(adev->power.state)); /* NVMe: 변경된 상태 디버그 로깅 */

	/*
	 * Notify AML of PCI config space availability.  Config space is
	 * accessible in all states except D3cold; the only transitions
	 * that change availability are transitions to D3cold and from
	 * D3cold to D0.
	 * NVMe: D3cold->D0 복귀 시 AML에 config space 접근 가능 통지.
	 */
	if (state == PCI_D0) /* NVMe: 활성 상태로 돌아오면 */
		acpi_pci_config_space_access(dev, true); /* NVMe: AML에 config space 접근 가능 통지 */

	return 0; /* NVMe: 전원 상태 전환 성공 */
}

/*
 * acpi_pci_get_power_state:
 *   ACPI를 통해 현재 NVMe 디바이스의 전원 상태를 조회한다.
 */
pci_power_t acpi_pci_get_power_state(struct pci_dev *dev)
{
	struct acpi_device *adev = ACPI_COMPANION(&dev->dev); /* NVMe: NVMe의 ACPI companion */
	static const pci_power_t state_conv[] = { /* NVMe: ACPI D-state -> PCI D-state 변환 테이블 */
		[ACPI_STATE_D0]      = PCI_D0,
		[ACPI_STATE_D1]      = PCI_D1,
		[ACPI_STATE_D2]      = PCI_D2,
		[ACPI_STATE_D3_HOT]  = PCI_D3hot,
		[ACPI_STATE_D3_COLD] = PCI_D3cold,
	};
	int state; /* NVMe: ACPI 내부 상태 */

	if (!adev || !acpi_device_power_manageable(adev)) /* NVMe: ACPI companion 없거나 전원 관리 불가면 */
		return PCI_UNKNOWN; /* NVMe: 상태를 알 수 없음 */

	state = adev->power.state; /* NVMe: ACPI device의 현재 상태 읽기 */
	if (state == ACPI_STATE_UNKNOWN) /* NVMe: ACPI 상태를 모르면 */
		return PCI_UNKNOWN; /* NVMe: PCI 상태도 알 수 없음 */

	return state_conv[state]; /* NVMe: PCI D-state로 변환하여 반환 */
}

/*
 * acpi_pci_refresh_power_state:
 *   NVMe 디바이스의 ACPI 전원 상태를 갱신하여 실제 하드웨어 상태와
 *   동기화한다. resume 후 상태 불일치 문제를 방지한다.
 */
void acpi_pci_refresh_power_state(struct pci_dev *dev)
{
	struct acpi_device *adev = ACPI_COMPANION(&dev->dev); /* NVMe: NVMe의 ACPI companion */

	if (adev && acpi_device_power_manageable(adev)) /* NVMe: ACPI 전원 관리 가능하면 */
		acpi_device_update_power(adev, NULL); /* NVMe: ACPI 상태 갱신 */
}

/*
 * acpi_pci_propagate_wakeup:
 *   NVMe endpoint에서 상위 bus를 따라 root bus까지 wake 기능을
 *   전파하며 활성화/비활성화한다. NVMe wake-on-LAN/디스크 wake 등에서
 *   상위 bridge의 wake도 함께 설정해야 한다.
 */
static int acpi_pci_propagate_wakeup(struct pci_bus *bus, bool enable)
{
	while (bus->parent) { /* NVMe: root bus에 도달할 때까지 상위로 이동 */
		if (acpi_pm_device_can_wakeup(&bus->self->dev)) /* NVMe: 현재 bridge가 wake 가능하면 */
			return acpi_pm_set_device_wakeup(&bus->self->dev, enable); /* NVMe: bridge wake 설정 */

		bus = bus->parent; /* NVMe: 부모 bus로 이동 */
	}

	/* We have reached the root bus. */
	if (bus->bridge) { /* NVMe: root bus에 bridge가 있으면 */
		if (acpi_pm_device_can_wakeup(bus->bridge)) /* NVMe: root bridge wake 가능하면 */
			return acpi_pm_set_device_wakeup(bus->bridge, enable); /* NVMe: root bridge wake 설정 */
	}
	return 0; /* NVMe: 전파 완료 */
}

/*
 * acpi_pci_wakeup:
 *   NVMe 디바이스의 ACPI wake 기능을 enable/disable한다. NVMe
 *   장치가 sleep 상태에서 시스템을 깨울 수 있도록 허용할 때 사용.
 */
int acpi_pci_wakeup(struct pci_dev *dev, bool enable)
{
	if (acpi_pci_disabled) /* NVMe: ACPI PCI가 비활성이면 */
		return 0; /* NVMe: 아무 것도 안 함 */

	if (acpi_pm_device_can_wakeup(&dev->dev)) /* NVMe: NVMe 디바이스 자체가 wake 가능하면 */
		return acpi_pm_set_device_wakeup(&dev->dev, enable); /* NVMe: 디바이스 wake 설정 */

	return acpi_pci_propagate_wakeup(dev->bus, enable); /* NVMe: 상위로 wake 전파 */
}

/*
 * acpi_pci_need_resume:
 *   NVMe 디바이스가 시스템 resume 시 반드시 resume해야 하는지
 *   ACPI 정보를 기반으로 판단한다. _PRW, _DSW 등 wake 설정에 따라
 *   달라진다.
 */
bool acpi_pci_need_resume(struct pci_dev *dev)
{
	struct acpi_device *adev; /* NVMe: NVMe의 ACPI companion */

	if (acpi_pci_disabled) /* NVMe: ACPI PCI가 비활성이면 */
		return false; /* NVMe: resume 필요 없음으로 처리 */

	/*
	 * In some cases (eg. Samsung 305V4A) leaving a bridge in suspend over
	 * system-wide suspend/resume confuses the platform firmware, so avoid
	 * doing that.  According to Section 16.1.6 of ACPI 6.2, endpoint
	 * devices are expected to be in D3 before invoking the S3 entry path
	 * from the firmware, so they should not be affected by this issue.
	 * NVMe: NVMe는 endpoint이므로 이 이슈에 영향받지 않음.
	 */
	if (pci_is_bridge(dev) && acpi_target_system_state() != ACPI_STATE_S0) /* NVMe: bridge이고 S0가 아니면 */
		return true; /* NVMe: bridge는 resume 필요 */

	adev = ACPI_COMPANION(&dev->dev); /* NVMe: ACPI companion 획득 */
	if (!adev || !acpi_device_power_manageable(adev)) /* NVMe: companion 없거나 전원 관리 불가면 */
		return false; /* NVMe: resume 불필요 */

	if (adev->wakeup.flags.valid &&
	    device_may_wakeup(&dev->dev) != !!adev->wakeup.prepare_count) /* NVMe: wake 설정과 prepare_count 불일치 시 */
		return true; /* NVMe: resume 필요 */

	if (acpi_target_system_state() == ACPI_STATE_S0) /* NVMe: S0(완전 활성)이면 */
		return false; /* NVMe: resume 불필요 */

	return !!adev->power.flags.dsw_present; /* NVMe: _DSW가 있으면 resume 필요 */
}

/*
 * acpi_pci_add_bus:
 *   PCI bus가 추가될 때 ACPI 측면의 초기화를 수행한다. NVMe가 속한
 *   bus의 slot enumeration, hotplug slot 등록, host bridge의 reset
 *   delay 최적화(_DSM func 8)를 처리한다.
 */
void acpi_pci_add_bus(struct pci_bus *bus)
{
	union acpi_object *obj; /* NVMe: _DSM 반환 객체 */
	struct pci_host_bridge *bridge; /* NVMe: 해당 bus의 host bridge */

	if (acpi_pci_disabled || !bus->bridge || !ACPI_HANDLE(bus->bridge)) /* NVMe: ACPI PCI 꺼져 있거나 핸들 없으면 */
		return; /* NVMe: ACPI 초기화 불필요 */

	acpi_pci_slot_enumerate(bus); /* NVMe: ACPI PCI slot 열거 */
	acpiphp_enumerate_slots(bus); /* NVMe: ACPI hotplug slot 등록 */

	/*
	 * For a host bridge, check its _DSM for function 8 and if
	 * that is available, mark it in pci_host_bridge.
	 * NVMe: host bridge _DSM func 8(reset delay) 조회.
	 */
	if (!pci_is_root_bus(bus)) /* NVMe: root bus가 아니면 */
		return; /* NVMe: host bridge _DSM 대상 아님 */

	obj = acpi_evaluate_dsm_typed(ACPI_HANDLE(bus->bridge), &pci_acpi_dsm_guid, 3,
				      DSM_PCI_POWER_ON_RESET_DELAY, NULL, ACPI_TYPE_INTEGER); /* NVMe: _DSM func 8 평가 */
	if (!obj) /* NVMe: _DSM func 8이 없으면 */
		return; /* NVMe: reset delay 최적화 불가 */

	if (obj->integer.value == 1) { /* NVMe: reset delay 무시 가능하면 */
		bridge = pci_find_host_bridge(bus); /* NVMe: host bridge 획득 */
		bridge->ignore_reset_delay = 1; /* NVMe: reset delay 무시 플래그 설정 */
	}
	ACPI_FREE(obj); /* NVMe: _DSM 반환 객체 해제 */
}

/*
 * acpi_pci_remove_bus:
 *   PCI bus가 제거될 때 ACPI hotplug/slot 리소스를 정리한다. NVMe
 *   장치가 제거되거나 bus가 사라질 때 호출.
 */
void acpi_pci_remove_bus(struct pci_bus *bus)
{
	if (acpi_pci_disabled || !bus->bridge) /* NVMe: ACPI PCI 꺼져 있거나 bridge 없으면 */
		return; /* NVMe: 정리 불필요 */

	acpiphp_remove_slots(bus); /* NVMe: ACPI hotplug slot 제거 */
	acpi_pci_slot_remove(bus); /* NVMe: ACPI PCI slot 제거 */
}

/* ACPI bus type */


static DECLARE_RWSEM(pci_acpi_companion_lookup_sem); /* NVMe: companion lookup hook 보호용 rwsem */
static struct acpi_device *(*pci_acpi_find_companion_hook)(struct pci_dev *); /* NVMe: 플랫폼별 companion lookup hook */

/**
 * pci_acpi_set_companion_lookup_hook - Set ACPI companion lookup callback.
 * @func: ACPI companion lookup callback pointer or NULL.
 *
 * Set a special ACPI companion lookup callback for PCI devices whose companion
 * objects in the ACPI namespace have _ADR with non-standard bus-device-function
 * encodings.
 *
 * Return 0 on success or a negative error code on failure (in which case no
 * changes are made).
 *
 * The caller is responsible for the appropriate ordering of the invocations of
 * this function with respect to the enumeration of the PCI devices needing the
 * callback installed by it.
 * NVMe: 특수 플랫폼에서 NVMe pci_dev의 ACPI companion을 찾는 custom hook을
 *       등록한다.
 */
int pci_acpi_set_companion_lookup_hook(struct acpi_device *(*func)(struct pci_dev *))
{
	int ret; /* NVMe: 반환 코드 */

	if (!func) /* NVMe: NULL hook이면 */
		return -EINVAL; /* NVMe: 잘못된 인자 */

	down_write(&pci_acpi_companion_lookup_sem); /* NVMe: 쓰기 락 획득 */

	if (pci_acpi_find_companion_hook) { /* NVMe: 이미 hook이 등록되어 있으면 */
		ret = -EBUSY; /* NVMe: 중복 등록 방지 */
	} else { /* NVMe: hook이 비어 있으면 */
		pci_acpi_find_companion_hook = func; /* NVMe: hook 등록 */
		ret = 0; /* NVMe: 등록 성공 */
	}

	up_write(&pci_acpi_companion_lookup_sem); /* NVMe: 쓰기 락 해제 */

	return ret; /* NVMe: 등록 결과 반환 */
}
EXPORT_SYMBOL_GPL(pci_acpi_set_companion_lookup_hook);

/**
 * pci_acpi_clear_companion_lookup_hook - Clear ACPI companion lookup callback.
 *
 * Clear the special ACPI companion lookup callback previously set by
 * pci_acpi_set_companion_lookup_hook().  Block until the last running instance
 * of the callback returns before clearing it.
 *
 * The caller is responsible for the appropriate ordering of the invocations of
 * this function with respect to the enumeration of the PCI devices needing the
 * callback cleared by it.
 * NVMe: custom companion lookup hook을 제거한다.
 */
void pci_acpi_clear_companion_lookup_hook(void)
{
	down_write(&pci_acpi_companion_lookup_sem); /* NVMe: 쓰기 락 획득 */

	pci_acpi_find_companion_hook = NULL; /* NVMe: hook 제거 */

	up_write(&pci_acpi_companion_lookup_sem); /* NVMe: 쓰기 락 해제 */
}
EXPORT_SYMBOL_GPL(pci_acpi_clear_companion_lookup_hook);

/*
 * acpi_pci_find_companion:
 *   NVMe pci_dev에 해당하는 ACPI companion device를 ACPI namespace에서
 *   찾는다. _ADR encoding을 기준으로 부모 아래의 child device를
 *   매칭한다. NVMe의 _DSD, _PRW, _SxW 등 ACPI 속성 접근의 전제조건.
 */
static struct acpi_device *acpi_pci_find_companion(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: generic device에서 pci_dev 변환 */
	struct acpi_device *adev; /* NVMe: 찾은 ACPI companion */
	bool check_children; /* NVMe: bridge 아래 child까지 검색 여부 */
	u64 addr; /* NVMe: _ADR 값(slot<<16 | function) */

	if (!dev->parent) /* NVMe: 부모 device가 없으면 */
		return NULL; /* NVMe: companion 검색 불가 */

	down_read(&pci_acpi_companion_lookup_sem); /* NVMe: 읽기 락 획득 */

	adev = pci_acpi_find_companion_hook ?
		pci_acpi_find_companion_hook(pci_dev) : NULL; /* NVMe: 플랫폼 hook이 있으면 우선 사용 */

	up_read(&pci_acpi_companion_lookup_sem); /* NVMe: 읽기 락 해제 */

	if (adev) /* NVMe: hook이 companion을 찾았으면 */
		return adev; /* NVMe: 해당 companion 반환 */

	check_children = pci_is_bridge(pci_dev); /* NVMe: bridge이면 하위도 검색 */
	/* Please ref to ACPI spec for the syntax of _ADR */
	addr = (PCI_SLOT(pci_dev->devfn) << 16) | PCI_FUNC(pci_dev->devfn); /* NVMe: _ADR = slot<<16 | func */
	adev = acpi_find_child_device(ACPI_COMPANION(dev->parent), addr,
				      check_children); /* NVMe: 부모 아래 _ADR 일치 child 검색 */

	/*
	 * There may be ACPI device objects in the ACPI namespace that are
	 * children of the device object representing the host bridge, but don't
	 * represent PCI devices.  Both _HID and _ADR may be present for them,
	 * even though that is against the specification (for example, see
	 * Section 6.1 of ACPI 6.3), but in many cases the _ADR returns 0 which
	 * appears to indicate that they should not be taken into consideration
	 * as potential companions of PCI devices on the root bus.
	 *
	 * To catch this special case, disregard the returned device object if
	 * it has a valid _HID, addr is 0 and the PCI device at hand is on the
	 * root bus.
	 * NVMe: root bus에서 _ADR 0이면서 _HID가 있는 가짜 companion을 무시.
	 */
	if (adev && adev->pnp.type.platform_id && !addr &&
	    pci_is_root_bus(pci_dev->bus)) /* NVMe: 위 가짜 companion 조건이면 */
		return NULL; /* NVMe: companion으로 간주하지 않음 */

	return adev; /* NVMe: 찾은 ACPI companion 반환(없으면 NULL) */
}

/**
 * pci_acpi_optimize_delay - optimize PCI D3 and D3cold delay from ACPI
 * @pdev: the PCI device whose delay is to be updated
 * @handle: ACPI handle of this device
 *
 * Update the d3hot_delay and d3cold_delay of a PCI device from the ACPI _DSM
 * control method of either the device itself or the PCI host bridge.
 *
 * Function 8, "Reset Delay," applies to the entire hierarchy below a PCI
 * host bridge.  If it returns one, the OS may assume that all devices in
 * the hierarchy have already completed power-on reset delays.
 *
 * Function 9, "Device Readiness Durations," applies only to the object
 * where it is located.  It returns delay durations required after various
 * events if the device requires less time than the spec requires.  Delays
 * from this function take precedence over the Reset Delay function.
 *
 * These _DSM functions are defined by the draft ECN of January 28, 2014,
 * titled "ACPI additions for FW latency optimizations."
 * NVMe: NVMe reset/resume 후 D3hot/D3cold 복귀 지연을 ACPI _DSM 기반으로
 *       최적화. NVMe 드라이버의 probe/reset 지연 시간에 직접 영향.
 */
static void pci_acpi_optimize_delay(struct pci_dev *pdev,
				    acpi_handle handle)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(pdev->bus); /* NVMe: NVMe bus의 host bridge */
	int value; /* NVMe: _DSM에서 읽은 지연값(마이크로초 -> 밀리초) */
	union acpi_object *obj, *elements; /* NVMe: _DSM 반환 객체 및 요소 배열 */

	if (bridge->ignore_reset_delay) /* NVMe: host bridge가 reset delay 무시 플래그 설정 시 */
		pdev->d3cold_delay = 0; /* NVMe: D3cold delay를 0으로 최적화 */

	obj = acpi_evaluate_dsm_typed(handle, &pci_acpi_dsm_guid, 3,
				      DSM_PCI_DEVICE_READINESS_DURATIONS, NULL,
				      ACPI_TYPE_PACKAGE); /* NVMe: _DSM func 9 평가 */
	if (!obj) /* NVMe: _DSM func 9가 없으면 */
		return; /* NVMe: 추가 최적화 불가 */

	if (obj->package.count == 5) { /* NVMe: 반환값이 5개 요소 package면 */
		elements = obj->package.elements; /* NVMe: 요소 배열 */
		if (elements[0].type == ACPI_TYPE_INTEGER) { /* NVMe: 첫 번째 요소가 D3cold readiness duration이면 */
			value = (int)elements[0].integer.value / 1000; /* NVMe: 마이크로초 -> 밀리초 변환 */
			if (value < PCI_PM_D3COLD_WAIT) /* NVMe: spec 기본값보다 짧으면 */
				pdev->d3cold_delay = value; /* NVMe: D3cold delay 최적화 */
		}
		if (elements[3].type == ACPI_TYPE_INTEGER) { /* NVMe: 네 번째 요소가 D3hot readiness duration이면 */
			value = (int)elements[3].integer.value / 1000; /* NVMe: 마이크로초 -> 밀리초 변환 */
			if (value < PCI_PM_D3HOT_WAIT) /* NVMe: spec 기본값보다 짧으면 */
				pdev->d3hot_delay = value; /* NVMe: D3hot delay 최적화 */
		}
	}
	ACPI_FREE(obj); /* NVMe: _DSM 반환 객체 해제 */
}

/*
 * pci_acpi_set_external_facing:
 *   Root Port의 _DSD "ExternalFacingPort" 속성을 읽어 external_facing
 *   플래그를 설정한다. NVMe가 외부 PCIe 케이지/확장 슬롯에 연결된 경우
 *   DMA 보안 정책(IOMMU, ATS)에 영향을 줄 수 있다.
 */
static void pci_acpi_set_external_facing(struct pci_dev *dev)
{
	u8 val; /* NVMe: ExternalFacingPort 속성값 */

	if (pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT) /* NVMe: Root Port가 아니면 */
		return; /* NVMe: external facing은 Root Port에만 적용 */
	if (device_property_read_u8(&dev->dev, "ExternalFacingPort", &val)) /* NVMe: _DSD 속성 읽기 */
		return; /* NVMe: 속성이 없으면 리턴 */

	/*
	 * These root ports expose PCIe (including DMA) outside of the
	 * system.  Everything downstream from them is external.
	 * NVMe: 이 Root Port 아래의 NVMe는 외부 접근 가능.
	 */
	if (val) /* NVMe: 속성값이 0이 아니면 */
		dev->external_facing = 1; /* NVMe: external_facing 플래그 설정 */
}

/*
 * pci_acpi_setup:
 *   NVMe pci_dev가 ACPI companion과 연결된 후 호출되는 통합 설정.
 *   delay 최적화, external facing, EDR notifier, PM notifier, wake
 *   설정을 수행한다. NVMe probe 초기화의 핵심 ACPI 진입점.
 */
void pci_acpi_setup(struct device *dev, struct acpi_device *adev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: generic device에서 pci_dev 변환 */

	pci_acpi_optimize_delay(pci_dev, adev->handle); /* NVMe: D3 delay 최적화 */
	pci_acpi_set_external_facing(pci_dev); /* NVMe: external facing 플래그 설정 */
	pci_acpi_add_edr_notifier(pci_dev); /* NVMe: ACPI EDR(error device removal) notifier 등록 */

	pci_acpi_add_pm_notifier(adev, pci_dev); /* NVMe: NVMe 디바이스 PM notifier 등록 */
	if (!adev->wakeup.flags.valid) /* NVMe: wake 설정이 유효하지 않으면 */
		return; /* NVMe: 이후 wake 설정 스킵 */

	device_set_wakeup_capable(dev, true); /* NVMe: 디바이스를 wake capable로 표시 */
	/*
	 * For bridges that can do D3 we enable wake automatically (as
	 * we do for the power management itself in that case). The
	 * reason is that the bridge may have additional methods such as
	 * _DSW that need to be called.
	 * NVMe: D3 가능 bridge는 wake 자동 활성(_DSW 등 메서드 호출 필요).
	 */
	if (pci_dev->bridge_d3) /* NVMe: bridge가 D3 가능하면 */
		device_wakeup_enable(dev); /* NVMe: wake 활성화 */

	acpi_pci_wakeup(pci_dev, false); /* NVMe: wake 비활성화 상태로 초기화 */
	acpi_device_power_add_dependent(adev, dev); /* NVMe: ACPI 전원 종속성 추가 */

	if (pci_is_bridge(pci_dev)) /* NVMe: 대상이 bridge이면 */
		acpi_dev_power_up_children_with_adr(adev); /* NVMe: _ADR child device 전원 관리 설정 */
}

/*
 * pci_acpi_cleanup:
 *   pci_acpi_setup()에서 등록한 ACPI notifier와 wake 설정을 제거한다.
 *   NVMe 디바이스 제거 시 호출.
 */
void pci_acpi_cleanup(struct device *dev, struct acpi_device *adev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: generic device에서 pci_dev 변환 */

	pci_acpi_remove_edr_notifier(pci_dev); /* NVMe: EDR notifier 제거 */
	pci_acpi_remove_pm_notifier(adev); /* NVMe: PM notifier 제거 */
	if (adev->wakeup.flags.valid) { /* NVMe: wake 설정이 유효했으면 */
		acpi_device_power_remove_dependent(adev, dev); /* NVMe: ACPI 전원 종속성 제거 */
		if (pci_dev->bridge_d3) /* NVMe: bridge D3였으면 */
			device_wakeup_disable(dev); /* NVMe: wake 비활성화 */

		device_set_wakeup_capable(dev, false); /* NVMe: wake capable 해제 */
	}
}

static struct fwnode_handle *(*pci_msi_get_fwnode_cb)(struct device *dev); /* NVMe: MSI fwnode 제공 콜백 */

/**
 * pci_msi_register_fwnode_provider - Register callback to retrieve fwnode
 * @fn:       Callback matching a device to a fwnode that identifies a PCI
 *            MSI domain.
 *
 * This should be called by irqchip driver, which is the parent of
 * the MSI domain to provide callback interface to query fwnode.
 * NVMe: ARM64 등에서 NVMe MSI-X vector 할당에 사용할 irq_domain의 fwnode를
 *       제공하는 callback을 등록.
 */
void
pci_msi_register_fwnode_provider(struct fwnode_handle *(*fn)(struct device *))
{
	pci_msi_get_fwnode_cb = fn; /* NVMe: MSI fwnode 콜백 등록 */
}

/**
 * pci_host_bridge_acpi_msi_domain - Retrieve MSI domain of a PCI host bridge
 * @bus:      The PCI host bridge bus.
 *
 * This function uses the callback function registered by
 * pci_msi_register_fwnode_provider() to retrieve the irq_domain with
 * type DOMAIN_BUS_PCI_MSI of the specified host bridge bus.
 * This returns NULL on error or when the domain is not found.
 * NVMe: NVMe가 연결된 host bridge의 MSI irq_domain을 조회한다.
 *       pci_alloc_irq_vectors() -> msi_device_domain_get() -> 본 함수로
 *       NVMe MSI-X vector 할당에 필요한 irq_domain을 얻는다.
 */
struct irq_domain *pci_host_bridge_acpi_msi_domain(struct pci_bus *bus)
{
	struct fwnode_handle *fwnode; /* NVMe: MSI domain을 식별하는 firmware node */

	if (!pci_msi_get_fwnode_cb) /* NVMe: fwnode 제공 콜백이 등록되지 않았으면 */
		return NULL; /* NVMe: MSI domain 조회 불가 */

	fwnode = pci_msi_get_fwnode_cb(&bus->dev); /* NVMe: bus device에 대한 fwnode 획득 시도 */
	if (!fwnode) /* NVMe: fwnode를 얻지 못하면 */
		return NULL; /* NVMe: domain 조회 불가 */

	return irq_find_matching_fwnode(fwnode, DOMAIN_BUS_PCI_MSI); /* NVMe: PCI MSI 타입 irq_domain 검색 및 반환 */
}

/*
 * acpi_pci_init:
 *   ACPI-PCI 서브시스템 초기화. FADT의 NO_MSI/NO_ASPM 플래그를 해석해
 *   전역적으로 MSI와 ASPM을 끈다. NVMe는 MSI-X 기반 큐를 사용하므로
 *   NO_MSI가 설정되면 INT#x로 fallback되며, NO_ASPM이 설정되면 NVMe
 *   링크의 절전 상태가 비활성화된다.
 */
static int __init acpi_pci_init(void)
{
	if (acpi_gbl_FADT.boot_flags & ACPI_FADT_NO_MSI) { /* NVMe: FADT가 MSI 미지원 선언 시 */
		pr_info("ACPI FADT declares the system doesn't support MSI, so disable it\n"); /* NVMe: 정보 출력 */
		pci_no_msi(); /* NVMe: 전역 MSI 비활성화 -> NVMe는 legacy INT#x 사용 */
	}

	if (acpi_gbl_FADT.boot_flags & ACPI_FADT_NO_ASPM) { /* NVMe: FADT가 ASPM 미지원 선언 시 */
		pr_info("ACPI FADT declares the system doesn't support PCIe ASPM, so disable it\n"); /* NVMe: 정보 출력 */
		pcie_no_aspm(); /* NVMe: PCIe ASPM 비활성화 -> NVMe 링크 절전 불가 */
	}

	if (acpi_pci_disabled) /* NVMe: ACPI PCI가 완전히 비활성이면 */
		return 0; /* NVMe: 이후 ACPI PCI 초기화 스킵 */

	acpi_pci_slot_init(); /* NVMe: ACPI PCI slot 초기화 */
	acpiphp_init(); /* NVMe: ACPI PCI hotplug 초기화 */

	return 0; /* NVMe: 초기화 성공 */
}
arch_initcall(acpi_pci_init); /* NVMe: 아키텍처 초기화 시 acpi_pci_init 등록 */

#if defined(CONFIG_ARM64) || defined(CONFIG_RISCV)

/*
 * pcibios_alloc_irq:
 *   ARM64/RISC-V에서 새 PCI 디바이스 probe 시 ACPI _PRT를 기반으로
 *   NVMe의 INT#x/MSI IRQ를 할당한다. x86의 PCI IRQ 라우팅과 대응.
 */
int pcibios_alloc_irq(struct pci_dev *dev)
{
	if (!acpi_disabled) /* NVMe: ACPI가 활성이면 */
		acpi_pci_irq_enable(dev); /* NVMe: ACPI _PRT에서 NVMe IRQ 할당 */

	return 0; /* NVMe: 항상 성공(할당 실패는 dev->irq=0 등으로 표현) */
}

struct acpi_pci_generic_root_info {
	struct acpi_pci_root_info	common;
	struct pci_config_window	*cfg;	/* NVMe: ECAM config space 매핑 윈도우 */
};

/*
 * acpi_pci_bus_find_domain_nr:
 *   NVMe가 속한 PCI bus의 segment(domain) 번호를 ACPI root 정보에서
 *   반환한다. 멀티 세그먼트 시스템에서 NVMe 장치의 domain 식별에
 *   사용된다.
 */
int acpi_pci_bus_find_domain_nr(struct pci_bus *bus)
{
	struct pci_config_window *cfg = bus->sysdata; /* NVMe: bus의 config window */
	struct acpi_device *adev = to_acpi_device(cfg->parent); /* NVMe: config window의 ACPI parent */
	struct acpi_pci_root *root = acpi_driver_data(adev); /* NVMe: ACPI root driver data */

	return root->segment; /* NVMe: PCI segment(domain) 번호 반환 */
}

/*
 * pcibios_root_bridge_prepare:
 *   ACPI root bridge가 생성되기 전에 ACPI companion과 NUMA node를
 *   설정한다. NVMe가 연결될 root bridge의 ACPI 바인딩 준비.
 */
int pcibios_root_bridge_prepare(struct pci_host_bridge *bridge)
{
	struct pci_config_window *cfg; /* NVMe: ECAM config window */
	struct acpi_device *adev;      /* NVMe: root bridge ACPI companion */
	struct device *bus_dev;        /* NVMe: PCI bus device */

	if (acpi_disabled) /* NVMe: ACPI가 비활성이면 */
		return 0; /* NVMe: ACPI 설정 불필요 */

	cfg = bridge->bus->sysdata; /* NVMe: bus sysdata에서 config window 획득 */

	/*
	 * On Hyper-V there is no corresponding ACPI device for a root bridge,
	 * therefore ->parent is set as NULL by the driver. And set 'adev' as
	 * NULL in this case because there is no proper ACPI device.
	 * NVMe: Hyper-V 등 가상화 환경에서는 ACPI companion이 없을 수 있음.
	 */
	if (!cfg->parent) /* NVMe: Hyper-V 등 parent가 NULL이면 */
		adev = NULL; /* NVMe: ACPI companion 없음 */
	else /* NVMe: 일반 ACPI 환경이면 */
		adev = to_acpi_device(cfg->parent); /* NVMe: ACPI companion 획득 */

	bus_dev = &bridge->bus->dev; /* NVMe: bus device 획득 */

	ACPI_COMPANION_SET(&bridge->dev, adev); /* NVMe: root bridge에 ACPI companion 설정 */
	set_dev_node(bus_dev, acpi_get_node(acpi_device_handle(adev))); /* NVMe: bus device NUMA node 설정 */

	return 0; /* NVMe: 준비 완료 */
}

/*
 * pci_acpi_root_prepare_resources:
 *   ACPI root bridge의 리소스(_CRS)를 probe하고 window만 남긴다.
 *   NVMe BAR가 할당될 PCI memory/IO window가 여기서 결정된다.
 */
static int pci_acpi_root_prepare_resources(struct acpi_pci_root_info *ci)
{
	struct resource_entry *entry, *tmp; /* NVMe: 리소스 엔트리 순회용 */
	int status; /* NVMe: 리소스 probe 상태 */

	status = acpi_pci_probe_root_resources(ci); /* NVMe: ACPI root 리소스 probe */
	resource_list_for_each_entry_safe(entry, tmp, &ci->resources) { /* NVMe: probe된 리소스 순회 */
		if (!(entry->res->flags & IORESOURCE_WINDOW)) /* NVMe: window가 아닌 고정 리소스면 */
			resource_list_destroy_entry(entry); /* NVMe: 리스트에서 제거 */
	}
	return status; /* NVMe: probe 상태 반환 */
}

/*
 * pci_acpi_setup_ecam_mapping:
 *   MCFG 테이블에서 root bridge의 ECAM 영역을 찾아 PCI config space
 *   매핑을 생성한다. NVMe의 BAR, capability, MSI-X table 등을 읽으려면
 *   이 config space 접근이 필수적이다.
 */
static struct pci_config_window *
pci_acpi_setup_ecam_mapping(struct acpi_pci_root *root)
{
	struct device *dev = &root->device->dev; /* NVMe: root bridge device */
	struct resource *bus_res = &root->secondary; /* NVMe: secondary bus 리소스 */
	u16 seg = root->segment; /* NVMe: PCI segment 번호 */
	const struct pci_ecam_ops *ecam_ops; /* NVMe: ECAM 운영 ops 포인터 */
	struct resource cfgres; /* NVMe: MCFG에서 찾은 ECAM 리소스 */
	struct acpi_device *adev; /* NVMe: ECAM 영역을 예약한 ACPI 장치 */
	struct pci_config_window *cfg; /* NVMe: 생성된 config window */
	int ret; /* NVMe: 반환 코드 */

	ret = pci_mcfg_lookup(root, &cfgres, &ecam_ops); /* NVMe: MCFG에서 segment에 해당하는 ECAM 조회 */
	if (ret) { /* NVMe: MCFG 조회 실패 시 */
		dev_err(dev, "%04x:%pR ECAM region not found\n", seg, bus_res); /* NVMe: 오류 출력 */
		return NULL; /* NVMe: config space 매핑 실패 */
	}

	adev = acpi_resource_consumer(&cfgres); /* NVMe: ECAM 리소스의 ACPI consumer 조회 */
	if (adev) /* NVMe: consumer가 있으면 */
		dev_info(dev, "ECAM area %pR reserved by %s\n", &cfgres,
			 dev_name(&adev->dev)); /* NVMe: 예약 정보 출력 */
	else /* NVMe: consumer가 없으면 */
		dev_warn(dev, FW_BUG "ECAM area %pR not reserved in ACPI namespace\n",
			 &cfgres); /* NVMe: ACPI namespace 미예약 경고 */

	cfg = pci_ecam_create(dev, &cfgres, bus_res, ecam_ops); /* NVMe: ECAM 매핑 생성 */
	if (IS_ERR(cfg)) { /* NVMe: 매핑 생성 실패 시 */
		dev_err(dev, "%04x:%pR error %ld mapping ECAM\n", seg, bus_res,
			PTR_ERR(cfg)); /* NVMe: 오류 출력 */
		return NULL; /* NVMe: 매핑 실패 반환 */
	}

	return cfg; /* NVMe: ECAM config window 반환 */
}

/* release_info: free resources allocated by init_info */
/* NVMe: root bridge 제거 시 ECAM 매핑과 동적 할당 메모리 해제 */
static void pci_acpi_generic_release_info(struct acpi_pci_root_info *ci)
{
	struct acpi_pci_generic_root_info *ri; /* NVMe: generic root info 구조체 */

	ri = container_of(ci, struct acpi_pci_generic_root_info, common); /* NVMe: common에서 ri 추출 */
	pci_ecam_free(ri->cfg); /* NVMe: ECAM config window 해제 */
	kfree(ci->ops); /* NVMe: root ops 메모리 해제 */
	kfree(ri); /* NVMe: ri 메모리 해제 */
}

/* Interface called from ACPI code to setup PCI host controller */
/*
 * pci_acpi_scan_root:
 *   ACPI PCI root bridge를 scan하여 PCI bus 트리를 생성한다. NVMe
 *   SSD가 연결될 root bus, Root Port, 그리고 NVMe endpoint를枚举하는
 *   시작점. ECAM 매핑 -> root bus 생성 -> 리소스 할당 -> PCIe
 *   설정 순으로 진행.
 */
struct pci_bus *pci_acpi_scan_root(struct acpi_pci_root *root)
{
	struct acpi_pci_generic_root_info *ri; /* NVMe: generic root bridge 정보 */
	struct pci_bus *bus, *child; /* NVMe: 생성된 root bus 및 하위 bus */
	struct acpi_pci_root_ops *root_ops; /* NVMe: root bridge ops */
	struct pci_host_bridge *host; /* NVMe: host bridge 메타정보 */

	ri = kzalloc_obj(*ri); /* NVMe: root bridge 정보 메모리 할당 */
	if (!ri) /* NVMe: 할당 실패 시 */
		return NULL; /* NVMe: root bus 생성 실패 */

	root_ops = kzalloc_obj(*root_ops); /* NVMe: root ops 메모리 할당 */
	if (!root_ops) { /* NVMe: 할당 실패 시 */
		kfree(ri); /* NVMe: 이미 할당한 ri 해제 */
		return NULL; /* NVMe: root bus 생성 실패 */
	}

	ri->cfg = pci_acpi_setup_ecam_mapping(root); /* NVMe: ECAM config space 매핑 설정 */
	if (!ri->cfg) { /* NVMe: ECAM 매핑 실패 시 */
		kfree(ri); /* NVMe: ri 해제 */
		kfree(root_ops); /* NVMe: root_ops 해제 */
		return NULL; /* NVMe: root bus 생성 실패 */
	}

	root_ops->release_info = pci_acpi_generic_release_info; /* NVMe: release callback 설정 */
	root_ops->prepare_resources = pci_acpi_root_prepare_resources; /* NVMe: 리소스 준비 callback 설정 */
	root_ops->pci_ops = (struct pci_ops *)&ri->cfg->ops->pci_ops; /* NVMe: ECAM read/write ops 설정 */
	bus = acpi_pci_root_create(root, root_ops, &ri->common, ri->cfg); /* NVMe: ACPI root bus 생성 */
	if (!bus) /* NVMe: bus 생성 실패 시 */
		return NULL; /* NVMe: root bus 없음 */

	/* If we must preserve the resource configuration, claim now */
	host = pci_find_host_bridge(bus); /* NVMe: 생성된 bus의 host bridge 획득 */
	if (host->preserve_config) /* NVMe: firmware 설정 보존 필요 시 */
		pci_bus_claim_resources(bus); /* NVMe: 기존 리소스를 미리 claim */

	/*
	 * Assign whatever was left unassigned. If we didn't claim above,
	 * this will reassign everything.
	 * NVMe: 할당되지 않은 BAR 등 리소스를 재할당. NVMe BAR0 포함.
	 */
	pci_assign_unassigned_root_bus_resources(bus); /* NVMe: 미할당 리소스 할당 */

	list_for_each_entry(child, &bus->children, node) /* NVMe: root bus의 각 하위 bus에 대해 */
		pcie_bus_configure_settings(child); /* NVMe: PCIe MPS, ASPM 등 버스 설정 적용 */

	return bus; /* NVMe: 생성된 root bus 반환 */
}

/*
 * pcibios_add_bus:
 *   ARM64/RISC-V에서 PCI bus 추가 시 ACPI bus 등록을 위한 wrapper.
 *   NVMe bus가 ACPI namespace에 추가될 때 호출.
 */
void pcibios_add_bus(struct pci_bus *bus)
{
	acpi_pci_add_bus(bus); /* NVMe: ACPI bus 추가 처리 */
}

/*
 * pcibios_remove_bus:
 *   ARM64/RISC-V에서 PCI bus 제거 시 ACPI bus 정리를 위한 wrapper.
 *   NVMe bus 제거 시 호출.
 */
void pcibios_remove_bus(struct pci_bus *bus)
{
	acpi_pci_remove_bus(bus); /* NVMe: ACPI bus 제거 처리 */
}

#endif
