// SPDX-License-Identifier: GPL-2.0+
/*
 * PCI Hot Plug Controller Driver for RPA-compliant PPC64 platform.
 * Copyright (C) 2003 Linda Xie <lxie@us.ibm.com>
 *
 * All rights reserved.
 *
 * Send feedback to <lxie@us.ibm.com>
 *
 */
/*
 * [한국어 설명] RPA PCI 핫플러그 드라이버의 본체 (rpaphp_core.c)
 *
 * === 파일의 역할 ===
 * IBM POWER 의 RPA(RS/6000 Platform Architecture) 규격을 따르는 PPC64 플랫폼에서
 * PCI 슬롯 단위 핫플러그를 구현하는 드라이버의 중심 파일이다. 같은 디렉터리의
 * rpaphp_slot.c(슬롯 구조체의 생성·등록·해제)와 rpaphp_pci.c(RTAS 센서 조회와
 * PCI 열거)가 부품을 대고, 이 파일이 그것들을 엮어 모듈로 만든다.
 * 하는 일은 넷이다. (1) 모듈 진입점 — 부팅 시 디바이스 트리에서 이름이 "pci" 인
 * 노드를 모두 훑어 핫플러그 가능한 슬롯을 찾아 등록한다. (2) DRC 속성 해석 —
 * 펌웨어가 디바이스 트리에 심어 둔 DRC(Dynamic Reconfiguration Connector) 정보를
 * 읽어 슬롯의 인덱스·이름·종류·전원 도메인을 뽑아낸다. 이 형식이 옛 판(v1)과
 * 새 판(v2) 둘로 나뉘어 있어 같은 일을 하는 함수가 두 벌씩 있다. (3) 공용
 * 핫플러그 코어가 요구하는 sysfs 콜백 여섯 개를 구현한다. 그 대부분은 하드웨어
 * 레지스터가 아니라 RTAS(Run-Time Abstraction Services) 펌웨어 호출로 처리된다.
 * (4) 슬롯을 켜고 끄는 실제 동작 — 카드가 있으면 PCI 열거를, 뺄 때는 장치 제거를
 * 수행한다.
 * 이 드라이버가 다른 플랫폼의 핫플러그 드라이버와 결정적으로 다른 점은 MMIO
 * 레지스터를 하나도 만지지 않는다는 것이다. LED 도, 전원 상태도, 카드 유무
 * 감지도 전부 펌웨어에 물어본다. 슬롯 정보의 출처도 config space 가 아니라
 * 디바이스 트리다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 위에는 sysfs 슬롯 인터페이스를 제공하는 공용 코어 pci_hotplug_core.c 가 있고,
 * 아래에는 RTAS 펌웨어가 있다. 이 파일은 그 사이의 어댑터다.
 * 진입 경로가 셋이다. 첫째, 모듈 적재 시 rpaphp_init() 이 디바이스 트리를 훑어
 * 슬롯을 등록한다. 둘째, 사용자가 슬롯의 sysfs 파일을 건드리면 공용 코어가
 * 이 파일의 콜백 여섯 개(rpaphp_hotplug_slot_ops)를 되부른다. 셋째, 상위 계층인
 * DLPAR(rpadlpar_core.c)이 슬롯을 파티션에 붙일 때 rpaphp_add_slot() 을,
 * 슬롯을 떼어 낼 때 rpaphp_check_drc_props() 를 직접 부른다 — 이 파일이
 * EXPORT_SYMBOL_GPL 로 내보내는 것이 그 둘과 전역 슬롯 리스트다.
 * 즉 계층 관계는 이렇다: rpadlpar(파티션에 슬롯을 넣고 빼는 상위 개념)
 *   → rpaphp(그 슬롯 하나의 핫플러그) → pci_hotplug_core(sysfs 노출).
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. RTAS 호출과 PCI 열거가 잠들 수 있어
 * 인터럽트 문맥에서는 어느 함수도 부를 수 없다. 이 파일에는 인터럽트 핸들러도
 * 커널 스레드도 없다 — 이벤트를 감지하는 주체가 OS 가 아니라 펌웨어와
 * 하이퍼바이저이기 때문이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: linux/pci_hotplug.h 의 struct hotplug_slot 과 hotplug_slot_ops.
 * 슬롯 등록·해제는 rpaphp_slot.c 가 pci_hp_register()/pci_hp_deregister() 로 한다.
 * 옆쪽: rpaphp.h 가 struct slot 과 로그 매크로, RTAS 토큰 상수를 공급하고,
 * rpaphp_slot.c 가 alloc_slot_struct()/dealloc_slot_struct()/
 * rpaphp_register_slot()/rpaphp_deregister_slot() 을, rpaphp_pci.c 가
 * rpaphp_get_sensor_state()/rpaphp_enable_slot() 을 구현한다.
 * 아래쪽: RTAS 펌웨어 호출(rtas_set_indicator, rtas_get_power_level), EEH
 * 서브시스템(pseries_eeh_init_edev_recursive), PowerPC 의 PCI_DN 매크로,
 * 그리고 핫플러그 전용 열거 경로 pci_hp_add_devices()/pci_hp_remove_devices().
 * 데이터 흐름: 디바이스 트리의 ibm,drc- 로 시작하는 속성들 → 이 파일의 파싱 →
 * struct slot → 공용 코어의 sysfs 슬롯 → 사용자 조작 → RTAS 호출 → 펌웨어.
 * 반대 방향으로는 RTAS 센서 값이 slot->state 로, PCI 열거 결과가 slot->bus 로
 * 들어온다.
 * 공유 상태: 전역 리스트 rpaphp_slot_head 와 디버그 플래그 rpaphp_debug 를
 * 이 파일이 소유하고 둘 다 내보낸다. 리스트를 지키는 락이 없는데, 슬롯 추가·제거가
 * 직렬화된다는 전제 위에 있다 — DLPAR 쪽은 자기 뮤텍스로 그것을 보장하지만
 * 이 파일의 부팅 경로와 sysfs 경로 사이에는 그런 장치가 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * - rpaphp_init() / rpaphp_exit(): 모듈 진입·종료. 전자는 DT 를 훑어 슬롯을
 *   등록하고, 후자는 남은 슬롯을 모두 걷어낸다.
 * - rpaphp_add_slot(): 노드 하나를 받아 슬롯으로 등록한다. DRC 형식이 v1 인지
 *   v2 인지 보고 갈라진다. 부팅 경로와 DLPAR 경로가 공유하는 진입점이다.
 * - rpaphp_check_drc_props(): 노드가 주어진 DRC 이름/종류와 맞는지 확인한다.
 *   DLPAR 이 "이 이름의 슬롯이 어느 노드인가" 를 찾을 때 이 함수를 쓴다.
 * - get_children_props() / rpaphp_check_drc_props_v1() / _v2() /
 *   rpaphp_drc_add_slot() / rpaphp_drc_info_add_slot(): DRC 속성의 두 형식을
 *   각각 다루는 짝. v1 은 네 개의 병렬 배열, v2 는 압축된 한 속성이다.
 * - enable_slot() / disable_slot(): 슬롯을 켜고 끈다. 실제 PCI 열거와 제거가
 *   여기서 일어나며, 이 파일에서 가장 무거운 동작이다.
 * - set_attention_status() / get_power_status() / get_attention_status() /
 *   get_adapter_status(): 나머지 sysfs 콜백. 앞의 둘은 RTAS 호출, 뒤의 둘은
 *   기억해 둔 값이나 센서 조회로 답한다.
 * - get_max_bus_speed(): 슬롯 종류 코드를 PCI 버스 속도로 옮기는 표.
 *
 * === DRC 속성의 두 형식 ===
 * 이 파일 코드의 절반이 이 차이를 다루는 데 쓰인다.
 *  - v1: 부모 노드에 네 개의 병렬 배열이 놓인다 — ibm,drc-indexes,
 *    ibm,drc-names, ibm,drc-types, ibm,drc-power-domains. 첫 셀이 개수이고
 *    그 뒤에 항목이 이어지는데, 이름과 종류는 고정폭이 아니라 널로 끝나는
 *    문자열이 줄줄이 붙은 형태라 i 번째 항목을 보려면 앞에서부터 문자열 길이만큼
 *    포인터를 밀어야 한다. 이 파일에 strlen 으로 포인터를 전진시키는 루프가
 *    두 곳 있는 이유가 그것이다.
 *  - v2: ibm,drc-info 속성 하나에 "인덱스 구간 + 이름 접두사 + 접미 시작 번호"
 *    형태로 압축되어 있다. 슬롯 이름은 저장된 것이 아니라 접두사와 번호를
 *    이어 붙여 그 자리에서 만들어 낸다(sprintf 가 쓰이는 이유).
 * 어느 형식인지는 ibm,drc-info 속성이 있는지로 판별하며, 그 분기가
 * rpaphp_add_slot() 과 rpaphp_check_drc_props() 두 곳에 있다.
 *
 * === 이 트리에서 확인할 수 없는 것 ===
 * 이 저장소는 sparse checkout 이라 drivers/{block,nvme,pci,s390,vfio} 만 있고
 * arch/powerpc 가 없다. 따라서 RTAS 호출의 구현과 규격 문서, asm/ 아래 헤더들
 * (firmware.h, eeh.h, rtas.h, pci-bridge.h, prom.h)의 내용, PCI_DN 매크로의
 * 정의, pseries_eeh_init_edev_recursive() 와 pci_hp_add_devices() 의 구현,
 * of_read_drc_info_cell() 과 struct of_drc_info 의 정의는 모두 확인 대상 밖이다.
 * 그런 이름들은 아래에서 전부 '호출 자리의 쓰임새' 로만 설명했고, 펌웨어가
 * 실제로 무엇을 하는지는 단정하지 않았다. DRC 속성의 규격 문서(RPA/PAPR)도
 * 트리에 없으므로, 위 두 형식 설명 역시 이 파일의 파싱 코드에서 역으로 읽어
 * 낸 것이다.
 */

/* [한국어] 범용 커널 매크로와 printk 계열 선언. rpaphp.h 의 로그 매크로가
 * printk 를 직접 부르므로 필요하다. */
#include <linux/kernel.h>
/* [한국어] module_init/module_exit 와 MODULE_ 계열 매크로. 이 파일이 모듈의
 * 진입점을 갖는다. */
#include <linux/module.h>
/* [한국어] module_param_named() 를 위해 포함한다. 디버그 플래그를 실행 중에도
 * 바꿀 수 있게 노출한다. */
#include <linux/moduleparam.h>
/* [한국어] 디바이스 트리 API — of_get_property(), of_find_property(),
 * of_property_present(), for_each_node_by_name(), of_node_name_eq(),
 * of_prop_next_u32(). 이 드라이버는 슬롯 정보를 config space 가 아니라
 * 디바이스 트리에서 얻으므로 이 헤더가 사실상의 하드웨어 서술 통로다. */
#include <linux/of.h>
/* [한국어] struct pci_bus 와 enum pci_bus_speed 등 PCI 코어 정의. */
#include <linux/pci.h>
/* [한국어] struct hotplug_slot 과 hotplug_slot_ops — PCI 핫플러그 공용 코어에
 * 붙는 통로다. */
#include <linux/pci_hotplug.h>
/* [한국어] SMP 관련 선언. [상류 코드 관찰] 이 파일에서 smp_ 로 시작하는 이름이나
 * num_online_cpus 같은 것을 쓰는 곳을 찾지 못했다. 다만 헤더가 다른 헤더를
 * 끌어오기 위해 필요할 수 있고, 그 포함 관계는 include/ 가 이 트리에 없어
 * 확인 못 함이므로 불필요하다고 단정하지 않는다. */
#include <linux/smp.h>
/* [한국어] __init 과 __exit 표시를 위해 포함한다. 모듈 진입·종료 함수와
 * cleanup_slots() 에 붙는다. */
