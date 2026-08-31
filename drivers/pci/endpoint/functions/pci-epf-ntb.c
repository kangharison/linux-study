// SPDX-License-Identifier: GPL-2.0
/*
 * Endpoint Function Driver to implement Non-Transparent Bridge functionality
 *
 * Copyright (C) 2020 Texas Instruments
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */

/*
 * The PCI NTB function driver configures the SoC with multiple PCIe Endpoint
 * (EP) controller instances (see diagram below) in such a way that
 * transactions from one EP controller are routed to the other EP controller.
 * Once PCI NTB function driver configures the SoC with multiple EP instances,
 * HOST1 and HOST2 can communicate with each other using SoC as a bridge.
 *
 *    +-------------+                                   +-------------+
 *    |             |                                   |             |
 *    |    HOST1    |                                   |    HOST2    |
 *    |             |                                   |             |
 *    +------^------+                                   +------^------+
 *           |                                                 |
 *           |                                                 |
 * +---------|-------------------------------------------------|---------+
 * |  +------v------+                                   +------v------+  |
 * |  |             |                                   |             |  |
 * |  |     EP      |                                   |     EP      |  |
 * |  | CONTROLLER1 |                                   | CONTROLLER2 |  |
 * |  |             <----------------------------------->             |  |
 * |  |             |                                   |             |  |
 * |  |             |                                   |             |  |
 * |  |             |  SoC With Multiple EP Instances   |             |  |
 * |  |             |  (Configured using NTB Function)  |             |  |
 * |  +-------------+                                   +-------------+  |
 * +---------------------------------------------------------------------+
 */

/*
 * [한국어 설명] 엔드포인트 컨트롤러 두 개로 두 호스트를 잇는 NTB 함수 드라이버
 *               (pci-epf-ntb.c)
 *
 * === 파일의 역할 ===
 * 한 SoC 안에 있는 PCIe 엔드포인트 컨트롤러(EPC) 두 개를 하나의 엔드포인트
 * 함수(EPF) 드라이버가 동시에 붙잡아, 서로 다른 두 호스트(HOST1, HOST2)를
 * 잇는 NTB(Non-Transparent Bridge, 비투명 브리지)를 소프트웨어로 흉내 낸다.
 * 두 호스트에게 이 SoC 는 각각 평범한 PCIe 엔드포인트로 보이며, BAR 를 통해
 * 제어 레지스터, 스크래치패드, 도어벨, 메모리 윈도우가 노출된다.
 * NTB 가 "비투명" 이라 불리는 이유는 두 호스트가 서로의 PCI 열거 공간을
 * 그대로 보지 못하고, 이 다리가 열어 준 창과 신호로만 대화하기 때문이다.
 * 이 파일이 하는 일은 결국 세 가지다 — 두 호스트에게 같은 물리 메모리를
 * 서로 다른 BAR 로 겹쳐 보여 주기(스크래치패드), 한쪽의 쓰기가 다른 쪽
 * 메모리로 흘러가도록 아웃바운드 창을 걸어 주기(메모리 윈도우), 그리고
 * 한쪽의 쓰기가 다른 쪽 인터럽트가 되도록 MSI/MSI-X 주소를 창에 걸어 주기
 * (도어벨)다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 컨텍스트는 호스트 커널이 아니라 엔드포인트 쪽 SoC 의 커널 모듈이다.
 * 사용자가 configfs(drivers/pci/endpoint/pci-ep-cfs.c)에서 pci_epf_ntb 함수를
 * 만들고 primary/secondary 두 EPC 에 각각 링크를 걸면,
 * pci_epc_add_epf() → pci_epf_bind() → 이 파일의 epf_ntb_bind() 로 들어온다.
 * bind 는 EPC 두 개가 모두 붙기 전에는 아무 것도 하지 않고 0 을 돌려주며,
 * 두 번째 링크에서야 BAR 배정 → 메모리 할당 → 하드웨어 설정 → 워크 기동의
 * 전체 초기화를 수행한다.
 * 런타임에는 인터페이스마다 하나씩 도는 5ms 주기 delayed work
 * (epf_ntb_cmd_handler)가 각 호스트가 제어 영역에 써 넣은 command 필드를
 * 폴링해 처리한다. 호스트에서 이쪽으로 오는 인터럽트 경로가 없기 때문에
 * 폴링 외에 방법이 없다.
 * 이 파일은 drivers/ntb 서브시스템에 등록하지 않는다. NTB 클라이언트는
 * 두 호스트 쪽 커널에서 돈다(Kconfig 의 PCI_EPF_NTB 가 NTB 를 depends 하지
 * 않는 것, 그리고 이 파일에 linux/ntb.h 인클루드가 없는 것이 근거다).
 * 형제 파일인 pci-epf-vntb.c 와의 차이가 여기서 갈린다 — 그쪽은 EPC 하나에
 * 로컬 가상 NTB 디바이스를 붙여 drivers/ntb 에 직접 등록한다.
 *
 * === 타 모듈과의 연결 ===
 * 아래로는 엔드포인트 코어 세 파일에 의존한다.
 *   drivers/pci/endpoint/pci-epc-core.c — pci_epc_set_bar/clear_bar,
 *     pci_epc_map_addr/unmap_addr, pci_epc_set_msi/set_msix,
 *     pci_epc_map_msi_irq, pci_epc_raise_irq, pci_epc_get_features,
 *     pci_epc_get_next_free_bar, pci_epc_write_header.
 *     이 호출들의 컨트롤러 콜백은 모두 epc->lock 뮤텍스 아래에서 실행된다.
 *   drivers/pci/endpoint/pci-epf-core.c — pci_epf_alloc_space(내부에서
 *     dma_alloc_coherent 를 쓴다), pci_epf_free_space, 드라이버 등록.
 *   drivers/pci/endpoint/pci-epc-mem.c — pci_epc_mem_alloc_addr/free_addr 로
 *     상대편 컨트롤러의 아웃바운드 주소 공간을 빌린다.
 * 위로는 configfs 속성(spad_count, db_count, num_mws, mw1~mw4)이 유일한
 * 사용자 설정 입구다.
 * 데이터 흐름은 이렇다. HOST2 가 자기 PCI 주소와 크기를 제어 영역의
 * ctrl->addr/ctrl->size 에 쓰고 COMMAND_CONFIGURE_MW 를 날리면, 이 드라이버가
 * HOST1 의 BAR 뒤에 놓인 아웃바운드 창을 그 PCI 주소로 매핑한다. 그러면
 * HOST1 이 자기 BAR 에 쓴 데이터가 창을 지나 HOST2 의 메모리에 도착한다.
 * 도어벨도 같은 구조다 — 창의 목적지가 메모리가 아니라 상대 호스트의
 * MSI/MSI-X 주소일 뿐이다.
 * 공유하는 핵심 자료구조는 struct epf_ntb_ctrl 이다. 이 구조체는 __packed 로
 * 선언되어 BAR 를 통해 호스트 메모리 공간에 그대로 노출되므로, 사실상
 * 호스트 쪽 드라이버와의 ABI 다.
 *
 * === 주요 함수/구조체 요약 ===
 * - epf_ntb_bind(): EPC 두 개가 모두 바인딩된 뒤 전체 초기화를 수행하는 진입점.
 * - epf_ntb_cmd_handler(): 5ms 폴링 워크. 호스트 명령(도어벨/창/링크)을 처리한다.
 * - epf_ntb_configure_mw(): 상대편 아웃바운드 창을 호스트가 지정한 PCI 주소에 건다.
 * - epf_ntb_configure_msi() / epf_ntb_configure_msix(): 같은 일을 MSI/MSI-X
 *   주소를 목적지로 삼아 수행한다 — 이것이 도어벨의 실체다.
 * - epf_ntb_config_spad_bar_alloc(): 제어 영역과 스크래치패드의 크기를 정하고
 *   실제 메모리를 잡는 가장 계산이 복잡한 함수.
 * - epf_ntb_peer_spad_bar_set(): 상대편 스크래치패드를 이쪽 BAR 로 겹쳐 노출한다.
 * - struct epf_ntb: 함수 전체 상태. epc[2] 가 두 인터페이스이고,
 *   ntb->epc[!type] 이 "상대편" 을 뜻하는 관용구가 이 배열에서 나온다.
 * - struct epf_ntb_epc: 인터페이스 하나의 상태. BAR 배정표, 아웃바운드 창 주소,
 *   컨트롤러 능력표, 폴링 워크가 들어 있다.
 * - struct epf_ntb_ctrl: 호스트가 BAR 로 보게 되는 공유 레지스터. 명령/인자/
 *   상태/링크/창 주소/도어벨 데이터가 한 구조체에 모여 있다.
 */

/* [한국어] delay.h: msecs_to_jiffies() 로 폴링 주기를 밀리초에서 지연 단위로 바꾼다. */
#include <linux/delay.h>
/* [한국어] io.h: __iomem 표시와 MMIO 접근 도우미. 아웃바운드 창 주소가 __iomem 이다. */
#include <linux/io.h>
/* [한국어] module.h: module_init/module_exit 와 MODULE_ 매크로. */
#include <linux/module.h>
/* [한국어] slab.h: devm_kzalloc 계열 할당. */
#include <linux/slab.h>

/* [한국어] pci-epc.h: 엔드포인트 컨트롤러 API. BAR 설정, 아웃바운드 매핑,
 * MSI/MSI-X 설정, 인터럽트 발생이 모두 여기서 온다.
 * (이 트리에는 include/ 가 거의 체크아웃되어 있지 않아 헤더 원문은 볼 수 없고,
 * 구현은 drivers/pci/endpoint/pci-epc-core.c 에서 확인할 수 있다.) */
#include <linux/pci-epc.h>
/* [한국어] pci-epf.h: 엔드포인트 함수 API. 드라이버 등록, BAR 메모리 할당,
 * pci_epf_bar 구조체 정의가 여기 있다. */
#include <linux/pci-epf.h>

/* [한국어] 명령 폴링 워크가 도는 전용 작업 큐. 모듈 하나에 하나뿐이라
 * 두 인터페이스의 워크가 이 큐를 공유한다.
 * 생성은 epf_ntb_init(), 해제는 epf_ntb_exit() 이다. */
static struct workqueue_struct *kpcintb_workqueue;

/* [한국어] 아래 여섯 개는 호스트가 command 필드에 써 넣는 명령 코드다.
 * 호스트 쪽 NTB 드라이버와 값을 맞춰야 하는 약속이며,
 * 이 트리에는 그 상대편(drivers/ntb)이 체크아웃되어 있지 않다.
 * 
 *   1 CONFIGURE_DOORBELL: 도어벨을 설정하라. 인자에 개수와 MSI-X 플래그.
 *   2 TEARDOWN_DOORBELL:  도어벨 매핑을 걷어라.
 *   3 CONFIGURE_MW:       메모리 윈도우를 걸어라. 인자가 창 번호.
 *   4 TEARDOWN_MW:        메모리 윈도우를 걷어라.
 *   5 LINK_UP:            이 호스트가 링크를 올렸다.
 *   6 LINK_DOWN:          이 호스트가 링크를 내렸다. */
#define COMMAND_CONFIGURE_DOORBELL	1
#define COMMAND_TEARDOWN_DOORBELL	2
#define COMMAND_CONFIGURE_MW		3
#define COMMAND_TEARDOWN_MW		4
#define COMMAND_LINK_UP			5
#define COMMAND_LINK_DOWN		6

/* [한국어] 명령 처리 결과 코드. 0 은 "아직 처리 안 됨" 을 뜻하도록 비워 두고
 * 1 을 성공, 2 를 실패로 쓴다. */
#define COMMAND_STATUS_OK		1
#define COMMAND_STATUS_ERROR		2

/* [한국어] link_status 필드의 비트 0. 링크가 올라가 있음을 뜻한다. */
#define LINK_STATUS_UP			BIT(0)

/* [한국어] 기본값과 한계값들.
 *   SPAD_COUNT(64), DB_COUNT(4): 기본값으로 두려던 상수로 보이나
 *     이 파일 어디에서도 참조되지 않는다(정의 한 곳뿐).
 *   NTB_MW_OFFSET(2): enum epf_ntb_bar 에서 BAR_CONFIG, BAR_PEER_SPAD
 *     두 칸을 건너뛰어야 창 BAR 가 시작되므로, 창 번호에 이 값을 더한다.
 *   DB_COUNT_MASK: CONFIGURE_DOORBELL 인자의 하위 16비트가 도어벨 개수.
 *   MSIX_ENABLE: 같은 인자의 비트 16 이 서면 MSI-X 를 쓰라는 뜻.
 *   MAX_DB_COUNT(32): db_data[]/db_offset[] 배열 칸 수와 같아야 한다.
 *   MAX_MW(4): mws_size[] 배열 칸 수와 같아야 한다. */
#define SPAD_COUNT			64
#define DB_COUNT			4
#define NTB_MW_OFFSET			2
#define DB_COUNT_MASK			GENMASK(15, 0)
#define MSIX_ENABLE			BIT(16)
#define MAX_DB_COUNT			32
#define MAX_MW				4

/* [한국어] NTB 구성요소를 가리키는 논리 인덱스. 실제 BAR 번호는 컨트롤러마다
 * 다를 수 있으므로, 이 인덱스로 epf_ntb_bar[] 표를 찾아 실제 번호를 얻는다.
 * 순서가 중요하다 — 필수 세 칸이 앞에, 선택 창이 뒤에 온다. */
enum epf_ntb_bar {
	/* [한국어] 제어 영역 + 자기 스크래치패드가 놓이는 BAR. 이 BAR 뒤의 메모리가
	 * 호스트가 명령을 써 넣는 창이 된다. */
	BAR_CONFIG,
	/* [한국어] 상대편의 스크래치패드를 들여다보는 BAR. 물리 메모리는 상대편 것이다. */
	BAR_PEER_SPAD,
	/* [한국어] 도어벨 영역과 첫 메모리 윈도우가 함께 놓이는 BAR.
	 * 앞쪽이 도어벨, mw1_offset 뒤가 창이다. */
	BAR_DB_MW1,
	/* [한국어] 두 번째 메모리 윈도우 전용 BAR. */
	BAR_MW2,
	/* [한국어] 세 번째 메모리 윈도우 전용 BAR. */
	BAR_MW3,
	/* [한국어] 네 번째 메모리 윈도우 전용 BAR. BAR 가 모자라면 배정되지 않는다. */
	BAR_MW4,
};

/* [한국어] NTB 함수 전체의 상태. EPF 디바이스 하나에 하나씩 있다. */
struct epf_ntb {
	/* [한국어] 쓸 메모리 윈도우 개수.
	 * 설정자: 사용자가 configfs 의 num_mws 에 쓰거나,
	 *   BAR 가 모자라면 epf_ntb_init_epc_bar_interface() 가 줄인다.
	 * 읽는 자: BAR 선택, 창 생성, 공유 레지스터 채우기.
	 * 값 범위: 0 ~ MAX_MW(4).
	 * 동기화: bind 전에 설정되고 그 뒤에는 사실상 읽기 전용이다. */
	u32 num_mws;
	/* [한국어] 도어벨 개수.
	 * 설정자: 사용자가 configfs 의 db_count 에 쓴다.
	 * 읽는 자: MSI/MSI-X 설정, 도어벨 영역 크기 계산.
	 * 값 범위: 0 ~ MAX_DB_COUNT(32).
	 * 동기화: bind 전에 설정된다. */
	u32 db_count;
	/* [한국어] 스크래치패드 워드 개수.
	 * 설정자: 사용자가 configfs 의 spad_count 에 쓴다.
	 * 읽는 자: epf_ntb_config_spad_bar_alloc().
	 * 값 범위: 0 이상. 상대편 BAR 가 고정 크기면 줄어들 수 있다.
	 * 동기화: bind 전에 설정된다. */
	u32 spad_count;
	/* [한국어] 이 상태가 매달린 EPF 디바이스.
	 * 설정자: epf_ntb_probe().
	 * 읽는 자: 로그 대상, EPC 포인터 획득, 메모리 할당/해제.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 수명이 EPF 디바이스와 같다. */
	struct pci_epf *epf;
	/* [한국어] 각 메모리 윈도우의 크기(바이트).
	 * 설정자: 사용자가 configfs 의 mw1~mw4 에 쓴다.
	 * 읽는 자: epf_ntb_db_mw_bar_init() 이 창을 만들 때,
	 *   epf_ntb_configure_mw() 가 호스트 요청 크기를 검증할 때.
	 * 값 범위: 0 이상. 0 이면 사실상 쓸 수 없는 창이 된다.
	 * 동기화: bind 전에 설정된다. */
	u64 mws_size[MAX_MW];
	/* [한국어] configfs 디렉터리를 나타내는 그룹. 구조체 안에 박혀 있어
	 *   별도 할당이 없다.
	 * 설정자: epf_ntb_add_cfs() 가 초기화한다.
	 * 읽는 자: to_epf_ntb() 매크로가 이 필드에서 거꾸로 구조체를 찾는다.
	 * 값 범위: configfs 가 관리하는 구조체.
	 * 동기화: configfs 내부 락이 지킨다. */
	struct config_group group;
	/* [한국어] 두 인터페이스의 상태. 인덱스가 곧 PRIMARY/SECONDARY 다.
	 *   ntb->epc[!type] 이 "상대편" 을 뜻하는 관용구가 이 배열 때문에 성립한다.
	 * 설정자: epf_ntb_epc_create_interface().
	 * 읽는 자: 거의 모든 함수.
	 * 값 범위: 두 칸 모두 채워져야 bind 가 성공한다.
	 * 동기화: bind 시점에 채워지고 이후 읽기 전용이다. */
	struct epf_ntb_epc *epc[2];
};

#define to_epf_ntb(epf_group) container_of((epf_group), struct epf_ntb, group)

/* [한국어] 인터페이스(=EPC 하나) 단위의 상태. 이 드라이버는 이런 구조체를
 * 두 개(PRIMARY/SECONDARY) 만들어 서로를 상대편으로 참조하게 한다. */
struct epf_ntb_epc {
	/* [한국어] 이 인터페이스의 PCI 물리 함수 번호.
	 * 설정자: epf_ntb_epc_create_interface() 가 epf->func_no 또는
	 *   epf->sec_epc_func_no 를 복사한다.
	 * 읽는 자: 모든 pci_epc_ 계열 호출.
	 * 값 범위: 컨트롤러가 지원하는 함수 번호.
	 * 동기화: bind 시점에 한 번만 쓰인다. */
	u8 func_no;
	/* [한국어] SR-IOV 가상 함수 번호.
	 * 설정자: 위와 같은 함수가 epf->vfunc_no 를 복사한다.
	 * 읽는 자: 모든 pci_epc_ 계열 호출과 config 헤더 쓰기 조건.
	 * 값 범위: 0 이면 물리 함수 자신.
	 * 동기화: bind 시점에 한 번만 쓰인다. */
	u8 vfunc_no;
	/* [한국어] 이 호스트가 링크를 올렸는가.
	 * 설정자: epf_ntb_cmd_handler() 가 COMMAND_LINK_UP/DOWN 에서 바꾼다.
	 * 읽는 자: 같은 핸들러가 양쪽이 모두 올라왔는지 볼 때.
	 * 값 범위: true/false. 초기값은 false(epf_ntb_epc_create_interface).
	 * 동기화: 두 인터페이스의 워크가 각각 자기 것만 쓰고 남의 것은 읽기만 한다. */
	bool linkup;
	/* [한국어] 도어벨을 MSI-X 로 설정했는가.
	 * 설정자: epf_ntb_configure_msix() 가 성공 시 true 로 만든다.
	 * 읽는 자: epf_ntb_link_up() 이 쏠 인터럽트 종류를 고를 때.
	 * 값 범위: true/false. 초기값 false 는 MSI 를 뜻한다.
	 * 동기화: 링크가 올라오기 전에 정해진다. */
	bool is_msix;
	/* [한국어] MSI-X 표가 놓인 BAR 번호.
	 * 설정자: epf_ntb_config_spad_bar_alloc() 이 config BAR 로 정한다.
	 * 읽는 자: epf_ntb_configure_interrupt() 가 pci_epc_set_msix() 에 넘긴다.
	 * 값 범위: 유효한 BAR 번호. MSI-X 를 지원하지 않으면 쓰이지 않는다.
	 * 동기화: 초기화 시점에만 쓰인다. */
	int msix_bar;
	/* [한국어] 확정된 스크래치패드 크기(바이트).
	 * 설정자: epf_ntb_config_spad_bar_alloc().
	 * 읽는 자: 상대편의 epf_ntb_peer_spad_bar_set() 이 BAR 크기로 쓴다.
	 * 값 범위: 정렬과 상대편 고정 BAR 크기를 반영한 값.
	 * 동기화: 초기화 시점에만 쓰인다. */
	u32 spad_size;
	/* [한국어] 이 인터페이스가 붙은 엔드포인트 컨트롤러.
	 * 설정자: epf_ntb_epc_create_interface() 가 epf->epc 또는 epf->sec_epc 를 넣는다.
	 * 읽는 자: 이 파일의 거의 모든 함수.
	 * 값 범위: 유효한 컨트롤러 포인터.
	 * 동기화: 컨트롤러 내부 상태는 epc->lock 이 지킨다. */
	struct pci_epc *epc;
	/* [한국어] 함수 전체 상태로 돌아가는 역방향 포인터.
	 * 설정자: epf_ntb_epc_create_interface().
	 * 읽는 자: cmd_handler 처럼 인터페이스 상태만 받은 함수가 상대편에
	 *   닿아야 할 때.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 수명이 EPF 디바이스와 같다. */
	struct epf_ntb *epf_ntb;
	/* [한국어] BAR 번호로 색인한 아웃바운드 창의 커널 가상 주소.
	 * 설정자: epf_ntb_alloc_peer_mem() 이 채우고 epf_ntb_free_peer_mem() 이 NULL 로 지운다.
	 * 읽는 자: 해제 경로. 실제 데이터 전송에는 쓰이지 않는다.
	 * 값 범위: 유효한 __iomem 포인터 또는 NULL.
	 * 동기화: bind/unbind 경로에서만 쓰인다. */
	void __iomem *mw_addr[6];
	/* [한국어] config BAR 안에서 MSI-X 표가 시작되는 오프셋.
	 * 설정자: epf_ntb_config_spad_bar_alloc().
	 * 읽는 자: epf_ntb_configure_interrupt() 와 epf_ntb_configure_msix().
	 * 값 범위: 8바이트 정렬된 값.
	 * 동기화: 초기화 시점에만 쓰인다. */
	size_t msix_table_offset;
	/* [한국어] 호스트와 공유하는 레지스터 영역의 시작(커널 가상 주소).
	 * 설정자: epf_ntb_config_spad_bar_alloc() 이 pci_epf_alloc_space() 결과를 넣는다.
	 * 읽는 자: cmd_handler 를 비롯한 거의 모든 함수.
	 * 값 범위: dma_alloc_coherent 로 잡힌 메모리. 해제 후 NULL 로 남지 않으므로
	 *   epf_ntb_config_spad_bar_free() 는 할당 여부를 이 값으로 판단한다.
	 * 동기화: 호스트와 동시에 접근하므로 명령 프로토콜의 순서 약속에 의존한다. */
	struct epf_ntb_ctrl *reg;
	/* [한국어] 이 인터페이스가 쓰는 pci_epf_bar 배열의 시작.
	 * 설정자: epf_ntb_epc_create_interface() 가 epf->bar 또는 epf->sec_epc_bar 를 넣는다.
	 * 읽는 자: BAR 를 걸고 걷는 모든 함수.
	 * 값 범위: 여섯 칸짜리 배열의 시작 주소.
	 * 동기화: bind/unbind 경로에서만 쓰인다. */
	struct pci_epf_bar *epf_bar;
	/* [한국어] 논리 구성요소(BAR_CONFIG 등)를 실제 BAR 번호로 옮기는 표.
	 * 설정자: epf_ntb_init_epc_bar_interface() 가 채운다.
	 * 읽는 자: BAR 를 다루는 모든 함수.
	 * 값 범위: 0 ~ 5, 또는 배정 실패 시 음수(NO_BAR).
	 * 동기화: bind 시점에 한 번만 쓰인다. */
	enum pci_barno epf_ntb_bar[6];
	/* [한국어] 5ms 주기 명령 폴링 워크.
	 * 설정자: epf_ntb_epc_init_interface() 가 초기화하고 큐에 넣는다.
	 * 읽는 자: 워크 함수 자신이 container_of 로 이 구조체를 되찾는다.
	 * 값 범위: 커널 워크큐가 관리하는 불투명 구조체.
	 * 동기화: 취소는 epf_ntb_epc_cleanup_interface() 의 cancel_delayed_work(). */
	struct delayed_work cmd_handler;
	/* [한국어] 이 인터페이스가 PRIMARY 인지 SECONDARY 인지.
	 * 설정자: epf_ntb_epc_create_interface().
	 * 읽는 자: cmd_handler 가 하위 함수에 넘길 때, 로그 문자열을 만들 때.
	 * 값 범위: PRIMARY_INTERFACE(0) 또는 SECONDARY_INTERFACE(1).
	 * 동기화: bind 시점에 한 번만 쓰인다. */
	enum pci_epc_interface_type type;
	/* [한국어] 이 컨트롤러의 능력표(예약 BAR, 정렬 요구, MSI/MSI-X 지원 여부).
	 * 설정자: epf_ntb_epc_create_interface() 가 pci_epc_get_features() 결과를 넣는다.
	 * 읽는 자: BAR 선택, 영역 크기 계산, 인터럽트 설정.
	 * 값 범위: 컨트롤러 드라이버가 제공하는 상수 구조체. const 라 수정 불가.
	 * 동기화: 읽기 전용이라 필요 없다. */
	const struct pci_epc_features *epc_features;
};

/* [한국어] 호스트와 이 드라이버가 공유하는 레지스터 영역의 배치.
 * 이 구조체가 그대로 BAR 로 노출되어 호스트가 직접 읽고 쓴다.
 * __packed 라 컴파일러가 패딩을 넣지 않으므로, 호스트 쪽 NTB 드라이버와
 * 바이트 단위로 배치가 일치한다 — 사실상 이 파일의 ABI 다. */
struct epf_ntb_ctrl {
	/* [한국어] 호스트 → EP 방향 명령 코드.
	 * 설정자: 호스트가 BAR 에 직접 쓴다.
	 * 읽는 자: epf_ntb_cmd_handler() 가 5ms 마다 폴링해 읽고 0 으로 지운다.
	 * 값 범위: COMMAND_CONFIGURE_DOORBELL(1) ~ COMMAND_LINK_DOWN(6), 0 은 없음.
	 * 동기화: 락이 없다. 호스트가 0 이 되기를 기다렸다가 다음 명령을 쓰는
	 *   약속으로만 보호된다. */
	u32	command;
	/* [한국어] 명령의 인자.
	 * 설정자: 호스트가 command 를 쓰기 전에 먼저 쓴다.
	 * 읽는 자: cmd_handler 가 command 를 읽은 직후에 읽는다.
	 * 값 범위: 명령마다 다르다. CONFIGURE_DOORBELL 이면 하위 16비트가 개수,
	 *   비트 16 이 MSI-X 플래그. CONFIGURE_MW/TEARDOWN_MW 면 창 번호.
	 * 동기화: command 와 같은 약속을 따른다. */
	u32	argument;
	/* [한국어] 명령 처리 결과.
	 * 설정자: cmd_handler 가 처리 후 쓴다.
	 * 읽는 자: 호스트가 폴링해 읽는다.
	 * 값 범위: COMMAND_STATUS_OK(1) 또는 COMMAND_STATUS_ERROR(2).
	 * 동기화: 없음. 호스트가 명령을 보낸 뒤 이 값이 바뀌기를 기다린다. */
	u16	command_status;
	/* [한국어] 링크 상태 비트맵.
	 * 설정자: epf_ntb_link_up() 이 비트를 세우거나 지운다.
	 * 읽는 자: 호스트가 자기 BAR 로 읽는다.
	 * 값 범위: 현재는 LINK_STATUS_UP(비트 0) 하나만 쓰인다.
	 * 동기화: 인터럽트를 쏘기 전에 갱신하는 순서로만 보장한다. */
	u16	link_status;
	/* [한국어] NTB 토폴로지.
	 * 설정자: 이 파일 어디에서도 쓰지 않는다 — 호스트 쪽 드라이버와의
	 *   약속을 위해 자리만 잡아 둔 필드다.
	 * 읽는 자: 호스트.
	 * 값 범위: 이 트리만으로는 확인할 수 없다(drivers/ntb 가 체크아웃되어 있지 않다).
	 * 동기화: 해당 없음. */
	u32	topology;
	/* [한국어] 메모리 윈도우를 걸 목적지 PCI 주소.
	 * 설정자: 호스트가 COMMAND_CONFIGURE_MW 를 보내기 전에 쓴다.
	 * 읽는 자: epf_ntb_configure_mw() 가 pci_epc_map_addr() 의 인자로 쓴다.
	 * 값 범위: 그 호스트가 관리하는 PCI 주소 공간의 유효한 주소.
	 * 동기화: 명령 프로토콜의 순서 약속으로만 보호된다. */
	u64	addr;
	/* [한국어] 그 창의 크기.
	 * 설정자: 호스트.
	 * 읽는 자: epf_ntb_configure_mw() 가 읽고, 우리가 준비한 창 크기보다
	 *   크면 거절한다.
	 * 값 범위: 0 초과, ntb->mws_size[mw] 이하.
	 * 동기화: addr 와 같다. */
	u64	size;
	/* [한국어] 이 EP 가 제공하는 메모리 윈도우 개수.
	 * 설정자: epf_ntb_config_spad_bar_alloc() 이 ntb->num_mws 를 복사해 넣는다.
	 * 읽는 자: 호스트가 창을 몇 개 쓸 수 있는지 판단할 때.
	 * 값 범위: 1 ~ MAX_MW(4). BAR 가 모자라면 줄어든 값이 들어간다.
	 * 동기화: 초기화 시점에 한 번만 쓰이고 이후 읽기 전용이다. */
	u32	num_mws;
	/* [한국어] 도어벨+MW1 BAR 안에서 MW1 이 시작되는 오프셋.
	 * 설정자: epf_ntb_db_mw_bar_init() 이 도어벨 영역 크기로 채운다.
	 * 읽는 자: 호스트, 그리고 epf_ntb_configure_mw()/teardown_mw() 자신.
	 * 값 범위: 도어벨 개수 x 도어벨 칸 크기를 창 크기로 올림한 값.
	 * 동기화: 초기화 시점에만 쓰인다. */
	u32	mw1_offset;
	/* [한국어] 제어 영역 시작점에서 스크래치패드까지의 거리.
	 * 설정자: epf_ntb_config_spad_bar_alloc().
	 * 읽는 자: 호스트, 그리고 epf_ntb_peer_spad_bar_set() 이 상대편 BAR 의
	 *   물리 주소를 계산할 때.
	 * 값 범위: 정렬을 반영한 제어 영역 크기.
	 * 동기화: 초기화 시점에만 쓰인다. */
	u32	spad_offset;
	/* [한국어] 실제로 쓸 수 있는 스크래치패드 워드 개수.
	 * 설정자: epf_ntb_config_spad_bar_alloc(). 상대편 BAR 가 고정 크기라
	 *   요청보다 작아질 수 있다.
	 * 읽는 자: 호스트.
	 * 값 범위: 0 ~ 사용자가 configfs 로 정한 spad_count.
	 * 동기화: 초기화 시점에만 쓰인다. */
	u32	spad_count;
	/* [한국어] 도어벨 한 칸의 크기(바이트).
	 * 설정자: epf_ntb_config_spad_bar_alloc() 이 컨트롤러 정렬 요구 또는 4 로 정한다.
	 * 읽는 자: 상대편 인터페이스의 epf_ntb_configure_msi()/msix() 가
	 *   창을 한 칸씩 나아갈 때, 그리고 호스트가 도어벨 i 의 주소를 계산할 때.
	 * 값 범위: 4 또는 컨트롤러의 align 값.
	 * 동기화: 초기화 시점에만 쓰인다. */
	u32	db_entry_size;
	/* [한국어] 도어벨 i 를 울리려면 써야 할 값.
	 * 설정자: 상대편을 설정하는 epf_ntb_configure_msi()/msix() 가 채운다 —
	 *   즉 이 배열은 "내가 쓰는 값" 이 아니라 "상대가 나에게 알려 준 값" 이다.
	 * 읽는 자: 호스트가 도어벨을 울릴 때.
	 * 값 범위: MSI 면 기준 데이터에 벡터 번호를 OR 한 값, MSI-X 면 표의 데이터.
	 * 동기화: 링크가 올라오기 전에만 쓰인다. */
	u32	db_data[MAX_DB_COUNT];
	/* [한국어] 도어벨 i 를 울릴 때 창 시작점에서 더해야 할 오프셋.
	 * 설정자: db_data 와 같은 함수들.
	 * 읽는 자: 호스트.
	 * 값 범위: MSI 면 모든 항목이 같고, MSI-X 면 정렬 때 잘려 나간 하위 비트.
	 * 동기화: db_data 와 같다. */
	u32	db_offset[MAX_DB_COUNT];
/* [한국어] __packed: 컴파일러가 패딩을 넣지 못하게 한다. 이 구조체는 호스트와
 * 바이트 배치를 공유하므로 패딩이 들어가면 양쪽 해석이 어긋난다. */
} __packed;

