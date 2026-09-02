// SPDX-License-Identifier: GPL-2.0+
/*
 * Compaq Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>
 *
 */

/*
 * [한국어 설명] Compaq 핫플러그 자원 표를 NVRAM 에 저장하고 읽는 계층 (cpqphp_nvram.c)
 *
 * === 파일의 역할 ===
 * Compaq 서버의 핫플러그 컨트롤러가 관리하는 **자원 표(HRT, Hot plug
 * Resource Table)** 를 시스템 NVRAM 에 넣고 꺼내는 파일이다. 그 표에는
 * 각 컨트롤러가 하위 슬롯에 나눠 줄 수 있는 메모리·프리페치 메모리·
 * I/O·버스 번호 구간이 들어 있다.
 *
 * 이것이 필요한 이유는 핫플러그의 성격 때문이다. 카드를 꽂았을 때 줄
 * 주소 공간을 미리 확보해 두어야 하는데, 부팅 때마다 다시 계산하면
 * 재부팅 뒤 같은 카드가 다른 주소를 받게 된다. NVRAM 에 남겨 두면
 * 다음 부팅에서 그 배치를 그대로 되살릴 수 있다.
 *
 * **이 파일은 x86 BIOS 에 직접 의존한다.** NVRAM 접근이 커널 API 가
 * 아니라 Compaq 이 정의한 INT 15h 확장 호출로 이뤄지며, 그 호출을
 * 인라인 어셈블리로 직접 건다. 그래서 이 파일 전체가 사실상 x86 전용이고,
 * 그 시대의 관용 — 인라인 어셈블리, 레지스터 이름을 그대로 딴 구조체,
 * 널리 쓰이지 않는 명명 규칙 — 이 곳곳에 남아 있다.
 *
 * 상류 개발자 자신이 이 방식을 미심쩍어했다는 흔적이 파일 앞머리에
 * 남아 있다("We really shouldn't be doing this unless there is a _very_
 * good reason to!!!"). 그 주석을 지우지 않고 그대로 두었다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   cpqphp_core.c 의 probe -> compaq_nvram_init(rom_start)
 *     -> INT 15h 진입점 주소를 계산해 저장
 *   컨트롤러마다 -> compaq_nvram_load(rom_start, ctrl)
 *     -> 처음이면 load_HRT() 로 NVRAM 을 통째로 읽어 evbuffer 에 담고,
 *        그 안에서 이 컨트롤러의 항목을 찾아 자원 목록으로 되살린다
 *
 * 종료·정리 시:
 *   cpqphp_core.c -> compaq_nvram_store(rom_start)
 *     -> store_HRT() 가 지금의 자원 목록을 직렬화해 NVRAM 에 쓴다
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. 다만 access_EV() 의 인라인
 * 어셈블리 구간은 인터럽트를 끄고 스핀락을 쥔 채 BIOS 로 진입한다 —
 * BIOS 호출이 재진입 불가능하기 때문이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: cpqphp_core.c 가 세 공개 함수(init/load/store)를 부른다.
 * 아래쪽: 시스템 ROM 의 INT 15h 진입점. 커널 바깥이라 이 트리에서
 *   확인할 수 없으며, 이 파일에서 읽히는 호출 규약까지만 적었다.
 * 옆쪽: cpqphp.h 의 struct controller 와 struct pci_resource 를 공유한다.
 *   그 자원 목록이 이 파일이 직렬화하고 되살리는 대상이다.
 *
 * 데이터 흐름:
 *   NVRAM -> access_EV(READ_EV) -> evbuffer(1KB) -> compaq_nvram_load()
 *     -> struct pci_resource 연결 목록 -> 컨트롤러의 mem/p_mem/io/bus 머리
 *   그 반대 방향이 store 다. 중간의 evbuffer 가 두 방향의 만남 지점이며,
 *   **파일 정적 변수** 라 모든 컨트롤러가 그 하나를 나눠 쓴다.
 *
 * 공유 상태: evbuffer 와 그 길이·초기화 여부, 그리고 INT 15h 진입점.
 *   모두 파일 정적이며, evbuffer_init 이 "이미 읽어 왔는가" 를 기억해
 *   여러 컨트롤러가 NVRAM 을 거듭 읽지 않게 한다.
 *
 * === NVMe 관점 ===
 * 이 하드웨어는 NVMe 보다 십수 년 앞선 것이라 직접적인 접점이 없다.
 * 다만 이 파일이 푸는 문제 — 핫플러그 슬롯에 줄 주소 공간을 미리 확보해
 * 재부팅 뒤에도 같은 배치를 유지한다 — 는 요즘 NVMe 핫플러그 백플레인이
 * 겪는 문제와 같으며, 지금은 그 역할을 펌웨어의 _DSM 과 커널의 자원
 * 재배정이 나눠 맡는다.
 *
 * === 주요 함수/구조체 요약 ===
 * compaq_nvram_init()   : INT 15h 진입점 주소를 계산해 둔다.
 * compaq_nvram_load()   : NVRAM 의 표에서 이 컨트롤러 항목을 찾아
 *                         자원 연결 목록으로 되살린다.
 * compaq_nvram_store()  : 지금의 자원 목록을 NVRAM 에 직렬화한다.
 * access_EV()           : INT 15h 를 인라인 어셈블리로 부르는 유일한 자리.
 * add_byte()/add_dword(): 버퍼 경계를 확인하며 값을 채워 넣는 직렬화 도구.
 * struct ev_hrt_header  : NVRAM 표의 머리 — 판본과 컨트롤러 수.
 * struct ev_hrt_ctrl    : 컨트롤러 하나의 위치와 자원 구간 개수.
 */

/* [한국어] MODULE_ 매크로들. */
#include <linux/module.h>
/* [한국어] 기본 커널 관용구. */
#include <linux/kernel.h>
/* [한국어] u8, u32 등 기본 타입. */
#include <linux/types.h>
/* [한국어] procfs 선언. 이 파일에서 직접 쓰는 곳은 없다(전수 확인). */
#include <linux/proc_fs.h>
/* [한국어] kmalloc_obj()/kfree(). 자원 노드를 하나씩 잡는다. */
#include <linux/slab.h>
/* [한국어] 워크큐 선언. 이 파일에서 직접 쓰는 곳은 없다(전수 확인). */
#include <linux/workqueue.h>
/* [한국어] PCI_SLOT()/PCI_FUNC(). 컨트롤러의 devfn 을 쪼개는 데 쓴다. */
#include <linux/pci.h>
/* [한국어] 핫플러그 공용 선언. */
#include <linux/pci_hotplug.h>
/* [한국어] uaccess 헬퍼. 이 파일에서 직접 쓰는 곳은 없다(전수 확인). */
#include <linux/uaccess.h>
/* [한국어] 이 드라이버의 struct controller 와 struct pci_resource, 그리고
 * dbg()/err() 매크로와 msg_unable_to_save 문자열. */
#include "cpqphp.h"
/* [한국어] 이 파일의 세 공개 함수 선언. */
#include "cpqphp_nvram.h"


/* [한국어] INT 15h 진입점의 **물리** 주소. compaq_nvram_init() 이 이 값과
 * ROM 의 물리·가상 주소 차이로 진입점의 가상 주소를 계산한다.
 * 이 상수의 근거는 Compaq 의 약속이며 이 트리에서 확인 못 함. */
#define ROM_INT15_PHY_ADDR		0x0FF859
/* [한국어] 환경 변수 읽기 연산 코드. Compaq 이 INT 15h 에 더한 확장이다. */
#define READ_EV				0xD8A4
/* [한국어] 쓰기 연산 코드. 두 값 모두 표준 INT 15h 기능이 아니라
 * Compaq BIOS 에만 있는 것이라, check_for_compaq_ROM() 의 확인이 선행돼야 한다. */
#define WRITE_EV			0xD8A5

/* [한국어] BIOS 호출에 넘길 레지스터 하나의 서술. **이 파일에서 쓰이지 않는다** —
 * access_EV() 가 인라인 어셈블리로 레지스터를 직접 다루기 때문이다.
 * 호출 규약을 자료구조로 표현해 두었다가 결국 다른 방법을 택한 흔적이다. */
