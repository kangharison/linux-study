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
 * === 주요 함수/구조체 요약 ===
 * pcim_enable_device()      : pci_enable_device + 자동 해제 등록.
 *                             한 번 쓰면 그 장치의 다른 자원도 devres 로
 *                             관리된다는 표시(is_managed)가 선다.
 * pcim_iomap()              : BAR 하나를 매핑하고 해제를 등록한다.
 * pcim_iomap_region()       : BAR 하나를 **예약까지 하고** 매핑한다.
 *                             pcim_iomap() 과 달리 다른 드라이버의 동시
 *                             사용을 막으므로 새 코드는 이쪽을 쓴다.
 * pcim_iomap_regions()      : 마스크로 지정한 여러 BAR 을 한 번에
 *                             request + iomap 하고 구형 조회 테이블에도
 *                             기록한다.
 * pcim_iomap_range()        : BAR 안의 일부 범위만 매핑한다. 구형 테이블에는
 *                             기록하지 않는다 -- 그 테이블은 BAR 하나당 한
 *                             칸이라 부분 범위를 담을 자리가 없기 때문이다
 *                             (상류 주석에 명시).
 * pcim_iomap_table()        : BAR 번호로 매핑 주소를 되찾는 구형 조회 표.
 *                             해제 콜백(pcim_iomap_release)이 **빈 함수**인
 *                             것은 실제 해제를 각 매핑이 따로 하기 때문이고,
 *                             그 콜백은 devres 에서 이 표를 식별하는
 *                             이름표 역할만 한다.
 * pcim_iounmap()            : 명시적으로 풀 때. 등록된 항목도 함께 지운다.
 * pcim_request_region()     : 자원 영역만 예약(매핑은 하지 않는다).
 * pcim_intx()               : INTx 설정을 바꾸되 원래 값을 기억해 둔다.
 * pcim_set_mwi()            : Memory-Write-Invalidate 를 켜고 자동 해제 등록.
 * struct pcim_addr_devres   : 매핑/예약 하나를 나타내는 devres 항목.
 *                             종류(type), BAR 번호, 매핑 주소를 담는다.
 *                             offset/len 필드도 있으나 **이 파일 안에서
 *                             채우거나 읽는 곳이 없다** -- 향후 확장을 위해
 *                             남겨 둔 것으로 보인다.
 *                             bar 의 '없음' 값이 0 이 아니라 **-1** 인 이유:
 *                             0 이 유효한 BAR0 이라 매칭이 잘못 걸린다.
 * struct pcim_intx_devres   : INTx 의 원래 상태를 담는 항목. 레지스터의
 *                             '끔=1' 의미를 뒤집어 저장해, 복원 때
 *                             pci_intx() 에 그대로 넘길 수 있게 한다.
 *
 * === NVMe 관점 (필수 4섹션에 대한 부가 절) ===
 * drivers/nvme 는 이 파일의 함수를 하나도 쓰지 않는다 -- "pcim_" 도
 * "devm_pci_" 도 0건이다(이 트리에서 확인).
 *
 * NVMe 는 자원을 직접 잡고 직접 푼다: pci_enable_device_mem()(pci.c:3164),
 * pci_request_mem_regions()(:3537), pci_release_mem_regions()(:3245, :3545).
 * nvme_dev_unmap() 이 그 짝이다.
 *
 * 그 선택에는 이유가 있다. NVMe 는 컨트롤러 리셋 중에 자원을 잠시 놓았다가
 * 다시 잡는데, devres 는 "드라이버가 떨어질 때 한 번에 푼다" 는 모델이라
 * 그런 중간 해제를 표현하기 어렵다. 수명이 probe~remove 와 정확히 일치하지
 * 않는 자원에는 devres 가 맞지 않는다.
 * (위 줄번호는 원본 스냅숏 1f0e418bb6 기준이다.)
 */

#include <linux/device.h>
#include <linux/pci.h>
#include "pci.h"

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
struct pcim_iomap_devres {
	void __iomem *table[PCI_NUM_RESOURCES];
};

/* Used to restore the old INTx state on driver detach. */
struct pcim_intx_devres {
	int orig_intx;
/* [한국어] 이 구조체가 필드 하나뿐인 것은 devres 가 '해제 콜백 + 데이터' 쌍으로만
 * 동작하기 때문이다. 데이터가 하나라도 구조체로 감싸야 devres 슬롯이 된다. */
};

enum pcim_addr_devres_type {
	/* Default initializer. */
	PCIM_ADDR_DEVRES_TYPE_INVALID,

	/* A requested region spanning an entire BAR. */
	PCIM_ADDR_DEVRES_TYPE_REGION,

	/*
	 * A requested region spanning an entire BAR, and a mapping for
	 * the entire BAR.
	 */
	PCIM_ADDR_DEVRES_TYPE_REGION_MAPPING,

	/*
	 * A mapping within a BAR, either spanning the whole BAR or just a
	 * range.  Without a requested region.
	 */
	PCIM_ADDR_DEVRES_TYPE_MAPPING,
};

/*
 * This struct envelops IO or MEM addresses, i.e., mappings and region
 * requests, because those are very frequently requested and released
 * together.
 */
struct pcim_addr_devres {
	enum pcim_addr_devres_type type;
	/* [한국어] 매핑된 커널 가상 주소.
	 * 설정자: pcim_iomap / pcim_iomap_region / pcim_iomap_range.
	 * 읽는 자: 해제 콜백의 pci_iounmap, 매칭 콜백의 MAPPING 비교.
	 * 값 범위: REGION 타입에서는 쓰이지 않아 NULL 로 남는다.
	 * 동기화: devres 코어의 잠금이 목록을 지킨다. */
	void __iomem *baseaddr;
	/* [한국어] 범위 매핑의 시작 오프셋.
	 * 설정자: 어느 함수도 채우지 않는다 -- pcim_iomap_range 조차 쓰지 않는다.
	 * 읽는 자: 없음.
	 * 값 범위: 항상 0(clear 가 memset 으로 비운 값).
	 * 동기화: 해당 없음. 향후 확장을 위해 남겨 둔 필드로 보인다. */
	unsigned long offset;
	/* [한국어] 범위 매핑의 길이. offset 과 마찬가지로 **이 파일 안에서 채우거나 읽는
	 * 곳이 없다.** */
	unsigned long len;
	/* [한국어] 이 자원이 대응하는 BAR 번호.
	 * 설정자: pcim_iomap_region / pcim_request_region.
	 * 읽는 자: 해제 콜백의 pci_release_region, 매칭 콜백의 REGION 계열 비교.
	 * 값 범위: **-1 이 '없음'** 이다. 0 이 유효한 BAR0 이라 0 을 쓸 수 없어
	 * pcim_addr_devres_clear 가 따로 -1 을 넣는다.
	 * 동기화: devres 코어의 잠금. */
	int bar;
/* [한국어] 이 서술자 하나가 세 종류의 주소 자원을 모두 표현한다 -- type 필드가
 * 어느 필드를 쓸지 정한다. */
};

/*
 * pcim_addr_devres_clear:
 *   pcim_addr_devres 구조체를 안전한 초기 상태로 만든다.
 *   NVMe BAR 매핑/region 등록 전 devres 템플릿을 초기화할 때 사용된다.
 */
/* [한국어]
 * pcim_addr_devres_clear - 주소 자원 서술자를 '아무것도 잡지 않은' 상태로 만든다
 *
 * @res: 초기화할 서술자.
 * @return: 없음.
 *
 * memset 만으로 끝내지 않고 **bar 를 -1 로** 따로 넣는 점이 핵심이다.
 * 0 은 유효한 BAR 번호(BAR0)이므로, 0 으로 남겨 두면
 * pcim_addr_resources_match() 가 BAR0 자원을 잘못 골라낼 수 있다.
 *
 * 두 곳에서 쓰인다: 새로 할당한 서술자를 비울 때, 그리고 devres 를 찾기
 * 위한 **검색 키**를 스택에 만들 때(pcim_iounmap 등).
 *
 * 실행 컨텍스트: 어디서나. 순수 메모리 조작이다.
 *
 * 호출 체인:
 *   pcim_addr_devres_alloc / pcim_iounmap / pcim_iounmap_region /
 *   pcim_release_region → [이 함수]
 */
static inline void pcim_addr_devres_clear(struct pcim_addr_devres *res)
{
	memset(res, 0, sizeof(*res));
	res->bar = -1;
/* [한국어] bar 를 -1 로 만드는 이 한 줄이 이 함수의 존재 이유다. memset 만으로는
 * 0(= BAR0)이 되어 매칭이 잘못 걸린다. */
}

/*
 * pcim_addr_resource_release:
 *   드라이버 detach 시 devres에 의해 자동으로 호출되어,
 *   NVMe 장치가 사용하던 BAR region / iomapping 을 정리한다.
 */
/* [한국어]
 * pcim_addr_resource_release - devres 가 자원을 되돌릴 때 부르는 해제 콜백
 *
 * @dev: 자원의 주인 장치.
 * @resource_raw: pcim_addr_devres 서술자.
 * @return: 없음.
 *
 * 이 파일의 세 가지 주소 자원(영역 예약, 영역+매핑, 매핑만)을 한 콜백이
 * 모두 처리한다. devres 는 콜백 함수 포인터로 자원 종류를 구별하므로,
 * **같은 콜백을 쓰는 것이 곧 같은 종류**라는 뜻이다 -- 그래서 서술자 안의
 * type 으로 다시 갈라야 한다.
 *
 * REGION_MAPPING 의 해제 순서가 요점이다: 먼저 iounmap 하고 그 다음
 * release_region 한다. 반대로 하면 예약이 풀린 범위를 여전히 매핑한 채로
 * 남는 구간이 생긴다.
 *
 * default 분기가 아무것도 하지 않는 것은 INVALID 타입(잘못 초기화된
 * 서술자)을 조용히 넘기기 위해서다.
 *
 * 실행 컨텍스트: 드라이버 분리 시 devres 해제 경로, 또는 devres_release()
 * 를 부르는 명시적 해제 경로. 프로세스 문맥.
 *
 * 호출 체인:
 *   devres 코어 / devres_release → [이 함수]
 *     → pci_iounmap → pci_release_region
 */
