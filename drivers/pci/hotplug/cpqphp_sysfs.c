// SPDX-License-Identifier: GPL-2.0+
/*
 * Compaq Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001,2003 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>
 *
 */

/* [한국어] THIS_MODULE — 아래 file_operations 의 owner 에 쓴다. */
/*
 * [한국어 설명] cpqphp 의 자원 목록을 debugfs 로 보여 주는 파일 (cpqphp_sysfs.c)
 *
 * === 파일의 역할 ===
 * Compaq 핫플러그 드라이버가 굴리는 자원 목록의 현재 상태를 사람이 볼 수
 * 있게 debugfs 파일 하나로 내보낸다. 이 드라이버는 PCI 코어의 자원
 * 할당기를 쓰지 않고 펌웨어(HRT, Hot Plug Resource Table)에서 통보받은
 * 자원을 자기 연결 목록으로 관리하는데, 그 목록이 커널 안에만 있어
 * 바깥에서 볼 방법이 없다. 이 파일이 그 창이다.
 * 보여 주는 것은 두 가지이며 그 대비가 이 파일의 핵심이다.
 * show_ctrl() 이 컨트롤러에 매달린 **자유 목록**(아직 나눠 주지 않은 자원)을
 * 찍고, show_dev() 가 각 슬롯의 함수에 매달린 **배정 목록**(이미 나눠 준
 * 자원)을 찍는다. 둘 다 같은 struct pci_resource 연결 목록이며, 어디에
 * 매달렸느냐가 뜻을 정한다.
 * 자원 종류가 넷(메모리, 프리페치 가능 메모리, I/O, 버스 번호)인데 모두
 * 같은 자료구조라, 두 함수 안에서 네 블록의 코드가 목록 머리만 다르고
 * 완전히 같다. 그 반복이 이 드라이버의 자원 관리 방식을 그대로 보여 준다.
 * 파일 이름이 sysfs 인데 실제로는 debugfs 를 쓴다. include 에 남아 있는
 * linux/proc_fs.h 와 함께, 이 정보의 출구가 /proc → sysfs → debugfs 로
 * 옮겨 다닌 흔적으로 보인다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 cpqphp 의 곁가지다. 자원을 만들거나 고치지 않고 읽기만 한다.
 *   모듈 초기화 → cpqhp_initialize_debugfs() → /sys/kernel/debug/cpqhp 생성
 *   컨트롤러 등록 → cpqhp_create_debugfs_files() → 그 아래 장치 이름 파일 생성
 *   사용자가 파일을 열면 → open() 이 16KB 버퍼를 잡고 내용을 **통째로** 만든다
 *     → spew_debug_info() → show_ctrl() + show_dev()
 *   읽기 → read() 는 그 버퍼에서 복사만 한다
 *   닫기 → release() 가 두 할당을 놓는다
 * 열 때 내용을 다 만들어 두는 설계라, read 가 여러 번 불려도 일관된
 * 스냅숏을 보고 read 경로가 헬퍼 한 줄로 끝난다. 대가는 열 때마다 16KB 다.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. 파일 열기가 cpqphp_mutex 를
 * 잡고 큰 할당을 하므로 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: VFS 의 file_operations 규약과 debugfs.
 * 옆쪽: cpqphp.h 의 struct controller(자유 목록 넷을 들고 있다),
 * struct slot, struct pci_func(배정 목록 넷), struct pci_resource,
 * 그리고 cpqphp_pci.c 의 cpqhp_slot_find().
 * 아래쪽: sprintf(크기 검사 없음 — index = 11 상한이 그 대비책이다),
 * fixed_size_llseek(), simple_read_from_buffer().
 * 이 파일은 cpqphp 의 다른 파일들이 만든 자료구조를 읽기만 하며,
 * 그쪽에 아무것도 되돌려 주지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - show_ctrl(): 컨트롤러의 자유 목록 넷을 찍는다. index = 11 상한이
 *   유일한 안전장치인데, sprintf 가 버퍼 크기를 검사하지 않으므로 목록이
 *   순환하거나 길어지면 넘치기 때문이다.
 * - show_dev(): 슬롯마다 함수 0 을 찾아 배정 목록 넷을 찍는다.
 *   cpqhp_slot_find() 가 NULL 을 주면 continue 가 아니라 break 라,
 *   빈 슬롯 하나가 그 뒤 슬롯의 출력을 전부 막는다.
 * - spew_debug_info(): 둘을 이어 붙이려는 함수. 산술이 의도와 맞지 않는다
 *   (아래 상류 코드 관찰 참고).
 * - open(): 내용을 미리 만들어 두는 설계의 중심. retval 초기값 -ENOMEM 과
 *   exit 라벨의 조합으로 두 할당 실패를 한 자리에서 처리한다.
 * - lseek() / read() / release(): 모두 표준 헬퍼에 맡기거나 두 줄로 끝난다.
 *   open() 이 무거운 만큼 나머지가 가볍다.
 * - struct ctrl_dbg: 열린 파일 하나의 상태. 크기와 버퍼 두 필드뿐이다.
 * - cpqhp_initialize/shutdown_debugfs(), cpqhp_create/remove_debugfs_files():
 *   디렉터리 하나와 컨트롤러별 파일의 생성·해제.
 *
 * === 상류 코드 관찰 ===
 * 코드는 고치지 않고 사실만 기록한다.
 * - spew_debug_info() 의 두 줄을 풀어 보면, show_dev() 의 쓰기 시작 위치가
 *   첫 출력 바로 뒤가 아니라 &data[size - len1] — 버퍼 거의 끝이 된다.
 *   반환값도 두 길이의 합이 아니라 차(len1 - len2)이며, len2 가 크면
 *   음수가 되어 그대로 dbg->size 에 들어간다. MAX_OUTPUT 이 16KB 로 넉넉해
 *   실제로 넘치지 않는 것으로 보인다.
 * - cpqphp_mutex 는 파일 열기끼리만 직렬화한다. 읽는 대상인 자유 목록을
 *   고치는 쪽(cpqphp_ctrl.c 의 자원 배정)은 이 뮤텍스를 알지 못한다.
 * - cpqhp_shutdown_debugfs() 가 root 를 NULL 로 되돌리지 않아, 모듈을
 *   다시 넣으면 해제된 dentry 를 그대로 쓰게 된다.
 *   cpqhp_remove_debugfs_files() 가 ctrl->dentry 를 되돌리는 것과 대비된다.
 * - show_ctrl() 의 첫 sprintf 만 out 이 아니라 buf 를 대상으로 쓴다.
 *   그 시점에는 둘이 같아 결과는 같지만 표기가 나머지 셋과 다르다.
 * - linux/proc_fs.h 를 비롯한 여러 include 가 이 파일에서 쓰이지 않는다.
 *
 * === NVMe 관점 ===
 * 접점이 없다. cpqphp 는 2001년 Compaq 서버의 PCI(PCIe 이전) 핫플러그
 * 드라이버이고, NVMe SSD 가 그 슬롯에 꽂힐 일은 없다.
 * 다만 이 파일이 보여 주는 대비 — 같은 자료구조가 "자유" 와 "배정" 두 뜻을
 * 갖고, 매달린 위치가 그것을 정한다 — 는 자원 관리의 흔한 형태다.
 * 지금의 PCI 코어도 kernel/resource.c 의 트리로 같은 구분을 하며,
 * NVMe 컨트롤러의 BAR 이 배정되는 것도 그 트리 위에서 일어난다.
 */

