// SPDX-License-Identifier: GPL-2.0
/*
 * PCI support in ACPI
 *
 * Copyright (C) 2005 David Shaohua Li <shaohua.li@intel.com>
 * Copyright (C) 2004 Tom Long Nguyen <tom.l.nguyen@intel.com>
 * Copyright (C) 2004 Intel Corp.
 */

/*
 * [한국어 설명] PCI 와 ACPI 펌웨어를 잇는 계층 (pci-acpi.c)
 *
 * === 파일의 역할 ===
 * ACPI 는 펌웨어가 OS 에게 하드웨어를 기술하는 방식이다. PCI 장치에 대해
 * 펌웨어만 아는 정보가 여럿 있고, 이 파일이 그것을 커널로 가져온다.
 *
 * 크게 네 갈래다.
 *
 *   1) 소유권 협상(_OSC) — 이 파일에서 가장 중요한 부분이다.
 *      펌웨어와 커널이 "PCIe 의 어떤 기능을 누가 관리할 것인가" 를 정한다.
 *      AER, 핫플러그, ASPM, DPC, LTR 같은 기능마다 소유권이 갈리며,
 *      펌웨어가 넘겨주지 않으면 커널의 해당 드라이버가 아예 붙지 않는다.
 *      pcie_aer_is_native() 나 DPC 소유권 판정이 그 결과를 본다.
 *
 *   2) 전원 관리 — ACPI 의 전원 상태와 PCI 의 D-state 를 잇는다.
 *      _PR3 가 있어야 D3cold 로 갈 수 있고(pci.c 의 pci_pr3_present 참고),
 *      _PS0/_PS3 메서드로 실제 전환이 이뤄진다. wakeup 소스 설정(_PRW)도
 *      여기서 다룬다.
 *
 *   3) 열거 보조 — MCFG 테이블로 ECAM 창을 만들고(ARM64/RISC-V),
 *      _CRS 로 루트 브리지의 주소 범위를 알아내고, _PRT 로 INTx 라우팅을
 *      해석한다. x86 은 상당 부분을 아키텍처 코드가 하고, ARM64 는
 *      이 파일에 더 많이 의존한다.
 *
 *   4) 슬롯과 hotplug 보조 — _SUN(슬롯 번호), _DSM(장치별 메서드),
 *      _HPX(권장 설정값), 그리고 hotplug 알림 처리.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅:  ACPI 코어가 루트 브리지 장치를 발견
 *          -> [이 파일] acpi_pci_root_add() 계열
 *             -> negotiate_os_control() 로 _OSC 협상
 *             -> pci_acpi_setup_ecam_mapping() 으로 ECAM 창 생성(ARM64/RISC-V)
 *             -> pci_create_root_bus() [probe.c] 로 버스 트리 시작
 *
 * 장치별: probe.c 가 장치를 설정할 때
 *          -> [이 파일] pci_acpi_program_hp_params() — _HPX 권장값 적용
 *          -> [이 파일] pcibios_alloc_irq() — _PRT 로 INTx 배정(ARM64/RISC-V)
 *
 * 전원:  pci.c 의 전원 상태 전환
 *          -> [이 파일] acpi_pci_set_power_state(), acpi_pci_get_power_state()
 *             -> ACPI 의 _PS0/_PS3 메서드 평가
 *
 * 실행 컨텍스트: 대부분 프로세스 컨텍스트. ACPI 메서드 평가는 인터프리터를
 * 돌리므로 시간이 걸리고 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/acpi/ 의 ACPI 코어, probe.c(열거), pci.c(전원 관리).
 * 아래쪽: ecam.c 의 pci_ecam_create, access.c 의 config 접근.
 * 옆쪽: pcie/portdrv.c 와 각 서비스 드라이버 — _OSC 협상 결과를 보고
 *   자기가 붙을지 말지 정한다. pcie/edr.c 는 아예 ACPI 알림으로만 동작한다.
 * 공유 상태: struct pci_host_bridge 의 native_* 비트들(각 기능의 소유권),
 *   struct pci_dev 의 ACPI companion 연결.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 * 하지만 이 파일이 정하는 두 가지가 NVMe 의 동작을 크게 좌우한다.
 *
 * 첫째, _OSC 협상 결과다. 펌웨어가 AER 소유권을 넘겨주지 않으면
 * pcie/aer.c 가 붙지 않고, NVMe 의 PCIe 오류가 커널에 보고되지 않는다.
 * 핫플러그 소유권도 마찬가지라, 넘겨받지 못하면 U.2 백플레인의
 * 드라이브 교체를 커널이 감지하지 못한다.
 *
 * 둘째, 전원 관리 정책이다. NVMe 는 절전 방식을 고를 때 펌웨어의 판단을
 * 참고한다:
 *
 *   nvme_pci_alloc_dev()  [drivers/nvme/host/pci.c]
 *     if (!noacpi && !(quirks & NVME_QUIRK_FORCE_NO_SIMPLE_SUSPEND) &&
 *         acpi_storage_d3(&pdev->dev)) {
 *             dev_info(..., "platform quirk: setting simple suspend\n");
 *     }
 *
 *   nvme_suspend()
 *     if (pm_suspend_via_firmware() || !ctrl->npss || ...)
 *             return nvme_disable_prepare_reset(ndev, true);
 *
 * acpi_storage_d3() 는 ACPI 의 StorageD3Enable 속성을 읽어 "이 플랫폼은
 * 스토리지를 D3 로 내려야 제대로 절전된다" 는 펌웨어의 지시를 확인한다.
 * 그 지시가 있으면 NVMe 는 자기 전력 상태 대신 PCI D3 를 쓴다.
 * 일부 노트북에서 이것이 없으면 절전이 되지 않아 배터리가 빨리 닳는다.
 *
 * 다만 acpi_storage_d3() 자체는 이 파일이 아니라 drivers/acpi/ 에 있다.
 * 이 파일은 그 판단의 토대가 되는 ACPI-PCI 연결을 만들 뿐이다.
 *
 * (기존 주석은 이 파일이 "MSI/MSI-X IRQ 도메인, ASPM, config space 접근" 을
 *  제어한다고 적었으나, MSI 도메인은 msi/irqdomain.c 가, ASPM 은
 *  pcie/aspm.c 가, config 접근은 access.c 가 담당한다. 이 파일은 그
 *  기능들의 *소유권* 을 펌웨어와 협상할 뿐이다.)
 *
 * === 주요 함수/구조체 요약 ===
 * negotiate_os_control()        : _OSC 로 기능별 소유권을 협상한다.
 *                                 이 파일의 가장 중요한 함수다.
 * acpi_pci_osc_control_set()    : 협상의 실제 메서드 호출.
 * pci_acpi_program_hp_params()  : _HPX/_HPP 권장값을 장치에 적용한다.
 * acpi_pci_set_power_state()    : _PS0/_PS3 로 전원 상태를 바꾼다.
 * acpi_pci_get_power_state()    : 현재 상태를 조회한다.
 * acpi_pci_power_manageable()   : ACPI 로 전원을 관리할 수 있는 장치인가.
 * acpi_pci_wakeup()             : wakeup 소스로 설정하거나 해제한다.
 * pci_acpi_setup_ecam_mapping() : MCFG 로 ECAM 창을 만든다(ARM64/RISC-V).
 * pcibios_alloc_irq()           : _PRT 로 INTx IRQ 를 배정한다(ARM64/RISC-V).
 * pci_acpi_scan_root()          : 루트 브리지 하나를 열거한다.
 * acpi_pci_find_companion()     : PCI 장치에 대응하는 ACPI 노드를 찾는다.
 * pci_dev_acpi_reset()          : _RST 메서드로 장치를 리셋한다.
 *                                 pci_reset_fn_methods[] 의 "acpi" 항목이다.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/iommu.h>
#include <linux/irqdomain.h>
#include <linux/pci.h>
#include <linux/msi.h>
#include <linux/pci_hotplug.h>
#include <linux/module.h>
#include <linux/pci-acpi.h>
#include <linux/pci-ecam.h>
#include <linux/pm_runtime.h>
#include <linux/pm_qos.h>
#include <linux/rwsem.h>
#include "pci.h"

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
		  0x91, 0x17, 0xea, 0x4d, 0x19, 0xc3, 0x43, 0x4d);

#if defined(CONFIG_PCI_QUIRKS) && defined(CONFIG_ARM64)
/*
 * acpi_get_rc_addr:
 *   ACPI _CRS(Current Resource Settings)에서 Root Complex의 메모리
 *   리소스를 파싱한다. NVMe BAR(DMA/MMIO window)가 속한 root bridge
 *   window를 결정하는 데 쓰인다.
 */
