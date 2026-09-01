// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Message Signaled Interrupt (MSI)
 *
 * Copyright (C) 2003-2004 Intel
 * Copyright (C) Tom Long Nguyen (tom.l.nguyen@intel.com)
 * Copyright (C) 2016 Christoph Hellwig.
 */

/*
 * [한국어 설명] MSI/MSI-X 하드웨어를 실제로 조작하는 구현부 (msi.c)
 *
 * === 파일의 역할 ===
 * api.c 가 결정한 것을 실제 하드웨어에 반영하는 곳이다. config space 의 MSI /
 * MSI-X capability 구조를 파싱하고, MSI-X 테이블이 놓인 BAR 영역을 ioremap 하고,
 * 벡터마다 msi_desc 를 만들어 커널 MSI 계층에 등록하고, 마지막으로 Message
 * Control 레지스터의 Enable 비트를 켠다. 해제는 그 역순이다.
 *
 * 이 파일을 읽으려면 MSI 와 MSI-X 가 하드웨어적으로 얼마나 다른지를 알아야 한다.
 *
 *   MSI  - 모든 정보가 config space 안의 capability 구조에 들어 있다.
 *          Message Address(32 또는 64비트) 하나와 Message Data 하나가 전부다.
 *          벡터가 여러 개여도 주소는 하나를 공유하고, 데이터의 하위 몇 비트만
 *          벡터 번호로 바뀐다. 그래서 벡터 수가 2의 거듭제곱이어야 하고,
 *          번호도 연속이어야 하며, 최대 32개다. 벡터별 마스킹은 선택 기능이라
 *          없는 장치도 많다.
 *
 *   MSI-X - capability 구조에는 "테이블이 어느 BAR 의 어느 오프셋에 있는가"
 *          (Table Offset/BIR)만 적혀 있고, 실제 벡터 정보는 그 BAR 안의
 *          MSI-X Table 에 있다. 항목 하나가 16바이트(주소 하위 4 + 상위 4 +
 *          데이터 4 + 벡터 제어 4)이고, 최대 2048개다. 항목마다 주소가 따로라
 *          벡터별로 다른 CPU 를 지정할 수 있고, Vector Control 의 0번 비트로
 *          벡터별 마스킹이 항상 가능하다.
 *
 * 이 차이 때문에 이 파일의 코드가 msi_ 계열과 msix_ 계열 두 갈래로 크게 나뉜다.
 * MSI-X 쪽이 BAR 매핑(msix_map_region)이라는 추가 단계를 갖는 것이 핵심 차이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * api.c (드라이버가 부르는 얼굴)
 *   -> [이 파일] __pci_enable_msi_range() / __pci_enable_msix_range()
 *      -> msi_capability_init() / msix_capability_init()
 *         -> msix_map_region()          : MSI-X 테이블이 있는 BAR 영역을 ioremap
 *         -> msix_setup_msi_descs()     : 벡터마다 msi_desc 를 만들어 등록
 *         -> pci_msi_setup_msi_irqs()   : irqdomain.c 로 넘겨 실제 IRQ 번호 확보
 *            -> 아키텍처 IRQ 컨트롤러가 주소/데이터를 정해 돌려준다
 *         -> msix_update_entries()      : 정해진 주소/데이터를 MSI-X 테이블에 기록
 *         -> pci_msix_clear_and_set_ctrl(): Enable 비트를 켜고 Function Mask 를 푼다
 *
 * 실행 컨텍스트: 대부분 프로세스 컨텍스트(뮤텍스와 메모리 할당이 있다).
 * 예외적으로 pci_msi_mask_irq()/pci_msi_unmask_irq() 와 __pci_write_msi_msg()
 * 계열은 인터럽트 처리 도중 IRQ 코어가 부를 수 있어 잠들지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: api.c (공개 API), pcidev_msi.c (장치 발견 시 초기 비활성화).
 * 아래쪽: irqdomain.c (irq_domain 에 벡터 등록), ../pci.h 의 config 접근 함수,
 *   그리고 커널 공통 MSI 계층(kernel/irq/msi.c)의 msi_desc 저장소.
 * 공유 상태:
 *   - struct pci_dev 의 msi_cap / msix_cap (capability 오프셋),
 *     msi_enabled / msix_enabled (현재 상태), msi_irq_groups (sysfs).
 *   - struct msi_desc 의 pci.mask_base (MSI-X 테이블의 가상 주소),
 *     pci.msi_mask / pci.msix_ctrl (마스크 레지스터의 소프트웨어 캐시).
 *     캐시를 두는 이유는 MSI-X 테이블 읽기가 느리고, 마스킹이 인터럽트
 *     처리 경로에서 일어나기 때문이다.
 * 데이터 흐름: capability 레지스터 -> msi_desc -> irqdomain -> 아키텍처가 정한
 *   message address/data -> 다시 하드웨어(MSI capability 또는 MSI-X 테이블).
 *   즉 정보가 하드웨어에서 올라갔다가 다시 내려온다.
 *
 * === 주요 함수/구조체 요약 ===
 * msi_capability_init()      : MSI capability 를 읽어 msi_desc 를 만들고 Enable.
 *                              실제 필드 계산은 msi_setup_msi_desc() 가 하며,
 *                              거기서 nvec 을 __roundup_pow_of_two() 로 2의 거듭제곱
 *                              까지 "올린" 뒤 그 log2 를 Message Control 의 QSIZE 에
 *                              넣는다. 내림이 아니라 올림이다.
 * msix_capability_init()     : MSI-X 테이블을 매핑하고 벡터들을 등록한 뒤 Enable.
 *                              등록 전에 모든 벡터를 마스크해 두는 순서가 중요하다.
 * msix_map_region()          : Table Offset/BIR 을 해석해 테이블이 있는 물리 주소를
 *                              구하고 ioremap 한다. MSI-X 만의 단계다.
 * msix_mask_all()            : 테이블의 모든 항목을 마스크. 켜자마자 엉뚱한
 *                              인터럽트가 오는 것을 막는다.
 * pci_msi_update_mask()      : MSI 의 Mask Bits 레지스터를 갱신(캐시 포함).
 * pci_msix_clear_and_set_ctrl(): Message Control 레지스터의 비트를 read-modify-write.
 * __pci_write_msi_msg()      : 결정된 주소/데이터를 하드웨어에 기록. MSI 면 config
 *                              space 에, MSI-X 면 테이블 항목에 쓴다.
 * __pci_restore_msi_state() / __pci_restore_msix_state() : 전원 복귀 후 복원.
 * pci_intx_for_msi()         : MSI 를 켤 때 INTx 를 끄고, 끌 때 다시 켠다.
 *                              두 방식이 동시에 활성이면 인터럽트가 두 번 온다.
 * pci_msi_enable (전역 bool) : "pci=nomsi" 부팅 인자로 MSI 전체를 끄는 스위치.
 *
 * === NVMe 드라이버가 실제로 쓰는 것 (drivers/nvme/ 전수 확인) ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다. 전부 api.c 를 거친다.
 * 아래 라인 번호는 모두 원본 스냅숏(1f0e418bb6) 기준이며, grep 으로 확인했다.
 *
 *   nvme_pci_enable()   [drivers/nvme/host/pci.c:3182]
 *     -> pci_alloc_irq_vectors(pdev, 1, 1, flags)      [api.c:367]
 *        -> [이 파일] __pci_enable_msix_range(...)
 *           -> msix_capability_init()  : 테이블 매핑 + 벡터 1개 등록 + Enable
 *      admin 큐 하나만 돌리기 위한 최소 구성이다.
 *
 *   nvme_setup_io_queues()
 *     -> pci_free_irq_vectors(pdev)   [pci.c:2995 -> api.c:514]
 *        -> [이 파일] pci_msix_shutdown() -> pci_free_msi_irqs()
 *     -> nvme_setup_irqs()
 *        -> pci_alloc_irq_vectors_affinity(pdev, 1, irq_queues, flags, &affd)
 *           [drivers/nvme/host/pci.c:2893 -> api.c:387]
 *        -> [이 파일] __pci_enable_msix_range(...) 를 다시
 *      벡터 수를 늘리려면 껐다 켜는 수밖에 없다. MSI-X Enable 상태에서
 *      테이블 크기를 바꿀 방법이 하드웨어에 없기 때문이다.
 *
 *   전원 복귀(D3cold -> D0) 시
 *     pci_restore_state() -> pci_restore_msi_state()   [api.c:529]
 *       -> [이 파일] __pci_restore_msix_state()
 *      MSI-X 테이블은 장치 안의 메모리라 전원이 끊기면 내용이 날아간다.
 *      그래서 커널이 msi_desc 에 캐시해 둔 주소/데이터를 다시 써 넣는다.
 *      NVMe 드라이버 코드에는 이 호출이 없다 - PCI 코어가 대신 한다.
 *
 * NVMe 가 요청하는 벡터 수: "admin 1개 + (I/O 큐 수 - 폴링 큐 수)".
 * 폴링 큐를 빼는 이유는 그 큐가 인터럽트 대신 blk-mq 의 poll 경로로
 * 완료를 확인하기 때문이다. NVME_QUIRK_SINGLE_VECTOR 가 걸린 Apple
 * 컨트롤러는 모든 큐가 0번 벡터를 공유해야 해서 1개만 요청한다.
 *
 * 핸들러 등록/해제는 이 파일이 아니라 drivers/pci/irq.c 가 담당한다.
 * NVMe 는 queue_request_irq() 에서 pci_request_irq(pdev, nvmeq->cq_vector, ...) 로
 * 벡터에 nvme_irq / nvme_irq_check 핸들러를 걸고(pci.c:2144, 2147),
 * pci_free_irq() 로 뗀다(pci.c:2024, 2961, 2989). 벡터 번호를 IRQ 번호로
 * 바꾸는 pci_irq_vector() 도 pci.c:1632 에서 쓴다 - 정의는 api.c 다.
 * 즉 "벡터를 몇 개 확보하느냐"(이 파일)와 "그 벡터에 어떤 함수를 거느냐"
 * (irq.c)가 서로 다른 계층이다.
 *
 * (기존 주석에 P2PDMA/CMB/SR-IOV/AER/ATS/ReBAR/DPC 를 나열한 문단이 있었으나
 *  이 파일의 코드와 아무 관계가 없어 삭제했다.)
 *
 * 기존 커널 영어 주석은 그대로 보존하고, 한국어 설명을 위/옆에 추가한다.
 */
/* [한국어] bitfield.h — FIELD_GET/FIELD_PREP 매크로. Message Control 안의 QSIZE 같은
 * 비트 필드를 마스크와 시프트를 직접 쓰지 않고 꺼내고 넣기 위해 필요하다. */
#include <linux/bitfield.h>
/* [한국어] err.h — 이 파일이 돌려주는 -EINVAL/-ENOSPC/-ENODEV 등 errno 상수. */
#include <linux/err.h>
/* [한국어] export.h — EXPORT_SYMBOL / EXPORT_SYMBOL_GPL. 이 파일의 pci_msi_mask_irq,
 * pci_write_msi_msg, pci_msi_vec_count, msi_desc_to_pci_dev 가 모듈에 공개된다. */
#include <linux/export.h>
/* [한국어] irq.h — struct irq_data 와 irq_desc. irq_chip 콜백(mask/unmask)이 받는 인자와
 * pci_msix_write_tph_tag 가 잠그는 irq_desc->lock 이 여기서 온다. */
#include <linux/irq.h>
/* [한국어] irqdomain.h — MSI 벡터를 등록할 irq_domain 자료구조. 실제 도메인 조작은
 * 같은 디렉터리의 irqdomain.c 가 하지만, 이 파일도 타입을 참조한다. */
#include <linux/irqdomain.h>

/* [한국어] ../pci.h — PCI 코어 내부 헤더. pci_intx(), pcibios_alloc_irq(),
 * pci_dev_is_disconnected() 같은 코어 헬퍼 선언이 들어 있다. */
#include "../pci.h"
/* [한국어] msi.h — msi/ 디렉터리 전용 사설 헤더. 마스킹 인라인 함수와
 * pci_msix_desc_addr(), 그리고 파일 간 호출용 선언이 모두 여기 있다. */
#include "msi.h"

/* [한국어] MSI 전역 스위치. true 가 기본값이고, pci_no_msi() 만이 이 값을 내린다.
 * 설정자: pci_no_msi() ( "pci=nomsi", ACPI FADT 의 NO_MSI, quirk ).
 * 읽는 자: pci_msi_supported(), pci_msi_shutdown(), pci_msix_shutdown(),
 * 그리고 api.c 의 pci_msi_enabled().
 * 값 범위: true(허용) / false(전역 금지).
 * 동기화: 없다. 부팅 초기에 한 번만 내려가는 값이라는 전제다. */
bool pci_msi_enable = true;

/* [한국어]
 * pci_msi_supported - 이 장치에 MSI/MSI-X 를 켜도 되는지 최상위 판정
 *
 * @dev:  대상 PCI 장치.
 * @nvec: 요청 벡터 수.
 * @return: 1 이면 켜도 된다, 0 이면 안 된다.
 *
 * 네 가지를 차례로 본다.
 *   1) 전역 스위치 pci_msi_enable. "pci=nomsi" 나 ACPI FADT 의 NO_MSI 비트,
 *      또는 quirk 가 pci_no_msi() 로 내려 두었을 수 있다.
 *   2) dev 가 NULL 이거나 dev->no_msi. 장치별 quirk 로 막아 둔 경우다.
 *   3) nvec < 1. 상류 주석대로 이유가 둘인데, 그 자체로 말이 안 되고
 *      아래 목록 조작 코드가 nvec >= 1 을 전제한다.
 *   4) 자기 자신부터 루트까지 모든 버스의 PCI_BUS_FLAGS_NO_MSI.
 *
 * 4번이 핵심이다. MSI 는 결국 메모리 쓰기라, 중간의 어느 브리지 하나라도
 * 그 쓰기를 아래에서 위로 넘겨 주지 못하면 인터럽트가 사라진다. 그래서
 * 그런 브리지는 자기 secondary 버스에 NO_MSI 를 세워 두고, 여기서 위로
 * 거슬러 올라가며 확인한다. 상류 주석이 그 플래그가 세워지는 두 경로
 * (아키텍처 호스트 드라이버 직접 설정, 브리지 quirk)와 간접 경로(호스트
 * 브리지 드라이버가 msi_domain 을 알렸는데 probe 때 도메인을 못 찾은 경우)를
 * 적어 두었다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 하드웨어를 만지지 않고 플래그만 본다.
 *
 * 호출 체인:
 *   __pci_enable_msi_range() / __pci_enable_msix_range() -> [이 함수]
 */
/**
 * pci_msi_supported - check whether MSI may be enabled on a device
 * @dev: pointer to the pci_dev data structure of MSI device function
 * @nvec: how many MSIs have been requested?
 *
 * Look at global flags, the device itself, and its parent buses
 * to determine if MSI/-X are supported for the device. If MSI/-X is
 * supported return 1, else return 0.
 **/
static int pci_msi_supported(struct pci_dev *dev, int nvec)
{
	/* [한국어] 부모 방향으로 버스를 거슬러 올라갈 때 쓸 커서. 아래 루프에서만 쓴다. */
	struct pci_bus *bus;

	/* MSI must be globally enabled and supported by the device */
	if (!pci_msi_enable)
		return 0;

	/* [한국어] 장치가 없거나 quirk 로 no_msi 가 붙은 장치는 즉시 거절.
	 * dev 가 NULL 일 수 있는 이유는 이 함수가 방어적으로 쓰이기 때문이다. */
	if (!dev || dev->no_msi)
		/* [한국어] 거절 — 반환값 0 이 "MSI 를 쓸 수 없다" 를 뜻한다. */
		return 0;

	/*
	 * You can't ask to have 0 or less MSIs configured.
	 *  a) it's stupid ..
	 *  b) the list manipulation code assumes nvec >= 1.
	 */
	/* [한국어] 벡터를 0개 이하로 달라는 요청. 위 상류 주석이 이유 두 가지를 적어 두었다. */
	if (nvec < 1)
		/* [한국어] 거절. */
		return 0;

	/*
	 * Any bridge which does NOT route MSI transactions from its
	 * secondary bus to its primary bus must set NO_MSI flag on
	 * the secondary pci_bus.
	 *
	 * The NO_MSI flag can either be set directly by:
	 * - arch-specific PCI host bus controller drivers (deprecated)
	 * - quirks for specific PCI bridges
	 *
	 * or indirectly by platform-specific PCI host bridge drivers by
	 * advertising the 'msi_domain' property, which results in
	 * the NO_MSI flag when no MSI domain is found for this bridge
	 * at probe time.
	 */
	/* [한국어] 자기 버스에서 시작해 parent 를 따라 루트까지 올라간다.
	 * MSI 는 메모리 쓰기라 경로 위의 브리지가 하나라도 막으면 전달되지 않는다. */
	for (bus = dev->bus; bus; bus = bus->parent)
		/* [한국어] PCI_BUS_FLAGS_NO_MSI — 이 버스에서 위로 MSI 쓰기를 넘기지 못한다는 표시. */
		if (bus->bus_flags & PCI_BUS_FLAGS_NO_MSI)
			/* [한국어] 경로 중 한 곳이라도 막혀 있으면 거절. */
			return 0;

	/* [한국어] 모든 검사를 통과 — MSI/MSI-X 를 켜도 좋다. */
	return 1;
}

/* [한국어]
 * pcim_msi_release - devres 가 장치를 정리할 때 벡터를 되돌려 주는 콜백
 *
 * @pcidev: devm_add_action() 에 넘겼던 void 포인터. 실제로는 struct pci_dev.
 * @return: 없음.
 *
 * 드라이버가 관리 모드(pcim_enable_device)에서 벡터를 잡아 두고 명시적으로
 * 반납하지 않은 채 사라져도, devres 가 이 콜백을 불러 정리한다.
 *
 * is_msi_managed 를 먼저 내리는 순서가 중요하다. 이 플래그가 서 있으면
 * pcim_setup_msi_release() 가 "이미 걸려 있다" 고 판단하는데, 해제 도중
 * 그 상태로 두면 다시 잡을 때 액션을 걸지 못한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치 해제 경로).
 *
 * 호출 체인:
 *   device 해제 -> devres 언와인드 -> [이 함수] -> pci_free_irq_vectors() [api.c]
 */
static void pcim_msi_release(void *pcidev)
{
	/* [한국어] devres 가 넘겨 준 void 포인터를 원래 타입으로 되돌린다.
	 * devm_add_action 의 콜백 서명이 void 인자라 이 복원이 필요하다. */
	struct pci_dev *dev = pcidev;

	/* [한국어] 먼저 관리 표시를 내린다. 아래 해제 도중 다시 잡히더라도
	 * pcim_setup_msi_release() 가 액션을 새로 걸 수 있게 하기 위해서다. */
	dev->is_msi_managed = false;
	/* [한국어] api.c 의 공개 해제 API 를 부른다. 그 안에서 이 파일의
	 * pci_msi_shutdown()/pci_msix_shutdown() 과 pci_free_msi_irqs() 가 실행된다. */
	pci_free_irq_vectors(dev);
}