#include <linux/init.h>
/* [한국어] vm_unmap_aliases() 를 위해 포함한다. 슬롯을 끌 때 남아 있는
 * vmalloc 별칭 매핑을 정리하는 데 쓴다 — MMIO 매핑이 남은 채 하드웨어가
 * 사라지는 것을 막기 위한 것으로 읽힌다. */
#include <linux/vmalloc.h>
/* [한국어] PowerPC 펌웨어 기능 질의용 헤더. [상류 코드 관찰] 이 파일에서
 * firmware_has_feature 같은 이름을 쓰는 곳을 찾지 못했다. */
#include <asm/firmware.h>
/* [한국어] EEH(Enhanced Error Handling) 서브시스템 헤더. 이 파일이 실제로 쓰는
 * 것은 pseries_eeh_init_edev_recursive() 하나다.
 * [상류 코드 관찰] 옆의 상류 주석은 eeh_add_device 를 위해 포함한다고 적어
 * 두었지만, 그 이름은 이 파일 어디에도 나오지 않는다 — 코드가 바뀌는 동안
 * 주석만 남은 흔적으로 보인다. arch/powerpc 가 이 트리에 없어 EEH 쪽 구현은
 * 확인 대상 밖이다. */
#include <asm/eeh.h>       /* for eeh_add_device() */
/* [한국어] RTAS(Run-Time Abstraction Services) 헤더. RTAS 는 IBM POWER 펌웨어가
 * 제공하는 호출 인터페이스로, OS 가 하드웨어를 직접 만지는 대신 펌웨어에
 * 요청하게 한다. 이 파일이 쓰는 것은 rtas_set_indicator() 와
 * rtas_get_power_level() 이다.
 * [상류 코드 관찰] 옆의 상류 주석은 rtas_call 을 위해 포함한다고 적었지만
 * 이 파일은 그 저수준 함수를 직접 부르지 않는다. */
#include <asm/rtas.h>		/* rtas_call */
/* [한국어] PowerPC 의 PCI 브리지 정의 헤더. struct pci_controller 와 PCI_DN()
 * 매크로가 여기서 온다. PCI_DN 은 디바이스 트리 노드에서 PowerPC 전용
 * PCI 정보(struct pci_dn)를 꺼내는 매크로이며, 그 정의는 arch/powerpc 에 있어
 * 이 트리에서 확인 못 함. */
#include <asm/pci-bridge.h>	/* for pci_controller */
/* [한국어] PowerPC 의 Open Firmware 헤더. [상류 코드 관찰] 이 파일에서
 * prom_ 으로 시작하는 이름을 쓰는 곳을 찾지 못했다. */
#include <asm/prom.h>
/* [한국어] drivers/pci 내부 전용 헤더. 상대 경로 따옴표인 것은 커널 전역 헤더가
 * 아니기 때문이다.
 * [상류 코드 관찰] 옆의 두 줄짜리 상류 주석은 pci_add_new_bus 와
 * pci_do_scan_bus 를 위해 포함한다고 적었지만, 그 두 이름은 이 파일 어디에도
 * 나오지 않는다. 실제로 쓰는 것은 pci_hp_add_devices() 와
 * pci_hp_remove_devices() 인데, 그 둘의 선언 위치는 이 트리에서 찾지 못했다
 * — drivers/pci/pci.h 에는 없고 arch/powerpc 쪽으로 보이나 확인할 수 없다. */
#include "../pci.h"		/* for pci_add_new_bus */
				/* and pci_do_scan_bus */
/* [한국어] 이 드라이버 묶음의 공용 헤더. struct slot, 로그 매크로 dbg/err/info,
 * RTAS 토큰 상수(DR_INDICATOR 등), 센서 값(EMPTY/PRESENT), 슬롯 상태
 * (CONFIGURED 등), to_slot(), 그리고 rpaphp_slot.c 와 rpaphp_pci.c 가
 * 구현하는 함수들의 원형이 모두 여기 있다. */
#include "rpaphp.h"

/* [한국어] 디버그 로그를 켤지 정하는 전역 플래그. rpaphp.h 의 dbg 매크로가 이 값을
 * 보고, 아래 module_param_named 가 이것을 sysfs 파라미터로 노출한다.
 * 설정자: 모듈 파라미터(부팅 인자 또는 실행 중 sysfs 쓰기).
 * 읽는 자: 이 파일과 rpaphp_slot.c / rpaphp_pci.c 의 dbg 매크로.
 * 값 범위: true/false. 초기값은 false(전역이라 0).
 * 동기화: 없다. 진단용이라 경쟁이 문제가 되지 않는다. */
bool rpaphp_debug;
/* [한국어] 등록된 모든 슬롯의 전역 리스트 머리. 매크로가 선언과 초기화를 함께 한다.
 * 설정자: rpaphp_slot.c 의 rpaphp_register_slot()/rpaphp_deregister_slot().
 * 읽는 자: 이 파일의 cleanup_slots(), rpaphp_slot.c 의 중복 검사, 그리고
 *   rpadlpar_core.c 의 find_php_slot() — 상위 계층이 이 리스트를 직접 순회한다.
 * 값 범위: 비어 있을 수도 있다.
 * 동기화: [상류 코드 관찰] 이 리스트를 지키는 락이 없다. 부팅 경로, sysfs 경로,
 *   DLPAR 경로가 모두 이 리스트에 닿는데, 직렬화를 보장하는 것은 DLPAR 쪽의
 *   자기 뮤텍스뿐이다. */
LIST_HEAD(rpaphp_slot_head);
/* [한국어] 그 리스트를 모듈 밖으로 내보낸다. rpadlpar_io 모듈이 별도 모듈이라
 * 심볼을 내보내야 그쪽 find_php_slot() 이 순회할 수 있다. GPL 전용 판이다. */
EXPORT_SYMBOL_GPL(rpaphp_slot_head);

/* [한국어] 드라이버 버전 문자열. 아래 rpaphp_init() 의 부팅 로그에만 쓰인다. */
#define DRIVER_VERSION	"0.1"
/* [한국어] 작성자 문자열. MODULE_AUTHOR 에 들어간다. */
#define DRIVER_AUTHOR	"Linda Xie <lxie@us.ibm.com>"
/* [한국어] 드라이버 설명 문자열. MODULE_DESCRIPTION 과 부팅 로그 양쪽에 쓰인다. */
#define DRIVER_DESC	"RPA HOT Plug PCI Controller Driver"

/* [한국어] 위치 코드(location code) 문자열의 최대 길이.
 * [상류 코드 관찰] 정의만 있고 이 파일에서 참조하지 않는다. 예전에 슬롯의
 * 물리 위치 코드를 다루던 코드가 있었던 흔적으로 보이나, 그 근거는 이 트리에
 * 남아 있지 않다. */
#define MAX_LOC_CODE 128

/* [한국어] modinfo 에 보일 작성자. */
MODULE_AUTHOR(DRIVER_AUTHOR);
/* [한국어] modinfo 에 보일 설명. */
MODULE_DESCRIPTION(DRIVER_DESC);
/* [한국어] 라이선스 선언. 이것이 없으면 커널이 모듈을 오염으로 표시하고,
 * 위 EXPORT_SYMBOL_GPL 같은 GPL 전용 심볼도 쓸 수 없다. */
MODULE_LICENSE("GPL");

/* [한국어] 전역 디버그 플래그를 debug 라는 이름의 모듈 파라미터로 노출한다.
 * 0644 라 적재 후에도 sysfs 로 켜고 끌 수 있다 — 핫플러그는 사람의 조작에
 * 반응하는 기능이라 재현이 어렵고, 실행 중에 로그를 켤 수 있는 것이 중요하다.
 * [상류 코드 관찰] MODULE_PARM_DESC 로 설명을 붙이지 않아 modinfo 에
 * 파라미터 설명이 나오지 않는다. */
module_param_named(debug, rpaphp_debug, bool, 0644);

/* [한국어]
 * set_attention_status - attention LED 상태를 펌웨어에 요청한다
 *
 * @hotplug_slot: 공용 코어가 넘겨 주는 슬롯 객체.
 * @value: LED 제어 값(옆의 상류 kernel-doc 이 sysfs 사용법까지 적어 두었다 —
 *         0 은 끔, 1 은 켬, 2 는 식별용 깜빡임).
 * @return: 0 성공, RTAS 호출이 돌려준 오류.
 *
 * 이 드라이버가 하드웨어 레지스터를 만지지 않는다는 성격이 가장 잘 드러나는
 * 함수다. LED 하나를 켜는 데도 MMIO 가 아니라 RTAS 펌웨어 호출을 쓰며,
 * 대상을 가리키는 것도 물리 주소가 아니라 DRC 인덱스다.
 * 값 검증 방식이 특이하다 — 범위를 벗어난 값을 거절하는 대신 1(켬)로 바꿔
 * 버린다. sysfs 로 아무 숫자나 써 넣을 수 있는 통로이므로, 펌웨어에 알 수 없는
 * 값을 넘기지 않으려는 방어로 읽힌다.
 * 실행 컨텍스트: 사용자의 sysfs 쓰기(프로세스 컨텍스트). RTAS 호출이 잠들 수
 * 있으므로 인터럽트 문맥에서는 부를 수 없다.
 * 에러 경로: 펌웨어가 실패를 알리면 기억해 둔 상태를 갱신하지 않고 그 오류를
 * 그대로 전달한다 — 실제 LED 와 소프트웨어 기억이 어긋나지 않게 하려는 순서다.
 *
 * 호출 체인:
 *   사용자의 sysfs 쓰기 → 공용 코어 → [set_attention_status]
 *     → to_slot(), rtas_set_indicator()
 */
/**
 * set_attention_status - set attention LED
 * @hotplug_slot: target &hotplug_slot
 * @value: LED control value
 *
 * echo 0 > attention -- set LED OFF
 * echo 1 > attention -- set LED ON
 * echo 2 > attention -- set LED ID(identify, light is blinking)
 */
static int set_attention_status(struct hotplug_slot *hotplug_slot, u8 value)
{
	/* [한국어] RTAS 호출 결과. */
	int rc;
	/* [한국어] 공용 코어가 준 포인터에서 이 드라이버의 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);

	/* [한국어] 받은 값이 아는 값인지 가른다. */
	switch (value) {
	/* [한국어] 0 = LED 끔. */
	case 0:
	/* [한국어] 1 = LED 켬. */
	case 1:
	/* [한국어] 2 = 식별용 깜빡임. */
	case 2:
		/* [한국어] 셋 중 하나면 그대로 쓴다. */
		break;
	/* [한국어] 그 밖의 값은 펌웨어가 어떻게 해석할지 알 수 없으므로 */
	default:
		/* [한국어] 켬으로 바꿔 버린다. 거절하지 않고 보정하는 선택인데, 그 이유를 밝힌
		 * 주석은 코드에 없다. */
		value = 1;
		/* [한국어] 보정 후 진행한다. */
		break;
	}

	/* [한국어] 펌웨어에 LED 제어를 요청한다. 첫 인자는 인디케이터 종류를 가리키는
	 * RTAS 토큰이고, 둘째는 이 슬롯의 DRC 인덱스, 셋째가 원하는 상태다.
	 * 하드웨어를 직접 만지지 않고 논리 식별자로 지목한다는 점이 이 플랫폼의 특징이다.
	 * RTAS 내부 동작은 arch/powerpc 가 이 트리에 없어 확인 못 함. */
	rc = rtas_set_indicator(DR_INDICATOR, slot->index, value);
	/* [한국어] 펌웨어가 성공을 알렸을 때만 */
	if (!rc)
		/* [한국어] 소프트웨어가 기억하는 LED 상태를 갱신한다. 아래 조회 콜백이 이 값을
		 * 그대로 돌려주므로, 실패했을 때 갱신하면 실제와 어긋나게 된다. */
		slot->attention_status = value;

	/* [한국어] 펌웨어의 결과를 그대로 사용자에게 전달한다. */
	return rc;
}