static int acpi_get_rc_addr(struct acpi_device *adev, struct resource *res)
{
	struct device *dev = &adev->dev;
	struct resource_entry *entry;
	struct list_head list;
	unsigned long flags;
	int ret;

	INIT_LIST_HEAD(&list);
	flags = IORESOURCE_MEM;
	ret = acpi_dev_get_resources(adev, &list,
				     acpi_dev_filter_resource_type_cb,
				     (void *) flags);
	if (ret < 0) {
		dev_err(dev, "failed to parse _CRS method, error code %d\n",
			ret);
		return ret;
	}

	if (ret == 0) {
		dev_err(dev, "no IO and memory resources present in _CRS\n");
		return -EINVAL;
	}

	entry = list_first_entry(&list, struct resource_entry, node);
	*res = *entry->res;
	acpi_dev_free_resource_list(&list);
	return 0;
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
	u16 *segment = context;
	unsigned long long uid;
	acpi_status status;

	status = acpi_evaluate_integer(handle, METHOD_NAME__UID, NULL, &uid);
	if (ACPI_FAILURE(status) || uid != *segment)
		return AE_CTRL_DEPTH;

	*(acpi_handle *)retval = handle;
	return AE_CTRL_TERMINATE;
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
	struct acpi_device *adev;
	acpi_status status;
	acpi_handle handle;
	int ret;

	status = acpi_get_devices(hid, acpi_match_rc, &segment, &handle);
	if (ACPI_FAILURE(status)) {
		dev_err(dev, "can't find _HID %s device to locate resources\n",
			hid);
		return -ENODEV;
	}

	adev = acpi_fetch_acpi_dev(handle);
	if (!adev)
		return -ENODEV;

	ret = acpi_get_rc_addr(adev, res);
	if (ret) {
		dev_err(dev, "can't get resource from %s\n",
			dev_name(&adev->dev));
		return ret;
	}

	return 0;
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
	acpi_status status = AE_NOT_EXIST;
	unsigned long long mcfg_addr;

	if (handle)
		status = acpi_evaluate_integer(handle, METHOD_NAME__CBA,
					       NULL, &mcfg_addr);
	if (ACPI_FAILURE(status))
		return 0;

	return (phys_addr_t)mcfg_addr;
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
	bool ret = false;

	if (ACPI_HANDLE(&host_bridge->dev)) {
		union acpi_object *obj;

		/*
		 * Evaluate the "PCI Boot Configuration" _DSM Function.  If it
		 * exists and returns 0, we must preserve any PCI resource
		 * assignments made by firmware for this host bridge.
		 * NVMe: firmware가 할당한 NVMe BAR/resource를 OS가 덮어쓰지 않음.
		 */
		obj = acpi_evaluate_dsm_typed(ACPI_HANDLE(&host_bridge->dev),
					      &pci_acpi_dsm_guid,
					      1, DSM_PCI_PRESERVE_BOOT_CONFIG,
					      NULL, ACPI_TYPE_INTEGER);
		if (obj && obj->integer.value == 0)
			ret = true;
		ACPI_FREE(obj);
	}

	return ret;
}

/* _HPX PCI Setting Record (Type 0); same as _HPP */
struct hpx_type0 {
	u32 revision;		/* Not present in _HPP */
	u8  cache_line_size;	/* Not applicable to PCIe */
	u8  latency_timer;	/* Not applicable to PCIe */
	u8  enable_serr;
	u8  enable_perr;
};

static struct hpx_type0 pci_default_type0 = {
	.revision = 1,
	.cache_line_size = 8,
	.latency_timer = 0x40,
	.enable_serr = 0,
	.enable_perr = 0,
};

/*
 * program_hpx_type0:
 *   _HPX/_HPP Type 0 레코드를 NVMe/PCI 디바이스의 PCI config space에
 *   기록한다. NVMe의 PCI_COMMAND, CACHE_LINE_SIZE 등이 여기서 설정될
 *   수 있다.
 */
static void program_hpx_type0(struct pci_dev *dev, struct hpx_type0 *hpx)
{
	u16 pci_cmd, pci_bctl;

	if (!hpx)
		hpx = &pci_default_type0;

	if (hpx->revision > 1) {
		pci_warn(dev, "PCI settings rev %d not supported; using defaults\n",
			 hpx->revision);
		hpx = &pci_default_type0;
	}

	pci_write_config_byte(dev, PCI_CACHE_LINE_SIZE, hpx->cache_line_size);
	pci_write_config_byte(dev, PCI_LATENCY_TIMER, hpx->latency_timer);
	pci_read_config_word(dev, PCI_COMMAND, &pci_cmd);
	if (hpx->enable_serr)
		pci_cmd |= PCI_COMMAND_SERR;
	if (hpx->enable_perr)
		pci_cmd |= PCI_COMMAND_PARITY;
	pci_write_config_word(dev, PCI_COMMAND, pci_cmd);

	/* Program bridge control value */
	if ((dev->class >> 8) == PCI_CLASS_BRIDGE_PCI) {
		pci_write_config_byte(dev, PCI_SEC_LATENCY_TIMER,
				      hpx->latency_timer);
		pci_read_config_word(dev, PCI_BRIDGE_CONTROL, &pci_bctl);
		if (hpx->enable_perr)
			pci_bctl |= PCI_BRIDGE_CTL_PARITY;
		pci_write_config_word(dev, PCI_BRIDGE_CONTROL, pci_bctl);
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
	int i;
	union acpi_object *fields = record->package.elements;
	u32 revision = fields[1].integer.value;

	switch (revision) {
	case 1:
		if (record->package.count != 6)
			return AE_ERROR;
		for (i = 2; i < 6; i++)
			if (fields[i].type != ACPI_TYPE_INTEGER)
				return AE_ERROR;
		hpx0->revision        = revision;
		hpx0->cache_line_size = fields[2].integer.value;
		hpx0->latency_timer   = fields[3].integer.value;
		hpx0->enable_serr     = fields[4].integer.value;
		hpx0->enable_perr     = fields[5].integer.value;
		break;
	default:
		pr_warn("%s: Type 0 Revision %d record not supported\n",
		       __func__, revision);
		return AE_ERROR;
	}
	return AE_OK;
}

/* _HPX PCI-X Setting Record (Type 1) */
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
	int pos;

	if (!hpx)
		return;

	pos = pci_find_capability(dev, PCI_CAP_ID_PCIX);
	if (!pos)
		return;

	pci_warn(dev, "PCI-X settings not supported\n");
}

/*
 * decode_type1_hpx_record:
 *   _HPX Type 1 PCI-X 레코드를 디코딩한다. NVMe에는 직접 사용되지
 *   않으나 구조체 형식을 맞추기 위해 파싱한다.
 */
static acpi_status decode_type1_hpx_record(union acpi_object *record,
					   struct hpx_type1 *hpx1)
{
	int i;
	union acpi_object *fields = record->package.elements;
	u32 revision = fields[1].integer.value;

	switch (revision) {
	case 1:
		if (record->package.count != 5)
			return AE_ERROR;
		for (i = 2; i < 5; i++)
			if (fields[i].type != ACPI_TYPE_INTEGER)
				return AE_ERROR;
		hpx1->revision      = revision;
		hpx1->max_mem_read  = fields[2].integer.value;
		hpx1->avg_max_split = fields[3].integer.value;
		hpx1->tot_max_split = fields[4].integer.value;
		break;
	default:
		pr_warn("%s: Type 1 Revision %d record not supported\n",
		       __func__, revision);
		return AE_ERROR;
	}
	return AE_OK;
}