/* [한국어]
 * pcim_setup_msi_release - 관리 장치라면 MSI 전용 devm 해제 액션을 등록한다
 *
 * @dev: 대상 PCI 장치.
 * @return: 0(등록했거나 등록할 필요가 없음), 또는 devm_add_action() 의 errno.
 *
 * pci_is_managed(dev) 가 참일 때만 - 즉 드라이버가 pcim_enable_device()
 * 로 devres 관리 모드에 들어갔을 때만 - 동작한다. 이미 걸어 두었으면
 * (is_msi_managed) 다시 걸지 않는다. 중복 등록하면 해제가 두 번 불린다.
 *
 * 상류 주석이 이 동작을 대놓고 위험하다고 적어 두었다. pcim_enable_device()
 * 를 부른 것만으로 IRQ 벡터까지 자동 관리로 넘어가 버려, 드라이버가
 * 의도하지 않은 시점에 벡터가 반납될 수 있기 때문이다. 그래서 TODO 로
 * 제거 대상이 표시되어 있다. 코드는 고치지 않고 관찰만 기록한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(devm_add_action 이 할당을 한다).
 *
 * 호출 체인:
 *   pci_setup_msi_context() -> [이 함수] -> devm_add_action()
 */
/*
 * Needs to be separate from pcim_release to prevent an ordering problem
 * vs. msi_device_data_release() in the MSI core code.
 *
 * TODO: Remove the legacy side-effect of pcim_enable_device() that
 * activates automatic IRQ vector management. This design is dangerous
 * and confusing because it switches normally un-managed functions
 * into managed mode. Drivers should explicitly manage their IRQ vectors
 * without this implicit behavior.
 *
 * The current implementation uses both pdev->is_managed and
 * pdev->is_msi_managed flags, which adds unnecessary complexity.
 * This should be simplified in a future kernel version.
 */
static int pcim_setup_msi_release(struct pci_dev *dev)
{
	/* [한국어] devm_add_action() 의 반환값을 받을 곳. */
	int ret;

	/* [한국어] 관리 모드가 아니거나(pci_is_managed) 이미 액션을 걸어 두었으면 할 일이 없다.
	 * 중복 등록하면 장치 해제 때 벡터 반납이 두 번 일어난다. */
	if (!pci_is_managed(dev) || dev->is_msi_managed)
		/* [한국어] 성공으로 돌려준다 — 걸 필요가 없었을 뿐 오류가 아니다. */
		return 0;

	/* [한국어] 장치 해제 시 pcim_msi_release(dev) 를 부르도록 devres 에 등록.
	 * 이 등록 자체가 메모리를 할당하므로 실패할 수 있다. */
	ret = devm_add_action(&dev->dev, pcim_msi_release, dev);
	/* [한국어] 등록 실패(대개 -ENOMEM). */
	if (ret)
		/* [한국어] 그대로 위로 전한다 — 여기서 삼키면 해제되지 않는 벡터가 남는다. */
		return ret;

	/* [한국어] 등록에 성공했음을 표시. 다음 호출이 이 값을 보고 건너뛴다. */
	dev->is_msi_managed = true;
	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * pci_setup_msi_context - MSI 용 장치 데이터를 만들고 devm 해제를 건다
 *
 * @dev: 대상 PCI 장치.
 * @return: 0 성공, 음수 errno 실패.
 *
 * MSI 를 켜기 전 반드시 거치는 준비다. msi_setup_device_data() 가 커널
 * 공통 MSI 계층에 이 장치의 descriptor 저장소(msi_device_data)를 만들고,
 * 그다음 pcim_setup_msi_release() 가 devm 해제 액션을 건다.
 *
 * 이 순서가 상류 주석이 말하는 "Ordering vs. devres" 다. devres 는 등록의
 * 역순으로 해제하므로, 저장소를 먼저 등록해야 해제 때 pcim_msi_release()
 * (벡터 반납)가 저장소 해제보다 앞서 불린다. 반대로 하면 이미 사라진
 * 저장소를 뒤져 벡터를 반납하려 든다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   __pci_enable_msi_range() / __pci_enable_msix_range() -> [이 함수]
 *     -> msi_setup_device_data() [커널 공통] -> pcim_setup_msi_release()
 */
/*
 * Ordering vs. devres: msi device data has to be installed first so that
 * pcim_msi_release() is invoked before it on device release.
 */
static int pci_setup_msi_context(struct pci_dev *dev)
{
	/* [한국어] 커널 공통 MSI 계층에 이 장치의 descriptor 저장소를 만든다.
	 * 이것이 없으면 msi_insert_msi_desc() 가 넣을 곳이 없다. */
	int ret = msi_setup_device_data(&dev->dev);

	/* [한국어] 저장소 생성 실패. */
	if (ret)
		/* [한국어] devm 액션을 걸기 전에 빠진다 — 순서상 저장소가 먼저이기 때문이다. */
		return ret;

	/* [한국어] 저장소가 준비됐으니 이제 해제 액션을 건다.
	 * 이 순서가 상류 주석이 말하는 "Ordering vs. devres" 다. */
	return pcim_setup_msi_release(dev);
}

/* [한국어] 여기부터 MSI 와 MSI-X 가 공유하는 저수준 조작 함수들이다.
 * 마스킹(pci_msi_update_mask, pci_msi_mask_irq, pci_msi_unmask_irq)과
 * message 읽기/쓰기(__pci_read_msi_msg, __pci_write_msi_msg 및 그 아래의
 * MSI/MSI-X 전용 두 갈래)가 이 묶음에 속한다.
 *
 * 이 묶음이 위쪽의 활성화 코드와 성격이 다른 점이 하나 있다. 활성화는
 * 프로세스 컨텍스트에서 한 번 일어나지만, 여기 함수들은 인터럽트를
 * 마스크하거나 CPU 를 옮길 때마다 인터럽트 문맥에서 불린다. 그래서
 * 잠들 수 있는 락이나 메모리 할당이 하나도 없다. */
/*
 * Helper functions for mask/unmask and MSI message handling
 */

/* [한국어]
 * pci_msi_update_mask - MSI 의 Mask Bits 레지스터를 캐시와 함께 갱신한다
 *
 * @desc:  대상 descriptor.
 * @clear: 0 으로 내릴 비트(=언마스크할 벡터).
 * @set:   1 로 올릴 비트(=마스크할 벡터).
 * @return: 없음.
 *
 * MSI 의 Mask Bits 는 32비트 레지스터 하나에 최대 32개 벡터의 마스크
 * 비트가 모여 있다. 그래서 벡터 하나만 바꾸려 해도 나머지 31개 비트를
 * 보존해야 하고, 그 보존을 위해 소프트웨어 캐시 desc->pci.msi_mask 를 둔다.
 * 캐시를 고친 뒤 통째로 써 넣는다 - 하드웨어를 읽지 않는 이유는 config
 * 읽기가 느리고, 이 경로가 인터럽트 처리 중에도 불리기 때문이다.
 *
 * raw_spinlock 인 dev->msi_lock 을 irqsave 로 잡는다. 이유가 둘이다.
 * 같은 레지스터를 서로 다른 벡터의 마스킹이 동시에 건드릴 수 있어
 * read-modify-write 가 깨질 수 있고(경쟁 조건), 이 함수가 인터럽트 문맥에서
 * 불리므로 잠들 수 있는 락을 쓸 수 없다.
 *
 * can_mask 가 0 이면 조용히 돌아간다. PCI 2.3 이전 장치에는 Mask Bits
 * 레지스터가 아예 없어 쓸 곳이 없기 때문이며, 그런 장치의 마스킹은
 * 상위 IRQ 계층이 대신 처리한다.
 *
 * 실행 컨텍스트: 잠들지 않는다. 인터럽트 문맥 포함.
 *
 * 호출 체인:
 *   pci_msi_mask()/pci_msi_unmask() [msi.h] -> [이 함수] -> config 워드 쓰기
 */
void pci_msi_update_mask(struct msi_desc *desc, u32 clear, u32 set)
{
	/* [한국어] descriptor 가 매달린 device 를 PCI 장치로 되돌린다. config 접근에 필요하다. */
	struct pci_dev *dev = msi_desc_to_pci_dev(desc);
	/* [한국어] 장치별 raw spinlock. Mask Bits 레지스터의 읽고-고쳐-쓰기를 보호한다. */
	raw_spinlock_t *lock = &dev->msi_lock;
	/* [한국어] 인터럽트 상태를 저장할 곳. 이 경로가 인터럽트 문맥에서도 불리므로
	 * 단순 spin_lock 이 아니라 irqsave 형태를 써야 한다. */
	unsigned long flags;

	/* [한국어] Mask Bits 레지스터가 없는 장치(PCI 2.3 이전)면 쓸 곳이 없다. */
	if (!desc->pci.msi_attrib.can_mask)
		/* [한국어] 조용히 돌아간다. 그런 장치의 마스킹은 상위 IRQ 계층이 대신 처리한다. */
		return;

	/* [한국어] 락 획득. 서로 다른 벡터의 마스킹이 같은 레지스터를 동시에 고치는
	 * 경쟁을 막는다. irqsave 인 이유는 인터럽트 문맥과도 겨루기 때문이다. */
	raw_spin_lock_irqsave(lock, flags);
	/* [한국어] clear 에 표시된 비트를 캐시에서 내린다(= 그 벡터를 언마스크). */
	desc->pci.msi_mask &= ~clear;
	/* [한국어] set 에 표시된 비트를 캐시에서 올린다(= 그 벡터를 마스크).
	 * clear 를 먼저 적용하므로 같은 비트를 양쪽에 주면 결과는 1 이다. */
	desc->pci.msi_mask |= set;
	/* [한국어] 고친 캐시를 통째로 하드웨어에 쓴다. 하드웨어를 읽지 않는 이유는
	 * config 읽기가 느리고 이 경로가 인터럽트 처리 중에도 불리기 때문이다. */
	pci_write_config_dword(dev, desc->pci.mask_pos, desc->pci.msi_mask);
	/* [한국어] 락 해제와 인터럽트 상태 복원. */
	raw_spin_unlock_irqrestore(lock, flags);
}

/* [한국어]
 * pci_msi_mask_irq - virq 하나를 마스크하는 irq_chip 콜백
 *
 * @data: IRQ 코어가 넘기는 irq_data. 여기서 descriptor 를 얻는다.
 * @return: 없음.
 *
 * BIT(data->irq - desc->irq) 계산이 이 함수의 전부다. MSI 는 벡터들이
 * 연속된 virq 번호를 받고 descriptor 의 irq 가 그 첫 번째이므로, 빼면
 * 몇 번째 벡터인지가 나오고 그 자리 비트만 세우면 그 벡터만 막힌다.
 * MSI-X 경로에서는 항목마다 제어 워드가 따로라 이 인자가 무시된다.
 *
 * irq_chip 의 .irq_mask 로 등록되어 disable_irq() 나 인터럽트 처리 중
 * IRQ 코어가 부른다. EXPORT_SYMBOL_GPL 이라 아키텍처/컨트롤러 드라이버가
 * 자기 irq_chip 에 이 함수를 그대로 꽂아 쓴다.
 * 실행 컨텍스트: 잠들지 않는다. IRQ 코어가 이미 락을 잡은 상태다.
 *
 * 호출 체인:
 *   IRQ 코어 -> [이 함수] -> __pci_msi_mask_desc() [msi.h]
 *     -> pci_msix_mask() 또는 pci_msi_mask()
 */
/**
 * pci_msi_mask_irq - Generic IRQ chip callback to mask PCI/MSI interrupts
 * @data:	pointer to irqdata associated to that interrupt
 */
void pci_msi_mask_irq(struct irq_data *data)
{
	/* [한국어] IRQ 코어가 준 irq_data 에서 이 벡터의 descriptor 를 꺼낸다. */
	struct msi_desc *desc = irq_data_get_msi_desc(data);

	/* [한국어] data->irq 는 이 벡터의 virq, desc->irq 는 첫 벡터의 virq.
	 * 빼면 몇 번째 벡터인지가 나오고, 그 자리 비트만 세워 그 벡터만 막는다.
	 * MSI-X 경로에서는 항목마다 제어 워드가 따로라 이 인자가 무시된다. */
	__pci_msi_mask_desc(desc, BIT(data->irq - desc->irq));
}
/* [한국어] 모듈에 공개. 아키텍처와 컨트롤러 드라이버가 자기 irq_chip 의
 * .irq_mask 에 이 함수를 그대로 꽂아 쓴다. */
EXPORT_SYMBOL_GPL(pci_msi_mask_irq);

/* [한국어]
 * pci_msi_unmask_irq - virq 하나의 마스크를 푸는 irq_chip 콜백
 *
 * @data: IRQ 코어가 넘기는 irq_data. 여기서 descriptor 를 얻는다.
 * @return: 없음.
 *
 * pci_msi_mask_irq() 의 짝이다. 계산도 대칭이다 - data->irq 에서
 * desc->irq(첫 벡터)를 빼 몇 번째 벡터인지 구하고 그 비트만 푼다.
 *
 * irq_chip 의 .irq_unmask 로 등록되어 enable_irq() 나 인터럽트 처리
 * 완료 시점에 IRQ 코어가 부른다. EXPORT_SYMBOL_GPL.
 * 실행 컨텍스트: 잠들지 않는다.
 *
 * 호출 체인:
 *   IRQ 코어 -> [이 함수] -> __pci_msi_unmask_desc() [msi.h]
 *     -> pci_msix_unmask() 또는 pci_msi_unmask()
 */
/**
 * pci_msi_unmask_irq - Generic IRQ chip callback to unmask PCI/MSI interrupts
 * @data:	pointer to irqdata associated to that interrupt
 */
void pci_msi_unmask_irq(struct irq_data *data)
{
	/* [한국어] 같은 방식으로 descriptor 를 꺼낸다. */
	struct msi_desc *desc = irq_data_get_msi_desc(data);

	/* [한국어] 같은 비트 계산으로 그 벡터의 마스크만 푼다. */
	__pci_msi_unmask_desc(desc, BIT(data->irq - desc->irq));
}
/* [한국어] 모듈에 공개. irq_chip 의 .irq_unmask 자리에 쓰인다. */
EXPORT_SYMBOL_GPL(pci_msi_unmask_irq);

/* [한국어]
 * __pci_read_msi_msg - 지금 하드웨어에 설정된 주소/데이터를 읽어 온다
 *
 * @entry: 대상 벡터의 descriptor.
 * @msg:   읽은 값을 담을 곳.
 * @return: 없음.
 *
 * 쓰기 경로(__pci_write_msi_msg)의 거울이다. MSI-X 면 테이블 항목에서,
 * MSI 면 config space 의 capability 에서 읽는다.
 *
 * 시작 부분의 BUG_ON 은 D0 가 아닌 장치에서 읽으면 의미 없는 값(보통
 * 전부 1)이 돌아오는데, 그 값을 그대로 믿고 진행하면 나중에 엉뚱한 곳으로
 * 인터럽트를 보내게 되기 때문이다. 조용히 틀리느니 즉시 멈춘다는 판단이다.
 * 가상 항목은 읽을 하드웨어가 없어 WARN 후 돌아간다.
 *
 * 32비트 MSI 장치에는 주소 상위 필드가 없으므로 address_hi 를 0 으로 채운다.
 * 데이터는 하드웨어에서 16비트지만 struct msi_msg 의 data 는 32비트라
 * u16 지역 변수를 거쳐 옮긴다.
 *
 * 실행 컨텍스트: 잠들지 않는다.
 *
 * 호출 체인:
 *   IRQ 코어의 상태 조회 경로 -> [이 함수] -> readl() 또는 config 읽기
 */
void __pci_read_msi_msg(struct msi_desc *entry, struct msi_msg *msg)
{
	/* [한국어] descriptor 에서 PCI 장치를 되돌린다. */
	struct pci_dev *dev = msi_desc_to_pci_dev(entry);

	/* [한국어] D0 가 아니면 읽은 값이 의미가 없다(대개 전부 1). 그 값을 믿고 진행하면
	 * 나중에 엉뚱한 주소로 인터럽트를 보내게 되므로 여기서 즉시 멈춘다. */
	BUG_ON(dev->current_state != PCI_D0);

	/* [한국어] MSI-X 는 BAR 안의 테이블에서, MSI 는 config space 에서 읽는다. */
	if (entry->pci.msi_attrib.is_msix) {
		/* [한국어] 이 벡터의 16바이트 테이블 항목이 시작하는 가상 주소.
		 * mask_base + msi_index * 16 이다(msi.h 의 pci_msix_desc_addr). */
		void __iomem *base = pci_msix_desc_addr(entry);

		/* [한국어] 가상 항목은 대응하는 하드웨어가 없다. 읽으면 매핑 밖을 건드린다. */
		if (WARN_ON_ONCE(entry->pci.msi_attrib.is_virtual))
			/* [한국어] 경고만 남기고 돌아간다 — msg 는 채우지 않는다. */
			return;

		/* [한국어] 항목의 0번 워드: 목적지 주소 하위 32비트. */
		msg->address_lo = readl(base + PCI_MSIX_ENTRY_LOWER_ADDR);
		/* [한국어] 항목의 4번 오프셋: 목적지 주소 상위 32비트. MSI-X 는 항상 64비트다. */
		msg->address_hi = readl(base + PCI_MSIX_ENTRY_UPPER_ADDR);
		/* [한국어] 항목의 8번 오프셋: 데이터 워드. 12번은 Vector Control 이라 읽지 않는다. */
		msg->data = readl(base + PCI_MSIX_ENTRY_DATA);
	/* [한국어] 여기부터 MSI 경로. */
	} else {
		/* [한국어] MSI capability 의 config space 오프셋. 장치마다 다르다. */
		int pos = dev->msi_cap;
		/* [한국어] MSI 의 데이터 필드는 하드웨어에서 16비트라 u16 으로 받는다. */
		u16 data;

		/* [한국어] capability + PCI_MSI_ADDRESS_LO 에서 주소 하위 32비트. */
		pci_read_config_dword(dev, pos + PCI_MSI_ADDRESS_LO,
				      &msg->address_lo);
		/* [한국어] 64비트 주소를 지원하는 장치인가. 이 값이 뒤 필드들의 오프셋을 가른다. */
		if (entry->pci.msi_attrib.is_64) {
			/* [한국어] 주소 상위 32비트를 읽는다. */
			pci_read_config_dword(dev, pos + PCI_MSI_ADDRESS_HI,
					      &msg->address_hi);
			/* [한국어] 64비트 장치의 데이터 필드는 상위 주소 뒤에 있어 오프셋이 다르다. */
			pci_read_config_word(dev, pos + PCI_MSI_DATA_64, &data);
		/* [한국어] 32비트 장치 — 상위 주소 필드 자체가 없다. */
		} else {
			/* [한국어] 읽을 곳이 없으므로 0 으로 채워 준다. 호출자가 조립할 때 필요하다. */
			msg->address_hi = 0;
			/* [한국어] 상위 주소 필드가 없는 만큼 데이터 필드가 앞으로 당겨져 있다. */
			pci_read_config_word(dev, pos + PCI_MSI_DATA_32, &data);
		}
		/* [한국어] u16 으로 읽은 값을 struct msi_msg 의 32비트 data 로 옮긴다. */
		msg->data = data;
	}
}

/* [한국어]
 * pci_write_msg_msi - MSI capability 에 주소/데이터와 벡터 수를 써 넣는다
 *
 * @dev:  대상 PCI 장치.
 * @desc: 대상 descriptor. multiple 과 is_64 를 여기서 읽는다.
 * @msg:  써 넣을 주소와 데이터.
 * @return: 없음.
 *
 * MSI 는 벡터별 저장소가 없어 capability 구조 하나에 전부 적는다.
 *   1) Message Control 의 QSIZE(Multiple Message Enable) 필드에 "커널이
 *      실제로 쓸 벡터 수의 log2" 를 적는다. 장치는 이 값을 보고 데이터의
 *      하위 몇 비트를 벡터 번호로 바꿔 쓸지 결정한다.
 *   2) 주소 하위 32비트를 쓴다.
 *   3) 64비트 장치면 주소 상위 32비트를 쓰고 데이터를 PCI_MSI_DATA_64 에,
 *      32비트 장치면 데이터를 PCI_MSI_DATA_32 에 쓴다. 상위 주소 필드가
 *      아예 없어 그만큼 데이터 필드가 앞으로 당겨져 있기 때문이다.
 *   4) 마지막에 Message Control 을 한 번 읽는다. 쓰기가 장치에 도달했음을
 *      보장하기 위한 flush 이며, 읽은 값은 쓰지 않는다.
 *
 * 실행 컨텍스트: 잠들지 않는다.
 *
 * 호출 체인:
 *   __pci_write_msi_msg() -> [이 함수] -> config 읽기/쓰기
 */
static inline void pci_write_msg_msi(struct pci_dev *dev, struct msi_desc *desc,
				     struct msi_msg *msg)
{
	/* [한국어] MSI capability 의 config 오프셋. */
	int pos = dev->msi_cap;
	/* [한국어] Message Control 레지스터 값을 담을 곳. */
	u16 msgctl;

	/* [한국어] 현재 Message Control 을 읽는다. 통째로 쓰면 다른 필드가 망가지므로
	 * 읽고-고쳐-쓰기가 필수다. */
	pci_read_config_word(dev, pos + PCI_MSI_FLAGS, &msgctl);
	/* [한국어] QSIZE(Multiple Message Enable) 필드를 지운다. */
	msgctl &= ~PCI_MSI_FLAGS_QSIZE;
	/* [한국어] 커널이 실제로 쓸 벡터 수의 log2 를 그 자리에 넣는다.
	 * 장치는 이 값을 보고 데이터의 하위 몇 비트를 벡터 번호로 바꿀지 정한다. */
	msgctl |= FIELD_PREP(PCI_MSI_FLAGS_QSIZE, desc->pci.msi_attrib.multiple);
	/* [한국어] 고친 Message Control 을 되쓴다. */
	pci_write_config_word(dev, pos + PCI_MSI_FLAGS, msgctl);

	/* [한국어] 목적지 주소 하위 32비트. */
	pci_write_config_dword(dev, pos + PCI_MSI_ADDRESS_LO, msg->address_lo);
	/* [한국어] 64비트 주소를 쓰는 장치인가. */
	if (desc->pci.msi_attrib.is_64) {
		/* [한국어] 주소 상위 32비트. */
		pci_write_config_dword(dev, pos + PCI_MSI_ADDRESS_HI,  msg->address_hi);
		/* [한국어] 64비트 장치의 데이터 필드 위치. */
		pci_write_config_word(dev, pos + PCI_MSI_DATA_64, msg->data);
	/* [한국어] 32비트 장치. */
	} else {
		/* [한국어] 상위 주소 필드가 없어 데이터가 앞당겨진 위치. */
		pci_write_config_word(dev, pos + PCI_MSI_DATA_32, msg->data);
	}
	/* Ensure that the writes are visible in the device */
	/* [한국어] 읽은 값은 쓰지 않는다. PCIe 쓰기는 posted 라 읽기 하나로
	 * 앞선 쓰기들이 장치에 도달했음을 강제하는 flush 다. */
	pci_read_config_word(dev, pos + PCI_MSI_FLAGS, &msgctl);
}

/* [한국어]
 * pci_write_msg_msix - MSI-X 테이블 항목 하나에 주소/데이터를 써 넣는다
 *
 * @desc: 대상 벡터의 descriptor. 여기서 테이블 항목 주소를 얻는다.
 * @msg:  써 넣을 주소 하위/상위와 데이터.
 * @return: 없음.
 *
 * 항목 하나는 16바이트다 - 주소 하위 4, 주소 상위 4, 데이터 4, Vector
 * Control 4. 앞의 세 워드를 순서대로 채운다.
 *
 * 마스킹 왕복이 이 함수의 핵심이다. 스펙이 "마스크되지 않은 항목의 주소나
 * 데이터를 바꾸면 결과가 정의되지 않는다" 고 못박고 있어(상류 주석이 그
 * 문장을 그대로 인용한다), 지금 마스크가 풀려 있으면 잠시 마스크했다가
 * 쓰고 원래대로 되돌린다. 현재 상태는 msix_ctrl 소프트웨어 캐시로 판단하므로
 * 테이블을 다시 읽지 않는다.
 *
 * 가상 항목(is_virtual)은 대응하는 하드웨어가 없으므로 곧바로 돌아간다.
 * 마지막 readl 은 값을 쓰려는 것이 아니라 앞선 쓰기들이 장치까지 도달하도록
 * 밀어내는 용도다 - PCIe 쓰기는 posted 라 읽기 하나로 순서를 강제해야 한다.
 *
 * 실행 컨텍스트: 잠들지 않는다.
 *
 * 호출 체인:
 *   __pci_write_msi_msg() -> [이 함수]
 *     -> pci_msix_desc_addr() [msi.h] -> writel() / readl()
 */
static inline void pci_write_msg_msix(struct msi_desc *desc, struct msi_msg *msg)
{
	/* [한국어] 이 벡터의 테이블 항목 시작 주소. */
	void __iomem *base = pci_msix_desc_addr(desc);
	/* [한국어] Vector Control 의 소프트웨어 캐시. 하드웨어를 읽지 않고 이 값으로 판단한다. */
	u32 ctrl = desc->pci.msix_ctrl;
	/* [한국어] 지금 마스크가 풀려 있는가. 풀려 있으면 아래에서 잠시 막았다 되돌린다. */
	bool unmasked = !(ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT);

	/* [한국어] 가상 항목에는 쓸 하드웨어가 없다. */
	if (desc->pci.msi_attrib.is_virtual)
		/* [한국어] 조용히 돌아간다. */
		return;
	/*
	 * The specification mandates that the entry is masked
	 * when the message is modified:
	 *
	 * "If software changes the Address or Data value of an
	 * entry while the entry is unmasked, the result is
	 * undefined."
	 */
	/* [한국어] 마스크가 풀린 상태였다면 */
	if (unmasked)
		/* [한국어] 캐시 값에 마스크 비트만 얹어 항목을 잠시 막는다.
		 * 캐시 자체는 고치지 않으므로 아래에서 원래 값으로 되돌릴 수 있다. */
		pci_msix_write_vector_ctrl(desc, ctrl | PCI_MSIX_ENTRY_CTRL_MASKBIT);

	/* [한국어] 목적지 주소 하위 32비트. */
	writel(msg->address_lo, base + PCI_MSIX_ENTRY_LOWER_ADDR);
	/* [한국어] 목적지 주소 상위 32비트. */
	writel(msg->address_hi, base + PCI_MSIX_ENTRY_UPPER_ADDR);
	/* [한국어] 데이터 워드. */
	writel(msg->data, base + PCI_MSIX_ENTRY_DATA);

	/* [한국어] 원래 풀려 있었다면 */
	if (unmasked)
		/* [한국어] 캐시에 담긴 원래 Vector Control 로 되돌려 다시 열어 준다. */
		pci_msix_write_vector_ctrl(desc, ctrl);

	/* Ensure that the writes are visible in the device */
	/* [한국어] 읽은 값은 버린다. 앞선 세 번의 writel 이 장치에 도달하도록 밀어내는 flush. */
	readl(base + PCI_MSIX_ENTRY_DATA);
}

/* [한국어]
 * __pci_write_msi_msg - 정해진 주소/데이터를 하드웨어와 캐시에 반영한다
 *
 * @entry: 대상 벡터의 descriptor.
 * @msg:   아키텍처 IRQ 컨트롤러가 정한 목적지 주소와 데이터.
 * @return: 없음.
 *
 * 이 파일의 데이터 흐름이 닫히는 지점이다. capability 에서 올라간 정보가
 * irqdomain 을 거쳐 message 로 바뀌어 다시 하드웨어로 내려온다.
 *
 * 세 갈래로 나뉜다.
 *   - D0 가 아니거나 장치가 뽑혔으면 하드웨어를 만지지 않는다. 절전 상태의
 *     config 접근은 의미가 없고, 뽑힌 장치는 전부 0xff 를 돌려준다.
 *     그래도 entry->msg 캐시는 갱신하므로 나중에 resume 경로가 복원할 수 있다.
 *   - MSI-X 면 BAR 안의 테이블 항목에 쓴다.
 *   - MSI 면 config space 의 capability 에 쓴다.
 *
 * 마지막에 write_msi_msg 콜백이 있으면 부른다. 인터럽트 리매핑처럼 상위
 * 계층이 자기 표를 함께 갱신해야 하는 구성에서 쓰인다.
 *
 * 실행 컨텍스트: 잠들지 않는다. IRQ 코어가 인터럽트를 옮기는 도중에도 부른다.
 *
 * 호출 체인:
 *   irqdomain 의 irq_write_msi_msg 콜백 / __pci_restore_msi_state()
 *     -> [이 함수] -> pci_write_msg_msix() 또는 pci_write_msg_msi()
 */
void __pci_write_msi_msg(struct msi_desc *entry, struct msi_msg *msg)
{
	/* [한국어] descriptor 에서 PCI 장치를 되돌린다. */
	struct pci_dev *dev = msi_desc_to_pci_dev(entry);

	/* [한국어] D0 가 아니거나 장치가 이미 뽑혔으면 하드웨어를 만지지 않는다.
	 * 절전 상태의 config 접근은 무의미하고, 뽑힌 장치는 전부 0xff 를 준다. */
	if (dev->current_state != PCI_D0 || pci_dev_is_disconnected(dev)) {
		/* Don't touch the hardware now */
	/* [한국어] MSI-X 면 BAR 안의 테이블 항목에 쓴다. */
	} else if (entry->pci.msi_attrib.is_msix) {
		pci_write_msg_msix(entry, msg);
	/* [한국어] 아니면 MSI — config space 의 capability 에 쓴다. */
	} else {
		/* [한국어] MSI 경로. */
		pci_write_msg_msi(dev, entry, msg);
	}

	/* [한국어] 어느 경로였든 소프트웨어 캐시는 갱신한다. 하드웨어를 건너뛴 경우에도
	 * 이 값이 남아야 나중에 resume 경로가 복원할 수 있다. */
	entry->msg = *msg;

	/* [한국어] 상위 계층이 자기 표도 함께 고쳐야 하는 구성(인터럽트 리매핑 등)이면 */
	if (entry->write_msi_msg)
		/* [한국어] 등록해 둔 콜백을 부른다. */
		entry->write_msi_msg(entry, entry->write_msi_msg_data);
}

/* [한국어]
 * pci_write_msi_msg - virq 번호로 descriptor 를 찾아 message 를 기록한다
 *
 * @irq: Linux 가상 IRQ 번호.
 * @msg: 써 넣을 주소/데이터.
 * @return: 없음.
 *
 * __pci_write_msi_msg() 는 msi_desc 를 받는데, IRQ 코어 쪽 코드는 보통
 * virq 번호만 들고 있다. 그 간극을 메우는 얇은 겹이다. irq_get_msi_desc()
 * 로 descriptor 를 찾아 그대로 넘긴다.
 *
 * EXPORT_SYMBOL_GPL 로 내보내므로 MSI 도메인을 직접 구현하는 컨트롤러
 * 드라이버나 아키텍처 코드가 쓸 수 있다. descriptor 가 없으면 NULL 이
 * 그대로 넘어가 역참조하지만, 등록되지 않은 virq 로 부르는 것은 호출자
 * 잘못이라는 전제다. 코드를 고치지 않고 관찰만 기록한다.
 *
 * 실행 컨텍스트: 호출자를 따른다. __pci_write_msi_msg() 가 잠들지 않으므로
 * 인터럽트 문맥에서도 부를 수 있다.
 *
 * 호출 체인:
 *   IRQ 코어 / 컨트롤러 드라이버 -> [이 함수] -> __pci_write_msi_msg()
 */
void pci_write_msi_msg(unsigned int irq, struct msi_msg *msg)
{
	/* [한국어] virq 번호로 descriptor 를 찾는다. IRQ 코어 쪽 코드는 보통 번호만 갖는다. */
	struct msi_desc *entry = irq_get_msi_desc(irq);

	/* [한국어] 찾은 descriptor 로 본체를 부른다. */
	__pci_write_msi_msg(entry, msg);
}
/* [한국어] 모듈에 공개 — MSI 도메인을 직접 구현하는 컨트롤러가 쓴다. */
EXPORT_SYMBOL_GPL(pci_write_msi_msg);


/* PCI/MSI specific functionality */

/* [한국어]
 * pci_intx_for_msi - MSI 전환에 맞춰 레거시 INTx 를 끄거나 되살린다
 *
 * @dev:    대상 PCI 장치.
 * @enable: 0 이면 INTx 를 끄고(=MSI 를 켤 때), 1 이면 되살린다(=끌 때).
 * @return: 없음.
 *
 * 두 인터럽트 방식이 동시에 살아 있으면 같은 사건이 두 번 보고된다.
 * 그래서 MSI/MSI-X 를 켤 때 Command 레지스터의 INTx Disable 비트를 세우고,
 * 끌 때 다시 푼다. 실제 조작은 pci_intx()(drivers/pci/pci.c)가 한다.
 *
 * 다만 그 전환에서 고장 나는 장치가 있다. PCI_DEV_FLAGS_MSI_INTX_DISABLE_BUG
 * 가 붙은 장치는 INTx Disable 을 세우면 MSI 마저 오지 않으므로, 이 함수가
 * 그 플래그를 보고 통째로 건너뛴다. quirk 검사를 호출부마다 흩어 놓지 않고
 * 이 한 겹으로 감싼 것이 이 함수의 존재 이유다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   __msi_capability_init() / msix_capability_init() / 각 shutdown / restore
 *     -> [이 함수] -> pci_intx() [drivers/pci/pci.c]
 */
static void pci_intx_for_msi(struct pci_dev *dev, int enable)
{
	/* [한국어] 이 quirk 가 붙은 장치는 INTx Disable 을 세우면 MSI 마저 오지 않는다.
	 * 그래서 플래그가 있으면 전환을 통째로 건너뛴다. */
	if (!(dev->dev_flags & PCI_DEV_FLAGS_MSI_INTX_DISABLE_BUG))
		/* [한국어] drivers/pci/pci.c 의 pci_intx() 가 Command 레지스터의
		 * INTx Disable 비트를 실제로 조작한다. enable=0 이면 끄고 1 이면 켠다. */
		pci_intx(dev, enable);
}

/* [한국어]
 * pci_msi_set_enable - MSI capability 의 MSI Enable 비트를 켜거나 끈다
 *
 * @dev:    대상 PCI 장치.
 * @enable: 0 이면 끄고, 0 이 아니면 켠다.
 * @return: 없음.
 *
 * Message Control(msi_cap + PCI_MSI_FLAGS)의 0번 비트가 MSI Enable 이다.
 * 이 비트가 1 이 되는 순간부터 장치는 INTx 핀 대신 메모리 쓰기로 인터럽트를
 * 보낸다. 같은 워드에 Multiple Message Capable/Enable, 64비트 여부, 마스킹
 * 지원 여부가 함께 들어 있어 통째로 쓰면 안 되므로 읽고-고쳐-쓴다.
 *
 * MSI-X 쪽 pci_msix_clear_and_set_ctrl() 과 달리 다루는 비트가 하나뿐이라
 * clear/set 인자가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   msi_capability_init() / __msi_capability_init() / pci_msi_shutdown()
 *   / __pci_restore_msi_state() -> [이 함수] -> config 워드 읽기/쓰기
 */
static void pci_msi_set_enable(struct pci_dev *dev, int enable)
{
	/* [한국어] Message Control 값을 담을 곳. */
	u16 control;

	/* [한국어] 현재 Message Control 을 읽는다. 같은 워드에 벡터 수와 64비트 여부,
	 * 마스킹 지원 여부가 함께 있어 통째로 쓰면 안 된다. */
	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &control);
	/* [한국어] MSI Enable(0번 비트)을 일단 내린다. */
	control &= ~PCI_MSI_FLAGS_ENABLE;
	/* [한국어] 켜라는 요청이면 */
	if (enable)
		/* [한국어] 그 비트만 다시 올린다. */
		control |= PCI_MSI_FLAGS_ENABLE;
	/* [한국어] 고친 값을 되쓴다. 이 순간부터(또는 이 순간까지) 장치가 MSI 를 보낸다. */
	pci_write_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, control);
}

