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
/* [한국어] of_node 관련 정의. %pOF 서식과 device_node 필드 접근에 필요하다. */
/*
 * [한국어 설명] RPA 핫플러그의 RTAS 센서 조회와 PCI 열거 (rpaphp_pci.c)
 *
 * === 파일의 역할 ===
 * 슬롯에 카드가 실제로 꽂혀 있는지 펌웨어에 묻고, 있으면 그 아래 PCI 장치를
 * 열거해 커널에 등록하는 파일이다. 같은 드라이버의 rpaphp_slot.c 가 순수한
 * 객체 관리를 맡는 것과 달리, 이쪽은 RTAS(펌웨어)와 PCI 코어 양쪽을 실제로
 * 호출한다.
 * 세 가지가 이 파일의 핵심이다. 첫째, RTAS 오류 코드를 리눅스 errno 로
 * 번역하는 경계(rtas_get_sensor_errno) — 두 체계의 숫자가 충돌하므로 이
 * 변환 없이는 상위 코드가 값을 잘못 읽는다. 둘째, EEH 복구 중에는 재시도를
 * 내장한 표준 래퍼를 우회하고 rtas_call() 을 직접 부르는 경로 — 파일 안의 긴
 * 영어 주석이 그 배경을 설명한다. 표준 래퍼는 확장 지연(9902) 동안 약 6초를
 * 재시도로 소비하는데, 그 사이 EEH 핸들러가 멈춰 복구가 실패하는 일이 있었다.
 * 셋째, 상태 조회가 상태를 바꾸는 부수 효과 — 일부 슬롯은 전원이 들어와야
 * 센서를 읽을 수 있어서, rpaphp_get_sensor_state() 가 스스로 전원을 켜고
 * 다시 읽는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * IBM POWER 의 PCI 핫플러그 스택은 세 층이다. 위에는 sysfs 슬롯 인터페이스를
 * 제공하는 공용 코어 pci_hotplug_core.c 가 있고, 아래에는 RTAS(Run-Time
 * Abstraction Services)라는 펌웨어 호출 인터페이스가 있다. rpaphp 는 그 사이에서
 * 공용 코어의 콜백을 RTAS 호출로 번역하는 어댑터다. 다른 플랫폼의 핫플러그
 * 드라이버가 MMIO 레지스터를 직접 두드리는 것과 달리, 이 드라이버는 하드웨어
 * 레지스터를 하나도 만지지 않는다 — 슬롯 전원도, LED 도, 카드 유무 감지도
 * 전부 펌웨어에 요청한다. 슬롯 정보의 출처도 config space 가 아니라 디바이스
 * 트리이며, 각 슬롯은 DRC(Dynamic Reconfiguration Connector)라는 논리
 * 식별자로 구분된다.
 * 드라이버는 네 파일로 나뉜다 — rpaphp_core.c(모듈 진입점, sysfs 콜백 구현,
 * DT 순회), rpaphp_slot.c(슬롯 구조체의 생성·등록·해제), rpaphp_pci.c(RTAS
 * 센서 조회와 PCI 장치 열거), 그리고 공용 정의를 담은 rpaphp.h.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. RTAS 호출과 PCI 열거가 잠들 수 있어
 * 인터럽트 문맥에서는 어느 함수도 부를 수 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: linux/pci_hotplug.h 의 struct hotplug_slot 과 pci_hp_register/deregister.
 * 이 드라이버의 struct slot 이 hotplug_slot 을 값으로 내장해 to_slot() 이
 * container_of 로 역변환한다.
 * 아래쪽: asm/rtas.h 의 rtas_token()/rtas_call()/rtas_get_sensor()/
 * rtas_get_power_level()/rtas_set_power_level(), asm/pci-bridge.h 의 PCI_DN()
 * 매크로와 pci_find_bus_by_node(), 그리고 EEH 서브시스템(eeh_dev_to_pe,
 * pseries_eeh_init_edev_recursive). 모두 PowerPC 전용이라 이 드라이버는
 * 아키텍처에 강하게 묶여 있다.
 * 옆쪽: drivers/pci/pci.h 의 pci_hp_add_devices() — 핫플러그 전용 열거 경로다.
 * 데이터 흐름: 디바이스 트리의 DRC 정보 → struct slot → 공용 코어의 sysfs 슬롯
 * → 사용자 조작 → RTAS 호출 → 펌웨어 → 하드웨어. 반대 방향으로는 RTAS 센서
 * 값이 slot->state 로, PCI 열거 결과가 slot->bus / pci_devs 로 들어온다.
 * 공유 상태: 전역 리스트 rpaphp_slot_head(정의는 rpaphp_core.c)와 디버그
 * 플래그 rpaphp_debug. 리스트 접근에 락이 없는데, 슬롯 추가·제거가 직렬화된다는
 * 전제 위에 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - rtas_get_sensor_errno(): RTAS 코드 → errno 번역표. -9000/-9001 을 -EFAULT 로
 *   접는데, 여기서 -EFAULT 는 "잘못된 주소"가 아니라 "전원부터 넣어라"라는
 *   내부 신호로 쓰인다. 확장 지연 990x 는 case 범위 문법으로 한 줄에 묶여 -EBUSY 가 된다.
 * - __rpaphp_get_sensor_state(): 센서 조회의 두 경로 분기. EEH 복구 중이면
 *   rtas_call() 직접 호출, 아니면 재시도 래퍼 rtas_get_sensor().
 * - rpaphp_get_sensor_state(): 위 함수를 부르고, -EFAULT/-EEXIST 면 슬롯 전원을
 *   켠 뒤 한 번 더 읽는다. 조회 함수이면서 하드웨어 상태를 바꾼다는 점에 주의.
 * - rpaphp_enable_slot(): 전원 조회 → 센서 조회 → PCI 버스 탐색 →
 *   (카드가 있으면) EEH 장치 정보 생성 → pci_hp_add_devices() →
 *   slot->state 를 CONFIGURED 로. 빈 슬롯을 확인한 것도 성공(0)이다.
 * - RTAS_SLOT_* 상수 세 개: 파일 앞의 영어 주석이 PAPR 규격의 반환값 표를
 *   그대로 옮겨 두었고, 그중 이 드라이버가 특별 취급하는 셋을 이름으로 뽑았다.
 * - [상류 코드 관찰, 수정하지 않음] rpaphp_enable_slot() 이
 *   rtas_get_power_level() 로 읽은 level 값을 이후 어디서도 쓰지 않는다.
 *   또 어댑터 자식 노드가 없어 -EINVAL 로 중단할 때, 이미 채운
 *   slot->bus 와 slot->pci_devs 를 되돌리지 않아 구조체가 부분 초기화 상태로 남는다.
 */

