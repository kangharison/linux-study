// SPDX-License-Identifier: GPL-2.0+
/*
 * CompactPCI Hot Plug Driver PCI functions
 *
 * Copyright (C) 2002,2005 by SOMA Networks, Inc.
 *
 * All rights reserved.
 *
 * Send feedback to <scottm@somanetworks.com>
 */

/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
/*
 * [한국어 설명] CompactPCI 핫스왑 레지스터 조작과 슬롯 열거 (cpci_hotplug_pci.c)
 *
 * === 파일의 역할 ===
 * CompactPCI 핫플러그 코어(cpci_hotplug_core.c)가 쓰는 PCI 쪽 일감을 모아
 * 둔 파일이다. 열 개의 함수가 있고 성격이 뚜렷하게 둘로 갈린다.
 * 앞의 여덟(get_attention_status, set_attention_status, get_hs_csr,
 * check_and_clear_ins, check_ext, clear_ext, led_on, led_off)은 모두
 * **같은 레지스터 하나**만 만진다 — CompactPCI Hot Swap capability
 * (PCI_CAP_ID_CHSWP)의 HS_CSR 이다. 여덟 함수가 예외 없이 같은 세 줄로
 * 시작하는데, capability 를 찾고 HS_CSR 을 읽는 것이다. 어디에도 캐시하지
 * 않는 이유는 카드가 뽑히면 capability 자체가 사라지기 때문이다.
 * 뒤의 둘(configure_slot, unconfigure_slot)은 완전히 다른 일을 한다.
 * PCI 코어와 함께 장치를 열거하고 자원을 배정하거나 그 반대를 한다.
 * 앞의 여덟이 "카드가 무슨 말을 하는가" 라면 뒤의 둘은 "그 카드를 커널에
 * 어떻게 들이고 내보내는가" 다.
 * 반환 규약이 함수마다 다른 점에 주의해야 한다. get_attention_status 는
 * 0/1, set_attention_status 는 **1이 성공**, get_hs_csr 은 실패가 0xFFFF,
 * clear_ext 와 led_on/off 는 **0이 성공**(errno 규약)이다. 같은 파일 안에서
 * 성공을 뜻하는 값이 정반대인 함수가 나란히 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * CompactPCI 핫플러그는 세 층이다. 위에 cpci_hotplug_core.c 가 있어 슬롯
 * 자료구조와 sysfs, 그리고 #ENUM 신호를 감시하는 폴링 스레드를 담당하고,
 * 아래에 cpcihp_generic.c 나 cpcihp_zt5550.c 같은 보드별 드라이버가 있어
 * "지금 #ENUM 이 서 있는가" 한 가지에 답한다. 이 파일은 그 사이에서
 * 카드 자체와 대화하는 몫을 맡는다.
 * 흐름:
 *   폴링 스레드가 #ENUM 을 봄 → core 가 어느 슬롯인지 찾음
 *     → [이 파일] cpci_check_and_clear_ins() 로 삽입인지 확인
 *     → [이 파일] cpci_configure_slot() 으로 열거·자원 배정
 *     → [이 파일] cpci_led_off() 로 파란 LED 를 꺼 "쓸 수 있음" 을 표시
 *   추출 요청 → [이 파일] cpci_check_ext() 로 확인
 *     → [이 파일] cpci_unconfigure_slot() 으로 제거
 *     → [이 파일] cpci_led_on() 으로 파란 LED 를 켜 "뽑아도 됨" 을 표시
 *     → [이 파일] cpci_clear_ext() 로 요청 비트를 지움
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. 앞의 여덟은 config 접근만 하고,
 * 뒤의 둘은 pci_lock_rescan_remove() 를 잡고 열거·해제를 하므로 오래 걸린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: cpci_hotplug_core.c 가 이 파일의 열 함수를 모두 부른다.
 * 옆쪽: cpci_hotplug.h 의 struct slot(bus, devfn, number, dev 필드를 쓴다),
 * slot_name(), 전역 cpci_debug, 그리고 HS_CSR_INS / HS_CSR_EXT / HS_CSR_LOO
 * 비트 정의.
 * 아래쪽: PCI 코어의 두 갈래를 쓴다. config 접근 쪽은
 * pci_bus_find_capability() 와 pci_bus_read/write_config_word() 이고,
 * 열거 쪽은 pci_get_slot(), pci_scan_slot(), pci_hp_add_bridge(),
 * pci_assign_unassigned_bridge_resources(), pci_bus_add_devices(),
 * pci_stop_and_remove_bus_device() 다.
 * 규격 근거: CompactPCI Hot Swap 규격의 HS_CSR 레지스터. INS(삽입),
 * EXT(추출 요청), LOO(파란 LED) 세 비트가 이 파일이 다루는 전부이며,
 * INS 와 EXT 는 1 을 써서 지우는 W1C 다.
 * 공유 상태: 없다. 전역 변수를 두지 않고 인자로 받은 struct slot 만 다룬다.
 *
 * === 주요 함수/구조체 요약 ===
 * - 여덟 개의 HS_CSR 함수: 모두 capability 를 찾고 → HS_CSR 을 읽고 →
 *   비트를 보거나 고치는 같은 형태다. 캐시하지 않는 이유는 카드가 뽑히면
 *   capability 가 사라지기 때문이다.
 * - cpci_check_and_clear_ins(): 확인과 지우기를 함께 한다. 읽은 값을 그대로
 *   되써서 지우는데(W1C), 쓰기가 실패하면 삽입을 **보고하지 않는다** —
 *   지우지 못한 채 보고하면 같은 삽입이 반복되기 때문이다.
 * - cpci_check_ext() / cpci_clear_ext(): 이쪽은 확인과 지우기가 나뉘어 있다.
 *   추출 요청을 확인한 뒤 카드를 실제로 내리는 데 시간이 걸리고, 그 사이에도
 *   상태가 유지되어야 하기 때문이다.
 * - cpci_led_on() / cpci_led_off(): 값이 이미 원하는 대로면 config 쓰기를
 *   생략한다. cpci_set_attention_status() 가 조건 없이 쓰는 것과 다르다.
 * - cpci_configure_slot(): 장치를 세 단계로 찾는다(캐시 → pci_get_slot →
 *   pci_scan_slot). 마지막 단계가 핵심인데, 방금 꽂힌 카드는 커널이 아직
 *   모르기 때문이다. 그 뒤 브리지 등록, 자원 배정, 드라이버 바인딩을 한다.
 * - cpci_unconfigure_slot(): 슬롯 번호가 같은 장치를 **모두** 제거한다.
 *   다중 기능 카드를 위한 것이며, 참조 관리가 두 겹(루프 안의 get/put 과
 *   configure 가 올린 참조의 put)이다.
 * - 로그 매크로 넷: dev_ 계열을 쓸 수 없어 직접 정의한다. 이 파일이
 *   struct slot 만 받고 대응하는 struct device 를 들고 있지 않기 때문이다.
 *   info 와 warn 은 정의만 되고 쓰이지 않는다.
 *
 * === 상류 코드 관찰 ===
 * 코드는 고치지 않고 사실만 기록한다.
 * - 반환 규약이 파일 안에서 일관되지 않다. cpci_set_attention_status() 는
 *   1 이 성공이고 cpci_led_on/off() 는 0 이 성공이다. 둘 다 같은 LOO 비트를
 *   다루는데도 그렇다.
 * - 오류와 정상값이 구분되지 않는 함수가 여럿이다.
 *   cpci_get_attention_status() 는 capability 가 없어도, 읽기가 실패해도,
 *   LED 가 꺼져 있어도 모두 0 을 반환한다.
 * - cpci_get_attention_status() 의 마지막 줄만 HS_CSR_LOO 대신 0x0008 을
 *   직접 쓴다. 같은 값이지만 표기가 다르다.
 * - info 와 warn 매크로가 정의만 되고 쓰이지 않는다.
 * - linux/module.h 와 linux/proc_fs.h 가 include 되어 있으나 쓰이지 않는다.
 *
 * === NVMe 관점 ===
 * 접점이 없다. CompactPCI 는 통신 장비용 랙 규격이고 NVMe 가 등장하기
 * 훨씬 전의 것이다.
 * 다만 cpci_configure_slot() 의 세 단계 — 이미 있는지 보고, 열거된 것을
 * 찾고, 없으면 스캔한다 — 는 지금의 핫플러그도 그대로 쓰는 형태다.
 * NVMe SSD 를 핫플러그로 꽂았을 때 pciehp 가 하는 일도 같은 순서이며,
 * pci_assign_unassigned_bridge_resources() 와 pci_bus_add_devices() 를
 * 부르는 것까지 동일하다. 달라진 것은 카드의 존재를 알아내는 방법
 * (#ENUM 신호 대 링크 상태 변화)뿐이다.
 */