/* [한국어]
 * msi_setup_msi_desc - MSI capability 를 읽어 descriptor 한 개를 채워 등록한다
 *
 * @dev:   대상 PCI 장치.
 * @nvec:  요청 벡터 수.
 * @masks: affinity 마스크 배열(선택).
 * @return: msi_insert_msi_desc() 의 결과. 0 성공, 음수 errno.
 *
 * MSI 의 모든 하드웨어 사실이 여기서 소프트웨어로 옮겨진다.
 *   is_64      : 주소가 64비트인가. 이 값이 이후 데이터 레지스터의 오프셋을
 *                가른다(PCI_MSI_DATA_64 대 PCI_MSI_DATA_32).
 *   can_mask   : Mask Bits 레지스터가 있는가. PCI 2.3 이전에는 없었다.
 *   default_irq: 해제 후 되돌아갈 INTx 번호.
 *   multi_cap  : 장치가 알리는 최대 2^N 의 N.
 *   multiple   : 커널이 실제로 쓰겠다고 알릴 2^M 의 M. nvec 을
 *                __roundup_pow_of_two() 로 "올린" 뒤 log2 를 취한다.
 *                예컨대 3개를 요청하면 4개로 올려 M=2 가 된다. MSI 는
 *                데이터의 하위 비트를 벡터 번호로 쓰므로 개수가 반드시
 *                2의 거듭제곱이어야 하기 때문이다.
 *   mask_pos   : Mask Bits 레지스터의 config 오프셋. 주소가 64비트냐에 따라
 *                capability 안에서의 위치가 다르다.
 *
 * 능력값을 그대로 믿지 않는 대목이 둘 있다. quirk 로 "이 장치는 사실
 * 마스킹이 된다"(PCI_DEV_FLAGS_HAS_MSI_MASKING)를 강제로 켜기도 하고,
 * 반대로 도메인이 MSI_FLAG_NO_MASK 를 걸면 강제로 끈다. 상류 주석의
 * "Lies, damned lies, and MSIs" 가 이 사정을 말한다.
 *
 * 마스킹이 가능하면 현재 Mask Bits 값을 읽어 캐시(msi_mask)의 초기값으로
 * 삼는다. 이후 마스킹은 캐시를 고쳐 통째로 쓰는 방식이라 초기값이 맞아야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msi_descs_lock 안.
 *
 * 호출 체인:
 *   __msi_capability_init() -> [이 함수] -> msi_insert_msi_desc()
 */
