// SPDX-License-Identifier: GPL-2.0+
/*
 * cpcihp_generic.c
 *
 * Generic port I/O CompactPCI driver
 *
 * Copyright 2002 SOMA Networks, Inc.
 * Copyright 2001 Intel San Luis Obispo
 * Copyright 2000,2001 MontaVista Software Inc.
 *
 * This generic CompactPCI hotplug driver should allow using the PCI hotplug
 * mechanism on any CompactPCI board that exposes the #ENUM signal as a bit
 * in a system register that can be read through standard port I/O.
 *
 * Send feedback to <scottm@somanetworks.com>
 */

/* [한국어] module_init/exit, module_param, MODULE_ 계열 매크로. */
/*
 * [한국어 설명] 포트 I/O 로 #ENUM 을 읽는 범용 CompactPCI 핫플러그 드라이버 (cpcihp_generic.c)
 *
 * === 파일의 역할 ===
 * CompactPCI 보드의 슬롯 착탈을 리눅스 핫플러그로 노출한다. CompactPCI 는
 * 통신 장비 등에서 쓰이던 랙 형식 규격으로, 슬롯에 보드를 꽂거나 뽑을 때
 * #ENUM 이라는 신호선이 서고 소프트웨어가 그것을 보고 반응한다.
 * 이 드라이버의 특징은 파일 상단 영어 주석에 그대로 적혀 있다 — 특정 보드가
 * 아니라, #ENUM 을 표준 I/O 포트의 한 비트로 읽을 수 있는 **어떤** 보드에도
 * 쓸 수 있게 만들어졌다. 보드마다 다른 것은 포트 주소와 비트 위치뿐이고
 * 둘 다 모듈 파라미터로 받으므로, 실제 하드웨어 코드가 query_enum() 한
 * 함수, 그것도 세 줄로 끝난다.
 * 대신 사람이 알려 주어야 할 것이 많다. 어느 브리지 아래인지(bridge,
 * "<버스>:<슬롯>" 16진수), 몇 번 슬롯부터 몇 번까지인지(first_slot,
 * last_slot), #ENUM 이 어느 포트의 몇 번 비트인지(port, enum_bit).
 * 그래서 이 파일에서 가장 긴 함수가 하드웨어 조작이 아니라 그 파라미터를
 * 검사·파싱하는 validate_parameters() 다.
 * 오래된 코드라는 흔적이 곳곳에 있다. dev_err 가 아니라 printk 매크로를
 * 직접 정의해 쓰고(로그 시점에 대응하는 struct device 가 없다), PCI 도메인을
 * 0 으로 고정하며, inb_p() 처럼 ISA 시절 지연을 넣는 접근자를 쓴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * CompactPCI 핫플러그는 두 층이다. 위층인 cpci_hotplug_core.c 가 슬롯
 * 자료구조와 sysfs 노출, 그리고 #ENUM 을 주기적으로 확인하는 폴링 스레드를
 * 담당하고, 아래층인 이 파일 같은 드라이버가 "지금 #ENUM 이 서 있는가" 라는
 * 단 하나의 질문에 답한다.
 * 흐름: module_init → cpcihp_generic_init()
 *   → validate_parameters() 로 파라미터 파싱
 *   → request_region() 으로 #ENUM 포트 한 바이트 확보
 *   → pci_get_domain_bus_and_slot() 으로 브리지를 찾아 세컨더리 버스 확보
 *   → cpci_hp_register_controller() 로 query_enum 콜백 등록
 *   → cpci_hp_register_bus() 로 슬롯 범위 등록(sysfs 항목이 생긴다)
 *   → cpci_hp_start() 로 폴링 스레드 시작
 * 그 뒤로는 폴링 스레드가 주기적으로 query_enum() 을 부르고, 값이 바뀌면
 * 코어가 슬롯 삽입·제거 처리를 진행한다.
 * 실행 컨텍스트는 둘이다. 초기화·해제는 모듈 로드/언로드 시점의 프로세스
 * 컨텍스트이고, query_enum() 은 코어의 폴링 스레드에서 불린다. 어느 쪽도
 * 인터럽트 문맥이 아니다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/hotplug/cpci_hotplug_core.c 의 cpci_hp_register_controller() /
 * cpci_hp_register_bus() / cpci_hp_start() / 그 짝이 되는 해제 함수들.
 * 규약은 cpci_hotplug.h 의 struct cpci_hp_controller 와 그 ops 다.
 * 이 드라이버는 그 ops 중 query_enum 하나만 채운다.
 * 아래쪽: I/O 포트 접근(inb_p, request_region, release_region)과
 * PCI 코어의 pci_get_domain_bus_and_slot() / pci_dev_put().
 * 옆쪽: 같은 디렉터리의 cpcihp_zt5550.c 가 대비되는 예다. 그쪽은 특정
 * 보드(Ziatech ZT5550)의 CSR 를 직접 다루는 반면, 이 파일은 보드에 대해
 * 아무것도 모른 채 파라미터로만 동작한다.
 * 공유 상태: 전역 변수 열한 개가 사실상 이 드라이버의 상태 전부다.
 * 그중 여섯(debug, bridge, first_slot, last_slot, port, enum_bit)은 모듈
 * 파라미터이고, 셋(bridge_busnr, bridge_slot, enum_mask)은 그것을 파싱한
 * 결과이며, bus 는 조회 결과, generic_hpc/generic_hpc_ops 는 코어에 넘길
 * 서술자다. 인스턴스가 하나뿐이라는 전제 위에 있는 구조다.
 *
 * === 주요 함수/구조체 요약 ===
 * - validate_parameters(): 이 파일에서 가장 긴 함수. "<버스>:<슬롯>" 문자열을
 *   두 번에 걸쳐 파싱하며 매번 세 가지를 확인한다 — 한 글자라도 읽었는가,
 *   범위 안인가, 구분자가 맞는가. bridge 가 아예 없을 때만 err 대신 info 로
 *   남기는데, 설정하지 않은 시스템에서 오류 로그를 내지 않으려는 배려다.
 *   마지막에 enum_bit 를 마스크로 바꿔 캐시한다.
 * - query_enum(): 이 드라이버가 코어에 제공하는 유일한 콜백이자 유일한
 *   하드웨어 접근. 포트에서 한 바이트를 읽어 마스크와 비교한다.
 * - cpcihp_generic_init(): 다섯 단계를 밟고, 되감기를 라벨 두 개로 계단
 *   지어 둔다. [상류 코드 관찰] 다만 세 오류 경로 모두 request_region() 으로
 *   잡은 I/O 영역을 놓지 않아, 실패 후 모듈을 다시 넣으면 -EBUSY 가 난다.
 * - cpcihp_generic_exit(): 초기화의 정확한 역순. 폴링을 가장 먼저 멈추는데,
 *   그러지 않으면 해제 중인 자료구조를 폴링 스레드가 밟기 때문이다.
 * - 로그 매크로 넷(dbg/err/info/warn): dev_ 계열을 쓸 수 없어 직접 정의한다.
 *   dbg 만 debug 파라미터로 켜고 끌 수 있으며, warn 은 이 파일에서 쓰이지 않는다.
 * - module_param_hw(port, ushort, ioport, 0): 평범한 module_param 과 달리
 *   이것이 I/O 포트임을 커널에 알려 자원 충돌 검사를 받게 한다.
 *
 * === NVMe 관점 ===
 * 접점이 없다. CompactPCI 는 NVMe 가 등장하기 훨씬 전의 규격이고, 이
 * 드라이버가 다루는 슬롯에 꽂히는 것은 통신·산업용 보드다.
 * 다만 구조는 낯익다. "하드웨어 상태를 읽는 얇은 콜백 하나만 제공하고
 * 나머지는 공용 코어가 처리한다" 는 분담은 pciehp 나 acpiphp 도 같으며,
 * NVMe SSD 를 핫플러그로 다루는 pciehp 역시 슬롯 상태를 읽는 몇 개의
 * 콜백을 중심으로 짜여 있다. 다른 것은 그 상태를 어디서 읽느냐뿐이다 —
 * 여기서는 I/O 포트 한 비트, pciehp 에서는 Slot Status 레지스터다.
 */

