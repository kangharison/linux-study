// SPDX-License-Identifier: GPL-2.0
/*
 * ACPI PCI Hot Plug Extension for Ampere Altra. Allows control of
 * attention LEDs via requests to system firmware.
 *
 * Copyright (C) 2023 Ampere Computing LLC
 */

/* [한국어] 커널 로그 접두사 지정. 이 파일의 dev_err 은 디바이스 접두사를 따로 붙이지만,
 * pr_* 계열을 쓸 때를 대비한 관례적 선언이다. */
/*
 * [한국어 설명] Ampere Altra 용 acpiphp attention LED 확장 (acpiphp_ampere_altra.c)
 *
 * === 파일의 역할 ===
 * Ampere Altra 서버의 PCIe 슬롯 attention LED 를 제어하는 확장 모듈이다.
 * 파일 첫머리의 영어 주석이 밝히듯 "시스템 펌웨어에 요청을 보내" LED 를
 * 켜고 끄는 것이 전부이며, 슬롯 열거나 핫플러그 상태 관리는 전혀 하지 않는다 —
 * 그 일은 acpiphp 가 이미 하고 있고, 이 파일은 거기에 콜백 두 개를 얹는다.
 * 이 플랫폼에서 LED 가 OS 가 직접 만질 수 있는 레지스터가 아니라는 점이 핵심이다.
 * LED 는 시스템 펌웨어가 관리하는 자원이라, ARM 의 SMC(Secure Monitor Call)로
 * EL3 펌웨어에 요청을 보내야 한다. 그 요청은 세 단계로 이루어진다 —
 * UUID 로 서비스 핸들을 열고, 명령을 보내고, 핸들을 닫는다. 세 SMC 가 하나의
 * 트랜잭션이라 그 구간을 인터럽트 차단으로 보호한다.
 * 기능의 비대칭이 이 확장의 한계다. 설정(set_attn)은 되지만 조회(get_attn)는
 * 언제나 -EINVAL 을 돌려준다 — 펌웨어 인터페이스가 되읽기를 제공하지 않기 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 핫플러그 스택에서 이 파일은 가장 바깥의 선택적 부속이다. 아래에서부터
 * ACPI 펌웨어 → acpiphp(ACPI 기반 핫플러그 드라이버) → pci_hotplug_core(공용
 * sysfs 인터페이스) → 사용자 순인데, 이 파일은 acpiphp 옆에 붙어 attention
 * 콜백만 제공한다. acpiphp 는 자기 슬롯에 attention 요청이 오면 등록된 확장이
 * 있는지 보고, 있으면 그쪽으로 넘긴다.
 * 진입 경로는 둘이다. (1) ACPI HID "AMPC0008" 로 매칭되면 probe 가 UUID 를 읽고
 * 확장을 등록한다. (2) 이후 사용자가 sysfs 의 attention 파일에 쓰면
 * set_attention_status() 가 불려 SMC 를 보낸다.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. 다만 set_attention_status() 는
 * local_irq_save() 로 인터럽트를 끈 구간에서 SMC 세 번을 수행하므로,
 * 그동안 이 CPU 의 인터럽트 지연이 늘어난다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: acpiphp.h 의 struct acpiphp_attention_info 와
 * acpiphp_register_attention()/acpiphp_unregister_attention(). 이 두 함수가
 * acpiphp 와의 유일한 접점이며, THIS_MODULE 을 함께 넘겨 확장이 등록된 동안
 * 이 모듈이 언로드되지 않게 한다.
 * 아래쪽: ARM SMCCC(arm_smccc_smc)로 EL3 펌웨어를 부른다. 그리고 PCI 코어의
 * pcie_find_root_port() 와 pci_domain_nr(), PCI_SLOT() 으로 슬롯 식별자를 만든다.
 * 옆쪽: ACPI 속성 접근(dev_fwnode, fwnode_property_read_u32_array)으로
 * 펌웨어가 서술한 서비스 UUID 를 읽는다. DT 가 아니라 fwnode 계열을 쓰는 것이
 * 이 플랫폼이 ACPI 부팅임을 보여 준다.
 * 데이터 흐름: ACPI 의 uuid 속성 → 전역 led_service_id[4] → HANDLE_OPEN SMC →
 * 핸들 → REQUEST SMC(명령 + 슬롯 식별자) → 펌웨어 → LED.
 * 슬롯 식별자는 루트 포트의 장치 번호를 4비트 밀고 PCI 도메인 번호 하위
 * 4비트를 붙인 값으로, (장치, 도메인) 쌍이 이 플랫폼에서 슬롯을 지목하는 방식이다.
 * 공유 상태: 전역 배열 led_service_id[4] 하나뿐이다. probe 에서 한 번 채운 뒤
 * 읽기 전용이라 동기화가 필요 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * - set_attention_status(): 이 파일의 본체. 루트 포트를 찾고, 인터럽트를 끈 채
 *   SMC 세 개(열기-요청-닫기)를 하나의 트랜잭션으로 수행한다. 요청이 실패해도
 *   핸들 닫기는 반드시 거치는데, 열어 둔 핸들이 쌓이면 펌웨어 자원이
 *   고갈되기 때문이다.
 * - get_attention_status(): 언제나 -EINVAL. 펌웨어가 되읽기를 제공하지 않는다.
 *   콜백을 NULL 로 두지 않고 실패 함수를 두는 것은 acpiphp 코어가 두 콜백을
 *   모두 요구하기 때문으로 보인다.
 * - led_status(): 코어의 상태 코드(0/1/2)를 펌웨어 명령으로 번역한다.
 *   default 가 OFF 라 정의되지 않은 값이 와도 안전한 쪽으로 떨어진다.
 * - altra_led_probe() / altra_led_remove(): UUID 를 읽어 확장을 등록하고 해제한다.
 * - ampere_altra_attn: 두 콜백과 THIS_MODULE 을 담은 등록 구조체.
 * - SMC 함수 ID 와 LED 명령 상수들(HANDLE_OPEN/CLOSE, REQUEST, LED_ 계열):
 *   Ampere 의 펌웨어 인터페이스 규약에 속하며 이 트리에 그 문서가 없다.
 *   각 상수는 코드가 그것을 어떻게 쓰는지로만 설명해 두었다.
 */