#include <linux/module.h>
/* [한국어] 기본 커널 유틸. */
#include <linux/kernel.h>
/* [한국어] kmalloc()/kfree(). */
#include <linux/slab.h>
/* [한국어] 기본 타입 정의. */
#include <linux/types.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. 예전에 이 정보가
 * /proc 에 있었던 흔적으로 보인다 — 지금은 debugfs 를 쓴다. */
#include <linux/proc_fs.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/workqueue.h>
/* [한국어] dev_name() 을 쓰기 위한 경로. */
#include <linux/pci.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/pci_hotplug.h>
/* [한국어] DEFINE_MUTEX. */
#include <linux/mutex.h>
/* [한국어] debugfs_create_dir/file/remove. */
#include <linux/debugfs.h>
/* [한국어] struct controller, struct slot, struct pci_func, struct pci_resource,
 * 그리고 cpqhp_slot_find(). */
#include "cpqphp.h"

/* [한국어] 파일을 열 때의 버퍼 준비를 직렬화한다. 여러 프로세스가 같은 debugfs
 * 파일을 동시에 열 수 있고, 그때마다 큰 버퍼를 할당하기 때문이다.
 * [상류 코드 관찰] 정작 보호하는 대상인 ctrl 의 자유 목록을 읽는
 * spew_debug_info() 는 이 락 안에서 불리지만, 그 목록을 고치는 쪽
 * (cpqphp_ctrl.c 의 자원 배정)은 이 뮤텍스를 알지 못한다. */
static DEFINE_MUTEX(cpqphp_mutex);
/* [한국어]
 * show_ctrl - 컨트롤러가 아직 나눠 주지 않은 자원 네 종류를 찍는다
 *
 * @ctrl: 대상 컨트롤러.
 * @buf: 출력 버퍼.
 * @return: 쓴 바이트 수.
 *
 * 이 드라이버는 PCI 코어의 자원 할당기를 쓰지 않고 자기 자유 목록을 굴린다.
 * 그 목록의 현재 상태를 사람이 볼 수 있게 하는 것이 이 함수다.
 *
 * 네 목록(메모리, 프리페치 가능 메모리, I/O, 버스 번호)이 모두 같은
 * struct pci_resource 연결 목록이라, 네 블록의 코드가 목록 머리만 다르고
 * 완전히 같다. 자원 종류가 달라도 같은 자료구조로 다루는 것이 이 드라이버의
 * 자원 관리 방식이며, 그 사실이 이 함수의 반복에 그대로 드러난다.
 *
 * index = 11 상한이 이 함수의 유일한 안전장치다. sprintf 는 버퍼 크기를
 * 검사하지 않으므로, 목록이 순환하거나 예상보다 길면 버퍼를 넘는다.
 * 항목마다 약 40바이트를 쓰고 네 목록에 11개씩이면 넉넉히 MAX_OUTPUT 안이다.
 *
 * 실행 컨텍스트: debugfs 파일 열기. cpqphp_mutex 아래이며 프로세스 컨텍스트다.
 *
 * 에러 경로: 없다. 버퍼가 모자라도 알 방법이 없다.
 *
 * 호출 체인:
 *   open() → spew_debug_info() → [이 함수] → sprintf()
 */
