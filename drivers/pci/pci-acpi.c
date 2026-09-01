// SPDX-License-Identifier: GPL-2.0
/*
 * PCI support in ACPI
 *
 * Copyright (C) 2005 David Shaohua Li <shaohua.li@intel.com>
 * Copyright (C) 2004 Tom Long Nguyen <tom.l.nguyen@intel.com>
 * Copyright (C) 2004 Intel Corp.
 */

/*
 * [한국어 설명] PCI 와 ACPI 펌웨어를 잇는 계층 (pci-acpi.c)
 *
 * === 파일의 역할 ===
 * ACPI 는 펌웨어가 OS 에게 하드웨어를 기술하는 방식이다. PCI 장치에 대해
 * 펌웨어만 아는 정보가 여럿 있고, 이 파일이 그것을 커널로 가져온다.
 * PCI 코어 쪽에서 ACPI 를 향해 내미는 손이 이 파일이고, 반대쪽 손인
 * drivers/acpi/pci_root.c 는 이 스파스 체크아웃에 없다.
 *
 * 이 파일이 실제로 하는 일은 크게 다섯 갈래다(파일을 전수로 훑어 정리했다).
 *
 *   1) 전원 관리 — 이 파일에서 가장 큰 덩어리 중 하나다. ACPI 의 D-state 와
 *      PCI 의 D-state 를 잇는다. acpi_pci_set_power_state() 가 _PS0/_PS3 를
 *      평가하고, acpi_pci_get_power_state() 가 현재 상태를 읽고,
 *      acpi_pci_choose_state() 가 시스템 수면 상태에 맞는 D-state 를 고른다.
 *      깨우기(_PRW/_DSW)와 D3cold 진입 판정(acpi_pci_bridge_d3)도 여기다.
 *      pci.c:1969, 1996, 2022, 2045, 2071, 2098, 2123, 2150 이 전부 이 파일의
 *      함수로 넘어오는 자리다.
 *
 *   2) _HPX/_HPP 권장값 적용 — 파일의 절반 가까이(약 900줄)를 차지한다.
 *      펌웨어가 "이 슬롯의 장치는 이렇게 설정하라" 고 남긴 값을 Type 0/1/2/3
 *      네 종류로 나눠 파싱하고 config space 에 써 넣는다. 진입점은
 *      pci_acpi_program_hp_params() 하나이고, 그 아래로
 *      acpi_run_hpx()/acpi_run_hpp() -> decode_*_hpx_record() ->
 *      program_hpx_type*() 로 갈라진다.
 *
 *   3) ACPI companion 연결 — PCI 장치와 ACPI 네임스페이스 노드를 짝지어
 *      준다. acpi_pci_find_companion() 이 _ADR 로 짝을 찾고,
 *      pci_acpi_setup()/pci_acpi_cleanup() 이 그 짝에 딸린 알림과 깨우기
 *      설정을 붙이고 뗀다. 이 연결이 없으면 위 1)의 어떤 것도 동작하지 않는다.
 *
 *   4) 소유권 판정 결과의 *소비* — pciehp_is_native()/shpchp_is_native() 가
 *      "이 핫플러그 포트를 커널이 다루는가" 를 답한다. 여기서 주의할 점은,
 *      그 소유권을 *정하는* _OSC 협상 코드는 이 파일에 없다는 것이다
 *      (아래 "이 파일에 없는 것" 절 참고). 이 파일은 그 결과가 담긴
 *      host->native_* 비트를 읽어 답할 뿐이다.
 *
 *   5) ARM64/RISC-V 전용 열거 지원 — 파일 끝의 #if 블록이다. MCFG 로
 *      ECAM 창을 만들고(pci_acpi_setup_ecam_mapping), 루트 브리지를 열거하고
 *      (pci_acpi_scan_root), _PRT 기반 INTx 를 배정한다(pcibios_alloc_irq).
 *      x86 은 이 일들을 아키텍처 코드가 따로 한다.
 *
 * === 이 파일에 없는 것 (기존 주석의 오류를 코드로 바로잡음) ===
 * 이전 주석은 "_OSC 소유권 협상이 이 파일에서 가장 중요한 부분" 이라 적고
 * 요약에 negotiate_os_control() 과 acpi_pci_osc_control_set() 을 올렸다.
 * 두 함수 모두 이 트리 전체에 존재하지 않는다(전수 grep). 상류 리눅스에서
 * 그것들은 drivers/acpi/pci_root.c 에 있고, 그 디렉터리는 이 스파스
 * 체크아웃에 포함되지 않았다. 같은 이유로 "부팅 흐름이 acpi_pci_root_add()
 * 계열로 들어온다" 는 서술도 이 트리에서는 확인할 수 없다 — 그 이름 역시
 * 여기 없다. 이 파일 안에서 _OSC 라는 문자열이 나오는 곳은 주석뿐이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 열거(모든 아키텍처):
 *          probe.c:2489  -> [이 파일] pci_acpi_preserve_config()
 *                           _DSM "PCI Boot Configuration" 으로
 *                           "펌웨어 자원 배치를 그대로 두라" 인지 묻는다
 *          probe.c:4933  -> [이 파일] pci_set_acpi_fwnode()
 *                           -> acpi_pci_find_companion() 으로 짝을 찾아 건다
 *          probe.c:5862  -> [이 파일] pci_acpi_program_hp_params()
 *                           _HPX/_HPP 권장값을 config space 에 적용
 *          probe.c:2386  -> [이 파일] pci_host_bridge_acpi_msi_domain()
 *          probe.c:2608  -> [이 파일] pcibios_root_bridge_prepare() (ARM64/RISC-V)
 *          probe.c:2672, 3071 -> [이 파일] pcibios_add_bus() (ARM64/RISC-V)
 *          remove.c:183  -> [이 파일] pcibios_remove_bus() (ARM64/RISC-V)
 *
 * 전원 관리:  pci.c 의 플랫폼 훅들
 *          pci.c:1969 -> acpi_pci_power_manageable()
 *          pci.c:1996 -> acpi_pci_set_power_state()   -> ACPI _PS0/_PS3
 *          pci.c:2022 -> acpi_pci_get_power_state()
 *          pci.c:2045 -> acpi_pci_refresh_power_state()
 *          pci.c:2071 -> acpi_pci_choose_state()      -> ACPI _SxD/_SxW
 *          pci.c:2098 -> acpi_pci_wakeup()            -> ACPI _PRW
 *          pci.c:2123 -> acpi_pci_need_resume()
 *          pci.c:2150 -> acpi_pci_bridge_d3()         -> _S0W, _DSD
 *
 * 리셋:    pci.c:9953 의 pci_reset_fn_methods[] 첫 항목이
 *          pci_dev_acpi_reset() 이다 — ACPI _RST 를 쓰는 리셋 방법.
 *
 * 핫플러그: hotplug/acpiphp_glue.c:1830 과 :1995 가 밝히듯, 이 파일의
 *          acpi_pci_add_bus()/acpi_pci_remove_bus() 가 acpiphp 의
 *          슬롯 열거·해제를 부른다.
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트. ACPI 메서드 평가는 AML
 * 인터프리터를 돌리므로 시간이 걸리고 잠들 수 있다. 그래서 이 파일의
 * 어떤 함수도 인터럽트 문맥에서 부를 수 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/acpi/ 의 ACPI 코어(이 트리에 없음), probe.c(열거),
 *   pci.c(전원 관리·리셋), remove.c(버스 제거).
 * 아래쪽: ecam.c 의 pci_ecam_create(), access.c 의 config 접근 헬퍼,
 *   그리고 acpi_evaluate_object()/acpi_evaluate_dsm_typed() 등 ACPI 평가 API.
 * 옆쪽: hotplug/acpiphp_glue.c(슬롯 열거), pcie/edr.c(EDR 알림 등록·해제를
 *   pci_acpi_setup/cleanup 이 대신 불러 준다), hotplug/shpchp_core.c
 *   (shpc_managed 비트를 세우고 이 파일의 shpchp_is_native() 가 읽는다).
 * 공유 상태: struct pci_host_bridge 의 native_* 비트(소유권 판정 결과.
 *   probe.c:1754 등이 기본값 1 로 세우고, 트리 밖 _OSC 코드가 낮춘다),
 *   struct pci_dev 의 ACPI companion 포인터, pdev->d3hot_delay/d3cold_delay.
 *
 * === 엔드포인트와의 접점 (근거를 확인한 것만) ===
 * 이 파일은 엔드포인트의 종류를 가리지 않는 코어 계층이다. 어떤 장치
 * 드라이버도 이 파일의 함수를 직접 부르지 않는다(drivers/nvme 전수 확인
 * 결과 0건). 다만 이 파일이 만들어 주는 ACPI companion 연결이 있어야
 * 장치 드라이버가 ACPI 속성을 조회할 수 있다. 이 트리에서 확인되는
 * 유일한 실사용 예가 그것이다.
 *
 *   nvme_pci_alloc_dev()  [drivers/nvme/host/pci.c:4686~4696]
 *     if (!noacpi &&
 *         !(quirks & NVME_QUIRK_FORCE_NO_SIMPLE_SUSPEND) &&
 *         acpi_storage_d3(&pdev->dev)) {
 *             dev_info(&pdev->dev,
 *                      "platform quirk: setting simple suspend\n");
 *             quirks |= NVME_QUIRK_SIMPLE_SUSPEND;
 *     }
 *
 * acpi_storage_d3() 는 ACPI 의 StorageD3Enable 속성을 읽어 "이 플랫폼은
 * 스토리지를 D3 로 내려야 제대로 절전된다" 는 펌웨어 지시를 확인한다.
 * 그 함수 자체는 drivers/acpi/ 에 있어 이 트리에 없지만, 그것이 읽는
 * ACPI companion 을 pci_set_acpi_fwnode() -> acpi_pci_find_companion() 이
 * 걸어 준다. 즉 이 파일은 그 판단의 *토대* 를 만든다.
 *
 * (이전 주석의 다음 서술들은 코드로 반증되어 지웠다:
 *  "이 파일이 MSI/MSI-X IRQ 도메인, ASPM, config space 접근을 제어한다" —
 *  MSI 도메인은 msi/irqdomain.c, ASPM 은 pcie/aspm.c, config 접근은
 *  access.c 가 담당한다. 이 파일이 MSI 에 대해 하는 일은
 *  pci_msi_register_fwnode_provider() 로 콜백을 받아 두었다가
 *  pci_host_bridge_acpi_msi_domain() 에서 fwnode 를 되찾아 주는 것뿐이다.
 *  또 함수마다 "NVMe BAR", "NVMe doorbell", "NVMe MSI-X vector" 를 끌어다
 *  붙였으나 이 파일에는 BAR 도 doorbell 도 등장하지 않는다 — 전부 지웠다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_acpi_program_hp_params()  : _HPX/_HPP 권장값을 장치에 적용한다.
 *                                 ACPI 노드를 위로 거슬러 올라가며 찾는다.
 * acpi_run_hpx() / acpi_run_hpp(): 그 메서드를 실제로 평가하고 Type 별로 분배.
 * program_hpx_type0/1/2/3()     : 각 Type 의 값을 config space 에 쓴다.
 * acpi_pci_set_power_state()    : _PS0/_PS3 로 전원 상태를 바꾼다.
 *                                 D3cold 전후로 _REG 를 평가해 AML 에
 *                                 "config space 를 쓸 수 있다/없다" 를 알린다.
 * acpi_pci_get_power_state()    : 현재 상태를 조회한다.
 * acpi_pci_choose_state()       : 시스템 수면 시 들어갈 D-state 를 고른다.
 * acpi_pci_bridge_d3()          : 이 브리지를 D3 로 내려도 핫플러그 이벤트를
 *                                 놓치지 않는가. _S0W 와 _DSD 를 본다.
 * acpi_pci_wakeup()             : 깨우기를 켜거나 끈다. 이 장치가 못 하면
 *                                 상위 버스로 거슬러 올라간다.
 * acpi_pci_find_companion()     : PCI 장치에 대응하는 ACPI 노드를 _ADR 로 찾는다.
 * pci_acpi_setup() / _cleanup() : companion 이 연결된 뒤/끊기기 전의 일괄 설정.
 * pci_dev_acpi_reset()          : _RST 메서드로 장치를 리셋한다.
 *                                 pci.c:9953 의 pci_reset_fn_methods[] "acpi" 항목.
 * pci_acpi_setup_ecam_mapping() : MCFG 로 ECAM 창을 만든다(ARM64/RISC-V).
 * pci_acpi_scan_root()          : 루트 브리지 하나를 열거한다(ARM64/RISC-V).
 * pcibios_alloc_irq()           : _PRT 로 INTx IRQ 를 배정한다(ARM64/RISC-V).
 * struct hpx_type0/1/2/3        : _HPX 의 네 가지 레코드를 담는 구조체.
 * struct acpi_pci_generic_root_info : 루트 브리지 정보 + ECAM 창 포인터.
 *
 * === 이 트리에서 확인하지 못한 것 ===
 * include/linux/pci-acpi.h, include/linux/pci.h, include/acpi/ 가 이
 * 스파스 체크아웃에 없다. 따라서 DSM_PCI_PRESERVE_BOOT_CONFIG,
 * DSM_PCI_POWER_ON_RESET_DELAY, DSM_PCI_DEVICE_READINESS_DURATIONS 의
 * 실제 함수 번호와, ACPI_STATE_D* / PCI_D* / ACPI_REG_CONNECT 등 상수의
 * 값은 여기서 확인할 수 없다. 아래 주석은 값을 지어내지 않고 "무엇을
 * 뜻하는가" 만 적는다.
 */

#include <linux/delay.h>	/* [한국어] 지연 헬퍼. 이 파일이 다루는 d3hot_delay/
				 * d3cold_delay 값이 결국 여기 함수들로 쓰인다 */
#include <linux/init.h>		/* [한국어] __init 과 arch_initcall(). 파일 끝의
				 * acpi_pci_init() 을 부팅 단계에 등록한다 */
#include <linux/iommu.h>	/* [한국어] pci_dev_acpi_reset() 이 리셋 전후로 IOMMU 를
				 * 멈췄다 되살리기 때문에 필요하다 */
#include <linux/irqdomain.h>	/* [한국어] irq_find_matching_fwnode(). 호스트 브리지의
				 * MSI irq_domain 을 fwnode 로 찾아 준다 */
#include <linux/pci.h>		/* [한국어] struct pci_dev / pci_bus / pci_host_bridge.
				 * 이 헤더는 스파스 체크아웃에 없어 상수 값은 확인하지 못했다 */
#include <linux/msi.h>		/* [한국어] DOMAIN_BUS_PCI_MSI 같은 MSI 도메인 식별자 */
#include <linux/pci_hotplug.h>	/* [한국어] 핫플러그 공용 선언. acpiphp_enumerate_slots()
				 * 계열과 pciehp/shpchp 판정에 필요하다 */
#include <linux/module.h>	/* [한국어] EXPORT_SYMBOL_GPL. companion lookup 훅 두 개를
				 * 모듈에 노출한다 */
#include <linux/pci-acpi.h>	/* [한국어] PCI-ACPI 다리 선언. pci_acpi_dsm_guid,
				 * DSM_PCI_* 함수 번호, acpi_pci_* 원형이 여기 있다.
				 * 이 트리에 없어 DSM 번호의 실제 값은 확인하지 못했다 */
#include <linux/pci-ecam.h>	/* [한국어] pci_ecam_create()/pci_ecam_free() 와
				 * struct pci_config_window. ARM64/RISC-V 블록에서 쓴다 */
#include <linux/pm_runtime.h>	/* [한국어] pm_request_resume(). 깨우기 알림을 받았을 때
				 * 런타임 PM 에 "이 장치를 깨워 달라" 고 요청한다 */
#include <linux/pm_qos.h>	/* [한국어] dev_pm_qos_flags()/PM_QOS_FLAG_NO_POWER_OFF.
				 * 사용자나 상위 계층이 "전원을 끊지 마라" 고 걸어 둔
				 * 제약을 D3cold 진입 전에 확인한다 */
#include <linux/rwsem.h>	/* [한국어] DECLARE_RWSEM. companion lookup 훅을 보호하는
				 * 읽기/쓰기 세마포어를 만든다 */
#include "pci.h"		/* [한국어] PCI 코어 내부 헤더. pci_dev_is_added(),
				 * pci_dev_reset_iommu_prepare(), pcie_ports_native,
				 * acpi_pci_disabled 등의 선언이 여기서 온다 */

/* [한국어] PCI Firmware Specification 이 정한 _DSM 식별자(GUID).
 *
 * _DSM(Device Specific Method)은 ACPI 의 만능 확장 구멍이다. 한 장치가
 * 여러 규격의 확장 메서드를 동시에 가질 수 있어야 하므로, 어느 규격의
 * 어느 함수를 부르는지 GUID + 리비전 + 함수 번호로 지정한다. 이 GUID 가
 * "PCI Firmware Spec 이 정의한 함수들" 을 가리킨다.
 *
 * 이 파일이 이 GUID 로 부르는 함수는 셋이다.
 *   DSM_PCI_PRESERVE_BOOT_CONFIG      pci_acpi_preserve_config()
 *   DSM_PCI_POWER_ON_RESET_DELAY      acpi_pci_add_bus()
 *   DSM_PCI_DEVICE_READINESS_DURATIONS pci_acpi_optimize_delay()
 * 세 상수의 실제 번호는 include/linux/pci-acpi.h 에 있는데, 그 헤더가
 * 이 스파스 체크아웃에 없어 값을 확인하지 못했다.
 *
 * 설정자: 컴파일 시점 상수. GUID_INIT 이 리틀엔디언 필드 순서까지
 *   규격대로 배치해 준다(앞의 세 필드는 정수, 뒤 여덟 바이트는 그대로).
 * 읽는 자: 이 파일의 세 호출 자리, 그리고 trailing const 라 다른 파일에서도
 *   참조할 수 있다(pci-acpi.h 가 extern 으로 내보낸다).
 * 값 범위: 고정 GUID e5c937d0-3553-4d7a-9117-ea4d19c3434d.
 * 동기화: 읽기 전용 상수라 불필요.
 *
 * The GUID is defined in the PCI Firmware Specification available
 * here to PCI-SIG members:
 * https://members.pcisig.com/wg/PCI-SIG/document/15350
 */
const guid_t pci_acpi_dsm_guid =
	GUID_INIT(0xe5c937d0, 0x3553, 0x4d7a,
		  0x91, 0x17, 0xea, 0x4d, 0x19, 0xc3, 0x43, 0x4d);

#if defined(CONFIG_PCI_QUIRKS) && defined(CONFIG_ARM64)
/* [한국어]
 * acpi_get_rc_addr - ACPI 장치의 _CRS 에서 첫 메모리 자원을 꺼낸다
 *
 * @adev:   대상 ACPI 장치(여기서는 Root Complex 를 나타내는 노드).
 * @res:    [출력] 찾은 자원을 복사해 넣을 자리.
 * @return: 0 = 찾음, 음수 = 실패.
 *
 * _CRS(Current Resource Settings)는 ACPI 장치가 지금 쓰고 있는 자원
 * 목록이다. 여기서는 메모리 자원만 걸러 첫 항목 하나를 가져온다.
 *
 * "첫 항목 하나" 라는 단순화가 성립하는 이유는 이 함수의 유일한 호출자를
 * 보면 드러난다. acpi_get_rc_resources() 는 특정 _HID 로 지목한 노드
 * 하나에서 컨트롤러 레지스터 창 하나를 얻으려는 것이라, 목록이 길 일이
 * 없다. 범용 자원 파서가 아니다.
 *
 * flags 를 unsigned long 지역 변수에 담아 (void *) 로 넘기는 모양이
 * 눈에 띄는데, acpi_dev_filter_resource_type_cb() 콜백이 문맥 포인터를
 * 정수로 되읽는 규약이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 컨트롤러 드라이버 probe 중.
 *   ACPI 평가가 잠들 수 있다.
 * 에러 경로: 파싱 실패(-errno)와 "자원이 하나도 없음"(-EINVAL)을 나눠
 *   보고한다. 앞의 경우 목록이 만들어지지 않았으므로 해제할 것도 없다.
 *
 * 호출 체인:
 *   thunder_pem_acpi_init() [controller/pci-thunder-pem.c:380]
 *     — 그 안에서 thunder_pem_init()(:306)으로 이어진다
 *     -> acpi_get_rc_resources() -> [acpi_get_rc_addr]
 *        -> acpi_dev_get_resources(), acpi_dev_free_resource_list()
 */
static int acpi_get_rc_addr(struct acpi_device *adev, struct resource *res)
{
	struct device *dev = &adev->dev;	/* [한국어] 로그 접두사로 쓸 device. adev 안에 박혀 있는 것을 꺼내 별칭을 만든다 */
	struct resource_entry *entry;	/* [한국어] 자원 목록의 첫 항목을 담을 포인터 */
	struct list_head list;	/* [한국어] ACPI 코어가 채워 줄 자원 목록의 머리 */
	unsigned long flags;	/* [한국어] 걸러 낼 자원 종류. 콜백에 문맥 포인터로 넘기려고 지역 변수에 담는다 */
	int ret;	/* [한국어] 반환값 겸 찾은 자원 개수 */

	INIT_LIST_HEAD(&list);	/* [한국어] 목록을 빈 상태로 초기화한다. 실패해도 acpi_dev_free_resource_list 를 안전하게 부를 수 있게 된다 */
	flags = IORESOURCE_MEM;	/* [한국어] 메모리 자원만 받겠다는 뜻 */
	ret = acpi_dev_get_resources(adev, &list,	/* [한국어] _CRS 를 평가해 자원을 목록에 채운다 */
				     acpi_dev_filter_resource_type_cb,	/* [한국어] 자원 종류로 거르는 표준 콜백 */
				     (void *) flags);	/* [한국어] 그 콜백이 문맥 포인터를 정수로 되읽는 규약이라 (void *) 로 감싸 넘긴다 */
	if (ret < 0) {	/* [한국어] 파싱 자체가 실패했다 */
		dev_err(dev, "failed to parse _CRS method, error code %d\n",	/* [한국어] 펌웨어 테이블 문제는 로그 없이는 추적이 거의 불가능하다 */
			ret);	/* [한국어] 오류 코드까지 남긴다 */
		return ret;	/* [한국어] 목록이 만들어지지 않았으므로 해제할 것도 없다 */
	}

	if (ret == 0) {	/* [한국어] 파싱은 됐는데 자원이 하나도 없다 — 실패와 구분해 보고한다 */
		dev_err(dev, "no IO and memory resources present in _CRS\n");	/* [한국어] 어떤 종류가 없었는지 밝힌다 */
		return -EINVAL;	/* [한국어] 자원 부재는 -EINVAL 로 구분한다 */
	}

	entry = list_first_entry(&list, struct resource_entry, node);	/* [한국어] 첫 메모리 자원 하나만 쓴다. 이 함수의 유일한 호출자가 컨트롤러 레지스터 창 하나를 얻으려는 것이라 목록이 길 일이 없다 */
	*res = *entry->res;	/* [한국어] 구조체 통째로 복사한다 — 목록은 곧 해제되므로 포인터를 남기면 안 된다 */
	acpi_dev_free_resource_list(&list);	/* [한국어] ACPI 코어가 할당한 목록을 되돌려 준다 */
	return 0;	/* [한국어] 성공 */
}

/* [한국어]
 * acpi_match_rc - _UID 가 찾는 세그먼트 번호와 같은 노드인지 판정하는 콜백
 *
 * @handle: 지금 방문 중인 ACPI 노드.
 * @lvl:    네임스페이스 깊이. 쓰지 않는다.
 * @context: 찾는 세그먼트 번호(u16 *). acpi_get_devices() 가 그대로 전달한다.
 * @retval: [출력] 일치했을 때 그 노드의 핸들을 써 넣을 자리.
 * @return: AE_CTRL_DEPTH = 이 가지 아래로 더 내려가지 마라(불일치),
 *          AE_CTRL_TERMINATE = 찾았으니 순회를 끝내라.
 *
 * acpi_get_devices() 가 _HID 로 후보를 좁힌 뒤, 후보마다 이 콜백을 부른다.
 * 같은 _HID 를 가진 Root Complex 가 여러 개인 시스템에서 세그먼트 번호로
 * 하나를 고르는 것이 목적이다.
 *
 * 반환값 두 가지가 모두 "성공" 이 아니라는 점이 헷갈리기 쉽다.
 * ACPI 순회 콜백은 반환값으로 순회를 제어하는데, AE_CTRL_DEPTH 는
 * "이 노드는 아니니 하위로 내려가지 말고 다음 형제로" 라는 뜻이고
 * AE_CTRL_TERMINATE 만이 "찾았다" 를 뜻한다. 실패도 AE_CTRL_DEPTH 로
 * 표현되므로, _UID 평가가 실패한 경우와 값이 다른 경우가 한 줄에 묶여 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, ACPI 네임스페이스 순회 중.
 *
 * 호출 체인:
 *   acpi_get_rc_resources() -> acpi_get_devices() -> [acpi_match_rc]
 */
static acpi_status acpi_match_rc(acpi_handle handle, u32 lvl, void *context,
				 void **retval)
{
	u16 *segment = context;	/* [한국어] acpi_get_devices 가 그대로 전달한 문맥. 찾는 세그먼트 번호다 */
	unsigned long long uid;	/* [한국어] _UID 평가 결과를 담을 자리. ACPI 정수는 64비트다 */
	acpi_status status;	/* [한국어] 평가 결과 상태 */

	status = acpi_evaluate_integer(handle, METHOD_NAME__UID, NULL, &uid);	/* [한국어] _UID(Unique ID)를 평가한다. 같은 _HID 가 여럿일 때의 구분자다 */
	if (ACPI_FAILURE(status) || uid != *segment)	/* [한국어] 평가 실패와 값 불일치를 한 줄에 묶었다 — 둘 다 "이 노드가 아니다" 로 처리하면 되기 때문 */
		return AE_CTRL_DEPTH;	/* [한국어] 이 가지 아래로 더 내려가지 말고 다음 형제로. 순회 콜백의 관례상 이것이 "불일치" 를 뜻한다 */

	*(acpi_handle *)retval = handle;	/* [한국어] 찾았으니 핸들을 호출자 자리에 써 넣는다 */
	return AE_CTRL_TERMINATE;	/* [한국어] 순회를 끝내라. 이것만이 "찾았다" 를 뜻한다 */
}

/* [한국어]
 * acpi_get_rc_resources - _HID 와 세그먼트로 노드를 찾아 그 메모리 자원을 준다
 *
 * @dev:     오류를 보고할 장치(로그 접두사용).
 * @hid:     찾을 ACPI _HID 문자열. 호출자가 자기 하드웨어의 ID 를 안다.
 * @segment: PCI 세그먼트(도메인) 번호. 같은 _HID 가 여럿일 때의 구분자.
 * @res:     [출력] 찾은 자원.
 * @return:  0 = 성공, -ENODEV / 그 밖의 음수 = 실패.
 *
 * 이 함수가 이 파일에 있는 이유가 조금 특이하다. ARM64 의 특정 호스트
 * 컨트롤러(Cavium ThunderX PEM)는 ACPI 가 기술한 루트 브리지 자원과 별개로
 * 자기 컨트롤러 레지스터 창을 따로 찾아야 한다. 그 조회를 컨트롤러
 * 드라이버마다 되풀이하지 않도록 코어에 한 벌 두었다.
 *
 * 그래서 #if 조건이 CONFIG_PCI_QUIRKS && CONFIG_ARM64 이다 — 특정 플랫폼의
 * 우회를 위한 코드임을 조건 자체가 말한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 컨트롤러 probe 중.
 * 에러 경로: 세 갈래 모두 dev_err 로 무엇이 없었는지 남긴다 — 펌웨어
 *   테이블 문제는 로그 없이는 추적이 거의 불가능하기 때문이다.
 *
 * 호출 체인:
 *   thunder_pem_init() [controller/pci-thunder-pem.c:392]
 *     -> [acpi_get_rc_resources]
 *        -> acpi_get_devices(hid, acpi_match_rc, ...) -> acpi_get_rc_addr()
 */
int acpi_get_rc_resources(struct device *dev, const char *hid, u16 segment,
			  struct resource *res)
{
	struct acpi_device *adev;	/* [한국어] 찾은 노드의 acpi_device */
	acpi_status status;	/* [한국어] ACPI 호출 결과 */
	acpi_handle handle;	/* [한국어] 콜백이 채워 줄 핸들 */
	int ret;	/* [한국어] acpi_get_rc_addr 의 반환값 */

	status = acpi_get_devices(hid, acpi_match_rc, &segment, &handle);	/* [한국어] _HID 로 후보를 좁힌 뒤 후보마다 acpi_match_rc 를 부른다. segment 를 문맥으로 넘긴다 */
	if (ACPI_FAILURE(status)) {	/* [한국어] 일치하는 노드를 못 찾았다 */
		dev_err(dev, "can't find _HID %s device to locate resources\n",	/* [한국어] 어느 _HID 를 찾다 실패했는지 남긴다 */
			hid);	/* [한국어] 펌웨어 테이블이 예상과 다르다는 뜻이라 로그가 유일한 단서다 */
		return -ENODEV;	/* [한국어] 장치 없음 */
	}

	adev = acpi_fetch_acpi_dev(handle);	/* [한국어] 핸들을 acpi_device 구조체로 되돌린다 */
	if (!adev)	/* [한국어] 핸들은 있는데 등록된 acpi_device 가 없는 경우 */
		return -ENODEV;	/* [한국어] 쓸 수 없다 */

	ret = acpi_get_rc_addr(adev, res);	/* [한국어] 그 노드의 _CRS 에서 첫 메모리 자원을 꺼낸다 */
	if (ret) {	/* [한국어] 자원을 못 얻었다 */
		dev_err(dev, "can't get resource from %s\n",	/* [한국어] 어느 노드에서 실패했는지 이름으로 남긴다 */
			dev_name(&adev->dev));	/* [한국어] acpi_device 의 device 이름 */
		return ret;	/* [한국어] 안쪽 오류 코드를 그대로 올린다 */
	}

	return 0;	/* [한국어] 성공 */
}
#endif

/* [한국어]
 * acpi_pci_root_get_mcfg_addr - _CBA 를 평가해 ECAM 창의 물리 주소를 얻는다
 *
 * @handle: 루트 브리지의 ACPI 핸들. NULL 이어도 된다.
 * @return: ECAM 기준 물리 주소, 없거나 실패하면 0.
 *
 * MCFG 테이블은 부팅 시점에 고정된 ECAM 창 목록을 담는데, 핫플러그되는
 * 호스트 브리지처럼 부팅 후에 생기는 것은 거기에 없다. _CBA(Configuration
 * Base Address)는 그런 경우에 루트 브리지 노드가 직접 자기 ECAM 주소를
 * 알려 주는 메서드다.
 *
 * handle 이 NULL 이어도 되도록 만든 것이 이 함수의 편의점이다. status 를
 * AE_NOT_EXIST 로 미리 초기화해 두어, handle 이 없으면 평가를 건너뛰고
 * 그대로 실패 처리로 흘러간다. 호출자가 NULL 검사를 따로 하지 않아도 된다.
 *
 * 0 을 "없음" 으로 쓰는 것은 물리 주소 0 이 ECAM 창의 기준일 수 없다는
 * 사실에 기댄 관용이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. ACPI 평가가 잠들 수 있다.
 * 에러 경로: 평가 실패와 메서드 부재를 구분하지 않고 모두 0.
 *
 * 호출 체인: 이 스파스 체크아웃 안에는 호출자가 없다(전수 grep).
 *   상류에서는 drivers/acpi/pci_mcfg.c 와 아키텍처별 MMCONFIG 코드가
 *   쓰는데, 그 디렉터리들이 이 트리에 없다.
 */
phys_addr_t acpi_pci_root_get_mcfg_addr(acpi_handle handle)
{
	acpi_status status = AE_NOT_EXIST;	/* [한국어] AE_NOT_EXIST 로 미리 초기화해 두는 것이 요점이다. handle 이 NULL 이면 평가를 건너뛰고 그대로 실패 처리로 흘러간다 — 호출자가 NULL 검사를 따로 하지 않아도 된다 */
	unsigned long long mcfg_addr;	/* [한국어] _CBA 평가 결과를 담을 자리 */

	if (handle)	/* [한국어] 핸들이 있을 때만 평가한다 */
		status = acpi_evaluate_integer(handle, METHOD_NAME__CBA,	/* [한국어] _CBA(Configuration Base Address). 부팅 후에 생긴 호스트 브리지처럼 MCFG 에 없는 경우에 쓴다 */
					       NULL, &mcfg_addr);	/* [한국어] 인자 없이 평가하고 결과를 정수로 받는다 */
	if (ACPI_FAILURE(status))	/* [한국어] 메서드가 없거나 평가가 실패했다 */
		return 0;	/* [한국어] 0 을 "없음" 으로 쓴다. 물리 주소 0 이 ECAM 창의 기준일 수 없다는 사실에 기댄 관용이다 */

	return (phys_addr_t)mcfg_addr;	/* [한국어] 64비트 ACPI 정수를 플랫폼의 물리 주소 형으로 줄인다 */
}