struct register_foo {
	union {
		/* [한국어] 32비트 접근 갈래(eax).
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해당 없음.
		 * 동기화: 해당 없음. */
		unsigned long lword;		/* eax */
		/* [한국어] 16비트 접근 갈래(ax).
		 * 설정자·읽는 자: 이 파일에서 이 공용체를 실제로 쓰는 코드는 없다 —
		 * access_EV() 가 인라인 어셈블리로 직접 레지스터를 다루기 때문이다.
		 * 값 범위: 32비트 값의 하위 16비트.
		 * 동기화: 해당 없음. */
		unsigned short word;		/* ax */

		struct {
			/* [한국어] 8비트 하위 갈래(al). */
			unsigned char low;	/* al */
			/* [한국어] 8비트 상위 갈래(ah). BIOS 가 상태를 여기 담아 돌려주는데,
			 * access_EV() 는 이 구조체 대신 반환값을 8비트 밀어 꺼낸다. */
			unsigned char high;	/* ah */
		/* [한국어] 바이트 단위 갈래. */
		} byte;
	/* [한국어] 레지스터 하나의 데이터. x86 의 eax/ax/ah/al 중첩을 그대로 흉내 낸 것이다. */
	} data;

	/* [한국어] 연산 코드(옆의 상류 주석이 '아래 참조' 라고 하나, 이 파일에 그 설명은 없다).
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해당 없음.
	 * 동기화: 해당 없음. */
	unsigned char opcode;	/* see below */
	/* [한국어] 그 레지스터가 포인터일 때 가리키는 데이터의 길이(옆의 상류 주석).
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해당 없음.
	 * 동기화: 해당 없음. */
	unsigned long length;	/* if the reg. is a pointer, how much data */
/* [한국어] 레지스터 하나의 서술. __packed 인 것은 BIOS 와 주고받을 형식이기 때문이나,
 * 이 파일은 결국 이 구조체를 쓰지 않는다. */
} __attribute__ ((packed));

/* [한국어] BIOS 호출에 넘길 레지스터 한 벌. 위 구조체와 마찬가지로 쓰이지 않는다. */
struct all_reg {
	/* [한국어] eax 레지스터. */
	struct register_foo eax_reg;
	/* [한국어] ebx. */
	struct register_foo ebx_reg;
	/* [한국어] ecx. */
	struct register_foo ecx_reg;
	/* [한국어] edx. */
	struct register_foo edx_reg;
	/* [한국어] edi. */
	struct register_foo edi_reg;
	/* [한국어] esi. */
	struct register_foo esi_reg;
	/* [한국어] eflags. 일곱 레지스터를 담은 이 구조체 역시 이 파일에서 쓰이지 않는다 —
	 * BIOS 호출 규약을 자료구조로 표현해 두었다가 결국 인라인 어셈블리로 갔다. */
	struct register_foo eflags_reg;
/* [한국어] BIOS 호출에 넘길 레지스터 한 벌. */
} __attribute__ ((packed));


/* [한국어] NVRAM 표의 머리. 이 아래 구조체 둘이 실제 직렬화 형식을 정의한다. */
struct ev_hrt_header {
	/* [한국어] 표의 판본.
	 * 설정자: store_HRT() 가 `1 + ctrl->push_flag` 를 넣는다 — 그 플래그가 서면 2 다.
	 * 읽는 자: compaq_nvram_load() 가 이 값으로 표를 만든 드라이버의 세대를 가린다.
	 * 값 범위: 1 또는 2.
	 * 동기화: NVRAM 버퍼 위의 값이다. */
	u8 Version;
	/* [한국어] 이 표에 담긴 컨트롤러 수.
	 * 설정자: store_HRT() 가 **모든 컨트롤러를 쓴 뒤** 마지막에 채운다.
	 * 읽는 자: 이 파일의 파싱 경로는 이 값을 쓰지 않고 길이 검사로 끝을 판단한다.
	 * 값 범위: 1 이상.
	 * 동기화: NVRAM 버퍼 위의 값이다. */
	u8 num_of_ctrl;
	/* [한국어] **첫 컨트롤러 항목이 시작하는 자리** 를 가리키는 표시.
	 * 설정자: 이 필드에 값을 넣는 코드는 없다 — 그 주소 자체가 쓰인다.
	 * 읽는 자: compaq_nvram_load() 가 `&p_EV_header->next` 를 첫 항목의
	 * 시작으로 삼는다.
	 * 값 범위: 해당 없음(자리 표시자).
	 * 동기화: NVRAM 버퍼 위의 값이다. */
	u8 next;
/* [한국어] 표의 머리. 판본과 컨트롤러 수, 그리고 첫 항목의 자리. */
};

/* [한국어] 컨트롤러 하나의 항목. */
struct ev_hrt_ctrl {
	/* [한국어] 이 컨트롤러의 버스 번호.
	 * 설정자: store_HRT().
	 * 읽는 자: compaq_nvram_load() 가 찾는 컨트롤러인지 비교한다.
	 * 값 범위: 유효한 PCI 버스 번호.
	 * 동기화: NVRAM 버퍼 위의 값이다. */
	u8 bus;
	/* [한국어] 장치 번호. 위와 같은 규약이다. */
	u8 device;
	/* [한국어] 기능 번호. 위 셋이 이 컨트롤러를 특정한다. */
	u8 function;
	/* [한국어] 메모리 자원 구간의 개수.
	 * 설정자: store_HRT() 가 컨트롤러의 자원 목록을 세어 채운다.
	 * 읽는 자: compaq_nvram_load() 가 그 개수만큼 dword 쌍을 읽는다.
	 * 값 범위: 그 종류의 자원 구간 수.
	 * 동기화: NVRAM 버퍼 위의 값이라 파싱하는 쪽만 본다. */
	u8 mem_avail;
	/* [한국어] 프리페치 메모리 구간의 개수.
	 * 설정자: store_HRT() 가 컨트롤러의 자원 목록을 세어 채운다.
	 * 읽는 자: compaq_nvram_load() 가 그 개수만큼 dword 쌍을 읽는다.
	 * 값 범위: 그 종류의 자원 구간 수.
	 * 동기화: NVRAM 버퍼 위의 값이라 파싱하는 쪽만 본다. */
	u8 p_mem_avail;
	/* [한국어] I/O 구간의 개수.
	 * 설정자: store_HRT() 가 컨트롤러의 자원 목록을 세어 채운다.
	 * 읽는 자: compaq_nvram_load() 가 그 개수만큼 dword 쌍을 읽는다.
	 * 값 범위: 그 종류의 자원 구간 수.
	 * 동기화: NVRAM 버퍼 위의 값이라 파싱하는 쪽만 본다. */
	u8 io_avail;
	/* [한국어] 버스 번호 구간의 개수.
	 * 설정자: store_HRT() 가 컨트롤러의 자원 목록을 세어 채운다.
	 * 읽는 자: compaq_nvram_load() 가 그 개수만큼 dword 쌍을 읽는다.
	 * 값 범위: 그 종류의 자원 구간 수.
	 * 동기화: NVRAM 버퍼 위의 값이라 파싱하는 쪽만 본다. */
	u8 bus_avail;
	/* [한국어] **자원 데이터가 시작하는 자리** 를 가리키는 표시. 위 머리의 next 와 같은 쓰임이다. */
	u8 next;
/* [한국어] 컨트롤러 하나의 항목. 위치와 네 종류 자원의 구간 개수를 담는다. */
};


/* [한국어] NVRAM 을 이미 읽어 왔는지.
 * 설정자: compaq_nvram_load() 가 첫 호출에서 1 로 세운다.
 * 읽는 자: 같은 함수가 거듭 읽지 않도록 확인하고,
 * compaq_nvram_store() 가 저장해도 되는지 판단하는 데 쓴다.
 * 값 범위: 0 = 아직, 1 = 읽어 왔음.
 * 동기화: 파일 정적이며 별도 잠금이 없다 — 컨트롤러 초기화가
 * 직렬로 진행된다는 전제다. */
static u8 evbuffer_init;
/* [한국어] 그 버퍼에 실제로 담긴 길이.
 * 설정자: load_HRT() 가 읽어 온 크기를, store_HRT() 가 쓴 크기를 담는다.
 * 읽는 자: compaq_nvram_load() 의 범위 검사가 이 값을 끝으로 삼는다.
 * 값 범위: 0~255. **u8 이라 255 를 넘는 길이는 잘린다** — 버퍼가 1024바이트인데
 * 길이는 8비트로만 기억하는 셈이다.
 * 동기화: 파일 정적이며 별도 잠금이 없다. */
