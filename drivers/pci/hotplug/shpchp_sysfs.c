// SPDX-License-Identifier: GPL-2.0+
/*
 * Compaq Hot Plug Controller Driver
 *
 * Copyright (c) 1995,2001 Compaq Computer Corporation
 * Copyright (c) 2001,2003 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (c) 2001 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>
 *
 */

/* [한국어] 이 파일이 shpchp 모듈의 일부로 빌드되므로 포함한다. */
/*
 * [한국어 설명] SHPC 컨트롤러의 자원 현황 sysfs 진단 속성 (shpchp_sysfs.c)
 *
 * === 파일의 역할 ===
 * SHPC(Standard Hot Plug Controller) 컨트롤러마다 sysfs 파일 하나를 만들어,
 * 그 컨트롤러가 관리하는 브리지의 자원 창(메모리·프리페치 가능 메모리·I/O 포트)과
 * 아직 쓰이지 않는 버스 번호를 사람이 읽을 수 있는 텍스트로 보여 준다.
 * 이 파일이 만드는 속성은 "ctrl" 하나뿐이고, 권한은 0444 로 읽기 전용이며
 * 위치는 컨트롤러 PCI 함수의 sysfs 디렉토리(/sys/bus/pci/devices/<도메인:버스:장치.함수>/ctrl)다.
 * 존재 이유는 진단이다 — 핫플러그로 카드를 꽂았을 때 자원 배정이 실패하는 원인은
 * 대개 브리지 창이 좁거나 버스 번호가 동난 것인데, 커널 로그만으로는 그 사실을
 * 판단하기 어렵다. 관리자가 카드를 꽂기 전에 이 파일을 읽으면 브리지가 나눠 줄 수
 * 있는 자원의 윤곽을 미리 확인할 수 있다. 이 파일은 슬롯 단위 sysfs 인터페이스
 * (power/attention/latch 등)와는 무관하다 — 그쪽은 공용 코어 pci_hotplug_core.c 의 몫이고,
 * 여기는 컨트롤러 단위 디버그 창구 하나만 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SHPC 드라이버는 진입점 shpchp_core.c, 슬롯 상태 기계 shpchp_ctrl.c,
 * 컨트롤러 레지스터 접근 shpchp_hpc.c, PCI 열거/제거 어댑터 shpchp_pci.c,
 * 그리고 이 파일로 나뉜다. 다섯 조각 중 이 파일만 하드웨어에도 상태 기계에도
 * 관여하지 않는 순수 관찰자다. 생명주기는 컨트롤러의 probe/remove 에 그대로 묶인다 —
 * shpc_probe() 가 shpc_init() 과 init_slots() 를 마친 뒤 shpchp_create_ctrl_files() 를
 * 부르고(shpchp_core.c:660), shpc_remove() 가 컨트롤러 자원을 풀기 전에
 * shpchp_remove_ctrl_files() 를 부른다(:707). 그 순서가 중요한 이유는 두 가지다.
 * 생성이 늦어야 show_ctrl 이 읽을 상태가 이미 유효하고, 제거가 일러야 사용자가
 * 해제된 컨트롤러를 읽는 use-after-free 가 생기지 않는다.
 * 실행 컨텍스트는 두 갈래다. 두 create/remove 함수는 드라이버 probe/remove 문맥이고,
 * show_ctrl 은 사용자 공간 read() 시스템 콜 문맥에서 비동기적으로 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽 의존: shpchp.h 가 struct controller 를 정의하고, 이 파일의 두 함수를
 * 선언한다(:140~141). 생성 쪽에는 __must_check 가 붙어 호출자가 실패를 무시하지
 * 못하게 강제한다. 유일한 호출자는 shpchp_core.c 다.
 * 아래쪽 의존: linux/pci.h 의 to_pci_dev(), pci_bus_for_each_resource(),
 * pci_find_bus(), pci_domain_nr() 와 IORESOURCE_MEM/IO/PREFETCH 플래그,
 * 그리고 드라이버 코어의 DEVICE_ATTR / device_create_file() / device_remove_file(),
 * sysfs 출력 헬퍼 sysfs_emit_at().
 * 데이터 흐름은 단방향 읽기뿐이다: struct controller → ctrl->pci_dev →
 * pdev->subordinate(브리지 하위 버스) → 그 버스의 자원 배열과 busn_res →
 * 텍스트 → 사용자 공간. 이 파일은 어떤 상태도 바꾸지 않고 어떤 하드웨어 레지스터도
 * 건드리지 않는다.
 * 공유 상태: struct pci_bus 의 자원 배열과 버스 번호 범위를 읽지만 락을 잡지 않는다.
 * 핫플러그가 동시에 그 목록을 바꾸면 출력은 그 순간의 근사값이 되며, 진단 목적이라
 * 그 정도 정확도로 충분하다는 것이 상류의 판단이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - show_ctrl(): 유일한 실질 로직. 네 구획(메모리 / 프리페치 가능 메모리 / I/O /
 *   버스 번호)을 차례로 출력한다. 앞 세 구획은 pci_bus_for_each_resource() 로 자원
 *   배열을 구획마다 처음부터 다시 훑으며 플래그로 걸러 내고, 네 번째만 성격이 달라
 *   busn_res 범위를 앞에서부터 훑어 pci_find_bus() 로 미사용 첫 번호를 찾는다.
 * - dev_attr_ctrl (DEVICE_ATTR 매크로가 만드는 변수): 이름 "ctrl", 권한 S_IRUGO(0444),
 *   show = show_ctrl, store = NULL. store 가 NULL 이라 쓰기는 -EACCES 로 거부된다.
 * - shpchp_create_ctrl_files(): probe 시 파일 생성. __must_check 로 반환값 검사를 강제.
 * - shpchp_remove_ctrl_files(): remove 시 파일 제거. 진행 중인 read 가 끝날 때까지
 *   기다려 주므로, 돌아온 뒤에는 show_ctrl 이 실행 중이 아님이 보장된다.
 * - [상류 코드 관찰, 수정하지 않음] 구획 제목의 "Free resources" 는 앞 세 구획에는
 *   들어맞지 않는다. 그것들은 브리지에 배정된 창 전체를 찍을 뿐 그 안의 빈 공간을
 *   계산하지 않는다. 또 버스 번호 구획의 조건이 busn_res.end 와 < 비교라
 *   마지막 번호만 비어 있으면 보고되지 않고, 길이도 포함 구간 기준으로 1 작다.
 */