/* [한국어] 이 함수가 호스트에게 내보일 PCI config 헤더의 원본.
 * epf_ntb_probe() 가 epf->header 에 연결하고,
 * epf_ntb_epc_init_interface() 가 pci_epc_write_header() 로 실제로 쓴다. */
static struct pci_epf_header epf_ntb_header = {
	/* [한국어] 벤더 ID 를 PCI_ANY_ID 로 둔다. 실제 값은 컨트롤러 드라이버나
	 * 디바이스 트리 쪽에서 채워지는 것을 기대한 것으로 읽힌다.
	 * (vntb 쪽은 configfs 로 vendor/device 를 받는 것과 대비된다.) */
	.vendorid	= PCI_ANY_ID,
	/* [한국어] 디바이스 ID 도 마찬가지. */
	.deviceid	= PCI_ANY_ID,
	/* [한국어] 기본 클래스 코드를 "메모리 컨트롤러" 로 선언한다.
	 * NTB 는 결국 메모리 창을 노출하는 장치라 이 분류를 쓴다. */
	.baseclass_code	= PCI_BASE_CLASS_MEMORY,
	/* [한국어] 레거시 INTx 핀을 INTA 로 선언한다. 실제 도어벨은 MSI/MSI-X 를 쓰지만
	 * config 헤더의 이 필드는 채워 두어야 한다. */
	.interrupt_pin	= PCI_INTERRUPT_INTA,
};

/**
 * epf_ntb_link_up() - Raise link_up interrupt to both the hosts
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @link_up: true or false indicating Link is UP or Down
 *
 * Once NTB function in HOST1 and the NTB function in HOST2 invoke
 * ntb_link_enable(), this NTB function driver will trigger a link event to
 * the NTB client in both the hosts.
 */
/* [한국어]
 * epf_ntb_link_up - 두 호스트 모두에게 링크 상태 변화를 알린다
 *
 * @ntb: HOST1 과 HOST2 를 잇는 NTB 함수 전체 상태
 * @link_up: true 면 링크 업, false 면 링크 다운
 * @return: 0 성공. 인터럽트 발생 실패 시 그 오류 코드를 그대로 올린다.
 *
 * 왜 필요한가: NTB 는 양쪽 호스트가 서로의 존재를 알아야 의미가 있다.
 * 두 호스트의 NTB 클라이언트가 각각 링크를 올리면(COMMAND_LINK_UP),
 * 이 함수가 양쪽 공유 레지스터의 link_status 를 갱신하고 인터럽트를 쏜다.
 * 이 알림이 없으면 호스트 쪽 드라이버는 상대가 준비됐는지 영영 알 수 없다.
 *
 * 동작 단계: PRIMARY, SECONDARY 를 차례로 돌면서
 *   (1) link_status 비트를 갱신하고 — 인터럽트보다 먼저 해야 인터럽트를
 *       받은 호스트가 곧바로 읽었을 때 올바른 값을 본다,
 *   (2) 그 인터페이스에 설정된 종류(MSI 또는 MSI-X)로 1번 벡터를 울린다.
 *
 * 실행 컨텍스트: epf_ntb_cmd_handler() 워크 안, 즉 프로세스 컨텍스트.
 * pci_epc_raise_irq() 안에서 epc->lock 뮤텍스를 잡으므로 잠들 수 있다.
 *
 * 에러 경로: 한쪽에서 실패하면 곧바로 반환한다 — 이미 갱신된 반대쪽
 * link_status 는 되돌리지 않으므로, 링크가 반쪽만 올라간 상태가 남는다.
 * 호출자는 COMMAND_STATUS_ERROR 로 호스트에게 알린다.
 *
 * 호출 체인:
 *   epf_ntb_cmd_handler() → [epf_ntb_link_up] → pci_epc_raise_irq()
 */
static int epf_ntb_link_up(struct epf_ntb *ntb, bool link_up)
{
	/* [한국어] 두 인터페이스를 순회할 반복자. */
	enum pci_epc_interface_type type;
	/* [한국어] 각 인터페이스의 상태. */
	struct epf_ntb_epc *ntb_epc;
	/* [한국어] 각 인터페이스의 공유 레지스터. */
	struct epf_ntb_ctrl *ctrl;
	/* [한국어] 쏘아야 할 인터럽트 종류(MSI 또는 MSI-X). */
	unsigned int irq_type;
	/* [한국어] 인터럽트를 쏠 컨트롤러. */
	struct pci_epc *epc;
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] 이 인터페이스가 MSI-X 로 설정되어 있는가. */
	bool is_msix;
	/* [한국어] 실패 코드. */
	int ret;

	/* [한국어] 두 호스트 모두에게 알려야 하므로 PRIMARY, SECONDARY 를 다 돈다.
	 * NTB 링크는 양쪽이 같은 상태를 보아야 의미가 있기 때문이다. */
	for (type = PRIMARY_INTERFACE; type <= SECONDARY_INTERFACE; type++) {
		/* [한국어] 이 인터페이스의 상태. */
		ntb_epc = ntb->epc[type];
		/* [한국어] 이 인터페이스의 컨트롤러. */
		epc = ntb_epc->epc;
		/* [한국어] 함수 번호. */
		func_no = ntb_epc->func_no;
		/* [한국어] 가상 함수 번호. */
		vfunc_no = ntb_epc->vfunc_no;
		/* [한국어] MSI-X 로 설정되었는지. epf_ntb_configure_msix() 가 이 값을 세운다. */
		is_msix = ntb_epc->is_msix;
		/* [한국어] 이 호스트가 읽을 공유 레지스터. */
		ctrl = ntb_epc->reg;
		/* [한국어] 링크를 올리는 중인가. */
		if (link_up)
			/* [한국어] link_status 의 비트 0 을 세운다. 호스트는 자기 BAR 로 이 필드를 읽어
			 * 링크 상태를 안다. 인터럽트보다 먼저 상태를 갱신해야, 인터럽트를 받은
			 * 호스트가 곧바로 읽었을 때 올바른 값을 본다. */
			ctrl->link_status |= LINK_STATUS_UP;
		else
			/* [한국어] 링크를 내리는 중이면 같은 비트를 지운다. */
			ctrl->link_status &= ~LINK_STATUS_UP;
		/* [한국어] 설정된 방식에 맞는 인터럽트 종류를 고른다. 종류가 맞지 않으면
		 * 컨트롤러가 엉뚱한 레지스터를 보게 된다. */
		irq_type = is_msix ? PCI_IRQ_MSIX : PCI_IRQ_MSI;
		/* [한국어] 인터럽트를 쏜다. 마지막 인자 1 은 벡터 번호이며 1-기반이다 —
		 * 컨트롤러 드라이버가 내부에서 1 을 빼 0번 벡터로 쓴다
		 * (drivers/pci/controller/dwc/pcie-designware-ep.c 의 interrupt_num - 1).
		 * 즉 링크 이벤트는 언제나 첫 번째 벡터로 알린다. */
		ret = pci_epc_raise_irq(epc, func_no, vfunc_no, irq_type, 1);
		/* [한국어] 한쪽이라도 실패하면 링크가 반쪽만 올라간 상태가 되므로 즉시 중단한다. */
		if (ret) {
			dev_err(&epc->dev,
				"%s intf: Failed to raise Link Up IRQ\n",
				pci_epc_interface_string(type));
			return ret;
		}
	}

	return 0;
}

/**
 * epf_ntb_configure_mw() - Configure the Outbound Address Space for one host
 *   to access the memory window of other host
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @type: PRIMARY interface or SECONDARY interface
 * @mw: Index of the memory window (either 0, 1, 2 or 3)
 *
 * +-----------------+    +---->+----------------+-----------+-----------------+
 * |       BAR0      |    |     |   Doorbell 1   +-----------> MSI|X ADDRESS 1 |
 * +-----------------+    |     +----------------+           +-----------------+
 * |       BAR1      |    |     |   Doorbell 2   +---------+ |                 |
 * +-----------------+----+     +----------------+         | |                 |
 * |       BAR2      |          |   Doorbell 3   +-------+ | +-----------------+
 * +-----------------+----+     +----------------+       | +-> MSI|X ADDRESS 2 |
 * |       BAR3      |    |     |   Doorbell 4   +-----+ |   +-----------------+
 * +-----------------+    |     |----------------+     | |   |                 |
 * |       BAR4      |    |     |                |     | |   +-----------------+
 * +-----------------+    |     |      MW1       +---+ | +-->+ MSI|X ADDRESS 3||
 * |       BAR5      |    |     |                |   | |     +-----------------+
 * +-----------------+    +---->-----------------+   | |     |                 |
 *   EP CONTROLLER 1            |                |   | |     +-----------------+
 *                              |                |   | +---->+ MSI|X ADDRESS 4 |
 *                              +----------------+   |       +-----------------+
 *                      (A)      EP CONTROLLER 2     |       |                 |
 *                                 (OB SPACE)        |       |                 |
 *                                                   +------->      MW1        |
 *                                                           |                 |
 *                                                           |                 |
 *                                                   (B)     +-----------------+
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           +-----------------+
 *                                                           PCI Address Space
 *                                                           (Managed by HOST2)
 *
 * This function performs stage (B) in the above diagram (see MW1) i.e., map OB
 * address space of memory window to PCI address space.
 *
 * This operation requires 3 parameters
 *  1) Address in the outbound address space
 *  2) Address in the PCI Address space
 *  3) Size of the address region to be mapped
 *
 * The address in the outbound address space (for MW1, MW2, MW3 and MW4) is
 * stored in epf_bar corresponding to BAR_DB_MW1 for MW1 and BAR_MW2, BAR_MW3
 * BAR_MW4 for rest of the BARs of epf_ntb_epc that is connected to HOST1. This
 * is populated in epf_ntb_alloc_peer_mem() in this driver.
 *
 * The address and size of the PCI address region that has to be mapped would
 * be provided by HOST2 in ctrl->addr and ctrl->size of epf_ntb_epc that is
 * connected to HOST2.
 *
 * Please note Memory window1 (MW1) and Doorbell registers together will be
 * mapped to a single BAR (BAR2) above for 32-bit BARs. The exact BAR that's
 * used for Memory window (MW) can be obtained from epf_ntb_bar[BAR_DB_MW1],
 * epf_ntb_bar[BAR_MW2], epf_ntb_bar[BAR_MW2], epf_ntb_bar[BAR_MW2].
 */
/* [한국어]
 * epf_ntb_configure_mw - 한 호스트가 상대 호스트 메모리에 닿도록 아웃바운드 창을 건다
 *
 * @ntb: 함수 전체 상태
 * @type: 명령을 보낸 쪽 인터페이스(PRIMARY 또는 SECONDARY)
 * @mw: 창 번호(0~3)
 * @return: 0 성공, -EINVAL 이면 호스트가 요청한 크기가 준비된 창보다 큼,
 *          그 밖에는 pci_epc_map_addr() 의 실패 코드
 *
 * 왜 필요한가: NTB 의 핵심 기능이 메모리 윈도우다. 상대 호스트가 자기
 * BAR 에 쓴 데이터가 이 호스트의 실제 메모리에 도달하려면, 그 BAR 뒤에
 * 있는 아웃바운드 창을 이 호스트가 지정한 PCI 주소로 매핑해야 한다.
 *
 * 동작 단계:
 *   (1) 상대편 인터페이스(!type)에서 이 창에 배정된 BAR 를 찾는다.
 *       창 번호에 NTB_MW_OFFSET(2)을 더해 논리 BAR 인덱스로 바꾼다.
 *   (2) 그 BAR 뒤의 아웃바운드 물리 주소를 얻는다. 첫 창이면 도어벨
 *       영역 크기(mw1_offset)만큼 건너뛴다.
 *   (3) 호스트가 공유 레지스터에 써 둔 목적지 주소/크기를 읽고 크기를 검증한다.
 *   (4) pci_epc_map_addr() 로 실제 매핑을 건다.
 *
 * 실행 컨텍스트: cmd_handler 워크(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 에러 경로: 크기 검증 실패는 매핑을 시도조차 하지 않고 -EINVAL 로 빠진다.
 * 어느 쪽이든 반환값이 COMMAND_STATUS_ERROR 로 호스트에게 전달된다.
 *
 * 호출 체인:
 *   epf_ntb_cmd_handler() → [epf_ntb_configure_mw] → pci_epc_map_addr()
 */
static int epf_ntb_configure_mw(struct epf_ntb *ntb,
				enum pci_epc_interface_type type, u32 mw)
{
	/* [한국어] 상대편과 이쪽 인터페이스의 상태. */
	struct epf_ntb_epc *peer_ntb_epc, *ntb_epc;
	/* [한국어] 상대편 창 BAR 의 서술자. */
	struct pci_epf_bar *peer_epf_bar;
	/* [한국어] 상대편 BAR 번호. */
	enum pci_barno peer_barno;
	/* [한국어] 이쪽 공유 레지스터 — 호스트가 써 넣은 목적지 주소/크기를 읽는다. */
	struct epf_ntb_ctrl *ctrl;
	/* [한국어] 매핑할 아웃바운드 창의 물리 주소(이 SoC 관점의 주소). */
	phys_addr_t phys_addr;
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] 매핑을 걸 컨트롤러(이쪽). */
	struct pci_epc *epc;
	/* [한국어] 호스트가 알려 준 목적지 PCI 주소와 크기. */
	u64 addr, size;
	/* [한국어] 실패 코드. goto 로 건너뛰는 경로가 있어 미리 0 으로 둔다. */
	int ret = 0;

	/* [한국어] 이쪽 인터페이스 — 명령을 보낸 호스트 쪽이다. */
	ntb_epc = ntb->epc[type];
	/* [한국어] 로그와 매핑에 쓸 컨트롤러. */
	epc = ntb_epc->epc;

	/* [한국어] 상대편 인터페이스. !type 으로 PRIMARY 와 SECONDARY 를 뒤집는다. */
	peer_ntb_epc = ntb->epc[!type];
	/* [한국어] 창 번호를 논리 BAR 인덱스로 바꾼다(NTB_MW_OFFSET 만큼 건너뛴다).
	 * 이 BAR 는 상대편에게 노출된 BAR 이고, 그 뒤에 있는 아웃바운드 창이
	 * 지금 매핑할 대상이다. */
	peer_barno = peer_ntb_epc->epf_ntb_bar[mw + NTB_MW_OFFSET];
	/* [한국어] 그 BAR 의 서술자. */
	peer_epf_bar = &peer_ntb_epc->epf_bar[peer_barno];

	/* [한국어] 창의 시작 물리 주소. epf_ntb_alloc_peer_mem() 이 채워 둔 값이다. */
	phys_addr = peer_epf_bar->phys_addr;
	/* [한국어] 이쪽 공유 레지스터. */
	ctrl = ntb_epc->reg;
	/* [한국어] 호스트가 써 넣은 목적지 PCI 주소. 이 호스트가 "여기로 보내라" 고
	 * 지정한 자기 메모리의 주소다. */
	addr = ctrl->addr;
	/* [한국어] 매핑할 크기. 역시 호스트가 지정한다. */
	size = ctrl->size;
	/* [한국어] 첫 창인가? 첫 BAR 는 앞쪽을 도어벨이 쓰고 있다. */
	if (mw + NTB_MW_OFFSET == BAR_DB_MW1)
		/* [한국어] 도어벨 영역만큼 건너뛴 지점이 창의 실제 시작이다. */
		phys_addr += ctrl->mw1_offset;

	/* [한국어] 호스트가 요청한 크기가 우리가 준비한 창보다 크면 매핑할 수 없다.
	 * 호스트가 보낸 값을 그대로 믿지 않고 반드시 검사한다. */
	if (size > ntb->mws_size[mw]) {
		/* [한국어] 어느 인터페이스의 몇 번 창에서 났는지, 요청/지원 크기와 함께 남긴다. */
		dev_err(&epc->dev,
			"%s intf: MW: %d Req Sz:%llxx > Supported Sz:%llx\n",
			pci_epc_interface_string(type), mw, size,
			ntb->mws_size[mw]);
		/* [한국어] 크기가 맞지 않으므로 -EINVAL 로 거절한다. */
		ret = -EINVAL;
		/* [한국어] 매핑을 시도하지 않고 반환 지점으로 간다. */
		goto err_invalid_size;
	}

	/* [한국어] 이쪽 함수 번호. */
	func_no = ntb_epc->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb_epc->vfunc_no;

	/* [한국어] 핵심 호출. 이쪽 컨트롤러의 아웃바운드 주소 phys_addr 를 상대 호스트가
	 * 관리하는 PCI 주소 addr 에 size 만큼 건다. 이 매핑이 서면,
	 * 상대 호스트가 자기 BAR 에 쓴 데이터가 이 창을 통해 이쪽 호스트의
	 * addr 로 흘러간다. epc->lock 뮤텍스 아래에서 컨트롤러 콜백이 불린다. */
	ret = pci_epc_map_addr(epc, func_no, vfunc_no, phys_addr, addr, size);
	/* [한국어] 매핑 실패를 알린다. 이 경우에도 ret 가 그대로 호출자에게 올라가
	 * COMMAND_STATUS_ERROR 로 호스트에게 전달된다. */
	if (ret)
		dev_err(&epc->dev,
			"%s intf: Failed to map memory window %d address\n",
			pci_epc_interface_string(type), mw);

/* [한국어] 크기 검사 실패용 레이블. 매핑을 시도하지 않은 경로다. */
err_invalid_size:

	return ret;
}

/**
 * epf_ntb_teardown_mw() - Teardown the configured OB ATU
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @type: PRIMARY interface or SECONDARY interface
 * @mw: Index of the memory window (either 0, 1, 2 or 3)
 *
 * Teardown the configured OB ATU configured in epf_ntb_configure_mw() using
 * pci_epc_unmap_addr()
 */
/* [한국어]
 * epf_ntb_teardown_mw - configure_mw 가 건 아웃바운드 매핑을 걷는다
 *
 * @ntb: 함수 전체 상태
 * @type: 명령을 보낸 쪽 인터페이스
 * @mw: 창 번호(0~3)
 * @return: 없음. 실패를 알릴 방법이 없어 호출자는 항상 성공으로 보고한다.
 *
 * 왜 필요한가: 호스트가 창을 다 쓰고 나면 매핑을 걷어야 컨트롤러의
 * 아웃바운드 ATU 항목이 풀린다. 걷지 않으면 항목이 새어 나가 다음
 * 설정이 실패할 수 있다.
 *
 * 동작 단계: configure_mw 와 똑같이 물리 주소를 다시 계산한 뒤
 * pci_epc_unmap_addr() 에 넘긴다. 컨트롤러는 이 주소로 어느 항목을
 * 걷을지 찾으므로, 설정 때와 한 바이트도 다르면 안 된다 — 첫 창의
 * mw1_offset 을 여기서도 똑같이 더하는 이유가 그것이다.
 *
 * 실행 컨텍스트: cmd_handler 워크(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 호출 체인:
 *   epf_ntb_cmd_handler() → [epf_ntb_teardown_mw] → pci_epc_unmap_addr()
 */
static void epf_ntb_teardown_mw(struct epf_ntb *ntb,
				enum pci_epc_interface_type type, u32 mw)
{
	/* [한국어] 상대편과 이쪽 인터페이스의 상태. */
	struct epf_ntb_epc *peer_ntb_epc, *ntb_epc;
	/* [한국어] 상대편 창 BAR 의 서술자. 아웃바운드 창의 물리 주소가 여기 있다. */
	struct pci_epf_bar *peer_epf_bar;
	/* [한국어] 상대편 BAR 번호. */
	enum pci_barno peer_barno;
	/* [한국어] BAR 안에서 창이 시작되는 오프셋(mw1_offset)을 읽기 위한 포인터. */
	struct epf_ntb_ctrl *ctrl;
	/* [한국어] 걷을 아웃바운드 창의 물리 주소. */
	phys_addr_t phys_addr;
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] 매핑을 걷을 컨트롤러(이쪽). */
	struct pci_epc *epc;

	/* [한국어] 이쪽 인터페이스. */
	ntb_epc = ntb->epc[type];
	/* [한국어] 아웃바운드 매핑을 들고 있는 컨트롤러. */
	epc = ntb_epc->epc;

	/* [한국어] 상대편 인터페이스 — 창 자리는 상대편 아웃바운드 공간에 있다. */
	peer_ntb_epc = ntb->epc[!type];
	/* [한국어] 창 번호 mw 에 대응하는 상대편 BAR 번호. NTB_MW_OFFSET(2)을 더하는 것은
	 * enum 에서 BAR_CONFIG, BAR_PEER_SPAD 두 칸을 건너뛰어야
	 * BAR_DB_MW1 부터 시작되기 때문이다. */
	peer_barno = peer_ntb_epc->epf_ntb_bar[mw + NTB_MW_OFFSET];
	/* [한국어] 그 BAR 의 서술자. */
	peer_epf_bar = &peer_ntb_epc->epf_bar[peer_barno];

	/* [한국어] 창의 시작 물리 주소. */
	phys_addr = peer_epf_bar->phys_addr;
	/* [한국어] 이 인터페이스의 공유 레지스터. */
	ctrl = ntb_epc->reg;
	/* [한국어] 첫 창은 도어벨과 BAR 를 나눠 쓰므로 도어벨 영역만큼 건너뛴 지점이
	 * 실제 창의 시작이다. 설정 때와 정확히 같은 주소여야 컨트롤러가
	 * 어느 매핑을 걷어야 할지 찾아낸다. */
	if (mw + NTB_MW_OFFSET == BAR_DB_MW1)
		/* [한국어] 도어벨 영역 크기만큼 더한다. */
		phys_addr += ctrl->mw1_offset;
	/* [한국어] 이쪽 함수 번호. */
	func_no = ntb_epc->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb_epc->vfunc_no;

	/* [한국어] 아웃바운드 ATU 를 지운다. 이후 이 호스트가 창에 써도
	 * 상대 호스트 메모리에 닿지 않는다. */
	pci_epc_unmap_addr(epc, func_no, vfunc_no, phys_addr);
}

/**
 * epf_ntb_configure_msi() - Map OB address space to MSI address
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @type: PRIMARY interface or SECONDARY interface
 * @db_count: Number of doorbell interrupts to map
 *
 *+-----------------+    +----->+----------------+-----------+-----------------+
 *|       BAR0      |    |      |   Doorbell 1   +---+------->   MSI ADDRESS   |
 *+-----------------+    |      +----------------+   |       +-----------------+
 *|       BAR1      |    |      |   Doorbell 2   +---+       |                 |
 *+-----------------+----+      +----------------+   |       |                 |
 *|       BAR2      |           |   Doorbell 3   +---+       |                 |
 *+-----------------+----+      +----------------+   |       |                 |
 *|       BAR3      |    |      |   Doorbell 4   +---+       |                 |
 *+-----------------+    |      |----------------+           |                 |
 *|       BAR4      |    |      |                |           |                 |
 *+-----------------+    |      |      MW1       |           |                 |
 *|       BAR5      |    |      |                |           |                 |
 *+-----------------+    +----->-----------------+           |                 |
 *  EP CONTROLLER 1             |                |           |                 |
 *                              |                |           |                 |
 *                              +----------------+           +-----------------+
 *                     (A)       EP CONTROLLER 2             |                 |
 *                                 (OB SPACE)                |                 |
 *                                                           |      MW1        |
 *                                                           |                 |
 *                                                           |                 |
 *                                                   (B)     +-----------------+
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           +-----------------+
 *                                                           PCI Address Space
 *                                                           (Managed by HOST2)
 *
 *
 * This function performs stage (B) in the above diagram (see Doorbell 1,
 * Doorbell 2, Doorbell 3, Doorbell 4) i.e map OB address space corresponding to
 * doorbell to MSI address in PCI address space.
 *
 * This operation requires 3 parameters
 *  1) Address reserved for doorbell in the outbound address space
 *  2) MSI-X address in the PCIe Address space
 *  3) Number of MSI-X interrupts that has to be configured
 *
 * The address in the outbound address space (for the Doorbell) is stored in
 * epf_bar corresponding to BAR_DB_MW1 of epf_ntb_epc that is connected to
 * HOST1. This is populated in epf_ntb_alloc_peer_mem() in this driver along
 * with address for MW1.
 *
 * pci_epc_map_msi_irq() takes the MSI address from MSI capability register
 * and maps the OB address (obtained in epf_ntb_alloc_peer_mem()) to the MSI
 * address.
 *
 * epf_ntb_configure_msi() also stores the MSI data to raise each interrupt
 * in db_data of the peer's control region. This helps the peer to raise
 * doorbell of the other host by writing db_data to the BAR corresponding to
 * BAR_DB_MW1.
 */
/* [한국어]
 * epf_ntb_configure_msi - 도어벨 창을 상대 호스트의 MSI 주소에 매핑한다
 *
 * @ntb: 함수 전체 상태
 * @type: 도어벨을 "받게 될" 쪽 인터페이스
 * @db_count: 매핑할 도어벨 개수
 * @return: 0 성공, 그 밖에는 pci_epc_map_msi_irq() 의 실패 코드
 *
 * 왜 필요한가: 도어벨은 결국 "상대 호스트가 어떤 주소에 어떤 값을 쓰면
 * 이 호스트에 인터럽트가 뜬다" 는 장치다. MSI 의 목적지 주소는 config
 * 공간의 능력 레지스터에만 있고 메모리에 표로 노출되지 않으므로,
 * 컨트롤러 도우미에게 대신 읽어 매핑까지 해 달라고 부탁해야 한다.
 *
 * 동작 단계:
 *   (1) 상대편 인터페이스의 BAR_DB_MW1 BAR 뒤에 있는 아웃바운드 창 주소를 얻는다.
 *   (2) pci_epc_map_msi_irq() 로 그 창을 이 호스트의 MSI 주소에 건다.
 *       도우미가 쓸 데이터의 기준값과 주소 안 오프셋을 돌려준다.
 *   (3) 그 두 값을 상대편 공유 레지스터의 db_data/db_offset 에 적어 준다.
 *       MSI 는 데이터 하위 비트가 벡터 번호이므로 기준값에 i 를 OR 한다.
 *
 * 실행 컨텍스트: cmd_handler 워크(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 에러 경로: 매핑 실패는 도어벨 전체가 죽는다는 뜻이라 즉시 반환한다.
 *
 * 호출 체인:
 *   epf_ntb_cmd_handler() → epf_ntb_configure_db()
 *     → [epf_ntb_configure_msi] → pci_epc_map_msi_irq()
 */