#define pr_fmt(fmt) "acpiphp_ampere_altra: " fmt

/* [한국어] __init 계열 매크로. */
#include <linux/init.h>
/* [한국어] MODULE_* 매크로와 THIS_MODULE. */
#include <linux/module.h>
/* [한국어] PCI_SLOT/pci_domain_nr/pcie_find_root_port 등 PCI 코어 API. */
#include <linux/pci.h>
/* [한국어] struct hotplug_slot 정의 — 핫플러그 공용 코어와의 접점이다. */
#include <linux/pci_hotplug.h>
/* [한국어] platform_driver 와 dev_fwnode. */
#include <linux/platform_device.h>

/* [한국어] acpiphp 드라이버의 자체 헤더 — struct acpiphp_attention_info 와
 * acpiphp_register_attention()/unregister_attention() 이 여기서 온다.
 * 이 파일이 acpiphp 의 확장(extension)으로 동작하는 통로다. */
#include "acpiphp.h"

/* [한국어] SMC(Secure Monitor Call) 함수 ID — LED 서비스 핸들을 연다.
 * [값의 근거] 이 상수의 의미는 Ampere 의 펌웨어 인터페이스 규약에 속하며
 * 이 트리에는 그 문서가 없다. 코드가 그것을 어떻게 쓰는지로만 설명한다. */
#define HANDLE_OPEN	0xb0200000
/* [한국어] 같은 핸들을 닫는 SMC 함수 ID.
 * [값의 근거] 이 상수의 의미는 Ampere 의 펌웨어 인터페이스 규약에 속하며
 * 이 트리에는 그 문서가 없다. 코드가 그것을 어떻게 쓰는지로만 설명한다. */
#define HANDLE_CLOSE	0xb0300000
/* [한국어] 실제 요청을 보내는 SMC 함수 ID.
 * [값의 근거] 이 상수의 의미는 Ampere 의 펌웨어 인터페이스 규약에 속하며
 * 이 트리에는 그 문서가 없다. 코드가 그것을 어떻게 쓰는지로만 설명한다. */
