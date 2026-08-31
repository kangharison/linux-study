// SPDX-License-Identifier: GPL-2.0
/*
 * Xen PCI Frontend
 *
 * Author: Ryan Wilson <hap9@epoch.ncsc.mil>
 */

/*
 * [한국어 설명] Xen 반가상화 PCI 프론트엔드 (xen-pcifront.c)
 *
 * === 파일의 역할 ===
 * Xen PV 게스트(domU) 안에서 "가짜 PCI 호스트 브리지" 역할을 하는 드라이버다.
 * 게스트에 패스스루된 PCI 장치를 보이게 만들되, config 공간 접근을 실제
 * 하드웨어로 보내지 않고 dom0 의 백엔드(pciback)에게 대신 시킨다.
 * 왜 그래야 하는가. config 공간에는 게스트가 마음대로 바꾸면 격리가 깨지는
 * 필드가 많다 - BAR 를 남의 MMIO 위로 옮기거나, Bus Master 를 켜 임의 주소로
 * DMA 를 걸거나, 브리지의 서브버스 번호를 바꿔 남의 장치를 자기 아래로
 * 끌어올 수 있다. 그래서 모든 접근을 하이퍼바이저 쪽 검사대를 거치게 한다.
 * 이 파일이 하는 일은 결국 세 가지다. (1) config 접근을 메시지로 바꿔
 * 백엔드에 넘기고, (2) MSI/MSI-X 벡터 배정을 백엔드에 위임하고,
 * (3) 백엔드가 알려 온 AER 오류를 게스트 안 드라이버의 콜백으로 전달한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 게스트 커널의 PCI 코어 바로 아래, 보통 하드웨어 호스트 브리지 드라이버가
 * 있어야 할 자리에 놓인다. 실행 컨텍스트는 게스트 커널 모듈이다.
 *
 *   게스트 드라이버(예: NVMe) -> PCI 코어 -> drivers/pci/access.c
 *     -> bus->ops->read/write == pcifront_bus_read/write   [이 파일]
 *       -> do_pci_op(): 공유 페이지에 요청을 싣고 이벤트 채널로 알림
 *         -> (하이퍼바이저) -> dom0 의 pciback 이 실제 하드웨어 접근
 *       -> 응답을 공유 페이지에서 읽어 PCIBIOS_* 로 변환해 반환
 *
 * 반대 방향도 있다. 백엔드가 AER 오류를 알리면
 *   이벤트 채널 -> pcifront_handler_aer() -> 워크큐 -> pcifront_do_aer()
 *     -> pcifront_common_process() -> 장치 드라이버의 err_handler 콜백
 *
 * 생명 주기는 XenBus 상태 기계가 지배한다.
 *   probe -> alloc_pdev()        : 공유 페이지 + grant 발급
 *         -> publish_info()      : XenStore 에 grant 참조/이벤트 채널 발행,
 *                                  상태를 Initialised 로
 *   백엔드가 Connected -> pcifront_try_connect() -> pcifront_connect()
 *                                : 루트 버스 생성, 장치 열거, 상태를 Connected 로
 *   백엔드가 Reconfiguring -> pcifront_detach_devices()  : 핫 리무브
 *   백엔드가 Reconfigured  -> pcifront_attach_devices()  : 핫 애드
 *   백엔드가 Closing/Closed -> pcifront_try_disconnect() : 전부 걷어내고 Closed
 *
 * === 타 모듈과의 연결 ===
 * 위쪽(이 파일에 의존하는 쪽): 게스트의 PCI 코어 전체.
 *   drivers/pci/access.c 가 bus->ops->read/write 를 통해 이 파일을 부르고,
 *   drivers/pci/probe.c 의 pci_scan_root_bus()/pci_scan_single_device() 가
 *   그 위에서 장치를 열거한다. drivers/pci/setup-res.c 의 pci_claim_resource() 는
 *   pcifront_claim_resource() 를 통해 쓰인다.
 * 아래쪽(이 파일이 의존하는 쪽): XenBus/XenStore(상태 기계와 파라미터 교환),
 *   grant table(페이지 공유 허가), 이벤트 채널(도메인 간 알림), 워크큐(AER).
 *   해당 헤더들(xen/xenbus.h, xen/events.h, xen/grant_table.h,
 *   xen/interface/io/pciif.h, asm/xen/pci.h)은 이 트리에 들어 있지 않아
 *   내부 정의를 직접 확인할 수는 없다.
 * 데이터 흐름: 요청은 스택의 struct xen_pci_op -> 공유 페이지의 op 슬롯 ->
 *   (백엔드) -> 같은 슬롯 -> 스택으로 되복사. AER 은 반대로
 *   백엔드 -> sh_info->aer_op -> 드라이버 콜백 -> 같은 필드에 결과 기록.
 * 공유 상태: struct xen_pci_sharedinfo 한 페이지. 요청 슬롯(op)과
 *   AER 슬롯(aer_op), 그리고 진행 표시 비트가 든 flags 로 이뤄진다.
 *   이 파일이 접근하는 필드로 볼 때 각 방향의 진행 중 요청은 한 건뿐이며,
 *   그래서 sh_info_lock 스핀락 하나로 전체를 직렬화한다.
 *
 * === 주요 함수/구조체 요약 ===
 * do_pci_op()               : 모든 통신이 지나는 관문. 요청을 공유 페이지에
 *                             싣고 비트를 세운 뒤 응답까지 폴링한다(2초 판정).
 * pcifront_bus_read/write() : struct pci_ops 에 꽂히는 config 접근 구현.
 *                             이 두 함수가 "게스트의 config 접근을 가로채
 *                             하이퍼바이저에 위임한다" 는 이 파일의 요지다.
 * pci_frontend_enable_msix() 계열 : MSI/MSI-X 배정을 백엔드에 위임한다.
 *                             전역 xen_pci_frontend 훅으로 연결된다.
 * pcifront_scan_root()      : pcifront_bus_ops 를 꽂아 PV 루트 버스를 만들고
 *                             모든 devfn 을 훑어 장치를 붙인다.
 * pcifront_do_aer()         : 백엔드가 올린 AER 단계를 드라이버 콜백으로 옮기고
 *                             결과를 되돌려 준다. 워크큐에서 실행된다.
 * pcifront_backend_changed(): XenBus 상태 기계의 진입점.
 * struct pcifront_device    : 이 채널의 모든 상태(공유 페이지, 이벤트 채널,
 *                             IRQ, 루트 버스 목록, AER 워크)를 담는다.
 * struct pcifront_sd        : 버스에서 위 구조체로 되돌아오는 통로.
 *
 * === NVMe 관점 ===
 * 이 파일에는 NVMe 관련 코드가 전혀 없다. 장치 종류를 가리지 않는 계층이다.
 * drivers/nvme 트리를 주석을 제거한 상태로 전수 검색해도 xen, pcifront,
 * xen_pci_frontend, swiotlb 참조는 0건이다. 그러므로 "NVMe 가 이 파일을
 * 호출한다" 같은 서술은 쓰지 않는다.
 *
 * 다만 함수 포인터를 통해 실제로 이어지는 지점이 하나 확인된다. AER 경로다.
 * pcifront_common_process() 는 대상 장치에 바인딩된 드라이버의
 * pdrv->err_handler->error_detected / mmio_enabled / slot_reset / resume 를
 * 부른다. drivers/nvme/host/pci.c 는 nvme_err_handler 를 정의해
 * error_detected, slot_reset, resume, reset_prepare, reset_done 를 채워
 * pci_driver 의 .err_handler 로 등록한다(같은 파일 안에서 확인).
 * 즉 PV 게스트에 넘어간 NVMe 컨트롤러에 백엔드가 AER 단계를 지시하면
 * 그 콜백들이 불리는 구조다.
 *
 * 여기서 코드로 확인되는 사실 하나. 이 파일은 mmio_enabled 와 slot_reset 을
 * NULL 검사 없이 부르는데(검사하는 것은 error_detected 뿐이다),
 * nvme_err_handler 에는 mmio_enabled 가 채워져 있지 않다.
 * 백엔드가 XEN_PCI_OP_aer_mmio 단계를 실제로 보내는지는 이 트리에 백엔드
 * 코드가 없어 확인할 수 없으므로, 그 이상은 단정하지 않는다.
 *
 * 또 하나 학습에 쓸모 있는 지점은 MSI-X 벡터 개수 상한이다.
 * pci_frontend_enable_msix() 는 요청 벡터 수가 SH_INFO_MAX_VEC 를 넘으면
 * 거부한다. 큐마다 인터럽트 벡터를 하나씩 쓰는 장치라면 이 상한이 곧
 * 큐 개수의 상한이 된다. 다만 SH_INFO_MAX_VEC 의 값은 이 트리에 없는
 * xen/interface/io/pciif.h 에 있어 확인할 수 없다.
 */

#include <linux/module.h>	/* [한국어] module_init/module_exit, MODULE_LICENSE 등 모듈 골격 매크로를 위해 필요하다 */
#include <linux/init.h>	/* [한국어] pcifront_init 의 __init, pcifront_cleanup 의 __exit 섹션 표시를 위해 필요하다 */
#include <linux/mm.h>	/* [한국어] 이 파일에서 mm.h 의 심볼을 직접 쓰는 곳은 찾지 못했다. 다른 헤더를 끌어오는 효과로 남아 있는 것으로 보이나, 헤더가 이 트리에 없어 확인할 수 없다 */
#include <xen/xenbus.h>	/* [한국어] XenBus 상태 기계와 XenStore 접근 - xenbus_switch_state, xenbus_scanf, xenbus_printf, xenbus_setup_ring 등이 여기서 온다 */
#include <xen/events.h>	/* [한국어] 이벤트 채널과 그 IRQ 바인딩 - notify_remote_via_evtchn, bind_evtchn_to_irqhandler, xen_poll_irq_timeout, xen_clear_irq_pending */
#include <xen/grant_table.h>	/* [한국어] grant table - 공유 페이지를 백엔드가 매핑하도록 허가하는 grant_ref_t 타입이 여기서 온다 */
#include <xen/page.h>	/* [한국어] Xen 의 페이지 관련 헬퍼. 이 파일에서 직접 쓰는 심볼을 특정하지는 못했고, 헤더가 이 트리에 없어 확인할 수 없다 */
#include <linux/spinlock.h>	/* [한국어] spinlock_t 와 DEFINE_SPINLOCK - 공유 페이지 슬롯과 전역 포인터를 보호한다 */
#include <linux/pci.h>	/* [한국어] PCI 코어 API 전반 - struct pci_ops, pci_scan_root_bus, pci_claim_resource, PCIBIOS_* 코드가 여기서 온다 */
#include <linux/msi.h>	/* [한국어] MSI 서술자 순회 - msi_for_each_desc 와 struct msi_desc 를 위해 필요하다 */
#include <xen/interface/io/pciif.h>	/* [한국어] Xen PV PCI 프로토콜 정의 - struct xen_pci_op, XEN_PCI_OP_*, XEN_PCI_ERR_*, _XEN_PCIF_active, SH_INFO_MAX_VEC, XEN_PCI_MAGIC 이 모두 여기 있다. 이 트리에는 이 헤더가 없어 값들을 직접 확인할 수 없다 */
#include <asm/xen/pci.h>	/* [한국어] 전역 xen_pci_frontend 포인터와 struct xen_pci_frontend_ops 정의. MSI 요청을 PV 경로로 돌리는 훅이다. 역시 이 트리에는 없다 */
#include <linux/interrupt.h>	/* [한국어] irqreturn_t 와 IRQ_HANDLED - AER 인터럽트 핸들러의 반환 타입 */
#include <linux/atomic.h>	/* [한국어] smp_mb__before_atomic / smp_mb__after_atomic 메모리 배리어를 위해 필요하다 */
#include <linux/workqueue.h>	/* [한국어] struct work_struct, INIT_WORK, schedule_work, cancel_work_sync - AER 을 프로세스 문맥으로 미루는 데 쓴다 */
#include <linux/bitops.h>	/* [한국어] test_bit, set_bit, clear_bit, test_and_set_bit - 공유 flags 와 pdev->flags 조작에 쓴다 */
#include <linux/time.h>	/* [한국어] 시간 관련 기본 정의. 이 파일이 직접 쓰는 것은 아래 ktime 계열이며, 이 헤더에서 오는 심볼을 특정하지는 못했다 */
#include <linux/ktime.h>	/* [한국어] ktime_get_ns 와 NSEC_PER_SEC - do_pci_op 의 2초 타임아웃 판정에 쓴다 */
#include <xen/platform_pci.h>	/* [한국어] xen_has_pv_devices() - 이 게스트에 PV 장치 채널이 있는지 확인하는 데 쓴다 */

#include <asm/xen/swiotlb-xen.h>	/* [한국어] Xen 용 swiotlb 헤더. 다만 이 파일에서 swiotlb 관련 심볼을 쓰는 곳은 없다(주석을 제거하고 전수 확인). 과거 코드의 잔재로 보이나 이 트리만으로는 단정할 수 없다 */

/* [한국어] 이벤트 채널이 아직 할당되지 않았음을 나타내는 표식. 0 은 유효한 포트 번호일 수 있어 쓸 수 없으므로 -1 을 쓴다. alloc_pdev 이 이 값으로 초기화하고, free_pdev 이 이 값과 비교해 해제 여부를 정한다. */
#define INVALID_EVTCHN    (-1)

/*
 * [한국어] pci_bus_entry - 이 프론트엔드가 만든 루트 버스 하나를 추적하는 항목.
 *
 * 백엔드가 여러 개의 PCI 루트를 내보낼 수 있어(XenStore 의 root_num),
 * 해체할 때 무엇을 지워야 하는지 기억해 둘 목록이 필요하다.
 */
struct pci_bus_entry {
	struct list_head list;	/* [한국어] pcifront_device.root_buses 리스트에 연결하는 고리. 설정자: pcifront_scan_root 의 list_add. 읽는 자: pcifront_free_roots 의 순회. 동기화: pci_lock_rescan_remove 뮤텍스 아래에서만 다룬다 */
	struct pci_bus *bus;	/* [한국어] 이 항목이 가리키는 루트 버스. 설정자: pcifront_scan_root 이 pci_scan_root_bus 결과를 저장. 읽는 자: pcifront_free_roots 가 장치 제거와 버스 제거에 쓴다. 값 범위: 유효한 pci_bus 포인터(NULL 로 남는 경우 없음). 동기화: 위와 같다 */
};

/* [한국어] 아래 두 매크로는 pdev->flags 의 비트 0 을 가리킨다. 이 비트는 "AER 워크가 이미 예약됐다" 는 표시이며, 같은 pdev 의 AER 워크가 두 개 동시에 돌지 않도록 test_and_set_bit 로 배타를 잡는 데 쓴다. */
#define _PDEVB_op_active		(0)	/* [한국어] 비트 번호 0. test_and_set_bit / clear_bit 은 마스크가 아니라 비트 번호를 받으므로 실제로 쓰이는 쪽은 이것이다 */
#define PDEVB_op_active			(1 << (_PDEVB_op_active))	/* [한국어] 같은 비트를 마스크로 표현한 것. 다만 이 파일 안에서 이 이름을 쓰는 곳은 없다(주석을 제거하고 전수 확인). 정의만 남아 있다 */

/*
 * [한국어] pcifront_device - XenBus 채널 하나에 대응하는 프론트엔드 상태 전부.
 *
 * alloc_pdev() 이 만들고 free_pdev() 이 해제한다. XenBus 장치의 drvdata 에
 * 매달려 있어 모든 콜백이 dev_get_drvdata 로 되찾는다. 백엔드와 공유하는
 * 페이지, 그 페이지를 깨우는 이벤트 채널, 그 위에 만든 루트 버스들,
 * 그리고 AER 워크가 여기 다 모여 있다.
 */
struct pcifront_device {
	struct xenbus_device *xdev;	/* [한국어] 이 프론트엔드가 붙어 있는 XenBus 장치. 설정자: alloc_pdev. 읽는 자: 거의 모든 함수(로그 출력, XenStore 경로 nodename/otherend, 상태 전이). 값 범위: 유효한 포인터, NULL 이 되지 않는다. 동기화: 생성 후 불변이라 락이 필요 없다 */
	struct list_head root_buses;	/* [한국어] 이 프론트엔드가 만든 루트 버스들의 목록 머리. 설정자: alloc_pdev 이 INIT_LIST_HEAD 로 비우고, pcifront_scan_root 이 항목을 추가한다. 읽는 자: pcifront_free_roots. 값 범위: 빈 리스트일 수 있다. 동기화: pci_lock_rescan_remove 뮤텍스 아래에서만 다룬다 */

	int evtchn;	/* [한국어] 백엔드를 깨울 이벤트 채널 포트 번호. 설정자: pcifront_publish_info 의 xenbus_alloc_evtchn. 읽는 자: do_pci_op 과 pcifront_do_aer 의 notify_remote_via_evtchn, free_pdev 의 해제. 값 범위: 유효 포트 또는 INVALID_EVTCHN(-1). 동기화: probe 에서 한 번 설정되고 이후 불변 */
	grant_ref_t gnt_ref;	/* [한국어] 공유 페이지의 grant 참조 번호. 설정자: alloc_pdev 의 xenbus_setup_ring. 읽는 자: pcifront_publish_info 가 XenStore 의 pci-op-ref 로 발행하고, free_pdev 이 회수에 쓴다. 값 범위: grant table 이 배정한 번호. 동기화: 설정 후 불변 */

	int irq;	/* [한국어] 위 이벤트 채널에 바인딩된 Linux IRQ 번호. 설정자: pcifront_publish_info 의 bind_evtchn_to_irqhandler 반환값. 읽는 자: do_pci_op 의 폴링(xen_poll_irq_timeout), free_pdev 의 unbind. 값 범위: 0 이상이면 유효, -1 이면 미바인딩. 동기화: probe 에서 한 번 설정 */

	/* Lock this when doing any operations in sh_info */
	spinlock_t sh_info_lock;	/* [한국어] 아래 sh_info 접근 전체를 직렬화하는 스핀락. 설정자: alloc_pdev 의 spin_lock_init. 읽는 자: do_pci_op 이 irqsave 판으로 잡는다. 값 범위: 스핀락. 동기화: 공유 페이지의 요청 슬롯이 하나뿐이라 이 락이 곧 "동시에 한 요청" 규칙이다. 인터럽트 문맥에서도 config 접근이 있을 수 있어 irqsave 를 쓴다 */
	struct xen_pci_sharedinfo *sh_info;	/* [한국어] 백엔드와 함께 보는 페이지. 설정자: alloc_pdev 의 xenbus_setup_ring. 읽는 자/쓰는 자: do_pci_op(op 슬롯과 flags), pcifront_do_aer 와 pcifront_common_process(aer_op 슬롯), schedule_pcifront_aer_op(flags). 값 범위: 유효한 커널 가상 주소. 동기화: op 방향은 sh_info_lock 이, aer_op 방향은 _PDEVB_op_active 비트가 배타를 만든다. 백엔드와의 순서는 wmb 와 flags 비트로 맞춘다 */
	struct work_struct op_work;	/* [한국어] AER 요청을 프로세스 문맥에서 처리할 워크 아이템. 설정자: alloc_pdev 의 INIT_WORK(핸들러는 pcifront_do_aer). 읽는 자: schedule_work / cancel_work_sync. 값 범위: 초기화된 work_struct. 동기화: 워크큐 코어가 관리하고, 중복 예약은 _PDEVB_op_active 비트가 막는다 */
	unsigned long flags;	/* [한국어] 이 프론트엔드 자체의 플래그. 지금 쓰이는 비트는 _PDEVB_op_active(비트 0) 하나뿐이다. 설정자/읽는 자: schedule_pcifront_aer_op 의 test_and_set_bit 과 pcifront_do_aer 의 clear_bit. 값 범위: 비트 0 만 의미가 있다. 동기화: 원자적 비트 연산과 앞뒤 메모리 배리어로만 다룬다 - 락을 쓰지 않는 이유는 인터럽트 핸들러에서도 만지기 때문이다 */

};

/*
 * [한국어] pcifront_sd - PV 루트 버스에서 프론트엔드로 되돌아오는 통로.
 *
 * PCI 코어는 버스마다 sysdata 라는 불투명 포인터 하나를 보관해 준다.
 * pcifront_bus_read/write 는 bus->sysdata 를 이 구조체로 해석해
 * 어느 XenBus 채널로 요청을 보낼지 알아낸다. 첫 필드가 pci_sysdata 인 것이
 * 중요하다 - PCI 코어가 sysdata 를 struct pci_sysdata 로 읽는 경로가 있어,
 * 그쪽에서도 앞부분이 그대로 유효해야 하기 때문이다.
 */
