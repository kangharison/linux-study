// SPDX-License-Identifier: GPL-2.0
/*
 * Support routines for initializing a PCI subsystem
 *
 * Extruded from code written by
 *      Dave Rusling (david.rusling@reo.mts.dec.com)
 *      David Mosberger (davidm@cs.arizona.edu)
 *	David Miller (davem@redhat.com)
 *
 * Fixed for multiple PCI buses, 1999 Andrea Arcangeli <andrea@suse.de>
 *
 * Nov 2000, Ivan Kokshaysky <ink@jurassic.park.msu.ru>
 *	     Resource sorting
 */
/*
 * [한국어 설명] 확정된 주소를 실제 BAR 레지스터에 써 넣는 곳 (setup-res.c)
 *
 * === 파일의 역할 ===
 * 자원 배치의 마지막 단계를 담당한다. setup-bus.c 가 "어느 BAR 를 어디에
 * 놓을지" 계산했다면, 이 파일은 그 결정을 실제 하드웨어 레지스터에 기록하고,
 * 커널의 자원 트리(iomem_resource / ioport_resource)에 등록해 다른 누구도
 * 그 구간을 쓰지 못하게 못박는다.
 *
 * 세 덩어리로 나뉜다.
 *   1) 기록 - pci_update_resource() / pci_std_update_resource() 가
 *      struct resource 의 값을 BAR 레지스터에 써 넣는다. 64비트 BAR 를
 *      두 개의 32비트 레지스터에 나눠 쓰는 처리, 쓰기 후 되읽어 확인하는
 *      검증이 여기 있다.
 *   2) 예약 - pci_claim_resource() 가 자원 트리에 구간을 등록한다.
 *      이것이 성공해야 그 주소가 이 장치의 것이 된다.
 *   3) 배치 - pci_assign_resource() 계열이 빈 구간을 찾아 1과 2를 함께 한다.
 *      찾기는 bus.c 의 pci_bus_alloc_resource() 에 위임한다.
 *
 * 한 가지 미묘한 점: 자원을 "정한다" 와 "쓴다" 가 분리되어 있다는 것이다.
 * struct resource 를 고치는 것만으로는 하드웨어가 바뀌지 않고, 반대로
 * BAR 에 쓰기만 하고 자원 트리에 등록하지 않으면 다른 장치가 같은 구간을
 * 배정받는다. 그래서 이 파일의 함수들은 대개 둘을 짝지어 수행한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 열거:  probe.c 의 __pci_read_base() 가 BAR 를 읽어 크기와 종류를 알아낸다
 *          -> struct resource 에 담긴다(아직 주소는 펌웨어가 넣은 값)
 *   -> setup-bus.c 가 계층 전체의 배치를 계산
 *      -> bus.c 의 pci_bus_alloc_resource() 로 빈 구간을 얻고
 *      -> [이 파일] pci_assign_resource() / pci_update_resource()
 *         -> pci_write_config_dword(BAR)
 *   -> pci-driver.c 의 pci_device_probe()
 *      -> pci_enable_device() -> [이 파일] pci_enable_resources()
 *         -> Command 레지스터의 Memory/IO Space Enable 을 켠다
 *      -> 드라이버의 probe (nvme_probe 등)
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트. config 접근과 자원 트리 조작이
 * 있으며, 자원 트리는 자체 스핀락으로 보호된다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: setup-bus.c(배치 계산), probe.c(열거), pci.c(pci_enable_device),
 *   그리고 hotplug 재스캔 경로.
 * 아래쪽: access.c 의 config 접근 함수, kernel/resource.c 의 자원 트리
 *   (request_resource, allocate_resource), bus.c 의 구간 찾기.
 * 옆쪽: 아키텍처가 덮어쓸 수 있는 __weak 훅 두 개 —
 *   pcibios_align_resource()(플랫폼 고유 정렬 요구)와
 *   pcibios_retrieve_fw_addr()(펌웨어가 정한 주소 조회).
 * 공유 상태: struct pci_dev 의 resource[] 배열이 핵심이다. 이 배열의
 *   start/end/flags 가 곧 "이 장치가 어디를 쓰는가" 이고, 이 파일이
 *   그것을 하드웨어와 동기화한다.
 *
 * === NVMe 드라이버가 실제로 쓰는 것 (drivers/nvme/ 전수 확인) ===
 * NVMe 드라이버는 이 파일의 함수를 하나도 직접 부르지 않는다.
 * 그럼에도 NVMe 가 동작하기 위한 전제를 이 파일이 만든다.
 *
 * NVMe 컨트롤러 레지스터(CAP, VS, CC, CSTS, 그리고 도어벨 배열)는 전부
 * BAR0 가 가리키는 메모리 창 안에 있다. 그 창의 물리 주소가 확정되어
 * 하드웨어 BAR0 레지스터에 기록되는 지점이 pci_update_resource() 이고,
 * 그 값이 struct resource 에 남아 pci_resource_start(pdev, 0) 으로 읽힌다.
 * NVMe 드라이버는 그것을 ioremap 해서 도어벨을 두드린다.
 *
 * 그리고 pci_enable_resources() 가 Command 레지스터의 Memory Space Enable
 * 비트를 켜지 않으면, BAR0 주소로 보낸 접근을 컨트롤러가 무시한다.
 * NVMe 가 부르는 pci_enable_device_mem() 이 결국 이 함수로 내려온다 —
 * 이것이 이 파일과 NVMe 를 잇는 가장 가까운 고리다.
 *
 * (기존 주석은 NVMe 경로로 "pci_enable_device -> pci_assign_resource ->
 *  pci_iomap(BAR0) -> pci_enable_msix_range" 를 적어 두었으나, 그중
 *  pci_iomap 과 pci_enable_msix_range 는 drivers/nvme/ 에 호출이 0건이다.
 *  또 pci_assign_resource 는 pci_enable_device 가 부르는 함수가 아니라
 *  그보다 훨씬 앞선 열거 단계에서 불린다. 위 내용으로 대체했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_std_update_resource()  : 표준 BAR 한 칸을 하드웨어에 기록한다.
 *                              64비트 BAR 의 상하위 분할, ROM BAR 의 Enable
 *                              비트 보존, 쓰기 후 되읽기 검증이 모두 여기 있다.
 * pci_update_resource()      : 위를 감싼 진입점. Memory 디코딩을 잠시 꺼서
 *                              기록 도중 어중간한 주소가 노출되지 않게 한다.
 * pci_claim_resource()       : 자원 트리에 구간을 등록해 소유권을 확정한다.
 *                              펌웨어가 이미 배치해 둔 주소를 그대로 쓸 때 부른다.
 * pci_release_resource()     : 그 반대. 구간을 놓고 UNSET 표시를 단다.
 * pci_assign_resource()      : 빈 구간을 찾아 배정하고 하드웨어에 기록한다.
 *                              실패하면 상위 버스로 올라가며 재시도한다.
 * pci_reassign_resource()    : 이미 배정된 자원의 크기를 늘려 다시 배정한다.
 *                              hotplug 로 늘어난 요구를 수용할 때 쓴다.
 * pci_enable_resources()     : Command 레지스터의 Memory/IO Space Enable 을
 *                              켠다. 이것이 켜져야 BAR 접근이 장치에 닿는다.
 * pci_disable_bridge_window(): 브리지 윈도우를 무효화한다. 재배치 전 정리용.
 * pci_revert_fw_address()    : 커널 배치가 실패했을 때 펌웨어가 정해 둔
 *                              주소로 되돌리는 최후 수단.
 * pcibios_align_resource()   : 아키텍처가 추가 정렬을 요구할 수 있는 __weak 훅.
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/pci.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/cache.h>
#include <linux/slab.h>
#include "pci.h"

/*
 * [한국어]
 * pci_std_update_resource - 표준 BAR 한 칸에 확정된 주소를 써 넣는다
 *
 * @dev:   대상 장치
 * @resno: 자원 번호. 0~5 는 표준 BAR, PCI_ROM_RESOURCE 는 ROM BAR.
 *         브리지 윈도우 자원은 여기서 다루지 않는다.
 * @return: 없음. 실패해도 경고만 남긴다.
 *
 * struct resource 에 담긴 CPU 주소를 PCI 버스 주소로 바꿔 BAR 레지스터에
 * 기록하고, 되읽어 제대로 들어갔는지 확인한다. 이 파일에서 가장 하드웨어에
 * 가까운 함수다.
 *
 * 세 가지 까다로운 처리가 들어 있다.
 *
 *   1) 하위 플래그 비트 보존 - BAR 레지스터의 하위 몇 비트는 주소가 아니라
 *      종류 표시다(메모리/IO, 32/64비트, prefetchable 여부). 주소만 쓰고
 *      그 비트를 지우면 장치가 BAR 의 종류를 잘못 알린다. 그래서 마스크로
 *      주소 부분만 남기고 flags 에서 종류 비트를 다시 OR 한다.
 *
 *   2) 64비트 BAR 의 비원자적 갱신 - 64비트 주소는 연속된 두 개의 32비트
 *      레지스터에 나눠 써야 하는데, 그 사이에 BAR 가 "하위는 새 주소,
 *      상위는 옛 주소" 인 엉뚱한 값을 갖는 순간이 생긴다. 그 주소가 다른
 *      장치의 것과 겹치면 두 장치가 같은 접근에 응답한다. 그래서 쓰는
 *      동안 Memory Space Enable 을 꺼 둔다.
 *
 *   3) ROM BAR 의 특수 사정 - 아래 원문 주석이 밝히듯, 일부 Matrox 장치는
 *      ROM 이 비활성일 때 BAR 가 0 으로 읽힌다. 그래서 비활성 ROM 은
 *      건드리지 않는 것이 원칙이지만, ROM 이 다른 BAR 와 겹치는 버그 장치
 *      (rom_bar_overlap quirk)에서는 비활성이어도 옮겨야 한다.
 *
 * VF 를 걸러 내는 첫 검사도 중요하다. SR-IOV 가상 함수의 BAR 는 스펙상
 * 읽기 전용 0 이라 쓰기가 아무 효과가 없고, 실제 주소는 PF 의 SR-IOV
 * capability 안 VF BAR 로 정해진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 접근).
 * 호출자: pci_update_resource() 하나뿐이다.
 * 피호출자: pcibios_resource_to_bus(), pci_read/write_config_dword().
 *
 * 호출 체인:
 *   pci_assign_resource -> pci_update_resource -> [pci_std_update_resource]
 *     -> pci_write_config_dword(BAR)
 */