/* _HPX PCI Express Setting Record (Type 2) */
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
	int pos;
	u32 reg32;
	const struct pci_host_bridge *host;

	if (!hpx)
		return;

	if (!pci_is_pcie(dev))
		return;

	host = pci_find_host_bridge(dev->bus);

	/*
	 * Only do the _HPX Type 2 programming if OS owns PCIe native
	 * hotplug but not AER.
	 * NVMe: OS가 native hotplug을, AER은 firmware가 관리할 때만 적용.
	 */
	if (!host->native_pcie_hotplug || host->native_aer)
		return;

	if (hpx->revision > 1) {
		pci_warn(dev, "PCIe settings rev %d not supported\n",
			 hpx->revision);
		return;
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
	hpx->pci_exp_devctl_and |= ~PCI_EXP_AER_FLAGS;
	hpx->pci_exp_devctl_or &= PCI_EXP_AER_FLAGS;

	/* Initialize Device Control Register */
	pcie_capability_clear_and_set_word(dev, PCI_EXP_DEVCTL,
			~hpx->pci_exp_devctl_and, hpx->pci_exp_devctl_or);

	/* Log if _HPX attempts to modify Link Control Register */
	if (pcie_cap_has_lnkctl(dev)) {
		if (hpx->pci_exp_lnkctl_and != 0xffff ||
		    hpx->pci_exp_lnkctl_or != 0)
			pci_info(dev, "_HPX attempts Link Control setting (AND %#06x OR %#06x)\n",
				 hpx->pci_exp_lnkctl_and,
				 hpx->pci_exp_lnkctl_or);
	}

	/* Find Advanced Error Reporting Enhanced Capability */
	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ERR);
	if (!pos)
		return;

	/* Initialize Uncorrectable Error Mask Register */
	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_MASK, &reg32);
	reg32 = (reg32 & hpx->unc_err_mask_and) | hpx->unc_err_mask_or;
	pci_write_config_dword(dev, pos + PCI_ERR_UNCOR_MASK, reg32);

	/* Initialize Uncorrectable Error Severity Register */
	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_SEVER, &reg32);
	reg32 = (reg32 & hpx->unc_err_sever_and) | hpx->unc_err_sever_or;
	pci_write_config_dword(dev, pos + PCI_ERR_UNCOR_SEVER, reg32);

	/* Initialize Correctable Error Mask Register */
	pci_read_config_dword(dev, pos + PCI_ERR_COR_MASK, &reg32);
	reg32 = (reg32 & hpx->cor_err_mask_and) | hpx->cor_err_mask_or;
	pci_write_config_dword(dev, pos + PCI_ERR_COR_MASK, reg32);

	/* Initialize Advanced Error Capabilities and Control Register */
	pci_read_config_dword(dev, pos + PCI_ERR_CAP, &reg32);
	reg32 = (reg32 & hpx->adv_err_cap_and) | hpx->adv_err_cap_or;

	/* Don't enable ECRC generation or checking if unsupported */
	if (!(reg32 & PCI_ERR_CAP_ECRC_GENC))
		reg32 &= ~PCI_ERR_CAP_ECRC_GENE;
	if (!(reg32 & PCI_ERR_CAP_ECRC_CHKC))
		reg32 &= ~PCI_ERR_CAP_ECRC_CHKE;
	pci_write_config_dword(dev, pos + PCI_ERR_CAP, reg32);

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
	int i;
	union acpi_object *fields = record->package.elements;
	u32 revision = fields[1].integer.value;

	switch (revision) {
	case 1:
		if (record->package.count != 18)
			return AE_ERROR;
		for (i = 2; i < 18; i++)
			if (fields[i].type != ACPI_TYPE_INTEGER)
				return AE_ERROR;
		hpx2->revision      = revision;
		hpx2->unc_err_mask_and      = fields[2].integer.value;
		hpx2->unc_err_mask_or       = fields[3].integer.value;
		hpx2->unc_err_sever_and     = fields[4].integer.value;
		hpx2->unc_err_sever_or      = fields[5].integer.value;
		hpx2->cor_err_mask_and      = fields[6].integer.value;
		hpx2->cor_err_mask_or       = fields[7].integer.value;
		hpx2->adv_err_cap_and       = fields[8].integer.value;
		hpx2->adv_err_cap_or        = fields[9].integer.value;
		hpx2->pci_exp_devctl_and    = fields[10].integer.value;
		hpx2->pci_exp_devctl_or     = fields[11].integer.value;
		hpx2->pci_exp_lnkctl_and    = fields[12].integer.value;
		hpx2->pci_exp_lnkctl_or     = fields[13].integer.value;
		hpx2->sec_unc_err_sever_and = fields[14].integer.value;
		hpx2->sec_unc_err_sever_or  = fields[15].integer.value;
		hpx2->sec_unc_err_mask_and  = fields[16].integer.value;
		hpx2->sec_unc_err_mask_or   = fields[17].integer.value;
		break;
	default:
		pr_warn("%s: Type 2 Revision %d record not supported\n",
		       __func__, revision);
		return AE_ERROR;
	}
	return AE_OK;
}

/* _HPX PCI Express Setting Record (Type 3) */
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
	HPX_TYPE_ENDPOINT	= BIT(0),
	HPX_TYPE_LEG_END	= BIT(1),
	HPX_TYPE_RC_END		= BIT(2),
	HPX_TYPE_RC_EC		= BIT(3),
	HPX_TYPE_ROOT_PORT	= BIT(4),
	HPX_TYPE_UPSTREAM	= BIT(5),
	HPX_TYPE_DOWNSTREAM	= BIT(6),
	HPX_TYPE_PCI_BRIDGE	= BIT(7),
	HPX_TYPE_PCIE_BRIDGE	= BIT(8),
};

/*
 * hpx3_device_type:
 *   pci_dev의 PCIe device/port type을 _HPX Type 3의 device_type
 *   비트마스크로 변환한다. NVMe endpoint인지 Root Port인지 판별할 때
 *   사용된다.
 */