struct pcifront_sd {
	struct pci_sysdata sd;	/* [한국어] PCI 코어가 기대하는 표준 sysdata. 설정자: pcifront_init_sd 가 node 와 domain 을 채운다. 읽는 자: PCI 코어(도메인 번호, NUMA 노드 조회). 값 범위: domain 은 백엔드가 알린 값, node 는 first_online_node. 동기화: 버스 생성 시 한 번 설정되고 이후 불변 */
	struct pcifront_device *pdev;	/* [한국어] 이 버스를 만든 프론트엔드로 되돌아가는 포인터. 설정자: pcifront_init_sd. 읽는 자: pcifront_get_pdev 를 통해 config 접근과 MSI 함수 전부. 값 범위: 유효한 포인터. 동기화: 불변이므로 락이 필요 없다. 해제는 pcifront_free_roots 가 버스와 함께 kfree 한다 */
};

/*
 * [한국어]
 * pcifront_get_pdev - 버스 sysdata 에서 프론트엔드 상태를 꺼낸다
 *
 * @sd: pci_bus 의 sysdata 를 struct pcifront_sd 로 해석한 포인터.
 * @return: 그 버스를 만든 pcifront_device. NULL 이 되는 경우는 없다
 *   (pcifront_init_sd 가 항상 채우기 때문이다).
 *
 * 한 줄짜리 접근자지만 역할이 분명하다. PCI 코어의 콜백들은 pci_bus 나
 * pci_dev 만 받는다. 거기서 "이 버스는 어느 XenBus 채널의 것인가" 를
 * 알아내는 유일한 경로가 이 함수다.
 *
 * 실행 컨텍스트: 호출자를 그대로 따른다. config 접근 경로에서는
 * pci_lock 을 잡은 상태일 수 있다. 락도 부작용도 없다.
 *
 * 호출 체인:
 *   pcifront_bus_read/write, pci_frontend_enable_msi 계열
 *     -> [pcifront_get_pdev]
 */
static inline struct pcifront_device *
pcifront_get_pdev(struct pcifront_sd *sd)
{
	return sd->pdev;	/* [한국어] 단순 필드 참조. 이 한 줄이 버스 -> 프론트엔드 방향의 전부다 */
}

/*
 * [한국어]
 * pcifront_init_sd - 새 루트 버스의 sysdata 를 채운다
 *
 * @sd: pcifront_scan_root 이 kzalloc 한 sysdata. 0 으로 초기화된 상태다.
 * @domain: 이 루트 버스의 PCI 도메인 번호.
 * @bus: 루트 버스 번호. 현재 이 함수는 이 값을 쓰지 않는다 - sysdata 에
 *   버스 번호를 담을 자리가 없기 때문이다(pci_scan_root_bus 에 따로 넘긴다).
 * @pdev: 이 버스를 만드는 프론트엔드.
 * @return: 없음.
 *
 * NUMA 노드를 first_online_node 로 고정하는 이유를 위 영어 주석이 밝히고 있다.
 * 백엔드가 XenBus 로 노드 정보를 알려 주지 않기 때문이다. 즉 게스트는 이
 * 장치가 물리적으로 어느 노드에 붙어 있는지 알 수 없고, 그래서 아무 노드나
 * 고른다. 노드별 메모리 할당 최적화는 이 환경에서 기대할 수 없다는 뜻이다.
 *
 * 실행 컨텍스트: 프로세스 문맥(버스 생성 중). 락 없음.
 *
 * 호출 체인:
 *   pcifront_scan_root() -> [pcifront_init_sd]
 */
static inline void pcifront_init_sd(struct pcifront_sd *sd,
				    unsigned int domain, unsigned int bus,
				    struct pcifront_device *pdev)
{
	/* Because we do not expose that information via XenBus. */
	sd->sd.node = first_online_node;	/* [한국어] NUMA 노드를 온라인 노드 중 첫 번째로 고정한다. 백엔드가 실제 노드를 알려 주지 않으니 다른 선택지가 없다 */
	sd->sd.domain = domain;	/* [한국어] 백엔드가 알린 PCI 도메인 번호를 그대로 저장한다. PCI 코어가 pci_domain_nr 로 읽어 간다 */
	sd->pdev = pdev;	/* [한국어] 버스에서 프론트엔드로 되돌아올 포인터를 심는다. 이 대입이 이후 모든 config 접근의 출발점이다 */
}

/*
 * [한국어] 아래 두 전역은 "PV PCI 프론트엔드는 시스템에 하나" 라는 규칙을 만든다.
 *
 * pcifront_dev 를 읽는 곳은 pcifront_disconnect() 의 동일성 비교 하나뿐이고
 * (주석을 제거하고 전수 확인), 쓰는 곳은 등록과 해제 두 곳이다.
 * 즉 지금 코드에서 이 전역의 역할은 배타 조건 그 자체다.
 */
static DEFINE_SPINLOCK(pcifront_dev_lock);	/* [한국어] 아래 전역 포인터를 보호하는 스핀락. 여러 XenBus 채널이 동시에 등록을 시도할 수 있다. 임계 구역이 포인터 대입뿐이라 irqsave 판이 필요 없다 */
static struct pcifront_device *pcifront_dev;	/* [한국어] 현재 등록된 프론트엔드. 설정자: pcifront_connect_and_init_dma. 해제자: pcifront_disconnect. 읽는 자: 그 두 함수뿐. 값 범위: NULL(미등록) 또는 유효한 포인터. 동기화: 위 스핀락 */

/*
 * [한국어]
 * errno_to_pcibios_err - 백엔드 오류 코드를 PCI 코어가 아는 코드로 바꾼다
 *
 * @errno: 백엔드가 돌려준 XEN_PCI_ERR_* 값, 또는 do_pci_op 이 타임아웃 시
 *   쓰는 XEN_PCI_ERR_dev_not_found.
 * @return: PCIBIOS_* 계열 코드. 표에 없는 값은 그대로 흘려보낸다.
 *
 * 두 오류 체계 사이의 번역기다. Xen PV 프로토콜은 자기 나름의 오류 코드를
 * 쓰지만, PCI 코어는 config 접근 결과를 PCIBIOS_* 로만 이해한다.
 * 이 표가 없으면 게스트의 PCI 코어가 백엔드의 거부를 성공으로 오해할 수 있다.
 * XEN_PCI_ERR_* 값들의 정의는 이 트리에 없는 pciif.h 에 있다.
 *
 * 실행 컨텍스트: 호출자를 따른다. 순수 함수라 락이 필요 없다.
 *
 * 호출 체인:
 *   pcifront_bus_read() / pcifront_bus_write() -> [errno_to_pcibios_err]
 */
static int errno_to_pcibios_err(int errno)
{
	switch (errno) {	/* [한국어] 백엔드 오류 코드로 분기한다 */
	/* [한국어] 성공. */
	case XEN_PCI_ERR_success:
		return PCIBIOS_SUCCESSFUL;	/* [한국어] PCI 코어가 아는 성공 코드 */

	/* [한국어] 그 BDF 에 장치가 없다. do_pci_op 의 타임아웃도 이 코드를 쓴다. */
	case XEN_PCI_ERR_dev_not_found:
		return PCIBIOS_DEVICE_NOT_FOUND;	/* [한국어] 열거 중이라면 이 코드가 정상적으로 나온다. pcifront_bus_read 는 -ENODEV 를 따로 0 으로 바꾸기도 한다 */

	/* [한국어] 오프셋이 범위를 벗어났거나 백엔드가 처리에 실패했다. */
	case XEN_PCI_ERR_invalid_offset:
	case XEN_PCI_ERR_op_failed:
		return PCIBIOS_BAD_REGISTER_NUMBER;	/* [한국어] 두 경우 모두 "레지스터 번호가 잘못됐다" 로 묶어 보고한다 */

	/* [한국어] 백엔드가 그 기능(예: MSI-X)을 구현하지 않았다. */
	case XEN_PCI_ERR_not_implemented:
		return PCIBIOS_FUNC_NOT_SUPPORTED;	/* [한국어] 기능 미지원으로 보고한다 */

	/* [한국어] 백엔드가 정책상 거부했다. 격리를 지키기 위한 거부가 여기로 온다. */
	case XEN_PCI_ERR_access_denied:
		return PCIBIOS_SET_FAILED;	/* [한국어] 쓰기 실패로 보고한다. 게스트 입장에서는 값이 반영되지 않았다는 뜻이다 */
	}
	return errno;	/* [한국어] 표에 없는 값(예: 음수 errno)은 변환하지 않고 그대로 넘긴다 */
}

/*
 * [한국어]
 * schedule_pcifront_aer_op - AER 요청이 있고 처리 중이 아니면 워크를 예약한다
 *
 * @pdev: 프론트엔드 상태.
 * @return: 없음.
 *
 * 두 개의 비트를 함께 본다.
 *  _XEN_PCIB_active - 공유 페이지의 flags 에 있고, 백엔드가 "AER 요청을
 *    올렸다" 는 뜻으로 세운다. 이름의 B 는 backend 방향을 가리킨다
 *    (프론트엔드 방향은 _XEN_PCIF_active 다).
 *  _PDEVB_op_active - 게스트 쪽 pdev->flags 의 비트 0 으로, 이미 워크를
 *    예약했다는 표시다. test_and_set_bit 으로 원자적으로 검사하고 세운다.
 *
 * 두 조건을 &&로 묶은 것이 핵심이다. 앞의 test_bit 은 "할 일이 있는가",
 * 뒤의 test_and_set_bit 은 "내가 처음인가" 를 묻는다. 뒤쪽이 원자적이라
 * 인터럽트 핸들러와 do_pci_op 과 워크 자신이 동시에 이 함수를 불러도
 * 워크가 두 번 예약되지 않는다. 락 대신 원자적 비트 연산을 쓰는 이유는
 * 인터럽트 문맥에서도 불리기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 핸들러(pcifront_handler_aer), 스핀락을 쥔
 * 프로세스 문맥(do_pci_op), 워크큐 스레드(pcifront_do_aer) 모두에서 불린다.
 * 그래서 잠들 수 있는 동작을 해서는 안 되고, 실제로 하는 일도
 * 비트 검사와 schedule_work 뿐이다.
 *
 * 호출 체인:
 *   pcifront_handler_aer() / do_pci_op() / pcifront_do_aer()
 *     -> [schedule_pcifront_aer_op] -> schedule_work()
 */
static inline void schedule_pcifront_aer_op(struct pcifront_device *pdev)
{
	if (test_bit(_XEN_PCIB_active, (unsigned long *)&pdev->sh_info->flags)	/* [한국어] 백엔드가 AER 요청을 올려 두었는지 확인한다. 공유 페이지의 flags 를 읽는 것이라 백엔드가 언제든 바꿀 수 있다 */
		&& !test_and_set_bit(_PDEVB_op_active, &pdev->flags)) {	/* [한국어] 아직 예약되지 않았을 때만 통과한다. 검사와 설정이 한 번에 이뤄져 경쟁이 없다. 이미 1 이면 다른 문맥이 이미 예약한 것이므로 아무것도 하지 않는다 */
		dev_dbg(&pdev->xdev->dev, "schedule aer frontend job\n");	/* [한국어] 예약 사실을 디버그 로그로 남긴다 */
		schedule_work(&pdev->op_work);	/* [한국어] 시스템 워크큐에 넣는다. 드라이버의 err_handler 콜백은 잠들 수 있어야 하므로 인터럽트 문맥에서 직접 부를 수 없다 */
	}
}

/*
 * [한국어]
 * do_pci_op - 요청 하나를 공유 페이지에 실어 백엔드에 보내고 응답까지 기다린다
 *
 * @pdev: 이 요청을 보낼 XenBus 채널의 프론트엔드 상태.
 * @op: 보낼 요청. 호출자가 스택에 만든 struct xen_pci_op 이며,
 *   응답도 이 구조체에 덮어써서 돌려준다(입출력 겸용).
 * @return: XEN_PCI_ERR_* 계열의 백엔드 오류 코드. 성공이면 0.
 *   타임아웃이면 XEN_PCI_ERR_dev_not_found. 호출자들은 대부분
 *   errno_to_pcibios_err() 로 변환해 PCI 코어에 돌려준다.
 *
 * 이 파일의 모든 통신이 여기를 지난다. config 읽기/쓰기, MSI/MSI-X 설정이
 * 전부 이 함수 하나로 수렴한다.
 *
 * 프로토콜:
 *  1) sh_info_lock 을 잡는다. 공유 페이지에 op 슬롯이 하나뿐이라
 *     동시에 두 요청을 실을 수 없기 때문이다.
 *  2) 요청을 공유 페이지로 복사한다.
 *  3) wmb() - 요청 내용이 먼저 보이고 그 다음에 "요청 있음" 비트가
 *     보이도록 강제한다. 순서가 뒤집히면 백엔드가 비트만 보고
 *     아직 쓰이지 않은 쓰레기 요청을 처리한다.
 *  4) _XEN_PCIF_active 비트를 세우고(F = frontend 가 세운다)
 *     이벤트 채널로 백엔드를 깨운다.
 *  5) 백엔드가 처리를 마치면 그 비트를 내린다. 그때까지 폴링한다.
 *  6) 응답을 호출자 구조체로 되복사하고 락을 푼다.
 *
 * 타임아웃: 폴링은 3초로 걸고 판정은 2초로 한다. 위 영어 주석이 설명하듯
 * 판정이 폴링보다 짧아야 "이미 지나간 시각으로 poll 을 반복 호출" 하는
 * 상황을 피할 수 있다. 2초가 지나면 비트를 강제로 내리고
 * XEN_PCI_ERR_dev_not_found 로 실패시킨다.
 *
 * AER 과의 얽힘: 백엔드는 config 응답과 AER 요청을 같은 이벤트 채널로
 * 보낸다. 그래서 이 함수가 응답을 기다리며 채널 알림을 소비하는 사이
 * AER 요청이 묻힐 수 있다. 그것을 막으려고 응답을 받은 뒤
 * _XEN_PCIB_active 를 다시 확인해 AER 작업을 재예약한다(위 영어 주석).
 *
 * 실행 컨텍스트: config 접근 경로에서는 drivers/pci/access.c 가
 * CONFIG_PCI_LOCKLESS_CONFIG 가 꺼진 빌드에서 raw_spin_lock_irqsave(&pci_lock) 를
 * 잡은 채로 이 함수를 부른다(access.c 의 pci_lock_config 정의로 확인).
 * 여기서 다시 sh_info_lock 을 irqsave 로 잡고 응답을 기다린다.
 * xen_poll_irq_timeout() 이 정확히 어떤 방식으로 대기하는지는
 * 이 트리에 없는 xen/events.h 에 있어 확인할 수 없다.
 *
 * 호출 체인:
 *   pcifront_bus_read/pcifront_bus_write/pci_frontend_enable_msi 계열
 *     -> [do_pci_op] -> notify_remote_via_evtchn() -> (백엔드 pciback)
 *                    -> xen_poll_irq_timeout()
 */
static int do_pci_op(struct pcifront_device *pdev, struct xen_pci_op *op)
{
	int err = 0;	/* [한국어] 백엔드가 돌려준 오류 코드. 성공이면 0 으로 남는다 */
	struct xen_pci_op *active_op = &pdev->sh_info->op;	/* [한국어] 공유 페이지 안의 유일한 요청 슬롯을 가리킨다. 이 포인터 너머는 백엔드와 함께 보는 메모리다 */
	unsigned long irq_flags;	/* [한국어] spin_lock_irqsave 가 저장할 인터럽트 상태 */
	evtchn_port_t port = pdev->evtchn;	/* [한국어] 알림을 보낼 이벤트 채널 포트. 락 밖에서 쓰기 위해 미리 지역 변수로 복사한다 */
	unsigned int irq = pdev->irq;	/* [한국어] 폴링에 쓸 Linux IRQ 번호. 역시 미리 복사한다 */
	s64 ns, ns_timeout;	/* [한국어] 현재 시각과 타임아웃 시각(나노초) */

	spin_lock_irqsave(&pdev->sh_info_lock, irq_flags);	/* [한국어] 공유 페이지의 op 슬롯이 하나뿐이라 요청 전체를 직렬화한다. irqsave 인 이유는 인터럽트 문맥에서도 config 접근이 일어날 수 있기 때문이다 */

	memcpy(active_op, op, sizeof(struct xen_pci_op));	/* [한국어] 호출자의 요청을 공유 페이지로 통째로 복사한다. 이 순간부터 백엔드가 읽을 수 있는 자리에 놓인다 */

	/* Go */
	wmb();	/* [한국어] 쓰기 배리어. 위 memcpy 가 아래 set_bit 보다 먼저 백엔드에게 보이도록 강제한다. 순서가 뒤집히면 백엔드가 쓰레기 요청을 처리한다 */
	set_bit(_XEN_PCIF_active, (unsigned long *)&pdev->sh_info->flags);	/* [한국어] _XEN_PCIF_active 비트를 세워 "요청이 준비됐다" 고 알린다. 백엔드가 처리를 마치면 이 비트를 내려 준다 */
	notify_remote_via_evtchn(port);	/* [한국어] 이벤트 채널로 백엔드 도메인에 가상 인터럽트를 올린다. 하이퍼콜 한 번이다 */

	/*
	 * We set a poll timeout of 3 seconds but give up on return after
	 * 2 seconds. It is better to time out too late rather than too early
	 * (in the latter case we end up continually re-executing poll() with a
	 * timeout in the past). 1s difference gives plenty of slack for error.
	 */
	ns_timeout = ktime_get_ns() + 2 * (s64)NSEC_PER_SEC;	/* [한국어] 현재 시각에 2초를 더해 판정 기준 시각을 만든다. 폴링은 3초로 걸어 이 값보다 늦게 깨어나도록 한다 */

	xen_clear_irq_pending(irq);	/* [한국어] 폴링 전에 밀린 이벤트 표시를 지운다. 남아 있으면 xen_poll_irq_timeout 이 즉시 반환해 바쁜 대기가 된다 */

	while (test_bit(_XEN_PCIF_active,
			(unsigned long *)&pdev->sh_info->flags)) {	/* [한국어] 백엔드가 비트를 내릴 때까지 반복한다. 이 비트가 완료 신호다 */
		xen_poll_irq_timeout(irq, jiffies + 3*HZ);	/* [한국어] 이 IRQ 에 이벤트가 올 때까지 최대 3초 대기한다 */
		xen_clear_irq_pending(irq);	/* [한국어] 깨어난 원인이 무엇이든 표시를 지워 다음 대기가 즉시 반환하지 않게 한다 */
		ns = ktime_get_ns();	/* [한국어] 현재 시각을 다시 읽는다 */
		if (ns > ns_timeout) {	/* [한국어] 2초가 지났으면 백엔드가 응답하지 않는 것으로 판정한다 */
			dev_err(&pdev->xdev->dev,
				"pciback not responding!!!\n");	/* [한국어] 무응답 사실을 남긴다 */
			clear_bit(_XEN_PCIF_active,
				  (unsigned long *)&pdev->sh_info->flags);	/* [한국어] 비트를 강제로 내려 다음 요청이 이 잔재에 걸리지 않게 한다 */
			err = XEN_PCI_ERR_dev_not_found;	/* [한국어] 장치를 못 찾은 것과 같은 코드로 실패시킨다. 호출자는 PCIBIOS_DEVICE_NOT_FOUND 로 변환한다 */
			goto out;	/* [한국어] 응답 복사를 건너뛰고 락 해제로 간다 */
		}
	}

	/*
	 * We might lose backend service request since we
	 * reuse same evtchn with pci_conf backend response. So re-schedule
	 * aer pcifront service.
	 */
	if (test_bit(_XEN_PCIB_active,
			(unsigned long *)&pdev->sh_info->flags)) {	/* [한국어] 백엔드가 AER 요청을 올려 둔 경우. 응답을 기다리는 사이 알림이 묻혔을 수 있다 */
		dev_err(&pdev->xdev->dev,
			"schedule aer pcifront service\n");	/* [한국어] AER 을 재예약한다는 사실을 남긴다 */
		schedule_pcifront_aer_op(pdev);	/* [한국어] 워크큐에 AER 작업을 넣는다. 실제 처리는 이 락 밖에서 이뤄진다 */
	}

	memcpy(op, active_op, sizeof(struct xen_pci_op));	/* [한국어] 백엔드가 채워 넣은 응답을 호출자 구조체로 되복사한다. config 값이나 배정된 벡터가 여기에 들어 있다 */

	err = op->err;	/* [한국어] 백엔드가 남긴 오류 코드를 꺼낸다 */
/* [한국어] 타임아웃 경로와 정상 경로가 함께 지나가는 락 해제 지점. */
out:
	spin_unlock_irqrestore(&pdev->sh_info_lock, irq_flags);	/* [한국어] 인터럽트 상태를 복원하며 락을 푼다. 이제 다음 요청이 슬롯을 쓸 수 있다 */
	return err;	/* [한국어] XEN_PCI_ERR_* 또는 0 */
}

