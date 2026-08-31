// SPDX-License-Identifier: GPL-2.0
/*
 * [한국어 설명] 드라이버가 잡은 PCI 자원을 자동으로 되돌려 주는 계층 (devres.c)
 *
 * === 파일의 역할 ===
 * devres(device resource management)는 "드라이버가 죽거나 언바인딩될 때
 * 잡아 둔 것을 커널이 알아서 풀어 주는" 커널 공통 기능이다. 이 파일은
 * 그것의 PCI 판 — pcim_* 접두사가 붙은 함수들을 제공한다.
 *
 * 문제의식은 이렇다. 드라이버의 probe 는 실패 지점이 여럿이고, 각 지점에서
 * 그때까지 잡은 것을 정확한 역순으로 풀어야 한다. goto 사슬로 그것을 쓰다
 * 보면 하나를 빠뜨리기 쉽고, 그 실수는 조용한 자원 누수가 된다.
 * devres 를 쓰면 잡은 것이 장치에 매달리고, remove 시점에 커널이 역순으로
 * 전부 푼다. probe 는 실패해도 그냥 return 하면 된다.
 *
 * 다루는 자원이 넷이다.
 *   - 장치 활성화(pcim_enable_device) — pci_disable_device 를 자동 호출
 *   - 자원 영역 예약(pcim_request_region 계열) — pci_release_region 자동
 *   - BAR 매핑(pcim_iomap 계열) — iounmap 자동
 *   - INTx 설정(pcim_intx) — 원래 상태로 자동 복원
 *
 * 구현 방식이 흥미롭다. devres 는 "해제 함수 + 데이터" 를 한 덩어리로
 * 장치에 매다는데, 이 파일은 자원 종류마다 그 덩어리의 모양과 해제
 * 함수를 정의한다. 예컨대 pcim_addr_devres 는 매핑 종류(BAR 번호,
 * 주소 범위)와 그것을 푸는 방법을 함께 담는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 드라이버 probe
 *   -> [이 파일] pcim_enable_device(pdev)
 *      -> pci_enable_device() + devres 에 해제 항목 등록
 *   -> [이 파일] pcim_iomap_regions(pdev, mask, name)
 *      -> pci_request_region + ioremap + 해제 항목 등록
 *   (실패하면 그냥 return — 정리는 커널이 한다)
 *
 * 드라이버 remove 또는 probe 실패
 *   -> 드라이버 코어가 devres 목록을 역순으로 훑으며 해제 함수 호출
 *      -> [이 파일] 각 해제 함수가 대응하는 pci_* 를 부른다
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 할당과 자원 조작이 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 드라이버들. pcim_ 을 쓰는 드라이버와 pci_ 를 직접 쓰는 드라이버가
 *   섞여 있고, 두 방식을 한 드라이버 안에서 섞으면 안 된다.
 * 아래쪽: drivers/base/devres.c 의 devres 코어, pci.c 의 자원 함수들.
 * 공유 상태: struct pci_dev 에 매달린 devres 목록(struct device 의 devres_head).
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 하나도 쓰지 않는다. drivers/nvme/ 에
 * "pcim_" 문자열이 0건이다(주석 제거 후 검색).
 *
 * NVMe 는 자원을 직접 잡고 직접 푼다 — pci_enable_device_mem(),
 * pci_request_mem_regions(), ioremap() 을 부르고, nvme_dev_unmap() 과
 * nvme_pci_disable() 에서 대칭적으로 되돌린다.
 *
 * 그 선택에는 이유가 있다. NVMe 는 컨트롤러 리셋 중에 자원을 잠시 놓았다가
 * 다시 잡는 경우가 있는데(nvme_dev_disable -> nvme_reset_work), devres 는
 * "드라이버가 떨어질 때 한 번에 푼다" 는 모델이라 그런 중간 해제를
 * 표현하기 어렵다. 수명이 probe~remove 와 정확히 일치하지 않는 자원에는
 * devres 가 맞지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * pcim_enable_device()      : pci_enable_device + 자동 해제 등록.
 *                             한 번 쓰면 그 장치의 다른 자원도 devres 로
 *                             관리된다는 표시(is_managed)가 선다.
 * pcim_iomap()              : BAR 하나를 매핑하고 해제를 등록한다.
 * pcim_iomap_regions()      : 마스크로 지정한 여러 BAR 을 한 번에
 *                             request + iomap 한다. 가장 자주 쓰이는 함수다.
 * pcim_iounmap()            : 명시적으로 풀 때. 등록된 항목도 함께 지운다.
 * pcim_request_region()     : 자원 영역만 예약(매핑은 하지 않는다).
 * pcim_intx()               : INTx 설정을 바꾸되 원래 값을 기억해 둔다.
 * pcim_set_mwi()            : Memory-Write-Invalidate 를 켜고 자동 해제 등록.
 * struct pcim_addr_devres   : 매핑/예약 하나를 나타내는 devres 항목.
 *                             종류(type), BAR 번호, 주소 범위를 담는다.
 * struct pcim_intx_devres   : INTx 의 원래 상태를 담는 항목.
 */

#include <linux/device.h> /* NVMe: PCI 장치의 device 생명주기 관리를 위한 핵심 헤더 포함. */
#include <linux/pci.h> /* NVMe: PCI 버스 및 NVMe 장치 레지스터 접근을 위한 PCI 핵심 헤더 포함. */
#include "pci.h" /* NVMe: PCI 서브시스템 내 부 선언을 위한 로컬 헤더 포함. */

/*
 * On the state of PCI's devres implementation:
 *
 * The older PCI devres API has one significant problem:
 *
 * It is very strongly tied to the statically allocated mapping table in struct
 * pcim_iomap_devres below. This is mostly solved in the sense of the pcim_
 * functions in this file providing things like ranged mapping by bypassing
 * this table, whereas the functions that were present in the old API still
 * enter the mapping addresses into the table for users of the old API.
 *
 * TODO:
 * Remove the legacy table entirely once all calls to pcim_iomap_table() in
 * the kernel have been removed.
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/devres.c)은 PCI 장치 드라이버가 사용하는 관리형
 * 리소스(devres) helper 함수들을 제공한다. 드라이버 detach(제거) 시
 * 자동으로 정리되어야 할 PCI 리소스(BAR region, BAR iomapping, INTx,
 * MWI, device enable 상태 등)를 device lifetime에 연결한다.
 *
 * NVMe PCIe SSD 입장에서 본 파일의 기능은 다음과 같다.
 *   - pcim_enable_device(): NVMe 컨트롤러의 PCI device 활성화를 관리형으로
 *     등록. 드라이버 제거 시 pci_disable_device()를 자동 수행.
 *   - pcim_iomap() / pcim_iomap_region() / pcim_iomap_range(): NVMe BAR0
 *     (register/doorbell 영역)이나 CMB가 위치한 BAR를 커널 가상 주소로
 *     매핑. 드라이버 제거 시 iounmap/release 를 자동 수행.
 *   - pcim_intx(): NVMe 장치가 MSI-X 대신 INTx를 사용하는 레거시 환경에서
 *     INTx enable/disable 을 관리하고, 드라이버 제거 시 원래 상태로 복원.
 *   - pcim_set_mwi(): Memory Write Invalidate 활성화를 관리형으로 등록.
 *   - pcim_pin_device(): NVMe 장치를 suspend/resume 중에도 비활성화되지
 *     않도록 고정할 때 사용.
 *   - devm_pci_remap_cfgspace()/devm_pci_remap_cfg_resource(): PCI
 *     configuration space 접근용 매핑을 관리형으로 수행. NVMe 드라이버는
 *     직접 호출하지 않지만 PCI core가 NVMe 장치의 config space를 다룰 때
 *     사용될 수 있다.
 *
 * 일반적인 NVMe 드라이버 호출 경로(예시):
 *   nvme_probe -> pci_enable_device_mem(pdev) (또는 pcim_enable_device)
 *              -> pci_set_master(pdev)
 *              -> pci_request_regions(pdev, DRV_NAME)
 *              -> pci_iomap(pdev, 0, ...) 또는 pcim_iomap_region(pdev, 0, ...)
 *              -> ioremap(pci_resource_start(pdev,0), size) (NVMe pci.c)
 *              -> readl/writel(dev->bar + NVME_REG_...)
 *              -> pci_alloc_irq_vectors() (MSI-X)
 *              -> dma_pool_create() (SQ/CQ/PRP/SGL descriptor)
 *              -> pci_save_state()/pci_restore_state() (suspend/resume)
 *              -> pci_disable_device() (remove/shutdown)
 *
 * 본 파일은 drivers/nvme/host/pci.c에서 직접 pcim_ 계열 함수를 모두
 * 사용하지는 않지만, NVMe 드라이버가 사용하는 PCI BAR, INTx, device enable
 * 등의 관리형 생명주기를 담당하는 핵심 코드이며, managed PCI API를 통해
 * 리소스 누수를 방지하는 역할을 한다.
 * ===================================================================
 */