static void pci_std_update_resource(struct pci_dev *dev, int resno)
{
	struct pci_bus_region region;
	bool disable;
	u16 cmd;
	u32 new, check, mask;
	int reg;
	struct resource *res = pci_resource_n(dev, resno);
	const char *res_name = pci_resource_name(dev, resno);

	/* Per SR-IOV spec 3.4.1.11, VF BARs are RO zero */
	if (dev->is_virtfn)
		return;

	/*
	 * Ignore resources for unimplemented BARs and unused resource slots
	 * for 64 bit BARs.
	 */
	if (!res->flags)
		return;

	if (res->flags & IORESOURCE_UNSET)
		return;

	/*
	 * Ignore non-moveable resources.  This might be legacy resources for
	 * which no functional BAR register exists or another important
	 * system resource we shouldn't move around.
	 */
	if (res->flags & IORESOURCE_PCI_FIXED)
		return;

	pcibios_resource_to_bus(dev->bus, &region, res);
	new = region.start;

	if (res->flags & IORESOURCE_IO) {
		mask = (u32)PCI_BASE_ADDRESS_IO_MASK;
		new |= res->flags & ~PCI_BASE_ADDRESS_IO_MASK;
	} else if (resno == PCI_ROM_RESOURCE) {
		mask = PCI_ROM_ADDRESS_MASK;
	} else {
		mask = (u32)PCI_BASE_ADDRESS_MEM_MASK;
		new |= res->flags & ~PCI_BASE_ADDRESS_MEM_MASK;
	}

	if (resno < PCI_ROM_RESOURCE) {
		reg = PCI_BASE_ADDRESS_0 + 4 * resno;
	} else if (resno == PCI_ROM_RESOURCE) {

		/*
		 * Apparently some Matrox devices have ROM BARs that read
		 * as zero when disabled, so don't update ROM BARs unless
		 * they're enabled.  See
		 * https://lore.kernel.org/r/43147B3D.1030309@vc.cvut.cz/
		 * But we must update ROM BAR for buggy devices where even a
		 * disabled ROM can conflict with other BARs.
		 */
		if (!(res->flags & IORESOURCE_ROM_ENABLE) &&
		    !dev->rom_bar_overlap)
			return;

		reg = dev->rom_base_reg;
		if (res->flags & IORESOURCE_ROM_ENABLE)
			new |= PCI_ROM_ADDRESS_ENABLE;
	} else
		return;

	/*
	 * We can't update a 64-bit BAR atomically, so when possible,
	 * disable decoding so that a half-updated BAR won't conflict
	 * with another device.
	 */
	disable = (res->flags & IORESOURCE_MEM_64) && !dev->mmio_always_on;
	if (disable) {
		pci_read_config_word(dev, PCI_COMMAND, &cmd);
		pci_write_config_word(dev, PCI_COMMAND,
				      cmd & ~PCI_COMMAND_MEMORY);
	}

	pci_write_config_dword(dev, reg, new);
	pci_read_config_dword(dev, reg, &check);

	if ((new ^ check) & mask) {
		pci_err(dev, "%s: error updating (%#010x != %#010x)\n",
			res_name, new, check);
	}

	if (res->flags & IORESOURCE_MEM_64) {
		new = region.start >> 16 >> 16;
		pci_write_config_dword(dev, reg + 4, new);
		pci_read_config_dword(dev, reg + 4, &check);
		if (check != new) {
			pci_err(dev, "%s: error updating (high %#010x != %#010x)\n",
				res_name, new, check);
		}
	}

	if (disable)
		pci_write_config_word(dev, PCI_COMMAND, cmd);
}

