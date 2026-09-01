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
 *       case PCI_CLASS_STORAGE_EXPRESS:   <- NVMe 의 24 비트 클래스 코드
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
#define VMD_FEATS_CLIENT	(VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP |	\
				 VMD_FEAT_HAS_BUS_RESTRICTIONS |	\
				 VMD_FEAT_OFFSET_FIRST_VECTOR |		\
				 VMD_FEAT_BIOS_PM_QUIRK)

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

/**
 * struct vmd_irq_list - list of driver requested IRQs mapping to a VMD vector
 * @irq_list:	the list of irq's the VMD one demuxes to.
 * @srcu:	SRCU struct for local synchronization.
 * @count:	number of child IRQs assigned to this vector; used to track
 *		sharing.
 * @virq:	The underlying VMD Linux interrupt number
 */
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

/* [한국어]
 * vmd_from_bus - 이 버스가 어느 VMD 인스턴스의 것인지 되찾는다
 *
 * @bus: VMD 도메인 안의 아무 PCI 버스.
 * @return: 그 버스를 만들어 낸 vmd_dev.
 *
 * struct vmd_dev 안에 struct pci_sysdata 가 값으로 박혀 있고, 그 주소가
 * 새 버스의 sysdata 로 넘어간다. 그래서 container_of 로 감싸고 있는
 * 바깥 구조체를 되찾을 수 있다. 커널에서 흔한 "포인터로부터 주인을
 * 역산하는" 기법이다.
 *
 * 이 트릭 덕분에 pci_ops 콜백처럼 버스만 받는 함수도 자기 VMD 인스턴스에
 * 닿을 수 있다. 하위 버스도 루트 버스의 sysdata 를 그대로 물려받으므로,
 * 도메인 안의 어느 깊이의 버스를 넣어도 같은 답이 나온다.
 *
 * 실행 컨텍스트: 어디서든. 포인터 산술 하나뿐이라 락도 필요 없다.
 *
 * 에러 경로: 없다. VMD 도메인 밖의 버스를 넣으면 엉뚱한 값이 나오므로
 * 호출자가 그것을 보장해야 한다(vmd_ops 를 통해 불리므로 자연히 보장된다).
 *
 * 호출 체인:
 *   vmd_pci_read() / vmd_pci_write() → [이 함수]
 */
static inline struct vmd_dev *vmd_from_bus(struct pci_bus *bus)
{
	return container_of(bus->sysdata, struct vmd_dev, sysdata);
}

/* [한국어]
 * index_from_irqs - vmd_irq_list 포인터를 배열 인덱스로 되돌린다
 *
 * @vmd: 이 VMD 인스턴스.  @irqs: vmd->irqs[] 안의 한 원소를 가리키는 포인터.
 * @return: 그 원소의 배열 인덱스.
 *
 * 포인터 뺄셈 한 번이다. 그런데 이 값이 단순한 인덱스가 아니라는 점이
 * 중요하다 — vmd->irqs[] 의 i 번째는 VMD 의 MSI-X 테이블 i 번 항목에
 * 대응하고, 그 i 가 vmd_compose_msi_msg() 에서 하위 장치의 MSI 메시지에
 * 목적지 ID 로 실린다. 즉 이 인덱스가 "하위 장치의 인터럽트를 VMD 의 몇
 * 번 벡터로 보낼 것인가" 를 결정하는 하드웨어 수준의 값이다.
 *
 * 실행 컨텍스트: 어디서든. vmd_compose_msi_msg() 를 통해 인터럽트 설정
 * 경로에서 불린다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vmd_compose_msi_msg() → [이 함수]
 */
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
/* [한국어]
 * vmd_compose_msi_msg - 하위 장치가 쓸 MSI 메시지를 VMD 앞으로 조립한다
 *
 * @data: 하위 장치의 인터럽트를 나타내는 irq_data. chip_data 에 vmd_irq 가 있다.
 * @msg: 채워 넣을 MSI 메시지 구조체. 이 값이 그대로 하위 장치의 MSI-X
 *   테이블에 기록되어, 장치가 인터럽트를 낼 때 쓰는 목적지 주소가 된다.
 * @return: 없음.
 *
 * 이 함수가 인터럽트 중계의 출발점이다. 보통의 MSI 라면 메시지의 목적지
 * ID 에 "이 인터럽트를 처리할 CPU 의 APIC ID" 가 들어간다. 그러나 VMD
 * 도메인에서는 하위 장치의 MSI 쓰기가 CPU 가 아니라 VMD 하드웨어에
 * 잡히므로, 목적지 ID 자리에 대신 "VMD 의 MSI-X 테이블 몇 번 항목" 을
 * 넣는다. 그래서 index_from_irqs() 가 준 배열 인덱스를 destid_0_7 에
 * 그대로 쓴다.
 *
 * 나머지 필드(address_hi/base_address)는 x86 의 MSI 주소 상수를 그대로
 * 쓴다. asm/irqdomain.h 를 포함하는 이유가 이 두 상수 때문이고, 이
 * 드라이버가 x86 전용인 이유이기도 하다.
 *
 * memset 으로 먼저 0 을 채우는 이유는 msi_msg 안에 이 함수가 건드리지
 * 않는 필드들이 있어, 남은 쓰레기 값이 하드웨어에 기록되면 안 되기
 * 때문이다.
 *
 * 실행 컨텍스트: MSI 설정 경로. 인터럽트가 꺼진 상태일 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   MSI 코어(irq_chip_compose_msi_msg) → [이 함수] → index_from_irqs()
 *   struct irq_chip vmd_msi_controller 의 .irq_compose_msi_msg 슬롯이다.
 */
static void vmd_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct vmd_irq *vmdirq = data->chip_data;	/* [한국어] chip_data 에 vmd_msi_alloc() 이 매달아 둔 이 인터럽트의 중계 정보 */
	struct vmd_irq_list *irq = vmdirq->irq;	/* [한국어] 그 인터럽트가 실릴 VMD 벡터 */
	struct vmd_dev *vmd = irq_data_get_irq_handler_data(data);	/* [한국어] handler_data 에 매달아 둔 vmd_dev. 배열 시작 주소를 알아야 인덱스를 구할 수 있다 */

	memset(msg, 0, sizeof(*msg));	/* [한국어] 건드리지 않는 필드에 쓰레기가 남아 하드웨어에 기록되는 것을 막는다 */
	msg->address_hi = X86_MSI_BASE_ADDRESS_HIGH;	/* [한국어] x86 MSI 주소의 상위 32 비트. 고정 상수다 */
	msg->arch_addr_lo.base_address = X86_MSI_BASE_ADDRESS_LOW;	/* [한국어] x86 MSI 주소의 하위 기본부. 여기까지는 보통의 MSI 와 같다 */
	msg->arch_addr_lo.destid_0_7 = index_from_irqs(vmd, irq);	/* [한국어] 여기가 핵심. 보통이면 목적지 CPU 의 APIC ID 가 들어갈 자리에, VMD 의 MSI-X 테이블 인덱스를 넣는다. 이 장치의 인터럽트를 VMD 의 그 벡터로 보내라는 뜻이다 */
}

/* [한국어]
 * vmd_irq_enable - 하위 인터럽트를 VMD 벡터의 중계 목록에 매단다
 *
 * @data: 하위 인터럽트의 irq_data. chip_data 가 vmd_irq 다.
 * @return: 없음.
 *
 * 중계의 실체는 목록이다. VMD 벡터가 울리면 vmd_irq() 가 그 벡터의
 * 목록을 훑어 달려 있는 하위 인터럽트를 모두 호출한다. 그러므로
 * "인터럽트를 켠다" 는 곧 "그 목록에 들어간다" 는 뜻이다.
 *
 * scoped_guard(raw_spinlock_irqsave, &list_lock) 은 블록 끝에서 자동으로
 * 락을 푸는 커널의 최신 관용구다. raw 이고 irqsave 인 이유는 이 락을
 * 인터럽트가 꺼진 문맥에서도 잡기 때문이다.
 *
 * list_add_tail_rcu 를 쓰는 이유는 vmd_irq() 가 SRCU 읽기 구간에서 락 없이
 * 이 목록을 순회하기 때문이다. _rcu 변형은 새 노드를 완전히 준비한 뒤에야
 * 목록에 보이도록 쓰기 순서를 강제한다.
 *
 * WARN_ON(vmdirq->enabled) 은 이중 활성화를 잡는 검사다. 이미 목록에
 * 있는데 또 넣으면 목록이 망가진다.
 *
 * 실행 컨텍스트: 인터럽트 설정 경로. 잠들 수 없다.
 *
 * 에러 경로: 없다. 잘못된 상태는 경고만 남기고 진행한다(상류 코드 그대로).
 *
 * 호출 체인:
 *   vmd_pci_msi_enable() → [이 함수]
 */
static void vmd_irq_enable(struct irq_data *data)
{
	struct vmd_irq *vmdirq = data->chip_data;	/* [한국어] 이 인터럽트의 중계 정보 */

	scoped_guard(raw_spinlock_irqsave, &list_lock) {	/* [한국어] 블록을 벗어날 때 자동으로 락을 푸는 관용구. raw + irqsave 라 인터럽트를 끄고 잡는다 */
		WARN_ON(vmdirq->enabled);	/* [한국어] 이미 목록에 있는데 또 넣으려는 상황을 잡는다. 그대로 두면 목록이 순환한다 */
		list_add_tail_rcu(&vmdirq->node, &vmdirq->irq->irq_list);	/* [한국어] 꼬리에 매단다. _rcu 변형이라 노드가 완전히 준비된 뒤에야 독자에게 보인다 */
		vmdirq->enabled = true;	/* [한국어] 목록에 들어갔음을 기록. disable 이 이 값을 보고 이중 제거를 피한다 */
	}	/* [한국어] 여기서 락이 자동으로 풀린다 */
}

/* [한국어]
 * vmd_pci_msi_enable - 하위 장치 쪽 마스크 해제와 중계 목록 등록을 함께
 *
 * @data: 하위 장치 계층의 irq_data.  @return: 없음.
 *
 * MSI 인터럽트는 두 계층으로 되어 있다. 위쪽은 PCI/MSI-X 계층(장치의
 * MSI-X 테이블 마스크 비트), 아래쪽은 이 드라이버의 VMD 중계 계층이다.
 * 인터럽트를 켜려면 둘 다 켜야 한다.
 *
 * 순서에 의미가 있다. 먼저 data->parent_data 로 부모(VMD) 계층을 목록에
 * 등록하고, 그다음 data->chip->irq_unmask 로 장치의 마스크를 푼다.
 * 반대로 하면 아직 중계 목록에 없는 상태에서 장치가 인터럽트를 낼 수
 * 있고, 그 인터럽트는 갈 곳이 없어 사라진다.
 *
 * 실행 컨텍스트: 인터럽트 설정 경로.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   MSI 코어 → [이 함수] → vmd_irq_enable(), chip->irq_unmask()
 *   vmd_init_dev_msi_info() 가 이 함수를 info->chip->irq_enable 에 꽂는다.
 */
static void vmd_pci_msi_enable(struct irq_data *data)
{
	vmd_irq_enable(data->parent_data);	/* [한국어] 먼저 부모(VMD) 계층을 중계 목록에 등록한다 */
	data->chip->irq_unmask(data);	/* [한국어] 그다음 장치의 MSI-X 마스크를 푼다. 순서를 바꾸면 목록에 없는 동안 나온 인터럽트가 사라진다 */
}

/* [한국어]
 * vmd_pci_msi_startup - irq_startup 슬롯. enable 과 같은 일을 하고 0 을 돌려준다
 *
 * @data: 하위 장치 계층의 irq_data.
 * @return: 항상 0. irq_chip 의 startup 규약상 "인터럽트가 이미 보류
 *   중이었는가" 를 알리는 값인데, 여기서는 알 방법이 없어 0(아니오)이다.
 *
 * 커널 irq 코어에는 startup 과 enable 이 따로 있다. startup 은 처음
 * 요청될 때 한 번, enable 은 그 뒤로 켜고 끌 때마다 불린다. 이 드라이버는
 * 둘을 구분할 이유가 없어 startup 을 enable 로 넘긴다.
 *
 * 실행 컨텍스트: request_irq 경로.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   irq 코어(irq_startup) → [이 함수] → vmd_pci_msi_enable()
 */
static unsigned int vmd_pci_msi_startup(struct irq_data *data)
{
	vmd_pci_msi_enable(data);	/* [한국어] startup 과 enable 을 구분할 이유가 없어 그대로 넘긴다 */
	return 0;	/* [한국어] "이미 보류 중인 인터럽트는 없다" 는 뜻. 알 방법이 없으므로 늘 0 이다 */
}

/* [한국어]
 * vmd_irq_disable - 하위 인터럽트를 중계 목록에서 뗀다
 *
 * @data: 하위 인터럽트의 irq_data.  @return: 없음.
 *
 * vmd_irq_enable() 의 짝. 목록에서 빠지면 VMD 벡터가 울려도 이 인터럽트는
 * 호출되지 않는다.
 *
 * enabled 를 먼저 확인하는 이유는, 이미 꺼진 것을 또 끄는 호출이 정상적으로
 * 있을 수 있기 때문이다(그래서 enable 쪽처럼 WARN 을 내지 않는다).
 *
 * list_del_rcu 는 노드를 목록에서 떼되 그 노드의 next 포인터는 그대로 두어,
 * 지금 순회 중인 독자가 계속 앞으로 나아갈 수 있게 한다. 노드를 실제로
 * 해제하는 것은 vmd_msi_free() 이며, 거기서 synchronize_srcu() 로 모든
 * 독자가 빠져나가기를 기다린 뒤에 kfree 한다.
 *
 * 실행 컨텍스트: 인터럽트 설정 경로. 잠들 수 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vmd_pci_msi_disable() → [이 함수]
 */
static void vmd_irq_disable(struct irq_data *data)
{
	struct vmd_irq *vmdirq = data->chip_data;	/* [한국어] 이 인터럽트의 중계 정보 */

	scoped_guard(raw_spinlock_irqsave, &list_lock) {	/* [한국어] enable 과 같은 락. 목록 조작은 반드시 이 안에서 */
		if (vmdirq->enabled) {	/* [한국어] 이미 꺼져 있으면 아무것도 하지 않는다. 이중 disable 은 정상적으로 일어날 수 있다 */
			list_del_rcu(&vmdirq->node);	/* [한국어] 목록에서 뗀다. 실제 해제는 vmd_msi_free() 가 SRCU 유예 뒤에 한다 */
			vmdirq->enabled = false;	/* [한국어] 꺼졌음을 기록 */
		}	/* [한국어] if 끝 */
	}
}

/* [한국어]
 * vmd_pci_msi_disable - 장치 마스크와 중계 목록 제거를 함께
 *
 * @data: 하위 장치 계층의 irq_data.  @return: 없음.
 *
 * vmd_pci_msi_enable() 의 거울상이며 순서도 정확히 뒤집혀 있다. 먼저
 * 장치의 MSI-X 마스크를 걸어 새 인터럽트가 나오지 않게 하고, 그다음
 * 중계 목록에서 뗀다. 반대로 하면 목록에서 빠진 뒤에 장치가 낸 인터럽트가
 * 갈 곳을 잃는다.
 *
 * 실행 컨텍스트: 인터럽트 설정 경로.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   MSI 코어 → [이 함수] → chip->irq_mask(), vmd_irq_disable()
 */
static void vmd_pci_msi_disable(struct irq_data *data)
{
	data->chip->irq_mask(data);
	vmd_irq_disable(data->parent_data);
}

/* [한국어]
 * vmd_pci_msi_shutdown - irq_shutdown 슬롯. disable 로 넘긴다
 *
 * @data: 하위 장치 계층의 irq_data.  @return: 없음.
 *
 * vmd_pci_msi_startup() 과 대칭이다. shutdown 과 disable 을 구분할 이유가
 * 없어 한쪽으로 몰아준다.
 *
 * 실행 컨텍스트: free_irq 경로.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   irq 코어(irq_shutdown) → [이 함수] → vmd_pci_msi_disable()
 */
static void vmd_pci_msi_shutdown(struct irq_data *data)
{
	vmd_pci_msi_disable(data);
}

/* [한국어]
 * vmd_msi_controller - 하위 인터럽트에 붙는 irq_chip
 *
 * vmd_msi_alloc() 이 irq_domain_set_info() 로 각 하위 인터럽트에 이것을
 * 지정한다. 슬롯이 하나뿐인 것이 특징이다 — MSI 메시지를 조립하는 일만
 * 이 드라이버가 맡고, 마스크/언마스크 같은 나머지는 상위 PCI/MSI 계층이
 * 처리한다.
 *
 * 설정자: 컴파일 시점 상수.
 * 읽는 자: irq 코어. 이름 "VMD-MSI" 는 /proc/interrupts 등에 뜬다.
 * 값 범위: .irq_compose_msi_msg 외의 슬롯은 NULL 이며, 코어가 그것을
 *   "해당 동작 없음" 으로 다룬다.
 * 동기화: 읽기 전용.
 */
static struct irq_chip vmd_msi_controller = {
	.name			= "VMD-MSI",	/* [한국어] 인터럽트 통계에 표시될 칩 이름 */
	.irq_compose_msi_msg	= vmd_compose_msi_msg,	/* [한국어] 하위 장치의 MSI-X 테이블에 쓸 메시지를 VMD 앞으로 조립한다 */
};

/*
 * XXX: We can be even smarter selecting the best IRQ once we solve the
 * affinity problem.
 */
/* [한국어]
 * vmd_next_irq - 이 하위 인터럽트를 어느 VMD 벡터에 실을지 고른다
 *
 * @vmd: 이 VMD 인스턴스.
 * @desc: 요청하는 하위 장치의 MSI 기술자. 여기서 장치를 되짚어 클래스
 *   코드를 볼 수 있다.
 * @return: 고른 벡터의 vmd_irq_list.
 *
 * 이 파일에서 NVMe 를 이름으로 지목하는 유일한 곳이다. 정책은 이렇다.
 *
 *   (1) 쓸 수 있는 벡터가 하나뿐이면 선택의 여지가 없다.
 *   (2) 요청한 장치의 클래스가 PCI_CLASS_STORAGE_EXPRESS(NVMe)가
 *       아니면 first_vec 하나에 몰아넣는다. 원본 주석이 그것을 "느린"
 *       공용 벡터라고 부른다.
 *   (3) NVMe 라면 first_vec+1 부터 끝까지 훑어 count 가 가장 작은 벡터를
 *       고르고 그 count 를 올린다. 즉 NVMe 컨트롤러들끼리 벡터를 고르게
 *       나눠 갖는다.
 *
 * 왜 이런 차별이 있는가. VMD 뒤에 붙는 것은 사실상 NVMe SSD 이고, 그
 * 인터럽트는 초당 수십만 번 발생하는 성능의 핵심이다. 반면 나머지 장치의
 * 인터럽트는 드물다. 드문 것들을 한 벡터에 몰아 두면 잦은 것들이 쓸 수
 * 있는 벡터가 늘어난다. 한 벡터를 여럿이 공유하면 vmd_irq() 가 목록을
 * 훑으며 모두를 호출하므로, 공유가 적을수록 지연이 짧다.
 *
 * 함수 위의 원본 XXX 주석은 이 선택이 CPU 친화도를 고려하지 않는다는
 * 사실을 스스로 인정한 것이다. 실제로 vmd_msi_parent_ops 는
 * MSI_FLAG_NO_AFFINITY 를 필수 플래그로 걸어 친화도 설정을 아예 막는다.
 *
 * 실행 컨텍스트: MSI 할당 경로(vmd_msi_alloc). scoped_guard(raw_spinlock_irq)
 * 로 list_lock 을 잡고 count 를 읽고 올린다.
 *
 * 에러 경로: 없다. 반드시 무언가를 돌려준다.
 *
 * 호출 체인:
 *   vmd_msi_alloc() → [이 함수]
 */