static int msi_setup_msi_desc(struct pci_dev *dev, int nvec,
			      struct irq_affinity_desc *masks)
{
	/* [한국어] 채워서 넘길 descriptor. 지역 변수인 이유는 msi_insert_msi_desc() 가
	 * 안에서 복사해 가기 때문이다 — 저장소 소유자는 커널 공통 MSI 계층이다. */
	struct msi_desc desc;
	/* [한국어] Message Control 값을 담을 곳. */
	u16 control;

	/* MSI Entry Initialization */
	/* [한국어] 쓰지 않는 필드가 쓰레기값으로 남지 않도록 0 으로 지운다. */
	memset(&desc, 0, sizeof(desc));

	/* [한국어] 장치가 알리는 MSI 능력을 읽는다. */
	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &control);
	/* Lies, damned lies, and MSIs */
	/* [한국어] 실제로는 마스킹이 되는데 능력 비트를 세우지 않는 장치용 quirk. */
	if (dev->dev_flags & PCI_DEV_FLAGS_HAS_MSI_MASKING)
		/* [한국어] 읽은 값에 마스킹 지원 비트를 강제로 얹는다. */
		control |= PCI_MSI_FLAGS_MASKBIT;
	/* [한국어] 반대로 도메인이 "이 경로에서는 마스킹을 쓰지 말라" 고 걸어 둔 경우. */
	if (pci_msi_domain_supports(dev, MSI_FLAG_NO_MASK, DENY_LEGACY))
		/* [한국어] 마스킹 지원 비트를 강제로 내린다. 상류 주석의
		 * "Lies, damned lies, and MSIs" 가 이 두 줄을 가리킨다. */
		control &= ~PCI_MSI_FLAGS_MASKBIT;

	/* [한국어] 이 descriptor 하나가 몇 개의 연속 벡터를 대표하는가.
	 * MSI 는 주소가 하나뿐이라 descriptor 도 하나이고, 여기 nvec 이 통째로 들어간다. */
	desc.nvec_used			= nvec;
	/* [한국어] 64비트 주소 지원 여부. 이후 데이터 필드 오프셋과 mask_pos 를 가른다. */
	desc.pci.msi_attrib.is_64	= !!(control & PCI_MSI_FLAGS_64BIT);
	/* [한국어] Mask Bits 레지스터가 있는가. 위에서 quirk 로 손본 값을 본다. */
	desc.pci.msi_attrib.can_mask	= !!(control & PCI_MSI_FLAGS_MASKBIT);
	/* [한국어] 해제 후 되돌아갈 레거시 INTx IRQ 번호를 미리 보관해 둔다. */
	desc.pci.msi_attrib.default_irq	= dev->irq;
	/* [한국어] 장치가 알리는 최대 벡터 수 2^N 의 N(Multiple Message Capable). */
	desc.pci.msi_attrib.multi_cap	= FIELD_GET(PCI_MSI_FLAGS_QMASK, control);
	/* [한국어] 커널이 실제로 쓰겠다고 알릴 2^M 의 M. nvec 을 2의 거듭제곱까지
	 * "올린" 뒤 log2 를 취한다. 3을 요청하면 4로 올라 M=2 다.
	 * MSI 는 데이터의 하위 비트를 벡터 번호로 쓰므로 개수가 2의 거듭제곱이어야 한다. */
	desc.pci.msi_attrib.multiple	= ilog2(__roundup_pow_of_two(nvec));
	/* [한국어] CPU 배분 규칙. NULL 이면 배분하지 않는다. */
	desc.affinity			= masks;

	/* [한국어] 주소가 64비트냐에 따라 Mask Bits 레지스터의 위치가 다르다. */
	if (control & PCI_MSI_FLAGS_64BIT)
		/* [한국어] 64비트 — 상위 주소 필드가 있는 만큼 뒤로 밀려 있다. */
		desc.pci.mask_pos = dev->msi_cap + PCI_MSI_MASK_64;
	/* [한국어] 32비트 장치. */
	else
		/* [한국어] 상위 주소 필드가 없어 앞당겨진 위치. */
		desc.pci.mask_pos = dev->msi_cap + PCI_MSI_MASK_32;

	/* Save the initial mask status */
	/* [한국어] 마스킹이 가능한 장치라면 */
	if (desc.pci.msi_attrib.can_mask)
		/* [한국어] 현재 Mask Bits 값을 읽어 캐시의 초기값으로 삼는다.
		 * 이후 마스킹은 이 캐시를 고쳐 통째로 쓰는 방식이라 초기값이 맞아야 한다. */
		pci_read_config_dword(dev, desc.pci.mask_pos, &desc.pci.msi_mask);

	/* [한국어] 커널 공통 MSI 계층의 저장소에 복사해 넣는다. 성패가 그대로 반환된다. */
	return msi_insert_msi_desc(&dev->dev, &desc);
}

/* [한국어]
 * msi_verify_entries - 아키텍처가 정한 목적지 주소가 장치 제약에 맞는지 본다
 *
 * @dev: 대상 PCI 장치.
 * @return: 0 이면 모두 통과. -EIO 면 하나라도 범위를 벗어났다.
 *
 * MSI 는 장치가 특정 주소로 메모리 쓰기를 날리는 방식이다. 그런데 장치가
 * 그 주소를 몇 비트까지 낼 수 있는지는 장치마다 다르다 - dev->msi_addr_mask
 * 가 그 한계를 담는다. 아키텍처의 IRQ 컨트롤러가 정해 준 주소가 이 한계를
 * 넘으면 인터럽트가 조용히 사라지므로, 켜기 전에 확인해야 한다.
 *
 * 마스크가 DMA_BIT_MASK(64)면 제약이 없다는 뜻이라 즉시 통과시킨다.
 * 아니면 address_hi<<32 | address_lo 로 64비트 주소를 조립해 마스크 밖
 * 비트가 있는지 본다.
 *
 * 반환 관용구가 특이하다. 루프를 break 로 빠져나오면 entry 가 문제의
 * descriptor 를 가리킨 채 남고, 끝까지 돌면 순회 매크로가 NULL 로 만든다.
 * 그래서 마지막 줄이 entry 의 NULL 여부만으로 성패를 가른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msi_descs_lock 안.
 *
 * 호출 체인:
 *   __msi_capability_init() / __msix_setup_interrupts() -> [이 함수]
 */
static int msi_verify_entries(struct pci_dev *dev)
{
	/* [한국어] 순회 커서. 루프를 어떻게 빠져나왔는지가 아래 반환값을 정한다. */
	struct msi_desc *entry;
	/* [한국어] 64비트로 조립한 목적지 주소. */
	u64 address;

	/* [한국어] 장치가 64비트 주소를 전부 받을 수 있으면 검사할 것이 없다. */
	if (dev->msi_addr_mask == DMA_BIT_MASK(64))
		/* [한국어] 즉시 통과. */
		return 0;

	/* [한국어] 이 장치의 모든 descriptor 를 훑는다. */
	msi_for_each_desc(entry, &dev->dev, MSI_DESC_ALL) {
		/* [한국어] 상위 32비트를 왼쪽으로 밀고 하위와 합쳐 64비트 주소를 만든다. */
		address = (u64)entry->msg.address_hi << 32 | entry->msg.address_lo;
		/* [한국어] 마스크 밖 비트가 하나라도 서 있으면 장치가 낼 수 없는 주소다. */
		if (address & ~dev->msi_addr_mask) {
			/* [한국어] 어느 주소가 어떤 한계를 넘었는지 로그로 남긴다. */
			pci_err(dev, "arch assigned 64-bit MSI address %#llx above device MSI address mask %#llx\n",
				address, dev->msi_addr_mask);
			/* [한국어] entry 를 문제의 descriptor 에 남긴 채 루프를 벗어난다. */
			break;
		}
	}
	/* [한국어] 끝까지 돌면 순회 매크로가 entry 를 NULL 로 만든다.
	 * 그래서 NULL 여부만으로 성패를 가를 수 있다. */
	return !entry ? 0 : -EIO;
}

/* [한국어]
 * __msi_capability_init - MSI descriptor 를 만들고 벡터를 붙인 뒤 Enable 한다
 *
 * @dev:   대상 PCI 장치.
 * @nvec:  벡터 수.
 * @masks: affinity 마스크 배열(선택).
 * @return: 0 성공, 음수 errno 실패.
 *
 * 순서와 그 이유:
 *   1) msi_setup_msi_desc() 로 descriptor 하나를 만든다. MSI 는 벡터가
 *      여럿이어도 descriptor 는 하나다 - 주소가 하나뿐이기 때문이다.
 *   2) 전부 마스크한다. MSI 는 리셋 후 언마스크가 기본이라, 주소를 채우기
 *      전에 인터럽트가 오는 것을 막으려면 명시적으로 막아야 한다.
 *   3) descriptor 를 지역 변수로 복사해 둔다. 계층형 irqdomain 경로에서는
 *      pci_msi_setup_msi_irqs() 가 실패하면서 원본 descriptor 를 해제해
 *      버리므로, 에러 경로에서 마스크를 풀려면 사본이 있어야 한다.
 *      상류 주석이 이 사정을 그대로 적어 두었다.
 *   4) irqdomain 에 등록해 virq 와 message 를 받는다.
 *   5) 받은 주소가 장치 제약을 지키는지 검사한다.
 *   6) 소프트웨어 표시(msi_enabled)를 세우고, INTx 를 끄고, MSI Enable 을 켠다.
 *   7) dev->irq 를 첫 벡터의 virq 로 바꾼다. 이후 드라이버가 보는 IRQ 번호다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msi_descs_lock 을 호출자가 잡았다.
 *
 * 호출 체인:
 *   msi_capability_init() -> [이 함수]
 *     -> msi_setup_msi_desc() -> pci_msi_setup_msi_irqs() [irqdomain.c]
 *     -> msi_verify_entries() -> pci_msi_set_enable()
 */
static int __msi_capability_init(struct pci_dev *dev, int nvec, struct irq_affinity_desc *masks)
{
	/* [한국어] descriptor 를 만들어 등록한다. 이 한 줄에 capability 읽기가 전부 들어 있다. */
	int ret = msi_setup_msi_desc(dev, nvec, masks);
	/* [한국어] entry 는 저장소 안의 실물 포인터, desc 는 그 사본이 될 값 타입 변수다. */
	struct msi_desc *entry, desc;

	/* [한국어] descriptor 생성 실패. */
	if (ret)
		/* [한국어] 아직 하드웨어를 만지지 않았으므로 되돌릴 것이 없다. */
		return ret;

	/* All MSIs are unmasked by default; mask them all */
	/* [한국어] 방금 넣은 descriptor 를 저장소에서 꺼낸다. */
	entry = msi_first_desc(&dev->dev, MSI_DESC_ALL);
	/* [한국어] multi_cap 에서 만든 비트 마스크로 이 장치의 모든 벡터를 막는다.
	 * MSI 는 리셋 후 언마스크가 기본이라 주소를 채우기 전에 막아야 한다. */
	pci_msi_mask(entry, msi_multi_mask(entry));
	/*
	 * Copy the MSI descriptor for the error path because
	 * pci_msi_setup_msi_irqs() will free it for the hierarchical
	 * interrupt domain case.
	 */
	/* [한국어] 에러 경로용 사본. 계층형 irqdomain 에서는 아래 호출이 실패하면서
	 * 원본 descriptor 를 해제해 버려, 마스크를 되돌리려면 사본이 필요하다. */
	memcpy(&desc, entry, sizeof(desc));

	/* Configure MSI capability structure */
	/* [한국어] irqdomain 에 벡터를 등록한다. 이 안에서 아키텍처가 목적지 주소와
	 * 데이터를 정하고, 그 값이 __pci_write_msi_msg() 로 하드웨어에 내려간다. */
	ret = pci_msi_setup_msi_irqs(dev, nvec, PCI_CAP_ID_MSI);
	/* [한국어] 등록 실패. */
	if (ret)
		/* [한국어] 마스크를 되돌리고 자원을 반납하는 공통 에러 경로로. */
		goto err;

	/* [한국어] 아키텍처가 정한 주소가 이 장치의 제약을 지키는지 확인한다. */
	ret = msi_verify_entries(dev);
	/* [한국어] 범위를 벗어난 주소가 있었다. */
	if (ret)
		/* [한국어] 같은 에러 경로로. */
		goto err;

	/* Set MSI enabled bits	*/
	/* [한국어] 소프트웨어 표시를 먼저 세운다. 이후 코드가 이 값을 보고 동작을 고른다. */
	dev->msi_enabled = 1;
	/* [한국어] INTx 를 끈다. 두 방식이 동시에 살아 있으면 인터럽트가 두 번 온다. */
	pci_intx_for_msi(dev, 0);
	/* [한국어] 하드웨어의 MSI Enable 을 올린다. 여기서부터 실제로 MSI 가 온다. */
	pci_msi_set_enable(dev, 1);

	/* [한국어] 아키텍처가 INTx 용으로 잡아 두었던 IRQ 자원을 반납한다. */
	pcibios_free_irq(dev);
	/* [한국어] 드라이버가 보게 될 IRQ 번호를 첫 벡터의 virq 로 바꾼다. */
	dev->irq = entry->irq;
	/* [한국어] 성공. */
	return 0;
/* [한국어] 공통 에러 경로 — 여기 오는 경우는 IRQ 등록 실패와 주소 검증 실패 둘이다. */
err:
	/* [한국어] 사본으로 마스크를 되돌린다. 원본은 이미 해제됐을 수 있다. */
	pci_msi_unmask(&desc, msi_multi_mask(&desc));
	/* [한국어] descriptor 와 virq 를 해제하고 MSI-X 테이블 매핑도 정리한다. */
	pci_free_msi_irqs(dev);
	/* [한국어] 실패 원인을 그대로 위로 전한다. */
	return ret;
}

/* [한국어]
 * msi_capability_init - MSI 를 켜기 전 준비를 하고 본체를 부른다
 *
 * @dev:  대상 PCI 장치.
 * @nvec: 이번에 시도할 벡터 수.
 * @affd: affinity 배분 규칙(선택).
 * @return: 0 성공, 음수 errno 실패, 1 은 "multi-MSI 를 못 쓰니 하나로 해보라".
 *
 * 세 가지를 한다.
 *   1) multi-MSI 조기 거절. irqdomain 이 MSI_FLAG_MULTI_PCI_MSI 를 지원하지
 *      않는데 nvec>1 이면, 하드웨어를 만지기 전에 1 을 돌려 호출자가 벡터
 *      하나로 다시 시도하게 한다. 실패 후 되돌리는 비용을 아끼는 것이다.
 *   2) 설정 전에 MSI Enable 을 내린다. 켜진 채로 주소/데이터를 바꾸면
 *      스펙상 동작이 정의되지 않는다.
 *   3) affinity 마스크를 만들고(__free(kfree) 로 수명을 묶는다)
 *      msi_descs_lock 을 잡은 뒤 __msi_capability_init() 로 넘긴다.
 *
 * 준비와 본체를 나눈 이유는 본체가 실패 경로에서 descriptor 를 되돌려야
 * 하는데, 락과 마스크 해제까지 함께 다루면 경로가 얽히기 때문이다.
 * 실행 컨텍스트: 프로세스 컨텍스트(뮤텍스와 GFP_KERNEL 할당).
 *
 * 호출 체인:
 *   __pci_enable_msi_range() -> [이 함수] -> __msi_capability_init()
 */
/**
 * msi_capability_init - configure device's MSI capability structure
 * @dev: pointer to the pci_dev data structure of MSI device function
 * @nvec: number of interrupts to allocate
 * @affd: description of automatic IRQ affinity assignments (may be %NULL)
 *
 * Setup the MSI capability structure of the device with the requested
 * number of interrupts.  A return value of zero indicates the successful
 * setup of an entry with the new MSI IRQ.  A negative return value indicates
 * an error, and a positive return value indicates the number of interrupts
 * which could have been allocated.
 */
static int msi_capability_init(struct pci_dev *dev, int nvec,
			       struct irq_affinity *affd)
{
	/* Reject multi-MSI early on irq domain enabled architectures */
	/* [한국어] 벡터를 여럿 요청했는데 irqdomain 이 multi-MSI 를 지원하지 않는 경우. */
	if (nvec > 1 && !pci_msi_domain_supports(dev, MSI_FLAG_MULTI_PCI_MSI, ALLOW_LEGACY))
		/* [한국어] 1 을 돌려준다 — 에러가 아니라 "하나로 다시 해보라" 는 제안이다.
		 * 하드웨어를 만지기 전에 판정해 되돌리기 비용을 아낀다. */
		return 1;

	/*
	 * Disable MSI during setup in the hardware, but mark it enabled
	 * so that setup code can evaluate it.
	 */
	/* [한국어] 설정 중에는 꺼 둔다. 켜진 채로 주소나 데이터를 바꾸면
	 * 스펙상 동작이 정의되지 않는다. */
	pci_msi_set_enable(dev, 0);

	/* [한국어] affinity 마스크 배열. __free(kfree) 로 묶어 두어 어느 경로로 빠져나가도 샌다. */
	struct irq_affinity_desc *masks __free(kfree) =
		/* [한국어] affd 가 없으면 배분하지 않으므로 NULL 이다. */
		affd ? irq_create_affinity_masks(nvec, affd) : NULL;

	/* [한국어] 이 장치의 descriptor 목록을 잠근다. guard 라 함수를 나갈 때 자동 해제된다.
	 * descriptor 를 넣고 IRQ 를 붙이는 사이에 끼어들면 목록이 깨진다. */
	guard(msi_descs_lock)(&dev->dev);
	/* [한국어] 준비가 끝났으니 본체로 넘긴다. */
	return __msi_capability_init(dev, nvec, masks);
}

/* [한국어]
 * __pci_enable_msi_range - 요청 범위 안에서 MSI 벡터를 확보한다
 *
 * @dev:    대상 PCI 장치.
 * @minvec: 이만큼도 못 얻으면 실패.
 * @maxvec: 최대 희망 개수.
 * @affd:   자동 affinity 배분 규칙(선택).
 * @return: 확보한 벡터 수(양수) 또는 음수 errno. -EINVAL 상태 오류,
 *          -ERANGE 범위 역전, -ENOTSUPP 도메인 미지원, -ENOSPC 최소치 미달.
 *
 * MSI-X 쪽 __pci_enable_msix_range() 와 짝을 이루는 MSI 쪽 구현이다.
 * 검사 순서는 거의 같지만 MSI 만의 제약이 하나 더 있다 - 장치가 알리는
 * 최대치(pci_msi_vec_count)가 2의 거듭제곱이고 32 를 넘지 못한다.
 *
 * 마지막 루프가 핵심이다. msi_capability_init() 가 양수를 돌려주면 그것은
 * 에러가 아니라 "이 수로 줄여 보라" 는 제안이므로 nvec 을 낮춰 다시 돈다.
 * 대표적으로 irqdomain 이 multi-MSI 를 지원하지 않을 때 1 을 돌려주고,
 * 그러면 벡터 하나로 다시 시도한다.
 *
 * affd 가 있으면 매 회차마다 irq_calc_affinity_vectors() 로 "이 CPU 배치
 * 규칙에서 의미 있는 최대 벡터 수" 를 다시 구한다 - CPU 수보다 많은 벡터를
 * 잡아 봐야 놀기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_alloc_irq_vectors_affinity() / pci_enable_msi() [api.c]
 *     -> [이 함수] -> pci_msi_supported() -> pci_setup_msi_context()
 *                  -> msi_capability_init()
 */