/* [한국어]
 * get_power_status - 슬롯 전원 상태를 펌웨어에 물어본다
 *
 * @hotplug_slot: 공용 코어가 넘겨 주는 슬롯 객체.
 * @value: 결과를 담을 곳.
 * @return: 0 성공, RTAS 호출이 돌려준 오류.
 *
 * 기억해 둔 값을 돌려주는 attention 쪽과 달리, 전원은 매번 펌웨어에 다시
 * 물어본다. 파티션 밖에서 전원이 바뀔 수 있는 환경이라 캐시하지 않는 것으로
 * 읽힌다.
 * 대상을 가리키는 값이 DRC 인덱스가 아니라 전원 도메인 번호인 점에 주의 —
 * 여러 슬롯이 한 전원 도메인을 공유할 수 있어 식별자가 따로 있다.
 * 실행 컨텍스트: 사용자의 sysfs 읽기(프로세스 컨텍스트). RTAS 호출이 잠들 수 있다.
 * 에러 경로: 실패하면 value 를 건드리지 않고 오류를 전달한다.
 *
 * 호출 체인:
 *   사용자의 sysfs 읽기 → 공용 코어 → [get_power_status]
 *     → to_slot(), rtas_get_power_level()
 */
/**
 * get_power_status - get power status of a slot
 * @hotplug_slot: slot to get status
 * @value: pointer to store status
 */
static int get_power_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] retval 은 RTAS 결과, level 은 펌웨어가 돌려줄 전원 준위. int 로 받아
	 * u8 에 옮기는 것은 RTAS API 의 형이 int 이기 때문이다. */
	int retval, level;
	/* [한국어] 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);

	/* [한국어] 이 슬롯이 속한 전원 도메인의 현재 준위를 펌웨어에 묻는다.
	 * 도메인 번호는 슬롯을 만들 때 디바이스 트리에서 읽어 둔 값이다. */
	retval = rtas_get_power_level(slot->power_domain, &level);
	/* [한국어] 성공했을 때만 */
	if (!retval)
		/* [한국어] 결과를 옮겨 담는다. 실패 시 건드리지 않아 호출자가 쓰레기 값을
		 * 보지 않는다. */
		*value = level;
	/* [한국어] 펌웨어의 결과를 그대로 전달한다. */
	return retval;
}

/* [한국어]
 * get_attention_status - 기억해 둔 attention LED 상태를 돌려준다
 *
 * @hotplug_slot: 공용 코어가 넘겨 주는 슬롯 객체.
 * @value: 결과를 담을 곳.
 * @return: 언제나 0(성공).
 *
 * 전원과 달리 LED 는 펌웨어에 다시 묻지 않고 소프트웨어가 기억한 값을 돌려준다.
 * RTAS 에 인디케이터를 '읽는' 서비스가 없거나 쓰지 않기 때문으로 보이며,
 * 그래서 set 쪽이 성공했을 때만 기억을 갱신하는 순서가 중요해진다.
 * 이 방식의 한계는 파티션 밖에서 LED 가 바뀌면 알 수 없다는 것이다.
 * 실행 컨텍스트: 사용자의 sysfs 읽기(프로세스 컨텍스트). 잠들지 않는다.
 *
 * 호출 체인:
 *   사용자의 sysfs 읽기 → 공용 코어 → [get_attention_status] → to_slot()
 */
/**
 * get_attention_status - get attention LED status
 * @hotplug_slot: slot to get status
 * @value: pointer to store status
 */
static int get_attention_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);
	/* [한국어] set 쪽이 성공했을 때 기록해 둔 값을 그대로 담는다. */
	*value = slot->attention_status;
	/* [한국어] 조회는 실패할 수 없다. */
	return 0;
}

/* [한국어]
 * get_adapter_status - 슬롯에 카드가 있는지, 있다면 구성 상태가 무엇인지 알려 준다
 *
 * @hotplug_slot: 공용 코어가 넘겨 주는 슬롯 객체.
 * @value: 결과를 담을 곳.
 * @return: 0 성공(센서 조회가 실패하면 그 오류).
 *
 * 두 층의 정보를 하나의 값으로 합쳐 돌려주는 것이 특징이다. 먼저 펌웨어 센서에
 * 카드가 물리적으로 있는지 묻고, 있다면 소프트웨어가 아는 구성 상태
 * (CONFIGURED / NOT_CONFIGURED 등)를 대신 실어 준다. 즉 '비어 있음' 은
 * 하드웨어 사실이고 그 밖의 값은 소프트웨어 상태다.
 * 먼저 NOT_VALID 로 채워 두는 순서에 주의 — 센서가 예상 밖의 값을 돌려주면
 * 아래 두 분기에 걸리지 않아 그 기본값이 그대로 남는다. 즉 '알 수 없음' 이
 * 기본이다.
 * 실행 컨텍스트: 사용자의 sysfs 읽기(프로세스 컨텍스트). 센서 조회가 RTAS
 * 호출이라 잠들 수 있다.
 * 에러 경로: 센서 조회 실패는 그대로 전달하며, 그때 value 는 NOT_VALID 다.
 *
 * 호출 체인:
 *   사용자의 sysfs 읽기 → 공용 코어 → [get_adapter_status]
 *     → to_slot(), rpaphp_get_sensor_state()
 */
static int get_adapter_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);
	/* [한국어] rc 는 센서 조회 결과, state 는 펌웨어가 돌려줄 센서 값. */
	int rc, state;

	/* [한국어] 펌웨어 센서에 카드 유무를 묻는다. 구현은 rpaphp_pci.c 에 있고,
	 * 그 안에서 RTAS 센서 호출을 하고 오류 코드를 errno 로 옮긴다. */
	rc = rpaphp_get_sensor_state(slot, &state);

	/* [한국어] 먼저 '알 수 없음' 으로 채워 둔다. 아래 두 분기에 걸리지 않는 센서
	 * 값이 오면 이 값이 그대로 남는 구조다. 조회 실패 시에도 이 값이 나간다. */
	*value = NOT_VALID;
	/* [한국어] 센서 조회 자체가 실패했으면 */
	if (rc)
		/* [한국어] 그 오류를 전달한다. */
		return rc;

	/* [한국어] 카드가 없다면 */
	if (state == EMPTY)
		/* [한국어] 비어 있음을 그대로 알린다 — 이것만은 하드웨어 사실이다. */
		*value = EMPTY;
	/* [한국어] 카드가 있다면 */
	else if (state == PRESENT)
		/* [한국어] 물리적 존재가 아니라 소프트웨어가 아는 구성 상태를 실어 준다.
		 * 그래서 같은 '카드 있음' 이라도 열거 전(NOT_CONFIGURED)과 후(CONFIGURED)가
		 * 다른 값으로 보인다. */
		*value = slot->state;

	/* [한국어] 조회 자체는 성공했으므로 0 을 돌려준다 — 센서 값이 예상 밖이어도
	 * 오류로 만들지 않고 NOT_VALID 로 전달한다. */
	return 0;
}

/* [한국어]
 * get_max_bus_speed - 슬롯 종류 코드를 PCI 버스 속도로 옮긴다
 *
 * @slot: 대상 슬롯. type 필드에 펌웨어가 준 종류 코드가 들어 있다.
 * @return: 그 종류에 대응하는 버스 속도(알 수 없으면 PCI_SPEED_UNKNOWN).
 *
 * 슬롯의 물리 규격을 알아내는 방법이 이 플랫폼에서는 config space 조회가
 * 아니라 디바이스 트리의 DRC 종류 코드다. 그 숫자를 리눅스의 버스 속도
 * 열거값으로 옮기는 표가 이 함수다. 코드 값의 의미(1~6 이 33MHz PCI,
 * 7~8 이 66MHz PCI, 11~16 이 PCI-X 세 단계)는 RPA/PAPR 규격에서 온 것인데,
 * 그 문서는 이 트리에 없어 확인 대상 밖이다 — 여기서는 코드가 매기는 대응을
 * 그대로 적었다.
 * 결과는 slot->bus->max_bus_speed 에 실려 사용자 공간에 노출된다.
 * 실행 컨텍스트: enable_slot() 안(프로세스 컨텍스트). 순수 계산이라 잠들지 않는다.
 *
 * [상류 코드 관찰] 9, 10 번 코드가 어느 case 에도 없어 PCI_SPEED_UNKNOWN 으로
 * 떨어진다. 그 두 값이 무엇인지는 규격 문서가 필요해 확인 못 함.
 *
 * 호출 체인:
 *   enable_slot() → [get_max_bus_speed]
 */
static enum pci_bus_speed get_max_bus_speed(struct slot *slot)
{
	/* [한국어] 돌려줄 속도 값. */
	enum pci_bus_speed speed;
	/* [한국어] 펌웨어가 준 종류 코드로 가른다. */
	switch (slot->type) {
	/* [한국어] 코드 1 — 아래 6까지가 같은 속도로 묶인다. */
	case 1:
	/* [한국어] 코드 2. */
	case 2:
	/* [한국어] 코드 3. */
	case 3:
	/* [한국어] 코드 4. */
	case 4:
	/* [한국어] 코드 5. */
	case 5:
	/* [한국어] 코드 6. */
	case 6:
		/* [한국어] 여섯 코드 모두 33MHz 재래식 PCI 다(옆의 상류 주석). */
		speed = PCI_SPEED_33MHz;	/* speed for case 1-6 */
		/* [한국어] 이 묶음 처리 끝. */
		break;
	/* [한국어] 코드 7 — 아래 8과 함께 묶인다. */
	case 7:
	/* [한국어] 코드 8. */
	case 8:
		/* [한국어] 두 코드는 66MHz 재래식 PCI 다. */
		speed = PCI_SPEED_66MHz;
		/* [한국어] 이 묶음 처리 끝. */
		break;
	/* [한국어] 코드 11 — 아래 14와 함께 묶인다. 9, 10 이 건너뛰어진 점에 주의. */
	case 11:
	/* [한국어] 코드 14. */
	case 14:
		/* [한국어] 두 코드는 66MHz PCI-X 다. */
		speed = PCI_SPEED_66MHz_PCIX;
		/* [한국어] 이 묶음 처리 끝. */
		break;
	/* [한국어] 코드 12 — 아래 15와 함께 묶인다. */
	case 12:
	/* [한국어] 코드 15. */
	case 15:
		/* [한국어] 두 코드는 100MHz PCI-X 다. */
		speed = PCI_SPEED_100MHz_PCIX;
		/* [한국어] 이 묶음 처리 끝. */
		break;
	/* [한국어] 코드 13 — 아래 16과 함께 묶인다. */
	case 13:
	/* [한국어] 코드 16. */
	case 16:
		/* [한국어] 두 코드는 133MHz PCI-X 로 이 표에서 가장 빠르다. */
		speed = PCI_SPEED_133MHz_PCIX;
		/* [한국어] 이 묶음 처리 끝. */
		break;
	/* [한국어] 표에 없는 코드(0, 9, 10, 17 이상)는 */
	default:
		/* [한국어] 알 수 없음으로 둔다. 오류로 만들지 않는 것은 속도를 몰라도 슬롯을
		 * 쓰는 데 지장이 없기 때문이다. */
		speed = PCI_SPEED_UNKNOWN;
		/* [한국어] 기본 처리 끝. */
		break;
	}

	/* [한국어] 정해진 속도를 돌려준다. */
	return speed;
}