/* [한국어]
 * pci_acpi_preserve_config - 펌웨어가 배치해 둔 자원을 건드리지 말라는 지시인지 묻는다
 *
 * @host_bridge: 판정할 호스트 브리지.
 * @return:      true = 펌웨어 배치를 보존하라, false = 커널이 재배치해도 된다.
 *
 * 보통 커널은 열거하면서 BAR 와 버스 번호를 자기 방식대로 다시 배치한다.
 * 그런데 펌웨어가 이미 특정 배치를 전제로 무언가를 해 둔 경우가 있다 —
 * 부팅 중인 콘솔 장치, 펌웨어가 계속 접근하는 관리 컨트롤러 따위다.
 * 그런 플랫폼은 _DSM 의 "PCI Boot Configuration" 함수로 "그대로 두라" 고
 * 알린다.
 *
 * 반환 규약이 뒤집혀 있어 헷갈리기 쉽다. _DSM 이 **0** 을 돌려줄 때가
 * "보존하라" 다(위 영어 주석이 그 규약을 밝힌다). 그래서 코드는
 * `obj->integer.value == 0` 일 때 true 를 준다.
 *
 * ACPI_TYPE_INTEGER 로 형을 지정해 부르므로, 반환형이 다르면
 * acpi_evaluate_dsm_typed() 가 NULL 을 준다. 그래서 형 검사 코드가
 * 따로 없다.
 *
 * ACPI_FREE 를 obj 가 NULL 일 때도 부르는 것이 눈에 띄는데,
 * ACPI_FREE 는 NULL 을 받아도 안전하다(kfree 와 같은 규약).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 루트 버스 생성 중.
 * 에러 경로: ACPI 핸들이 없거나 _DSM 이 없으면 false — 즉 "커널이
 *   재배치해도 된다" 는 기존 동작이 기본값이다.
 *
 * 호출 체인:
 *   pci_register_host_bridge() [probe.c:2489] -> [pci_acpi_preserve_config]
 *     -> acpi_evaluate_dsm_typed()
 */
bool pci_acpi_preserve_config(struct pci_host_bridge *host_bridge)
{
	bool ret = false;	/* [한국어] 기본은 false — 커널이 자원을 재배치해도 된다 */

	if (ACPI_HANDLE(&host_bridge->dev)) {	/* [한국어] ACPI 핸들이 없으면 물어볼 곳이 없다 */
		union acpi_object *obj;	/* [한국어] _DSM 반환 객체. 이 블록 안에서만 쓰이므로 안쪽에 선언했다 */

		/*
		 * Evaluate the "PCI Boot Configuration" _DSM Function.  If it
		 * exists and returns 0, we must preserve any PCI resource
		 * assignments made by firmware for this host bridge.
		 * [한국어] 규약이 뒤집혀 있다 — 0 이 "보존하라" 다. 그래서
		 * 아래에서 value == 0 일 때 true 를 돌려준다.
		 */	/* [한국어] PCI Firmware Spec 의 _DSM 을 부른다 */
		obj = acpi_evaluate_dsm_typed(ACPI_HANDLE(&host_bridge->dev),	/* [한국어] 이 파일 맨 위의 GUID */
					      &pci_acpi_dsm_guid,	/* [한국어] 리비전 1, 함수 번호는 PRESERVE_BOOT_CONFIG. 그 상수의 실제 값은 include/linux/pci-acpi.h 에 있는데 이 트리에 없어 확인하지 못했다 */
					      1, DSM_PCI_PRESERVE_BOOT_CONFIG,	/* [한국어] 인자 없음, 반환형은 정수로 지정. 형이 다르면 NULL 이 와서 아래 검사가 걸러 낸다 */
					      NULL, ACPI_TYPE_INTEGER);	/* [한국어] 0 이 "보존하라" 다 — 규약이 뒤집혀 있다 */
		if (obj && obj->integer.value == 0)	/* [한국어] 펌웨어 배치를 그대로 두라고 알린다 */
			ret = true;	/* [한국어] NULL 을 받아도 안전하다(kfree 와 같은 규약) */
		ACPI_FREE(obj);
	}
	/* [한국어] 조건에 걸리지 않았으면 false 가 그대로 나간다 */
	return ret;
}

/* _HPX PCI Setting Record (Type 0); same as _HPP */
/* [한국어] _HPX Type 0 레코드 — 가장 오래된 PCI 시절의 설정 묶음.
 * 위 영어 주석이 밝히듯 내용이 _HPP 와 같아서 두 메서드가 이 구조체를
 * 공유한다. _HPP 는 revision 필드가 없고, acpi_run_hpp() 가 그 자리에
 * 1 을 손으로 채워 넣는다. */
struct hpx_type0 {
	/* [한국어] 이 레코드의 판 번호.
	 * 설정자: decode_type0_hpx_record()(_HPX 경로) 또는
	 *   acpi_run_hpp() 가 상수 1 로(_HPP 경로).
	 * 읽는 자: program_hpx_type0() 이 1 보다 크면 해석할 수 없다고 보고
	 *   기본값으로 갈아탄다.
	 * 값 범위: 현재 1 만 정의되어 있다.
	 * 동기화: 스택 변수로만 쓰이므로 불필요. */
	u32 revision;		/* Not present in _HPP */

	/* [한국어] Cache Line Size 레지스터에 쓸 값(dword 단위).
	 * 전통 PCI 에서 버스트 전송 단위를 정하던 필드다. 영어 주석대로
	 * PCIe 에는 해당 없지만, PCIe 장치도 이 레지스터 자리는 갖고 있어
	 * 그대로 쓴다(무시된다).
	 * 설정자: decode_type0_hpx_record() / acpi_run_hpp().
	 * 읽는 자: program_hpx_type0().
	 * 값 범위: 8비트. 기본값 8(= 32바이트).
	 * 동기화: 스택 변수. */
	u8  cache_line_size;	/* Not applicable to PCIe */

	/* [한국어] Latency Timer 레지스터에 쓸 값(PCI 클럭 수).
	 * 전통 PCI 에서 한 마스터가 버스를 붙잡을 수 있는 시간의 상한이었다.
	 * PCIe 에는 버스 중재가 없어 의미가 없다.
	 * 설정자: decode_type0_hpx_record() / acpi_run_hpp().
	 * 읽는 자: program_hpx_type0(). 브리지라면 Secondary Latency Timer 에도
	 *   같은 값을 쓴다.
	 * 값 범위: 8비트. 기본값 0x40.
	 * 동기화: 스택 변수. */
	u8  latency_timer;	/* Not applicable to PCIe */

	/* [한국어] Command 레지스터의 SERR# Enable 을 켤 것인가.
	 * SERR# 은 치명적 오류를 시스템에 알리는 신호다.
	 * 설정자: decode_type0_hpx_record() / acpi_run_hpp().
	 * 읽는 자: program_hpx_type0() 이 0 이 아니면 PCI_COMMAND_SERR 를 켠다.
	 *   끄지는 않는다는 점이 중요하다 — 0 이면 그냥 손대지 않는다.
	 * 값 범위: 0 또는 0 이 아닌 값. 기본값 0.
	 * 동기화: 스택 변수. */
	u8  enable_serr;

	/* [한국어] Command 레지스터의 Parity Error Response 를 켤 것인가.
	 * 설정자: decode_type0_hpx_record() / acpi_run_hpp().
	 * 읽는 자: program_hpx_type0() 이 PCI_COMMAND_PARITY 를 켜고,
	 *   브리지라면 Bridge Control 의 PARITY 도 함께 켠다.
	 * 값 범위: 0 또는 0 이 아닌 값. 기본값 0.
	 * 동기화: 스택 변수. */
	u8  enable_perr;
};

/* [한국어] _HPX/_HPP 가 없거나 해석할 수 없을 때 쓰는 기본값.
 * program_hpx_type0() 이 hpx == NULL 이거나 revision 이 너무 높을 때
 * 이 표로 갈아탄다. 즉 이 파일은 "펌웨어가 아무 말도 하지 않으면 아무것도
 * 하지 않는다" 가 아니라 "기본값이라도 쓴다" 를 택했다.
 * 설정자: 컴파일 시점 상수(const 가 아닌 것은 상류 코드 그대로다).
 * 읽는 자: program_hpx_type0().
 * 값 범위: cache_line_size 8, latency_timer 0x40, 오류 보고는 둘 다 끔.
 * 동기화: 실제로 쓰기가 없어 문제되지 않는다. */
static struct hpx_type0 pci_default_type0 = {
	.revision = 1,		/* [한국어] 아래 값들이 revision 1 형식이라는 표시 */
	.cache_line_size = 8,	/* [한국어] 8 dword = 32바이트. 전통 PCI 의 흔한 기본값 */
	.latency_timer = 0x40,	/* [한국어] 64 PCI 클럭. 역시 관례적 기본값 */
	.enable_serr = 0,	/* [한국어] SERR# 는 건드리지 않는다 — 켜는 판단은 OS 몫 */
	.enable_perr = 0,	/* [한국어] 패리티 오류 응답도 건드리지 않는다 */
};

/* [한국어]
 * program_hpx_type0 - Type 0 레코드를 장치의 config space 에 써 넣는다
 *
 * @dev:    설정할 장치.
 * @hpx:    적용할 값. NULL 이면 pci_default_type0 을 쓴다.
 * @return: 없음.
 *
 * 네 개의 레지스터를 손댄다.
 *   Cache Line Size, Latency Timer   그대로 쓴다(PCIe 에서는 무의미).
 *   Command 의 SERR#/PARITY          요청된 것만 켠다.
 *   브리지라면 Secondary Latency Timer 와 Bridge Control 의 PARITY 도.
 *
 * 오류 보고 비트를 "켜기만" 하고 끄지 않는 것이 이 함수의 성격을 말해
 * 준다. 펌웨어의 권장은 최소 요구사항으로 다루고, OS 나 다른 코드가
 * 이미 켜 둔 것을 되돌리지 않는다.
 *
 * hpx 가 NULL 일 때 기본값으로 갈아타는 것도 눈에 띈다. acpi_run_hpx()
 * 경로에서는 항상 값이 있으므로, 이 갈아타기는 이 함수를 다른 곳에서
 * 재사용할 여지를 남긴 것이다(현재 이 파일 안의 두 호출 모두 값을 준다).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 장치 설정 중. 락 없음.
 * 에러 경로: revision 이 1 보다 크면 경고 후 기본값으로 계속 진행한다.
 *   실패로 끝내지 않는 것은, 해석 못 한 값보다 안전한 기본값이 낫다는 판단이다.
 *
 * 호출 체인:
 *   pci_acpi_program_hp_params() -> acpi_run_hpx() -> [program_hpx_type0]
 *   pci_acpi_program_hp_params() -> acpi_run_hpp() -> [program_hpx_type0]
 */
static void program_hpx_type0(struct pci_dev *dev, struct hpx_type0 *hpx)
{
	u16 pci_cmd, pci_bctl;	/* [한국어] Command 와 Bridge Control 을 담을 임시 변수 */

	if (!hpx)	/* [한국어] 펌웨어가 값을 주지 않았으면 */
		hpx = &pci_default_type0;	/* [한국어] 안전한 기본값으로 갈아탄다 */

	if (hpx->revision > 1) {	/* [한국어] 해석할 수 없는 판이면 */
		pci_warn(dev, "PCI settings rev %d not supported; using defaults\n",	/* [한국어] 조용히 넘기지 않고 무엇을 못 읽었는지 남긴다 */
			 hpx->revision);	/* [한국어] 실제 판 번호까지 찍는다 */
		hpx = &pci_default_type0;	/* [한국어] 역시 기본값으로 갈아탄다 — 해석 못 한 값보다 낫다 */
	}

	pci_write_config_byte(dev, PCI_CACHE_LINE_SIZE, hpx->cache_line_size);	/* [한국어] Cache Line Size. PCIe 에서는 무시되지만 레지스터 자리는 있어 그대로 쓴다 */
	pci_write_config_byte(dev, PCI_LATENCY_TIMER, hpx->latency_timer);	/* [한국어] Latency Timer. 역시 PCIe 에서는 의미 없다 */
	pci_read_config_word(dev, PCI_COMMAND, &pci_cmd);	/* [한국어] Command 는 통째로 덮으면 안 되므로 읽어서 비트만 얹는다 */
	if (hpx->enable_serr)	/* [한국어] SERR# 보고를 켜라고 했으면 */
		pci_cmd |= PCI_COMMAND_SERR;	/* [한국어] 그 비트만 세운다. 끄지는 않는다 — 0 이면 손대지 않는다 */
	if (hpx->enable_perr)	/* [한국어] 패리티 오류 응답을 켜라고 했으면 */
		pci_cmd |= PCI_COMMAND_PARITY;	/* [한국어] 그 비트만 세운다 */
	pci_write_config_word(dev, PCI_COMMAND, pci_cmd);	/* [한국어] 읽어서 얹은 값을 되쓴다 */

	/* Program bridge control value */
	if ((dev->class >> 8) == PCI_CLASS_BRIDGE_PCI) {	/* [한국어] 클래스 코드 상위 16비트가 PCI-to-PCI 브리지인가. 하위 8비트(프로그래밍 인터페이스)는 버린다 */
		pci_write_config_byte(dev, PCI_SEC_LATENCY_TIMER,	/* [한국어] 브리지에는 하위 버스용 Latency Timer 가 따로 있다 */
				      hpx->latency_timer);	/* [한국어] 같은 값을 쓴다 */
		pci_read_config_word(dev, PCI_BRIDGE_CONTROL, &pci_bctl);	/* [한국어] Bridge Control 도 통째로 덮으면 안 된다 */
		if (hpx->enable_perr)	/* [한국어] 패리티 응답을 켜라고 했으면 */
			pci_bctl |= PCI_BRIDGE_CTL_PARITY;	/* [한국어] 하위 버스 쪽 패리티 비트도 세운다 */
		pci_write_config_word(dev, PCI_BRIDGE_CONTROL, pci_bctl);	/* [한국어] 읽어서 얹은 값을 되쓴다 */
	}
}

/* [한국어]
 * decode_type0_hpx_record - _HPX 패키지의 Type 0 레코드를 구조체로 옮긴다
 *
 * @record: ACPI 가 돌려준 패키지 객체 하나(한 레코드).
 * @hpx0:   [출력] 채울 구조체.
 * @return: AE_OK = 성공, AE_ERROR = 형식이 맞지 않음.
 *
 * 레코드의 배치는 [0]=Type, [1]=Revision, [2..]=값들 이다. 그래서
 * fields[1] 을 판 번호로 읽고, 값은 첨자 2 부터 센다.
 *
 * 검사가 두 겹인 것이 이 함수의 요점이다.
 *   package.count 가 정확히 6 인가  — 개수가 다르면 다른 형식이다.
 *   [2..5] 가 모두 INTEGER 인가     — 펌웨어가 형을 틀리게 넣을 수 있다.
 * 펌웨어 테이블은 커널이 통제하지 못하는 입력이라, 형과 개수를 믿지 않고
 * 전부 확인한 뒤에야 값을 꺼낸다. 확인 없이 integer.value 를 읽으면
 * 다른 형의 공용체 필드를 읽어 쓰레기가 나온다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 락 없음.
 * 에러 경로: 알 수 없는 판 번호는 pr_warn 으로 남기고 AE_ERROR.
 *   호출자(acpi_run_hpx)는 그 자리에서 전체 처리를 중단한다.
 *
 * 호출 체인:
 *   pci_acpi_program_hp_params() -> acpi_run_hpx() -> [decode_type0_hpx_record]
 */
static acpi_status decode_type0_hpx_record(union acpi_object *record,
					   struct hpx_type0 *hpx0)
{
	int i;	/* [한국어] 필드 순회 첨자 */
	union acpi_object *fields = record->package.elements;	/* [한국어] 패키지의 원소 배열. [0]=Type, [1]=Revision, [2..]=값 */
	u32 revision = fields[1].integer.value;	/* [한국어] 판 번호. 이 값을 꺼내려면 [1] 이 정수여야 하는데, 호출자인 acpi_run_hpx 가 이미 확인했다 */

	switch (revision) {	/* [한국어] 판 번호로 분기한다 */
	case 1:	/* [한국어] 현재 정의된 유일한 판 */
		if (record->package.count != 6)	/* [한국어] 개수가 정확히 6 이 아니면 다른 형식이다 */
			return AE_ERROR;	/* [한국어] 펌웨어가 개수를 틀리게 적었는데 그대로 읽으면 배열 밖을 읽는다 */
		for (i = 2; i < 6; i++)	/* [한국어] 값 네 개의 형을 확인한다. 첨자 2 부터인 것은 앞의 둘이 머리말이기 때문 */
			if (fields[i].type != ACPI_TYPE_INTEGER)	/* [한국어] 정수가 아닌 원소가 섞여 있으면 */
				return AE_ERROR;	/* [한국어] 공용체의 다른 필드를 읽어 쓰레기가 나오므로 거절한다 */
		hpx0->revision        = revision;	/* [한국어] 판 번호를 그대로 옮긴다 */
		hpx0->cache_line_size = fields[2].integer.value;	/* [한국어] [2] = Cache Line Size */
		hpx0->latency_timer   = fields[3].integer.value;	/* [한국어] [3] = Latency Timer */
		hpx0->enable_serr     = fields[4].integer.value;	/* [한국어] [4] = SERR# 활성 여부 */
		hpx0->enable_perr     = fields[5].integer.value;	/* [한국어] [5] = 패리티 응답 활성 여부 */
		break;	/* [한국어] 성공 경로로 빠진다 */
	default:	/* [한국어] 알 수 없는 판 */
		pr_warn("%s: Type 0 Revision %d record not supported\n",	/* [한국어] 어느 함수에서 어느 판을 못 읽었는지 남긴다 */
		       __func__, revision);	/* [한국어] __func__ 로 함수 이름을 넣는다 */
		return AE_ERROR;	/* [한국어] 호출자가 그 자리에서 _HPX 전체 처리를 중단한다 */
	}
	return AE_OK;	/* [한국어] 모든 값이 무사히 옮겨졌다 */
}

/* _HPX PCI-X Setting Record (Type 1) */
/* [한국어] _HPX Type 1 레코드 — PCI-X 전용 설정.
 * PCI-X 는 PCIe 이전의 고속 병렬 버스로, 오늘날 새 하드웨어에는 없다.
 * 커널은 이 레코드를 파싱은 하지만 적용하지 않는다
 * (program_hpx_type1() 이 경고만 찍는다). 파싱을 남겨 둔 이유는,
 * _HPX 패키지 안에서 Type 1 레코드를 건너뛰려면 그 길이를 알아야 하고,
 * 길이를 알려면 형식대로 읽어 봐야 하기 때문이다. */
struct hpx_type1 {
	/* [한국어] 이 레코드의 판 번호.
	 * 설정자: decode_type1_hpx_record().
	 * 읽는 자: 아무도 읽지 않는다 — 적용 함수가 값을 쓰지 않기 때문이다.
	 * 값 범위: 현재 1 만 정의되어 있다.
	 * 동기화: 스택 변수. */
	u32 revision;

	/* [한국어] PCI-X Command 레지스터의 Maximum Memory Read Byte Count.
	 * 한 번의 메모리 읽기 요청으로 가져올 수 있는 최대 바이트 수를 정했다.
	 * 설정자: decode_type1_hpx_record().
	 * 읽는 자: 없음(적용 미구현).
	 * 값 범위: 8비트.
	 * 동기화: 스택 변수. */
	u8  max_mem_read;

	/* [한국어] Average Maximum Outstanding Split Transactions.
	 * PCI-X 의 split transaction 은 "요청과 응답을 떼어 놓는" 방식인데,
	 * 동시에 몇 개까지 떠 있게 할지를 이 값이 정했다.
	 * 설정자: decode_type1_hpx_record().
	 * 읽는 자: 없음(적용 미구현).
	 * 값 범위: 8비트.
	 * 동기화: 스택 변수. */
	u8  avg_max_split;

	/* [한국어] Total Maximum Outstanding Split Transactions.
	 * 위 avg 가 함수 하나의 평균이라면 이쪽은 전체 상한이다.
	 * 설정자: decode_type1_hpx_record().
	 * 읽는 자: 없음(적용 미구현).
	 * 값 범위: 16비트.
	 * 동기화: 스택 변수. */
	u16 tot_max_split;
};

/* [한국어]
 * program_hpx_type1 - Type 1(PCI-X) 설정을 적용... 하지 않고 경고만 남긴다
 *
 * @dev:    대상 장치.
 * @hpx:    적용할 값. 실제로는 쓰이지 않는다.
 * @return: 없음.
 *
 * 커널은 PCI-X 설정을 _HPX 로 적용하는 기능을 구현하지 않았다. 그렇다고
 * 조용히 무시하지도 않는다 — 이 장치가 실제로 PCI-X capability 를 가졌고
 * 펌웨어가 Type 1 값을 준 경우에만 경고를 남긴다. 그러면 "적용되지 않은
 * 권장값이 있다" 는 사실이 로그에 남는다.
 *
 * pos 를 받아 놓고 값으로 쓰지 않는 것이 처음에는 이상해 보이는데,
 * pci_find_capability() 의 반환값을 "이 장치가 PCI-X 인가" 의 판정으로만
 * 쓰기 때문이다. PCIe 장치에는 이 capability 가 없으므로 경고도 나지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 락 없음.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_acpi_program_hp_params() -> acpi_run_hpx() -> [program_hpx_type1]
 */
static void program_hpx_type1(struct pci_dev *dev, struct hpx_type1 *hpx)
{
	int pos;	/* [한국어] capability 오프셋. 값이 아니라 존재 여부로만 쓴다 */

	if (!hpx)	/* [한국어] 펌웨어가 Type 1 값을 주지 않았으면 */
		return;	/* [한국어] 경고할 일도 없다 */

	pos = pci_find_capability(dev, PCI_CAP_ID_PCIX);	/* [한국어] 이 장치가 PCI-X capability 를 가졌는가 */
	if (!pos)	/* [한국어] PCIe 장치에는 없다 */
		return;	/* [한국어] 조용히 끝낸다 — 애초에 해당 없는 장치라 경고할 이유가 없다 */

	pci_warn(dev, "PCI-X settings not supported\n");	/* [한국어] PCI-X 장치인데 값이 있다 = 적용되지 않은 권장값이 있다는 뜻이라 반드시 알린다 */
}

/* [한국어]
 * decode_type1_hpx_record - _HPX 패키지의 Type 1 레코드를 구조체로 옮긴다
 *
 * @record: 한 레코드에 해당하는 ACPI 패키지 객체.
 * @hpx1:   [출력] 채울 구조체.
 * @return: AE_OK = 성공, AE_ERROR = 형식 불일치.
 *
 * Type 0 판과 같은 구조다. 다른 것은 기대 개수가 5(= [0]Type, [1]Revision,
 * [2..4] 값 셋)이고 값이 셋이라는 점뿐이다.
 *
 * 적용 함수가 값을 쓰지 않는데도 파싱하는 이유는 위 struct hpx_type1
 * 주석에 적었다 — 형식 검증을 통과시켜야 acpi_run_hpx() 가 다음 레코드로
 * 넘어갈 수 있고, 통과하지 못하면 그 뒤의 Type 2/3 레코드까지 버려진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 락 없음.
 * 에러 경로: 알 수 없는 판 번호는 pr_warn 후 AE_ERROR.
 *
 * 호출 체인:
 *   pci_acpi_program_hp_params() -> acpi_run_hpx() -> [decode_type1_hpx_record]
 */
static acpi_status decode_type1_hpx_record(union acpi_object *record,
					   struct hpx_type1 *hpx1)
{
	int i;	/* [한국어] 필드 순회 첨자 */
	union acpi_object *fields = record->package.elements;	/* [한국어] 패키지의 원소 배열 */
	u32 revision = fields[1].integer.value;	/* [한국어] 판 번호([1] 자리) */

	switch (revision) {	/* [한국어] 판 번호로 분기 */
	case 1:	/* [한국어] 현재 정의된 유일한 판 */
		if (record->package.count != 5)	/* [한국어] 기대 개수는 5(머리말 둘 + 값 셋) */
			return AE_ERROR;	/* [한국어] 형식 불일치 */
		for (i = 2; i < 5; i++)	/* [한국어] 값 셋의 형을 확인한다 */
			if (fields[i].type != ACPI_TYPE_INTEGER)	/* [한국어] 정수가 아니면 */
				return AE_ERROR;	/* [한국어] 거절한다 */
		hpx1->revision      = revision;	/* [한국어] 판 번호를 옮긴다 */
		hpx1->max_mem_read  = fields[2].integer.value;	/* [한국어] [2] = Maximum Memory Read Byte Count */
		hpx1->avg_max_split = fields[3].integer.value;	/* [한국어] [3] = Average Maximum Outstanding Split Transactions */
		hpx1->tot_max_split = fields[4].integer.value;	/* [한국어] [4] = Total Maximum Outstanding Split Transactions */
		break;	/* [한국어] 성공 경로로 */
	default:	/* [한국어] 알 수 없는 판 */
		pr_warn("%s: Type 1 Revision %d record not supported\n",	/* [한국어] 어느 판을 못 읽었는지 남긴다 */
		       __func__, revision);	/* [한국어] 함수 이름과 판 번호 */
		return AE_ERROR;	/* [한국어] 호출자가 처리를 중단한다 */
	}
	return AE_OK;	/* [한국어] 성공. 다만 이 값들을 실제로 쓰는 코드는 없다 */
}

/* _HPX PCI Express Setting Record (Type 2) */
/* [한국어] _HPX Type 2 레코드 — PCIe 의 오류 보고 설정.
 *
 * 모든 필드가 _and / _or 쌍으로 되어 있는 것이 이 레코드의 설계다.
 * 레지스터를 통째로 덮어쓰지 않고
 *     새값 = (현재값 & _and) | _or
 * 로 계산한다. 그러면 펌웨어가 "이 비트만 끄고 저 비트만 켜라, 나머지는
 * 그대로" 를 표현할 수 있다. 통째로 쓰는 방식이었다면 펌웨어가 OS 의
 * 다른 설정을 모르는 채로 전부 덮어써야 했을 것이다.
 *
 * 대상 레지스터는 AER(Advanced Error Reporting) 확장 capability 의 마스크와
 * severity, 그리고 PCIe capability 의 Device Control / Link Control 이다.
 * 모든 필드가 같은 규약을 따르므로, 아래 필드 주석은 각자가 어느 레지스터의
 * 어느 절반인지를 밝히는 데 집중한다. */
struct hpx_type2 {
	/* [한국어] 이 레코드의 판 번호.
	 * 설정자: decode_type2_hpx_record(). 읽는 자: program_hpx_type2() 가
	 *   1 보다 크면 경고 후 아무것도 하지 않는다(Type 0 과 달리 기본값으로
	 *   갈아타지 않는다 — 오류 보고 설정에는 안전한 기본값이 없기 때문).
	 * 값 범위: 현재 1 만 정의. 동기화: 스택 변수. */
	u32 revision;

	/* [한국어] Uncorrectable Error Mask 레지스터에 AND 할 값.
	 * 마스크가 1 이면 그 오류를 보고하지 않는다.
	 * 설정자: decode_type2_hpx_record(). 읽는 자: program_hpx_type2().
	 * 값 범위: 32비트 마스크. 동기화: 스택 변수. */
	u32 unc_err_mask_and;
	/* [한국어] 같은 레지스터에 OR 할 값(= 이 비트들의 오류를 새로 가린다).
	 * 설정자/읽는 자/동기화는 위와 같다. */
	u32 unc_err_mask_or;

	/* [한국어] Uncorrectable Error Severity 레지스터에 AND 할 값.
	 * 이 레지스터는 각 오류를 fatal 로 볼지 non-fatal 로 볼지 정한다.
	 * 설정자/읽는 자/동기화는 위와 같다. */
	u32 unc_err_sever_and;
	/* [한국어] 같은 레지스터에 OR 할 값(= 이 오류들을 fatal 로 올린다).
	 * 설정자/읽는 자/동기화는 위와 같다. */
	u32 unc_err_sever_or;

	/* [한국어] Correctable Error Mask 레지스터에 AND 할 값.
	 * 정정 가능한 오류는 링크가 스스로 회복하지만, 잦으면 신호 품질
	 * 문제의 신호라 보고 여부를 조절할 값어치가 있다.
	 * 설정자/읽는 자/동기화는 위와 같다. */
	u32 cor_err_mask_and;
	/* [한국어] 같은 레지스터에 OR 할 값.
	 * 설정자/읽는 자/동기화는 위와 같다. */
	u32 cor_err_mask_or;

	/* [한국어] Advanced Error Capabilities and Control 레지스터에 AND 할 값.
	 * ECRC 생성/검사 활성 비트가 이 안에 있다.
	 * 설정자/읽는 자/동기화는 위와 같다. */
	u32 adv_err_cap_and;
	/* [한국어] 같은 레지스터에 OR 할 값. program_hpx_type2() 가 이 결과에서
	 * "지원하지 않는 ECRC 를 켜려는" 비트를 다시 지운다.
	 * 설정자/읽는 자/동기화는 위와 같다. */
	u32 adv_err_cap_or;

	/* [한국어] PCIe capability 의 Device Control 에 AND 할 값(16비트).
	 * program_hpx_type2() 가 이 값을 손봐서, AER 관련 네 비트
	 * (CERE/NFERE/FERE/URRE) 외에는 건드리지 못하게 만든다.
	 * 설정자/읽는 자/동기화는 위와 같다. */
	u16 pci_exp_devctl_and;
	/* [한국어] 같은 레지스터에 OR 할 값. 역시 AER 네 비트로 제한된다.
	 * 설정자/읽는 자/동기화는 위와 같다. */
	u16 pci_exp_devctl_or;

	/* [한국어] PCIe capability 의 Link Control 에 AND 할 값(16비트).
	 * 커널은 이 값을 *적용하지 않는다*. 링크 설정은 ASPM 정책 엔진
	 * (pcie/aspm.c)이 통째로 관리하므로 펌웨어가 끼어들면 충돌한다.
	 * 대신 펌웨어가 무언가 바꾸려 했다는 사실만 pci_info 로 남긴다.
	 * 설정자: decode_type2_hpx_record(). 읽는 자: program_hpx_type2() 가
	 *   0xffff 가 아닌지만 확인해 로그를 남긴다.
	 * 값 범위: 16비트. 0xffff 가 "아무것도 지우지 않음".
	 * 동기화: 스택 변수. */
	u16 pci_exp_lnkctl_and;
	/* [한국어] 같은 레지스터에 OR 할 값. 역시 적용하지 않고 로그만 남긴다.
	 * 0 이 "아무것도 세우지 않음" 이라, and==0xffff && or==0 이면
	 * 펌웨어가 링크를 건드릴 뜻이 없다는 뜻이고 로그도 나지 않는다.
	 * 설정자/읽는 자/동기화는 위와 같다. */
	u16 pci_exp_lnkctl_or;

	/* [한국어] Secondary Uncorrectable Error Severity 에 AND 할 값.
	 * "Secondary" 는 PCIe-to-PCI 브리지의 하위(전통 PCI) 쪽 오류다.
	 * 커널은 아직 이 두 쌍을 적용하지 않는다 — program_hpx_type2() 끝의
	 * FIXME 주석이 그 사실을 명시한다.
	 * 설정자: decode_type2_hpx_record(). 읽는 자: 없음.
	 * 값 범위: 32비트. 동기화: 스택 변수. */
	u32 sec_unc_err_sever_and;
	/* [한국어] 같은 레지스터에 OR 할 값. 역시 미적용.
	 * 설정자/읽는 자/동기화는 위와 같다. */
	u32 sec_unc_err_sever_or;
	/* [한국어] Secondary Uncorrectable Error Mask 에 AND 할 값. 미적용.
	 * 설정자/읽는 자/동기화는 위와 같다. */
	u32 sec_unc_err_mask_and;
	/* [한국어] 같은 레지스터에 OR 할 값. 미적용.
	 * 설정자/읽는 자/동기화는 위와 같다. */
	u32 sec_unc_err_mask_or;
};

/* [한국어]
 * program_hpx_type2 - Type 2(PCIe 오류 보고) 설정을 조건부로 적용한다
 *
 * @dev:    설정할 장치.
 * @hpx:    적용할 값. NULL 이면 아무것도 하지 않는다.
 * @return: 없음.
 *
 * 이 함수에서 가장 중요한 것은 "언제 적용하지 않는가" 다.
 *
 *   PCIe 가 아니면                    -> 대상 레지스터가 없다.
 *   native hotplug 를 커널이 못 쥐었으면 -> 적용하지 않는다.
 *   AER 을 커널이 쥐었으면            -> 적용하지 않는다.
 *
 * 뒤 두 조건이 함께 걸린 이유가 이 코드의 핵심이다. 영어 주석대로 "OS 가
 * 핫플러그는 쥐었지만 AER 은 못 쥔" 경우에만 적용한다. AER 소유권이
 * 커널에 있으면 오류 마스크는 pcie/aer.c 가 관리하므로 펌웨어 값이
 * 끼어들면 충돌한다. 반대로 핫플러그를 커널이 다루지 않는다면 애초에
 * 이 장치의 설정을 커널이 손댈 자리가 아니다.
 *
 * DEVCTL 에 대해서는 한 겹 더 제한을 건다. 펌웨어가 무엇을 요청했든
 * AER 관련 네 비트(Correctable/Non-Fatal/Fatal Error Reporting Enable,
 * Unsupported Request Reporting Enable) 밖으로는 나가지 못하도록
 * and 에 ~PCI_EXP_AER_FLAGS 를 OR 하고 or 를 PCI_EXP_AER_FLAGS 로 AND 한다.
 * 나머지 DEVCTL 비트(MPS, MRRS, Relaxed Ordering 등)는 커널이 플랫폼
 * 전체와 맞춰 관리해야 하기 때문이다.
 *
 * LNKCTL 은 아예 적용하지 않고 로그만 남긴다 — 링크 설정은 ASPM 정책
 * 엔진(pcie/aspm.c)의 소관이다.
 *
 * ECRC 는 "지원하지 않으면 켜지 않는다" 는 보정을 따로 한다. 지원 비트
 * (GENC/CHKC)가 0 인데 활성 비트(GENE/CHKE)를 켜면 정의되지 않은 동작이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 장치 설정 중. 락 없음.
 * 에러 경로: AER capability 가 없으면 DEVCTL 까지만 하고 조용히 끝낸다.
 *
 * 호출 체인:
 *   pci_acpi_program_hp_params() -> acpi_run_hpx() -> [program_hpx_type2]
 *     -> pcie_capability_clear_and_set_word(), pci_find_ext_capability()
 */
