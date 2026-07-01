// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Resizable BAR Extended Capability handling.
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/rebar.c)은 PCI Resizable BAR Extended Capability를
 * 탐지/읽기/쓰기하여 BAR(Base Address Register)의 크기를 런타임에 변경할
 * 수 있게 한다.
 * NVMe SSD 입장에서 BAR는 호스트가 NVMe 컨트롤러 레지스터(특히 BAR0의
 * doorbell, CAP/VS/CC 등)와 선택적 CMB(Controller Memory Buffer)를 MMIO로
 * 접근하는 창이다. ReBAR을 지원하면 다음에 활용 가능하다.
 *   - NVMe BAR0/1 등의 MMIO 영역을 더 큰 크기로 확장(CMB 포함)
 *   - P2PDMA, ATS/PRI, SR-IOV VF BAR 등에서 BAR 크기 조정
 *   - MSI/MSI-X 테이블이 포함된 BAR의 크기 조정
 *   - AER/DPC 등 PCIe 포트 서비스로 인한 재구성 시 BAR 복원
 * 일반적인 NVMe 드라이버 관련 호출 경로:
 *   nvme_probe -> pci_enable_device -> pci_request_regions -> pci_iomap
 *   -> (필요시) pci_resize_resource / pci_rebar_get_possible_sizes
 * 본 파일의 함수들은 PCI 장치 초기화, sysfs resize, VF BAR 설정,
 * pci_restore_rebar_state()를 통한 suspend/resume 시 복원 등에서 사용된다.
 * ===================================================================
 */

#include <linux/bits.h> /* NVMe: 비트 마스크 매크로 포함. */
#include <linux/bitfield.h> /* NVMe: PCIe REBAR 레지스터 필드 추출/조립용. */
#include <linux/bitops.h> /* NVMe: ilog2, roundup_pow_of_two 등 비트 연산용. */
#include <linux/errno.h> /* NVMe: -ENOENT, -ENOTSUPP, -EBUSY 등 오류 코드 정의. */
#include <linux/export.h> /* NVMe: EXPORT_SYMBOL_* 매크로 정의. */
#include <linux/ioport.h> /* NVMe: struct resource, resource_set_size() 등 정의. */
#include <linux/log2.h> /* NVMe: log2 관련 매크로 포함. */
#include <linux/pci.h> /* NVMe: struct pci_dev, PCI_EXT_CAP_ID_REBAR 등 정의. */
#include <linux/sizes.h> /* NVMe: SZ_1M, SZ_128T 등 크기 상수 정의. */
#include <linux/types.h> /* NVMe: u32, u64, resource_size_t 등 기본 타입 정의. */

#include "pci.h" /* NVMe: PCI 서브시스템 날부 헤더(PCI_REBAR_* 상수 등). */

#define PCI_REBAR_MIN_SIZE	((resource_size_t)SZ_1M) /* NVMe: ReBAR 최소 단위는 1MiB(인코딩 0). */

/**
 * pci_rebar_bytes_to_size - Convert size in bytes to PCI BAR Size
 * @bytes: size in bytes
 *
 * Convert size in bytes to encoded BAR Size in Resizable BAR Capability
 * (PCIe r6.2, sec. 7.8.6.3).
 *
 * Return: encoded BAR Size as defined in the PCIe spec (0=1MB, 31=128TB)
 */
/*
 * pci_rebar_bytes_to_size:
 *   NVMe BAR(예: BAR0 doorbell/CMB)의 실제 바이트 크기를 PCIe ReBAR 규격의
 *   인코딩 값(0=1MB, 31=128TB)으로 변환한다. sysfs나 드라이버가 ReBAR 크기를
 *   설정할 때 먼저 호출된다.
 */
int pci_rebar_bytes_to_size(u64 bytes)
{
	int rebar_minsize = ilog2(PCI_REBAR_MIN_SIZE); /* NVMe: 최소 1MiB에 대한 log2 값(20)을 계산. */

	bytes = roundup_pow_of_two(bytes); /* NVMe: NVMe BAR 크기를 2의 거듭제곱으로 올림(ReBAR은 2^n 단위만 지원). */

	return max(ilog2(bytes), rebar_minsize) - rebar_minsize; /* NVMe: 인코딩 값 = (요청 크기 log2 - 최소 log2), 예: 2MB -> (21-20)=1. */
}
EXPORT_SYMBOL_GPL(pci_rebar_bytes_to_size); /* NVMe: 외부 모듈(NVMe 포함)에서 사용 가능하도록 심볼 익스포트. */