#define REQUEST		0xf0700000
/* [한국어] 요청 종류 중 LED 제어를 뜻하는 명령 코드.
 * [값의 근거] 이 상수의 의미는 Ampere 의 펌웨어 인터페이스 규약에 속하며
 * 이 트리에는 그 문서가 없다. 코드가 그것을 어떻게 쓰는지로만 설명한다. */
#define LED_CMD		0x00000004
/* [한국어] 제어 대상이 attention LED 임을 뜻하는 값. 이 플랫폼에는 다른 종류의
 * LED 도 있을 수 있으나 이 드라이버는 attention 만 다룬다.
 * [값의 근거] 이 상수의 의미는 Ampere 의 펌웨어 인터페이스 규약에 속하며
 * 이 트리에는 그 문서가 없다. 코드가 그것을 어떻게 쓰는지로만 설명한다. */
#define LED_ATTENTION	0x00000002
/* [한국어] LED 켜기. */
#define LED_SET_ON	0x00000001
/* [한국어] LED 끄기. */
#define LED_SET_OFF	0x00000002
/* [한국어] LED 깜빡이기 — 슬롯 식별용이다. */
#define LED_SET_BLINK	0x00000003

/* [한국어] 펌웨어의 LED 서비스를 식별하는 128비트 UUID 를 32비트 넷으로 담는다.
 * 설정자: altra_led_probe() 가 ACPI 디바이스의 "uuid" 속성에서 읽는다.
 * 읽는 자: set_attention_status() 가 HANDLE_OPEN SMC 의 인자로 넘긴다.
 * 값 범위: 펌웨어가 정한 값. 속성이 없으면 probe 가 실패해 이 배열은 쓰이지 않는다.
 * 동기화: probe 에서 한 번 채운 뒤 읽기 전용이다. */
static u32 led_service_id[4];

/* [한국어]
 * led_status - 핫플러그 코어의 상태 코드를 펌웨어 LED 명령으로 번역한다
 *
 * @status: 핫플러그 코어가 넘긴 attention 상태(0 = 꺼짐, 1 = 켜짐, 2 = 식별).
 * @return: 펌웨어가 이해하는 LED 명령(LED_SET_ON / BLINK / OFF).
 *
 * sysfs 의 attention 인터페이스는 세 상태를 정의하고, 이 펌웨어는 세 명령을
 * 갖는다. 두 값 공간이 다르므로 그 사이를 잇는 표가 필요하다.
 *
 * default 가 OFF 인 것이 방어적이다 — 코어가 정의하지 않은 값을 보내도
 * LED 를 켜 두는 대신 끄는 쪽으로 떨어진다.
 *
 * 실행 컨텍스트: set_attention_status() 안, 인터럽트가 꺼진 구간.
 * 순수 값 변환이라 부수 효과가 없다.
 *
 * 에러 경로: 없다. 모든 입력에 대해 유효한 명령을 돌려준다.
 *
 * 호출 체인:
 *   set_attention_status() → [led_status]
 */
static int led_status(u8 status)
{
	/* [한국어] 핫플러그 코어가 쓰는 상태 코드를 이 펌웨어의 LED 명령으로 번역한다. */
	switch (status) {
	/* [한국어] 1 = 켜기. */
	case 1: return LED_SET_ON;
	/* [한국어] 2 = 깜빡이기(식별). */
	case 2: return LED_SET_BLINK;
	/* [한국어] 그 밖(0 포함)은 모두 끄기로 취급한다. 코어가 정의하지 않은 값을 보내도
	 * 안전한 쪽으로 떨어지게 하는 방어다. */
	default: return LED_SET_OFF;
	}
}

