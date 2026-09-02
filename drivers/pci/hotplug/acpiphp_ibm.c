// SPDX-License-Identifier: GPL-2.0+
/*
 * ACPI PCI Hot Plug IBM Extension
 *
 * Copyright (C) 2004 Vernon Mauery <vernux@us.ibm.com>
 * Copyright (C) 2004 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <vernux@us.ibm.com>
 *
 */

/*
 * [한국어 설명] IBM 서버용 ACPI PCI 핫플러그 확장 (acpiphp_ibm.c)
 *
 * === 파일의 역할 ===
 * 같은 디렉터리의 acpiphp 드라이버에 **IBM 고유의 주의(attention) LED
 * 제어** 를 얹는 확장 모듈이다. 표준 ACPI 핫플러그가 다루지 못하는 두
 * 가지를 메운다.
 *
 *   1) **주의 LED 켜고 끄기·읽기.** acpiphp 코어가 콜백 표를 등록받는
 *      자리가 있고, 이 파일이 거기에 자기 함수 둘을 꽂는다.
 *   2) **aPCI 표를 sysfs 로 내보내기.** 슬롯 정보를 담은 펌웨어 표를
 *      사용자 공간이 통째로 읽을 수 있게 한다.
 *
 * 그런데 이 파일의 성격을 규정하는 것은 **LED 상태를 읽는 방법이 없다는
 * 점** 이다. 켜는 것은 APLS 라는 ACPI 메서드로 되는데, 읽는 ACPI 메서드가
 * 없다. 그래서 상태를 알려면 aPCI 표 전체를 ACPI 에서 다시 읽어 와
 * 슬롯 서술자를 찾아 그 안의 비트를 봐야 한다 — 그 우회로가
 * ibm_slot_from_id() 이고, LED 를 켤 때조차 슬롯 ID 를 얻기 위해 같은
 * 일을 한다. 즉 **LED 한 번 조작에 표 한 벌을 읽고 버린다.**
 *
 * === aPCI 표의 구조 ===
 * ACPI 의 APCI 메서드가 버퍼 여러 개로 이뤄진 패키지를 돌려주는데,
 * 이 파일은 그것을 이어 붙여 한 덩어리로 만든다(ibm_get_table_from_acpi).
 *
 * 그 덩어리는 **서술자들이 줄줄이 이어진 형태** 이고, 각 서술자는 앞머리에
 * 자기 길이를 담고 있다. 그래서 순회는 "지금 서술자의 길이만큼 건너뛴다" 의
 * 반복이 된다.
 *   - 맨 앞은 "aPCI" 라는 서명이 든 머리 서술자.
 *   - 그 뒤로 종류(type)가 붙은 서술자들이 이어지며, 종류 0x82 가
 *     슬롯 서술자다.
 * union apci_descriptor 가 이 셋(머리·슬롯·일반)을 한 자리에 겹쳐 두어,
 * 같은 바이트를 세 가지 눈으로 볼 수 있게 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 모듈이 올라올 때:
 *   module_init -> ibm_acpiphp_init()
 *     -> ACPI 이름 공간 전체를 훑어 IBM37D0/IBM37D4 장치를 찾는다
 *       -> acpiphp 코어에 LED 콜백 표를 등록한다
 *         -> 그 장치의 ACPI 알림 처리기를 건다
 *           -> aPCI 표 크기를 재어 sysfs 이진 파일을 만든다
 *
 * 사용자가 LED 를 켤 때:
 *   sysfs(슬롯의 attention) -> 핫플러그 공용 코어 -> acpiphp
 *     -> acpiphp_attention_info.set_attn == [이 파일] ibm_set_attention_status()
 *        -> ibm_slot_from_id() 로 표를 읽어 슬롯 ID 를 얻고
 *           -> ACPI 의 APLS 메서드를 부른다
 *
 * 사용자가 표를 읽을 때:
 *   /sys/bus/pci/slots/apci_table 읽기 -> [이 파일] ibm_read_apci_table()
 *     -> ibm_get_table_from_acpi()
 *
 * 하드웨어 이벤트가 올 때:
 *   ACPI -> [이 파일] ibm_handle_events()
 *     -> 두 번에 나눠 오는 알림을 하나로 합쳐 netlink 이벤트로 올린다
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. ACPI 메서드 평가와 메모리
 * 할당이 곳곳에 있어 잠들 수 있어야 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 사용자 공간(sysfs)과 netlink 이벤트 소비자.
 * 옆쪽: 같은 디렉터리의 acpiphp — acpiphp_register_attention()/
 *   unregister_attention() 으로 콜백 표를 주고받고, to_slot() 매크로로
 *   핫플러그 슬롯에서 IBM 슬롯 번호(sun)를 꺼낸다.
 * 아래쪽: ACPI 서브시스템(acpi_evaluate_object, acpi_walk_namespace,
 *   알림 처리기). 그 구현은 이 트리에 없다.
 * 또한 drivers/pci/pci.h 의 pci_slots_kset 을 써서 sysfs 파일을 만든다.
 *
 * 데이터 흐름:
 *   ACPI 펌웨어의 APCI 메서드 -> 패키지(버퍼 여러 개)
 *     -> 이어 붙인 한 덩어리 -> 슬롯 서술자 -> LED 상태 비트
 *   반대 방향은 APLS 메서드에 슬롯 ID 와 0/1 을 넘기는 것뿐이다.
 *
 * 공유 상태: 전역 셋 — ACPI 핸들, 알림 문맥, sysfs 속성. 잠금이 하나도
 *   없는데, 알림 문맥의 직렬화는 코드가 아니라 **ACPI 규격의 보장** 에
 *   기댄다(ibm_handle_events 의 상류 주석 참조).
 *
 * === 이 파일을 읽을 때 유의할 점 ===
 * 2004년에 쓰인 코드이며, 지금 기준으로는 방어적이지 않은 자리가 여럿
 * 있다. 각 함수의 [상류 코드 관찰]에 그것들을 적어 두었다.
 *
 * === 주요 함수/구조체 요약 ===
 * ibm_slot_from_id()         : 표를 읽어 슬롯 서술자 하나를 복사해 준다.
 *                              LED 를 켜든 읽든 매번 이것을 지난다.
 * ibm_get_table_from_acpi()  : ACPI 패키지를 한 덩어리로 이어 붙인다.
 *                              버퍼를 NULL 로 주면 크기만 잰다.
 * ibm_set_attention_status() : APLS 메서드로 LED 를 켜고 끈다.
 * ibm_get_attention_status() : 표에서 상태 비트를 읽는다. ACPI 메서드가
 *                              없어 이렇게 우회한다.
 * ibm_handle_events()        : 두 번에 나눠 오는 알림을 하나로 합친다.
 * ibm_find_acpi_device()     : 이름 공간을 훑어 IBM 장치를 찾는 콜백.
 * union apci_descriptor      : 같은 바이트를 세 가지 눈으로 보는 공용체.
 * struct notification        : 알림 두 번 사이에 정보를 나르는 자리.
 */

#define pr_fmt(fmt) "acpiphp_ibm: " fmt

/* [한국어] __init/__exit 표시 — 모듈 진입·해제 함수에 붙는다. */
#include <linux/init.h>
/* [한국어] kzalloc/kfree/kmalloc_obj — 표와 서술자를 할당한다. */
#include <linux/slab.h>
/* [한국어] MODULE_ 계열 선언과 module_init/module_exit. */
#include <linux/module.h>
/* [한국어] 기본 커널 매크로들. */
#include <linux/kernel.h>
/* [한국어] sysfs_create_bin_file 등 — aPCI 표를 sysfs 로 내보낸다. */
#include <linux/sysfs.h>
/* [한국어] struct kobject — sysfs 파일이 붙을 자리를 가리킨다. */
#include <linux/kobject.h>
/* [한국어] [상류 코드 관찰] module_param 계열을 쓰는 곳을 이 파일에서 찾지 못했다.
 * 이 모듈에는 조정 가능한 매개변수가 없다. */
#include <linux/moduleparam.h>
/* [한국어] PCI 관련 정의. 핫플러그 슬롯이 PCI 장치에 딸려 있기 때문이다. */
#include <linux/pci.h>
/* [한국어] [상류 코드 관찰] copy_to_user 등 이 헤더의 이름을 쓰는 곳을 찾지 못했다.
 * sysfs 이진 속성은 커널 버퍼를 채우고 복사는 sysfs 계층이 맡는다. */
#include <linux/uaccess.h>

/* [한국어] 같은 디렉터리의 acpiphp 헤더. **acpiphp_attention_info 와
 * acpiphp_register_attention()이 여기 있어**, 이 파일이 코어에 콜백을
 * 꽂는 통로가 된다. to_slot() 도 여기서 온다. */