static u16 hpx3_device_type(struct pci_dev *dev)
{
	u16 pcie_type = pci_pcie_type(dev);
	static const int pcie_to_hpx3_type[] = {
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

	if (pcie_type >= ARRAY_SIZE(pcie_to_hpx3_type))
		return 0;

	return pcie_to_hpx3_type[pcie_type];
}

enum hpx_type3_fn_type {
	HPX_FN_NORMAL		= BIT(0),
	HPX_FN_SRIOV_PHYS	= BIT(1),
	HPX_FN_SRIOV_VIRT	= BIT(2),
};

/*
 * hpx3_function_type:
 *   pci_dev가 일반 PF, SR-IOV PF, VF 중 어느 것인지 _HPX Type 3의
 *   function_type 비트마스크로 반환한다. NVMe SR-IOV 환경에서 VF에
 *   대한 레지스터 패치 적용 여부를 결정한다.
 */
static u8 hpx3_function_type(struct pci_dev *dev)
{
	if (dev->is_virtfn)
		return HPX_FN_SRIOV_VIRT;
	else if (pci_find_ext_capability(dev, PCI_EXT_CAP_ID_SRIOV) > 0)
		return HPX_FN_SRIOV_PHYS;
	else
		return HPX_FN_NORMAL;
}

/*
 * hpx3_cap_ver_matches:
 *   _HPX Type 3에 지정된 capability version이 실제 PCIe capability
 *   version과 일치하는지 검사한다. NVMe의 다양한 PCIe/DVSEC capability
 *   버전 호환성 판단에 사용된다.
 */
static bool hpx3_cap_ver_matches(u8 pcie_cap_id, u8 hpx3_cap_id)
{
	u8 cap_ver = hpx3_cap_id & 0xf;

	if ((hpx3_cap_id & BIT(4)) && cap_ver >= pcie_cap_id)
		return true;
	else if (cap_ver == pcie_cap_id)
		return true;

	return false;
}

enum hpx_type3_cfg_loc {
	HPX_CFG_PCICFG		= 0,
	HPX_CFG_PCIE_CAP	= 1,
	HPX_CFG_PCIE_CAP_EXT	= 2,
	HPX_CFG_VEND_CAP	= 3,
	HPX_CFG_DVSEC		= 4,
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
	u32 match_reg, write_reg, header, orig_value;
	u16 pos;

	if (!(hpx3_device_type(dev) & reg->device_type))
		return;

	if (!(hpx3_function_type(dev) & reg->function_type))
		return;

	switch (reg->config_space_location) {
	case HPX_CFG_PCICFG:
		pos = 0;
		break;
	case HPX_CFG_PCIE_CAP:
		pos = pci_find_capability(dev, reg->pci_exp_cap_id);
		if (pos == 0)
			return;

		break;
	case HPX_CFG_PCIE_CAP_EXT:
		pos = pci_find_ext_capability(dev, reg->pci_exp_cap_id);
		if (pos == 0)
			return;

		pci_read_config_dword(dev, pos, &header);
		if (!hpx3_cap_ver_matches(PCI_EXT_CAP_VER(header),
					  reg->pci_exp_cap_ver))
			return;

		break;
	case HPX_CFG_VEND_CAP:
	case HPX_CFG_DVSEC:
	default:
		pci_warn(dev, "Encountered _HPX type 3 with unsupported config space location");
		return;
	}

	pci_read_config_dword(dev, pos + reg->match_offset, &match_reg);

	if ((match_reg & reg->match_mask_and) != reg->match_value)
		return;

	pci_read_config_dword(dev, pos + reg->reg_offset, &write_reg);
	orig_value = write_reg;
	write_reg &= reg->reg_mask_and;
	write_reg |= reg->reg_mask_or;

	if (orig_value == write_reg)
		return;

	pci_write_config_dword(dev, pos + reg->reg_offset, write_reg);

	pci_dbg(dev, "Applied _HPX3 at [0x%x]: 0x%08x -> 0x%08x",
		pos, orig_value, write_reg);
}

/*
 * program_hpx_type3:
 *   _HPX Type 3 레코드를 NVMe PCIe 디바이스에 적용한다. Type 3은
 *   DVSEC, vendor capability 등 다양한 config space 레지스터를
 *   유연하게 패치할 수 있다.
 */
static void program_hpx_type3(struct pci_dev *dev, struct hpx_type3 *hpx)
{
	if (!hpx)
		return;

	if (!pci_is_pcie(dev))
		return;

	program_hpx_type3_register(dev, hpx);
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
	hpx3_reg->device_type            = reg_fields[0].integer.value;
	hpx3_reg->function_type          = reg_fields[1].integer.value;
	hpx3_reg->config_space_location  = reg_fields[2].integer.value;
	hpx3_reg->pci_exp_cap_id         = reg_fields[3].integer.value;
	hpx3_reg->pci_exp_cap_ver        = reg_fields[4].integer.value;
	hpx3_reg->pci_exp_vendor_id      = reg_fields[5].integer.value;
	hpx3_reg->dvsec_id               = reg_fields[6].integer.value;
	hpx3_reg->dvsec_rev              = reg_fields[7].integer.value;
	hpx3_reg->match_offset           = reg_fields[8].integer.value;
	hpx3_reg->match_mask_and         = reg_fields[9].integer.value;
	hpx3_reg->match_value            = reg_fields[10].integer.value;
	hpx3_reg->reg_offset             = reg_fields[11].integer.value;
	hpx3_reg->reg_mask_and           = reg_fields[12].integer.value;
	hpx3_reg->reg_mask_or            = reg_fields[13].integer.value;
}

/*
 * program_type3_hpx_record:
 *   _HPX Type 3 패키지 전체를 순회하며 포함된 모든 레지스터 패치를
 *   NVMe/PCI 디바이스에 적용한다.
 */
static acpi_status program_type3_hpx_record(struct pci_dev *dev,
					   union acpi_object *record)
{
	union acpi_object *fields = record->package.elements;
	u32 desc_count, expected_length, revision;
	union acpi_object *reg_fields;
	struct hpx_type3 hpx3;
	int i;

	revision = fields[1].integer.value;
	switch (revision) {
	case 1:
		desc_count = fields[2].integer.value;
		expected_length = 3 + desc_count * 14;

		if (record->package.count != expected_length)
			return AE_ERROR;

		for (i = 2; i < expected_length; i++)
			if (fields[i].type != ACPI_TYPE_INTEGER)
				return AE_ERROR;

		for (i = 0; i < desc_count; i++) {
			reg_fields = fields + 3 + i * 14;
			parse_hpx3_register(&hpx3, reg_fields);
			program_hpx_type3(dev, &hpx3);
		}	/* [한국어] 순회 끝 */

		break;
	default:
		printk(KERN_WARNING
			"%s: Type 3 Revision %d record not supported\n",
			__func__, revision);
		return AE_ERROR;
	}
	return AE_OK;
}

/*
 * acpi_run_hpx:
 *   주어진 ACPI 핸들의 _HPX 메서드를 평가하여 Type 0/1/2/3 설정을
 *   NVMe/PCI 디바이스에 차례로 적용한다. NVMe 초기화 시 PCIe
 *   capability, AER, DVSEC 등에 platform quirk를 적용하는 통로다.
 */
static acpi_status acpi_run_hpx(struct pci_dev *dev, acpi_handle handle)
{
	acpi_status status;
	struct acpi_buffer buffer = {ACPI_ALLOCATE_BUFFER, NULL};
	union acpi_object *package, *record, *fields;
	struct hpx_type0 hpx0;
	struct hpx_type1 hpx1;
	struct hpx_type2 hpx2;
	u32 type;
	int i;

	status = acpi_evaluate_object(handle, "_HPX", NULL, &buffer);
	if (ACPI_FAILURE(status))
		return status;

	package = (union acpi_object *)buffer.pointer;
	if (package->type != ACPI_TYPE_PACKAGE) {
		status = AE_ERROR;
		goto exit;
	}

	for (i = 0; i < package->package.count; i++) {
		record = &package->package.elements[i];
		if (record->type != ACPI_TYPE_PACKAGE) {
			status = AE_ERROR;
			goto exit;
		}

		fields = record->package.elements;
		if (fields[0].type != ACPI_TYPE_INTEGER ||
		    fields[1].type != ACPI_TYPE_INTEGER) {
			status = AE_ERROR;
			goto exit;
		}

		type = fields[0].integer.value;
		switch (type) {
		case 0:
			memset(&hpx0, 0, sizeof(hpx0));
			status = decode_type0_hpx_record(record, &hpx0);
			if (ACPI_FAILURE(status))
				goto exit;
			program_hpx_type0(dev, &hpx0);
			break;
		case 1:
			memset(&hpx1, 0, sizeof(hpx1));
			status = decode_type1_hpx_record(record, &hpx1);
			if (ACPI_FAILURE(status))
				goto exit;
			program_hpx_type1(dev, &hpx1);
			break;
		case 2:
			memset(&hpx2, 0, sizeof(hpx2));
			status = decode_type2_hpx_record(record, &hpx2);
			if (ACPI_FAILURE(status))
				goto exit;
			program_hpx_type2(dev, &hpx2);
			break;
		case 3:
			status = program_type3_hpx_record(dev, record);
			if (ACPI_FAILURE(status))
				goto exit;
			break;
		default:
			pr_err("%s: Type %d record not supported\n",
			       __func__, type);
			status = AE_ERROR;
			goto exit;
		}
	}
 exit:
	kfree(buffer.pointer);
	return status;
}

/*
 * acpi_run_hpp:
 *   ACPI _HPP(Hot Plug Parameters) 메서드를 평가하여 Type 0와
 *   동일한 설정을 NVMe/PCI 디바이스에 적용한다. _HPX가 없는 플랫폼의
 *   fallback 경로다.
 */
static acpi_status acpi_run_hpp(struct pci_dev *dev, acpi_handle handle)
{
	acpi_status status;
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *package, *fields;
	struct hpx_type0 hpx0;
	int i;

	memset(&hpx0, 0, sizeof(hpx0));

	status = acpi_evaluate_object(handle, "_HPP", NULL, &buffer);
	if (ACPI_FAILURE(status))
		return status;

	package = (union acpi_object *) buffer.pointer;
	if (package->type != ACPI_TYPE_PACKAGE ||
	    package->package.count != 4) {
		status = AE_ERROR;
		goto exit;
	}

	fields = package->package.elements;
	for (i = 0; i < 4; i++) {
		if (fields[i].type != ACPI_TYPE_INTEGER) {
			status = AE_ERROR;
			goto exit;
		}
	}	/* [한국어] 순회 끝 */

	hpx0.revision        = 1;
	hpx0.cache_line_size = fields[0].integer.value;
	hpx0.latency_timer   = fields[1].integer.value;
	hpx0.enable_serr     = fields[2].integer.value;
	hpx0.enable_perr     = fields[3].integer.value;

	program_hpx_type0(dev, &hpx0);

exit:
	kfree(buffer.pointer);
	return status;
}

/* pci_acpi_program_hp_params
 *
 * @dev - the pci_dev for which we want parameters
 */
/*
 * [한국어]
 * pci_acpi_program_hp_params - 펌웨어가 권장하는 PCIe 설정값을 장치에 적용한다
 *
 * @dev:    설정할 장치
 * @return: 0 = 적용됨, 음수 = 적용할 값을 찾지 못함.
 *
 * _HPX(Hot Plug Parameter Extensions) 또는 그 이전 판인 _HPP 는 펌웨어가
 * "이 슬롯에 꽂히는 장치는 이렇게 설정하라" 고 권장하는 값의 묶음이다.
 * Cache Line Size, Latency Timer, SERR/PERR 활성화, 그리고 PCIe 쪽으로는
 * Max Payload Size, 오류 보고, Completion Timeout, AER 마스크 등이 들어 있다.
 *
 * 왜 펌웨어가 정해 주는가. 같은 장치라도 어느 보드의 어느 슬롯에 꽂히느냐에
 * 따라 안전한 설정이 달라지기 때문이다. 신호 품질이 나쁜 슬롯에서는
 * 오류 보고를 더 촘촘히 켜야 하고, 특정 칩셋에서는 어떤 기능을 꺼야 한다.
 * 보드 설계자만 아는 그 지식을 ACPI 테이블로 전달하는 것이다.
 *
 * 검색이 위로 올라가는 방식인 점이 중요하다. 이 장치의 ACPI 노드에
 * _HPX 가 없으면 부모로, 또 없으면 그 부모로 계속 올라간다. 슬롯마다
 * 값을 적어 두지 않고 브리지 하나에 적어 그 아래 전부에 적용하는 것이
 * 흔한 구성이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. ACPI 메서드 평가가 잠들 수 있다.
 * 호출자: pci_configure_device() 경로 — 장치를 발견해 설정할 때.
 */
int pci_acpi_program_hp_params(struct pci_dev *dev)
{
	acpi_status status;
	acpi_handle handle, phandle;
	struct pci_bus *pbus;

	if (acpi_pci_disabled)
		return -ENODEV;

	handle = NULL;
	for (pbus = dev->bus; pbus; pbus = pbus->parent) {
		handle = acpi_pci_get_bridge_handle(pbus);
		if (handle)
			break;
	}	/* [한국어] 순회 끝 */

	/*
	 * _HPP settings apply to all child buses, until another _HPP is
	 * encountered. If we don't find an _HPP for the input pci dev,
	 * look for it in the parent device scope since that would apply to
	 * this pci dev.
	 * NVMe: _HPP/_HPX는 하위 bus에 상속되므로, NVMe 핸들에서 못 찾으면
	 *       부모 bridge 범위까지 올라가며 검색한다.
	 */
	while (handle) {
		status = acpi_run_hpx(dev, handle);
		if (ACPI_SUCCESS(status))
			return 0;
		status = acpi_run_hpp(dev, handle);
		if (ACPI_SUCCESS(status))
			return 0;
		if (acpi_is_root_bridge(handle))
			break;
		status = acpi_get_parent(handle, &phandle);
		if (ACPI_FAILURE(status))
			break;
		handle = phandle;
	}	/* [한국어] 순회 끝 */
	return -ENODEV;
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
	const struct pci_host_bridge *host;

	if (!IS_ENABLED(CONFIG_HOTPLUG_PCI_PCIE))
		return false;

	if (pcie_ports_native)
		return true;

	host = pci_find_host_bridge(bridge->bus);
	return host->native_pcie_hotplug;
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
	return bridge->shpc_managed;
}

/**
 * pci_acpi_wake_bus - Root bus wakeup notification fork function.
 * @context: Device wakeup context.
 * NVMe: ACPI wake 이벤트 발생 시 NVMe가 속한 root bus의 PME 처리를
 *       fork하는 콜백.
 */
static void pci_acpi_wake_bus(struct acpi_device_wakeup_context *context)
{
	pci_pme_wakeup_bus(to_pci_host_bridge(context->dev)->bus);
}

/**
 * pci_acpi_wake_dev - PCI device wakeup notification work function.
 * @context: Device wakeup context.
 * NVMe: ACPI wake 이벤트 발생 시 NVMe endpoint의 PME status를 클리어하고
 *       resume를 요청하는 work 함수.
 */
static void pci_acpi_wake_dev(struct acpi_device_wakeup_context *context)
{
	struct pci_dev *pci_dev;

	pci_dev = to_pci_dev(context->dev);

	if (pci_dev->pme_poll)
		pci_dev->pme_poll = false;

	if (pci_dev->current_state == PCI_D3cold) {
		pci_wakeup_event(pci_dev);
		pm_request_resume(&pci_dev->dev);
		return;
	}

	/* Clear PME Status if set. */
	if (pci_dev->pme_support)
		pci_check_pme_status(pci_dev);

	pci_wakeup_event(pci_dev);
	pm_request_resume(&pci_dev->dev);

	pci_pme_wakeup_bus(pci_dev->subordinate);
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
	return acpi_add_pm_notifier(dev, root->bus->bridge, pci_acpi_wake_bus);
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
	return acpi_add_pm_notifier(dev, &pci_dev->dev, pci_acpi_wake_dev);
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
	int acpi_state, d_max;

	if (pdev->no_d3cold || !pdev->d3cold_allowed)
		d_max = ACPI_STATE_D3_HOT;
	else
		d_max = ACPI_STATE_D3_COLD;
	acpi_state = acpi_pm_device_sleep_state(&pdev->dev, NULL, d_max);
	if (acpi_state < 0)
		return PCI_POWER_ERROR;

	switch (acpi_state) {
	case ACPI_STATE_D0:
		return PCI_D0;
	case ACPI_STATE_D1:
		return PCI_D1;
	case ACPI_STATE_D2:
		return PCI_D2;
	case ACPI_STATE_D3_HOT:
		return PCI_D3hot;
	case ACPI_STATE_D3_COLD:
		return PCI_D3cold;
	}
	return PCI_POWER_ERROR;
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
	if (!dev_fwnode(&dev->dev) && !pci_dev_is_added(dev))
		ACPI_COMPANION_SET(&dev->dev,
				   acpi_pci_find_companion(&dev->dev));
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
	acpi_handle handle = ACPI_HANDLE(&dev->dev);
	int ret;

	if (!handle || !acpi_has_method(handle, "_RST"))
		return -ENOTTY;

	if (probe)
		return 0;

	ret = pci_dev_reset_iommu_prepare(dev);
	if (ret) {
		pci_err(dev, "failed to stop IOMMU for a PCI reset: %d\n", ret);
		return ret;
	}

	if (ACPI_FAILURE(acpi_evaluate_object(handle, "_RST", NULL, NULL))) {
		pci_warn(dev, "ACPI _RST failed\n");
		ret = -ENOTTY;
	}

	pci_dev_reset_iommu_done(dev);
	return ret;
}

/*
 * acpi_pci_power_manageable:
 *   NVMe 디바이스가 ACPI를 통해 전원 관리 가능한지 확인한다.
 *   _PSx 메서드가 있으면 true.
 */
bool acpi_pci_power_manageable(struct pci_dev *dev)
{
	struct acpi_device *adev = ACPI_COMPANION(&dev->dev);

	return adev && acpi_device_power_manageable(adev);
}

/*
 * acpi_pci_bridge_d3:
 *   NVMe가 연결된 PCIe bridge(또는 Root Port)가 D3 상태에서 wake를
 *   유지하며 hotplug 이벤트를 처리할 수 있는지 판단한다. NVMe
 *   hotplug/surprise removal 시 bridge 전원 정책에 영향을 준다.
 */
bool acpi_pci_bridge_d3(struct pci_dev *dev)
{
	struct pci_dev *rpdev;
	struct acpi_device *adev, *rpadev;
	const union acpi_object *obj;

	if (acpi_pci_disabled || !dev->is_pciehp)
		return false;

	adev = ACPI_COMPANION(&dev->dev);

	if (adev) {
		/*
		 * If the bridge has _S0W, whether or not it can go into D3
		 * depends on what is returned by that object.  In particular,
		 * if the power state returned by _S0W is D2 or shallower,
		 * entering D3 should not be allowed.
		 * NVMe: bridge의 _S0W가 D2 이하면 D3 진입 불가.
		 */
		if (acpi_dev_power_state_for_wake(adev) <= ACPI_STATE_D2)
			return false;

		/*
		 * Otherwise, assume that the bridge can enter D3 so long as it
		 * is power-manageable via ACPI.
		 * NVMe: ACPI 전원 관리 가능하면 D3 진입 가능.
		 */
		if (acpi_device_power_manageable(adev))
			return true;
	}

	rpdev = pcie_find_root_port(dev);
	if (!rpdev)
		return false;

	if (rpdev == dev)
		rpadev = adev;
	else
		rpadev = ACPI_COMPANION(&rpdev->dev);

	if (!rpadev)
		return false;

	/*
	 * If the Root Port cannot signal wakeup signals at all, i.e., it
	 * doesn't supply a wakeup GPE via _PRW, it cannot signal hotplug
	 * events from low-power states including D3hot and D3cold.
	 * NVMe: Root Port에 _PRW 기반 wake GPE가 없으면 D3에서 hotplug 이벤트 처리 불가.
	 */
	if (!rpadev->wakeup.flags.valid)
		return false;

	/*
	 * In the bridge-below-a-Root-Port case, evaluate _S0W for the Root Port
	 * to verify whether or not it can signal wakeup from D3.
	 * NVMe: Root Port 아래 bridge인 경우 Root Port의 _S0W도 확인.
	 */
	if (rpadev != adev &&
	    acpi_dev_power_state_for_wake(rpadev) <= ACPI_STATE_D2)
		return false;

	/*
	 * The "HotPlugSupportInD3" property in a Root Port _DSD indicates
	 * the Port can signal hotplug events while in D3.  We assume any
	 * bridges *below* that Root Port can also signal hotplug events
	 * while in D3.
	 * NVMe: Root Port _DSD에 HotPlugSupportInD3=1이면 D3에서 hotplug 지원.
	 */
	if (!acpi_dev_get_property(rpadev, "HotPlugSupportInD3",
				   ACPI_TYPE_INTEGER, &obj) &&
	    obj->integer.value == 1)
		return true;

	return false;
}

/*
 * acpi_pci_config_space_access:
 *   ACPI _REG 메서드를 호출해 NVMe 디바이스의 PCI config space 접근
 *   가능/불가를 AML에 통지한다. D3cold 진입/복귀 시 호출되어 config
 *   space 접근성을 동기화한다.
 */
static void acpi_pci_config_space_access(struct pci_dev *dev, bool enable)
{
	int val = enable ? ACPI_REG_CONNECT : ACPI_REG_DISCONNECT;
	int ret = acpi_evaluate_reg(ACPI_HANDLE(&dev->dev),
				    ACPI_ADR_SPACE_PCI_CONFIG, val);
	if (ret)
		pci_dbg(dev, "ACPI _REG %s evaluation failed (%d)\n",
			enable ? "connect" : "disconnect", ret);
}

/*
 * acpi_pci_set_power_state:
 *   NVMe 디바이스의 전원 상태를 PCI_D0/PCI_D3hot/PCI_D3cold 등으로
 *   ACPI _PSx 메서드를 통해 전환한다. NVMe reset, suspend, resume 시
 *   pci_set_power_state() 아래에서 호출된다.
 */
int acpi_pci_set_power_state(struct pci_dev *dev, pci_power_t state)
{
	struct acpi_device *adev = ACPI_COMPANION(&dev->dev);
	static const u8 state_conv[] = {
		[PCI_D0] = ACPI_STATE_D0,
		[PCI_D1] = ACPI_STATE_D1,
		[PCI_D2] = ACPI_STATE_D2,
		[PCI_D3hot] = ACPI_STATE_D3_HOT,
		[PCI_D3cold] = ACPI_STATE_D3_COLD,
	};
	int error;

	/* If the ACPI device has _EJ0, ignore the device */
	if (!adev || acpi_has_method(adev->handle, "_EJ0"))
		return -ENODEV;

	switch (state) {
	case PCI_D0:
	case PCI_D1:
	case PCI_D2:
	case PCI_D3hot:
	case PCI_D3cold:
		break;
	default:
		return -EINVAL;
	}

	if (state == PCI_D3cold) {
		if (dev_pm_qos_flags(&dev->dev, PM_QOS_FLAG_NO_POWER_OFF) ==
				PM_QOS_FLAGS_ALL)
			return -EBUSY;

		/* Notify AML lack of PCI config space availability */
		acpi_pci_config_space_access(dev, false);
	}

	error = acpi_device_set_power(adev, state_conv[state]);
	if (error)
		return error;

	pci_dbg(dev, "power state changed by ACPI to %s\n",
	        acpi_power_state_string(adev->power.state));

	/*
	 * Notify AML of PCI config space availability.  Config space is
	 * accessible in all states except D3cold; the only transitions
	 * that change availability are transitions to D3cold and from
	 * D3cold to D0.
	 * NVMe: D3cold->D0 복귀 시 AML에 config space 접근 가능 통지.
	 */
	if (state == PCI_D0)
		acpi_pci_config_space_access(dev, true);

	return 0;
}

/*
 * acpi_pci_get_power_state:
 *   ACPI를 통해 현재 NVMe 디바이스의 전원 상태를 조회한다.
 */
pci_power_t acpi_pci_get_power_state(struct pci_dev *dev)
{
	struct acpi_device *adev = ACPI_COMPANION(&dev->dev);
	static const pci_power_t state_conv[] = {
		[ACPI_STATE_D0]      = PCI_D0,
		[ACPI_STATE_D1]      = PCI_D1,
		[ACPI_STATE_D2]      = PCI_D2,
		[ACPI_STATE_D3_HOT]  = PCI_D3hot,
		[ACPI_STATE_D3_COLD] = PCI_D3cold,
	};
	int state;

	if (!adev || !acpi_device_power_manageable(adev))
		return PCI_UNKNOWN;

	state = adev->power.state;
	if (state == ACPI_STATE_UNKNOWN)
		return PCI_UNKNOWN;

	return state_conv[state];
}

/*
 * acpi_pci_refresh_power_state:
 *   NVMe 디바이스의 ACPI 전원 상태를 갱신하여 실제 하드웨어 상태와
 *   동기화한다. resume 후 상태 불일치 문제를 방지한다.
 */
void acpi_pci_refresh_power_state(struct pci_dev *dev)
{
	struct acpi_device *adev = ACPI_COMPANION(&dev->dev);

	if (adev && acpi_device_power_manageable(adev))
		acpi_device_update_power(adev, NULL);
}

/*
 * acpi_pci_propagate_wakeup:
 *   NVMe endpoint에서 상위 bus를 따라 root bus까지 wake 기능을
 *   전파하며 활성화/비활성화한다. NVMe wake-on-LAN/디스크 wake 등에서
 *   상위 bridge의 wake도 함께 설정해야 한다.
 */
static int acpi_pci_propagate_wakeup(struct pci_bus *bus, bool enable)
{
	while (bus->parent) {
		if (acpi_pm_device_can_wakeup(&bus->self->dev))
			return acpi_pm_set_device_wakeup(&bus->self->dev, enable);

		bus = bus->parent;
	}	/* [한국어] 순회 끝 */

	/* We have reached the root bus. */
	if (bus->bridge) {
		if (acpi_pm_device_can_wakeup(bus->bridge))
			return acpi_pm_set_device_wakeup(bus->bridge, enable);
	}
	return 0;
}

/*
 * acpi_pci_wakeup:
 *   NVMe 디바이스의 ACPI wake 기능을 enable/disable한다. NVMe
 *   장치가 sleep 상태에서 시스템을 깨울 수 있도록 허용할 때 사용.
 */
int acpi_pci_wakeup(struct pci_dev *dev, bool enable)
{
	if (acpi_pci_disabled)
		return 0;

	if (acpi_pm_device_can_wakeup(&dev->dev))
		return acpi_pm_set_device_wakeup(&dev->dev, enable);

	return acpi_pci_propagate_wakeup(dev->bus, enable);
}

/*
 * acpi_pci_need_resume:
 *   NVMe 디바이스가 시스템 resume 시 반드시 resume해야 하는지
 *   ACPI 정보를 기반으로 판단한다. _PRW, _DSW 등 wake 설정에 따라
 *   달라진다.
 */
bool acpi_pci_need_resume(struct pci_dev *dev)
{
	struct acpi_device *adev;

	if (acpi_pci_disabled)
		return false;

	/*
	 * In some cases (eg. Samsung 305V4A) leaving a bridge in suspend over
	 * system-wide suspend/resume confuses the platform firmware, so avoid
	 * doing that.  According to Section 16.1.6 of ACPI 6.2, endpoint
	 * devices are expected to be in D3 before invoking the S3 entry path
	 * from the firmware, so they should not be affected by this issue.
	 * NVMe: NVMe는 endpoint이므로 이 이슈에 영향받지 않음.
	 */
	if (pci_is_bridge(dev) && acpi_target_system_state() != ACPI_STATE_S0)
		return true;

	adev = ACPI_COMPANION(&dev->dev);
	if (!adev || !acpi_device_power_manageable(adev))
		return false;

	if (adev->wakeup.flags.valid &&
	    device_may_wakeup(&dev->dev) != !!adev->wakeup.prepare_count)
		return true;

	if (acpi_target_system_state() == ACPI_STATE_S0)
		return false;

	return !!adev->power.flags.dsw_present;
}

/*
 * acpi_pci_add_bus:
 *   PCI bus가 추가될 때 ACPI 측면의 초기화를 수행한다. NVMe가 속한
 *   bus의 slot enumeration, hotplug slot 등록, host bridge의 reset
 *   delay 최적화(_DSM func 8)를 처리한다.
 */
void acpi_pci_add_bus(struct pci_bus *bus)
{
	union acpi_object *obj;
	struct pci_host_bridge *bridge;

	if (acpi_pci_disabled || !bus->bridge || !ACPI_HANDLE(bus->bridge))
		return;

	acpi_pci_slot_enumerate(bus);
	acpiphp_enumerate_slots(bus);

	/*
	 * For a host bridge, check its _DSM for function 8 and if
	 * that is available, mark it in pci_host_bridge.
	 * NVMe: host bridge _DSM func 8(reset delay) 조회.
	 */
	if (!pci_is_root_bus(bus))
		return;

	obj = acpi_evaluate_dsm_typed(ACPI_HANDLE(bus->bridge), &pci_acpi_dsm_guid, 3,
				      DSM_PCI_POWER_ON_RESET_DELAY, NULL, ACPI_TYPE_INTEGER);
	if (!obj)
		return;

	if (obj->integer.value == 1) {
		bridge = pci_find_host_bridge(bus);
		bridge->ignore_reset_delay = 1;
	}
	ACPI_FREE(obj);
}

/*
 * acpi_pci_remove_bus:
 *   PCI bus가 제거될 때 ACPI hotplug/slot 리소스를 정리한다. NVMe
 *   장치가 제거되거나 bus가 사라질 때 호출.
 */
void acpi_pci_remove_bus(struct pci_bus *bus)
{
	if (acpi_pci_disabled || !bus->bridge)
		return;

	acpiphp_remove_slots(bus);
	acpi_pci_slot_remove(bus);
}

/* ACPI bus type */


static DECLARE_RWSEM(pci_acpi_companion_lookup_sem);
static struct acpi_device *(*pci_acpi_find_companion_hook)(struct pci_dev *);

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
	int ret;

	if (!func)
		return -EINVAL;

	down_write(&pci_acpi_companion_lookup_sem);

	if (pci_acpi_find_companion_hook) {
		ret = -EBUSY;
	} else {
		pci_acpi_find_companion_hook = func;
		ret = 0;
	}

	up_write(&pci_acpi_companion_lookup_sem);

	return ret;
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
	down_write(&pci_acpi_companion_lookup_sem);

	pci_acpi_find_companion_hook = NULL;

	up_write(&pci_acpi_companion_lookup_sem);
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
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct acpi_device *adev;
	bool check_children;
	u64 addr;

	if (!dev->parent)
		return NULL;

	down_read(&pci_acpi_companion_lookup_sem);

	adev = pci_acpi_find_companion_hook ?
		pci_acpi_find_companion_hook(pci_dev) : NULL;

	up_read(&pci_acpi_companion_lookup_sem);

	if (adev)
		return adev;

	check_children = pci_is_bridge(pci_dev);
	/* Please ref to ACPI spec for the syntax of _ADR */
	addr = (PCI_SLOT(pci_dev->devfn) << 16) | PCI_FUNC(pci_dev->devfn);
	adev = acpi_find_child_device(ACPI_COMPANION(dev->parent), addr,
				      check_children);

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
	    pci_is_root_bus(pci_dev->bus))
		return NULL;

	return adev;
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
	struct pci_host_bridge *bridge = pci_find_host_bridge(pdev->bus);
	int value;
	union acpi_object *obj, *elements;

	if (bridge->ignore_reset_delay)
		pdev->d3cold_delay = 0;

	obj = acpi_evaluate_dsm_typed(handle, &pci_acpi_dsm_guid, 3,
				      DSM_PCI_DEVICE_READINESS_DURATIONS, NULL,
				      ACPI_TYPE_PACKAGE);
	if (!obj)
		return;

	if (obj->package.count == 5) {
		elements = obj->package.elements;
		if (elements[0].type == ACPI_TYPE_INTEGER) {
			value = (int)elements[0].integer.value / 1000;
			if (value < PCI_PM_D3COLD_WAIT)
				pdev->d3cold_delay = value;
		}
		if (elements[3].type == ACPI_TYPE_INTEGER) {
			value = (int)elements[3].integer.value / 1000;
			if (value < PCI_PM_D3HOT_WAIT)
				pdev->d3hot_delay = value;
		}
	}
	ACPI_FREE(obj);
}