/* [한국어]
 * get_children_props - 부모 노드에서 v1 형식 DRC 속성 네 개를 한꺼번에 얻는다
 *
 * @dn: 속성을 가진 노드(호출자에 따라 슬롯 노드이거나 그 부모다).
 * @drc_indexes: DRC 인덱스 배열을 받을 곳. NULL 이면 받지 않는다.
 * @drc_names: 이름 문자열 뭉치를 받을 곳. NULL 이면 받지 않는다.
 * @drc_types: 종류 문자열 뭉치를 받을 곳. NULL 이면 받지 않는다.
 * @drc_power_domains: 전원 도메인 배열을 받을 곳. NULL 이면 받지 않는다.
 * @return: 0 성공, -EINVAL(네 속성 중 하나라도 없음).
 *
 * v1 형식에서는 한 노드 아래의 슬롯들이 네 개의 병렬 배열로 서술된다. 넷이
 * 모두 있어야 의미가 있으므로, 하나라도 없으면 옆의 상류 주석대로 '동적으로
 * 뗄 수 있는 자식이 없는 노드' 로 보고 실패를 돌려준다.
 * 출력 인자를 NULL 로 넘길 수 있게 한 것은 호출자마다 필요한 속성이 다르기
 * 때문이다 — 그래도 존재 검사는 네 개 모두에 대해 한다.
 * 실행 컨텍스트: 슬롯 등록 경로(프로세스 컨텍스트). of_get_property() 는
 * 노드 참조를 새로 잡지 않고 이미 파싱된 속성 값을 가리키는 포인터를 돌려주므로,
 * 반환된 포인터의 수명은 그 노드의 수명에 묶인다.
 *
 * 호출 체인:
 *   is_php_dn() / rpaphp_check_drc_props_v1() → [get_children_props]
 *     → of_get_property()
 */
static int get_children_props(struct device_node *dn, const __be32 **drc_indexes,
			      const __be32 **drc_names, const __be32 **drc_types,
			      const __be32 **drc_power_domains)
{
	/* [한국어] 네 속성 값을 임시로 받을 포인터들. 모두 빅엔디언 32비트 배열을
	 * 가리킨다 — 디바이스 트리는 바이트 순서가 빅엔디언으로 고정되어 있어,
	 * 값을 쓸 때는 be32_to_cpu 로 바꿔야 한다. */
	const __be32 *indexes, *names, *types, *domains;

	/* [한국어] 슬롯들의 DRC 인덱스 배열. 첫 셀이 개수이고 그 뒤에 항목이 이어진다. */
	indexes = of_get_property(dn, "ibm,drc-indexes", NULL);
	/* [한국어] 슬롯 이름들. 고정폭 배열이 아니라 널로 끝나는 문자열이 줄줄이
	 * 붙어 있는 형태라, i 번째 항목을 보려면 앞에서부터 길이만큼 밀어야 한다. */
	names = of_get_property(dn, "ibm,drc-names", NULL);
	/* [한국어] 슬롯 종류들. 이름과 같은 형식이다. */
	types = of_get_property(dn, "ibm,drc-types", NULL);
	/* [한국어] 전원 도메인 번호 배열. 인덱스 배열과 같은 형식이다. */
	domains = of_get_property(dn, "ibm,drc-power-domains", NULL);

	/* [한국어] 넷이 모두 있어야 v1 형식으로 해석할 수 있다. */
	if (!indexes || !names || !types || !domains) {
		/* Slot does not have dynamically-removable children */
		/* [한국어] 옆의 상류 주석대로 이 노드에는 동적으로 뗄 수 있는 자식이 없다는 뜻이다.
		 * 오류라기보다 '해당 없음' 에 가까운 반환이라, 호출자들이 이 값을 실패가 아니라
		 * 건너뛸 이유로 다룬다. */
		return -EINVAL;
	}
	/* [한국어] 호출자가 원할 때만 넘겨 준다. */
	if (drc_indexes)
		/* [한국어] 인덱스 배열을 전달한다. */
		*drc_indexes = indexes;
	/* [한국어] 이름을 원한다면 */
	if (drc_names)
		/* &drc_names[1] contains NULL terminated slot names */
		/* [한국어] 옆의 상류 주석대로, 첫 셀(개수)을 건너뛴 자리부터 널로 끝나는 슬롯
		 * 이름들이 이어진다. 여기서는 배열 시작을 그대로 넘기고, 건너뛰는 일은
		 * 호출자가 한다. */
		*drc_names = names;
	/* [한국어] 종류를 원한다면 */
	if (drc_types)
		/* &drc_types[1] contains NULL terminated slot types */
		/* [한국어] 이름과 같은 형식이므로 같은 방식으로 다뤄야 한다(옆의 상류 주석). */
		*drc_types = types;
	/* [한국어] 전원 도메인을 원한다면 */
	if (drc_power_domains)
		/* [한국어] 그 배열도 전달한다. */
		*drc_power_domains = domains;

	/* [한국어] 네 속성이 모두 존재했다. */
	return 0;
}


/* [한국어]
 * rpaphp_check_drc_props_v1 - v1 형식 DRC 속성에서 이름/종류가 맞는지 확인한다
 *
 * @dn: 검사할 노드. 속성은 이 노드가 아니라 그 부모에 있다.
 * @drc_name: 맞아야 할 DRC 이름. NULL 이면 이름은 따지지 않는다.
 * @drc_type: 맞아야 할 DRC 종류. NULL 이면 종류는 따지지 않는다.
 * @my_index: 이 노드의 DRC 인덱스. 호출자가 ibm,my-drc-index 에서 읽어 넘긴다.
 * @return: 0 일치, -EINVAL(속성 없음 또는 불일치).
 *
 * 아래 상류 주석이 절차를 요약해 두었다 — 자기 인덱스를 얻고, 부모에서 DRC
 * 정보를 얻고, 인덱스로 자기 항목을 찾아 이름과 종류를 검증한다. 이 함수는
 * 그중 뒤의 두 단계를 맡는다(첫 단계는 호출자가 이미 했다).
 * v1 형식의 성가신 점이 여기 그대로 드러난다. 인덱스는 고정폭 32비트 배열이라
 * 첨자로 바로 접근되지만, 이름과 종류는 널로 끝나는 문자열이 줄줄이 붙어 있어
 * 첨자가 없다. 그래서 인덱스를 한 칸 볼 때마다 문자열 포인터도 한 칸씩 함께
 * 밀어야 하고, 그것이 루프 안의 strlen 두 줄이다.
 * 실행 컨텍스트: 슬롯 검색·등록 경로(프로세스 컨텍스트). 잠들지 않는다.
 *
 * [상류 코드 관찰] 인덱스를 찾지 못한 채 루프가 끝나면 name_tmp 와 type_tmp 가
 * 배열 끝을 넘어선 자리를 가리키는데, 그 상태에서 그대로 문자열 비교로 들어간다
 * — '찾지 못했다' 를 따로 구분하는 코드가 없다.
 *
 * 호출 체인:
 *   rpaphp_check_drc_props() → [rpaphp_check_drc_props_v1]
 *     → get_children_props()
 */
/* Verify the existence of 'drc_name' and/or 'drc_type' within the
 * current node.  First obtain its my-drc-index property.  Next,
 * obtain the DRC info from its parent.  Use the my-drc-index for
 * correlation, and obtain/validate the requested properties.
 */

static int rpaphp_check_drc_props_v1(struct device_node *dn, char *drc_name,
				char *drc_type, unsigned int my_index)
{
	/* [한국어] 이름과 종류 문자열 뭉치를 훑어 갈 커서. 문자열 길이만큼 전진시킨다. */
	char *name_tmp, *type_tmp;
	/* [한국어] 부모에서 얻을 인덱스 배열과 이름 뭉치. */
	const __be32 *indexes, *names;
	/* [한국어] 종류 뭉치와 전원 도메인 배열. 후자는 이 함수에서 쓰이지 않지만
	 * get_children_props() 가 넷을 한꺼번에 다루므로 받을 자리가 필요하다. */
	const __be32 *types, *domains;
	/* [한국어] i 는 인덱스 배열 순회 커서, rc 는 속성 획득 결과. */
	int i, rc;

	/* [한국어] 속성은 이 노드가 아니라 부모에 있다 — 부모가 슬롯들을 한꺼번에
	 * 서술하는 형식이기 때문이다. 그래서 dn 이 아니라 dn->parent 를 넘긴다. */
	rc = get_children_props(dn->parent, &indexes, &names, &types, &domains);
	/* [한국어] 부모에 v1 속성이 없으면 이 노드는 v1 방식으로 검증할 수 없다. */
	if (rc < 0) {
		/* [한국어] 불일치로 보고한다. */
		return -EINVAL;
	}

	/* [한국어] 첫 셀은 개수이므로 그 다음 자리부터가 실제 이름들이다. const 를
	 * 벗기는 캐스팅이 필요한 것은 문자열 커서를 전진시켜야 하기 때문이다. */
	name_tmp = (char *) &names[1];
	/* [한국어] 종류 뭉치도 같은 방식으로 첫 항목을 가리키게 한다. */
	type_tmp = (char *) &types[1];

	/* Iterate through parent properties, looking for my-drc-index */
	/* [한국어] 옆의 상류 주석대로 부모의 인덱스 목록을 훑어 내 인덱스를 찾는다.
	 * 첫 셀이 개수이므로 그 값이 반복 횟수가 된다. 디바이스 트리 값은 빅엔디언
	 * 이라 be32_to_cpu 로 CPU 바이트 순서로 바꿔야 한다. */
	for (i = 0; i < be32_to_cpu(indexes[0]); i++) {
		/* [한국어] i 번째 인덱스가 내 것인가. 첫 셀이 개수라 항목은 [i + 1] 에 있다. */
		if (be32_to_cpu(indexes[i + 1]) == my_index)
			/* [한국어] 찾았다 — 이 시점의 두 문자열 커서가 내 항목을 가리킨다. */
			break;

		/* [한국어] 아직 아니면 이름 커서를 다음 문자열로 민다. 널 종료 문자까지 건너뛰어야
		 * 하므로 길이에 1 을 더한다. 첨자로 접근할 수 없는 형식이라 이렇게 걸어야 한다. */
		name_tmp += (strlen(name_tmp) + 1);
		/* [한국어] 종류 커서도 같은 방식으로 함께 민다. 두 뭉치의 항목 순서가 인덱스
		 * 배열과 같다는 전제 위에 있다. */
		type_tmp += (strlen(type_tmp) + 1);
	}

	/* [한국어] 이름과 종류를 각각 검증한다. NULL 이면 '따지지 않는다' 는 뜻이라
	 * 그 조건은 참으로 처리한다 — DLPAR 이 이름만 알고 종류는 모를 때 쓰는 방식이다. */
	if (((drc_name == NULL) || (drc_name && !strcmp(drc_name, name_tmp))) &&
	    ((drc_type == NULL) || (drc_type && !strcmp(drc_type, type_tmp))))
		/* [한국어] 둘 다 조건을 만족하면 이 노드가 찾던 슬롯이다. */
		return 0;

	/* [한국어] 아니면 불일치로 보고한다. 호출자는 다음 노드로 넘어간다. */
	return -EINVAL;
}

/* [한국어]
 * rpaphp_check_drc_props_v2 - v2 형식 DRC 속성에서 이름/종류가 맞는지 확인한다
 *
 * @dn: 검사할 노드. 속성은 그 부모에 있다.
 * @drc_name: 맞아야 할 DRC 이름. NULL 이면 따지지 않는다.
 * @drc_type: 맞아야 할 DRC 종류. NULL 이면 따지지 않는다.
 * @my_index: 이 노드의 DRC 인덱스.
 * @return: 0 일치, -EINVAL(속성 없음 또는 불일치).
 *
 * v1 판과 목적은 같지만 자료 형식이 달라 구현이 통째로 다르다. v2 는 슬롯을
 * 하나씩 나열하지 않고 '인덱스 구간 + 이름 접두사 + 접미 시작 번호' 로 묶어
 * 압축해 둔다. 그래서 내 인덱스가 어느 구간에 드는지 찾은 뒤, 구간 시작에서
 * 얼마나 떨어졌는지를 계산해 이름을 그 자리에서 조립한다 — 저장된 이름을
 * 읽는 것이 아니라 만들어 내는 것이 v1 과의 결정적 차이다.
 * 실행 컨텍스트: 슬롯 검색·등록 경로(프로세스 컨텍스트). 잠들지 않는다.
 *
 * [상류 코드 관찰] 두 가지.
 *  1. 구간을 찾지 못한 채 루프가 끝나면 cell_drc_name 이 한 번도 채워지지 않은
 *     채로 아래 문자열 비교에 쓰인다. 스택 버퍼라 그 시점의 내용이 무엇인지
 *     알 수 없다. 항목 수가 0 이면 drc 구조체도 마찬가지다.
 *  2. 이름 조립에 sprintf 를 써서 길이 검사가 없다. 버퍼는 MAX_DRC_NAME_LEN
 *     크기인데, 접두사와 번호의 길이가 그 안에 든다는 보장은 코드에 없다.
 * of_read_drc_info_cell() 과 struct of_drc_info 의 정의는 include/ 가 이 트리에
 * 없어 확인 못 함 — 여기서는 호출 자리의 쓰임새로만 설명했다.
 *
 * 호출 체인:
 *   rpaphp_check_drc_props() → [rpaphp_check_drc_props_v2]
 *     → of_find_property(), of_prop_next_u32(), of_read_drc_info_cell()
 */