/**
 * pci_rebar_size_to_bytes - Convert encoded BAR Size to size in bytes
 * @size: encoded BAR Size as defined in the PCIe spec (0=1MB, 31=128TB)
 *
 * Return: BAR size in bytes
 */
/*
 * pci_rebar_size_to_bytes:
 *   PCIe ReBAR 인코딩 값을 NVMe BAR의 실제 바이트 크기로 환산한다.
 *   예를 들어 인코딩 1은 2MiB, 2는 4MiB 등이다. resource 크기 갱신 시 사용.
 */
resource_size_t pci_rebar_size_to_bytes(int size)
{
	return 1ULL << (size + ilog2(PCI_REBAR_MIN_SIZE)); /* NVMe: 1MiB * 2^size 형태로 NVMe BAR 크기를 바이트로 산출. */
}
EXPORT_SYMBOL_GPL(pci_rebar_size_to_bytes); /* NVMe: NVMe 드라이버 등 외부 모듈에서 호출 가능. */

/*
 * pci_rebar_init:
 *   NVMe 장치가 PCI 버스에서 발견될 때(struct pci_dev 생성 시) ReBAR 확장
 *   캐패빌리티의 config space 오프셋을 찾아 pdev->rebar_cap에 저장한다.
 *   이후 모든 ReBAR 연산이 이 오프셋을 사용한다.
 */
void pci_rebar_init(struct pci_dev *pdev)
{
	pdev->rebar_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_REBAR); /* NVMe: NVMe SSD의 PCIe config space에서 Resizable BAR 확장 capability 위치 탐색. */
}

/**
 * pci_rebar_find_pos - find position of resize control reg for BAR
 * @pdev: PCI device
 * @bar: BAR to find
 *
 * Helper to find the position of the control register for a BAR.
 *
 * Return:
 * * %-ENOTSUPP if resizable BARs are not supported at all,
 * * %-ENOENT if no control register for the BAR could be found.
 */
/*
 * pci_rebar_find_pos:
 *   NVMe 장치의 특정 BAR(예: BAR0 doorbell, CMB BAR, 또는 VF BAR)에 대한
 *   ReBAR control 레지스터의 config space 위치를 반환한다. SR-IOV VF BAR의
 *   경우 VF ReBAR capability를 별도로 조회한다.
 */
static int pci_rebar_find_pos(struct pci_dev *pdev, int bar)
{
	unsigned int pos, nbars, i; /* NVMe: pos=config space 오프셋, nbars=ReBAR 컨트롤 개수, i=루프 인덱스. */
	u32 ctrl; /* NVMe: ReBAR control 레지스터 값을 담을 임시 변수. */

	if (pci_resource_is_iov(bar)) { /* NVMe: 대상 BAR가 SR-IOV VF 리소스인지 확인. */
		pos = pci_iov_vf_rebar_cap(pdev); /* NVMe: VF의 ReBAR capability 오프셋을 가져온다. */
		bar = pci_resource_num_to_vf_bar(bar); /* NVMe: 전체 리소스 번호를 VF 내의 BAR 인덱스로 변환. */
	} else { /* NVMe: 물리적 PF BAR인 경우. */
		pos = pdev->rebar_cap; /* NVMe: probe 시 저장한 PF ReBAR capability 오프셋 사용. */
	}

	if (!pos) /* NVMe: ReBAR capability가 없으면. */
		return -ENOTSUPP; /* NVMe: 해당 NVMe 장치/바는 ReBAR을 지원하지 않음을 알린다. */

	pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl); /* NVMe: ReBAR control 레지스터를 읽어 총 BAR 개수 획득. */
	nbars = FIELD_GET(PCI_REBAR_CTRL_NBAR_MASK, ctrl); /* NVMe: control 레지스터의 NBAR 필드로 등록된 BAR 수 추출. */

	for (i = 0; i < nbars; i++, pos += 8) { /* NVMe: 각 ReBAR 항목은 8바이트(cap+ctrl) 단위로 순회. */
		int bar_idx; /* NVMe: 현재 항목이 서술하는 BAR 인덱스. */

		pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl); /* NVMe: 현재 항목의 control 레지스터를 읽는다. */
		bar_idx = FIELD_GET(PCI_REBAR_CTRL_BAR_IDX, ctrl); /* NVMe: control 레지스터에서 BAR 인덱스 필드 추출. */
		if (bar_idx == bar) /* NVMe: 찾고자 하는 NVMe BAR와 일치하면. */
			return pos; /* NVMe: 해당 BAR의 ReBAR 레지스터 오프셋 반환. */
	}

	return -ENOENT; /* NVMe: 끝까지 못 찾으면 해당 BAR는 ReBAR을 지원하지 않음. */
}