static u8 evbuffer_length;
/* [한국어] NVRAM 에서 읽어 온 표를 담는 버퍼.
 * 설정자: load_HRT() 가 채우고 store_HRT() 가 쓸 내용을 만든다.
 * 읽는 자: compaq_nvram_load() 의 파싱.
 * 값 범위: 1KB 고정. NVRAM 환경 변수의 최대 크기와 맞춘 값이다.
 * 동기화: 파일 정적이며 모든 컨트롤러가 하나를 나눠 쓴다.
 * **읽기와 쓰기가 같은 버퍼를 쓴다** — store_HRT() 가 이 버퍼 위에서 직렬화한다. */
static u8 evbuffer[1024];

/* [한국어] INT 15h 진입점의 가상 주소.
 * 설정자: compaq_nvram_init() 이 물리 주소와 ROM 매핑에서 계산한다.
 * 읽는 자: access_EV() 가 인라인 어셈블리의 간접 호출 대상으로 쓴다.
 * 값 범위: 유효한 iomem 포인터 또는 NULL. NULL 이면 NVRAM 기능이 조용히 꺼진다.
 * 동기화: 초기화 후 불변. */
static void __iomem *compaq_int15_entry_point;

/* lock for ordering int15_bios_call() */
/* [한국어] BIOS 호출을 직렬화하는 스핀락(옆의 상류 주석대로 순서를 지키기 위한 것).
 * 설정자·읽는 자: access_EV() 만 잡는다.
 * 값 범위: 스핀락.
 * 동기화: 두 CPU 가 동시에 BIOS 로 들어가지 못하게 막는다.
 * 어셈블리 안의 cli 와 역할이 다르다 — 이쪽은 CPU 사이를,
 * 그쪽은 이 CPU 의 인터럽트를 막는다. */
static DEFINE_SPINLOCK(int15_lock);


/* This is a series of function that deals with
 * setting & getting the hotplug resource table in some environment variable.
 */

/*
 * We really shouldn't be doing this unless there is a _very_ good reason to!!!
 * greg k-h
 */


/* [한국어]
 * add_byte - 버퍼에 1바이트를 넣고 포인터를 한 칸 민다
 *
 * @p_buffer: 쓸 자리를 가리키는 포인터의 포인터. 쓴 뒤 1바이트 전진한다.
 * @value: 넣을 값.
 * @used: 지금까지 쓴 바이트 수. 여기서 1 늘어난다.
 * @avail: 쓸 수 있는 총 바이트 수.
 * @return: 0 = 성공, 1 = 자리가 모자람.
 *
 * store_HRT() 가 자원 표를 직렬화할 때 쓰는 두 도구 중 하나다.
 *
 * **넘침 검사가 이 함수의 존재 이유다.** 표의 크기가 컨트롤러 수와 자원
 * 구간 수에 따라 달라져 미리 계산하기 어려운데, 매 항목마다 남은 자리를
 * 확인하면 그 계산이 필요 없어진다.
 *
 * 포인터 타입 놀음이 눈에 띈다. 인자가 u32** 인데 1바이트만 쓰므로,
 * u8** 로 형변환한 뒤 그것을 통해 전진시킨다 — 그래야 4 가 아니라 1 만큼
 * 움직인다.
 *
 * 반환값 관용이 요즘과 반대다. 0 이 성공이고 1 이 실패인데, 음수 errno 가
 * 아니라 그 시대의 방식이다.
 *
 * 실행 컨텍스트: store_HRT() 안. 잠들지 않는다.
 *
 * 에러 경로: 자리가 모자라면 1 을 돌려주며, 호출자가 즉시 중단한다.
 *
 * 호출 체인:
 *   store_HRT() → [이 함수]
 */
static u32 add_byte(u32 **p_buffer, u8 value, u32 *used, u32 *avail)
{
	u8 **tByte;

	if ((*used + 1) > *avail)
		/* [한국어] 자리가 모자라면 1 을 돌려준다. 음수 errno 가 아니라 그 시대의 관용이다. */
		return(1);

	*((u8 *)*p_buffer) = value;
	tByte = (u8 **)p_buffer;
	(*tByte)++;
	*used += 1;
	return(0);
}


/* [한국어]
 * add_dword - 버퍼에 4바이트를 넣고 포인터를 한 칸 민다
 *
 * @p_buffer: 쓸 자리를 가리키는 포인터의 포인터. 쓴 뒤 4바이트 전진한다.
 * @value: 넣을 값.
 * @used: 지금까지 쓴 바이트 수. 여기서 4 늘어난다.
 * @avail: 쓸 수 있는 총 바이트 수.
 * @return: 0 = 성공, 1 = 자리가 모자람.
 *
 * add_byte() 의 4바이트 판이며 규약이 같다.
 *
 * 이쪽은 형변환이 필요 없다 — 포인터 타입이 이미 u32* 라 한 칸 전진이
 * 곧 4바이트다.
 *
 * 정렬을 확인하지 않는다. 자원 표에서 dword 앞에 바이트 항목이 홀수 개
 * 오면 정렬되지 않은 쓰기가 되는데, x86 이라 동작에는 문제가 없다.
 *
 * 실행 컨텍스트: store_HRT() 안. 잠들지 않는다.
 *
 * 에러 경로: 자리가 모자라면 1 을 돌려주며, 호출자가 즉시 중단한다.
 *
 * 호출 체인:
 *   store_HRT() → [이 함수]
 */
static u32 add_dword(u32 **p_buffer, u32 value, u32 *used, u32 *avail)
{
	if ((*used + 4) > *avail)
		return(1);

	**p_buffer = value;
	(*p_buffer)++;
	*used += 4;
	return(0);
}


/*
 * check_for_compaq_ROM
 *
 * this routine verifies that the ROM OEM string is 'COMPAQ'
 *
 * returns 0 for non-Compaq ROM, 1 for Compaq ROM
 */
/* [한국어]
 * check_for_compaq_ROM - ROM 의 OEM 문자열이 'COMPAQ' 인지 확인한다
 *
 * @rom_start: 매핑된 시스템 ROM 의 시작 주소.
 * @return: 1 = Compaq ROM, 0 = 아님.
 *
 * 위 상류 주석이 하는 일을 그대로 밝힌다.
 *
 * 이 확인이 필요한 이유는 아래 access_EV() 가 Compaq 이 정의한 INT 15h
 * 확장을 부르기 때문이다. 다른 벤더의 BIOS 에 그 호출을 걸면 무슨 일이
 * 일어날지 알 수 없으므로, 먼저 이 ROM 이 Compaq 것인지 확인한다.
 *
 * 문자를 여섯 개의 지역 변수에 하나씩 읽어 비교한다. memcmp 를 쓰지 않는
 * 것은 그 자리가 iomem 이라 일반 메모리 함수를 쓸 수 없기 때문이다.
 *
 * 0xffea 라는 오프셋은 Compaq 이 정한 OEM 문자열의 자리이며, 그 근거는
 * 이 트리에서 확인 못 함.
 *
 * 실행 컨텍스트: load_HRT()/store_HRT() 안. 잠들지 않는다.
 *
 * 에러 경로: 없다. Compaq 이 아니면 0 이며, 호출자가 그때 물러난다.
 *
 * 호출 체인:
 *   load_HRT() / store_HRT() → [이 함수] → readb()
 */