static struct vmd_irq_list *vmd_next_irq(struct vmd_dev *vmd, struct msi_desc *desc)
{
	int i, best;	/* [한국어] i 는 순회 커서, best 는 지금까지 가장 한가한 벡터의 인덱스 */

	if (vmd->msix_count == 1 + vmd->first_vec)	/* [한국어] 쓸 수 있는 벡터가 first_vec 하나뿐이라면 */
		return &vmd->irqs[vmd->first_vec];	/* [한국어] 고를 것이 없다 */

	/*
	 * White list for fast-interrupt handlers. All others will share the
	 * "slow" interrupt vector.
	 */
	switch (msi_desc_to_pci_dev(desc)->class) {	/* [한국어] 요청한 장치의 클래스 코드를 본다. msi_desc 에서 pci_dev 를 되짚는다 */
	case PCI_CLASS_STORAGE_EXPRESS:	/* [한국어] NVMe 컨트롤러의 24 비트 클래스 코드. Base 0x01(대용량 저장), Sub 0x08(비휘발성 메모리), Prog-IF 0x02(NVM Express) 를 합친 0x010802 다. 상수 정의(pci_ids.h)는 이 트리에 없어 값 자체는 확인하지 못했다. 이 파일에서 NVMe 를 지목하는 유일한 코드다 */
		break;	/* [한국어] 아래의 부하 분산 선택으로 내려간다 */
	default:	/* [한국어] NVMe 가 아닌 모든 장치는 */
		return &vmd->irqs[vmd->first_vec];	/* [한국어] first_vec 하나를 함께 쓴다. 원본 주석이 말하는 "느린" 공용 벡터다 */
	}

	scoped_guard(raw_spinlock_irq, &list_lock) {	/* [한국어] count 를 읽고 올리는 동안 다른 CPU 와 경쟁하면 안 된다. _irq 변형이라 인터럽트를 끄되 이전 상태는 저장하지 않는다(이 함수가 인터럽트 켜진 문맥에서만 불린다는 뜻) */
		best = vmd->first_vec + 1;	/* [한국어] 공용 벡터 다음부터가 NVMe 전용 후보다 */
		for (i = best; i < vmd->msix_count; i++)	/* [한국어] 남은 벡터를 전부 훑어 */
			if (vmd->irqs[i].count < vmd->irqs[best].count)	/* [한국어] 배정된 하위 인터럽트가 가장 적은 것을 찾는다 */
				best = i;	/* [한국어] 그것을 후보로 삼는다 */
		vmd->irqs[best].count++;	/* [한국어] 고른 벡터의 사용 수를 올린다. 다음 호출이 다른 벡터를 고르게 된다 */
	}	/* [한국어] 락 자동 해제 */

	return &vmd->irqs[best];	/* [한국어] 고른 벡터 */
}

/* [한국어] vmd_msi_free() 의 전방 선언. 정의는 아래 vmd_msi_alloc() 뒤에
 * 있지만, vmd_msi_alloc() 이 할당 실패 시 되돌리기로 이 함수를 부르기
 * 때문에 그보다 앞서 이름이 알려져 있어야 한다. 순환 참조를 푸는 흔한 방식이다. */
static void vmd_msi_free(struct irq_domain *domain, unsigned int virq,
			 unsigned int nr_irqs);

/* [한국어]
 * vmd_msi_alloc - 하위 장치가 요청한 MSI 인터럽트들을 VMD 벡터에 연결한다
 *
 * @domain: 이 VMD 의 MSI 부모 도메인.
 * @virq: 커널이 배정한 첫 가상 인터럽트 번호.
 * @nr_irqs: 요청 개수.
 * @arg: msi_alloc_info_t. 요청한 장치의 msi_desc 가 들어 있다.
 * @return: 0 성공, -ENOMEM 실패.
 *
 * irq_domain_ops 의 alloc 슬롯이다. 하위 장치가 pci_alloc_irq_vectors()
 * 같은 것을 부르면 결국 여기로 내려온다.
 *
 * 각 인터럽트마다 vmd_irq 하나를 만들고, vmd_next_irq() 로 실을 VMD
 * 벡터를 고른 뒤, irq_domain_set_info() 로 커널의 인터럽트 서술 구조에
 * 등록한다. 그 호출의 인자 하나하나가 중계의 배선이다 — 부모 virq 를
 * VMD 벡터의 virq 로 지정하고, irq_chip 을 vmd_msi_controller 로 두어
 * MSI 메시지 조립이 vmd_compose_msi_msg() 로 가게 하고, chip_data 에
 * 방금 만든 vmd_irq 를 매달고, handler_data 에 vmd 를 매단다.
 *
 * handle_untracked_irq 를 핸들러로 쓰는 것이 눈에 띈다. 이 하위 인터럽트는
 * 실제 하드웨어 선에 대응하지 않고 vmd_irq() 가 소프트웨어로 호출하는
 * 것이므로, 커널의 일반적인 인터럽트 흐름 추적(통계, 재진입 방지)을
 * 거치지 않는 가벼운 핸들러를 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(kzalloc 이 GFP_KERNEL).
 *
 * 에러 경로: 중간에 할당이 실패하면 지금까지 만든 i 개를 vmd_msi_free()
 * 로 되돌리고 -ENOMEM 을 반환한다. 이 되돌리기 때문에 파일 앞쪽에
 * vmd_msi_free() 의 전방 선언이 필요하다.
 *
 * 호출 체인:
 *   하위 장치의 MSI 요청 → irq 도메인 코어 → [이 함수]
 *   → vmd_next_irq(), irq_domain_set_info()
 */
static int vmd_msi_alloc(struct irq_domain *domain, unsigned int virq,
			 unsigned int nr_irqs, void *arg)
{
	struct msi_desc *desc = ((msi_alloc_info_t *)arg)->desc;	/* [한국어] 요청한 장치의 MSI 기술자. vmd_next_irq() 가 클래스 코드를 보는 데 쓴다 */
	struct vmd_dev *vmd = domain->host_data;	/* [한국어] vmd_create_irq_domain() 이 host_data 에 넣어 둔 인스턴스 */
	struct vmd_irq *vmdirq;	/* [한국어] 루프 안에서 만들 중계 정보 */

	for (int i = 0; i < nr_irqs; ++i) {	/* [한국어] 요청한 개수만큼 반복. C99 선언을 루프 안에 두는 최신 커널 스타일이다 */
		vmdirq = kzalloc_obj(*vmdirq);	/* [한국어] 중계 정보 한 칸을 0 초기화 할당 */
		if (!vmdirq) {	/* [한국어] 할당 실패면 */
			vmd_msi_free(domain, virq, i);	/* [한국어] 지금까지 만든 i 개를 되돌린다. 이 호출 때문에 위쪽에 전방 선언이 필요하다 */
			return -ENOMEM;	/* [한국어] 실패 반환 */
		}

		INIT_LIST_HEAD(&vmdirq->node);	/* [한국어] 목록 노드를 자기 자신을 가리키게 초기화. 아직 어느 목록에도 매달지 않는다 — 그것은 enable 의 몫이다 */
		vmdirq->irq = vmd_next_irq(vmd, desc);	/* [한국어] 어느 VMD 벡터에 실을지 고른다. NVMe 특례가 여기서 적용된다 */
		vmdirq->virq = virq + i;	/* [한국어] 이 하위 인터럽트에 배정된 리눅스 가상 IRQ 번호 */

		irq_domain_set_info(domain, virq + i, vmdirq->irq->virq,	/* [한국어] 중계의 배선을 커널 irq 코어에 등록한다. 세 번째 인자가 부모 IRQ — 고른 VMD 벡터의 virq 다 */
				    &vmd_msi_controller, vmdirq,	/* [한국어] irq_chip 은 vmd_msi_controller(MSI 메시지 조립만 담당), chip_data 는 방금 만든 중계 정보 */
				    handle_untracked_irq, vmd, NULL);	/* [한국어] 핸들러는 handle_untracked_irq — 실제 하드웨어 선이 아니라 vmd_irq() 가 소프트웨어로 부르는 것이라 흐름 추적이 필요 없다. 그다음이 handler_data 로 매달 vmd */
	}

	return 0;	/* [한국어] 모두 성공 */
}

/* [한국어]
 * vmd_msi_free - 하위 인터럽트와 VMD 벡터의 연결을 끊고 해제한다
 *
 * @domain: 이 VMD 의 MSI 부모 도메인.
 * @virq: 해제할 첫 가상 인터럽트 번호.  @nr_irqs: 개수.
 * @return: 없음.
 *
 * irq_domain_ops 의 free 슬롯이자, vmd_msi_alloc() 의 실패 되돌리기이기도
 * 하다. 후자로 쓰일 때는 nr_irqs 에 "지금까지 만든 개수" 가 들어온다.
 *
 * 각 인터럽트마다 세 단계를 밟는다.
 *   (1) synchronize_srcu(&vmdirq->irq->srcu) — 이 벡터의 목록을 순회 중인
 *       vmd_irq() 가 모두 빠져나가기를 기다린다. 이것이 없으면 하드 IRQ
 *       문맥에서 방금 해제한 메모리를 따라가는 사고가 난다. 이 호출이
 *       잠들 수 있으므로 이 함수는 반드시 프로세스 컨텍스트에서 불려야 한다.
 *   (2) list_lock 아래에서 그 벡터의 count 를 줄인다. 다음 할당 때
 *       vmd_next_irq() 가 이 벡터를 다시 고를 수 있게 하기 위해서다.
 *       원본의 XXX 주석은 여기서 남은 인터럽트들을 재분배하면 더 좋겠다는
 *       개선 아이디어를 남긴 것이다.
 *   (3) kfree.
 *
 * 목록에서 떼는 일(list_del_rcu)은 여기가 아니라 vmd_irq_disable() 이
 * 한다. irq 코어가 free 앞서 disable 을 부르는 것을 전제한 구조다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. synchronize_srcu 가 잠든다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   irq 도메인 코어 또는 vmd_msi_alloc() 의 실패 경로 → [이 함수]
 *   → synchronize_srcu(), kfree()
 */
static void vmd_msi_free(struct irq_domain *domain, unsigned int virq,
			 unsigned int nr_irqs)
{
	struct irq_data *irq_data;	/* [한국어] 각 인터럽트의 irq_data 를 받을 자리 */
	struct vmd_irq *vmdirq;	/* [한국어] 그 안의 chip_data 에서 꺼낼 중계 정보 */

	for (int i = 0; i < nr_irqs; ++i) {	/* [한국어] 해제할 개수만큼 반복 */
		irq_data = irq_domain_get_irq_data(domain, virq + i);	/* [한국어] virq 번호로부터 커널의 irq_data 를 얻는다 */
		vmdirq = irq_data->chip_data;	/* [한국어] vmd_msi_alloc() 이 매달아 둔 중계 정보를 되찾는다 */

		synchronize_srcu(&vmdirq->irq->srcu);	/* [한국어] 이 벡터의 목록을 순회 중인 vmd_irq() 가 모두 빠져나가기를 기다린다. 이 호출이 잠들 수 있어 이 함수는 프로세스 컨텍스트 전용이다 */

		/* XXX: Potential optimization to rebalance */
		scoped_guard(raw_spinlock_irq, &list_lock)	/* [한국어] 벡터 사용 수를 줄이는 동안의 경쟁 방지 */
			vmdirq->irq->count--;	/* [한국어] 다음 할당 때 이 벡터가 다시 후보가 될 수 있게 한다 */

		kfree(vmdirq);	/* [한국어] 이제 아무도 참조하지 않으므로 안전하게 해제 */
	}
}

/* [한국어]
 * vmd_msi_domain_ops - 이 VMD 의 MSI 도메인이 인터럽트를 잡고 놓는 방법
 *
 * 설정자: 컴파일 시점 상수. vmd_create_irq_domain() 이 irq_domain_info 에
 *   담아 넘긴다.
 * 읽는 자: 커널 irq 도메인 코어. 하위 장치가 MSI 를 요청하거나 반납할 때
 *   이 두 함수가 불린다.
 * 값 범위: irq_domain_ops 에는 map/translate 등 다른 슬롯도 있지만
 *   MSI 도메인에는 alloc/free 만 있으면 된다.
 * 동기화: 읽기 전용. 두 함수 내부가 list_lock 과 SRCU 로 보호한다.
 */
static const struct irq_domain_ops vmd_msi_domain_ops = {
	.alloc		= vmd_msi_alloc,	/* [한국어] 하위 인터럽트를 VMD 벡터에 연결 */
	.free		= vmd_msi_free,		/* [한국어] 그 연결을 끊고 해제 */
};

/* [한국어]
 * vmd_init_dev_msi_info - 하위 장치의 MSI 도메인 정보에 VMD 전용 콜백을 끼워 넣는다
 *
 * @dev: MSI 를 요청하는 하위 장치.
 * @domain: 만들어지는 도메인.  @real_parent: 실제 부모 도메인.
 * @info: 채울 msi_domain_info.
 * @return: true 성공, false 실패(공통 헬퍼가 거부한 경우).
 *
 * 커널의 MSI 계층은 "부모 도메인이 자식 도메인의 설정을 손볼 기회" 를
 * 이 콜백으로 준다. 먼저 공통 헬퍼 msi_lib_init_dev_msi_info() 로 표준
 * 설정을 다 채우게 하고, 그 결과의 irq_chip 에서 네 슬롯만 이 드라이버의
 * 것으로 바꿔 끼운다 — startup / shutdown / enable / disable.
 *
 * 왜 이 네 개인가. 이들이 곧 "인터럽트를 켜고 끄는" 동작이고, VMD 에서는
 * 그것이 중계 목록에 넣고 빼는 일이기 때문이다. 나머지(마스크, 메시지
 * 기록 등)는 표준 PCI/MSI 처리가 그대로 맞다.
 *
 * 실행 컨텍스트: MSI 도메인 생성 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 공통 헬퍼가 false 를 돌려주면 그대로 false. 그러면 그 장치는
 * MSI 를 쓸 수 없다.
 *
 * 호출 체인:
 *   MSI 코어 → [이 함수] → msi_lib_init_dev_msi_info()
 *   struct msi_parent_ops vmd_msi_parent_ops 의 .init_dev_msi_info 슬롯이다.
 */
static bool vmd_init_dev_msi_info(struct device *dev, struct irq_domain *domain,
				  struct irq_domain *real_parent,
				  struct msi_domain_info *info)
{
	if (!msi_lib_init_dev_msi_info(dev, domain, real_parent, info))	/* [한국어] 공통 헬퍼가 표준 MSI 도메인 정보를 다 채우게 한다 */
		return false;	/* [한국어] 헬퍼가 거부하면 이 장치는 MSI 를 쓸 수 없다 */

	info->chip->irq_startup		= vmd_pci_msi_startup;	/* [한국어] 이하 네 줄이 이 드라이버의 개입 지점. 인터럽트를 켜고 끄는 동작만 VMD 중계용으로 바꿔 끼운다 */
	info->chip->irq_shutdown	= vmd_pci_msi_shutdown;	/* [한국어] shutdown 도 마찬가지 */
	info->chip->irq_enable		= vmd_pci_msi_enable;	/* [한국어] enable — 중계 목록에 넣기 + 장치 마스크 해제 */
	info->chip->irq_disable		= vmd_pci_msi_disable;	/* [한국어] disable — 장치 마스크 걸기 + 중계 목록에서 빼기 */
	return true;	/* [한국어] 성공 */
}

/* [한국어] 자식 MSI 도메인이 요청할 수 있는 기능의 상한. 일반 MSI 기능
 * 전부(MSI_GENERIC_FLAGS_MASK)와 MSI-X 를 허용한다. 이 마스크에 없는 것을
 * 자식이 요구하면 도메인 생성이 거부된다. */
#define VMD_MSI_FLAGS_SUPPORTED	(MSI_GENERIC_FLAGS_MASK | MSI_FLAG_PCI_MSIX)
/* [한국어] 자식 도메인에 무조건 걸리는 기능 둘.
 * MSI_FLAG_USE_DEF_DOM_OPS 는 기본 도메인 동작을 쓰라는 뜻이고,
 * MSI_FLAG_NO_AFFINITY 가 이 드라이버의 성격을 드러낸다 — 하위 장치의
 * 인터럽트는 CPU 친화도를 지정할 수 없다. 실제로 CPU 에 닿는 것은 VMD 의
 * 벡터이고 하위 인터럽트는 vmd_irq() 가 소프트웨어로 되뿌리는 것이라,
 * 하위 인터럽트마다 다른 CPU 를 지정한다는 개념이 성립하지 않기 때문이다.
 * vmd_next_irq() 위의 원본 XXX 주석이 말하는 affinity problem 이 이것이다. */
#define VMD_MSI_FLAGS_REQUIRED	(MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_NO_AFFINITY)

/* [한국어]
 * vmd_msi_parent_ops - 이 VMD 의 MSI 도메인이 "부모" 로서 갖는 성질
 *
 * 커널의 MSI 계층은 부모 도메인과 자식 도메인으로 나뉜다. 하위 장치마다
 * 자식 도메인이 생기고, 그 자식이 어떤 성질을 갖는지를 부모가 이 구조체로
 * 규정한다.
 *
 * 설정자: 컴파일 시점 상수.
 * 읽는 자: msi_create_parent_irq_domain() 과 그 뒤의 MSI 코어.
 * 값 범위: 아래 각 필드 주석 참조.
 * 동기화: 읽기 전용.
 */
static const struct msi_parent_ops vmd_msi_parent_ops = {
	.supported_flags	= VMD_MSI_FLAGS_SUPPORTED,	/* [한국어] 자식 도메인이 요청할 수 있는 기능의 상한 */
	.required_flags		= VMD_MSI_FLAGS_REQUIRED,	/* [한국어] 자식 도메인에 무조건 강제되는 기능 */
	.bus_select_token	= DOMAIN_BUS_VMD_MSI,	/* [한국어] 이 도메인의 종류 표식. 커널이 "VMD 용 MSI 도메인" 을 찾을 때 쓴다 */
	.bus_select_mask	= MATCH_PCI_MSI,	/* [한국어] 이 부모가 받아 줄 자식의 종류 — PCI MSI/MSI-X 만 */
	.prefix			= "VMD-",	/* [한국어] 자식 도메인 이름 앞에 붙일 접두사. debugfs 등에서 구분용 */
	.init_dev_msi_info	= vmd_init_dev_msi_info,	/* [한국어] 자식 도메인의 irq_chip 에 이 드라이버의 콜백을 끼워 넣는 자리 */
};