static int show_ctrl(struct controller *ctrl, char *buf)
{
	/* [한국어] 출력 커서. 쓴 만큼 앞으로 밀어 나간다. */
	char *out = buf;
	/* [한국어] 루프 상한 카운터. */
	int index;
	/* [한국어] 자유 목록을 따라갈 커서. */
	struct pci_resource *res;

	/* [한국어] 메모리 자유 목록의 제목을 찍는다.
	 * [상류 코드 관찰] 이 한 줄만 out 이 아니라 buf 에 쓴다. 이 시점에는
	 * 둘이 같은 값이라 결과는 같지만, 아래 세 제목과 표기가 다르다. */
	out += sprintf(buf, "Free resources: memory\n");
	/* [한국어] 상한 11 로 초기화한다. */
	index = 11;
	/* [한국어] 메모리 자유 목록의 머리. */
	res = ctrl->mem_head;
	/* [한국어] 자유 목록을 따라가며 항목마다 시작 주소와 길이를 찍는다.
	 * index 를 11 로 두고 후위 감소로 세는 것이 상한이다 — 목록이 순환하거나
	 * 예상보다 길어도 버퍼를 넘지 않게 하려는 것으로, 이 파일이 크기 검사를
	 * 하지 않는 sprintf 를 쓰기 때문에 필요한 방어다. */
	while (res && index--) {
		/* [한국어] 시작 주소와 길이를 8자리 16진수로 찍는다. */
		out += sprintf(out, "start = %8.8x, length = %8.8x\n", res->base, res->length);
		/* [한국어] 다음 항목으로. */
		res = res->next;
	}
	/* [한국어] 프리페치 가능 메모리 제목. */
	out += sprintf(out, "Free resources: prefetchable memory\n");
	/* [한국어] 상한을 다시 세운다. */
	index = 11;
	/* [한국어] 그 목록의 머리. */
	res = ctrl->p_mem_head;
	/* [한국어] 자유 목록을 따라가며 항목마다 시작 주소와 길이를 찍는다.
	 * index 를 11 로 두고 후위 감소로 세는 것이 상한이다 — 목록이 순환하거나
	 * 예상보다 길어도 버퍼를 넘지 않게 하려는 것으로, 이 파일이 크기 검사를
	 * 하지 않는 sprintf 를 쓰기 때문에 필요한 방어다. */
	while (res && index--) {
		out += sprintf(out, "start = %8.8x, length = %8.8x\n", res->base, res->length);
		res = res->next;
	}
	/* [한국어] I/O 제목. */
	out += sprintf(out, "Free resources: IO\n");
	/* [한국어] 상한. */
	index = 11;
	/* [한국어] I/O 자유 목록. */
	res = ctrl->io_head;
	/* [한국어] 자유 목록을 따라가며 항목마다 시작 주소와 길이를 찍는다.
	 * index 를 11 로 두고 후위 감소로 세는 것이 상한이다 — 목록이 순환하거나
	 * 예상보다 길어도 버퍼를 넘지 않게 하려는 것으로, 이 파일이 크기 검사를
	 * 하지 않는 sprintf 를 쓰기 때문에 필요한 방어다. */
	while (res && index--) {
		out += sprintf(out, "start = %8.8x, length = %8.8x\n", res->base, res->length);
		res = res->next;
	}
	/* [한국어] 버스 번호 제목. 자원 네 종류가 모두 같은 자료구조로 관리된다는 것이
	 * 이 드라이버의 특징이다 — 메모리도 I/O 도 버스 번호도 struct pci_resource
	 * 목록 하나로 다룬다. */
	out += sprintf(out, "Free resources: bus numbers\n");
	/* [한국어] 상한. */
	index = 11;
	/* [한국어] 버스 번호 자유 목록. */
	res = ctrl->bus_head;
	/* [한국어] 자유 목록을 따라가며 항목마다 시작 주소와 길이를 찍는다.
	 * index 를 11 로 두고 후위 감소로 세는 것이 상한이다 — 목록이 순환하거나
	 * 예상보다 길어도 버퍼를 넘지 않게 하려는 것으로, 이 파일이 크기 검사를
	 * 하지 않는 sprintf 를 쓰기 때문에 필요한 방어다. */
	while (res && index--) {
		out += sprintf(out, "start = %8.8x, length = %8.8x\n", res->base, res->length);
		res = res->next;
	}

	/* [한국어] 쓴 바이트 수를 돌려준다. 커서와 시작점의 차이가 곧 길이다. */
	return out - buf;
}

