// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2002-2004, 2007 Greg Kroah-Hartman <greg@kroah.com>
 * (C) Copyright 2007 Novell Inc.
 */

/*
 * [한국어 설명] 장치와 드라이버를 짝지어 주는 PCI 버스 타입 구현 (pci-driver.c)
 *
 * === 파일의 역할 ===
 * 커널의 드라이버 모델에서 "버스" 는 장치와 드라이버를 이어 주는 중매인이다.
 * 이 파일이 PCI 버스의 그 역할을 구현한다. 구체적으로 세 가지를 한다.
 *
 *   1) 짝짓기(matching). pci_bus_match() 가 장치의 Vendor/Device/Class ID 를
 *      드라이버가 등록한 id_table 과 대조한다. 맞으면 드라이버 코어가 그
 *      드라이버의 probe 를 부른다. 대조 순서는 driver_override(사용자가
 *      sysfs 로 강제 지정) -> dynids(sysfs new_id 로 추가) -> 정적 id_table 이다.
 *   2) 생애주기. pci_device_probe() / pci_device_remove() / pci_device_shutdown()
 *      이 드라이버의 콜백을 부르기 전후로 PCI 고유의 준비와 정리를 한다 —
 *      IRQ 를 배정하고, struct pci_dev 의 참조를 잡고, 런타임 PM 사용 카운트를
 *      올려 probe 도중 장치가 잠들지 않게 한다.
 *   3) 전원 관리. 파일의 절반 이상이 pci_pm_* 함수들인데, 시스템 절전(S3/S4)과
 *      런타임 절전의 각 단계에서 PCI 표준 동작(config space 저장/복원,
 *      D-state 전환, PME 설정)을 수행하고 그 사이사이에 드라이버 콜백을 끼워 넣는다.
 *
 * 이 파일을 읽을 때 헷갈리기 쉬운 점 하나. pci_pm_* 함수가 스무 개 넘게 있는
 * 이유는 커널 PM 코어가 절전을 여러 단계로 쪼개 놓았기 때문이다. prepare ->
 * suspend -> suspend_late -> suspend_noirq 순으로 내려가는데, 뒤로 갈수록
 * 할 수 있는 일이 줄어든다(noirq 단계에서는 인터럽트가 꺼져 있어 완료
 * 인터럽트를 기다리는 동작을 할 수 없다). 그리고 시스템 절전(suspend),
 * 최대 절전(freeze/thaw/poweroff/restore), 런타임 절전이 각각 자기 계열을
 * 갖는다. 그래서 조합이 스무 개가 넘는다.
 *
 * 또 하나. 이 파일에는 특정 장치 종류를 위한 코드가 한 줄도 없다. 저장장치든
 * 그래픽이든 네트워크든 전부 struct pci_driver 하나로 똑같이 취급된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 장치 발견 (probe.c: pci_scan_device -> pci_device_add)
 *   -> device_add() -> 드라이버 모델이 pci_bus_type 의 match 를 부른다
 *      -> [이 파일] pci_bus_match() -> pci_match_device()
 *         -> 맞으면 -> 드라이버 코어가 pci_bus_type.dma_configure 로
 *            [이 파일] pci_dma_configure() 를 먼저 부르고(DMA/IOMMU 준비),
 *            이어서 pci_bus_type.probe 로 [이 파일] pci_device_probe()
 *            -> pci_assign_irq(), pcibios_alloc_irq() 등 사전 준비
 *            -> __pci_device_probe() -> pci_call_probe()
 *               -> local_pci_probe() -> drv->probe()
 *
 * 절전 시:
 *   PM 코어 -> [이 파일] pci_pm_suspend() -> drv->pm->suspend()
 *           -> [이 파일] pci_pm_suspend_noirq() -> pci_save_state(),
 *              pci_prepare_to_sleep() -> 장치를 D3 로
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. probe 는 PROBE_PREFER_ASYNCHRONOUS 를
 * 지정한 드라이버라면 드라이버 코어가 별도 비동기 스레드에서 돌리고, 그와
 * 별개로 pci_call_probe() 가 장치가 붙은 NUMA 노드의 CPU 로 다시 한 번
 * 옮겨 실행한다. _noirq 계열 PM 콜백만 인터럽트가 꺼진 상태에서 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 커널 드라이버 모델(drivers/base/dd.c, drivers/base/power/main.c).
 *   이 파일은 struct bus_type pci_bus_type 과 struct dev_pm_ops pci_dev_pm_ops 를
 *   채워 그쪽에 넘기는 형태로만 관여한다. 두 파일 모두 이 스파스 체크아웃에는
 *   없어서 호출 지점을 직접 확인하지는 못했다.
 * 아래쪽: pci.c 의 pci_save_state / pci_restore_state / pci_set_power_state /
 *   pci_prepare_to_sleep / pci_enable_acs, irq.c 의 pci_assign_irq 와
 *   pcibios_alloc_irq, iov.c 의 pci_iov_remove / pci_num_vf,
 *   pcie/ptm.c 의 pci_suspend_ptm / pci_resume_ptm.
 * 옆쪽: 각 PCI 드라이버의 struct pci_driver. 이 파일은 그 안의 함수 포인터를
 *   적절한 시점에 부르는 것이 일이다.
 * 공유 상태: struct pci_dev 의 driver 포인터(현재 바인딩된 드라이버),
 *   is_probed / state_saved / skip_bus_pm / current_state 플래그, 그리고
 *   struct pci_dynid 목록(sysfs 의 new_id/remove_id 로 런타임에 추가한 ID).
 *
 * === NVMe 드라이버와의 실제 관계 (drivers/nvme/ 전수 grep 으로 확인) ===
 * 이 트리에는 drivers/nvme 가 함께 있어서 양방향을 직접 확인할 수 있었다.
 * NVMe 가 이 파일을 향해 부르는 것은 등록/해제 한 쌍뿐이다.
 *
 *   drivers/nvme/host/pci.c:5397  pci_register_driver(&nvme_driver)
 *     (매크로가 __pci_register_driver(drv, THIS_MODULE, KBUILD_MODNAME) 로 펼친다)
 *   drivers/nvme/host/pci.c:5407  pci_unregister_driver(&nvme_driver)
 *
 * 나머지는 전부 반대 방향이다 — 이 파일이 NVMe 를 부른다.
 * struct pci_driver nvme_driver (drivers/nvme/host/pci.c:5370) 에 등록된 것들:
 *   .probe    = nvme_probe            <- local_pci_probe() 가 부른다
 *   .remove   = nvme_remove           <- pci_device_remove() 가 부른다
 *   .shutdown = nvme_shutdown         <- pci_device_shutdown() 이 부른다
 *   .id_table = nvme_id_table         <- pci_match_id() 가 대조한다. 그 표의
 *       마지막 항목이 PCI_DEVICE_CLASS(PCI_CLASS_STORAGE_EXPRESS, 0xffffff)
 *       (같은 파일 5361 줄)이라, 벤더를 모르는 컨트롤러도 클래스 코드만으로 잡힌다.
 *   .driver.pm = &nvme_dev_pm_ops     <- pci_pm_* 들이 각 단계에서 부른다
 *   .driver.probe_type = PROBE_PREFER_ASYNCHRONOUS
 *       NVMe 의 probe 는 Identify Controller 명령 완료를 기다리느라 느리다.
 *       이 지정이 있으면 드라이버 코어가 probe 를 비동기로 돌리므로,
 *       SSD 를 여러 개 꽂은 시스템의 부팅 시간이 줄어든다.
 *   .sriov_configure = pci_sriov_configure_simple  <- iov.c 의 sysfs 경로가 부른다
 *   .err_handler = &nvme_err_handler  <- 이것은 이 파일이 부르지 않는다.
 *       AER/EEH 복구 경로(drivers/pci/pcie/err.c 등)의 몫이다. 이 파일이
 *       그 경로와 닿는 곳은 사용자 공간에 uevent 를 쏘는 pci_uevent_ers()
 *       하나뿐이다.
 *
 * 한 가지 짚어 둘 것. nvme_dev_pm_ops (drivers/nvme/host/pci.c:5083) 에는
 * .suspend / .resume / .freeze / .thaw / .poweroff / .restore 여섯 개만
 * 있고 *_noirq 도 runtime_* 도 없다. 그래서 pci_pm_suspend_noirq() 나
 * pci_pm_runtime_suspend() 안의 pm->suspend_noirq / pm->runtime_suspend 는
 * NVMe 에 대해서는 늘 NULL 이고, 그 경우 PCI 계층이 스스로
 * pci_save_state() + pci_prepare_to_sleep() 을 해 준다. 이 파일의 함수
 * 이름만 보고 "nvme_suspend_noirq 가 있겠거니" 하고 넘겨짚으면 틀린다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_bus_match()        : 장치와 드라이버의 짝을 판정. pci_match_device() 로 위임.
 * pci_match_device()     : driver_override 를 먼저 보고, 런타임에 추가된 dynid,
 *                          마지막으로 드라이버의 정적 id_table 을 훑는다.
 * pci_device_probe()     : 바인딩 직전 준비 후 드라이버 probe 호출. 실패하면
 *                          잡아 둔 참조와 IRQ 를 되돌린다.
 * pci_call_probe()       : probe 를 어느 CPU 에서 실행할지 정한다. 장치가
 *                          붙은 NUMA 노드에서 돌려야 그 노드 메모리로
 *                          드라이버 자료구조가 잡힌다.
 * local_pci_probe()      : 실제로 drv->probe() 를 부르는 자리. 전후로 런타임 PM
 *                          참조를 잡아 probe 도중 장치가 잠들지 않게 한다.
 * pci_device_remove()    : drv->remove() 호출 후 IRQ/참조/런타임 PM 정리.
 * pci_device_shutdown()  : 시스템 종료 시 drv->shutdown() 호출. kexec 이면
 *                          Bus Master 를 꺼서 다음 커널의 메모리를 장치가
 *                          DMA 로 덮어쓰지 못하게 막는다.
 * pci_pm_* (20여 개)     : 시스템/최대절전/런타임 절전의 각 단계 처리.
 * pci_dev_pm_ops         : 위 함수들을 단계별 슬롯에 꽂은 struct dev_pm_ops.
 * pci_bus_type           : 위 전부를 담아 드라이버 코어에 넘기는 struct bus_type.
 * pci_add_dynid() / new_id_store() : sysfs 로 런타임에 ID 를 추가해 드라이버에
 *                          없는 장치를 강제로 바인딩하는 경로.
 * __pci_register_driver() / pci_unregister_driver() : 드라이버 등록/해제.
 * pci_dma_configure() / pci_dma_cleanup() : 바인딩 전후의 DMA·IOMMU 설정.
 */

#include <linux/pci.h>		/* [한국어] struct pci_dev, struct pci_driver, pci_match_one_device() 등 이 파일이 다루는 모든 PCI 자료형 */
#include <linux/module.h>	/* [한국어] __pci_register_driver() 가 받는 struct module, EXPORT_SYMBOL 계열 */
#include <linux/init.h>		/* [한국어] __init 과 postcore_initcall() — pci_driver_init() 을 부팅 초기에 돌리기 위해 */
#include <linux/device.h>	/* [한국어] struct bus_type, struct device_driver, driver_register(), driver_attach() 등 드라이버 모델 본체 */
#include <linux/mempolicy.h>	/* [한국어] NUMA 노드 관련 정의. pci_call_probe() 가 노드 번호를 다룬다 */
#include <linux/string.h>	/* [한국어] 문자열 헬퍼 */
#include <linux/slab.h>		/* [한국어] kzalloc_obj()/kfree() — pci_dynid 와 임시 pci_dev 할당 */
#include <linux/sched.h>	/* [한국어] 스케줄러/태스크 정의 */
#include <linux/sched/isolation.h> /* [한국어] housekeeping_cpumask(HK_TYPE_DOMAIN) — 격리된(isolcpus) CPU 에 probe 워크를 던지지 않기 위해 */
#include <linux/cpu.h>		/* [한국어] cpu_hotplug_disable()/enable() — probe 실행 CPU 를 고르는 동안 그 CPU 가 빠지지 않게 */
#include <linux/pm_runtime.h>	/* [한국어] pm_runtime_get_sync()/put_sync()/barrier() — probe·remove 구간의 런타임 PM 사용 카운트 */
#include <linux/suspend.h>	/* [한국어] pm_suspend_no_platform(), pm_resume_via_firmware() 등 절전 방식 판별 */
#include <linux/kexec.h>	/* [한국어] kexec_in_progress — 종료가 kexec 인지 판단해 Bus Master 를 끈다 */
#include <linux/of_device.h>	/* [한국어] of_dma_configure() — 디바이스 트리 기반 DMA 설정 경로 */
#include <linux/acpi.h>		/* [한국어] has_acpi_companion(), acpi_dma_configure() — ACPI 기반 DMA 설정 경로 */
#include <linux/dma-map-ops.h>	/* [한국어] arch_teardown_dma_ops() — DMA 설정 실패 시 되돌리기 */
#include <linux/iommu.h>	/* [한국어] iommu_device_use_default_domain()/unuse — 드라이버가 직접 IOMMU 를 다루지 않는 경우의 기본 도메인 사용 표시 */
#include "pci.h"		/* [한국어] drivers/pci 내부 전용 선언. pci_dev_need_resume(), pci_save_state() 등 이 파일이 쓰는 대부분의 헬퍼가 여기서 온다 */
#include "pcie/portdrv.h"	/* [한국어] pcie_port_bus_type — pci_driver_init() 이 이 버스도 같이 등록한다 */

/* [한국어]
 * struct pci_dynid - sysfs new_id 로 런타임에 덧붙인 PCI 장치 ID 한 칸
 *
 * 드라이버가 컴파일 시점에 갖는 id_table 은 고정이다. 그런데 같은 실리콘의
 * 새 리비전이 Device ID 만 바꿔 나오는 일이 흔하고, 그러면 기존 드라이버로
 * 충분히 동작하는데도 ID 를 몰라 바인딩되지 않는다. 그때 사용자가
 *   echo "8086 0953" > /sys/bus/pci/drivers/<드라이버>/new_id
 * 로 ID 를 밀어 넣으면 이 구조체 한 칸이 만들어져 그 드라이버의 dynids
 * 목록에 매달리고, 이후 pci_match_device() 가 정적 id_table 보다 먼저
 * 이 목록을 본다. 반대 방향은 remove_id sysfs 파일이다.
 *
 * 생성: pci_add_dynid() 가 kzalloc 으로 만든다(GFP_KERNEL, 잠들 수 있음).
 * 파괴: remove_id_store() 가 하나씩, pci_free_dynids() 가 드라이버 해제 때 전부.
 * 보호: drv->dynids.lock (spinlock). 순회·삽입·삭제 모두 이 락 안에서 한다.
 */
struct pci_dynid {
	struct list_head node;
	/* [한국어] struct pci_driver 의 dynids.list 에 이 칸을 매다는 링크.
	 * 설정자: pci_add_dynid() 가 list_add_tail() 로 꼬리에 붙인다.
	 * 읽는 자: pci_match_device() 의 list_for_each_entry(),
	 *   remove_id_store() 와 pci_free_dynids() 의 list_for_each_entry_safe().
	 * 값 범위: 목록에 들어 있는 동안 항상 유효한 리스트 노드다.
	 * 동기화: drv->dynids.lock 을 잡은 구간에서만 조작한다. */

	struct pci_device_id id;
	/* [한국어] 실제 대조에 쓰이는 ID 값 묶음. vendor/device/subvendor/
	 *   subdevice 와 class/class_mask, 그리고 driver_data 로 이루어진다.
	 * 설정자: pci_add_dynid() 가 인자를 그대로 복사해 채운다.
	 * 읽는 자: pci_match_device() 가 pci_match_one_device() 에 넘겨 대조한다.
	 * 값 범위: 각 ID 필드는 PCI_ANY_ID 로 "무엇이든" 을 뜻할 수 있다.
	 *   driver_data 는 new_id_store() 가 정적 id_table 에 이미 존재하는
	 *   값만 통과시키므로, 사용자가 임의의 정수를 드라이버에 주입해
	 *   quirk 비트를 조작하는 것은 막혀 있다.
	 * 동기화: node 와 같다. */
};

/**
 * pci_add_dynid - add a new PCI device ID to this driver and re-probe devices
 * @drv: target pci driver
 * @vendor: PCI vendor ID
 * @device: PCI device ID
 * @subvendor: PCI subvendor ID
 * @subdevice: PCI subdevice ID
 * @class: PCI class
 * @class_mask: PCI class mask
 * @driver_data: private driver data
 *
 * Adds a new dynamic pci device ID to this driver and causes the
 * driver to probe for all devices again.  @drv must have been
 * registered prior to calling this function.
 *
 * CONTEXT:
 * Does GFP_KERNEL allocation.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
/* [한국어]
 * pci_add_dynid - 드라이버에 ID 한 칸을 덧붙이고 장치를 다시 훑는다
 *
 * @drv: ID 를 받을 드라이버. 이미 driver_register 된 상태여야 한다.
 * @vendor/@device/@subvendor/@subdevice: 대조할 PCI ID 값들. PCI_ANY_ID 로
 *   "무엇이든" 을 지정할 수 있다.
 * @class/@class_mask: 클래스 코드 조건. mask 비트가 1 인 자리만 비교한다.
 * @driver_data: 매칭 시 probe 에 함께 넘어갈 드라이버 사설 값(보통 quirk 비트).
 * @return: 0 성공. -ENOMEM 이면 할당 실패, 그 밖의 음수는 driver_attach() 가
 *   돌려준 값이다. 호출자(new_id_store)는 실패를 그대로 사용자에게 돌려준다.
 *
 * 왜 필요한가. 드라이버의 id_table 은 컴파일 시점에 고정이라, 같은 칩의 새
 * 리비전이 Device ID 만 바꿔 나오면 커널을 새로 빌드하기 전에는 붙일 방법이
 * 없다. 이 함수가 그 구멍을 메운다.
 *
 * 동작 순서. (1) pci_dynid 한 칸을 kzalloc 한다. (2) 인자를 그 안의
 * pci_device_id 에 복사한다. (3) drv->dynids.lock 을 잡고 목록 꼬리에 매단다.
 * (4) driver_attach() 를 불러 시스템의 모든 PCI 장치를 이 드라이버 기준으로
 * 다시 훑게 한다. 그 재탐색이 pci_bus_match() -> pci_match_device() 로
 * 이어지고, 이번에는 새로 넣은 ID 가 목록에 있으니 짝이 맞는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. kzalloc 이 GFP_KERNEL 이므로 잠들 수 있다.
 * 목록 조작 구간만 spin_lock 으로 짧게 보호하고, driver_attach() 는 락 밖에서
 * 부른다 — 그 안에서 probe 가 돌며 잠들 수 있기 때문이다.
 *
 * 에러 경로: 할당 실패면 목록을 건드리기 전에 -ENOMEM 으로 빠진다. 목록에
 * 넣은 뒤 driver_attach() 가 실패해도 넣은 ID 는 되돌리지 않는다 — 그 ID 는
 * 유효하며, 나중에 remove_id 로 지우거나 드라이버 해제 때 정리된다.
 *
 * 호출 체인:
 *   사용자의 sysfs new_id 쓰기 → new_id_store() → [이 함수] → driver_attach()
 *   외부 모듈도 EXPORT_SYMBOL_GPL 로 이 함수를 직접 부를 수 있다.
 */
int pci_add_dynid(struct pci_driver *drv,
		  unsigned int vendor, unsigned int device,	/* [한국어] @drv 는 이미 driver_register() 된 드라이버여야 한다 — 아직 등록 전이면 driver_attach 가 할 일이 없다 */
		  unsigned int subvendor, unsigned int subdevice,
		  unsigned int class, unsigned int class_mask,
		  unsigned long driver_data)
{
	struct pci_dynid *dynid;	/* [한국어] 목록에 매달 새 ID 칸. 아래에서 kzalloc 으로 받는다 */

	dynid = kzalloc_obj(*dynid);	/* [한국어] kzalloc_obj(*dynid) 는 sizeof(*dynid) 만큼 0 초기화 할당. GFP_KERNEL 이라 잠들 수 있다 */
	if (!dynid)	/* [한국어] 할당 실패 검사 */
		return -ENOMEM;	/* [한국어] 목록을 건드리기 전이므로 되돌릴 것 없이 바로 나간다 */

	dynid->id.vendor = vendor;	/* [한국어] 이하 일곱 줄이 인자를 pci_device_id 로 옮겨 담는 부분. PCI_ANY_ID 를 그대로 받아 와일드카드로 쓴다 */
	dynid->id.device = device;	/* [한국어] Device ID */
	dynid->id.subvendor = subvendor;	/* [한국어] Subsystem Vendor ID */
	dynid->id.subdevice = subdevice;	/* [한국어] Subsystem Device ID */
	dynid->id.class = class;	/* [한국어] 클래스 코드(Base/Sub/ProgIF 3 바이트가 하나의 u32 에 들어 있다) */
	dynid->id.class_mask = class_mask;	/* [한국어] 클래스 비교에 쓸 마스크. 이 비트가 1 인 자리만 비교한다 */
	dynid->id.driver_data = driver_data;	/* [한국어] probe 에 함께 넘어갈 드라이버 사설 값. new_id_store 가 정적 표에 있는 값만 통과시킨다 */

	spin_lock(&drv->dynids.lock);	/* [한국어] 목록 조작 구간 진입. pci_match_device 의 순회와 경쟁하므로 반드시 이 락이 필요하다 */
	list_add_tail(&dynid->node, &drv->dynids.list);	/* [한국어] 꼬리에 붙인다 — 먼저 넣은 ID 가 먼저 대조되도록 */
	spin_unlock(&drv->dynids.lock);	/* [한국어] 락 해제. 아래 driver_attach 는 잠들 수 있으므로 반드시 락 밖에서 부른다 */

	return driver_attach(&drv->driver);	/* [한국어] 등록된 모든 PCI 장치를 이 드라이버 기준으로 다시 훑는다. 그 안에서 pci_bus_match → pci_device_probe 가 돌 수 있다 */
}
EXPORT_SYMBOL_GPL(pci_add_dynid);	/* [한국어] GPL 모듈만 쓸 수 있게 공개. 드라이버가 자기 ID 표를 런타임에 늘릴 때 쓴다 */

/* [한국어]
 * pci_free_dynids - 드라이버가 물고 있던 동적 ID 목록을 통째로 해제
 *
 * @drv: 등록 해제 중인 드라이버.  @return: 없음.
 *
 * sysfs new_id 로 밀어 넣은 ID 들은 kzalloc 으로 잡힌 메모리다. 드라이버가
 * 커널에서 빠질 때 이것을 놓아 주지 않으면 그대로 새는다. 이 함수가 그
 * 정리를 맡는다.
 *
 * 동작: dynids.lock 을 잡고 list_for_each_entry_safe 로 목록을 훑으며
 * 각 칸을 목록에서 떼어 낸 뒤 kfree 한다. _safe 변형을 쓰는 이유는 순회
 * 도중 현재 노드를 free 하기 때문이다 — 다음 노드 포인터를 미리 챙겨 두지
 * 않으면 해제된 메모리를 따라가게 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. spin_lock 구간 안에서 kfree 를 부르지만
 * kfree 자체는 잠들지 않으므로 문제되지 않는다.
 *
 * 에러 경로: 없다. 반환값이 없고 실패할 여지도 없다.
 *
 * 호출 체인:
 *   모듈 언로드 → pci_unregister_driver() → [이 함수] → kfree()
 */
static void pci_free_dynids(struct pci_driver *drv)
{
	struct pci_dynid *dynid, *n;	/* [한국어] dynid 는 현재 노드, n 은 다음 노드를 미리 담아 둘 자리. 현재 노드를 free 하므로 반드시 필요하다 */

	spin_lock(&drv->dynids.lock);	/* [한국어] 목록 조작 구간 진입 */
	list_for_each_entry_safe(dynid, n, &drv->dynids.list, node) {	/* [한국어] _safe 변형 — 본문에서 현재 노드를 kfree 하기 때문 */
		list_del(&dynid->node);	/* [한국어] 먼저 목록에서 떼어 낸다. 순서를 바꾸면 해제된 메모리를 목록이 가리키게 된다 */
		kfree(dynid);	/* [한국어] 그다음 해제 */
	}
	spin_unlock(&drv->dynids.lock);	/* [한국어] 목록 조작 구간 종료 */
}

/**
 * pci_match_id - See if a PCI device matches a given pci_id table
 * @ids: array of PCI device ID structures to search in
 * @dev: the PCI device structure to match against.
 *
 * Used by a driver to check whether a PCI device is in its list of
 * supported devices.  Returns the matching pci_device_id structure or
 * %NULL if there is no match.
 *
 * Deprecated; don't use this as it will not catch any dynamic IDs
 * that a driver might want to check for.
 */
/* [한국어]
 * pci_match_id - ID 표를 처음부터 훑어 이 장치와 맞는 항목을 찾는다
 *
 * @ids: 끝이 빈 항목(vendor·subvendor·class_mask 가 모두 0)으로 표시된
 *   pci_device_id 배열. NULL 이어도 된다.
 * @dev: 대조할 장치.
 * @return: 맞는 항목의 주소, 없으면 NULL.
 *
 * 순회 종료 조건이 조금 특이하다. `ids->vendor || ids->subvendor ||
 * ids->class_mask` 가 거짓이 되는 항목에서 멈추는데, 이는 표의 마지막에
 * 관례적으로 두는 `{ 0, }` 항목을 만나면 끝낸다는 뜻이다. device 필드는
 * 조건에 없다는 점에 주의 — vendor 없이 클래스만으로 매칭하는 항목
 * (PCI_DEVICE_CLASS 매크로가 만드는 형태)을 표 중간에 둘 수 있어야 하기
 * 때문이다.
 *
 * 실제 비교는 pci_match_one_device() 가 한다(이 트리에서는 선언을 찾지
 * 못했다 — include/linux/pci.h 가 체크아웃에 없다). 네 ID 필드는
 * PCI_ANY_ID 를 와일드카드로 인정하고, 클래스는 mask 로 가린 뒤 비교한다.
 *
 * 원본 주석이 밝히듯 이 함수는 deprecated 다. dynids 를 보지 않기 때문에
 * "이 드라이버가 이 장치를 맡을 수 있나" 라는 질문의 답으로는 불완전하다.
 * 그 완전한 판정은 pci_match_device() 다.
 *
 * 실행 컨텍스트: 락을 잡지 않는다. 정적 id_table 은 드라이버 수명 동안
 * 변하지 않으므로 보호가 필요 없다. 호출자가 dynids 목록을 넘길 때는
 * 호출자 쪽에서 락을 잡아야 하지만, 실제로 그렇게 쓰는 곳은 없다.
 *
 * 호출 체인:
 *   pci_match_device() → [이 함수] → pci_match_one_device()
 *   외부 드라이버도 EXPORT_SYMBOL 로 직접 부를 수 있다.
 */
const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
					 struct pci_dev *dev)
{
	if (ids) {	/* [한국어] ids 가 NULL 인 드라이버(id_table 을 두지 않은 드라이버)도 있으므로 먼저 확인 */
		while (ids->vendor || ids->subvendor || ids->class_mask) {	/* [한국어] 표의 끝 표시인 { 0, } 항목을 만나면 멈춘다. device 필드를 조건에 넣지 않는 이유는 벤더 없이 클래스만으로 매칭하는 항목을 허용하기 위해서다 */
			if (pci_match_one_device(ids, dev))	/* [한국어] 실제 비교. 네 ID 는 PCI_ANY_ID 를 와일드카드로 인정하고 클래스는 마스크로 가려 비교한다 */
				return ids;	/* [한국어] 맞았으면 그 항목의 주소를 그대로 돌려준다. 호출자는 이것을 probe 에 넘긴다 */
			ids++;	/* [한국어] 다음 항목으로 */
		}
	}
	return NULL;	/* [한국어] 끝까지 못 찾았거나 표가 없으면 NULL */
}
EXPORT_SYMBOL(pci_match_id);	/* [한국어] 라이선스 제한 없이 공개. 오래된 API 라 이렇게 남아 있다 */

/* [한국어]
 * pci_device_id_any - driver_override 로 강제 바인딩할 때 건네는 만능 ID
 *
 * /sys/bus/pci/devices/<주소>/driver_override 에 드라이버 이름을 쓰면,
 * 그 장치는 id_table 대조 결과와 무관하게 그 드라이버에 붙는다(vfio-pci 로
 * 장치를 넘길 때 쓰는 그 경로다). 그런데 드라이버의 probe() 는 두 번째
 * 인자로 "매칭된 pci_device_id" 를 받게 되어 있어서, 매칭을 건너뛴 이
 * 경우에도 무언가는 넘겨 주어야 한다. 그 자리를 채우는 더미가 이것이다.
 *
 * 설정자: 없다. 컴파일 시점 상수이며 파일 전역에 하나만 존재한다.
 * 읽는 자: pci_match_device() 의 마지막 분기가 주소를 반환하고, 그 값이
 *   pci_call_probe() -> local_pci_probe() 를 거쳐 drv->probe() 에 닿는다.
 * 값 범위: 네 ID 필드 모두 PCI_ANY_ID. class/class_mask 는 0 이므로
 *   클래스 조건도 사실상 없다. driver_data 도 0 이라 quirk 는 붙지 않는다.
 * 동기화: 읽기 전용 const 이므로 락이 필요 없다.
 */
static const struct pci_device_id pci_device_id_any = {
	.vendor = PCI_ANY_ID,		/* [한국어] Vendor ID 무시 */
	.device = PCI_ANY_ID,		/* [한국어] Device ID 무시 */
	.subvendor = PCI_ANY_ID,	/* [한국어] Subsystem Vendor ID 무시 */
	.subdevice = PCI_ANY_ID,	/* [한국어] Subsystem Device ID 무시 */
};

/**
 * pci_match_device - See if a device matches a driver's list of IDs
 * @drv: the PCI driver to match against
 * @dev: the PCI device structure to match against
 *
 * Used by a driver to check whether a PCI device is in its list of
 * supported devices or in the dynids list, which may have been augmented
 * via the sysfs "new_id" file.  Returns the matching pci_device_id
 * structure or %NULL if there is no match.
 */
