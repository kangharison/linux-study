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
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/setup-res.c)은 PCI endpoint(NVMe SSD 포함)의
 * BAR(Base Address Register) 리소스를 할당/갱신/활성화하고, 상위
 * bridge의 window를 설정한다.
 * NVMe SSD는 PCIe endpoint로서 최소한 BAR0(register/doorbell 영역)을
 * 사용하며, 때로는 추가 BAR/ROM 리소스를 갖는다. 이 파일에서 수행하는
 * resource 설정이 NVMe 드라이버가 장치에 접근하고 DMA/MSI-X/IRQ/ASPM
 * 등을 사용할 수 있는 기반이 된다.
 * NVMe 드라이버와의 주요 호출 경로:
 *   nvme_probe -> pci_enable_device -> pci_assign_resource /
 *   pci_enable_resources -> PCI_COMMAND_MEMORY/IO 활성화 ->
 *   pci_iomap(BAR0) -> doorbell/queue register 접근
 *   NVMe MSI-X 초기화(pci_enable_msix_range) 전에 BAR 리소스가 올바르게
 *   할당/claim되어야 하며, MSI-X table이 들어 있는 BAR도 동일한 메커니즘으로
 *   설정될 수 있다.
 *   DMA 마스터 동작을 위해 PCI_COMMAND_MEMORY가 켜져야 하고, ASPM/전원
 *   관리 시에도 BAR/command register 상태가 중요하다.
 * 본 파일은 drivers/nvme/host/pci.c가 직접 호출하지 않지만, NVMe endpoint의
 * BAR 할당, bridge window, DMA/IRQ/MSI-X/ASPM에 필요한 PCI config space
 * 작성을 담당하는 핵심 서브시스템 코드이다.
 * ===================================================================
 */

#include <linux/kernel.h> /* NVMe: 커널 기본 타입과 함수 정의 */
#include <linux/export.h> /* NVMe: EXPORT_SYMBOL 관련 매크로 */
#include <linux/pci.h> /* NVMe: PCI/NVMe 공용 구조체와 API 선언 */
#include <linux/errno.h> /* NVMe: 오류 코드 상수 */
#include <linux/ioport.h> /* NVMe: I/O/MEM 리소스 트리 관리 */
#include <linux/cache.h> /* NVMe: 캐시 정렬 관련 */
#include <linux/slab.h> /* NVMe: 메모리 할당 */
#include "pci.h" /* NVMe: PCI 서브시스템 낮부 헤더 */

/*
 * pci_std_update_resource:
 *   NVMe endpoint의 특정 BAR 레지스터에 bus 주소를 기록한다.
 *   NVMe driver가 pci_iomap()으로 매핑하기 전, BAR0/MSI-X BAR 등의
 *   주소가 PCI config space에 반영되는 단계이다.
 *   64비트 BAR의 경우 PCI_COMMAND_MEMORY를 일시적으로 끄고 갱신한다.
 */