#include "acpiphp.h"
/* [한국어] PCI 서브시스템 내부 헤더. pci_slots_kset 이 여기 있어 sysfs 파일을
 * 슬롯 디렉터리 아래에 만든다. */
#include "../pci.h"

/* [한국어] 모듈 버전 문자열. modinfo 에 나타난다. */
#define DRIVER_VERSION	"1.0.1"
/* [한국어] 작성자 표시. 이 모듈의 원저자 둘이다. */
#define DRIVER_AUTHOR	"Irene Zubarev <zubarev@us.ibm.com>, Vernon Mauery <vernux@us.ibm.com>"
/* [한국어] 모듈 설명. 이 파일이 acpiphp 의 **확장** 임을 이름으로 밝힌다. */
#define DRIVER_DESC	"ACPI Hot Plug PCI Controller Driver IBM extension"


MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);

/* [한국어] 이름 공간 순회를 멈추게 하는 값. 아스키로 읽으면 **"aPCI"** 이며,
 * 표 앞머리의 서명과 같은 글자다. 흔한 오류 코드와 겹치지 않으면서
 * 뜻이 통하도록 고른 값이다. */
#define FOUND_APCI 0x61504349
/* these are the names for the IBM ACPI pseudo-device */
/* [한국어] IBM 의 ACPI 유사 장치 하드웨어 ID 하나. */
#define IBM_HARDWARE_ID1 "IBM37D0"
/* [한국어] 그 다른 판. 둘 중 하나만 맞으면 이 모듈이 붙는다. */
#define IBM_HARDWARE_ID2 "IBM37D4"

/* [한국어] 공용 핫플러그 슬롯에서 IBM 이 쓰는 슬롯 번호(sun)를 꺼낸다.
 * to_slot() 은 acpiphp.h 소관이며, 공용 구조체에서 acpiphp 의 슬롯
 * 구조체로 되돌리는 매크로다. */
#define hpslot_to_sun(A) (to_slot(A)->sun)

/* union apci_descriptor - allows access to the
 * various device descriptors that are embedded in the
 * aPCI table
 */
union apci_descriptor {
	struct {
		/* [한국어] 표 앞머리의 서명 네 글자 — "aPCI" 여야 한다.
		 * 설정자: ACPI 펌웨어가 표에 넣어 둔다.
		 * 읽는 자: ibm_slot_from_id() 가 표를 신뢰할지 판단할 때.
		 * 값 범위: 문자 넷. 널 종료가 없어 memcmp 로 비교한다.
		 * 동기화: 표는 매번 새로 읽어 쓰고 버리므로 공유되지 않는다. */
		char sig[4];
		/* [한국어] 머리 서술자의 길이.
		 * 설정자: 펌웨어.
		 * 읽는 자: ibm_slot_from_id() 가 **첫 걸음의 폭** 으로 쓴다.
		 * 값 범위: 펌웨어가 정한다. 검사하지 않는다.
		 * 동기화: 필요 없다. */
		u8   len;
	} header;
	/* [한국어] 슬롯 서술자의 눈. 종류 0x82 인 서술자를 이 모양으로 읽는다. */
	struct {
		/* [한국어] 서술자 종류. **0x82 가 슬롯 서술자** 다.
		 * 설정자: 펌웨어.
		 * 읽는 자: ibm_slot_from_id() 의 순회 조건.
		 * 값 범위: 여러 종류가 있으나 이 파일은 0x82 만 알아본다.
		 * 동기화: 필요 없다. */
		u8  type;
		/* [한국어] 이 서술자의 길이.
		 * 설정자: 펌웨어.
		 * 읽는 자: 순회의 걸음 폭. 아래 generic.len 과 **같은 자리** 이며,
		 * 그것이 공용체를 쓰는 이유다 — 종류를 모르는 채로도 길이를 읽을 수 있다.
		 * 값 범위: 펌웨어가 정한다. 0 이면 순회가 무한 루프가 된다.
		 * 동기화: 필요 없다. */
		u8  len;
		/* [한국어] aPCI 가 쓰는 슬롯 ID. **리눅스의 슬롯 번호와 다른 값** 이다.
		 * 설정자: 펌웨어.
		 * 읽는 자: ibm_set_attention_status() 가 APLS 메서드의 첫 인자로 넘긴다.
		 * 값 범위: 16비트. 이 파일이 표를 읽는 주된 이유가 이 값을 얻기 위해서다.
		 * 동기화: 필요 없다. */
		u16 slot_id;
		/* [한국어] 이 슬롯이 달린 버스 번호.
		 * 설정자: 펌웨어.
		 * 읽는 자: [상류 코드 관찰] 이 파일에서 읽는 곳이 없다.
		 * 값 범위: PCI 버스 번호.
		 * 동기화: 필요 없다. */
		u8  bus_id;
		/* [한국어] 이 슬롯의 장치 번호.
		 * 설정자: 펌웨어.
		 * 읽는 자: [상류 코드 관찰] 이 파일에서 읽는 곳이 없다.
		 * 값 범위: PCI 장치 번호.
		 * 동기화: 필요 없다. */
		u8  dev_num;
		/* [한국어] **리눅스가 쓰는 슬롯 번호.** 위 slot_id 와 짝을 이루며, 이 둘의
		 * 대응을 얻는 것이 표를 읽는 목적이다.
		 * 설정자: 펌웨어.
		 * 읽는 자: ibm_slot_from_id() 의 순회 조건과 디버그 출력.
		 * 값 범위: hpslot_to_sun() 이 주는 값과 비교된다.
		 * 동기화: 필요 없다. */
		u8  slot_num;
		/* [한국어] 슬롯 속성 두 바이트.
		 * 설정자: 펌웨어.
		 * 읽는 자: [상류 코드 관찰] 이 파일에서 읽는 곳이 없다.
		 * 값 범위: 확인 못 함 — IBM aPCI 표 규격 소관이다.
		 * 동기화: 필요 없다. */
		u8  slot_attr[2];
		/* [한국어] 주의(attention) 상태 바이트.
		 * 설정자: 펌웨어가 갱신한다. APLS 메서드로 LED 를 켜면 이 값이 바뀐다.
		 * 읽는 자: ibm_get_attention_status() 가 0xa0 마스크로 두 비트를 본다.
		 * 값 범위: 그 두 비트의 뜻은 확인 못 함.
		 * 동기화: 매번 새로 읽으므로 스냅숏이다. */
		u8  attn;
		/* [한국어] 상태 두 바이트.
		 * 설정자: 펌웨어.
		 * 읽는 자: ibm_get_attention_status() 가 **둘째 바이트의 0x08 비트만** 본다.
		 * 첫째 바이트는 읽지 않는다.
		 * 값 범위: 그 비트의 뜻은 확인 못 함.
		 * 동기화: 매번 새로 읽으므로 스냅숏이다. */
		u8  status[2];
		/* [한국어] 이 서술자의 sun 값.
		 * 설정자: 펌웨어.
		 * 읽는 자: [상류 코드 관찰] 이 파일에서 읽는 곳이 없다. 리눅스 쪽 sun 은
		 * hpslot_to_sun() 으로 얻고 위 slot_num 과 비교하므로, 이 필드는 쓰이지 않는다.
		 * 값 범위: 확인 못 함.
		 * 동기화: 필요 없다. */
		u8  sun;
		/* [한국어] 예약 세 바이트. 서술자 크기를 맞추는 자리로 보인다.
		 * 설정자: 펌웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 정의되지 않았다.
		 * 동기화: 필요 없다. */
		u8  res[3];
	} slot;
	/* [한국어] 종류를 모르는 서술자를 볼 때 쓰는 최소한의 눈. */
	struct {
		/* [한국어] 서술자 종류. 위 slot.type 과 같은 자리다.
		 * 설정자: 펌웨어.
		 * 읽는 자: 순회 조건이 0x82 인지 확인할 때.
		 * 값 범위: 여러 종류.
		 * 동기화: 필요 없다. */
		u8 type;
		/* [한국어] 서술자 길이. 위 slot.len 과 같은 자리다.
		 * 설정자: 펌웨어.
		 * 읽는 자: **순회의 걸음 폭.** 종류를 모르는 서술자도 이 값으로 건너뛸 수
		 * 있다는 것이 공용체 설계의 요점이다.
		 * 값 범위: 펌웨어가 정한다.
		 * 동기화: 필요 없다. */
		u8 len;
	} generic;
};

/* struct notification - keeps info about the device
 * that cause the ACPI notification event
 */