static void pcim_addr_resource_release(struct device *dev, void *resource_raw)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pcim_addr_devres *res = resource_raw;
/* [한국어] type 으로 어느 자원인지 가른다 -- devres 는 콜백 포인터로만 종류를
 * 구별하므로, 한 콜백을 공유하는 셋을 여기서 다시 나눠야 한다. */

	switch (res->type) {
	/* [한국어] 예약만 한 자원. */
	case PCIM_ADDR_DEVRES_TYPE_REGION:
		/* [한국어] 매핑이 없으므로 예약만 되돌린다. */
		pci_release_region(pdev, res->bar);
		break;
	case PCIM_ADDR_DEVRES_TYPE_REGION_MAPPING:
		/* [한국어] 예약과 매핑을 함께 한 자원. **매핑을 먼저 푼다** -- 반대로 하면 예약이
		 * 풀린 범위를 여전히 매핑한 채로 남는 구간이 생긴다. */
		pci_iounmap(pdev, res->baseaddr);
		pci_release_region(pdev, res->bar);
		/* [한국어] 두 단계를 마쳤다. */
		break;
	case PCIM_ADDR_DEVRES_TYPE_MAPPING:
		/* [한국어] 매핑만 한 자원. 예약이 없으므로 iounmap 만 한다. */
		pci_iounmap(pdev, res->baseaddr);
		break;
	default:
		break;
	}
}

/*
 * pcim_addr_devres_alloc:
 *   NVMe 장치가 속한 NUMA 노드를 고려해 BAR/region 관리용 devres를 할당한다.
 */
/* [한국어]
 * pcim_addr_devres_alloc - 주소 자원 서술자를 NUMA 지역성을 고려해 할당한다
 *
 * @pdev: 자원의 주인 장치.
 * @return: 초기화된 서술자, 또는 NULL.
 *
 * devres_alloc_node 로 **장치가 붙은 NUMA 노드에** 할당한다. 이 서술자는
 * 해제 경로에서만 읽히므로 성능에 큰 영향은 없지만, 장치 관련 할당을
 * 같은 노드에 모으는 커널의 일반 관례를 따른다.
 *
 * 할당에 성공하면 곧바로 clear 를 불러 bar 를 -1 로 만든다 -- 호출자가
 * type 과 필요한 필드만 채우면 되게 하는 장치다.
 *
 * **아직 devres 에 등록하지 않는다.** devres_add 는 실제 자원을 잡는 데
 * 성공한 뒤에야 호출자가 부른다. 실패하면 pcim_addr_devres_free 로 그냥
 * 버리면 되므로, 반쯤 등록된 상태가 생기지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. GFP_KERNEL 로 잠들 수 있다.
 *
 * 호출 체인:
 *   pcim_iomap / pcim_iomap_region / pcim_request_region /
 *   pcim_iomap_range → [이 함수] → devres_alloc_node
 */
static struct pcim_addr_devres *pcim_addr_devres_alloc(struct pci_dev *pdev)
{
	struct pcim_addr_devres *res;

	res = devres_alloc_node(pcim_addr_resource_release, sizeof(*res),
				/* [한국어] 장치가 붙은 NUMA 노드에 할당한다. 장치 관련 할당을 같은 노드에 모으는
				 * 커널의 일반 관례다. */
				GFP_KERNEL, dev_to_node(&pdev->dev));
	if (res)
		/* [한국어] 할당 직후 bar 를 -1 로 만들어, 호출자가 type 과 필요한 필드만 채우면
		 * 되게 한다. */
		pcim_addr_devres_clear(res);
	return res;
/* [한국어] **아직 devres_add 하지 않는다** -- 실제 자원을 잡는 데 성공한 뒤에야
 * 호출자가 등록한다. 그래서 반쯤 등록된 상태가 생기지 않는다. */
}

/* Just for consistency and readability. */
/* [한국어]
 * pcim_addr_devres_free - 아직 등록되지 않은 서술자를 버린다
 *
 * @res: 버릴 서술자.
 * @return: 없음.
 *
 * devres_free() 는 **devres_add 되지 않은** 것만 해제한다. 이미 등록된
 * 자원은 devres_release 로 풀어야 한다 -- 두 함수를 혼동하면 이중 해제가
 * 된다.
 *
 * 그래서 이 함수는 오직 '자원을 잡는 데 실패해 등록 전에 되돌리는' 경로
 * 에서만 쓰인다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pcim_iomap / pcim_iomap_region / pcim_request_region /
 *   pcim_iomap_range 의 실패 경로 → [이 함수] → devres_free
 */
static inline void pcim_addr_devres_free(struct pcim_addr_devres *res)
{
	devres_free(res);
}

/*
 * Used by devres to identify a pcim_addr_devres.
 */
/* [한국어]
 * pcim_addr_resources_match - devres 탐색에서 두 주소 자원이 같은 것인지 판정한다
 *
 * @dev: 장치. 쓰지 않는다.
 * @a_raw: 등록된 서술자.
 * @b_raw: 찾고 있는 서술자(검색 키).
 * @return: 1 이면 같은 자원.
 *
 * devres_release() 가 어느 항목을 풀지 고를 때 쓰는 콜백이다. 종류가
 * 먼저 같아야 하고, 그 다음 종류별로 다른 기준을 쓴다:
 *  - REGION / REGION_MAPPING: **BAR 번호**로 구별한다. 한 BAR 에 대해
 *    같은 종류의 자원이 둘일 수 없기 때문이다.
 *  - MAPPING: **매핑 주소**로 구별한다. 같은 BAR 안에서도 범위가 다른
 *    매핑이 여럿 있을 수 있어(pcim_iomap_range) BAR 로는 부족하다.
 *
 * default 가 0 을 돌려주는 것은 INVALID 타입끼리는 절대 일치시키지
 * 않겠다는 뜻이다.
 *
 * 실행 컨텍스트: devres 코어의 잠금 아래. 순수 비교만 한다.
 *
 * 호출 체인:
 *   devres_release → [이 함수]
 */
static int pcim_addr_resources_match(struct device *dev,
				     void *a_raw, void *b_raw)
{
	struct pcim_addr_devres *a, *b;

	a = a_raw;
	/* [한국어] 검색 키 쪽. */
	b = b_raw;
/* [한국어] 먼저 종류가 같은지 본다. */

	if (a->type != b->type)
		/* [한국어] 종류가 다르면 볼 것도 없다. */
		return 0;

	switch (a->type) {
	/* [한국어] 예약만 한 자원과 예약+매핑 자원은 **BAR 번호**로 구별한다. 한 BAR 에
	 * 같은 종류가 둘일 수 없기 때문이다. */
	case PCIM_ADDR_DEVRES_TYPE_REGION:
	/* [한국어] 두 종류가 같은 기준을 쓰므로 case 를 이어 붙였다. */
	case PCIM_ADDR_DEVRES_TYPE_REGION_MAPPING:
		return a->bar == b->bar;
	case PCIM_ADDR_DEVRES_TYPE_MAPPING:
		/* [한국어] 매핑만 한 자원은 **주소**로 구별한다. 같은 BAR 안에서도 범위가 다른
		 * 매핑이 여럿 있을 수 있어(pcim_iomap_range) BAR 로는 부족하다. */
		return a->baseaddr == b->baseaddr;
	default:
		return 0;
	}
}

/*
 * devm_pci_unmap_iospace:
 *   관리형 I/O space 매핑 해제 콜백. NVMe는 주로 MMIO를 사용하지만,
 *   일부 레거시 환경에서 I/O space 기반 PCI 접근 시 사용될 수 있다.
 */
/* [한국어]
 * devm_pci_unmap_iospace - IO 공간 매핑을 되돌리는 devres 콜백
 *
 * @dev: 장치. 쓰지 않는다.
 * @ptr: 저장해 둔 resource 포인터의 주소.
 * @return: 없음.
 *
 * devres 는 콜백에 '저장한 데이터의 주소' 를 넘기므로, 한 번 역참조해
 * 실제 resource 포인터를 꺼낸다.
 *
 * 실행 컨텍스트: 드라이버 분리 시의 프로세스 문맥.
 *
 * 호출 체인:
 *   devres 코어 → [이 함수] → pci_unmap_iospace
 */
static void devm_pci_unmap_iospace(struct device *dev, void *ptr)
{
	struct resource **res = ptr;

	pci_unmap_iospace(*res);
}

/**
 * devm_pci_remap_iospace - Managed pci_remap_iospace()
 * @dev: Generic device to remap IO address for
 * @res: Resource describing the I/O space
 * @phys_addr: physical address of range to be mapped
 *
 * Managed pci_remap_iospace().  Map is automatically unmapped on driver
 * detach.
 */
/* [한국어]
 * devm_pci_remap_iospace - IO 공간을 매핑하고 자동 해제되게 등록한다
 *
 * @dev: 이 매핑을 소유할 장치.
 * @res: 매핑할 IO 자원.
 * @phys_addr: 대응하는 물리 주소.
 * @return: 0 성공, -ENOMEM 또는 pci_remap_iospace 의 실패값.
 *
 * devres 관용구의 전형이다: **먼저 devres 슬롯을 잡고, 실제 작업을 한 뒤,
 * 성공했을 때만 등록한다.** 순서가 이런 이유는 devres_alloc 이 잠들 수 있는
 * 할당이라 실제 매핑 뒤로 미루면 실패 시 되돌릴 것이 늘기 때문이다.
 *
 * 실패하면 devres_free 로 슬롯만 버린다 -- 등록 전이므로 이중 해제가 없다.
 *
 * 저장하는 것이 매핑 결과가 아니라 **resource 포인터**인 점에 유의.
 * pci_unmap_iospace 가 그 자원을 인자로 받기 때문이다. 그래서 호출자는
 * res 가 가리키는 구조체를 이 매핑보다 오래 살려 두어야 한다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   호스트 브리지 드라이버 → [이 함수] → pci_remap_iospace → devres_add
 */