static void program_hpx_type2(struct pci_dev *dev, struct hpx_type2 *hpx)
{
	int pos;	/* [한국어] AER 확장 capability 의 오프셋 */
	u32 reg32;	/* [한국어] 레지스터 값을 읽고 고칠 임시 변수. 네 번 재사용한다 */
	const struct pci_host_bridge *host;	/* [한국어] 소유권 비트를 읽을 호스트 브리지. 읽기만 하므로 const */

	if (!hpx)	/* [한국어] 펌웨어가 Type 2 값을 주지 않았으면 */
		return;	/* [한국어] 할 일이 없다 */

	if (!pci_is_pcie(dev))	/* [한국어] 전통 PCI 장치에는 PCIe capability 도 AER 도 없다 */
		return;	/* [한국어] 손댈 레지스터가 없다 */

	host = pci_find_host_bridge(dev->bus);	/* [한국어] 이 장치가 속한 호스트 브리지. 소유권 비트가 거기 있다 */

	/*
	 * Only do the _HPX Type 2 programming if OS owns PCIe native
	 * hotplug but not AER.
	 * [한국어] 두 조건이 함께 걸린 이유: AER 을 커널이 쥐었다면 오류
	 * 마스크는 pcie/aer.c 가 관리하므로 펌웨어 값이 끼어들면 충돌한다.
	 * 핫플러그를 커널이 다루지 않는다면 애초에 이 장치 설정을 커널이
	 * 손댈 자리가 아니다.
	 */
	if (!host->native_pcie_hotplug || host->native_aer)	/* [한국어] 핫플러그를 못 쥐었거나 AER 을 쥐었으면 */
		return;	/* [한국어] 적용하지 않는다 */

	if (hpx->revision > 1) {	/* [한국어] 해석할 수 없는 판이면 */
		pci_warn(dev, "PCIe settings rev %d not supported\n",	/* [한국어] 무엇을 못 읽었는지 남기고 */
			 hpx->revision);	/* [한국어] 실제 판 번호까지 찍는다 */
		return;	/* [한국어] Type 0 과 달리 기본값으로 갈아타지 않는다 — 오류 보고 설정에는 안전한 기본값이 없다 */
	}

	/*
	 * We only allow _HPX to program DEVCTL bits related to AER, namely
	 * PCI_EXP_DEVCTL_CERE, PCI_EXP_DEVCTL_NFERE, PCI_EXP_DEVCTL_FERE,
	 * and PCI_EXP_DEVCTL_URRE.
	 *
	 * The rest of DEVCTL is managed by the OS to make sure it's
	 * consistent with the rest of the platform.
	 * [한국어] 아래 두 줄이 그 제한을 마스크에 심는다. and 에
	 * ~PCI_EXP_AER_FLAGS 를 OR 하면 그 밖의 비트는 절대 지워지지 않고,
	 * or 를 PCI_EXP_AER_FLAGS 로 AND 하면 그 밖의 비트는 절대 세워지지 않는다.
	 */
	hpx->pci_exp_devctl_and |= ~PCI_EXP_AER_FLAGS;	/* [한국어] AER 네 비트 밖은 절대 지워지지 않게 만든다 */
	hpx->pci_exp_devctl_or &= PCI_EXP_AER_FLAGS;	/* [한국어] AER 네 비트 밖은 절대 세워지지 않게 만든다 */

	/* Initialize Device Control Register */
	pcie_capability_clear_and_set_word(dev, PCI_EXP_DEVCTL,	/* [한국어] Device Control 에 적용한다 */
			~hpx->pci_exp_devctl_and, hpx->pci_exp_devctl_or);	/* [한국어] 이 헬퍼는 (지울 마스크, 세울 값) 을 받으므로 and 를 부정해 넘긴다 — and 에서 0 인 비트가 곧 지울 비트다 */

	/* Log if _HPX attempts to modify Link Control Register */
	if (pcie_cap_has_lnkctl(dev)) {	/* [한국어] LNKCTL 이 있는 장치인가. Root Complex 통합 엔드포인트처럼 링크가 없는 종류에는 이 레지스터가 없다 */
		if (hpx->pci_exp_lnkctl_and != 0xffff ||	/* [한국어] 0xffff 는 "아무것도 지우지 않음" 이므로 그와 다르거나 */
		    hpx->pci_exp_lnkctl_or != 0)	/* [한국어] 0 은 "아무것도 세우지 않음" 이므로 그와 다르면 — 펌웨어가 링크를 건드릴 뜻이 있다 */
			pci_info(dev, "_HPX attempts Link Control setting (AND %#06x OR %#06x)\n",	/* [한국어] 적용하지 않고 로그만 남긴다. 링크 설정은 pcie/aspm.c 의 소관이라 펌웨어가 끼어들면 충돌한다 */
				 hpx->pci_exp_lnkctl_and,	/* [한국어] 펌웨어가 요청한 AND 값 */
				 hpx->pci_exp_lnkctl_or);	/* [한국어] 펌웨어가 요청한 OR 값 */
	}

	/* Find Advanced Error Reporting Enhanced Capability */
	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ERR);	/* [한국어] AER 확장 capability 를 찾는다 */
	if (!pos)	/* [한국어] AER 이 없는 장치면 */
		return;	/* [한국어] DEVCTL 까지만 하고 끝낸다 */

	/* Initialize Uncorrectable Error Mask Register */
	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_MASK, &reg32);	/* [한국어] 현재 마스크를 읽는다 */
	reg32 = (reg32 & hpx->unc_err_mask_and) | hpx->unc_err_mask_or;	/* [한국어] and/or 규약대로 계산한다 — 통째로 덮지 않으므로 펌웨어가 일부 비트만 지정할 수 있다 */
	pci_write_config_dword(dev, pos + PCI_ERR_UNCOR_MASK, reg32);	/* [한국어] 되쓴다 */

	/* Initialize Uncorrectable Error Severity Register */
	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_SEVER, &reg32);	/* [한국어] Uncorrectable Error Severity. 각 오류를 fatal 로 볼지 non-fatal 로 볼지 정한다 */
	reg32 = (reg32 & hpx->unc_err_sever_and) | hpx->unc_err_sever_or;	/* [한국어] 같은 and/or 규약 */
	pci_write_config_dword(dev, pos + PCI_ERR_UNCOR_SEVER, reg32);	/* [한국어] 되쓴다 */

	/* Initialize Correctable Error Mask Register */
	pci_read_config_dword(dev, pos + PCI_ERR_COR_MASK, &reg32);	/* [한국어] Correctable Error Mask. 정정된 오류를 보고할지 정한다 */
	reg32 = (reg32 & hpx->cor_err_mask_and) | hpx->cor_err_mask_or;	/* [한국어] 같은 and/or 규약 */
	pci_write_config_dword(dev, pos + PCI_ERR_COR_MASK, reg32);	/* [한국어] 되쓴다 */

	/* Initialize Advanced Error Capabilities and Control Register */
	pci_read_config_dword(dev, pos + PCI_ERR_CAP, &reg32);	/* [한국어] Advanced Error Capabilities and Control. ECRC 활성 비트가 여기 있다 */
	reg32 = (reg32 & hpx->adv_err_cap_and) | hpx->adv_err_cap_or;	/* [한국어] 같은 and/or 규약. 다만 곧바로 쓰지 않고 아래에서 보정한다 */

	/* Don't enable ECRC generation or checking if unsupported */
	if (!(reg32 & PCI_ERR_CAP_ECRC_GENC))	/* [한국어] ECRC 생성을 지원하지 않는데(GENC 가 0) */
		reg32 &= ~PCI_ERR_CAP_ECRC_GENE;	/* [한국어] 생성 활성 비트(GENE)를 켜면 정의되지 않은 동작이라 지운다 */
	if (!(reg32 & PCI_ERR_CAP_ECRC_CHKC))	/* [한국어] ECRC 검사를 지원하지 않는데(CHKC 가 0) */
		reg32 &= ~PCI_ERR_CAP_ECRC_CHKE;	/* [한국어] 검사 활성 비트(CHKE)를 지운다 */
	pci_write_config_dword(dev, pos + PCI_ERR_CAP, reg32);	/* [한국어] 보정을 마친 값을 쓴다 */

	/*
	 * FIXME: The following two registers are not supported yet.
	 *
	 *   o Secondary Uncorrectable Error Severity Register
	 *   o Secondary Uncorrectable Error Mask Register
	 * [한국어] 두 레지스터는 PCIe-to-PCI 브리지의 하위(전통 PCI) 쪽
	 * 오류를 다룬다. 구조체에는 값이 파싱되어 들어와 있지만 쓰는 코드가
	 * 없다 — 상류가 남긴 FIXME 그대로다.
	 */
}

/* [한국어]
 * decode_type2_hpx_record - _HPX 패키지의 Type 2 레코드를 구조체로 옮긴다
 *
 * @record: 한 레코드에 해당하는 ACPI 패키지 객체.
 * @hpx2:   [출력] 채울 구조체.
 * @return: AE_OK = 성공, AE_ERROR = 형식 불일치.
 *
 * Type 0/1 과 같은 틀이지만 필드가 16개라 기대 개수가 18(= Type, Revision,
 * 값 16개)이다. 값을 꺼내는 순서가 곧 규격이 정한 레코드 배치 순서이며,
 * 순서를 하나라도 어긋나게 읽으면 엉뚱한 레지스터에 엉뚱한 마스크가
 * 들어간다. 그래서 구조체 필드 선언 순서와 이 대입 순서가 정확히 같다.
 *
 * 16개 전부가 INTEGER 인지 먼저 확인하는 것도 앞의 두 함수와 같은 이유다 —
 * 펌웨어 테이블은 신뢰할 수 없는 입력이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 락 없음.
 * 에러 경로: 알 수 없는 판 번호는 pr_warn 후 AE_ERROR.
 *
 * 호출 체인:
 *   pci_acpi_program_hp_params() -> acpi_run_hpx() -> [decode_type2_hpx_record]
 */
static acpi_status decode_type2_hpx_record(union acpi_object *record,
					   struct hpx_type2 *hpx2)
{
	int i;	/* [한국어] 필드 순회 첨자 */
	union acpi_object *fields = record->package.elements;	/* [한국어] 패키지의 원소 배열 */
	u32 revision = fields[1].integer.value;	/* [한국어] 판 번호([1] 자리) */

	switch (revision) {	/* [한국어] 판 번호로 분기 */
	case 1:	/* [한국어] 현재 정의된 유일한 판 */
		if (record->package.count != 18)	/* [한국어] 기대 개수는 18(머리말 둘 + 값 16) */
			return AE_ERROR;	/* [한국어] 형식 불일치 */
		for (i = 2; i < 18; i++)	/* [한국어] 값 16개의 형을 한 번에 확인한다 */
			if (fields[i].type != ACPI_TYPE_INTEGER)	/* [한국어] 정수가 아니면 */
				return AE_ERROR;	/* [한국어] 거절한다 */
		hpx2->revision      = revision;	/* [한국어] 판 번호를 옮긴다 */
		hpx2->unc_err_mask_and      = fields[2].integer.value;	/* [한국어] [2] Uncorrectable Error Mask 의 AND */
		hpx2->unc_err_mask_or       = fields[3].integer.value;	/* [한국어] [3] 같은 레지스터의 OR */
		hpx2->unc_err_sever_and     = fields[4].integer.value;	/* [한국어] [4] Uncorrectable Error Severity 의 AND */
		hpx2->unc_err_sever_or      = fields[5].integer.value;	/* [한국어] [5] 같은 레지스터의 OR */
		hpx2->cor_err_mask_and      = fields[6].integer.value;	/* [한국어] [6] Correctable Error Mask 의 AND */
		hpx2->cor_err_mask_or       = fields[7].integer.value;	/* [한국어] [7] 같은 레지스터의 OR */
		hpx2->adv_err_cap_and       = fields[8].integer.value;	/* [한국어] [8] Advanced Error Capabilities and Control 의 AND */
		hpx2->adv_err_cap_or        = fields[9].integer.value;	/* [한국어] [9] 같은 레지스터의 OR */
		hpx2->pci_exp_devctl_and    = fields[10].integer.value;	/* [한국어] [10] Device Control 의 AND */
		hpx2->pci_exp_devctl_or     = fields[11].integer.value;	/* [한국어] [11] 같은 레지스터의 OR */
		hpx2->pci_exp_lnkctl_and    = fields[12].integer.value;	/* [한국어] [12] Link Control 의 AND(적용하지 않고 로그만) */
		hpx2->pci_exp_lnkctl_or     = fields[13].integer.value;	/* [한국어] [13] 같은 레지스터의 OR */
		hpx2->sec_unc_err_sever_and = fields[14].integer.value;	/* [한국어] [14] Secondary Uncorrectable Error Severity 의 AND(미적용) */
		hpx2->sec_unc_err_sever_or  = fields[15].integer.value;	/* [한국어] [15] 같은 레지스터의 OR */
		hpx2->sec_unc_err_mask_and  = fields[16].integer.value;	/* [한국어] [16] Secondary Uncorrectable Error Mask 의 AND(미적용) */
		hpx2->sec_unc_err_mask_or   = fields[17].integer.value;	/* [한국어] [17] 같은 레지스터의 OR. 이 순서가 곧 규격의 레코드 배치다 */
		break;	/* [한국어] 성공 경로로 */
	default:	/* [한국어] 알 수 없는 판 */
		pr_warn("%s: Type 2 Revision %d record not supported\n",	/* [한국어] 어느 판을 못 읽었는지 남긴다 */
		       __func__, revision);	/* [한국어] 함수 이름과 판 번호 */
		return AE_ERROR;	/* [한국어] 호출자가 처리를 중단한다 */
	}
	return AE_OK;	/* [한국어] 16개 값이 모두 무사히 옮겨졌다 */
}

/* _HPX PCI Express Setting Record (Type 3) */
/* [한국어] _HPX Type 3 레코드 — 임의의 레지스터를 조건부로 고치는 범용 패치.
 *
 * Type 0/1/2 는 "어느 레지스터를 고칠지" 가 규격에 못 박혀 있었다.
 * Type 3 은 그 자리를 데이터로 뺐다. 한 레코드가 곧
 *   "장치 종류가 X 이고 함수 종류가 Y 이고, config space 의 Z 위치에서
 *    match_offset 을 읽어 match_mask_and 로 가린 값이 match_value 면,
 *    reg_offset 자리를 (현재값 & reg_mask_and) | reg_mask_or 로 바꿔라"
 * 라는 하나의 규칙이다.
 *
 * 왜 이런 것이 필요한가. 새 PCIe capability 나 벤더 고유 레지스터가 나올
 * 때마다 커널에 코드를 넣고 배포하려면 시간이 걸린다. Type 3 은 그 사이를
 * 펌웨어 테이블만으로 메우게 해 준다. 대가는 커널이 무엇을 고치는지 미리
 * 알 수 없다는 것이라, program_hpx_type3_register() 가 적용 결과를
 * pci_dbg 로 남긴다. */
struct hpx_type3 {
	/* [한국어] 적용 대상 장치 종류의 비트마스크(enum hpx_type3_dev_type).
	 * 설정자: parse_hpx3_register() 가 패키지 [0] 에서 읽는다.
	 * 읽는 자: program_hpx_type3_register() 가 hpx3_device_type(dev) 와
	 *   AND 해 0 이면 이 규칙을 건너뛴다.
	 * 값 범위: HPX_TYPE_* 조합. 동기화: 스택 변수. */
	u16 device_type;

	/* [한국어] 적용 대상 함수 종류의 비트마스크(enum hpx_type3_fn_type).
	 * 일반 함수 / SR-IOV PF / VF 를 구분한다.
	 * 설정자: parse_hpx3_register()(패키지 [1]).
	 * 읽는 자: program_hpx_type3_register().
	 * 값 범위: HPX_FN_* 조합. 동기화: 스택 변수. */
	u16 function_type;

	/* [한국어] 어느 config space 영역을 기준으로 오프셋을 셀 것인가
	 * (enum hpx_type3_cfg_loc).
	 * 설정자: parse_hpx3_register()(패키지 [2]).
	 * 읽는 자: program_hpx_type3_register() 의 switch.
	 * 값 범위: HPX_CFG_PCICFG / _PCIE_CAP / _PCIE_CAP_EXT 만 지원한다.
	 *   VEND_CAP 과 DVSEC 는 미구현이라 경고 후 건너뛴다.
	 * 동기화: 스택 변수. */
	u16 config_space_location;

	/* [한국어] 찾을 capability 의 ID.
	 * config_space_location 이 PCIE_CAP 이면 표준 capability ID,
	 * PCIE_CAP_EXT 면 확장 capability ID 로 해석된다.
	 * 설정자: parse_hpx3_register()(패키지 [3]).
	 * 읽는 자: program_hpx_type3_register() 가 pci_find_capability() 또는
	 *   pci_find_ext_capability() 에 넘긴다.
	 * 값 범위: 규격이 정한 capability ID. 동기화: 스택 변수. */
	u16 pci_exp_cap_id;

	/* [한국어] 요구하는 capability 판 번호(하위 4비트) + 비교 방식(비트 4).
	 * 비트 4 가 서 있으면 "이 판 이상" 이고, 없으면 "정확히 이 판" 이다.
	 * 그 해석을 hpx3_cap_ver_matches() 가 한다.
	 * 설정자: parse_hpx3_register()(패키지 [4]).
	 * 읽는 자: program_hpx_type3_register()(확장 capability 경로에서만).
	 * 값 범위: 하위 4비트 판 번호 + BIT(4). 동기화: 스택 변수. */
	u16 pci_exp_cap_ver;

	/* [한국어] DVSEC 을 찾을 때 쓸 벤더 ID.
	 * 설정자: parse_hpx3_register()(패키지 [5]).
	 * 읽는 자: 없음 — DVSEC 경로가 미구현이다.
	 * 값 범위: PCI 벤더 ID. 동기화: 스택 변수. */
	u16 pci_exp_vendor_id;

	/* [한국어] DVSEC 안에서 찾을 하위 ID.
	 * 설정자: parse_hpx3_register()(패키지 [6]).
	 * 읽는 자: 없음(미구현). 값 범위: 16비트. 동기화: 스택 변수. */
	u16 dvsec_id;

	/* [한국어] 요구하는 DVSEC 판 번호.
	 * 설정자: parse_hpx3_register()(패키지 [7]).
	 * 읽는 자: 없음(미구현). 값 범위: 16비트. 동기화: 스택 변수. */
	u16 dvsec_rev;

	/* [한국어] 조건을 검사할 레지스터의 오프셋(기준점으로부터).
	 * 설정자: parse_hpx3_register()(패키지 [8]).
	 * 읽는 자: program_hpx_type3_register() 가 pos + 이 값에서 dword 를 읽는다.
	 * 값 범위: 16비트 오프셋. 동기화: 스택 변수. */
	u16 match_offset;

	/* [한국어] 조건 검사에서 볼 비트만 남기는 마스크.
	 * 설정자: parse_hpx3_register()(패키지 [9]).
	 * 읽는 자: program_hpx_type3_register().
	 * 값 범위: 32비트 마스크. 동기화: 스택 변수. */
	u32 match_mask_and;

	/* [한국어] 위 마스크를 씌운 값이 이것과 같아야 규칙이 적용된다.
	 * 설정자: parse_hpx3_register()(패키지 [10]).
	 * 읽는 자: program_hpx_type3_register().
	 * 값 범위: 32비트. 동기화: 스택 변수. */
	u32 match_value;

	/* [한국어] 실제로 고칠 레지스터의 오프셋(기준점으로부터).
	 * match_offset 과 다를 수 있다 — 한 레지스터를 보고 다른 레지스터를
	 * 고치는 규칙을 쓸 수 있다는 뜻이다.
	 * 설정자: parse_hpx3_register()(패키지 [11]).
	 * 읽는 자: program_hpx_type3_register(). 값 범위: 16비트 오프셋.
	 * 동기화: 스택 변수. */
	u16 reg_offset;

	/* [한국어] 고칠 레지스터에 AND 할 값(= 이 비트들만 남긴다).
	 * 설정자: parse_hpx3_register()(패키지 [12]).
	 * 읽는 자: program_hpx_type3_register(). 값 범위: 32비트 마스크.
	 * 동기화: 스택 변수. */
	u32 reg_mask_and;

	/* [한국어] 고칠 레지스터에 OR 할 값(= 이 비트들을 세운다).
	 * 설정자: parse_hpx3_register()(패키지 [13]).
	 * 읽는 자: program_hpx_type3_register(). 값 범위: 32비트.
	 * 동기화: 스택 변수. */
	u32 reg_mask_or;
};

/* [한국어] Type 3 의 device_type 필드에 쓰이는 비트값.
 * PCIe capability 의 Device/Port Type 필드(4비트 값)를 비트마스크로 펼친
 * 것이다. 값이 아니라 비트인 이유는 한 규칙이 여러 종류에 동시에 적용될
 * 수 있어야 하기 때문이다 — 예를 들어 "모든 엔드포인트" 는
 * ENDPOINT | LEG_END | RC_END 세 비트를 함께 세운다.
 * hpx3_device_type() 이 pci_pcie_type() 의 값을 이 비트로 옮겨 준다. */
enum hpx_type3_dev_type {
	HPX_TYPE_ENDPOINT	= BIT(0),	/* [한국어] 일반 PCIe 엔드포인트 */
	HPX_TYPE_LEG_END	= BIT(1),	/* [한국어] Legacy 엔드포인트 — I/O 공간을 쓰는 옛 방식 */
	HPX_TYPE_RC_END		= BIT(2),	/* [한국어] Root Complex 통합 엔드포인트(칩셋 내장 장치) */
	HPX_TYPE_RC_EC		= BIT(3),	/* [한국어] Root Complex Event Collector — RC 내장 장치의 오류를 모은다 */
	HPX_TYPE_ROOT_PORT	= BIT(4),	/* [한국어] Root Port */
	HPX_TYPE_UPSTREAM	= BIT(5),	/* [한국어] 스위치의 상류 포트 */
	HPX_TYPE_DOWNSTREAM	= BIT(6),	/* [한국어] 스위치의 하류 포트 */
	HPX_TYPE_PCI_BRIDGE	= BIT(7),	/* [한국어] PCIe -> PCI/PCI-X 브리지 */
	HPX_TYPE_PCIE_BRIDGE	= BIT(8),	/* [한국어] PCI/PCI-X -> PCIe 브리지(반대 방향) */
};

/* [한국어]
 * hpx3_device_type - PCIe Device/Port Type 값을 Type 3 의 비트마스크로 옮긴다
 *
 * @dev:    대상 장치.
 * @return: 대응하는 HPX_TYPE_* 비트 하나, 알 수 없는 종류면 0.
 *
 * PCIe capability 의 Device/Port Type 은 0~9 같은 작은 정수인데, _HPX
 * Type 3 의 device_type 은 비트마스크다. 그 사이를 표 하나로 옮긴다.
 *
 * 표를 static const 지역 배열로 둔 것이 눈에 띈다. 함수 안에 두어 쓰임을
 * 좁히면서도 static 이라 호출마다 다시 만들지 않는다.
 *
 * 범위 검사를 반드시 해야 한다. pci_pcie_type() 은 하드웨어가 광고한
 * 값을 그대로 돌려주므로, 규격에 없는 값을 광고하는 장치가 있으면
 * 배열 밖을 읽게 된다. 그래서 ARRAY_SIZE 로 먼저 막는다.
 *
 * 표에 없는 자리(예: 예약된 종류 번호)는 지정 초기화가 채우지 않아 0 이
 * 남고, 그러면 어떤 규칙과도 AND 되지 않아 자연스럽게 건너뛰어진다.
 *
 * 실행 컨텍스트: 순수 계산. 락 없음.
 *
 * 호출 체인:
 *   program_hpx_type3_register() -> [hpx3_device_type] -> pci_pcie_type()
 */
static u16 hpx3_device_type(struct pci_dev *dev)
{
	u16 pcie_type = pci_pcie_type(dev);	/* [한국어] PCIe capability 의 Device/Port Type 필드 값(작은 정수) */
	static const int pcie_to_hpx3_type[] = {	/* [한국어] 그 정수를 비트마스크로 옮기는 표. static const 라 호출마다 다시 만들지 않는다 */
		[PCI_EXP_TYPE_ENDPOINT]    = HPX_TYPE_ENDPOINT,
		[PCI_EXP_TYPE_LEG_END]     = HPX_TYPE_LEG_END,
		[PCI_EXP_TYPE_RC_END]      = HPX_TYPE_RC_END,
		[PCI_EXP_TYPE_RC_EC]       = HPX_TYPE_RC_EC,
		[PCI_EXP_TYPE_ROOT_PORT]   = HPX_TYPE_ROOT_PORT,
		[PCI_EXP_TYPE_UPSTREAM]    = HPX_TYPE_UPSTREAM,
		[PCI_EXP_TYPE_DOWNSTREAM]  = HPX_TYPE_DOWNSTREAM,
		[PCI_EXP_TYPE_PCI_BRIDGE]  = HPX_TYPE_PCI_BRIDGE,
		[PCI_EXP_TYPE_PCIE_BRIDGE] = HPX_TYPE_PCIE_BRIDGE,
	};

	if (pcie_type >= ARRAY_SIZE(pcie_to_hpx3_type))	/* [한국어] 하드웨어가 규격에 없는 종류 번호를 광고할 수 있으므로 반드시 범위를 막는다. 없으면 배열 밖을 읽는다 */
		return 0;	/* [한국어] 어떤 규칙과도 AND 되지 않아 자연스럽게 건너뛰어진다 */

	return pcie_to_hpx3_type[pcie_type];	/* [한국어] 표에 지정되지 않은 자리는 0 이 남아 같은 결과가 된다 */
}

/* [한국어] Type 3 의 function_type 필드에 쓰이는 비트값.
 * SR-IOV(Single Root I/O Virtualization)는 물리 함수(PF) 하나가 가상
 * 함수(VF) 여럿을 만들어 내는 기능이다. PF 와 VF 는 config space 구성이
 * 달라 같은 패치를 적용하면 안 되는 경우가 있어, 규칙마다 대상을 고를 수
 * 있게 했다. hpx3_function_type() 이 장치를 셋 중 하나로 분류한다. */
enum hpx_type3_fn_type {
	HPX_FN_NORMAL		= BIT(0),	/* [한국어] SR-IOV 와 무관한 보통 함수 */
	HPX_FN_SRIOV_PHYS	= BIT(1),	/* [한국어] VF 를 만들어 내는 물리 함수(PF) */
	HPX_FN_SRIOV_VIRT	= BIT(2),	/* [한국어] PF 가 만들어 낸 가상 함수(VF) */
};

/* [한국어]
 * hpx3_function_type - 이 함수가 보통 함수인지 PF 인지 VF 인지 분류한다
 *
 * @dev:    대상 장치.
 * @return: HPX_FN_* 비트 하나.
 *
 * 판정 순서가 중요하다. VF 인지 먼저 보고, 아니면 SR-IOV capability 가
 * 있는지 본다. 순서를 뒤집으면 안 된다 — VF 도 SR-IOV 확장 capability 를
 * 가질 수 있는 구성이 있어, PF 로 오분류될 여지가 생긴다.
 *
 * dev->is_virtfn 은 커널이 VF 를 만들면서 세워 두는 플래그라 하드웨어
 * 레지스터를 읽지 않아도 되는 확실한 판정이다.
 *
 * 반환형이 u8 인데 enum 값은 BIT(0..2) 라 세 값 모두 8비트에 들어간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. pci_find_ext_capability() 가 config
 *   space 를 읽으므로 잠들 수 있다. 락 없음.
 *
 * 호출 체인:
 *   program_hpx_type3_register() -> [hpx3_function_type]
 *     -> pci_find_ext_capability()
 */
static u8 hpx3_function_type(struct pci_dev *dev)
{
	if (dev->is_virtfn)
		return HPX_FN_SRIOV_VIRT;
	else if (pci_find_ext_capability(dev, PCI_EXT_CAP_ID_SRIOV) > 0)
		return HPX_FN_SRIOV_PHYS;	/* [한국어] VF 를 만들어 낼 수 있는 물리 함수. VF 판정을 먼저 한 뒤에 보는 순서가 중요하다 — VF 도 SR-IOV 확장 capability 를 가질 수 있는 구성이 있다 */
	else
		return HPX_FN_NORMAL;
}

/* [한국어]
 * hpx3_cap_ver_matches - 레코드가 요구하는 capability 판 번호와 맞는지 본다
 *
 * @pcie_cap_id: 실제 장치에서 읽은 capability 판 번호.
 * @hpx3_cap_id: 레코드가 지정한 값(하위 4비트 = 판 번호, 비트 4 = 비교 방식).
 * @return:      true = 이 규칙을 적용해도 된다.
 *
 * 인자 이름이 헷갈린다. 둘 다 "cap_id" 지만 실제로는 판 번호(version)다.
 * 상류 코드의 이름을 그대로 두었으므로 여기서 뜻을 밝혀 둔다.
 *
 * 비교 방식이 두 가지인 이유는 명확하다. 어떤 패치는 특정 판에서만
 * 유효하고(정확히 일치), 어떤 패치는 "이 판 이후로는 계속 유효" 하다
 * (이상). 후자를 표현하려고 비트 4 를 방식 플래그로 썼다.
 *
 *   hpx3_cap_id & BIT(4) 가 서 있으면 -> cap_ver >= pcie_cap_id 이면 참
 *   서 있지 않으면                     -> cap_ver == pcie_cap_id 이면 참
 *
 * 부등호의 방향을 눈여겨볼 만하다. cap_ver 는 *레코드가 요구하는* 판이고
 * pcie_cap_id 는 *장치의* 판이므로, `cap_ver >= pcie_cap_id` 는
 * "요구 판이 장치 판보다 높거나 같으면" 이라는 뜻이 된다.
 * 상류 코드가 그렇게 쓰여 있으므로 그대로 두고, 여기서는 코드가 실제로
 * 무엇을 계산하는지만 적는다.
 *
 * 실행 컨텍스트: 순수 계산. 락 없음.
 *
 * 호출 체인:
 *   program_hpx_type3_register() -> [hpx3_cap_ver_matches]
 */
static bool hpx3_cap_ver_matches(u8 pcie_cap_id, u8 hpx3_cap_id)
{
	u8 cap_ver = hpx3_cap_id & 0xf;	/* [한국어] 하위 4비트가 요구 판 번호 */

	if ((hpx3_cap_id & BIT(4)) && cap_ver >= pcie_cap_id)	/* [한국어] 비트 4 는 비교 방식 플래그다. 서 있으면 "이상" 비교로 간다 */
		return true;	/* [한국어] 조건을 만족했다 */
	else if (cap_ver == pcie_cap_id)	/* [한국어] 플래그가 없으면 정확히 일치해야 한다 */
		return true;	/* [한국어] 조건을 만족했다 */

	return false;	/* [한국어] 어느 쪽도 아니면 이 규칙은 이 장치에 해당하지 않는다 */
}

/* [한국어] Type 3 의 config_space_location 값 — 오프셋을 어디서부터 셀 것인가.
 * 같은 오프셋 숫자라도 기준점이 다르면 전혀 다른 레지스터가 되므로,
 * 규칙마다 기준을 명시해야 한다.
 * 커널은 앞의 셋만 구현했고, 나머지는 program_hpx_type3_register() 가
 * 경고를 남기고 건너뛴다. */
enum hpx_type3_cfg_loc {
	HPX_CFG_PCICFG		= 0,	/* [한국어] config space 맨 앞(오프셋 0)부터. 표준 헤더 영역 */
	HPX_CFG_PCIE_CAP	= 1,	/* [한국어] 표준 capability 사슬에서 찾은 capability 의 시작부터 */
	HPX_CFG_PCIE_CAP_EXT	= 2,	/* [한국어] 확장 capability(0x100 이후) 사슬에서 찾은 것의 시작부터 */
	HPX_CFG_VEND_CAP	= 3,	/* [한국어] 벤더 고유 capability 기준. 커널 미구현 */
	HPX_CFG_DVSEC		= 4,	/* [한국어] DVSEC(Designated Vendor-Specific Extended Cap) 기준. 커널 미구현 */
	HPX_CFG_MAX,			/* [한국어] 개수 표시용 보초값. 실제 위치가 아니다 */
};

/* [한국어]
 * program_hpx_type3_register - Type 3 규칙 하나를 조건 검사 후 적용한다
 *
 * @dev:    대상 장치.
 * @reg:    적용할 규칙 하나.
 * @return: 없음. 조건에 맞지 않으면 조용히 돌아간다.
 *
 * 규칙 하나가 실제로 레지스터를 고치기까지 통과해야 할 관문이 다섯이다.
 *
 *   1. 장치 종류가 맞는가        hpx3_device_type(dev) & reg->device_type
 *   2. 함수 종류가 맞는가        hpx3_function_type(dev) & reg->function_type
 *   3. 기준점을 찾을 수 있는가   config_space_location 에 따라 pos 를 정한다
 *   4. (확장 capability 라면) 판 번호가 맞는가
 *   5. match 조건이 맞는가       (읽은 값 & match_mask_and) == match_value
 *
 * 그러고 나서야 reg_offset 자리를 (현재값 & reg_mask_and) | reg_mask_or 로
 * 바꾼다. 값이 이미 같으면 쓰지 않는다 — config 쓰기는 비용이 있고,
 * 아래 pci_dbg 로그도 실제 변화가 있을 때만 남기고 싶기 때문이다.
 *
 * pos 를 정하는 switch 에서 HPX_CFG_PCICFG 만 pos = 0 이다. config space
 * 맨 앞을 기준으로 삼는다는 뜻이라, 이 경우 reg_offset 이 곧 절대
 * 오프셋이 된다.
 *
 * VEND_CAP 과 DVSEC 은 미구현이라 default 와 함께 묶여 경고를 남긴다.
 * 조용히 무시하지 않는 이유는, 펌웨어가 기대한 설정이 적용되지 않았다는
 * 사실이 로그에 남아야 하기 때문이다.
 *
 * 적용에 성공하면 pci_dbg 로 오프셋과 이전/이후 값을 남긴다. Type 3 은
 * 커널이 내용을 미리 알 수 없는 패치라, 이 로그가 사후 추적의 유일한
 * 단서가 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 장치 설정 중. 락 없음.
 * 에러 경로: 모든 불일치는 조용한 return 이다. 로그를 남기는 것은
 *   미구현 위치 하나뿐이다.
 *
 * 호출 체인:
 *   acpi_run_hpx() -> program_type3_hpx_record() -> program_hpx_type3()
 *     -> [program_hpx_type3_register]
 *        -> hpx3_device_type(), hpx3_function_type(),
 *           pci_find_capability(), pci_find_ext_capability(),
 *           hpx3_cap_ver_matches()
 */