struct notification {
	/* [한국어] 알림을 낸 ACPI 장치.
	 * 설정자: ibm_acpiphp_init() 이 한 번 채운다.
	 * 읽는 자: ibm_handle_events() 가 netlink 이벤트에 장치 이름과 종류를
	 * 담을 때.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 초기화 이후 불변. */
	struct acpi_device *device;
	/* [한국어] **첫째 알림에서 받은 사건 ID 를 둘째 알림까지 나르는 자리.**
	 * 설정자·읽는 자: 모두 ibm_handle_events().
	 * 값 범위: 알림 값 한 바이트.
	 * 동기화: **잠금이 없다.** 위 함수의 상류 주석대로 ACPI 규격이 두 알림의
	 * 직렬화를 보장한다는 전제에 기댄다. */
	u8                  event;
};

static int ibm_set_attention_status(struct hotplug_slot *slot, u8 status);
/* [한국어] LED 상태 읽기 콜백 선언. */
static int ibm_get_attention_status(struct hotplug_slot *slot, u8 *status);
/* [한국어] ACPI 알림 처리기 선언. */
static void ibm_handle_events(acpi_handle handle, u32 event, void *context);
/* [한국어] 표 읽기 도우미 선언. */
static int ibm_get_table_from_acpi(char **bufp);
/* [한국어] sysfs 이진 파일 읽기 콜백 선언. */
static ssize_t ibm_read_apci_table(struct file *filp, struct kobject *kobj,
				   const struct bin_attribute *bin_attr,
				   char *buffer, loff_t pos, size_t size);
static acpi_status __init ibm_find_acpi_device(acpi_handle handle,
		u32 lvl, void *context, void **rv);
static int __init ibm_acpiphp_init(void);
/* [한국어] 모듈 해제 함수 선언.
 * **선언이 이렇게 모여 있는 이유** 는 아래 정적 구조체들이 이 함수들을
 * 가리키는데, 그 구조체가 함수 정의보다 먼저 나오기 때문이다. */
static void __exit ibm_acpiphp_exit(void);

/* [한국어] 찾아낸 IBM ACPI 장치의 핸들.
 * 설정자: ibm_find_acpi_device() 가 이름 공간 순회 중에 채운다.
 * 읽는 자: ACPI 메서드를 평가하는 모든 자리와 알림 처리기 등록·해제.
 * 값 범위: 유효한 핸들. 못 찾으면 초기화가 실패해 모듈이 올라오지 않는다.
 * 동기화: 초기화 이후 불변. */
static acpi_handle ibm_acpi_handle;
/* [한국어] 알림 문맥 하나.
 * 설정자·읽는 자: ibm_acpiphp_init() 과 ibm_handle_events().
 * 값 범위: 위 struct notification.
 * 동기화: 없다. ACPI 규격의 직렬화 보장에 기댄다.
 * **전역 하나뿐** 이므로 이 장치가 시스템에 하나라는 전제이기도 하다. */
static struct notification ibm_note;
/* [한국어] aPCI 표를 내보내는 sysfs 이진 속성.
 * 설정자: 대부분 여기서 정하고, **크기만 초기화 함수가 실행 시점에 채운다.**
 * 읽는 자: sysfs 계층.
 * 값 범위: 아래 필드 참조.
 * 동기화: 없다.
 * __ro_after_init 이 붙어 있는데, 크기를 채우는 것이 모듈 초기화 중이라
 * 아직 읽기 전용이 되기 전이다. */
static struct bin_attribute ibm_apci_table_attr __ro_after_init = {
	    /* [한국어] sysfs 속성의 이름과 권한. */
	    .attr = {
		    /* [한국어] 파일 이름. /sys/bus/pci/slots/apci_table 로 나타난다. */
		    .name = "apci_table",
		    /* [한국어] 모두에게 읽기만 허용한다. 아래에서 쓰기 콜백도 두지 않는다. */
		    .mode = S_IRUGO,
	    },
	    /* [한국어] 읽기 콜백. 위에서 선언해 둔 함수를 여기서 가리킨다. */
	    .read = ibm_read_apci_table,
	    /* [한국어] 쓰기 콜백을 명시적으로 NULL 로 둔다. 정적 변수라 어차피 0 이지만,
	     * "쓰기를 지원하지 않는다" 를 눈에 보이게 적은 것이다. */
	    .write = NULL,
};
static struct acpiphp_attention_info ibm_attention_info =
{
	/* [한국어] LED 켜기 콜백. 이 표가 acpiphp 코어에 등록된다. */
	.set_attn = ibm_set_attention_status,
	/* [한국어] LED 읽기 콜백. */
	.get_attn = ibm_get_attention_status,
	.owner = THIS_MODULE,
};

/* [한국어]
 * ibm_slot_from_id - 표를 통째로 읽어 슬롯 서술자 하나를 복사해 준다
 *
 * @id: 리눅스가 이 슬롯을 부르는 번호(sun).
 * @return: 복사된 슬롯 서술자. 다 쓰면 kfree 로 풀어야 한다. 못 찾으면 NULL.
 *
 * **이 파일에서 가장 자주 불리는 함수** 이며, LED 를 켜든 읽든 매번 지난다.
 * 그때마다 ACPI 에서 표를 통째로 읽어 오고, 필요한 서술자 하나만 복사한 뒤
 * 표를 버린다. 위 상류 주석이 "bad ibm hardware 를 위한 우회로" 라고 적은
 * 것이 이 비용을 가리킨다.
 *
 * 네 단계다.
 * 1. 표를 읽어 온다.
 * 2. 맨 앞의 "aPCI" 서명을 확인한다. 아니면 그대로 물러난다.
 * 3. 머리 서술자의 길이만큼 건너뛴 뒤, 종류 0x82(슬롯)이면서 슬롯 번호가
 *    맞는 것이 나올 때까지 **각 서술자의 길이만큼 건너뛰며** 훑는다.
 * 4. 찾았으면 새로 할당해 복사한다.
 *
 * 3번의 순회가 이 파일 특유의 관용이다. 서술자가 저마다 자기 길이를 앞머리에
 * 담고 있어서, 배열처럼 첨자로 접근할 수 없고 길이를 따라가야 한다.
 * `ind += des->generic.len` 이 그 걸음이다.
 *
 * 조건의 순서도 눈여겨볼 만하다 — 종류를 먼저 보고 슬롯 번호를 나중에 본다.
 * 단락 평가 덕분에, 슬롯 서술자가 아닌 것에서는 슬롯 번호 자리를 읽지 않는다.
 *
 * [상류 코드 관찰] 세 가지.
 *  1) 서술자의 길이가 0 이면 3번의 순회가 제자리에 머물러 **무한 루프** 가
 *     된다. 펌웨어가 준 값을 검사 없이 걸음 폭으로 쓴다.
 *  2) 복사가 언제나 공용체 전체 크기만큼 이뤄진다. 표의 마지막 서술자가
 *     그보다 짧으면 표 끝을 넘어 읽는다.
 *  3) `ret` 에 새 할당을 덮어쓴 뒤 `des` 에서 복사하는데, 그 시점에 둘이
 *     같은 곳을 가리키므로 결과는 맞다. 다만 읽는 사람이 헷갈리기 쉬운 배치다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: sysfs 를 통한 LED 조작. 프로세스 컨텍스트이며 ACPI 평가와
 * 메모리 할당으로 잠들 수 있다.
 *
 * 에러 경로: 표를 못 읽거나 서명이 다르거나 슬롯을 못 찾으면 NULL.
 * 할당이 실패해도 NULL 이며, 어느 쪽이든 표는 반드시 풀고 나간다.
 *
 * 호출 체인:
 *   ibm_set_attention_status() / ibm_get_attention_status() → [이 함수]
 *     → ibm_get_table_from_acpi() → kmalloc_obj() → kfree()
 */
/**
 * ibm_slot_from_id - workaround for bad ibm hardware
 * @id: the slot number that linux refers to the slot by
 *
 * Description: This method returns the aCPI slot descriptor
 * corresponding to the Linux slot number.  This descriptor
 * has info about the aPCI slot id and attention status.
 * This descriptor must be freed using kfree when done.
 */
