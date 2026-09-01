// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Resizable BAR Extended Capability handling.
 */

/*
 * [한국어 설명] BAR 의 크기를 소프트웨어가 바꾸는 기능 (rebar.c)
 *
 * === 파일의 역할 ===
 * 보통 BAR 의 크기는 하드웨어가 정해 놓은 고정값이다. 크기를 알아내는
 * 방법도 "all-ones 를 써 넣고 되읽어 무시된 하위 비트를 세는" 것이라,
 * 크기 자체가 하드웨어의 성질이다.
 *
 * Resizable BAR(ReBAR)는 그것을 바꿀 수 있게 한 확장 capability 다.
 * 장치가 "나는 1MB, 2MB, ... 8GB 중 아무거나 될 수 있다" 고 지원 크기
 * 목록을 밝히고, 소프트웨어가 그중 하나를 골라 설정한다.
 *
 * 왜 필요한가. 대표적인 예가 GPU 다. 예전에는 VRAM 이 커도 BAR 는
 * 256MB 로 고정이라 CPU 가 한 번에 그만큼만 볼 수 있었고, 나머지는
 * 창을 옮겨 가며 접근해야 했다. ReBAR 로 BAR 를 VRAM 전체 크기로
 * 키우면 그 번거로움이 사라진다.
 *
 * 대가는 주소 공간이다. BAR 를 키우면 그만큼 넓은 연속 구간이 필요하고,
 * 32비트 주소 공간에서는 자리가 없을 수 있다. 그래서 크기를 바꾼 뒤에는
 * 자원을 재배치해야 하며, 그 과정이 실패하면 원래 크기로 되돌린다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 조회: 드라이버 또는 sysfs
 *         -> [이 파일] pci_rebar_get_possible_sizes() — 지원 크기 비트맵
 *
 * 변경: 드라이버 또는 sysfs 의 resource<N>_resize
 *         -> [이 파일] pci_resize_resource(pdev, bar, size)
 *            -> 자원을 놓고 -> BAR Control 에 새 크기를 쓰고
 *            -> struct resource 의 크기를 갱신하고 UNSET 표시
 *            -> 상위 브리지 윈도우를 다시 계산하도록 요청한다
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. config 접근과 자원 조작이 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: GPU 드라이버(amdgpu, i915 등), pci-sysfs.c 의 resize 속성.
 * 아래쪽: access.c 의 확장 capability 접근, setup-res.c 의 자원 해제,
 *   setup-bus.c 의 재배치.
 * 공유 상태: struct pci_dev 의 resource[] 크기와 IORESOURCE_UNSET 플래그.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 쓰지 않는다(전수 확인, "pci_rebar" 0건).
 *
 * NVMe 의 BAR0 는 컨트롤러 레지스터와 도어벨 배열을 담는데, 그 크기가
 * 큐 개수에 따라 정해지긴 하지만 하드웨어가 결정하는 값이고 소프트웨어가
 * 키울 이유가 없다. CMB 를 담는 BAR 도 컨트롤러가 가진 메모리 크기라
 * 마찬가지다.
 *
 * ReBAR 가 의미 있으려면 "장치 안에 큰 메모리가 있는데 창이 작아서
 * 다 못 본다" 는 상황이어야 하는데, NVMe 는 데이터를 창으로 들여다보는
 * 것이 아니라 DMA 로 주고받으므로 그런 상황이 생기지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_rebar_find_pos()          : ReBAR capability 안에서 이 BAR 에 해당하는
 *                                 항목의 위치를 찾는다. 항목 순서가 BAR
 *                                 번호와 일치하지 않을 수 있어 필요하다.
 * pci_rebar_get_possible_sizes(): 지원 크기 비트맵. 비트 n 이 1이면
 *                                 2^n MB 를 지원한다는 뜻이다.
 * pci_rebar_get_current_size()  : 지금 설정된 크기(같은 인코딩).
 * pci_rebar_set_size()          : BAR Control 에 새 크기를 쓴다.
 * pci_resize_resource()         : 크기 변경의 진입점. 자원을 놓고, 크기를
 *                                 바꾸고, 재배치가 필요하다고 표시한다.
 * pci_resize_is_memory_decoding_enabled() : 메모리 디코딩이 켜져 있으면
 *                                 크기를 바꿀 수 없으므로 먼저 확인한다.
 * pci_resize_resource_set_size() : 크기 변경 뒤 자원 구조체를 갱신한다.
 * 상위 브리지 윈도우 재계산은 이 파일에 없다. pci_resize_resource() 가
 * setup-bus.c:6541 의 pci_do_resource_release_and_resize() 로 넘긴다.
 */

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/ioport.h>
#include <linux/log2.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/types.h>

