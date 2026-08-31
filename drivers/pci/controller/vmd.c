// SPDX-License-Identifier: GPL-2.0
/*
 * Volume Management Device driver
 * Copyright (c) 2015, Intel Corporation.
 */

/*
 * [한국어 설명] 여러 PCIe 루트 포트를 하나의 엔드포인트 뒤에 숨기는 인텔 VMD (vmd.c)
 *
 * === 파일의 역할 ===
 * VMD(Volume Management Device)는 인텔 서버·클라이언트 칩셋의 기능으로,
 * 여러 PCIe 루트 포트를 하나의 PCI 엔드포인트 뒤로 감춘다. 호스트에서 보면
 * VMD 장치 하나만 보이고, 그 뒤에 별도의 PCI 도메인이 생겨 실제 장치들이
 * 거기 놓인다.
 *
 * 이 파일이 다른 drivers/pci/controller/ 드라이버와 결정적으로 다른 점이
 * 하나 있다. 나머지는 대개 platform_driver 로 등록되어 디바이스 트리나
 * ACPI 로부터 호스트 브리지 자원을 받아 오는데, 이 드라이버는
 * module_pci_driver(vmd_drv) 로 등록되는 **PCI 장치 드라이버**다. 즉
 * 자기 자신이 하나의 PCI 엔드포인트로 열거되어 pci-driver.c 의
 * pci_device_probe() 를 거쳐 바인딩된다. 그러고 나서 자기가 새로운 PCI
 * 도메인의 호스트가 된다. "PCI 장치인 동시에 PCI 호스트" 인 셈이다.
 *
 * 하는 일은 그 숨겨진 도메인을 커널에 되살리는 것이다.
 *   1) VMD 엔드포인트의 BAR 0(VMD_CFGBAR)을 매핑한다. 그 BAR 안이 곧
 *      숨겨진 도메인의 config space(ECAM 배치)다.
 *   2) 새 PCI 도메인 번호를 받아(pci.c 의 pci_bus_find_emul_domain_nr,
 *      0x10000 이상에서 고른다) 루트 버스를 만든다.
 *   3) config 접근 ops(vmd_ops)를 제공해, 그 도메인의 config 읽기·쓰기가
 *      VMD BAR 안의 해당 위치로 가게 한다.
 *   4) 인터럽트를 중계한다. 하위 장치들의 MSI-X 를 VMD 자신의 벡터로
 *      받아 다시 뿌린다.
 *
 * 4번이 이 드라이버의 가장 복잡한 부분이다. 하위 장치들이 각자 MSI-X 를
 * 쓰지만 실제로 CPU 에 닿는 벡터는 VMD 가 가진 것뿐이라, VMD 의 MSI
 * 도메인이 그 사이를 중계해야 한다. vmd_msi_domain_ops(alloc/free)와
 * vmd_irq_list, 그리고 인터럽트 핸들러 vmd_irq() 가 그 구현이다.
 * 다만 하드웨어가 VMD_FEAT_CAN_BYPASS_MSI_REMAP 을 지원하고 주소 변환이
 * 필요 없는 경우에는 이 중계를 아예 끄고 하위 장치가 상위 MSI 도메인을
 * 직접 쓰게 한다(vmd_enable_domain 의 else 분기).
 *
 * === 전체 아키텍처에서의 위치 ===
 * VMD 엔드포인트를 커널이 발견 (drivers/pci/probe.c)
 *   -> pci-driver.c 의 pci_bus_match() 가 vmd_ids 와 대조
 *   -> pci_device_probe() -> local_pci_probe() -> vmd_probe()
 *      -> vmd_enable_domain()
 *         -> BAR 매핑, 도메인 번호 획득, 자원 창 계산
 *         -> vmd_alloc_irqs() + vmd_create_irq_domain() (또는 중계 우회)
 *         -> pci_create_root_bus(&vmd_ops) 로 새 도메인의 루트 버스 생성
 *         -> pci_scan_child_bus() 로 숨겨진 도메인 열거
 *         -> pci_assign_unassigned_bus_resources() 로 자원 배정
 *         -> pci_bus_add_devices() -> 그 안의 장치들에 드라이버가 붙는다
 *
 * 실행 컨텍스트: probe/remove 는 프로세스 컨텍스트. vmd_irq() 는 하드 IRQ
 * (IRQF_NO_THREAD 로 등록되어 스레드화되지 않는다). config 접근은 어느
 * 컨텍스트에서든 불릴 수 있어 raw_spinlock_irqsave 로 보호한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci-driver.c 가 이 드라이버를 바인딩한다.
 * 아래쪽: probe.c 의 pci_scan_child_bus(열거), bus.c 의 pci_bus_add_devices,
 *   setup-bus.c 의 pci_assign_unassigned_bus_resources(자원 배정),
 *   pci.c 의 pci_bus_find_emul_domain_nr(도메인 번호 할당)와
 *   pci_reset_bus, msi/ 와 커널 irqdomain 코어(MSI 중계),
 *   pci-acpi 의 companion lookup 훅.
 * 공유 상태: struct vmd_dev 하나에 이 인스턴스의 모든 것이 들어 있고,
 *   드라이버 사설 데이터(pci_set_drvdata)로 매달린다. 그 안의
 *   struct pci_sysdata 가 새 버스의 sysdata 가 되어, vmd_from_bus() 가
 *   container_of 로 어느 버스든 그 VMD 인스턴스를 되찾는 통로가 된다.
 *   전역 상태는 인스턴스 번호를 주는 vmd_instance_ida, 모든 VMD 의 IRQ
 *   목록을 함께 보호하는 list_lock, ACPI 훅 설치 여부인 hook_installed 셋뿐이다.
 *
 * === NVMe 와의 관계 — 코드에 있는 것과 쓰임새에 있는 것 ===
 * 두 가지를 구분해야 한다.
 *
 * (가) 코드에 실제로 있는 것. 이 파일에서 NVMe 를 특정하는 코드는 딱 한
 *   군데다. vmd_next_irq() 안의
 *       switch (msi_desc_to_pci_dev(desc)->class) {
 *       case PCI_CLASS_STORAGE_EXPRESS:   <- NVMe 클래스 코드 0x0108
 *   가 그것이다. NVMe 컨트롤러에게는 부하가 가장 적은 전용 벡터를 골라
 *   주고, 그 밖의 모든 장치는 first_vec 하나를 함께 쓰게 한다. 원본
 *   주석이 이것을 "fast-interrupt handlers 의 화이트리스트" 라고 부른다.
 *   VMD 뒤에 붙는 것이 사실상 NVMe SSD 뿐이라는 전제 위에 선 최적화다.
 *
 * (나) 코드가 아니라 쓰임새에 있는 것. VMD 를 켜는 이유 자체가 NVMe
 *   관리다 — 핫플러그를 VMD 가 가로채 처리하므로 운영체제가 루트 포트를
 *   직접 다루지 않아도 드라이브를 갈아끼울 수 있고, 인텔 VROC 같은 RAID
 *   소프트웨어가 그 도메인을 통째로 관리한다. VMD 가 켜진 시스템에서
 *   NVMe SSD 는 도메인 0 이 아니라 이 드라이버가 만든 별도 도메인
 *   (0x10000 이상, lspci 에 "10000:00:00.0" 같은 주소로 보인다)에 나타난다.
 *   BIOS 에서 VMD 를 켰는데 설치 미디어에 이 모듈이 없으면 드라이브가
 *   아예 보이지 않는 문제가 바로 그것이다.
 *
 * 방향도 분명히 해 둔다. drivers/nvme/ 전체를 grep 해 보면 vmd 라는
 * 문자열이 한 번도 나오지 않는다. NVMe 드라이버는 이 파일의 어떤 함수도
 * 부르지 않는다. 관계는 언제나 이쪽에서 저쪽으로다 —
 * vmd_enable_domain() 의 pci_bus_add_devices() 가 도메인 안의 장치들에
 * 드라이버를 붙이고, 그 결과로 nvme_probe() 가 불린다.
 *
 * 한 가지 바로잡을 것. 이 자리에 있던 기존 주석은 "pci.c 의
 * pci_real_dma_dev() 를 덮어쓰는 것이 이 드라이버다" 라고 적어 두었는데
 * 사실이 아니다. 이 파일에는 pci_real_dma_dev 라는 정의가 없다.
 * 이 파일이 하는 일은 vmd_enable_domain() 에서 sd->vmd_dev = vmd->dev 로
 * pci_sysdata 에 "이 도메인의 진짜 DMA 주체" 를 적어 두는 것뿐이고,
 * 그 값을 읽어 pci_real_dma_dev() 를 재정의하는 코드는 아키텍처 쪽에 있다
 * (drivers/pci/pci.c:12970 의 정의가 __weak 인 것이 그 증거다). 그
 * 아키텍처 파일은 이 스파스 체크아웃에 없어 직접 확인하지는 못했다.
 *
 * === 주요 함수/구조체 요약 ===
 * vmd_probe()             : VMD 엔드포인트에 바인딩되어 인스턴스를 세운다.
 * vmd_enable_domain()     : 실제 작업 본체. 자원 창 계산부터 열거·바인딩까지.
 * vmd_pci_read() / vmd_pci_write() : 숨겨진 도메인의 config 접근.
 * vmd_cfg_addr()          : 버스·devfn·레지스터를 BAR 안 오프셋으로 바꾸는 계산.
 * vmd_attach_resources() / vmd_detach_resources() : VMD BAR 자원과
 *                           도메인 자원 창을 부모-자식으로 잇거나 끊는다.
 * vmd_msi_alloc() / vmd_msi_free() : 하위 장치의 MSI-X 를 VMD 벡터에 중계
 *                           (struct irq_domain_ops vmd_msi_domain_ops 의 두 슬롯).
 * vmd_next_irq()          : 어느 VMD 벡터에 실을지 고른다. NVMe 특례가 여기 있다.
 * vmd_irq()               : VMD 벡터 하나에 실린 하위 인터럽트들을 되뿌리는 핸들러.
 * vmd_irq_enable() / vmd_irq_disable() : 중계 목록에 넣고 빼기.
 * vmd_domain_reset()      : 열거 전에 도메인 안 브리지들의 창 레지스터를 정리.
 * struct vmd_dev          : 이 VMD 인스턴스의 모든 상태.
 * struct vmd_irq_list     : VMD 벡터 하나가 되뿌릴 하위 인터럽트 목록.
 * struct vmd_irq          : 그 목록의 항목 하나(하위 장치의 인터럽트 하나).
 */