/* Access to this function is spinlocked in drivers/pci/access.c */
/*
 * [한국어]
 * pcifront_bus_read - PV 버스의 config 읽기 구현
 *
 * @bus: 읽을 대상이 매달린 pci_bus. pcifront_scan_root() 가 만든 PV 루트 버스다.
 * @devfn: device/function 번호.
 * @where: config 공간 오프셋. 0x00 vendor/device ID, 0x04 command/status,
 *   0x10 부터 BAR0~5, 0x34 capabilities pointer 같은 표준 배치를 따른다.
 * @size: 읽을 바이트 수(1/2/4).
 * @val: 읽은 값을 담아 돌려줄 곳.
 * @return: PCIBIOS_* 코드. PCI 코어는 이 규약의 값만 이해한다.
 *
 * 이것이 이 파일 전체의 존재 이유다. 보통의 호스트 브리지 드라이버라면
 * 여기서 CF8/CFC 포트나 ECAM(MMCONFIG) 영역에 실제로 접근하겠지만,
 * 여기서는 요청을 구조체로 만들어 백엔드에게 넘긴다. 게스트는 물리
 * config 공간을 단 한 바이트도 직접 건드리지 않는다.
 *
 * 왜 그래야 하는가. config 공간에는 게스트가 마음대로 바꾸면 격리가 깨지는
 * 필드가 많다. BAR 를 다른 장치의 MMIO 영역으로 옮기거나, Bus Master 비트를
 * 켜 임의 주소로 DMA 를 시키거나, 브리지의 서브버스 번호를 바꿔 남의 장치를
 * 자기 아래로 끌어올 수 있다. 그래서 하이퍼바이저 쪽(dom0 의 pciback)이
 * 모든 접근을 중간에서 검사하고, 허용되지 않는 필드는 가짜 값으로 답하거나
 * 쓰기를 무시한다. 이 함수는 그 검사대로 요청을 보내는 창구다.
 *
 * -ENODEV 를 0 으로 바꾸는 처리에 주의. 열거 중에는 없는 devfn 을 읽는 일이
 * 정상이며, 그때는 오류가 아니라 "전부 0" 을 돌려줘야 PCI 코어가
 * 장치 없음으로 판단하고 조용히 넘어간다(위 영어 주석의 pretend).
 *
 * 실행 컨텍스트: drivers/pci/access.c 가 CONFIG_PCI_LOCKLESS_CONFIG 가 꺼진
 * 빌드에서 raw_spin_lock_irqsave(&pci_lock) 를 잡은 채로 부른다
 * (위 영어 주석이 말하는 spinlocked 가 이것이다).
 *
 * 호출 체인:
 *   pci_read_config_word 계열 -> pci_bus_read_config_word (access.c)
 *     -> bus->ops->read -> [pcifront_bus_read] -> do_pci_op()
 */
static int pcifront_bus_read(struct pci_bus *bus, unsigned int devfn,
			     int where, int size, u32 *val)
{
	int err = 0;	/* [한국어] do_pci_op() 결과 */
	struct xen_pci_op op = {	/* [한국어] 백엔드에 보낼 요청을 스택에 만든다 */
		.cmd    = XEN_PCI_OP_conf_read,	/* [한국어] config 읽기 명령 코드 */
		.domain = pci_domain_nr(bus),	/* [한국어] 이 버스의 PCI 도메인 번호 */
		.bus    = bus->number,	/* [한국어] 버스 번호 */
		.devfn  = devfn,	/* [한국어] device/function 번호 */
		.offset = where,	/* [한국어] config 공간 오프셋 */
		.size   = size,	/* [한국어] 읽을 바이트 수. 1/2/4 만 유효하다 */
	};
	struct pcifront_sd *sd = bus->sysdata;	/* [한국어] pcifront_scan_root() 이 버스에 붙여 둔 sysdata */
	struct pcifront_device *pdev = pcifront_get_pdev(sd);	/* [한국어] sysdata 에서 프론트엔드 상태를 꺼낸다. 이것이 버스에서 XenBus 채널로 돌아오는 유일한 통로다 */

	dev_dbg(&pdev->xdev->dev,
		"read dev=%04x:%02x:%02x.%d - offset %x size %d\n",
		pci_domain_nr(bus), bus->number, PCI_SLOT(devfn),
		PCI_FUNC(devfn), where, size);	/* [한국어] 어떤 BDF 의 어느 오프셋을 몇 바이트 읽는지 남긴다 */

	err = do_pci_op(pdev, &op);	/* [한국어] 공유 페이지에 실어 백엔드에 넘기고 응답까지 기다린다 */

	if (likely(!err)) {	/* [한국어] 정상 응답. likely 는 대부분의 읽기가 성공한다는 힌트다 */
		dev_dbg(&pdev->xdev->dev, "read got back value %x\n",
			op.value);	/* [한국어] 읽어 온 값을 남긴다 */

		*val = op.value;	/* [한국어] 백엔드가 채워 준 값을 호출자에게 돌려준다 */
	} else if (err == -ENODEV) {	/* [한국어] 장치가 없다는 응답 */
		/* No device here, pretend that it just returned 0 */
		err = 0;	/* [한국어] 오류가 아니라 정상으로 처리한다 */
		*val = 0;	/* [한국어] 없는 장치의 config 읽기는 0 을 돌려주는 것이 관례다. 열거 중 빈 슬롯이 이 경로를 탄다 */
	}

	return errno_to_pcibios_err(err);	/* [한국어] XEN_PCI_ERR_* 를 PCIBIOS_* 로 바꿔 PCI 코어에 돌려준다 */
}

/* Access to this function is spinlocked in drivers/pci/access.c */
/*
 * [한국어]
 * pcifront_bus_write - PV 버스의 config 쓰기 구현
 *
 * @bus: 쓸 대상이 매달린 PV 루트 버스.
 * @devfn: device/function 번호.
 * @where: config 공간 오프셋.
 * @size: 쓸 바이트 수(1/2/4).
 * @val: 쓸 값.
 * @return: PCIBIOS_* 코드.
 *
 * 읽기와 대칭이지만 두 가지가 다르다. 첫째, 값(val)을 요청에 함께 싣는다.
 * 둘째, -ENODEV 를 0 으로 바꾸는 예외가 없다 - 없는 장치에 쓰는 것은
 * 읽기와 달리 정상 상황이 아니기 때문이다.
 *
 * 격리 관점에서 더 중요한 쪽은 이 방향이다. 백엔드는 여기로 들어오는
 * 쓰기를 검사해, 허용된 필드가 아니면 무시하거나 거부한다
 * (거부하면 XEN_PCI_ERR_access_denied 가 돌아와 PCIBIOS_SET_FAILED 가 된다).
 * pcifront_claim_resource() 가 BAR 를 재배치하지 않고 그대로 등록만 하는
 * 이유도 여기에 있다 - 어차피 BAR 쓰기가 통하지 않는다.
 *
 * 실행 컨텍스트: 읽기와 같다. access.c 가 pci_lock 을 잡은 채로 부른다
 * (CONFIG_PCI_LOCKLESS_CONFIG 가 꺼진 빌드).
 *
 * 호출 체인:
 *   pci_write_config_word 계열 -> pci_bus_write_config_word (access.c)
 *     -> bus->ops->write -> [pcifront_bus_write] -> do_pci_op()
 */
static int pcifront_bus_write(struct pci_bus *bus, unsigned int devfn,
			      int where, int size, u32 val)
{
	struct xen_pci_op op = {	/* [한국어] 백엔드에 보낼 요청을 스택에 만든다 */
		.cmd    = XEN_PCI_OP_conf_write,	/* [한국어] config 쓰기 명령 코드 */
		.domain = pci_domain_nr(bus),	/* [한국어] 이 버스의 PCI 도메인 번호 */
		.bus    = bus->number,	/* [한국어] 버스 번호 */
		.devfn  = devfn,	/* [한국어] device/function 번호 */
		.offset = where,	/* [한국어] config 공간 오프셋 */
		.size   = size,	/* [한국어] 쓸 바이트 수 */
		.value  = val,	/* [한국어] 쓸 값. 읽기 요청에는 없는 필드다 */
	};
	struct pcifront_sd *sd = bus->sysdata;	/* [한국어] 버스에 붙여 둔 sysdata */
	struct pcifront_device *pdev = pcifront_get_pdev(sd);	/* [한국어] sysdata 에서 프론트엔드 상태를 꺼낸다 */

	dev_dbg(&pdev->xdev->dev,
		"write dev=%04x:%02x:%02x.%d - offset %x size %d val %x\n",
		pci_domain_nr(bus), bus->number,
		PCI_SLOT(devfn), PCI_FUNC(devfn), where, size, val);	/* [한국어] 어떤 BDF 의 어느 오프셋에 무슨 값을 쓰는지 남긴다 */

	return errno_to_pcibios_err(do_pci_op(pdev, &op));	/* [한국어] 백엔드에 넘기고, 돌아온 오류 코드를 PCIBIOS_* 로 바꿔 그대로 반환한다 */
}

/*
 * [한국어] pcifront_bus_ops - PV 루트 버스에 꽂을 config 접근 방법.
 *
 * PCI 코어는 버스마다 이 두 함수 포인터만 보고 config 공간을 다룬다.
 * 하드웨어 브리지 드라이버라면 CF8/CFC 나 ECAM 접근을 넣겠지만,
 * 여기에는 공유 페이지로 요청을 넘기는 함수가 들어간다.
 * pcifront_scan_root() 이 pci_scan_root_bus() 에 이 구조체를 넘기는 순간
 * 그 버스 아래 모든 장치의 config 접근이 하이퍼바이저를 거치게 된다.
 */
static struct pci_ops pcifront_bus_ops = {
	.read = pcifront_bus_read,	/* [한국어] config 읽기 진입점 */
	.write = pcifront_bus_write,	/* [한국어] config 쓰기 진입점 */
};

/*
 * [한국어] 여기부터 #endif 까지는 MSI/MSI-X 를 PV 경로로 우회시키는 부분이다.
 *
 * 왜 별도 처리가 필요한가. MSI/MSI-X 는 config 공간의 capability 레지스터를
 * 쓰는 것만으로 끝나지 않는다. 인터럽트 벡터를 실제로 배정하고 CPU 의
 * 인터럽트 컨트롤러(APIC 등)에 연결하는 작업이 따라오는데, PV 게스트에는
 * 그런 하드웨어가 없다. 그래서 커널의 MSI 코어가 config 공간을 직접
 * 만지도록 두는 대신, 요청 자체를 백엔드로 넘겨 dom0 가 실제 벡터를
 * 배정하게 하고 그 결과(Xen 의 PIRQ 번호)를 받아 온다.
 *
 * 연결 방식은 전역 함수 포인터다. pci_frontend_registrar() 가
 * xen_pci_frontend 전역에 아래 pci_frontend_ops 를 꽂아 두면,
 * Xen 용 MSI 코드가 그 포인터를 통해 이 함수들을 부른다.
 * 그 전역과 struct xen_pci_frontend_ops 의 정의는 이 트리에 없는
 * asm/xen/pci.h 에 있어 여기서는 확인할 수 없다.
 *
 * CONFIG_PCI_MSI 가 꺼져 있으면 아래 #else 의 빈 함수만 남는다.
 */
#ifdef CONFIG_PCI_MSI
/*
 * [한국어]
 * pci_frontend_enable_msix - 백엔드에게 MSI-X 벡터 배정을 요청한다
 *
 * @dev: MSI-X 를 켤 PCI 장치.
 * @vector: 배정된 IRQ 번호를 채워 돌려줄 배열. 호출자가 준비한다.
 * @nvec: 요청하는 벡터 개수.
 * @return: 0 이면 vector[] 가 모두 유효하다. -EINVAL 은 요청 개수가
 *   한도를 넘었거나 백엔드가 일부 벡터를 배정하지 못한 경우.
 *   그 밖의 양수/음수는 백엔드가 돌려준 값이 그대로 전달된다.
 *
 * 동작 단계:
 *  1) nvec 이 SH_INFO_MAX_VEC 이하인지 확인한다. 공유 페이지의
 *     msix_entries 배열 크기가 고정이라 그 이상은 실을 수 없다.
 *     큐마다 벡터를 하나씩 쓰는 장치라면 이 상한이 곧 큐 개수 상한이 된다.
 *     SH_INFO_MAX_VEC 의 실제 값은 이 트리에 없는 pciif.h 에 있어 확인할 수 없다.
 *  2) 아직 IRQ 가 배정되지 않은 MSI 서술자를 순회하며 MSI-X 테이블
 *     인덱스만 공유 페이지에 채운다. 벡터 번호는 이 시점에 의미가 없어
 *     -1 로 둔다(위 영어 주석).
 *  3) do_pci_op() 로 백엔드에 넘긴다. 백엔드가 실제 벡터를 배정한다.
 *  4) 돌아온 msix_entries[i].vector 를 호출자 배열에 옮긴다.
 *
 * op.value 는 입력에서는 요청 벡터 개수, 출력에서는 백엔드의 부가 결과
 * 코드로 쓰인다 - 같은 필드가 방향에 따라 다른 뜻을 갖는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. do_pci_op() 가 폴링 대기를 하므로 잠들 수 있다.
 *
 * 호출 체인:
 *   Xen MSI 코드 -> xen_pci_frontend->enable_msix -> [pci_frontend_enable_msix]
 *     -> do_pci_op()
 */
static int pci_frontend_enable_msix(struct pci_dev *dev,
				    int vector[], int nvec)
{
	int err;	/* [한국어] do_pci_op() 반환값 */
	int i;	/* [한국어] msix_entries 배열 인덱스 */
	struct xen_pci_op op = {	/* [한국어] 백엔드에 보낼 요청. 스택에 만들어 do_pci_op() 가 공유 페이지로 복사한다 */
		.cmd    = XEN_PCI_OP_enable_msix,	/* [한국어] MSI-X 를 켜 달라는 명령 코드 */
		.domain = pci_domain_nr(dev->bus),	/* [한국어] 대상 장치의 PCI 도메인 */
		.bus = dev->bus->number,	/* [한국어] 대상 장치의 버스 번호 */
		.devfn = dev->devfn,	/* [한국어] 대상 device/function 번호 */
		.value = nvec,	/* [한국어] 입력으로서의 value - 요청하는 벡터 개수 */
	};
	struct pcifront_sd *sd = dev->bus->sysdata;	/* [한국어] 버스에 붙여 둔 sysdata */
	struct pcifront_device *pdev = pcifront_get_pdev(sd);	/* [한국어] 그 sysdata 에서 프론트엔드 상태를 꺼낸다 */
	struct msi_desc *entry;	/* [한국어] MSI 서술자 순회용 커서 */

	if (nvec > SH_INFO_MAX_VEC) {	/* [한국어] 공유 페이지의 msix_entries 배열이 고정 크기라 그 이상은 담을 수 없다 */
		pci_err(dev, "too many vectors (0x%x) for PCI frontend:"
				   " Increase SH_INFO_MAX_VEC\n", nvec);	/* [한국어] 한도를 넘겼음을 알리고 상수를 늘리라고 안내한다 */
		return -EINVAL;	/* [한국어] 요청을 보내지 않고 실패시킨다 */
	}

	i = 0;	/* [한국어] 배열 채우기 인덱스를 0 으로 시작한다 */
	msi_for_each_desc(entry, &dev->dev, MSI_DESC_NOTASSOCIATED) {	/* [한국어] 아직 IRQ 가 연결되지 않은(NOTASSOCIATED) MSI 서술자만 순회한다. 이미 배정된 것을 다시 요청하지 않기 위해서다 */
		op.msix_entries[i].entry = entry->msi_index;	/* [한국어] MSI-X 테이블 안에서의 엔트리 번호. 백엔드는 이 번호로 어느 벡터인지 식별한다 */
		/* Vector is useless at this point. */
		op.msix_entries[i].vector = -1;	/* [한국어] 벡터 번호는 백엔드가 채워 줄 출력이므로 -1 로 표시해 둔다 */
		i++;	/* [한국어] 다음 슬롯으로 */
	}

	err = do_pci_op(pdev, &op);	/* [한국어] 공유 페이지에 실어 백엔드에 넘기고 응답을 기다린다 */

	if (likely(!err)) {	/* [한국어] 전송 자체가 성공한 경우 */
		if (likely(!op.value)) {	/* [한국어] 출력으로서의 value 가 0 이면 백엔드가 정상 처리했다는 뜻이다 */
			/* we get the result */
			for (i = 0; i < nvec; i++) {	/* [한국어] 요청한 개수만큼 결과를 옮긴다 */
				if (op.msix_entries[i].vector <= 0) {	/* [한국어] 0 이하는 유효한 IRQ 번호가 아니다 - 이 엔트리는 배정에 실패했다 */
					pci_warn(dev, "MSI-X entry %d is invalid: %d!\n",
						i, op.msix_entries[i].vector);	/* [한국어] 몇 번 엔트리가 실패했는지 남긴다 */
					err = -EINVAL;	/* [한국어] 전체를 실패로 표시한다 */
					vector[i] = -1;	/* [한국어] 호출자가 알아볼 수 있도록 -1 을 채운다 */
					continue;	/* [한국어] 나머지 엔트리도 계속 확인한다 */
				}
				vector[i] = op.msix_entries[i].vector;	/* [한국어] 백엔드가 배정한 IRQ 번호를 호출자 배열에 옮긴다 */
			}
		} else {	/* [한국어] value 가 0 이 아니면 백엔드가 부가 결과 코드를 실어 보낸 것이다 */
			pr_info("enable msix get value %x\n", op.value);	/* [한국어] 그 값을 로그로 남긴다 */
			err = op.value;	/* [한국어] 그대로 반환값으로 쓴다 */
		}
	} else {	/* [한국어] do_pci_op() 자체가 실패한 경우 */
		pci_err(dev, "enable msix get err %x\n", err);	/* [한국어] 전송 오류를 남긴다 */
	}
	return err;	/* [한국어] 0 이면 vector[] 가 모두 유효하다 */
}

/*
 * [한국어]
 * pci_frontend_disable_msix - 백엔드에게 MSI-X 해제를 요청한다
 *
 * @dev: MSI-X 를 끌 PCI 장치.
 * @return: 없음. 실패해도 호출자에게 알릴 수단이 없다.
 *
 * 배정의 짝이다. 개수나 엔트리 목록을 실을 필요가 없어 BDF 만 담아 보낸다.
 * 실패해도 되돌릴 방법이 마땅치 않아 로그만 남긴다 - 위 영어 주석의
 * "What should do for error ?" 가 그 사정을 그대로 드러낸다.
 *
 * 실행 컨텍스트: 프로세스 문맥. do_pci_op() 폴링 때문에 잠들 수 있다.
 *
 * 호출 체인:
 *   Xen MSI 코드 -> xen_pci_frontend->disable_msix -> [pci_frontend_disable_msix]
 *     -> do_pci_op()
 */
static void pci_frontend_disable_msix(struct pci_dev *dev)
{
	int err;	/* [한국어] do_pci_op() 반환값 */
	struct xen_pci_op op = {	/* [한국어] 백엔드에 보낼 요청 */
		.cmd    = XEN_PCI_OP_disable_msix,	/* [한국어] MSI-X 를 꺼 달라는 명령 코드 */
		.domain = pci_domain_nr(dev->bus),	/* [한국어] 대상 도메인 */
		.bus = dev->bus->number,	/* [한국어] 대상 버스 */
		.devfn = dev->devfn,	/* [한국어] 대상 device/function */
	};
	struct pcifront_sd *sd = dev->bus->sysdata;	/* [한국어] 버스 sysdata */
	struct pcifront_device *pdev = pcifront_get_pdev(sd);	/* [한국어] 프론트엔드 상태 */

	err = do_pci_op(pdev, &op);	/* [한국어] 백엔드에 넘기고 응답을 기다린다 */

	/* What should do for error ? */
	if (err)
		pci_err(dev, "pci_disable_msix get err %x\n", err);	/* [한국어] 실패해도 되돌릴 수단이 없어 기록만 남긴다 */
}

/*
 * [한국어]
 * pci_frontend_enable_msi - 백엔드에게 MSI 벡터 하나를 요청한다
 *
 * @dev: MSI 를 켤 PCI 장치.
 * @vector: 배정된 IRQ 번호를 담아 돌려줄 배열. 첫 칸만 쓴다.
 * @return: 0 이면 vector[0] 이 유효하다. -EINVAL 은 전송 실패이거나
 *   백엔드가 유효하지 않은 벡터를 돌려준 경우다.
 *
 * MSI 는 MSI-X 와 달리 벡터가 하나뿐(정확히는 연속된 블록의 시작 하나)이라
 * 엔트리 배열이 필요 없다. 그래서 결과도 op.value 한 칸으로 받는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. do_pci_op() 폴링 때문에 잠들 수 있다.
 *
 * 호출 체인:
 *   Xen MSI 코드 -> xen_pci_frontend->enable_msi -> [pci_frontend_enable_msi]
 *     -> do_pci_op()
 */