/*
 * Legacy struct storing addresses to whole mapped BARs.
 */
struct pcim_iomap_devres { /* NVMe: 각 BAR에 대한 iomapped 가상 주소를 저장하는 레거시 관리형 리소스 구조체 정의. */
	void __iomem *table[PCI_NUM_RESOURCES]; /* NVMe: 각 BAR(0~5 등)에 대한 iomapped 가상 주소를 저장하는 레거시 테이블. */
}; /* NVMe: pcim_iomap_devres 구조체/열거형 정의 끝. */

/* Used to restore the old INTx state on driver detach. */
struct pcim_intx_devres { /* NVMe: 드라이버 로드 전 INTx 원래 상태를 저장하는 관리형 리소스 구조체 정의. */
	int orig_intx; /* NVMe: 드라이버 로드 전 INTx 원래 상태(활성/비활성)를 저장해 드라이버 제거 시 복원. */
}; /* NVMe: pcim_intx_devres 구조체/열거형 정의 끝. */

enum pcim_addr_devres_type { /* NVMe: BAR region/mapping devres의 종류를 구분하는 열거형 정의. */
	/* Default initializer. */
	PCIM_ADDR_DEVRES_TYPE_INVALID, /* NVMe: 초기화되지 않은 상태. */

	/* A requested region spanning an entire BAR. */
	PCIM_ADDR_DEVRES_TYPE_REGION, /* NVMe: 전체 BAR에 대한 region 요청만 기록. */

	/*
	 * A requested region spanning an entire BAR, and a mapping for
	 * the entire BAR.
	 */
	PCIM_ADDR_DEVRES_TYPE_REGION_MAPPING, /* NVMe: 전체 BAR의 region 요청과 iomapping을 함께 기록. */

	/*
	 * A mapping within a BAR, either spanning the whole BAR or just a
	 * range.  Without a requested region.
	 */
	PCIM_ADDR_DEVRES_TYPE_MAPPING, /* NVMe: 전체 또는 일부 BAR에 대한 iomapping만 기록. */
}; /* NVMe: pcim_addr_devres_type 구조체/열거형 정의 끝. */

/*
 * This struct envelops IO or MEM addresses, i.e., mappings and region
 * requests, because those are very frequently requested and released
 * together.
 */
struct pcim_addr_devres { /* NVMe: BAR region 요청과 iomapping 정보를 함께 저장하는 관리형 리소스 구조체 정의. */
	enum pcim_addr_devres_type type; /* NVMe: 이 devres가 어떤 종류의 BAR/매핑 리소스를 다루는지 식별. */
	void __iomem *baseaddr; /* NVMe: ioremap으로 얻은 NVMe BAR의 커널 가상 주소. */
	unsigned long offset; /* NVMe: BAR 내 매핑 오프셋(예: doorbell 영역의 시작 위치). */
	unsigned long len; /* NVMe: 매핑 길이(예: NVMe BAR0 전체 또는 일부 doorbell 크기). */
	int bar; /* NVMe: PCI BAR 인덱스(0, 1, 2, ...). */
}; /* NVMe: pcim_addr_devres 구조체/열거형 정의 끝. */

/*
 * pcim_addr_devres_clear:
 *   pcim_addr_devres 구조체를 안전한 초기 상태로 만든다.
 *   NVMe BAR 매핑/region 등록 전 devres 템플릿을 초기화할 때 사용된다.
 */
static inline void pcim_addr_devres_clear(struct pcim_addr_devres *res) /* NVMe: BAR 주소 관리 devres를 초기화. */
{ /* NVMe: pcim_addr_devres_clear() 함수 본문 시작. */
	memset(res, 0, sizeof(*res)); /* NVMe: devres 구조체를 0으로 초기화. */
	res->bar = -1; /* NVMe: BAR 인덱스를 유효하지 않은 값(-1)로 설정해 초기 상태 표시. */
} /* NVMe: pcim_addr_devres_clear() 함수 끝. */

/*
 * pcim_addr_resource_release:
 *   드라이버 detach 시 devres에 의해 자동으로 호출되어,
 *   NVMe 장치가 사용하던 BAR region / iomapping 을 정리한다.
 */
static void pcim_addr_resource_release(struct device *dev, void *resource_raw) /* NVMe: detach 시 BAR region/mapping 자동 해제 콜백. */
{ /* NVMe: pcim_addr_resource_release() 함수 본문 시작. */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: generic device에서 PCI device 구조체를 얻는다. */
	struct pcim_addr_devres *res = resource_raw; /* NVMe: 해제할 devres 데이터를 가져온다. */

	switch (res->type) { /* NVMe: devres 유형에 따라 적절한 해제 동작을 선택. */
	case PCIM_ADDR_DEVRES_TYPE_REGION: /* NVMe: BAR region 요청만 등록된 경우. */
		pci_release_region(pdev, res->bar); /* NVMe: NVMe 장치의 해당 BAR region을 반납. */
		break; /* NVMe: switch 분기를 빠져나간다. */
	case PCIM_ADDR_DEVRES_TYPE_REGION_MAPPING: /* NVMe: region 요청과 iomapping이 모두 등록된 경우. */
		pci_iounmap(pdev, res->baseaddr); /* NVMe: BAR에 대한 커널 가상 주소 매핑을 해제. */
		pci_release_region(pdev, res->bar); /* NVMe: BAR region을 반납. */
		break; /* NVMe: switch 분기를 빠져나간다. */
	case PCIM_ADDR_DEVRES_TYPE_MAPPING: /* NVMe: iomapping만 등록된 경우(전체 또는 범위). */
		pci_iounmap(pdev, res->baseaddr); /* NVMe: NVMe BAR 매핑을 해제. */
		break; /* NVMe: switch 분기를 빠져나간다. */
	default: /* NVMe: 정의되지 않은 유형은 아무것도 하지 않는다. */
		break; /* NVMe: switch 분기를 빠져나간다. */
	} /* NVMe: 낮은 코드 블록 종료. */
} /* NVMe: pcim_addr_resource_release() 함수 끝. */

/*
 * pcim_addr_devres_alloc:
 *   NVMe 장치가 속한 NUMA 노드를 고려해 BAR/region 관리용 devres를 할당한다.
 */
static struct pcim_addr_devres *pcim_addr_devres_alloc(struct pci_dev *pdev) /* NVMe: BAR 주소 관리용 devres 할당. */
{ /* NVMe: pcim_addr_devres_alloc() 함수 본문 시작. */
	struct pcim_addr_devres *res; /* NVMe: 새로 할당할 devres 포인터. */

	res = devres_alloc_node(pcim_addr_resource_release, sizeof(*res), /* NVMe: devres_alloc_node() 호출 결과를 res에 저장. */
				GFP_KERNEL, dev_to_node(&pdev->dev)); /* NVMe: NVMe 장치가 속한 NUMA 노드에 맞춰 devres를 할당. */
	if (res) /* NVMe: 할당에 성공하면 구조체를 초기화. */
		pcim_addr_devres_clear(res); /* NVMe: type을 INVALID로, bar를 -1로 초기화. */
	return res; /* NVMe: 할당된 devres 포인터 반환(실패 시 NULL). */
} /* NVMe: pcim_addr_devres_alloc() 함수 끝. */

/* Just for consistency and readability. */
static inline void pcim_addr_devres_free(struct pcim_addr_devres *res) /* NVMe: 미등록 devres 메모리 해제. */
{ /* NVMe: pcim_addr_devres_free() 함수 본문 시작. */
	devres_free(res); /* NVMe: 할당만 되고 등록되지 않은 임시 devres 메모리를 해제. */
} /* NVMe: pcim_addr_devres_free() 함수 끝. */