static int rpaphp_check_drc_props_v2(struct device_node *dn, char *drc_name,
				char *drc_type, unsigned int my_index)
{
	/* [한국어] 부모의 ibm,drc-info 속성. 값 포인터가 아니라 속성 객체를 통째로
	 * 잡는 것은, 아래 읽기 함수가 속성 경계를 확인하며 한 항목씩 전진하기 때문이다. */
	struct property *info;
	/* [한국어] 속성에 들어 있는 항목의 개수. */
	unsigned int entries;
	/* [한국어] 한 항목을 풀어 담을 구조체. 인덱스 구간, 이름 접두사, 접미 시작 번호,
	 * 종류 문자열, 전원 도메인이 들어 있다(필드 정의는 이 트리에서 확인 못 함). */
	struct of_drc_info drc;
	/* [한국어] 속성 값을 훑어 가는 커서. 읽기 함수가 이 값을 전진시킨다. */
	const __be32 *value;
	/* [한국어] 조립한 슬롯 이름을 담을 스택 버퍼(위 관찰 1, 2 참조). */
	char cell_drc_name[MAX_DRC_NAME_LEN];
	/* [한국어] 항목 순회 커서. */
	int j;

	/* [한국어] 부모에서 v2 속성을 찾는다. 없으면 v2 형식이 아니다. */
	info = of_find_property(dn->parent, "ibm,drc-info", NULL);
	/* [한국어] 없으면 */
	if (info == NULL)
		/* [한국어] 불일치로 보고한다. */
		return -EINVAL;

	/* [한국어] 속성의 첫 32비트 값을 읽는다. 그것이 항목 개수다. */
	value = of_prop_next_u32(info, NULL, &entries);
	/* [한국어] 속성이 비어 있으면 */
	if (!value)
		/* [한국어] 읽을 것이 없으므로 불일치로 보고한다. */
		return -EINVAL;
	/* [한국어] 읽었다면 */
	else
		/* [한국어] 커서를 한 칸 밀어 실제 항목들의 시작을 가리키게 한다. */
		value++;

	/* [한국어] 항목을 하나씩 풀어 내 인덱스가 드는 구간을 찾는다. */
	for (j = 0; j < entries; j++) {
		/* [한국어] 한 항목을 구조체로 풀고 커서를 그만큼 전진시킨다. 속성 객체를 함께
		 * 넘기는 것은 경계를 넘지 않도록 확인하기 위해서로 읽히지만, 그 구현은
		 * 이 트리에 없어 확인 못 함. */
		of_read_drc_info_cell(&info, &value, &drc);

		/* Should now know end of current entry */

		/* Found it */
		/* [한국어] 옆의 상류 주석대로 여기가 찾는 지점이다 — 내 인덱스가 이 항목의
		 * 구간 안에 드는지 본다. v2 가 슬롯들을 구간으로 묶어 두기에 가능한 검사다. */
		if (my_index >= drc.drc_index_start && my_index <= drc.last_drc_index) {
			/* [한국어] 구간 시작에서 얼마나 떨어졌는지 — 이것이 이 슬롯의 순번이 된다. */
			int index = my_index - drc.drc_index_start;
			/* [한국어] 접두사에 그만큼 더한 번호를 붙여 이름을 만든다. v1 이 저장된 이름을
			 * 읽는 것과 달리 v2 는 이렇게 조립한다. 길이 검사가 없는 점은 위 관찰 2 참조. */
			sprintf(cell_drc_name, "%s%d", drc.drc_name_prefix,
				drc.drc_name_suffix_start + index);
			/* [한국어] 찾았으니 순회를 끝낸다. 이 시점의 drc 와 cell_drc_name 이 아래
			 * 비교에 쓰인다. */
			break;
		}
	}

	/* [한국어] v1 과 같은 방식으로 이름과 종류를 검증한다. NULL 은 '따지지 않는다'
	 * 는 뜻이다. 종류는 조립할 필요 없이 구조체에 그대로 들어 있다. */
	if (((drc_name == NULL) ||
	     (drc_name && !strcmp(drc_name, cell_drc_name))) &&
	    ((drc_type == NULL) ||
	     (drc_type && !strcmp(drc_type, drc.drc_type))))
		/* [한국어] 둘 다 만족하면 이 노드가 찾던 슬롯이다. */
		return 0;

	/* [한국어] 아니면 불일치로 보고한다. */
	return -EINVAL;
}

/* [한국어]
 * rpaphp_check_drc_props - 노드가 주어진 DRC 이름/종류와 맞는지 확인한다
 *
 * @dn: 검사할 노드.
 * @drc_name: 맞아야 할 DRC 이름. NULL 이면 따지지 않는다.
 * @drc_type: 맞아야 할 DRC 종류. NULL 이면 따지지 않는다.
 * @return: 0 일치, -EINVAL(핫플러그/DLPAR 대상이 아니거나 불일치).
 *
 * 두 형식을 흡수하는 진입점이다. 하는 일은 셋이다. (1) 이 노드에
 * ibm,my-drc-index 가 있는지 본다 — 없으면 애초에 동적 재구성 대상이 아니다.
 * (2) 부모에 ibm,drc-info 가 있는지로 형식을 판별한다. (3) 그에 맞는 판을 부른다.
 * 이 함수가 EXPORT_SYMBOL_GPL 로 내보내지는 이유가 중요하다 — 상위 계층인
 * rpadlpar_core.c 가 "이 DRC 이름의 슬롯이 어느 디바이스 트리 노드인가" 를
 * 찾을 때 노드를 하나씩 훑으며 이 함수로 물어보기 때문이다. 즉 DLPAR 과
 * rpaphp 를 잇는 두 접점 중 하나다(다른 하나는 rpaphp_add_slot).
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들지 않는다.
 *
 * 호출 체인:
 *   rpadlpar_core.c 의 find_vio_slot_node() / find_php_slot_pci_node(),
 *   그리고 이 파일의 등록 경로 → [rpaphp_check_drc_props]
 *     → rpaphp_check_drc_props_v2() 또는 rpaphp_check_drc_props_v1()
 */
int rpaphp_check_drc_props(struct device_node *dn, char *drc_name,
			char *drc_type)
{
	/* [한국어] 이 노드 자신의 DRC 인덱스를 담을 포인터. */
	const __be32 *my_index;

	/* [한국어] 노드가 스스로 밝히는 DRC 인덱스를 읽는다. 부모의 목록에서 자기 항목을
	 * 찾는 열쇠가 이 값이다. */
	my_index = of_get_property(dn, "ibm,my-drc-index", NULL);
	/* [한국어] 이 속성이 없다면 */
	if (!my_index) {
		/* Node isn't DLPAR/hotplug capable */
		/* [한국어] 옆의 상류 주석대로 이 노드는 DLPAR/핫플러그 대상이 아니다.
		 * 오류라기보다 '해당 없음' 이며, 호출자들이 다음 노드로 넘어가는 신호로 쓴다. */
		return -EINVAL;
	}

	/* [한국어] 부모에 v2 속성이 있는지로 형식을 판별한다. 있으면 새 형식이다. */
	if (of_property_present(dn->parent, "ibm,drc-info"))
		/* [한국어] v2 판에 넘긴다. 인덱스는 빅엔디언이라 CPU 순서로 바꿔서 준다. */
		return rpaphp_check_drc_props_v2(dn, drc_name, drc_type,
						be32_to_cpu(*my_index));
	/* [한국어] 없으면 옛 형식이다. */
	else
		/* [한국어] v1 판에 넘긴다. */
		return rpaphp_check_drc_props_v1(dn, drc_name, drc_type,
						be32_to_cpu(*my_index));
}
/* [한국어] rpadlpar_io 가 별도 모듈이라 이 심볼을 내보내야 그쪽에서 부를 수 있다. */
EXPORT_SYMBOL_GPL(rpaphp_check_drc_props);


/* [한국어]
 * is_php_type - DRC 종류 문자열이 PCI 핫플러그 슬롯의 것인지 판정한다
 *
 * @drc_type: 검사할 종류 문자열.
 * @return: 1 이면 핫플러그 슬롯, 0 이면 아니다.
 *
 * 판정 방법이 특이하다 — 문자열의 내용을 비교하는 대신 '숫자로 읽히는가' 만
 * 본다. 옆의 상류 주석이 그 근거를 밝힌다: PCI 핫플러그 노드의 DRC 종류는
 * 정수 문자열이고, 그렇지 않은 것(예: "SLOT", "PHB" 같은 문자열)은 다른
 * 종류의 동적 재구성 대상이다.
 * 변환에 실패했는지는 반환값이 아니라 endptr 이 한 글자도 전진하지 않았는지로
 * 판단한다 — 문자열 파싱의 표준 관용구다.
 * 실행 컨텍스트: 슬롯 등록 경로(프로세스 컨텍스트). 잠들지 않는다.
 *
 * 호출 체인:
 *   is_php_dn() / rpaphp_drc_info_add_slot() → [is_php_type] → simple_strtoul()
 */
static int is_php_type(char *drc_type)
{
	/* [한국어] 변환이 멈춘 지점을 받을 포인터. 값 자체보다 이것이 중요하다. */
	char *endptr;

	/* PCI Hotplug nodes have an integer for drc_type */
	/* [한국어] 옆의 상류 주석대로 10진 정수로 읽어 본다. 변환값은 버리고 어디서
	 * 멈췄는지만 본다. */
	simple_strtoul(drc_type, &endptr, 10);
	/* [한국어] 시작 지점에서 한 글자도 나아가지 못했다면 숫자로 시작하지 않는
	 * 문자열이라는 뜻이다. */
	if (endptr == drc_type)
		/* [한국어] 핫플러그 슬롯이 아니라고 보고한다. */
		return 0;

	/* [한국어] 숫자로 읽혔으므로 PCI 핫플러그 슬롯이다. */
	return 1;
}

/* [한국어]
 * is_php_dn - 이 노드가 핫플러그 가능한 PCI 슬롯을 담고 있는지 판정한다
 *
 * @dn: 검사할 노드.
 * @indexes: DRC 인덱스 배열을 받을 곳.
 * @names: 이름 뭉치를 받을 곳.
 * @types: 종류 뭉치를 받을 곳. 판정에 성공했을 때만 채워진다.
 * @power_domains: 전원 도메인 배열을 받을 곳.
 * @return: 1 이면 핫플러그 슬롯, 0 이면 아니다.
 *
 * 판정과 속성 획득을 한 번에 하는 함수다. 옆의 상류 kernel-doc 이 밝히듯,
 * 내장(built-in) 슬롯은 DLPAR 로 뗄 수 있더라도 핫플러그 대상은 아니므로
 * 여기서 거짓을 돌려준다 — 그 구분이 DRC 종류가 숫자인지 여부로 이루어진다.
 * 종류 포인터만 성공 경로에서 따로 채우는 순서에 주의 — 판정에 실패하면
 * 호출자의 types 변수가 손대지 않은 채로 남는다. 나머지 셋은
 * get_children_props() 가 이미 채워 버렸다는 점에서 대칭이 아니다.
 * 실행 컨텍스트: 슬롯 등록 경로(프로세스 컨텍스트). 잠들지 않는다.
 *
 * 호출 체인:
 *   rpaphp_drc_add_slot() → [is_php_dn]
 *     → get_children_props(), is_php_type()
 */