#include <linux/module.h>
/* [한국어] __init / __exit 섹션 표시. */
#include <linux/init.h>
/* [한국어] -EINVAL / -EBUSY / -ENODEV. */
#include <linux/errno.h>
/* [한국어] pci_get_domain_bus_and_slot(), PCI_DEVFN(), PCI_HEADER_TYPE_BRIDGE,
 * 그리고 request_region()/inb_p() 로 이어지는 I/O 포트 접근. */
#include <linux/pci.h>
/* [한국어] simple_strtoul() 을 쓰기 위한 문자열 헤더. */
#include <linux/string.h>
/* [한국어] struct cpci_hp_controller 와 그 ops, cpci_hp_register_controller() 등
 * CompactPCI 핫플러그 코어의 규약. */
#include "cpci_hotplug.h"

/* [한국어] 모듈 정보 문자열. 초기화 로그와 MODULE_ 매크로에 함께 쓴다. */
#define DRIVER_VERSION	"0.1"
/* [한국어] 작성자. */
#define DRIVER_AUTHOR	"Scott Murray <scottm@somanetworks.com>"
/* [한국어] 설명. */
#define DRIVER_DESC	"Generic port I/O CompactPCI Hot Plug Driver"

/* [한국어] 내장 빌드이면, */
#if !defined(MODULE)
/* [한국어] 이름을 문자열 상수로 박는다. THIS_MODULE 이 없기 때문이다. */
#define MY_NAME	"cpcihp_generic"
#else
/* [한국어] 모듈 빌드이면 모듈 이름을 실행 시점에 가져온다. */
#define MY_NAME	THIS_MODULE->name
#endif