int devm_pci_remap_iospace(struct device *dev, const struct resource *res,
			   phys_addr_t phys_addr)
{
	const struct resource **ptr;
	int error;
/* [한국어] 먼저 devres 슬롯을 잡는다 -- 실제 작업 뒤로 미루면 실패 시 되돌릴 것이
 * 늘어난다. */

	ptr = devres_alloc(devm_pci_unmap_iospace, sizeof(*ptr), GFP_KERNEL);
	/* [한국어] 슬롯 할당 실패. */
	if (!ptr)
		/* [한국어] 아직 매핑하지 않았으므로 되돌릴 것이 없다. */
		return -ENOMEM;

	error = pci_remap_iospace(res, phys_addr);
	/* [한국어] 매핑 실패. */
	if (error) {
		/* [한국어] 등록 전이므로 devres_free 로 슬롯만 버린다. devres_release 를 쓰면
		 * 이중 해제가 된다. */
		devres_free(ptr);
	} else	{
		*ptr = res;
		devres_add(dev, ptr);
	}

	return error;
}
EXPORT_SYMBOL(devm_pci_remap_iospace);

/**
 * devm_pci_remap_cfgspace - Managed pci_remap_cfgspace()
 * @dev: Generic device to remap IO address for
 * @offset: Resource address to map
 * @size: Size of map
 *
 * Managed pci_remap_cfgspace().  Map is automatically unmapped on driver
 * detach.
 */
/* [한국어]
 * devm_pci_remap_cfgspace - 설정공간 접근에 맞는 속성으로 매핑하고 자동 해제되게 한다
 *
 * @dev: 이 매핑을 소유할 장치.
 * @offset: 매핑할 물리 주소.
 * @size: 크기.
 * @return: 매핑된 주소, 또는 NULL.
 *
 * 일반 ioremap 과 다른 점은 **메모리 속성**이다. PCI 설정공간은 쓰기 결합
 * (write-combining)이나 재정렬이 일어나면 안 되므로, 아키텍처가 제공하는
 * pci_remap_cfgspace() 를 쓴다.
 *
 * 해제 콜백으로 **devm_ioremap_release** 를 쓰는 점에 유의 -- 이 파일이
 * 정의한 것이 아니라 devres 코어의 공용 콜백이다. 그 덕에 iounmap 만 하면
 * 되는 단순한 해제를 위해 자체 콜백을 만들 필요가 없다.
 *
 * 다만 그것이 부작용도 낳는다: 같은 콜백을 쓰는 다른 devm_ioremap 매핑과
 * devres 상에서 구별되지 않으므로, 이 매핑만 골라 해제하는 API 가 없다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   devm_pci_remap_cfg_resource / 컨트롤러 드라이버 → [이 함수]
 *     → pci_remap_cfgspace → devres_add
 */
void __iomem *devm_pci_remap_cfgspace(struct device *dev,
				      resource_size_t offset,
				      resource_size_t size)
{
	void __iomem **ptr, *addr;

	ptr = devres_alloc(devm_ioremap_release, sizeof(*ptr), GFP_KERNEL);
	/* [한국어] 슬롯 할당 실패. */
	if (!ptr)
		/* [한국어] 이 함수는 실패를 **NULL** 로 알린다 -- 같은 파일의 remap_cfg_resource 가
		 * IOMEM_ERR_PTR 을 쓰는 것과 다르다. */
		return NULL;

	addr = pci_remap_cfgspace(offset, size);
	/* [한국어] 매핑 성공. */
	if (addr) {
		*ptr = addr;
		devres_add(dev, ptr);
	} else
		/* [한국어] 실패하면 슬롯만 버린다. */
		devres_free(ptr);

	return addr;
/* [한국어] 성공이든 실패든 addr 을 그대로 돌려주므로, 호출자가 NULL 로 판별한다. */
}
EXPORT_SYMBOL(devm_pci_remap_cfgspace);

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
/* [한국어]
 * devm_pci_remap_cfg_resource - 자원을 예약하고 설정공간 속성으로 매핑한다
 *
 * @dev: 이 매핑을 소유할 장치.
 * @res: 매핑할 MEM 자원.
 * @return: 매핑된 주소, 또는 IOMEM_ERR_PTR 로 감싼 오류.
 *
 * 예약과 매핑을 한 번에 처리하는 편의 함수다. DWC 를 비롯한 여러 컨트롤러
 * 드라이버가 "config" reg 를 이 함수로 잡는다.
 *
 * 반환값이 NULL 이 아니라 **IOMEM_ERR_PTR** 인 점이 devm_pci_remap_cfgspace
 * 와 다르다. 호출자는 IS_ERR 로 판별해야 한다.
 *
 * 이름을 만드는 부분이 눈에 띈다: 자원에 이름이 있으면 "장치명 자원명",
 * 없으면 "장치명" 으로 짓는다. /proc/iomem 에서 어느 장치의 어느 창인지
 * 알아보게 하려는 것이고, devm_ 계열로 잡아 이 함수가 실패해도 새지 않는다.
 *
 * 매핑에 실패하면 방금 잡은 메모리 영역 예약을 **명시적으로** 되돌린다 --
 * devm 이라 언젠가는 풀리지만, 실패한 프로브가 자원을 붙들고 있으면
 * 재시도가 -EBUSY 로 실패하기 때문이다.
 *
 * BUG_ON(!dev) 는 상류 그대로다. dev 가 NULL 이면 devm 계열을 쓸 수 없어
 * 어차피 진행할 수 없다는 뜻이다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_host_get_resources 등 컨트롤러 드라이버 → [이 함수]
 *     → devm_request_mem_region → devm_pci_remap_cfgspace
 */
void __iomem *devm_pci_remap_cfg_resource(struct device *dev,
					  struct resource *res)
{
	resource_size_t size;
	const char *name;
	/* [한국어] 반환할 매핑 주소 또는 오류 포인터. */
	void __iomem *dest_ptr;
/* [한국어] 아래에서 인자를 검증한다. */

	BUG_ON(!dev);
/* [한국어] dev 가 NULL 이면 devm 계열을 쓸 수 없어 어차피 진행할 수 없다. */

	if (!res || resource_type(res) != IORESOURCE_MEM) {
		/* [한국어] 자원이 없거나 MEM 이 아니다 -- 설정공간은 반드시 메모리 자원이어야 한다. */
		dev_err(dev, "invalid resource\n");
		/* [한국어] IOMEM_ERR_PTR 로 감싸 돌려준다. 호출자는 IS_ERR 로 판별한다. */
		return IOMEM_ERR_PTR(-EINVAL);
	/* [한국어] 이제 예약 이름을 만든다. */
	}

	size = resource_size(res);
/* [한국어] 자원에 이름이 있으면 장치명과 붙여 쓴다. */

	if (res->name)
		/* [한국어] /proc/iomem 에서 어느 장치의 어느 창인지 알아보게 하려는 것이다. */
		name = devm_kasprintf(dev, GFP_KERNEL, "%s %s", dev_name(dev),
				      /* [한국어] devm_ 으로 잡아 이 함수가 실패해도 문자열이 새지 않는다. */
				      res->name);
	else
		name = devm_kstrdup(dev, dev_name(dev), GFP_KERNEL);
	/* [한국어] 이름 할당 실패. */
	if (!name)
		/* [한국어] 아직 예약 전이라 되돌릴 것이 없다. */
		return IOMEM_ERR_PTR(-ENOMEM);
/* [한국어] 이제 메모리 영역을 예약한다. */

	if (!devm_request_mem_region(dev, res->start, size, name)) {
		/* [한국어] 다른 드라이버가 이미 그 범위를 잡고 있다. */
		dev_err(dev, "can't request region for resource %pR\n", res);
		/* [한국어] 자원 충돌은 -EBUSY 로 알린다. */
		return IOMEM_ERR_PTR(-EBUSY);
	/* [한국어] 예약에 성공했으므로 매핑으로 넘어간다. */
	}

	dest_ptr = devm_pci_remap_cfgspace(dev, res->start, size);
	/* [한국어] 매핑 실패. */
	if (!dest_ptr) {
		/* [한국어] 어느 자원에서 실패했는지 %pR 로 찍는다. */
		dev_err(dev, "ioremap failed for resource %pR\n", res);
		/* [한국어] 방금 잡은 예약을 **명시적으로** 되돌린다. devm 이라 언젠가는 풀리지만,
		 * 실패한 프로브가 자원을 붙들고 있으면 재시도가 -EBUSY 로 실패한다. */
		devm_release_mem_region(dev, res->start, size);
		/* [한국어] 오류를 포인터로 감싼다. */
		dest_ptr = IOMEM_ERR_PTR(-ENOMEM);
	/* [한국어] 성공/실패 모두 dest_ptr 로 흘러간다. */
	}

	return dest_ptr;
/* [한국어] 한 변수로 두 경우를 돌려주는 관용구다. */
}
EXPORT_SYMBOL(devm_pci_remap_cfg_resource);

/*
 * __pcim_clear_mwi:
 *   pcim_set_mwi() 등록 시 드라이버 detach에 호출되어
 *   NVMe 장치의 Memory Write Invalidate(MWI)를 해제한다.
 */
/* [한국어]
 * __pcim_clear_mwi - MWI 를 되돌리는 devm 액션 콜백
 *
 * @pdev_raw: pci_dev 포인터.
 * @return: 없음.
 *
 * devm_add_action 은 인자를 void* 하나만 받으므로, 형만 맞춰 주는 얇은
 * 껍데기다.
 *
 * 실행 컨텍스트: 드라이버 분리 시의 프로세스 문맥.
 *
 * 호출 체인:
 *   devres 코어 → [이 함수] → pci_clear_mwi
 */