/* [한국어]
 * show_dev - 각 슬롯의 카드에 배정된 자원을 찍는다
 *
 * @ctrl: 대상 컨트롤러.
 * @buf: 출력 버퍼.
 * @return: 쓴 바이트 수.
 *
 * show_ctrl() 의 짝이다. 그쪽이 "아직 나눠 주지 않은 것" 을 찍는다면
 * 이쪽은 "이미 나눠 준 것" 을 찍는다. 같은 struct pci_resource 목록이
 * 컨트롤러에 매달리면 자유 목록이고 함수에 매달리면 배정 목록이 되는데,
 * 그 두 뜻을 나란히 보여 주는 것이 이 파일의 목적이다.
 *
 * 슬롯 목록을 따라가며 각 슬롯의 함수 0 을 찾아 그 네 목록을 찍는다.
 *
 * cpqhp_slot_find() 가 NULL 을 주면 break 로 순회 전체를 멈춘다. continue 가
 * 아니라 break 인 탓에, 중간에 카드가 없는 슬롯이 하나 있으면 그 뒤 슬롯들이
 * 출력되지 않는다.
 *
 * 실행 컨텍스트: debugfs 파일 열기. cpqphp_mutex 아래.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   open() → spew_debug_info() → [이 함수]
 *     → cpqhp_slot_find() → sprintf()
 */
static int show_dev(struct controller *ctrl, char *buf)
{
	/* [한국어] 출력 커서. */
	char *out = buf;
	/* [한국어] 루프 상한. */
	int index;
	/* [한국어] 자원 목록 커서. */
	struct pci_resource *res;
	/* [한국어] 슬롯에 대응하는 함수 정보. */
	struct pci_func *new_slot;
	/* [한국어] 슬롯 목록 커서. */
	struct slot *slot;

	/* [한국어] 이 컨트롤러의 첫 슬롯. */
	slot = ctrl->slot;

	/* [한국어] 슬롯을 하나씩 따라간다. */
	while (slot) {
		/* [한국어] 그 슬롯의 함수 0 을 찾는다. 배정된 자원이 함수 단위로 매달려 있기 때문이다. */
		new_slot = cpqhp_slot_find(slot->bus, slot->device, 0);
		/* [한국어] 없으면, */
		if (!new_slot)
			/* [한국어] 순회를 멈춘다. 남은 슬롯을 건너뛰지 않고 통째로 중단하는 것이라,
			 * 중간에 빈 슬롯이 있으면 그 뒤가 출력되지 않는다. */
			break;
		/* [한국어] 이 슬롯에 배정된 메모리 제목. */
		out += sprintf(out, "assigned resources: memory\n");
		/* [한국어] 상한. */
		index = 11;
		/* [한국어] 함수에 매달린 메모리 목록. show_ctrl() 이 컨트롤러의 **자유** 목록을
		 * 찍는 것과 달리 이쪽은 **배정된** 목록을 찍는다. 같은 자료구조가 두 뜻으로
		 * 쓰이는 것이 이 드라이버의 자원 관리 방식이다. */
		res = new_slot->mem_head;
		/* [한국어] 자유 목록을 따라가며 항목마다 시작 주소와 길이를 찍는다.
		 * index 를 11 로 두고 후위 감소로 세는 것이 상한이다 — 목록이 순환하거나
		 * 예상보다 길어도 버퍼를 넘지 않게 하려는 것으로, 이 파일이 크기 검사를
		 * 하지 않는 sprintf 를 쓰기 때문에 필요한 방어다. */
		while (res && index--) {
			out += sprintf(out, "start = %8.8x, length = %8.8x\n", res->base, res->length);
			res = res->next;
		}
		/* [한국어] 프리페치 가능 메모리 제목. */
		out += sprintf(out, "assigned resources: prefetchable memory\n");
		/* [한국어] 상한. */
		index = 11;
		/* [한국어] 그 목록. */
		res = new_slot->p_mem_head;
		/* [한국어] 자유 목록을 따라가며 항목마다 시작 주소와 길이를 찍는다.
		 * index 를 11 로 두고 후위 감소로 세는 것이 상한이다 — 목록이 순환하거나
		 * 예상보다 길어도 버퍼를 넘지 않게 하려는 것으로, 이 파일이 크기 검사를
		 * 하지 않는 sprintf 를 쓰기 때문에 필요한 방어다. */
		while (res && index--) {
			out += sprintf(out, "start = %8.8x, length = %8.8x\n", res->base, res->length);
			res = res->next;
		}
		/* [한국어] I/O 제목. */
		out += sprintf(out, "assigned resources: IO\n");
		/* [한국어] 상한. */
		index = 11;
		/* [한국어] I/O 목록. */
		res = new_slot->io_head;
		/* [한국어] 자유 목록을 따라가며 항목마다 시작 주소와 길이를 찍는다.
		 * index 를 11 로 두고 후위 감소로 세는 것이 상한이다 — 목록이 순환하거나
		 * 예상보다 길어도 버퍼를 넘지 않게 하려는 것으로, 이 파일이 크기 검사를
		 * 하지 않는 sprintf 를 쓰기 때문에 필요한 방어다. */
		while (res && index--) {
			out += sprintf(out, "start = %8.8x, length = %8.8x\n", res->base, res->length);
			res = res->next;
		}
		/* [한국어] 버스 번호 제목. */
		out += sprintf(out, "assigned resources: bus numbers\n");
		/* [한국어] 상한. */
		index = 11;
		/* [한국어] 버스 번호 목록. */
		res = new_slot->bus_head;
		/* [한국어] 자유 목록을 따라가며 항목마다 시작 주소와 길이를 찍는다.
		 * index 를 11 로 두고 후위 감소로 세는 것이 상한이다 — 목록이 순환하거나
		 * 예상보다 길어도 버퍼를 넘지 않게 하려는 것으로, 이 파일이 크기 검사를
		 * 하지 않는 sprintf 를 쓰기 때문에 필요한 방어다. */
		while (res && index--) {
			out += sprintf(out, "start = %8.8x, length = %8.8x\n", res->base, res->length);
			res = res->next;
		}
		/* [한국어] 다음 슬롯으로. */
		slot = slot->next;
	}

	/* [한국어] 쓴 바이트 수. */
	return out - buf;
}