static int pci_frontend_enable_msi(struct pci_dev *dev, int vector[])
{
	int err;	/* [한국어] do_pci_op() 반환값 */
	struct xen_pci_op op = {	/* [한국어] 백엔드에 보낼 요청 */
		.cmd    = XEN_PCI_OP_enable_msi,	/* [한국어] MSI 를 켜 달라는 명령 코드 */
		.domain = pci_domain_nr(dev->bus),	/* [한국어] 대상 도메인 */
		.bus = dev->bus->number,	/* [한국어] 대상 버스 */
		.devfn = dev->devfn,	/* [한국어] 대상 device/function */
	};
	struct pcifront_sd *sd = dev->bus->sysdata;	/* [한국어] 버스 sysdata */
	struct pcifront_device *pdev = pcifront_get_pdev(sd);	/* [한국어] 프론트엔드 상태 */

	err = do_pci_op(pdev, &op);	/* [한국어] 백엔드에 넘기고 응답을 기다린다 */
	if (likely(!err)) {	/* [한국어] 전송이 성공한 경우 */
		vector[0] = op.value;	/* [한국어] 출력으로서의 value 가 배정된 IRQ 번호다 */
		if (op.value <= 0) {	/* [한국어] 0 이하는 유효한 IRQ 가 아니다 */
			pci_warn(dev, "MSI entry is invalid: %d!\n",
				op.value);	/* [한국어] 무효한 값을 받았음을 남긴다 */
			err = -EINVAL;	/* [한국어] 실패로 처리한다 */
			vector[0] = -1;	/* [한국어] 호출자가 알아볼 수 있도록 -1 로 덮어쓴다 */
		}
	} else {	/* [한국어] do_pci_op() 자체가 실패한 경우 */
		pci_err(dev, "pci frontend enable msi failed for dev "
				    "%x:%x\n", op.bus, op.devfn);	/* [한국어] 어느 장치에서 실패했는지 버스/devfn 과 함께 남긴다 */
		err = -EINVAL;	/* [한국어] 백엔드 오류 코드 대신 -EINVAL 로 통일해 돌려준다 */
	}
	return err;	/* [한국어] 0 이면 vector[0] 이 유효하다 */
}

/*
 * [한국어]
 * pci_frontend_disable_msi - 백엔드에게 MSI 해제를 요청한다
 *
 * @dev: MSI 를 끌 PCI 장치.
 * @return: 없음.
 *
 * 두 가지 실패를 구분해 로그만 남긴다.
 *  - XEN_PCI_ERR_dev_not_found: do_pci_op() 가 타임아웃했을 때 쓰는 값이기도
 *    하다(do_pci_op 의 타임아웃 경로 참고). 즉 백엔드가 응답하지 않았다는 뜻이다.
 *  - 그 밖의 오류: 백엔드가 실패를 알려 온 경우.
 * 어느 쪽이든 이미 해제를 시도한 뒤라 되돌릴 것이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥. do_pci_op() 폴링 때문에 잠들 수 있다.
 *
 * 호출 체인:
 *   Xen MSI 코드 -> xen_pci_frontend->disable_msi -> [pci_frontend_disable_msi]
 *     -> do_pci_op()
 */
static void pci_frontend_disable_msi(struct pci_dev *dev)
{
	int err;	/* [한국어] do_pci_op() 반환값 */
	struct xen_pci_op op = {	/* [한국어] 백엔드에 보낼 요청 */
		.cmd    = XEN_PCI_OP_disable_msi,	/* [한국어] MSI 를 꺼 달라는 명령 코드 */
		.domain = pci_domain_nr(dev->bus),	/* [한국어] 대상 도메인 */
		.bus = dev->bus->number,	/* [한국어] 대상 버스 */
		.devfn = dev->devfn,	/* [한국어] 대상 device/function */
	};
	struct pcifront_sd *sd = dev->bus->sysdata;	/* [한국어] 버스 sysdata */
	struct pcifront_device *pdev = pcifront_get_pdev(sd);	/* [한국어] 프론트엔드 상태 */

	err = do_pci_op(pdev, &op);	/* [한국어] 백엔드에 넘기고 응답을 기다린다 */
	if (err == XEN_PCI_ERR_dev_not_found) {	/* [한국어] do_pci_op() 는 타임아웃에도 이 값을 쓴다 - 응답이 없었다는 뜻이다 */
		/* XXX No response from backend, what shall we do? */
		pr_info("get no response from backend for disable MSI\n");	/* [한국어] 무응답 사실만 남긴다 */
		return;	/* [한국어] 더 할 수 있는 일이 없어 반환한다 */
	}
	if (err)	/* [한국어] 그 밖의 오류 코드가 온 경우 */
		/* how can pciback notify us fail? */
		pr_info("get fake response from backend\n");	/* [한국어] 백엔드가 실패를 어떻게 알려야 하는지 규약이 없어 로그만 남긴다 */
}

/*
 * [한국어] pci_frontend_ops - PV MSI 처리를 대신할 콜백 묶음.
 *
 * 이 구조체 주소가 전역 xen_pci_frontend 에 꽂히면, 커널의 Xen MSI 경로가
 * 물리 하드웨어를 만지는 대신 여기 등록된 함수를 부른다. 즉 이 네 줄이
 * "게스트의 인터럽트 설정을 하이퍼바이저 쪽으로 넘긴다" 는 정책의 실체다.
 */
static struct xen_pci_frontend_ops pci_frontend_ops = {
	.enable_msi = pci_frontend_enable_msi,	/* [한국어] MSI 켜기 요청을 PV 경로로 넘긴다 */
	.disable_msi = pci_frontend_disable_msi,	/* [한국어] MSI 끄기 요청을 PV 경로로 넘긴다 */
	.enable_msix = pci_frontend_enable_msix,	/* [한국어] MSI-X 켜기 요청을 PV 경로로 넘긴다 */
	.disable_msix = pci_frontend_disable_msix,	/* [한국어] MSI-X 끄기 요청을 PV 경로로 넘긴다 */
};

/*
 * [한국어]
 * pci_frontend_registrar - 전역 MSI 훅을 켜거나 끈다
 *
 * @enable: 0 이 아니면 등록, 0 이면 해제.
 * @return: 없음.
 *
 * 이 훅은 장치가 아니라 시스템 전체에 하나다. 그래서 모듈 init 에서 켜고
 * exit 에서 끈다. 켜는 시점이 xenbus_register_frontend() 보다 앞서야
 * 장치가 붙자마자 MSI 요청을 받아도 훅이 준비돼 있고, 끄는 시점은
 * xenbus_unregister_driver() 뒤여야 아직 살아 있는 장치가 사라진 훅을
 * 참조하지 않는다.
 *
 * 실행 컨텍스트: 모듈 init/exit(프로세스 문맥). 별도 락이 없는데,
 * 이 두 시점에는 경쟁할 상대가 없다는 전제다.
 *
 * 호출 체인:
 *   pcifront_init() / pcifront_cleanup() -> [pci_frontend_registrar]
 */
static void pci_frontend_registrar(int enable)
{
	if (enable)	/* [한국어] 등록 요청 */
		xen_pci_frontend = &pci_frontend_ops;	/* [한국어] 전역 훅에 위 콜백 묶음을 꽂는다. 이후 Xen MSI 코드가 이 경로를 탄다 */
	else	/* [한국어] 해제 요청 */
		xen_pci_frontend = NULL;	/* [한국어] 훅을 비운다. 이후 MSI 요청은 PV 경로를 타지 못한다 */
};
/* [한국어] CONFIG_PCI_MSI 가 꺼진 빌드에서는 위 함수들이 아예 없다. */
#else
/*
 * [한국어]
 * pci_frontend_registrar - CONFIG_PCI_MSI 가 꺼진 빌드용 빈 스텁
 *
 * @enable: 무시된다. 위쪽 실제 구현과 시그니처만 맞춰 둔 것이다.
 * @return: 없음.
 *
 * MSI 지원 없이 빌드하면 pci_frontend_ops 도 xen_pci_frontend 훅도 존재하지
 * 않는다. 그런데 pcifront_init() 과 pcifront_cleanup() 은 #ifdef 밖에 있어
 * 이 이름을 그대로 부른다. 호출부마다 조건부 컴파일을 넣는 대신 아무 일도
 * 하지 않는 인라인 함수를 두어, 컴파일러가 호출 자체를 지워 버리게 한다.
 *
 * 이 빌드에서는 PV MSI 경로가 없으므로, 패스스루된 장치는 MSI/MSI-X 를
 * 이 프론트엔드를 통해 설정할 수 없다. 그 경우 어떤 일이 벌어지는지는
 * Xen MSI 코드 쪽 사정이며 이 트리만으로는 확인할 수 없다.
 *
 * 실행 컨텍스트: 호출자를 따른다. 본문이 비어 있어 부작용이 없다.
 *
 * 호출 체인:
 *   pcifront_init() / pcifront_cleanup() -> [pci_frontend_registrar]
 */
static inline void pci_frontend_registrar(int enable) { };	/* [한국어] 빈 스텁. 호출부를 #ifdef 로 감싸지 않아도 되도록 이름만 맞춰 둔다 */
#endif /* CONFIG_PCI_MSI */	/* [한국어] CONFIG_PCI_MSI 분기 끝 */

/* Claim resources for the PCI frontend as-is, backend won't allow changes */
/*
 * [한국어]
 * pcifront_claim_resource - 장치의 BAR 를 자원 트리에 "있는 그대로" 등록한다
 *
 * @dev: pci_walk_bus() 가 넘겨주는 이 버스의 PCI 장치.
 * @data: pci_walk_bus() 에 넘긴 pcifront_device 포인터(로그 출력용).
 * @return: 항상 0. pci_walk_bus() 는 0 이 아닌 값을 만나면 순회를 중단하므로,
 *   한 장치의 실패로 나머지를 건너뛰지 않도록 언제나 0 을 돌려준다.
 *
 * 일반적인 PCI 열거에서는 커널이 BAR 크기를 재고 주소를 새로 배정한다.
 * 그러나 여기서는 백엔드가 이미 배정해 둔 주소를 게스트가 바꿀 수 없다.
 * 백엔드가 BAR 쓰기를 막기 때문이다(위 영어 주석). 그래서 재배치 없이
 * pci_claim_resource() 로 "이 영역은 내가 쓴다" 고 등록만 한다.
 *
 * 조건 세 개를 모두 만족해야 등록 대상이다.
 *   r->parent 가 NULL   - 아직 자원 트리에 편입되지 않았다
 *   r->start 가 0 이 아님 - 백엔드가 실제 주소를 배정했다
 *   r->flags 가 0 이 아님 - 쓰이는 BAR 다(빈 BAR 는 flags 가 0)
 *
 * 실패해도 치명적이지 않지만, 그 장치는 BAR 를 매핑하지 못해 사실상
 * 쓸 수 없게 된다. 오류 메시지가 e820_host=1 을 권하는 이유는, 게스트의
 * 메모리 맵이 호스트와 달라 BAR 주소가 게스트 RAM 과 겹치는 경우가
 * 흔하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 pci_lock_rescan_remove() 를
 * 잡은 상태에서 불린다.
 *
 * 호출 체인:
 *   pcifront_scan_root()/pcifront_rescan_root() -> pci_walk_bus()
 *     -> [pcifront_claim_resource] -> pci_claim_resource()
 */
static int pcifront_claim_resource(struct pci_dev *dev, void *data)
{
	struct pcifront_device *pdev = data;	/* [한국어] 로그에 XenBus 장치 이름을 찍기 위한 프론트엔드 상태 */
	int i;	/* [한국어] BAR 인덱스 */
	struct resource *r;	/* [한국어] 순회 중인 자원(BAR) */

	pci_dev_for_each_resource(dev, r, i) {	/* [한국어] 이 장치의 모든 자원을 순회한다. i 에는 BAR 번호가 들어온다 */
		if (!r->parent && r->start && r->flags) {	/* [한국어] 아직 트리에 없고, 주소가 배정돼 있고, 실제로 쓰이는 BAR 만 대상이다 */
			dev_info(&pdev->xdev->dev, "claiming resource %s/%d\n",
				pci_name(dev), i);	/* [한국어] 어떤 장치의 몇 번 BAR 를 등록하는지 남긴다 */
			if (pci_claim_resource(dev, i)) {	/* [한국어] 부모 자원(ioport_resource 또는 iomem_resource) 아래에 이 영역을 삽입한다. 겹치면 실패한다 */
				dev_err(&pdev->xdev->dev, "Could not claim resource %s/%d! "
					"Device offline. Try using e820_host=1 in the guest config.\n",
					pci_name(dev), i);	/* [한국어] 실패해도 순회를 계속한다. 그 장치만 쓸 수 없게 될 뿐이다 */
			}
		}
	}

	return 0;	/* [한국어] 항상 0 - pci_walk_bus() 가 중간에 멈추지 않도록 */
}

/*
 * [한국어]
 * pcifront_scan_bus - 버스의 모든 devfn 을 훑어 새 장치를 pci_dev 로 만든다
 *
 * @pdev: 프론트엔드 상태(로그용).
 * @domain: 로그에 찍을 도메인 번호.
 * @bus: 로그에 찍을 버스 번호.
 * @b: 실제로 스캔할 pci_bus.
 * @return: 항상 0.
 *
 * PCI 코어의 pci_scan_root_bus() 는 표준 규칙을 따른다 - 어떤 슬롯의
 * function 0 이 없으면 그 슬롯 전체를 건너뛴다. 물리 하드웨어에서는
 * 맞는 규칙이지만, 패스스루에서는 백엔드가 function 3 하나만 게스트에
 * 노출하는 것이 얼마든지 가능하다. 그래서 여기서 0x00 부터 0xff 까지
 * 256개 devfn 을 빠짐없이 직접 훑는다.
 *
 * 여기서 일어나는 config 접근(pci_scan_single_device 안의 vendor ID 읽기)은
 * 모두 pcifront_bus_read() 를 거쳐 백엔드로 나간다. 즉 이 루프는
 * 최대 256번의 do_pci_op() 왕복을 만든다.
 *
 * 브리지는 다루지 않는다. 백엔드(pciback)가 브리지 자체를 내보내지 않기
 * 때문이라고 위 영어 주석이 밝히고 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥. do_pci_op() 가 폴링 대기를 하므로 잠들 수 있다.
 *
 * 호출 체인:
 *   pcifront_scan_root()/pcifront_rescan_root() -> [pcifront_scan_bus]
 *     -> pci_scan_single_device() -> pcifront_bus_read() -> do_pci_op()
 */
static int pcifront_scan_bus(struct pcifront_device *pdev,
				unsigned int domain, unsigned int bus,
				struct pci_bus *b)
{
	struct pci_dev *d;	/* [한국어] 찾았거나 새로 만든 장치 */
	unsigned int devfn;	/* [한국어] device/function 을 합친 8비트 번호. 상위 5비트가 device, 하위 3비트가 function */

	/*
	 * Scan the bus for functions and add.
	 * We omit handling of PCI bridge attachment because pciback prevents
	 * bridges from being exported.
	 */
	for (devfn = 0; devfn < 0x100; devfn++) {	/* [한국어] 0x100 = 32 device x 8 function. 한 버스의 모든 조합을 훑는다 */
		d = pci_get_slot(b, devfn);	/* [한국어] 이 devfn 에 이미 pci_dev 가 있는지 본다. 있으면 참조가 하나 올라간다 */
		if (d) {	/* [한국어] 재스캔 경로에서는 대부분 이미 알고 있는 장치다 */
			/* Device is already known. */
			pci_dev_put(d);	/* [한국어] pci_get_slot() 이 올린 참조를 즉시 내려놓는다 */
			continue;	/* [한국어] 다음 devfn 으로 넘어간다 */
		}

		d = pci_scan_single_device(b, devfn);	/* [한국어] config 공간을 읽어 장치가 있으면 pci_dev 를 만들어 버스에 매단다. 없으면 NULL */
		if (d)	/* [한국어] 새 장치를 찾은 경우에만 */
			dev_info(&pdev->xdev->dev, "New device on "
				 "%04x:%02x:%02x.%d found.\n", domain, bus,
				 PCI_SLOT(devfn), PCI_FUNC(devfn));	/* [한국어] 발견 사실을 도메인:버스:슬롯.함수 형태로 남긴다 */
	}

	return 0;	/* [한국어] 실패를 보고할 수단이 없어 항상 0 이다. 호출자도 이 값을 오류로 다루지 않는다 */
}

/*
 * [한국어]
 * pcifront_scan_root - PV 루트 버스를 하나 만들고 그 아래 장치를 열거한다
 *
 * @pdev: 프론트엔드 상태. 만들어진 루트 버스가 pdev->root_buses 에 매달린다.
 * @domain: 만들 루트 버스의 PCI 도메인 번호.
 * @bus: 만들 루트 버스의 번호.
 * @return: 0 이면 성공. -ENOMEM 은 할당 실패 또는 버스 생성 실패,
 *   -EINVAL 은 CONFIG_PCI_DOMAINS 없이 0 이 아닌 도메인을 요구받은 경우다.
 *
 * 이 함수가 이 드라이버의 결정적 순간이다. pci_scan_root_bus() 에
 * pcifront_bus_ops 를 넘기는 순간, 이 버스 아래 모든 config 접근이
 * 하드웨어 대신 공유 페이지를 거치게 된다. 게스트의 PCI 코어는 자신이
 * 가짜 버스를 다루고 있다는 사실을 전혀 모른다.
 *
 * 함께 넘기는 sysdata(struct pcifront_sd)는 버스에서 프론트엔드로
 * 되돌아오는 통로다. pcifront_bus_read() 는 bus->sysdata 를 통해
 * 어느 XenBus 채널로 요청을 보낼지 알아낸다.
 *
 * 자원 목록에 ioport_resource 와 iomem_resource 를 통째로 넣는 점에 주의.
 * 백엔드가 이미 주소를 배정해 두었으므로 게스트는 범위를 제한할 필요가 없고,
 * 아래 pcifront_claim_resource() 가 그 주소를 그대로 등록만 한다.
 *
 * 순서도 중요하다. 스캔 -> claim -> add_devices 순인데, add_devices 가
 * 드라이버 바인딩을 유발하므로 그 전에 BAR 등록이 끝나 있어야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. GFP_KERNEL 할당과
 * pci_lock_rescan_remove() 뮤텍스를 쓴다.
 *
 * 호출 체인:
 *   pcifront_rescan_root() -> [pcifront_scan_root]
 *     -> pci_scan_root_bus() -> pcifront_scan_bus()
 *     -> pci_walk_bus(pcifront_claim_resource) -> pci_bus_add_devices()
 */