/*
 * pci_acpi_set_external_facing:
 *   Root Port의 _DSD "ExternalFacingPort" 속성을 읽어 external_facing
 *   플래그를 설정한다. NVMe가 외부 PCIe 케이지/확장 슬롯에 연결된 경우
 *   DMA 보안 정책(IOMMU, ATS)에 영향을 줄 수 있다.
 */
static void pci_acpi_set_external_facing(struct pci_dev *dev)
{
	u8 val;

	if (pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT)
		return;
	if (device_property_read_u8(&dev->dev, "ExternalFacingPort", &val))
		return;

	/*
	 * These root ports expose PCIe (including DMA) outside of the
	 * system.  Everything downstream from them is external.
	 * NVMe: 이 Root Port 아래의 NVMe는 외부 접근 가능.
	 */
	if (val)
		dev->external_facing = 1;
}

/*
 * pci_acpi_setup:
 *   NVMe pci_dev가 ACPI companion과 연결된 후 호출되는 통합 설정.
 *   delay 최적화, external facing, EDR notifier, PM notifier, wake
 *   설정을 수행한다. NVMe probe 초기화의 핵심 ACPI 진입점.
 */
void pci_acpi_setup(struct device *dev, struct acpi_device *adev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);

	pci_acpi_optimize_delay(pci_dev, adev->handle);
	pci_acpi_set_external_facing(pci_dev);
	pci_acpi_add_edr_notifier(pci_dev);

	pci_acpi_add_pm_notifier(adev, pci_dev);
	if (!adev->wakeup.flags.valid)
		return;

	device_set_wakeup_capable(dev, true);
	/*
	 * For bridges that can do D3 we enable wake automatically (as
	 * we do for the power management itself in that case). The
	 * reason is that the bridge may have additional methods such as
	 * _DSW that need to be called.
	 * NVMe: D3 가능 bridge는 wake 자동 활성(_DSW 등 메서드 호출 필요).
	 */
	if (pci_dev->bridge_d3)
		device_wakeup_enable(dev);

	acpi_pci_wakeup(pci_dev, false);
	acpi_device_power_add_dependent(adev, dev);

	if (pci_is_bridge(pci_dev))
		acpi_dev_power_up_children_with_adr(adev);
}