#include <linux/device.h>	/* [한국어] struct device, devm_* 관리 할당(devm_kzalloc/devm_kcalloc/devm_request_irq) */
#include <linux/interrupt.h>	/* [한국어] irqreturn_t, IRQF_NO_THREAD, request_irq 계열 — vmd_irq() 등록에 쓴다 */
#include <linux/irq.h>		/* [한국어] struct irq_data, struct irq_chip, generic_handle_irq() — 인터럽트 중계의 뼈대 */
#include <linux/irqchip/irq-msi-lib.h>	/* [한국어] msi_lib_init_dev_msi_info() 와 MATCH_PCI_MSI — MSI 부모 도메인 공통 헬퍼 */
#include <linux/kernel.h>	/* [한국어] container_of, WARN_ON 등 기본 매크로 */
#include <linux/module.h>	/* [한국어] module_pci_driver(), MODULE_* 매크로 — 이 파일이 모듈로 빌드되기 위해 */
#include <linux/msi.h>		/* [한국어] struct msi_msg, struct msi_desc, msi_create_parent_irq_domain() */
#include <linux/pci.h>		/* [한국어] struct pci_dev/pci_bus/pci_ops, pci_read_config_ 계열 등 PCI 자료형 전반 */
#include <linux/pci-acpi.h>	/* [한국어] pci_acpi_set_companion_lookup_hook() — VMD 뒤 장치의 ACPI 노드를 찾아 주는 훅 */
#include <linux/pci-ecam.h>	/* [한국어] PCIE_ECAM_OFFSET() — 버스/devfn/레지스터를 ECAM 오프셋으로 바꾸는 계산 */
#include <linux/srcu.h>		/* [한국어] srcu_read_lock/synchronize_srcu — 인터럽트 중계 목록의 읽기 측 보호 */
#include <linux/rculist.h>	/* [한국어] list_add_tail_rcu/list_del_rcu/list_for_each_entry_rcu */
#include <linux/rcupdate.h>	/* [한국어] RCU 기본 API */

#include <xen/xen.h>		/* [한국어] xen_domain() — Xen 게스트에서는 MSI 중계 우회를 금지해야 한다 */

#include <asm/irqdomain.h>	/* [한국어] X86_MSI_BASE_ADDRESS_HIGH/LOW — MSI 메시지 주소를 직접 조립하기 위한 x86 정의. 이 드라이버가 x86 전용인 이유이기도 하다 */

/* [한국어] VMD 엔드포인트 자신의 BAR 번호들. struct pci_dev 의 resource[] 인덱스로 쓴다.
 * BAR 인덱스가 0,2,4 로 건너뛰는 이유는 이들이 모두 64 비트 BAR 라서 한 개가
 * 32 비트 슬롯 두 칸을 차지하기 때문이다(PCI 규격의 64 비트 BAR 규칙). */
#define VMD_CFGBAR	0	/* [한국어] 숨겨진 도메인의 config space(ECAM)가 통째로 들어 있는 창 */
#define VMD_MEMBAR1	2	/* [한국어] 하위 장치들에 나눠 줄 non-prefetchable MMIO 창 */
#define VMD_MEMBAR2	4	/* [한국어] 하위 장치들에 나눠 줄 두 번째 MMIO 창. 앞머리 일부는 VMD 자신의 레지스터(MSI-X 테이블, shadow 레지스터)가 쓴다 */

/* [한국어] 아래는 VMD 엔드포인트의 config space 에 있는 인텔 벤더 고유
 * 레지스터들이다. PCI 규격이 정한 표준 헤더(0x00~0x3f) 바깥이라 스펙 문서가
 * 아니라 이 드라이버의 정의가 유일한 근거다. */
#define PCI_REG_VMCAP		0x40	/* [한국어] VMD Capability. 이 하드웨어가 어떤 기능을 갖췄는지 알려 준다 */
#define BUS_RESTRICT_CAP(vmcap)	(vmcap & 0x1)	/* [한국어] bit 0 — 버스 번호 제한 기능이 있는가 */
#define PCI_REG_VMCONFIG	0x44	/* [한국어] VMD Configuration. 현재 설정값이 들어 있다 */
#define BUS_RESTRICT_CFG(vmcfg)	((vmcfg >> 8) & 0x3)	/* [한국어] bit 9:8 — 버스 번호 시작점 선택(0=0, 1=128, 2=224) */
#define VMCONFIG_MSI_REMAP	0x2	/* [한국어] bit 1 — 이 비트가 서면 MSI 재매핑을 "우회" 한다. 즉 중계를 켜려면 이 비트를 지워야 한다(vmd_set_msi_remapping 참조) */
#define PCI_REG_VMLOCK		0x70	/* [한국어] VMD Lock. shadow 레지스터 활성 여부가 여기 있다 */
#define MB2_SHADOW_EN(vmlock)	(vmlock & 0x2)	/* [한국어] bit 1 — MEMBAR2 안의 shadow 레지스터가 유효한가 */

/* [한국어] MEMBAR2 안에서 shadow 레지스터가 놓인 자리. 가상화된 게스트가
 * 자기에게 보이는 BAR 주소가 아니라 호스트의 진짜 물리 주소를 알아야
 * 루트 포트와 하위 장치의 자원을 올바로 배정할 수 있어서 둔 창구다. */
#define MB2_SHADOW_OFFSET	0x2000	/* [한국어] MEMBAR2 시작에서 8KB 지점 */
#define MB2_SHADOW_SIZE		16	/* [한국어] 64 비트 물리 주소 두 개 = 16 바이트 */

/* [한국어]
 * enum vmd_features - 하드웨어 세대별 차이를 vmd_ids 의 driver_data 로 실어 나르는 비트 플래그
 *
 * 설정자: 아래 vmd_ids[] 의 각 항목이 .driver_data 로 지정한다. 즉 어느
 *   Device ID 인지에 따라 정해지며 런타임에 바뀌지 않는다.
 * 읽는 자: vmd_probe() 가 id->driver_data 에서 꺼내 vmd_enable_domain() 과
 *   vmd_pm_enable_quirk() 에 unsigned long 로 전달한다.
 * 값 범위: 아래 여섯 비트의 조합. VMD_FEATS_CLIENT 는 그중 자주 쓰이는
 *   네 개를 묶은 이름이다.
 * 동기화: 읽기 전용 상수이므로 필요 없다.
 */
enum vmd_features {
	/*
	 * Device may contain registers which hint the physical location of the
	 * membars, in order to allow proper address translation during
	 * resource assignment to enable guest virtualization
	 */
	/* [한국어] MEMBAR2 안에 호스트 물리 주소를 알려 주는 shadow 레지스터가 있다.
	 * 읽는 곳: vmd_enable_domain() 이 이 비트를 보고 vmd_get_phys_offsets(native_hint=true)
	 *   를 부른다. 그러면 게스트가 보는 BAR 주소와 호스트 물리 주소의 차이(offset)를
	 *   구해, 하위 장치에 자원을 배정할 때 그만큼 보정할 수 있다.
	 * 이 비트가 있으면 membar2_offset 도 0x2000 이 아니라
	 *   MB2_SHADOW_OFFSET + MB2_SHADOW_SIZE 로 밀려, shadow 레지스터 영역을
	 *   하위 장치에 나눠 주지 않게 된다. */
	VMD_FEAT_HAS_MEMBAR_SHADOW		= (1 << 0),

	/*
	 * Device may provide root port configuration information which limits
	 * bus numbering
	 */
	/* [한국어] 이 VMD 가 담당할 버스 번호 범위가 제한되어 있고, 그 설정을
	 *   config 레지스터에서 읽을 수 있다.
	 * 읽는 곳: vmd_enable_domain() 이 vmd_get_bus_number_start() 를 불러
	 *   busn_start 를 0 / 128 / 224 중 하나로 정한다. 한 시스템에 VMD 가
	 *   여럿일 때 서로 버스 번호가 겹치지 않게 나눠 갖기 위한 장치다. */
	VMD_FEAT_HAS_BUS_RESTRICTIONS		= (1 << 1),

	/*
	 * Device contains physical location shadow registers in
	 * vendor-specific capability space
	 */
	/* [한국어] 위와 같은 shadow 레지스터가 MEMBAR2 안이 아니라 벤더 고유
	 *   capability(PCI_CAP_ID_VNDR) 안에 있다. 하이퍼바이저가 흉내 낸
	 *   capability 라 config 공간으로만 접근할 수 있다.
	 * 읽는 곳: vmd_enable_domain() 이 vmd_get_phys_offsets(native_hint=false)
	 *   를 부른다. 그쪽은 capability 안의 "SHDW" 서명을 확인한 뒤 값을 읽는다. */
	VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP	= (1 << 2),

	/*
	 * Device may use MSI-X vector 0 for software triggering and will not
	 * be used for MSI remapping
	 */
	/* [한국어] MSI-X 벡터 0 번은 하드웨어가 소프트웨어 트리거 용도로 쓰므로
	 *   인터럽트 중계에 쓰면 안 된다.
	 * 읽는 곳: vmd_probe() 가 이 비트를 보고 vmd->first_vec 을 1 로 만든다.
	 *   그러면 벡터 할당도, vmd_next_irq() 의 "느린 공용 벡터" 선택도 모두
	 *   1 번부터 시작한다. */
	VMD_FEAT_OFFSET_FIRST_VECTOR		= (1 << 3),