/*
 * Used by devres to identify a pcim_addr_devres.
 */
static int pcim_addr_resources_match(struct device *dev, /* NVMe: 동일한 BAR 주소 리소스인지 비교. */
				     void *a_raw, void *b_raw) /* NVMe: 두 devres 포인터(a_raw, b_raw)를 받는 매개변수 선언. */
{ /* NVMe: pcim_addr_resources_match() 함수 본문 시작. */
	struct pcim_addr_devres *a, *b; /* NVMe: 비교할 두 devres 포인터. */

	a = a_raw; /* NVMe: 첫 번째 devres 데이터. */
	b = b_raw; /* NVMe: 두 번째 devres 데이터. */

	if (a->type != b->type) /* NVMe: 관리형 BAR 리소스의 종류가 다를면 다른 리소스로 판단. */
		return 0; /* NVMe: 일치하지 않음을 반환. */

	switch (a->type) { /* NVMe: 같은 종류일 때 세부 기준으로 비교. */
	case PCIM_ADDR_DEVRES_TYPE_REGION: /* NVMe: region만 요청한 경우. */
	case PCIM_ADDR_DEVRES_TYPE_REGION_MAPPING: /* NVMe: region + mapping 경우. */
		return a->bar == b->bar; /* NVMe: 같은 BAR 인덱스를 가리키면 일치. */
	case PCIM_ADDR_DEVRES_TYPE_MAPPING: /* NVMe: mapping만 등록한 경우. */
		return a->baseaddr == b->baseaddr; /* NVMe: 같은 커널 가상 주소를 가리키면 일치. */
	default: /* NVMe: 그 외에는 일치하지 않는 것으로 처리. */
		return 0; /* NVMe: 성공(0)을 반환. */
	} /* NVMe: 낮은 코드 블록 종료. */
} /* NVMe: pcim_addr_resources_match() 함수 끝. */

/*
 * devm_pci_unmap_iospace:
 *   관리형 I/O space 매핑 해제 콜백. NVMe는 주로 MMIO를 사용하지만,
 *   일부 레거시 환경에서 I/O space 기반 PCI 접근 시 사용될 수 있다.
 */
static void devm_pci_unmap_iospace(struct device *dev, void *ptr) /* NVMe: 관리형 I/O space 매핑 해제 콜백. */
{ /* NVMe: devm_pci_unmap_iospace() 함수 본문 시작. */
	struct resource **res = ptr; /* NVMe: 해제할 I/O space 리소스 이중 포인터. */

	pci_unmap_iospace(*res); /* NVMe: 해당 I/O space 매핑을 해제. */
} /* NVMe: devm_pci_unmap_iospace() 함수 끝. */

/**
 * devm_pci_remap_iospace - Managed pci_remap_iospace()
 * @dev: Generic device to remap IO address for
 * @res: Resource describing the I/O space
 * @phys_addr: physical address of range to be mapped
 *
 * Managed pci_remap_iospace().  Map is automatically unmapped on driver
 * detach.
 */
int devm_pci_remap_iospace(struct device *dev, const struct resource *res, /* NVMe: 관리형 PCI I/O space 매핑. */
			   phys_addr_t phys_addr) /* NVMe: 매핑할 물리 주소(phys_addr) 매개변수 선언. */
{ /* NVMe: devm_pci_remap_iospace() 함수 본문 시작. */
	const struct resource **ptr; /* NVMe: devres에 저장할 리소스 포인터. */
	int error; /* NVMe: 매핑 결과를 저장할 변수. */

	ptr = devres_alloc(devm_pci_unmap_iospace, sizeof(*ptr), GFP_KERNEL); /* NVMe: 관리형 해제 콜백과 함께 devres 할당. */
	if (!ptr) /* NVMe: 메모리 할당 실패 검사. */
		return -ENOMEM; /* NVMe: 메모리 부족 오류 반환. */

	error = pci_remap_iospace(res, phys_addr); /* NVMe: 물리 주소에 해당하는 PCI I/O space를 커널에 매핑. */
	if (error) { /* NVMe: 매핑 실패 시. */
		devres_free(ptr); /* NVMe: 할당한 devres만 해제. */
	} else	{ /* NVMe: 매핑 성공 시. */
		*ptr = res; /* NVMe: devres에 해제할 리소스를 기록. */
		devres_add(dev, ptr); /* NVMe: device의 devres 리스트에 등록해 detach 시 자동 해제. */
	} /* NVMe: 낮은 코드 블록 종료. */

	return error; /* NVMe: pci_remap_iospace()의 결과 반환. */
} /* NVMe: devm_pci_remap_iospace() 함수 끝. */
EXPORT_SYMBOL(devm_pci_remap_iospace); /* NVMe: devm_pci_remap_iospace 심볼을 NVMe/PCI 드라이버 모듈에 노출. */

/**
 * devm_pci_remap_cfgspace - Managed pci_remap_cfgspace()
 * @dev: Generic device to remap IO address for
 * @offset: Resource address to map
 * @size: Size of map
 *
 * Managed pci_remap_cfgspace().  Map is automatically unmapped on driver
 * detach.
 */
void __iomem *devm_pci_remap_cfgspace(struct device *dev, /* NVMe: 관리형 PCI config space 매핑. */
				      resource_size_t offset, /* NVMe: 매핑할 config space의 오프셋 매개변수 선언. */
				      resource_size_t size) /* NVMe: 매핑할 config space의 크기 매개변수 선언. */
{ /* NVMe: devm_pci_remap_cfgspace() 함수 본문 시작. */
	void __iomem **ptr, *addr; /* NVMe: devres 포인터와 매핑된 가상 주소. */

	ptr = devres_alloc(devm_ioremap_release, sizeof(*ptr), GFP_KERNEL); /* NVMe: ioremap 해제용 devm_ioremap_release devres 할당. */
	if (!ptr) /* NVMe: 메모리 부족 시. */
		return NULL; /* NVMe: 실패를 NULL로 반환. */

	addr = pci_remap_cfgspace(offset, size); /* NVMe: PCI config space 영역을 커널 가상 주소에 매핑. */
	if (addr) { /* NVMe: 매핑 성공 시. */
		*ptr = addr; /* NVMe: devres에 매핑 주소 저장. */
		devres_add(dev, ptr); /* NVMe: device lifetime에 연결. */
	} else /* NVMe: 매핑 실패 시. */
		devres_free(ptr); /* NVMe: devres만 해제. */

	return addr; /* NVMe: 매핑된 가상 주소(또는 NULL) 반환. */
} /* NVMe: devm_pci_remap_cfgspace() 함수 끝. */
EXPORT_SYMBOL(devm_pci_remap_cfgspace); /* NVMe: devm_pci_remap_cfgspace 심볼을 NVMe/PCI 드라이버 모듈에 노출. */

/**
 * devm_pci_remap_cfg_resource - check, request region and ioremap cfg resource
 * @dev: generic device to handle the resource for
 * @res: configuration space resource to be handled
 *
 * Checks that a resource is a valid memory region, requests the memory
 * region and ioremaps with pci_remap_cfgspace() API that ensures the
 * proper PCI configuration space memory attributes are guaranteed.
 *
 * All operations are managed and will be undone on driver detach.
 *
 * Returns a pointer to the remapped memory or an IOMEM_ERR_PTR() encoded error
 * code on failure. Usage example::
 *
 *	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
 *	base = devm_pci_remap_cfg_resource(&pdev->dev, res);
 *	if (IS_ERR(base))
 *		return PTR_ERR(base);
 */
void __iomem *devm_pci_remap_cfg_resource(struct device *dev, /* NVMe: PCI config space region 요청 및 매핑. */
					  struct resource *res) /* NVMe: 매핑할 PCI config space 리소스 매개변수 선언. */
{ /* NVMe: devm_pci_remap_cfg_resource() 함수 본문 시작. */
	resource_size_t size; /* NVMe: 매핑할 리소스의 크기. */
	const char *name; /* NVMe: 메모리 region에 부여할 이름. */
	void __iomem *dest_ptr; /* NVMe: 최종 매핑된 가상 주소. */