/*
 * pci_acpi_cleanup:
 *   pci_acpi_setup()에서 등록한 ACPI notifier와 wake 설정을 제거한다.
 *   NVMe 디바이스 제거 시 호출.
 */
void pci_acpi_cleanup(struct device *dev, struct acpi_device *adev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);

	pci_acpi_remove_edr_notifier(pci_dev);
	pci_acpi_remove_pm_notifier(adev);
	if (adev->wakeup.flags.valid) {
		acpi_device_power_remove_dependent(adev, dev);
		if (pci_dev->bridge_d3)
			device_wakeup_disable(dev);

		device_set_wakeup_capable(dev, false);
	}
}

static struct fwnode_handle *(*pci_msi_get_fwnode_cb)(struct device *dev);

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
	pci_msi_get_fwnode_cb = fn;
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
	struct fwnode_handle *fwnode;

	if (!pci_msi_get_fwnode_cb)
		return NULL;

	fwnode = pci_msi_get_fwnode_cb(&bus->dev);
	if (!fwnode)
		return NULL;

	return irq_find_matching_fwnode(fwnode, DOMAIN_BUS_PCI_MSI);
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
	if (acpi_gbl_FADT.boot_flags & ACPI_FADT_NO_MSI) {
		pr_info("ACPI FADT declares the system doesn't support MSI, so disable it\n");
		pci_no_msi();
	}

	if (acpi_gbl_FADT.boot_flags & ACPI_FADT_NO_ASPM) {
		pr_info("ACPI FADT declares the system doesn't support PCIe ASPM, so disable it\n");
		pcie_no_aspm();
	}

	if (acpi_pci_disabled)
		return 0;

	acpi_pci_slot_init();
	acpiphp_init();

	return 0;
}
arch_initcall(acpi_pci_init);