/*
 * [한국어]
 * pci_update_resource - 자원 번호에 따라 알맞은 BAR 갱신 경로로 보낸다
 *
 * @dev:   대상 장치
 * @resno: 자원 번호
 * @return: 없음.
 *
 * 갈림길 하나뿐인 얇은 함수다. 표준 BAR(0~ROM)이면 pci_std_update_resource(),
 * SR-IOV VF BAR 이면 pci_iov_update_resource() 로 보낸다.
 *
 * 두 경로가 다른 이유: VF 의 BAR 는 VF 자신의 config space 가 아니라 PF 의
 * SR-IOV capability 안에 있는 VF BAR 레지스터로 정해진다. 게다가 한 번의
 * 쓰기가 모든 VF 에 동시에 영향을 주므로, 표준 BAR 와 완전히 다른 처리가
 * 필요하다.
 *
 * 이 분기를 여기 한 곳에 모아 두어, 자원을 배치하는 상위 코드
 * (pci_assign_resource 등)가 표준 BAR 와 VF BAR 를 구분하지 않아도 되게 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: pci_assign_resource(), pci_reassign_resource(), pci_restore_state().
 */
void pci_update_resource(struct pci_dev *dev, int resno)
{
	if (resno <= PCI_ROM_RESOURCE)
		pci_std_update_resource(dev, resno);
	else if (pci_resource_is_iov(resno))
		pci_iov_update_resource(dev, resno);
}

/*
 * [한국어]
 * pci_claim_resource - 이미 정해진 주소를 자원 트리에 등록해 소유권을 확정한다
 *
 * @dev:      대상 장치
 * @resource: 자원 번호
 * @return:   0 = 등록 성공, 음수 = 상위 창을 못 찾았거나 다른 자원과 충돌.
 *
 * "배정" 이 아니라 "주장" 이다. 주소를 새로 고르지 않고, struct resource 에
 * 이미 들어 있는 구간을 그대로 커널 자원 트리에 등록한다. 펌웨어가 배치해
 * 둔 주소를 커널이 그대로 받아들일 때 쓰는 경로다.
 *
 * 두 단계로 실패할 수 있다.
 *   1) 상위 창을 못 찾음 - 이 자원을 담을 부모 구간(브리지 윈도우 또는
 *      루트 버스의 자원)이 없다. 브리지 설정이 잘못됐거나, 이 주소가
 *      어떤 창에도 속하지 않는다는 뜻이다.
 *   2) 충돌 - 부모는 찾았는데 그 안에서 다른 자원과 겹친다. 어느 자원과
 *      겹쳤는지 conflict 로 돌아오므로 로그에 함께 남긴다.
 *
 * 실패하면 호출자가 대개 pci_assign_resource() 로 넘어가 새 주소를 찾는다.
 * 즉 "펌웨어 배치를 먼저 시도하고, 안 되면 커널이 다시 배치한다" 는
 * 흐름의 앞쪽 절반이다.
 *
 * NVMe 학습 관점: 정상 부팅에서는 대부분의 NVMe BAR 가 이 경로로 확정된다.
 * 펌웨어(UEFI)가 이미 적절한 주소를 넣어 두었기 때문이다. 여기서 실패하는
 * 보드는 "pci=realloc" 부팅 인자가 필요한 경우가 많다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 자원 트리 조작은 내부 스핀락으로 보호된다.
 * 호출자: pci_assign_resource() 의 앞단, hotplug 재스캔, 아키텍처 초기화 코드.
 * 피호출자: pci_find_parent_resource(), request_resource_conflict().
 *
 * 호출 체인:
 *   pcibios_resource_survey_bus -> [pci_claim_resource]
 *     -> pci_find_parent_resource -> request_resource_conflict
 */
int pci_claim_resource(struct pci_dev *dev, int resource)
{
	struct resource *res = &dev->resource[resource];
	const char *res_name = pci_resource_name(dev, resource);
	struct resource *root, *conflict;

	if (res->flags & IORESOURCE_UNSET) {
		pci_info(dev, "%s %pR: can't claim; no address assigned\n",
			 res_name, res);
		return -EINVAL;
	}

	/*
	 * If we have a shadow copy in RAM, the PCI device doesn't respond
	 * to the shadow range, so we don't need to claim it, and upstream
	 * bridges don't need to route the range to the device.
	 */
	if (res->flags & IORESOURCE_ROM_SHADOW)
		return 0;

	root = pci_find_parent_resource(dev, res);
	if (!root) {
		pci_info(dev, "%s %pR: can't claim; no compatible bridge window\n",
			 res_name, res);
		res->flags |= IORESOURCE_UNSET;
		return -EINVAL;
	}

	conflict = request_resource_conflict(root, res);
	if (conflict) {
		pci_info(dev, "%s %pR: can't claim; address conflict with %s %pR\n",
			 res_name, res, conflict->name, conflict);
		res->flags |= IORESOURCE_UNSET;
		return -EBUSY;
	}

	return 0;
}
EXPORT_SYMBOL(pci_claim_resource);