/**
 * is_php_dn() - return 1 if this is a hotpluggable pci slot, else 0
 * @dn: target &device_node
 * @indexes: passed to get_children_props()
 * @names: passed to get_children_props()
 * @types: returned from get_children_props()
 * @power_domains:
 *
 * This routine will return true only if the device node is
 * a hotpluggable slot. This routine will return false
 * for built-in pci slots (even when the built-in slots are
 * dlparable.)
 */
static int is_php_dn(struct device_node *dn, const __be32 **indexes,
		     const __be32 **names, const __be32 **types,
		     const __be32 **power_domains)
{
	/* [한국어] 종류 뭉치를 임시로 받을 자리. 판정을 통과했을 때만 호출자에게 넘긴다. */
	const __be32 *drc_types;
	/* [한국어] 속성 획득 결과. */
	int rc;

	/* [한국어] 이 노드에서 v1 형식 속성 넷을 얻는다. 종류만 지역 변수로 받는 것이
	 * 위에 적은 비대칭의 원인이다. */
	rc = get_children_props(dn, indexes, names, &drc_types, power_domains);
	/* [한국어] 속성이 없으면 핫플러그 슬롯이 아니다. */
	if (rc < 0)
		/* [한국어] 거짓을 돌려준다. */
		return 0;

	/* [한국어] 첫 셀(개수)을 건너뛴 첫 종류 문자열이 숫자로 읽히는지 본다.
	 * 한 노드 아래 슬롯들의 종류가 모두 같다고 보고 첫 항목만 검사하는 셈이다. */
	if (!is_php_type((char *) &drc_types[1]))
		/* [한국어] 숫자가 아니면 내장 슬롯 등 다른 종류이므로 거짓을 돌려준다. */
		return 0;

	/* [한국어] 통과했을 때만 종류 포인터를 호출자에게 넘긴다. */
	*types = drc_types;
	/* [한국어] 핫플러그 슬롯이라고 보고한다. */
	return 1;
}

/* [한국어]
 * rpaphp_drc_info_add_slot - v2 형식 노드에서 슬롯 하나를 만들어 등록한다
 *
 * @dn: 슬롯을 서술한 디바이스 트리 노드.
 * @return: 0 성공(또는 등록 대상이 아님), -ENOMEM, 등록 실패 코드.
 *
 * v2 형식을 다루는 등록 경로다. 아래 v1 판과 견주면 차이가 뚜렷하다 —
 * v1 은 한 노드가 슬롯 여러 개를 서술해 루프를 돌지만, 이쪽은 첫 항목 하나만
 * 읽어 슬롯 하나를 만든다.
 * 등록 절차는 두 형식이 같다: 슬롯 구조체를 만들고 → 종류를 채우고 →
 * 활성화(센서 조회와 필요하면 PCI 열거)한 뒤 → 공용 코어에 등록한다.
 * 어느 단계든 실패하면 구조체를 되돌린다.
 * '등록 대상이 아님' 과 '실패' 를 모두 다루지만 전자는 0 을 돌려준다는 점에
 * 주의 — 속성이 없거나 종류가 핫플러그가 아니면 조용히 성공으로 돌아간다.
 * 실행 컨텍스트: 부팅 경로 또는 DLPAR 경로(프로세스 컨텍스트). 활성화 단계에서
 * RTAS 호출과 PCI 열거가 일어나 잠들 수 있다.
 *
 * [상류 코드 관찰] 속성에 항목이 여럿 있어도 첫 항목만 읽는다. v2 형식이
 * 인덱스 구간으로 여러 슬롯을 묶을 수 있는데도 구간의 시작 인덱스와 시작
 * 이름만 써서 슬롯 하나를 만든다 — 그것이 의도인지는 코드만으로 알 수 없다.
 *
 * 호출 체인:
 *   rpaphp_add_slot() → [rpaphp_drc_info_add_slot]
 *     → of_find_property(), of_prop_next_u32(), of_read_drc_info_cell(),
 *       is_php_type(), alloc_slot_struct(), rpaphp_enable_slot(),
 *       rpaphp_register_slot(), dealloc_slot_struct()
 */
static int rpaphp_drc_info_add_slot(struct device_node *dn)
{
	/* [한국어] 이번에 만들 슬롯 객체. */
	struct slot *slot;
	/* [한국어] 이 노드의 ibm,drc-info 속성. */
	struct property *info;
	/* [한국어] 그 속성에서 풀어 낸 항목 하나. */
	struct of_drc_info drc;
	/* [한국어] 접두사와 번호로 조립할 슬롯 이름. */
	char drc_name[MAX_DRC_NAME_LEN];
	/* [한국어] 속성 값을 훑는 커서. */
	const __be32 *cur;
	/* [한국어] 속성에 든 항목 개수. 읽기는 하지만 아래에서 반복에 쓰지는 않는다. */
	u32 count;
	/* [한국어] 각 단계의 결과. */
	int retval = 0;

	/* [한국어] v2 속성을 찾는다. */
	info = of_find_property(dn, "ibm,drc-info", NULL);
	/* [한국어] 없으면 v2 노드가 아니다. */
	if (!info)
		/* [한국어] 오류가 아니라 '해당 없음' 이므로 성공으로 돌아간다 — 부팅 시 모든
		 * pci 노드를 훑는 경로에서 이 반환이 대부분을 차지한다. */
		return 0;

	/* [한국어] 첫 값(항목 개수)을 읽는다. */
	cur = of_prop_next_u32(info, NULL, &count);
	/* [한국어] 읽혔으면 */
	if (cur)
		/* [한국어] 커서를 한 칸 밀어 실제 항목을 가리키게 한다. */
		cur++;
	/* [한국어] 속성이 비어 있으면 */
	else
		/* [한국어] 역시 조용히 돌아간다. */
		return 0;

	/* [한국어] 첫 항목을 구조체로 푼다(위 관찰대로 첫 항목만 본다). */
	of_read_drc_info_cell(&info, &cur, &drc);
	/* [한국어] 그 종류가 PCI 핫플러그 슬롯의 것인지 확인한다. */
	if (!is_php_type(drc.drc_type))
		/* [한국어] 아니면 등록 대상이 아니므로 조용히 돌아간다. */
		return 0;

	/* [한국어] 접두사에 시작 번호를 붙여 슬롯 이름을 만든다. v2 는 이름을 저장하지
	 * 않고 이렇게 조립한다. 길이 검사가 없는 점은 v2 검증 함수의 관찰과 같다. */
	sprintf(drc_name, "%s%d", drc.drc_name_prefix, drc.drc_name_suffix_start);

	/* [한국어] 슬롯 구조체를 만든다. 인덱스는 구간의 시작값, 이름은 방금 조립한 것,
	 * 전원 도메인은 항목에 들어 있던 값을 쓴다. 이 함수가 이름을 복사해 두므로
	 * 지역 버퍼를 넘겨도 안전하다(구현은 rpaphp_slot.c). */
	slot = alloc_slot_struct(dn, drc.drc_index_start, drc_name, drc.drc_power_domain);
	/* [한국어] 메모리 부족. */
	if (!slot)
		return -ENOMEM;

	/* [한국어] 종류 코드를 숫자로 바꿔 담는다. 구조체를 만들 때가 아니라 지금 채우는
	 * 것은 alloc 이 종류를 인자로 받지 않기 때문이다. 이 값이 나중에 버스 속도
	 * 계산에 쓰인다. */
	slot->type = simple_strtoul(drc.drc_type, NULL, 10);
	/* [한국어] 슬롯을 활성화한다 — 전원 준위와 센서를 확인하고, 카드가 있고 버스가
	 * 비어 있으면 PCI 열거까지 한다(구현은 rpaphp_pci.c). */
	retval = rpaphp_enable_slot(slot);
	/* [한국어] 활성화가 성공했을 때만 */
	if (!retval)
		/* [한국어] 공용 핫플러그 코어에 등록해 sysfs 슬롯을 만든다. 순서가 중요하다 —
		 * 등록이 먼저면 아직 상태가 정해지지 않은 슬롯이 사용자에게 노출된다. */
		retval = rpaphp_register_slot(slot);

	/* [한국어] 어느 단계든 실패했으면 */
	if (retval)
		/* [한국어] 구조체를 되돌린다. 등록 전이므로 공용 코어에서 뗄 필요는 없다. */
		dealloc_slot_struct(slot);

	/* [한국어] 결과를 호출자에게 전달한다. */
	return retval;
}

/* [한국어]
 * rpaphp_drc_add_slot - v1 형식 노드에서 슬롯들을 만들어 등록한다
 *
 * @dn: 슬롯들을 서술한 디바이스 트리 노드.
 * @return: 0 또는 마지막 슬롯의 등록 결과(아래 관찰 참조).
 *
 * v1 형식에서는 한 노드가 슬롯 여러 개를 병렬 배열로 서술하므로, 그 개수만큼
 * 루프를 돌며 슬롯을 하나씩 만든다. 슬롯 하나를 만드는 절차 자체는 v2 판과
 * 같다 — 구조체 생성 → 종류 채우기 → 활성화 → 등록 → 실패 시 되돌리기.
 * v1 의 성가신 점이 루프 끝의 두 줄에 있다. 인덱스와 전원 도메인은 첨자로
 * 접근되지만 이름과 종류는 널로 끝나는 문자열이 이어 붙은 형태라, 반복마다
 * 문자열 길이만큼 커서를 밀어야 한다.
 * 실행 컨텍스트: 부팅 경로 또는 DLPAR 경로(프로세스 컨텍스트). 슬롯마다
 * RTAS 호출과 PCI 열거가 일어나 잠들 수 있다.
 *
 * [상류 코드 관찰] 옆의 상류 XXX FIXME 주석이 밝히듯, 중간 슬롯이 실패해도
 * 루프가 계속되고 retval 이 다음 반복에서 덮여, 결국 마지막 슬롯의 결과만
 * 반환된다. 또 alloc 실패만은 곧바로 -ENOMEM 으로 빠져나가는데, 그때 앞서
 * 등록한 슬롯들을 되돌리는 코드는 없다.
 *
 * 호출 체인:
 *   rpaphp_add_slot() → [rpaphp_drc_add_slot]
 *     → is_php_dn(), alloc_slot_struct(), rpaphp_enable_slot(),
 *       rpaphp_register_slot(), dealloc_slot_struct()
 */