#if defined(CONFIG_ARM64) || defined(CONFIG_RISCV)

/*
 * Try to assign the IRQ number when probing a new device
 */
/*
 * [한국어]
 * pcibios_alloc_irq - 장치를 발견했을 때 INTx IRQ 번호를 배정한다
 *
 * @dev:    대상 장치
 * @return: 항상 0. 실패해도 0 을 돌려준다.
 *
 * irq.c 의 __weak 훅을 ARM64/RISC-V 가 덮어쓴 판이다. ACPI 의 _PRT
 * (PCI Routing Table)를 보고 이 장치의 INTx 핀이 어느 인터럽트 컨트롤러의
 * 몇 번 입력에 연결됐는지 알아내 dev->irq 에 채운다.
 *
 * 반환값이 항상 0 인 것이 눈에 띈다. IRQ 배정에 실패해도 probe 를 막지
 * 않는다는 뜻이다. MSI/MSI-X 를 쓰는 장치는 INTx 가 없어도 잘 동작하므로,
 * 여기서 실패했다고 장치를 포기할 이유가 없다.
 *
 * x86 에는 이 훅의 덮어쓰기가 없다. 그쪽은 pci_enable_device() 시점에
 * 아키텍처 코드가 따로 처리한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 드라이버 바인딩 직전에 불린다.
 * 호출자: pci-driver.c 의 pci_device_probe().
 */
int pcibios_alloc_irq(struct pci_dev *dev)
{
	if (!acpi_disabled)
		acpi_pci_irq_enable(dev);

	return 0;
}