/* [한국어]
 * pci_match_device - 이 드라이버가 이 장치를 맡을 수 있는지 최종 판정
 *
 * @drv: 후보 드라이버.  @dev: 후보 장치.
 * @return: 매칭에 쓰인 pci_device_id 의 주소, 아니면 NULL. 이 반환값은
 *   그대로 drv->probe() 의 두 번째 인자가 되므로 단순한 참·거짓이 아니라
 *   "어느 항목으로 맞았는지" 를 알려 주는 것이 중요하다.
 *
 * 판정은 세 단계다.
 *
 *   (1) driver_override. 사용자가 /sys/bus/pci/devices/<주소>/driver_override
 *       에 드라이버 이름을 써 두었으면 그 이름과 다른 드라이버는 이 장치에
 *       손댈 수 없다. device_match_driver_override() 가 0 을 돌려주면
 *       (다른 드라이버가 지정되어 있다는 뜻) 즉시 NULL 이다.
 *   (2) dynids. sysfs new_id 로 추가한 목록을 먼저 본다. 사용자가 명시적으로
 *       넣은 것이니 정적 표보다 우선한다.
 *   (3) 정적 id_table. pci_match_id() 로 훑는다. 다만 override_only 표시가
 *       붙은 항목은 driver_override 로 지정된 경우(ret > 0)에만 인정한다 —
 *       "자동으로는 붙지 말고 사용자가 지목할 때만 붙어라" 라는 항목이다.
 *
 * 세 단계를 다 통과하지 못했는데 driver_override 로 이 드라이버가 지목된
 * 상태라면(ret > 0) 그래도 붙여야 한다. 이때 probe 에 넘길 ID 가 없으므로
 * 만능 더미 pci_device_id_any 의 주소를 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. dynids 목록 순회 구간만 spin_lock 으로
 * 보호한다. 락 안에서는 포인터만 챙기고 락을 푼 뒤에 반환한다 — 반환된
 * ID 가 그 사이 remove_id 로 사라질 수 있다는 이론적 창이 있지만, 상류
 * 코드가 그렇게 되어 있다.
 *
 * 에러 경로: 실패라는 개념이 없다. 못 맞추면 NULL 이고, 그것은 정상이다.
 *
 * 호출 체인:
 *   드라이버 코어의 버스 매칭 → pci_bus_match() → [이 함수]
 *   __pci_device_probe() → [이 함수]  (probe 직전 ID 를 다시 얻기 위해)
 *   new_id_store() → [이 함수]        (중복 등록인지 확인하기 위해)
 */
static const struct pci_device_id *pci_match_device(struct pci_driver *drv,
						    struct pci_dev *dev)
{
	struct pci_dynid *dynid;	/* [한국어] dynids 목록 순회용 커서 */
	const struct pci_device_id *found_id = NULL, *ids;	/* [한국어] found_id 는 결과, ids 는 정적 표를 훑는 커서. found_id 를 NULL 로 시작해 두는 것이 아래 분기의 전제다 */
	int ret;	/* [한국어] device_match_driver_override 의 결과. 0 = 다른 드라이버가 지정됨, 양수 = 이 드라이버가 지정됨, 음수 = 지정 없음 */

	/* When driver_override is set, only bind to the matching driver */
	ret = device_match_driver_override(&dev->dev, &drv->driver);	/* [한국어] 사용자가 driver_override 로 특정 드라이버를 지목했는지 확인한다 */
	if (ret == 0)	/* [한국어] 0 이면 다른 드라이버가 지목된 것이므로 이 드라이버는 손대면 안 된다 */
		return NULL;	/* [한국어] 즉시 실패. 정적 표에 있더라도 무시한다 */

	/* Look at the dynamic ids first, before the static ones */
	spin_lock(&drv->dynids.lock);	/* [한국어] dynids 목록 순회 구간 진입 */
	list_for_each_entry(dynid, &drv->dynids.list, node) {	/* [한국어] sysfs new_id 로 넣은 항목들을 먼저 본다 — 사용자가 명시적으로 넣은 것이므로 정적 표보다 우선한다 */
		if (pci_match_one_device(&dynid->id, dev)) {	/* [한국어] 정적 표와 같은 비교 함수를 쓴다 */
			found_id = &dynid->id;	/* [한국어] 맞은 항목의 주소를 챙긴다 */
			break;	/* [한국어] 첫 번째 것만 쓰고 멈춘다 */
		}
	}
	spin_unlock(&drv->dynids.lock);	/* [한국어] 락 해제. 아래에서 found_id 를 반환하지만 그 대상은 remove_id 가 지우기 전까지 유효하다(상류 코드 그대로) */

	if (found_id)	/* [한국어] dynids 에서 찾았으면 */
		return found_id;	/* [한국어] 정적 표는 보지도 않고 그것을 돌려준다 */

	for (ids = drv->id_table; (found_id = pci_match_id(ids, dev));	/* [한국어] 정적 id_table 순회. 조건식 안에서 pci_match_id 를 부르는 형태라, 한 번 맞을 때마다 루프 본문이 돈다 */
	     ids = found_id + 1) {	/* [한국어] 다음 회차에는 방금 맞은 항목의 바로 다음부터 다시 훑는다 — override_only 때문에 건너뛴 뒤 계속 찾아야 하므로 */
		/*
		 * The match table is split based on driver_override.
		 * In case override_only was set, enforce driver_override
		 * matching.
		 */
		if (found_id->override_only) {	/* [한국어] 이 항목이 driver_override 로 지목했을 때만 유효한 항목인가 */
			if (ret > 0)	/* [한국어] 그렇다면 실제로 지목된 경우(ret 양수)에만 인정한다 */
				return found_id;	/* [한국어] 인정 */
		} else {	/* [한국어] 평범한 항목이면 */
			return found_id;	/* [한국어] 조건 없이 그대로 돌려준다 */
		}
	}

	/* driver_override will always match, send a dummy id */
	if (ret > 0)	/* [한국어] 표에서 못 찾았지만 사용자가 이 드라이버를 지목했다면 */
		return &pci_device_id_any;	/* [한국어] probe 에 넘길 것이 없으므로 만능 더미 ID 의 주소를 돌려준다 */
	return NULL;	/* [한국어] 어느 경로로도 못 맞췄다 — 정상적인 결과다 */
}

/**
 * new_id_store - sysfs frontend to pci_add_dynid()
 * @driver: target device driver
 * @buf: buffer for scanning device ID data
 * @count: input size
 *
 * Allow PCI IDs to be added to an existing driver via sysfs.
 */
/* [한국어]
 * new_id_store - sysfs new_id 파일에 쓴 문자열을 파싱해 동적 ID 로 등록
 *
 * @driver: 대상 드라이버(sysfs 속성이 달린 device_driver).
 * @buf: 사용자가 쓴 문자열. "vendor device [subvendor subdevice class
 *   class_mask driver_data]" 형태로 최대 7 개의 16 진수를 받는다.
 * @count: 쓴 바이트 수.
 * @return: 성공하면 @count(전부 소비했다는 뜻), 실패하면 음수 errno.
 *   sysfs 규약상 반환값이 count 와 다르면 write(2) 가 그만큼만 썼다고
 *   보고하거나 오류를 낸다.
 *
 * 동작 순서.
 *   (1) sscanf 로 필드를 뽑는다. 최소 두 개(vendor, device)는 있어야 한다.
 *   (2) 7 개를 다 주지 않았다면 driver_data 를 사용자가 지정하지 않은
 *       것이므로, 임시 pci_dev 를 하나 만들어 이미 매칭되는지 확인한다.
 *       이미 붙을 수 있는 장치라면 -EEXIST 로 거절한다. 이 임시 pci_dev 는
 *       진짜 장치가 아니라 pci_match_device() 에 넘기기 위한 껍데기이며
 *       바로 kfree 된다.
 *   (3) driver_data 를 지정했다면, 그 값이 드라이버의 정적 id_table 에
 *       실제로 존재하는 값인지 검사한다. 이것이 중요한 안전장치다 —
 *       driver_data 는 보통 quirk 비트 묶음이거나 드라이버 내부 인덱스라,
 *       사용자가 임의의 정수를 넣게 두면 드라이버가 엉뚱한 동작을 하거나
 *       배열 밖을 짚을 수 있다.
 *   (4) 검사를 통과하면 pci_add_dynid() 로 넘긴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(쓰기 시스템 호출). 잠들 수 있다.
 *
 * 에러 경로: 필드 부족은 -EINVAL, 임시 pci_dev 할당 실패는 -ENOMEM,
 * 이미 매칭되면 -EEXIST, driver_data 가 표에 없으면 -EINVAL.
 *
 * 호출 체인:
 *   echo ... > /sys/bus/pci/drivers/<이름>/new_id → sysfs 저장 핸들러
 *   → [이 함수] → pci_add_dynid() → driver_attach()
 */
static ssize_t new_id_store(struct device_driver *driver, const char *buf,
			    size_t count)
{
	struct pci_driver *pdrv = to_pci_driver(driver);	/* [한국어] sysfs 는 일반 device_driver 를 넘겨 주므로 감싸고 있는 pci_driver 로 되돌린다 */
	const struct pci_device_id *ids = pdrv->id_table;	/* [한국어] driver_data 검증에 쓸 정적 ID 표. 표가 없는 드라이버면 NULL 이다 */
	u32 vendor, device, subvendor = PCI_ANY_ID,	/* [한국어] 지정하지 않은 필드의 기본값을 미리 넣어 둔다 — subvendor/subdevice 는 와일드카드, */
		subdevice = PCI_ANY_ID, class = 0, class_mask = 0;	/* [한국어] class 는 0 이고 class_mask 도 0 이라 클래스 조건이 없는 셈이 된다 */
	unsigned long driver_data = 0;	/* [한국어] driver_data 기본값 0 */
	int fields;	/* [한국어] sscanf 가 실제로 채운 필드 개수 */
	int retval = 0;	/* [한국어] 반환값 겸 중간 판정 저장소 */

	fields = sscanf(buf, "%x %x %x %x %x %x %lx",	/* [한국어] %x 여섯 개와 %lx 하나. 사용자는 앞에서부터 원하는 만큼만 줄 수 있다 */
			&vendor, &device, &subvendor, &subdevice,	/* [한국어] ID 네 개 */
			&class, &class_mask, &driver_data);	/* [한국어] 클래스 두 개와 driver_data */
	if (fields < 2)	/* [한국어] vendor 와 device 는 반드시 있어야 한다 */
		return -EINVAL;	/* [한국어] 모자라면 잘못된 입력 */

	if (fields != 7) {	/* [한국어] 일곱 개를 다 주지 않았다 = driver_data 를 지정하지 않았다 */
		struct pci_dev *pdev = kzalloc_obj(*pdev);	/* [한국어] 대조용 임시 껍데기. 진짜 장치가 아니라 pci_match_device 에 넘기기 위한 것이다 */
		if (!pdev)	/* [한국어] 할당 실패 검사 */
			return -ENOMEM;	/* [한국어] 즉시 실패 */

		pdev->vendor = vendor;	/* [한국어] 이하 다섯 줄이 껍데기에 사용자가 준 ID 를 채우는 부분 */
		pdev->device = device;	/* [한국어] Device ID */
		pdev->subsystem_vendor = subvendor;	/* [한국어] Subsystem Vendor ID */
		pdev->subsystem_device = subdevice;	/* [한국어] Subsystem Device ID */
		pdev->class = class;	/* [한국어] 클래스 코드 */

		if (pci_match_device(pdrv, pdev))	/* [한국어] 이 ID 로 이미 매칭이 되는가 — 그렇다면 굳이 추가할 필요가 없다 */
			retval = -EEXIST;	/* [한국어] 중복이므로 거절한다 */

		kfree(pdev);	/* [한국어] 껍데기는 역할이 끝났으니 즉시 해제. 반환 경로마다 free 를 흩뿌리지 않으려고 판정과 해제를 분리했다 */

		if (retval)	/* [한국어] 중복 판정이 났으면 */
			return retval;	/* [한국어] 그 오류를 그대로 돌려준다 */
	}

	/* Only accept driver_data values that match an existing id_table
	   entry */
	if (ids) {	/* [한국어] 정적 표가 있는 드라이버라면 driver_data 값을 검증한다 */
		retval = -EINVAL;	/* [한국어] 일단 실패로 놓고 시작 — 표 안에서 같은 값을 찾아야만 통과다 */
		while (ids->vendor || ids->subvendor || ids->class_mask) {	/* [한국어] 표의 끝 표시를 만날 때까지 */
			if (driver_data == ids->driver_data) {	/* [한국어] 사용자가 준 driver_data 가 이 항목의 값과 같은가 */
				retval = 0;	/* [한국어] 같으면 통과 */
				break;	/* [한국어] 더 볼 것 없다 */
			}
			ids++;	/* [한국어] 다음 항목으로 */
		}
		if (retval)	/* No match */
			return retval;	/* [한국어] 거절한다. 임의의 정수를 드라이버 사설 값으로 주입하지 못하게 막는 안전장치다 */
	}

	retval = pci_add_dynid(pdrv, vendor, device, subvendor, subdevice,	/* [한국어] 검증을 다 통과했으니 실제 등록으로 넘긴다 */
			       class, class_mask, driver_data);	/* [한국어] 클래스 조건과 driver_data 를 함께 전달 */
	if (retval)	/* [한국어] 등록 실패면 */
		return retval;	/* [한국어] 그 오류를 그대로 사용자에게 */
	return count;	/* [한국어] 성공. sysfs 규약상 소비한 바이트 수를 돌려줘야 한다 */
}
/* [한국어]
 * static DRIVER_ATTR_WO(new_id) — 쓰기 전용 sysfs 속성 하나를 만든다.
 * 매크로가 struct driver_attribute driver_attr_new_id 를 만들고, 그 안의
 * .store 를 위의 new_id_store() 로, 권한을 0200(소유자 쓰기 전용)으로
 * 채운다. WO(write only) 이므로 읽으려 하면 실패한다 — 이미 넣은 ID 를
 * 되읽는 인터페이스는 없다.
 */
static DRIVER_ATTR_WO(new_id);

/**
 * remove_id_store - remove a PCI device ID from this driver
 * @driver: target device driver
 * @buf: buffer for scanning device ID data
 * @count: input size
 *
 * Removes a dynamic pci device ID to this driver.
 */
/* [한국어]
 * remove_id_store - sysfs remove_id 로 동적 ID 한 칸을 지운다
 *
 * @driver: 대상 드라이버.
 * @buf: "vendor device [subvendor subdevice class class_mask]" 형태의 문자열.
 *   new_id 와 달리 driver_data 는 받지 않는다 — 지울 때는 식별만 하면 된다.
 * @count: 쓴 바이트 수.
 * @return: 성공하면 @count, 해당 ID 가 없으면 -ENODEV, 파싱 실패면 -EINVAL.
 *
 * 대조 규칙이 pci_match_one_device() 와 다르다는 점에 주의. 여기서는
 * vendor 와 device 는 정확히 같아야 하고, subvendor/subdevice 는 사용자가
 * PCI_ANY_ID 를 주었으면 무시하며, class 는 class_mask 로 가린 뒤 비교한다.
 * 즉 "장치를 찾는 매칭" 이 아니라 "내가 넣었던 항목을 되찾는 매칭" 이다.
 *
 * retval 의 타입이 size_t 인데 -ENODEV 를 담는다는 점은 상류 코드의
 * 기묘한 부분이다. 부호 없는 타입에 음수를 넣었다가 ssize_t 로 반환하며
 * 다시 음수가 되는 것에 의존한다. 코드는 건드리지 않고 사실만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. dynids.lock 을 잡은 채 목록을 훑고
 * 찾으면 그 자리에서 떼어 내고 kfree 한다.
 *
 * 에러 경로: 위 반환값 설명과 같다. 이미 그 ID 로 바인딩된 장치가 있어도
 * 강제로 언바인딩하지는 않는다 — 목록에서 빠질 뿐이다.
 *
 * 호출 체인:
 *   echo ... > /sys/bus/pci/drivers/<이름>/remove_id → sysfs 저장 핸들러
 *   → [이 함수] → kfree()
 */
static ssize_t remove_id_store(struct device_driver *driver, const char *buf,
			       size_t count)
{
	struct pci_dynid *dynid, *n;	/* [한국어] dynid 는 현재 노드, n 은 다음 노드. 찾으면 그 자리에서 free 하므로 필요하다 */
	struct pci_driver *pdrv = to_pci_driver(driver);	/* [한국어] sysfs 가 준 device_driver 를 pci_driver 로 되돌린다 */
	u32 vendor, device, subvendor = PCI_ANY_ID,	/* [한국어] new_id 와 같은 기본값. 지정하지 않은 서브시스템 ID 는 와일드카드로 둔다 */
		subdevice = PCI_ANY_ID, class = 0, class_mask = 0;	/* [한국어] 클래스 조건도 기본은 없음 */
	int fields;	/* [한국어] sscanf 가 채운 개수 */
	size_t retval = -ENODEV;	/* [한국어] 못 찾았을 때의 반환값을 미리 넣어 둔다. size_t 에 음수를 담는 상류 코드의 기묘한 부분이다 */

	fields = sscanf(buf, "%x %x %x %x %x %x",	/* [한국어] new_id 와 달리 %lx(driver_data) 가 없다 — 지울 때는 식별만 하면 된다 */
			&vendor, &device, &subvendor, &subdevice,	/* [한국어] ID 네 개 */
			&class, &class_mask);	/* [한국어] 클래스 두 개 */
	if (fields < 2)	/* [한국어] vendor 와 device 는 필수 */
		return -EINVAL;	/* [한국어] 모자라면 잘못된 입력 */

	spin_lock(&pdrv->dynids.lock);	/* [한국어] 목록 조작 구간 진입 */
	list_for_each_entry_safe(dynid, n, &pdrv->dynids.list, node) {	/* [한국어] 찾으면 그 자리에서 떼어 내고 free 하므로 _safe 변형 */
		struct pci_device_id *id = &dynid->id;	/* [한국어] 가독성을 위해 현재 항목의 ID 를 지역 포인터로 받는다 */
		if ((id->vendor == vendor) &&	/* [한국어] Vendor 는 정확히 일치해야 한다 */
		    (id->device == device) &&	/* [한국어] Device 도 정확히 일치 */
		    (subvendor == PCI_ANY_ID || id->subvendor == subvendor) &&	/* [한국어] 사용자가 와일드카드를 줬으면 이 필드는 안 본다 */
		    (subdevice == PCI_ANY_ID || id->subdevice == subdevice) &&	/* [한국어] Subsystem Device 도 마찬가지 */
		    !((id->class ^ class) & class_mask)) {	/* [한국어] XOR 후 마스크 — 마스크 비트가 1 인 자리가 모두 같아야 0 이 되어 참이 된다 */
			list_del(&dynid->node);	/* [한국어] 목록에서 떼어 내고 */
			kfree(dynid);	/* [한국어] 해제 */
			retval = count;	/* [한국어] 성공했으니 소비한 바이트 수로 바꾼다 */
			break;	/* [한국어] 하나만 지우고 끝낸다 */
		}
	}
	spin_unlock(&pdrv->dynids.lock);	/* [한국어] 목록 조작 구간 종료 */

	return retval;	/* [한국어] 찾았으면 count, 못 찾았으면 -ENODEV */
}
/* [한국어]
 * static DRIVER_ATTR_WO(remove_id) — 같은 방식으로 driver_attr_remove_id 를
 * 만든다. .store 는 remove_id_store(). new_id 로 넣은 것을 다시 빼는
 * 짝이다.
 */
static DRIVER_ATTR_WO(remove_id);

/* [한국어]
 * pci_drv_attrs / pci_drv_groups - 모든 PCI 드라이버에 붙는 sysfs 속성 목록
 *
 * 위에서 만든 두 속성을 NULL 로 끝나는 배열에 담고, ATTRIBUTE_GROUPS 매크로가
 * 그것을 struct attribute_group 하나짜리 배열 pci_drv_groups 로 감싼다.
 * 그 배열이 pci_bus_type.drv_groups 에 꽂히므로, 어떤 PCI 드라이버가
 * 등록되든 /sys/bus/pci/drivers/<이름>/ 아래에 new_id 와 remove_id 파일이
 * 자동으로 생긴다.
 *
 * 설정자: 컴파일 시점 상수.
 * 읽는 자: 드라이버 코어의 sysfs 생성 코드.
 * 값 범위: 배열 끝은 반드시 NULL 이어야 한다. 매크로가 그것을 전제한다.
 * 동기화: 읽기 전용.
 */
static struct attribute *pci_drv_attrs[] = {
	&driver_attr_new_id.attr,	/* [한국어] ID 추가 파일 */
	&driver_attr_remove_id.attr,	/* [한국어] ID 제거 파일 */
	NULL,				/* [한국어] 배열 끝 표시 */
};
ATTRIBUTE_GROUPS(pci_drv);		/* [한국어] 위 배열을 pci_drv_groups 라는 attribute_group 배열로 포장 */

/* [한국어]
 * struct drv_dev_and_id - probe 호출에 필요한 세 값을 한 덩어리로 묶은 임시 상자
 *
 * pci_call_probe() 는 probe 를 다른 CPU 의 워커에서 돌릴 수 있다. 워커에
 * 넘길 수 있는 것은 포인터 하나뿐이므로, 드라이버·장치·매칭 ID 세 개를
 * 이 구조체에 담아 그 주소만 넘긴다. 스택에 잡히며 pci_call_probe() 가
 * 반환하기 전에 사라진다.
 *
 * 생성/파괴: pci_call_probe() 의 스택 지역 변수. 힙 할당이 아니다.
 * 읽는 자: local_pci_probe() 하나뿐.
 * 동기화: pci_call_probe() 가 flush_work() 로 워커 종료를 기다린 뒤에야
 *   스택 프레임을 버리므로, 수명 경합은 그 flush 로 막혀 있다.
 */
struct drv_dev_and_id {
	struct pci_driver *drv;
	/* [한국어] probe() 를 부를 대상 드라이버.
	 * 설정자: pci_call_probe() 가 초기화 리스트로 채운다.
	 * 읽는 자: local_pci_probe() 가 pci_drv->probe 를 꺼낸다.
	 * 값 범위: NULL 이 아니다. __pci_device_probe() 가 drv->probe 존재를
	 *   확인한 뒤에만 여기까지 온다.
	 * 동기화: 이 구조체를 만든 스레드와 워커 스레드만 본다. */

	struct pci_dev *dev;
	/* [한국어] 바인딩 대상 PCI 장치.
	 * 설정자: pci_call_probe() 가 채운다.
	 * 읽는 자: local_pci_probe() 가 dev->driver 를 세우고 probe 에 넘긴다.
	 * 값 범위: pci_device_probe() 가 pci_dev_get() 으로 참조를 하나 올려
	 *   둔 장치이므로 probe 도중 해제되지 않는다.
	 * 동기화: 위와 같다. */

	const struct pci_device_id *id;
	/* [한국어] 대조에 성공한 ID 항목. 드라이버는 여기 실린 driver_data 로
	 *   모델별 quirk 를 구분하는 것이 관례다.
	 * 설정자: pci_call_probe() 가 __pci_device_probe() 에서 받은 값을 넣는다.
	 * 읽는 자: local_pci_probe() -> drv->probe(dev, id).
	 * 값 범위: 드라이버의 정적 id_table 안의 한 항목이거나, dynids 목록의
	 *   항목이거나, driver_override 경로면 pci_device_id_any 의 주소다.
	 * 동기화: 가리키는 대상은 드라이버 수명 동안 살아 있다. */
};

/* [한국어]
 * local_pci_probe - 드라이버의 probe() 를 실제로 부르는 자리
 *
 * @ddi: 드라이버·장치·매칭 ID 세 값을 묶은 임시 상자.
 * @return: 0 성공, 음수 errno 실패. 드라이버가 양수를 돌려주면 경고를
 *   찍고 0 으로 바꾼다(옛 드라이버 호환).
 *
 * 이 함수 자체는 짧지만 위치가 중요하다. 이 파일 전체가 하는 일의 결론이
 * 이 한 줄 — pci_drv->probe(pci_dev, ddi->id) — 이다.
 *
 * probe 전후로 런타임 PM 을 조율하는 것이 이 함수의 실질적 내용이다.
 * pm_runtime_get_sync() 로 사용 카운트를 올려 두면 probe 가 도는 동안
 * 장치가 저전력 상태로 내려가지 않는다. 바인딩되지 않은 PCI 장치는
 * 런타임 PM 상태와 무관하게 D0 에 두는 것이 이 계층의 규칙이고, probe 는
 * 그 D0 상태에서 시작하도록 보장된다. 런타임 PM 을 쓰려는 드라이버는
 * 자기 probe 안에서 pm_runtime_put_noidle() 로 이 카운트를 내려놓고,
 * remove 에서 pm_runtime_get_noresume() 로 다시 올려 균형을 맞춘다.
 * (원본 영어 주석이 설명하는 그 규약이다.)
 *
 * pci_dev->driver 를 probe 호출 전에 세우는 이유는, probe 안에서 부르는
 * PCI 헬퍼들이 "이 장치의 드라이버" 를 참조할 수 있어야 하기 때문이다.
 * 실패하면 그 자리에서 NULL 로 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. pci_call_probe() 가 직접 부르면
 * 호출자 스레드에서, 워크큐 경로면 대상 NUMA 노드의 CPU 에 묶인 워커
 * 스레드에서 실행된다.
 *
 * 에러 경로: probe 가 음수를 돌려주면 driver 포인터를 되돌리고
 * pm_runtime_put_sync() 로 카운트를 원복한 뒤 그 값을 그대로 올려보낸다.
 *
 * 호출 체인:
 *   pci_call_probe() → [이 함수] → drv->probe()
 *   pci_call_probe() → 워크큐 → local_pci_probe_callback() → [이 함수]
 */
static int local_pci_probe(struct drv_dev_and_id *ddi)
{
	struct pci_dev *pci_dev = ddi->dev;	/* [한국어] 세 값을 지역 변수로 풀어 둔다 */
	struct pci_driver *pci_drv = ddi->drv;	/* [한국어] probe 를 부를 드라이버 */
	struct device *dev = &pci_dev->dev;	/* [한국어] 런타임 PM API 는 일반 struct device 를 받으므로 그 포인터도 미리 꺼내 둔다 */
	int rc;	/* [한국어] probe 반환값 */

	/*
	 * Unbound PCI devices are always put in D0, regardless of
	 * runtime PM status.  During probe, the device is set to
	 * active and the usage count is incremented.  If the driver
	 * supports runtime PM, it should call pm_runtime_put_noidle(),
	 * or any other runtime PM helper function decrementing the usage
	 * count, in its probe routine and pm_runtime_get_noresume() in
	 * its remove routine.
	 */
	pm_runtime_get_sync(dev);	/* [한국어] 사용 카운트를 올리고 필요하면 장치를 깨운다. _sync 라 실제로 깨어날 때까지 기다린다 */
	pci_dev->driver = pci_drv;	/* [한국어] probe 안에서 부르는 PCI 헬퍼들이 이 값을 참조할 수 있도록 미리 세운다 */
	rc = pci_drv->probe(pci_dev, ddi->id);	/* [한국어] 드라이버 코드로 넘어가는 유일한 지점. NVMe 라면 여기가 nvme_probe() 다 */
	if (!rc)	/* [한국어] 0 이면 정상 성공 */
		return rc;	/* [한국어] 런타임 PM 카운트는 올린 채로 둔다 — 바인딩된 동안 유지되어야 한다 */
	if (rc < 0) {	/* [한국어] 음수는 실패 */
		pci_dev->driver = NULL;		/* [한국어] probe 가 실패했으니 소유 표시를 되돌린다 — 실패한 드라이버가 붙어 있는 것처럼 보이면 안 된다 */
		pm_runtime_put_sync(dev);	/* [한국어] get_sync 로 올린 카운트를 내린다. _sync 라 필요하면 이 자리에서 다시 재운다 */
		return rc;	/* [한국어] 실패 값을 그대로 올려보낸다 */
	}
	/*
	 * Probe function should return < 0 for failure, 0 for success
	 * Treat values > 0 as success, but warn.
	 */
	pci_warn(pci_dev, "Driver probe function unexpectedly returned %d\n",	/* [한국어] 양수 반환은 규약 위반이지만 옛 드라이버 호환을 위해 성공으로 처리하고 경고만 남긴다 */
		 rc);	/* [한국어] 실제 반환값을 함께 찍어 어느 드라이버인지 추적할 수 있게 한다 */
	return 0;	/* [한국어] 경고했지만 성공으로 정규화 */
}

/* [한국어]
 * pci_probe_wq - probe 를 대상 NUMA 노드의 CPU 에서 돌리기 위한 전용 워크큐
 *
 * 설정자: pci_driver_init() 이 alloc_workqueue() 로 만든다. 해제하는 곳은
 *   없다(부팅에 성공하면 시스템 수명 내내 살아 있다).
 * 읽는 자: pci_call_probe() 가 queue_work_on() 의 대상으로,
 *   pci_probe_flush_workqueue() 가 flush 대상으로 쓴다.
 * 값 범위: 성공하면 유효한 포인터, 실패하면 pci_driver_init() 이 -ENOMEM 을
 *   반환하므로 NULL 인 채로 쓰이지는 않는다. 그래도 pci_call_probe() 는
 *   WARN_ON_ONCE 로 한 번 더 확인하고 system_percpu_wq 로 대체한다.
 * 동기화: 워크큐 코어가 내부적으로 처리한다. 이 포인터 자체는 초기화 후
 *   변하지 않는다.
 */
static struct workqueue_struct *pci_probe_wq;

/* [한국어]
 * struct pci_probe_arg - probe 를 다른 CPU 의 워크큐에서 돌리기 위한 포장지
 *
 * work_struct 를 통해 넘길 수 있는 것은 그 work_struct 의 주소뿐이므로,
 * container_of() 로 되찾을 수 있도록 인자와 결과를 한 구조체에 함께 둔다.
 * pci_call_probe() 의 스택에 잡히고(INIT_WORK_ONSTACK), flush_work() 로
 * 완료를 기다린 뒤 destroy_work_on_stack() 으로 정리된다.
 *
 * 생성/파괴: pci_call_probe() 의 스택 지역 변수.
 * 읽는 자: local_pci_probe_callback() 이 워커 컨텍스트에서 읽고 쓴다.
 * 동기화: 작성(워커) -> flush_work() -> 판독(호출자) 순서가 워크큐의
 *   완료 보장으로 직렬화되므로 별도 락이 없다.
 */
struct pci_probe_arg {
	struct drv_dev_and_id *ddi;
	/* [한국어] 워커에게 넘길 probe 인자 묶음(스택 상의 drv_dev_and_id).
	 * 설정자: pci_call_probe() 가 지정 초기화로 채운다.
	 * 읽는 자: local_pci_probe_callback() 이 그대로 local_pci_probe() 에 넘긴다.
	 * 값 범위: NULL 이 아니다.
	 * 동기화: 가리키는 대상도 같은 스택 프레임에 있어 flush 전까지 유효하다. */

	struct work_struct work;
	/* [한국어] 워크큐에 실제로 올리는 작업 항목.
	 * 설정자: INIT_WORK_ONSTACK(&arg.work, local_pci_probe_callback).
	 * 읽는 자: 워크큐 코어. 콜백은 이 필드 주소로부터 container_of() 해서
	 *   바깥 pci_probe_arg 를 되찾는다.
	 * 값 범위: 워크큐 코어가 관리하는 불투명 상태. 직접 건드리지 않는다.
	 * 동기화: ONSTACK 변형은 스택 객체임을 lockdep/디버그 객체에 알린다. */