/* [한국어]
 * spew_debug_info - 두 출력을 이어 붙여 파일 내용을 만든다
 *
 * @ctrl: 대상 컨트롤러.
 * @data: 출력 버퍼.
 * @size: 그 버퍼의 크기.
 * @return: 파일 내용의 길이로 쓰일 값.
 *
 * 이름 그대로 자유 목록과 배정 목록을 이어 붙이는 것이 의도다.
 *
 * [상류 코드 관찰] 그런데 산술이 그 의도와 맞지 않는다. 두 줄을 풀어 보면
 * show_ctrl() 이 len1 을, show_dev() 가 len2 를 반환할 때
 *   첫 줄:  used = size - len1
 *   둘째 줄: show_dev 의 쓰기 시작 위치가 &data[used] = &data[size - len1]
 *           최종 used = (size - used) - len2 = len1 - len2
 * 가 된다. 두 가지가 어긋난다.
 *   (1) 두 번째 출력이 첫 출력 바로 뒤가 아니라 버퍼 거의 끝에서 시작한다.
 *       len1 이 작을수록 더 뒤로 가며, MAX_OUTPUT 이 16KB 이고 실제 출력이
 *       그보다 훨씬 짧은 것이 그나마 넘침을 막아 준다.
 *   (2) 반환값이 두 길이의 합이 아니라 차다. len2 가 더 크면 음수가 되고,
 *       그 값이 dbg->size 로 들어가 lseek 과 read 의 경계가 된다.
 * 의도한 형태는 used = len1 뒤에 used += show_dev(ctrl, &data[used]) 로 보인다.
 * 코드는 고치지 않고 사실만 기록한다.
 *
 * 실행 컨텍스트: debugfs 파일 열기. cpqphp_mutex 아래.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   open() → [이 함수] → show_ctrl() → show_dev()
 */
static int spew_debug_info(struct controller *ctrl, char *data, int size)
{
	/* [한국어] 쓴 바이트 수를 담을 변수. */
	int used;

	/* [한국어] 컨트롤러의 자유 목록을 찍는다.
	 * [상류 코드 관찰] 반환값을 그대로 쓰지 않고 size 에서 뺀다. 이후 계산이
	 * 그것을 전제로 하지 않아 결과가 어긋난다 — 아래 두 줄 참고. */
	used = size - show_ctrl(ctrl, data);
	/* [한국어] [상류 코드 관찰] 이 줄에 두 가지 문제가 있다.
	 *   (1) 두 번째 출력의 시작 위치가 &data[used] = &data[size - len1] 이다.
	 *       첫 출력 바로 뒤(&data[len1])여야 하는데, 버퍼 거의 끝에서 쓰기
	 *       시작하는 셈이다. len1 이 작을수록 더 뒤로 간다.
	 *   (2) 최종 used 가 (size - used) - len2 = len1 - len2 가 된다.
	 *       두 출력의 합(len1 + len2)이어야 하는데 차가 되며, len2 가 더 크면
	 *       음수가 된다.
	 *   의도한 형태는 used = len1; used += show_dev(ctrl, &data[used]); 로
	 *   보인다. 코드는 고치지 않고 사실만 적는다. */
	used = (size - used) - show_dev(ctrl, &data[used]);
	/* [한국어] 계산된 값을 돌려준다. 호출자가 이것을 파일 크기로 삼는다. */
	return used;
}