/* [한국어] ACPI 기반 루트 브리지 하나의 정보. 공통 부분에 ECAM 창
 * 포인터를 덧붙인 형태다. common 을 첫 필드로 두어 container_of 로
 * 서로를 오갈 수 있게 한 것이 관용적인 확장 방식이다. */
struct acpi_pci_generic_root_info {
	struct acpi_pci_root_info	common;
	/* [한국어] 이 루트 브리지의 ECAM 창.
	 * 설정자: pci_acpi_setup_ecam_mapping() 이 MCFG 를 보고 만든다.
	 * 읽는 자: 이 도메인의 모든 config 접근이 bus->sysdata 를 통해
	 *   이 포인터에 닿는다(acpi_pci_bus_find_domain_nr 참고).
	 * 값 범위: NULL 이 아닌 유효한 창. NULL 이면 루트 브리지 등록이 실패한다.
	 * 동기화: 등록 시 한 번 설정되고 해제 시까지 바뀌지 않는다. */
	struct pci_config_window	*cfg;	/* config space mapping */
};

/*
 * acpi_pci_bus_find_domain_nr:
 *   NVMe가 속한 PCI bus의 segment(domain) 번호를 ACPI root 정보에서
 *   반환한다. 멀티 세그먼트 시스템에서 NVMe 장치의 domain 식별에
 *   사용된다.
 */
int acpi_pci_bus_find_domain_nr(struct pci_bus *bus)
{
	struct pci_config_window *cfg = bus->sysdata;
	struct acpi_device *adev = to_acpi_device(cfg->parent);
	struct acpi_pci_root *root = acpi_driver_data(adev);

	return root->segment;
}

/*
 * pcibios_root_bridge_prepare:
 *   ACPI root bridge가 생성되기 전에 ACPI companion과 NUMA node를
 *   설정한다. NVMe가 연결될 root bridge의 ACPI 바인딩 준비.
 */
int pcibios_root_bridge_prepare(struct pci_host_bridge *bridge)
{
	struct pci_config_window *cfg;
	struct acpi_device *adev;
	struct device *bus_dev;

	if (acpi_disabled)
		return 0;

	cfg = bridge->bus->sysdata;

	/*
	 * On Hyper-V there is no corresponding ACPI device for a root bridge,
	 * therefore ->parent is set as NULL by the driver. And set 'adev' as
	 * NULL in this case because there is no proper ACPI device.
	 * NVMe: Hyper-V 등 가상화 환경에서는 ACPI companion이 없을 수 있음.
	 */
	if (!cfg->parent)
		adev = NULL;
	else
		adev = to_acpi_device(cfg->parent);

	bus_dev = &bridge->bus->dev;

	ACPI_COMPANION_SET(&bridge->dev, adev);
	set_dev_node(bus_dev, acpi_get_node(acpi_device_handle(adev)));

	return 0;
}

/*
 * pci_acpi_root_prepare_resources:
 *   ACPI root bridge의 리소스(_CRS)를 probe하고 window만 남긴다.
 *   NVMe BAR가 할당될 PCI memory/IO window가 여기서 결정된다.
 */
static int pci_acpi_root_prepare_resources(struct acpi_pci_root_info *ci)
{
	struct resource_entry *entry, *tmp;
	int status;

	status = acpi_pci_probe_root_resources(ci);
	resource_list_for_each_entry_safe(entry, tmp, &ci->resources) {
		if (!(entry->res->flags & IORESOURCE_WINDOW))
			resource_list_destroy_entry(entry);
	}
	return status;
}

/*
 * Lookup the bus range for the domain in MCFG, and set up config space
 * mapping.
 */
/*
 * [한국어]
 * pci_acpi_setup_ecam_mapping - MCFG 테이블을 보고 ECAM 창을 만든다
 *
 * @root:   ACPI 가 기술한 루트 브리지
 * @return: 만들어진 struct pci_config_window, 실패하면 NULL.
 *
 * MCFG 는 ACPI 테이블 중 하나로, "이 세그먼트의 버스 N~M 번은 물리 주소
 * X 부터 시작하는 ECAM 창으로 접근하라" 는 정보를 담는다. 이 함수가 그것을
 * 조회해 ecam.c 의 pci_ecam_create() 로 실제 매핑을 만든다.
 *
 * ACPI 시스템에서 config 접근이 시작되는 지점이다. 이 매핑이 없으면
 * 어떤 장치의 config space 도 읽을 수 없어 열거 자체가 불가능하다.
 *
 * 버스 범위를 확인하는 이유: ACPI 가 기술한 버스 범위와 MCFG 가 커버하는
 * 범위가 어긋날 수 있다. 그 경우 겹치는 부분만 쓰고 나머지는 포기한다 —
 * 매핑되지 않은 버스에 접근하면 시스템이 죽기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 부팅 중 루트 브리지 등록 시.
 * 호출자: pci_acpi_scan_root().
 */
static struct pci_config_window *
pci_acpi_setup_ecam_mapping(struct acpi_pci_root *root)
{
	struct device *dev = &root->device->dev;
	struct resource *bus_res = &root->secondary;
	u16 seg = root->segment;
	const struct pci_ecam_ops *ecam_ops;
	struct resource cfgres;
	struct acpi_device *adev;
	struct pci_config_window *cfg;
	int ret;

	ret = pci_mcfg_lookup(root, &cfgres, &ecam_ops);
	if (ret) {
		dev_err(dev, "%04x:%pR ECAM region not found\n", seg, bus_res);
		return NULL;
	}

	adev = acpi_resource_consumer(&cfgres);
	if (adev)
		dev_info(dev, "ECAM area %pR reserved by %s\n", &cfgres,
			 dev_name(&adev->dev));
	else
		dev_warn(dev, FW_BUG "ECAM area %pR not reserved in ACPI namespace\n",
			 &cfgres);

	cfg = pci_ecam_create(dev, &cfgres, bus_res, ecam_ops);
	if (IS_ERR(cfg)) {
		dev_err(dev, "%04x:%pR error %ld mapping ECAM\n", seg, bus_res,
			PTR_ERR(cfg));
		return NULL;
	}

	return cfg;
}

/* release_info: free resources allocated by init_info */
static void pci_acpi_generic_release_info(struct acpi_pci_root_info *ci)
{
	struct acpi_pci_generic_root_info *ri;

	ri = container_of(ci, struct acpi_pci_generic_root_info, common);
	pci_ecam_free(ri->cfg);
	kfree(ci->ops);
	kfree(ri);
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
	struct acpi_pci_generic_root_info *ri;
	struct pci_bus *bus, *child;
	struct acpi_pci_root_ops *root_ops;
	struct pci_host_bridge *host;

	ri = kzalloc_obj(*ri);
	if (!ri)
		return NULL;

	root_ops = kzalloc_obj(*root_ops);
	if (!root_ops) {
		kfree(ri);
		return NULL;
	}

	ri->cfg = pci_acpi_setup_ecam_mapping(root);
	if (!ri->cfg) {
		kfree(ri);
		kfree(root_ops);
		return NULL;
	}

	root_ops->release_info = pci_acpi_generic_release_info;
	root_ops->prepare_resources = pci_acpi_root_prepare_resources;
	root_ops->pci_ops = (struct pci_ops *)&ri->cfg->ops->pci_ops;
	bus = acpi_pci_root_create(root, root_ops, &ri->common, ri->cfg);
	if (!bus)
		return NULL;

	/* If we must preserve the resource configuration, claim now */
	host = pci_find_host_bridge(bus);
	if (host->preserve_config)
		pci_bus_claim_resources(bus);

	/*
	 * Assign whatever was left unassigned. If we didn't claim above,
	 * this will reassign everything.
	 * NVMe: 할당되지 않은 BAR 등 리소스를 재할당. NVMe BAR0 포함.
	 */
	pci_assign_unassigned_root_bus_resources(bus);

	list_for_each_entry(child, &bus->children, node)
		pcie_bus_configure_settings(child);

	return bus;
}

/*
 * pcibios_add_bus:
 *   ARM64/RISC-V에서 PCI bus 추가 시 ACPI bus 등록을 위한 wrapper.
 *   NVMe bus가 ACPI namespace에 추가될 때 호출.
 */
void pcibios_add_bus(struct pci_bus *bus)
{
	acpi_pci_add_bus(bus);
}

/*
 * pcibios_remove_bus:
 *   ARM64/RISC-V에서 PCI bus 제거 시 ACPI bus 정리를 위한 wrapper.
 *   NVMe bus 제거 시 호출.
 */
void pcibios_remove_bus(struct pci_bus *bus)
{
	acpi_pci_remove_bus(bus);
}

#endif