static int epf_ntb_configure_msi(struct epf_ntb *ntb,
				 enum pci_epc_interface_type type, u16 db_count)
{
	/* [한국어] 상대편과 이쪽 인터페이스의 상태. */
	struct epf_ntb_epc *peer_ntb_epc, *ntb_epc;
	/* [한국어] 도어벨 한 칸 크기, 컨트롤러가 돌려주는 MSI 데이터와 주소 오프셋. */
	u32 db_entry_size, db_data, db_offset;
	/* [한국어] 상대편 도어벨 BAR 의 서술자. */
	struct pci_epf_bar *peer_epf_bar;
	/* [한국어] 상대편 공유 레지스터 — 도어벨 정보를 여기에 적어 준다. */
	struct epf_ntb_ctrl *peer_ctrl;
	/* [한국어] 상대편 BAR 번호. */
	enum pci_barno peer_barno;
	/* [한국어] 매핑할 아웃바운드 창의 물리 주소. */
	phys_addr_t phys_addr;
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] 매핑을 걸 컨트롤러(이쪽). */
	struct pci_epc *epc;
	/* [한국어] 실패 코드와 도어벨 순회 인덱스. */
	int ret, i;

	/* [한국어] 이쪽 인터페이스 — MSI 능력 레지스터는 이쪽 호스트가 설정한 것이다. */
	ntb_epc = ntb->epc[type];
	/* [한국어] 아웃바운드 매핑을 걸 컨트롤러. */
	epc = ntb_epc->epc;

	/* [한국어] 상대편 인터페이스. */
	peer_ntb_epc = ntb->epc[!type];
	/* [한국어] 상대편의 도어벨+MW1 BAR 번호. */
	peer_barno = peer_ntb_epc->epf_ntb_bar[BAR_DB_MW1];
	/* [한국어] 그 BAR 의 서술자. */
	peer_epf_bar = &peer_ntb_epc->epf_bar[peer_barno];
	/* [한국어] 상대편 공유 레지스터. */
	peer_ctrl = peer_ntb_epc->reg;
	/* [한국어] 도어벨 한 칸 크기. 상대편 영역 할당 때 정해진 값이다. */
	db_entry_size = peer_ctrl->db_entry_size;

	/* [한국어] 도어벨 창의 시작 물리 주소. */
	phys_addr = peer_epf_bar->phys_addr;
	/* [한국어] 이쪽 함수 번호. */
	func_no = ntb_epc->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb_epc->vfunc_no;

	/* [한국어] MSI 는 주소가 하나뿐이라 컨트롤러 도우미 한 번으로 끝난다.
	 * 컨트롤러가 MSI 능력 레지스터에서 주소를 읽어 아웃바운드 창을 걸고,
	 * 쓸 데이터의 기준값과 주소 안 오프셋을 돌려준다.
	 * MSI-X 처럼 표를 직접 읽지 않는 이유는 MSI 주소가 config 공간의
	 * 능력 레지스터에만 있고 메모리에 표로 노출되지 않기 때문이다. */
	ret = pci_epc_map_msi_irq(epc, func_no, vfunc_no, phys_addr, db_count,
				  db_entry_size, &db_data, &db_offset);
	/* [한국어] 매핑 실패는 도어벨 전체가 죽는다는 뜻이다. */
	if (ret) {
		/* [한국어] 어느 인터페이스에서 났는지 남긴다. */
		dev_err(&epc->dev, "%s intf: Failed to map MSI IRQ\n",
			pci_epc_interface_string(type));
		return ret;
	}

	/* [한국어] 도어벨 개수만큼 정보를 적어 준다. */
	for (i = 0; i < db_count; i++) {
		/* [한국어] MSI 는 데이터의 하위 비트로 벡터를 구분한다.
		 * 기준값에 i 를 OR 해서 벡터 i 의 데이터를 만든다.
		 * (MSI 스펙상 multi-message 는 데이터 하위 비트가 벡터 번호가 된다.) */
		peer_ctrl->db_data[i] = db_data | i;
		/* [한국어] 주소 안 오프셋은 모든 벡터가 같다 — 주소가 하나이기 때문이다. */
		peer_ctrl->db_offset[i] = db_offset;
	}

	return 0;
}

/**
 * epf_ntb_configure_msix() - Map OB address space to MSI-X address
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @type: PRIMARY interface or SECONDARY interface
 * @db_count: Number of doorbell interrupts to map
 *
 *+-----------------+    +----->+----------------+-----------+-----------------+
 *|       BAR0      |    |      |   Doorbell 1   +-----------> MSI-X ADDRESS 1 |
 *+-----------------+    |      +----------------+           +-----------------+
 *|       BAR1      |    |      |   Doorbell 2   +---------+ |                 |
 *+-----------------+----+      +----------------+         | |                 |
 *|       BAR2      |           |   Doorbell 3   +-------+ | +-----------------+
 *+-----------------+----+      +----------------+       | +-> MSI-X ADDRESS 2 |
 *|       BAR3      |    |      |   Doorbell 4   +-----+ |   +-----------------+
 *+-----------------+    |      |----------------+     | |   |                 |
 *|       BAR4      |    |      |                |     | |   +-----------------+
 *+-----------------+    |      |      MW1       +     | +-->+ MSI-X ADDRESS 3||
 *|       BAR5      |    |      |                |     |     +-----------------+
 *+-----------------+    +----->-----------------+     |     |                 |
 *  EP CONTROLLER 1             |                |     |     +-----------------+
 *                              |                |     +---->+ MSI-X ADDRESS 4 |
 *                              +----------------+           +-----------------+
 *                     (A)       EP CONTROLLER 2             |                 |
 *                                 (OB SPACE)                |                 |
 *                                                           |      MW1        |
 *                                                           |                 |
 *                                                           |                 |
 *                                                   (B)     +-----------------+
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           +-----------------+
 *                                                           PCI Address Space
 *                                                           (Managed by HOST2)
 *
 * This function performs stage (B) in the above diagram (see Doorbell 1,
 * Doorbell 2, Doorbell 3, Doorbell 4) i.e map OB address space corresponding to
 * doorbell to MSI-X address in PCI address space.
 *
 * This operation requires 3 parameters
 *  1) Address reserved for doorbell in the outbound address space
 *  2) MSI-X address in the PCIe Address space
 *  3) Number of MSI-X interrupts that has to be configured
 *
 * The address in the outbound address space (for the Doorbell) is stored in
 * epf_bar corresponding to BAR_DB_MW1 of epf_ntb_epc that is connected to
 * HOST1. This is populated in epf_ntb_alloc_peer_mem() in this driver along
 * with address for MW1.
 *
 * The MSI-X address is in the MSI-X table of EP CONTROLLER 2 and
 * the count of doorbell is in ctrl->argument of epf_ntb_epc that is connected
 * to HOST2. MSI-X table is stored memory mapped to ntb_epc->msix_bar and the
 * offset is in ntb_epc->msix_table_offset. From this epf_ntb_configure_msix()
 * gets the MSI-X address and data.
 *
 * epf_ntb_configure_msix() also stores the MSI-X data to raise each interrupt
 * in db_data of the peer's control region. This helps the peer to raise
 * doorbell of the other host by writing db_data to the BAR corresponding to
 * BAR_DB_MW1.
 */
/* [한국어]
 * epf_ntb_configure_msix - 도어벨 창을 상대 호스트의 MSI-X 주소들에 하나씩 매핑한다
 *
 * @ntb: 함수 전체 상태
 * @type: 도어벨을 "받게 될" 쪽 인터페이스
 * @db_count: 매핑할 도어벨 개수
 * @return: 0 성공, 그 밖에는 pci_epc_map_addr() 의 실패 코드
 *
 * 왜 필요한가: MSI 와 달리 MSI-X 는 벡터마다 목적지 주소가 다르다.
 * 그래서 도우미 한 번으로 끝나지 않고 벡터 수만큼 창을 나눠 매핑해야 한다.
 * 대신 MSI-X 표는 BAR 안의 메모리에 있어 이 드라이버가 직접 읽을 수 있다.
 *
 * 동작 단계:
 *   (1) 이쪽 config BAR 안 msix_table_offset 지점에서 MSI-X 표를 찾는다.
 *       이 표는 호스트가 MSI-X 를 설정하면서 직접 채운 값이다.
 *   (2) 벡터마다 주소를 컨트롤러 정렬 경계로 내림해 창 한 칸을 건다.
 *   (3) 잘려 나간 하위 비트를 db_offset 에, 표의 데이터를 db_data 에
 *       상대편 공유 레지스터로 적어 준다.
 *   (4) is_msix 를 세워 link_up 이 쏠 인터럽트 종류를 바꾼다.
 *
 * 실행 컨텍스트: cmd_handler 워크(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 에러 경로: 중간에 실패하면 이미 건 매핑을 되돌리지 않고 반환한다.
 * 호출자는 COMMAND_STATUS_ERROR 로 알리고, 정리는 호스트가 보내는
 * COMMAND_TEARDOWN_DOORBELL 이나 unbind 경로가 맡는다.
 *
 * 호출 체인:
 *   epf_ntb_cmd_handler() → epf_ntb_configure_db()
 *     → [epf_ntb_configure_msix] → pci_epc_map_addr()
 */
static int epf_ntb_configure_msix(struct epf_ntb *ntb,
				  enum pci_epc_interface_type type,
				  u16 db_count)
{
	/* [한국어] 이쪽 컨트롤러의 능력표. 주소 정렬 요구를 여기서 읽는다. */
	const struct pci_epc_features *epc_features;
	/* [한국어] 상대편과 이쪽 인터페이스의 상태. */
	struct epf_ntb_epc *peer_ntb_epc, *ntb_epc;
	/* [한국어] 상대편 도어벨 BAR 서술자와 이쪽 MSI-X 표가 놓인 BAR 서술자. */
	struct pci_epf_bar *peer_epf_bar, *epf_bar;
	/* [한국어] MSI-X 표 한 항목의 모양(주소 하위/상위, 데이터, 벡터 제어). */
	struct pci_epf_msix_tbl *msix_tbl;
	/* [한국어] 상대편의 공유 레지스터 — 도어벨 데이터/오프셋을 여기에 적어 준다. */
	struct epf_ntb_ctrl *peer_ctrl;
	/* [한국어] 도어벨 한 칸의 크기와 이번 벡터의 MSI-X 데이터. */
	u32 db_entry_size, msg_data;
	/* [한국어] 상대편 BAR 번호. */
	enum pci_barno peer_barno;
	/* [한국어] 매핑할 아웃바운드 창의 물리 주소. */
	phys_addr_t phys_addr;
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] 매핑을 걸 컨트롤러(이쪽). */
	struct pci_epc *epc;
	/* [한국어] 컨트롤러가 요구하는 주소 정렬. */
	size_t align;
	/* [한국어] 이번 벡터의 MSI-X 주소. */
	u64 msg_addr;
	/* [한국어] 실패 코드와 벡터 순회 인덱스. */
	int ret, i;

	/* [한국어] 이쪽 인터페이스 — MSI-X 표는 이쪽 호스트가 채워 준 것이다. */
	ntb_epc = ntb->epc[type];
	/* [한국어] 아웃바운드 매핑을 걸 컨트롤러. */
	epc = ntb_epc->epc;

	/* [한국어] MSI-X 표가 놓인 BAR 의 서술자. epf_ntb_config_spad_bar_alloc() 이
	 * msix_bar 를 config BAR 로 정해 두었다. */
	epf_bar = &ntb_epc->epf_bar[ntb_epc->msix_bar];
	/* [한국어] BAR 의 커널 가상 주소에서 표 오프셋만큼 나아간 곳이 표의 시작이다.
	 * 이 표는 호스트가 MSI-X 를 설정하면서 직접 써 넣은 값이라,
	 * 여기서 읽는 주소와 데이터가 곧 "이 호스트에게 인터럽트를 쏘는 법" 이다. */
	msix_tbl = epf_bar->addr + ntb_epc->msix_table_offset;

	/* [한국어] 상대편 인터페이스 — 도어벨 창은 상대편 아웃바운드 공간에 있다. */
	peer_ntb_epc = ntb->epc[!type];
	/* [한국어] 상대편의 도어벨+MW1 BAR 번호. */
	peer_barno = peer_ntb_epc->epf_ntb_bar[BAR_DB_MW1];
	/* [한국어] 그 BAR 의 서술자. */
	peer_epf_bar = &peer_ntb_epc->epf_bar[peer_barno];
	/* [한국어] 도어벨 창의 시작 물리 주소. 이 지점부터 벡터마다 한 칸씩 나아간다. */
	phys_addr = peer_epf_bar->phys_addr;
	/* [한국어] 상대편 공유 레지스터. 상대 호스트가 읽을 db_data/db_offset 을 여기 적는다. */
	peer_ctrl = peer_ntb_epc->reg;
	/* [한국어] 이쪽 컨트롤러의 능력표. */
	epc_features = ntb_epc->epc_features;
	/* [한국어] 주소 정렬 요구. 아웃바운드 매핑은 이 단위로만 걸 수 있다. */
	align = epc_features->align;

	/* [한국어] 이쪽 함수 번호. */
	func_no = ntb_epc->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb_epc->vfunc_no;
	/* [한국어] 도어벨 한 칸의 크기. 상대편이 알려 준 값을 그대로 쓴다. */
	db_entry_size = peer_ctrl->db_entry_size;

	/* [한국어] 호스트가 요청한 도어벨 개수만큼 벡터를 하나씩 매핑한다. */
	for (i = 0; i < db_count; i++) {
		/* [한국어] MSI-X 벡터의 목적지 주소를 정렬 경계로 내림한다.
		 * 컨트롤러가 정렬된 주소로만 아웃바운드 창을 걸 수 있기 때문이다.
		 * 잘려 나간 하위 비트는 아래 db_offset 으로 따로 알려 준다. */
		msg_addr = ALIGN_DOWN(msix_tbl[i].msg_addr, align);
		/* [한국어] 이 벡터에 실어야 할 데이터 값. 호스트가 정한 값이다. */
		msg_data = msix_tbl[i].msg_data;
		/* [한국어] 아웃바운드 창 한 칸을 이 MSI-X 주소에 건다. 이후 상대 호스트가
		 * 자기 도어벨 BAR 의 이 칸에 db_data 를 쓰면, 그 쓰기가 이쪽
		 * 호스트의 MSI-X 주소로 나가 인터럽트가 된다. */
		ret = pci_epc_map_addr(epc, func_no, vfunc_no, phys_addr, msg_addr,
				       db_entry_size);
		/* [한국어] 매핑 실패는 도어벨 하나가 죽는다는 뜻이라 전체를 실패로 본다. */
		if (ret) {
			dev_err(&epc->dev,
				"%s intf: Failed to configure MSI-X IRQ\n",
				pci_epc_interface_string(type));
			return ret;
		}
		/* [한국어] 다음 벡터를 위해 한 칸 나아간다. 창이 벡터마다 하나씩 필요하다. */
		phys_addr = phys_addr + db_entry_size;
		/* [한국어] 상대 호스트에게 "이 값을 쓰면 벡터 i 가 울린다" 고 알린다. */
		peer_ctrl->db_data[i] = msg_data;
		/* [한국어] 정렬 때 잘려 나간 하위 비트를 오프셋으로 알린다.
		 * 상대 호스트는 창의 시작에서 이만큼 떨어진 곳에 써야 한다.
		 * align - 1 마스크가 성립하는 것은 align 이 2의 거듭제곱이기 때문이다. */
		peer_ctrl->db_offset[i] = msix_tbl[i].msg_addr & (align - 1);
	}
	/* [한국어] 이 인터페이스가 MSI-X 로 동작 중임을 기록한다.
	 * epf_ntb_link_up() 이 이 값을 보고 어떤 인터럽트 종류를 쏠지 정한다. */
	ntb_epc->is_msix = true;

	return 0;
}

/**
 * epf_ntb_configure_db() - Configure the Outbound Address Space for one host
 *   to ring the doorbell of other host
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @type: PRIMARY interface or SECONDARY interface
 * @db_count: Count of the number of doorbells that has to be configured
 * @msix: Indicates whether MSI-X or MSI should be used
 *
 * Invokes epf_ntb_configure_msix() or epf_ntb_configure_msi() required for
 * one HOST to ring the doorbell of other HOST.
 */
/* [한국어]
 * epf_ntb_configure_db - 도어벨 설정을 MSI 경로와 MSI-X 경로로 갈라 보낸다
 *
 * @ntb: 함수 전체 상태
 * @type: 도어벨을 받게 될 쪽 인터페이스
 * @db_count: 호스트가 요청한 도어벨 개수
 * @msix: 호스트가 MSI-X 를 쓰겠다고 했는가
 * @return: 0 성공, -EINVAL 이면 개수가 MAX_DB_COUNT 를 넘음, 그 밖에는 하위 실패 코드
 *
 * 왜 필요한가: 두 경로가 하는 일은 같지만 방법이 완전히 다르다.
 * 이 함수는 그 분기와 공통 검증(개수 상한)을 한 자리에 모은다.
 * 호스트가 보낸 개수를 그대로 믿으면 db_data[]/db_offset[] 배열
 * 밖을 건드리게 되므로, 이 검사가 보안상 중요하다.
 *
 * 실행 컨텍스트: cmd_handler 워크(프로세스 컨텍스트).
 *
 * 에러 경로: 상한 초과는 곧바로 -EINVAL. 하위 실패는 로그만 남기고 그대로 올린다.
 *
 * 호출 체인:
 *   epf_ntb_cmd_handler() → [epf_ntb_configure_db]
 *     → epf_ntb_configure_msix() 또는 epf_ntb_configure_msi()
 */
static int epf_ntb_configure_db(struct epf_ntb *ntb,
				enum pci_epc_interface_type type,
				u16 db_count, bool msix)
{
	/* [한국어] 설정 대상 인터페이스의 상태. */
	struct epf_ntb_epc *ntb_epc;
	/* [한국어] 로그용 컨트롤러. */
	struct pci_epc *epc;
	/* [한국어] 하위 호출의 실패를 담는다. */
	int ret;

	/* [한국어] db_data[]/db_offset[] 배열이 MAX_DB_COUNT(32) 칸이므로
	 * 그보다 많은 도어벨은 배열 밖을 건드리게 된다. 호스트가 보낸 값을
	 * 그대로 믿지 않고 여기서 반드시 검사한다. */
	if (db_count > MAX_DB_COUNT)
		/* [한국어] 범위를 넘으면 명령을 거절한다. */
		return -EINVAL;

	/* [한국어] 설정 대상 인터페이스. */
	ntb_epc = ntb->epc[type];
	/* [한국어] 로그 대상 컨트롤러. */
	epc = ntb_epc->epc;

	/* [한국어] 호스트가 MSI-X 를 쓰겠다고 했는가. */
	if (msix)
		/* [한국어] MSI-X 경로: 벡터마다 주소가 다르므로 도어벨마다 따로 매핑한다. */
		ret = epf_ntb_configure_msix(ntb, type, db_count);
	else
		/* [한국어] MSI 경로: 주소가 하나라 컨트롤러 도우미 한 번으로 끝난다. */
		ret = epf_ntb_configure_msi(ntb, type, db_count);

	/* [한국어] 어느 쪽이든 실패는 같은 방식으로 보고한다. */
	if (ret)
		/* [한국어] 어느 인터페이스에서 났는지 남긴다. */
		dev_err(&epc->dev, "%s intf: Failed to configure DB\n",
			pci_epc_interface_string(type));

	return ret;
}

/**
 * epf_ntb_teardown_db() - Unmap address in OB address space to MSI/MSI-X
 *   address
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @type: PRIMARY interface or SECONDARY interface
 *
 * Invoke pci_epc_unmap_addr() to unmap OB address to MSI/MSI-X address.
 */
/* [한국어]
 * epf_ntb_teardown_db - 도어벨 창에 걸어 둔 아웃바운드 매핑을 걷는다
 *
 * @ntb: 함수 전체 상태
 * @type: 명령을 보낸 쪽 인터페이스
 * @return: 없음. 실패를 알릴 방법이 없어 호출자는 항상 성공으로 보고한다.
 *
 * 왜 필요한가: epf_ntb_configure_msi()/msix() 가 건 매핑을 되돌린다.
 * 걷지 않으면 컨트롤러의 아웃바운드 ATU 항목이 새어 나간다.
 *
 * 동작 단계: 상대편 인터페이스의 BAR_DB_MW1 BAR 뒤에 있는 창의 시작
 * 물리 주소를 다시 계산해 pci_epc_unmap_addr() 에 넘긴다. 컨트롤러는
 * 이 주소로 어느 항목을 걷을지 찾으므로 설정 때와 같은 값이어야 한다.
 *
 * 한계: MSI-X 로 설정했다면 벡터마다 창을 한 칸씩 따로 걸었는데,
 * 여기서는 첫 칸 하나만 걷는다. 나머지 칸의 매핑은 남는다.
 * 상류 코드 그대로 둔다.
 *
 * 실행 컨텍스트: cmd_handler 워크(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 호출 체인:
 *   epf_ntb_cmd_handler() → [epf_ntb_teardown_db] → pci_epc_unmap_addr()
 */
static void
epf_ntb_teardown_db(struct epf_ntb *ntb, enum pci_epc_interface_type type)
{
	/* [한국어] 상대편과 이쪽 인터페이스의 상태. */
	struct epf_ntb_epc *peer_ntb_epc, *ntb_epc;
	/* [한국어] 상대편 도어벨 BAR 의 서술자. 아웃바운드 창의 물리 주소가 여기 있다. */
	struct pci_epf_bar *peer_epf_bar;
	/* [한국어] 상대편 BAR 번호. */
	enum pci_barno peer_barno;
	/* [한국어] 걷을 아웃바운드 창의 물리 주소. */
	phys_addr_t phys_addr;
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] 매핑을 걷을 컨트롤러(이쪽). */
	struct pci_epc *epc;

	/* [한국어] 이쪽 인터페이스. */
	ntb_epc = ntb->epc[type];
	/* [한국어] 이쪽 컨트롤러 — 아웃바운드 매핑은 이쪽 컨트롤러가 들고 있다. */
	epc = ntb_epc->epc;

	/* [한국어] 상대편 인터페이스. */
	peer_ntb_epc = ntb->epc[!type];
	/* [한국어] 상대편의 도어벨+MW1 BAR 번호. */
	peer_barno = peer_ntb_epc->epf_ntb_bar[BAR_DB_MW1];
	/* [한국어] 그 BAR 의 서술자. */
	peer_epf_bar = &peer_ntb_epc->epf_bar[peer_barno];
	/* [한국어] 매핑을 건 물리 주소. 설정 때와 같은 주소여야 컨트롤러가 찾아낸다. */
	phys_addr = peer_epf_bar->phys_addr;
	/* [한국어] 이쪽 함수 번호. */
	func_no = ntb_epc->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb_epc->vfunc_no;

	/* [한국어] 아웃바운드 ATU 를 지운다. 이후 이 창에 쓴 값은 상대 호스트에
	 * 도달하지 않는다. 아웃바운드 주소 공간 자체는 여전히 잡혀 있고,
	 * epf_ntb_free_peer_mem() 이 따로 돌려준다. */
	pci_epc_unmap_addr(epc, func_no, vfunc_no, phys_addr);
}

/**
 * epf_ntb_cmd_handler() - Handle commands provided by the NTB Host
 * @work: work_struct for the two epf_ntb_epc (PRIMARY and SECONDARY)
 *
 * Workqueue function that gets invoked for the two epf_ntb_epc
 * periodically (once every 5ms) to see if it has received any commands
 * from NTB host. The host can send commands to configure doorbell or
 * configure memory window or to update link status.
 */
/* [한국어]
 * epf_ntb_cmd_handler - 호스트가 써 넣은 명령을 5ms 마다 폴링해 처리하는 워크
 *
 * @work: 인터페이스 상태 안에 박혀 있는 delayed_work
 * @return: 없음
 *
 * 왜 필요한가: 호스트에서 이 EP 로 오는 인터럽트 경로가 없다.
 * 호스트가 할 수 있는 일은 자기 BAR 를 통해 공유 레지스터에 값을 쓰는 것뿐이라,
 * 이쪽에서 주기적으로 읽어 보는 수밖에 없다. 그 폴링 루프가 이 함수다.
 *
 * 동작 단계:
 *   (1) container_of 로 자기 인터페이스 상태를 되찾는다.
 *   (2) command 를 읽고, 0 이면 아무 것도 하지 않고 재예약만 한다.
 *   (3) argument 를 읽은 뒤 command/argument 를 0 으로 지워 "받았다" 고 알린다.
 *   (4) 명령별로 도어벨/메모리 윈도우/링크 처리를 하고 command_status 에 결과를 적는다.
 *   (5) 5ms 뒤의 자신을 다시 큐에 넣어 루프를 이어 간다.
 *
 * 실행 컨텍스트: kpcintb 작업 큐의 워커 스레드(프로세스 컨텍스트).
 * 인터페이스마다 하나씩 있으므로 PRIMARY 용과 SECONDARY 용 두 인스턴스가
 * 동시에 돌 수 있다. 두 인스턴스는 서로의 공유 레지스터를 읽고 쓰지만
 * 별도의 락이 없다 — 링크 판단(양쪽 linkup 확인)이 그 대표적인 경합 지점이다.
 *
 * 에러 경로: 하위 함수의 실패는 COMMAND_STATUS_ERROR 로 호스트에게만 알리고
 * 워크 자체는 계속 돈다. 워크를 멈추는 것은 cancel_delayed_work() 뿐이다.
 *
 * 호출 체인:
 *   워크큐 워커 → [epf_ntb_cmd_handler]
 *     → epf_ntb_configure_db() / epf_ntb_teardown_db()
 *     → epf_ntb_configure_mw() / epf_ntb_teardown_mw()
 *     → epf_ntb_link_up()
 */
static void epf_ntb_cmd_handler(struct work_struct *work)
{
	/* [한국어] 이 워크가 어느 인터페이스의 것인지(PRIMARY/SECONDARY). */
	enum pci_epc_interface_type type;
	/* [한국어] container_of 로 되찾을 인터페이스 상태. */
	struct epf_ntb_epc *ntb_epc;
	/* [한국어] 호스트와 공유하는 레지스터 영역. */
	struct epf_ntb_ctrl *ctrl;
	/* [한국어] 호스트가 써 넣은 명령과 인자. */
	u32 command, argument;
	/* [한국어] 함수 전체 상태. */
	struct epf_ntb *ntb;
	/* [한국어] 로그용. */
	struct device *dev;
	/* [한국어] COMMAND_CONFIGURE_DOORBELL 인자에서 뽑아낼 도어벨 개수. */
	u16 db_count;
	/* [한국어] 같은 인자에서 뽑아낼 MSI-X 사용 여부. */
	bool is_msix;
	/* [한국어] 하위 호출의 실패를 담는다. */
	int ret;

	/* [한국어] delayed_work 는 각 인터페이스 상태 안에 박혀 있으므로
	 * container_of 로 그 구조체 전체를 되찾는다.
	 * 이 워크는 커널 워커 스레드(프로세스 컨텍스트)에서 돌며,
	 * 인터페이스마다 하나씩 있으므로 두 인스턴스가 동시에 돌 수 있다. */
	ntb_epc = container_of(work, struct epf_ntb_epc, cmd_handler.work);
	/* [한국어] 호스트가 BAR 를 통해 직접 쓰는 공유 레지스터 영역. */
	ctrl = ntb_epc->reg;
	/* [한국어] 명령 필드를 읽는다. 호스트가 쓴 값이 DMA 일관성 메모리를 통해 보인다. */
	command = ctrl->command;
	/* [한국어] 0 이면 새 명령이 없다는 뜻이므로 재예약만 하고 물러난다. */
	if (!command)
		goto reset_handler;
	/* [한국어] 명령의 인자를 읽는다. 반드시 명령을 지우기 전에 읽어야 한다. */
	argument = ctrl->argument;

	/* [한국어] 명령을 지워 "받았다" 고 알린다. 호스트는 이 필드가 0 이 되는 것을
	 * 보고 다음 명령을 넣을 수 있다. */
	ctrl->command = 0;
	/* [한국어] 인자도 함께 지운다. */
	ctrl->argument = 0;

	/* [한국어] 같은 포인터를 다시 대입한다. 상류 코드 그대로 둔다. */
	ctrl = ntb_epc->reg;
	/* [한국어] 이 인터페이스가 PRIMARY 인지 SECONDARY 인지. 하위 함수들이
	 * 이 값으로 "나" 와 "상대" 를 구분한다. */
	type = ntb_epc->type;
	/* [한국어] 함수 전체 상태로 올라간다. */
	ntb = ntb_epc->epf_ntb;
	/* [한국어] 로그 대상. */
	dev = &ntb->epf->dev;

	/* [한국어] 명령 코드로 분기한다. 코드 값은 이 파일 위쪽 COMMAND_ 매크로에
	 * 정의되어 있고, 호스트 쪽 NTB 드라이버와 값을 맞춰야 하는 약속이다. */
	switch (command) {
	/* [한국어] 도어벨 설정 요청. */
	case COMMAND_CONFIGURE_DOORBELL:
		/* [한국어] 인자의 하위 16비트가 도어벨 개수다(DB_COUNT_MASK). */
		db_count = argument & DB_COUNT_MASK;
		/* [한국어] 인자의 16번째 비트가 서 있으면 MSI-X 를 쓰라는 뜻이다(MSIX_ENABLE).
		 * 하나의 32비트 인자에 개수와 플래그를 같이 실어 보내는 구조다. */
		is_msix = argument & MSIX_ENABLE;
		/* [한국어] 실제 매핑 작업. 상대편 아웃바운드 창을 이 호스트의 MSI/MSI-X 주소에 건다. */
		ret = epf_ntb_configure_db(ntb, type, db_count, is_msix);
		/* [한국어] 실패하면 호스트에게 오류로 알린다. */
		if (ret < 0)
			/* [한국어] 호스트는 command_status 를 폴링해 결과를 안다. */
			ctrl->command_status = COMMAND_STATUS_ERROR;
		else
			/* [한국어] 성공을 알린다. */
			ctrl->command_status = COMMAND_STATUS_OK;
		break;
	/* [한국어] 도어벨 해제 요청. */
	case COMMAND_TEARDOWN_DOORBELL:
		/* [한국어] 아웃바운드 매핑을 걷는다. 실패할 수 없는 경로다. */
		epf_ntb_teardown_db(ntb, type);
		/* [한국어] 항상 성공으로 보고한다. */
		ctrl->command_status = COMMAND_STATUS_OK;
		break;
	/* [한국어] 메모리 윈도우 설정 요청. 인자가 창 번호다. */
	case COMMAND_CONFIGURE_MW:
		/* [한국어] 호스트가 ctrl->addr/ctrl->size 에 미리 써 둔 PCI 주소로 창을 건다. */
		ret = epf_ntb_configure_mw(ntb, type, argument);
		/* [한국어] 실패를 호스트에게 알린다. */
		if (ret < 0)
			/* [한국어] 성공/실패를 구분해 보고한다. */
			ctrl->command_status = COMMAND_STATUS_ERROR;
		else
			/* [한국어] 성공을 알린다. */
			ctrl->command_status = COMMAND_STATUS_OK;
		break;
	/* [한국어] 메모리 윈도우 해제 요청. */
	case COMMAND_TEARDOWN_MW:
		/* [한국어] 아웃바운드 매핑을 걷는다. */
		epf_ntb_teardown_mw(ntb, type, argument);
		/* [한국어] 항상 성공으로 보고한다. */
		ctrl->command_status = COMMAND_STATUS_OK;
		break;
	/* [한국어] 링크 올림 요청. 이 호스트 쪽 NTB 드라이버가 ntb_link_enable() 을
	 * 부른 결과로 도착한다. */
	case COMMAND_LINK_UP:
		/* [한국어] 이 인터페이스만 먼저 올라간 것으로 표시한다. */
		ntb_epc->linkup = true;
		/* [한국어] 양쪽 호스트가 모두 링크를 올렸는지 확인한다.
		 * NTB 는 두 호스트가 다 준비되어야 의미가 있으므로,
		 * 한쪽만 올라온 상태에서는 인터럽트를 쏘지 않는다. */
		if (ntb->epc[PRIMARY_INTERFACE]->linkup &&
		    ntb->epc[SECONDARY_INTERFACE]->linkup) {
			/* [한국어] 두 호스트 모두에게 링크 업 인터럽트를 쏜다. */
			ret = epf_ntb_link_up(ntb, true);
			/* [한국어] 실패를 알린다. */
			if (ret < 0)
				/* [한국어] 결과를 구분해 보고한다. */
				ctrl->command_status = COMMAND_STATUS_ERROR;
			else
				/* [한국어] 성공을 알린다. */
				ctrl->command_status = COMMAND_STATUS_OK;
			/* [한국어] 여기서는 break 가 아니라 goto 다. 아래 default 를 지나지 않고
			 * 곧장 재예약으로 간다 — 동작상 break 와 같지만 상류 코드 그대로 둔다. */
			goto reset_handler;
		}
		/* [한국어] 아직 상대가 올라오지 않았어도 이 명령 자체는 받아들여졌다. */
		ctrl->command_status = COMMAND_STATUS_OK;
		break;
	/* [한국어] 링크 내림 요청. */
	case COMMAND_LINK_DOWN:
		/* [한국어] 이 인터페이스의 링크 상태를 내린다. */
		ntb_epc->linkup = false;
		/* [한국어] 양쪽 모두에게 링크 다운 인터럽트를 쏜다 — 한쪽이 내려가면
		 * NTB 전체가 쓸 수 없기 때문이다. */
		ret = epf_ntb_link_up(ntb, false);
		/* [한국어] 실패를 알린다. */
		if (ret < 0)
			/* [한국어] 결과를 구분해 보고한다. */
			ctrl->command_status = COMMAND_STATUS_ERROR;
		else
			/* [한국어] 성공을 알린다. */
			ctrl->command_status = COMMAND_STATUS_OK;
		break;
	/* [한국어] 약속에 없는 명령 코드. 호스트 쪽 드라이버와 버전이 어긋난 경우다. */
	default:
		/* [한국어] 어느 인터페이스에서 어떤 코드가 왔는지 남긴다. */
		dev_err(dev, "%s intf UNKNOWN command: %d\n",
			pci_epc_interface_string(type), command);
		break;
	}

/* [한국어] 명령이 없었을 때와 처리를 마친 뒤 모두 이곳으로 온다. */
reset_handler:
	/* [한국어] 5ms 뒤에 스스로를 다시 큐에 넣는다. 이 재예약이 폴링 루프를
	 * 이어 가며, 워크가 취소되면(cancel_delayed_work) 루프가 끊긴다.
	 * 호스트에서 오는 인터럽트가 없으므로 폴링 외에 방법이 없다. */
	queue_delayed_work(kpcintb_workqueue, &ntb_epc->cmd_handler,
			   msecs_to_jiffies(5));
}