static int check_for_compaq_ROM(void __iomem *rom_start)
{
	u8 temp1, temp2, temp3, temp4, temp5, temp6;
	int result = 0;

	temp1 = readb(rom_start + 0xffea + 0);
	/* [한국어] 'O'. */
	temp2 = readb(rom_start + 0xffea + 1);
	/* [한국어] 'M'. */
	temp3 = readb(rom_start + 0xffea + 2);
	/* [한국어] 'P'. */
	temp4 = readb(rom_start + 0xffea + 3);
	/* [한국어] 'A'. */
	temp5 = readb(rom_start + 0xffea + 4);
	/* [한국어] 'Q'. 여섯 바이트를 하나씩 읽는 것은 그 자리가 iomem 이라
	 * memcmp 같은 일반 메모리 함수를 쓸 수 없기 때문이다. */
	temp6 = readb(rom_start + 0xffea + 5);
	/* [한국어] 여섯 글자가 모두 맞으면 — */
	if ((temp1 == 'C') &&
	    /* [한국어] 한 글자라도 다르면 Compaq ROM 이 아니다. */
	    (temp2 == 'O') &&
	    (temp3 == 'M') &&
	    (temp4 == 'P') &&
	    (temp5 == 'A') &&
	    (temp6 == 'Q')) {
		result = 1;
	/* [한국어] 판정 끝. */
	}
	dbg("%s - returned %d\n", __func__, result);
	/* [한국어] 1 이면 Compaq, 0 이면 아니다. */
	return result;
/* [한국어] 이 확인 없이 아래 INT 15h 확장을 부르면 다른 벤더의 BIOS 에서
 * 무슨 일이 일어날지 알 수 없다. */
}


/* [한국어]
 * access_EV - BIOS 의 INT 15h 확장으로 NVRAM 환경 변수를 읽거나 쓴다
 *
 * @operation: READ_EV 또는 WRITE_EV.
 * @ev_name: 환경 변수 이름. 이 파일은 언제나 "CQTHPS" 를 쓴다.
 * @buffer: 읽어 담을 또는 써 보낼 버퍼.
 * @buf_size: 버퍼 크기. 읽기에서는 실제로 읽은 크기가 여기 돌아온다.
 * @return: BIOS 가 돌려준 상태 코드(0 이 성공), 또는 -ENODEV.
 *
 * **이 파일에서 유일하게 인라인 어셈블리를 쓰는 함수** 이며, 커널 바깥의
 * BIOS 코드로 직접 진입한다.
 *
 * 어셈블리가 하는 일이 그 시대의 호출 규약 그대로다 — 인자를 특정
 * 레지스터에 실어 두고(eax 에 연산, ecx 에 크기, esi 에 이름, edi 에 버퍼),
 * 플래그와 코드 세그먼트를 밀어 넣은 뒤 진입점으로 간접 호출한다.
 * `push %cs` 가 있는 것은 BIOS 가 far return 으로 돌아오기 때문이다.
 *
 * 잠금과 인터럽트 차단이 둘 다 있는 이유가 다르다. 스핀락은 두 CPU 가
 * 동시에 BIOS 로 들어가지 못하게 막고(옆의 상류 주석이 순서를 지키기
 * 위한 것이라 밝힌다), 어셈블리 안의 `cli` 는 BIOS 실행 중에 인터럽트가
 * 들어오지 못하게 막는다 — 그 코드가 재진입 불가능하기 때문이다.
 *
 * 반환값을 8비트 밀어 꺼낸다. BIOS 가 상태를 ah 에 담아 돌려주는데,
 * 그 레지스터가 eax 의 8~15비트 자리이기 때문이다.
 *
 * 진입점이 없으면 -ENODEV 다. compaq_nvram_init() 이 그것을 계산해 두지
 * 않았거나 ROM 이 매핑되지 않은 경우다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 스핀락을 쥐고 인터럽트를 끈 채로
 * BIOS 에 들어가므로 그 구간 동안 이 CPU 가 다른 일을 하지 못한다.
 *
 * 에러 경로: 진입점 부재는 -ENODEV, 그 밖은 BIOS 의 상태 코드를 그대로
 * 올려보낸다.
 *
 * 호출 체인:
 *   load_HRT() / store_HRT() → [이 함수] → (INT 15h BIOS 코드)
 */
static u32 access_EV(u16 operation, u8 *ev_name, u8 *buffer, u32 *buf_size)
{
	unsigned long flags;
	int op = operation;
	/* [한국어] BIOS 가 돌려줄 값. */
	int ret_val;
/* [한국어] 먼저 진입점이 준비돼 있는지 본다. */

	if (!compaq_int15_entry_point)
		/* [한국어] compaq_nvram_init() 이 계산하지 않았거나 ROM 이 매핑되지 않은 경우다. */
		return -ENODEV;

	spin_lock_irqsave(&int15_lock, flags);
	/* [한국어] **여기부터 커널 바깥의 BIOS 코드로 들어간다.** */
	__asm__ (
		/* [한국어] ebx 와 edx 를 0 으로 지운다 — BIOS 가 그 둘을 쓰지 않는다는 약속이다. */
		"xorl   %%ebx,%%ebx\n" \
		"xorl    %%edx,%%edx\n" \
		"pushf\n" \
		"push %%cs\n" \
		"cli\n" \
		"call *%6\n"
		: "=c" (*buf_size), "=a" (ret_val)
		/* [한국어] eax 에 연산, ecx 에 크기, esi 에 변수 이름을 싣는다. */
		: "a" (op), "c" (*buf_size), "S" (ev_name),
		/* [한국어] edi 에 버퍼를, 그리고 진입점 주소를 메모리 피연산자로 넘긴다. */
		"D" (buffer), "m" (compaq_int15_entry_point)
		: "%ebx", "%edx");
	/* [한국어] BIOS 에서 돌아왔으니 잠금을 놓는다. */
	spin_unlock_irqrestore(&int15_lock, flags);

	return((ret_val & 0xFF00) >> 8);
/* [한국어] 상태가 ah 에 담겨 오므로 8비트 밀어 꺼낸다. */
}


/*
 * load_HRT
 *
 * Read the hot plug Resource Table from NVRAM
 */
/* [한국어]
 * load_HRT - NVRAM 에서 자원 표를 읽어 evbuffer 에 담는다
 *
 * @rom_start: 매핑된 시스템 ROM 의 시작 주소.
 * @return: 0 = 성공, -ENODEV 또는 BIOS 상태 코드.
 *
 * 위 상류 주석이 하는 일을 밝힌다.
 *
 * **읽은 뒤 곧바로 무효화하는 것** 이 이 함수의 특이한 점이다. 옆의 상류
 * 주석이 이유를 밝히는데, 커널이 이제부터 자원 목록을 직접 관리하므로
 * NVRAM 에 남은 옛 정보가 다른 주체에게 읽히면 안 되기 때문이다. 1바이트
 * 0xFF 를 써서 표를 무효 표시한다.
 *
 * 그래서 이 함수는 읽기와 쓰기를 연달아 한다 — 그 둘이 한 쌍이며,
 * 나중에 compaq_nvram_store() 가 새 표를 다시 써 넣는다.
 *
 * [상류 코드 관찰] 읽기의 결과(rc)를 확인하지 않고 곧바로 덮어쓴다.
 * 읽기가 실패해도 evbuffer 의 내용을 그대로 유효한 표로 다루게 되며,
 * 반환되는 것은 무효화 쓰기의 결과뿐이다. 다만 호출자
 * compaq_nvram_load() 가 실패 시 evbuffer 를 0 으로 지우므로 결과적으로는
 * 빈 표가 된다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: compaq_nvram_load() 안. 프로세스 컨텍스트.
 *
 * 에러 경로: Compaq ROM 이 아니면 -ENODEV. 그 밖은 무효화 쓰기의 결과다.
 *
 * 호출 체인:
 *   compaq_nvram_load() → [이 함수]
 *     → check_for_compaq_ROM() → access_EV(READ_EV) → access_EV(WRITE_EV)
 */
static int load_HRT(void __iomem *rom_start)
{
	u32 available;
	u32 temp_dword;
	/* [한국어] 무효화에 쓸 값. 0xFF 한 바이트가 '이 표는 유효하지 않다' 를 뜻한다. */
	u8 temp_byte = 0xFF;
	/* [한국어] BIOS 호출의 결과. */
	u32 rc;

	if (!check_for_compaq_ROM(rom_start))
		/* [한국어] Compaq ROM 이 아니면 이 확장 호출을 걸 수 없다. */
		return -ENODEV;

	available = 1024;
/* [한국어] 버퍼 크기를 알린다. 읽은 뒤 실제 크기가 이 자리에 돌아온다. */

	/* Now load the EV */
	temp_dword = available;

	rc = access_EV(READ_EV, "CQTHPS", evbuffer, &temp_dword);
/* [한국어] **읽기 결과를 확인하지 않고 아래에서 덮어쓴다**(위 함수 블록의 관찰 참조). */

	evbuffer_length = temp_dword;
/* [한국어] 읽어 온 길이를 기록해 둔다. 파싱의 범위 검사가 이 값을 쓴다. */

	/* We're maintaining the resource lists so write FF to invalidate old
	 * info
	 */
	temp_dword = 1;

	rc = access_EV(WRITE_EV, "CQTHPS", &temp_byte, &temp_dword);
/* [한국어] 1바이트만 쓰겠다고 알린다. */

	return rc;
/* [한국어] 반환되는 것은 무효화 쓰기의 결과다. */
}