#include "pci.h"

#define PCI_REBAR_MIN_SIZE	((resource_size_t)SZ_1M)

/**
 * pci_rebar_bytes_to_size - Convert size in bytes to PCI BAR Size
 * @bytes: size in bytes
 *
 * Convert size in bytes to encoded BAR Size in Resizable BAR Capability
 * (PCIe r6.2, sec. 7.8.6.3).
 *
 * Return: encoded BAR Size as defined in the PCIe spec (0=1MB, 31=128TB)
 */
int pci_rebar_bytes_to_size(u64 bytes)
{
	int rebar_minsize = ilog2(PCI_REBAR_MIN_SIZE);

	bytes = roundup_pow_of_two(bytes);

	return max(ilog2(bytes), rebar_minsize) - rebar_minsize;
}
EXPORT_SYMBOL_GPL(pci_rebar_bytes_to_size);

/**
 * pci_rebar_size_to_bytes - Convert encoded BAR Size to size in bytes
 * @size: encoded BAR Size as defined in the PCIe spec (0=1MB, 31=128TB)
 *
 * Return: BAR size in bytes
 */
resource_size_t pci_rebar_size_to_bytes(int size)
{
	return 1ULL << (size + ilog2(PCI_REBAR_MIN_SIZE));
}
EXPORT_SYMBOL_GPL(pci_rebar_size_to_bytes);