/*
 * [한국어]
 * pci_disable_bridge_window - 브리지의 메모리 윈도우를 무효화한다
 *
 * @dev: 브리지 장치
 * @return: 없음.
 *
 * 브리지가 어떤 주소도 하위 버스로 통과시키지 않게 만든다. 자원을 재배치하기
 * 전에 옛 윈도우를 치우는 용도다.
 *
 * "무효화" 하는 방법이 특이하다. 윈도우를 끄는 별도 비트가 없어서,
 * base 를 limit 보다 크게 만들어 "빈 구간" 으로 표현한다. PCI-to-PCI 브리지
 * 스펙이 정한 관용구다 — base > limit 이면 그 윈도우는 비활성이다.
 *
 * 쓰는 값을 하나씩 보면:
 *   PCI_MEMORY_BASE 에 0x0000fff0 - 이 레지스터는 32비트지만 상위 16비트가
 *     limit, 하위 16비트가 base 다. 즉 base=0xfff0, limit=0x0000 이 되어
 *     base > limit 이 성립한다.
 *   PCI_PREF_MEMORY_BASE 에 0x0000fff0 - 프리페치 윈도우도 같은 방식.
 *   PCI_PREF_BASE_UPPER32 에 0xffffffff, PCI_PREF_LIMIT_UPPER32 에 0 -
 *     64비트 프리페치 윈도우의 상위 32비트도 base 를 최대, limit 을 0 으로
 *     만들어 확실히 뒤집는다. 상위를 먼저 0 으로 만들고 나중에 최대로 쓰는
 *     순서가 중요하다 — 반대로 하면 중간에 base < limit 인 순간이 생겨
 *     엉뚱한 거대 구간이 잠시 열린다.
 *
 * I/O 윈도우는 건드리지 않는다. 이 함수의 호출자들이 메모리 자원의
 * 재배치만 다루기 때문이다.
 *
 * (기존 주석은 이 함수가 "DMA/MSI-X/IRQ 트래픽" 을 차단한다고 적었으나
 *  방향이 반대다. 브리지 메모리 윈도우는 호스트에서 장치로 내려가는
 *  접근을 라우팅하는 것이고, DMA 와 MSI-X 쓰기는 장치에서 호스트로
 *  올라가는 트래픽이라 이 윈도우와 무관하다. 그쪽을 막는 것은
 *  Command 레지스터의 Bus Master Enable 이다.)
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 쓰기).
 * 호출자: pci_reassigndev_resource_alignment() [pci.c], setup-bus.c 의 재배치 경로.
 */
void pci_disable_bridge_window(struct pci_dev *dev)
{
	/* MMIO Base/Limit */
	pci_write_config_dword(dev, PCI_MEMORY_BASE, 0x0000fff0);

	/* Prefetchable MMIO Base/Limit */
	pci_write_config_dword(dev, PCI_PREF_LIMIT_UPPER32, 0);
	pci_write_config_dword(dev, PCI_PREF_MEMORY_BASE, 0x0000fff0);
	pci_write_config_dword(dev, PCI_PREF_BASE_UPPER32, 0xffffffff);
}

/*
 * Generic function that returns a value indicating that the device's
 * original BIOS BAR address was not saved and so is not available for
 * reinstatement.
 *
 * Can be over-ridden by architecture specific code that implements
 * reinstatement functionality rather than leaving it disabled when
 * normal allocation attempts fail.
 */
/*
 * [한국어]
 * pcibios_retrieve_fw_addr - 펌웨어가 이 BAR 에 넣어 두었던 주소를 알려 준다
 *
 * @dev: 대상 장치
 * @idx: 자원 번호
 * @return: 펌웨어가 정한 주소. 0 = 알 수 없음(기본 구현).
 *
 * __weak 훅이며 기본 구현은 항상 0 을 돌려준다. 위 원문 주석이 그 뜻을
 * 밝힌다 — "원래 BIOS BAR 주소를 저장해 두지 않았으므로 되살릴 수 없다".
 *
 * 커널이 자원 배치에 실패했을 때 마지막으로 시도하는 것이 "펌웨어가 정해
 * 두었던 주소로 되돌리기" 다(pci_revert_fw_address). 그러려면 그 주소를
 * 기억하고 있어야 하는데, 일반 커널은 그것을 보관하지 않는다. 아키텍처가
 * 보관해 두었다면 이 훅을 덮어써서 알려 줄 수 있다.
 *
 * 0 을 "알 수 없음" 으로 쓰는 것이 안전한 이유: 주소 0 은 어떤 BAR 에도
 * 유효한 값이 아니다(그 자리에는 시스템 메모리나 레거시 영역이 있다).
 *
 * 실행 컨텍스트: 제약 없음.
 * 호출자: pci_revert_fw_address().
 */
resource_size_t __weak pcibios_retrieve_fw_addr(struct pci_dev *dev, int idx)
{
	return 0;
}

/*
 * [한국어]
 * pci_revert_fw_address - 배치에 실패했을 때 펌웨어가 쓰던 주소로 되돌린다
 *
 * @res:   되돌릴 자원
 * @dev:   대상 장치
 * @resno: 자원 번호
 * @size:  필요한 크기
 * @return: 0 = 되돌리기 성공, 음수 = 실패(펌웨어 주소를 모르거나 그 구간도 충돌).
 *
 * 최후의 수단이다. 커널이 빈 구간을 찾지 못했을 때, "적어도 부팅 시점에는
 * 동작하던" 펌웨어 배치로 되돌아가 본다.
 *
 * 이것이 성립하는 논리: 펌웨어가 그 주소를 골랐다는 것은 그 시점에는
 * 충돌이 없었다는 뜻이다. 커널이 자원 트리를 다시 구성하면서 무언가
 * 어긋났을 수 있으므로, 원래 배치로 돌아가면 통할 가능성이 있다.
 *
 * 다만 pcibios_retrieve_fw_addr() 의 기본 구현이 0(모름)을 돌려주므로,
 * 대부분의 아키텍처에서 이 함수는 곧바로 실패한다. 실질적으로는
 * 그 훅을 구현한 아키텍처에서만 의미가 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: pci_assign_resource() 의 실패 경로.
 *
 * 호출 체인:
 *   pci_assign_resource -> [pci_revert_fw_address]
 *     -> pcibios_retrieve_fw_addr -> request_resource_conflict
 */