static int pcifront_scan_root(struct pcifront_device *pdev,
				 unsigned int domain, unsigned int bus)
{
	struct pci_bus *b;	/* [한국어] 만들어진 루트 버스 */
	LIST_HEAD(resources);	/* [한국어] 이 루트에 줄 자원 목록. 스택에 리스트 헤드를 만든다 */
	struct pcifront_sd *sd = NULL;	/* [한국어] 버스와 프론트엔드를 잇는 sysdata. 실패 시 kfree 하려고 NULL 로 시작한다 */
	struct pci_bus_entry *bus_entry = NULL;	/* [한국어] 루트 버스 추적 항목. 역시 NULL 로 시작한다 */
	int err = 0;	/* [한국어] 반환값 */
	/* [한국어] 버스 번호 범위 자원. static 인 이유는 pci_add_resource() 가
	 * 포인터를 자원 목록에 그대로 담아 두어 이 함수가 끝난 뒤에도
	 * 살아 있어야 하기 때문이다. 여러 루트가 같은 객체를 공유한다. */
	static struct resource busn_res = {
		.start = 0,	/* [한국어] 버스 번호 0 부터 */
		.end = 255,	/* [한국어] 255 까지 - PCI 버스 번호는 8비트다 */
		.flags = IORESOURCE_BUS,	/* [한국어] 이 자원이 버스 번호 범위임을 나타내는 플래그 */
	};

/* [한국어] 커널이 PCI 도메인(세그먼트)을 지원하도록 빌드되지 않은 경우,
 * 0 이 아닌 도메인은 표현할 방법이 없다. */
#ifndef CONFIG_PCI_DOMAINS
	if (domain != 0) {	/* [한국어] 도메인 0 이 아니면 이 커널로는 다룰 수 없다 */
		dev_err(&pdev->xdev->dev,
			"PCI Root in non-zero PCI Domain! domain=%d\n", domain);	/* [한국어] 어떤 도메인이 문제인지 알린다 */
		dev_err(&pdev->xdev->dev,
			"Please compile with CONFIG_PCI_DOMAINS\n");	/* [한국어] 해결 방법(빌드 옵션)을 함께 알린다 */
		err = -EINVAL;	/* [한국어] 잘못된 인자로 처리한다 */
		goto err_out;	/* [한국어] 아직 아무것도 할당하지 않았지만 공통 정리 경로로 간다 */
	}
/* [한국어] CONFIG_PCI_DOMAINS 분기 끝. */
#endif

	dev_info(&pdev->xdev->dev, "Creating PCI Frontend Bus %04x:%02x\n",
		 domain, bus);	/* [한국어] 어떤 도메인:버스에 루트를 만드는지 남긴다 */

	bus_entry = kzalloc_obj(*bus_entry);	/* [한국어] 루트 버스 추적 항목을 0 초기화 할당한다 */
	sd = kzalloc_obj(*sd);	/* [한국어] 버스 -> 프론트엔드 역참조용 sysdata 를 0 초기화 할당한다 */
	if (!bus_entry || !sd) {	/* [한국어] 둘 중 하나라도 실패하면 */
		err = -ENOMEM;	/* [한국어] 메모리 부족 */
		goto err_out;	/* [한국어] 둘 다 kfree 하는 공통 경로로 간다. kfree(NULL) 은 안전하다 */
	}
	pci_add_resource(&resources, &ioport_resource);	/* [한국어] I/O 포트 공간 전체를 이 루트의 자원으로 준다 */
	pci_add_resource(&resources, &iomem_resource);	/* [한국어] MMIO 공간 전체를 준다. 장치의 메모리 BAR 가 여기에 등록된다 */
	pci_add_resource(&resources, &busn_res);	/* [한국어] 버스 번호 범위 0-255 를 준다 */
	pcifront_init_sd(sd, domain, bus, pdev);	/* [한국어] sysdata 에 도메인/NUMA 노드와 프론트엔드 역포인터를 채운다 */

	pci_lock_rescan_remove();	/* [한국어] PCI 코어의 재스캔/제거 뮤텍스를 잡는다. 버스를 만들고 장치를 붙이는 동안 다른 재스캔이 끼어들면 안 된다 */

	/* [한국어] 여기가 핵심이다. pcifront_bus_ops 를 이 버스의 pci_ops 로 꽂으면,
	 * 이후 이 버스 아래 모든 config 읽기/쓰기가 pcifront_bus_read/write 를
	 * 거쳐 공유 페이지와 이벤트 채널로 백엔드에 위임된다.
	 * sd 는 bus->sysdata 가 되어 다시 이 pdev 를 찾아오는 통로가 된다. */
	b = pci_scan_root_bus(&pdev->xdev->dev, bus,
				  &pcifront_bus_ops, sd, &resources);	/* [한국어] 버스 번호, ops, sysdata, 자원 목록을 넘겨 루트 버스를 만든다 */
	if (!b) {	/* [한국어] 버스 생성 실패 */
		dev_err(&pdev->xdev->dev,
			"Error creating PCI Frontend Bus!\n");	/* [한국어] 실패 사실을 남긴다 */
		err = -ENOMEM;	/* [한국어] 메모리 부족으로 보고한다 */
		pci_unlock_rescan_remove();	/* [한국어] 뮤텍스를 먼저 푼다 */
		pci_free_resource_list(&resources);	/* [한국어] 자원 목록에 붙여 둔 항목들을 해제한다. 버스가 인수하지 못했으므로 우리가 치운다 */
		goto err_out;	/* [한국어] bus_entry 와 sd 를 해제하는 경로로 간다 */
	}

	bus_entry->bus = b;	/* [한국어] 나중에 해체할 수 있도록 버스를 기록한다 */

	list_add(&bus_entry->list, &pdev->root_buses);	/* [한국어] 프론트엔드의 루트 버스 목록에 매단다. pcifront_free_roots() 가 이 목록을 순회한다 */

	/*
	 * pci_scan_root_bus skips devices which do not have a
	 * devfn==0. The pcifront_scan_bus enumerates all devfn.
	 */
	err = pcifront_scan_bus(pdev, domain, bus, b);	/* [한국어] function 0 이 없는 슬롯까지 포함해 모든 devfn 을 다시 훑는다 */

	/* Claim resources before going "live" with our devices */
	pci_walk_bus(b, pcifront_claim_resource, pdev);	/* [한국어] 드라이버가 붙기 전에 모든 BAR 를 자원 트리에 등록한다 */

	/* Create SysFS and notify udev of the devices. Aka: "going live" */
	pci_bus_add_devices(b);	/* [한국어] sysfs 노출과 uevent 발생. 이 호출이 드라이버 바인딩을 유발한다 */

	pci_unlock_rescan_remove();	/* [한국어] 뮤텍스 해제 */
	return err;	/* [한국어] pcifront_scan_bus() 가 항상 0 이므로 실질적으로 0 이다 */

/* [한국어] 할당 실패, 도메인 오류, 버스 생성 실패가 모두 모이는 정리 경로. */
err_out:
	kfree(bus_entry);	/* [한국어] kfree(NULL) 은 안전하므로 조건 검사 없이 부른다 */
	kfree(sd);	/* [한국어] sd 도 마찬가지 */

	return err;	/* [한국어] -EINVAL 또는 -ENOMEM */
}

/*
 * [한국어]
 * pcifront_rescan_root - 이미 있는 루트면 재스캔하고, 없으면 새로 만든다
 *
 * @pdev: 프론트엔드 상태.
 * @domain: PCI 도메인 번호.
 * @bus: 루트 버스 번호.
 * @return: 0 이면 성공. 음수는 pcifront_scan_root() 의 실패(-ENOMEM 등).
 *
 * pcifront_connect() 는 최초 연결과 핫플러그 재구성 양쪽에서 불린다.
 * 두 경우를 구분하지 않고 같은 코드로 처리하기 위해, 여기서 버스의
 * 존재 여부만 보고 갈래를 나눈다. 이미 있으면 새 장치만 찾아 붙이고,
 * 없으면 루트 버스 자체를 만든다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 재스캔 경로에서는 pci_lock_rescan_remove() 를
 * 잡지 않는데, 이는 이 코드를 그대로 읽은 사실이다. 새로 만드는 경로
 * (pcifront_scan_root)에서는 잡는다.
 *
 * 호출 체인:
 *   pcifront_connect() -> [pcifront_rescan_root]
 *     -> pcifront_scan_root() (버스가 없을 때)
 *     -> pcifront_scan_bus() + pci_walk_bus() + pci_bus_add_devices()
 */
static int pcifront_rescan_root(struct pcifront_device *pdev,
				   unsigned int domain, unsigned int bus)
{
	int err;	/* [한국어] 하위 호출의 반환값 */
	struct pci_bus *b;	/* [한국어] 찾아낸 기존 버스 */

	b = pci_find_bus(domain, bus);	/* [한국어] 도메인/번호로 이미 등록된 PCI 버스를 찾는다 */
	if (!b)
		/* If the bus is unknown, create it. */
		return pcifront_scan_root(pdev, domain, bus);	/* [한국어] 버스가 없으면 루트 버스 생성 경로로 넘긴다. 핫플러그로 새 루트가 생긴 경우다 */

	dev_info(&pdev->xdev->dev, "Rescanning PCI Frontend Bus %04x:%02x\n",
		 domain, bus);	/* [한국어] 기존 버스를 다시 스캔한다는 사실을 로그로 남긴다 */

	err = pcifront_scan_bus(pdev, domain, bus, b);	/* [한국어] devfn 0x00 부터 0xff 까지 훑어 새로 나타난 장치를 pci_dev 로 만든다 */

	/* Claim resources before going "live" with our devices */
	pci_walk_bus(b, pcifront_claim_resource, pdev);	/* [한국어] 새로 만든 pci_dev 들의 BAR 를 자원 트리에 등록한다. 드라이버가 붙기 전이어야 한다 */

	/* Create SysFS and notify udev of the devices. Aka: "going live" */
	pci_bus_add_devices(b);	/* [한국어] sysfs 에 노출하고 uevent 를 올려 드라이버 바인딩을 유발한다 */

	return err;	/* [한국어] pcifront_scan_bus() 는 항상 0 을 돌려주므로 실질적으로 0 이다 */
}

/*
 * [한국어]
 * free_root_bus_devs - 한 루트 버스에 매달린 PCI 장치를 모두 제거한다
 *
 * @bus: 비울 루트 버스.
 * @return: 없음.
 *
 * 리스트가 빌 때까지 첫 항목을 꺼내 제거하는 구조다. 흔한
 * list_for_each_entry_safe 대신 while 을 쓰는 이유는,
 * pci_stop_and_remove_bus_device() 가 대상 장치뿐 아니라 그 아래 버스의
 * 장치들까지 함께 지울 수 있어 next 포인터를 미리 잡아 두면 위험하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자(pcifront_free_roots)가
 * pci_lock_rescan_remove() 를 이미 잡고 있어야 한다.
 *
 * 호출 체인:
 *   pcifront_free_roots() -> [free_root_bus_devs]
 *     -> pci_stop_and_remove_bus_device()
 */
static void free_root_bus_devs(struct pci_bus *bus)
{
	struct pci_dev *dev;	/* [한국어] 현재 제거 중인 장치 */

	while (!list_empty(&bus->devices)) {	/* [한국어] 리스트가 빌 때까지 반복한다. 매 회 처음부터 다시 읽는 것이 핵심이다 */
		dev = container_of(bus->devices.next, struct pci_dev,
				   bus_list);	/* [한국어] bus_list 멤버 주소에서 바깥 pci_dev 를 복원한다 */
		pci_dbg(dev, "removing device\n");	/* [한국어] 어떤 장치를 지우는지 로그로 남긴다 */
		pci_stop_and_remove_bus_device(dev);	/* [한국어] 드라이버 remove 를 부르고 sysfs 에서 지운 뒤 리스트에서 뗀다. 이 호출이 리스트를 줄이므로 루프가 끝난다 */
	}
}

/*
 * [한국어]
 * pcifront_free_roots - 이 프론트엔드가 만든 모든 루트 버스를 해체한다
 *
 * @pdev: 프론트엔드 상태.
 * @return: 없음.
 *
 * pcifront_scan_root() 가 만든 것들을 역순으로 되돌린다.
 * 루트 하나마다 (1) 장치 제거 -> (2) sysdata(pcifront_sd) 해제 ->
 * (3) 브리지 device 등록 해제 -> (4) 버스 제거 -> (5) 추적 항목 해제 순이다.
 *
 * sysdata 를 버스 제거보다 먼저 free 하는 점에 주의. 이 시점에는
 * 이미 모든 장치가 사라져 config 접근이 없으므로 sysdata 를 참조할 코드가
 * 남아 있지 않다는 전제다.
 *
 * 실행 컨텍스트: 프로세스 문맥. pci_lock_rescan_remove() 뮤텍스를 잡으므로
 * 잠들 수 있다. 이 뮤텍스는 PCI 코어의 재스캔/제거 전체를 직렬화한다.
 *
 * 호출 체인:
 *   pcifront_try_disconnect() / free_pdev() -> [pcifront_free_roots]
 *     -> free_root_bus_devs() -> pci_remove_bus()
 */
static void pcifront_free_roots(struct pcifront_device *pdev)
{
	struct pci_bus_entry *bus_entry, *t;	/* [한국어] 순회 커서와 안전 순회용 임시 포인터 */

	dev_dbg(&pdev->xdev->dev, "cleaning up root buses\n");	/* [한국어] 정리 시작을 로그로 남긴다 */

	pci_lock_rescan_remove();	/* [한국어] PCI 코어의 재스캔/제거와 경쟁하지 않도록 전역 뮤텍스를 잡는다 */
	list_for_each_entry_safe(bus_entry, t, &pdev->root_buses, list) {	/* [한국어] 항목을 지우면서 순회하므로 _safe 판을 쓴다 */
		list_del(&bus_entry->list);	/* [한국어] 먼저 리스트에서 떼어 낸다. 이후 이 항목은 우리만 참조한다 */

		free_root_bus_devs(bus_entry->bus);	/* [한국어] 이 루트에 매달린 PCI 장치를 모두 제거한다 */

		kfree(bus_entry->bus->sysdata);	/* [한국어] pcifront_scan_root() 에서 kzalloc 한 struct pcifront_sd 를 해제한다 */

		device_unregister(bus_entry->bus->bridge);	/* [한국어] 루트 버스의 브리지 device 객체 등록을 해제한다 */
		pci_remove_bus(bus_entry->bus);	/* [한국어] PCI 코어의 버스 목록에서 이 버스를 제거한다 */

		kfree(bus_entry);	/* [한국어] 추적용 항목 자체를 해제한다 */
	}
	pci_unlock_rescan_remove();	/* [한국어] 뮤텍스 해제. 이제 다른 재스캔이 진행될 수 있다 */
}

/*
 * [한국어]
 * pcifront_common_process - 백엔드가 보낸 AER 단계를 해당 드라이버 콜백으로 넘긴다
 *
 * @cmd: AER 단계. XEN_PCI_OP_aer_detected / aer_mmio / aer_slotreset / aer_resume
 *   중 하나이며, 값의 정의는 이 트리에 없는 xen/interface/io/pciif.h 에 있다.
 * @pdev: 프론트엔드 상태. 대상 장치의 BDF 는 sh_info->aer_op 에서 읽는다.
 * @state: 채널 상태(pci_channel_state_t). error_detected 콜백에만 전달된다.
 * @return: 드라이버 콜백이 돌려준 pci_ers_result_t. 처리할 드라이버가 없거나
 *   resume 단계면 PCI_ERS_RESULT_NONE. 호출자(pcifront_do_aer)가 이 값을
 *   공유 페이지에 써서 백엔드에게 돌려준다.
 *
 * AER(Advanced Error Reporting)은 PCIe 링크/장치 오류를 보고하고 복구하는
 * 스펙상의 절차다. PV 게스트는 AER 레지스터에 직접 접근하지 못하므로,
 * dom0 의 백엔드가 오류를 감지해 "지금 error_detected 를 부르라",
 * "이제 slot_reset 을 하라" 는 식으로 단계를 대신 지시한다.
 * 이 함수는 그 지시를 게스트 안에 바인딩된 드라이버의
 * struct pci_error_handlers 콜백으로 번역한다.
 *
 * 참조 카운트 주의: pci_get_domain_bus_and_slot() 은 참조를 하나 올린다.
 * 실패 경로에서는 pci_dev_put() 으로 내려놓지만, 성공 경로에서는
 * 콜백 결과를 그대로 return 해 버려 참조를 내려놓지 않는다.
 * 이 코드를 그대로 읽은 사실이며, 의도인지 여부는 이 트리만으로 알 수 없다.
 *
 * 실행 컨텍스트: 워크큐 스레드(pcifront_do_aer). 드라이버 콜백은 잠들 수
 * 있어야 하므로 인터럽트 문맥에서는 부를 수 없고, 그래서 인터럽트 핸들러가
 * 직접 처리하지 않고 워크큐로 넘긴다.
 *
 * 호출 체인:
 *   pcifront_do_aer() -> [pcifront_common_process]
 *     -> pdrv->err_handler->error_detected/mmio_enabled/slot_reset/resume
 */
static pci_ers_result_t pcifront_common_process(int cmd,
						struct pcifront_device *pdev,
						pci_channel_state_t state)
{
	struct pci_driver *pdrv;	/* [한국어] 대상 장치에 바인딩된 PCI 드라이버 */
	int bus = pdev->sh_info->aer_op.bus;	/* [한국어] 백엔드가 공유 페이지에 써 넣은 대상 버스 번호 */
	int devfn = pdev->sh_info->aer_op.devfn;	/* [한국어] 대상 device/function 번호 */
	int domain = pdev->sh_info->aer_op.domain;	/* [한국어] 대상 PCI 도메인 번호 */
	struct pci_dev *pcidev;	/* [한국어] BDF 로 찾아낸 pci_dev */

	dev_dbg(&pdev->xdev->dev,
		"pcifront AER process: cmd %x (bus:%x, devfn%x)",
		cmd, bus, devfn);	/* [한국어] 어떤 단계를 어느 장치에 적용하는지 디버그 로그로 남긴다 */

	pcidev = pci_get_domain_bus_and_slot(domain, bus, devfn);	/* [한국어] 도메인/버스/devfn 으로 pci_dev 를 찾는다. 성공 시 참조 카운트가 1 올라간다 */
	if (!pcidev || !pcidev->dev.driver) {	/* [한국어] 장치가 없거나 아직 드라이버가 붙지 않은 경우 */
		dev_err(&pdev->xdev->dev, "device or AER driver is NULL\n");	/* [한국어] 처리할 대상이 없음을 로그로 남긴다 */
		pci_dev_put(pcidev);	/* [한국어] pcidev 가 NULL 이어도 안전하다. 참조를 얻었다면 여기서 내려놓는다 */
		return PCI_ERS_RESULT_NONE;	/* [한국어] "처리하지 않았다" 는 뜻의 결과를 돌려준다 */
	}
	pdrv = to_pci_driver(pcidev->dev.driver);	/* [한국어] device_driver 에서 감싸는 pci_driver 를 얻는다 */

	if (pdrv->err_handler && pdrv->err_handler->error_detected) {	/* [한국어] err_handler 자체가 없거나 error_detected 가 없으면 AER 을 다룰 수 없는 드라이버다 */
		pci_dbg(pcidev, "trying to call AER service\n");	/* [한국어] 콜백 호출 직전을 로그로 남긴다 */
		switch (cmd) {	/* [한국어] 백엔드가 지시한 단계에 따라 분기한다 */
		/* [한국어] 1단계: 오류가 감지됐다. 드라이버는 I/O 를 멈추고 복구 가능 여부를 답한다. */
		case XEN_PCI_OP_aer_detected:
			return pdrv->err_handler->error_detected(pcidev, state);	/* [한국어] 채널 상태를 넘긴다. 반환값이 그대로 백엔드에 전달된다 */
		/* [한국어] 2단계: MMIO 접근이 다시 가능해졌다. 드라이버가 상태를 점검한다. */
		case XEN_PCI_OP_aer_mmio:
			return pdrv->err_handler->mmio_enabled(pcidev);	/* [한국어] mmio_enabled 는 error_detected 가 있으면 있다고 가정하고 부른다 */
		/* [한국어] 3단계: 슬롯 리셋이 수행됐다. 드라이버는 장치를 재초기화한다. */
		case XEN_PCI_OP_aer_slotreset:
			return pdrv->err_handler->slot_reset(pcidev);	/* [한국어] slot_reset 역시 NULL 검사 없이 부른다 */
		/* [한국어] 4단계: 복구 완료. 드라이버가 정상 동작을 재개한다. */
		case XEN_PCI_OP_aer_resume:
			pdrv->err_handler->resume(pcidev);	/* [한국어] resume 은 반환값이 없다 */
			return PCI_ERS_RESULT_NONE;	/* [한국어] 그래서 결과 없음을 돌려준다 */
		/* [한국어] 위 네 가지가 아니면 백엔드가 알 수 없는 명령을 보낸 것이다. */
		default:
			dev_err(&pdev->xdev->dev,
				"bad request in aer recovery operation!\n");	/* [한국어] 오류만 기록하고 아래로 떨어져 PCI_ERS_RESULT_NONE 을 돌려준다 */
		}
	}

	/* [한국어] err_handler 가 없었거나 default 로 떨어진 경우의 공통 반환. */
	return PCI_ERS_RESULT_NONE;	/* [한국어] 백엔드는 이 값을 보고 "프론트엔드가 처리하지 못했다" 고 판단한다 */
}


/*
 * [한국어]
 * pcifront_do_aer - 워크큐에서 AER 요청 한 건을 처리하고 백엔드에 응답한다
 *
 * @data: INIT_WORK 으로 등록된 work_struct. container_of 로 pcifront_device 를 복원한다.
 * @return: 없음.
 *
 * 이 함수가 공유 페이지의 "역방향" 절반을 담당한다. 지금까지의 함수들은
 * 게스트가 요청을 내고 백엔드가 답하는 방향이었지만, AER 은 백엔드가
 * 요청을 내고 게스트가 답한다. 그 요청 슬롯이 sh_info->aer_op 이고,
 * 진행 표시 비트가 _XEN_PCIB_active(B = backend 가 세운다)다.
 *
 * 처리 순서:
 *  1) aer_op 에서 명령과 채널 상태를 읽는다.
 *  2) pcifront_common_process() 로 드라이버 콜백을 부른다.
 *  3) 결과를 aer_op.err 에 되쓴다 - 같은 필드가 입력(채널 상태)과
 *     출력(복구 결과) 양쪽으로 쓰인다.
 *  4) wmb() 로 결과 쓰기를 먼저 보이게 한 뒤 _XEN_PCIB_active 를 내리고
 *     이벤트 채널로 백엔드를 깨운다. 순서가 뒤집히면 백엔드가 비트만 보고
 *     아직 쓰이지 않은 결과를 읽어 갈 수 있다.
 *  5) _PDEVB_op_active 를 내려 다음 AER 요청을 큐에 넣을 수 있게 하고,
 *     그 사이에 새 요청이 도착했을 수 있으므로 다시 한 번 스케줄을 시도한다.
 *
 * 위쪽 영어 주석은 "config op 가 진행 중이면 끝날 때까지 기다려야 한다" 고
 * 말하지만, 이 함수 본문에는 명시적인 대기가 없다. 실제 순서 맞춤은
 * do_pci_op() 쪽에서 이뤄진다 - 그 함수가 응답을 기다리는 동안
 * _XEN_PCIB_active 를 발견하면 schedule_pcifront_aer_op() 를 다시 부른다.
 *
 * 실행 컨텍스트: 시스템 워크큐 스레드. 잠들 수 있다. 같은 pdev 의 이 워크는
 * _PDEVB_op_active 비트 덕분에 동시에 두 개가 돌지 않는다.
 *
 * 호출 체인:
 *   schedule_work() -> [pcifront_do_aer] -> pcifront_common_process()
 *                                        -> notify_remote_via_evtchn()
 *                                        -> schedule_pcifront_aer_op()
 */