/* [한국어]
 * vmd_create_irq_domain - 이 VMD 의 MSI 부모 도메인을 만든다
 *
 * @vmd: 이 VMD 인스턴스.
 * @return: 0 성공, -ENODEV 실패.
 *
 * 하위 장치들의 MSI 요청을 받아 줄 도메인을 세운다. 도메인의 크기는
 * msix_count 이고, alloc/free 동작은 vmd_msi_domain_ops 가, 부모로서의
 * 성질은 vmd_msi_parent_ops 가 정의한다. host_data 에 vmd 를 넣어 두어
 * vmd_msi_alloc() 이 domain->host_data 로 인스턴스를 되찾는다.
 *
 * fwnode 는 이 도메인을 식별하는 이름표다. "VMD-MSI" 에 도메인 번호를
 * 붙여 만들므로, 한 시스템에 VMD 가 여럿이어도 /sys/kernel/debug/irq/domains
 * 등에서 구분된다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: fwnode 할당 실패면 -ENODEV. 도메인 생성 실패면 만들어 둔
 * fwnode 를 해제하고 -ENODEV. 되돌리기가 짝을 이루고 있다.
 *
 * 호출 체인:
 *   vmd_enable_domain() → [이 함수] → irq_domain_alloc_named_id_fwnode(),
 *   msi_create_parent_irq_domain()
 */
static int vmd_create_irq_domain(struct vmd_dev *vmd)
{
	struct irq_domain_info info = {	/* [한국어] 만들 도메인의 명세를 구조체 하나로 모아 넘긴다 */
		.size		= vmd->msix_count,	/* [한국어] 도메인 크기 = 확보한 VMD 벡터 개수 */
		.ops		= &vmd_msi_domain_ops,	/* [한국어] alloc/free 동작 정의 */
		.host_data	= vmd,	/* [한국어] vmd_msi_alloc() 이 domain->host_data 로 되찾을 인스턴스 */
	};

	info.fwnode = irq_domain_alloc_named_id_fwnode("VMD-MSI",	/* [한국어] 도메인을 식별할 이름표. "VMD-MSI" 에 도메인 번호를 붙여 */
						       vmd->sysdata.domain);	/* [한국어] VMD 가 여럿이어도 debugfs 등에서 구분되게 한다 */
	if (!info.fwnode)	/* [한국어] 이름표 할당 실패 */
		return -ENODEV;	/* [한국어] 도메인을 만들 수 없다 */

	vmd->irq_domain = msi_create_parent_irq_domain(&info,	/* [한국어] MSI 부모 도메인을 만든다. 이때부터 하위 장치의 MSI 요청이 이리로 들어온다 */
						       &vmd_msi_parent_ops);	/* [한국어] 부모로서의 성질(플래그, 자식 초기화 콜백)을 함께 넘긴다 */
	if (!vmd->irq_domain) {	/* [한국어] 도메인 생성 실패면 */
		irq_domain_free_fwnode(info.fwnode);	/* [한국어] 앞서 만든 이름표를 해제한다 — 되돌리기의 짝이다 */
		return -ENODEV;	/* [한국어] 실패 반환 */
	}

	return 0;	/* [한국어] 성공 */
}

/* [한국어]
 * vmd_set_msi_remapping - 하드웨어의 MSI 재매핑을 켜고 끈다
 *
 * @vmd: 이 VMD 인스턴스.
 * @enable: true 면 재매핑(중계)을 켠다, false 면 우회하게 한다.
 * @return: 없음.
 *
 * 비트 극성이 뒤집혀 있다는 점에 주의해야 한다. VMCONFIG_MSI_REMAP 비트는
 * "재매핑을 우회한다" 는 뜻이므로, 재매핑을 켜려면(enable=true) 그 비트를
 * 지워야 하고 끄려면 세워야 한다. 코드의 삼항 연산자가 정확히 그렇게 되어
 * 있는데, 이름만 보면 반대로 읽히기 쉬운 자리다.
 *
 * 재매핑을 켜면 하위 장치의 MSI 쓰기가 VMD 의 MSI-X 테이블로 잡히고,
 * 이 드라이버의 중계 계층이 필요해진다. 끄면 하위 장치의 MSI 가 그대로
 * 통과해 상위 인터럽트 컨트롤러에 닿는다.
 *
 * 실행 컨텍스트: probe, remove, resume 경로. config 접근이므로 프로세스
 * 컨텍스트가 적절하다.
 *
 * 에러 경로: 없다. config 읽기·쓰기 실패를 확인하지 않는다(상류 코드 그대로).
 *
 * 호출 체인:
 *   vmd_enable_domain() / vmd_remove_irq_domain() / vmd_resume()
 *   → [이 함수] → pci_read_config_ 계열, pci_write_config_ 계열
 */
static void vmd_set_msi_remapping(struct vmd_dev *vmd, bool enable)
{
	u16 reg;	/* [한국어] VMCONFIG 레지스터 값을 담을 자리 */

	pci_read_config_word(vmd->dev, PCI_REG_VMCONFIG, &reg);	/* [한국어] 현재 설정을 읽어 온다. 다른 비트를 보존해야 하므로 읽고 고쳐 쓰기(read-modify-write)를 한다 */
	reg = enable ? (reg & ~VMCONFIG_MSI_REMAP) :	/* [한국어] 비트 극성에 주의. VMCONFIG_MSI_REMAP 은 "우회한다" 는 뜻이므로 중계를 켜려면(enable) 이 비트를 지운다 */
		       (reg | VMCONFIG_MSI_REMAP);	/* [한국어] 끄려면 세운다 */
	pci_write_config_word(vmd->dev, PCI_REG_VMCONFIG, reg);	/* [한국어] 되돌려 쓴다 */
}

/* [한국어]
 * vmd_remove_irq_domain - MSI 도메인을 없애고 하드웨어 설정을 되돌린다
 *
 * @vmd: 이 VMD 인스턴스.  @return: 없음.
 *
 * 두 가지 일을 한다.
 *
 * 첫째, msix_count 가 0 이면 — 즉 이 인스턴스가 중계를 쓰지 않고 우회
 * 모드로 돌고 있었다면 — 재매핑을 다시 켜 둔다. 원본 주석이 이유를
 * 밝힌다: 일부 양산 BIOS 가 소프트 재부팅 사이에 재매핑을 켜 주지 않기
 * 때문에, 드라이버가 내려가기 전에 원래대로 돌려놓아야 한다.
 *
 * 둘째, 만들어 둔 irq_domain 이 있으면 없앤다. fwnode 포인터를 미리
 * 지역 변수에 챙겨 두는 이유는 irq_domain_remove() 가 도메인 구조체를
 * 해제해 버려 그 뒤에는 domain->fwnode 를 읽을 수 없기 때문이다.
 * 순서를 지키지 않으면 해제된 메모리를 읽는다.
 *
 * 실행 컨텍스트: remove/shutdown 경로, 그리고 vmd_enable_domain() 의
 * 실패 되돌리기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vmd_remove() / vmd_shutdown() / vmd_enable_domain() 실패 경로
 *   → [이 함수] → vmd_set_msi_remapping(), irq_domain_remove()
 */
static void vmd_remove_irq_domain(struct vmd_dev *vmd)
{
	/*
	 * Some production BIOS won't enable remapping between soft reboots.
	 * Ensure remapping is restored before unloading the driver.
	 */
	if (!vmd->msix_count)	/* [한국어] msix_count 가 0 이다 = 벡터를 확보하지 않았다 = 중계 우회 모드로 돌고 있었다 */
		vmd_set_msi_remapping(vmd, true);	/* [한국어] 원본 주석대로, 일부 BIOS 가 소프트 재부팅 때 재매핑을 켜 주지 않으므로 내려가기 전에 원래대로 돌려놓는다 */

	if (vmd->irq_domain) {	/* [한국어] MSI 도메인을 만들었다면 */
		struct fwnode_handle *fn = vmd->irq_domain->fwnode;	/* [한국어] 이름표 포인터를 미리 챙겨 둔다. 아래 remove 가 도메인 구조체를 해제해 버리므로 그 뒤에는 읽을 수 없다 */

		irq_domain_remove(vmd->irq_domain);	/* [한국어] 도메인 제거 */
		irq_domain_free_fwnode(fn);	/* [한국어] 그다음 이름표 해제. 순서를 바꾸면 해제된 메모리를 읽는다 */
	}
}

/* [한국어]
 * vmd_cfg_addr - 도메인 안 config 접근을 VMD BAR 안의 주소로 바꾼다
 *
 * @vmd: 이 VMD 인스턴스.  @bus: 대상 버스.  @devfn: 장치·함수 번호.
 * @reg: config space 안의 레지스터 오프셋.  @len: 접근 폭(1/2/4 바이트).
 * @return: 접근할 가상 주소. 창을 벗어나면 NULL.
 *
 * 이 계산이 "숨겨진 도메인" 이라는 개념의 실체다. VMD 의 BAR 0 안에는
 * 그 도메인 전체의 config space 가 ECAM(Enhanced Configuration Access
 * Mechanism) 배치로 통째로 들어 있다. ECAM 은 config 주소를
 *   (버스 << 20) | (devfn << 12) | 레지스터
 * 로 평평하게 펼치는 방식이다. 버스 하나가 1MB(32 장치 x 8 함수 x 4KB)를
 * 차지한다. PCIE_ECAM_OFFSET() 이 그 계산을 해 준다.
 *
 * 버스 번호에서 busn_start 를 먼저 빼는 이유는, 이 도메인의 버스 번호가
 * 0 이 아니라 128 이나 224 부터 시작할 수 있기 때문이다(버스 제한 기능).
 * BAR 안의 오프셋은 언제나 0 부터 세므로 상대 번호로 바꿔야 한다.
 *
 * 범위 검사는 offset + len 이 CFGBAR 크기를 넘는지 본다. 넘으면 NULL 을
 * 돌려주고 호출자가 -EFAULT 로 응답한다. 이 검사가 없으면 매핑 밖을
 * 읽어 커널이 죽는다.
 *
 * 실행 컨텍스트: config 접근 경로. 어느 컨텍스트에서든 불릴 수 있다.
 *
 * 에러 경로: 창을 벗어나면 NULL.
 *
 * 호출 체인:
 *   vmd_pci_read() / vmd_pci_write() → [이 함수] → PCIE_ECAM_OFFSET()
 */
static void __iomem *vmd_cfg_addr(struct vmd_dev *vmd, struct pci_bus *bus,
				  unsigned int devfn, int reg, int len)
{
	unsigned int busnr_ecam = bus->number - vmd->busn_start;	/* [한국어] ECAM 안의 오프셋은 0 번 버스부터 세므로, 이 도메인의 시작 버스 번호를 빼 상대 번호로 바꾼다 */
	u32 offset = PCIE_ECAM_OFFSET(busnr_ecam, devfn, reg);	/* [한국어] ECAM 배치 계산: (버스 << 20) | (devfn << 12) | 레지스터. 버스 하나가 1MB(32 장치 x 8 함수 x 4KB)를 차지한다 */

	if (offset + len >= resource_size(&vmd->dev->resource[VMD_CFGBAR]))	/* [한국어] 매핑한 창을 벗어나는 접근을 막는다. 이 검사가 없으면 커널이 죽는다 */
		return NULL;	/* [한국어] 호출자가 -EFAULT 로 응답한다 */

	return vmd->cfgbar + offset;	/* [한국어] 창 시작 주소에 오프셋을 더한 최종 가상 주소 */
}

/*
 * CPU may deadlock if config space is not serialized on some versions of this
 * hardware, so all config space access is done under a spinlock.
 */
/* [한국어]
 * vmd_pci_read - 숨겨진 도메인의 config space 읽기
 *
 * @bus: 대상 버스.  @devfn: 장치·함수.  @reg: 레지스터 오프셋.
 * @len: 1, 2, 4 중 하나.  @value: 읽은 값을 담을 곳.
 * @return: 0 성공, -EFAULT(주소가 창 밖), -EINVAL(폭이 이상함).
 *
 * struct pci_ops 의 read 슬롯이다. 이 도메인의 모든 config 읽기 —
 * 열거, capability 순회, BAR 크기 측정, 드라이버의 pci_read_config_ 계열
 * 호출 — 이 전부 여기를 지난다.
 *
 * 주소만 구하면 나머지는 평범한 MMIO 읽기다. 폭에 따라 readb/readw/readl 을
 * 고른다.
 *
 * guard(raw_spinlock_irqsave)(&vmd->cfg_lock) 이 이 함수의 핵심 제약이다.
 * 원본 주석이 밝히듯 이 하드웨어의 일부 버전은 config 접근을 직렬화하지
 * 않으면 CPU 가 교착에 빠진다. guard 는 함수가 어느 return 으로 빠져나가든
 * 자동으로 락을 푸는 관용구라, switch 안의 여러 return 이 있어도 안전하다.
 *
 * 실행 컨텍스트: 어디서든. 락이 raw + irqsave 인 것이 그 대비다.
 *
 * 에러 경로: 위 반환값 설명대로. -EFAULT 인 경우 *value 는 채워지지 않는데,
 * PCI 코어가 실패를 보면 0xffffffff 로 취급한다.
 *
 * 호출 체인:
 *   PCI 코어의 config 접근 → vmd_ops.read → [이 함수] → vmd_cfg_addr()
 */
static int vmd_pci_read(struct pci_bus *bus, unsigned int devfn, int reg,
			int len, u32 *value)
{
	struct vmd_dev *vmd = vmd_from_bus(bus);	/* [한국어] 이 버스가 속한 VMD 인스턴스를 sysdata 로부터 되찾는다 */
	void __iomem *addr = vmd_cfg_addr(vmd, bus, devfn, reg, len);	/* [한국어] 접근할 주소를 계산한다 */

	if (!addr)	/* [한국어] 창을 벗어났으면 */
		return -EFAULT;	/* [한국어] PCI 코어는 이 실패를 0xffffffff 로 취급한다 */

	guard(raw_spinlock_irqsave)(&vmd->cfg_lock);	/* [한국어] 하드웨어 요구사항. 원본 주석대로 일부 버전은 config 접근을 직렬화하지 않으면 CPU 가 교착한다. guard 는 어느 return 으로 빠져나가든 자동으로 락을 푼다 */
	switch (len) {	/* [한국어] PCI config 접근 폭은 1/2/4 바이트만 유효하다 */
	case 1:	/* [한국어] 1 바이트 */
		*value = readb(addr);	/* [한국어] MMIO 바이트 읽기 */
		return 0;	/* [한국어] 성공(락은 guard 가 푼다) */
	case 2:	/* [한국어] 2 바이트 */
		*value = readw(addr);	/* [한국어] MMIO 워드 읽기 */
		return 0;	/* [한국어] 성공 */
	case 4:	/* [한국어] 4 바이트 */
		*value = readl(addr);	/* [한국어] MMIO 더블워드 읽기 */
		return 0;	/* [한국어] 성공 */
	default:	/* [한국어] 그 밖의 폭은 규격에 없다 */
		return -EINVAL;	/* [한국어] 잘못된 인자 */
	}
}

/*
 * VMD h/w converts non-posted config writes to posted memory writes. The
 * read-back in this function forces the completion so it returns only after
 * the config space was written, as expected.
 */
/* [한국어]
 * vmd_pci_write - 숨겨진 도메인의 config space 쓰기
 *
 * @bus: 대상 버스.  @devfn: 장치·함수.  @reg: 레지스터 오프셋.
 * @len: 1, 2, 4 중 하나.  @value: 쓸 값.
 * @return: 0 성공, -EFAULT, -EINVAL.
 *
 * 읽기와 대칭이지만 한 가지가 더 있다. 쓴 직후에 같은 주소를 다시 읽는다.
 * 원본 주석이 이유를 설명한다 — VMD 하드웨어는 non-posted 여야 할 config
 * 쓰기를 posted 메모리 쓰기로 바꿔 버린다. posted 쓰기는 하드웨어에
 * 도달했는지 확인하지 않고 곧바로 반환하므로, 쓰기가 실제로 반영되기 전에
 * 다음 코드가 진행될 수 있다. 읽기는 posted 가 아니므로, 한 번 읽어 주면
 * 그 앞의 쓰기가 완료될 때까지 기다리게 된다. PCI 규격이 config 쓰기에
 * 요구하는 완료 보장을 이렇게 되살린다.
 *
 * 이 되읽기가 없으면 예컨대 BAR 를 쓰고 곧바로 그 BAR 로 MMIO 접근을
 * 하는 코드가 옛 주소로 접근하는 사고가 난다.
 *
 * 실행 컨텍스트: 어디서든. cfg_lock 으로 직렬화한다.
 *
 * 에러 경로: 읽기와 같다.
 *
 * 호출 체인:
 *   PCI 코어의 config 접근 → vmd_ops.write → [이 함수] → vmd_cfg_addr()
 */
static int vmd_pci_write(struct pci_bus *bus, unsigned int devfn, int reg,
			 int len, u32 value)
{
	struct vmd_dev *vmd = vmd_from_bus(bus);	/* [한국어] 이 버스가 속한 VMD 인스턴스 */
	void __iomem *addr = vmd_cfg_addr(vmd, bus, devfn, reg, len);	/* [한국어] 접근할 주소 */

	if (!addr)	/* [한국어] 창 밖이면 */
		return -EFAULT;	/* [한국어] 실패 */

	guard(raw_spinlock_irqsave)(&vmd->cfg_lock);	/* [한국어] 읽기와 같은 락으로 직렬화 */
	switch (len) {	/* [한국어] 폭별 분기 */
	case 1:	/* [한국어] 1 바이트 */
		writeb(value, addr);	/* [한국어] 쓴다 */
		readb(addr);	/* [한국어] 되읽어 완료를 강제한다. VMD 는 config 쓰기를 posted 로 바꾸므로, 읽지 않으면 반영 전에 다음 코드가 진행된다 */
		return 0;	/* [한국어] 성공 */
	case 2:	/* [한국어] 2 바이트 */
		writew(value, addr);	/* [한국어] 쓴다 */
		readw(addr);	/* [한국어] 되읽어 완료 강제 */
		return 0;	/* [한국어] 성공 */
	case 4:	/* [한국어] 4 바이트 */
		writel(value, addr);	/* [한국어] 쓴다 */
		readl(addr);	/* [한국어] 되읽어 완료 강제 */
		return 0;	/* [한국어] 성공 */
	default:	/* [한국어] 그 밖의 폭 */
		return -EINVAL;	/* [한국어] 잘못된 인자 */
	}
}

/* [한국어]
 * vmd_ops - 이 도메인의 config space 접근 방법
 *
 * pci_create_root_bus() 에 넘겨져 이 도메인 아래 모든 버스의 ops 가 된다.
 * PCI 코어가 config 를 읽고 쓸 때마다 여기를 지나므로, 열거부터 드라이버의
 * pci_read_config_ 계열 호출까지 전부 이 두 함수를 통한다.
 *
 * 설정자: 컴파일 시점 상수.
 * 읽는 자: PCI 코어의 config 접근 경로. 그리고 vmd_acpi_find_companion() 이
 *   "이 버스가 VMD 도메인의 것인가" 를 판정하는 표식으로도 쓴다
 *   (bus->ops == &vmd_ops 비교).
 * 값 범위: 두 슬롯만 채운다. add_bus/remove_bus 같은 선택 슬롯은 없다.
 * 동기화: 읽기 전용. 두 함수 내부가 cfg_lock 으로 직렬화한다.
 */
static struct pci_ops vmd_ops = {
	.read		= vmd_pci_read,		/* [한국어] VMD BAR 안의 ECAM 위치에서 읽는다 */
	.write		= vmd_pci_write,	/* [한국어] 쓰고 나서 되읽어 완료를 강제한다 */
};