/**
 * epf_ntb_peer_spad_bar_clear() - Clear Peer Scratchpad BAR
 * @ntb_epc: EPC associated with one of the HOST which holds peer's outbound
 *	     address.
 *
 *+-----------------+------->+------------------+        +-----------------+
 *|       BAR0      |        |  CONFIG REGION   |        |       BAR0      |
 *+-----------------+----+   +------------------+<-------+-----------------+
 *|       BAR1      |    |   |SCRATCHPAD REGION |        |       BAR1      |
 *+-----------------+    +-->+------------------+<-------+-----------------+
 *|       BAR2      |            Local Memory            |       BAR2      |
 *+-----------------+                                    +-----------------+
 *|       BAR3      |                                    |       BAR3      |
 *+-----------------+                                    +-----------------+
 *|       BAR4      |                                    |       BAR4      |
 *+-----------------+                                    +-----------------+
 *|       BAR5      |                                    |       BAR5      |
 *+-----------------+                                    +-----------------+
 *  EP CONTROLLER 1                                        EP CONTROLLER 2
 *
 * Clear BAR1 of EP CONTROLLER 2 which contains the HOST2's peer scratchpad
 * region. While BAR1 is the default peer scratchpad BAR, an NTB could have
 * other BARs for peer scratchpad (because of 64-bit BARs or reserved BARs).
 * This function can get the exact BAR used for peer scratchpad from
 * epf_ntb_bar[BAR_PEER_SPAD].
 *
 * Since HOST2's peer scratchpad is also HOST1's self scratchpad, this function
 * gets the address of peer scratchpad from
 * peer_ntb_epc->epf_ntb_bar[BAR_CONFIG].
 */
/* [한국어]
 * epf_ntb_peer_spad_bar_clear - peer 스크래치패드 BAR 의 인바운드 매핑을 지운다
 *
 * @ntb_epc: 정리할 인터페이스의 상태
 * @return: 없음
 *
 * 왜 필요한가: 이 BAR 뒤의 물리 메모리는 상대편 인터페이스가 잡은 것이다.
 * 그래서 여기서는 "내 BAR 를 통해 그 메모리로 들어오는 길" 만 끊고,
 * 메모리 해제는 소유자인 상대편 정리 경로에 맡긴다.
 * 두 곳에서 해제하면 이중 해제가 된다.
 *
 * 실행 컨텍스트: bind 실패 경로와 unbind 경로(프로세스 컨텍스트).
 * pci_epc_clear_bar() 안에서 epc->lock 을 잡는다.
 *
 * 호출 체인:
 *   epf_ntb_epc_init_interface() 실패 경로 또는
 *   epf_ntb_epc_cleanup_interface() → [epf_ntb_peer_spad_bar_clear]
 *     → pci_epc_clear_bar()
 */
static void epf_ntb_peer_spad_bar_clear(struct epf_ntb_epc *ntb_epc)
{
	/* [한국어] 걷을 BAR 의 서술자. */
	struct pci_epf_bar *epf_bar;
	/* [한국어] peer 스크래치패드 BAR 번호. */
	enum pci_barno barno;
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] BAR 를 걷을 컨트롤러. */
	struct pci_epc *epc;

	/* [한국어] 이 인터페이스의 컨트롤러. */
	epc = ntb_epc->epc;
	/* [한국어] 함수 번호. */
	func_no = ntb_epc->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb_epc->vfunc_no;
	/* [한국어] peer 스크래치패드로 쓰던 BAR 번호. */
	barno = ntb_epc->epf_ntb_bar[BAR_PEER_SPAD];
	/* [한국어] 그 BAR 의 서술자. */
	epf_bar = &ntb_epc->epf_bar[barno];
	/* [한국어] 인바운드 ATU 를 지운다. 이 BAR 뒤의 메모리는 상대편 인터페이스가
	 * 잡은 것이므로 여기서 해제하지 않는다 — 그래서 clear 만 한다. */
	pci_epc_clear_bar(epc, func_no, vfunc_no, epf_bar);
}

/**
 * epf_ntb_peer_spad_bar_set() - Set peer scratchpad BAR
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @type: PRIMARY interface or SECONDARY interface
 *
 *+-----------------+------->+------------------+        +-----------------+
 *|       BAR0      |        |  CONFIG REGION   |        |       BAR0      |
 *+-----------------+----+   +------------------+<-------+-----------------+
 *|       BAR1      |    |   |SCRATCHPAD REGION |        |       BAR1      |
 *+-----------------+    +-->+------------------+<-------+-----------------+
 *|       BAR2      |            Local Memory            |       BAR2      |
 *+-----------------+                                    +-----------------+
 *|       BAR3      |                                    |       BAR3      |
 *+-----------------+                                    +-----------------+
 *|       BAR4      |                                    |       BAR4      |
 *+-----------------+                                    +-----------------+
 *|       BAR5      |                                    |       BAR5      |
 *+-----------------+                                    +-----------------+
 *  EP CONTROLLER 1                                        EP CONTROLLER 2
 *
 * Set BAR1 of EP CONTROLLER 2 which contains the HOST2's peer scratchpad
 * region. While BAR1 is the default peer scratchpad BAR, an NTB could have
 * other BARs for peer scratchpad (because of 64-bit BARs or reserved BARs).
 * This function can get the exact BAR used for peer scratchpad from
 * epf_ntb_bar[BAR_PEER_SPAD].
 *
 * Since HOST2's peer scratchpad is also HOST1's self scratchpad, this function
 * gets the address of peer scratchpad from
 * peer_ntb_epc->epf_ntb_bar[BAR_CONFIG].
 */
/* [한국어]
 * epf_ntb_peer_spad_bar_set - 상대편의 스크래치패드를 이쪽 호스트의 peer 스크래치패드 BAR 로 노출한다
 *
 * @ntb: 함수 전체 상태
 * @type: BAR 를 걸 쪽 인터페이스
 * @return: 0 성공, 그 밖에는 pci_epc_set_bar() 의 실패 코드
 *
 * 왜 필요한가: NTB 스크래치패드의 본질은 "같은 물리 메모리를 두 호스트가
 * 서로 다른 BAR 로 본다" 는 것이다. HOST2 의 자기 스크래치패드가 곧
 * HOST1 의 peer 스크래치패드다. 이 함수가 그 겹침을 실제로 만든다.
 *
 * 동작 단계:
 *   (1) 상대편(!type)의 config BAR 물리 주소를 얻는다.
 *   (2) 그 주소에 상대편이 기록해 둔 spad_offset 을 더한다 — 제어 영역을
 *       건너뛴 자리가 스크래치패드의 시작이다.
 *   (3) 크기는 상대편이 확정한 spad_size 로 맞춘다.
 *   (4) 그 주소·크기로 이쪽 BAR 를 건다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 에러 경로: 실패하면 호출자가 config BAR 를 걷고 초기화를 접는다.
 *
 * 호출 체인:
 *   epf_ntb_epc_init_interface() → [epf_ntb_peer_spad_bar_set] → pci_epc_set_bar()
 */
static int epf_ntb_peer_spad_bar_set(struct epf_ntb *ntb,
				     enum pci_epc_interface_type type)
{
	/* [한국어] 상대편(peer)과 이쪽 인터페이스의 상태. */
	struct epf_ntb_epc *peer_ntb_epc, *ntb_epc;
	/* [한국어] 상대편 config BAR 서술자와 이쪽 peer 스크래치패드 BAR 서술자. */
	struct pci_epf_bar *peer_epf_bar, *epf_bar;
	/* [한국어] 각각의 실제 BAR 번호. */
	enum pci_barno peer_barno, barno;
	/* [한국어] 상대편 영역 안에서 스크래치패드가 시작되는 오프셋. */
	u32 peer_spad_offset;
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] BAR 를 걸 컨트롤러(이쪽). */
	struct pci_epc *epc;
	/* [한국어] 로그용. */
	struct device *dev;
	/* [한국어] 실패 코드. */
	int ret;

	/* [한국어] EPF 디바이스 기준 로그. */
	dev = &ntb->epf->dev;

	/* [한국어] 상대편 인터페이스. !type 은 PRIMARY(0)와 SECONDARY(1)를 뒤집는 관용구다. */
	peer_ntb_epc = ntb->epc[!type];
	/* [한국어] 상대편의 config + 자기 스크래치패드 BAR 번호. */
	peer_barno = peer_ntb_epc->epf_ntb_bar[BAR_CONFIG];
	/* [한국어] 그 BAR 의 서술자. 물리 주소가 상대편 영역의 시작이다. */
	peer_epf_bar = &peer_ntb_epc->epf_bar[peer_barno];

	/* [한국어] 이쪽 인터페이스. */
	ntb_epc = ntb->epc[type];
	/* [한국어] 이쪽에서 peer 스크래치패드로 쓸 BAR 번호. */
	barno = ntb_epc->epf_ntb_bar[BAR_PEER_SPAD];
	/* [한국어] 그 BAR 의 서술자. 아래에서 직접 채운다. */
	epf_bar = &ntb_epc->epf_bar[barno];
	/* [한국어] 이쪽 함수 번호. */
	func_no = ntb_epc->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb_epc->vfunc_no;
	/* [한국어] 이쪽 컨트롤러. */
	epc = ntb_epc->epc;

	/* [한국어] 상대편 영역 안에서 스크래치패드가 시작되는 오프셋.
	 * epf_ntb_config_spad_bar_alloc() 이 ctrl->spad_offset 에 기록해 둔 값이다. */
	peer_spad_offset = peer_ntb_epc->reg->spad_offset;
	/* [한국어] 핵심 계산이다. 상대편이 잡아 둔 물리 메모리의 스크래치패드 부분을
	 * 그대로 이쪽 BAR 뒤에 건다. 즉 같은 물리 메모리가 상대편에게는
	 * "내 스크래치패드", 이쪽 호스트에게는 "peer 스크래치패드" 로 보인다.
	 * 두 호스트가 값을 주고받는 통로가 바로 이 겹침이다. */
	epf_bar->phys_addr = peer_epf_bar->phys_addr + peer_spad_offset;
	/* [한국어] 크기는 상대편이 확정한 스크래치패드 크기와 같아야 한다. */
	epf_bar->size = peer_ntb_epc->spad_size;
	/* [한국어] 서술자에 BAR 번호를 채운다. */
	epf_bar->barno = barno;
	/* [한국어] 32비트 메모리 BAR 로 선언한다. */
	epf_bar->flags = PCI_BASE_ADDRESS_MEM_TYPE_32;

	/* [한국어] 인바운드 ATU 를 건다. 여기서 크기가 2의 거듭제곱인지 등이 검증된다. */
	ret = pci_epc_set_bar(epc, func_no, vfunc_no, epf_bar);
	/* [한국어] 실패하면 스크래치패드 통신이 불가능하다. */
	if (ret) {
		/* [한국어] 어느 인터페이스인지 남긴다. */
		dev_err(dev, "%s intf: peer SPAD BAR set failed\n",
			pci_epc_interface_string(type));
		return ret;
	}

	return 0;
}

/**
 * epf_ntb_config_sspad_bar_clear() - Clear Config + Self scratchpad BAR
 * @ntb_epc: EPC associated with one of the HOST which holds peer's outbound
 *	     address.
 *
 * +-----------------+------->+------------------+        +-----------------+
 * |       BAR0      |        |  CONFIG REGION   |        |       BAR0      |
 * +-----------------+----+   +------------------+<-------+-----------------+
 * |       BAR1      |    |   |SCRATCHPAD REGION |        |       BAR1      |
 * +-----------------+    +-->+------------------+<-------+-----------------+
 * |       BAR2      |            Local Memory            |       BAR2      |
 * +-----------------+                                    +-----------------+
 * |       BAR3      |                                    |       BAR3      |
 * +-----------------+                                    +-----------------+
 * |       BAR4      |                                    |       BAR4      |
 * +-----------------+                                    +-----------------+
 * |       BAR5      |                                    |       BAR5      |
 * +-----------------+                                    +-----------------+
 *   EP CONTROLLER 1                                        EP CONTROLLER 2
 *
 * Clear BAR0 of EP CONTROLLER 1 which contains the HOST1's config and
 * self scratchpad region (removes inbound ATU configuration). While BAR0 is
 * the default self scratchpad BAR, an NTB could have other BARs for self
 * scratchpad (because of reserved BARs). This function can get the exact BAR
 * used for self scratchpad from epf_ntb_bar[BAR_CONFIG].
 *
 * Please note the self scratchpad region and config region is combined to
 * a single region and mapped using the same BAR. Also note HOST2's peer
 * scratchpad is HOST1's self scratchpad.
 */
/* [한국어]
 * epf_ntb_config_sspad_bar_clear - config + 자기 스크래치패드 BAR 의 인바운드 매핑을 지운다
 *
 * @ntb_epc: 정리할 인터페이스의 상태
 * @return: 없음
 *
 * 왜 필요한가: 이 BAR 를 걷어야 호스트가 더 이상 공유 레지스터에
 * 명령을 써 넣을 수 없다. 메모리 자체는 아직 살아 있고,
 * epf_ntb_config_spad_bar_free() 가 따로 해제한다 — 정리 순서상
 * "길을 먼저 끊고 그 다음에 메모리를 돌려준다" 를 지키기 위한 분리다.
 *
 * 실행 컨텍스트: bind 실패 경로와 unbind 경로(프로세스 컨텍스트).
 * pci_epc_clear_bar() 안에서 epc->lock 을 잡는다.
 *
 * 호출 체인:
 *   epf_ntb_epc_cleanup_interface() → [epf_ntb_config_sspad_bar_clear]
 *     → pci_epc_clear_bar()
 */
static void epf_ntb_config_sspad_bar_clear(struct epf_ntb_epc *ntb_epc)
{
	/* [한국어] 걷을 BAR 의 서술자. */
	struct pci_epf_bar *epf_bar;
	/* [한국어] config BAR 번호. */
	enum pci_barno barno;
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] BAR 를 걷을 컨트롤러. */
	struct pci_epc *epc;

	/* [한국어] 이 인터페이스의 컨트롤러. */
	epc = ntb_epc->epc;
	/* [한국어] 함수 번호. */
	func_no = ntb_epc->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb_epc->vfunc_no;
	/* [한국어] config + 자기 스크래치패드 BAR 번호. */
	barno = ntb_epc->epf_ntb_bar[BAR_CONFIG];
	/* [한국어] 그 BAR 의 서술자. */
	epf_bar = &ntb_epc->epf_bar[barno];
	/* [한국어] 인바운드 ATU 를 지운다. 메모리 자체는 남아 있고
	 * epf_ntb_config_spad_bar_free() 가 따로 해제한다. */
	pci_epc_clear_bar(epc, func_no, vfunc_no, epf_bar);
}

/**
 * epf_ntb_config_sspad_bar_set() - Set Config + Self scratchpad BAR
 * @ntb_epc: EPC associated with one of the HOST which holds peer's outbound
 *	     address.
 *
 * +-----------------+------->+------------------+        +-----------------+
 * |       BAR0      |        |  CONFIG REGION   |        |       BAR0      |
 * +-----------------+----+   +------------------+<-------+-----------------+
 * |       BAR1      |    |   |SCRATCHPAD REGION |        |       BAR1      |
 * +-----------------+    +-->+------------------+<-------+-----------------+
 * |       BAR2      |            Local Memory            |       BAR2      |
 * +-----------------+                                    +-----------------+
 * |       BAR3      |                                    |       BAR3      |
 * +-----------------+                                    +-----------------+
 * |       BAR4      |                                    |       BAR4      |
 * +-----------------+                                    +-----------------+
 * |       BAR5      |                                    |       BAR5      |
 * +-----------------+                                    +-----------------+
 *   EP CONTROLLER 1                                        EP CONTROLLER 2
 *
 * Map BAR0 of EP CONTROLLER 1 which contains the HOST1's config and
 * self scratchpad region. While BAR0 is the default self scratchpad BAR, an
 * NTB could have other BARs for self scratchpad (because of reserved BARs).
 * This function can get the exact BAR used for self scratchpad from
 * epf_ntb_bar[BAR_CONFIG].
 *
 * Please note the self scratchpad region and config region is combined to
 * a single region and mapped using the same BAR. Also note HOST2's peer
 * scratchpad is HOST1's self scratchpad.
 */
/* [한국어]
 * epf_ntb_config_sspad_bar_set - config + 자기 스크래치패드 영역을 BAR 로 노출한다
 *
 * @ntb_epc: BAR 를 걸 인터페이스의 상태
 * @return: 0 성공, 그 밖에는 pci_epc_set_bar() 의 실패 코드
 *
 * 왜 필요한가: 이 BAR 가 열려야 호스트가 공유 레지스터에 명령을 쓸 수 있다.
 * 그래서 인터페이스 초기화의 첫 단계다.
 *
 * 동작 단계: 물리 주소·크기·플래그는 이미 pci_epf_alloc_space() 가
 * 채워 두었으므로, 여기서는 그 서술자를 그대로 컨트롤러에 넘겨
 * 인바운드 ATU 를 거는 일만 한다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 에러 경로: 실패하면 되돌릴 것이 없으므로 곧바로 반환한다.
 *
 * 호출 체인:
 *   epf_ntb_epc_init_interface() → [epf_ntb_config_sspad_bar_set] → pci_epc_set_bar()
 */
static int epf_ntb_config_sspad_bar_set(struct epf_ntb_epc *ntb_epc)
{
	/* [한국어] 걸어 줄 BAR 서술자. 이미 pci_epf_alloc_space() 가 물리 주소·크기·
	 * 플래그를 채워 두었으므로 여기서는 손대지 않는다. */
	struct pci_epf_bar *epf_bar;
	/* [한국어] config + 자기 스크래치패드가 놓인 BAR 번호. */
	enum pci_barno barno;
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] 로그 대상을 얻기 위한 함수 전체 상태. */
	struct epf_ntb *ntb;
	/* [한국어] BAR 를 걸 컨트롤러. */
	struct pci_epc *epc;
	/* [한국어] 로그용. */
	struct device *dev;
	/* [한국어] 실패 코드. */
	int ret;

	/* [한국어] 역방향 포인터로 함수 전체 상태에 닿는다. */
	ntb = ntb_epc->epf_ntb;
	/* [한국어] 로그는 EPF 디바이스 기준으로 남긴다. */
	dev = &ntb->epf->dev;

	/* [한국어] 이 인터페이스의 컨트롤러. */
	epc = ntb_epc->epc;
	/* [한국어] 함수 번호. */
	func_no = ntb_epc->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb_epc->vfunc_no;
	/* [한국어] config BAR 번호. */
	barno = ntb_epc->epf_ntb_bar[BAR_CONFIG];
	/* [한국어] 그 BAR 의 서술자. 물리 주소는 dma_alloc_coherent 가 준 것이다. */
	epf_bar = &ntb_epc->epf_bar[barno];

	/* [한국어] 인바운드 ATU 를 건다. 이 순간부터 호스트가 이 BAR 에 접근하면
	 * 우리가 잡아 둔 메모리에 닿는다 — 즉 호스트가 command 를 써 넣을 수 있게 된다. */
	ret = pci_epc_set_bar(epc, func_no, vfunc_no, epf_bar);
	/* [한국어] 실패하면 호스트가 이 함수와 대화할 통로가 없으므로 치명적이다. */
	if (ret) {
		/* [한국어] 어느 인터페이스인지 남긴다. */
		dev_err(dev, "%s inft: Config/Status/SPAD BAR set failed\n",
			pci_epc_interface_string(ntb_epc->type));
		return ret;
	}

	return 0;
}

/**
 * epf_ntb_config_spad_bar_free() - Free the physical memory associated with
 *   config + scratchpad region
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 *
 * +-----------------+------->+------------------+        +-----------------+
 * |       BAR0      |        |  CONFIG REGION   |        |       BAR0      |
 * +-----------------+----+   +------------------+<-------+-----------------+
 * |       BAR1      |    |   |SCRATCHPAD REGION |        |       BAR1      |
 * +-----------------+    +-->+------------------+<-------+-----------------+
 * |       BAR2      |            Local Memory            |       BAR2      |
 * +-----------------+                                    +-----------------+
 * |       BAR3      |                                    |       BAR3      |
 * +-----------------+                                    +-----------------+
 * |       BAR4      |                                    |       BAR4      |
 * +-----------------+                                    +-----------------+
 * |       BAR5      |                                    |       BAR5      |
 * +-----------------+                                    +-----------------+
 *   EP CONTROLLER 1                                        EP CONTROLLER 2
 *
 * Free the Local Memory mentioned in the above diagram. After invoking this
 * function, any of config + self scratchpad region of HOST1 or peer scratchpad
 * region of HOST2 should not be accessed.
 */
/* [한국어]
 * epf_ntb_config_spad_bar_free - 두 인터페이스의 config + 스크래치패드 메모리를 해제한다
 *
 * @ntb: 함수 전체 상태
 * @return: 없음
 *
 * 왜 필요한가: 이 메모리는 dma_alloc_coherent 로 잡힌 것이라 반드시
 * 짝이 되는 해제가 필요하다. devm_ 이 아니므로 자동으로 풀리지 않는다.
 *
 * 동작 단계: PRIMARY, SECONDARY 를 돌며 config BAR 번호를 찾아
 * pci_epf_free_space() 를 부른다. reg 가 NULL 인 인터페이스는 건너뛴다 —
 * 첫 인터페이스에서 할당이 실패해 두 번째는 잡히지 않았을 수 있기 때문이다.
 *
 * 실행 컨텍스트: bind 실패 경로와 unbind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_bind() 실패 경로 또는 epf_ntb_unbind()
 *     → [epf_ntb_config_spad_bar_free] → pci_epf_free_space()
 */
static void epf_ntb_config_spad_bar_free(struct epf_ntb *ntb)
{
	/* [한국어] 두 인터페이스를 순회할 반복자. */
	enum pci_epc_interface_type type;
	/* [한국어] 각 인터페이스의 상태. */
	struct epf_ntb_epc *ntb_epc;
	/* [한국어] 해제할 BAR 번호. */
	enum pci_barno barno;
	/* [한국어] pci_epf_free_space() 에 넘길 EPF 디바이스. */
	struct pci_epf *epf;

	/* [한국어] 두 인터페이스가 공유하는 EPF 디바이스. */
	epf = ntb->epf;
	/* [한국어] PRIMARY, SECONDARY 두 영역을 모두 해제한다. */
	for (type = PRIMARY_INTERFACE; type <= SECONDARY_INTERFACE; type++) {
		/* [한국어] 이 인터페이스의 상태. */
		ntb_epc = ntb->epc[type];
		/* [한국어] config + 스크래치패드가 놓인 BAR 번호. */
		barno = ntb_epc->epf_ntb_bar[BAR_CONFIG];
		/* [한국어] 할당에 성공한 인터페이스만 해제한다. 첫 인터페이스에서 실패해
		 * 두 번째는 잡히지 않았을 수 있다. */
		if (ntb_epc->reg)
			/* [한국어] dma_free_coherent 로 메모리를 돌려주고 BAR 서술자를 지운다.
			 * type 이 어느 BAR 배열을 정리할지 정한다. */
			pci_epf_free_space(epf, ntb_epc->reg, barno, type);
	}
}

/**
 * epf_ntb_config_spad_bar_alloc() - Allocate memory for config + scratchpad
 *   region
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @type: PRIMARY interface or SECONDARY interface
 *
 * +-----------------+------->+------------------+        +-----------------+
 * |       BAR0      |        |  CONFIG REGION   |        |       BAR0      |
 * +-----------------+----+   +------------------+<-------+-----------------+
 * |       BAR1      |    |   |SCRATCHPAD REGION |        |       BAR1      |
 * +-----------------+    +-->+------------------+<-------+-----------------+
 * |       BAR2      |            Local Memory            |       BAR2      |
 * +-----------------+                                    +-----------------+
 * |       BAR3      |                                    |       BAR3      |
 * +-----------------+                                    +-----------------+
 * |       BAR4      |                                    |       BAR4      |
 * +-----------------+                                    +-----------------+
 * |       BAR5      |                                    |       BAR5      |
 * +-----------------+                                    +-----------------+
 *   EP CONTROLLER 1                                        EP CONTROLLER 2
 *
 * Allocate the Local Memory mentioned in the above diagram. The size of
 * CONFIG REGION is sizeof(struct epf_ntb_ctrl) and size of SCRATCHPAD REGION
 * is obtained from "spad-count" configfs entry.
 *
 * The size of both config region and scratchpad region has to be aligned,
 * since the scratchpad region will also be mapped as PEER SCRATCHPAD of
 * other host using a separate BAR.
 */