/* [한국어]
 * ibm_slot_from_id - 표를 통째로 읽어 슬롯 서술자 하나를 복사해 준다
 *
 * @id: 리눅스가 이 슬롯을 부르는 번호(sun).
 * @return: 복사된 슬롯 서술자. 다 쓰면 kfree 로 풀어야 한다. 못 찾으면 NULL.
 *
 * **이 파일에서 가장 자주 불리는 함수** 이며, LED 를 켜든 읽든 매번 지난다.
 * 그때마다 ACPI 에서 표를 통째로 읽어 오고, 필요한 서술자 하나만 복사한 뒤
 * 표를 버린다. 위 상류 주석이 "bad ibm hardware 를 위한 우회로" 라고 적은
 * 것이 이 비용을 가리킨다.
 *
 * 네 단계다.
 * 1. 표를 읽어 온다.
 * 2. 맨 앞의 "aPCI" 서명을 확인한다. 아니면 그대로 물러난다.
 * 3. 머리 서술자의 길이만큼 건너뛴 뒤, 종류 0x82(슬롯)이면서 슬롯 번호가
 *    맞는 것이 나올 때까지 **각 서술자의 길이만큼 건너뛰며** 훑는다.
 * 4. 찾았으면 새로 할당해 복사한다.
 *
 * 3번의 순회가 이 파일 특유의 관용이다. 서술자가 저마다 자기 길이를 앞머리에
 * 담고 있어서, 배열처럼 첨자로 접근할 수 없고 길이를 따라가야 한다.
 * `ind += des->generic.len` 이 그 걸음이다.
 *
 * 조건의 순서도 눈여겨볼 만하다 — 종류를 먼저 보고 슬롯 번호를 나중에 본다.
 * 단락 평가 덕분에, 슬롯 서술자가 아닌 것에서는 슬롯 번호 자리를 읽지 않는다.
 *
 * [상류 코드 관찰] 세 가지.
 *  1) 서술자의 길이가 0 이면 3번의 순회가 제자리에 머물러 **무한 루프** 가
 *     된다. 펌웨어가 준 값을 검사 없이 걸음 폭으로 쓴다.
 *  2) 복사가 언제나 공용체 전체 크기만큼 이뤄진다. 표의 마지막 서술자가
 *     그보다 짧으면 표 끝을 넘어 읽는다.
 *  3) `ret` 에 새 할당을 덮어쓴 뒤 `des` 에서 복사하는데, 그 시점에 둘이
 *     같은 곳을 가리키므로 결과는 맞다. 다만 읽는 사람이 헷갈리기 쉬운 배치다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: sysfs 를 통한 LED 조작. 프로세스 컨텍스트이며 ACPI 평가와
 * 메모리 할당으로 잠들 수 있다.
 *
 * 에러 경로: 표를 못 읽거나 서명이 다르거나 슬롯을 못 찾으면 NULL.
 * 할당이 실패해도 NULL 이며, 어느 쪽이든 표는 반드시 풀고 나간다.
 *
 * 호출 체인:
 *   ibm_set_attention_status() / ibm_get_attention_status() → [이 함수]
 *     → ibm_get_table_from_acpi() → kmalloc_obj() → kfree()
 */
static union apci_descriptor *ibm_slot_from_id(int id)
{
	int ind = 0, size;
	union apci_descriptor *ret = NULL, *des;
	/* [한국어] 표를 담을 자리. */
	char *table;

	size = ibm_get_table_from_acpi(&table);
	/* [한국어] 표를 못 읽었다. */
	if (size < 0)
		/* [한국어] 슬롯을 찾을 수 없으므로 NULL 을 돌려준다. */
		return NULL;
	des = (union apci_descriptor *)table;
	/* [한국어] 표 앞머리의 서명을 확인한다. **널 종료가 없어 memcmp 로 정확히
	 * 네 바이트만 비교한다.** */
	if (memcmp(des->header.sig, "aPCI", 4) != 0)
		/* [한국어] 서명이 다르면 표를 신뢰할 수 없다. 표를 풀고 나가는 자리로 간다. */
		goto ibm_slot_done;

	des = (union apci_descriptor *)&table[ind += des->header.len];
	/* [한국어] 머리 서술자를 건너뛴 자리에서 시작해, 슬롯 서술자이면서 번호가
	 * 맞는 것을 찾을 때까지 훑는다. 종류를 먼저 보므로, 슬롯 서술자가 아닌
	 * 것에서는 슬롯 번호 자리를 읽지 않는다. */
	while (ind < size && (des->generic.type != 0x82 ||
			des->slot.slot_num != id)) {
		des = (union apci_descriptor *)&table[ind += des->generic.len];
	}

	if (ind < size && des->slot.slot_num == id)
		/* [한국어] 찾았다. 그 자리를 가리켜 둔다. */
		ret = des;

ibm_slot_done:
	if (ret) {
		ret = kmalloc_obj(union apci_descriptor);
		/* [한국어] 할당에 성공했으면 복사한다. **des 에서 복사하는데**, 이 시점에
		 * ret 이 방금 덮어써졌으므로 옛 ret 이 아니라 des 를 쓰는 것이 맞다 —
		 * 둘이 같은 곳을 가리키고 있었기 때문이다. */
		if (ret)
			/* [한국어] 공용체 **전체 크기** 만큼 복사한다. 표의 마지막 서술자가 그보다
			 * 짧으면 표 끝을 넘어 읽는다. */
			memcpy(ret, des, sizeof(union apci_descriptor));
	}
	kfree(table);
	return ret;
}

/* [한국어]
 * ibm_set_attention_status - APLS 메서드로 주의 LED 를 켜고 끈다
 *
 * @slot: 공용 핫플러그 슬롯.
 * @status: 0 = 끄기, 그 밖 = 켜기.
 * @return: 0 = 성공, -ENODEV = 슬롯을 못 찾거나 메서드 평가 실패,
 *          -ERANGE = 메서드가 실패를 알림.
 *
 * acpiphp 코어에 등록되는 콜백 둘 중 하나다.
 *
 * **표를 읽는 이유가 상태를 알기 위해서가 아니다.** APLS 메서드가 요구하는
 * 것은 리눅스의 슬롯 번호가 아니라 aPCI 의 슬롯 ID 인데, 그 둘의 대응이
 * 표에만 있다. 그래서 켜기만 하는 이 함수도 표를 한 벌 읽는다.
 *
 * 인자 둘을 ACPI 객체로 만들어 넘긴다 — 슬롯 ID 와, 0 또는 1 로 정규화한
 * 상태다. 어떤 0 아닌 값이 와도 1 로 접히므로 호출자가 신경 쓸 것이 없다.
 *
 * **서술자를 메서드 평가 전에 미리 푼다.** 필요한 값을 이미 인자 배열에
 * 옮겨 담았기 때문이며, 평가가 오래 걸리는 동안 메모리를 쥐고 있지 않게
 * 하려는 배치로 보인다.
 *
 * 반환값 판정이 두 겹이다. ACPI 평가 자체의 실패와, 평가는 됐지만 메서드가
 * 0 을 돌려준 경우를 나눠 각각 다른 오류로 보고한다. 후자에 -ERANGE 를
 * 쓰는 것이 다소 뜻밖인데, 그 선택의 근거는 코드에 적혀 있지 않다.
 *
 * 실행 컨텍스트: sysfs 쓰기. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 위 셋. 어느 경우에도 서술자는 이미 풀린 뒤다.
 *
 * 호출 체인:
 *   sysfs → 핫플러그 공용 코어 → acpiphp
 *     → acpiphp_attention_info.set_attn == [이 함수]
 *     → ibm_slot_from_id() → kfree() → acpi_evaluate_integer("APLS")
 */
/**
 * ibm_set_attention_status - callback method to set the attention LED
 * @slot: the hotplug_slot to work with
 * @status: what to set the LED to (0 or 1)
 *
 * Description: This method is registered with the acpiphp module as a
 * callback to do the device specific task of setting the LED status.
 */
static int ibm_set_attention_status(struct hotplug_slot *slot, u8 status)
{
	union acpi_object args[2];
	struct acpi_object_list params = { .pointer = args, .count = 2 };
	/* [한국어] ACPI 평가의 성패. */
	acpi_status stat;
	/* [한국어] APLS 메서드가 돌려주는 값. */
	unsigned long long rc;
	/* [한국어] 표에서 찾아 온 슬롯 서술자. */
	union apci_descriptor *ibm_slot;
	/* [한국어] 리눅스 슬롯 번호. 매크로가 공용 구조체에서 꺼내 준다. */
	int id = hpslot_to_sun(slot);

	ibm_slot = ibm_slot_from_id(id);
	/* [한국어] 그 번호의 슬롯 서술자를 표에서 찾지 못했다. */
	if (!ibm_slot) {
		/* [한국어] 실패를 알린다. */
		pr_err("APLS null ACPI descriptor for slot %d\n", id);
		/* [한국어] 장치 없음으로 답한다. */
		return -ENODEV;
	}

	pr_debug("%s: set slot %d (%d) attention status to %d\n", __func__,
			/* [한국어] 디버그 출력에 리눅스 번호와 aPCI ID 를 함께 찍어 대응을 보여 준다. */
			ibm_slot->slot.slot_num, ibm_slot->slot.slot_id,
			(status ? 1 : 0));

	args[0].type = ACPI_TYPE_INTEGER;
	/* [한국어] **aPCI 슬롯 ID** 를 넘긴다. 리눅스 번호가 아니라는 것이 표를 읽어야
	 * 했던 이유다. */
	args[0].integer.value = ibm_slot->slot.slot_id;
	/* [한국어] 둘째 인자도 정수다. */
	args[1].type = ACPI_TYPE_INTEGER;
	/* [한국어] 0 아닌 값은 모두 1 로 접는다. 호출자가 어떤 값을 주든 메서드에는
	 * 0 이나 1 만 간다. */
	args[1].integer.value = (status) ? 1 : 0;

	kfree(ibm_slot);

	stat = acpi_evaluate_integer(ibm_acpi_handle, "APLS", &params, &rc);
	/* [한국어] ACPI 평가 자체가 실패했다. 메서드가 없거나 인자가 맞지 않는 경우다. */
	if (ACPI_FAILURE(stat)) {
		/* [한국어] 실패를 알린다. */
		pr_err("APLS evaluation failed:  0x%08x\n", stat);
		/* [한국어] 장치 없음으로 답한다. */
		return -ENODEV;
	} else if (!rc) {
		/* [한국어] 평가는 됐지만 메서드가 0 을 돌려줬다 — 하드웨어가 요청을 거절한
		 * 경우로 보인다. */
		pr_err("APLS method failed:  0x%08llx\n", rc);
		/* [한국어] 위와 구별되도록 다른 오류로 답한다. */
		return -ERANGE;
	}
	return 0;
}