#ifdef CONFIG_ACPI
/* [한국어]
 * vmd_acpi_find_companion - VMD 뒤 장치에 대응하는 ACPI 노드를 찾아 준다
 *
 * @pci_dev: ACPI 짝을 찾고 있는 장치.
 * @return: 찾은 acpi_device, 없거나 대상이 아니면 NULL.
 *
 * 왜 특별한 처리가 필요한가. 보통의 PCI 장치는 ACPI 네임스페이스에서
 * 자기 부모 브리지 아래의 노드로 발견된다. 그런데 VMD 도메인 안의
 * 장치들은 펌웨어가 만든 도메인이 아니라 이 드라이버가 런타임에 만들어
 * 낸 도메인에 있으므로, 커널의 표준 탐색으로는 짝을 찾지 못한다.
 * 그래서 이 드라이버가 탐색 훅을 걸어 직접 찾아 준다.
 *
 * 계산의 근거는 ACPI 규격이 정한 _ADR 형식이다. PCI 장치의 _ADR 은
 * 상위 16 비트가 장치 번호, 하위 16 비트가 함수 번호다. 여기서 만드는
 * 주소는 거기에 상대 버스 번호까지 얹은
 *   (버스 << 24) | (devfn << 16) | 0x8000FFFF
 * 형태인데, 이것은 VMD 를 위해 펌웨어와 약속한 확장 형식이며 표준 _ADR
 * 이 아니다. 이 트리 안에서는 그 약속을 기술한 문서를 찾지 못했다.
 *
 * 버스 번호가 31 을 넘으면 NULL 을 돌려준다. 원본 주석대로 위 계산이
 * 상대 버스 번호 32 미만에서만 성립하기 때문이다.
 *
 * 첫 줄의 bus->ops != &vmd_ops 검사가 중요하다. 이 훅은 전역으로 걸리므로
 * 시스템의 모든 PCI 장치에 대해 불린다. VMD 도메인 안의 장치가 아니면
 * NULL 을 돌려주어 커널이 표준 탐색을 하게 둔다.
 *
 * 실행 컨텍스트: 장치 열거 중. 프로세스 컨텍스트. CONFIG_ACPI 에서만
 * 컴파일된다.
 *
 * 에러 경로: 못 찾으면 NULL. 그것이 정상적인 결과일 수 있다.
 *
 * 호출 체인:
 *   PCI 열거 → ACPI companion 탐색 → (훅) → [이 함수]
 *   → acpi_find_child_device()
 */
static struct acpi_device *vmd_acpi_find_companion(struct pci_dev *pci_dev)
{
	struct pci_host_bridge *bridge;	/* [한국어] 이 장치가 매달린 호스트 브리지를 받을 자리 */
	u32 busnr, addr;	/* [한국어] busnr 은 상대 버스 번호, addr 은 조립할 ACPI 주소 */

	if (pci_dev->bus->ops != &vmd_ops)	/* [한국어] 이 훅은 시스템의 모든 PCI 장치에 대해 불린다. config ops 가 vmd_ops 인지로 VMD 도메인 소속을 가려낸다 */
		return NULL;	/* [한국어] 아니면 커널이 표준 탐색을 하도록 NULL 을 돌려준다 */

	bridge = pci_find_host_bridge(pci_dev->bus);	/* [한국어] 이 도메인의 호스트 브리지(이 드라이버가 만든 것) */
	busnr = pci_dev->bus->number - bridge->bus->number;	/* [한국어] 루트 버스 번호를 빼 상대 버스 번호를 구한다 */
	/*
	 * The address computation below is only applicable to relative bus
	 * numbers below 32.
	 */
	if (busnr > 31)	/* [한국어] 원본 주석대로 아래 계산은 상대 버스 32 미만에서만 성립한다 */
		return NULL;	/* [한국어] 범위를 넘으면 찾아 줄 수 없다 */

	addr = (busnr << 24) | ((u32)pci_dev->devfn << 16) | 0x8000FFFFU;	/* [한국어] ACPI 주소 조립. 표준 _ADR 은 (장치<<16 | 함수) 인데 여기에 상대 버스를 24 비트 위로 얹고 하위 16 비트를 0xFFFF 로 채운 확장 형식이다. 펌웨어와의 약속이며 이 트리에서 그 문서를 찾지는 못했다 */

	dev_dbg(&pci_dev->dev, "Looking for ACPI companion (address 0x%x)\n",	/* [한국어] 디버그 로그. 어떤 주소로 찾고 있는지 남긴다 */
		addr);	/* [한국어] 조립한 주소 */

	return acpi_find_child_device(ACPI_COMPANION(bridge->dev.parent), addr,	/* [한국어] 호스트 브리지의 부모(진짜 루트 포트 쪽)의 ACPI 노드 아래에서 그 주소를 가진 자식을 찾는다 */
				      false);	/* [한국어] 세 번째 인자 false 는 "찾은 노드의 참조를 올리지 말라" 는 뜻이다 */
}

/* [한국어] ACPI companion 탐색 훅이 실제로 걸렸는지 기억하는 전역 플래그.
 * 설정자: vmd_acpi_begin() 이 훅 설치에 성공하면 true 로,
 *   vmd_acpi_end() 가 뗀 뒤 false 로.
 * 읽는 자: vmd_acpi_end() 가 "걸지도 않은 것을 떼려 하는" 상황을 막기 위해.
 * 값 범위: true/false.
 * 동기화: 전역인데 락이 없다. 훅은 시스템 전체에 하나뿐이고
 *   pci_acpi_set_companion_lookup_hook() 이 이미 걸려 있으면 실패를
 *   돌려주므로, VMD 가 둘이어도 한쪽만 true 를 갖게 된다. 그 경쟁 자체는
 *   훅 API 쪽이 막아 준다는 전제다(상류 코드 그대로). */
static bool hook_installed;

/* [한국어]
 * vmd_acpi_begin - VMD 도메인 열거 동안 companion 탐색 훅을 건다
 *
 * @return: 없음. 성공 여부는 hook_installed 에 남긴다.
 *
 * 커널이 PCI 장치의 ACPI 짝을 찾을 때 이 드라이버의
 * vmd_acpi_find_companion() 을 대신 부르게 만든다. VMD 도메인은 펌웨어가
 * 모르는 도메인이라 표준 탐색으로는 짝을 찾을 수 없기 때문이다.
 *
 * 반환값 없이 hook_installed 에만 결과를 남기는 것은, 훅이 걸리지 않아도
 * 열거 자체는 진행되어야 하기 때문이다(ACPI 짝이 없으면 그 장치의 전원
 * 관리나 핫플러그 통지가 제한될 뿐이다). 훅 설치가 실패하는 대표적인
 * 경우는 다른 VMD 인스턴스가 이미 걸어 둔 때다.
 *
 * pci_acpi_set_companion_lookup_hook() 이 0 이 아닌 값(오류)을 돌려주면
 * 그대로 반환하므로, hook_installed 는 거짓으로 남는다. 성공했을 때만
 * 참이 되어 vmd_acpi_end() 가 뗄 자격을 갖는다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트. CONFIG_ACPI 에서만 컴파일된다.
 *
 * 에러 경로: 조용히 넘어간다.
 *
 * 호출 체인:
 *   vmd_enable_domain() → [이 함수] → pci_acpi_set_companion_lookup_hook()
 */
static void vmd_acpi_begin(void)
{
	if (pci_acpi_set_companion_lookup_hook(vmd_acpi_find_companion))	/* [한국어] 훅을 건다. 0 이 아닌 값이면 실패(대개 이미 다른 인스턴스가 걸어 둔 경우) */
		return;	/* [한국어] 실패하면 hook_installed 를 거짓인 채로 두고 조용히 넘어간다 */

	hook_installed = true;	/* [한국어] 성공했음을 기록. 이 값이 있어야 vmd_acpi_end() 가 뗀다 */
}

/* [한국어]
 * vmd_acpi_end - companion 탐색 훅을 뗀다
 *
 * @return: 없음.
 *
 * vmd_acpi_begin() 의 짝. 훅은 시스템 전역에 하나뿐인 자원이므로, 열거가
 * 끝나면 곧바로 놓아 주어야 다른 VMD 인스턴스가 자기 열거 때 쓸 수 있다.
 * 그래서 vmd_enable_domain() 은 열거 구간만 begin/end 로 감싼다.
 *
 * hook_installed 를 먼저 확인하는 이유는, 걸지도 못한 상황에서 떼려 하면
 * 다른 인스턴스가 걸어 둔 훅을 빼앗게 되기 때문이다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트. CONFIG_ACPI 에서만 컴파일된다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vmd_enable_domain() → [이 함수] → pci_acpi_clear_companion_lookup_hook()
 */
static void vmd_acpi_end(void)
{
	if (!hook_installed)	/* [한국어] 걸지 못했다면 */
		return;	/* [한국어] 뗄 것이 없다. 남의 훅을 빼앗으면 안 된다 */

	pci_acpi_clear_companion_lookup_hook();	/* [한국어] 훅을 뗀다. 이제 커널이 다시 표준 탐색을 한다 */
	hook_installed = false;	/* [한국어] 상태 원복 */
}
#else
/* [한국어]
 * vmd_acpi_begin / vmd_acpi_end - (CONFIG_ACPI 없는 빌드) 아무것도 하지 않는 짝
 *
 * @return: 없음.
 *
 * ACPI 가 없는 커널에서는 companion 탐색 훅이라는 개념 자체가 없다.
 * 호출부에 #ifdef 를 흩뿌리지 않으려고 같은 이름의 빈 inline 함수를 둔다.
 * 컴파일러가 통째로 지우므로 실행 비용이 0 이다.
 *
 * 실행 컨텍스트: vmd_enable_domain() 의 열거 전후. 아무 일도 하지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vmd_enable_domain() → [이 함수들]
 */
static inline void vmd_acpi_begin(void) { }	/* [한국어] 훅을 걸 것이 없으므로 빈 몸통. 컴파일러가 통째로 지운다 */
static inline void vmd_acpi_end(void) { }	/* [한국어] 뗄 것도 없으므로 빈 몸통 */
#endif /* CONFIG_ACPI */

/* [한국어]
 * vmd_domain_reset - 도메인 안 모든 브리지의 자원 창 레지스터를 초기화한다
 *
 * @vmd: 이 VMD 인스턴스.  @return: 없음.
 *
 * 열거 직후에 도메인 안의 PCI-to-PCI 브리지를 전부 찾아 I/O 창과 메모리
 * 창 레지스터를 "닫힌" 상태로 되돌린다. 부팅 펌웨어나 이전 커널이 남겨
 * 놓은 값이 그대로 있으면 이후의 자원 배정이 그것과 충돌할 수 있다.
 *
 * 훑는 방식은 무식하지만 확실하다 — 버스 0 부터 resources[0] 크기만큼,
 * 각 버스의 장치 0~31 을 돈다. 각 장치의 함수 0 에서 Header Type 을 읽어
 * 다기능 장치면 8 함수, 아니면 1 함수만 본다. 그리고 헤더 타입이 브리지
 * (PCI_HEADER_TYPE_BRIDGE)이고 클래스가 PCI-to-PCI 브리지인 것만 손댄다.
 *
 * 쓰는 값들에 규칙이 있다. PCI 브리지의 창 레지스터는 base 가 limit 보다
 * 크면 "창이 닫혔다" 는 뜻이다. 그래서 I/O 는 base=0xf0, limit=0x00 이
 * 되도록 0x00f0 을 쓰고, 메모리는 base=0xfff0, limit=0x0000 이 되도록
 * 0x0000fff0 을 쓴다. 32 비트 쓰기 한 번으로 base 와 limit 를 함께 넣는
 * 것이라, 하위 16 비트가 base, 상위 16 비트가 limit 다.
 *
 * I/O 상위 16 비트를 먼저 0x0000ffff 로 만들었다가 마지막에 0 으로 되돌리는
 * 순서는 원본 주석이 밝히듯 "PCI_IO_BASE 를 바꾸기 전에 I/O 범위를 잠깐
 * 무효로 만들기" 위한 것이다. 중간 상태에서 유효한 창이 잠깐 생기는 것을
 * 막는다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트. cfgbar 를 직접 readb/writel
 * 하므로 vmd_pci_read/write 를 거치지 않는다는 점에 유의 — cfg_lock 을
 * 잡지 않는다. probe 중이라 아직 다른 접근자가 없다는 전제다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vmd_enable_domain() → [이 함수]
 */
static void vmd_domain_reset(struct vmd_dev *vmd)
{
	u16 bus, max_buses = resource_size(&vmd->resources[0]);	/* [한국어] bus 는 순회 커서, max_buses 는 이 도메인이 담당하는 버스 개수(resources[0] 의 크기) */
	u8 dev, functions, fn, hdr_type;	/* [한국어] dev 는 장치 번호, functions 는 이 장치의 함수 개수, fn 은 함수 커서, hdr_type 은 읽어 온 헤더 타입 */
	char __iomem *base;	/* [한국어] ECAM 안의 접근 주소 */

	for (bus = 0; bus < max_buses; bus++) {	/* [한국어] 담당 버스 전부를 훑는다 */
		for (dev = 0; dev < 32; dev++) {	/* [한국어] 한 버스에 장치 번호는 0~31 (PCI 규격의 5 비트 장치 번호) */
			base = vmd->cfgbar + PCIE_ECAM_OFFSET(bus,	/* [한국어] 함수 0 의 config 시작 주소를 계산한다 */
						PCI_DEVFN(dev, 0), 0);	/* [한국어] PCI_DEVFN(dev, 0) 은 (dev << 3) | 0 */

			hdr_type = readb(base + PCI_HEADER_TYPE);	/* [한국어] Header Type 레지스터(오프셋 0x0e)를 읽는다 */

			functions = (hdr_type & PCI_HEADER_TYPE_MFD) ? 8 : 1;	/* [한국어] bit 7(PCI_HEADER_TYPE_MFD)이 서 있으면 다기능 장치라 함수 8 개를 다 봐야 한다 */
			for (fn = 0; fn < functions; fn++) {	/* [한국어] 각 함수를 순회 */
				base = vmd->cfgbar + PCIE_ECAM_OFFSET(bus,	/* [한국어] 그 함수의 config 시작 주소 */
						PCI_DEVFN(dev, fn), 0);	/* [한국어] PCI_DEVFN(dev, fn) 은 (dev << 3) | fn */

				hdr_type = readb(base + PCI_HEADER_TYPE) &	/* [한국어] Header Type 을 다시 읽되 */
						PCI_HEADER_TYPE_MASK;	/* [한국어] 다기능 비트를 지우고 하위 7 비트의 타입만 남긴다 */

				if (hdr_type != PCI_HEADER_TYPE_BRIDGE ||	/* [한국어] 타입 1(PCI-to-PCI 브리지)이 아니거나 */
				    (readw(base + PCI_CLASS_DEVICE) !=	/* [한국어] 클래스 코드(오프셋 0x0a)가 */
				     PCI_CLASS_BRIDGE_PCI))	/* [한국어] PCI-to-PCI 브리지가 아니면 */
					continue;	/* [한국어] 건드리지 않는다. 창 레지스터가 있는 것은 브리지뿐이다 */

				/*
				 * Temporarily disable the I/O range before updating
				 * PCI_IO_BASE.
				 */
				writel(0x0000ffff, base + PCI_IO_BASE_UPPER16);	/* [한국어] I/O 상위 16 비트를 base=0xffff, limit=0xffff 로 만들어 범위를 잠시 무효화한다(원본 주석의 의도) */
				/* Update lower 16 bits of I/O base/limit */
				writew(0x00f0, base + PCI_IO_BASE);	/* [한국어] I/O 하위 바이트: base=0xf0, limit=0x00. base > limit 이므로 창이 닫힌다 */
				/* Update upper 16 bits of I/O base/limit */
				writel(0, base + PCI_IO_BASE_UPPER16);	/* [한국어] I/O 상위 16 비트를 0 으로 되돌린다 */

				/* MMIO Base/Limit */
				writel(0x0000fff0, base + PCI_MEMORY_BASE);	/* [한국어] MMIO base=0xfff0, limit=0x0000. 하위 워드가 base, 상위 워드가 limit 이므로 32 비트 한 번에 둘 다 쓴다 */

				/* Prefetchable MMIO Base/Limit */
				writel(0, base + PCI_PREF_LIMIT_UPPER32);	/* [한국어] prefetchable 상위 32 비트의 limit 을 0 으로 */
				writel(0x0000fff0, base + PCI_PREF_MEMORY_BASE);	/* [한국어] prefetchable base=0xfff0, limit=0x0000 */
				writel(0xffffffff, base + PCI_PREF_BASE_UPPER32);	/* [한국어] prefetchable 상위 32 비트의 base 를 0xffffffff 로. base 가 limit 보다 크므로 창이 닫힌 상태가 된다 */
			}	/* [한국어] 함수 루프 끝 */
		}
	}
}

/* [한국어]
 * vmd_attach_resources - 도메인의 자원 창을 VMD BAR 자원의 자식으로 등록한다
 *
 * @vmd: 이 VMD 인스턴스.  @return: 없음.
 *
 * 리눅스의 자원 트리는 부모-자식 관계로 겹침을 관리한다. VMD 의
 * MEMBAR1/MEMBAR2 는 VMD 엔드포인트가 차지한 물리 주소 영역이고, 그
 * 안을 쪼개 하위 장치에 나눠 줄 것이 vmd->resources[1]/[2] 다. 그러므로
 * 후자가 전자의 자식이어야 자원 코어가 "이 영역은 이미 임자가 있다" 를
 * 올바로 판단한다.
 *
 * 여기서는 부모 쪽의 child 포인터만 세운다. 자식 쪽의 parent 포인터는
 * vmd_enable_domain() 이 resources[1]/[2] 를 만들 때 이미 채워 두었다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vmd_enable_domain() → [이 함수]
 */
static void vmd_attach_resources(struct vmd_dev *vmd)
{
	vmd->dev->resource[VMD_MEMBAR1].child = &vmd->resources[1];	/* [한국어] MEMBAR1 BAR 자원의 자식으로 도메인의 첫 MMIO 창을 등록한다. 자식 쪽 parent 포인터는 vmd_enable_domain() 이 이미 채워 두었다 */
	vmd->dev->resource[VMD_MEMBAR2].child = &vmd->resources[2];	/* [한국어] MEMBAR2 도 마찬가지 */
}

/* [한국어]
 * vmd_detach_resources - 위 연결을 끊는다
 *
 * @vmd: 이 VMD 인스턴스.  @return: 없음.
 *
 * vmd_attach_resources() 의 짝. 드라이버가 내려갈 때 부모 BAR 자원이
 * 곧 사라질 자식을 계속 가리키지 않도록 끊어 준다.
 *
 * 순서가 중요하다. vmd_remove() 는 pci_remove_root_bus() 로 도메인의
 * 버스와 장치를 먼저 없애고 나서 이것을 부른다. 반대로 하면 아직 살아
 * 있는 하위 장치의 자원이 트리에서 떨어져 나간다.
 *
 * 실행 컨텍스트: remove 경로.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vmd_remove() → [이 함수]
 */
static void vmd_detach_resources(struct vmd_dev *vmd)
{
	vmd->dev->resource[VMD_MEMBAR1].child = NULL;	/* [한국어] MEMBAR1 BAR 자원의 자식 포인터를 끊는다 */
	vmd->dev->resource[VMD_MEMBAR2].child = NULL;	/* [한국어] MEMBAR2 도 마찬가지. 이 시점에는 하위 장치가 모두 사라진 뒤여야 한다 */
}