static void program_hpx_type3_register(struct pci_dev *dev,
				       const struct hpx_type3 *reg)
{
	u32 match_reg, write_reg, header, orig_value;	/* [한국어] match_reg = 조건 검사용, write_reg = 고칠 값, header = 확장 capability 머리말, orig_value = 변화 여부 비교용 */
	u16 pos;	/* [한국어] 기준점 오프셋 */

	if (!(hpx3_device_type(dev) & reg->device_type))	/* [한국어] 장치 종류가 규칙의 대상 집합에 들어가는가 */
		return;	/* [한국어] 아니면 이 규칙은 해당 없음 */

	if (!(hpx3_function_type(dev) & reg->function_type))	/* [한국어] 함수 종류(보통/PF/VF)가 대상인가 */
		return;	/* [한국어] 아니면 해당 없음 */

	switch (reg->config_space_location) {	/* [한국어] 오프셋을 어디서부터 셀 것인가 */
	case HPX_CFG_PCICFG:	/* [한국어] config space 맨 앞 기준 */
		pos = 0;	/* [한국어] 그러면 reg_offset 이 곧 절대 오프셋이 된다 */
		break;	/* [한국어] 다음 단계로 */
	case HPX_CFG_PCIE_CAP:	/* [한국어] 표준 capability 사슬에서 찾은 것 기준 */
		pos = pci_find_capability(dev, reg->pci_exp_cap_id);	/* [한국어] 그 capability 의 시작 오프셋을 찾는다 */
		if (pos == 0)	/* [한국어] 이 장치에 그 capability 가 없으면 */
			return;	/* [한국어] 규칙을 적용할 자리가 없다 */

		break;	/* [한국어] 다음 단계로 */
	case HPX_CFG_PCIE_CAP_EXT:	/* [한국어] 확장 capability(0x100 이후) 사슬 기준 */
		pos = pci_find_ext_capability(dev, reg->pci_exp_cap_id);	/* [한국어] 확장 capability 의 시작 오프셋 */
		if (pos == 0)	/* [한국어] 없으면 */
			return;	/* [한국어] 해당 없음 */

		pci_read_config_dword(dev, pos, &header);	/* [한국어] 확장 capability 머리말에 ID 와 판 번호가 함께 들어 있다 */
		if (!hpx3_cap_ver_matches(PCI_EXT_CAP_VER(header),	/* [한국어] 머리말에서 판 번호만 뽑아 */
					  reg->pci_exp_cap_ver))	/* [한국어] 규칙이 요구하는 판과 맞는지 본다 */
			return;	/* [한국어] 안 맞으면 이 규칙은 다른 판을 위한 것이다 */

		break;	/* [한국어] 다음 단계로 */
	case HPX_CFG_VEND_CAP:	/* [한국어] 벤더 고유 capability 기준 — 커널 미구현 */
	case HPX_CFG_DVSEC:	/* [한국어] DVSEC 기준 — 커널 미구현 */
	default:	/* [한국어] 규격에 없는 값도 여기로 온다 */
		pci_warn(dev, "Encountered _HPX type 3 with unsupported config space location");	/* [한국어] 조용히 무시하지 않는다. 펌웨어가 기대한 설정이 적용되지 않았다는 사실이 로그에 남아야 한다 */
		return;	/* [한국어] 적용을 포기한다 */
	}

	pci_read_config_dword(dev, pos + reg->match_offset, &match_reg);	/* [한국어] 조건을 검사할 레지스터를 읽는다 */

	if ((match_reg & reg->match_mask_and) != reg->match_value)	/* [한국어] 관심 있는 비트만 남긴 값이 기대값과 다르면 */
		return;	/* [한국어] 이 규칙의 대상이 아니다 */

	pci_read_config_dword(dev, pos + reg->reg_offset, &write_reg);	/* [한국어] 실제로 고칠 레지스터를 읽는다. match 와 다른 자리일 수 있다 */
	orig_value = write_reg;	/* [한국어] 변화가 있었는지 비교하려고 원본을 챙겨 둔다 */
	write_reg &= reg->reg_mask_and;	/* [한국어] and 로 남길 비트를 고르고 */
	write_reg |= reg->reg_mask_or;	/* [한국어] or 로 세울 비트를 얹는다 */

	if (orig_value == write_reg)	/* [한국어] 값이 그대로면 */
		return;	/* [한국어] 쓰지 않는다. config 쓰기 비용을 아끼고, 아래 로그도 실제 변화가 있을 때만 남긴다 */

	pci_write_config_dword(dev, pos + reg->reg_offset, write_reg);	/* [한국어] 바뀐 값을 써 넣는다 */

	pci_dbg(dev, "Applied _HPX3 at [0x%x]: 0x%08x -> 0x%08x",	/* [한국어] Type 3 은 커널이 내용을 미리 알 수 없는 패치라, 이 로그가 사후 추적의 유일한 단서다 */
		pos, orig_value, write_reg);	/* [한국어] 기준점 오프셋, 이전 값, 이후 값 */
}

/* [한국어]
 * program_hpx_type3 - Type 3 규칙 하나를 적용하기 전에 전제를 확인한다
 *
 * @dev:    대상 장치.
 * @hpx:    적용할 규칙. NULL 이면 아무것도 하지 않는다.
 * @return: 없음.
 *
 * 실질적으로 program_hpx_type3_register() 를 감싸는 얇은 껍데기다.
 * 하는 일은 두 가지 전제 확인뿐이다 — 규칙이 있는가, 그리고 PCIe 장치인가.
 *
 * Type 3 의 기준점이 전부 PCIe capability 나 확장 capability 라서
 * (HPX_CFG_PCICFG 하나만 예외) 전통 PCI 장치에는 적용할 자리가 거의 없다.
 * 그래서 여기서 미리 걸러 낸다.
 *
 * 껍데기를 따로 둔 이유는 호출 구조에 있다. program_type3_hpx_record() 가
 * 규칙마다 이 함수를 부르므로, 확인 로직을 여기 두면 규칙 개수와 무관하게
 * 한 자리에서 관리된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 락 없음.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   acpi_run_hpx() -> program_type3_hpx_record() -> [program_hpx_type3]
 *     -> program_hpx_type3_register()
 */
static void program_hpx_type3(struct pci_dev *dev, struct hpx_type3 *hpx)
{
	if (!hpx)	/* [한국어] 펌웨어가 규칙을 주지 않았으면 */
		return;	/* [한국어] 할 일이 없다 */

	if (!pci_is_pcie(dev))	/* [한국어] 전통 PCI 장치인가. Type 3 의 기준점이 대부분 PCIe capability 라 적용할 자리가 거의 없다 */
		return;	/* [한국어] 미리 걸러 낸다 */

	program_hpx_type3_register(dev, hpx);	/* [한국어] 모든 전제를 통과했으니 실제 적용으로 넘긴다 */
}

/* [한국어]
 * parse_hpx3_register - 14개 필드를 순서대로 구조체에 옮긴다
 *
 * @hpx3_reg:   [출력] 채울 구조체.
 * @reg_fields: 이 규칙에 해당하는 14개 ACPI 객체의 시작 위치.
 * @return:     없음.
 *
 * 형 검사를 여기서 하지 않는 것이 앞의 decode_*_hpx_record() 들과 다르다.
 * 호출자인 program_type3_hpx_record() 가 규칙 전체 범위에 대해 INTEGER
 * 여부를 미리 확인한 뒤 부르기 때문이다. 규칙이 여럿이라 한 번에 확인하는
 * 편이 낫다는 판단이다.
 *
 * 대입 순서가 곧 규격이 정한 필드 배치다. 하나라도 어긋나면 엉뚱한 값이
 * 엉뚱한 자리에 들어가고, Type 3 은 임의 레지스터를 고치는 패치라
 * 그 결과가 무엇일지 예측할 수 없다. 그래서 구조체 선언 순서와 이 함수의
 * 대입 순서를 눈으로 대조할 수 있게 나란히 정렬해 두었다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 락 없음.
 * 에러 경로: 없다(검사는 호출자 몫).
 *
 * 호출 체인:
 *   acpi_run_hpx() -> program_type3_hpx_record() -> [parse_hpx3_register]
 */
static void parse_hpx3_register(struct hpx_type3 *hpx3_reg,
				union acpi_object *reg_fields)
{
	hpx3_reg->device_type            = reg_fields[0].integer.value;	/* [한국어] [0] 대상 장치 종류 */
	hpx3_reg->function_type          = reg_fields[1].integer.value;	/* [한국어] [1] 대상 함수 종류 */
	hpx3_reg->config_space_location  = reg_fields[2].integer.value;	/* [한국어] [2] 오프셋의 기준점 */
	hpx3_reg->pci_exp_cap_id         = reg_fields[3].integer.value;	/* [한국어] [3] 찾을 capability ID */
	hpx3_reg->pci_exp_cap_ver        = reg_fields[4].integer.value;	/* [한국어] [4] 요구 판 번호 + 비교 방식 */
	hpx3_reg->pci_exp_vendor_id      = reg_fields[5].integer.value;	/* [한국어] [5] DVSEC 용 벤더 ID(현재 미사용) */
	hpx3_reg->dvsec_id               = reg_fields[6].integer.value;	/* [한국어] [6] DVSEC 하위 ID(현재 미사용) */
	hpx3_reg->dvsec_rev              = reg_fields[7].integer.value;	/* [한국어] [7] DVSEC 판 번호(현재 미사용) */
	hpx3_reg->match_offset           = reg_fields[8].integer.value;	/* [한국어] [8] 조건을 검사할 오프셋 */
	hpx3_reg->match_mask_and         = reg_fields[9].integer.value;	/* [한국어] [9] 조건 검사 마스크 */
	hpx3_reg->match_value            = reg_fields[10].integer.value;	/* [한국어] [10] 기대값 */
	hpx3_reg->reg_offset             = reg_fields[11].integer.value;	/* [한국어] [11] 고칠 레지스터의 오프셋 */
	hpx3_reg->reg_mask_and           = reg_fields[12].integer.value;	/* [한국어] [12] 남길 비트 마스크 */
	hpx3_reg->reg_mask_or            = reg_fields[13].integer.value;	/* [한국어] [13] 세울 비트. 이 대입 순서가 곧 규격의 필드 배치다 */
}

/* [한국어]
 * program_type3_hpx_record - Type 3 레코드 안의 모든 규칙을 차례로 적용한다
 *
 * @dev:    대상 장치.
 * @record: Type 3 레코드에 해당하는 ACPI 패키지 객체.
 * @return: AE_OK = 성공, AE_ERROR = 형식 불일치.
 *
 * Type 0/1/2 는 레코드 하나에 값 묶음 하나였지만, Type 3 은 레코드 하나에
 * 규칙이 여럿 들어간다. 그래서 이 함수만 "디코딩 + 적용" 을 함께 한다
 * (앞의 셋은 decode 와 program 이 나뉘어 있다).
 *
 * 길이 검증이 이 함수의 핵심이다.
 *   expected_length = 3 + desc_count * 14
 * 3 은 [0]Type, [1]Revision, [2]규칙 개수이고, 규칙 하나는 14개 필드다.
 * 이 값이 실제 package.count 와 다르면 형식이 어긋난 것이므로 통째로
 * 거절한다. 펌웨어가 개수를 잘못 적었는데 그대로 읽으면 배열 밖을 읽는다.
 *
 * 그다음 [2..expected_length) 전 범위가 INTEGER 인지 확인한다. 규칙마다
 * 확인하지 않고 한 번에 하는 덕에 parse_hpx3_register() 가 검사 없이
 * 단순해질 수 있다.
 *
 * 규칙의 시작 위치는 fields + 3 + i * 14 로 계산한다. 3 은 머리말 세 칸,
 * 14 는 규칙 하나의 길이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 락 없음.
 * 에러 경로: 길이/형 불일치는 조용한 AE_ERROR(로그 없음), 알 수 없는
 *   판 번호는 printk 로 알리고 AE_ERROR. 어느 쪽이든 호출자가 그 자리에서
 *   _HPX 전체 처리를 중단한다.
 *
 * 호출 체인:
 *   pci_acpi_program_hp_params() -> acpi_run_hpx()
 *     -> [program_type3_hpx_record]
 *        -> parse_hpx3_register(), program_hpx_type3()
 */
static acpi_status program_type3_hpx_record(struct pci_dev *dev,
					   union acpi_object *record)
{
	union acpi_object *fields = record->package.elements;	/* [한국어] 패키지의 원소 배열 */
	u32 desc_count, expected_length, revision;	/* [한국어] desc_count = 규칙 개수, expected_length = 기대 원소 수, revision = 판 번호 */
	union acpi_object *reg_fields;	/* [한국어] 규칙 하나의 시작 위치를 가리킬 커서 */
	struct hpx_type3 hpx3;	/* [한국어] 규칙 하나를 담을 스택 구조체. 규칙마다 덮어쓰며 재사용한다 */
	int i;	/* [한국어] 순회 첨자 */

	revision = fields[1].integer.value;	/* [한국어] 판 번호([1] 자리) */
	switch (revision) {	/* [한국어] 판 번호로 분기 */
	case 1:	/* [한국어] 현재 정의된 유일한 판 */
		desc_count = fields[2].integer.value;	/* [한국어] [2] 가 규칙 개수다. Type 0/1/2 에는 없는 자리 */
		expected_length = 3 + desc_count * 14;	/* [한국어] 3 은 머리말 세 칸(Type, Revision, 개수), 14 는 규칙 하나의 필드 수 */

		if (record->package.count != expected_length)	/* [한국어] 실제 개수가 계산과 다르면 */
			return AE_ERROR;	/* [한국어] 펌웨어가 개수를 잘못 적은 것이다. 그대로 읽으면 배열 밖을 읽는다 */

		for (i = 2; i < expected_length; i++)	/* [한국어] 규칙 영역 전체의 형을 한 번에 확인한다 */
			if (fields[i].type != ACPI_TYPE_INTEGER)	/* [한국어] 정수가 아닌 원소가 섞여 있으면 */
				return AE_ERROR;	/* [한국어] 거절한다. 이 확인 덕에 parse_hpx3_register 가 검사 없이 단순해진다 */

		for (i = 0; i < desc_count; i++) {	/* [한국어] 규칙 개수만큼 반복 */
			reg_fields = fields + 3 + i * 14;	/* [한국어] 3 은 머리말, 14 는 규칙 하나의 길이. i 번째 규칙의 시작 위치 */
			parse_hpx3_register(&hpx3, reg_fields);	/* [한국어] 14개 필드를 구조체로 옮기고 */
			program_hpx_type3(dev, &hpx3);	/* [한국어] 조건 검사와 적용으로 넘긴다 */
		}	/* [한국어] 순회 끝 */

		break;	/* [한국어] 성공 경로로 */
	default:	/* [한국어] 알 수 없는 판 */
		printk(KERN_WARNING	/* [한국어] pr_warn 대신 printk 를 쓴 것은 상류 코드 그대로다 */
			"%s: Type 3 Revision %d record not supported\n",	/* [한국어] 어느 판을 못 읽었는지 */
			__func__, revision);	/* [한국어] 함수 이름과 판 번호 */
		return AE_ERROR;	/* [한국어] 호출자가 처리를 중단한다 */
	}
	return AE_OK;	/* [한국어] 모든 규칙을 처리했다 */
}

/* [한국어]
 * acpi_run_hpx - _HPX 메서드를 평가해 그 안의 모든 레코드를 Type 별로 분배한다
 *
 * @dev:    설정할 장치.
 * @handle: _HPX 를 평가할 ACPI 노드. 장치 자신일 수도, 조상일 수도 있다.
 * @return: AE_OK = 전부 처리, 그 밖 = 메서드 부재 또는 형식 오류.
 *
 * _HPX(Hot Plug Parameter Extensions)는 레코드의 패키지다. 바깥 패키지의
 * 원소 하나하나가 레코드이고, 각 레코드의 [0] 이 Type 번호다. 이 함수는
 * 그 Type 을 보고 알맞은 디코더/적용 함수 쌍으로 넘긴다.
 *
 * 구조체 셋(hpx0/1/2)을 스택에 잡아 두고 재사용하는 것이 눈에 띈다.
 * 레코드마다 memset 으로 지우고 다시 채우므로, 앞 레코드의 값이 남아
 * 새 레코드에 섞이지 않는다. Type 3 만 구조체가 없는데, 그쪽은
 * program_type3_hpx_record() 가 자기 안에서 지역 변수로 처리한다.
 *
 * 오류가 나면 그 자리에서 goto exit 으로 빠져나가 남은 레코드를 버린다.
 * 부분 적용을 허용하지 않는 셈인데, 형식이 어긋난 테이블이면 그 뒤의
 * 해석도 신뢰할 수 없다는 판단이다.
 *
 * exit 라벨에서 kfree(buffer.pointer) 하는 것을 잊으면 안 된다.
 * ACPI_ALLOCATE_BUFFER 는 ACPI 코어가 크기를 재서 할당해 주는 방식이라,
 * 해제 책임이 호출자에게 있다. 모든 경로가 이 라벨을 지나도록 goto 를
 * 쓴 이유가 그것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. AML 평가가 잠들 수 있다. 락 없음.
 * 에러 경로: 메서드가 없으면 그 status 를 그대로 돌려주고(호출자가
 *   _HPP 로 넘어간다), 형식 오류는 AE_ERROR.
 *
 * 호출 체인:
 *   pci_configure_device() [probe.c:5862] -> pci_acpi_program_hp_params()
 *     -> [acpi_run_hpx]
 *        -> decode_type0/1/2_hpx_record(), program_hpx_type0/1/2(),
 *           program_type3_hpx_record()
 */
static acpi_status acpi_run_hpx(struct pci_dev *dev, acpi_handle handle)
{
	acpi_status status;	/* [한국어] ACPI 호출 결과. 마지막에 그대로 반환한다 */
	struct acpi_buffer buffer = {ACPI_ALLOCATE_BUFFER, NULL};	/* [한국어] ACPI_ALLOCATE_BUFFER 는 "크기를 네가 재서 할당해 달라" 는 뜻이라, 해제 책임이 이쪽에 있다 */
	union acpi_object *package, *record, *fields;	/* [한국어] package = 바깥 패키지, record = 그 원소 하나(레코드), fields = 레코드의 원소 배열 */
	struct hpx_type0 hpx0;	/* [한국어] Type 0 값을 담을 자리. 레코드마다 지우고 재사용한다 */
	struct hpx_type1 hpx1;	/* [한국어] Type 1 용 */
	struct hpx_type2 hpx2;	/* [한국어] Type 2 용. Type 3 은 자기 함수 안에서 지역 변수로 처리해 여기 없다 */
	u32 type;	/* [한국어] 현재 레코드의 Type 번호 */
	int i;	/* [한국어] 레코드 순회 첨자 */

	status = acpi_evaluate_object(handle, "_HPX", NULL, &buffer);	/* [한국어] _HPX 메서드를 평가한다. 인자는 없다 */
	if (ACPI_FAILURE(status))	/* [한국어] 메서드가 없거나 평가에 실패했다 */
		return status;	/* [한국어] 버퍼가 할당되지 않았으므로 해제 없이 그대로 돌려준다. 호출자가 _HPP 로 넘어간다 */

	package = (union acpi_object *)buffer.pointer;	/* [한국어] 불투명 버퍼 포인터를 ACPI 객체로 본다 */
	if (package->type != ACPI_TYPE_PACKAGE) {	/* [한국어] 바깥이 패키지가 아니면 형식이 어긋난 것이다 */
		status = AE_ERROR;	/* [한국어] 오류로 표시하고 */
		goto exit;	/* [한국어] 버퍼 해제를 거쳐 나간다 */
	}

	for (i = 0; i < package->package.count; i++) {	/* [한국어] 레코드를 하나씩 처리한다 */
		record = &package->package.elements[i];	/* [한국어] i 번째 레코드 */
		if (record->type != ACPI_TYPE_PACKAGE) {	/* [한국어] 레코드도 패키지여야 한다 */
			status = AE_ERROR;	/* [한국어] 오류로 표시하고 */
			goto exit;	/* [한국어] 해제를 거쳐 나간다 */
		}

		fields = record->package.elements;	/* [한국어] 레코드 안의 원소 배열 */
		if (fields[0].type != ACPI_TYPE_INTEGER ||	/* [한국어] [0] Type 과 */
		    fields[1].type != ACPI_TYPE_INTEGER) {	/* [한국어] [1] Revision 이 모두 정수여야 한다. 아래 디코더들이 이 둘을 검사 없이 읽으므로 여기서 미리 확인한다 */
			status = AE_ERROR;	/* [한국어] 오류로 표시하고 */
			goto exit;	/* [한국어] 해제를 거쳐 나간다 */
		}

		type = fields[0].integer.value;	/* [한국어] 레코드의 종류 */
		switch (type) {	/* [한국어] 종류별로 디코더/적용 함수 쌍을 고른다 */
		case 0:	/* [한국어] Type 0 — 전통 PCI 설정 */
			memset(&hpx0, 0, sizeof(hpx0));	/* [한국어] 앞 레코드의 값이 남아 섞이지 않도록 지운다 */
			status = decode_type0_hpx_record(record, &hpx0);	/* [한국어] 패키지를 구조체로 옮긴다 */
			if (ACPI_FAILURE(status))	/* [한국어] 형식이 어긋났으면 */
				goto exit;	/* [한국어] 남은 레코드를 버리고 나간다 — 부분 적용을 허용하지 않는다 */
			program_hpx_type0(dev, &hpx0);	/* [한국어] config space 에 적용한다 */
			break;	/* [한국어] 다음 레코드로 */
		case 1:	/* [한국어] Type 1 — PCI-X 설정 */
			memset(&hpx1, 0, sizeof(hpx1));	/* [한국어] 재사용 전 지운다 */
			status = decode_type1_hpx_record(record, &hpx1);	/* [한국어] 파싱은 한다(길이 검증을 통과시켜야 다음 레코드로 넘어갈 수 있다) */
			if (ACPI_FAILURE(status))	/* [한국어] 형식 오류면 */
				goto exit;	/* [한국어] 중단 */
			program_hpx_type1(dev, &hpx1);	/* [한국어] 적용은 하지 않고 경고만 남긴다 */
			break;	/* [한국어] 다음 레코드로 */
		case 2:	/* [한국어] Type 2 — PCIe 오류 보고 설정 */
			memset(&hpx2, 0, sizeof(hpx2));	/* [한국어] 재사용 전 지운다 */
			status = decode_type2_hpx_record(record, &hpx2);	/* [한국어] 16개 필드를 구조체로 옮긴다 */
			if (ACPI_FAILURE(status))	/* [한국어] 형식 오류면 */
				goto exit;	/* [한국어] 중단 */
			program_hpx_type2(dev, &hpx2);	/* [한국어] 조건이 맞으면 AER/DEVCTL 에 적용한다 */
			break;	/* [한국어] 다음 레코드로 */
		case 3:	/* [한국어] Type 3 — 범용 레지스터 패치 */
			status = program_type3_hpx_record(dev, record);	/* [한국어] 이쪽만 디코딩과 적용이 한 함수에 있다. 레코드 하나에 규칙이 여럿이기 때문 */
			if (ACPI_FAILURE(status))
				goto exit;	/* [한국어] 형식 오류면 중단 */
			break;	/* [한국어] 다음 레코드로 */
		default:	/* [한국어] 알 수 없는 Type */
			pr_err("%s: Type %d record not supported\n",	/* [한국어] 어느 종류를 모르는지 남긴다. 여기만 pr_err 인 것은 상류 코드 그대로다 */
			       __func__, type);	/* [한국어] 함수 이름과 Type 번호 */
			status = AE_ERROR;	/* [한국어] 오류로 표시하고 */
			goto exit;	/* [한국어] 중단 */
		}
	}
 exit:	/* [한국어] 모든 경로가 지나는 자리. 성공도 이 라벨을 지난다 */
	kfree(buffer.pointer);	/* [한국어] ACPI 코어가 할당해 준 버퍼를 해제한다. 이것을 빠뜨리면 열거할 때마다 누수가 쌓인다 */
	return status;	/* [한국어] 마지막 status 를 그대로 돌려준다 */
}

/* [한국어]
 * acpi_run_hpp - 옛 _HPP 메서드를 평가해 Type 0 설정으로 적용한다
 *
 * @dev:    설정할 장치.
 * @handle: _HPP 를 평가할 ACPI 노드.
 * @return: AE_OK = 적용함, 그 밖 = 메서드 부재 또는 형식 오류.
 *
 * _HPP 는 _HPX 이전의 메서드다. 레코드 구분도 판 번호도 없이 정수 네 개만
 * 담긴 패키지이고, 그 넷의 뜻이 Type 0 의 네 값과 같다. 그래서 이 함수는
 * 직접 hpx_type0 구조체를 채우고 program_hpx_type0() 을 부른다.
 *
 * revision 에 1 을 손으로 넣는 것이 그 사정을 보여 준다 — _HPP 에는 판
 * 번호 필드가 없지만, 공유하는 적용 함수가 그 필드를 보므로 채워 줘야 한다.
 *
 * 호출자인 pci_acpi_program_hp_params() 가 _HPX 를 먼저 시도하고 실패했을
 * 때만 이쪽으로 온다. 새 메서드를 우선하고 없으면 옛 것으로 내려가는
 * 흔한 배치다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. AML 평가가 잠들 수 있다. 락 없음.
 * 에러 경로: 패키지가 아니거나 원소가 4개가 아니거나 정수가 아니면
 *   AE_ERROR. 모든 경로가 exit 라벨을 지나 버퍼를 해제한다.
 *
 * 호출 체인:
 *   pci_configure_device() [probe.c:5862] -> pci_acpi_program_hp_params()
 *     -> [acpi_run_hpp] -> program_hpx_type0()
 */
static acpi_status acpi_run_hpp(struct pci_dev *dev, acpi_handle handle)
{
	acpi_status status;	/* [한국어] ACPI 호출 결과 */
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };	/* [한국어] ACPI 코어가 할당할 버퍼 */
	union acpi_object *package, *fields;	/* [한국어] package = 바깥 패키지, fields = 그 원소 배열 */
	struct hpx_type0 hpx0;	/* [한국어] Type 0 형식으로 채울 구조체 */
	int i;	/* [한국어] 순회 첨자 */

	memset(&hpx0, 0, sizeof(hpx0));	/* [한국어] 쓰지 않는 필드가 쓰레기로 남지 않도록 미리 지운다 */

	status = acpi_evaluate_object(handle, "_HPP", NULL, &buffer);	/* [한국어] _HPP 메서드를 평가한다 */
	if (ACPI_FAILURE(status))	/* [한국어] 메서드가 없으면 */
		return status;	/* [한국어] 버퍼 없이 그대로 돌려준다 */

	package = (union acpi_object *) buffer.pointer;	/* [한국어] 버퍼를 ACPI 객체로 본다 */
	if (package->type != ACPI_TYPE_PACKAGE ||	/* [한국어] 패키지가 아니거나 */
	    package->package.count != 4) {	/* [한국어] 원소가 정확히 4개가 아니면 _HPP 형식이 아니다 */
		status = AE_ERROR;	/* [한국어] 오류로 표시하고 */
		goto exit;	/* [한국어] 해제를 거쳐 나간다 */
	}

	fields = package->package.elements;	/* [한국어] 원소 배열 */
	for (i = 0; i < 4; i++) {	/* [한국어] 네 원소의 형을 확인한다. _HPP 에는 머리말이 없어 첨자 0 부터 값이다 */
		if (fields[i].type != ACPI_TYPE_INTEGER) {	/* [한국어] 정수가 아니면 */
			status = AE_ERROR;	/* [한국어] 오류로 표시하고 */
			goto exit;	/* [한국어] 해제를 거쳐 나간다 */
		}
	}	/* [한국어] 순회 끝 */

	hpx0.revision        = 1;	/* [한국어] _HPP 에는 판 번호 필드가 없다. 공유하는 적용 함수가 그 필드를 보므로 1 을 손으로 채워 넣는다 */
	hpx0.cache_line_size = fields[0].integer.value;	/* [한국어] [0] Cache Line Size */
	hpx0.latency_timer   = fields[1].integer.value;	/* [한국어] [1] Latency Timer */
	hpx0.enable_serr     = fields[2].integer.value;	/* [한국어] [2] SERR# 활성 여부 */
	hpx0.enable_perr     = fields[3].integer.value;	/* [한국어] [3] 패리티 응답 활성 여부 */

	program_hpx_type0(dev, &hpx0);	/* [한국어] Type 0 과 같은 적용 함수를 쓴다 */

exit:	/* [한국어] 성공도 이 라벨을 지난다 */
	kfree(buffer.pointer);	/* [한국어] 버퍼 해제 */
	return status;	/* [한국어] 성공이면 AE_OK 가 그대로 나간다 */
}

/* pci_acpi_program_hp_params
 *
 * @dev - the pci_dev for which we want parameters
 */
/*
 * [한국어]
 * pci_acpi_program_hp_params - 펌웨어가 권장하는 PCIe 설정값을 장치에 적용한다
 *
 * @dev:    설정할 장치
 * @return: 0 = 적용됨, 음수 = 적용할 값을 찾지 못함.
 *
 * _HPX(Hot Plug Parameter Extensions) 또는 그 이전 판인 _HPP 는 펌웨어가
 * "이 슬롯에 꽂히는 장치는 이렇게 설정하라" 고 권장하는 값의 묶음이다.
 * Cache Line Size, Latency Timer, SERR/PERR 활성화, 그리고 PCIe 쪽으로는
 * Max Payload Size, 오류 보고, Completion Timeout, AER 마스크 등이 들어 있다.
 *
 * 왜 펌웨어가 정해 주는가. 같은 장치라도 어느 보드의 어느 슬롯에 꽂히느냐에
 * 따라 안전한 설정이 달라지기 때문이다. 신호 품질이 나쁜 슬롯에서는
 * 오류 보고를 더 촘촘히 켜야 하고, 특정 칩셋에서는 어떤 기능을 꺼야 한다.
 * 보드 설계자만 아는 그 지식을 ACPI 테이블로 전달하는 것이다.
 *
 * 검색이 위로 올라가는 방식인 점이 중요하다. 이 장치의 ACPI 노드에
 * _HPX 가 없으면 부모로, 또 없으면 그 부모로 계속 올라간다. 슬롯마다
 * 값을 적어 두지 않고 브리지 하나에 적어 그 아래 전부에 적용하는 것이
 * 흔한 구성이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. ACPI 메서드 평가가 잠들 수 있다.
 * 호출자: pci_configure_device() 경로 — 장치를 발견해 설정할 때.
 */
int pci_acpi_program_hp_params(struct pci_dev *dev)
{
	acpi_status status;	/* [한국어] ACPI 호출 결과 */
	acpi_handle handle, phandle;	/* [한국어] handle = 지금 보는 노드, phandle = 그 부모 */
	struct pci_bus *pbus;	/* [한국어] 버스를 거슬러 올라갈 커서 */

	if (acpi_pci_disabled)	/* [한국어] ACPI 로 PCI 를 다루지 않는 시스템이면 */
		return -ENODEV;	/* [한국어] 적용할 값이 있을 리 없다 */

	handle = NULL;	/* [한국어] 못 찾은 상태로 시작 */
	for (pbus = dev->bus; pbus; pbus = pbus->parent) {	/* [한국어] 이 장치의 버스부터 위로 올라가며 */
		handle = acpi_pci_get_bridge_handle(pbus);	/* [한국어] ACPI 핸들이 붙은 브리지를 찾는다 */
		if (handle)	/* [한국어] 찾았으면 */
			break;	/* [한국어] 거기서 시작한다 */
	}	/* [한국어] 순회 끝 */

	/*
	 * _HPP settings apply to all child buses, until another _HPP is
	 * encountered. If we don't find an _HPP for the input pci dev,
	 * look for it in the parent device scope since that would apply to
	 * this pci dev.
	 * [한국어] 위 영어 주석이 밝히는 상속 규칙이 아래 while 루프의 근거다.
	 * 이 장치의 ACPI 노드에 _HPX 가 없으면 부모로, 또 없으면 그 부모로
	 * 계속 올라간다. 슬롯마다 값을 적어 두지 않고 브리지 하나에 적어
	 * 그 아래 전부에 적용하는 것이 흔한 구성이기 때문이다.
	 */
	while (handle) {	/* [한국어] 찾은 노드에서 위로 올라가며 _HPX 나 _HPP 를 찾는다 */
		status = acpi_run_hpx(dev, handle);	/* [한국어] 새 형식을 먼저 시도한다 */
		if (ACPI_SUCCESS(status))	/* [한국어] 성공했으면 */
			return 0;	/* [한국어] 적용을 마쳤으므로 끝낸다 */
		status = acpi_run_hpp(dev, handle);	/* [한국어] 없으면 옛 형식을 시도한다 */
		if (ACPI_SUCCESS(status))	/* [한국어] 성공했으면 */
			return 0;	/* [한국어] 끝낸다 */
		if (acpi_is_root_bridge(handle))	/* [한국어] 루트 브리지까지 올라왔으면 */
			break;	/* [한국어] 더 올라갈 곳이 없다 */
		status = acpi_get_parent(handle, &phandle);	/* [한국어] 부모 노드를 얻는다 */
		if (ACPI_FAILURE(status))	/* [한국어] 부모가 없으면 */
			break;	/* [한국어] 거기서 멈춘다 */
		handle = phandle;	/* [한국어] 한 단계 올라가 다시 시도 */
	}	/* [한국어] 순회 끝 */
	return -ENODEV;	/* [한국어] 어디에서도 못 찾았다. 실패가 아니라 "권장값이 없다" 는 뜻이라 호출자는 그냥 넘어간다 */
}

