// SPDX-License-Identifier: GPL-2.0
#include <linux/pci.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/wait.h>

#include "pci.h"

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/access.c)은 PCI/PCIe 설정 공간(configuration space)에
 * 대한 읽기/쓰기 래퍼와 PCI Express Capability 레지스터 접근 함수를 제공한다.
 * NVMe SSD는 PCIe endpoint로서 다음과 같은 동작에서 본 파일의 함수를 직접
 * 또는 간접적으로 사용한다.
 *   - nvme_probe -> pci_enable_device: Vendor/Device ID, CLASS, BAR,
 *     Command/Status 등을 읽어 장치를 식별하고 활성화 (pci_read_config_*)
 *   - BAR(doorbell) 매핑 전 BAR 주소/크기 획득 (config space base address
 *     레지스터 읽기)
 *   - MSI-X/MSI/INTx IRQ 설정: PCI/PCIe capability 탐색 및 MSI-X table
 *     설정 시 config space 접근 (pcie_capability_read/write_*,
 *     pci_write_config_*)
 *   - DMA 마스크, 펌웨어 제어, 전원 상태 전환을 위한 config space 쓰기
 *   - ASPM, 링크 제어, 전원 관리 관련 PCIe Capability(LNKCTL, DEVCTL,
 *     DEVSTA) 읽기/쓰기
 * 본 파일은 모든 PCIe endpoint의 config space 접근을 직렬화하는 pci_lock을
 * 관리하며, NVMe 장치의 안정적인 초기화, 런타임 제어, 전원 관리에 필수적이다.
 * ===================================================================
 */

/*
 * This interrupt-safe spinlock protects all accesses to PCI
 * configuration space.
 */

DEFINE_RAW_SPINLOCK(pci_lock); /* NVMe: NVMe 장치를 포함한 모든 PCI 함수의 config space 접근을 보호하는 락 */

/*
 * Wrappers for all PCI configuration access functions.  They just check
 * alignment, do locking and call the low-level functions pointed to
 * by pci_dev->ops.
 */

#define PCI_byte_BAD 0 /* NVMe: byte 단위 접근은 항상 정렬 조건을 만족함 */
#define PCI_word_BAD (pos & 1) /* NVMe: word 접근 시 하위 1비트가 0이어야 정렬됨 */
#define PCI_dword_BAD (pos & 3) /* NVMe: dword 접근 시 하위 2비트가 0이어야 정렬됨 */

#ifdef CONFIG_PCI_LOCKLESS_CONFIG
# define pci_lock_config(f)	do { (void)(f); } while (0) /* NVMe: lockless 설정 시 컴파일 타임에 락 획득/해제 제거 */
# define pci_unlock_config(f)	do { (void)(f); } while (0) /* NVMe: lockless 모드에서는 unlock도 no-op */
#else
# define pci_lock_config(f)	raw_spin_lock_irqsave(&pci_lock, f) /* NVMe: NVMe config 접근 직전 인터럽트를 끄고 pci_lock 획득 */
# define pci_unlock_config(f)	raw_spin_unlock_irqrestore(&pci_lock, f) /* NVMe: config 접근 후 락 해제 및 인터럽트 상태 복원 */
#endif

/*
 * PCI_OP_READ / PCI_OP_WRITE:
 *   크기별(byte/word/dword) PCI config space 읽기/쓰기 함수를 생성하는 매크로.
 *   NVMe 장치의 Vendor/Device ID, CLASS, BAR, Command/Status, MSI-X capability,
 *   PCIe capability 등을 읽거나 쓸 때 pci_read_config_*/pci_write_config_* 경로로
 *   이 함수들이 간접 호출된다. pci_lock으로 동시 접근을 직렬화한다.
 */