/*
 * store_HRT
 *
 * Save the hot plug Resource Table in NVRAM
 */
/* [한국어]
 * store_HRT - 지금의 자원 목록을 직렬화해 NVRAM 에 쓴다
 *
 * @rom_start: 매핑된 시스템 ROM 의 시작 주소.
 * @return: 0 = 성공, 1 = 실패.
 *
 * load_HRT() 의 짝이며 이 파일에서 가장 긴 함수다.
 *
 * 직렬화 형식이 두 겹이다.
 * - 머리 하나 — 판본과 컨트롤러 수.
 * - 컨트롤러마다 항목 하나 — 위치(버스·장치·기능)와 네 종류 자원의
 *   구간 개수, 그 뒤에 각 구간의 시작과 길이가 dword 쌍으로 이어진다.
 *
 * **개수를 나중에 채우는 것** 이 이 함수의 요령이다. 자원 구간이 몇 개인지
 * 미리 알 수 없으므로, 항목의 시작 주소를 기억해 두고 구간을 다 쓴 뒤에
 * 그 자리로 돌아가 개수를 적는다. p_ev_ctrl 과 p_EV_header 가 그 기억이다.
 *
 * 판본에 push_flag 를 더하는 것이 눈에 띈다. 그 플래그가 서면 판본이 2 가
 * 되어, compaq_nvram_load() 가 그것으로 이 표를 만든 드라이버의 세대를 가린다.
 *
 * 넘침이 나면 즉시 중단한다. 그때 NVRAM 에는 아무것도 쓰이지 않으므로
 * 이전 내용(무효 표시)이 그대로 남는다.
 *
 * [상류 코드 관찰] `buffer = (u32 *) evbuffer;` 뒤에 `if (!buffer)` 검사가
 * 있는데, evbuffer 는 파일 정적 배열이라 그 주소가 NULL 일 수 없다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: compaq_nvram_store() 안. 프로세스 컨텍스트.
 *
 * 에러 경로: Compaq ROM 이 아니거나 버퍼가 모자라면 1. NVRAM 쓰기가
 * 실패하면 기록을 남기고 1.
 *
 * 호출 체인:
 *   compaq_nvram_store() → [이 함수]
 *     → check_for_compaq_ROM() → add_byte()/add_dword() → access_EV(WRITE_EV)
 */
static u32 store_HRT(void __iomem *rom_start)
{
	u32 *buffer;
	u32 *pFill;
	/* [한국어] 지금까지 쓴 바이트 수. */
	u32 usedbytes;
	/* [한국어] 쓸 수 있는 총 바이트 수. */
	u32 available;
	/* [한국어] BIOS 에 넘길 크기. */
	u32 temp_dword;
	/* [한국어] 각 단계의 결과. */
	u32 rc;
	/* [한국어] 자원 구간을 세는 카운터. */
	u8 loop;
	/* [한국어] 실제로 쓴 컨트롤러 수. 마지막에 머리에 채운다. */
	u8 numCtrl = 0;
	/* [한국어] 컨트롤러 목록을 훑을 포인터. */
	struct controller *ctrl;
	/* [한국어] 자원 목록을 훑을 포인터. */
	struct pci_resource *resNode;
	/* [한국어] 표 머리를 가리킬 포인터. **개수를 나중에 채우려고 주소를 기억해 둔다.** */
	struct ev_hrt_header *p_EV_header;
	/* [한국어] 컨트롤러 항목을 가리킬 포인터. 역시 나중에 개수를 채우기 위한 것이다. */
	struct ev_hrt_ctrl *p_ev_ctrl;
/* [한국어] 먼저 쓸 수 있는 크기를 정한다. */

	available = 1024;
/* [한국어] 버퍼 크기와 같은 1KB 다. */

	if (!check_for_compaq_ROM(rom_start))
		/* [한국어] Compaq ROM 이 아니면 쓸 수 없다. */
		return(1);

	buffer = (u32 *) evbuffer;
/* [한국어] **읽기에 썼던 같은 버퍼 위에서 직렬화한다.** */

	if (!buffer)
		/* [한국어] [상류 코드 관찰] evbuffer 는 파일 정적 배열이라 그 주소가 NULL 일 수 없다 —
		 * 이 검사는 결코 참이 되지 않는다. 원본에서 확인했으며 코드는 고치지 않았다. */
		return(1);

	pFill = buffer;
	/* [한국어] 쓴 바이트를 0 부터 센다. */
	usedbytes = 0;
/* [한국어] 머리의 자리를 기억해 둔다. */

	p_EV_header = (struct ev_hrt_header *) pFill;
/* [한국어] 컨트롤러 목록의 처음부터 시작한다. */

	ctrl = cpqhp_ctrl_list;
/* [한국어] 판본을 쓴다 — push_flag 가 서면 2 가 되어, 읽는 쪽이 세대를 가린다. */

	/* The revision of this structure */
	rc = add_byte(&pFill, 1 + ctrl->push_flag, &usedbytes, &available);
	if (rc)
		/* [한국어] 자리가 모자라면 즉시 중단한다. 그때 NVRAM 에는 아무것도 쓰이지 않는다. */
		return(rc);

	/* The number of controllers */
	rc = add_byte(&pFill, 1, &usedbytes, &available);
	if (rc)
		/* [한국어] 컨트롤러 수 자리도 일단 1 로 채운다 — 실제 값은 마지막에 덮는다. */
		return(rc);

	while (ctrl) {
		/* [한국어] 이 컨트롤러 항목의 시작 주소를 기억해 둔다. */
		p_ev_ctrl = (struct ev_hrt_ctrl *) pFill;

		numCtrl++;
/* [한국어] 실제로 쓴 컨트롤러 수를 센다. */

		/* The bus number */
		rc = add_byte(&pFill, ctrl->bus, &usedbytes, &available);
		if (rc)
			/* [한국어] 버스 번호를 쓰지 못하면 중단한다. */
			return(rc);

		/* The device Number */
		rc = add_byte(&pFill, PCI_SLOT(ctrl->pci_dev->devfn), &usedbytes, &available);
		if (rc)
			/* [한국어] 장치 번호도 마찬가지다. */
			return(rc);

		/* The function Number */
		rc = add_byte(&pFill, PCI_FUNC(ctrl->pci_dev->devfn), &usedbytes, &available);
		if (rc)
			/* [한국어] 기능 번호까지 쓰면 이 컨트롤러의 위치가 특정된다. */
			return(rc);

		/* Skip the number of available entries */
		rc = add_dword(&pFill, 0, &usedbytes, &available);
		if (rc)
			/* [한국어] **네 종류 구간 개수 자리를 0 으로 비워 둔다** — 실제 값은 각 자원을
			 * 다 쓴 뒤 위에서 기억해 둔 주소로 돌아가 채운다. */
			return(rc);

		/* Figure out memory Available */

		resNode = ctrl->mem_head;
/* [한국어] 메모리 자원부터 센다. */

		loop = 0;
/* [한국어] 구간 수를 0 부터 센다. */

		while (resNode) {
			/* [한국어] 구간 하나를 셀 때마다 늘린다. */
			loop++;

			/* base */
			rc = add_dword(&pFill, resNode->base, &usedbytes, &available);
			if (rc)
				/* [한국어] 시작 주소를 쓰지 못하면 중단한다. */
				return(rc);

			/* length */
			rc = add_dword(&pFill, resNode->length, &usedbytes, &available);
			if (rc)
				/* [한국어] 길이도 마찬가지다. */
				return(rc);

			resNode = resNode->next;
		/* [한국어] 다음 구간으로 넘어간다. */
		}

		/* Fill in the number of entries */
		p_ev_ctrl->mem_avail = loop;

		/* Figure out prefetchable memory Available */

		resNode = ctrl->p_mem_head;
/* [한국어] 이제 프리페치 메모리를 같은 방식으로 센다. */

		loop = 0;
/* [한국어] 카운터를 다시 0 으로. */

		while (resNode) {
			/* [한국어] 구간 하나. */
			loop++;

			/* base */
			rc = add_dword(&pFill, resNode->base, &usedbytes, &available);
			if (rc)
				/* [한국어] 시작 주소. */
				return(rc);

			/* length */
			rc = add_dword(&pFill, resNode->length, &usedbytes, &available);
			if (rc)
				/* [한국어] 길이. */
				return(rc);

			resNode = resNode->next;
		/* [한국어] 다음 구간. */
		}

		/* Fill in the number of entries */
		p_ev_ctrl->p_mem_avail = loop;

		/* Figure out IO Available */

		resNode = ctrl->io_head;
/* [한국어] I/O 자원도 같다. */

		loop = 0;
/* [한국어] 카운터 초기화. */

		while (resNode) {
			/* [한국어] 구간 하나. */
			loop++;

			/* base */
			rc = add_dword(&pFill, resNode->base, &usedbytes, &available);
			if (rc)
				/* [한국어] 시작 주소. */
				return(rc);

			/* length */
			rc = add_dword(&pFill, resNode->length, &usedbytes, &available);
			if (rc)
				/* [한국어] 길이. */
				return(rc);

			resNode = resNode->next;
		/* [한국어] 다음 구간. */
		}

		/* Fill in the number of entries */
		p_ev_ctrl->io_avail = loop;

		/* Figure out bus Available */

		resNode = ctrl->bus_head;
/* [한국어] 마지막으로 버스 번호 구간이다. */

		loop = 0;
/* [한국어] 카운터 초기화. */

		while (resNode) {
			/* [한국어] 구간 하나. */
			loop++;

			/* base */
			rc = add_dword(&pFill, resNode->base, &usedbytes, &available);
			if (rc)
				/* [한국어] 시작 주소. */
				return(rc);

			/* length */
			rc = add_dword(&pFill, resNode->length, &usedbytes, &available);
			if (rc)
				/* [한국어] 길이. */
				return(rc);

			resNode = resNode->next;
		/* [한국어] 다음 구간. */
		}

		/* Fill in the number of entries */
		p_ev_ctrl->bus_avail = loop;

		ctrl = ctrl->next;
	/* [한국어] 이 컨트롤러 항목이 완성됐다. 다음 컨트롤러로 넘어간다. */
	}

	p_EV_header->num_of_ctrl = numCtrl;
/* [한국어] **모든 컨트롤러를 쓴 뒤에야** 실제 개수를 머리에 채운다 —
 * 미리 알 수 없어 나중에 돌아와 적는 방식이다. */

	/* Now store the EV */

	temp_dword = usedbytes;
/* [한국어] 실제로 쓴 바이트 수를 BIOS 에 알린다. */

	rc = access_EV(WRITE_EV, "CQTHPS", (u8 *) buffer, &temp_dword);
/* [한국어] 직렬화한 내용을 NVRAM 에 쓴다. */

	dbg("usedbytes = 0x%x, length = 0x%x\n", usedbytes, temp_dword);
/* [한국어] 쓴 크기와 BIOS 가 돌려준 크기를 함께 기록에 남긴다. */

	evbuffer_length = temp_dword;
/* [한국어] 그 크기를 기억해 둔다. */

	if (rc) {
		/* [한국어] 쓰기가 실패했으면 그 사실을 남긴다. */
		err(msg_unable_to_save);
		return(1);
	}

	return(0);
}