/* [한국어]
 * ibm_get_attention_status - 표에서 LED 상태 비트를 읽어 답한다
 *
 * @slot: 공용 핫플러그 슬롯.
 * @status: 결과를 담을 자리. 0 또는 1.
 * @return: 0 = 성공, -ENODEV = 슬롯을 못 찾음.
 *
 * 위 상류 주석이 이 함수가 존재하는 방식의 이유를 밝힌다 — **LED 상태를
 * 직접 얻는 ACPI 메서드가 없어서**, 표를 읽어 자기 슬롯 서술자를 찾아
 * 그 안의 상태 비트를 보는 수밖에 없다.
 *
 * 판정이 두 자리를 **또는** 으로 묶는다.
 *   - 주의 필드의 두 비트(0xa0 마스크)
 *   - 상태 필드 둘째 바이트의 한 비트(0x08)
 * 둘 중 하나라도 서 있으면 켜진 것으로 본다. 각 비트가 정확히 무엇을
 * 뜻하는지, 왜 세 비트가 같은 결론으로 모이는지는 이 트리에서 확인 못 함 —
 * IBM 의 aPCI 표 규격 소관이다.
 *
 * 결과를 0 이나 1 로 정규화한다. 공용 코어가 그 두 값만 기대하기 때문이다.
 *
 * 실행 컨텍스트: sysfs 읽기. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 슬롯을 못 찾으면 -ENODEV 이며, 그 경우 status 는 손대지 않는다.
 *
 * 호출 체인:
 *   sysfs → 핫플러그 공용 코어 → acpiphp
 *     → acpiphp_attention_info.get_attn == [이 함수]
 *     → ibm_slot_from_id() → kfree()
 */
/**
 * ibm_get_attention_status - callback method to get attention LED status
 * @slot: the hotplug_slot to work with
 * @status: returns what the LED is set to (0 or 1)
 *
 * Description: This method is registered with the acpiphp module as a
 * callback to do the device specific task of getting the LED status.
 *
 * Because there is no direct method of getting the LED status directly
 * from an ACPI call, we read the aPCI table and parse out our
 * slot descriptor to read the status from that.
 */
static int ibm_get_attention_status(struct hotplug_slot *slot, u8 *status)
{
	union apci_descriptor *ibm_slot;
	int id = hpslot_to_sun(slot);

	ibm_slot = ibm_slot_from_id(id);
	/* [한국어] 그 번호의 슬롯 서술자를 찾지 못했다. */
	if (!ibm_slot) {
		/* [한국어] 실패를 알린다. */
		pr_err("APLS null ACPI descriptor for slot %d\n", id);
		/* [한국어] 장치 없음으로 답한다. */
		return -ENODEV;
	}

	if (ibm_slot->slot.attn & 0xa0 || ibm_slot->slot.status[1] & 0x08)
		*status = 1;
	else
		*status = 0;

	pr_debug("%s: get slot %d (%d) attention status is %d\n", __func__,
			/* [한국어] 여기서도 두 번호를 함께 찍어 대응을 보여 준다. */
			ibm_slot->slot.slot_num, ibm_slot->slot.slot_id,
			*status);

	kfree(ibm_slot);
	return 0;
}

/* [한국어]
 * ibm_handle_events - 두 번에 나눠 오는 알림을 하나로 합쳐 올린다
 *
 * @handle: 알림을 낸 장치의 ACPI 핸들. 쓰지 않는다.
 * @event: 알림 값. 상위 니블이 종류, 하위 니블이 세부 정보다.
 * @context: 등록 시 넘겨 둔 알림 문맥.
 *
 * 위 상류 주석이 이 함수의 전부를 설명한다. **하드웨어가 한 사건을 알림
 * 두 번으로 나눠 보낸다** — 먼저 사건 ID 를, 그다음 그것을 일으킨 슬롯
 * 번호를 보낸다. 이 함수가 그 둘을 모아 사용자 공간에는 한 번으로 올린다.
 *
 * 알림 값을 상위·하위 니블로 쪼개는 것이 그 구현이다.
 *   - 상위 니블이 0x80 이면: **둘째 알림** 이다. 앞서 보관해 둔 사건 ID 와
 *     지금 온 하위 니블(슬롯 번호)을 묶어 netlink 이벤트로 올린다.
 *   - 그 밖이면: **첫째 알림** 이다. 값을 통째로 보관만 하고 돌아간다.
 *
 * 보관 자리가 전역 구조체 하나뿐인데도 경쟁이 없는 이유를 상류 주석이
 * 밝힌다 — ACPI 2.0 규격 5.6.2.2 절에 따라 OSPM 이 이 함수가 **돌아온 뒤에야**
 * 그 인터럽트를 다시 허용하므로, 두 알림이 겹칠 수 없다. 즉 **직렬화가
 * 코드가 아니라 규격에 의해 보장된다.**
 *
 * [상류 코드 관찰] 그 보장에 기대므로 이 파일에는 관련 잠금이 하나도 없다.
 * 규격 해석이 어긋나거나 다른 경로가 이 문맥을 건드리면 지켜지지 않는
 * 구조인데, 주석이 그 전제를 명시해 두었다는 점은 짚어 둘 만하다.
 *
 * 실행 컨텍스트: ACPI 알림 처리 문맥. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 없다. netlink 이벤트 생성의 결과를 확인하지 않는다.
 *
 * 호출 체인:
 *   ACPI 알림 → [이 함수] → acpi_bus_generate_netlink_event()
 */
/**
 * ibm_handle_events - listens for ACPI events for the IBM37D0 device
 * @handle: an ACPI handle to the device that caused the event
 * @event: the event info (device specific)
 * @context: passed context (our notification struct)
 *
 * Description: This method is registered as a callback with the ACPI
 * subsystem it is called when this device has an event to notify the OS of.
 *
 * The events actually come from the device as two events that get
 * synthesized into one event with data by this function.  The event
 * ID comes first and then the slot number that caused it.  We report
 * this as one event to the OS.
 *
 * From section 5.6.2.2 of the ACPI 2.0 spec, I understand that the OSPM will
 * only re-enable the interrupt that causes this event AFTER this method
 * has returned, thereby enforcing serial access for the notification struct.
 */
static void ibm_handle_events(acpi_handle handle, u32 event, void *context)
{
	u8 detail = event & 0x0f;
	u8 subevent = event & 0xf0;
	/* [한국어] 등록 시 넘겨 둔 알림 문맥. 두 알림 사이의 정보를 나르는 자리다. */
	struct notification *note = context;

	pr_debug("%s: Received notification %02x\n", __func__, event);

	if (subevent == 0x80) {
		/* [한국어] 둘째 알림이므로 이제 사용자 공간에 올릴 수 있다. */
		pr_debug("%s: generating bus event\n", __func__);
		/* [한국어] 장치 종류와 이름, **앞서 보관해 둔 사건 ID**, 그리고 지금 온
		 * 세부 정보(슬롯 번호)를 한 이벤트로 묶어 올린다. */
		acpi_bus_generate_netlink_event(note->device->pnp.device_class,
						  dev_name(&note->device->dev),
						  note->event, detail);
	} else
		/* [한국어] 첫째 알림이다. 값을 보관만 하고 돌아간다 — 다음 알림이 올 때까지
		 * 이 자리가 사건 ID 를 쥐고 있는다. */
		note->event = event;
}