struct ctrl_dbg {
	/* [한국어] 이 열린 파일이 담고 있는 내용의 길이.
	 * 설정자: open() 이 spew_debug_info() 의 반환값을 넣는다.
	 * 읽는 자: lseek() 과 read() 가 경계로 쓴다.
	 * 값 범위: 위 관찰대로 의도한 값과 다를 수 있고 음수도 가능하다.
	 * 동기화: 파일 하나에 하나씩 만들어지므로 공유되지 않는다. */
	int size;
	/* [한국어] 내용을 담은 버퍼.
	 * 설정자: open() 이 MAX_OUTPUT 만큼 할당해 채운다.
	 * 읽는 자: read() 가 여기서 사용자 공간으로 복사한다.
	 * 값 범위: 유효한 커널 포인터.
	 * 동기화: 파일별로 따로 있어 공유되지 않는다. */
	char *data;
};

/* [한국어] 출력 버퍼 크기. 페이지 넷은 16KB(4KB 페이지 기준)로, 자유 목록과 배정
 * 목록을 모두 담기에 넉넉하다고 본 값이다. */
#define MAX_OUTPUT	(4*PAGE_SIZE)

/* [한국어]
 * open - debugfs 파일을 열면서 내용을 통째로 만들어 둔다
 *
 * @inode: 이 파일의 inode. i_private 에 컨트롤러가 실려 있다.
 * @file: 열린 파일.
 * @return: 0 = 성공, -ENOMEM = 할당 실패.
 *
 * 읽을 때가 아니라 **열 때** 내용을 다 만들어 두는 것이 이 파일의 방식이다.
 * 그 덕분에 read 가 여러 번 불려도 일관된 스냅숏을 보게 되고, read 경로가
 * 단순한 버퍼 복사로 끝난다.
 *
 * 대가는 메모리다. 열 때마다 16KB(MAX_OUTPUT)를 잡는다. 디버그 파일이라
 * 감수한 설계로 보인다.
 *
 * cpqphp_mutex 가 이 준비 과정을 감싼다.
 * [상류 코드 관찰] 다만 그 락이 실제로 보호하는 것은 이 함수의 중복 실행뿐이다.
 * 읽는 대상인 컨트롤러의 자유 목록을 고치는 쪽(cpqphp_ctrl.c 의 자원 배정)은
 * 이 뮤텍스를 알지 못하므로, 목록이 바뀌는 도중에 읽을 수 있다.
 *
 * retval 초기값이 -ENOMEM 이고 exit 라벨로 뛰는 두 경로가 그것을 그대로
 * 쓰는 구조라, 성공 경로에서만 0 으로 덮는다.
 *
 * 실행 컨텍스트: 파일 열기. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 두 할당 실패 모두 -ENOMEM. 둘째 실패는 첫 할당을 놓고 나간다.
 *
 * 호출 체인:
 *   open(2) → VFS → [이 함수] → kmalloc() ×2 → spew_debug_info()
 */
static int open(struct inode *inode, struct file *file)
{
	/* [한국어] debugfs 파일을 만들 때 매달아 둔 컨트롤러. */
	struct controller *ctrl = inode->i_private;
	/* [한국어] 이 열기에 붙일 상태. */
	struct ctrl_dbg *dbg;
	/* [한국어] 기본값을 메모리 부족으로 둔다. 아래 exit 라벨로 뛰는 두 경로가 이 값을
	 * 그대로 쓴다. */
	int retval = -ENOMEM;

	/* [한국어] 락을 잡는다. */
	mutex_lock(&cpqphp_mutex);
	/* [한국어] 상태 구조체를 할당한다. */
	dbg = kmalloc_obj(*dbg);
	/* [한국어] 실패하면, */
	if (!dbg)
		goto exit;
	/* [한국어] 내용 버퍼를 할당한다. 열 때마다 16KB 를 잡는 것이라 가볍지 않지만,
	 * 디버그 파일이라 감수한 설계다. */
	dbg->data = kmalloc(MAX_OUTPUT, GFP_KERNEL);
	/* [한국어] 실패하면, */
	if (!dbg->data) {
		/* [한국어] 먼저 잡은 것을 놓고, */
		kfree(dbg);
		goto exit;
	}
	/* [한국어] 내용을 지금 채운다. 읽을 때가 아니라 열 때 한 번에 만드는 것이라,
	 * read 가 여러 번 불려도 일관된 스냅숏을 본다. */
	dbg->size = spew_debug_info(ctrl, dbg->data, MAX_OUTPUT);
	/* [한국어] 파일에 상태를 매단다. 이후 lseek/read/release 가 이것을 꺼내 쓴다. */
	file->private_data = dbg;
	/* [한국어] 성공으로 표시한다. */
	retval = 0;
exit:
	/* [한국어] 락을 푼다. 성공이든 실패든 이 자리를 지난다. */
	mutex_unlock(&cpqphp_mutex);
	/* [한국어] 결과를 돌려준다. */
	return retval;
}