/* [한국어]
 * set_attention_status - 펌웨어에 SMC 를 보내 슬롯의 attention LED 를 바꾼다
 *
 * @slot: 대상 슬롯. pci_slot->bus 로 버스를 얻는다.
 * @status: 원하는 상태(0 = 꺼짐, 1 = 켜짐, 2 = 식별).
 * @return: 0 = 성공. -ENODEV = 루트 포트를 못 찾음, 핸들 열기 실패,
 *       또는 펌웨어가 요청을 거부함.
 *
 * 왜 SMC 인가: 이 플랫폼에서 LED 는 OS 가 직접 제어할 수 있는 레지스터가 아니라
 * 시스템 펌웨어가 관리하는 자원이다. 그래서 ARM 의 SMC(Secure Monitor Call)로
 * EL3 펌웨어에 요청을 보낸다 — 파일 첫머리의 영어 주석이 "requests to system
 * firmware" 라고 밝히는 그것이다.
 *
 * 동작 과정:
 *   1) 슬롯이 달린 버스에서 루트 포트를 찾는다. 펌웨어에 슬롯을 지목할 때
 *      루트 포트의 장치 번호를 쓰기 때문이다.
 *   2) 인터럽트를 끄고 세 개의 SMC 를 하나의 트랜잭션으로 수행한다 —
 *      핸들 열기, 요청, 핸들 닫기. 중간에 다른 코드가 같은 서비스에 접근하면
 *      핸들이 꼬이므로 그 구간을 보호해야 한다.
 *   3) HANDLE_OPEN 으로 UUID 를 넘겨 서비스 핸들을 얻는다. 반환값 a1 의
 *      상위 16비트가 핸들이다.
 *   4) REQUEST 로 LED 명령을 보낸다. 슬롯 식별자는 루트 포트의 장치 번호를
 *      4비트 밀고 PCI 도메인 번호 하위 4비트를 붙인 값이다 — (장치, 도메인)
 *      쌍이 이 플랫폼에서 슬롯을 유일하게 지목하는 방식이다.
 *   5) 요청 성공 여부와 무관하게 HANDLE_CLOSE 로 핸들을 닫는다. 열어 둔 핸들이
 *      쌓이면 펌웨어 자원이 고갈되기 때문이다. 닫기의 반환값은 확인하지 않는데,
 *      실패해도 할 수 있는 일이 없어서다.
 *
 * 락 대신 인터럽트 차단을 쓰는 이유는, SMC 가 CPU 를 EL3 로 넘겨 그 사이
 * 스케줄링 자체가 일어나지 않는다는 전제 위에 있는 것으로 보인다.
 *
 * 실행 컨텍스트: sysfs 쓰기 문맥에서 acpiphp 코어를 거쳐 불린다.
 * 인터럽트를 끈 구간에서 SMC 세 번을 수행하므로 그동안 지연이 늘어난다.
 *
 * 에러 경로: 루트 포트 부재는 SMC 를 시작하기 전이라 곧장 반환하고,
 * 핸들 열기 실패는 out 라벨로 가 인터럽트만 되살린다. 요청 실패는 ret 만
 * 기록하고 핸들 닫기를 반드시 거친다.
 *
 * 호출 체인:
 *   사용자의 sysfs attention 쓰기 → 핫플러그 코어 → acpiphp
 *     → acpiphp_attention_info.set_attn == [이 함수]
 *     → pcie_find_root_port() → arm_smccc_smc() ×3
 */