static void pcifront_do_aer(struct work_struct *data)
{
	struct pcifront_device *pdev =
		container_of(data, struct pcifront_device, op_work);	/* [한국어] work_struct 가 박혀 있는 바깥 구조체를 되찾는다 */
	int cmd = pdev->sh_info->aer_op.cmd;	/* [한국어] 백엔드가 지시한 AER 단계 */
	pci_channel_state_t state =
		(pci_channel_state_t)pdev->sh_info->aer_op.err;	/* [한국어] 같은 aer_op.err 필드를 입력으로 읽는다 - 백엔드가 여기에 채널 상태를 실어 보낸다 */

	/*
	 * If a pci_conf op is in progress, we have to wait until it is done
	 * before service aer op
	 */
	dev_dbg(&pdev->xdev->dev,
		"pcifront service aer bus %x devfn %x\n",
		pdev->sh_info->aer_op.bus, pdev->sh_info->aer_op.devfn);	/* [한국어] 어느 장치의 AER 을 처리하는지 로그로 남긴다 */

	pdev->sh_info->aer_op.err = pcifront_common_process(cmd, pdev, state);	/* [한국어] 드라이버 콜백을 부르고 그 결과를 같은 필드에 되쓴다. 이제 이 필드는 출력이다 */

	/* Post the operation to the guest. */
	wmb();	/* [한국어] 쓰기 배리어. 위의 결과 저장이 아래의 비트 클리어보다 먼저 백엔드에게 보이도록 강제한다. 순서가 뒤집히면 백엔드가 낡은 결과를 읽는다 */
	clear_bit(_XEN_PCIB_active, (unsigned long *)&pdev->sh_info->flags);	/* [한국어] _XEN_PCIB_active 를 내려 "이 AER 요청 처리 완료" 를 알린다 */
	notify_remote_via_evtchn(pdev->evtchn);	/* [한국어] 이벤트 채널로 백엔드를 깨운다. 백엔드는 비트를 다시 확인하고 결과를 읽어 간다 */

	/*in case of we lost an aer request in four lines time_window*/
	smp_mb__before_atomic();	/* [한국어] 아래 clear_bit 앞뒤로 배리어를 둔다. 비트를 내리기 전의 모든 쓰기가 먼저 보여야 하기 때문이다 */
	clear_bit(_PDEVB_op_active, &pdev->flags);	/* [한국어] _PDEVB_op_active 를 내려 다음 AER 워크를 큐에 넣을 수 있게 만든다 */
	smp_mb__after_atomic();	/* [한국어] 비트를 내린 사실이 다른 CPU 에 보인 뒤에 아래 재검사가 이뤄지도록 한다 */

	schedule_pcifront_aer_op(pdev);	/* [한국어] 비트를 내리기 직전에 도착한 요청은 test_and_set_bit 에서 튕겨 나갔을 수 있다. 그 요청을 놓치지 않도록 여기서 한 번 더 확인한다 */

}

/*
 * [한국어]
 * pcifront_handler_aer - 이벤트 채널 인터럽트 핸들러
 *
 * @irq: bind_evtchn_to_irqhandler() 가 배정한 Linux IRQ 번호. 쓰지 않는다.
 * @dev: 등록 시 넘긴 pcifront_device 포인터.
 * @return: 항상 IRQ_HANDLED. 이 IRQ 는 이 프론트엔드 전용이므로
 *   공유 IRQ 처럼 IRQ_NONE 을 돌려줄 상황이 없다.
 *
 * 백엔드는 config 응답과 AER 요청을 같은 이벤트 채널로 보낸다.
 * 그래서 이 핸들러는 두 경우 모두에 대해 불릴 수 있는데,
 * config 응답 대기는 do_pci_op() 안의 폴링이 맡으므로 여기서는
 * AER 쪽만 신경 쓴다. schedule_pcifront_aer_op() 이 _XEN_PCIB_active 비트를
 * 확인하므로, AER 요청이 아니면 아무 일도 일어나지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 잠들 수 없다. 그래서 실제 처리는
 * 워크큐로 넘기고 즉시 반환한다.
 *
 * 호출 체인:
 *   Xen 이벤트 채널 -> IRQ 코어 -> [pcifront_handler_aer]
 *     -> schedule_pcifront_aer_op() -> schedule_work()
 */
static irqreturn_t pcifront_handler_aer(int irq, void *dev)
{
	struct pcifront_device *pdev = dev;	/* [한국어] 등록 시 넘긴 인자를 프론트엔드 상태로 되돌린다 */

	schedule_pcifront_aer_op(pdev);	/* [한국어] AER 요청이 실제로 있고 이미 처리 중이 아닐 때만 워크를 큐에 넣는다 */
	return IRQ_HANDLED;	/* [한국어] 이 IRQ 는 전용이므로 항상 처리했다고 답한다 */
}
/*
 * [한국어]
 * pcifront_connect_and_init_dma - 전역 프론트엔드 포인터를 이 pdev 로 등록한다
 *
 * @pdev: 등록하려는 프론트엔드 상태.
 * @return: 0 이면 이 pdev 가 전역 포인터의 주인이 되었다.
 *   -EEXIST 면 다른 pdev 가 이미 등록돼 있다는 뜻이며, 호출자들은 이 값을
 *   실패로 취급하지 않는다(정상적으로 있을 수 있는 상황이다).
 *
 * pcifront_dev 는 이 파일 안에서만 쓰이는 static 전역이고, 이 파일에서
 * 그 값을 읽는 곳은 pcifront_disconnect() 의 동일성 비교 하나뿐이다.
 * 즉 지금 코드에서의 실질적 역할은 "프론트엔드는 하나만" 이라는 배타 조건을
 * 강제하는 것이다. 함수 이름에 DMA 가 들어 있으나, 이 트리의 코드에는
 * DMA 나 swiotlb 관련 호출이 남아 있지 않다(이 파일 전수 확인).
 * 이름의 유래는 이 트리만으로는 확인할 수 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥(XenBus 워커). spin_lock() 을 쓰므로
 * 임계 구역 안에서 잠들면 안 되는데, 실제로 하는 일은 포인터 대입뿐이다.
 * 인터럽트 문맥에서는 이 전역을 건드리지 않으므로 irqsave 판이 필요 없다.
 *
 * 호출 체인:
 *   pcifront_try_connect() / pcifront_detach_devices()
 *     -> [pcifront_connect_and_init_dma]
 */
static int pcifront_connect_and_init_dma(struct pcifront_device *pdev)
{
	int err = 0;	/* [한국어] 반환값. 등록에 성공하면 0 그대로 나간다 */

	spin_lock(&pcifront_dev_lock);	/* [한국어] 전역 포인터를 보호한다. 여러 XenBus 채널이 동시에 등록을 시도할 수 있다 */

	if (!pcifront_dev) {	/* [한국어] 아직 아무도 등록하지 않았다면 이 pdev 가 주인이 된다 */
		dev_info(&pdev->xdev->dev, "Installing PCI frontend\n");	/* [한국어] 설치 사실을 로그로 남긴다 */
		pcifront_dev = pdev;	/* [한국어] 전역 포인터를 이 pdev 로 세운다. 해제는 pcifront_disconnect() 가 한다 */
	} else	/* [한국어] 이미 다른 pdev 가 등록돼 있는 경우 */
		err = -EEXIST;	/* [한국어] 중복임을 알린다. 호출자는 이 값을 오류로 보지 않는다 */

	spin_unlock(&pcifront_dev_lock);	/* [한국어] 임계 구역 종료 */

	return err;	/* [한국어] 0 또는 -EEXIST */
}

/*
 * [한국어]
 * pcifront_disconnect - 전역 프론트엔드 포인터가 이 pdev 면 지운다
 *
 * @pdev: 물러나려는 프론트엔드 상태.
 * @return: 없음.
 *
 * pcifront_connect_and_init_dma() 의 짝이다. 전역 포인터가 자신을 가리킬
 * 때만 지운다 - 다른 pdev 가 주인이라면 남의 등록을 지워서는 안 되기 때문이다.
 * 그래서 -EEXIST 를 받고도 나중에 이 함수를 호출하는 경로가 안전하다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 포인터 비교와 대입만 하므로 스핀락으로 충분하다.
 *
 * 호출 체인:
 *   pcifront_try_disconnect() -> [pcifront_disconnect]
 */
static void pcifront_disconnect(struct pcifront_device *pdev)
{
	spin_lock(&pcifront_dev_lock);	/* [한국어] 전역 포인터를 보호한다 */

	if (pdev == pcifront_dev) {	/* [한국어] 전역 포인터가 정확히 이 pdev 일 때만 지운다 */
		dev_info(&pdev->xdev->dev,
			 "Disconnecting PCI Frontend Buses\n");	/* [한국어] 해제 사실을 로그로 남긴다 */
		pcifront_dev = NULL;	/* [한국어] 다음 프론트엔드가 등록할 수 있도록 비운다 */
	}

	spin_unlock(&pcifront_dev_lock);	/* [한국어] 임계 구역 종료 */
}
/*
 * [한국어]
 * alloc_pdev - 프론트엔드 상태 구조체와 백엔드와 공유할 페이지를 만든다
 *
 * @xdev: XenBus 코어가 넘겨준 프론트엔드 장치.
 * @return: 초기화된 struct pcifront_device 포인터. 실패 시 NULL.
 *   호출자(pcifront_xenbus_probe)는 NULL 이면 -ENOMEM 을 반환한다.
 *
 * 여기서 만들어지는 공유 페이지가 이 드라이버의 심장이다.
 * xenbus_setup_ring() 은 페이지를 할당하고, 그 페이지를 백엔드 도메인이
 * 매핑할 수 있도록 grant 를 발급해 참조 번호를 gnt_ref 에 돌려준다.
 * 이후 게스트의 모든 config 접근은 이 페이지 안의 op 슬롯에 실려 나간다.
 *
 * 주의: 이름은 "ring" 이지만 여기서는 페이지 하나(nr_pages=1)뿐이고,
 * 이 파일이 접근하는 필드는 sh_info->op 하나와 sh_info->aer_op 하나다.
 * 즉 진행 중인 요청은 방향마다 한 건뿐이며, 그래서 do_pci_op() 가
 * sh_info_lock 으로 전체를 직렬화한다. struct xen_pci_sharedinfo 의 정의는
 * 이 트리에 없는 xen/interface/io/pciif.h 에 있어 직접 확인할 수 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥(XenBus probe). GFP_KERNEL 할당을 한다.
 *
 * 호출 체인:
 *   pcifront_xenbus_probe() -> [alloc_pdev] -> xenbus_setup_ring()
 */
static struct pcifront_device *alloc_pdev(struct xenbus_device *xdev)
{
	struct pcifront_device *pdev;	/* [한국어] 만들어서 돌려줄 프론트엔드 상태 */

	pdev = kzalloc_obj(struct pcifront_device);	/* [한국어] 0 으로 채워 할당한다. 뒤에서 명시적으로 채우지 않는 필드는 0 이어야 한다 */
	if (pdev == NULL)	/* [한국어] 할당 실패 */
		goto out;	/* [한국어] pdev 가 NULL 인 채로 반환된다 */

	/* [한국어] 공유 페이지 1개를 할당하고 백엔드가 매핑할 수 있도록 grant 를 발급한다.
	 * 성공하면 sh_info 는 게스트 가상 주소, gnt_ref 는 백엔드에게 알려 줄
	 * 허가표 번호다. XenStore 의 "pci-op-ref" 로 발행된다. */
	if (xenbus_setup_ring(xdev, GFP_KERNEL, (void **)&pdev->sh_info, 1,
			      &pdev->gnt_ref)) {	/* [한국어] 실패 시 0 이 아닌 값을 돌려준다 */
		kfree(pdev);	/* [한국어] 공유 페이지가 없으면 통신할 수 없으므로 구조체도 버린다 */
		pdev = NULL;	/* [한국어] 호출자에게 실패를 알리기 위해 NULL 로 만든다 */
		goto out;	/* [한국어] 할당 실패 경로로 빠진다 */
	}
	pdev->sh_info->flags = 0;	/* [한국어] 공유 플래그를 명시적으로 0 으로 초기화한다. 백엔드도 이 필드를 읽으므로 쓰레기 값이 남아서는 안 된다 */

	/*Flag for registering PV AER handler*/
	set_bit(_XEN_PCIB_AERHANDLER, (void *)&pdev->sh_info->flags);	/* [한국어] _XEN_PCIB_AERHANDLER 비트를 세워 "이 프론트엔드는 PV AER 요청을 처리할 수 있다" 고 백엔드에 광고한다. 이 비트가 없으면 백엔드는 AER 을 넘기지 않는다 */

	dev_set_drvdata(&xdev->dev, pdev);	/* [한국어] XenBus 장치에 이 구조체를 매단다. 이후 콜백들이 dev_get_drvdata 로 되찾는다 */
	pdev->xdev = xdev;	/* [한국어] 반대 방향 포인터. 로그 출력과 XenStore 접근에 쓰인다 */

	INIT_LIST_HEAD(&pdev->root_buses);	/* [한국어] 아직 루트 버스가 없으므로 빈 리스트로 시작한다 */

	spin_lock_init(&pdev->sh_info_lock);	/* [한국어] 공유 페이지 접근을 직렬화할 스핀락을 초기화한다 */

	pdev->evtchn = INVALID_EVTCHN;	/* [한국어] 아직 이벤트 채널이 없음을 나타내는 표식. free_pdev() 가 이 값과 비교해 해제 여부를 정한다 */
	pdev->irq = -1;	/* [한국어] 아직 IRQ 가 없음을 나타내는 표식. free_pdev() 가 0 이상일 때만 unbind 한다 */

	INIT_WORK(&pdev->op_work, pcifront_do_aer);	/* [한국어] AER 요청을 처리할 워크 아이템을 준비한다. 인터럽트 핸들러는 이것을 큐에 넣기만 한다 */

	dev_dbg(&xdev->dev, "Allocated pdev @ 0x%p pdev->sh_info @ 0x%p\n",
		pdev, pdev->sh_info);	/* [한국어] 할당된 주소를 디버그 로그에 남긴다 */
/* [한국어] 성공과 실패가 함께 지나가는 반환 지점. 실패면 pdev 가 NULL 이다. */
out:
	return pdev;	/* [한국어] NULL 이면 호출자가 -ENOMEM 으로 바꾼다 */
}

/*
 * [한국어]
 * free_pdev - 프론트엔드가 잡고 있던 모든 자원을 순서대로 해제한다
 *
 * @pdev: 해제할 프론트엔드 상태. 호출 후에는 무효 포인터가 된다.
 * @return: 없음.
 *
 * 해제 순서가 곧 안전성이다.
 *  1) 루트 버스와 그 아래 장치를 먼저 없앤다. 장치가 살아 있는 동안
 *     config 접근이 들어오면 이미 해제된 공유 페이지를 건드리게 된다.
 *  2) 진행 중인 AER 워크가 끝날 때까지 기다린다(cancel_work_sync).
 *     이 워크도 sh_info 를 읽고 쓰므로 페이지 해제보다 먼저 잠재워야 한다.
 *  3) IRQ 를 떼고 이벤트 채널을 닫는다. 그래야 새 알림이 오지 않는다.
 *  4) 마지막으로 공유 페이지의 grant 를 회수하고 페이지를 반납한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. cancel_work_sync() 가 잠들 수 있으므로
 * 인터럽트 문맥에서 부르면 안 된다.
 *
 * 호출 체인:
 *   pcifront_xenbus_remove() / pcifront_xenbus_probe(실패 경로)
 *     -> [free_pdev] -> pcifront_free_roots() -> cancel_work_sync()
 *                    -> xenbus_teardown_ring()
 */
static void free_pdev(struct pcifront_device *pdev)
{
	dev_dbg(&pdev->xdev->dev, "freeing pdev @ 0x%p\n", pdev);	/* [한국어] 해제 시작을 로그로 남긴다 */

	pcifront_free_roots(pdev);	/* [한국어] 루트 버스와 그 아래 모든 PCI 장치를 먼저 제거한다 */

	cancel_work_sync(&pdev->op_work);	/* [한국어] 큐에 남았거나 실행 중인 AER 워크가 끝날 때까지 기다린다. 이 워크는 sh_info 를 만지므로 페이지 해제 전에 반드시 잠재워야 한다 */

	if (pdev->irq >= 0)	/* [한국어] IRQ 를 실제로 바인딩한 적이 있을 때만 */
		unbind_from_irqhandler(pdev->irq, pdev);	/* [한국어] 핸들러를 떼고 IRQ 를 반납한다. 이후 이 채널로 들어오는 알림은 무시된다 */

	if (pdev->evtchn != INVALID_EVTCHN)	/* [한국어] 이벤트 채널을 실제로 할당한 적이 있을 때만 */
		xenbus_free_evtchn(pdev->xdev, pdev->evtchn);	/* [한국어] 하이퍼바이저에게 채널을 반납한다 */

	xenbus_teardown_ring((void **)&pdev->sh_info, 1, &pdev->gnt_ref);	/* [한국어] grant 를 회수하고 공유 페이지를 반납한다. 이 시점 이후 sh_info 접근은 use-after-free 다 */

	dev_set_drvdata(&pdev->xdev->dev, NULL);	/* [한국어] XenBus 장치에 매달아 둔 포인터를 끊는다. remove 가 두 번 불려도 안전하도록 */

	kfree(pdev);	/* [한국어] 마지막으로 구조체 자체를 해제한다 */
}

/*
 * [한국어]
 * pcifront_publish_info - 공유 페이지와 이벤트 채널을 XenStore 에 발행한다
 *
 * @pdev: alloc_pdev() 가 만든 프론트엔드 상태. 이미 공유 페이지와
 *   grant 참조(gnt_ref)는 확보돼 있고, 이벤트 채널만 아직 없다.
 * @return: 0 이면 발행 성공이며 상태가 Initialised 로 올라간다.
 *   음수면 실패이고, 호출자(pcifront_xenbus_probe)가 free_pdev() 로 되감는다.
 *
 * 프론트엔드와 백엔드가 통신하려면 두 가지를 합의해야 한다.
 *  1) 공유 메모리 - 이미 확보한 페이지의 grant 참조 번호.
 *     grant 는 "이 페이지를 저 도메인이 매핑해도 좋다" 는 허가표이며,
 *     하이퍼바이저가 관리한다. 번호만 알려 주면 백엔드가 그것으로 매핑한다.
 *  2) 알림 수단 - 이벤트 채널. 하이퍼콜 한 번으로 상대 도메인에
 *     가상 인터럽트를 올리는 경량 채널이다.
 * 이 두 값을 XenStore 의 우리 노드 아래에 쓰면 백엔드가 읽어 간다.
 *
 * XenStore 쓰기는 트랜잭션으로 묶는다. 백엔드가 세 키 중 일부만 읽는 중간
 * 상태를 보면 안 되기 때문이다. -EAGAIN 은 다른 쓰기와 충돌했다는 뜻이라
 * do_publish 로 되돌아가 통째로 다시 시도한다.
 *
 * 실행 컨텍스트: 프로세스 문맥(XenBus probe). XenStore 접근은 잠들 수 있다.
 *
 * 호출 체인:
 *   pcifront_xenbus_probe() -> [pcifront_publish_info]
 *     -> xenbus_alloc_evtchn() -> bind_evtchn_to_irqhandler()
 *     -> xenbus_transaction_start()/xenbus_printf()/xenbus_transaction_end()
 *     -> xenbus_switch_state(Initialised)
 */