/* [한국어]
 * epf_ntb_config_spad_bar_alloc - 한 인터페이스의 config + 스크래치패드 영역 크기를 정하고 메모리를 잡는다
 *
 * @ntb: 함수 전체 상태
 * @type: 할당할 인터페이스
 * @return: 0 성공, -EINVAL 이면 크기 제약을 만족할 수 없음, -ENOMEM 이면 할당 실패
 *
 * 왜 필요한가: 이 파일에서 가장 계산이 복잡한 함수다. 하나의 BAR 안에
 * 제어 레지스터, MSI-X 표, PBA, 스크래치패드를 모두 담아야 하고,
 * 그 중 스크래치패드 부분은 상대편 BAR 로도 통째로 노출되어야 한다.
 * 그래서 컨트롤러 정렬 요구, 고정 BAR 크기, 상대편 BAR 의 고정 크기까지
 * 한꺼번에 만족시키는 배치를 찾아야 한다.
 *
 * 동작 단계:
 *   (1) 이쪽 BAR 의 고정 크기와 정렬 요구, 상대편 BAR 의 고정 크기를 읽는다.
 *   (2) 제어 영역 = 공유 레지스터 구조체. MSI-X 를 쓰면 그 뒤에 표와 PBA 를
 *       8바이트 정렬로 덧붙이고, 표의 위치를 기록해 둔다.
 *   (3) 정렬 요구가 있으면 그 배수로, 없으면 2의 거듭제곱으로 올린다.
 *   (4) 상대편 BAR 가 고정 크기면 스크래치패드를 거기에 맞춘다(작으면 워드 수를 줄인다).
 *   (5) 스크래치패드가 제어 영역보다 크면 제어 영역을 늘려, 스크래치패드의
 *       시작 오프셋이 자기 크기의 배수가 되게 한다.
 *   (6) pci_epf_alloc_space() 로 실제 메모리를 잡고 공유 레지스터를 초기화한다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트). 안쪽에서 dma_alloc_coherent
 * 를 부르므로 GFP_KERNEL 로 잠들 수 있다.
 *
 * 에러 경로: 어느 단계든 실패하면 호출자가 이미 잡힌 쪽을 해제한다.
 *
 * 호출 체인:
 *   epf_ntb_bind() → epf_ntb_config_spad_bar_alloc_interface()
 *     → [epf_ntb_config_spad_bar_alloc] → pci_epf_alloc_space()
 */
static int epf_ntb_config_spad_bar_alloc(struct epf_ntb *ntb,
					 enum pci_epc_interface_type type)
{
	/* [한국어] 이쪽과 상대편 컨트롤러의 능력표. 고정 BAR 크기와 정렬을 여기서 읽는다. */
	const struct pci_epc_features *peer_epc_features, *epc_features;
	/* [한국어] 이쪽과 상대편 인터페이스의 상태. */
	struct epf_ntb_epc *peer_ntb_epc, *ntb_epc;
	/* [한국어] MSI-X 표 크기, PBA(Pending Bit Array) 크기, 정렬 요구. */
	size_t msix_table_size, pba_size, align;
	/* [한국어] 상대편 peer 스크래치패드 BAR 번호와 이쪽 config BAR 번호. */
	enum pci_barno peer_barno, barno;
	/* [한국어] 할당한 메모리의 시작을 공유 레지스터 구조체로 보기 위한 포인터. */
	struct epf_ntb_ctrl *ctrl;
	/* [한국어] 스크래치패드 영역 크기와 제어 영역 크기. */
	u32 spad_size, ctrl_size;
	/* [한국어] 이쪽 BAR 전체 크기와 상대편 BAR 의 고정 크기. */
	u64 size, peer_size;
	/* [한국어] EPF 디바이스. */
	struct pci_epf *epf;
	/* [한국어] 로그용. */
	struct device *dev;
	/* [한국어] MSI-X 지원 여부. */
	bool msix_capable;
	/* [한국어] 스크래치패드 워드 개수(사용자 설정값에서 시작해 줄어들 수 있다). */
	u32 spad_count;
	/* [한국어] 할당된 메모리의 커널 가상 주소. */
	void *base;

	/* [한국어] 두 인터페이스가 공유하는 EPF 디바이스. */
	epf = ntb->epf;
	/* [한국어] 로그 대상. */
	dev = &epf->dev;
	/* [한국어] 이번에 할당할 인터페이스의 상태. */
	ntb_epc = ntb->epc[type];

	/* [한국어] 이쪽 컨트롤러의 능력표. */
	epc_features = ntb_epc->epc_features;
	/* [한국어] config + 자기 스크래치패드를 담을 BAR 번호. */
	barno = ntb_epc->epf_ntb_bar[BAR_CONFIG];
	/* [한국어] 그 BAR 가 고정 크기라면 그 값. 0 이면 크기를 우리가 정할 수 있다. */
	size = epc_features->bar[barno].fixed_size;
	/* [한국어] 이 컨트롤러가 요구하는 주소 정렬. */
	align = epc_features->align;

	/* [한국어] 상대편 인터페이스. 우리 스크래치패드는 상대편에게 peer 스크래치패드로
	 * 보이므로 상대편 BAR 의 제약도 함께 고려해야 한다. */
	peer_ntb_epc = ntb->epc[!type];
	/* [한국어] 상대편 컨트롤러의 능력표. */
	peer_epc_features = peer_ntb_epc->epc_features;
	/* [한국어] peer 스크래치패드에 쓸 BAR 번호. 이쪽 배열에서 읽는 점에 주의 —
	 * 상대편 BAR 번호가 아니라 이쪽에 배정된 번호를 상대편 능력표로
	 * 조회하고 있다. 상류 코드 그대로 둔다. */
	peer_barno = ntb_epc->epf_ntb_bar[BAR_PEER_SPAD];
	/* [한국어] 그 BAR 가 고정 크기라면 그 값. */
	peer_size = peer_epc_features->bar[peer_barno].fixed_size;

	/* Check if epc_features is populated incorrectly */
	/* [한국어] 상류 주석대로 능력표가 잘못 채워진 경우를 잡는다.
	 * 고정 크기가 정렬 요구의 배수가 아니면 이후 계산이 전부 어긋난다. */
	if ((!IS_ALIGNED(size, align)))
		/* [한국어] 컨트롤러 드라이버의 버그이므로 -EINVAL 로 거절한다. */
		return -EINVAL;

	/* [한국어] 사용자가 configfs 로 정한 스크래치패드 워드 개수. */
	spad_count = ntb->spad_count;

	/* [한국어] 제어 영역 크기는 공유 레지스터 구조체 크기 그대로다.
	 * 이 구조체는 __packed 라 패딩이 없어 호스트와 배치가 정확히 일치한다. */
	ctrl_size = sizeof(struct epf_ntb_ctrl);
	/* [한국어] 스크래치패드는 32비트 워드 배열이므로 개수 x 4 바이트다. */
	spad_size = spad_count * 4;

	/* [한국어] MSI-X 를 지원하는가. */
	msix_capable = epc_features->msix_capable;
	/* [한국어] 지원하면 MSI-X 표와 PBA 를 이 BAR 안에 함께 둬야 한다.
	 * PCI 스펙상 MSI-X 표는 어떤 BAR 안의 오프셋으로 지정되기 때문이다. */
	if (msix_capable) {
		/* [한국어] 표 한 항목은 PCI_MSIX_ENTRY_SIZE(16바이트: 주소 하위/상위/데이터/벡터제어)다. */
		msix_table_size = PCI_MSIX_ENTRY_SIZE * ntb->db_count;
		/* [한국어] MSI-X 표는 8바이트(QWORD) 경계에 놓여야 하므로 제어 영역 끝을 올림한다. */
		ctrl_size = ALIGN(ctrl_size, 8);
		/* [한국어] 표가 시작되는 BAR 내 오프셋을 기록해 둔다.
		 * epf_ntb_configure_interrupt() 가 이 값을 pci_epc_set_msix() 에 넘기고,
		 * epf_ntb_configure_msix() 는 이 오프셋으로 표를 직접 읽는다. */
		ntb_epc->msix_table_offset = ctrl_size;
		/* [한국어] 표가 놓인 BAR 번호도 함께 기록한다. */
		ntb_epc->msix_bar = barno;
		/* Align to QWORD or 8 Bytes */
		/* [한국어] PBA 는 벡터당 1비트라 db_count 비트가 필요하다. 바이트로 올린 뒤
		 * 다시 8바이트 경계로 올린다 — 상류 주석이 말하는 QWORD 정렬이다. */
		pba_size = ALIGN(DIV_ROUND_UP(ntb->db_count, 8), 8);
		/* [한국어] 제어 영역 = 레지스터 + MSI-X 표 + PBA. */
		ctrl_size = ctrl_size + msix_table_size + pba_size;
	}

	/* [한국어] 정렬 요구가 없는 컨트롤러라면? */
	if (!align) {
		/* [한국어] BAR 크기 규칙에 맞게 2의 거듭제곱으로 올린다. */
		ctrl_size = roundup_pow_of_two(ctrl_size);
		/* [한국어] 스크래치패드도 마찬가지로 올린다. */
		spad_size = roundup_pow_of_two(spad_size);
	} else {
		/* [한국어] 정렬 요구가 있으면 그 배수로 올린다. */
		ctrl_size = ALIGN(ctrl_size, align);
		/* [한국어] 스크래치패드도 같은 정렬을 따른다. */
		spad_size = ALIGN(spad_size, align);
	}

	/* [한국어] 상대편 peer 스크래치패드 BAR 가 고정 크기인가? */
	if (peer_size) {
		/* [한국어] 고정 크기가 우리가 원하는 스크래치패드보다 작으면? */
		if (peer_size < spad_size)
			/* [한국어] 들어갈 수 있는 만큼으로 워드 개수를 줄인다. 사용자가 요청한 값보다
			 * 작아질 수 있으며, 그 사실은 ctrl->spad_count 로 호스트에게 전달된다. */
			spad_count = peer_size / 4;
		/* [한국어] 어느 쪽이든 스크래치패드 크기는 상대편 BAR 의 고정 크기에 맞춘다 —
		 * 그 BAR 로 통째로 노출되어야 하기 때문이다. */
		spad_size = peer_size;
	}

	/*
	 * In order to make sure SPAD offset is aligned to its size,
	 * expand control region size to the size of SPAD if SPAD size
	 * is greater than control region size.
	 */
	/* [한국어] 상류 주석대로, 스크래치패드가 제어 영역보다 크면 제어 영역을
	 * 스크래치패드 크기까지 늘린다. 그래야 스크래치패드의 시작 오프셋
	 * (=제어 영역 크기)이 자기 크기의 배수가 되어, 그 영역만 떼어
	 * 상대편 BAR 로 거는 것이 정렬 규칙에 맞는다. */
	if (spad_size > ctrl_size)
		/* [한국어] 제어 영역을 늘린다. */
		ctrl_size = spad_size;

	/* [한국어] 이 BAR 가 고정 크기가 아니라면? */
	if (!size)
		/* [한국어] 우리가 필요한 만큼(제어 + 스크래치패드)을 BAR 크기로 삼는다. */
		size = ctrl_size + spad_size;
	/* [한국어] 고정 크기인데 필요한 양보다 작으면 담을 수가 없다. */
	else if (size < ctrl_size + spad_size)
		/* [한국어] 들어가지 않으므로 -EINVAL 로 거절한다. */
		return -EINVAL;

	/* [한국어] 실제 메모리를 잡는다. 안쪽에서 dma_alloc_coherent() 를 쓰므로
	 * 호스트가 BAR 로 읽고 쓰는 것과 이쪽 CPU 가 보는 값이 일관된다.
	 * type 인자가 PRIMARY/SECONDARY 중 어느 BAR 배열을 채울지 정한다. */
	base = pci_epf_alloc_space(epf, size, barno, epc_features, type);
	/* [한국어] 할당 실패. */
	if (!base) {
		/* [한국어] 어느 인터페이스인지 남긴다. */
		dev_err(dev, "%s intf: Config/Status/SPAD alloc region fail\n",
			pci_epc_interface_string(type));
		return -ENOMEM;
	}

	/* [한국어] 이 인터페이스의 공유 레지스터 시작 주소로 기억한다.
	 * 이후 모든 코드가 ntb_epc->reg 로 이 영역에 접근한다. */
	ntb_epc->reg = base;

	/* [한국어] 구조체 포인터로 다시 본다. */
	ctrl = ntb_epc->reg;
	/* [한국어] 스크래치패드가 시작되는 BAR 내 오프셋을 호스트에게 알린다.
	 * epf_ntb_peer_spad_bar_set() 도 이 값을 써서 상대편 BAR 의
	 * 물리 주소를 계산한다. */
	ctrl->spad_offset = ctrl_size;
	/* [한국어] 실제로 쓸 수 있는 스크래치패드 워드 개수(줄어들었을 수 있다). */
	ctrl->spad_count = spad_count;
	/* [한국어] 쓸 수 있는 메모리 윈도우 개수. */
	ctrl->num_mws = ntb->num_mws;
	/* [한국어] 도어벨 한 칸의 크기. 정렬 요구가 있으면 그 값, 없으면 4바이트.
	 * 호스트는 이 값으로 도어벨 i 의 주소를 계산한다. */
	ctrl->db_entry_size = align ? align : 4;
	/* [한국어] 정리 경로와 peer BAR 설정이 쓸 수 있도록 최종 스크래치패드 크기를 보관한다. */
	ntb_epc->spad_size = spad_size;

	return 0;
}

/**
 * epf_ntb_config_spad_bar_alloc_interface() - Allocate memory for config +
 *   scratchpad region for each of PRIMARY and SECONDARY interface
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 *
 * Wrapper for epf_ntb_config_spad_bar_alloc() which allocates memory for
 * config + scratchpad region for a specific interface
 */
/* [한국어]
 * epf_ntb_config_spad_bar_alloc_interface - 두 인터페이스 각각에 대해 영역 할당을 반복한다
 *
 * @ntb: 함수 전체 상태
 * @return: 0 성공, 그 밖에는 첫 실패의 오류 코드
 *
 * 왜 필요한가: PRIMARY 와 SECONDARY 는 각자 자기 config + 스크래치패드
 * 영역을 갖는다. 그리고 그 스크래치패드가 상대편의 peer 스크래치패드가
 * 되므로 반드시 둘 다 있어야 NTB 가 성립한다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트).
 *
 * 에러 경로: 하나라도 실패하면 즉시 반환한다. 먼저 성공한 쪽의 해제는
 * 상위 epf_ntb_bind() 의 err_bar_alloc 경로가 맡는다.
 *
 * 호출 체인:
 *   epf_ntb_bind() → [epf_ntb_config_spad_bar_alloc_interface]
 *     → epf_ntb_config_spad_bar_alloc()
 */
static int epf_ntb_config_spad_bar_alloc_interface(struct epf_ntb *ntb)
{
	/* [한국어] 두 인터페이스를 순회할 반복자. */
	enum pci_epc_interface_type type;
	/* [한국어] 로그용. */
	struct device *dev;
	/* [한국어] 하위 호출의 실패를 담는다. */
	int ret;

	/* [한국어] EPF 디바이스 기준 로그. */
	dev = &ntb->epf->dev;

	/* [한국어] PRIMARY 와 SECONDARY 각각 자기 config + 스크래치패드 영역을 갖는다.
	 * 두 영역은 서로의 peer 스크래치패드로도 쓰이므로 둘 다 있어야 한다. */
	for (type = PRIMARY_INTERFACE; type <= SECONDARY_INTERFACE; type++) {
		/* [한국어] 인터페이스 하나의 영역 할당. */
		ret = epf_ntb_config_spad_bar_alloc(ntb, type);
		/* [한국어] 하나라도 실패하면 전체 실패다. 이미 잡힌 쪽은 상위
		 * epf_ntb_bind() 의 err_bar_alloc 경로가 되돌린다. */
		if (ret) {
			/* [한국어] 어느 인터페이스인지 남긴다. */
			dev_err(dev, "%s intf: Config/SPAD BAR alloc failed\n",
				pci_epc_interface_string(type));
			return ret;
		}
	}

	return 0;
}

/**
 * epf_ntb_free_peer_mem() - Free memory allocated in peers outbound address
 *   space
 * @ntb_epc: EPC associated with one of the HOST which holds peers outbound
 *   address regions
 *
 * +-----------------+    +---->+----------------+-----------+-----------------+
 * |       BAR0      |    |     |   Doorbell 1   +-----------> MSI|X ADDRESS 1 |
 * +-----------------+    |     +----------------+           +-----------------+
 * |       BAR1      |    |     |   Doorbell 2   +---------+ |                 |
 * +-----------------+----+     +----------------+         | |                 |
 * |       BAR2      |          |   Doorbell 3   +-------+ | +-----------------+
 * +-----------------+----+     +----------------+       | +-> MSI|X ADDRESS 2 |
 * |       BAR3      |    |     |   Doorbell 4   +-----+ |   +-----------------+
 * +-----------------+    |     |----------------+     | |   |                 |
 * |       BAR4      |    |     |                |     | |   +-----------------+
 * +-----------------+    |     |      MW1       +---+ | +-->+ MSI|X ADDRESS 3||
 * |       BAR5      |    |     |                |   | |     +-----------------+
 * +-----------------+    +---->-----------------+   | |     |                 |
 *   EP CONTROLLER 1            |                |   | |     +-----------------+
 *                              |                |   | +---->+ MSI|X ADDRESS 4 |
 *                              +----------------+   |       +-----------------+
 *                      (A)      EP CONTROLLER 2     |       |                 |
 *                                 (OB SPACE)        |       |                 |
 *                                                   +------->      MW1        |
 *                                                           |                 |
 *                                                           |                 |
 *                                                   (B)     +-----------------+
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           +-----------------+
 *                                                           PCI Address Space
 *                                                           (Managed by HOST2)
 *
 * Free memory allocated in EP CONTROLLER 2 (OB SPACE) in the above diagram.
 * It'll free Doorbell 1, Doorbell 2, Doorbell 3, Doorbell 4, MW1 (and MW2, MW3,
 * MW4).
 */
/* [한국어]
 * epf_ntb_free_peer_mem - 상대편에게 빌려 준 아웃바운드 주소 공간을 돌려준다
 *
 * @ntb_epc: 아웃바운드 공간의 주인인 인터페이스
 * @return: 없음
 *
 * 왜 필요한가: 도어벨과 메모리 윈도우가 놓이는 창은 상대편 컨트롤러의
 * 아웃바운드 주소 공간에서 빌려 온 것이다. 돌려주지 않으면 그 공간의
 * 비트맵이 새어 나가 다음 bind 가 실패한다.
 *
 * 동작 단계: BAR_DB_MW1 부터 순회하며 mw_addr 가 채워진 항목만
 * pci_epc_mem_free_addr() 로 돌려주고 NULL 로 지운다. NULL 로 지우는 것이
 * 중요하다 — 이 함수는 오류 경로와 unbind 양쪽에서 불릴 수 있어 두 번
 * 해제될 여지가 있기 때문이다.
 *
 * 주의: 루프 조건이 bar < BAR_MW4 라 마지막 창(BAR_MW4)은 이 루프에
 * 들어오지 않는다. 상류 코드 그대로 둔다.
 *
 * 실행 컨텍스트: bind 실패 경로와 unbind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_db_mw_bar_cleanup() → [epf_ntb_free_peer_mem]
 *     → pci_epc_mem_free_addr()
 */
static void epf_ntb_free_peer_mem(struct epf_ntb_epc *ntb_epc)
{
	/* [한국어] 각 BAR 의 서술자. 물리 주소와 크기를 여기서 읽는다. */
	struct pci_epf_bar *epf_bar;
	/* [한국어] 해제할 아웃바운드 창의 커널 가상 주소. */
	void __iomem *mw_addr;
	/* [한국어] 같은 창의 물리 주소. */
	phys_addr_t phys_addr;
	/* [한국어] 논리 BAR 인덱스. */
	enum epf_ntb_bar bar;
	/* [한국어] 실제 BAR 번호. */
	enum pci_barno barno;
	/* [한국어] 공간을 돌려줄 컨트롤러 — 이 인터페이스 자신의 아웃바운드 공간이다. */
	struct pci_epc *epc;
	/* [한국어] 돌려줄 크기. */
	size_t size;

	/* [한국어] 이 인터페이스의 컨트롤러. */
	epc = ntb_epc->epc;

	/* [한국어] 도어벨+MW1 부터 순회한다. 여기서도 조건이 < BAR_MW4 라
	 * 마지막 창은 빠진다 — 상류 코드 그대로 둔다. */
	for (bar = BAR_DB_MW1; bar < BAR_MW4; bar++) {
		/* [한국어] 이 구성요소의 실제 BAR 번호. */
		barno = ntb_epc->epf_ntb_bar[bar];
		/* [한국어] BAR 번호로 색인해 둔 가상 주소를 꺼낸다. */
		mw_addr = ntb_epc->mw_addr[barno];
		/* [한국어] 그 BAR 의 서술자. */
		epf_bar = &ntb_epc->epf_bar[barno];
		/* [한국어] 해제에 필요한 물리 주소. */
		phys_addr = epf_bar->phys_addr;
		/* [한국어] 해제에 필요한 크기. 할당 때와 같은 값이어야 비트맵이 맞는다. */
		size = epf_bar->size;
		/* [한국어] 할당된 적이 있는 창만 해제한다. epf_ntb_epc_create_interface() 가
		 * kzalloc 으로 상태를 잡았으므로 미할당 항목은 NULL 이다. */
		if (mw_addr) {
			/* [한국어] 아웃바운드 주소 공간 비트맵에 이 구간을 돌려주고 iounmap 한다. */
			pci_epc_mem_free_addr(epc, phys_addr, mw_addr, size);
			/* [한국어] 두 번 해제되지 않도록 반드시 NULL 로 지운다 —
			 * epf_ntb_db_mw_bar_cleanup() 이 오류 경로와 unbind 양쪽에서
			 * 불릴 수 있기 때문이다. */
			ntb_epc->mw_addr[barno] = NULL;
		}
	}
}

/**
 * epf_ntb_db_mw_bar_clear() - Clear doorbell and memory BAR
 * @ntb_epc: EPC associated with one of the HOST which holds peer's outbound
 *   address
 *
 * +-----------------+    +---->+----------------+-----------+-----------------+
 * |       BAR0      |    |     |   Doorbell 1   +-----------> MSI|X ADDRESS 1 |
 * +-----------------+    |     +----------------+           +-----------------+
 * |       BAR1      |    |     |   Doorbell 2   +---------+ |                 |
 * +-----------------+----+     +----------------+         | |                 |
 * |       BAR2      |          |   Doorbell 3   +-------+ | +-----------------+
 * +-----------------+----+     +----------------+       | +-> MSI|X ADDRESS 2 |
 * |       BAR3      |    |     |   Doorbell 4   +-----+ |   +-----------------+
 * +-----------------+    |     |----------------+     | |   |                 |
 * |       BAR4      |    |     |                |     | |   +-----------------+
 * +-----------------+    |     |      MW1       +---+ | +-->+ MSI|X ADDRESS 3||
 * |       BAR5      |    |     |                |   | |     +-----------------+
 * +-----------------+    +---->-----------------+   | |     |                 |
 *   EP CONTROLLER 1            |                |   | |     +-----------------+
 *                              |                |   | +---->+ MSI|X ADDRESS 4 |
 *                              +----------------+   |       +-----------------+
 *                      (A)      EP CONTROLLER 2     |       |                 |
 *                                 (OB SPACE)        |       |                 |
 *                                                   +------->      MW1        |
 *                                                           |                 |
 *                                                           |                 |
 *                                                   (B)     +-----------------+
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           +-----------------+
 *                                                           PCI Address Space
 *                                                           (Managed by HOST2)
 *
 * Clear doorbell and memory BARs (remove inbound ATU configuration). In the above
 * diagram it clears BAR2 TO BAR5 of EP CONTROLLER 1 (Doorbell BAR, MW1 BAR, MW2
 * BAR, MW3 BAR and MW4 BAR).
 */
/* [한국어]
 * epf_ntb_db_mw_bar_clear - 도어벨/메모리 윈도우 BAR 의 인바운드 매핑을 지운다
 *
 * @ntb_epc: BAR 를 걷을 인터페이스
 * @return: 없음
 *
 * 왜 필요한가: 아웃바운드 공간을 돌려주기 전에 먼저 인바운드 길을 끊어야
 * 한다. 순서가 뒤바뀌면 아직 살아 있는 BAR 가 이미 해제된 공간을 가리키게 된다.
 *
 * 동작 단계: BAR_DB_MW1 부터 순회하며 각 BAR 에 pci_epc_clear_bar() 를 부른다.
 * 여기서도 루프 조건이 bar < BAR_MW4 라 마지막 창은 빠진다 — 상류 코드 그대로 둔다.
 *
 * 실행 컨텍스트: bind 실패 경로와 unbind 경로(프로세스 컨텍스트).
 * pci_epc_clear_bar() 안에서 epc->lock 을 잡는다.
 *
 * 호출 체인:
 *   epf_ntb_db_mw_bar_cleanup() → [epf_ntb_db_mw_bar_clear] → pci_epc_clear_bar()
 */
static void epf_ntb_db_mw_bar_clear(struct epf_ntb_epc *ntb_epc)
{
	/* [한국어] 각 BAR 의 서술자. */
	struct pci_epf_bar *epf_bar;
	/* [한국어] 논리 BAR 인덱스. */
	enum epf_ntb_bar bar;
	/* [한국어] 실제 BAR 번호. */
	enum pci_barno barno;
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] BAR 를 걷을 컨트롤러. */
	struct pci_epc *epc;

	/* [한국어] 이 인터페이스의 컨트롤러. */
	epc = ntb_epc->epc;

	/* [한국어] 함수 번호. */
	func_no = ntb_epc->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb_epc->vfunc_no;

	/* [한국어] 도어벨+MW1 BAR 부터 MW4 직전까지 순회한다.
	 * 조건이 < BAR_MW4 라서 마지막 창(BAR_MW4)은 이 루프에 들어오지
	 * 않는다 — 상류 코드 그대로 둔다. */
	for (bar = BAR_DB_MW1; bar < BAR_MW4; bar++) {
		/* [한국어] 이 논리 구성요소의 실제 BAR 번호. */
		barno = ntb_epc->epf_ntb_bar[bar];
		/* [한국어] 그 BAR 의 서술자. */
		epf_bar = &ntb_epc->epf_bar[barno];
		/* [한국어] 인바운드 ATU 설정을 지운다. 이후 호스트가 이 BAR 에 접근해도
		 * 아무 데도 닿지 않는다. */
		pci_epc_clear_bar(epc, func_no, vfunc_no, epf_bar);
	}
}

/**
 * epf_ntb_db_mw_bar_cleanup() - Clear doorbell/memory BAR and free memory
 *   allocated in peers outbound address space
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @type: PRIMARY interface or SECONDARY interface
 *
 * Wrapper for epf_ntb_db_mw_bar_clear() to clear HOST1's BAR and
 * epf_ntb_free_peer_mem() which frees up HOST2 outbound memory.
 */
/* [한국어]
 * epf_ntb_db_mw_bar_cleanup - 도어벨/창 BAR 를 걷고 상대편 아웃바운드 메모리까지 되돌린다
 *
 * @ntb: 함수 전체 상태
 * @type: 정리할 인터페이스
 * @return: 없음
 *
 * 왜 필요한가: BAR 는 이쪽 것이고 그 뒤의 아웃바운드 공간은 상대편 것이라,
 * 정리도 두 쪽에 걸쳐 일어난다. 이 함수가 그 짝을 올바른 순서로 묶는다.
 *
 * 동작 단계: (1) 이쪽 BAR 를 걷는다 → (2) 상대편 아웃바운드 공간을 돌려준다.
 *
 * 실행 컨텍스트: bind 실패 경로와 unbind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_db_mw_bar_init() 실패 경로 또는 epf_ntb_epc_cleanup_interface()
 *     → [epf_ntb_db_mw_bar_cleanup]
 *     → epf_ntb_db_mw_bar_clear() → epf_ntb_free_peer_mem()
 */
static void epf_ntb_db_mw_bar_cleanup(struct epf_ntb *ntb,
				      enum pci_epc_interface_type type)
{
	/* [한국어] 이쪽 인터페이스와 상대 인터페이스의 상태. */
	struct epf_ntb_epc *peer_ntb_epc, *ntb_epc;

	/* [한국어] BAR 를 걷을 쪽. */
	ntb_epc = ntb->epc[type];
	/* [한국어] 아웃바운드 메모리를 되돌려 줄 쪽. 이쪽 BAR 뒤에 있던 공간은
	 * 상대 컨트롤러의 아웃바운드 공간에서 빌려 온 것이기 때문이다. */
	peer_ntb_epc = ntb->epc[!type];

	/* [한국어] 먼저 인바운드 매핑(BAR)을 걷는다. 호스트가 더 이상 이 창으로
	 * 들어오지 못하게 하는 것이 먼저다. */
	epf_ntb_db_mw_bar_clear(ntb_epc);
	/* [한국어] 그 다음에 아웃바운드 공간을 해제한다. 순서를 뒤집으면 아직 살아 있는
	 * BAR 가 해제된 공간을 가리키게 된다. */
	epf_ntb_free_peer_mem(peer_ntb_epc);
}

/**
 * epf_ntb_configure_interrupt() - Configure MSI/MSI-X capability
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @type: PRIMARY interface or SECONDARY interface
 *
 * Configure MSI/MSI-X capability for each interface with number of
 * interrupts equal to "db_count" configfs entry.
 */
/* [한국어]
 * epf_ntb_configure_interrupt - 이 인터페이스의 MSI/MSI-X 능력을 도어벨 개수만큼 설정한다
 *
 * @ntb: 함수 전체 상태
 * @type: 설정할 인터페이스
 * @return: 0 성공, -EINVAL 이면 MSI/MSI-X 를 둘 다 지원하지 않거나
 *          도어벨 개수가 한계를 넘음, 그 밖에는 컨트롤러 설정 실패 코드
 *
 * 왜 필요한가: 도어벨은 결국 상대 호스트로 나가는 인터럽트다. 호스트가
 * 열거할 때 이 함수의 MSI/MSI-X 능력 레지스터를 보고 벡터를 할당하므로,
 * BAR 를 걸기 전에 개수를 미리 선언해 두어야 한다.
 *
 * 동작 단계: 능력표에서 지원 여부를 확인하고, db_count 를 검증한 뒤,
 * 지원하는 방식마다 pci_epc_set_msi() / pci_epc_set_msix() 를 부른다.
 * MSI-X 쪽은 표가 놓인 BAR 번호와 오프셋까지 함께 넘겨야 한다 —
 * PCI 스펙상 MSI-X 표의 위치가 능력 구조체에 기록되기 때문이다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 에러 경로: 실패하면 호출자가 앞서 건 BAR 두 개를 되돌린다.
 *
 * 호출 체인:
 *   epf_ntb_epc_init_interface() → [epf_ntb_configure_interrupt]
 *     → pci_epc_set_msi() / pci_epc_set_msix()
 */