#include <linux/module.h>
/* [한국어] 커널 공통 정의. */
#include <linux/kernel.h>
/* [한국어] u8/ssize_t 등 기본 타입 정의. show 콜백의 반환형 ssize_t 가 여기서 온다. */
#include <linux/types.h>
/* [한국어] PCI 코어 공개 API — to_pci_dev(), pci_bus_for_each_resource(), pci_find_bus(),
 * pci_domain_nr(), IORESOURCE_MEM/IO/PREFETCH 플래그가 모두 여기서 온다. */
#include <linux/pci.h>
/* [한국어] SHPC 드라이버 자체 헤더. struct controller 정의와 아래 두 함수의 선언이 있다. */
#include "shpchp.h"


/* [한국어] [아래 영어 주석 보충] 이 파일에는 sysfs 속성이 단 하나(ctrl)뿐이며,
 * 그 속성을 만들고 지우는 두 함수와 읽기 콜백 하나로 전부다. */
/* A few routines that create sysfs entries for the hot plug controller */

/* [한국어]
 * show_ctrl - sysfs 의 "ctrl" 속성을 읽을 때 브리지 자원 창 현황을 문자열로 만든다
 *
 * @dev: 속성이 붙어 있는 struct device. 실제로는 SHPC 컨트롤러의 PCI 함수
 *      (ctrl->pci_dev->dev)이며, 함수 첫머리에서 to_pci_dev() 로 struct pci_dev 로 되돌린다.
 * @attr: 어떤 속성이 읽혔는지 알려 주는 인자. 이 파일에는 속성이 "ctrl" 하나뿐이라
 *      구분할 필요가 없어 전혀 사용하지 않는다.
 * @buf: 커널이 미리 잡아 준 PAGE_SIZE 크기의 출력 버퍼. 여기에 쓴 내용이 그대로
 *      사용자 공간의 read() 결과가 된다.
 * @return: buf 에 실제로 쓴 바이트 수. sysfs 규약상 음수 errno 도 가능하지만
 *      이 구현은 실패 경로가 없어 항상 0 이상을 돌려준다.
 *
 * 왜 필요한가: 핫플러그로 카드를 꽂았는데 자원 배정이 실패하는 일이 잦다. 브리지의
 * 메모리/IO 창이 좁거나 버스 번호가 동났기 때문인데, 그 사실을 커널 로그만 보고
 * 판단하기 어렵다. 이 속성은 관리자가 카드를 꽂기 전에
 * /sys/bus/pci/devices/<BDF>/ctrl 를 읽어 "이 브리지가 나눠 줄 수 있는 자원이 무엇인지"를
 * 눈으로 확인할 수 있게 해 주는 순수 진단 인터페이스다.
 *
 * 동작 과정: 네 구획을 차례로 출력한다.
 *   1) "Free resources: memory" — 프리페치 불가 메모리 창.
 *   2) "Free resources: prefetchable memory" — 프리페치 가능 메모리 창.
 *   3) "Free resources: IO" — I/O 포트 창.
 *      이 셋은 모두 pci_bus_for_each_resource() 로 버스 자원 배열을 처음부터 다시 훑으며,
 *      플래그 조합으로 자기 구획에 해당하는 창만 골라 시작 주소와 길이를 찍는다.
 *      res 가 NULL 일 수 있어 모든 조건문이 res 검사부터 시작한다.
 *   4) "Free resources: bus numbers" — 여기만 성격이 다르다. 자원 배열이 아니라
 *      busn_res 범위를 앞에서부터 훑어 pci_find_bus() 로 아직 등록되지 않은 첫 번호를
 *      찾고, 찾았을 때만 한 줄을 출력한다.
 * 출력은 매번 sysfs_emit_at(buf, len, ...) 로 이어 쓰고 반환값을 len 에 누적한다.
 * 이 헬퍼가 PAGE_SIZE 경계 검사를 대신해 주므로 별도의 오버플로 방어가 없다.
 *
 * [상류 코드 관찰, 수정하지 않음] 제목은 "Free resources" 지만 앞 세 구획이 출력하는
 * 것은 브리지에 배정된 창 전체이지 그 안에서 아직 쓰지 않은 부분이 아니다. 실제로
 * "남은 것"을 계산하는 구획은 네 번째(버스 번호)뿐이다. 또 그 네 번째의 조건이
 * busnr < end 라 마지막 번호 하나만 비어 있는 경우는 보고되지 않고, 길이를 end - busnr
 * 로 계산해 포함 구간 기준으로는 1 작다.
 *
 * 실행 컨텍스트: 사용자 공간의 read() 시스템 콜 문맥(프로세스 컨텍스트). sysfs 코어가
 * 속성 단위로 직렬화해 주지만, 이 함수가 읽는 버스 자원 목록은 핫플러그가 동시에 변경할
 * 수 있다 — 락을 잡지 않으므로 출력은 그 순간의 근사값이며, 진단용이라 그 정도로 충분하다는
 * 설계 판단이다.
 *
 * 에러 경로: 없다. 자원이 하나도 없으면 제목 네 줄만 출력되고 정상 종료한다.
 *
 * 호출 체인:
 *   사용자 공간 read("/sys/bus/pci/devices/<BDF>/ctrl")
 *     → VFS → sysfs/kernfs → dev_attr_ctrl.show == [show_ctrl]
 *     → pci_bus_for_each_resource() / pci_find_bus() / sysfs_emit_at()
 */