static void __pcim_clear_mwi(void *pdev_raw)
{
	struct pci_dev *pdev = pdev_raw;

	pci_clear_mwi(pdev);
}

/**
 * pcim_set_mwi - a device-managed pci_set_mwi()
 * @pdev: the PCI device for which MWI is enabled
 *
 * Managed pci_set_mwi().
 *
 * RETURNS: An appropriate -ERRNO error value on error, or zero for success.
 */
/* [한국어]
 * pcim_set_mwi - MWI(Memory Write and Invalidate)를 켜고 자동으로 꺼지게 한다
 *
 * @pdev: 대상 장치.
 * @return: 0 성공, 음수는 액션 등록 또는 pci_set_mwi 의 실패값.
 *
 * 순서가 다른 devres 함수들과 **반대**다. 보통은 자원을 잡고 나서 등록하는데,
 * 여기서는 **액션을 먼저 등록하고 그 다음 MWI 를 켠다.** 그리고 켜기에
 * 실패하면 devm_remove_action 으로 등록을 취소한다.
 *
 * 이렇게 하는 이유: devm_add_action 은 할당을 동반해 실패할 수 있는데,
 * MWI 를 먼저 켠 뒤 등록에 실패하면 켜 놓은 MWI 를 되돌릴 devres 항목이
 * 없어진다. 반대 순서면 그런 상태가 생기지 않는다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 probe → [이 함수] → devm_add_action → pci_set_mwi
 */
int pcim_set_mwi(struct pci_dev *pdev)
{
	int ret;

	ret = devm_add_action(&pdev->dev, __pcim_clear_mwi, pdev);
	/* [한국어] 액션 등록 실패(할당 실패). */
	if (ret != 0)
		/* [한국어] 아직 MWI 를 켜지 않았으므로 되돌릴 것이 없다. */
		return ret;

	ret = pci_set_mwi(pdev);
	/* [한국어] MWI 켜기 실패. */
	if (ret != 0)
		/* [한국어] 등록해 둔 액션을 취소한다. 그러지 않으면 켜지지도 않은 MWI 를 끄려는
		 * 콜백이 분리 시에 돈다. */
		devm_remove_action(&pdev->dev, __pcim_clear_mwi, pdev);
/* [한국어] 성공이든 실패든 pci_set_mwi 의 결과를 그대로 돌려준다. */

	return ret;
}
EXPORT_SYMBOL(pcim_set_mwi);

/* [한국어]
 * mask_contains_bar - BAR 비트마스크에 그 BAR 가 포함됐는지 본다
 *
 * @mask: BAR 비트마스크. 비트 n 이 BAR n 을 뜻한다.
 * @bar: 확인할 BAR 번호.
 * @return: 포함됐으면 참(0 이 아닌 값).
 *
 * pcim_iomap_regions() 가 마스크를 훑을 때만 쓰는 한 줄 헬퍼다. 반환형이
 * bool 이라 `mask & BIT(bar)` 의 0 이 아닌 값이 true 로 변환된다.
 *
 * 실행 컨텍스트: 어디서나.
 *
 * 호출 체인:
 *   pcim_iomap_regions → [이 함수]
 */
static inline bool mask_contains_bar(int mask, int bar)
{
	return mask & BIT(bar);
}

/*
 * pcim_intx_restore:
 *   드라이버 detach 시 원래 INTx 상태로 복원한다.
 *   NVMe가 MSI-X 대신 INTx를 쓰는 경우에 해당한다.
 */
/* [한국어]
 * pcim_intx_restore - INTx 설정을 원래대로 되돌리는 devres 콜백
 *
 * @dev: 장치.
 * @data: 저장해 둔 pcim_intx_devres(원래 상태).
 * @return: 없음.
 *
 * pcim_intx() 가 처음 불렸을 때 기록해 둔 값을 그대로 다시 쓴다. 드라이버가
 * INTx 를 껐다가 분리되어도, 다음 드라이버나 펌웨어가 원래 상태를 보게 된다.
 *
 * 실행 컨텍스트: 드라이버 분리 시의 프로세스 문맥.
 *
 * 호출 체인:
 *   devres 코어 → [이 함수] → pci_intx
 */
static void pcim_intx_restore(struct device *dev, void *data)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pcim_intx_devres *res = data;
/* [한국어] pci_intx 는 '켬' 을 1 로 받으므로, 아래에서 레지스터 의미를 뒤집어 저장한다. */

	pci_intx(pdev, res->orig_intx);
/* [한국어] 저장한 값을 그대로 pci_intx 에 넘기면 원래 상태로 돌아간다. */
}

/*
 * save_orig_intx:
 *   pcim_intx() 최초 호출 시 PCI command 레지스터를 읽어
 *   NVMe 장치의 원래 INTx 상태를 보관한다.
 */
/* [한국어]
 * save_orig_intx - 현재 INTx 활성 여부를 서술자에 기록한다
 *
 * @pdev: 대상 장치.
 * @res: 기록할 서술자.
 * @return: 없음.
 *
 * PCI_COMMAND 의 INTX_DISABLE 비트를 읽어 **부정**해 담는다 -- 레지스터는
 * '끔' 을 1 로 표현하는데 pci_intx() 는 '켬' 을 1 로 받으므로, 그대로
 * 되돌려 쓸 수 있게 의미를 뒤집어 저장하는 것이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pcim_intx → [이 함수] → pci_read_config_word
 */
static void save_orig_intx(struct pci_dev *pdev, struct pcim_intx_devres *res)
{
	u16 pci_command;

	pci_read_config_word(pdev, PCI_COMMAND, &pci_command);
	/* [한국어] INTX_DISABLE 은 '끔' 이 1 이므로 **부정해서** 저장한다 -- 그래야
	 * pci_intx(pdev, orig_intx) 로 그대로 되돌릴 수 있다. */
	res->orig_intx = !(pci_command & PCI_COMMAND_INTX_DISABLE);
/* [한국어] 이 한 줄의 부정이 복원 전체를 성립시킨다. */
}

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
/* [한국어]
 * pcim_intx - INTx 를 켜거나 끄되, 처음 상태를 기억해 자동 복원되게 한다
 *
 * @pdev: 대상 장치.
 * @enable: 1 이면 켜고 0 이면 끈다.
 * @return: 0 성공, -ENOMEM 할당 실패.
 *
 * 이 함수의 요령은 **devres 항목을 하나만 만든다**는 것이다. 먼저
 * devres_find 로 이미 있는지 보고, 없을 때만 만들어 그 시점의 상태를
 * 기록한다. 그래서 드라이버가 pcim_intx 를 여러 번 불러도 '가장 처음의
 * 상태' 가 보존된다 -- 두 번째 호출이 첫 호출의 결과를 원본으로 기록해
 * 버리면 복원이 무의미해진다.
 *
 * 기록과 등록이 끝난 뒤에야 실제 pci_intx 를 부른다. 순서가 반대면 이미
 * 바뀐 상태를 원본으로 기록하게 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. devres_alloc 이 GFP_KERNEL 로 잠들 수 있다.
 *
 * 호출 체인:
 *   드라이버 → [이 함수] → devres_find → save_orig_intx → pci_intx
 */
int pcim_intx(struct pci_dev *pdev, int enable)
{
	struct pcim_intx_devres *res;
	struct device *dev = &pdev->dev;
/* [한국어] 이미 등록된 항목이 있는지 먼저 본다. */

	/*
	 * pcim_intx() must only restore the INTx value that existed before the
	 * driver was loaded, i.e., before it called pcim_intx() for the
	 * first time.
	 */
	res = devres_find(dev, pcim_intx_restore, NULL, NULL);
	if (!res) {
		/* [한국어] 없을 때만 새로 만든다 -- 두 번째 호출이 첫 호출의 결과를 원본으로
		 * 기록해 버리면 복원이 무의미해지기 때문이다. */
		res = devres_alloc(pcim_intx_restore, sizeof(*res), GFP_KERNEL);
		/* [한국어] 할당 실패. */
		if (!res)
			/* [한국어] 아직 pci_intx 를 부르기 전이라 상태가 바뀌지 않았다. */
			return -ENOMEM;

		save_orig_intx(pdev, res);
		/* [한국어] 기록이 끝난 뒤에 등록한다. */
		devres_add(dev, res);
	/* [한국어] 이미 있었으면 이 블록 전체를 건너뛴다 -- 원본은 처음 것이 유지된다. */
	}

	pci_intx(pdev, enable);
/* [한국어] 등록이 끝난 뒤에야 실제로 바꾼다. 순서가 반대면 이미 바뀐 상태를
 * 원본으로 기록하게 된다. */

	return 0;
}
EXPORT_SYMBOL_GPL(pcim_intx);

/*
 * pcim_disable_device:
 *   pcim_enable_device()가 등록한 해제 콜백. 드라이버 detach 시
 *   NVMe 장치가 고정되지 않았다면 pci_disable_device()를 호출한다.
 */
/* [한국어]
 * pcim_disable_device - 장치를 비활성화하는 devm 액션 콜백
 *
 * @pdev_raw: pci_dev 포인터.
 * @return: 없음.
 *
 * **pinned 검사**가 이 콜백의 핵심이다. pcim_pin_device() 로 고정된 장치는
 * 드라이버가 분리돼도 비활성화하지 않는다 -- 예컨대 콘솔로 쓰이는 VGA
 * 장치처럼, 드라이버가 사라져도 계속 동작해야 하는 경우가 있다.
 *
 * is_managed 를 false 로 되돌리는 것은 pinned 여부와 무관하다. 이 플래그는
 * '이 장치가 devres 로 관리되고 있다' 는 표시라, 관리가 끝나면 지워야 한다.
 *
 * 실행 컨텍스트: 드라이버 분리 시의 프로세스 문맥.
 *
 * 호출 체인:
 *   devres 코어 → [이 함수] → pci_disable_device
 */