/* [한국어]
 * vmd_get_phys_offsets - 게스트가 보는 BAR 주소와 호스트 물리 주소의 차이를 구한다
 *
 * @vmd: 이 VMD 인스턴스.
 * @native_hint: true 면 MEMBAR2 안의 shadow 레지스터에서, false 면
 *   하이퍼바이저가 흉내 낸 벤더 고유 capability 에서 읽는다.
 * @offset1: MEMBAR1 의 보정값을 담을 곳.  @offset2: MEMBAR2 의 보정값.
 * @return: 0 성공(shadow 정보가 없어 보정이 필요 없는 경우도 0),
 *   -ENODEV(레지스터 읽기 실패), -ENOMEM(매핑 실패).
 *
 * 무엇을 위한 계산인가. 가상 머신 안에서 VMD 를 쓸 때, 게스트에게 보이는
 * VMD BAR 주소는 하이퍼바이저가 정한 값이고 호스트의 실제 물리 주소와
 * 다르다. 그런데 VMD 하드웨어가 하위 장치의 주소를 해석할 때 쓰는 것은
 * 호스트 물리 주소다. 그래서 게스트 커널이 하위 장치에 자원을 배정할 때
 * 그 차이만큼 보정해 주지 않으면 엉뚱한 곳을 가리키게 된다. 이 함수가
 * 그 차이를 구한다.
 *
 * 두 경로가 있다.
 *   (1) native_hint=true: VMLOCK 레지스터의 shadow 활성 비트를 확인하고,
 *       MEMBAR2 를 잠깐 매핑해 0x2000 지점에서 64 비트 값 두 개를 읽는다.
 *       읽고 나면 곧바로 매핑을 푼다.
 *   (2) native_hint=false: 벤더 고유 capability 를 찾아 그 안의 서명이
 *       "SHDW"(0x53484457, ASCII 'S','H','D','W' 를 리틀엔디언 u32 로
 *       읽은 값)인지 확인하고, 이어지는 네 개의 32 비트 레지스터를
 *       두 쌍으로 묶어 64 비트 물리 주소 두 개를 만든다.
 *
 * 어느 쪽이든 shadow 정보가 없으면 보정 없이 0 을 돌려준다. 호출자는
 * offset 이 0 이면 보정이 필요 없는 것으로 다룬다.
 *
 * 마지막 계산에서 PCI_BASE_ADDRESS_MEM_MASK 로 하위 비트를 지우는 이유는,
 * 읽어 온 값이 BAR 형식이라 하위 4 비트에 타입 정보(prefetchable, 64 비트
 * 여부 등)가 섞여 있기 때문이다. 주소만 뽑아야 한다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 위 반환값 설명대로. (2) 경로는 pci_read_config_dword 의
 * 실패를 확인하지 않고, pos 가 0 일 때도 먼저 읽어 본 뒤에 검사한다
 * (상류 코드 그대로 — pos+4 = 4 를 읽게 되지만 그 값은 곧 서명 검사에서
 * 걸러진다).
 *
 * 호출 체인:
 *   vmd_enable_domain() → [이 함수] → pci_iomap(), pci_find_capability()
 */
static int vmd_get_phys_offsets(struct vmd_dev *vmd, bool native_hint,
				resource_size_t *offset1,
				resource_size_t *offset2)
{
	struct pci_dev *dev = vmd->dev;	/* [한국어] VMD 엔드포인트 자신 */
	u64 phys1, phys2;	/* [한국어] 읽어 올 호스트 물리 주소 두 개 */

	if (native_hint) {	/* [한국어] 네이티브 경로 — shadow 레지스터가 MEMBAR2 안에 있는 하드웨어 */
		u32 vmlock;	/* [한국어] VMLOCK 레지스터 값 */
		int ret;	/* [한국어] config 읽기 결과 */

		ret = pci_read_config_dword(dev, PCI_REG_VMLOCK, &vmlock);	/* [한국어] VMLOCK(0x70)을 읽는다 */
		if (ret || PCI_POSSIBLE_ERROR(vmlock))	/* [한국어] 읽기 실패이거나 값이 전부 1(장치가 사라졌을 때 나오는 패턴)이면 */
			return -ENODEV;	/* [한국어] 장치가 없다 */

		if (MB2_SHADOW_EN(vmlock)) {	/* [한국어] bit 1 이 서 있어야 shadow 값이 유효하다 */
			void __iomem *membar2;	/* [한국어] 잠시 매핑할 MEMBAR2 */

			membar2 = pci_iomap(dev, VMD_MEMBAR2, 0);	/* [한국어] MEMBAR2 전체를 매핑한다. 세 번째 인자 0 은 "BAR 크기 전부" 라는 뜻이다 */
			if (!membar2)	/* [한국어] 매핑 실패 */
				return -ENOMEM;	/* [한국어] 메모리 부족 */
			phys1 = readq(membar2 + MB2_SHADOW_OFFSET);	/* [한국어] MEMBAR2 시작에서 0x2000 지점의 64 비트 값 = MEMBAR1 의 호스트 물리 주소 */
			phys2 = readq(membar2 + MB2_SHADOW_OFFSET + 8);	/* [한국어] 그 다음 8 바이트 = MEMBAR2 의 호스트 물리 주소 */
			pci_iounmap(dev, membar2);	/* [한국어] 읽었으니 곧바로 매핑을 푼다. 오래 잡고 있을 이유가 없다 */
		} else	/* [한국어] shadow 가 꺼져 있으면 */
			return 0;	/* [한국어] 보정 없이 성공으로 돌아간다. offset 은 호출자가 0 으로 초기화해 두었다 */
	} else {	/* [한국어] 가상화 경로 — shadow 가 벤더 고유 capability 안에 있는 경우 */
		/* Hypervisor-Emulated Vendor-Specific Capability */
		int pos = pci_find_capability(dev, PCI_CAP_ID_VNDR);	/* [한국어] 벤더 고유 capability(ID 0x09)를 찾는다. 없으면 0 */
		u32 reg, regu;	/* [한국어] 32 비트 두 조각을 담을 자리 */

		pci_read_config_dword(dev, pos + 4, &reg);	/* [한국어] capability 시작에서 4 바이트 뒤를 읽는다. pos 가 0 이어도 일단 읽는데(오프셋 4), 그 값은 아래 서명 검사에서 걸러진다(상류 코드 그대로) */

		/* "SHDW" */
		if (pos && reg == 0x53484457) {	/* [한국어] pos 가 유효하고 서명이 "SHDW" 여야 한다. 0x53484457 은 ASCII S,H,D,W 를 리틀엔디언 u32 로 읽은 값이다 */
			pci_read_config_dword(dev, pos + 8, &reg);	/* [한국어] MEMBAR1 물리 주소의 하위 32 비트 */
			pci_read_config_dword(dev, pos + 12, &regu);	/* [한국어] 상위 32 비트 */
			phys1 = (u64) regu << 32 | reg;	/* [한국어] 둘을 합쳐 64 비트 주소로 */

			pci_read_config_dword(dev, pos + 16, &reg);	/* [한국어] MEMBAR2 물리 주소의 하위 32 비트 */
			pci_read_config_dword(dev, pos + 20, &regu);	/* [한국어] 상위 32 비트 */
			phys2 = (u64) regu << 32 | reg;	/* [한국어] 합친다 */
		} else	/* [한국어] 서명이 없으면 */
			return 0;	/* [한국어] 보정 없이 성공 */
	}

	*offset1 = dev->resource[VMD_MEMBAR1].start -	/* [한국어] 보정값 = 지금 보이는 BAR 주소 - 호스트 물리 주소 */
			(phys1 & PCI_BASE_ADDRESS_MEM_MASK);	/* [한국어] 하위 4 비트에는 BAR 타입 정보가 섞여 있으므로 마스크로 지우고 주소만 쓴다 */
	*offset2 = dev->resource[VMD_MEMBAR2].start -	/* [한국어] MEMBAR2 도 같은 계산 */
			(phys2 & PCI_BASE_ADDRESS_MEM_MASK);	/* [한국어] 같은 마스크 적용 */

	return 0;	/* [한국어] 성공 */
}

/* [한국어]
 * vmd_get_bus_number_start - 이 VMD 가 쓸 버스 번호 시작점을 하드웨어에서 읽는다
 *
 * @vmd: 이 VMD 인스턴스.
 * @return: 0 성공, -ENODEV(알 수 없는 설정값).
 *
 * 한 시스템에 VMD 가 여럿 있을 수 있고, 그때 서로 버스 번호가 겹치면
 * 안 된다. 그래서 하드웨어가 각 VMD 에 담당 구간을 정해 주고, 그 설정을
 * VMCONFIG 레지스터에서 읽을 수 있게 해 둔다.
 *
 * 먼저 VMCAP 의 bit 0 을 보고 이 기능이 있는지 확인한다. 있으면 VMCONFIG
 * 의 bit 9:8 을 읽어 0 이면 버스 0 부터, 1 이면 128 부터, 2 이면 224 부터로
 * 정한다. 3 은 정의되지 않은 값이라 오류로 처리한다.
 *
 * 기능이 없으면 busn_start 를 건드리지 않으므로 devm_kzalloc 이 준 0 이
 * 그대로 남는다.
 *
 * 이 값이 이후에 두 군데서 쓰인다. resources[0](버스 번호 범위)의 시작이
 * 되고, vmd_cfg_addr() 이 실제 버스 번호에서 이 값을 빼 ECAM 안의 상대
 * 위치를 구한다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 알 수 없는 설정값이면 로그를 남기고 -ENODEV. 그러면 probe 가
 * 실패한다.
 *
 * 호출 체인:
 *   vmd_enable_domain() → [이 함수] → pci_read_config_ 계열
 */
static int vmd_get_bus_number_start(struct vmd_dev *vmd)
{
	struct pci_dev *dev = vmd->dev;	/* [한국어] VMD 엔드포인트 자신 */
	u16 reg;	/* [한국어] config 레지스터 값을 담을 자리 */

	pci_read_config_word(dev, PCI_REG_VMCAP, &reg);	/* [한국어] VMCAP(0x40)을 읽는다 */
	if (BUS_RESTRICT_CAP(reg)) {	/* [한국어] bit 0 — 버스 번호 제한 기능이 있는가 */
		pci_read_config_word(dev, PCI_REG_VMCONFIG, &reg);	/* [한국어] 있으면 VMCONFIG(0x44)에서 실제 설정을 읽는다 */

		switch (BUS_RESTRICT_CFG(reg)) {	/* [한국어] bit 9:8 을 꺼내 분기 */
		case 0:	/* [한국어] 설정 0 */
			vmd->busn_start = 0;	/* [한국어] 버스 0 부터 */
			break;
		case 1:	/* [한국어] 설정 1 */
			vmd->busn_start = 128;	/* [한국어] 버스 128 부터 */
			break;
		case 2:	/* [한국어] 설정 2 */
			vmd->busn_start = 224;	/* [한국어] 버스 224 부터 */
			break;
		default:	/* [한국어] 3 은 정의되지 않은 값이다 */
			pci_err(dev, "Unknown Bus Offset Setting (%d)\n",	/* [한국어] 무슨 값이었는지 로그로 남기고 */
				BUS_RESTRICT_CFG(reg));	/* [한국어] 그 값을 함께 찍는다 */
			return -ENODEV;	/* [한국어] probe 를 실패시킨다. 잘못된 버스 범위로 진행하면 다른 도메인과 충돌한다 */
		}
	}

	return 0;	/* [한국어] 기능이 없으면 busn_start 는 0 인 채로 성공 */
}

/* [한국어]
 * vmd_irq - VMD 벡터가 울렸을 때 하위 인터럽트들을 되뿌리는 핸들러
 *
 * @irq: 울린 VMD 벡터의 리눅스 IRQ 번호(쓰지 않는다).
 * @data: devm_request_irq 에 넘긴 vmd_irq_list. 어느 벡터인지 알려 준다.
 * @return: 항상 IRQ_HANDLED.
 *
 * 인터럽트 중계의 도착점이다. VMD 뒤의 어느 장치가 인터럽트를 내면
 * 그것이 VMD 의 MSI-X 벡터 하나로 잡히고, 커널은 그 벡터에 등록된 이
 * 핸들러를 부른다. 그런데 그 벡터를 여러 하위 장치가 나눠 쓰고 있으므로,
 * 누가 냈는지 알 수 없다. 그래서 목록에 달린 모두를 호출한다.
 * generic_handle_irq(vmdirq->virq) 한 줄이 그 되뿌리기다.
 *
 * "모두를 부른다" 는 것이 성능의 관건이다. 한 벡터를 나눠 쓰는 장치가
 * 많을수록 헛호출이 늘어난다. vmd_next_irq() 가 NVMe 를 우대해 벡터를
 * 나눠 주는 이유가 바로 이것이다.
 *
 * SRCU 읽기 구간으로 순회를 감싸는 이유는, 순회하는 동안 다른 CPU 가
 * vmd_msi_free() 로 목록의 항목을 해제할 수 있기 때문이다. SRCU 읽기
 * 구간이 열려 있으면 그쪽의 synchronize_srcu() 가 기다려 준다. 일반
 * RCU 가 아니라 SRCU 인 덕분에 벡터마다 독립된 유예 기간을 갖는다.
 *
 * 실행 컨텍스트: 하드 IRQ. IRQF_NO_THREAD 로 등록되어 있어 PREEMPT_RT
 * 에서도 스레드로 밀리지 않는다. 잠들 수 없다. 이것이 목록 보호에
 * raw_spinlock 과 SRCU 를 쓰는 이유다.
 *
 * 에러 경로: 없다. 목록이 비어 있어도 IRQ_HANDLED 를 돌려준다 — 공유
 * 벡터라 "내 것이 아니다" 를 판정할 방법이 없기 때문이다.
 *
 * 호출 체인:
 *   하드웨어 인터럽트 → 커널 irq 코어 → [이 함수] → generic_handle_irq()
 *   → 하위 장치 드라이버의 핸들러
 */
static irqreturn_t vmd_irq(int irq, void *data)
{
	struct vmd_irq_list *irqs = data;	/* [한국어] devm_request_irq 에 넘긴 값 = 울린 벡터의 중계 목록 */
	struct vmd_irq *vmdirq;	/* [한국어] 목록 순회 커서 */
	int idx;	/* [한국어] SRCU 읽기 구간의 토큰 */

	idx = srcu_read_lock(&irqs->srcu);	/* [한국어] 읽기 구간 진입. 이 동안에는 vmd_msi_free() 의 synchronize_srcu 가 기다려 준다 */
	list_for_each_entry_rcu(vmdirq, &irqs->irq_list, node)	/* [한국어] 이 벡터를 함께 쓰는 하위 인터럽트를 모두 훑는다. 누가 냈는지 알 수 없으므로 전부 부른다 */
		generic_handle_irq(vmdirq->virq);	/* [한국어] 중계의 실체 — 하위 장치의 가상 IRQ 를 소프트웨어로 호출한다 */
	srcu_read_unlock(&irqs->srcu, idx);	/* [한국어] 읽기 구간 종료 */

	return IRQ_HANDLED;	/* [한국어] 공유 벡터라 "내 것이 아니다" 를 판정할 수 없으므로 언제나 처리했다고 답한다 */
}

/* [한국어]
 * vmd_alloc_irqs - VMD 자신의 MSI-X 벡터를 확보하고 핸들러를 건다
 *
 * @vmd: 이 VMD 인스턴스.
 * @return: 0 성공, -ENODEV / -ENOMEM / request_irq 의 오류.
 *
 * 중계에 쓸 "진짜" 인터럽트를 마련하는 함수다. 하위 장치들의 인터럽트는
 * 전부 여기서 확보한 벡터 중 하나로 들어온다.
 *
 *   (1) pci_msix_vec_count() 로 이 하드웨어가 몇 개까지 지원하는지 묻는다.
 *   (2) pci_alloc_irq_vectors() 로 최소 first_vec+1 개, 최대 지원 개수만큼
 *       요청한다. 최소값에 first_vec 이 들어가는 이유는 0 번을 쓰지 못하는
 *       하드웨어라면 최소 두 개(0 번과 실제로 쓸 1 번)가 필요하기 때문이다.
 *   (3) 확보한 개수만큼 vmd_irq_list 배열을 잡는다. devm 이라 언바인딩 시
 *       자동 해제된다.
 *   (4) 각 원소마다 SRCU 를 초기화하고, 목록을 비우고, virq 를 얻고,
 *       vmd_irq() 를 핸들러로 건다.
 *
 * IRQF_NO_THREAD 를 주는 이유는 이 핸들러가 "진짜 일" 을 하지 않고
 * 곧바로 하위 핸들러로 넘기기만 하기 때문이다. 스레드화하면 중계 한
 * 단계마다 문맥 전환이 끼어 지연이 늘어난다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 중간에 실패하면 그대로 반환한다. 이미 초기화한 SRCU 나
 * 확보한 벡터를 되돌리지 않는데, devm 관리분은 probe 실패 시 코어가
 * 정리해 주고 SRCU 는 vmd_cleanup_srcu() 가 msix_count 만큼 도는 구조라
 * 부분 초기화 상태에서 문제가 될 소지가 있다(상류 코드 그대로 기록해 둔다).
 *
 * 호출 체인:
 *   vmd_enable_domain() → [이 함수] → pci_alloc_irq_vectors(),
 *   init_srcu_struct(), devm_request_irq()
 */
static int vmd_alloc_irqs(struct vmd_dev *vmd)
{
	struct pci_dev *dev = vmd->dev;	/* [한국어] VMD 엔드포인트 자신 */
	int i, err;	/* [한국어] i 는 순회 커서, err 는 오류 코드 */

	vmd->msix_count = pci_msix_vec_count(dev);	/* [한국어] 이 하드웨어가 지원하는 MSI-X 벡터 최대 개수를 묻는다 */
	if (vmd->msix_count < 0)	/* [한국어] 음수면 MSI-X capability 자체가 없다 */
		return -ENODEV;	/* [한국어] 중계를 할 수 없으니 장치가 없는 것으로 처리한다 */

	vmd->msix_count = pci_alloc_irq_vectors(dev, vmd->first_vec + 1,	/* [한국어] 최소 first_vec+1 개, 최대 지원 개수만큼 요청한다. 최소값에 first_vec 이 들어가는 이유는 0 번을 못 쓰는 하드웨어라면 최소 두 개가 필요하기 때문이다 */
						vmd->msix_count, PCI_IRQ_MSIX);	/* [한국어] MSI-X 만 받는다. 레거시 INTx 나 MSI 로는 중계를 구현할 수 없다 */
	if (vmd->msix_count < 0)	/* [한국어] 실패면 */
		return vmd->msix_count;	/* [한국어] 그 오류를 그대로 올려보낸다 */

	vmd->irqs = devm_kcalloc(&dev->dev, vmd->msix_count, sizeof(*vmd->irqs),	/* [한국어] 확보한 개수만큼 중계 목록 배열을 잡는다. devm 이라 언바인딩 시 자동 해제 */
				 GFP_KERNEL);	/* [한국어] 프로세스 컨텍스트이므로 GFP_KERNEL 로 잠들며 할당해도 된다 */
	if (!vmd->irqs)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 메모리 부족 */

	for (i = 0; i < vmd->msix_count; i++) {	/* [한국어] 각 벡터를 초기화 */
		err = init_srcu_struct(&vmd->irqs[i].srcu);	/* [한국어] 이 벡터 전용 SRCU 를 초기화한다 */
		if (err)	/* [한국어] 실패면 */
			return err;	/* [한국어] 그대로 반환. 이미 초기화한 앞의 것들은 되돌리지 않는다(상류 코드 그대로) */

		INIT_LIST_HEAD(&vmd->irqs[i].irq_list);	/* [한국어] 중계 목록을 빈 상태로 초기화 */
		vmd->irqs[i].virq = pci_irq_vector(dev, i);	/* [한국어] i 번째 MSI-X 벡터의 리눅스 가상 IRQ 번호를 얻는다 */
		err = devm_request_irq(&dev->dev, vmd->irqs[i].virq,	/* [한국어] 그 IRQ 에 핸들러를 건다. devm 이라 자동 해제된다 */
				       vmd_irq, IRQF_NO_THREAD,	/* [한국어] IRQF_NO_THREAD — 이 핸들러는 곧바로 하위 핸들러로 넘기기만 하므로 스레드화하면 지연만 늘어난다 */
				       vmd->name, &vmd->irqs[i]);	/* [한국어] 이름은 "vmd0" 등(/proc/interrupts 에 표시), 마지막 인자가 핸들러에 전달될 vmd_irq_list */
		if (err)	/* [한국어] 등록 실패면 */
			return err;	/* [한국어] 그대로 반환 */
	}

	return 0;	/* [한국어] 모두 성공 */
}