	/*
	 * Device can bypass remapping MSI-X transactions into its MSI-X table,
	 * avoiding the requirement of a VMD MSI domain for child device
	 * interrupt handling.
	 */
	/* [한국어] 하위 장치의 MSI-X 트랜잭션을 VMD 자신의 MSI-X 테이블로
	 *   재매핑하지 않고 그대로 통과시킬 수 있다.
	 * 읽는 곳: vmd_enable_domain() 이 이 비트가 서 있고 주소 보정도 필요
	 *   없으면 VMD MSI 도메인을 아예 만들지 않고(vmd_set_msi_remapping(false))
	 *   하위 장치가 상위 MSI 도메인을 직접 쓰게 한다. 중계 계층이 빠지므로
	 *   인터럽트 지연이 줄고, 벡터를 공유하지 않아도 된다.
	 * 예외: vmd_probe() 는 Xen 게스트에서 이 비트를 강제로 지운다. */
	VMD_FEAT_CAN_BYPASS_MSI_REMAP		= (1 << 4),

	/* [한국어] 아래 영어 블록은 이 저장소의 이전 작업이 상류 주석을 복제해
	 * 만들어 넣은 것이었다. 원본(1f0e418bb6)에는 "Enable ASPM on the PCIE
	 * root ports and set the default LTR ..." 로 시작하는 한 개만 있다.
	 * 지어낸 쪽을 지우고 상류 원문만 남겼다. */
	/*
	 * Enable ASPM on the PCIE root ports and set the default LTR of the
	 * storage devices on platforms where these values are not configured by
	 * BIOS. This is needed for laptops, which require these settings for
	 * proper power management of the SoC.
	 */
	/* [한국어] BIOS 가 해 주지 않은 절전 설정을 이 드라이버가 대신 넣는다.
	 * 노트북에서 문제가 되는 부분이다. VMD 뒤의 저장장치에 ASPM(링크 저전력
	 * 상태)과 LTR(Latency Tolerance Reporting, 장치가 견딜 수 있는 지연을
	 * 플랫폼에 알리는 값)이 설정되어 있지 않으면 SoC 가 깊은 절전 상태로
	 * 내려가지 못해 배터리가 빨리 닳는다. 데스크톱/서버 BIOS 는 이 값을
	 * 넣어 주지만 일부 노트북 BIOS 는 넣지 않아서, 그런 플랫폼의 Device ID
	 * 에 이 플래그를 달아 드라이버가 보정하게 한다.
	 * 읽는 곳: vmd_pm_enable_quirk() 의 첫 줄. vmd_enable_domain() 이
	 *   pci_walk_bus() 로 도메인 안 모든 장치에 대해 그 함수를 돌린다. */
	VMD_FEAT_BIOS_PM_QUIRK		= (1 << 5),
};

/* [한국어] BIOS 가 LTR 값을 넣어 주지 않았을 때 드라이버가 대신 써 넣는 값.
 * PCIe LTR 레지스터의 인코딩은 값(0~1023)과 스케일(3 비트)로 나뉘는데
 * 0x1003 은 스케일 필드가 1(=1024ns 단위)이고 값이 3 이라는 뜻이다.
 * 상류 주석이 그 결과를 3145728 ns(약 3.1ms)로 적어 두었다. 이 트리에는
 * LTR 인코딩 정의가 있는 헤더가 없어 그 계산을 직접 확인하지는 못했다. */
#define VMD_BIOS_PM_QUIRK_LTR	0x1003	/* 3145728 ns */

/* [한국어] 클라이언트(노트북·데스크톱)용 VMD 에 공통으로 필요한 네 기능을
 * 묶은 이름. vmd_ids[] 의 여러 항목이 이 하나로 driver_data 를 지정한다. */
#define VMD_FEATS_CLIENT	(VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP |					 VMD_FEAT_HAS_BUS_RESTRICTIONS |					 VMD_FEAT_OFFSET_FIRST_VECTOR |						 VMD_FEAT_BIOS_PM_QUIRK)

/* [한국어] VMD 인스턴스 번호 발급기(IDA = ID Allocator).
 * 설정자/읽는 자: vmd_probe() 가 ida_alloc 으로 번호를 받아 "vmd0", "vmd1"
 *   같은 이름을 만들고, vmd_remove() 와 probe 실패 경로가 ida_free 로 반납한다.
 * 값 범위: 0 부터. 한 시스템에 VMD 가 여럿일 수 있어 필요하다.
 * 동기화: IDA 자체가 내부 락을 가진다. */
static DEFINE_IDA(vmd_instance_ida);

/*
 * Lock for manipulating VMD IRQ lists.
 */
/* [한국어] 모든 VMD 인스턴스의 인터럽트 중계 목록을 함께 보호하는 전역 락.
 * 설정자/읽는 자: vmd_irq_enable()/vmd_irq_disable() 이 목록에 넣고 뺄 때,
 *   vmd_next_irq() 와 vmd_msi_free() 가 벡터별 count 를 조정할 때 잡는다.
 * 값 범위: raw_spinlock. raw 인 이유는 이 코드가 인터럽트를 끈 채 도는
 *   경로(하드 IRQ 문맥, PREEMPT_RT 에서도 잠들면 안 되는 자리)에서
 *   불리기 때문이다. 보통의 spinlock 은 PREEMPT_RT 에서 잠들 수 있는
 *   뮤텍스로 바뀌므로 여기에 쓸 수 없다.
 * 동기화: 인스턴스별이 아니라 전역이라는 점에 유의. 락 경합보다 단순함을
 *   택한 상류의 선택이다. 읽기 측(vmd_irq 의 목록 순회)은 이 락 대신
 *   SRCU 로 보호한다. */
static DEFINE_RAW_SPINLOCK(list_lock);

/**
 * struct vmd_irq - private data to map driver IRQ to the VMD shared vector
 * @node:	list item for parent traversal.
 * @irq:	back pointer to parent.
 * @enabled:	true if driver enabled IRQ
 * @virq:	the virtual IRQ value provided to the requesting driver.
 *
 * Every MSI/MSI-X IRQ requested for a device in a VMD domain will be mapped to
 * a VMD IRQ using this structure.
 */
/* [한국어]
 * struct vmd_irq - 하위 장치의 인터럽트 하나를 VMD 공용 벡터에 매다는 고리
 *
 * VMD 도메인 안의 장치가 MSI/MSI-X 를 하나 요청할 때마다 이 구조체가
 * 하나 만들어진다. 그 장치는 자기가 전용 인터럽트를 받는다고 믿지만,
 * 실제로 CPU 에 도달하는 벡터는 VMD 가 가진 몇 개 중 하나이고 여럿이
 * 그것을 나눠 쓴다. 이 구조체가 "그 공용 벡터가 울렸을 때 누구에게
 * 되뿌려야 하는가" 의 답을 담는다.
 *
 * 생성: vmd_msi_alloc() 이 kzalloc 으로 만든다.
 * 파괴: vmd_msi_free() 가 SRCU 유예 기간을 기다린 뒤 kfree 한다.
 * 보호: 목록 조작은 list_lock, 순회는 SRCU.
 */
struct vmd_irq {
	struct list_head	node;
	/* [한국어] 이 고리를 vmd_irq_list.irq_list 에 매다는 링크.
	 * 설정자: vmd_irq_enable() 이 list_add_tail_rcu 로 붙이고,
	 *   vmd_irq_disable() 이 list_del_rcu 로 뗀다.
	 * 읽는 자: vmd_irq() 가 list_for_each_entry_rcu 로 순회한다.
	 * 값 범위: 붙어 있거나(enabled=true) 떨어져 있거나(false) 둘 중 하나.
	 *   vmd_msi_alloc() 이 INIT_LIST_HEAD 로 자기 자신을 가리키게 초기화한다.
	 * 동기화: 쓰기는 list_lock 아래, 읽기는 SRCU 읽기 구간 안에서.
	 *   _rcu 변형을 쓰는 이유는 하드 IRQ 문맥의 순회와 나란히 목록이
	 *   바뀔 수 있어서다. */

	struct vmd_irq_list	*irq;
	/* [한국어] 이 고리가 매달린 VMD 벡터(부모)를 되짚는 포인터.
	 * 설정자: vmd_msi_alloc() 이 vmd_next_irq() 가 고른 벡터를 넣는다.
	 * 읽는 자: vmd_irq_enable()/disable() 이 어느 목록에 넣고 뺄지 알기 위해,
	 *   vmd_compose_msi_msg() 가 MSI 메시지에 실을 벡터 번호를 알기 위해,
	 *   vmd_msi_free() 가 count 를 줄이고 SRCU 를 기다리기 위해 읽는다.
	 * 값 범위: vmd->irqs[] 배열의 한 원소. 수명 동안 바뀌지 않는다.
	 * 동기화: 불변이므로 별도 보호가 필요 없다. */

	bool			enabled;
	/* [한국어] 지금 목록에 매달려 있는가.
	 * 설정자: vmd_irq_enable() 이 true 로, vmd_irq_disable() 이 false 로.
	 * 읽는 자: vmd_irq_disable() 이 이중 제거를 막기 위해, vmd_irq_enable()
	 *   이 WARN_ON 으로 이중 추가를 잡기 위해 읽는다.
	 * 값 범위: true/false.
	 * 동기화: list_lock 안에서만 읽고 쓴다. node 와 한 몸으로 움직이므로
	 *   따로 원자성을 줄 필요가 없다. */

	unsigned int		virq;
	/* [한국어] 하위 장치 드라이버가 받은 리눅스 가상 인터럽트 번호.
	 * 설정자: vmd_msi_alloc() 이 virq + i 로 채운다.
	 * 읽는 자: vmd_irq() 가 generic_handle_irq(vmdirq->virq) 로 되뿌릴 때.
	 *   이 한 줄이 인터럽트 중계의 핵심이다 — VMD 벡터가 울리면 목록에
	 *   달린 모든 하위 virq 를 차례로 호출한다.
	 * 값 범위: 커널이 발급한 유효한 virq 번호.
	 * 동기화: 불변. */
};