int __pci_enable_msi_range(struct pci_dev *dev, int minvec, int maxvec,
			   struct irq_affinity *affd)
{
	/* [한국어] 이번 회차에 시도할 벡터 수. */
	int nvec;
	/* [한국어] 하위 호출의 반환값을 받을 곳. */
	int rc;

	/* [한국어] 전역/장치/버스 차원에서 MSI 가 막혀 있거나 장치가 D0 가 아니면 안 된다.
	 * D0 가 아니면 config 접근 자체가 의미가 없다. */
	if (!pci_msi_supported(dev, minvec) || dev->current_state != PCI_D0)
		/* [한국어] 인자 오류로 거절. */
		return -EINVAL;

	/* Check whether driver already requested MSI-X IRQs */
	/* [한국어] MSI-X 가 이미 켜져 있으면 MSI 를 켤 수 없다. 하드웨어가 둘 중
	 * 하나만 허용하기 때문이다. */
	if (dev->msix_enabled) {
		/* [한국어] 드라이버가 알아볼 수 있도록 이유를 로그로 남긴다. */
		pci_info(dev, "can't enable MSI (MSI-X already enabled)\n");
		/* [한국어] 거절. */
		return -EINVAL;
	}

	/* [한국어] 범위가 뒤집혀 있다. */
	if (maxvec < minvec)
		/* [한국어] -ERANGE 로 구분해 준다 — 다른 실패와 원인이 다르기 때문이다. */
		return -ERANGE;

	/* [한국어] 이미 MSI 가 켜져 있는데 또 켜려는 것은 호출자 버그다. */
	if (WARN_ON_ONCE(dev->msi_enabled))
		/* [한국어] WARN 을 남기고 거절. */
		return -EINVAL;

	/* Test for the availability of MSI support */
	/* [한국어] 이 장치가 속한 irqdomain 이 MSI 자체를 다룰 수 있는가. */
	if (!pci_msi_domain_supports(dev, 0, ALLOW_LEGACY))
		/* [한국어] 못 다루면 지원하지 않음. */
		return -ENOTSUPP;

	/* [한국어] 장치가 알리는 최대 벡터 수. */
	nvec = pci_msi_vec_count(dev);
	/* [한국어] capability 가 없으면 음수가 온다. */
	if (nvec < 0)
		/* [한국어] 그 errno 를 그대로 전한다. */
		return nvec;
	/* [한국어] 장치가 낼 수 있는 최대치가 요청 최소치에도 못 미친다. */
	if (nvec < minvec)
		/* [한국어] 자리 부족. */
		return -ENOSPC;

	/* [한국어] descriptor 저장소와 devm 해제 액션을 준비한다. */
	rc = pci_setup_msi_context(dev);
	/* [한국어] 준비 실패(대개 -ENOMEM). */
	if (rc)
		/* [한국어] 그대로 전한다. */
		return rc;

	/* [한국어] 이 장치 전용 MSI irq_domain 을 세운다(irqdomain.c). */
	if (!pci_setup_msi_device_domain(dev, nvec))
		/* [한국어] 세우지 못하면 벡터를 붙일 곳이 없다. */
		return -ENODEV;

	/* [한국어] 장치 능력이 요청 상한보다 크면 */
	if (nvec > maxvec)
		/* [한국어] 요청 상한으로 낮춘다. 필요 이상으로 잡을 이유가 없다. */
		nvec = maxvec;

	/* [한국어] 성공하거나 확실히 실패할 때까지 개수를 줄여 가며 반복한다. */
	for (;;) {
		/* [한국어] CPU 배분 규칙이 있으면 */
		if (affd) {
			/* [한국어] 그 규칙에서 의미 있는 최대 벡터 수를 다시 구한다.
			 * CPU 수보다 많이 잡아 봐야 놀기 때문이다. */
			nvec = irq_calc_affinity_vectors(minvec, nvec, affd);
			/* [한국어] 줄인 결과가 최소치에도 못 미치면 */
			if (nvec < minvec)
				/* [한국어] 자리 부족으로 포기. */
				return -ENOSPC;
		}

		/* [한국어] 실제로 켜 본다. */
		rc = msi_capability_init(dev, nvec, affd);
		/* [한국어] 0 이면 성공이다. */
		if (rc == 0)
			/* [한국어] 확보한 개수를 돌려준다. */
			return nvec;

		/* [한국어] 음수는 진짜 실패다. */
		if (rc < 0)
			/* [한국어] 그대로 전한다. */
			return rc;
		/* [한국어] 양수인데 최소치보다 작으면 더 줄여 봐야 소용이 없다. */
		if (rc < minvec)
			/* [한국어] 자리 부족. */
			return -ENOSPC;

		/* [한국어] 제안받은 수로 낮춰 다시 돈다. */
		nvec = rc;
	}
}

/* [한국어]
 * pci_msi_vec_count - 이 장치가 MSI 로 받을 수 있는 최대 벡터 수
 *
 * @dev: 대상 PCI 장치.
 * @return: 2의 거듭제곱(1..32). MSI capability 가 없으면 -EINVAL.
 *
 * Message Control 의 Multiple Message Capable 필드(PCI_MSI_FLAGS_QMASK,
 * 3비트)는 장치가 "나는 2^N 개까지 받을 수 있다" 를 N 으로 알리는 자리다.
 * 그래서 읽은 값을 그대로 쓰지 않고 1 << N 으로 되돌린다. N 이 최대 5 라
 * 상한이 32 이고, 이것이 MSI 가 MSI-X(최대 2048)에 밀리는 첫 번째 이유다.
 *
 * 두 번째 이유는 이 값이 "능력" 일 뿐이라는 점이다. 실제로 몇 개를 쓸지는
 * 커널이 정해 Message Control 의 QSIZE 필드에 따로 적어 준다.
 *
 * 실행 컨텍스트: 제약 없음. config 읽기 한 번이다.
 * EXPORT_SYMBOL 이라 모듈에서도 부를 수 있다.
 *
 * 호출 체인:
 *   __pci_enable_msi_range() 및 외부 드라이버 -> [이 함수] -> config 읽기
 */
/**
 * pci_msi_vec_count - Return the number of MSI vectors a device can send
 * @dev: device to report about
 *
 * This function returns the number of MSI vectors a device requested via
 * Multiple Message Capable register. It returns a negative errno if the
 * device is not capable sending MSI interrupts. Otherwise, the call succeeds
 * and returns a power of two, up to a maximum of 2^5 (32), according to the
 * MSI specification.
 **/
int pci_msi_vec_count(struct pci_dev *dev)
{
	/* [한국어] 1 << N 결과를 담을 곳. */
	int ret;
	/* [한국어] Message Control 값. */
	u16 msgctl;

	/* [한국어] MSI capability 자체가 없는 장치. */
	if (!dev->msi_cap)
		/* [한국어] 인자 오류로 구분해 돌려준다. */
		return -EINVAL;

	/* [한국어] Message Control 을 읽는다. */
	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &msgctl);
	/* [한국어] QMASK(Multiple Message Capable, 3비트)는 2^N 의 N 이므로
	 * 1 을 그만큼 밀어 실제 개수로 되돌린다. N 최대 5 라 상한이 32 다. */
	ret = 1 << FIELD_GET(PCI_MSI_FLAGS_QMASK, msgctl);

	/* [한국어] 1..32 사이의 2의 거듭제곱. */
	return ret;
}
/* [한국어] 모듈에 공개(GPL 제한 없음). */
EXPORT_SYMBOL(pci_msi_vec_count);

/* [한국어]
 * arch_restore_msi_irqs - resume 때 message 를 커널이 다시 써야 하는가
 *
 * @dev: 대상 PCI 장치.
 * @return: true 면 이 파일의 복원 루틴이 message 를 다시 기록한다.
 *          false 면 아키텍처가 이미 자기 방식으로 복원했으니 손대지 말라는 뜻.
 *
 * __weak 이라 아키텍처가 같은 이름으로 덮어쓸 수 있는 기본 구현이다.
 * 기본값 true 는 "평범한 경우 커널이 직접 다시 쓴다" 를 뜻한다.
 * 하이퍼바이저가 인터럽트 라우팅을 소유하는 환경(예: 반가상화 게스트)에서는
 * 게스트가 message 를 다시 쓰면 안 되므로 false 로 덮어쓴다.
 *
 * 이 트리 안에서 이 이름을 덮어쓴 구현은 없다(전수 grep 확인) - 아키텍처
 * 코드는 이 스파스 체크아웃에 들어 있지 않다.
 * 실행 컨텍스트: 프로세스 컨텍스트(resume).
 *
 * 호출 체인:
 *   __pci_restore_msi_state()(780) / __pci_restore_msix_state()(1219)
 *     -> [이 함수]
 */
/*
 * Architecture override returns true when the PCI MSI message should be
 * written by the generic restore function.
 */
bool __weak arch_restore_msi_irqs(struct pci_dev *dev)
{
	/* [한국어] 기본 구현은 항상 참 — 평범한 경우 커널이 message 를 직접 다시 쓴다.
	 * 하이퍼바이저가 라우팅을 소유하는 환경만 이 함수를 덮어써 거짓을 준다. */
	return true;
}

/* [한국어]
 * __pci_restore_msi_state - 전원 복귀 후 MSI capability 를 다시 세운다
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * MSI 는 벡터 정보가 config space 에 있으므로, D3cold 등으로 config 가
 * 초기화된 뒤에는 커널이 캐시해 둔 값을 다시 써 넣어야 한다.
 *
 * 순서:
 *   1) msi_enabled 가 아니면 복원할 것이 없다
 *   2) dev->irq 로 descriptor 를 찾는다. MSI 는 벡터들이 연속이라 첫 IRQ
 *      번호 하나로 descriptor 를 찾을 수 있다
 *   3) INTx 를 끄고 MSI Enable 도 잠시 내린다 - 주소/데이터를 바꾸는 동안
 *      켜 두면 안 된다
 *   4) message 를 다시 쓴다(아키텍처가 원하면)
 *   5) Mask Bits 를 캐시값으로 되돌린다. pci_msi_update_mask(entry, 0, 0)
 *      은 clear 도 set 도 0 이라 값은 그대로 두고 하드웨어에만 다시 쓴다
 *   6) QSIZE 를 다시 채우고 Enable 을 올린다
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(resume 경로).
 *
 * 호출 체인:
 *   pci_restore_state() -> pci_restore_msi_state() [api.c]
 *     -> [이 함수] -> __pci_write_msi_msg() / pci_msi_update_mask()
 */
void __pci_restore_msi_state(struct pci_dev *dev)
{
	/* [한국어] 복원할 벡터의 descriptor. */
	struct msi_desc *entry;
	/* [한국어] Message Control 값을 담을 곳. */
	u16 control;

	/* [한국어] 켜져 있지 않았다면 복원할 상태가 없다. */
	if (!dev->msi_enabled)
		/* [한국어] 그대로 돌아간다. */
		return;

	/* [한국어] dev->irq 로 descriptor 를 찾는다. MSI 는 벡터들이 연속된 virq 를 받고
	 * descriptor 의 irq 가 그 첫 번째라 번호 하나로 찾을 수 있다. */
	entry = irq_get_msi_desc(dev->irq);

	/* [한국어] INTx 를 끈다. 복원 도중 두 경로가 동시에 살아 있으면 안 된다. */
	pci_intx_for_msi(dev, 0);
	/* [한국어] MSI Enable 도 잠시 내린다. 켜진 채로 주소/데이터를 바꾸면
	 * 스펙상 동작이 정의되지 않는다. */
	pci_msi_set_enable(dev, 0);
	/* [한국어] 아키텍처가 "커널이 직접 다시 쓰라" 고 하면 */
	if (arch_restore_msi_irqs(dev))
		/* [한국어] 캐시해 둔 message 를 config space 에 되쓴다. */
		__pci_write_msi_msg(entry, &entry->msg);

	/* [한국어] 현재 Message Control 을 읽는다. 아래에서 고쳐 쓸 바탕이다. */
	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &control);
	/* [한국어] clear 와 set 이 모두 0 — 캐시 값은 그대로 두고 하드웨어에만 다시 쓴다.
	 * 전원이 끊기며 사라진 Mask Bits 를 복원하는 것이 목적이다. */
	pci_msi_update_mask(entry, 0, 0);
	/* [한국어] QSIZE 필드를 지운다. */
	control &= ~PCI_MSI_FLAGS_QSIZE;
	/* [한국어] MSI Enable 을 올리면서 */
	control |= PCI_MSI_FLAGS_ENABLE |
		   /* [한국어] 쓰던 벡터 수의 log2 를 QSIZE 에 다시 넣는다. */
		   FIELD_PREP(PCI_MSI_FLAGS_QSIZE, entry->pci.msi_attrib.multiple);
	/* [한국어] 한 번의 쓰기로 Enable 과 QSIZE 를 함께 반영한다. */
	pci_write_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, control);
}

/* [한국어]
 * pci_msi_shutdown - MSI 를 끄고 장치를 INTx 상태로 되돌린다
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * __msi_capability_init() 의 역순이다. Message Control 의 MSI Enable 을
 * 내리고, INTx 를 되살리고, dev->msi_enabled 를 0 으로 하고, 마스크를 풀고,
 * dev->irq 를 descriptor 에 보관해 둔 원래 INTx 번호로 되돌린 뒤
 * pcibios_alloc_irq() 로 아키텍처에 다시 알린다.
 *
 * 마스크를 "풀어서" 돌려주는 것이 이상해 보이지만, MSI 의 Mask Bits 는
 * 리셋 직후 전부 0(언마스크)이 규정값이라 초기 상태로 되돌리는 것이 맞다.
 *
 * dev->irq 복원 순서를 눈여겨볼 만하다. desc 를 먼저 얻어 두고 나중에
 * default_irq 를 읽는데, msi_first_desc() 가 NULL 을 줄 수 있는 상황을
 * WARN_ON_ONCE 로만 걸러 두어 그 뒤 역참조가 남아 있다. 코드를 고치지
 * 않고 관찰만 기록한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_disable_msi() / pci_free_irq_vectors() [api.c]
 *     -> [이 함수] -> pci_msi_set_enable() / pci_intx_for_msi() / pci_msi_unmask()
 */
void pci_msi_shutdown(struct pci_dev *dev)
{
	/* [한국어] 마스크를 되돌릴 descriptor. */
	struct msi_desc *desc;

	/* [한국어] 전역으로 꺼져 있거나, 장치가 없거나, 애초에 MSI 가 아니면 할 일이 없다. */
	if (!pci_msi_enable || !dev || !dev->msi_enabled)
		/* [한국어] 그대로 돌아간다. */
		return;

	/* [한국어] 하드웨어의 MSI Enable 을 내린다. 여기서부터 MSI 가 오지 않는다. */
	pci_msi_set_enable(dev, 0);
	/* [한국어] INTx 를 되살린다. 인자 1 이 "다시 켜라" 다. */
	pci_intx_for_msi(dev, 1);
	/* [한국어] 소프트웨어 표시도 내린다. */
	dev->msi_enabled = 0;

	/* Return the device with MSI unmasked as initial states */
	/* [한국어] 마스크를 되돌릴 대상. MSI 는 descriptor 가 하나뿐이다. */
	desc = msi_first_desc(&dev->dev, MSI_DESC_ALL);
	/* [한국어] 없으면 상태가 어긋난 것이라 WARN 을 남긴다. */
	if (!WARN_ON_ONCE(!desc))
		/* [한국어] 모든 벡터의 마스크를 푼다. 리셋 직후 언마스크가 규정값이라
		 * 초기 상태로 되돌리는 것이 맞다. */
		pci_msi_unmask(desc, msi_multi_mask(desc));

	/* Restore dev->irq to its default pin-assertion IRQ */
	/* [한국어] 드라이버가 보는 IRQ 번호를 원래 INTx 번호로 되돌린다.
	 * 위 WARN_ON_ONCE 가 desc==NULL 을 걸러 내기만 하고 흐름을 끊지 않아,
	 * 여기서 NULL 역참조가 남아 있다. 코드는 고치지 않고 관찰만 적어 둔다. */
	dev->irq = desc->pci.msi_attrib.default_irq;
	/* [한국어] 아키텍처에 INTx IRQ 를 다시 잡아 달라고 알린다. */
	pcibios_alloc_irq(dev);
}

/* PCI/MSI-X specific functionality */

/* [한국어]
 * pci_msix_clear_and_set_ctrl - MSI-X Message Control 비트를 읽고-고쳐-쓴다
 *
 * @dev:   대상 PCI 장치.
 * @clear: 0 으로 내릴 비트 마스크.
 * @set:   1 로 올릴 비트 마스크.
 * @return: 없음.
 *
 * Message Control(capability + PCI_MSIX_FLAGS)에는 서로 다른 뜻의 필드가
 * 한 워드에 모여 있다 - Table Size(하위 11비트, 읽기 전용), Function Mask
 * (PCI_MSIX_FLAGS_MASKALL), MSI-X Enable(PCI_MSIX_FLAGS_ENABLE). 그래서
 * 통째로 쓰면 Table Size 를 포함한 다른 필드를 망가뜨린다. 반드시 읽어서
 * 고친 뒤 다시 써야 하고, 그 패턴을 한 곳에 모아 둔 것이 이 함수다.
 *
 * clear 를 set 보다 먼저 적용하므로, 같은 비트를 양쪽에 넣으면 결과는 1 이다.
 * 락이 없는데, 호출자들이 모두 MSI-X 상태 전이를 직렬화하는 문맥(초기화,
 * 종료, resume)에서만 부르기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   msix_capability_init() / pci_msix_shutdown() / __pci_restore_msix_state()
 *     -> [이 함수] -> config 워드 읽기/쓰기
 */
static void pci_msix_clear_and_set_ctrl(struct pci_dev *dev, u16 clear, u16 set)
{
	/* [한국어] Message Control 값을 담을 곳. */
	u16 ctrl;

	/* [한국어] 현재 값을 읽는다. 같은 워드에 읽기 전용 Table Size 가 들어 있어
	 * 통째로 쓰면 그것까지 망가뜨린다. */
	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &ctrl);
	/* [한국어] 내릴 비트를 먼저 지운다. */
	ctrl &= ~clear;
	/* [한국어] 올릴 비트를 세운다. clear 가 먼저라 같은 비트를 양쪽에 주면 결과는 1 이다. */
	ctrl |= set;
	/* [한국어] 고친 값을 되쓴다. */
	pci_write_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, ctrl);
}