/**
 * pciehp_is_native - Check whether a hotplug port is handled by the OS
 * @bridge: Hotplug port to check
 *
 * Returns true if the given @bridge is handled by the native PCIe hotplug
 * driver.
 */
/* [한국어]
 * pciehp_is_native - 이 핫플러그 포트를 커널의 pciehp 가 다루는가
 *
 * @bridge: 판정할 핫플러그 포트.
 * @return: true = 커널이 다룬다, false = 펌웨어(ACPI 핫플러그)가 다룬다.
 *
 * PCIe 핫플러그는 커널의 pciehp 드라이버가 다룰 수도 있고 펌웨어가
 * ACPI 이벤트로 다룰 수도 있다. 둘이 동시에 다루면 슬롯 전원을 서로
 * 반대로 조작하는 등 충돌하므로, 부팅 시 소유권을 정한다.
 *
 * 판정이 세 단계다.
 *   1. CONFIG_HOTPLUG_PCI_PCIE 가 꺼져 있으면 pciehp 자체가 없다.
 *   2. pcie_ports_native 가 참이면(부팅 인자 "pcie_ports=native")
 *      펌웨어 판단을 무시하고 커널이 전부 가져간다.
 *   3. 그 밖에는 호스트 브리지의 native_pcie_hotplug 비트를 본다.
 *
 * 3번의 비트가 어디서 정해지는지가 중요한데, 이 파일이 아니다.
 * probe.c 가 기본값 1 로 세우고, _OSC 협상 코드가 펌웨어가 넘겨주지
 * 않으면 0 으로 낮춘다. 그 협상 코드는 drivers/acpi/pci_root.c 에 있고
 * 이 스파스 체크아웃에는 없다. 이 파일은 결과를 읽어 답할 뿐이다.
 *
 * 실행 컨텍스트: 아무 데서나. 락 없이 읽기만 한다.
 *
 * 호출 체인:
 *   pci_bridge_d3_possible() [pci.c:5748] 등 -> [pciehp_is_native]
 *     -> pci_find_host_bridge()
 */
bool pciehp_is_native(struct pci_dev *bridge)
{
	const struct pci_host_bridge *host;	/* [한국어] 소유권 비트를 읽을 호스트 브리지. 읽기만 하므로 const */

	if (!IS_ENABLED(CONFIG_HOTPLUG_PCI_PCIE))	/* [한국어] pciehp 드라이버가 빌드에 없으면 */
		return false;	/* [한국어] 커널이 다룰 수가 없다 */

	if (pcie_ports_native)	/* [한국어] 부팅 인자 "pcie_ports=native" 가 주어졌으면 */
		return true;	/* [한국어] 펌웨어 판단을 무시하고 커널이 전부 가져간다 */

	host = pci_find_host_bridge(bridge->bus);	/* [한국어] 이 브리지가 속한 호스트 브리지 */
	return host->native_pcie_hotplug;	/* [한국어] _OSC 협상 결과가 담긴 비트. 그 협상 코드는 이 트리에 없다(drivers/acpi/pci_root.c) */
}

/**
 * shpchp_is_native - Check whether a hotplug port is handled by the OS
 * @bridge: Hotplug port to check
 *
 * Returns true if the given @bridge is handled by the native SHPC hotplug
 * driver.
 */
/* [한국어]
 * shpchp_is_native - 이 브리지를 커널의 shpchp 가 다루는가
 *
 * @bridge: 판정할 브리지.
 * @return: bridge->shpc_managed 그대로.
 *
 * SHPC(Standard Hot-Plug Controller)는 전통 PCI/PCI-X 시절의 핫플러그
 * 규격이다. PCIe 는 pciehp 를 쓰므로 오늘날 이 경로는 거의 쓰이지 않는다.
 *
 * pciehp_is_native() 와 달리 판정이 한 줄인 이유는, shpc_managed 비트를
 * 세우는 쪽이 이미 모든 조건을 따진 뒤 세우기 때문이다.
 * hotplug/shpchp_core.c:76 과 :610 의 주석이 그 관계를 밝힌다 —
 * shpchp 드라이버가 컨트롤러를 실제로 잡았을 때만 이 비트가 선다.
 *
 * 실행 컨텍스트: 아무 데서나. 락 없이 읽기만 한다.
 *
 * 호출 체인:
 *   (트리 밖 및 코어의 핫플러그 판정 자리) -> [shpchp_is_native]
 */
bool shpchp_is_native(struct pci_dev *bridge)
{
	return bridge->shpc_managed;
}

/**
 * pci_acpi_wake_bus - Root bus wakeup notification fork function.
 * @context: Device wakeup context.
 */
/* [한국어]
 * pci_acpi_wake_bus - 루트 브리지가 깨우기 알림을 받았을 때 실행되는 콜백
 *
 * @context: ACPI 깨우기 문맥. 그 안의 dev 가 루트 브리지의 device 다.
 * @return:  없음.
 *
 * 루트 브리지 수준의 깨우기 GPE 가 울리면 ACPI 코어가 이 콜백을 부른다.
 * 그런데 "어느 장치가 깨웠는가" 는 GPE 만으로는 알 수 없다 — 하나의 GPE 가
 * 그 아래 버스 전체를 대표하기 때문이다. 그래서 버스를 훑어 PME Status 가
 * 서 있는 장치를 찾아 처리하는 pci_pme_wakeup_bus() 로 넘긴다.
 *
 * to_pci_host_bridge(context->dev) 로 device 를 호스트 브리지로 되돌린
 * 뒤 ->bus 를 넘기는 것이 이 한 줄의 전부다. 아래
 * pci_acpi_add_root_pm_notifier() 가 이 콜백을 등록할 때 root->bus->bridge
 * 를 문맥 장치로 주었기 때문에 그 변환이 성립한다.
 *
 * 실행 컨텍스트: ACPI 코어의 알림 처리 문맥(work queue 기반 프로세스
 *   컨텍스트). 잠들 수 있다.
 *
 * 호출 체인:
 *   ACPI GPE -> acpi_pm_notify_handler()(ACPI 코어. drivers/acpi 가 이
 *     스파스 체크아웃에 없어 이 트리에서는 확인할 수 없다)
 *       -> [pci_acpi_wake_bus]
 *     -> pci_pme_wakeup_bus()
 */
static void pci_acpi_wake_bus(struct acpi_device_wakeup_context *context)
{
	pci_pme_wakeup_bus(to_pci_host_bridge(context->dev)->bus);
}

/**
 * pci_acpi_wake_dev - PCI device wakeup notification work function.
 * @context: Device wakeup context.
 */
/* [한국어]
 * pci_acpi_wake_dev - 개별 장치가 깨우기 알림을 받았을 때 실행되는 콜백
 *
 * @context: ACPI 깨우기 문맥. 그 안의 dev 가 pci_dev 의 device 다.
 * @return:  없음.
 *
 * 장치 하나에 대한 깨우기 GPE 가 울렸을 때의 처리다. 위 wake_bus 판과 달리
 * 어느 장치인지 이미 알고 있어 할 일이 구체적이다.
 *
 * 동작 순서
 *   1. pme_poll 을 끈다. 이 장치가 실제로 GPE 로 깨울 수 있음이 증명되었으니,
 *      커널이 주기적으로 PME Status 를 폴링할 이유가 사라진다. 폴링을 끄면
 *      그만큼 유휴 시 CPU 를 덜 쓴다.
 *   2. D3cold 였다면 PME Status 를 읽을 수 없다(config space 가 죽어 있다).
 *      그래서 깨우기 이벤트만 알리고 런타임 PM 에 복귀를 요청한 뒤 끝낸다.
 *   3. 그 밖의 상태라면 PME Status 를 확인해 지운다. 지우지 않으면 같은
 *      알림이 반복해서 올라온다.
 *   4. 이 장치가 브리지라면 그 아래 버스도 훑는다 — 하위 장치의 PME 가
 *      브리지를 통해 올라왔을 수 있다. subordinate 가 NULL 이면
 *      pci_pme_wakeup_bus() 가 알아서 아무것도 하지 않는다.
 *
 * 실행 컨텍스트: ACPI 알림 처리 문맥(프로세스 컨텍스트). 잠들 수 있다.
 *
 * 호출 체인:
 *   ACPI GPE -> acpi_pm_notify_handler() -> [pci_acpi_wake_dev]
 *     -> pci_check_pme_status(), pci_wakeup_event(),
 *        pm_request_resume(), pci_pme_wakeup_bus()
 */
static void pci_acpi_wake_dev(struct acpi_device_wakeup_context *context)
{
	struct pci_dev *pci_dev;	/* [한국어] 문맥에서 되찾을 pci_dev */

	pci_dev = to_pci_dev(context->dev);	/* [한국어] 등록 때 &pci_dev->dev 를 문맥으로 주었기 때문에 이 변환이 성립한다 */

	if (pci_dev->pme_poll)	/* [한국어] 폴링으로 PME 를 감시하던 장치라면 */
		pci_dev->pme_poll = false;	/* [한국어] 이제 GPE 로 깨울 수 있음이 증명되었으니 폴링을 끈다. 유휴 시 CPU 를 그만큼 덜 쓴다 */

	if (pci_dev->current_state == PCI_D3cold) {	/* [한국어] D3cold 에서는 config space 가 죽어 있어 PME Status 를 읽을 수 없다 */
		pci_wakeup_event(pci_dev);	/* [한국어] 깨우기 이벤트만 알리고 */
		pm_request_resume(&pci_dev->dev);	/* [한국어] 런타임 PM 에 복귀를 요청한 뒤 */
		return;	/* [한국어] 끝낸다 */
	}

	/* Clear PME Status if set. */
	if (pci_dev->pme_support)	/* [한국어] PME 를 지원하는 장치라면 */
		pci_check_pme_status(pci_dev);	/* [한국어] PME Status 를 읽고 서 있으면 지운다. 지우지 않으면 같은 알림이 반복해서 올라온다 */

	pci_wakeup_event(pci_dev);	/* [한국어] 깨우기 이벤트를 알린다 */
	pm_request_resume(&pci_dev->dev);	/* [한국어] 런타임 PM 에 복귀를 요청한다 */

	pci_pme_wakeup_bus(pci_dev->subordinate);	/* [한국어] 이 장치가 브리지라면 그 아래 버스도 훑는다. subordinate 가 NULL 이면 이 함수가 알아서 아무것도 하지 않는다 */
}

/**
 * pci_acpi_add_root_pm_notifier - Register PM notifier for root PCI bus.
 * @dev: PCI root bridge ACPI device.
 * @root: PCI root corresponding to @dev.
 */
/* [한국어]
 * pci_acpi_add_root_pm_notifier - 루트 브리지에 깨우기 알림 핸들러를 단다
 *
 * @dev:    루트 브리지의 ACPI 장치.
 * @root:   그에 대응하는 acpi_pci_root.
 * @return: acpi_add_pm_notifier() 의 acpi_status.
 *
 * 위 pci_acpi_wake_bus() 를 콜백으로 등록하는 한 줄짜리 함수다.
 *
 * 세 번째 인자로 root->bus->bridge 를 넘기는 것이 이 함수의 요점이다.
 * ACPI 코어는 그 device 포인터를 깨우기 문맥에 담아 콜백에 되돌려 주는데,
 * 콜백이 to_pci_host_bridge() 로 그것을 호스트 브리지로 되돌린다.
 * 즉 여기서 무엇을 넘기느냐가 콜백에서 무엇을 꺼낼 수 있는지를 정한다.
 *
 * 이 스파스 체크아웃 안에는 호출자가 없다(전수 grep). 상류에서는
 * drivers/acpi/pci_root.c 가 루트 브리지를 등록하면서 부르는데,
 * 그 파일이 이 트리에 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 루트 브리지 등록 중.
 *
 * 호출 체인:
 *   (drivers/acpi/pci_root.c — 이 트리에 없음)
 *     -> [pci_acpi_add_root_pm_notifier] -> acpi_add_pm_notifier()
 */
acpi_status pci_acpi_add_root_pm_notifier(struct acpi_device *dev,
					  struct acpi_pci_root *root)
{
	return acpi_add_pm_notifier(dev, root->bus->bridge, pci_acpi_wake_bus);
}

/**
 * pci_acpi_add_pm_notifier - Register PM notifier for given PCI device.
 * @dev: ACPI device to add the notifier for.
 * @pci_dev: PCI device to check for the PME status if an event is signaled.
 */
/* [한국어]
 * pci_acpi_add_pm_notifier - 개별 장치에 깨우기 알림 핸들러를 단다
 *
 * @dev:     이 PCI 장치의 ACPI companion.
 * @pci_dev: 대상 PCI 장치.
 * @return:  acpi_add_pm_notifier() 의 acpi_status.
 *
 * 위 root 판과 짝을 이루는 장치 단위 등록이다. 콜백은
 * pci_acpi_wake_dev() 이고, 문맥 장치로 &pci_dev->dev 를 넘긴다.
 * 그래서 콜백이 to_pci_dev(context->dev) 로 장치를 되찾을 수 있다.
 *
 * 해제는 pci_acpi_cleanup() 이 pci_acpi_remove_pm_notifier() 로 한다.
 * 그쪽은 이 파일이 아니라 ACPI 코어(혹은 pci-acpi.h 의 매크로)에 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, companion 연결 직후
 *   (pci_acpi_setup 안).
 *
 * 호출 체인:
 *   pci_acpi_setup() -> [pci_acpi_add_pm_notifier] -> acpi_add_pm_notifier()
 */
acpi_status pci_acpi_add_pm_notifier(struct acpi_device *dev,
				     struct pci_dev *pci_dev)
{
	return acpi_add_pm_notifier(dev, &pci_dev->dev, pci_acpi_wake_dev);
}

/*
 * _SxD returns the D-state with the highest power
 * (lowest D-state number) supported in the S-state "x".
 *
 * If the devices does not have a _PRW
 * (Power Resources for Wake) supporting system wakeup from "x"
 * then the OS is free to choose a lower power (higher number
 * D-state) than the return value from _SxD.
 *
 * But if _PRW is enabled at S-state "x", the OS
 * must not choose a power lower than _SxD --
 * unless the device has an _SxW method specifying
 * the lowest power (highest D-state number) the device
 * may enter while still able to wake the system.
 *
 * ie. depending on global OS policy:
 *
 * if (_PRW at S-state x)
 *	choose from highest power _SxD to lowest power _SxW
 * else // no _PRW at S-state x
 *	choose highest power _SxD or any lower power
 */

/* [한국어]
 * acpi_pci_choose_state - 시스템 수면 시 이 장치를 어느 D-state 로 내릴지 고른다
 *
 * @pdev:   대상 장치.
 * @return: PCI_D0 ~ PCI_D3cold 중 하나, 판단 실패면 PCI_POWER_ERROR.
 *
 * 실제 판단은 ACPI 코어의 acpi_pm_device_sleep_state() 가 한다. 바로 위
 * 영어 주석이 그 규칙을 자세히 적어 두었다 — _SxD 가 상한(가장 높은 전력)을
 * 주고, _PRW 가 있으면 _SxW 가 하한(가장 낮은 전력)을 준다.
 *
 * 이 함수가 하는 일은 둘이다.
 *   1. 커널 쪽 제약을 d_max 로 접어 넘긴다. no_d3cold 이거나
 *      d3cold_allowed 가 아니면 D3hot 까지만 허용한다. 이 두 플래그는
 *      드라이버나 사용자가 "이 장치는 전원을 끊지 마라" 고 걸어 두는 것이다.
 *   2. ACPI 의 D-state 표기를 PCI 의 표기로 옮긴다. 값이 같지 않을 수
 *      있으므로 switch 로 하나씩 옮긴다(배열 첨자로 쓰지 않는다).
 *
 * D3hot 과 D3cold 의 차이가 여기서 갈린다. D3hot 은 장치가 전원은 받되
 * 대부분을 끈 상태라 config space 가 살아 있고, D3cold 는 전원 자체가
 * 끊겨 config space 도 죽는다. 복귀 비용이 크게 다르다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 시스템 절전 준비 중. AML 평가가
 *   잠들 수 있다.
 * 에러 경로: acpi_pm_device_sleep_state() 가 음수면 PCI_POWER_ERROR.
 *   switch 를 빠져나가는 경우도 같은 값이다.
 *
 * 호출 체인:
 *   pci_choose_state() / pci_target_state() -> platform_pci_choose_state()
 *     [pci.c:2071] -> [acpi_pci_choose_state]
 *        -> acpi_pm_device_sleep_state()
 */
pci_power_t acpi_pci_choose_state(struct pci_dev *pdev)
{
	int acpi_state, d_max;	/* [한국어] acpi_state = 조회 결과, d_max = 허용할 가장 낮은 전력 상태 */

	if (pdev->no_d3cold || !pdev->d3cold_allowed)	/* [한국어] no_d3cold 나 !d3cold_allowed 는 "이 장치의 전원을 끊지 마라" 는 커널 쪽 제약이다 */
		d_max = ACPI_STATE_D3_HOT;	/* [한국어] D3hot 까지만 허용한다 — 전원은 유지된다 */
	else	/* [한국어] 제약이 없으면 */
		d_max = ACPI_STATE_D3_COLD;	/* [한국어] D3cold 까지 허용한다 */
	acpi_state = acpi_pm_device_sleep_state(&pdev->dev, NULL, d_max);	/* [한국어] 실제 판단은 ACPI 코어가 한다. 두 번째 인자 NULL 은 "목표 시스템 상태를 코어가 알아서 보라" 는 뜻 */
	if (acpi_state < 0)	/* [한국어] 판단에 실패했으면 */
		return PCI_POWER_ERROR;	/* [한국어] 호출자가 다른 방법을 찾는다 */

	switch (acpi_state) {	/* [한국어] ACPI 표기를 PCI 표기로 옮긴다. 값이 같다는 보장이 없어 배열 첨자 대신 switch 를 쓴다 */
	case ACPI_STATE_D0:	/* [한국어] 완전 동작 */
		return PCI_D0;	/* [한국어] PCI 쪽 표기 */
	case ACPI_STATE_D1:
		return PCI_D1;	/* [한국어] 얕은 절전 */
	case ACPI_STATE_D2:
		return PCI_D2;	/* [한국어] 더 깊은 절전 */
	case ACPI_STATE_D3_HOT:
		return PCI_D3hot;	/* [한국어] 전원은 유지한 채 대부분을 끔. config space 는 살아 있다 */
	case ACPI_STATE_D3_COLD:
		return PCI_D3cold;	/* [한국어] 전원 자체가 끊김. config space 도 죽는다 */
	}
	return PCI_POWER_ERROR;	/* [한국어] 규격에 없는 값이 오면 판단 실패로 처리한다 */
}

/* [한국어] acpi_pci_find_companion() 의 전방 선언.
 * 실제 정의는 이 파일 훨씬 아래(companion lookup 훅 옆)에 있는데,
 * 바로 아래 pci_set_acpi_fwnode() 가 그것을 써야 해서 미리 선언해 둔다.
 * 정의를 위로 옮기지 않은 이유는, 그 함수가 훅 변수와 세마포어를 쓰므로
 * 그것들의 정의 뒤에 있어야 하기 때문이다. */
static struct acpi_device *acpi_pci_find_companion(struct device *dev);

/* [한국어]
 * pci_set_acpi_fwnode - PCI 장치에 ACPI companion 을 찾아 걸어 준다
 *
 * @dev:    열거 중인 장치.
 * @return: 없음.
 *
 * 이 파일의 다른 거의 모든 기능이 이 한 줄에 매달려 있다. ACPI companion
 * 이 걸려야 ACPI_COMPANION(&dev->dev) 가 값을 주고, 그래야 전원 관리도
 * 깨우기도 _DSD 속성 조회도 가능해진다.
 *
 * 조건이 둘인 것이 중요하다.
 *   !dev_fwnode(&dev->dev)  이미 firmware node 가 걸려 있으면 건드리지
 *     않는다. devicetree 로 기술된 장치나, 컨트롤러 드라이버가 미리
 *     걸어 둔 경우가 있다.
 *   !pci_dev_is_added(dev)  장치가 아직 device 모델에 등록되기 전이어야
 *     한다. 등록 뒤에 fwnode 를 바꾸면 이미 그것을 읽은 코드와 어긋난다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 열거 중(장치 등록 직전).
 * 에러 경로: 짝을 못 찾으면 NULL 이 걸린다. 그 자체가 정상 — ACPI 에
 *   기술되지 않은 장치가 흔하다.
 *
 * 호출 체인:
 *   pci_scan_single_device() -> pci_device_add() [probe.c:4933]
 *     -> [pci_set_acpi_fwnode] -> acpi_pci_find_companion()
 */
void pci_set_acpi_fwnode(struct pci_dev *dev)
{
	if (!dev_fwnode(&dev->dev) && !pci_dev_is_added(dev))
		ACPI_COMPANION_SET(&dev->dev,
				   acpi_pci_find_companion(&dev->dev));
}

/**
 * pci_dev_acpi_reset - do a function level reset using _RST method
 * @dev: device to reset
 * @probe: if true, return 0 if device supports _RST
 */
/* [한국어]
 * pci_dev_acpi_reset - ACPI _RST 메서드로 장치를 리셋한다
 *
 * @dev:    리셋할 장치.
 * @probe:  true 면 실제로 리셋하지 않고 "이 방법을 쓸 수 있는가" 만 답한다.
 * @return: 0 = 성공(또는 probe 통과), -ENOTTY = 이 방법을 쓸 수 없음,
 *          그 밖의 음수 = IOMMU 준비 실패.
 *
 * pci.c:9953 의 pci_reset_fn_methods[] 첫 항목이 이 함수다. 커널은 장치를
 * 리셋해야 할 때 그 표를 위에서부터 훑으며 쓸 수 있는 방법을 찾는데,
 * 펌웨어가 제공하는 _RST 가 가장 먼저 시도된다 — 보드 설계자가 그 장치에
 * 맞는 리셋 절차를 알고 있을 가능성이 가장 높기 때문이다.
 *
 * probe 인자의 두 얼굴이 이 표의 규약이다. 같은 함수를 두 번 부르는데,
 * 처음에는 probe=true 로 "가능한가" 를 묻고 나중에 probe=false 로 실제
 * 실행한다. 그래서 지원 여부 판정 코드가 한 곳에만 있게 된다.
 *
 * IOMMU 를 멈췄다 되살리는 것이 이 함수에서 눈여겨볼 부분이다. 리셋 중에는
 * 장치가 어떤 DMA 를 낼지 알 수 없고, 리셋 직후 requester ID 조차 잠시
 * 불안정할 수 있다. 그 사이 IOMMU 매핑이 살아 있으면 엉뚱한 메모리가
 * 노출될 수 있으므로 미리 끊는다.
 *
 * 반환값 처리에 미묘한 점이 있다. _RST 평가가 실패하면 ret 를 -ENOTTY 로
 * 덮어쓰지만, 그러고도 pci_dev_reset_iommu_done() 은 반드시 부른다.
 * prepare 를 했으면 done 을 해야 IOMMU 가 원래대로 돌아가기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 리셋 경로. AML 평가가 잠들 수 있다.
 * 에러 경로: 위 세 반환값.
 *
 * 호출 체인:
 *   __pci_reset_function_locked() -> pci_reset_fn_methods[] [pci.c:9953]
 *     -> [pci_dev_acpi_reset]
 *        -> pci_dev_reset_iommu_prepare(), acpi_evaluate_object("_RST"),
 *           pci_dev_reset_iommu_done()
 */
int pci_dev_acpi_reset(struct pci_dev *dev, bool probe)
{
	acpi_handle handle = ACPI_HANDLE(&dev->dev);	/* [한국어] 이 장치의 ACPI 핸들. companion 이 없으면 NULL */
	int ret;	/* [한국어] 반환값 겸 중간 결과 */

	if (!handle || !acpi_has_method(handle, "_RST"))	/* [한국어] 핸들이 없거나 _RST 메서드가 없으면 */
		return -ENOTTY;	/* [한국어] -ENOTTY 는 리셋 방법 표에서 "이 방법은 쓸 수 없다, 다음을 시도하라" 는 뜻이다 */

	if (probe)	/* [한국어] 지원 여부만 묻는 호출이면 */
		return 0;	/* [한국어] 여기까지 왔다는 것이 곧 지원한다는 뜻이라 0 */

	ret = pci_dev_reset_iommu_prepare(dev);	/* [한국어] 리셋 중에는 장치가 어떤 DMA 를 낼지 알 수 없고 requester ID 조차 잠시 불안정할 수 있어 IOMMU 를 먼저 끊는다 */
	if (ret) {	/* [한국어] IOMMU 를 멈추지 못했으면 */
		pci_err(dev, "failed to stop IOMMU for a PCI reset: %d\n", ret);	/* [한국어] 이유를 남기고 */
		return ret;	/* [한국어] 리셋하지 않고 끝낸다. prepare 가 실패했으므로 done 도 부르지 않는다 */
	}

	if (ACPI_FAILURE(acpi_evaluate_object(handle, "_RST", NULL, NULL))) {	/* [한국어] _RST 를 인자 없이 평가한다 */
		pci_warn(dev, "ACPI _RST failed\n");	/* [한국어] 실패를 알린다 */
		ret = -ENOTTY;	/* [한국어] 호출자가 다음 리셋 방법으로 넘어가게 한다 */
	}

	pci_dev_reset_iommu_done(dev);	/* [한국어] 성공이든 실패든 반드시 부른다 — prepare 를 했으면 done 을 해야 IOMMU 가 원래대로 돌아간다 */
	return ret;	/* [한국어] 0 또는 -ENOTTY */
}

/* [한국어]
 * acpi_pci_power_manageable - 이 장치의 전원을 ACPI 로 다룰 수 있는가
 *
 * @dev:    질의 대상 장치.
 * @return: true = ACPI 전원 관리 가능, false = 불가(또는 companion 없음).
 *
 * PCI 장치의 전원을 바꾸는 방법은 둘이다. 장치 자신의 PM Capability 를
 * 쓰거나(표준 PCI 방식), 플랫폼의 전원 자원을 ACPI 로 조작하거나.
 * 뒤쪽이 가능해야 D3cold(전원 자체를 끊는 상태)에 갈 수 있다 — 장치
 * 스스로는 자기 전원을 끊을 수 없기 때문이다.
 *
 * 판정은 두 단계다. companion 이 걸려 있는가, 그리고 그 ACPI 장치에
 * 전원 자원(_PR0/_PR3)이나 전원 메서드(_PS0/_PS3)가 있는가.
 * 앞의 것은 pci_set_acpi_fwnode() 가 걸어 준 것이고, 뒤의 판정은
 * ACPI 코어의 acpi_device_power_manageable() 이 한다.
 *
 * 실행 컨텍스트: 아무 데서나. 락 없이 플래그만 읽는다.
 *
 * 호출 체인:
 *   platform_pci_power_manageable() [pci.c:1969]
 *     -> [acpi_pci_power_manageable] -> acpi_device_power_manageable()
 */
bool acpi_pci_power_manageable(struct pci_dev *dev)
{
	struct acpi_device *adev = ACPI_COMPANION(&dev->dev);

	return adev && acpi_device_power_manageable(adev);
}

/* [한국어]
 * acpi_pci_bridge_d3 - 이 브리지를 D3 로 내려도 핫플러그 이벤트를 놓치지 않는가
 *
 * @dev:    판정할 브리지(핫플러그 포트여야 한다).
 * @return: true = D3 로 내려도 된다, false = 깨어 있어야 한다.
 *
 * 브리지를 D3 로 내리면 그 아래 전력이 크게 줄지만, 대가가 있다. 슬롯에
 * 무언가 꽂히거나 빠져도 브리지가 자고 있으면 알아채지 못한다. 그래서
 * "자면서도 알림을 보낼 수 있는가" 를 확인해야 하고, 그 판단이 이 함수다.
 *
 * 판정이 여러 겹이라 순서를 따라가야 한다.
 *
 *   0. 핫플러그 포트가 아니면(!dev->is_pciehp) 애초에 놓칠 이벤트가 없다.
 *      그런데 여기서 false 를 돌려주는 것이 눈에 띈다 — 이 함수는
 *      "핫플러그 포트에 대한 D3 허용 여부" 만 답하고, 그 밖의 경우는
 *      호출자(pci.c 의 pci_bridge_d3_possible)가 다른 규칙으로 판단한다.
 *
 *   1. 브리지 자신에게 companion 이 있으면
 *      1a. _S0W 가 D2 이하를 돌려주면 -> 안 된다. _S0W 는 "S0 에서 깨우기를
 *          유지하며 갈 수 있는 가장 낮은 전력 상태" 라, 그 답이 D2 이하면
 *          D3 에서는 깨울 수 없다는 뜻이다.
 *      1b. ACPI 로 전원 관리가 되면 -> 된다.
 *
 *   2. 그 밖에는 Root Port 를 찾아 그쪽을 본다. 스위치 하류 포트처럼
 *      자기 companion 이 없는 브리지가 흔하기 때문이다.
 *      2a. Root Port 에 깨우기 GPE(_PRW)가 아예 없으면 -> 안 된다.
 *      2b. Root Port 의 _S0W 도 D2 이하면 -> 안 된다.
 *      2c. Root Port 의 _DSD 에 HotPlugSupportInD3=1 이 있으면 -> 된다.
 *          이 속성이 "이 포트는 D3 에서도 핫플러그를 알린다" 는 펌웨어의
 *          명시적 보증이고, 아래 브리지들도 그 보증을 물려받는다고 본다.
 *
 *   3. 아무 근거도 없으면 false — 보수적으로 깨어 있게 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. AML 평가가 잠들 수 있다.
 * 에러 경로: 모든 불확실은 false 로 수렴한다.
 *
 * 호출 체인:
 *   pci_bridge_d3_possible() -> platform_pci_bridge_d3() [pci.c:2150]
 *     -> [acpi_pci_bridge_d3]
 *        -> acpi_dev_power_state_for_wake(), acpi_device_power_manageable(),
 *           pcie_find_root_port(), acpi_dev_get_property()
 */
bool acpi_pci_bridge_d3(struct pci_dev *dev)
{
	struct pci_dev *rpdev;	/* [한국어] 이 브리지 위의 Root Port */
	struct acpi_device *adev, *rpadev;	/* [한국어] adev = 이 브리지의 companion, rpadev = Root Port 의 companion */
	const union acpi_object *obj;	/* [한국어] _DSD 속성 값을 받을 자리 */

	if (acpi_pci_disabled || !dev->is_pciehp)	/* [한국어] ACPI 를 안 쓰거나 PCIe 핫플러그 포트가 아니면 */
		return false;	/* [한국어] 이 함수는 핫플러그 포트에 대한 D3 허용 여부만 답한다. 그 밖은 호출자가 다른 규칙으로 판단한다 */

	adev = ACPI_COMPANION(&dev->dev);	/* [한국어] 이 브리지 자신의 companion */

	if (adev) {	/* [한국어] companion 이 있으면 브리지 자신의 정보로 먼저 판단해 본다 */
		/*
		 * If the bridge has _S0W, whether or not it can go into D3
		 * depends on what is returned by that object.  In particular,
		 * if the power state returned by _S0W is D2 or shallower,
		 * entering D3 should not be allowed.
		 * [한국어] _S0W 는 "S0 에서 깨우기를 유지하며 갈 수 있는 가장 낮은
		 * 전력 상태" 다. 그 답이 D2 이하라면 D3 에서는 깨울 수 없다는 뜻.
		 */
		if (acpi_dev_power_state_for_wake(adev) <= ACPI_STATE_D2)	/* [한국어] _S0W 가 D2 이하면 D3 에서는 깨울 수 없다는 뜻 */
			return false;	/* [한국어] D3 를 허용하지 않는다 */

		/*
		 * Otherwise, assume that the bridge can enter D3 so long as it
		 * is power-manageable via ACPI.
		 * [한국어] _S0W 가 막지 않았고 ACPI 로 전원을 다룰 수 있다면,
		 * 플랫폼이 이 브리지의 D3 를 뒷받침한다고 본다.
		 */
		if (acpi_device_power_manageable(adev))	/* [한국어] _S0W 가 막지 않았고 ACPI 로 전원을 다룰 수 있으면 */
			return true;	/* [한국어] 허용한다 */
	}

	rpdev = pcie_find_root_port(dev);	/* [한국어] 브리지 자신으로 판단하지 못했으니 Root Port 를 찾는다. 스위치 하류 포트처럼 자기 companion 이 없는 브리지가 흔하다 */
	if (!rpdev)	/* [한국어] Root Port 를 못 찾으면 */
		return false;	/* [한국어] 판단 근거가 없다 */

	if (rpdev == dev)	/* [한국어] 이 브리지가 Root Port 자신이면 */
		rpadev = adev;	/* [한국어] 위에서 이미 얻은 companion 을 그대로 쓴다 */
	else	/* [한국어] 아니면 */
		rpadev = ACPI_COMPANION(&rpdev->dev);	/* [한국어] Root Port 쪽 companion 을 따로 얻는다 */

	if (!rpadev)	/* [한국어] Root Port 에 companion 이 없으면 */
		return false;	/* [한국어] 판단 근거가 없다 */

	/*
	 * If the Root Port cannot signal wakeup signals at all, i.e., it
	 * doesn't supply a wakeup GPE via _PRW, it cannot signal hotplug
	 * events from low-power states including D3hot and D3cold.
	 * [한국어] wakeup.flags.valid 가 곧 "_PRW 로 깨우기 GPE 가 기술되어
	 * 있다" 는 뜻이다. 그것이 없으면 저전력 상태에서 알릴 방법 자체가 없다.
	 */
	if (!rpadev->wakeup.flags.valid)	/* [한국어] _PRW 로 깨우기 GPE 가 기술되어 있는가 */
		return false;	/* [한국어] 없으면 저전력 상태에서 알릴 방법 자체가 없다 */

	/*
	 * In the bridge-below-a-Root-Port case, evaluate _S0W for the Root Port
	 * to verify whether or not it can signal wakeup from D3.
	 * [한국어] rpadev != adev 는 "이 브리지가 Root Port 자신이 아니다" 를
	 * 뜻한다. 그때는 실제로 알림을 보낼 주체인 Root Port 쪽 _S0W 를 봐야 한다.
	 */
	if (rpadev != adev &&	/* [한국어] 이 브리지가 Root Port 자신이 아니고 */
	    acpi_dev_power_state_for_wake(rpadev) <= ACPI_STATE_D2)	/* [한국어] Root Port 의 _S0W 도 D2 이하면 */
		return false;	/* [한국어] 허용하지 않는다 */

	/*
	 * The "HotPlugSupportInD3" property in a Root Port _DSD indicates
	 * the Port can signal hotplug events while in D3.  We assume any
	 * bridges *below* that Root Port can also signal hotplug events
	 * while in D3.
	 * [한국어] 이 속성이 펌웨어의 명시적 보증이고, 위 영어 주석대로
	 * 그 아래 브리지들도 같은 보증을 물려받는다고 본다.
	 */
	if (!acpi_dev_get_property(rpadev, "HotPlugSupportInD3",
				   ACPI_TYPE_INTEGER, &obj) &&
	    obj->integer.value == 1)
		return true;

	return false;
}