/* [한국어]
 * struct vmd_irq_list - VMD 벡터 하나와 그것을 나눠 쓰는 하위 인터럽트들
 *
 * vmd->irqs[] 배열의 원소이며, 원소 하나가 VMD 자신의 MSI-X 벡터 하나에
 * 대응한다. 배열 인덱스가 곧 MSI-X 테이블 인덱스이고, 그 인덱스가
 * vmd_compose_msi_msg() 에서 하위 장치의 MSI 목적지 ID 로 쓰인다.
 *
 * 생성: vmd_alloc_irqs() 가 devm_kcalloc 으로 msix_count 개를 한 번에 만든다.
 * 파괴: devm 관리라 드라이버 언바인딩 시 자동 해제. 다만 SRCU 구조체만은
 *   vmd_cleanup_srcu() 가 명시적으로 정리한다.
 * 보호: count 와 목록은 list_lock, 목록 순회는 srcu.
 */
struct vmd_irq_list {
	struct list_head	irq_list;
	/* [한국어] 이 벡터가 울렸을 때 되뿌릴 하위 인터럽트들의 목록 머리.
	 * 설정자: vmd_alloc_irqs() 가 INIT_LIST_HEAD 로 초기화하고, 이후
	 *   vmd_irq_enable()/disable() 이 항목을 넣고 뺀다.
	 * 읽는 자: vmd_irq() 가 순회한다.
	 * 값 범위: 비어 있을 수도 있다(그 벡터를 쓰는 장치가 아직 없거나 모두
	 *   disable 된 경우). 그러면 vmd_irq() 는 아무 일도 하지 않고 끝난다.
	 * 동기화: 쓰기는 list_lock, 읽기는 아래 srcu. */

	struct srcu_struct	srcu;
	/* [한국어] 목록 순회를 보호하는 SRCU(Sleepable RCU) 인스턴스.
	 * 설정자: vmd_alloc_irqs() 가 init_srcu_struct 로 초기화,
	 *   vmd_cleanup_srcu() 가 cleanup_srcu_struct 로 정리.
	 * 읽는 자: vmd_irq() 가 srcu_read_lock/unlock 으로 감싸 순회하고,
	 *   vmd_msi_free() 가 synchronize_srcu 로 그 순회가 끝나기를 기다린다.
	 * 값 범위: SRCU 코어가 관리하는 불투명 상태.
	 * 동기화: 왜 일반 RCU 가 아니라 SRCU 인가 — 벡터마다 독립된 유예
	 *   기간을 갖기 위해서다. 하나의 하위 인터럽트를 해제할 때 시스템
	 *   전체의 RCU 유예를 기다리는 대신 그 벡터의 순회만 기다리면 된다.
	 *   synchronize_srcu 는 잠들 수 있으므로 vmd_msi_free() 는 반드시
	 *   프로세스 컨텍스트에서 불려야 한다. */

	unsigned int		count;
	/* [한국어] 이 벡터에 배정된 하위 인터럽트 개수. 부하 분산의 척도다.
	 * 설정자: vmd_next_irq() 가 고를 때 ++, vmd_msi_free() 가 놓을 때 --.
	 * 읽는 자: vmd_next_irq() 가 가장 한가한 벡터를 고를 때 비교 기준.
	 * 값 범위: 0 이상. 다만 first_vec 이 가리키는 "느린" 공용 벡터는
	 *   이 값을 올리지 않고 무조건 배정되므로, 그 칸의 count 는 실제
	 *   사용자 수를 반영하지 않는다(상류 코드 그대로).
	 * 동기화: list_lock 아래에서만 조작한다. */

	unsigned int		virq;
	/* [한국어] 이 VMD 벡터에 대응하는 리눅스 가상 인터럽트 번호.
	 * 설정자: vmd_alloc_irqs() 가 pci_irq_vector(dev, i) 로 얻어 채운다.
	 * 읽는 자: devm_request_irq()/devm_free_irq() 의 대상,
	 *   그리고 vmd_msi_alloc() 이 irq_domain_set_info() 로 하위 virq 의
	 *   부모로 지정할 때.
	 * 값 범위: 유효한 virq 번호.
	 * 동기화: 초기화 후 불변. */
};

/* [한국어]
 * struct vmd_dev - VMD 인스턴스 하나의 모든 상태
 *
 * vmd_probe() 가 devm_kzalloc 으로 하나 만들어 pci_set_drvdata 로 매단다.
 * 이 구조체 하나가 "숨겨진 도메인" 그 자체다 — 그 도메인의 config 창,
 * 자원 창, 인터럽트 벡터, 루트 버스가 모두 여기 들어 있다.
 *
 * 생성/파괴: vmd_probe() 의 devm_kzalloc. devm 이므로 언바인딩 시 자동 해제.
 * 되찾는 법: 드라이버 쪽에서는 pci_get_drvdata(pdev), 버스 쪽에서는
 *   vmd_from_bus(bus) 가 sysdata 필드로부터 container_of 로 되찾는다.
 */
struct vmd_dev {
	struct pci_dev		*dev;
	/* [한국어] VMD 엔드포인트 자기 자신. 이 드라이버가 바인딩된 PCI 장치다.
	 * 설정자: vmd_probe() 가 probe 인자를 그대로 넣는다.
	 * 읽는 자: 거의 모든 함수. BAR 자원(dev->resource[])을 읽고,
	 *   config 레지스터를 읽고 쓰고, devm 할당의 주인으로 삼는다.
	 * 값 범위: NULL 이 아니다.
	 * 동기화: 불변. */

	raw_spinlock_t		cfg_lock;
	/* [한국어] 이 도메인의 config space 접근을 직렬화하는 락.
	 * 설정자: vmd_probe() 가 raw_spin_lock_init 으로 초기화.
	 * 읽는 자: vmd_pci_read()/vmd_pci_write() 가 guard 로 잡는다.
	 * 값 범위: raw_spinlock.
	 * 동기화: 왜 필요한가 — 원본 주석이 밝히듯 이 하드웨어의 일부
	 *   버전은 config 접근을 직렬화하지 않으면 CPU 가 교착에 빠진다.
	 *   irqsave 변형을 쓰는 이유는 config 접근이 인터럽트 문맥에서도
	 *   일어날 수 있기 때문이고, raw 인 이유는 PREEMPT_RT 에서도 잠들면
	 *   안 되기 때문이다. */

	void __iomem		*cfgbar;
	/* [한국어] VMD_CFGBAR(BAR 0)를 매핑한 가상 주소. 이 창 안이 곧 숨겨진
	 *   도메인의 config space 이고, ECAM 규칙(버스<<20 | devfn<<12 | reg)으로
	 *   배치되어 있다.
	 * 설정자: vmd_probe() 의 pcim_iomap(dev, VMD_CFGBAR, 0).
	 * 읽는 자: vmd_cfg_addr() 이 여기에 오프셋을 더해 최종 주소를 만들고,
	 *   vmd_domain_reset() 이 직접 더해 브리지 레지스터를 만진다.
	 * 값 범위: 유효한 __iomem 포인터. pcim_ 접두사라 관리 매핑이므로
	 *   언바인딩 시 자동 해제된다.
	 * 동기화: 포인터 자체는 불변. 접근은 cfg_lock 으로 보호한다. */

	int msix_count;
	/* [한국어] 이 VMD 가 실제로 확보한 MSI-X 벡터 개수 = irqs[] 배열 길이.
	 * 설정자: vmd_alloc_irqs() 가 pci_alloc_irq_vectors() 의 반환값으로 채운다.
	 *   MSI 중계를 우회하는 경로에서는 vmd_alloc_irqs() 를 아예 부르지 않아
	 *   0 인 채로 남는다.
	 * 읽는 자: vmd_next_irq() 의 탐색 범위, vmd_create_irq_domain() 의
	 *   도메인 크기, vmd_cleanup_srcu()/vmd_suspend()/vmd_resume() 의 순회 범위,
	 *   그리고 vmd_remove_irq_domain() 이 "0 이면 중계를 안 쓴 것" 으로
	 *   판정하는 근거.
	 * 값 범위: 0 또는 양수.
	 * 동기화: probe 중에만 쓰이고 이후 불변. */

	struct vmd_irq_list	*irqs;
	/* [한국어] VMD 벡터별 중계 목록 배열. 길이는 msix_count.
	 * 설정자: vmd_alloc_irqs() 의 devm_kcalloc.
	 * 읽는 자: vmd_next_irq(), vmd_irq() 의 핸들러 인자, index_from_irqs()
	 *   가 포인터 뺄셈으로 인덱스를 되구하는 대상.
	 * 값 범위: msix_count 가 0 이면 NULL 일 수 있다.
	 * 동기화: 배열 자체는 불변, 각 원소의 내용은 list_lock/SRCU 로 보호. */

	struct pci_sysdata	sysdata;
	/* [한국어] 새로 만들 PCI 도메인의 아키텍처별 사설 데이터. 값이 아니라
	 *   구조체가 통째로 박혀 있다는 점이 중요하다 — 이것이 새 버스의
	 *   sysdata 포인터가 되므로, vmd_from_bus() 가 container_of 로
	 *   바깥 vmd_dev 를 되찾을 수 있다.
	 * 설정자: vmd_probe() 가 domain 을 PCI_DOMAIN_NR_NOT_SET 으로 초기화하고,
	 *   vmd_enable_domain() 이 vmd_dev(진짜 DMA 주체)와 domain(0x10000 이상의
	 *   빈 번호), node(NUMA 노드)를 채운다.
	 * 읽는 자: pci_create_root_bus() 에 넘겨져 그 아래 모든 버스의 sysdata 가
	 *   된다. sysdata.vmd_dev 는 아키텍처 코드가 DMA 별칭을 정할 때 읽는다.
	 * 값 범위: domain 은 0x10000 이상(ACPI _SEG 가 쓰는 하위 16 비트와 겹치지
	 *   않게 하기 위해).
	 * 동기화: probe 중에만 쓰이고 이후 불변. */