	int ret;
	/* [한국어] drv->probe() 가 돌려준 값을 호출자에게 전달하는 자리.
	 * 설정자: local_pci_probe_callback() (워커 컨텍스트).
	 * 읽는 자: pci_call_probe() 가 flush_work() 이후에 읽는다.
	 * 값 범위: 0 성공, 음수 errno 실패. 양수는 local_pci_probe() 가
	 *   경고를 찍고 0 으로 정규화하므로 여기엔 오지 않는다.
	 * 동기화: flush_work() 가 메모리 순서까지 보장한다. */
};

/* [한국어]
 * local_pci_probe_callback - 워크큐가 부르는 얇은 껍데기
 *
 * @work: 워크큐가 넘겨 준 work_struct. pci_probe_arg 안에 박혀 있다.
 * @return: 없음. 결과는 arg->ret 에 남긴다.
 *
 * 워크큐 콜백의 시그니처는 void (*)(struct work_struct *) 로 고정이라
 * 인자도 반환값도 실을 자리가 없다. 그래서 container_of() 로 바깥
 * pci_probe_arg 를 되찾아 인자를 꺼내고, 결과는 그 구조체의 ret 필드에
 * 써 둔다. 호출자는 flush_work() 로 완료를 기다린 뒤 그 값을 읽는다.
 *
 * 실행 컨텍스트: 워커 스레드. queue_work_on() 으로 지정한 CPU 에서 돈다.
 * 그 CPU 가 곧 대상 장치가 붙은 NUMA 노드에 속하므로, probe 안에서 하는
 * 메모리 할당이 자연스럽게 그 노드의 메모리로 잡힌다.
 *
 * 에러 경로: 없다. probe 의 실패는 값으로 전달될 뿐 여기서 처리하지 않는다.
 *
 * 호출 체인:
 *   pci_call_probe() → queue_work_on() → 워크큐 코어 → [이 함수]
 *   → local_pci_probe()
 */
static void local_pci_probe_callback(struct work_struct *work)
{
	struct pci_probe_arg *arg = container_of(work, struct pci_probe_arg, work);	/* [한국어] work_struct 주소에서 그것을 품고 있는 pci_probe_arg 를 역산한다. 인자를 실을 자리가 없는 워크큐 콜백의 표준 기법이다 */

	arg->ret = local_pci_probe(arg->ddi);	/* [한국어] 실제 probe 를 부르고 결과를 구조체에 적어 둔다. 호출자는 flush_work 뒤에 이 값을 읽는다 */
}

/* [한국어]
 * pci_physfn_is_probed - 이 VF 의 PF 가 지금 probe 중인가
 *
 * @dev: 검사할 장치.
 * @return: 이 장치가 SR-IOV 가상 함수이고 그 물리 함수가 현재 probe 중이면
 *   true, 아니면 false. CONFIG_PCI_IOV 가 꺼져 있으면 항상 false 다.
 *
 * 왜 이런 질문이 필요한가. SR-IOV 는 PF 의 probe 안에서 VF 들을 만들어
 * 등록하는 구조라, PF 의 probe 가 워커에서 돌고 있는 동안 그 안에서
 * 다시 VF 의 probe 가 시작될 수 있다. pci_call_probe() 는 probe 를
 * 워커에 던지고 flush_work() 로 기다리는데, 이 중첩이 일어나면 워커
 * 안에서 다른 워크를 기다리는 모양이 되어 교착의 소지가 있다. 그래서
 * 이 경우에는 워크큐를 거치지 않고 지금 이 스레드에서 바로 부른다.
 *
 * 실행 컨텍스트: pci_call_probe() 안, 프로세스 컨텍스트. 락은 잡지 않는다.
 * physfn->is_probed 는 PF 쪽 pci_call_probe() 가 세우고 지우는 플래그다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_call_probe() → [이 함수]
 */
static bool pci_physfn_is_probed(struct pci_dev *dev)
{
#ifdef CONFIG_PCI_IOV	/* [한국어] SR-IOV 지원 빌드에서만 VF 여부를 따질 수 있다 */
	return dev->is_virtfn && dev->physfn->is_probed;	/* [한국어] 이 장치가 VF 이고, 그 PF 가 지금 probe 중이면 참 */
#else
	return false;	/* [한국어] VF 라는 개념이 없는 빌드에서는 중첩 걱정도 없다 */
#endif
}

/* [한국어]
 * pci_call_probe - probe 를 장치와 같은 NUMA 노드의 CPU 에서 실행한다
 *
 * @drv: 드라이버.  @dev: 장치.  @id: 매칭된 ID.
 * @return: probe 가 돌려준 값 그대로.
 *
 * 왜 CPU 를 골라야 하는가. 드라이버의 probe 는 그 장치가 평생 쓸 자료구조를
 * 할당하는 자리다. 커널의 기본 메모리 할당 정책은 "지금 실행 중인 CPU 가
 * 속한 노드" 에서 잡는 것이므로, 아무 CPU 에서나 probe 를 돌리면 장치가
 * 붙어 있는 노드와 다른 노드의 메모리에 자료구조가 잡힐 수 있다. 그러면
 * 그 뒤로 계속 노드 간 접근이 일어난다. 그래서 probe 만큼은 장치가 붙은
 * 노드의 CPU 로 옮겨 실행한다.
 *
 * 동작 순서.
 *   (1) dev_to_node() 로 장치의 NUMA 노드를 얻고 is_probed 플래그를 세운다.
 *   (2) cpu_hotplug_disable() — 고른 CPU 가 그 사이 빠져 버리면 안 된다.
 *   (3) 노드 번호가 유효하지 않거나(-1 등), 온라인이 아니거나, 이 장치가
 *       probe 중인 PF 의 VF 라면 옮기지 않고 그 자리에서 부른다.
 *   (4) 아니면 그 노드의 CPU 중 housekeeping(격리되지 않은) CPU 를 하나
 *       골라 워크를 던지고 flush_work() 로 기다린다. 고를 CPU 가 없으면
 *       (그 노드가 전부 격리되었다면) 역시 그 자리에서 부른다.
 *   (5) is_probed 를 지우고 cpu_hotplug_enable().
 *
 * RCU 읽기 구간으로 CPU 선택과 큐잉을 함께 묶는 이유는 원본 영어 주석에
 * 적혀 있다 — housekeeping 마스크가 바뀌어 워크큐 풀이 flush 된 뒤에는,
 * 이후의 독자가 반드시 새 마스크에 맞는 CPU 로 큐잉하도록 보장하기 위해서다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. flush_work() 에서 잠든다.
 *
 * 에러 경로: 이 함수 자체가 실패하는 경로는 없다. probe 의 실패는 값으로
 * 그대로 전달된다.
 *
 * 호출 체인:
 *   __pci_device_probe() → [이 함수] → local_pci_probe() 또는
 *   queue_work_on() → local_pci_probe_callback() → local_pci_probe()
 */
static int pci_call_probe(struct pci_driver *drv, struct pci_dev *dev,
			  const struct pci_device_id *id)
{
	int error, node, cpu;	/* [한국어] error 는 결과, node 는 장치의 NUMA 노드, cpu 는 고른 실행 CPU */
	struct drv_dev_and_id ddi = { drv, dev, id };	/* [한국어] 세 값을 한 상자에 담는다. 지정 초기화가 아니라 순서 초기화라 구조체 필드 순서에 의존한다(상류 코드 그대로) */

	/*
	 * Execute driver initialization on node where the device is
	 * attached.  This way the driver likely allocates its local memory
	 * on the right node.
	 */
	node = dev_to_node(&dev->dev);	/* [한국어] 이 장치가 어느 NUMA 노드에 붙어 있는지. 정보가 없으면 음수(NUMA_NO_NODE)가 나온다 */
	dev->is_probed = 1;	/* [한국어] PF 의 probe 가 도는 동안 그 VF 들이 이 표시를 보고 중첩을 피한다 */

	cpu_hotplug_disable();	/* [한국어] CPU 를 고르고 그 CPU 에 워크를 던지는 동안 그 CPU 가 빠지면 안 되므로 핫플러그를 막는다 */
	/*
	 * Prevent nesting work_on_cpu() for the case where a Virtual Function
	 * device is probed from work_on_cpu() of the Physical device.
	 */
	if (node < 0 || node >= MAX_NUMNODES || !node_online(node) ||	/* [한국어] 노드 번호가 없거나 범위 밖이거나 오프라인이면 옮길 곳이 없다 */
	    pci_physfn_is_probed(dev)) {	/* [한국어] 또는 PF 가 probe 중인 VF 라면(워커 안에서 워커를 기다리는 중첩을 피한다) */
		error = local_pci_probe(&ddi);	/* [한국어] 그냥 이 스레드에서 바로 부른다 */
	} else {	/* [한국어] 그 밖의 경우는 워크큐로 옮긴다 */
		struct pci_probe_arg arg = { .ddi = &ddi };	/* [한국어] 스택에 워크 인자를 잡는다 */

		INIT_WORK_ONSTACK(&arg.work, local_pci_probe_callback);	/* [한국어] ONSTACK 변형 — 스택 위의 work_struct 임을 디버그 객체 추적기에 알린다 */
		/*
		 * The target election and the enqueue of the work must be within
		 * the same RCU read side section so that when the workqueue pool
		 * is flushed after a housekeeping cpumask update, further readers
		 * are guaranteed to queue the probing work to the appropriate
		 * targets.
		 */
		rcu_read_lock();	/* [한국어] CPU 선택과 큐잉을 한 RCU 읽기 구간에 묶는다(원본 주석의 이유 참조) */
		cpu = cpumask_any_and(cpumask_of_node(node),	/* [한국어] 그 노드에 속하면서 */
				      housekeeping_cpumask(HK_TYPE_DOMAIN));	/* [한국어] 격리되지 않은(housekeeping) CPU 를 하나 고른다. isolcpus 로 격리한 CPU 에 커널 워크를 던지면 격리가 깨진다 */

		if (cpu < nr_cpu_ids) {	/* [한국어] 유효한 CPU 를 찾았다면 */
			struct workqueue_struct *wq = pci_probe_wq;	/* [한국어] 전용 워크큐를 쓴다 */

			if (WARN_ON_ONCE(!wq))	/* [한국어] 초기화 순서 문제로 아직 없다면(있어서는 안 되는 상황) */
				wq = system_percpu_wq;	/* [한국어] 시스템 기본 per-CPU 워크큐로 대체해 진행은 시킨다 */
			queue_work_on(cpu, wq, &arg.work);	/* [한국어] 고른 CPU 에 워크를 올린다 */
			rcu_read_unlock();	/* [한국어] 큐잉까지 끝났으니 RCU 읽기 구간 종료 */
			flush_work(&arg.work);	/* [한국어] 그 워크가 끝날 때까지 잠들며 기다린다 */
			error = arg.ret;	/* [한국어] 워커가 적어 둔 결과를 읽는다 */
		} else {	/* [한국어] 그 노드에 쓸 수 있는 CPU 가 하나도 없으면 */
			rcu_read_unlock();	/* [한국어] RCU 구간을 닫고 */
			error = local_pci_probe(&ddi);	/* [한국어] 그냥 이 스레드에서 부른다 */
		}

		destroy_work_on_stack(&arg.work);	/* [한국어] 스택 위 work_struct 의 디버그 추적을 해제한다. 이것을 빼먹으면 스택이 사라진 뒤 추적기가 오탐한다 */
	}

	dev->is_probed = 0;	/* [한국어] 중첩 방지 표시 해제 */
	cpu_hotplug_enable();	/* [한국어] CPU 핫플러그 재허용 */
	return error;	/* [한국어] probe 결과를 그대로 올려보낸다 */
}

/* [한국어]
 * pci_probe_flush_workqueue - probe 워크큐에 밀린 작업이 끝나기를 기다린다
 *
 * @return: 없음. flush_workqueue() 가 반환하면 큐가 비었다는 뜻이다.
 *
 * pci_call_probe() 는 각 probe 를 던진 뒤 자기 것만 flush_work() 로
 * 기다린다. 이 함수는 큐 전체를 기다리는 더 센 버전으로, "지금까지 예약된
 * 모든 probe 가 끝났음" 을 보장해야 하는 쪽이 쓴다.
 *
 * 다만 이 스파스 체크아웃 안에서는 이 함수를 부르는 곳을 찾지 못했다.
 * drivers/pci/pci.h 에도 선언이 없으므로 include/linux/pci.h 에 선언되어
 * drivers/pci 밖(드라이버 코어나 CPU 격리 관련 코드로 추정)에서 불릴
 * 것으로 보이나, 그 파일들이 이 트리에 없어 확인하지 못했다. static 이
 * 아니고 EXPORT 도 없다는 사실만 확실하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. flush_workqueue() 에서 잠든다.
 * 워커 안에서 부르면 자기 자신을 기다리게 되므로 불러서는 안 된다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   (이 트리에서 호출자 미확인) → [이 함수] → flush_workqueue()
 */
void pci_probe_flush_workqueue(void)
{
	flush_workqueue(pci_probe_wq);	/* [한국어] 큐 전체가 빌 때까지 잠들며 기다린다 */
}

/**
 * __pci_device_probe - check if a driver wants to claim a specific PCI device
 * @drv: driver to call to check if it wants the PCI device
 * @pci_dev: PCI device being probed
 *
 * returns 0 on success, else error.
 * side-effect: pci_dev->driver is set to drv when drv claims pci_dev.
 */
/* [한국어]
 * __pci_device_probe - 드라이버가 이 장치를 맡겠다고 하는지 확인하고 probe 로 넘긴다
 *
 * @drv: 드라이버.  @pci_dev: 장치.
 * @return: 0 성공. drv->probe 가 아예 없으면 0 을 그대로 돌려주고,
 *   probe 는 있는데 ID 대조에 실패하면 -ENODEV, probe 가 실패하면 그 값.
 *
 * 부수 효과가 하나 있다(원본 주석이 밝히는 대로) — 드라이버가 장치를
 * 맡으면 local_pci_probe() 안에서 pci_dev->driver 가 그 드라이버로 세워진다.
 *
 * error 를 먼저 -ENODEV 로 놓고 시작하는 구조에 주의. drv->probe 가 없으면
 * 그 초기화조차 하지 않아 0 이 반환된다. 즉 "probe 콜백이 없는 드라이버"
 * 는 성공으로 취급된다 — 실제로 pci_compat_driver 처럼 콜백 없는 껍데기
 * 드라이버가 존재하기 때문이다.
 *
 * pci_bus_match() 가 이미 매칭을 확인했는데 여기서 pci_match_device() 를
 * 다시 부르는 이유는, match 는 참·거짓만 돌려주고 버려지기 때문이다.
 * probe 에는 "어느 ID 항목으로 맞았는지" 를 넘겨야 하므로 한 번 더 찾는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 락 없음.
 *
 * 에러 경로: 위 반환값 설명 그대로. 뒷정리는 호출자 pci_device_probe() 가 한다.
 *
 * 호출 체인:
 *   pci_device_probe() → [이 함수] → pci_match_device(), pci_call_probe()
 */
static int __pci_device_probe(struct pci_driver *drv, struct pci_dev *pci_dev)
{
	const struct pci_device_id *id;	/* [한국어] 대조에 성공한 ID 를 받을 자리 */
	int error = 0;	/* [한국어] probe 콜백이 없는 드라이버는 이 초기값 0 이 그대로 반환된다 — 성공으로 취급된다 */

	if (drv->probe) {	/* [한국어] probe 콜백이 있는 드라이버만 아래를 진행한다 */
		error = -ENODEV;	/* [한국어] 일단 실패로 놓는다. 아래에서 ID 를 못 찾으면 이 값이 그대로 나간다 */

		id = pci_match_device(drv, pci_dev);	/* [한국어] match 단계에서 버려진 ID 를 다시 찾는다. probe 에 넘겨야 하기 때문이다 */
		if (id)	/* [한국어] 찾았으면 */
			error = pci_call_probe(drv, pci_dev, id);	/* [한국어] 적절한 CPU 를 골라 probe 를 실행한다 */
	}
	return error;	/* [한국어] 0, -ENODEV, 또는 probe 가 돌려준 값 */
}

#ifdef CONFIG_PCI_IOV
/* [한국어]
 * pci_device_can_probe - (SR-IOV 지원 빌드) 이 장치를 바인딩해도 되는가
 *
 * @pdev: 검사할 장치.
 * @return: 바인딩을 허용하면 true.
 *
 * 물리 함수(PF)는 언제나 true 다. 가상 함수(VF)만 조건이 붙는데, 그 조건이
 * 두 가지다.
 *   - PF 의 sriov->drivers_autoprobe 가 켜져 있는가. 이 값은
 *     /sys/bus/pci/devices/<PF 주소>/sriov_drivers_autoprobe 로 사용자가
 *     끌 수 있다. 끄는 이유는 명확하다 — VF 를 만들자마자 호스트 드라이버가
 *     낚아채면 그것을 가상 머신에 넘길 수 없기 때문이다. 그래서 VF 를
 *     패스스루로 쓰려는 사람은 이 값을 먼저 0 으로 만들고 VF 를 만든다.
 *   - 또는 이 VF 에 driver_override 가 걸려 있는가. 사용자가 특정
 *     드라이버(대개 vfio-pci)를 콕 집어 지정했다면 autoprobe 설정과
 *     무관하게 그 드라이버에는 붙여 준다.
 *
 * 실행 컨텍스트: pci_device_probe() 의 첫 줄. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. false 를 돌려주면 호출자가 -ENODEV 로 물러난다.
 *
 * 호출 체인:
 *   pci_device_probe() → [이 함수]
 */
static inline bool pci_device_can_probe(struct pci_dev *pdev)
{
	return (!pdev->is_virtfn || pdev->physfn->sriov->drivers_autoprobe ||	/* [한국어] PF 는 무조건 허용. VF 는 PF 의 sriov_drivers_autoprobe 가 켜져 있을 때만 */
		device_has_driver_override(&pdev->dev));	/* [한국어] 또는 이 VF 에 driver_override 가 걸려 있으면 허용(대개 vfio-pci 로 넘기려는 경우) */
}
#else
/* [한국어]
 * pci_device_can_probe - (SR-IOV 미지원 빌드) 항상 허용
 *
 * @pdev: 쓰이지 않는다.  @return: 항상 true.
 *
 * CONFIG_PCI_IOV 가 꺼져 있으면 가상 함수라는 개념 자체가 없으므로
 * 막을 이유도 없다. 호출부에 #ifdef 를 흩뿌리지 않으려고 같은 이름의
 * 빈 판정을 두는, 커널에서 흔한 방식이다.
 *
 * 실행 컨텍스트: 위와 같다. inline 이라 컴파일러가 통째로 지운다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_device_probe() → [이 함수]
 */
static inline bool pci_device_can_probe(struct pci_dev *pdev)
{
	return true;
}
#endif

/* [한국어]
 * pci_device_probe - pci_bus_type.probe 슬롯. 바인딩의 PCI 쪽 진입점
 *
 * @dev: 일반 struct device. 실제로는 struct pci_dev 안에 박힌 것이다.
 * @return: 0 성공, 음수 errno 실패. 드라이버 코어는 실패를 보면 이 장치와
 *   드라이버의 바인딩을 취소한다.
 *
 * 드라이버 코어가 match 로 짝을 확인한 뒤 부르는 함수다. 여기서 하는 일은
 * probe 를 부르기 전에 PCI 쪽에서 갖춰 줘야 할 것들을 갖추는 것이다.
 *
 *   (1) pci_device_can_probe() — SR-IOV VF 인데 PF 가 자동 바인딩을 꺼
 *       두었다면 여기서 -ENODEV 로 물러난다.
 *   (2) pci_assign_irq() — 이 장치의 레거시 INTx 인터럽트가 어느 IRQ 선에
 *       연결되는지를 확정한다(ACPI _PRT 나 디바이스 트리를 참조). 드라이버가
 *       MSI/MSI-X 를 쓰더라도, pci_dev->irq 는 이 시점에 채워져 있어야 한다.
 *   (3) pcibios_alloc_irq() — 아키텍처별 추가 처리. 대부분의 플랫폼에서는
 *       아무것도 하지 않는 __weak 빈 함수다(drivers/pci/irq.c:545).
 *   (4) pci_dev_get() — 드라이버가 물고 있는 동안 장치 구조체가 사라지지
 *       않게 참조를 하나 올린다. 짝은 pci_device_remove() 에 있다.
 *   (5) __pci_device_probe() 로 내려간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 드라이버 코어가 device_lock 을 잡은
 * 상태에서 부른다. PROBE_PREFER_ASYNCHRONOUS 드라이버라면 부팅 스레드가
 * 아니라 비동기 probe 스레드에서 불릴 수 있다.
 *
 * 에러 경로: pcibios_alloc_irq 실패는 바로 반환. probe 실패면 잡아 둔 IRQ
 * (pcibios_free_irq)와 참조(pci_dev_put)를 되돌린 뒤 그 값을 올려보낸다.
 * DMA/IOMMU 설정은 이 함수가 하지 않으므로 여기서 되돌릴 것도 없다 —
 * 그쪽은 드라이버 코어가 pci_bus_type.dma_configure/dma_cleanup 으로 짝을 맞춘다.
 *
 * 호출 체인:
 *   드라이버 코어(really_probe) → [이 함수] → __pci_device_probe()
 *   → pci_call_probe() → local_pci_probe() → drv->probe()
 */
static int pci_device_probe(struct device *dev)
{
	int error;	/* [한국어] probe 결과 */
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 감싸고 있는 pci_dev 를 얻는다 */
	struct pci_driver *drv = to_pci_driver(dev->driver);	/* [한국어] 마찬가지로 device_driver 에서 pci_driver 를 얻는다 */

	if (!pci_device_can_probe(pci_dev))	/* [한국어] VF 자동 바인딩이 꺼져 있는 경우를 먼저 걸러낸다 */
		return -ENODEV;	/* [한국어] 붙이지 않는다. 드라이버 코어는 이 값을 보고 조용히 넘어간다 */

	pci_assign_irq(pci_dev);	/* [한국어] 레거시 INTx 인터럽트가 어느 IRQ 선으로 오는지 확정한다(ACPI _PRT 나 디바이스 트리 참조). MSI 를 쓸 드라이버라도 pci_dev->irq 는 채워져 있어야 한다 */

	error = pcibios_alloc_irq(pci_dev);	/* [한국어] 아키텍처별 추가 IRQ 처리. 대부분의 플랫폼에서는 아무것도 하지 않는 __weak 빈 함수다(drivers/pci/irq.c:545) */
	if (error < 0)	/* [한국어] 음수만 실패로 본다 */
		return error;	/* [한국어] 참조를 올리기 전이므로 되돌릴 것이 없다 */

	pci_dev_get(pci_dev);		/* [한국어] 드라이버가 물고 있는 동안 struct pci_dev 가 free 되지 않도록 참조를 하나 올린다. 짝은 pci_device_remove() 의 pci_dev_put() */
	error = __pci_device_probe(drv, pci_dev);	/* [한국어] 실제 probe 로 내려간다 */
	if (error) {	/* [한국어] 실패했다면 */
		pcibios_free_irq(pci_dev);	/* [한국어] IRQ 를 반납하고 */
		pci_dev_put(pci_dev);	/* [한국어] 참조를 내린다. 이 순서는 pci_device_remove() 의 역순과 같다 */
	}

	return error;	/* [한국어] 0 또는 실패 값 */
}

/* [한국어]
 * pci_device_remove - pci_bus_type.remove 슬롯. 언바인딩의 PCI 쪽 처리
 *
 * @dev: 대상 장치.  @return: 없음. 언바인딩은 실패할 수 없다.
 *
 * rmmod, sysfs unbind, 핫플러그 제거 어느 쪽이든 결국 여기로 온다.
 * 순서가 중요하다.
 *
 *   (1) drv->remove 가 있으면 pm_runtime_get_sync() 로 장치를 깨우고
 *       사용 카운트를 올린다. remove 는 장치 레지스터를 만질 수 있어야
 *       하므로 잠들어 있으면 안 된다.
 *   (2) pm_runtime_barrier() — 드라이버가 .runtime_idle 콜백을 가지고 있고
 *       그것이 이미 돌기 시작했다면 아래 코드와 나란히 실행될 수 있다.
 *       원본 주석대로, 그 런타임 PM 활동이 다 끝날 때까지 기다린다.
 *   (3) drv->remove() — 드라이버가 자기 자원을 반납한다.
 *   (4) pm_runtime_put_noidle() — (1)에서 올린 카운트를 내린다. _noidle
 *       변형이라 이 시점에 유휴 판정을 촉발하지는 않는다.
 *   (5) IRQ 반납, driver 포인터 지우기, SR-IOV VF 정리.
 *   (6) pm_runtime_put_sync() — local_pci_probe() 가 올려 둔 카운트의 짝.
 *   (7) 장치가 아직 D0 이면 current_state 를 PCI_UNKNOWN 으로 바꾼다.
 *       다음에 드라이버를 다시 올릴 때까지 무슨 일이 있을지 모르므로,
 *       알고 있다고 착각하느니 모른다고 표시해 두는 편이 안전하다.
 *   (8) pci_dev_put() — pci_device_probe() 가 올린 참조를 내린다.
 *
 * 원본에는 "pci_disable_device() 를 안 부른 드라이버에게 잔소리하고 싶지만
 * 그러면 이상한 BIOS·브리지 조합에서 문제가 생겨서 참는다" 는 취지의
 * 유명한 넋두리 주석이 있다. 코드가 아니라 현실에 대한 기록이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. device_lock 을 잡은 상태.
 *
 * 에러 경로: 없다. 반환값이 void 다.
 *
 * 호출 체인:
 *   드라이버 코어(device_release_driver) → [이 함수] → drv->remove(),
 *   pcibios_free_irq(), pci_iov_remove(), pci_dev_put()
 */
static void pci_device_remove(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	struct pci_driver *drv = pci_dev->driver;	/* [한국어] 바인딩 시 세워 둔 드라이버 포인터. 아래에서 NULL 로 지운다 */

	if (drv->remove) {	/* [한국어] remove 콜백이 있는 드라이버만 */
		pm_runtime_get_sync(dev);	/* [한국어] 장치를 깨우고 사용 카운트를 올린다 — remove 는 레지스터를 만질 수 있어야 한다 */
		/*
		 * If the driver provides a .runtime_idle() callback and it has
		 * started to run already, it may continue to run in parallel
		 * with the code below, so wait until all of the runtime PM
		 * activity has completed.
		 */
		pm_runtime_barrier(dev);	/* [한국어] 런타임 PM 활동(특히 이미 돌기 시작한 runtime_idle)이 끝나기를 기다린다 */
		drv->remove(pci_dev);		/* [한국어] 드라이버의 정리 콜백. 여기서 자기 자원(IRQ, 매핑, 큐)을 반납한다 */
		pm_runtime_put_noidle(dev);	/* [한국어] 1230 에서 올린 카운트를 내린다. _noidle 이라 여기서 유휴 판정을 촉발하지 않는다 */
	}
	pcibios_free_irq(pci_dev);		/* [한국어] pci_device_probe() 의 pcibios_alloc_irq() 짝 */
	pci_dev->driver = NULL;			/* [한국어] 소유 표시 해제. 이후 pci_dev_driver() 는 이 장치를 임자 없음으로 본다 */
	pci_iov_remove(pci_dev);	/* [한국어] 이 장치가 만들어 둔 SR-IOV 가상 함수들을 정리한다 */

	/* Undo the runtime PM settings in local_pci_probe() */
	pm_runtime_put_sync(dev);	/* [한국어] local_pci_probe() 가 올려 둔 카운트를 내린다. 이 시점부터 장치는 다시 잠들 수 있다 */

	/*
	 * If the device is still on, set the power state as "unknown",
	 * since it might change by the next time we load the driver.
	 */
	if (pci_dev->current_state == PCI_D0)	/* [한국어] 아직 켜져 있다면 */
		pci_dev->current_state = PCI_UNKNOWN;	/* [한국어] 다음에 드라이버를 올릴 때까지 무슨 일이 있을지 모르므로 안다고 가정하지 않는다 */

	/*
	 * We would love to complain here if pci_dev->is_enabled is set, that
	 * the driver should have called pci_disable_device(), but the
	 * unfortunate fact is there are too many odd BIOS and bridge setups
	 * that don't like drivers doing that all of the time.
	 * Oh well, we can dream of sane hardware when we sleep, no matter how
	 * horrible the crap we have to deal with is when we are awake...
	 */

	pci_dev_put(pci_dev);	/* [한국어] pci_device_probe() 가 올린 참조를 내린다. 마지막 참조였다면 여기서 pci_dev 가 해제된다 */
}

/* [한국어]
 * pci_device_shutdown - pci_bus_type.shutdown 슬롯. 재부팅·종료 직전 처리
 *
 * @dev: 대상 장치.  @return: 없음.
 *
 * remove 와 다른 점은 "자원을 반납하는 것" 이 목적이 아니라는 것이다.
 * 곧 전원이 나가거나 다른 커널이 올라오므로, 장치가 더 이상 시스템 메모리를
 * 건드리지 않게 만드는 것이 목적이다.
 *
 *   (1) pm_runtime_resume() — 잠들어 있으면 깨운다. 잠든 장치에는
 *       shutdown 콜백도 부를 수 없고 config 레지스터도 못 쓴다.
 *   (2) drv->shutdown() — 드라이버가 자기 방식으로 장치를 정지시킨다.
 *   (3) kexec 인 경우에만 pci_clear_master() 로 Bus Master 비트를 끈다.
 *
 * (3)의 조건이 왜 kexec 뿐인가. 보통의 재부팅이라면 펌웨어가 리셋으로
 * 모든 장치를 강제로 멈춰 준다. 그러나 kexec 은 펌웨어를 거치지 않고
 * 곧바로 다음 커널로 뛰므로, 장치가 진행 중이던 DMA 가 살아남아 새 커널의
 * 메모리를 덮어쓸 수 있다. Bus Master 비트(PCI COMMAND 레지스터의 bit 2)를
 * 끄면 그 장치는 더 이상 버스의 주인이 되어 메모리에 쓰기를 낼 수 없다.
 *
 * current_state <= PCI_D3hot 조건은 "D3cold 이거나 상태를 모르는 장치는
 * 건드리지 말라" 는 뜻이다. 그런 장치는 config 접근 자체가 안 되거나
 * 엉뚱한 값을 읽어 오히려 문제를 만든다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 종료 경로. 이미 대부분의 서비스가
 * 내려간 뒤라 잠들 수는 있지만 오래 걸리면 안 된다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   커널 종료(device_shutdown) → [이 함수] → drv->shutdown(),
 *   pci_clear_master()
 */