static int pci_revert_fw_address(struct resource *res, struct pci_dev *dev,
		int resno, resource_size_t size)
{
	struct resource *root, *conflict;
	resource_size_t fw_addr, start, end;
	const char *res_name = pci_resource_name(dev, resno);

	fw_addr = pcibios_retrieve_fw_addr(dev, resno);
	if (!fw_addr)
		return -ENOMEM;

	start = res->start;
	end = res->end;
	resource_set_range(res, fw_addr, size);
	res->flags &= ~IORESOURCE_UNSET;

	root = pci_find_parent_resource(dev, res);
	if (!root) {
		/*
		 * If dev is behind a bridge, accesses will only reach it
		 * if res is inside the relevant bridge window.
		 */
		if (pci_upstream_bridge(dev))
			return -ENXIO;

		/*
		 * On the root bus, assume the host bridge will forward
		 * everything.
		 */
		if (res->flags & IORESOURCE_IO)
			root = &ioport_resource;
		else
			root = &iomem_resource;
	}

	pci_info(dev, "%s: trying firmware assignment %pR\n", res_name, res);
	conflict = request_resource_conflict(root, res);
	if (conflict) {
		pci_info(dev, "%s %pR: conflicts with %s %pR\n", res_name, res,
			 conflict->name, conflict);
		res->start = start;
		res->end = end;
		res->flags |= IORESOURCE_UNSET;
		return -EBUSY;
	}
	return 0;
}

/*
 * For mem bridge windows, try to relocate tail remainder space to space
 * before res->start if there's enough free space there. This enables
 * tighter packing for resources.
 */
/*
 * [한국어]
 * pci_align_resource - 후보 시작 주소를 이 장치가 요구하는 경계로 밀어 올린다
 *
 * @dev:       대상 장치(정렬 요구의 주체)
 * @res:       배치하려는 자원
 * @empty_res: 지금 검토 중인 빈 구간
 * @size:      필요한 크기
 * @align:     요구 정렬
 * @return:    조정된 시작 주소.
 *
 * 자원 할당기(kernel/resource.c 의 allocate_resource)가 빈 구간을 훑으면서
 * "이 자리에 놓을 수 있는가" 를 물을 때마다 부르는 콜백이다. 후보 주소를
 * 받아 이 장치가 쓸 수 있는 형태로 밀어 올린 값을 돌려주면, 할당기가
 * 그 자리에 크기가 들어가는지 다시 확인한다.
 *
 * 콜백으로 만든 이유가 있다. 정렬 요구는 장치마다 다르고(quirk, 아키텍처
 * 정책, IOMMU 페이지 크기), 그 지식을 일반 할당기에 넣을 수 없기 때문이다.
 * 그래서 할당기는 "밀어 올리는 방법" 만 물어보고 판단은 여기에 맡긴다.
 *
 * 실행 컨텍스트: 자원 트리 락을 쥔 상태에서 불린다. 잠들면 안 되고,
 *   자원 트리를 다시 건드려서도 안 된다.
 * 호출자: pcibios_align_resource() 를 통해 allocate_resource() 가 부른다.
 */
resource_size_t pci_align_resource(struct pci_dev *dev,
				   const struct resource *res,
				   const struct resource *empty_res,
				   resource_size_t size,
				   resource_size_t align)
{
	resource_size_t remainder, start_addr;

	if (!(res->flags & IORESOURCE_MEM))
		return res->start;

	if (IS_ALIGNED(size, align))
		return res->start;

	remainder = size - ALIGN_DOWN(size, align);
	/* Don't mess with size that doesn't align with window size granularity */
	if (!IS_ALIGNED(remainder, pci_min_window_alignment(dev->bus, res->flags)))
		return res->start;
	/* Try to place remainder that doesn't fill align before */
	if (res->start < remainder)
		return res->start;
	start_addr = res->start - remainder;
	if (empty_res->start > start_addr)
		return res->start;

	pci_dbg(dev, "%pR: moving candidate start address below align to %llx\n",
		res, (unsigned long long)start_addr);
	return start_addr;
}

/*
 * We don't have to worry about legacy ISA devices, so nothing to do here.
 * This is marked as __weak because multiple architectures define it; it should
 * eventually go away.
 */
/*
 * [한국어]
 * pcibios_align_resource - 아키텍처가 정렬 정책을 덮어쓸 수 있는 __weak 훅
 *
 * @data:      실제로는 struct pci_dev *. 콜백 시그니처가 void * 라 캐스팅해 쓴다.
 * @res:       배치하려는 자원
 * @empty_res: 검토 중인 빈 구간
 * @size:      필요한 크기
 * @align:     요구 정렬
 * @return:    조정된 시작 주소.
 *
 * 기본 구현은 pci_align_resource() 로 그대로 넘긴다. 아키텍처가 추가 제약을
 * 갖고 있으면 이 함수를 덮어써서 그 위에 자기 규칙을 얹는다 — 예컨대 x86 은
 * 레거시 I/O 영역(0x3B0~0x3DF 등)을 피해 가는 처리를 여기에 넣는다.
 *
 * 첫 인자가 void * 인 것은 커널 공통 할당기(allocate_resource)의 콜백
 * 시그니처를 따라야 하기 때문이다. 그쪽은 PCI 를 모르므로 불투명 포인터로만
 * 받고, 해석은 이 함수가 한다.
 *
 * 실행 컨텍스트: 자원 트리 락 안. 잠들 수 없다.
 * 호출자: kernel/resource.c 의 allocate_resource().
 */