static int epf_ntb_configure_interrupt(struct epf_ntb *ntb,
				       enum pci_epc_interface_type type)
{
	/* [한국어] 컨트롤러 능력표. MSI/MSI-X 지원 여부를 여기서 읽는다. */
	const struct pci_epc_features *epc_features;
	/* [한국어] 각각 MSI-X 지원, MSI 지원 여부. */
	bool msix_capable, msi_capable;
	/* [한국어] 설정할 인터페이스의 상태. */
	struct epf_ntb_epc *ntb_epc;
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] 설정 대상 컨트롤러. */
	struct pci_epc *epc;
	/* [한국어] 로그용. */
	struct device *dev;
	/* [한국어] 설정할 인터럽트 개수 = 도어벨 개수. */
	u32 db_count;
	/* [한국어] 실패 코드. */
	int ret;

	/* [한국어] 이 인터페이스의 상태. */
	ntb_epc = ntb->epc[type];
	/* [한국어] 로그는 EPF 디바이스 기준으로 남긴다. */
	dev = &ntb->epf->dev;

	/* [한국어] 이 컨트롤러의 능력표. */
	epc_features = ntb_epc->epc_features;
	/* [한국어] MSI-X 를 지원하는가. 지원하면 벡터마다 주소가 달라 도어벨을
	 * 하나씩 구분해 매핑할 수 있다. */
	msix_capable = epc_features->msix_capable;
	/* [한국어] MSI 를 지원하는가. MSI 는 주소가 하나이고 데이터의 하위 비트로
	 * 벡터를 구분한다. */
	msi_capable = epc_features->msi_capable;

	/* [한국어] 둘 다 없으면 도어벨을 만들 수 없다 — 도어벨은 결국 상대 호스트에게
	 * 인터럽트를 쏘는 일이기 때문이다. */
	if (!(msix_capable || msi_capable)) {
		/* [한국어] 설정 오류이므로 dev_err 로 분명히 남긴다. */
		dev_err(dev, "MSI or MSI-X is required for doorbell\n");
		/* [한국어] 기능이 없는 컨트롤러이므로 -EINVAL 로 거절한다. */
		return -EINVAL;
	}

	/* [한국어] EPC 콜백에 넘길 함수 번호. */
	func_no = ntb_epc->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb_epc->vfunc_no;

	/* [한국어] 사용자가 configfs 로 정한 도어벨 개수. */
	db_count = ntb->db_count;
	/* [한국어] db_data[]/db_offset[] 배열 크기가 MAX_DB_COUNT(32)라 그 이상은
	 * 공유 레지스터 배열 밖을 건드리게 된다. */
	if (db_count > MAX_DB_COUNT) {
		/* [한국어] 허용 한계를 사용자에게 알린다. */
		dev_err(dev, "DB count cannot be more than %d\n", MAX_DB_COUNT);
		return -EINVAL;
	}

	/* [한국어] 검사를 통과한 값을 되쓴다(값 자체는 그대로다). 상류 코드 그대로 둔다. */
	ntb->db_count = db_count;
	/* [한국어] 설정 대상 컨트롤러. */
	epc = ntb_epc->epc;

	/* [한국어] MSI 를 지원하면 MSI 능력 구조체의 개수 필드를 db_count 로 맞춘다.
	 * 호스트가 열거할 때 이 개수를 보고 벡터를 할당한다. */
	if (msi_capable) {
		/* [한국어] 컨트롤러 레지스터에 실제로 쓴다. epc->lock 아래에서 실행된다. */
		ret = pci_epc_set_msi(epc, func_no, vfunc_no, db_count);
		/* [한국어] 실패하면 도어벨이 없으므로 초기화 전체를 접는다. */
		if (ret) {
			/* [한국어] 어느 인터페이스인지 남긴다. */
			dev_err(dev, "%s intf: MSI configuration failed\n",
				pci_epc_interface_string(type));
			return ret;
		}
	}

	/* [한국어] MSI-X 를 지원하면 MSI-X 표의 위치까지 함께 알려야 한다. */
	if (msix_capable) {
		/* [한국어] 벡터 개수와 함께 MSI-X 표가 놓인 BAR 번호와 그 안에서의 오프셋을 넘긴다.
		 * 이 두 값은 epf_ntb_config_spad_bar_alloc() 에서 config 영역 뒤에
		 * 표 자리를 만들면서 기록해 둔 것이다. */
		ret = pci_epc_set_msix(epc, func_no, vfunc_no, db_count,
				       ntb_epc->msix_bar,
				       ntb_epc->msix_table_offset);
		/* [한국어] 실패하면 초기화를 접는다. */
		if (ret) {
			/* [한국어] 상류 메시지가 MSI 라고 적혀 있으나 실제로는 MSI-X 설정이다.
			 * 상류 코드 그대로 둔다. */
			dev_err(dev, "MSI configuration failed\n");
			return ret;
		}
	}

	return 0;
}

/**
 * epf_ntb_alloc_peer_mem() - Allocate memory in peer's outbound address space
 * @dev: The PCI device.
 * @ntb_epc: EPC associated with one of the HOST whose BAR holds peer's outbound
 *   address
 * @bar: BAR of @ntb_epc in for which memory has to be allocated (could be
 *   BAR_DB_MW1, BAR_MW2, BAR_MW3, BAR_MW4)
 * @peer_ntb_epc: EPC associated with HOST whose outbound address space is
 *   used by @ntb_epc
 * @size: Size of the address region that has to be allocated in peers OB SPACE
 *
 *
 * +-----------------+    +---->+----------------+-----------+-----------------+
 * |       BAR0      |    |     |   Doorbell 1   +-----------> MSI|X ADDRESS 1 |
 * +-----------------+    |     +----------------+           +-----------------+
 * |       BAR1      |    |     |   Doorbell 2   +---------+ |                 |
 * +-----------------+----+     +----------------+         | |                 |
 * |       BAR2      |          |   Doorbell 3   +-------+ | +-----------------+
 * +-----------------+----+     +----------------+       | +-> MSI|X ADDRESS 2 |
 * |       BAR3      |    |     |   Doorbell 4   +-----+ |   +-----------------+
 * +-----------------+    |     |----------------+     | |   |                 |
 * |       BAR4      |    |     |                |     | |   +-----------------+
 * +-----------------+    |     |      MW1       +---+ | +-->+ MSI|X ADDRESS 3||
 * |       BAR5      |    |     |                |   | |     +-----------------+
 * +-----------------+    +---->-----------------+   | |     |                 |
 *   EP CONTROLLER 1            |                |   | |     +-----------------+
 *                              |                |   | +---->+ MSI|X ADDRESS 4 |
 *                              +----------------+   |       +-----------------+
 *                      (A)      EP CONTROLLER 2     |       |                 |
 *                                 (OB SPACE)        |       |                 |
 *                                                   +------->      MW1        |
 *                                                           |                 |
 *                                                           |                 |
 *                                                   (B)     +-----------------+
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           |                 |
 *                                                           +-----------------+
 *                                                           PCI Address Space
 *                                                           (Managed by HOST2)
 *
 * Allocate memory in OB space of EP CONTROLLER 2 in the above diagram. Allocate
 * for Doorbell 1, Doorbell 2, Doorbell 3, Doorbell 4, MW1 (and MW2, MW3, MW4).
 */
/* [한국어]
 * epf_ntb_alloc_peer_mem - 상대편 컨트롤러의 아웃바운드 공간에서 창 하나를 잡는다
 *
 * @dev: 로그용 device(EPF 디바이스)
 * @ntb_epc: 이 창을 BAR 로 노출할 인터페이스
 * @bar: 그 논리 BAR 인덱스(BAR_DB_MW1 ~ BAR_MW4)
 * @peer_ntb_epc: 아웃바운드 공간을 빌려 줄 인터페이스
 * @size: 필요한 크기
 * @return: 0 성공, -ENOMEM 이면 아웃바운드 공간 부족
 *
 * 왜 필요한가: 이 드라이버의 데이터 경로는 "이쪽 BAR → 상대편 아웃바운드
 * 창 → 상대 호스트" 로 흐른다. 그 가운데 토막인 아웃바운드 창을
 * 여기서 확보하고, 그 물리 주소를 이쪽 BAR 서술자에 채운다.
 *
 * 동작 단계:
 *   (1) 크기를 다듬는다 — 최소 128바이트를 보장하고, 컨트롤러 정렬 요구가
 *       있으면 그 배수로, 없으면 2의 거듭제곱으로 올린다. PCI BAR 크기가
 *       언제나 2의 거듭제곱이어야 한다는 제약 때문이다.
 *   (2) pci_epc_mem_alloc_addr() 로 상대편 아웃바운드 공간에서 잡는다.
 *   (3) 얻은 물리 주소·크기·BAR 번호·32비트 메모리 플래그를 서술자에 채운다.
 *       가상 주소는 해제 때 필요하므로 mw_addr 에 보관한다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트).
 *
 * 에러 경로: 공간이 없으면 -ENOMEM. 호출자가 지금까지 만든 창을 모두 되돌린다.
 *
 * 호출 체인:
 *   epf_ntb_db_mw_bar_init() → [epf_ntb_alloc_peer_mem]
 *     → pci_epc_mem_alloc_addr()
 */
static int epf_ntb_alloc_peer_mem(struct device *dev,
				  struct epf_ntb_epc *ntb_epc,
				  enum epf_ntb_bar bar,
				  struct epf_ntb_epc *peer_ntb_epc,
				  size_t size)
{
	/* [한국어] 이쪽 인터페이스의 컨트롤러 능력표. 정렬 요구를 읽는다. */
	const struct pci_epc_features *epc_features;
	/* [한국어] 채워 넣을 BAR 서술자. */
	struct pci_epf_bar *epf_bar;
	/* [한국어] 공간을 빌려 줄 상대편 컨트롤러. */
	struct pci_epc *peer_epc;
	/* [한국어] 할당된 아웃바운드 공간의 물리 주소(CPU 관점). */
	phys_addr_t phys_addr;
	/* [한국어] 같은 공간의 커널 가상 주소. __iomem 이 붙어 있어 readl/writel 계열로만
	 * 접근해야 한다. */
	void __iomem *mw_addr;
	/* [한국어] 실제 BAR 번호. */
	enum pci_barno barno;
	/* [한국어] 컨트롤러가 요구하는 주소 정렬. */
	size_t align;

	/* [한국어] BAR 를 걸 쪽의 능력표를 본다 — 정렬 요구는 BAR 를 노출하는
	 * 컨트롤러 쪽 제약이기 때문이다. */
	epc_features = ntb_epc->epc_features;
	/* [한국어] 정렬 요구값. */
	align = epc_features->align;

	/* [한국어] 최소 128바이트를 보장한다. 너무 작은 창은 컨트롤러가 매핑하지 못하거나
	 * BAR 크기 규칙(최소값)에 걸릴 수 있어서다. */
	if (size < 128)
		size = 128;

	/* [한국어] 정렬 요구가 있으면 그 배수로 올린다. */
	if (align)
		/* [한국어] ALIGN() 은 align 이 2의 거듭제곱임을 전제한다. */
		size = ALIGN(size, align);
	else
		/* [한국어] 정렬 요구가 없으면 2의 거듭제곱으로 올린다.
		 * PCI BAR 크기는 언제나 2의 거듭제곱이어야 하기 때문이다. */
		size = roundup_pow_of_two(size);

	/* [한국어] 실제로 공간을 잡을 컨트롤러는 상대편이다 — 이쪽 BAR 로 들어온
	 * 트랜잭션이 상대편 아웃바운드 창을 거쳐 저쪽 호스트로 나가야 하므로. */
	peer_epc = peer_ntb_epc->epc;
	/* [한국어] 상대 컨트롤러의 아웃바운드 주소 공간에서 size 만큼 잡는다.
	 * 반환값은 매핑된 가상 주소, phys_addr 에는 물리 주소가 채워진다. */
	mw_addr = pci_epc_mem_alloc_addr(peer_epc, &phys_addr, size);
	/* [한국어] 공간 부족은 흔한 실패다. */
	if (!mw_addr) {
		/* [한국어] 어느 쪽 인터페이스의 공간이 모자랐는지 남긴다. */
		dev_err(dev, "%s intf: Failed to allocate OB address\n",
			pci_epc_interface_string(peer_ntb_epc->type));
		return -ENOMEM;
	}

	/* [한국어] 이 논리 구성요소에 배정된 실제 BAR 번호. */
	barno = ntb_epc->epf_ntb_bar[bar];
	/* [한국어] 그 BAR 의 서술자. */
	epf_bar = &ntb_epc->epf_bar[barno];
	/* [한국어] 해제할 때 필요하므로 가상 주소를 BAR 번호로 색인해 보관한다. */
	ntb_epc->mw_addr[barno] = mw_addr;

	/* [한국어] BAR 뒤에 놓일 물리 주소. 호스트가 이 BAR 에 쓰면 이 주소로 간다. */
	epf_bar->phys_addr = phys_addr;
	/* [한국어] 정렬까지 반영된 최종 크기. */
	epf_bar->size = size;
	/* [한국어] 서술자에 BAR 번호를 채운다. pci_epc_set_bar() 가 이 필드로 대상을 고른다. */
	epf_bar->barno = barno;
	/* [한국어] 32비트 메모리 BAR 로 선언한다. 이 드라이버는 아웃바운드 창에
	 * 64비트 BAR 를 쓰지 않는다. */
	epf_bar->flags = PCI_BASE_ADDRESS_MEM_TYPE_32;

	return 0;
}

/**
 * epf_ntb_db_mw_bar_init() - Configure Doorbell and Memory window BARs
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @type: PRIMARY interface or SECONDARY interface
 *
 * Wrapper for epf_ntb_alloc_peer_mem() and pci_epc_set_bar() that allocates
 * memory in OB address space of HOST2 and configures BAR of HOST1
 */
/* [한국어]
 * epf_ntb_db_mw_bar_init - 도어벨과 메모리 윈도우를 위한 창을 잡고 BAR 로 노출한다
 *
 * @ntb: 함수 전체 상태
 * @type: BAR 를 걸 인터페이스
 * @return: 0 성공, 그 밖에는 첫 실패의 오류 코드
 *
 * 왜 필요한가: 도어벨과 창은 이 드라이버의 두 가지 통신 수단이다.
 * 첫 BAR 는 두 가지를 함께 담는다 — 앞쪽이 도어벨 칸들, 그 뒤가 MW1 이다.
 * 이렇게 합치는 이유는 BAR 개수가 여섯 개뿐이라 아끼기 위해서다.
 *
 * 동작 단계: 창 개수만큼 돌면서
 *   (1) 첫 BAR 면 도어벨 영역 크기를 계산하고 그 뒤에 창을 붙인다.
 *       도어벨 시작에서 창까지의 거리를 ctrl->mw1_offset 에 기록해
 *       호스트와 configure_mw() 가 같은 지점을 가리키게 한다.
 *   (2) 상대편 아웃바운드 공간에서 그 크기만큼 잡는다.
 *   (3) 그 물리 주소로 이쪽 BAR 를 건다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 에러 경로: 어느 창에서 실패하든 epf_ntb_db_mw_bar_cleanup() 으로
 * 지금까지 만든 것을 모두 되돌린 뒤 오류를 올린다.
 *
 * 호출 체인:
 *   epf_ntb_epc_init_interface() → [epf_ntb_db_mw_bar_init]
 *     → epf_ntb_alloc_peer_mem() → pci_epc_set_bar()
 */
static int epf_ntb_db_mw_bar_init(struct epf_ntb *ntb,
				  enum pci_epc_interface_type type)
{
	/* [한국어] 컨트롤러 능력표. 정렬 요구를 여기서 읽는다. */
	const struct pci_epc_features *epc_features;
	/* [한국어] 이쪽 인터페이스와 상대 인터페이스의 상태. */
	struct epf_ntb_epc *peer_ntb_epc, *ntb_epc;
	/* [한국어] BAR 서술자. 물리 주소와 크기를 채워 EPC 에 넘긴다. */
	struct pci_epf_bar *epf_bar;
	/* [한국어] 공유 레지스터 영역. mw1_offset 을 여기에 기록한다. */
	struct epf_ntb_ctrl *ctrl;
	/* [한국어] 만들 창 개수와 도어벨 개수. */
	u32 num_mws, db_count;
	/* [한국어] 논리 BAR 인덱스(BAR_DB_MW1 부터). */
	enum epf_ntb_bar bar;
	/* [한국어] 실제 BAR 번호. */
	enum pci_barno barno;
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] 이쪽 인터페이스의 컨트롤러. */
	struct pci_epc *epc;
	/* [한국어] 로그용. */
	struct device *dev;
	/* [한국어] 컨트롤러가 요구하는 주소 정렬. 0 이면 정렬 제약이 없다는 뜻이다. */
	size_t align;
	/* [한국어] 실패 코드와 창 순회 인덱스. */
	int ret, i;
	/* [한국어] 각 BAR 에 걸 창 크기. */
	u64 size;

	/* [한국어] 이쪽(BAR 를 걸 쪽) 인터페이스. */
	ntb_epc = ntb->epc[type];
	/* [한국어] 상대편(아웃바운드 공간을 빌려 줄 쪽) 인터페이스. */
	peer_ntb_epc = ntb->epc[!type];

	/* [한국어] 로그 대상. */
	dev = &ntb->epf->dev;
	/* [한국어] 이쪽 컨트롤러의 능력표. */
	epc_features = ntb_epc->epc_features;
	/* [한국어] 정렬 요구값을 꺼내 둔다. */
	align = epc_features->align;
	/* [한국어] 이쪽 함수 번호. */
	func_no = ntb_epc->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb_epc->vfunc_no;
	/* [한국어] BAR 를 걸 컨트롤러. */
	epc = ntb_epc->epc;
	/* [한국어] 만들 메모리 윈도우 개수. */
	num_mws = ntb->num_mws;
	/* [한국어] 도어벨 개수. 첫 BAR 안에 도어벨 영역을 함께 두므로 크기 계산에 필요하다. */
	db_count = ntb->db_count;

	/* [한국어] BAR_DB_MW1 부터 창 개수만큼 BAR 를 순회한다.
	 * bar 는 논리 인덱스, i 는 창 번호다. */
	for (bar = BAR_DB_MW1, i = 0; i < num_mws; bar++, i++) {
		/* [한국어] 첫 BAR 는 도어벨과 MW1 을 함께 담는 특별한 BAR 다. */
		if (bar == BAR_DB_MW1) {
			/* [한국어] 정렬 제약이 없으면 도어벨 한 칸을 4바이트로 잡는다.
			 * 도어벨 하나가 결국 상대 아웃바운드 공간의 한 지점이므로,
			 * 컨트롤러가 요구하는 최소 매핑 단위(align)를 따라야 한다. */
			align = align ? align : 4;
			/* [한국어] 도어벨 영역 전체 크기 = 도어벨 개수 x 한 칸 크기. */
			size = db_count * align;
			/* [한국어] MW1 이 시작될 지점을 창 크기에 맞춰 올림한다. 그래야 창의
			 * 시작 주소가 창 크기 경계에 놓인다. */
			size = ALIGN(size, ntb->mws_size[i]);
			/* [한국어] 공유 레지스터 영역을 가리킨다. */
			ctrl = ntb_epc->reg;
			/* [한국어] 호스트에게 "이 BAR 안에서 MW1 은 이만큼 뒤에서 시작한다" 고 알린다.
			 * epf_ntb_configure_mw() 도 이 값을 더해 실제 매핑 주소를 잡는다. */
			ctrl->mw1_offset = size;
			/* [한국어] 도어벨 영역 뒤에 창 크기를 더한 것이 이 BAR 전체 크기다. */
			size += ntb->mws_size[i];
		} else {
			/* [한국어] 나머지 BAR 는 창 하나만 담으므로 크기가 곧 창 크기다. */
			size = ntb->mws_size[i];
		}

		/* [한국어] 상대편 컨트롤러의 아웃바운드 공간에서 이 크기만큼 잡고,
		 * 그 물리 주소를 이쪽 BAR 서술자에 채운다. */
		ret = epf_ntb_alloc_peer_mem(dev, ntb_epc, bar,
					     peer_ntb_epc, size);
		/* [한국어] 공간을 못 얻으면 지금까지 만든 BAR 를 모두 되돌린다. */
		if (ret) {
			/* [한국어] 어느 인터페이스에서 났는지 남긴다. */
			dev_err(dev, "%s intf: DoorBell mem alloc failed\n",
				pci_epc_interface_string(type));
			goto err_alloc_peer_mem;
		}

		/* [한국어] 방금 채워진 실제 BAR 번호. */
		barno = ntb_epc->epf_ntb_bar[bar];
		/* [한국어] 그 BAR 의 서술자. 위 호출이 phys_addr/size/flags 를 이미 채웠다. */
		epf_bar = &ntb_epc->epf_bar[barno];

		/* [한국어] BAR 를 실제로 건다. 이 호출 안에서 크기가 2의 거듭제곱인지,
		 * BAR5 에 64비트 플래그를 붙이지는 않았는지 등이 검증되고,
		 * epc->lock 뮤텍스 아래에서 컨트롤러 콜백이 불린다. */
		ret = pci_epc_set_bar(epc, func_no, vfunc_no, epf_bar);
		/* [한국어] BAR 설정 실패도 같은 정리 경로로 간다. */
		if (ret) {
			/* [한국어] 어느 인터페이스인지 남긴다. */
			dev_err(dev, "%s intf: DoorBell BAR set failed\n",
				pci_epc_interface_string(type));
			goto err_alloc_peer_mem;
		}
	}

	return 0;

/* [한국어] 실패 경로. 이 인터페이스의 BAR 를 모두 걷고 상대 아웃바운드
 * 메모리를 되돌린다. 부분 성공 상태를 남기지 않는 것이 중요하다. */
err_alloc_peer_mem:
	epf_ntb_db_mw_bar_cleanup(ntb, type);

	return ret;
}

/**
 * epf_ntb_epc_create_interface() - Create and initialize NTB EPC interface
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @epc: struct pci_epc to which a particular NTB interface should be associated
 * @type: PRIMARY interface or SECONDARY interface
 *
 * Allocate memory for NTB EPC interface and initialize it.
 */
/* [한국어]
 * epf_ntb_epc_create_interface - 인터페이스 하나의 상태 구조체를 만들고 EPC 정보를 채운다
 *
 * @ntb: 함수 전체 상태
 * @epc: 이 인터페이스가 붙을 엔드포인트 컨트롤러
 * @type: PRIMARY 또는 SECONDARY
 * @return: 0 성공, -ENOMEM 할당 실패, -EINVAL 이면 컨트롤러 능력표를 못 얻음
 *
 * 왜 필요한가: 하나의 EPF 가 두 EPC 에 붙는 이 드라이버 구조에서,
 * "어느 컨트롤러의 몇 번 함수인가", "어느 BAR 배열을 쓰는가" 가
 * 인터페이스마다 다르다. 그 차이를 여기서 한 번 흡수해 두면
 * 이후 모든 함수가 type 인덱스만으로 대칭적으로 동작할 수 있다.
 *
 * 동작 단계: 상태를 kzalloc 으로 잡고(0 초기화가 정리 경로의 안전을
 * 보장한다), PRIMARY 면 epf->func_no/epf->bar 를, SECONDARY 면
 * epf->sec_epc_func_no/epf->sec_epc_bar 를 복사한 뒤,
 * pci_epc_get_features() 로 능력표를 읽어 둔다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트).
 * devm_kzalloc 이므로 해제는 EPF 디바이스 소멸 시 자동이다.
 *
 * 에러 경로: 어느 실패든 호출자가 그대로 올린다. 이미 잡힌 devm_ 메모리는
 * 따로 되돌리지 않아도 된다.
 *
 * 호출 체인:
 *   epf_ntb_bind() → epf_ntb_epc_create() → [epf_ntb_epc_create_interface]
 *     → pci_epc_get_features()
 */
static int epf_ntb_epc_create_interface(struct epf_ntb *ntb,
					struct pci_epc *epc,
					enum pci_epc_interface_type type)
{
	/* [한국어] EPC 능력표를 담을 포인터. */
	const struct pci_epc_features *epc_features;
	/* [한국어] 이 인터페이스가 쓸 pci_epf_bar 배열의 시작. PRIMARY 와 SECONDARY 가
	 * 서로 다른 배열을 쓴다. */
	struct pci_epf_bar *epf_bar;
	/* [한국어] 새로 만들 인터페이스 상태. */
	struct epf_ntb_epc *ntb_epc;
	/* [한국어] EPC 호출에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] EPF 디바이스. */
	struct pci_epf *epf;
	/* [한국어] devm_ 할당과 로그에 쓸 device 포인터. */
	struct device *dev;

	/* [한국어] EPF 디바이스에 수명을 매단다. 이 디바이스가 사라지면 아래
	 * devm_kzalloc 메모리도 자동으로 풀린다. */
	dev = &ntb->epf->dev;

	/* [한국어] 인터페이스 상태를 0 으로 채워 잡는다. linkup=false, mw_addr 전부 NULL
	 * 로 시작한다는 뜻이라 정리 경로가 안전해진다. */
	ntb_epc = devm_kzalloc(dev, sizeof(*ntb_epc), GFP_KERNEL);
	/* [한국어] 할당 실패는 그대로 올린다. */
	if (!ntb_epc)
		return -ENOMEM;

	/* [한국어] 두 인터페이스가 공유하는 EPF 디바이스. */
	epf = ntb->epf;
	/* [한국어] 가상 함수 번호는 두 인터페이스가 같다. */
	vfunc_no = epf->vfunc_no;
	/* [한국어] 주 인터페이스인가? */
	if (type == PRIMARY_INTERFACE) {
		/* [한국어] 주 인터페이스의 함수 번호는 epf->func_no 다. */
		func_no = epf->func_no;
		/* [한국어] 주 인터페이스의 BAR 배열도 epf->bar 다. */
		epf_bar = epf->bar;
	} else {
		/* [한국어] 보조 인터페이스는 함수 번호와 BAR 배열이 따로 있다.
		 * pci_epc_add_epf(epc, epf, SECONDARY_INTERFACE) 가 이 값들을 채운다. */
		func_no = epf->sec_epc_func_no;
		/* [한국어] 보조 인터페이스 전용 BAR 배열. */
		epf_bar = epf->sec_epc_bar;
	}

	/* [한국어] 링크는 아직 내려가 있다. 두 호스트가 모두 COMMAND_LINK_UP 을 보내야
	 * 실제 링크 이벤트가 올라간다. */
	ntb_epc->linkup = false;
	/* [한국어] 이 인터페이스가 설정할 컨트롤러. */
	ntb_epc->epc = epc;
	/* [한국어] EPC 콜백에 넘길 함수 번호. */
	ntb_epc->func_no = func_no;
	/* [한국어] 가상 함수 번호. */
	ntb_epc->vfunc_no = vfunc_no;
	/* [한국어] 자기가 PRIMARY 인지 SECONDARY 인지 기억한다. cmd_handler 가
	 * container_of 로 이 구조체에 도달한 뒤 이 필드로 방향을 판단한다. */
	ntb_epc->type = type;
	/* [한국어] BAR 배열 시작 주소를 기억한다. */
	ntb_epc->epf_bar = epf_bar;
	/* [한국어] 역방향 포인터. 하위 함수가 상대 인터페이스(ntb->epc[!type])에 닿는 통로다. */
	ntb_epc->epf_ntb = ntb;

	/* [한국어] 컨트롤러 능력을 읽어 둔다. BAR 예약 여부, 정렬 요구, MSI/MSI-X 지원
	 * 여부가 모두 여기서 나온다. */
	epc_features = pci_epc_get_features(epc, func_no, vfunc_no);
	/* [한국어] 능력표를 못 얻으면 이후 모든 판단의 근거가 없어지므로 실패한다. */
	if (!epc_features)
		return -EINVAL;
	/* [한국어] 이후 BAR 선택과 정렬 계산이 이 표를 참조한다. */
	ntb_epc->epc_features = epc_features;

	/* [한국어] 완성된 인터페이스 상태를 함수 전체 상태의 배열에 꽂는다.
	 * 이 배열 인덱스가 곧 PRIMARY/SECONDARY 다. */
	ntb->epc[type] = ntb_epc;

	return 0;
}

/**
 * epf_ntb_epc_create() - Create and initialize NTB EPC interface
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 *
 * Get a reference to EPC device and bind NTB function device to that EPC
 * for each of the interface. It is also a wrapper to
 * epf_ntb_epc_create_interface() to allocate memory for NTB EPC interface
 * and initialize it
 */
/* [한국어]
 * epf_ntb_epc_create - PRIMARY 와 SECONDARY 두 인터페이스 상태를 만든다
 *
 * @ntb: 함수 전체 상태
 * @return: 0 성공, 그 밖에는 실패한 인터페이스의 오류 코드
 *
 * 왜 필요한가: 이 드라이버는 EPC 두 개가 모두 있어야 성립한다.
 * epf->epc 는 첫 링크에서, epf->sec_epc 는 secondary 링크에서 채워지며,
 * 이 함수가 불리는 시점에는 epf_ntb_bind() 가 이미 둘 다 있음을 확인했다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트).
 *
 * 에러 경로: 두 번째가 실패해도 첫 번째를 되돌리지 않는다 — devm_ 메모리라
 * EPF 디바이스가 사라질 때 함께 풀리기 때문이다.
 *
 * 호출 체인:
 *   epf_ntb_bind() → [epf_ntb_epc_create] → epf_ntb_epc_create_interface()
 */