static void pci_std_update_resource(struct pci_dev *dev, int resno)
{
	struct pci_bus_region region; /* NVMe: BAR에 기록할 bus 주소 영역 임시 변수 */
	bool disable; /* NVMe: 64비트 BAR 갱신 시 메모리 디코딩을 일시 끌지 여부 */
	u16 cmd; /* NVMe: PCI_COMMAND 레지스터 값을 저장해 복원 */
	u32 new, check, mask; /* NVMe: BAR에 쓸 새 주소, 확인용 읽기 값, 필터 마스크 */
	int reg; /* NVMe: 대상 BAR 레지스터의 config space 오프셋 */
	struct resource *res = pci_resource_n(dev, resno); /* NVMe: NVMe BAR(예: BAR0 doorbell) 리소스 포인터 */
	const char *res_name = pci_resource_name(dev, resno); /* NVMe: 로그 출력용 BAR 이름 */

	/* Per SR-IOV spec 3.4.1.11, VF BARs are RO zero */
	if (dev->is_virtfn) /* NVMe: SR-IOV VF의 BAR은 읽기 전용이므로 건 너뛴다 */
		return; /* NVMe: VF BAR 갱신 중단 */

	/*
	 * Ignore resources for unimplemented BARs and unused resource slots
	 * for 64 bit BARs.
	 */
	if (!res->flags) /* NVMe: 미구현 BAR 또는 64비트 BAR 상위 슬롯이면 무시 */
		return; /* NVMe: 갱신할 필요 없음 */

	if (res->flags & IORESOURCE_UNSET) /* NVMe: 주소가 아직 할당되지 않은 BAR은 config에 기록하지 않는다 */
		return; /* NVMe: UNSET 상태 유지 */

	/*
	 * Ignore non-moveable resources.  This might be legacy resources for
	 * which no functional BAR register exists or another important
	 * system resource we shouldn't move around.
	 */
	if (res->flags & IORESOURCE_PCI_FIXED) /* NVMe: firmware/BIOS가 고정한 BAR은 이동 불가 */
		return; /* NVMe: 고정 BAR 보존 */

	pcibios_resource_to_bus(dev->bus, &region, res); /* NVMe: CPU 물리 주소를 PCIe bus 주소로 변환(BAR에는 bus 주소 기록) */
	new = region.start; /* NVMe: BAR에 쓸 시작 bus 주소 */

	if (res->flags & IORESOURCE_IO) { /* NVMe: I/O 공간 BAR이면 IO 마스크/플래그 적용 */
		mask = (u32)PCI_BASE_ADDRESS_IO_MASK; /* NVMe: I/O BAR 하위 2비트 마스크 */
		new |= res->flags & ~PCI_BASE_ADDRESS_IO_MASK; /* NVMe: BAR 하위 플래그 비트 보존 */
	} else if (resno == PCI_ROM_RESOURCE) { /* NVMe: ROM BAR이면 전용 마스크 사용 */
		mask = PCI_ROM_ADDRESS_MASK; /* NVMe: ROM 주소 마스크 */
	} else { /* NVMe: 메모리 BAR(32/64비트, prefetchable) 처리 */
		mask = (u32)PCI_BASE_ADDRESS_MEM_MASK; /* NVMe: 메모리 BAR 주소/플래그 마스크 */
		new |= res->flags & ~PCI_BASE_ADDRESS_MEM_MASK; /* NVMe: prefetchable 등 하위 플래그 보존 */
	}

	if (resno < PCI_ROM_RESOURCE) { /* NVMe: 일반 BAR의 config 오프셋 계산 */
		reg = PCI_BASE_ADDRESS_0 + 4 * resno; /* NVMe: BAR0/1/... 레지스터 오프셋 산출 */
	} else if (resno == PCI_ROM_RESOURCE) { /* NVMe: ROM BAR 처리 분기 */

		/*
		 * Apparently some Matrox devices have ROM BARs that read
		 * as zero when disabled, so don't update ROM BARs unless
		 * they're enabled.  See
		 * https://lore.kernel.org/r/43147B3D.1030309@vc.cvut.cz/
		 * But we must update ROM BAR for buggy devices where even a
		 * disabled ROM can conflict with other BARs.
		 */
		if (!(res->flags & IORESOURCE_ROM_ENABLE) && /* NVMe: ROM이 비활성화면 갱신 생략(단 중첩 버그 장치 제외) */
		    !dev->rom_bar_overlap) /* NVMe: ROM BAR이 다른 BAR과 중첩되지 않으면 스킵 */
			return; /* NVMe: ROM BAR 갱신 생략 */

		reg = dev->rom_base_reg; /* NVMe: ROM BAR config 오프셋 */
		if (res->flags & IORESOURCE_ROM_ENABLE) /* NVMe: ROM 활성화 시 ENABLE 비트 추가 */
			new |= PCI_ROM_ADDRESS_ENABLE; /* NVMe: ROM BAR enable bit 설정 */
	} else /* NVMe: ROM 이외의 bridge 리소스는 여기서 처리하지 않음 */
		return; /* NVMe: 갱신 대상 아님 */

	/*
	 * We can't update a 64-bit BAR atomically, so when possible,
	 * disable decoding so that a half-updated BAR won't conflict
	 * with another device.
	 */
	disable = (res->flags & IORESOURCE_MEM_64) && !dev->mmio_always_on; /* NVMe: 64비트 BAR은 원자적 갱신이 불가하므로 디코딩 끔 */
	if (disable) { /* NVMe: 메모리 디코딩 일시 중단 필요 시 */
		pci_read_config_word(dev, PCI_COMMAND, &cmd); /* NVMe: 현재 PCI_COMMAND(메모리/IO 마스터 비트 포함) 읽기 */
		pci_write_config_word(dev, PCI_COMMAND, /* NVMe: MEMORY 비트만 클리어하여 NVMe endpoint BAR 디코딩 중단 */
				      cmd & ~PCI_COMMAND_MEMORY); /* NVMe: PCI_COMMAND_MEMORY 비트 제거 */
	}

	pci_write_config_dword(dev, reg, new); /* NVMe: 계산된 bus 주소를 BAR 레지스터에 기록 */
	pci_read_config_dword(dev, reg, &check); /* NVMe: BAR에 정상 기록되었는지 확인용 읽기 */

	if ((new ^ check) & mask) { /* NVMe: 쓴 값과 읽은 값이 마스크 영역에서 다륾면 실패 */
		pci_err(dev, "%s: error updating (%#010x != %#010x)\n", /* NVMe: BAR 갱신 실패 로그(잘못된 doorbell 주소 원인 가능) */
			res_name, new, check); /* NVMe: 오류 메시지 인자 전달 */
	}

	if (res->flags & IORESOURCE_MEM_64) { /* NVMe: 64비트 BAR의 상위 32비트도 기록 */
		new = region.start >> 16 >> 16; /* NVMe: 64비트 주소의 상위 32비트 추출 */
		pci_write_config_dword(dev, reg + 4, new); /* NVMe: BAR+4에 상위 32비트 기록 */
		pci_read_config_dword(dev, reg + 4, &check); /* NVMe: 상위 32비트 확인 읽기 */
		if (check != new) { /* NVMe: 상위 32비트 불일치 시 실패 */
			pci_err(dev, "%s: error updating (high %#010x != %#010x)\n", /* NVMe: 상위 32비트 갱신 실패 로그 */
				res_name, new, check); /* NVMe: 오류 메시지 인자 전달 */
		}
	}

	if (disable) /* NVMe: 갱신 완료 후 이전 PCI_COMMAND 복원 */
		pci_write_config_word(dev, PCI_COMMAND, cmd); /* NVMe: NVMe endpoint의 메모리 디코딩 원래대로 복원 */
}