/* [한국어]
 * compaq_nvram_init - INT 15h 진입점의 가상 주소를 계산해 둔다
 *
 * @rom_start: 매핑된 시스템 ROM 의 시작 주소. NULL 일 수 있다.
 *
 * 이 파일의 다른 모든 NVRAM 접근이 이 값에 기댄다.
 *
 * 계산이 물리 주소를 가상 주소로 옮기는 것이다. INT 15h 진입점의 **물리**
 * 주소는 상수로 알려져 있고, ROM 이 매핑된 가상 주소도 알고 있으므로,
 * 그 둘의 차이를 이용해 진입점의 가상 주소를 구한다.
 *
 * rom_start 가 NULL 이면 아무것도 하지 않는다. 그러면 진입점이 NULL 로
 * 남아 access_EV() 가 -ENODEV 를 돌려주며, 결과적으로 NVRAM 기능이
 * 조용히 꺼진다.
 *
 * 실행 컨텍스트: 드라이버 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값이 없어 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   cpqphp_core.c 의 초기화 → [이 함수]
 */
void compaq_nvram_init(void __iomem *rom_start)
{
	if (rom_start)
		compaq_int15_entry_point = (rom_start + ROM_INT15_PHY_ADDR - ROM_PHY_ADDR);
/* [한국어] 여기까지 오면 표가 NVRAM 에 저장됐다. */

	dbg("int15 entry  = %p\n", compaq_int15_entry_point);
/* [한국어] 다음 부팅에서 compaq_nvram_load() 가 이것을 되살린다. */
}


/* [한국어]
 * compaq_nvram_load - NVRAM 의 표에서 이 컨트롤러의 자원을 되살린다
 *
 * @rom_start: 매핑된 시스템 ROM 의 시작 주소.
 * @ctrl: 자원을 채워 넣을 컨트롤러.
 * @return: 0 = 성공, 1 또는 2 = 표를 쓸 수 없음.
 *
 * 이 파일의 주된 진입점이며, 직렬화된 표를 다시 연결 목록으로 푸는 일을 한다.
 *
 * **NVRAM 을 한 번만 읽는다.** evbuffer_init 이 그것을 기억하며, 컨트롤러가
 * 여럿이어도 첫 호출에서만 실제로 BIOS 를 부른다. 그 뒤로는 파일 정적
 * 버퍼에 담긴 내용을 파싱할 뿐이다.
 *
 * 읽기가 실패하면 버퍼를 0 으로 지운다. 쓰레기를 표로 해석하지 않기 위해서다.
 *
 * 판본 검사가 방어적이다. 위 상류 주석이 그 사정을 밝히는데, 이 드라이버의
 * 1.0 판이 지원하지 않는 하드웨어에서 NVRAM 에 잘못된 내용을 써 둔 적이
 * 있어, 그 경우를 걸러야 한다.
 *
 * 이 컨트롤러를 찾는 방법이 선형 탐색이다. 표의 항목을 하나씩 지나며
 * 버스·장치·기능이 맞는 것을 찾는데, 각 항목의 크기가 자원 구간 수에
 * 따라 달라 그만큼씩 건너뛴다.
 *
 * 경계 검사가 곳곳에 있다. NVRAM 의 내용을 신뢰할 수 없어, 포인터가
 * 읽어 온 길이를 넘으면 2 로 물러난다.
 *
 * 찾은 뒤에는 네 종류 자원을 각각 연결 목록으로 만들어 컨트롤러에 매달고,
 * 마지막에 정렬·병합해 겹치는 구간을 정리한다.
 *
 * 실행 컨텍스트: 컨트롤러 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: 표가 범위를 벗어나면 2, 판본이 맞지 않거나 자원 정리가
 * 실패하면 1.
 *
 * 호출 체인:
 *   cpqphp_core.c 의 컨트롤러 초기화 → [이 함수]
 *     → load_HRT() → kmalloc() → cpqhp_resource_sort_and_combine()
 */