	BUG_ON(!dev); /* NVMe: device 포인터가 반드시 유효해야 함. */

	if (!res || resource_type(res) != IORESOURCE_MEM) { /* NVMe: 리소스가 유효한 MEM 영역인지 확인. */
		dev_err(dev, "invalid resource\n"); /* NVMe: 잘못된 리소스를 로그로 출력. */
		return IOMEM_ERR_PTR(-EINVAL); /* NVMe: 잘못된 인자 오류 반환. */
	} /* NVMe: 낮은 코드 블록 종료. */

	size = resource_size(res); /* NVMe: 리소스 시작~끝 길이 계산. */

	if (res->name) /* NVMe: 리소스에 이름이 있으면. */
		name = devm_kasprintf(dev, GFP_KERNEL, "%s %s", dev_name(dev), /* NVMe: devm_kasprintf() 호출 결과를 name에 저장. */
				      res->name); /* NVMe: "device_name res_name" 형태로 이름 생성. */
	else /* NVMe: 리소스 이름이 없으면. */
		name = devm_kstrdup(dev, dev_name(dev), GFP_KERNEL); /* NVMe: device 이름만 복사. */
	if (!name) /* NVMe: 이름 할당 실패 검사. */
		return IOMEM_ERR_PTR(-ENOMEM); /* NVMe: 메모리 부족 오류 반환. */

	if (!devm_request_mem_region(dev, res->start, size, name)) { /* NVMe: 물리 메모리 region을 요청(다른 드라이버와 충돌 방지). */
		dev_err(dev, "can't request region for resource %pR\n", res); /* NVMe: region 요청 실패 로그. */
		return IOMEM_ERR_PTR(-EBUSY); /* NVMe: 리소스가 사용 중임을 알리는 오류 반환. */
	} /* NVMe: 낮은 코드 블록 종료. */

	dest_ptr = devm_pci_remap_cfgspace(dev, res->start, size); /* NVMe: config space 속성을 유지하며 ioremap 수행. */
	if (!dest_ptr) { /* NVMe: ioremap 실패 시. */
		dev_err(dev, "ioremap failed for resource %pR\n", res); /* NVMe: 매핑 실패 로그. */
		devm_release_mem_region(dev, res->start, size); /* NVMe: 앞서 요청한 mem region을 반납. */
		dest_ptr = IOMEM_ERR_PTR(-ENOMEM); /* NVMe: 메모리 부족으로 오류 포인터 설정. */
	} /* NVMe: 낮은 코드 블록 종료. */

	return dest_ptr; /* NVMe: 매핑 주소 또는 오류 포인터 반환. */
} /* NVMe: devm_pci_remap_cfg_resource() 함수 끝. */
EXPORT_SYMBOL(devm_pci_remap_cfg_resource); /* NVMe: devm_pci_remap_cfg_resource 심볼을 NVMe/PCI 드라이버 모듈에 노출. */

/*
 * __pcim_clear_mwi:
 *   pcim_set_mwi() 등록 시 드라이버 detach에 호출되어
 *   NVMe 장치의 Memory Write Invalidate(MWI)를 해제한다.
 */
static void __pcim_clear_mwi(void *pdev_raw) /* NVMe: 드라이버 제거 시 MWI 해제 콜백. */
{ /* NVMe: __pcim_clear_mwi() 함수 본문 시작. */
	struct pci_dev *pdev = pdev_raw; /* NVMe: 해제할 PCI device. */

	pci_clear_mwi(pdev); /* NVMe: NVMe 장치의 MWI 비트를 해제. */
} /* NVMe: __pcim_clear_mwi() 함수 끝. */

/**
 * pcim_set_mwi - a device-managed pci_set_mwi()
 * @pdev: the PCI device for which MWI is enabled
 *
 * Managed pci_set_mwi().
 *
 * RETURNS: An appropriate -ERRNO error value on error, or zero for success.
 */
int pcim_set_mwi(struct pci_dev *pdev) /* NVMe: 관리형 MWI 활성화. */
{ /* NVMe: pcim_set_mwi() 함수 본문 시작. */
	int ret; /* NVMe: 함수 호출 결과. */

	ret = devm_add_action(&pdev->dev, __pcim_clear_mwi, pdev); /* NVMe: 드라이버 detach 시 MWI를 끌 콜백을 등록. */
	if (ret != 0) /* NVMe: devres 등록 실패 검사. */
		return ret; /* NVMe: 오류를 그대로 반환. */

	ret = pci_set_mwi(pdev); /* NVMe: PCI command 레지스터의 MWI 비트를 설정. */
	if (ret != 0) /* NVMe: MWI 설정 실패 시. */
		devm_remove_action(&pdev->dev, __pcim_clear_mwi, pdev); /* NVMe: 등록한 해제 콜백을 제거. */

	return ret; /* NVMe: 성공(0) 또는 오류 반환. */
} /* NVMe: pcim_set_mwi() 함수 끝. */
EXPORT_SYMBOL(pcim_set_mwi); /* NVMe: pcim_set_mwi 심볼을 NVMe/PCI 드라이버 모듈에 노출. */

static inline bool mask_contains_bar(int mask, int bar) /* NVMe: mask에 특정 BAR 비트가 포함됐는지 확인. */
{ /* NVMe: mask_contains_bar() 함수 본문 시작. */
	return mask & BIT(bar); /* NVMe: mask에서 해당 BAR 비트가 1인지 확인. */
} /* NVMe: mask_contains_bar() 함수 끝. */

/*
 * pcim_intx_restore:
 *   드라이버 detach 시 원래 INTx 상태로 복원한다.
 *   NVMe가 MSI-X 대신 INTx를 쓰는 경우에 해당한다.
 */
static void pcim_intx_restore(struct device *dev, void *data) /* NVMe: 드라이버 제거 시 INTx 원래 상태 복원 콜백. */
{ /* NVMe: pcim_intx_restore() 함수 본문 시작. */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: generic device에서 PCI device 추출. */
	struct pcim_intx_devres *res = data; /* NVMe: 저장된 원래 INTx 상태. */

	pci_intx(pdev, res->orig_intx); /* NVMe: 드라이버 제거 시 원래 INTx 상태로 복원. */
} /* NVMe: pcim_intx_restore() 함수 끝. */

/*
 * save_orig_intx:
 *   pcim_intx() 최초 호출 시 PCI command 레지스터를 읽어
 *   NVMe 장치의 원래 INTx 상태를 보관한다.
 */
static void save_orig_intx(struct pci_dev *pdev, struct pcim_intx_devres *res) /* NVMe: NVMe 장치의 원래 INTx 상태 저장. */
{ /* NVMe: save_orig_intx() 함수 본문 시작. */
	u16 pci_command; /* NVMe: PCI command 레지스터 값. */

	pci_read_config_word(pdev, PCI_COMMAND, &pci_command); /* NVMe: NVMe 장치의 PCI command 레지스터를 읽는다. */
	res->orig_intx = !(pci_command & PCI_COMMAND_INTX_DISABLE); /* NVMe: INTx disable 비트가 0이면 원래 INTx가 활성화된 것으로 기록. */
} /* NVMe: save_orig_intx() 함수 끝. */

/**
 * pcim_intx - managed pci_intx()
 * @pdev: the PCI device to operate on
 * @enable: boolean: whether to enable or disable PCI INTx
 *
 * Returns: 0 on success, -ENOMEM on error.
 *
 * Enable/disable PCI INTx for device @pdev.
 * Restore the original state on driver detach.
 */