/*
 * pci_update_resource:
 *   pci_std_update_resource()나 SR-IOV VF용 갱신 함수를 호출하는 wrapper.
 *   NVMe BAR(일반 BAR 또는 MSI-X가 포함된 BAR)의 config space 갱신을
 *   적절한 경로로 분기한다.
 */
void pci_update_resource(struct pci_dev *dev, int resno)
{
	if (resno <= PCI_ROM_RESOURCE) /* NVMe: 일반 BAR(0~ROM)은 표준 갱신 경로 사용 */
		pci_std_update_resource(dev, resno); /* NVMe: 해당 BAR의 config space 갱신 수행 */
	else if (pci_resource_is_iov(resno)) /* NVMe: SR-IOV VF BAR인 경우 */
		pci_iov_update_resource(dev, resno); /* NVMe: VF BAR 갱신 함수 호출 */
}

/*
 * pci_claim_resource:
 *   NVMe BAR가 상위 bridge window 내에서 중복 없이 등록되는지 확인한다.
 *   BAR 주소가 확복되어야 pci_request_regions() 및 pci_iomap()이 가능.
 */
int pci_claim_resource(struct pci_dev *dev, int resource)
{
	struct resource *res = &dev->resource[resource]; /* NVMe: claim하려는 NVMe BAR 리소스 */
	const char *res_name = pci_resource_name(dev, resource); /* NVMe: 로그용 BAR 이름 */
	struct resource *root, *conflict; /* NVMe: 상위 bridge window와 충돌 리소스 */

	if (res->flags & IORESOURCE_UNSET) { /* NVMe: 주소 미할당 BAR은 claim할 수 없다 */
		pci_info(dev, "%s %pR: can't claim; no address assigned\n", /* NVMe: 미할당 로그(BAR0 doorbell 매핑 실패 원인) */
			 res_name, res); /* NVMe: 로그 인자 전달 */
		return -EINVAL; /* NVMe: claim 실패 */
	}

	/*
	 * If we have a shadow copy in RAM, the PCI device doesn't respond
	 * to the shadow range, so we don't need to claim it, and upstream
	 * bridges don't need to route the range to the device.
	 */
	if (res->flags & IORESOURCE_ROM_SHADOW) /* NVMe: RAM에 섀도우된 ROM은 PCI 응답 안 함 */
		return 0; /* NVMe: claim 필요 없음 */

	root = pci_find_parent_resource(dev, res); /* NVMe: NVMe BAR가 속할 상위 bridge window 탐색 */
	if (!root) { /* NVMe: 호환 bridge window가 없으면 BAR 사용 불가 */
		pci_info(dev, "%s %pR: can't claim; no compatible bridge window\n", /* NVMe: 상위 window 부재 로그 */
			 res_name, res); /* NVMe: 로그 인자 전달 */
		res->flags |= IORESOURCE_UNSET; /* NVMe: BAR를 미할당 상태로 되돌림 */
		return -EINVAL; /* NVMe: claim 실패 */
	}

	conflict = request_resource_conflict(root, res); /* NVMe: 상위 window 내에서 리소스 등록(충돌 검사) */
	if (conflict) { /* NVMe: 다른 장치와 주소가 겹치면 claim 실패 */
		pci_info(dev, "%s %pR: can't claim; address conflict with %s %pR\n", /* NVMe: 주소 충돌 로그 */
			 res_name, res, conflict->name, conflict); /* NVMe: 충돌 리소스 정보 전달 */
		res->flags |= IORESOURCE_UNSET; /* NVMe: 충돌로 인해 BAR 미할당 처리 */
		return -EBUSY; /* NVMe: 리소스 사용 중 */
	}

	return 0; /* NVMe: claim 성공 */
}
EXPORT_SYMBOL(pci_claim_resource); /* NVMe: 다른 드라이버에서도 pci_claim_resource 사용 */

/*
 * pci_disable_bridge_window:
 *   bridge의 MMIO/Prefetchable MMIO window를 닫는다.
 *   NVMe 장치가 연결된 downstream bridge의 주소 라우팅을 차단하여
 *   DMA/MSI-X/IRQ 트래픽이 더 이상 해당 방향으로 흐르지 않도록 한다.
 */