int compaq_nvram_load(void __iomem *rom_start, struct controller *ctrl)
{
	u8 bus, device, function;
	u8 nummem, numpmem, numio, numbus;
	/* [한국어] 각 단계의 결과. */
	u32 rc;
	/* [한국어] 버퍼를 바이트 단위로 훑을 포인터. 범위 검사의 기준이기도 하다. */
	u8 *p_byte;
	/* [한국어] 메모리 구간 노드. */
	struct pci_resource *mem_node;
	/* [한국어] 프리페치 메모리 구간 노드. */
	struct pci_resource *p_mem_node;
	/* [한국어] I/O 구간 노드. */
	struct pci_resource *io_node;
	/* [한국어] 버스 번호 구간 노드. */
	struct pci_resource *bus_node;
	/* [한국어] 지금 보고 있는 컨트롤러 항목. */
	struct ev_hrt_ctrl *p_ev_ctrl;
	/* [한국어] 표의 머리. */
	struct ev_hrt_header *p_EV_header;
/* [한국어] **NVRAM 을 한 번만 읽는다.** */

	if (!evbuffer_init) {
		/* Read the resource list information in from NVRAM */
		if (load_HRT(rom_start))
			memset(evbuffer, 0, 1024);
/* [한국어] 읽기가 실패했으면 버퍼를 0 으로 지운다 — 쓰레기를 표로 해석하지 않기 위해서다. */

		evbuffer_init = 1;
	/* [한국어] 이제 다른 컨트롤러가 불러도 다시 읽지 않는다. */
	}

	/* If we saved information in NVRAM, use it now */
	p_EV_header = (struct ev_hrt_header *) evbuffer;

	/* The following code is for systems where version 1.0 of this
	 * driver has been loaded, but doesn't support the hardware.
	 * In that case, the driver would incorrectly store something
	 * in NVRAM.
	 */
	if ((p_EV_header->Version == 2) ||
	    ((p_EV_header->Version == 1) && !ctrl->push_flag)) {
		p_byte = &(p_EV_header->next);
/* [한국어] 버퍼의 시작이 곧 표의 머리다. */

		p_ev_ctrl = (struct ev_hrt_ctrl *) &(p_EV_header->next);
/* [한국어] 판본 검사다. 위 상류 주석이 그 사정을 밝힌다 — 이 드라이버의 1.0 판이
 * 지원하지 않는 하드웨어에서 NVRAM 에 잘못된 내용을 써 둔 적이 있다. */

		p_byte += 3;
/* [한국어] 머리의 next 자리가 첫 컨트롤러 항목의 시작이다. */

		if (p_byte > ((u8 *)p_EV_header + evbuffer_length))
			/* [한국어] **3바이트를 미리 건너뛴 자리** 가 버퍼 끝을 넘으면 표가 잘린 것이다 —
			 * 버스·장치·기능 세 바이트를 읽기 전에 확인한다. */
			return 2;

		bus = p_ev_ctrl->bus;
		/* [한국어] 장치 번호와, */
		device = p_ev_ctrl->device;
		/* [한국어] 기능 번호를 읽는다. */
		function = p_ev_ctrl->function;

		while ((bus != ctrl->bus) ||
		       /* [한국어] 장치 번호가 다르거나, */
		       (device != PCI_SLOT(ctrl->pci_dev->devfn)) ||
		       (function != PCI_FUNC(ctrl->pci_dev->devfn))) {
			nummem = p_ev_ctrl->mem_avail;
			/* [한국어] 프리페치 메모리 구간 수와, */
			numpmem = p_ev_ctrl->p_mem_avail;
			/* [한국어] I/O 구간 수, */
			numio = p_ev_ctrl->io_avail;
			/* [한국어] 버스 구간 수를 읽어 — */
			numbus = p_ev_ctrl->bus_avail;
/* [한국어] 그만큼 건너뛸 거리를 계산한다. */

			p_byte += 4;
/* [한국어] **항목 크기가 자원 구간 수에 따라 달라** 이렇게 세어 가며 건너뛴다. */

			if (p_byte > ((u8 *)p_EV_header + evbuffer_length))
				/* [한국어] 건너뛴 자리가 버퍼를 넘으면 물러난다. */
				return 2;

			/* Skip forward to the next entry */
			p_byte += (nummem + numpmem + numio + numbus) * 8;

			if (p_byte > ((u8 *)p_EV_header + evbuffer_length))
				/* [한국어] 그 자리도 확인한다 — NVRAM 의 내용을 매 단계 검증한다. */
				return 2;

			p_ev_ctrl = (struct ev_hrt_ctrl *) p_byte;
/* [한국어] 다음 항목의 시작이다. */

			p_byte += 3;
/* [한국어] 그 항목의 세 바이트를 읽기 전에 다시 확인하고, */

			if (p_byte > ((u8 *)p_EV_header + evbuffer_length))
				/* [한국어] 넘었으면 물러난다. */
				return 2;

			bus = p_ev_ctrl->bus;
			/* [한국어] 장치 번호와, */
			device = p_ev_ctrl->device;
			/* [한국어] 기능 번호를 읽어 다시 비교한다. */
			function = p_ev_ctrl->function;
		/* [한국어] 찾을 때까지 반복한다. */
		}

		nummem = p_ev_ctrl->mem_avail;
		/* [한국어] 프리페치 메모리 구간 수와, */
		numpmem = p_ev_ctrl->p_mem_avail;
		/* [한국어] I/O 구간 수, */
		numio = p_ev_ctrl->io_avail;
		/* [한국어] 버스 구간 수를 읽어 둔다. */
		numbus = p_ev_ctrl->bus_avail;
/* [한국어] 이제 자원 데이터가 시작하는 자리로 간다. */

		p_byte += 4;
/* [한국어] 항목의 next 자리부터가 자원 데이터다. */

		if (p_byte > ((u8 *)p_EV_header + evbuffer_length))
			/* [한국어] 그 자리가 버퍼를 넘으면 물러난다. */
			return 2;

		while (nummem--) {
			/* [한국어] 이 종류의 자원 구간을 하나씩 되살린다. */
			mem_node = kmalloc_obj(struct pci_resource);

			/* [한국어] 노드를 잡지 못하면 — */
			if (!mem_node)
				break;
/* [한국어] 루프를 빠져나간다. 지금까지 만든 노드는 목록에 남는다. */

			mem_node->base = *(u32 *)p_byte;
			dbg("mem base = %8.8x\n", mem_node->base);
			/* [한국어] 읽어 온 시작 주소를 기록에 남긴다. */
			p_byte += 4;
/* [한국어] 다음 dword 로 넘어간다. */

			if (p_byte > ((u8 *)p_EV_header + evbuffer_length)) {
				/* [한국어] 범위를 벗어났으면 방금 잡은 노드를 놓고, */
				kfree(mem_node);
				return 2;
			/* [한국어] 물러난다. NVRAM 의 길이 정보를 신뢰할 수 없어 매 dword 마다 확인한다. */
			}

			mem_node->length = *(u32 *)p_byte;
			/* [한국어] 읽어 온 길이를 기록에 남긴다. */
			dbg("mem length = %8.8x\n", mem_node->length);
			/* [한국어] 다음 dword 로 넘어간다. */
			p_byte += 4;

			/* [한국어] 여기서도 범위를 확인하고 벗어났으면 노드를 놓는다. */
			if (p_byte > ((u8 *)p_EV_header + evbuffer_length)) {
				kfree(mem_node);
				/* [한국어] 그리고 물러난다. */
				return 2;
			}

			mem_node->next = ctrl->mem_head;
			/* [한국어] **목록 앞에 붙인다** — 그래서 되살린 순서가 뒤집히지만,
			 * 아래 정렬·병합이 그것을 바로잡는다. */
			ctrl->mem_head = mem_node;
		/* [한국어] 이 구간 처리 끝. */
		}

		while (numpmem--) {
			/* [한국어] 이 종류의 자원 구간을 하나씩 되살린다. */
			p_mem_node = kmalloc_obj(struct pci_resource);

			/* [한국어] 노드를 잡지 못하면 — */
			if (!p_mem_node)
				break;
/* [한국어] 루프를 빠져나간다. 지금까지 만든 노드는 목록에 남는다. */

			p_mem_node->base = *(u32 *)p_byte;
			dbg("pre-mem base = %8.8x\n", p_mem_node->base);
			/* [한국어] 읽어 온 시작 주소를 기록에 남긴다. */
			p_byte += 4;
/* [한국어] 다음 dword 로 넘어간다. */

			if (p_byte > ((u8 *)p_EV_header + evbuffer_length)) {
				/* [한국어] 범위를 벗어났으면 방금 잡은 노드를 놓고, */
				kfree(p_mem_node);
				return 2;
			/* [한국어] 물러난다. NVRAM 의 길이 정보를 신뢰할 수 없어 매 dword 마다 확인한다. */
			}

			p_mem_node->length = *(u32 *)p_byte;
			/* [한국어] 읽어 온 길이를 기록에 남긴다. */
			dbg("pre-mem length = %8.8x\n", p_mem_node->length);
			/* [한국어] 다음 dword 로 넘어간다. */
			p_byte += 4;

			/* [한국어] 여기서도 범위를 확인하고 벗어났으면 노드를 놓는다. */
			if (p_byte > ((u8 *)p_EV_header + evbuffer_length)) {
				kfree(p_mem_node);
				/* [한국어] 그리고 물러난다. */
				return 2;
			}

			p_mem_node->next = ctrl->p_mem_head;
			/* [한국어] **목록 앞에 붙인다** — 그래서 되살린 순서가 뒤집히지만,
			 * 아래 정렬·병합이 그것을 바로잡는다. */
			ctrl->p_mem_head = p_mem_node;
		/* [한국어] 이 구간 처리 끝. */
		}

		while (numio--) {
			/* [한국어] 이 종류의 자원 구간을 하나씩 되살린다. */
			io_node = kmalloc_obj(struct pci_resource);

			/* [한국어] 노드를 잡지 못하면 — */
			if (!io_node)
				break;
/* [한국어] 루프를 빠져나간다. 지금까지 만든 노드는 목록에 남는다. */

			io_node->base = *(u32 *)p_byte;
			dbg("io base = %8.8x\n", io_node->base);
			/* [한국어] 읽어 온 시작 주소를 기록에 남긴다. */
			p_byte += 4;
/* [한국어] 다음 dword 로 넘어간다. */

			if (p_byte > ((u8 *)p_EV_header + evbuffer_length)) {
				/* [한국어] 범위를 벗어났으면 방금 잡은 노드를 놓고, */
				kfree(io_node);
				return 2;
			/* [한국어] 물러난다. NVRAM 의 길이 정보를 신뢰할 수 없어 매 dword 마다 확인한다. */
			}

			io_node->length = *(u32 *)p_byte;
			/* [한국어] 읽어 온 길이를 기록에 남긴다. */
			dbg("io length = %8.8x\n", io_node->length);
			/* [한국어] 다음 dword 로 넘어간다. */
			p_byte += 4;

			/* [한국어] 여기서도 범위를 확인하고 벗어났으면 노드를 놓는다. */
			if (p_byte > ((u8 *)p_EV_header + evbuffer_length)) {
				kfree(io_node);
				/* [한국어] 그리고 물러난다. */
				return 2;
			}

			io_node->next = ctrl->io_head;
			/* [한국어] **목록 앞에 붙인다** — 그래서 되살린 순서가 뒤집히지만,
			 * 아래 정렬·병합이 그것을 바로잡는다. */
			ctrl->io_head = io_node;
		/* [한국어] 이 구간 처리 끝. */
		}

		while (numbus--) {
			/* [한국어] 이 종류의 자원 구간을 하나씩 되살린다. */
			bus_node = kmalloc_obj(struct pci_resource);

			/* [한국어] 노드를 잡지 못하면 — */
			if (!bus_node)
				break;
/* [한국어] 루프를 빠져나간다. 지금까지 만든 노드는 목록에 남는다. */

			bus_node->base = *(u32 *)p_byte;
			p_byte += 4;
/* [한국어] 읽어 온 시작 주소를 기록에 남긴다. */

			/* [한국어] 다음 dword 로 넘어간다. */
			if (p_byte > ((u8 *)p_EV_header + evbuffer_length)) {
				kfree(bus_node);
				/* [한국어] 범위를 벗어났으면 방금 잡은 노드를 놓고, */
				return 2;
			}
/* [한국어] 물러난다. NVRAM 의 길이 정보를 신뢰할 수 없어 매 dword 마다 확인한다. */

			bus_node->length = *(u32 *)p_byte;
			p_byte += 4;
/* [한국어] 읽어 온 길이를 기록에 남긴다. */

			/* [한국어] 다음 dword 로 넘어간다. */
			if (p_byte > ((u8 *)p_EV_header + evbuffer_length)) {
				kfree(bus_node);
				/* [한국어] 여기서도 범위를 확인하고 벗어났으면 노드를 놓는다. */
				return 2;
			}
/* [한국어] 그리고 물러난다. */

			bus_node->next = ctrl->bus_head;
			ctrl->bus_head = bus_node;
		}
/* [한국어] **목록 앞에 붙인다** — 그래서 되살린 순서가 뒤집히지만,
 * 아래 정렬·병합이 그것을 바로잡는다. */

		/* [한국어] 이 구간 처리 끝. */
		/* If all of the following fail, we don't have any resources for
		 * hot plug add
		 */
		rc = 1;
		rc &= cpqhp_resource_sort_and_combine(&(ctrl->mem_head));
		/* [한국어] 프리페치 메모리도, */
		rc &= cpqhp_resource_sort_and_combine(&(ctrl->p_mem_head));
		/* [한국어] I/O 도, */
		rc &= cpqhp_resource_sort_and_combine(&(ctrl->io_head));
		/* [한국어] 버스 번호도 정렬·병합한다. **되살린 순서가 뒤집혀 있어** 이 단계가 필요하다. */
		rc &= cpqhp_resource_sort_and_combine(&(ctrl->bus_head));

		if (rc)
			/* [한국어] 하나라도 실패하면 그 값을 올려보낸다 — rc 가 AND 로 합쳐져 있어
			 * 모두 성공해야 0 이 아닌 값이 남는다. */
			return(rc);
	} else {
		if ((evbuffer[0] != 0) && (!ctrl->push_flag))
			/* [한국어] 판본이 맞지 않는데 버퍼에 뭔가 들어 있으면 실패로 답한다 —
			 * 1.0 판이 써 둔 잘못된 내용일 수 있다. */
			return 1;
	/* [한국어] 판본 검사 끝. */
	}

	return 0;
}