resource_size_t __weak pcibios_align_resource(void *data,
					      const struct resource *res,
					      const struct resource *empty_res,
					      resource_size_t size,
					      resource_size_t align)
{
	struct pci_dev *dev = data;

	return pci_align_resource(dev, res, empty_res, size, align);
}

/*
 * [한국어]
 * __pci_assign_resource - 이 버스의 창들에서 자원이 들어갈 자리를 찾는다
 *
 * @bus:   찾아볼 버스(이 버스의 창과 그 조상들을 훑는다)
 * @dev:   대상 장치
 * @resno: 자원 번호
 * @size:  필요한 크기
 * @align: 요구 정렬
 * @return: 0 = 자리를 찾아 배정, 음수 = 실패.
 *
 * 창의 종류를 세 단계로 좁혀 가며 시도한다. 순서가 중요하다.
 *
 *   1) 정확히 일치하는 창부터 - 자원의 (prefetch, 64bit) 조합과 완전히
 *      같은 창을 찾는다. 원문 주석이 그 이유를 밝힌다: 64비트 프리페치
 *      창이 4GB 아래에 있더라도 32비트 프리페치 자원을 넣으면 안 된다.
 *      크기를 계산한 pbus_size_mem() 이 "64비트 창에는 32비트 자원이
 *      없다" 고 전제했기 때문이다. 계산과 다르게 배치하면 나중에 자리가
 *      모자란다.
 *
 *   2) 64비트 프리페치 자원을 32비트 프리페치 창에 - 프리페치 창이
 *      32비트뿐인 시스템에서는 이렇게 해야 한다. 64비트 자원을 4GB
 *      아래에 두는 것은 언제나 유효하므로 안전하다.
 *
 *   3) 아무 메모리 창에나 - 마지막 수단. 프리페치 가능한 자원을
 *      비프리페치 창에 넣으면 성능은 손해지만 동작은 한다.
 *      다만 32비트 비프리페치 자원은 1단계에서 이미 유일한 가능성을
 *      시도했으므로 여기서 다시 하지 않는다(그 조건이 마지막 if 다).
 *
 * PCIBIOS_MIN_IO / PCIBIOS_MIN_MEM 하한을 두는 이유: 주소 공간의 맨
 * 아래쪽에는 레거시 장치와 시스템 영역이 있어 PCI 자원을 놓을 수 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: _pci_assign_resource().
 * 피호출자: pci_bus_alloc_resource() [bus.c].
 *
 * 호출 체인:
 *   pci_assign_resource -> _pci_assign_resource -> [__pci_assign_resource]
 *     -> pci_bus_alloc_resource -> allocate_resource
 */
static int __pci_assign_resource(struct pci_bus *bus, struct pci_dev *dev,
		int resno, resource_size_t size, resource_size_t align)
{
	struct resource *res = pci_resource_n(dev, resno);
	resource_size_t min;
	int ret;

	min = (res->flags & IORESOURCE_IO) ? PCIBIOS_MIN_IO : PCIBIOS_MIN_MEM;

	/*
	 * First, try exact prefetching match.  Even if a 64-bit
	 * prefetchable bridge window is below 4GB, we can't put a 32-bit
	 * prefetchable resource in it because pbus_size_mem() assumes a
	 * 64-bit window will contain no 32-bit resources.  If we assign
	 * things differently than they were sized, not everything will fit.
	 */
	ret = pci_bus_alloc_resource(bus, res, size, align, min,
				     IORESOURCE_PREFETCH | IORESOURCE_MEM_64,
				     pcibios_align_resource, dev);
	if (ret == 0)
		return 0;

	/*
	 * If the prefetchable window is only 32 bits wide, we can put
	 * 64-bit prefetchable resources in it.
	 */
	if ((res->flags & (IORESOURCE_PREFETCH | IORESOURCE_MEM_64)) ==
	     (IORESOURCE_PREFETCH | IORESOURCE_MEM_64)) {
		ret = pci_bus_alloc_resource(bus, res, size, align, min,
					     IORESOURCE_PREFETCH,
					     pcibios_align_resource, dev);
		if (ret == 0)
			return 0;
	}

	/*
	 * If we didn't find a better match, we can put any memory resource
	 * in a non-prefetchable window.  If this resource is 32 bits and
	 * non-prefetchable, the first call already tried the only possibility
	 * so we don't need to try again.
	 */
	if (res->flags & (IORESOURCE_PREFETCH | IORESOURCE_MEM_64))
		ret = pci_bus_alloc_resource(bus, res, size, align, min, 0,
					     pcibios_align_resource, dev);

	return ret;
}

/*
 * [한국어]
 * _pci_assign_resource - 자리를 찾을 때까지 상위 버스로 올라가며 재시도한다
 *
 * @dev:       대상 장치
 * @resno:     자원 번호
 * @size:      필요한 크기
 * @min_align: 요구 정렬
 * @return: 0 = 배정 성공, 음수 = 어느 버스에서도 자리를 못 찾음.
 *
 * __pci_assign_resource() 를 이 장치의 버스에서 시작해 위로 올라가며
 * 반복 호출한다. 어디서 멈추는지가 이 함수의 핵심 논리다.
 *
 * transparent bridge(투명 브리지)라는 개념을 알아야 한다. 보통의 브리지는
 * 자기 윈도우 안의 주소만 하위로 통과시키지만, transparent bridge 는
 * 상위 버스의 주소 공간을 그대로 물려받아 필터 없이 통과시킨다.
 * 그래서 그 뒤의 장치는 상위 버스의 창을 자기 것처럼 쓸 수 있다.
 *
 * 루프는 그 성질을 이용한다 — 현재 버스에서 실패하면, 상위로 올라가는 길이
 * transparent 인 동안 계속 올라가며 시도한다. 불투명 브리지를 만나면
 * 그 위의 주소는 하위로 내려올 수 없으므로 거기서 멈춘다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: pci_assign_resource(), pci_reassign_resource().
 */
static int _pci_assign_resource(struct pci_dev *dev, int resno,
				resource_size_t size, resource_size_t min_align)
{
	struct pci_bus *bus;
	int ret;

	bus = dev->bus;
	while ((ret = __pci_assign_resource(bus, dev, resno, size, min_align))) {
		if (!bus->parent || !bus->self->transparent)
			break;
		bus = bus->parent;
	}

	return ret;
}