/* [한국어] debug 파라미터가 켜졌을 때만 찍는 디버그 매크로. do-while(0) 로 감싼 것은
 * if 문 뒤에 세미콜론 없이 붙어도 문법이 깨지지 않게 하는 관용구다. */
#define dbg(format, arg...)					\
	do {							\
		if (debug)					\
			printk(KERN_DEBUG "%s: " format "\n",	\
				MY_NAME, ## arg);		\
	} while (0)
/* [한국어] 오류 로그. 이 파일은 dev_err 를 쓸 수 없는데, 로그를 낼 시점에 대응하는
 * struct device 가 없기 때문이다(모듈 파라미터로 동작하는 드라이버다). */
#define err(format, arg...) printk(KERN_ERR "%s: " format "\n", MY_NAME, ## arg)
/* [한국어] 정보 로그. */
#define info(format, arg...) printk(KERN_INFO "%s: " format "\n", MY_NAME, ## arg)
/* [한국어] 경고 로그. 이 파일에서는 쓰이지 않는다. */
#define warn(format, arg...) printk(KERN_WARNING "%s: " format "\n", MY_NAME, ## arg)

/* local variables */
/* [한국어] 디버그 로그를 켤지. module_param 으로 노출되며 실행 중에도 바꿀 수 있다.
 * 설정자: 모듈 파라미터(0644).  읽는 자: 위 dbg 매크로.
 * 값 범위: true/false.  동기화: 단순 읽기라 보호가 없다. */
static bool debug;
/* [한국어] 핫스왑 버스 브리지를 "<버스>:<슬롯>" 16진수 문자열로 받는 파라미터.
 * 설정자: 모듈 파라미터(읽기 전용).  읽는 자: validate_parameters() 가 파싱한다.
 * 값 범위: NULL 이면 드라이버가 스스로 비활성화된다.
 * 동기화: 초기화 시 한 번 읽는다. */
static char *bridge;
/* [한국어] 위 문자열에서 파싱한 버스 번호.
 * 설정자: validate_parameters().  읽는 자: cpcihp_generic_init() 의 장치 조회.
 * 값 범위: 0~0xff.  동기화: 초기화 시 정해진다. */
static u8 bridge_busnr;
/* [한국어] 파싱한 슬롯 번호.
 * 설정자/읽는 자: 위와 같다.
 * 값 범위: 0~0x1f. PCI 스펙상 버스당 장치 번호의 범위다.
 * 동기화: 초기화 시 정해진다. */
static u8 bridge_slot;
/* [한국어] 핫스왑 슬롯들이 매달린 버스(브리지의 세컨더리 버스).
 * 설정자: cpcihp_generic_init().  읽는 자: 등록·해제 경로.
 * 값 범위: 유효한 pci_bus 포인터.
 * 동기화: 초기화 시 정해지고 해제까지 유지된다. */
static struct pci_bus *bus;
/* [한국어] 핫스왑 대상 슬롯 범위의 시작.
 * 설정자: 모듈 파라미터.  읽는 자: cpci_hp_register_bus() 인자.
 * 값 범위: 0 이면 오류로 처리된다 — 지정이 필수라는 뜻이다.
 * 동기화: 초기화 시 읽는다. */
static u8 first_slot;
/* [한국어] 범위의 끝.
 * 설정자/읽는 자: 위와 같다.
 * 값 범위: first_slot 이상이어야 한다.
 * 동기화: 초기화 시 읽는다. */
static u8 last_slot;
/* [한국어] #ENUM 신호가 보이는 I/O 포트 주소.
 * 설정자: 모듈 파라미터. module_param_hw 로 ioport 종류임을 명시해
 *   커널이 자원 충돌을 검사할 수 있게 한다.
 * 읽는 자: query_enum() 이 매번 읽고, 초기화/해제가 region 을 잡고 놓는다.
 * 값 범위: 보드마다 다르다.  동기화: 초기화 시 정해진다. */
static u16 port;
/* [한국어] 그 포트 안에서 #ENUM 이 놓인 비트 번호.
 * 설정자: 모듈 파라미터.  읽는 자: validate_parameters() 가 마스크로 바꾼다.
 * 값 범위: 0~7. 한 바이트를 읽으므로 그 이상은 오류다.
 * 동기화: 초기화 시 정해진다. */
static unsigned int enum_bit;
/* [한국어] 위 비트 번호를 미리 마스크로 바꿔 둔 것.
 * 설정자: validate_parameters() 의 마지막 줄.
 * 읽는 자: query_enum() 이 매 호출마다 쓴다.
 * 값 범위: 1, 2, 4, ... 0x80 중 하나.
 * 동기화: 초기화 시 정해진다. 매번 시프트하지 않으려는 캐시다. */
static u8 enum_mask;

/* [한국어] 핫플러그 코어에 넘길 콜백 표. 이 드라이버는 query_enum 하나만 채운다. */
static struct cpci_hp_controller_ops generic_hpc_ops;
/* [한국어] 컨트롤러 서술자. 위 ops 를 가리키며, 코어가 이 포인터로 드라이버를 부른다. */
static struct cpci_hp_controller generic_hpc;

/* [한국어]
 * validate_parameters - 모듈 파라미터를 검사하고 파싱한다
 *
 * @return: 0 = 모두 유효, -EINVAL = 하나라도 잘못됨.
 *
 * 이 드라이버는 하드웨어를 스스로 찾아내지 못한다. 어느 브리지 아래의 어느
 * 슬롯이 핫스왑 대상이고 #ENUM 신호가 어느 포트의 어느 비트에 있는지를
 * 사람이 모듈 파라미터로 알려 주어야 한다. 그 값들을 여기서 한 번에 검사한다.
 *
 * 브리지 파싱이 가장 손이 많이 간다. "<버스>:<슬롯>" 형식의 16진수 문자열을
 * simple_strtoul() 로 두 번 잘라 읽으며, 매번 세 가지를 확인한다 —
 * 한 글자라도 읽었는가(p == str 검사), 범위 안인가, 구분자가 맞는가.
 *
 * bridge 가 아예 없을 때만 err 가 아니라 info 로 남기는 것이 눈에 띈다.
 * 이 모듈이 자동 로드될 수 있어, 설정하지 않은 시스템에서 오류 로그를 내지
 * 않으려는 배려다.
 *
 * 마지막 줄에서 enum_bit 를 마스크로 바꿔 캐시한다. query_enum() 이 인터럽트
 * 없이 자주 불리므로 매번 시프트하지 않게 하려는 것이다.
 *
 * 실행 컨텍스트: 모듈 초기화. __init 이라 부팅 후 해제된다.
 *
 * 에러 경로: 모두 -EINVAL 이며, 어느 검사에서 걸렸는지는 로그로만 구분된다.
 *
 * 호출 체인:
 *   cpcihp_generic_init() → [이 함수] → simple_strtoul() ×2
 */
static int __init validate_parameters(void)
{
	/* [한국어] 파싱 커서. */
	char *str;
	/* [한국어] strtoul 이 멈춘 위치를 받을 곳. */
	char *p;
	/* [한국어] 파싱한 값. u8 로 좁히기 전에 범위를 검사하려고 unsigned long 으로 받는다. */
	unsigned long tmp;

	/* [한국어] bridge 파라미터가 없으면 이 드라이버를 쓸 수 없다. */
	if (!bridge) {
		/* [한국어] 오류가 아니라 정보로 남긴다. 이 모듈이 자동 로드될 수 있어, 설정하지
		 * 않은 시스템에서 오류 로그를 내지 않으려는 배려다. */
		info("not configured, disabling.");
		/* [한국어] 그래도 반환은 실패다. */
		return -EINVAL;
	}
	/* [한국어] 파싱을 시작한다. */
	str = bridge;
	/* [한국어] 빈 문자열이면, */
	if (!*str)
		return -EINVAL;

	/* [한국어] 16진수로 버스 번호를 읽는다. */
	tmp = simple_strtoul(str, &p, 16);
	/* [한국어] 한 글자도 읽지 못했거나(p == str) 범위를 넘으면, */
	if (p == str || tmp > 0xff) {
		err("Invalid hotplug bus bridge device bus number");
		return -EINVAL;
	}
	/* [한국어] u8 로 좁혀 저장한다. 위에서 범위를 확인했으므로 안전하다. */
	bridge_busnr = (u8) tmp;
	/* [한국어] 파싱 결과를 디버그 로그로 남긴다. */
	dbg("bridge_busnr = 0x%02x", bridge_busnr);
	/* [한국어] 구분자가 콜론이 아니면 형식이 틀렸다. */
	if (*p != ':') {
		err("Invalid hotplug bus bridge device");
		return -EINVAL;
	}
	/* [한국어] 콜론 다음부터 슬롯 번호를 읽는다. */
	str = p + 1;
	/* [한국어] 역시 16진수. */
	tmp = simple_strtoul(str, &p, 16);
	/* [한국어] 읽지 못했거나 0x1f 를 넘으면, */
	if (p == str || tmp > 0x1f) {
		err("Invalid hotplug bus bridge device slot number");
		return -EINVAL;
	}
	/* [한국어] u8 로 좁힌다. */
	bridge_slot = (u8) tmp;
	/* [한국어] 디버그 로그. */
	dbg("bridge_slot = 0x%02x", bridge_slot);

	/* [한국어] 슬롯 범위도 확인한다. */
	dbg("first_slot = 0x%02x", first_slot);
	/* [한국어] 끝 슬롯. */
	dbg("last_slot = 0x%02x", last_slot);
	/* [한국어] 둘 중 하나라도 0 이면 지정하지 않은 것이다. 슬롯 번호 0 을 유효하지
	 * 않은 값으로 쓰는 관례다. */
	if (!(first_slot && last_slot)) {
		err("Need to specify first_slot and last_slot");
		return -EINVAL;
	}
	/* [한국어] 범위가 뒤집혀 있으면, */
	if (last_slot < first_slot) {
		err("first_slot must be less than last_slot");
		return -EINVAL;
	}

	/* [한국어] 포트 주소. */
	dbg("port = 0x%04x", port);
	/* [한국어] 비트 번호. */
	dbg("enum_bit = 0x%02x", enum_bit);
	/* [한국어] 한 바이트를 읽으므로 8 이상은 있을 수 없다. */
	if (enum_bit > 7) {
		err("Invalid #ENUM bit");
		return -EINVAL;
	}
	/* [한국어] 비트 번호를 마스크로 바꿔 캐시해 둔다. query_enum() 이 매 호출마다
	 * 시프트하지 않아도 되게 하려는 것이다. */
	enum_mask = 1 << enum_bit;
	/* [한국어] 모든 검사를 통과했다. */
	return 0;
}

/* [한국어]
 * query_enum - #ENUM 신호가 서 있는지 I/O 포트에서 읽어 답한다
 *
 * @return: 1 = #ENUM 이 서 있음(슬롯 상태가 바뀜), 0 = 아님.
 *
 * 이 드라이버가 CompactPCI 핫플러그 코어에 제공하는 유일한 콜백이다.
 * 파일 상단의 영어 주석이 밝히듯, 이 드라이버는 #ENUM 신호를 표준 I/O
 * 포트로 읽을 수 있는 어떤 CompactPCI 보드에서도 쓸 수 있게 만들어졌다.
 * 보드마다 다른 것은 포트 주소와 비트 위치뿐이고, 그 둘을 모듈 파라미터로
 * 받으므로 코드가 이렇게 짧다.
 *
 * inb_p() 의 _p 접미사는 읽기 뒤에 짧은 지연을 넣는다는 뜻으로, 느린 ISA
 * 계열 장치를 배려하는 오래된 관용구다.
 *
 * 마스크와 비교해 0/1 로 만드는 이유는 반환 타입이 int 이고 코어가 참/거짓을
 * 기대하기 때문이다. 마스크 값(예: 0x40)을 그대로 돌려주어도 참이긴 하지만,
 * 규약을 명확히 하려는 것이다.
 *
 * 실행 컨텍스트: 코어의 폴링 스레드. 프로세스 컨텍스트이며 잠들지 않는다.
 *
 * 에러 경로: 없다. 포트 읽기는 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   cpci_hp_start() 가 띄운 폴링 스레드 → controller->ops->query_enum
 *     → [이 함수] → inb_p()
 */
static int query_enum(void)
{
	/* [한국어] 포트에서 읽은 한 바이트. */
	u8 value;

	/* [한국어] I/O 포트를 읽는다. _p 판은 읽기 뒤에 짧은 지연을 넣는 것으로,
	 * 느린 ISA 계열 장치를 배려하는 오래된 관용구다. */
	value = inb_p(port);
	/* [한국어] #ENUM 비트가 서 있으면 1, 아니면 0. 마스크와 비교해 참/거짓으로 만드는
	 * 이유는 반환 타입이 int 이고 호출자가 0/1 을 기대하기 때문이다. */
	return ((value & enum_mask) == enum_mask);
}

/* [한국어]
 * cpcihp_generic_init - 파라미터를 검사하고 CompactPCI 핫플러그를 시작한다
 *
 * @return: 0 = 성공, -EINVAL / -EBUSY / -ENODEV 또는 코어가 낸 오류.
 *
 * 모듈 진입점이다. 다섯 단계를 차례로 밟는다 — 파라미터 검사, I/O 영역 확보,
 * 브리지 장치 조회, 컨트롤러 등록, 슬롯 등록과 폴링 시작.
 *
 * 브리지 조회에서 도메인을 0 으로 고정하는 점이 시대를 보여 준다. 이 드라이버가
 * 대상으로 하는 CompactPCI 시스템에는 다중 PCI 도메인이 없다.
 *
 * 조회로 올린 참조를 세컨더리 버스만 챙긴 뒤 바로 놓는다. 버스는 브리지가
 * 살아 있는 한 유지된다는 전제이며, 그래서 전역 bus 포인터가 안전하다고 본다.
 *
 * 되감기가 라벨 두 개로 계단을 이룬다. 폴링 시작이 실패하면 버스 등록부터,
 * 버스 등록이 실패하면 컨트롤러 등록부터 되돌린다.
 *
 * [상류 코드 관찰] 세 오류 경로 모두 request_region() 으로 잡은 I/O 영역을
 * 놓지 않는다. 브리지 조회 실패(:143)와 두 되감기 라벨 어디에도
 * release_region() 이 없어, 실패 후 모듈을 다시 넣으면 -EBUSY 가 난다.
 * 코드는 고치지 않고 사실만 기록한다.
 *
 * 실행 컨텍스트: 모듈 초기화. __init.
 *
 * 에러 경로: 위 관찰 참조.
 *
 * 호출 체인:
 *   module_init → [이 함수] → validate_parameters() → request_region()
 *     → pci_get_domain_bus_and_slot() → cpci_hp_register_controller()
 *     → cpci_hp_register_bus() → cpci_hp_start()
 */
static int __init cpcihp_generic_init(void)
{
	/* [한국어] 각 단계의 결과. */
	int status;
	/* [한국어] 확보한 I/O 영역. */
	struct resource *r;
	/* [한국어] 찾아낸 브리지 장치. */
	struct pci_dev *dev;

	/* [한국어] 버전과 함께 시작을 알린다. */
	info(DRIVER_DESC " version: " DRIVER_VERSION);
	/* [한국어] 모듈 파라미터를 검사·파싱한다. */
	status = validate_parameters();
	/* [한국어] 하나라도 잘못됐으면, */
	if (status)
		return status;

	/* [한국어] #ENUM 포트 한 바이트를 독점 확보한다. 다른 드라이버가 같은 포트를
	 * 건드리지 못하게 하는 것이 목적이다. */
	r = request_region(port, 1, "#ENUM hotswap signal register");
	/* [한국어] 이미 누가 쓰고 있으면, */
	if (!r)
		return -EBUSY;

	/* [한국어] 파싱한 버스·슬롯으로 브리지 장치를 찾는다. 도메인은 0 으로 고정인데,
	 * 이 드라이버가 대상으로 하는 오래된 CompactPCI 시스템에 다중 도메인이
	 * 없기 때문이다. */
	dev = pci_get_domain_bus_and_slot(0, bridge_busnr,
					  PCI_DEVFN(bridge_slot, 0));
	/* [한국어] 장치가 없거나 브리지가 아니면 지정이 잘못된 것이다. */
	if (!dev || dev->hdr_type != PCI_HEADER_TYPE_BRIDGE) {
		/* [한국어] 어떤 문자열을 받았는지 함께 남긴다. */
		err("Invalid bridge device %s", bridge);
		/* [한국어] 참조를 놓는다. dev 가 NULL 이어도 안전하다. */
		pci_dev_put(dev);
		/* [한국어] 잘못된 인자. */
		return -EINVAL;
	}
	/* [한국어] 브리지의 세컨더리 버스가 핫스왑 슬롯들이 매달린 버스다. */
	bus = dev->subordinate;
	/* [한국어] 버스 포인터만 남기고 장치 참조는 놓는다. 버스는 브리지가 살아 있는 한
	 * 유지되므로 이렇게 해도 된다는 전제다. */
	pci_dev_put(dev);

	/* [한국어] 컨트롤러 서술자를 0 으로 초기화한다. static 이라 이미 0 이지만,
	 * 모듈을 뺐다 다시 넣는 경우를 위해 명시적으로 지운다. */
	memset(&generic_hpc, 0, sizeof(struct cpci_hp_controller));
	/* [한국어] 이 드라이버가 제공하는 유일한 콜백을 연결한다. */
	generic_hpc_ops.query_enum = query_enum;
	/* [한국어] 서술자에 콜백 표를 매단다. */
	generic_hpc.ops = &generic_hpc_ops;

	/* [한국어] 코어에 컨트롤러를 등록한다. */
	status = cpci_hp_register_controller(&generic_hpc);
	/* [한국어] 실패하면, */
	if (status != 0) {
		err("Could not register cPCI hotplug controller");
		/* [한국어] 장치 없음으로 보고한다. status 를 그대로 쓰지 않고 -ENODEV 로 바꾸는
		 * 것이 아래 두 오류 경로와 다른 점이다. */
		return -ENODEV;
	}
	/* [한국어] 등록 성공을 디버그 로그에 남긴다. */
	dbg("registered controller");

	/* [한국어] 슬롯 범위를 코어에 알린다. 이 호출이 각 슬롯의 sysfs 항목을 만든다. */
	status = cpci_hp_register_bus(bus, first_slot, last_slot);
	/* [한국어] 실패하면, */
	if (status != 0) {
		err("Could not register cPCI hotplug bus");
		/* [한국어] 컨트롤러 등록을 되돌리러 간다. */
		goto init_bus_register_error;
	}
	/* [한국어] 디버그 로그. */
	dbg("registered bus");

	/* [한국어] 폴링 스레드를 띄워 실제 감시를 시작한다. */
	status = cpci_hp_start();
	/* [한국어] 실패하면, */
	if (status != 0) {
		err("Could not started cPCI hotplug system");
		/* [한국어] 버스 등록부터 되돌리러 간다. */
		goto init_start_error;
	}
	/* [한국어] 디버그 로그. */
	dbg("started cpci hp system");
	/* [한국어] 모든 준비가 끝났다. */
	return 0;
/* [한국어] 시작 실패 경로. */
init_start_error:
	/* [한국어] 버스 등록을 되돌리고, */
	cpci_hp_unregister_bus(bus);
/* [한국어] 버스 등록 실패 경로. 위에서 흘러내려 온다. */
init_bus_register_error:
	/* [한국어] 컨트롤러 등록을 되돌린다. */
	cpci_hp_unregister_controller(&generic_hpc);
	/* [한국어] 어느 단계에서 실패했는지 알 수 있게 코드를 남긴다. */
	err("status = %d", status);
	/* [한국어] 원래 오류를 그대로 올려보낸다.
	 * [상류 코드 관찰] 두 되감기 경로 모두 request_region() 으로 잡은 I/O
	 * 영역을 놓지 않는다. 브리지 조회 실패 경로(:143)도 마찬가지다.
	 * 코드는 고치지 않고 사실만 적는다. */
	return status;

}

/* [한국어]
 * cpcihp_generic_exit - 폴링을 멈추고 등록한 것을 모두 되돌린다
 *
 * 모듈 종료점이다. 초기화의 정확한 역순으로 되감는다 — 폴링 정지,
 * 슬롯 해제, 컨트롤러 해제, I/O 영역 반환.
 *
 * 폴링을 가장 먼저 멈추는 것이 중요하다. 아래에서 슬롯과 컨트롤러를 해제하는
 * 동안 폴링 스레드가 query_enum() 을 부르면, 이미 해제된 자료구조를 밟게 된다.
 *
 * 실행 컨텍스트: 모듈 해제. __exit.
 *
 * 에러 경로: 없다. 반환값이 없고 각 해제 함수도 실패를 알리지 않는다.
 *
 * 호출 체인:
 *   module_exit → [이 함수] → cpci_hp_stop() → cpci_hp_unregister_bus()
 *     → cpci_hp_unregister_controller() → release_region()
 */
static void __exit cpcihp_generic_exit(void)
{
	/* [한국어] 먼저 폴링을 멈춘다. 아래 해제 중에 콜백이 불리면 안 되기 때문이다. */
	cpci_hp_stop();
	/* [한국어] 슬롯 등록을 해제한다. */
	cpci_hp_unregister_bus(bus);
	/* [한국어] 컨트롤러 등록을 해제한다. 등록의 역순이다. */
	cpci_hp_unregister_controller(&generic_hpc);
	/* [한국어] I/O 영역을 놓는다. */
	release_region(port, 1);
}

/* [한국어] 모듈 진입점. */
module_init(cpcihp_generic_init);
/* [한국어] 모듈 종료점. */
module_exit(cpcihp_generic_exit);

/* [한국어] 작성자. */
MODULE_AUTHOR(DRIVER_AUTHOR);
/* [한국어] 설명. */
MODULE_DESCRIPTION(DRIVER_DESC);
/* [한국어] 라이선스. 없으면 GPL 전용 심볼을 쓸 수 없다. */
MODULE_LICENSE("GPL");
/* [한국어] 실행 중에도 바꿀 수 있게 쓰기 권한을 준다. 디버그 로그라 안전하다. */
module_param(debug, bool, S_IRUGO | S_IWUSR);
/* [한국어] 설명 문자열. */
MODULE_PARM_DESC(debug, "Debugging mode enabled or not");
/* [한국어] 권한 0 이라 sysfs 에 노출되지 않는다. 부팅/삽입 시에만 지정할 수 있다는 뜻이다. */
module_param(bridge, charp, 0);
/* [한국어] 형식 설명. 사용자가 modinfo 로 읽는다. */
MODULE_PARM_DESC(bridge, "Hotswap bus bridge device, <bus>:<slot> (bus and slot are in hexadecimal)");
/* [한국어] 슬롯 범위 시작. */
module_param(first_slot, byte, 0);
/* [한국어] 설명. */
MODULE_PARM_DESC(first_slot, "Hotswap bus first slot number");
/* [한국어] 슬롯 범위 끝. */
module_param(last_slot, byte, 0);
/* [한국어] 설명. */
MODULE_PARM_DESC(last_slot, "Hotswap bus last slot number");
/* [한국어] module_param_hw 로 이것이 I/O 포트임을 알린다. 그러면 커널이 다른
 * 드라이버와의 자원 충돌을 검사해 준다 — 평범한 module_param 과의 차이다. */
module_param_hw(port, ushort, ioport, 0);
/* [한국어] 설명. */
MODULE_PARM_DESC(port, "#ENUM signal I/O port");
/* [한국어] #ENUM 비트 번호. */
module_param(enum_bit, uint, 0);
/* [한국어] 허용 범위를 설명에 명시한다. */
MODULE_PARM_DESC(enum_bit, "#ENUM signal bit (0-7)");