#include <linux/module.h>
/* [한국어] 기본 커널 유틸. */
#include <linux/kernel.h>
/* [한국어] pci_bus_find_capability(), pci_bus_read/write_config_word(),
 * pci_get_slot(), pci_scan_slot(), for_each_pci_bridge(). */
#include <linux/pci.h>
/* [한국어] pci_hp_add_bridge(). */
#include <linux/pci_hotplug.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/proc_fs.h>
/* [한국어] pci_lock_rescan_remove(), pci_stop_and_remove_bus_device(),
 * pci_assign_unassigned_bridge_resources(). */
#include "../pci.h"
/* [한국어] struct slot, slot_name(), 전역 cpci_debug, HS_CSR_ 계열 비트 정의. */
#include "cpci_hotplug.h"

/* [한국어] 로그 접두사. cpcihp_generic.c 가 모듈 여부에 따라 THIS_MODULE->name 을
 * 쓰는 것과 달리 여기서는 상수 하나로 고정한다. */
#define MY_NAME	"cpci_hotplug"

/* [한국어] cpci_debug 가 켜졌을 때만 찍는 디버그 매크로. do-while(0) 로 감싸
 * if 문 뒤에 붙어도 문법이 깨지지 않게 한다. */
#define dbg(format, arg...)					\
	do {							\
		if (cpci_debug)					\
			printk(KERN_DEBUG "%s: " format "\n",	\
				MY_NAME, ## arg);		\
	} while (0)
/* [한국어] 오류 로그. dev_err 를 쓰지 않는 것은 이 파일이 struct slot 만 받고
 * 대응하는 struct device 를 들고 있지 않기 때문이다. */
#define err(format, arg...) printk(KERN_ERR "%s: " format "\n", MY_NAME, ## arg)
/* [한국어] 정보 로그. 이 파일에서는 쓰이지 않는다. */
#define info(format, arg...) printk(KERN_INFO "%s: " format "\n", MY_NAME, ## arg)
/* [한국어] 경고 로그. 역시 쓰이지 않는다. */
#define warn(format, arg...) printk(KERN_WARNING "%s: " format "\n", MY_NAME, ## arg)


/* [한국어]
 * cpci_get_attention_status - 파란 LED 가 켜져 있는지 답한다
 *
 * @slot: 대상 슬롯.
 * @return: 1 = 켜짐, 0 = 꺼짐 또는 조회 실패.
 *
 * CompactPCI 의 파란 LED(LOO, LED On/Off)는 "이 카드를 뽑아도 된다" 는 표시다.
 * 그 상태를 HS_CSR 에서 읽어 답한다.
 *
 * 이 파일의 여덟 HS_CSR 함수가 모두 같은 형태다 — capability 를 찾고,
 * HS_CSR 을 읽고, 비트를 보거나 고친다. capability 위치를 캐시하지 않는
 * 이유는 카드가 뽑히면 그것 자체가 사라지기 때문이다.
 *
 * [상류 코드 관찰] 세 가지 다른 상황이 모두 0 이 된다 — capability 가 없을 때,
 * 읽기가 실패했을 때, 그리고 LED 가 실제로 꺼져 있을 때. 호출자가 셋을
 * 구분할 방법이 없다. 또 마지막 줄만 HS_CSR_LOO 대신 0x0008 을 직접 쓴다.
 *
 * 실행 컨텍스트: sysfs 읽기 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 위 관찰 참조.
 *
 * 호출 체인:
 *   cpci_hotplug_core.c 의 get_attention_status 콜백 → [이 함수]
 *     → pci_bus_find_capability() → pci_bus_read_config_word()
 */
u8 cpci_get_attention_status(struct slot *slot)
{
	/* [한국어] capability 오프셋. */
	int hs_cap;
	/* [한국어] 읽어 올 상태 제어 레지스터. */
	u16 hs_csr;

	/* [한국어] Hot Swap capability(PCI_CAP_ID_CHSWP)의 위치를 찾는다. 이 파일의 여덟
	 * 함수가 모두 같은 세 줄로 시작하는데, 매번 찾는 이유는 어디에도 캐시해
	 * 두지 않기 때문이다 — 카드가 뽑히면 capability 자체가 사라지므로
	 * 캐시하면 위험하다. */
	hs_cap = pci_bus_find_capability(slot->bus,
					 slot->devfn,
					 PCI_CAP_ID_CHSWP);
	/* [한국어] 없으면 이 슬롯은 핫스왑을 지원하지 않는다. */
	if (!hs_cap)
		/* [한국어] 0(꺼짐)으로 답한다. 오류와 "꺼져 있음" 이 같은 값이라 호출자가
		 * 구분할 수 없다. */
		return 0;

	/* [한국어] HS_CSR 레지스터를 읽는다. capability 헤더에서 2바이트 뒤가 그 자리다.
	 *   이 파일 전체가 그 하나의 16비트 레지스터만 다룬다. */
	if (pci_bus_read_config_word(slot->bus,
				     slot->devfn,
				     hs_cap + 2,
				     &hs_csr))
		/* [한국어] 읽기 실패도 0 으로 답한다. */
		return 0;

	/* [한국어] LOO(LED On/Off) 비트가 서 있으면 1, 아니면 0. 0x0008 이 HS_CSR_LOO 와
	 * 같은 값인데, 이 한 줄만 상수 대신 숫자를 쓴다. */
	return hs_csr & 0x0008 ? 1 : 0;
}

/* [한국어]
 * cpci_set_attention_status - 파란 LED 를 켜거나 끈다
 *
 * @slot: 대상 슬롯.
 * @status: 0 이 아니면 켜기, 0 이면 끄기.
 * @return: 1 = 성공, 0 = 실패.
 *
 * 읽기-수정-쓰기로 LOO 비트만 바꾼다. HS_CSR 의 다른 비트(INS, EXT)가
 * W1C 라, 읽은 값을 그대로 되쓰면 그 비트들이 함께 지워질 수 있다는 점에
 * 주의할 만하다 — 이 함수는 그것을 고려하지 않는다.
 *
 * 이 파일의 여덟 HS_CSR 함수가 모두 같은 형태다 — capability 를 찾고,
 * HS_CSR 을 읽고, 비트를 보거나 고친다. capability 위치를 캐시하지 않는
 * 이유는 카드가 뽑히면 그것 자체가 사라지기 때문이다.
 *
 * [상류 코드 관찰] 반환 규약이 이 파일 안에서 뒤집힌다. 여기서는 1 이
 * 성공인데, 같은 LOO 비트를 다루는 cpci_led_on()/cpci_led_off() 는 0 이
 * 성공이다. 또 아래 두 함수와 달리 값이 이미 원하는 대로여도 조건 없이 쓴다.
 *
 * 실행 컨텍스트: sysfs 쓰기 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 세 실패 지점 모두 0 을 반환한다.
 *
 * 호출 체인:
 *   cpci_hotplug_core.c 의 set_attention_status 콜백 → [이 함수]
 *     → pci_bus_find_capability() → pci_bus_read/write_config_word()
 */
int cpci_set_attention_status(struct slot *slot, int status)
{
	/* [한국어] capability 오프셋. */
	int hs_cap;
	/* [한국어] 읽고 고칠 레지스터 값. */
	u16 hs_csr;

	/* [한국어] Hot Swap capability(PCI_CAP_ID_CHSWP)의 위치를 찾는다. 이 파일의 여덟
	 * 함수가 모두 같은 세 줄로 시작하는데, 매번 찾는 이유는 어디에도 캐시해
	 * 두지 않기 때문이다 — 카드가 뽑히면 capability 자체가 사라지므로
	 * 캐시하면 위험하다. */
	hs_cap = pci_bus_find_capability(slot->bus,
					 slot->devfn,
					 PCI_CAP_ID_CHSWP);
	/* [한국어] 없으면, */
	if (!hs_cap)
		/* [한국어] 0 을 반환한다. 이 함수는 1 이 성공이므로 0 이 실패다 — 아래
		 * cpci_led_on/off 가 0 을 성공으로 쓰는 것과 정반대라, 같은 파일 안에서
		 * 반환 규약이 둘로 갈린다. */
		return 0;
	/* [한국어] HS_CSR 레지스터를 읽는다. capability 헤더에서 2바이트 뒤가 그 자리다.
	 *   이 파일 전체가 그 하나의 16비트 레지스터만 다룬다. */
	if (pci_bus_read_config_word(slot->bus,
				     slot->devfn,
				     hs_cap + 2,
				     &hs_csr))
		/* [한국어] 읽기 실패면 0. */
		return 0;
	/* [한국어] 켜라는 요청이면, */
	if (status)
		/* [한국어] LOO 비트를 세운다. */
		hs_csr |= HS_CSR_LOO;
	else
		/* [한국어] 끄라는 요청이면 지운다. */
		hs_csr &= ~HS_CSR_LOO;
	/* [한국어] 되쓴다. */
	if (pci_bus_write_config_word(slot->bus,
				      slot->devfn,
				      hs_cap + 2,
				      hs_csr))
		/* [한국어] 쓰기 실패면 0. */
		return 0;
	/* [한국어] 성공은 1. */
	return 1;
}

/* [한국어]
 * cpci_get_hs_csr - HS_CSR 레지스터 값을 그대로 돌려준다
 *
 * @slot: 대상 슬롯.
 * @return: HS_CSR 값, 실패하면 0xFFFF.
 *
 * 비트를 해석하지 않고 원본을 넘기는 유일한 함수다. 호출자가 원하는 비트를
 * 직접 보게 하며, 디버그 출력에 쓰인다.
 *
 * 실패 표시가 이 파일에서 유일하게 0xFFFF 인 이유가 있다. 유효한 HS_CSR 이
 * 0 일 수 있으므로 0 을 실패로 쓸 수 없고, 반대로 모든 비트가 1 인 값은
 * 장치가 응답하지 않을 때 config 읽기가 돌려주는 값이라 실패를 뜻하기에 알맞다.
 *
 * 이 파일의 여덟 HS_CSR 함수가 모두 같은 형태다 — capability 를 찾고,
 * HS_CSR 을 읽고, 비트를 보거나 고친다. capability 위치를 캐시하지 않는
 * 이유는 카드가 뽑히면 그것 자체가 사라지기 때문이다.
 *
 * 실행 컨텍스트: 디버그 출력 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 두 실패 지점 모두 0xFFFF.
 *
 * 호출 체인:
 *   cpci_hotplug_core.c 의 디버그 출력 → [이 함수]
 *     → pci_bus_find_capability() → pci_bus_read_config_word()
 */
u16 cpci_get_hs_csr(struct slot *slot)
{
	/* [한국어] capability 오프셋. */
	int hs_cap;
	/* [한국어] 읽어 올 값. */
	u16 hs_csr;

	/* [한국어] Hot Swap capability(PCI_CAP_ID_CHSWP)의 위치를 찾는다. 이 파일의 여덟
	 * 함수가 모두 같은 세 줄로 시작하는데, 매번 찾는 이유는 어디에도 캐시해
	 * 두지 않기 때문이다 — 카드가 뽑히면 capability 자체가 사라지므로
	 * 캐시하면 위험하다. */
	hs_cap = pci_bus_find_capability(slot->bus,
					 slot->devfn,
					 PCI_CAP_ID_CHSWP);
	/* [한국어] 없으면, */
	if (!hs_cap)
		/* [한국어] 0xFFFF 를 반환한다. 이 함수만 실패 표시가 다른데, 유효한 레지스터 값이
		 * 모두 0 일 수도 있어 0 을 실패로 쓸 수 없기 때문이다. */
		return 0xFFFF;
	/* [한국어] HS_CSR 레지스터를 읽는다. capability 헤더에서 2바이트 뒤가 그 자리다.
	 *   이 파일 전체가 그 하나의 16비트 레지스터만 다룬다. */
	if (pci_bus_read_config_word(slot->bus,
				     slot->devfn,
				     hs_cap + 2,
				     &hs_csr))
		/* [한국어] 읽기 실패도 0xFFFF. */
		return 0xFFFF;
	/* [한국어] 레지스터 값을 그대로 돌려준다. 호출자가 원하는 비트를 직접 본다. */
	return hs_csr;
}

/* [한국어]
 * cpci_check_and_clear_ins - 삽입 표시를 확인하고 같은 동작으로 지운다
 *
 * @slot: 대상 슬롯.
 * @return: 1 = 삽입이 있었고 표시를 지웠다, 0 = 없었거나 지우지 못했다.
 *
 * INS(삽입) 비트는 W1C 라 1 을 쓰면 지워진다. 방금 읽은 값에 이미 그 비트가
 * 1 로 서 있으므로, **읽은 값을 그대로 되쓰면** 지워진다 — 함수 안의 영어
 * 주석이 그 방식을 밝힌다.
 *
 * 지우기가 실패하면 삽입을 보고하지 않는 것이 이 함수의 핵심 판단이다.
 * 지우지 못한 채 1 을 돌려주면 같은 삽입이 폴링마다 반복해서 보고되어,
 * 코어가 같은 카드를 계속 열거하려 든다.
 *
 * 아래 cpci_check_ext() 와 대비된다. 그쪽은 확인만 하고 지우기를 별도
 * 함수로 분리했는데, 추출은 확인 뒤 실제 제거까지 시간이 걸려 그 사이에도
 * 상태가 유지되어야 하기 때문이다. 삽입은 즉시 처리하면 되므로 합쳐 두었다.
 *
 * 이 파일의 여덟 HS_CSR 함수가 모두 같은 형태다 — capability 를 찾고,
 * HS_CSR 을 읽고, 비트를 보거나 고친다. capability 위치를 캐시하지 않는
 * 이유는 카드가 뽑히면 그것 자체가 사라지기 때문이다.
 *
 * 실행 컨텍스트: 폴링 스레드의 이벤트 확인. 프로세스 컨텍스트.
 *
 * 에러 경로: 모든 실패가 0 이다.
 *
 * 호출 체인:
 *   cpci_hotplug_core.c 의 폴링 → [이 함수]
 *     → pci_bus_find_capability() → pci_bus_read/write_config_word()
 */
int cpci_check_and_clear_ins(struct slot *slot)
{
	/* [한국어] capability 오프셋. */
	int hs_cap;
	/* [한국어] 읽고 되쓸 값. */
	u16 hs_csr;
	/* [한국어] 삽입이 있었는지. 기본값 0. */
	int ins = 0;

	/* [한국어] Hot Swap capability(PCI_CAP_ID_CHSWP)의 위치를 찾는다. 이 파일의 여덟
	 * 함수가 모두 같은 세 줄로 시작하는데, 매번 찾는 이유는 어디에도 캐시해
	 * 두지 않기 때문이다 — 카드가 뽑히면 capability 자체가 사라지므로
	 * 캐시하면 위험하다. */
	hs_cap = pci_bus_find_capability(slot->bus,
					 slot->devfn,
					 PCI_CAP_ID_CHSWP);
	/* [한국어] 없으면, */
	if (!hs_cap)
		return 0;
	/* [한국어] HS_CSR 레지스터를 읽는다. capability 헤더에서 2바이트 뒤가 그 자리다.
	 *   이 파일 전체가 그 하나의 16비트 레지스터만 다룬다. */
	if (pci_bus_read_config_word(slot->bus,
				     slot->devfn,
				     hs_cap + 2,
				     &hs_csr))
		return 0;
	/* [한국어] INS(삽입) 비트가 서 있으면, */
	if (hs_csr & HS_CSR_INS) {
		/* Clear INS (by setting it) */
		/* [한국어] 옆의 영어 주석대로 **읽은 값을 그대로 되써서** 지운다. INS 는 W1C 라
		 * 1 을 쓰면 지워지므로, 방금 읽은 값에 이미 1 이 서 있어 그대로 쓰면 된다. */
		if (pci_bus_write_config_word(slot->bus,
					      slot->devfn,
					      hs_cap + 2,
					      hs_csr))
			/* [한국어] 쓰기가 실패하면 지우지 못한 것이므로 삽입을 보고하지 않는다.
			 * 지우지 못한 채 1 을 돌려주면 같은 삽입이 반복해서 보고된다. */
			ins = 0;
		else
			/* [한국어] 성공하면 삽입이 있었다고 답한다. */
			ins = 1;
	}
	/* [한국어] 결과를 돌려준다. INS 가 서 있지 않았으면 0 이다. */
	return ins;
}

/* [한국어]
 * cpci_check_ext - 추출 요청이 있었는지 확인한다 (지우지는 않는다)
 *
 * @slot: 대상 슬롯.
 * @return: 1 = 추출 요청 있음, 0 = 없음 또는 조회 실패.
 *
 * EXT(추출) 비트는 사용자가 카드의 걸쇠를 열었다는 뜻이다. 이 함수는 그것을
 * 확인만 하고 지우지 않는다.
 *
 * cpci_check_and_clear_ins() 와 이름부터 다른 이유가 여기 있다. 추출은
 * 확인한 뒤 드라이버를 내리고 자원을 회수하는 데 시간이 걸리고, 그 과정이
 * 끝날 때까지 상태가 유지되어야 한다. 지우는 것은 그 뒤에
 * cpci_clear_ext() 가 따로 맡는다.
 *
 * 이 파일의 여덟 HS_CSR 함수가 모두 같은 형태다 — capability 를 찾고,
 * HS_CSR 을 읽고, 비트를 보거나 고친다. capability 위치를 캐시하지 않는
 * 이유는 카드가 뽑히면 그것 자체가 사라지기 때문이다.
 *
 * [상류 코드 관찰] capability 가 없을 때와 요청이 없을 때가 모두 0 이라
 * 구분되지 않는다.
 *
 * 실행 컨텍스트: 폴링 스레드의 이벤트 확인. 프로세스 컨텍스트.
 *
 * 에러 경로: 위 관찰 참조.
 *
 * 호출 체인:
 *   cpci_hotplug_core.c 의 폴링 → [이 함수]
 *     → pci_bus_find_capability() → pci_bus_read_config_word()
 */
int cpci_check_ext(struct slot *slot)
{
	/* [한국어] capability 오프셋. */
	int hs_cap;
	/* [한국어] 읽어 올 값. */
	u16 hs_csr;
	/* [한국어] 추출 요청이 있었는지. */
	int ext = 0;

	/* [한국어] Hot Swap capability(PCI_CAP_ID_CHSWP)의 위치를 찾는다. 이 파일의 여덟
	 * 함수가 모두 같은 세 줄로 시작하는데, 매번 찾는 이유는 어디에도 캐시해
	 * 두지 않기 때문이다 — 카드가 뽑히면 capability 자체가 사라지므로
	 * 캐시하면 위험하다. */
	hs_cap = pci_bus_find_capability(slot->bus,
					 slot->devfn,
					 PCI_CAP_ID_CHSWP);
	/* [한국어] 없으면, */
	if (!hs_cap)
		return 0;
	/* [한국어] HS_CSR 레지스터를 읽는다. capability 헤더에서 2바이트 뒤가 그 자리다.
	 *   이 파일 전체가 그 하나의 16비트 레지스터만 다룬다. */
	if (pci_bus_read_config_word(slot->bus,
				     slot->devfn,
				     hs_cap + 2,
				     &hs_csr))
		return 0;
	/* [한국어] EXT(추출) 비트가 서 있으면, */
	if (hs_csr & HS_CSR_EXT)
		/* [한국어] 1 로 표시한다. cpci_check_and_clear_ins() 와 달리 여기서는 지우지 않는다 —
		 * 지우는 것은 아래 cpci_clear_ext() 가 따로 맡는다. 확인과 지우기를 나눈
		 * 이유는, 추출 요청을 확인한 뒤 실제로 카드를 내리는 데 시간이 걸리고
		 * 그 사이에도 상태가 유지되어야 하기 때문이다. */
		ext = 1;
	/* [한국어] 결과를 돌려준다. */
	return ext;
}

/* [한국어]
 * cpci_clear_ext - 추출 요청 표시를 지운다
 *
 * @slot: 대상 슬롯.
 * @return: 0 = 성공, -ENODEV = capability 가 없거나 config 접근 실패.
 *
 * cpci_check_ext() 의 짝이며, 카드 제거가 끝난 뒤에 불린다.
 *
 * INS 와 같은 W1C 방식이라 읽은 값을 그대로 되쓴다. 다만 EXT 가 서 있을
 * 때만 쓰는 조건이 붙어 있어, 불필요한 config 쓰기를 하지 않는다.
 *
 * 반환 규약이 위 확인 함수들과 다르다. 그쪽은 0/1 이고 이쪽은 errno 다.
 * 같은 파일 안에서 규약이 나뉘는 자리 중 하나다.
 *
 * 이 파일의 여덟 HS_CSR 함수가 모두 같은 형태다 — capability 를 찾고,
 * HS_CSR 을 읽고, 비트를 보거나 고친다. capability 위치를 캐시하지 않는
 * 이유는 카드가 뽑히면 그것 자체가 사라지기 때문이다.
 *
 * 실행 컨텍스트: 제거 처리의 마무리. 프로세스 컨텍스트.
 *
 * 에러 경로: 세 실패 지점 모두 -ENODEV.
 *
 * 호출 체인:
 *   cpci_hotplug_core.c 의 제거 처리 → [이 함수]
 *     → pci_bus_find_capability() → pci_bus_read/write_config_word()
 */
int cpci_clear_ext(struct slot *slot)
{
	/* [한국어] capability 오프셋. */
	int hs_cap;
	/* [한국어] 읽고 되쓸 값. */
	u16 hs_csr;

	/* [한국어] Hot Swap capability(PCI_CAP_ID_CHSWP)의 위치를 찾는다. 이 파일의 여덟
	 * 함수가 모두 같은 세 줄로 시작하는데, 매번 찾는 이유는 어디에도 캐시해
	 * 두지 않기 때문이다 — 카드가 뽑히면 capability 자체가 사라지므로
	 * 캐시하면 위험하다. */
	hs_cap = pci_bus_find_capability(slot->bus,
					 slot->devfn,
					 PCI_CAP_ID_CHSWP);
	/* [한국어] 없으면, */
	if (!hs_cap)
		/* [한국어] 장치 없음으로 답한다. 위 확인 함수들이 0 을 쓰는 것과 달리 이쪽은
		 * errno 를 쓴다. */
		return -ENODEV;
	/* [한국어] HS_CSR 레지스터를 읽는다. capability 헤더에서 2바이트 뒤가 그 자리다.
	 *   이 파일 전체가 그 하나의 16비트 레지스터만 다룬다. */
	if (pci_bus_read_config_word(slot->bus,
				     slot->devfn,
				     hs_cap + 2,
				     &hs_csr))
		/* [한국어] 읽기 실패도 장치 없음. */
		return -ENODEV;
	/* [한국어] EXT 가 서 있을 때만, */
	if (hs_csr & HS_CSR_EXT) {
		/* Clear EXT (by setting it) */
		/* [한국어] 읽은 값을 그대로 되써서 지운다. INS 와 같은 W1C 방식이다. */
		if (pci_bus_write_config_word(slot->bus,
					      slot->devfn,
					      hs_cap + 2,
					      hs_csr))
			/* [한국어] 쓰기 실패면 장치 없음. */
			return -ENODEV;
	}
	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * cpci_led_on - 파란 LED 를 켜 "뽑아도 됨" 을 표시한다
 *
 * @slot: 대상 슬롯.
 * @return: 0 = 성공, -ENODEV = capability 가 없거나 config 접근 실패.
 *
 * 카드를 제거한 뒤 사용자에게 "이제 뽑아도 된다" 고 알리는 신호다.
 * CompactPCI 에서 파란 LED 는 그 뜻으로 정해져 있다.
 *
 * 이미 켜져 있으면 config 쓰기를 생략한다. cpci_set_attention_status() 가
 * 같은 LOO 비트를 조건 없이 쓰는 것과 다르며, 실패 시 로그를 남기는 것도
 * 그쪽에는 없는 처리다.
 *
 * 이 파일의 여덟 HS_CSR 함수가 모두 같은 형태다 — capability 를 찾고,
 * HS_CSR 을 읽고, 비트를 보거나 고친다. capability 위치를 캐시하지 않는
 * 이유는 카드가 뽑히면 그것 자체가 사라지기 때문이다.
 *
 * [상류 코드 관찰] 이 함수와 cpci_set_attention_status() 가 같은 비트를
 * 다루면서 반환 규약이 정반대다 — 여기서는 0 이 성공이고 그쪽은 1 이 성공이다.
 *
 * 실행 컨텍스트: 제거 처리. 프로세스 컨텍스트.
 *
 * 에러 경로: 세 실패 지점 모두 -ENODEV. 쓰기 실패만 로그를 남긴다.
 *
 * 호출 체인:
 *   cpci_hotplug_core.c 의 제거 처리 → [이 함수]
 *     → pci_bus_find_capability() → pci_bus_read/write_config_word()
 */
int cpci_led_on(struct slot *slot)
{
	/* [한국어] capability 오프셋. */
	int hs_cap;
	/* [한국어] 읽고 고칠 값. */
	u16 hs_csr;

	/* [한국어] Hot Swap capability(PCI_CAP_ID_CHSWP)의 위치를 찾는다. 이 파일의 여덟
	 * 함수가 모두 같은 세 줄로 시작하는데, 매번 찾는 이유는 어디에도 캐시해
	 * 두지 않기 때문이다 — 카드가 뽑히면 capability 자체가 사라지므로
	 * 캐시하면 위험하다. */
	hs_cap = pci_bus_find_capability(slot->bus,
					 slot->devfn,
					 PCI_CAP_ID_CHSWP);
	/* [한국어] 없으면, */
	if (!hs_cap)
		return -ENODEV;
	/* [한국어] HS_CSR 레지스터를 읽는다. capability 헤더에서 2바이트 뒤가 그 자리다.
	 *   이 파일 전체가 그 하나의 16비트 레지스터만 다룬다. */
	if (pci_bus_read_config_word(slot->bus,
				     slot->devfn,
				     hs_cap + 2,
				     &hs_csr))
		/* [한국어] 읽기 실패면, */
		return -ENODEV;
	/* [한국어] 이미 켜져 있지 않을 때만 쓴다. 값이 같으면 config 쓰기를 생략하는 것으로,
	 * cpci_set_attention_status() 가 조건 없이 쓰는 것과 다르다. */
	if ((hs_csr & HS_CSR_LOO) != HS_CSR_LOO) {
		/* [한국어] LOO 비트를 세우고, */
		hs_csr |= HS_CSR_LOO;
		/* [한국어] 되쓴다. */
		if (pci_bus_write_config_word(slot->bus,
					      slot->devfn,
					      hs_cap + 2,
					      hs_csr)) {
			/* [한국어] 실패하면 어느 슬롯이었는지 남기고, */
			err("Could not set LOO for slot %s", slot_name(slot));
			/* [한국어] 장치 없음으로 답한다. */
			return -ENODEV;
		}
	}
	/* [한국어] 성공은 0. 위 cpci_set_attention_status() 가 1 을 성공으로 쓰는 것과
	 * 정반대다. */
	return 0;
}

/* [한국어]
 * cpci_led_off - 파란 LED 를 꺼 "사용 중" 을 표시한다
 *
 * @slot: 대상 슬롯.
 * @return: 0 = 성공, -ENODEV = capability 가 없거나 config 접근 실패.
 *
 * cpci_led_on() 의 짝이며, 카드를 열거하고 드라이버를 붙인 뒤에 불린다.
 * LED 가 꺼지면 그 카드가 사용 중이라는 뜻이다.
 *
 * 켜기 쪽과 완전히 대칭이다. 이미 꺼져 있으면 쓰지 않고, 실패하면 로그를
 * 남기며, 0 이 성공이다.
 *
 * 이 파일의 여덟 HS_CSR 함수가 모두 같은 형태다 — capability 를 찾고,
 * HS_CSR 을 읽고, 비트를 보거나 고친다. capability 위치를 캐시하지 않는
 * 이유는 카드가 뽑히면 그것 자체가 사라지기 때문이다.
 *
 * 실행 컨텍스트: 삽입 처리의 마무리. 프로세스 컨텍스트.
 *
 * 에러 경로: 세 실패 지점 모두 -ENODEV.
 *
 * 호출 체인:
 *   cpci_hotplug_core.c 의 삽입 처리 → [이 함수]
 *     → pci_bus_find_capability() → pci_bus_read/write_config_word()
 */
int cpci_led_off(struct slot *slot)
{
	/* [한국어] capability 오프셋. */
	int hs_cap;
	/* [한국어] 읽고 고칠 값. */
	u16 hs_csr;

	/* [한국어] Hot Swap capability(PCI_CAP_ID_CHSWP)의 위치를 찾는다. 이 파일의 여덟
	 * 함수가 모두 같은 세 줄로 시작하는데, 매번 찾는 이유는 어디에도 캐시해
	 * 두지 않기 때문이다 — 카드가 뽑히면 capability 자체가 사라지므로
	 * 캐시하면 위험하다. */
	hs_cap = pci_bus_find_capability(slot->bus,
					 slot->devfn,
					 PCI_CAP_ID_CHSWP);
	/* [한국어] 없으면, */
	if (!hs_cap)
		return -ENODEV;
	/* [한국어] HS_CSR 레지스터를 읽는다. capability 헤더에서 2바이트 뒤가 그 자리다.
	 *   이 파일 전체가 그 하나의 16비트 레지스터만 다룬다. */
	if (pci_bus_read_config_word(slot->bus,
				     slot->devfn,
				     hs_cap + 2,
				     &hs_csr))
		/* [한국어] 읽기 실패면, */
		return -ENODEV;
	/* [한국어] 켜져 있을 때만 끈다. */
	if (hs_csr & HS_CSR_LOO) {
		/* [한국어] LOO 비트를 지우고, */
		hs_csr &= ~HS_CSR_LOO;
		/* [한국어] 되쓴다. */
		if (pci_bus_write_config_word(slot->bus,
					      slot->devfn,
					      hs_cap + 2,
					      hs_csr)) {
			/* [한국어] 실패하면 기록하고, */
			err("Could not clear LOO for slot %s", slot_name(slot));
			return -ENODEV;
		}
	}
	/* [한국어] 성공. */
	return 0;
}


/*
 * Device configuration functions
 */

/* [한국어]
 * cpci_configure_slot - 방금 꽂힌 카드를 찾아 열거하고 자원을 배정한다
 *
 * @slot: 대상 슬롯.
 * @return: 0 = 성공, -ENODEV = 카드를 찾지 못함.
 *
 * 이 파일에서 실제로 PCI 코어와 일하는 두 함수 중 하나다. 나머지 여덟은
 * Hot Swap capability 의 레지스터 하나만 만지는 반면, 이 함수와 짝인
 * cpci_unconfigure_slot() 은 장치 열거와 해제를 다룬다.
 *
 * 장치를 찾는 데 세 단계를 밟는다. 먼저 slot->dev 에 이미 있는지 보고,
 * 없으면 pci_get_slot() 으로 이미 열거된 것을 찾고, 그래도 없으면
 * pci_scan_slot() 으로 직접 스캔한다. 마지막 단계가 핵심인데, 카드가 방금
 * 꽂혔다면 커널이 그 존재를 아직 모르기 때문이다.
 *
 * 찾은 뒤에는 셋을 한다. 같은 슬롯 번호의 브리지를 핫플러그 브리지로
 * 등록하고(카드가 브리지를 품고 있을 수 있다), 미배정 자원을 배정하고,
 * 장치들을 드라이버 모델에 올린다. 마지막 단계에서 드라이버가 붙어 카드가
 * 실제로 동작하기 시작한다.
 *
 * pci_lock_rescan_remove() 가 전체를 감싸는 것은 PCI 코어의 요구다.
 * 열거와 해제가 같은 잠금 아래에서 직렬화되어야 한다.
 *
 * 실행 컨텍스트: 핫플러그 삽입 처리. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 카드를 찾지 못하면 -ENODEV. 그 경우에도 out 라벨을 지나
 * 잠금을 푼다.
 *
 * 호출 체인:
 *   cpci_hotplug_core.c 의 삽입 처리 → [이 함수]
 *     → pci_get_slot() → pci_scan_slot() → pci_hp_add_bridge()
 *     → pci_assign_unassigned_bridge_resources() → pci_bus_add_devices()
 */
int cpci_configure_slot(struct slot *slot)
{
	/* [한국어] 순회 커서. */
	struct pci_dev *dev;
	/* [한국어] 이 슬롯이 매달린 버스. */
	struct pci_bus *parent;
	/* [한국어] 결과. */
	int ret = 0;

	/* [한국어] 진입을 디버그 로그에 남긴다. 이 파일의 두 긴 함수만 진입·퇴장 로그를
	 * 남기는데, 열거와 해제가 오래 걸리고 실패 지점이 많기 때문이다. */
	dbg("%s - enter", __func__);

	/* [한국어] 열거와 해제 경로 전체를 직렬화한다. PCI 코어가 요구하는 잠금이다. */
	pci_lock_rescan_remove();

	/* [한국어] 아직 장치를 찾지 못했으면, */
	if (slot->dev == NULL) {
		/* [한국어] 어느 위치를 찾는지 남기고, */
		dbg("pci_dev null, finding %02x:%02x:%x",
		    slot->bus->number, PCI_SLOT(slot->devfn), PCI_FUNC(slot->devfn));
		/* [한국어] 이미 열거된 장치가 있는지 먼저 본다. 참조가 올라간 채로 온다. */
		slot->dev = pci_get_slot(slot->bus, slot->devfn);
	}

	/* Still NULL? Well then scan for it! */
	/* [한국어] 옆의 영어 주석대로 그래도 없으면 직접 스캔한다. */
	if (slot->dev == NULL) {
		int n;
		/* [한국어] 그 사실을 남긴다. */
		dbg("pci_dev still null");

		/*
		 * This will generate pci_dev structures for all functions, but
		 * we will only call this case when lookup fails.
		 */
		/* [한국어] 이 슬롯 위치를 스캔해 장치를 만든다. 카드가 방금 꽂혔다면 커널이
		 * 아직 모르므로 이 단계가 필요하다. */
		n = pci_scan_slot(slot->bus, slot->devfn);
		/* [한국어] 몇 개를 찾았는지 남긴다. */
		dbg("%s: pci_scan_slot returned %d", __func__, n);
		/* [한국어] 다시 조회한다. 스캔이 만들었다면 이번에는 찾힌다. */
		slot->dev = pci_get_slot(slot->bus, slot->devfn);
		/* [한국어] 그래도 없으면 카드가 없거나 응답하지 않는 것이다. */
		if (slot->dev == NULL) {
			/* [한국어] 어느 슬롯이었는지 남기고, */
			err("Could not find PCI device for slot %02x", slot->number);
			/* [한국어] 장치 없음으로, */
			ret = -ENODEV;
			goto out;
		}
	}
	/* [한국어] 장치가 매달린 버스. 아래 브리지 처리와 자원 배정의 기준이 된다. */
	parent = slot->dev->bus;

	/* [한국어] 그 버스의 브리지들을 훑으며, */
	for_each_pci_bridge(dev, parent) {
		/* [한국어] 이 슬롯과 같은 장치 번호를 가진 브리지면, */
		if (PCI_SLOT(dev->devfn) == PCI_SLOT(slot->devfn))
			/* [한국어] 핫플러그 브리지로 등록한다. 꽂힌 카드가 브리지를 품고 있으면 그 아래도
			 * 열거해야 하기 때문이다. */
			pci_hp_add_bridge(dev);
	}

	/* [한국어] 이 브리지 아래의 미배정 자원을 배정한다. 새로 꽂힌 카드의 BAR 이
	 * 여기서 실제 주소를 얻는다. */
	pci_assign_unassigned_bridge_resources(parent->self);

	/* [한국어] 장치들을 드라이버 모델에 올린다. 이 호출로 드라이버 바인딩이 일어나
	 * 카드가 실제로 동작하기 시작한다. */
	pci_bus_add_devices(parent);

 out:
	/* [한국어] 잠금을 푼다. 성공이든 실패든 이 자리를 지난다. */
	pci_unlock_rescan_remove();
	/* [한국어] 퇴장을 남긴다. */
	dbg("%s - exit", __func__);
	/* [한국어] 결과를 돌려준다. */
	return ret;
}

/* [한국어]
 * cpci_unconfigure_slot - 이 슬롯의 장치들을 모두 제거한다
 *
 * @slot: 대상 슬롯.
 * @return: 0 = 성공, -ENODEV = 애초에 장치가 없었음.
 *
 * cpci_configure_slot() 의 짝이다.
 *
 * 슬롯 번호가 같은 장치를 모두 제거하는 것이 요점이다. 다중 기능 카드면
 * 기능마다 pci_dev 가 하나씩 있으므로, 하나만 제거해서는 안 된다.
 * _safe 순회를 쓰는 이유도 그것이다 — 제거가 현재 항목을 목록에서 빼기 때문이다.
 *
 * 참조 관리가 두 겹이다. 루프 안의 get/put 은 제거 도중 포인터가 유효하도록
 * 잡는 것이고, 루프 뒤의 pci_dev_put(slot->dev) 는 configure 쪽의
 * pci_get_slot() 이 올린 참조를 놓는 것이다. 서로 다른 참조다.
 *
 * slot->dev 를 NULL 로 되돌리는 마지막 줄이 두 함수를 잇는다. 다음에 같은
 * 슬롯에 카드가 꽂히면 configure 가 그 NULL 을 보고 처음부터 다시 찾는다.
 *
 * 실행 컨텍스트: 핫플러그 제거 처리. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 장치가 없으면 -ENODEV. 개별 제거의 실패는 알릴 방법이 없어
 * 언제나 0 을 반환한다.
 *
 * 호출 체인:
 *   cpci_hotplug_core.c 의 제거 처리 → [이 함수]
 *     → pci_dev_get() → pci_stop_and_remove_bus_device() → pci_dev_put()
 */
int cpci_unconfigure_slot(struct slot *slot)
{
	/* [한국어] 안전 순회용 커서와 임시 저장. */
	struct pci_dev *dev, *temp;

	/* [한국어] 진입을 남긴다. */
	dbg("%s - enter", __func__);
	/* [한국어] 애초에 장치가 없으면 해제할 것도 없다. */
	if (!slot->dev) {
		/* [한국어] 그 사실을 남기고, */
		err("No device for slot %02x\n", slot->number);
		return -ENODEV;
	}

	/* [한국어] 열거·해제 잠금을 잡는다. */
	pci_lock_rescan_remove();

	/* [한국어] 이 버스의 장치들을 안전 순회한다. _safe 판이 필요한 이유는 아래에서
	 * 현재 항목을 목록에서 빼기 때문이다. */
	list_for_each_entry_safe(dev, temp, &slot->bus->devices, bus_list) {
		/* [한국어] 이 슬롯의 장치가 아니면, */
		if (PCI_SLOT(dev->devfn) != PCI_SLOT(slot->devfn))
			continue;
		/* [한국어] 참조를 올린다. 아래 제거가 마지막 참조를 놓을 수 있어, 그 뒤에도
		 * 포인터를 쓸 수 있게 잡아 두는 것이다. */
		pci_dev_get(dev);
		/* [한국어] 장치를 정지시키고 제거한다. 다중 기능 카드면 같은 슬롯의 모든 기능이
		 * 이 루프에서 차례로 제거된다. */
		pci_stop_and_remove_bus_device(dev);
		/* [한국어] 잡아 둔 참조를 놓는다. */
		pci_dev_put(dev);
	}
	/* [한국어] cpci_configure_slot() 이 pci_get_slot() 으로 올린 참조를 놓는다.
	 * 위 루프의 get/put 과는 다른 참조다. */
	pci_dev_put(slot->dev);
	/* [한국어] 포인터를 지운다. 다음에 같은 슬롯에 카드가 꽂히면 configure 가
	 * 이 NULL 을 보고 새로 찾는다. */
	slot->dev = NULL;

	/* [한국어] 잠금을 푼다. */
	pci_unlock_rescan_remove();

	/* [한국어] 퇴장을 남긴다. */
	dbg("%s - exit", __func__);
	/* [한국어] 언제나 성공. 개별 제거의 실패를 알릴 방법이 없다. */
	return 0;
}