#include <linux/of.h>
/* [한국어] PCI 코어 공개 API — struct pci_bus, pci_name(), list 순회. */
#include <linux/pci.h>
/* [한국어] 문자열 함수 선언. */
#include <linux/string.h>

/* [한국어] PowerPC 전용 헤더. struct pci_controller 와 PCI_DN() 매크로,
 * pci_find_bus_by_node() 가 여기서 온다. 이 파일이 아키텍처에 강하게
 * 묶여 있음을 보여 주는 포함이다. */
#include <asm/pci-bridge.h>
/* [한국어] RTAS(펌웨어 호출) 인터페이스 — rtas_token(), rtas_call(),
 * rtas_get_sensor(), rtas_get_power_level(), rtas_set_power_level(),
 * 그리고 RTAS_BUSY / RTAS_EXTENDED_DELAY_* 상수. */
#include <asm/rtas.h>
/* [한국어] 머신별 후크 정의. 이 파일에서 직접 쓰지는 않지만 위 헤더들의 의존성이다. */
#include <asm/machdep.h>

/* [한국어] PCI 서브시스템 내부 헤더 — 옆의 상류 주석대로 pci_add_new_bus() 때문이며,
 * 실제로는 pci_hp_add_devices() 도 이 경로로 온다. */
#include "../pci.h"		/* for pci_add_new_bus */
/* [한국어] 이 드라이버의 자체 헤더 — struct slot, DR_ENTITY_SENSE, POWER_ON,
 * EMPTY/PRESENT, 슬롯 상태 상수, 로그 매크로. */
#include "rpaphp.h"

/*
 * RTAS call get-sensor-state(DR_ENTITY_SENSE) return values as per PAPR:
 * -- generic return codes ---
 *    -1: Hardware Error
 *    -2: RTAS_BUSY
 *    -3: Invalid sensor. RTAS Parameter Error.
 * -- rtas_get_sensor function specific return codes ---
 * -9000: Need DR entity to be powered up and unisolated before RTAS call
 * -9001: Need DR entity to be powered up, but not unisolated, before RTAS call
 * -9002: DR entity unusable
 *  990x: Extended delay - where x is a number in the range of 0-5
 */