static int epf_ntb_epc_create(struct epf_ntb *ntb)
{
	/* [한국어] 두 인터페이스가 공유하는 EPF 디바이스. */
	struct pci_epf *epf;
	/* [한국어] 로그용. */
	struct device *dev;
	/* [한국어] 하위 호출 실패를 담는다. */
	int ret;

	/* [한국어] EPF 디바이스를 꺼낸다. */
	epf = ntb->epf;
	/* [한국어] 로그 대상. */
	dev = &epf->dev;

	/* [한국어] 주 인터페이스 생성. epf->epc 는 첫 번째 링크 때 채워진 컨트롤러다. */
	ret = epf_ntb_epc_create_interface(ntb, epf->epc, PRIMARY_INTERFACE);
	/* [한국어] 실패하면 여기서 끝낸다. */
	if (ret) {
		/* [한국어] 어느 쪽인지 명시해 로그를 남긴다. */
		dev_err(dev, "PRIMARY intf: Fail to create NTB EPC\n");
		return ret;
	}

	/* [한국어] 보조 인터페이스 생성. epf->sec_epc 는 secondary 링크 때 채워진다. */
	ret = epf_ntb_epc_create_interface(ntb, epf->sec_epc,
					   SECONDARY_INTERFACE);
	/* [한국어] 실패해도 주 인터페이스를 되돌리지 않는다 — devm_ 메모리라
	 * EPF 디바이스가 사라질 때 함께 풀리기 때문이다. */
	if (ret)
		/* [한국어] 어느 쪽인지 남긴다. */
		dev_err(dev, "SECONDARY intf: Fail to create NTB EPC\n");

	/* [한국어] 성공이든 실패든 두 번째 호출의 결과를 그대로 올린다. */
	return ret;
}

/**
 * epf_ntb_init_epc_bar_interface() - Identify BARs to be used for each of
 *   the NTB constructs (scratchpad region, doorbell, memorywindow)
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @type: PRIMARY interface or SECONDARY interface
 *
 * Identify the free BARs to be used for each of BAR_CONFIG, BAR_PEER_SPAD,
 * BAR_DB_MW1, BAR_MW2, BAR_MW3 and BAR_MW4.
 */
/* [한국어]
 * epf_ntb_init_epc_bar_interface - 한 인터페이스에서 각 NTB 구성요소가 쓸 BAR 번호를 고른다
 *
 * @ntb: 함수 전체 상태
 * @type: BAR 를 고를 인터페이스
 * @return: 0 성공, 필수 BAR 가 모자라면 음수(NO_BAR 값이 그대로 올라간다)
 *
 * 왜 필요한가: 컨트롤러마다 쓸 수 있는 BAR 가 다르다. 어떤 BAR 는 예약되어
 * 있고, 어떤 BAR 는 64비트 전용이라 두 칸을 먹는다. 그래서 BAR 번호를
 * 코드에 박아 넣을 수 없고, 능력표를 보며 빈 자리를 찾아야 한다.
 *
 * 동작 단계:
 *   (1) 필수 세 칸(BAR_CONFIG, BAR_PEER_SPAD, BAR_DB_MW1)을 먼저 배정한다.
 *       하나라도 없으면 NTB 가 성립하지 않으므로 실패한다.
 *   (2) 선택 칸(MW2~MW4)을 배정한다. 모자라면 num_mws 를 줄여 조용히
 *       기능을 축소하고 계속 진행한다.
 *
 * pci_epc_get_next_free_bar() 가 예약/비활성 BAR 를 건너뛰고,
 * 직전 BAR 가 64비트 전용이면 한 칸 더 건너뛴다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트).
 *
 * 에러 경로: 필수 BAR 부족은 곧 초기화 실패다.
 *
 * 호출 체인:
 *   epf_ntb_bind() → epf_ntb_init_epc_bar() → [epf_ntb_init_epc_bar_interface]
 *     → pci_epc_get_next_free_bar()
 */
static int epf_ntb_init_epc_bar_interface(struct epf_ntb *ntb,
					  enum pci_epc_interface_type type)
{
	/* [한국어] 이 인터페이스가 붙은 EPC 가 알려 주는 능력표. 어떤 BAR 가 예약되어
	 * 있고 어떤 BAR 가 64비트 전용인지가 여기 들어 있다. */
	const struct pci_epc_features *epc_features;
	/* [한국어] BAR 번호를 저장할 이 인터페이스의 상태. */
	struct epf_ntb_epc *ntb_epc;
	/* [한국어] 지금까지 배정한 BAR 번호. 다음 탐색의 시작점이 된다. */
	enum pci_barno barno;
	/* [한국어] NTB 구성요소를 가리키는 논리 인덱스(BAR_CONFIG, BAR_PEER_SPAD, ...). */
	enum epf_ntb_bar bar;
	/* [한국어] 로그용 device 포인터. */
	struct device *dev;
	/* [한국어] 메모리 윈도우 개수. 선택 BAR 를 몇 개까지 시도할지 결정한다. */
	u32 num_mws;
	/* [한국어] 선택 BAR 루프에서 실제로 배정된 창 개수를 세는 변수. */
	int i;

	/* [한국어] BAR0 부터 훑기 시작한다. */
	barno = BAR_0;
	/* [한국어] 이 인터페이스의 상태를 고른다. */
	ntb_epc = ntb->epc[type];
	/* [한국어] 사용자가 configfs 로 정한 창 개수. */
	num_mws = ntb->num_mws;
	/* [한국어] 로그 대상. */
	dev = &ntb->epf->dev;
	/* [한국어] 컨트롤러 능력표. pci_epc_get_next_free_bar() 가 이것을 보고
	 * 예약/비활성 BAR 를 건너뛴다. */
	epc_features = ntb_epc->epc_features;

	/* These are required BARs which are mandatory for NTB functionality */
	for (bar = BAR_CONFIG; bar <= BAR_DB_MW1; bar++, barno++) {
		/* [한국어] 필수 BAR 세 개: BAR_CONFIG, BAR_PEER_SPAD, BAR_DB_MW1.
		 * 비어 있는 다음 BAR 번호를 얻는다. 직전 BAR 가 64비트 전용이면
		 * 이 함수가 알아서 한 칸 더 건너뛴다(64비트 BAR 는 두 칸을 먹는다). */
		barno = pci_epc_get_next_free_bar(epc_features, barno);
		/* [한국어] NO_BAR(음수)가 오면 남은 BAR 가 없다는 뜻이다. 필수 구성요소이므로
		 * 여기서 실패해야 한다. */
		if (barno < 0) {
			/* [한국어] 어느 인터페이스에서 모자랐는지 남긴다. */
			dev_err(dev, "%s intf: Fail to get NTB function BAR\n",
				pci_epc_interface_string(type));
			/* [한국어] 음수인 barno 를 그대로 오류 코드로 올린다. */
			return barno;
		}
		/* [한국어] 이 논리 구성요소가 쓸 실제 BAR 번호를 기록한다.
		 * 이후 모든 코드가 epf_ntb_bar[논리인덱스] 로 실제 BAR 를 찾는다. */
		ntb_epc->epf_ntb_bar[bar] = barno;
	}

	/* These are optional BARs which don't impact NTB functionality */
	for (bar = BAR_MW2, i = 1; i < num_mws; bar++, barno++, i++) {
		/* [한국어] 선택 BAR: 두 번째 이후의 메모리 윈도우. 없어도 NTB 는 동작한다. */
		barno = pci_epc_get_next_free_bar(epc_features, barno);
		/* [한국어] 더 이상 BAR 가 없으면 창 개수를 지금까지 성공한 만큼으로 낮춘다. */
		if (barno < 0) {
			/* [한국어] num_mws 를 줄이면 이후 단계가 그만큼만 창을 만든다. */
			ntb->num_mws = i;
			/* [한국어] 오류가 아니므로 dev_dbg 로만 남긴다. */
			dev_dbg(dev, "BAR not available for > MW%d\n", i + 1);
		}
		/* [한국어] 실패했더라도 배정 결과를 그대로 기록한다 — 음수가 들어갈 수 있으나
		 * 위에서 num_mws 를 줄였으므로 그 항목은 더 이상 쓰이지 않는다. */
		ntb_epc->epf_ntb_bar[bar] = barno;
	}

	return 0;
}

/**
 * epf_ntb_init_epc_bar() - Identify BARs to be used for each of the NTB
 * constructs (scratchpad region, doorbell, memorywindow)
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 *
 * Wrapper to epf_ntb_init_epc_bar_interface() to identify the free BARs
 * to be used for each of BAR_CONFIG, BAR_PEER_SPAD, BAR_DB_MW1, BAR_MW2,
 * BAR_MW3 and BAR_MW4 for all the interfaces.
 */
/* [한국어]
 * epf_ntb_init_epc_bar - 두 인터페이스 각각에 대해 BAR 배정을 반복한다
 *
 * @ntb: 함수 전체 상태
 * @return: 0 성공, 그 밖에는 첫 실패의 오류 코드
 *
 * 왜 필요한가: 두 컨트롤러의 능력이 서로 다를 수 있으므로, 같은 논리
 * 구성요소가 인터페이스마다 다른 BAR 번호에 배정될 수 있다. 그래서
 * 배정을 인터페이스별로 독립적으로 수행한다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_bind() → [epf_ntb_init_epc_bar] → epf_ntb_init_epc_bar_interface()
 */
static int epf_ntb_init_epc_bar(struct epf_ntb *ntb)
{
	/* [한국어] 두 인터페이스를 순회할 반복자. */
	enum pci_epc_interface_type type;
	/* [한국어] 로그용. */
	struct device *dev;
	/* [한국어] 하위 호출 실패를 담는다. */
	int ret;

	/* [한국어] EPF 디바이스 기준으로 로그를 남긴다. */
	dev = &ntb->epf->dev;
	/* [한국어] PRIMARY, SECONDARY 각각 독립적으로 BAR 를 고른다.
	 * 두 EPC 의 능력이 다를 수 있으므로 같은 논리 구성요소가 서로 다른
	 * BAR 번호에 배정될 수 있다. */
	for (type = PRIMARY_INTERFACE; type <= SECONDARY_INTERFACE; type++) {
		/* [한국어] 인터페이스 하나의 BAR 배정. */
		ret = epf_ntb_init_epc_bar_interface(ntb, type);
		/* [한국어] 하나라도 실패하면 전체 실패다. */
		if (ret) {
			/* [한국어] 어느 인터페이스인지 남긴다. */
			dev_err(dev, "Fail to init EPC bar for %s interface\n",
				pci_epc_interface_string(type));
			return ret;
		}
	}

	return 0;
}

/**
 * epf_ntb_epc_init_interface() - Initialize NTB interface
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @type: PRIMARY interface or SECONDARY interface
 *
 * Wrapper to initialize a particular EPC interface and start the workqueue
 * to check for commands from host. This function will write to the
 * EP controller HW for configuring it.
 */
/* [한국어]
 * epf_ntb_epc_init_interface - 한 인터페이스를 실제 하드웨어에 설정하고 폴링 워크를 띄운다
 *
 * @ntb: 함수 전체 상태
 * @type: 초기화할 인터페이스
 * @return: 0 성공, 그 밖에는 실패한 단계의 오류 코드
 *
 * 왜 필요한가: 여기까지는 메모리 계산과 번호 배정뿐이었고,
 * 이 함수부터가 컨트롤러 레지스터를 실제로 건드리는 지점이다.
 *
 * 동작 단계(순서가 중요하다):
 *   (1) config + 자기 스크래치패드 BAR 를 건다 — 호스트가 명령을 쓸 통로.
 *   (2) peer 스크래치패드 BAR 를 건다 — 상대편 메모리를 들여다보는 창.
 *   (3) MSI/MSI-X 능력을 설정한다 — 호스트가 열거할 때 벡터를 받도록.
 *   (4) 도어벨/창 BAR 를 만든다.
 *   (5) config 헤더를 쓴다(vfunc_no 가 0 이나 1 일 때만).
 *   (6) 명령 폴링 워크를 큐에 넣는다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트). 여러 EPC 호출에서
 * epc->lock 을 잡았다 놓는다.
 *
 * 에러 경로: 단계마다 레이블이 있어, 실패한 지점까지 거꾸로 되돌린다.
 * err_write_header → err_db_mw_bar_init → err_peer_spad_bar_init 순으로
 * 흘러 내리며 정확히 성공한 단계만 취소한다.
 *
 * 호출 체인:
 *   epf_ntb_bind() → epf_ntb_epc_init() → [epf_ntb_epc_init_interface]
 *     → epf_ntb_config_sspad_bar_set() → epf_ntb_peer_spad_bar_set()
 *     → epf_ntb_configure_interrupt() → epf_ntb_db_mw_bar_init()
 *     → pci_epc_write_header() → queue_work()
 */
static int epf_ntb_epc_init_interface(struct epf_ntb *ntb,
				      enum pci_epc_interface_type type)
{
	/* [한국어] 이 인터페이스의 상태. epf_ntb_epc_create_interface() 가 만들어 둔 것. */
	struct epf_ntb_epc *ntb_epc;
	/* [한국어] EPC 호출마다 넘겨야 하는 물리 함수 번호와 가상 함수 번호. */
	u8 func_no, vfunc_no;
	/* [한국어] 이 인터페이스가 붙어 있는 엔드포인트 컨트롤러. */
	struct pci_epc *epc;
	/* [한국어] EPF 디바이스. config 헤더를 쓸 때 epf->header 가 필요하다. */
	struct pci_epf *epf;
	/* [한국어] 에러 로그용 device 포인터. */
	struct device *dev;
	/* [한국어] 각 단계의 실패를 담을 변수. */
	int ret;

	/* [한국어] type(PRIMARY/SECONDARY)으로 이 인터페이스의 상태를 고른다. */
	ntb_epc = ntb->epc[type];
	/* [한국어] EPF 디바이스는 두 인터페이스가 공유한다 — 하나의 EPF 가 두 EPC 에 붙는
	 * 구조이기 때문이다. */
	epf = ntb->epf;
	/* [한국어] dev_err 대상. EPF 디바이스 기준으로 찍어 두 인터페이스 로그가
	 * 같은 디바이스 이름 아래 모이게 한다. */
	dev = &epf->dev;
	/* [한국어] 이 인터페이스가 실제로 설정할 하드웨어 컨트롤러. */
	epc = ntb_epc->epc;
	/* [한국어] EPC 콜백에 넘길 함수 번호. PRIMARY 와 SECONDARY 가 서로 다르다. */
	func_no = ntb_epc->func_no;
	/* [한국어] SR-IOV 가상 함수 번호. 0 이면 물리 함수 자신이다. */
	vfunc_no = ntb_epc->vfunc_no;

	/* [한국어] 1단계: config + 자기 스크래치패드 영역을 BAR 로 노출한다.
	 * 이미 epf_ntb_config_spad_bar_alloc() 이 메모리를 잡아 두었으므로
	 * 여기서는 인바운드 ATU 를 거는 일만 한다. */
	ret = epf_ntb_config_sspad_bar_set(ntb->epc[type]);
	/* [한국어] 실패하면 아직 되돌릴 것이 없으므로 그냥 반환한다. */
	if (ret) {
		/* [한국어] 어느 인터페이스에서 났는지 문자열로 남긴다. */
		dev_err(dev, "%s intf: Config/self SPAD BAR init failed\n",
			pci_epc_interface_string(type));
		return ret;
	}

	/* [한국어] 2단계: 상대편의 자기 스크래치패드를 이쪽 호스트의 "peer 스크래치패드"
	 * BAR 로 노출한다. 두 호스트가 같은 물리 메모리를 서로 다른 BAR 로
	 * 보게 되는 것이 NTB 스크래치패드의 핵심이다. */
	ret = epf_ntb_peer_spad_bar_set(ntb, type);
	/* [한국어] 여기서 실패하면 1단계에서 건 BAR 를 걷어야 한다. */
	if (ret) {
		/* [한국어] 실패한 인터페이스를 로그에 남긴다. */
		dev_err(dev, "%s intf: Peer SPAD BAR init failed\n",
			pci_epc_interface_string(type));
		goto err_peer_spad_bar_init;
	}

	/* [한국어] 3단계: MSI/MSI-X 능력을 db_count 개만큼 설정한다.
	 * 호스트가 이 개수만큼 벡터를 할당해 주어야 도어벨이 동작한다. */
	ret = epf_ntb_configure_interrupt(ntb, type);
	/* [한국어] 실패하면 앞의 두 BAR 를 되돌린다. */
	if (ret) {
		/* [한국어] 어느 인터페이스인지 남긴다. */
		dev_err(dev, "%s intf: Interrupt configuration failed\n",
			pci_epc_interface_string(type));
		goto err_peer_spad_bar_init;
	}

	/* [한국어] 4단계: 도어벨 + 메모리 윈도우 BAR 를 만든다. 상대 EPC 의 아웃바운드
	 * 공간에 창을 잡고, 그 물리 주소를 이쪽 BAR 뒤에 건다. */
	ret = epf_ntb_db_mw_bar_init(ntb, type);
	/* [한국어] 실패하면 3단계까지 되돌린다. */
	if (ret) {
		/* [한국어] 어느 인터페이스인지 남긴다. */
		dev_err(dev, "%s intf: DB/MW BAR init failed\n",
			pci_epc_interface_string(type));
		goto err_db_mw_bar_init;
	}

	/* [한국어] vfunc_no 가 0 이나 1 일 때만 config 헤더를 쓴다.
	 * 가상 함수(SR-IOV VF)는 헤더를 물리 함수와 공유하는 경우가 많아
	 * 2 이상에서는 쓰지 않는다. */
	if (vfunc_no <= 1) {
		/* [한국어] 벤더/디바이스 ID, 클래스 코드 등을 컨트롤러 레지스터에 써서
		 * 호스트가 열거할 때 보게 될 모습을 만든다. */
		ret = pci_epc_write_header(epc, func_no, vfunc_no, epf->header);
		/* [한국어] 헤더 쓰기 실패는 호스트가 이 함수를 알아보지 못한다는 뜻이라 치명적이다. */
		if (ret) {
			/* [한국어] 실패한 인터페이스를 남긴다. */
			dev_err(dev, "%s intf: Configuration header write failed\n",
				pci_epc_interface_string(type));
			goto err_write_header;
		}
	}

	/* [한국어] 5단계: 명령 폴링 워크를 초기화한다. 이 워크가 호스트가 CONFIG 영역에
	 * 써 넣는 command 필드를 5ms 마다 읽어 처리한다. */
	INIT_DELAYED_WORK(&ntb->epc[type]->cmd_handler, epf_ntb_cmd_handler);
	/* [한국어] 첫 실행을 지연 없이 큐에 넣는다. cmd_handler 가 스스로
	 * queue_delayed_work 로 5ms 뒤 재예약하며 계속 돈다. */
	queue_work(kpcintb_workqueue, &ntb->epc[type]->cmd_handler.work);

	return 0;

err_write_header:
	epf_ntb_db_mw_bar_cleanup(ntb, type);

err_db_mw_bar_init:
	epf_ntb_peer_spad_bar_clear(ntb->epc[type]);

err_peer_spad_bar_init:
	epf_ntb_config_sspad_bar_clear(ntb->epc[type]);

	return ret;
}

/**
 * epf_ntb_epc_cleanup_interface() - Cleanup NTB interface
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 * @type: PRIMARY interface or SECONDARY interface
 *
 * Wrapper to cleanup a particular NTB interface.
 */
/* [한국어]
 * epf_ntb_epc_cleanup_interface - 한 인터페이스의 설정을 초기화의 역순으로 되돌린다
 *
 * @ntb: 함수 전체 상태
 * @type: 정리할 인터페이스. 음수면 아무 것도 하지 않는다.
 * @return: 없음
 *
 * 왜 필요한가: epf_ntb_epc_init() 이 첫 인터페이스에서 실패하면
 * type - 1 이 -1 로 넘어온다. 그 경우를 걸러 내려고 음수 검사가 있다.
 *
 * 동작 단계: 폴링 워크를 취소하고 → 도어벨/창 BAR 와 아웃바운드 메모리를
 * 정리하고 → peer 스크래치패드 BAR 를 걷고 → config BAR 를 걷는다.
 *
 * 주의: cancel_delayed_work() 는 이미 실행 중인 워크가 끝나기를 기다리지
 * 않는다. 즉 이 시점에 cmd_handler 가 돌고 있을 수 있다. 상류 코드 그대로 둔다.
 *
 * 실행 컨텍스트: bind 실패 경로와 unbind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_epc_cleanup() 또는 epf_ntb_epc_init() 실패 경로
 *     → [epf_ntb_epc_cleanup_interface]
 */
static void epf_ntb_epc_cleanup_interface(struct epf_ntb *ntb,
					  enum pci_epc_interface_type type)
{
	/* [한국어] 정리 대상 인터페이스의 상태. */
	struct epf_ntb_epc *ntb_epc;

	/* [한국어] type 이 음수면 초기화된 인터페이스가 하나도 없다는 뜻이다.
	 * epf_ntb_epc_init() 이 첫 인터페이스에서 실패하면 type - 1 이
	 * -1 로 넘어오므로 이 검사가 필요하다. */
	if (type < 0)
		return;

	/* [한국어] 정리할 인터페이스 상태를 고른다. */
	ntb_epc = ntb->epc[type];
	/* [한국어] 폴링 워크를 취소한다. cancel_delayed_work() 는 이미 실행 중인
	 * 워크가 끝나기를 기다리지 않는다 — 즉 이 시점에 cmd_handler 가
	 * 돌고 있을 수 있다. 상류 코드 그대로 둔다. */
	cancel_delayed_work(&ntb_epc->cmd_handler);
	/* [한국어] 도어벨/메모리 윈도우 BAR 를 걷고 상대 아웃바운드 메모리를 해제한다. */
	epf_ntb_db_mw_bar_cleanup(ntb, type);
	/* [한국어] peer 스크래치패드 BAR 를 걷는다. */
	epf_ntb_peer_spad_bar_clear(ntb_epc);
	/* [한국어] config + 자기 스크래치패드 BAR 를 걷는다. 역순 정리다. */
	epf_ntb_config_sspad_bar_clear(ntb_epc);
}

/**
 * epf_ntb_epc_cleanup() - Cleanup all NTB interfaces
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 *
 * Wrapper to cleanup all NTB interfaces.
 */
/* [한국어]
 * epf_ntb_epc_cleanup - 두 인터페이스를 모두 정리한다
 *
 * @ntb: 함수 전체 상태
 * @return: 없음
 *
 * 왜 필요한가: unbind 경로에서 두 인터페이스를 한 번에 걷어 내기 위한 얇은 껍데기다.
 *
 * 실행 컨텍스트: unbind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_unbind() → [epf_ntb_epc_cleanup] → epf_ntb_epc_cleanup_interface()
 */
static void epf_ntb_epc_cleanup(struct epf_ntb *ntb)
{
	/* [한국어] 두 인터페이스를 순회할 반복자. */
	enum pci_epc_interface_type type;

	/* [한국어] PRIMARY 부터 SECONDARY 까지. 두 값이 연속된 enum 이라 ++ 로 순회된다. */
	for (type = PRIMARY_INTERFACE; type <= SECONDARY_INTERFACE; type++)
		/* [한국어] 인터페이스 하나씩 정리한다. 실패해도 계속 진행해야 하므로 반환값이 없다. */
		epf_ntb_epc_cleanup_interface(ntb, type);
}

/**
 * epf_ntb_epc_init() - Initialize all NTB interfaces
 * @ntb: NTB device that facilitates communication between HOST1 and HOST2
 *
 * Wrapper to initialize all NTB interface and start the workqueue
 * to check for commands from host.
 */
/* [한국어]
 * epf_ntb_epc_init - 두 인터페이스를 차례로 초기화한다
 *
 * @ntb: 함수 전체 상태
 * @return: 0 성공, 그 밖에는 실패한 인터페이스의 오류 코드
 *
 * 왜 필요한가: 두 호스트 모두에게 같은 모습을 보여야 하므로 두 인터페이스가
 * 모두 초기화되어야 한다.
 *
 * 에러 경로: 두 번째에서 실패하면 첫 번째를 되돌린다. 첫 번째에서 실패하면
 * type - 1 이 -1 이 되어 cleanup 이 아무 것도 하지 않는다 — 이것이
 * epf_ntb_epc_cleanup_interface() 의 음수 검사가 존재하는 이유다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_bind() → [epf_ntb_epc_init] → epf_ntb_epc_init_interface()
 */
static int epf_ntb_epc_init(struct epf_ntb *ntb)
{
	/* [한국어] 두 인터페이스를 순회할 반복자. 실패 시 어디까지 갔는지도 이 값이 알려 준다. */
	enum pci_epc_interface_type type;
	/* [한국어] 에러 로그용. */
	struct device *dev;
	/* [한국어] 각 인터페이스 초기화의 실패를 담는다. */
	int ret;

	/* [한국어] EPF 디바이스 기준으로 로그를 남긴다. */
	dev = &ntb->epf->dev;

	/* [한국어] PRIMARY → SECONDARY 순서로 초기화한다. */
	for (type = PRIMARY_INTERFACE; type <= SECONDARY_INTERFACE; type++) {
		/* [한국어] 인터페이스 하나를 통째로 설정한다(BAR, 인터럽트, 워크까지). */
		ret = epf_ntb_epc_init_interface(ntb, type);
		/* [한국어] 하나라도 실패하면 앞서 성공한 인터페이스를 되돌려야 한다. */
		if (ret) {
			/* [한국어] 실패한 인터페이스 이름을 로그에 남긴다. */
			dev_err(dev, "%s intf: Failed to initialize\n",
				pci_epc_interface_string(type));
			goto err_init_type;
		}
	}

	return 0;

/* [한국어] 여기 오면 type 번째에서 실패한 것이므로, 그 앞(type - 1) 인터페이스만
 * 정리하면 된다. type 이 PRIMARY(0)였다면 -1 이 넘어가고
 * epf_ntb_epc_cleanup_interface() 의 음수 검사가 그것을 걸러 낸다. */
err_init_type:
	epf_ntb_epc_cleanup_interface(ntb, type - 1);

	return ret;
}

/**
 * epf_ntb_bind() - Initialize endpoint controller to provide NTB functionality
 * @epf: NTB endpoint function device
 *
 * Initialize both the endpoint controllers associated with NTB function device.
 * Invoked when a primary interface or secondary interface is bound to EPC
 * device. This function will succeed only when EPC is bound to both the
 * interfaces.
 */
/* [한국어]
 * epf_ntb_bind - EPC 링크가 걸릴 때 불리는 진입점. 두 EPC 가 다 붙어야 실제 초기화를 한다
 *
 * @epf: NTB 엔드포인트 함수 디바이스
 * @return: 0 성공(아직 한쪽만 붙은 경우도 0), 그 밖에는 초기화 실패 코드
 *
 * 왜 필요한가: configfs 에서 사용자가 EPF 를 EPC 에 링크할 때마다
 * EPF 코어가 이 콜백을 부른다. 이 드라이버는 EPC 두 개가 필요하므로,
 * 첫 번째 링크에서는 아무 것도 하지 않고 물러났다가 두 번째 링크에서
 * 전체 초기화를 수행한다. 그래서 앞의 두 검사가 이 파일의 구조를 결정한다.
 *
 * 동작 단계:
 *   (1) epf->epc 와 epf->sec_epc 가 모두 있는지 확인한다.
 *   (2) 두 인터페이스 상태를 만든다(epf_ntb_epc_create).
 *   (3) BAR 번호를 배정한다(epf_ntb_init_epc_bar).
 *   (4) config + 스크래치패드 메모리를 잡는다.
 *   (5) 하드웨어를 설정하고 폴링 워크를 띄운다(epf_ntb_epc_init).
 *
 * 실행 컨텍스트: configfs 의 심볼릭 링크 생성(사용자 ln) 문맥,
 * 프로세스 컨텍스트. 안쪽에서 GFP_KERNEL 할당과 뮤텍스를 쓴다.
 *
 * 에러 경로: (4) 이후의 실패는 err_bar_alloc 로 가서 메모리를 되돌린다.
 * (2)(3) 의 실패는 되돌릴 것이 없다.
 *
 * 호출 체인:
 *   configfs(pci_epc_epf_link 또는 pci_secondary_epc_epf_link)
 *     → pci_epf_bind() → [epf_ntb_bind]
 *     → epf_ntb_epc_create() → epf_ntb_init_epc_bar()
 *     → epf_ntb_config_spad_bar_alloc_interface() → epf_ntb_epc_init()
 */
static int epf_ntb_bind(struct pci_epf *epf)
{
	/* [한국어] EPF 디바이스에 매달아 둔 함수 상태를 꺼낸다. */
	struct epf_ntb *ntb = epf_get_drvdata(epf);
	/* [한국어] 로그용 device 포인터. */
	struct device *dev = &epf->dev;
	/* [한국어] 각 단계 실패를 담는다. */
	int ret;

	/* [한국어] 주 인터페이스가 아직 EPC 에 링크되지 않았다. */
	if (!epf->epc) {
		/* [한국어] 오류가 아니라 "아직" 이므로 dev_dbg 로만 남기고 성공을 돌려준다.
		 * bind 는 두 EPC 링크 각각에 대해 불리므로, 두 번째 링크 때 실제
		 * 초기화가 이뤄진다. */
		dev_dbg(dev, "PRIMARY EPC interface not yet bound\n");
		return 0;
	}

	/* [한국어] 보조 인터페이스가 아직 링크되지 않았다. */
	if (!epf->sec_epc) {
		/* [한국어] 마찬가지로 성공을 돌려주고 물러난다. 이 두 검사가 "EPC 두 개가
		 * 모두 붙어야 NTB 가 성립한다" 는 이 드라이버의 전제를 구현한다. */
		dev_dbg(dev, "SECONDARY EPC interface not yet bound\n");
		return 0;
	}

	/* [한국어] 1단계: 두 인터페이스의 상태 구조체를 만들고 EPC 능력을 읽어 둔다. */
	ret = epf_ntb_epc_create(ntb);
	/* [한국어] 여기서 실패하면 devm_ 메모리 말고는 되돌릴 것이 없다. */
	if (ret) {
		/* [한국어] 상류 메시지 그대로. 실제로는 EPC 인터페이스 생성 실패다. */
		dev_err(dev, "Failed to create NTB EPC\n");
		return ret;
	}

	/* [한국어] 2단계: 각 NTB 구성요소(config, peer spad, 도어벨+MW)에 쓸 BAR 번호를
	 * 고른다. 컨트롤러가 예약해 둔 BAR 는 건너뛴다. */
	ret = epf_ntb_init_epc_bar(ntb);
	/* [한국어] BAR 가 모자라면 여기서 실패한다. */
	if (ret) {
		/* [한국어] 상류 메시지 그대로. */
		dev_err(dev, "Failed to create NTB EPC\n");
		return ret;
	}

	/* [한국어] 3단계: config + 스크래치패드 영역의 실제 메모리를 잡는다.
	 * 안쪽에서 dma_alloc_coherent 를 쓰므로 이 시점부터 호스트가 읽고 쓸 수 있는
	 * 일관성 있는 메모리가 생긴다. */
	ret = epf_ntb_config_spad_bar_alloc_interface(ntb);
	/* [한국어] 할당 실패는 되돌릴 것이 생긴 첫 지점이다. */
	if (ret) {
		/* [한국어] 어느 인터페이스인지는 안쪽에서 이미 찍었다. */
		dev_err(dev, "Failed to allocate BAR memory\n");
		goto err_bar_alloc;
	}

	/* [한국어] 4단계: BAR 를 실제로 걸고 인터럽트를 설정하고 폴링 워크를 띄운다. */
	ret = epf_ntb_epc_init(ntb);
	/* [한국어] 실패하면 3단계에서 잡은 메모리를 되돌린다. */
	if (ret) {
		/* [한국어] 안쪽에서 인터페이스별 원인을 이미 찍었다. */
		dev_err(dev, "Failed to initialize EPC\n");
		goto err_bar_alloc;
	}

	/* [한국어] 모든 초기화가 끝난 뒤 다시 한 번 매단다. probe 에서 이미 했으므로
	 * 중복이지만 상류 코드 그대로 둔다. */
	epf_set_drvdata(epf, ntb);

	return 0;

/* [한국어] 3단계 이후의 실패 경로. config + 스크래치패드 메모리를 되돌린다.
 * 2단계(BAR 번호 선택)와 1단계(devm_ 할당)는 되돌릴 것이 없다. */
err_bar_alloc:
	epf_ntb_config_spad_bar_free(ntb);

	return ret;
}