static int rpaphp_drc_add_slot(struct device_node *dn)
{
	/* [한국어] 이번 반복에서 만들 슬롯. */
	struct slot *slot;
	/* [한국어] 각 단계의 결과(위 관찰대로 마지막 반복의 값만 남는다). */
	int retval = 0;
	/* [한국어] 슬롯 순회 커서. */
	int i;
	/* [한국어] 노드에서 얻을 네 배열. 모두 빅엔디언 32비트 값을 가리킨다. */
	const __be32 *indexes, *names, *types, *power_domains;
	/* [한국어] 이름과 종류 문자열을 훑어 갈 커서. */
	char *name, *type;

	/* If this is not a hotplug slot, return without doing anything. */
	/* [한국어] 옆의 상류 주석대로, 핫플러그 슬롯이 아니면 아무것도 하지 않는다.
	 * 이 호출이 속성 넷을 얻는 일까지 겸한다. */
	if (!is_php_dn(dn, &indexes, &names, &types, &power_domains))
		/* [한국어] 조용히 성공으로 돌아간다 — 부팅 시 모든 pci 노드를 훑는 경로에서
		 * 대부분이 여기서 걸러진다. */
		return 0;

	/* [한국어] 진입 흔적. %pOF 는 device_node 를 경로 문자열로 출력하는 커널
	 * 포맷 지정자다. */
	dbg("Entry %s: dn=%pOF\n", __func__, dn);

	/* register PCI devices */
	/* [한국어] 옆의 상류 주석대로 슬롯들을 등록한다. 첫 셀(개수)을 건너뛴 자리부터
	 * 이름 문자열들이 이어진다. */
	name = (char *) &names[1];
	/* [한국어] 종류 문자열들도 같은 방식으로 첫 항목을 가리키게 한다. */
	type = (char *) &types[1];
	/* [한국어] 첫 셀에 든 개수만큼 반복한다. 빅엔디언이라 CPU 순서로 바꿔 읽는다. */
	for (i = 0; i < be32_to_cpu(indexes[0]); i++) {
		/* [한국어] 이번 슬롯의 DRC 인덱스를 담을 지역 변수. */
		int index;

		/* [한국어] i 번째 인덱스를 읽는다. 첫 셀이 개수라 항목은 [i + 1] 에 있다. */
		index = be32_to_cpu(indexes[i + 1]);
		/* [한국어] 슬롯 구조체를 만든다. 이름은 현재 커서가 가리키는 문자열이고,
		 * 전원 도메인도 같은 규칙으로 [i + 1] 자리에서 읽는다. */
		slot = alloc_slot_struct(dn, index, name,
					 be32_to_cpu(power_domains[i + 1]));
		/* [한국어] 메모리 부족이면 */
		if (!slot)
			/* [한국어] 곧바로 빠져나간다. 앞서 등록한 슬롯을 되돌리지 않는 점은 위 관찰 참조. */
			return -ENOMEM;

		/* [한국어] 종류 문자열을 숫자로 바꿔 담는다. 버스 속도 계산에 쓰인다. */
		slot->type = simple_strtoul(type, NULL, 10);

		/* [한국어] 어떤 슬롯을 찾았는지 세 값을 함께 남긴다. 디바이스 트리 파싱은
		 * 눈에 보이지 않아 이 로그가 진단에 유용하다. */
		dbg("Found drc-index:0x%x drc-name:%s drc-type:%s\n",
				index, name, type);

		/* [한국어] 슬롯을 활성화한다(센서 조회, 필요 시 PCI 열거). */
		retval = rpaphp_enable_slot(slot);
		/* [한국어] 성공했을 때만 */
		if (!retval)
			/* [한국어] 공용 코어에 등록한다. v2 판과 같은 순서다. */
			retval = rpaphp_register_slot(slot);

		/* [한국어] 어느 단계든 실패했으면 */
		if (retval)
			/* [한국어] 이 슬롯만 되돌린다. 루프는 계속되므로 다음 슬롯은 그대로 시도된다. */
			dealloc_slot_struct(slot);

		/* [한국어] 이름 커서를 다음 문자열로 민다. 널 종료 문자까지 건너뛰려고 1 을 더한다. */
		name += strlen(name) + 1;
		/* [한국어] 종류 커서도 함께 민다. 두 뭉치가 인덱스 배열과 같은 순서라는 전제다. */
		type += strlen(type) + 1;
	}
	/* [한국어] 종료 흔적과 마지막 결과를 남긴다. */
	dbg("%s - Exit: rc[%d]\n", __func__, retval);

	/* XXX FIXME: reports a failure only if last entry in loop failed */
	/* [한국어] 옆의 상류 XXX FIXME 주석대로, 마지막 슬롯의 결과만 반환된다. */
	return retval;
}

/* [한국어]
 * rpaphp_add_slot - 디바이스 트리 노드 하나를 핫플러그 슬롯으로 등록한다
 *
 * @dn: 슬롯일 수도 있는 노드.
 * @return: 0 성공 또는 해당 없음, 음수는 등록 실패.
 *
 * 옆의 상류 kernel-doc 이 이 함수의 자리를 잘 요약해 두었다 — 부팅 시에도
 * 불리고 DLPAR 이 슬롯을 붙일 때도 불리는 공통 진입점이며, 내장 슬롯이면
 * 아무것도 하지 않고 돌아간다. 짝이 되는 제거는 rpaphp_deregister_slot() 이다.
 * 하는 일은 두 번의 갈림뿐이다. 먼저 노드 이름이 "pci" 인지 보고, 그 다음
 * DRC 속성 형식이 v2 인지 v1 인지 보아 알맞은 판에 넘긴다.
 * 이 함수가 EXPORT_SYMBOL_GPL 로 내보내지는 이유가 중요하다 — 상위 계층인
 * rpadlpar_core.c 가 PCI 슬롯이나 PHB 를 파티션에 붙인 뒤 이 함수를 불러
 * 핫플러그 슬롯을 만든다. rpaphp_check_drc_props() 와 함께 DLPAR 과 rpaphp 를
 * 잇는 두 접점이다.
 * 실행 컨텍스트: 부팅 경로 또는 DLPAR 경로(프로세스 컨텍스트). 잠들 수 있다.
 *
 * 호출 체인:
 *   rpaphp_init() 또는 rpadlpar_core.c 의 dlpar_add_pci_slot()/dlpar_add_phb()
 *     → [rpaphp_add_slot]
 *     → rpaphp_drc_info_add_slot() 또는 rpaphp_drc_add_slot()
 */
/**
 * rpaphp_add_slot -- declare a hotplug slot to the hotplug subsystem.
 * @dn: device node of slot
 *
 * This subroutine will register a hotpluggable slot with the
 * PCI hotplug infrastructure. This routine is typically called
 * during boot time, if the hotplug slots are present at boot time,
 * or is called later, by the dlpar add code, if the slot is
 * being dynamically added during runtime.
 *
 * If the device node points at an embedded (built-in) slot, this
 * routine will just return without doing anything, since embedded
 * slots cannot be hotplugged.
 *
 * To remove a slot, it suffices to call rpaphp_deregister_slot().
 */
int rpaphp_add_slot(struct device_node *dn)
{
	/* [한국어] 이름이 "pci" 인 노드만 다룬다. 부팅 경로는 이미 그 이름으로 훑지만,
	 * DLPAR 경로는 임의의 노드를 넘길 수 있어 여기서 한 번 더 거른다. */
	if (!of_node_name_eq(dn, "pci"))
		/* [한국어] 아니면 조용히 성공으로 돌아간다. */
		return 0;

	/* [한국어] DRC 속성 형식을 판별한다. 이 노드에 ibm,drc-info 가 있으면 v2 다.
	 * 검증 함수(rpaphp_check_drc_props)가 '부모' 를 보는 것과 달리 등록 경로는
	 * '자기 자신' 을 보는데, 두 형식에서 속성이 놓이는 층이 다르기 때문이다. */
	if (of_property_present(dn, "ibm,drc-info"))
		/* [한국어] v2 판에 넘긴다. */
		return rpaphp_drc_info_add_slot(dn);
	/* [한국어] 아니면 옛 형식이다. */
	else
		/* [한국어] v1 판에 넘긴다. */
		return rpaphp_drc_add_slot(dn);
}
/* [한국어] rpadlpar_io 가 별도 모듈이라 이 심볼을 내보내야 그쪽에서 부를 수 있다. */
EXPORT_SYMBOL_GPL(rpaphp_add_slot);

/* [한국어]
 * cleanup_slots - 등록된 슬롯을 모두 걷어낸다
 *
 * @return: 없음.
 *
 * 모듈 제거 시에만 불리므로 __exit 이 붙는다 — 모듈로 빌드되지 않으면 이 코드는
 * 아예 빠진다.
 * 슬롯 하나를 걷어내는 순서(리스트에서 떼기 → 공용 코어 등록 해제 → 구조체
 * 반납)가 정해져 있다. 공용 코어 해제가 구조체 반납보다 먼저여야, sysfs 를 통해
 * 이미 사라진 슬롯에 접근하는 일이 없다.
 * 순회 중에 노드를 지우므로 안전한 순회 판을 써야 한다 — 지운 노드의 다음
 * 포인터를 따라가면 안 되기 때문이다.
 * 실행 컨텍스트: 모듈 제거(프로세스 컨텍스트). 잠들 수 있다.
 *
 * [상류 코드 관찰] 전역 리스트를 지키는 락이 없다. 모듈 제거 시점에는 다른
 * 접근이 없다는 전제이지만, 그것을 강제하는 장치는 코드에 없다.
 *
 * 호출 체인:
 *   rpaphp_exit() → [cleanup_slots]
 *     → pci_hp_deregister(), dealloc_slot_struct()
 */
static void __exit cleanup_slots(void)
{
	/* [한국어] 순회 커서와, 안전한 순회를 위해 다음 노드를 미리 담아 둘 자리. */
	struct slot *slot, *next;

	/*
	 * Unregister all of our slots with the pci_hotplug subsystem,
	 * and free up all memory that we had allocated.
	 */

	/* [한국어] 위 상류 주석대로 모든 슬롯을 걷어낸다. 지우면서 순회하므로 안전한
	 * 판을 쓴다. */
	list_for_each_entry_safe(slot, next, &rpaphp_slot_head,
				 rpaphp_slot_list) {
		/* [한국어] 먼저 전역 리스트에서 뗀다. */
		list_del(&slot->rpaphp_slot_list);
		/* [한국어] 공용 코어에서 등록을 해제해 sysfs 슬롯을 없앤다. 구조체를 풀기 전에
		 * 해야 사용자가 사라진 객체에 닿지 않는다. */
		pci_hp_deregister(&slot->hotplug_slot);
		/* [한국어] 마지막으로 이름 문자열, DT 노드 참조, 구조체를 반납한다. */
		dealloc_slot_struct(slot);
	}
}

/* [한국어]
 * rpaphp_init - 모듈 진입점. 디바이스 트리를 훑어 슬롯을 모두 등록한다
 *
 * @return: 언제나 0.
 *
 * 이 드라이버에는 probe 가 없다. 슬롯을 알려 주는 버스도 인터럽트도 없고,
 * 정보의 출처가 디바이스 트리뿐이기 때문이다. 그래서 모듈이 올라오면 이름이
 * "pci" 인 노드를 전부 훑어 그중 핫플러그 슬롯인 것만 골라 등록한다.
 * 순회 매크로는 반복마다 이전 노드의 참조를 놓고 다음 노드의 참조를 잡아 주므로,
 * 끝까지 도는 한 참조가 새지 않는다. 이 루프에는 break 가 없어 그 조건이 지켜진다.
 * 실행 컨텍스트: 모듈 적재(프로세스 컨텍스트). 슬롯마다 RTAS 호출과 PCI 열거가
 * 일어나 잠들 수 있다.
 *
 * [상류 코드 관찰] rpaphp_add_slot() 의 반환값을 확인하지 않고 언제나 0 을
 * 돌려준다. 슬롯 하나가 실패해도 모듈 적재는 성공하며, 그 사실은 로그로만 남는다.
 * 대부분의 노드가 '해당 없음' 으로 0 을 돌려주는 구조라 실패를 가려내기 어려운
 * 것도 이유로 보이지만, 그 판단을 밝힌 주석은 코드에 없다.
 *
 * 호출 체인:
 *   module_init → [rpaphp_init] → rpaphp_add_slot()
 */
static int __init rpaphp_init(void)
{
	/* [한국어] 순회 커서. 매크로가 참조 카운트까지 관리한다. */
	struct device_node *dn;

	/* [한국어] 어떤 드라이버가 올라왔는지 버전과 함께 남긴다. 문자열 리터럴을
	 * 이어 붙이는 C 문법으로 설명과 버전을 한 줄로 만든다. */
	info(DRIVER_DESC " version: " DRIVER_VERSION "\n");

	/* [한국어] 이름이 "pci" 인 노드를 모두 훑는다. 이 매크로가 반복마다 이전 노드의
	 * 참조를 놓고 다음 노드의 참조를 잡으므로, 중간에 빠져나가지 않는 한 참조가
	 * 새지 않는다. */
	for_each_node_by_name(dn, "pci")
		/* [한국어] 노드 하나를 슬롯으로 등록해 본다. 핫플러그 슬롯이 아니면 그 안에서
		 * 조용히 돌아간다. 반환값을 보지 않는 점은 위 관찰 참조. */
		rpaphp_add_slot(dn);

	/* [한국어] 언제나 성공을 알린다. */
	return 0;
}