/**
 * pci_rebar_get_possible_sizes - get possible sizes for Resizable BAR
 * @pdev: PCI device
 * @bar: BAR to query
 *
 * Get the possible sizes of a resizable BAR as bitmask.
 *
 * Return: A bitmask of possible sizes (bit 0=1MB, bit 31=128TB), or %0 if
 *	   BAR isn't resizable.
 */
/*
 * pci_rebar_get_possible_sizes:
 *   NVMe BAR가 지원할 수 있는 모든 크기를 비트마스크로 반환한다. 각 비트는
 *   1MiB부터 128TiB까지 2의 거듭제곱 크기를 의미한다. 이 값은 resize 가능
 *   여부 판단과 sysfs 노출에 사용된다.
 */
u64 pci_rebar_get_possible_sizes(struct pci_dev *pdev, int bar)
{
	int pos; /* NVMe: 대상 BAR의 ReBAR capability 오프셋. */
	u32 cap; /* NVMe: ReBAR capability 레지스터 값. */

	pos = pci_rebar_find_pos(pdev, bar); /* NVMe: NVMe BAR의 ReBAR 레지스터 위치 탐색. */
	if (pos < 0) /* NVMe: ReBAR을 지원하지 않거나 위치를 찾지 못한 경우. */
		return 0; /* NVMe: 가능 크기 집합이 비어 있음(0). */

	pci_read_config_dword(pdev, pos + PCI_REBAR_CAP, &cap); /* NVMe: capability 레지스터를 읽어 지원 크기 비트마스크 획득. */
	cap = FIELD_GET(PCI_REBAR_CAP_SIZES, cap); /* NVMe: SIZES 필드만 추출한다. */

	/* Sapphire RX 5600 XT Pulse has an invalid cap dword for BAR 0 */
	if (pdev->vendor == PCI_VENDOR_ID_ATI && pdev->device == 0x731f && /* NVMe: 특정 GPU의 잘못된 capability 워드에 대한 워크어라운드 조건. */
	    bar == 0 && cap == 0x700) /* NVMe: 조건이 모두 만족하면. */
		return 0x3f00; /* NVMe: 하드코딩된 올바른 가능 크기 마스크 반환. */

	return cap; /* NVMe: NVMe BAR가 지원하는 크기 비트마스크 반환. */
}
EXPORT_SYMBOL(pci_rebar_get_possible_sizes); /* NVMe: NVMe 드라이버 및 pciutils 등에서 사용 가능. */

/**
 * pci_rebar_size_supported - check if size is supported for BAR
 * @pdev: PCI device
 * @bar: BAR to check
 * @size: encoded size as defined in the PCIe spec (0=1MB, 31=128TB)
 *
 * Return: %true if @bar is resizable and @size is supported, otherwise
 *	   %false.
 */
/*
 * pci_rebar_size_supported:
 *   NVMe BAR가 지정한 인코딩 크기를 지원하는지 확인한다. resize 요청이
 *   유효한 범위 내에 있는지 먼저 검사한 뒤, capability 비트마스크에서
 *   해당 비트가 설정되어 있는지 본다.
 */
bool pci_rebar_size_supported(struct pci_dev *pdev, int bar, int size)
{
	u64 sizes = pci_rebar_get_possible_sizes(pdev, bar); /* NVMe: 해당 NVMe BAR의 지원 크기 집합 획득. */

	if (size < 0 || size > ilog2(SZ_128T) - ilog2(PCI_REBAR_MIN_SIZE)) /* NVMe: 요청 인코딩이 0~31 범위를 벗어나는지 검사. */
		return false; /* NVMe: 범위를 벗어나면 지원 불가. */

	return BIT(size) & sizes; /* NVMe: 요청 크기에 해당하는 비트가 capability 마스크에 있는지 반환. */
}
EXPORT_SYMBOL_GPL(pci_rebar_size_supported); /* NVMe: 외부 모듈에서 사용 가능. */