/**
 * epf_ntb_unbind() - Cleanup the initialization from epf_ntb_bind()
 * @epf: NTB endpoint function device
 *
 * Cleanup the initialization from epf_ntb_bind()
 */
/* [한국어]
 * epf_ntb_unbind - bind 가 만든 모든 것을 역순으로 되돌린다
 *
 * @epf: NTB 엔드포인트 함수 디바이스
 * @return: 없음
 *
 * 왜 필요한가: 사용자가 configfs 링크를 지우거나 모듈이 내려갈 때
 * 하드웨어 설정과 메모리를 모두 되돌려야 한다.
 *
 * 동작 단계: 먼저 BAR 를 걷고 워크를 멈춘 뒤(epf_ntb_epc_cleanup),
 * 그 다음에 메모리를 해제한다. 순서를 뒤집으면 아직 호스트에게 노출된
 * 메모리를 해제하게 된다.
 *
 * 실행 컨텍스트: configfs 링크 해제 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   configfs → pci_epf_unbind() → [epf_ntb_unbind]
 *     → epf_ntb_epc_cleanup() → epf_ntb_config_spad_bar_free()
 */
static void epf_ntb_unbind(struct pci_epf *epf)
{
	/* [한국어] unbind 는 EPF 코어가 링크 해제 때 부른다. 매달아 둔 상태를 꺼내
	 * 초기화의 역순으로 정리한다. */
	struct epf_ntb *ntb = epf_get_drvdata(epf);

	/* [한국어] 먼저 두 인터페이스의 BAR 를 걷고 워크를 멈춘다. */
	epf_ntb_epc_cleanup(ntb);
	/* [한국어] 그 다음 config + 스크래치패드 메모리를 해제한다.
	 * 순서가 뒤바뀌면 아직 BAR 로 노출된 메모리를 해제하게 된다. */
	epf_ntb_config_spad_bar_free(ntb);
}

/* [한국어] 아래 네 매크로는 configfs 속성의 show/store 함수를 찍어내는 틀이다.
 * 실제 함수 본문을 손으로 열 번 넘게 반복하지 않으려고 매크로로 묶었다.
 * 매크로 안에서는 줄 끝의 역슬래시가 줄을 잇고 있으므로, 이 블록 안에는
 * 주석 줄을 끼워 넣을 수 없다 — 그래서 설명을 모두 여기 바깥에 둔다.
 * 
 * EPF_NTB_R(_name): epf_ntb_<_name>_show() 를 만든다.
 *   config_item 을 config_group 으로, 다시 to_epf_ntb() 로 struct epf_ntb 로
 *   되돌린 뒤 ntb-><_name> 필드를 십진수로 찍는다.
 *   sysfs_emit() 은 PAGE_SIZE 경계를 스스로 지켜 주는 안전한 출력 도우미다. */
#define EPF_NTB_R(_name)						\
static ssize_t epf_ntb_##_name##_show(struct config_item *item,		\
				      char *page)			\
{									\
	struct config_group *group = to_config_group(item);		\
	struct epf_ntb *ntb = to_epf_ntb(group);			\
									\
	return sysfs_emit(page, "%d\n", ntb->_name);			\
}

/* [한국어] EPF_NTB_W(_name): epf_ntb_<_name>_store() 를 만든다.
 *   kstrtou32(page, 0, &val) 로 사용자가 쓴 문자열을 32비트 부호 없는
 *   정수로 바꾼다. base 0 이라 0x 접두사도 해석한다. 실패하면 -EINVAL.
 *   검증 없이 ntb-><_name> 에 그대로 대입하므로, 범위 검사가 필요한
 *   num_mws 만 따로 손으로 구현되어 있다(epf_ntb_num_mws_store).
 *   반환값 len 은 "입력을 전부 소비했다" 는 configfs 규약이다. */
#define EPF_NTB_W(_name)						\
static ssize_t epf_ntb_##_name##_store(struct config_item *item,	\
				       const char *page, size_t len)	\
{									\
	struct config_group *group = to_config_group(item);		\
	struct epf_ntb *ntb = to_epf_ntb(group);			\
	u32 val;							\
									\
	if (kstrtou32(page, 0, &val) < 0)				\
		return -EINVAL;						\
									\
	ntb->_name = val;						\
									\
	return len;							\
}

/* [한국어] EPF_NTB_MW_R(_name): 메모리 윈도우 크기 읽기 함수를 만든다.
 *   #_name 은 매크로 인자를 문자열로 바꾸는 전처리 연산자라
 *   EPF_NTB_MW_R(mw3) 이면 "mw3" 이 된다. 그것을 sscanf 로 파싱해
 *   win_no 를 3 으로 얻고, mws_size[win_no - 1] 을 찍는다.
 *   즉 이름 자체를 배열 인덱스로 되돌리는 기법이다.
 *   주의: sscanf 반환값을 검사하지 않아 win_no 가 초기화되지 않은 채
 *   쓰일 여지가 있다(store 쪽은 검사한다). 상류 코드 그대로 둔다. */
#define EPF_NTB_MW_R(_name)						\
static ssize_t epf_ntb_##_name##_show(struct config_item *item,		\
				      char *page)			\
{									\
	struct config_group *group = to_config_group(item);		\
	struct epf_ntb *ntb = to_epf_ntb(group);			\
	int win_no;							\
									\
	sscanf(#_name, "mw%d", &win_no);				\
									\
	return sysfs_emit(page, "%lld\n", ntb->mws_size[win_no - 1]);	\
}

/* [한국어] EPF_NTB_MW_W(_name): 메모리 윈도우 크기 쓰기 함수를 만든다.
 *   크기는 64비트라 kstrtou64 를 쓴다.
 *   sscanf 반환값을 검사해 이름 파싱이 실패하면 -EINVAL.
 *   ntb->num_mws 보다 큰 창 번호는 거부한다 — 예를 들어 num_mws 가 2 인데
 *   mw3 에 쓰면 쓰이지 않을 값이 들어가므로 사용자에게 오류로 알린다.
 *   다만 이 검사는 mws_size[] 배열 범위(MAX_MW=4)와는 별개다. */
#define EPF_NTB_MW_W(_name)						\
static ssize_t epf_ntb_##_name##_store(struct config_item *item,	\
				       const char *page, size_t len)	\
{									\
	struct config_group *group = to_config_group(item);		\
	struct epf_ntb *ntb = to_epf_ntb(group);			\
	struct device *dev = &ntb->epf->dev;				\
	int win_no;							\
	u64 val;							\
									\
	if (kstrtou64(page, 0, &val) < 0)				\
		return -EINVAL;						\
									\
	if (sscanf(#_name, "mw%d", &win_no) != 1)			\
		return -EINVAL;						\
									\
	if (ntb->num_mws < win_no) {					\
		dev_err(dev, "Invalid num_nws: %d value\n", ntb->num_mws); \
		return -EINVAL;						\
	}								\
									\
	ntb->mws_size[win_no - 1] = val;				\
									\
	return len;							\
}

/* [한국어]
 * epf_ntb_num_mws_store - configfs 의 num_mws 파일에 쓴 값을 받는다
 *
 * @item: 쓰기가 일어난 configfs 항목
 * @page: 사용자가 쓴 바이트열(널 종결)
 * @len: 그 길이
 * @return: 성공하면 len, 파싱 실패나 범위 초과면 -EINVAL
 *
 * 왜 필요한가: 다른 정수 속성은 EPF_NTB_W 매크로가 찍어 주지만,
 * num_mws 는 MAX_MW 상한 검사가 필요해서 손으로 구현되어 있다.
 * 이 검사가 없으면 mws_size[] 배열 밖을 건드리게 된다.
 *
 * 실행 컨텍스트: 사용자의 write(2) 문맥(프로세스 컨텍스트).
 * configfs 내부 락 아래에서 불린다.
 *
 * 호출 체인:
 *   사용자 echo → configfs → [epf_ntb_num_mws_store]
 */
static ssize_t epf_ntb_num_mws_store(struct config_item *item,
				     const char *page, size_t len)
{
	/* [한국어] configfs 는 파일 하나를 config_item 으로 준다. 그 item 이 속한
	 * config_group 을 먼저 얻어야 우리 epf_ntb 구조체로 되돌릴 수 있다. */
	struct config_group *group = to_config_group(item);
	/* [한국어] to_epf_ntb() 는 container_of 매크로다. group 필드가 struct epf_ntb
	 * 안에 박혀 있으므로 그 오프셋만큼 빼면 함수 전체 상태에 닿는다. */
	struct epf_ntb *ntb = to_epf_ntb(group);
	/* [한국어] 사용자가 써 넣은 십진/십육진 문자열을 담을 임시 변수. */
	u32 val;

	/* [한국어] kstrtou32(page, 0, &val): base 0 이라 "4" 도 "0x4" 도 받는다.
	 * 음수나 쓰레기 문자열이면 음수를 돌려주므로 -EINVAL 로 거절한다. */
	if (kstrtou32(page, 0, &val) < 0)
		return -EINVAL;

	/* [한국어] 메모리 윈도우 개수 상한 검사. MAX_MW(4)를 넘으면 mws_size[] 배열
	 * 밖을 건드리게 되므로 여기서 반드시 막아야 한다. */
	if (val > MAX_MW)
		return -EINVAL;

	/* [한국어] 검증을 통과한 값만 함수 상태에 반영한다. 이 값은 나중에
	 * epf_ntb_init_epc_bar_interface() 가 BAR 를 몇 개 잡을지 결정할 때 쓰인다. */
	ntb->num_mws = val;

	/* [한국어] configfs 규약상 store 는 소비한 바이트 수를 돌려줘야 한다.
	 * 전부 소비했다는 뜻으로 len 을 그대로 돌려준다. */
	return len;
}

/* [한국어] 아래 EPF_NTB_R/W 매크로 전개들. 각각 epf_ntb_<이름>_show(),
 * epf_ntb_<이름>_store() 함수를 실제로 만들어 낸다.
 * spad_count 는 스크래치패드 레지스터 개수(32비트 워드 단위),
 * db_count 는 도어벨 개수, num_mws 는 메모리 윈도우 개수다.
 * num_mws 는 상한 검사가 필요해 store 만 위에서 손으로 구현했다. */
EPF_NTB_R(spad_count)
EPF_NTB_W(spad_count)
EPF_NTB_R(db_count)
EPF_NTB_W(db_count)
EPF_NTB_R(num_mws)
/* [한국어] mw1~mw4 는 각 메모리 윈도우의 크기(바이트)다. EPF_NTB_MW_R/W 가
 * 이름 문자열을 sscanf 로 파싱해 mws_size[] 인덱스를 뽑아낸다. */
EPF_NTB_MW_R(mw1)
EPF_NTB_MW_W(mw1)
EPF_NTB_MW_R(mw2)
EPF_NTB_MW_W(mw2)
EPF_NTB_MW_R(mw3)
EPF_NTB_MW_W(mw3)
EPF_NTB_MW_R(mw4)
EPF_NTB_MW_W(mw4)

/* [한국어] CONFIGFS_ATTR(접두사, 이름): 위에서 만든 show/store 한 쌍을 묶어
 * configfs_attribute 구조체 epf_ntb_attr_<이름> 을 정의한다.
 * 읽기/쓰기 모두 되므로 파일 권한은 0644 가 된다. */
CONFIGFS_ATTR(epf_ntb_, spad_count);
CONFIGFS_ATTR(epf_ntb_, db_count);
CONFIGFS_ATTR(epf_ntb_, num_mws);
/* [한국어] mw1~mw4 속성. 각 메모리 윈도우의 크기를 바이트 단위로 읽고 쓴다.
 * 값은 ntb->mws_size[] 에 들어가며, bind 시점에 창 BAR 크기를 정한다. */
CONFIGFS_ATTR(epf_ntb_, mw1);
CONFIGFS_ATTR(epf_ntb_, mw2);
CONFIGFS_ATTR(epf_ntb_, mw3);
CONFIGFS_ATTR(epf_ntb_, mw4);

/* [한국어] configfs 디렉터리에 만들어질 속성 파일 목록. NULL 로 끝나는 배열이며
 * configfs 가 순회하면서 파일을 하나씩 만든다. */
static struct configfs_attribute *epf_ntb_attrs[] = {
	/* [한국어] spad_count: 스크래치패드 워드 개수. epf_ntb_config_spad_bar_alloc() 이
	 * 이 값 x 4 바이트만큼 영역을 잡는다. */
	&epf_ntb_attr_spad_count,
	/* [한국어] db_count: 도어벨(=MSI/MSI-X 벡터) 개수. MAX_DB_COUNT(32) 이하여야 한다. */
	&epf_ntb_attr_db_count,
	/* [한국어] num_mws: 메모리 윈도우 개수. BAR 를 몇 개 소비할지 결정한다. */
	&epf_ntb_attr_num_mws,
	/* [한국어] mw1: 첫 번째 메모리 윈도우 크기. BAR_DB_MW1 BAR 안에서 도어벨 뒤쪽에 놓인다. */
	&epf_ntb_attr_mw1,
	/* [한국어] mw2: 두 번째 메모리 윈도우 크기. 전용 BAR 하나를 쓴다. */
	&epf_ntb_attr_mw2,
	/* [한국어] mw3: 세 번째 메모리 윈도우 크기. */
	&epf_ntb_attr_mw3,
	/* [한국어] mw4: 네 번째 메모리 윈도우 크기. BAR 가 모자라면 조용히 줄어든다. */
	&epf_ntb_attr_mw4,
	/* [한국어] NULL 종결자. 이것이 없으면 configfs 가 배열 밖을 읽는다. */
	NULL,
};

/* [한국어] 이 config_group 의 타입 서술자. epf_ntb_add_cfs() 가
 * config_group_init_type_name() 에 이 타입을 넘겨 디렉터리를 만든다. */
static const struct config_item_type ntb_group_type = {
	/* [한국어] ct_attrs: 위에서 만든 속성 목록을 연결한다. */
	.ct_attrs	= epf_ntb_attrs,
	/* [한국어] ct_owner: 이 디렉터리가 살아 있는 동안 모듈이 언로드되지 않도록
	 * 모듈 참조를 건다. THIS_MODULE 이 pci_epf_ntb 모듈 자신이다. */
	.ct_owner	= THIS_MODULE,
};

/**
 * epf_ntb_add_cfs() - Add configfs directory specific to NTB
 * @epf: NTB endpoint function device
 * @group: A pointer to the config_group structure referencing a group of
 *	   config_items of a specific type that belong to a specific sub-system.
 *
 * Add configfs directory specific to NTB. This directory will hold
 * NTB specific properties like db_count, spad_count, num_mws etc.,
 */
/* [한국어]
 * epf_ntb_add_cfs - 이 함수 전용 configfs 디렉터리를 만들어 준다
 *
 * @epf: NTB 엔드포인트 함수 디바이스
 * @group: 부모 config_group(EPF 코어가 넘겨준다)
 * @return: 이 함수의 속성들을 담은 config_group 포인터
 *
 * 왜 필요한가: db_count, spad_count, num_mws, mw1~mw4 같은 NTB 고유
 * 설정은 EPF 공통 속성이 아니다. EPF 코어가 이 콜백을 불러 함수별
 * 하위 디렉터리를 만들 기회를 준다.
 *
 * 동작 단계: struct epf_ntb 안에 박아 둔 config_group 을 EPF 디바이스
 * 이름으로 초기화하고 ntb_group_type 을 붙인다. 그 순간 속성 파일들이 생긴다.
 * 따로 할당하지 않으므로 해제 책임도 없다.
 *
 * 실행 컨텍스트: configfs 디렉터리 생성(사용자 mkdir) 문맥.
 *
 * 호출 체인:
 *   사용자 mkdir → configfs → pci-ep-cfs.c → [epf_ntb_add_cfs]
 *     → config_group_init_type_name()
 */
static struct config_group *epf_ntb_add_cfs(struct pci_epf *epf,
					    struct config_group *group)
{
	/* [한국어] EPF 디바이스에 붙여 둔 드라이버 전용 데이터를 꺼낸다.
	 * epf_ntb_probe() 에서 epf_set_drvdata() 로 심어 둔 그 포인터다. */
	struct epf_ntb *ntb = epf_get_drvdata(epf);
	/* [한국어] struct epf_ntb 안에 박아 둔 config_group 을 그대로 쓴다.
	 * 따로 할당하지 않으므로 해제 책임도 없다. */
	struct config_group *ntb_group = &ntb->group;
	/* [한국어] 디렉터리 이름으로 쓸 EPF 디바이스 이름을 얻기 위한 device 포인터. */
	struct device *dev = &epf->dev;

	/* [한국어] 그룹을 초기화하면서 이름을 EPF 디바이스 이름(예: pci_epf_ntb.0)으로
	 * 정하고 위의 ntb_group_type 을 붙인다. 이 시점에 속성 파일들이 생긴다. */
	config_group_init_type_name(ntb_group, dev_name(dev), &ntb_group_type);

	/* [한국어] 호출자(pci-ep-cfs.c 의 pci_epf_cfs_add_type_group 경로)가 이 그룹을
	 * 부모 디렉터리 밑에 등록한다. */
	return ntb_group;
}

/**
 * epf_ntb_probe() - Probe NTB function driver
 * @epf: NTB endpoint function device
 * @id: NTB endpoint function device ID
 *
 * Probe NTB function driver when endpoint function bus detects a NTB
 * endpoint function.
 */
/* [한국어]
 * epf_ntb_probe - EPF 가상 버스가 이 함수를 발견했을 때 상태만 만들어 둔다
 *
 * @epf: 새로 만들어진 NTB 엔드포인트 함수 디바이스
 * @id: 매칭된 이름 표 항목
 * @return: 0 성공, -ENOMEM 할당 실패
 *
 * 왜 필요한가: probe 시점에는 아직 어떤 EPC 에 붙을지 모른다.
 * 그래서 하드웨어는 건드리지 않고 상태 구조체만 잡아 둔다.
 * 실제 설정은 configfs 로 EPC 링크가 걸린 뒤 epf_ntb_bind() 에서 이뤄진다.
 *
 * 동작 단계: devm_kzalloc 으로 상태를 0 초기화해 잡고, config 헤더를
 * 연결하고, 역방향 포인터를 채운 뒤 EPF 디바이스에 매단다.
 * 0 초기화라 num_mws/db_count/spad_count 가 모두 0 으로 시작한다 —
 * 사용자가 configfs 로 채우기 전에는 아무 것도 만들지 않는다는 뜻이다.
 *
 * 실행 컨텍스트: EPF 가상 버스의 probe(사용자 mkdir 문맥, 프로세스 컨텍스트).
 *
 * 호출 체인:
 *   사용자 mkdir → configfs → pci_epf_create() → EPF 버스 매칭
 *     → [epf_ntb_probe]
 */
static int epf_ntb_probe(struct pci_epf *epf,
			 const struct pci_epf_device_id *id)
{
	/* [한국어] 이 함수에서 새로 만들 NTB 함수 상태 포인터. */
	struct epf_ntb *ntb;
	/* [한국어] 에러 로그와 devm_ 할당에 쓸 device 포인터. */
	struct device *dev;

	/* [한국어] EPF 디바이스의 struct device. devm_ 로 잡은 메모리는 이 디바이스가
	 * 사라질 때 자동으로 풀린다 — 그래서 별도 free 경로가 없다. */
	dev = &epf->dev;

	/* [한국어] 함수 전체 상태를 0 으로 채워 할당한다. kzalloc 이라 num_mws,
	 * db_count, spad_count 가 모두 0 으로 시작한다는 점이 중요하다 —
	 * 사용자가 configfs 로 채워 주기 전에는 아무 것도 만들지 않는다. */
	ntb = devm_kzalloc(dev, sizeof(*ntb), GFP_KERNEL);
	/* [한국어] 할당 실패는 그대로 상위(pci_epf_bus 의 probe)로 올린다. */
	if (!ntb)
		return -ENOMEM;

	/* [한국어] 이 함수가 호스트에게 내보일 PCI config 헤더를 지정한다.
	 * 실제로 쓰이는 곳은 epf_ntb_epc_init_interface() 의 pci_epc_write_header(). */
	epf->header = &epf_ntb_header;
	/* [한국어] 역방향 포인터. 모든 하위 함수가 ntb->epf 로 EPC 에 닿는다. */
	ntb->epf = epf;
	/* [한국어] EPF 디바이스에 이 상태를 매단다. 이후 bind/unbind/add_cfs 가
	 * epf_get_drvdata() 로 되찾는다. */
	epf_set_drvdata(epf, ntb);

	/* [한국어] probe 는 상태만 만들고 끝난다. 하드웨어 설정은 configfs 로 EPC 에
	 * 링크가 걸린 뒤 epf_ntb_bind() 에서 이뤄진다. */
	return 0;
}

/* [한국어] EPF 코어가 부르는 콜백 묶음. pci-epf-core.c 의 pci_epf_bind() 와
 * pci_epf_unbind() 가 여기를 통해 이 드라이버로 들어온다. */
static const struct pci_epf_ops epf_ntb_ops = {
	/* [한국어] bind: EPC 에 링크가 걸릴 때마다 불린다. 두 EPC 가 다 붙기 전에는
	 * 아무 것도 하지 않고 0 을 돌려준다. */
	.bind	= epf_ntb_bind,
	/* [한국어] unbind: 링크가 풀릴 때 BAR 를 걷고 메모리를 되돌린다. */
	.unbind	= epf_ntb_unbind,
	/* [한국어] add_cfs: configfs 에 이 함수 전용 디렉터리를 만들어 준다. */
	.add_cfs = epf_ntb_add_cfs,
};

/* [한국어] 이 드라이버가 맡을 EPF 이름 표. 사용자가 configfs 에서
 * mkdir pci_epf_ntb.0 을 하면 이 이름으로 매칭된다. */
static const struct pci_epf_device_id epf_ntb_ids[] = {
	{
		/* [한국어] 이름이 곧 매칭 키다. pci_epf_bus_type 의 match 가 이 문자열을 본다. */
		.name = "pci_epf_ntb",
	},
	/* [한국어] 빈 항목이 표의 끝을 뜻한다. */
	{},
};

/* [한국어] EPF 가상 버스에 등록할 드라이버 서술자. */
static struct pci_epf_driver epf_ntb_driver = {
	/* [한국어] 드라이버 이름. sysfs 의 /sys/bus/pci-epf/drivers/ 아래 이 이름으로 보인다. */
	.driver.name	= "pci_epf_ntb",
	/* [한국어] probe: 위에서 정의한 상태 할당 함수. */
	.probe		= epf_ntb_probe,
	/* [한국어] id_table: 위 이름 표. */
	.id_table	= epf_ntb_ids,
	/* [한국어] ops: bind/unbind/add_cfs 콜백 묶음. */
	.ops		= &epf_ntb_ops,
	/* [한국어] owner: 바인딩된 함수가 있는 동안 모듈 언로드를 막는다. */
	.owner		= THIS_MODULE,
};

/* [한국어]
 * epf_ntb_init - 모듈 적재 시각. 폴링 작업 큐를 만들고 EPF 드라이버를 등록한다
 *
 * @return: 0 성공, -ENOMEM 이면 작업 큐 생성 실패, 그 밖에는 드라이버 등록 실패 코드
 *
 * 왜 필요한가: 명령 폴링 워크가 돌 전용 큐가 있어야 하고, EPF 가상 버스에
 * 드라이버가 등록되어야 사용자가 만든 함수가 이 코드에 매칭된다.
 *
 * 작업 큐 플래그의 의미:
 *   WQ_MEM_RECLAIM  메모리 회수 경로에서도 진행이 보장되도록 전용 구조 워커 확보
 *   WQ_HIGHPRI      5ms 폴링이 밀리지 않도록 높은 우선순위
 *   WQ_PERCPU       CPU 를 넘나들지 않는 per-CPU 워커 풀 사용
 *
 * 실행 컨텍스트: insmod 문맥(프로세스 컨텍스트).
 *
 * 에러 경로: 드라이버 등록에 실패하면 앞서 만든 작업 큐를 반드시 되돌린다.
 *
 * 호출 체인:
 *   module_init → [epf_ntb_init] → alloc_workqueue() → pci_epf_register_driver()
 */
static int __init epf_ntb_init(void)
{
	/* [한국어] 작업 큐 생성과 드라이버 등록의 실패를 담을 변수. */
	int ret;

	/* [한국어] 명령 폴링 워크가 돌 전용 작업 큐를 만든다.
	 * WQ_MEM_RECLAIM: 메모리 회수 경로에서도 진행이 보장되도록 전용
	 * 구조 워커를 확보한다 — 호스트 명령 처리가 메모리 압박에 막히면
	 * 링크가 영영 올라오지 않는다.
	 * WQ_HIGHPRI: 5ms 폴링이 밀리지 않도록 높은 우선순위 워커를 쓴다.
	 * WQ_PERCPU: CPU 를 넘나들지 않는 per-CPU 워커 풀을 쓴다.
	 * 마지막 0 은 max_active 무제한(기본값)을 뜻한다. */
	kpcintb_workqueue = alloc_workqueue("kpcintb",
				    WQ_MEM_RECLAIM | WQ_HIGHPRI | WQ_PERCPU, 0);
	/* [한국어] 작업 큐를 못 만들면 폴링을 돌릴 수 없으므로 모듈 적재를 포기한다. */
	if (!kpcintb_workqueue) {
		/* [한국어] 아직 드라이버가 없어 dev_err 를 쓸 수 없으므로 pr_err 를 쓴다. */
		pr_err("Failed to allocate kpcintb workqueue\n");
		return -ENOMEM;
	}

	/* [한국어] EPF 가상 버스에 드라이버를 등록한다. 이 시점부터 사용자가
	 * configfs 로 만든 pci_epf_ntb 함수가 이 드라이버에 매칭된다. */
	ret = pci_epf_register_driver(&epf_ntb_driver);
	/* [한국어] 등록 실패 시 앞서 만든 작업 큐를 반드시 되돌려야 한다. */
	if (ret) {
		/* [한국어] 작업 큐 해제. 순서가 뒤바뀌면 큐가 새어 나간다. */
		destroy_workqueue(kpcintb_workqueue);
		/* [한국어] 실패 원인을 알 수 있도록 errno 를 함께 찍는다. */
		pr_err("Failed to register pci epf ntb driver --> %d\n", ret);
		/* [한국어] 실패 코드를 그대로 올려 모듈 적재를 중단시킨다. */
		return ret;
	}

	/* [한국어] 여기까지 오면 폴링 큐와 드라이버가 모두 준비된 상태다. */
	return 0;
}
/* [한국어] 모듈 적재 시각의 진입점 등록. */
module_init(epf_ntb_init);

/* [한국어]
 * epf_ntb_exit - 모듈 해제 시각. 드라이버를 내리고 작업 큐를 없앤다
 *
 * @return: 없음
 *
 * 왜 필요한가: 순서가 중요하다. 드라이버를 먼저 내려야 아직 바인딩된
 * 함수들의 unbind 가 불려 워크가 취소된다. 그 뒤에야 큐를 없앨 수 있다.
 * 반대로 하면 아직 큐잉된 워크가 사라진 큐를 참조하게 된다.
 *
 * 실행 컨텍스트: rmmod 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   module_exit → [epf_ntb_exit] → pci_epf_unregister_driver() → destroy_workqueue()
 */
static void __exit epf_ntb_exit(void)
{
	/* [한국어] 드라이버를 먼저 내린다. 이 호출 안에서 아직 바인딩된 함수가 있으면
	 * 각각의 unbind 가 불려 워크가 취소된다. */
	pci_epf_unregister_driver(&epf_ntb_driver);
	/* [한국어] 모든 워크가 사라진 뒤에야 작업 큐를 없앤다. 순서를 뒤집으면
	 * 아직 큐잉된 워크가 사라진 큐를 참조하게 된다. */
	destroy_workqueue(kpcintb_workqueue);
}
/* [한국어] 모듈 해제 시각의 진입점 등록. */
module_exit(epf_ntb_exit);

/* [한국어] modinfo 에 보일 설명. */
MODULE_DESCRIPTION("PCI EPF NTB DRIVER");
/* [한국어] 원저자 표기. 상류 그대로 둔다. */
MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
/* [한국어] 라이선스 표기. GPL v2 가 아니면 GPL 심볼을 쓸 수 없다. */
MODULE_LICENSE("GPL v2");