	struct resource		resources[3];
	/* [한국어] 이 도메인이 하위 장치에 나눠 줄 자원 창 세 개.
	 *   [0]은 버스 번호 범위, [1]은 MEMBAR1 에서 잘라 낸 MMIO 창,
	 *   [2]는 MEMBAR2 에서 잘라 낸 MMIO 창이다.
	 * 설정자: vmd_enable_domain() 이 VMD 엔드포인트의 BAR 값으로부터 계산해
	 *   채운다. [1]과 [2]는 parent 를 원래 BAR 자원으로 가리키게 해 두고,
	 *   vmd_attach_resources() 가 그 BAR 자원의 child 로 자기를 등록한다.
	 * 읽는 자: pci_add_resource(_offset) 으로 루트 버스에 등록된 뒤
	 *   pci_assign_unassigned_bus_resources() 가 여기서 하위 장치에 배정한다.
	 *   vmd_domain_reset() 은 [0]의 크기로 훑을 버스 개수를 정한다.
	 * 값 범위: [0]은 busn_start 부터 CFGBAR 크기를 1MB 로 나눈 개수만큼.
	 *   ECAM 에서 버스 하나가 1MB(32 장치 x 8 함수 x 4KB)를 차지하기 때문이다.
	 * 동기화: probe 중 설정 후 자원 코어가 관리한다. */

	struct irq_domain	*irq_domain;
	/* [한국어] 이 VMD 가 만든 MSI 부모 도메인. 하위 장치의 MSI 할당 요청이
	 *   여기로 들어와 vmd_msi_alloc() 이 처리한다.
	 * 설정자: vmd_create_irq_domain() 이 만들고, vmd_remove_irq_domain() 이 없앤다.
	 * 읽는 자: vmd_enable_domain() 이 새 버스의 MSI 도메인으로 지정할 때,
	 *   vmd_resume() 이 "중계를 쓰는 구성인가" 를 판정할 때.
	 * 값 범위: MSI 중계를 우회하는 구성에서는 NULL 이다. 그때는 새 버스가
	 *   VMD 엔드포인트 자신의 MSI 도메인을 그대로 물려받는다.
	 * 동기화: probe 중 설정 후 불변. */

	struct pci_bus		*bus;
	/* [한국어] 이 드라이버가 만들어 낸 새 도메인의 루트 버스.
	 * 설정자: vmd_enable_domain() 의 pci_create_root_bus().
	 * 읽는 자: 열거(pci_scan_child_bus), 자원 배정, 바인딩(pci_bus_add_devices),
	 *   그리고 vmd_remove() 의 정지·제거.
	 * 값 범위: 생성 실패면 NULL 이고 그때는 probe 가 -ENODEV 로 끝난다.
	 * 동기화: PCI 코어의 버스 락이 관리한다. */

	u8			busn_start;
	/* [한국어] 이 도메인의 버스 번호 시작점. 0, 128, 224 중 하나.
	 * 설정자: 기본 0(devm_kzalloc 의 0 초기화). VMD_FEAT_HAS_BUS_RESTRICTIONS
	 *   가 있으면 vmd_get_bus_number_start() 가 하드웨어 설정대로 바꾼다.
	 * 읽는 자: resources[0] 의 범위 계산, pci_create_root_bus() 의 시작 버스,
	 *   그리고 vmd_cfg_addr() 이 실제 버스 번호에서 이 값을 빼 ECAM 안의
	 *   상대 버스 번호를 구할 때.
	 * 값 범위: 0 / 128 / 224.
	 * 동기화: probe 중 설정 후 불변. */

	u8			first_vec;
	/* [한국어] 인터럽트 중계에 쓸 수 있는 첫 MSI-X 벡터 번호.
	 * 설정자: 기본 0. VMD_FEAT_OFFSET_FIRST_VECTOR 가 있으면 vmd_probe() 가
	 *   1 로 만든다(0 번은 하드웨어가 소프트웨어 트리거로 쓰기 때문).
	 * 읽는 자: vmd_alloc_irqs() 의 최소 벡터 수 계산, vmd_next_irq() 의
	 *   "느린 공용 벡터" 지정과 탐색 시작점.
	 * 값 범위: 0 또는 1.
	 * 동기화: probe 중 설정 후 불변. */

	char			*name;
	/* [한국어] "vmd0", "vmd1" 같은 인스턴스 이름. /proc/interrupts 에
	 *   이 이름으로 뜬다.
	 * 설정자: vmd_probe() 의 devm_kasprintf.
	 * 읽는 자: devm_request_irq() 의 이름 인자(vmd_alloc_irqs 와 vmd_resume).
	 * 값 범위: 할당 실패면 NULL 이고 그때는 probe 가 -ENOMEM 으로 끝난다.
	 * 동기화: 불변. devm 이므로 자동 해제된다. */

	int			instance;
	/* [한국어] IDA 에서 받은 인스턴스 번호. name 을 만드는 재료다.
	 * 설정자: vmd_probe() 의 ida_alloc.
	 * 읽는 자: name 생성, 그리고 ida_free 로 반납할 때.
	 * 값 범위: 0 이상. 음수면 할당 실패이며 그 값이 그대로 probe 의
	 *   반환값이 된다.
	 * 동기화: 불변. */
};

static inline struct vmd_dev *vmd_from_bus(struct pci_bus *bus)
{
	return container_of(bus->sysdata, struct vmd_dev, sysdata);
}

static inline unsigned int index_from_irqs(struct vmd_dev *vmd,
					   struct vmd_irq_list *irqs)
{
	return irqs - vmd->irqs;
}

/*
 * Drivers managing a device in a VMD domain allocate their own IRQs as before,
 * but the MSI entry for the hardware it's driving will be programmed with a
 * destination ID for the VMD MSI-X table.  The VMD muxes interrupts in its
 * domain into one of its own, and the VMD driver de-muxes these for the
 * handlers sharing that VMD IRQ.  The vmd irq_domain provides the operations
 * and irq_chip to set this up.
 */
static void vmd_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct vmd_irq *vmdirq = data->chip_data;
	struct vmd_irq_list *irq = vmdirq->irq;
	struct vmd_dev *vmd = irq_data_get_irq_handler_data(data);

	memset(msg, 0, sizeof(*msg));
	msg->address_hi = X86_MSI_BASE_ADDRESS_HIGH;
	msg->arch_addr_lo.base_address = X86_MSI_BASE_ADDRESS_LOW;
	msg->arch_addr_lo.destid_0_7 = index_from_irqs(vmd, irq);
}

static void vmd_irq_enable(struct irq_data *data)
{
	struct vmd_irq *vmdirq = data->chip_data;

	scoped_guard(raw_spinlock_irqsave, &list_lock) {
		WARN_ON(vmdirq->enabled);
		list_add_tail_rcu(&vmdirq->node, &vmdirq->irq->irq_list);
		vmdirq->enabled = true;
	}
}

static void vmd_pci_msi_enable(struct irq_data *data)
{
	vmd_irq_enable(data->parent_data);
	data->chip->irq_unmask(data);
}

static unsigned int vmd_pci_msi_startup(struct irq_data *data)
{
	vmd_pci_msi_enable(data);
	return 0;
}

static void vmd_irq_disable(struct irq_data *data)
{
	struct vmd_irq *vmdirq = data->chip_data;

	scoped_guard(raw_spinlock_irqsave, &list_lock) {
		if (vmdirq->enabled) {
			list_del_rcu(&vmdirq->node);
			vmdirq->enabled = false;
		}
	}
}

static void vmd_pci_msi_disable(struct irq_data *data)
{
	data->chip->irq_mask(data);
	vmd_irq_disable(data->parent_data);
}

static void vmd_pci_msi_shutdown(struct irq_data *data)
{
	vmd_pci_msi_disable(data);
}

static struct irq_chip vmd_msi_controller = {
	.name			= "VMD-MSI",
	.irq_compose_msi_msg	= vmd_compose_msi_msg,
};

/*
 * XXX: We can be even smarter selecting the best IRQ once we solve the
 * affinity problem.
 */
static struct vmd_irq_list *vmd_next_irq(struct vmd_dev *vmd, struct msi_desc *desc)
{
	int i, best;

	if (vmd->msix_count == 1 + vmd->first_vec)
		return &vmd->irqs[vmd->first_vec];

	/*
	 * White list for fast-interrupt handlers. All others will share the
	 * "slow" interrupt vector.
	 */
	switch (msi_desc_to_pci_dev(desc)->class) {
	case PCI_CLASS_STORAGE_EXPRESS:
		break;
	default:
		return &vmd->irqs[vmd->first_vec];
	}

	scoped_guard(raw_spinlock_irq, &list_lock) {
		best = vmd->first_vec + 1;
		for (i = best; i < vmd->msix_count; i++)
			if (vmd->irqs[i].count < vmd->irqs[best].count)
				best = i;
		vmd->irqs[best].count++;
	}

	return &vmd->irqs[best];
}

static void vmd_msi_free(struct irq_domain *domain, unsigned int virq,
			 unsigned int nr_irqs);

static int vmd_msi_alloc(struct irq_domain *domain, unsigned int virq,
			 unsigned int nr_irqs, void *arg)
{
	struct msi_desc *desc = ((msi_alloc_info_t *)arg)->desc;
	struct vmd_dev *vmd = domain->host_data;
	struct vmd_irq *vmdirq;

	for (int i = 0; i < nr_irqs; ++i) {
		vmdirq = kzalloc_obj(*vmdirq);
		if (!vmdirq) {
			vmd_msi_free(domain, virq, i);
			return -ENOMEM;
		}

		INIT_LIST_HEAD(&vmdirq->node);
		vmdirq->irq = vmd_next_irq(vmd, desc);
		vmdirq->virq = virq + i;

		irq_domain_set_info(domain, virq + i, vmdirq->irq->virq,
				    &vmd_msi_controller, vmdirq,
				    handle_untracked_irq, vmd, NULL);
	}

	return 0;
}

static void vmd_msi_free(struct irq_domain *domain, unsigned int virq,
			 unsigned int nr_irqs)
{
	struct irq_data *irq_data;
	struct vmd_irq *vmdirq;

	for (int i = 0; i < nr_irqs; ++i) {
		irq_data = irq_domain_get_irq_data(domain, virq + i);
		vmdirq = irq_data->chip_data;

		synchronize_srcu(&vmdirq->irq->srcu);

		/* XXX: Potential optimization to rebalance */
		scoped_guard(raw_spinlock_irq, &list_lock)
			vmdirq->irq->count--;

		kfree(vmdirq);
	}
}