/*
 * Since VMD is an aperture to regular PCIe root ports, only allow it to
 * control features that the OS is allowed to control on the physical PCI bus.
 */
/* [한국어]
 * vmd_copy_host_bridge_flags - 진짜 호스트 브리지의 권한을 새 도메인에 그대로 물려준다
 *
 * @root_bridge: VMD 엔드포인트가 매달린 진짜 호스트 브리지.
 * @vmd_bridge: 이 드라이버가 만든 가짜 호스트 브리지.
 * @return: 없음.
 *
 * native_* 플래그들은 "이 기능을 운영체제가 직접 다루어도 되는가, 아니면
 * 펌웨어에 맡겨야 하는가" 를 나타낸다. ACPI 시스템에서는 _OSC 협상으로
 * 정해지며, 펌웨어가 내주지 않은 기능을 커널이 건드리면 충돌이 난다.
 *
 * VMD 가 만든 도메인은 펌웨어가 모르는 것이라 _OSC 협상 대상이 아니다.
 * 그래서 원본 주석대로 "VMD 는 결국 보통의 PCIe 루트 포트로 가는 창일
 * 뿐이므로, 물리 PCI 버스에서 허용된 것만 허용한다" 는 원칙을 따라 진짜
 * 호스트 브리지의 판정을 그대로 복사한다.
 *
 * 여섯 가지다 — PCIe 핫플러그, SHPC 핫플러그, AER(오류 보고), PME(웨이크업),
 * LTR(지연 허용 보고), DPC(다운스트림 포트 격리).
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vmd_enable_domain() → [이 함수]
 */
static void vmd_copy_host_bridge_flags(struct pci_host_bridge *root_bridge,
				       struct pci_host_bridge *vmd_bridge)
{
	vmd_bridge->native_pcie_hotplug = root_bridge->native_pcie_hotplug;	/* [한국어] PCIe 네이티브 핫플러그를 커널이 직접 다뤄도 되는가 */
	vmd_bridge->native_shpc_hotplug = root_bridge->native_shpc_hotplug;	/* [한국어] SHPC(표준 핫플러그 컨트롤러)도 마찬가지 */
	vmd_bridge->native_aer = root_bridge->native_aer;	/* [한국어] AER(Advanced Error Reporting) 권한 */
	vmd_bridge->native_pme = root_bridge->native_pme;	/* [한국어] PME(웨이크업 신호) 권한 */
	vmd_bridge->native_ltr = root_bridge->native_ltr;	/* [한국어] LTR(Latency Tolerance Reporting) 권한 */
	vmd_bridge->native_dpc = root_bridge->native_dpc;	/* [한국어] DPC(Downstream Port Containment) 권한 */
}

/*
 * Enable ASPM and LTR settings on devices that aren't configured by BIOS.
 */
/* [한국어]
 * vmd_pm_enable_quirk - BIOS 가 빠뜨린 절전 설정을 장치마다 채워 넣는다
 *
 * @pdev: 도메인 안의 장치 하나(pci_walk_bus 가 하나씩 넘겨 준다).
 * @userdata: vmd_enable_domain() 이 넘긴 features 비트의 주소.
 * @return: 항상 0. pci_walk_bus 는 0 이 아닌 값을 받으면 순회를 멈추므로,
 *   0 을 돌려준다는 것은 "끝까지 다 돌아라" 는 뜻이다.
 *
 * VMD_FEAT_BIOS_PM_QUIRK 가 걸린 플랫폼에서만 실제 일을 한다. 그렇지
 * 않으면 첫 줄에서 0 을 돌려주고 끝난다.
 *
 * LTR 부분. LTR(Latency Tolerance Reporting)은 장치가 "나는 이만큼의
 * 지연은 견딜 수 있다" 를 플랫폼에 알리는 PCIe 확장 기능이고, 그 값이
 * 있어야 SoC 가 안심하고 깊은 절전 상태로 내려간다. BIOS 가 이미 값을
 * 넣었으면(max snoop latency 가 0 이 아니면) 건드리지 않고, 비어 있으면
 * VMD_BIOS_PM_QUIRK_LTR 을 써 넣는다. 32 비트 쓰기 한 번으로 하위 워드에
 * max snoop, 상위 워드에 max non-snoop 을 함께 넣는다.
 *
 * out_state_change 부분은 LTR capability 유무와 무관하게 언제나 실행된다.
 * 장치를 D0 로 올린 뒤 ASPM 링크 상태를 전부 켠다. 순서에 규격상의 근거가
 * 있는데, 원본 주석이 PCIe r6.0 5.5.4 절을 들어 "PCI-PM L1 PM Substates 를
 * 켜기 전에 장치가 D0 여야 한다" 고 밝힌다.
 *
 * _locked 접미사가 붙은 API 를 쓰는 이유는 pci_walk_bus 가 이미 버스 락을
 * 잡은 상태로 부르기 때문이다. 락을 다시 잡는 보통 버전을 쓰면 교착한다.
 *
 * 실행 컨텍스트: probe 경로, pci_walk_bus 안. 프로세스 컨텍스트이며
 * 버스 락이 잡혀 있다.
 *
 * 에러 경로: 없다. 실패해도 0 을 돌려주어 순회를 계속한다.
 *
 * 호출 체인:
 *   vmd_enable_domain() → pci_walk_bus() → [이 함수]
 *   → pci_set_power_state_locked(), pci_enable_link_state_locked()
 */
static int vmd_pm_enable_quirk(struct pci_dev *pdev, void *userdata)
{
	unsigned long features = *(unsigned long *)userdata;	/* [한국어] pci_walk_bus 는 void* 하나만 넘길 수 있어, 호출자가 features 의 주소를 넘겨 준 것을 여기서 되꺼낸다 */
	u16 ltr = VMD_BIOS_PM_QUIRK_LTR;	/* [한국어] 써 넣을 기본 LTR 값 */
	u32 ltr_reg;	/* [한국어] 읽어 온 LTR 레지스터 값 */
	int pos;	/* [한국어] LTR capability 의 위치 */

	if (!(features & VMD_FEAT_BIOS_PM_QUIRK))	/* [한국어] 이 플랫폼에 quirk 가 필요하지 않으면 */
		return 0;	/* [한국어] 아무것도 하지 않는다. 0 을 돌려주어 pci_walk_bus 가 순회를 계속하게 한다 */

	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_LTR);	/* [한국어] 확장 capability 목록에서 LTR 을 찾는다(PCIe 확장 capability 는 config 0x100 이후에 있다) */
	if (!pos)	/* [한국어] LTR capability 가 없으면 */
		goto out_state_change;	/* [한국어] LTR 설정은 건너뛰고 ASPM 처리만 한다 */

	/*
	 * Skip if the max snoop LTR is non-zero, indicating BIOS has set it
	 * so the LTR quirk is not needed.
	 */
	pci_read_config_dword(pdev, pos + PCI_LTR_MAX_SNOOP_LAT, &ltr_reg);	/* [한국어] max snoop latency 레지스터를 읽는다 */
	if (!!(ltr_reg & (PCI_LTR_VALUE_MASK | PCI_LTR_SCALE_MASK)))	/* [한국어] 값이나 스케일 비트 중 하나라도 서 있으면 BIOS 가 이미 설정했다는 뜻이다 */
		goto out_state_change;	/* [한국어] 그러면 건드리지 않고 ASPM 처리로 넘어간다 */

	/*
	 * Set the default values to the maximum required by the platform to
	 * allow the deepest power management savings. Write as a DWORD where
	 * the lower word is the max snoop latency and the upper word is the
	 * max non-snoop latency.
	 */
	ltr_reg = (ltr << 16) | ltr;	/* [한국어] 하위 워드에 max snoop, 상위 워드에 max non-snoop. 같은 값을 두 번 넣는다 */
	pci_write_config_dword(pdev, pos + PCI_LTR_MAX_SNOOP_LAT, ltr_reg);	/* [한국어] 32 비트 한 번에 두 값을 함께 쓴다 */
	pci_info(pdev, "VMD: Default LTR value set by driver\n");	/* [한국어] 드라이버가 기본값을 넣었음을 로그로 남긴다 */

out_state_change:	/* [한국어] LTR 처리 여부와 무관하게 여기로 모인다 */
	/*
	 * Ensure devices are in D0 before enabling PCI-PM L1 PM Substates, per
	 * PCIe r6.0, sec 5.5.4.
	 */
	pci_set_power_state_locked(pdev, PCI_D0);	/* [한국어] ASPM L1 PM Substates 를 켜기 전에 D0 여야 한다(원본 주석이 PCIe r6.0 5.5.4 절을 든다). _locked 접미사는 pci_walk_bus 가 이미 버스 락을 잡고 있어서다 */
	pci_enable_link_state_locked(pdev, PCIE_LINK_STATE_ALL);	/* [한국어] 모든 ASPM 링크 상태를 허용한다. 이것이 있어야 SoC 가 깊은 절전으로 내려간다 */
	return 0;	/* [한국어] 0 을 돌려주어 순회를 계속 */
}

/* [한국어]
 * vmd_enable_domain - 숨겨진 도메인을 실제로 만들어 내는 본체
 *
 * @vmd: 이 VMD 인스턴스(BAR 매핑까지는 vmd_probe 가 끝내 두었다).
 * @features: 이 하드웨어의 기능 비트(vmd_ids 의 driver_data).
 * @return: 0 성공, 음수 errno 실패.
 *
 * 이 파일에서 가장 긴 함수이고, 드라이버가 하는 일의 전부가 여기 있다.
 * 순서대로 보면 이렇다.
 *
 *   (1) 주소 보정. shadow 레지스터가 있는 하드웨어라면 게스트 주소와
 *       호스트 물리 주소의 차이를 구해 offset[] 에 담는다. 그 위치에
 *       따라 membar2_offset(MEMBAR2 앞머리 중 하위 장치에 주지 않고
 *       남겨 둘 크기)도 달라진다.
 *   (2) 버스 번호 시작점. 제한 기능이 있으면 하드웨어 설정대로 정한다.
 *   (3) 자원 창 세 개 계산. [0]은 버스 번호 범위인데, CFGBAR 크기를
 *       20 비트 우시프트(=1MB 로 나누기)한 것이 버스 개수다. ECAM 에서
 *       버스 하나가 1MB 를 차지하기 때문이다. [1]과 [2]는 MEMBAR1/2 를
 *       그대로 물려받되, 창이 4GB 아래에 있으면 IORESOURCE_MEM_64 를
 *       지운다. 원본의 긴 주석이 그 이유를 설명한다 — 64 비트 창에는
 *       32 비트 자원을 넣지 않는다는 pbus_size_mem() 의 가정 때문에,
 *       32 비트 자원을 담으려면 32 비트 창으로 보이게 해야 한다.
 *   (4) 인터럽트. 중계 우회가 불가능하거나 주소 보정이 필요하면
 *       벡터를 확보하고 재매핑을 켜고 MSI 도메인을 만든다. 아니면
 *       재매핑을 꺼서 하위 장치가 상위 도메인을 직접 쓰게 한다.
 *   (5) 도메인 번호를 0x10000 이상에서 얻는다. 원본 주석대로 ACPI _SEG
 *       가 쓰는 하위 16 비트와 겹치지 않게 하기 위해서다.
 *   (6) pci_create_root_bus() 로 루트 버스를 만든다. 여기에 vmd_ops 를
 *       넘기는 것이 "이 도메인의 config 접근은 내가 처리한다" 는 선언이다.
 *   (7) 호스트 브리지 권한 복사, 자원 연결, MSI 도메인 지정,
 *       sysfs 심볼릭 링크 생성.
 *   (8) ACPI 훅을 걸고 pci_scan_child_bus() 로 열거한 뒤 훅을 뗀다.
 *       열거 도중에만 훅이 필요하기 때문이다.
 *   (9) vmd_domain_reset() 으로 브리지 창을 정리하고, 하위 장치 하나를
 *       골라 pci_reset_bus() 로 계층 전체를 리셋한다. 원본 주석이 자식
 *       장치를 넘기는 이유를 설명한다 — pci_reset_bus 는 인자의 부모에
 *       리셋을 걸므로, 브리지 수준에서 걸리게 하려면 그 아래 장치를
 *       넘겨야 한다.
 *  (10) 자원 배정, 절전 quirk 적용, PCIe 버스 설정. 마지막 것은 VMD 의
 *       가상 루트 버스가 pci_is_pcie() 를 통과하지 못하므로 진짜 루트
 *       포트인 자식 버스들에 대해 따로 돌린다(원본 주석의 설명).
 *  (11) pci_bus_add_devices() 로 발견한 장치들에 드라이버를 붙인다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트. 열거와 리셋이 있어
 * 상당한 시간이 걸린다.
 *
 * 에러 경로: (1)(2)(4)(5)의 실패는 그대로 반환한다. (6)의 실패만 되돌리기가
 * 있다 — 도메인 번호를 반납하고 자원 목록을 해제하고 IRQ 도메인을 없앤다.
 * 그 앞 단계의 되돌리기가 없는 것은 devm 관리분이기 때문이다.
 *
 * 호출 체인:
 *   vmd_probe() → [이 함수] → pci_create_root_bus(), pci_scan_child_bus(),
 *   pci_assign_unassigned_bus_resources(), pci_bus_add_devices()
 */