static ssize_t show_ctrl(struct device *dev, struct device_attribute *attr, char *buf)
{
	/* [한국어] 컨트롤러의 PCI 함수 자체. show 콜백이 받은 struct device 를 PCI 관점으로 되돌린 것이다. */
	struct pci_dev *pdev;
	/* [한국어] 순회 중인 자원 창(window) 포인터. 루프 매크로가 매 반복마다 갱신한다. */
	struct resource *res;
	/* [한국어] 컨트롤러 브리지의 하위(secondary) 버스. 출력하려는 모든 자원 정보의 출처다. */
	struct pci_bus *bus;
	/* [한국어] 지금까지 buf 에 쓴 바이트 수. 0 에서 시작해 sysfs_emit_at() 반환값을 누적하며,
	 * 마지막에 그대로 반환해 커널이 사용자에게 넘길 길이가 된다. */
	size_t len = 0;
	/* [한국어] 버스 번호 탐색 루프의 인덱스. 루프가 끝난 뒤에도 값을 쓰므로 루프 밖에 선언되어 있다. */
	int busnr;

	/* [한국어] sysfs show 콜백은 struct device 를 받으므로, container_of 기반 매크로로
	 * 이를 감싸고 있는 struct pci_dev 로 되돌린다. 이 dev 는 shpchp_create_ctrl_files() 가
	 * 속성을 붙여 둔 ctrl->pci_dev->dev 와 동일한 객체다. */
	pdev = to_pci_dev(dev);
	/* [한국어] 브리지의 subordinate = secondary 버스 포인터. 핫플러그 슬롯들이 매달린 버스이며,
	 * 이 버스의 자원 창이 곧 "슬롯에 배분할 수 있는 자원"의 상한이다. */
	bus = pdev->subordinate;

	/* [한국어] 첫 번째 구획 제목. sysfs_emit_at(buf, len, ...) 은 buf 의 len 오프셋부터 이어 쓰고 실제로 쓴 바이트 수를
	 * 돌려주는 sysfs 전용 출력 헬퍼다. snprintf 대신 이것을 쓰는 이유는 sysfs 버퍼가
	 * PAGE_SIZE 로 고정되어 있어 오버플로 검사를 매번 손으로 하면 실수가 나기 때문이며,
	 * 이 헬퍼가 경계 검사를 대신 해 준다. 반환값을 len 에 누적해 다음 출력 위치를 옮긴다. */
	len += sysfs_emit_at(buf, len, "Free resources: memory\n");
	/* [한국어] 버스에 할당된 자원 창들을 순회한다. PCI 브리지는 최대 4개(IO, MEM, PREFETCH MEM,
	 * 버스 번호)의 창을 갖고, 매크로는 그 배열을 훑는다. 배열에 빈 칸이 있을 수 있어
	 * 본문에서 res 가 NULL 인지 먼저 확인해야 한다. */
	pci_bus_for_each_resource(bus, res) {
		/* [한국어] NULL 이 아니면서 메모리 창이고(IORESOURCE_MEM), 프리페치 불가여야 통과.
		 * 두 조건을 AND 로 묶은 이유는 프리페치 가능 메모리를 아래에서 따로 출력하기 위해서다.
		 * PCI 규격상 프리페치 가능/불가 메모리는 브리지에서 별도의 창으로 관리된다. */
		if (res && (res->flags & IORESOURCE_MEM) &&
				!(res->flags & IORESOURCE_PREFETCH)) {
			/* [한국어] 창의 시작 주소와 길이를 16진수 8자리로 출력한다. sysfs_emit_at(buf, len, ...) 은 buf 의 len 오프셋부터 이어 쓰고 실제로 쓴 바이트 수를
			 * 돌려주는 sysfs 전용 출력 헬퍼다. snprintf 대신 이것을 쓰는 이유는 sysfs 버퍼가
			 * PAGE_SIZE 로 고정되어 있어 오버플로 검사를 매번 손으로 하면 실수가 나기 때문이며,
			 * 이 헬퍼가 경계 검사를 대신 해 준다. 반환값을 len 에 누적해 다음 출력 위치를 옮긴다. */
			len += sysfs_emit_at(buf, len,
					     "start = %8.8llx, length = %8.8llx\n",
					     (unsigned long long)res->start,
					     (unsigned long long)resource_size(res));
		}
	}
	/* [한국어] 두 번째 구획 제목 — 프리페치 가능 메모리. sysfs_emit_at(buf, len, ...) 은 buf 의 len 오프셋부터 이어 쓰고 실제로 쓴 바이트 수를
	 * 돌려주는 sysfs 전용 출력 헬퍼다. snprintf 대신 이것을 쓰는 이유는 sysfs 버퍼가
	 * PAGE_SIZE 로 고정되어 있어 오버플로 검사를 매번 손으로 하면 실수가 나기 때문이며,
	 * 이 헬퍼가 경계 검사를 대신 해 준다. 반환값을 len 에 누적해 다음 출력 위치를 옮긴다. */
	len += sysfs_emit_at(buf, len, "Free resources: prefetchable memory\n");
	/* [한국어] 같은 자원 배열을 다시 처음부터 순회한다. 창의 순서가 보장되지 않으므로
	 * 구획별로 전체를 다시 훑는 단순한 구조를 택했다. */
	pci_bus_for_each_resource(bus, res) {
		/* [한국어] 메모리 창이면서 프리페치 가능(IORESOURCE_PREFETCH 가 켜짐)인 것만 통과.
		 * 위 구획의 조건과 정확히 반대라서 두 구획이 겹치지 않고 메모리 창 전체를 덮는다. */
		if (res && (res->flags & IORESOURCE_MEM) &&
			       (res->flags & IORESOURCE_PREFETCH)) {
			/* [한국어] 프리페치 가능 창의 시작 주소와 길이 출력. sysfs_emit_at(buf, len, ...) 은 buf 의 len 오프셋부터 이어 쓰고 실제로 쓴 바이트 수를
			 * 돌려주는 sysfs 전용 출력 헬퍼다. snprintf 대신 이것을 쓰는 이유는 sysfs 버퍼가
			 * PAGE_SIZE 로 고정되어 있어 오버플로 검사를 매번 손으로 하면 실수가 나기 때문이며,
			 * 이 헬퍼가 경계 검사를 대신 해 준다. 반환값을 len 에 누적해 다음 출력 위치를 옮긴다. */
			len += sysfs_emit_at(buf, len,
					     "start = %8.8llx, length = %8.8llx\n",
					     (unsigned long long)res->start,
					     (unsigned long long)resource_size(res));
		}
	}
	/* [한국어] 세 번째 구획 제목 — I/O 포트 공간. sysfs_emit_at(buf, len, ...) 은 buf 의 len 오프셋부터 이어 쓰고 실제로 쓴 바이트 수를
	 * 돌려주는 sysfs 전용 출력 헬퍼다. snprintf 대신 이것을 쓰는 이유는 sysfs 버퍼가
	 * PAGE_SIZE 로 고정되어 있어 오버플로 검사를 매번 손으로 하면 실수가 나기 때문이며,
	 * 이 헬퍼가 경계 검사를 대신 해 준다. 반환값을 len 에 누적해 다음 출력 위치를 옮긴다. */
	len += sysfs_emit_at(buf, len, "Free resources: IO\n");
	/* [한국어] 세 번째 순회. I/O 창은 보통 하나뿐이지만 같은 형태를 유지한다. */
	pci_bus_for_each_resource(bus, res) {
		/* [한국어] IORESOURCE_IO 만 확인하면 된다 — I/O 공간에는 프리페치 개념이 없기 때문이다. */
		if (res && (res->flags & IORESOURCE_IO)) {
			/* [한국어] I/O 창의 시작 주소와 길이 출력. 값은 포트 번호지만 서식은 메모리와 동일하다. sysfs_emit_at(buf, len, ...) 은 buf 의 len 오프셋부터 이어 쓰고 실제로 쓴 바이트 수를
			 * 돌려주는 sysfs 전용 출력 헬퍼다. snprintf 대신 이것을 쓰는 이유는 sysfs 버퍼가
			 * PAGE_SIZE 로 고정되어 있어 오버플로 검사를 매번 손으로 하면 실수가 나기 때문이며,
			 * 이 헬퍼가 경계 검사를 대신 해 준다. 반환값을 len 에 누적해 다음 출력 위치를 옮긴다. */
			len += sysfs_emit_at(buf, len,
					     "start = %8.8llx, length = %8.8llx\n",
					     (unsigned long long)res->start,
					     (unsigned long long)resource_size(res));
		}
	}
	/* [한국어] 네 번째 구획 제목 — 버스 번호. 앞의 셋과 달리 자원 배열이 아니라
	 * busn_res(버스 번호 범위)를 직접 훑어 계산한다. sysfs_emit_at(buf, len, ...) 은 buf 의 len 오프셋부터 이어 쓰고 실제로 쓴 바이트 수를
	 * 돌려주는 sysfs 전용 출력 헬퍼다. snprintf 대신 이것을 쓰는 이유는 sysfs 버퍼가
	 * PAGE_SIZE 로 고정되어 있어 오버플로 검사를 매번 손으로 하면 실수가 나기 때문이며,
	 * 이 헬퍼가 경계 검사를 대신 해 준다. 반환값을 len 에 누적해 다음 출력 위치를 옮긴다. */
	len += sysfs_emit_at(buf, len, "Free resources: bus numbers\n");
	/* [한국어] 브리지에 배정된 버스 번호 범위 [start, end] 를 앞에서부터 한 칸씩 확인한다.
	 * 핫애드 시 새 브리지에 줄 수 있는 "아직 안 쓰는 첫 버스 번호"를 찾는 것이 목적이다. */
	for (busnr = bus->busn_res.start; busnr <= bus->busn_res.end; busnr++) {
		/* [한국어] 그 번호로 등록된 struct pci_bus 가 없으면 = 아직 아무도 쓰지 않는 번호다.
		 * pci_find_bus() 는 (도메인, 버스번호) 쌍으로 전역 버스 목록을 조회한다.
		 * 도메인을 함께 넘기는 이유는 다중 PCI 도메인 시스템에서 번호가 도메인마다
		 * 독립적이기 때문이다. */
		if (!pci_find_bus(pci_domain_nr(bus), busnr))
			/* [한국어] 빈 번호를 찾았으므로 즉시 탈출한다. 이때 busnr 이 그 빈 번호이며,
			 * 끝까지 못 찾으면 busnr 은 end + 1 이 되어 아래 조건에서 걸러진다. */
			break;
	}
	/* [한국어] 빈 번호가 실제로 존재할 때만 출력한다.
	 * [상류 코드 관찰, 수정 없음] 비교가 <= 가 아니라 < 이므로 마지막 번호(end) 하나만
	 * 비어 있는 경우는 보고되지 않는다. 또 아래 길이 계산이 end - busnr 이라
	 * [busnr, end] 를 포함 구간으로 보면 실제 개수보다 1 작다. 상류 그대로 둔다. */
	if (busnr < bus->busn_res.end)
		/* [한국어] 빈 버스 번호의 시작과 개수를 출력한다. 앞의 세 구획과 달리 %8.8llx 가 아니라
		 * %8.8x 인 이유는 버스 번호가 8비트 정수여서 64비트 캐스팅이 필요 없기 때문이다. sysfs_emit_at(buf, len, ...) 은 buf 의 len 오프셋부터 이어 쓰고 실제로 쓴 바이트 수를
		 * 돌려주는 sysfs 전용 출력 헬퍼다. snprintf 대신 이것을 쓰는 이유는 sysfs 버퍼가
		 * PAGE_SIZE 로 고정되어 있어 오버플로 검사를 매번 손으로 하면 실수가 나기 때문이며,
		 * 이 헬퍼가 경계 검사를 대신 해 준다. 반환값을 len 에 누적해 다음 출력 위치를 옮긴다. */
		len += sysfs_emit_at(buf, len,
				     "start = %8.8x, length = %8.8x\n",
				     busnr, (int)(bus->busn_res.end - busnr));

	/* [한국어] 누적 길이를 그대로 반환한다. sysfs 규약상 show 콜백은 buf 에 쓴 바이트 수를
	 * 돌려줘야 하며, 커널이 그만큼만 사용자 공간에 복사한다.
	 * [상류 코드 관찰, 수정 없음] 구획 제목은 "Free resources" 라고 하지만 앞 세 구획이
	 * 출력하는 것은 브리지에 배정된 자원 창 전체이지 그 안에서 남아 있는 빈 공간이 아니다.
	 * 실제로 "남은 것"을 계산하는 구획은 버스 번호뿐이다. */
	return len;
}
/* [한국어] sysfs 속성 하나를 정적으로 정의하는 매크로. 이름은 "ctrl", 권한은 S_IRUGO(0444,
 * 모두 읽기 가능·쓰기 불가), show 콜백은 위 show_ctrl, store 콜백은 NULL 이다.
 * store 가 NULL 이라 쓰기를 시도하면 커널이 -EACCES 를 돌려준다 — 진단용 읽기 전용
 * 속성이라는 뜻이다. 이 매크로가 struct device_attribute dev_attr_ctrl 이라는
 * 변수를 만들어 내며, 아래 두 함수가 그 이름을 참조한다. */