static void pcim_disable_device(void *pdev_raw)
{
	struct pci_dev *pdev = pdev_raw;

	if (!pdev->pinned)
		/* [한국어] pinned 가 아닐 때만 끈다. 콘솔로 쓰이는 장치처럼 드라이버가 사라져도
		 * 계속 동작해야 하는 경우를 위한 예외다. */
		pci_disable_device(pdev);

	pdev->is_managed = false;
/* [한국어] is_managed 는 pinned 와 무관하게 항상 내린다 -- 관리가 끝났다는 표시다. */
}

/**
 * pcim_enable_device - Managed pci_enable_device()
 * @pdev: PCI device to be initialized
 *
 * Returns: 0 on success, negative error code on failure.
 *
 * Managed pci_enable_device(). Device will automatically be disabled on
 * driver detach.
 */
/* [한국어]
 * pcim_enable_device - 장치를 켜고 드라이버 분리 시 자동으로 꺼지게 한다
 *
 * @pdev: 대상 장치.
 * @return: 0 성공, 음수는 액션 등록 또는 pci_enable_device 의 실패값.
 *
 * pcim_set_mwi 와 같은 '액션 먼저, 작업 나중' 순서다. 켜기에 실패하면
 * 등록을 취소해, 켜지지도 않은 장치를 끄려는 콜백이 남지 않게 한다.
 *
 * is_managed 를 **성공한 뒤에만** 세우는 점에 유의. 이 플래그를 보고
 * 다른 코드가 devres 관리 여부를 판단하므로, 실패한 장치에 세우면 안 된다.
 *
 * 이 함수를 쓰면 pci_disable_device 를 직접 부를 필요가 없어지고, 그래서
 * 오류 경로가 크게 단순해진다 -- devres 계열의 존재 이유 그 자체다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 probe → [이 함수] → devm_add_action → pci_enable_device
 */
int pcim_enable_device(struct pci_dev *pdev)
{
	int ret;

	ret = devm_add_action(&pdev->dev, pcim_disable_device, pdev);
	/* [한국어] 액션 등록 실패. */
	if (ret != 0)
		/* [한국어] 아직 장치를 켜지 않았다. */
		return ret;

	/*
	 * We prefer removing the action in case of an error over
	 * devm_add_action_or_reset() because the latter could theoretically be
	 * disturbed by users having pinned the device too soon.
	 */
	ret = pci_enable_device(pdev);
	if (ret != 0) {
		/* [한국어] 켜기에 실패했으므로 등록을 취소한다. */
		devm_remove_action(&pdev->dev, pcim_disable_device, pdev);
		/* [한국어] 실패값을 그대로 올린다. */
		return ret;
	}

	pdev->is_managed = true;
/* [한국어] **성공한 뒤에만** is_managed 를 세운다. 실패한 장치에 세우면 다른 코드가
 * devres 관리 중이라고 오해한다. */

	return ret;
}
EXPORT_SYMBOL(pcim_enable_device);

/**
 * pcim_pin_device - Pin managed PCI device
 * @pdev: PCI device to pin
 *
 * Pin managed PCI device @pdev. Pinned device won't be disabled on driver
 * detach. @pdev must have been enabled with pcim_enable_device().
 */
/* [한국어]
 * pcim_pin_device - 드라이버가 분리돼도 장치를 끄지 않도록 표시한다
 *
 * @pdev: 대상 장치.
 * @return: 없음.
 *
 * 플래그 하나를 세우는 것이 전부지만, 그 효과는 pcim_disable_device 가
 * 이 플래그를 보고 pci_disable_device 를 건너뛰는 것이다.
 *
 * 쓰이는 상황: 드라이버가 사라진 뒤에도 장치가 계속 동작해야 하는 경우
 * (부트 콘솔로 쓰이는 프레임버퍼 등). 그런 장치를 끄면 화면이 멈춘다.
 *
 * 되돌리는 API 가 없다는 점에 유의 -- 한 번 고정하면 그 장치는 이 커널
 * 부팅 동안 계속 고정 상태다.
 *
 * 실행 컨텍스트: 어디서나. 플래그 대입뿐이다.
 *
 * 호출 체인:
 *   드라이버 → [이 함수]
 */
void pcim_pin_device(struct pci_dev *pdev)
{
	pdev->pinned = true;
}
EXPORT_SYMBOL(pcim_pin_device);

/*
 * pcim_iomap_release:
 *   레거시 iomap 테이블용 no-op 해제 콜백.
 *   실제 매핑 정리는 매핑 등록 시 사용된 콜백에서 수행된다.
 */
/* [한국어]
 * pcim_iomap_release - 구형 iomap 테이블의 devres 해제 콜백 (지금은 빈 함수)
 *
 * @gendev: 장치. 쓰지 않는다.
 * @res: 테이블 서술자. 쓰지 않는다.
 * @return: 없음.
 *
 * 본문이 비어 있는 것이 의도다. 상류 주석이 설명하듯, 실제 매핑 해제는
 * 각 매핑이 자기 pcim_addr_devres 항목으로 따로 관리하므로 이 테이블은
 * **조회용 사본**에 지나지 않는다. 여기서 다시 해제하면 이중 해제가 된다.
 *
 * 그럼에도 함수가 남아 있는 이유: devres 는 **콜백 함수 포인터를 자원의
 * 신원**으로 쓴다. pcim_iomap_table() 이 devres_find 로 테이블을 찾으려면
 * 고유한 콜백이 하나 있어야 한다. 즉 이 빈 함수는 '테이블 자원' 이라는
 * 이름표 역할을 한다.
 *
 * 실행 컨텍스트: 드라이버 분리 시의 프로세스 문맥. 아무 일도 하지 않는다.
 *
 * 호출 체인:
 *   devres 코어 → [이 함수] (본문 없음)
 */
static void pcim_iomap_release(struct device *gendev, void *res)
{
	/*
	 * Do nothing. This is legacy code.
	 *
	 * Cleanup of the mappings is now done directly through the callbacks
	 * registered when creating them.
	 */
}

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
/* [한국어]
 * pcim_iomap_table - BAR 번호로 매핑 주소를 찾는 구형 조회 테이블을 얻는다
 *
 * @pdev: 대상 장치.
 * @return: PCI_NUM_RESOURCES 칸짜리 배열, 또는 NULL.
 *
 * pcim_iomap()/pcim_iomap_regions() 를 쓰던 옛 드라이버가 매핑 주소를
 * 되찾는 통로다. 새 코드는 pcim_iomap_region() 이 돌려주는 주소를 직접
 * 보관하는 편이 낫다 -- 이 테이블은 BAR 하나당 한 칸이라 범위 매핑
 * (pcim_iomap_range)을 표현할 수 없기 때문이다.
 *
 * devres_get() 을 쓰는 것이 요령이다. 그 함수는 '같은 자원이 이미 있으면
 * 그것을 돌려주고 새로 만든 것은 버린다' 는 의미라, 두 스레드가 동시에
 * 불러도 테이블이 하나만 남는다. 앞의 devres_find 는 흔한 경우(이미 있음)
 * 에서 할당을 피하려는 빠른 경로다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): devres_get 의 반환값을 NULL
 * 검사하지 않고 dr->table 로 역참조한다. devres_get 은 방금 넘긴 new_dr
 * 을 돌려주거나 기존 항목을 돌려주므로 NULL 이 되지 않는다는 전제다.
 *
 * 실행 컨텍스트: 프로세스 문맥. GFP_KERNEL 할당이 있어 잠들 수 있다.
 *
 * 호출 체인:
 *   드라이버 / pcim_add_mapping_to_legacy_table → [이 함수]
 *     → devres_find → devres_alloc_node → devres_get
 */
void __iomem * const *pcim_iomap_table(struct pci_dev *pdev)
{
	struct pcim_iomap_devres *dr, *new_dr;

	dr = devres_find(&pdev->dev, pcim_iomap_release, NULL, NULL);
	/* [한국어] 이미 테이블이 있으면 그대로 돌려준다 -- 흔한 경우의 빠른 경로다. */
	if (dr)
		/* [한국어] 새 할당을 피한다. */
		return dr->table;
/* [한국어] 없으면 새로 만든다. */

	new_dr = devres_alloc_node(pcim_iomap_release, sizeof(*new_dr), GFP_KERNEL,
				   /* [한국어] 장치가 붙은 NUMA 노드에 할당한다. */
				   dev_to_node(&pdev->dev));
	if (!new_dr)
		/* [한국어] 할당 실패. 호출자가 -ENOMEM 으로 처리한다. */
		return NULL;
	dr = devres_get(&pdev->dev, new_dr, NULL, NULL);
	/* [한국어] devres_get 은 '같은 자원이 이미 있으면 그것을 돌려주고 새로 만든 것은
	 * 버린다' 는 의미라, 두 스레드가 동시에 불러도 테이블이 하나만 남는다.
	 * 코드 관찰: 그 반환값을 NULL 검사하지 않고 역참조한다 -- 방금 넘긴
	 * new_dr 이나 기존 항목을 돌려주므로 NULL 이 되지 않는다는 전제다. */
	return dr->table;
/* [한국어] 이 테이블은 조회용 사본일 뿐, 실제 해제는 각 매핑의 devres 항목이 한다. */
}
EXPORT_SYMBOL(pcim_iomap_table);

/*
 * Fill the legacy mapping-table, so that drivers using the old API can
 * still get a BAR's mapping address through pcim_iomap_table().
 */
/* [한국어]
 * pcim_add_mapping_to_legacy_table - 구형 테이블의 해당 BAR 칸에 매핑을 기록한다
 *
 * @pdev: 대상 장치.
 * @mapping: 기록할 매핑 주소.
 * @bar: BAR 번호.
 * @return: 0 성공, -EINVAL 잘못된 BAR, -ENOMEM 테이블 확보 실패.
 *
 * BAR 번호 유효성을 먼저 확인한다 -- 배열 인덱스로 그대로 쓰이므로
 * 범위를 벗어나면 다른 메모리를 덮어쓴다.
 *
 * 이미 값이 있는 칸을 덮어쓸 수 있다는 점에 유의. 같은 BAR 를 두 번
 * 매핑하면 나중 것이 앞의 것을 가린다 -- 구형 API 의 한계이고, 그래서
 * 새 코드는 반환된 주소를 직접 보관하는 편이 낫다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pcim_iomap / pcim_iomap_regions → [이 함수] → pcim_iomap_table
 */