static int pcifront_publish_info(struct pcifront_device *pdev)
{
	int err = 0;	/* [한국어] 반환값. 0 으로 시작한다 */
	struct xenbus_transaction trans;	/* [한국어] XenStore 트랜잭션 핸들. 세 키를 원자적으로 쓰기 위해 쓴다 */

	err = xenbus_alloc_evtchn(pdev->xdev, &pdev->evtchn);	/* [한국어] 하이퍼바이저에게 이벤트 채널을 하나 할당받아 pdev->evtchn 에 저장한다 */
	if (err)	/* [한국어] 채널을 못 얻으면 알림 수단이 없어 통신이 불가능하다 */
		goto out;	/* [한국어] out 으로 가서 err 를 그대로 반환한다 */

	/* [한국어] 채널에 인터럽트 핸들러를 붙인다. 백엔드는 config 응답과 AER 요청
	 * 모두를 같은 채널로 보내므로, 이 핸들러는 AER 쪽만 처리하고
	 * config 응답 대기는 do_pci_op() 의 폴링이 맡는다. */
	err = bind_evtchn_to_irqhandler(pdev->evtchn, pcifront_handler_aer,
		0, "pcifront", pdev);	/* [한국어] 플래그 0, 이름 "pcifront", 핸들러 인자로 pdev 를 넘긴다 */

	if (err < 0)	/* [한국어] bind_evtchn_to_irqhandler 는 성공 시 IRQ 번호(음이 아닌 값)를 준다 */
		return err;	/* [한국어] 실패 시 out 을 거치지 않고 바로 반환한다 - 이벤트 채널 해제는 호출자의 free_pdev() 가 맡는다 */

	pdev->irq = err;	/* [한국어] 이후 do_pci_op() 의 폴링과 free_pdev() 의 해제가 이 IRQ 번호를 쓴다 */

/* [한국어] 트랜잭션 충돌(-EAGAIN) 시 여기로 되돌아와 처음부터 다시 쓴다. */
do_publish:
	err = xenbus_transaction_start(&trans);	/* [한국어] XenStore 트랜잭션을 연다. 이후 쓰기는 커밋 전까지 다른 도메인에 보이지 않는다 */
	if (err) {	/* [한국어] 트랜잭션 자체를 열지 못한 경우 */
		xenbus_dev_fatal(pdev->xdev, err,
				 "Error writing configuration for backend "
				 "(start transaction)");	/* [한국어] XenStore 의 error 노드에 기록하고 상태를 Closing 으로 만든다 */
		goto out;	/* [한국어] 되돌릴 트랜잭션이 없으므로 바로 반환한다 */
	}

	err = xenbus_printf(trans, pdev->xdev->nodename,
			    "pci-op-ref", "%u", pdev->gnt_ref);	/* [한국어] 공유 페이지의 grant 참조 번호. 백엔드는 이 번호로 페이지를 매핑한다 */
	if (!err)	/* [한국어] 앞의 쓰기가 성공했을 때만 다음 키를 쓴다 */
		err = xenbus_printf(trans, pdev->xdev->nodename,
				    "event-channel", "%u", pdev->evtchn);	/* [한국어] 이벤트 채널 포트 번호. 백엔드는 이 포트로 알림을 보낸다 */
	if (!err)	/* [한국어] 여기서도 앞선 실패가 없을 때만 진행한다 */
		err = xenbus_printf(trans, pdev->xdev->nodename,
				    "magic", XEN_PCI_MAGIC);	/* [한국어] 프로토콜 버전 표식. 백엔드는 이 문자열이 맞아야 연결을 진행한다. 값의 정의는 이 트리에 없는 xen/interface/io/pciif.h 에 있다 */

	if (err) {	/* [한국어] 세 번의 xenbus_printf 중 하나라도 실패한 경우 */
		xenbus_transaction_end(trans, 1);	/* [한국어] 두 번째 인자 1 = abort. 지금까지의 쓰기를 모두 버린다 */
		xenbus_dev_fatal(pdev->xdev, err,
				 "Error writing configuration for backend");	/* [한국어] 실패 사유를 XenStore 에 남긴다 */
		goto out;	/* [한국어] 이벤트 채널/IRQ 정리는 호출자의 free_pdev() 가 맡는다 */
	} else {	/* [한국어] 세 키를 모두 성공적으로 썼으므로 커밋을 시도한다 */
		err = xenbus_transaction_end(trans, 0);	/* [한국어] 두 번째 인자 0 = commit */
		if (err == -EAGAIN)	/* [한국어] -EAGAIN 은 다른 트랜잭션과 충돌했다는 뜻이다 */
			goto do_publish;	/* [한국어] do_publish 로 돌아가 처음부터 다시 쓴다. 이때 이벤트 채널은 이미 있으므로 다시 만들지 않는다 */
		else if (err) {	/* [한국어] -EAGAIN 이 아닌 실제 오류 */
			xenbus_dev_fatal(pdev->xdev, err,
					 "Error completing transaction "
					 "for backend");	/* [한국어] 커밋 실패를 기록한다 */
			goto out;	/* [한국어] 반환한다 */
		}
	}

	/* [한국어] 여기부터가 상태 기계의 첫 전이다. Initialised 는 "우리 쪽 준비가
	 * 끝났고 백엔드가 읽어 갈 정보가 XenStore 에 있다" 는 뜻이며,
	 * pcifront_try_connect() 가 이 상태인지 확인해 중복 연결을 막는다. */
	xenbus_switch_state(pdev->xdev, XenbusStateInitialised);	/* [한국어] XenStore 의 우리 노드 state 를 Initialised(3)로 올린다 */

	dev_dbg(&pdev->xdev->dev, "publishing successful!\n");	/* [한국어] 발행 성공을 디버그 로그로 남긴다 */

/* [한국어] 실패와 성공이 함께 지나가는 반환 지점. err 가 0 이면 성공이다. */
out:
	return err;	/* [한국어] 0 이면 probe 가 성공으로 끝난다 */
}

/*
 * [한국어]
 * pcifront_connect - 백엔드가 내보낸 PCI 루트 목록을 읽어 버스를 만든다
 *
 * @pdev: 프론트엔드 상태. 이 시점에 전역 등록(pcifront_connect_and_init_dma)은
 *   이미 끝나 있다.
 * @return: 없음. 실패는 xenbus_dev_fatal() 로 기록하고 상태를 올리지 않는다.
 *   상태가 Connected 로 올라가지 못하면 게스트는 장치를 보지 못한다.
 *
 * XenStore 규약: 백엔드 노드(otherend) 아래에
 *   root_num  - 내보낸 PCI 루트 버스의 개수
 *   root-<i>  - i 번째 루트의 "domain:bus" 문자열 (16진수)
 * root_num 키가 아예 없으면(-ENOENT) 구형 백엔드로 보고 0000:00 하나만
 * 있다고 가정한다.
 *
 * 여기서 만들어진 루트 버스의 pci_ops 는 pcifront_bus_ops 다. 따라서 이후
 * 이 버스 아래 모든 config 접근은 실제 하드웨어가 아니라 do_pci_op() 를 거쳐
 * 백엔드에게 위임된다 - 이것이 이 드라이버의 핵심이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. pcifront_scan_root() 안에서
 * pci_lock_rescan_remove() 뮤텍스를 잡고 GFP_KERNEL 할당을 한다.
 *
 * 호출 체인:
 *   pcifront_try_connect() / pcifront_attach_devices()
 *     -> [pcifront_connect] -> pcifront_rescan_root() -> pcifront_scan_root()
 *     -> xenbus_switch_state(Connected)
 */
static void pcifront_connect(struct pcifront_device *pdev)
{
	int err;	/* [한국어] XenStore 읽기 결과를 담는다 */
	int i, num_roots, len;	/* [한국어] 루프 인덱스, 루트 개수, snprintf 결과 길이 */
	char str[64];	/* [한국어] "root-<i>" 키 이름을 담을 버퍼. 64바이트면 충분하다 */
	unsigned int domain, bus;	/* [한국어] root-<i> 에서 파싱한 PCI 도메인과 버스 번호 */

	err = xenbus_scanf(XBT_NIL, pdev->xdev->otherend,
			   "root_num", "%d", &num_roots);	/* [한국어] 백엔드 노드에서 루트 개수를 읽는다. 성공 시 반환값은 변환 항목 수인 1 */
	if (err == -ENOENT) {	/* [한국어] 키가 아예 없는 경우 - 루트를 명시하지 않는 구형 백엔드 */
		xenbus_dev_error(pdev->xdev, err,
				 "No PCI Roots found, trying 0000:00");	/* [한국어] 치명적이지는 않으므로 error 노드에만 남긴다 */
		err = pcifront_rescan_root(pdev, 0, 0);	/* [한국어] 도메인 0, 버스 0 하나만 있다고 가정하고 스캔한다 */
		if (err) {	/* [한국어] 그 스캔마저 실패하면 더 진행할 수 없다 */
			xenbus_dev_fatal(pdev->xdev, err,
					 "Error scanning PCI root 0000:00");
			return;	/* [한국어] 상태를 Connected 로 올리지 않고 반환한다 */
		}
		num_roots = 0;	/* [한국어] 아래 루프를 돌지 않도록 0 으로 만든다 */
	} else if (err != 1) {	/* [한국어] 키는 있는데 정수 하나를 읽지 못한 경우 */
		xenbus_dev_fatal(pdev->xdev, err >= 0 ? -EINVAL : err,
				 "Error reading number of PCI roots");	/* [한국어] 음수면 그대로, 아니면 -EINVAL 로 바꿔 치명적 오류로 기록한다 */
		return;	/* [한국어] 버스를 만들지 않고 반환한다 */
	}

	for (i = 0; i < num_roots; i++) {	/* [한국어] 루트를 하나씩 순회한다 */
		len = snprintf(str, sizeof(str), "root-%d", i);	/* [한국어] i 번째 루트의 키 이름을 만든다 */
		if (unlikely(len >= (sizeof(str) - 1)))	/* [한국어] 버퍼가 잘렸는지 확인한다 - 잘리면 엉뚱한 키를 읽게 된다 */
			return;	/* [한국어] 상태를 올리지 않고 반환한다 */

		err = xenbus_scanf(XBT_NIL, pdev->xdev->otherend, str,
				   "%x:%x", &domain, &bus);	/* [한국어] "domain:bus" 를 16진수 두 조각으로 파싱한다 */
		if (err != 2) {	/* [한국어] 두 값을 모두 얻지 못하면 형식 오류다 */
			xenbus_dev_fatal(pdev->xdev, err >= 0 ? -EINVAL : err,
					 "Error reading PCI root %d", i);	/* [한국어] 음수면 그대로, 아니면 -EINVAL 로 기록한다 */
			return;	/* [한국어] 반환한다 */
		}

		err = pcifront_rescan_root(pdev, domain, bus);	/* [한국어] 이미 있는 버스면 재스캔, 없으면 루트 버스를 새로 만든다 */
		if (err) {	/* [한국어] 스캔 실패 */
			xenbus_dev_fatal(pdev->xdev, err,
					 "Error scanning PCI root %04x:%02x",
					 domain, bus);	/* [한국어] 어느 루트에서 실패했는지 도메인:버스와 함께 기록한다 */
			return;	/* [한국어] 반환한다 */
		}
	}

	/* [한국어] 모든 루트를 성공적으로 열거했다. 상태를 Connected 로 올리면
	 * 백엔드는 이제 이 게스트가 장치를 쓰고 있다고 간주한다. */
	xenbus_switch_state(pdev->xdev, XenbusStateConnected);	/* [한국어] XenStore 의 우리 노드 state 를 Connected(4)로 올린다 */
}

/*
 * [한국어]
 * pcifront_try_connect - 프론트엔드가 아직 연결 전이면 연결을 시도한다
 *
 * @pdev: 이 XenBus 채널의 프론트엔드 상태.
 * @return: 없음. 실패는 xenbus_dev_fatal() 로 XenStore 에 기록된다.
 *
 * 백엔드가 Connected 로 올라올 때마다 불릴 수 있으므로, 우리 쪽 상태가
 * 정확히 Initialised 일 때만 진행해 중복 연결을 막는다. Initialised 는
 * pcifront_publish_info() 가 grant 참조와 이벤트 채널을 XenStore 에 쓴 뒤
 * 설정하는 상태다. 즉 "발행은 끝났고 아직 버스는 안 만들었다" 는 뜻이다.
 *
 * 실행 컨텍스트: 프로세스 문맥(XenBus 워커). 아래에서 스핀락과
 * pci_lock_rescan_remove() 뮤텍스를 차례로 잡는다.
 *
 * 호출 체인:
 *   pcifront_backend_changed(Connected) -> [pcifront_try_connect]
 *     -> pcifront_connect_and_init_dma() -> pcifront_connect()
 */
static void pcifront_try_connect(struct pcifront_device *pdev)
{
	int err;	/* [한국어] 하위 호출의 반환값을 담는다 */

	/* Only connect once */
	/* [한국어] XenStore 에서 우리 자신의 상태를 다시 읽는다 - 메모리 캐시가 아니라
	 * XenStore 가 유일한 진실이기 때문이다. */
	if (xenbus_read_driver_state(pdev->xdev, pdev->xdev->nodename) !=
	    XenbusStateInitialised)	/* [한국어] Initialised 가 아니면 이미 연결했거나 아직 발행 전이다 */
		return;	/* [한국어] 어느 쪽이든 여기서 할 일이 없다 */

	err = pcifront_connect_and_init_dma(pdev);	/* [한국어] 전역 프론트엔드 포인터를 이 pdev 로 등록한다. 이미 등록돼 있으면 -EEXIST */
	if (err && err != -EEXIST) {	/* [한국어] -EEXIST 는 "다른 프론트엔드가 이미 있다" 는 뜻이라 치명적이지 않다. 그 외의 오류만 실패로 본다 */
		xenbus_dev_fatal(pdev->xdev, err,
				 "Error setting up PCI Frontend");
		return;	/* [한국어] 치명적 오류를 XenStore 에 기록했으므로 버스 생성으로 넘어가지 않는다 */
	}

	pcifront_connect(pdev);	/* [한국어] 백엔드가 내보낸 루트 버스 목록을 읽어 실제 PCI 버스를 만들고 상태를 Connected 로 올린다 */
}

/*
 * [한국어]
 * pcifront_try_disconnect - 백엔드가 사라질 때 게스트 쪽 장치를 걷어낸다
 *
 * @pdev: 해제할 프론트엔드 상태.
 * @return: xenbus_switch_state() 의 결과. 이미 Closing 이상이면 0.
 *   호출자(pcifront_backend_changed)는 반환값을 무시한다.
 *
 * 두 가지를 막아야 한다. 첫째, 이미 종료 절차에 들어간 상태에서 다시 들어와
 * 버스를 두 번 해제하는 것. 둘째, 아직 Connected 가 아니었는데 버스를
 * 해제하려 드는 것. 그래서 XenStore 의 현재 상태를 먼저 읽어 분기한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. pcifront_free_roots() 안에서
 * pci_lock_rescan_remove() 뮤텍스를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   pcifront_backend_changed(Closing/Closed) -> [pcifront_try_disconnect]
 *     -> pcifront_free_roots() -> pcifront_disconnect()
 */
static int pcifront_try_disconnect(struct pcifront_device *pdev)
{
	int err = 0;	/* [한국어] 기본 반환값 0 - 이미 종료 중이면 아무것도 하지 않고 이 값을 돌려준다 */
	enum xenbus_state prev_state;	/* [한국어] 진입 시점의 프론트엔드 상태를 담을 변수 */


	prev_state = xenbus_read_driver_state(pdev->xdev, pdev->xdev->nodename);	/* [한국어] XenStore 에서 우리 자신의 상태를 읽는다 */

	if (prev_state >= XenbusStateClosing)	/* [한국어] Closing 이상이면 이미 이 경로를 지났다는 뜻이다 */
		goto out;	/* [한국어] 중복 해제를 피해 그대로 빠져나간다 */

	if (prev_state == XenbusStateConnected) {	/* [한국어] Connected 였을 때만 실제로 만들어 둔 버스가 존재한다 */
		pcifront_free_roots(pdev);	/* [한국어] 루트 버스마다 장치를 정지-제거하고 버스 자체를 없앤다 */
		pcifront_disconnect(pdev);	/* [한국어] 전역 프론트엔드 포인터가 우리를 가리키면 NULL 로 되돌린다 */
	}

	err = xenbus_switch_state(pdev->xdev, XenbusStateClosed);	/* [한국어] 상태를 Closed 로 올려 백엔드에게 정리가 끝났음을 알린다 */

out:

	return err;
}

/*
 * [한국어]
 * pcifront_attach_devices - 재구성이 끝난 뒤 새로 붙은 장치를 열거한다
 *
 * @pdev: 프론트엔드 상태.
 * @return: 없음.
 *
 * 핫플러그의 뒷단이다. pcifront_detach_devices() 가 우리 상태를
 * Reconfiguring 으로 올려 두었고, 백엔드가 그에 응답해 Reconfigured 로
 * 올라오면 이 함수가 불린다. 우리 상태가 Reconfiguring 일 때만 진행해
 * 엉뚱한 시점의 재스캔을 막는다.
 *
 * 실행 컨텍스트: 프로세스 문맥(XenBus 워커).
 *
 * 호출 체인:
 *   pcifront_backend_changed(Reconfigured) -> [pcifront_attach_devices]
 *     -> pcifront_connect()
 */
static void pcifront_attach_devices(struct pcifront_device *pdev)
{
	if (xenbus_read_driver_state(pdev->xdev, pdev->xdev->nodename) ==
	    XenbusStateReconfiguring)	/* [한국어] 우리가 Reconfiguring 을 선언한 상태여야 새 장치를 받을 차례다 */
		pcifront_connect(pdev);	/* [한국어] 루트 목록을 다시 읽어 새로 나타난 장치를 스캔하고 상태를 Connected 로 올린다 */
}

/*
 * [한국어]
 * pcifront_detach_devices - 백엔드가 빼려는 장치를 게스트에서 먼저 제거한다
 *
 * @pdev: 프론트엔드 상태.
 * @return: 0 이면 정상. 음수면 XenStore 읽기 실패나 문자열 버퍼 부족.
 *   호출자는 반환값을 무시하지만, 실패 시 상태 전이가 일어나지 않아
 *   백엔드가 다음 단계로 진행하지 못한다.
 *
 * 백엔드가 Reconfiguring 으로 올라오면 "이 장치들을 빼겠다" 는 신호다.
 * 게스트가 아직 그 장치를 쓰고 있는데 백엔드가 먼저 회수하면 안 되므로,
 * 여기서 드라이버를 언바인드하고 pci_dev 를 제거한 뒤에야
 * 우리 상태를 Reconfiguring 으로 올려 진행을 허락한다.
 *
 * XenStore 규약: otherend 아래에 num_devs, 그리고 각 i 마다
 *   state-<i>  - 그 장치의 백엔드 상태 (Closing 이면 제거 대상)
 *   vdev-<i>   - 게스트가 보는 BDF 문자열 "domain:bus:slot.func"
 *
 * 특수 경우: 우리가 Connected 를 건너뛰고 Initialised 에 머물러 있었다면
 * (백엔드의 Connected 알림을 놓친 경우) 제거할 장치가 없으므로 DMA 컨텍스트만
 * 세우고 곧장 상태 전이로 넘어간다.
 *
 * 실행 컨텍스트: 프로세스 문맥. pci_lock_rescan_remove() 뮤텍스를 잡는다.
 *
 * 호출 체인:
 *   pcifront_backend_changed(Reconfiguring) -> [pcifront_detach_devices]
 *     -> pci_stop_and_remove_bus_device()
 */