static int vmd_enable_domain(struct vmd_dev *vmd, unsigned long features)
{
	struct pci_sysdata *sd = &vmd->sysdata;	/* [한국어] 새 도메인의 sysdata. vmd_dev 안에 박혀 있어 vmd_from_bus() 가 되찾을 수 있다 */
	struct resource *res;	/* [한국어] BAR 자원을 가리킬 임시 포인터 */
	u32 upper_bits;	/* [한국어] BAR 끝 주소의 상위 32 비트. 4GB 아래인지 판정하는 데 쓴다 */
	unsigned long flags;	/* [한국어] 자원 플래그 계산용 */
	LIST_HEAD(resources);	/* [한국어] 루트 버스에 넘길 자원 목록. 스택에 리스트 머리를 만든다 */
	resource_size_t offset[2] = {0};	/* [한국어] 게스트 주소와 호스트 물리 주소의 차이. 기본은 보정 없음(0) */
	resource_size_t membar2_offset = 0x2000;	/* [한국어] MEMBAR2 앞머리 중 하위 장치에 주지 않고 남길 크기. 기본 8KB 는 VMD 자신의 MSI-X 테이블 자리다 */
	struct pci_bus *child;	/* [한국어] 자식 버스 순회 커서 */
	struct pci_dev *dev;	/* [한국어] 리셋 대상으로 고를 장치 */
	int ret;	/* [한국어] 각 단계의 반환값 */

	/*
	 * Shadow registers may exist in certain VMD device ids which allow
	 * guests to correctly assign host physical addresses to the root ports
	 * and child devices. These registers will either return the host value
	 * or 0, depending on an enable bit in the VMD device.
	 */
	if (features & VMD_FEAT_HAS_MEMBAR_SHADOW) {	/* [한국어] shadow 레지스터가 MEMBAR2 안에 있는 하드웨어라면 */
		membar2_offset = MB2_SHADOW_OFFSET + MB2_SHADOW_SIZE;	/* [한국어] shadow 영역까지 하위 장치에 주지 않도록 남길 크기를 늘린다 */
		ret = vmd_get_phys_offsets(vmd, true, &offset[0], &offset[1]);	/* [한국어] 네이티브 경로로 보정값을 구한다 */
		if (ret)	/* [한국어] 실패면 */
			return ret;	/* [한국어] probe 를 중단한다 */
	} else if (features & VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP) {	/* [한국어] shadow 가 벤더 고유 capability 에 있는 하드웨어라면 */
		ret = vmd_get_phys_offsets(vmd, false, &offset[0], &offset[1]);	/* [한국어] 가상화 경로로 보정값을 구한다 */
		if (ret)	/* [한국어] 실패면 */
			return ret;	/* [한국어] 중단 */
	}

	/*
	 * Certain VMD devices may have a root port configuration option which
	 * limits the bus range to between 0-127, 128-255, or 224-255
	 */
	if (features & VMD_FEAT_HAS_BUS_RESTRICTIONS) {	/* [한국어] 버스 번호 제한 기능이 있으면 */
		ret = vmd_get_bus_number_start(vmd);	/* [한국어] 하드웨어 설정대로 시작 버스 번호를 정한다 */
		if (ret)	/* [한국어] 알 수 없는 설정이면 */
			return ret;	/* [한국어] 중단 */
	}

	res = &vmd->dev->resource[VMD_CFGBAR];	/* [한국어] CFGBAR 의 크기가 곧 담당할 버스 개수를 말해 준다 */
	vmd->resources[0] = (struct resource) {	/* [한국어] 자원 [0] — 버스 번호 범위. 구조체 통째 대입으로 한 번에 채운다 */
		.name  = "VMD CFGBAR",	/* [한국어] /proc/iomem 등에 표시될 이름 */
		.start = vmd->busn_start,	/* [한국어] 시작 버스 번호 */
		.end   = vmd->busn_start + (resource_size(res) >> 20) - 1,	/* [한국어] CFGBAR 크기를 1MB 로 나눈 것이 버스 개수다(ECAM 에서 버스 하나가 1MB). -1 은 끝이 포함 범위이기 때문 */
		.flags = IORESOURCE_BUS | IORESOURCE_PCI_FIXED,	/* [한국어] 버스 번호 자원이며 재배치 불가(BUS + PCI_FIXED) */
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
	res = &vmd->dev->resource[VMD_MEMBAR1];	/* [한국어] MEMBAR1 BAR 자원 */
	upper_bits = upper_32_bits(res->end);	/* [한국어] 끝 주소의 상위 32 비트를 본다 */
	flags = res->flags & ~IORESOURCE_SIZEALIGN;	/* [한국어] 크기 정렬 요구는 지운다. 이미 BIOS 가 배치를 끝낸 창이므로 다시 정렬할 필요가 없다 */
	if (!upper_bits)	/* [한국어] 상위 비트가 0 이면 창 전체가 4GB 아래에 있다 */
		flags &= ~IORESOURCE_MEM_64;	/* [한국어] 그러면 64 비트 표시를 지운다. 원본 주석대로 pbus_size_mem() 이 64 비트 창에는 32 비트 자원을 넣지 않는다고 가정하기 때문이다 */
	vmd->resources[1] = (struct resource) {	/* [한국어] 자원 [1] — 도메인의 첫 MMIO 창 */
		.name  = "VMD MEMBAR1",	/* [한국어] 표시 이름 */
		.start = res->start,	/* [한국어] BAR 시작 주소 그대로 */
		.end   = res->end,	/* [한국어] BAR 끝 주소 그대로 */
		.flags = flags,	/* [한국어] 위에서 계산한 플래그 */
		.parent = res,	/* [한국어] 부모는 원래 BAR 자원. 자식 등록은 vmd_attach_resources() 가 한다 */
	};

	res = &vmd->dev->resource[VMD_MEMBAR2];	/* [한국어] MEMBAR2 BAR 자원 */
	upper_bits = upper_32_bits(res->end);	/* [한국어] 같은 판정 */
	flags = res->flags & ~IORESOURCE_SIZEALIGN;	/* [한국어] 같은 플래그 처리 */
	if (!upper_bits)	/* [한국어] 4GB 아래면 */
		flags &= ~IORESOURCE_MEM_64;	/* [한국어] 64 비트 표시 제거 */
	vmd->resources[2] = (struct resource) {	/* [한국어] 자원 [2] — 도메인의 두 번째 MMIO 창 */
		.name  = "VMD MEMBAR2",	/* [한국어] 표시 이름 */
		.start = res->start + membar2_offset,	/* [한국어] 앞머리를 membar2_offset 만큼 건너뛴다. 그 앞은 VMD 자신의 레지스터가 쓴다 */
		.end   = res->end,	/* [한국어] 끝은 BAR 끝 그대로 */
		.flags = flags,	/* [한국어] 계산한 플래그 */
		.parent = res,	/* [한국어] 부모는 원래 BAR 자원 */
	};

	/*
	 * Currently MSI remapping must be enabled in guest passthrough mode
	 * due to some missing interrupt remapping plumbing. This is probably
	 * acceptable because the guest is usually CPU-limited and MSI
	 * remapping doesn't become a performance bottleneck.
	 */
	if (!(features & VMD_FEAT_CAN_BYPASS_MSI_REMAP) ||	/* [한국어] 하드웨어가 중계 우회를 지원하지 않거나 */
	    offset[0] || offset[1]) {	/* [한국어] 주소 보정이 필요한 상황(게스트 패스스루)이면 중계를 써야 한다. 원본 주석대로 게스트에서는 인터럽트 재매핑 배관이 아직 없어 중계가 강제된다 */
		ret = vmd_alloc_irqs(vmd);	/* [한국어] VMD 자신의 MSI-X 벡터를 확보하고 핸들러를 건다 */
		if (ret)	/* [한국어] 실패면 */
			return ret;	/* [한국어] 중단 */

		vmd_set_msi_remapping(vmd, true);	/* [한국어] 하드웨어의 재매핑을 켠다. 하위 장치의 MSI 가 VMD 테이블로 잡히게 된다 */

		ret = vmd_create_irq_domain(vmd);	/* [한국어] 하위 장치의 MSI 요청을 받을 도메인을 만든다 */
		if (ret)	/* [한국어] 실패면 */
			return ret;	/* [한국어] 중단 */
	} else {	/* [한국어] 우회 가능한 경우 */
		vmd_set_msi_remapping(vmd, false);	/* [한국어] 재매핑을 꺼서 하위 장치의 MSI 가 그대로 통과하게 한다. 이 경우 msix_count 는 0 으로 남는다 */
	}

	pci_add_resource(&resources, &vmd->resources[0]);	/* [한국어] 버스 번호 자원을 목록에 넣는다 */
	pci_add_resource_offset(&resources, &vmd->resources[1], offset[0]);	/* [한국어] 첫 MMIO 창. offset 을 함께 주면 CPU 주소와 버스 주소의 차이로 등록된다 */
	pci_add_resource_offset(&resources, &vmd->resources[2], offset[1]);	/* [한국어] 두 번째 MMIO 창도 마찬가지 */

	sd->vmd_dev = vmd->dev;	/* [한국어] 이 도메인의 "진짜 DMA 주체" 를 기록한다. VMD 뒤 장치의 DMA 는 VMD 의 requester ID 로 나가므로 IOMMU 가 그것을 알아야 한다. 이 값을 읽어 pci_real_dma_dev() 를 재정의하는 코드는 아키텍처 쪽에 있으며 이 트리에는 없다 */

	/*
	 * Emulated domains start at 0x10000 to not clash with ACPI _SEG
	 * domains.  Per ACPI r6.0, sec 6.5.6, _SEG returns an integer, of
	 * which the lower 16 bits are the PCI Segment Group (domain) number.
	 * Other bits are currently reserved.
	 */
	sd->domain = pci_bus_find_emul_domain_nr(0, 0x10000, INT_MAX);	/* [한국어] 0x10000 이상에서 빈 도메인 번호를 얻는다. 원본 주석대로 ACPI _SEG 가 쓰는 하위 16 비트와 겹치지 않게 하기 위해서다 */
	if (sd->domain < 0)	/* [한국어] 번호를 못 얻었으면 */
		return sd->domain;	/* [한국어] 그 오류를 그대로 반환 */

	sd->node = pcibus_to_node(vmd->dev->bus);	/* [한국어] 이 도메인의 NUMA 노드를 VMD 엔드포인트가 붙은 노드로 정한다. 하위 장치의 메모리 할당이 그 노드에서 이뤄지게 된다 */

	vmd->bus = pci_create_root_bus(&vmd->dev->dev, vmd->busn_start,	/* [한국어] 새 도메인의 루트 버스를 만든다. 첫 인자가 부모 device(VMD 엔드포인트), 둘째가 시작 버스 번호 */
				       &vmd_ops, sd, &resources);	/* [한국어] vmd_ops 를 주는 것이 "이 도메인의 config 접근은 내가 처리한다" 는 선언이고, sd 가 sysdata, resources 가 나눠 줄 창 목록이다 */
	if (!vmd->bus) {	/* [한국어] 생성 실패면 */
		pci_bus_release_emul_domain_nr(sd->domain);	/* [한국어] 받아 둔 도메인 번호를 반납하고 */
		pci_free_resource_list(&resources);	/* [한국어] 자원 목록을 해제하고 */
		vmd_remove_irq_domain(vmd);	/* [한국어] 만들어 둔 IRQ 도메인도 없앤다 */
		return -ENODEV;	/* [한국어] 실패 반환 */
	}

	vmd_copy_host_bridge_flags(pci_find_host_bridge(vmd->dev->bus),	/* [한국어] 진짜 호스트 브리지의 native_* 권한을 */
				   to_pci_host_bridge(vmd->bus->bridge));	/* [한국어] 이 드라이버가 만든 가짜 호스트 브리지에 그대로 복사한다 */

	vmd_attach_resources(vmd);	/* [한국어] 자원 트리에서 부모-자식 관계를 잇는다 */
	if (vmd->irq_domain)	/* [한국어] 중계 모드라면 */
		dev_set_msi_domain(&vmd->bus->dev, vmd->irq_domain);	/* [한국어] 새 버스의 MSI 도메인을 이 드라이버가 만든 것으로 지정한다 */
	else	/* [한국어] 우회 모드라면 */
		dev_set_msi_domain(&vmd->bus->dev,	/* [한국어] VMD 엔드포인트 자신의 MSI 도메인을 */
				   dev_get_msi_domain(&vmd->dev->dev));	/* [한국어] 그대로 물려준다. 하위 장치의 MSI 가 중계 없이 상위로 간다 */

	WARN(sysfs_create_link(&vmd->dev->dev.kobj, &vmd->bus->dev.kobj,	/* [한국어] /sys/.../<VMD 주소>/domain 심볼릭 링크를 만들어 사용자가 이 VMD 가 만든 버스를 찾을 수 있게 한다 */
			       "domain"), "Can't create symlink to domain\n");	/* [한국어] 실패해도 치명적이지 않아 경고만 남기고 진행한다 */

	vmd_acpi_begin();	/* [한국어] 열거 동안만 ACPI companion 탐색 훅을 건다 */

	pci_scan_child_bus(vmd->bus);	/* [한국어] 숨겨진 도메인을 실제로 훑어 장치들을 발견한다. 여기서 vmd_pci_read() 가 수없이 불린다 */
	vmd_domain_reset(vmd);	/* [한국어] 열거 직후 브리지들의 자원 창 레지스터를 정리한다 */

	/* When Intel VMD is enabled, the OS does not discover the Root Ports
	 * owned by Intel VMD within the MMCFG space. pci_reset_bus() applies
	 * a reset to the parent of the PCI device supplied as argument. This
	 * is why we pass a child device, so the reset can be triggered at
	 * the Intel bridge level and propagated to all the children in the
	 * hierarchy.
	 */
	list_for_each_entry(child, &vmd->bus->children, node) {	/* [한국어] 자식 버스(= 진짜 루트 포트 아래 버스)들을 훑는다 */
		if (!list_empty(&child->devices)) {	/* [한국어] 장치가 하나라도 있는 버스를 찾으면 */
			dev = list_first_entry(&child->devices,	/* [한국어] 그 첫 장치를 고른다 */
					       struct pci_dev, bus_list);	/* [한국어] bus_list 링크로 매달린 pci_dev 를 꺼낸다 */
			ret = pci_reset_bus(dev);	/* [한국어] 그 장치의 부모(브리지) 수준에서 리셋을 건다. 원본 주석대로 자식을 넘기는 이유가 이것이다 */
			if (ret)	/* [한국어] 실패해도 */
				pci_warn(dev, "can't reset device: %d\n", ret);	/* [한국어] 경고만 남기고 진행한다 */

			break;	/* [한국어] 한 번만 하면 계층 전체에 전파되므로 더 볼 필요가 없다 */
		}
	}

	pci_assign_unassigned_bus_resources(vmd->bus);	/* [한국어] 열거로 발견한 장치들에 위에서 만든 자원 창을 나눠 준다. BAR 주소가 여기서 정해진다 */

	pci_walk_bus(vmd->bus, vmd_pm_enable_quirk, &features);	/* [한국어] 도메인 안 모든 장치를 훑으며 절전 quirk 를 적용한다. features 의 주소를 넘기는 것에 유의 */

	/*
	 * VMD root buses are virtual and don't return true on pci_is_pcie()
	 * and will fail pcie_bus_configure_settings() early. It can instead be
	 * run on each of the real root ports.
	 */
	list_for_each_entry(child, &vmd->bus->children, node)	/* [한국어] VMD 의 루트 버스는 가상이라 pci_is_pcie() 를 통과하지 못하므로 */
		pcie_bus_configure_settings(child);	/* [한국어] 진짜 루트 포트인 자식 버스들에 대해 따로 PCIe 설정(MPS/MRRS 등)을 맞춘다 */

	/* [한국어] 발견해 둔 장치들에 드라이버를 붙인다. 열거(pci_scan_child_bus)와
	 * 바인딩이 나뉘어 있는 이유는 그 사이에 자원 배정이 끼어야 하기 때문이다.
	 * 이 호출이 끝나야 VMD 도메인 안의 NVMe 들이 실제로 동작하기 시작한다 —
	 * 여기서 drivers/nvme/host/pci.c 의 nvme_probe() 가 불린다. */
	pci_bus_add_devices(vmd->bus);

	vmd_acpi_end();	/* [한국어] 훅을 뗀다. 다른 VMD 인스턴스가 자기 열거 때 쓸 수 있게 하기 위해 곧바로 놓는다 */
	return 0;	/* [한국어] 성공 */
}

/* [한국어]
 * vmd_probe - VMD 엔드포인트에 바인딩되어 인스턴스를 세운다
 *
 * @dev: 바인딩된 VMD PCI 장치.
 * @id: 대조에 성공한 vmd_ids 항목. driver_data 에 기능 비트가 실려 있다.
 * @return: 0 성공, 음수 errno 실패.
 *
 * struct pci_driver vmd_drv 의 .probe 슬롯. pci-driver.c 의
 * local_pci_probe() 가 부른다. 즉 이 드라이버는 다른 PCI 드라이버와
 * 똑같은 방식으로 바인딩된다.
 *
 * 하는 일은 vmd_enable_domain() 을 부르기 위한 준비다.
 *
 *   (1) Xen 게스트 예외. 원본 주석이 길게 설명한다 — Xen 은 VMD 뒤
 *       장치들의 config space 를 모르므로 그것들을 발견하거나 설정할 수
 *       없다. MSI 재매핑 우회 모드는 리눅스가 MSI 항목을 직접 쓰는 것을
 *       전제하는데, Xen 에서는 인터럽트 컨트롤러를 Xen 이 관리하므로
 *       그 쓰기가 동작하지 않는다. 반면 VMD 의 인터럽트 다중화는 Xen
 *       아래서도 동작하므로, 우회 비트를 강제로 지워 중계 모드를 쓰게 한다.
 *   (2) CFGBAR 크기 검사. 1MB 미만이면 버스 하나도 담지 못한다.
 *   (3) vmd_dev 할당(devm), 인스턴스 번호 발급, 이름 생성.
 *   (4) pcim_enable_device() 로 장치를 켜고 pcim_iomap() 으로 CFGBAR 를
 *       매핑한다. pcim_ 접두사는 devm 관리 버전이라 실패 경로에서
 *       일일이 되돌릴 필요가 없다.
 *   (5) pci_set_master() 로 Bus Master 를 켠다. 하위 장치들의 DMA 가
 *       VMD 를 통해 나가므로 VMD 자신이 버스 마스터여야 한다.
 *   (6) DMA 마스크를 64 비트로, 실패하면 32 비트로 설정한다.
 *   (7) 벡터 0 을 쓰지 못하는 하드웨어면 first_vec 을 1 로.
 *   (8) config 락 초기화, drvdata 등록, vmd_enable_domain() 호출.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. pci-driver.c 의 pci_call_probe() 가
 * 장치가 붙은 NUMA 노드의 CPU 로 옮겨 실행할 수 있다.
 *
 * 에러 경로: 대부분 out_release_instance 로 모여 인스턴스 번호를 반납하고
 * 끝난다. 나머지 자원은 전부 devm 관리라 드라이버 코어가 정리한다.
 * 다만 (3)의 ida_alloc 실패는 아직 번호를 받지 못한 상태라 그 자리에서
 * 바로 반환한다.
 *
 * 호출 체인:
 *   pci-driver.c 의 local_pci_probe() → [이 함수] → vmd_enable_domain()
 */
static int vmd_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	unsigned long features = (unsigned long) id->driver_data;	/* [한국어] vmd_ids 항목이 실어 보낸 기능 비트. driver_data 는 unsigned long 로 저장되어 있다 */
	struct vmd_dev *vmd;	/* [한국어] 이 인스턴스의 상태 구조체 */
	int err;	/* [한국어] 오류 코드 */

	if (xen_domain()) {	/* [한국어] Xen 게스트에서 도는가 */
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
		features &= ~VMD_FEAT_CAN_BYPASS_MSI_REMAP;	/* [한국어] 중계 우회 비트를 강제로 지운다. 원본 주석대로 Xen 은 인터럽트 컨트롤러를 자기가 관리하므로 리눅스의 직접 MSI 쓰기가 동작하지 않는다. 반면 다중화(중계)는 Xen 아래서도 동작한다 */
	}

	if (resource_size(&dev->resource[VMD_CFGBAR]) < (1 << 20))	/* [한국어] CFGBAR 가 1MB 미만이면 버스 하나도 담지 못한다(ECAM 에서 버스 하나가 1MB) */
		return -ENOMEM;	/* [한국어] 쓸 수 없는 하드웨어 */

	vmd = devm_kzalloc(&dev->dev, sizeof(*vmd), GFP_KERNEL);	/* [한국어] 인스턴스 상태를 devm 으로 잡는다. 실패 경로에서 일일이 free 하지 않아도 된다 */
	if (!vmd)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 메모리 부족 */

	vmd->dev = dev;	/* [한국어] 자기 자신을 기억해 둔다 */
	vmd->sysdata.domain = PCI_DOMAIN_NR_NOT_SET;	/* [한국어] 아직 도메인 번호를 받지 않았음을 표시. vmd_enable_domain() 이 채운다 */
	vmd->instance = ida_alloc(&vmd_instance_ida, GFP_KERNEL);	/* [한국어] 인스턴스 번호를 받는다 */
	if (vmd->instance < 0)	/* [한국어] 음수면 실패 */
		return vmd->instance;	/* [한국어] 아직 아무것도 잡지 않았으므로 그 값을 바로 반환한다 */

	vmd->name = devm_kasprintf(&dev->dev, GFP_KERNEL, "vmd%d",	/* [한국어] "vmd0" 같은 이름을 만든다. /proc/interrupts 에 이 이름으로 뜬다 */
				   vmd->instance);	/* [한국어] 인스턴스 번호를 붙인다 */
	if (!vmd->name) {	/* [한국어] 할당 실패면 */
		err = -ENOMEM;	/* [한국어] 메모리 부족으로 */
		goto out_release_instance;	/* [한국어] 인스턴스 번호를 반납하고 나간다 */
	}

	err = pcim_enable_device(dev);	/* [한국어] 장치를 enable 한다. pcim_ 은 devm 관리 버전이라 언바인딩 시 자동으로 disable 된다 */
	if (err < 0)	/* [한국어] 실패면 */
		goto out_release_instance;	/* [한국어] 되돌리기로 */

	vmd->cfgbar = pcim_iomap(dev, VMD_CFGBAR, 0);	/* [한국어] CFGBAR 를 매핑한다. 세 번째 인자 0 은 "BAR 크기 전부" */
	if (!vmd->cfgbar) {	/* [한국어] 매핑 실패면 */
		err = -ENOMEM;	/* [한국어] 메모리 부족으로 */
		goto out_release_instance;	/* [한국어] 되돌리기로 */
	}

	pci_set_master(dev);	/* [한국어] Bus Master 를 켠다. 하위 장치들의 DMA 가 이 VMD 를 통해 나가므로 반드시 필요하다 */
	if (dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(64)) &&	/* [한국어] 64 비트 DMA 를 먼저 시도하고 */
	    dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(32))) {	/* [한국어] 안 되면 32 비트로 낮춘다. 둘 다 실패하면(둘 다 0 이 아닌 값을 돌려주면) */
		err = -ENODEV;	/* [한국어] 쓸 수 없는 장치 */
		goto out_release_instance;	/* [한국어] 되돌리기로 */
	}

	if (features & VMD_FEAT_OFFSET_FIRST_VECTOR)	/* [한국어] 벡터 0 을 소프트웨어 트리거로 쓰는 하드웨어라면 */
		vmd->first_vec = 1;	/* [한국어] 중계에 쓸 첫 벡터를 1 번으로 민다 */

	raw_spin_lock_init(&vmd->cfg_lock);	/* [한국어] config 접근 직렬화 락을 초기화한다. vmd_enable_domain() 의 열거가 곧 이 락을 쓴다 */
	pci_set_drvdata(dev, vmd);	/* [한국어] pci_get_drvdata() 로 되찾을 수 있게 매단다. vmd_remove/shutdown/suspend/resume 이 이것을 쓴다 */
	err = vmd_enable_domain(vmd, features);	/* [한국어] 준비가 끝났으니 실제 도메인 생성으로 넘어간다 */
	if (err)	/* [한국어] 실패면 */
		goto out_release_instance;	/* [한국어] 되돌리기로 */

	dev_info(&vmd->dev->dev, "Bound to PCI domain %04x\n",	/* [한국어] 성공을 로그로 남긴다. 여기 찍히는 도메인 번호가 lspci 에 보이는 앞자리다 */
		 vmd->sysdata.domain);	/* [한국어] 예: 10000 */
	return 0;	/* [한국어] 바인딩 성공 */

 out_release_instance:	/* [한국어] 실패 되돌리기 지점. devm 관리분은 코어가 정리하므로 인스턴스 번호만 반납하면 된다 */
	ida_free(&vmd_instance_ida, vmd->instance);	/* [한국어] IDA 에 번호를 돌려준다 */
	return err;	/* [한국어] 실패 코드를 pci-driver.c 로 올려보낸다 */
}