/* [한국어]
 * acpi_pci_config_space_access - AML 에 "지금 config space 를 쓸 수 있다/없다" 를 알린다
 *
 * @dev:    대상 장치.
 * @enable: true = 접근 가능해졌다, false = 곧 접근할 수 없게 된다.
 * @return: 없음.
 *
 * ACPI 의 AML 코드도 PCI config space 를 읽고 쓸 수 있다(PCI_Config
 * operation region). 그런데 장치가 D3cold 로 내려가면 그 영역이 죽는다.
 * AML 이 그 사실을 모른 채 접근하면 쓰레기를 읽거나 시스템이 멈춘다.
 *
 * _REG 는 그 알림을 위한 메서드다. ACPI 규격이 정한 규약으로,
 * "이 operation region 을 이제 쓸 수 있다/없다" 를 AML 에 통지한다.
 * AML 은 그 시점에 캐시를 비우거나 다른 경로로 전환할 수 있다.
 *
 * 그래서 acpi_pci_set_power_state() 가 D3cold 로 내려가기 *전에*
 * disconnect 를, D0 로 올라온 *뒤에* connect 를 부른다. 순서가 뒤바뀌면
 * AML 이 죽은 영역에 접근하는 창이 생긴다.
 *
 * 실패를 pci_dbg 로만 남기는 것이 눈에 띈다. _REG 가 없는 장치가 대부분
 * 이라 실패가 정상이고, 경고로 올리면 로그가 시끄러워진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 전원 전환 경로. AML 평가가 잠들 수 있다.
 * 에러 경로: 로그만 남기고 계속 진행한다.
 *
 * 호출 체인:
 *   acpi_pci_set_power_state() -> [acpi_pci_config_space_access]
 *     -> acpi_evaluate_reg()
 */
static void acpi_pci_config_space_access(struct pci_dev *dev, bool enable)
{
	int val = enable ? ACPI_REG_CONNECT : ACPI_REG_DISCONNECT;	/* [한국어] connect 는 "이제 쓸 수 있다", disconnect 는 "곧 쓸 수 없다". 그 두 상수의 실제 값은 include/acpi/ 에 있는데 이 트리에 없어 확인하지 못했다 */
	int ret = acpi_evaluate_reg(ACPI_HANDLE(&dev->dev),	/* [한국어] _REG 를 평가한다. 두 번째 인자가 어느 operation region 인지 지정한다 */
				    ACPI_ADR_SPACE_PCI_CONFIG, val);	/* [한국어] PCI config space 영역 */
	if (ret)	/* [한국어] 실패했으면 */
		pci_dbg(dev, "ACPI _REG %s evaluation failed (%d)\n",	/* [한국어] _REG 가 없는 장치가 대부분이라 실패가 정상이다. 경고로 올리면 로그가 시끄러워지므로 디버그 수준으로 남긴다 */
			enable ? "connect" : "disconnect", ret);	/* [한국어] 어느 방향의 알림이 실패했는지 밝힌다 */
}

/* [한국어]
 * acpi_pci_set_power_state - ACPI 를 통해 장치의 전원 상태를 바꾼다
 *
 * @dev:    대상 장치.
 * @state:  목표 상태(PCI_D0 ~ PCI_D3cold).
 * @return: 0 = 성공, -ENODEV = ACPI 로 다룰 수 없음, -EINVAL = 잘못된 상태,
 *          -EBUSY = 전원 차단이 금지됨, 그 밖의 음수 = ACPI 실패.
 *
 * PCI 의 D-state 표기를 ACPI 의 D-state 로 옮겨 acpi_device_set_power() 에
 * 넘긴다. 그 함수가 _PS0/_PS3 메서드를 부르고 전원 자원(_PR0/_PR3)을
 * 켜고 끈다.
 *
 * _EJ0 가 있으면 -ENODEV 로 거절하는 것이 눈에 띈다. _EJ0 는 "이 장치를
 * 뽑을 수 있다" 는 뜻이라 ACPI 핫플러그(acpiphp)의 관할이다. 그쪽이
 * 슬롯 전원을 직접 다루므로, 전원 관리가 끼어들면 충돌한다.
 *
 * D3cold 로 갈 때만 두 가지를 더 한다.
 *   1. PM QoS 의 NO_POWER_OFF 플래그를 확인한다. 사용자나 상위 계층이
 *      "이 장치의 전원을 끊지 마라" 고 걸어 두었으면 -EBUSY.
 *      PM_QOS_FLAGS_ALL 과 비교하는 것은 "이 장치와 그 조상 전부가
 *      그 플래그를 걸었을 때" 만 막는다는 뜻이다.
 *   2. AML 에 config space 가 곧 죽는다고 알린다(_REG disconnect).
 * 그리고 D0 로 올라온 뒤에는 다시 살아났다고 알린다(_REG connect).
 *
 * 영어 주석이 밝히듯, 접근 가능성이 실제로 달라지는 전이는 D3cold 로
 * 들어갈 때와 D3cold 에서 D0 로 나올 때뿐이다. 그래서 알림도 그 두
 * 자리에만 있다.
 *
 * state_conv[] 를 지정 초기화로 만든 덕에 PCI_D* 상수 값이 무엇이든
 * 짝이 어긋나지 않는다. 다만 그 값이 배열 첨자로 쓰이므로, 위 switch 가
 * 유효한 값만 통과시키는 것이 안전의 전제다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 전원 전환 경로. AML 평가가 잠들 수 있다.
 * 에러 경로: 위 네 가지 반환값. acpi_device_set_power() 실패 시에는
 *   _REG disconnect 를 이미 보냈을 수 있는데, 되돌리지 않는다 —
 *   상태가 불확실하면 AML 이 접근하지 않는 쪽이 안전하기 때문이다.
 *
 * 호출 체인:
 *   pci_set_power_state() -> platform_pci_set_power_state() [pci.c:1996]
 *     -> [acpi_pci_set_power_state]
 *        -> dev_pm_qos_flags(), acpi_pci_config_space_access(),
 *           acpi_device_set_power()
 */
int acpi_pci_set_power_state(struct pci_dev *dev, pci_power_t state)
{
	struct acpi_device *adev = ACPI_COMPANION(&dev->dev);	/* [한국어] 이 장치의 ACPI companion */
	static const u8 state_conv[] = {	/* [한국어] PCI 표기를 ACPI 표기로 옮기는 표. 지정 초기화라 PCI_D* 상수 값이 무엇이든 짝이 어긋나지 않는다 */
		[PCI_D0] = ACPI_STATE_D0,	/* [한국어] 완전 동작 */
		[PCI_D1] = ACPI_STATE_D1,	/* [한국어] 얕은 절전 */
		[PCI_D2] = ACPI_STATE_D2,	/* [한국어] 더 깊은 절전 */
		[PCI_D3hot] = ACPI_STATE_D3_HOT,	/* [한국어] 전원 유지, 대부분 정지 */
		[PCI_D3cold] = ACPI_STATE_D3_COLD,	/* [한국어] 전원 차단 */
	};
	int error;	/* [한국어] acpi_device_set_power 의 반환값 */

	/* If the ACPI device has _EJ0, ignore the device */
	if (!adev || acpi_has_method(adev->handle, "_EJ0"))	/* [한국어] companion 이 없거나 _EJ0(뽑기 메서드)가 있으면 — _EJ0 는 ACPI 핫플러그의 관할이라 그쪽이 슬롯 전원을 직접 다룬다 */
		return -ENODEV;	/* [한국어] 전원 관리가 끼어들면 충돌하므로 물러난다 */

	switch (state) {	/* [한국어] 위 표의 첨자로 쓰기 전에 유효한 값인지 확인한다 */
	case PCI_D0:	/* [한국어] 완전 동작 */
	case PCI_D1:	/* [한국어] 얕은 절전 */
	case PCI_D2:	/* [한국어] 더 깊은 절전 */
	case PCI_D3hot:	/* [한국어] 전원 유지 절전 */
	case PCI_D3cold:	/* [한국어] 전원 차단 */
		break;	/* [한국어] 다섯 중 하나면 통과 */
	default:	/* [한국어] 그 밖의 값은 */
		return -EINVAL;	/* [한국어] 배열 밖을 읽지 않도록 여기서 막는다 */
	}

	if (state == PCI_D3cold) {	/* [한국어] 전원을 끊는 상태로 갈 때만 추가 처리가 있다 */
		if (dev_pm_qos_flags(&dev->dev, PM_QOS_FLAG_NO_POWER_OFF) ==	/* [한국어] 사용자나 상위 계층이 "전원을 끊지 마라" 고 걸어 두었는가 */
				PM_QOS_FLAGS_ALL)	/* [한국어] PM_QOS_FLAGS_ALL 은 이 장치와 그 조상 전부가 그 플래그를 걸었을 때를 뜻한다 */
			return -EBUSY;	/* [한국어] 거절한다 */

		/* Notify AML lack of PCI config space availability */
		acpi_pci_config_space_access(dev, false);	/* [한국어] config space 가 곧 죽는다고 AML 에 알린다. 반드시 전환 *전에* 해야 한다 */
	}

	error = acpi_device_set_power(adev, state_conv[state]);	/* [한국어] ACPI 표기로 옮겨 실제 전환을 맡긴다. _PS0/_PS3 와 전원 자원이 여기서 동작한다 */
	if (error)	/* [한국어] 실패했으면 */
		return error;	/* [한국어] 오류를 그대로 올린다. 이미 보낸 _REG disconnect 를 되돌리지 않는 것은, 상태가 불확실하면 AML 이 접근하지 않는 쪽이 안전하기 때문이다 */

	pci_dbg(dev, "power state changed by ACPI to %s\n",	/* [한국어] 바뀐 결과를 디버그 로그로 남긴다 */
	        acpi_power_state_string(adev->power.state));	/* [한국어] ACPI 가 보고하는 상태를 사람이 읽는 문자열로 */

	/*
	 * Notify AML of PCI config space availability.  Config space is
	 * accessible in all states except D3cold; the only transitions
	 * that change availability are transitions to D3cold and from
	 * D3cold to D0.
	 * [한국어] 알림이 두 자리에만 있는 이유가 위 영어 주석에 있다 —
	 * 접근 가능성이 실제로 달라지는 전이는 D3cold 로 들어갈 때와
	 * D3cold 에서 D0 로 나올 때뿐이다.
	 */
	if (state == PCI_D0)
		acpi_pci_config_space_access(dev, true);

	return 0;
}

/* [한국어]
 * acpi_pci_get_power_state - ACPI 가 보는 현재 전원 상태를 PCI 표기로 돌려준다
 *
 * @dev:    질의 대상 장치.
 * @return: PCI_D0 ~ PCI_D3cold, 또는 PCI_UNKNOWN.
 *
 * set 판의 거울상이다. adev->power.state 는 ACPI 코어가 캐시해 둔 값이라
 * 여기서 AML 을 평가하지 않는다 — 그래서 이 함수는 빠르고 잠들지 않는다.
 * 캐시가 실제 하드웨어와 어긋났을 수 있는데, 그것을 맞추는 것이 아래
 * acpi_pci_refresh_power_state() 의 일이다.
 *
 * PCI_UNKNOWN 을 돌려주는 경우가 셋이다 — companion 이 없거나, ACPI 로
 * 전원 관리가 안 되거나, 캐시가 ACPI_STATE_UNKNOWN 이거나.
 * 셋 다 "모른다" 이지 "꺼져 있다" 가 아니라는 점이 중요하다. 호출자는
 * 이 값을 보고 표준 PCI PM 경로로 넘어가거나 판단을 미룬다.
 *
 * state_conv[] 를 ACPI_STATE_* 로 첨자를 매기는 것이 set 판과 반대
 * 방향이다. adev->power.state 가 유효 범위 안이라는 보장은
 * ACPI 코어에 있다(ACPI_STATE_UNKNOWN 만 위에서 걸러 냈다).
 *
 * 실행 컨텍스트: 아무 데서나. 락 없이 캐시 값만 읽는다.
 *
 * 호출 체인:
 *   pci_update_current_state() -> platform_pci_get_power_state() [pci.c:2022]
 *     -> [acpi_pci_get_power_state]
 */
pci_power_t acpi_pci_get_power_state(struct pci_dev *dev)
{
	struct acpi_device *adev = ACPI_COMPANION(&dev->dev);	/* [한국어] 이 장치의 ACPI companion */
	static const pci_power_t state_conv[] = {	/* [한국어] ACPI 표기를 PCI 표기로 옮기는 표. set 판과 방향이 반대다 */
		[ACPI_STATE_D0]      = PCI_D0,	/* [한국어] 완전 동작 */
		[ACPI_STATE_D1]      = PCI_D1,	/* [한국어] 얕은 절전 */
		[ACPI_STATE_D2]      = PCI_D2,	/* [한국어] 더 깊은 절전 */
		[ACPI_STATE_D3_HOT]  = PCI_D3hot,	/* [한국어] 전원 유지 절전 */
		[ACPI_STATE_D3_COLD] = PCI_D3cold,	/* [한국어] 전원 차단 */
	};
	int state;	/* [한국어] 캐시된 ACPI 전원 상태 */

	if (!adev || !acpi_device_power_manageable(adev))	/* [한국어] companion 이 없거나 ACPI 로 전원을 다룰 수 없으면 */
		return PCI_UNKNOWN;	/* [한국어] 모른다. 꺼져 있다가 아니다 — 호출자는 표준 PCI PM 경로로 넘어간다 */

	state = adev->power.state;	/* [한국어] ACPI 코어가 캐시해 둔 값. 여기서 AML 을 평가하지 않아 빠르고 잠들지 않는다 */
	if (state == ACPI_STATE_UNKNOWN)	/* [한국어] 코어도 모르는 상태면 */
		return PCI_UNKNOWN;	/* [한국어] 모른다고 답한다 */

	return state_conv[state];	/* [한국어] 유효 범위 보장은 ACPI 코어에 있다. UNKNOWN 만 위에서 걸러 냈다 */
}

/* [한국어]
 * acpi_pci_refresh_power_state - ACPI 의 전원 상태 캐시를 하드웨어에서 다시 읽는다
 *
 * @dev:    대상 장치.
 * @return: 없음.
 *
 * acpi_pci_get_power_state() 가 돌려주는 값은 ACPI 코어가 캐시해 둔
 * 것이다. 시스템 복귀 직후처럼 펌웨어가 커널 몰래 전원 상태를 바꿔 놓았을
 * 수 있는 시점에는 그 캐시를 믿을 수 없다. 이 함수가 _STA 등을 다시
 * 평가해 캐시를 실제 상태로 맞춘다.
 *
 * 두 번째 인자를 NULL 로 주는 것은 "새 상태를 알려 줄 곳이 없다,
 * 그냥 갱신만 해라" 는 뜻이다.
 *
 * 두 조건(companion 이 있고 전원 관리가 가능한가)을 확인하는 것은
 * 이 파일의 다른 전원 함수들과 같은 관용이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 복귀 경로. AML 평가가 잠들 수 있다.
 * 에러 경로: 없다 — 갱신에 실패해도 옛 캐시가 남을 뿐이다.
 *
 * 호출 체인:
 *   pci_refresh_power_state() [pci.c:2045] -> [acpi_pci_refresh_power_state]
 *     -> acpi_device_update_power()
 */
void acpi_pci_refresh_power_state(struct pci_dev *dev)
{
	struct acpi_device *adev = ACPI_COMPANION(&dev->dev);	/* [한국어] 이 장치의 ACPI companion */

	if (adev && acpi_device_power_manageable(adev))	/* [한국어] companion 이 있고 ACPI 로 전원을 다룰 수 있을 때만 */
		acpi_device_update_power(adev, NULL);	/* [한국어] _STA 등을 다시 평가해 캐시를 실제 상태로 맞춘다. 두 번째 인자 NULL 은 "새 상태를 알려 줄 곳이 없다, 갱신만 해라" 는 뜻 */
}

/* [한국어]
 * acpi_pci_propagate_wakeup - 깨우기를 다룰 수 있는 조상을 찾아 그쪽에 설정한다
 *
 * @bus:    출발점 버스(장치 자신이 못 하면 그 버스부터 올라간다).
 * @enable: 켤 것인가 끌 것인가.
 * @return: 0 = 처리됨(또는 처리할 곳이 없음), 그 밖 = 설정 실패.
 *
 * 깨우기 GPE 는 장치마다 있는 것이 아니다. 흔히 Root Port 하나가 그 아래
 * 전체를 대표해 하나의 GPE 를 갖는다. 그래서 어떤 장치가 "나를 깨울 수
 * 있게 해 달라" 고 하면, 실제로 설정할 곳은 조상 중 그 능력을 가진
 * 첫 노드다.
 *
 * 그것을 찾아 올라가는 것이 이 함수다. bus->parent 를 따라 오르며
 * bus->self(그 버스의 브리지)가 깨우기를 다룰 수 있는지 묻고, 처음
 * 찾은 곳에 설정한 뒤 곧바로 반환한다. 여러 곳에 중복 설정하지 않는다.
 *
 * 루트 버스에 이르면 bus->parent 가 NULL 이 되어 루프가 끝난다.
 * 루트 버스에는 self 가 없으므로(그 위에 브리지가 없다) bus->bridge
 * (호스트 브리지의 device)를 대신 본다. 그 구분 때문에 루프 밖에
 * 같은 모양의 코드가 한 번 더 있다.
 *
 * 아무 데서도 못 찾으면 0 을 돌려준다 — 실패가 아니라 "이 계층에는
 * 깨우기를 다룰 노드가 없다" 는 뜻이라, 호출자는 성공으로 취급한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. AML 평가가 잠들 수 있다.
 *
 * 호출 체인:
 *   acpi_pci_wakeup() -> [acpi_pci_propagate_wakeup]
 *     -> acpi_pm_device_can_wakeup(), acpi_pm_set_device_wakeup()
 */
static int acpi_pci_propagate_wakeup(struct pci_bus *bus, bool enable)
{
	while (bus->parent) {	/* [한국어] 루트 버스에 이를 때까지 위로 올라간다 */
		if (acpi_pm_device_can_wakeup(&bus->self->dev))	/* [한국어] 이 버스의 브리지가 깨우기를 다룰 수 있는가 */
			return acpi_pm_set_device_wakeup(&bus->self->dev, enable);	/* [한국어] 처음 찾은 곳에 설정하고 곧바로 반환한다. 여러 곳에 중복 설정하지 않는다 */

		bus = bus->parent;	/* [한국어] 한 단계 위로 */
	}	/* [한국어] 순회 끝 */

	/* We have reached the root bus. */
	if (bus->bridge) {	/* [한국어] 루트 버스에는 self 가 없으므로(위에 브리지가 없다) 호스트 브리지의 device 를 대신 본다 */
		if (acpi_pm_device_can_wakeup(bus->bridge))	/* [한국어] 거기가 깨우기를 다룰 수 있으면 */
			return acpi_pm_set_device_wakeup(bus->bridge, enable);	/* [한국어] 거기에 설정한다 */
	}
	return 0;	/* [한국어] 아무 데서도 못 찾았다. 실패가 아니라 이 계층에 깨우기를 다룰 노드가 없다는 뜻이라 호출자는 성공으로 취급한다 */
}

/* [한국어]
 * acpi_pci_wakeup - 이 장치의 깨우기를 켜거나 끈다
 *
 * @dev:    대상 장치.
 * @enable: 켤 것인가.
 * @return: 0 = 처리됨, 그 밖 = 설정 실패.
 *
 * 판단이 두 갈래다. 장치 자신이 깨우기를 다룰 수 있으면 거기에 설정하고,
 * 아니면 조상을 찾아 올라간다(acpi_pci_propagate_wakeup).
 *
 * 조상에 설정하는 것이 왜 맞는가. 깨우기 GPE 는 흔히 Root Port 가
 * 대표로 갖고, 그 GPE 가 울리면 위 pci_acpi_wake_dev()/wake_bus() 가
 * 버스를 훑어 실제로 깨운 장치를 찾아낸다. 그러니 조상 쪽 GPE 만 켜 두면
 * 목적이 달성된다.
 *
 * acpi_pci_disabled 일 때 0 을 돌려주는 것은 "ACPI 를 안 쓰는 시스템이니
 * 여기서 할 일이 없다, 실패는 아니다" 라는 뜻이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. AML 평가가 잠들 수 있다.
 *
 * 호출 체인:
 *   __pci_enable_wake() -> platform_pci_set_wakeup() [pci.c:2098]
 *     -> [acpi_pci_wakeup] -> acpi_pm_set_device_wakeup() 또는
 *        acpi_pci_propagate_wakeup()
 *   pci_acpi_setup() (이 파일) -> [acpi_pci_wakeup]
 */
int acpi_pci_wakeup(struct pci_dev *dev, bool enable)
{
	if (acpi_pci_disabled)	/* [한국어] ACPI 를 안 쓰는 시스템이면 */
		return 0;	/* [한국어] 여기서 할 일이 없다. 실패는 아니다 */

	if (acpi_pm_device_can_wakeup(&dev->dev))	/* [한국어] 이 장치 자신이 깨우기를 다룰 수 있으면 */
		return acpi_pm_set_device_wakeup(&dev->dev, enable);	/* [한국어] 거기에 바로 설정한다 */

	return acpi_pci_propagate_wakeup(dev->bus, enable);	/* [한국어] 아니면 조상을 찾아 올라간다 */
}

/* [한국어]
 * acpi_pci_need_resume - 시스템 복귀 때 이 장치를 반드시 깨워야 하는가
 *
 * @dev:    판정할 장치.
 * @return: true = 반드시 깨워라, false = 자고 있어도 된다.
 *
 * 시스템 절전에서 돌아올 때, 자고 있던 장치를 전부 깨우면 복귀가 느려진다.
 * 그래서 커널은 가능하면 그대로 두려 하는데, 그러면 안 되는 경우가 있고
 * 이 함수가 그것을 가려낸다.
 *
 * true 를 주는 경우가 셋이다.
 *
 *   1. 브리지인데 S0 가 아닌 상태에서 돌아오는 경우. 위 영어 주석이
 *      실제 사례(Samsung 305V4A)를 들어 설명한다 — 시스템 전체 절전을
 *      넘어 브리지를 자게 두면 펌웨어가 혼란스러워한다. 같은 주석이
 *      엔드포인트는 ACPI 6.2 sec 16.1.6 에 따라 S3 진입 전 이미 D3 여야
 *      하므로 영향받지 않는다고 덧붙인다.
 *
 *   2. 깨우기 설정이 어긋난 경우. device_may_wakeup(사용자/드라이버가
 *      원하는 상태)과 adev->wakeup.prepare_count(실제로 준비된 상태)가
 *      다르면, 자고 있는 동안 설정을 고칠 수 없으므로 깨워서 맞춘다.
 *      !! 로 count 를 bool 로 접어 비교하는 것이 그 뜻이다.
 *
 *   3. _DSW 메서드가 있는 경우. _DSW 는 절전 전후로 장치에 알려야 하는
 *      플랫폼 고유 절차라, 그것을 부르려면 장치가 깨어 있어야 한다.
 *      다만 S0(런타임 절전)에서는 해당 없어 그 앞에서 걸러진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 복귀 경로.
 * 에러 경로: companion 이 없거나 전원 관리가 안 되면 false.
 *
 * 호출 체인:
 *   pci_dev_need_resume() -> platform_pci_need_resume() [pci.c:2123]
 *     -> [acpi_pci_need_resume] -> acpi_target_system_state()
 */
bool acpi_pci_need_resume(struct pci_dev *dev)
{
	struct acpi_device *adev;	/* [한국어] 이 장치의 ACPI companion */

	if (acpi_pci_disabled)	/* [한국어] ACPI 를 안 쓰는 시스템이면 */
		return false;	/* [한국어] 판단할 근거가 없다 */

	/*
	 * In some cases (eg. Samsung 305V4A) leaving a bridge in suspend over
	 * system-wide suspend/resume confuses the platform firmware, so avoid
	 * doing that.  According to Section 16.1.6 of ACPI 6.2, endpoint
	 * devices are expected to be in D3 before invoking the S3 entry path
	 * from the firmware, so they should not be affected by this issue.
	 * [한국어] 위 영어 주석이 밝히듯 엔드포인트는 ACPI 6.2 sec 16.1.6 에
	 * 따라 S3 진입 전에 이미 D3 여야 하므로 이 문제에 걸리지 않는다.
	 * 아래 조건이 브리지만 걸러 내는 이유가 그것이다.
	 */
	if (pci_is_bridge(dev) && acpi_target_system_state() != ACPI_STATE_S0)
		return true;

	adev = ACPI_COMPANION(&dev->dev);	/* [한국어] 이 장치의 ACPI companion */
	if (!adev || !acpi_device_power_manageable(adev))	/* [한국어] companion 이 없거나 ACPI 로 전원을 다룰 수 없으면 */
		return false;	/* [한국어] 깨워야 할 ACPI 쪽 이유가 없다 */

	if (adev->wakeup.flags.valid &&	/* [한국어] 깨우기 기능이 기술되어 있고 */
	    device_may_wakeup(&dev->dev) != !!adev->wakeup.prepare_count)	/* [한국어] 원하는 상태(device_may_wakeup)와 실제로 준비된 상태(prepare_count)가 다르면 — 자고 있는 동안 그 설정을 고칠 수 없다 */
		return true;	/* [한국어] 깨워서 맞춘다 */

	if (acpi_target_system_state() == ACPI_STATE_S0)	/* [한국어] 런타임 절전(S0)이면 */
		return false;	/* [한국어] 아래 _DSW 규칙은 시스템 절전에만 해당하므로 여기서 끝낸다 */

	return !!adev->power.flags.dsw_present;	/* [한국어] _DSW 는 절전 전후로 불러야 하는 플랫폼 고유 절차라, 그것이 있으면 장치가 깨어 있어야 한다 */
}

/* [한국어]
 * acpi_pci_add_bus - 버스가 생겼을 때의 ACPI 쪽 초기화
 *
 * @bus:    새로 만들어진 PCI 버스.
 * @return: 없음.
 *
 * 두 가지 다른 일을 한다.
 *
 *   1. 모든 버스에 대해: ACPI 가 기술한 슬롯을 열거한다.
 *      acpi_pci_slot_enumerate() 는 _SUN 으로 물리 슬롯 번호를 붙이고,
 *      acpiphp_enumerate_slots() 는 ACPI 핫플러그 슬롯 객체를 만든다.
 *      후자는 hotplug/acpiphp_glue.c:1830 의 주석이 밝히듯 이 함수가
 *      유일한 진입점이다.
 *
 *   2. 루트 버스에 대해서만: 호스트 브리지의 _DSM Function 8
 *      ("Reset Delay")을 조회한다. 1 을 돌려주면 "이 계층의 모든 장치가
 *      이미 전원 인가 후 리셋 지연을 마쳤다" 는 펌웨어의 보증이라,
 *      커널이 그 대기를 건너뛸 수 있다. 부팅 시간이 눈에 띄게 줄어든다.
 *      그 보증을 bridge->ignore_reset_delay 에 기록해 두면
 *      pci_acpi_optimize_delay() 가 장치마다 d3cold_delay 를 0 으로 만든다.
 *
 * pci_is_root_bus() 검사 뒤에 return 이 있어 코드가 두 부분으로 갈리는
 * 모양이 되었다. 하위 버스는 1번만 하고 끝난다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 버스 생성 직후. AML 평가가 잠들 수 있다.
 * 에러 경로: 핸들이 없거나 _DSM 이 없으면 조용히 넘어간다.
 *
 * 호출 체인:
 *   pci_create_root_bus()/pci_add_new_bus() -> pcibios_add_bus()
 *     [probe.c:2672, 3071] -> (ARM64/RISC-V 판) -> [acpi_pci_add_bus]
 *        -> acpi_pci_slot_enumerate(), acpiphp_enumerate_slots(),
 *           acpi_evaluate_dsm_typed()
 */
void acpi_pci_add_bus(struct pci_bus *bus)
{
	union acpi_object *obj;	/* [한국어] _DSM 반환 객체 */
	struct pci_host_bridge *bridge;	/* [한국어] 플래그를 세울 호스트 브리지 */

	if (acpi_pci_disabled || !bus->bridge || !ACPI_HANDLE(bus->bridge))	/* [한국어] ACPI 를 안 쓰거나 브리지 device 가 없거나 ACPI 핸들이 없으면 */
		return;	/* [한국어] ACPI 쪽에서 할 일이 없다 */

	acpi_pci_slot_enumerate(bus);	/* [한국어] _SUN 으로 물리 슬롯 번호를 붙인다 */
	acpiphp_enumerate_slots(bus);	/* [한국어] ACPI 핫플러그 슬롯 객체를 만든다. hotplug/acpiphp_glue.c:1830 의 주석이 이 함수가 유일한 진입점이라고 밝힌다 */

	/*
	 * For a host bridge, check its _DSM for function 8 and if
	 * that is available, mark it in pci_host_bridge.
	 * [한국어] 1 을 돌려주면 "이 계층의 모든 장치가 이미 전원 인가 리셋
	 * 지연을 마쳤다" 는 펌웨어의 보증이다. 그것을 기록해 두면
	 * pci_acpi_optimize_delay() 가 장치마다 d3cold_delay 를 0 으로 만든다.
	 */
	if (!pci_is_root_bus(bus))	/* [한국어] 아래 _DSM 조회는 루트 버스에만 해당한다 */
		return;	/* [한국어] 하위 버스는 슬롯 열거까지만 하고 끝난다 */

	obj = acpi_evaluate_dsm_typed(ACPI_HANDLE(bus->bridge), &pci_acpi_dsm_guid, 3,	/* [한국어] 호스트 브리지에 리비전 3 으로 _DSM 을 부른다 */
				      DSM_PCI_POWER_ON_RESET_DELAY, NULL, ACPI_TYPE_INTEGER);	/* [한국어] 함수 번호는 POWER_ON_RESET_DELAY. 그 상수의 값은 include/linux/pci-acpi.h 에 있는데 이 트리에 없어 확인하지 못했다 */
	if (!obj)	/* [한국어] 메서드가 없거나 형이 다르면 */
		return;	/* [한국어] 기본 대기 시간을 그대로 쓴다 */

	if (obj->integer.value == 1) {	/* [한국어] 1 이 "이미 지연을 마쳤다" 는 보증이다 */
		bridge = pci_find_host_bridge(bus);	/* [한국어] 이 버스의 호스트 브리지를 찾아 */
		bridge->ignore_reset_delay = 1;	/* [한국어] 플래그를 세운다. pci_acpi_optimize_delay() 가 장치마다 이것을 읽는다 */
	}
	ACPI_FREE(obj);	/* [한국어] NULL 을 받아도 안전하다 */
}

/* [한국어]
 * acpi_pci_remove_bus - 버스가 사라질 때의 ACPI 쪽 정리
 *
 * @bus:    없어질 PCI 버스.
 * @return: 없음.
 *
 * acpi_pci_add_bus() 의 1번이 만든 것을 되돌린다. 순서가 생성의 역순인
 * 점을 눈여겨볼 만하다 — 핫플러그 슬롯을 먼저 없애고 그다음 슬롯 정보를
 * 없앤다. 반대로 하면 핫플러그 슬롯이 이미 사라진 슬롯 정보를 참조할 수 있다.
 *
 * _DSM 조회(add 의 2번)에 대응하는 정리는 없다. 그것은 상태를 만들지 않고
 * 플래그 하나만 세웠고, 그 플래그는 호스트 브리지와 수명을 함께한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 버스 제거 경로.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_remove_bus() -> pcibios_remove_bus() [remove.c:183]
 *     -> (ARM64/RISC-V 판) -> [acpi_pci_remove_bus]
 *        -> acpiphp_remove_slots(), acpi_pci_slot_remove()
 */
void acpi_pci_remove_bus(struct pci_bus *bus)
{
	if (acpi_pci_disabled || !bus->bridge)
		return;

	acpiphp_remove_slots(bus);
	acpi_pci_slot_remove(bus);
}

/* ACPI bus type */

/* [한국어] companion lookup 훅과 그것을 지키는 세마포어.
 *
 * 대부분의 PCI 장치는 _ADR 이 표준 bus-device-function 인코딩을 따르므로
 * acpi_pci_find_companion() 의 기본 계산으로 짝을 찾을 수 있다. 그러나
 * 일부 플랫폼(가상화 환경, 특수 컨트롤러)은 비표준 인코딩을 쓴다.
 * 그런 곳이 자기만의 찾기 방법을 끼워 넣을 수 있게 훅 하나를 두었다.
 *
 * 세마포어가 필요한 이유는 수명 때문이다. 훅을 등록한 모듈이 내려갈 때
 * "지금 실행 중인 콜백이 없음" 을 보장해야 함수 포인터를 안전하게
 * 지울 수 있다. rwsem 의 down_write 가 그 대기를 해 준다 —
 * 모든 down_read 가 풀릴 때까지 막히기 때문이다.
 * 그래서 조회 쪽(acpi_pci_find_companion)은 down_read 로 훅을 읽고,
 * 등록/해제 쪽은 down_write 를 쓴다. */