int pcim_intx(struct pci_dev *pdev, int enable) /* NVMe: 관리형 INTx 활성화/비활성화. */
{ /* NVMe: pcim_intx() 함수 본문 시작. */
	struct pcim_intx_devres *res; /* NVMe: INTx 상태를 저장할 devres. */
	struct device *dev = &pdev->dev; /* NVMe: generic device 포인터. */

	/*
	 * pcim_intx() must only restore the INTx value that existed before the
	 * driver was loaded, i.e., before it called pcim_intx() for the
	 * first time.
	 */
	res = devres_find(dev, pcim_intx_restore, NULL, NULL); /* NVMe: 이미 등록된 INTx 복원 devres가 있는지 검색. */
	if (!res) { /* NVMe: 처음 호출된 경우에만 원래 상태를 저장. */
		res = devres_alloc(pcim_intx_restore, sizeof(*res), GFP_KERNEL); /* NVMe: 복원 콜백과 상태 저장 공간 할당. */
		if (!res) /* NVMe: 메모리 부족 시. */
			return -ENOMEM; /* NVMe: 메모리 부족 오류 반환. */

		save_orig_intx(pdev, res); /* NVMe: 현재 command 레지스터에서 원래 INTx 상태를 읽어 저장. */
		devres_add(dev, res); /* NVMe: device의 devres 리스트에 등록. */
	} /* NVMe: 낮은 코드 블록 종료. */

	pci_intx(pdev, enable); /* NVMe: 요청에 따라 NVMe 장치의 INTx를 활성화/비활성화. */

	return 0; /* NVMe: 성공. */
} /* NVMe: pcim_intx() 함수 끝. */
EXPORT_SYMBOL_GPL(pcim_intx); /* NVMe: pcim_intx 심볼을 NVMe/PCI 드라이버 모듈에 노출. */

/*
 * pcim_disable_device:
 *   pcim_enable_device()가 등록한 해제 콜백. 드라이버 detach 시
 *   NVMe 장치가 고정되지 않았다면 pci_disable_device()를 호출한다.
 */
static void pcim_disable_device(void *pdev_raw) /* NVMe: 관리형 PCI 장치 비활성화 콜백. */
{ /* NVMe: pcim_disable_device() 함수 본문 시작. */
	struct pci_dev *pdev = pdev_raw; /* NVMe: disable할 PCI device. */

	if (!pdev->pinned) /* NVMe: 장치가 고정(pin)되지 않은 경우에만 disable. */
		pci_disable_device(pdev); /* NVMe: NVMe 장치의 I/O/MEM 리소스를 비활성화. */

	pdev->is_managed = false; /* NVMe: managed 상태 플래그를 해제. */
} /* NVMe: pcim_disable_device() 함수 끝. */

/**
 * pcim_enable_device - Managed pci_enable_device()
 * @pdev: PCI device to be initialized
 *
 * Returns: 0 on success, negative error code on failure.
 *
 * Managed pci_enable_device(). Device will automatically be disabled on
 * driver detach.
 */
int pcim_enable_device(struct pci_dev *pdev) /* NVMe: 관리형 PCI 장치 활성화. */
{ /* NVMe: pcim_enable_device() 함수 본문 시작. */
	int ret; /* NVMe: 결과 코드. */

	ret = devm_add_action(&pdev->dev, pcim_disable_device, pdev); /* NVMe: detach 시 자동으로 pci_disable_device()가 호출되도록 등록. */
	if (ret != 0) /* NVMe: devres 등록 실패 시. */
		return ret; /* NVMe: 오류 반환. */

	/*
	 * We prefer removing the action in case of an error over
	 * devm_add_action_or_reset() because the latter could theoretically be
	 * disturbed by users having pinned the device too soon.
	 */
	ret = pci_enable_device(pdev); /* NVMe: NVMe 장치의 PCI command 레지스터에서 I/O/MEM 버스 마스터링 등을 활성화. */
	if (ret != 0) { /* NVMe: enable 실패 시. */
		devm_remove_action(&pdev->dev, pcim_disable_device, pdev); /* NVMe: 등록한 disable 콜백을 제거. */
		return ret; /* NVMe: 오류 반환. */
	} /* NVMe: 낮은 코드 블록 종료. */

	pdev->is_managed = true; /* NVMe: managed enable 상태임을 표시. */

	return ret; /* NVMe: 성공(0) 반환. */
} /* NVMe: pcim_enable_device() 함수 끝. */
EXPORT_SYMBOL(pcim_enable_device); /* NVMe: pcim_enable_device 심볼을 NVMe/PCI 드라이버 모듈에 노출. */

/**
 * pcim_pin_device - Pin managed PCI device
 * @pdev: PCI device to pin
 *
 * Pin managed PCI device @pdev. Pinned device won't be disabled on driver
 * detach. @pdev must have been enabled with pcim_enable_device().
 */
void pcim_pin_device(struct pci_dev *pdev) /* NVMe: 관리형 장치를 pin해 제거 시 비활성화 방지. */
{ /* NVMe: pcim_pin_device() 함수 본문 시작. */
	pdev->pinned = true; /* NVMe: 드라이버 detach 시에도 pci_disable_device()가 호출되지 않도록 장치를 고정. */
} /* NVMe: pcim_pin_device() 함수 끝. */
EXPORT_SYMBOL(pcim_pin_device); /* NVMe: pcim_pin_device 심볼을 NVMe/PCI 드라이버 모듈에 노출. */

/*
 * pcim_iomap_release:
 *   레거시 iomap 테이블용 no-op 해제 콜백.
 *   실제 매핑 정리는 매핑 등록 시 사용된 콜백에서 수행된다.
 */
static void pcim_iomap_release(struct device *gendev, void *res) /* NVMe: 레거시 iomap 테이블용 no-op 해제 콜백. */
{ /* NVMe: pcim_iomap_release() 함수 본문 시작. */
	/*
	 * Do nothing. This is legacy code.
	 *
	 * Cleanup of the mappings is now done directly through the callbacks
	 * registered when creating them.
	 */
} /* NVMe: pcim_iomap_release() 함수 끝. */

/**
 * pcim_iomap_table - access iomap allocation table (DEPRECATED)
 * @pdev: PCI device to access iomap table for
 *
 * Returns:
 * Const pointer to array of __iomem pointers on success, NULL on failure.
 *
 * Access iomap allocation table for @dev.  If iomap table doesn't
 * exist and @pdev is managed, it will be allocated.  All iomaps
 * recorded in the iomap table are automatically unmapped on driver
 * detach.
 *
 * This function might sleep when the table is first allocated but can
 * be safely called without context and guaranteed to succeed once
 * allocated.
 *
 * This function is DEPRECATED. Do not use it in new code. Instead, obtain a
 * mapping's address directly from one of the pcim_* mapping functions. For
 * example:
 * void __iomem \*mappy = pcim_iomap(pdev, bar, length);
 */
void __iomem * const *pcim_iomap_table(struct pci_dev *pdev) /* NVMe: 레거시 BAR iomap 테이블 접근(할당). */
{ /* NVMe: pcim_iomap_table() 함수 본문 시작. */
	struct pcim_iomap_devres *dr, *new_dr; /* NVMe: 기존 테이블과 새로 할당할 테이블. */

	dr = devres_find(&pdev->dev, pcim_iomap_release, NULL, NULL); /* NVMe: device에 이미 할당된 레거시 iomap 테이블을 검색. */
	if (dr) /* NVMe: 기존 테이블이 있으면. */
		return dr->table; /* NVMe: 해당 테이블 포인터 반환. */

	new_dr = devres_alloc_node(pcim_iomap_release, sizeof(*new_dr), GFP_KERNEL, /* NVMe: devres_alloc_node() 호출 결과를 new_dr에 저장. */
				   dev_to_node(&pdev->dev)); /* NVMe: NUMA 노드를 고려해 새 레거시 테이블 할당. */
	if (!new_dr) /* NVMe: 메모리 부족 시. */
		return NULL; /* NVMe: NULL 반환. */
	dr = devres_get(&pdev->dev, new_dr, NULL, NULL); /* NVMe: device의 devres에 등록하거나 기존 동일 devres를 반환. */
	return dr->table; /* NVMe: BAR별 매핑 주소 배열 반환. */
} /* NVMe: pcim_iomap_table() 함수 끝. */
EXPORT_SYMBOL(pcim_iomap_table); /* NVMe: pcim_iomap_table 심볼을 NVMe/PCI 드라이버 모듈에 노출. */

/*
 * Fill the legacy mapping-table, so that drivers using the old API can
 * still get a BAR's mapping address through pcim_iomap_table().
 */