/* [한국어]
 * rpaphp_exit - 모듈 종료점. 등록한 슬롯을 모두 걷어낸다
 *
 * @return: 없음.
 *
 * 정리 작업을 통째로 cleanup_slots() 에 맡기는 한 줄짜리 껍데기다. 둘로 나눈
 * 것은 정리 로직 자체를 모듈 종료 경로와 분리해 두려는 것으로 읽히지만,
 * 현재 cleanup_slots() 를 부르는 곳은 여기뿐이다.
 * 실행 컨텍스트: 모듈 제거(프로세스 컨텍스트). 잠들 수 있다.
 *
 * 호출 체인:
 *   module_exit → [rpaphp_exit] → cleanup_slots()
 */
static void __exit rpaphp_exit(void)
{
	/* [한국어] 남은 슬롯을 모두 걷어낸다. */
	cleanup_slots();
}

/* [한국어]
 * enable_slot - 슬롯을 켠다. 카드가 있으면 PCI 열거까지 수행한다
 *
 * @hotplug_slot: 공용 코어가 넘겨 주는 슬롯 객체.
 * @return: 0 성공, 센서 조회 오류, -EINVAL(센서 값이 예상 밖).
 *
 * 이 파일에서 가장 무거운 동작이다. 사용자가 sysfs 로 슬롯을 켜면 실제 PCI
 * 장치가 커널에 등장한다.
 * 순서에 뜻이 있다. (1) 이미 구성된 슬롯이면 아무것도 하지 않는다 — 두 번
 * 열거하면 장치가 중복 등록된다. (2) 센서로 카드 유무를 확인한다. (3) 카드가
 * 있으면 먼저 EEH(Enhanced Error Handling) 자료구조를 만들어 둔다. 이것이
 * 열거보다 먼저여야 하는 이유는, 장치가 등장한 뒤에는 이미 오류를 겪을 수 있어
 * 그 전에 오류 처리 준비가 끝나 있어야 하기 때문으로 읽힌다. (4) 재스캔 락을
 * 잡고 열거한다. (5) 마지막에 버스 최대 속도를 채운다.
 * 재스캔 락은 PCI 코어 전역의 것으로, 열거와 제거가 서로 겹치지 않게 한다.
 * 실행 컨텍스트: 사용자의 sysfs 쓰기(프로세스 컨텍스트). RTAS 호출과 PCI 열거가
 * 모두 잠들 수 있다.
 * 에러 경로: 센서가 예상 밖의 값을 주면 상태를 NOT_VALID 로 두고 -EINVAL 을
 * 돌려준다.
 *
 * [상류 코드 관찰] 마지막의 버스 속도 대입이 세 분기 밖에 있어, 카드가 없는
 * 슬롯에서도 실행된다. 그때 slot->bus 는 rpaphp_enable_slot() 이 등록 시점에
 * 채워 둔 값이다.
 *
 * 호출 체인:
 *   사용자의 sysfs 쓰기 → 공용 코어 → [enable_slot]
 *     → to_slot(), rpaphp_get_sensor_state(),
 *       pseries_eeh_init_edev_recursive(), pci_hp_add_devices(),
 *       get_max_bus_speed()
 */
static int enable_slot(struct hotplug_slot *hotplug_slot)
{
	/* [한국어] 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);
	/* [한국어] 센서가 돌려줄 카드 유무 값. */
	int state;
	/* [한국어] 센서 조회 결과. */
	int retval;

	/* [한국어] 이미 구성이 끝난 슬롯이면 */
	if (slot->state == CONFIGURED)
		/* [한국어] 아무것도 하지 않고 성공으로 돌아간다. 두 번 열거하면 같은 장치가
		 * 중복 등록되기 때문이다. */
		return 0;

	/* [한국어] 펌웨어 센서에 카드가 있는지 묻는다. */
	retval = rpaphp_get_sensor_state(slot, &state);
	/* [한국어] 조회 자체가 실패했으면 */
	if (retval)
		/* [한국어] 그 오류를 전달한다. */
		return retval;

	/* [한국어] 카드가 꽂혀 있다면 */
	if (state == PRESENT) {
		/* [한국어] 먼저 EEH 자료구조를 이 노드 아래로 재귀적으로 만들어 둔다. EEH 는
		 * POWER 의 PCI 오류 격리·복구 기능으로, 장치가 등장한 뒤에는 이미 오류를 겪을
		 * 수 있으므로 열거보다 먼저 준비되어야 한다. PCI_DN 매크로로 노드에서 PowerPC
		 * 전용 정보를 꺼내 넘기며, 그 구현은 arch/powerpc 가 이 트리에 없어 확인 못 함. */
		pseries_eeh_init_edev_recursive(PCI_DN(slot->dn));

		/* [한국어] PCI 코어 전역의 재스캔·제거 락을 잡는다. 열거 도중에 다른 경로가
		 * 장치를 제거하는 것을 막는다. */
		pci_lock_rescan_remove();
		/* [한국어] 이 슬롯의 버스 아래를 훑어 장치를 등록한다. 핫플러그 전용 열거
		 * 경로이며, 선언 위치는 이 트리에서 찾지 못했다(위 include 주석 참조). */
		pci_hp_add_devices(slot->bus);
		/* [한국어] 락을 푼다. */
		pci_unlock_rescan_remove();
		/* [한국어] 구성이 끝났음을 기록한다. 위의 중복 열거 방지 검사가 이 값을 본다. */
		slot->state = CONFIGURED;
	/* [한국어] 카드가 없다면 */
	} else if (state == EMPTY) {
		/* [한국어] 비어 있음으로만 기록하고 아무것도 하지 않는다. 오류가 아니다 —
		 * 빈 슬롯을 켜는 것은 정상적인 요청이다. */
		slot->state = EMPTY;
	/* [한국어] 센서가 아는 두 값 중 어느 것도 아니라면 */
	} else {
		/* [한국어] 어느 슬롯이 이상한지 남기고 */
		err("%s: slot[%s] is in invalid state\n", __func__, slot->name);
		/* [한국어] '알 수 없음' 으로 표시한 뒤 */
		slot->state = NOT_VALID;
		/* [한국어] 잘못된 상태로 보고한다. */
		return -EINVAL;
	}

	/* [한국어] 슬롯 종류 코드로 계산한 최대 버스 속도를 채운다. 사용자 공간이 이 값을
	 * sysfs 로 읽는다. 세 분기 밖에 있어 빈 슬롯에서도 실행된다(위 관찰 참조). */
	slot->bus->max_bus_speed = get_max_bus_speed(slot);
	/* [한국어] 슬롯 켜기 성공. */
	return 0;
}

/* [한국어]
 * disable_slot - 슬롯을 끈다. 그 아래 PCI 장치를 모두 제거한다
 *
 * @hotplug_slot: 공용 코어가 넘겨 주는 슬롯 객체.
 * @return: 0 성공, -EINVAL(이미 꺼져 있음).
 *
 * enable 의 짝이다. 재스캔 락을 잡고 버스 아래 장치를 모두 떼어 낸 뒤,
 * 남아 있는 vmalloc 별칭 매핑을 정리한다. 그 마지막 단계가 중요한 이유는
 * 장치가 사라진 뒤에도 그 MMIO 영역을 가리키는 커널 매핑이 남아 있으면
 * 이후 접근이 사라진 하드웨어에 닿게 되기 때문으로 읽힌다.
 * enable 과 달리 센서를 확인하지 않는다 — 카드가 물리적으로 남아 있든 아니든
 * 커널 쪽 표현을 걷어내는 것이 이 동작의 목적이기 때문이다.
 * 실행 컨텍스트: 사용자의 sysfs 쓰기(프로세스 컨텍스트). 장치 제거가 잠들 수 있다.
 *
 * [상류 코드 관찰] '이미 꺼져 있음' 판정이 NOT_CONFIGURED 하나만 본다.
 * 빈 슬롯(EMPTY)이나 알 수 없는 상태(NOT_VALID)에서는 그대로 제거 경로로
 * 들어가며, 그때 slot->bus 가 무엇인지에 따라 결과가 달라진다.
 *
 * 호출 체인:
 *   사용자의 sysfs 쓰기 → 공용 코어 → [disable_slot]
 *     → to_slot(), pci_hp_remove_devices(), vm_unmap_aliases()
 */
static int disable_slot(struct hotplug_slot *hotplug_slot)
{
	/* [한국어] 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);
	/* [한국어] 이미 구성이 풀린 슬롯이면 할 일이 없다(위 관찰 참조). */
	if (slot->state == NOT_CONFIGURED)
		/* [한국어] 잘못된 요청으로 보고한다. */
		return -EINVAL;

	/* [한국어] PCI 코어 전역의 재스캔·제거 락을 잡는다. 제거 도중 다른 경로가
	 * 같은 버스를 훑는 것을 막는다. */
	pci_lock_rescan_remove();
	/* [한국어] 이 슬롯의 버스 아래 장치를 모두 떼어 낸다. */
	pci_hp_remove_devices(slot->bus);
	/* [한국어] 락을 푼다. */
	pci_unlock_rescan_remove();
	/* [한국어] 장치가 사라진 뒤 남아 있을 수 있는 vmalloc 별칭 매핑을 정리한다.
	 * 사라진 하드웨어를 가리키는 매핑이 남지 않게 하려는 것으로 읽히며,
	 * 상위 계층인 rpadlpar_core.c 의 제거 경로도 같은 함수를 부른다. */
	vm_unmap_aliases();

	/* [한국어] 구성이 풀렸음을 기록한다. */
	slot->state = NOT_CONFIGURED;
	/* [한국어] 슬롯 끄기 성공. */
	return 0;
}

/* [한국어] 이 드라이버가 공용 핫플러그 코어에 제공하는 콜백 표. 슬롯마다 이 같은
 * 표를 가리키므로, 콜백들은 받은 포인터에서 to_slot() 으로 자기 슬롯을 되찾는다.
 * const 이고 비정적인 것은 rpaphp_slot.c 의 alloc_slot_struct() 가 이 표를
 * 슬롯에 걸기 때문이다(선언은 rpaphp.h 에 있다). */
const struct hotplug_slot_ops rpaphp_hotplug_slot_ops = {
	/* [한국어] 슬롯 켜기 — PCI 열거까지 수행한다. */
	.enable_slot = enable_slot,
	/* [한국어] 슬롯 끄기 — 장치 제거까지 수행한다. */
	.disable_slot = disable_slot,
	/* [한국어] attention LED 설정. RTAS 호출로 처리된다. */
	.set_attention_status = set_attention_status,
	/* [한국어] 전원 상태 조회. 매번 펌웨어에 다시 묻는다. */
	.get_power_status = get_power_status,
	/* [한국어] attention LED 상태 조회. 기억해 둔 값을 돌려준다. */
	.get_attention_status = get_attention_status,
	/* [한국어] 어댑터 유무와 구성 상태 조회. 센서와 소프트웨어 상태를 합쳐 답한다.
	 * [상류 코드 관찰] 공용 코어가 요구하는 콜백 중 get_latch_status 와
	 * reset_slot 은 채우지 않는다 — 이 플랫폼에 래치 개념이 없고 슬롯 리셋도
	 * 펌웨어 소관이기 때문으로 읽히지만, 그 이유를 밝힌 주석은 코드에 없다. */
	.get_adapter_status = get_adapter_status,
};

/* [한국어] 모듈이 올라올 때 부를 함수를 등록한다. */
module_init(rpaphp_init);
/* [한국어] 모듈이 내려갈 때 부를 함수를 등록한다. */
module_exit(rpaphp_exit);