static DECLARE_RWSEM(pci_acpi_companion_lookup_sem);
/* [한국어] 플랫폼이 끼워 넣은 companion 찾기 콜백. NULL 이면 기본 방식만 쓴다.
 * 설정자: pci_acpi_set_companion_lookup_hook()(NULL 이 아닐 때만),
 *   pci_acpi_clear_companion_lookup_hook()(NULL 로 지운다).
 * 읽는 자: acpi_pci_find_companion() 이 down_read 아래에서 읽는다.
 * 값 범위: NULL 또는 유효한 함수 포인터. 동시에 하나만 등록할 수 있어
 *   이미 있으면 등록이 -EBUSY 로 거절된다.
 * 동기화: pci_acpi_companion_lookup_sem. */
static struct acpi_device *(*pci_acpi_find_companion_hook)(struct pci_dev *);

/**
 * pci_acpi_set_companion_lookup_hook - Set ACPI companion lookup callback.
 * @func: ACPI companion lookup callback pointer or NULL.
 *
 * Set a special ACPI companion lookup callback for PCI devices whose companion
 * objects in the ACPI namespace have _ADR with non-standard bus-device-function
 * encodings.
 *
 * Return 0 on success or a negative error code on failure (in which case no
 * changes are made).
 *
 * The caller is responsible for the appropriate ordering of the invocations of
 * this function with respect to the enumeration of the PCI devices needing the
 * callback installed by it.
 */
/* [한국어]
 * pci_acpi_set_companion_lookup_hook - 플랫폼 고유 companion 찾기 콜백을 등록한다
 *
 * @func:   등록할 콜백. NULL 은 받지 않는다.
 * @return: 0 = 등록됨, -EINVAL = func 가 NULL, -EBUSY = 이미 등록된 훅이 있음.
 *
 * 위 kernel-doc 이 쓰임을 밝힌다 — _ADR 이 표준 bus-device-function
 * 인코딩을 따르지 않는 플랫폼을 위한 구멍이다.
 *
 * 훅을 하나만 허용하는 것이 설계의 요점이다. 여럿을 사슬로 잇는 대신
 * -EBUSY 로 거절하는데, 그러면 "누가 이 시스템의 companion 찾기를
 * 책임지는가" 가 명확해진다. 두 플랫폼 코드가 동시에 등록하려 들면
 * 그것 자체가 설정 오류다.
 *
 * NULL 을 -EINVAL 로 거절하는 것도 의도적이다. NULL 을 넘겨 해제하는
 * 방식이었다면 clear 함수와 뜻이 겹치고, 실수로 지워 버릴 여지가 생긴다.
 *
 * kernel-doc 의 마지막 문단이 중요한 계약을 하나 더 밝힌다 — 이 훅이
 * 필요한 장치들이 열거되기 *전에* 등록을 마치는 것은 호출자 책임이다.
 * 이 함수는 이미 열거된 장치의 companion 을 고쳐 주지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. down_write 가 잠들 수 있다.
 *
 * 호출 체인: 이 스파스 체크아웃 안에는 호출자가 없다(전수 grep).
 *   EXPORT_SYMBOL_GPL 로 모듈에 노출되어 있다.
 */
int pci_acpi_set_companion_lookup_hook(struct acpi_device *(*func)(struct pci_dev *))
{
	int ret;	/* [한국어] 반환값 */

	if (!func)	/* [한국어] NULL 로 해제하는 방식을 막는다 — clear 함수와 뜻이 겹치고 실수로 지울 여지가 생긴다 */
		return -EINVAL;	/* [한국어] 잘못된 인자 */

	down_write(&pci_acpi_companion_lookup_sem);	/* [한국어] 쓰기 잠금. 실행 중인 콜백이 모두 끝날 때까지 기다린다 */

	if (pci_acpi_find_companion_hook) {	/* [한국어] 이미 등록된 훅이 있으면 */
		ret = -EBUSY;	/* [한국어] 덮어쓰지 않고 거절한다. 훅을 하나만 허용해 "누가 책임지는가" 를 명확히 한다 */
	} else {
		pci_acpi_find_companion_hook = func;	/* [한국어] 비어 있으면 등록한다 */
		ret = 0;	/* [한국어] 성공 */
	}

	up_write(&pci_acpi_companion_lookup_sem);	/* [한국어] 잠금 해제 */

	return ret;	/* [한국어] 0 또는 -EBUSY */
}
EXPORT_SYMBOL_GPL(pci_acpi_set_companion_lookup_hook);

/**
 * pci_acpi_clear_companion_lookup_hook - Clear ACPI companion lookup callback.
 *
 * Clear the special ACPI companion lookup callback previously set by
 * pci_acpi_set_companion_lookup_hook().  Block until the last running instance
 * of the callback returns before clearing it.
 *
 * The caller is responsible for the appropriate ordering of the invocations of
 * this function with respect to the enumeration of the PCI devices needing the
 * callback cleared by it.
 */
/* [한국어]
 * pci_acpi_clear_companion_lookup_hook - 등록된 콜백을 지운다
 *
 * @return: 없음.
 *
 * 짝인 set 함수와 달리 실패할 수 없다. 훅이 없어도 NULL 을 다시 NULL 로
 * 만들 뿐이다.
 *
 * kernel-doc 이 밝히는 "마지막으로 실행 중인 콜백이 끝날 때까지 막힌다"
 * 가 이 함수의 존재 이유다. 훅을 제공한 모듈이 내려가려면 그 함수가
 * 더 이상 실행 중이 아님을 보장받아야 하는데, down_write 가 정확히
 * 그것을 해 준다 — 모든 down_read 가 풀릴 때까지 기다린다.
 *
 * 그냥 포인터에 NULL 을 대입하는 것으로는 부족하다. 대입 직후에도
 * 이미 그 값을 읽어 호출 중인 스레드가 있을 수 있고, 그 스레드가
 * 해제된 모듈 코드로 뛰어들면 커널이 죽는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. down_write 가 잠들 수 있다.
 *
 * 호출 체인: 이 스파스 체크아웃 안에는 호출자가 없다(전수 grep).
 *   EXPORT_SYMBOL_GPL 로 모듈에 노출되어 있다.
 */
void pci_acpi_clear_companion_lookup_hook(void)
{
	down_write(&pci_acpi_companion_lookup_sem);	/* [한국어] 쓰기 잠금이 실행 중인 콜백이 모두 끝나기를 기다려 준다. 그 대기가 이 함수의 존재 이유다 */

	pci_acpi_find_companion_hook = NULL;	/* [한국어] 그냥 대입만으로는 부족하다 — 이미 값을 읽어 호출 중인 스레드가 해제된 모듈 코드로 뛰어들 수 있다 */

	up_write(&pci_acpi_companion_lookup_sem);	/* [한국어] 잠금 해제 */
}
EXPORT_SYMBOL_GPL(pci_acpi_clear_companion_lookup_hook);

/* [한국어]
 * acpi_pci_find_companion - PCI 장치에 대응하는 ACPI 노드를 찾는다
 *
 * @dev:    PCI 장치의 device. to_pci_dev 로 되돌려 쓴다.
 * @return: 짝이 되는 acpi_device, 없으면 NULL.
 *
 * 이 파일의 전제를 만드는 함수다. 여기서 찾은 짝이 걸려야 전원 관리도
 * 깨우기도 _DSD 속성 조회도 동작한다.
 *
 * 찾는 방법이 두 갈래다.
 *   1. 플랫폼이 훅을 등록해 두었으면 그것을 먼저 쓴다. 비표준 _ADR
 *      인코딩을 쓰는 플랫폼을 위한 길이다.
 *   2. 그 밖에는 표준 방식 — _ADR 이 (슬롯 << 16) | 함수 라는 규약을
 *      쓴다(영어 주석이 ACPI 규격을 참조하라고 적어 두었다).
 *      부모의 companion 아래에서 그 주소를 가진 자식을 찾는다.
 *
 * check_children 을 브리지일 때만 참으로 주는 것이 눈에 띈다. ACPI 노드가
 * 계층을 그대로 반영하지 않는 경우가 있어, 브리지라면 한 단계 더 내려가
 * 찾아 볼 값어치가 있다는 뜻이다.
 *
 * 마지막 예외 처리가 이 함수에서 가장 미묘하다. 호스트 브리지 노드 아래에
 * PCI 장치가 아닌 ACPI 객체가 있을 수 있는데, 그런 것이 _HID 와 _ADR 을
 * 함께 갖는 경우가 있다(영어 주석: ACPI 6.3 sec 6.1 위반이지만 실제로
 * 흔하다). 그 _ADR 은 흔히 0 이라, 루트 버스의 devfn 0 장치와 잘못
 * 짝지어진다. 그래서 "_HID 가 있고, addr 이 0 이고, 루트 버스" 세 조건이
 * 모두 맞으면 가짜로 보고 버린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 열거 중. down_read 와 ACPI 조회가
 *   잠들 수 있다.
 * 에러 경로: 못 찾으면 NULL. 정상적인 결과다.
 *
 * 호출 체인:
 *   pci_device_add() [probe.c:4933] -> pci_set_acpi_fwnode()
 *     -> [acpi_pci_find_companion] -> acpi_find_child_device()
 */
static struct acpi_device *acpi_pci_find_companion(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] sysfs 의 device 를 pci_dev 로 되돌린다 */
	struct acpi_device *adev;	/* [한국어] 찾은 companion 을 담을 자리 */
	bool check_children;	/* [한국어] 자식 노드까지 뒤질 것인가 */
	u64 addr;	/* [한국어] _ADR 규약에 맞춘 주소 */

	if (!dev->parent)	/* [한국어] 부모가 없으면 — 부모의 companion 아래에서 찾는 방식이라 성립하지 않는다 */
		return NULL;	/* [한국어] 짝을 찾을 수 없다 */

	down_read(&pci_acpi_companion_lookup_sem);	/* [한국어] 읽기 잠금. 훅이 해제되는 중에 그것을 부르지 않도록 막는다 */

	adev = pci_acpi_find_companion_hook ?	/* [한국어] 플랫폼이 훅을 등록해 두었으면 그것을 먼저 쓴다 */
		pci_acpi_find_companion_hook(pci_dev) : NULL;	/* [한국어] 없으면 NULL */

	up_read(&pci_acpi_companion_lookup_sem);	/* [한국어] 콜백 호출이 끝났으므로 푼다 */

	if (adev)	/* [한국어] 훅이 짝을 찾았으면 */
		return adev;	/* [한국어] 그대로 쓴다 */

	check_children = pci_is_bridge(pci_dev);	/* [한국어] 브리지라면 한 단계 더 내려가 찾아 볼 값어치가 있다 — ACPI 노드가 PCI 계층을 그대로 반영하지 않는 경우가 있다 */
	/* Please ref to ACPI spec for the syntax of _ADR */
	addr = (PCI_SLOT(pci_dev->devfn) << 16) | PCI_FUNC(pci_dev->devfn);	/* [한국어] _ADR 규약: 상위 16비트가 슬롯(장치) 번호, 하위 16비트가 함수 번호 */
	adev = acpi_find_child_device(ACPI_COMPANION(dev->parent), addr,	/* [한국어] 부모의 companion 아래에서 그 주소를 가진 자식을 찾는다 */
				      check_children);	/* [한국어] 브리지면 손자까지 본다 */

	/*
	 * There may be ACPI device objects in the ACPI namespace that are
	 * children of the device object representing the host bridge, but don't
	 * represent PCI devices.  Both _HID and _ADR may be present for them,
	 * even though that is against the specification (for example, see
	 * Section 6.1 of ACPI 6.3), but in many cases the _ADR returns 0 which
	 * appears to indicate that they should not be taken into consideration
	 * as potential companions of PCI devices on the root bus.
	 *
	 * To catch this special case, disregard the returned device object if
	 * it has a valid _HID, addr is 0 and the PCI device at hand is on the
	 * root bus.
	 * [한국어] 세 조건이 모두 맞을 때만 버린다 — _HID 가 있고(platform_id),
	 * addr 이 0 이고, 루트 버스에 있는 장치일 때. 하나라도 다르면
	 * 진짜 짝일 수 있으므로 그대로 쓴다.
	 */
	if (adev && adev->pnp.type.platform_id && !addr &&	/* [한국어] 찾긴 했는데 _HID 가 있고 주소가 0 이고 */
	    pci_is_root_bus(pci_dev->bus))	/* [한국어] 루트 버스의 장치라면 — 세 조건이 모두 맞으면 PCI 장치가 아닌 가짜 짝이다 */
		return NULL;	/* [한국어] 버린다 */

	return adev;	/* [한국어] 못 찾았으면 NULL 이 그대로 나간다. 정상적인 결과다 */
}

/**
 * pci_acpi_optimize_delay - optimize PCI D3 and D3cold delay from ACPI
 * @pdev: the PCI device whose delay is to be updated
 * @handle: ACPI handle of this device
 *
 * Update the d3hot_delay and d3cold_delay of a PCI device from the ACPI _DSM
 * control method of either the device itself or the PCI host bridge.
 *
 * Function 8, "Reset Delay," applies to the entire hierarchy below a PCI
 * host bridge.  If it returns one, the OS may assume that all devices in
 * the hierarchy have already completed power-on reset delays.
 *
 * Function 9, "Device Readiness Durations," applies only to the object
 * where it is located.  It returns delay durations required after various
 * events if the device requires less time than the spec requires.  Delays
 * from this function take precedence over the Reset Delay function.
 *
 * These _DSM functions are defined by the draft ECN of January 28, 2014,
 * titled "ACPI additions for FW latency optimizations."
 */
/* [한국어]
 * pci_acpi_optimize_delay - 펌웨어가 알려 준 값으로 전원 전환 대기 시간을 줄인다
 *
 * @pdev:   대상 장치.
 * @handle: 이 장치의 ACPI 핸들.
 * @return: 없음.
 *
 * 규격은 전원 전환 뒤 장치가 준비될 때까지 기다려야 하는 시간을 넉넉하게
 * 정해 두었다(D3cold 복귀 100ms 등). 모든 장치가 그만큼 필요한 것은
 * 아니지만, 커널은 어느 장치가 더 빠른지 알 수 없어 규격대로 기다린다.
 * 보드 설계자는 그것을 알므로, 펌웨어가 실제 값을 알려 주면 부팅과
 * 복귀가 눈에 띄게 빨라진다.
 *
 * 위 kernel-doc 이 두 _DSM 함수의 차이를 밝힌다.
 *   Function 8 "Reset Delay"        호스트 브리지 아래 계층 전체에 적용.
 *     1 을 돌려주면 모든 장치가 이미 전원 인가 리셋 지연을 마쳤다는 뜻.
 *     그 결과는 acpi_pci_add_bus() 가 bridge->ignore_reset_delay 에 담아
 *     두었고, 이 함수 첫 줄이 그것을 읽어 d3cold_delay 를 0 으로 만든다.
 *   Function 9 "Device Readiness Durations"  이 객체에만 적용.
 *     여러 상황별 지연을 패키지로 돌려주며, Function 8 보다 우선한다.
 *
 * 그 우선 관계가 코드 순서에 그대로 드러난다 — 먼저 Function 8 의 결과로
 * 0 을 넣고, 그다음 Function 9 의 값으로 덮어쓴다.
 *
 * 패키지에서 [0] 과 [3] 만 쓰는 것은 각각 D3cold 와 D3hot 복귀 지연이기
 * 때문이다. 나머지 항목은 커널이 아직 쓰지 않는다.
 * 값을 1000 으로 나누는 것은 _DSM 이 마이크로초로, pdev 필드가 밀리초로
 * 값을 담기 때문이다.
 *
 * 규격 기본값보다 작을 때만 받아들이는 것이 안전장치다. 펌웨어가 더 큰
 * 값을 주더라도 커널은 기존 값을 유지한다 — 이 최적화의 목적은 줄이는
 * 것이지 늘리는 것이 아니다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, companion 연결 직후. AML 평가가
 *   잠들 수 있다.
 * 에러 경로: _DSM 이 없거나 형이 다르면 조용히 끝낸다.
 *
 * 호출 체인:
 *   pci_acpi_setup() -> [pci_acpi_optimize_delay]
 *     -> acpi_evaluate_dsm_typed()
 */
static void pci_acpi_optimize_delay(struct pci_dev *pdev,
				    acpi_handle handle)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(pdev->bus);	/* [한국어] Function 8 의 결과가 담긴 호스트 브리지 */
	int value;	/* [한국어] 마이크로초를 밀리초로 줄인 값을 담을 자리 */
	union acpi_object *obj, *elements;	/* [한국어] obj = _DSM 반환 패키지, elements = 그 원소 배열 */

	if (bridge->ignore_reset_delay)	/* [한국어] Function 8 이 "이미 지연을 마쳤다" 고 보증했으면 */
		pdev->d3cold_delay = 0;	/* [한국어] D3cold 대기를 없앤다. 아래 Function 9 가 이것을 덮어쓸 수 있다 */

	obj = acpi_evaluate_dsm_typed(handle, &pci_acpi_dsm_guid, 3,	/* [한국어] Function 9 를 부른다. 반환형이 패키지다 */
				      DSM_PCI_DEVICE_READINESS_DURATIONS, NULL,	/* [한국어] 함수 번호는 DEVICE_READINESS_DURATIONS. 그 상수의 값은 이 트리에서 확인하지 못했다 */
				      ACPI_TYPE_PACKAGE);	/* [한국어] 패키지가 아니면 NULL 이 온다 */
	if (!obj)	/* [한국어] 메서드가 없거나 형이 다르면 */
		return;	/* [한국어] 위에서 정한 값을 그대로 둔다 */

	if (obj->package.count == 5) {	/* [한국어] 규격이 정한 원소 수가 5 다. 다르면 해석하지 않는다 */
		elements = obj->package.elements;	/* [한국어] 원소 배열 */
		if (elements[0].type == ACPI_TYPE_INTEGER) {	/* [한국어] [0] 이 정수인지 확인한다 — 펌웨어 값을 믿지 않는다 */
			value = (int)elements[0].integer.value / 1000;	/* [한국어] [0] = D3cold 복귀 지연. _DSM 은 마이크로초, pdev 필드는 밀리초라 1000 으로 나눈다 */
			if (value < PCI_PM_D3COLD_WAIT)	/* [한국어] 규격 기본값보다 작을 때만 */
				pdev->d3cold_delay = value;	/* [한국어] 받아들인다. 더 큰 값은 무시한다 — 이 최적화의 목적은 줄이는 것이지 늘리는 것이 아니다 */
		}
		if (elements[3].type == ACPI_TYPE_INTEGER) {	/* [한국어] [3] 이 정수인지 확인한다 */
			value = (int)elements[3].integer.value / 1000;	/* [한국어] [3] = D3hot 복귀 지연 */
			if (value < PCI_PM_D3HOT_WAIT)	/* [한국어] 규격 기본값보다 작을 때만 */
				pdev->d3hot_delay = value;	/* [한국어] 받아들인다. 나머지 원소는 커널이 아직 쓰지 않는다 */
		}
	}
	ACPI_FREE(obj);	/* [한국어] NULL 을 받아도 안전하다 */
}

/* [한국어]
 * pci_acpi_set_external_facing - 이 Root Port 가 외부로 노출되는지 표시한다
 *
 * @dev:    판정할 장치. Root Port 가 아니면 아무것도 하지 않는다.
 * @return: 없음.
 *
 * Thunderbolt/USB4 포트처럼 사용자가 아무 장치나 꽂을 수 있는 PCIe 는
 * 신뢰 경계 밖이다. 그런 포트 아래 장치가 DMA 로 시스템 메모리를 마음대로
 * 읽으면 곧바로 보안 사고가 된다.
 *
 * 그래서 커널은 그런 포트를 표시해 두고 아래 장치들에 IOMMU 를 엄격히
 * 적용한다. 어느 포트가 그런지는 보드 설계자만 아는 정보라, 펌웨어가
 * _DSD 의 "ExternalFacingPort" 속성으로 알려 준다.
 *
 * Root Port 만 검사하는 이유는 영어 주석이 밝힌다 — 그 아래로는 전부
 * 외부로 간주되므로, 경계를 Root Port 한 곳에만 표시하면 충분하다.
 *
 * 플래그를 세우기만 하고 지우지 않는 것도 의도적이다. 속성이 없거나 0 이면
 * 손대지 않는다 — 다른 경로(devicetree 등)가 이미 세워 두었을 수 있고,
 * 보안 관련 플래그는 낮추는 쪽으로 실수하면 안 되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, companion 연결 직후.
 * 에러 경로: 속성이 없으면 조용히 끝낸다.
 *
 * 호출 체인:
 *   pci_acpi_setup() -> [pci_acpi_set_external_facing]
 *     -> device_property_read_u8()
 */
static void pci_acpi_set_external_facing(struct pci_dev *dev)
{
	u8 val;	/* [한국어] 속성 값을 받을 자리 */

	if (pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT)	/* [한국어] Root Port 가 아니면 */
		return;	/* [한국어] 경계를 표시할 자리가 아니다 */
	if (device_property_read_u8(&dev->dev, "ExternalFacingPort", &val))	/* [한국어] _DSD 의 ExternalFacingPort 속성. 없으면 0 이 아닌 값이 반환된다 */
		return;	/* [한국어] 펌웨어가 알려 주지 않았으면 손대지 않는다 */

	/*
	 * These root ports expose PCIe (including DMA) outside of the
	 * system.  Everything downstream from them is external.
	 * [한국어] 경계를 Root Port 한 곳에만 표시하면 되는 이유가
	 * 위 영어 주석에 있다 — 그 아래로는 전부 외부로 간주된다.
	 */
	if (val)	/* [한국어] 1 이면 이 포트 아래가 신뢰 경계 밖이다 */
		dev->external_facing = 1;	/* [한국어] 세우기만 하고 지우지 않는다 — 다른 경로가 이미 세워 두었을 수 있고, 보안 플래그는 낮추는 쪽으로 실수하면 안 된다 */
}

/* [한국어]
 * pci_acpi_setup - companion 이 연결된 직후의 일괄 설정
 *
 * @dev:    PCI 장치의 device.
 * @adev:   방금 연결된 ACPI companion.
 * @return: 없음.
 *
 * ACPI 코어가 companion 을 걸어 준 뒤 부르는 콜백이다(ACPI bus type 의
 * .setup). 여기서 하는 일이 이 파일 기능 대부분의 출발점이다.
 *
 * 순서대로
 *   1. 전원 전환 대기 시간 최적화(_DSM Function 8/9).
 *   2. 외부 노출 포트 표시(_DSD ExternalFacingPort).
 *   3. EDR(Error Disconnect Recover) 알림 핸들러 등록.
 *      실제 등록 코드는 pcie/edr.c:309 에 있다.
 *   4. 깨우기 알림 핸들러 등록.
 *   5. 깨우기 기능이 있으면 그것을 device 모델에 알리고 초기 상태를 정한다.
 *
 * 5번의 처리에 생각할 거리가 있다. 브리지가 D3 로 갈 수 있으면 깨우기를
 * *자동으로* 켠다. 영어 주석이 이유를 밝힌다 — 그런 브리지에는 _DSW 같은
 * 추가 메서드가 있을 수 있고, 그것을 부르려면 깨우기 경로를 활성화해 두어야
 * 한다. 전원 관리 자체도 같은 이유로 자동 활성화된다.
 *
 * 그러고 나서 acpi_pci_wakeup(pci_dev, false) 를 부르는 것이 언뜻 모순처럼
 * 보인다. device_wakeup_enable() 은 device 모델 수준의 "이 장치는 깨울 수
 * 있다" 표시이고, acpi_pci_wakeup(false) 는 ACPI GPE 를 지금은 무장하지
 * 말라는 뜻이라 층이 다르다. 초기 상태를 꺼 둔 채 시작하고, 실제 필요할 때
 * 전원 관리 코어가 켠다.
 *
 * 마지막 acpi_dev_power_up_children_with_adr() 는 브리지 아래에 ACPI 로
 * 기술된 자식이 있으면 미리 전원을 올려 두는 것이다. 그래야 열거 때
 * config space 를 읽을 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 장치 등록 중. AML 평가가 잠들 수 있다.
 * 에러 경로: 깨우기가 없으면 5번 이후를 건너뛰고 끝낸다.
 *
 * 호출 체인:
 *   acpi_bind_one() (ACPI 코어) -> [pci_acpi_setup]
 *     -> pci_acpi_optimize_delay(), pci_acpi_set_external_facing(),
 *        pci_acpi_add_edr_notifier() [pcie/edr.c:309],
 *        pci_acpi_add_pm_notifier(), acpi_pci_wakeup()
 */
void pci_acpi_setup(struct device *dev, struct acpi_device *adev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] ACPI 코어가 준 device 를 pci_dev 로 되돌린다 */

	pci_acpi_optimize_delay(pci_dev, adev->handle);	/* [한국어] _DSM 으로 전원 전환 대기 시간을 줄인다 */
	pci_acpi_set_external_facing(pci_dev);	/* [한국어] _DSD 로 외부 노출 포트를 표시한다 */
	pci_acpi_add_edr_notifier(pci_dev);	/* [한국어] EDR(Error Disconnect Recover) 알림 핸들러를 단다. 실제 구현은 pcie/edr.c:309 */

	pci_acpi_add_pm_notifier(adev, pci_dev);	/* [한국어] 깨우기 알림 핸들러를 단다 */
	if (!adev->wakeup.flags.valid)	/* [한국어] 이 장치에 깨우기 기능이 기술되어 있지 않으면 */
		return;	/* [한국어] 아래 설정은 할 것이 없다 */

	device_set_wakeup_capable(dev, true);	/* [한국어] device 모델에 "이 장치는 깨울 수 있다" 를 알린다 */
	/*
	 * For bridges that can do D3 we enable wake automatically (as
	 * we do for the power management itself in that case). The
	 * reason is that the bridge may have additional methods such as
	 * _DSW that need to be called.
	 * [한국어] 자동으로 켜는 이유가 위 영어 주석에 있다 — 그런 브리지에는
	 * _DSW 같은 추가 메서드가 있을 수 있고, 그것을 부르려면 깨우기 경로가
	 * 활성화되어 있어야 한다.
	 */
	if (pci_dev->bridge_d3)	/* [한국어] D3 로 갈 수 있는 브리지라면 */
		device_wakeup_enable(dev);	/* [한국어] 깨우기를 자동으로 켠다 */

	acpi_pci_wakeup(pci_dev, false);	/* [한국어] ACPI GPE 는 지금 무장하지 않는다. 위의 device_wakeup_enable 과 층이 다르다 — 실제 필요할 때 전원 관리 코어가 켠다 */
	acpi_device_power_add_dependent(adev, dev);	/* [한국어] 이 장치가 companion 의 전원 자원에 의존한다고 등록한다 */

	if (pci_is_bridge(pci_dev))	/* [한국어] 브리지라면 */
		acpi_dev_power_up_children_with_adr(adev);	/* [한국어] 그 아래 ACPI 로 기술된 자식들의 전원을 미리 올린다. 그래야 열거 때 config space 를 읽을 수 있다 */
}

/* [한국어]
 * pci_acpi_cleanup - companion 연결이 끊기기 전의 정리
 *
 * @dev:    PCI 장치의 device.
 * @adev:   끊길 ACPI companion.
 * @return: 없음.
 *
 * pci_acpi_setup() 의 역순 정리다. 알림 핸들러를 떼고 깨우기 설정을 되돌린다.
 *
 * 되돌리지 않는 것이 둘 있다는 점이 눈에 띈다.
 *   pci_acpi_optimize_delay() 가 고친 d3hot_delay/d3cold_delay —
 *     장치 구조체와 함께 사라지므로 되돌릴 필요가 없다.
 *   pci_acpi_set_external_facing() 이 세운 external_facing —
 *     역시 장치와 수명을 함께한다.
 * 즉 여기서 정리하는 것은 "바깥에 등록해 둔 것" 뿐이다. 그것을 남기면
 * 사라진 장치를 가리키는 콜백이 남아 위험하다.
 *
 * 깨우기 정리를 wakeup.flags.valid 로 감싼 것은 setup 쪽과 대칭을
 * 맞추기 위해서다. setup 이 그 조건에서만 설정했으므로, 같은 조건에서만
 * 되돌려야 짝이 맞는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 장치 제거 경로.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   acpi_unbind_one() (ACPI 코어) -> [pci_acpi_cleanup]
 *     -> pci_acpi_remove_edr_notifier() [pcie/edr.c:338],
 *        pci_acpi_remove_pm_notifier()
 */
void pci_acpi_cleanup(struct device *dev, struct acpi_device *adev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);	/* [한국어] ACPI 코어가 준 device 를 pci_dev 로 */

	pci_acpi_remove_edr_notifier(pci_dev);	/* [한국어] EDR 알림 핸들러를 뗀다. 사라질 장치를 가리키는 콜백을 남기면 위험하다 */
	pci_acpi_remove_pm_notifier(adev);	/* [한국어] 깨우기 알림 핸들러를 뗀다 */
	if (adev->wakeup.flags.valid) {	/* [한국어] setup 이 그 조건에서만 설정했으므로 같은 조건에서만 되돌린다 */
		acpi_device_power_remove_dependent(adev, dev);	/* [한국어] 전원 의존 등록을 푼다 */
		if (pci_dev->bridge_d3)	/* [한국어] D3 가능 브리지였다면 */
			device_wakeup_disable(dev);	/* [한국어] 자동으로 켰던 깨우기를 끈다 */

		device_set_wakeup_capable(dev, false);	/* [한국어] device 모델의 깨우기 가능 표시를 지운다 */
	}
}

/* [한국어] MSI irq_domain 의 fwnode 를 찾아 주는 콜백.
 * 인터럽트 컨트롤러 드라이버가 pci_msi_register_fwnode_provider() 로
 * 등록하고, pci_host_bridge_acpi_msi_domain() 이 그것을 부른다.
 *
 * 왜 콜백인가. PCI 코어는 어느 인터럽트 컨트롤러가 이 호스트 브리지의
 * MSI 를 처리하는지 알지 못한다. ARM64 처럼 GIC ITS 가 여럿일 수 있는
 * 플랫폼에서는 IORT 같은 펌웨어 테이블을 봐야 알 수 있는데, 그 해석은
 * 인터럽트 컨트롤러 쪽 코드의 몫이다. 그래서 방향을 뒤집어, 그쪽이
 * 콜백을 맡겨 두면 PCI 코어가 필요할 때 부른다.
 *
 * 설정자: pci_msi_register_fwnode_provider() 하나뿐.
 * 읽는 자: pci_host_bridge_acpi_msi_domain().
 * 값 범위: NULL(등록 전) 또는 유효한 함수 포인터.
 * 동기화: 없다. 부팅 초기에 한 번 등록되고 그 뒤로는 읽기만 하는 값이라
 *   상류 코드가 락을 두지 않았다. companion lookup 훅 쪽과 달리 해제
 *   경로가 없어 수명 문제가 생기지 않는다. */
static struct fwnode_handle *(*pci_msi_get_fwnode_cb)(struct device *dev);

/**
 * pci_msi_register_fwnode_provider - Register callback to retrieve fwnode
 * @fn:       Callback matching a device to a fwnode that identifies a PCI
 *            MSI domain.
 *
 * This should be called by irqchip driver, which is the parent of
 * the MSI domain to provide callback interface to query fwnode.
 */
/* [한국어]
 * pci_msi_register_fwnode_provider - MSI 도메인 fwnode 제공자를 등록한다
 *
 * @fn:     인터럽트 컨트롤러 드라이버가 주는 콜백.
 * @return: 없음. 실패할 수 없다.
 *
 * 위 kernel-doc 이 밝히듯 호출자는 MSI 도메인의 부모인 irqchip 드라이버다.
 * 대입 한 줄이 전부인 함수지만, 전역 변수를 직접 노출하지 않고 함수로
 * 감싼 덕에 다른 파일이 실수로 값을 바꾸는 일이 없다.
 *
 * 반환값이 없어 이미 등록된 것을 덮어써도 알 수 없다. companion lookup
 * 훅이 -EBUSY 로 거절하는 것과 대조적인데, 이쪽은 플랫폼에 인터럽트
 * 컨트롤러 계층이 하나뿐인 상황을 전제하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 부팅 초기 irqchip 초기화 중. 락 없음.
 *
 * 호출 체인: 이 스파스 체크아웃 안에는 호출자가 없다(전수 grep).
 *   상류에서는 drivers/irqchip/ 의 GIC 계열 드라이버가 부르는데,
 *   그 디렉터리가 이 트리에 없다.
 */
void
pci_msi_register_fwnode_provider(struct fwnode_handle *(*fn)(struct device *))
{
	pci_msi_get_fwnode_cb = fn;
}

/**
 * pci_host_bridge_acpi_msi_domain - Retrieve MSI domain of a PCI host bridge
 * @bus:      The PCI host bridge bus.
 *
 * This function uses the callback function registered by
 * pci_msi_register_fwnode_provider() to retrieve the irq_domain with
 * type DOMAIN_BUS_PCI_MSI of the specified host bridge bus.
 * This returns NULL on error or when the domain is not found.
 */