/* [한국어] 위 상류 주석의 -9000 — DR 엔티티에 전원이 들어오고 격리가 풀려 있어야
 * 센서를 읽을 수 있다는 뜻이다. */
#define RTAS_SLOT_UNISOLATED		-9000
/* [한국어] -9001 — 전원은 필요하지만 격리 해제는 필요 없다는 변형. */
#define RTAS_SLOT_NOT_UNISOLATED	-9001
/* [한국어] -9002 — 그 DR 엔티티를 쓸 수 없다. */
#define RTAS_SLOT_NOT_USABLE		-9002

/* [한국어]
 * rtas_get_sensor_errno - RTAS 반환 코드를 리눅스 errno 로 번역한다
 *
 * @rtas_rc: rtas_call(get-sensor-state) 이 돌려준 값. 위 상류 주석이
 *       그 값들의 의미를 PAPR 규격에 따라 나열한다.
 * @return: 대응하는 리눅스 errno(0 또는 음수).
 *
 * 왜 필요한가: RTAS 는 IBM POWER 펌웨어의 호출 인터페이스이고 자기만의
 * 오류 코드 체계를 갖는다. 그 값을 그대로 상위로 올리면 리눅스 errno 와
 * 숫자가 충돌해 호출자가 완전히 잘못 해석한다. 이 함수가 그 경계를 지킨다.
 *
 * 번역표의 의도:
 *   - 0 → 0. 유일한 성공이다.
 *   - -9000 / -9001 (전원·격리 필요) → -EFAULT. 여기서 -EFAULT 는 "잘못된 주소"가
 *     아니라 "슬롯에 전원부터 넣고 다시 시도하라"는 신호로 쓰인다. 실제로
 *     rpaphp_get_sensor_state() 가 이 값을 보고 전원을 켠 뒤 재시도한다.
 *   - -9002 (사용 불가) → -ENODEV. 재시도해도 소용없다는 뜻이다.
 *   - RTAS_BUSY 와 확장 지연(990x) → -EBUSY. "나중에 다시"다.
 *     case 범위 문법(...)은 990x 다섯 값을 한 줄로 묶기 위한 GCC 확장이다.
 *   - 그 밖 → RTAS 공통 변환 함수에 위임.
 *
 * 실행 컨텍스트: __rpaphp_get_sensor_state() 안, 프로세스 컨텍스트.
 * 순수 값 변환이라 부수 효과가 없다.
 *
 * 에러 경로: 없다. 모든 입력에 대해 무언가를 돌려준다.
 *
 * 호출 체인:
 *   __rpaphp_get_sensor_state() → [rtas_get_sensor_errno] → rtas_error_rc()
 */
static int rtas_get_sensor_errno(int rtas_rc)
{
	/* [한국어] RTAS 가 돌려준 값을 리눅스 errno 로 번역한다. 두 오류 체계가 완전히 다르므로
	 * 이 변환이 없으면 상위 코드가 값을 잘못 해석한다. */
	switch (rtas_rc) {
	/* [한국어] 0 만이 성공이다. */
	case 0:
		/* Success case */
		/* [한국어] 성공을 그대로 0 으로. */
		return 0;
	/* [한국어] 전원/격리 문제 두 가지를, */
	case RTAS_SLOT_UNISOLATED:
	/* [한국어] 하나로 묶어, */
	case RTAS_SLOT_NOT_UNISOLATED:
		/* [한국어] -EFAULT 로 번역한다. 호출자는 이 값을 보고 슬롯에 전원을 넣은 뒤 재시도한다 —
		 * 즉 -EFAULT 가 여기서는 "접근 오류"가 아니라 "전원부터 넣어라"는 신호다. */
		return -EFAULT;
	/* [한국어] 쓸 수 없는 엔티티는, */
	case RTAS_SLOT_NOT_USABLE:
		/* [한국어] -ENODEV 로. 재시도해도 소용없다는 뜻이다. */
		return -ENODEV;
	/* [한국어] 펌웨어가 바쁘거나, */
	case RTAS_BUSY:
	/* [한국어] 확장 지연(990x)을 요청한 경우. ... 은 GCC 의 case 범위 확장 문법이다. */
	case RTAS_EXTENDED_DELAY_MIN...RTAS_EXTENDED_DELAY_MAX:
		/* [한국어] -EBUSY 로 번역해 "나중에 다시"를 알린다. */
		return -EBUSY;
	default:
		/* [한국어] 그 밖의 값은 RTAS 공통 변환 함수에 맡긴다. */
		return rtas_error_rc(rtas_rc);
	}
}