/* [한국어]
 * msix_map_region - MSI-X 테이블이 놓인 BAR 구간을 찾아 ioremap 한다
 *
 * @dev:        대상 PCI 장치.
 * @nr_entries: 매핑할 항목 수. 보통 테이블 전체 크기.
 * @return: 매핑된 커널 가상 주소. 실패하면 NULL.
 *
 * MSI 에는 없고 MSI-X 에만 있는 단계다. MSI 는 벡터 정보가 config space
 * 안에 다 들어 있지만, MSI-X 는 capability 에 "어느 BAR 의 어느 오프셋" 만
 * 적혀 있고 실제 테이블은 그 BAR 안에 있다.
 *
 * capability + PCI_MSIX_TABLE 위치의 32비트 워드가 그 정보를 담는다.
 *   하위 3비트  = BIR(BAR Indicator Register). 0..5 중 어느 BAR 인지.
 *   상위 29비트 = 그 BAR 안에서의 오프셋(8바이트 정렬이라 하위 3비트가 남았다).
 * 그래서 PCI_MSIX_TABLE_BIR 로 아래를 떼고 PCI_MSIX_TABLE_OFFSET 으로 위를
 * 떼는 두 번의 마스킹이 필요하다.
 *
 * 자원 플래그가 비었거나 IORESOURCE_UNSET 이면 그 BAR 가 아직 배정되지
 * 않았다는 뜻이라 실패로 돌린다 - 주소를 계산해 봐야 의미가 없다.
 * 매핑 크기는 항목 수 곱하기 PCI_MSIX_ENTRY_SIZE(16바이트)다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(ioremap 이 잠들 수 있다).
 *
 * 호출 체인:
 *   msix_capability_init() -> [이 함수] -> pci_resource_start() / ioremap()
 */
static void __iomem *msix_map_region(struct pci_dev *dev,
				     unsigned int nr_entries)
{
	/* [한국어] 테이블이 놓인 물리 주소. */
	resource_size_t phys_addr;
	/* [한국어] Table Offset/BIR 레지스터 값. 두 정보가 한 워드에 섞여 있다. */
	u32 table_offset;
	/* [한국어] 해당 BAR 의 자원 플래그. */
	unsigned long flags;
	/* [한국어] BAR Indicator Register — 몇 번 BAR 인가. */
	u8 bir;

	/* [한국어] capability + PCI_MSIX_TABLE 위치의 32비트 워드를 읽는다. */
	pci_read_config_dword(dev, dev->msix_cap + PCI_MSIX_TABLE,
			      &table_offset);
	/* [한국어] 하위 3비트가 BIR. 0..5 중 어느 BAR 에 테이블이 있는지를 가리킨다. */
	bir = (u8)(table_offset & PCI_MSIX_TABLE_BIR);
	/* [한국어] 그 BAR 의 자원 플래그를 가져온다. */
	flags = pci_resource_flags(dev, bir);
	/* [한국어] 플래그가 비었거나 아직 배정되지 않은(UNSET) BAR 이면 주소를 계산해도 소용없다. */
	if (!flags || (flags & IORESOURCE_UNSET))
		/* [한국어] 실패로 돌린다 — 호출자가 -ENOMEM 으로 바꾼다. */
		return NULL;

	/* [한국어] 상위 29비트만 남겨 BAR 안에서의 오프셋을 얻는다.
	 * 테이블이 8바이트 정렬이라 하위 3비트를 BIR 이 쓰고 있었다. */
	table_offset &= PCI_MSIX_TABLE_OFFSET;
	/* [한국어] BAR 의 물리 시작 주소에 오프셋을 더해 테이블의 물리 주소를 얻는다. */
	phys_addr = pci_resource_start(dev, bir) + table_offset;

	/* [한국어] 항목 수 곱하기 16바이트만큼 커널 가상 주소 공간에 매핑한다.
	 * 결과가 dev->msix_base 가 되어 모든 벡터의 mask_base 로 쓰인다. */
	return ioremap(phys_addr, nr_entries * PCI_MSIX_ENTRY_SIZE);
}

/* [한국어]
 * msix_prepare_msi_desc - 반쯤 채워진 MSI-X descriptor 의 나머지를 채운다
 *
 * @dev:  대상 PCI 장치.
 * @desc: msi_index 와 affinity 만 채워져 들어오는 descriptor.
 * @return: 없음.
 *
 * 채우는 것은 넷이다. nvec_used=1(MSI-X 는 항목 하나가 벡터 하나),
 * is_msix=1, is_64=1(MSI-X 주소는 항상 64비트라 조건 분기가 없다),
 * default_irq(해제 후 되돌아갈 INTx 번호), mask_base(테이블의 가상 주소).
 *
 * 그다음 마스킹 가능 여부를 정한다. MSI-X 는 스펙상 항목별 마스킹이
 * 필수라 보통 can_mask=1 이지만, 도메인이 MSI_FLAG_NO_MASK 를 걸었거나
 * 가상 항목이면 만질 실체가 없어 건너뛴다. 만질 수 있으면 현재 Vector
 * Control 값을 읽어 msix_ctrl 캐시의 초기값으로 삼는다 - 이후 마스킹은
 * 이 캐시를 고쳐 쓰는 방식이라 초기값이 맞아야 한다.
 *
 * SUN NIU 장치용 우회가 하나 있다. 그 장치는 항목을 읽기 전에 한 번
 * 써야 정상 값을 돌려주므로, 해당 quirk 플래그가 있으면 Data 필드에 0 을
 * 먼저 쓴다.
 *
 * 이 함수가 msix_setup_msi_descs() 에서 떨어져 나온 이유는 상류 주석대로
 * MSI-X 를 이미 켠 뒤 동적으로 벡터를 더 붙이는 경로가 있기 때문이다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   msix_setup_msi_descs() 또는 msi/irqdomain.c:341 의 동적 할당 경로
 *     -> [이 함수] -> pci_msix_desc_addr() [msi.h] -> readl()
 */
/**
 * msix_prepare_msi_desc - Prepare a half initialized MSI descriptor for operation
 * @dev:	The PCI device for which the descriptor is prepared
 * @desc:	The MSI descriptor for preparation
 *
 * This is separate from msix_setup_msi_descs() below to handle dynamic
 * allocations for MSI-X after initial enablement.
 *
 * Ideally the whole MSI-X setup would work that way, but there is no way to
 * support this for the legacy arch_setup_msi_irqs() mechanism and for the
 * fake irq domains like the x86 XEN one. Sigh...
 *
 * The descriptor is zeroed and only @desc::msi_index and @desc::affinity
 * are set. When called from msix_setup_msi_descs() then the is_virtual
 * attribute is initialized as well.
 *
 * Fill in the rest.
 */
void msix_prepare_msi_desc(struct pci_dev *dev, struct msi_desc *desc)
{
	/* [한국어] MSI-X 는 항목 하나가 벡터 하나다. MSI 처럼 여러 개를 대표하지 않는다. */
	desc->nvec_used			= 1;
	/* [한국어] 이 descriptor 가 MSI-X 쪽임을 표시. 이후 모든 분기가 이 비트를 본다. */
	desc->pci.msi_attrib.is_msix	= 1;
	/* [한국어] MSI-X 주소는 스펙상 항상 64비트라 조건 분기 없이 1 이다. */
	desc->pci.msi_attrib.is_64	= 1;
	/* [한국어] MSI/MSI-X 를 해제했을 때 되돌아갈 레거시 INTx IRQ 번호를 보관해 둔다. */
	desc->pci.msi_attrib.default_irq = dev->irq;
	/* [한국어] ioremap 해 둔 테이블의 시작 주소. 여기에 항목 번호를 곱해 더하면
	 * 이 벡터의 항목 주소가 나온다(msi.h 의 pci_msix_desc_addr). */
	desc->pci.mask_base		= dev->msix_base;


	/* [한국어] 도메인이 마스킹을 금지하지 않았고 */
	if (!pci_msi_domain_supports(dev, MSI_FLAG_NO_MASK, DENY_LEGACY) &&
	    /* [한국어] 실체 없는 가상 항목도 아니라면 — 만질 하드웨어가 있다는 뜻이다. */
	    !desc->pci.msi_attrib.is_virtual) {
		/* [한국어] 이 벡터의 항목 주소. */
		void __iomem *addr = pci_msix_desc_addr(desc);

		/* [한국어] 항목별 마스킹이 가능하다고 표시. */
		desc->pci.msi_attrib.can_mask = 1;
		/* Workaround for SUN NIU insanity, which requires write before read */
		/* [한국어] SUN NIU 장치용 우회 — 읽기 전에 한 번 써야 정상 값을 돌려준다. */
		if (dev->dev_flags & PCI_DEV_FLAGS_MSIX_TOUCH_ENTRY_DATA_FIRST)
			/* [한국어] 그래서 Data 필드에 0 을 먼저 써 준다. */
			writel(0, addr + PCI_MSIX_ENTRY_DATA);
		/* [한국어] 현재 Vector Control 을 읽어 캐시의 초기값으로 삼는다.
		 * 이후 마스킹은 이 캐시를 고쳐 쓰는 방식이라 초기값이 맞아야 한다. */
		desc->pci.msix_ctrl = readl(addr + PCI_MSIX_ENTRY_VECTOR_CTRL);
	}
}

/* [한국어]
 * msix_setup_msi_descs - 요청한 개수만큼 MSI-X descriptor 를 만들어 등록한다
 *
 * @dev:     대상 PCI 장치.
 * @entries: 항목 번호 배열(선택). NULL 이면 번호가 0,1,2,... 가 된다.
 * @nvec:    만들 개수.
 * @masks:   affinity 마스크 배열(선택).
 * @return: 0 성공, 음수 errno(주로 -ENOMEM). 도중에 실패하면 그때까지 넣은
 *          descriptor 는 호출자 쪽 자동 해제가 걷어낸다.
 *
 * 지역 변수 desc 하나를 재사용해 매번 채우고 msi_insert_msi_desc() 로
 * 복사해 넣는다 - 저장소를 소유하는 쪽은 커널 공통 MSI 계층이다.
 *
 * is_virtual 판정이 여기서 나온다. 항목 번호가 하드웨어 테이블 크기
 * (pci_msix_vec_count)보다 크거나 같으면 "실체 없는 벡터" 로 표시하고,
 * 이후 코드가 그 항목에 대해서는 MMIO 접근을 건너뛴다. PCI_IRQ_VIRTUAL
 * 로 요청했을 때만 생기는 상태다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msi_descs_lock 안.
 *
 * 호출 체인:
 *   __msix_setup_interrupts() -> [이 함수]
 *     -> msix_prepare_msi_desc() -> msi_insert_msi_desc()
 */
static int msix_setup_msi_descs(struct pci_dev *dev, struct msix_entry *entries,
				int nvec, struct irq_affinity_desc *masks)
{
	/* [한국어] ret 는 삽입 결과, i 는 반복자, vec_count 는 하드웨어 테이블의 실제 항목 수.
	 * 마지막 값이 아래 is_virtual 판정의 기준이 된다. */
	int ret = 0, i, vec_count = pci_msix_vec_count(dev);
	/* [한국어] affinity 마스크 배열을 훑을 커서. */
	struct irq_affinity_desc *curmsk;
	/* [한국어] 매번 채워 넣을 임시 descriptor. 하나를 재사용한다. */
	struct msi_desc desc;

	/* [한국어] 쓰지 않는 필드가 쓰레기값으로 남지 않게 지운다. */
	memset(&desc, 0, sizeof(desc));

	/* [한국어] 요청 개수만큼 돌면서 마스크 커서도 함께 민다. */
	for (i = 0, curmsk = masks; i < nvec; i++, curmsk++) {
		/* [한국어] 드라이버가 항목 번호를 지정했으면 그것을, 아니면 0,1,2,... 를 쓴다. */
		desc.msi_index = entries ? entries[i].entry : i;
		/* [한국어] 이번 벡터의 CPU 배분 마스크. 배열이 없으면 NULL 이다. */
		desc.affinity = masks ? curmsk : NULL;
		/* [한국어] 항목 번호가 하드웨어 테이블 크기 이상이면 실체가 없는 가상 항목이다.
		 * PCI_IRQ_VIRTUAL 로 요청했을 때만 생기며, 이후 MMIO 접근을 건너뛴다. */
		desc.pci.msi_attrib.is_virtual = desc.msi_index >= vec_count;

		/* [한국어] 나머지 필드(is_msix, is_64, mask_base, can_mask 등)를 채운다. */
		msix_prepare_msi_desc(dev, &desc);

		/* [한국어] 커널 공통 MSI 계층의 저장소에 복사해 넣는다. */
		ret = msi_insert_msi_desc(&dev->dev, &desc);
		/* [한국어] 삽입 실패(대개 -ENOMEM). */
		if (ret)
			/* [한국어] 중간에 그만둔다. 그때까지 넣은 것은 호출자 쪽 자동 해제가 걷어낸다. */
			break;
	}
	/* [한국어] 마지막 삽입 결과가 그대로 성패다. */
	return ret;
}

/* [한국어]
 * msix_update_entries - 드라이버가 넘긴 msix_entry 배열에 배정된 virq 를 채운다
 *
 * @dev:     대상 PCI 장치.
 * @entries: 드라이버 배열. NULL 이면 아무 일도 하지 않는다.
 * @return: 없음.
 *
 * pci_enable_msix_range() 계열의 옛 API 는 드라이버가 struct msix_entry
 * 배열을 넘기고, 커널이 그 안의 vector 필드에 Linux virq 번호를 되돌려
 * 주는 규약이다. 그 되돌림을 여기서 한다.
 *
 * descriptor 목록 순회 순서와 배열 순서가 같다는 전제인데, 바로 앞
 * msix_setup_msi_descs() 가 배열 순서대로 넣었으므로 성립한다.
 * NULL 검사가 앞에 있는 이유는 pci_alloc_irq_vectors 계열의 새 API 는
 * 배열을 넘기지 않고 나중에 pci_irq_vector() 로 물어보기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msi_descs_lock 안에서 불린다.
 *
 * 호출 체인:
 *   __msix_setup_interrupts() -> [이 함수] -> msi_for_each_desc()
 */
static void msix_update_entries(struct pci_dev *dev, struct msix_entry *entries)
{
	/* [한국어] 순회 커서. */
	struct msi_desc *desc;

	/* [한국어] 옛 API 로 배열을 넘긴 경우에만 되돌려 줄 것이 있다.
	 * pci_alloc_irq_vectors 계열은 나중에 pci_irq_vector() 로 물어보므로 NULL 이다. */
	if (entries) {
		/* [한국어] descriptor 목록을 순서대로 훑는다. 바로 앞 msix_setup_msi_descs() 가
		 * 배열 순서대로 넣었으므로 두 순서가 일치한다. */
		msi_for_each_desc(desc, &dev->dev, MSI_DESC_ALL) {
			/* [한국어] 배정된 Linux 가상 IRQ 번호를 드라이버 배열에 적어 준다. */
			entries->vector = desc->irq;
			/* [한국어] 배열 커서를 다음 칸으로. */
			entries++;
		}
	}
}

/* [한국어]
 * msix_mask_all - MSI-X 테이블의 모든 항목을 마스크한다
 *
 * @base:  ioremap 된 MSI-X 테이블의 시작 가상 주소.
 * @tsize: 테이블 항목 수(Message Control 의 Table Size + 1).
 * @return: 없음.
 *
 * 항목마다 Vector Control 워드에 마스크 비트만 세운 값을 통째로 써 넣는다.
 * read-modify-write 가 아니라 덮어쓰기인 것이 의도다 - 이 시점의 목표는
 * "무슨 값이 남아 있든 전부 막는다" 이기 때문이다.
 *
 * 필요한 이유가 두 가지다. 하나는 새로 켠 직후 아직 채우지 않은 항목이
 * 인터럽트를 내는 것을 막는 것. 다른 하나는 kdump 로 넘어온 crash 커널에서
 * 앞 커널이 남긴 항목이 살아 있는 경우다 - 그 항목의 목적지 주소는 이제
 * 아무 의미가 없으므로 반드시 막아야 한다.
 *
 * 항목 하나가 PCI_MSIX_ENTRY_SIZE(16바이트)이므로 포인터를 그만큼씩 민다.
 * 실행 컨텍스트: 프로세스 컨텍스트. MMIO 쓰기만 한다.
 *
 * 호출 체인:
 *   msix_capability_init() -> [이 함수] -> writel()
 */
static void msix_mask_all(void __iomem *base, int tsize)
{
	/* [한국어] 마스크 비트만 세운 Vector Control 값. 모든 항목에 이 값을 통째로 쓴다.
	 * 읽고-고치기가 아니라 덮어쓰기인 것이 의도다 — 무슨 값이 남아 있든 막는다. */
	u32 ctrl = PCI_MSIX_ENTRY_CTRL_MASKBIT;
	/* [한국어] 항목 반복자. */
	int i;

	/* [한국어] 항목 하나가 16바이트(PCI_MSIX_ENTRY_SIZE)이므로 base 를 그만큼씩 민다. */
	for (i = 0; i < tsize; i++, base += PCI_MSIX_ENTRY_SIZE)
		/* [한국어] 항목의 12번 오프셋인 Vector Control 에 마스크 값을 쓴다. */
		writel(ctrl, base + PCI_MSIX_ENTRY_VECTOR_CTRL);
}

/* [한국어] cleanup.h 의 자동 해제 클래스 정의. 아래 __msix_setup_interrupts() 에서
 * __free(free_msi_irqs) 로 쓰이며, 포인터가 NULL 이 아닌 채 스코프를 벗어나면
 * pci_free_msi_irqs() 가 자동으로 불린다. goto 사슬 없이 되돌리기를 표현한다. */
DEFINE_FREE(free_msi_irqs, struct pci_dev *, if (_T) pci_free_msi_irqs(_T));

/* [한국어]
 * __msix_setup_interrupts - descriptor 생성 -> IRQ 할당 -> 검증 -> 결과 반환
 *
 * @__dev:   대상 PCI 장치. 이름 앞에 밑줄이 붙은 이유는 아래 참조.
 * @entries: 항목 번호 배열(선택).
 * @nvec:    벡터 수.
 * @masks:   미리 만들어 둔 affinity 마스크 배열(선택).
 * @return: 0 성공, 음수 errno 실패.
 *
 * 정리 방식이 특이하다. 인자 __dev 를 지역변수 dev 에 넣으면서
 * __free(free_msi_irqs) 를 걸어, 함수를 어떻게 빠져나가든
 * pci_free_msi_irqs() 가 자동으로 불리게 만든다. 성공했을 때만 마지막에
 * retain_and_null_ptr(dev) 로 포인터를 비워 그 자동 해제를 취소한다.
 * goto 사슬 없이 "실패하면 전부 되돌린다" 를 표현하는 방식이다.
 *
 * 단계는 넷이다 - msi_desc 를 nvec 개 만들고(msix_setup_msi_descs),
 * irqdomain 에 넘겨 실제 virq 와 message 를 받고(pci_msi_setup_msi_irqs),
 * 받은 주소가 장치의 msi_addr_mask 를 넘지 않는지 확인하고
 * (msi_verify_entries), 드라이버 배열에 virq 를 채워 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msi_descs_lock 은 호출자가 이미 잡았다.
 *
 * 호출 체인:
 *   msix_setup_interrupts() -> [이 함수]
 *     -> msix_setup_msi_descs() -> pci_msi_setup_msi_irqs() [irqdomain.c]
 *     -> msi_verify_entries() -> msix_update_entries()
 */