static void pci_device_shutdown(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	struct pci_driver *drv = pci_dev->driver;	/* [한국어] 드라이버가 없을 수도 있다 — 아래에서 NULL 검사를 한다 */

	pm_runtime_resume(dev);	/* [한국어] 잠들어 있으면 깨운다. 잠든 장치에는 shutdown 콜백도 config 접근도 할 수 없다 */

	if (drv && drv->shutdown)	/* [한국어] 드라이버가 붙어 있고 shutdown 콜백을 주었다면 */
		drv->shutdown(pci_dev);	/* [한국어] 드라이버 방식으로 장치를 정지시킨다 */

	/*
	 * If this is a kexec reboot, turn off Bus Master bit on the
	 * device to tell it to not continue to do DMA. Don't touch
	 * devices in D3cold or unknown states.
	 * If it is not a kexec reboot, firmware will hit the PCI
	 * devices with big hammer and stop their DMA any way.
	 */
	if (kexec_in_progress && (pci_dev->current_state <= PCI_D3hot))	/* [한국어] kexec 이고, 상태를 알며 D3cold 가 아닌 장치만 */
		pci_clear_master(pci_dev);	/* [한국어] PCI COMMAND 레지스터의 Bus Master 비트를 끈다. 이 비트가 0 이면 장치가 버스의 주인이 되어 메모리에 쓰기를 낼 수 없다 */
}

#ifdef CONFIG_PM_SLEEP	/* [한국어] 이하는 시스템 절전(S3/S4)을 빌드할 때만 컴파일되는 구간 */

/* Auxiliary functions used for system resume */

/**
 * pci_restore_standard_config - restore standard config registers of PCI device
 * @pci_dev: PCI device to handle
 */
/* [한국어]
 * pci_restore_standard_config - 장치를 D0 로 올리고 저장해 둔 config 를 되돌린다
 *
 * @pci_dev: 대상 장치.
 * @return: 0 성공, pci_set_power_state() 가 실패하면 그 errno.
 *
 * 이름의 "standard config" 는 PCI 설정 공간의 표준 헤더 영역(BAR, Command,
 * Cache Line Size 등)과 커널이 함께 저장해 두는 확장 상태(MSI/MSI-X 설정,
 * PCIe capability 등)를 뜻한다.
 *
 * 순서에 이유가 있다. (1) pci_update_current_state(pci_dev, PCI_UNKNOWN) 로
 * 하드웨어에서 현재 전원 상태를 다시 읽어 온다 — 절전 중에 펌웨어가 상태를
 * 바꿔 놓았을 수 있으므로 커널이 기억하던 값을 믿지 않는다. (2) D0 가
 * 아니면 D0 로 올린다. config 쓰기는 D0 에서만 온전히 동작한다.
 * (3) pci_restore_state() 로 저장분을 되쓴다. (4) pci_pme_restore() 로
 * PME(웨이크업 신호) 설정을 원래대로 돌린다.
 *
 * 이 함수는 정상 복귀 경로가 아니라 오류 처리 경로에서 주로 불린다 —
 * pci_pm_resume() 과 pci_pm_restore() 가 "state_saved 가 아직 참이다" 를
 * 보고 부른다. 그 플래그가 참이라는 것은 저장은 했는데 복원 단계를 거치지
 * 않고 이리로 왔다는 뜻, 즉 suspend 도중 실패해 되감기는 중이라는 뜻이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. CONFIG_PM_SLEEP 에서만 컴파일된다.
 *
 * 에러 경로: D0 전환 실패면 복원을 시도하지 않고 바로 반환한다.
 *
 * 호출 체인:
 *   pci_pm_resume() / pci_pm_restore() → [이 함수] → pci_set_power_state(),
 *   pci_restore_state(), pci_pme_restore()
 */
static int pci_restore_standard_config(struct pci_dev *pci_dev)
{
	pci_update_current_state(pci_dev, PCI_UNKNOWN);	/* [한국어] 하드웨어에서 전원 상태를 다시 읽어 온다. PCI_UNKNOWN 을 넘긴다는 것은 "커널이 기억하던 값을 믿지 말고 다시 확인하라" 는 뜻이다 */

	if (pci_dev->current_state != PCI_D0) {	/* [한국어] D0 가 아니면 config 쓰기가 온전히 동작하지 않는다 */
		int error = pci_set_power_state(pci_dev, PCI_D0);	/* [한국어] D0 로 올린다 */
		if (error)	/* [한국어] 전원 전환 실패면 */
			return error;	/* [한국어] 복원을 시도하지 않고 그대로 실패를 올려보낸다 */
	}

	pci_restore_state(pci_dev);	/* [한국어] 저장해 둔 표준 헤더(BAR, COMMAND 등)와 확장 상태(MSI/MSI-X, PCIe capability)를 되쓴다 */
	pci_pme_restore(pci_dev);	/* [한국어] PME(웨이크업 신호) 설정을 원래대로 되돌린다 */
	return 0;	/* [한국어] 성공 */
}
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_PM	/* [한국어] 이하는 시스템 절전과 런타임 절전 양쪽에서 쓰는 헬퍼 구간(CONFIG_PM) */

/* Auxiliary functions used for system resume and run-time resume */

/* [한국어]
 * pci_pm_default_resume - 복귀 뒷마무리 중 전원 상태와 무관한 부분
 *
 * @pci_dev: 대상 장치.  @return: 없음.
 *
 * 두 줄뿐이지만 둘 다 이유가 있다.
 *
 * pci_fixup_device(pci_fixup_resume, ...) 는 이 장치에 걸린 resume quirk 를
 * 실행한다. 커널에는 "특정 칩은 복귀 후 이 레지스터를 다시 써 줘야 한다"
 * 같은 하드웨어별 보정이 DECLARE_PCI_FIXUP_RESUME 매크로로 등록되어 있고,
 * 이 호출이 그중 해당하는 것들을 찾아 돌린다.
 *
 * pci_enable_wake(pci_dev, PCI_D0, false) 는 웨이크업 신호를 끈다. 이미
 * 깨어난 뒤이므로 PME 를 계속 켜 둘 이유가 없고, 켜 둔 채로 두면 엉뚱한
 * 시점에 시스템을 깨울 수 있다.
 *
 * 이름에 default 가 붙었지만 "드라이버가 콜백을 안 줬을 때만" 이라는 뜻은
 * 아니다. pci_pm_resume() 과 pci_pm_runtime_resume() 이 드라이버 콜백
 * 유무와 무관하게 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. CONFIG_PM 에서 컴파일된다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_pm_resume() / pci_pm_restore() / pci_pm_runtime_resume()
 *   → [이 함수] → pci_fixup_device(), pci_enable_wake()
 */
static void pci_pm_default_resume(struct pci_dev *pci_dev)
{
	pci_fixup_device(pci_fixup_resume, pci_dev);	/* [한국어] DECLARE_PCI_FIXUP_RESUME 로 등록된 하드웨어별 복귀 보정을 실행한다 */
	pci_enable_wake(pci_dev, PCI_D0, false);	/* [한국어] 이미 깨어났으므로 웨이크업 신호를 끈다. 켜 둔 채로 두면 엉뚱한 시점에 시스템을 깨울 수 있다 */
}

/* [한국어]
 * pci_pm_default_resume_early - 복귀 뒷마무리 중 전원과 config 를 되살리는 부분
 *
 * @pci_dev: 대상 장치.  @return: 없음.
 *
 * 앞의 pci_pm_default_resume() 과 이름이 비슷하지만 하는 일이 다르다.
 * 이쪽은 "장치를 다시 접근 가능한 상태로 만드는" 일이고, 그래서 더 이른
 * 단계(_noirq)에서 불린다.
 *
 *   (1) pci_pm_power_up_and_verify_state() — D0 로 올리고, 정말 올라왔는지
 *       확인한다. 확인이 필요한 이유는 장치가 링크에서 빠졌거나
 *       전원 자원이 살아나지 않았을 수 있기 때문이다.
 *   (2) pci_restore_state() — 저장해 둔 config 를 되쓴다. BAR 주소가
 *       여기서 돌아와야 그 뒤의 MMIO 접근이 성립한다. MSI-X 테이블처럼
 *       MMIO 공간에 있는 상태도 이 안에서 복원되며, 그래서 (1)이 먼저다.
 *   (3) pci_pme_restore() — PME 설정 복원.
 *
 * 실행 컨텍스트: 대부분 _noirq 단계에서 불리므로 인터럽트가 꺼져 있다.
 * 잠들면 안 된다.
 *
 * 에러 경로: 없다. (1)이 실패해도 반환값이 없어 상위로 알리지 않는다 —
 * 후속 접근이 모두 0xffffffff 를 읽으며 실패하는 것으로 드러난다.
 *
 * 호출 체인:
 *   pci_pm_resume_noirq() / pci_pm_restore_noirq() / pci_pm_runtime_resume()
 *   → [이 함수] → pci_pm_power_up_and_verify_state(), pci_restore_state()
 */
static void pci_pm_default_resume_early(struct pci_dev *pci_dev)
{
	pci_pm_power_up_and_verify_state(pci_dev);	/* [한국어] D0 로 올리고 정말 올라왔는지 확인한다. 링크가 죽었거나 전원 자원이 살아나지 않았을 수 있다 */
	pci_restore_state(pci_dev);			/* [한국어] config 복원. MSI-X 테이블은 MMIO 공간에 있으므로 반드시 D0 가 된 뒤여야 한다 */
	pci_pme_restore(pci_dev);	/* [한국어] PME 설정 복원 */
}

/* [한국어]
 * pci_pm_bridge_power_up_actions - 브리지가 깨어난 뒤 그 아래 계층을 수습한다
 *
 * @pci_dev: 방금 D3cold 에서 올라온 브리지.  @return: 없음.
 *
 * 브리지가 D3cold 에 있었다는 것은 그 아래 링크가 통째로 죽어 있었다는
 * 뜻이다. 전원을 올렸다고 링크가 즉시 서지는 않는다.
 *
 *   (1) pci_bridge_wait_for_secondary_bus() — 하위 링크가 올라오기를
 *       기다린다. PCIe 규격이 정한 대기 시간과 링크 학습 완료 확인이
 *       그 안에 들어 있다.
 *   (2) 링크가 끝내 서지 않으면, 그 아래 장치들을 전부 "연결 끊김" 으로
 *       표시한다(pci_walk_bus + pci_dev_set_disconnected). 그래야 PM
 *       코어가 없는 장치를 깨우려다 오래 매달리지 않는다.
 *   (3) 링크가 섰으면 pci_resume_bus() 로 아래 계층을 깨운다. 원본 주석의
 *       설명대로, 브리지를 D3cold 에서 올리면 그 아래 전체가
 *       D0uninitialized 라는 어중간한 상태로 함께 올라오기 때문에, 일단
 *       제대로 깨워서 각자 다시 잠들 기회를 주는 편이 낫다.
 *
 * 실행 컨텍스트: _noirq 단계 또는 런타임 복귀. 링크 대기가 있으므로
 * 짧지 않은 시간이 걸릴 수 있다.
 *
 * 에러 경로: (2)가 그 처리다. 반환값으로 실패를 알리지는 않는다.
 *
 * 호출 체인:
 *   pci_pm_resume_noirq() / pci_pm_runtime_resume()
 *   → [이 함수] → pci_bridge_wait_for_secondary_bus(), pci_resume_bus()
 */
static void pci_pm_bridge_power_up_actions(struct pci_dev *pci_dev)
{
	int ret;	/* [한국어] 하위 링크 대기 결과 */

	ret = pci_bridge_wait_for_secondary_bus(pci_dev, "resume");	/* [한국어] PCIe 규격이 정한 시간만큼 기다리며 하위 링크가 서는지 본다. "resume" 은 로그에 찍힐 문맥 문자열이다 */
	if (ret) {	/* [한국어] 끝내 서지 않았다면 */
		/*
		 * The downstream link failed to come up, so mark the
		 * devices below as disconnected to make sure we don't
		 * attempt to resume them.
		 */
		pci_walk_bus(pci_dev->subordinate, pci_dev_set_disconnected,	/* [한국어] 그 아래 모든 장치를 순회하며 */
			     NULL);	/* [한국어] 연결 끊김으로 표시한다. 그래야 PM 코어가 없는 장치를 깨우려다 오래 매달리지 않는다 */
		return;	/* [한국어] 더 할 일이 없다 */
	}

	/*
	 * When powering on a bridge from D3cold, the whole hierarchy may be
	 * powered on into D0uninitialized state, resume them to give them a
	 * chance to suspend again
	 */
	pci_resume_bus(pci_dev->subordinate);	/* [한국어] 링크가 섰으니 하위 계층을 깨운다. 원본 주석대로 D0uninitialized 로 함께 올라온 장치들에 다시 잠들 기회를 주기 위해서다 */
}

#endif /* CONFIG_PM */

#ifdef CONFIG_PM_SLEEP	/* [한국어] 이하는 다시 시스템 절전 전용 구간 */

/*
 * Default "suspend" method for devices that have no driver provided suspend,
 * or not even a driver at all (second part).
 */
/* [한국어]
 * pci_pm_set_unknown_state - 절전에 들어가며 "전원 상태를 모른다" 고 표시
 *
 * @pci_dev: 대상 장치.  @return: 없음.
 *
 * 원본 주석이 이유를 밝힌다 — 절전 중에 BIOS 나 플랫폼 펌웨어가 장치의
 * 전원 상태를 바꿔 놓을 수 있고, 커널은 그것을 알 방법이 없다. 그러니
 * D0 라고 기억하고 있던 것을 PCI_UNKNOWN 으로 바꿔 두면, 복귀할 때
 * "이미 D0 니까 아무것도 안 해도 된다" 는 잘못된 최적화를 하지 않는다.
 *
 * D0 일 때만 바꾸는 이유는, 이미 D3 등으로 내려간 장치는 커널이 스스로
 * 내린 것이라 그 값이 여전히 유효한 정보이기 때문이다.
 *
 * 실행 컨텍스트: _noirq 단계. 인터럽트가 꺼져 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_legacy_suspend_late() / pci_pm_suspend_noirq() /
 *   pci_pm_freeze_noirq() → [이 함수]
 */
static void pci_pm_set_unknown_state(struct pci_dev *pci_dev)
{
	/*
	 * mark its power state as "unknown", since we don't know if
	 * e.g. the BIOS will change its device state when we suspend.
	 */
	if (pci_dev->current_state == PCI_D0)	/* [한국어] 아직 D0 라고 기억하고 있다면 */
		pci_dev->current_state = PCI_UNKNOWN;	/* [한국어] 모른다고 바꾼다. 커널이 스스로 내린 D3 등은 그대로 두는데, 그 값은 여전히 유효한 정보이기 때문이다 */
}

/*
 * Default "resume" method for devices that have no driver provided resume,
 * or not even a driver at all (second part).
 */
/* [한국어]
 * pci_pm_reenable_device - 드라이버 resume 콜백이 없을 때의 기본 되살리기
 *
 * @pci_dev: 대상 장치.
 * @return: pci_reenable_device() 의 반환값. 0 성공, 음수 errno 실패.
 *
 * 드라이버가 .resume 을 주지 않았다면 누군가는 장치를 다시 쓸 수 있게
 * 만들어야 한다. 그 최소한이 두 가지다.
 *
 *   (1) pci_reenable_device() — 절전 전에 enable 상태였다면 다시 enable 한다.
 *       PCI COMMAND 레지스터의 I/O Space / Memory Space 활성 비트를
 *       되살리는 일이다. "reenable" 인 이유는 enable 카운트를 다시 올리지
 *       않고 하드웨어 쪽만 되살리기 때문이다.
 *   (2) 절전 전에 Bus Master 였다면 pci_set_master() 로 그것도 되살린다.
 *       이게 없으면 장치가 DMA 를 낼 수 없어 조용히 동작하지 않는다.
 *
 * is_busmaster 플래그는 pci_set_master()/pci_clear_master() 가 관리하는
 * 커널 쪽 기억이다. config 복원만으로는 이 소프트웨어 상태와 하드웨어가
 * 어긋날 수 있어 명시적으로 다시 맞춘다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. CONFIG_PM_SLEEP 에서 컴파일된다.
 *
 * 에러 경로: pci_reenable_device() 의 실패를 그대로 올려보낸다. Bus Master
 * 복원은 그 실패와 무관하게 시도된다(코드 순서상 항상 실행된다).
 *
 * 호출 체인:
 *   pci_legacy_resume() / pci_pm_resume() / pci_pm_thaw() / pci_pm_restore()
 *   → [이 함수] → pci_reenable_device(), pci_set_master()
 */
static int pci_pm_reenable_device(struct pci_dev *pci_dev)
{
	int retval;	/* [한국어] pci_reenable_device 의 결과 */

	/* if the device was enabled before suspend, re-enable */
	retval = pci_reenable_device(pci_dev);	/* [한국어] 절전 전에 enable 상태였다면 하드웨어를 다시 켠다. enable 카운트는 건드리지 않는다 */
	/*
	 * if the device was busmaster before the suspend, make it busmaster
	 * again
	 */
	if (pci_dev->is_busmaster)	/* [한국어] 절전 전에 Bus Master 였는지는 커널이 따로 기억하고 있다 */
		pci_set_master(pci_dev);	/* [한국어] 그랬다면 다시 켠다. 이게 없으면 장치가 DMA 를 못 내면서도 조용히 동작하는 것처럼 보인다 */

	return retval;	/* [한국어] reenable 결과를 그대로 올려보낸다. Bus Master 복원 실패는 반영되지 않는다 */
}

/* [한국어]
 * pci_legacy_suspend - 옛 방식 .suspend 콜백을 가진 드라이버용 절전 경로
 *
 * @dev: 대상 장치.  @state: pm_message_t. PMSG_SUSPEND / PMSG_FREEZE /
 *   PMSG_HIBERNATE 중 하나가 호출자에 따라 넘어온다.
 * @return: 0 성공, 드라이버 콜백이 실패하면 그 errno.
 *
 * struct pci_driver 에는 dev_pm_ops 가 도입되기 전부터 있던 .suspend /
 * .resume 함수 포인터가 아직 남아 있다. 그것을 쓰는 드라이버를 위한
 * 경로가 이 함수다. 새로 작성되는 드라이버는 driver.pm(dev_pm_ops) 를
 * 써야 하며, 둘을 동시에 쓰면 pci_has_legacy_pm_support() 가 경고한다.
 *
 * 동작. state_saved 를 false 로 지워 "아직 저장 안 함" 으로 만든 뒤
 * 드라이버 콜백을 부른다. 그리고 검사를 한다 — 드라이버가 장치를 D0 가
 * 아닌 상태로 내려놓았는데 config 를 저장하지 않았다면 경고한다. 저전력
 * 상태로 내려가면 config 가 날아가므로, 내리기 전에 반드시 저장했어야
 * 하기 때문이다. 조건에 current_state != prev 를 붙인 것은 "드라이버가
 * 상태를 바꾼 경우에만" 따지겠다는 뜻이다.
 *
 * 마지막으로 suspend quirk(pci_fixup_suspend)를 돌린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 인터럽트는 아직 살아 있다.
 *
 * 에러 경로: 드라이버가 실패를 돌려주면 suspend_report_result() 로 어느
 * 함수가 실패했는지 기록한 뒤 그 값을 올려보낸다. 그러면 PM 코어가 절전
 * 전체를 되감는다.
 *
 * 호출 체인:
 *   pci_pm_suspend() / pci_pm_freeze() / pci_pm_poweroff()
 *   → [이 함수] → drv->suspend()
 */
static int pci_legacy_suspend(struct device *dev, pm_message_t state)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	struct pci_driver *drv = pci_dev->driver;	/* [한국어] 레거시 콜백을 가진 드라이버 */

	pci_dev->state_saved = false;	/* [한국어] "아직 config 를 저장하지 않았다" 로 초기화. 드라이버가 pci_save_state 를 부르면 그 안에서 참이 된다 */

	if (drv && drv->suspend) {	/* [한국어] 드라이버가 붙어 있고 레거시 suspend 를 주었다면 */
		pci_power_t prev = pci_dev->current_state;	/* [한국어] 드라이버 호출 전의 전원 상태를 기억해 둔다. 아래 경고 조건에 쓰인다 */
		int error;	/* [한국어] 드라이버 반환값 */

		error = drv->suspend(pci_dev, state);	/* [한국어] 레거시 콜백은 목표 전원 상태를 pm_message_t 로 함께 받는다 */
		suspend_report_result(dev, drv->suspend, error);	/* [한국어] 실패했다면 어느 함수가 실패했는지 커널 로그에 기록한다 */
		if (error)	/* [한국어] 실패 검사 */
			return error;	/* [한국어] 그대로 올려보내면 PM 코어가 절전 전체를 되감는다 */

		if (!pci_dev->state_saved && pci_dev->current_state != PCI_D0	/* [한국어] 저장은 안 했는데 D0 도 UNKNOWN 도 아니다 = 저전력으로 내려가면서 config 를 버렸다 */
		    && pci_dev->current_state != PCI_UNKNOWN) {	/* [한국어] UNKNOWN 은 상태를 모르는 것이므로 따지지 않는다 */
			pci_WARN_ONCE(pci_dev, pci_dev->current_state != prev,	/* [한국어] 다만 드라이버가 실제로 상태를 바꾼 경우에만 경고한다 */
				      "PCI PM: Device state not saved by %pS\n",	/* [한국어] %pS 는 함수 포인터를 심볼 이름으로 찍는 커널 전용 형식이다 */
				      drv->suspend);	/* [한국어] 문제를 일으킨 드라이버 콜백을 지목한다 */
		}
	}

	pci_fixup_device(pci_fixup_suspend, pci_dev);	/* [한국어] suspend quirk 실행 */

	return 0;	/* [한국어] 드라이버가 실패하지 않았다면 언제나 성공 */
}

/* [한국어]
 * pci_legacy_suspend_late - 옛 방식 드라이버의 절전 마무리(noirq 단계)
 *
 * @dev: 대상 장치.  @return: 항상 0.
 *
 * 레거시 .suspend 에는 noirq 짝이 없다. 그래서 그 자리에서 PCI 계층이
 * 대신 마무리를 한다.
 *
 *   (1) 드라이버가 config 를 저장하지 않았으면(state_saved 가 거짓)
 *       pci_save_state() 로 대신 저장한다.
 *   (2) pci_pm_set_unknown_state() — 전원 상태를 모른다고 표시.
 *   (3) suspend_late quirk 를 돌린다.
 *
 * 눈여겨볼 것 하나. 새 방식 경로의 pci_pm_suspend_noirq() 와 달리
 * 여기서는 pci_prepare_to_sleep() 을 부르지 않는다. 즉 D-state 를 내리지
 * 않는다. 레거시 드라이버는 자기 .suspend 안에서 전원 상태까지 직접
 * 다루는 것이 전제이기 때문이다.
 *
 * 실행 컨텍스트: _noirq 단계. 인터럽트가 꺼져 있다.
 *
 * 에러 경로: 없다. 항상 0 을 돌려준다.
 *
 * 호출 체인:
 *   pci_pm_suspend_noirq() / pci_pm_freeze_noirq() / pci_pm_poweroff_noirq()
 *   → [이 함수] → pci_save_state(), pci_pm_set_unknown_state()
 */
static int pci_legacy_suspend_late(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */

	if (!pci_dev->state_saved)	/* [한국어] 드라이버가 저장하지 않았다면 */
		pci_save_state(pci_dev);	/* [한국어] PCI 계층이 대신 저장한다 */

	pci_pm_set_unknown_state(pci_dev);	/* [한국어] 전원 상태를 모른다고 표시 */

	pci_fixup_device(pci_fixup_suspend_late, pci_dev);	/* [한국어] suspend_late quirk 실행 */

	return 0;	/* [한국어] 실패할 여지가 없다 */
}

/* [한국어]
 * pci_legacy_resume - 옛 방식 .resume 콜백을 가진 드라이버용 복귀 경로
 *
 * @dev: 대상 장치.
 * @return: 드라이버 .resume 이 있으면 그 반환값, 없으면
 *   pci_pm_reenable_device() 의 반환값.
 *
 * 먼저 resume quirk 를 돌리고, 그다음 드라이버 콜백이 있으면 부르고
 * 없으면 기본 되살리기를 한다. 삼항 연산자 한 줄이 그 분기다.
 *
 * pci_legacy_suspend() 이 state_saved 나 D-state 를 꼼꼼히 따지는 것에
 * 비하면 이쪽은 훨씬 단순하다. 복원(pci_restore_state)은 이 함수가 아니라
 * 그 앞의 noirq 단계에서 이미 끝났기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 인터럽트가 살아난 뒤다.
 *
 * 에러 경로: 드라이버가 돌려준 값을 그대로 올려보낸다.
 *
 * 호출 체인:
 *   pci_pm_resume() / pci_pm_thaw() / pci_pm_restore()
 *   → [이 함수] → drv->resume() 또는 pci_pm_reenable_device()
 */
static int pci_legacy_resume(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	struct pci_driver *drv = pci_dev->driver;	/* [한국어] 레거시 콜백을 가진 드라이버 */

	pci_fixup_device(pci_fixup_resume, pci_dev);	/* [한국어] resume quirk 를 먼저 실행한다 */

	return drv && drv->resume ?	/* [한국어] 드라이버가 붙어 있고 레거시 resume 을 주었으면 그것을, */
			drv->resume(pci_dev) : pci_pm_reenable_device(pci_dev);	/* [한국어] 아니면 최소한의 되살리기(enable + Bus Master)를 한다 */
}

/* Auxiliary functions used by the new power management framework */

/* [한국어]
 * pci_pm_default_suspend - PM 콜백이 아예 없는 장치를 절전 전에 정리
 *
 * @pci_dev: 대상 장치.  @return: 없음.
 *
 * 드라이버가 없거나 있어도 PM 콜백을 하나도 주지 않은 장치를 위한 처리다.
 * 그런 장치는 그냥 disable 해 버린다 — 어차피 아무도 쓰지 않는다.
 *
 * 브리지는 예외다. pci_has_subordinate() 로 하위 버스가 있는지 보고,
 * 있으면 건드리지 않는다. 브리지를 disable 하면 그 아래 장치들의 접근
 * 경로가 통째로 끊기기 때문이다.
 *
 * pci_disable_enabled_device() 는 pci_disable_device() 와 달리 enable
 * 카운트를 건드리지 않고 하드웨어만 끈다. 복귀할 때
 * pci_pm_reenable_device() 가 짝을 맞춰 되살린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_pm_suspend() / pci_pm_freeze() / pci_pm_poweroff()
 *   → [이 함수] → pci_disable_enabled_device()
 */
static void pci_pm_default_suspend(struct pci_dev *pci_dev)
{
	/* Disable non-bridge devices without PM support */
	if (!pci_has_subordinate(pci_dev))	/* [한국어] 하위 버스가 없는 장치, 즉 브리지가 아닌 장치만 */
		pci_disable_enabled_device(pci_dev);	/* [한국어] disable 한다. 브리지를 끄면 그 아래 접근 경로가 통째로 끊긴다 */
}

/* [한국어]
 * pci_has_legacy_pm_support - 이 장치의 드라이버가 옛 방식 PM 을 쓰는가
 *
 * @pci_dev: 대상 장치.
 * @return: 드라이버가 .suspend 나 .resume 중 하나라도 가지고 있으면 true.
 *
 * 거의 모든 pci_pm_* 함수가 맨 앞에서 이것부터 묻는다. 참이면 레거시
 * 경로로 갈라지고, 거짓이면 dev_pm_ops 경로로 간다.
 *
 * 판정 겸 검사 역할도 한다. 레거시 콜백과 dev_pm_ops 를 둘 다 가진
 * 드라이버는 잘못 작성된 것이므로 pci_WARN 으로 경고한다. 원본 주석대로
 * 둘 중 하나만 써야 하고, 둘 다 있으면 레거시가 이긴다(이 함수가 true 를
 * 돌려주므로 dev_pm_ops 는 무시된다).
 *
 * 실행 컨텍스트: 아무 PM 단계에서나 불린다. _noirq 단계에서도 불리므로
 * 잠들면 안 되는데, 하는 일이 포인터 검사와 경고 출력뿐이라 문제없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   거의 모든 pci_pm_* → [이 함수]
 */
static bool pci_has_legacy_pm_support(struct pci_dev *pci_dev)
{
	struct pci_driver *drv = pci_dev->driver;	/* [한국어] 현재 바인딩된 드라이버 */
	bool ret = drv && (drv->suspend || drv->resume);	/* [한국어] 레거시 콜백을 하나라도 가지고 있으면 레거시로 본다 */

	/*
	 * Legacy PM support is used by default, so warn if the new framework is
	 * supported as well.  Drivers are supposed to support either the
	 * former, or the latter, but not both at the same time.
	 */
	pci_WARN(pci_dev, ret && drv->driver.pm, "device %04x:%04x\n",	/* [한국어] 레거시와 dev_pm_ops 를 둘 다 가진 잘못된 드라이버를 경고한다 */
		 pci_dev->vendor, pci_dev->device);	/* [한국어] 어느 장치인지 Vendor/Device ID 로 지목한다 */

	return ret;	/* [한국어] 참이면 호출자가 레거시 경로로 갈라진다 */
}

/* New power management framework */

/* [한국어]
 * pci_pm_prepare - 절전 사이클의 첫 단계. 이 장치를 건너뛸 수 있는지 판정
 *
 * @dev: 대상 장치.
 * @return: 음수면 errno(절전 중단). 0 이면 "정상적으로 절전 단계를 다
 *   밟아라". 1 이면 "direct-complete — 이 장치는 건드리지 말고 넘어가라".
 *
 * direct-complete 는 PM 코어의 최적화다. 이미 런타임 절전으로 잠들어 있고
 * 시스템 절전을 위해 굳이 깨울 필요도 없는 장치라면, suspend/resume 단계를
 * 통째로 건너뛰고 complete 만 부른다. 수백 개의 장치가 달린 시스템에서
 * 절전 시간을 크게 줄여 준다.
 *
 * 판정 순서.
 *   (1) dev_pm_set_strict_midlayer(dev, true) — 이 절전 사이클 동안
 *       PCI 라는 중간 계층의 규약을 엄격히 적용하라고 PM 코어에 알린다.
 *   (2) 드라이버가 .prepare 를 주었으면 부른다. 음수면 바로 중단.
 *       드라이버가 0 을 돌려주고 DPM_FLAG_SMART_PREPARE 플래그를 걸어
 *       두었다면 "내가 판단했으니 그 결과를 존중하라" 는 뜻이라 그대로 0.
 *   (3) pci_dev_need_resume() — PCI 계층이 보기에 이 장치를 꼭 깨워야
 *       하는가(웨이크업 설정을 바꿔야 한다든지). 그렇다면 0.
 *   (4) 아니면 direct-complete 를 허용하되, 그 전에 pci_dev_adjust_pme()
 *       로 PME 설정만 맞춰 둔다. 원본 주석이 밝히는 대로, 건너뛸 것이므로
 *       나중에 맞출 기회가 없어 지금 해야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 절전 시작 시점. 아직 여유가 있다.
 *
 * 에러 경로: 드라이버 .prepare 의 음수 반환만이 실패다.
 *
 * 호출 체인:
 *   PM 코어(device_prepare) → [이 함수] → pm->prepare(),
 *   pci_dev_need_resume(), pci_dev_adjust_pme()
 */