static int pcifront_detach_devices(struct pcifront_device *pdev)
{
	int err = 0;	/* [한국어] 반환값. 정상 경로에서는 마지막 상태 전이 결과로 덮어쓴다 */
	int i, num_devs;	/* [한국어] 반복 인덱스와 백엔드가 알린 장치 개수 */
	enum xenbus_state state;	/* [한국어] 프론트엔드 자신의 XenBus 상태 */
	unsigned int domain, bus, slot, func;	/* [한국어] vdev-N 문자열에서 뽑아낼 BDF 구성 요소 */
	struct pci_dev *pci_dev;	/* [한국어] 제거 대상 pci_dev 를 담을 포인터 */
	char str[64];	/* [한국어] XenStore 키 이름과 값을 담을 임시 버퍼 */

	state = xenbus_read_driver_state(pdev->xdev, pdev->xdev->nodename);	/* [한국어] 우리 자신의 현재 상태를 XenStore 에서 읽는다 */
	if (state == XenbusStateInitialised) {	/* [한국어] Connected 를 거치지 않고 Initialised 에 머물러 있는 경우 */
		dev_dbg(&pdev->xdev->dev, "Handle skipped connect.\n");	/* [한국어] 연결 단계를 건너뛰었음을 로그로 남긴다 */
		/* We missed Connected and need to initialize. */
		err = pcifront_connect_and_init_dma(pdev);	/* [한국어] 전역 프론트엔드 포인터만 세워 둔다. 제거할 장치는 아직 없다 */
		if (err && err != -EEXIST) {	/* [한국어] -EEXIST 는 다른 프론트엔드가 먼저 등록된 정상 상황이므로 넘어간다 */
			xenbus_dev_fatal(pdev->xdev, err,
					 "Error setting up PCI Frontend");
			goto out;	/* [한국어] 치명적 오류를 기록했으므로 상태 전이 없이 반환한다 */
		}

		goto out_switch_state;	/* [한국어] 제거 루프를 건너뛰고 상태 전이만 수행한다 */
	} else if (state != XenbusStateConnected) {	/* [한국어] Connected 도 Initialised 도 아니면 재구성을 처리할 단계가 아니다 */
		goto out;	/* [한국어] 아무것도 하지 않고 반환한다 */
	}

	err = xenbus_scanf(XBT_NIL, pdev->xdev->otherend, "num_devs", "%d",
			   &num_devs);	/* [한국어] 백엔드가 내보낸 장치 개수를 읽는다. xenbus_scanf 는 변환된 항목 수를 돌려준다 */
	if (err != 1) {	/* [한국어] 1 이 아니면 키가 없거나 형식이 어긋난 것이다 */
		if (err >= 0)	/* [한국어] scanf 가 0 이나 양수를 준 형식 오류라면 */
			err = -EINVAL;	/* [한국어] errno 형태로 바꿔 준다 */
		xenbus_dev_fatal(pdev->xdev, err,
				 "Error reading number of PCI devices");
		goto out;	/* [한국어] 개수를 모르면 순회할 수 없으므로 종료한다 */
	}

	/* Find devices being detached and remove them. */
	for (i = 0; i < num_devs; i++) {	/* [한국어] 백엔드가 알린 장치를 하나씩 검사한다 */
		int l, state;	/* [한국어] l 은 snprintf 결과 길이, state 는 이 장치의 백엔드 상태. 바깥 state 를 가린다 */

		l = snprintf(str, sizeof(str), "state-%d", i);	/* [한국어] i 번째 장치의 상태 키 이름을 만든다 */
		if (unlikely(l >= (sizeof(str) - 1))) {	/* [한국어] 버퍼가 잘렸다면 키 이름이 틀려 엉뚱한 값을 읽게 된다 */
			err = -ENOMEM;	/* [한국어] 잘림을 -ENOMEM 으로 보고한다 */
			goto out;	/* [한국어] 더 진행하지 않는다 */
		}
		state = xenbus_read_unsigned(pdev->xdev->otherend, str,
					     XenbusStateUnknown);	/* [한국어] 키가 없으면 XenbusStateUnknown 을 기본값으로 받는다 */

		if (state != XenbusStateClosing)	/* [한국어] Closing 이 아니면 이번에 빼려는 장치가 아니다 */
			continue;	/* [한국어] 다음 인덱스로 넘어간다 */

		/* Remove device. */
		l = snprintf(str, sizeof(str), "vdev-%d", i);	/* [한국어] i 번째 장치의 BDF 를 담은 키 이름을 만든다 */
		if (unlikely(l >= (sizeof(str) - 1))) {	/* [한국어] 여기서도 문자열 잘림을 확인한다 */
			err = -ENOMEM;	/* [한국어] 잘렸다면 -ENOMEM */
			goto out;	/* [한국어] 종료한다 */
		}
		err = xenbus_scanf(XBT_NIL, pdev->xdev->otherend, str,
				   "%x:%x:%x.%x", &domain, &bus, &slot, &func);	/* [한국어] "domain:bus:slot.func" 를 16진수 네 조각으로 파싱한다 */
		if (err != 4) {	/* [한국어] 네 개를 모두 얻지 못하면 형식 오류다 */
			if (err >= 0)	/* [한국어] scanf 가 음수 errno 를 주지 않았다면 */
				err = -EINVAL;	/* [한국어] -EINVAL 로 통일한다 */
			xenbus_dev_fatal(pdev->xdev, err,
					 "Error reading PCI device %d", i);
			goto out;	/* [한국어] BDF 를 모르면 어떤 장치를 뺄지 알 수 없으므로 종료한다 */
		}

		pci_dev = pci_get_domain_bus_and_slot(domain, bus,
				PCI_DEVFN(slot, func));	/* [한국어] slot 과 func 를 devfn 한 바이트로 합쳐 pci_dev 를 찾는다. 성공하면 참조 카운트가 올라간다 */
		if (!pci_dev) {	/* [한국어] 이미 없어졌거나 애초에 열거되지 않은 장치 */
			dev_dbg(&pdev->xdev->dev,
				"Cannot get PCI device %04x:%02x:%02x.%d\n",
				domain, bus, slot, func);	/* [한국어] 찾지 못한 사실만 남긴다. 참조를 얻지 못했으므로 put 도 필요 없다 */
			continue;	/* [한국어] 다음 장치로 넘어간다 */
		}
		pci_lock_rescan_remove();	/* [한국어] 버스 재스캔/제거 뮤텍스를 잡는다. PCI 코어의 다른 재스캔과 경쟁하면 안 되기 때문이다 */
		pci_stop_and_remove_bus_device(pci_dev);	/* [한국어] 바인딩된 드라이버의 remove 를 부르고 pci_dev 를 버스에서 떼어낸다 */
		pci_dev_put(pci_dev);	/* [한국어] pci_get_domain_bus_and_slot() 이 올려 둔 참조를 내려놓는다 */
		pci_unlock_rescan_remove();	/* [한국어] 뮤텍스 해제. 여기까지가 한 장치의 제거다 */

		dev_dbg(&pdev->xdev->dev,
			"PCI device %04x:%02x:%02x.%d removed.\n",
			domain, bus, slot, func);	/* [한국어] 제거 완료를 로그로 남긴다 */
	}

 /* [한국어] 제거 루프를 마쳤거나 Initialised 단축 경로에서 건너뛰어 도달한다. */
 out_switch_state:
	err = xenbus_switch_state(pdev->xdev, XenbusStateReconfiguring);	/* [한국어] 우리 상태를 Reconfiguring 으로 올린다. 백엔드는 이 신호를 보고 회수를 진행하고, 끝나면 Reconfigured 로 응답한다 */

/* [한국어] 오류로 빠져나오는 공통 지점. err 에는 실패 원인이 담겨 있다. */
out:
	return err;	/* [한국어] 호출자는 이 값을 쓰지 않지만, 실패 시 상태 전이가 없었다는 사실이 그대로 반영된다 */
}

/*
 * [한국어]
 * pcifront_backend_changed - 백엔드(dom0 의 pciback)의 XenBus 상태 변화에 반응한다
 *
 * @xdev: 상태가 바뀐 XenBus 프론트엔드 장치. XenBus 코어가 넘겨준다.
 * @be_state: 백엔드가 XenStore 에 새로 써 넣은 상태값.
 * @return: 없음. 실패는 각 하위 함수가 xenbus_dev_fatal() 로 보고한다.
 *
 * XenBus 는 프론트엔드와 백엔드가 XenStore(하이퍼바이저가 중재하는 공유
 * 키-값 저장소) 안의 상태 문자열을 서로 감시하며 진행하는 상태 기계다.
 * 이 콜백은 xenbus_driver.otherend_changed 에 등록되어(아래 xenpci_driver),
 * 백엔드 상태가 바뀔 때마다 XenBus 코어의 워커 문맥에서 불린다.
 *
 * 상태 전이의 뜻:
 *   Unknown/Initialising/InitWait/Initialised - 백엔드가 아직 준비 중.
 *     이쪽에서 할 일이 없으므로 그냥 빠져나온다.
 *   Connected      - 백엔드가 PCI 장치를 넘겨줄 준비를 마쳤다.
 *                    루트 버스를 만들고 장치를 열거한다.
 *   Closing/Closed - 백엔드가 사라진다. 게스트에서 장치를 걷어낸다.
 *   Reconfiguring  - 백엔드가 장치를 빼려 한다(핫 리무브).
 *   Reconfigured   - 백엔드가 장치를 새로 붙였다(핫 애드).
 *
 * 실행 컨텍스트: 프로세스 문맥(XenBus 워커). 잠들 수 있고, 아래에서
 * pci_lock_rescan_remove() 같은 뮤텍스를 잡는다. 인터럽트 문맥이 아니다.
 *
 * 호출 체인:
 *   XenBus 코어(otherend 감시) -> [pcifront_backend_changed]
 *     -> pcifront_try_connect() / pcifront_try_disconnect()
 *     -> pcifront_detach_devices() / pcifront_attach_devices()
 */
static void pcifront_backend_changed(struct xenbus_device *xdev,
						  enum xenbus_state be_state)
{
	struct pcifront_device *pdev = dev_get_drvdata(&xdev->dev);	/* [한국어] XenBus 장치에 붙여 둔 프론트엔드 상태를 꺼낸다. alloc_pdev() 가 dev_set_drvdata() 로 심어 둔 값이다 */

	switch (be_state) {	/* [한국어] 백엔드가 알린 새 상태에 따라 분기한다 */
	/* [한국어] 아래 네 상태는 백엔드가 아직 채널을 열지 않았거나 준비 중인 단계다. */
	case XenbusStateUnknown:
	case XenbusStateInitialising:
	case XenbusStateInitWait:
	case XenbusStateInitialised:
		break;	/* [한국어] 할 일이 없다 - 프론트엔드는 이미 pcifront_publish_info() 에서 Initialised 로 넘어가 기다리는 중이다 */

	/* [한국어] 백엔드가 Connected 로 올라왔다 = 이제 config 요청을 처리해 줄 수 있다. */
	case XenbusStateConnected:
		pcifront_try_connect(pdev);	/* [한국어] 한 번만 연결한다. 안에서 프론트엔드 자신의 상태가 Initialised 인지 확인한다 */
		break;	/* [한국어] 연결 처리 끝 */

	/* [한국어] 백엔드가 Closed 로 갔다. Closing 을 놓쳤을 수 있으므로 아래로 흘려보낸다. */
	case XenbusStateClosed:
		if (xdev->state == XenbusStateClosed)	/* [한국어] 이미 우리도 Closed 라면 두 번 정리할 필요가 없다 */
			break;	/* [한국어] 중복 정리를 피하고 빠져나간다 */
		/* [한국어] 아직 Closed 가 아니면 Closing 처리와 같은 정리를 그대로 수행한다. */
		fallthrough;	/* Missed the backend's CLOSING state */
	case XenbusStateClosing:
		dev_warn(&xdev->dev, "backend going away!\n");	/* [한국어] 백엔드가 사라진다는 사실을 로그로 남긴다 */
		pcifront_try_disconnect(pdev);	/* [한국어] 루트 버스와 그 아래 장치를 모두 걷어내고 상태를 Closed 로 내린다 */
		break;	/* [한국어] 종료 처리 끝 */

	/* [한국어] 백엔드가 장치를 빼려 한다 - 먼저 게스트 쪽에서 떼어내야 한다. */
	case XenbusStateReconfiguring:
		pcifront_detach_devices(pdev);	/* [한국어] XenStore 의 state-N 을 읽어 Closing 인 장치를 제거하고, 우리 상태를 Reconfiguring 으로 올린다 */
		break;	/* [한국어] 핫 리무브 처리 끝 */

	/* [한국어] 백엔드가 재구성을 마쳤다 - 새로 붙은 장치를 열거할 차례다. */
	case XenbusStateReconfigured:
		pcifront_attach_devices(pdev);	/* [한국어] 우리 상태가 Reconfiguring 일 때만 다시 스캔한다 */
		break;	/* [한국어] 핫 애드 처리 끝 */
	}
}

/*
 * [한국어]
 * pcifront_xenbus_probe - XenBus 가 "pci" 백엔드를 찾았을 때 프론트엔드를 만든다
 *
 * @xdev: XenBus 코어가 새로 만든 프론트엔드 장치 객체.
 * @id: xenpci_ids[] 중 매치된 항목. 이 드라이버는 "pci" 하나뿐이라 쓰지 않는다.
 * @return: 0 이면 성공. 음수 errno 면 XenBus 코어가 probe 실패로 처리한다.
 *
 * 이 시점에는 아직 PCI 버스가 없다. 여기서 하는 일은 공유 페이지와
 * 이벤트 채널을 준비하고 그 참조를 XenStore 에 실어 백엔드에게 알리는 것뿐이다.
 * 실제 버스 생성과 장치 열거는 백엔드가 Connected 로 올라온 뒤
 * pcifront_backend_changed() 를 통해 일어난다.
 *
 * 실행 컨텍스트: 프로세스 문맥(XenBus probe). GFP_KERNEL 할당을 한다.
 *
 * 호출 체인:
 *   XenBus 코어 -> [pcifront_xenbus_probe] -> alloc_pdev()
 *                                          -> pcifront_publish_info()
 */
static int pcifront_xenbus_probe(struct xenbus_device *xdev,
				 const struct xenbus_device_id *id)
{
	int err = 0;	/* [한국어] 반환값. 0 으로 시작해 실패 시에만 덮어쓴다 */
	struct pcifront_device *pdev = alloc_pdev(xdev);	/* [한국어] 프론트엔드 상태 구조체와 백엔드와 공유할 페이지를 한 번에 준비한다 */

	if (pdev == NULL) {	/* [한국어] 할당 실패 - 공유 페이지 또는 구조체를 얻지 못했다 */
		err = -ENOMEM;	/* [한국어] 메모리 부족을 호출자에게 알린다 */
		xenbus_dev_fatal(xdev, err,
				 "Error allocating pcifront_device struct");
		goto out;	/* [한국어] 할당된 것이 없으므로 정리 없이 바로 반환한다 */
	}

	err = pcifront_publish_info(pdev);	/* [한국어] grant 참조와 이벤트 채널 번호를 XenStore 에 쓰고 상태를 Initialised 로 올린다 */
	if (err)	/* [한국어] 발행에 실패하면 이미 만든 자원을 되돌려야 한다 */
		free_pdev(pdev);	/* [한국어] 이벤트 채널, IRQ, 공유 페이지를 모두 해제하고 구조체를 free 한다 */

out:
	return err;	/* [한국어] 0 이면 XenBus 코어가 이 장치를 이 드라이버에 바인딩한 채로 둔다 */
}

/*
 * [한국어]
 * pcifront_xenbus_remove - XenBus 장치가 사라질 때 프론트엔드를 해체한다
 *
 * @xdev: 제거되는 XenBus 프론트엔드 장치.
 * @return: 없음. xenbus_driver.remove 는 void 를 요구한다.
 *
 * 모듈 언로드나 백엔드 소멸로 XenBus 코어가 장치를 떼어낼 때 불린다.
 * free_pdev() 안에서 루트 버스에 매달린 PCI 장치들을 먼저 제거하므로,
 * 이 경로를 타면 게스트가 보던 패스스루 장치가 모두 사라진다.
 *
 * 실행 컨텍스트: 프로세스 문맥. cancel_work_sync() 로 AER 작업이 끝날 때까지
 * 기다리므로 잠들 수 있다.
 *
 * 호출 체인:
 *   XenBus 코어 -> [pcifront_xenbus_remove] -> free_pdev()
 */
static void pcifront_xenbus_remove(struct xenbus_device *xdev)
{
	struct pcifront_device *pdev = dev_get_drvdata(&xdev->dev);	/* [한국어] probe 에서 심어 둔 프론트엔드 상태를 꺼낸다 */

	if (pdev)	/* [한국어] probe 가 실패했다면 NULL 일 수 있으므로 확인한다 */
		free_pdev(pdev);	/* [한국어] 루트 버스 제거, 작업 취소, 이벤트 채널/공유 페이지 해제까지 한 번에 처리한다 */
}

/*
 * [한국어] xenpci_ids - 이 프론트엔드가 붙을 XenBus 백엔드 종류 목록.
 *
 * XenBus 는 XenStore 경로의 device/<type>/<id> 에서 <type> 문자열로
 * 프론트엔드 드라이버를 고른다. 빈 문자열이 배열의 끝 표시다.
 */
static const struct xenbus_device_id xenpci_ids[] = {
	{"pci"},	/* [한국어] "pci" 타입 - dom0 의 pciback 이 내보내는 PCI 패스스루 채널을 뜻한다 */
	{""},	/* [한국어] 빈 문자열 = 목록 끝 센티널. XenBus 코어가 여기서 순회를 멈춘다 */
};

/*
 * [한국어] xenpci_driver - XenBus 코어에 등록할 프론트엔드 드라이버 정의.
 *
 * 이 구조체가 이 파일의 "진입점 묶음"이다. 아래 세 콜백이
 * 장치 생성 - 상태 추적 - 장치 소멸의 전 과정을 담당한다.
 */
static struct xenbus_driver xenpci_driver = {
	.name			= "pcifront",	/* [한국어] XenStore 와 sysfs 에 보이는 드라이버 이름 */
	.ids			= xenpci_ids,	/* [한국어] 위에서 정의한 매치 테이블. "pci" 백엔드에만 붙는다 */
	.probe			= pcifront_xenbus_probe,	/* [한국어] 장치 발견 시 호출 - 공유 페이지/이벤트 채널을 만들고 XenStore 에 발행한다 */
	.remove			= pcifront_xenbus_remove,	/* [한국어] 장치 소멸 시 호출 - 루트 버스와 자원을 모두 해제한다 */
	.otherend_changed	= pcifront_backend_changed,	/* [한국어] 백엔드(otherend) 상태가 바뀔 때마다 호출 - 연결/해제/핫플러그를 처리한다 */
};

/*
 * [한국어]
 * pcifront_init - 모듈 적재 시 프론트엔드를 XenBus 에 등록한다
 *
 * @return: 0 이면 등록 성공. -ENODEV 는 "여기는 이 드라이버가 동작할 환경이
 *   아니다" 라는 뜻이고, 그 밖의 음수는 xenbus_register_frontend() 의 실패다.
 *   음수를 반환하면 모듈이 적재되지 않는다.
 *
 * 두 개의 관문이 있다.
 *  1) xen_pv_domain() 이 참이고 xen_initial_domain() 이 거짓 - 즉 PV 게스트여야
 *     한다. dom0 는 실제 하드웨어를 직접 다루므로 이 프론트엔드가 필요 없다.
 *  2) xen_has_pv_devices() - 이 게스트에 PV 장치 채널이 제공되어야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥(모듈 init). __init 이므로 부팅/적재 후 해제된다.
 *
 * 호출 체인:
 *   module_init -> [pcifront_init] -> pci_frontend_registrar()
 *                                  -> xenbus_register_frontend()
 */
static int __init pcifront_init(void)
{
	if (!xen_pv_domain() || xen_initial_domain())	/* [한국어] PV 게스트가 아니거나 dom0 이면 이 드라이버가 할 일이 없다 */
		return -ENODEV;	/* [한국어] 환경 불일치를 -ENODEV 로 알려 모듈 적재를 막는다 */

	if (!xen_has_pv_devices())	/* [한국어] PV 장치 채널 자체가 없으면 XenBus 로 통신할 수 없다 */
		return -ENODEV;	/* [한국어] 역시 -ENODEV. 이후 등록 절차를 진행하지 않는다 */

	pci_frontend_registrar(1 /* enable */);	/* [한국어] MSI/MSI-X 요청을 PV 경로로 우회시키는 전역 훅을 켠다. XenBus 등록보다 먼저 해야 장치가 붙자마자 쓸 수 있다 */

	return xenbus_register_frontend(&xenpci_driver);	/* [한국어] XenBus 코어에 드라이버를 등록한다. 이후 "pci" 백엔드가 나타나면 probe 가 불린다 */
}

/*
 * [한국어]
 * pcifront_cleanup - 모듈 해제 시 등록을 되돌린다
 *
 * @return: 없음.
 *
 * 순서가 중요하다. 먼저 XenBus 등록을 풀어 더 이상 새 장치가 붙지 않게 하고
 * (그 과정에서 이미 붙은 장치들의 remove 콜백이 불린다), 그 다음에
 * 전역 MSI 훅을 내린다. 반대로 하면 아직 살아 있는 장치가 사라진 훅을
 * 참조할 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥(모듈 exit). __exit 이므로 내장 빌드 시 버려진다.
 *
 * 호출 체인:
 *   module_exit -> [pcifront_cleanup] -> xenbus_unregister_driver()
 *                                     -> pci_frontend_registrar()
 */
static void __exit pcifront_cleanup(void)
{
	xenbus_unregister_driver(&xenpci_driver);	/* [한국어] XenBus 에서 드라이버를 떼어낸다. 붙어 있던 장치마다 pcifront_xenbus_remove() 가 불린다 */
	pci_frontend_registrar(0 /* disable */);	/* [한국어] 전역 xen_pci_frontend 훅을 NULL 로 되돌린다. 남은 참조가 없어진 뒤에 해야 안전하다 */
}
/* [한국어] 모듈 적재 진입점 등록. 커널 내장 빌드에서는 initcall 로 들어간다. */
module_init(pcifront_init);
/* [한국어] 모듈 해제 진입점 등록. */
module_exit(pcifront_cleanup);

/* [한국어] modinfo 로 보이는 설명 문자열. */
MODULE_DESCRIPTION("Xen PCI passthrough frontend.");
/* [한국어] 라이선스 선언. GPL 이 아니면 GPL 전용 심볼을 쓸 수 없어 적재가 거부된다. */
MODULE_LICENSE("GPL");
/* [한국어] 모듈 별칭. udev/modprobe 가 "xen:pci" 요청으로 이 모듈을 자동 적재한다. */
MODULE_ALIAS("xen:pci");
