// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2002-2004 Greg Kroah-Hartman <greg@kroah.com>
 * (C) Copyright 2002-2004 IBM Corp.
 * (C) Copyright 2003 Matthew Wilcox
 * (C) Copyright 2003 Hewlett-Packard
 * (C) Copyright 2004 Jon Smirl <jonsmirl@yahoo.com>
 * (C) Copyright 2004 Silicon Graphics, Inc. Jesse Barnes <jbarnes@sgi.com>
 *
 * File attributes for PCI devices
 *
 * Modeled after usb's driverfs.c
 */

/*
 * [한국어 설명] PCI 장치/버스의 sysfs 속성 파일 구현 (drivers/pci/pci-sysfs.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 /sys/bus/pci/devices/<도메인:버스:장치.기능>/ 아래에 보이는 거의
 * 모든 파일을 만들어 내는 곳이다. vendor, device, class, revision 같은
 * 설정공간(Configuration Space) 필드 덤프부터 resource(BAR 목록),
 * current_link_speed/current_link_width(PCIe 링크 현재 상태), enable,
 * numa_node, reset, reset_method, remove, rescan, resourceN_resize 같은
 * 제어용 파일, 그리고 config/resourceN/rom 같은 바이너리 창까지 여기서
 * 정의한다. 커널 내부 상태를 사람이 읽는 텍스트로 바꾸는 얇은 표현 계층이며,
 * 실제 하드웨어 조작은 전부 drivers/pci/pci.c 등 아래 계층에 위임한다.
 * 이 파일 자체는 PCI 열거·자원 할당 로직을 갖고 있지 않다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 코어의 열거(probe.c) -> 자원 할당(setup-bus.c/setup-res.c) ->
 * 장치 등록(bus.c) 흐름의 맨 끝, "사용자 공간에 노출" 단계에 해당한다.
 * 구체적으로 세 갈래로 붙는다. (1) drivers/pci/pci-driver.c 의 pci_bus_type 이
 * .dev_groups = pci_dev_groups, .bus_groups = pci_bus_groups 로 이 파일의
 * 그룹 배열을 가져가고, (2) drivers/pci/probe.c 의 device_type pci_dev_type 이
 * .groups = pci_dev_attr_groups 를, class pcibus_class 가
 * .dev_groups = pcibus_groups 를 가져간다. (3) 그룹으로 표현할 수 없는
 * 개수 가변 파일(resource0..5, resource0_wc 등)은 bus.c 의
 * pci_bus_add_device() 가 부르는 pci_create_sysfs_dev_files() 가 그때그때
 * 만들고, remove.c 의 pci_stop_dev() 가 pci_remove_sysfs_dev_files() 로 지운다.
 * 실행 컨텍스트는 전부 커널 프로세스 문맥이다 — show/store 콜백은 사용자
 * 프로세스가 read(2)/write(2) 를 부른 그 문맥에서 실행되므로 잠들 수 있다.
 *
 * === sysfs 속성이 만들어지는 메커니즘 ===
 * 1) 매크로가 struct device_attribute 한 개를 만든다. DEVICE_ATTR_RO(x) 는
 *    x_show() 를 읽기 전용 속성 dev_attr_x 로, DEVICE_ATTR_RW(x) 는
 *    x_show()/x_store() 를 읽기·쓰기 속성으로 묶는다. 권한 비트를 직접
 *    정해야 할 때는 __ATTR(이름, 모드, show, store) 를 쓴다 — 이 파일에서는
 *    dev_attr_dev_rescan 과 dev_attr_bus_rescan 이 그렇다. 두 속성은 파일
 *    이름이 똑같이 "rescan" 이면서 대상(장치 vs 버스)이 달라 C 심볼 이름을
 *    달리해야 했기 때문이다.
 * 2) 만들어진 속성 포인터들을 struct attribute 배열(pci_dev_attrs[] 등)에
 *    모으고, 그 배열을 struct attribute_group 에 담는다.
 * 3) attribute_group 의 .is_visible 콜백이 장치마다 개별 속성을 보일지
 *    말지를 결정한다. 반환값 0 이면 그 장치에는 파일 자체가 생기지 않고,
 *    0 이 아니면 그 값이 파일 모드가 된다. 그래서 브리지가 아닌 장치에는
 *    secondary_bus_number 가 없고, PCIe 가 아닌 장치에는
 *    current_link_speed 가 없다. 정적 배열 하나로 온갖 장치를 다루기 위한
 *    장치다.
 * 4) 그룹 배열(pci_dev_groups[] 등)을 bus_type/device_type/class 에 걸어
 *    두면, 장치가 등록될 때 드라이버 코어가 자동으로 파일을 만든다.
 *
 * === 일반 속성과 바이너리 속성(bin_attribute)의 차이 ===
 * 일반 속성의 show() 는 페이지 한 장짜리 버퍼에 텍스트를 채우는 방식이라
 * 크기가 PAGE_SIZE 로 제한되고, mmap 도 seek 도 안 된다. 그래서
 * 설정공간 전체(표준 256B, PCIe 확장 4096B), BAR 창(수 MB~수 GB), 옵션 ROM
 * 처럼 크거나 임의 오프셋 접근이 필요한 것은 struct bin_attribute 로 만든다.
 * bin_attribute 의 read/write 는 (버퍼, 오프셋, 길이) 를 그대로 받으므로
 * pread/pwrite 로 원하는 오프셋만 집어 읽을 수 있고, .mmap 콜백을 채우면
 * 사용자 공간이 BAR 를 직접 매핑해 MMIO 레지스터를 만질 수도 있다. 이 파일의
 * bin_attribute 는 config(설정공간), resourceN/resourceN_wc(BAR), rom(옵션 ROM),
 * legacy_io/legacy_mem(레거시 ISA 창) 이다. 크기가 장치마다 다르므로
 * .bin_size 콜백(pci_dev_config_attr_bin_size, pci_dev_rom_attr_bin_size)이
 * 장치별 실제 크기를 알려 준다.
 *
 * === 권한 검사 ===
 * 위험도에 따라 세 겹으로 막는다. (1) 파일 모드 — config 는 0644 라 읽기는
 * 누구나 되지만 쓰기는 소유자(root)만, rom 과 resourceN 은 0600, remove 는
 * 0220 (쓰기 전용), rescan 은 0200 이다. (2) 능력(capability) 검사 —
 * enable_store, numa_node_store, msi_bus_store, reset_subordinate_store 는
 * capable(CAP_SYS_ADMIN) 을 직접 확인하고, pci_read_config() 는
 * file_ns_capable(filp, &init_user_ns, CAP_SYS_ADMIN) 여부에 따라 보여 주는
 * 범위를 64바이트(표준 헤더)로 줄인다 — 권한 없는 사용자에게 장치의 전체
 * 설정공간을 흘리지 않기 위해서다. (3) 커널 락다운 — pci_write_config(),
 * pci_write_resource_io(), pci_mmap_resource() 는
 * security_locked_down(LOCKDOWN_PCI_ACCESS) 를 물어, 서명 검증된 커널을
 * 우회할 수 있는 임의 MMIO/설정공간 쓰기를 차단한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽(이 파일을 쓰는 쪽): drivers/pci/pci-driver.c(pci_bus_type),
 * drivers/pci/probe.c(pci_dev_type, pcibus_class, pci_create_legacy_files),
 * drivers/pci/bus.c(pci_create_sysfs_dev_files),
 * drivers/pci/remove.c(pci_remove_sysfs_dev_files, pci_remove_legacy_files).
 * 아래쪽(이 파일이 부르는 쪽): drivers/pci/pci.c 의 pci_enable_device,
 * pci_disable_device, pci_reset_function, pci_config_pm_runtime_get/put,
 * pci_reset_fn_methods[]; drivers/pci/probe.c 의 pci_rescan_bus 계열;
 * drivers/pci/setup-res.c 의 pci_resize_resource; drivers/pci/rom.c 의
 * pci_map_rom/pci_unmap_rom; drivers/pci/mmap.c 의 pci_mmap_resource_range.
 * 다른 파일에 정의된 속성 그룹들(VPD, SMBIOS, ACPI, SR-IOV, AER, ASPM, DOE,
 * TSM)도 이 파일 맨 아래의 그룹 배열에 이름만 얹혀 함께 등록된다 —
 * 선언은 drivers/pci/pci.h 에 extern 으로 있다.
 * 공유 상태: struct pci_dev 자체(resource[], reset_methods[], res_attr[],
 * res_attr_wc[], rom_attr_enabled, no_msi, d3cold_allowed 등)와 파일 정적
 * 변수 sysfs_initialized 하나뿐이다. 이 파일은 별도 락을 거의 잡지 않고,
 * 필요한 곳에서 device_lock()/pci_lock_rescan_remove() 를 빌려 쓴다.
 *
 * === NVMe 학습 관점 ===
 * 이 파일의 함수를 drivers/nvme 가 직접 호출하는 곳은 0건이다(주석을 제거한
 * 토큰 기준으로 drivers/nvme 전체를 검색해 확인했다). 연결은 전부 간접적이다.
 * - /sys/class/nvme/nvme0/device 심볼릭 링크가 가리키는 곳이 바로 이 파일이
 *   채운 디렉터리다. drivers/nvme/host/core.c 의 nvme_init_ctrl() 이
 *   ctrl->device->parent = ctrl->dev 로 걸어 두고, nvme-pci 는
 *   drivers/nvme/host/pci.c 에서 nvme_init_ctrl(&dev->ctrl, &pdev->dev, ...)
 *   로 그 부모를 pci_dev 의 device 로 준다. 즉 nvme0 의 "device" 를 따라가면
 *   /sys/bus/pci/devices/<BDF> 가 나오고, 거기 있는 파일이 여기 것들이다.
 * - lspci 는 이 파일이 만든 config 바이너리 파일(또는 /proc/bus/pci)을 읽어
 *   설정공간을 덤프한다. root 가 아니면 pci_read_config() 가 64바이트만
 *   돌려주므로 capability 목록이 잘려 보인다.
 * - NVMe SSD 의 성능 문제를 볼 때 실제로 들여다보는 파일이 여기 있다:
 *   current_link_speed/current_link_width(협상된 링크가 기대보다 낮은가),
 *   max_link_speed/max_link_width(장치가 낼 수 있는 상한),
 *   numa_node(어느 소켓에 붙었나 — 큐 affinity 와 직결), irq,
 *   resource(BAR0 의 물리 주소 범위 — NVMe 는 BAR0 안 NVME_REG_DBS 오프셋에
 *   도어벨 창을 두고 ioremap 한다. drivers/nvme/host/pci.c 의 nvme_remap_bar()
 *   가 pci_resource_start(pdev, 0) 을 쓰는 것이 근거다).
 * - reset/reset_method 는 NVMe 컨트롤러를 통째로 리셋할 때 쓰는 마지막 수단
 *   경로다. reset_method 에 나열되는 이름들(device_specific, acpi, flr,
 *   af_flr, pm, bus, cxl_bus)의 실체는 drivers/pci/pci.c 의
 *   pci_reset_fn_methods[] 다. 이것은 PCI 함수 수준 리셋이라, NVMe 스펙의
 *   컨트롤러 리셋(CC.EN 을 0 으로)이나 NSSR 과는 다른 층위다.
 * - 주의: irq_show() 는 MSI 일 때만 첫 MSI 벡터를 보여 주고 MSI-X 는
 *   레거시 INTx 번호를 보여 준다. NVMe 는 pci_alloc_irq_vectors_affinity()
 *   로 MSI-X 를 쓰므로(drivers/nvme/host/pci.c 에서 확인), /sys/.../irq 값은
 *   실제 큐 인터럽트와 무관하다. 큐별 벡터는 /proc/interrupts 나
 *   msi_irqs/ 디렉터리에서 봐야 한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - pci_read_config()/pci_write_config(): config 바이너리 파일의 읽기·쓰기.
 *   오프셋 정렬에 맞춰 1/2/4바이트 접근으로 쪼개고, 권한과 락다운을 검사한다.
 * - pci_mmap_resource()/pci_create_attr()/pci_create_resource_files():
 *   BAR 별 resourceN, resourceN_wc 바이너리 파일을 만들고 mmap 을 처리한다.
 * - pci_read_rom()/pci_write_rom(): 옵션 ROM 을 잠깐 켜서 읽게 해 주는 창.
 *   rom 에 1 을 써서 켜고 읽은 뒤 0 을 써서 끄는 2단계 프로토콜이다.
 * - reset_method_show()/reset_method_store(): 이 장치에 허용할 리셋 방법
 *   목록을 이름으로 읽고 쓴다.
 * - __resource_resize_show()/__resource_resize_store(): PCIe Resizable BAR
 *   능력을 사용자에게 노출한다. BAR 크기를 바꾸면 자원 재할당이 필요하므로
 *   속성 파일을 지웠다가 다시 만든다.
 * - pci_dev_groups[]/pci_dev_attr_groups[]/pcibus_groups[]/pci_bus_groups[]:
 *   드라이버 코어에 넘겨지는 최상위 그룹 배열. 파일 맨 아래에 모여 있다.
 * - sysfs_initialized: late_initcall 인 pci_sysfs_init() 이 1 로 만들기 전에는
 *   자원 파일을 만들지 않게 하는 문지기. 부팅 초기에 등록된 장치는
 *   pci_sysfs_init() 이 나중에 일괄로 훑어 파일을 만들어 준다.
 */

/* [한국어] 아래 include 들에 대한 주의: 이 작업 트리에는 include/linux/ 의
 * 극히 일부(blkdev.h, blk-mq.h, iommufd.h, nvme.h, vfio.h, vfio_pci_core.h)만
 * 있고 sysfs.h/device.h/pci.h 등은 없다. 따라서 "이 헤더가 이 심볼의 유일한
 * 출처" 임을 원문으로 확인할 수는 없다. 아래 설명은 이 파일에서 실제로
 * 쓰이는 심볼을 근거로 한 것이며, 근거를 못 찾은 것은 그렇다고 적었다. */

#include <linux/bitfield.h> /* [한국어] FIELD_GET() 을 쓰기 위해. 아래
	* current_link_width_show() 가 FIELD_GET(PCI_EXP_LNKSTA_NLW, linkstat)
	* 로 링크 상태 레지스터에서 협상된 레인 수 필드만 뽑아낸다. */
#include <linux/cleanup.h> /* [한국어] __free() 정리 속성을 쓰기 위해.
	* reset_method_store() 의 `char *options __free(kfree)` 가 이것으로,
	* 함수를 어느 return 으로 빠져나가도 kfree 가 자동 호출되게 한다.
	* 그 함수에 return 이 여러 개라 수동 해제는 실수하기 쉽다. */
#include <linux/kernel.h> /* [한국어] 커널 공용 매크로 모음 헤더. 이 파일에서
	* 이 헤더만이 출처인 심볼은 특정하지 못했다(트리에 헤더가 없어 확인 불가). */
#include <linux/sched.h> /* [한국어] current 태스크 포인터를 쓰기 위해.
	* pci_write_config() 이 커널 전용 설정공간 영역에 쓰기가 들어오면
	* current->comm 으로 어떤 프로세스가 그랬는지 경고에 찍는다. */
#include <linux/pci.h> /* [한국어] 이 파일의 근간. struct pci_dev, to_pci_dev(),
	* pci_read_config_byte 계열, PCI_EXP_LNKSTA 같은 스펙 상수, 그리고
	* pci_enable_device 계열 외부 API 가 전부 여기서 온다. */
#include <linux/stat.h> /* [한국어] 파일 모드 상수(S_IRUGO 계열) 헤더. 다만
	* 이 파일은 0644/0600/0220/0200 처럼 8진수를 직접 쓰고 S_I 접두 매크로는
	* 한 번도 쓰지 않는다 — 즉 이 파일에서 실사용 근거는 찾지 못했다. */
#include <linux/export.h> /* [한국어] EXPORT_SYMBOL 계열 매크로 헤더. 이 파일에
	* EXPORT_SYMBOL 은 하나도 없다(검색으로 확인). 전역 배열
	* pci_dev_groups[] 등은 drivers/pci/pci.h 의 extern 선언으로 PCI 코어
	* 안에서만 공유되므로 export 가 필요 없다. */
#include <linux/topology.h> /* [한국어] CPU 토폴로지 질의용. dev_to_node(),
	* cpumask_of_node(), cpumask_of_pcibus() 를 써서 local_cpus,
	* local_cpulist, cpuaffinity, cpulistaffinity 파일을 만든다. NVMe 큐를
	* 어느 CPU 에 붙일지 판단할 때 사용자 공간이 읽는 값이다. */
#include <linux/mm.h> /* [한국어] struct vm_area_struct 정의가 필요하다.
	* bin_attribute 의 .mmap 콜백(pci_mmap_resource 계열,
	* pci_mmap_legacy_mem/io)이 vma 를 받아 BAR 를 사용자 주소공간에 매핑한다. */
#include <linux/fs.h> /* [한국어] struct file, loff_t, fixed_size_llseek() 를
	* 쓰기 위해. bin_attribute 의 read/write 는 loff_t 오프셋을 받고,
	* pci_llseek_resource() 가 fixed_size_llseek 으로 seek 를 처리한다. */
#include <linux/capability.h> /* [한국어] capable(), file_ns_capable(),
	* CAP_SYS_ADMIN 을 쓰기 위해. 장치를 켜고 끄거나 NUMA 노드를 덮어쓰는
	* 등 시스템을 망가뜨릴 수 있는 store 들이 이걸로 막힌다. */
#include <linux/security.h> /* [한국어] security_locked_down() 과
	* LOCKDOWN_PCI_ACCESS 를 쓰기 위해. 커널 락다운이 켜진 시스템에서
	* 설정공간 쓰기와 BAR mmap 을 금지해, 서명된 커널의 무결성 보장을
	* 임의 MMIO 쓰기로 우회하지 못하게 한다. */
#include <linux/slab.h> /* [한국어] 동적 할당용. pci_create_attr() 의 kzalloc,
	* pci_create_legacy_files() 의 kzalloc_objs, reset_method_store() 의
	* kstrndup, 그리고 대응하는 kfree 가 여기서 온다. */
#include <linux/vgaarb.h> /* [한국어] vga_default_device() 를 쓰기 위해.
	* boot_vga_show() 가 이 장치가 부팅 화면을 담당한 VGA 장치인지 판단한다. */
#include <linux/pm_runtime.h> /* [한국어] 런타임 전원 관리 API. reset_store 의
	* pm_runtime_get_sync/put, d3cold_allowed_store 의 pm_runtime_resume,
	* reset_method_store 의 PM_RUNTIME_ACQUIRE 가 여기서 온다. 설정공간을
	* 읽으려면 장치가 D3cold 에서 깨어 있어야 하므로 반드시 필요하다. */
#include <linux/msi.h> /* [한국어] MSI 관련 정의 헤더. 이 파일이 쓰는
	* pdev->msi_enabled 는 struct pci_dev 의 필드이고 pci_irq_vector() 는
	* 통상 pci.h 에 선언되어 있어, 이 헤더만이 출처인 심볼은 특정하지 못했다. */
#include <linux/of.h> /* [한국어] 디바이스 트리(Open Firmware) 지원.
	* devspec_show() 가 pci_device_to_OF_node() 로 struct device_node 를 얻어
	* %pOF 로 노드 경로를 찍는다. CONFIG_OF 일 때만 쓰인다. */
#include <linux/aperture.h> /* [한국어] aperture_remove_conflicting_pci_devices()
	* 를 쓰기 위해. BAR 크기를 바꾸기 전에 그 영역을 물고 있는 부트 콘솔
	* 프레임버퍼 드라이버를 먼저 쫓아내야 하므로 VGA 클래스에서만 부른다. */
#include <linux/unaligned.h> /* [한국어] put_unaligned_be64() 를 쓰기 위해.
	* serial_number_show() 가 64비트 DSN 을 빅엔디언 바이트 배열로 펴서
	* %8phD 형식으로 찍는데, 스택 배열이 8바이트 정렬이라는 보장이 없다. */
#include "pci.h" /* [한국어] PCI 코어 내부 전용 헤더(drivers/pci/pci.h).
	* pci_config_pm_runtime_get/put, pci_reset_supported,
	* pci_init_reset_methods, pci_try_reset_bridge, pci_reset_fn_methods[],
	* pci_resource_is_bridge_win(), pci_mmap_fits(), PCI_CFG_SPACE_SIZE 와
	* PCI_CFG_SPACE_EXP_SIZE, 그리고 이 파일 맨 아래에서 참조하는 외부 속성
	* 그룹들(pci_dev_vpd_attr_group, aer_stats_attr_group 등)의 extern 선언이
	* 전부 여기 있다 — 실제로 그 파일에서 확인했다. */

/* [한국어] ARCH_PCI_DEV_GROUPS 는 아키텍처가 자기만의 sysfs 속성 그룹을
 * pci_dev_groups[] 에 끼워 넣을 수 있게 뚫어 둔 구멍이다. 아키텍처 헤더가
 * 이 이름을 "&내_그룹," 처럼 정의해 두면 아래 pci_dev_groups[] 안의
 * ARCH_PCI_DEV_GROUPS 자리에 그대로 전개된다(그래서 그 자리에 쉼표가 없다).
 * 정의해 둔 곳이 없으면 여기서 빈 매크로로 만들어 아무것도 추가하지 않는다.
 * 어느 아키텍처가 실제로 이걸 정의하는지는 이 트리에 arch/ 디렉터리가 없어
 * 확인할 수 없다. */
#ifndef ARCH_PCI_DEV_GROUPS /* [한국어] 아키텍처 헤더가 먼저 정의했는지 확인 */
#define ARCH_PCI_DEV_GROUPS /* [한국어] 정의가 없으면 빈 것으로 -> 배열에 추가 없음 */
#endif /* [한국어] ARCH_PCI_DEV_GROUPS 기본 정의 분기 끝 */

/* [한국어] sysfs_initialized - sysfs 서브시스템이 PCI 자원 파일을 만들 준비가
 * 되었는지를 나타내는 파일 스코프 플래그.
 * 설정자: 이 파일 아래쪽의 pci_sysfs_init() 이 딱 한 번 1 로 만든다.
 *   late_initcall 이라 부팅의 늦은 단계에서야 실행된다.
 * 읽는 자: pci_create_sysfs_dev_files(), pci_remove_sysfs_dev_files(),
 *   pci_create_legacy_files(). 0 이면 아무 파일도 만들지 않고 조용히 물러난다.
 * 값 범위: 0(아직 준비 안 됨) 또는 1(준비됨). 다시 0 이 되는 일은 없다.
 * 왜 필요한가: 부팅 아주 이른 시점에 발견된 장치는 sysfs 파일을 만들 수
 *   없다. 그런 장치는 파일 생성을 건너뛰고, 나중에 pci_sysfs_init() 이
 *   for_each_pci_dev() 로 전부 다시 훑어 파일을 만들어 준다.
 * 동기화: 초기화 시점에 한 번만 쓰이고 그 뒤로는 읽기 전용이라 락이 없다.
 *   주석 `= 0` 은 BSS 에 놓여 0 으로 시작함을 명시한 상류의 표기다. */
static int sysfs_initialized;	/* = 0 */

/* show configuration fields */
/* [한국어] pci_config_attr(field, format_string) - 설정공간에서 캐시해 둔
 * pci_dev 필드 하나를 그대로 찍는 읽기 전용 sysfs 속성을 통째로 찍어내는 매크로.
 *
 * @field: struct pci_dev 의 필드 이름이자 sysfs 파일 이름
 *   (vendor, device, subsystem_vendor, subsystem_device, revision, class).
 * @format_string: sysfs_emit 에 넘길 출력 형식. 필드 폭에 맞춘 16진수다.
 *
 * 이 매크로 한 번의 전개가 만드는 것은 두 가지다.
 *   (1) static ssize_t <field>_show(dev, attr, buf) 함수 —
 *       device 를 pci_dev 로 되돌린 뒤 pdev-><field> 를 형식대로 찍는다.
 *   (2) static DEVICE_ATTR_RO(<field>) — 위 show 를 읽기 전용 속성
 *       dev_attr_<field> 로 묶는다. 아래 pci_dev_attrs[] 가 이 심볼을 쓴다.
 * 여섯 속성이 코드가 완전히 같아서 매크로로 묶은 것이며, 값은 열거 때
 * probe.c 가 설정공간에서 읽어 pci_dev 에 캐시해 둔 것이라 이 show 는
 * 하드웨어를 건드리지 않는다 — 그래서 런타임 PM 을 깨울 필요도 없다.
 *
 * 만들어지는 파일과 읽었을 때 알 수 있는 것:
 *   vendor           - PCI Vendor ID (설정공간 오프셋 0x00, 16비트).
 *                      lspci 가 이 값으로 제조사 이름을 찾는다.
 *   device           - PCI Device ID (오프셋 0x02, 16비트).
 *                      vendor 와 짝지어 드라이버 매칭에 쓰인다.
 *   subsystem_vendor - Subsystem Vendor ID (오프셋 0x2c).
 *   subsystem_device - Subsystem Device ID (오프셋 0x2e). 같은 컨트롤러
 *                      칩을 쓰는 여러 완제품을 구분하는 값이라, 같은 NVMe
 *                      컨트롤러의 OEM 변종을 가려낼 때 본다.
 *   revision         - Revision ID (오프셋 0x08, 8비트). 칩 리비전.
 *   class            - Class Code 24비트(오프셋 0x09~0x0b: prog-if,
 *                      subclass, base class). NVMe 장치는 여기가
 *                      PCI_CLASS_STORAGE_EXPRESS 로, drivers/nvme/host/pci.c
 *                      의 id 테이블이 PCI_DEVICE_CLASS 로 이 값을 매칭한다.
 *                      상수의 실제 숫자값은 include/linux/pci_ids.h 에 있고
 *                      이 트리에는 그 헤더가 없어 확인할 수 없다.
 *
 * 왜 sprintf 가 아니라 sysfs_emit 인가: sysfs 규약상 show 콜백이 받는 buf 는
 * 페이지 하나의 시작 주소다. sysfs_emit 은 그 전제를 알고 PAGE_SIZE 경계를
 * 스스로 지키므로, 형식 문자열이 예상보다 길어져도 버퍼를 넘지 않는다.
 * sprintf 는 경계를 모르기 때문에 sysfs show 에서는 쓰면 안 된다.
 * (반대로 이 파일의 pci_create_attr() 은 sprintf 를 쓰는데, 그쪽은 필요한
 *  바이트 수를 직접 계산해 할당한 버퍼라 경계가 이미 보장되어 있다.)
 * ## 는 전처리기의 토큰 붙이기 연산자다. field 에 vendor 가 들어오면
 * field##_show 가 vendor_show 라는 하나의 식별자로 합쳐진다.
 *
 * 실행 컨텍스트: 사용자가 파일을 read(2) 한 프로세스 문맥. 재진입 가능하고
 * 락을 잡지 않는다 — 읽는 값이 열거 이후 바뀌지 않는 캐시이기 때문이다.
 *
 * 호출 체인:
 *   사용자 공간 read(2) -> sysfs -> dev_attr_show -> [<field>_show] -> sysfs_emit
 *
 * 매크로 문법 주의: 아래 각 줄은 반드시 백슬래시로 끝나야 줄이 이어진다.
 * 그래서 설명 주석은 백슬래시 앞에 둔다. */
#define pci_config_attr(field, format_string) /* [한국어] 속성 6종을 찍어내는 틀 */ \
static ssize_t /* [한국어] sysfs show 콜백의 규약 반환형: 쓴 바이트 수 또는 음수 오류 */ \
field##_show(struct device *dev, struct device_attribute *attr, char *buf) /* [한국어] ## 로 이름 생성 */ \
{ /* [한국어] 매크로 본문 시작 */ \
	struct pci_dev *pdev; /* [한국어] 아래에서 device 로부터 복원할 PCI 장치 */ \
 /* [한국어] (빈 줄 — 매크로 안이라 백슬래시가 필요하다) */ \
	pdev = to_pci_dev(dev); /* [한국어] container_of 로 struct device -> struct pci_dev 복원 */ \
	return sysfs_emit(buf, format_string, pdev->field); /* [한국어] PAGE_SIZE 경계는 sysfs_emit 이 지킨다 */ \
} /* [한국어] 매크로 본문 종료 */ \
static DEVICE_ATTR_RO(field) /* [한국어] dev_attr_<field> 생성. 세미콜론은 사용처에서 붙인다 */

pci_config_attr(vendor, "0x%04x\n"); /* [한국어] /sys/.../vendor — Vendor ID 16비트. 예로 0x144d 는 삼성이다(drivers/nvme/host/pci.c 의 check_vendor_combination_bug 이 0x144d 를 검사하고, 바로 아래 상류 영어 주석이 그 장치들을 Samsung 이라 부른다) */
pci_config_attr(device, "0x%04x\n"); /* [한국어] /sys/.../device — Device ID 16비트. vendor 와 함께 드라이버 매칭 키 */
pci_config_attr(subsystem_vendor, "0x%04x\n"); /* [한국어] /sys/.../subsystem_vendor — 완제품 제조사 구분용 16비트 */
pci_config_attr(subsystem_device, "0x%04x\n"); /* [한국어] /sys/.../subsystem_device — 같은 칩의 OEM 변종 구분용 16비트 */
pci_config_attr(revision, "0x%02x\n"); /* [한국어] /sys/.../revision — Revision ID 8비트. 실리콘 리비전 */
pci_config_attr(class, "0x%06x\n"); /* [한국어] /sys/.../class — Class Code 24비트(base/subclass/prog-if). NVMe 는 대용량 저장장치 + NVM + NVMe 인터페이스 */

/*
 * [한국어]
 * irq_show - /sys/bus/pci/devices/<BDF>/irq 파일 읽기 콜백
 *
 * @dev: sysfs 가 넘겨주는 struct device. 실제로는 struct pci_dev 안에 박혀
 *   있으므로 to_pci_dev() 로 되돌린다.
 * @attr: 어떤 속성이 읽혔는지 알려 주는 서술자. 이 함수는 속성이 하나뿐이라
 *   쓰지 않는다.
 * @buf: 드라이버 코어가 준 페이지 하나짜리 출력 버퍼.
 * @return: buf 에 채운 바이트 수. 그대로 read(2) 의 반환값이 된다.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 장치가 쓰는 인터럽트 번호 하나.
 * 다만 "하나" 라는 점이 함정이다. 상류 영어 주석이 말하듯, MSI 를 쓰는
 * 장치는 첫 번째 MSI 벡터의 IRQ 번호를, 그 밖의 모든 경우(레거시 INTx 는
 * 물론 MSI-X 도 포함)는 pdev->irq 에 들어 있는 레거시 INTx 번호를 보여 준다.
 * 벡터가 여러 개인 장치의 전체 목록은 이 파일로 알 수 없고
 * /proc/interrupts 나 장치 디렉터리의 msi_irqs/ 를 봐야 한다.
 *
 * NVMe 학습 관점: NVMe 는 큐마다 인터럽트를 하나씩 쓰려고 MSI-X 를 쓴다
 * (drivers/nvme/host/pci.c 가 pci_alloc_irq_vectors_affinity() 로 벡터를
 * 받고 pci_irq_vector() 로 큐별 IRQ 를 얻는 것이 근거다). 따라서 NVMe SSD 의
 * /sys/.../irq 값은 실제 완료 인터럽트와 아무 관계가 없다. 여기 숫자만 보고
 * "NVMe 인터럽트가 하나뿐" 이라고 결론 내리면 틀린다.
 *
 * 실행 컨텍스트: 사용자 프로세스가 read(2) 한 문맥. 락을 잡지 않으며,
 * 하드웨어에 접근하지 않고 pci_dev 에 캐시된 값만 읽으므로 런타임 PM 을
 * 깨울 필요도 없다.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [irq_show] -> pci_irq_vector / sysfs_emit
 */
static ssize_t irq_show(struct device *dev, /* [한국어] 읽힌 sysfs 파일의 주인 device */
			struct device_attribute *attr, /* [한국어] 속성 서술자 — 여기서는 미사용 */
			char *buf) /* [한국어] 출력 버퍼(페이지 시작 주소) */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] struct device 를 감싸고 있는 pci_dev 복원 */

#ifdef CONFIG_PCI_MSI /* [한국어] MSI 지원이 빌드에 들어간 경우에만 아래 분기가 존재 */
	/*
	 * For MSI, show the first MSI IRQ; for all other cases including
	 * MSI-X, show the legacy INTx IRQ.
	 */
	/* [한국어] 위 영어 주석의 뜻: msi_enabled 는 "MSI(MSI-X 아님)를 켰다" 는
	 * 플래그다. MSI 는 벡터들이 연속된 IRQ 번호를 갖기 때문에 첫 벡터만
	 * 보여 줘도 의미가 있지만, MSI-X 는 벡터마다 번호가 흩어져 있어 하나만
	 * 보여 주는 것이 오히려 오해를 부른다. 그래서 MSI-X 는 아래로 흘려보내
	 * 레거시 INTx 번호를 보여 준다. */
	if (pdev->msi_enabled) /* [한국어] MSI(MSI-X 는 아님)가 활성화된 장치인가 */
		return sysfs_emit(buf, "%u\n", pci_irq_vector(pdev, 0)); /* [한국어] 0번 벡터의 리눅스 IRQ 번호를 찍는다 */
#endif /* [한국어] CONFIG_PCI_MSI 분기 끝 */

	return sysfs_emit(buf, "%u\n", pdev->irq); /* [한국어] 그 외 전부: 열거 때 설정공간 0x3c(Interrupt Line)에서 온 레거시 INTx 번호 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(irq); /* [한국어] dev_attr_irq 생성 -> pci_dev_attrs[] 에 등록되어 /sys/.../irq 가 된다 */

/*
 * [한국어]
 * broken_parity_status_show - /sys/.../broken_parity_status 읽기 콜백
 *
 * @dev: 읽힌 파일의 주인 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 장치의 패리티 오류 보고를 "고장 난
 * 것으로 치고 무시하도록" 표시해 두었는지 여부(0 또는 1). 어떤 하드웨어는
 * 실제로는 멀쩡한데 설정공간 Status 레지스터의 패리티 오류 비트를 잘못
 * 세워 올린다. 그런 장치에 이 플래그를 세워 두면 커널이 그 보고를 무시한다.
 * 즉 이것은 하드웨어의 상태가 아니라 커널의 정책 플래그다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 캐시된 필드만 읽어 락도 PM 도 없다.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [broken_parity_status_show]
 */
static ssize_t broken_parity_status_show(struct device *dev, /* [한국어] 대상 device */
					 struct device_attribute *attr, /* [한국어] 속성 서술자 — 미사용 */
					 char *buf) /* [한국어] 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	return sysfs_emit(buf, "%u\n", pdev->broken_parity_status); /* [한국어] 비트필드 값을 그대로 0/1 로 출력 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * broken_parity_status_store - /sys/.../broken_parity_status 쓰기 콜백
 *
 * @dev: 쓰인 파일의 주인 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 사용자가 write(2) 로 넣은 문자열. NUL 로 끝난다는 보장이 sysfs
 *   규약상 주어진다.
 * @count: 사용자가 쓴 바이트 수.
 * @return: 성공 시 count(=다 소비했다는 뜻), 파싱 실패 시 -EINVAL.
 *   호출자인 sysfs 는 이 값을 write(2) 의 반환값으로 그대로 넘긴다.
 *
 * 이 파일에 0 이나 1 을 써서 위 플래그를 켜고 끈다. 여기에는 CAP_SYS_ADMIN
 * 검사가 없는데, 파일 모드가 DEVICE_ATTR_RW 의 기본값이라 쓰기는 소유자
 * (root)만 가능하고, 이 플래그 자체가 하드웨어를 건드리지 않는 순수 정책
 * 비트여서 위험도가 낮기 때문이다. 실제 위험한 store 들(enable, numa_node,
 * msi_bus, reset_subordinate)은 아래에서 capable(CAP_SYS_ADMIN) 을 직접 건다.
 *
 * kstrtoul(buf, 0, &val) 의 두 번째 인자 0 은 "진법을 문자열에서 추론하라"는
 * 뜻이다. 0x 로 시작하면 16진수, 0 으로 시작하면 8진수, 아니면 10진수로
 * 읽는다. 이 파일이 쓰는 모든 kstrtoul 호출이 같은 규약이라, 사용자가
 * "1" 이든 "0x1" 이든 쓸 수 있다.
 *
 * 실행 컨텍스트: write(2) 프로세스 문맥. 잠들 수 있고, 락은 잡지 않는다.
 *
 * 호출 체인:
 *   write(2) -> sysfs -> dev_attr_store -> [broken_parity_status_store]
 */
static ssize_t broken_parity_status_store(struct device *dev, /* [한국어] 대상 device */
					  struct device_attribute *attr, /* [한국어] 속성 서술자 — 미사용 */
					  const char *buf, size_t count) /* [한국어] 사용자 입력 문자열과 그 길이 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	unsigned long val; /* [한국어] 파싱된 사용자 값을 받을 자리 */

	if (kstrtoul(buf, 0, &val) < 0) /* [한국어] 진법 자동 판별 파싱. 숫자가 아니거나 넘치면 음수 */
		return -EINVAL; /* [한국어] 파싱 실패 -> 아무것도 바꾸지 않고 오류 반환 */

	pdev->broken_parity_status = !!val; /* [한국어] !! 로 0/1 로 정규화 — 이 필드는 1비트 비트필드라 2 이상을 넣으면 잘린다 */

	return count; /* [한국어] 입력을 전부 소비했다고 알림. 이보다 작으면 사용자 공간이 재시도한다 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RW(broken_parity_status); /* [한국어] dev_attr_broken_parity_status 생성(읽기+쓰기) */

/*
 * [한국어]
 * pci_dev_show_local_cpu - local_cpus 와 local_cpulist 의 공통 본체
 *
 * @dev: 대상 device(PCI 장치).
 * @list: 출력 형식 선택. false 면 비트마스크 16진 문자열("00000000,000000ff"),
 *   true 면 사람이 읽는 목록("0-7").
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 두 파일을 읽으면 알 수 있는 것: 이 PCI 장치와 "가까운" CPU 집합.
 * 즉 이 장치가 붙은 NUMA 노드의 CPU 들이다. 장치 DMA 버퍼를 어느 노드에
 * 잡을지, 인터럽트를 어느 CPU 에 묶을지 정할 때 사용자 공간(irqbalance,
 * numactl, 성능 튜닝 스크립트)이 읽는 값이다.
 *
 * 동작: CONFIG_NUMA 가 켜져 있으면 device 의 numa_node 로 판단한다. 노드가
 * 미지정(NUMA_NO_NODE)이면 "아무 CPU 나 괜찮다"는 뜻이므로 온라인 CPU 전체를
 * 돌려주고, 아니면 그 노드의 CPU 마스크를 돌려준다. NUMA 가 꺼진 커널에서는
 * 노드 개념이 없으므로 버스에서 유도한 마스크(cpumask_of_pcibus)를 쓴다.
 * 마지막에 cpumap_print_to_pagebuf() 가 마스크를 buf 에 찍는데, 이 함수는
 * buf 가 페이지 시작이라는 전제로 크기를 스스로 지키므로 별도 경계 검사가
 * 필요 없다.
 *
 * NVMe 학습 관점: NVMe 큐를 CPU 에 어떻게 매핑할지는 결국 이 정보에서
 * 출발한다. NVMe SSD 가 소켓 1 에 붙었는데 소켓 0 의 CPU 가 I/O 를 내면
 * 노드 간 홉이 생겨 지연이 늘어난다. 다만 커널 안에서 NVMe 드라이버가
 * 이 함수를 부르지는 않는다 — 이것은 순수히 사용자 공간에 보여 주는 창이다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   local_cpus_show / local_cpulist_show -> [pci_dev_show_local_cpu]
 *     -> cpumap_print_to_pagebuf
 */
static ssize_t pci_dev_show_local_cpu(struct device *dev, bool list, /* [한국어] list=false 면 마스크, true 면 범위 목록 형식 */
				      struct device_attribute *attr, char *buf) /* [한국어] 속성 서술자(미사용)와 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	const struct cpumask *mask; /* [한국어] 출력할 CPU 집합을 가리킬 포인터. 전역/노드 마스크를 가리키므로 해제 대상 아님 */

#ifdef CONFIG_NUMA /* [한국어] NUMA 를 아는 커널에서는 노드 정보를 쓴다 */
	if (dev_to_node(dev) == NUMA_NO_NODE) /* [한국어] 이 장치의 노드가 미지정인가(펌웨어가 알려 주지 않은 경우) */
		mask = cpu_online_mask; /* [한국어] 미지정이면 특정 노드로 좁힐 근거가 없으므로 온라인 CPU 전체 */
	else /* [한국어] 노드가 확정된 경우 */
		mask = cpumask_of_node(dev_to_node(dev)); /* [한국어] 그 노드에 속한 CPU 들만 */
#else /* [한국어] NUMA 가 꺼진 빌드 — 노드 개념 자체가 없다 */
	mask = cpumask_of_pcibus(to_pci_dev(dev)->bus); /* [한국어] 버스에서 유도한 마스크(아키텍처가 정의). 보통 전체 CPU */
#endif /* [한국어] CONFIG_NUMA 분기 끝 */
	return cpumap_print_to_pagebuf(list, buf, mask); /* [한국어] 마스크를 buf 에 형식대로 찍고 길이 반환. PAGE_SIZE 경계는 이 함수가 지킨다 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * local_cpus_show - /sys/bus/pci/devices/<BDF>/local_cpus 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(그대로 아래로 넘긴다).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 이 장치와 같은 NUMA 노드에 있는 CPU 집합이 비트마스크
 * 16진 문자열로 나온다(예: "0000,000000ff"). 사람이 읽기에는 local_cpulist
 * 가 편하지만, 스크립트가 마스크 그대로 필요할 때 이쪽을 쓴다.
 * 실제 계산은 전부 pci_dev_show_local_cpu() 에 있고 이 함수는 형식 선택
 * 인자 false 만 얹어 넘기는 얇은 껍데기다 — 같은 로직을 두 벌 두지 않기
 * 위한 구조다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [local_cpus_show]
 *     -> pci_dev_show_local_cpu(list=false)
 */
static ssize_t local_cpus_show(struct device *dev, /* [한국어] 대상 device */
			       struct device_attribute *attr, char *buf) /* [한국어] 속성 서술자와 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	return pci_dev_show_local_cpu(dev, false, attr, buf); /* [한국어] false = 비트마스크 형식으로 출력 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(local_cpus); /* [한국어] dev_attr_local_cpus 생성 -> /sys/.../local_cpus */

/*
 * [한국어]
 * local_cpulist_show - /sys/bus/pci/devices/<BDF>/local_cpulist 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(그대로 아래로 넘긴다).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 위 local_cpus 와 같은 정보가 사람이 읽는 범위 목록
 * 형식으로 나온다(예: "0-15,32-47"). NVMe 장치 디렉터리에서 이 파일을 읽으면
 * "이 SSD 와 같은 소켓의 CPU 가 어디까지인가" 를 바로 알 수 있어, 큐 개수나
 * IRQ affinity 를 손으로 조정할 때 가장 먼저 보는 파일이다.
 * 본체는 pci_dev_show_local_cpu() 이며 여기서는 list=true 만 얹는다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [local_cpulist_show]
 *     -> pci_dev_show_local_cpu(list=true)
 */
static ssize_t local_cpulist_show(struct device *dev, /* [한국어] 대상 device */
				  struct device_attribute *attr, char *buf) /* [한국어] 속성 서술자와 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	return pci_dev_show_local_cpu(dev, true, attr, buf); /* [한국어] true = "0-7" 같은 범위 목록 형식으로 출력 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(local_cpulist); /* [한국어] dev_attr_local_cpulist 생성 -> /sys/.../local_cpulist */

/*
 * PCI Bus Class Devices
 */
/*
 * [한국어]
 * cpuaffinity_show - /sys/class/pci_bus/<도메인:버스>/cpuaffinity 읽기 콜백
 *
 * @dev: 대상 device. 여기서는 pci_dev 가 아니라 struct pci_bus 안에 박힌
 *   device 다 — 그래서 to_pci_dev() 가 아니라 to_pci_bus() 를 쓴다.
 *   이 속성은 pcibus_groups[] 를 통해 pcibus_class 에 걸리며,
 *   그 등록은 drivers/pci/probe.c 의 pcibus_class 정의에서 이뤄진다.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 PCI 버스 전체와 가까운 CPU 집합이
 * 비트마스크 형식으로 나온다. 앞의 local_cpus 는 장치 단위, 이것은 버스
 * 단위라는 점만 다르다. 루트 버스에서 읽으면 그 호스트 브리지가 어느
 * 소켓에 물려 있는지가 드러난다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 락 없음. cpumask_of_pcibus() 는
 * 아키텍처가 제공하는 매핑이고, 결과는 정적 마스크를 가리키므로 해제하지
 * 않는다.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [cpuaffinity_show]
 *     -> cpumap_print_to_pagebuf
 */
static ssize_t cpuaffinity_show(struct device *dev, /* [한국어] pci_bus 에 박힌 device */
				struct device_attribute *attr, char *buf) /* [한국어] 속성 서술자(미사용)와 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	const struct cpumask *cpumask = cpumask_of_pcibus(to_pci_bus(dev)); /* [한국어] device -> pci_bus 복원 후, 그 버스에 가까운 CPU 마스크 조회 */

	return cpumap_print_to_pagebuf(false, buf, cpumask); /* [한국어] false = 비트마스크 16진 형식. 길이 관리는 이 함수가 한다 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(cpuaffinity); /* [한국어] dev_attr_cpuaffinity 생성 -> pcibus_attrs[] 를 거쳐 버스 디렉터리에 붙는다 */

/*
 * [한국어]
 * cpulistaffinity_show - /sys/class/pci_bus/<도메인:버스>/cpulistaffinity 읽기 콜백
 *
 * @dev: pci_bus 에 박힌 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * cpuaffinity 와 같은 정보를 사람이 읽는 범위 목록("0-15")으로 보여 준다.
 * 두 함수를 합치지 않고 나란히 둔 것은, 장치 쪽(local_cpus/local_cpulist)과
 * 달리 본체가 두 줄뿐이라 공통 함수를 뽑을 이득이 없기 때문이다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [cpulistaffinity_show]
 *     -> cpumap_print_to_pagebuf
 */
static ssize_t cpulistaffinity_show(struct device *dev, /* [한국어] pci_bus 에 박힌 device */
				    struct device_attribute *attr, char *buf) /* [한국어] 속성 서술자(미사용)와 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	const struct cpumask *cpumask = cpumask_of_pcibus(to_pci_bus(dev)); /* [한국어] 버스에 가까운 CPU 마스크 조회 */

	return cpumap_print_to_pagebuf(true, buf, cpumask); /* [한국어] true = "0-15" 같은 범위 목록 형식 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(cpulistaffinity); /* [한국어] dev_attr_cpulistaffinity 생성 -> 버스 디렉터리에 붙는다 */

/*
 * [한국어]
 * power_state_show - /sys/bus/pci/devices/<BDF>/power_state 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 커널이 파악하고 있는 이 장치의 PCI 전원
 * 상태 이름("D0", "D1", "D2", "D3hot", "D3cold", "unknown").
 * D0 가 완전 동작 상태이고 숫자가 커질수록 더 많이 잠든 상태이며,
 * D3cold 는 전원 자체가 끊긴 상태다.
 *
 * 주의할 점: 이 값은 pdev->current_state 라는 커널의 기록이지, 지금 하드웨어
 * 레지스터를 읽어 온 값이 아니다. 그래서 이 show 는 런타임 PM 을 깨우지
 * 않는다(깨우면 값이 바뀌어 버려 관측 자체가 무의미해진다).
 *
 * NVMe 학습 관점: 여기 보이는 D 상태는 PCI 링크/장치 수준의 전원 상태이고,
 * NVMe 스펙의 APST(Autonomous Power State Transition)나 NVMe Power State
 * (PS0..PSn)와는 다른 층위다. NVMe 컨트롤러가 자기 내부 전력 상태를 낮추는
 * 것은 PCI 입장에서는 여전히 D0 다. 둘을 섞어 읽으면 안 된다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [power_state_show] -> pci_power_name
 */
static ssize_t power_state_show(struct device *dev, /* [한국어] 대상 device */
				struct device_attribute *attr, char *buf) /* [한국어] 속성 서술자(미사용)와 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */

	return sysfs_emit(buf, "%s\n", pci_power_name(pdev->current_state)); /* [한국어] 커널이 기록해 둔 현재 D 상태를 이름 문자열로 변환해 출력 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(power_state); /* [한국어] dev_attr_power_state 생성 -> /sys/.../power_state */

/* show resources */
/*
 * [한국어]
 * resource_show - /sys/bus/pci/devices/<BDF>/resource 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼(페이지 시작).
 * @return: 찍은 총 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 장치에 할당된 모든 자원 창의 물리 주소
 * 범위와 속성 플래그가 한 줄에 하나씩, "시작 끝 플래그" 세 개의 64비트
 * 16진수로 나온다. 줄 번호가 곧 resource[] 배열 인덱스이므로, 0번째 줄이
 * BAR0, 1번째 줄이 BAR1 이다. 즉 이 텍스트 파일의 N번째 줄이 바이너리 파일
 * resourceN 이 매핑하는 영역과 같은 것을 가리킨다.
 * 자원이 할당되지 않은 BAR 는 시작과 끝이 모두 0 으로 나온다.
 *
 * resource[] 배열의 구획은 drivers/pci/pci.h 에 적혀 있다: 앞쪽이 표준 BAR
 * 0~5, 그다음이 확장 ROM(PCI_ROM_RESOURCE), SR-IOV 를 쓰면 VF BAR,
 * 마지막이 브리지 윈도우(PCI_BRIDGE_RESOURCES..PCI_BRIDGE_RESOURCE_END) 다.
 *
 * 왜 max 를 나누는가: 브리지가 아닌 일반 장치에는 브리지 윈도우가 의미가
 * 없으므로 PCI_BRIDGE_RESOURCES 앞까지만 찍는다. 하위 버스를 거느린
 * 브리지라면 DEVICE_COUNT_RESOURCE 까지 전부 찍어 브리지 윈도우도 보여 준다.
 * 두 상수의 정의는 include/linux/pci.h 에 있고 이 트리에는 그 헤더가 없어
 * 원문은 확인할 수 없다 — 다만 이 코드에서 "브리지 윈도우 구획의 시작"과
 * "배열 전체 크기" 로 쓰인다는 것은 분명하다.
 *
 * NVMe 학습 관점: NVMe SSD 는 브리지가 아니므로 BAR 0~5 와 ROM 까지만
 * 나온다. 실질적으로 값이 채워지는 것은 BAR0 하나로, 컨트롤러 레지스터와
 * 도어벨 창이 거기에 있다. drivers/nvme/host/pci.c 의 nvme_remap_bar() 가
 * pci_resource_start(pdev, 0) 과 pci_resource_len(pdev, 0) 을 쓰는 것이
 * 근거이며, 이 파일이 찍는 0번째 줄이 바로 그 값이다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 락 없음. 하드웨어가 아니라
 * 커널이 이미 할당해 둔 resource[] 를 읽는다.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [resource_show]
 *     -> pci_resource_to_user -> sysfs_emit_at
 */
/* show resources */
static ssize_t resource_show(struct device *dev, struct device_attribute *attr, /* [한국어] 대상 device 와 속성 서술자(미사용) */
			     char *buf) /* [한국어] 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	int i; /* [한국어] resource[] 를 훑는 인덱스 */
	int max; /* [한국어] 어디까지 찍을지의 상한 — 브리지인지에 따라 달라진다 */
	resource_size_t start, end; /* [한국어] 사용자에게 보여 줄 시작/끝 주소. 아키텍처에 따라 32/64비트 */
	size_t len = 0; /* [한국어] 지금까지 buf 에 찍은 누적 길이. sysfs_emit_at 의 오프셋으로 쓰인다 */

	if (pci_dev->subordinate) /* [한국어] 하위 버스를 거느린 장치인가 = 브리지인가 */
		max = DEVICE_COUNT_RESOURCE; /* [한국어] 브리지면 브리지 윈도우까지 배열 전체를 찍는다 */
	else /* [한국어] 일반 엔드포인트(예: NVMe SSD) */
		max = PCI_BRIDGE_RESOURCES; /* [한국어] 브리지 윈도우 구획 직전까지만 — 그 뒤는 의미가 없다 */

	for (i = 0; i < max; i++) { /* [한국어] 자원 하나마다 한 줄씩 */
		struct resource *res =  &pci_dev->resource[i]; /* [한국어] i 번째 자원 서술자. 아래에서 조건에 따라 다른 곳을 가리키게 바뀔 수 있다 */
		struct resource zerores = {}; /* [한국어] 전부 0 인 더미 자원. 아래 호환성 처리에서 "빈 값"을 찍기 위해 쓴다 */

		/* For backwards compatibility */
		/* [한국어] 위 영어 주석의 뜻: 브리지 윈도우가 아직 배정되지 않았거나
		 * (IORESOURCE_UNSET) 꺼져 있으면(IORESOURCE_DISABLED), 예전 커널처럼
		 * 0 0 0 을 찍어 준다. 그렇게 하지 않으면 내부적으로 들고 있는
		 * 쓰레기 범위가 그대로 새어 나가, 이 파일을 파싱하는 오래된 도구가
		 * 존재하지 않는 윈도우를 실재하는 것으로 오해한다. */
		if (pci_resource_is_bridge_win(i) && /* [한국어] i 가 브리지 윈도우 구획인가(drivers/pci/pci.h 의 인라인 함수) */
		    res->flags & (IORESOURCE_UNSET | IORESOURCE_DISABLED)) /* [한국어] 미배정 또는 비활성 비트가 하나라도 서 있는가 */
			res = &zerores; /* [한국어] 실제 자원 대신 0 짜리 더미를 가리키게 바꿔, 아래에서 0 0 0 이 찍히게 한다 */

		pci_resource_to_user(pci_dev, i, res, &start, &end); /* [한국어] 커널 내부 주소를 사용자에게 보여 줄 주소로 변환. 아키텍처에 따라 버스 주소와 CPU 물리 주소가 다를 수 있어 반드시 거쳐야 한다 */
		len += sysfs_emit_at(buf, len, "0x%016llx 0x%016llx 0x%016llx\n", /* [한국어] 이미 찍은 len 바이트 뒤에 이어 붙인다. sysfs_emit_at 이 PAGE_SIZE 경계를 스스로 지키므로 넘칠 걱정이 없다 */
				     (unsigned long long)start, /* [한국어] 시작 물리 주소. resource_size_t 폭이 아키텍처마다 달라 형식과 맞추려 캐스팅 */
				     (unsigned long long)end, /* [한국어] 끝 물리 주소(이 주소까지 포함). 크기는 end - start + 1 */
				     (unsigned long long)res->flags); /* [한국어] IORESOURCE_MEM/IO/PREFETCH 등 속성 비트. 어떤 BAR 가 prefetchable 인지 여기서 읽는다 */
	} /* [한국어] 자원 루프 종료 */
	return len; /* [한국어] 누적 길이 = read(2) 가 돌려받을 바이트 수 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(resource); /* [한국어] dev_attr_resource 생성 -> /sys/.../resource */

/*
 * [한국어]
 * max_link_speed_show - /sys/bus/pci/devices/<BDF>/max_link_speed 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 PCIe 장치가 "낼 수 있는" 최대 링크
 * 속도가 사람이 읽는 문자열로 나온다("2.5 GT/s PCIe", "8.0 GT/s PCIe" 등).
 * 이것은 능력(capability)이지 현재 상태가 아니다. 지금 실제로 협상된 속도는
 * 아래 current_link_speed 다. 두 값을 비교하는 것이 PCIe 성능 진단의 첫
 * 단계다 — max 는 Gen4 인데 current 가 Gen1 이면 슬롯 배선, 라이저,
 * 링크 훈련 실패, 전원 절약 정책 중 하나를 의심한다.
 *
 * 이 속성은 pcie_dev_attrs[] 에 들어 있고 그 그룹의 is_visible 이
 * pcie_dev_attrs_are_visible() 이라, PCIe 장치가 아니면 파일 자체가 생기지
 * 않는다. 그래서 여기서 다시 PCIe 여부를 검사하지 않는다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. pcie_get_speed_cap() 안에서 설정공간
 * 접근이 일어날 수 있으나, 여기서는 런타임 PM 을 잡지 않는다 — 바로 아래
 * max_link_width_show 는 잡는다는 점과 대비된다(상류 코드의 비대칭이며,
 * 왜 그런지는 이 트리의 정보만으로는 확인할 수 없다).
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [max_link_speed_show]
 *     -> pcie_get_speed_cap -> pci_speed_string
 */
static ssize_t max_link_speed_show(struct device *dev, /* [한국어] 대상 device */
				   struct device_attribute *attr, char *buf) /* [한국어] 속성 서술자(미사용)와 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */

	return sysfs_emit(buf, "%s\n", /* [한국어] 문자열 한 줄로 출력 */
			  pci_speed_string(pcie_get_speed_cap(pdev))); /* [한국어] 링크 능력 레지스터에서 최대 속도를 얻어(pcie_get_speed_cap) 사람이 읽는 이름으로 변환(pci_speed_string) */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(max_link_speed); /* [한국어] dev_attr_max_link_speed 생성 -> pcie_dev_attrs[] 경유로 PCIe 장치에만 생긴다 */

/*
 * [한국어]
 * max_link_width_show - /sys/bus/pci/devices/<BDF>/max_link_width 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 장치가 지원하는 최대 레인 수
 * (1, 2, 4, 8, 16 등). NVMe SSD 는 대개 4 다. 이 값이 4 인데
 * current_link_width 가 1 이나 2 로 나오면 슬롯이 x4 로 배선되지 않았거나
 * 레인 일부가 훈련에 실패한 것이다 — 대역폭이 그만큼 줄어든다.
 *
 * 위 max_link_speed_show 와 달리 여기서는 런타임 PM 을 명시적으로 잡는다.
 * 상류 영어 주석이 그 이유를 밝힌다: PCI_EXP_LNKCAP 레지스터를 실제로
 * 읽어야 하므로 장치가 접근 가능한 상태여야 한다는 것이다. 장치가
 * D3cold 로 내려가 있으면 설정공간 읽기가 전부 0xffffffff 로 돌아온다.
 * pci_config_pm_runtime_get() 이 장치를 잠시 깨우고,
 * pci_config_pm_runtime_put() 이 다시 잠들 수 있게 놓아 준다. 이 쌍을
 * 짝맞추지 않으면 장치가 영원히 깨어 있게 되어 전력이 샌다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 런타임 PM 을 깨우는 동안 잠들 수
 * 있으므로 원자적 문맥에서는 부를 수 없다.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [max_link_width_show]
 *     -> pci_config_pm_runtime_get -> pcie_get_width_cap -> pci_config_pm_runtime_put
 */
static ssize_t max_link_width_show(struct device *dev, /* [한국어] 대상 device */
				   struct device_attribute *attr, char *buf) /* [한국어] 속성 서술자(미사용)와 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	ssize_t ret; /* [한국어] sysfs_emit 결과를 담아 뒀다가 PM 해제 뒤에 반환하기 위한 임시 변수 */

	/* We read PCI_EXP_LNKCAP, so we need the device to be accessible. */
	pci_config_pm_runtime_get(pdev); /* [한국어] 장치를 D0 로 깨워 설정공간 접근이 유효하게 만든다. 실패해도 반환값이 없으므로 아래는 그대로 진행한다 */
	ret = sysfs_emit(buf, "%u\n", pcie_get_width_cap(pdev)); /* [한국어] Link Capabilities 레지스터의 Maximum Link Width 필드를 레인 수로 출력 */
	pci_config_pm_runtime_put(pdev); /* [한국어] get 과 반드시 짝을 이뤄야 한다. 여기서 놓아 줘야 장치가 다시 절전 상태로 갈 수 있다 */

	return ret; /* [한국어] PM 해제를 먼저 하려고 값을 잡아 두었다가 이제 반환 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(max_link_width); /* [한국어] dev_attr_max_link_width 생성 -> PCIe 장치에만 생긴다 */

/*
 * [한국어]
 * current_link_speed_show - /sys/bus/pci/devices/<BDF>/current_link_speed 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 성공 시 찍은 바이트 수, 설정공간 읽기 실패 시 -EINVAL.
 *   호출자인 sysfs 가 음수를 그대로 read(2) 의 errno 로 바꿔 준다.
 *
 * 이 파일을 읽으면 알 수 있는 것: 지금 실제로 협상되어 동작 중인 링크 속도.
 * 기대보다 낮으면 슬롯 배선이나 라이저 품질, 링크 훈련 실패, 또는 절전
 * 정책으로 링크가 낮은 속도로 떨어진 상황을 의심한다. NVMe SSD 의 순차
 * 대역폭이 스펙의 절반밖에 안 나올 때 가장 먼저 확인해야 할 파일이다.
 *
 * 동작 단계:
 *  1) 설정공간을 실제로 읽어야 하므로 런타임 PM 으로 장치를 깨운다.
 *  2) PCIe Capability 안의 Link Status 레지스터(PCI_EXP_LNKSTA)를 16비트로
 *     읽는다. pcie_capability_read_word() 는 PCIe Capability 구조체의
 *     시작 오프셋을 알아서 더해 주므로 절대 오프셋을 쓸 필요가 없다.
 *  3) 하위 4비트인 Current Link Speed 필드(PCI_EXP_LNKSTA_CLS)를 떼어
 *     pcie_link_speed[] 표의 인덱스로 쓴다. 그 표는 drivers/pci/probe.c 에
 *     있으며 16칸이라 4비트 값 어느 것으로 인덱싱해도 배열을 벗어나지
 *     않는다 — 그래서 여기서 범위 검사를 하지 않아도 안전하다. 값 1~6 이
 *     각각 2.5/5.0/8.0/16.0/32.0/64.0 GT/s(Gen1~Gen6)이고 나머지는
 *     PCI_SPEED_UNKNOWN 이다.
 *  4) pci_speed_string() 으로 사람이 읽는 문자열로 바꿔 찍는다.
 *
 * 에러 경로: 읽기가 실패하면(장치가 사라졌거나 PCIe Capability 가 없으면)
 * 값을 지어내지 않고 -EINVAL 을 돌려준다. 이때도 PM 해제는 이미 위에서
 * 끝났으므로 참조가 새지 않는다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 설정공간 접근 때문에 잠들 수 있다.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [current_link_speed_show]
 *     -> pcie_capability_read_word -> pcie_link_speed[] -> pci_speed_string
 */
static ssize_t current_link_speed_show(struct device *dev, /* [한국어] 대상 device */
				       struct device_attribute *attr, char *buf) /* [한국어] 속성 서술자(미사용)와 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	u16 linkstat; /* [한국어] PCIe Link Status 레지스터 원본 16비트 값 */
	int err; /* [한국어] 설정공간 읽기 성공 여부(0 이 성공) */
	enum pci_bus_speed speed; /* [한국어] 표에서 찾아낸 속도 열거값 */

	pci_config_pm_runtime_get(pci_dev); /* [한국어] 설정공간을 읽으려면 장치가 D0 여야 하므로 깨운다 */
	err = pcie_capability_read_word(pci_dev, PCI_EXP_LNKSTA, &linkstat); /* [한국어] PCIe Capability 기준 상대 오프셋으로 Link Status 를 읽는다. 장치에 PCIe Capability 가 없으면 오류 */
	pci_config_pm_runtime_put(pci_dev); /* [한국어] 오류든 성공이든 먼저 PM 참조를 놓는다 — 아래 조기 return 에서 새지 않도록 */

	if (err) /* [한국어] 읽기 실패(장치 사라짐, Capability 없음 등) */
		return -EINVAL; /* [한국어] 잘못된 값을 찍느니 오류를 돌려준다 */

	speed = pcie_link_speed[linkstat & PCI_EXP_LNKSTA_CLS]; /* [한국어] 하위 4비트 Current Link Speed 필드를 그대로 인덱스로. 표가 16칸이라 범위 초과가 구조적으로 불가능하다 */

	return sysfs_emit(buf, "%s\n", pci_speed_string(speed)); /* [한국어] "8.0 GT/s PCIe" 같은 문자열로 변환해 출력 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(current_link_speed); /* [한국어] dev_attr_current_link_speed 생성 -> PCIe 장치에만 생긴다 */

/*
 * [한국어]
 * current_link_width_show - /sys/bus/pci/devices/<BDF>/current_link_width 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 성공 시 찍은 바이트 수, 설정공간 읽기 실패 시 -EINVAL.
 *
 * 이 파일을 읽으면 알 수 있는 것: 지금 협상되어 동작 중인 레인 수.
 * max_link_width 보다 작으면 레인 일부가 붙지 않은 것이다. x4 짜리 NVMe SSD
 * 가 x2 로 붙었다면 대역폭이 절반이 되므로, 벤치마크 수치가 이상할 때
 * current_link_speed 와 함께 반드시 확인한다.
 *
 * 위 current_link_speed_show 와 같은 레지스터(Link Status)를 읽지만, 여기서는
 * 속도 필드 대신 Negotiated Link Width 필드를 뽑는다. 그 필드는 연속된 여러
 * 비트라 단순 AND 로는 안 되고, FIELD_GET(마스크, 값) 이 마스크 위치만큼
 * 오른쪽으로 밀어 준다 — 시프트 상수를 손으로 쓰지 않아도 되고, 마스크와
 * 시프트가 어긋나는 실수를 막는다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 설정공간 접근 때문에 잠들 수 있다.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [current_link_width_show]
 *     -> pcie_capability_read_word -> FIELD_GET -> sysfs_emit
 */
static ssize_t current_link_width_show(struct device *dev, /* [한국어] 대상 device */
				       struct device_attribute *attr, char *buf) /* [한국어] 속성 서술자(미사용)와 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	u16 linkstat; /* [한국어] Link Status 레지스터 원본 16비트 값 */
	int err; /* [한국어] 설정공간 읽기 성공 여부(0 이 성공) */

	pci_config_pm_runtime_get(pci_dev); /* [한국어] 설정공간 접근 전에 장치를 깨운다 */
	err = pcie_capability_read_word(pci_dev, PCI_EXP_LNKSTA, &linkstat); /* [한국어] 같은 Link Status 레지스터를 읽는다 */
	pci_config_pm_runtime_put(pci_dev); /* [한국어] 조기 return 전에 PM 참조를 먼저 놓는다 */

	if (err) /* [한국어] 읽기 실패 */
		return -EINVAL; /* [한국어] 값을 지어내지 않고 오류 반환 */

	return sysfs_emit(buf, "%u\n", FIELD_GET(PCI_EXP_LNKSTA_NLW, linkstat)); /* [한국어] Negotiated Link Width 필드만 뽑아 레인 수로 출력. FIELD_GET 이 마스크에 맞춰 시프트까지 처리한다 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(current_link_width); /* [한국어] dev_attr_current_link_width 생성 -> PCIe 장치에만 생긴다 */

/*
 * [한국어]
 * secondary_bus_number_show - /sys/bus/pci/devices/<BDF>/secondary_bus_number 읽기 콜백
 *
 * @dev: 대상 device. 이 속성은 pci_bridge_attrs[] 에 있고 그 그룹의
 *   is_visible 이 pci_bridge_attrs_are_visible() 이라, 브리지가 아닌
 *   장치에는 파일 자체가 만들어지지 않는다.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 성공 시 찍은 바이트 수, 설정공간 읽기 실패 시 -EINVAL.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 브리지 바로 뒤에 있는 버스의 번호.
 * PCI 브리지는 Type 1 설정공간 헤더를 쓰는데, 그 안에 Primary(내가 붙은
 * 상위 버스), Secondary(내 바로 아래 버스), Subordinate(내 아래 전체에서
 * 가장 큰 버스 번호) 세 개의 버스 번호가 1바이트씩 들어 있다.
 * PCI_SECONDARY_BUS 는 그중 Secondary 필드의 오프셋이며, 상수의 정의는
 * include/uapi/linux/pci_regs.h 에 있다 — 이 트리에는 그 헤더가 없어
 * 숫자값 원문은 확인할 수 없다.
 *
 * 왜 캐시된 값 대신 하드웨어를 읽는가: 열거 때 커널이 배정한 번호와
 * 하드웨어에 실제로 쓰여 있는 값이 어긋날 수 있기 때문이다(펌웨어가 나중에
 * 건드렸거나 hot-reset 이 있었던 경우). 그래서 진짜 레지스터를 읽고, 그러기
 * 위해 런타임 PM 으로 장치를 깨운다.
 *
 * NVMe 학습 관점: NVMe SSD 자신에게는 이 파일이 없다(엔드포인트이므로).
 * 대신 SSD 의 부모 디렉터리(루트 포트나 스위치 다운스트림 포트)에서 읽으면
 * SSD 가 어느 버스에 앉아 있는지가 나오며, 이는 장치 주소 <BDF> 의 버스
 * 부분과 일치한다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 설정공간 접근 때문에 잠들 수 있다.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [secondary_bus_number_show]
 *     -> pci_read_config_byte
 */
static ssize_t secondary_bus_number_show(struct device *dev, /* [한국어] 대상 device(브리지) */
					 struct device_attribute *attr, /* [한국어] 속성 서술자 — 미사용 */
					 char *buf) /* [한국어] 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	u8 sec_bus; /* [한국어] 읽어 올 Secondary Bus Number(1바이트, 0~255) */
	int err; /* [한국어] 설정공간 읽기 성공 여부(0 이 성공) */

	pci_config_pm_runtime_get(pci_dev); /* [한국어] 실제 레지스터를 읽어야 하므로 장치를 D0 로 깨운다 */
	err = pci_read_config_byte(pci_dev, PCI_SECONDARY_BUS, &sec_bus); /* [한국어] Type 1 헤더의 Secondary Bus Number 를 1바이트 읽는다 */
	pci_config_pm_runtime_put(pci_dev); /* [한국어] 조기 return 전에 PM 참조를 먼저 놓는다 */

	if (err) /* [한국어] 읽기 실패(장치 사라짐 등) */
		return -EINVAL; /* [한국어] 값을 지어내지 않고 오류 반환 */

	return sysfs_emit(buf, "%u\n", sec_bus); /* [한국어] 버스 번호를 10진수로 출력 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(secondary_bus_number); /* [한국어] dev_attr_secondary_bus_number 생성 -> 브리지에만 생긴다 */

/*
 * [한국어]
 * subordinate_bus_number_show - /sys/bus/pci/devices/<BDF>/subordinate_bus_number 읽기 콜백
 *
 * @dev: 대상 device(브리지에만 파일이 생긴다).
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 성공 시 찍은 바이트 수, 설정공간 읽기 실패 시 -EINVAL.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 브리지 아래에 존재하는 버스 번호 중
 * 가장 큰 값. 즉 이 브리지가 담당하는 버스 번호 구간의 끝이다.
 * secondary_bus_number 와 짝을 이뤄 [secondary, subordinate] 구간을 이루며,
 * 브리지는 그 구간에 드는 트랜잭션만 아래로 흘려보낸다. 두 값이 같으면
 * 이 브리지 아래에 버스가 하나뿐이라는 뜻이고, 차이가 크면 스위치가 여러
 * 단으로 붙어 있다는 뜻이다.
 *
 * 이 값이 부족하면(즉 구간을 너무 좁게 잡아 두면) 나중에 hotplug 로 장치를
 * 꽂아도 버스 번호를 줄 수 없어 열거가 실패한다 — 그래서 디버깅할 때
 * 실제로 들여다보는 값이다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 설정공간 접근 때문에 잠들 수 있다.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [subordinate_bus_number_show]
 *     -> pci_read_config_byte
 */
static ssize_t subordinate_bus_number_show(struct device *dev, /* [한국어] 대상 device(브리지) */
					   struct device_attribute *attr, /* [한국어] 속성 서술자 — 미사용 */
					   char *buf) /* [한국어] 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	u8 sub_bus; /* [한국어] 읽어 올 Subordinate Bus Number(1바이트, 0~255) */
	int err; /* [한국어] 설정공간 읽기 성공 여부(0 이 성공) */

	pci_config_pm_runtime_get(pci_dev); /* [한국어] 실제 레지스터를 읽어야 하므로 장치를 깨운다 */
	err = pci_read_config_byte(pci_dev, PCI_SUBORDINATE_BUS, &sub_bus); /* [한국어] Type 1 헤더의 Subordinate Bus Number 를 1바이트 읽는다 */
	pci_config_pm_runtime_put(pci_dev); /* [한국어] 조기 return 전에 PM 참조를 먼저 놓는다 */

	if (err) /* [한국어] 읽기 실패 */
		return -EINVAL; /* [한국어] 값을 지어내지 않고 오류 반환 */

	return sysfs_emit(buf, "%u\n", sub_bus); /* [한국어] 버스 번호를 10진수로 출력 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(subordinate_bus_number); /* [한국어] dev_attr_subordinate_bus_number 생성 -> 브리지에만 생긴다 */

/*
 * [한국어]
 * ari_enabled_show - /sys/bus/pci/devices/<BDF>/ari_enabled 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 장치가 붙은 버스에서 ARI
 * (Alternative Routing-ID Interpretation)가 켜져 있는지(0 또는 1).
 * 보통 PCI 주소는 8비트 버스 + 5비트 장치 + 3비트 기능으로 쪼개져 한 장치에
 * 기능이 최대 8개다. ARI 를 켜면 장치/기능 8비트를 통째로 기능 번호로 써서
 * 한 장치가 최대 256개의 기능을 가질 수 있다. SR-IOV 로 VF 를 많이 만들 때
 * 필수적인 확장이다.
 *
 * 인자가 pci_dev 가 아니라 pci_dev->bus 인 점에 주목: ARI 는 개별 장치가
 * 아니라 상위 브리지가 켜 주는 링크 단위 설정이라, "이 장치가 앉은 버스에서
 * ARI 포워딩이 켜졌는가" 를 묻는 것이 정확한 질문이다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. pci_ari_enabled() 는 버스 플래그를
 * 읽을 뿐이라 하드웨어 접근이 없고 락도 필요 없다.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [ari_enabled_show] -> pci_ari_enabled
 */
static ssize_t ari_enabled_show(struct device *dev, /* [한국어] 대상 device */
				struct device_attribute *attr, /* [한국어] 속성 서술자 — 미사용 */
				char *buf) /* [한국어] 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */

	return sysfs_emit(buf, "%u\n", pci_ari_enabled(pci_dev->bus)); /* [한국어] 장치가 아니라 그 장치가 앉은 버스에 ARI 포워딩이 켜졌는지를 묻는다 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(ari_enabled); /* [한국어] dev_attr_ari_enabled 생성 -> /sys/.../ari_enabled */

/*
 * [한국어]
 * modalias_show - /sys/bus/pci/devices/<BDF>/modalias 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 장치를 가리키는 모듈 별칭 문자열.
 * 형식은 "pci:v<벤더>d<장치>sv<서브벤더>sd<서브장치>bc<베이스클래스>
 * sc<서브클래스>i<프로그래밍 인터페이스>" 이고, 각 필드는 고정 폭 대문자
 * 16진수다. udev 와 modprobe 가 이 문자열을 모듈들이 등록해 둔 별칭 패턴
 * (MODULE_DEVICE_TABLE 로 만들어진 것)과 맞춰 보고 어떤 드라이버를 올릴지
 * 결정한다. 즉 "이 장치에 왜 이 드라이버가 붙었나" 를 추적하는 출발점이다.
 *
 * 클래스 코드는 pci_dev->class 안에 24비트로 뭉쳐 있어서 바이트 세 개로
 * 쪼개야 한다: 16비트 오른쪽 시프트가 base class, 8비트가 subclass,
 * 그대로가 programming interface 다. (u8) 캐스팅이 상위 비트를 잘라 준다.
 *
 * NVMe 학습 관점: NVMe 드라이버는 벤더/장치가 아니라 클래스로 매칭하는
 * 항목을 갖고 있다 — drivers/nvme/host/pci.c 의 id 테이블 마지막에
 * PCI_DEVICE_CLASS(PCI_CLASS_STORAGE_EXPRESS, 0xffffff) 가 있고,
 * MODULE_DEVICE_TABLE(pci, nvme_id_table) 로 별칭이 만들어진다. 그래서
 * 처음 보는 벤더의 NVMe SSD 라도 이 modalias 의 bc/sc/i 부분만으로 nvme
 * 모듈이 자동으로 올라온다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 캐시된 필드만 읽어 락도 PM 도 없다.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [modalias_show] -> sysfs_emit
 */
static ssize_t modalias_show(struct device *dev, struct device_attribute *attr, /* [한국어] 대상 device 와 속성 서술자(미사용) */
			     char *buf) /* [한국어] 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */

	return sysfs_emit(buf, "pci:v%08Xd%08Xsv%08Xsd%08Xbc%02Xsc%02Xi%02X\n", /* [한국어] 별칭 형식. 폭이 고정(%08X, %02X)이라야 modprobe 의 패턴 매칭이 성립한다 */
			  pci_dev->vendor, pci_dev->device, /* [한국어] v = Vendor ID, d = Device ID */
			  pci_dev->subsystem_vendor, pci_dev->subsystem_device, /* [한국어] sv = Subsystem Vendor ID, sd = Subsystem Device ID */
			  (u8)(pci_dev->class >> 16), (u8)(pci_dev->class >> 8), /* [한국어] bc = base class(클래스 24비트의 상위 바이트), sc = subclass(가운데 바이트) */
			  (u8)(pci_dev->class)); /* [한국어] i = programming interface(하위 바이트). NVMe 는 이 세 바이트로 매칭된다 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(modalias); /* [한국어] dev_attr_modalias 생성 -> /sys/.../modalias */

/*
 * [한국어]
 * enable_store - /sys/bus/pci/devices/<BDF>/enable 쓰기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 사용자가 쓴 문자열. 0 이 아니면 활성화, 0 이면 비활성화 요청.
 * @count: 쓴 바이트 수.
 * @return: 성공 시 count, 권한 없으면 -EPERM, 파싱 실패면 -EINVAL,
 *   드라이버가 이미 붙어 있으면 -EBUSY, 켜져 있지도 않은데 끄라고 하면
 *   -EIO, pci_enable_device() 자체가 실패하면 그 오류를 그대로 전달한다.
 *
 * 이 파일에 1 을 쓰면 pci_enable_device() 가 불려 설정공간 Command
 * 레지스터의 I/O Space / Memory Space 비트가 켜지고, 그제서야 BAR 로의
 * 접근이 하드웨어에서 디코딩된다. 0 을 쓰면 반대로 꺼진다.
 * 커널 드라이버 없이 사용자 공간에서 장치를 다루는 경우(예전 방식의
 * 유저스페이스 드라이버)에 쓰라고 만들어진 문이다.
 *
 * 왜 위험한가: 상류 영어 주석이 그대로 경고한다 — 엉뚱한 장치에 하면 기계가
 * 죽는다. 동작 중인 장치의 MMIO 디코딩을 꺼 버리면 그 뒤의 모든 접근이
 * 잘못된 데이터를 읽거나 머신 체크를 일으킨다. 그래서 세 겹으로 막는다.
 *  1) capable(CAP_SYS_ADMIN) — 루트 권한이 아니면 즉시 -EPERM.
 *  2) device_lock(dev) — 이 검사와 실행 사이에 드라이버가 바인딩/언바인딩되는
 *     경쟁을 막는다. 드라이버 프로브/제거 경로도 같은 락을 잡는다.
 *  3) dev->driver 검사 — 커널 드라이버가 이미 이 장치를 쓰고 있으면
 *     사용자가 끼어들 수 없게 -EBUSY. 즉 nvme 가 붙어 있는 SSD 의 enable 을
 *     사용자가 임의로 끌 수는 없다.
 *
 * 열거된 분기의 의미:
 *  - dev->driver 가 있으면 -EBUSY (커널이 쓰는 중).
 *  - val 이 0 이 아니면 활성화. pci_enable_device() 는 참조 계수를 올리므로
 *    여러 번 써도 카운트만 늘어난다.
 *  - val 이 0 이고 지금 켜져 있으면 비활성화.
 *  - val 이 0 인데 켜져 있지도 않으면 -EIO (짝이 맞지 않는 해제 시도).
 *
 * 실행 컨텍스트: write(2) 프로세스 문맥. device_lock() 은 잠들 수 있는
 * 뮤텍스이므로 원자적 문맥에서는 부를 수 없다.
 *
 * 호출 체인:
 *   write(2) -> sysfs -> dev_attr_store -> [enable_store]
 *     -> pci_enable_device / pci_disable_device (drivers/pci/pci.c)
 */
static ssize_t enable_store(struct device *dev, struct device_attribute *attr, /* [한국어] 대상 device 와 속성 서술자(미사용) */
			     const char *buf, size_t count) /* [한국어] 사용자 입력 문자열과 길이 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	unsigned long val; /* [한국어] 파싱된 사용자 값(0 = 끄기, 그 외 = 켜기) */
	ssize_t result = 0; /* [한국어] 분기별 결과. 0 으로 시작해 "성공" 을 기본값으로 둔다 */

	/* this can crash the machine when done on the "wrong" device */
	if (!capable(CAP_SYS_ADMIN)) /* [한국어] 위 영어 경고 때문에 루트만 허용한다. 파일 모드만으로는 부족하다고 본 것 */
		return -EPERM; /* [한국어] 권한 부족 — 아무것도 하지 않고 반환 */

	if (kstrtoul(buf, 0, &val) < 0) /* [한국어] 진법 자동 판별(0x/0/10진) 파싱 */
		return -EINVAL; /* [한국어] 숫자가 아니면 거부 */

	device_lock(dev); /* [한국어] 드라이버 바인딩 상태를 보는 동안 바뀌지 않도록 잠근다. 드라이버 코어의 probe/remove 와 같은 락 */
	if (dev->driver) /* [한국어] 커널 드라이버가 이미 이 장치를 점유 중인가 */
		result = -EBUSY; /* [한국어] 그렇다면 사용자가 끼어들 수 없다 */
	else if (val) /* [한국어] 드라이버가 없고, 켜라는 요청인 경우 */
		result = pci_enable_device(pdev); /* [한국어] Command 레지스터의 I/O/Memory 디코딩을 켜고 참조 계수를 올린다 */
	else if (pci_is_enabled(pdev)) /* [한국어] 끄라는 요청인데, 실제로 켜져 있는가 */
		pci_disable_device(pdev); /* [한국어] 참조 계수를 내리고 0 이 되면 디코딩을 끈다. 반환값이 없어 result 는 0 그대로 */
	else /* [한국어] 끄라는 요청인데 애초에 켜져 있지 않은 경우 */
		result = -EIO; /* [한국어] 짝이 맞지 않는 해제 — 사용자에게 알린다 */
	device_unlock(dev); /* [한국어] 모든 분기가 여기로 모이므로 락 해제가 한 번뿐이다 */

	return result < 0 ? result : count; /* [한국어] 오류면 그 오류를, 성공이면 "입력 전부 소비" 를 뜻하는 count 를 반환 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * enable_show - /sys/bus/pci/devices/<BDF>/enable 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 장치를 "켜 놓은" 주체가 몇 개인지를
 * 나타내는 참조 계수(enable_cnt). 0 이면 아무도 켜지 않은 상태이고,
 * 1 이상이면 그만큼의 주체가 pci_enable_device() 를 불러 둔 상태다.
 * 단순한 불리언이 아니라 카운트라는 점이 중요하다 — 같은 장치를 여러
 * 주체가 켤 수 있고, 마지막 하나가 놓을 때에만 실제로 꺼지기 때문이다.
 *
 * atomic_read() 를 쓰는 이유: enable_cnt 는 atomic_t 라 다른 CPU 가 동시에
 * 올리거나 내릴 수 있다. atomic_read 는 그 시점의 값을 찢어짐 없이 한 번에
 * 읽어 온다. 다만 읽은 직후 값이 바뀔 수 있으므로, 여기서 나온 숫자는
 * "그 순간의 스냅숏" 일 뿐 이후를 보장하지 않는다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [enable_show] -> atomic_read
 */
static ssize_t enable_show(struct device *dev, struct device_attribute *attr, /* [한국어] 대상 device 와 속성 서술자(미사용) */
			    char *buf) /* [한국어] 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev; /* [한국어] 복원할 pci_dev. 선언과 대입을 나눈 것은 상류의 표기 습관이며 의미 차이는 없다 */

	pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	return sysfs_emit(buf, "%u\n", atomic_read(&pdev->enable_cnt)); /* [한국어] 원자적으로 읽은 활성화 참조 계수를 출력 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RW(enable); /* [한국어] dev_attr_enable 생성. show 와 store 가 모두 있으므로 읽기+쓰기 속성 */

#ifdef CONFIG_NUMA /* [한국어] NUMA 를 아는 커널에서만 numa_node 파일이 존재한다 */
/*
 * [한국어]
 * numa_node_store - /sys/bus/pci/devices/<BDF>/numa_node 쓰기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 사용자가 쓴 노드 번호 문자열. -1(NUMA_NO_NODE)도 허용된다.
 * @count: 쓴 바이트 수.
 * @return: 성공 시 count, 권한 없으면 -EPERM, 파싱/범위/오프라인 노드면 -EINVAL.
 *
 * 이 파일에 노드 번호를 써서 커널이 파악한 NUMA 위치를 강제로 덮어쓴다.
 * 원래 이 값은 펌웨어(ACPI SRAT/_PXM 등)가 알려 주는 것인데, 잘못 알려 주는
 * 펌웨어가 있어 사람이 고칠 수단을 열어 둔 것이다.
 *
 * 이것은 순수한 "펌웨어 버그 우회" 수단이라, 커널은 이 경로를 지날 때
 * add_taint(TAINT_FIRMWARE_WORKAROUND) 로 커널을 오염(taint) 표시한다.
 * 나중에 문제가 생겨 버그 리포트가 올라오면, 그 커널이 정상 상태가 아니었음이
 * 로그에 남게 하려는 것이다. LOCKDEP_STILL_OK 는 "이 오염은 lockdep 의
 * 신뢰성과는 무관하니 lockdep 을 끄지 말라" 는 뜻이다.
 * 이어지는 pci_alert() 는 FW_BUG 접두를 붙여, 사용자가 아니라 펌웨어가
 * 잘못했음을 로그에서 바로 알아볼 수 있게 한다.
 *
 * 검사 순서와 이유:
 *  1) CAP_SYS_ADMIN — 시스템 전역 성능/할당 정책을 바꾸는 일이므로 루트만.
 *  2) kstrtoint(buf, 0, &node) — 음수를 받아야 하므로 unsigned 계열이 아니라
 *     정수 파서를 쓴다. 진법 인자 0 은 여기서도 자동 판별이다.
 *  3) 범위 검사 — 음수는 NUMA_NO_NODE 하나만 허용하고, 상한은
 *     MAX_NUMNODES 미만이어야 배열 인덱스로 안전하다.
 *  4) node_online(node) — 존재하지만 꺼져 있는 노드를 가리키면 그 노드의
 *     메모리에 할당을 시도하다 실패한다. 미리 막는다.
 *  5) 값이 그대로면 아무것도 하지 않고 성공 처리 — 불필요한 taint 를
 *     남기지 않기 위한 지름길이다.
 *
 * 실행 컨텍스트: write(2) 프로세스 문맥. 락 없음(단일 정수 대입).
 *
 * 호출 체인:
 *   write(2) -> sysfs -> dev_attr_store -> [numa_node_store]
 *     -> add_taint / pci_alert
 */
static ssize_t numa_node_store(struct device *dev, /* [한국어] 대상 device */
			       struct device_attribute *attr, const char *buf, /* [한국어] 속성 서술자(미사용)와 사용자 입력 */
			       size_t count) /* [한국어] 입력 길이 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] 아래 pci_alert() 로 장치 이름이 붙은 로그를 찍기 위해 필요하다 */
	int node; /* [한국어] 파싱된 노드 번호. 음수(NUMA_NO_NODE)가 있을 수 있어 부호 있는 정수 */

	if (!capable(CAP_SYS_ADMIN)) /* [한국어] 시스템 전역 정책 변경이므로 루트만 허용 */
		return -EPERM; /* [한국어] 권한 부족 */

	if (kstrtoint(buf, 0, &node) < 0) /* [한국어] 음수를 받아야 하므로 정수 파서. 0 은 진법 자동 판별 */
		return -EINVAL; /* [한국어] 숫자가 아니면 거부 */

	if ((node < 0 && node != NUMA_NO_NODE) || node >= MAX_NUMNODES) /* [한국어] 음수는 NUMA_NO_NODE 만 허용, 상한은 노드 배열 크기 미만이어야 인덱스로 안전 */
		return -EINVAL; /* [한국어] 배열 범위를 벗어날 값은 거부 */

	if (node != NUMA_NO_NODE && !node_online(node)) /* [한국어] 존재는 하지만 꺼져 있는 노드인가 */
		return -EINVAL; /* [한국어] 꺼진 노드를 가리키면 나중에 메모리 할당이 실패하므로 미리 막는다 */

	if (node == dev->numa_node) /* [한국어] 이미 같은 값이면 바꿀 것이 없다 */
		return count; /* [한국어] 아래의 taint 와 경고를 남기지 않고 조용히 성공 처리 */

	add_taint(TAINT_FIRMWARE_WORKAROUND, LOCKDEP_STILL_OK); /* [한국어] 펌웨어 버그를 사람이 덮어썼음을 커널 전역에 표시. LOCKDEP_STILL_OK 는 lockdep 신뢰성에는 영향이 없다는 뜻 */
	pci_alert(pdev, FW_BUG "Overriding NUMA node to %d.  Contact your vendor for updates.", /* [한국어] FW_BUG 접두로 "펌웨어가 잘못한 것" 임을 로그에 명시 */
		  node); /* [한국어] 새로 설정할 노드 번호를 메시지에 채운다 */

	dev->numa_node = node; /* [한국어] 이 PCI 장치가 속한 것으로 간주할 NUMA 노드를 덮어쓴다. 이후 DMA 버퍼 할당과 IRQ affinity 판단이 이 값을 따른다 */
	return count; /* [한국어] 입력을 전부 소비했다고 알림 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * numa_node_show - /sys/bus/pci/devices/<BDF>/numa_node 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 장치가 붙어 있다고 커널이 판단한 NUMA
 * 노드 번호. -1 이면 펌웨어가 알려 주지 않아 모르는 상태다.
 *
 * NVMe 학습 관점: 여러 소켓이 있는 서버에서 NVMe SSD 의 numa_node 는 성능에
 * 직결된다. -1 로 나오면 커널이 노드를 몰라 메모리와 인터럽트를 아무 데나
 * 붙이게 되므로, 실제 배치를 알고 있다면 위 numa_node_store 로 고칠 수 있다.
 * 다만 그 순간 커널은 taint 표시가 된다. 값이 확인된 그 파일 하나가
 * local_cpulist 의 내용도 결정한다(pci_dev_show_local_cpu 참고).
 *
 * 여기서 to_pci_dev() 를 거치지 않는 점에 주목: numa_node 는 PCI 고유
 * 필드가 아니라 모든 장치가 갖는 struct device 의 공통 필드다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [numa_node_show] -> sysfs_emit
 */
static ssize_t numa_node_show(struct device *dev, struct device_attribute *attr, /* [한국어] 대상 device 와 속성 서술자(미사용) */
			      char *buf) /* [한국어] 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	return sysfs_emit(buf, "%d\n", dev->numa_node); /* [한국어] PCI 가 아니라 struct device 공통 필드를 그대로 출력. -1 은 "모름" */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RW(numa_node); /* [한국어] dev_attr_numa_node 생성(읽기+쓰기) */
#endif /* [한국어] CONFIG_NUMA 블록 끝 — NUMA 가 없으면 이 속성 자체가 존재하지 않는다 */

/*
 * [한국어]
 * dma_mask_bits_show - /sys/bus/pci/devices/<BDF>/dma_mask_bits 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 장치가 DMA 주소로 쓸 수 있는 비트 수.
 * 32 면 4GB 아래 주소만 쓸 수 있다는 뜻이고, 64 면 제한이 사실상 없다는
 * 뜻이다. 32 로 나오는 장치에 4GB 위의 버퍼를 주면 커널이 바운스 버퍼로
 * 복사해야 해서 성능이 크게 떨어진다.
 *
 * fls64(마스크) 는 "가장 높은 1 비트의 위치(1부터 셈)" 를 돌려준다.
 * DMA 마스크는 하위 N 비트가 전부 1 인 형태라, fls64 결과가 곧 N 이 된다.
 * 예를 들어 마스크가 0xffffffff 면 fls64 는 32 다. 비트를 하나씩 세지
 * 않고 이 한 줄로 끝내는 이유다.
 *
 * NVMe 학습 관점: nvme-pci 는 프로브에서 마스크를 명시적으로 정한다 —
 * drivers/nvme/host/pci.c 가 NVME_QUIRK_DMA_ADDRESS_BITS_48 quirk 가 붙은
 * 장치에는 DMA_BIT_MASK(48) 을, 나머지에는 DMA_BIT_MASK(64) 를
 * dma_set_mask_and_coherent() 로 설정한다. 따라서 정상적인 NVMe SSD 라면
 * 이 파일이 64 를, 그 quirk 가 걸린 장치라면 48 을 보여 준다. 48 이 보이면
 * 그 SSD 가 주소 상위 비트를 제대로 못 다루는 알려진 물건이라는 신호다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [dma_mask_bits_show] -> fls64
 */
static ssize_t dma_mask_bits_show(struct device *dev, /* [한국어] 대상 device */
				  struct device_attribute *attr, char *buf) /* [한국어] 속성 서술자(미사용)와 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] 스트리밍 DMA 마스크는 pci_dev 쪽에 있으므로 복원이 필요하다 */

	return sysfs_emit(buf, "%d\n", fls64(pdev->dma_mask)); /* [한국어] 마스크의 최상위 1 비트 위치 = 사용 가능한 주소 비트 수 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(dma_mask_bits); /* [한국어] dev_attr_dma_mask_bits 생성 -> /sys/.../dma_mask_bits */

/*
 * [한국어]
 * consistent_dma_mask_bits_show - /sys/bus/pci/devices/<BDF>/consistent_dma_mask_bits 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 코히런트(일관성 보장) DMA 버퍼에 쓸 수
 * 있는 주소 비트 수. 위 dma_mask_bits 가 일반 스트리밍 DMA 용이라면, 이쪽은
 * dma_alloc_coherent() 로 잡는 버퍼에 적용된다. 장치가 이 두 가지에 서로
 * 다른 제한을 갖는 경우가 있어 따로 노출한다.
 *
 * 여기서는 to_pci_dev() 를 쓰지 않는다 — coherent_dma_mask 는 PCI 고유
 * 필드가 아니라 struct device 의 공통 필드이기 때문이다. 위 dma_mask_bits
 * 가 pci_dev->dma_mask 를 보는 것과 대비된다.
 *
 * NVMe 학습 관점: nvme-pci 는 dma_set_mask_and_coherent() 를 쓰므로 두 마스크가
 * 같은 값으로 설정된다(drivers/nvme/host/pci.c 에서 확인). 즉 정상적인 NVMe
 * SSD 라면 이 파일과 dma_mask_bits 가 같은 숫자를 보여 준다. 다르게 나오면
 * 무언가가 나중에 마스크를 바꾼 것이다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [consistent_dma_mask_bits_show] -> fls64
 */
static ssize_t consistent_dma_mask_bits_show(struct device *dev, /* [한국어] 대상 device */
					     struct device_attribute *attr, /* [한국어] 속성 서술자 — 미사용 */
					     char *buf) /* [한국어] 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	return sysfs_emit(buf, "%d\n", fls64(dev->coherent_dma_mask)); /* [한국어] struct device 공통 필드를 그대로 읽어 비트 수로 환산 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(consistent_dma_mask_bits); /* [한국어] dev_attr_consistent_dma_mask_bits 생성 */

/*
 * [한국어]
 * msi_bus_show - /sys/bus/pci/devices/<BDF>/msi_bus 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 앞으로 이 장치(또는 이 브리지 아래 버스의
 * 장치들)가 MSI/MSI-X 를 쓰는 것이 허용되어 있는지(1 = 허용, 0 = 금지).
 *
 * 대상이 두 갈래인 것이 이 속성의 핵심이다.
 *  - 브리지가 아닌 일반 장치(pdev->subordinate 가 NULL): 그 장치 자신의
 *    no_msi 플래그를 뒤집어 보여 준다. no_msi 가 1 이면 금지이므로 !no_msi.
 *  - 브리지(하위 버스가 있음): 자기 자신이 아니라 하위 버스의
 *    PCI_BUS_FLAGS_NO_MSI 플래그를 본다. MSI 는 인터럽트를 메모리 쓰기로
 *    바꿔 상위로 흘려보내는 방식이라, 중간의 브리지가 그 쓰기를 제대로
 *    전달하지 못하면 그 아래 모든 장치가 MSI 를 쓸 수 없다. 그래서 금지
 *    여부가 장치가 아니라 버스에 붙는다.
 *
 * 왜 필요한가: MSI 쓰기를 제대로 라우팅하지 못하는 고장 난 브리지가 실재해,
 * 그런 하드웨어 아래에서는 MSI 를 꺼야 인터럽트가 살아난다. 진단할 때
 * 사람이 손으로 꺼 보게 하려고 만든 문이다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 락 없음(플래그 한 비트 읽기).
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [msi_bus_show] -> sysfs_emit
 */
static ssize_t msi_bus_show(struct device *dev, struct device_attribute *attr, /* [한국어] 대상 device 와 속성 서술자(미사용) */
			    char *buf) /* [한국어] 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	struct pci_bus *subordinate = pdev->subordinate; /* [한국어] 이 장치가 브리지라면 그 아래 버스, 엔드포인트라면 NULL. 아래 두 분기를 가르는 기준이다 */

	return sysfs_emit(buf, "%u\n", subordinate ? /* [한국어] 브리지인지 엔드포인트인지에 따라 보는 곳이 달라진다 */
			  !(subordinate->bus_flags & PCI_BUS_FLAGS_NO_MSI) /* [한국어] 브리지: 하위 버스의 "MSI 금지" 비트를 뒤집어 "허용" 으로 표시 */
			    : !pdev->no_msi); /* [한국어] 엔드포인트: 자기 자신의 no_msi 를 뒤집어 표시 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * msi_bus_store - /sys/bus/pci/devices/<BDF>/msi_bus 쓰기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 사용자가 쓴 문자열. 0 이면 MSI 금지, 그 외면 허용.
 * @count: 쓴 바이트 수.
 * @return: 성공 시 count, 권한 없으면 -EPERM, 파싱 실패면 -EINVAL.
 *
 * 이 파일에 0 을 써서 앞으로의 MSI/MSI-X 사용을 막고, 1 을 써서 다시 푼다.
 * 상류 영어 주석이 결정적인 한계를 못박고 있다: 이 플래그들은 드라이버가
 * "앞으로" MSI 나 MSI-X 를 요청할 때에만 영향을 준다. 이미 요청해서 쓰고
 * 있는 드라이버에는 아무 효과가 없다. 따라서 실제로 효과를 보려면 대상
 * 드라이버를 언바인드했다가 다시 바인드해야 한다.
 *
 * 두 분기:
 *  - 하위 버스가 없으면(엔드포인트): 자기 자신의 no_msi 를 설정한다.
 *    val 이 1(허용)이면 no_msi = 0 이 되도록 !val 을 넣는다.
 *  - 하위 버스가 있으면(브리지): 그 버스의 PCI_BUS_FLAGS_NO_MSI 비트를
 *    켜거나 끈다. 허용하려면 AND NOT 으로 비트를 지우고, 금지하려면
 *    OR 로 비트를 세운다. 이 한 번의 변경이 그 버스 아래 모든 장치에
 *    영향을 준다는 점에서 파급력이 크다.
 *
 * 두 경우 모두 커널 로그에 무엇이 바뀌었는지 남긴다. 엔드포인트는
 * pci_info(장치 기준), 브리지는 dev_info(하위 버스 기준)로 찍어, 나중에
 * "왜 이 장치가 MSI 를 못 쓰지" 를 추적할 수 있게 한다.
 *
 * 권한: capable(CAP_SYS_ADMIN). 인터럽트 방식을 바꾸는 일이라 시스템 전체의
 * 안정성에 영향을 준다.
 *
 * NVMe 학습 관점: 이 파일을 0 으로 두고 NVMe 드라이버를 다시 바인드하면
 * nvme-pci 가 MSI-X 벡터를 얻지 못한다. drivers/nvme/host/pci.c 는
 * pci_alloc_irq_vectors_affinity() 로 벡터를 요청하므로, MSI 계열이 막히면
 * 레거시 인터럽트로 떨어지고 큐가 하나로 줄어든다. MSI-X 가 NVMe 성능에
 * 얼마나 결정적인지를 실험으로 확인할 수 있는 스위치다.
 *
 * 실행 컨텍스트: write(2) 프로세스 문맥. 락 없음 — 플래그 갱신이 원자적이지
 * 않지만, 다음 드라이버 바인딩 때 읽히는 힌트일 뿐이라 상류는 락을 두지
 * 않았다.
 *
 * 호출 체인:
 *   write(2) -> sysfs -> dev_attr_store -> [msi_bus_store]
 */
static ssize_t msi_bus_store(struct device *dev, struct device_attribute *attr, /* [한국어] 대상 device 와 속성 서술자(미사용) */
			     const char *buf, size_t count) /* [한국어] 사용자 입력 문자열과 길이 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	struct pci_bus *subordinate = pdev->subordinate; /* [한국어] 브리지면 하위 버스, 엔드포인트면 NULL */
	unsigned long val; /* [한국어] 파싱된 사용자 값(0 = 금지, 그 외 = 허용) */

	if (!capable(CAP_SYS_ADMIN)) /* [한국어] 인터럽트 방식을 바꾸는 일이라 루트만 허용 */
		return -EPERM; /* [한국어] 권한 부족 */

	if (kstrtoul(buf, 0, &val) < 0) /* [한국어] 진법 자동 판별 파싱 */
		return -EINVAL; /* [한국어] 숫자가 아니면 거부 */

	/*
	 * "no_msi" and "bus_flags" only affect what happens when a driver
	 * requests MSI or MSI-X.  They don't affect any drivers that have
	 * already requested MSI or MSI-X.
	 */
	/* [한국어] 위 영어 주석의 뜻: 이 두 플래그는 "앞으로의 요청" 에만
	 * 작용한다. 이미 MSI/MSI-X 를 할당받아 동작 중인 드라이버에는 아무
	 * 영향이 없으므로, 효과를 보려면 드라이버를 언바인드했다 다시 붙여야
	 * 한다. 이것을 모르고 값을 바꾼 뒤 "안 먹는다" 고 오해하기 쉽다. */
	if (!subordinate) { /* [한국어] 하위 버스가 없다 = 브리지가 아닌 일반 엔드포인트 */
		pdev->no_msi = !val; /* [한국어] 사용자가 쓴 "허용" 의미를 커널의 "금지" 플래그로 뒤집어 저장 */
		pci_info(pdev, "MSI/MSI-X %s for future drivers\n", /* [한국어] 정책 변경을 커널 로그에 남긴다 — 나중에 원인 추적용 */
			 val ? "allowed" : "disallowed"); /* [한국어] 사람이 읽을 문자열 선택 */
		return count; /* [한국어] 엔드포인트 처리는 여기서 끝 */
	} /* [한국어] 엔드포인트 분기 종료 */

	if (val) /* [한국어] 여기부터는 브리지 — 허용하라는 요청인가 */
		subordinate->bus_flags &= ~PCI_BUS_FLAGS_NO_MSI; /* [한국어] AND NOT 으로 "MSI 금지" 비트만 지운다. 다른 버스 플래그는 건드리지 않는다 */
	else /* [한국어] 금지하라는 요청 */
		subordinate->bus_flags |= PCI_BUS_FLAGS_NO_MSI; /* [한국어] OR 로 그 비트만 세운다. 이 버스 아래 모든 장치에 영향이 간다 */

	dev_info(&subordinate->dev, "MSI/MSI-X %s for future drivers of devices on this bus\n", /* [한국어] 브리지가 아니라 "그 아래 버스" 이름으로 로그를 남겨 영향 범위를 분명히 한다 */
		 val ? "allowed" : "disallowed"); /* [한국어] 사람이 읽을 문자열 선택 */
	return count; /* [한국어] 입력을 전부 소비했다고 알림 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RW(msi_bus); /* [한국어] dev_attr_msi_bus 생성(읽기+쓰기) */

/*
 * [한국어]
 * rescan_store - /sys/bus/pci/rescan 쓰기 콜백 (버스 타입 전역 속성)
 *
 * @bus: 이 속성이 걸린 bus_type. 여기서는 pci_bus_type 뿐이라 쓰지 않는다.
 * @buf: 사용자가 쓴 문자열. 0 이 아니면 재스캔한다.
 * @count: 쓴 바이트 수.
 * @return: 성공 시 count, 파싱 실패면 -EINVAL. 재스캔 자체는 실패를
 *   보고하지 않는다(개별 버스의 실패를 여기서 모아 알릴 방법이 없다).
 *
 * 이 파일은 개별 장치가 아니라 PCI 버스 타입 전체에 하나만 있는 파일이다.
 * 여기에 1 을 쓰면 시스템의 모든 PCI 루트 버스를 처음부터 다시 훑어,
 * 새로 나타난 장치를 찾아 등록한다. hotplug 알림이 오지 않는 환경(가상화
 * 플랫폼, 일부 서버 슬롯)에서 장치를 꽂은 뒤 손으로 찾게 하는 수단이다.
 *
 * 동작 단계:
 *  1) 사용자 입력을 파싱한다. 0 이면 아무 일도 하지 않는다.
 *  2) pci_lock_rescan_remove() 로 전역 직렬화 락을 잡는다. 이 락은 열거와
 *     제거를 동시에 못 하게 막는다 — 스캔 도중 다른 쪽에서 장치를 지우면
 *     이미 해제된 pci_dev 를 따라가게 되어 커널이 죽는다.
 *  3) pci_find_next_bus(NULL) 부터 시작해 루트 버스를 하나씩 순회하며
 *     pci_rescan_bus() 를 부른다. NULL 을 주면 처음부터라는 규약이다.
 *  4) 락을 푼다.
 *
 * NVMe 학습 관점: NVMe SSD 를 hot-add 했는데 /dev/nvme0 이 생기지 않을 때
 * 이 파일에 1 을 쓰면 열거가 다시 돌아 nvme 드라이버 프로브까지 이어진다.
 * 반대 방향(제거)은 장치 디렉터리의 remove 파일이다.
 *
 * 실행 컨텍스트: write(2) 프로세스 문맥. 전역 뮤텍스를 잡고 버스 전체를
 * 훑으므로 오래 걸릴 수 있고, 그동안 잠든다.
 *
 * 호출 체인:
 *   write(2) -> sysfs -> bus_attr_store -> [rescan_store]
 *     -> pci_lock_rescan_remove -> pci_find_next_bus / pci_rescan_bus
 */
static ssize_t rescan_store(const struct bus_type *bus, const char *buf, size_t count) /* [한국어] 장치가 아니라 bus_type 단위 속성이라 시그니처가 다르다 */
{ /* [한국어] 함수 본문 시작 */
	unsigned long val; /* [한국어] 파싱된 사용자 값. 0 이면 아무 일도 하지 않는다 */
	struct pci_bus *b = NULL; /* [한국어] 순회 커서. NULL 로 시작해야 "처음부터" 라는 뜻이 된다 */

	if (kstrtoul(buf, 0, &val) < 0) /* [한국어] 진법 자동 판별 파싱 */
		return -EINVAL; /* [한국어] 숫자가 아니면 거부 */

	if (val) { /* [한국어] 0 이 아닐 때만 실제로 재스캔한다 — 0 을 쓰는 것은 no-op */
		pci_lock_rescan_remove(); /* [한국어] 열거와 제거를 직렬화하는 전역 뮤텍스. 스캔 중 장치가 사라지는 경쟁을 막는다 */
		while ((b = pci_find_next_bus(b)) != NULL) /* [한국어] 루트 버스를 하나씩 넘겨받는다. 커서를 갱신하며 끝(NULL)까지 */
			pci_rescan_bus(b); /* [한국어] 그 버스 아래를 다시 열거해 새 장치를 찾고 자원을 배정한다 */
		pci_unlock_rescan_remove(); /* [한국어] 반드시 짝을 맞춰 푼다. 여기서 못 풀면 이후 모든 hotplug 가 멈춘다 */
	} /* [한국어] 재스캔 블록 종료 */
	return count; /* [한국어] 입력을 전부 소비했다고 알림 */
} /* [한국어] 함수 본문 종료 */
static BUS_ATTR_WO(rescan); /* [한국어] bus_attr_rescan 생성. WO = 쓰기 전용(읽으면 의미가 없으므로 show 가 없다) */

/* [한국어] pci_bus_attrs - pci_bus_type 자체(/sys/bus/pci/)에 붙는 속성 목록.
 * 장치별 디렉터리가 아니라 버스 타입 디렉터리에 만들어진다. */
static struct attribute *pci_bus_attrs[] = { /* [한국어] 배열 시작 */
	&bus_attr_rescan.attr, /* [한국어] /sys/bus/pci/rescan 파일을 만든다. 위 rescan_store 가 그 쓰기 콜백 */
	NULL, /* [한국어] 배열의 끝을 알리는 센티널. sysfs 코어가 NULL 을 만날 때까지 순회하므로 반드시 있어야 한다 */
}; /* [한국어] 배열 종료 */

/* [한국어] pci_bus_group - 위 배열 하나를 담은 속성 그룹. */
static const struct attribute_group pci_bus_group = { /* [한국어] 구조체 초기화 시작 */
	.attrs = pci_bus_attrs, /* [한국어] 이 그룹이 만들 일반 속성 목록.
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: 드라이버 코어의 sysfs 등록 코드.
		 * 값 범위: NULL 로 끝나는 struct attribute 포인터 배열.
		 * 동기화: const 정적 데이터라 읽기 전용, 락 불필요.
		 * is_visible 을 두지 않았으므로 조건 없이 항상 만들어진다 —
		 * 버스 타입은 하나뿐이라 장치별로 가릴 이유가 없다. */
}; /* [한국어] 구조체 초기화 종료 */

/* [한국어] pci_bus_groups - 드라이버 코어에 넘겨질 최상위 그룹 배열.
 * drivers/pci/pci-driver.c 의 pci_bus_type 이 .bus_groups = pci_bus_groups
 * 로 이 배열을 가져간다(그 파일에서 확인). static 이 아닌 이유가 그것이다. */
const struct attribute_group *pci_bus_groups[] = { /* [한국어] 배열 시작 */
	&pci_bus_group, /* [한국어] 위에서 만든 그룹 하나 — /sys/bus/pci/rescan 을 만든다 */
	NULL, /* [한국어] 끝 센티널 */
}; /* [한국어] 배열 종료 */

/*
 * [한국어]
 * dev_rescan_store - /sys/bus/pci/devices/<BDF>/rescan 쓰기 콜백 (장치 디렉터리)
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 사용자가 쓴 문자열. 0 이 아니면 재스캔한다.
 * @count: 쓴 바이트 수.
 * @return: 성공 시 count, 파싱 실패면 -EINVAL.
 *
 * 위의 전역 rescan 과 이름은 같지만 범위가 다르다. 이쪽은 이 장치가 앉아
 * 있는 버스 하나만 다시 훑는다. 시스템 전체를 훑으면 관계없는 장치까지
 * 건드리게 되므로, 대상이 분명할 때는 이쪽이 훨씬 안전하고 빠르다.
 *
 * 쓰임새의 예: 같은 슬롯의 다른 기능(function)이 뒤늦게 나타났을 때,
 * 또는 SR-IOV 로 VF 를 만든 뒤 그것들을 열거시킬 때.
 *
 * 이 속성만 __ATTR() 을 직접 쓰는 이유: 파일 이름이 "rescan" 이어야 하는데
 * DEVICE_ATTR_WO(rescan) 을 쓰면 심볼 이름이 dev_attr_rescan 이 되어,
 * 뒤에 나오는 버스용 rescan(dev_attr_bus_rescan)과 충돌한다. 그래서
 * __ATTR(파일이름, 모드, show, store) 로 파일 이름과 C 심볼 이름을 분리했다.
 * 모드 0200 은 소유자 쓰기 전용 — 읽어 봐야 의미가 없고, 아무나 재열거를
 * 유발하게 두면 안 되기 때문이다.
 *
 * 실행 컨텍스트: write(2) 프로세스 문맥. 전역 직렬화 락을 잡고 열거하므로
 * 잠들 수 있다.
 *
 * 호출 체인:
 *   write(2) -> sysfs -> dev_attr_store -> [dev_rescan_store]
 *     -> pci_lock_rescan_remove -> pci_rescan_bus
 */
static ssize_t dev_rescan_store(struct device *dev, /* [한국어] 대상 device */
				struct device_attribute *attr, const char *buf, /* [한국어] 속성 서술자(미사용)와 사용자 입력 */
				size_t count) /* [한국어] 입력 길이 */
{ /* [한국어] 함수 본문 시작 */
	unsigned long val; /* [한국어] 파싱된 사용자 값 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] 이 장치가 앉은 버스(pdev->bus)를 알아내기 위해 복원한다 */

	if (kstrtoul(buf, 0, &val) < 0) /* [한국어] 진법 자동 판별 파싱 */
		return -EINVAL; /* [한국어] 숫자가 아니면 거부 */

	if (val) { /* [한국어] 0 이 아닐 때만 재스캔 */
		pci_lock_rescan_remove(); /* [한국어] 전역 열거/제거 직렬화 락 — 범위는 버스 하나여도 락은 전역이다 */
		pci_rescan_bus(pdev->bus); /* [한국어] 이 장치가 앉은 버스만 다시 훑는다. 시스템 전체를 건드리지 않는다 */
		pci_unlock_rescan_remove(); /* [한국어] 락 해제 */
	} /* [한국어] 재스캔 블록 종료 */
	return count; /* [한국어] 입력을 전부 소비했다고 알림 */
} /* [한국어] 함수 본문 종료 */
/* [한국어] dev_attr_dev_rescan - 파일 이름은 "rescan", C 심볼은 dev_attr_dev_rescan.
 * __ATTR(name, mode, show, store) 의 네 인자가 각각
 * 파일 이름 / 파일 모드 / 읽기 콜백 / 쓰기 콜백이다.
 * show 에 NULL 을 준 것은 읽기를 지원하지 않는다는 뜻이고, 모드 0200 이
 * 그것과 일관된다(소유자 쓰기만, 읽기 비트 없음).
 * 이 속성은 아래 pci_dev_hp_attrs[] 에 들어가며, 그 그룹의 is_visible 인
 * pci_dev_hp_attrs_are_visible() 이 SR-IOV VF 에는 파일을 만들지 않는다. */
static struct device_attribute dev_attr_dev_rescan = __ATTR(rescan, 0200, NULL, /* [한국어] 파일 이름 "rescan", 모드 0200(소유자 쓰기 전용), show 없음 */
							    dev_rescan_store); /* [한국어] 쓰기 콜백은 위 함수 */

/*
 * [한국어]
 * remove_store - /sys/bus/pci/devices/<BDF>/remove 쓰기 콜백
 *
 * @dev: 대상 device.
 * @attr: 이 속성 자신의 서술자. 다른 store 들과 달리 여기서는 실제로
 *   쓰인다 — 자기 자신을 지우기 위해 device_remove_file_self() 에 넘긴다.
 * @buf: 사용자가 쓴 문자열. 0 이 아니면 장치를 제거한다.
 * @count: 쓴 바이트 수.
 * @return: 항상 count(파싱 실패면 -EINVAL). 제거 실패는 보고하지 않는다.
 *
 * 이 파일에 1 을 쓰면 이 PCI 장치가 커널에서 논리적으로 뽑힌다. 드라이버가
 * 언바인드되고(그 과정에서 nvme 라면 nvme_remove() 가 불린다),
 * /sys/bus/pci/devices/<BDF> 디렉터리 자체가 사라진다. 하드웨어를 물리적으로
 * 뽑는 것이 아니라 커널이 그 장치를 잊는 것이며, 다시 보이게 하려면 위의
 * rescan 을 써야 한다.
 *
 * 가장 까다로운 지점 — 자기 자신을 지우는 문제:
 * 이 store 콜백은 remove 파일에 대한 write(2) 안에서 실행 중이다. 그런데 그
 * 안에서 장치를 지우면 remove 파일 자신도 지워야 한다. 자기가 실행 중인
 * 파일을 지우면 교착에 빠지므로, 커널은 device_remove_file_self() 라는
 * 전용 헬퍼를 둔다. 이 함수는 "이 파일을 지우는 권한을 정확히 한 번만"
 * 내주고, 동시에 여러 프로세스가 1 을 써도 딱 하나만 true 를 받는다.
 * 그래서 && 의 단락 평가로 true 를 받은 쪽만 실제 제거로 넘어간다 —
 * 이중 제거 경쟁이 구조적으로 불가능해진다.
 *
 * DEVICE_ATTR_IGNORE_LOCKDEP 를 쓰는 이유도 같은 맥락이다. sysfs 파일의
 * 쓰기 락을 잡은 채 그 파일이 속한 장치를 지우는 이 패턴은 lockdep 이 보기에
 * 잠재적 순환으로 보인다. 실제로는 위 헬퍼가 안전을 보장하므로, 그 속성에
 * 한해 lockdep 검사를 면제한다. 매크로의 정의는 include/linux/device.h 에
 * 있고 이 트리에는 그 헤더가 없어 원문은 확인할 수 없다.
 *
 * 모드 0220 은 소유자와 그룹의 쓰기만 허용하고 읽기 비트는 없다는 뜻이다.
 * 읽을 내용이 없기도 하지만, 존재 자체가 파괴적인 파일이라 권한을 좁혔다.
 *
 * NVMe 학습 관점: NVMe SSD 를 안전하게 논리적으로 떼어낼 때 쓰는 표준 방법이
 * 이 파일이다. 마운트된 파일시스템이 있으면 먼저 언마운트해야 하며,
 * 그러지 않으면 nvme_remove() 가 진행되면서 I/O 오류가 쏟아진다.
 *
 * 실행 컨텍스트: write(2) 프로세스 문맥. 드라이버 remove 콜백까지 동기적으로
 * 실행되므로 오래 걸릴 수 있고 잠든다.
 *
 * 호출 체인:
 *   write(2) -> sysfs -> dev_attr_store -> [remove_store]
 *     -> device_remove_file_self -> pci_stop_and_remove_bus_device_locked
 */
static ssize_t remove_store(struct device *dev, struct device_attribute *attr, /* [한국어] 대상 device 와 자기 자신의 속성 서술자(여기서는 실제로 쓰인다) */
			    const char *buf, size_t count) /* [한국어] 사용자 입력 문자열과 길이 */
{ /* [한국어] 함수 본문 시작 */
	unsigned long val; /* [한국어] 파싱된 사용자 값. 0 이면 아무 일도 하지 않는다 */

	if (kstrtoul(buf, 0, &val) < 0) /* [한국어] 진법 자동 판별 파싱 */
		return -EINVAL; /* [한국어] 숫자가 아니면 거부 */

	if (val && device_remove_file_self(dev, attr)) /* [한국어] 단락 평가가 핵심: 0 이면 헬퍼조차 부르지 않고, 동시 요청 중 딱 하나만 true 를 받아 아래로 진입한다 */
		pci_stop_and_remove_bus_device_locked(to_pci_dev(dev)); /* [한국어] 드라이버 언바인드 -> sysfs/proc 정리 -> pci_dev 해제까지. 이름의 _locked 는 내부에서 rescan/remove 전역 락을 잡아 준다는 뜻 */
	return count; /* [한국어] 제거 여부와 무관하게 입력은 소비했다고 알린다 */
} /* [한국어] 함수 본문 종료 */
/* [한국어] dev_attr_remove - 파일 이름 "remove", 모드 0220(쓰기 전용),
 * show 없음, store 는 위 remove_store.
 * IGNORE_LOCKDEP 판이라 이 속성에 한해 lockdep 의 자기참조 경고가 면제된다.
 * 아래 pci_dev_hp_attrs[] 에 들어가며, VF 에는 만들어지지 않는다. */
static DEVICE_ATTR_IGNORE_LOCKDEP(remove, 0220, NULL, /* [한국어] 이름/모드/show(NULL) */
				  remove_store); /* [한국어] store 콜백 */

/*
 * [한국어]
 * bus_rescan_store - /sys/class/pci_bus/<도메인:버스>/rescan 쓰기 콜백
 *
 * @dev: 대상 device. 이번에는 pci_dev 가 아니라 pci_bus 안에 박힌 device 다.
 * @attr: 속성 서술자(미사용).
 * @buf: 사용자가 쓴 문자열. 0 이 아니면 재스캔한다.
 * @count: 쓴 바이트 수.
 * @return: 성공 시 count, 파싱 실패면 -EINVAL.
 *
 * 앞의 두 rescan 과 세 번째 변종이다. 전역 rescan 은 시스템 전부,
 * 장치의 rescan 은 그 장치가 앉은 버스, 이것은 버스 객체 자신을 대상으로
 * 한다. 셋이 이름은 같고 붙는 자리가 달라 C 심볼만 다르다.
 *
 * 특별한 분기 하나: 루트 버스가 아니면서 지금 장치가 하나도 없는 버스라면
 * 그냥 다시 훑는 대신 pci_rescan_bus_bridge_resize(bus->self) 를 부른다.
 * 왜인가 — 텅 빈 브리지 아래에는 자원 창(브리지 윈도우)이 최소 크기로만
 * 잡혀 있거나 아예 닫혀 있을 수 있다. 그 상태로 새 장치를 발견해도 BAR 를
 * 넣을 자리가 없어 열거가 실패한다. 그래서 브리지 자신부터 다시 열어
 * 윈도우 크기를 재조정하면서 열거한다. 반대로 이미 장치가 있는 버스나
 * 루트 버스는 창이 이미 잡혀 있으므로 평범한 재스캔으로 충분하다.
 * bus->self 는 그 버스를 만들어 낸 상위 브리지 장치를 가리킨다 —
 * 루트 버스에서는 NULL 이므로, 루트 버스를 먼저 걸러 내는 조건이
 * NULL 역참조를 막는 역할도 겸한다.
 *
 * 실행 컨텍스트: write(2) 프로세스 문맥. 전역 직렬화 락을 잡는다.
 *
 * 호출 체인:
 *   write(2) -> sysfs -> dev_attr_store -> [bus_rescan_store]
 *     -> pci_rescan_bus_bridge_resize 또는 pci_rescan_bus
 */
static ssize_t bus_rescan_store(struct device *dev, /* [한국어] pci_bus 에 박힌 device */
				struct device_attribute *attr, /* [한국어] 속성 서술자 — 미사용 */
				const char *buf, size_t count) /* [한국어] 사용자 입력 문자열과 길이 */
{ /* [한국어] 함수 본문 시작 */
	unsigned long val; /* [한국어] 파싱된 사용자 값 */
	struct pci_bus *bus = to_pci_bus(dev); /* [한국어] device -> pci_bus 복원. to_pci_dev 가 아님에 주의 */

	if (kstrtoul(buf, 0, &val) < 0) /* [한국어] 진법 자동 판별 파싱 */
		return -EINVAL; /* [한국어] 숫자가 아니면 거부 */

	if (val) { /* [한국어] 0 이 아닐 때만 재스캔 */
		pci_lock_rescan_remove(); /* [한국어] 전역 열거/제거 직렬화 락 */
		if (!pci_is_root_bus(bus) && list_empty(&bus->devices)) /* [한국어] 루트가 아니면서 비어 있는 버스인가. 루트를 먼저 거르므로 아래의 bus->self 가 NULL 일 수 없다 */
			pci_rescan_bus_bridge_resize(bus->self); /* [한국어] 상위 브리지부터 다시 열며 브리지 윈도우 크기를 재조정한다. 창이 닫힌 빈 브리지에 새 장치를 넣으려면 필요하다 */
		else /* [한국어] 루트 버스이거나 이미 장치가 있는 버스 */
			pci_rescan_bus(bus); /* [한국어] 창이 이미 잡혀 있으므로 평범하게 다시 훑기만 하면 된다 */
		pci_unlock_rescan_remove(); /* [한국어] 락 해제 */
	} /* [한국어] 재스캔 블록 종료 */
	return count; /* [한국어] 입력을 전부 소비했다고 알림 */
} /* [한국어] 함수 본문 종료 */
/* [한국어] dev_attr_bus_rescan - 파일 이름은 역시 "rescan" 이지만 C 심볼이
 * 달라서 위 dev_attr_dev_rescan 과 공존한다. 모드 0200(소유자 쓰기 전용),
 * show 없음. 아래 pcibus_attrs[] 에 들어가 버스 디렉터리에 붙는다. */
static struct device_attribute dev_attr_bus_rescan = __ATTR(rescan, 0200, NULL, /* [한국어] 파일 이름 "rescan", 모드 0200, show 없음 */
							    bus_rescan_store); /* [한국어] 쓰기 콜백은 위 함수 */

/*
 * [한국어]
 * reset_subordinate_store - /sys/bus/pci/devices/<BDF>/reset_subordinate 쓰기 콜백
 *
 * @dev: 대상 device. 이 속성은 pci_bridge_attrs[] 에 있어 브리지에만 생긴다.
 * @attr: 속성 서술자(미사용).
 * @buf: 사용자가 쓴 문자열. 0 이 아니면 리셋한다.
 * @count: 쓴 바이트 수.
 * @return: 성공 시 count, 권한 없으면 -EPERM, 파싱 실패면 -EINVAL,
 *   리셋이 실패하면 pci_try_reset_bridge() 의 오류를 그대로 전달한다.
 *
 * 이 파일에 1 을 쓰면 이 브리지의 하위 버스에 Secondary Bus Reset(SBR)을
 * 건다. 즉 이 브리지 아래에 매달린 모든 장치가 한꺼번에 리셋된다.
 * 장치 하나만 리셋하는 reset 속성(아래 reset_store)과 범위가 다르다.
 *
 * 언제 쓰는가: 장치 자신의 리셋(FLR 등)으로 되살아나지 않는 하드웨어를
 * 강제로 되살릴 때 쓰는 마지막 수단이다. 파급이 크기 때문에
 * capable(CAP_SYS_ADMIN) 으로 루트만 허용하고, 실패를 감추지 않고
 * 사용자에게 그대로 돌려준다 — 다른 store 들이 count 만 돌려주는 것과
 * 대비되는 점이다.
 *
 * NVMe 학습 관점: NVMe SSD 하나만 있는 루트 포트에서 이걸 쓰면 결과적으로
 * 그 SSD 가 리셋된다. 다만 같은 포트 아래에 다른 장치가 있으면 그것들도
 * 같이 죽는다. 그리고 이것은 PCIe 링크 수준의 리셋이라, NVMe 스펙의
 * 컨트롤러 리셋(CC.EN 을 0 으로 내리는 것)이나 서브시스템 리셋(NSSR)과는
 * 다른 층위다 — 리셋 후에는 BAR, MSI-X, Command 레지스터를 전부 다시
 * 설정해야 하므로 드라이버 재바인딩에 준하는 복구가 필요하다.
 *
 * 실행 컨텍스트: write(2) 프로세스 문맥. 리셋 후 링크 훈련을 기다리며
 * 수백 밀리초 단위로 잠들 수 있다.
 *
 * 호출 체인:
 *   write(2) -> sysfs -> dev_attr_store -> [reset_subordinate_store]
 *     -> pci_try_reset_bridge (drivers/pci/pci.c 에 선언, pci.h 에서 확인)
 */
static ssize_t reset_subordinate_store(struct device *dev, /* [한국어] 대상 device(브리지) */
				struct device_attribute *attr, /* [한국어] 속성 서술자 — 미사용 */
				const char *buf, size_t count) /* [한국어] 사용자 입력 문자열과 길이 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	unsigned long val; /* [한국어] 파싱된 사용자 값 */

	if (!capable(CAP_SYS_ADMIN)) /* [한국어] 하위 버스 전체를 리셋하는 파괴적 동작이라 루트만 허용 */
		return -EPERM; /* [한국어] 권한 부족 */

	if (kstrtoul(buf, 0, &val) < 0) /* [한국어] 진법 자동 판별 파싱 */
		return -EINVAL; /* [한국어] 숫자가 아니면 거부 */

	if (val) { /* [한국어] 0 이 아닐 때만 실제로 리셋한다 */
		int ret = pci_try_reset_bridge(pdev); /* [한국어] 하위 버스에 Secondary Bus Reset 을 건다. "try" 인 이유는 사용 중인 장치가 있으면 거절하기 때문 */

		if (ret) /* [한국어] 리셋 실패(사용 중이거나 지원하지 않음) */
			return ret; /* [한국어] 실패를 감추지 않고 그대로 사용자에게 전달한다 */
	} /* [한국어] 리셋 블록 종료 */

	return count; /* [한국어] 성공했거나 0 을 썼을 때 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_WO(reset_subordinate); /* [한국어] dev_attr_reset_subordinate 생성. WO = 쓰기 전용(읽을 상태가 없다) */

#if defined(CONFIG_PM) && defined(CONFIG_ACPI) /* [한국어] D3cold 진입은 슬롯 전원을 끄는 플랫폼 동작이라 전원 관리와 ACPI 가 모두 있어야 의미가 있다 */
/*
 * [한국어]
 * d3cold_allowed_store - /sys/bus/pci/devices/<BDF>/d3cold_allowed 쓰기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 사용자가 쓴 문자열. 0 이면 D3cold 금지, 그 외면 허용.
 * @count: 쓴 바이트 수.
 * @return: 성공 시 count, 파싱 실패면 -EINVAL.
 *
 * 이 파일에 0 을 쓰면 이 장치가 D3cold(전원이 완전히 끊긴 상태)로 내려가지
 * 못하게 막는다. D3cold 는 가장 깊은 절전이라 전력은 가장 아끼지만,
 * 되살아날 때 링크 훈련과 설정공간 복원이 전부 다시 필요해 지연이 크고,
 * 일부 하드웨어는 아예 제대로 깨어나지 못한다. 그런 장치를 진단할 때
 * 이 스위치로 D3cold 만 배제해 문제를 좁힌다.
 *
 * 세 줄이 순서대로 하는 일:
 *  1) pdev->d3cold_allowed 를 0/1 로 정규화해 저장한다. !! 를 쓰는 이유는
 *     이 필드가 1비트 비트필드라 2 이상을 그대로 넣으면 잘리기 때문이다.
 *  2) pci_bridge_d3_update() 로 상위 브리지의 판단을 다시 계산한다.
 *     브리지는 자기 아래 장치가 전부 D3cold 를 허용할 때에만 자신도
 *     D3cold 로 갈 수 있다. 그래서 장치 하나가 금지로 바뀌면 그 위 브리지의
 *     결론도 즉시 달라져야 한다 — 이 호출을 빠뜨리면 브리지가 전원을
 *     끊어 버려 밑의 장치가 죽는다.
 *  3) pm_runtime_resume() 으로 장치를 지금 당장 깨운다. 방금 정책을 바꿨는데
 *     장치가 이미 D3cold 에 들어가 있으면 새 정책이 반영되지 않은 상태로
 *     남는다. 한 번 깨워서 다음 절전 진입 때 새 정책이 적용되게 만든다.
 *
 * 이 속성은 CONFIG_PM 과 CONFIG_ACPI 가 모두 켜진 빌드에만 존재한다.
 * D3cold 진입은 슬롯 전원을 끄는 플랫폼 동작이라 ACPI 같은 펌웨어 인터페이스가
 * 있어야 가능하기 때문이다.
 *
 * NVMe 학습 관점: drivers/nvme 는 이 플래그를 직접 건드리지 않는다(주석을
 * 제거한 토큰 기준으로 검색해 0건임을 확인했다). 즉 nvme 드라이버가 이 값을
 * 읽거나 쓰는 일은 없고, 이것은 순수하게 PCI 전원 관리 정책을 사람이
 * 조정하는 창이다. 또한 여기서 말하는 D3cold 는 PCI 장치 전원 상태이며,
 * NVMe 스펙의 전력 상태(PS0..PSn)나 APST 와는 다른 층위다.
 *
 * 실행 컨텍스트: write(2) 프로세스 문맥. pm_runtime_resume() 이 장치를
 * 깨우는 동안 잠들 수 있다.
 *
 * 호출 체인:
 *   write(2) -> sysfs -> dev_attr_store -> [d3cold_allowed_store]
 *     -> pci_bridge_d3_update -> pm_runtime_resume
 */
static ssize_t d3cold_allowed_store(struct device *dev, /* [한국어] 대상 device */
				    struct device_attribute *attr, /* [한국어] 속성 서술자 — 미사용 */
				    const char *buf, size_t count) /* [한국어] 사용자 입력 문자열과 길이 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	unsigned long val; /* [한국어] 파싱된 사용자 값(0 = 금지, 그 외 = 허용) */

	if (kstrtoul(buf, 0, &val) < 0) /* [한국어] 진법 자동 판별 파싱 */
		return -EINVAL; /* [한국어] 숫자가 아니면 거부 */

	pdev->d3cold_allowed = !!val; /* [한국어] 1비트 비트필드이므로 !! 로 0/1 정규화해 저장 */
	pci_bridge_d3_update(pdev); /* [한국어] 상위 브리지의 "나도 D3 로 갈 수 있는가" 판단을 즉시 다시 계산. 이걸 빼면 브리지가 잘못된 전제로 전원을 끊는다 */

	pm_runtime_resume(dev); /* [한국어] 이미 절전에 들어가 있을 수 있으므로 한 번 깨워서 새 정책이 다음 진입부터 반영되게 한다 */

	return count; /* [한국어] 입력을 전부 소비했다고 알림 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * d3cold_allowed_show - /sys/bus/pci/devices/<BDF>/d3cold_allowed 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 장치가 D3cold 까지 내려가도 되는지에
 * 대한 커널의 현재 정책(1 = 허용, 0 = 금지). 지금 실제로 D3cold 에 있는지가
 * 아니라 "허용 여부" 라는 점이 중요하다. 현재 상태는 위쪽의 power_state
 * 파일에서 본다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 락 없음, 하드웨어 접근 없음.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [d3cold_allowed_show] -> sysfs_emit
 */
static ssize_t d3cold_allowed_show(struct device *dev, /* [한국어] 대상 device */
				   struct device_attribute *attr, char *buf) /* [한국어] 속성 서술자(미사용)와 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	return sysfs_emit(buf, "%u\n", pdev->d3cold_allowed); /* [한국어] 정책 비트를 그대로 0/1 로 출력 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RW(d3cold_allowed); /* [한국어] dev_attr_d3cold_allowed 생성(읽기+쓰기) */
#endif /* [한국어] CONFIG_PM && CONFIG_ACPI 블록 끝 — 둘 다 없으면 이 속성 자체가 없다 */

#ifdef CONFIG_OF /* [한국어] 디바이스 트리를 쓰는 빌드(주로 임베디드/ARM)에만 존재한다 */
/*
 * [한국어]
 * devspec_show - /sys/bus/pci/devices/<BDF>/devspec 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수. 대응하는 디바이스 트리 노드가 없으면 0
 *   (빈 파일로 읽힌다 — 오류가 아니다).
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 PCI 장치에 대응하는 디바이스 트리
 * 노드의 전체 경로. x86 처럼 ACPI 를 쓰는 플랫폼에는 디바이스 트리가 없어
 * 이 속성 자체가 빌드에 들어가지 않는다. ARM 계열 SoC 처럼 PCI 컨트롤러와
 * 그 아래 장치가 디바이스 트리에 기술되는 플랫폼에서, 커널이 이 장치를
 * 트리의 어느 노드와 짝지었는지 확인하는 용도다.
 *
 * %pOF 는 커널 printf 확장으로, struct device_node 포인터를 받아 그 노드의
 * 전체 경로를 찍어 준다. 일반 %s 로는 노드 이름밖에 못 찍는다.
 *
 * 노드가 없을 때 오류가 아니라 0 을 돌려주는 이유: 디바이스 트리에 기술되지
 * 않은 장치(핫플러그로 발견된 장치 등)는 정상적인 상황이지 오류가 아니다.
 * 0 을 돌려주면 사용자 공간에는 빈 파일로 보인다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [devspec_show]
 *     -> pci_device_to_OF_node -> sysfs_emit
 */
static ssize_t devspec_show(struct device *dev, /* [한국어] 대상 device */
			    struct device_attribute *attr, char *buf) /* [한국어] 속성 서술자(미사용)와 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	struct device_node *np = pci_device_to_OF_node(pdev); /* [한국어] 이 PCI 장치에 대응하는 디바이스 트리 노드를 찾는다. 없으면 NULL */

	if (np == NULL) /* [한국어] 디바이스 트리에 기술되지 않은 장치인가 */
		return 0; /* [한국어] 오류가 아니라 "내용 없음". 사용자에게는 빈 파일로 보인다 */
	return sysfs_emit(buf, "%pOF\n", np); /* [한국어] %pOF 는 device_node 를 전체 경로 문자열로 찍는 커널 printf 확장 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(devspec); /* [한국어] dev_attr_devspec 생성 -> CONFIG_OF 빌드에만 존재 */
#endif /* [한국어] CONFIG_OF 블록 끝 — 디바이스 트리가 없는 플랫폼에는 이 속성 자체가 없다 */

/* [한국어] pci_dev_attrs - 모든 PCI 장치에 조건 없이 만들어지는 기본 속성 목록.
 * 아래 pci_dev_group 에 담기고, 그 그룹은 pci_dev_groups[] 를 거쳐
 * drivers/pci/pci-driver.c 의 pci_bus_type.dev_groups 로 등록된다.
 * 이 그룹에는 is_visible 콜백이 없다 — 즉 여기 있는 파일들은 장치 종류를
 * 가리지 않고 항상 생긴다. 장치에 따라 있거나 없어야 하는 것들(브리지 전용,
 * PCIe 전용, hotplug 전용)은 다른 배열로 분리되어 있다.
 * 전처리 조건이 붙은 항목은 그 커널 설정이 꺼져 있으면 배열에서 아예 빠져
 * 파일도 생기지 않는다.
 * 설정자: 이 정적 초기화뿐. 읽는 자: 드라이버 코어의 sysfs 등록 코드.
 * 동기화: 정적 데이터라 락 불필요. */
static struct attribute *pci_dev_attrs[] = { /* [한국어] 배열 시작 */
	&dev_attr_power_state.attr, /* [한국어] power_state — 커널이 파악한 현재 D 상태 이름 */
	&dev_attr_resource.attr, /* [한국어] resource — BAR 별 시작/끝/플래그 목록(줄 번호가 BAR 번호) */
	&dev_attr_vendor.attr, /* [한국어] vendor — Vendor ID(pci_config_attr 매크로가 만든 것) */
	&dev_attr_device.attr, /* [한국어] device — Device ID */
	&dev_attr_subsystem_vendor.attr, /* [한국어] subsystem_vendor — 완제품 제조사 ID */
	&dev_attr_subsystem_device.attr, /* [한국어] subsystem_device — 완제품 모델 ID */
	&dev_attr_revision.attr, /* [한국어] revision — 칩 리비전 */
	&dev_attr_class.attr, /* [한국어] class — 24비트 클래스 코드. NVMe 판별의 근거가 되는 값 */
	&dev_attr_irq.attr, /* [한국어] irq — MSI 첫 벡터 또는 레거시 INTx 번호(MSI-X 는 여기 안 나온다) */
	&dev_attr_local_cpus.attr, /* [한국어] local_cpus — 가까운 CPU 집합(비트마스크) */
	&dev_attr_local_cpulist.attr, /* [한국어] local_cpulist — 가까운 CPU 집합(범위 목록) */
	&dev_attr_modalias.attr, /* [한국어] modalias — udev/modprobe 가 드라이버를 고르는 별칭 문자열 */
#ifdef CONFIG_NUMA /* [한국어] NUMA 를 아는 커널에서만 */
	&dev_attr_numa_node.attr, /* [한국어] numa_node — 이 장치가 속한 노드(쓰기도 가능하지만 커널이 taint 된다) */
#endif /* [한국어] CONFIG_NUMA 끝 */
	&dev_attr_dma_mask_bits.attr, /* [한국어] dma_mask_bits — 스트리밍 DMA 주소 비트 수(NVMe 는 보통 64) */
	&dev_attr_consistent_dma_mask_bits.attr, /* [한국어] consistent_dma_mask_bits — 코히런트 DMA 주소 비트 수 */
	&dev_attr_enable.attr, /* [한국어] enable — 활성화 참조 계수(읽기) / 활성화·비활성화(쓰기) */
	&dev_attr_broken_parity_status.attr, /* [한국어] broken_parity_status — 패리티 오류 보고를 믿을지 여부 */
	&dev_attr_msi_bus.attr, /* [한국어] msi_bus — 앞으로의 MSI/MSI-X 사용 허용 여부 */
#if defined(CONFIG_PM) && defined(CONFIG_ACPI) /* [한국어] 전원 관리와 ACPI 가 모두 있을 때만 */
	&dev_attr_d3cold_allowed.attr, /* [한국어] d3cold_allowed — D3cold 진입 허용 정책 */
#endif /* [한국어] CONFIG_PM && CONFIG_ACPI 끝 */
#ifdef CONFIG_OF /* [한국어] 디바이스 트리를 쓰는 빌드에서만 */
	&dev_attr_devspec.attr, /* [한국어] devspec — 대응하는 디바이스 트리 노드 경로 */
#endif /* [한국어] CONFIG_OF 끝 */
	&dev_attr_ari_enabled.attr, /* [한국어] ari_enabled — 이 장치가 앉은 버스에서 ARI 가 켜졌는지 */
	NULL, /* [한국어] 끝 센티널 — sysfs 코어가 NULL 을 만날 때까지 순회한다 */
}; /* [한국어] 배열 종료 */

/* [한국어] pci_bridge_attrs - 브리지에만 의미가 있는 속성 목록.
 * 아래 pci_bridge_attr_group 에 담기고, 그 그룹의 is_visible 인
 * pci_bridge_attrs_are_visible() 이 pci_is_bridge() 가 아닌 장치에는
 * 0 을 돌려주어 파일 자체를 만들지 않는다. NVMe SSD 같은 엔드포인트에는
 * 이 세 파일이 없다.
 * 설정자: 이 정적 초기화뿐. 읽는 자: 드라이버 코어. 동기화: 불필요. */
static struct attribute *pci_bridge_attrs[] = { /* [한국어] 배열 시작 */
	&dev_attr_subordinate_bus_number.attr, /* [한국어] subordinate_bus_number — 이 브리지가 담당하는 버스 번호 구간의 끝 */
	&dev_attr_secondary_bus_number.attr, /* [한국어] secondary_bus_number — 이 브리지 바로 아래 버스 번호 */
	&dev_attr_reset_subordinate.attr, /* [한국어] reset_subordinate — 하위 버스 전체에 SBR 을 거는 쓰기 전용 파일 */
	NULL, /* [한국어] 끝 센티널 */
}; /* [한국어] 배열 종료 */

/* [한국어] pcie_dev_attrs - PCIe 장치에만 의미가 있는 링크 관련 속성 목록.
 * 아래 pcie_dev_attr_group 에 담기고, 그 is_visible 인
 * pcie_dev_attrs_are_visible() 이 pci_is_pcie() 가 아닌(구형 병렬 PCI)
 * 장치에는 파일을 만들지 않는다. 그래서 각 show 함수 안에서 PCIe 여부를
 * 다시 검사하지 않아도 된다.
 * NVMe SSD 는 당연히 PCIe 이므로 이 네 파일이 항상 있으며, 성능 진단의
 * 출발점이 된다.
 * 설정자: 이 정적 초기화뿐. 읽는 자: 드라이버 코어. 동기화: 불필요. */
static struct attribute *pcie_dev_attrs[] = { /* [한국어] 배열 시작 */
	&dev_attr_current_link_speed.attr, /* [한국어] current_link_speed — 지금 협상된 속도. 기대보다 낮으면 배선/훈련 문제 */
	&dev_attr_current_link_width.attr, /* [한국어] current_link_width — 지금 협상된 레인 수 */
	&dev_attr_max_link_width.attr, /* [한국어] max_link_width — 장치가 지원하는 최대 레인 수 */
	&dev_attr_max_link_speed.attr, /* [한국어] max_link_speed — 장치가 지원하는 최대 속도 */
	NULL, /* [한국어] 끝 센티널 */
}; /* [한국어] 배열 종료 */

/* [한국어] pcibus_attrs - PCI 장치가 아니라 PCI "버스" 객체에 붙는 속성 목록.
 * /sys/class/pci_bus/<도메인:버스>/ 아래에 만들어진다.
 * 설정자: 이 정적 초기화뿐. 읽는 자: 드라이버 코어. 동기화: 불필요. */
static struct attribute *pcibus_attrs[] = { /* [한국어] 배열 시작 */
	&dev_attr_bus_rescan.attr, /* [한국어] rescan(버스판) — 이 버스만 다시 열거한다. 빈 브리지면 윈도우 재조정까지 */
	&dev_attr_cpuaffinity.attr, /* [한국어] cpuaffinity — 이 버스에 가까운 CPU 집합(비트마스크) */
	&dev_attr_cpulistaffinity.attr, /* [한국어] cpulistaffinity — 같은 정보의 범위 목록 형식 */
	NULL, /* [한국어] 끝 센티널 */
}; /* [한국어] 배열 종료 */

/* [한국어] pcibus_group - 위 버스 속성 배열 하나를 담은 그룹. */
static const struct attribute_group pcibus_group = { /* [한국어] 구조체 초기화 시작 */
	.attrs = pcibus_attrs, /* [한국어] 이 그룹이 만들 속성 목록.
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: 드라이버 코어의 class 등록 경로.
		 * 값 범위: NULL 로 끝나는 struct attribute 포인터 배열.
		 * 동기화: const 정적 데이터라 락 불필요.
		 * is_visible 이 없어 모든 pci_bus 에 세 파일이 항상 생긴다. */
}; /* [한국어] 구조체 초기화 종료 */

/* [한국어] pcibus_groups - 버스 객체용 최상위 그룹 배열.
 * drivers/pci/probe.c 의 pcibus_class 가 .dev_groups = pcibus_groups 로
 * 이 배열을 가져간다(그 파일에서 확인). static 이 아닌 이유가 그것이다. */
const struct attribute_group *pcibus_groups[] = { /* [한국어] 배열 시작 */
	&pcibus_group, /* [한국어] 위에서 만든 그룹 하나 */
	NULL, /* [한국어] 끝 센티널 */
}; /* [한국어] 배열 종료 */

/*
 * [한국어]
 * boot_vga_show - /sys/bus/pci/devices/<BDF>/boot_vga 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 장치가 부팅 화면을 담당한 VGA 장치인지
 * (1) 아닌지(0). 그래픽 카드가 여러 장일 때 어느 것이 콘솔을 갖고 있었는지를
 * 알아내는 용도라, 이 파일은 아래 pci_dev_dev_attrs[] 에 있고 그 그룹의
 * is_visible 인 pci_dev_attrs_are_visible() 이 VGA 장치에만 파일을 만든다.
 *
 * 판정이 두 갈래인 이유:
 *  1) VGA 중재(arbitration) 계층이 기본 장치를 알고 있으면
 *     (vga_default_device() 가 NULL 이 아니면) 그것과 같은지만 비교하면
 *     된다. 이쪽이 정확한 답이다.
 *  2) 모르면 옛 방식으로 추측한다 — 확장 ROM 자원에 IORESOURCE_ROM_SHADOW
 *     플래그가 있는지 본다. 부팅 시 펌웨어가 VGA BIOS 를 레거시 주소
 *     0xC0000 대역에 복사(shadow)해 두는데, 그 흔적이 이 플래그다. 즉
 *     "펌웨어가 이 카드의 ROM 을 그림자 복사했다면 그 카드가 부팅 화면을
 *     담당했다" 는 추론이다.
 * !! 로 감싼 것은 플래그 AND 결과(0 이 아닌 임의의 비트값)를 0/1 로
 * 정규화하기 위해서다.
 *
 * NVMe 학습 관점: NVMe SSD 에는 이 파일이 만들어지지 않는다(VGA 클래스가
 * 아니므로). 여기 나오는 IORESOURCE_ROM_SHADOW 는 아래 pci_read_rom() 이
 * 다루는 옵션 ROM 과 같은 자원 슬롯(PCI_ROM_RESOURCE)을 가리킨다는 점만
 * 연결해서 보면 된다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [boot_vga_show] -> vga_default_device
 */
static ssize_t boot_vga_show(struct device *dev, struct device_attribute *attr, /* [한국어] 대상 device 와 속성 서술자(미사용) */
			     char *buf) /* [한국어] 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	struct pci_dev *vga_dev = vga_default_device(); /* [한국어] VGA 중재 계층이 알고 있는 "기본" VGA 장치. 없으면 NULL */

	if (vga_dev) /* [한국어] 중재 계층이 답을 알고 있는가 */
		return sysfs_emit(buf, "%u\n", (pdev == vga_dev)); /* [한국어] 포인터 비교로 정확히 판정. 비교 결과 0/1 이 그대로 출력된다 */

	return sysfs_emit(buf, "%u\n", /* [한국어] 모를 때의 대체 판정 */
			  !!(pdev->resource[PCI_ROM_RESOURCE].flags & /* [한국어] 확장 ROM 자원 슬롯의 플래그를 본다 */
			     IORESOURCE_ROM_SHADOW)); /* [한국어] 펌웨어가 VGA BIOS 를 레거시 주소로 그림자 복사한 흔적. !! 로 0/1 정규화 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RO(boot_vga); /* [한국어] dev_attr_boot_vga 생성 -> pci_dev_dev_attrs[] 경유로 VGA 장치에만 생긴다 */

/*
 * [한국어]
 * serial_number_show - /sys/bus/pci/devices/<BDF>/serial_number 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 성공 시 찍은 바이트 수, DSN 이 없으면 -EIO.
 *
 * 이 파일을 읽으면 알 수 있는 것: PCIe Device Serial Number(DSN) 확장
 * capability 가 알려 주는 64비트 고유 식별자. "AA-BB-CC-DD-EE-FF-00-11"
 * 처럼 바이트를 하이픈으로 이어 붙인 형태로 나온다.
 * 이 파일은 아래 pci_dev_dev_attrs[] 에 있고, 그 그룹의 is_visible 인
 * pci_dev_attrs_are_visible() 이 pci_get_dsn() 이 0 이 아닌 장치에만
 * 파일을 만든다. 그래서 DSN 이 없는 장치에는 파일 자체가 없다.
 *
 * put_unaligned_be64() 를 쓰는 이유: DSN 은 u64 로 얻어지는데, 이것을
 * 바이트 순서가 정해진 배열로 펴야 사람이 읽는 표기가 된다. 그냥
 * memcpy 하면 리틀엔디언 기계에서 바이트 순서가 뒤집힌다. be64 판을 써서
 * 항상 빅엔디언(사람이 읽는 순서)으로 넣고, unaligned 판인 이유는 스택
 * 배열 bytes[8] 이 8바이트 경계에 정렬돼 있다는 보장이 없어 정렬을
 * 요구하는 아키텍처에서 예외가 날 수 있기 때문이다.
 *
 * "%8phD" 는 커널 printf 확장이다 — 포인터가 가리키는 8바이트를 16진수로
 * 찍되 'D' 판이라 바이트 사이를 하이픈으로 잇는다. 이 확장의 구현은
 * lib/vsprintf.c 에 있고 이 트리에는 그 파일이 없어 원문은 확인할 수 없다.
 *
 * 권한: DEVICE_ATTR_ADMIN_RO 라 관리자(루트)만 읽을 수 있는 판으로 만들어져
 * 있다. 정확한 모드 숫자는 include/linux/device.h 의 매크로 정의에 있고
 * 이 트리에는 그 헤더가 없어 확인할 수 없다. 왜 관리자 전용인지에 대한
 * 상류 주석은 없다 — 다만 DSN 이 장치마다 유일한 값이라는 것은 사실이다.
 *
 * NVMe 학습 관점: 이 값은 PCIe 계층의 시리얼 번호이지, `nvme list` 나
 * `nvme id-ctrl` 이 보여 주는 컨트롤러 시리얼 번호(SN)와는 다른 값이다.
 * 후자는 NVMe Identify Controller 자료구조의 sn[20] 필드에서 오며,
 * 그 구조체는 include/linux/nvme.h 의 struct nvme_id_ctrl 에서 확인할 수
 * 있다. 두 값이 우연히 같을 이유가 없으므로 섞어 쓰면 안 된다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. pci_get_dsn() 이 확장 설정공간을
 * 읽으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [serial_number_show]
 *     -> pci_get_dsn -> put_unaligned_be64 -> sysfs_emit
 */
static ssize_t serial_number_show(struct device *dev, /* [한국어] 대상 device */
				  struct device_attribute *attr, char *buf) /* [한국어] 속성 서술자(미사용)와 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	u64 dsn; /* [한국어] 읽어 올 64비트 Device Serial Number 원본 값 */
	u8 bytes[8]; /* [한국어] 사람이 읽는 순서로 펴 담을 바이트 배열. 스택이라 8바이트 정렬 보장이 없다 */

	dsn = pci_get_dsn(pci_dev); /* [한국어] DSN 확장 capability 를 찾아 64비트를 읽는다. 없으면 0 */
	if (!dsn) /* [한국어] DSN capability 가 없거나 값이 0 인 경우 */
		return -EIO; /* [한국어] 0 을 그대로 찍으면 유효한 시리얼처럼 보이므로 오류로 처리한다 */

	put_unaligned_be64(dsn, bytes); /* [한국어] 정렬 요구 없이, 항상 빅엔디언(사람이 읽는 순서)으로 8바이트에 펴 넣는다 */
	return sysfs_emit(buf, "%8phD\n", bytes); /* [한국어] 8바이트를 하이픈으로 이어 16진수로 출력하는 커널 printf 확장 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_ADMIN_RO(serial_number); /* [한국어] dev_attr_serial_number 생성. ADMIN 판이라 관리자만 읽는 모드로 만들어진다 */

/*
 * [한국어]
 * pci_read_config - /sys/bus/pci/devices/<BDF>/config 바이너리 파일의 읽기 콜백
 *
 * @filp: 이 파일을 연 struct file. 권한 검사에 쓰인다 — 지금 읽고 있는
 *   프로세스의 권한이 아니라 "파일을 열 때의" 권한을 봐야 하기 때문이다.
 * @kobj: 이 바이너리 속성이 붙은 kobject. 여기서 device 를 거쳐 pci_dev 로
 *   되돌린다.
 * @bin_attr: 바이너리 속성 서술자(이 함수는 쓰지 않는다).
 * @buf: 커널이 준비한 커널 공간 버퍼. 사용자 포인터가 아니므로 copy_to_user
 *   가 필요 없다 — sysfs 코어가 대신 복사한다.
 * @off: 읽기 시작할 설정공간 오프셋(바이트).
 * @count: 읽으려는 바이트 수.
 * @return: 실제로 채운 바이트 수. 범위를 완전히 벗어나면 0(EOF).
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 장치의 PCI 설정공간 원본 바이트열.
 * lspci 가 -x 옵션으로 덤프하는 것이 바로 이 내용이고, capability 목록
 * 순회(0x34 의 Capabilities Pointer 를 따라가는 것)도 여기서 읽은 바이트로
 * 이뤄진다. 바이너리 속성이므로 pread(2) 로 원하는 오프셋만 집어 읽을 수
 * 있다 — 일반 텍스트 속성으로는 불가능한 일이다.
 *
 * === 권한에 따라 보이는 범위가 달라진다 ===
 * size 의 초기값 64 가 그 출발점이다. 즉 아무 권한이 없으면 표준 헤더 앞부분
 * 64바이트만 보인다.
 *  - file_ns_capable(filp, &init_user_ns, CAP_SYS_ADMIN) 이 참이면
 *    dev->cfg_size 전부(표준 256바이트, PCIe 확장이면 4096바이트)를 준다.
 *    "지금 read 를 부른 태스크" 가 아니라 "이 파일을 연 시점의 자격" 을
 *    보는 이유는, 특권 프로세스가 fd 를 열어 비특권 프로세스에 넘기는
 *    공격을 막기 위해서다. init_user_ns 를 지정한 것은 컨테이너 안에서
 *    얻은 가짜 CAP_SYS_ADMIN 으로는 통과하지 못하게 하려는 것이다.
 *  - CardBus 브리지는 예외로 128바이트까지 허용한다.
 * 상류 영어 주석이 이 제한의 이유를 밝힌다: 정의되지 않은 설정공간 영역을
 * 읽으면 잠겨 버리는(lock up) 칩이 여럿 있었다. 즉 보안만이 아니라 하드웨어
 * 보호가 함께 걸린 제한이다.
 *
 * === 경계 처리 ===
 *  - off 가 size 를 넘으면 0 을 돌려준다. 사용자 공간에는 EOF 로 보인다.
 *  - off + count 가 size 를 넘으면 count 를 남은 만큼으로 줄인다. 이때
 *    size 는 "허용 상한" 에서 "이번에 옮길 바이트 수" 로 의미가 바뀐다.
 *  - 그렇지 않으면 size 에 count 를 넣어 같은 의미로 통일한다.
 * 이 뒤로 size 는 "아직 옮기지 않은 바이트 수" 로만 쓰이며, off 는 계속
 * 앞으로 나아간다. 그래서 버퍼 안의 위치는 항상 off - init_off 로 구한다 —
 * init_off 를 따로 저장해 둔 이유가 이것이다.
 *
 * === 왜 정렬에 맞춰 쪼개는가 ===
 * PCI 설정공간 접근은 하드웨어 수준에서 1/2/4바이트 단위이고, 2바이트
 * 접근은 짝수, 4바이트 접근은 4의 배수 오프셋이어야 한다. 그래서
 *  1) off 가 홀수면 먼저 1바이트를 읽어 짝수로 맞추고,
 *  2) 아직 4의 배수가 아니면 2바이트를 읽어 4의 배수로 맞춘 다음,
 *  3) 본체를 4바이트씩 훑고,
 *  4) 꼬리를 2바이트, 1바이트 순으로 처리한다.
 * 가운데 루프에서 최대한 4바이트씩 옮기는 것이 접근 횟수를 줄여 빠르다.
 *
 * cond_resched() 가 4바이트 루프 안에만 있는 이유: 확장 설정공간 4096바이트를
 * 통째로 읽으면 이 루프가 1024번 돈다. 각 접근이 하드웨어를 기다리므로
 * 누적 시간이 길어져, 선점이 꺼진 커널에서는 다른 태스크가 굶는다.
 * 여기서 자발적으로 CPU 를 양보한다.
 *
 * pci_user_read_config_* 계열을 쓰는 것도 중요하다. 커널 내부용
 * pci_read_config_* 계열과 달리, user 판은 장치가 제거 중이거나 접근이
 * 금지된 상태를 확인하고 실패 시 0xff 로 채워 준다. 그래서 여기서 반환값을
 * 검사하지 않아도 사용자에게는 "존재하지 않는 곳은 전부 1" 이라는 PCI 의
 * 관례적 결과가 그대로 보인다.
 *
 * NVMe 학습 관점: NVMe 드라이버가 이 함수를 부르는 일은 없다. 대신
 * NVMe SSD 를 조사하는 사용자 공간 도구가 이 파일을 읽는다. 루트가 아니면
 * 64바이트만 보이므로, 그 안에 없는 PCIe Capability(링크 상태, MSI-X 표
 * 위치 등)는 보이지 않는다. `lspci -vv` 가 루트로 실행해야 온전한 이유다.
 *
 * 실행 컨텍스트: read/pread(2) 프로세스 문맥. 런타임 PM 을 깨우고 설정공간을
 * 반복 접근하므로 잠들 수 있다. 락은 잡지 않으며, 설정공간 접근의 직렬화는
 * pci_user_read_config_* 안쪽에서 처리된다.
 *
 * 에러 경로: 범위를 벗어나면 0(EOF). 개별 접근 실패는 위에서 말한 대로
 * 0xff 로 나타나며 여기서는 오류로 승격하지 않는다.
 *
 * 호출 체인:
 *   read/pread(2) -> sysfs bin_attr read -> [pci_read_config]
 *     -> pci_user_read_config_byte/word/dword (drivers/pci/access.c)
 */
static ssize_t pci_read_config(struct file *filp, struct kobject *kobj, /* [한국어] 파일 핸들(권한 검사용)과 이 속성이 붙은 kobject */
			       const struct bin_attribute *bin_attr, char *buf, /* [한국어] 바이너리 속성 서술자(미사용)와 커널 공간 출력 버퍼 */
			       loff_t off, size_t count) /* [한국어] 읽기 시작 오프셋과 요청 바이트 수 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *dev = to_pci_dev(kobj_to_dev(kobj)); /* [한국어] kobject -> device -> pci_dev 두 단계 복원. 바이너리 속성은 device 가 아니라 kobject 를 받는다 */
	unsigned int size = 64; /* [한국어] 권한 없는 사용자에게 보여 줄 기본 상한. 표준 헤더 앞 64바이트뿐이다 */
	loff_t init_off = off; /* [한국어] 시작 오프셋을 보존한다. 아래에서 off 가 계속 전진하므로 버퍼 위치는 off - init_off 로 구한다 */
	u8 *data = (u8 *) buf; /* [한국어] 바이트 단위로 채우기 위한 별칭. buf 는 커널 버퍼라 copy_to_user 가 필요 없다 */

	/* Several chips lock up trying to read undefined config space */
	/* [한국어] 위 영어 주석의 뜻: 정의되지 않은 설정공간을 읽으면 멈춰
	 * 버리는 칩이 실제로 여럿 있었다. 그래서 이 제한은 정보 은닉만이
	 * 아니라 하드웨어 보호이기도 하다. */
	if (file_ns_capable(filp, &init_user_ns, CAP_SYS_ADMIN)) /* [한국어] "지금 읽는 태스크" 가 아니라 "파일을 연 시점" 의 자격을 본다. 특권 fd 를 넘겨받는 우회를 막는다. init_user_ns 지정으로 컨테이너 내부 권한은 인정하지 않는다 */
		size = dev->cfg_size; /* [한국어] 관리자에게는 전부 — 표준 256바이트, PCIe 확장 장치면 4096바이트 */
	else if (dev->hdr_type == PCI_HEADER_TYPE_CARDBUS) /* [한국어] CardBus 브리지는 헤더가 128바이트까지 정의되어 있다 */
		size = 128; /* [한국어] 그 경우에만 64 대신 128 까지 허용 */

	if (off > size) /* [한국어] 시작 위치가 아예 허용 범위 밖인가 */
		return 0; /* [한국어] 0 = EOF. 오류가 아니라 "더 읽을 것이 없음" 이다 */
	if (off + count > size) { /* [한국어] 요청 끝이 상한을 넘는가 */
		size -= off; /* [한국어] 여기서 size 의 의미가 "상한" 에서 "남은 바이트 수" 로 바뀐다 */
		count = size; /* [한국어] 사용자에게도 줄어든 길이를 보고할 수 있게 count 를 맞춘다 */
	} else { /* [한국어] 요청이 상한 안에 완전히 들어오는 경우 */
		size = count; /* [한국어] 이후 로직이 size 를 "옮길 바이트 수" 로만 쓰도록 의미를 통일한다 */
	} /* [한국어] 경계 조정 종료 */

	pci_config_pm_runtime_get(dev); /* [한국어] 설정공간을 실제로 접근하므로 장치를 D0 로 깨운다. 아래 모든 접근이 끝날 때까지 유지된다 */

	if ((off & 1) && size) { /* [한국어] 시작이 홀수 주소인가. 그렇다면 2바이트 접근을 할 수 없으므로 1바이트로 정렬을 맞춘다 */
		u8 val; /* [한국어] 읽어 온 1바이트 */
		pci_user_read_config_byte(dev, off, &val); /* [한국어] 사용자 경유 접근 — 장치가 사라졌으면 0xff 를 채워 준다 */
		data[off - init_off] = val; /* [한국어] 버퍼 안의 대응 위치에 저장. init_off 기준 상대 위치다 */
		off++; /* [한국어] 설정공간 커서 전진 */
		size--; /* [한국어] 남은 바이트 감소 */
	} /* [한국어] 1바이트 정렬 맞춤 종료 */

	if ((off & 3) && size > 2) { /* [한국어] 아직 4의 배수가 아니고, 2바이트를 옮겨도 남는 것이 있는가. size > 2 조건이 있어야 꼬리를 여기서 먹어 버리지 않는다 */
		u16 val; /* [한국어] 읽어 온 2바이트 */
		pci_user_read_config_word(dev, off, &val); /* [한국어] 2바이트 접근. 이 시점 off 는 짝수임이 보장된다 */
		data[off - init_off] = val & 0xff; /* [한국어] 하위 바이트를 먼저 — 설정공간은 리틀엔디언이므로 낮은 주소가 하위 바이트다 */
		data[off - init_off + 1] = (val >> 8) & 0xff; /* [한국어] 상위 바이트를 다음 위치에 */
		off += 2; /* [한국어] 커서 전진 */
		size -= 2; /* [한국어] 남은 바이트 감소 */
	} /* [한국어] 4바이트 정렬 맞춤 종료 */

	while (size > 3) { /* [한국어] 본체 루프 — 4바이트씩 옮길 수 있는 동안 반복. 접근 횟수를 최소화한다 */
		u32 val; /* [한국어] 읽어 온 4바이트 */
		pci_user_read_config_dword(dev, off, &val); /* [한국어] 4바이트 접근. off 는 4의 배수임이 보장된다 */
		data[off - init_off] = val & 0xff; /* [한국어] 바이트 0 (최하위) */
		data[off - init_off + 1] = (val >> 8) & 0xff; /* [한국어] 바이트 1 */
		data[off - init_off + 2] = (val >> 16) & 0xff; /* [한국어] 바이트 2 */
		data[off - init_off + 3] = (val >> 24) & 0xff; /* [한국어] 바이트 3 (최상위) */
		off += 4; /* [한국어] 커서 전진 */
		size -= 4; /* [한국어] 남은 바이트 감소 */
		cond_resched(); /* [한국어] 확장 설정공간 4096바이트면 이 루프가 1024번 돈다. 자발적으로 CPU 를 양보해 다른 태스크가 굶지 않게 한다 */
	} /* [한국어] 본체 루프 종료 */

	if (size >= 2) { /* [한국어] 꼬리 처리 — 2바이트 이상 남았다면 */
		u16 val; /* [한국어] 읽어 온 2바이트 */
		pci_user_read_config_word(dev, off, &val); /* [한국어] 2바이트 접근 */
		data[off - init_off] = val & 0xff; /* [한국어] 하위 바이트 */
		data[off - init_off + 1] = (val >> 8) & 0xff; /* [한국어] 상위 바이트 */
		off += 2; /* [한국어] 커서 전진 */
		size -= 2; /* [한국어] 남은 바이트 감소 */
	} /* [한국어] 2바이트 꼬리 종료 */

	if (size > 0) { /* [한국어] 마지막 1바이트가 남았는가 */
		u8 val; /* [한국어] 읽어 온 1바이트 */
		pci_user_read_config_byte(dev, off, &val); /* [한국어] 1바이트 접근 */
		data[off - init_off] = val; /* [한국어] 버퍼 마지막 자리에 저장. 여기서는 off 를 더 전진시킬 필요가 없다 */
	} /* [한국어] 1바이트 꼬리 종료 */

	pci_config_pm_runtime_put(dev); /* [한국어] 모든 접근이 끝났으므로 PM 참조를 놓는다. get 과 반드시 짝이다 */

	return count; /* [한국어] 경계 조정 단계에서 확정된 실제 전송 길이를 반환 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * pci_write_config - /sys/bus/pci/devices/<BDF>/config 바이너리 파일의 쓰기 콜백
 *
 * @filp: 이 파일을 연 struct file(이 함수에서는 쓰지 않는다).
 * @kobj: 이 바이너리 속성이 붙은 kobject.
 * @bin_attr: 바이너리 속성 서술자(미사용).
 * @buf: 사용자가 쓴 내용이 담긴 커널 버퍼.
 * @off: 쓰기 시작할 설정공간 오프셋.
 * @count: 쓰려는 바이트 수.
 * @return: 실제로 쓴 바이트 수. 락다운이면 그 오류, 범위 밖이면 0.
 *
 * 이 파일에 쓰면 장치의 PCI 설정공간이 직접 바뀐다. setpci 같은 도구가
 * 쓰는 통로이며, 잘못 쓰면 동작 중인 장치를 즉시 망가뜨릴 수 있는
 * 가장 위험한 인터페이스 중 하나다. 그래서 방어가 여러 겹이다.
 *
 * === 방어 1: 커널 락다운 ===
 * security_locked_down(LOCKDOWN_PCI_ACCESS) 를 가장 먼저 부른다. 락다운이
 * 켜진 시스템에서는 설정공간 임의 쓰기가 서명된 커널의 무결성을 우회하는
 * 수단이 된다(BAR 를 옮겨 커널 메모리에 DMA 를 걸 수 있다). 여기서 걸리면
 * 아무것도 하지 않고 그 오류를 그대로 돌려준다. 읽기 쪽(pci_read_config)에는
 * 이 검사가 없다는 점이 대비된다 — 읽기는 무결성을 깨뜨리지 않기 때문이다.
 *
 * === 방어 2: 파일 모드 ===
 * 아래 BIN_ATTR(config, 0644, ...) 에서 보듯 쓰기 비트는 소유자에게만 있다.
 * 즉 루트만 이 경로에 들어올 수 있어, 여기서 capable() 을 따로 부르지 않는다.
 *
 * === 방어 3: 커널 전용 영역 경고 ===
 * resource_is_exclusive(&dev->driver_exclusive_resource, off, count) 는
 * "드라이버가 자기만 쓰겠다고 예약해 둔 설정공간 구간" 과 이번 쓰기가
 * 겹치는지 본다. 그 부모 리소스는 drivers/pci/probe.c 의 pci_device_add()
 * 에서 start=0, end=-1 로 주소 공간 전체를 덮도록 초기화되며, 실제 예약은
 * 그 아래에 자식 리소스로 달린다. 다만 이 트리 안에서 자식을 실제로
 * 등록하는 코드는 찾지 못했다(drivers/pci 전체를 검색한 결과다).
 * 겹치면 쓰기를 막지는 않고, 대신 (a) 어떤 프로세스가 그랬는지
 * current->comm 과 함께 한 번만 경고를 찍고, (b) add_taint(TAINT_USER) 로
 * 커널을 오염 표시한다. 이후 버그 리포트가 올라오면 "사용자가 설정공간을
 * 직접 건드린 커널" 이었음이 로그에 남는다. pci_warn_once 인 이유는 같은
 * 경고가 반복해서 로그를 덮지 않게 하기 위해서다.
 *
 * === 읽기 쪽과 다른 점: 범위 상한 ===
 * 읽기는 권한에 따라 64/128/전체로 상한이 달라졌지만, 쓰기는 언제나
 * dev->cfg_size 전체다. 애초에 루트만 들어올 수 있는 경로라 권한별로
 * 나눌 이유가 없기 때문이다.
 *
 * === 정렬 처리 ===
 * 읽기와 완전히 대칭이다. 홀수 오프셋이면 1바이트, 4의 배수가 아니면
 * 2바이트로 맞춘 뒤 4바이트씩 본체를 처리하고, 꼬리를 2바이트·1바이트로
 * 마무리한다. 다만 방향이 반대라, 버퍼의 바이트들을 시프트와 OR 로 조립해
 * 하나의 워드/더블워드를 만든 다음 하드웨어에 쓴다. 낮은 주소 바이트가
 * 하위 비트로 가는 것은 설정공간이 리틀엔디언이기 때문이다.
 * 읽기 쪽에 있던 cond_resched() 가 여기에는 없다 — 이 트리의 정보만으로는
 * 그 비대칭의 이유를 확인할 수 없다.
 *
 * NVMe 학습 관점: 이 경로로 NVMe SSD 의 Command 레지스터(오프셋 0x04)를
 * 건드려 Bus Master 나 Memory Space 를 꺼 버리면, 동작 중인 컨트롤러가
 * DMA 를 완료하지 못해 I/O 가 멈추고 커널이 타임아웃 복구에 들어간다.
 * nvme 드라이버는 이 함수를 부르지 않는다 — 순수하게 사용자 공간용 통로다.
 *
 * 실행 컨텍스트: write/pwrite(2) 프로세스 문맥. 런타임 PM 을 깨우고 하드웨어
 * 접근을 반복하므로 잠들 수 있다.
 *
 * 호출 체인:
 *   write/pwrite(2) -> sysfs bin_attr write -> [pci_write_config]
 *     -> pci_user_write_config_byte/word/dword (drivers/pci/access.c)
 */
static ssize_t pci_write_config(struct file *filp, struct kobject *kobj, /* [한국어] 파일 핸들(여기서는 미사용)과 이 속성이 붙은 kobject */
				const struct bin_attribute *bin_attr, char *buf, /* [한국어] 바이너리 속성 서술자(미사용)와 사용자가 쓴 내용이 담긴 커널 버퍼 */
				loff_t off, size_t count) /* [한국어] 쓰기 시작 오프셋과 요청 바이트 수 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *dev = to_pci_dev(kobj_to_dev(kobj)); /* [한국어] kobject -> device -> pci_dev 복원 */
	unsigned int size = count; /* [한국어] 아직 쓰지 않은 바이트 수. 읽기 쪽과 달리 처음부터 count 로 시작한다 */
	loff_t init_off = off; /* [한국어] 시작 오프셋 보존 — 버퍼 위치는 off - init_off */
	u8 *data = (u8 *) buf; /* [한국어] 바이트 단위로 꺼내 조립하기 위한 별칭 */
	int ret; /* [한국어] 락다운 검사 결과 */

	ret = security_locked_down(LOCKDOWN_PCI_ACCESS); /* [한국어] 커널 락다운에서 설정공간 임의 쓰기는 무결성 우회 수단이므로 가장 먼저 막는다 */
	if (ret) /* [한국어] 락다운에 걸렸다면 */
		return ret; /* [한국어] 하드웨어를 건드리기 전에 즉시 반환 */

	if (resource_is_exclusive(&dev->driver_exclusive_resource, off, /* [한국어] 드라이버가 독점 예약해 둔 설정공간 구간과 겹치는지 확인 */
				  count)) { /* [한국어] 겹친다면 */
		pci_warn_once(dev, "%s: Unexpected write to kernel-exclusive config offset %llx", /* [한국어] 막지는 않되 한 번만 경고. _once 라 같은 경고로 로그가 넘치지 않는다 */
			      current->comm, off); /* [한국어] 어떤 프로세스가 무슨 오프셋에 썼는지 남긴다. current 를 쓰기 위해 sched.h 가 필요했다 */
		add_taint(TAINT_USER, LOCKDEP_STILL_OK); /* [한국어] "사용자가 직접 하드웨어를 건드린 커널" 이라고 표시. 이후 버그 리포트의 신뢰도 판단에 쓰인다 */
	} /* [한국어] 경고 블록 종료 */

	if (off > dev->cfg_size) /* [한국어] 시작 위치가 설정공간 밖인가. 읽기와 달리 권한별 상한이 없다 */
		return 0; /* [한국어] 0 = 아무것도 쓰지 않음 */
	if (off + count > dev->cfg_size) { /* [한국어] 요청 끝이 설정공간을 넘는가 */
		size = dev->cfg_size - off; /* [한국어] 실제로 쓸 수 있는 만큼으로 줄인다 */
		count = size; /* [한국어] 사용자에게 보고할 길이도 같이 줄인다 */
	} /* [한국어] 경계 조정 종료. 읽기와 달리 else 절이 없는데, size 가 이미 count 로 시작했기 때문이다 */

	pci_config_pm_runtime_get(dev); /* [한국어] 설정공간에 실제로 쓰므로 장치를 D0 로 깨운다 */

	if ((off & 1) && size) { /* [한국어] 시작이 홀수 주소면 1바이트를 써서 정렬을 맞춘다 */
		pci_user_write_config_byte(dev, off, data[off - init_off]); /* [한국어] 버퍼의 대응 바이트를 그대로 1바이트 쓰기 */
		off++; /* [한국어] 커서 전진 */
		size--; /* [한국어] 남은 바이트 감소 */
	} /* [한국어] 1바이트 정렬 맞춤 종료 */

	if ((off & 3) && size > 2) { /* [한국어] 아직 4의 배수가 아니고 2바이트를 써도 남는 것이 있는가 */
		u16 val = data[off - init_off]; /* [한국어] 낮은 주소 바이트가 하위 8비트로 — 설정공간은 리틀엔디언 */
		val |= (u16) data[off - init_off + 1] << 8; /* [한국어] 다음 바이트를 상위 8비트로 올려 OR. 캐스팅을 먼저 해야 시프트에서 잘리지 않는다 */
		pci_user_write_config_word(dev, off, val); /* [한국어] 조립한 16비트를 2바이트 쓰기 */
		off += 2; /* [한국어] 커서 전진 */
		size -= 2; /* [한국어] 남은 바이트 감소 */
	} /* [한국어] 4바이트 정렬 맞춤 종료 */

	while (size > 3) { /* [한국어] 본체 루프 — 4바이트씩 쓸 수 있는 동안 반복 */
		u32 val = data[off - init_off]; /* [한국어] 바이트 0 -> 비트 0..7 */
		val |= (u32) data[off - init_off + 1] << 8; /* [한국어] 바이트 1 -> 비트 8..15 */
		val |= (u32) data[off - init_off + 2] << 16; /* [한국어] 바이트 2 -> 비트 16..23 */
		val |= (u32) data[off - init_off + 3] << 24; /* [한국어] 바이트 3 -> 비트 24..31 */
		pci_user_write_config_dword(dev, off, val); /* [한국어] 조립한 32비트를 4바이트 쓰기. off 는 4의 배수임이 보장된다 */
		off += 4; /* [한국어] 커서 전진 */
		size -= 4; /* [한국어] 남은 바이트 감소 */
	} /* [한국어] 본체 루프 종료. 읽기 쪽에 있던 cond_resched 가 여기 없는 이유는 이 트리의 정보만으로는 확인할 수 없다 */

	if (size >= 2) { /* [한국어] 꼬리 처리 — 2바이트 이상 남았다면 */
		u16 val = data[off - init_off]; /* [한국어] 하위 바이트 */
		val |= (u16) data[off - init_off + 1] << 8; /* [한국어] 상위 바이트 */
		pci_user_write_config_word(dev, off, val); /* [한국어] 2바이트 쓰기 */
		off += 2; /* [한국어] 커서 전진 */
		size -= 2; /* [한국어] 남은 바이트 감소 */
	} /* [한국어] 2바이트 꼬리 종료 */

	if (size) /* [한국어] 마지막 1바이트가 남았는가 */
		pci_user_write_config_byte(dev, off, data[off - init_off]); /* [한국어] 남은 1바이트 쓰기 */

	pci_config_pm_runtime_put(dev); /* [한국어] 모든 접근이 끝났으므로 PM 참조를 놓는다 */

	return count; /* [한국어] 경계 조정 단계에서 확정된 실제 전송 길이를 반환 */
} /* [한국어] 함수 본문 종료 */
/* [한국어] bin_attr_config - /sys/bus/pci/devices/<BDF>/config 파일을 만드는
 * 바이너리 속성. BIN_ATTR(이름, 모드, read, write, size) 의 다섯 인자다.
 * 모드 0644 는 "누구나 읽고 소유자만 쓴다" 는 뜻 — 읽기 쪽은 권한에 따라
 * pci_read_config() 안에서 보여 주는 범위를 스스로 줄이고, 쓰기 쪽은 루트만
 * 들어오므로 함수 안에서 별도 권한 검사를 하지 않는다.
 * 마지막 인자 size 가 0 인 것은 "크기가 장치마다 다르니 정적으로 정하지
 * 않는다" 는 뜻이고, 실제 크기는 아래 그룹의 .bin_size 콜백인
 * pci_dev_config_attr_bin_size() 가 알려 준다.
 * 일반 속성이 아니라 바이너리 속성인 이유는 (1) 내용이 텍스트가 아닌 원본
 * 바이트열이고 (2) 최대 4096바이트로 커서 페이지 하나 기준의 일반 속성
 * 규약에 어울리지 않으며 (3) pread 로 임의 오프셋만 집어 읽을 수 있어야
 * 하기 때문이다. */
static const BIN_ATTR(config, 0644, pci_read_config, pci_write_config, 0); /* [한국어] 이름/모드/읽기/쓰기/정적크기(0=동적) */

/* [한국어] pci_dev_config_attrs - config 파일 하나를 담은 바이너리 속성 배열.
 * 일반 속성 배열과 타입이 다르다(struct bin_attribute 포인터 배열). */
static const struct bin_attribute *const pci_dev_config_attrs[] = { /* [한국어] 배열 시작 */
	&bin_attr_config, /* [한국어] 위에서 만든 config 바이너리 속성 */
	NULL, /* [한국어] 끝 센티널 */
}; /* [한국어] 배열 종료 */

/*
 * [한국어]
 * pci_dev_config_attr_bin_size - config 파일의 크기를 장치마다 알려 주는 콜백
 *
 * @kobj: 대상 kobject. 여기서 pci_dev 를 복원한다.
 * @a: 크기를 묻는 바이너리 속성(여기서는 config 하나뿐이라 쓰지 않는다).
 * @n: 그룹 안에서의 인덱스(마찬가지로 쓰지 않는다).
 * @return: 이 장치의 config 파일이 가질 크기(바이트).
 *
 * 왜 필요한가: BIN_ATTR 에 넣은 정적 크기가 0 이라, 파일 크기를 장치마다
 * 따로 알려 줘야 한다. 이 값이 ls -l 에 보이는 파일 크기가 되고, 사용자
 * 공간이 "끝까지 읽었다" 를 판단하는 기준이 된다.
 *
 * 판정: pdev->cfg_size 가 표준 크기(PCI_CFG_SPACE_SIZE, 즉 256바이트)보다
 * 크면 PCIe 확장 설정공간을 가진 장치이므로 PCI_CFG_SPACE_EXP_SIZE
 * (4096바이트)를, 아니면 표준 크기를 돌려준다. 두 상수는 drivers/pci/pci.h
 * 에 있다(그 파일에서 확인). cfg_size 를 그대로 돌려주지 않고 두 값 중
 * 하나로 정규화하는 셈이다.
 *
 * NVMe 학습 관점: NVMe SSD 는 PCIe 장치이므로 이 파일이 4096바이트로
 * 보인다. 그 안에 AER, Latency Tolerance Reporting, L1 PM Substates 같은
 * 확장 capability 들이 들어 있고, `lspci -vvv` 가 루트로 실행돼야 그것들이
 * 보이는 이유가 pci_read_config() 의 권한 검사다.
 *
 * 실행 컨텍스트: sysfs 가 파일 속성을 만들거나 stat 할 때 부른다.
 * 프로세스 문맥이며 락은 잡지 않는다.
 *
 * 호출 체인:
 *   sysfs 그룹 생성/stat -> bin_size 콜백 -> [pci_dev_config_attr_bin_size]
 */
static size_t pci_dev_config_attr_bin_size(struct kobject *kobj, /* [한국어] 대상 kobject */
					   const struct bin_attribute *a, /* [한국어] 어느 속성인지 — config 하나뿐이라 미사용 */
					   int n) /* [한국어] 그룹 내 인덱스 — 미사용 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* [한국어] kobject -> device -> pci_dev 복원 */

	if (pdev->cfg_size > PCI_CFG_SPACE_SIZE) /* [한국어] 표준 256바이트를 넘는가 = PCIe 확장 설정공간이 있는가 */
		return PCI_CFG_SPACE_EXP_SIZE; /* [한국어] 확장 크기 4096바이트로 보고한다 */
	return PCI_CFG_SPACE_SIZE; /* [한국어] 그렇지 않으면 표준 256바이트 */
} /* [한국어] 함수 본문 종료 */

/* [한국어] pci_dev_config_attr_group - config 바이너리 파일을 만드는 그룹.
 * pci_dev_groups[] 에 들어가 pci_bus_type.dev_groups 로 등록되므로,
 * 모든 PCI 장치에 config 파일이 생긴다.
 * is_visible 계열 콜백이 없다 — 설정공간은 모든 PCI 장치가 갖고 있으므로
 * 가릴 이유가 없기 때문이다(rom 그룹이 is_bin_visible 을 두는 것과 대비). */
static const struct attribute_group pci_dev_config_attr_group = { /* [한국어] 구조체 초기화 시작 */
	.bin_attrs = pci_dev_config_attrs, /* [한국어] 이 그룹이 만들 바이너리 속성 목록(config 하나).
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: 드라이버 코어의 sysfs 그룹 생성 코드.
		 * 값 범위: NULL 로 끝나는 const bin_attribute 포인터 배열.
		 * 동기화: const 정적 데이터라 락 불필요.
		 * 일반 속성용 .attrs 가 아니라 .bin_attrs 임에 유의 — 두 종류는
		 * 필드가 따로이고, 한 그룹이 둘 다 가질 수도 있다. */
	.bin_size = pci_dev_config_attr_bin_size, /* [한국어] 각 바이너리 속성의 크기를 장치마다 계산해 주는 콜백.
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: sysfs 코어가 파일을 만들거나 크기를 물을 때.
		 * 값 범위: 함수 포인터(NULL 이면 BIN_ATTR 의 정적 크기를 쓴다).
		 * 동기화: 콜백 자신이 락을 잡지 않으며 pci_dev 의 불변 필드만 읽는다.
		 * 이 콜백이 있어야 PCIe 확장 장치의 config 가 4096바이트로 보인다. */
}; /* [한국어] 구조체 초기화 종료 */

/*
 * llseek operation for mmappable PCI resources.
 * May be left unused if the arch doesn't provide them.
 */
/*
 * [한국어]
 * pci_llseek_resource - mmap 가능한 PCI 자원 파일의 lseek 콜백
 *
 * @filep: 대상 파일 핸들. 현재 위치가 여기에 들어 있다.
 * @kobj: 이 속성이 붙은 kobject. __always_unused 로 표시되어 있듯 쓰지
 *   않는다 — 크기는 attr 에서 얻으므로 장치를 알 필요가 없다.
 * @attr: 이 파일에 대응하는 바이너리 속성. attr->size 가 이 파일의 크기다.
 * @offset: lseek(2) 가 넘긴 오프셋.
 * @whence: SEEK_SET/SEEK_CUR/SEEK_END 중 하나.
 * @return: 새 파일 위치, 또는 음수 오류.
 *
 * 왜 필요한가: 바이너리 속성은 기본적으로 크기를 모르는 스트림처럼 다뤄져
 * SEEK_END 가 동작하지 않는다. 그런데 BAR 나 레거시 창은 크기가 분명히
 * 정해져 있고, 사용자 공간이 "끝에서 얼마 전" 같은 위치 지정을 하고 싶어
 * 한다. fixed_size_llseek() 에 attr->size 를 넘겨, 크기가 고정된 파일처럼
 * 정상적인 seek 의미를 갖게 해 준다.
 *
 * 상류 영어 주석의 뜻: 이 함수는 mmap 이 가능한 자원 파일에만 쓰이므로,
 * 아키텍처가 PCI mmap 을 제공하지 않으면 아무 데서도 참조되지 않을 수 있다.
 * 그때 "정의했는데 안 쓴다" 는 컴파일 경고가 나므로 __maybe_unused 를 붙였다.
 * 이 함수를 실제로 대입하는 곳은 pci_create_legacy_files()(legacy_io,
 * legacy_mem)와 pci_create_attr()(resourceN, resourceN_wc)이다.
 *
 * 실행 컨텍스트: lseek(2) 프로세스 문맥. 하드웨어 접근이 없고 락도 없다.
 *
 * 호출 체인:
 *   lseek(2) -> sysfs bin_attr llseek -> [pci_llseek_resource]
 *     -> fixed_size_llseek
 */
static __maybe_unused loff_t /* [한국어] 아키텍처에 따라 아무도 참조하지 않을 수 있어 미사용 경고를 막는다 */
pci_llseek_resource(struct file *filep, /* [한국어] 현재 파일 위치를 담고 있는 핸들 */
		    struct kobject *kobj __always_unused, /* [한국어] 콜백 규약상 받지만 쓰지 않는다 — 크기는 attr 에서 온다 */
		    const struct bin_attribute *attr, /* [한국어] 이 파일의 바이너리 속성. attr->size 가 파일 크기 */
		    loff_t offset, int whence) /* [한국어] lseek(2) 인자 그대로 */
{ /* [한국어] 함수 본문 시작 */
	return fixed_size_llseek(filep, offset, whence, attr->size); /* [한국어] 크기가 고정된 파일의 seek 의미를 구현해 준다. SEEK_END 가 제대로 동작하게 되는 핵심 */
} /* [한국어] 함수 본문 종료 */

/* [한국어] --- 레거시 ISA 창(legacy_io / legacy_mem) 구역 ---
 * HAVE_PCI_LEGACY 는 "이 아키텍처가 버스별 레거시 I/O 포트 공간과 ISA 메모리
 * 공간 접근을 제공한다" 는 표시다. 이 트리에서는 drivers/pci/pci.h 에서만
 * 참조되며, 실제로 정의하는 아키텍처 헤더는 arch/ 디렉터리가 없어 확인할 수
 * 없다. 정의되지 않은 빌드(x86_64 등 흔한 경우)에서는 아래 코드가 통째로
 * 컴파일에서 빠지고 legacy_io/legacy_mem 파일도 생기지 않는다.
 *
 * 무엇을 위한 것인가: PCI 이전 시대의 ISA 장치는 고정된 I/O 포트 번호와
 * 물리 메모리 첫 1MB 대역을 썼다. PCI 브리지 뒤에 그 옛 주소 공간을 그대로
 * 흉내 내는 하드웨어가 있는 플랫폼에서, 사용자 공간이 버스 단위로 그
 * 공간에 접근할 수 있게 열어 주는 창이다. NVMe 와는 아무 관계가 없다 —
 * NVMe 는 MMIO(BAR0)만 쓰며 I/O 포트를 쓰지 않는다.
 *
 * 이 구역의 파일은 장치가 아니라 struct pci_bus 에 붙는다. 그래서 아래
 * 콜백들이 to_pci_dev() 가 아니라 to_pci_bus() 를 쓴다. */
#ifdef HAVE_PCI_LEGACY /* [한국어] 아키텍처가 레거시 창을 제공할 때만 이 구역 전체를 컴파일한다 */
/**
 * pci_read_legacy_io - read byte(s) from legacy I/O port space
 * @filp: open sysfs file
 * @kobj: kobject corresponding to file to read from
 * @bin_attr: struct bin_attribute for this file
 * @buf: buffer to store results
 * @off: offset into legacy I/O port space
 * @count: number of bytes to read
 *
 * Reads 1, 2, or 4 bytes from legacy I/O port space using an arch specific
 * callback routine (pci_legacy_read).
 */
/*
 * [한국어]
 * pci_read_legacy_io - /sys/class/pci_bus/<도메인:버스>/legacy_io 읽기 콜백
 *
 * @filp: 파일 핸들(이 함수에서는 쓰지 않는다).
 * @kobj: 이 속성이 붙은 kobject. 여기서 pci_bus 를 복원한다.
 * @bin_attr: 바이너리 속성 서술자(미사용).
 * @buf: 결과를 담을 커널 버퍼.
 * @off: 레거시 I/O 포트 공간에서의 오프셋 = 사실상 포트 번호.
 * @count: 읽을 바이트 수. 1, 2, 4 만 허용된다.
 * @return: 읽은 바이트 수, 또는 음수 오류.
 *
 * 이 파일을 pread(2) 로 읽으면, 그 오프셋에 해당하는 레거시 I/O 포트에서
 * 값을 읽어 온다. 파일 크기는 0xffff 로 설정되어(pci_create_legacy_files
 * 참고) 16비트 I/O 포트 주소 공간 전체를 덮는다.
 *
 * 왜 1/2/4 바이트만 되는가: I/O 포트 접근 명령(x86 의 inb/inw/inl 등)이
 * 그 세 폭만 지원하기 때문이다. 그 외 길이는 하드웨어 접근으로 옮길 방법이
 * 없으므로 -EINVAL 로 거절한다. buf 를 (u32 *) 로 캐스팅해 넘기는 것도
 * 하위 계층이 최대 4바이트를 한 워드로 다루기 때문이다.
 *
 * 실제 포트 접근은 아키텍처가 제공하는 pci_legacy_read() 가 한다.
 * 이 트리에는 그 구현이 없다(arch/ 가 없다).
 *
 * 실행 컨텍스트: read/pread(2) 프로세스 문맥.
 *
 * 호출 체인:
 *   pread(2) -> sysfs bin_attr read -> [pci_read_legacy_io] -> pci_legacy_read
 */
static ssize_t pci_read_legacy_io(struct file *filp, struct kobject *kobj, /* [한국어] 파일 핸들(미사용)과 이 속성이 붙은 kobject */
				  const struct bin_attribute *bin_attr, /* [한국어] 바이너리 속성 서술자 — 미사용 */
				  char *buf, loff_t off, size_t count) /* [한국어] 결과 버퍼, 포트 오프셋, 요청 폭 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_bus *bus = to_pci_bus(kobj_to_dev(kobj)); /* [한국어] 이 파일은 장치가 아니라 버스에 붙으므로 pci_bus 로 복원한다 */

	/* Only support 1, 2 or 4 byte accesses */
	/* [한국어] 위 영어 주석대로 폭이 1/2/4 가 아니면 거절한다. I/O 포트
	 * 명령 자체가 그 세 폭만 지원하기 때문이다. */
	if (count != 1 && count != 2 && count != 4) /* [한국어] 허용되지 않는 접근 폭인가 */
		return -EINVAL; /* [한국어] 하드웨어로 옮길 방법이 없으므로 거절 */

	return pci_legacy_read(bus, off, (u32 *)buf, count); /* [한국어] 아키텍처가 제공하는 실제 포트 읽기. 최대 4바이트라 u32 포인터로 넘긴다 */
} /* [한국어] 함수 본문 종료 */

/**
 * pci_write_legacy_io - write byte(s) to legacy I/O port space
 * @filp: open sysfs file
 * @kobj: kobject corresponding to file to read from
 * @bin_attr: struct bin_attribute for this file
 * @buf: buffer containing value to be written
 * @off: offset into legacy I/O port space
 * @count: number of bytes to write
 *
 * Writes 1, 2, or 4 bytes from legacy I/O port space using an arch specific
 * callback routine (pci_legacy_write).
 */
/*
 * [한국어]
 * pci_write_legacy_io - /sys/class/pci_bus/<도메인:버스>/legacy_io 쓰기 콜백
 *
 * @filp: 파일 핸들(미사용).
 * @kobj: 이 속성이 붙은 kobject. pci_bus 로 복원한다.
 * @bin_attr: 바이너리 속성 서술자(미사용).
 * @buf: 쓸 값이 담긴 커널 버퍼.
 * @off: 레거시 I/O 포트 공간 오프셋 = 포트 번호.
 * @count: 쓸 바이트 수. 1, 2, 4 만 허용.
 * @return: 쓴 바이트 수, 또는 음수 오류.
 *
 * 위 읽기 함수의 거울상이다. 다른 점은 buf 를 포인터로 넘기지 않고
 * *(u32 *)buf 로 값을 꺼내 넘긴다는 것 — 쓰기는 결과를 돌려받을 곳이
 * 필요 없으므로 값 전달로 충분하다.
 *
 * 주의: 여기에는 security_locked_down() 검사가 없다. 설정공간 쓰기
 * (pci_write_config)나 BAR mmap(pci_mmap_resource)과 달리 락다운 검사가
 * 빠져 있는 이유는 이 트리의 정보만으로는 확인할 수 없다. 다만 파일 모드가
 * 0600 이라 루트만 열 수 있다는 점은 pci_create_legacy_files() 에서 확인된다.
 *
 * 실행 컨텍스트: write/pwrite(2) 프로세스 문맥.
 *
 * 호출 체인:
 *   pwrite(2) -> sysfs bin_attr write -> [pci_write_legacy_io] -> pci_legacy_write
 */
static ssize_t pci_write_legacy_io(struct file *filp, struct kobject *kobj, /* [한국어] 파일 핸들(미사용)과 이 속성이 붙은 kobject */
				   const struct bin_attribute *bin_attr, /* [한국어] 바이너리 속성 서술자 — 미사용 */
				   char *buf, loff_t off, size_t count) /* [한국어] 쓸 값이 담긴 버퍼, 포트 오프셋, 폭 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_bus *bus = to_pci_bus(kobj_to_dev(kobj)); /* [한국어] 버스 단위 속성이므로 pci_bus 로 복원 */

	/* Only support 1, 2 or 4 byte accesses */
	/* [한국어] 읽기와 같은 이유 — I/O 포트 명령이 지원하는 폭만 허용한다. */
	if (count != 1 && count != 2 && count != 4) /* [한국어] 허용되지 않는 접근 폭인가 */
		return -EINVAL; /* [한국어] 거절 */

	return pci_legacy_write(bus, off, *(u32 *)buf, count); /* [한국어] 버퍼에서 최대 4바이트를 값으로 꺼내 아키텍처 구현에 넘긴다 */
} /* [한국어] 함수 본문 종료 */

/**
 * pci_mmap_legacy_mem - map legacy PCI memory into user memory space
 * @filp: open sysfs file
 * @kobj: kobject corresponding to device to be mapped
 * @attr: struct bin_attribute for this file
 * @vma: struct vm_area_struct passed to mmap
 *
 * Uses an arch specific callback, pci_mmap_legacy_mem_page_range, to mmap
 * legacy memory space (first meg of bus space) into application virtual
 * memory space.
 */
/*
 * [한국어]
 * pci_mmap_legacy_mem - legacy_mem 파일의 mmap 콜백
 *
 * @filp: 파일 핸들(미사용).
 * @kobj: 이 속성이 붙은 kobject. pci_bus 로 복원한다.
 * @attr: 바이너리 속성 서술자(미사용).
 * @vma: mmap(2) 이 준비한 사용자 가상 메모리 영역 서술자. 여기에 물리
 *   페이지를 연결하는 것이 이 콜백의 일이다.
 * @return: 0 이면 성공, 음수면 오류.
 *
 * 이 파일을 mmap 하면 버스 공간의 첫 1MB(옛 ISA 메모리 영역)가 프로세스
 * 주소 공간에 직접 매핑된다. read/write 콜백이 없고 mmap 만 있는 이유가
 * 여기 있다 — 이 영역은 프레임버퍼처럼 통째로 매핑해 쓰는 것이 자연스럽고,
 * 바이트 단위 read 로 접근하는 용도가 아니다.
 * 실제 페이지 연결은 아키텍처가 제공하는 pci_mmap_legacy_page_range() 가
 * 한다. 두 번째 인자 pci_mmap_mem 이 "메모리 공간" 임을 알린다.
 *
 * 실행 컨텍스트: mmap(2) 프로세스 문맥. vma 를 다루므로 mm 락 문맥 안이다.
 *
 * 호출 체인:
 *   mmap(2) -> sysfs bin_attr mmap -> [pci_mmap_legacy_mem]
 *     -> pci_mmap_legacy_page_range(아키텍처 구현)
 */
static int pci_mmap_legacy_mem(struct file *filp, struct kobject *kobj, /* [한국어] 파일 핸들(미사용)과 이 속성이 붙은 kobject */
			       const struct bin_attribute *attr, /* [한국어] 바이너리 속성 서술자 — 미사용 */
			       struct vm_area_struct *vma) /* [한국어] 채워 넣을 사용자 가상 메모리 영역 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_bus *bus = to_pci_bus(kobj_to_dev(kobj)); /* [한국어] 버스 단위 속성이므로 pci_bus 로 복원 */

	return pci_mmap_legacy_page_range(bus, vma, pci_mmap_mem); /* [한국어] 아키텍처 구현에 위임. pci_mmap_mem 은 "메모리 공간" 을 뜻하는 열거값 */
} /* [한국어] 함수 본문 종료 */

/**
 * pci_mmap_legacy_io - map legacy PCI IO into user memory space
 * @filp: open sysfs file
 * @kobj: kobject corresponding to device to be mapped
 * @attr: struct bin_attribute for this file
 * @vma: struct vm_area_struct passed to mmap
 *
 * Uses an arch specific callback, pci_mmap_legacy_io_page_range, to mmap
 * legacy IO space (first meg of bus space) into application virtual
 * memory space. Returns -ENOSYS if the operation isn't supported
 */
/*
 * [한국어]
 * pci_mmap_legacy_io - legacy_io 파일의 mmap 콜백
 *
 * @filp: 파일 핸들(미사용).
 * @kobj: 이 속성이 붙은 kobject. pci_bus 로 복원한다.
 * @attr: 바이너리 속성 서술자(미사용).
 * @vma: mmap(2) 이 준비한 사용자 가상 메모리 영역.
 * @return: 0 이면 성공, 음수면 오류. 상류 주석이 밝히듯 아키텍처가 이
 *   동작을 지원하지 않으면 -ENOSYS 가 나온다.
 *
 * 위 pci_mmap_legacy_mem 과 거의 같고, 마지막 인자만 pci_mmap_io 로 달라
 * "I/O 포트 공간" 을 매핑한다는 뜻이 된다. I/O 포트를 메모리에 매핑한다는
 * 것이 이상해 보이지만, 일부 아키텍처(x86 이 아닌)는 I/O 포트 공간을
 * 물리 주소 창으로 노출하므로 매핑이 성립한다. 그렇지 않은 아키텍처에서는
 * -ENOSYS 로 거절된다.
 *
 * legacy_io 에는 read/write 콜백도 있고 mmap 도 있다는 점이 legacy_mem 과
 * 다르다 — 포트 접근은 바이트 단위 read/write 가 자연스러운 반면, 큰 창을
 * 통째로 매핑하고 싶은 경우도 있어 둘 다 제공한다.
 *
 * 실행 컨텍스트: mmap(2) 프로세스 문맥.
 *
 * 호출 체인:
 *   mmap(2) -> sysfs bin_attr mmap -> [pci_mmap_legacy_io]
 *     -> pci_mmap_legacy_page_range(아키텍처 구현)
 */
static int pci_mmap_legacy_io(struct file *filp, struct kobject *kobj, /* [한국어] 파일 핸들(미사용)과 이 속성이 붙은 kobject */
			      const struct bin_attribute *attr, /* [한국어] 바이너리 속성 서술자 — 미사용 */
			      struct vm_area_struct *vma) /* [한국어] 채워 넣을 사용자 가상 메모리 영역 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_bus *bus = to_pci_bus(kobj_to_dev(kobj)); /* [한국어] 버스 단위 속성이므로 pci_bus 로 복원 */

	return pci_mmap_legacy_page_range(bus, vma, pci_mmap_io); /* [한국어] pci_mmap_io 는 "I/O 공간" 을 뜻하는 열거값. 지원하지 않는 아키텍처면 -ENOSYS */
} /* [한국어] 함수 본문 종료 */

/**
 * pci_adjust_legacy_attr - adjustment of legacy file attributes
 * @b: bus to create files under
 * @mmap_type: I/O port or memory
 *
 * Stub implementation. Can be overridden by arch if necessary.
 */
/*
 * [한국어]
 * pci_adjust_legacy_attr - 레거시 속성을 아키텍처가 손볼 수 있게 하는 훅
 *
 * @b: 파일을 만들 대상 버스.
 * @mmap_type: 지금 조정하려는 것이 I/O 공간인지 메모리 공간인지
 *   (pci_mmap_io / pci_mmap_mem).
 * @return: 없음.
 *
 * __weak 로 선언된 빈 함수다. __weak 은 "같은 이름의 강한(strong) 정의가
 * 링크 시점에 있으면 그쪽이 이긴다" 는 뜻이라, 아키텍처가 필요하면 자기
 * 버전을 제공해 이 기본 구현을 대체할 수 있다. 예를 들어 버스마다 레거시
 * 창의 크기가 다른 플랫폼은 여기서 b->legacy_io->size 를 고쳐 놓을 수 있다.
 *
 * 이 트리에는 arch/ 디렉터리가 없어, 실제로 이 함수를 재정의하는 아키텍처가
 * 있는지는 확인할 수 없다. drivers/ 안에는 재정의가 없다(검색으로 확인).
 * 따라서 이 트리 기준으로는 항상 아무 일도 하지 않는다.
 *
 * 부르는 곳은 아래 pci_create_legacy_files() 두 군데 — legacy_io 를 만든
 * 직후와 legacy_mem 을 만든 직후다. 즉 속성 필드를 다 채운 뒤,
 * device_create_bin_file() 로 파일을 실제로 만들기 직전에 끼어든다.
 *
 * 실행 컨텍스트: 버스 등록 경로(pci_create_legacy_files)의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_create_legacy_files -> [pci_adjust_legacy_attr] (아키텍처 재정의 가능)
 */
void __weak pci_adjust_legacy_attr(struct pci_bus *b, /* [한국어] 대상 버스 */
				   enum pci_mmap_state mmap_type) /* [한국어] I/O 공간인지 메모리 공간인지 구분 */
{ /* [한국어] 함수 본문 시작 */
} /* [한국어] 기본 구현은 아무것도 하지 않는다. 아키텍처가 필요하면 __weak 을 덮어써 자기 구현을 넣는다 */

/**
 * pci_create_legacy_files - create legacy I/O port and memory files
 * @b: bus to create files under
 *
 * Some platforms allow access to legacy I/O port and ISA memory space on
 * a per-bus basis.  This routine creates the files and ties them into
 * their associated read, write and mmap files from pci-sysfs.c
 *
 * On error unwind, but don't propagate the error to the caller
 * as it is ok to set up the PCI bus without these files.
 */
/*
 * [한국어]
 * pci_create_legacy_files - 버스 하나에 legacy_io, legacy_mem 파일을 만든다
 *
 * @b: 파일을 만들 대상 PCI 버스.
 * @return: 없음. 상류 주석이 밝히듯 실패해도 호출자에게 전파하지 않는다 —
 *   이 파일들이 없어도 버스는 정상 동작하기 때문이다. 실패는 경고 한 줄로만
 *   남긴다.
 *
 * 왜 그룹이 아니라 손으로 만드는가: 이 파일들은 pci_bus 안에 포인터로
 * 매달아 두어야 하고(b->legacy_io, b->legacy_mem), 크기와 이름을 실행 중에
 * 채워야 하며, 아키텍처 훅으로 손볼 여지도 남겨야 한다. attribute_group 은
 * 정적 배열을 전제하므로 이런 동적 구성에 맞지 않는다.
 *
 * 동작 단계:
 *  1) sysfs_initialized 가 0 이면 아직 파일을 만들 수 없으므로 조용히 물러난다.
 *     그 경우 나중에 pci_sysfs_init() 이 모든 버스를 훑으며 다시 부른다.
 *  2) bin_attribute 두 개를 한 번에 할당한다. 하나만 kfree 하면 되도록
 *     연속 할당한 것이고, 그래서 아래에서 legacy_mem 을 legacy_io + 1 로
 *     구한다. GFP_ATOMIC 인 이유는 이 경로가 잠들 수 없는 문맥에서도
 *     불릴 수 있기 때문이다(hotplug 경로에서 스핀락을 쥔 채 들어올 수 있다).
 *  3) sysfs_bin_attr_init() 으로 lockdep 키를 초기화한다. 정적으로 선언된
 *     속성과 달리 동적 할당한 속성은 이 초기화가 없으면 lockdep 이 모든
 *     인스턴스를 같은 락으로 오인한다.
 *  4) 이름, 크기, 모드, 콜백들을 채운다. legacy_io 는 0xffff 바이트
 *     (16비트 포트 주소 공간 전체), legacy_mem 은 1MB(옛 ISA 메모리 창)다.
 *     둘 다 모드 0600 이라 루트만 열 수 있다.
 *  5) 아키텍처 훅을 부른 뒤 device_create_bin_file() 로 실제 파일을 만든다.
 *
 * 에러 경로(goto 레이블 세 개)는 거꾸로 풀어 나가는 표준적인 형태다.
 *  - legacy_mem 생성 실패: 이미 만든 legacy_io 파일을 지우고 이어서 아래로.
 *  - legacy_io 생성 실패: 할당한 메모리를 해제하고 포인터를 NULL 로 되돌린다.
 *    NULL 로 되돌리는 것이 중요하다 — pci_remove_legacy_files() 가 이
 *    포인터로 "만들어졌는지" 를 판단하기 때문이다.
 *  - 할당 실패: 곧바로 경고만 찍는다.
 * 세 경로가 모두 마지막 dev_warn 으로 흘러 들어가므로 경고 문구가 한 벌이다.
 *
 * 호출자: drivers/pci/probe.c 두 군데(루트 버스를 등록할 때와 새 하위 버스를
 * 만들 때)와, 아래 pci_sysfs_init() 의 뒤늦은 일괄 생성 루프.
 *
 * 실행 컨텍스트: 버스 등록 경로. GFP_ATOMIC 을 쓰는 것으로 보아 잠들 수 없는
 * 문맥에서도 불릴 수 있음을 전제한다.
 *
 * 호출 체인:
 *   probe.c 의 버스 등록 / pci_sysfs_init -> [pci_create_legacy_files]
 *     -> kzalloc_objs -> pci_adjust_legacy_attr -> device_create_bin_file
 */
void pci_create_legacy_files(struct pci_bus *b) /* [한국어] 이 버스에 legacy_io/legacy_mem 두 파일을 만든다 */
{ /* [한국어] 함수 본문 시작 */
	int error; /* [한국어] device_create_bin_file 의 결과. 0 이 성공 */

	if (!sysfs_initialized) /* [한국어] pci_sysfs_init() 이 아직 실행되지 않았는가 */
		return; /* [한국어] 지금은 만들 수 없다. 나중에 pci_sysfs_init() 이 모든 버스를 훑으며 다시 부른다 */

	b->legacy_io = kzalloc_objs(struct bin_attribute, 2, GFP_ATOMIC); /* [한국어] 두 속성을 연속으로 한 번에 할당 — 해제도 한 번이면 된다. GFP_ATOMIC 은 잠들 수 없는 문맥을 대비한 것 */
	if (!b->legacy_io) /* [한국어] 할당 실패 */
		goto kzalloc_err; /* [한국어] 되돌릴 것이 없으므로 경고만 찍는 레이블로 */

	sysfs_bin_attr_init(b->legacy_io); /* [한국어] 동적 할당한 속성에는 lockdep 키 초기화가 필수다. 빼먹으면 모든 인스턴스가 같은 락으로 오인된다 */
	b->legacy_io->attr.name = "legacy_io"; /* [한국어] 이 이름이 그대로 sysfs 파일 이름이 된다 */
	b->legacy_io->size = 0xffff; /* [한국어] 16비트 I/O 포트 주소 공간 전체 크기. llseek 의 SEEK_END 기준이기도 하다 */
	b->legacy_io->attr.mode = 0600; /* [한국어] 소유자(루트)만 읽고 쓸 수 있다. 임의 포트 접근은 위험하므로 좁힌다 */
	b->legacy_io->read = pci_read_legacy_io; /* [한국어] 1/2/4바이트 포트 읽기 콜백 */
	b->legacy_io->write = pci_write_legacy_io; /* [한국어] 1/2/4바이트 포트 쓰기 콜백 */
	/* See pci_create_attr() for motivation */
	b->legacy_io->llseek = pci_llseek_resource; /* [한국어] 영어 주석이 가리키는 대로, 이유는 pci_create_attr() 쪽 설명 참고. 크기가 정해진 파일의 seek 의미를 갖게 한다 */
	b->legacy_io->mmap = pci_mmap_legacy_io; /* [한국어] 창 전체를 통째로 매핑하고 싶을 때의 경로 */
	b->legacy_io->f_mapping = iomem_get_mapping; /* [한국어] 이 파일의 매핑을 iomem 주소 공간에 연결한다. /dev/mem 과 같은 주소 공간을 공유해, 장치가 사라질 때 매핑을 일괄 무효화할 수 있게 된다 */
	pci_adjust_legacy_attr(b, pci_mmap_io); /* [한국어] 파일을 만들기 직전에 아키텍처가 값을 손볼 기회를 준다(기본 구현은 no-op) */
	error = device_create_bin_file(&b->dev, b->legacy_io); /* [한국어] 실제로 sysfs 에 파일을 만든다 */
	if (error) /* [한국어] 생성 실패 */
		goto legacy_io_err; /* [한국어] 할당만 되돌리면 된다 */

	/* Allocated above after the legacy_io struct */
	b->legacy_mem = b->legacy_io + 1; /* [한국어] 위에서 두 개를 연속 할당했으므로 두 번째 원소가 여기 있다. 별도 할당이 아니라 포인터 산술이라는 점이 뒤의 해제 로직과 맞물린다 */
	sysfs_bin_attr_init(b->legacy_mem); /* [한국어] 두 번째 속성도 lockdep 키를 따로 초기화한다 */
	b->legacy_mem->attr.name = "legacy_mem"; /* [한국어] sysfs 파일 이름 */
	b->legacy_mem->size = 1024*1024; /* [한국어] 옛 ISA 메모리 창 1MB. 상수를 곱셈으로 써서 "1MB" 임을 눈에 보이게 했다 */
	b->legacy_mem->attr.mode = 0600; /* [한국어] 루트만 접근 */
	b->legacy_mem->mmap = pci_mmap_legacy_mem; /* [한국어] 메모리 창은 mmap 만 제공한다 — read/write 콜백이 없다 */
	/* See pci_create_attr() for motivation */
	b->legacy_mem->llseek = pci_llseek_resource; /* [한국어] 크기가 정해진 파일의 seek 의미 */
	b->legacy_mem->f_mapping = iomem_get_mapping; /* [한국어] iomem 주소 공간에 연결해 일괄 무효화가 가능하게 한다 */
	pci_adjust_legacy_attr(b, pci_mmap_mem); /* [한국어] 메모리 공간 쪽으로 아키텍처 훅 호출 */
	error = device_create_bin_file(&b->dev, b->legacy_mem); /* [한국어] 두 번째 파일 생성 */
	if (error) /* [한국어] 생성 실패 */
		goto legacy_mem_err; /* [한국어] 이미 만든 첫 파일까지 되돌려야 한다 */

	return; /* [한국어] 두 파일 모두 성공 — 정상 종료 */

legacy_mem_err: /* [한국어] legacy_mem 생성 실패: 앞서 만든 legacy_io 부터 되돌린다 */
	device_remove_bin_file(&b->dev, b->legacy_io); /* [한국어] 만들어 둔 첫 파일 제거. 이어서 아래로 흘러 메모리도 해제한다 */
legacy_io_err: /* [한국어] legacy_io 생성 실패: 파일은 없고 메모리만 있다 */
	kfree(b->legacy_io); /* [한국어] 두 속성을 한 번에 할당했으므로 한 번의 kfree 로 충분하다 */
	b->legacy_io = NULL; /* [한국어] 반드시 NULL 로. pci_remove_legacy_files() 가 이 포인터로 "만들어졌는지" 를 판단하므로, 남겨 두면 해제된 메모리를 다시 건드린다 */
kzalloc_err: /* [한국어] 할당 자체가 실패: 되돌릴 것이 없다 */
	dev_warn(&b->dev, "could not create legacy I/O port and ISA memory resources in sysfs\n"); /* [한국어] 세 경로가 모두 여기로 모인다. 실패를 호출자에게 전파하지 않고 경고만 남긴다 — 이 파일이 없어도 버스는 동작한다 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * pci_remove_legacy_files - 버스의 legacy_io, legacy_mem 파일을 제거한다
 *
 * @b: 대상 PCI 버스.
 * @return: 없음.
 *
 * pci_create_legacy_files() 의 짝이다. drivers/pci/remove.c 가 버스를 없앨 때
 * 부른다(그 파일에서 확인).
 *
 * b->legacy_io 가 NULL 인지로 "만들어진 적이 있는가" 를 판단한다. 생성
 * 실패 경로가 이 포인터를 NULL 로 되돌려 두기 때문에 이 판단이 성립한다.
 * legacy_mem 을 따로 검사하지 않는 이유는 둘이 항상 함께 만들어지고 함께
 * 사라지기 때문이다.
 *
 * 마지막 kfree 가 하나뿐인 이유는 상류 주석 "both are allocated here" 가
 * 밝힌 대로, 두 속성이 하나의 연속된 할당이기 때문이다. legacy_mem 을
 * 따로 kfree 하면 할당 블록 중간을 해제하는 셈이 되어 커널이 깨진다.
 *
 * 실행 컨텍스트: 버스 제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/pci/remove.c -> [pci_remove_legacy_files] -> device_remove_bin_file
 */
void pci_remove_legacy_files(struct pci_bus *b) /* [한국어] 이 버스의 레거시 파일 두 개를 없앤다 */
{ /* [한국어] 함수 본문 시작 */
	if (b->legacy_io) { /* [한국어] NULL 이 아니면 생성에 성공한 적이 있다는 뜻. 생성 실패 경로가 NULL 로 되돌려 두었다 */
		device_remove_bin_file(&b->dev, b->legacy_io); /* [한국어] legacy_io 파일 제거 */
		device_remove_bin_file(&b->dev, b->legacy_mem); /* [한국어] legacy_mem 파일 제거. 둘은 항상 함께 존재하므로 따로 검사하지 않는다 */
		kfree(b->legacy_io); /* both are allocated here */
		/* [한국어] 위 영어 주석대로 두 속성이 한 번의 할당으로 만들어졌으므로
		 * 해제도 한 번이면 된다. legacy_mem 을 따로 kfree 하면 할당 블록
		 * 중간을 해제하는 것이 되어 힙이 망가진다. */
	} /* [한국어] 제거 블록 종료 */
} /* [한국어] 함수 본문 종료 */
#endif /* HAVE_PCI_LEGACY */

/* [한국어] --- BAR 파일(resourceN, resourceN_wc) 구역 ---
 * 이 구역 전체는 아키텍처가 PCI mmap 을 지원할 때만 컴파일된다.
 * HAVE_PCI_MMAP 은 아키텍처가 자기 구현을 제공한다는 표시이고,
 * ARCH_GENERIC_PCI_MMAP_RESOURCE 는 drivers/pci/mmap.c 의 일반 구현을
 * 쓰겠다는 표시다(그 파일에서 두 이름을 모두 확인했다). 둘 중 하나라도
 * 있으면 resourceN 파일이 생긴다.
 *
 * 여기서 만드는 파일들:
 *   resourceN     - BAR N 을 캐시 없이(uncached) 매핑하는 창
 *   resourceN_wc  - 같은 BAR 를 write-combining 으로 매핑하는 창
 * 사용자 공간 드라이버(DPDK, VFIO 이전 방식의 도구 등)가 장치 레지스터를
 * 직접 만질 때 쓰는 통로다.
 *
 * NVMe 학습 관점: NVMe 컨트롤러의 레지스터와 도어벨은 BAR0 안에 있다
 * (drivers/nvme/host/pci.c 의 nvme_remap_bar() 가 pci_resource_start(pdev, 0)
 * 을 ioremap 하는 것이 근거다). 그러므로 커널 드라이버를 언바인드한 뒤
 * resource0 을 mmap 하면 사용자 공간에서 같은 레지스터를 볼 수 있다.
 * 다만 nvme 드라이버 자신은 이 경로를 쓰지 않는다 — 커널 안에서는
 * ioremap 을 직접 부른다. */
#if defined(HAVE_PCI_MMAP) || defined(ARCH_GENERIC_PCI_MMAP_RESOURCE) /* [한국어] 아키텍처 구현이든 일반 구현이든 PCI mmap 이 있을 때만 */
/**
 * pci_mmap_resource - map a PCI resource into user memory space
 * @kobj: kobject for mapping
 * @attr: struct bin_attribute for the file being mapped
 * @vma: struct vm_area_struct passed into the mmap
 * @write_combine: 1 for write_combine mapping
 *
 * Use the regular PCI mapping routines to map a PCI resource into userspace.
 */
/*
 * [한국어]
 * pci_mmap_resource - resourceN / resourceN_wc 의 mmap 공통 본체
 *
 * @kobj: 이 속성이 붙은 kobject. 여기서 pci_dev 를 복원한다.
 * @attr: 매핑되는 파일의 바이너리 속성. attr->private 에 BAR 번호가
 *   정수로 들어 있다(pci_create_attr 이 넣어 둔 것).
 * @vma: mmap(2) 이 준비한 사용자 가상 메모리 영역.
 * @write_combine: 0 이면 uncached, 1 이면 write-combining 매핑.
 * @return: 0 이면 성공, 음수면 오류.
 *
 * 어떻게 BAR 번호를 아는가: 파일마다 별도 함수를 두지 않고, 속성의
 * private 필드에 BAR 번호를 숫자로 넣어 둔다. 포인터 크기 정수로 왕복
 * 캐스팅하는 관용구다. 그래서 resource0..resource5 여섯 파일이 모두 같은
 * 콜백을 공유할 수 있다.
 *
 * 세 겹의 검사:
 *  1) security_locked_down(LOCKDOWN_PCI_ACCESS) — BAR 를 사용자 공간에
 *     매핑하면 그 창으로 DMA 를 프로그램해 커널 메모리를 읽고 쓸 수 있다.
 *     락다운 커널에서는 서명 검증을 통째로 무력화하는 수단이므로 막는다.
 *  2) iomem_is_exclusive(res->start) — 이 물리 영역이 커널 드라이버에
 *     독점 예약되어 있는지 본다. 예약된 영역을 사용자에게 열어 주면
 *     커널과 사용자가 같은 레지스터를 동시에 만지게 되어 장치가 망가진다.
 *     메모리 자원일 때만 검사하는 이유는 iomem 예약 개념이 메모리 공간에만
 *     적용되기 때문이다.
 *  3) pci_mmap_fits(pdev, bar, vma, PCI_MMAP_SYSFS) — 사용자가 요청한
 *     오프셋과 길이가 이 BAR 안에 완전히 들어오는지 검사한다. 이것이 없으면
 *     BAR 를 벗어난 물리 주소를 매핑해 옆 장치의 레지스터나 시스템 메모리를
 *     열어 주게 된다. 마지막 인자 PCI_MMAP_SYSFS 는 "sysfs 경유 요청" 이라는
 *     뜻으로, /proc 경유(PCI_MMAP_PROCFS)와 오프셋 해석 규칙이 다르기 때문에
 *     구분해서 넘긴다.
 *
 * 마지막으로 자원 종류에 따라 mmap_type 을 정하고 실제 매핑은
 * pci_mmap_resource_range() 에 위임한다(drivers/pci/mmap.c 에 있다).
 *
 * 실행 컨텍스트: mmap(2) 프로세스 문맥.
 *
 * 호출 체인:
 *   mmap(2) -> pci_mmap_resource_uc / pci_mmap_resource_wc
 *     -> [pci_mmap_resource] -> pci_mmap_fits -> pci_mmap_resource_range
 */
static int pci_mmap_resource(struct kobject *kobj, const struct bin_attribute *attr, /* [한국어] 이 속성이 붙은 kobject 와 매핑 대상 속성 */
			     struct vm_area_struct *vma, int write_combine) /* [한국어] 채울 사용자 가상 영역과 캐시 정책(0=uncached, 1=write-combining) */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* [한국어] kobject -> device -> pci_dev 복원 */
	int bar = (unsigned long)attr->private; /* [한국어] private 에 숫자로 넣어 둔 BAR 번호를 꺼낸다. 포인터 크기 정수를 거쳐야 경고 없이 왕복한다 */
	enum pci_mmap_state mmap_type; /* [한국어] 아래에서 메모리 공간인지 I/O 공간인지 정한다 */
	struct resource *res = &pdev->resource[bar]; /* [한국어] 그 BAR 의 자원 서술자. 시작 주소와 플래그가 여기 있다 */
	int ret; /* [한국어] 락다운 검사 결과 */

	ret = security_locked_down(LOCKDOWN_PCI_ACCESS); /* [한국어] BAR 매핑은 임의 DMA 프로그래밍의 통로라 락다운 커널에서는 금지한다 */
	if (ret) /* [한국어] 락다운에 걸렸다면 */
		return ret; /* [한국어] 아무것도 매핑하지 않고 반환 */

	if (res->flags & IORESOURCE_MEM && iomem_is_exclusive(res->start)) /* [한국어] 메모리 자원이면서 커널이 독점 예약한 영역인가. I/O 자원에는 이 개념이 없어 검사하지 않는다 */
		return -EINVAL; /* [한국어] 커널과 사용자가 같은 레지스터를 동시에 만지는 것을 막는다 */

	if (!pci_mmap_fits(pdev, bar, vma, PCI_MMAP_SYSFS)) /* [한국어] 요청한 오프셋+길이가 이 BAR 안에 온전히 들어오는가. 이 검사가 경계를 넘는 매핑을 막는 핵심이다 */
		return -EINVAL; /* [한국어] BAR 밖을 요청하면 거절 — 옆 장치나 시스템 메모리를 열어 주게 되므로 */

	mmap_type = res->flags & IORESOURCE_MEM ? pci_mmap_mem : pci_mmap_io; /* [한국어] 자원 플래그로 메모리 공간인지 I/O 공간인지 결정. 하위 계층이 페이지 속성을 다르게 잡는다 */

	return pci_mmap_resource_range(pdev, bar, vma, mmap_type, write_combine); /* [한국어] 실제 페이지 매핑은 drivers/pci/mmap.c 에 위임 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * pci_mmap_resource_uc - resourceN 파일의 mmap 콜백 (uncached 매핑)
 *
 * @filp: 파일 핸들(콜백 규약상 받지만 아래로 넘기지 않는다).
 * @kobj: 이 속성이 붙은 kobject.
 * @attr: 매핑 대상 바이너리 속성(private 에 BAR 번호).
 * @vma: 사용자 가상 메모리 영역.
 * @return: 0 이면 성공, 음수면 오류.
 *
 * 본체는 pci_mmap_resource() 이고, 여기서는 write_combine 인자에 0 을 얹어
 * "캐시하지 않는 매핑" 을 요청한다. 장치 레지스터는 읽고 쓰는 순서와 횟수가
 * 그대로 하드웨어에 전달되어야 하므로, 쓰기를 모아 두는 write-combining 을
 * 쓰면 안 된다. 그래서 기본이 이쪽이다.
 *
 * bin_attribute 의 mmap 콜백 시그니처가 filp 를 요구하지만 아래 본체는
 * 필요로 하지 않아, 이 얇은 껍데기가 시그니처만 맞춰 준다.
 *
 * 실행 컨텍스트: mmap(2) 프로세스 문맥.
 *
 * 호출 체인:
 *   mmap(2) -> sysfs bin_attr mmap -> [pci_mmap_resource_uc]
 *     -> pci_mmap_resource(write_combine=0)
 */
static int pci_mmap_resource_uc(struct file *filp, struct kobject *kobj, /* [한국어] 파일 핸들(미사용)과 kobject */
				const struct bin_attribute *attr, /* [한국어] 매핑 대상 속성 */
				struct vm_area_struct *vma) /* [한국어] 사용자 가상 메모리 영역 */
{ /* [한국어] 함수 본문 시작 */
	return pci_mmap_resource(kobj, attr, vma, 0); /* [한국어] 0 = uncached. 레지스터 접근은 순서와 횟수가 보존돼야 하므로 기본값이다 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * pci_mmap_resource_wc - resourceN_wc 파일의 mmap 콜백 (write-combining 매핑)
 *
 * @filp: 파일 핸들(미사용).
 * @kobj: 이 속성이 붙은 kobject.
 * @attr: 매핑 대상 바이너리 속성(private 에 BAR 번호).
 * @vma: 사용자 가상 메모리 영역.
 * @return: 0 이면 성공, 음수면 오류.
 *
 * write_combine 에 1 을 얹는 것만 위와 다르다. write-combining 은 연속된
 * 쓰기를 CPU 가 모아 한 번에 내보내게 해 대역폭을 크게 높이지만, 쓰기의
 * 순서와 경계가 보장되지 않는다. 그래서 프레임버퍼처럼 "많은 데이터를
 * 흘려 넣는" 영역에만 쓸 수 있고, 레지스터에는 쓰면 안 된다.
 * 그래서 이 파일은 아무 BAR 에나 생기지 않는다 — 아래
 * pci_create_resource_files() 가 IORESOURCE_PREFETCH 플래그가 있는 BAR
 * 에만 만든다. prefetchable 이라는 것은 곧 "읽어도 부작용이 없고 쓰기를
 * 모아도 되는 영역" 이라는 하드웨어의 선언이기 때문이다.
 *
 * 실행 컨텍스트: mmap(2) 프로세스 문맥.
 *
 * 호출 체인:
 *   mmap(2) -> sysfs bin_attr mmap -> [pci_mmap_resource_wc]
 *     -> pci_mmap_resource(write_combine=1)
 */
static int pci_mmap_resource_wc(struct file *filp, struct kobject *kobj, /* [한국어] 파일 핸들(미사용)과 kobject */
				const struct bin_attribute *attr, /* [한국어] 매핑 대상 속성 */
				struct vm_area_struct *vma) /* [한국어] 사용자 가상 메모리 영역 */
{ /* [한국어] 함수 본문 시작 */
	return pci_mmap_resource(kobj, attr, vma, 1); /* [한국어] 1 = write-combining. prefetchable BAR 에만 이 파일이 만들어진다 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * pci_resource_io - I/O 공간 BAR 를 다루는 resourceN 파일의 읽기/쓰기 공통 본체
 *
 * @filp: 파일 핸들(이 함수에서는 쓰지 않는다).
 * @kobj: 이 속성이 붙은 kobject. pci_dev 로 복원한다.
 * @attr: 바이너리 속성. private 에 BAR 번호가 들어 있다.
 * @buf: 읽은 값을 담거나 쓸 값이 담긴 커널 버퍼.
 * @off: BAR 시작을 0 으로 본 상대 오프셋.
 * @count: 접근 폭. 1, 2, 4 만 유효하다.
 * @write: false 면 읽기, true 면 쓰기.
 * @return: 옮긴 바이트 수(1/2/4), 범위를 완전히 벗어나면 0,
 *   폭이 잘못됐거나 끝을 넘어가면 -EINVAL, I/O 포트 자체를 못 쓰는
 *   아키텍처면 -ENXIO.
 *
 * 왜 필요한가: 메모리 BAR 는 mmap 으로 통째로 매핑하면 되지만, I/O 공간
 * BAR 는 그럴 수 없는 아키텍처가 많다. I/O 포트는 CPU 의 별도 명령
 * (inb/outb 계열)으로만 접근되기 때문이다. 그래서 I/O BAR 에 대해서는
 * mmap 대신 read/write 콜백을 제공한다. 이 함수가 그 둘의 공통 본체다.
 *
 * 동작 단계:
 *  1) attr->private 에서 BAR 번호를 꺼내고, 그 BAR 의 시작 포트 번호에
 *     사용자가 준 오프셋을 더해 실제 포트 주소를 만든다. 즉 파일 오프셋이
 *     곧 BAR 안에서의 상대 위치다.
 *  2) 경계 검사가 두 번이다. 시작 포트가 이미 BAR 끝을 넘었으면 0(EOF),
 *     시작은 들어오는데 끝이 넘어가면 -EINVAL 이다. 두 경우를 다르게
 *     다루는 이유: 앞은 "더 읽을 것이 없다"는 정상적인 파일 끝이고,
 *     뒤는 "걸쳐 있는 잘못된 요청" 이라 오류다. `port + count - 1` 에서
 *     1 을 빼는 것은 끝 주소가 마지막 바이트를 가리키는 포함 범위이기
 *     때문이다.
 *  3) 폭에 따라 inb/inw/inl 또는 outb/outw/outl 로 실제 포트를 건드린다.
 *     이 명령들은 값이 그대로 하드웨어에 전달되며, 컴파일러나 CPU 가
 *     순서를 바꾸지 않는다.
 *  4) switch 를 빠져나오면 폭이 1/2/4 가 아니었다는 뜻이므로 -EINVAL.
 *
 * CONFIG_HAS_IOPORT 가 없는 아키텍처(I/O 포트 개념 자체가 없는 플랫폼)에서는
 * 함수 전체가 -ENXIO 한 줄로 대체된다. 그렇게 하면 호출부를 조건부로
 * 감싸지 않아도 되고, 파일은 생기되 접근하면 오류가 나는 형태가 된다.
 *
 * NVMe 학습 관점: NVMe 는 I/O 공간 BAR 를 쓰지 않는다. 컨트롤러 레지스터와
 * 도어벨은 모두 메모리 BAR0 에 있다(drivers/nvme/host/pci.c 확인). 따라서
 * NVMe SSD 에서는 이 경로가 쓰이지 않고, resource0 은 mmap 전용으로 만들어진다.
 *
 * 실행 컨텍스트: read/write(2) 프로세스 문맥. 포트 접근은 잠들지 않는다.
 *
 * 호출 체인:
 *   pci_read_resource_io / pci_write_resource_io -> [pci_resource_io]
 *     -> inb/inw/inl 또는 outb/outw/outl
 */
static ssize_t pci_resource_io(struct file *filp, struct kobject *kobj, /* [한국어] 파일 핸들(미사용)과 kobject */
			       const struct bin_attribute *attr, char *buf, /* [한국어] 속성(private=BAR 번호)과 데이터 버퍼 */
			       loff_t off, size_t count, bool write) /* [한국어] BAR 상대 오프셋, 접근 폭, 방향 */
{ /* [한국어] 함수 본문 시작 */
#ifdef CONFIG_HAS_IOPORT /* [한국어] I/O 포트 명령이 존재하는 아키텍처에서만 실제 구현을 컴파일한다 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* [한국어] kobject -> device -> pci_dev 복원 */
	int bar = (unsigned long)attr->private; /* [한국어] 속성에 숫자로 심어 둔 BAR 번호를 꺼낸다 */
	unsigned long port = off; /* [한국어] 파일 오프셋을 BAR 안의 상대 위치로 본다. 아래에서 절대 포트 번호로 바뀐다 */

	port += pci_resource_start(pdev, bar); /* [한국어] BAR 의 시작 포트 번호를 더해 실제 접근할 포트 주소를 만든다 */

	if (port > pci_resource_end(pdev, bar)) /* [한국어] 시작부터 BAR 끝을 넘었는가 */
		return 0; /* [한국어] 정상적인 파일 끝(EOF). 오류가 아니다 */

	if (port + count - 1 > pci_resource_end(pdev, bar)) /* [한국어] 시작은 안에 있는데 끝이 넘어가는가. -1 은 끝 주소가 마지막 바이트를 포함하기 때문 */
		return -EINVAL; /* [한국어] BAR 경계에 걸친 요청은 오류로 거절한다 */

	switch (count) { /* [한국어] 사용자가 쓴 바이트 수로 분기 */
	case 1: /* [한국어] 1바이트 접근 */
		if (write) /* [한국어] 방향 분기 */
			outb(*(u8 *)buf, port); /* [한국어] 버퍼의 1바이트를 포트에 내보낸다 */
		else /* [한국어] 읽기 */
			*(u8 *)buf = inb(port); /* [한국어] 포트에서 1바이트를 읽어 버퍼에 담는다 */
		return 1; /* [한국어] 옮긴 바이트 수 */
	case 2: /* [한국어] 2바이트 접근 */
		if (write) /* [한국어] 방향 분기 */
			outw(*(u16 *)buf, port); /* [한국어] 버퍼의 2바이트를 포트에 내보낸다 */
		else /* [한국어] 읽기 */
			*(u16 *)buf = inw(port); /* [한국어] 포트에서 2바이트를 읽어 버퍼에 담는다 */
		return 2; /* [한국어] 옮긴 바이트 수 */
	case 4: /* [한국어] 4바이트 접근 */
		if (write) /* [한국어] 방향 분기 */
			outl(*(u32 *)buf, port); /* [한국어] 버퍼의 4바이트를 포트에 내보낸다 */
		else /* [한국어] 읽기 */
			*(u32 *)buf = inl(port); /* [한국어] 포트에서 4바이트를 읽어 버퍼에 담는다 */
		return 4; /* [한국어] 옮긴 바이트 수 */
	} /* [한국어] switch 종료 — 여기 도달했다면 폭이 1/2/4 가 아니었다는 뜻 */
	return -EINVAL; /* [한국어] I/O 포트 명령이 지원하지 않는 폭이므로 거절 */
#else /* [한국어] I/O 포트 개념이 없는 아키텍처 */
	return -ENXIO; /* [한국어] 함수 전체를 이 한 줄로 대체해, 호출부를 조건부로 감싸지 않아도 되게 한다 */
#endif /* [한국어] CONFIG_HAS_IOPORT 분기 끝 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * pci_read_resource_io - I/O BAR 를 노출하는 resourceN 파일의 읽기 콜백
 *
 * @filp: 파일 핸들(그대로 아래로 넘긴다).
 * @kobj: 이 속성이 붙은 kobject.
 * @attr: 바이너리 속성(private 에 BAR 번호).
 * @buf: 읽은 값을 담을 커널 버퍼.
 * @off: BAR 상대 오프셋.
 * @count: 접근 폭(1/2/4).
 * @return: 읽은 바이트 수, 또는 음수 오류.
 *
 * 본체는 pci_resource_io() 이고, 여기서는 방향 인자에 false 를 얹는다.
 * 읽기에는 락다운 검사가 없다 — 포트를 읽는 것만으로는 커널 무결성을
 * 깨뜨릴 수 없기 때문이다(쓰기 쪽과 대비된다).
 *
 * 실행 컨텍스트: pread(2) 프로세스 문맥.
 *
 * 호출 체인:
 *   pread(2) -> sysfs bin_attr read -> [pci_read_resource_io]
 *     -> pci_resource_io(write=false)
 */
static ssize_t pci_read_resource_io(struct file *filp, struct kobject *kobj, /* [한국어] 파일 핸들과 kobject */
				    const struct bin_attribute *attr, char *buf, /* [한국어] 속성과 결과 버퍼 */
				    loff_t off, size_t count) /* [한국어] BAR 상대 오프셋과 접근 폭 */
{ /* [한국어] 함수 본문 시작 */
	return pci_resource_io(filp, kobj, attr, buf, off, count, false); /* [한국어] false = 읽기 방향 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * pci_write_resource_io - I/O BAR 를 노출하는 resourceN 파일의 쓰기 콜백
 *
 * @filp: 파일 핸들(그대로 아래로 넘긴다).
 * @kobj: 이 속성이 붙은 kobject.
 * @attr: 바이너리 속성(private 에 BAR 번호).
 * @buf: 쓸 값이 담긴 커널 버퍼.
 * @off: BAR 상대 오프셋.
 * @count: 접근 폭(1/2/4).
 * @return: 쓴 바이트 수, 락다운이면 그 오류, 그 밖의 오류는 본체가 정한다.
 *
 * 읽기 쪽과 달리 여기에는 security_locked_down(LOCKDOWN_PCI_ACCESS) 검사가
 * 먼저 온다. 장치의 I/O 레지스터에 임의 값을 쓰는 것은 장치를 통해 시스템을
 * 조작하는 통로가 될 수 있어, 락다운 커널에서는 금지된다. 검사를 본체가
 * 아니라 이 껍데기에 둔 이유가 그것이다 — 읽기 경로에는 걸지 않기 위해서다.
 *
 * 실행 컨텍스트: pwrite(2) 프로세스 문맥.
 *
 * 호출 체인:
 *   pwrite(2) -> sysfs bin_attr write -> [pci_write_resource_io]
 *     -> security_locked_down -> pci_resource_io(write=true)
 */
static ssize_t pci_write_resource_io(struct file *filp, struct kobject *kobj, /* [한국어] 파일 핸들과 kobject */
				     const struct bin_attribute *attr, char *buf, /* [한국어] 속성과 쓸 값이 담긴 버퍼 */
				     loff_t off, size_t count) /* [한국어] BAR 상대 오프셋과 접근 폭 */
{ /* [한국어] 함수 본문 시작 */
	int ret; /* [한국어] 락다운 검사 결과 */

	ret = security_locked_down(LOCKDOWN_PCI_ACCESS); /* [한국어] 임의 I/O 쓰기는 락다운 커널에서 금지. 읽기 경로에는 이 검사가 없다 */
	if (ret) /* [한국어] 락다운에 걸렸다면 */
		return ret; /* [한국어] 하드웨어를 건드리기 전에 반환 */

	return pci_resource_io(filp, kobj, attr, buf, off, count, true); /* [한국어] true = 쓰기 방향 */
} /* [한국어] 함수 본문 종료 */

/**
 * pci_remove_resource_files - cleanup resource files
 * @pdev: dev to cleanup
 *
 * If we created resource files for @pdev, remove them from sysfs and
 * free their resources.
 */
/*
 * [한국어]
 * pci_remove_resource_files - 이 장치에 만들어 둔 BAR 파일들을 모두 지운다
 *
 * @pdev: 정리 대상 장치.
 * @return: 없음.
 *
 * resourceN 과 resourceN_wc 파일은 정적 속성 그룹이 아니라 실행 중에 하나씩
 * 만들어졌고, 그 포인터가 pdev->res_attr[] 와 pdev->res_attr_wc[] 에 보관되어
 * 있다. 여기서는 그 배열을 훑으며 NULL 이 아닌 항목만 sysfs 에서 지우고
 * 메모리를 해제한다.
 *
 * 왜 NULL 검사가 필요한가: 모든 BAR 에 파일이 만들어지는 것이 아니다.
 * 크기가 0 인 BAR 는 건너뛰었고, write-combining 파일은 prefetchable BAR
 * 에만 만들어졌다. 그래서 배열에는 빈 칸이 섞여 있다.
 *
 * 주의: 지운 뒤 배열 원소를 NULL 로 되돌리지 않는다. 그래도 되는 이유는
 * 이 함수를 부르는 두 경로가 모두 그 뒤에 다시 만들거나(BAR 크기 변경 경로:
 * __resource_resize_store 가 지우고 곧바로 pci_create_resource_files 로 다시
 * 만든다) 장치를 통째로 없애기(pci_remove_sysfs_dev_files) 때문이다.
 * 다시 만드는 경로에서는 pci_create_attr() 이 같은 자리에 새 포인터를 덮어쓴다.
 *
 * 호출자: pci_remove_sysfs_dev_files(), pci_create_resource_files() 의 실패
 * 되감기, __resource_resize_store().
 *
 * 실행 컨텍스트: 프로세스 문맥. sysfs 파일 제거가 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_remove_sysfs_dev_files / __resource_resize_store
 *     -> [pci_remove_resource_files] -> sysfs_remove_bin_file -> kfree
 */
static void pci_remove_resource_files(struct pci_dev *pdev) /* [한국어] 이 장치의 resourceN, resourceN_wc 파일을 전부 정리한다 */
{ /* [한국어] 함수 본문 시작 */
	int i; /* [한국어] BAR 인덱스 */

	for (i = 0; i < PCI_STD_NUM_BARS; i++) { /* [한국어] 표준 BAR 0~5 만 훑는다. ROM 과 브리지 윈도우에는 이 파일들이 없다 */
		struct bin_attribute *res_attr; /* [한국어] 이번 칸의 속성 포인터를 담을 임시 변수. 두 배열에 재사용된다 */

		res_attr = pdev->res_attr[i]; /* [한국어] uncached 판(resourceN) 포인터를 꺼낸다. 만들지 않았다면 NULL */
		if (res_attr) { /* [한국어] 실제로 만들어진 경우에만 */
			sysfs_remove_bin_file(&pdev->dev.kobj, res_attr); /* [한국어] sysfs 에서 파일을 먼저 없앤다. 열려 있는 사용자가 있어도 안전하게 처리된다 */
			kfree(res_attr); /* [한국어] 속성 구조체와 그 뒤에 붙여 둔 이름 문자열을 한 번에 해제. 이름은 같은 할당 안에 있다 */
		} /* [한국어] uncached 판 정리 종료 */

		res_attr = pdev->res_attr_wc[i]; /* [한국어] write-combining 판(resourceN_wc) 포인터. prefetchable BAR 에만 존재한다 */
		if (res_attr) { /* [한국어] 실제로 만들어진 경우에만 */
			sysfs_remove_bin_file(&pdev->dev.kobj, res_attr); /* [한국어] sysfs 에서 파일 제거 */
			kfree(res_attr); /* [한국어] 구조체와 이름을 한 번에 해제 */
		} /* [한국어] write-combining 판 정리 종료 */
	} /* [한국어] BAR 루프 종료 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * pci_create_attr - BAR 하나에 대응하는 resourceN 또는 resourceN_wc 파일을 만든다
 *
 * @pdev: 대상 장치.
 * @num: BAR 번호(0~5). 파일 이름과 attr->private 에 모두 쓰인다.
 * @write_combine: 0 이면 resourceN(uncached), 1 이면 resourceN_wc.
 * @return: 0 이면 성공, 할당 실패면 -ENOMEM, sysfs 생성 실패면 그 오류.
 *
 * 왜 정적 속성이 아닌가: 파일 이름에 BAR 번호가 들어가고, 크기가 장치마다
 * 다르며, BAR 의 종류(I/O 인지 메모리인지, prefetchable 인지)에 따라 붙일
 * 콜백이 달라진다. 정적 배열로는 표현할 수 없어 실행 중에 하나씩 만든다.
 *
 * 이름 문자열을 구조체 뒤에 붙여 할당하는 기법:
 *  - name_len 이 13 또는 10 인 근거는 문자열 길이다. "resource%d_wc" 는
 *    resource(8) + 숫자(1) + _wc(3) + NUL(1) = 13, "resource%d" 는
 *    8 + 1 + NUL(1) = 10 이다. BAR 번호가 한 자리(0~5)라는 전제가 깔려 있다.
 *  - kzalloc(sizeof(*res_attr) + name_len) 로 구조체와 이름 자리를 한 번에
 *    잡고, res_attr + 1 (구조체 하나만큼 뒤)을 이름 버퍼로 쓴다. 이렇게 하면
 *    해제도 kfree 한 번으로 끝나 수명 관리가 단순해진다. 이것이
 *    pci_remove_resource_files() 가 kfree 를 한 번만 부르는 이유다.
 *  - GFP_ATOMIC 은 이 경로가 잠들 수 없는 문맥에서도 불릴 수 있음을 전제한 것이다.
 *
 * 콜백 선택 규칙:
 *  - write_combine 판이면 mmap 만 붙인다(pci_mmap_resource_wc).
 *  - 아니면서 I/O BAR 이면 read/write 를 붙이고, 아키텍처가 I/O 공간 mmap 을
 *    지원할 때만 mmap 도 붙인다(arch_can_pci_mmap_io()).
 *  - 아니면서 메모리 BAR 이면 mmap 만 붙인다. 메모리 BAR 를 바이트 단위
 *    read/write 로 노출하지 않는 것은, 그 접근이 MMIO 순서 보장을 깨뜨리기
 *    쉽고 mmap 으로 충분하기 때문이다.
 *
 * mmap 이 붙었을 때만 하는 두 가지 추가 설정:
 *  - f_mapping = iomem_get_mapping — 이 파일의 매핑을 iomem 주소 공간에
 *    소속시킨다. 장치가 사라질 때 그 주소 공간의 매핑을 일괄로 무효화할 수
 *    있게 되어, 사라진 장치의 레지스터를 계속 만지는 사고를 막는다.
 *  - llseek = pci_llseek_resource — 상류 영어 주석이 이유를 정확히 밝힌다.
 *    기본 generic_file_llseek() 은 f_mapping->host 에서 파일 크기를 얻는데,
 *    방금 f_mapping 을 iomem 쪽으로 바꿔 놓아 그 inode 는 이 속성의 크기를
 *    모른다. 그래서 llseek 도 함께 갈아 끼워야 SEEK_END 가 올바로 동작한다.
 *    즉 이 두 줄은 반드시 짝으로 다녀야 하는 설정이다.
 *
 * 마지막에 attr.mode 를 0600 으로 두어 루트만 열 수 있게 하고,
 * size 에 BAR 길이를, private 에 BAR 번호를 심는다. private 에 숫자를 넣는
 * 덕분에 여섯 개 파일이 콜백 함수를 공유할 수 있다.
 *
 * 에러 경로: sysfs_create_bin_file() 이 실패하면 방금 할당한 것을 해제하고
 * 오류를 돌려준다. 이때 pdev->res_attr[num] 에는 아무것도 넣지 않으므로,
 * 나중에 pci_remove_resource_files() 가 해제된 포인터를 건드릴 일이 없다.
 * 성공했을 때에만 배열에 기록하는 순서가 그래서 중요하다.
 *
 * 실행 컨텍스트: 장치 등록 또는 BAR 크기 변경 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_create_resource_files -> [pci_create_attr]
 *     -> kzalloc -> sysfs_bin_attr_init -> sysfs_create_bin_file
 */
static int pci_create_attr(struct pci_dev *pdev, int num, int write_combine) /* [한국어] BAR num 에 대한 파일 하나를 만든다 */
{ /* [한국어] 함수 본문 시작 */
	/* allocate attribute structure, piggyback attribute name */
	int name_len = write_combine ? 13 : 10; /* [한국어] "resource%d_wc"(8+1+3+NUL=13) 또는 "resource%d"(8+1+NUL=10). BAR 번호가 한 자리라는 전제가 깔려 있다 */
	struct bin_attribute *res_attr; /* [한국어] 만들 바이너리 속성. 아래에서 이름 자리까지 함께 할당한다 */
	char *res_attr_name; /* [한국어] 구조체 바로 뒤에 붙일 이름 버퍼를 가리킬 포인터 */
	int retval; /* [한국어] sysfs 파일 생성 결과 */

	res_attr = kzalloc(sizeof(*res_attr) + name_len, GFP_ATOMIC); /* [한국어] 구조체 + 이름 자리를 한 덩어리로 할당. kfree 한 번으로 정리되게 하려는 의도다. GFP_ATOMIC 은 잠들 수 없는 문맥 대비 */
	if (!res_attr) /* [한국어] 할당 실패 */
		return -ENOMEM; /* [한국어] 되돌릴 것이 없으므로 곧바로 반환 */

	res_attr_name = (char *)(res_attr + 1); /* [한국어] 구조체 하나만큼 뒤 = 방금 함께 잡아 둔 이름 자리. 포인터 산술이 sizeof(*res_attr) 만큼 더한다 */

	sysfs_bin_attr_init(res_attr); /* [한국어] 동적 할당 속성에 필수인 lockdep 키 초기화 */
	if (write_combine) { /* [한국어] write-combining 판을 만드는 경우 */
		sprintf(res_attr_name, "resource%d_wc", num); /* [한국어] 위에서 13바이트를 정확히 계산해 잡았으므로 오버런이 없다. 그래서 snprintf 가 아니어도 안전하다 */
		res_attr->mmap = pci_mmap_resource_wc; /* [한국어] wc 판은 mmap 만 제공한다 — 쓰기를 모으는 매핑이라 바이트 단위 접근은 의미가 없다 */
	} else { /* [한국어] 기본(uncached) 판 */
		sprintf(res_attr_name, "resource%d", num); /* [한국어] 10바이트 자리에 정확히 들어간다 */
		if (pci_resource_flags(pdev, num) & IORESOURCE_IO) { /* [한국어] 이 BAR 가 I/O 공간인가 */
			res_attr->read = pci_read_resource_io; /* [한국어] I/O 는 포트 명령으로만 접근되므로 read 콜백을 제공한다 */
			res_attr->write = pci_write_resource_io; /* [한국어] 쓰기도 마찬가지. 락다운 검사는 그 함수 안에 있다 */
			if (arch_can_pci_mmap_io()) /* [한국어] 이 아키텍처가 I/O 공간을 물리 창으로 매핑할 수 있는가 */
				res_attr->mmap = pci_mmap_resource_uc; /* [한국어] 가능하면 mmap 도 함께 제공한다 */
		} else { /* [한국어] 메모리 공간 BAR */
			res_attr->mmap = pci_mmap_resource_uc; /* [한국어] 메모리 BAR 는 mmap 만. read/write 를 두지 않는 것은 MMIO 접근 순서를 사용자에게 맡기지 않기 위해서다 */
		} /* [한국어] 자원 종류 분기 종료 */
	} /* [한국어] wc / 기본 판 분기 종료 */
	if (res_attr->mmap) { /* [한국어] mmap 을 제공하는 경우에만 아래 두 설정이 필요하다 */
		res_attr->f_mapping = iomem_get_mapping; /* [한국어] 매핑을 iomem 주소 공간에 소속시켜, 장치가 사라질 때 일괄 무효화가 가능하게 한다 */
		/*
		 * generic_file_llseek() consults f_mapping->host to determine
		 * the file size. As iomem_inode knows nothing about the
		 * attribute, it's not going to work, so override it as well.
		 */
		/* [한국어] 위 영어 주석의 뜻: 기본 llseek 은 f_mapping 의 inode 에서
		 * 파일 크기를 얻는데, 방금 f_mapping 을 iomem 쪽으로 바꿔 놓았고
		 * 그 inode 는 이 속성의 크기를 모른다. 그래서 llseek 도 반드시
		 * 함께 갈아 끼워야 한다 — 위 f_mapping 대입과 짝으로 다니는 설정이다. */
		res_attr->llseek = pci_llseek_resource; /* [한국어] attr->size 를 기준으로 seek 하는 구현으로 교체 */
	} /* [한국어] mmap 전용 설정 종료 */
	res_attr->attr.name = res_attr_name; /* [한국어] 방금 만든 이름 문자열을 속성에 연결. 같은 할당 안이라 수명이 함께 간다 */
	res_attr->attr.mode = 0600; /* [한국어] 소유자(루트)만 읽고 쓴다. BAR 직접 접근은 위험하므로 좁힌다 */
	res_attr->size = pci_resource_len(pdev, num); /* [한국어] 이 BAR 의 실제 길이 = 파일 크기. llseek 과 mmap 경계 검사의 기준이 된다 */
	res_attr->private = (void *)(unsigned long)num; /* [한국어] BAR 번호를 포인터 자리에 숫자로 심는다. 덕분에 여섯 파일이 같은 콜백을 공유할 수 있다 */
	retval = sysfs_create_bin_file(&pdev->dev.kobj, res_attr); /* [한국어] 실제로 파일을 만든다 */
	if (retval) { /* [한국어] 생성 실패 */
		kfree(res_attr); /* [한국어] 구조체와 이름을 한 번에 해제 */
		return retval; /* [한국어] 배열에 기록하기 전에 반환 — 그래야 나중에 해제된 포인터를 건드리지 않는다 */
	} /* [한국어] 실패 처리 종료 */

	if (write_combine) /* [한국어] 성공했으므로 어느 배열에 기록할지 고른다 */
		pdev->res_attr_wc[num] = res_attr; /* [한국어] write-combining 판 보관. 나중에 pci_remove_resource_files 가 이걸 보고 지운다 */
	else /* [한국어] 기본 판 */
		pdev->res_attr[num] = res_attr; /* [한국어] uncached 판 보관 */

	return 0; /* [한국어] 성공 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * pci_create_resource_files - 이 장치의 모든 BAR 에 대해 resourceN 파일을 만든다
 *
 * @pdev: 대상 장치.
 * @return: 0 이면 성공, 실패하면 그 오류(이때 이미 만든 것은 모두 정리된 상태).
 *
 * 동작:
 *  1) non_mappable_bars 가 서 있으면 아무것도 만들지 않는다. 이 플래그는
 *     "이 장치의 BAR 는 사용자 공간에 매핑하면 안 된다" 는 표시로,
 *     이 트리에서는 drivers/pci/proc.c 와 drivers/vfio/pci/vfio_pci_core.c
 *     도 같은 플래그를 본다(검색으로 확인). 대표적으로 VFIO 가 장치를
 *     가져갔을 때처럼 다른 주체가 BAR 를 독점해야 하는 상황이다.
 *  2) 표준 BAR 0~5 를 훑으며, 길이가 0 인 BAR(장치가 구현하지 않은 BAR)는
 *     건너뛴다. BAR 번호는 비어 있을 수 있어 파일 번호도 띄엄띄엄해진다.
 *  3) 각 BAR 에 기본 파일 하나를 만들고, 조건이 맞으면 wc 판도 만든다.
 *     조건은 두 가지가 모두 참일 때다 — 아키텍처가 write-combining 매핑을
 *     지원하고(arch_can_pci_mmap_wc()), 그 BAR 가 prefetchable 로 선언되어
 *     있을 것(IORESOURCE_PREFETCH). prefetchable 이 아닌 BAR 에 쓰기를
 *     모으면 레지스터 접근 순서가 깨지므로 반드시 필요한 조건이다.
 *  4) 하나라도 실패하면 지금까지 만든 것을 전부 지우고 오류를 반환한다.
 *     부분적으로만 파일이 존재하는 어중간한 상태를 남기지 않으려는 것이다.
 *
 * NVMe 학습 관점: NVMe SSD 는 대개 BAR0 하나만 구현한다. 그래서 이 루프가
 * 실질적으로 만드는 것은 resource0 하나다. NVMe 의 BAR0 는 레지스터 창이라
 * prefetchable 이 아닌 것이 보통이며, 그 경우 resource0_wc 는 생기지 않는다.
 * (다만 특정 장치가 실제로 어떤 플래그를 다는지는 하드웨어마다 다르므로,
 * 이 트리의 코드만으로 단정할 수는 없다.)
 *
 * 실행 컨텍스트: 장치 등록 또는 BAR 크기 변경 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_create_sysfs_dev_files / __resource_resize_store
 *     -> [pci_create_resource_files] -> pci_create_attr
 */
/**
 * pci_create_resource_files - create resource files in sysfs for @dev
 * @pdev: dev in question
 *
 * Walk the resources in @pdev creating files for each resource available.
 */
static int pci_create_resource_files(struct pci_dev *pdev) /* [한국어] 이 장치의 BAR 들에 대한 sysfs 파일을 일괄 생성 */
{ /* [한국어] 함수 본문 시작 */
	int i; /* [한국어] BAR 인덱스 */
	int retval; /* [한국어] 각 파일 생성 결과 */

	/* Skip devices with non-mappable BARs */
	/* [한국어] 위 영어 주석대로, BAR 를 사용자 공간에 매핑하면 안 되는
	 * 장치는 건너뛴다. 파일을 아예 만들지 않는 것이 가장 확실한 차단이다. */
	if (pdev->non_mappable_bars) /* [한국어] 이 장치의 BAR 가 매핑 금지로 표시되어 있는가 */
		return 0; /* [한국어] 오류가 아니라 "만들 것이 없음". 호출자는 성공으로 본다 */

	/* Expose the PCI resources from this device as files */
	for (i = 0; i < PCI_STD_NUM_BARS; i++) { /* [한국어] 표준 BAR 0~5 만 대상. ROM 은 별도의 rom 파일이 담당한다 */

		/* skip empty resources */
		if (!pci_resource_len(pdev, i)) /* [한국어] 길이가 0 = 장치가 구현하지 않은 BAR */
			continue; /* [한국어] 파일을 만들지 않는다. 그래서 파일 번호가 띄엄띄엄해질 수 있다 */

		retval = pci_create_attr(pdev, i, 0); /* [한국어] 기본(uncached) 판 resourceN 생성 */
		/* for prefetchable resources, create a WC mappable file */
		if (!retval && arch_can_pci_mmap_wc() && /* [한국어] 앞이 성공했고, 아키텍처가 write-combining 매핑을 지원하며 */
		    pdev->resource[i].flags & IORESOURCE_PREFETCH) /* [한국어] 그 BAR 가 prefetchable 로 선언되어 있을 때만. 레지스터 창에 쓰기를 모으면 순서가 깨지므로 필수 조건이다 */
			retval = pci_create_attr(pdev, i, 1); /* [한국어] write-combining 판 resourceN_wc 추가 생성 */
		if (retval) { /* [한국어] 둘 중 하나라도 실패했다면 */
			pci_remove_resource_files(pdev); /* [한국어] 지금까지 만든 파일을 전부 되돌린다. 반쯤 만들어진 상태를 남기지 않는다 */
			return retval; /* [한국어] 실패를 호출자에게 전달 */
		} /* [한국어] 실패 처리 종료 */
	} /* [한국어] BAR 루프 종료 */
	return 0; /* [한국어] 모든 BAR 처리 성공 */
} /* [한국어] 함수 본문 종료 */
#else /* !(defined(HAVE_PCI_MMAP) || defined(ARCH_GENERIC_PCI_MMAP_RESOURCE)) */
/* [한국어] 여기부터는 PCI mmap 을 전혀 지원하지 않는 아키텍처용 대체 구현이다.
 * 위 구역 전체가 컴파일되지 않으므로, 호출자(pci_create_sysfs_dev_files,
 * pci_remove_sysfs_dev_files, __resource_resize_store)가 쓰는 두 이름만
 * 아무 일도 하지 않는 함수로 채워 준다. 이렇게 하면 호출부를 #ifdef 로
 * 감쌀 필요가 없어 코드가 단순해진다.
 * __weak 인 이유는 아키텍처가 자기 구현을 따로 제공할 수 있게 하기 위해서다.
 * 이 트리에는 arch/ 가 없어 실제로 그런 아키텍처가 있는지는 확인할 수 없다.
 * 또한 여기서는 static 이 아니라 외부 링키지를 갖는데, 위 구역의 static
 * 판과 달리 아키텍처 코드에서 덮어쓸 수 있어야 하기 때문이다. */
int __weak pci_create_resource_files(struct pci_dev *dev) { return 0; } /* [한국어] 만들 파일이 없으므로 성공만 반환한다 */
void __weak pci_remove_resource_files(struct pci_dev *dev) { return; } /* [한국어] 지울 파일도 없으므로 그냥 돌아간다 */
#endif /* [한국어] PCI mmap 지원 여부 분기 끝 */

/**
 * pci_write_rom - used to enable access to the PCI ROM display
 * @filp: sysfs file
 * @kobj: kernel object handle
 * @bin_attr: struct bin_attribute for this file
 * @buf: user input
 * @off: file offset
 * @count: number of byte in input
 *
 * writing anything except 0 enables it
 */
/*
 * [한국어]
 * pci_write_rom - /sys/bus/pci/devices/<BDF>/rom 쓰기 콜백 (읽기 허용 스위치)
 *
 * @filp: 파일 핸들(미사용).
 * @kobj: 이 속성이 붙은 kobject. pci_dev 로 복원한다.
 * @bin_attr: 바이너리 속성 서술자(미사용).
 * @buf: 사용자가 쓴 내용.
 * @off: 파일 오프셋.
 * @count: 쓴 바이트 수.
 * @return: 항상 count.
 *
 * 이 파일에 쓰는 것은 ROM 내용을 바꾸는 것이 아니다. "앞으로 읽기를
 * 허용할지" 를 켜고 끄는 스위치일 뿐이다. 상류 영어 주석이 말하듯
 * 0 이 아닌 무엇을 쓰면 켜진다.
 *
 * 왜 이런 2단계 프로토콜인가: 옵션 ROM 을 읽으려면 커널이 ROM BAR 의
 * 디코딩을 잠시 켜야 하는데, 그동안 그 물리 주소 창이 살아나면서 다른
 * 자원과 겹칠 수 있다. 항상 켜 두면 위험하므로, 사용자가 명시적으로
 * "지금부터 읽겠다" 고 선언하게 만든 것이다. 실제 사용 순서는
 *   echo 1 > rom  ->  cat rom > out.bin  ->  echo 0 > rom
 * 이다.
 *
 * 끄는 조건이 유난히 까다로운 이유: off 가 0 이고, 첫 바이트가 문자 '0' 이며,
 * count 가 정확히 2 일 때만 끈다. count 가 2 인 것은 셸의 echo 가 "0" 뒤에
 * 줄바꿈을 붙여 두 바이트를 쓰기 때문이다. 이 세 조건을 모두 만족하지
 * 않으면 전부 "켜기" 로 해석한다. 즉 애매한 입력은 안전한 쪽이 아니라
 * 켜는 쪽으로 기운다는 점에 유의해야 한다.
 * 문자 '0' 을 비교하는 것이지 숫자 0 이 아니라는 점도 중요하다 —
 * 이 파일은 kstrtoul 을 쓰지 않고 바이트를 직접 본다.
 *
 * NVMe 학습 관점: NVMe SSD 는 대개 옵션 ROM 을 갖지 않는다(부팅용 ROM 이
 * 있는 일부 엔터프라이즈 제품은 예외다). ROM 자원이 없으면 아래
 * pci_dev_rom_attr_is_visible() 이 파일 자체를 만들지 않으므로, 보통은
 * NVMe 장치 디렉터리에 rom 파일이 보이지 않는다.
 *
 * 실행 컨텍스트: write(2) 프로세스 문맥. 하드웨어 접근이 없고 락도 없다.
 *
 * 호출 체인:
 *   write(2) -> sysfs bin_attr write -> [pci_write_rom]
 */
static ssize_t pci_write_rom(struct file *filp, struct kobject *kobj, /* [한국어] 파일 핸들(미사용)과 kobject */
			     const struct bin_attribute *bin_attr, char *buf, /* [한국어] 속성 서술자(미사용)와 사용자 입력 */
			     loff_t off, size_t count) /* [한국어] 파일 오프셋과 입력 길이 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* [한국어] kobject -> device -> pci_dev 복원 */

	if ((off ==  0) && (*buf == '0') && (count == 2)) /* [한국어] 정확히 "0\n" 두 바이트를 파일 처음에 썼을 때만 끈다. 문자 '0' 이지 숫자 0 이 아니다 */
		pdev->rom_attr_enabled = 0; /* [한국어] 읽기 금지로 되돌린다 */
	else /* [한국어] 그 밖의 모든 입력 */
		pdev->rom_attr_enabled = 1; /* [한국어] 켠다. 애매한 입력은 켜는 쪽으로 해석된다는 점에 유의 */

	return count; /* [한국어] 입력을 전부 소비했다고 알림 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * pci_read_rom - /sys/bus/pci/devices/<BDF>/rom 읽기 콜백 (옵션 ROM 덤프)
 *
 * @filp: 파일 핸들(미사용).
 * @kobj: 이 속성이 붙은 kobject. pci_dev 로 복원한다.
 * @bin_attr: 바이너리 속성 서술자(미사용).
 * @buf: 읽은 ROM 내용을 담을 커널 버퍼.
 * @off: ROM 안에서의 오프셋.
 * @count: 읽으려는 바이트 수.
 * @return: 실제로 읽은 바이트 수(끝을 넘으면 0), 스위치가 꺼져 있으면
 *   -EINVAL, 매핑에 실패하면 -EIO.
 *
 * 이 파일을 읽으면 장치의 옵션 ROM(PCI Expansion ROM) 내용이 나온다.
 * 그래픽 카드의 VGA BIOS, 네트워크 카드의 PXE 부팅 코드 같은 것이 여기
 * 들어 있다. 위 pci_write_rom() 으로 먼저 켜 두지 않으면 -EINVAL 이다.
 *
 * 동작 단계:
 *  1) rom_attr_enabled 검사. 꺼져 있으면 하드웨어를 건드리지 않고 거절한다.
 *  2) pci_map_rom() 으로 ROM 창을 매핑한다. 상류 주석이 밝히듯 size 는
 *     처음에 "PCI 창 크기" 로 들어갔다가, 함수가 실제 ROM 이미지 크기로
 *     줄여서 돌려준다. 즉 이 인자는 입출력 겸용이다. 창이 4MB 라도 실제
 *     이미지는 64KB 일 수 있으므로, 이 구분이 중요하다.
 *  3) 매핑이 실패했거나 크기가 0 이면 -EIO. 두 조건을 모두 보는 이유는
 *     매핑은 됐는데 유효한 이미지가 없는 경우가 있기 때문이다.
 *  4) 경계 처리. off 가 이미 끝을 넘었으면 count 를 0 으로 만들어 EOF 로
 *     알리고, 걸쳐 있으면 남은 만큼으로 줄인다. 여기서 오류를 내지 않고
 *     조용히 줄이는 것은 파일 읽기의 자연스러운 규약이다.
 *  5) memcpy_fromio() 로 복사한다. 일반 memcpy 가 아닌 이유는 원본이
 *     일반 메모리가 아니라 MMIO 창이기 때문이다. 이 함수는 그 아키텍처에
 *     맞는 접근 폭과 순서로 읽어 준다.
 *  6) pci_unmap_rom() 으로 반드시 매핑을 되돌린다. 이 함수는 ROM 디코딩을
 *     다시 꺼 주기도 하므로, 빠뜨리면 ROM 창이 계속 열린 채 남는다.
 *     조기 return 이 매핑 전에만 있어서 누수가 발생하지 않는 구조다.
 *
 * 실행 컨텍스트: read/pread(2) 프로세스 문맥. 매핑과 MMIO 접근 때문에
 * 잠들 수 있다.
 *
 * 호출 체인:
 *   pread(2) -> sysfs bin_attr read -> [pci_read_rom]
 *     -> pci_map_rom -> memcpy_fromio -> pci_unmap_rom (drivers/pci/rom.c)
 */
/**
 * pci_read_rom - read a PCI ROM
 * @filp: sysfs file
 * @kobj: kernel object handle
 * @bin_attr: struct bin_attribute for this file
 * @buf: where to put the data we read from the ROM
 * @off: file offset
 * @count: number of bytes to read
 *
 * Put @count bytes starting at @off into @buf from the ROM in the PCI
 * device corresponding to @kobj.
 */
static ssize_t pci_read_rom(struct file *filp, struct kobject *kobj, /* [한국어] 파일 핸들(미사용)과 kobject */
			    const struct bin_attribute *bin_attr, char *buf, /* [한국어] 속성 서술자(미사용)와 결과 버퍼 */
			    loff_t off, size_t count) /* [한국어] ROM 안 오프셋과 요청 바이트 수 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* [한국어] kobject -> device -> pci_dev 복원 */
	void __iomem *rom; /* [한국어] 매핑된 ROM 창의 커널 가상 주소. __iomem 표시가 "일반 메모리처럼 역참조하지 말라" 는 뜻이다 */
	size_t size; /* [한국어] 입출력 겸용 — 들어갈 때는 창 크기, 나올 때는 실제 이미지 크기 */

	if (!pdev->rom_attr_enabled) /* [한국어] 사용자가 아직 rom 파일에 1 을 쓰지 않았는가 */
		return -EINVAL; /* [한국어] 하드웨어를 건드리기 전에 거절. ROM 디코딩을 함부로 켜지 않기 위한 2단계 프로토콜의 앞 단계다 */

	rom = pci_map_rom(pdev, &size);	/* size starts out as PCI window size */
	/* [한국어] 위 영어 주석이 핵심을 짚는다: size 는 창 크기로 들어갔다가
	 * 실제 ROM 이미지 크기로 줄어 나온다. 창이 4MB 라도 이미지가 64KB 면
	 * 이후 경계 검사는 64KB 를 기준으로 이뤄진다. */
	if (!rom || !size) /* [한국어] 매핑 실패이거나, 매핑은 됐지만 유효한 이미지가 없는 경우 */
		return -EIO; /* [한국어] 둘 다 읽을 것이 없다는 뜻이라 같은 오류로 처리 */

	if (off >= size) /* [한국어] 시작 위치가 이미 이미지 끝을 넘었는가 */
		count = 0; /* [한국어] 0 바이트를 읽은 것으로 처리 = EOF. 아래 unmap 을 거쳐야 하므로 여기서 return 하지 않는다 */
	else { /* [한국어] 읽을 것이 남아 있는 경우 */
		if (off + count > size) /* [한국어] 요청 끝이 이미지를 넘어가는가 */
			count = size - off; /* [한국어] 남은 만큼으로 줄인다. 오류가 아니라 짧은 읽기로 처리하는 것이 파일 규약이다 */

		memcpy_fromio(buf, rom + off, count); /* [한국어] MMIO 창에서 커널 버퍼로 복사. 일반 memcpy 를 쓰면 아키텍처에 따라 잘못된 폭/순서로 접근하게 된다 */
	} /* [한국어] 복사 블록 종료 */
	pci_unmap_rom(pdev, rom); /* [한국어] 매핑을 되돌리고 ROM 디코딩도 다시 끈다. 위 조기 return 들이 모두 매핑 전에 있어 누수가 없다 */

	return count; /* [한국어] 실제로 옮긴 바이트 수(0 이면 EOF) */
} /* [한국어] 함수 본문 종료 */
/* [한국어] bin_attr_rom - /sys/bus/pci/devices/<BDF>/rom 파일을 만드는 바이너리 속성.
 * 모드 0600 이라 루트만 열 수 있다 — config(0644)와 달리 읽기도 제한하는데,
 * ROM 접근이 하드웨어 디코딩 창을 켜는 부작용을 동반하기 때문이다.
 * 정적 크기 0 은 "장치마다 다르니 bin_size 콜백에 묻는다" 는 뜻이다. */
static const BIN_ATTR(rom, 0600, pci_read_rom, pci_write_rom, 0); /* [한국어] 이름/모드/읽기/쓰기/정적크기(0=동적) */

/* [한국어] pci_dev_rom_attrs - rom 파일 하나를 담은 바이너리 속성 배열. */
static const struct bin_attribute *const pci_dev_rom_attrs[] = { /* [한국어] 배열 시작 */
	&bin_attr_rom, /* [한국어] 위에서 만든 rom 바이너리 속성 */
	NULL, /* [한국어] 끝 센티널 */
}; /* [한국어] 배열 종료 */

/*
 * [한국어]
 * pci_dev_rom_attr_is_visible - rom 파일을 이 장치에 만들지 말지 결정하는 콜백
 *
 * @kobj: 대상 kobject. pci_dev 로 복원한다.
 * @a: 가시성을 묻는 바이너리 속성(여기서는 rom 하나뿐).
 * @n: 그룹 안 인덱스(미사용).
 * @return: 0 이면 파일을 만들지 않고, 0 이 아니면 그 값이 파일 모드가 된다.
 *
 * 이것이 attribute_group 의 is_bin_visible 메커니즘이다. 정적 배열 하나로
 * 온갖 장치를 다루면서도, 장치마다 파일을 있게 하거나 없게 할 수 있다.
 * 일반 속성용은 .is_visible, 바이너리 속성용은 .is_bin_visible 로 필드가
 * 나뉘어 있다.
 *
 * 판정: 확장 ROM 자원(PCI_ROM_RESOURCE)의 끝 주소가 0 이면 이 장치에는
 * ROM 이 없다는 뜻이므로 0 을 돌려 파일을 만들지 않는다. 있으면 속성이
 * 원래 갖고 있던 모드(0600)를 그대로 돌려준다. 모드를 여기서 새로 지어내지
 * 않고 a->attr.mode 를 되돌려 주는 것이 관례다 — 권한 정의를 한 곳
 * (BIN_ATTR)에만 두기 위해서다.
 *
 * NVMe 학습 관점: 옵션 ROM 이 없는 보통의 NVMe SSD 에서는 이 콜백이 0 을
 * 돌려주므로 rom 파일이 아예 만들어지지 않는다. 장치 디렉터리에 rom 이
 * 없다고 해서 오류가 아니다.
 *
 * 실행 컨텍스트: sysfs 그룹 생성 시점의 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   장치 등록 -> sysfs 그룹 생성 -> [pci_dev_rom_attr_is_visible]
 */
static umode_t pci_dev_rom_attr_is_visible(struct kobject *kobj, /* [한국어] 대상 kobject */
					   const struct bin_attribute *a, int n) /* [한국어] 가시성을 묻는 속성과 그룹 내 인덱스(미사용) */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* [한국어] kobject -> device -> pci_dev 복원 */

	/* If the device has a ROM, try to expose it in sysfs. */
	/* [한국어] 위 영어 주석대로, ROM 이 있는 장치에만 파일을 노출한다. */
	if (!pci_resource_end(pdev, PCI_ROM_RESOURCE)) /* [한국어] 확장 ROM 자원의 끝 주소가 0 = 이 장치에는 ROM 이 없다 */
		return 0; /* [한국어] 0 을 돌려주면 sysfs 는 이 파일을 아예 만들지 않는다 */

	return a->attr.mode; /* [한국어] 속성이 원래 갖고 있던 모드(0600)를 그대로. 권한 정의를 BIN_ATTR 한 곳에만 두기 위한 관례다 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * pci_dev_rom_attr_bin_size - rom 파일의 크기를 장치마다 알려 주는 콜백
 *
 * @kobj: 대상 kobject. pci_dev 로 복원한다.
 * @a: 크기를 묻는 속성(미사용).
 * @n: 그룹 안 인덱스(미사용).
 * @return: 이 장치의 확장 ROM 자원 길이(바이트).
 *
 * BIN_ATTR 에 넣은 정적 크기가 0 이므로, 파일 크기를 여기서 알려 준다.
 * 이 값이 ls -l 에 보이는 크기이고, 사용자 공간이 얼마나 읽으면 되는지
 * 판단하는 기준이다.
 *
 * 주의: 여기서 돌려주는 것은 PCI 가 이 장치에 배정한 ROM "창" 의 크기다.
 * 실제 ROM 이미지가 그보다 작을 수 있으며, 그 경우 pci_read_rom() 이
 * pci_map_rom() 이 알려 준 실제 이미지 크기로 다시 잘라 준다. 즉 파일
 * 크기보다 실제로 읽히는 양이 적을 수 있다.
 *
 * 실행 컨텍스트: sysfs 파일 생성/stat 시점의 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   sysfs 그룹 생성/stat -> bin_size 콜백 -> [pci_dev_rom_attr_bin_size]
 */
static size_t pci_dev_rom_attr_bin_size(struct kobject *kobj, /* [한국어] 대상 kobject */
					const struct bin_attribute *a, int n) /* [한국어] 속성과 그룹 내 인덱스 — 둘 다 미사용 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* [한국어] kobject -> device -> pci_dev 복원 */

	return pci_resource_len(pdev, PCI_ROM_RESOURCE); /* [한국어] ROM 창의 길이. 실제 이미지는 이보다 작을 수 있고 그 조정은 pci_read_rom 이 한다 */
} /* [한국어] 함수 본문 종료 */

/* [한국어] pci_dev_rom_attr_group - rom 파일을 만드는 그룹.
 * pci_dev_groups[] 를 거쳐 pci_bus_type.dev_groups 로 등록된다.
 * config 그룹과 달리 is_bin_visible 을 두어, ROM 이 없는 장치에는 파일이
 * 만들어지지 않게 한다. */
static const struct attribute_group pci_dev_rom_attr_group = { /* [한국어] 구조체 초기화 시작 */
	.bin_attrs = pci_dev_rom_attrs, /* [한국어] 이 그룹이 만들 바이너리 속성 목록(rom 하나).
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: 드라이버 코어의 sysfs 그룹 생성 코드.
		 * 값 범위: NULL 로 끝나는 const bin_attribute 포인터 배열.
		 * 동기화: const 정적 데이터라 락 불필요. */
	.is_bin_visible = pci_dev_rom_attr_is_visible, /* [한국어] 장치마다 이 파일을 만들지 결정하는 콜백.
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: sysfs 코어가 그룹을 만들 때 속성마다 한 번씩 부른다.
		 * 값 범위: 함수 포인터. NULL 이면 항상 만든다는 뜻이다.
		 * 동기화: 콜백이 락 없이 pci_dev 의 자원 정보만 읽는다.
		 * 일반 속성용 .is_visible 과 필드가 다르다는 점에 유의. */
	.bin_size = pci_dev_rom_attr_bin_size, /* [한국어] 이 파일의 크기를 장치마다 알려 주는 콜백.
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: sysfs 코어가 파일을 만들거나 크기를 물을 때.
		 * 값 범위: 함수 포인터. NULL 이면 BIN_ATTR 의 정적 크기를 쓴다.
		 * 동기화: 락 없이 자원 길이만 읽는다. */
}; /* [한국어] 구조체 초기화 종료 */

/*
 * [한국어]
 * reset_store - /sys/bus/pci/devices/<BDF>/reset 쓰기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 사용자가 쓴 문자열. 정확히 1 이어야 한다.
 * @count: 쓴 바이트 수.
 * @return: 성공 시 count, 파싱 실패나 1 이 아닌 값이면 -EINVAL,
 *   리셋 실패면 pci_reset_function() 의 오류.
 *
 * 이 파일에 1 을 쓰면 이 장치 하나만 리셋된다. 앞의
 * reset_subordinate(브리지 하위 버스 전체)와 범위가 다르다.
 * 실제로 어떤 방식으로 리셋할지는 아래 reset_method 파일이 정한 우선순위
 * 목록을 따라 pci_reset_function() 이 순서대로 시도한다.
 *
 * 입력 검사가 두 겹인 이유: kstrtoul 로 숫자인지 보고, 그다음 값이 정확히
 * 1 인지 본다. 다른 store 들이 "0 이 아니면 실행" 인 것과 달리 여기서는
 * 1 만 받는다. 실수로 아무 숫자나 흘려 넣어 장치를 리셋하는 사고를 줄이려는
 * 의도로 보이며, 이 판단의 근거가 되는 상류 주석은 없다.
 *
 * pm_runtime_get_sync / put 쌍의 역할: 리셋은 설정공간과 링크를 건드리므로
 * 장치가 절전 상태여서는 안 된다. _sync 판은 실제로 깨어날 때까지 기다린다.
 * 리셋 결과와 무관하게 put 을 먼저 부르고 나서 오류를 검사하는 순서라,
 * 실패해도 PM 참조가 새지 않는다.
 *
 * 여기에는 capable(CAP_SYS_ADMIN) 검사가 없다. DEVICE_ATTR_WO 로 만들어진
 * 쓰기 전용 파일이라 소유자(루트)만 열 수 있기 때문이다 — 정확한 모드 값은
 * include/linux/device.h 의 매크로 정의에 있고 이 트리에는 그 헤더가 없어
 * 확인할 수 없다.
 *
 * NVMe 학습 관점: NVMe 컨트롤러가 응답하지 않을 때 마지막으로 시도하는
 * 수단이다. 다만 이것은 PCI 함수 수준 리셋(대개 FLR)이라, NVMe 스펙의
 * 컨트롤러 리셋(CC.EN 을 0 으로)이나 서브시스템 리셋(NSSR)과는 층위가 다르다.
 * 리셋 후에는 BAR, MSI-X, Command 레지스터가 초기화되므로 드라이버가
 * 전부 다시 설정해야 하며, 드라이버가 붙어 있는 채로 이 파일을 쓰면
 * 진행 중이던 I/O 는 실패한다.
 *
 * 실행 컨텍스트: write(2) 프로세스 문맥. 장치를 깨우고 리셋 후 복구를
 * 기다리므로 수백 밀리초 단위로 잠들 수 있다.
 *
 * 호출 체인:
 *   write(2) -> sysfs -> dev_attr_store -> [reset_store]
 *     -> pm_runtime_get_sync -> pci_reset_function (drivers/pci/pci.c)
 */
static ssize_t reset_store(struct device *dev, struct device_attribute *attr, /* [한국어] 대상 device 와 속성 서술자(미사용) */
			   const char *buf, size_t count) /* [한국어] 사용자 입력 문자열과 길이 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	unsigned long val; /* [한국어] 파싱된 사용자 값 */
	ssize_t result; /* [한국어] 리셋 결과. 음수면 오류 */

	if (kstrtoul(buf, 0, &val) < 0) /* [한국어] 진법 자동 판별 파싱 */
		return -EINVAL; /* [한국어] 숫자가 아니면 거부 */

	if (val != 1) /* [한국어] 다른 store 들과 달리 정확히 1 만 받는다 */
		return -EINVAL; /* [한국어] 실수로 아무 값이나 흘려 넣어 리셋되는 것을 막는다 */

	pm_runtime_get_sync(dev); /* [한국어] 리셋은 설정공간과 링크를 건드리므로 장치가 깨어 있어야 한다. _sync 판은 실제로 D0 가 될 때까지 기다린다 */
	result = pci_reset_function(pdev); /* [한국어] reset_method 가 정한 우선순위대로 리셋 방법을 시도한다 */
	pm_runtime_put(dev); /* [한국어] 결과를 검사하기 전에 먼저 놓는다 — 아래 조기 return 에서 참조가 새지 않도록 */
	if (result < 0) /* [한국어] 어떤 방법으로도 리셋하지 못했는가 */
		return result; /* [한국어] 실패를 감추지 않고 그대로 전달 */

	return count; /* [한국어] 리셋 성공 — 입력을 전부 소비했다고 알림 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_WO(reset); /* [한국어] dev_attr_reset 생성. WO = 쓰기 전용(읽을 상태가 없다) */

/* [한국어] pci_dev_reset_attrs - reset 파일 하나를 담은 속성 배열.
 * 아래 pci_dev_reset_attr_group 에 담기고, 그 is_visible 이
 * 리셋을 지원하지 않는 장치에는 파일을 만들지 않는다. */
static struct attribute *pci_dev_reset_attrs[] = { /* [한국어] 배열 시작 */
	&dev_attr_reset.attr, /* [한국어] reset — 이 장치 하나를 리셋하는 쓰기 전용 파일 */
	NULL, /* [한국어] 끝 센티널 */
}; /* [한국어] 배열 종료 */

/*
 * [한국어]
 * pci_dev_reset_attr_is_visible - reset 과 reset_method 파일의 가시성 판정 콜백
 *
 * @kobj: 대상 kobject. pci_dev 로 복원한다.
 * @a: 가시성을 묻는 속성.
 * @n: 그룹 안 인덱스(미사용).
 * @return: 0 이면 파일을 만들지 않고, 0 이 아니면 그 값이 파일 모드가 된다.
 *
 * 판정은 한 줄이다 — pci_reset_supported() 가 거짓이면 이 장치는 어떤
 * 방법으로도 리셋할 수 없으므로 reset 파일을 만들 이유가 없다. 있으면
 * 속성이 원래 갖고 있던 모드를 그대로 돌려준다.
 *
 * 이 콜백 하나가 두 그룹에서 공유된다는 점이 특징이다.
 * pci_dev_reset_attr_group(reset 파일)과
 * pci_dev_reset_method_attr_group(reset_method 파일)이 같은 함수를 쓴다.
 * 리셋 방법 목록을 보여 줄 이유가 곧 리셋이 가능하다는 것과 같기 때문에,
 * 판정 로직을 한 벌만 둔 것이다.
 *
 * 실행 컨텍스트: sysfs 그룹 생성 시점의 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   장치 등록 -> sysfs 그룹 생성 -> [pci_dev_reset_attr_is_visible]
 *     -> pci_reset_supported (drivers/pci/pci.h 에 선언)
 */
static umode_t pci_dev_reset_attr_is_visible(struct kobject *kobj, /* [한국어] 대상 kobject */
					     struct attribute *a, int n) /* [한국어] 가시성을 묻는 속성과 그룹 내 인덱스(미사용) */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* [한국어] kobject -> device -> pci_dev 복원 */

	if (!pci_reset_supported(pdev)) /* [한국어] 이 장치에 쓸 수 있는 리셋 방법이 하나도 없는가 */
		return 0; /* [한국어] 그렇다면 파일을 아예 만들지 않는다 */

	return a->mode; /* [한국어] 속성이 원래 갖고 있던 모드를 그대로. 바이너리 속성의 a->attr.mode 와 달리 일반 속성은 a->mode 다 */
} /* [한국어] 함수 본문 종료 */

/* [한국어] pci_dev_reset_attr_group - reset 파일을 만드는 그룹.
 * pci_dev_groups[] 를 거쳐 모든 PCI 장치에 시도되지만, is_visible 이
 * 리셋 불가 장치를 걸러 낸다. */
static const struct attribute_group pci_dev_reset_attr_group = { /* [한국어] 구조체 초기화 시작 */
	.attrs = pci_dev_reset_attrs, /* [한국어] 이 그룹이 만들 일반 속성 목록(reset 하나).
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: 드라이버 코어의 sysfs 그룹 생성 코드.
		 * 값 범위: NULL 로 끝나는 struct attribute 포인터 배열.
		 * 동기화: const 정적 데이터라 락 불필요. */
	.is_visible = pci_dev_reset_attr_is_visible, /* [한국어] 리셋을 지원하는 장치에만 파일을 만들게 하는 콜백.
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: sysfs 코어가 그룹을 만들 때 속성마다 한 번씩.
		 * 값 범위: 함수 포인터. 0 을 돌려주면 파일이 생기지 않는다.
		 * 동기화: 콜백이 락 없이 pci_dev 를 읽는다.
		 * 아래 reset_method 그룹과 같은 콜백을 공유한다. */
}; /* [한국어] 구조체 초기화 종료 */

/*
 * [한국어]
 * reset_method_show - /sys/bus/pci/devices/<BDF>/reset_method 읽기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수. 허용된 방법이 하나도 없으면 0(빈 파일).
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 장치를 리셋할 때 커널이 시도할 방법의
 * 이름들이, 시도할 순서대로 공백으로 이어져 나온다. 예: "flr pm bus".
 * 이름의 실체는 drivers/pci/pci.c 의 pci_reset_fn_methods[] 배열이며,
 * 그 파일에서 확인한 이름은 device_specific, acpi, flr, af_flr, pm, bus,
 * cxl_bus 일곱 가지다(0번 칸은 이름이 없는 목록 종료 표시다).
 *
 * pdev->reset_methods[] 에는 그 배열의 인덱스가 우선순위 순서로 들어 있고,
 * 값 0 이 목록의 끝을 뜻한다. 그래서 루프는 0 을 만나면 break 한다.
 * 이 규약은 drivers/pci/pci.c 의 pci_reset_fn_methods[] 첫 원소가 빈
 * 항목인 것과 짝을 이룬다 — 0 을 유효한 방법 번호로 쓰지 않기 위해서다.
 *
 * 출력 형식 만들기: sysfs_emit_at(buf, len, ...) 로 이미 찍은 len 바이트
 * 뒤에 이어 붙인다. 구분자를 `len ? " " : ""` 로 고른 것이 요령인데,
 * 첫 항목 앞에는 공백을 넣지 않고 두 번째부터만 넣는다. 마지막에 줄바꿈은
 * 무언가를 찍었을 때(len 이 0 이 아닐 때)에만 붙인다 — 빈 목록을 줄바꿈
 * 하나짜리 파일로 만들지 않으려는 것이다. 결과적으로 "모든 방법이 꺼져
 * 있다" 는 상태는 완전히 빈 파일로 표현된다.
 *
 * sprintf 가 아니라 sysfs_emit_at 을 쓰는 이유: sysfs_emit 계열은 buf 가
 * 페이지 시작이라는 sysfs 규약을 전제로 PAGE_SIZE 경계를 스스로 지킨다.
 * 방법 이름을 이어 붙이다 페이지를 넘길 일은 실제로 없지만, 규약을 따르는
 * 것이 안전하다.
 *
 * NVMe 학습 관점: NVMe SSD 에서 이 파일을 읽으면 보통 "flr pm bus" 같은
 * 목록이 나온다. 첫 항목이 실제로 시도될 방법이며, FLR(Function Level
 * Reset)이 가능한 장치라면 그것이 앞에 온다. 이것은 PCI 함수 리셋이지
 * NVMe 컨트롤러 리셋이 아니라는 점을 다시 확인해 두어야 한다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 하드웨어 접근 없이 캐시된 배열만
 * 읽으므로 락도 PM 도 없다.
 *
 * 호출 체인:
 *   read(2) -> sysfs -> dev_attr_show -> [reset_method_show] -> sysfs_emit_at
 */
static ssize_t reset_method_show(struct device *dev, /* [한국어] 대상 device */
				 struct device_attribute *attr, char *buf) /* [한국어] 속성 서술자(미사용)와 출력 버퍼 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	ssize_t len = 0; /* [한국어] 지금까지 찍은 누적 길이. 구분자 판단과 sysfs_emit_at 의 오프셋에 함께 쓰인다 */
	int i, m; /* [한국어] i 는 우선순위 배열 인덱스, m 은 거기 담긴 방법 번호 */

	for (i = 0; i < PCI_NUM_RESET_METHODS; i++) { /* [한국어] 우선순위 배열을 앞에서부터 훑는다. 순서가 곧 시도 순서다 */
		m = pdev->reset_methods[i]; /* [한국어] pci_reset_fn_methods[] 안에서의 번호를 꺼낸다 */
		if (!m) /* [한국어] 0 은 "목록의 끝" 을 뜻하는 약속이다(pci_reset_fn_methods[0] 이 빈 항목인 이유) */
			break; /* [한국어] 더 볼 것이 없으므로 루프 종료 */

		len += sysfs_emit_at(buf, len, "%s%s", len ? " " : "", /* [한국어] 첫 항목 앞에는 구분자를 붙이지 않고 두 번째부터만 공백을 넣는다 */
				     pci_reset_fn_methods[m].name); /* [한국어] 번호를 사람이 읽는 이름으로 바꾼다. 배열은 drivers/pci/pci.c 에 있다 */
	} /* [한국어] 목록 루프 종료 */

	if (len) /* [한국어] 무언가 찍었을 때에만 */
		len += sysfs_emit_at(buf, len, "\n"); /* [한국어] 줄바꿈을 붙인다. 빈 목록을 줄바꿈만 있는 파일로 만들지 않기 위한 조건이다 */

	return len; /* [한국어] 0 이면 "허용된 리셋 방법이 하나도 없음" 을 뜻하는 빈 파일이 된다 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * reset_method_lookup - 리셋 방법 이름을 pci_reset_fn_methods[] 인덱스로 바꾼다
 *
 * @name: 사용자가 쓴 방법 이름 한 토막(예: "flr").
 * @return: 찾으면 1 이상의 인덱스, 못 찾으면 0.
 *
 * 왜 1 부터 도는가: pci_reset_fn_methods[0] 은 실제 방법이 아니라 빈
 * 항목이다(drivers/pci/pci.c 에서 확인). 그리고 0 을 "못 찾음" 의 반환값으로
 * 쓰기 때문에, 0번을 검색 대상에 넣으면 두 의미가 충돌한다. 그래서 검색은
 * 반드시 1 부터 시작한다.
 *
 * strcmp 가 아니라 sysfs_streq() 를 쓰는 이유: sysfs 입력은 끝에 줄바꿈이
 * 붙어 오는 것이 보통이라, 단순 문자열 비교로는 "flr\n" 과 "flr" 이 다르다고
 * 나온다. sysfs_streq 는 끝의 줄바꿈을 무시하고 비교해 준다.
 *
 * 실행 컨텍스트: reset_method_store 안에서만 불린다. 락 없음.
 *
 * 호출 체인:
 *   reset_method_store -> [reset_method_lookup] -> sysfs_streq
 */
static int reset_method_lookup(const char *name) /* [한국어] 이름 -> 방법 번호 변환 */
{ /* [한국어] 함수 본문 시작 */
	int m; /* [한국어] pci_reset_fn_methods[] 를 훑을 인덱스 */

	for (m = 1; m < PCI_NUM_RESET_METHODS; m++) { /* [한국어] 0번은 빈 항목이자 "못 찾음" 의 반환값이므로 1 부터 시작해야 한다 */
		if (sysfs_streq(name, pci_reset_fn_methods[m].name)) /* [한국어] 끝의 줄바꿈을 무시하고 비교한다. sysfs 입력에는 개행이 붙어 오기 때문 */
			return m; /* [한국어] 찾은 방법 번호 */
	} /* [한국어] 검색 루프 종료 */

	return 0;	/* not found */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * reset_method_store - /sys/bus/pci/devices/<BDF>/reset_method 쓰기 콜백
 *
 * @dev: 대상 device.
 * @attr: 속성 서술자(미사용).
 * @buf: 사용자가 쓴 문자열. 공백으로 구분된 방법 이름 목록,
 *   빈 문자열, 또는 "default" 중 하나다.
 * @count: 쓴 바이트 수.
 * @return: 성공 시 count, 잘못된 이름/지원하지 않는 방법/개수 초과면 -EINVAL,
 *   장치를 깨우지 못하면 -ENXIO, 메모리 부족이면 -ENOMEM.
 *
 * 이 파일에 써서 리셋 방법의 허용 목록과 우선순위를 사람이 직접 정한다.
 * 세 가지 입력 형태가 있다.
 *  1) 빈 문자열 — 모든 리셋 방법을 끈다. reset_methods[0] 에 0(목록 끝)을
 *     넣는 것만으로 목록 전체가 비워진다. 이 경우 이후 reset 파일에 1 을
 *     써도 리셋할 방법이 없다. 위험한 상태이므로 경고를 남긴다.
 *  2) "default" — 커널이 스스로 조사한 기본 목록으로 되돌린다
 *     (pci_init_reset_methods).
 *  3) 이름 목록 — 예 "flr pm". 쓴 순서가 곧 시도 순서가 된다.
 *
 * 검증 우선 전략: 상류 주석이 "입력이 잘못되면 이전 목록을 그대로 둔다" 고
 * 밝힌 대로, 파싱한 결과를 지역 배열 reset_methods[] 에 모으고 모든 검사를
 * 통과한 뒤에야 마지막 memcpy 로 pdev->reset_methods 에 한 번에 옮긴다.
 * 중간에 실패하면 장치의 기존 목록은 손대지 않은 상태로 남는다. 절반만
 * 반영된 목록으로 리셋을 시도하는 최악의 상황을 막는 구조다.
 *
 * 각 이름에 대한 세 가지 검사:
 *  - reset_method_lookup 으로 이름이 실재하는지.
 *  - 그 방법의 reset_fn 을 PCI_RESET_PROBE 모드로 불러, 실제로 이 장치에서
 *     쓸 수 있는지. PROBE 모드는 "지원 여부만 조사하고 실제로 리셋하지는
 *     말라" 는 뜻이다(drivers/pci/pci.c 의 사용례에서 확인).
 *  - 개수가 배열 용량을 넘지 않는지. PCI_NUM_RESET_METHODS - 1 에서 멈추는
 *     것은 마지막 한 칸을 목록 끝 표시(0)로 남겨 둬야 하기 때문이다.
 *
 * 문자열 처리:
 *  - kstrndup 으로 사용자 버퍼를 복사한다. buf 는 const 이고 strsep 이
 *    원본을 파괴적으로 자르기 때문에 사본이 필요하다. count 를 상한으로
 *    주는 kstrndup 판을 쓰는 것이 안전하다.
 *  - __free(kfree) 덕분에 아래 어느 return 으로 빠져나가도 자동 해제된다.
 *    return 이 네 곳이나 되어 수동 해제였다면 실수하기 딱 좋은 구조다.
 *  - strsep 은 tmp_options 포인터를 전진시키며 토막을 돌려준다. 그래서
 *    원본 options 포인터를 따로 보존해야 하고(그것이 tmp_options 를 두는
 *    이유다), 연속된 공백은 빈 토막을 만들어 내므로 sysfs_streq(name, "")
 *    로 건너뛴다. strim 은 남은 앞뒤 공백과 줄바꿈을 떼어 낸다.
 *
 * PM_RUNTIME_ACQUIRE(dev, pm): 아래에서 각 방법의 지원 여부를 조사하려면
 * 설정공간에 접근해야 하므로 장치가 깨어 있어야 한다. 이 매크로가 그
 * 참조를 잡고, PM_RUNTIME_ACQUIRE_ERR 로 실패를 확인한다. 이후 여러 곳에서
 * 명시적 해제 없이 return 하는 것으로 보아 범위를 벗어날 때 자동으로
 * 놓아 주는 형태다. 다만 이 매크로의 정의는 include/linux/pm_runtime.h 에
 * 있고 이 트리에는 그 헤더가 없어 원문은 확인할 수 없다.
 * 빈 문자열 처리가 이 매크로 앞에 있는 것에 주목 — 목록을 비우는 데에는
 * 하드웨어 접근이 필요 없으므로 장치를 깨우지 않는다.
 *
 * 마지막 경고: 인덱스 1 번 방법(drivers/pci/pci.c 에서 확인한 바로는
 * device_specific)이 이 장치에서 지원되는데도 사용자가 그것을 첫 순위로
 * 두지 않았다면 경고를 남긴다. 장치 제조사가 제공한 전용 절차가 대개 가장
 * 안전하고 완전한 리셋이기 때문에, 그것을 뒤로 미루거나 빼는 것은 위험을
 * 감수하는 선택임을 알리는 것이다.
 *
 * 실행 컨텍스트: write(2) 프로세스 문맥. 지원 여부 조사에서 설정공간을
 * 건드리므로 잠들 수 있다.
 *
 * 호출 체인:
 *   write(2) -> sysfs -> dev_attr_store -> [reset_method_store]
 *     -> reset_method_lookup / pci_reset_fn_methods[].reset_fn(PROBE)
 *     -> memcpy 로 pdev->reset_methods 갱신
 */
static ssize_t reset_method_store(struct device *dev, /* [한국어] 대상 device */
				  struct device_attribute *attr, /* [한국어] 속성 서술자 — 미사용 */
				  const char *buf, size_t count) /* [한국어] 사용자 입력 문자열과 길이 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	char *tmp_options, *name; /* [한국어] tmp_options 는 strsep 이 전진시킬 커서, name 은 잘라 낸 토막 */
	int m, n; /* [한국어] m 은 방법 번호, n 은 지역 배열에 채운 개수 */
	u8 reset_methods[PCI_NUM_RESET_METHODS] = {}; /* [한국어] 검증을 통과한 결과를 모을 지역 배열. 0 으로 초기화되어 있어 끝 표시가 기본으로 들어 있다. 여기 모았다가 마지막에 한 번에 반영하는 것이 이 함수의 핵심 전략이다 */

	if (sysfs_streq(buf, "")) { /* [한국어] 빈 입력 = 모든 리셋 방법을 끄라는 뜻 */
		pdev->reset_methods[0] = 0; /* [한국어] 첫 칸에 목록 끝 표시를 넣으면 그것만으로 목록 전체가 비워진다 */
		pci_warn(pdev, "All device reset methods disabled by user"); /* [한국어] 이제 이 장치는 리셋할 수 없다 — 위험한 상태이므로 로그를 남긴다 */
		return count; /* [한국어] 하드웨어 접근이 없으므로 아래 PM 획득 전에 끝낸다 */
	} /* [한국어] 빈 입력 처리 종료 */

	PM_RUNTIME_ACQUIRE(dev, pm); /* [한국어] 아래에서 각 방법의 지원 여부를 조사하려면 설정공간 접근이 필요하므로 장치를 깨워 둔다. 범위를 벗어날 때 자동 해제되는 형태로 보이며, 매크로 정의는 이 트리에 없어 확인할 수 없다 */
	if (PM_RUNTIME_ACQUIRE_ERR(&pm)) /* [한국어] 장치를 깨우지 못했는가 */
		return -ENXIO; /* [한국어] 접근할 수 없는 장치이므로 조사 자체가 불가능하다 */

	if (sysfs_streq(buf, "default")) { /* [한국어] "default" = 커널이 스스로 정한 기본 목록으로 되돌리라는 뜻 */
		pci_init_reset_methods(pdev); /* [한국어] 장치를 다시 조사해 우선순위 배열을 처음부터 채운다 */
		return count; /* [한국어] 사용자 목록을 파싱할 필요가 없으므로 여기서 끝 */
	} /* [한국어] default 처리 종료 */

	char *options __free(kfree) = kstrndup(buf, count, GFP_KERNEL); /* [한국어] strsep 이 원본을 파괴적으로 자르므로 사본이 필요하다. __free(kfree)로 어느 return 에서든 자동 해제된다 — 아래 return 이 네 곳이나 되어 수동 해제는 실수하기 쉽다 */
	if (!options) /* [한국어] 복사 실패 */
		return -ENOMEM; /* [한국어] 장치 목록은 손대지 않은 채 반환 */

	n = 0; /* [한국어] 지역 배열에 채운 개수 */
	tmp_options = options; /* [한국어] strsep 이 이 포인터를 전진시킨다. 원본 options 는 해제를 위해 그대로 둬야 한다 */
	while ((name = strsep(&tmp_options, " ")) != NULL) { /* [한국어] 공백을 구분자로 한 토막씩 잘라 낸다. 더 없으면 NULL */
		if (sysfs_streq(name, "")) /* [한국어] 연속된 공백은 빈 토막을 만든다 */
			continue; /* [한국어] 그냥 건너뛴다 */

		name = strim(name); /* [한국어] 남은 앞뒤 공백과 줄바꿈을 떼어 낸다. 사용자가 echo 로 쓰면 끝에 개행이 붙는다 */

		/* Leave previous methods unchanged if input is invalid */
		/* [한국어] 위 영어 주석이 이 함수 전체의 설계를 요약한다: 입력이
		 * 잘못되면 장치의 기존 목록을 건드리지 않는다. 그래서 아래 실패
		 * 경로들이 모두 지역 배열만 버리고 그냥 return 한다. */
		m = reset_method_lookup(name); /* [한국어] 이름 -> 방법 번호. 없으면 0 */
		if (!m) { /* [한국어] 그런 이름의 리셋 방법이 없다 */
			pci_err(pdev, "Invalid reset method '%s'", name); /* [한국어] 어떤 이름이 문제였는지 로그로 알려 준다 */
			return -EINVAL; /* [한국어] 기존 목록은 그대로 둔 채 실패 */
		} /* [한국어] 이름 검증 실패 처리 종료 */

		if (pci_reset_fn_methods[m].reset_fn(pdev, PCI_RESET_PROBE)) { /* [한국어] PROBE 모드로 불러 "이 장치에서 이 방법을 쓸 수 있는가" 만 조사한다. 실제로 리셋하지는 않는다 */
			pci_err(pdev, "Unsupported reset method '%s'", name); /* [한국어] 이름은 맞지만 이 장치가 지원하지 않는 경우 */
			return -EINVAL; /* [한국어] 쓸 수 없는 방법을 목록에 넣어 두면 리셋 때 헛돌게 되므로 거절 */
		} /* [한국어] 지원 여부 검증 실패 처리 종료 */

		if (n == PCI_NUM_RESET_METHODS - 1) { /* [한국어] 마지막 한 칸은 목록 끝 표시(0) 자리로 남겨 둬야 한다 */
			pci_err(pdev, "Too many reset methods\n"); /* [한국어] 배열 용량 초과 */
			return -EINVAL; /* [한국어] 넘치기 전에 거절 — 배열을 넘어 쓰지 않도록 */
		} /* [한국어] 개수 검증 실패 처리 종료 */

		reset_methods[n++] = m; /* [한국어] 세 검사를 모두 통과했으므로 지역 배열에 순서대로 담는다. 아직 장치에는 반영하지 않는다 */
	} /* [한국어] 토막 파싱 루프 종료 */

	reset_methods[n] = 0; /* [한국어] 마지막에 목록 끝 표시를 명시적으로 넣는다. 배열이 0 으로 초기화되어 있어 사실상 이미 0 이지만, 의도를 분명히 하는 코드다 */

	/* Warn if dev-specific supported but not highest priority */
	/* [한국어] 위 영어 주석대로, 장치 전용 리셋을 쓸 수 있는데 사용자가
	 * 그것을 첫 순위에 두지 않은 경우를 경고한다. 제조사가 제공한 전용
	 * 절차가 대개 가장 안전하고 완전하기 때문이다. */
	if (pci_reset_fn_methods[1].reset_fn(pdev, PCI_RESET_PROBE) == 0 && /* [한국어] 1번 방법(drivers/pci/pci.c 기준 device_specific)이 이 장치에서 지원되는가. 0 이 "지원됨" 이다 */
	    reset_methods[0] != 1) /* [한국어] 그런데 사용자가 정한 첫 순위가 1번이 아닌가(빼 버렸거나 뒤로 미뤘거나) */
		pci_warn(pdev, "Device-specific reset disabled/de-prioritized by user"); /* [한국어] 막지는 않고 경고만. 사용자의 선택을 존중하되 위험을 기록한다 */
	memcpy(pdev->reset_methods, reset_methods, sizeof(pdev->reset_methods)); /* [한국어] 모든 검증을 통과한 뒤에야 한 번에 반영한다. 이 한 줄 앞에서 실패했다면 장치 목록은 손대지 않은 상태다 */
	return count; /* [한국어] 입력을 전부 소비했다고 알림 */
} /* [한국어] 함수 본문 종료 */
static DEVICE_ATTR_RW(reset_method); /* [한국어] dev_attr_reset_method 생성(읽기+쓰기) */

/* [한국어] pci_dev_reset_method_attrs - reset_method 파일 하나를 담은 배열. */
static struct attribute *pci_dev_reset_method_attrs[] = { /* [한국어] 배열 시작 */
	&dev_attr_reset_method.attr, /* [한국어] reset_method — 허용 리셋 방법 목록을 읽고 쓰는 파일 */
	NULL, /* [한국어] 끝 센티널 */
}; /* [한국어] 배열 종료 */

/* [한국어] pci_dev_reset_method_attr_group - reset_method 파일을 만드는 그룹.
 * is_visible 로 위 reset 그룹과 같은 콜백을 재사용한다 — 리셋이 불가능한
 * 장치에는 방법 목록을 보여 줄 이유도 없기 때문이다. */
static const struct attribute_group pci_dev_reset_method_attr_group = { /* [한국어] 구조체 초기화 시작 */
	.attrs = pci_dev_reset_method_attrs, /* [한국어] 이 그룹이 만들 속성 목록(reset_method 하나).
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: 드라이버 코어의 sysfs 그룹 생성 코드.
		 * 값 범위: NULL 로 끝나는 struct attribute 포인터 배열.
		 * 동기화: const 정적 데이터라 락 불필요. */
	.is_visible = pci_dev_reset_attr_is_visible, /* [한국어] reset 그룹과 공유하는 가시성 콜백.
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: sysfs 코어가 그룹을 만들 때.
		 * 값 범위: 함수 포인터. pci_reset_supported() 가 거짓이면 0.
		 * 동기화: 콜백이 락 없이 pci_dev 를 읽는다.
		 * 두 그룹이 같은 함수를 쓰는 것은 판정 근거가 동일하기 때문이다. */
}; /* [한국어] 구조체 초기화 종료 */

/*
 * [한국어]
 * __resource_resize_show - resourceN_resize 파일들의 읽기 공통 본체
 *
 * @dev: 대상 device.
 * @n: BAR 번호(0~5). 매크로가 만든 여섯 개의 얇은 래퍼가 각각 상수로 넣는다.
 * @buf: 출력 버퍼.
 * @return: 찍은 바이트 수.
 *
 * 이 파일을 읽으면 알 수 있는 것: 이 BAR 를 어떤 크기로 바꿀 수 있는지를
 * 나타내는 비트맵이 16자리 16진수로 나온다. PCIe Resizable BAR
 * capability 의 규약에 따라, 비트 i 가 서 있으면 2^i MB 크기를 지원한다는
 * 뜻이다(예: 비트 0 = 1MB, 비트 10 = 1GB).
 *
 * 왜 런타임 PM 을 잡는가: 가능한 크기 목록은 하드웨어의 Resizable BAR
 * capability 레지스터에서 읽어야 하므로 장치가 D0 여야 한다. get 과 put 을
 * 짝지어 잡고, 결과를 ret 에 담아 두었다가 put 뒤에 반환한다.
 *
 * 이 파일은 pci_dev_resource_resize_group 에 속하고, 그 is_visible 인
 * resource_resize_is_visible() 이 Resizable BAR 를 지원하지 않는 BAR 에는
 * 파일을 만들지 않는다.
 *
 * NVMe 학습 관점: Resizable BAR 는 주로 GPU 처럼 큰 프레임버퍼를 노출해야
 * 하는 장치에서 쓰인다. 보통의 NVMe SSD 는 BAR0 크기가 고정이라 이 파일이
 * 만들어지지 않는다. 특정 장치가 실제로 이 capability 를 갖는지는 하드웨어에
 * 달린 문제라 이 트리의 코드만으로는 알 수 없다.
 *
 * 실행 컨텍스트: read(2) 프로세스 문맥. 설정공간 접근 때문에 잠들 수 있다.
 *
 * 호출 체인:
 *   read(2) -> resourceN_resize_show(매크로 생성) -> [__resource_resize_show]
 *     -> pci_rebar_get_possible_sizes
 */
static ssize_t __resource_resize_show(struct device *dev, int n, char *buf) /* [한국어] BAR n 의 가능한 크기 비트맵을 찍는다 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	ssize_t ret; /* [한국어] 출력 길이. PM 해제를 먼저 하려고 잡아 둔다 */

	pci_config_pm_runtime_get(pdev); /* [한국어] Resizable BAR capability 레지스터를 읽어야 하므로 장치를 깨운다 */

	ret = sysfs_emit(buf, "%016llx\n", /* [한국어] 64비트 비트맵을 앞을 0 으로 채운 16자리 16진수로. 폭을 고정해야 파싱하는 쪽이 편하다 */
			 pci_rebar_get_possible_sizes(pdev, n)); /* [한국어] 비트 i = 2^i MB 크기를 지원한다는 뜻의 비트맵을 얻는다 */

	pci_config_pm_runtime_put(pdev); /* [한국어] get 과 짝을 맞춰 놓는다 */

	return ret; /* [한국어] PM 해제 뒤에 반환 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * __resource_resize_store - resourceN_resize 파일들의 쓰기 공통 본체
 *
 * @dev: 대상 device.
 * @n: BAR 번호(0~5).
 * @buf: 사용자가 쓴 크기 지수. 비트 번호를 쓴다(예: 10 이면 2^10 MB = 1GB).
 * @count: 쓴 바이트 수.
 * @return: 성공 시 count, 파싱 실패면 -EINVAL, 드라이버가 붙어 있거나 VF 가
 *   활성이면 -EBUSY, 그 밖에는 하위 함수의 오류.
 *
 * 이 파일에 써서 BAR 크기를 실제로 바꾼다. BAR 크기가 바뀌면 그 장치가
 * 차지하는 주소 범위가 달라지므로, 이 함수는 단순히 레지스터 하나를 쓰는
 * 것이 아니라 자원 재배치까지 포함한 큰 수술을 수행한다.
 *
 * 왜 이렇게 조심스러운가 — 단계별로:
 *  1) device_lock(dev) 로 드라이버 바인딩 상태를 고정한다.
 *  2) 드라이버가 붙어 있거나(dev->driver) SR-IOV VF 가 활성이면
 *     (pci_num_vf) -EBUSY. 사용 중인 장치의 BAR 를 옮기면 그 드라이버가
 *     들고 있던 매핑이 전부 엉뚱한 곳을 가리키게 된다.
 *  3) 런타임 PM 으로 장치를 깨운다(설정공간을 여러 번 건드린다).
 *  4) VGA 클래스 장치라면 그 BAR 를 물고 있는 부트 콘솔 프레임버퍼
 *     드라이버를 먼저 쫓아낸다(aperture_remove_conflicting_pci_devices).
 *     클래스를 8비트 오른쪽으로 민 값과 비교하는 것은, class 필드 24비트
 *     중 하위 바이트인 programming interface 를 버리고 base+subclass 만
 *     보기 위해서다.
 *  5) Command 레지스터의 Memory Space 비트를 끈다. 크기를 바꾸는 동안
 *     BAR 디코딩이 살아 있으면, 아직 정리되지 않은 주소 범위로 트랜잭션이
 *     흘러 들어와 시스템이 죽을 수 있다. 원래 값을 cmd 에 보관해 두었다가
 *     마지막에 그대로 복원한다.
 *  6) 기존 resourceN 파일들을 지운다. 크기가 바뀌면 파일 크기도 달라지므로,
 *     고쳐 쓰는 대신 지웠다 다시 만드는 편이 안전하다.
 *  7) pci_resize_resource() 로 실제 크기를 바꾸고,
 *     pci_assign_unassigned_bus_resources() 로 버스 전체의 자원을 다시
 *     배치한다. 커진 BAR 가 들어갈 자리를 만들려면 이웃 장치의 배치까지
 *     조정해야 할 수 있기 때문이다.
 *  8) 파일을 다시 만든다. 실패해도 경고만 남기고 진행하는데, 파일이 없는
 *     것이 장치 동작을 막지는 않기 때문이다.
 *  9) Command 레지스터를 원래대로 복원한다.
 *
 * 에러 경로: goto 레이블이 두 개다. pm_put 은 PM 만 놓고 락을 푸는 경로,
 * unlock 은 PM 을 잡기도 전에 실패해 락만 푸는 경로다. 레이블이 중첩
 * 순서대로 배치되어 있어 잡은 것의 역순으로 정확히 풀린다.
 * 주의: 크기 변경 실패(ret 이 0 이 아닌 경우)에도 8단계와 9단계는 그대로
 * 실행된다 — 파일을 다시 만들고 Command 를 복원해야 장치가 원래 상태로
 * 돌아오기 때문이다. ret 은 마지막 return 에서 반환값으로만 쓰인다.
 *
 * 실행 컨텍스트: write(2) 프로세스 문맥. 자원 재배치까지 포함해 오래
 * 걸리며 여러 번 잠든다.
 *
 * 호출 체인:
 *   write(2) -> resourceN_resize_store(매크로 생성) -> [__resource_resize_store]
 *     -> pci_resize_resource(drivers/pci/setup-res.c)
 *     -> pci_assign_unassigned_bus_resources
 */
static ssize_t __resource_resize_store(struct device *dev, int n, /* [한국어] 대상 device 와 BAR 번호 */
				       const char *buf, size_t count) /* [한국어] 사용자 입력(크기 지수)과 길이 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev 복원 */
	struct pci_bus *bus = pdev->bus; /* [한국어] 나중에 버스 단위 자원 재배치를 부르려고 미리 잡아 둔다 */
	unsigned long size; /* [한국어] 사용자가 요청한 크기 지수(비트 번호) */
	int ret; /* [한국어] 각 단계의 결과. 마지막 반환값으로 쓰인다 */
	u16 cmd; /* [한국어] Command 레지스터 원본 값. 작업이 끝나면 그대로 복원한다 */

	if (kstrtoul(buf, 0, &size) < 0) /* [한국어] 진법 자동 판별 파싱 */
		return -EINVAL; /* [한국어] 숫자가 아니면 거부. 아직 아무것도 잡지 않았으므로 곧바로 반환해도 된다 */

	device_lock(dev); /* [한국어] 드라이버 바인딩 상태를 보는 동안 바뀌지 않도록 잠근다 */
	if (dev->driver || pci_num_vf(pdev)) { /* [한국어] 커널 드라이버가 쓰고 있거나 SR-IOV VF 가 살아 있는가 */
		ret = -EBUSY; /* [한국어] 사용 중인 장치의 BAR 를 옮기면 기존 매핑이 전부 무효가 된다 */
		goto unlock; /* [한국어] 아직 PM 을 잡지 않았으므로 락만 풀면 된다 */
	} /* [한국어] 사용 중 검사 종료 */

	pci_config_pm_runtime_get(pdev); /* [한국어] 아래에서 설정공간을 여러 번 읽고 쓰므로 장치를 깨운다 */

	if ((pdev->class >> 8) == PCI_CLASS_DISPLAY_VGA) { /* [한국어] 8비트 시프트로 programming interface 를 버리고 base+subclass 만 비교. VGA 인가 */
		ret = aperture_remove_conflicting_pci_devices(pdev, /* [한국어] 이 BAR 를 물고 있는 부트 콘솔 프레임버퍼 드라이버를 먼저 쫓아낸다 */
						"resourceN_resize"); /* [한국어] 로그에 남을 요청자 이름 */
		if (ret) /* [한국어] 쫓아내지 못했다면 */
			goto pm_put; /* [한국어] PM 과 락을 순서대로 풀고 나간다 */
	} /* [한국어] VGA 특례 처리 종료 */

	pci_read_config_word(pdev, PCI_COMMAND, &cmd); /* [한국어] Command 레지스터 원본을 보관. 마지막에 그대로 복원하기 위해서다 */
	pci_write_config_word(pdev, PCI_COMMAND, /* [한국어] 크기를 바꾸는 동안 BAR 디코딩이 살아 있으면 정리되지 않은 주소로 트랜잭션이 들어와 위험하다 */
			      cmd & ~PCI_COMMAND_MEMORY); /* [한국어] Memory Space 비트만 지운다. 다른 비트(Bus Master 등)는 건드리지 않는다 */

	pci_remove_resource_files(pdev); /* [한국어] 크기가 바뀌면 파일 크기도 달라지므로, 고쳐 쓰지 않고 지웠다 다시 만든다 */

	ret = pci_resize_resource(pdev, n, size, 0); /* [한국어] Resizable BAR 레지스터에 새 크기를 쓴다. 실패해도 아래 복구 단계는 그대로 진행된다 */

	pci_assign_unassigned_bus_resources(bus); /* [한국어] 커진 BAR 가 들어갈 자리를 만들려면 이웃 장치의 배치까지 조정해야 할 수 있어, 버스 전체를 다시 배치한다 */

	if (pci_create_resource_files(pdev)) /* [한국어] 새 크기에 맞춰 파일을 다시 만든다 */
		pci_warn(pdev, "Failed to recreate resource files after BAR resizing\n"); /* [한국어] 실패해도 진행한다 — 파일이 없다고 장치가 못 도는 것은 아니다 */

	pci_write_config_word(pdev, PCI_COMMAND, cmd); /* [한국어] 보관해 둔 원본으로 Command 를 복원해 BAR 디코딩을 다시 켠다 */
pm_put: /* [한국어] PM 을 잡은 뒤 실패한 경로가 여기로 온다 */
	pci_config_pm_runtime_put(pdev); /* [한국어] PM 참조 해제 */
unlock: /* [한국어] PM 을 잡기도 전에 실패한 경로가 여기로 온다 */
	device_unlock(dev); /* [한국어] 잡은 것의 역순으로 정확히 풀린다 */

	return ret ? ret : count; /* [한국어] 어느 단계든 실패했으면 그 오류를, 전부 성공했으면 소비한 길이를 반환 */
} /* [한국어] 함수 본문 종료 */

/* [한국어] pci_dev_resource_resize_attr(n) - BAR 하나에 대한 resize 속성 한 벌을
 * 통째로 찍어내는 매크로.
 *
 * @n: BAR 번호(0~5). 토큰 붙이기(##)로 함수 이름과 파일 이름을 만든다.
 *
 * 한 번의 전개가 만드는 것:
 *   (1) resource<n>_resize_show()  — __resource_resize_show(dev, n, buf) 호출
 *   (2) resource<n>_resize_store() — __resource_resize_store(dev, n, buf, count) 호출
 *   (3) static DEVICE_ATTR_RW(resource<n>_resize) — 위 둘을 읽기+쓰기 속성
 *       dev_attr_resource<n>_resize 로 묶는다.
 *
 * 왜 이런 구조인가: sysfs 의 show/store 콜백 시그니처에는 "몇 번 BAR 인가" 를
 * 넘길 자리가 없다. 바이너리 속성이라면 private 필드에 숫자를 심을 수
 * 있지만(pci_create_attr 이 그렇게 한다), 일반 속성에는 그런 자리가 없다.
 * 그래서 BAR 번호를 컴파일 시점에 박아 넣은 함수를 여섯 벌 만든다.
 * 본체는 __resource_resize_show/store 한 벌뿐이라 로직이 중복되지는 않는다.
 *
 * 만들어지는 파일: /sys/bus/pci/devices/<BDF>/resource0_resize ~ resource5_resize.
 * 읽으면 가능한 크기 비트맵, 쓰면 실제 크기 변경이다. 다만 아래
 * resource_resize_is_visible() 이 Resizable BAR 를 지원하지 않는 BAR 에는
 * 파일을 만들지 않으므로, 대부분의 장치에서는 이 파일들이 보이지 않는다.
 *
 * 매크로 문법 주의: 각 줄이 백슬래시로 끝나야 이어진다. 그래서 설명은
 * 백슬래시 앞에만 둘 수 있다. */
#define pci_dev_resource_resize_attr(n) /* [한국어] BAR n 용 show/store/속성 한 벌 */ \
static ssize_t resource##n##_resize_show(struct device *dev, /* [한국어] ## 로 이름 생성 */ \
					 struct device_attribute *attr, /* [한국어] 속성 서술자 — 본체로 넘기지 않는다 */ \
					 char *buf) /* [한국어] sysfs 출력 버퍼 */ \
{ /* [한국어] 매크로 본문 시작 */ \
	return __resource_resize_show(dev, n, buf); /* [한국어] BAR 번호를 컴파일 시점 상수로 얹어 공통 본체에 위임 */ \
} /* [한국어] 매크로 본문 종료 */ \
static ssize_t resource##n##_resize_store(struct device *dev, /* [한국어] ## 로 store 판 생성 */ \
					  struct device_attribute *attr, /* [한국어] 속성 서술자 — 본체로 넘기지 않는다 */ \
					  const char *buf, size_t count) /* [한국어] 사용자 입력과 길이 */ \
{ /* [한국어] 매크로 본문 시작 */ \
	return __resource_resize_store(dev, n, buf, count); /* [한국어] BAR 번호를 얹어 공통 본체에 위임 */ \
} /* [한국어] 매크로 본문 종료 */ \
static DEVICE_ATTR_RW(resource##n##_resize) /* [한국어] dev_attr_resource<n>_resize 생성. 세미콜론은 사용처에서 붙인다 */

pci_dev_resource_resize_attr(0); /* [한국어] /sys/.../resource0_resize — BAR0 의 크기 조회/변경 */
pci_dev_resource_resize_attr(1); /* [한국어] /sys/.../resource1_resize — BAR1 */
pci_dev_resource_resize_attr(2); /* [한국어] /sys/.../resource2_resize — BAR2 */
pci_dev_resource_resize_attr(3); /* [한국어] /sys/.../resource3_resize — BAR3 */
pci_dev_resource_resize_attr(4); /* [한국어] /sys/.../resource4_resize — BAR4 */
pci_dev_resource_resize_attr(5); /* [한국어] /sys/.../resource5_resize — BAR5 */

/* [한국어] resource_resize_attrs - 여섯 개 resize 속성을 모은 배열.
 * 배열에는 여섯 개가 다 들어 있지만, 아래 is_visible 이 BAR 마다 따로
 * 판정하므로 실제로 만들어지는 파일은 그중 일부이거나 하나도 없을 수 있다.
 * 배열 순서가 곧 is_visible 에 넘어가는 인덱스 n 이 되고, 그 n 이 다시
 * BAR 번호로 쓰인다 — 그래서 이 배열의 순서를 바꾸면 안 된다. */
static struct attribute *resource_resize_attrs[] = { /* [한국어] 배열 시작 */
	&dev_attr_resource0_resize.attr, /* [한국어] 인덱스 0 = BAR0 */
	&dev_attr_resource1_resize.attr, /* [한국어] 인덱스 1 = BAR1 */
	&dev_attr_resource2_resize.attr, /* [한국어] 인덱스 2 = BAR2 */
	&dev_attr_resource3_resize.attr, /* [한국어] 인덱스 3 = BAR3 */
	&dev_attr_resource4_resize.attr, /* [한국어] 인덱스 4 = BAR4 */
	&dev_attr_resource5_resize.attr, /* [한국어] 인덱스 5 = BAR5 */
	NULL, /* [한국어] 끝 센티널 */
}; /* [한국어] 배열 종료 */

/*
 * [한국어]
 * resource_resize_is_visible - resourceN_resize 파일을 만들지 결정하는 콜백
 *
 * @kobj: 대상 kobject. pci_dev 로 복원한다.
 * @a: 가시성을 묻는 속성.
 * @n: 그룹 배열 안에서의 인덱스. 여기서는 이것이 곧 BAR 번호로 쓰인다.
 * @return: 0 이면 파일을 만들지 않고, 아니면 속성의 원래 모드.
 *
 * 이 콜백이 특이한 점은 인덱스 n 을 그대로 BAR 번호로 쓴다는 것이다.
 * resource_resize_attrs[] 가 BAR0..BAR5 순서로 정확히 배열되어 있기 때문에
 * 성립하는 규약이며, 그래서 그 배열의 순서를 바꾸면 엉뚱한 BAR 를 검사하게
 * 된다.
 *
 * 판정: pci_rebar_get_current_size() 가 음수를 돌려주면 그 BAR 에는
 * Resizable BAR capability 가 없다는 뜻이므로 파일을 만들지 않는다.
 * 있으면 속성의 원래 모드를 그대로 돌려준다.
 *
 * 실행 컨텍스트: sysfs 그룹 생성 시점의 프로세스 문맥. 여기서는 런타임 PM 을
 * 잡지 않는데, 이 시점에는 장치가 아직 절전으로 내려가기 전이라는 전제가
 * 깔린 것으로 보인다 — 그 근거를 밝히는 상류 주석은 없다.
 *
 * 호출 체인:
 *   장치 등록 -> sysfs 그룹 생성 -> [resource_resize_is_visible]
 *     -> pci_rebar_get_current_size (drivers/pci/pci.h 에 선언)
 */
static umode_t resource_resize_is_visible(struct kobject *kobj, /* [한국어] 대상 kobject */
					  struct attribute *a, int n) /* [한국어] 속성과 배열 인덱스 — 인덱스가 곧 BAR 번호다 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* [한국어] kobject -> device -> pci_dev 복원 */

	return pci_rebar_get_current_size(pdev, n) < 0 ? 0 : a->mode; /* [한국어] 음수 = 이 BAR 에 Resizable BAR capability 가 없다 -> 파일을 만들지 않는다. 있으면 원래 모드 그대로 */
} /* [한국어] 함수 본문 종료 */

/* [한국어] pci_dev_resource_resize_group - resourceN_resize 파일들을 만드는 그룹.
 * pci_dev_groups[] 를 거쳐 모든 PCI 장치에 시도되지만, is_visible 이 BAR
 * 마다 따로 판정하므로 실제 파일은 Resizable BAR 를 가진 BAR 에만 생긴다. */
static const struct attribute_group pci_dev_resource_resize_group = { /* [한국어] 구조체 초기화 시작 */
	.attrs = resource_resize_attrs, /* [한국어] 이 그룹이 만들 속성 목록(여섯 개).
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: 드라이버 코어의 sysfs 그룹 생성 코드.
		 * 값 범위: NULL 로 끝나는 struct attribute 포인터 배열.
		 * 동기화: const 정적 데이터라 락 불필요.
		 * 배열 순서가 BAR 번호와 일치해야 아래 is_visible 이 성립한다. */
	.is_visible = resource_resize_is_visible, /* [한국어] BAR 마다 Resizable BAR 지원 여부로 파일을 가리는 콜백.
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: sysfs 코어가 그룹을 만들 때 속성마다 한 번씩(인덱스를 함께 넘긴다).
		 * 값 범위: 함수 포인터. 0 을 돌려주면 그 파일은 생기지 않는다.
		 * 동기화: 콜백이 락 없이 설정공간을 조회한다. */
}; /* [한국어] 구조체 초기화 종료 */

/*
 * [한국어]
 * pci_create_sysfs_dev_files - 장치 하나에 대해 실행 중 만들어야 하는 sysfs 파일을 만든다
 *
 * @pdev: 대상 장치.
 * @return: 0 이면 성공, sysfs 가 아직 준비되지 않았으면 -EACCES,
 *   그 밖에는 pci_create_resource_files() 의 오류.
 *   __must_check 가 붙어 있어 반환값을 무시하면 컴파일 경고가 난다.
 *
 * 이 파일이 만드는 것 중 대부분(vendor, device, config, rom 등)은 정적
 * 속성 그룹으로 등록되어 드라이버 코어가 알아서 만들어 준다. 그러나
 * resourceN 과 resourceN_wc 는 개수가 장치마다 달라 그 방식으로 표현할 수
 * 없다. 그런 것들을 이 함수가 만든다.
 *
 * sysfs_initialized 검사: 부팅 아주 이른 시점에 발견된 장치는 아직 파일을
 * 만들 수 없다. 그때는 -EACCES 를 돌려주고, 나중에 late_initcall 인
 * pci_sysfs_init() 이 모든 장치를 다시 훑으며 만들어 준다.
 * 호출자인 drivers/pci/bus.c 의 pci_bus_add_device() 는 이 반환값을 특별히
 * 다루지 않는다(그 파일에서 확인) — 실패해도 장치 등록 자체는 계속된다.
 *
 * 호출자: drivers/pci/bus.c 의 pci_bus_add_device(), 그리고 아래
 * pci_sysfs_init() 의 일괄 생성 루프.
 *
 * NVMe 학습 관점: NVMe SSD 가 열거되어 pci_bus_add_device() 를 지날 때 이
 * 함수가 불려 resource0 파일이 만들어진다. 그 뒤에 nvme 드라이버의 프로브가
 * 이어진다.
 *
 * 실행 컨텍스트: 장치 등록 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_bus_add_device(drivers/pci/bus.c) -> [pci_create_sysfs_dev_files]
 *     -> pci_create_resource_files -> pci_create_attr
 */
int __must_check pci_create_sysfs_dev_files(struct pci_dev *pdev) /* [한국어] 이 장치의 동적 sysfs 파일(BAR 창)을 만든다 */
{ /* [한국어] 함수 본문 시작 */
	if (!sysfs_initialized) /* [한국어] 아직 pci_sysfs_init() 이 실행되지 않았는가 */
		return -EACCES; /* [한국어] 지금은 만들 수 없다. 나중에 pci_sysfs_init() 이 일괄로 만들어 준다 */

	return pci_create_resource_files(pdev); /* [한국어] 실제 작업은 전부 여기에. BAR 별 resourceN, resourceN_wc 를 만든다 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * pci_remove_sysfs_dev_files - 장치의 동적 sysfs 파일을 정리한다
 *
 * @pdev: 대상 장치.
 * @return: 없음.
 *
 * pci_create_sysfs_dev_files() 의 짝이다. drivers/pci/remove.c 의
 * pci_stop_dev() 가 부른다(그 파일에서 확인) — 드라이버를 언바인드한 직후,
 * /proc 진입점을 지우는 것과 나란히 놓여 있다.
 *
 * sysfs_initialized 검사를 여기서도 하는 이유: 초기화 전에 등록됐다 제거되는
 * 장치는 애초에 파일이 만들어지지 않았으므로 지울 것도 없다. 검사를
 * 생략해도 pci_remove_resource_files() 가 NULL 포인터를 건너뛰어 안전하지만,
 * 생성 쪽과 대칭을 맞춰 의도를 분명히 한 것이다.
 *
 * 실행 컨텍스트: 장치 제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_stop_dev(drivers/pci/remove.c) -> [pci_remove_sysfs_dev_files]
 *     -> pci_remove_resource_files
 */
/**
 * pci_remove_sysfs_dev_files - cleanup PCI specific sysfs files
 * @pdev: device whose entries we should free
 *
 * Cleanup when @pdev is removed from sysfs.
 */
void pci_remove_sysfs_dev_files(struct pci_dev *pdev) /* [한국어] 이 장치의 동적 sysfs 파일을 정리한다 */
{ /* [한국어] 함수 본문 시작 */
	if (!sysfs_initialized) /* [한국어] 초기화 전이었다면 만들어진 파일도 없다 */
		return; /* [한국어] 지울 것이 없으므로 그냥 돌아간다 */

	pci_remove_resource_files(pdev); /* [한국어] resourceN, resourceN_wc 를 전부 지우고 메모리를 해제한다 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * pci_sysfs_init - PCI sysfs 지원을 켜고, 이미 등록된 것들을 뒤늦게 채운다
 *
 * @return: 0 이면 성공, 파일 생성에 실패하면 그 오류.
 *
 * late_initcall 로 등록되어 부팅의 늦은 단계에 딱 한 번 실행된다.
 * 하는 일이 두 가지다.
 *  1) sysfs_initialized 를 1 로 만들어, 이후 등록되는 장치들은 그때그때
 *     파일을 만들 수 있게 한다.
 *  2) 그 플래그가 0 이던 동안 등록되어 파일을 못 만든 장치와 버스를 전부
 *     훑으며 뒤늦게 만들어 준다. 이 "따라잡기" 가 이 함수의 핵심이다.
 *
 * 왜 late_initcall 인가: sysfs 자체와 PCI 코어가 모두 올라온 뒤여야 파일을
 * 만들 수 있다. 그런데 PCI 열거는 그보다 훨씬 이른 단계에서 시작되므로,
 * 초기 장치들은 어쩔 수 없이 나중에 채워 넣는 구조가 된다.
 *
 * 참조 관리 — 이 함수에서 가장 주의할 점:
 * for_each_pci_dev(pdev) 는 pci_get_device() 를 반복 호출하는 순회다.
 * drivers/pci/search.c 의 상류 kernel-doc 이 규약을 명시한다 — 장치를
 * 찾으면 그 장치의 참조 계수를 올리고, 인자로 준 이전 장치의 참조 계수는
 * 항상 내린다. 따라서 루프가 정상적으로 끝까지 돌면 마지막에 NULL 을
 * 돌려주면서 참조가 모두 정리된다. 그러나 중간에 빠져나가면 지금 손에
 * 들고 있는 pdev 의 참조가 남는다. 그래서 오류 경로에서 pci_dev_put(pdev)
 * 를 명시적으로 부른다. 이것을 빠뜨리면 그 장치는 영원히 해제되지 않는다.
 * 정상 종료 경로에는 put 이 없는 것이 옳다 — 루프가 이미 정리했기 때문이다.
 * (for_each_pci_dev 매크로 자체의 정의는 include/linux/pci.h 에 있고 이
 * 트리에는 그 헤더가 없어 원문 확인은 불가능하다. 위 설명은 search.c 의
 * pci_get_device kernel-doc 과 이 코드의 pci_dev_put 배치를 근거로 한 것이다.)
 *
 * 버스 순회는 참조를 잡지 않는 pci_find_next_bus() 를 쓰므로 대칭되는
 * put 이 없다.
 *
 * NVMe 학습 관점: 부팅 시점에 이미 꽂혀 있던 NVMe SSD 는 이 함수가 실행되기
 * 전에 열거되었을 수 있다. 그런 장치의 resource0 파일은 여기서 만들어진다.
 * 반대로 부팅 후 hot-add 된 SSD 는 pci_bus_add_device() 경로에서 바로
 * 만들어진다. 결과적으로 두 경우 모두 같은 파일이 생긴다.
 *
 * 실행 컨텍스트: 부팅 후반의 initcall 문맥(프로세스 문맥). 한 번만 실행된다.
 *
 * 호출 체인:
 *   late_initcall -> [pci_sysfs_init]
 *     -> pci_create_sysfs_dev_files (장치마다)
 *     -> pci_create_legacy_files (버스마다)
 */
static int __init pci_sysfs_init(void) /* [한국어] __init 이라 부팅이 끝나면 이 코드는 메모리에서 해제된다 */
{ /* [한국어] 함수 본문 시작 */
	struct pci_dev *pdev = NULL; /* [한국어] 장치 순회 커서. NULL 로 시작해야 "처음부터" 라는 뜻이 된다 */
	struct pci_bus *pbus = NULL; /* [한국어] 버스 순회 커서. 마찬가지로 NULL 로 시작 */
	int retval; /* [한국어] 파일 생성 결과 */

	sysfs_initialized = 1; /* [한국어] 이 줄부터 pci_create_sysfs_dev_files 가 -EACCES 를 돌려주지 않는다. 아래 루프보다 먼저 세워야 하는 이유가 그것이다 */
	for_each_pci_dev(pdev) { /* [한국어] 이미 등록된 모든 장치를 훑는다. 이 순회는 각 장치의 참조 계수를 올리고 이전 것을 내린다(drivers/pci/search.c 의 pci_get_device kernel-doc 규약) */
		retval = pci_create_sysfs_dev_files(pdev); /* [한국어] 플래그가 0 이던 동안 못 만든 파일을 지금 만든다 */
		if (retval) { /* [한국어] 생성 실패 */
			pci_dev_put(pdev); /* [한국어] 루프를 중간에 빠져나가므로 지금 들고 있는 참조를 손으로 놓아야 한다. 이 줄이 없으면 이 장치는 영원히 해제되지 않는다 */
			return retval; /* [한국어] initcall 실패로 보고. 나머지 장치는 파일 없이 남는다 */
		} /* [한국어] 실패 처리 종료 */
	} /* [한국어] 장치 루프 종료 — 끝까지 돌면 순회가 참조를 모두 정리하므로 여기서는 put 이 필요 없다 */

	while ((pbus = pci_find_next_bus(pbus))) /* [한국어] 이번에는 버스를 훑는다. 이 순회는 참조를 잡지 않아 대칭되는 put 이 없다 */
		pci_create_legacy_files(pbus); /* [한국어] legacy_io/legacy_mem 을 뒤늦게 만든다. HAVE_PCI_LEGACY 가 없으면 이 함수 자체가 없다 */

	return 0; /* [한국어] 모든 따라잡기 성공 */
} /* [한국어] 함수 본문 종료 */
late_initcall(pci_sysfs_init); /* [한국어] 부팅 후반에 한 번 실행되도록 등록. sysfs 와 PCI 코어가 모두 올라온 뒤여야 파일을 만들 수 있기 때문이다 */

/* [한국어] pci_dev_dev_attrs - 장치마다 있을 수도 없을 수도 있는 두 속성.
 * 아래 pci_dev_attr_group 에 담기고, 그 is_visible 인
 * pci_dev_attrs_are_visible() 이 속성별로 따로 판정한다. 두 속성의 조건이
 * 서로 다르기 때문에(VGA 인가 / DSN 이 있는가) 하나의 그룹 조건으로는
 * 표현할 수 없고, 콜백 안에서 속성 포인터를 비교해 갈라야 한다.
 * 이 그룹은 pci_dev_attr_groups[] 를 거쳐 probe.c 의 device_type
 * pci_dev_type 에 등록된다(그 파일에서 확인). */
static struct attribute *pci_dev_dev_attrs[] = { /* [한국어] 배열 시작 */
	&dev_attr_boot_vga.attr, /* [한국어] boot_vga — 이 장치가 부팅 화면을 담당한 VGA 인가. VGA 장치에만 생긴다 */
	&dev_attr_serial_number.attr, /* [한국어] serial_number — PCIe DSN. DSN capability 가 있는 장치에만 생기고, 관리자만 읽는다 */
	NULL, /* [한국어] 끝 센티널 */
}; /* [한국어] 배열 종료 */

/*
 * [한국어]
 * pci_dev_attrs_are_visible - boot_vga 와 serial_number 의 가시성을 각각 판정
 *
 * @kobj: 대상 kobject. pci_dev 로 복원한다.
 * @a: 지금 판정 중인 속성. 이 콜백에서는 이 포인터를 비교하는 것이 핵심이다.
 * @n: 그룹 안 인덱스(쓰지 않는다).
 * @return: 0 이면 파일을 만들지 않고, 아니면 속성의 원래 모드.
 *
 * 앞서 본 is_visible 콜백들(reset, bridge, pcie)은 그룹 전체에 같은 조건을
 * 적용했다. 이 콜백은 다르다 — 두 속성의 조건이 서로 무관하므로,
 * a 를 각 속성의 주소와 비교해 어느 것을 묻고 있는지 먼저 가려낸다.
 * 이것이 한 그룹에 조건이 다른 속성을 섞어 넣는 표준적인 방법이다.
 *
 * 구조에 주목할 점: 두 if 문 모두 "조건이 맞으면 모드를 돌려주고" 끝에서
 * 0 을 돌려주는 형태다. 즉 기본값이 "보이지 않음" 이다. 새 속성을 배열에
 * 추가하고 여기에 조건을 쓰는 것을 잊으면 그 파일은 조용히 만들어지지
 * 않는다 — 잘못 노출되는 것보다는 안전한 기본값이다.
 *
 * 두 조건:
 *  - boot_vga: pci_is_vga(pdev) — VGA 클래스 장치인가.
 *  - serial_number: pci_get_dsn(pdev) 가 0 이 아닌가 — DSN capability 가
 *    있고 값이 유효한가. 이 호출은 실제로 확장 설정공간을 읽으므로,
 *    가시성 판정 한 번에 하드웨어 접근이 따라붙는다.
 *
 * NVMe 학습 관점: NVMe SSD 는 VGA 가 아니므로 boot_vga 는 생기지 않는다.
 * serial_number 는 그 SSD 가 DSN capability 를 구현했는지에 따라 갈린다.
 * 생기더라도 그 값은 PCIe 계층의 시리얼이지 nvme id-ctrl 의 SN 이 아니다.
 *
 * 실행 컨텍스트: sysfs 그룹 생성 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   장치 등록 -> sysfs 그룹 생성 -> [pci_dev_attrs_are_visible]
 *     -> pci_is_vga / pci_get_dsn
 */
static umode_t pci_dev_attrs_are_visible(struct kobject *kobj, /* [한국어] 대상 kobject */
					 struct attribute *a, int n) /* [한국어] 판정 중인 속성과 인덱스(미사용) */
{ /* [한국어] 함수 본문 시작 */
	struct device *dev = kobj_to_dev(kobj); /* [한국어] kobject -> device. 아래에서 한 번 더 변환하므로 중간 변수를 둔다 */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev */

	if (a == &dev_attr_boot_vga.attr && pci_is_vga(pdev)) /* [한국어] 지금 묻는 것이 boot_vga 이고, 이 장치가 VGA 클래스인가. 포인터 비교로 속성을 식별한다 */
		return a->mode; /* [한국어] 조건 충족 — 원래 모드로 파일을 만든다 */

	if (a == &dev_attr_serial_number.attr && pci_get_dsn(pdev)) /* [한국어] 지금 묻는 것이 serial_number 이고, DSN 이 0 이 아닌가. 이 호출은 실제로 확장 설정공간을 읽는다 */
		return a->mode; /* [한국어] 조건 충족 — 파일을 만든다 */

	return 0; /* [한국어] 기본값은 "보이지 않음". 새 속성을 추가하고 조건을 빠뜨리면 그 파일은 생기지 않는다 */
} /* [한국어] 함수 본문 종료 */

/* [한국어] pci_dev_hp_attrs - hotplug 조작용 속성 두 개.
 * 아래 pci_dev_hp_attr_group 에 담기며, is_visible 이 SR-IOV VF 를 걸러 낸다.
 * 두 파일 모두 __ATTR / DEVICE_ATTR_IGNORE_LOCKDEP 로 파일 이름과 C 심볼
 * 이름을 분리해 만든 것들이다. */
static struct attribute *pci_dev_hp_attrs[] = { /* [한국어] 배열 시작 */
	&dev_attr_remove.attr, /* [한국어] remove — 이 장치를 커널에서 논리적으로 뽑는다(쓰기 전용 0220) */
	&dev_attr_dev_rescan.attr, /* [한국어] rescan — 이 장치가 앉은 버스를 다시 열거한다(쓰기 전용 0200) */
	NULL, /* [한국어] 끝 센티널 */
}; /* [한국어] 배열 종료 */

/*
 * [한국어]
 * pci_dev_hp_attrs_are_visible - remove 와 rescan 파일의 가시성 판정
 *
 * @kobj: 대상 kobject. pci_dev 로 복원한다.
 * @a: 판정 중인 속성.
 * @n: 그룹 안 인덱스(미사용).
 * @return: VF 이면 0, 아니면 속성의 원래 모드.
 *
 * 판정은 하나뿐이다 — SR-IOV Virtual Function 에는 이 두 파일을 만들지
 * 않는다. VF 는 물리 장치가 아니라 PF 가 만들어 낸 가상 함수라서, 개별
 * 제거나 재열거의 대상이 아니기 때문이다. VF 의 개수는 PF 쪽의
 * sriov_numvfs 로 조절한다(그 속성은 이 파일이 아니라 다른 파일에 있고,
 * 아래 pci_dev_attr_groups[] 에 sriov_pf_dev_attr_group 으로 얹힌다).
 *
 * 위 pci_dev_attrs_are_visible 과 기본값이 반대라는 점에 유의: 이쪽은
 * "특별한 경우만 감추고 기본은 보인다" 이고, 저쪽은 "조건에 맞을 때만
 * 보이고 기본은 감춘다" 이다. 무엇이 안전한 기본값인가에 따라 갈린다.
 *
 * 실행 컨텍스트: sysfs 그룹 생성 시점의 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   장치 등록 -> sysfs 그룹 생성 -> [pci_dev_hp_attrs_are_visible]
 */
static umode_t pci_dev_hp_attrs_are_visible(struct kobject *kobj, /* [한국어] 대상 kobject */
					    struct attribute *a, int n) /* [한국어] 판정 중인 속성과 인덱스(미사용) */
{ /* [한국어] 함수 본문 시작 */
	struct device *dev = kobj_to_dev(kobj); /* [한국어] kobject -> device */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev */

	if (pdev->is_virtfn) /* [한국어] SR-IOV Virtual Function 인가 */
		return 0; /* [한국어] VF 는 개별 제거/재열거 대상이 아니므로 두 파일을 만들지 않는다 */

	return a->mode; /* [한국어] 그 밖의 모든 장치에는 원래 모드 그대로 만든다 — 기본값이 "보임" 이다 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * pci_bridge_attrs_are_visible - 브리지 전용 속성들의 가시성 판정
 *
 * @kobj: 대상 kobject. pci_dev 로 복원한다.
 * @a: 판정 중인 속성.
 * @n: 그룹 안 인덱스(미사용).
 * @return: 브리지면 속성의 원래 모드, 아니면 0.
 *
 * pci_bridge_attrs[] 의 세 파일(subordinate_bus_number,
 * secondary_bus_number, reset_subordinate)에 공통으로 적용된다. 셋 다
 * "하위 버스를 거느린 장치" 에만 의미가 있으므로 조건이 하나로 충분하다.
 *
 * 이 콜백 덕분에 secondary_bus_number_show() 같은 함수가 "이 장치가 정말
 * 브리지인가" 를 다시 검사하지 않아도 된다. 가시성 콜백이 문지기 역할을
 * 하는 전형적인 예다.
 *
 * NVMe 학습 관점: NVMe SSD 는 엔드포인트라 이 세 파일이 없다. 대신 SSD 의
 * 부모 디렉터리(루트 포트나 스위치 포트)에 가면 있다.
 *
 * 실행 컨텍스트: sysfs 그룹 생성 시점의 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   장치 등록 -> sysfs 그룹 생성 -> [pci_bridge_attrs_are_visible]
 *     -> pci_is_bridge
 */
static umode_t pci_bridge_attrs_are_visible(struct kobject *kobj, /* [한국어] 대상 kobject */
					    struct attribute *a, int n) /* [한국어] 판정 중인 속성과 인덱스(미사용) */
{ /* [한국어] 함수 본문 시작 */
	struct device *dev = kobj_to_dev(kobj); /* [한국어] kobject -> device */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev */

	if (pci_is_bridge(pdev)) /* [한국어] 헤더 타입이 브리지인가(하위 버스를 거느리는 장치인가) */
		return a->mode; /* [한국어] 브리지에만 세 파일을 만든다 */

	return 0; /* [한국어] 엔드포인트에는 만들지 않는다 — 그래서 각 show 함수가 브리지 여부를 다시 검사하지 않아도 된다 */
} /* [한국어] 함수 본문 종료 */

/*
 * [한국어]
 * pcie_dev_attrs_are_visible - PCIe 링크 속성들의 가시성 판정
 *
 * @kobj: 대상 kobject. pci_dev 로 복원한다.
 * @a: 판정 중인 속성.
 * @n: 그룹 안 인덱스(미사용).
 * @return: PCIe 장치면 속성의 원래 모드, 아니면 0.
 *
 * pcie_dev_attrs[] 의 네 파일(current_link_speed, current_link_width,
 * max_link_width, max_link_speed)에 공통 적용된다. 넷 다 PCIe Capability
 * 레지스터를 읽어야 하므로, 그것이 없는 구형 병렬 PCI 장치에는 파일을
 * 만들지 않는다.
 *
 * 이 판정이 있기 때문에 current_link_speed_show() 등이 곧바로
 * pcie_capability_read_word() 를 부를 수 있다. 다만 그 함수들도 읽기 실패
 * 시 -EINVAL 을 돌려주는 방어를 갖고 있는데, 파일이 만들어진 뒤 장치가
 * 사라지는 경우가 있기 때문이다 — 가시성 판정은 생성 시점에 한 번뿐이다.
 *
 * NVMe 학습 관점: NVMe SSD 는 정의상 PCIe 장치이므로 이 네 파일이 항상
 * 있다. 성능 진단에서 가장 먼저 읽는 파일들이다.
 *
 * 실행 컨텍스트: sysfs 그룹 생성 시점의 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   장치 등록 -> sysfs 그룹 생성 -> [pcie_dev_attrs_are_visible]
 *     -> pci_is_pcie
 */
static umode_t pcie_dev_attrs_are_visible(struct kobject *kobj, /* [한국어] 대상 kobject */
					  struct attribute *a, int n) /* [한국어] 판정 중인 속성과 인덱스(미사용) */
{ /* [한국어] 함수 본문 시작 */
	struct device *dev = kobj_to_dev(kobj); /* [한국어] kobject -> device */
	struct pci_dev *pdev = to_pci_dev(dev); /* [한국어] device -> pci_dev */

	if (pci_is_pcie(pdev)) /* [한국어] PCIe Capability 를 가진 장치인가 */
		return a->mode; /* [한국어] PCIe 장치에만 링크 관련 네 파일을 만든다 */

	return 0; /* [한국어] 구형 병렬 PCI 장치에는 링크 개념이 없어 만들지 않는다 */
} /* [한국어] 함수 본문 종료 */

/* [한국어] pci_dev_group - 모든 PCI 장치에 조건 없이 붙는 기본 속성 그룹.
 * is_visible 이 없어 pci_dev_attrs[] 의 모든 파일이 항상 만들어진다. */
static const struct attribute_group pci_dev_group = { /* [한국어] 구조체 초기화 시작 */
	.attrs = pci_dev_attrs, /* [한국어] vendor/device/class/resource/enable 등 기본 속성 목록.
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: 드라이버 코어의 sysfs 그룹 생성 코드.
		 * 값 범위: NULL 로 끝나는 struct attribute 포인터 배열.
		 * 동기화: const 정적 데이터라 락 불필요.
		 * is_visible 을 두지 않은 것은 이 파일들이 장치 종류를 가리지 않고
		 * 항상 의미가 있기 때문이다. */
}; /* [한국어] 구조체 초기화 종료 */

/* [한국어] pci_dev_groups - PCI 버스 타입에 등록되는 최상위 그룹 배열.
 * drivers/pci/pci-driver.c 의 pci_bus_type 이 .dev_groups = pci_dev_groups
 * 로 가져간다(그 파일에서 확인). 그래서 static 이 아니다.
 * 여기 나열된 그룹들이 모든 PCI 장치에 시도되며, 각 그룹의 is_visible /
 * is_bin_visible 이 장치마다 실제로 만들 파일을 걸러 낸다.
 * 다른 파일에 정의된 그룹(VPD, SMBIOS, ACPI)도 이름만 얹혀 함께 등록되는데,
 * 그 extern 선언은 drivers/pci/pci.h 에 있다(그 파일에서 확인).
 * 설정자: 이 정적 초기화뿐. 읽는 자: 드라이버 코어. 동기화: 불필요. */
const struct attribute_group *pci_dev_groups[] = { /* [한국어] 배열 시작 */
	&pci_dev_group, /* [한국어] 기본 속성들 — vendor, device, class, resource, enable, irq 등 */
	&pci_dev_config_attr_group, /* [한국어] config 바이너리 파일 — 설정공간 전체 덤프. lspci 가 읽는 곳 */
	&pci_dev_rom_attr_group, /* [한국어] rom 바이너리 파일 — ROM 이 있는 장치에만 생긴다 */
	&pci_dev_reset_attr_group, /* [한국어] reset — 리셋을 지원하는 장치에만 */
	&pci_dev_reset_method_attr_group, /* [한국어] reset_method — 리셋 방법 목록. 위와 같은 조건 */
	&pci_dev_vpd_attr_group, /* [한국어] vpd — Vital Product Data. 정의는 다른 파일이고 선언은 drivers/pci/pci.h */
#ifdef CONFIG_DMI /* [한국어] DMI(SMBIOS) 지원이 있을 때만 */
	&pci_dev_smbios_attr_group, /* [한국어] label, index 등 펌웨어가 알려 주는 슬롯 이름. 선언은 drivers/pci/pci.h */
#endif /* [한국어] CONFIG_DMI 끝 */
#ifdef CONFIG_ACPI /* [한국어] ACPI 펌웨어를 쓰는 플랫폼에서만 */
	&pci_dev_acpi_attr_group, /* [한국어] ACPI 경로 등 펌웨어 관련 속성. 선언은 drivers/pci/pci.h */
#endif /* [한국어] CONFIG_ACPI 끝 */
	&pci_dev_resource_resize_group, /* [한국어] resourceN_resize — Resizable BAR 를 가진 BAR 에만 */
	ARCH_PCI_DEV_GROUPS /* [한국어] 아키텍처가 끼워 넣는 자리. 파일 위쪽에서 빈 매크로로 정의되면 아무것도 전개되지 않는다. 그래서 이 줄 끝에 쉼표가 없다 — 쉼표까지 매크로 쪽이 갖고 있어야 빈 전개가 성립한다 */
	NULL, /* [한국어] 끝 센티널 */
}; /* [한국어] 배열 종료 */

/* [한국어] pci_dev_hp_attr_group - hotplug 조작 파일(remove, rescan)의 그룹.
 * VF 에는 만들어지지 않는다. */
static const struct attribute_group pci_dev_hp_attr_group = { /* [한국어] 구조체 초기화 시작 */
	.attrs = pci_dev_hp_attrs, /* [한국어] remove 와 rescan 두 속성.
		 * 설정자: 이 정적 초기화뿐. 읽는 자: 드라이버 코어.
		 * 값 범위: NULL 로 끝나는 속성 포인터 배열. 동기화: 불필요. */
	.is_visible = pci_dev_hp_attrs_are_visible, /* [한국어] SR-IOV VF 를 걸러 내는 콜백.
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: sysfs 코어가 그룹을 만들 때 속성마다.
		 * 값 범위: 함수 포인터. VF 이면 0 을 돌려준다.
		 * 동기화: 콜백이 락 없이 pci_dev 플래그만 읽는다. */
}; /* [한국어] 구조체 초기화 종료 */

/* [한국어] pci_dev_attr_group - boot_vga 와 serial_number 의 그룹.
 * 두 속성의 조건이 서로 달라 콜백 안에서 속성별로 갈린다. */
static const struct attribute_group pci_dev_attr_group = { /* [한국어] 구조체 초기화 시작 */
	.attrs = pci_dev_dev_attrs, /* [한국어] boot_vga 와 serial_number 두 속성.
		 * 설정자: 이 정적 초기화뿐. 읽는 자: 드라이버 코어.
		 * 값 범위: NULL 로 끝나는 속성 포인터 배열. 동기화: 불필요. */
	.is_visible = pci_dev_attrs_are_visible, /* [한국어] 속성 포인터를 비교해 각각 다른 조건을 적용하는 콜백.
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: sysfs 코어가 그룹을 만들 때 속성마다.
		 * 값 범위: 함수 포인터. 기본값이 0(감춤)이라는 점에 유의.
		 * 동기화: serial_number 판정에서 설정공간을 읽는다. */
}; /* [한국어] 구조체 초기화 종료 */

/* [한국어] pci_bridge_attr_group - 브리지 전용 세 파일의 그룹. */
static const struct attribute_group pci_bridge_attr_group = { /* [한국어] 구조체 초기화 시작 */
	.attrs = pci_bridge_attrs, /* [한국어] subordinate/secondary_bus_number 와 reset_subordinate.
		 * 설정자: 이 정적 초기화뿐. 읽는 자: 드라이버 코어.
		 * 값 범위: NULL 로 끝나는 속성 포인터 배열. 동기화: 불필요. */
	.is_visible = pci_bridge_attrs_are_visible, /* [한국어] 브리지인지로 세 파일을 한꺼번에 가리는 콜백.
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: sysfs 코어가 그룹을 만들 때 속성마다.
		 * 값 범위: 함수 포인터. 엔드포인트면 0.
		 * 동기화: 락 없이 헤더 타입만 확인한다. */
}; /* [한국어] 구조체 초기화 종료 */

/* [한국어] pcie_dev_attr_group - PCIe 링크 상태 네 파일의 그룹.
 * NVMe SSD 에서 성능 진단 때 읽는 파일들이 여기 모여 있다. */
static const struct attribute_group pcie_dev_attr_group = { /* [한국어] 구조체 초기화 시작 */
	.attrs = pcie_dev_attrs, /* [한국어] current/max 의 link speed 와 width 네 속성.
		 * 설정자: 이 정적 초기화뿐. 읽는 자: 드라이버 코어.
		 * 값 범위: NULL 로 끝나는 속성 포인터 배열. 동기화: 불필요. */
	.is_visible = pcie_dev_attrs_are_visible, /* [한국어] PCIe 장치인지로 네 파일을 한꺼번에 가리는 콜백.
		 * 설정자: 이 정적 초기화뿐.
		 * 읽는 자: sysfs 코어가 그룹을 만들 때 속성마다.
		 * 값 범위: 함수 포인터. 구형 병렬 PCI 면 0.
		 * 동기화: 락 없이 PCIe Capability 유무만 확인한다. */
}; /* [한국어] 구조체 초기화 종료 */

/* [한국어] pci_dev_attr_groups - device_type 에 등록되는 최상위 그룹 배열.
 * drivers/pci/probe.c 의 device_type pci_dev_type 이 .groups = pci_dev_attr_groups
 * 로 가져간다(그 파일에서 확인). 위 pci_dev_groups[] 가 bus_type 경유인 것과
 * 등록 경로가 다르지만, 결과적으로 만들어지는 파일은 같은 장치 디렉터리에
 * 나란히 놓인다. 두 갈래로 나뉘어 있는 역사적 이유는 이 트리의 정보만으로는
 * 확인할 수 없다.
 * 여기에는 이 파일 밖에서 정의된 그룹이 특히 많다. 전부 drivers/pci/pci.h 에
 * extern 으로 선언되어 있으며, 각 기능의 CONFIG 가 켜졌을 때만 배열에 들어간다.
 * 설정자: 이 정적 초기화뿐. 읽는 자: 드라이버 코어. 동기화: 불필요. */
const struct attribute_group *pci_dev_attr_groups[] = { /* [한국어] 배열 시작 */
	&pci_dev_attr_group, /* [한국어] boot_vga, serial_number */
	&pci_dev_hp_attr_group, /* [한국어] remove, rescan (VF 제외) */
#ifdef CONFIG_PCI_IOV /* [한국어] SR-IOV 지원이 있을 때만 */
	&sriov_pf_dev_attr_group, /* [한국어] PF 쪽 속성(VF 개수 조절 등). 정의는 다른 파일, 선언은 drivers/pci/pci.h */
	&sriov_vf_dev_attr_group, /* [한국어] VF 쪽 속성. 정의는 다른 파일 */
#endif /* [한국어] CONFIG_PCI_IOV 끝 */
	&pci_bridge_attr_group, /* [한국어] 브리지 전용 버스 번호와 하위 버스 리셋 */
	&pcie_dev_attr_group, /* [한국어] PCIe 링크 속도/폭 — NVMe 진단의 출발점 */
#ifdef CONFIG_PCIEAER /* [한국어] PCIe 고급 오류 보고를 쓸 때만 */
	&aer_stats_attr_group, /* [한국어] AER 오류 통계. 정정 가능/불가능 오류 누적 횟수를 보여 준다. 선언은 drivers/pci/pci.h */
	&aer_attr_group, /* [한국어] AER 제어 속성. 정의는 다른 파일 */
#endif /* [한국어] CONFIG_PCIEAER 끝 */
#ifdef CONFIG_PCIEASPM /* [한국어] ASPM(링크 전력 관리)을 쓸 때만 */
	&aspm_ctrl_attr_group, /* [한국어] L0s/L1 등 링크 절전 상태의 허용 여부를 조절한다. 선언은 drivers/pci/pci.h */
#endif /* [한국어] CONFIG_PCIEASPM 끝 */
#ifdef CONFIG_PCI_DOE /* [한국어] Data Object Exchange 를 쓸 때만 */
	&pci_doe_sysfs_group, /* [한국어] DOE 프로토콜 목록 등. 선언은 drivers/pci/pci.h */
#endif /* [한국어] CONFIG_PCI_DOE 끝 */
#ifdef CONFIG_PCI_TSM /* [한국어] TEE 보안 관련 기능을 쓸 때만 */
	&pci_tsm_auth_attr_group, /* [한국어] 장치 인증 관련 속성. 선언은 drivers/pci/pci.h */
	&pci_tsm_attr_group, /* [한국어] TSM 제어 속성. 선언은 drivers/pci/pci.h */
#endif /* [한국어] CONFIG_PCI_TSM 끝 */
	NULL, /* [한국어] 끝 센티널 — 이 파일의 마지막 자료구조다 */
}; /* [한국어] 배열 종료 */