static DEVICE_ATTR(ctrl, S_IRUGO, show_ctrl, NULL);

/* [한국어]
 * shpchp_create_ctrl_files - 컨트롤러의 sysfs 디렉토리에 "ctrl" 진단 파일을 만든다
 *
 * @ctrl: SHPC 컨트롤러 객체. 여기서 쓰는 것은 ctrl->pci_dev->dev 하나뿐으로,
 *      그 device 아래에 속성 파일을 붙인다.
 * @return: device_create_file() 의 반환값을 그대로 전달한다. 0 = 성공,
 *      음수 errno = 실패(대개 메모리 부족 또는 같은 이름의 파일이 이미 있는 경우).
 *      선언부(shpchp.h:140)에 __must_check 가 붙어 있어 호출자가 반환값을 무시하면
 *      컴파일 경고가 난다 — 부수 효과만 있는 함수처럼 보이지만 실패할 수 있음을
 *      타입 수준에서 강제하는 장치다.
 *
 * 왜 필요한가: 정적으로 정의한 dev_attr_ctrl 은 "이런 속성이 있다"는 서술일 뿐,
 * 실제 파일은 누군가 device_create_file() 을 불러야 생긴다. 컨트롤러가 여러 개일 수
 * 있으므로 그 시점은 probe 때, 컨트롤러마다 한 번이다.
 *
 * 동작 과정: 한 줄짜리 래퍼다. ctrl 에서 struct device 를 꺼내 device_create_file() 에
 * 넘기고 결과를 그대로 반환한다. 별도 상태를 만들지 않으므로 실패해도 정리할 것이 없다.
 *
 * 실행 컨텍스트: shpc_probe() 안, 프로세스 컨텍스트. 이 호출이 성공하는 순간부터
 * 사용자가 파일을 열 수 있으므로, 그 전에 show_ctrl 이 읽을 상태
 * (ctrl->pci_dev->subordinate)가 이미 유효해야 한다 — 실제로 이 호출은 shpc_init() 과
 * init_slots() 가 끝난 뒤인 shpchp_core.c:660 에 놓여 그 순서를 지킨다.
 *
 * 에러 경로: 실패 시 shpc_probe() 가 err_cleanup_slots 라벨로 되감아 슬롯과 컨트롤러를
 * 해제하고 -ENODEV 를 돌려준다. 즉 진단 파일 하나를 못 만들면 드라이버 등록 자체를 포기한다.
 *
 * 호출 체인:
 *   shpc_probe() (shpchp_core.c:660) → [shpchp_create_ctrl_files]
 *     → device_create_file() → sysfs/kernfs 노드 생성
 */