static int pci_pm_prepare(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버가 붙어 있으면 그 dev_pm_ops, 아니면 NULL. 이 관용구가 아래 모든 pci_pm_* 에 반복된다 */

	dev_pm_set_strict_midlayer(dev, true);	/* [한국어] 이번 절전 사이클 동안 PCI 라는 중간 계층의 규약을 엄격히 적용하라고 PM 코어에 알린다 */

	if (pm && pm->prepare) {	/* [한국어] 드라이버가 prepare 콜백을 주었다면 */
		int error = pm->prepare(dev);	/* [한국어] 부른다. 반환값 규약은 이 함수와 같다 */
		if (error < 0)	/* [한국어] 음수는 오류 */
			return error;	/* [한국어] 그대로 올려보내면 절전이 중단된다 */

		if (!error && dev_pm_test_driver_flags(dev, DPM_FLAG_SMART_PREPARE))	/* [한국어] 드라이버가 0(정상)을 돌려주었고 SMART_PREPARE 플래그를 걸었다면 그 판단을 존중한다 */
			return 0;	/* [한국어] direct-complete 를 하지 않고 정상 경로로 간다 */
	}
	if (pci_dev_need_resume(pci_dev))	/* [한국어] PCI 계층이 보기에 이 장치를 꼭 깨워야 하는가 */
		return 0;	/* [한국어] 그렇다면 건너뛰지 않는다 */

	/*
	 * The PME setting needs to be adjusted here in case the direct-complete
	 * optimization is used with respect to this device.
	 */
	pci_dev_adjust_pme(pci_dev);	/* [한국어] 건너뛸 것이므로 나중에 맞출 기회가 없다. PME 설정만 지금 조정해 둔다 */
	return 1;	/* [한국어] 1 = direct-complete 허용. suspend/resume 단계를 통째로 건너뛴다 */
}

/* [한국어]
 * pci_pm_complete - 절전 사이클의 마지막 단계. 뒷정리와 펌웨어 리셋 감지
 *
 * @dev: 대상 장치.  @return: 없음.
 *
 * prepare 의 짝이다. 절전에 성공했든 중간에 실패해 되감았든 반드시 불린다.
 *
 *   (1) pci_dev_complete_resume() — PCI 계층의 사이클 종료 처리.
 *   (2) pm_generic_complete() — 드라이버의 .complete 콜백을 부른다.
 *   (3) 펌웨어 리셋 감지. 장치가 아직 런타임 절전 상태이고 이번 복귀가
 *       펌웨어를 거친 것이라면(S3 등), 펌웨어가 장치를 리셋해 D0 로
 *       올려놓았을 수 있다. pci_refresh_power_state() 로 실제 상태를 다시
 *       읽고, 커널이 기억하던 것보다 높은 전원 상태(숫자가 작은 쪽)로
 *       올라와 있으면 pm_request_resume() 으로 정식 복귀를 요청한다.
 *       그렇게 하지 않으면 커널은 잠들어 있다고 믿는데 실제로는 깨어 있는
 *       불일치가 남는다.
 *   (4) dev_pm_set_strict_midlayer(dev, false) — prepare 에서 켠 것을 끈다.
 *
 * 원본 주석은 (3)의 부작용도 인정한다 — ACPI 플랫폼에서는 전원 자원을
 * 공유하는 다른 장치 때문에 이 조건이 헛맞을 수 있다. 그래도 깨우는 편이
 * 낫다고 판단한 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 복귀 후.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   PM 코어(device_complete) → [이 함수] → pci_dev_complete_resume(),
 *   pm_generic_complete(), pm_request_resume()
 */
static void pci_pm_complete(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */

	pci_dev_complete_resume(pci_dev);	/* [한국어] PCI 계층의 사이클 종료 처리 */
	pm_generic_complete(dev);	/* [한국어] 드라이버의 complete 콜백을 부른다 */

	/* Resume device if platform firmware has put it in reset-power-on */
	if (pm_runtime_suspended(dev) && pm_resume_via_firmware()) {	/* [한국어] 런타임 절전 상태로 남아 있고, 이번 복귀가 펌웨어를 거친 것이라면(S3 등) */
		pci_power_t pre_sleep_state = pci_dev->current_state;	/* [한국어] 커널이 기억하던 절전 전 상태를 챙겨 둔다 */

		pci_refresh_power_state(pci_dev);	/* [한국어] 하드웨어에서 실제 전원 상태를 다시 읽는다 */
		/*
		 * On platforms with ACPI this check may also trigger for
		 * devices sharing power resources if one of those power
		 * resources has been activated as a result of a change of the
		 * power state of another device sharing it.  However, in that
		 * case it is also better to resume the device, in general.
		 */
		if (pci_dev->current_state < pre_sleep_state)	/* [한국어] D-state 는 숫자가 작을수록 높은 전력이다. 기억보다 작아졌다 = 펌웨어가 깨워 놓았다 */
			pm_request_resume(dev);	/* [한국어] 그렇다면 정식 복귀를 요청해 커널의 인식과 하드웨어를 일치시킨다 */
	}

	dev_pm_set_strict_midlayer(dev, false);	/* [한국어] prepare 에서 켠 엄격 모드를 끈다 */
}

#else /* !CONFIG_PM_SLEEP */
/* [한국어] 시스템 절전을 아예 빌드하지 않는 커널에서는 두 콜백 이름을 NULL 로
 * 정의해 둔다. 그러면 아래 pci_dev_pm_ops 의 해당 칸이 NULL 이 되고, PM
 * 코어는 그 단계를 건너뛴다. #ifdef 를 구조체 초기화 안에 넣지 않으려는
 * 흔한 기법이다. */
#define pci_pm_prepare	NULL	/* [한국어] prepare 단계 없음 */
#define pci_pm_complete	NULL	/* [한국어] complete 단계 없음 */

#endif /* !CONFIG_PM_SLEEP */

#ifdef CONFIG_SUSPEND	/* [한국어] 이하는 서스펜드(S3 등)를 빌드할 때만 컴파일되는 구간 */
/* [한국어]
 * pcie_pme_root_status_cleanup - 루트 포트의 PME Status 찌꺼기를 지운다
 *
 * @pci_dev: 검사할 장치. 루트 포트가 아니면 아무 일도 하지 않는다.
 * @return: 없음.
 *
 * PCIe 에서 하위 장치의 웨이크업 요청(PME)은 루트 포트의 Root Status
 * 레지스터에 기록된다. 그 비트는 소프트웨어가 지워 줘야 다음 PME 를 받을
 * 수 있는데, 원본 주석이 밝히듯 일부 BIOS 가 시스템 웨이크업 후 그것을
 * 지우지 않고 넘긴다. 그러면 이후의 ACPI 기반 런타임 웨이크업이 아예
 * 동작하지 않는다. 그래서 복귀 경로에서 커널이 한 번 지워 준다.
 *
 * 대상은 PCIe 장치 중 타입이 Root Port(PCI_EXP_TYPE_ROOT_PORT) 이거나
 * Root Complex Event Collector(PCI_EXP_TYPE_RC_EC) 인 것뿐이다. 후자는
 * 루트 컴플렉스에 통합된 장치들의 이벤트를 모으는 특수 기능이라 PME 도
 * 그쪽으로 올라온다.
 *
 * 원본 주석의 "shouldn't hurt" 가 이 함수의 성격을 잘 말해 준다 — 지워도
 * 손해 볼 것이 없으니 그냥 지운다.
 *
 * 실행 컨텍스트: pci_pm_resume_noirq() 안, 인터럽트가 꺼진 상태.
 * CONFIG_SUSPEND 에서만 컴파일된다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_pm_resume_noirq() → [이 함수] → pcie_clear_root_pme_status()
 */
static void pcie_pme_root_status_cleanup(struct pci_dev *pci_dev)
{
	/*
	 * Some BIOSes forget to clear Root PME Status bits after system
	 * wakeup, which breaks ACPI-based runtime wakeup on PCI Express.
	 * Clear those bits now just in case (shouldn't hurt).
	 */
	if (pci_is_pcie(pci_dev) &&	/* [한국어] PCIe 장치이면서 */
	    (pci_pcie_type(pci_dev) == PCI_EXP_TYPE_ROOT_PORT ||	/* [한국어] 루트 포트이거나 */
	     pci_pcie_type(pci_dev) == PCI_EXP_TYPE_RC_EC))	/* [한국어] 루트 컴플렉스 이벤트 컬렉터라면(루트 컴플렉스 내장 장치들의 이벤트를 모으는 기능) */
		pcie_clear_root_pme_status(pci_dev);	/* [한국어] Root Status 의 PME Status 비트를 지운다 */
}

/* [한국어]
 * pci_pm_suspend - 시스템 절전(S3 등)의 첫 실질 단계
 *
 * @dev: 대상 장치.  @return: 0 성공, 드라이버 콜백이 실패하면 그 errno.
 *
 * 인터럽트가 아직 살아 있는 단계다. 장치와 명령을 주고받으며 정지시키는
 * 일은 여기서 해야 한다(그 다음 _noirq 에서는 완료 인터럽트를 못 받는다).
 *
 * 동작 순서.
 *   (1) skip_bus_pm 플래그를 지운다. 이번 사이클에서 다시 판정할 값이다.
 *   (2) pci_suspend_ptm() — PTM(Precision Time Measurement)을 끈다.
 *       원본 주석대로 커피레이크 이후 인텔 모바일 칩 등에서 PTM 이 켜져
 *       있으면 더 깊은 절전 상태로 못 내려간다.
 *   (3) 레거시 PM 드라이버면 그쪽으로 넘긴다.
 *   (4) dev_pm_ops 조차 없으면 pci_pm_default_suspend() 로 disable 만 하고 끝.
 *   (5) 런타임 절전 중이던 장치를 깨울지 판정한다. 원본 주석이 두 가지
 *       이유를 든다 — 이 장치로 시스템을 깨울 예정이면 그에 맞게 다시
 *       설정해야 하고, 반대로 깨우지 않을 예정이면 웨이크업을 막아야
 *       한다. 또 드라이버가 "런타임 절전 상태에서도 내 시스템 절전
 *       콜백이 잘 동작한다"(smart suspend)고 밝히지 않았다면 안전하게
 *       깨우는 편이 낫다. 깨우면 state_saved 를 지워 다시 저장하게 만든다.
 *       깨우지 않기로 했으면 PME 설정만 조정한다.
 *   (6) 드라이버 .suspend 를 부르고, 레거시 경로와 같은 검사(저전력으로
 *       내려갔는데 config 를 저장하지 않았는가)를 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. CONFIG_SUSPEND 에서만 컴파일된다.
 *
 * 에러 경로: 드라이버 실패를 그대로 올려보내면 PM 코어가 절전을 되감는다.
 *
 * 호출 체인:
 *   PM 코어 → [이 함수] → pm->suspend() 또는 pci_legacy_suspend()
 */
static int pci_pm_suspend(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버의 dev_pm_ops. 없으면 NULL */

	pci_dev->skip_bus_pm = false;	/* [한국어] 이번 사이클에서 새로 판정할 값이므로 지우고 시작한다 */

	/*
	 * Disabling PTM allows some systems, e.g., Intel mobile chips
	 * since Coffee Lake, to enter a lower-power PM state.
	 */
	pci_suspend_ptm(pci_dev);	/* [한국어] PTM(Precision Time Measurement)을 끈다. 켜져 있으면 일부 칩셋이 더 깊은 절전으로 못 내려간다 */

	if (pci_has_legacy_pm_support(pci_dev))	/* [한국어] 레거시 콜백을 쓰는 드라이버라면 */
		return pci_legacy_suspend(dev, PMSG_SUSPEND);	/* [한국어] 그쪽 경로로 넘긴다. PMSG_SUSPEND 가 "S3 진입" 을 뜻한다 */

	if (!pm) {	/* [한국어] dev_pm_ops 자체가 없으면 */
		pci_pm_default_suspend(pci_dev);	/* [한국어] 브리지가 아닌 장치를 disable 하는 것으로 갈음한다 */
		return 0;	/* [한국어] 더 할 일이 없다 */
	}

	/*
	 * PCI devices suspended at run time may need to be resumed at this
	 * point, because in general it may be necessary to reconfigure them for
	 * system suspend.  Namely, if the device is expected to wake up the
	 * system from the sleep state, it may have to be reconfigured for this
	 * purpose, or if the device is not expected to wake up the system from
	 * the sleep state, it should be prevented from signaling wakeup events
	 * going forward.
	 *
	 * Also if the driver of the device does not indicate that its system
	 * suspend callbacks can cope with runtime-suspended devices, it is
	 * better to resume the device from runtime suspend here.
	 */
	if (!dev_pm_smart_suspend(dev) || pci_dev_need_resume(pci_dev)) {	/* [한국어] 드라이버가 런타임 절전 상태를 감당한다고 밝히지 않았거나, PCI 계층이 보기에 깨워야 한다면 */
		pm_runtime_resume(dev);	/* [한국어] 깨운다 */
		pci_dev->state_saved = false;	/* [한국어] 깨웠으니 저장분이 무의미하다. 다시 저장하도록 지운다 */
	} else {	/* [한국어] 그 밖의 경우는 잠든 채로 둔다 */
		pci_dev_adjust_pme(pci_dev);	/* [한국어] 깨우지 않기로 했으므로 PME(웨이크업) 설정만 이번 절전에 맞게 조정해 둔다 */
	}

	if (pm->suspend) {	/* [한국어] 드라이버가 suspend 콜백을 주었다면 */
		pci_power_t prev = pci_dev->current_state;	/* [한국어] 호출 전 전원 상태를 기억해 둔다 */
		int error;	/* [한국어] 드라이버 반환값 */

		error = pm->suspend(dev);	/* [한국어] 드라이버의 시스템 suspend 콜백. 인터럽트가 아직 살아 있으므로 장치와 주고받는 명령을 낼 수 있는 마지막 단계다 */
		suspend_report_result(dev, pm->suspend, error);	/* [한국어] 실패하면 어느 콜백이었는지 로그에 남긴다 */
		if (error)	/* [한국어] 실패 검사 */
			return error;	/* [한국어] 그대로 올려보내 절전을 되감게 한다 */

		if (!pci_dev->state_saved && pci_dev->current_state != PCI_D0	/* [한국어] 저장 없이 저전력으로 내려간 경우를 잡아낸다 */
		    && pci_dev->current_state != PCI_UNKNOWN) {	/* [한국어] UNKNOWN 은 판정에서 제외 */
			pci_WARN_ONCE(pci_dev, pci_dev->current_state != prev,	/* [한국어] 드라이버가 실제로 상태를 바꾼 경우에만 경고 */
				      "PCI PM: State of device not saved by %pS\n",	/* [한국어] 경고 문구 */
				      pm->suspend);	/* [한국어] 문제의 콜백을 심볼 이름으로 지목 */
		}
	}

	return 0;	/* [한국어] 성공 */
}

/* [한국어]
 * pci_pm_suspend_late - suspend 와 suspend_noirq 사이의 중간 단계
 *
 * @dev: 대상 장치.  @return: 0 또는 드라이버 .suspend_late 의 반환값.
 *
 * dev_pm_skip_suspend() 가 참이면 아무것도 하지 않는다. direct-complete 로
 * 건너뛰기로 한 장치이거나, 앞 단계에서 이미 처리가 끝난 경우다.
 *
 * 그렇지 않으면 suspend quirk(pci_fixup_suspend)를 돌리고
 * pm_generic_suspend_late() 로 드라이버 콜백을 부른다. "generic" 헬퍼를
 * 쓴다는 것은 PCI 계층이 이 단계에서 따로 할 일이 없다는 뜻이다.
 *
 * quirk 를 suspend 가 아니라 여기서 돌리는 이유는, 드라이버의 .suspend 가
 * 끝난 뒤에 보정을 넣어야 하는 하드웨어가 있기 때문이다. 레거시 경로에서는
 * pci_legacy_suspend() 안에서 같은 quirk 를 돌린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 아직 인터럽트는 살아 있다.
 *
 * 에러 경로: 드라이버 콜백 실패를 그대로 전달한다.
 *
 * 호출 체인:
 *   PM 코어 → [이 함수] → pci_fixup_device(), pm_generic_suspend_late()
 */
static int pci_pm_suspend_late(struct device *dev)
{
	if (dev_pm_skip_suspend(dev))	/* [한국어] direct-complete 등으로 건너뛰기로 한 장치라면 */
		return 0;	/* [한국어] 아무것도 하지 않는다 */

	pci_fixup_device(pci_fixup_suspend, to_pci_dev(dev));	/* [한국어] suspend quirk 실행. 레거시 경로에서는 pci_legacy_suspend 안에서 같은 것을 돌린다 */

	return pm_generic_suspend_late(dev);	/* [한국어] PCI 계층이 이 단계에서 따로 할 일이 없어 드라이버 콜백만 부른다 */
}

/* [한국어]
 * pci_pm_suspend_noirq - 인터럽트를 끈 뒤의 절전 마무리. 실제로 전원을 내리는 곳
 *
 * @dev: 대상 장치.  @return: 0 성공, 드라이버 콜백 실패 시 그 errno.
 *
 * 이 파일에서 가장 복잡한 함수다. PCI 절전의 핵심 동작 — config 저장과
 * D-state 전환 — 이 여기서 일어난다.
 *
 * 흐름.
 *   (1) 건너뛰기 판정, 레거시 경로 분기, PM 콜백 없는 장치 처리(저장만 하고
 *       Fixup 으로 점프).
 *   (2) 드라이버 .suspend_noirq 를 부른다. 성공했는데 저전력 상태로
 *       내려가면서 저장을 안 했으면 경고하고 Fixup 으로 점프한다 — 그런
 *       장치는 아래의 저장·전환을 해 봐야 소용없다.
 *   (3) 아직 저장이 안 되어 있으면 pci_save_state() 로 저장하고,
 *       skip_bus_pm 이 아니고 전원 관리가 가능한 장치면
 *       pci_prepare_to_sleep() 으로 적절한 D-state 로 내린다.
 *   (4) 결과 상태를 로그로 남긴다.
 *   (5) 그래도 D0 에 남아 있다면 skip_bus_pm 을 세우고, 부모 브리지에도
 *       같은 표시를 한다. 원본 주석이 근거를 댄다 — PCI PM 규격 1.2 표 6-1
 *       에 따르면 하위 장치가 D0 이면 브리지도 D0 여야 한다.
 *   (6) suspend-to-idle 처럼 플랫폼이 개입하지 않는 절전이고 skip_bus_pm
 *       이면 여기서 그만둔다.
 *   (7) 전원 상태를 UNKNOWN 으로 표시.
 *   (8) EHCI USB 컨트롤러 특례. 원본 주석대로 일부 ASUS BIOS 가 COMMAND
 *       레지스터가 0 이 아니면 "아직 안 멈췄다" 고 오해해 컨트롤러를
 *       끄려 들고, 이미 D3 면 멈추거나 메모리를 깨뜨린다. 절전 후에는
 *       COMMAND 값이 의미 없으므로 그냥 0 으로 써 버린다.
 *   (9) Fixup: suspend_late quirk 실행.
 *  (10) 웨이크업 가능하지만 이번엔 웨이크업원이 아닌 장치는
 *       may_skip_resume 을 끈다. 원본 주석대로, 그렇지 않으면 복귀 단계를
 *       건너뛰어 잘못된 웨이크업 설정이 그대로 남는다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 상태. 잠들 수 없다.
 *
 * 에러 경로: 드라이버 콜백 실패만이 실패다.
 *
 * 호출 체인:
 *   PM 코어 → [이 함수] → pm->suspend_noirq(), pci_save_state(),
 *   pci_prepare_to_sleep()
 */
static int pci_pm_suspend_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버의 dev_pm_ops */

	if (dev_pm_skip_suspend(dev))	/* [한국어] 건너뛰기로 한 장치면 */
		return 0;	/* [한국어] 아무것도 하지 않는다 */

	if (pci_has_legacy_pm_support(pci_dev))	/* [한국어] 레거시 드라이버면 */
		return pci_legacy_suspend_late(dev);	/* [한국어] 레거시 마무리 경로로 넘긴다 */

	if (!pm) {	/* [한국어] dev_pm_ops 가 없으면 */
		pci_save_state(pci_dev);	/* [한국어] config 만 저장하고 */
		goto Fixup;	/* [한국어] quirk 만 돌리고 끝낸다 */
	}

	if (pm->suspend_noirq) {	/* [한국어] 드라이버가 suspend_noirq 를 주었다면 */
		pci_power_t prev = pci_dev->current_state;	/* [한국어] 호출 전 전원 상태 */
		int error;	/* [한국어] 드라이버 반환값 */

		error = pm->suspend_noirq(dev);		/* [한국어] 인터럽트가 꺼진 뒤의 드라이버 콜백. 완료 인터럽트를 기다리는 일은 여기서 할 수 없다 */
		suspend_report_result(dev, pm->suspend_noirq, error);	/* [한국어] 실패 시 로그 */
		if (error)	/* [한국어] 실패 검사 */
			return error;	/* [한국어] 그대로 올려보낸다 */

		if (!pci_dev->state_saved && pci_dev->current_state != PCI_D0	/* [한국어] 저장 없이 저전력으로 내려갔다면 */
		    && pci_dev->current_state != PCI_UNKNOWN) {	/* [한국어] UNKNOWN 제외 */
			pci_WARN_ONCE(pci_dev, pci_dev->current_state != prev,	/* [한국어] 경고하고 */
				      "PCI PM: State of device not saved by %pS\n",	/* [한국어] 경고 문구 */
				      pm->suspend_noirq);	/* [한국어] 문제의 콜백 지목 */
			goto Fixup;	/* [한국어] 아래의 저장·전환은 해 봐야 소용없으므로 건너뛴다 */
		}
	}

	if (!pci_dev->state_saved) {	/* [한국어] 아직 저장이 안 되어 있다면 */
		pci_save_state(pci_dev);	/* [한국어] PCI 계층이 대신 저장한다 */

		/*
		 * If the device is a bridge with a child in D0 below it,
		 * it needs to stay in D0, so check skip_bus_pm to avoid
		 * putting it into a low-power state in that case.
		 */
		if (!pci_dev->skip_bus_pm && pci_power_manageable(pci_dev))	/* [한국어] 하위에 D0 장치가 있어 D0 로 남아야 하는 경우가 아니고, 전원 관리가 가능한 장치면 */
			pci_prepare_to_sleep(pci_dev);	/* [한국어] 적절한 저전력 D-state 로 내리고 필요한 웨이크업 설정을 건다 */
	}

	pci_dbg(pci_dev, "PCI PM: Suspend power state: %s\n",	/* [한국어] 실제로 어떤 전원 상태로 들어갔는지 디버그 로그로 남긴다 */
		pci_power_name(pci_dev->current_state));	/* [한국어] D0/D1/D2/D3hot/D3cold 같은 문자열로 변환 */

	if (pci_dev->current_state == PCI_D0) {	/* [한국어] 끝내 D0 에 남았다면 */
		pci_dev->skip_bus_pm = true;	/* [한국어] 이 장치는 버스 차원의 전원 관리에서 제외한다고 표시 */
		/*
		 * Per PCI PM r1.2, table 6-1, a bridge must be in D0 if any
		 * downstream device is in D0, so avoid changing the power state
		 * of the parent bridge by setting the skip_bus_pm flag for it.
		 */
		if (pci_dev->bus->self)	/* [한국어] 루트 버스가 아니라 부모 브리지가 있다면 */
			pci_dev->bus->self->skip_bus_pm = true;	/* [한국어] 그 브리지도 D0 에 남아야 한다. PCI PM 규격 1.2 표 6-1 의 요구다 */
	}

	if (pci_dev->skip_bus_pm && pm_suspend_no_platform()) {	/* [한국어] D0 로 남았고 플랫폼이 개입하지 않는 절전(suspend-to-idle)이라면 */
		pci_dbg(pci_dev, "PCI PM: Skipped\n");	/* [한국어] 건너뛰었음을 기록하고 */
		goto Fixup;	/* [한국어] 전원 상태 표시와 EHCI 특례를 건너뛴다 */
	}

	pci_pm_set_unknown_state(pci_dev);	/* [한국어] 전원 상태를 모른다고 표시 */

	/*
	 * Some BIOSes from ASUS have a bug: If a USB EHCI host controller's
	 * PCI COMMAND register isn't 0, the BIOS assumes that the controller
	 * hasn't been quiesced and tries to turn it off.  If the controller
	 * is already in D3, this can hang or cause memory corruption.
	 *
	 * Since the value of the COMMAND register doesn't matter once the
	 * device has been suspended, we can safely set it to 0 here.
	 */
	if (pci_dev->class == PCI_CLASS_SERIAL_USB_EHCI)	/* [한국어] 클래스 코드가 EHCI USB 호스트 컨트롤러라면 */
		pci_write_config_word(pci_dev, PCI_COMMAND, 0);	/* [한국어] COMMAND 레지스터(오프셋 0x04)를 0 으로 쓴다. 절전 후에는 이 값이 의미 없으므로 안전하다 */

Fixup:	/* [한국어] 여러 경로가 모이는 마무리 지점 */
	pci_fixup_device(pci_fixup_suspend_late, pci_dev);	/* [한국어] suspend_late quirk 실행 */

	/*
	 * If the target system sleep state is suspend-to-idle, it is sufficient
	 * to check whether or not the device's wakeup settings are good for
	 * runtime PM.  Otherwise, the pm_resume_via_firmware() check will cause
	 * pci_pm_complete() to take care of fixing up the device's state
	 * anyway, if need be.
	 */
	if (device_can_wakeup(dev) && !device_may_wakeup(dev))	/* [한국어] 웨이크업이 가능한 장치인데 이번엔 웨이크업원으로 쓰지 않는다면 */
		dev->power.may_skip_resume = false;	/* [한국어] 복귀 단계를 건너뛰지 못하게 한다. 건너뛰면 잘못된 웨이크업 설정이 그대로 남는다 */

	return 0;	/* [한국어] 성공 */
}

/* [한국어]
 * pci_pm_resume_noirq - 인터럽트를 켜기 전의 복귀. 장치를 되살리는 곳
 *
 * @dev: 대상 장치.  @return: 0 또는 드라이버 .resume_noirq 의 반환값.
 *
 * pci_pm_suspend_noirq() 의 거울상이다. 순서가 뒤집혀 있다.
 *
 *   (1) dev_pm_skip_resume() 이면 건너뛴다.
 *   (2) suspend 때 D0 에 남겨 둔 장치이고(skip_bus_pm) 플랫폼 개입 없는
 *       절전이었다면, 원본 주석대로 그대로 D0 에 있으니 복원할 것도
 *       없고 다시 D0 로 올릴 것도 없다. 그 외에는
 *       pci_pm_default_resume_early() 로 D0 복귀 + config 복원을 한다.
 *   (3) resume_early quirk 실행.
 *   (4) pcie_pme_root_status_cleanup() — 루트 포트라면 PME Status 청소.
 *   (5) D3cold 에서 올라온 브리지라면 하위 링크가 서기를 기다리고
 *       아래 계층을 깨운다.
 *   (6) 레거시 드라이버면 여기서 끝(레거시에는 noirq 짝이 없다).
 *   (7) 드라이버 .resume_noirq 를 부른다.
 *
 * prev_state 와 skip_bus_pm 을 함수 맨 앞에서 지역 변수에 복사해 두는
 * 이유는, (2)의 pci_pm_default_resume_early() 가 current_state 와
 * skip_bus_pm 을 바꿔 버리기 때문이다. (5)의 판정에는 바뀌기 전 값이
 * 필요하다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 상태. 다만 (5)의 링크 대기는 시간이
 * 걸릴 수 있다.
 *
 * 에러 경로: 드라이버 콜백 실패를 그대로 전달한다.
 *
 * 호출 체인:
 *   PM 코어 → [이 함수] → pci_pm_default_resume_early(),
 *   pci_pm_bridge_power_up_actions(), pm->resume_noirq()
 */
static int pci_pm_resume_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버의 dev_pm_ops */
	pci_power_t prev_state = pci_dev->current_state;	/* [한국어] 복원 전의 전원 상태를 챙겨 둔다. 아래 pci_pm_default_resume_early 가 이 값을 바꿔 버리기 때문이다 */
	bool skip_bus_pm = pci_dev->skip_bus_pm;	/* [한국어] 같은 이유로 skip_bus_pm 도 미리 복사해 둔다 */

	if (dev_pm_skip_resume(dev))	/* [한국어] 복귀를 건너뛰기로 한 장치라면 */
		return 0;	/* [한국어] 아무것도 하지 않는다 */

	/*
	 * In the suspend-to-idle case, devices left in D0 during suspend will
	 * stay in D0, so it is not necessary to restore or update their
	 * configuration here and attempting to put them into D0 again is
	 * pointless, so avoid doing that.
	 */
	if (!(skip_bus_pm && pm_suspend_no_platform()))	/* [한국어] D0 로 남겨 둔 채 플랫폼 개입 없이 절전했던 경우가 아니라면 */
		pci_pm_default_resume_early(pci_dev);		/* [한국어] D0 로 올리고 저장해 둔 config 를 되쓴다 */

	pci_fixup_device(pci_fixup_resume_early, pci_dev);	/* [한국어] resume_early quirk 실행 */
	pcie_pme_root_status_cleanup(pci_dev);	/* [한국어] 루트 포트라면 PME Status 찌꺼기를 지운다 */

	if (!skip_bus_pm && prev_state == PCI_D3cold)	/* [한국어] 버스 전원 관리를 건너뛴 것도 아니고 D3cold 에서 올라온 것이라면 */
		pci_pm_bridge_power_up_actions(pci_dev);	/* [한국어] 하위 링크가 서기를 기다리고 아래 계층을 깨운다 */

	if (pci_has_legacy_pm_support(pci_dev))	/* [한국어] 레거시 드라이버면 */
		return 0;	/* [한국어] noirq 짝이 없으므로 여기서 끝 */

	if (pm && pm->resume_noirq)	/* [한국어] 드라이버가 resume_noirq 를 주었다면 */
		return pm->resume_noirq(dev);		/* [한국어] 인터럽트 재개 전 드라이버 콜백. config 는 위에서 이미 복원해 두었다 */

	return 0;	/* [한국어] 그 외에는 성공 */
}