static int __msix_setup_interrupts(struct pci_dev *__dev, struct msix_entry *entries,
				   int nvec, struct irq_affinity_desc *masks)
{
	/* [한국어] 인자를 지역 변수로 옮기면서 자동 해제를 건다. 이 시점부터 어느 경로로
	 * 빠져나가도 pci_free_msi_irqs() 가 실행된다. 성공했을 때만 아래에서 취소한다. */
	struct pci_dev *dev __free(free_msi_irqs) = __dev;

	/* [한국어] 요청 개수만큼 descriptor 를 만들어 저장소에 넣는다. */
	int ret = msix_setup_msi_descs(dev, entries, nvec, masks);
	/* [한국어] 만들기 실패. */
	if (ret)
		/* [한국어] 돌아가는 순간 자동 해제가 지금까지 넣은 것을 걷어낸다. */
		return ret;

	/* [한국어] irqdomain 에 벡터를 등록해 virq 와 목적지 주소/데이터를 받는다.
	 * PCI_CAP_ID_MSIX 로 어느 방식인지 알려 준다. */
	ret = pci_msi_setup_msi_irqs(dev, nvec, PCI_CAP_ID_MSIX);
	/* [한국어] 등록 실패. */
	if (ret)
		/* [한국어] 역시 자동 해제에 맡기고 돌아간다. */
		return ret;

	/* Check if all MSI entries honor device restrictions */
	/* [한국어] 아키텍처가 정한 주소가 장치의 msi_addr_mask 를 지키는지 확인. */
	ret = msi_verify_entries(dev);
	/* [한국어] 범위를 벗어난 항목이 있었다. */
	if (ret)
		/* [한국어] 자동 해제에 맡긴다. */
		return ret;

	/* [한국어] 드라이버가 넘긴 배열이 있으면 virq 를 채워 준다. */
	msix_update_entries(dev, entries);
	/* [한국어] 여기까지 왔으면 성공이다. 포인터를 NULL 로 만들어 자동 해제를 취소한다.
	 * 이 한 줄이 "실패하면 되돌리고 성공하면 유지한다" 를 완성한다. */
	retain_and_null_ptr(dev);
	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * msix_setup_interrupts - affinity 마스크를 만들고 락을 잡은 뒤 본체를 부른다
 *
 * @dev:     대상 PCI 장치.
 * @entries: 항목 번호 배열(선택).
 * @nvec:    벡터 수.
 * @affd:    affinity 배분 규칙. NULL 이면 마스크를 만들지 않는다.
 * @return: __msix_setup_interrupts() 의 반환값 그대로.
 *
 * 껍데기처럼 보이지만 두 가지 자원 수명을 맡고 있다.
 *   - irq_create_affinity_masks() 가 kmalloc 으로 만든 마스크 배열을
 *     __free(kfree) 로 묶어, 어느 경로로 빠져나가도 새지 않게 한다.
 *   - guard(msi_descs_lock) 으로 이 장치의 msi_desc 목록을 잠근다.
 *     descriptor 를 넣고 IRQ 를 붙이는 사이에 다른 문맥이 끼어들면
 *     목록이 깨지기 때문이다.
 * 이 정리를 본체와 분리해 두면 본체는 성공 경로만 쓰면 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(뮤텍스와 GFP_KERNEL 할당).
 *
 * 호출 체인:
 *   msix_capability_init() -> [이 함수] -> __msix_setup_interrupts()
 */
static int msix_setup_interrupts(struct pci_dev *dev, struct msix_entry *entries,
				 int nvec, struct irq_affinity *affd)
{
	/* [한국어] affinity 마스크 배열. __free(kfree) 로 묶어 어느 경로로 나가도 해제된다. */
	struct irq_affinity_desc *masks __free(kfree) =
		/* [한국어] 배분 규칙이 없으면 만들 것도 없다. */
		affd ? irq_create_affinity_masks(nvec, affd) : NULL;

	/* [한국어] 이 장치의 descriptor 목록을 잠근다. 넣고 붙이는 사이에 끼어들면 목록이 깨진다.
	 * guard 라 함수를 나갈 때 자동으로 풀린다. */
	guard(msi_descs_lock)(&dev->dev);
	/* [한국어] 자원 수명 관리를 끝냈으니 본체로 넘긴다. */
	return __msix_setup_interrupts(dev, entries, nvec, masks);
}

/* [한국어]
 * msix_capability_init - MSI-X 를 실제로 켜는 핵심 절차
 *
 * @dev:     대상 PCI 장치.
 * @entries: 항목 번호 배열(선택).
 * @nvec:    이번에 시도할 벡터 수.
 * @affd:    affinity 배분 규칙(선택).
 * @return: 0 성공. 음수 errno 는 실패. 양수는 "이만큼으로 줄이면 될 것 같다" 는
 *          제안이라 호출자가 그 수로 다시 부른다.
 *
 * 순서가 전부인 함수다. 왜 이 순서여야 하는지가 중요하다.
 *   1) Function Mask 와 MSI-X Enable 을 동시에 올린다. 일부 장치는 Enable
 *      전에는 MSI-X 테이블 접근 자체를 거부하고, 그렇다고 마스크 없이 켜면
 *      아직 채우지 않은 테이블 내용으로 인터럽트가 튄다. 그래서 둘을 함께.
 *   2) dev->msix_enabled 를 먼저 1 로 둔다. 뒤이어 부르는 함수들이 이 값을
 *      보고 동작을 고르기 때문에, 하드웨어보다 소프트웨어 표시가 앞선다.
 *   3) Message Control 의 Table Size 로 테이블 크기를 알아내 ioremap.
 *   4) 벡터를 등록(msix_setup_interrupts).
 *   5) INTx 를 끈다.
 *   6) 테이블 전체를 마스크. 늦게 하는 이유가 상류 주석에 있다 - 마스크
 *      비트를 MSI-X 비활성 상태에서도 반영해 버리는 고장난 Marvell 장치가
 *      있어서, 켠 뒤에 해야 한다.
 *   7) Function Mask 를 내려 실제로 개통.
 *
 * 실패하면 out_unmap/out_disable 로 3)과 1)을 역순으로 되돌린다.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioremap 과 메모리 할당이 있다).
 *
 * 호출 체인:
 *   __pci_enable_msix_range() -> [이 함수]
 *     -> msix_map_region() -> msix_setup_interrupts() -> msix_mask_all()
 */
/**
 * msix_capability_init - configure device's MSI-X capability
 * @dev: pointer to the pci_dev data structure of MSI-X device function
 * @entries: pointer to an array of struct msix_entry entries
 * @nvec: number of @entries
 * @affd: Optional pointer to enable automatic affinity assignment
 *
 * Setup the MSI-X capability structure of device function with a
 * single MSI-X IRQ. A return of zero indicates the successful setup of
 * requested MSI-X entries with allocated IRQs or non-zero for otherwise.
 **/
static int msix_capability_init(struct pci_dev *dev, struct msix_entry *entries,
				int nvec, struct irq_affinity *affd)
{
	/* [한국어] ret 는 하위 호출 결과, tsize 는 하드웨어 테이블 항목 수. */
	int ret, tsize;
	/* [한국어] Message Control 값을 담을 곳. */
	u16 control;

	/*
	 * Some devices require MSI-X to be enabled before the MSI-X
	 * registers can be accessed.  Mask all the vectors to prevent
	 * interrupts coming in before they're fully set up.
	 */
	/* [한국어] Function Mask 와 MSI-X Enable 을 동시에 올린다. 일부 장치는 Enable 전에
	 * 테이블 접근 자체를 거부하고, 마스크 없이 켜면 빈 테이블로 인터럽트가 튄다. */
	pci_msix_clear_and_set_ctrl(dev, 0, PCI_MSIX_FLAGS_MASKALL |
				    /* [한국어] 그래서 두 비트를 한 번에 세운다. */
				    PCI_MSIX_FLAGS_ENABLE);

	/* Mark it enabled so setup functions can query it */
	/* [한국어] 소프트웨어 표시를 하드웨어보다 먼저 세운다.
	 * 뒤이어 부르는 함수들이 이 값을 보고 동작을 고르기 때문이다. */
	dev->msix_enabled = 1;

	/* [한국어] Message Control 을 읽는다. */
	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &control);
	/* Request & Map MSI-X table region */
	/* [한국어] 하위 11비트 Table Size 는 0-기반이라 +1 을 해야 실제 개수가 된다
	 * (msi.h 의 msix_table_size 매크로). */
	tsize = msix_table_size(control);
	/* [한국어] Table Offset/BIR 을 해석해 테이블 구간을 ioremap 한다. */
	dev->msix_base = msix_map_region(dev, tsize);
	/* [한국어] 매핑 실패. */
	if (!dev->msix_base) {
		/* [한국어] 메모리 부족으로 보고, */
		ret = -ENOMEM;
		/* [한국어] 1번 단계에서 올린 두 비트를 되돌리는 경로로. */
		goto out_disable;
	}

	/* [한국어] descriptor 를 만들고 벡터를 붙인다. */
	ret = msix_setup_interrupts(dev, entries, nvec, affd);
	/* [한국어] 실패. */
	if (ret)
		/* [한국어] 매핑까지 되돌려야 하므로 out_unmap 으로. */
		goto out_unmap;

	/* Disable INTX */
	/* [한국어] INTx 를 끈다. 두 방식이 동시에 살아 있으면 인터럽트가 두 번 온다. */
	pci_intx_for_msi(dev, 0);

	/* [한국어] 도메인이 마스킹을 금지하지 않았다면 */
	if (!pci_msi_domain_supports(dev, MSI_FLAG_NO_MASK, DENY_LEGACY)) {
		/*
		 * Ensure that all table entries are masked to prevent
		 * stale entries from firing in a crash kernel.
		 *
		 * Done late to deal with a broken Marvell NVME device
		 * which takes the MSI-X mask bits into account even
		 * when MSI-X is disabled, which prevents MSI delivery.
		 */
		/* [한국어] 테이블 전체를 마스크한다. 늦게 하는 이유가 상류 주석에 있다 — 마스크
		 * 비트를 MSI-X 비활성 상태에서도 반영해 버리는 고장난 Marvell 장치 때문이다. */
		msix_mask_all(dev->msix_base, tsize);
	}
	/* [한국어] Function Mask 를 내려 실제로 개통한다. Enable 은 그대로 둔다. */
	pci_msix_clear_and_set_ctrl(dev, PCI_MSIX_FLAGS_MASKALL, 0);

	/* [한국어] 아키텍처가 INTx 용으로 잡아 두었던 IRQ 자원을 반납한다. */
	pcibios_free_irq(dev);
	/* [한국어] 성공. */
	return 0;

/* [한국어] 벡터 등록에 실패한 경로. */
out_unmap:
	/* [한국어] ioremap 을 되돌린다. */
	iounmap(dev->msix_base);
/* [한국어] 매핑에 실패한 경로는 여기로 바로 온다. */
out_disable:
	/* [한국어] 소프트웨어 표시를 내리고 */
	dev->msix_enabled = 0;
	/* [한국어] 1번 단계에서 올린 Function Mask 와 Enable 을 함께 내린다. */
	pci_msix_clear_and_set_ctrl(dev, PCI_MSIX_FLAGS_MASKALL | PCI_MSIX_FLAGS_ENABLE, 0);

	/* [한국어] 실패 원인을 그대로 위로 전한다. */
	return ret;
}

/* [한국어]
 * pci_msix_validate_entries - 드라이버가 지정한 MSI-X 항목 번호 배열을 검사한다
 *
 * @dev:     대상 PCI 장치(도메인 능력을 물어보기 위해 필요).
 * @entries: 항목 번호 배열. NULL 이면 검사할 것이 없어 곧바로 true.
 * @nvec:    배열 길이.
 * @return: true 면 사용 가능, false 면 __pci_enable_msix_range() 가 -EINVAL.
 *
 * 두 가지를 본다.
 *   1) 중복 - 같은 테이블 항목을 두 번 요청하면 나중 것이 앞 것을 덮어써
 *      벡터 하나가 조용히 사라진다. O(n^2) 이중 루프로 잡는다.
 *   2) 구멍 - 도메인이 MSI_FLAG_MSIX_CONTIGUOUS 를 걸어 두었으면 항목 번호가
 *      반드시 0,1,2,... 여야 한다. 그런 도메인은 벡터를 연속 블록으로만
 *      할당할 수 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 계산이라 하드웨어를 만지지 않는다.
 *
 * 호출 체인:
 *   __pci_enable_msix_range() -> [이 함수] -> pci_msi_domain_supports() [irqdomain.c]
 */
static bool pci_msix_validate_entries(struct pci_dev *dev, struct msix_entry *entries, int nvec)
{
	/* [한국어] 항목 번호가 반드시 연속이어야 하는 도메인인가. */
	bool nogap;
	/* [한국어] 이중 루프의 반복자. */
	int i, j;

	/* [한국어] 드라이버가 배열을 안 넘겼으면 검사할 것이 없다. */
	if (!entries)
		/* [한국어] 통과. */
		return true;

	/* [한국어] MSI_FLAG_MSIX_CONTIGUOUS — 벡터를 연속 블록으로만 할당할 수 있는 도메인. */
	nogap = pci_msi_domain_supports(dev, MSI_FLAG_MSIX_CONTIGUOUS, DENY_LEGACY);

	/* [한국어] 모든 항목을 하나씩 본다. */
	for (i = 0; i < nvec; i++) {
		/* Check for duplicate entries */
		/* [한국어] 자기 뒤쪽만 비교하면 모든 쌍을 한 번씩 본다. */
		for (j = i + 1; j < nvec; j++) {
			/* [한국어] 같은 테이블 항목을 두 번 요청했다. 나중 것이 앞 것을 덮어써
			 * 벡터 하나가 조용히 사라진다. */
			if (entries[i].entry == entries[j].entry)
				/* [한국어] 거절. */
				return false;
		}
		/* Check for unsupported gaps */
		/* [한국어] 연속만 허용하는 도메인인데 번호가 0,1,2,... 가 아니면 구멍이 있다. */
		if (nogap && entries[i].entry != i)
			/* [한국어] 거절. */
			return false;
	}
	/* [한국어] 모든 검사 통과. */
	return true;
}

/* [한국어]
 * __pci_enable_msix_range - 요청 범위 안에서 MSI-X 벡터를 최대한 확보한다
 *
 * @dev:     대상 PCI 장치.
 * @entries: 드라이버가 항목 번호를 직접 지정하는 경우의 배열. NULL 이면
 *           0..nvec-1 을 차례로 쓴다.
 * @minvec:  이만큼도 못 얻으면 실패로 본다.
 * @maxvec:  최대 희망 개수.
 * @affd:    자동 CPU affinity 배분 규칙. NULL 이면 배분하지 않는다.
 * @flags:   PCI_IRQ_VIRTUAL 이면 하드웨어 테이블보다 많은 항목을 허용한다
 *           (VF 에 벡터를 미리 잡아 두는 용도).
 * @return: 실제로 확보한 벡터 수(양수), 또는 음수 errno.
 *          -ERANGE 범위 역전, -EINVAL 상태/인자 오류, -ENOTSUPP 도메인 미지원,
 *          -ENOSPC 최소치 미달, -ENODEV 장치 도메인 생성 실패.
 *
 * api.c 의 공개 API 뒤에 있는 MSI-X 쪽 실제 구현이다. 하는 일은 두 가지다.
 * 먼저 켤 수 있는 상태인지 전부 검사한다 - MSI 가 이미 켜져 있지 않은지,
 * 이미 MSI-X 가 켜져 있지 않은지, D0 인지, 도메인이 MSI-X 를 지원하는지,
 * 하드웨어 테이블 크기(pci_msix_vec_count)가 몇인지, 항목 번호 배열이
 * 유효한지. 그다음 msix_capability_init() 를 부르는데, 이것이 "요청이 너무
 * 많다" 는 뜻으로 양수를 돌려주면 그 수로 낮춰 다시 시도하는 루프를 돈다.
 *
 * MSI 와 달리 벡터 수가 2의 거듭제곱일 필요도, 연속일 필요도 없다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_alloc_irq_vectors_affinity() / pci_enable_msix_range() [api.c]
 *     -> [이 함수] -> msix_capability_init() -> msix_setup_interrupts()
 */
int __pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries, int minvec,
			    int maxvec, struct irq_affinity *affd, int flags)
{
	/* [한국어] hwsize 는 하드웨어 테이블 크기, nvec 은 이번 회차 시도 개수(상한에서 시작). */
	int hwsize, rc, nvec = maxvec;

	/* [한국어] 범위가 뒤집혔다. */
	if (maxvec < minvec)
		/* [한국어] -ERANGE 로 구분한다. */
		return -ERANGE;

	/* [한국어] MSI 가 이미 켜져 있으면 MSI-X 를 켤 수 없다. 하드웨어가 둘 중 하나만 허용한다. */
	if (dev->msi_enabled) {
		/* [한국어] 드라이버가 알아볼 수 있게 이유를 남긴다. */
		pci_info(dev, "can't enable MSI-X (MSI already enabled)\n");
		/* [한국어] 거절. */
		return -EINVAL;
	}

	/* [한국어] 이미 MSI-X 가 켜져 있는데 또 켜려는 것은 호출자 버그다. */
	if (WARN_ON_ONCE(dev->msix_enabled))
		/* [한국어] WARN 을 남기고 거절. */
		return -EINVAL;

	/* Check MSI-X early on irq domain enabled architectures */
	/* [한국어] 도메인이 PCI MSI-X 를 다룰 수 있는가. */
	if (!pci_msi_domain_supports(dev, MSI_FLAG_PCI_MSIX, ALLOW_LEGACY))
		/* [한국어] 못 다루면 지원하지 않음. */
		return -ENOTSUPP;

	/* [한국어] 전역/장치/버스 차원의 MSI 허용 여부와 D0 상태를 함께 본다. */
	if (!pci_msi_supported(dev, nvec) || dev->current_state != PCI_D0)
		/* [한국어] 인자/상태 오류로 거절. */
		return -EINVAL;

	/* [한국어] Message Control 의 Table Size 로 하드웨어 항목 수를 구한다(api.c). */
	hwsize = pci_msix_vec_count(dev);
	/* [한국어] capability 가 없으면 음수. */
	if (hwsize < 0)
		/* [한국어] 그 errno 를 그대로 전한다. */
		return hwsize;

	/* [한국어] 드라이버가 지정한 항목 번호 배열에 중복이나 금지된 구멍이 있는지 본다. */
	if (!pci_msix_validate_entries(dev, entries, nvec))
		/* [한국어] 인자 오류. */
		return -EINVAL;

	/* [한국어] 하드웨어가 가진 항목보다 많이 요청한 경우. */
	if (hwsize < nvec) {
		/* Keep the IRQ virtual hackery working */
		/* [한국어] PCI_IRQ_VIRTUAL 이면 실체 없는 항목도 허용한다(VF 용 미리 잡기). */
		if (flags & PCI_IRQ_VIRTUAL)
			/* [한국어] 그래서 도메인 크기를 요청에 맞춰 늘린다. */
			hwsize = nvec;
		/* [한국어] 평범한 요청이면 */
		else
			/* [한국어] 하드웨어가 줄 수 있는 만큼으로 낮춘다. */
			nvec = hwsize;
	}

	/* [한국어] 낮춘 결과가 최소치에도 못 미치면 */
	if (nvec < minvec)
		/* [한국어] 자리 부족. */
		return -ENOSPC;

	/* [한국어] descriptor 저장소와 devm 해제 액션을 준비한다. */
	rc = pci_setup_msi_context(dev);
	/* [한국어] 준비 실패. */
	if (rc)
		/* [한국어] 그대로 전한다. */
		return rc;

	/* [한국어] 이 장치 전용 MSI-X irq_domain 을 세운다. 크기는 hwsize 다. */
	if (!pci_setup_msix_device_domain(dev, hwsize))
		/* [한국어] 세우지 못하면 벡터를 붙일 곳이 없다. */
		return -ENODEV;

	/* [한국어] 성공하거나 확실히 실패할 때까지 개수를 줄여 가며 반복한다. */
	for (;;) {
		/* [한국어] CPU 배분 규칙이 있으면 */
		if (affd) {
			/* [한국어] 그 규칙에서 의미 있는 최대 벡터 수를 다시 구한다. */
			nvec = irq_calc_affinity_vectors(minvec, nvec, affd);
			/* [한국어] 최소치에 못 미치면 */
			if (nvec < minvec)
				/* [한국어] 자리 부족으로 포기. */
				return -ENOSPC;
		}

		/* [한국어] 실제로 켜 본다. */
		rc = msix_capability_init(dev, entries, nvec, affd);
		/* [한국어] 0 이면 성공. */
		if (rc == 0)
			/* [한국어] 확보한 개수를 돌려준다. */
			return nvec;

		/* [한국어] 음수는 진짜 실패. */
		if (rc < 0)
			/* [한국어] 그대로 전한다. */
			return rc;
		/* [한국어] 양수인데 최소치보다 작으면 더 줄여도 소용없다. */
		if (rc < minvec)
			/* [한국어] 자리 부족. */
			return -ENOSPC;

		/* [한국어] 제안받은 수로 낮춰 다시 돈다. */
		nvec = rc;
	}
}