/* [한국어]
 * lseek - 고정 크기 파일의 표준 탐색에 맡긴다
 *
 * @file: 열린 파일.
 * @off: 이동할 위치.
 * @whence: 기준(SEEK_SET/CUR/END).
 * @return: 새 위치, 또는 음수 오류.
 *
 * 내용이 열 때 이미 확정되었으므로 크기가 고정이고, 커널이 제공하는
 * fixed_size_llseek() 로 충분하다.
 *
 * 경계로 넘기는 dbg->size 는 spew_debug_info() 가 계산한 값이라,
 * 그 함수의 산술 문제가 여기까지 이어진다.
 *
 * 실행 컨텍스트: lseek(2). 프로세스 컨텍스트.
 *
 * 에러 경로: 헬퍼가 처리한다.
 *
 * 호출 체인:
 *   lseek(2) → VFS → [이 함수] → fixed_size_llseek()
 */
static loff_t lseek(struct file *file, loff_t off, int whence)
{
	/* [한국어] 열 때 매달아 둔 상태. */
	struct ctrl_dbg *dbg = file->private_data;
	/* [한국어] 고정 크기 파일의 표준 lseek 구현에 맡긴다. dbg->size 를 경계로 준다. */
	return fixed_size_llseek(file, off, whence, dbg->size);
}

/* [한국어]
 * read - 준비해 둔 버퍼에서 사용자 공간으로 복사한다
 *
 * @file: 열린 파일.
 * @buf: 사용자 공간 버퍼.
 * @nbytes: 요청 바이트 수.
 * @ppos: 현재 위치. 헬퍼가 갱신한다.
 * @return: 복사한 바이트 수, 또는 음수 오류.
 *
 * 내용이 이미 만들어져 있으므로 표준 헬퍼 한 줄로 끝난다.
 * simple_read_from_buffer() 가 위치 확인, 경계 자르기, copy_to_user 를 모두
 * 처리한다.
 *
 * 이 함수가 이렇게 짧은 것이 open() 에서 내용을 미리 만들어 두는 설계의
 * 값어치다.
 *
 * 실행 컨텍스트: read(2). 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 헬퍼가 처리한다.
 *
 * 호출 체인:
 *   read(2) → VFS → [이 함수] → simple_read_from_buffer()
 */
static ssize_t read(struct file *file, char __user *buf,
		    size_t nbytes, loff_t *ppos)
{
	/* [한국어] 열 때 매달아 둔 상태. */
	struct ctrl_dbg *dbg = file->private_data;
	/* [한국어] 커널 버퍼에서 사용자 공간으로 복사하는 표준 헬퍼에 맡긴다. 위치와 경계
	 * 검사를 모두 처리해 준다. */
	return simple_read_from_buffer(buf, nbytes, ppos, dbg->data, dbg->size);
}

/* [한국어]
 * release - 열 때 잡은 두 할당을 놓는다
 *
 * @inode: 이 파일의 inode. 쓰지 않는다.
 * @file: 닫히는 파일.
 * @return: 언제나 0.
 *
 * open() 의 kmalloc 두 번과 정확히 짝을 이룬다. 버퍼를 먼저 놓고 상태
 * 구조체를 나중에 놓는 순서인데, 그 반대로 하면 이미 해제된 구조체에서
 * 버퍼 포인터를 읽게 된다.
 *
 * open() 과 달리 락을 잡지 않는다. 이 파일만의 상태를 놓는 것이라 다른
 * 열기와 겹칠 일이 없기 때문이다.
 *
 * 실행 컨텍스트: close(2). 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   close(2) → VFS → [이 함수] → kfree() ×2
 */
static int release(struct inode *inode, struct file *file)
{
	/* [한국어] 열 때 매달아 둔 상태. */
	struct ctrl_dbg *dbg = file->private_data;

	/* [한국어] 내용 버퍼를 놓고, */
	kfree(dbg->data);
	/* [한국어] 상태 구조체를 놓는다. open() 의 할당 두 번과 정확히 짝을 이룬다. */
	kfree(dbg);
	return 0;
}

static const struct file_operations debug_ops = {
	/* [한국어] 모듈이 살아 있는 동안만 이 파일이 열려 있게 한다. */
	.owner = THIS_MODULE,
	/* [한국어] 내용을 만드는 곳. */
	.open = open,
	/* [한국어] 고정 크기 탐색. */
	.llseek = lseek,
	/* [한국어] 버퍼에서 복사. */
	.read = read,
	/* [한국어] 해제. */
	.release = release,
};

/* [한국어] debugfs 의 "cpqhp" 디렉터리. 컨트롤러가 여럿이어도 하나만 만든다.
 * 설정자: cpqhp_initialize_debugfs().  읽는 자: 파일 생성과 해제.
 * 값 범위: 유효한 dentry 또는 NULL.
 * 동기화: 초기화가 한 번만 불린다는 전제이며, 아래 NULL 검사가 중복
 *   호출을 막는다. */
static struct dentry *root;