void pci_rebar_init(struct pci_dev *pdev)
{
	pdev->rebar_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_REBAR);
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
static int pci_rebar_find_pos(struct pci_dev *pdev, int bar)
{
	unsigned int pos, nbars, i;
	u32 ctrl;

	if (pci_resource_is_iov(bar)) {
		pos = pci_iov_vf_rebar_cap(pdev);
		bar = pci_resource_num_to_vf_bar(bar);
	} else {
		pos = pdev->rebar_cap;
	}

	if (!pos)
		return -ENOTSUPP;

	pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
	nbars = FIELD_GET(PCI_REBAR_CTRL_NBAR_MASK, ctrl);

	for (i = 0; i < nbars; i++, pos += 8) {
		int bar_idx;

		pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
		bar_idx = FIELD_GET(PCI_REBAR_CTRL_BAR_IDX, ctrl);
		if (bar_idx == bar)
			return pos;
	}

	return -ENOENT;
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
u64 pci_rebar_get_possible_sizes(struct pci_dev *pdev, int bar)
{
	int pos;
	u32 cap;

	pos = pci_rebar_find_pos(pdev, bar);
	if (pos < 0)
		return 0;

	pci_read_config_dword(pdev, pos + PCI_REBAR_CAP, &cap);
	cap = FIELD_GET(PCI_REBAR_CAP_SIZES, cap);

	/* Sapphire RX 5600 XT Pulse has an invalid cap dword for BAR 0 */
	if (pdev->vendor == PCI_VENDOR_ID_ATI && pdev->device == 0x731f &&
	    bar == 0 && cap == 0x700)
		return 0x3f00;

	return cap;
}
EXPORT_SYMBOL(pci_rebar_get_possible_sizes);

/**
 * pci_rebar_size_supported - check if size is supported for BAR
 * @pdev: PCI device
 * @bar: BAR to check
 * @size: encoded size as defined in the PCIe spec (0=1MB, 31=128TB)
 *
 * Return: %true if @bar is resizable and @size is supported, otherwise
 *	   %false.
 */
bool pci_rebar_size_supported(struct pci_dev *pdev, int bar, int size)
{
	u64 sizes = pci_rebar_get_possible_sizes(pdev, bar);

	if (size < 0 || size > ilog2(SZ_128T) - ilog2(PCI_REBAR_MIN_SIZE))
		return false;

	return BIT(size) & sizes;
}
EXPORT_SYMBOL_GPL(pci_rebar_size_supported);

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
int pci_rebar_get_max_size(struct pci_dev *pdev, int bar)
{
	u64 sizes;

	sizes = pci_rebar_get_possible_sizes(pdev, bar);
	if (!sizes)
		return -ENOENT;

	return __fls(sizes);
}
EXPORT_SYMBOL_GPL(pci_rebar_get_max_size);

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
int pci_rebar_get_current_size(struct pci_dev *pdev, int bar)
{
	int pos;
	u32 ctrl;

	pos = pci_rebar_find_pos(pdev, bar);
	if (pos < 0)
		return pos;

	pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
	return FIELD_GET(PCI_REBAR_CTRL_BAR_SIZE, ctrl);
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
int pci_rebar_set_size(struct pci_dev *pdev, int bar, int size)
{
	int pos;
	u32 ctrl;

	pos = pci_rebar_find_pos(pdev, bar);
	if (pos < 0)
		return pos;

	pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
	ctrl &= ~PCI_REBAR_CTRL_BAR_SIZE;
	ctrl |= FIELD_PREP(PCI_REBAR_CTRL_BAR_SIZE, size);
	pci_write_config_dword(pdev, pos + PCI_REBAR_CTRL, ctrl);

	if (pci_resource_is_iov(bar))
		pci_iov_resource_set_size(pdev, bar, size);

	return 0;
}

void pci_restore_rebar_state(struct pci_dev *pdev)
{
	unsigned int pos, nbars, i;
	u32 ctrl;

	pos = pdev->rebar_cap;
	if (!pos)
		return;

	pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
	nbars = FIELD_GET(PCI_REBAR_CTRL_NBAR_MASK, ctrl);

	for (i = 0; i < nbars; i++, pos += 8) {
		struct resource *res;
		int bar_idx, size;

		pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
		bar_idx = ctrl & PCI_REBAR_CTRL_BAR_IDX;
		res = pci_resource_n(pdev, bar_idx);
		size = pci_rebar_bytes_to_size(resource_size(res));
		ctrl &= ~PCI_REBAR_CTRL_BAR_SIZE;
		ctrl |= FIELD_PREP(PCI_REBAR_CTRL_BAR_SIZE, size);
		pci_write_config_dword(pdev, pos + PCI_REBAR_CTRL, ctrl);
	}
}

static bool pci_resize_is_memory_decoding_enabled(struct pci_dev *dev,
						  int resno)
{
	u16 cmd;

	if (pci_resource_is_iov(resno))
		return pci_iov_is_memory_decoding_enabled(dev);

	pci_read_config_word(dev, PCI_COMMAND, &cmd);

	return cmd & PCI_COMMAND_MEMORY;
}

void pci_resize_resource_set_size(struct pci_dev *dev, int resno, int size)
{
	resource_size_t res_size = pci_rebar_size_to_bytes(size);
	struct resource *res = pci_resource_n(dev, resno);

	if (pci_resource_is_iov(resno))
		res_size *= pci_sriov_get_totalvfs(dev);

	resource_set_size(res, res_size);
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
int pci_resize_resource(struct pci_dev *dev, int resno, int size,
			int exclude_bars)
{
	struct pci_host_bridge *host;

	/* Check if we must preserve the firmware's resource assignment */
	host = pci_find_host_bridge(dev->bus);
	if (host->preserve_config)
		return -ENOTSUPP;

	if (pci_resize_is_memory_decoding_enabled(dev, resno))
		return -EBUSY;

	if (!pci_rebar_size_supported(dev, resno, size))
		return -EINVAL;

	return pci_do_resource_release_and_resize(dev, resno, size, exclude_bars);
}
EXPORT_SYMBOL(pci_resize_resource);