static int pcim_add_mapping_to_legacy_table(struct pci_dev *pdev,
					    void __iomem *mapping, int bar)
{
	void __iomem **legacy_iomap_table;

	if (!pci_bar_index_is_valid(bar))
		/* [한국어] 배열 인덱스로 그대로 쓰이므로 범위를 벗어나면 다른 메모리를 덮어쓴다. */
		return -EINVAL;

	legacy_iomap_table = (void __iomem **)pcim_iomap_table(pdev);
	/* [한국어] 테이블 확보 실패. */
	if (!legacy_iomap_table)
		/* [한국어] 메모리 부족이므로 -ENOMEM 으로 알린다. */
		return -ENOMEM;

	legacy_iomap_table[bar] = mapping;
/* [한국어] 이미 값이 있는 칸을 덮어쓸 수 있다 -- 같은 BAR 를 두 번 매핑하면 나중
 * 것이 앞의 것을 가린다. 구형 API 의 한계다. */

	return 0;
}

/*
 * Remove a mapping. The table only contains whole-BAR mappings, so this will
 * never interfere with ranged mappings.
 */
/* [한국어]
 * pcim_remove_mapping_from_legacy_table - 매핑 주소로 찾아 그 칸을 비운다
 *
 * @pdev: 대상 장치.
 * @addr: 지울 매핑 주소.
 * @return: 없음.
 *
 * BAR 번호를 모르고 주소만 아는 경우에 쓴다(pcim_iounmap). 그래서 배열을
 * 훑으며 주소가 같은 칸을 찾는다.
 *
 * 순회 상한이 **PCI_STD_NUM_BARS** 인 점에 유의. 테이블 자체는
 * PCI_NUM_RESOURCES 칸이라 그보다 크지만, 표준 BAR 범위 밖(ROM, 브리지
 * 윈도 등)은 이 API 로 매핑하지 않으므로 훑을 이유가 없다.
 *
 * 첫 일치에서 곧바로 반환한다 -- 같은 주소가 두 칸에 있을 수 없다는 전제다.
 * 테이블이 없으면(NULL) 조용히 돌아간다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pcim_iounmap → [이 함수] → pcim_iomap_table
 */
static void pcim_remove_mapping_from_legacy_table(struct pci_dev *pdev,
						  void __iomem *addr)
{
	int bar;
	void __iomem **legacy_iomap_table;
/* [한국어] 테이블이 없으면 지울 것도 없다. */

	legacy_iomap_table = (void __iomem **)pcim_iomap_table(pdev);
	/* [한국어] 조용히 돌아간다 -- 반환형이 void 라 알릴 통로가 없다. */
	if (!legacy_iomap_table)
		/* [한국어] 이 경우 호출자(pcim_iounmap)는 이미 devres 해제를 마친 뒤다. */
		return;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		/* [한국어] 주소가 같은 칸을 찾는다. BAR 번호를 모르고 주소만 알 때 쓰는 경로다. */
		if (legacy_iomap_table[bar] == addr) {
			/* [한국어] 찾았으면 비운다. */
			legacy_iomap_table[bar] = NULL;
			/* [한국어] 같은 주소가 두 칸에 있을 수 없다는 전제로 곧바로 반환한다. */
			return;
		}
	}
}

/*
 * The same as pcim_remove_mapping_from_legacy_table(), but identifies the
 * mapping by its BAR index.
 */
/* [한국어]
 * pcim_remove_bar_from_legacy_table - BAR 번호로 그 칸을 비운다
 *
 * @pdev: 대상 장치.
 * @bar: 비울 BAR 번호.
 * @return: 없음.
 *
 * 위 함수와 달리 BAR 번호를 알 때 쓴다 -- 탐색 없이 한 칸을 지운다.
 * pcim_iomap_regions 의 실패 되감기가 이 형태를 쓴다.
 *
 * 유효성 검사와 테이블 NULL 검사를 모두 통과해야 지운다. 반환형이 void 라
 * 실패를 알릴 통로가 없으므로 조용히 넘어간다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pcim_iomap_regions(실패 경로) → [이 함수] → pcim_iomap_table
 */
static void pcim_remove_bar_from_legacy_table(struct pci_dev *pdev, int bar)
{
	void __iomem **legacy_iomap_table;

	if (!pci_bar_index_is_valid(bar))
		/* [한국어] BAR 번호가 유효하지 않으면 배열을 건드리지 않는다. */
		return;

	legacy_iomap_table = (void __iomem **)pcim_iomap_table(pdev);
	/* [한국어] 테이블이 없으면 지울 것도 없다. */
	if (!legacy_iomap_table)
		/* [한국어] 조용히 돌아간다. */
		return;

	legacy_iomap_table[bar] = NULL;
/* [한국어] 위 함수와 달리 탐색 없이 한 칸을 지운다 -- BAR 번호를 알 때 쓴다. */
}

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
/* [한국어]
 * pcim_iomap - BAR 를 매핑하고 자동 해제되게 등록한다 (예약은 하지 않는다)
 *
 * @pdev: 대상 장치.
 * @bar: BAR 번호.
 * @maxlen: 매핑할 최대 길이. 0 이면 BAR 전체.
 * @return: 매핑 주소, 또는 NULL.
 *
 * **자원 예약(request_region)을 하지 않는다**는 점이 pcim_iomap_region 과의
 * 결정적 차이다. 다른 드라이버가 같은 BAR 를 동시에 매핑해도 막지 못하므로,
 * 새 코드는 예약까지 하는 pcim_iomap_region 을 쓰는 편이 낫다.
 *
 * 구형 조회 테이블에도 기록해, 드라이버가 pcim_iomap_table()[bar] 로
 * 되찾을 수 있게 한다.
 *
 * 되감기 라벨 두 개가 정확히 대칭이다: 테이블 기록에 실패하면 매핑을 풀고,
 * 매핑에 실패하면 서술자만 버린다. **아직 devres_add 전이므로**
 * pcim_addr_devres_free 로 그냥 버리면 된다.
 *
 * 실패를 NULL 로 알리는 점에 유의 -- 같은 파일의 pcim_iomap_region 은
 * IOMEM_ERR_PTR 을 쓴다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 → [이 함수] → pcim_addr_devres_alloc → pci_iomap
 *     → pcim_add_mapping_to_legacy_table → devres_add
 */
void __iomem *pcim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen)
{
	void __iomem *mapping;
	struct pcim_addr_devres *res;
/* [한국어] 먼저 BAR 번호를 검증한다. */

	if (!pci_bar_index_is_valid(bar))
		/* [한국어] 이 함수는 실패를 **NULL** 로 알린다. */
		return NULL;

	res = pcim_addr_devres_alloc(pdev);
	/* [한국어] 서술자 할당 실패. */
	if (!res)
		/* [한국어] 같은 이유로 NULL 이다. */
		return NULL;
	res->type = PCIM_ADDR_DEVRES_TYPE_MAPPING;
/* [한국어] 예약 없이 매핑만 하므로 MAPPING 타입이다. */

	mapping = pci_iomap(pdev, bar, maxlen);
	/* [한국어] 매핑 실패. */
	if (!mapping)
		/* [한국어] 서술자만 버리면 된다 -- 아직 등록 전이다. */
		goto err_iomap;
	res->baseaddr = mapping;
/* [한국어] 매핑 주소를 서술자에 담는다. 나중에 해제 콜백이 이 값으로 iounmap 한다. */

	if (pcim_add_mapping_to_legacy_table(pdev, mapping, bar) != 0)
		/* [한국어] 구형 테이블 기록 실패 -- 방금 만든 매핑을 되돌려야 한다. */
		goto err_table;

	devres_add(&pdev->dev, res);
	/* [한국어] 성공. 등록이 끝났으므로 이제 devres 가 수명을 관리한다. */
	return mapping;
/* [한국어] 아래는 되감기 경로다. */

err_table:
	pci_iounmap(pdev, mapping);
err_iomap:
	pcim_addr_devres_free(res);
	return NULL;
}
EXPORT_SYMBOL(pcim_iomap);

/**
 * pcim_iounmap - Managed pci_iounmap()
 * @pdev: PCI device to iounmap for
 * @addr: Address to unmap
 *
 * Managed pci_iounmap(). @addr must have been mapped using a pcim_* mapping
 * function.
 */
/* [한국어]
 * pcim_iounmap - pcim_iomap 으로 만든 매핑을 명시적으로 푼다
 *
 * @pdev: 대상 장치.
 * @addr: 풀 매핑 주소.
 * @return: 없음.
 *
 * devres 는 보통 드라이버가 분리될 때 한꺼번에 풀리지만, 드라이버가
 * 그보다 일찍 풀고 싶을 때가 있다. 그 통로다.
 *
 * 방법이 특이하다: **스택에 검색 키를 만들어** devres_release 에 넘긴다.
 * clear 로 비우고 type 과 baseaddr 만 채운 뒤, 매칭 콜백
 * (pcim_addr_resources_match)이 그 둘을 보고 실제 항목을 찾는다.
 *
 * devres_release 는 찾아서 해제하면 0, 못 찾으면 0 이 아닌 값을 준다.
 * 못 찾으면 구형 테이블도 건드리지 않고 그대로 돌아간다 -- 그 주소가
 * 이 API 로 만든 매핑이 아니라는 뜻이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 → [이 함수] → devres_release
 *     → pcim_remove_mapping_from_legacy_table
 */