static int pcim_add_mapping_to_legacy_table(struct pci_dev *pdev, /* NVMe: 매핑을 레거시 iomap 테이블에 기록. */
					    void __iomem *mapping, int bar) /* NVMe: 매핑 주소(mapping)와 BAR 인덱스(bar) 매개변수 선언. */
{ /* NVMe: pcim_add_mapping_to_legacy_table() 함수 본문 시작. */
	void __iomem **legacy_iomap_table; /* NVMe: 레거시 테이블 포인터. */

	if (!pci_bar_index_is_valid(bar)) /* NVMe: BAR 인덱스가 유효한지 확인. */
		return -EINVAL; /* NVMe: 잘못된 BAR 인덱스 오류. */

	legacy_iomap_table = (void __iomem **)pcim_iomap_table(pdev); /* NVMe: NVMe 장치의 레거시 iomap 테이블 획득. */
	if (!legacy_iomap_table) /* NVMe: 테이블 할당 실패 시. */
		return -ENOMEM; /* NVMe: 메모리 부족 오류. */

	legacy_iomap_table[bar] = mapping; /* NVMe: 해당 BAR 슬롯에 매핑 주소 저장. */

	return 0; /* NVMe: 성공. */
} /* NVMe: pcim_add_mapping_to_legacy_table() 함수 끝. */

/*
 * Remove a mapping. The table only contains whole-BAR mappings, so this will
 * never interfere with ranged mappings.
 */
static void pcim_remove_mapping_from_legacy_table(struct pci_dev *pdev, /* NVMe: 주소로 레거시 iomap 테이블 항목 제거. */
						  void __iomem *addr) /* NVMe: 제거할 매핑 가상 주소(addr) 매개변수 선언. */
{ /* NVMe: pcim_remove_mapping_from_legacy_table() 함수 본문 시작. */
	int bar; /* NVMe: 순회할 BAR 인덱스. */
	void __iomem **legacy_iomap_table; /* NVMe: 레거시 테이블. */

	legacy_iomap_table = (void __iomem **)pcim_iomap_table(pdev); /* NVMe: NVMe 장치의 레거시 테이블 획득. */
	if (!legacy_iomap_table) /* NVMe: 테이블이 없으면 아무것도 안 함. */
		return; /* NVMe: 레거시 테이블이 없으면 즉시 반환. */

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) { /* NVMe: 표준 6개 BAR를 순회. */
		if (legacy_iomap_table[bar] == addr) { /* NVMe: 제거할 매핑 주소를 찾으면. */
			legacy_iomap_table[bar] = NULL; /* NVMe: 해당 슬롯을 비움. */
			return; /* NVMe: 하나만 제거하고 종료. */
		} /* NVMe: 낮은 코드 블록 종료. */
	} /* NVMe: 낮은 코드 블록 종료. */
} /* NVMe: pcim_remove_mapping_from_legacy_table() 함수 끝. */

/*
 * The same as pcim_remove_mapping_from_legacy_table(), but identifies the
 * mapping by its BAR index.
 */
static void pcim_remove_bar_from_legacy_table(struct pci_dev *pdev, int bar) /* NVMe: BAR 인덱스로 레거시 iomap 테이블 항목 제거. */
{ /* NVMe: pcim_remove_bar_from_legacy_table() 함수 본문 시작. */
	void __iomem **legacy_iomap_table; /* NVMe: 레거시 테이블 포인터. */

	if (!pci_bar_index_is_valid(bar)) /* NVMe: BAR 인덱스 유효성 검사. */
		return; /* NVMe: 잘못된 BAR 인덱스이므로 즉시 반환. */

	legacy_iomap_table = (void __iomem **)pcim_iomap_table(pdev); /* NVMe: NVMe 장치의 레거시 테이블 획득. */
	if (!legacy_iomap_table) /* NVMe: 테이블이 없으면 종료. */
		return; /* NVMe: 레거시 테이블이 없으면 즉시 반환. */

	legacy_iomap_table[bar] = NULL; /* NVMe: 지정한 BAR 슬롯을 비움. */
} /* NVMe: pcim_remove_bar_from_legacy_table() 함수 끝. */

/**
 * pcim_iomap - Managed pcim_iomap()
 * @pdev: PCI device to iomap for
 * @bar: BAR to iomap
 * @maxlen: Maximum length of iomap
 *
 * Returns: __iomem pointer on success, NULL on failure.
 *
 * Managed pci_iomap(). Map is automatically unmapped on driver detach. If
 * desired, unmap manually only with pcim_iounmap().
 *
 * This SHOULD only be used once per BAR.
 *
 * NOTE:
 * Contrary to the other pcim_* functions, this function does not return an
 * IOMEM_ERR_PTR() on failure, but a simple NULL. This is done for backwards
 * compatibility.
 */
void __iomem *pcim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen) /* NVMe: 단일 BAR 관리형 ioremap. */
{ /* NVMe: pcim_iomap() 함수 본문 시작. */
	void __iomem *mapping; /* NVMe: ioremap 결과 가상 주소. */
	struct pcim_addr_devres *res; /* NVMe: 관리형 매핑 devres. */

	if (!pci_bar_index_is_valid(bar)) /* NVMe: 요청한 BAR 인덱스가 유효한지 검사. */
		return NULL; /* NVMe: 잘못된 BAR이면 NULL 반환. */

	res = pcim_addr_devres_alloc(pdev); /* NVMe: BAR 매핑용 devres를 할당. */
	if (!res) /* NVMe: 메모리 부족 시. */
		return NULL; /* NVMe: NULL 반환. */
	res->type = PCIM_ADDR_DEVRES_TYPE_MAPPING; /* NVMe: 이 devres는 mapping 전용임을 표시. */

	mapping = pci_iomap(pdev, bar, maxlen); /* NVMe: NVMe BAR를 커널 가상 주소 공간에 매핑. */
	if (!mapping) /* NVMe: 매핑 실패 시. */
		goto err_iomap; /* NVMe: 오류 처리 경로로 이동. */
	res->baseaddr = mapping; /* NVMe: devres에 매핑 주소 기록. */

	if (pcim_add_mapping_to_legacy_table(pdev, mapping, bar) != 0) /* NVMe: 레거시 API를 위한 테이블에도 등록. */
		goto err_table; /* NVMe: 테이블 등록 실패 시 매핑 해제. */

	devres_add(&pdev->dev, res); /* NVMe: device lifetime에 devres를 연결. */
	return mapping; /* NVMe: NVMe 드라이버가 사용할 가상 주소 반환. */

err_table: /* NVMe: err_table 레이블: 실패 시 뒷정리 수행 지점. */
	pci_iounmap(pdev, mapping); /* NVMe: 레거시 테이블 등록 실패로 매핑을 해제. */
err_iomap: /* NVMe: err_iomap 레이블: 실패 시 뒷정리 수행 지점. */
	pcim_addr_devres_free(res); /* NVMe: 사용하지 않는 devres 메모리 해제. */
	return NULL; /* NVMe: 최종 실패 반환. */
} /* NVMe: pcim_iomap() 함수 끝. */
EXPORT_SYMBOL(pcim_iomap); /* NVMe: pcim_iomap 심볼을 NVMe/PCI 드라이버 모듈에 노출. */

/**
 * pcim_iounmap - Managed pci_iounmap()
 * @pdev: PCI device to iounmap for
 * @addr: Address to unmap
 *
 * Managed pci_iounmap(). @addr must have been mapped using a pcim_* mapping
 * function.
 */
void pcim_iounmap(struct pci_dev *pdev, void __iomem *addr) /* NVMe: 관리형 BAR iounmap. */
{ /* NVMe: pcim_iounmap() 함수 본문 시작. */
	struct pcim_addr_devres res_searched; /* NVMe: 검색할 매핑 devres 템플릿. */

	pcim_addr_devres_clear(&res_searched); /* NVMe: 템플릿을 초기화. */
	res_searched.type = PCIM_ADDR_DEVRES_TYPE_MAPPING; /* NVMe: mapping 타입으로 설정. */
	res_searched.baseaddr = addr; /* NVMe: 찾을 가상 주소 설정. */

	if (devres_release(&pdev->dev, pcim_addr_resource_release, /* NVMe: 조건 검사를 시작. */
			pcim_addr_resources_match, &res_searched) != 0) { /* NVMe: devres 리스트에서 해당 매핑을 찾아 해제. */
		/* Doesn't exist. User passed nonsense. */
		return; /* NVMe: 일치하는 매핑이 없으면 조용히 리턴. */
	} /* NVMe: 낮은 코드 블록 종료. */

	pcim_remove_mapping_from_legacy_table(pdev, addr); /* NVMe: 레거시 테이블에서도 해당 주소를 제거. */
} /* NVMe: pcim_iounmap() 함수 끝. */
EXPORT_SYMBOL(pcim_iounmap); /* NVMe: pcim_iounmap 심볼을 NVMe/PCI 드라이버 모듈에 노출. */