/**
 * pci_rebar_get_max_size - get the maximum supported size of a BAR
 * @pdev: PCI device
 * @bar: BAR to query
 *
 * Get the largest supported size of a resizable BAR as a size.
 *
 * Return: the encoded maximum BAR size as defined in the PCIe spec
 *	   (0=1MB, 31=128TB), or %-NOENT on error.
 */
/*
 * pci_rebar_get_max_size:
 *   NVMe BAR가 지원하는 최대 크기의 인코딩 값을 반환한다. 예를 들어 NVMe
 *   CMB를 위한 BAR를 최대로 확장하고자 할 때 사용할 수 있다.
 */
int pci_rebar_get_max_size(struct pci_dev *pdev, int bar)
{
	u64 sizes; /* NVMe: 지원 크기 비트마스크. */

	sizes = pci_rebar_get_possible_sizes(pdev, bar); /* NVMe: NVMe BAR의 가능 크기 마스크 획득. */
	if (!sizes) /* NVMe: 마스크가 0이면 ReBAR을 지원하지 않음. */
		return -ENOENT; /* NVMe: 오류 코드 반환. */

	return __fls(sizes); /* NVMe: 가장 높은 비트 위치를 반환(최대 크기 인코딩). */
}
EXPORT_SYMBOL_GPL(pci_rebar_get_max_size); /* NVMe: 외부 모듈에서 호출 가능. */

/**
 * pci_rebar_get_current_size - get the current size of a Resizable BAR
 * @pdev: PCI device
 * @bar: BAR to get the size from
 *
 * Read the current size of a BAR from the Resizable BAR config.
 *
 * Return: BAR Size if @bar is resizable (0=1MB, 31=128TB), or negative on
 *         error.
 */
/*
 * pci_rebar_get_current_size:
 *   NVMe BAR의 현재 설정된 ReBAR 인코딩 값을 읽는다. 현재 MMIO로 매핑된
 *   NVMe BAR(예: doorbell/CMB)의 실제 크기를 파악할 때 사용된다.
 */
int pci_rebar_get_current_size(struct pci_dev *pdev, int bar)
{
	int pos; /* NVMe: ReBAR 레지스터 오프셋. */
	u32 ctrl; /* NVMe: ReBAR control 레지스터 값. */

	pos = pci_rebar_find_pos(pdev, bar); /* NVMe: NVMe BAR의 control 레지스터 위치 탐색. */
	if (pos < 0) /* NVMe: 탐색 실패 시. */
		return pos; /* NVMe: 음수 오류 코드 그대로 반환. */

	pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl); /* NVMe: control 레지스터 읽기. */
	return FIELD_GET(PCI_REBAR_CTRL_BAR_SIZE, ctrl); /* NVMe: BAR_SIZE 필드 추출하여 현재 크기 인코딩 반환. */
}

/**
 * pci_rebar_set_size - set a new size for a Resizable BAR
 * @pdev: PCI device
 * @bar: BAR to set size to
 * @size: new size as defined in the PCIe spec (0=1MB, 31=128TB)
 *
 * Set the new size of a BAR as defined in the spec.
 *
 * Return: %0 if resizing was successful, or negative on error.
 */
/*
 * pci_rebar_set_size:
 *   NVMe BAR의 ReBAR control 레지스터에 새 크기 인코딩을 쓴다. BAR 리소스
 *   할당 알고리즘 재수행 전/후에 호출되며, VF BAR인 경우 VF 리소스 크기도
 *   함께 갱신한다.
 */