static const struct irq_domain_ops vmd_msi_domain_ops = {
	.alloc		= vmd_msi_alloc,
	.free		= vmd_msi_free,
};

static bool vmd_init_dev_msi_info(struct device *dev, struct irq_domain *domain,
				  struct irq_domain *real_parent,
				  struct msi_domain_info *info)
{
	if (!msi_lib_init_dev_msi_info(dev, domain, real_parent, info))
		return false;

	info->chip->irq_startup		= vmd_pci_msi_startup;
	info->chip->irq_shutdown	= vmd_pci_msi_shutdown;
	info->chip->irq_enable		= vmd_pci_msi_enable;
	info->chip->irq_disable		= vmd_pci_msi_disable;
	return true;
}

#define VMD_MSI_FLAGS_SUPPORTED	(MSI_GENERIC_FLAGS_MASK | MSI_FLAG_PCI_MSIX)
#define VMD_MSI_FLAGS_REQUIRED	(MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_NO_AFFINITY)

static const struct msi_parent_ops vmd_msi_parent_ops = {
	.supported_flags	= VMD_MSI_FLAGS_SUPPORTED,
	.required_flags		= VMD_MSI_FLAGS_REQUIRED,
	.bus_select_token	= DOMAIN_BUS_VMD_MSI,
	.bus_select_mask	= MATCH_PCI_MSI,
	.prefix			= "VMD-",
	.init_dev_msi_info	= vmd_init_dev_msi_info,
};

static int vmd_create_irq_domain(struct vmd_dev *vmd)
{
	struct irq_domain_info info = {
		.size		= vmd->msix_count,
		.ops		= &vmd_msi_domain_ops,
		.host_data	= vmd,
	};

	info.fwnode = irq_domain_alloc_named_id_fwnode("VMD-MSI",
					       vmd->sysdata.domain);
	if (!info.fwnode)
		return -ENODEV;

	vmd->irq_domain = msi_create_parent_irq_domain(&info,
					       &vmd_msi_parent_ops);
	if (!vmd->irq_domain) {
		irq_domain_free_fwnode(info.fwnode);
		return -ENODEV;
	}

	return 0;
}

static void vmd_set_msi_remapping(struct vmd_dev *vmd, bool enable)
{
	u16 reg;

	pci_read_config_word(vmd->dev, PCI_REG_VMCONFIG, &reg);
	reg = enable ? (reg & ~VMCONFIG_MSI_REMAP) :
		       (reg | VMCONFIG_MSI_REMAP);
	pci_write_config_word(vmd->dev, PCI_REG_VMCONFIG, reg);
}

static void vmd_remove_irq_domain(struct vmd_dev *vmd)
{
	/*
	 * Some production BIOS won't enable remapping between soft reboots.
	 * Ensure remapping is restored before unloading the driver.
	 */
	if (!vmd->msix_count)
		vmd_set_msi_remapping(vmd, true);

	if (vmd->irq_domain) {
		struct fwnode_handle *fn = vmd->irq_domain->fwnode;

		irq_domain_remove(vmd->irq_domain);
		irq_domain_free_fwnode(fn);
	}
}

static void __iomem *vmd_cfg_addr(struct vmd_dev *vmd, struct pci_bus *bus,
				  unsigned int devfn, int reg, int len)
{
	unsigned int busnr_ecam = bus->number - vmd->busn_start;
	u32 offset = PCIE_ECAM_OFFSET(busnr_ecam, devfn, reg);

	if (offset + len >= resource_size(&vmd->dev->resource[VMD_CFGBAR]))
		return NULL;

	return vmd->cfgbar + offset;
}

/*
 * CPU may deadlock if config space is not serialized on some versions of this
 * hardware, so all config space access is done under a spinlock.
 */
static int vmd_pci_read(struct pci_bus *bus, unsigned int devfn, int reg,
			int len, u32 *value)
{
	struct vmd_dev *vmd = vmd_from_bus(bus);
	void __iomem *addr = vmd_cfg_addr(vmd, bus, devfn, reg, len);

	if (!addr)
		return -EFAULT;

	guard(raw_spinlock_irqsave)(&vmd->cfg_lock);
	switch (len) {
	case 1:
		*value = readb(addr);
		return 0;
	case 2:
		*value = readw(addr);
		return 0;
	case 4:
		*value = readl(addr);
		return 0;
	default:
		return -EINVAL;
	}
}

/*
 * VMD h/w converts non-posted config writes to posted memory writes. The
 * read-back in this function forces the completion so it returns only after
 * the config space was written, as expected.
 */
static int vmd_pci_write(struct pci_bus *bus, unsigned int devfn, int reg,
			 int len, u32 value)
{
	struct vmd_dev *vmd = vmd_from_bus(bus);
	void __iomem *addr = vmd_cfg_addr(vmd, bus, devfn, reg, len);

	if (!addr)
		return -EFAULT;

	guard(raw_spinlock_irqsave)(&vmd->cfg_lock);
	switch (len) {
	case 1:
		writeb(value, addr);
		readb(addr);
		return 0;
	case 2:
		writew(value, addr);
		readw(addr);
		return 0;
	case 4:
		writel(value, addr);
		readl(addr);
		return 0;
	default:
		return -EINVAL;
	}
}

static struct pci_ops vmd_ops = {
	.read		= vmd_pci_read,
	.write		= vmd_pci_write,
};

#ifdef CONFIG_ACPI
static struct acpi_device *vmd_acpi_find_companion(struct pci_dev *pci_dev)
{
	struct pci_host_bridge *bridge;
	u32 busnr, addr;

	if (pci_dev->bus->ops != &vmd_ops)
		return NULL;

	bridge = pci_find_host_bridge(pci_dev->bus);
	busnr = pci_dev->bus->number - bridge->bus->number;
	/*
	 * The address computation below is only applicable to relative bus
	 * numbers below 32.
	 */
	if (busnr > 31)
		return NULL;

	addr = (busnr << 24) | ((u32)pci_dev->devfn << 16) | 0x8000FFFFU;

	dev_dbg(&pci_dev->dev, "Looking for ACPI companion (address 0x%x)\n",
		addr);

	return acpi_find_child_device(ACPI_COMPANION(bridge->dev.parent), addr,
				      false);
}

static bool hook_installed;

static void vmd_acpi_begin(void)
{
	if (pci_acpi_set_companion_lookup_hook(vmd_acpi_find_companion))
		return;

	hook_installed = true;
}

static void vmd_acpi_end(void)
{
	if (!hook_installed)
		return;

	pci_acpi_clear_companion_lookup_hook();
	hook_installed = false;
}
#else
static inline void vmd_acpi_begin(void) { }
static inline void vmd_acpi_end(void) { }
#endif /* CONFIG_ACPI */

static void vmd_domain_reset(struct vmd_dev *vmd)
{
	u16 bus, max_buses = resource_size(&vmd->resources[0]);
	u8 dev, functions, fn, hdr_type;
	char __iomem *base;

	for (bus = 0; bus < max_buses; bus++) {
		for (dev = 0; dev < 32; dev++) {
			base = vmd->cfgbar + PCIE_ECAM_OFFSET(bus,
					    PCI_DEVFN(dev, 0), 0);

			hdr_type = readb(base + PCI_HEADER_TYPE);

			functions = (hdr_type & PCI_HEADER_TYPE_MFD) ? 8 : 1;
			for (fn = 0; fn < functions; fn++) {
				base = vmd->cfgbar + PCIE_ECAM_OFFSET(bus,
						    PCI_DEVFN(dev, fn), 0);

				hdr_type = readb(base + PCI_HEADER_TYPE) &
						PCI_HEADER_TYPE_MASK;

				if (hdr_type != PCI_HEADER_TYPE_BRIDGE ||
				    (readw(base + PCI_CLASS_DEVICE) !=
				     PCI_CLASS_BRIDGE_PCI))
					continue;

				/*
				 * Temporarily disable the I/O range before updating
				 * PCI_IO_BASE.
				 */
				writel(0x0000ffff, base + PCI_IO_BASE_UPPER16);
				/* Update lower 16 bits of I/O base/limit */
				writew(0x00f0, base + PCI_IO_BASE);
				/* Update upper 16 bits of I/O base/limit */
				writel(0, base + PCI_IO_BASE_UPPER16);

				/* MMIO Base/Limit */
				writel(0x0000fff0, base + PCI_MEMORY_BASE);

				/* Prefetchable MMIO Base/Limit */
				writel(0, base + PCI_PREF_LIMIT_UPPER32);
				writel(0x0000fff0, base + PCI_PREF_MEMORY_BASE);
				writel(0xffffffff, base + PCI_PREF_BASE_UPPER32);
			}
		}
	}
}

static void vmd_attach_resources(struct vmd_dev *vmd)
{
	vmd->dev->resource[VMD_MEMBAR1].child = &vmd->resources[1];
	vmd->dev->resource[VMD_MEMBAR2].child = &vmd->resources[2];
}

static void vmd_detach_resources(struct vmd_dev *vmd)
{
	vmd->dev->resource[VMD_MEMBAR1].child = NULL;
	vmd->dev->resource[VMD_MEMBAR2].child = NULL;
}