/**
 * pcim_iomap_region - Request and iomap a PCI BAR
 * @pdev: PCI device to map IO resources for
 * @bar: Index of a BAR to map
 * @name: Name of the driver requesting the resource
 *
 * Returns: __iomem pointer on success, an IOMEM_ERR_PTR on failure.
 *
 * Mapping and region will get automatically released on driver detach. If
 * desired, release manually only with pcim_iounmap_region().
 */
void __iomem *pcim_iomap_region(struct pci_dev *pdev, int bar, /* NVMe: BAR region 요청 및 관리형 ioremap. */
			       const char *name) /* NVMe: region 요청 드라이버 이름(name) 매개변수 선언. */
{ /* NVMe: pcim_iomap_region() 함수 본문 시작. */
	int ret; /* NVMe: 결과 코드. */
	struct pcim_addr_devres *res; /* NVMe: region+mapping devres. */

	if (!pci_bar_index_is_valid(bar)) /* NVMe: BAR 인덱스 유효성 검사. */
		return IOMEM_ERR_PTR(-EINVAL); /* NVMe: 잘못된 BAR 오류. */

	res = pcim_addr_devres_alloc(pdev); /* NVMe: devres 할당. */
	if (!res) /* NVMe: 메모리 부족. */
		return IOMEM_ERR_PTR(-ENOMEM); /* NVMe: 메모리 부족 오류. */

	res->type = PCIM_ADDR_DEVRES_TYPE_REGION_MAPPING; /* NVMe: region 요청과 mapping을 함께 관리. */
	res->bar = bar; /* NVMe: 관리할 BAR 인덱스 기록. */

	ret = pci_request_region(pdev, bar, name); /* NVMe: NVMe 장치의 해당 BAR region을 요청(다른 드라이버와 충돌 방지). */
	if (ret != 0) /* NVMe: region 요청 실패. */
		goto err_region; /* NVMe: 오류 처리. */

	res->baseaddr = pci_iomap(pdev, bar, 0); /* NVMe: BAR 전체를 ioremap. */
	if (!res->baseaddr) { /* NVMe: 매핑 실패. */
		ret = -EINVAL; /* NVMe: 오류 코드 설정. */
		goto err_iomap; /* NVMe: region 반납 후 종료. */
	} /* NVMe: 낮은 코드 블록 종료. */

	devres_add(&pdev->dev, res); /* NVMe: devres 등록으로 detach 시 자동 해제. */
	return res->baseaddr; /* NVMe: NVMe 드라이버가 접근할 가상 주소 반환. */

err_iomap: /* NVMe: err_iomap 레이블: 실패 시 뒷정리 수행 지점. */
	pci_release_region(pdev, bar); /* NVMe: 요청한 region을 반납. */
err_region: /* NVMe: err_region 레이블: 실패 시 뒷정리 수행 지점. */
	pcim_addr_devres_free(res); /* NVMe: devres 메모리 해제. */

	return IOMEM_ERR_PTR(ret); /* NVMe: 오류 포인터 반환. */
} /* NVMe: pcim_iomap_region() 함수 끝. */
EXPORT_SYMBOL(pcim_iomap_region); /* NVMe: pcim_iomap_region 심볼을 NVMe/PCI 드라이버 모듈에 노출. */

/**
 * pcim_iounmap_region - Unmap and release a PCI BAR
 * @pdev: PCI device to operate on
 * @bar: Index of BAR to unmap and release
 *
 * Unmap a BAR and release its region manually. Only pass BARs that were
 * previously mapped by pcim_iomap_region().
 */
void pcim_iounmap_region(struct pci_dev *pdev, int bar) /* NVMe: BAR region 및 매핑 수동 해제. */
{ /* NVMe: pcim_iounmap_region() 함수 본문 시작. */
	struct pcim_addr_devres res_searched; /* NVMe: 검색용 devres 템플릿. */

	pcim_addr_devres_clear(&res_searched); /* NVMe: 템플릿 초기화. */
	res_searched.type = PCIM_ADDR_DEVRES_TYPE_REGION_MAPPING; /* NVMe: region+mapping 타입으로 설정. */
	res_searched.bar = bar; /* NVMe: 해제할 BAR 인덱스 지정. */

	devres_release(&pdev->dev, pcim_addr_resource_release, /* NVMe: devres_release() 함수 호출. */
			pcim_addr_resources_match, &res_searched); /* NVMe: 해당 BAR의 region과 mapping을 모두 해제. */
} /* NVMe: pcim_iounmap_region() 함수 끝. */
EXPORT_SYMBOL(pcim_iounmap_region); /* NVMe: pcim_iounmap_region 심볼을 NVMe/PCI 드라이버 모듈에 노출. */

/**
 * pcim_iomap_regions - Request and iomap PCI BARs (DEPRECATED)
 * @pdev: PCI device to map IO resources for
 * @mask: Mask of BARs to request and iomap
 * @name: Name of the driver requesting the resources
 *
 * Returns: 0 on success, negative error code on failure.
 *
 * Request and iomap regions specified by @mask.
 *
 * This function is DEPRECATED. Do not use it in new code.
 * Use pcim_iomap_region() instead.
 */
int pcim_iomap_regions(struct pci_dev *pdev, int mask, const char *name) /* NVMe: 여러 BAR region 요청 및 매핑(레거시). */
{ /* NVMe: pcim_iomap_regions() 함수 본문 시작. */
	int ret; /* NVMe: 결과 코드. */
	int bar; /* NVMe: 순회할 BAR 인덱스. */
	void __iomem *mapping; /* NVMe: 각 BAR 매핑 주소. */

	for (bar = 0; bar < DEVICE_COUNT_RESOURCE; bar++) { /* NVMe: 모든 PCI 리소스 슬롯을 순회. */
		if (!mask_contains_bar(mask, bar)) /* NVMe: mask에 포함되지 않은 BAR는 건너뜀. */
			continue; /* NVMe: 다음 BAR로 진행. */

		mapping = pcim_iomap_region(pdev, bar, name); /* NVMe: 해당 BAR를 요청하고 ioremap. */
		if (IS_ERR(mapping)) { /* NVMe: 매핑 실패. */
			ret = PTR_ERR(mapping); /* NVMe: 오류 코드 추출. */
			goto err; /* NVMe: 앞서 매핑한 BAR들을 정리. */
		} /* NVMe: 낮은 코드 블록 종료. */
		ret = pcim_add_mapping_to_legacy_table(pdev, mapping, bar); /* NVMe: 레거시 테이블에도 추가. */
		if (ret != 0) /* NVMe: 레거시 테이블 등록 실패. */
			goto err; /* NVMe: 정리. */
	} /* NVMe: 낮은 코드 블록 종료. */

	return 0; /* NVMe: 모든 요청한 BAR 매핑 성공. */

err: /* NVMe: err 레이블: 실패 시 뒷정리 수행 지점. */
	while (--bar >= 0) { /* NVMe: 실패한 BAR 직전까지 역순으로 롤백. */
		pcim_iounmap_region(pdev, bar); /* NVMe: 각 BAR의 region과 mapping을 해제. */
		pcim_remove_bar_from_legacy_table(pdev, bar); /* NVMe: 레거시 테이블에서도 제거. */
	} /* NVMe: 낮은 코드 블록 종료. */

	return ret; /* NVMe: 오류 코드 반환. */
} /* NVMe: pcim_iomap_regions() 함수 끝. */
EXPORT_SYMBOL(pcim_iomap_regions); /* NVMe: pcim_iomap_regions 심볼을 NVMe/PCI 드라이버 모듈에 노출. */

/**
 * pcim_request_region - Request a PCI BAR
 * @pdev: PCI device to request region for
 * @bar: Index of BAR to request
 * @name: Name of the driver requesting the resource
 *
 * Returns: 0 on success, a negative error code on failure.
 *
 * Request region specified by @bar.
 *
 * The region will automatically be released on driver detach. If desired,
 * release manually only with pcim_release_region().
 */