/* [한국어]
 * pci_pm_resume_early - resume_noirq 와 resume 사이의 중간 단계
 *
 * @dev: 대상 장치.  @return: 0 또는 드라이버 .resume_early 의 반환값.
 *
 * pci_pm_suspend_late() 의 짝이다. 건너뛸 장치가 아니면 드라이버 콜백을
 * pm_generic_resume_early() 로 부르는 것이 전부다. PCI 계층이 이 단계에서
 * 따로 할 일은 없다 — quirk 도 resume_early 것은 pci_pm_resume_noirq() 가
 * 이미 돌렸다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 인터럽트가 살아난 뒤다.
 *
 * 에러 경로: 드라이버 콜백 실패를 그대로 전달한다.
 *
 * 호출 체인:
 *   PM 코어 → [이 함수] → pm_generic_resume_early()
 */
static int pci_pm_resume_early(struct device *dev)
{
	if (dev_pm_skip_resume(dev))	/* [한국어] 복귀를 건너뛸 장치면 */
		return 0;	/* [한국어] 아무것도 하지 않는다 */

	return pm_generic_resume_early(dev);	/* [한국어] PCI 계층이 이 단계에서 할 일이 없어 드라이버 콜백만 부른다 */
}

/* [한국어]
 * pci_pm_resume - 시스템 복귀의 마지막 단계
 *
 * @dev: 대상 장치.  @return: 0 또는 드라이버 .resume 의 반환값.
 *
 *   (1) state_saved 가 아직 참이면 pci_restore_standard_config() 를 부른다.
 *       원본 주석이 이유를 밝힌다 — 절전이 실패해 되감기는 경우, noirq
 *       복원 단계를 거치지 않고 곧장 여기로 올 수 있다. 그때는 config 가
 *       복원되지 않은 상태이므로 여기서라도 해야 한다.
 *   (2) pci_resume_ptm() — 절전 때 껐던 PTM 을 다시 켠다.
 *   (3) 레거시면 그쪽으로.
 *   (4) pci_pm_default_resume() — quirk 실행과 웨이크업 해제.
 *   (5) 드라이버 .resume 이 있으면 부르고, dev_pm_ops 자체가 없으면
 *       pci_pm_reenable_device() 로 최소한의 되살리기를 한다.
 *
 * (5)의 구조가 조금 특이하다. pm 은 있는데 pm->resume 이 NULL 이면
 * 아무것도 하지 않고 0 을 돌려준다 — 드라이버가 dev_pm_ops 를 주었다는
 * 것 자체를 "내가 알아서 한다" 는 선언으로 보기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 인터럽트 살아 있음.
 *
 * 에러 경로: 드라이버 콜백 실패를 그대로 전달한다.
 *
 * 호출 체인:
 *   PM 코어 → [이 함수] → pci_restore_standard_config(),
 *   pci_pm_default_resume(), pm->resume()
 */
static int pci_pm_resume(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버의 dev_pm_ops */

	/*
	 * This is necessary for the suspend error path in which resume is
	 * called without restoring the standard config registers of the device.
	 */
	if (pci_dev->state_saved)	/* [한국어] 저장된 채로 남아 있다 = noirq 복원을 거치지 않고 왔다(절전 실패 되감기) */
		pci_restore_standard_config(pci_dev);	/* [한국어] 그러면 여기서라도 D0 로 올리고 config 를 복원한다 */

	pci_resume_ptm(pci_dev);	/* [한국어] 절전 때 껐던 PTM 을 다시 켠다 */

	if (pci_has_legacy_pm_support(pci_dev))	/* [한국어] 레거시 드라이버면 */
		return pci_legacy_resume(dev);	/* [한국어] 그쪽 경로로 */

	pci_pm_default_resume(pci_dev);	/* [한국어] quirk 실행과 웨이크업 해제 */

	if (pm) {	/* [한국어] dev_pm_ops 를 가진 드라이버면 */
		if (pm->resume)	/* [한국어] resume 콜백이 있을 때만 부른다. 없으면 아무것도 하지 않는다 — dev_pm_ops 를 주었다는 것 자체가 "내가 알아서 한다" 는 선언이다 */
			return pm->resume(dev);			/* [한국어] 드라이버의 시스템 resume 콜백. 여기서 장치를 다시 동작 상태로 만든다 */
	} else {	/* [한국어] dev_pm_ops 자체가 없으면 */
		pci_pm_reenable_device(pci_dev);	/* [한국어] 최소한의 되살리기(enable + Bus Master)를 대신 해 준다 */
	}

	return 0;	/* [한국어] 성공 */
}

#else /* !CONFIG_SUSPEND */
/* [한국어] 서스펜드(S3 등)를 빌드하지 않는 커널용. 위와 같은 이유로 여섯 개
 * 이름을 NULL 로 만든다. 하이버네이션(freeze/thaw/poweroff/restore)은 별도
 * 옵션이라 여기서 건드리지 않는다. */
#define pci_pm_suspend		NULL	/* [한국어] */
#define pci_pm_suspend_late	NULL	/* [한국어] */
#define pci_pm_suspend_noirq	NULL	/* [한국어] */
#define pci_pm_resume		NULL	/* [한국어] */
#define pci_pm_resume_early	NULL	/* [한국어] */
#define pci_pm_resume_noirq	NULL	/* [한국어] */

#endif /* !CONFIG_SUSPEND */

#ifdef CONFIG_HIBERNATE_CALLBACKS	/* [한국어] 이하는 하이버네이션(최대 절전)을 빌드할 때만 컴파일되는 구간 */

/* [한국어]
 * pci_pm_freeze - 최대 절전(하이버네이션)의 이미지 뜨기 직전 단계
 *
 * @dev: 대상 장치.  @return: 0 또는 드라이버 .freeze 의 반환값.
 *
 * 하이버네이션은 절전이 두 국면으로 나뉜다. 먼저 메모리 이미지를 만들어
 * 디스크에 쓰고(freeze/thaw), 그다음 실제로 전원을 내린다(poweroff).
 * freeze 는 그 첫 국면 — "이미지를 뜨는 동안 장치가 메모리를 건드리지
 * 못하게 하라" 가 목적이지 전원을 내리는 것이 목적이 아니다.
 *
 * 그래서 pci_pm_suspend() 과 비교하면 빠진 것이 있다. PTM 을 끄지 않고,
 * 웨이크업 설정(pci_dev_adjust_pme)도 조정하지 않는다. 잠깐 멈췄다가
 * 곧 다시 쓸 것이므로 그럴 이유가 없다.
 *
 * 대신 런타임 절전된 장치는 무조건 깨운다. 원본 주석이 이유를 설명한다 —
 * 이미지를 읽어 되살리는 쪽 커널(restore kernel)이 런타임 절전 상태를
 * 일관되게 다루리라 기대할 수 없고, 어차피 시스템 복귀 시에는 런타임
 * 활성 상태로 만들어야 하므로, 이미지에 저장되는 상태를 처음부터 그렇게
 * 맞춰 두는 편이 낫다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. CONFIG_HIBERNATE_CALLBACKS 에서만
 * 컴파일된다.
 *
 * 에러 경로: 드라이버 콜백 실패를 그대로 전달한다.
 *
 * 호출 체인:
 *   PM 코어 → [이 함수] → pm->freeze() 또는 pci_legacy_suspend(PMSG_FREEZE)
 */
static int pci_pm_freeze(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버의 dev_pm_ops */

	if (pci_has_legacy_pm_support(pci_dev))	/* [한국어] 레거시 드라이버면 */
		return pci_legacy_suspend(dev, PMSG_FREEZE);	/* [한국어] PMSG_FREEZE 로 "이미지 뜨기 직전 정지" 임을 알린다 */

	if (!pm) {	/* [한국어] dev_pm_ops 가 없으면 */
		pci_pm_default_suspend(pci_dev);	/* [한국어] 브리지가 아닌 장치를 disable */
		if (!pm_runtime_suspended(dev))	/* [한국어] 런타임 절전 상태가 아니라면(즉 깨어 있었다면) */
			pci_dev->state_saved = false;	/* [한국어] 저장분을 무효로 만들어 아래 단계에서 다시 저장하게 한다 */
		return 0;	/* [한국어] 더 할 일 없음 */
	}

	/*
	 * Resume all runtime-suspended devices before creating a snapshot
	 * image of system memory, because the restore kernel generally cannot
	 * be expected to always handle them consistently and they need to be
	 * put into the runtime-active metastate during system resume anyway,
	 * so it is better to ensure that the state saved in the image will be
	 * always consistent with that.
	 */
	pm_runtime_resume(dev);	/* [한국어] 런타임 절전된 장치를 모두 깨운다. 이미지에 담길 상태를 일관되게 만들기 위해서다 */
	pci_dev->state_saved = false;	/* [한국어] 깨웠으니 저장분도 다시 뜬다 */

	if (pm->freeze) {	/* [한국어] 드라이버가 freeze 콜백을 주었다면 */
		int error;	/* [한국어] 드라이버 반환값 */

		error = pm->freeze(dev);	/* [한국어] 부른다 */
		suspend_report_result(dev, pm->freeze, error);	/* [한국어] 실패 시 로그 */
		if (error)	/* [한국어] 실패 검사 */
			return error;	/* [한국어] 그대로 올려보낸다 */
	}

	return 0;	/* [한국어] 성공 */
}

/* [한국어]
 * pci_pm_freeze_noirq - freeze 의 noirq 짝. 상태를 저장하되 전원은 내리지 않는다
 *
 * @dev: 대상 장치.  @return: 0 또는 드라이버 .freeze_noirq 의 반환값.
 *
 * pci_pm_suspend_noirq() 와 비교하면 차이가 분명하다. 저장은 하지만
 * pci_prepare_to_sleep() 을 부르지 않는다 — 곧 이미지를 뜨고 다시 깨어날
 * 것이므로 전원을 내릴 이유가 없다.
 *
 * 저장을 하는 이유는 이미지에 그 저장분이 함께 들어가야 하기 때문이다.
 * 나중에 복원 커널이 이미지를 읽어 올리면 pci_pm_restore_noirq() 가 그
 * 저장분으로 하드웨어를 되돌린다.
 *
 * pci_pm_set_unknown_state() 는 부른다. 이미지를 뜨는 동안이나 그 뒤에
 * 무슨 일이 생길지 모르니 상태를 안다고 가정하지 않는다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 상태.
 *
 * 에러 경로: 드라이버 콜백 실패를 그대로 전달한다.
 *
 * 호출 체인:
 *   PM 코어 → [이 함수] → pm->freeze_noirq(), pci_save_state()
 */
static int pci_pm_freeze_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버의 dev_pm_ops */

	if (pci_has_legacy_pm_support(pci_dev))	/* [한국어] 레거시 드라이버면 */
		return pci_legacy_suspend_late(dev);	/* [한국어] 레거시 마무리 경로로 */

	if (pm && pm->freeze_noirq) {	/* [한국어] 드라이버가 freeze_noirq 를 주었다면 */
		int error;	/* [한국어] 드라이버 반환값 */

		error = pm->freeze_noirq(dev);	/* [한국어] 부른다 */
		suspend_report_result(dev, pm->freeze_noirq, error);	/* [한국어] 실패 시 로그 */
		if (error)	/* [한국어] 실패 검사 */
			return error;	/* [한국어] 그대로 올려보낸다 */
	}

	if (!pci_dev->state_saved)	/* [한국어] 아직 저장이 안 되어 있다면 */
		pci_save_state(pci_dev);	/* [한국어] 저장한다. 이 저장분이 하이버네이션 이미지에 담긴다 */

	pci_pm_set_unknown_state(pci_dev);	/* [한국어] 전원 상태를 모른다고 표시. 다만 pci_prepare_to_sleep 은 부르지 않는다 — 곧 다시 쓸 것이므로 전원을 내리지 않는다 */

	return 0;	/* [한국어] 성공 */
}

/* [한국어]
 * pci_pm_thaw_noirq - freeze 를 되돌리는 noirq 단계
 *
 * @dev: 대상 장치.  @return: 0 또는 드라이버 .thaw_noirq 의 반환값.
 *
 * 이미지를 다 뜬 뒤(또는 복원 커널에서) 장치를 다시 쓸 수 있게 만든다.
 *
 * 다른 복귀 경로와 달리 조건 없이 pci_pm_power_up_and_verify_state() 와
 * pci_restore_state() 를 부른다는 점이 특징이다. 원본 주석이 두 가지
 * 이유를 댄다. 첫째, 드라이버의 .thaw_noirq 는 장치가 D0 이고 config 가
 * 복원된 상태를 전제한다. 둘째, pci_restore_state() 는 MMIO 공간에 있는
 * MSI-X 상태까지 복원하는데 그러려면 장치가 D0 여야 한다 — 드라이버의
 * freeze 콜백이 장치를 저전력으로 내려놓았을 수 있으므로 먼저 올린다.
 *
 * 레거시 드라이버면 위 두 가지만 하고 끝낸다. 레거시에는 thaw 계열
 * 콜백 자체가 없기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 상태.
 *
 * 에러 경로: 드라이버 콜백 실패를 그대로 전달한다.
 *
 * 호출 체인:
 *   PM 코어 → [이 함수] → pci_pm_power_up_and_verify_state(),
 *   pci_restore_state(), pm->thaw_noirq()
 */
static int pci_pm_thaw_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버의 dev_pm_ops */

	/*
	 * The pm->thaw_noirq() callback assumes the device has been
	 * returned to D0 and its config state has been restored.
	 *
	 * In addition, pci_restore_state() restores MSI-X state in MMIO
	 * space, which requires the device to be in D0, so return it to D0
	 * in case the driver's "freeze" callbacks put it into a low-power
	 * state.
	 */
	pci_pm_power_up_and_verify_state(pci_dev);	/* [한국어] 조건 없이 D0 로 올린다. 드라이버의 freeze 콜백이 저전력으로 내려놓았을 수 있기 때문이다 */
	pci_restore_state(pci_dev);	/* [한국어] 조건 없이 config 를 복원한다. MSI-X 상태가 MMIO 에 있어 D0 가 전제다 */

	if (pci_has_legacy_pm_support(pci_dev))	/* [한국어] 레거시 드라이버면 */
		return 0;	/* [한국어] thaw 계열 콜백이 없으므로 여기서 끝 */

	if (pm && pm->thaw_noirq)	/* [한국어] 드라이버가 thaw_noirq 를 주었다면 */
		return pm->thaw_noirq(dev);	/* [한국어] 부르고 그 값을 그대로 돌려준다 */

	return 0;	/* [한국어] 그 외에는 성공 */
}

/* [한국어]
 * pci_pm_thaw - freeze 를 되돌리는 마지막 단계
 *
 * @dev: 대상 장치.  @return: 0 또는 드라이버 .thaw 의 반환값.
 *
 * pci_pm_resume() 과 비슷하지만 훨씬 짧다. config 복원은 thaw_noirq 가
 * 이미 했고, PTM 이나 quirk 처리는 freeze 경로에서 애초에 하지 않았으니
 * 되돌릴 것도 없다. 남은 것은 드라이버 콜백을 부르는 일뿐이다.
 *
 * dev_pm_ops 가 아예 없으면 pci_pm_reenable_device() 로 최소한의
 * 되살리기를 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 인터럽트 살아 있음.
 *
 * 에러 경로: 드라이버 콜백의 반환값을 그대로 전달한다.
 *
 * 호출 체인:
 *   PM 코어 → [이 함수] → pm->thaw() 또는 pci_legacy_resume()
 */
static int pci_pm_thaw(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버의 dev_pm_ops */
	int error = 0;	/* [한국어] 드라이버 반환값을 담을 자리. 콜백이 없으면 0 그대로 나간다 */

	if (pci_has_legacy_pm_support(pci_dev))	/* [한국어] 레거시 드라이버면 */
		return pci_legacy_resume(dev);	/* [한국어] 레거시 복귀 경로로 */

	if (pm) {	/* [한국어] dev_pm_ops 가 있으면 */
		if (pm->thaw)	/* [한국어] thaw 콜백이 있을 때만 */
			error = pm->thaw(dev);	/* [한국어] 부른다 */
	} else {	/* [한국어] dev_pm_ops 자체가 없으면 */
		pci_pm_reenable_device(pci_dev);	/* [한국어] 최소한의 되살리기 */
	}

	return error;	/* [한국어] 드라이버 결과 또는 0 */
}

/* [한국어]
 * pci_pm_poweroff - 하이버네이션 둘째 국면. 실제로 전원을 내리기 직전
 *
 * @dev: 대상 장치.  @return: 0 또는 드라이버 .poweroff 의 반환값.
 *
 * 이미지를 디스크에 다 쓴 뒤, 이제 진짜로 시스템을 끄기 직전에 불린다.
 * 그래서 pci_pm_freeze() 가 아니라 pci_pm_suspend() 을 닮았다 —
 * 런타임 절전 장치를 깨울지 판정하는 부분(smart suspend / need_resume)이
 * 그대로 들어 있고, 원본도 "pci_pm_suspend() 과 같은 이유" 라고만 적어
 * 두었다.
 *
 * 다만 PTM 은 끄지 않는다. 곧 전원이 완전히 나갈 것이므로 링크의 저전력
 * 상태를 신경 쓸 필요가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 에러 경로: 드라이버 콜백 실패를 그대로 전달한다.
 *
 * 호출 체인:
 *   PM 코어 → [이 함수] → pm->poweroff() 또는
 *   pci_legacy_suspend(PMSG_HIBERNATE)
 */
static int pci_pm_poweroff(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버의 dev_pm_ops */

	if (pci_has_legacy_pm_support(pci_dev))	/* [한국어] 레거시 드라이버면 */
		return pci_legacy_suspend(dev, PMSG_HIBERNATE);	/* [한국어] PMSG_HIBERNATE 로 "전원 차단 직전" 임을 알린다 */

	if (!pm) {	/* [한국어] dev_pm_ops 가 없으면 */
		pci_pm_default_suspend(pci_dev);	/* [한국어] 브리지가 아닌 장치를 disable */
		return 0;	/* [한국어] 끝 */
	}

	/* The reason to do that is the same as in pci_pm_suspend(). */
	if (!dev_pm_smart_suspend(dev) || pci_dev_need_resume(pci_dev)) {	/* [한국어] pci_pm_suspend 과 같은 판정 — 런타임 절전 장치를 깨울 것인가 */
		pm_runtime_resume(dev);	/* [한국어] 깨운다 */
		pci_dev->state_saved = false;	/* [한국어] 저장분 무효화 */
	} else {	/* [한국어] 깨우지 않기로 했으면 */
		pci_dev_adjust_pme(pci_dev);	/* [한국어] PME 설정만 조정 */
	}

	if (pm->poweroff) {	/* [한국어] 드라이버가 poweroff 콜백을 주었다면 */
		int error;	/* [한국어] 드라이버 반환값 */

		error = pm->poweroff(dev);	/* [한국어] 부른다 */
		suspend_report_result(dev, pm->poweroff, error);	/* [한국어] 실패 시 로그 */
		if (error)	/* [한국어] 실패 검사 */
			return error;	/* [한국어] 그대로 올려보낸다 */
	}

	return 0;	/* [한국어] 성공 */
}

/* [한국어]
 * pci_pm_poweroff_late - poweroff 와 poweroff_noirq 사이의 중간 단계
 *
 * @dev: 대상 장치.  @return: 0 또는 드라이버 .poweroff_late 의 반환값.
 *
 * pci_pm_suspend_late() 와 구조가 같다. 건너뛸 장치가 아니면 suspend
 * quirk 를 돌리고 드라이버 콜백을 부른다. quirk 종류가 같은 이유는
 * poweroff 도 결국 "장치를 끄는" 동작이라 같은 보정이 필요하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 에러 경로: 드라이버 콜백 실패를 그대로 전달한다.
 *
 * 호출 체인:
 *   PM 코어 → [이 함수] → pci_fixup_device(), pm_generic_poweroff_late()
 */
static int pci_pm_poweroff_late(struct device *dev)
{
	if (dev_pm_skip_suspend(dev))	/* [한국어] 건너뛸 장치면 */
		return 0;	/* [한국어] 아무것도 하지 않는다 */

	pci_fixup_device(pci_fixup_suspend, to_pci_dev(dev));	/* [한국어] suspend quirk 실행 — poweroff 도 장치를 끄는 동작이라 같은 보정이 필요하다 */

	return pm_generic_poweroff_late(dev);	/* [한국어] 드라이버 콜백만 부른다 */
}

/* [한국어]
 * pci_pm_poweroff_noirq - 전원 차단 직전의 마지막 PCI 처리
 *
 * @dev: 대상 장치.  @return: 0 또는 드라이버 .poweroff_noirq 의 반환값.
 *
 * pci_pm_suspend_noirq() 의 하이버네이션 판이지만 더 간단하다.
 *
 *   (1) 건너뛰기 판정과 레거시 분기.
 *   (2) dev_pm_ops 가 없으면 quirk 만 돌리고 끝.
 *   (3) 드라이버 .poweroff_noirq 를 부른다.
 *   (4) 저장이 안 되어 있고 브리지가 아니면 pci_prepare_to_sleep() 으로
 *       D-state 를 내린다. 브리지를 제외하는 조건이
 *       pci_pm_suspend_noirq() 의 skip_bus_pm 판정보다 단순하다.
 *   (5) EHCI COMMAND=0 특례. 원본 주석대로 pci_pm_suspend_noirq() 과
 *       같은 이유다.
 *   (6) suspend_late quirk 실행.
 *
 * pci_pm_suspend_noirq() 에 있던 skip_bus_pm 계산과 may_skip_resume
 * 처리가 없는데, 하이버네이션에는 "복귀를 건너뛴다" 는 개념이 없고
 * 어차피 전원이 완전히 나가기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 상태.
 *
 * 에러 경로: 드라이버 콜백 실패를 그대로 전달한다.
 *
 * 호출 체인:
 *   PM 코어 → [이 함수] → pm->poweroff_noirq(), pci_prepare_to_sleep()
 */
static int pci_pm_poweroff_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버의 dev_pm_ops */

	if (dev_pm_skip_suspend(dev))	/* [한국어] 건너뛸 장치면 */
		return 0;	/* [한국어] 끝 */

	if (pci_has_legacy_pm_support(pci_dev))	/* [한국어] 레거시 드라이버면 */
		return pci_legacy_suspend_late(dev);	/* [한국어] 레거시 마무리 경로로 */

	if (!pm) {	/* [한국어] dev_pm_ops 가 없으면 */
		pci_fixup_device(pci_fixup_suspend_late, pci_dev);	/* [한국어] quirk 만 돌리고 */
		return 0;	/* [한국어] 끝낸다 */
	}

	if (pm->poweroff_noirq) {	/* [한국어] 드라이버가 poweroff_noirq 를 주었다면 */
		int error;	/* [한국어] 드라이버 반환값 */

		error = pm->poweroff_noirq(dev);	/* [한국어] 부른다 */
		suspend_report_result(dev, pm->poweroff_noirq, error);	/* [한국어] 실패 시 로그 */
		if (error)	/* [한국어] 실패 검사 */
			return error;	/* [한국어] 그대로 올려보낸다 */
	}

	if (!pci_dev->state_saved && !pci_has_subordinate(pci_dev))	/* [한국어] 저장이 안 되어 있고 브리지가 아니라면 */
		pci_prepare_to_sleep(pci_dev);	/* [한국어] 저전력 D-state 로 내린다. 브리지를 제외하는 조건이 pci_pm_suspend_noirq 의 skip_bus_pm 판정보다 단순하다 */

	/*
	 * The reason for doing this here is the same as for the analogous code
	 * in pci_pm_suspend_noirq().
	 */
	if (pci_dev->class == PCI_CLASS_SERIAL_USB_EHCI)	/* [한국어] EHCI USB 호스트 컨트롤러라면 */
		pci_write_config_word(pci_dev, PCI_COMMAND, 0);	/* [한국어] COMMAND 를 0 으로. pci_pm_suspend_noirq 와 같은 ASUS BIOS 회피다 */

	pci_fixup_device(pci_fixup_suspend_late, pci_dev);	/* [한국어] suspend_late quirk 실행 */

	return 0;	/* [한국어] 성공 */
}

/* [한국어]
 * pci_pm_restore_noirq - 복원 커널이 장치를 처음 만지는 지점
 *
 * @dev: 대상 장치.  @return: 0 또는 드라이버 .restore_noirq 의 반환값.
 *
 * 하이버네이션에서 깨어나는 경로는 특이하다. 부팅한 커널이 디스크의
 * 이미지를 읽어 메모리에 올린 뒤 그쪽으로 제어를 넘기는데, 그 시점의
 * 하드웨어는 방금 전원이 들어온 상태다. 커널이 이미지에서 되살린
 * 자료구조가 기억하는 상태와 실제 하드웨어가 완전히 어긋나 있다.
 *
 * 그래서 조건 없이 pci_pm_default_resume_early() 를 부른다 — D0 로 올리고
 * 저장분으로 config 를 통째로 덮어쓴다. 이어서 resume_early quirk 를
 * 돌리고 드라이버 콜백을 부른다.
 *
 * pci_pm_resume_noirq() 에 있는 skip_bus_pm 최적화나 브리지 링크 대기가
 * 여기엔 없다. 방금 전원이 들어온 상황이라 그런 최적화의 전제가 아예
 * 성립하지 않기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트가 꺼진 상태.
 *
 * 에러 경로: 드라이버 콜백 실패를 그대로 전달한다.
 *
 * 호출 체인:
 *   PM 코어 → [이 함수] → pci_pm_default_resume_early(), pm->restore_noirq()
 */
static int pci_pm_restore_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버의 dev_pm_ops */

	pci_pm_default_resume_early(pci_dev);	/* [한국어] 조건 없이 D0 복귀 + config 복원. 방금 전원이 들어온 상태라 하드웨어와 커널의 인식이 완전히 어긋나 있다 */
	pci_fixup_device(pci_fixup_resume_early, pci_dev);	/* [한국어] resume_early quirk 실행 */

	if (pci_has_legacy_pm_support(pci_dev))	/* [한국어] 레거시 드라이버면 */
		return 0;	/* [한국어] 여기서 끝 */

	if (pm && pm->restore_noirq)	/* [한국어] 드라이버가 restore_noirq 를 주었다면 */
		return pm->restore_noirq(dev);	/* [한국어] 부르고 그 값을 돌려준다 */

	return 0;	/* [한국어] 그 외에는 성공 */
}

/* [한국어]
 * pci_pm_restore - 하이버네이션 복원의 마지막 단계
 *
 * @dev: 대상 장치.  @return: 0 또는 드라이버 .restore 의 반환값.
 *
 * pci_pm_resume() 과 거의 같은 구조다. state_saved 가 남아 있으면
 * (복원 경로가 중간에 실패해 되감기는 중이라는 뜻) config 를 복원하고,
 * quirk 를 돌린 뒤 드라이버 .restore 를 부른다.
 *
 * 차이는 pci_resume_ptm() 이 없다는 것뿐이다. poweroff 경로에서 PTM 을
 * 끄지 않았으므로 다시 켤 것도 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 인터럽트 살아 있음.
 *
 * 에러 경로: 드라이버 콜백 실패를 그대로 전달한다.
 *
 * 호출 체인:
 *   PM 코어 → [이 함수] → pci_restore_standard_config(),
 *   pci_pm_default_resume(), pm->restore()
 */
static int pci_pm_restore(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버의 dev_pm_ops */

	/*
	 * This is necessary for the hibernation error path in which restore is
	 * called without restoring the standard config registers of the device.
	 */
	if (pci_dev->state_saved)	/* [한국어] 저장된 채로 남아 있다 = 복원 경로가 중간에 실패해 되감기는 중이다 */
		pci_restore_standard_config(pci_dev);	/* [한국어] 그러면 여기서라도 복원한다 */

	if (pci_has_legacy_pm_support(pci_dev))	/* [한국어] 레거시 드라이버면 */
		return pci_legacy_resume(dev);	/* [한국어] 레거시 복귀 경로로 */

	pci_pm_default_resume(pci_dev);	/* [한국어] quirk 실행과 웨이크업 해제 */

	if (pm) {	/* [한국어] dev_pm_ops 가 있으면 */
		if (pm->restore)	/* [한국어] restore 콜백이 있을 때만 */
			return pm->restore(dev);	/* [한국어] 부르고 그 값을 돌려준다 */
	} else {	/* [한국어] dev_pm_ops 자체가 없으면 */
		pci_pm_reenable_device(pci_dev);	/* [한국어] 최소한의 되살리기 */
	}

	return 0;	/* [한국어] 성공 */
}

#else /* !CONFIG_HIBERNATE_CALLBACKS */
/* [한국어] 하이버네이션을 빌드하지 않는 커널용. freeze/thaw/poweroff/restore
 * 아홉 개 이름을 NULL 로 만든다. 서스펜드와 별개의 옵션이라는 점에 주의 —
 * 서스펜드만 쓰는 임베디드 커널이 흔하다. */
#define pci_pm_freeze		NULL	/* [한국어] */
#define pci_pm_freeze_noirq	NULL	/* [한국어] */
#define pci_pm_thaw		NULL	/* [한국어] */
#define pci_pm_thaw_noirq	NULL	/* [한국어] */
#define pci_pm_poweroff		NULL	/* [한국어] */
#define pci_pm_poweroff_late	NULL	/* [한국어] */
#define pci_pm_poweroff_noirq	NULL	/* [한국어] */
#define pci_pm_restore		NULL	/* [한국어] */
#define pci_pm_restore_noirq	NULL	/* [한국어] */

#endif /* !CONFIG_HIBERNATE_CALLBACKS */

#ifdef CONFIG_PM	/* [한국어] 이하는 런타임 전원 관리 구간(시스템 절전과 별개의 CONFIG_PM) */