/*
 * get_adapter_status() can be called by the EEH handler during EEH recovery.
 * On certain PHB failures, the RTAS call rtas_call(get-sensor-state) returns
 * extended busy error (9902) until PHB is recovered by pHyp. The RTAS call
 * interface rtas_get_sensor() loops over the RTAS call on extended delay
 * return code (9902) until the return value is either success (0) or error
 * (-1). This causes the EEH handler to get stuck for ~6 seconds before it
 * could notify that the PCI error has been detected and stop any active
 * operations. This sometimes causes EEH recovery to fail. To avoid this issue,
 * invoke rtas_call(get-sensor-state) directly if the respective PE is in EEH
 * recovery state and return -EBUSY error based on RTAS return status. This
 * will help the EEH handler to notify the driver about the PCI error
 * immediately and successfully proceed with EEH recovery steps.
 */

/* [한국어]
 * __rpaphp_get_sensor_state - 슬롯에 카드가 꽂혀 있는지 펌웨어에 묻는다
 *
 * @slot: 대상 슬롯. slot->dn 으로 호스트 브리지를, slot->index 로 DRC 인덱스를 얻는다.
 * @state: 센서 값(EMPTY 또는 PRESENT)을 받을 출력 인자.
 * @return: 0 = 성공. -ENOENT = 펌웨어가 이 RTAS 서비스를 제공하지 않음.
 *       그 밖의 음수 = 센서 오류(errno 로 번역된 값).
 *
 * 왜 두 경로인가: 위 상류 주석이 그 이유를 자세히 설명한다. 평상시에는
 * 재시도를 내장한 rtas_get_sensor() 래퍼가 편하지만, EEH 복구 중에는 그
 * 재시도가 독이 된다. PHB 장애 시 펌웨어가 확장 지연(9902)을 계속 돌려주고,
 * 래퍼는 성공이나 하드 에러가 나올 때까지 약 6초를 재시도로 소비한다.
 * 그동안 EEH 핸들러가 멈춰 있어 드라이버에 오류를 알리지 못하고, 그 결과
 * EEH 복구 자체가 실패하는 일이 있었다.
 *
 * 동작 과정:
 *   1) RTAS 서비스 토큰을 얻는다. 없으면 -ENOENT.
 *   2) 호스트 브리지의 첫 자식 노드를 찾는다. 없으면(빈 슬롯이거나 아직
 *      열거되지 않음) EEH 판정을 할 수 없으므로 일반 경로로 간다.
 *   3) 그 장치의 EEH PE 가 복구 중이면, 래퍼를 건너뛰고 rtas_call() 을
 *      직접 부른다. 확장 지연이 와도 즉시 -EBUSY 로 번역해 돌려주므로
 *      EEH 핸들러가 바로 다음 단계로 넘어갈 수 있다.
 *   4) 그 밖에는 재시도 래퍼를 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. EEH 복구 경로에서도 불리므로,
 * 그 경우 오래 잠들지 않는 것이 이 함수 설계의 핵심 요구사항이다.
 *
 * 에러 경로: 토큰 부재만 자체 오류이고, 나머지는 RTAS 결과를 번역해 전달한다.
 *
 * 호출 체인:
 *   rpaphp_get_sensor_state() → [__rpaphp_get_sensor_state]
 *     → rtas_token() / eeh_dev_to_pe()
 *     → rtas_call() + rtas_get_sensor_errno()  (EEH 복구 중)
 *     → rtas_get_sensor()                      (평상시)
 */