/* [한국어]
 * pci_host_bridge_acpi_msi_domain - 이 호스트 브리지의 MSI irq_domain 을 찾는다
 *
 * @bus:    호스트 브리지의 루트 버스.
 * @return: 찾은 irq_domain, 없으면 NULL.
 *
 * 두 단계다. 먼저 등록된 콜백으로 fwnode 를 얻고, 그 fwnode 로
 * DOMAIN_BUS_PCI_MSI 종류의 irq_domain 을 찾는다.
 *
 * fwnode 를 한 번 거치는 이유가 이 설계의 요점이다. irq_domain 객체는
 * 인터럽트 컨트롤러 드라이버가 만드는데, PCI 코어가 그 객체를 직접
 * 가리키면 두 서브시스템이 강하게 묶인다. fwnode 는 펌웨어 노드를
 * 가리키는 중립적 식별자라, 그것을 매개로 삼으면 결합이 느슨해진다.
 *
 * NULL 을 돌려주는 세 경우 — 콜백이 등록되지 않음, 콜백이 fwnode 를
 * 못 찾음, 그 fwnode 에 맞는 도메인이 없음 — 를 구분하지 않는다.
 * 호출자(probe.c:2386)는 어느 쪽이든 "이 버스에는 ACPI 기반 MSI 도메인이
 * 없다" 로 처리하고 다른 방법을 찾는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 루트 버스 생성 중. 락 없음.
 *
 * 호출 체인:
 *   pci_set_bus_msi_domain() [probe.c:2386]
 *     -> [pci_host_bridge_acpi_msi_domain] -> irq_find_matching_fwnode()
 */
struct irq_domain *pci_host_bridge_acpi_msi_domain(struct pci_bus *bus)
{
	struct fwnode_handle *fwnode;	/* [한국어] 콜백이 돌려줄 펌웨어 노드 */

	if (!pci_msi_get_fwnode_cb)	/* [한국어] 콜백이 등록되지 않았으면 */
		return NULL;	/* [한국어] 이 버스에는 ACPI 기반 MSI 도메인이 없다 */

	fwnode = pci_msi_get_fwnode_cb(&bus->dev);	/* [한국어] 인터럽트 컨트롤러 쪽이 IORT 같은 테이블을 보고 fwnode 를 찾아 준다 */
	if (!fwnode)	/* [한국어] 못 찾았으면 */
		return NULL;	/* [한국어] 역시 NULL */

	return irq_find_matching_fwnode(fwnode, DOMAIN_BUS_PCI_MSI);	/* [한국어] 그 fwnode 에 묶인 PCI MSI 종류의 도메인을 찾는다. fwnode 를 매개로 삼아 두 서브시스템의 결합을 느슨하게 한다 */
}

/* [한국어]
 * acpi_pci_init - 부팅 시 FADT 의 금지 플래그를 읽고 슬롯/핫플러그를 초기화한다
 *
 * @return: 항상 0. initcall 규약상 음수면 초기화 실패로 기록된다.
 *
 * arch_initcall 로 등록되어 부팅 중 한 번 실행된다. 하는 일이 둘이다.
 *
 *   1. FADT(Fixed ACPI Description Table)의 boot_flags 를 읽어 플랫폼이
 *      금지한 기능을 커널 전역에 알린다.
 *        ACPI_FADT_NO_MSI  -> pci_no_msi(). 이 칩셋에서는 MSI 가
 *          제대로 동작하지 않으니 전부 INTx 로 가라는 뜻이다.
 *        ACPI_FADT_NO_ASPM -> pcie_no_aspm() [pcie/aspm.c:3780].
 *          커널이 ASPM 레지스터를 건드리지 말라는 뜻이다. 끄라는 것이
 *          아니라 손대지 말라는 것이라는 점이 그 함수 주석에 적혀 있다.
 *      두 경우 모두 pr_info 로 이유를 남긴다 — 사용자가 "왜 MSI 가 안
 *      잡히지" 를 추적할 수 있어야 하기 때문이다.
 *
 *   2. acpi_pci_disabled 가 아니면 ACPI 슬롯과 ACPI 핫플러그를 초기화한다.
 *
 * 두 일의 순서가 중요하다. FADT 해석은 acpi_pci_disabled 여부와 무관하게
 * 항상 한다. "ACPI 로 PCI 를 열거하지 않는다" 와 "FADT 가 금지한 기능이
 * 있다" 는 별개의 사실이기 때문이다 — 예를 들어 커널이 MCFG 로 직접
 * 열거하더라도 그 칩셋의 MSI 가 고장 났다는 사실은 여전히 유효하다.
 *
 * 실행 컨텍스트: 부팅 초기(arch_initcall). 다른 스레드가 없다.
 * 에러 경로: 없다. 항상 0.
 *
 * 호출 체인:
 *   커널 initcall 처리 -> [acpi_pci_init]
 *     -> pci_no_msi(), pcie_no_aspm() [pcie/aspm.c],
 *        acpi_pci_slot_init(), acpiphp_init()
 */
static int __init acpi_pci_init(void)
{
	if (acpi_gbl_FADT.boot_flags & ACPI_FADT_NO_MSI) {	/* [한국어] FADT 가 이 시스템에서 MSI 가 동작하지 않는다고 선언했는가 */
		pr_info("ACPI FADT declares the system doesn't support MSI, so disable it\n");	/* [한국어] 이유를 남긴다 — 사용자가 왜 MSI 가 안 잡히는지 추적할 수 있어야 한다 */
		pci_no_msi();	/* [한국어] 커널 전역에서 MSI 를 끈다. 모든 장치가 INTx 로 간다 */
	}

	if (acpi_gbl_FADT.boot_flags & ACPI_FADT_NO_ASPM) {	/* [한국어] FADT 가 ASPM 을 금지했는가 */
		pr_info("ACPI FADT declares the system doesn't support PCIe ASPM, so disable it\n");	/* [한국어] 역시 이유를 남긴다 */
		pcie_no_aspm();	/* [한국어] pcie/aspm.c:3780. 끄라는 것이 아니라 커널이 손대지 말라는 뜻이다 */
	}

	if (acpi_pci_disabled)	/* [한국어] ACPI 로 PCI 를 열거하지 않는 시스템이면 */
		return 0;	/* [한국어] 아래 슬롯/핫플러그 초기화는 건너뛴다. 위 FADT 해석을 그 앞에 둔 것은, 열거 방식과 무관하게 칩셋의 결함은 여전히 유효하기 때문이다 */

	acpi_pci_slot_init();	/* [한국어] ACPI 슬롯 정보 초기화 */
	acpiphp_init();	/* [한국어] ACPI 핫플러그 초기화 */

	return 0;	/* [한국어] initcall 규약상 0 이 성공이다 */
}
arch_initcall(acpi_pci_init);	/* [한국어] 부팅 중 arch 단계에서 한 번 실행하도록 등록한다 */

#if defined(CONFIG_ARM64) || defined(CONFIG_RISCV)

/*
 * Try to assign the IRQ number when probing a new device
 */
/*
 * [한국어]
 * pcibios_alloc_irq - 장치를 발견했을 때 INTx IRQ 번호를 배정한다
 *
 * @dev:    대상 장치
 * @return: 항상 0. 실패해도 0 을 돌려준다.
 *
 * irq.c 의 __weak 훅을 ARM64/RISC-V 가 덮어쓴 판이다. ACPI 의 _PRT
 * (PCI Routing Table)를 보고 이 장치의 INTx 핀이 어느 인터럽트 컨트롤러의
 * 몇 번 입력에 연결됐는지 알아내 dev->irq 에 채운다.
 *
 * 반환값이 항상 0 인 것이 눈에 띈다. IRQ 배정에 실패해도 probe 를 막지
 * 않는다는 뜻이다. MSI/MSI-X 를 쓰는 장치는 INTx 가 없어도 잘 동작하므로,
 * 여기서 실패했다고 장치를 포기할 이유가 없다.
 *
 * x86 에는 이 훅의 덮어쓰기가 없다. 그쪽은 pci_enable_device() 시점에
 * 아키텍처 코드가 따로 처리한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 드라이버 바인딩 직전에 불린다.
 * 호출자: pci-driver.c 의 pci_device_probe().
 */
int pcibios_alloc_irq(struct pci_dev *dev)
{
	if (!acpi_disabled)
		acpi_pci_irq_enable(dev);

	return 0;
}

/* [한국어] ACPI 기반 루트 브리지 하나의 정보. 공통 부분에 ECAM 창
 * 포인터를 덧붙인 형태다. common 을 첫 필드로 두어 container_of 로
 * 서로를 오갈 수 있게 한 것이 관용적인 확장 방식이다. */
struct acpi_pci_generic_root_info {
	struct acpi_pci_root_info	common;
	/* [한국어] 이 루트 브리지의 ECAM 창.
	 * 설정자: pci_acpi_setup_ecam_mapping() 이 MCFG 를 보고 만든다.
	 * 읽는 자: 이 도메인의 모든 config 접근이 bus->sysdata 를 통해
	 *   이 포인터에 닿는다(acpi_pci_bus_find_domain_nr 참고).
	 * 값 범위: NULL 이 아닌 유효한 창. NULL 이면 루트 브리지 등록이 실패한다.
	 * 동기화: 등록 시 한 번 설정되고 해제 시까지 바뀌지 않는다. */
	struct pci_config_window	*cfg;	/* config space mapping */
};

/* [한국어]
 * acpi_pci_bus_find_domain_nr - 이 버스의 세그먼트(도메인) 번호를 돌려준다
 *
 * @bus:    질의 대상 버스.
 * @return: ACPI 가 기술한 PCI 세그먼트 번호.
 *
 * 큰 시스템에서는 버스 번호 256개(8비트)만으로는 부족해 "세그먼트" 라는
 * 상위 구분을 둔다. ACPI 는 _SEG 로 그것을 기술하고, 커널은 그것을
 * 도메인 번호로 쓴다. sysfs 경로의 0000:00:1f.2 에서 앞의 0000 이 그것이다.
 *
 * 이 함수의 세 줄이 포인터를 세 번 건너뛴다.
 *   bus->sysdata          -> pci_config_window (ECAM 창)
 *   cfg->parent           -> device (= ACPI 장치의 device)
 *   acpi_driver_data(adev) -> acpi_pci_root
 * 그 사슬이 성립하는 것은 pci_acpi_scan_root() 가 ri->cfg 를 sysdata 로
 * 넘겨 루트 버스를 만들었기 때문이다. 즉 이 함수는 그 함수와 짝을 이룬다.
 *
 * 검사가 하나도 없다는 점이 눈에 띈다. ARM64/RISC-V 의 ACPI 경로에서만
 * 쓰이고, 그 경로에서는 위 사슬이 반드시 성립한다는 전제 위에 서 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 버스 생성 중. 락 없음.
 *
 * 호출 체인:
 *   pci_bus_find_domain_nr() [pci.c:13864] -> [acpi_pci_bus_find_domain_nr]
 */
int acpi_pci_bus_find_domain_nr(struct pci_bus *bus)
{
	struct pci_config_window *cfg = bus->sysdata;	/* [한국어] 루트 버스의 sysdata 는 pci_acpi_scan_root() 이 넣어 둔 ECAM 창이다 */
	struct acpi_device *adev = to_acpi_device(cfg->parent);	/* [한국어] 그 창의 부모 device 가 곧 ACPI 장치의 device 다 */
	struct acpi_pci_root *root = acpi_driver_data(adev);	/* [한국어] 그 ACPI 장치에 매달린 acpi_pci_root */

	return root->segment;	/* [한국어] _SEG 가 알려 준 세그먼트 번호. sysfs 경로 0000:00:1f.2 의 앞 0000 이 이것이다 */
}

/* [한국어]
 * pcibios_root_bridge_prepare - 루트 브리지 등록 직전의 ARM64/RISC-V 훅
 *
 * @bridge: 곧 등록될 호스트 브리지.
 * @return: 0. 이 판은 실패하지 않는다.
 *
 * probe.c:7741 에 __weak 기본 판이 있고, ARM64/RISC-V 가 그것을 이 함수로
 * 덮어쓴다. 등록 직전에 두 가지를 채워 넣는다.
 *
 *   1. 호스트 브리지 device 에 ACPI companion 을 건다. 이것이 있어야
 *      그 아래 장치들의 companion 찾기(acpi_pci_find_companion)가
 *      부모부터 시작할 수 있다.
 *   2. 버스 device 의 NUMA 노드를 ACPI 가 알려 준 값으로 정한다.
 *      이 값이 이후 그 아래 장치들의 메모리 할당 위치를 좌우한다.
 *
 * Hyper-V 예외가 이 함수의 특이점이다. 영어 주석대로 그 가상화 환경에는
 * 루트 브리지에 대응하는 ACPI 장치가 없어 cfg->parent 가 NULL 이다.
 * 그대로 to_acpi_device() 를 부르면 엉뚱한 포인터가 나오므로, NULL 을
 * 그대로 companion 으로 건다. ACPI_COMPANION_SET 은 NULL 을 받아도 되고,
 * acpi_device_handle(NULL) 도 NULL 을 돌려주어 set_dev_node 가
 * NUMA_NO_NODE 를 받게 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 루트 브리지 등록 직전.
 * 에러 경로: acpi_disabled 면 아무것도 하지 않고 0.
 *
 * 호출 체인:
 *   pci_register_host_bridge() [probe.c:2608] -> [pcibios_root_bridge_prepare]
 *     -> ACPI_COMPANION_SET(), acpi_get_node()
 */
int pcibios_root_bridge_prepare(struct pci_host_bridge *bridge)
{
	struct pci_config_window *cfg;	/* [한국어] 루트 버스의 ECAM 창 */
	struct acpi_device *adev;	/* [한국어] 걸어 줄 ACPI companion */
	struct device *bus_dev;	/* [한국어] NUMA 노드를 설정할 버스 device */

	if (acpi_disabled)	/* [한국어] ACPI 를 아예 쓰지 않는 부팅이면 */
		return 0;	/* [한국어] 채워 넣을 정보가 없다 */

	cfg = bridge->bus->sysdata;	/* [한국어] 컨트롤러 드라이버가 넣어 둔 ECAM 창 */

	/*
	 * On Hyper-V there is no corresponding ACPI device for a root bridge,
	 * therefore ->parent is set as NULL by the driver. And set 'adev' as
	 * NULL in this case because there is no proper ACPI device.
	 * [한국어] 그 경우 cfg->parent 가 NULL 이라, 그대로 to_acpi_device() 를
	 * 부르면 엉뚱한 포인터가 나온다. NULL 을 그대로 companion 으로 걸면
	 * 아래 acpi_device_handle(NULL) 도 NULL 을 돌려주어 무해하게 흘러간다.
	 */
	if (!cfg->parent)	/* [한국어] Hyper-V 처럼 루트 브리지에 대응하는 ACPI 장치가 없는 경우 */
		adev = NULL;	/* [한국어] NULL 을 그대로 companion 으로 건다 */
	else
		adev = to_acpi_device(cfg->parent);	/* [한국어] 보통은 그 device 를 ACPI 장치로 되돌린다 */

	bus_dev = &bridge->bus->dev;	/* [한국어] NUMA 노드는 버스 device 에 설정한다. 브리지 device 가 아니다 */

	ACPI_COMPANION_SET(&bridge->dev, adev);	/* [한국어] 이것이 있어야 그 아래 장치들의 companion 찾기가 부모부터 시작할 수 있다 */
	set_dev_node(bus_dev, acpi_get_node(acpi_device_handle(adev)));	/* [한국어] ACPI 가 알려 준 NUMA 노드를 설정한다. adev 가 NULL 이면 handle 도 NULL 이 되어 무해하게 흘러간다 */

	return 0;	/* [한국어] 이 판은 실패하지 않는다 */
}

/* [한국어]
 * pci_acpi_root_prepare_resources - 루트 브리지 자원 중 "창" 만 남긴다
 *
 * @ci:     ACPI 가 채워 줄 루트 브리지 정보(자원 목록 포함).
 * @return: acpi_pci_probe_root_resources() 의 반환값.
 *
 * acpi_pci_root_ops 의 .prepare_resources 콜백이다. ACPI 코어가 _CRS 로
 * 자원 목록을 채운 뒤, 그 목록을 손볼 기회를 이 콜백에 준다.
 *
 * 여기서 하는 일은 IORESOURCE_WINDOW 가 아닌 항목을 전부 버리는 것이다.
 * "창(window)" 은 브리지가 하위로 전달하는 주소 범위를 뜻하고, 창이 아닌
 * 자원은 브리지 자신이 소비하는 것이다(컨트롤러 레지스터 등). 하위 장치의
 * BAR 를 배치할 때 필요한 것은 창뿐이라, 나머지를 남겨 두면 오히려
 * 할당기가 혼동한다.
 *
 * resource_list_for_each_entry_safe 를 쓰는 이유는 순회 중 항목을
 * 지우기 때문이다. _safe 판은 다음 항목을 미리 잡아 두어, 현재 항목을
 * 해제해도 순회가 깨지지 않는다.
 *
 * status 를 그대로 돌려주는 것도 눈여겨볼 만하다. 걸러 내기가 실패할 수
 * 없으므로, 이 함수의 성패는 곧 자원 조회의 성패다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 루트 버스 생성 중.
 *
 * 호출 체인:
 *   pci_acpi_scan_root() -> acpi_pci_root_create()
 *     -> root_ops->prepare_resources = [pci_acpi_root_prepare_resources]
 *        -> acpi_pci_probe_root_resources()
 */
static int pci_acpi_root_prepare_resources(struct acpi_pci_root_info *ci)
{
	struct resource_entry *entry, *tmp;	/* [한국어] entry = 현재 항목, tmp = 다음 항목을 미리 잡아 두는 자리 */
	int status;	/* [한국어] 자원 조회 결과 */

	status = acpi_pci_probe_root_resources(ci);	/* [한국어] _CRS 를 평가해 자원 목록을 채운다 */
	resource_list_for_each_entry_safe(entry, tmp, &ci->resources) {	/* [한국어] 순회 중 항목을 지우므로 _safe 판을 쓴다. 다음 항목을 미리 잡아 두어 현재 항목을 해제해도 순회가 깨지지 않는다 */
		if (!(entry->res->flags & IORESOURCE_WINDOW))	/* [한국어] 창(브리지가 하위로 전달하는 주소 범위)이 아니면 — 브리지 자신이 소비하는 자원이다 */
			resource_list_destroy_entry(entry);	/* [한국어] 버린다. 남겨 두면 하위 BAR 를 배치할 때 할당기가 혼동한다 */
	}
	return status;	/* [한국어] 걸러 내기는 실패할 수 없으므로 이 함수의 성패는 곧 자원 조회의 성패다 */
}

/*
 * Lookup the bus range for the domain in MCFG, and set up config space
 * mapping.
 */
/*
 * [한국어]
 * pci_acpi_setup_ecam_mapping - MCFG 테이블을 보고 ECAM 창을 만든다
 *
 * @root:   ACPI 가 기술한 루트 브리지
 * @return: 만들어진 struct pci_config_window, 실패하면 NULL.
 *
 * MCFG 는 ACPI 테이블 중 하나로, "이 세그먼트의 버스 N~M 번은 물리 주소
 * X 부터 시작하는 ECAM 창으로 접근하라" 는 정보를 담는다. 이 함수가 그것을
 * 조회해 ecam.c 의 pci_ecam_create() 로 실제 매핑을 만든다.
 *
 * ACPI 시스템에서 config 접근이 시작되는 지점이다. 이 매핑이 없으면
 * 어떤 장치의 config space 도 읽을 수 없어 열거 자체가 불가능하다.
 *
 * 버스 범위를 확인하는 이유: ACPI 가 기술한 버스 범위와 MCFG 가 커버하는
 * 범위가 어긋날 수 있다. 그 경우 겹치는 부분만 쓰고 나머지는 포기한다 —
 * 매핑되지 않은 버스에 접근하면 시스템이 죽기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 부팅 중 루트 브리지 등록 시.
 * 호출자: pci_acpi_scan_root().
 */
static struct pci_config_window *
pci_acpi_setup_ecam_mapping(struct acpi_pci_root *root)
{
	struct device *dev = &root->device->dev;	/* [한국어] 오류를 보고할 device */
	struct resource *bus_res = &root->secondary;	/* [한국어] 이 루트 브리지가 관할하는 버스 번호 범위 */
	u16 seg = root->segment;	/* [한국어] PCI 세그먼트 번호. 로그에만 쓴다 */
	const struct pci_ecam_ops *ecam_ops;	/* [한국어] ECAM 접근 방법(정렬 제약, 우회 등이 구현마다 다르다) */
	struct resource cfgres;	/* [한국어] MCFG 가 알려 준 ECAM 물리 주소 범위 */
	struct acpi_device *adev;	/* [한국어] 그 영역을 예약해 둔 ACPI 장치 */
	struct pci_config_window *cfg;	/* [한국어] 만들어질 ECAM 창 */
	int ret;	/* [한국어] 조회 결과 */

	ret = pci_mcfg_lookup(root, &cfgres, &ecam_ops);	/* [한국어] MCFG 테이블에서 이 세그먼트/버스 범위에 맞는 ECAM 창을 찾는다 */
	if (ret) {	/* [한국어] 못 찾았으면 */
		dev_err(dev, "%04x:%pR ECAM region not found\n", seg, bus_res);	/* [한국어] 어느 세그먼트의 어느 버스 범위였는지 남긴다. %pR 은 resource 를 사람이 읽는 형식으로 찍는 커널 확장 서식 */
		return NULL;	/* [한국어] config 접근이 불가능하므로 이 루트 브리지는 쓸 수 없다 */
	}

	adev = acpi_resource_consumer(&cfgres);	/* [한국어] 그 물리 영역을 자원으로 예약해 둔 ACPI 장치를 찾는다 */
	if (adev)	/* [한국어] 예약되어 있으면 */
		dev_info(dev, "ECAM area %pR reserved by %s\n", &cfgres,	/* [한국어] 누가 예약했는지 알린다 */
			 dev_name(&adev->dev));	/* [한국어] 그 장치 이름 */
	else	/* [한국어] 예약되어 있지 않으면 */
		dev_warn(dev, FW_BUG "ECAM area %pR not reserved in ACPI namespace\n",	/* [한국어] FW_BUG 접두사로 펌웨어 결함임을 표시한다. 예약 없이 쓰면 다른 드라이버가 같은 영역을 잡을 수 있다 */
			 &cfgres);	/* [한국어] 어느 영역인지 */

	cfg = pci_ecam_create(dev, &cfgres, bus_res, ecam_ops);	/* [한국어] 실제 매핑을 만든다. 이 창이 있어야 config space 를 읽을 수 있다 */
	if (IS_ERR(cfg)) {	/* [한국어] IS_ERR 로 검사하는 것은 이 함수가 오류를 포인터에 실어 돌려주기 때문이다 */
		dev_err(dev, "%04x:%pR error %ld mapping ECAM\n", seg, bus_res,	/* [한국어] 실패 이유까지 남긴다 */
			PTR_ERR(cfg));	/* [한국어] 오류 포인터를 정수 오류 코드로 되돌린다 */
		return NULL;	/* [한국어] 호출자에게는 NULL 로 통일해 알린다 */
	}

	return cfg;	/* [한국어] 만들어진 창 */
}

/* release_info: free resources allocated by init_info */
/* [한국어]
 * pci_acpi_generic_release_info - 루트 브리지 정보와 ECAM 창을 해제한다
 *
 * @ci:     해제할 공통 정보 구조체.
 * @return: 없음.
 *
 * acpi_pci_root_ops 의 .release_info 콜백이다. 루트 브리지가 사라질 때
 * ACPI 코어가 부른다.
 *
 * container_of 로 바깥 구조체를 되찾는 것이 이 함수의 요점이다.
 * struct acpi_pci_generic_root_info 는 common 을 첫 필드로 두어 공통
 * 구조체를 감싸고 있고, 콜백은 그 안쪽 주소만 받는다. container_of 가
 * 오프셋을 빼서 바깥 주소를 복원해 준다. 첫 필드라 오프셋이 0 이지만,
 * 그것에 기대지 않고 매크로를 쓰는 것이 관례다 — 나중에 필드가 앞에
 * 추가되어도 코드가 그대로 맞는다.
 *
 * 해제 순서가 셋이다. ECAM 창 -> ops -> 바깥 구조체.
 * ops 를 ci->ops 로 지우는 것은 pci_acpi_scan_root() 가 root_ops 를
 * 따로 kzalloc 해서 넘겼기 때문이다. 그 포인터가 공통 구조체 안에
 * 보관되어 있어 여기서 되찾아 지운다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 루트 브리지 해제 경로.
 *
 * 호출 체인:
 *   (ACPI 코어의 루트 브리지 해제) -> root_ops->release_info
 *     = [pci_acpi_generic_release_info] -> pci_ecam_free(), kfree()
 */
static void pci_acpi_generic_release_info(struct acpi_pci_root_info *ci)
{
	struct acpi_pci_generic_root_info *ri;	/* [한국어] 바깥 구조체를 담을 자리 */

	ri = container_of(ci, struct acpi_pci_generic_root_info, common);	/* [한국어] 공통 구조체 주소에서 오프셋을 빼 바깥 구조체 주소를 복원한다. 첫 필드라 오프셋은 0 이지만, 나중에 필드가 앞에 추가되어도 맞도록 매크로를 쓴다 */
	pci_ecam_free(ri->cfg);	/* [한국어] ECAM 매핑을 해제한다 */
	kfree(ci->ops);	/* [한국어] pci_acpi_scan_root() 이 따로 할당해 넘긴 ops 구조체 */
	kfree(ri);	/* [한국어] 바깥 구조체 자체 */
}

/* Interface called from ACPI code to setup PCI host controller */
/* [한국어]
 * pci_acpi_scan_root - ACPI 루트 브리지 하나를 열거해 버스 트리를 만든다
 *
 * @root:   ACPI 가 기술한 루트 브리지.
 * @return: 만들어진 루트 버스, 실패하면 NULL.
 *
 * ARM64/RISC-V 에서 PCI 열거가 시작되는 지점이다. x86 은 아키텍처 코드가
 * 같은 일을 따로 한다.
 *
 * 진행 순서
 *   1. 루트 브리지 정보 구조체(ri)와 ops 구조체를 할당한다.
 *   2. MCFG 를 조회해 ECAM 창을 만든다. 이것이 없으면 어떤 config 접근도
 *      할 수 없으므로 여기서 실패하면 끝이다.
 *   3. ops 에 콜백 셋을 채우고 acpi_pci_root_create() 로 루트 버스를 만든다.
 *      pci_ops 를 ri->cfg->ops->pci_ops 에서 가져오는 것은, ECAM 구현마다
 *      config 접근 방법이 조금씩 다르기 때문이다(정렬 제약, 우회 등).
 *   4. 펌웨어 배치를 보존해야 하면 지금 자원을 "청구" 한다.
 *      pci_acpi_preserve_config() 의 결과가 host->preserve_config 에 담겨 있다.
 *   5. 배정되지 않은 자원을 배정한다. 4번을 하지 않았다면 여기서 전부
 *      다시 배정된다(영어 주석이 그 관계를 밝힌다).
 *   6. 하위 버스마다 PCIe 설정(MPS/MRRS 등)을 맞춘다.
 *
 * 오류 처리에 상류 코드의 특징이 하나 보인다. 3번 이전까지는 실패할 때
 * 할당한 것을 손으로 되돌리지만, acpi_pci_root_create() 가 실패하면
 * ri 와 root_ops 를 해제하지 않고 NULL 만 돌려준다. 그 함수가 실패
 * 경로에서 release_info 콜백을 부르도록 되어 있다면 이중 해제를 피하는
 * 것이 맞지만, 이 트리만으로는 그 함수의 구현을 확인할 수 없어
 * (drivers/acpi/ 가 없다) 단정하지 못한다. 코드는 그대로 두고 관찰만 적는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 부팅 또는 루트 브리지 핫플러그 중.
 * 에러 경로: 세 번의 할당/생성 실패 지점 모두 NULL 을 돌려준다.
 *
 * 호출 체인:
 *   (drivers/acpi/pci_root.c — 이 트리에 없음) -> [pci_acpi_scan_root]
 *     -> pci_acpi_setup_ecam_mapping(), acpi_pci_root_create(),
 *        pci_bus_claim_resources(), pci_assign_unassigned_root_bus_resources(),
 *        pcie_bus_configure_settings()
 */
struct pci_bus *pci_acpi_scan_root(struct acpi_pci_root *root)
{
	struct acpi_pci_generic_root_info *ri;	/* [한국어] 루트 브리지 정보 + ECAM 창을 담을 구조체 */
	struct pci_bus *bus, *child;	/* [한국어] bus = 만들어질 루트 버스, child = 하위 버스 순회 커서 */
	struct acpi_pci_root_ops *root_ops;	/* [한국어] ACPI 코어에 넘길 콜백 묶음 */
	struct pci_host_bridge *host;	/* [한국어] preserve_config 를 읽을 호스트 브리지 */

	ri = kzalloc_obj(*ri);	/* [한국어] 0 으로 채워 할당한다 */
	if (!ri)	/* [한국어] 메모리가 없으면 */
		return NULL;	/* [한국어] 열거를 포기한다 */

	root_ops = kzalloc_obj(*root_ops);	/* [한국어] 콜백 묶음도 따로 할당한다. 루트 브리지마다 별도 인스턴스가 필요하다 */
	if (!root_ops) {	/* [한국어] 실패했으면 */
		kfree(ri);	/* [한국어] 앞서 잡은 것을 되돌리고 */
		return NULL;	/* [한국어] 포기한다 */
	}

	ri->cfg = pci_acpi_setup_ecam_mapping(root);	/* [한국어] MCFG 로 ECAM 창을 만든다. 이것이 없으면 어떤 config 접근도 할 수 없다 */
	if (!ri->cfg) {	/* [한국어] 실패했으면 */
		kfree(ri);	/* [한국어] 둘 다 되돌리고 */
		kfree(root_ops);	/* [한국어] 포기한다 */
		return NULL;
	}

	root_ops->release_info = pci_acpi_generic_release_info;	/* [한국어] 해제 콜백 */
	root_ops->prepare_resources = pci_acpi_root_prepare_resources;	/* [한국어] 자원 준비 콜백 */
	root_ops->pci_ops = (struct pci_ops *)&ri->cfg->ops->pci_ops;	/* [한국어] config 접근 방법. ECAM 구현마다 다르므로 창에서 가져온다 */
	bus = acpi_pci_root_create(root, root_ops, &ri->common, ri->cfg);	/* [한국어] 루트 버스를 만든다. ri->cfg 가 bus->sysdata 가 되어 acpi_pci_bus_find_domain_nr() 의 포인터 사슬이 성립한다 */
	if (!bus)	/* [한국어] 실패했으면 */
		return NULL;	/* [한국어] ri 와 root_ops 를 해제하지 않고 NULL 만 돌려준다. 그 함수가 실패 경로에서 release_info 를 부르는지는 이 트리에서 확인할 수 없어(drivers/acpi/ 가 없다) 단정하지 않는다 — 코드는 그대로 두고 관찰만 적는다 */

	/* If we must preserve the resource configuration, claim now */
	host = pci_find_host_bridge(bus);	/* [한국어] 펌웨어 배치를 보존할지 판정한 결과가 여기 담겨 있다 */
	if (host->preserve_config)	/* [한국어] 보존해야 하면 */
		pci_bus_claim_resources(bus);	/* [한국어] 지금 자원을 청구해 커널이 그대로 쓰게 만든다 */

	/*
	 * Assign whatever was left unassigned. If we didn't claim above,
	 * this will reassign everything.
	 * [한국어] 위에서 청구(claim)를 하지 않았다면 이 호출이 사실상
	 * 전부를 다시 배정한다 — 영어 주석이 그 관계를 밝힌다.
	 */
	pci_assign_unassigned_root_bus_resources(bus);	/* [한국어] 배정되지 않은 것을 배정한다 */

	list_for_each_entry(child, &bus->children, node)	/* [한국어] 하위 버스마다 */
		pcie_bus_configure_settings(child);	/* [한국어] MPS/MRRS 같은 PCIe 설정을 맞춘다. 경로 전체가 일관되어야 하므로 열거가 끝난 뒤에 한다 */

	return bus;	/* [한국어] 만들어진 루트 버스 */
}

/* [한국어]
 * pcibios_add_bus - 버스가 등록된 직후의 ARM64/RISC-V 훅
 *
 * @bus:    새로 등록된 버스.
 * @return: 없음.
 *
 * probe.c:7771 의 __weak 기본 판(아무것도 하지 않는다)을 ARM64/RISC-V 가
 * 이 함수로 덮어쓴다. 하는 일은 acpi_pci_add_bus() 를 부르는 것 하나다.
 *
 * 한 줄짜리 함수를 따로 두는 이유는 이름 공간에 있다. pcibios_ 접두사는
 * "아키텍처가 덮어쓸 수 있는 훅" 을 뜻하는 커널 관례이고,
 * acpi_pci_add_bus() 는 ACPI 기능의 이름이다. 둘을 나눠 두면
 * acpi_pci_add_bus() 를 다른 경로에서도 부를 수 있고, 이 훅이 나중에
 * ACPI 말고 다른 일을 더 하게 되어도 호출 자리를 고치지 않아도 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 버스 등록 직후.
 *
 * 호출 체인:
 *   pci_create_root_bus() [probe.c:2672], pci_add_new_bus() [probe.c:3071]
 *     -> [pcibios_add_bus] -> acpi_pci_add_bus()
 */
void pcibios_add_bus(struct pci_bus *bus)
{
	acpi_pci_add_bus(bus);
}

/* [한국어]
 * pcibios_remove_bus - 버스가 제거되기 직전의 ARM64/RISC-V 훅
 *
 * @bus:    없어질 버스.
 * @return: 없음.
 *
 * 위 pcibios_add_bus() 의 짝이다. probe.c:7791 의 __weak 판을 덮어쓰고
 * acpi_pci_remove_bus() 로 넘긴다.
 *
 * "제거되기 직전" 이라는 시점이 중요하다. 아직 버스 구조체가 살아 있어야
 * acpiphp 가 그 버스에 매달린 슬롯 객체를 찾아 정리할 수 있다.
 * probe.c:7802 의 주석이 그 호출 시점을 밝힌다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 버스 제거 경로.
 *
 * 호출 체인:
 *   pci_remove_bus() [remove.c:183] -> [pcibios_remove_bus]
 *     -> acpi_pci_remove_bus()
 */
void pcibios_remove_bus(struct pci_bus *bus)
{
	acpi_pci_remove_bus(bus);
}

#endif