int shpchp_create_ctrl_files(struct controller *ctrl)
{
	/* [한국어] 컨트롤러의 PCI 디바이스 sysfs 디렉토리(/sys/bus/pci/devices/<BDF>/)에 "ctrl"
	 * 파일을 만든다. 반환값을 그대로 넘겨 실패 시 호출자가 초기화를 중단할 수 있게 한다.
	 * sysfs 파일이 만들어지는 즉시 사용자가 열 수 있으므로, 이 호출 전에 show_ctrl 이
	 * 읽을 상태(ctrl->pci_dev->subordinate)가 이미 유효해야 한다. */
	return device_create_file(&ctrl->pci_dev->dev, &dev_attr_ctrl);
}

/* [한국어]
 * shpchp_remove_ctrl_files - 컨트롤러의 "ctrl" sysfs 파일을 제거한다
 *
 * @ctrl: 제거 대상 컨트롤러. create 쪽과 정확히 같은 device 와 같은 속성 구조체를
 *      넘겨야 짝이 맞는다.
 *
 * 반환값이 없는 이유: device_remove_file() 자체가 void 이고, 파일 제거는 실패할 수 있는
 * 연산이 아니다(없는 파일을 지우려 해도 조용히 넘어간다). 제거 경로에서 되돌릴 수 있는
 * 것이 없으므로 호출자에게 알릴 것도 없다.
 *
 * 왜 필요한가: 컨트롤러가 사라진 뒤에도 sysfs 파일이 남아 있으면, 사용자가 그것을 읽는
 * 순간 show_ctrl 이 이미 해제된 ctrl->pci_dev 를 따라가 use-after-free 가 된다.
 * 그래서 컨트롤러 자원을 해제하기 "전에" 반드시 이 함수를 먼저 불러야 한다.
 * device_remove_file() 은 진행 중인 read 가 끝날 때까지 기다려 주므로, 이 함수가
 * 돌아온 시점에는 show_ctrl 이 더 이상 실행 중이 아님이 보장된다 — 이 순서가
 * 안전성의 핵심이다.
 *
 * 실행 컨텍스트: shpc_remove() 안, 프로세스 컨텍스트. 진행 중인 sysfs 읽기를
 * 기다리며 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   shpc_remove() (shpchp_core.c:707) → [shpchp_remove_ctrl_files]
 *     → device_remove_file() → 진행 중인 show_ctrl 완료 대기 → 노드 제거
 */
void shpchp_remove_ctrl_files(struct controller *ctrl)
{
	/* [한국어] 위에서 만든 "ctrl" 파일을 제거한다. 반환값이 없어 실패 개념이 없고,
	 * 이 함수가 돌아온 뒤에는 show_ctrl 이 더 이상 호출되지 않음이 보장된다 —
	 * device_remove_file() 이 진행 중인 읽기가 끝날 때까지 기다려 주기 때문이다. */
	device_remove_file(&ctrl->pci_dev->dev, &dev_attr_ctrl);
}