/*
 * [한국어]
 * pci_assign_resource - 자원 하나에 주소를 배정하고 하드웨어에 기록한다
 *
 * @dev:   대상 장치
 * @resno: 자원 번호
 * @return: 0 = 배정 완료, 음수 = 어디에도 넣지 못함.
 *
 * 이 파일의 주 진입점이다. 배치 계산이 아니라 "이 자원 하나를 어디든
 * 넣어라" 는 요청을 처리한다. 흐름은 네 단계다.
 *
 *   1) 크기와 정렬을 구한다. 정렬이 0 이면 자원 자체가 잘못된 것이라
 *      바로 실패한다 — 크기가 0 인 BAR 는 애초에 배치 대상이 아니다.
 *   2) _pci_assign_resource() 로 자리를 찾는다. 실패하면 상위 버스로
 *      올라가며 재시도하는 것이 그 함수의 일이다.
 *   3) 그래도 실패하면 pci_revert_fw_address() 로 펌웨어 주소를 시도한다.
 *      다만 대부분의 아키텍처에서 이것은 곧바로 실패한다.
 *   4) 성공했으면 pci_update_resource() 로 BAR 레지스터에 기록한다.
 *
 * 3단계가 "최후의 수단" 인 이유: 커널이 자리를 못 찾았다는 것은 자원
 * 트리가 이미 꽉 찼다는 뜻인데, 펌웨어 주소는 그 트리에 등록되지 않은
 * 구간일 수 있다. 위험하지만 아무 주소도 없는 것보다는 낫다는 판단이다.
 *
 * NVMe 학습 관점: NVMe SSD 의 BAR0 가 이 경로로 주소를 얻는다. 여기서
 * 실패하면 pci_enable_device_mem() 이 -ENOMEM 등을 돌려주고,
 * nvme_probe() 가 실패해 그 드라이브는 아예 보이지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: setup-bus.c 의 배치 경로, hotplug 재스캔.
 *
 * 호출 체인:
 *   pci_assign_unassigned_bus_resources -> [pci_assign_resource]
 *     -> _pci_assign_resource -> __pci_assign_resource -> pci_bus_alloc_resource
 *     -> pci_update_resource -> pci_write_config_dword(BAR)
 */
int pci_assign_resource(struct pci_dev *dev, int resno)
{
	struct resource *res = pci_resource_n(dev, resno);
	const char *res_name = pci_resource_name(dev, resno);
	resource_size_t align, size;
	int ret;

	if (res->flags & IORESOURCE_PCI_FIXED)
		return 0;

	res->flags |= IORESOURCE_UNSET;
	align = pci_resource_alignment(dev, res);
	if (!align) {
		pci_info(dev, "%s %pR: can't assign; bogus alignment\n",
			 res_name, res);
		return -EINVAL;
	}

	size = resource_size(res);
	ret = _pci_assign_resource(dev, resno, size, align);

	/*
	 * If we failed to assign anything, let's try the address
	 * where firmware left it.  That at least has a chance of
	 * working, which is better than just leaving it disabled.
	 */
	if (ret < 0) {
		pci_info(dev, "%s %pR: can't assign; no space\n", res_name, res);
		ret = pci_revert_fw_address(res, dev, resno, size);
	}

	if (ret < 0) {
		pci_info(dev, "%s %pR: failed to assign\n", res_name, res);
		return ret;
	}

	res->flags &= ~IORESOURCE_UNSET;
	res->flags &= ~IORESOURCE_STARTALIGN;
	if (pci_resource_is_bridge_win(resno))
		res->flags &= ~IORESOURCE_DISABLED;

	pci_info(dev, "%s %pR: assigned\n", res_name, res);
	if (resno < PCI_BRIDGE_RESOURCES)
		pci_update_resource(dev, resno);

	return 0;
}
EXPORT_SYMBOL(pci_assign_resource);

/*
 * [한국어]
 * pci_reassign_resource - 이미 배정된 자원을 더 크게 만들어 다시 배정한다
 *
 * @dev:       대상 장치
 * @resno:     자원 번호
 * @addsize:   기존 크기에 더할 바이트 수
 * @min_align: 새로 요구할 최소 정렬
 * @return: 0 = 재배정 성공, 음수 = 실패(이 경우 원래 크기와 주소가 복원된다).
 *
 * 주로 브리지 윈도우를 넓힐 때 쓴다. 핫플러그로 장치가 추가되어 그 아래
 * 자원이 늘어나면, 감싸는 창도 커져야 하기 때문이다.
 *
 * 실패 시 원상 복구가 이 함수의 핵심이다. 크기를 늘려 재배정을 시도했다가
 * 실패하면, 원래 크기와 주소로 되돌려 놓는다. 그러지 않으면 "커진 채로
 * 자리는 없는" 상태가 되어 이후 모든 계산이 어긋난다.
 *
 * 이 함수를 "최선의 노력" 으로 쓴다는 점도 중요하다 — 실패해도 시스템이
 * 망가지지 않는다. 그냥 창이 원래 크기로 남고, 새 장치가 자원을 못 받아
 * 동작하지 않을 뿐이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: setup-bus.c 의 add_size 처리 경로(선택적 추가 크기 반영).
 */
int pci_reassign_resource(struct pci_dev *dev, int resno,
			  resource_size_t addsize, resource_size_t min_align)
{
	struct resource *res = pci_resource_n(dev, resno);
	const char *res_name = pci_resource_name(dev, resno);
	unsigned long flags;
	resource_size_t new_size;
	int ret;

	if (res->flags & IORESOURCE_PCI_FIXED)
		return 0;

	flags = res->flags;
	res->flags |= IORESOURCE_UNSET;
	if (!res->parent) {
		pci_info(dev, "%s %pR: can't reassign; unassigned resource\n",
			 res_name, res);
		return -EINVAL;
	}

	new_size = resource_size(res) + addsize;
	ret = _pci_assign_resource(dev, resno, new_size, min_align);
	if (ret) {
		res->flags = flags;
		pci_info(dev, "%s %pR: failed to expand by %#llx\n",
			 res_name, res, (unsigned long long) addsize);
		return ret;
	}

	res->flags &= ~IORESOURCE_UNSET;
	res->flags &= ~IORESOURCE_STARTALIGN;
	pci_info(dev, "%s %pR: reassigned; expanded by %#llx\n",
		 res_name, res, (unsigned long long) addsize);
	if (resno < PCI_BRIDGE_RESOURCES)
		pci_update_resource(dev, resno);

	return 0;
}