/* [한국어]
 * cpqhp_initialize_debugfs - debugfs 에 "cpqhp" 디렉터리를 만든다
 *
 * 컨트롤러가 여럿 있어도 디렉터리는 하나여야 하므로, 이미 있으면 만들지
 * 않는다. 그 검사가 중복 호출에 대한 유일한 방어다.
 *
 * 부모를 NULL 로 주면 debugfs 루트 아래에 만들어진다.
 *
 * 반환값이 없다. 실패해도 root 가 NULL 로 남고, 그러면 아래
 * cpqhp_create_debugfs_files() 가 루트 아래에 파일을 만들게 된다 —
 * debugfs_create_dir 실패가 조용히 다른 위치로 이어지는 셈이다.
 *
 * 실행 컨텍스트: 모듈 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   cpqphp_core.c 의 모듈 초기화 → [이 함수] → debugfs_create_dir()
 */
void cpqhp_initialize_debugfs(void)
{
	/* [한국어] 아직 없을 때만 만든다. 컨트롤러마다 초기화가 불려도 디렉터리는 하나다. */
	if (!root)
		/* [한국어] 부모를 NULL 로 주어 debugfs 루트 아래에 만든다. */
		root = debugfs_create_dir("cpqhp", NULL);
}

/* [한국어]
 * cpqhp_shutdown_debugfs - 그 디렉터리를 지운다
 *
 * debugfs_remove() 가 디렉터리와 그 아래 남은 파일을 함께 없앤다.
 *
 * [상류 코드 관찰] root 를 NULL 로 되돌리지 않는다. 모듈을 뺐다 다시 넣으면
 * cpqhp_initialize_debugfs() 가 "이미 있다" 고 판단해 만들지 않고, 해제된
 * dentry 를 그대로 쓰게 된다. 아래 cpqhp_remove_debugfs_files() 가
 * ctrl->dentry 를 NULL 로 되돌리는 것과 대비된다.
 *
 * 실행 컨텍스트: 모듈 해제. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   cpqphp_core.c 의 모듈 해제 → [이 함수] → debugfs_remove()
 */
void cpqhp_shutdown_debugfs(void)
{
	/* [한국어] 디렉터리를 지운다. 그 아래 남은 파일도 함께 사라진다.
	 * [상류 코드 관찰] root 를 NULL 로 되돌리지 않아, 초기화를 다시 부르면
	 * 해제된 dentry 를 그대로 두고 만들지 않는다. */
	debugfs_remove(root);
}

/* [한국어]
 * cpqhp_create_debugfs_files - 이 컨트롤러의 디버그 파일을 만든다
 *
 * @ctrl: 대상 컨트롤러.
 *
 * 파일 이름을 컨트롤러의 PCI 장치 이름으로 짓는다. 컨트롤러가 여럿이어도
 * 겹치지 않는 이름이 자동으로 나온다.
 *
 * i_private 에 컨트롤러를 실어 두는 것이 이 함수의 요점이다. open() 이
 * inode->i_private 로 그것을 꺼내므로, 파일과 컨트롤러가 이 한 줄로 이어진다.
 *
 * 읽기 전용(S_IRUGO)으로 만든다. 위 file_operations 에도 write 가 없어
 * 둘이 일치한다.
 *
 * 실행 컨텍스트: 컨트롤러 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값을 확인하지 않으며, 실패하면 ctrl->dentry 가
 * 오류 포인터가 된다.
 *
 * 호출 체인:
 *   cpqphp_core.c 의 컨트롤러 등록 → [이 함수] → debugfs_create_file()
 */
void cpqhp_create_debugfs_files(struct controller *ctrl)
{
	/* [한국어] 컨트롤러의 PCI 이름으로 파일을 만든다. 컨트롤러가 여럿이면 이름으로
	 * 구분된다. */
	ctrl->dentry = debugfs_create_file(dev_name(&ctrl->pci_dev->dev),
					   /* [한국어] 읽기 전용으로 만들고, i_private 에 컨트롤러를 실어 둔다. open() 이
					    * 그것을 꺼내 쓴다. */
					   S_IRUGO, root, ctrl, &debug_ops);
}

/* [한국어]
 * cpqhp_remove_debugfs_files - 이 컨트롤러의 디버그 파일을 없앤다
 *
 * @ctrl: 대상 컨트롤러.
 *
 * 파일을 지우고 포인터를 NULL 로 되돌린다. 위 cpqhp_shutdown_debugfs() 가
 * root 를 되돌리지 않는 것과 달리 이쪽은 되돌리므로, 같은 컨트롤러를
 * 다시 등록해도 문제가 없다.
 *
 * 실행 컨텍스트: 컨트롤러 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   cpqphp_core.c 의 컨트롤러 해제 → [이 함수] → debugfs_remove()
 */
void cpqhp_remove_debugfs_files(struct controller *ctrl)
{
	/* [한국어] 파일을 지우고, */
	debugfs_remove(ctrl->dentry);
	/* [한국어] 포인터를 지운다. 위 shutdown 과 달리 이쪽은 NULL 로 되돌린다. */
	ctrl->dentry = NULL;
}