void pcim_iounmap(struct pci_dev *pdev, void __iomem *addr)
{
	struct pcim_addr_devres res_searched;

	pcim_addr_devres_clear(&res_searched);
	res_searched.type = PCIM_ADDR_DEVRES_TYPE_MAPPING;
	/* [한국어] 매칭 기준이 될 주소를 검색 키에 담는다. */
	res_searched.baseaddr = addr;
/* [한국어] 이제 devres 에서 그 항목을 찾아 해제한다. */

	if (devres_release(&pdev->dev, pcim_addr_resource_release,
			/* [한국어] 0 이 아니면 못 찾았다는 뜻 -- 그 주소가 이 API 로 만든 매핑이 아니다. */
			pcim_addr_resources_match, &res_searched) != 0) {
		/* Doesn't exist. User passed nonsense. */
		return;
	}

	pcim_remove_mapping_from_legacy_table(pdev, addr);
/* [한국어] 찾아서 해제했을 때만 구형 테이블도 정리한다. */
}
EXPORT_SYMBOL(pcim_iounmap);

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
/* [한국어]
 * pcim_iomap_region - BAR 를 예약하고 매핑한다 (권장되는 새 API)
 *
 * @pdev: 대상 장치.
 * @bar: BAR 번호.
 * @name: /proc/iomem 에 나타날 예약 이름.
 * @return: 매핑 주소, 또는 IOMEM_ERR_PTR 로 감싼 오류.
 *
 * pcim_iomap 과 달리 **예약까지 한다.** 그래서 다른 드라이버가 같은 BAR 를
 * 잡으려 하면 -EBUSY 로 막힌다. 자원 종류도 REGION_MAPPING 이라, 해제
 * 콜백이 iounmap 과 release_region 을 짝으로 되돌린다.
 *
 * 구형 조회 테이블에는 **기록하지 않는다** -- 반환된 주소를 호출자가
 * 직접 보관하는 새 방식을 전제하기 때문이다. 그래서
 * pcim_iomap_regions() 는 이 함수를 부른 뒤 테이블 기록을 따로 한다.
 *
 * 되감기가 두 라벨로 갈린다: 매핑 실패면 예약을 풀고, 예약 실패면 서술자만
 * 버린다. 두 경로 모두 devres_add 전이라 이중 해제가 생기지 않는다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 / pcim_iomap_regions → [이 함수] → pci_request_region
 *     → pci_iomap → devres_add
 */
void __iomem *pcim_iomap_region(struct pci_dev *pdev, int bar,
			       const char *name)
{
	int ret;
	struct pcim_addr_devres *res;
/* [한국어] BAR 번호를 먼저 검증한다. */

	if (!pci_bar_index_is_valid(bar))
		/* [한국어] 이 함수는 실패를 **IOMEM_ERR_PTR** 로 알린다 -- pcim_iomap 의 NULL 과
		 * 다르다. */
		return IOMEM_ERR_PTR(-EINVAL);
/* [한국어] 서술자를 잡는다. */

	res = pcim_addr_devres_alloc(pdev);
	/* [한국어] 할당 실패. */
	if (!res)
		/* [한국어] 오류를 포인터로 감싼다. */
		return IOMEM_ERR_PTR(-ENOMEM);

	res->type = PCIM_ADDR_DEVRES_TYPE_REGION_MAPPING;
	/* [한국어] 예약과 매핑을 함께 하므로 BAR 번호가 필요하다. */
	res->bar = bar;
/* [한국어] 먼저 예약한다 -- 다른 드라이버가 같은 BAR 를 잡으려 하면 여기서 막힌다. */

	ret = pci_request_region(pdev, bar, name);
	/* [한국어] 예약 실패(-EBUSY 등). */
	if (ret != 0)
		/* [한국어] 서술자만 버린다. */
		goto err_region;

	res->baseaddr = pci_iomap(pdev, bar, 0);
	/* [한국어] 매핑 실패. */
	if (!res->baseaddr) {
		/* [한국어] pci_iomap 은 NULL 로 실패를 알리므로 오류 코드를 직접 정한다. */
		ret = -EINVAL;
		goto err_iomap;
	}

	devres_add(&pdev->dev, res);
	/* [한국어] 성공. 서술자에 담아 둔 주소를 그대로 돌려준다. */
	return res->baseaddr;
/* [한국어] 아래는 되감기 경로다. */

err_iomap:
	pci_release_region(pdev, bar);
err_region:
	pcim_addr_devres_free(res);

	return IOMEM_ERR_PTR(ret);
/* [한국어] 두 라벨이 잡은 순서의 역순으로 되돌린다. */
}
EXPORT_SYMBOL(pcim_iomap_region);

/**
 * pcim_iounmap_region - Unmap and release a PCI BAR
 * @pdev: PCI device to operate on
 * @bar: Index of BAR to unmap and release
 *
 * Unmap a BAR and release its region manually. Only pass BARs that were
 * previously mapped by pcim_iomap_region().
 */
/* [한국어]
 * pcim_iounmap_region - pcim_iomap_region 으로 잡은 BAR 를 풀고 예약도 되돌린다
 *
 * @pdev: 대상 장치.
 * @bar: 풀 BAR 번호.
 * @return: 없음.
 *
 * pcim_iounmap 과 같은 '스택 검색 키' 방식이되, 타입이 REGION_MAPPING 이라
 * 매칭 기준이 baseaddr 이 아니라 **bar 번호**다. 그래서 매핑 주소를
 * 몰라도 BAR 번호만으로 풀 수 있다.
 *
 * devres_release 의 반환값을 보지 않는 점이 pcim_iounmap 과 다르다 --
 * 구형 테이블을 정리할 일이 없어 성패에 따라 달라질 동작이 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 / pcim_iomap_regions(실패 경로) → [이 함수] → devres_release
 */
void pcim_iounmap_region(struct pci_dev *pdev, int bar)
{
	struct pcim_addr_devres res_searched;

	pcim_addr_devres_clear(&res_searched);
	res_searched.type = PCIM_ADDR_DEVRES_TYPE_REGION_MAPPING;
	/* [한국어] 매칭 기준이 될 BAR 번호를 검색 키에 담는다. */
	res_searched.bar = bar;
/* [한국어] 이제 해제한다. */

	devres_release(&pdev->dev, pcim_addr_resource_release,
			/* [한국어] 반환값을 보지 않는 점이 pcim_iounmap 과 다르다 -- 구형 테이블을 정리할
			 * 일이 없어 성패에 따라 달라질 동작이 없다. */
			pcim_addr_resources_match, &res_searched);
}
EXPORT_SYMBOL(pcim_iounmap_region);

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
/* [한국어]
 * pcim_iomap_regions - 마스크로 지정한 여러 BAR 를 한꺼번에 예약·매핑한다
 *
 * @pdev: 대상 장치.
 * @mask: BAR 비트마스크. 비트 n 이 BAR n 을 뜻한다.
 * @name: 예약 이름.
 * @return: 0 성공, 음수는 첫 실패의 코드.
 *
 * 드라이버가 여러 BAR 를 쓰는 흔한 경우를 한 줄로 줄여 주는 함수다.
 * BAR 마다 pcim_iomap_region 을 부르고 구형 테이블에도 기록한다 --
 * 그 기록 때문에 드라이버가 pcim_iomap_table()[bar] 로 주소를 되찾을 수 있다.
 *
 * 순회 상한이 **DEVICE_COUNT_RESOURCE** 인 점에 유의. 표준 BAR 여섯 개를
 * 넘어 ROM 과 브리지 윈도 칸까지 훑는다. 반면 같은 파일의
 * pcim_release_all_regions() 는 PCI_STD_NUM_BARS 까지만 돈다 -- 두 상한이
 * 다르다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): 되감기가 `while (--bar >= 0)` 라
 * **실패한 bar 자신은 되돌리지 않는다.** pcim_iomap_region 이 실패한
 * 경우에는 그 함수가 스스로 정리했으므로 맞지만,
 * pcim_add_mapping_to_legacy_table 이 실패한 경우에는 그 BAR 의 예약·매핑이
 * 이미 devres 에 등록된 채로 남는다. 드라이버 분리 시 devres 가 결국
 * 풀어 주므로 누수는 아니지만, 다른 BAR 들과 처리가 달라진다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 → [이 함수] → pcim_iomap_region
 *     → pcim_add_mapping_to_legacy_table
 */
int pcim_iomap_regions(struct pci_dev *pdev, int mask, const char *name)
{
	int ret;
	int bar;
	/* [한국어] 각 BAR 의 매핑 결과. */
	void __iomem *mapping;
/* [한국어] 마스크에 있는 BAR 만 처리한다. */

	for (bar = 0; bar < DEVICE_COUNT_RESOURCE; bar++) {
		/* [한국어] 비트가 없으면 건너뛴다. */
		if (!mask_contains_bar(mask, bar))
			/* [한국어] 다음 BAR 로. */
			continue;

		mapping = pcim_iomap_region(pdev, bar, name);
		/* [한국어] 예약·매핑 실패. */
		if (IS_ERR(mapping)) {
			/* [한국어] 오류 포인터에서 코드를 꺼낸다. */
			ret = PTR_ERR(mapping);
			/* [한국어] 되감기 경로로. */
			goto err;
		}
		ret = pcim_add_mapping_to_legacy_table(pdev, mapping, bar);
		/* [한국어] 구형 테이블 기록 실패. */
		if (ret != 0)
			/* [한국어] 같은 되감기 경로로. **코드 관찰: 이 경우 방금 성공한 이 BAR 의 예약·매핑은
			 * 되돌려지지 않는다** -- 아래 루프가 bar-1 부터 시작하기 때문이다. */
			goto err;
	}

	return 0;

err:
	while (--bar >= 0) {
		pcim_iounmap_region(pdev, bar);
		/* [한국어] 구형 테이블도 함께 비운다. */
		pcim_remove_bar_from_legacy_table(pdev, bar);
	/* [한국어] bar 가 0 이 되면 끝난다. */
	}

	return ret;
}
EXPORT_SYMBOL(pcim_iomap_regions);

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
/* [한국어]
 * pcim_request_region - BAR 를 예약만 하고 자동 해제되게 등록한다 (매핑 없음)
 *
 * @pdev: 대상 장치.
 * @bar: BAR 번호.
 * @name: 예약 이름.
 * @return: 0 성공, -EINVAL 잘못된 BAR, -ENOMEM, 또는 예약 실패값.
 *
 * IO 포트처럼 매핑 없이 접근하는 자원이나, 매핑은 나중에 따로 하려는
 * 경우에 쓴다. 자원 종류가 REGION 이라 해제 콜백이 release_region 만 한다.
 *
 * 실패 처리가 다른 함수들보다 단순하다 -- 잡은 것이 예약 하나뿐이라
 * 서술자만 버리면 된다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 / pcim_request_all_regions → [이 함수] → pci_request_region
 *     → devres_add
 */