/* [한국어]
 * compaq_nvram_store - 지금의 자원 목록을 NVRAM 에 저장한다
 *
 * @rom_start: 매핑된 시스템 ROM 의 시작 주소.
 * @return: 0 = 성공, 1 또는 -ENODEV.
 *
 * compaq_nvram_load() 의 짝이며, 실제 직렬화는 store_HRT() 가 한다.
 *
 * **evbuffer_init 을 먼저 확인하는 것** 이 요점이다. 한 번도 읽어 온 적이
 * 없으면 저장도 하지 않는데, 그런 상태에서 쓰면 NVRAM 에 있던 다른 정보를
 * 덮을 수 있기 때문이다.
 *
 * 그래서 초기값이 1(실패)이다. 읽어 온 적이 없으면 그 값이 그대로 나가,
 * 호출자가 "저장하지 못했다" 로 읽는다.
 *
 * rom_start 가 NULL 이면 -ENODEV 다 — 이 파일의 다른 함수들과 달리
 * 음수 errno 를 쓰는데, 반환값 관용이 한 파일 안에서 섞여 있다.
 *
 * 실행 컨텍스트: 드라이버 정리 또는 자원 변경 후. 프로세스 컨텍스트.
 *
 * 에러 경로: ROM 이 없으면 -ENODEV, 저장이 실패하면 기록을 남기고 1.
 *
 * 호출 체인:
 *   cpqphp_core.c 의 정리 경로 → [이 함수] → store_HRT()
 */
int compaq_nvram_store(void __iomem *rom_start)
{
	int rc = 1;

	if (rom_start == NULL)
		/* [한국어] ROM 이 없으면 이 파일의 다른 함수들과 달리 음수 errno 를 쓴다 —
		 * 반환값 관용이 한 파일 안에서 섞여 있다. */
		return -ENODEV;

	if (evbuffer_init) {
		/* [한국어] **한 번도 읽어 온 적이 없으면 저장도 하지 않는다** — 그런 상태에서 쓰면
		 * NVRAM 에 있던 다른 정보를 덮을 수 있다. */
		rc = store_HRT(rom_start);
		/* [한국어] 저장이 실패하면, */
		if (rc)
			/* [한국어] 그 사실을 남긴다. 반환값은 아래에서 그대로 나간다. */
			err(msg_unable_to_save);
	}
	return rc;
/* [한국어] 읽어 온 적이 없으면 초기값 1 이 그대로 나가, 호출자가 '저장하지 못했다' 로 읽는다. */
}