int pci_rebar_set_size(struct pci_dev *pdev, int bar, int size)
{
	int pos; /* NVMe: 대상 BAR의 ReBAR control 레지스터 오프셋. */
	u32 ctrl; /* NVMe: ReBAR control 레지스터 값. */

	pos = pci_rebar_find_pos(pdev, bar); /* NVMe: NVMe BAR의 control 레지스터 위치 탐색. */
	if (pos < 0) /* NVMe: 위치를 찾지 못하면. */
		return pos; /* NVMe: 오류 코드 반환. */

	pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl); /* NVMe: 현재 control 레지스터 값 읽기. */
	ctrl &= ~PCI_REBAR_CTRL_BAR_SIZE; /* NVMe: BAR_SIZE 필드 클리어. */
	ctrl |= FIELD_PREP(PCI_REBAR_CTRL_BAR_SIZE, size); /* NVMe: 새 크기 인코딩을 BAR_SIZE 필드에 기록. */
	pci_write_config_dword(pdev, pos + PCI_REBAR_CTRL, ctrl); /* NVMe: 변경된 control 레지스터를 PCIe config space에 쓴다. */

	if (pci_resource_is_iov(bar)) /* NVMe: 설정 대상이 SR-IOV VF BAR인 경우. */
		pci_iov_resource_set_size(pdev, bar, size); /* NVMe: VF 리소스 크기도 동일하게 갱신. */

	return 0; /* NVMe: ReBAR 크기 설정 성공. */
}

/*
 * pci_restore_rebar_state:
 *   NVMe 장치의 suspend/resume, AER/DPC 등으로 인한 PCIe 포트 서비스 복구
 *   후, kernel이 관리하는 resource 크기와 일치하도록 모든 ReBAR control
 *   레지스터를 복원한다. BAR 크기가 resume 시 firmware에 의해 바뀌었을
 *   가능성을 방어한다.
 */
void pci_restore_rebar_state(struct pci_dev *pdev)
{
	unsigned int pos, nbars, i; /* NVMe: pos=오프셋, nbars=BAR 수, i=순회 인덱스. */
	u32 ctrl; /* NVMe: ReBAR control 레지스터 값. */

	pos = pdev->rebar_cap; /* NVMe: PF의 ReBAR capability 오프셋 획득. */
	if (!pos) /* NVMe: PF가 ReBAR을 지원하지 않으면. */
		return; /* NVMe: 복원할 것이 없으므로 즉시 반환. */

	pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl); /* NVMe: control 레지스터 읽어 총 BAR 개수 확인. */
	nbars = FIELD_GET(PCI_REBAR_CTRL_NBAR_MASK, ctrl); /* NVMe: NBAR 필드로 등록된 BAR 수 추출. */

	for (i = 0; i < nbars; i++, pos += 8) { /* NVMe: 각 ReBAR 항목(8바이트)을 순회하며 복원. */
		struct resource *res; /* NVMe: 현재 BAR에 해당하는 kernel resource 포인터. */
		int bar_idx, size; /* NVMe: bar_idx=BAR 번호, size=복원할 인코딩 크기. */

		pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl); /* NVMe: 현재 항목의 control 레지스터 읽기. */
		bar_idx = ctrl & PCI_REBAR_CTRL_BAR_IDX; /* NVMe: BAR 인덱스 필드 획득. */
		res = pci_resource_n(pdev, bar_idx); /* NVMe: 해당 BAR의 struct resource 획득. */
		size = pci_rebar_bytes_to_size(resource_size(res)); /* NVMe: kernel resource 크기를 ReBAR 인코딩으로 변환. */
		ctrl &= ~PCI_REBAR_CTRL_BAR_SIZE; /* NVMe: 기존 BAR_SIZE 필드 클리어. */
		ctrl |= FIELD_PREP(PCI_REBAR_CTRL_BAR_SIZE, size); /* NVMe: kernel resource 크기에 맞춰 BAR_SIZE 필드 설정. */
		pci_write_config_dword(pdev, pos + PCI_REBAR_CTRL, ctrl); /* NVMe: 복원된 control 레지스터를 config space에 기록. */
	}
}

/*
 * pci_resize_is_memory_decoding_enabled:
 *   NVMe BAR resize 전에 해당 BAR가 속한 메모리 공간의 decoding이 켜져
 *   있는지 확인한다. Memory decoding이 활성화된 상태에서 BAR 크기를 바꾸면
 *   버스/리소스 일관성이 깨질 수 있으므로 resize는 거부해야 한다.
 */