static int __rpaphp_get_sensor_state(struct slot *slot, int *state)
{
	/* [한국어] RTAS 호출 결과. */
	int rc;
	/* [한국어] "get-sensor-state" RTAS 서비스의 토큰을 얻는다. RTAS 는 서비스마다
	 * 번호(토큰)를 갖고, 그 번호는 펌웨어가 부팅 시 정해 디바이스 트리에 실어 준다. */
	int token = rtas_token("get-sensor-state");
	/* [한국어] EEH 경로를 판별하기 위해 훑을 PCI DN 노드. */
	struct pci_dn *pdn;
	/* [한국어] EEH(Enhanced Error Handling) 파티션 엔드포인트. */
	struct eeh_pe *pe;
	/* [한국어] 이 슬롯이 속한 PCI 호스트 브리지. PCI_DN() 은 DT 노드에 붙어 있는
	 * PowerPC 전용 PCI 정보를 꺼내는 매크로다. */
	struct pci_controller *phb = PCI_DN(slot->dn)->phb;

	/* [한국어] 펌웨어가 이 서비스를 제공하지 않으면 아무것도 할 수 없다. */
	if (token == RTAS_UNKNOWN_SERVICE)
		/* [한국어] "그런 서비스 없음"을 뜻하는 -ENOENT. */
		return -ENOENT;

	/*
	 * Fallback to existing method for empty slot or PE isn't in EEH
	 * recovery.
	 */
	/* [한국어] 위 상류 주석이 길게 설명하는 EEH 회피 경로의 시작이다.
	 * 브리지의 첫 자식 노드를 찾아 그 EEH 상태를 확인한다. */
	pdn = list_first_entry_or_null(&PCI_DN(phb->dn)->child_list,
					struct pci_dn, list);
	/* [한국어] 자식이 없으면(빈 슬롯) EEH 판정을 할 수 없으므로, */
	if (!pdn)
		/* [한국어] 일반 경로로 간다. */
		goto fallback;

	/* [한국어] 그 장치가 속한 EEH PE 를 얻는다. */
	pe = eeh_dev_to_pe(pdn->edev);
	/* [한국어] PE 가 EEH 복구 중이라면, 상류 주석이 설명하는 문제 상황이다 —
	 * 일반 경로인 rtas_get_sensor() 는 확장 지연(9902) 동안 내부에서 재시도를
	 * 반복해 약 6초를 소비하고, 그 사이 EEH 핸들러가 멈춰 복구가 실패할 수 있다. */
	if (pe && (pe->state & EEH_PE_RECOVERING)) {
		/* [한국어] 그래서 재시도 래퍼를 건너뛰고 rtas_call() 을 직접 부른다.
		 * 인자 2개(DR_ENTITY_SENSE, slot->index)를 넣고 결과 2개를 받는 호출이며,
		 * state 가 그 출력 자리다. */
		rc = rtas_call(token, 2, 2, state, DR_ENTITY_SENSE,
			       slot->index);
		/* [한국어] RTAS 코드를 errno 로 번역해 돌려준다 — 확장 지연이면 -EBUSY 가 되어
		 * EEH 핸들러가 즉시 오류를 인지하고 복구를 진행할 수 있다. */
		return rtas_get_sensor_errno(rc);
	}
/* [한국어] EEH 복구 중이 아닐 때 오는 일반 경로. */
fallback:
	/* [한국어] 재시도를 포함한 표준 래퍼를 쓴다. 평상시에는 이쪽이 더 안전하다 —
	 * 일시적인 바쁨을 호출자가 신경 쓰지 않아도 되기 때문이다. */
	return rtas_get_sensor(DR_ENTITY_SENSE, slot->index, state);
}