static int set_attention_status(struct hotplug_slot *slot, u8 status)
{
	/* [한국어] SMC 반환값 네 개를 담는 구조체. */
	struct arm_smccc_res res;
	/* [한국어] 슬롯이 속한 PCI 버스. */
	struct pci_bus *bus;
	/* [한국어] 그 버스를 거슬러 올라가 만나는 루트 포트. */
	struct pci_dev *root_port;
	/* [한국어] 인터럽트 플래그 저장용. */
	unsigned long flags;
	/* [한국어] 펌웨어가 돌려줄 서비스 핸들. */
	u32 handle;
	/* [한국어] 반환값. 성공 경로에서 대입하지 않으므로 0 초기화가 곧 성공 값이다. */
	int ret = 0;

	/* [한국어] 슬롯이 달린 버스를 얻는다. */
	bus = slot->pci_slot->bus;
	/* [한국어] 그 버스의 상위 브리지에서 루트 포트를 찾는다. 펌웨어에 슬롯을 지목할 때
	 * 루트 포트의 장치 번호를 쓰기 때문이다. */
	root_port = pcie_find_root_port(bus->self);
	/* [한국어] 루트 포트를 못 찾으면 어느 슬롯인지 펌웨어에 알릴 방법이 없다. */
	if (!root_port)
		/* [한국어] -ENODEV. */
		return -ENODEV;

	/* [한국어] 인터럽트를 끈다. SMC 호출 세 개가 하나의 트랜잭션(열기-요청-닫기)이라
	 * 중간에 다른 코드가 끼어들어 같은 서비스에 접근하면 핸들이 꼬인다.
	 * 락 대신 인터럽트 차단을 쓰는 이유는 SMC 가 CPU 를 EL3 로 넘겨
	 * 그 사이 스케줄링 자체가 일어나지 않는다는 전제 때문으로 보인다. */
	local_irq_save(flags);
	/* [한국어] LED 서비스 핸들을 연다. UUID 네 워드를 인자로 넘긴다. */
	arm_smccc_smc(HANDLE_OPEN, led_service_id[0], led_service_id[1],
		      led_service_id[2], led_service_id[3], 0, 0, 0, &res);
	/* [한국어] SMC 관례상 a0 이 0 이 아니면 오류다. */
	if (res.a0) {
		/* [한국어] 핸들을 못 얻었으므로 요청을 보낼 수 없다. */
		ret = -ENODEV;
		/* [한국어] 인터럽트만 되돌리는 정리 구간으로 — 아직 핸들이 없으니 닫을 것도 없다. */
		goto out;
	}
	/* [한국어] a1 의 상위 16비트가 핸들이다. 하위 비트를 잘라 내는 이 마스크가
	 * 펌웨어 규약의 일부다. */
	handle = res.a1 & 0xffff0000;

	/* [한국어] 실제 LED 명령을 보낸다. 인자 배치는 (명령=LED_CMD, 값=on/off/blink,
	 * 대상=ATTENTION, 슬롯 식별자, ..., 핸들) 이다. */
	arm_smccc_smc(REQUEST, LED_CMD, led_status(status), LED_ATTENTION,
		 /* [한국어] 슬롯 식별자를 조립한다 — 루트 포트의 장치 번호를 4비트 왼쪽으로 밀고
		  * PCI 도메인 번호 하위 4비트를 붙인다. 즉 (장치, 도메인) 쌍이 이 플랫폼에서
		  * 슬롯을 유일하게 지목하는 방식이다. */
		 (PCI_SLOT(root_port->devfn) << 4) | (pci_domain_nr(bus) & 0xf),
		 0, 0, handle, &res);
	/* [한국어] 요청 실패 검사. */
	if (res.a0)
		/* [한국어] -ENODEV 로 기록한다. 실패해도 아래 핸들 닫기는 반드시 수행한다. */
		ret = -ENODEV;

	/* [한국어] 핸들을 닫는다. 성공·실패와 무관하게 부르는 것이 중요하다 —
	 * 열어 둔 핸들이 쌓이면 펌웨어 자원이 고갈된다. 반환값은 확인하지 않는데,
	 * 닫기가 실패해도 할 수 있는 일이 없기 때문이다. */
	arm_smccc_smc(HANDLE_CLOSE, handle, 0, 0, 0, 0, 0, 0, &res);

 /* [한국어] 정리 라벨. 핸들 열기 실패와 정상 경로가 함께 도달한다. */
 out:
	/* [한국어] 인터럽트를 되살린다. */
	local_irq_restore(flags);
	/* [한국어] 기록해 둔 결과를 전달한다. */
	return ret;
}

