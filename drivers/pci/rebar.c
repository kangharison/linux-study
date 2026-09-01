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

	/* [한국어] 먼저 2의 거듭제곱으로 올린다. BAR 크기는 언제나 2의 거듭제곱이어야 하므로,
	 * 요청이 그렇지 않으면 담을 수 있는 가장 가까운 크기로 키운다. */
	bytes = roundup_pow_of_two(bytes);

	/* [한국어] 인코딩은 1MB 를 0 으로 시작하므로, 로그값에서 1MB 의 로그값을 뺀다.
	 * max() 로 하한을 거는 이유는 1MB 보다 작은 크기를 요청받아도 인코딩이
	 * 음수가 되지 않게 하려는 것이다 — 규격상 그보다 작은 BAR 은 표현할 수 없다. */
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
	/* [한국어] 위 함수의 역변환. 인코딩 값에 1MB 의 로그값을 더해 2의 거듭제곱으로
	 * 되돌린다. 1ULL 로 시작하는 것이 중요한데, 인코딩 31 이면 128TB 라
	 * 32비트로는 표현할 수 없기 때문이다. */
	return 1ULL << (size + ilog2(PCI_REBAR_MIN_SIZE));
}
EXPORT_SYMBOL_GPL(pci_rebar_size_to_bytes);

/* [한국어]
 * pci_rebar_init - Resizable BAR capability 의 위치를 찾아 캐시한다
 *
 * @pdev: 열거 중인 장치.
 *
 * 한 줄짜리 함수지만 이 파일 전체의 전제를 만든다. 여기서 찾아 둔
 * pdev->rebar_cap 이 0 이면 "이 장치는 BAR 크기 조절을 지원하지 않는다" 는
 * 뜻이 되고, 아래 모든 함수가 그 0 을 보고 물러난다.
 *
 * 매번 capability 를 찾지 않고 캐시하는 이유는 pci_find_ext_capability() 가
 * 확장 capability 사슬을 config 읽기로 훑어야 해서 비싸기 때문이다.
 *
 * VF BAR 쪽은 이 캐시를 쓰지 않는다. VF 전용 Resizable BAR capability 가
 * 따로 있어 pci_rebar_find_pos() 가 그때그때 조회한다.
 *
 * 실행 컨텍스트: 장치 열거. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 없으면 0 이 저장될 뿐이고 그것이 정상적인 상태다.
 *
 * 호출 체인:
 *   pci_setup_device() 계열 → [이 함수] → pci_find_ext_capability()
 */