/* [한국어]
 * __pci_restore_msix_state - 전원이 끊겼다 돌아온 뒤 MSI-X 를 다시 세운다
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * MSI-X 테이블은 장치 안의 메모리라 D3cold 등으로 전원이 끊기면 내용이
 * 사라진다. 커널은 벡터마다 msi_desc 에 최종 message 와 Vector Control 값을
 * 캐시해 두었으므로, 여기서 그것을 그대로 다시 써 넣는다.
 *
 * 순서:
 *   1) INTx 를 끈다 - 복원 도중 두 경로가 동시에 살아 있으면 안 된다
 *   2) MSI-X Enable 과 Function Mask 를 동시에 올린다. Function Mask 를 함께
 *      올려 두어야 테이블을 채우는 동안 인터럽트가 새지 않는다
 *   3) 벡터마다 message 와 msix_ctrl 을 다시 기록
 *   4) Function Mask 를 내려 실제로 열어 준다
 *
 * arch_restore_msi_irqs() 가 false 를 주는 아키텍처에서는 message 재기록을
 * 건너뛴다 - 그 아키텍처가 이미 자기 방식으로 복원했다는 뜻이다.
 * 실행 컨텍스트: 프로세스 컨텍스트(resume 경로). msi_descs_lock 을 잡는다.
 *
 * 호출 체인:
 *   pci_restore_state() -> pci_restore_msi_state() [api.c]
 *     -> [이 함수] -> __pci_write_msi_msg() / pci_msix_write_vector_ctrl()
 */
void __pci_restore_msix_state(struct pci_dev *dev)
{
	/* [한국어] 복원할 벡터를 훑을 커서. */
	struct msi_desc *entry;
	/* [한국어] message 를 커널이 직접 다시 써야 하는가. */
	bool write_msg;

	/* [한국어] 켜져 있지 않았다면 복원할 것이 없다. */
	if (!dev->msix_enabled)
		/* [한국어] 그대로 돌아간다. */
		return;

	/* route the table */
	/* [한국어] INTx 를 끈다. 복원 도중 두 경로가 동시에 살아 있으면 안 된다. */
	pci_intx_for_msi(dev, 0);
	/* [한국어] Enable 과 Function Mask 를 함께 올린다. Function Mask 를 같이 올려야
	 * 테이블을 채우는 동안 인터럽트가 새지 않는다. */
	pci_msix_clear_and_set_ctrl(dev, 0,
				PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL);

	/* [한국어] 아키텍처가 이미 자기 방식으로 복원했으면 false 가 온다. */
	write_msg = arch_restore_msi_irqs(dev);

	/* [한국어] descriptor 목록을 잠근 블록. 블록을 벗어날 때 자동으로 풀린다. */
	scoped_guard (msi_descs_lock, &dev->dev) {
		/* [한국어] 이 장치의 모든 벡터를 훑는다. */
		msi_for_each_desc(entry, &dev->dev, MSI_DESC_ALL) {
			/* [한국어] 커널이 다시 써야 하는 경우에만 */
			if (write_msg)
				/* [한국어] 캐시해 둔 message 를 테이블 항목에 되쓴다. */
				__pci_write_msi_msg(entry, &entry->msg);
			/* [한국어] Vector Control 캐시값도 되쓴다. 어느 벡터가 마스크돼 있었는지까지 복원한다. */
			pci_msix_write_vector_ctrl(entry, entry->pci.msix_ctrl);
		}
	}

	/* [한국어] Function Mask 를 내려 실제로 개통한다. */
	pci_msix_clear_and_set_ctrl(dev, PCI_MSIX_FLAGS_MASKALL, 0);
}

/* [한국어]
 * pci_msix_shutdown - MSI-X 를 끄고 장치를 INTx 상태로 되돌린다
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * msix_capability_init() 의 역순이다. 모든 항목을 다시 마스크하고, Message
 * Control 의 MSI-X Enable 을 내리고, INTx 를 되살리고, dev->irq 를 아키텍처가
 * 다시 배정하게 한다(pcibios_alloc_irq).
 *
 * 항목을 먼저 마스크하는 이유는 "초기 상태로 돌려놓기" 다. MSI-X Enable 만
 * 내리면 테이블 내용이 남아, 다음에 켰을 때 옛 주소로 인터럽트가 튈 수 있다.
 *
 * 장치가 이미 뽑힌(disconnected) 경우에는 config/MMIO 접근이 전부 0xff 를
 * 돌려주므로 하드웨어를 만지지 않고 소프트웨어 상태만 내린다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_disable_msix() / pci_free_irq_vectors() [api.c]
 *     -> [이 함수] -> pci_msix_mask() [msi.h] / pci_msix_clear_and_set_ctrl()
 *                  -> pci_intx_for_msi() -> pcibios_alloc_irq()
 */
void pci_msix_shutdown(struct pci_dev *dev)
{
	/* [한국어] 마스크할 벡터를 훑을 커서. */
	struct msi_desc *desc;

	/* [한국어] 전역으로 꺼져 있거나 장치가 없거나 애초에 MSI-X 가 아니면 할 일이 없다. */
	if (!pci_msi_enable || !dev || !dev->msix_enabled)
		/* [한국어] 그대로 돌아간다. */
		return;

	/* [한국어] 장치가 이미 뽑혔으면 config/MMIO 접근이 전부 0xff 를 준다. */
	if (pci_dev_is_disconnected(dev)) {
		/* [한국어] 하드웨어를 만지지 않고 소프트웨어 표시만 내린다. */
		dev->msix_enabled = 0;
		/* [한국어] 바로 돌아간다. */
		return;
	}

	/* Return the device with MSI-X masked as initial states */
	/* [한국어] 모든 항목을 다시 마스크한다. Enable 만 내리면 테이블 내용이 남아
	 * 다음에 켰을 때 옛 주소로 인터럽트가 튈 수 있다. */
	msi_for_each_desc(desc, &dev->dev, MSI_DESC_ALL)
		/* [한국어] Vector Control 의 마스크 비트를 세운다(msi.h). */
		pci_msix_mask(desc);

	/* [한국어] MSI-X Enable 을 내린다. */
	pci_msix_clear_and_set_ctrl(dev, PCI_MSIX_FLAGS_ENABLE, 0);
	/* [한국어] INTx 를 되살린다. */
	pci_intx_for_msi(dev, 1);
	/* [한국어] 소프트웨어 표시도 내린다. */
	dev->msix_enabled = 0;
	/* [한국어] 아키텍처에 INTx IRQ 를 다시 잡아 달라고 알린다. */
	pcibios_alloc_irq(dev);
}

/* Common interfaces */

/* [한국어]
 * pci_free_msi_irqs - 벡터들을 IRQ 계층에서 떼고 MSI-X 테이블 매핑을 해제한다
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * 해제의 마지막 단계다. 앞서 pci_msi_shutdown() / pci_msix_shutdown() 이
 * 하드웨어를 껐고, 여기서는 소프트웨어 자원을 반납한다.
 *   1) pci_msi_teardown_msi_irqs() (irqdomain.c) 로 msi_desc 와 virq 를 해제
 *   2) msix_map_region() 이 ioremap 해 둔 MSI-X 테이블을 iounmap 하고
 *      dev->msix_base 를 NULL 로 (MSI 경로에서는 애초에 NULL 이라 건너뛴다)
 *
 * 순서가 중요하다. 테이블을 먼저 unmap 하면 teardown 도중 마스킹을 시도하는
 * 코드가 해제된 주소를 건드리게 된다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_free_irq_vectors() [api.c] 또는 __msi_capability_init() 의 에러 경로
 *     -> [이 함수] -> pci_msi_teardown_msi_irqs() [irqdomain.c] / iounmap()
 */
void pci_free_msi_irqs(struct pci_dev *dev)
{
	/* [한국어] irqdomain 에서 msi_desc 와 virq 를 해제한다(irqdomain.c).
	 * 테이블 unmap 보다 먼저인 이유는 이 과정에서 마스킹을 시도하는 코드가
	 * 해제된 주소를 건드리면 안 되기 때문이다. */
	pci_msi_teardown_msi_irqs(dev);

	/* [한국어] MSI-X 였다면 매핑이 남아 있다. MSI 경로에서는 NULL 이라 건너뛴다. */
	if (dev->msix_base) {
		/* [한국어] msix_map_region() 이 잡아 둔 테이블 매핑을 반납한다. */
		iounmap(dev->msix_base);
		/* [한국어] 다음에 켤 때 재사용하지 않도록 비운다. */
		dev->msix_base = NULL;
	}
}

/* [한국어] TPH(TLP Processing Hints)가 빌드에 포함될 때만 아래 함수를 컴파일한다.
 * 유일한 호출자인 drivers/pci/tph.c 도 같은 옵션에 묶여 있다. */
#ifdef CONFIG_PCIE_TPH
/* [한국어]
 * pci_msix_write_tph_tag - MSI-X 벡터 하나의 Steering Tag(TPH) 를 갱신한다
 *
 * @pdev:  대상 PCI 장치. MSI-X 가 켜져 있어야 한다(msix_enabled).
 * @index: MSI-X 테이블 항목 번호.
 * @tag:   써 넣을 16비트 Steering Tag.
 * @return: 0 성공. -ENXIO 는 MSI-X 미활성 / virq 없음 / irq_desc 없음 /
 *          msi_desc 없음 또는 가상(is_virtual) 항목인 경우.
 *
 * TPH(TLP Processing Hints)는 요청자가 TLP 에 "이 데이터를 어느 캐시로
 * 보내라" 는 힌트를 붙이는 PCIe 기능이고, Steering Tag 가 그 힌트값이다.
 * MSI-X 항목의 Vector Control 상위 비트(PCI_MSIX_ENTRY_CTRL_ST)가 인터럽트
 * TLP 용 ST 를 담으므로, 갱신 지점이 MSI-X 테이블인 이 파일에 있다.
 *
 * 동시성이 까다롭다. 같은 Vector Control 워드를 마스킹 경로도 만지기
 * 때문에, 이 함수는 msi_descs_lock 과 irq_desc->lock 을 둘 다 잡고
 * desc->pci.msix_ctrl 소프트웨어 캐시와 하드웨어를 함께 갱신한다.
 * 상류 주석이 이 방식을 "horrible hack" 이라 부르는 이유다.
 * 실행 컨텍스트: 프로세스 컨텍스트(뮤텍스를 잡는다).
 *
 * 이 트리에서 확인한 유일한 호출자는 drivers/pci/tph.c:449 의
 * pci_tph_set_st_entry() 이며, drivers/nvme/ 에는 호출이 하나도 없다
 * (전수 grep 확인). 앞선 주석이 "NVMe CMB/P2PDMA 지역성 최적화" 를
 * 말했으나 근거가 없어 삭제했다.
 *
 * 호출 체인:
 *   드라이버 -> pci_tph_set_st_entry() [tph.c]
 *     -> [이 함수] -> pci_msix_write_vector_ctrl() [msi.h] -> writel()
 */
/**
 * pci_msix_write_tph_tag - Update the TPH tag for a given MSI-X vector
 * @pdev:	The PCIe device to update
 * @index:	The MSI-X index to update
 * @tag:	The tag to write
 *
 * Returns: 0 on success, error code on failure
 */
int pci_msix_write_tph_tag(struct pci_dev *pdev, unsigned int index, u16 tag)
{
	/* [한국어] 갱신할 벡터의 descriptor. */
	struct msi_desc *msi_desc;
	/* [한국어] 그 벡터의 IRQ descriptor. 아래 락을 잡기 위해 필요하다. */
	struct irq_desc *irq_desc;
	/* [한국어] index 에 대응하는 Linux 가상 IRQ 번호. */
	unsigned int virq;

	/* [한국어] MSI-X 가 켜져 있지 않으면 만질 테이블이 없다. */
	if (!pdev->msix_enabled)
		/* [한국어] 장치/자원 없음으로 거절. */
		return -ENXIO;

	/* [한국어] 항목 번호로 virq 를 찾는다. */
	virq = msi_get_virq(&pdev->dev, index);
	/* [한국어] 그 항목에 벡터가 붙어 있지 않다. */
	if (!virq)
		/* [한국어] 거절. */
		return -ENXIO;

	/* [한국어] descriptor 목록을 잠근다. 블록을 벗어날 때 자동으로 풀린다. */
	guard(msi_descs_lock)(&pdev->dev);

	/*
	 * This is a horrible hack, but short of implementing a PCI
	 * specific interrupt chip callback and a huge pile of
	 * infrastructure, this is the minor nuisance. It provides the
	 * protection against concurrent operations on this entry and keeps
	 * the control word cache in sync.
	 */
	/* [한국어] virq 로 IRQ descriptor 를 찾는다. 상류 주석이 이 방식을 "horrible hack"
	 * 이라 부르는데, 마스킹 경로와 같은 락을 빌려 쓰기 위한 우회이기 때문이다. */
	irq_desc = irq_to_desc(virq);
	/* [한국어] 없으면 IRQ 계층에 등록되지 않은 번호다. */
	if (!irq_desc)
		/* [한국어] 거절. */
		return -ENXIO;

	/* [한국어] 마스킹 경로와 같은 락을 잡는다. 같은 Vector Control 워드를 두 경로가
	 * 동시에 고치는 것과 msix_ctrl 캐시가 어긋나는 것을 함께 막는다. */
	guard(raw_spinlock_irq)(&irq_desc->lock);
	/* [한국어] 락 안에서 다시 descriptor 를 얻는다. */
	msi_desc = irq_data_get_msi_desc(&irq_desc->irq_data);
	/* [한국어] 없거나 실체 없는 가상 항목이면 쓸 하드웨어가 없다. */
	if (!msi_desc || msi_desc->pci.msi_attrib.is_virtual)
		/* [한국어] 거절. */
		return -ENXIO;

	/* [한국어] 캐시에서 Steering Tag 필드만 지운다. 마스크 비트 등 나머지는 보존한다. */
	msi_desc->pci.msix_ctrl &= ~PCI_MSIX_ENTRY_CTRL_ST;
	/* [한국어] 새 태그를 그 자리에 넣는다. */
	msi_desc->pci.msix_ctrl |= FIELD_PREP(PCI_MSIX_ENTRY_CTRL_ST, tag);
	/* [한국어] 고친 캐시를 Vector Control 워드에 통째로 쓴다. */
	pci_msix_write_vector_ctrl(msi_desc, msi_desc->pci.msix_ctrl);
	/* Flush the write */
	/* [한국어] 읽은 값은 버린다. 앞선 쓰기가 장치에 도달하도록 밀어내는 flush. */
	readl(pci_msix_desc_addr(msi_desc));
	/* [한국어] 성공. */
	return 0;
}
/* [한국어] CONFIG_PCIE_TPH 블록 끝. */
#endif

/* Misc. infrastructure */

/* [한국어]
 * msi_desc_to_pci_dev - msi_desc 가 매달린 struct device 를 pci_dev 로 되돌린다
 *
 * @desc: 벡터 하나를 표현하는 커널 공통 디스크립터.
 * @return: 그 벡터를 소유한 PCI 장치.
 *
 * struct msi_desc 는 PCI 전용이 아니라 플랫폼 장치도 쓰는 공통 구조체라,
 * 안에는 버스 중립적인 struct device 포인터만 있다. PCI 쪽 코드가 config
 * space 나 capability 오프셋을 만지려면 pci_dev 로 되돌려야 하고, 그
 * to_pci_dev() 변환을 이 한 곳에 모아 EXPORT_SYMBOL 로 내보낸다.
 *
 * 실행 컨텍스트: 제약 없음. 포인터 산술 한 번이라 인터럽트 문맥에서도 안전하다.
 *
 * 이 파일 안 호출자: pci_msi_update_mask()(274), __pci_read_msi_msg()(328),
 * __pci_write_msi_msg()(433). 파일 밖에서는 drivers/pci/controller/pci-hyperv.c
 * 등 MSI 도메인을 직접 구현하는 컨트롤러 드라이버가 쓴다.
 *
 * 호출 체인:
 *   MSI 조작 함수 -> [이 함수] -> to_pci_dev() (container_of)
 */
struct pci_dev *msi_desc_to_pci_dev(struct msi_desc *desc)
{
	/* [한국어] struct device 를 감싼 pci_dev 로 되돌린다. container_of 매크로다. */
	return to_pci_dev(desc->dev);
}
/* [한국어] 모듈에 공개(GPL 제한 없음). pci-hyperv.c 등이 쓴다. */
EXPORT_SYMBOL(msi_desc_to_pci_dev);

/* [한국어]
 * pci_no_msi - MSI/MSI-X 사용을 커널 전역에서 끈다
 *
 * @return: 없음.
 *
 * 전역 스위치 pci_msi_enable 을 false 로 내린다. 이 값은 이 파일의
 * pci_msi_supported() 와 pci_msi_shutdown() / pci_msix_shutdown() 이
 * 읽으므로, 이후 모든 장치의 MSI/MSI-X 요청이 거부되고 INTx 만 남는다.
 * 이미 켜진 장치를 되돌리지는 않는다 - 플래그만 내린다.
 *
 * 부팅 초기에 한 번 불리는 것이 전제라 락이 없다.
 * 실행 컨텍스트: 프로세스 컨텍스트(부팅 파라미터 파싱, ACPI 초기화, quirk).
 *
 * 이 트리에서 확인한 호출자 셋:
 *   drivers/pci/pci.c:14006  "pci=nomsi" 부팅 인자 처리
 *   drivers/pci/pci-acpi.c:3510  ACPI FADT 의 NO_MSI 비트
 *   drivers/pci/quirks.c:5892  MSI 가 깨진 칩셋 quirk
 *
 * 호출 체인:
 *   pci_setup() / acpi 초기화 / quirk -> [이 함수] -> pci_msi_enable = false
 */
void pci_no_msi(void)
{
	/* [한국어] 전역 스위치를 내린다. 이후 pci_msi_supported() 가 모든 요청을 거절한다.
	 * 이미 켜진 장치를 되돌리지는 않는다. */
	pci_msi_enable = false;
}