/* [한국어]
 * rpaphp_get_sensor_state - 센서를 읽고, 필요하면 전원을 넣어 다시 읽는다
 *
 * @slot: 대상 슬롯.
 * @state: 센서 값을 받을 출력 인자.
 * @return: 0 = 성공, 음수 = 실패.
 *
 * 왜 재시도가 필요한가: 옆의 상류 주석대로 일부 슬롯은 전원이 들어와 있어야
 * 센서를 읽을 수 있다. 그런데 슬롯 상태를 알아보려는 시점은 대개 전원이
 * 꺼져 있을 때다. 그래서 -EFAULT(= "전원부터 넣어라")를 받으면 이 함수가
 * 스스로 전원을 켜고 한 번 더 읽는다.
 *
 * 주목할 점: 이것은 조회 함수인데 하드웨어 상태를 바꾸는 부수 효과를 갖는다.
 * 전원을 켠 뒤 다시 끄지 않으므로, 이 함수를 부른 것만으로 슬롯 전원이
 * 들어온 채 남을 수 있다.
 *
 * 동작 과정:
 *   1) 그냥 읽어 본다.
 *   2) -EFAULT 또는 -EEXIST 면 전원을 켜고 다시 읽는다. 전원 인가 자체가
 *      실패하면 그 오류가 최종 반환값이 된다.
 *   3) -ENODEV 면 "쓸 수 없는 슬롯"이라는 정보 로그만 남긴다 — 오류가 아니라
 *      정상적인 상태 중 하나로 취급한다.
 *   4) 그 밖의 실패는 오류 로그를 남긴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. RTAS 호출과 전원 제어가 들어가므로
 * 잠들 수 있다. EEH 복구 경로에서도 불리지만, 그 경우 하위 함수가
 * 빠르게 -EBUSY 를 돌려주므로 여기서도 오래 머물지 않는다.
 *
 * 에러 경로: 자체 오류를 만들지 않고 하위 결과를 그대로 전달한다.
 *
 * 호출 체인:
 *   rpaphp_enable_slot() / rpaphp_core.c 의 sysfs 콜백
 *     → [rpaphp_get_sensor_state] → __rpaphp_get_sensor_state()
 *     → (필요 시) rtas_set_power_level() → __rpaphp_get_sensor_state() 재호출
 */
int rpaphp_get_sensor_state(struct slot *slot, int *state)
{
	/* [한국어] 센서 읽기 결과. */
	int rc;
	/* [한국어] rtas_set_power_level() 이 실제로 설정한 전력 수준을 받을 출력 인자.
	 * 이 함수는 그 값을 쓰지 않지만 API 가 요구한다. */
	int setlevel;

	/* [한국어] 먼저 그냥 읽어 본다. */
	rc = __rpaphp_get_sensor_state(slot, state);

	/* [한국어] 실패한 경우에만 아래 복구를 시도한다. */
	if (rc < 0) {
		/* [한국어] -EFAULT 는 위 번역표대로 "전원부터 넣어라"는 뜻이고,
		 * -EEXIST 는 rtas_error_rc() 가 돌려줄 수 있는 또 다른 값이다. */
		if (rc == -EFAULT || rc == -EEXIST) {
			/* [한국어] 왜 다시 시도하는지 디버그 로그로 남긴다. */
			dbg("%s: slot must be power up to get sensor-state\n",
			    __func__);

			/* some slots have to be powered up
			 * before get-sensor will succeed.
			 */
			/* [한국어] 위 상류 주석대로, 일부 슬롯은 전원이 들어와야 센서를 읽을 수 있다.
			 * 그래서 요청받지 않았어도 전원을 넣는다 — 상태를 알아내기 위해 상태를
			 * 바꾸는 셈이며, 이 함수의 가장 눈에 띄는 부수 효과다. */
			rc = rtas_set_power_level(slot->power_domain, POWER_ON,
						  &setlevel);
			/* [한국어] 전원 인가 실패. */
			if (rc < 0) {
				/* [한국어] 실패를 로그로 남기고, rc 에 담긴 오류가 그대로 반환된다. */
				dbg("%s: power on slot[%s] failed rc=%d.\n",
				    __func__, slot->name, rc);
			} else {
				/* [한국어] 전원이 들어왔으니 다시 읽는다. 이 두 번째 결과가 최종 반환값이 된다. */
				rc = __rpaphp_get_sensor_state(slot, state);
			}
		/* [한국어] 쓸 수 없는 슬롯이면, */
		} else if (rc == -ENODEV)
			/* [한국어] 정보 로그만 남긴다 — 오류가 아니라 정상적인 상태 중 하나로 취급한다. */
			info("%s: slot is unusable\n", __func__);
		else
			/* [한국어] 그 밖의 실패는 오류 로그. */
			err("%s failed to get sensor state\n", __func__);
	}
	/* [한국어] 성공이면 0, 실패면 마지막 시도의 오류가 그대로 나간다. */
	return rc;
}

/**
 * rpaphp_enable_slot - record slot state, config pci device
 * @slot: target &slot
 *
 * Initialize values in the slot structure to indicate if there is a pci card
 * plugged into the slot. If the slot is not empty, run the pcibios routine
 * to get pcibios stuff correctly set up.
 */