static int vmd_get_phys_offsets(struct vmd_dev *vmd, bool native_hint,
				resource_size_t *offset1,
				resource_size_t *offset2)
{
	struct pci_dev *dev = vmd->dev;
	u64 phys1, phys2;

	if (native_hint) {
		u32 vmlock;
		int ret;

		ret = pci_read_config_dword(dev, PCI_REG_VMLOCK, &vmlock);
		if (ret || PCI_POSSIBLE_ERROR(vmlock))
			return -ENODEV;

		if (MB2_SHADOW_EN(vmlock)) {
			void __iomem *membar2;

			membar2 = pci_iomap(dev, VMD_MEMBAR2, 0);
			if (!membar2)
				return -ENOMEM;
			phys1 = readq(membar2 + MB2_SHADOW_OFFSET);
			phys2 = readq(membar2 + MB2_SHADOW_OFFSET + 8);
			pci_iounmap(dev, membar2);
		} else
			return 0;
	} else {
		/* Hypervisor-Emulated Vendor-Specific Capability */
		int pos = pci_find_capability(dev, PCI_CAP_ID_VNDR);
		u32 reg, regu;

		pci_read_config_dword(dev, pos + 4, &reg);

		/* "SHDW" */
		if (pos && reg == 0x53484457) {
			pci_read_config_dword(dev, pos + 8, &reg);
			pci_read_config_dword(dev, pos + 12, &regu);
			phys1 = (u64) regu << 32 | reg;

			pci_read_config_dword(dev, pos + 16, &reg);
			pci_read_config_dword(dev, pos + 20, &regu);
			phys2 = (u64) regu << 32 | reg;
		} else
			return 0;
	}

	*offset1 = dev->resource[VMD_MEMBAR1].start -
			(phys1 & PCI_BASE_ADDRESS_MEM_MASK);
	*offset2 = dev->resource[VMD_MEMBAR2].start -
			(phys2 & PCI_BASE_ADDRESS_MEM_MASK);

	return 0;
}

static int vmd_get_bus_number_start(struct vmd_dev *vmd)
{
	struct pci_dev *dev = vmd->dev;
	u16 reg;

	pci_read_config_word(dev, PCI_REG_VMCAP, &reg);
	if (BUS_RESTRICT_CAP(reg)) {
		pci_read_config_word(dev, PCI_REG_VMCONFIG, &reg);

		switch (BUS_RESTRICT_CFG(reg)) {
		case 0:
			vmd->busn_start = 0;
			break;
		case 1:
			vmd->busn_start = 128;
			break;
		case 2:
			vmd->busn_start = 224;
			break;
		default:
			pci_err(dev, "Unknown Bus Offset Setting (%d)\n",
				BUS_RESTRICT_CFG(reg));
			return -ENODEV;
		}
	}

	return 0;
}

static irqreturn_t vmd_irq(int irq, void *data)
{
	struct vmd_irq_list *irqs = data;
	struct vmd_irq *vmdirq;
	int idx;

	idx = srcu_read_lock(&irqs->srcu);
	list_for_each_entry_rcu(vmdirq, &irqs->irq_list, node)
		generic_handle_irq(vmdirq->virq);
	srcu_read_unlock(&irqs->srcu, idx);

	return IRQ_HANDLED;
}

static int vmd_alloc_irqs(struct vmd_dev *vmd)
{
	struct pci_dev *dev = vmd->dev;
	int i, err;

	vmd->msix_count = pci_msix_vec_count(dev);
	if (vmd->msix_count < 0)
		return -ENODEV;

	vmd->msix_count = pci_alloc_irq_vectors(dev, vmd->first_vec + 1,
						vmd->msix_count, PCI_IRQ_MSIX);
	if (vmd->msix_count < 0)
		return vmd->msix_count;

	vmd->irqs = devm_kcalloc(&dev->dev, vmd->msix_count, sizeof(*vmd->irqs),
				 GFP_KERNEL);
	if (!vmd->irqs)
		return -ENOMEM;

	for (i = 0; i < vmd->msix_count; i++) {
		err = init_srcu_struct(&vmd->irqs[i].srcu);
		if (err)
			return err;

		INIT_LIST_HEAD(&vmd->irqs[i].irq_list);
		vmd->irqs[i].virq = pci_irq_vector(dev, i);
		err = devm_request_irq(&dev->dev, vmd->irqs[i].virq,
				       vmd_irq, IRQF_NO_THREAD,
				       vmd->name, &vmd->irqs[i]);
		if (err)
			return err;
	}

	return 0;
}

/*
 * Since VMD is an aperture to regular PCIe root ports, only allow it to
 * control features that the OS is allowed to control on the physical PCI bus.
 */
static void vmd_copy_host_bridge_flags(struct pci_host_bridge *root_bridge,
				       struct pci_host_bridge *vmd_bridge)
{
	vmd_bridge->native_pcie_hotplug = root_bridge->native_pcie_hotplug;
	vmd_bridge->native_shpc_hotplug = root_bridge->native_shpc_hotplug;
	vmd_bridge->native_aer = root_bridge->native_aer;
	vmd_bridge->native_pme = root_bridge->native_pme;
	vmd_bridge->native_ltr = root_bridge->native_ltr;
	vmd_bridge->native_dpc = root_bridge->native_dpc;
}

/*
 * Enable ASPM and LTR settings on devices that aren't configured by BIOS.
 */
static int vmd_pm_enable_quirk(struct pci_dev *pdev, void *userdata)
{
	unsigned long features = *(unsigned long *)userdata;
	u16 ltr = VMD_BIOS_PM_QUIRK_LTR;
	u32 ltr_reg;
	int pos;

	if (!(features & VMD_FEAT_BIOS_PM_QUIRK))
		return 0;

	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_LTR);
	if (!pos)
		goto out_state_change;

	/*
	 * Skip if the max snoop LTR is non-zero, indicating BIOS has set it
	 * so the LTR quirk is not needed.
	 */
	pci_read_config_dword(pdev, pos + PCI_LTR_MAX_SNOOP_LAT, &ltr_reg);
	if (!!(ltr_reg & (PCI_LTR_VALUE_MASK | PCI_LTR_SCALE_MASK)))
		goto out_state_change;

	/*
	 * Set the default values to the maximum required by the platform to
	 * allow the deepest power management savings. Write as a DWORD where
	 * the lower word is the max snoop latency and the upper word is the
	 * max non-snoop latency.
	 */
	ltr_reg = (ltr << 16) | ltr;
	pci_write_config_dword(pdev, pos + PCI_LTR_MAX_SNOOP_LAT, ltr_reg);
	pci_info(pdev, "VMD: Default LTR value set by driver\n");

out_state_change:
	/*
	 * Ensure devices are in D0 before enabling PCI-PM L1 PM Substates, per
	 * PCIe r6.0, sec 5.5.4.
	 */
	pci_set_power_state_locked(pdev, PCI_D0);
	pci_enable_link_state_locked(pdev, PCIE_LINK_STATE_ALL);
	return 0;
}