void pci_rebar_init(struct pci_dev *pdev)
{
	/* [한국어] Resizable BAR 확장 capability 의 위치를 찾아 캐시해 둔다. 없으면 0 이
	 * 저장되고, 그것이 아래 모든 함수의 "지원하지 않음" 판정 근거가 된다. */
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
	/* [한국어] capability 위치, BAR 개수, 순회 인덱스. */
	unsigned int pos, nbars, i;
	/* [한국어] 제어 레지스터 값. */
	u32 ctrl;

	/* [한국어] SR-IOV 가상 기능용 BAR 이면, */
	if (pci_resource_is_iov(bar)) {
		/* [한국어] VF 전용 Resizable BAR capability 를 쓴다. 물리 기능의 것과 별개의
		 * capability 라 위치가 다르다. */
		pos = pci_iov_vf_rebar_cap(pdev);
		/* [한국어] BAR 번호도 VF 기준으로 바꾼다. 커널이 PF 와 VF 의 BAR 을 한 번호 공간에
		 * 몰아 두기 때문에 되돌려야 한다. */
		bar = pci_resource_num_to_vf_bar(bar);
	} else {
		/* [한국어] 보통은 pci_rebar_init() 이 캐시해 둔 위치를 쓴다. */
		pos = pdev->rebar_cap;
	}

	/* [한국어] capability 가 없으면, */
	if (!pos)
		return -ENOTSUPP;

	/* [한국어] 첫 제어 레지스터를 읽는다. */
	pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
	/* [한국어] 그 안의 NBAR 필드가 이 capability 가 다루는 BAR 의 개수다. */
	nbars = FIELD_GET(PCI_REBAR_CTRL_NBAR_MASK, ctrl);

	/* [한국어] BAR 마다 제어 레지스터가 8바이트씩 떨어져 있어, pos 를 8씩 밀며 순회한다.
	 * i 와 pos 가 함께 전진하는 것이 이 루프의 관용구다. */
	for (i = 0; i < nbars; i++, pos += 8) {
		/* [한국어] 이 항목이 가리키는 BAR 번호. */
		int bar_idx;

		/* [한국어] 항목의 제어 레지스터를 읽고, */
		pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
		/* [한국어] BAR 인덱스 필드를 꺼낸다. 항목의 순서가 곧 BAR 번호는 아니므로
		 * 이 필드로 확인해야 한다. */
		bar_idx = FIELD_GET(PCI_REBAR_CTRL_BAR_IDX, ctrl);
		/* [한국어] 찾던 BAR 이면, */
		if (bar_idx == bar)
			/* [한국어] 그 항목의 위치를 돌려준다. */
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
	/* [한국어] 제어 레지스터 위치. */
	int pos;
	/* [한국어] capability 레지스터 값. */
	u32 cap;

	/* [한국어] 이 BAR 의 항목을 찾는다. */
	pos = pci_rebar_find_pos(pdev, bar);
	/* [한국어] 없으면(지원하지 않거나 그 BAR 은 크기 조절 대상이 아니면), */
	if (pos < 0)
		return 0;

	/* [한국어] capability 레지스터를 읽고, */
	pci_read_config_dword(pdev, pos + PCI_REBAR_CAP, &cap);
	/* [한국어] 지원 크기 비트맵 필드를 꺼낸다. 비트 n 이 1 이면 인코딩 n 의 크기를
	 * 지원한다는 뜻이다. */
	cap = FIELD_GET(PCI_REBAR_CAP_SIZES, cap);

	/* Sapphire RX 5600 XT Pulse has an invalid cap dword for BAR 0 */
	if (pdev->vendor == PCI_VENDOR_ID_ATI && pdev->device == 0x731f &&
	    bar == 0 && cap == 0x700)
		/* [한국어] 옆의 영어 주석대로 이 칩은 capability 값을 잘못 보고하므로, 알려진
		 * 올바른 값으로 바꿔 준다. 벤더·장치 ID 와 BAR 번호, 그리고 잘못된 값까지
		 * 네 조건을 모두 확인해 다른 장치에 영향이 가지 않게 한다. */
		return 0x3f00;

	/* [한국어] 그 밖에는 하드웨어가 보고한 값을 그대로 쓴다. */
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
	/* [한국어] 지원 크기 비트맵을 얻는다. */
	u64 sizes = pci_rebar_get_possible_sizes(pdev, bar);

	/* [한국어] 인코딩 범위를 벗어나면(음수이거나 128TB 를 넘으면), */
	if (size < 0 || size > ilog2(SZ_128T) - ilog2(PCI_REBAR_MIN_SIZE))
		return false;

	/* [한국어] 해당 비트가 서 있는지 본다. 반환값이 bool 이 아니라 그 비트 자체이므로
	 * 0 이 아니면 지원한다는 뜻이다. */
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
	/* [한국어] 지원 크기 비트맵. */
	u64 sizes;

	/* [한국어] 비트맵을 얻고, */
	sizes = pci_rebar_get_possible_sizes(pdev, bar);
	/* [한국어] 하나도 없으면, */
	if (!sizes)
		return -ENOENT;

	/* [한국어] 가장 높은 비트의 위치가 곧 최대 크기의 인코딩이다. */
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
	/* [한국어] 제어 레지스터 위치. */
	int pos;
	/* [한국어] 제어 레지스터 값. */
	u32 ctrl;

	/* [한국어] 이 BAR 의 항목을 찾는다. */
	pos = pci_rebar_find_pos(pdev, bar);
	/* [한국어] 없으면, */
	if (pos < 0)
		/* [한국어] 그 오류를 그대로 올려보낸다. 위 두 함수가 0 으로 뭉갠 것과 달리
		 * 여기서는 -ENOTSUPP 과 -ENOENT 를 구분해 전달한다. */
		return pos;

	/* [한국어] 제어 레지스터를 읽고, */
	pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
	/* [한국어] 현재 크기 필드를 꺼낸다. */
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
	/* [한국어] 제어 레지스터 위치. */
	int pos;
	/* [한국어] 제어 레지스터 값. */
	u32 ctrl;

	/* [한국어] 이 BAR 의 항목을 찾는다. */
	pos = pci_rebar_find_pos(pdev, bar);
	/* [한국어] 없으면, */
	if (pos < 0)
		/* [한국어] 오류를 올려보낸다. */
		return pos;

	/* [한국어] 현재 값을 읽는다. 다른 필드를 보존해야 하므로 읽기-수정-쓰기다. */
	pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
	/* [한국어] 크기 필드를 지우고, */
	ctrl &= ~PCI_REBAR_CTRL_BAR_SIZE;
	/* [한국어] 새 값을 그 자리에 넣는다. */
	ctrl |= FIELD_PREP(PCI_REBAR_CTRL_BAR_SIZE, size);
	/* [한국어] 되쓴다. 이 쓰기로 BAR 의 크기가 실제로 바뀌며, 그 순간 이전 주소 배정이
	 * 무효가 되므로 호출자가 자원을 미리 놓아 두어야 한다. */
	pci_write_config_dword(pdev, pos + PCI_REBAR_CTRL, ctrl);

	/* [한국어] VF BAR 이면, */
	if (pci_resource_is_iov(bar))
		/* [한국어] SR-IOV 쪽 자원 크기도 함께 갱신한다. VF BAR 은 VF 개수만큼 배수가 되므로
		 * 그 계산을 SR-IOV 코드가 따로 한다. */
		pci_iov_resource_set_size(pdev, bar, size);

	return 0;
}

/* [한국어]
 * pci_restore_rebar_state - 절전에서 복귀한 뒤 BAR 크기를 되돌린다
 *
 * @pdev: 복귀 중인 장치.
 *
 * D3 에서 돌아오면 config 공간이 기본값으로 초기화되어 BAR 크기도 하드웨어
 * 기본값으로 돌아간다. 그대로 두면 커널이 알고 있는 자원 크기와 실제 장치가
 * 디코딩하는 범위가 어긋나므로 되돌려야 한다.
 *
 * 되돌릴 값의 출처가 이 함수의 핵심이다. 저장해 둔 config 값이 아니라
**커널의 자원 구조체** 에서 현재 크기를 읽어 인코딩으로 바꾼다. 자원 구조체가
 * 진실의 원천이라는 뜻으로, 그래야 절전 중에 자원이 재배치되었더라도
 * 그 결과가 반영된다.
 *
 * 순회 구조는 pci_rebar_find_pos() 와 같다. 첫 제어 레지스터에서 BAR 개수를
 * 읽고, 항목마다 8바이트씩 밀며 돈다. 다만 특정 BAR 을 찾는 것이 아니라
 * 전부 처리하므로 조기 반환이 없다.
 *
 * BAR 인덱스를 꺼낼 때 FIELD_GET 대신 마스크를 직접 쓰는데, 그 필드가
 * 최하위에 있어 시프트가 필요 없기 때문이다.
 *
 * 실행 컨텍스트: 절전 복귀 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. capability 가 없으면 조용히 돌아간다.
 *
 * 호출 체인:
 *   pci_restore_state() → [이 함수]
 *     → pci_resource_n() → pci_rebar_bytes_to_size()
 *     → pci_write_config_dword()
 */
void pci_restore_rebar_state(struct pci_dev *pdev)
{
	/* [한국어] capability 위치, BAR 개수, 순회 인덱스. */
	unsigned int pos, nbars, i;
	/* [한국어] 제어 레지스터 값. */
	u32 ctrl;

	/* [한국어] 캐시해 둔 위치를 쓴다. */
	pos = pdev->rebar_cap;
	/* [한국어] 없으면 복원할 것도 없다. */
	if (!pos)
		return;

	/* [한국어] 첫 제어 레지스터를 읽고, */
	pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
	/* [한국어] BAR 개수를 얻는다. */
	nbars = FIELD_GET(PCI_REBAR_CTRL_NBAR_MASK, ctrl);

	/* [한국어] pci_rebar_find_pos() 와 같은 방식으로 항목들을 순회한다. */
	for (i = 0; i < nbars; i++, pos += 8) {
		/* [한국어] 그 BAR 에 대응하는 자원. */
		struct resource *res;
		/* [한국어] BAR 번호와 인코딩된 크기. */
		int bar_idx, size;

		/* [한국어] 항목의 제어 레지스터를 읽고, */
		pci_read_config_dword(pdev, pos + PCI_REBAR_CTRL, &ctrl);
		/* [한국어] BAR 인덱스를 꺼낸다. 여기서는 FIELD_GET 대신 마스크를 직접 쓰는데,
		 * 그 필드가 최하위에 있어 시프트가 필요 없기 때문이다. */
		bar_idx = ctrl & PCI_REBAR_CTRL_BAR_IDX;
		/* [한국어] 커널이 들고 있는 그 BAR 의 자원 구조체를 가져온다. */
		res = pci_resource_n(pdev, bar_idx);
		/* [한국어] 자원의 현재 크기를 인코딩으로 바꾼다. 하드웨어가 아니라 **커널의 자원
		 * 구조체** 가 진실의 원천이라는 점이 이 함수의 핵심이다 — 절전 복귀 시
		 * 하드웨어는 기본값으로 돌아가 있으므로 소프트웨어 쪽 값으로 되돌려야 한다. */
		size = pci_rebar_bytes_to_size(resource_size(res));
		/* [한국어] 크기 필드를 지우고, */
		ctrl &= ~PCI_REBAR_CTRL_BAR_SIZE;
		/* [한국어] 계산한 값을 넣어, */
		ctrl |= FIELD_PREP(PCI_REBAR_CTRL_BAR_SIZE, size);
		/* [한국어] 되쓴다. */
		pci_write_config_dword(pdev, pos + PCI_REBAR_CTRL, ctrl);
	}
}

/* [한국어]
 * pci_resize_is_memory_decoding_enabled - 지금 BAR 크기를 바꿔도 되는지 확인한다
 *
 * @dev: 대상 장치.
 * @resno: 자원 번호(BAR 번호).
 * @return: true = 메모리 디코딩이 켜져 있어 바꾸면 안 됨, false = 바꿔도 됨.
 *
 * BAR 크기를 바꾸면 장치가 응답하는 주소 범위가 그 순간 달라진다. 메모리
 * 디코딩이 켜진 상태에서 그렇게 하면 다른 장치의 주소를 잠깐 가로채거나
 * 자기 주소를 잃는 구간이 생기므로, 먼저 꺼져 있어야 한다.
 *
 * VF BAR 은 판정 방법이 다르다. VF 의 메모리 디코딩은 PF 의 Command
 * 레지스터가 아니라 SR-IOV Control 레지스터가 제어하므로 그쪽에 묻는다.
 *
 * pci_resize_resource() 가 이 함수를 보고 -EBUSY 를 반환하며, 드라이버가
 * 먼저 장치를 비활성화하도록 요구한다.
 *
 * 실행 컨텍스트: 크기 변경 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 판정 결과가 곧 반환값이다.
 *
 * 호출 체인:
 *   pci_resize_resource() → [이 함수]
 *     → pci_iov_is_memory_decoding_enabled() 또는 pci_read_config_word(PCI_COMMAND)
 */
static bool pci_resize_is_memory_decoding_enabled(struct pci_dev *dev,
						  int resno)
{
	/* [한국어] Command 레지스터 값. */
	u16 cmd;

	/* [한국어] VF BAR 이면, */
	if (pci_resource_is_iov(resno))
		/* [한국어] SR-IOV 쪽 판정을 쓴다. VF 의 메모리 디코딩은 PF 의 Command 가 아니라
		 * SR-IOV Control 레지스터가 제어하기 때문이다. */
		return pci_iov_is_memory_decoding_enabled(dev);

	/* [한국어] 보통은 Command 레지스터를 읽어, */
	pci_read_config_word(dev, PCI_COMMAND, &cmd);

	/* [한국어] 메모리 디코딩 비트를 본다. 켜져 있으면 장치가 주소를 응답하고 있다는
	 * 뜻이라, 그 상태에서 BAR 크기를 바꾸면 응답 범위가 바뀌는 순간이 생긴다. */
	return cmd & PCI_COMMAND_MEMORY;
}

/* [한국어]
 * pci_resize_resource_set_size - 인코딩된 크기를 자원 구조체에 반영한다
 *
 * @dev: 대상 장치.
 * @resno: 자원 번호.
 * @size: 인코딩된 BAR 크기(0=1MB, 31=128TB).
 *
 * 하드웨어를 건드리지 않고 커널 쪽 자원 구조체만 갱신한다. 하드웨어 쓰기는
 * pci_rebar_set_size() 가 따로 하며, 둘을 나눠 둔 덕분에 상위 계층이
 * "소프트웨어 상 크기를 정해 두고 나중에 재배치" 하는 순서를 쓸 수 있다.
 *
 * VF BAR 에 대한 곱셈이 이 함수의 특징이다. SR-IOV 의 VF BAR 은 하나의 자원
 * 안에 모든 VF 의 몫이 연속으로 배치되므로, 한 VF 의 크기에 최대 VF 개수를
 * 곱해야 자원 전체 크기가 된다.
 *
 * 시작 주소를 건드리지 않는 점도 중요하다. 크기만 바꾸고 재배치는
 * setup-bus.c 가 하며, 그래서 이 함수 뒤에는 반드시 재배치 단계가 따라온다.
 *
 * 실행 컨텍스트: 크기 변경 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_do_resource_release_and_resize() [setup-bus.c] → [이 함수]
 *     → pci_rebar_size_to_bytes() → pci_sriov_get_totalvfs() → resource_set_size()
 */
void pci_resize_resource_set_size(struct pci_dev *dev, int resno, int size)
{
	/* [한국어] 인코딩을 바이트 크기로 되돌린다. */
	resource_size_t res_size = pci_rebar_size_to_bytes(size);
	/* [한국어] 갱신할 자원 구조체. */
	struct resource *res = pci_resource_n(dev, resno);

	/* [한국어] VF BAR 이면, */
	if (pci_resource_is_iov(resno))
		/* [한국어] VF 최대 개수만큼 곱한다. SR-IOV 의 VF BAR 은 하나의 자원 안에 모든 VF 의
		 * 몫이 연속으로 배치되기 때문이다. */
		res_size *= pci_sriov_get_totalvfs(dev);

	/* [한국어] 자원의 크기를 갱신한다. 시작 주소는 건드리지 않는데, 재배치는 상위
	 * 계층(setup-bus.c)이 하기 때문이다. */
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
	/* [한국어] 이 장치가 매달린 호스트 브리지. */
	struct pci_host_bridge *host;

	/* Check if we must preserve the firmware's resource assignment */
	host = pci_find_host_bridge(dev->bus);
	/* [한국어] 펌웨어가 잡아 둔 설정을 보존해야 하는 플랫폼이면 자원을 재배치할 수
	 * 없으므로, */
	if (host->preserve_config)
		return -ENOTSUPP;

	/* [한국어] 메모리 디코딩이 켜져 있으면 지금 크기를 바꿀 수 없다. 드라이버가
	 * 먼저 pci_disable_device() 등으로 꺼야 한다. */
	if (pci_resize_is_memory_decoding_enabled(dev, resno))
		return -EBUSY;

	/* [한국어] 하드웨어가 그 크기를 지원하지 않으면, */
	if (!pci_rebar_size_supported(dev, resno, size))
		return -EINVAL;

	/* [한국어] 실제 작업은 setup-bus.c:6541 의 함수에 넘긴다. 자원을 놓고, 크기를 바꾸고,
	 * 상위 브리지 윈도우까지 다시 계산하는 일이 거기서 이루어진다. */
	return pci_do_resource_release_and_resize(dev, resno, size, exclude_bars);
}
EXPORT_SYMBOL(pci_resize_resource);