int pcim_request_region(struct pci_dev *pdev, int bar, const char *name)
{
	int ret;
	struct pcim_addr_devres *res;
/* [한국어] BAR 번호를 검증한다. */

	if (!pci_bar_index_is_valid(bar))
		/* [한국어] 이 함수는 실패를 **정수 오류 코드**로 알린다 -- 매핑 주소를 돌려주지
		 * 않으므로 포인터를 쓸 이유가 없다. */
		return -EINVAL;

	res = pcim_addr_devres_alloc(pdev);
	/* [한국어] 서술자 할당 실패. */
	if (!res)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;
	res->type = PCIM_ADDR_DEVRES_TYPE_REGION;
	/* [한국어] 예약만 하므로 BAR 번호만 필요하다. */
	res->bar = bar;
/* [한국어] 이제 실제로 예약한다. */

	ret = pci_request_region(pdev, bar, name);
	/* [한국어] 예약 실패. */
	if (ret != 0) {
		/* [한국어] 등록 전이라 서술자만 버린다. */
		pcim_addr_devres_free(res);
		return ret;
	}

	devres_add(&pdev->dev, res);
	/* [한국어] 성공. 이제 devres 가 해제를 맡는다. */
	return 0;
}
EXPORT_SYMBOL(pcim_request_region);

/**
 * pcim_release_region - Release a PCI BAR
 * @pdev: PCI device to operate on
 * @bar: Index of BAR to release
 *
 * Release a region manually that was previously requested by
 * pcim_request_region().
 */
/* [한국어]
 * pcim_release_region - pcim_request_region 으로 잡은 예약 하나를 되돌린다
 *
 * @pdev: 대상 장치.
 * @bar: 풀 BAR 번호.
 * @return: 없음.
 *
 * 같은 '스택 검색 키' 방식이고, 타입이 REGION 이라 bar 번호로 매칭된다.
 *
 * **static 이다** -- 외부에 공개되지 않고 pcim_release_all_regions() 만
 * 부른다. 개별 예약을 푸는 공개 API 는 이 파일에 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pcim_release_all_regions → [이 함수] → devres_release
 */
static void pcim_release_region(struct pci_dev *pdev, int bar)
{
	struct pcim_addr_devres res_searched;

	pcim_addr_devres_clear(&res_searched);
	res_searched.type = PCIM_ADDR_DEVRES_TYPE_REGION;
	/* [한국어] 매칭 기준이 될 BAR 번호. */
	res_searched.bar = bar;
/* [한국어] 해제한다. */

	devres_release(&pdev->dev, pcim_addr_resource_release,
			/* [한국어] 잡히지 않은 BAR 에 대해 불려도 devres_release 가 못 찾고 아무 일도
			 * 하지 않으므로 안전하다 -- 그 성질이 아래 release_all 을 한 줄로 만든다. */
			pcim_addr_resources_match, &res_searched);
}


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
/* [한국어]
 * pcim_release_all_regions - 표준 BAR 여섯 개의 예약을 모두 되돌린다
 *
 * @pdev: 대상 장치.
 * @return: 없음.
 *
 * 잡히지 않은 BAR 에 대해서도 그냥 부른다 -- devres_release 가 못 찾으면
 * 아무 일도 하지 않으므로 안전하다. 그래서 어느 BAR 까지 성공했는지
 * 추적할 필요가 없고, pcim_request_all_regions 의 되감기가 한 줄로 끝난다.
 *
 * 순회 상한이 PCI_STD_NUM_BARS 인 점에 유의 -- pcim_iomap_regions 의
 * DEVICE_COUNT_RESOURCE 와 다르다. 이 함수의 짝인
 * pcim_request_all_regions 도 같은 상한을 쓰므로 그 쌍 안에서는 일관된다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pcim_request_all_regions(실패 경로) → [이 함수] → pcim_release_region
 */
static void pcim_release_all_regions(struct pci_dev *pdev)
{
	int bar;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++)
		/* [한국어] 잡히지 않은 BAR 도 그냥 부른다. 어디까지 성공했는지 셀 필요가 없어진다. */
		pcim_release_region(pdev, bar);
/* [한국어] 순회 상한이 PCI_STD_NUM_BARS 인 점에 유의 -- pcim_iomap_regions 는
 * DEVICE_COUNT_RESOURCE 까지 돈다. */
}

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
/* [한국어]
 * pcim_request_all_regions - 표준 BAR 여섯 개를 모두 예약한다
 *
 * @pdev: 대상 장치.
 * @name: 예약 이름.
 * @return: 0 성공, 음수는 첫 실패의 코드.
 *
 * BAR 를 가리지 않고 여섯 개를 모두 잡는다 -- 정의되지 않은 BAR 에 대해
 * pci_request_region 이 성공으로 처리하므로 성립한다.
 *
 * 되감기가 한 줄인 이유는 위 pcim_release_all_regions 가 잡히지 않은 것도
 * 안전하게 넘기기 때문이다. 어디까지 성공했는지 세지 않아도 된다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 → [이 함수] → pcim_request_region
 *     → pcim_release_all_regions(실패 시)
 */
int pcim_request_all_regions(struct pci_dev *pdev, const char *name)
{
	int ret;
	int bar;
/* [한국어] 표준 BAR 여섯 개를 모두 잡는다. */

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		/* [한국어] 정의되지 않은 BAR 에 대해 pci_request_region 이 성공으로 처리하므로
		 * 가리지 않고 부를 수 있다. */
		ret = pcim_request_region(pdev, bar, name);
		/* [한국어] 하나라도 실패하면 전부 되돌린다. */
		if (ret != 0)
			/* [한국어] 되감기가 한 줄인 것은 release_all 이 잡히지 않은 것도 안전하게 넘기기
			 * 때문이다. */
			goto err;
	}

	return 0;

err:
	pcim_release_all_regions(pdev);

	return ret;
}
EXPORT_SYMBOL(pcim_request_all_regions);

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
/* [한국어]
 * pcim_iomap_range - BAR 안의 일부 범위만 매핑하고 자동 해제되게 등록한다
 *
 * @pdev: 대상 장치.
 * @bar: BAR 번호.
 * @offset: BAR 시작으로부터의 오프셋.
 * @len: 매핑할 길이.
 * @return: 매핑 주소, 또는 IOMEM_ERR_PTR 로 감싼 오류.
 *
 * BAR 가 아주 큰데 일부만 필요할 때 쓴다. 자원 종류는 MAPPING 이라
 * 예약은 하지 않는다.
 *
 * **구형 조회 테이블에 기록하지 않는다.** 상류 주석이 그 이유를 못 박아
 * 두었다 -- 그 테이블은 BAR 하나당 한 칸이라 '전체 BAR' 만 표현할 수 있고,
 * 부분 범위를 담을 자리가 없다. 그래서 이 API 로 만든 매핑은
 * pcim_iomap_table() 로 되찾을 수 없고, 호출자가 주소를 직접 보관해야 한다.
 *
 * 해제는 pcim_iounmap(주소) 으로 한다 -- 매칭 기준이 baseaddr 이므로
 * 같은 BAR 의 여러 범위 매핑도 각각 구별해 풀 수 있다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 → [이 함수] → pci_iomap_range → devres_add
 */
void __iomem *pcim_iomap_range(struct pci_dev *pdev, int bar,
		unsigned long offset, unsigned long len)
{
	void __iomem *mapping;
	struct pcim_addr_devres *res;
/* [한국어] BAR 번호를 검증한다. */

	if (!pci_bar_index_is_valid(bar))
		/* [한국어] 이 함수도 IOMEM_ERR_PTR 로 실패를 알린다. */
		return IOMEM_ERR_PTR(-EINVAL);
/* [한국어] 서술자를 잡는다. */

	res = pcim_addr_devres_alloc(pdev);
	/* [한국어] 할당 실패. */
	if (!res)
		/* [한국어] 오류를 포인터로 감싼다. */
		return IOMEM_ERR_PTR(-ENOMEM);

	mapping = pci_iomap_range(pdev, bar, offset, len);
	/* [한국어] 범위 매핑 실패. */
	if (!mapping) {
		/* [한국어] 등록 전이라 서술자만 버린다. */
		pcim_addr_devres_free(res);
		return IOMEM_ERR_PTR(-EINVAL);
	/* [한국어] pci_iomap_range 는 NULL 로 실패를 알리므로 코드를 직접 정한다. */
	}

	res->type = PCIM_ADDR_DEVRES_TYPE_MAPPING;
	/* [한국어] 해제 콜백이 이 주소로 iounmap 한다. 매칭 기준도 이 주소다 -- 그래서
	 * 같은 BAR 의 여러 범위 매핑을 각각 구별해 풀 수 있다. */
	res->baseaddr = mapping;
/* [한국어] 구형 테이블에는 기록하지 않는다(바로 아래 상류 주석 참조). */

	/*
	 * Ranged mappings don't get added to the legacy-table, since the table
	 * only ever keeps track of whole BARs.
	 */

	devres_add(&pdev->dev, res);
	/* [한국어] 성공. 호출자가 이 주소를 직접 보관해야 한다 -- 테이블로 되찾을 수 없다. */
	return mapping;
/* [한국어] 이 파일의 마지막 공개 함수다. */
}
EXPORT_SYMBOL(pcim_iomap_range);