/* [한국어]
 * get_attention_status - LED 상태 조회를 언제나 거부한다
 *
 * @slot: 대상 슬롯. 쓰지 않는다.
 * @status: 결과를 담을 출력 인자. 채우지 않는다.
 * @return: 언제나 -EINVAL.
 *
 * 이 펌웨어 인터페이스는 LED 상태를 되읽는 방법을 제공하지 않는다. 쓰기만 되고
 * 읽기는 안 되는 비대칭이 이 확장의 한계이며, 그것을 오류로 명확히 알리는 것이
 * 이 함수의 역할이다.
 *
 * 콜백을 아예 NULL 로 두지 않고 실패 함수를 두는 이유는, acpiphp 코어의
 * struct acpiphp_attention_info 가 두 콜백을 모두 요구하기 때문으로 보인다 —
 * NULL 검사 없이 부르는 구조라면 스텁이 반드시 있어야 한다.
 *
 * 실행 컨텍스트: sysfs 읽기 문맥.
 *
 * 에러 경로: 이 함수 자체가 오류 경로다.
 *
 * 호출 체인:
 *   사용자의 sysfs attention 읽기 → 핫플러그 코어 → acpiphp
 *     → acpiphp_attention_info.get_attn == [이 함수] → -EINVAL
 */
static int get_attention_status(struct hotplug_slot *slot, u8 *status)
{
	/* [한국어] 이 펌웨어 인터페이스는 LED 상태를 되읽는 방법을 제공하지 않는다.
	 * 그래서 언제나 -EINVAL 을 돌려주며, acpiphp 코어는 그것을 보고
	 * sysfs 의 attention 읽기를 실패로 처리한다. 쓰기만 되고 읽기는 안 되는
	 * 비대칭이 이 확장의 한계다. */
	return -EINVAL;
}

static struct acpiphp_attention_info ampere_altra_attn = {
	/* [한국어] attention LED 설정 콜백. */
	.set_attn = set_attention_status,
	/* [한국어] 조회 콜백 — 위에서 본 대로 언제나 실패한다. */
	.get_attn = get_attention_status,
	/* [한국어] 이 확장을 제공하는 모듈. acpiphp 코어가 참조 카운트를 잡아,
	 * 확장이 등록된 동안 이 모듈이 언로드되지 않게 한다. */
	.owner = THIS_MODULE,
};

/* [한국어]
 * altra_led_probe - ACPI 에서 UUID 를 읽고 acpiphp 확장으로 등록한다
 *
 * @pdev: ACPI HID "AMPC0008" 로 매칭된 플랫폼 디바이스.
 * @return: 0 = 성공. 음수 = uuid 속성 부재 또는 확장 등록 실패.
 *
 * 이 드라이버는 자체적으로 슬롯을 관리하지 않는다. acpiphp 가 이미 하고 있는
 * 핫플러그 처리에 "attention LED 를 이렇게 제어하라"는 콜백만 얹는 확장이다.
 * 그래서 probe 가 하는 일은 두 가지뿐이다.
 *
 *   1) ACPI 디바이스의 "uuid" 속성에서 32비트 네 개를 읽어 전역 배열에 담는다.
 *      이 값이 펌웨어의 LED 서비스를 지목하는 식별자이므로, 없으면 아무것도
 *      할 수 없어 probe 를 실패시킨다.
 *   2) acpiphp_register_attention() 으로 콜백 쌍을 등록한다. 이 시점부터
 *      사용자가 sysfs 의 attention 파일에 쓰면 set_attention_status() 가 불린다.
 *
 * DT 가 아니라 fwnode 계열 API 를 쓰는 것이 이 플랫폼이 ACPI 로 부팅한다는 표시다.
 *
 * 실행 컨텍스트: 드라이버 코어의 probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 두 지점 모두 곧장 return 한다. 1)이 성공한 뒤 2)가 실패하면
 * led_service_id 가 채워진 채 남지만, 확장이 등록되지 않아 아무도 그것을
 * 읽지 않으므로 문제가 되지 않는다.
 *
 * 호출 체인:
 *   ACPI 매칭 → platform_driver.probe == [이 함수]
 *     → fwnode_property_read_u32_array() → acpiphp_register_attention()
 */