/* [한국어]
 * ibm_get_table_from_acpi - ACPI 패키지를 한 덩어리 표로 이어 붙인다
 *
 * @bufp: 할당한 표를 돌려줄 자리. **NULL 을 주면 크기만 잰다.**
 * @return: 0 이상이면 표의 바이트 수, 음수면 오류.
 *
 * 이 파일의 모든 표 읽기가 지나는 함수다.
 *
 * ACPI 의 APCI 메서드는 버퍼 여러 개로 이뤄진 **패키지** 를 돌려주는데,
 * 서술자를 순회하려면 한 덩어리여야 한다. 그 이어 붙이기가 이 함수의 일이다.
 *
 * **두 가지 모드로 쓰인다** 는 것이 설계의 요점이다.
 *   - 버퍼 자리를 주면: 크기를 잰 뒤 할당하고 내용까지 채운다.
 *   - NULL 을 주면: 크기만 재고 돌아간다. 모듈 초기화가 sysfs 파일 크기를
 *     정할 때 이 모드를 쓴다.
 *
 * 네 단계다.
 * 1. APCI 메서드를 평가해 패키지를 얻는다.
 * 2. 그것이 정말 패키지이고 원소가 있는지 확인한다.
 * 3. 원소를 모두 훑어 **전체 크기를 먼저 계산한다.** 이때 원소가 하나라도
 *    버퍼가 아니면 표 자체가 잘못된 것으로 보고 물러난다.
 * 4. 할당한 뒤 다시 훑으며 이어 붙인다.
 *
 * 3번과 4번으로 나뉜 것은 미리 크기를 알아야 한 번에 할당할 수 있기
 * 때문이다. 그래서 패키지를 두 번 훑는다.
 *
 * `size` 변수가 세 역할을 겸한다 — 처음에는 오류 코드(-EIO), 3번에서는
 * 전체 크기, 4번에서는 채워 넣은 위치다. 그 재사용 덕에 하나의 반환 경로로
 * 모든 경우를 처리한다.
 *
 * [상류 코드 관찰] 두 가지.
 *  1) 패키지가 비어 있어 크기가 0 이면 `kzalloc(0)` 이 되는데, 그것은
 *     NULL 이 아닌 특수한 값을 돌려준다. 그 값이 호출자에게 표로 전달되고
 *     호출자는 0 을 오류로 보지 않으므로, 그 포인터를 그대로 읽게 된다.
 *  2) 할당 직후의 디버그 출력이 NULL 검사보다 **앞** 에 있어, 할당이
 *     실패한 경우 NULL 을 찍는다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: sysfs 경로와 모듈 초기화. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 메서드 평가 실패는 -ENODEV(이때는 버퍼도 풀지 않고 곧바로
 * 돌아간다), 패키지가 잘못됐으면 -EIO, 할당 실패는 -ENOMEM 이다.
 * 그 밖의 모든 경로는 ACPI 버퍼를 풀고 나간다.
 *
 * 호출 체인:
 *   ibm_slot_from_id() / ibm_read_apci_table() / ibm_acpiphp_init()
 *     → [이 함수] → acpi_evaluate_object("APCI") → kzalloc() → memcpy()
 */
/**
 * ibm_get_table_from_acpi - reads the APLS buffer from ACPI
 * @bufp: address to pointer to allocate for the table
 *
 * Description: This method reads the APLS buffer in from ACPI and
 * stores the "stripped" table into a single buffer
 * it allocates and passes the address back in bufp.
 *
 * If NULL is passed in as buffer, this method only calculates
 * the size of the table and returns that without filling
 * in the buffer.
 *
 * Returns < 0 on error or the size of the table on success.
 */
static int ibm_get_table_from_acpi(char **bufp)
{
	union acpi_object *package;
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	/* [한국어] ACPI 평가의 성패. */
	acpi_status status;
	/* [한국어] 이어 붙일 버퍼. 실패 경로에서 검사하므로 NULL 로 시작한다. */
	char *lbuf = NULL;
	/* [한국어] 루프 첨자와, **세 역할을 겸하는 크기 변수.** 처음에는 오류 코드,
	 * 그다음 전체 크기, 마지막에는 채워 넣은 위치가 된다. */
	int i, size = -EIO;

	status = acpi_evaluate_object(ibm_acpi_handle, "APCI", NULL, &buffer);
	/* [한국어] APCI 메서드를 평가하지 못했다. */
	if (ACPI_FAILURE(status)) {
		/* [한국어] 실패를 알린다. */
		pr_err("%s:  APCI evaluation failed\n", __func__);
		/* [한국어] 장치 없음으로 답한다. **이 갈래만 ACPI 버퍼를 풀지 않는데**,
		 * 평가가 실패했으면 버퍼가 할당되지 않았기 때문이다. */
		return -ENODEV;
	}

	package = (union acpi_object *) buffer.pointer;
	/* [한국어] 돌려받은 것이 정말 패키지이고 원소가 있는지 확인한다. */
	if (!(package) ||
			(package->type != ACPI_TYPE_PACKAGE) ||
			!(package->package.elements)) {
		pr_err("%s:  Invalid APCI object\n", __func__);
		/* [한국어] 잘못된 표다. size 가 초기값(-EIO)인 채로 나간다. */
		goto read_table_done;
	}

	for (size = 0, i = 0; i < package->package.count; i++) {
		/* [한국어] 원소 하나라도 버퍼가 아니면 표 형식이 깨진 것이다. */
		if (package->package.elements[i].type != ACPI_TYPE_BUFFER) {
			/* [한국어] 몇 번째 원소가 문제인지 알린다. */
			pr_err("%s:  Invalid APCI element %d\n", __func__, i);
			/* [한국어] [상류 코드 관찰] 여기서 빠져나가면 **오류가 아니라 그때까지 더한
			 * 부분 크기가 반환된다.** 루프 시작에서 size 를 0 으로 다시 두었기 때문에
			 * 초기값 -EIO 가 이미 덮인 상태다. 호출자는 size < 0 만 실패로 보므로
			 * 잘린 표를 정상으로 받아들이게 된다. 원본(1f0e418bb6)에서 확인했으며
			 * 코드는 고치지 않았다. */
			goto read_table_done;
		}
		size += package->package.elements[i].buffer.length;
	}

	if (bufp == NULL)
		/* [한국어] 크기만 재는 모드다. 여기까지 계산한 크기를 그대로 돌려준다 —
		 * 모듈 초기화가 sysfs 파일 크기를 정할 때 이 경로를 쓴다. */
		goto read_table_done;

	lbuf = kzalloc(size, GFP_KERNEL);
	/* [한국어] [상류 코드 관찰] 이 디버그 출력이 아래 NULL 검사보다 **앞** 에 있어,
	 * 할당이 실패한 경우 NULL 을 찍는다. */
	pr_debug("%s: element count: %i, ASL table size: %i, &table = 0x%p\n",
			__func__, package->package.count, size, lbuf);

	if (lbuf) {
		*bufp = lbuf;
	} else {
		size = -ENOMEM;
		/* [한국어] 메모리 부족으로 나간다. */
		goto read_table_done;
	}

	size = 0;
	/* [한국어] 이제 다시 훑으며 이어 붙인다. 크기를 이미 알아 한 번에 할당했다. */
	for (i = 0; i < package->package.count; i++) {
		/* [한국어] 지금까지 채운 위치부터 이 원소의 내용을 이어 붙인다. */
		memcpy(&lbuf[size],
				package->package.elements[i].buffer.pointer,
				package->package.elements[i].buffer.length);
		/* [한국어] 채운 만큼 위치를 옮긴다. 마지막에는 이 값이 전체 크기가 된다. */
		size += package->package.elements[i].buffer.length;
	}

/* [한국어] 모든 경로가 여기로 모여 ACPI 버퍼를 풀고 나간다. */
read_table_done:
	/* [한국어] ACPI 가 할당한 버퍼를 푼다. 이어 붙인 표는 호출자가 푼다. */
	kfree(buffer.pointer);
	return size;
}

/* [한국어]
 * ibm_read_apci_table - sysfs 의 apci_table 파일을 읽을 때 표를 넘겨준다
 *
 * @filp: 열린 sysfs 파일. 쓰지 않는다.
 * @kobj: 이 속성이 붙은 kobject. 쓰지 않는다.
 * @bin_attr: 이 이진 속성. 쓰지 않는다.
 * @buffer: 채울 커널 버퍼.
 * @pos: 파일 안의 위치.
 * @size: 요청된 바이트 수.
 * @return: 읽은 바이트 수, 또는 음수 오류.
 *
 * 위 상류 주석이 이 함수의 제약을 밝힌다 — 이 파일에는 열기·닫기 알림이
 * 오지 않아 읽는 동안 표를 붙들어 둘 수가 없다. 그래서 **한 번에 전부
 * 읽는 것만 허용한다.**
 *
 * 그 구현이 위치 검사다. 위치가 0 이 아니면 이어 읽기이므로 -EINVAL 로
 * 거절한다. 위치가 0 일 때만 표를 새로 읽어 넘긴다.
 *
 * 매번 ACPI 에서 다시 읽으므로, 읽을 때마다 최신 상태가 반영된다.
 *
 * [상류 코드 관찰] 표가 요청된 크기보다 크면 복사를 건너뛰는데,
 * **반환값은 그대로 표의 크기다.** 즉 아무것도 채우지 않고 "이만큼 읽었다"
 * 고 답한다. 사용자 공간은 채워지지 않은 버퍼를 유효한 내용으로 받게 된다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: sysfs 읽기. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 이어 읽기는 -EINVAL, 표 읽기 실패는 그 오류가 그대로 나간다.
 * 어느 쪽이든 표는 풀고 나간다(NULL 을 kfree 하는 것은 안전하다).
 *
 * 호출 체인:
 *   사용자 공간의 read() → sysfs → bin_attribute.read == [이 함수]
 *     → ibm_get_table_from_acpi() → memcpy() → kfree()
 */