static int vmd_enable_domain(struct vmd_dev *vmd, unsigned long features)
{
	struct pci_sysdata *sd = &vmd->sysdata;
	struct resource *res;
	u32 upper_bits;
	unsigned long flags;
	LIST_HEAD(resources);
	resource_size_t offset[2] = {0};
	resource_size_t membar2_offset = 0x2000;
	struct pci_bus *child;
	struct pci_dev *dev;
	int ret;

	/*
	 * Shadow registers may exist in certain VMD device ids which allow
	 * guests to correctly assign host physical addresses to the root ports
	 * and child devices. These registers will either return the host value
	 * or 0, depending on an enable bit in the VMD device.
	 */
	if (features & VMD_FEAT_HAS_MEMBAR_SHADOW) {
		membar2_offset = MB2_SHADOW_OFFSET + MB2_SHADOW_SIZE;
		ret = vmd_get_phys_offsets(vmd, true, &offset[0], &offset[1]);
		if (ret)
			return ret;
	} else if (features & VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP) {
		ret = vmd_get_phys_offsets(vmd, false, &offset[0], &offset[1]);
		if (ret)
			return ret;
	}

	/*
	 * Certain VMD devices may have a root port configuration option which
	 * limits the bus range to between 0-127, 128-255, or 224-255
	 */
	if (features & VMD_FEAT_HAS_BUS_RESTRICTIONS) {
		ret = vmd_get_bus_number_start(vmd);
		if (ret)
			return ret;
	}

	res = &vmd->dev->resource[VMD_CFGBAR];
	vmd->resources[0] = (struct resource) {
		.name  = "VMD CFGBAR",
		.start = vmd->busn_start,
		.end   = vmd->busn_start + (resource_size(res) >> 20) - 1,
		.flags = IORESOURCE_BUS | IORESOURCE_PCI_FIXED,
	};

	/*
	 * If the window is below 4GB, clear IORESOURCE_MEM_64 so we can
	 * put 32-bit resources in the window.
	 *
	 * There's no hardware reason why a 64-bit window *couldn't*
	 * contain a 32-bit resource, but pbus_size_mem() computes the
	 * bridge window size assuming a 64-bit window will contain no
	 * 32-bit resources.  __pci_assign_resource() enforces that
	 * artificial restriction to make sure everything will fit.
	 *
	 * The only way we could use a 64-bit non-prefetchable MEMBAR is
	 * if its address is <4GB so that we can convert it to a 32-bit
	 * resource.  To be visible to the host OS, all VMD endpoints must
	 * be initially configured by platform BIOS, which includes setting
	 * up these resources.  We can assume the device is configured
	 * according to the platform needs.
	 */
	res = &vmd->dev->resource[VMD_MEMBAR1];
	upper_bits = upper_32_bits(res->end);
	flags = res->flags & ~IORESOURCE_SIZEALIGN;
	if (!upper_bits)
		flags &= ~IORESOURCE_MEM_64;
	vmd->resources[1] = (struct resource) {
		.name  = "VMD MEMBAR1",
		.start = res->start,
		.end   = res->end,
		.flags = flags,
		.parent = res,
	};

	res = &vmd->dev->resource[VMD_MEMBAR2];
	upper_bits = upper_32_bits(res->end);
	flags = res->flags & ~IORESOURCE_SIZEALIGN;
	if (!upper_bits)
		flags &= ~IORESOURCE_MEM_64;
	vmd->resources[2] = (struct resource) {
		.name  = "VMD MEMBAR2",
		.start = res->start + membar2_offset,
		.end   = res->end,
		.flags = flags,
		.parent = res,
	};

	/*
	 * Currently MSI remapping must be enabled in guest passthrough mode
	 * due to some missing interrupt remapping plumbing. This is probably
	 * acceptable because the guest is usually CPU-limited and MSI
	 * remapping doesn't become a performance bottleneck.
	 */
	if (!(features & VMD_FEAT_CAN_BYPASS_MSI_REMAP) ||
	    offset[0] || offset[1]) {
		ret = vmd_alloc_irqs(vmd);
		if (ret)
			return ret;

		vmd_set_msi_remapping(vmd, true);

		ret = vmd_create_irq_domain(vmd);
		if (ret)
			return ret;
	} else {
		vmd_set_msi_remapping(vmd, false);
	}

	pci_add_resource(&resources, &vmd->resources[0]);
	pci_add_resource_offset(&resources, &vmd->resources[1], offset[0]);
	pci_add_resource_offset(&resources, &vmd->resources[2], offset[1]);

	sd->vmd_dev = vmd->dev;

	/*
	 * Emulated domains start at 0x10000 to not clash with ACPI _SEG
	 * domains.  Per ACPI r6.0, sec 6.5.6, _SEG returns an integer, of
	 * which the lower 16 bits are the PCI Segment Group (domain) number.
	 * Other bits are currently reserved.
	 */
	sd->domain = pci_bus_find_emul_domain_nr(0, 0x10000, INT_MAX);
	if (sd->domain < 0)
		return sd->domain;

	sd->node = pcibus_to_node(vmd->dev->bus);

	vmd->bus = pci_create_root_bus(&vmd->dev->dev, vmd->busn_start,
				       &vmd_ops, sd, &resources);
	if (!vmd->bus) {
		pci_bus_release_emul_domain_nr(sd->domain);
		pci_free_resource_list(&resources);
		vmd_remove_irq_domain(vmd);
		return -ENODEV;
	}

	vmd_copy_host_bridge_flags(pci_find_host_bridge(vmd->dev->bus),
				   to_pci_host_bridge(vmd->bus->bridge));

	vmd_attach_resources(vmd);
	if (vmd->irq_domain)
		dev_set_msi_domain(&vmd->bus->dev, vmd->irq_domain);
	else
		dev_set_msi_domain(&vmd->bus->dev,
				   dev_get_msi_domain(&vmd->dev->dev));

	WARN(sysfs_create_link(&vmd->dev->dev.kobj, &vmd->bus->dev.kobj,
			       "domain"), "Can't create symlink to domain\n");

	vmd_acpi_begin();

	pci_scan_child_bus(vmd->bus);
	vmd_domain_reset(vmd);

	/* When Intel VMD is enabled, the OS does not discover the Root Ports
	 * owned by Intel VMD within the MMCFG space. pci_reset_bus() applies
	 * a reset to the parent of the PCI device supplied as argument. This
	 * is why we pass a child device, so the reset can be triggered at
	 * the Intel bridge level and propagated to all the children in the
	 * hierarchy.
	 */
	list_for_each_entry(child, &vmd->bus->children, node) {
		if (!list_empty(&child->devices)) {
			dev = list_first_entry(&child->devices,
					       struct pci_dev, bus_list);
			ret = pci_reset_bus(dev);
			if (ret)
				pci_warn(dev, "can't reset device: %d\n", ret);

			break;
		}
	}

	pci_assign_unassigned_bus_resources(vmd->bus);

	pci_walk_bus(vmd->bus, vmd_pm_enable_quirk, &features);

	/*
	 * VMD root buses are virtual and don't return true on pci_is_pcie()
	 * and will fail pcie_bus_configure_settings() early. It can instead be
	 * run on each of the real root ports.
	 */
	list_for_each_entry(child, &vmd->bus->children, node)
		pcie_bus_configure_settings(child);

	/* [한국어] 발견해 둔 장치들에 드라이버를 붙인다. 열거(pci_scan_child_bus)와
	 * 바인딩이 나뉘어 있는 이유는 그 사이에 자원 배정이 끼어야 하기 때문이다.
	 * 이 호출이 끝나야 VMD 도메인 안의 NVMe 들이 실제로 동작하기 시작한다 —
	 * 여기서 drivers/nvme/host/pci.c 의 nvme_probe() 가 불린다. */
	pci_bus_add_devices(vmd->bus);

	vmd_acpi_end();
	return 0;
}

static int vmd_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	unsigned long features = (unsigned long) id->driver_data;
	struct vmd_dev *vmd;
	int err;

	if (xen_domain()) {
		/*
		 * Xen doesn't have knowledge about devices in the VMD bus
		 * because the config space of devices behind the VMD bridge is
		 * not known to Xen, and hence Xen cannot discover or configure
		 * them in any way.
		 *
		 * Bypass of MSI remapping won't work in that case as direct
		 * write by Linux to the MSI entries won't result in functional
		 * interrupts, as Xen is the entity that manages the host
		 * interrupt controller and must configure interrupts.  However
		 * multiplexing of interrupts by the VMD bridge will work under
		 * Xen, so force the usage of that mode which must always be
		 * supported by VMD bridges.
		 */
		features &= ~VMD_FEAT_CAN_BYPASS_MSI_REMAP;
	}

	if (resource_size(&dev->resource[VMD_CFGBAR]) < (1 << 20))
		return -ENOMEM;

	vmd = devm_kzalloc(&dev->dev, sizeof(*vmd), GFP_KERNEL);
	if (!vmd)
		return -ENOMEM;

	vmd->dev = dev;
	vmd->sysdata.domain = PCI_DOMAIN_NR_NOT_SET;
	vmd->instance = ida_alloc(&vmd_instance_ida, GFP_KERNEL);
	if (vmd->instance < 0)
		return vmd->instance;

	vmd->name = devm_kasprintf(&dev->dev, GFP_KERNEL, "vmd%d",
				   vmd->instance);
	if (!vmd->name) {
		err = -ENOMEM;
		goto out_release_instance;
	}

	err = pcim_enable_device(dev);
	if (err < 0)
		goto out_release_instance;

	vmd->cfgbar = pcim_iomap(dev, VMD_CFGBAR, 0);
	if (!vmd->cfgbar) {
		err = -ENOMEM;
		goto out_release_instance;
	}

	pci_set_master(dev);
	if (dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(64)) &&
	    dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(32))) {
		err = -ENODEV;
		goto out_release_instance;
	}

	if (features & VMD_FEAT_OFFSET_FIRST_VECTOR)
		vmd->first_vec = 1;

	raw_spin_lock_init(&vmd->cfg_lock);
	pci_set_drvdata(dev, vmd);
	err = vmd_enable_domain(vmd, features);
	if (err)
		goto out_release_instance;

	dev_info(&vmd->dev->dev, "Bound to PCI domain %04x\n",
		 vmd->sysdata.domain);
	return 0;

 out_release_instance:
	ida_free(&vmd_instance_ida, vmd->instance);
	return err;
}

static void vmd_cleanup_srcu(struct vmd_dev *vmd)
{
	int i;

	for (i = 0; i < vmd->msix_count; i++)
		cleanup_srcu_struct(&vmd->irqs[i].srcu);
}

static void vmd_remove(struct pci_dev *dev)
{
	struct vmd_dev *vmd = pci_get_drvdata(dev);

	/* [한국어] 도메인 전체를 정지시킨다. 아래 장치부터 차례로 드라이버가
	 * 떨어져 나가므로, VMD 뒤의 NVMe 들은 여기서 nvme_remove() 를 거친다.
	 * stop 과 remove 가 나뉜 이유는 드라이버를 떼는 일과 struct pci_dev 를
	 * 없애는 일을 분리해야 참조가 남은 상태에서 해제하는 사고를 막을 수 있어서다. */
	pci_stop_root_bus(vmd->bus);
	sysfs_remove_link(&vmd->dev->dev.kobj, "domain");
	pci_remove_root_bus(vmd->bus);
	vmd_cleanup_srcu(vmd);
	vmd_detach_resources(vmd);
	vmd_remove_irq_domain(vmd);
	ida_free(&vmd_instance_ida, vmd->instance);
	pci_bus_release_emul_domain_nr(vmd->sysdata.domain);
}

static void vmd_shutdown(struct pci_dev *dev)
{
	struct vmd_dev *vmd = pci_get_drvdata(dev);

	vmd_remove_irq_domain(vmd);
}

#ifdef CONFIG_PM_SLEEP
static int vmd_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct vmd_dev *vmd = pci_get_drvdata(pdev);
	int i;

	for (i = 0; i < vmd->msix_count; i++)
		devm_free_irq(dev, vmd->irqs[i].virq, &vmd->irqs[i]);

	return 0;
}

static int vmd_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct vmd_dev *vmd = pci_get_drvdata(pdev);
	int err, i;

	vmd_set_msi_remapping(vmd, !!vmd->irq_domain);

	for (i = 0; i < vmd->msix_count; i++) {
		err = devm_request_irq(dev, vmd->irqs[i].virq,
				       vmd_irq, IRQF_NO_THREAD,
				       vmd->name, &vmd->irqs[i]);
		if (err)
			return err;
	}

	return 0;
}
#endif
static SIMPLE_DEV_PM_OPS(vmd_dev_pm_ops, vmd_suspend, vmd_resume);

static const struct pci_device_id vmd_ids[] = {
	{PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_VMD_201D),
		.driver_data = VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP,},
	{PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_VMD_28C0),
		.driver_data = VMD_FEAT_HAS_MEMBAR_SHADOW |
				VMD_FEAT_HAS_BUS_RESTRICTIONS |
				VMD_FEAT_CAN_BYPASS_MSI_REMAP,},
	{PCI_VDEVICE(INTEL, 0x467f),
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0x4c3d),
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xa77f),
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0x7d0b),
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xad0b),
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_VMD_9A0B),
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xb60b),
                .driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xb06f),
                .driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xb07f),
                .driver_data = VMD_FEATS_CLIENT,},
	{0,}
};
MODULE_DEVICE_TABLE(pci, vmd_ids);

static struct pci_driver vmd_drv = {
	.name		= "vmd",
	.id_table	= vmd_ids,
	.probe		= vmd_probe,
	.remove		= vmd_remove,
	.shutdown	= vmd_shutdown,
	.driver		= {
		.pm	= &vmd_dev_pm_ops,
	},
};
module_pci_driver(vmd_drv);

MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("Volume Management Device driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.6");