/* [한국어]
 * pci_pm_runtime_suspend - 유휴 장치를 사용자 몰래 재우는 경로
 *
 * @dev: 대상 장치.  @return: 0 성공. -EBUSY/-EAGAIN 은 "지금은 곤란하니
 *   나중에 다시 시도하라" 는 뜻이고, 그 밖의 음수는 진짜 실패다.
 *
 * 시스템 절전과 완전히 별개의 계열이다. 사용자가 아무것도 하지 않았는데
 * 런타임 PM 코어가 "이 장치는 한동안 놀고 있다" 고 판단해 부른다.
 *
 *   (1) pci_suspend_ptm() — PTM 을 끈다.
 *   (2) 드라이버가 붙어 있지 않은 장치는 D0 에 그대로 둔다. 다만 원본
 *       주석대로 위 브리지가 런타임 절전하면 덩달아 D3cold 로 떨어질 수
 *       있으므로 config 는 저장해 둔다.
 *   (3) 드라이버 .runtime_suspend 를 부른다. -EBUSY/-EAGAIN 은 흔한
 *       정상 상황이라 pci_dbg 로만 남기고, 그 외 실패는 pci_err 로 남긴다.
 *       이 구분이 있는 이유는, 재시도 요청은 초당 여러 번 일어날 수 있어
 *       err 로 찍으면 로그가 넘치기 때문이다.
 *   (4) suspend quirk 실행.
 *   (5) 드라이버가 저전력으로 내려놓고 저장을 안 했으면 경고하고 그만둔다.
 *   (6) 아직 저장이 안 되어 있으면 저장하고 pci_finish_runtime_suspend()
 *       로 최종 D-state 전환과 웨이크업 설정을 맡긴다.
 *
 * prev 를 함수 앞에서 챙겨 두는 이유는 (5)의 경고 조건이 "드라이버가
 * 상태를 바꿨는가" 를 따지기 때문이다.
 *
 * 실행 컨텍스트: 런타임 PM 코어의 워크큐 또는 put 을 부른 스레드.
 * 프로세스 컨텍스트이고 잠들 수 있다. CONFIG_PM 에서 컴파일된다.
 *
 * 에러 경로: 위 반환값 설명대로. 실패하면 런타임 PM 코어가 장치를 활성
 * 상태로 되돌린다.
 *
 * 호출 체인:
 *   런타임 PM 코어 → [이 함수] → pm->runtime_suspend(),
 *   pci_save_state(), pci_finish_runtime_suspend()
 */
static int pci_pm_runtime_suspend(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버의 dev_pm_ops */
	pci_power_t prev = pci_dev->current_state;	/* [한국어] 드라이버 호출 전 전원 상태. 아래 경고 조건에 쓴다 */
	int error;	/* [한국어] 드라이버 반환값 */

	pci_suspend_ptm(pci_dev);	/* [한국어] PTM 을 끈다 */

	/*
	 * If pci_dev->driver is not set (unbound), we leave the device in D0,
	 * but it may go to D3cold when the bridge above it runtime suspends.
	 * Save its config space in case that happens.
	 */
	if (!pci_dev->driver) {	/* [한국어] 드라이버가 붙어 있지 않은 장치라면 */
		pci_save_state(pci_dev);	/* [한국어] 임자 없는 장치라도 위 브리지가 잠들면 덩달아 D3cold 로 떨어져 config 가 날아간다. 그때를 대비한 저장 */
		return 0;	/* [한국어] D0 에 그대로 둔다. 실제 판단은 여기서 끝난다 */
	}

	pci_dev->state_saved = false;	/* [한국어] "아직 저장 안 함" 으로 놓고 시작 */
	if (pm && pm->runtime_suspend) {	/* [한국어] 드라이버가 runtime_suspend 를 주었다면 */
		error = pm->runtime_suspend(dev);	/* [한국어] 드라이버의 런타임 절전 콜백. 시스템 절전과 달리 사용자 몰래 수시로 불린다 */
		/*
		 * -EBUSY and -EAGAIN is used to request the runtime PM core
		 * to schedule a new suspend, so log the event only with debug
		 * log level.
		 */
		if (error == -EBUSY || error == -EAGAIN) {	/* [한국어] 재시도 요청은 흔한 정상 상황이다 */
			pci_dbg(pci_dev, "can't suspend now (%ps returned %d)\n",	/* [한국어] 그래서 디버그 수준으로만 남긴다. err 로 찍으면 초당 여러 번 로그가 넘친다 */
				pm->runtime_suspend, error);	/* [한국어] %ps 로 어느 드라이버 콜백인지와 반환값을 함께 */
			return error;	/* [한국어] 런타임 PM 코어가 나중에 다시 시도한다 */
		} else if (error) {	/* [한국어] 그 밖의 실패는 진짜 오류다 */
			pci_err(pci_dev, "can't suspend (%ps returned %d)\n",	/* [한국어] 오류 수준으로 남긴다 */
				pm->runtime_suspend, error);	/* [한국어] 콜백 이름과 반환값 */
			return error;	/* [한국어] 실패를 올려보내면 코어가 장치를 활성 상태로 되돌린다 */
		}
	}

	pci_fixup_device(pci_fixup_suspend, pci_dev);	/* [한국어] suspend quirk 실행 */

	if (pm && pm->runtime_suspend	/* [한국어] 드라이버가 runtime_suspend 를 주었는데 */
	    && !pci_dev->state_saved && pci_dev->current_state != PCI_D0	/* [한국어] 저장은 안 했고 D0 도 아니며 */
	    && pci_dev->current_state != PCI_UNKNOWN) {	/* [한국어] UNKNOWN 도 아니라면 = 저장 없이 저전력으로 내려갔다 */
		pci_WARN_ONCE(pci_dev, pci_dev->current_state != prev,	/* [한국어] 드라이버가 실제로 상태를 바꾼 경우에만 경고 */
			      "PCI PM: State of device not saved by %pS\n",	/* [한국어] 경고 문구 */
			      pm->runtime_suspend);	/* [한국어] 문제의 콜백 지목 */
		return 0;	/* [한국어] 아래의 저장·전환은 소용없으므로 여기서 끝낸다 */
	}

	if (!pci_dev->state_saved) {	/* [한국어] 아직 저장이 안 되어 있다면(드라이버가 직접 D-state 를 다루지 않는 보통의 경우) */
		pci_save_state(pci_dev);	/* [한국어] PCI 계층이 저장하고 */
		pci_finish_runtime_suspend(pci_dev);	/* [한국어] 웨이크업 설정을 걸고 적절한 저전력 D-state(D3hot 또는 D3cold)로 최종 전환한다 */
	}

	return 0;	/* [한국어] 성공 */
}

/* [한국어]
 * pci_pm_runtime_resume - 런타임 절전된 장치를 다시 깨운다
 *
 * @dev: 대상 장치.  @return: 0 또는 드라이버 .runtime_resume 의 반환값.
 *
 *   (1) pci_pm_default_resume_early() 를 조건 없이 부른다. 원본 주석이
 *       이유를 밝힌다 — 드라이버가 붙어 있지 않아 D0 에 두었던 장치라도
 *       위 브리지가 런타임 절전하면서 D3cold 로 떨어졌을 수 있으므로,
 *       바인딩 여부와 무관하게 config 를 복원해야 한다.
 *   (2) pci_resume_ptm() — PTM 을 되살린다.
 *   (3) 드라이버가 없으면 여기서 끝. (1)만 해 주면 된다.
 *   (4) resume_early quirk, pci_pm_default_resume().
 *   (5) D3cold 에서 올라왔다면 브리지 처리(하위 링크 대기 + 하위 깨우기).
 *   (6) 드라이버 .runtime_resume 을 부른다.
 *
 * prev_state 를 앞에서 챙기는 이유는 (1)이 current_state 를 바꾸기
 * 때문이다. (5)의 판정에는 바뀌기 전 값이 필요하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 브리지 링크 대기가 있어 오래 걸릴 수
 * 있고, 그래서 런타임 복귀는 I/O 지연으로 체감된다.
 *
 * 에러 경로: 드라이버 콜백 실패를 그대로 전달한다.
 *
 * 호출 체인:
 *   런타임 PM 코어 → [이 함수] → pci_pm_default_resume_early(),
 *   pci_pm_bridge_power_up_actions(), pm->runtime_resume()
 */
static int pci_pm_runtime_resume(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버의 dev_pm_ops */
	pci_power_t prev_state = pci_dev->current_state;	/* [한국어] 복원 전 전원 상태. 아래 D3cold 판정에 쓰이므로 미리 챙긴다 */
	int error = 0;	/* [한국어] 드라이버 반환값 */

	/*
	 * Restoring config space is necessary even if the device is not bound
	 * to a driver because although we left it in D0, it may have gone to
	 * D3cold when the bridge above it runtime suspended.
	 */
	pci_pm_default_resume_early(pci_dev);	/* [한국어] 드라이버 유무와 무관하게 D0 로 올리고 config 를 복원한다 */
	pci_resume_ptm(pci_dev);	/* [한국어] 절전 때 껐던 PTM 을 되살린다 */

	if (!pci_dev->driver)	/* [한국어] 드라이버가 붙어 있지 않으면 */
		return 0;	/* [한국어] config 복원만 해 주고 끝낸다 */

	pci_fixup_device(pci_fixup_resume_early, pci_dev);	/* [한국어] resume_early quirk 실행 */
	pci_pm_default_resume(pci_dev);	/* [한국어] quirk 실행과 웨이크업 해제 */

	if (prev_state == PCI_D3cold)	/* [한국어] D3cold 에서 올라왔다면 */
		pci_pm_bridge_power_up_actions(pci_dev);	/* [한국어] 하위 링크 대기와 하위 계층 깨우기 */

	if (pm && pm->runtime_resume)	/* [한국어] 드라이버가 runtime_resume 을 주었다면 */
		error = pm->runtime_resume(dev);		/* [한국어] 드라이버의 런타임 복귀 콜백. config 복원은 위에서 이미 끝났다 */

	return error;	/* [한국어] 드라이버 결과 또는 0 */
}

/* [한국어]
 * pci_pm_runtime_idle - 사용 카운트가 0 이 되었을 때 "재워도 되나" 를 묻는 자리
 *
 * @dev: 대상 장치.
 * @return: 0 이면 런타임 PM 코어가 절전을 진행해도 좋다는 뜻.
 *   드라이버 콜백이 다른 값을 돌려주면 그 판단을 따른다.
 *
 * 런타임 PM 은 사용 카운트가 0 이 되는 순간 곧바로 재우지 않고 먼저
 * .runtime_idle 을 물어본다. 드라이버가 "조금 더 기다려라" 거나
 * "나는 재우지 말라" 고 말할 기회를 주는 것이다.
 *
 * 드라이버가 붙어 있지 않은 장치는 0 을 돌려주지만, 그것이 곧 절전으로
 * 이어지지는 않는다. 원본 주석대로 바인딩되지 않은 PCI 장치는 런타임 PM
 * 상태와 무관하게 항상 D0 에 두는 것이 이 계층의 규칙이고, 실제 판단은
 * pci_pm_runtime_suspend() 의 앞부분이 한다.
 *
 * 실행 컨텍스트: 런타임 PM 코어. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 드라이버의 판단을 그대로 전달할 뿐이다.
 *
 * 호출 체인:
 *   런타임 PM 코어 → [이 함수] → pm->runtime_idle()
 */
static int pci_pm_runtime_idle(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;	/* [한국어] 드라이버의 dev_pm_ops */

	/*
	 * If pci_dev->driver is not set (unbound), the device should
	 * always remain in D0 regardless of the runtime PM status
	 */
	if (!pci_dev->driver)	/* [한국어] 드라이버가 붙어 있지 않으면 */
		return 0;	/* [한국어] 0 을 돌려주지만, 실제로 재울지는 pci_pm_runtime_suspend 의 앞부분이 판단한다 */

	if (pm && pm->runtime_idle)	/* [한국어] 드라이버가 runtime_idle 을 주었다면 */
		return pm->runtime_idle(dev);	/* [한국어] 그 판단을 그대로 따른다 */

	return 0;	/* [한국어] 콜백이 없으면 절전을 진행해도 좋다는 뜻의 0 */
}

/* [한국어]
 * pci_dev_pm_ops - PCI 버스가 PM 코어에 등록하는 단계별 콜백 표
 *
 * 커널의 절전은 여러 단계로 쪼개져 있고, 각 단계마다 "버스가 할 일" 과
 * "드라이버가 할 일" 이 나뉜다. 이 표는 그 버스 쪽 절반이다. PM 코어는
 * dev->bus->pm 에서 이 표를 찾아 부르고, 여기 있는 pci_pm_* 함수가 PCI
 * 표준 동작(config space 저장·복원, D-state 전환, PME 설정)을 수행하면서
 * 그 사이사이에 dev->driver->pm 의 같은 이름 콜백을 끼워 넣는다.
 *
 * 설정자: 컴파일 시점 상수. 런타임에 바뀌지 않는다.
 * 읽는 자: drivers/base/power 의 PM 코어. pci_bus_type.pm 에 꽂혀 있다.
 * 값 범위: CONFIG_SUSPEND / CONFIG_HIBERNATE_CALLBACKS 가 꺼지면 해당
 *   슬롯의 이름들이 위에서 NULL 매크로로 바뀌므로 그 칸이 비게 된다.
 * 동기화: PM 코어가 장치 트리 순서(자식 먼저 suspend, 부모 먼저 resume)로
 *   직렬화해 부른다. _noirq 계열은 인터럽트가 꺼진 상태에서 불린다.
 */
static const struct dev_pm_ops pci_dev_pm_ops = {
	.prepare = pci_pm_prepare,		/* [한국어] 절전 시작 통보. 1 을 반환하면 direct-complete(이 장치를 건너뛰기) */
	.complete = pci_pm_complete,		/* [한국어] 사이클 종료 통보. 펌웨어가 장치를 리셋해 놓았으면 여기서 되살린다 */
	.suspend = pci_pm_suspend,		/* [한국어] S3 진입, 인터럽트 살아 있는 단계 */
	.suspend_late = pci_pm_suspend_late,	/* [한국어] S3 진입, suspend 와 noirq 사이 */
	.resume = pci_pm_resume,		/* [한국어] S3 복귀, 인터럽트 살아난 뒤 */
	.resume_early = pci_pm_resume_early,	/* [한국어] S3 복귀, resume_noirq 와 resume 사이 */
	.freeze = pci_pm_freeze,		/* [한국어] 하이버네이션: 메모리 이미지를 뜨기 직전 정지 */
	.thaw = pci_pm_thaw,			/* [한국어] 하이버네이션: 이미지를 뜬 뒤 다시 동작시킴 */
	.poweroff = pci_pm_poweroff,		/* [한국어] 하이버네이션: 이미지를 쓴 뒤 실제 전원 차단 직전 */
	.poweroff_late = pci_pm_poweroff_late,	/* [한국어] poweroff 와 poweroff_noirq 사이 */
	.restore = pci_pm_restore,		/* [한국어] 하이버네이션 복원 커널에서의 되살리기 */
	.suspend_noirq = pci_pm_suspend_noirq,	/* [한국어] 인터럽트 차단 후. config 저장과 D3 전환이 여기서 일어난다 */
	.resume_noirq = pci_pm_resume_noirq,	/* [한국어] 인터럽트 재개 전. D0 복귀와 config 복원이 여기서 */
	.freeze_noirq = pci_pm_freeze_noirq,	/* [한국어] freeze 의 noirq 짝. 상태 저장까지만 하고 전원은 내리지 않는다 */
	.thaw_noirq = pci_pm_thaw_noirq,	/* [한국어] thaw 의 noirq 짝. D0 복귀 + config 복원 */
	.poweroff_noirq = pci_pm_poweroff_noirq, /* [한국어] poweroff 의 noirq 짝. 여기서 D3 로 내린다 */
	.restore_noirq = pci_pm_restore_noirq,	/* [한국어] restore 의 noirq 짝. 복원 커널이 처음 장치를 만지는 지점 */
	.runtime_suspend = pci_pm_runtime_suspend, /* [한국어] 유휴 시 자동 절전. 시스템 절전과 독립된 계열 */
	.runtime_resume = pci_pm_runtime_resume, /* [한국어] 접근이 생겨 깨울 때 */
	.runtime_idle = pci_pm_runtime_idle,	/* [한국어] 사용 카운트가 0 이 되었을 때 "지금 재워도 되나" 를 묻는 자리 */
};

#define PCI_PM_OPS_PTR	(&pci_dev_pm_ops)	/* [한국어] pci_bus_type.pm 에 꽂을 값. CONFIG_PM 이 켜져 있을 때 */

#else /* !CONFIG_PM */

#define pci_pm_runtime_suspend	NULL	/* [한국어] 런타임 절전 자체가 없는 빌드 */
#define pci_pm_runtime_resume	NULL	/* [한국어] */
#define pci_pm_runtime_idle	NULL	/* [한국어] */

#define PCI_PM_OPS_PTR	NULL	/* [한국어] 전원 관리 표를 아예 달지 않는다 */

#endif /* !CONFIG_PM */

/**
 * __pci_register_driver - register a new pci driver
 * @drv: the driver structure to register
 * @owner: owner module of drv
 * @mod_name: module name string
 *
 * Adds the driver structure to the list of registered drivers.
 * Returns a negative value on error, otherwise 0.
 * If no error occurred, the driver remains registered even if
 * no device was claimed during registration.
 */
/* [한국어]
 * __pci_register_driver - PCI 드라이버를 커널에 등록한다
 *
 * @drv: 등록할 struct pci_driver. 호출자가 정적으로 정의해 둔 것이다.
 * @owner: 이 드라이버를 담고 있는 모듈. pci_register_driver 매크로가
 *   THIS_MODULE 을 넣어 준다. 장치가 바인딩되어 있는 동안 모듈이
 *   언로드되지 않게 하는 참조 계산에 쓰인다.
 * @mod_name: 모듈 이름 문자열. 매크로가 KBUILD_MODNAME 을 넣는다.
 * @return: 0 성공, 음수 errno 실패(driver_register 의 반환값).
 *
 * 이름 앞의 밑줄 두 개는 "직접 부르지 말라" 는 표시다. 드라이버는
 * pci_register_driver(drv) 매크로를 쓰고, 그것이 뒤의 두 인자를 채워
 * 이 함수를 부른다.
 *
 * 하는 일은 두 가지다. 첫째, struct pci_driver 안에 박혀 있는 일반
 * struct device_driver 의 필드들을 채운다 — 이름, 소속 버스(pci_bus_type),
 * 모듈, sysfs 속성군. 이 "채워 넣기" 가 곧 PCI 드라이버를 커널 드라이버
 * 모델에 편입시키는 작업이다. 둘째, dynids 목록의 락과 헤드를 초기화한다.
 * 그다음 driver_register() 로 코어에 넘기면, 코어가 이미 등록되어 있는
 * PCI 장치들을 훑으며 pci_bus_type.match 를 불러 짝을 찾는다.
 *
 * 반환값이 0 이면 등록에 성공한 것이고, 원본 주석대로 등록 시점에 맡을
 * 장치를 하나도 찾지 못했더라도 드라이버는 등록된 채로 남는다. 나중에
 * 핫플러그로 장치가 나타나면 그때 바인딩된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 보통 모듈 init. driver_register 가
 * 그 안에서 probe 까지 돌릴 수 있으므로 오래 걸릴 수 있다.
 *
 * 에러 경로: driver_register 의 실패를 그대로 올려보낸다. 그 경우 모듈
 * init 이 실패하고 모듈이 언로드된다.
 *
 * 호출 체인:
 *   모듈 init → pci_register_driver 매크로 → [이 함수] → driver_register()
 *   → 코어가 pci_bus_match() → pci_device_probe() → drv->probe()
 */
int __pci_register_driver(struct pci_driver *drv, struct module *owner,
			  const char *mod_name)
{
	/* initialize common driver fields */
	drv->driver.name = drv->name;	/* [한국어] sysfs 에 보일 드라이버 이름 */
	drv->driver.bus = &pci_bus_type;	/* [한국어] 소속 버스를 PCI 로 못 박는다. 이 한 줄이 "이 드라이버는 PCI 장치를 맡는다" 는 선언이다 */
	drv->driver.owner = owner;	/* [한국어] 모듈 참조 계산용. 장치가 바인딩된 동안 모듈이 언로드되지 않게 한다 */
	drv->driver.mod_name = mod_name;	/* [한국어] 모듈 이름 문자열 */
	drv->driver.groups = drv->groups;	/* [한국어] 드라이버 자신에게 붙일 추가 sysfs 속성군(있으면) */
	drv->driver.dev_groups = drv->dev_groups;	/* [한국어] 이 드라이버가 맡는 장치들에 붙일 추가 sysfs 속성군(있으면) */

	spin_lock_init(&drv->dynids.lock);	/* [한국어] 동적 ID 목록을 보호할 스핀락 초기화 */
	INIT_LIST_HEAD(&drv->dynids.list);	/* [한국어] 동적 ID 목록 헤드 초기화. 이 두 줄이 없으면 첫 new_id 쓰기에서 터진다 */

	/* register with core */
	return driver_register(&drv->driver);	/* [한국어] 드라이버 코어에 넘긴다. 이 안에서 기존 장치들과의 매칭과 probe 가 돌 수 있다 */
}
EXPORT_SYMBOL(__pci_register_driver);	/* [한국어] 모든 PCI 드라이버 모듈이 pci_register_driver 매크로를 통해 이 심볼을 쓴다 */

/**
 * pci_unregister_driver - unregister a pci driver
 * @drv: the driver structure to unregister
 *
 * Deletes the driver structure from the list of registered PCI drivers,
 * gives it a chance to clean up by calling its remove() function for
 * each device it was responsible for, and marks those devices as
 * driverless.
 */

/* [한국어]
 * pci_unregister_driver - PCI 드라이버를 커널에서 뺀다
 *
 * @drv: 해제할 드라이버.  @return: 없음.
 *
 * driver_unregister() 가 이 드라이버에 바인딩된 모든 장치를 찾아
 * 언바인딩한다. 그 과정에서 장치마다 pci_bus_type.remove 즉
 * pci_device_remove() 가 불리고, 그 안에서 drv->remove() 가 실행된다.
 * 원본 주석이 말하는 "정리할 기회를 준다" 가 그것이다. 그 뒤 장치들은
 * 드라이버 없는 상태로 남는다.
 *
 * 그다음 pci_free_dynids() 로 sysfs new_id 로 붙였던 동적 ID 들을 해제한다.
 * 순서가 중요하다 — 먼저 언바인딩을 끝내야 그 ID 를 참조하는 쪽이 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 보통 모듈 exit. remove 콜백이 도는
 * 동안 잠들 수 있으므로 오래 걸릴 수 있다.
 *
 * 에러 경로: 없다. 언바인딩은 실패할 수 없다.
 *
 * 호출 체인:
 *   모듈 exit → [이 함수] → driver_unregister() → pci_device_remove()
 *                        → pci_free_dynids()
 */
void pci_unregister_driver(struct pci_driver *drv)
{
	driver_unregister(&drv->driver);
	pci_free_dynids(drv);
}
EXPORT_SYMBOL(pci_unregister_driver);	/* [한국어] 짝이 되는 해제 함수도 함께 공개 */

/* [한국어]
 * pci_compat_driver - 이름만 있는 자리 표시용 가짜 드라이버
 *
 * pci_dev_driver() 는 "이 장치를 누가 쓰고 있는가" 를 묻는 오래된 API 다.
 * 정식 드라이버가 붙어 있지 않은데도 BAR 자원이 IORESOURCE_BUSY 로
 * 잡혀 있는 장치가 있다 — 예컨대 옛날 방식으로 request_region() 만 해서
 * 자원을 선점한 코드가 그렇다. 그런 경우 NULL 을 돌려주면 호출자가
 * "임자 없음" 으로 오해하므로, 이름이 "compat" 인 껍데기를 대신 돌려준다.
 *
 * 설정자: 컴파일 시점 상수. probe/remove 같은 콜백이 하나도 없다.
 * 읽는 자: pci_dev_driver() 뿐이다.
 * 값 범위: .name 만 채워져 있고 나머지는 0/NULL 이다.
 * 동기화: 읽기만 하므로 락이 없다.
 */
static struct pci_driver pci_compat_driver = {
	.name = "compat"	/* [한국어] 이 이름이 곧 "정식 드라이버는 없다" 는 표시다 */
};

/**
 * pci_dev_driver - get the pci_driver of a device
 * @dev: the device to query
 *
 * Returns the appropriate pci_driver structure or %NULL if there is no
 * registered driver for the device.
 */
/* [한국어]
 * pci_dev_driver - 이 장치를 지금 누가 쓰고 있는가
 *
 * @dev: 물어볼 장치.
 * @return: 바인딩된 pci_driver, 없지만 자원이 선점되어 있으면
 *   &pci_compat_driver, 정말 아무도 안 쓰면 NULL.
 *
 * 두 번째 경우가 이 함수의 존재 이유다. 정식 드라이버 없이 자원만
 * 선점해 둔 옛 방식 코드가 있는데, 그런 장치에 NULL 을 돌려주면 호출자가
 * "임자 없음" 으로 오해해 장치를 가져가려 할 수 있다. 그래서 이름만 있는
 * 껍데기를 대신 돌려준다.
 *
 * 판정은 자원 배열을 PCI_ROM_RESOURCE 까지 훑으며 IORESOURCE_BUSY 비트를
 * 찾는 것이다. 그 범위는 표준 BAR 여섯 개(0~5)에 확장 ROM BAR 하나를
 * 더한 것으로, PCI 규격의 타입 0 헤더가 가진 주소 자원 전부에 해당한다.
 *
 * 실행 컨텍스트: 아무 프로세스 컨텍스트. 락을 잡지 않으므로 반환 직후
 * 바인딩이 바뀔 수 있다 — 정확성이 중요한 곳에서 쓸 API 는 아니다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   EXPORT_SYMBOL 로 공개되어 있다. 이 스파스 체크아웃 안에서는 호출자를
 *   찾지 못했다(drivers/pci 밖에서 부르는 것으로 보인다).
 */
struct pci_driver *pci_dev_driver(const struct pci_dev *dev)
{
	int i;	/* [한국어] 자원 배열 순회용 인덱스 */

	if (dev->driver)	/* [한국어] 정식 드라이버가 붙어 있으면 */
		return dev->driver;	/* [한국어] 그것을 돌려준다 */

	for (i = 0; i <= PCI_ROM_RESOURCE; i++)	/* [한국어] 표준 BAR 여섯 개(0~5)에 확장 ROM BAR 를 더한 범위. PCI 타입 0 헤더의 주소 자원 전부다 */
		if (dev->resource[i].flags & IORESOURCE_BUSY)	/* [한국어] IORESOURCE_BUSY 는 누군가 이 영역을 선점했다는 표시다 */
			return &pci_compat_driver;	/* [한국어] 정식 드라이버는 없지만 임자는 있다 — 이름만 있는 껍데기를 돌려준다 */

	return NULL;	/* [한국어] 정말 아무도 안 쓴다 */
}
EXPORT_SYMBOL(pci_dev_driver);	/* [한국어] 공개되어 있으나 이 트리 안에서는 호출자를 찾지 못했다 */

/**
 * pci_bus_match - Tell if a PCI device structure has a matching PCI device id structure
 * @dev: the PCI device structure to match against
 * @drv: the device driver to search for matching PCI device id structures
 *
 * Used by a driver to check whether a PCI device present in the
 * system is in its list of supported devices. Returns the matching
 * pci_device_id structure or %NULL if there is no match.
 */
/* [한국어]
 * pci_bus_match - pci_bus_type.match 슬롯. 장치와 드라이버의 짝을 판정
 *
 * @dev: 후보 장치.  @drv: 후보 드라이버.
 * @return: 1 이면 짝이 맞음, 0 이면 아님. 드라이버 코어는 1 을 받으면
 *   이어서 pci_bus_type.probe 를 부른다.
 *
 * 드라이버 코어는 새 장치가 등록될 때마다 모든 드라이버에 대해, 새
 * 드라이버가 등록될 때마다 모든 장치에 대해 이 함수를 부른다. 그래서
 * 시스템에 장치 N 개와 드라이버 M 개가 있으면 최대 N×M 번 불린다.
 *
 * 먼저 pci_dev_binding_disallowed() 로 이 장치가 바인딩 금지 상태인지
 * 본다(사용자가 sysfs 로 막아 두었거나 시스템 정책상 금지된 경우).
 * 그다음은 pci_match_device() 에 전부 위임하고, 결과를 참·거짓으로
 * 줄여서 돌려준다. 어느 ID 로 맞았는지는 여기서 버려지고,
 * __pci_device_probe() 가 나중에 다시 찾는다.
 *
 * to_pci_driver(drv) 앞의 명시적 캐스팅은 인자가 const 인데 pci_driver 는
 * const 가 아니어서 붙은 것이다. 코드는 건드리지 않고 사실만 적는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 드라이버 코어가 버스 락을 잡은 상태.
 *
 * 에러 경로: 없다. 못 맞추는 것은 정상이다.
 *
 * 호출 체인:
 *   드라이버 코어(driver_match_device) → [이 함수] → pci_match_device()
 */
static int pci_bus_match(struct device *dev, const struct device_driver *drv)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */
	struct pci_driver *pci_drv;	/* [한국어] 아래에서 캐스팅해 받을 자리 */
	const struct pci_device_id *found_id;	/* [한국어] 매칭 결과를 받을 자리 */

	if (pci_dev_binding_disallowed(pci_dev))	/* [한국어] 사용자나 시스템 정책이 이 장치의 바인딩을 막아 두었는가 */
		return 0;	/* [한국어] 그렇다면 어떤 드라이버와도 짝짓지 않는다 */

	pci_drv = (struct pci_driver *)to_pci_driver(drv);	/* [한국어] const device_driver 에서 pci_driver 로. 인자가 const 라 명시적 캐스팅이 붙었다(상류 코드 그대로) */
	found_id = pci_match_device(pci_drv, pci_dev);	/* [한국어] 실제 판정은 전부 여기에 위임한다 */
	if (found_id)	/* [한국어] 맞았으면 */
		return 1;	/* [한국어] 참. 어느 ID 로 맞았는지는 여기서 버려지고 __pci_device_probe 가 다시 찾는다 */

	return 0;	/* [한국어] 못 맞췄다 */
}

/**
 * pci_dev_get - increments the reference count of the pci device structure
 * @dev: the device being referenced
 *
 * Each live reference to a device should be refcounted.
 *
 * Drivers for PCI devices should normally record such references in
 * their probe() methods, when they bind to a device, and release
 * them by calling pci_dev_put(), in their disconnect() methods.
 *
 * A pointer to the device with the incremented reference counter is returned.
 */
/* [한국어]
 * pci_dev_get - struct pci_dev 의 참조 계수를 하나 올린다
 *
 * @dev: 참조할 장치. NULL 이어도 안전하다(그대로 NULL 을 돌려준다).
 * @return: 인자로 받은 그 포인터. 체이닝을 편하게 하려는 관례다.
 *
 * struct pci_dev 는 그 안의 struct device 가 kobject 참조 계수를 갖는다.
 * 이 함수는 get_device() 를 감싼 얇은 래퍼로, 그 계수를 올려 장치 구조체가
 * 해제되지 않게 붙잡는다. 핫플러그로 장치가 뽑혀도 계수가 0 이 될 때까지는
 * 메모리가 살아 있다(다만 하드웨어 접근은 실패하게 된다).
 *
 * 짝은 반드시 pci_dev_put() 이어야 한다. 이 파일 안에서는
 * pci_device_probe() 가 올리고 pci_device_remove() 가 내린다.
 *
 * 원본 주석은 드라이버 쪽 관례도 적어 두었다 — probe 에서 참조를 기록하고
 * disconnect 에서 놓으라는 것이다. 실제로는 대부분의 드라이버가 그렇게
 * 하지 않는데, PCI 계층이 바인딩 구간 전체에 대해 이미 참조를 잡아 주기
 * 때문이다.
 *
 * 실행 컨텍스트: 어디서든. 원자적 증가 하나뿐이라 인터럽트 컨텍스트에서도
 * 안전하다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_device_probe() 등 → [이 함수] → get_device()
 */