/**
 * ibm_read_apci_table - callback for the sysfs apci_table file
 * @filp: the open sysfs file
 * @kobj: the kobject this binary attribute is a part of
 * @bin_attr: struct bin_attribute for this file
 * @buffer: the kernel space buffer to fill
 * @pos: the offset into the file
 * @size: the number of bytes requested
 *
 * Description: Gets registered with sysfs as the reader callback
 * to be executed when /sys/bus/pci/slots/apci_table gets read.
 *
 * Since we don't get notified on open and close for this file,
 * things get really tricky here...
 * our solution is to only allow reading the table in all at once.
 */
static ssize_t ibm_read_apci_table(struct file *filp, struct kobject *kobj,
				   const struct bin_attribute *bin_attr,
				   char *buffer, loff_t pos, size_t size)
{
	/* [한국어] 반환값. **기본이 -EINVAL** 이라, 위치가 0 이 아니면 아무것도 하지
	 * 않고 그 값이 그대로 나간다. */
	int bytes_read = -EINVAL;
	/* [한국어] 이어 붙인 표를 담을 자리. 실패 경로에서 풀 수 있게 NULL 로 시작한다. */
	char *table = NULL;

	/* [한국어] 요청된 위치와 크기를 기록에 남긴다. */
	pr_debug("%s: pos = %d, size = %zd\n", __func__, (int)pos, size);

	if (pos == 0) {
		/* [한국어] 위치가 0 일 때만 표를 새로 읽는다. 옆의 상류 주석대로, 열기·닫기
		 * 알림이 없어 표를 붙들어 둘 수 없으므로 한 번에 전부 읽는 것만 허용한다. */
		bytes_read = ibm_get_table_from_acpi(&table);
		/* [한국어] 읽은 표가 요청된 크기 안에 들어갈 때만 복사한다.
		 * **들어가지 않으면 복사를 건너뛰는데 반환값은 그대로 표 크기다.** */
		if (bytes_read > 0 && bytes_read <= size)
			/* [한국어] 커널 버퍼에 채운다. 사용자 공간으로 옮기는 것은 sysfs 계층의 몫이다. */
			memcpy(buffer, table, bytes_read);
		/* [한국어] 표를 푼다. 위치가 0 이 아니어서 읽지 않은 경우에도 이 줄에 닿지
		 * 않으므로, NULL 을 푸는 일은 생기지 않는다. */
		kfree(table);
	}
	return bytes_read;
}

/* [한국어]
 * ibm_find_acpi_device - ACPI 이름 공간을 훑다가 IBM 장치를 찾으면 멈춘다
 *
 * @handle: 지금 살펴보는 장치의 ACPI 핸들.
 * @lvl: 이름 공간 트리의 깊이. 쓰지 않는다.
 * @context: 찾은 핸들을 담을 자리(핸들의 주소).
 * @rv: 원하면 채울 반환 자리. 쓰지 않는다.
 * @return: 0 = 계속 찾아라, FOUND_APCI = 찾았으니 멈춰라.
 *
 * 이름 공간의 장치마다 한 번씩 불리는 콜백이며, 위 상류 주석이 그 규약을
 * 밝힌다 — **0 이 아닌 값을 돌려주면 순회가 멈추고 그 값이 호출자에게
 * 그대로 전달된다.**
 *
 * 판정이 세 조건의 곱이다.
 * 1. 장치가 지금 켜져 있어야 한다.
 * 2. 하드웨어 ID 를 갖고 있어야 한다.
 * 3. 그 ID 가 IBM37D0 이나 IBM37D4 여야 한다.
 *
 * 찾으면 핸들을 호출자가 준 자리에 담고 멈춤 값을 돌려준다.
 *
 * 멈춤 값 FOUND_APCI 의 정체가 재미있다. 0x61504349 는 아스키로 "aPCI" 이며,
 * 표 앞머리의 서명과 같은 글자다. 흔한 오류 코드와 겹치지 않는 값을 고르되
 * 뜻이 통하게 한 선택이다.
 *
 * 옆의 상류 주석이 그 대가도 적어 두었다 — 0 아닌 값을 돌려주면 ACPI
 * 디버그 코드가 경고를 찍는다는 것이다. 그래도 순회를 일찍 끝내는 쪽을
 * 골랐다.
 *
 * [상류 코드 관찰] 세 가지.
 *  1) 반환형이 acpi_status(부호 없는 32비트)인데 지역 변수는 int 이고,
 *     그 값을 그대로 돌려준다.
 *  2) 장치 정보를 못 얻으면 **정보 구조체를 풀지 않고** 곧바로 돌아간다.
 *     다만 실패한 경우이므로 채워졌을 리는 없다.
 *  3) 상태를 얻는 호출의 반환값을 확인하지 않는다. 실패하면 상태가 0 인
 *     채로 남아 조건 1 에서 걸러진다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 모듈 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: 정보를 못 얻으면 0 을 돌려줘 순회를 계속하게 한다 — 그 장치를
 * 건너뛰는 셈이다.
 *
 * 호출 체인:
 *   ibm_acpiphp_init() → acpi_walk_namespace() → [이 함수]
 *     → acpi_get_object_info() → acpi_bus_get_status_handle() → kfree()
 */
/**
 * ibm_find_acpi_device - callback to find our ACPI device
 * @handle: the ACPI handle of the device we are inspecting
 * @lvl: depth into the namespace tree
 * @context: a pointer to our handle to fill when we find the device
 * @rv: a return value to fill if desired
 *
 * Description: Used as a callback when calling acpi_walk_namespace
 * to find our device.  When this method returns non-zero
 * acpi_walk_namespace quits its search and returns our value.
 */
static acpi_status __init ibm_find_acpi_device(acpi_handle handle,
		u32 lvl, void *context, void **rv)
{
	acpi_handle *phandle = (acpi_handle *)context;
	unsigned long long current_status = 0;
	/* [한국어] ACPI 호출의 성패. */
	acpi_status status;
	/* [한국어] 얻어 올 장치 정보. 다 쓰면 풀어야 한다. */
	struct acpi_device_info *info;
	/* [한국어] 반환값. **0 이 "계속 찾아라" 를 뜻하므로** 기본값이 곧 계속이다. */
	int retval = 0;

	status = acpi_get_object_info(handle, &info);
	/* [한국어] 이 장치의 정보를 얻지 못했다. */
	if (ACPI_FAILURE(status)) {
		/* [한국어] 실패를 알린다. */
		pr_err("%s:  Failed to get device information status=0x%x\n",
			__func__, status);
		return retval;
	/* [한국어] 0 을 돌려줘 순회를 계속하게 한다 — 이 장치만 건너뛰는 셈이다.
	 * [상류 코드 관찰] 정보 구조체를 풀지 않는데, 실패했으면 채워지지
	 * 않았을 것이라는 전제다. */
	}

	acpi_bus_get_status_handle(handle, &current_status);

	if (current_status && (info->valid & ACPI_VALID_HID) &&
			/* [한국어] 두 하드웨어 ID 중 하나와 맞는지 본다. */
			(!strcmp(info->hardware_id.string, IBM_HARDWARE_ID1) ||
			 !strcmp(info->hardware_id.string, IBM_HARDWARE_ID2))) {
		pr_debug("found hardware: %s, handle: %p\n",
			/* [한국어] 찾은 것을 디버그 출력에 남긴다. */
			info->hardware_id.string, handle);
		*phandle = handle;
		/* returning non-zero causes the search to stop
		 * and returns this value to the caller of
		 * acpi_walk_namespace, but it also causes some warnings
		 * in the acpi debug code to print...
		 */
		retval = FOUND_APCI;
	}
	kfree(info);
	return retval;
}