/* [한국어]
 * rpaphp_enable_slot - 슬롯 상태를 확정하고 카드가 있으면 PCI 장치를 구성한다
 *
 * @slot: 대상 슬롯. 이 함수가 slot->state / bus / pci_devs 를 채운다.
 * @return: 0 = 성공(빈 슬롯도 성공이다). 음수 = 전원/센서 조회 실패,
 *       PCI 버스를 못 찾음(-EINVAL), 또는 DT 에 어댑터 자식 노드가 없음(-EINVAL).
 *
 * 위 커널독이 요약하듯 두 가지를 한다 — 슬롯 상태를 기록하고, 카드가 있으면
 * PCI 장치를 열거해 구성한다.
 *
 * 동작 과정:
 *   1) state 를 EMPTY 로 초기화한다. 중간에 실패해 돌아가더라도 상태가
 *      과대평가되지 않게 하는 안전한 출발점이다.
 *   2) 전원 수준을 조회한다. [상류 코드 관찰] 읽은 level 값을 이후 어디서도
 *      쓰지 않는다 — 호출이 성공하는지만 확인하는 셈이다.
 *   3) 센서로 카드 유무를 확인한다. 이 호출은 필요하면 슬롯 전원을 켜는
 *      부수 효과를 가질 수 있다.
 *   4) DT 노드로 PCI 버스를 찾아 slot->bus 와 slot->pci_devs 를 채운다.
 *      alloc_slot_struct() 가 남겨 둔 필드가 여기서 채워진다.
 *   5) 카드가 있으면(PRESENT) state 를 NOT_CONFIGURED 로 올리고,
 *      DT 에 어댑터 자식 노드가 있는지 확인한 뒤,
 *      버스가 비어 있으면 EEH 장치 정보를 먼저 만들고 pci_hp_add_devices() 로
 *      열거한다. EEH 준비가 앞서야 하는 이유는 장치 등록 시점부터 오류를
 *      감시할 수 있어야 하기 때문이다.
 *   6) 열거 결과 장치가 생겼으면 state 를 CONFIGURED 로 올린다.
 *   7) 디버그 모드면 붙은 장치 목록을 찍는다.
 *
 * 실행 컨텍스트: 슬롯 활성화 경로, 프로세스 컨텍스트. RTAS 호출과 PCI 열거,
 * 드라이버 probe 까지 유발하므로 오래 걸리고 잠들 수 있다.
 *
 * 에러 경로: 네 지점 모두 곧장 return 한다. 되돌리는 코드가 없어,
 * 4)에서 bus/pci_devs 를 채운 뒤 5)에서 실패하면 슬롯 구조체가 부분적으로만
 * 초기화된 채 남는다.
 *
 * 호출 체인:
 *   rpaphp_core.c 의 enable_slot 콜백 / 슬롯 추가 경로
 *     → [rpaphp_enable_slot]
 *     → rtas_get_power_level() → rpaphp_get_sensor_state()
 *     → pci_find_bus_by_node() → pseries_eeh_init_edev_recursive()
 *     → pci_hp_add_devices()
 */