/* [한국어]
 * vmd_cleanup_srcu - 벡터마다 초기화해 둔 SRCU 구조체를 정리한다
 *
 * @vmd: 이 VMD 인스턴스.  @return: 없음.
 *
 * vmd_irq_list 배열 자체는 devm 관리라 자동으로 해제되지만, 그 안의
 * srcu_struct 는 커널 SRCU 코어에 등록된 것이라 명시적으로 떼어 내야
 * 한다. 그러지 않으면 이미 해제된 메모리를 SRCU 코어가 계속 참조한다.
 *
 * msix_count 가 0 인 구성(중계 우회 모드)에서는 루프가 한 번도 돌지 않아
 * 자연스럽게 아무 일도 하지 않는다.
 *
 * 실행 컨텍스트: remove 경로. cleanup_srcu_struct 가 잠들 수 있으므로
 * 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vmd_remove() → [이 함수] → cleanup_srcu_struct()
 */
static void vmd_cleanup_srcu(struct vmd_dev *vmd)
{
	int i;	/* [한국어] 벡터 순회 커서 */

	for (i = 0; i < vmd->msix_count; i++)	/* [한국어] 확보했던 벡터 수만큼(우회 모드면 0 이라 한 번도 돌지 않는다) */
		cleanup_srcu_struct(&vmd->irqs[i].srcu);	/* [한국어] SRCU 를 커널 코어에서 떼어 낸다. 배열 메모리는 devm 이 해제하지만 이 등록은 명시적으로 풀어야 한다 */
}

/* [한국어]
 * vmd_remove - 도메인을 통째로 걷어낸다
 *
 * @dev: 언바인딩되는 VMD 장치.  @return: 없음.
 *
 * struct pci_driver vmd_drv 의 .remove 슬롯. pci-driver.c 의
 * pci_device_remove() 가 부른다.
 *
 * 순서가 전부다. 만든 순서의 정확한 역순이어야 한다.
 *
 *   (1) pci_stop_root_bus() — 도메인 안 장치들의 드라이버를 떼어 낸다.
 *       아직 struct pci_dev 는 남아 있다.
 *   (2) sysfs 심볼릭 링크 제거.
 *   (3) pci_remove_root_bus() — 이제 struct pci_dev 들과 버스를 없앤다.
 *       (1)과 나뉘어 있는 이유는 드라이버를 떼는 일과 구조체를 없애는
 *       일을 분리해야 참조가 남은 상태에서 해제하는 사고를 막을 수 있어서다.
 *   (4) SRCU 정리 — 하위 인터럽트가 모두 사라진 뒤여야 안전하다.
 *   (5) 자원 트리 연결 끊기 — 하위 장치의 자원이 모두 반납된 뒤여야 한다.
 *   (6) IRQ 도메인 제거와 재매핑 원복.
 *   (7) 인스턴스 번호와 도메인 번호 반납.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 하위 드라이버들의 remove 가 도는
 * 동안 상당한 시간이 걸릴 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci-driver.c 의 pci_device_remove() → [이 함수]
 *   → pci_stop_root_bus(), pci_remove_root_bus(), vmd_remove_irq_domain()
 */
static void vmd_remove(struct pci_dev *dev)
{
	struct vmd_dev *vmd = pci_get_drvdata(dev);	/* [한국어] probe 에서 매달아 둔 인스턴스 상태를 되찾는다 */

	/* [한국어] 도메인 전체를 정지시킨다. 아래 장치부터 차례로 드라이버가
	 * 떨어져 나가므로, VMD 뒤의 NVMe 들은 여기서 nvme_remove() 를 거친다.
	 * stop 과 remove 가 나뉜 이유는 드라이버를 떼는 일과 struct pci_dev 를
	 * 없애는 일을 분리해야 참조가 남은 상태에서 해제하는 사고를 막을 수 있어서다. */
	pci_stop_root_bus(vmd->bus);
	sysfs_remove_link(&vmd->dev->dev.kobj, "domain");	/* [한국어] /sys/.../domain 심볼릭 링크를 지운다 */
	pci_remove_root_bus(vmd->bus);	/* [한국어] 이제 struct pci_dev 들과 버스 자체를 없앤다 */
	vmd_cleanup_srcu(vmd);	/* [한국어] 하위 인터럽트가 모두 사라진 뒤에야 SRCU 를 정리할 수 있다 */
	vmd_detach_resources(vmd);	/* [한국어] 자원 트리에서 부모-자식 연결을 끊는다. 하위 장치의 자원이 모두 반납된 뒤여야 한다 */
	vmd_remove_irq_domain(vmd);	/* [한국어] IRQ 도메인을 없애고 재매핑 설정을 원복한다 */
	ida_free(&vmd_instance_ida, vmd->instance);	/* [한국어] 인스턴스 번호 반납 */
	pci_bus_release_emul_domain_nr(vmd->sysdata.domain);	/* [한국어] 도메인 번호 반납. 다음 VMD 가 이 번호를 다시 쓸 수 있게 된다 */
}

/* [한국어]
 * vmd_shutdown - 시스템 종료 직전 IRQ 도메인만 정리한다
 *
 * @dev: 종료 중인 VMD 장치.  @return: 없음.
 *
 * struct pci_driver vmd_drv 의 .shutdown 슬롯. pci-driver.c 의
 * pci_device_shutdown() 이 부른다.
 *
 * remove 와 달리 도메인을 걷어내지 않는다. 곧 전원이 나가거나 다른
 * 커널이 올라오므로 자료구조를 정리할 이유가 없다. 대신
 * vmd_remove_irq_domain() 만 부르는데, 그 안에 있는 "중계 우회 모드였다면
 * 재매핑을 다시 켠다" 는 처리가 목적이다. 그것을 해 두지 않으면 다음에
 * 올라오는 커널이 BIOS 가 손대지 않은 상태를 그대로 물려받아 인터럽트가
 * 동작하지 않을 수 있다.
 *
 * 실행 컨텍스트: 종료 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci-driver.c 의 pci_device_shutdown() → [이 함수]
 *   → vmd_remove_irq_domain()
 */
static void vmd_shutdown(struct pci_dev *dev)
{
	struct vmd_dev *vmd = pci_get_drvdata(dev);	/* [한국어] probe 에서 매달아 둔 인스턴스 상태 */

	vmd_remove_irq_domain(vmd);	/* [한국어] IRQ 도메인만 없앤다. 그 안의 "우회 모드였다면 재매핑을 다시 켠다" 는 처리가 목적이다 */
}

#ifdef CONFIG_PM_SLEEP	/* [한국어] 이하 두 함수는 시스템 절전을 빌드할 때만 컴파일된다 */
/* [한국어]
 * vmd_suspend - 절전 전에 VMD 벡터의 핸들러를 떼어 낸다
 *
 * @dev: 절전 중인 장치(일반 struct device).
 * @return: 항상 0.
 *
 * SIMPLE_DEV_PM_OPS 로 .suspend 와 .freeze/.poweroff 에 함께 꽂힌다.
 * pci-driver.c 의 pci_pm_suspend() 등이 부른다.
 *
 * 하는 일은 확보해 둔 모든 벡터에서 vmd_irq() 핸들러를 떼는 것뿐이다.
 * 벡터 자체(pci_alloc_irq_vectors 로 잡은 것)는 반납하지 않는다.
 * config space 저장이나 D-state 전환은 PCI 계층(pci_pm_suspend_noirq)이
 * 알아서 하므로 여기서 할 일이 아니다.
 *
 * msix_count 가 0 인 중계 우회 구성에서는 루프가 돌지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 인터럽트가 아직 살아 있는 단계.
 * CONFIG_PM_SLEEP 에서만 컴파일된다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   PM 코어 → pci-driver.c 의 pci_pm_suspend() → [이 함수]
 *   → devm_free_irq()
 */
static int vmd_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] PM 코어는 일반 device 를 넘기므로 pci_dev 로 되돌린다 */
	struct vmd_dev *vmd = pci_get_drvdata(pdev);	/* [한국어] 거기서 인스턴스 상태를 되찾는다 */
	int i;	/* [한국어] 벡터 순회 커서 */

	for (i = 0; i < vmd->msix_count; i++)	/* [한국어] 확보한 벡터 전부에 대해(우회 모드면 0 이라 돌지 않는다) */
		devm_free_irq(dev, vmd->irqs[i].virq, &vmd->irqs[i]);	/* [한국어] 핸들러를 뗀다. 벡터 자체는 반납하지 않는다 — 복귀 때 그대로 다시 쓴다 */

	return 0;	/* [한국어] 실패할 여지가 없다 */
}

/* [한국어]
 * vmd_resume - 복귀 후 재매핑 설정과 핸들러를 되살린다
 *
 * @dev: 복귀 중인 장치(일반 struct device).
 * @return: 0 성공, devm_request_irq 가 실패하면 그 errno.
 *
 * vmd_suspend() 의 짝이지만 한 가지가 더 있다. 먼저
 * vmd_set_msi_remapping() 으로 하드웨어의 재매핑 설정을 되돌린다.
 * 인자가 !!vmd->irq_domain 인 것이 요령인데 — 이 인스턴스가 MSI 도메인을
 * 가지고 있다는 것은 곧 중계 모드로 돌고 있다는 뜻이므로 재매핑을 켜야
 * 하고, 없으면 우회 모드이므로 꺼야 한다. probe 때의 판정을 다시 계산하지
 * 않고 그 결과물의 존재 여부로 되짚는 것이다.
 *
 * 그다음 모든 벡터에 vmd_irq() 를 다시 건다. 인자는 vmd_alloc_irqs() 의
 * 것과 완전히 같다.
 *
 * config space 복원은 PCI 계층(pci_pm_resume_noirq)이 이미 끝냈으므로
 * 여기서는 신경 쓰지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 인터럽트가 살아난 뒤.
 *
 * 에러 경로: 핸들러 등록이 실패하면 그 자리에서 반환한다. 앞서 성공한
 * 것들은 되돌리지 않는다(상류 코드 그대로).
 *
 * 호출 체인:
 *   PM 코어 → pci-driver.c 의 pci_pm_resume() → [이 함수]
 *   → vmd_set_msi_remapping(), devm_request_irq()
 */
static int vmd_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] 일반 device 를 pci_dev 로 */
	struct vmd_dev *vmd = pci_get_drvdata(pdev);	/* [한국어] 인스턴스 상태 */
	int err, i;	/* [한국어] err 는 오류 코드, i 는 순회 커서 */

	vmd_set_msi_remapping(vmd, !!vmd->irq_domain);	/* [한국어] MSI 도메인이 있다 = 중계 모드다. 그 사실로 재매핑을 켤지 끌지를 되짚는다. probe 때의 판정을 다시 계산하지 않는 요령이다 */

	for (i = 0; i < vmd->msix_count; i++) {	/* [한국어] 확보했던 벡터 전부에 */
		err = devm_request_irq(dev, vmd->irqs[i].virq,	/* [한국어] 핸들러를 다시 건다. 인자는 vmd_alloc_irqs() 의 것과 완전히 같다 */
				       vmd_irq, IRQF_NO_THREAD,	/* [한국어] 중계 핸들러는 스레드화하지 않는다 */
				       vmd->name, &vmd->irqs[i]);	/* [한국어] 이름과 핸들러 인자도 동일 */
		if (err)	/* [한국어] 등록 실패면 */
			return err;	/* [한국어] 그대로 반환한다. 앞서 성공한 것들은 되돌리지 않는다(상류 코드 그대로) */
	}

	return 0;	/* [한국어] 성공 */
}
#endif	/* [한국어] CONFIG_PM_SLEEP 끝 */
/* [한국어] SIMPLE_DEV_PM_OPS 는 struct dev_pm_ops vmd_dev_pm_ops 를 만들면서
 * .suspend/.resume 뿐 아니라 하이버네이션 계열(.freeze/.thaw/.poweroff/
 * .restore)까지 같은 두 함수로 채워 준다. VMD 는 절전 종류에 따라 다르게
 * 할 일이 없어 이 단순 매크로로 충분하다.
 * 주의: 이 줄이 #endif 밖에 있어 CONFIG_PM_SLEEP 이 꺼져 있어도 컴파일된다.
 * 그 경우에도 성립하도록 매크로 쪽이 함수 이름을 조건부로 감싼다(상류 코드 그대로). */
static SIMPLE_DEV_PM_OPS(vmd_dev_pm_ops, vmd_suspend, vmd_resume);

/* [한국어]
 * vmd_ids - 이 드라이버가 맡는 VMD 하드웨어 목록
 *
 * pci-driver.c 의 pci_match_id() 가 이 표를 훑어 짝을 찾는다. 전부
 * PCI_VDEVICE(INTEL, ...) 라 벤더는 인텔로 고정이고 Device ID 만 다르다.
 * .driver_data 에 실린 것이 enum vmd_features 조합이며, 그것이 곧
 * "이 세대 하드웨어가 무엇을 할 수 있는가" 다. vmd_probe() 가 id->driver_data
 * 로 꺼내 쓴다.
 *
 * 설정자: 컴파일 시점 상수.
 * 읽는 자: pci_match_id()(대조), vmd_probe()(기능 비트 추출),
 *   그리고 MODULE_DEVICE_TABLE 이 만들어 내는 modules.alias 항목.
 * 값 범위: 마지막 {0,} 이 표의 끝 표시다. 이것이 없으면 pci_match_id() 의
 *   순회가 끝나지 않는다.
 * 동기화: 읽기 전용.
 *
 * PCI_DEVICE_ID_INTEL_VMD_* 상수들은 include/linux/pci_ids.h 에 있을 것으로
 * 보이나 그 파일이 이 스파스 체크아웃에 없어 실제 값은 확인하지 못했다.
 * 숫자로 직접 적힌 것들은 클라이언트 플랫폼용이며 모두 VMD_FEATS_CLIENT 를 쓴다.
 */
static const struct pci_device_id vmd_ids[] = {
	{PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_VMD_201D),	/* [한국어] shadow 레지스터가 벤더 고유 capability 에 있는 세대 */
		.driver_data = VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP,},
	{PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_VMD_28C0),	/* [한국어] shadow 가 MEMBAR2 안에 있고 버스 제한과 MSI 중계 우회를 모두 지원하는 세대 */
		.driver_data = VMD_FEAT_HAS_MEMBAR_SHADOW |
				VMD_FEAT_HAS_BUS_RESTRICTIONS |
				VMD_FEAT_CAN_BYPASS_MSI_REMAP,},
	{PCI_VDEVICE(INTEL, 0x467f),	/* [한국어] 이하 클라이언트 플랫폼들 — 공통 기능 묶음을 쓴다 */
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0x4c3d),	/* [한국어] 클라이언트 */
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xa77f),	/* [한국어] 클라이언트 */
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0x7d0b),	/* [한국어] 클라이언트 */
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xad0b),	/* [한국어] 클라이언트 */
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_VMD_9A0B),	/* [한국어] 클라이언트. 이것만 이름 상수로 정의되어 있다 */
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xb60b),	/* [한국어] 클라이언트. 아래 세 항목은 들여쓰기가 탭이 아니라 공백인데 상류 코드 그대로 둔다 */
                .driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xb06f),	/* [한국어] 클라이언트 */
                .driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xb07f),	/* [한국어] 클라이언트 */
                .driver_data = VMD_FEATS_CLIENT,},
	{0,}	/* [한국어] 표의 끝 표시. pci_match_id() 의 순회 종료 조건이다 */
};
/* [한국어] 위 표를 모듈 메타데이터로 내보낸다. 빌드 시 이 표에서
 * "pci:v00008086d0000467f..." 같은 별칭 문자열이 뽑혀 modules.alias 에
 * 들어가고, udev 가 pci_uevent() 가 보낸 MODALIAS 와 대조해 이 모듈을
 * 자동으로 올린다. 이것이 없으면 사용자가 손으로 modprobe vmd 해야 한다. */
MODULE_DEVICE_TABLE(pci, vmd_ids);

/* [한국어]
 * vmd_drv - 이 파일이 PCI 장치 드라이버임을 보여 주는 구조체
 *
 * 파일이 drivers/pci/controller/ 에 있지만 platform_driver 가 아니라
 * pci_driver 라는 점이 이 드라이버의 성격을 말해 준다. VMD 는 자기 자신이
 * PCI 엔드포인트로 열거되고, pci-driver.c 의 pci_bus_match() ->
 * pci_device_probe() 를 거쳐 여느 드라이버와 똑같이 바인딩된다.
 *
 * 설정자: 컴파일 시점 상수.
 * 읽는 자: pci-driver.c 의 매칭·바인딩·전원관리 경로 전부.
 * 값 범위: .err_handler 나 .sriov_configure 슬롯은 채우지 않는다.
 * 동기화: 등록 후 읽기 전용.
 */
static struct pci_driver vmd_drv = {
	.name		= "vmd",	/* [한국어] /sys/bus/pci/drivers/vmd 라는 경로가 여기서 나온다 */
	.id_table	= vmd_ids,	/* [한국어] pci_match_id() 가 대조할 표 */
	.probe		= vmd_probe,	/* [한국어] local_pci_probe() 가 부른다 */
	.remove		= vmd_remove,	/* [한국어] pci_device_remove() 가 부른다 */
	.shutdown	= vmd_shutdown,	/* [한국어] pci_device_shutdown() 이 부른다 */
	.driver		= {	/* [한국어] 안에 박힌 일반 device_driver. __pci_register_driver() 가 나머지 필드를 채운다 */
		.pm	= &vmd_dev_pm_ops,	/* [한국어] pci-driver.c 의 pci_pm_* 들이 각 단계에서 부른다 */
	},
};
/* [한국어] module_init/module_exit 두 함수를 대신 만들어 주는 매크로.
 * 펼치면 init 쪽이 pci_register_driver(&vmd_drv) 를, exit 쪽이
 * pci_unregister_driver(&vmd_drv) 를 부른다. 즉 이 한 줄이 pci-driver.c 의
 * __pci_register_driver() 로 들어가는 입구다. */
module_pci_driver(vmd_drv);

MODULE_AUTHOR("Intel Corporation");	/* [한국어] modinfo 에 뜨는 작성자 */
MODULE_DESCRIPTION("Volume Management Device driver");	/* [한국어] modinfo 설명 */
MODULE_LICENSE("GPL v2");	/* [한국어] 라이선스 선언. EXPORT_SYMBOL_GPL 심볼을 쓰려면 GPL 계열이어야 한다 */
MODULE_VERSION("0.6");	/* [한국어] 드라이버 자체 버전 문자열 */