/* [한국어]
 * ibm_acpiphp_init - IBM 장치를 찾아 LED 콜백·알림 처리기·sysfs 파일을 건다
 *
 * @return: 0 = 성공, 음수 오류.
 *
 * 모듈의 진입점이며 네 단계다.
 *
 * 1. **ACPI 이름 공간 전체를 훑어** IBM 장치를 찾는다. 순회가 위 콜백의
 *    멈춤 값을 그대로 돌려주므로, 그 값과 같은지로 성패를 가른다.
 * 2. 그 핸들에 대응하는 ACPI 장치 구조체를 얻는다. 알림 이벤트를 올릴 때
 *    장치 이름과 종류가 필요하기 때문이다.
 * 3. acpiphp 코어에 LED 콜백 표를 등록한다. 이때부터 슬롯의 attention
 *    sysfs 조작이 이 파일로 들어온다.
 * 4. 알림 처리기를 걸고, 표 크기를 재어 sysfs 이진 파일을 만든다.
 *
 * 4번에서 크기를 재는 방식이 눈에 띈다 — 표를 읽는 함수에 NULL 을 넘겨
 * **크기만 얻는** 모드를 쓴다. sysfs 이진 파일은 만들 때 크기를 알려 줘야
 * 하기 때문이다.
 *
 * [상류 코드 관찰] 세 가지.
 *  1) 그 크기 계산이 실패하면 음수가 돌아오는데, 그것을 부호 없는 크기
 *     필드에 그대로 넣는다. 결과적으로 아주 큰 값이 된다.
 *  2) 마지막의 sysfs 파일 생성이 실패해도 **앞서 건 알림 처리기와 콜백
 *     등록을 되돌리지 않고** 그 오류를 그대로 돌려준다. 모듈 적재는
 *     실패하는데 정리는 되지 않는 상태가 된다.
 *  3) 되감기 라벨이 둘인데 하나(init_return)는 실제로 되감는 것이 없다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 모듈 적재. 프로세스 컨텍스트.
 *
 * 에러 경로: 장치를 못 찾으면 -ENODEV, 콜백 등록 실패도 -ENODEV,
 * 알림 처리기 등록 실패는 -EBUSY 이며 이때만 콜백 등록을 되감는다.
 *
 * 호출 체인:
 *   module_init → [이 함수]
 *     → acpi_walk_namespace() → acpi_fetch_acpi_dev()
 *     → acpiphp_register_attention() → acpi_install_notify_handler()
 *     → ibm_get_table_from_acpi(NULL) → sysfs_create_bin_file()
 */
static int __init ibm_acpiphp_init(void)
{
	int retval = 0;
	acpi_status status;
	/* [한국어] 알림 이벤트에 쓸 ACPI 장치. */
	struct acpi_device *device;
	/* [한국어] sysfs 파일이 붙을 자리 — PCI 슬롯 디렉터리다. 이 kset 은
	 * drivers/pci/pci.h 소관이다. */
	struct kobject *sysdir = &pci_slots_kset->kobj;

	pr_debug("%s\n", __func__);

	if (acpi_walk_namespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
			/* [한국어] 이름 공간의 **모든 장치** 를 훑으며 위 콜백을 부른다. 콜백이
			 * 멈춤 값을 돌려주면 순회가 끝나고 그 값이 여기로 온다. */
			ACPI_UINT32_MAX, ibm_find_acpi_device, NULL,
			&ibm_acpi_handle, NULL) != FOUND_APCI) {
		pr_err("%s: acpi_walk_namespace failed\n", __func__);
		/* [한국어] IBM 장치가 없는 시스템이다. 이 모듈이 할 일이 없다. */
		retval = -ENODEV;
		/* [한국어] 되감을 것이 없는 자리로 간다. */
		goto init_return;
	}
	pr_debug("%s: found IBM aPCI device\n", __func__);
	/* [한국어] 핸들에 대응하는 ACPI 장치 구조체를 얻는다. */
	device = acpi_fetch_acpi_dev(ibm_acpi_handle);
	/* [한국어] 장치 구조체를 못 얻었다. */
	if (!device) {
		/* [한국어] 실패를 알린다. */
		pr_err("%s: acpi_fetch_acpi_dev failed\n", __func__);
		/* [한국어] 장치 없음으로 답한다. */
		retval = -ENODEV;
		goto init_return;
	}
	/* [한국어] acpiphp 코어에 LED 콜백 표를 등록한다. 실패하면 0 이 아닌 값이 온다. */
	if (acpiphp_register_attention(&ibm_attention_info)) {
		/* [한국어] 장치 없음으로 답한다. */
		retval = -ENODEV;
		/* [한국어] 되감을 것이 없는 자리로 간다 — 등록이 실패했으니 해제할 것도 없다. */
		goto init_return;
	}

	/* [한국어] 알림 처리기가 쓸 문맥에 장치를 심는다. 처리기를 걸기 전이어야 한다. */
	ibm_note.device = device;
	/* [한국어] 그 장치의 ACPI 알림 처리기를 건다. */
	status = acpi_install_notify_handler(ibm_acpi_handle,
			/* [한국어] ACPI_DEVICE_NOTIFY 는 장치별 알림을 받겠다는 뜻이다. 문맥으로
			 * 위 전역 구조체의 주소를 넘겨, 처리기가 두 알림 사이의 정보를 나를 수
			 * 있게 한다. */
			ACPI_DEVICE_NOTIFY, ibm_handle_events,
			&ibm_note);
	if (ACPI_FAILURE(status)) {
		/* [한국어] 처리기를 걸지 못했다. */
		pr_err("%s: Failed to register notification handler\n",
				/* [한국어]  */
				__func__);
		retval = -EBUSY;
		/* [한국어] 이번에는 되감을 것이 있다 — 앞서 등록한 콜백 표를 해제해야 한다. */
		goto init_cleanup;
	}

	ibm_apci_table_attr.size = ibm_get_table_from_acpi(NULL);
	/* [한국어] sysfs 이진 파일을 만든다.
	 * [상류 코드 관찰] 이것이 실패해도 앞서 건 알림 처리기와 콜백 등록을
	 * 되돌리지 않고 그 오류를 그대로 돌려준다. */
	retval = sysfs_create_bin_file(sysdir, &ibm_apci_table_attr);
/* [한국어]  */

	return retval;
/* [한국어]  */

init_cleanup:
	acpiphp_unregister_attention(&ibm_attention_info);
init_return:
	return retval;
}

/* [한국어]
 * ibm_acpiphp_exit - 등록한 것 셋을 모두 되돌린다
 *
 * 모듈이 내려갈 때 불리며, 초기화의 역순으로 셋을 정리한다 — LED 콜백
 * 등록 해제, 알림 처리기 제거, sysfs 파일 삭제다.
 *
 * **세 단계가 서로의 실패에 영향받지 않는다.** 앞이 실패해도 기록만 남기고
 * 다음으로 넘어가며, 마지막 sysfs 삭제는 결과를 보지도 않는다. 모듈을
 * 내리는 중이라 중단할 수 없기 때문이다.
 *
 * 순서가 초기화의 정확한 역순은 아니다. 초기화는 콜백 등록 → 알림 처리기 →
 * sysfs 인데, 여기서도 같은 차례로 되돌린다. **알림 처리기를 먼저 떼지
 * 않으므로**, 콜백 등록을 해제한 뒤에도 알림이 올 수 있는 창이 잠시 열린다.
 * 다만 알림 처리기는 콜백 표를 쓰지 않으므로 실제 문제로 이어지지는 않는다.
 *
 * 실행 컨텍스트: 모듈 제거. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환형이 void 라 실패를 알릴 방법이 없고, 앞의 둘은
 * 기록만 남긴다.
 *
 * 호출 체인:
 *   module_exit → [이 함수]
 *     → acpiphp_unregister_attention() → acpi_remove_notify_handler()
 *     → sysfs_remove_bin_file()
 */
static void __exit ibm_acpiphp_exit(void)
{
	acpi_status status;
	struct kobject *sysdir = &pci_slots_kset->kobj;
/* [한국어]  */

	pr_debug("%s\n", __func__);
/* [한국어]  */

	if (acpiphp_unregister_attention(&ibm_attention_info))
		/* [한국어] 해제가 실패해도 기록만 남기고 계속한다. 모듈을 내리는 중이라
		 * 중단할 수 없다. */
		pr_err("%s: attention info deregistration failed", __func__);
/* [한국어]  */

	status = acpi_remove_notify_handler(
			   /* [한국어] 등록할 때와 같은 핸들·종류·함수를 주어야 짝이 맞는다. */
			   ibm_acpi_handle,
			   ACPI_DEVICE_NOTIFY,
			   ibm_handle_events);
	if (ACPI_FAILURE(status))
		/* [한국어] 역시 기록만 남기고 계속한다. */
		pr_err("%s: Notification handler removal failed\n", __func__);
	/* remove the /sys entries */
	sysfs_remove_bin_file(sysdir, &ibm_apci_table_attr);
}

module_init(ibm_acpiphp_init);
module_exit(ibm_acpiphp_exit);