#define PCI_OP_READ(size, type, len) \
int noinline pci_bus_read_config_##size \
	(struct pci_bus *bus, unsigned int devfn, int pos, type *value)	\
{									\
	unsigned long flags; /* NVMe: 인터럽트 상태 저장용 플래그 */						\
	u32 data = 0; /* NVMe: config read로 받은 원시 32비트 데이터 */						\
	int res; /* NVMe: 하위 버스 read 결과 코드 */								\
												\
	if (PCI_##size##_BAD) /* NVMe: pos 정렬이 잘못되었는지 검사 */						\
		return PCIBIOS_BAD_REGISTER_NUMBER; /* NVMe: 정렬 오류 반환, NVMe BAR/캡 읽기 실패 방지 */		\
												\
	pci_lock_config(flags); /* NVMe: config space 보호용 락 획득 */						\
	res = bus->ops->read(bus, devfn, pos, len, &data); /* NVMe: 실제 RC/호스트 bridge의 config read 수행 */	\
	if (res) /* NVMe: read 실패 시(장치 분리 등) */								\
		PCI_SET_ERROR_RESPONSE(value); /* NVMe: 호출자 버퍼에 PCIe error response 표기 */			\
	else /* NVMe: 정상 읽기 성공 시 */										\
		*value = (type)data; /* NVMe: 요청한 크기로 데이터 축소 후 저장 */					\
	pci_unlock_config(flags); /* NVMe: 락 해제 */								\
												\
	return res; /* NVMe: 성공/실패 코드 반환 */								\
}

#define PCI_OP_WRITE(size, type, len) \
int noinline pci_bus_write_config_##size \
	(struct pci_bus *bus, unsigned int devfn, int pos, type value)	\
{										\
	unsigned long flags; /* NVMe: 인터럽트 상태 저장용 */							\
	int res; /* NVMe: write 결과 코드 */									\
												\
	if (PCI_##size##_BAD) /* NVMe: pos 정렬 검사 */							\
		return PCIBIOS_BAD_REGISTER_NUMBER; /* NVMe: 정렬 오류 반환 */					\
												\
	pci_lock_config(flags); /* NVMe: config space 락 획득 */							\
	res = bus->ops->write(bus, devfn, pos, len, value); /* NVMe: RC config write 수행 (예: NVMe command 레지스터 갱신) */	\
	pci_unlock_config(flags); /* NVMe: 락 해제 */								\
												\
	return res; /* NVMe: write 결과 반환 */								\
}

PCI_OP_READ(byte, u8, 1) /* NVMe: byte 단위 config read 함수 생성 (NVMe status/캡 byte) */
PCI_OP_READ(word, u16, 2) /* NVMe: word 단위 config read 함수 생성 (NVMe PCI_STATUS 등) */
PCI_OP_READ(dword, u32, 4) /* NVMe: dword 단위 config read 함수 생성 (NVMe BAR/캡 dword) */
PCI_OP_WRITE(byte, u8, 1) /* NVMe: byte 단위 config write 함수 생성 */
PCI_OP_WRITE(word, u16, 2) /* NVMe: word 단위 config write 함수 생성 */
PCI_OP_WRITE(dword, u32, 4) /* NVMe: dword 단위 config write 함수 생성 (NVMe BAR/DMA 설정) */

EXPORT_SYMBOL(pci_bus_read_config_byte); /* NVMe: 외부 모듈(NVMe 포함)에서 config byte read 가능 */
EXPORT_SYMBOL(pci_bus_read_config_word); /* NVMe: 외부 모듈에서 config word read 가능 */
EXPORT_SYMBOL(pci_bus_read_config_dword); /* NVMe: 외부 모듈에서 config dword read 가능 */
EXPORT_SYMBOL(pci_bus_write_config_byte); /* NVMe: 외부 모듈에서 config byte write 가능 */
EXPORT_SYMBOL(pci_bus_write_config_word); /* NVMe: 외부 모듈에서 config word write 가능 */
EXPORT_SYMBOL(pci_bus_write_config_dword); /* NVMe: 외부 모듈에서 config dword write 가능 */

/*
 * pci_generic_config_read:
 *   host bridge의 map_bus를 통해 메모리 맵된 config space에서 값을 읽는다.
 *   NVMe 장치의 config read가 RC 드라이버의 이 함수를 통해 수행될 수 있다.
 */
int pci_generic_config_read(struct pci_bus *bus, unsigned int devfn,
			    int where, int size, u32 *val)
{
	void __iomem *addr; /* NVMe: 메모리 맵된 config space 주소 */

	addr = bus->ops->map_bus(bus, devfn, where); /* NVMe: bus/devfn/offset에 해당하는 가상 주소 획득 */
	if (!addr) /* NVMe: 장치가 존재하지 않으면 */
		return PCIBIOS_DEVICE_NOT_FOUND; /* NVMe: 장치 미발견 오류 반환 */

	if (size == 1) /* NVMe: 1바이트 읽기 요청 */
		*val = readb(addr); /* NVMe: 메모리 맵 IO로 1바이트 읽기 */
	else if (size == 2) /* NVMe: 2바이트 읽기 요청 */
		*val = readw(addr); /* NVMe: 2바이트 읽기 */
	else /* NVMe: 4바이트 이상(dword) 요청 */
		*val = readl(addr); /* NVMe: 4바이트 읽기 */

	return PCIBIOS_SUCCESSFUL; /* NVMe: 성공 반환 */
}
EXPORT_SYMBOL_GPL(pci_generic_config_read); /* NVMe: GPL 모듈용 generic config read export */

/*
 * pci_generic_config_write:
 *   메모리 맵된 config space에 값을 쓴다.
 *   NVMe의 Command register, BAR, MSI-X table 등 쓰기 시 RC 드라이버가 사용.
 */
int pci_generic_config_write(struct pci_bus *bus, unsigned int devfn,
			     int where, int size, u32 val)
{
	void __iomem *addr; /* NVMe: 메모리 맵된 config space 주소 */

	addr = bus->ops->map_bus(bus, devfn, where); /* NVMe: bus/devfn/offset에 해당하는 가상 주소 획득 */
	if (!addr) /* NVMe: 장치가 없으면 */
		return PCIBIOS_DEVICE_NOT_FOUND; /* NVMe: 장치 미발견 오류 반환 */

	if (size == 1) /* NVMe: 1바이트 쓰기 */
		writeb(val, addr); /* NVMe: 1바이트 메모리 맵 IO 쓰기 */
	else if (size == 2) /* NVMe: 2바이트 쓰기 */
		writew(val, addr); /* NVMe: 2바이트 쓰기 */
	else /* NVMe: 4바이트 쓰기 */
		writel(val, addr); /* NVMe: 4바이트 쓰기 */

	return PCIBIOS_SUCCESSFUL; /* NVMe: 성공 반환 */
}
EXPORT_SYMBOL_GPL(pci_generic_config_write); /* NVMe: GPL 모듈용 generic config write export */

/*
 * pci_generic_config_read32:
 *   32비트 접근만 가능한 하드웨어에서 dword 정렬 주소를 읽고 필요한 바이트만
 *   추출한다. NVMe config space가 ECAM이 아닌 32비트 전용 경로로 연결된
 *   플랫폼에서 사용될 수 있다.
 */
int pci_generic_config_read32(struct pci_bus *bus, unsigned int devfn,
			      int where, int size, u32 *val)
{
	void __iomem *addr; /* NVMe: 메모리 맵된 config space 주소 */

	addr = bus->ops->map_bus(bus, devfn, where & ~0x3); /* NVMe: dword 정렬된 주소로 매핑 */
	if (!addr) /* NVMe: 장치가 없으면 */
		return PCIBIOS_DEVICE_NOT_FOUND; /* NVMe: 장치 미발견 오류 반환 */

	*val = readl(addr); /* NVMe: 4바이트 전체 읽기 */

	if (size <= 2) /* NVMe: byte/word 요청이면 */
		*val = (*val >> (8 * (where & 3))) & ((1 << (size * 8)) - 1); /* NVMe: 요청한 바이트만 시프트 후 마스크 */

	return PCIBIOS_SUCCESSFUL; /* NVMe: 성공 반환 */
}
EXPORT_SYMBOL_GPL(pci_generic_config_read32); /* NVMe: 32비트 전용 read 경로 export */

/*
 * pci_generic_config_write32:
 *   32비트 접근만 가능한 하드웨어에서 read-modify-write로 부분 쓰기를 수행.
 *   NVMe capability/BAR 등 byte/word 단위 쓰기가 필요할 때 주의해서 사용.
 */
int pci_generic_config_write32(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 val)
{
	void __iomem *addr; /* NVMe: 메모리 맵된 config space 주소 */
	u32 mask, tmp; /* NVMe: 비트 마스크와 임시 저장 변수 */

	addr = bus->ops->map_bus(bus, devfn, where & ~0x3); /* NVMe: dword 정렬 주소 매핑 */
	if (!addr) /* NVMe: 장치가 없으면 */
		return PCIBIOS_DEVICE_NOT_FOUND; /* NVMe: 장치 미발견 오류 반환 */

	if (size == 4) { /* NVMe: 전체 dword 쓰기면 */
		writel(val, addr); /* NVMe: 4바이트 한 번에 쓰기 */
		return PCIBIOS_SUCCESSFUL; /* NVMe: 성공 반환 */
	}

	/*
	 * In general, hardware that supports only 32-bit writes on PCI is
	 * not spec-compliant.  For example, software may perform a 16-bit
	 * write.  If the hardware only supports 32-bit accesses, we must
	 * do a 32-bit read, merge in the 16 bits we intend to write,
	 * followed by a 32-bit write.  If the 16 bits we *don't* intend to
	 * write happen to have any RW1C (write-one-to-clear) bits set, we
	 * just inadvertently cleared something we shouldn't have.
	 */
	if (!bus->unsafe_warn) { /* NVMe: 아직 경고를 출력하지 않은 버스라면 */
		dev_warn(&bus->dev, "%d-byte config write to %04x:%02x:%02x.%d offset %#x may corrupt adjacent RW1C bits\n",
			 size, pci_domain_nr(bus), bus->number,
			 PCI_SLOT(devfn), PCI_FUNC(devfn), where); /* NVMe: 부분 쓰기로 인접 RW1C 비트 훼손 가능성 경고 */
		bus->unsafe_warn = 1; /* NVMe: 중복 경고 방지용 플래그 설정 */
	}

	mask = ~(((1 << (size * 8)) - 1) << ((where & 0x3) * 8)); /* NVMe: 덮어쓰지 않을 상위/하위 비트 보존용 마스크 */
	tmp = readl(addr) & mask; /* NVMe: 기존 dword 값에서 수정할 비트만 남기고 보존 */
	tmp |= val << ((where & 0x3) * 8); /* NVMe: 새 값을 해당 바이트 위치에 합성 */
	writel(tmp, addr); /* NVMe: 합성된 dword를 다시 쓰기 */

	return PCIBIOS_SUCCESSFUL; /* NVMe: 성공 반환 */
}
EXPORT_SYMBOL_GPL(pci_generic_config_write32); /* NVMe: 32비트 전용 write 경로 export */

/**
 * pci_bus_set_ops - Set raw operations of pci bus
 * @bus:	pci bus struct
 * @ops:	new raw operations
 *
 * Return previous raw operations
 */
struct pci_ops *pci_bus_set_ops(struct pci_bus *bus, struct pci_ops *ops)
{
	struct pci_ops *old_ops; /* NVMe: 이전 bus ops 포인터 보관 */
	unsigned long flags; /* NVMe: 인터럽트 상태 저장 */

	raw_spin_lock_irqsave(&pci_lock, flags); /* NVMe: bus ops 교체 시 config 접근 직렬화 */
	old_ops = bus->ops; /* NVMe: 기존 ops 백업 (NVMe bus도 동일) */
	bus->ops = ops; /* NVMe: 새로운 config access ops 설정 */
	raw_spin_unlock_irqrestore(&pci_lock, flags); /* NVMe: 락 해제 및 인터럽트 복원 */
	return old_ops; /* NVMe: 이전 ops 반환 (복구/비교용) */
}
EXPORT_SYMBOL(pci_bus_set_ops); /* NVMe: bus ops 교체 함수 export */

/*
 * The following routines are to prevent the user from accessing PCI config
 * space when it's unsafe to do so.  Some devices require this during BIST and
 * we're required to prevent it during D-state transitions.
 *
 * We have a bit per device to indicate it's blocked and a global wait queue
 * for callers to sleep on until devices are unblocked.
 */
static DECLARE_WAIT_QUEUE_HEAD(pci_cfg_wait); /* NVMe: config 접근 차단 시 대기할 글로벌 wait queue */

/*
 * pci_wait_cfg:
 *   config 접근이 차단된 NVMe 장치에 대해 차단이 풀릴 때까지 슬립한다.
 *   D-state 전환, BIST, hotplug 제거 등에서 사용.
 */
static noinline void pci_wait_cfg(struct pci_dev *dev)
	__must_hold(&pci_lock)
{
	do {
		raw_spin_unlock_irq(&pci_lock); /* NVMe: 락을 풀고 잠들 수 있게 함 */
		wait_event(pci_cfg_wait, !dev->block_cfg_access); /* NVMe: block_cfg_access가 0이 될 때까지 대기 */
		raw_spin_lock_irq(&pci_lock); /* NVMe: 깨어난 후 다시 pci_lock 획득 */
	} while (dev->block_cfg_access); /* NVMe: 깨어난 후에도 여전히 차단되었으면 다시 대기 */
}

/* Returns 0 on success, negative values indicate error. */
#define PCI_USER_READ_CONFIG(size, type)				\
int pci_user_read_config_##size						\
	(struct pci_dev *dev, int pos, type *val)			\
{										\
	u32 data = -1; /* NVMe: userspace config read용 32비트 임시 버퍼 */						\
	int ret; /* NVMe: userspace read 반환 값 */								\
												\
	if (PCI_##size##_BAD) /* NVMe: pos 정렬 검사 */							\
		return -EINVAL; /* NVMe: 잘못된 레지스터 번호 반환 */						\
												\
	raw_spin_lock_irq(&pci_lock); /* NVMe: config 접근 보호 락 획득 */					\
	if (unlikely(dev->block_cfg_access)) /* NVMe: NVMe 장치가 D-state 등으로 차단되었는지 확인 */			\
		pci_wait_cfg(dev); /* NVMe: 차단 해제까지 대기 */						\
	ret = dev->bus->ops->read(dev->bus, dev->devfn,						\
				  pos, sizeof(type), &data); /* NVMe: 실제 config read (NVMe devfn 사용) */			\
	raw_spin_unlock_irq(&pci_lock); /* NVMe: 락 해제 */						\
	if (ret) /* NVMe: read 실패 시 */									\
		PCI_SET_ERROR_RESPONSE(val); /* NVMe: error response 설정 */					\
	else /* NVMe: 성공 시 */										\
		*val = (type)data; /* NVMe: 요청 크기로 축소하여 userspace에 전달 */					\
												\
	return pcibios_err_to_errno(ret); /* NVMe: errno로 변환하여 반환 */					\
}										\
EXPORT_SYMBOL_GPL(pci_user_read_config_##size);

/* Returns 0 on success, negative values indicate error. */
#define PCI_USER_WRITE_CONFIG(size, type)				\
int pci_user_write_config_##size						\
	(struct pci_dev *dev, int pos, type val)			\
{										\
	int ret; /* NVMe: userspace write 반환 값 */								\
												\
	if (PCI_##size##_BAD) /* NVMe: pos 정렬 검사 */							\
		return -EINVAL; /* NVMe: 잘못된 레지스터 번호 반환 */						\
												\
	raw_spin_lock_irq(&pci_lock); /* NVMe: config 접근 보호 락 획득 */					\
	if (unlikely(dev->block_cfg_access)) /* NVMe: NVMe 장치가 차단 중인지 확인 */					\
		pci_wait_cfg(dev); /* NVMe: 차단 해제까지 대기 */						\
	ret = dev->bus->ops->write(dev->bus, dev->devfn,						\
				   pos, sizeof(type), val); /* NVMe: 실제 config write (NVMe 레지스터 제어) */			\
	raw_spin_unlock_irq(&pci_lock); /* NVMe: 락 해제 */						\
												\
	return pcibios_err_to_errno(ret); /* NVMe: errno로 변환하여 반환 */					\
}										\
EXPORT_SYMBOL_GPL(pci_user_write_config_##size);

PCI_USER_READ_CONFIG(byte, u8) /* NVMe: userspace용 byte read 함수 생성 */
PCI_USER_READ_CONFIG(word, u16) /* NVMe: userspace용 word read 함수 생성 */
PCI_USER_READ_CONFIG(dword, u32) /* NVMe: userspace용 dword read 함수 생성 */
PCI_USER_WRITE_CONFIG(byte, u8) /* NVMe: userspace용 byte write 함수 생성 */
PCI_USER_WRITE_CONFIG(word, u16) /* NVMe: userspace용 word write 함수 생성 */
PCI_USER_WRITE_CONFIG(dword, u32) /* NVMe: userspace용 dword write 함수 생성 */

/**
 * pci_cfg_access_lock - Lock PCI config reads/writes
 * @dev:	pci device struct
 *
 * When access is locked, any userspace reads or writes to config
 * space and concurrent lock requests will sleep until access is
 * allowed via pci_cfg_access_unlock() again.
 */
void pci_cfg_access_lock(struct pci_dev *dev)
{
	might_sleep(); /* NVMe: 이 함수에서 수면 가능함을 커널에 알림 */

	raw_spin_lock_irq(&pci_lock); /* NVMe: config 접근 보호 락 획득 */
	if (dev->block_cfg_access) /* NVMe: 이미 다른 경로에서 차단 중이면 */
		pci_wait_cfg(dev); /* NVMe: 해제될 때까지 대기 */
	dev->block_cfg_access = 1; /* NVMe: 현재 컨텍스트가 NVMe config 접근을 차단함 */
	raw_spin_unlock_irq(&pci_lock); /* NVMe: 락 해제 */
}
EXPORT_SYMBOL_GPL(pci_cfg_access_lock); /* NVMe: config 접근 lock 함수 export */

/**
 * pci_cfg_access_trylock - try to lock PCI config reads/writes
 * @dev:	pci device struct
 *
 * Same as pci_cfg_access_lock, but will return 0 if access is
 * already locked, 1 otherwise. This function can be used from
 * atomic contexts.
 */
bool pci_cfg_access_trylock(struct pci_dev *dev)
{
	unsigned long flags; /* NVMe: 인터럽트 상태 저장 */
	bool locked = true; /* NVMe: 락 획득 성공 가정 */

	raw_spin_lock_irqsave(&pci_lock, flags); /* NVMe: 원자 컨텍스트에서 안전하게 락 획득 */
	if (dev->block_cfg_access) /* NVMe: 이미 차단 중이면 */
		locked = false; /* NVMe: 락 획득 실패 */
	else
		dev->block_cfg_access = 1; /* NVMe: 차단 플래그 설정 */
	raw_spin_unlock_irqrestore(&pci_lock, flags); /* NVMe: 락 해제 및 인터럽트 복원 */

	return locked; /* NVMe: 획득 여부 반환 */
}
EXPORT_SYMBOL_GPL(pci_cfg_access_trylock); /* NVMe: 원자 컨텍스트용 trylock export */

/**
 * pci_cfg_access_unlock - Unlock PCI config reads/writes
 * @dev:	pci device struct
 *
 * This function allows PCI config accesses to resume.
 */
void pci_cfg_access_unlock(struct pci_dev *dev)
{
	unsigned long flags; /* NVMe: 인터럽트 상태 저장 */

	raw_spin_lock_irqsave(&pci_lock, flags); /* NVMe: 차단 플래그 해제 전 락 획득 */

	/*
	 * This indicates a problem in the caller, but we don't need
	 * to kill them, unlike a double-block above.
	 */
	WARN_ON(!dev->block_cfg_access); /* NVMe: lock 없이 unlock 호출 시 경고 */

	dev->block_cfg_access = 0; /* NVMe: NVMe 장치 config 접근 차단 해제 */
	raw_spin_unlock_irqrestore(&pci_lock, flags); /* NVMe: 락 해제 및 인터럽트 복원 */

	wake_up_all(&pci_cfg_wait); /* NVMe: config 접근 대기 중인 태스크 모두 깨움 */
}
EXPORT_SYMBOL_GPL(pci_cfg_access_unlock); /* NVMe: config 접근 unlock export */

/*
 * pcie_cap_version:
 *   PCIe capability 버전을 읽는다.
 *   NVMe 장치의 PCIe capability 구조체 버전 판별에 사용.
 */
static inline int pcie_cap_version(const struct pci_dev *dev)
{
	return pcie_caps_reg(dev) & PCI_EXP_FLAGS_VERS; /* NVMe: PCIe Capabilities Register의 버전 필드 반환 */
}

/*
 * pcie_cap_has_lnkctl:
 *   현재 PCIe function이 Link Control 레지스터를 갖는지 판단.
 *   NVMe endpoint는 이 레지스터를 통해 ASPM/active state link 제어를 한다.
 */
bool pcie_cap_has_lnkctl(const struct pci_dev *dev)
{
	int type = pci_pcie_type(dev); /* NVMe: 현재 PCIe function 타입(endpoint/root port 등) 획득 */

	return type == PCI_EXP_TYPE_ENDPOINT ||
	       type == PCI_EXP_TYPE_LEG_END ||
	       type == PCI_EXP_TYPE_ROOT_PORT ||
	       type == PCI_EXP_TYPE_UPSTREAM ||
	       type == PCI_EXP_TYPE_DOWNSTREAM ||
	       type == PCI_EXP_TYPE_PCI_BRIDGE ||
	       type == PCI_EXP_TYPE_PCIE_BRIDGE; /* NVMe: NVMe endpoint에 해당하면 LNKCTL 존재 */
}

/*
 * pcie_cap_has_lnkctl2:
 *   PCIe capability 버전 2 이상인 경우 LNKCTL2 레지스터 존재 여부.
 *   NVMe 링크 속도/폭 협상 시 참조.
 */
bool pcie_cap_has_lnkctl2(const struct pci_dev *dev)
{
	return pcie_cap_has_lnkctl(dev) && pcie_cap_version(dev) > 1; /* NVMe: LNKCTL 있고 버전이 2 이상이면 true */
}

/*
 * pcie_cap_has_sltctl:
 *   Downstream port의 slot control 레지스터 존재 여부.
 *   NVMe가 연결된 루트/다운스트림 포트의 hotplug/slot 제어에 관련.
 */
static inline bool pcie_cap_has_sltctl(const struct pci_dev *dev)
{
	return pcie_downstream_port(dev) &&
	       pcie_caps_reg(dev) & PCI_EXP_FLAGS_SLOT; /* NVMe: downstream port이고 slot 플래그가 있으면 true */
}

/*
 * pcie_cap_has_rtctl:
 *   Root Port의 Root Control 레지스터 존재 여부.
 *   NVMe 장치의 AER/PME 등 루트 포트 제어와 관련.
 */
bool pcie_cap_has_rtctl(const struct pci_dev *dev)
{
	int type = pci_pcie_type(dev); /* NVMe: PCIe function 타입 획득 */

	return type == PCI_EXP_TYPE_ROOT_PORT ||
	       type == PCI_EXP_TYPE_RC_EC; /* NVMe: Root Port 또는 RC Event Collector면 RTCTL 존재 */
}

/*
 * pcie_capability_reg_implemented:
 *   주어진 PCIe capability 오프셋이 현재 function에 구현되었는지 확인.
 *   NVMe가 ASPM(LNKCTL), ERR_CORR 등을 읽기 전 먼저 이 함수로 유효성을 검사.
 */
static bool pcie_capability_reg_implemented(struct pci_dev *dev, int pos)
{
	if (!pci_is_pcie(dev)) /* NVMe: 장치가 PCIe 기능을 가지고 있는지 확인 */
		return false; /* NVMe: PCIe가 아니면 capability 레지스터 없음 */

	switch (pos) { /* NVMe: 요청한 capability 오프셋에 따라 분기 */
	case PCI_EXP_FLAGS: /* NVMe: PCIe Capabilities Register는 항상 존재 */
		return true;
	case PCI_EXP_DEVCAP: /* NVMe: Device Capability */
	case PCI_EXP_DEVCTL: /* NVMe: Device Control (NVMe의 MPS, unsupported request 등 제어) */
	case PCI_EXP_DEVSTA: /* NVMe: Device Status */
		return true;
	case PCI_EXP_LNKCAP: /* NVMe: Link Capability (NVMe 링크 속도/폭 정보) */
	case PCI_EXP_LNKCTL: /* NVMe: Link Control (ASPM 활성화/비활성화 등) */
	case PCI_EXP_LNKSTA: /* NVMe: Link Status */
		return pcie_cap_has_lnkctl(dev); /* NVMe: NVMe endpoint는 보통 LNKCTL을 가짐 */
	case PCI_EXP_SLTCAP: /* NVMe: Slot Capability */
	case PCI_EXP_SLTCTL: /* NVMe: Slot Control */
	case PCI_EXP_SLTSTA: /* NVMe: Slot Status */
		return pcie_cap_has_sltctl(dev); /* NVMe: downstream/slot 포트에서만 존재 */
	case PCI_EXP_RTCTL: /* NVMe: Root Control */
	case PCI_EXP_RTCAP: /* NVMe: Root Capability */
	case PCI_EXP_RTSTA: /* NVMe: Root Status */
		return pcie_cap_has_rtctl(dev); /* NVMe: Root Port에서만 존재 */
	case PCI_EXP_DEVCAP2: /* NVMe: Device Capability 2 */
	case PCI_EXP_DEVCTL2: /* NVMe: Device Control 2 (ARI 등) */
		return pcie_cap_version(dev) > 1; /* NVMe: PCIe capability 버전 2 이상 */
	case PCI_EXP_LNKCAP2: /* NVMe: Link Capability 2 */
	case PCI_EXP_LNKCTL2: /* NVMe: Link Control 2 */
	case PCI_EXP_LNKSTA2: /* NVMe: Link Status 2 */
		return pcie_cap_has_lnkctl2(dev); /* NVMe: LNKCTL2가 있는 경우에만 존재 */
	default: /* NVMe: 알 수 없는 오프셋 */
		return false; /* NVMe: 구현되지 않은 것으로 처리 */
	}
}

/*
 * Note that these accessor functions are only for the "PCI Express
 * Capability" (see PCIe spec r3.0, sec 7.8).  They do not apply to the
 * other "PCI Express Extended Capabilities" (AER, VC, ACS, MFVC, etc.)
 */

/*
 * pcie_capability_read_word:
 *   PCI Express Capability 영역에서 word를 읽는다.
 *   NVMe는 DEVCTL/LNKCTL 등 PCIe capability를 이 경로로 읽어 ASPM, MPS,
 *   link 상태를 확인한다.
 */
int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val)
{
	int ret; /* NVMe: read 반환 값 */

	*val = 0; /* NVMe: 기본값 0으로 초기화 */
	if (pos & 1) /* NVMe: word 정렬 검사 */
		return PCIBIOS_BAD_REGISTER_NUMBER; /* NVMe: 정렬 오류 반환 */

	if (pcie_capability_reg_implemented(dev, pos)) { /* NVMe: 해당 레지스터가 NVMe 장치에 구현되었는지 확인 */
		ret = pci_read_config_word(dev, pci_pcie_cap(dev) + pos, val); /* NVMe: PCIe capability base + offset에서 word 읽기 */
		/*
		 * Reset *val to 0 if pci_read_config_word() fails; it may
		 * have been written as 0xFFFF (PCI_ERROR_RESPONSE) if the
		 * config read failed on PCI.
		 */
		if (ret) /* NVMe: read 실패 시 */
			*val = 0; /* NVMe: error response 대신 0으로 되돌림 */
		return ret; /* NVMe: 성공/실패 반환 */
	}

	/*
	 * For Functions that do not implement the Slot Capabilities,
	 * Slot Status, and Slot Control registers, these spaces must
	 * be hardwired to 0b, with the exception of the Presence Detect
	 * State bit in the Slot Status register of Downstream Ports,
	 * which must be hardwired to 1b.  (PCIe Base Spec 3.0, sec 7.8)
	 */
	if (pci_is_pcie(dev) && pcie_downstream_port(dev) &&
	    pos == PCI_EXP_SLTSTA) /* NVMe: downstream 포트의 Slot Status 미구현 예외 처리 */
		*val = PCI_EXP_SLTSTA_PDS; /* NVMe: Presence Detect State 비트를 1로 강제 */

	return 0; /* NVMe: 정상적으로 0 반환(미구현 레지스터는 0으로 간주) */
}
EXPORT_SYMBOL(pcie_capability_read_word); /* NVMe: PCIe capability word read export */

/*
 * pcie_capability_read_dword:
 *   PCI Express Capability 영역에서 dword를 읽는다.
 *   NVMe는 DEVCAP/LNKCAP 등 32비트 capability 정보를 이 함수로 읽는다.
 */
int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val)
{
	int ret; /* NVMe: read 반환 값 */

	*val = 0; /* NVMe: 기본값 0으로 초기화 */
	if (pos & 3) /* NVMe: dword 정렬 검사 */
		return PCIBIOS_BAD_REGISTER_NUMBER; /* NVMe: 정렬 오류 반환 */

	if (pcie_capability_reg_implemented(dev, pos)) { /* NVMe: 레지스터 구현 여부 확인 */
		ret = pci_read_config_dword(dev, pci_pcie_cap(dev) + pos, val); /* NVMe: PCIe capability base + offset에서 dword 읽기 */
		/*
		 * Reset *val to 0 if pci_read_config_dword() fails; it may
		 * have been written as 0xFFFFFFFF (PCI_ERROR_RESPONSE) if
		 * the config read failed on PCI.
		 */
		if (ret) /* NVMe: read 실패 시 */
			*val = 0; /* NVMe: error response 대신 0으로 초기화 */
		return ret; /* NVMe: 성공/실패 반환 */
	}

	if (pci_is_pcie(dev) && pcie_downstream_port(dev) &&
	    pos == PCI_EXP_SLTSTA) /* NVMe: downstream 포트 Slot Status 예외 */
		*val = PCI_EXP_SLTSTA_PDS; /* NVMe: Presence Detect State 비트 강제 */

	return 0; /* NVMe: 정상적으로 0 반환 */
}
EXPORT_SYMBOL(pcie_capability_read_dword); /* NVMe: PCIe capability dword read export */

/*
 * pcie_capability_write_word:
 *   PCI Express Capability 영역에 word를 쓴다.
 *   NVMe는 LNKCTL의 ASPM 비트 등을 변경할 때 이 함수를 사용한다.
 */
int pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val)
{
	if (pos & 1) /* NVMe: word 정렬 검사 */
		return PCIBIOS_BAD_REGISTER_NUMBER; /* NVMe: 정렬 오류 반환 */

	if (!pcie_capability_reg_implemented(dev, pos)) /* NVMe: 레지스터가 구현되지 않았으면 */
		return 0; /* NVMe: 아무 것도 하지 않고 성공 처리 */

	return pci_write_config_word(dev, pci_pcie_cap(dev) + pos, val); /* NVMe: PCIe capability 영역에 word 쓰기 */
}
EXPORT_SYMBOL(pcie_capability_write_word); /* NVMe: PCIe capability word write export */

/*
 * pcie_capability_write_dword:
 *   PCI Express Capability 영역에 dword를 쓴다.
 *   NVMe PCIe capability의 32비트 레지스터 갱신에 사용.
 */
int pcie_capability_write_dword(struct pci_dev *dev, int pos, u32 val)
{
	if (pos & 3) /* NVMe: dword 정렬 검사 */
		return PCIBIOS_BAD_REGISTER_NUMBER; /* NVMe: 정렬 오류 반환 */

	if (!pcie_capability_reg_implemented(dev, pos)) /* NVMe: 레지스터 미구현 시 */
		return 0; /* NVMe: 쓰기 skip */

	return pci_write_config_dword(dev, pci_pcie_cap(dev) + pos, val); /* NVMe: PCIe capability 영역에 dword 쓰기 */
}
EXPORT_SYMBOL(pcie_capability_write_dword); /* NVMe: PCIe capability dword write export */

/*
 * pcie_capability_clear_and_set_word_unlocked:
 *   PCIe capability word 레지스터에서 특정 비트를 클리어/설정(read-modify-write).
 *   NVMe가 DEVCTL/LNKCTL 등의 플래그를 원자적으로 변경할 때 사용.
 */
int pcie_capability_clear_and_set_word_unlocked(struct pci_dev *dev, int pos,
							u16 clear, u16 set)
{
	int ret; /* NVMe: read/write 반환 값 */
	u16 val; /* NVMe: 현재 capability 값 */

	ret = pcie_capability_read_word(dev, pos, &val); /* NVMe: 현재 레지스터 값 읽기 */
	if (ret) /* NVMe: 읽기 실패 시 */
		return ret; /* NVMe: 오류 반환 */

	val &= ~clear; /* NVMe: clear 비트들을 0으로 만듦 */
	val |= set; /* NVMe: set 비트들을 1로 설정 */
	return pcie_capability_write_word(dev, pos, val); /* NVMe: 변경된 값을 다시 씀 */
}
EXPORT_SYMBOL(pcie_capability_clear_and_set_word_unlocked); /* NVMe: unlocked RMW export */

/*
 * pcie_capability_clear_and_set_word_locked:
 *   dev->pcie_cap_lock으로 보호하며 PCIe capability word를 RMW.
 *   NVMe가 다중 컨텍스트에서 LNKCTL/DEVCTL을 동시에 갱신할 때 충돌 방지.
 */
int pcie_capability_clear_and_set_word_locked(struct pci_dev *dev, int pos,
						      u16 clear, u16 set)
{
	unsigned long flags; /* NVMe: 인터럽트 상태 저장 */
	int ret; /* NVMe: 반환 값 */

	spin_lock_irqsave(&dev->pcie_cap_lock, flags); /* NVMe: PCIe capability 전용 락 획득 */
	ret = pcie_capability_clear_and_set_word_unlocked(dev, pos, clear, set); /* NVMe: RMW 수행 */
	spin_unlock_irqrestore(&dev->pcie_cap_lock, flags); /* NVMe: 락 해제 */

	return ret; /* NVMe: RMW 결과 반환 */
}
EXPORT_SYMBOL(pcie_capability_clear_and_set_word_locked); /* NVMe: locked RMW export */

/*
 * pcie_capability_clear_and_set_dword:
 *   PCIe capability dword 레지스터 RMW.
 *   NVMe가 32비트 capability(DEVCAP2, LNKCAP2 등)를 변경할 때 사용.
 */
int pcie_capability_clear_and_set_dword(struct pci_dev *dev, int pos,
						u32 clear, u32 set)
{
	int ret; /* NVMe: 반환 값 */
	u32 val; /* NVMe: 현재 32비트 capability 값 */

	ret = pcie_capability_read_dword(dev, pos, &val); /* NVMe: 현재 dword 값 읽기 */
	if (ret) /* NVMe: 읽기 실패 시 */
		return ret; /* NVMe: 오류 반환 */

	val &= ~clear; /* NVMe: clear 비트 제거 */
	val |= set; /* NVMe: set 비트 추가 */
	return pcie_capability_write_dword(dev, pos, val); /* NVMe: 수정된 dword 쓰기 */
}
EXPORT_SYMBOL(pcie_capability_clear_and_set_dword); /* NVMe: dword RMW export */

/*
 * pci_read_config_byte/word/dword:
 *   NVMe 장치를 포함한 PCI 함수의 config space 읽기 래퍼.
 *   장치가 분리되었는지 먼저 확인하고 bus-level 함수를 호출한다.
 */
int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val)
{
	if (pci_dev_is_disconnected(dev)) { /* NVMe: NVMe 장치가 hotplug 등으로 분리되었는지 확인 */
		PCI_SET_ERROR_RESPONSE(val); /* NVMe: 분리 시 error response 설정 */
		return PCIBIOS_DEVICE_NOT_FOUND; /* NVMe: 장치 미발견 반환 */
	}
	return pci_bus_read_config_byte(dev->bus, dev->devfn, where, val); /* NVMe: bus 함수로 byte 읽기 요청 */
}
EXPORT_SYMBOL(pci_read_config_byte); /* NVMe: byte read 래퍼 export */

int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val)
{
	if (pci_dev_is_disconnected(dev)) { /* NVMe: NVMe 장치 분리 여부 확인 */
		PCI_SET_ERROR_RESPONSE(val); /* NVMe: 분리 시 error response */
		return PCIBIOS_DEVICE_NOT_FOUND; /* NVMe: 장치 미발견 */
	}
	return pci_bus_read_config_word(dev->bus, dev->devfn, where, val); /* NVMe: bus 함수로 word 읽기 (NVMe PCI_STATUS 등) */
}
EXPORT_SYMBOL(pci_read_config_word); /* NVMe: word read 래퍼 export */

int pci_read_config_dword(const struct pci_dev *dev, int where,
					u32 *val)
{
	if (pci_dev_is_disconnected(dev)) { /* NVMe: NVMe 장치 분리 여부 확인 */
		PCI_SET_ERROR_RESPONSE(val); /* NVMe: 분리 시 error response */
		return PCIBIOS_DEVICE_NOT_FOUND; /* NVMe: 장치 미발견 */
	}
	return pci_bus_read_config_dword(dev->bus, dev->devfn, where, val); /* NVMe: bus 함수로 dword 읽기 (NVMe BAR 등) */
}
EXPORT_SYMBOL(pci_read_config_dword); /* NVMe: dword read 래퍼 export */

/*
 * pci_write_config_byte/word/dword:
 *   NVMe 장치의 config space 쓰기 래퍼.
 *   command register, BAR, MSI-X capability 등을 변경할 때 사용.
 */
int pci_write_config_byte(const struct pci_dev *dev, int where, u8 val)
{
	if (pci_dev_is_disconnected(dev)) /* NVMe: NVMe 장치 분리 여부 확인 */
		return PCIBIOS_DEVICE_NOT_FOUND; /* NVMe: 장치 미발견 */
	return pci_bus_write_config_byte(dev->bus, dev->devfn, where, val); /* NVMe: bus 함수로 byte 쓰기 */
}
EXPORT_SYMBOL(pci_write_config_byte); /* NVMe: byte write 래퍼 export */

int pci_write_config_word(const struct pci_dev *dev, int where, u16 val)
{
	if (pci_dev_is_disconnected(dev)) /* NVMe: NVMe 장치 분리 여부 확인 */
		return PCIBIOS_DEVICE_NOT_FOUND; /* NVMe: 장치 미발견 */
	return pci_bus_write_config_word(dev->bus, dev->devfn, where, val); /* NVMe: bus 함수로 word 쓰기 */
}
EXPORT_SYMBOL(pci_write_config_word); /* NVMe: word write 래퍼 export */

int pci_write_config_dword(const struct pci_dev *dev, int where,
						 u32 val)
{
	if (pci_dev_is_disconnected(dev)) /* NVMe: NVMe 장치 분리 여부 확인 */
		return PCIBIOS_DEVICE_NOT_FOUND; /* NVMe: 장치 미발견 */
	return pci_bus_write_config_dword(dev->bus, dev->devfn, where, val); /* NVMe: bus 함수로 dword 쓰기 (NVMe DMA/PM) */
}
EXPORT_SYMBOL(pci_write_config_dword); /* NVMe: dword write 래퍼 export */

/*
 * pci_clear_and_set_config_dword:
 *   NVMe 장치의 config dword 레지스터에서 read-modify-write로 비트를 변경.
 *   command register의 bus master/MEM/IO 비트 등을 안전하게 토글할 때 사용.
 */
void pci_clear_and_set_config_dword(const struct pci_dev *dev, int pos,
				    u32 clear, u32 set)
{
	u32 val; /* NVMe: 현재 config dword 값 */

	pci_read_config_dword(dev, pos, &val); /* NVMe: 현재 값 읽기 */
	val &= ~clear; /* NVMe: clear 할 비트 제거 */
	val |= set; /* NVMe: set 할 비트 추가 */
	pci_write_config_dword(dev, pos, val); /* NVMe: 변경된 dword 값 쓰기 */
}
EXPORT_SYMBOL(pci_clear_and_set_config_dword); /* NVMe: config dword RMW export */