int pcim_request_region(struct pci_dev *pdev, int bar, const char *name) /* NVMe: 단일 BAR region 관리형 요청. */
{ /* NVMe: pcim_request_region() 함수 본문 시작. */
	int ret; /* NVMe: 결과. */
	struct pcim_addr_devres *res; /* NVMe: region devres. */

	if (!pci_bar_index_is_valid(bar)) /* NVMe: BAR 인덱스 검사. */
		return -EINVAL; /* NVMe: 잘못된 BAR 오류. */

	res = pcim_addr_devres_alloc(pdev); /* NVMe: devres 할당. */
	if (!res) /* NVMe: 메모리 부족. */
		return -ENOMEM; /* NVMe: 메모리 부족 오류. */
	res->type = PCIM_ADDR_DEVRES_TYPE_REGION; /* NVMe: region 전용으로 설정. */
	res->bar = bar; /* NVMe: BAR 인덱스 기록. */

	ret = pci_request_region(pdev, bar, name); /* NVMe: NVMe 장치의 BAR region을 요청. */
	if (ret != 0) { /* NVMe: 요청 실패. */
		pcim_addr_devres_free(res); /* NVMe: devres 메모리 해제. */
		return ret; /* NVMe: 오류 반환. */
	} /* NVMe: 낮은 코드 블록 종료. */

	devres_add(&pdev->dev, res); /* NVMe: detach 시 자동 반납하도록 등록. */
	return 0; /* NVMe: 성공. */
} /* NVMe: pcim_request_region() 함수 끝. */
EXPORT_SYMBOL(pcim_request_region); /* NVMe: pcim_request_region 심볼을 NVMe/PCI 드라이버 모듈에 노출. */

/**
 * pcim_release_region - Release a PCI BAR
 * @pdev: PCI device to operate on
 * @bar: Index of BAR to release
 *
 * Release a region manually that was previously requested by
 * pcim_request_region().
 */
static void pcim_release_region(struct pci_dev *pdev, int bar) /* NVMe: 단일 BAR region 수동 반납. */
{ /* NVMe: pcim_release_region() 함수 본문 시작. */
	struct pcim_addr_devres res_searched; /* NVMe: 검색용 템플릿. */

	pcim_addr_devres_clear(&res_searched); /* NVMe: 초기화. */
	res_searched.type = PCIM_ADDR_DEVRES_TYPE_REGION; /* NVMe: region 타입 설정. */
	res_searched.bar = bar; /* NVMe: 반납할 BAR 인덱스. */

	devres_release(&pdev->dev, pcim_addr_resource_release, /* NVMe: devres_release() 함수 호출. */
			pcim_addr_resources_match, &res_searched); /* NVMe: 해당 BAR region을 반납. */
} /* NVMe: pcim_release_region() 함수 끝. */


/**
 * pcim_release_all_regions - Release all regions of a PCI-device
 * @pdev: the PCI device
 *
 * Release all regions previously requested through pcim_request_region()
 * or pcim_request_all_regions().
 *
 * Can be called from any context, i.e., not necessarily as a counterpart to
 * pcim_request_all_regions().
 */
static void pcim_release_all_regions(struct pci_dev *pdev) /* NVMe: 모든 표준 BAR region 수동 반납. */
{ /* NVMe: pcim_release_all_regions() 함수 본문 시작. */
	int bar; /* NVMe: BAR 인덱스. */

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) /* NVMe: 표준 6개 BAR 모두. */
		pcim_release_region(pdev, bar); /* NVMe: 각 BAR region을 반납. */
} /* NVMe: pcim_release_all_regions() 함수 끝. */

/**
 * pcim_request_all_regions - Request all regions
 * @pdev: PCI device to map IO resources for
 * @name: name of the driver requesting the resources
 *
 * Returns: 0 on success, negative error code on failure.
 *
 * Requested regions will automatically be released at driver detach. If
 * desired, release individual regions with pcim_release_region() or all of
 * them at once with pcim_release_all_regions().
 */
int pcim_request_all_regions(struct pci_dev *pdev, const char *name) /* NVMe: 모든 표준 BAR region 관리형 요청. */
{ /* NVMe: pcim_request_all_regions() 함수 본문 시작. */
	int ret; /* NVMe: 결과. */
	int bar; /* NVMe: BAR 인덱스. */

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) { /* NVMe: 모든 표준 BAR에 대해. */
		ret = pcim_request_region(pdev, bar, name); /* NVMe: 각 BAR region을 요청. */
		if (ret != 0) /* NVMe: 하나라도 실패하면. */
			goto err; /* NVMe: 롤백. */
	} /* NVMe: 낮은 코드 블록 종료. */

	return 0; /* NVMe: 모든 BAR 요청 성공. */

err: /* NVMe: err 레이블: 실패 시 뒷정리 수행 지점. */
	pcim_release_all_regions(pdev); /* NVMe: 이미 요청한 region들을 모두 반납. */

	return ret; /* NVMe: 오류 반환. */
} /* NVMe: pcim_request_all_regions() 함수 끝. */
EXPORT_SYMBOL(pcim_request_all_regions); /* NVMe: pcim_request_all_regions 심볼을 NVMe/PCI 드라이버 모듈에 노출. */

/**
 * pcim_iomap_range - Create a ranged __iomap mapping within a PCI BAR
 * @pdev: PCI device to map IO resources for
 * @bar: Index of the BAR
 * @offset: Offset from the begin of the BAR
 * @len: Length in bytes for the mapping
 *
 * Returns: __iomem pointer on success, an IOMEM_ERR_PTR on failure.
 *
 * Creates a new IO-Mapping within the specified @bar, ranging from @offset to
 * @offset + @len.
 *
 * The mapping will automatically get unmapped on driver detach. If desired,
 * release manually only with pcim_iounmap().
 */
void __iomem *pcim_iomap_range(struct pci_dev *pdev, int bar, /* NVMe: BAR 내 범위 관리형 ioremap. */
		unsigned long offset, unsigned long len) /* NVMe: BAR 내 오프셋과 길이(offset, len) 매개변수 선언. */
{ /* NVMe: pcim_iomap_range() 함수 본문 시작. */
	void __iomem *mapping; /* NVMe: 매핑된 가상 주소. */
	struct pcim_addr_devres *res; /* NVMe: mapping devres. */

	if (!pci_bar_index_is_valid(bar)) /* NVMe: BAR 인덱스 유효성 검사. */
		return IOMEM_ERR_PTR(-EINVAL); /* NVMe: 잘못된 BAR 오류. */

	res = pcim_addr_devres_alloc(pdev); /* NVMe: devres 할당. */
	if (!res) /* NVMe: 메모리 부족. */
		return IOMEM_ERR_PTR(-ENOMEM); /* NVMe: 메모리 부족 오류. */

	mapping = pci_iomap_range(pdev, bar, offset, len); /* NVMe: NVMe BAR 내 offset부터 len 바이트만큼만 ioremap. */
	if (!mapping) { /* NVMe: 범위 매핑 실패. */
		pcim_addr_devres_free(res); /* NVMe: devres 메모리 해제. */
		return IOMEM_ERR_PTR(-EINVAL); /* NVMe: 오류 반환. */
	} /* NVMe: 낮은 코드 블록 종료. */

	res->type = PCIM_ADDR_DEVRES_TYPE_MAPPING; /* NVMe: mapping 전용 devres. */
	res->baseaddr = mapping; /* NVMe: 매핑 주소 기록. */

	/*
	 * Ranged mappings don't get added to the legacy-table, since the table
	 * only ever keeps track of whole BARs.
	 */
	/* NVMe: 범위 매핑은 전체 BAR 매핑이 아니므로 레거시 테이블에 저장하지 않는다. */

	devres_add(&pdev->dev, res); /* NVMe: device lifetime에 연결. */
	return mapping; /* NVMe: 범위 매핑된 가상 주소 반환. */
} /* NVMe: pcim_iomap_range() 함수 끝. */
EXPORT_SYMBOL(pcim_iomap_range); /* NVMe: pcim_iomap_range 심볼을 NVMe/PCI 드라이버 모듈에 노출. */