/*
 * [한국어]
 * pci_release_resource - 자원을 트리에서 놓고 "주소 미정" 상태로 되돌린다
 *
 * @dev:   대상 장치
 * @resno: 자원 번호
 * @return: 0 = 해제 성공, 음수 = 애초에 등록돼 있지 않았음.
 *
 * pci_claim_resource() 의 반대다. 커널 자원 트리에서 구간을 빼고,
 * struct resource 에 IORESOURCE_UNSET 을 달아 "이 자원은 아직 주소가
 * 없다" 고 표시한다.
 *
 * 하드웨어 BAR 는 건드리지 않는다는 점에 주의. 자원 트리와 struct resource
 * 만 바뀌고, 실제 BAR 레지스터에는 옛 주소가 그대로 남는다. 그래서 이
 * 함수를 부른 뒤에는 Memory 디코딩을 꺼 두거나 곧바로 새 주소를 배정해야
 * 한다 — 그러지 않으면 그 구간을 물려받은 다른 장치와 충돌한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: 재배치 경로(setup-bus.c), 장치 제거(remove.c 의 pci_free_resources).
 */
int pci_release_resource(struct pci_dev *dev, int resno)
{
	struct resource *res = pci_resource_n(dev, resno);
	const char *res_name = pci_resource_name(dev, resno);
	int ret;

	if (!res->parent)
		return 0;

	pci_info(dev, "%s %pR: releasing\n", res_name, res);

	ret = release_resource(res);
	if (ret)
		return ret;
	res->end = resource_size(res) - 1;
	res->start = 0;
	res->flags |= IORESOURCE_UNSET;

	return 0;
}
EXPORT_SYMBOL(pci_release_resource);

/*
 * [한국어]
 * pci_enable_resources - Command 레지스터의 디코딩 비트를 켜서 BAR 접근을 연다
 *
 * @dev:  대상 장치
 * @mask: 켤 자원들의 비트마스크. 비트 i 가 1 이면 i 번 자원을 쓰겠다는 뜻이다.
 *        pci_select_bars() 가 만들어 주는 값이다.
 * @return: 0 = 성공, -EINVAL = 요청한 자원 중 주소가 배정되지 않은 것이 있음.
 *
 * 배치가 끝난 자원을 실제로 "쓸 수 있게" 만드는 마지막 단계다. BAR 에
 * 주소가 들어 있어도 Command 레지스터의 Memory Space Enable(또는 I/O Space
 * Enable)이 꺼져 있으면 장치가 그 주소로 오는 접근을 무시한다.
 *
 * 하는 일은 셋이다.
 *   1) 요청한 자원들을 훑어 종류를 확인하고, 필요한 Enable 비트를 모은다.
 *   2) 그중 하나라도 IORESOURCE_UNSET(주소 미배정)이면 실패한다. 주소도
 *      없는 BAR 의 디코딩을 켜면 엉뚱한 구간에 응답하게 되기 때문이다.
 *   3) ROM 이 활성화돼 있으면 Memory Enable 도 함께 켠다 — ROM BAR 도
 *      메모리 공간이기 때문이다.
 *
 * 값이 바뀔 때만 config 쓰기를 한다(old_cmd 와 비교). 불필요한 쓰기를
 * 줄이는 것도 있지만, 로그를 남기는 조건이기도 해서 "무엇이 새로 켜졌는지"
 * 가 dmesg 에 정확히 나타난다.
 *
 * NVMe 학습 관점: 이 파일에서 NVMe 와 가장 가까운 함수다. NVMe 드라이버가
 * 부르는 pci_enable_device_mem() 이 결국 여기로 내려와, BAR0 의 Memory
 * Space Enable 을 켠다. 이것이 켜지지 않으면 ioremap 한 주소로 CAP 레지스터를
 * 읽어도 all-ones 만 돌아온다.
 *
 * 다만 Bus Master Enable(DMA 허용)은 여기서 켜지 않는다. 그것은
 * pci_set_master() 의 몫이고, NVMe 드라이버가 따로 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 접근).
 * 호출자: pci.c 의 do_pci_enable_device() -> pci_enable_device_mem() 경로.
 *
 * 호출 체인:
 *   nvme_pci_enable -> pci_enable_device_mem -> do_pci_enable_device
 *     -> [pci_enable_resources] -> pci_write_config_word(PCI_COMMAND)
 */
int pci_enable_resources(struct pci_dev *dev, int mask)
{
	u16 cmd, old_cmd;
	int i;
	struct resource *r;
	const char *r_name;

	pci_read_config_word(dev, PCI_COMMAND, &cmd);
	old_cmd = cmd;

	pci_dev_for_each_resource(dev, r, i) {
		if (!(mask & (1 << i)))
			continue;

		r_name = pci_resource_name(dev, i);

		if (!(r->flags & (IORESOURCE_IO | IORESOURCE_MEM)))
			continue;
		if (pci_resource_is_optional(dev, i))
			continue;

		if (i < PCI_BRIDGE_RESOURCES) {
			if (r->flags & IORESOURCE_UNSET) {
				pci_err(dev, "%s %pR: not assigned; can't enable device\n",
					r_name, r);
				return -EINVAL;
			}

			if (!r->parent) {
				pci_err(dev, "%s %pR: not claimed; can't enable device\n",
					r_name, r);
				return -EINVAL;
			}
		}

		if (r->parent) {
			if (r->flags & IORESOURCE_IO)
				cmd |= PCI_COMMAND_IO;
			if (r->flags & IORESOURCE_MEM)
				cmd |= PCI_COMMAND_MEMORY;
		}
	}

	if (cmd != old_cmd) {
		pci_info(dev, "enabling device (%04x -> %04x)\n", old_cmd, cmd);
		pci_write_config_word(dev, PCI_COMMAND, cmd);
	}
	return 0;
}