static bool pci_resize_is_memory_decoding_enabled(struct pci_dev *dev,
						  int resno)
{
	u16 cmd; /* NVMe: PCI Command 레지스터 값. */

	if (pci_resource_is_iov(resno)) /* NVMe: 대상 리소스가 SR-IOV VF BAR인지 확인. */
		return pci_iov_is_memory_decoding_enabled(dev); /* NVMe: VF의 메모리 decoding 상태 반환. */

	pci_read_config_word(dev, PCI_COMMAND, &cmd); /* NVMe: NVMe 장치의 PCI Command 레지스터 읽기. */

	return cmd & PCI_COMMAND_MEMORY; /* NVMe: Memory Space Enable 비트가 1이면 true 반환. */
}

/*
 * pci_resize_resource_set_size:
 *   NVMe BAR resource의 struct resource 크기를 새 ReBAR 인코딩에 맞게
 *   갱신한다. VF BAR인 경우 총 VF 수를 곱하여 전체 VF 리소스 크기를
 *   반영한다.
 */
void pci_resize_resource_set_size(struct pci_dev *dev, int resno, int size)
{
	resource_size_t res_size = pci_rebar_size_to_bytes(size); /* NVMe: 인코딩 값을 바이트 크기로 변환. */
	struct resource *res = pci_resource_n(dev, resno); /* NVMe: 대상 NVMe BAR의 struct resource 획득. */

	if (pci_resource_is_iov(resno)) /* NVMe: SR-IOV VF BAR인 경우. */
		res_size *= pci_sriov_get_totalvfs(dev); /* NVMe: 전체 VF 수를 곱해 모든 VF가 사용할 크기 산출. */

	resource_set_size(res, res_size); /* NVMe: kernel resource의 크기 필드를 갱신. */
}

/**
 * pci_resize_resource - reconfigure a Resizable BAR and resources
 * @dev: the PCI device
 * @resno: index of the BAR to be resized
 * @size: new size as defined in the spec (0=1MB, 31=128TB)
 * @exclude_bars: a mask of BARs that should not be released
 *
 * Reconfigure @resno to @size and re-run resource assignment algorithm
 * with the new size.
 *
 * Prior to resize, release @dev resources that share a bridge window with
 * @resno.  This unpins the bridge window resource to allow changing it.
 *
 * The caller may prevent releasing a particular BAR by providing
 * @exclude_bars mask, but this may result in the resize operation failing
 * due to insufficient space.
 *
 * Return: 0 on success, or negative on error. In case of an error, the
 *         resources are restored to their original places.
 */
/*
 * pci_resize_resource:
 *   NVMe 장치의 특정 BAR 크기를 런타임에 변경한다. NVMe 드라이버나 sysfs
 *   resize 인터페이스를 통해 호출될 수 있으며, BAR가 현재 MMIO decoding 중이면
 *   -EBUSY를 반환한다. 성공 시 리소스 할당 알고리즘을 재수행하여 bridge
 *   window를 새 크기에 맞게 조정한다. SR-IOV VF BAR, CMB BAR, MSI-X table
 *   BAR 등의 동적 크기 조정에 활용될 수 있다.
 */
int pci_resize_resource(struct pci_dev *dev, int resno, int size,
			int exclude_bars)
{
	struct pci_host_bridge *host; /* NVMe: NVMe 장치가 연결된 host bridge 포인터. */

	/* Check if we must preserve the firmware's resource assignment */
	host = pci_find_host_bridge(dev->bus); /* NVMe: NVMe 장치가 연결된 host bridge 획득. */
	if (host->preserve_config) /* NVMe: firmware 할당을 유지해야 하는 host bridge면. */
		return -ENOTSUPP; /* NVMe: resize를 허용하지 않음. */

	if (pci_resize_is_memory_decoding_enabled(dev, resno)) /* NVMe: NVMe BAR의 memory decoding이 켜져 있는지 확인. */
		return -EBUSY; /* NVMe: decoding 중이면 resize 불가(드라이버가 pci_iounmap 후 시도해야 함). */

	if (!pci_rebar_size_supported(dev, resno, size)) /* NVMe: 요청한 크기가 NVMe BAR에서 지원되는지 확인. */
		return -EINVAL; /* NVMe: 지원되지 않는 크기이면 오류 반환. */

	return pci_do_resource_release_and_resize(dev, resno, size, exclude_bars); /* NVMe: 기존 리소스 해제 후 새 크기로 재할당 수행. */
}
EXPORT_SYMBOL(pci_resize_resource); /* NVMe: NVMe 드라이버 및 pci resize sysfs 인터페이스에서 사용 가능. */