void pci_disable_bridge_window(struct pci_dev *dev)
{
	/* MMIO Base/Limit */
	pci_write_config_dword(dev, PCI_MEMORY_BASE, 0x0000fff0); /* NVMe: bridge의 non-prefetchable MMIO window base/limit를 무효화 */

	/* Prefetchable MMIO Base/Limit */
	pci_write_config_dword(dev, PCI_PREF_LIMIT_UPPER32, 0); /* NVMe: prefetchable MMIO 상위 limit 0으로 설정 */
	pci_write_config_dword(dev, PCI_PREF_MEMORY_BASE, 0x0000fff0); /* NVMe: prefetchable MMIO base/limit 무효화 */
	pci_write_config_dword(dev, PCI_PREF_BASE_UPPER32, 0xffffffff); /* NVMe: prefetchable MMIO 상위 base를 최대로 설정해 window 폐쇄 */
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
 * pcibios_retrieve_fw_addr:
 *   firmware(BIOS/UEFI)가 NVMe BAR에 할당했던 주소를 되살릴 수 있는지
 *   반환한다. 아키텍처별로 재정의 가능한 __weak 함수.
 */
resource_size_t __weak pcibios_retrieve_fw_addr(struct pci_dev *dev, int idx)
{
	return 0; /* NVMe: firmware 주소를 복원할 수 없음을 의미 */
}

/*
 * pci_revert_fw_address:
 *   커널이 BAR를 새로 할당하지 못했을 때 firmware 주소로 되돌린다.
 *   NVMe 장치의 BAR0 등이 비활성화되지 않도록 최후의 수단으로 사용.
 */
static int pci_revert_fw_address(struct resource *res, struct pci_dev *dev,
		int resno, resource_size_t size) /* NVMe: 함수 매개변수 선언 (NVMe BAR 번호와 크기) */
{
	struct resource *root, *conflict; /* NVMe: 상위 bridge window와 충돌 리소스 */
	resource_size_t fw_addr, start, end; /* NVMe: firmware 주소, 원래 범위 백업 */
	const char *res_name = pci_resource_name(dev, resno); /* NVMe: 로그용 BAR 이름 */

	fw_addr = pcibios_retrieve_fw_addr(dev, resno); /* NVMe: firmware가 남긴 BAR 주소 획득 시도 */
	if (!fw_addr) /* NVMe: firmware 주소가 없으면 fallback 불가 */
		return -ENOMEM; /* NVMe: fallback 실패 */

	start = res->start; /* NVMe: 실패 시 복원할 시작 주소 저장 */
	end = res->end; /* NVMe: 실패 시 복원할 끝 주소 저장 */
	resource_set_range(res, fw_addr, size); /* NVMe: BAR 범위를 firmware 주소로 설정 */
	res->flags &= ~IORESOURCE_UNSET; /* NVMe: firmware 주소를 사용하므로 UNSET 클리어 */

	root = pci_find_parent_resource(dev, res); /* NVMe: firmware 주소가 들어갈 상위 window 탐색 */
	if (!root) { /* NVMe: 상위 window를 찾지 못한 경우 */
		/*
		 * If dev is behind a bridge, accesses will only reach it
		 * if res is inside the relevant bridge window.
		 */
		if (pci_upstream_bridge(dev)) /* NVMe: bridge 뒤에 있는 NVMe 장치는 window 외부 주소에 접근 불가 */
			return -ENXIO; /* NVMe: bridge window 외부이므로 실패 */

		/*
		 * On the root bus, assume the host bridge will forward
		 * everything.
		 */
		if (res->flags & IORESOURCE_IO) /* NVMe: IO 리소스면 루트 IO 리소스 사용 */
			root = &ioport_resource; /* NVMe: 시스템 전체 I/O 포트 트리 */
		else /* NVMe: MEM 리소스면 루트 MEM 리소스 사용 */
			root = &iomem_resource; /* NVMe: 시스템 전체 physical memory I/O 트리 */
	}

	pci_info(dev, "%s: trying firmware assignment %pR\n", res_name, res); /* NVMe: firmware 주소로 재시도 로그 */
	conflict = request_resource_conflict(root, res); /* NVMe: firmware 주소 등록 및 충돌 검사 */
	if (conflict) { /* NVMe: 충돌 시 원래 값 복원 */
		pci_info(dev, "%s %pR: conflicts with %s %pR\n", res_name, res, /* NVMe: 충돌 리소스 로그 */
			 conflict->name, conflict); /* NVMe: 충돌 리소스 정보 전달 */
		res->start = start; /* NVMe: 시작 주소 복원 */
		res->end = end; /* NVMe: 끝 주소 복원 */
		res->flags |= IORESOURCE_UNSET; /* NVMe: 다시 UNSET 상태로 표시 */
		return -EBUSY; /* NVMe: firmware 주소도 사용 불가 */
	}
	return 0; /* NVMe: firmware 주소 fallback 성공 */
}

/*
 * For mem bridge windows, try to relocate tail remainder space to space
 * before res->start if there's enough free space there. This enables
 * tighter packing for resources.
 */
/*
 * pci_align_resource:
 *   bridge window 안에서 메모리 BAR의 시작 주소를 정렬 기준에 맞게
 *   미세 조정하여 공간을 촘촘하게 배치한다. NVMe BAR 할당 시 상위
 *   bridge의 granularity와 충돌하지 않도록 한다.
 */
resource_size_t pci_align_resource(struct pci_dev *dev,
				   const struct resource *res, /* NVMe: 기존 할당 후보 리소스 */
				   const struct resource *empty_res, /* NVMe: 비어 있는 영역 정보 */
				   resource_size_t size, /* NVMe: 요청 크기 */
				   resource_size_t align) /* NVMe: 요구 정렬 */
{
	resource_size_t remainder, start_addr; /* NVMe: 정렬 후 남은 공간과 후보 시작 주소 */

	if (!(res->flags & IORESOURCE_MEM)) /* NVMe: 메모리 BAR이 아니면 정렬 조정 불필요 */
		return res->start; /* NVMe: 그대로 반환 */

	if (IS_ALIGNED(size, align)) /* NVMe: 이미 정렬에 딱 맞으면 조정 불필요 */
		return res->start; /* NVMe: 그대로 반환 */

	remainder = size - ALIGN_DOWN(size, align); /* NVMe: 정렬 단위로 채우지 못한 꼬리 공간 계산 */
	/* Don't mess with size that doesn't align with window size granularity */
	if (!IS_ALIGNED(remainder, pci_min_window_alignment(dev->bus, res->flags))) /* NVMe: bridge window 세분성과 맞지 않으면 조정 불가 */
		return res->start; /* NVMe: 그대로 반환 */
	/* Try to place remainder that doesn't fill align before */
	if (res->start < remainder) /* NVMe: 앞쪽에 여유 공간이 부족하면 조정 불가 */
		return res->start; /* NVMe: 그대로 반환 */
	start_addr = res->start - remainder; /* NVMe: 꼬리 공간을 앞쪽으로 밀어 시작 주소 계산 */
	if (empty_res->start > start_addr) /* NVMe: 비어 있는 영역이 후볳보다 뒤에 있으면 불가 */
		return res->start; /* NVMe: 그대로 반환 */

	pci_dbg(dev, "%pR: moving candidate start address below align to %llx\n", /* NVMe: 시작 주소 조정 로그 */
		res, (unsigned long long)start_addr); /* NVMe: 로그 인자 전달 */
	return start_addr; /* NVMe: 조정된 시작 주소 반환 */
}

/*
 * We don't have to worry about legacy ISA devices, so nothing to do here.
 * This is marked as __weak because multiple architectures define it; it should
 * eventually go away.
 */
/*
 * pcibios_align_resource:
 *   아키텍처별 정렬 함수의 __weak 기본 구현. NVMe BAR 정렬을 위해
 *   pci_align_resource()를 호출한다.
 */
resource_size_t __weak pcibios_align_resource(void *data,
					      const struct resource *res, /* NVMe: 정렬 대상 리소스 (매개변수) */
					      const struct resource *empty_res, /* NVMe: 비어 있는 영역 (매개변수) */
					      resource_size_t size, /* NVMe: 요청 크기 (매개변수) */
					      resource_size_t align) /* NVMe: 요구 정렬 (매개변수) */
{
	struct pci_dev *dev = data; /* NVMe: 정렬 대상 NVMe 장치 포인터 */

	return pci_align_resource(dev, res, empty_res, size, align); /* NVMe: 아키텍처 독립적 정렬 함수 호출 */
}

/*
 * __pci_assign_resource:
 *   주어진 NVMe BAR의 크기/정렬을 만족하는 bridge window 내 공간을
 *   할당한다. prefetchable, 64비트, non-prefetchable 순으로 시도.
 */
static int __pci_assign_resource(struct pci_bus *bus, struct pci_dev *dev,
		int resno, resource_size_t size, resource_size_t align) /* NVMe: 매개변수: BAR 번호, 크기, 정렬 */
{
	struct resource *res = pci_resource_n(dev, resno); /* NVMe: 할당할 NVMe BAR 리소스 */
	resource_size_t min; /* NVMe: 할당 최소 주소(IO/MEM 구분) */
	int ret; /* NVMe: 할당 결과 */

	min = (res->flags & IORESOURCE_IO) ? PCIBIOS_MIN_IO : PCIBIOS_MIN_MEM; /* NVMe: IO BAR이면 최소 IO, 아니면 최소 MEM 주소 */

	/*
	 * First, try exact prefetching match.  Even if a 64-bit
	 * prefetchable bridge window is below 4GB, we can't put a 32-bit
	 * prefetchable resource in it because pbus_size_mem() assumes a
	 * 64-bit window will contain no 32-bit resources.  If we assign
	 * things differently than they were sized, not everything will fit.
	 */
	ret = pci_bus_alloc_resource(bus, res, size, align, min, /* NVMe: 64비트 prefetchable window에 정확히 맞춰 할당 시도 */
				     IORESOURCE_PREFETCH | IORESOURCE_MEM_64, /* NVMe: prefetchable+64비트 조건 */
				     pcibios_align_resource, dev); /* NVMe: 정렬 콜백과 NVMe 장치 전달 */
	if (ret == 0) /* NVMe: 성공하면 바로 반환 */
		return 0; /* NVMe: BAR 할당 성공 */

	/*
	 * If the prefetchable window is only 32 bits wide, we can put
	 * 64-bit prefetchable resources in it.
	 */
	if ((res->flags & (IORESOURCE_PREFETCH | IORESOURCE_MEM_64)) == /* NVMe: 64비트 prefetchable BAR이면 32비트 prefetchable window도 시도 */
	     (IORESOURCE_PREFETCH | IORESOURCE_MEM_64)) { /* NVMe: prefetchable+64비트 플래그 비교 */
		ret = pci_bus_alloc_resource(bus, res, size, align, min, /* NVMe: 32비트 prefetchable window 할당 시도 */
					     IORESOURCE_PREFETCH, /* NVMe: prefetchable 조건 */
					     pcibios_align_resource, dev); /* NVMe: 정렬 콜백과 NVMe 장치 전달 */
		if (ret == 0) /* NVMe: 성공하면 반환 */
			return 0; /* NVMe: BAR 할당 성공 */
	}

	/*
	 * If we didn't find a better match, we can put any memory resource
	 * in a non-prefetchable window.  If this resource is 32 bits and
	 * non-prefetchable, the first call already tried the only possibility
	 * so we don't need to try again.
	 */
	if (res->flags & (IORESOURCE_PREFETCH | IORESOURCE_MEM_64)) /* NVMe: non-prefetchable window에도 시도할 대상 */
		ret = pci_bus_alloc_resource(bus, res, size, align, min, 0, /* NVMe: non-prefetchable window 할당 시도 */
					     pcibios_align_resource, dev); /* NVMe: 정렬 콜백과 NVMe 장치 전달 */

	return ret; /* NVMe: 할당 결과 반환(0이면 성공) */
}

/*
 * _pci_assign_resource:
 *   transparent bridge 뒤에 있는 NVMe 장치라면 상위 bus로 올라가며
 *   할당 가능한 window를 찾는다.
 */
static int _pci_assign_resource(struct pci_dev *dev, int resno,
				resource_size_t size, resource_size_t min_align) /* NVMe: 매개변수: 크기, 최소 정렬 */
{
	struct pci_bus *bus; /* NVMe: 현재 NVMe 장치가 속한 bus */
	int ret; /* NVMe: 할당 결과 */

	bus = dev->bus; /* NVMe: NVMe endpoint의 현재 bus */
	while ((ret = __pci_assign_resource(bus, dev, resno, size, min_align))) { /* NVMe: 상위 transparent bridge를 따라 할당 재시도 */
		if (!bus->parent || !bus->self->transparent) /* NVMe: root bus에 도달하거나 non-transparent bridge면 중단 */
			break; /* NVMe: 더 이상 올라갈 수 없음 */
		bus = bus->parent; /* NVMe: 상위 bus로 이동 */
	}

	return ret; /* NVMe: 최종 할당 결과 반환 */
}

/*
 * pci_assign_resource:
 *   NVMe BAR의 전체 할당 흐름: 정렬/크기 계산, 공간 할당, 실패 시
 *   firmware 주소 fallback, 할당 성공 시 config space에 기록.
 */
int pci_assign_resource(struct pci_dev *dev, int resno)
{
	struct resource *res = pci_resource_n(dev, resno); /* NVMe: 할당할 NVMe BAR 리소스 */
	const char *res_name = pci_resource_name(dev, resno); /* NVMe: 로그용 BAR 이름 */
	resource_size_t align, size; /* NVMe: BAR 정렬과 크기 */
	int ret; /* NVMe: 할당 결과 */

	if (res->flags & IORESOURCE_PCI_FIXED) /* NVMe: 고정 BAR은 할당할 필요 없음 */
		return 0; /* NVMe: 고정 BAR 유지 */

	res->flags |= IORESOURCE_UNSET; /* NVMe: 재할당 전 일단 UNSET 표시 */
	align = pci_resource_alignment(dev, res); /* NVMe: BAR의 요구 정렬 값 산출 */
	if (!align) { /* NVMe: 정렬 값이 0이면 잘못된 BAR */
		pci_info(dev, "%s %pR: can't assign; bogus alignment\n", /* NVMe: 잘못된 정렬 로그 */
			 res_name, res); /* NVMe: 로그 인자 전달 */
		return -EINVAL; /* NVMe: 할당 실패 */
	}

	size = resource_size(res); /* NVMe: BAR의 실제 크기 */
	ret = _pci_assign_resource(dev, resno, size, align); /* NVMe: 상위 bus를 따라 BAR 공간 할당 시도 */

	/*
	 * If we failed to assign anything, let's try the address
	 * where firmware left it.  That at least has a chance of
	 * working, which is better than just leaving it disabled.
	 */
	if (ret < 0) { /* NVMe: 커널 할당 실패 시 firmware 주소로 fallback */
		pci_info(dev, "%s %pR: can't assign; no space\n", res_name, res); /* NVMe: 공간 부족 로그 */
		ret = pci_revert_fw_address(res, dev, resno, size); /* NVMe: firmware가 남긴 주소로 되돌림 */
	}

	if (ret < 0) { /* NVMe: firmware fallback도 실패하면 최종 실패 */
		pci_info(dev, "%s %pR: failed to assign\n", res_name, res); /* NVMe: 최종 할당 실패 로그 */
		return ret; /* NVMe: 실패 코드 반환 */
	}

	res->flags &= ~IORESOURCE_UNSET; /* NVMe: 할당 성공, UNSET 클리어 */
	res->flags &= ~IORESOURCE_STARTALIGN; /* NVMe: 시작 정렬 플래그 클리어 */
	if (pci_resource_is_bridge_win(resno)) /* NVMe: bridge window 리소스면 disabled 클리어 */
		res->flags &= ~IORESOURCE_DISABLED; /* NVMe: bridge window 활성화 */

	pci_info(dev, "%s %pR: assigned\n", res_name, res); /* NVMe: BAR 할당 성공 로그 */
	if (resno < PCI_BRIDGE_RESOURCES) /* NVMe: endpoint BAR이면 config space에도 기록 */
		pci_update_resource(dev, resno); /* NVMe: BAR 레지스터에 bus 주소 기록 */

	return 0; /* NVMe: pci_assign_resource 성공 */
}
EXPORT_SYMBOL(pci_assign_resource); /* NVMe: NVMe 드라이버 등에서 간접 사용 */

/*
 * pci_reassign_resource:
 *   NVMe BAR의 크기를 확장(예: 리소스 핫플러그/재조정)하여 재할당하고
 *   config space를 갱신한다.
 */
int pci_reassign_resource(struct pci_dev *dev, int resno,
			  resource_size_t addsize, resource_size_t min_align) /* NVMe: 매개변수: 확장 크기, 최소 정렬 */
{
	struct resource *res = pci_resource_n(dev, resno); /* NVMe: 확장할 NVMe BAR 리소스 */
	const char *res_name = pci_resource_name(dev, resno); /* NVMe: 로그용 BAR 이름 */
	unsigned long flags; /* NVMe: 원래 리소스 플래그 백업 */
	resource_size_t new_size; /* NVMe: 확장된 BAR 크기 */
	int ret; /* NVMe: 재할당 결과 */

	if (res->flags & IORESOURCE_PCI_FIXED) /* NVMe: 고정 BAR은 확장 불가 */
		return 0; /* NVMe: 고정 BAR 유지 */

	flags = res->flags; /* NVMe: 실패 시 복원할 원래 플래그 저장 */
	res->flags |= IORESOURCE_UNSET; /* NVMe: 재할당 전 UNSET 표시 */
	if (!res->parent) { /* NVMe: 아직 부모에 등록되지 않은 BAR은 재할당 불가 */
		pci_info(dev, "%s %pR: can't reassign; unassigned resource\n", /* NVMe: 미할당 로그 */
			 res_name, res); /* NVMe: 로그 인자 전달 */
		return -EINVAL; /* NVMe: 재할당 실패 */
	}

	new_size = resource_size(res) + addsize; /* NVMe: 확장된 크기 계산 */
	ret = _pci_assign_resource(dev, resno, new_size, min_align); /* NVMe: 확장 크기로 재할당 시도 */
	if (ret) { /* NVMe: 실패 시 원래 플래그 복원 */
		res->flags = flags; /* NVMe: 원래 플래그 복원 */
		pci_info(dev, "%s %pR: failed to expand by %#llx\n", /* NVMe: 확장 실패 로그 */
			 res_name, res, (unsigned long long) addsize); /* NVMe: 로그 인자 전달 */
		return ret; /* NVMe: 재할당 실패 */
	}

	res->flags &= ~IORESOURCE_UNSET; /* NVMe: 재할당 성공, UNSET 클리어 */
	res->flags &= ~IORESOURCE_STARTALIGN; /* NVMe: 시작 정렬 플래그 클리어 */
	pci_info(dev, "%s %pR: reassigned; expanded by %#llx\n", /* NVMe: 재할당 성공 로그 */
		 res_name, res, (unsigned long long) addsize); /* NVMe: 로그 인자 전달 */
	if (resno < PCI_BRIDGE_RESOURCES) /* NVMe: endpoint BAR이면 config space 갱신 */
		pci_update_resource(dev, resno); /* NVMe: BAR 레지스터에 새 주소/크기 반영 */

	return 0; /* NVMe: pci_reassign_resource 성공 */
}

/*
 * pci_release_resource:
 *   NVMe BAR를 리소스 트리에서 해제하고 초기 상태로 되돌린다.
 *   장치 제거나 전원 관리 과정에서 호출될 수 있다.
 */
int pci_release_resource(struct pci_dev *dev, int resno)
{
	struct resource *res = pci_resource_n(dev, resno); /* NVMe: 해제할 NVMe BAR 리소스 */
	const char *res_name = pci_resource_name(dev, resno); /* NVMe: 로그용 BAR 이름 */
	int ret; /* NVMe: 해제 결과 */

	if (!res->parent) /* NVMe: 이미 등록되지 않은 BAR은 할 필요 없음 */
		return 0; /* NVMe: 이미 해제됨 */

	pci_info(dev, "%s %pR: releasing\n", res_name, res); /* NVMe: BAR 해제 로그 */

	ret = release_resource(res); /* NVMe: 상위 리소스 트리에서 제거 */
	if (ret) /* NVMe: 해제 실패 시 */
		return ret; /* NVMe: 실패 코드 반환 */
	res->end = resource_size(res) - 1; /* NVMe: end를 크기-1로 재설정 */
	res->start = 0; /* NVMe: 시작 주소 초기화 */
	res->flags |= IORESOURCE_UNSET; /* NVMe: 미할당 상태로 표시 */

	return 0; /* NVMe: pci_release_resource 성공 */
}
EXPORT_SYMBOL(pci_release_resource); /* NVMe: 외부에서 호출 가능 */

/*
 * pci_enable_resources:
 *   NVMe endpoint의 PCI_COMMAND 레지스터에 IO/MEMORY 비트를 설정하여
 *   BAR 접근과 DMA 마스터 동작을 가능하게 한다. MSI-X/IRQ/doorbell
 *   사용 전에 반드시 수행되어야 한다.
 */
int pci_enable_resources(struct pci_dev *dev, int mask)
{
	u16 cmd, old_cmd; /* NVMe: PCI_COMMAND 레지스터 값 */
	int i; /* NVMe: 리소스 인덱스 */
	struct resource *r; /* NVMe: 순회 중인 NVMe BAR */
	const char *r_name; /* NVMe: 로그용 리소스 이름 */

	pci_read_config_word(dev, PCI_COMMAND, &cmd); /* NVMe: 현재 PCI_COMMAND 읽기 */
	old_cmd = cmd; /* NVMe: 변경 전 값 백업 */

	pci_dev_for_each_resource(dev, r, i) { /* NVMe: NVMe 장치의 모든 BAR를 순회 */
		if (!(mask & (1 << i))) /* NVMe: mask에 포함되지 않은 리소스는 건 너뛴다 */
			continue; /* NVMe: 다음 리소스로 */

		r_name = pci_resource_name(dev, i); /* NVMe: 현재 리소스 이름 */

		if (!(r->flags & (IORESOURCE_IO | IORESOURCE_MEM))) /* NVMe: IO/MEM 리소스가 아니면 무시 */
			continue; /* NVMe: 다음 리소스로 */
		if (pci_resource_is_optional(dev, i)) /* NVMe: optional 리소스면 enable 생략 */
			continue; /* NVMe: 다음 리소스로 */

		if (i < PCI_BRIDGE_RESOURCES) { /* NVMe: endpoint BAR인 경우 할당/claim 확인 */
			if (r->flags & IORESOURCE_UNSET) { /* NVMe: 주소 미할당 BAR은 enable 불가 */
				pci_err(dev, "%s %pR: not assigned; can't enable device\n", /* NVMe: 미할당 오류 로그(doorbell 접근 불가 원인) */
					r_name, r); /* NVMe: 오류 메시지 인자 전달 */
				return -EINVAL; /* NVMe: 장치 활성화 실패 */
			}

			if (!r->parent) { /* NVMe: claim되지 않은 BAR은 enable 불가 */
				pci_err(dev, "%s %pR: not claimed; can't enable device\n", /* NVMe: 미claim 오류 로그 */
					r_name, r); /* NVMe: 오류 메시지 인자 전달 */
				return -EINVAL; /* NVMe: 장치 활성화 실패 */
			}
		}

		if (r->parent) { /* NVMe: 유효한 리소스면 command bit 설정 */
			if (r->flags & IORESOURCE_IO) /* NVMe: IO BAR이면 IO enable */
				cmd |= PCI_COMMAND_IO; /* NVMe: PCI_COMMAND_IO 비트 설정 */
			if (r->flags & IORESOURCE_MEM) /* NVMe: MEM BAR이면 memory enable */
				cmd |= PCI_COMMAND_MEMORY; /* NVMe: PCI_COMMAND_MEMORY 설정(DMA/MSI-X/도어벨 전제) */
		}
	}

	if (cmd != old_cmd) { /* NVMe: 변경 사항이 있을 때만 기록 */
		pci_info(dev, "enabling device (%04x -> %04x)\n", old_cmd, cmd); /* NVMe: command 변경 로그 */
		pci_write_config_word(dev, PCI_COMMAND, cmd); /* NVMe: NVMe endpoint의 PCI_COMMAND 갱신 */
	}
	return 0; /* NVMe: enable 성공 */
}