struct pci_dev *pci_dev_get(struct pci_dev *dev)
{
	if (dev)
		get_device(&dev->dev);
	return dev;
}
EXPORT_SYMBOL(pci_dev_get);	/* [한국어] 장치 포인터를 오래 보관하려는 모든 코드가 쓴다 */

/**
 * pci_dev_put - release a use of the pci device structure
 * @dev: device that's been disconnected
 *
 * Must be called when a user of a device is finished with it.  When the last
 * user of the device calls this function, the memory of the device is freed.
 */
/* [한국어]
 * pci_dev_put - struct pci_dev 의 참조 계수를 하나 내린다
 *
 * @dev: 놓아 줄 장치. NULL 이어도 안전하다.  @return: 없음.
 *
 * pci_dev_get() 의 짝. 계수가 0 이 되면 struct device 의 release 콜백이
 * 불려 pci_dev 메모리가 실제로 해제된다. 그래서 이 함수를 부른 뒤에는
 * 그 포인터를 다시 쓰면 안 된다 — 마지막 참조였다면 이미 사라진
 * 메모리다.
 *
 * 실행 컨텍스트: 어디서든. 다만 마지막 참조를 놓는 경우 release 경로가
 * 잠들 수 있으므로, 그 가능성이 있는 자리에서는 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_device_probe() 실패 경로, pci_device_remove() 등 → [이 함수]
 *   → put_device()
 */
void pci_dev_put(struct pci_dev *dev)
{
	if (dev)
		put_device(&dev->dev);
}
EXPORT_SYMBOL(pci_dev_put);	/* [한국어] 위의 짝 */

/* [한국어]
 * pci_uevent - pci_bus_type.uevent 슬롯. udev 에 보낼 환경 변수를 채운다
 *
 * @dev: 이벤트의 주인공 장치.
 * @env: 채워 넣을 환경 변수 버퍼. add_uevent_var() 로 한 줄씩 붙인다.
 * @return: 0 성공. 버퍼가 모자라면 -ENOMEM, dev 가 NULL 이면 -ENODEV.
 *
 * PCI 장치가 나타나거나 사라질 때 커널은 사용자 공간에 uevent 를 쏜다.
 * 그 메시지에 무엇을 담을지 정하는 것이 이 함수다.
 *
 * 담는 것은 다섯 가지다. PCI_CLASS, PCI_ID(vendor:device),
 * PCI_SUBSYS_ID(subsystem vendor:device), PCI_SLOT_NAME(도메인:버스:장치.함수
 * 형태의 주소), 그리고 MODALIAS.
 *
 * MODALIAS 가 실질적으로 가장 중요하다. 형식은
 *   pci:v<VENDOR>d<DEVICE>sv<SUBVENDOR>sd<SUBDEVICE>bc<BASECLASS>sc<SUBCLASS>i<PROGIF>
 * 이고 각 필드는 고정 폭 16 진수다. 모듈들은 빌드 시 자기 id_table 을
 * 이 형식의 와일드카드 패턴으로 뽑아 modules.alias 에 넣어 두므로,
 * udev 가 이 문자열로 그 파일을 찾아 올릴 모듈을 결정할 수 있다.
 * 클래스가 bc/sc/i 세 조각으로 쪼개지는 것은 PCI 클래스 코드가
 * 3 바이트(Base Class, Sub-Class, Programming Interface)로 되어 있기
 * 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. kobject uevent 경로에서 불린다.
 *
 * 에러 경로: add_uevent_var() 중 하나라도 실패하면 즉시 -ENOMEM 을
 * 돌려준다. 그러면 uevent 자체가 발송되지 않는다.
 *
 * 호출 체인:
 *   kobject_uevent() → 드라이버 코어 → [이 함수] → add_uevent_var()
 */
static int pci_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct pci_dev *pdev;	/* [한국어] 아래에서 캐스팅해 받을 자리 */

	if (!dev)	/* [한국어] 방어적 검사 */
		return -ENODEV;	/* [한국어] 장치가 없다 */

	pdev = to_pci_dev(dev);	/* [한국어] 일반 device 에서 pci_dev 로 */

	if (add_uevent_var(env, "PCI_CLASS=%04X", pdev->class))	/* [한국어] 클래스 코드. %04X 라 3 바이트 클래스의 상위 한 바이트가 잘려 나가는 상류 코드의 특이점이 있다 */
		return -ENOMEM;	/* [한국어] 환경 버퍼가 모자라면 uevent 자체를 포기한다 */

	if (add_uevent_var(env, "PCI_ID=%04X:%04X", pdev->vendor, pdev->device))	/* [한국어] Vendor:Device ID */
		return -ENOMEM;	/* [한국어] 버퍼 부족 */

	if (add_uevent_var(env, "PCI_SUBSYS_ID=%04X:%04X", pdev->subsystem_vendor,	/* [한국어] Subsystem Vendor ID 와 */
			   pdev->subsystem_device))	/* [한국어] Subsystem Device ID */
		return -ENOMEM;	/* [한국어] 버퍼 부족 */

	if (add_uevent_var(env, "PCI_SLOT_NAME=%s", pci_name(pdev)))	/* [한국어] 도메인:버스:장치.함수 형태의 주소 문자열 */
		return -ENOMEM;	/* [한국어] 버퍼 부족 */

	if (add_uevent_var(env, "MODALIAS=pci:v%08Xd%08Xsv%08Xsd%08Xbc%02Xsc%02Xi%02X",	/* [한국어] udev 가 올릴 모듈을 찾는 데 쓰는 핵심 문자열. 모듈들은 빌드 시 자기 id_table 을 이 형식의 패턴으로 뽑아 둔다 */
			   pdev->vendor, pdev->device,	/* [한국어] v/d 자리 */
			   pdev->subsystem_vendor, pdev->subsystem_device,	/* [한국어] sv/sd 자리 */
			   (u8)(pdev->class >> 16), (u8)(pdev->class >> 8),	/* [한국어] bc = Base Class(클래스 코드의 상위 바이트), sc = Sub-Class(가운데 바이트) */
			   (u8)(pdev->class)))	/* [한국어] i = Programming Interface(하위 바이트) */
		return -ENOMEM;	/* [한국어] 버퍼 부족 */

	return 0;
}

/* [한국어] 오류 복구 uevent 는 그 기능을 실제로 쓰는 아키텍처/옵션에서만
 * 컴파일한다. PCIEAER 는 PCIe 표준 오류 보고, EEH 는 IBM Power 의 오류
 * 격리, S390 은 z 시스템의 PCI 오류 처리다. 셋 다 없으면 이 함수를
 * 부르는 쪽도 없으므로 통째로 빠진다. */
#if defined(CONFIG_PCIEAER) || defined(CONFIG_EEH) || defined(CONFIG_S390)
/**
 * pci_uevent_ers - emit a uevent during recovery path of PCI device
 * @pdev: PCI device undergoing error recovery
 * @err_type: type of error event
 */
/* [한국어]
 * pci_uevent_ers - 오류 복구 진행 상황을 사용자 공간에 알린다
 *
 * @pdev: 복구 중인 장치.
 * @err_type: 복구 단계/결과를 나타내는 pci_ers_result 값.
 * @return: 없음.
 *
 * PCIe AER(Advanced Error Reporting)이나 IBM Power 의 EEH, s390 의 오류
 * 처리에서 장치를 복구할 때, 그 진행 상황을 udev 이벤트로 흘려 준다.
 * 관리 도구가 "이 장치가 지금 복구 중이니 잠시 쓰지 말라" 를 알 수 있게
 * 하는 통로다.
 *
 * 세 갈래로 나뉜다. NONE/CAN_RECOVER/NEED_RESET 은 복구 시작이므로
 * BEGIN_RECOVERY 와 DEVICE_ONLINE=0. RECOVERED 는 성공이라
 * SUCCESSFUL_RECOVERY 와 DEVICE_ONLINE=1. DISCONNECT 는 실패라
 * FAILED_RECOVERY 와 DEVICE_ONLINE=0. 그 밖의 값은 아무것도 보내지 않는다.
 *
 * envp 배열이 3 칸인 이유는 변수 두 개에 마지막 NULL 종료자를 더한
 * 크기이기 때문이다. idx > 0 검사는 default 로 빠져 아무것도 담지 않은
 * 경우 발송 자체를 건너뛰기 위한 것이다.
 *
 * 이 파일에서 오류 복구와 닿는 유일한 함수다. 실제 복구 로직(드라이버의
 * err_handler 콜백을 부르는 일)은 이 파일이 아니라 PCIe 오류 처리 쪽에 있다.
 *
 * 실행 컨텍스트: 복구 작업 스레드. 프로세스 컨텍스트.
 * CONFIG_PCIEAER / CONFIG_EEH / CONFIG_S390 중 하나가 켜져야 컴파일된다.
 *
 * 에러 경로: 없다. uevent 발송 실패는 무시된다.
 *
 * 호출 체인:
 *   AER/EEH 복구 코드 → [이 함수] → kobject_uevent_env()
 */
void pci_uevent_ers(struct pci_dev *pdev, enum pci_ers_result err_type)
{
	int idx = 0;	/* [한국어] 담은 변수 개수 */
	char *envp[3];	/* [한국어] 변수 두 개 + 마지막 NULL 종료자 */

	switch (err_type) {	/* [한국어] 복구 결과에 따라 갈라진다 */
	case PCI_ERS_RESULT_NONE:	/* [한국어] 아직 판정 전이거나 */
	case PCI_ERS_RESULT_CAN_RECOVER:	/* [한국어] 복구 가능하거나 */
	case PCI_ERS_RESULT_NEED_RESET:
		envp[idx++] = "ERROR_EVENT=BEGIN_RECOVERY";	/* [한국어] 셋 다 "복구를 시작한다" 는 뜻이다 */
		envp[idx++] = "DEVICE_ONLINE=0";	/* [한국어] 그동안 이 장치는 쓸 수 없다 */
		break;	/* [한국어] 다음 분기로 넘어가지 않게 */
	case PCI_ERS_RESULT_RECOVERED:	/* [한국어] 복구 성공 */
		envp[idx++] = "ERROR_EVENT=SUCCESSFUL_RECOVERY";	/* [한국어] 성공을 알린다 */
		envp[idx++] = "DEVICE_ONLINE=1";	/* [한국어] 다시 쓸 수 있다 */
		break;	/* [한국어] 분기 종료 */
	case PCI_ERS_RESULT_DISCONNECT:	/* [한국어] 복구 실패, 장치와의 연결이 끊겼다 */
		envp[idx++] = "ERROR_EVENT=FAILED_RECOVERY";	/* [한국어] 실패를 알린다 */
		envp[idx++] = "DEVICE_ONLINE=0";	/* [한국어] 쓸 수 없다 */
		break;	/* [한국어] 분기 종료 */
	default:
		break;	/* [한국어] 그 밖의 값은 알릴 것이 없다 */
	}

	if (idx > 0) {	/* [한국어] 담은 것이 있을 때만 발송한다 */
		envp[idx++] = NULL;	/* [한국어] 배열 끝 표시 */
		kobject_uevent_env(&pdev->dev.kobj, KOBJ_CHANGE, envp);	/* [한국어] KOBJ_CHANGE 로 udev 에 변화 이벤트를 쏜다 */
	}
}
#endif	/* [한국어] CONFIG_PCIEAER || CONFIG_EEH || CONFIG_S390 끝 */

/* [한국어]
 * pci_bus_num_vf - pci_bus_type.num_vf 슬롯. 활성 VF 개수를 알려 준다
 *
 * @dev: 물어볼 장치(SR-IOV 물리 함수).
 * @return: 현재 활성화된 가상 함수 개수. SR-IOV 를 안 쓰면 0.
 *
 * 드라이버 코어에는 버스 종류와 무관하게 "이 장치에 딸린 가상 함수가
 * 몇 개인가" 를 묻는 인터페이스가 있고, PCI 는 그 답을 pci_num_vf() 에서
 * 가져온다. 이 함수는 struct device 를 struct pci_dev 로 바꿔 주는
 * 한 줄짜리 어댑터일 뿐이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   드라이버 코어 → [이 함수] → pci_num_vf()  (drivers/pci/iov.c)
 */
static int pci_bus_num_vf(struct device *dev)
{
	return pci_num_vf(to_pci_dev(dev));
}

/**
 * pci_dma_configure - Setup DMA configuration
 * @dev: ptr to dev structure
 *
 * Function to update PCI devices's DMA configuration using the same
 * info from the OF node or ACPI node of host bridge's parent (if any).
 */
/* [한국어]
 * pci_dma_configure - pci_bus_type.dma_configure 슬롯. 바인딩 직전 DMA·IOMMU 준비
 *
 * @dev: 대상 장치.
 * @return: 0 성공, 음수 errno 실패. 실패하면 바인딩이 중단된다.
 *
 * 드라이버 코어가 probe 를 부르기 직전에 부른다. 드라이버가 probe 안에서
 * dma_set_mask() 나 dma_alloc_coherent() 를 부를 수 있으려면 그 전에
 * 이 준비가 끝나 있어야 한다.
 *
 * 세 부분으로 나뉜다.
 *
 *   (1) 펌웨어에서 DMA 속성을 읽어 온다. 흥미로운 점은 장치 자신이 아니라
 *       호스트 브리지의 부모 노드를 본다는 것이다. DMA 가 실제로 어디까지
 *       닿을 수 있는지(주소 폭, 오프셋, 캐시 일관성 여부)는 개별 PCI 장치가
 *       아니라 그것을 매단 버스와 그 위 SoC 의 성질이기 때문이다. 디바이스
 *       트리 시스템이면 of_dma_configure(), ACPI 시스템이면
 *       acpi_dma_configure() 로 갈라진다.
 *   (2) pci_enable_acs() 로 ACS(Access Control Services)를 켠다. 원본
 *       주석대로 capability 유무와 무관하게 시도하는데, 표준 ACS
 *       capability 는 없으면서 quirk 를 통해 같은 기능을 제공하는
 *       루트 포트가 있기 때문이다. ACS 는 같은 스위치 아래 장치끼리
 *       IOMMU 를 거치지 않고 직접 통신(peer-to-peer)하는 것을 막아
 *       격리를 보장한다.
 *   (3) 드라이버가 IOMMU 를 직접 다루지 않는다면(driver_managed_dma 가
 *       거짓) iommu_device_use_default_domain() 으로 기본 도메인 사용을
 *       표시한다. VFIO 처럼 직접 다루는 드라이버는 이 표시를 하지 않는다.
 *
 * READ_ONCE(dev->driver) 로 드라이버 포인터를 한 번만 읽는 이유는 원본
 * 주석에 있다 — IOMMU 계층에서 부를 때는 그 값이 유효하지 않을 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 바인딩 경로.
 *
 * 에러 경로: (3)이 실패하면 arch_teardown_dma_ops() 로 (1)에서 설정한
 * DMA ops 를 되돌린 뒤 실패를 반환한다. (1)의 실패는 ret 에 담긴 채
 * (2)를 지나 (3)의 조건에서 걸러진다.
 *
 * 호출 체인:
 *   드라이버 코어(really_probe) → [이 함수] → of_dma_configure() 또는
 *   acpi_dma_configure(), pci_enable_acs(),
 *   iommu_device_use_default_domain()
 */
static int pci_dma_configure(struct device *dev)
{
	const struct device_driver *drv = READ_ONCE(dev->driver);	/* [한국어] 드라이버 포인터를 한 번만 읽어 둔다. IOMMU 계층에서 부를 때는 유효하지 않을 수 있어 재차 읽으면 값이 달라질 수 있다 */
	struct device *bridge;	/* [한국어] DMA 속성을 읽어 올 대상 */
	int ret = 0;	/* [한국어] 반환값 겸 중간 판정 */

	bridge = pci_get_host_bridge_device(to_pci_dev(dev));	/* [한국어] 이 장치가 매달린 호스트 브리지를 얻는다. 참조가 하나 올라가므로 아래에서 반드시 내려야 한다 */

	if (IS_ENABLED(CONFIG_OF) && bridge->parent &&	/* [한국어] 디바이스 트리 시스템이고 브리지에 부모 노드가 있으면 */
	    bridge->parent->of_node) {	/* [한국어] 그 부모의 of_node 를 본다. DMA 의 성질은 장치가 아니라 그것을 매단 버스와 SoC 의 것이기 때문이다 */
		ret = of_dma_configure(dev, bridge->parent->of_node, true);	/* [한국어] 주소 폭·오프셋·캐시 일관성을 읽어 dma_ops 를 세운다. 세 번째 인자 true 는 "강제로 설정하라" 는 뜻이다 */
	} else if (has_acpi_companion(bridge)) {	/* [한국어] ACPI 시스템이면 */
		struct acpi_device *adev = to_acpi_device_node(bridge->fwnode);	/* [한국어] 브리지의 ACPI 노드를 얻고 */

		ret = acpi_dma_configure(dev, acpi_get_dma_attr(adev));	/* [한국어] _CCA 등에서 DMA 속성을 읽어 설정한다 */
	}	/* [한국어] 둘 다 아니면 아무것도 하지 않는다(아키텍처 기본 dma_ops 를 그대로 쓴다) */

	/*
	 * Attempt to enable ACS regardless of capability because some Root
	 * Ports (e.g. those quirked with *_intel_pch_acs_*) do not have
	 * the standard ACS capability but still support ACS via those
	 * quirks.
	 */
	pci_enable_acs(to_pci_dev(dev));	/* [한국어] ACS 를 켠다. 같은 스위치 아래 장치끼리 IOMMU 를 우회해 직접 통신하는 것을 막아 격리를 보장한다 */

	pci_put_host_bridge_device(bridge);	/* [한국어] 3601 에서 올린 브리지 참조를 내린다 */

	/* @drv may not be valid when we're called from the IOMMU layer */
	if (!ret && drv && !to_pci_driver(drv)->driver_managed_dma) {	/* [한국어] 앞이 실패하지 않았고, 드라이버가 유효하며, IOMMU 를 직접 다루지 않는 드라이버라면 */
		ret = iommu_device_use_default_domain(dev);	/* [한국어] IOMMU 기본 도메인 사용을 표시한다. vfio-pci 처럼 직접 다루는 드라이버는 이 표시를 하지 않는다 */
		if (ret)	/* [한국어] 실패했다면 */
			arch_teardown_dma_ops(dev);	/* [한국어] 앞서 세운 DMA ops 를 되돌린다 */
	}

	return ret;	/* [한국어] 0 이면 바인딩 진행, 음수면 중단 */
}

/* [한국어]
 * pci_dma_cleanup - pci_bus_type.dma_cleanup 슬롯. 위 설정을 되돌린다
 *
 * @dev: 대상 장치.  @return: 없음.
 *
 * 언바인딩 시 pci_dma_configure() 의 (3)만 되돌린다. IOMMU 기본 도메인
 * 사용 표시를 지워, 다음에 이 장치를 다른 드라이버(예컨대 IOMMU 를 직접
 * 다루는 드라이버)에 넘길 수 있게 한다.
 *
 * (1)의 DMA ops 나 (2)의 ACS 는 되돌리지 않는다. 그것들은 장치의 성질이자
 * 시스템 격리 정책이라 드라이버가 바뀌어도 유지되어야 하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 언바인딩 경로.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   드라이버 코어 → [이 함수] → iommu_device_unuse_default_domain()
 */
static void pci_dma_cleanup(struct device *dev)
{
	struct pci_driver *driver = to_pci_driver(dev->driver);	/* [한국어] 현재 바인딩된 드라이버 */

	if (!driver->driver_managed_dma)	/* [한국어] IOMMU 를 직접 다루지 않는 드라이버였다면 */
		iommu_device_unuse_default_domain(dev);	/* [한국어] 기본 도메인 사용 표시를 지운다. DMA ops 와 ACS 는 되돌리지 않는다 */
}

/*
 * pci_device_irq_get_affinity - get IRQ affinity mask for device
 * @dev: ptr to dev structure
 * @irq_vec: interrupt vector number
 *
 * Return the CPU affinity mask for @dev and @irq_vec.
 */
/* [한국어]
 * pci_device_irq_get_affinity - pci_bus_type.irq_get_affinity 슬롯
 *
 * @dev: 대상 장치.
 * @irq_vec: 몇 번째 인터럽트 벡터인지(드라이버가 할당받은 순번).
 * @return: 그 벡터가 처리하도록 배정된 CPU 마스크. 없으면 NULL.
 *
 * MSI-X 를 여러 개 할당하면 커널이 각 벡터를 CPU 들에 나눠 배정한다.
 * 그 배정 결과를 버스와 무관한 방식으로 물을 수 있게 하는 것이 이
 * 인터페이스이고, PCI 는 pci_irq_get_affinity() 에 그대로 위임한다.
 *
 * 이 정보가 필요한 대표적인 쪽이 blk-mq 다. 하드웨어 큐 하나를 어느
 * CPU 들에 대응시킬지를 정할 때, 그 큐의 완료 인터럽트가 실제로 어느
 * CPU 로 가는지를 알아야 큐와 인터럽트를 같은 CPU 에 맞출 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 정보가 없으면 NULL 이다.
 *
 * 호출 체인:
 *   블록 계층 등 → 드라이버 코어의 irq_get_affinity → [이 함수]
 *   → pci_irq_get_affinity()
 */
static const struct cpumask *pci_device_irq_get_affinity(struct device *dev,
					unsigned int irq_vec)
{
	return pci_irq_get_affinity(to_pci_dev(dev), irq_vec);
}

/* [한국어]
 * pci_bus_type - PCI 버스 그 자체. 이 파일의 모든 함수가 여기로 모인다
 *
 * 커널 드라이버 모델에서 bus_type 은 "장치와 드라이버를 어떻게 짝지어
 * 주고, 짝지어진 뒤 무엇을 해 주는가" 를 정의하는 표다. 위쪽의
 * pci_bus_match / pci_device_probe / pci_device_remove / pci_pm_* 이 전부
 * 이 표의 칸을 채우려고 존재한다. 반대로 말하면, 어떤 PCI 드라이버든
 * 자기 probe 가 불리기까지 반드시 이 표를 거친다.
 *
 * 설정자: 컴파일 시점 상수.
 * 읽는 자: drivers/base 의 드라이버 코어 전체. pci_driver_init() 가
 *   bus_register() 로 등록한 뒤부터 유효하다.
 * 값 범위: .pm 은 CONFIG_PM 이 꺼지면 PCI_PM_OPS_PTR 매크로가 NULL 이 된다.
 * 동기화: 등록 이후에는 읽기 전용이다. 이 표 안의 콜백들이 각자
 *   필요한 락(dynids.lock, device_lock 등)을 잡는다.
 */
const struct bus_type pci_bus_type = {
	.name		= "pci",		/* [한국어] /sys/bus/pci 라는 이름이 여기서 나온다 */
	.driver_override = true,		/* [한국어] driver_override sysfs 속성을 이 버스의 장치에 만들어 준다 */
	.match		= pci_bus_match,	/* [한국어] 장치-드라이버 짝짓기 판정 */
	.uevent		= pci_uevent,		/* [한국어] udev 로 보낼 MODALIAS 등을 채운다 */
	.probe		= pci_device_probe,	/* [한국어] 짝이 맞은 뒤 드라이버 probe 호출 */
	.remove		= pci_device_remove,	/* [한국어] 언바인딩 */
	.shutdown	= pci_device_shutdown,	/* [한국어] 시스템 종료/재부팅 */
	.irq_get_affinity = pci_device_irq_get_affinity, /* [한국어] 벡터별 CPU 친화도 조회 */
	.dev_groups	= pci_dev_groups,	/* [한국어] 장치 sysfs 속성군. 정의는 pci-sysfs.c */
	.bus_groups	= pci_bus_groups,	/* [한국어] 버스 sysfs 속성군(rescan 등) */
	.drv_groups	= pci_drv_groups,	/* [한국어] 드라이버 sysfs 속성군 — 이 파일의 new_id/remove_id */
	.pm		= PCI_PM_OPS_PTR,	/* [한국어] 위의 pci_dev_pm_ops. CONFIG_PM 이 없으면 NULL */
	.num_vf		= pci_bus_num_vf,	/* [한국어] SR-IOV VF 개수 조회 */
	.dma_configure	= pci_dma_configure,	/* [한국어] 바인딩 직전 DMA/IOMMU 설정 */
	.dma_cleanup	= pci_dma_cleanup,	/* [한국어] 언바인딩 시 그 설정 되돌리기 */
};
EXPORT_SYMBOL(pci_bus_type);	/* [한국어] 이 심볼이 있어야 모듈이 "이 장치가 PCI 인가" 를 dev->bus 비교로 판별할 수 있다 */

/* [한국어]
 * pci_driver_init - PCI 버스 타입을 커널에 등록하는 초기화
 *
 * @return: 0 성공, 음수 errno 실패.
 *
 * postcore_initcall 로 등록되어 부팅 아주 이른 시점에 실행된다. 이것이
 * 끝나야 /sys/bus/pci 가 생기고, 그 뒤에야 어떤 PCI 드라이버든 등록될 수
 * 있다. PCI 호스트 브리지 스캔보다도 먼저 와야 하므로 이렇게 이른
 * initcall 레벨을 쓴다.
 *
 * 하는 일. (1) probe 전용 워크큐를 만든다. (2) pci_bus_type 을 등록한다.
 * (3) CONFIG_PCIEPORTBUS 면 pcie_port_bus_type 도 등록한다 — PCIe 포트
 * 서비스(AER, 핫플러그, PME 등)가 쓰는 별도의 가상 버스다.
 * (4) dma_debug_add_bus() 로 DMA 디버깅에 이 버스를 등록한다.
 *
 * 상류 코드 관찰 두 가지. 첫째, 만드는 워크큐의 이름이 "sync_wq" 인데
 * 변수 이름은 pci_probe_wq 다. 이름이 어긋나 있어 /proc 이나 ps 에서
 * 이 워커를 찾을 때 헷갈릴 수 있다. 둘째, (2)나 (3)이 실패하면 그대로
 * 반환할 뿐 앞서 만든 워크큐를 해제하지 않는다. initcall 실패는 사실상
 * 부팅 실패라 실무상 문제가 되지 않지만, 되돌리기가 없는 것은 사실이다.
 * 둘 다 주석으로만 기록하고 코드는 건드리지 않는다.
 *
 * 실행 컨텍스트: 부팅 초기, 단일 스레드. 프로세스 컨텍스트.
 *
 * 에러 경로: 워크큐 할당 실패는 -ENOMEM. bus_register 실패는 그 값.
 *
 * 호출 체인:
 *   커널 initcall 처리 → [이 함수] → alloc_workqueue(), bus_register()
 */
static int __init pci_driver_init(void)
{
	int ret;	/* [한국어] bus_register 반환값 */

	pci_probe_wq = alloc_workqueue("sync_wq", WQ_PERCPU, 0);	/* [한국어] probe 전용 per-CPU 워크큐를 만든다. 이름이 "sync_wq" 로 변수명과 어긋나 있다(상류 코드 그대로) */
	if (!pci_probe_wq)	/* [한국어] 할당 실패 검사 */
		return -ENOMEM;	/* [한국어] 부팅 실패로 이어진다 */

	ret = bus_register(&pci_bus_type);	/* [한국어] PCI 버스 타입을 등록한다. 이 순간 /sys/bus/pci 가 생긴다 */
	if (ret)	/* [한국어] 실패하면 */
		return ret;	/* [한국어] 앞서 만든 워크큐를 해제하지 않고 그대로 반환한다(상류 코드 그대로) */

#ifdef CONFIG_PCIEPORTBUS	/* [한국어] PCIe 포트 서비스(AER·핫플러그·PME·DPC)를 쓰는 빌드에서만 */
	ret = bus_register(&pcie_port_bus_type);	/* [한국어] 포트 서비스 드라이버들이 붙을 가상 버스를 하나 더 등록한다 */
	if (ret)
		return ret;	/* [한국어] 실패해도 앞서 등록한 pci_bus_type 은 되돌리지 않는다(상류 코드 그대로) */
#endif
	dma_debug_add_bus(&pci_bus_type);
	return 0;
}
postcore_initcall(pci_driver_init);	/* [한국어] initcall 레벨 2. 코어 초기화 직후, 대부분의 서브시스템보다 먼저 실행되어 /sys/bus/pci 를 만든다 */

/* [한국어]
 * === 이 파일을 다 읽고 남는 그림 ===
 *
 * 1) 짝짓기. 드라이버 코어가 pci_bus_type.match 를 부른다 ->
 *    pci_bus_match() -> pci_match_device(). 우선순위는
 *    driver_override > dynids(sysfs new_id) > 정적 id_table 이다.
 *
 * 2) 바인딩. pci_bus_type.probe -> pci_device_probe(). IRQ 를 배정하고
 *    (pci_assign_irq, pcibios_alloc_irq), 참조를 하나 올리고
 *    (pci_dev_get), __pci_device_probe -> pci_call_probe ->
 *    local_pci_probe -> drv->probe() 로 내려간다. DMA/IOMMU 설정은
 *    이보다 앞서 드라이버 코어가 pci_bus_type.dma_configure 로 해 둔다.
 *
 * 3) 전원 관리. pci_dev_pm_ops 의 스무 개 남짓한 슬롯이 시스템 절전,
 *    최대 절전, 런타임 절전 세 계열로 나뉜다. 공통 골격은 같다 —
 *    드라이버 콜백을 부르고, 드라이버가 상태를 저장하지 않았으면
 *    pci_save_state() 로 대신 저장하고, pci_prepare_to_sleep() 으로
 *    저전력 D-state 로 내린다. 복귀는 그 역순이다.
 *
 * 4) 해제. pci_device_remove() 가 drv->remove() 를 부르고 IRQ 와
 *    참조를 돌려놓는다. pci_device_shutdown() 은 재부팅 경로이며,
 *    kexec 일 때는 Bus Master 를 꺼서 다음 커널의 메모리를 장치가
 *    DMA 로 덮어쓰지 못하게 한다.
 *
 * 이 파일에는 특정 장치 종류를 위한 코드가 한 줄도 없다. NVMe 컨트롤러도
 * 그래픽 카드도 랜카드도 전부 struct pci_driver 하나로 똑같이 취급된다.
 * NVMe 쪽에서 이 파일을 향해 부르는 것은 drivers/nvme/host/pci.c:5397 의
 * pci_register_driver(&nvme_driver) 와 5407 의 pci_unregister_driver() 뿐이고,
 * 나머지는 전부 이 파일이 nvme_probe/nvme_remove/nvme_shutdown 과
 * nvme_dev_pm_ops 의 콜백을 불러 주는 반대 방향이다.
 */