int rpaphp_enable_slot(struct slot *slot)
{
	/* [한국어] rc: 각 단계 결과. level: 전원 수준. state: 카드 유무 센서 값. */
	int rc, level, state;
	/* [한국어] 이 슬롯에 대응하는 PCI 버스. */
	struct pci_bus *bus;

	/* [한국어] 먼저 비어 있다고 가정한다. 아래에서 실제 상태에 따라 올려 잡는다 —
	 * 중간에 실패해 돌아가더라도 상태가 과대평가되지 않게 하는 안전한 초기값이다. */
	slot->state = EMPTY;

	/* Find out if the power is turned on for the slot */
	/* [한국어] 슬롯 전원이 들어와 있는지 펌웨어에 묻는다.
	 * [상류 코드 관찰] level 을 읽어 두고도 아래에서 쓰지 않는다 —
	 * 호출 자체가 성공하는지만 확인하는 셈이다. */
	rc = rtas_get_power_level(slot->power_domain, &level);
	/* [한국어] 실패하면 더 진행할 수 없다. */
	if (rc)
		return rc;

	/* Figure out if there is an adapter in the slot */
	/* [한국어] 카드가 꽂혀 있는지 확인한다. 위에서 본 대로 이 호출은 필요하면
	 * 슬롯 전원을 켜는 부수 효과를 가질 수 있다. */
	rc = rpaphp_get_sensor_state(slot, &state);
	/* [한국어] 실패 전파. */
	if (rc)
		return rc;

	/* [한국어] DT 노드로 PCI 버스를 찾는다. PowerPC 전용 헬퍼이며, 버스가 없으면
	 * 슬롯 아래에 장치를 붙일 자리가 없다는 뜻이다. */
	bus = pci_find_bus_by_node(slot->dn);
	/* [한국어] 버스를 못 찾은 경우. */
	if (!bus) {
		/* [한국어] 어느 노드에서 실패했는지 %pOF 로 전체 경로를 찍는다. */
		err("%s: no pci_bus for dn %pOF\n", __func__, slot->dn);
		/* [한국어] DT 와 실제 버스 구성이 어긋난 것이므로 -EINVAL. */
		return -EINVAL;
	}

	/* [한국어] 찾은 버스를 슬롯에 기록한다. alloc_slot_struct() 가 채우지 않고 남겨 둔
	 * 필드가 여기서 채워진다. */
	slot->bus = bus;
	/* [한국어] 장치 목록도 그 버스의 것을 가리키게 한다. 슬롯이 목록을 소유하지 않고
	 * 버스의 것을 빌려 보는 구조다. */
	slot->pci_devs = &bus->devices;

	/* if there's an adapter in the slot, go add the pci devices */
	/* [한국어] 카드가 실제로 꽂혀 있을 때만 아래를 진행한다. */
	if (state == PRESENT) {
		/* [한국어] 일단 "카드는 있으나 아직 구성 안 됨"으로 올린다. */
		slot->state = NOT_CONFIGURED;

		/* non-empty slot has to have child */
		/* [한국어] 카드가 있다면 DT 에 그 어댑터를 나타내는 자식 노드가 반드시 있어야 한다
		 * (옆의 상류 주석). 없다면 펌웨어가 준 정보가 일관되지 않은 것이다. */
		if (!slot->dn->child) {
			/* [한국어] 어느 슬롯에서 어긋났는지 로그로 남긴다. */
			err("%s: slot[%s]'s device_node doesn't have child for adapter\n",
			    __func__, slot->name);
			/* [한국어] -EINVAL 로 중단한다. 이 시점에는 bus 와 pci_devs 를 이미 채워 두었으므로,
			 * 슬롯 구조체가 부분적으로만 초기화된 채 남는다. */
			return -EINVAL;
		}

		/* [한국어] 버스에 장치가 하나도 없으면 아직 열거되지 않은 것이다. */
		if (list_empty(&bus->devices)) {
			/* [한국어] EEH 장치 정보를 재귀적으로 만들어 둔다. PCI 장치를 만들기 전에 해야 하는
			 * 이유는 EEH 가 장치 등록 시점에 이미 준비되어 있어야 오류를 놓치지 않기 때문이다. */
			pseries_eeh_init_edev_recursive(PCI_DN(slot->dn));
			/* [한국어] 핫플러그 전용 열거 경로로 버스를 스캔하고 장치를 등록한다.
			 * 부팅 시 열거와 달리 이미 사용 중인 버스 번호를 피해 배정한다. */
			pci_hp_add_devices(bus);
		}

		/* [한국어] 열거 결과 장치가 하나라도 생겼으면, */
		if (!list_empty(&bus->devices)) {
			/* [한국어] 구성 완료 상태로 올린다. 조건문을 따로 둔 이유는 위 열거가 실패해
			 * 아무것도 안 생겼을 수 있기 때문이다. */
			slot->state = CONFIGURED;
		}

		/* [한국어] 디버그 모드일 때만 아래 목록을 찍는다. */
		if (rpaphp_debug) {
			/* [한국어] 순회용 포인터. */
			struct pci_dev *dev;
			/* [한국어] 어느 슬롯의 목록인지 헤더를 찍는다. */
			dbg("%s: pci_devs of slot[%pOF]\n", __func__, slot->dn);
			/* [한국어] 버스에 붙은 장치를 모두 순회하며, */
			list_for_each_entry(dev, &bus->devices, bus_list)
				/* [한국어] 각각의 이름을 들여쓰기해 찍는다. */
				dbg("\t%s\n", pci_name(dev));
		}
	}

	/* [한국어] 여기까지 왔으면 성공이다. 슬롯이 비어 있어도(state == EMPTY) 오류가 아니다 —
	 * 빈 슬롯을 확인한 것도 정상적인 결과이기 때문이다. */
	return 0;
}