static int altra_led_probe(struct platform_device *pdev)
{
	/* [한국어] ACPI 디바이스의 펌웨어 노드를 얻는다. DT 가 아니라 ACPI 로 부팅하는
	 * 플랫폼이라 fwnode 계열 API 를 쓴다. */
	struct fwnode_handle *fwnode = dev_fwnode(&pdev->dev);
	/* [한국어] 각 단계 결과. */
	int ret;

	/* [한국어] ACPI 가 제공하는 "uuid" 속성에서 32비트 네 개를 읽는다.
	 * 이 값이 있어야 펌웨어의 LED 서비스를 지목할 수 있다. */
	ret = fwnode_property_read_u32_array(fwnode, "uuid", led_service_id, 4);
	/* [한국어] 속성이 없거나 형식이 다르면, */
	if (ret) {
		/* [한국어] 무엇이 빠졌는지 알리고, */
		dev_err(&pdev->dev, "can't find uuid\n");
		/* [한국어] 오류를 전달한다. */
		return ret;
	}

	/* [한국어] acpiphp 코어에 이 확장을 등록한다. 이 시점부터 사용자가 sysfs 의
	 * attention 파일에 쓰면 위 set_attention_status() 가 불린다. */
	ret = acpiphp_register_attention(&ampere_altra_attn);
	/* [한국어] 등록 실패 — 이미 다른 확장이 등록되어 있는 경우 등이다. */
	if (ret) {
		/* [한국어] 실패 로그. */
		dev_err(&pdev->dev, "can't register driver\n");
		/* [한국어] 오류 전달. led_service_id 는 이미 채워졌지만 쓰이지 않으므로 문제가 없다. */
		return ret;
	}
	/* [한국어] 등록 완료. */
	return 0;
}

/* [한국어]
 * altra_led_remove - acpiphp 확장 등록을 해제한다
 *
 * @pdev: 제거되는 플랫폼 디바이스. 쓰지 않는다.
 *
 * probe 의 짝이다. 이 호출 이후 attention 요청이 이 파일로 오지 않는다.
 * 반환값이 없어 실패 개념이 없다.
 *
 * led_service_id 는 전역 배열이라 해제할 것이 없고, 확장이 등록 해제된 뒤에는
 * 아무도 읽지 않으므로 값을 지울 필요도 없다.
 *
 * 실행 컨텍스트: 언바인드 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   드라이버 코어의 언바인드 → platform_driver.remove == [이 함수]
 *     → acpiphp_unregister_attention()
 */
static void altra_led_remove(struct platform_device *pdev)
{
	/* [한국어] 확장 등록을 해제한다. 이 호출 이후 attention 요청이 이 파일로 오지 않는다.
	 * 반환값이 없어 실패 개념이 없다. */
	acpiphp_unregister_attention(&ampere_altra_attn);
}

static const struct acpi_device_id altra_led_ids[] = {
	/* [한국어] Ampere Altra 의 LED 서비스를 나타내는 ACPI HID. 이 ID 를 가진 디바이스가
	 * 펌웨어에 서술되어 있어야 이 드라이버가 붙는다. */
	{ "AMPC0008", 0 },
	/* [한국어] 테이블 끝을 알리는 빈 항목. */
	{ }
};
/* [한국어] 모듈 자동 로딩을 위해 매칭 테이블을 모듈 메타데이터로 내보낸다. */
MODULE_DEVICE_TABLE(acpi, altra_led_ids);

static struct platform_driver altra_led_driver = {
	.driver = {
		/* [한국어] 드라이버 이름. */
		.name = "ampere-altra-leds",
		/* [한국어] ACPI 매칭 테이블. of_match_table 이 아니라 acpi_match_table 인 것이
		 * 이 플랫폼이 DT 를 쓰지 않는다는 표시다. */
		.acpi_match_table = altra_led_ids,
	},
	/* [한국어] 디바이스가 나타났을 때 불릴 진입점. */
	.probe = altra_led_probe,
	/* [한국어] 사라질 때 불릴 정리 함수. */
	.remove = altra_led_remove,
};
/* [한국어] module_init/module_exit 보일러플레이트. */
module_platform_driver(altra_led_driver);

/* [한국어] modinfo 에 표시될 작성자. */
MODULE_AUTHOR("D Scott Phillips <scott@os.amperecomputing.com>");
/* [한국어] modinfo 에 표시될 설명. */
MODULE_DESCRIPTION("ACPI PCI Hot Plug Extension for Ampere Altra");
/* [한국어] 라이선스 선언. */
MODULE_LICENSE("GPL");
