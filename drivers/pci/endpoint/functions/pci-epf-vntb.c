// SPDX-License-Identifier: GPL-2.0
/*
 * Endpoint Function Driver to implement Non-Transparent Bridge functionality
 * Between PCI RC and EP
 *
 * Copyright (C) 2020 Texas Instruments
 * Copyright (C) 2022 NXP
 *
 * Based on pci-epf-ntb.c
 * Author: Frank Li <Frank.Li@nxp.com>
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */

/*
 * +------------+         +---------------------------------------+
 * |            |         |                                       |
 * +------------+         |                        +--------------+
 * | NTB        |         |                        | NTB          |
 * | NetDev     |         |                        | NetDev       |
 * +------------+         |                        +--------------+
 * | NTB        |         |                        | NTB          |
 * | Transfer   |         |                        | Transfer     |
 * +------------+         |                        +--------------+
 * |            |         |                        |              |
 * |  PCI NTB   |         |                        |              |
 * |    EPF     |         |                        |              |
 * |   Driver   |         |                        | PCI Virtual  |
 * |            |         +---------------+        | NTB Driver   |
 * |            |         | PCI EP NTB    |<------>|              |
 * |            |         |  FN Driver    |        |              |
 * +------------+         +---------------+        +--------------+
 * |            |         |               |        |              |
 * |  PCI Bus   | <-----> |  PCI EP Bus   |        |  Virtual PCI |
 * |            |  PCI    |               |        |     Bus      |
 * +------------+         +---------------+--------+--------------+
 * PCIe Root Port                        PCI EP
 */

/*
 * [한국어 설명] 엔드포인트 컨트롤러 하나와 로컬 가상 NTB 디바이스로
 *               루트 포트와 엔드포인트를 잇는 NTB 함수 드라이버 (pci-epf-vntb.c)
 *
 * === 파일의 역할 ===
 * PCIe 루트 포트(HOST)와 PCIe 엔드포인트(VHOST, 이 SoC 자신) 사이에
 * NTB(Non-Transparent Bridge, 비투명 브리지)를 소프트웨어로 만든다.
 * 형제 파일 pci-epf-ntb.c 가 EPC 두 개로 "두 외부 호스트" 를 잇는 데 반해,
 * 이 파일은 EPC 하나만 쓰고 그 반대편을 자기 커널 안에 만든다 —
 * 가상 PCI 버스와 그 위의 가상 PCI 장치를 소프트웨어로 세우고, 거기에
 * NTB 디바이스를 등록해 drivers/ntb 의 클라이언트(ntb_transport,
 * ntb_netdev 등)가 붙을 수 있게 하는 것이다.
 * 그래서 이 파일은 두 개의 얼굴을 갖는다. 아래쪽 절반은 호스트에게
 * BAR 를 노출하는 엔드포인트 함수이고, 위쪽 절반은 로컬 커널에게
 * NTB 디바이스를 제공하는 드라이버다. 두 얼굴이 struct epf_ntb 하나를
 * 공유하며, 그 안의 공유 레지스터(struct epf_ntb_ctrl)가 접점이다.
 * 결과적으로 EP 쪽 리눅스와 호스트 쪽 리눅스가 마치 NTB 로 연결된
 * 두 시스템처럼 통신할 수 있게 된다 — 상단 그림의 왼쪽이 호스트,
 * 오른쪽이 이 EP 다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 컨텍스트는 엔드포인트 쪽 SoC 의 커널 모듈이다.
 * 사용자가 configfs(drivers/pci/endpoint/pci-ep-cfs.c)에서 pci_epf_vntb
 * 함수를 만들어 EPC 에 링크하면 pci_epf_bind() → epf_ntb_bind() 로 들어온다.
 * epf_ntb_bind() 는 (1) BAR 번호 확정 → (2) 제어 + 스크래치패드 메모리
 * 할당 → (3) EPC 설정과 폴링 워크 기동 → (4) 가상 PCI 드라이버 등록 →
 * (5) 가상 PCI 버스 스캔의 순서로 진행하고, 마지막 단계에서
 * pci_vntb_probe() 가 불려 ntb_register_device() 로 NTB 디바이스가 태어난다.
 * 런타임에는 두 가지 통지 경로가 돈다.
 *   HOST → VHOST: 호스트가 도어벨 BAR 에 값을 쓴다. 플랫폼 MSI 도어벨이
 *     성공했다면 그 쓰기가 곧 SoC 인터럽트가 되어 epf_ntb_doorbell_handler
 *     가 즉시 처리하고, 실패해 폴링 방식으로 물러섰다면 5ms 주기
 *     epf_ntb_cmd_handler 가 배열을 읽어 처리한다.
 *   VHOST → HOST: vntb_epf_peer_db_set() 이 pci_epc_raise_irq() 로 MSI 를 쏜다.
 * 호스트가 보내는 명령(창 설정, 링크 상태)은 언제나 공유 레지스터를 통한
 * 폴링으로 받는다 — 그쪽에서 이쪽으로 오는 명령 통지 경로가 없기 때문이다.
 *
 * === 타 모듈과의 연결 ===
 * 아래로는 엔드포인트 코어에 의존한다.
 *   drivers/pci/endpoint/pci-epc-core.c — pci_epc_set_bar/clear_bar,
 *     pci_epc_map_addr/unmap_addr, pci_epc_set_msi, pci_epc_raise_irq,
 *     pci_epc_get_features, pci_epc_get_next_free_bar, pci_epc_write_header.
 *     컨트롤러 콜백은 모두 epc->lock 뮤텍스 아래에서 실행된다.
 *   drivers/pci/endpoint/pci-epf-core.c — pci_epf_alloc_space(내부에서
 *     dma_alloc_coherent), pci_epf_free_space, pci_epf_assign_bar_space,
 *     pci_epf_align_inbound_addr.
 *   drivers/pci/endpoint/pci-ep-msi.c — pci_epf_alloc_doorbell,
 *     pci_epf_free_doorbell. 플랫폼 MSI 도메인에서 벡터를 잡아 그 주소를
 *     BAR 로 노출하는 인터럽트 기반 도어벨의 토대다.
 *   drivers/pci/endpoint/pci-epc-mem.c — pci_epc_mem_alloc_addr/free_addr.
 * 위로는 두 갈래다. 하나는 configfs 속성(spad_count, db_count, num_mws,
 * mw1~mw4, vbus_number, vntb_pid, vntb_vid, ctrl_bar/db_bar/mwN_bar)이고,
 * 다른 하나는 NTB 서브시스템이다 — ntb_register_device(), ntb_link_event(),
 * ntb_db_event() 를 부르고 struct ntb_dev_ops 를 통해 요청을 받는다.
 * 이 트리에는 drivers/ntb 와 include/linux/ntb.h 가 체크아웃되어 있지 않아
 * 그쪽 구현은 확인하지 못했고, 호출부만 확인했다.
 * (Kconfig 에서 PCI_EPF_VNTB 가 NTB 를 depends 하는 반면 PCI_EPF_NTB 는
 *  그렇지 않다는 점이 두 파일의 구조 차이를 그대로 보여 준다.)
 * 공유 상태의 핵심은 struct epf_ntb_ctrl 이다. __packed 로 선언되어 BAR 를
 * 통해 호스트 메모리 공간에 그대로 노출되므로 사실상 호스트와의 ABI 다.
 * 스크래치패드는 그 뒤에 두 절반으로 놓이며, 한쪽이 "내 것" 으로 쓰는
 * 절반을 다른 쪽이 "상대 것" 으로 읽는 교차 배치다.
 *
 * === 주요 함수/구조체 요약 ===
 * - epf_ntb_bind(): EPC 설정부터 가상 PCI 버스 스캔까지 전체를 세우는 진입점.
 * - pci_vntb_probe(): 가상 PCI 장치에 붙어 ntb_register_device() 로
 *   NTB 디바이스를 태어나게 하는 지점. 이 파일 구조의 정점이다.
 * - epf_ntb_db_bar_init_msi_doorbell(): 플랫폼 MSI 주소를 BAR 로 노출해
 *   인터럽트 기반 도어벨을 만든다. 실패하면 폴링 방식으로 물러선다.
 * - epf_ntb_cmd_handler(): 도어벨 폴링과 호스트 명령 처리를 겸하는 주기 워크.
 * - epf_ntb_find_bar(): 사용자가 configfs 로 지정하지 않은 구성요소에만
 *   BAR 를 자동 배정한다 — 형제 파일에는 없는 vntb 만의 기능이다.
 * - vntb_epf_ops: NTB 연산 표. drivers/ntb 코어가 이 표로만 이 드라이버를 부른다.
 * - struct epf_ntb: 함수 전체 상태. 첫 필드가 struct ntb_dev 라
 *   ntb_ndev() 의 container_of 가 성립한다.
 * - struct epf_ntb_ctrl: 호스트가 BAR 로 보게 되는 공유 레지스터.
 * - pci_space[] / vpci_ops / vpci_scan_bus(): 실제 하드웨어 없이 PCI 버스와
 *   장치 하나를 흉내 내는 계층. drivers/ntb 가 요구하는 "PCI 디바이스에
 *   얹힌 NTB 디바이스" 모양을 갖추기 위해 존재한다.
 */

/* [한국어] atomic.h: 밀린 도어벨 비트맵을 atomic64_or/and/read 로 다룬다.
 * 인터럽트 문맥과 워크 문맥이 같은 변수를 만지므로 필요하다. */
#include <linux/atomic.h>
/* [한국어] delay.h: msecs_to_jiffies() 로 폴링 주기를 지연 단위로 바꾼다. */
#include <linux/delay.h>
/* [한국어] io.h: readl/writel 과 __iomem 표시. 스크래치패드 접근에 쓴다. */
#include <linux/io.h>
/* [한국어] module.h: module_init/module_exit 와 MODULE_ 매크로. */
#include <linux/module.h>
/* [한국어] slab.h: devm_kzalloc 계열 할당. */
#include <linux/slab.h>

/* [한국어] pci-ep-msi.h: 플랫폼 MSI 기반 도어벨 API.
 * pci_epf_alloc_doorbell/free_doorbell 이 여기서 온다.
 * 구현은 drivers/pci/endpoint/pci-ep-msi.c 에 있다. */
#include <linux/pci-ep-msi.h>
/* [한국어] pci-epc.h: 엔드포인트 컨트롤러 API. BAR 설정, 아웃바운드 매핑,
 * MSI 설정, 인터럽트 발생이 모두 여기서 온다. */
#include <linux/pci-epc.h>
/* [한국어] pci-epf.h: 엔드포인트 함수 API. 드라이버 등록, BAR 메모리 할당,
 * pci_epf_bar 구조체 정의. */
#include <linux/pci-epf.h>
/* [한국어] ntb.h: NTB 서브시스템 API. struct ntb_dev, struct ntb_dev_ops,
 * ntb_register_device(), ntb_link_event(), ntb_db_event() 가 여기서 온다.
 * 이 인클루드가 형제 파일 pci-epf-ntb.c 와의 결정적 차이다 —
 * 그쪽은 drivers/ntb 와 아무 관계가 없다.
 * (이 트리에는 drivers/ntb 와 include/linux/ntb.h 가 체크아웃되어 있지 않아
 *  호출부만 확인했고 구현은 확인하지 못했다.) */
#include <linux/ntb.h>

/* [한국어] 명령 폴링 워크가 도는 전용 작업 큐. 모듈 하나에 하나뿐이다.
 * 생성은 epf_ntb_init(), 해제는 epf_ntb_exit() 이다. */
static struct workqueue_struct *kpcintb_workqueue;

/* [한국어] 아래 여섯 개는 호스트가 command 필드에 써 넣는 명령 코드다.
 * 형제 파일 pci-epf-ntb.c 와 값이 같다 — 호스트 쪽 드라이버가
 * 두 구현을 같은 프로토콜로 다룰 수 있게 하기 위한 것으로 읽힌다.
 * 다만 vntb 는 도어벨 명령을 받아도 아무 것도 하지 않는다. */
#define COMMAND_CONFIGURE_DOORBELL	1
#define COMMAND_TEARDOWN_DOORBELL	2
#define COMMAND_CONFIGURE_MW		3
#define COMMAND_TEARDOWN_MW		4
#define COMMAND_LINK_UP			5
#define COMMAND_LINK_DOWN		6

/* [한국어] 명령 처리 결과 코드. 0 은 "아직 처리 안 됨" 을 뜻하도록 비워 둔다. */
#define COMMAND_STATUS_OK		1
#define COMMAND_STATUS_ERROR		2

/* [한국어] link_status 필드의 비트 0. 링크가 올라가 있음을 뜻한다. */
#define LINK_STATUS_UP			BIT(0)

/* [한국어] 기본값과 한계값들.
 *   SPAD_COUNT(64), DB_COUNT(4): 기본값으로 두려던 상수로 보이나
 *     이 파일 어디에서도 참조되지 않는다(정의 한 곳뿐).
 *   NTB_MW_OFFSET(2): 이 파일에서는 쓰이지 않는다. 형제 파일에서
 *     가져오면서 함께 남은 것으로 읽힌다.
 *   DB_COUNT_MASK, MSIX_ENABLE: 도어벨 명령 인자의 형식이지만,
 *     vntb 는 그 명령을 처리하지 않아 역시 쓰이지 않는다.
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
 * 다를 수 있고 사용자가 직접 지정할 수도 있으므로,
 * 이 인덱스로 epf_ntb_bar[] 표를 찾아 실제 번호를 얻는다.
 * ntb 쪽 enum 과 달리 peer 스크래치패드 칸이 없다 — vntb 는 스크래치패드
 * 두 절반을 한 영역에 나란히 두므로 BAR 가 하나만 있으면 된다. */
enum epf_ntb_bar {
	/* [한국어] 제어 영역 + 스크래치패드 두 절반이 놓이는 BAR. */
	BAR_CONFIG,
	/* [한국어] 도어벨이 놓이는 BAR. MSI 방식이면 MSI 컨트롤러 주소를,
	 * 폴링 방식이면 우리가 잡은 워드 배열을 노출한다. */
	BAR_DB,
	/* [한국어] 첫 메모리 윈도우 BAR. ntb 쪽처럼 도어벨과 합치지 않고 따로 쓴다. */
	BAR_MW1,
	/* [한국어] 두 번째 메모리 윈도우 BAR. */
	BAR_MW2,
	/* [한국어] 세 번째 메모리 윈도우 BAR. */
	BAR_MW3,
	/* [한국어] 네 번째 메모리 윈도우 BAR. */
	BAR_MW4,
	/* [한국어] 칸 수를 세는 데 쓰는 마지막 값. epf_ntb_bar[] 배열 크기이자
	 * epf_ntb_is_bar_used() 의 순회 상한이다. */
	VNTB_BAR_NUM,
};

/* [한국어] 상류 그림이 스크래치패드 배치를 보여 준다. 핵심은 아래쪽 좌우가
 * 엇갈려 있다는 점이다 — 같은 물리 메모리를 두고, 가상 NTB 쪽에서는
 * 앞 절반이 "상대 것" 이고 뒤 절반이 "내 것" 인데, PCIe 엔드포인트
 * (호스트) 쪽에서는 정반대로 읽는다. 그래서 한쪽이 자기 스크래치패드에
 * 쓴 값을 다른 쪽이 상대 스크래치패드에서 읽게 된다.
 * 이 파일의 vntb_epf_spad_read/write 가 뒤 절반을,
 * vntb_epf_peer_spad_read/write 가 앞 절반을 다루는 이유가 이것이다. */
/*
 * +--------------------------------------------------+ Base
 * |                                                  |
 * |                                                  |
 * |                                                  |
 * |          Common Control Register                 |
 * |                                                  |
 * |                                                  |
 * |                                                  |
 * +-----------------------+--------------------------+ Base+spad_offset
 * |                       |                          |
 * |    Peer Spad Space    |    Spad Space            |
 * |                       |                          |
 * |                       |                          |
 * +-----------------------+--------------------------+ Base+spad_offset
 * |                       |                          |     +spad_count * 4
 * |                       |                          |
 * |     Spad Space        |   Peer Spad Space        |
 * |                       |                          |
 * +-----------------------+--------------------------+
 *       Virtual PCI             PCIe Endpoint
 *       NTB Driver               NTB Driver
 */
struct epf_ntb_ctrl {
	/* [한국어] 호스트 → EP 방향 명령 코드.
	 * 설정자: 호스트가 BAR 에 직접 쓴다.
	 * 읽는 자: epf_ntb_cmd_handler() 가 폴링해 읽고 0 으로 지운다.
	 * 값 범위: COMMAND_CONFIGURE_DOORBELL(1) ~ COMMAND_LINK_DOWN(6).
	 * 동기화: 락이 없다. 호스트가 0 이 되기를 기다렸다가 다음 명령을 쓰는
	 *   약속으로만 보호된다. */
	u32 command;
	/* [한국어] 명령의 인자.
	 * 설정자: 호스트가 command 를 쓰기 전에 먼저 쓴다.
	 * 읽는 자: cmd_handler 가 command 를 읽은 직후에 읽는다.
	 * 값 범위: CONFIGURE_MW/TEARDOWN_MW 면 창 번호.
	 *   (vntb 는 도어벨 명령에서 인자를 쓰지 않는다.)
	 * 동기화: command 와 같은 약속을 따른다. */
	u32 argument;
	/* [한국어] 명령 처리 결과.
	 * 설정자: cmd_handler 가 처리 후 쓴다.
	 * 읽는 자: 호스트가 폴링해 읽는다.
	 * 값 범위: COMMAND_STATUS_OK(1) 또는 COMMAND_STATUS_ERROR(2).
	 * 동기화: 없음. */
	u16 command_status;
	/* [한국어] 링크 상태 비트맵.
	 * 설정자: epf_ntb_link_up().
	 * 읽는 자: 호스트가 자기 BAR 로 읽고, 로컬 쪽은 vntb_epf_link_is_up() 이 읽는다.
	 * 값 범위: LINK_STATUS_UP(비트 0) 하나만 쓰인다.
	 * 동기화: 없음. */
	u16 link_status;
	/* [한국어] NTB 토폴로지.
	 * 설정자: 이 파일 어디에서도 쓰지 않는다 — 호스트 쪽 드라이버와의
	 *   약속을 위해 자리만 잡아 둔 필드다.
	 * 읽는 자: 호스트.
	 * 값 범위: 이 트리만으로는 확인할 수 없다.
	 * 동기화: 해당 없음. */
	u32 topology;
	/* [한국어] 메모리 윈도우를 걸 목적지 PCI 주소.
	 * 설정자: 호스트가 COMMAND_CONFIGURE_MW 전에 쓴다.
	 * 읽는 자: epf_ntb_configure_mw() 가 pci_epc_map_addr() 의 인자로 쓴다.
	 * 값 범위: 호스트가 관리하는 PCI 주소.
	 * 동기화: 명령 프로토콜의 순서 약속. */
	u64 addr;
	/* [한국어] 그 창의 크기.
	 * 설정자: 호스트.
	 * 읽는 자: epf_ntb_configure_mw(). ntb 쪽과 달리 상한 검사가 없다.
	 * 값 범위: 0 초과.
	 * 동기화: addr 와 같다. */
	u64 size;
	/* [한국어] 이 EP 가 제공하는 메모리 윈도우 개수.
	 * 설정자: epf_ntb_config_spad_bar_alloc() 이 ntb->num_mws 를 복사한다.
	 * 읽는 자: 호스트.
	 * 값 범위: 0 ~ MAX_MW(4).
	 * 동기화: 초기화 시점에만 쓰인다. */
	u32 num_mws;
	/* [한국어] 예약 필드.
	 * 설정자: 아무도 쓰지 않는다.
	 * 읽는 자: 아무도 읽지 않는다.
	 * 값 범위: 0.
	 * 동기화: 해당 없음.
	 * 존재 이유: 형제 파일 pci-epf-ntb.c 의 같은 구조체에서 이 자리에
	 *   mw1_offset 이 있다. 배치를 맞추려고 자리를 비워 둔 것으로 읽히나,
	 *   이 트리에서 그 근거를 확정하지는 못했다. */
	u32 reserved;
	/* [한국어] 제어 영역 시작점에서 스크래치패드까지의 거리.
	 * 설정자: epf_ntb_config_spad_bar_alloc().
	 * 읽는 자: 호스트, 그리고 vntb_epf_spad_read/write 와 peer_spad_read/write.
	 * 값 범위: 4바이트 정렬된 제어 영역 크기.
	 * 동기화: 초기화 시점에만 쓰인다. */
	u32 spad_offset;
	/* [한국어] 한 절반의 스크래치패드 워드 개수.
	 * 설정자: epf_ntb_config_spad_bar_alloc().
	 * 읽는 자: 호스트, 그리고 vntb_epf_spad_read/write 가 뒤쪽 절반의
	 *   위치를 계산할 때(오프셋 + spad_count x 4).
	 * 값 범위: 사용자가 정한 spad_count.
	 * 동기화: 초기화 시점에만 쓰인다. */
	u32 spad_count;
	/* [한국어] 도어벨 한 칸의 크기(바이트).
	 * 설정자: epf_ntb_config_spad_bar_alloc() 이 4 로 두고,
	 *   MSI 도어벨이 서면 epf_ntb_db_bar_init_msi_doorbell() 이 0 으로 덮어쓴다.
	 * 읽는 자: 호스트가 도어벨 i 의 위치를 계산할 때.
	 * 값 범위: 4(폴링 방식) 또는 0(MSI 방식 — db_offset 을 그대로 쓰라는 뜻).
	 * 동기화: bind 시점에만 쓰인다. */
	u32 db_entry_size;
	/* [한국어] 도어벨 i 를 울리려면 써야 할 값.
	 * 설정자: 폴링 방식이면 1 + i, MSI 방식이면 그 벡터의 MSI 데이터.
	 * 읽는 자: 호스트.
	 * 값 범위: 0 이 아닌 값(폴링 방식은 0 을 "울리지 않음" 으로 쓴다).
	 * 동기화: bind 시점에만 쓰인다. */
	u32 db_data[MAX_DB_COUNT];
	/* [한국어] 도어벨 i 를 울릴 때 BAR 안에서의 오프셋.
	 * 설정자: 폴링 방식이면 0, MSI 방식이면 정렬된 BAR 시작점에서
	 *   그 벡터의 MSI 주소까지의 거리.
	 * 읽는 자: 호스트.
	 * 값 범위: BAR 크기 미만.
	 * 동기화: bind 시점에만 쓰인다. */
	u32 db_offset[MAX_DB_COUNT];
/* [한국어] __packed: 컴파일러가 패딩을 넣지 못하게 한다. 이 구조체는 호스트와
 * 바이트 배치를 공유하므로 패딩이 들어가면 양쪽 해석이 어긋난다. */
} __packed;

/* [한국어] vntb 함수 전체의 상태. EPF 디바이스 하나에 하나씩 있다.
 * ntb 쪽 struct epf_ntb 와 이름은 같지만 내용이 다르다 —
 * 가장 큰 차이는 첫 필드가 struct ntb_dev 라는 점이다. */
struct epf_ntb {
	/* [한국어] NTB 코어에 등록되는 디바이스 자체.
	 * 설정자: pci_vntb_probe() 가 pdev/topo/ops 를 채우고 ntb_register_device() 를 부른다.
	 * 읽는 자: drivers/ntb 코어와 그 클라이언트.
	 * 값 범위: 유효한 구조체. 첫 필드라 ntb_ndev() 의 container_of 가 성립한다.
	 * 동기화: NTB 코어가 관리한다. (이 트리에 drivers/ntb 가 없어
	 *   구현은 확인하지 못했다.) */
	struct ntb_dev ntb;
	/* [한국어] 이 상태가 매달린 EPF 디바이스.
	 * 설정자: epf_ntb_probe().
	 * 읽는 자: EPC 포인터 획득, BAR 서술자 접근, 로그 대상.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 수명이 EPF 디바이스와 같다. */
	struct pci_epf *epf;
	/* [한국어] configfs 디렉터리를 나타내는 그룹. 구조체 안에 박혀 있어 별도 할당이 없다.
	 * 설정자: epf_ntb_add_cfs().
	 * 읽는 자: to_epf_ntb() 매크로.
	 * 값 범위: configfs 가 관리하는 구조체.
	 * 동기화: configfs 내부 락. */
	struct config_group group;

	/* [한국어] 쓸 메모리 윈도우 개수.
	 * 설정자: 사용자가 configfs 의 num_mws 에 쓰거나, BAR 가 모자라면
	 *   epf_ntb_init_epc_bar() 가 줄인다.
	 * 읽는 자: BAR 배정, 창 생성, vntb_epf_mw_count().
	 * 값 범위: 0 ~ MAX_MW(4).
	 * 동기화: bind 전에 설정되고 그 뒤에는 사실상 읽기 전용이다. */
	u32 num_mws;
	/* [한국어] 도어벨 개수.
	 * 설정자: 사용자가 configfs 의 db_count 에 쓴다.
	 * 읽는 자: 도어벨 IRQ 할당, 폴링 루프, vntb_epf_db_valid_mask().
	 * 값 범위: 0 ~ MAX_DB_COUNT(32).
	 * 동기화: bind 전에 설정된다. */
	u32 db_count;
	/* [한국어] 스크래치패드 워드 개수(한 절반 기준).
	 * 설정자: 사용자가 configfs 의 spad_count 에 쓴다.
	 * 읽는 자: epf_ntb_config_spad_bar_alloc(), vntb_epf_spad_count().
	 * 값 범위: 0 이상.
	 * 동기화: bind 전에 설정된다. */
	u32 spad_count;
	/* [한국어] 각 메모리 윈도우의 크기(바이트).
	 * 설정자: 사용자가 configfs 의 mw1~mw4 에 쓴다.
	 * 읽는 자: 창 BAR 생성, vntb_epf_mw_get_align(), peer_mw_get_addr().
	 * 값 범위: 0 이상.
	 * 동기화: bind 전에 설정된다. */
	u64 mws_size[MAX_MW];
	/* [한국어] 밀린 도어벨 비트맵.
	 * 설정자: epf_ntb_doorbell_handler()(인터럽트 문맥) 또는
	 *   epf_ntb_cmd_handler()(워크 문맥)가 atomic64_or 로 비트를 세운다.
	 * 읽는 자: vntb_epf_db_read() 가 읽고, vntb_epf_db_clear() 가 지운다.
	 * 값 범위: 하위 db_count 비트.
	 * 동기화: atomic64 연산으로 보호된다 — 세우는 쪽과 지우는 쪽의
	 *   실행 문맥이 서로 달라 락 없이 겹칠 수 있기 때문이다. */
	atomic64_t db;
	/* [한국어] 만들 가상 PCI 버스의 번호.
	 * 설정자: epf_ntb_probe() 가 0xff 로 초기화하고 사용자가 configfs 로 바꾼다.
	 * 읽는 자: vpci_scan_bus() 의 pci_scan_bus().
	 * 값 범위: 0 ~ 255. 실제 PCI 버스와 겹치지 않아야 한다.
	 * 동기화: bind 전에 설정된다. */
	u32 vbus_number;
	/* [한국어] 가상 NTB 장치의 PCI 디바이스 ID.
	 * 설정자: 사용자가 configfs 의 vntb_pid 에 쓴다.
	 * 읽는 자: epf_ntb_bind() 가 pci_space[0] 와 매칭 표에 반영한다.
	 * 값 범위: 16비트. 0 이면 매칭이 되지 않는다.
	 * 동기화: bind 전에 설정된다. */
	u16 vntb_pid;
	/* [한국어] 가상 NTB 장치의 PCI 벤더 ID.
	 * 설정자: 사용자가 configfs 의 vntb_vid 에 쓴다.
	 * 읽는 자: vntb_pid 와 같다.
	 * 값 범위: 16비트.
	 * 동기화: bind 전에 설정된다. */
	u16 vntb_vid;

	/* [한국어] 링크가 올라가 있는가.
	 * 설정자: epf_ntb_cmd_handler() 의 LINK_UP/LINK_DOWN 처리.
	 * 읽는 자: 이 파일 안에서 실제로 읽는 곳은 없다 — 상태 기록용이다.
	 * 값 범위: true/false.
	 * 동기화: 워크 문맥에서만 쓰인다. */
	bool linkup;
	/* [한국어] 플랫폼 MSI 기반 도어벨이 성공했는가.
	 * 설정자: epf_ntb_db_bar_init_msi_doorbell() 이 성공 시 true 로 만든다.
	 * 읽는 자: cmd_handler 가 폴링 여부와 주기를 정할 때,
	 *   epf_ntb_db_bar_clear() 가 IRQ 반납 여부를 정할 때.
	 * 값 범위: true 면 인터럽트 방식, false 면 메모리 폴링 방식.
	 * 동기화: bind 시점에 정해지고 이후 읽기 전용이다. */
	bool msi_doorbell;
	/* [한국어] 확정된 스크래치패드 전체 크기(두 절반 합).
	 * 설정자: epf_ntb_config_spad_bar_alloc().
	 * 읽는 자: 이 파일 안에서 다시 읽는 곳은 없다 — 기록용이다.
	 * 값 범위: 2 x spad_count x 4 바이트.
	 * 동기화: bind 시점에만 쓰인다. */
	u32 spad_size;

	/* [한국어] 논리 구성요소를 실제 BAR 번호로 옮기는 표.
	 * 설정자: epf_ntb_probe() 가 NO_BAR 로 채우고, 사용자가 configfs 로
	 *   일부를 지정할 수 있으며, epf_ntb_find_bar() 가 나머지를 자동 배정한다.
	 * 읽는 자: BAR 를 다루는 모든 함수.
	 * 값 범위: NO_BAR(음수) 또는 0 ~ 5.
	 * 동기화: bind 시점까지 쓰이고 이후 읽기 전용이다. */
	enum pci_barno epf_ntb_bar[VNTB_BAR_NUM];

	/* [한국어] 호스트와 공유하는 레지스터 영역의 시작(커널 가상 주소).
	 * 설정자: epf_ntb_config_spad_bar_alloc() 이 pci_epf_alloc_space() 결과를 넣는다.
	 * 읽는 자: cmd_handler, 스크래치패드 접근 함수들, 링크 상태 읽기.
	 * 값 범위: dma_alloc_coherent 로 잡힌 메모리.
	 * 동기화: 호스트와 동시에 접근하므로 명령 프로토콜의 순서 약속에 의존한다. */
	struct epf_ntb_ctrl *reg;

	/* [한국어] 폴링 방식 도어벨 배열의 시작(커널 가상 주소).
	 * 설정자: epf_ntb_db_bar_init() 이 폴링 경로로 물러설 때만 채운다.
	 * 읽는 자: cmd_handler 의 도어벨 폴링 루프.
	 * 값 범위: 유효한 포인터 또는 NULL(MSI 방식일 때).
	 * 동기화: 호스트가 쓰고 워크가 읽는다. 락 없이 워드 단위 접근에 의존한다. */
	u32 *epf_db;

	/* [한국어] 각 아웃바운드 창의 물리 주소.
	 * 설정자: epf_ntb_mw_bar_init() 이 pci_epc_mem_alloc_addr 로 얻는다.
	 * 읽는 자: epf_ntb_configure_mw(), teardown_mw(), vntb_epf_peer_mw_get_addr().
	 * 값 범위: 컨트롤러 아웃바운드 공간 안의 물리 주소.
	 * 동기화: bind 시점에 정해지고 이후 읽기 전용이다. */
	phys_addr_t vpci_mw_phy[MAX_MW];
	/* [한국어] 같은 창의 커널 가상 주소. 로컬 NTB 클라이언트가 여기에 쓰면
	 *   창을 지나 호스트 메모리로 나간다.
	 * 설정자: epf_ntb_mw_bar_init().
	 * 읽는 자: 해제 경로. (실제 데이터 쓰기는 NTB 클라이언트가 한다.)
	 * 값 범위: 유효한 __iomem 포인터 또는 NULL.
	 * 동기화: bind/unbind 경로에서만 쓰인다. */
	void __iomem *vpci_mw_addr[MAX_MW];

	/* [한국어] 5ms(또는 MSI 도어벨이면 500ms) 주기 명령 폴링 워크.
	 * 설정자: epf_ntb_epc_init() 이 초기화하고 큐에 넣는다.
	 * 읽는 자: 워크 함수 자신이 container_of 로 이 구조체를 되찾는다.
	 * 값 범위: 커널 워크큐가 관리하는 불투명 구조체.
	 * 동기화: 정지는 epf_ntb_epc_cleanup() 의 disable_delayed_work_sync(). */
	struct delayed_work cmd_handler;
};

/* [한국어] to_epf_ntb: configfs 그룹에서 함수 상태로 되돌아오는 매크로.
 * ntb_ndev: NTB 디바이스에서 함수 상태로 되돌아오는 매크로.
 * 둘 다 container_of 로, 구조체 안에 박힌 필드의 오프셋을 빼는 방식이다.
 * 이 두 매크로가 있어 configfs 쪽 코드와 NTB 쪽 코드가 같은 상태를 공유한다. */
#define to_epf_ntb(epf_group) container_of((epf_group), struct epf_ntb, group)
#define ntb_ndev(__ntb) container_of(__ntb, struct epf_ntb, ntb)

/* [한국어] 이 함수가 호스트에게 내보일 PCI config 헤더의 원본.
 * epf_ntb_probe() 가 epf->header 에 연결하고,
 * epf_ntb_epc_init() 이 pci_epc_write_header() 로 실제로 쓴다.
 * 주의: 이 헤더는 "호스트가 보는 EP" 의 정체이고,
 * 위쪽 pci_space[] 는 "로컬 가상 버스에서 보는 장치" 의 정체다 —
 * 둘은 서로 다른 값이며 후자만 configfs 로 설정할 수 있다. */
static struct pci_epf_header epf_ntb_header = {
	/* [한국어] 벤더 ID 를 PCI_ANY_ID 로 둔다. */
	.vendorid	= PCI_ANY_ID,
	/* [한국어] 디바이스 ID 도 마찬가지. */
	.deviceid	= PCI_ANY_ID,
	/* [한국어] 기본 클래스 코드를 "메모리 컨트롤러" 로 선언한다. */
	.baseclass_code	= PCI_BASE_CLASS_MEMORY,
	/* [한국어] 레거시 INTx 핀을 INTA 로 선언한다. */
	.interrupt_pin	= PCI_INTERRUPT_INTA,
};

/**
 * epf_ntb_link_up() - Raise link_up interrupt to Virtual Host (VHOST)
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 * @link_up: true or false indicating Link is UP or Down
 *
 * Once NTB function in HOST invoke ntb_link_enable(),
 * this NTB function driver will trigger a link event to VHOST.
 *
 * Returns: Zero for success, or an error code in case of failure
 */
/* [한국어]
 * epf_ntb_link_up - 링크 상태를 갱신하고 로컬 NTB 코어에 알린다
 *
 * @ntb: HOST(루트 포트)와 VHOST(로컬 가상 NTB)를 잇는 함수 상태
 * @link_up: true 면 링크 업, false 면 링크 다운
 * @return: 항상 0. 실패할 여지가 없는 경로다.
 *
 * 왜 필요한가: 링크 사건을 양쪽에 알려야 한다. 호스트 쪽에는 공유
 * 레지스터의 link_status 비트를 갱신해 두면 호스트가 폴링으로 읽어 가고,
 * 로컬 쪽에는 ntb_link_event() 로 NTB 코어를 직접 깨운다.
 * 형제 파일 pci-epf-ntb.c 가 두 호스트 모두에게 인터럽트를 쏘는 것과
 * 대비되는 지점이며, vntb 의 "한쪽은 같은 커널 안에 있다" 는 구조가
 * 여기서 그대로 드러난다.
 *
 * 실행 컨텍스트: epf_ntb_cmd_handler() 워크 안(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_cmd_handler() → [epf_ntb_link_up] → ntb_link_event()
 */
static int epf_ntb_link_up(struct epf_ntb *ntb, bool link_up)
{
	/* [한국어] 링크를 올리는 중인가. */
	if (link_up)
		/* [한국어] 공유 레지스터의 link_status 비트 0 을 세운다. 호스트가 자기 BAR 로
		 * 이 필드를 읽어 링크 상태를 안다. */
		ntb->reg->link_status |= LINK_STATUS_UP;
	else
		/* [한국어] 링크를 내리는 중이면 같은 비트를 지운다. */
		ntb->reg->link_status &= ~LINK_STATUS_UP;

	/* [한국어] 로컬 NTB 코어에 링크 사건을 알린다. 이것이 ntb 쪽 파일과 결정적으로
	 * 다른 점이다 — 그쪽은 두 호스트에게 인터럽트를 쏘지만, 여기서는
	 * 같은 커널 안의 NTB 클라이언트를 직접 깨운다.
	 * 호스트 쪽에는 위에서 갱신한 link_status 를 폴링으로 알린다. */
	ntb_link_event(&ntb->ntb);
	/* [한국어] 항상 성공을 돌려준다. 실패할 여지가 없는 경로다. */
	return 0;
}

/**
 * epf_ntb_configure_mw() - Configure the Outbound Address Space for VHOST
 *   to access the memory window of HOST
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 * @mw: Index of the memory window (either 0, 1, 2 or 3)
 *
 *                          EP Outbound Window
 * +--------+              +-----------+
 * |        |              |           |
 * |        |              |           |
 * |        |              |           |
 * |        |              |           |
 * |        |              +-----------+
 * | Virtual|              | Memory Win|
 * | NTB    | -----------> |           |
 * | Driver |              |           |
 * |        |              +-----------+
 * |        |              |           |
 * |        |              |           |
 * +--------+              +-----------+
 *  VHOST                   PCI EP
 *
 * Returns: Zero for success, or an error code in case of failure
 */
/* [한국어]
 * epf_ntb_configure_mw - 호스트가 지정한 PCI 주소로 아웃바운드 창을 건다
 *
 * @ntb: 함수 상태
 * @mw: 창 번호(0~3)
 * @return: 0 성공, 그 밖에는 pci_epc_map_addr() 의 실패 코드
 *
 * 왜 필요한가: 로컬 NTB 클라이언트가 vpci_mw_addr 에 쓴 데이터가 호스트
 * 메모리로 흘러가려면, 그 아웃바운드 창을 호스트가 지정한 PCI 주소로
 * 매핑해야 한다. 호스트는 공유 레지스터의 addr/size 에 목적지를 써 두고
 * COMMAND_CONFIGURE_MW 를 보낸다.
 *
 * 주의: 형제 파일 pci-epf-ntb.c 는 호스트가 요청한 크기가 준비된 창보다
 * 크지 않은지 검사하지만, 이 함수에는 그 검사가 없다. 상류 코드 그대로 둔다.
 *
 * 실행 컨텍스트: cmd_handler 워크(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 호출 체인:
 *   epf_ntb_cmd_handler() → [epf_ntb_configure_mw] → pci_epc_map_addr()
 */
static int epf_ntb_configure_mw(struct epf_ntb *ntb, u32 mw)
{
	/* [한국어] 매핑할 아웃바운드 창의 물리 주소. */
	phys_addr_t phys_addr;
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] 호스트가 알려 준 목적지 PCI 주소와 크기. */
	u64 addr, size;
	/* [한국어] 매핑 실패를 담는다. */
	int ret = 0;

	/* [한국어] 이 창에 대응하는 아웃바운드 공간의 물리 주소.
	 * epf_ntb_mw_bar_init() 이 pci_epc_mem_alloc_addr 로 얻어 둔 값이다. */
	phys_addr = ntb->vpci_mw_phy[mw];
	/* [한국어] 호스트가 공유 레지스터에 써 넣은 목적지 PCI 주소. */
	addr = ntb->reg->addr;
	/* [한국어] 매핑할 크기. 역시 호스트가 지정한다.
	 * ntb 쪽과 달리 여기서는 준비된 창 크기와 대조하는 검사가 없다.
	 * 상류 코드 그대로 둔다. */
	size = ntb->reg->size;

	/* [한국어] 이 EPF 의 물리 함수 번호. */
	func_no = ntb->epf->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb->epf->vfunc_no;

	/* [한국어] 아웃바운드 주소 phys_addr 를 호스트가 지정한 PCI 주소에 size 만큼 건다.
	 * 이 매핑이 서면, 로컬 NTB 클라이언트가 vpci_mw_addr 에 쓴 데이터가
	 * 창을 지나 호스트 메모리로 흘러간다. epc->lock 아래에서 실행된다. */
	ret = pci_epc_map_addr(ntb->epf->epc, func_no, vfunc_no, phys_addr, addr, size);
	/* [한국어] 매핑 실패를 알린다. */
	if (ret)
		dev_err(&ntb->epf->epc->dev,
			"Failed to map memory window %d address\n", mw);
	return ret;
}

/**
 * epf_ntb_teardown_mw() - Teardown the configured OB ATU
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 * @mw: Index of the memory window (either 0, 1, 2 or 3)
 *
 * Teardown the configured OB ATU configured in epf_ntb_configure_mw() using
 * pci_epc_unmap_addr()
 */
/* [한국어]
 * epf_ntb_teardown_mw - configure_mw 가 건 아웃바운드 매핑을 걷는다
 *
 * @ntb: 함수 상태
 * @mw: 창 번호(0~3)
 * @return: 없음. 호출자는 항상 성공으로 보고한다.
 *
 * 왜 필요한가: 매핑을 걷지 않으면 컨트롤러의 아웃바운드 ATU 항목이
 * 새어 나가 다음 설정이 실패할 수 있다.
 *
 * 동작 단계: 설정 때와 같은 물리 주소(vpci_mw_phy[mw])를 그대로 넘긴다.
 * 컨트롤러는 이 주소로 어느 항목을 걷을지 찾으므로 값이 같아야 한다.
 *
 * 실행 컨텍스트: cmd_handler 워크(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 호출 체인:
 *   epf_ntb_cmd_handler() → [epf_ntb_teardown_mw] → pci_epc_unmap_addr()
 */
static void epf_ntb_teardown_mw(struct epf_ntb *ntb, u32 mw)
{
	pci_epc_unmap_addr(ntb->epf->epc,
			   ntb->epf->func_no,
			   ntb->epf->vfunc_no,
			   ntb->vpci_mw_phy[mw]);
}

/**
 * epf_ntb_cmd_handler() - Handle commands provided by the NTB HOST
 * @work: work_struct for the epf_ntb_epc
 *
 * Workqueue function that gets invoked for the two epf_ntb_epc
 * periodically (once every 5ms) to see if it has received any commands
 * from NTB HOST. The HOST can send commands to configure doorbell or
 * configure memory window or to update link status.
 */
/* [한국어]
 * epf_ntb_cmd_handler - 도어벨을 폴링하고 호스트 명령을 처리하는 주기 워크
 *
 * @work: 함수 상태 안에 박혀 있는 delayed_work
 * @return: 없음
 *
 * 왜 필요한가: 이 워크는 두 가지 일을 겸한다.
 *   (1) MSI 도어벨이 서지 않은 경우, 호스트가 도어벨 BAR 배열에 써 넣은
 *       값을 읽어 NTB 도어벨 사건으로 바꾼다.
 *   (2) 호스트가 공유 레지스터에 써 넣은 명령을 읽어 처리한다.
 * 호스트에서 이쪽으로 오는 통지 경로가 (MSI 도어벨을 제외하면) 없기
 * 때문에 폴링 외에 방법이 없다.
 *
 * 동작 단계:
 *   (1) msi_doorbell 이 false 일 때만 epf_db[1..db_count-1] 을 훑어
 *       0 이 아닌 칸을 도어벨 사건으로 바꾸고 칸을 0 으로 지운다.
 *   (2) command 를 읽고 0 이면 재예약만 한다.
 *   (3) argument 를 읽은 뒤 두 필드를 지워 "받았다" 고 알린다.
 *   (4) 명령별 처리를 하고 command_status 에 결과를 적는다.
 *   (5) MSI 도어벨이면 500ms, 아니면 5ms 뒤의 자신을 다시 큐에 넣는다.
 *
 * 실행 컨텍스트: kpcintb 작업 큐의 워커 스레드(프로세스 컨텍스트).
 * EPF 하나에 인스턴스가 하나뿐이라 자기 자신과 겹치지 않는다.
 * 다만 atomic64 도어벨 비트맵은 인터럽트 문맥의 도어벨 핸들러와 공유한다.
 *
 * 호출 체인:
 *   워크큐 워커 → [epf_ntb_cmd_handler]
 *     → ntb_db_event() / epf_ntb_configure_mw() / epf_ntb_teardown_mw()
 *     → epf_ntb_link_up()
 */
static void epf_ntb_cmd_handler(struct work_struct *work)
{
	/* [한국어] 호스트와 공유하는 레지스터 영역. */
	struct epf_ntb_ctrl *ctrl;
	/* [한국어] 호스트가 써 넣은 명령과 인자. */
	u32 command, argument;
	/* [한국어] container_of 로 되찾을 함수 상태. */
	struct epf_ntb *ntb;
	/* [한국어] 로그용 device 포인터. */
	struct device *dev;
	/* [한국어] 하위 호출의 실패를 담는다. */
	int ret;
	/* [한국어] 도어벨 폴링 순회 인덱스. */
	int i;

	/* [한국어] delayed_work 가 struct epf_ntb 안에 박혀 있으므로 container_of 로
	 * 구조체 전체를 되찾는다. 이 워크는 커널 워커 스레드(프로세스 컨텍스트)
	 * 에서 돌며, EPF 하나에 인스턴스가 하나뿐이라 자기 자신과 겹치지 않는다. */
	ntb = container_of(work, struct epf_ntb, cmd_handler.work);

	/* [한국어] MSI 도어벨이 서지 않았을 때만 도어벨 배열을 폴링한다.
	 * MSI 가 섰다면 epf_ntb_doorbell_handler 가 즉시 처리하므로 여기서는
	 * 조건이 거짓이 되어 루프를 통째로 건너뛴다.
	 * 인덱스가 1 부터인 것은 0번 도어벨을 쓰지 않기 때문이다. */
	for (i = 1; i < ntb->db_count && !ntb->msi_doorbell; i++) {
		/* [한국어] 호스트가 이 칸에 0 이 아닌 값을 썼는가. */
		if (ntb->epf_db[i]) {
			/* [한국어] 밀린 도어벨 비트맵에 비트를 세운다. i - 1 인 것은 NTB 클라이언트
			 * 관점의 번호로 바꾸기 위해서다. */
			atomic64_or(1 << (i - 1), &ntb->db);
			/* [한국어] NTB 코어에 도어벨 사건을 알린다. */
			ntb_db_event(&ntb->ntb, i);
			/* [한국어] 칸을 0 으로 지워 다음 울림을 받을 준비를 한다.
			 * 호스트가 다시 쓰기 전까지는 이 칸이 0 으로 남는다. */
			ntb->epf_db[i] = 0;
		}
	}

	/* [한국어] 공유 레지스터. */
	ctrl = ntb->reg;
	/* [한국어] 명령 필드를 읽는다. */
	command = ctrl->command;
	/* [한국어] 0 이면 새 명령이 없으므로 재예약만 하고 물러난다. */
	if (!command)
		goto reset_handler;
	/* [한국어] 명령의 인자를 읽는다. 반드시 명령을 지우기 전에 읽어야 한다. */
	argument = ctrl->argument;

	/* [한국어] 명령을 지워 "받았다" 고 알린다. */
	ctrl->command = 0;
	/* [한국어] 인자도 함께 지운다. */
	ctrl->argument = 0;

	/* [한국어] 같은 포인터를 다시 대입한다. 상류 코드 그대로 둔다. */
	ctrl = ntb->reg;
	/* [한국어] 로그 대상. */
	dev = &ntb->epf->dev;

	/* [한국어] 명령 코드로 분기한다. 코드 값은 이 파일 위쪽 COMMAND_ 매크로에
	 * 정의되어 있고 호스트 쪽 드라이버와 맞춰야 하는 약속이다. */
	switch (command) {
	/* [한국어] 도어벨 설정 요청. */
	case COMMAND_CONFIGURE_DOORBELL:
		/* [한국어] vntb 는 도어벨을 bind 시점에 이미 만들어 두므로 여기서 할 일이 없다.
		 * 형제 파일 pci-epf-ntb.c 가 이 명령에서 실제 매핑을 거는 것과 대비된다. */
		ctrl->command_status = COMMAND_STATUS_OK;
		break;
	/* [한국어] 성공만 보고한다. */
	case COMMAND_TEARDOWN_DOORBELL:
		ctrl->command_status = COMMAND_STATUS_OK;
		break;
	/* [한국어] 도어벨 해제 요청. 역시 할 일이 없다. */
	case COMMAND_CONFIGURE_MW:
		ret = epf_ntb_configure_mw(ntb, argument);
		/* [한국어] 성공만 보고한다. */
		if (ret < 0)
			ctrl->command_status = COMMAND_STATUS_ERROR;
		else
			/* [한국어] 메모리 윈도우 설정 요청. 인자가 창 번호다. */
			ctrl->command_status = COMMAND_STATUS_OK;
		break;
	/* [한국어] 호스트가 ctrl->addr/size 에 미리 써 둔 PCI 주소로 창을 건다. */
	case COMMAND_TEARDOWN_MW:
		epf_ntb_teardown_mw(ntb, argument);
		/* [한국어] 실패를 호스트에게 알린다. */
		ctrl->command_status = COMMAND_STATUS_OK;
		break;
	/* [한국어] 성공/실패를 구분해 보고한다. */
	case COMMAND_LINK_UP:
		ntb->linkup = true;
		/* [한국어] 메모리 윈도우 해제 요청. */
		ret = epf_ntb_link_up(ntb, true);
		/* [한국어] 아웃바운드 매핑을 걷는다. */
		if (ret < 0)
			/* [한국어] 항상 성공으로 보고한다. */
			ctrl->command_status = COMMAND_STATUS_ERROR;
		else
			/* [한국어] 링크 올림 요청. 호스트 쪽 NTB 드라이버가 링크를 올린 결과다. */
			ctrl->command_status = COMMAND_STATUS_OK;
		goto reset_handler;
	/* [한국어] 링크 상태를 올린다. */
	case COMMAND_LINK_DOWN:
		ntb->linkup = false;
		/* [한국어] 공유 레지스터의 link_status 를 갱신하고 로컬 NTB 코어에 사건을 알린다.
		 * ntb 쪽과 달리 상대가 준비됐는지 기다릴 필요가 없다 — 이쪽 상대는
		 * 같은 커널 안의 가상 NTB 디바이스이기 때문이다. */
		ret = epf_ntb_link_up(ntb, false);
		/* [한국어] 실패를 알린다. */
		if (ret < 0)
			/* [한국어] 성공/실패를 구분해 보고한다. */
			ctrl->command_status = COMMAND_STATUS_ERROR;
		else
			/* [한국어] break 가 아니라 goto 다. 아래를 지나지 않고 곧장 재예약으로 간다. */
			ctrl->command_status = COMMAND_STATUS_OK;
		break;
	/* [한국어] 링크 내림 요청. */
	default:
		/* [한국어] 링크 상태를 내린다. */
		dev_err(dev, "UNKNOWN command: %d\n", command);
		/* [한국어] 공유 레지스터를 갱신하고 NTB 코어에 알린다. */
		break;
	}

/* [한국어] 약속에 없는 명령 코드. 호스트 쪽 드라이버와 버전이 어긋난 경우다. */
reset_handler:
	/* [한국어] 어떤 코드가 왔는지 남긴다. */
	queue_delayed_work(kpcintb_workqueue, &ntb->cmd_handler,
			   ntb->msi_doorbell ? msecs_to_jiffies(500) : msecs_to_jiffies(5));
}

/* [한국어] 명령이 없었을 때와 처리를 마친 뒤 모두 이곳으로 온다. */
/* [한국어]
 * epf_ntb_doorbell_handler - 호스트가 울린 MSI 도어벨을 받는 인터럽트 핸들러
 *
 * @irq: 발생한 인터럽트 번호
 * @data: request_irq 에 넘겨 둔 함수 상태
 * @return: 항상 IRQ_HANDLED
 *
 * 왜 필요한가: MSI 도어벨 방식에서는 호스트가 도어벨 BAR 에 쓴 값이
 * SoC 의 MSI 컨트롤러에 도달해 실제 인터럽트가 된다. 그 인터럽트를 받아
 * NTB 도어벨 사건으로 바꾸는 것이 이 함수다. 폴링 방식보다 지연이
 * 훨씬 짧아 cmd_handler 의 주기를 500ms 로 늦출 수 있게 해 준다.
 *
 * 동작 단계: 도어벨 1번부터 훑어 이 irq 와 짝인 항목을 찾고,
 * 비트맵에 (i - 1)번 비트를 원자적으로 세운 뒤 ntb_db_event() 를 부른다.
 * 인덱스가 1 부터인 것은 0번 도어벨을 쓰지 않기 때문이다.
 *
 * 실행 컨텍스트: 하드웨어 인터럽트 문맥. 잠들 수 없다.
 * 그래서 비트맵 갱신에 락 대신 atomic64_or 를 쓴다 —
 * vntb_epf_db_clear() 가 다른 문맥에서 같은 변수를 지울 수 있기 때문이다.
 *
 * 호출 체인:
 *   플랫폼 MSI 인터럽트 → [epf_ntb_doorbell_handler] → ntb_db_event()
 */
static irqreturn_t epf_ntb_doorbell_handler(int irq, void *data)
/* [한국어] 스스로를 다시 큐에 넣어 폴링 루프를 잇는다.
 * MSI 도어벨이 서 있으면 도어벨 폴링이 필요 없으므로 주기를 500ms 로
 * 늦추고, 아니면 5ms 로 자주 돈다 — 명령 처리 지연과 CPU 사용의 절충이다. */
{
	/* [한국어] request_irq 에 넘겨 둔 함수 상태. 인터럽트 문맥에서 이 포인터로
	 * 어느 EPF 의 도어벨인지 알아낸다. */
	struct epf_ntb *ntb = data;
	/* [한국어] 도어벨 순회 인덱스. */
	int i;

	/* [한국어] 어느 도어벨의 IRQ 인지 찾는다. 인덱스가 1 부터 시작하는 점에 주의 —
	 * 0번 도어벨은 쓰지 않고 1번부터 NTB 도어벨로 삼는다. */
	for (i = 1; i < ntb->db_count; i++)
		/* [한국어] 이 인터럽트 번호와 일치하는 도어벨을 찾는다.
		 * 벡터마다 따로 request_irq 했으므로 사실 한 번에 하나만 맞는다. */
		if (irq == ntb->epf->db_msg[i].virq) {
			/* [한국어] 밀린 도어벨 비트맵에 이 비트를 원자적으로 더한다.
			 * i - 1 인 것은 NTB 클라이언트에게는 도어벨 0 부터 보이기 때문이다.
			 * atomic64_or 를 쓰는 이유는 vntb_epf_db_clear() 가 다른 문맥에서
			 * 같은 변수를 지울 수 있어 읽기-수정-쓰기가 겹치기 때문이다. */
			atomic64_or(1 << (i - 1), &ntb->db);
			/* [한국어] NTB 코어에 도어벨 사건을 알린다. 이 호출이 NTB 클라이언트
			 * (ntb_transport 등)의 처리 경로를 깨운다.
			 * 인터럽트 문맥에서 불리므로 그쪽 구현이 잠들지 않아야 한다. */
			ntb_db_event(&ntb->ntb, i);
		}

	/* [한국어] 이 인터럽트는 이 핸들러가 처리했다고 알린다. */
	return IRQ_HANDLED;
}

/**
 * epf_ntb_config_sspad_bar_clear() - Clear Config + Self scratchpad BAR
 * @ntb: EPC associated with one of the HOST which holds peer's outbound
 *	 address.
 *
 * Clear BAR0 of EP CONTROLLER 1 which contains the HOST1's config and
 * self scratchpad region (removes inbound ATU configuration). While BAR0 is
 * the default self scratchpad BAR, an NTB could have other BARs for self
 * scratchpad (because of reserved BARs). This function can get the exact BAR
 * used for self scratchpad from epf_ntb_bar[BAR_CONFIG].
 *
 * Please note the self scratchpad region and config region is combined to
 * a single region and mapped using the same BAR. Also note VHOST's peer
 * scratchpad is HOST's self scratchpad.
 *
 * Returns: void
 */
/* [한국어]
 * epf_ntb_config_sspad_bar_clear - 제어 + 스크래치패드 BAR 의 인바운드 매핑을 지운다
 *
 * @ntb: 함수 상태
 * @return: 없음
 *
 * 왜 필요한가: 이 BAR 를 걷어야 호스트가 더 이상 공유 레지스터에 명령을
 * 쓸 수 없다. 메모리 자체는 살아 있고 epf_ntb_config_spad_bar_free() 가
 * 따로 해제한다 — "길을 먼저 끊고 그 다음 메모리를 돌려준다" 는 순서를
 * 지키기 위한 분리다.
 *
 * 실행 컨텍스트: bind 실패 경로와 unbind 경로(프로세스 컨텍스트).
 * pci_epc_clear_bar() 안에서 epc->lock 을 잡는다.
 *
 * 호출 체인:
 *   epf_ntb_epc_cleanup() → [epf_ntb_config_sspad_bar_clear] → pci_epc_clear_bar()
 */
static void epf_ntb_config_sspad_bar_clear(struct epf_ntb *ntb)
{
	/* [한국어] 걷을 BAR 의 서술자. */
	struct pci_epf_bar *epf_bar;
	/* [한국어] 제어 영역이 놓인 BAR 번호. */
	enum pci_barno barno;

	/* [한국어] 제어 + 스크래치패드 BAR 번호. */
	barno = ntb->epf_ntb_bar[BAR_CONFIG];
	/* [한국어] 그 BAR 의 서술자. */
	epf_bar = &ntb->epf->bar[barno];

	/* [한국어] 인바운드 ATU 를 지운다. 메모리 자체는 남아 있고
	 * epf_ntb_config_spad_bar_free() 가 따로 해제한다. */
	pci_epc_clear_bar(ntb->epf->epc, ntb->epf->func_no, ntb->epf->vfunc_no, epf_bar);
}

/**
 * epf_ntb_config_sspad_bar_set() - Set Config + Self scratchpad BAR
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 *
 * Map BAR0 of EP CONTROLLER which contains the VHOST's config and
 * self scratchpad region.
 *
 * Please note the self scratchpad region and config region is combined to
 * a single region and mapped using the same BAR.
 *
 * Returns: Zero for success, or an error code in case of failure
 */
/* [한국어]
 * epf_ntb_config_sspad_bar_set - 제어 + 스크래치패드 영역을 BAR 로 노출한다
 *
 * @ntb: 함수 상태
 * @return: 0 성공, 그 밖에는 pci_epc_set_bar() 의 실패 코드
 *
 * 왜 필요한가: 이 BAR 가 열려야 호스트가 명령을 쓰고 스크래치패드를
 * 주고받을 수 있다. 그래서 EPC 초기화의 첫 단계다.
 *
 * 동작 단계: 물리 주소·크기·플래그는 pci_epf_alloc_space() 가 이미
 * 채워 두었으므로, 그 서술자를 컨트롤러에 넘겨 인바운드 ATU 를 건다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 호출 체인:
 *   epf_ntb_epc_init() → [epf_ntb_config_sspad_bar_set] → pci_epc_set_bar()
 */
static int epf_ntb_config_sspad_bar_set(struct epf_ntb *ntb)
{
	/* [한국어] 걸어 줄 BAR 서술자. */
	struct pci_epf_bar *epf_bar;
	/* [한국어] 제어 영역이 놓인 BAR 번호. */
	enum pci_barno barno;
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] 로그용 device 포인터. */
	struct device *dev;
	/* [한국어] BAR 설정 실패를 담는다. */
	int ret;

	/* [한국어] 로그 대상. */
	dev = &ntb->epf->dev;
	/* [한국어] 이 EPF 의 물리 함수 번호. */
	func_no = ntb->epf->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb->epf->vfunc_no;
	/* [한국어] 제어 BAR 번호. */
	barno = ntb->epf_ntb_bar[BAR_CONFIG];
	/* [한국어] 그 BAR 의 서술자. pci_epf_alloc_space 가 이미 물리 주소·크기·플래그를 채웠다. */
	epf_bar = &ntb->epf->bar[barno];

	/* [한국어] 인바운드 ATU 를 건다. 이 순간부터 호스트가 이 BAR 로 명령을 쓸 수 있다. */
	ret = pci_epc_set_bar(ntb->epf->epc, func_no, vfunc_no, epf_bar);
	/* [한국어] 실패하면 호스트와 대화할 통로가 없다. */
	if (ret) {
		/* [한국어] 상류 메시지에 줄바꿈이 빠져 있고 오타(inft)가 있다. 상류 코드 그대로 둔다. */
		dev_err(dev, "inft: Config/Status/SPAD BAR set failed\n");
		return ret;
	}
	return 0;
}

/**
 * epf_ntb_config_spad_bar_free() - Free the physical memory associated with
 *   config + scratchpad region
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 */
/* [한국어]
 * epf_ntb_config_spad_bar_free - 제어 + 스크래치패드 메모리를 해제한다
 *
 * @ntb: 함수 상태
 * @return: 없음
 *
 * 왜 필요한가: 이 메모리는 dma_alloc_coherent 로 잡힌 것이라 반드시
 * 짝이 되는 해제가 필요하다. devm_ 이 아니므로 자동으로 풀리지 않는다.
 *
 * 실행 컨텍스트: bind 실패 경로와 unbind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_bind() 실패 경로 또는 epf_ntb_unbind()
 *     → [epf_ntb_config_spad_bar_free] → pci_epf_free_space()
 */
static void epf_ntb_config_spad_bar_free(struct epf_ntb *ntb)
{
	/* [한국어] 해제할 BAR 번호. */
	enum pci_barno barno;

	/* [한국어] 제어 + 스크래치패드가 놓인 BAR 번호. */
	barno = ntb->epf_ntb_bar[BAR_CONFIG];
	/* [한국어] dma_free_coherent 로 메모리를 돌려주고 BAR 서술자를 지운다.
	 * 마지막 인자 0 은 PRIMARY_INTERFACE 다. */
	pci_epf_free_space(ntb->epf, ntb->reg, barno, 0);
}

/**
 * epf_ntb_config_spad_bar_alloc() - Allocate memory for config + scratchpad
 *   region
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 *
 * Allocate the Local Memory mentioned in the above diagram. The size of
 * CONFIG REGION is sizeof(struct epf_ntb_ctrl) and size of SCRATCHPAD REGION
 * is obtained from "spad-count" configfs entry.
 *
 * Returns: Zero for success, or an error code in case of failure
 */
/* [한국어]
 * epf_ntb_config_spad_bar_alloc - 제어 영역과 스크래치패드 두 절반의 메모리를 잡는다
 *
 * @ntb: 함수 상태
 * @return: 0 성공, -ENOMEM 할당 실패
 *
 * 왜 필요한가: vntb 의 스크래치패드는 한 영역 안에 두 절반으로 놓인다.
 * 앞 절반이 상대(호스트) 몫, 뒤 절반이 내 몫이다. 파일 상단 그림의
 * 좌우 교차가 이 배치이며, 같은 메모리를 양쪽이 서로 반대 절반으로
 * 읽고 쓰기 때문에 값이 오간다.
 * 형제 파일 pci-epf-ntb.c 가 별도 BAR 로 상대 영역을 겹쳐 노출하는 것과
 * 근본적으로 다른 방식이다 — 그쪽은 EPC 가 둘이라 물리 메모리도 둘이지만,
 * 여기서는 하나뿐이기 때문이다.
 *
 * 동작 단계: 제어 영역 크기를 4바이트로 올리고, 스크래치패드를 두 벌
 * 잡고, pci_epf_alloc_space() 로 합친 크기를 할당한 뒤, 공유 레지스터의
 * 오프셋·개수·창 수·도어벨 정보를 초기화한다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트). 안쪽에서
 * dma_alloc_coherent 를 부르므로 GFP_KERNEL 로 잠들 수 있다.
 *
 * 호출 체인:
 *   epf_ntb_bind() → [epf_ntb_config_spad_bar_alloc] → pci_epf_alloc_space()
 */
static int epf_ntb_config_spad_bar_alloc(struct epf_ntb *ntb)
{
	/* [한국어] 제어 영역을 놓을 BAR 번호. */
	enum pci_barno barno;
	/* [한국어] 할당한 메모리를 공유 레지스터로 보기 위한 포인터. */
	struct epf_ntb_ctrl *ctrl;
	/* [한국어] 스크래치패드 영역과 제어 영역의 크기. */
	u32 spad_size, ctrl_size;
	/* [한국어] EPF 디바이스. */
	struct pci_epf *epf = ntb->epf;
	/* [한국어] 로그용 device 포인터. */
	struct device *dev = &epf->dev;
	/* [한국어] 스크래치패드 워드 개수. */
	u32 spad_count;
	/* [한국어] 할당된 메모리의 커널 가상 주소. */
	void *base;
	/* [한국어] 도어벨 초기화 순회 인덱스. */
	int i;
	/* [한국어] 컨트롤러 능력표. pci_epf_alloc_space 가 크기를 다듬을 때 쓴다.
	 * 선언과 초기화를 함수 중간에서 하는 형태라 위쪽 선언들과 섞여 있다. */
	const struct pci_epc_features *epc_features = pci_epc_get_features(epf->epc,
								epf->func_no,
								epf->vfunc_no);
	/* [한국어] 제어 + 스크래치패드를 놓을 BAR 번호. */
	barno = ntb->epf_ntb_bar[BAR_CONFIG];
	/* [한국어] 사용자가 정한 스크래치패드 워드 개수. */
	spad_count = ntb->spad_count;

	/* [한국어] 제어 영역 크기를 4바이트 경계로 올린다. 그래야 뒤따르는
	 * 스크래치패드가 워드 정렬을 유지한다. */
	ctrl_size = ALIGN(sizeof(struct epf_ntb_ctrl), sizeof(u32));
	/* [한국어] 스크래치패드를 두 벌 잡는다. 앞 절반이 상대(호스트) 몫,
	 * 뒤 절반이 내 몫이다. 파일 상단 그림의 좌우 교차가 이 배치다 —
	 * 같은 메모리를 양쪽이 서로 반대 절반으로 읽고 쓴다.
	 * ntb 쪽이 별도 BAR 로 겹쳐 노출하는 것과 달리, vntb 는 한 영역 안에
	 * 두 절반을 나란히 둔다. */
	spad_size = 2 * spad_count * sizeof(u32);

	/* [한국어] 제어 영역과 스크래치패드 두 벌을 합친 크기로 메모리를 잡는다.
	 * 마지막 인자 0 은 PRIMARY_INTERFACE 로, 이 드라이버는 주 인터페이스
	 * 하나만 쓴다. */
	base = pci_epf_alloc_space(epf, ctrl_size + spad_size,
				   barno, epc_features, 0);
	/* [한국어] 할당 실패. */
	if (!base) {
		/* [한국어] 원인을 남긴다. */
		dev_err(dev, "Config/Status/SPAD alloc region fail\n");
		return -ENOMEM;
	}

	/* [한국어] 이 영역의 시작을 공유 레지스터로 기억한다. */
	ntb->reg = base;

	/* [한국어] 구조체 포인터로 다시 본다. */
	ctrl = ntb->reg;
	/* [한국어] 스크래치패드가 시작되는 BAR 내 오프셋을 호스트에게 알린다.
	 * vntb_epf_spad_read/write 도 이 값을 기준으로 자리를 계산한다. */
	ctrl->spad_offset = ctrl_size;

	/* [한국어] 쓸 수 있는 워드 개수(한 절반 기준). */
	ctrl->spad_count = spad_count;
	/* [한국어] 쓸 수 있는 창 개수. */
	ctrl->num_mws = ntb->num_mws;
	/* [한국어] 정리와 접근 계산에 쓰도록 전체 스크래치패드 크기를 보관한다. */
	ntb->spad_size = spad_size;

	/* [한국어] 도어벨 한 칸을 4바이트로 둔다. MSI 도어벨이 성공하면
	 * epf_ntb_db_bar_init_msi_doorbell() 이 이 값을 0 으로 덮어써
	 * "오프셋을 그대로 쓰라" 고 알린다. */
	ctrl->db_entry_size = sizeof(u32);

	/* [한국어] 폴링 방식의 기본 도어벨 정보를 채운다. */
	for (i = 0; i < ntb->db_count; i++) {
		/* [한국어] 도어벨 i 를 울리는 값으로 1 + i 를 쓴다. 0 이 아닌 값이면
		 * cmd_handler 의 검사에 걸리므로 어떤 값이든 되지만,
		 * 호스트가 어느 도어벨인지 구분할 수 있게 서로 다른 값을 준다. */
		ntb->reg->db_data[i] = 1 + i;
		/* [한국어] 오프셋은 모두 0 이다. 호스트는 db_entry_size 로 칸을 계산한다. */
		ntb->reg->db_offset[i] = 0;
	}

	return 0;
}

/**
 * epf_ntb_configure_interrupt() - Configure MSI/MSI-X capability
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 *
 * Configure MSI/MSI-X capability for each interface with number of
 * interrupts equal to "db_count" configfs entry.
 *
 * Returns: Zero for success, or an error code in case of failure
 */
/* [한국어]
 * epf_ntb_configure_interrupt - MSI 능력을 설정한다
 *
 * @ntb: 함수 상태
 * @return: 0 성공, -EINVAL 이면 MSI/MSI-X 를 둘 다 지원하지 않거나
 *          도어벨 개수가 한계를 넘음, 그 밖에는 컨트롤러 설정 실패 코드
 *
 * 왜 필요한가: 도어벨의 한쪽 방향(VHOST → HOST)은 결국 호스트에게 MSI 를
 * 쏘는 일이다(vntb_epf_peer_db_set). 호스트가 열거할 때 벡터를 할당하도록
 * MSI 능력 레지스터의 개수를 미리 선언해 두어야 한다.
 *
 * 주의: 능력 확인은 MSI-X 도 함께 보지만 실제로는 MSI 만 설정한다.
 * 또 벡터 개수를 db_count 가 아니라 16 으로 박아 넣는다.
 * 상류 코드 그대로 둔다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 호출 체인:
 *   epf_ntb_epc_init() → [epf_ntb_configure_interrupt] → pci_epc_set_msi()
 */
static int epf_ntb_configure_interrupt(struct epf_ntb *ntb)
{
	/* [한국어] 컨트롤러 능력표. MSI/MSI-X 지원 여부를 여기서 읽는다. */
	const struct pci_epc_features *epc_features;
	/* [한국어] 로그용 device 포인터. */
	struct device *dev;
	/* [한국어] 설정할 인터럽트 개수. */
	u32 db_count;
	/* [한국어] 컨트롤러 설정의 실패를 담는다. */
	int ret;

	/* [한국어] 로그 대상. */
	dev = &ntb->epf->dev;

	/* [한국어] 이 컨트롤러의 능력표를 읽는다. */
	epc_features = pci_epc_get_features(ntb->epf->epc, ntb->epf->func_no, ntb->epf->vfunc_no);

	/* [한국어] MSI 도 MSI-X 도 없으면 도어벨을 만들 수 없다 — 도어벨의 한쪽
	 * 방향(VHOST → HOST)이 결국 MSI 를 쏘는 일이기 때문이다. */
	if (!(epc_features->msix_capable || epc_features->msi_capable)) {
		/* [한국어] 설정 오류이므로 dev_err 로 분명히 남긴다. */
		dev_err(dev, "MSI or MSI-X is required for doorbell\n");
		return -EINVAL;
	}

	/* [한국어] 사용자가 configfs 로 정한 도어벨 개수. */
	db_count = ntb->db_count;
	/* [한국어] db_data[]/db_offset[] 배열이 MAX_DB_COUNT(32) 칸이라 그 이상은
	 * 배열 밖을 건드리게 된다. */
	if (db_count > MAX_DB_COUNT) {
		/* [한국어] 허용 한계를 사용자에게 알린다. */
		dev_err(dev, "DB count cannot be more than %d\n", MAX_DB_COUNT);
		return -EINVAL;
	}

	/* [한국어] 검사를 통과한 값을 되쓴다(값 자체는 그대로다). 상류 코드 그대로 둔다. */
	ntb->db_count = db_count;

	/* [한국어] MSI 를 지원하면 능력 레지스터의 개수 필드를 설정한다. */
	if (epc_features->msi_capable) {
		/* [한국어] 벡터 개수를 db_count 가 아니라 16 으로 박아 넣는다.
		 * vntb_epf_peer_db_set() 이 ffs 값에 2 를 더한 번호로 인터럽트를 쏘므로
		 * 도어벨 개수보다 넉넉한 벡터가 필요하기 때문으로 읽히나,
		 * 이 트리의 코드만으로 그 의도를 확정할 근거는 찾지 못했다.
		 * 상류 코드 그대로 둔다. */
		ret = pci_epc_set_msi(ntb->epf->epc,
				      ntb->epf->func_no,
				      ntb->epf->vfunc_no,
				      16);
		/* [한국어] MSI 설정 실패는 도어벨의 한쪽 방향이 죽는다는 뜻이다. */
		if (ret) {
			/* [한국어] 원인을 남긴다. */
			dev_err(dev, "MSI configuration failed\n");
			return ret;
		}
	}

	/* [한국어] MSI-X 는 여기서 설정하지 않는다. 위에서 지원 여부만 확인하고
	 * 실제로는 MSI 만 설정한다 — 이 드라이버의 도어벨은 MSI 를 전제한다. */
	return 0;
}

/* [한국어]
 * epf_ntb_db_bar_init_msi_doorbell - 플랫폼 MSI 주소를 BAR 로 노출해 인터럽트 기반 도어벨을 만든다
 *
 * @ntb: 함수 상태
 * @db_bar: 도어벨에 배정된 BAR 서술자
 * @epc_features: 컨트롤러 능력표
 * @barno: 그 BAR 번호
 * @return: 0 성공, 그 밖에는 실패 코드(호출자가 폴링 방식으로 물러선다)
 *
 * 왜 필요한가: 폴링 도어벨은 최대 5ms 의 지연이 있다. 대신 SoC 자신의
 * 플랫폼 MSI 컨트롤러 주소를 BAR 로 노출해 두면, 호스트가 그 BAR 에 쓴
 * 순간 실제 인터럽트가 발생해 지연이 사라진다. 이 함수가 그 구조를 만든다.
 *
 * 동작 단계:
 *   (1) 도어벨 개수만큼 플랫폼 MSI 벡터를 잡는다. 이 API 는 주소가 변하지
 *       않는(immutable) MSI 도메인만 받아들인다 — 호스트에게 알려 준 주소가
 *       나중에 바뀌면 초인종이 울리지 않기 때문이다.
 *   (2) 벡터마다 request_irq 로 핸들러를 건다.
 *   (3) 모든 벡터의 목적지 주소 중 최저·최고를 구해, 그 범위를 덮는 BAR 를
 *       만든다(pci_epf_assign_bar_space). 메모리를 새로 잡는 것이 아니라
 *       이미 존재하는 하드웨어 주소를 BAR 로 노출하는 것이다.
 *   (4) 벡터마다 BAR 시작점에서의 오프셋과 데이터를 공유 레지스터에 적어
 *       호스트에게 "어디에 무엇을 쓰면 되는지" 를 알린다.
 *   (5) db_entry_size 를 0 으로 두어 "칸 크기로 계산하지 말고 오프셋을
 *       그대로 쓰라" 고 표시하고, msi_doorbell 을 세운다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트).
 *
 * 에러 경로: err_free_irq 에서 지금까지 건 IRQ 를 역순으로 반납하고
 * 플랫폼 MSI 벡터도 되돌린다. 호출자는 이 실패를 치명적으로 보지 않고
 * 폴링 방식으로 물러선다.
 *
 * 호출 체인:
 *   epf_ntb_db_bar_init() → [epf_ntb_db_bar_init_msi_doorbell]
 *     → pci_epf_alloc_doorbell() → request_irq()
 *     → pci_epf_assign_bar_space() → pci_epc_set_bar()
 *     → pci_epf_align_inbound_addr()
 */
static int epf_ntb_db_bar_init_msi_doorbell(struct epf_ntb *ntb,
					    struct pci_epf_bar *db_bar,
					    const struct pci_epc_features *epc_features,
					    enum pci_barno barno)
{
	/* [한국어] EPF 디바이스. */
	struct pci_epf *epf = ntb->epf;
	/* [한국어] 모든 도어벨 MSI 주소 중 최저값과 최고값. 이 범위를 덮는 BAR 를 만든다. */
	dma_addr_t low, high;
	/* [한국어] 플랫폼 MSI 메시지(주소와 데이터). */
	struct msi_msg *msg;
	/* [한국어] BAR 로 노출할 크기. */
	size_t sz;
	/* [한국어] 각 단계의 실패를 담는다. */
	int ret;
	/* [한국어] 순회 인덱스와, 실패 시 되돌릴 IRQ 개수를 세는 변수. */
	int i, req;

	/* [한국어] 플랫폼 MSI 도메인에서 도어벨 개수만큼 벡터를 잡는다.
	 * 이 호출이 성공하면 epf->db_msg[i] 에 각 벡터의 목적지 주소와
	 * virq 가 채워진다. 여기서 잡는 것은 PCI MSI 가 아니라 플랫폼 MSI 다 —
	 * 인터럽트를 받는 것이 PCI 장치가 아니라 SoC 자신이기 때문이다. */
	ret = pci_epf_alloc_doorbell(epf,  ntb->db_count);
	/* [한국어] 컨트롤러가 불변 MSI 도메인을 갖고 있지 않으면 여기서 실패한다.
	 * 호출자가 폴링 방식으로 물러선다. */
	if (ret)
		return ret;

	/* [한국어] 벡터마다 핸들러를 건다. req 가 지금까지 성공한 개수를 기억해
	 * 실패 시 그만큼만 되돌릴 수 있게 한다. */
	for (req = 0; req < ntb->db_count; req++) {
		/* [한국어] epf_ntb_doorbell_handler 를 건다. 플래그 0 은 공유하지 않는 전용
		 * 인터럽트라는 뜻이고, 이름은 /proc/interrupts 에 보인다. */
		ret = request_irq(epf->db_msg[req].virq, epf_ntb_doorbell_handler,
				  0, "pci_epf_vntb_db", ntb);

		/* [한국어] 하나라도 실패하면 지금까지 건 것을 모두 되돌린다. */
		if (ret) {
			/* [한국어] 어느 virq 에서 났는지 남긴다. */
			dev_err(&epf->dev,
				"Failed to request doorbell IRQ: %d\n",
				epf->db_msg[req].virq);
			goto err_free_irq;
		}
	}

	/* [한국어] 첫 벡터의 메시지. 아래 최저/최고 계산의 출발점이다. */
	msg = &epf->db_msg[0].msg;

	/* [한국어] 최고값을 0 에서 시작한다. */
	high = 0;
	/* [한국어] 최저값은 첫 벡터의 주소로 시작한다. 상위 32비트와 하위 32비트를
	 * 합쳐 64비트 주소를 만든다. */
	low = (u64)msg->address_hi << 32 | msg->address_lo;

	/* [한국어] 모든 벡터의 주소를 훑어 최저·최고를 구한다. */
	for (i = 0; i < ntb->db_count; i++) {
		/* [한국어] 이 벡터의 메시지. */
		struct msi_msg *msg = &epf->db_msg[i].msg;
		/* [한국어] 64비트 주소로 합친다. */
		dma_addr_t addr = (u64)msg->address_hi << 32 | msg->address_lo;

		/* [한국어] 최저값 갱신. */
		low = min(low, addr);
		/* [한국어] 최고값 갱신. */
		high = max(high, addr);
	}

	/* [한국어] BAR 가 덮어야 할 크기. 최고 주소까지 포함해야 하므로 워드 하나를 더한다.
	 * 이렇게 범위를 잡는 이유는, 여러 MSI 벡터의 목적지 주소가 흩어져 있어도
	 * BAR 하나로 전부 덮으면 호스트가 오프셋만 달리해 각 벡터를 울릴 수
	 * 있기 때문이다. */
	sz = high - low + sizeof(u32);

	/* [한국어] 그 범위를 덮는 BAR 를 만든다. 이 함수는 주소를 BAR 크기 경계로
	 * 내림해 시작점을 잡고, 그 시작점에서 범위가 다 들어가는지 확인한다.
	 * 메모리를 새로 잡는 것이 아니라 이미 존재하는 MSI 컨트롤러 주소를
	 * BAR 로 노출하는 것이라 pci_epf_alloc_space 가 아니라 이 함수를 쓴다. */
	ret = pci_epf_assign_bar_space(epf, sz, barno, epc_features, 0, low);
	/* [한국어] 범위가 BAR 하나에 들어가지 않으면 실패한다. */
	if (ret) {
		/* [한국어] 원인을 남긴다. */
		dev_err(&epf->dev, "Failed to assign Doorbell BAR space\n");
		goto err_free_irq;
	}

	/* [한국어] BAR 를 실제로 건다. 이 순간부터 호스트가 이 BAR 에 쓰면 그 쓰기가
	 * SoC 의 MSI 컨트롤러에 도달해 인터럽트가 된다. */
	ret = pci_epc_set_bar(ntb->epf->epc, ntb->epf->func_no,
			      ntb->epf->vfunc_no, db_bar);
	/* [한국어] BAR 설정 실패. */
	if (ret) {
		/* [한국어] 원인을 남긴다. */
		dev_err(&epf->dev, "Failed to set Doorbell BAR\n");
		goto err_free_irq;
	}

	/* [한국어] 이제 호스트에게 "어떤 값을 어디에 쓰면 도어벨 i 가 울리는지" 를 알린다. */
	for (i = 0; i < ntb->db_count; i++) {
		/* [한국어] 이 벡터의 메시지. */
		struct msi_msg *msg = &epf->db_msg[i].msg;
		/* [한국어] BAR 시작점에서의 거리를 담을 변수. */
		dma_addr_t addr;
		/* [한국어] 정렬된 시작 주소를 담을 변수. */
		size_t offset;

		/* [한국어] BAR 크기 경계로 내림한 시작 주소와, 실제 목적지까지의 거리를 구한다.
		 * 이 계산이 필요한 이유는 대부분의 EP 컨트롤러가 BAR 시작 주소의
		 * 하위 비트를 잘라 버리기 때문이다. */
		ret = pci_epf_align_inbound_addr(epf, db_bar->barno,
				((u64)msg->address_hi << 32) | msg->address_lo,
				&addr, &offset);

		/* [한국어] 정렬을 맞출 수 없으면 MSI 도어벨을 포기한다. */
		if (ret) {
			/* [한국어] 플래그를 내리고 정리 경로로 간다. 호출자가 폴링으로 물러선다. */
			ntb->msi_doorbell = false;
			goto err_free_irq;
		}

		/* [한국어] 이 벡터에 실어야 할 데이터. 호스트가 이 값을 쓴다. */
		ntb->reg->db_data[i] = msg->data;
		/* [한국어] BAR 안에서의 오프셋. 호스트는 도어벨 BAR 의 이 지점에 쓴다. */
		ntb->reg->db_offset[i] = offset;
	}

	/* [한국어] db_entry_size 를 0 으로 둔다. 이것이 호스트에게 "칸 크기로 오프셋을
	 * 계산하지 말고 db_offset[i] 를 그대로 써라" 고 알리는 표시다.
	 * 폴링 방식에서는 sizeof(u32) 가 들어간다. */
	ntb->reg->db_entry_size = 0;

	/* [한국어] MSI 도어벨이 성공했음을 기록한다. cmd_handler 가 이 값을 보고
	 * 폴링 주기를 5ms 대신 500ms 로 늘리고 배열 검사를 건너뛴다. */
	ntb->msi_doorbell = true;

	/* [한국어] 여기까지 오면 인터럽트 기반 도어벨이 완성된 상태다. */
	return 0;

/* [한국어] 실패 경로. request_irq 를 성공한 만큼만 되돌린다. */
err_free_irq:
	/* [한국어] req 는 마지막으로 시도한 인덱스이므로 하나 줄여 시작한다.
	 * BAR 설정 단계에서 넘어온 경우 req 는 db_count 와 같아,
	 * 결과적으로 모든 IRQ 를 반납한다. */
	for (req--; req >= 0; req--)
		/* [한국어] 핸들러를 떼어 낸다. */
		free_irq(epf->db_msg[req].virq, ntb);

	/* [한국어] 플랫폼 MSI 벡터 자체를 반납한다. */
	pci_epf_free_doorbell(ntb->epf);
	/* [한국어] 실패 코드를 그대로 올린다. 호출자가 폴링 방식으로 물러선다. */
	return ret;
}

/**
 * epf_ntb_db_bar_init() - Configure Doorbell window BARs
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 *
 * Returns: Zero for success, or an error code in case of failure
 */
/* [한국어]
 * epf_ntb_db_bar_init - 도어벨 BAR 를 만든다. MSI 방식을 먼저 시도하고 안 되면 폴링으로 물러선다
 *
 * @ntb: 함수 상태
 * @return: 0 성공, -ENOMEM 이면 폴링용 메모리 할당 실패, -1 이면 BAR 설정 실패
 *
 * 왜 필요한가: 도어벨은 두 방식 중 하나로 구현된다. 어느 쪽이든 호스트에게
 * 보이는 모습은 BAR 하나로 같고, db_entry_size 와 db_offset 값으로
 * 호스트가 방식을 구분한다. 이 함수가 그 선택을 담당한다.
 *
 * 동작 단계: MSI 도어벨을 시도하고, 실패하면 도어벨 개수 x 4바이트짜리
 * 워드 배열을 잡아 그 BAR 뒤에 건다. 그 배열을 cmd_handler 가 폴링한다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 에러 경로: 폴링 경로의 BAR 설정이 실패하면 방금 잡은 메모리를 되돌리고
 * -1 을 돌려준다(errno 가 아닌 점에 주의). 상류 코드 그대로 둔다.
 *
 * 호출 체인:
 *   epf_ntb_epc_init() → [epf_ntb_db_bar_init]
 *     → epf_ntb_db_bar_init_msi_doorbell() 또는 pci_epf_alloc_space()
 */
static int epf_ntb_db_bar_init(struct epf_ntb *ntb)
{
	/* [한국어] 컨트롤러 능력표. 정렬 요구와 BAR 제약을 여기서 읽는다. */
	const struct pci_epc_features *epc_features;
	/* [한국어] 로그용 device 포인터. */
	struct device *dev = &ntb->epf->dev;
	/* [한국어] 각 경로의 실패를 담는다. */
	int ret;
	/* [한국어] 도어벨 BAR 의 서술자. */
	struct pci_epf_bar *epf_bar;
	/* [한국어] 폴링 방식으로 물러섰을 때 잡을 메모리의 가상 주소. */
	void *mw_addr;
	/* [한국어] 도어벨 BAR 번호. */
	enum pci_barno barno;
	/* [한국어] 폴링 방식에서 필요한 크기 = 도어벨 개수 x 4바이트.
	 * 호스트가 도어벨 i 를 울리려면 이 배열의 i 번째 워드에 값을 쓴다. */
	size_t size = sizeof(u32) * ntb->db_count;

	/* [한국어] 이 컨트롤러의 능력표. */
	epc_features = pci_epc_get_features(ntb->epf->epc,
					    ntb->epf->func_no,
					    ntb->epf->vfunc_no);
	/* [한국어] 도어벨에 배정된 BAR 번호. */
	barno = ntb->epf_ntb_bar[BAR_DB];
	/* [한국어] 그 BAR 의 서술자. */
	epf_bar = &ntb->epf->bar[barno];

	/* [한국어] 먼저 플랫폼 MSI 기반 도어벨을 시도한다. 성공하면 호스트의 쓰기가
	 * 곧바로 SoC 인터럽트가 되어 폴링 없이 즉시 반응한다. */
	ret = epf_ntb_db_bar_init_msi_doorbell(ntb, epf_bar, epc_features, barno);
	/* [한국어] 상류 주석대로, 실패하면 폴링 방식으로 물러선다.
	 * 두 방식은 호스트에게 노출되는 모습(BAR 하나)은 같고,
	 * db_entry_size 와 db_data/db_offset 값으로 구분된다. */
	if (ret) {
		/* fall back to polling mode */
		mw_addr = pci_epf_alloc_space(ntb->epf, size, barno, epc_features, 0);
		/* [한국어] 도어벨 개수만큼의 워드 배열을 잡고 그 BAR 뒤에 건다.
		 * 호스트가 여기에 쓴 값을 cmd_handler 가 5ms 마다 읽는다. */
		if (!mw_addr) {
			/* [한국어] 할당 실패는 도어벨을 만들 수 없다는 뜻이다. */
			dev_err(dev, "Failed to allocate OB address\n");
			return -ENOMEM;
		}

		/* [한국어] 폴링에서 읽을 배열의 시작 주소를 기억한다. */
		ntb->epf_db = mw_addr;

		/* [한국어] BAR 를 건다. 이 순간부터 호스트가 도어벨 배열에 접근할 수 있다. */
		ret = pci_epc_set_bar(ntb->epf->epc, ntb->epf->func_no,
				      ntb->epf->vfunc_no, epf_bar);
		/* [한국어] BAR 설정 실패. */
		if (ret) {
			/* [한국어] 방금 잡은 메모리를 되돌린다. */
			dev_err(dev, "Doorbell BAR set failed\n");
			goto err_alloc_peer_mem;
		}
	}
	/* [한국어] MSI 경로가 성공했으면 ret 가 0 이고, 폴링 경로를 거쳤어도 여기서는
	 * 0 이다. 즉 두 방식 중 하나가 서면 성공이다. */
	return ret;

/* [한국어] 폴링 경로의 BAR 설정 실패용 레이블. */
err_alloc_peer_mem:
	/* [한국어] 방금 잡은 메모리를 해제한다. */
	pci_epf_free_space(ntb->epf, mw_addr, barno, 0);
	/* [한국어] -1 을 돌려준다. errno 가 아니라 -1(-EPERM)이 나가는 점에 주의.
	 * 상류 코드 그대로 둔다. */
	return -1;
}

/* [한국어] 전방 선언. epf_ntb_mw_bar_init() 의 오류 경로가 이 함수를 부르는데
 * 정의가 아래에 있어 먼저 선언해 둔다. */
/* [한국어]
 * epf_ntb_mw_bar_clear - 창 BAR 를 걷고 아웃바운드 공간을 돌려준다
 *
 * @ntb: 함수 상태
 * @num_mws: 정리할 창 개수. 오류 경로에서 부분적으로 만들어진 상태를
 *   되돌릴 수 있도록 개수를 인자로 받는다.
 * @return: 없음
 *
 * 왜 필요한가: BAR 를 먼저 걷고 그 다음 아웃바운드 공간을 돌려주는 순서를
 * 지켜야 한다. 뒤집으면 아직 살아 있는 BAR 가 해제된 공간을 가리킨다.
 *
 * 실행 컨텍스트: bind 실패 경로와 unbind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_mw_bar_init() 실패 경로 또는 epf_ntb_epc_cleanup()
 *     → [epf_ntb_mw_bar_clear] → pci_epc_clear_bar() → pci_epc_mem_free_addr()
 */
static void epf_ntb_mw_bar_clear(struct epf_ntb *ntb, int num_mws);

/**
 * epf_ntb_db_bar_clear() - Clear doorbell BAR and free memory
 *   allocated in peer's outbound address space
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 */
/* [한국어]
 * epf_ntb_db_bar_clear - 도어벨 IRQ 를 반납하고 BAR 와 메모리를 되돌린다
 *
 * @ntb: 함수 상태
 * @return: 없음
 *
 * 왜 필요한가: MSI 방식이었다면 걸어 둔 IRQ 핸들러와 플랫폼 MSI 벡터를
 * 반납해야 하고, 폴링 방식이었다면 잡아 둔 워드 배열을 해제해야 한다.
 * 두 방식을 한 함수에서 다루므로 msi_doorbell 플래그로 갈라진다.
 *
 * 동작 단계: (1) MSI 방식이면 IRQ 를 하나씩 반납한다 — free_irq 는 실행
 * 중인 핸들러가 끝나기를 기다리므로 이후로는 핸들러가 돌지 않는다.
 * (2) 플랫폼 MSI 벡터를 반납한다. (3) 폴링용 메모리를 해제한다.
 * (4) 마지막으로 BAR 의 인바운드 매핑을 지운다.
 *
 * 실행 컨텍스트: bind 실패 경로와 unbind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_epc_cleanup() → [epf_ntb_db_bar_clear]
 *     → free_irq() → pci_epf_free_doorbell() → pci_epc_clear_bar()
 */
static void epf_ntb_db_bar_clear(struct epf_ntb *ntb)
{
	/* [한국어] 걷을 BAR 번호. */
	enum pci_barno barno;

	/* [한국어] MSI 도어벨을 쓰고 있었는가. */
	if (ntb->msi_doorbell) {
		/* [한국어] IRQ 반납 순회 인덱스. */
		int i;

		/* [한국어] 요청했던 도어벨 IRQ 를 하나씩 반납한다. 핸들러가 더 이상 불리지
		 * 않게 하는 것이 먼저다. */
		for (i = 0; i < ntb->db_count; i++)
			/* [한국어] free_irq 는 실행 중인 핸들러가 끝나기를 기다리므로,
			 * 이 시점 이후에는 epf_ntb_doorbell_handler 가 돌지 않는다. */
			free_irq(ntb->epf->db_msg[i].virq, ntb);
	}

	/* [한국어] 플랫폼 MSI 벡터 자체를 반납한다. db_msg 가 NULL 이면 애초에
	 * 할당하지 않은 것이므로 건너뛴다. */
	if (ntb->epf->db_msg)
		pci_epf_free_doorbell(ntb->epf);

	/* [한국어] 도어벨 BAR 번호. */
	barno = ntb->epf_ntb_bar[BAR_DB];
	/* [한국어] 폴링 방식이었다면 여기서 잡아 둔 메모리를 해제한다.
	 * MSI 방식이었다면 ntb->epf_db 가 NULL 이라 아무 것도 하지 않는다.
	 * (pci_epf_free_space 는 주소를 받지만 실제 해제는 BAR 서술자 기준이다.) */
	pci_epf_free_space(ntb->epf, ntb->epf_db, barno, 0);
	/* [한국어] 마지막으로 BAR 의 인바운드 매핑을 지운다. */
	pci_epc_clear_bar(ntb->epf->epc,
			  ntb->epf->func_no,
			  ntb->epf->vfunc_no,
			  &ntb->epf->bar[barno]);
}

/**
 * epf_ntb_mw_bar_init() - Configure Memory window BARs
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 *
 * Returns: Zero for success, or an error code in case of failure
 */
/* [한국어]
 * epf_ntb_mw_bar_init - 메모리 윈도우 BAR 를 선언하고 아웃바운드 창을 잡는다
 *
 * @ntb: 함수 상태
 * @return: 0 성공, -ENOMEM 이면 아웃바운드 공간 부족, 그 밖에는 BAR 설정 실패
 *
 * 왜 필요한가: 창은 두 방향으로 쓰인다. 호스트 → 로컬 방향은 BAR 로
 * 들어와 나중에 vntb_epf_mw_set_trans() 가 지정하는 주소에 닿고,
 * 로컬 → 호스트 방향은 여기서 잡는 아웃바운드 창을 지나간다.
 * 이 함수는 그 두 가지를 한 번에 준비한다.
 *
 * 동작 단계: 창마다
 *   (1) BAR 서술자에 크기와 32/64비트 플래그만 채운다. 물리 주소는 0 이다 —
 *       실제 목적지는 NTB 클라이언트가 나중에 지정하기 때문이다.
 *   (2) 그 상태로 BAR 를 걸어 호스트가 크기를 열거할 수 있게 한다.
 *   (3) 같은 크기의 아웃바운드 창을 컨트롤러 공간에서 잡는다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 에러 경로: err_set_bar 는 방금 건 BAR 를 걷고, err_alloc_mem 은
 * 지금까지 완성된 i 개의 창을 정리한다.
 *
 * 호출 체인:
 *   epf_ntb_epc_init() → [epf_ntb_mw_bar_init]
 *     → pci_epc_set_bar() → pci_epc_mem_alloc_addr()
 */
static int epf_ntb_mw_bar_init(struct epf_ntb *ntb)
{
	/* [한국어] 각 단계의 실패를 담는다. */
	int ret = 0;
	/* [한국어] 창 순회 인덱스. 실패 시 어디까지 갔는지도 이 값이 알려 준다. */
	int i;
	/* [한국어] 이번 창의 크기. */
	u64 size;
	/* [한국어] 이번 창에 배정된 BAR 번호. */
	enum pci_barno barno;
	/* [한국어] 로그용 device 포인터. */
	struct device *dev = &ntb->epf->dev;

	/* [한국어] 사용자가 정한 창 개수만큼 만든다. */
	for (i = 0; i < ntb->num_mws; i++) {
		/* [한국어] 이 창의 크기(configfs 의 mwN 값). */
		size = ntb->mws_size[i];
		/* [한국어] BAR_MW1 부터 세므로 창 0 이 BAR_MW1 이다. */
		barno = ntb->epf_ntb_bar[BAR_MW1 + i];

		/* [한국어] BAR 서술자에 번호를 채운다. */
		ntb->epf->bar[barno].barno = barno;
		/* [한국어] 크기를 채운다. */
		ntb->epf->bar[barno].size = size;
		/* [한국어] 가상 주소는 없다. 이 BAR 뒤에는 우리가 잡은 메모리가 아니라
		 * NTB 클라이언트가 나중에 vntb_epf_mw_set_trans() 로 지정할 주소가 온다. */
		ntb->epf->bar[barno].addr = NULL;
		/* [한국어] 물리 주소도 아직 0 이다. 같은 이유다 — 이 시점에는 크기만 선언해
		 * 호스트가 BAR 를 열거할 수 있게 하고, 실제 목적지는 나중에 붙인다. */
		ntb->epf->bar[barno].phys_addr = 0;
		/* [한국어] 크기가 32비트를 넘으면 64비트 메모리 BAR 로, 아니면 32비트로 선언한다.
		 * upper_32_bits(size) 가 0 이 아니라는 것은 4GB 를 넘는다는 뜻이다.
		 * 플래그를 |= 로 더하는 점에 주의 — 기존 플래그를 지우지 않는다. */
		ntb->epf->bar[barno].flags |= upper_32_bits(size) ?
				PCI_BASE_ADDRESS_MEM_TYPE_64 :
				PCI_BASE_ADDRESS_MEM_TYPE_32;

		/* [한국어] BAR 를 건다. 물리 주소가 0 이지만 크기와 플래그가 유효하므로
		 * 호스트는 이 BAR 를 크기만큼의 영역으로 열거한다. */
		ret = pci_epc_set_bar(ntb->epf->epc,
				      ntb->epf->func_no,
				      ntb->epf->vfunc_no,
				      &ntb->epf->bar[barno]);
		/* [한국어] 실패하면 지금까지 만든 창을 되돌린다. */
		if (ret) {
			/* [한국어] 원인을 남긴다. */
			dev_err(dev, "MW set failed\n");
			goto err_alloc_mem;
		}

		/* Allocate EPC outbound memory windows to vpci vntb device */
		ntb->vpci_mw_addr[i] = pci_epc_mem_alloc_addr(ntb->epf->epc,
							      &ntb->vpci_mw_phy[i],
							      size);
		/* [한국어] 아웃바운드 공간을 못 얻으면 방금 건 BAR 부터 되돌려야 한다. */
		if (!ntb->vpci_mw_addr[i]) {
			ret = -ENOMEM;
			/* [한국어] 원인을 남긴다. */
			dev_err(dev, "Failed to allocate source address\n");
			goto err_set_bar;
		}
	}

	return ret;

/* [한국어] 창 할당 실패 경로. 방금 건 BAR 를 먼저 걷는다. */
err_set_bar:
	pci_epc_clear_bar(ntb->epf->epc,
			  ntb->epf->func_no,
			  ntb->epf->vfunc_no,
			  &ntb->epf->bar[barno]);
/* [한국어] BAR 설정 실패 경로. 지금까지 완성된 i 개의 창을 정리한다. */
err_alloc_mem:
	epf_ntb_mw_bar_clear(ntb, i);
	return ret;
}

/**
 * epf_ntb_mw_bar_clear() - Clear Memory window BARs
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 * @num_mws: the number of Memory window BARs that to be cleared
 */
/* [한국어]
 * epf_ntb_mw_bar_clear - 창 BAR 를 걷고 아웃바운드 공간을 돌려준다 (정의부)
 *
 * @ntb: 함수 상태
 * @num_mws: 정리할 창 개수. 오류 경로에서 부분적으로 만들어진 상태를
 *   되돌릴 수 있도록 개수를 인자로 받는다.
 * @return: 없음
 *
 * 왜 필요한가: BAR 를 먼저 걷고 그 다음 아웃바운드 공간을 돌려주는 순서를
 * 지켜야 한다. 뒤집으면 아직 살아 있는 BAR 가 해제된 공간을 가리키게 된다.
 * 개수를 인자로 받는 덕분에 epf_ntb_mw_bar_init() 이 i 번째 창에서
 * 실패했을 때 앞의 i 개만 정확히 되돌릴 수 있다.
 *
 * 동작 단계: 창마다 (1) epf_ntb_bar[BAR_MW1 + i] 로 실제 BAR 번호를 찾아
 * 인바운드 매핑을 지우고, (2) 그 창에 대응하는 아웃바운드 주소 공간을
 * pci_epc_mem_free_addr() 로 돌려준다.
 *
 * 실행 컨텍스트: bind 실패 경로와 unbind 경로(프로세스 컨텍스트).
 * pci_epc_clear_bar() 안에서 epc->lock 뮤텍스를 잡는다.
 *
 * 호출 체인:
 *   epf_ntb_mw_bar_init() 실패 경로 또는 epf_ntb_epc_init() 실패 경로 또는
 *   epf_ntb_epc_cleanup() → [epf_ntb_mw_bar_clear]
 *     → pci_epc_clear_bar() → pci_epc_mem_free_addr()
 */
static void epf_ntb_mw_bar_clear(struct epf_ntb *ntb, int num_mws)
{
	/* [한국어] 걷을 BAR 번호. */
	enum pci_barno barno;
	/* [한국어] 창 순회 인덱스. */
	int i;

	/* [한국어] 요청받은 개수만큼만 정리한다. 오류 경로에서 부분적으로만 만들어진
	 * 상태를 되돌릴 수 있도록 개수를 인자로 받는 구조다. */
	for (i = 0; i < num_mws; i++) {
		/* [한국어] 이 창에 배정된 BAR 번호. */
		barno = ntb->epf_ntb_bar[BAR_MW1 + i];
		/* [한국어] BAR 의 인바운드 매핑을 지운다. 호스트가 더 이상 이 창으로 들어오지
		 * 못하게 하는 것이 먼저다. */
		pci_epc_clear_bar(ntb->epf->epc,
				  ntb->epf->func_no,
				  ntb->epf->vfunc_no,
				  &ntb->epf->bar[barno]);

		/* [한국어] 그 다음 아웃바운드 창을 돌려준다. 순서를 뒤집으면 아직 살아 있는
		 * BAR 가 해제된 공간을 가리키게 된다. */
		pci_epc_mem_free_addr(ntb->epf->epc,
				      ntb->vpci_mw_phy[i],
				      ntb->vpci_mw_addr[i],
				      ntb->mws_size[i]);
	}
}

/**
 * epf_ntb_is_bar_used() - Check if a bar is used in the ntb configuration
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 * @barno: Checked bar number
 *
 * Returns: true if used, false if free.
 */
/* [한국어]
 * epf_ntb_is_bar_used - 이 BAR 번호를 이미 쓰는 구성요소가 있는지 본다
 *
 * @ntb: 함수 상태
 * @barno: 확인할 BAR 번호
 * @return: 쓰이고 있으면 true, 비어 있으면 false
 *
 * 왜 필요한가: vntb 는 사용자가 configfs 로 특정 구성요소의 BAR 를 직접
 * 지정할 수 있다. 나머지를 자동 배정할 때 그 지정과 겹치면 안 되므로,
 * 배정 후보를 이 함수로 걸러 낸다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_find_bar() → [epf_ntb_is_bar_used]
 */
static bool epf_ntb_is_bar_used(struct epf_ntb *ntb,
				enum pci_barno barno)
{
	/* [한국어] 배정표를 훑을 반복자. */
	int i;

	/* [한국어] 모든 논리 구성요소를 확인한다. VNTB_BAR_NUM 은 enum 의 마지막 값이라
	 * 칸 수와 같다. */
	for (i = 0; i < VNTB_BAR_NUM; i++) {
		/* [한국어] 이 BAR 번호를 이미 쓰는 구성요소가 있는가. */
		if (ntb->epf_ntb_bar[i] == barno)
			/* [한국어] 하나라도 있으면 쓰이는 중이다. */
			return true;
	}

	/* [한국어] 어느 칸도 이 번호를 쓰지 않으면 비어 있다. */
	return false;
}

/**
 * epf_ntb_find_bar() - Assign BAR number when no configuration is provided
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 * @epc_features: The features provided by the EPC specific to this EPF
 * @bar: NTB BAR index
 * @barno: Bar start index
 *
 * When the BAR configuration was not provided through the userspace
 * configuration, automatically assign BAR as it has been historically
 * done by this endpoint function.
 *
 * Returns: the BAR number found, if any. -1 otherwise
 */
/* [한국어]
 * epf_ntb_find_bar - 사용자가 지정하지 않은 구성요소에만 BAR 를 자동 배정한다
 *
 * @ntb: 함수 상태
 * @epc_features: 컨트롤러 능력표
 * @bar: 배정할 논리 구성요소
 * @barno: 탐색을 시작할 BAR 번호
 * @return: 마지막으로 본 BAR 번호. 남은 BAR 가 없으면 음수(NO_BAR).
 *
 * 왜 필요한가: 이것이 vntb 가 형제 파일과 다른 또 하나의 지점이다.
 * epf_ntb_probe() 가 배정표를 NO_BAR 로 채워 두고, 사용자가 configfs 의
 * ctrl_bar/db_bar/mwN_bar 로 원하는 칸만 지정한다. 이 함수는 아직
 * NO_BAR 인 칸에 대해서만 돌면서, 예약 BAR 와 이미 쓰인 BAR 를 피해
 * 빈 자리를 찾는다.
 *
 * 동작 단계: while 조건이 "아직 배정되지 않았는가" 이므로, 사용자가
 * 이미 지정한 칸이면 루프에 들어가지도 않고 그 지정이 유지된다.
 * 후보를 얻을 때마다 epf_ntb_is_bar_used() 로 겹침을 확인한다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_init_epc_bar() → [epf_ntb_find_bar]
 *     → pci_epc_get_next_free_bar() → epf_ntb_is_bar_used()
 */
static int epf_ntb_find_bar(struct epf_ntb *ntb,
			    const struct pci_epc_features *epc_features,
			    enum epf_ntb_bar bar,
			    enum pci_barno barno)
{
	/* [한국어] 이 논리 구성요소가 아직 배정되지 않은 동안만 돈다.
	 * 이미 사용자가 configfs 로 지정해 두었다면 루프 자체에 들어가지 않아
	 * 그 지정이 그대로 유지된다 — 이것이 이 함수의 핵심 동작이다. */
	while (ntb->epf_ntb_bar[bar] < 0) {
		/* [한국어] 능력표를 보며 다음 빈 BAR 번호를 얻는다. 예약/비활성 BAR 를 건너뛰고,
		 * 직전 BAR 가 64비트 전용이면 한 칸 더 건너뛴다. */
		barno = pci_epc_get_next_free_bar(epc_features, barno);
		/* [한국어] NO_BAR(음수)면 더 이상 쓸 BAR 가 없다. */
		if (barno < 0)
			/* [한국어] 루프를 빠져나가 음수를 그대로 돌려준다. 호출자가 실패로 판단한다. */
			break; /* No more BAR available */

		/*
		 * Verify if the BAR found is not already assigned
		 * through the provided configuration
		 */
		if (!epf_ntb_is_bar_used(ntb, barno))
			/* [한국어] 이 BAR 가 다른 구성요소에 이미 쓰이고 있지 않을 때만 배정한다.
			 * 사용자가 configfs 로 지정한 BAR 와 겹치는 것을 피하기 위한 검사다. */
			ntb->epf_ntb_bar[bar] = barno;

		/* [한국어] 겹쳤든 배정했든 다음 번호로 나아간다. 겹쳤다면 이 구성요소는
		 * 아직 NO_BAR 이므로 루프가 한 번 더 돈다. */
		barno += 1;
	}

	/* [한국어] 마지막으로 본 BAR 번호를 돌려준다. 호출자는 이것을 다음 탐색의
	 * 시작점으로 쓰고, 음수인지로 실패를 판단한다. */
	return barno;
}

/**
 * epf_ntb_init_epc_bar() - Identify BARs to be used for each of the NTB
 * constructs (scratchpad region, doorbell, memorywindow)
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 *
 * Returns: Zero for success, or an error code in case of failure
 */
/* [한국어]
 * epf_ntb_init_epc_bar - 각 NTB 구성요소가 쓸 BAR 번호를 확정한다
 *
 * @ntb: 함수 상태
 * @return: 0 성공, -ENOENT 이면 필수 구성요소에 줄 BAR 가 모자람
 *
 * 왜 필요한가: 컨트롤러마다 쓸 수 있는 BAR 가 다르고, 사용자가 일부를
 * 직접 지정할 수도 있다. 그 두 가지를 합쳐 최종 배치를 정한다.
 *
 * 동작 단계: 필수 세 칸(제어, 도어벨, 첫 창)을 먼저 확정하고,
 * 선택 칸(두 번째 이후 창)은 모자라면 num_mws 를 줄여 조용히 기능을 축소한다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_bind() → [epf_ntb_init_epc_bar] → epf_ntb_find_bar()
 */
static int epf_ntb_init_epc_bar(struct epf_ntb *ntb)
{
	/* [한국어] 컨트롤러 능력표. 어떤 BAR 가 예약/비활성인지 여기서 읽는다. */
	const struct pci_epc_features *epc_features;
	/* [한국어] 자동 배정의 진행 위치. */
	enum pci_barno barno;
	/* [한국어] 논리 구성요소 인덱스. */
	enum epf_ntb_bar bar;
	/* [한국어] 로그용 device 포인터. */
	struct device *dev;
	/* [한국어] 창 개수. 선택 BAR 를 몇 개까지 시도할지 결정한다. */
	u32 num_mws;
	/* [한국어] 선택 BAR 루프에서 실제 배정된 창 수를 세는 변수. */
	int i;

	/* [한국어] BAR0 부터 훑기 시작한다. */
	barno = BAR_0;
	/* [한국어] 사용자가 configfs 로 정한 창 개수. */
	num_mws = ntb->num_mws;
	/* [한국어] 로그 대상. */
	dev = &ntb->epf->dev;
	/* [한국어] 이 컨트롤러의 능력표. */
	epc_features = pci_epc_get_features(ntb->epf->epc, ntb->epf->func_no, ntb->epf->vfunc_no);

	/* These are required BARs which are mandatory for NTB functionality */
	for (bar = BAR_CONFIG; bar <= BAR_MW1; bar++) {
		/* [한국어] 필수 구성요소: 제어 BAR, 도어벨 BAR, 첫 창 BAR.
		 * 사용자가 configfs 로 이미 지정한 칸은 epf_ntb_find_bar 안에서
		 * 그대로 유지되고, NO_BAR 로 남은 칸만 자동 배정된다. */
		barno = epf_ntb_find_bar(ntb, epc_features, bar, barno);
		/* [한국어] BAR 가 모자라면 필수 구성요소를 만들 수 없다. */
		if (barno < 0) {
			/* [한국어] 원인을 남기고 -ENOENT 로 실패한다. */
			dev_err(dev, "Fail to get NTB function BAR\n");
			return -ENOENT;
		}
	}

	/* These are optional BARs which don't impact NTB functionality */
	for (bar = BAR_MW1, i = 1; i < num_mws; bar++, i++) {
		/* [한국어] 선택 구성요소: 두 번째 이후의 창. 없어도 NTB 는 동작한다.
		 * 루프가 BAR_MW1 에서 시작하고 i 가 1 부터라, 첫 반복은 이미 위에서
		 * 배정된 BAR_MW1 을 다시 본다 — epf_ntb_find_bar 가 이미 배정된 칸에는
		 * 손대지 않으므로 결과는 같다. 상류 코드 그대로 둔다. */
		barno = epf_ntb_find_bar(ntb, epc_features, bar, barno);
		/* [한국어] BAR 를 못 찾으면 창 개수를 지금까지 성공한 만큼으로 낮춘다. */
		if (barno < 0) {
			/* [한국어] num_mws 를 줄이면 이후 단계가 그만큼만 창을 만든다. */
			ntb->num_mws = i;
			/* [한국어] 오류가 아니므로 dev_dbg 로만 남긴다. */
			dev_dbg(dev, "BAR not available for > MW%d\n", i + 1);
		}
	}

	return 0;
}

/**
 * epf_ntb_epc_init() - Initialize NTB interface
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 *
 * Wrapper to initialize a particular EPC interface and start the workqueue
 * to check for commands from HOST. This function will write to the
 * EP controller HW for configuring it.
 *
 * Returns: Zero for success, or an error code in case of failure
 */
/* [한국어]
 * epf_ntb_epc_init - EPC 쪽 설정을 모두 수행하고 폴링 워크를 띄운다
 *
 * @ntb: 함수 상태
 * @return: 0 성공, 그 밖에는 실패한 단계의 오류 코드
 *
 * 왜 필요한가: 여기부터가 컨트롤러 레지스터를 실제로 건드리는 지점이다.
 *
 * 동작 단계(순서가 중요하다):
 *   (1) 제어 + 스크래치패드 BAR 를 건다 — 호스트가 명령을 쓸 통로.
 *   (2) MSI 능력을 설정한다.
 *   (3) 도어벨 BAR 를 만든다(MSI 우선, 실패 시 폴링).
 *   (4) 창 BAR 를 걸고 아웃바운드 창을 잡는다.
 *   (5) config 헤더를 쓴다(vfunc_no 가 0 이나 1 일 때만).
 *   (6) 명령 폴링 워크를 큐에 넣는다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트). 여러 EPC 호출에서
 * epc->lock 을 잡았다 놓는다.
 *
 * 에러 경로: 단계마다 레이블이 있어 성공한 단계만 역순으로 취소한다.
 * err_db_bar_init 과 err_config_interrupt 는 같은 자리로 흘러 내린다.
 *
 * 호출 체인:
 *   epf_ntb_bind() → [epf_ntb_epc_init]
 *     → epf_ntb_config_sspad_bar_set() → epf_ntb_configure_interrupt()
 *     → epf_ntb_db_bar_init() → epf_ntb_mw_bar_init()
 *     → pci_epc_write_header() → queue_work()
 */
static int epf_ntb_epc_init(struct epf_ntb *ntb)
{
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] 이 EPF 가 붙은 엔드포인트 컨트롤러. */
	struct pci_epc *epc;
	/* [한국어] EPF 디바이스. config 헤더를 쓸 때 epf->header 가 필요하다. */
	struct pci_epf *epf;
	/* [한국어] 로그용 device 포인터. */
	struct device *dev;
	/* [한국어] 각 단계의 실패를 담는다. */
	int ret;

	/* [한국어] EPF 디바이스. */
	epf = ntb->epf;
	/* [한국어] 로그 대상. */
	dev = &epf->dev;
	/* [한국어] 설정할 컨트롤러. */
	epc = epf->epc;
	/* [한국어] 물리 함수 번호. */
	func_no = ntb->epf->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb->epf->vfunc_no;

	/* [한국어] 1단계: 제어 + 스크래치패드 BAR 를 건다. 이 BAR 가 열려야 호스트가
	 * 명령을 써 넣을 수 있으므로 가장 먼저 한다. */
	ret = epf_ntb_config_sspad_bar_set(ntb);
	/* [한국어] 실패하면 아직 되돌릴 것이 없다. */
	if (ret) {
		/* [한국어] 상류 메시지에 줄바꿈이 빠져 있다. 상류 코드 그대로 둔다. */
		dev_err(dev, "Config/self SPAD BAR init failed");
		return ret;
	}

	/* [한국어] 2단계: MSI 능력을 설정한다. 호스트가 열거할 때 이 개수를 보고
	 * 벡터를 할당한다. */
	ret = epf_ntb_configure_interrupt(ntb);
	/* [한국어] 실패하면 1단계를 되돌린다. */
	if (ret) {
		/* [한국어] 원인을 남긴다. */
		dev_err(dev, "Interrupt configuration failed\n");
		goto err_config_interrupt;
	}

	/* [한국어] 3단계: 도어벨 BAR 를 만든다. 플랫폼 MSI 기반 도어벨을 먼저 시도하고,
	 * 안 되면 메모리 폴링 방식으로 물러선다. */
	ret = epf_ntb_db_bar_init(ntb);
	/* [한국어] 실패하면 앞 단계를 되돌린다. */
	if (ret) {
		/* [한국어] 원인을 남긴다. */
		dev_err(dev, "DB BAR init failed\n");
		goto err_db_bar_init;
	}

	/* [한국어] 4단계: 메모리 윈도우 BAR 를 걸고 아웃바운드 창을 잡는다. */
	ret = epf_ntb_mw_bar_init(ntb);
	/* [한국어] 실패하면 도어벨까지 되돌린다. */
	if (ret) {
		/* [한국어] 원인을 남긴다. */
		dev_err(dev, "MW BAR init failed\n");
		goto err_mw_bar_init;
	}

	/* [한국어] vfunc_no 가 0 이나 1 일 때만 config 헤더를 쓴다.
	 * 가상 함수는 헤더를 물리 함수와 공유하는 경우가 많기 때문이다. */
	if (vfunc_no <= 1) {
		/* [한국어] 벤더/디바이스 ID 와 클래스 코드를 컨트롤러에 쓴다.
		 * 이것이 호스트가 열거할 때 보게 될 모습이다. */
		ret = pci_epc_write_header(epc, func_no, vfunc_no, epf->header);
		/* [한국어] 헤더 쓰기 실패는 호스트가 이 함수를 알아보지 못한다는 뜻이다. */
		if (ret) {
			/* [한국어] 원인을 남긴다. */
			dev_err(dev, "Configuration header write failed\n");
			goto err_write_header;
		}
	}

	/* [한국어] 5단계: 명령 폴링 워크를 초기화한다. */
	INIT_DELAYED_WORK(&ntb->cmd_handler, epf_ntb_cmd_handler);
	/* [한국어] 첫 실행을 지연 없이 큐에 넣는다. 이후 cmd_handler 가 스스로
	 * 재예약하며 계속 돈다. */
	queue_work(kpcintb_workqueue, &ntb->cmd_handler.work);

	return 0;

/* [한국어] 헤더 쓰기 실패 경로. 창부터 되돌린다. */
err_write_header:
	epf_ntb_mw_bar_clear(ntb, ntb->num_mws);
/* [한국어] 창 설정 실패 경로. 도어벨을 되돌린다. */
err_mw_bar_init:
	epf_ntb_db_bar_clear(ntb);
/* [한국어] 도어벨 실패 경로. 아래로 흘러 내려 제어 BAR 를 걷는다. */
err_db_bar_init:
/* [한국어] 인터럽트 설정 실패 경로도 같은 자리로 온다. */
err_config_interrupt:
	/* [한국어] 제어 + 스크래치패드 BAR 를 걷는다. 메모리 해제는 상위 bind 가 맡는다. */
	epf_ntb_config_sspad_bar_clear(ntb);

	return ret;
}


/**
 * epf_ntb_epc_cleanup() - Cleanup all NTB interfaces
 * @ntb: NTB device that facilitates communication between HOST and VHOST
 *
 * Wrapper to cleanup all NTB interfaces.
 */
/* [한국어]
 * epf_ntb_epc_cleanup - EPC 쪽 설정을 초기화의 역순으로 되돌린다
 *
 * @ntb: 함수 상태
 * @return: 없음
 *
 * 왜 필요한가: 워크를 확실히 멈춘 뒤 BAR 를 걷어야 한다. 워크가 계속
 * 돌면 이미 걷힌 BAR 뒤의 메모리를 읽게 된다.
 *
 * 동작 단계: disable_delayed_work_sync() 로 워크를 멈춘다.
 * cmd_handler 가 처리를 마칠 때마다 스스로를 다시 큐에 넣는 구조라
 * (이 파일의 reset_handler 레이블에서 확인할 수 있다), 단순한 취소로는
 * "취소한 직후 워크가 스스로를 다시 넣는" 경주가 남는다. 그래서 형제 파일
 * pci-epf-ntb.c 의 cancel_delayed_work() 보다 강한 이 함수를 쓴다.
 * (이 함수의 정확한 의미론은 include/linux/workqueue.h 에 있는데
 *  이 트리에는 그 헤더가 체크아웃되어 있지 않아 확인하지 못했다.)
 * 그 뒤 창 → 도어벨 → 제어 BAR 순으로 걷는다.
 *
 * 실행 컨텍스트: bind 실패 경로와 unbind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_unbind() 또는 epf_ntb_bind() 실패 경로 → [epf_ntb_epc_cleanup]
 */
static void epf_ntb_epc_cleanup(struct epf_ntb *ntb)
{
	/* [한국어] 폴링 워크를 멈춘다. cmd_handler 가 처리를 마칠 때마다
	 * 스스로를 다시 큐에 넣으므로(이 파일의 reset_handler 레이블),
	 * 단순한 취소로는 "취소 직후 다시 큐에 들어가는" 경주가 남는다.
	 * 그래서 형제 파일 pci-epf-ntb.c 의 cancel_delayed_work() 보다
	 * 강한 이 함수를 쓴다. (이 함수의 정확한 의미론은
	 *  include/linux/workqueue.h 소관인데 이 트리에 그 헤더가 없어
	 *  확인하지 못했다.) */
	disable_delayed_work_sync(&ntb->cmd_handler);
	/* [한국어] 창 BAR 를 걷고 아웃바운드 창을 돌려준다. num_mws 개 전부가 대상이다. */
	epf_ntb_mw_bar_clear(ntb, ntb->num_mws);
	/* [한국어] 도어벨 BAR 를 걷고, MSI 도어벨을 썼다면 IRQ 도 반납한다. */
	epf_ntb_db_bar_clear(ntb);
	/* [한국어] 마지막으로 제어 + 스크래치패드 BAR 를 걷는다.
	 * 초기화의 역순이다. */
	epf_ntb_config_sspad_bar_clear(ntb);
}

/* [한국어] 아래 여섯 매크로가 configfs 속성의 show/store 함수를 찍어낸다.
 * 매크로 안에서는 줄 끝 역슬래시가 줄을 잇고 있어 주석 줄을 끼워 넣을 수
 * 없으므로, 설명을 모두 매크로 바깥에 둔다.
 * 
 * EPF_NTB_R(_name): epf_ntb_<_name>_show() 를 만든다.
 *   config_item → config_group → to_epf_ntb() 로 struct epf_ntb 를 되찾아
 *   ntb-><_name> 을 십진수로 찍는다.
 *   ntb 쪽이 sysfs_emit 을 쓰는 데 반해 여기서는 sprintf 를 쓴다. */
#define EPF_NTB_R(_name)						\
static ssize_t epf_ntb_##_name##_show(struct config_item *item,		\
				      char *page)			\
{									\
	struct config_group *group = to_config_group(item);		\
	struct epf_ntb *ntb = to_epf_ntb(group);			\
									\
	return sprintf(page, "%d\n", ntb->_name);			\
}

/* [한국어] EPF_NTB_W(_name): epf_ntb_<_name>_store() 를 만든다.
 *   kstrtou32 의 오류 코드를 그대로 사용자에게 돌려준다(ntb 쪽은 -EINVAL 로 뭉갠다).
 *   검증 없이 대입하므로 범위 검사가 필요한 속성은 따로 구현되어 있다. */
#define EPF_NTB_W(_name)						\
static ssize_t epf_ntb_##_name##_store(struct config_item *item,	\
				       const char *page, size_t len)	\
{									\
	struct config_group *group = to_config_group(item);		\
	struct epf_ntb *ntb = to_epf_ntb(group);			\
	u32 val;							\
	int ret;							\
									\
	ret = kstrtou32(page, 0, &val);					\
	if (ret)							\
		return ret;						\
									\
	ntb->_name = val;						\
									\
	return len;							\
}

/* [한국어] EPF_NTB_MW_R(_name): 메모리 윈도우 크기 읽기 함수를 만든다.
 *   ntb 쪽과 달리 sscanf 반환값을 검사하고 범위도 확인하며,
 *   array_index_nospec() 으로 투기적 실행 방어까지 넣었다. */
#define EPF_NTB_MW_R(_name)						\
static ssize_t epf_ntb_##_name##_show(struct config_item *item,		\
				      char *page)			\
{									\
	struct config_group *group = to_config_group(item);		\
	struct epf_ntb *ntb = to_epf_ntb(group);			\
	struct device *dev = &ntb->epf->dev;				\
	int win_no, idx;						\
									\
	if (sscanf(#_name, "mw%d", &win_no) != 1)			\
		return -EINVAL;						\
									\
	idx = win_no - 1;						\
	if (idx < 0 || idx >= ntb->num_mws) {				\
		dev_err(dev, "MW%d out of range (num_mws=%d)\n",	\
			win_no, ntb->num_mws);				\
		return -ERANGE;						\
	}								\
	idx = array_index_nospec(idx, ntb->num_mws);			\
	return sprintf(page, "%llu\n", ntb->mws_size[idx]);		\
}

/* [한국어] EPF_NTB_MW_W(_name): 메모리 윈도우 크기 쓰기 함수를 만든다.
 *   #_name 을 sscanf 로 파싱해 창 번호를 얻고 1 을 빼 인덱스로 삼는다.
 *   범위를 벗어나면 -ERANGE 로 거절하며, 통과한 뒤에도
 *   array_index_nospec() 으로 인덱스를 한 번 더 좁힌다 —
 *   분기 예측을 이용한 투기적 실행(Spectre v1)으로 배열 밖을 읽는 것을
 *   막기 위한 방어다. 사용자 입력이 배열 인덱스가 되는 자리이므로 필요하다. */
#define EPF_NTB_MW_W(_name)						\
static ssize_t epf_ntb_##_name##_store(struct config_item *item,	\
				       const char *page, size_t len)	\
{									\
	struct config_group *group = to_config_group(item);		\
	struct epf_ntb *ntb = to_epf_ntb(group);			\
	struct device *dev = &ntb->epf->dev;				\
	int win_no, idx;						\
	u64 val;							\
	int ret;							\
									\
	ret = kstrtou64(page, 0, &val);					\
	if (ret)							\
		return ret;						\
									\
	if (sscanf(#_name, "mw%d", &win_no) != 1)			\
		return -EINVAL;						\
									\
	idx = win_no - 1;						\
	if (idx < 0 || idx >= ntb->num_mws) {				\
		dev_err(dev, "MW%d out of range (num_mws=%d)\n",	\
			win_no, ntb->num_mws);				\
		return -ERANGE;						\
	}								\
	idx = array_index_nospec(idx, ntb->num_mws);			\
	ntb->mws_size[idx] = val;					\
									\
	return len;							\
}

/* [한국어] EPF_NTB_BAR_R(_name, _id): BAR 번호 읽기 함수를 만든다.
 *   _id 는 enum epf_ntb_bar 값이며, epf_ntb_bar[_id] 를 십진수로 찍는다.
 *   아직 배정되지 않았으면 NO_BAR(음수)가 그대로 보인다. */
#define EPF_NTB_BAR_R(_name, _id)					\
	static ssize_t epf_ntb_##_name##_show(struct config_item *item,	\
					      char *page)		\
	{								\
		struct config_group *group = to_config_group(item);	\
		struct epf_ntb *ntb = to_epf_ntb(group);		\
									\
		return sprintf(page, "%d\n", ntb->epf_ntb_bar[_id]);	\
	}

/* [한국어] EPF_NTB_BAR_W(_name, _id): BAR 번호 쓰기 함수를 만든다.
 *   값의 범위를 NO_BAR 부터 BAR_5 까지로 검사한다 — NO_BAR 를 쓰면
 *   "자동 배정으로 되돌린다" 는 뜻이 되고, 0~5 는 그 BAR 를 강제한다.
 *   범위를 벗어난 값은 이후 epf_ntb_bar[] 색인이나 BAR 배열 접근에서
 *   문제가 되므로 여기서 반드시 막는다.
 *   주의: 같은 BAR 를 두 구성요소에 지정하는 것은 여기서 막지 않고,
 *   epf_ntb_is_bar_used() 가 자동 배정 때만 겹침을 피한다. */
#define EPF_NTB_BAR_W(_name, _id)					\
	static ssize_t epf_ntb_##_name##_store(struct config_item *item, \
					       const char *page, size_t len) \
	{								\
		struct config_group *group = to_config_group(item);	\
		struct epf_ntb *ntb = to_epf_ntb(group);		\
		int val;						\
		int ret;						\
									\
		ret = kstrtoint(page, 0, &val);				\
		if (ret)						\
			return ret;					\
									\
		if (val < NO_BAR || val > BAR_5)			\
			return -EINVAL;					\
									\
		ntb->epf_ntb_bar[_id] = val;				\
									\
		return len;						\
	}

/* [한국어]
 * epf_ntb_num_mws_store - configfs 의 num_mws 파일에 쓴 값을 받는다
 *
 * @item: 쓰기가 일어난 configfs 항목
 * @page: 사용자가 쓴 바이트열
 * @len: 그 길이
 * @return: 성공하면 len, 파싱 실패면 그 오류 코드, 범위 초과면 -EINVAL
 *
 * 왜 필요한가: 다른 정수 속성은 EPF_NTB_W 매크로가 찍어 주지만,
 * num_mws 는 MAX_MW 상한 검사가 필요해 손으로 구현되어 있다.
 * 이 검사가 없으면 mws_size[] 와 epf_ntb_bar[] 배열 밖을 건드리게 된다.
 *
 * 실행 컨텍스트: 사용자의 write(2) 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   사용자 echo → configfs → [epf_ntb_num_mws_store]
 */
static ssize_t epf_ntb_num_mws_store(struct config_item *item,
				     const char *page, size_t len)
{
	/* [한국어] 쓰기가 일어난 configfs 항목을 그 그룹으로 되돌린다. */
	struct config_group *group = to_config_group(item);
	/* [한국어] to_epf_ntb() 로 함수 전체 상태에 닿는다. */
	struct epf_ntb *ntb = to_epf_ntb(group);
	/* [한국어] 사용자가 쓴 값을 담을 임시 변수. */
	u32 val;
	/* [한국어] kstrtou32 의 실패 코드를 담는다. */
	int ret;

	/* [한국어] 문자열을 32비트 부호 없는 정수로 바꾼다. base 0 이라 0x 접두사도 해석한다. */
	ret = kstrtou32(page, 0, &val);
	/* [한국어] 파싱 실패는 그 오류 코드를 그대로 사용자에게 돌려준다.
	 * (ntb 쪽이 -EINVAL 로 뭉개는 것과 달리 여기서는 원인을 보존한다.) */
	if (ret)
		return ret;

	/* [한국어] MAX_MW(4)를 넘으면 mws_size[] 와 epf_ntb_bar[] 배열 밖을 건드리게 된다. */
	if (val > MAX_MW)
		/* [한국어] 범위를 벗어난 값은 거부한다. */
		return -EINVAL;

	/* [한국어] 검증을 통과한 값만 반영한다. bind 시점에 창을 몇 개 만들지 결정한다. */
	ntb->num_mws = val;

	/* [한국어] configfs 규약상 store 는 소비한 바이트 수를 돌려준다. */
	return len;
}

/* [한국어] 아래 EPF_NTB_R/W 매크로 전개들. epf_ntb_<이름>_show/store 함수를 만든다.
 * num_mws 는 상한 검사가 필요해 store 만 손으로 구현되어 있다. */
EPF_NTB_R(spad_count)
EPF_NTB_W(spad_count)
EPF_NTB_R(db_count)
EPF_NTB_W(db_count)
EPF_NTB_R(num_mws)
EPF_NTB_R(vbus_number)
EPF_NTB_W(vbus_number)
EPF_NTB_R(vntb_pid)
EPF_NTB_W(vntb_pid)
EPF_NTB_R(vntb_vid)
EPF_NTB_W(vntb_vid)
/* [한국어] mw1~mw4 는 창 크기다. EPF_NTB_MW_R/W 가 이름을 파싱해
 * mws_size[] 인덱스를 얻는다. */
EPF_NTB_MW_R(mw1)
EPF_NTB_MW_W(mw1)
EPF_NTB_MW_R(mw2)
EPF_NTB_MW_W(mw2)
EPF_NTB_MW_R(mw3)
EPF_NTB_MW_W(mw3)
EPF_NTB_MW_R(mw4)
EPF_NTB_MW_W(mw4)
/* [한국어] 아래는 EPF_NTB_BAR_R/W 전개다. epf_ntb_bar[논리인덱스] 를 읽고 쓴다.
 * 두 번째 인자가 enum epf_ntb_bar 의 값이라, 속성 이름과 논리 구성요소가
 * 여기서 짝지어진다. */
EPF_NTB_BAR_R(ctrl_bar, BAR_CONFIG)
EPF_NTB_BAR_W(ctrl_bar, BAR_CONFIG)
EPF_NTB_BAR_R(db_bar, BAR_DB)
EPF_NTB_BAR_W(db_bar, BAR_DB)
EPF_NTB_BAR_R(mw1_bar, BAR_MW1)
EPF_NTB_BAR_W(mw1_bar, BAR_MW1)
EPF_NTB_BAR_R(mw2_bar, BAR_MW2)
EPF_NTB_BAR_W(mw2_bar, BAR_MW2)
EPF_NTB_BAR_R(mw3_bar, BAR_MW3)
EPF_NTB_BAR_W(mw3_bar, BAR_MW3)
EPF_NTB_BAR_R(mw4_bar, BAR_MW4)
EPF_NTB_BAR_W(mw4_bar, BAR_MW4)

/* [한국어] CONFIGFS_ATTR(접두사, 이름): show/store 한 쌍을 묶어
 * configfs_attribute 구조체 epf_ntb_attr_<이름> 을 만든다. */
CONFIGFS_ATTR(epf_ntb_, spad_count);
CONFIGFS_ATTR(epf_ntb_, db_count);
CONFIGFS_ATTR(epf_ntb_, num_mws);
/* [한국어] mw1~mw4 속성. 각 메모리 윈도우의 크기를 바이트 단위로 읽고 쓴다.
 * 값은 ntb->mws_size[] 에 들어가며, bind 시점에 창 BAR 크기를 정한다. */
CONFIGFS_ATTR(epf_ntb_, mw1);
CONFIGFS_ATTR(epf_ntb_, mw2);
CONFIGFS_ATTR(epf_ntb_, mw3);
CONFIGFS_ATTR(epf_ntb_, mw4);
/* [한국어] vbus_number/vntb_pid/vntb_vid 는 가상 PCI 장치의 정체를 정한다.
 * bind 시점에 pci_space[0] 와 pci_vntb_table[0] 에 반영된다. */
CONFIGFS_ATTR(epf_ntb_, vbus_number);
CONFIGFS_ATTR(epf_ntb_, vntb_pid);
CONFIGFS_ATTR(epf_ntb_, vntb_vid);
/* [한국어] ctrl_bar 부터는 BAR 를 직접 지정하는 속성이다.
 * ntb 쪽에는 없는 vntb 만의 기능으로, 보드마다 쓸 수 있는 BAR 가
 * 다른 경우에 사용자가 배치를 강제할 수 있게 한다. */
CONFIGFS_ATTR(epf_ntb_, ctrl_bar);
CONFIGFS_ATTR(epf_ntb_, db_bar);
CONFIGFS_ATTR(epf_ntb_, mw1_bar);
/* [한국어] mw2_bar~mw4_bar 속성. 두 번째 이후 창을 놓을 BAR 번호를 직접 지정한다.
 * 지정하지 않으면 NO_BAR 로 남아 epf_ntb_find_bar() 가 자동 배정한다. */
CONFIGFS_ATTR(epf_ntb_, mw2_bar);
CONFIGFS_ATTR(epf_ntb_, mw3_bar);
CONFIGFS_ATTR(epf_ntb_, mw4_bar);

/* [한국어] configfs 디렉터리에 만들어질 속성 파일 목록. NULL 로 끝난다.
 * ntb 쪽보다 항목이 훨씬 많은데, vntb 는 가상 PCI 장치의 ID 와
 * BAR 배정까지 사용자가 정할 수 있기 때문이다. */
static struct configfs_attribute *epf_ntb_attrs[] = {
	/* [한국어] spad_count: 스크래치패드 워드 개수(내 몫과 상대 몫 각각). */
	&epf_ntb_attr_spad_count,
	/* [한국어] db_count: 도어벨 개수. */
	&epf_ntb_attr_db_count,
	/* [한국어] num_mws: 메모리 윈도우 개수. */
	&epf_ntb_attr_num_mws,
	/* [한국어] mw1~mw4: 각 창의 크기(바이트). */
	&epf_ntb_attr_mw1,
	&epf_ntb_attr_mw2,
	&epf_ntb_attr_mw3,
	&epf_ntb_attr_mw4,
	/* [한국어] vbus_number: 만들 가상 PCI 버스의 번호. 기본값 0xff. */
	&epf_ntb_attr_vbus_number,
	/* [한국어] vntb_pid: 가상 NTB 장치의 PCI 디바이스 ID. */
	&epf_ntb_attr_vntb_pid,
	/* [한국어] vntb_vid: 가상 NTB 장치의 PCI 벤더 ID.
	 * 이 둘이 정해져야 vpci_scan_bus 에서 드라이버가 매칭된다. */
	&epf_ntb_attr_vntb_vid,
	/* [한국어] ctrl_bar: 제어 + 스크래치패드를 놓을 BAR 번호(직접 지정용). */
	&epf_ntb_attr_ctrl_bar,
	/* [한국어] db_bar: 도어벨을 놓을 BAR 번호. */
	&epf_ntb_attr_db_bar,
	/* [한국어] mw1_bar~mw4_bar: 각 창을 놓을 BAR 번호.
	 * 지정하지 않으면 NO_BAR 로 남아 epf_ntb_find_bar() 가 자동 배정한다. */
	&epf_ntb_attr_mw1_bar,
	&epf_ntb_attr_mw2_bar,
	&epf_ntb_attr_mw3_bar,
	&epf_ntb_attr_mw4_bar,
	/* [한국어] NULL 종결자. 이것이 없으면 configfs 가 배열 밖을 읽는다. */
	NULL,
};

/* [한국어] 이 config_group 의 타입 서술자. */
static const struct config_item_type ntb_group_type = {
	/* [한국어] ct_attrs: 위에서 만든 속성 목록. */
	.ct_attrs	= epf_ntb_attrs,
	/* [한국어] ct_owner: 디렉터리가 살아 있는 동안 모듈 언로드를 막는다. */
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
 *
 * Returns: Pointer to config_group
 */
/* [한국어]
 * epf_ntb_add_cfs - 이 함수 전용 configfs 디렉터리를 만들어 준다
 *
 * @epf: NTB 엔드포인트 함수 디바이스
 * @group: 부모 config_group
 * @return: 이 함수의 속성들을 담은 config_group 포인터
 *
 * 왜 필요한가: db_count, spad_count, num_mws, mw1~mw4 에 더해 vntb 는
 * 가상 PCI 장치의 ID 와 BAR 배정까지 사용자가 정할 수 있다.
 * 그 속성들을 담을 하위 디렉터리를 여기서 만든다.
 *
 * 동작 단계: struct epf_ntb 안에 박아 둔 config_group 을 EPF 디바이스
 * 이름으로 초기화하고 ntb_group_type 을 붙인다. 따로 할당하지 않으므로
 * 해제 책임도 없다.
 *
 * 실행 컨텍스트: configfs 디렉터리 생성(사용자 mkdir) 문맥.
 *
 * 호출 체인:
 *   사용자 mkdir → configfs → pci-ep-cfs.c → [epf_ntb_add_cfs]
 */
static struct config_group *epf_ntb_add_cfs(struct pci_epf *epf,
					    struct config_group *group)
{
	/* [한국어] EPF 디바이스에 매달아 둔 함수 상태를 꺼낸다. */
	struct epf_ntb *ntb = epf_get_drvdata(epf);
	/* [한국어] struct epf_ntb 안에 박아 둔 config_group 을 그대로 쓴다.
	 * 따로 할당하지 않으므로 해제 책임도 없다. */
	struct config_group *ntb_group = &ntb->group;
	/* [한국어] 디렉터리 이름으로 쓸 EPF 디바이스 이름을 얻기 위한 device 포인터. */
	struct device *dev = &epf->dev;

	/* [한국어] 그룹을 초기화하면서 이름을 EPF 디바이스 이름으로 정하고
	 * ntb_group_type 을 붙인다. 이 시점에 속성 파일들이 생긴다. */
	config_group_init_type_name(ntb_group, dev_name(dev), &ntb_group_type);

	/* [한국어] 호출자가 이 그룹을 부모 디렉터리 밑에 등록한다. */
	return ntb_group;
}

/* [한국어] 여기부터가 가상 PCI 버스 구현이다. 실제 PCIe 링크 없이 버스 하나와
 * 장치 하나를 소프트웨어로 만들어, 그 위에 아래의 가상 NTB 드라이버가
 * 붙게 한다. 이 계층이 있어야 drivers/ntb 가 요구하는 "PCI 디바이스에
 * 얹힌 NTB 디바이스" 모양을 갖출 수 있다. */
/*==== virtual PCI bus driver, which only load virtual NTB PCI driver ====*/

/* [한국어] 가상 PCI 장치의 config 공간을 흉내 내는 워드 배열.
 * 커널의 PCI 열거 코드가 이 버스를 훑을 때 여기서 값을 읽어 간다.
 * 각 워드의 의미는 옆의 상류 주석대로 PCI 규약을 따른다.
 * 실제 장치가 아니므로 BAR 는 모두 0 이고 능력 목록도 없다. */
static u32 pci_space[] = {
	/* [한국어] 첫 워드(디바이스 ID + 벤더 ID)의 초기값 0xffffffff.
	 * epf_ntb_bind() 가 사용자가 정한 vntb_pid/vntb_vid 로 덮어쓴다.
	 * 0xffffffff 는 PCI 에서 "장치 없음" 을 뜻하므로, 덮어쓰기 전에
	 * 열거가 일어나면 장치가 발견되지 않는다. */
	0xffffffff,	/* Device ID, Vendor ID */
	0,		/* Status, Command */
	0xffffffff,	/* Base Class, Subclass, Prog Intf, Revision ID */
	/* [한국어] 헤더 타입 0(일반 장치)에 캐시 라인 크기 0x40 을 넣어 둔 워드. */
	0x40,		/* BIST, Header Type, Latency Timer, Cache Line Size */
	0,		/* BAR 0 */
	0,		/* BAR 1 */
	0,		/* BAR 2 */
	0,		/* BAR 3 */
	0,		/* BAR 4 */
	0,		/* BAR 5 */
	0,		/* Cardbus CIS Pointer */
	0,		/* Subsystem ID, Subsystem Vendor ID */
	0,		/* ROM Base Address */
	0,		/* Reserved, Capabilities Pointer */
	0,		/* Reserved */
	0,		/* Max_Lat, Min_Gnt, Interrupt Pin, Interrupt Line */
};

/* [한국어]
 * pci_read - 가상 PCI 버스의 config 공간 읽기를 흉내 낸다
 *
 * @bus: 읽기가 일어난 가상 버스(쓰지 않는다)
 * @devfn: 장치/함수 번호
 * @where: config 공간 안의 오프셋
 * @size: 읽을 바이트 수
 * @val: 읽은 값을 담을 곳
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND
 *
 * 왜 필요한가: 커널 PCI 열거 코드는 config 공간을 읽어 장치가 있는지
 * 판단한다. 이 가상 버스에는 실제 하드웨어가 없으므로, 위의 pci_space[]
 * 배열에서 값을 복사해 주는 것으로 장치 하나를 흉내 낸다.
 *
 * 동작 단계: devfn 0(버스 위의 유일한 장치)에 대해서만 배열에서 복사하고,
 * 다른 devfn 에는 "장치 없음" 을 답해 열거를 멈추게 한다.
 *
 * 주의: where 에 대한 경계 검사가 없다. 이 버스에 접근하는 것이 커널
 * 열거 코드뿐이라 오프셋이 config 공간 안이라고 전제한 것으로 읽힌다.
 *
 * 실행 컨텍스트: pci_scan_bus() 안(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   vpci_scan_bus() → pci_scan_bus() → PCI 코어 → [pci_read]
 */
static int pci_read(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 *val)
{
	/* [한국어] devfn 0, 즉 버스 위의 유일한 장치에 대한 접근만 응답한다.
	 * 장치 번호 0, 함수 번호 0 이 이 가상 버스의 단 하나뿐인 장치다. */
	if (devfn == 0) {
		/* [한국어] 요청한 오프셋에서 요청한 바이트 수만큼 배열에서 복사한다.
		 * 경계 검사가 없다는 점에 주의 — 이 버스에 접근하는 것은 커널
		 * PCI 열거 코드뿐이라 오프셋이 config 공간 안이라고 전제한 것으로 읽힌다. */
		memcpy(val, ((u8 *)pci_space) + where, size);
		/* [한국어] PCI config 읽기 성공 코드. */
		return PCIBIOS_SUCCESSFUL;
	}
	/* [한국어] 다른 devfn 은 장치가 없다고 답한다. 이것이 열거를 devfn 0 에서
	 * 멈추게 하는 장치다. */
	return PCIBIOS_DEVICE_NOT_FOUND;
}

/* [한국어]
 * pci_write - 가상 PCI 버스의 config 공간 쓰기를 무시한다
 *
 * @bus: 쓰기가 일어난 가상 버스
 * @devfn: 장치/함수 번호
 * @where: config 공간 안의 오프셋
 * @size: 쓸 바이트 수
 * @val: 쓸 값
 * @return: 항상 0
 *
 * 왜 필요한가: PCI 코어는 열거 과정에서 명령 레지스터나 BAR 에 값을 쓴다.
 * 이 가상 장치는 설정할 것이 없으므로 그 쓰기를 조용히 삼킨다.
 * 쓰기를 반영하지 않아도 되는 이유는 이 장치가 실제 자원을 갖지 않고,
 * 진짜 BAR 는 EPC 쪽에서 따로 관리되기 때문이다.
 *
 * 실행 컨텍스트: pci_scan_bus() 안(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   PCI 코어 → [pci_write]
 */
static int pci_write(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 val)
{
	/* [한국어] config 쓰기는 전부 무시한다. 이 가상 장치는 BAR 를 스스로 갖지 않고
	 * 설정될 것도 없으므로, 커널이 쓰려 해도 조용히 삼킨다. */
	return 0;
}

/* [한국어] 가상 버스의 config 공간 접근 연산. 실제 하드웨어 접근이 아니라
 * 위의 배열을 읽고 쓰기를 무시하는 것으로 흉내 낸다. */
static struct pci_ops vpci_ops = {
	/* [한국어] read: 배열에서 복사해 준다. */
	.read = pci_read,
	/* [한국어] write: 아무 것도 하지 않는다. */
	.write = pci_write,
};

/* [한국어]
 * vpci_scan_bus - 가상 PCI 버스를 만들고 스캔해 NTB 디바이스가 붙게 한다
 *
 * @sysdata: 함수 상태(struct epf_ntb). 버스와 그 위 장치의 sysdata 가 된다.
 * @return: 0 성공, -EINVAL 이면 버스 생성 실패
 *
 * 왜 필요한가: drivers/ntb 는 "PCI 디바이스에 얹힌 NTB 디바이스" 모양을
 * 요구한다. 하지만 EP 쪽에는 그런 PCI 디바이스가 없다. 그래서 이 함수가
 * 버스 하나와 장치 하나를 소프트웨어로 만들어 그 모양을 갖춘다.
 * 이 한 번의 스캔에서 pci_vntb_probe 가 불리고, 그 안에서
 * ntb_register_device() 로 NTB 디바이스가 태어난다.
 *
 * 동작 단계: 사용자가 정한 버스 번호로 pci_scan_bus() 를 부르고(config
 * 접근은 vpci_ops 가 메모리 배열로 흉내 낸다), pci_bus_add_devices() 로
 * 발견된 장치를 디바이스 모델에 등록해 드라이버 매칭을 일으킨다.
 *
 * 실행 컨텍스트: bind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_bind() → [vpci_scan_bus] → pci_scan_bus()
 *     → pci_bus_add_devices() → pci_vntb_probe()
 */
static int vpci_scan_bus(void *sysdata)
{
	/* [한국어] 만들어질 가상 PCI 버스. */
	struct pci_bus *vpci_bus;
	/* [한국어] sysdata 로 넘어온 함수 상태. 이 포인터가 pci_vntb_probe 에서
	 * pdev->sysdata 로 다시 나온다. */
	struct epf_ntb *ndev = sysdata;

	/* [한국어] 가상 PCI 버스를 만들고 스캔한다. 버스 번호는 사용자가 정한 값이고,
	 * config 접근은 위의 vpci_ops 가 메모리 배열로 흉내 낸다.
	 * 실제 하드웨어 버스가 아니라 순전히 소프트웨어로 만든 버스다. */
	vpci_bus = pci_scan_bus(ndev->vbus_number, &vpci_ops, sysdata);
	/* [한국어] 버스를 못 만들면 NTB 디바이스도 만들 수 없다. */
	if (!vpci_bus) {
		/* [한국어] 아직 dev_err 대상이 마땅치 않아 pr_err 를 쓴다. */
		pr_err("create pci bus failed\n");
		return -EINVAL;
	}

	/* [한국어] 스캔으로 발견된 장치들을 커널 디바이스 모델에 등록한다.
	 * 이 호출 안에서 드라이버 매칭이 일어나 pci_vntb_probe 가 불린다. */
	pci_bus_add_devices(vpci_bus);

	return 0;
}

/* [한국어] 여기부터가 로컬 쪽 가상 NTB 디바이스의 구현이다.
 * 위쪽 가상 PCI 버스에서 발견된 장치에 pci_vntb_probe 가 붙고,
 * 그것이 ntb_register_device() 로 drivers/ntb 에 등록되면
 * 아래 vntb_epf_ 계열 함수들이 NTB 클라이언트의 요청을 받는다. */
/*==================== Virtual PCIe NTB driver ==========================*/

/* [한국어]
 * vntb_epf_mw_count - 로컬에서 쓸 수 있는 메모리 윈도우 개수를 알려 준다
 *
 * @ntb: NTB 디바이스
 * @pidx: 상대 포트 인덱스(이 구현에서는 쓰지 않는다 — 상대가 하나뿐이다)
 * @return: 사용자가 configfs 로 정한 창 개수
 *
 * 왜 필요한가: NTB 클라이언트(ntb_transport 등)가 창을 몇 개 쓸 수 있는지
 * 물을 때 답하는 연산이다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_mw_count() → [vntb_epf_mw_count]
 */
static int vntb_epf_mw_count(struct ntb_dev *ntb, int pidx)
{
	/* [한국어] NTB 디바이스에서 이 EPF 상태로 되돌아온다. */
	struct epf_ntb *ndev = ntb_ndev(ntb);

	/* [한국어] 사용자가 configfs 로 정한 창 개수를 그대로 돌려준다. */
	return ndev->num_mws;
}

/* [한국어]
 * vntb_epf_spad_count - 스크래치패드 워드 개수를 알려 준다
 *
 * @ntb: NTB 디바이스
 * @return: 한 절반의 워드 개수(사용자가 정한 spad_count)
 *
 * 왜 필요한가: NTB 클라이언트가 쓸 수 있는 스크래치패드 인덱스의 상한이다.
 * 두 벌을 잡지만 클라이언트에게 보이는 것은 한 절반뿐이다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_spad_count() → [vntb_epf_spad_count]
 */
static int vntb_epf_spad_count(struct ntb_dev *ntb)
{
	/* [한국어] 스크래치패드 워드 개수. ntb_ndev() 매크로가 NTB 디바이스에서
	 * struct epf_ntb 로 되돌린다. */
	return ntb_ndev(ntb)->spad_count;
}

/* [한국어]
 * vntb_epf_peer_mw_count - 상대(호스트) 쪽 창 개수를 알려 준다
 *
 * @ntb: NTB 디바이스
 * @return: num_mws 를 그대로 돌려준다
 *
 * 왜 필요한가: 이 구현에서는 같은 창을 양쪽이 반대편에서 보는 것이라
 * 로컬 창 개수와 상대 창 개수가 같다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_peer_mw_count() → [vntb_epf_peer_mw_count]
 */
static int vntb_epf_peer_mw_count(struct ntb_dev *ntb)
{
	/* [한국어] 상대(호스트) 쪽에서 볼 창 개수. 이 구현에서는 양쪽이 같은 창을
	 * 반대편에서 보는 것이라 num_mws 를 그대로 돌려준다. */
	return ntb_ndev(ntb)->num_mws;
}

/* [한국어]
 * vntb_epf_db_valid_mask - 유효한 도어벨 비트의 마스크를 알려 준다
 *
 * @ntb: NTB 디바이스
 * @return: 하위 db_count 비트가 모두 1 인 마스크
 *
 * 왜 필요한가: NTB 클라이언트는 이 마스크로 어떤 도어벨 비트를 쓸 수
 * 있는지 안다. BIT_ULL(n) - 1 이 정확히 하위 n 비트를 세우는 관용구다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_db_valid_mask() → [vntb_epf_db_valid_mask]
 */
static u64 vntb_epf_db_valid_mask(struct ntb_dev *ntb)
{
	/* [한국어] 유효한 도어벨 비트 마스크. db_count 개의 하위 비트를 모두 1 로 만든다.
	 * BIT_ULL(n) - 1 이 정확히 하위 n 비트를 세우는 관용구다. */
	return BIT_ULL(ntb_ndev(ntb)->db_count) - 1;
}

/* [한국어]
 * vntb_epf_db_set_mask - 도어벨 마스킹 요청을 받아들이지만 아무 것도 하지 않는다
 *
 * @ntb: NTB 디바이스
 * @db_bits: 마스킹할 도어벨 비트
 * @return: 항상 0(성공)
 *
 * 왜 필요한가: NTB 연산 표에 자리가 있어야 클라이언트가 이 호출을 할 수
 * 있다. 이 구현에는 도어벨을 하드웨어 수준에서 가릴 수단이 없으므로
 * 성공만 돌려주고 실제로는 마스킹하지 않는다. 클라이언트가 원치 않는
 * 도어벨을 받게 되더라도 db_read/db_clear 로 걸러 낼 수 있다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥.
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_db_set_mask() → [vntb_epf_db_set_mask]
 */
static int vntb_epf_db_set_mask(struct ntb_dev *ntb, u64 db_bits)
{
	return 0;
}

/* [한국어]
 * vntb_epf_mw_set_trans - 로컬 창 BAR 를 클라이언트가 준 DMA 주소로 다시 건다
 *
 * @ndev: NTB 디바이스
 * @pidx: 상대 포트 인덱스(쓰지 않는다)
 * @idx: 창 번호
 * @addr: 이 창이 가리킬 DMA 주소(보통 ntb_transport 의 수신 버퍼)
 * @size: 그 크기
 * @return: 0 성공, 그 밖에는 pci_epc_set_bar() 의 실패 코드
 *
 * 왜 필요한가: epf_ntb_mw_bar_init() 은 크기만 선언한 채 BAR 를 걸어
 * 호스트가 열거할 수 있게 해 두었다. 실제 목적지는 NTB 클라이언트가
 * 버퍼를 준비한 뒤 이 연산으로 알려 준다. 그때 BAR 를 다시 거는 것이다.
 * 이 매핑이 서면 호스트가 그 BAR 에 쓴 데이터가 곧바로 버퍼에 들어간다.
 *
 * 주의: pci_epc_set_bar 에 함수 번호를 0, 0 으로 박아 넣는다.
 * ntb->epf->func_no 를 쓰지 않는다. 상류 코드 그대로 둔다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_mw_set_trans() → [vntb_epf_mw_set_trans]
 *     → pci_epc_set_bar()
 */
static int vntb_epf_mw_set_trans(struct ntb_dev *ndev, int pidx, int idx,
		dma_addr_t addr, resource_size_t size)
{
	/* [한국어] NTB 디바이스에서 이 EPF 상태로 되돌아온다. */
	struct epf_ntb *ntb = ntb_ndev(ndev);
	/* [한국어] 다시 걸 BAR 의 서술자. */
	struct pci_epf_bar *epf_bar;
	/* [한국어] 그 BAR 번호. */
	enum pci_barno barno;
	/* [한국어] BAR 설정 실패를 담는다. */
	int ret;
	/* [한국어] 로그용 device 포인터. */
	struct device *dev;

	/* [한국어] NTB 디바이스 기준으로 로그를 남긴다. */
	dev = &ntb->ntb.dev;
	/* [한국어] 창 번호 idx 에 대응하는 BAR 번호. BAR_MW1 부터 세므로 창 0 이 BAR_MW1 이다. */
	barno = ntb->epf_ntb_bar[BAR_MW1 + idx];
	/* [한국어] 그 BAR 의 서술자. */
	epf_bar = &ntb->epf->bar[barno];
	/* [한국어] NTB 클라이언트가 준 DMA 주소를 BAR 뒤에 건다. 이 주소는 보통
	 * ntb_transport 가 잡은 수신 버퍼다 — 즉 호스트가 이 창에 쓰면
	 * 곧바로 그 버퍼에 들어간다. */
	epf_bar->phys_addr = addr;
	/* [한국어] 서술자에 BAR 번호를 채운다. */
	epf_bar->barno = barno;
	/* [한국어] 클라이언트가 요청한 크기. */
	epf_bar->size = size;

	/* [한국어] BAR 를 다시 건다. 함수 번호를 0, 0 으로 박아 넣은 점에 주의 —
	 * ntb->epf->func_no 를 쓰지 않는다. 상류 코드 그대로 둔다. */
	ret = pci_epc_set_bar(ntb->epf->epc, 0, 0, epf_bar);
	/* [한국어] 실패하면 창이 서지 않는다. */
	if (ret) {
		/* [한국어] NTB 디바이스 기준으로 남긴다. */
		dev_err(dev, "failure set mw trans\n");
		return ret;
	}
	return 0;
}

/* [한국어]
 * vntb_epf_mw_clear_trans - 창 매핑 해제 요청을 받아들이지만 아무 것도 하지 않는다
 *
 * @ntb: NTB 디바이스
 * @pidx: 상대 포트 인덱스
 * @idx: 창 번호
 * @return: 항상 0(성공)
 *
 * 왜 필요한가: 연산 표에 자리는 필요하지만, 이 구현은 BAR 를 걷지 않는다.
 * 실제 정리는 unbind 경로의 epf_ntb_mw_bar_clear() 가 한꺼번에 한다.
 * 클라이언트가 창을 재설정하려면 mw_set_trans 를 다시 부르면 되므로
 * 중간에 걷을 필요가 없다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥.
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_mw_clear_trans() → [vntb_epf_mw_clear_trans]
 */
static int vntb_epf_mw_clear_trans(struct ntb_dev *ntb, int pidx, int idx)
{
	return 0;
}

/* [한국어]
 * vntb_epf_peer_mw_get_addr - 상대 창에 쓰려면 어디에 써야 하는지 알려 준다
 *
 * @ndev: NTB 디바이스
 * @idx: 창 번호
 * @base: 물리 주소를 담을 곳(NULL 이면 건너뛴다)
 * @size: 크기를 담을 곳(NULL 이면 건너뛴다)
 * @return: 항상 0
 *
 * 왜 필요한가: NTB 클라이언트가 상대(호스트) 메모리에 쓰려면 아웃바운드
 * 창의 물리 주소를 알아야 한다. 그 주소를 ioremap 해 쓰면 데이터가 창을
 * 지나 호스트로 나간다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_peer_mw_get_addr() → [vntb_epf_peer_mw_get_addr]
 */
static int vntb_epf_peer_mw_get_addr(struct ntb_dev *ndev, int idx,
				phys_addr_t *base, resource_size_t *size)
{

	/* [한국어] NTB 디바이스에서 이 EPF 상태로 되돌아온다. */
	struct epf_ntb *ntb = ntb_ndev(ndev);

	/* [한국어] 호출자가 주소를 원하면 아웃바운드 창의 물리 주소를 알려 준다.
	 * 이 주소가 곧 "상대 창에 쓰려면 여기에 써라" 는 뜻이다. */
	if (base)
		/* [한국어] epf_ntb_mw_bar_init() 이 pci_epc_mem_alloc_addr 로 얻어 둔 값이다. */
		*base = ntb->vpci_mw_phy[idx];

	/* [한국어] 호출자가 크기를 원하면 사용자가 정한 창 크기를 알려 준다. */
	if (size)
		/* [한국어] configfs 의 mwN 값이다. */
		*size = ntb->mws_size[idx];

	return 0;
}

/* [한국어]
 * vntb_epf_link_enable - 링크 활성화 요청을 받아들이지만 아무 것도 하지 않는다
 *
 * @ntb: NTB 디바이스
 * @max_speed: 요청 속도(무시)
 * @max_width: 요청 폭(무시)
 * @return: 항상 0(성공)
 *
 * 왜 필요한가: 실제 PCIe 링크를 여기서 올릴 수는 없다 — 링크는 호스트가
 * 이 엔드포인트를 열거하는 순간 이미 서 있다. 소프트웨어 링크 상태는
 * 호스트가 COMMAND_LINK_UP 을 보낼 때 epf_ntb_link_up() 이 올린다.
 * 그래서 여기서는 할 일이 없다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥.
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_link_enable() → [vntb_epf_link_enable]
 */
static int vntb_epf_link_enable(struct ntb_dev *ntb,
			enum ntb_speed max_speed,
			enum ntb_width max_width)
{
	return 0;
}

/* [한국어]
 * vntb_epf_spad_read - 내 스크래치패드(뒤쪽 절반)에서 한 워드를 읽는다
 *
 * @ndev: NTB 디바이스
 * @idx: 워드 인덱스
 * @return: 읽은 32비트 값
 *
 * 왜 필요한가: 스크래치패드는 두 절반으로 놓여 있고, 로컬 쪽에서 "내 것"
 * 은 뒤쪽 절반이다. 주소 계산이 오프셋 + 절반크기 + 인덱스x4 인 이유가 그것이다.
 * 같은 자리를 호스트는 자기 peer 스크래치패드로 읽는다.
 *
 * 주의: idx 에 대한 범위 검사가 없다. 호출자가 spad_count 안의 값을
 * 준다고 전제한다. 상류 코드 그대로 둔다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트).
 * readl 을 쓰는 것은 이 영역이 호스트와 공유되기 때문이다.
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_spad_read() → [vntb_epf_spad_read]
 */
static u32 vntb_epf_spad_read(struct ntb_dev *ndev, int idx)
{
	/* [한국어] NTB 디바이스에서 이 EPF 상태로 되돌아온다. */
	struct epf_ntb *ntb = ntb_ndev(ndev);
	/* [한국어] 스크래치패드 시작 오프셋과 절반 크기를 한 줄에서 계산한다. */
	int off = ntb->reg->spad_offset, ct = ntb->reg->spad_count * sizeof(u32);
	/* [한국어] 읽어 낼 값. */
	u32 val;
	/* [한국어] MMIO 접근용 기준 주소. */
	void __iomem *base = (void __iomem *)ntb->reg;

	/* [한국어] 뒤쪽 절반(내 몫)에서 읽는다. */
	val = readl(base + off + ct + idx * sizeof(u32));
	/* [한국어] 읽은 값을 그대로 돌려준다. */
	return val;
}

/* [한국어]
 * vntb_epf_spad_write - 내 스크래치패드(뒤쪽 절반)에 한 워드를 쓴다
 *
 * @ndev: NTB 디바이스
 * @idx: 워드 인덱스
 * @val: 쓸 값
 * @return: 항상 0
 *
 * 왜 필요한가: 여기에 쓴 값을 호스트가 자기 peer 스크래치패드에서 읽는다.
 * 이것이 두 쪽이 짧은 상태 값을 주고받는 통로다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_spad_write() → [vntb_epf_spad_write]
 */
static int vntb_epf_spad_write(struct ntb_dev *ndev, int idx, u32 val)
{
	/* [한국어] NTB 디바이스에서 이 EPF 상태로 되돌아온다. */
	struct epf_ntb *ntb = ntb_ndev(ndev);
	/* [한국어] 공유 레지스터. */
	struct epf_ntb_ctrl *ctrl = ntb->reg;
	/* [한국어] 스크래치패드 시작 오프셋과 절반 크기(워드 수 x 4바이트). */
	int off = ctrl->spad_offset, ct = ctrl->spad_count * sizeof(u32);
	/* [한국어] MMIO 접근용 기준 주소. */
	void __iomem *base = (void __iomem *)ntb->reg;

	/* [한국어] 오프셋 + 절반 크기만큼 더 나아간 자리, 즉 뒤쪽 절반에 쓴다.
	 * 앞쪽 절반은 상대(호스트) 몫이고 뒤쪽 절반이 내 몫이다. */
	writel(val, base + off + ct + idx * sizeof(u32));
	return 0;
}

/* [한국어]
 * vntb_epf_peer_spad_read - 상대 스크래치패드(앞쪽 절반)에서 한 워드를 읽는다
 *
 * @ndev: NTB 디바이스
 * @pidx: 상대 포트 인덱스(쓰지 않는다)
 * @idx: 워드 인덱스
 * @return: 읽은 32비트 값
 *
 * 왜 필요한가: 앞쪽 절반이 호스트가 "자기 스크래치패드" 로 쓰는 자리다.
 * 그래서 여기서 읽으면 호스트가 써 넣은 값이 나온다.
 * 절반 크기를 더하지 않는 것이 vntb_epf_spad_read 와의 유일한 차이다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_peer_spad_read() → [vntb_epf_peer_spad_read]
 */
static u32 vntb_epf_peer_spad_read(struct ntb_dev *ndev, int pidx, int idx)
{
	/* [한국어] NTB 디바이스에서 이 EPF 상태로 되돌아온다. */
	struct epf_ntb *ntb = ntb_ndev(ndev);
	/* [한국어] 공유 레지스터. */
	struct epf_ntb_ctrl *ctrl = ntb->reg;
	/* [한국어] 스크래치패드 영역이 시작되는 오프셋. */
	int off = ctrl->spad_offset;
	/* [한국어] MMIO 접근용 기준 주소. */
	void __iomem *base = (void __iomem *)ntb->reg;
	/* [한국어] 읽어 낼 값. */
	u32 val;

	/* [한국어] peer 쪽 절반에서 읽는다. 호스트가 써 넣은 값을 보게 된다. */
	val = readl(base + off + idx * sizeof(u32));
	/* [한국어] 읽은 값을 그대로 돌려준다. */
	return val;
}

/* [한국어]
 * vntb_epf_peer_spad_write - 상대 스크래치패드(앞쪽 절반)에 한 워드를 쓴다
 *
 * @ndev: NTB 디바이스
 * @pidx: 상대 포트 인덱스(쓰지 않는다)
 * @idx: 워드 인덱스
 * @val: 쓸 값
 * @return: 항상 0
 *
 * 왜 필요한가: 호스트가 자기 스크래치패드로 읽는 자리에 직접 써 넣는다.
 * NTB 스크래치패드 규약상 "상대 것에 쓰고 내 것에서 읽는" 방식과
 * "내 것에 쓰고 상대 것에서 읽는" 방식이 모두 쓰이므로 양쪽 연산이 다 있다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_peer_spad_write() → [vntb_epf_peer_spad_write]
 */
static int vntb_epf_peer_spad_write(struct ntb_dev *ndev, int pidx, int idx, u32 val)
{
	/* [한국어] NTB 디바이스에서 이 EPF 상태로 되돌아온다. */
	struct epf_ntb *ntb = ntb_ndev(ndev);
	/* [한국어] 공유 레지스터. */
	struct epf_ntb_ctrl *ctrl = ntb->reg;
	/* [한국어] 스크래치패드 영역이 시작되는 오프셋. */
	int off = ctrl->spad_offset;
	/* [한국어] MMIO 접근용 기준 주소. 이 영역은 dma_alloc_coherent 로 잡혔지만
	 * 호스트와 공유되므로 readl/writel 로 접근한다. */
	void __iomem *base = (void __iomem *)ntb->reg;

	/* [한국어] peer 쪽 절반(앞쪽 spad_count 워드)에 쓴다. 이 절반은 호스트가
	 * "자기 스크래치패드" 로 읽는 자리다 — 파일 상단 그림의 좌우 대칭이 이것이다. */
	writel(val, base + off + idx * sizeof(u32));
	return 0;
}

/* [한국어]
 * vntb_epf_peer_db_set - 호스트의 도어벨을 울린다 = 호스트에게 MSI 를 쏜다
 *
 * @ndev: NTB 디바이스
 * @db_bits: 울릴 도어벨 비트
 * @return: 0 성공, 그 밖에는 pci_epc_raise_irq() 의 실패 코드
 *
 * 왜 필요한가: 도어벨의 로컬 → 호스트 방향이다. 반대 방향(호스트 → 로컬)은
 * 호스트가 도어벨 BAR 에 값을 써서 MSI 인터럽트나 폴링을 깨우는 완전히
 * 다른 경로를 쓴다.
 *
 * 인터럽트 번호 계산: ffs(db_bits) 는 가장 낮은 1 비트의 위치를 1-기반으로
 * 돌려준다. 거기에 1 을 더해 interrupt_num 을 만들고, 넘길 때 또 1 을 더한다.
 * 즉 비트 0 을 울리면 최종 벡터 번호가 3 이 된다.
 * pci_epc_raise_irq() 의 interrupt_num 은 1-기반이라 컨트롤러 드라이버가
 * 내부에서 1 을 뺀다(drivers/pci/controller/dwc/pcie-designware-ep.c 에서
 * interrupt_num - 1 로 쓰는 것을 확인했다). 이 어긋남은 상류 코드 그대로 둔다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트). epc->lock 을 잡는다.
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_peer_db_set() → [vntb_epf_peer_db_set]
 *     → pci_epc_raise_irq()
 */
static int vntb_epf_peer_db_set(struct ntb_dev *ndev, u64 db_bits)
{
	/* [한국어] 울릴 도어벨 비트에서 인터럽트 번호를 만든다.
	 * ffs()는 가장 낮은 1 비트의 위치를 1-기반으로 돌려준다.
	 * 여기에 1 을 더하고, 아래에서 또 1 을 더해 넘긴다 — 즉 비트 0 이면
	 * 최종적으로 벡터 번호 3 이 된다. 이 어긋남은 상류 코드 그대로 둔다.
	 * (비교: pci_epc_raise_irq() 의 interrupt_num 은 1-기반이라
	 *  컨트롤러 드라이버가 내부에서 1 을 뺀다.) */
	u32 interrupt_num = ffs(db_bits) + 1;
	/* [한국어] NTB 디바이스에서 이 EPF 상태로 되돌아온다. */
	struct epf_ntb *ntb = ntb_ndev(ndev);
	/* [한국어] EPC 콜백에 넘길 함수 번호들. */
	u8 func_no, vfunc_no;
	/* [한국어] 인터럽트 발생의 실패를 담는다. */
	int ret;

	/* [한국어] 이 EPF 의 물리 함수 번호. */
	func_no = ntb->epf->func_no;
	/* [한국어] 가상 함수 번호. */
	vfunc_no = ntb->epf->vfunc_no;

	/* [한국어] 호스트 쪽으로 MSI 를 쏜다. 이것이 "VHOST 가 HOST 의 도어벨을 울린다" 의
	 * 실체다. 반대 방향(HOST → VHOST)은 호스트가 도어벨 BAR 에 값을 써서
	 * 이쪽 인터럽트나 폴링을 깨우는 방식이라 경로가 완전히 다르다. */
	ret = pci_epc_raise_irq(ntb->epf->epc, func_no, vfunc_no,
				PCI_IRQ_MSI, interrupt_num + 1);
	/* [한국어] 인터럽트 발생 실패를 알린다. */
	if (ret)
		/* [한국어] NTB 디바이스 기준으로 로그를 남긴다. */
		dev_err(&ntb->ntb.dev, "Failed to raise IRQ\n");

	return ret;
}

/* [한국어]
 * vntb_epf_db_read - 밀린 도어벨 비트맵을 읽는다
 *
 * @ndev: NTB 디바이스
 * @return: 아직 처리되지 않은 도어벨 비트들
 *
 * 왜 필요한가: 도어벨 사건을 통지받은 클라이언트가 "어떤 도어벨이
 * 울렸는가" 를 확인하는 연산이다.
 *
 * 비트맵을 세우는 쪽이 인터럽트 문맥(epf_ntb_doorbell_handler)이거나
 * 워크 문맥(epf_ntb_cmd_handler)이므로 atomic64_read 로 읽는다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_db_read() → [vntb_epf_db_read]
 */
static u64 vntb_epf_db_read(struct ntb_dev *ndev)
{
	/* [한국어] NTB 디바이스에서 이 EPF 상태로 되돌아온다. */
	struct epf_ntb *ntb = ntb_ndev(ndev);

	/* [한국어] 밀린 도어벨 비트맵을 원자적으로 읽는다. 이 값을 세우는 쪽이
	 * 인터럽트 핸들러이므로 원자 연산이 필요하다. */
	return atomic64_read(&ntb->db);
}

/* [한국어]
 * vntb_epf_mw_get_align - 창의 주소/크기 정렬 요구와 최대 크기를 알려 준다
 *
 * @ndev: NTB 디바이스
 * @pidx: 상대 포트 인덱스(쓰지 않는다)
 * @idx: 창 번호
 * @addr_align: 시작 주소 정렬 요구를 담을 곳
 * @size_align: 크기 정렬 요구를 담을 곳
 * @size_max: 최대 크기를 담을 곳
 * @return: 항상 0
 *
 * 왜 필요한가: NTB 클라이언트가 창에 걸 버퍼를 잡기 전에 제약을 물어본다.
 * 주소는 4KB 경계에 맞춰야 하는데, 대부분의 EP 컨트롤러가 인바운드 주소의
 * 하위 비트를 잘라 버리기 때문이다. 크기 정렬은 요구하지 않는다(1).
 * 최대 크기는 사용자가 configfs 로 정한 창 크기다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_mw_get_align() → [vntb_epf_mw_get_align]
 */
static int vntb_epf_mw_get_align(struct ntb_dev *ndev, int pidx, int idx,
			resource_size_t *addr_align,
			resource_size_t *size_align,
			resource_size_t *size_max)
{
	/* [한국어] NTB 디바이스에서 이 EPF 상태로 되돌아온다. */
	struct epf_ntb *ntb = ntb_ndev(ndev);

	/* [한국어] 창의 시작 주소는 4KB 경계에 맞춰야 한다. 이 값은 BAR 를 다시 걸 때
	 * 컨트롤러가 하위 비트를 잘라 버리는 것을 고려한 보수적인 값이다. */
	if (addr_align)
		*addr_align = SZ_4K;

	/* [한국어] 크기 정렬은 요구하지 않는다(1 = 제약 없음). */
	if (size_align)
		*size_align = 1;

	/* [한국어] 쓸 수 있는 최대 크기는 사용자가 configfs 로 정한 창 크기다. */
	if (size_max)
		*size_max = ntb->mws_size[idx];

	return 0;
}

/* [한국어]
 * vntb_epf_link_is_up - 소프트웨어 링크 상태를 돌려준다
 *
 * @ndev: NTB 디바이스
 * @speed: 링크 속도를 담을 곳(채우지 않는다)
 * @width: 링크 폭을 담을 곳(채우지 않는다)
 * @return: 공유 레지스터의 link_status 값. 0 이 아니면 링크 업.
 *
 * 왜 필요한가: 클라이언트가 상대가 준비됐는지 확인하는 연산이다.
 * speed/width 를 채우지 않는 것은 여기서 다루는 것이 실제 PCIe 링크가
 * 아니라 호스트가 COMMAND_LINK_UP 으로 알린 소프트웨어 상태이기 때문이다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_link_is_up() → [vntb_epf_link_is_up]
 */
static u64 vntb_epf_link_is_up(struct ntb_dev *ndev,
			enum ntb_speed *speed,
			enum ntb_width *width)
{
	/* [한국어] NTB 디바이스에서 이 EPF 상태로 되돌아온다. */
	struct epf_ntb *ntb = ntb_ndev(ndev);

	/* [한국어] 공유 레지스터의 link_status 를 그대로 돌려준다.
	 * speed/width 인자는 채우지 않는다 — 실제 PCIe 링크 속도가 아니라
	 * 소프트웨어 링크 상태이기 때문이다. */
	return ntb->reg->link_status;
}

/* [한국어]
 * vntb_epf_db_clear_mask - 도어벨 마스크 해제 요청을 받아들이지만 아무 것도 하지 않는다
 *
 * @ndev: NTB 디바이스
 * @db_bits: 마스크를 풀 도어벨 비트
 * @return: 항상 0(성공)
 *
 * 왜 필요한가: db_set_mask 가 실제로 마스킹하지 않으므로 풀 것도 없다.
 * 연산 표에 자리만 채워 두어 클라이언트가 호출해도 문제가 없게 한다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥.
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_db_clear_mask() → [vntb_epf_db_clear_mask]
 */
static int vntb_epf_db_clear_mask(struct ntb_dev *ndev, u64 db_bits)
{
	return 0;
}

/* [한국어]
 * vntb_epf_db_clear - 처리한 도어벨 비트를 비트맵에서 지운다
 *
 * @ndev: NTB 디바이스
 * @db_bits: 지울 도어벨 비트
 * @return: 항상 0
 *
 * 왜 필요한가: 클라이언트가 도어벨을 처리한 뒤 이 연산으로 비트를 지워야
 * 다음 울림을 구분할 수 있다. 지우지 않으면 db_read 가 계속 같은 비트를
 * 돌려준다.
 *
 * atomic64_and(~db_bits, ...) 로 지우는 이유는, 세우는 쪽이 인터럽트
 * 문맥이거나 워크 문맥이라 읽기-수정-쓰기가 겹칠 수 있기 때문이다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_db_clear() → [vntb_epf_db_clear]
 */
static int vntb_epf_db_clear(struct ntb_dev *ndev, u64 db_bits)
{
	/* [한국어] NTB 디바이스에서 이 EPF 상태로 되돌아온다. */
	struct epf_ntb *ntb = ntb_ndev(ndev);

	/* [한국어] 처리한 도어벨 비트를 지운다. atomic64_and 로 원자적으로 지우는 이유는
	 * 도어벨을 세우는 쪽이 인터럽트 핸들러(epf_ntb_doorbell_handler)이거나
	 * 워크(epf_ntb_cmd_handler)라서, 읽기-수정-쓰기가 겹칠 수 있기 때문이다. */
	atomic64_and(~db_bits, &ntb->db);
	return 0;
}

/* [한국어]
 * vntb_epf_link_disable - 링크 비활성화 요청을 받아들이지만 아무 것도 하지 않는다
 *
 * @ntb: NTB 디바이스
 * @return: 항상 0(성공)
 *
 * 왜 필요한가: link_enable 과 대칭으로 자리만 채운다. 실제 PCIe 링크를
 * 이쪽에서 내릴 수 없고, 소프트웨어 링크 상태는 호스트가
 * COMMAND_LINK_DOWN 을 보낼 때 내려간다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥.
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_link_disable() → [vntb_epf_link_disable]
 */
static int vntb_epf_link_disable(struct ntb_dev *ntb)
{
	return 0;
}

/* [한국어]
 * vntb_epf_get_dma_dev - 실제 DMA 를 수행할 디바이스를 알려 준다
 *
 * @ndev: NTB 디바이스
 * @return: 엔드포인트 컨트롤러의 부모 device
 *
 * 왜 필요한가: NTB 클라이언트가 DMA 매핑을 하려면 어떤 디바이스 기준으로
 * 주소를 변환해야 하는지 알아야 한다. 그 주체는 가상 PCI 디바이스도
 * EPC 자신도 아니고, EPC 를 만든 SoC 의 PCIe 컨트롤러 하드웨어 디바이스다 —
 * DMA 마스크와 IOMMU 설정이 그 디바이스에 달려 있기 때문이다.
 *
 * 실행 컨텍스트: NTB 클라이언트 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   NTB 클라이언트 → ntb_get_dma_dev() → [vntb_epf_get_dma_dev]
 */
static struct device *vntb_epf_get_dma_dev(struct ntb_dev *ndev)
{
	/* [한국어] NTB 디바이스에서 이 EPF 상태로 되돌아온다. */
	struct epf_ntb *ntb = ntb_ndev(ndev);
	/* [한국어] 이 EPF 가 붙은 엔드포인트 컨트롤러. */
	struct pci_epc *epc = ntb->epf->epc;

	/* [한국어] 실제 DMA 를 수행할 디바이스는 컨트롤러 자신이 아니라 그 부모,
	 * 즉 SoC 의 PCIe 컨트롤러 하드웨어 디바이스다. DMA 마스크와
	 * 스트리밍 매핑이 그 디바이스 기준으로 관리되기 때문이다. */
	return epc->dev.parent;
}

/* [한국어] NTB 연산 표. drivers/ntb 코어가 이 표를 통해서만 이 드라이버를 부른다.
 * 각 항목이 하나의 NTB 기능에 대응하며, 여기 없는 기능은 지원하지 않는 것으로
 * 간주된다. pci_vntb_probe() 가 ndev->ntb.ops 에 이 표를 연결한다. */
static const struct ntb_dev_ops vntb_epf_ops = {
	/* [한국어] mw_count: 이 쪽에서 쓸 수 있는 메모리 윈도우 개수. */
	.mw_count		= vntb_epf_mw_count,
	/* [한국어] spad_count: 스크래치패드 워드 개수. */
	.spad_count		= vntb_epf_spad_count,
	/* [한국어] peer_mw_count: 상대(호스트) 쪽 창 개수. 여기서는 같은 값을 돌려준다. */
	.peer_mw_count		= vntb_epf_peer_mw_count,
	/* [한국어] db_valid_mask: 유효한 도어벨 비트 마스크. */
	.db_valid_mask		= vntb_epf_db_valid_mask,
	/* [한국어] db_set_mask: 도어벨 마스킹. 구현하지 않고 0 만 돌려준다. */
	.db_set_mask		= vntb_epf_db_set_mask,
	/* [한국어] mw_set_trans: 로컬 창을 특정 DMA 주소로 매핑한다(BAR 를 다시 건다). */
	.mw_set_trans		= vntb_epf_mw_set_trans,
	/* [한국어] mw_clear_trans: 그 매핑을 지운다. 구현하지 않는다. */
	.mw_clear_trans		= vntb_epf_mw_clear_trans,
	/* [한국어] peer_mw_get_addr: 상대 창의 물리 주소와 크기를 알려 준다. */
	.peer_mw_get_addr	= vntb_epf_peer_mw_get_addr,
	/* [한국어] link_enable: 링크를 올린다. 여기서는 할 일이 없다. */
	.link_enable		= vntb_epf_link_enable,
	/* [한국어] spad_read/spad_write: 내 스크래치패드 접근. */
	.spad_read		= vntb_epf_spad_read,
	.spad_write		= vntb_epf_spad_write,
	/* [한국어] peer_spad_read/peer_spad_write: 상대 스크래치패드 접근. */
	.peer_spad_read		= vntb_epf_peer_spad_read,
	.peer_spad_write	= vntb_epf_peer_spad_write,
	/* [한국어] peer_db_set: 상대(호스트)의 도어벨을 울린다 = MSI 를 쏜다. */
	.peer_db_set		= vntb_epf_peer_db_set,
	/* [한국어] db_read: 밀린 도어벨 비트맵을 읽는다. */
	.db_read		= vntb_epf_db_read,
	/* [한국어] mw_get_align: 창의 주소/크기 정렬 요구를 알려 준다. */
	.mw_get_align		= vntb_epf_mw_get_align,
	/* [한국어] link_is_up: 링크 상태를 읽는다. */
	.link_is_up		= vntb_epf_link_is_up,
	/* [한국어] db_clear_mask: 도어벨 마스크 해제. 구현하지 않는다. */
	.db_clear_mask		= vntb_epf_db_clear_mask,
	/* [한국어] db_clear: 처리한 도어벨 비트를 지운다. */
	.db_clear		= vntb_epf_db_clear,
	/* [한국어] link_disable: 링크를 내린다. 구현하지 않는다. */
	.link_disable		= vntb_epf_link_disable,
	/* [한국어] get_dma_dev: DMA 를 수행할 실제 디바이스를 알려 준다. */
	.get_dma_dev		= vntb_epf_get_dma_dev,
};

/* [한국어]
 * pci_vntb_probe - 가상 PCI 장치에 붙어 NTB 디바이스를 등록한다
 *
 * @pdev: vpci_scan_bus() 가 만든 가상 PCI 디바이스
 * @id: 매칭된 장치 표 항목
 * @return: 0 성공, 그 밖에는 DMA 마스크 설정이나 NTB 등록의 실패 코드
 *
 * 왜 필요한가: 이 함수가 vntb 구조의 정점이다. EP 쪽 커널 안에 NTB
 * 디바이스를 실제로 태어나게 해, ntb_transport 나 ntb_netdev 같은
 * 클라이언트가 붙을 수 있게 한다. 형제 파일 pci-epf-ntb.c 에는 이런
 * 경로가 아예 없다 — 그쪽의 NTB 클라이언트는 두 호스트 쪽 커널에서 돈다.
 *
 * 동작 단계:
 *   (1) pdev->sysdata 에 심어 둔 함수 상태를 꺼낸다.
 *   (2) NTB 디바이스에 pdev, 토폴로지(NTB_TOPO_NONE), 연산 표를 채운다.
 *   (3) DMA 마스크를 32비트로 고정한다.
 *   (4) ntb_register_device() 로 NTB 서브시스템에 등록한다.
 *
 * 실행 컨텍스트: pci_bus_add_devices() 안, 즉 bind 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   epf_ntb_bind() → vpci_scan_bus() → pci_bus_add_devices()
 *     → [pci_vntb_probe] → ntb_register_device()
 */
static int pci_vntb_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	/* [한국어] NTB 디바이스 등록의 실패를 담는다. */
	int ret;
	/* [한국어] 가상 PCI 디바이스의 sysdata 에 심어 둔 함수 상태를 꺼낸다.
	 * vpci_scan_bus() 가 pci_scan_bus() 의 sysdata 인자로 넘긴 그 포인터다. */
	struct epf_ntb *ndev = (struct epf_ntb *)pdev->sysdata;
	/* [한국어] 로그와 DMA 마스크 설정에 쓸 device 포인터. */
	struct device *dev = &pdev->dev;

	/* [한국어] NTB 디바이스가 어느 PCI 디바이스에 얹혀 있는지 기록한다.
	 * drivers/ntb 쪽 코드가 이 포인터로 DMA 나 로그 대상을 찾는다. */
	ndev->ntb.pdev = pdev;
	/* [한국어] 토폴로지를 NTB_TOPO_NONE 으로 둔다. 실제 NTB 하드웨어의 B2B 나
	 * PRI/SEC 같은 배치가 아니라 소프트웨어로 만든 다리이기 때문이다. */
	ndev->ntb.topo = NTB_TOPO_NONE;
	/* [한국어] NTB 연산 표를 연결한다. 이 순간부터 drivers/ntb 의 클라이언트가
	 * vntb_epf_ 계열 함수를 통해 이 EPF 를 조작하게 된다. */
	ndev->ntb.ops =  &vntb_epf_ops;

	/* [한국어] DMA 마스크를 32비트로 고정한다. 이 가상 PCI 디바이스는 실제
	 * 하드웨어가 아니라 주소 변환 능력을 알 수 없으므로 보수적으로 잡는다. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	/* [한국어] 마스크 설정 실패는 이후 DMA 를 쓸 수 없다는 뜻이라 치명적이다. */
	if (ret) {
		/* [한국어] 원인을 남긴다. */
		dev_err(dev, "Cannot set DMA mask\n");
		return ret;
	}

	/* [한국어] drivers/ntb 서브시스템에 이 디바이스를 등록한다. 이것이 vntb 와
	 * 형제 파일 pci-epf-ntb.c 를 가르는 핵심이다 — 이쪽은 EP 쪽 커널 안에
	 * NTB 디바이스를 직접 만들어 ntb_transport/ntb_netdev 같은 클라이언트가
	 * 붙을 수 있게 한다. (drivers/ntb 는 이 트리에 체크아웃되어 있지 않아
	 * 구현은 확인할 수 없고, 호출부만 확인했다.) */
	ret = ntb_register_device(&ndev->ntb);
	/* [한국어] 등록 실패는 NTB 기능 전체가 서지 않는다는 뜻이다. */
	if (ret) {
		/* [한국어] 원인을 남긴다. */
		dev_err(dev, "Failed to register NTB device\n");
		return ret;
	}

	/* [한국어] 등록 성공을 디버그 로그로 남긴다. */
	dev_dbg(dev, "PCI Virtual NTB driver loaded\n");
	return 0;
}

/* [한국어] 가상 PCI 드라이버가 매칭할 장치 표. 벤더/디바이스 ID 는 bind 시점에
 * 사용자 설정값으로 덮어써진다. */
static struct pci_device_id pci_vntb_table[] = {
	{
		/* [한국어] 초기값 0xffff/0xffff 는 자리 표시자다. 이 값 그대로는 실제 장치와
		 * 겹치지 않으므로 안전하다. */
		PCI_DEVICE(0xffff, 0xffff),
	},
	/* [한국어] 빈 항목이 표의 끝을 뜻한다. */
	{},
};

/* [한국어] 가상 PCI 버스에 등록할 드라이버. */
static struct pci_driver vntb_pci_driver = {
	/* [한국어] 드라이버 이름. */
	.name           = "pci-vntb",
	/* [한국어] 위에서 정의한 매칭 표. */
	.id_table       = pci_vntb_table,
	/* [한국어] probe: 매칭되면 NTB 디바이스로 등록한다. */
	.probe          = pci_vntb_probe,
};

/* ============ PCIe EPF Driver Bind ====================*/

/**
 * epf_ntb_bind() - Initialize endpoint controller to provide NTB functionality
 * @epf: NTB endpoint function device
 *
 * Initialize both the endpoint controllers associated with NTB function device.
 * Invoked when a primary interface or secondary interface is bound to EPC
 * device. This function will succeed only when EPC is bound to both the
 * interfaces.
 *
 * Returns: Zero for success, or an error code in case of failure
 */
/* [한국어]
 * epf_ntb_bind - EPC 링크가 걸릴 때 전체 초기화를 수행하는 진입점
 *
 * @epf: NTB 엔드포인트 함수 디바이스
 * @return: 0 성공(EPC 가 아직 없는 경우도 0), 그 밖에는 초기화 실패 코드
 *
 * 왜 필요한가: configfs 에서 사용자가 EPF 를 EPC 에 링크하면 EPF 코어가
 * 이 콜백을 부른다. vntb 는 EPC 하나만 있으면 되므로, 형제 파일과 달리
 * 보조 인터페이스를 기다리지 않는다.
 *
 * 동작 단계:
 *   (1) EPC 가 붙었는지 확인한다.
 *   (2) BAR 번호를 확정한다(사용자 지정 + 자동 배정).
 *   (3) 제어 + 스크래치패드 메모리를 잡는다.
 *   (4) EPC 를 설정하고 폴링 워크를 띄운다.
 *   (5) 가상 PCI 장치의 벤더/디바이스 ID 를 사용자 설정값으로 채운다.
 *   (6) 가상 PCI 드라이버를 등록한다.
 *   (7) 가상 PCI 버스를 만들어 스캔한다 — 여기서 pci_vntb_probe 가 불려
 *       NTB 디바이스가 태어난다.
 *
 * 실행 컨텍스트: configfs 심볼릭 링크 생성(사용자 ln) 문맥, 프로세스 컨텍스트.
 *
 * 에러 경로: err_unregister → err_epc_cleanup → err_bar_alloc 순으로
 * 흘러 내리며 성공한 단계만 역순으로 취소한다.
 *
 * 호출 체인:
 *   configfs(pci_epc_epf_link) → pci_epf_bind() → [epf_ntb_bind]
 *     → epf_ntb_init_epc_bar() → epf_ntb_config_spad_bar_alloc()
 *     → epf_ntb_epc_init() → pci_register_driver() → vpci_scan_bus()
 */
static int epf_ntb_bind(struct pci_epf *epf)
{
	/* [한국어] EPF 디바이스에 매달아 둔 함수 상태를 꺼낸다. */
	struct epf_ntb *ntb = epf_get_drvdata(epf);
	/* [한국어] 로그용 device 포인터. */
	struct device *dev = &epf->dev;
	/* [한국어] 각 단계 실패를 담는다. */
	int ret;

	/* [한국어] EPC 가 아직 링크되지 않았다. */
	if (!epf->epc) {
		/* [한국어] 오류가 아니라 "아직" 이므로 dev_dbg 로만 남기고 성공을 돌려준다.
		 * ntb 쪽과 달리 여기서는 EPC 하나만 확인한다 — 이 드라이버는
		 * 엔드포인트 컨트롤러 하나와 로컬 가상 NTB 디바이스로 다리를 만들기 때문이다. */
		dev_dbg(dev, "PRIMARY EPC interface not yet bound\n");
		return 0;
	}

	/* [한국어] 1단계: 각 NTB 구성요소가 쓸 BAR 번호를 정한다. 사용자가 configfs 로
	 * 지정한 칸은 그대로 두고 나머지만 자동 배정한다. */
	ret = epf_ntb_init_epc_bar(ntb);
	/* [한국어] BAR 가 모자라면 여기서 실패한다. */
	if (ret) {
		/* [한국어] 상류 메시지 그대로. */
		dev_err(dev, "Failed to create NTB EPC\n");
		return ret;
	}

	/* [한국어] 2단계: 제어 + 스크래치패드 영역의 메모리를 잡는다.
	 * vntb 는 스크래치패드를 자기 몫과 상대 몫 두 벌로 잡는다는 점이 다르다. */
	ret = epf_ntb_config_spad_bar_alloc(ntb);
	/* [한국어] 할당 실패는 되돌릴 것이 생긴 첫 지점이다. */
	if (ret) {
		/* [한국어] 상류 메시지 그대로. */
		dev_err(dev, "Failed to allocate BAR memory\n");
		goto err_bar_alloc;
	}

	/* [한국어] 3단계: BAR 를 걸고 도어벨을 만들고 창을 잡고 폴링 워크를 띄운다. */
	ret = epf_ntb_epc_init(ntb);
	/* [한국어] 실패하면 2단계에서 잡은 메모리를 되돌린다. */
	if (ret) {
		/* [한국어] 안쪽에서 단계별 원인을 이미 찍었다. */
		dev_err(dev, "Failed to initialize EPC\n");
		goto err_bar_alloc;
	}

	/* [한국어] 모든 초기화가 끝난 뒤 다시 한 번 매단다. probe 에서 이미 했으므로
	 * 중복이지만 상류 코드 그대로 둔다. */
	epf_set_drvdata(epf, ntb);

	/* [한국어] 가상 PCI config 공간의 첫 워드(디바이스 ID + 벤더 ID)를 채운다.
	 * 상위 16비트가 디바이스 ID, 하위 16비트가 벤더 ID 라는 PCI 규약을 따른다.
	 * 이 값은 사용자가 configfs 의 vntb_pid/vntb_vid 로 정한다. */
	pci_space[0] = (ntb->vntb_pid << 16) | ntb->vntb_vid;
	/* [한국어] 가상 PCI 드라이버의 매칭 표도 같은 값으로 채운다. 그래야 아래에서
	 * 버스를 스캔할 때 pci_vntb_probe 가 매칭된다.
	 * 전역 표를 실행 중에 고쳐 쓰는 방식이라 이 모듈은 EPF 함수 하나만
	 * 다룰 수 있다는 제약이 생긴다. */
	pci_vntb_table[0].vendor = ntb->vntb_vid;
	/* [한국어] 디바이스 ID 도 채운다. */
	pci_vntb_table[0].device = ntb->vntb_pid;

	/* [한국어] 가상 PCI 드라이버를 실제 PCI 버스 타입에 등록한다. 아직 버스가 없어
	 * 매칭될 장치는 없지만, 아래 스캔 전에 등록되어 있어야 한다. */
	ret = pci_register_driver(&vntb_pci_driver);
	/* [한국어] 등록 실패는 NTB 디바이스를 만들 수 없다는 뜻이다. */
	if (ret) {
		/* [한국어] 원인을 남긴다. */
		dev_err(dev, "failure register vntb pci driver\n");
		goto err_epc_cleanup;
	}

	/* [한국어] 가상 PCI 버스를 만들어 스캔한다. 이 안에서 devfn 0 짜리 장치가
	 * 하나 발견되고, 위에서 등록한 드라이버가 매칭되어 pci_vntb_probe 가
	 * 불리며, 거기서 ntb_register_device() 로 drivers/ntb 에 등록된다.
	 * 이 한 줄이 "로컬 쪽 가상 NTB 디바이스" 를 만드는 지점이다. */
	ret = vpci_scan_bus(ntb);
	/* [한국어] 스캔 실패는 드라이버 등록을 되돌려야 한다. */
	if (ret)
		goto err_unregister;

	return 0;

/* [한국어] 스캔 실패 경로. 가상 PCI 드라이버를 등록 해제한다. */
err_unregister:
	pci_unregister_driver(&vntb_pci_driver);
/* [한국어] 드라이버 등록 실패 경로. EPC 설정을 되돌린다. */
err_epc_cleanup:
	epf_ntb_epc_cleanup(ntb);
/* [한국어] 메모리 할당 이후의 모든 실패가 거쳐 가는 지점. 제어 영역을 해제한다. */
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
 * epf_ntb_unbind - bind 가 만든 것을 역순으로 되돌린다
 *
 * @epf: NTB 엔드포인트 함수 디바이스
 * @return: 없음
 *
 * 왜 필요한가: 사용자가 configfs 링크를 지우거나 모듈이 내려갈 때
 * 하드웨어 설정, 메모리, 가상 PCI 드라이버를 모두 되돌려야 한다.
 *
 * 동작 단계: EPC 설정을 걷고 → 제어 메모리를 해제하고 →
 * 가상 PCI 드라이버를 등록 해제한다. 드라이버가 내려가면서
 * pci_vntb_probe 로 만들어진 NTB 디바이스도 함께 떨어진다.
 *
 * 주의: bind 가 만든 가상 PCI 버스 자체(vpci_scan_bus 의 pci_scan_bus)를
 * 제거하는 코드는 이 파일에 없다. 상류 코드 그대로 둔다.
 *
 * 실행 컨텍스트: configfs 링크 해제 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   configfs → pci_epf_unbind() → [epf_ntb_unbind]
 *     → epf_ntb_epc_cleanup() → epf_ntb_config_spad_bar_free()
 *     → pci_unregister_driver()
 */
static void epf_ntb_unbind(struct pci_epf *epf)
{
	/* [한국어] EPF 디바이스에 매달아 둔 함수 상태를 꺼낸다. */
	struct epf_ntb *ntb = epf_get_drvdata(epf);

	/* [한국어] EPC 설정(BAR, 도어벨 IRQ, 아웃바운드 창)을 모두 되돌리고 워크를 멈춘다. */
	epf_ntb_epc_cleanup(ntb);
	/* [한국어] 제어 + 스크래치패드 메모리를 해제한다. BAR 를 먼저 걷고 그 다음
	 * 메모리를 푸는 순서를 지킨다. */
	epf_ntb_config_spad_bar_free(ntb);

	/* [한국어] 가상 PCI 드라이버를 등록 해제한다. 이때 pci_vntb_probe 로 만들어진
	 * NTB 디바이스도 함께 떨어져 나간다.
	 * 주의: bind 가 만든 가상 PCI 버스 자체(vpci_scan_bus 의 pci_scan_bus)를
	 * 제거하는 코드는 이 파일에 없다. 상류 코드 그대로 둔다. */
	pci_unregister_driver(&vntb_pci_driver);
}

// EPF driver probe
static const struct pci_epf_ops epf_ntb_ops = {
	/* [한국어] EPF 코어가 부르는 콜백 묶음. 상류가 이 위에 // 주석으로 표시해 두었다. */
	.bind   = epf_ntb_bind,
	/* [한국어] bind: EPC 링크가 걸릴 때 불린다. */
	.unbind = epf_ntb_unbind,
	/* [한국어] unbind: 링크가 풀릴 때 정리한다. */
	.add_cfs = epf_ntb_add_cfs,
/* [한국어] add_cfs: configfs 에 이 함수 전용 디렉터리를 만든다. */
};

/**
 * epf_ntb_probe() - Probe NTB function driver
 * @epf: NTB endpoint function device
 * @id: NTB endpoint function device ID
 *
 * Probe NTB function driver when endpoint function bus detects a NTB
 * endpoint function.
 *
 * Returns: Zero for success, or an error code in case of failure
 */
/* [한국어]
 * epf_ntb_probe - EPF 가상 버스가 이 함수를 발견했을 때 상태만 만들어 둔다
 *
 * @epf: 새로 만들어진 NTB 엔드포인트 함수 디바이스
 * @id: 매칭된 이름 표 항목
 * @return: 0 성공, -ENOMEM 할당 실패
 *
 * 왜 필요한가: probe 시점에는 아직 어떤 EPC 에 붙을지 모르므로 하드웨어를
 * 건드리지 않고 상태만 잡는다. 실제 설정은 epf_ntb_bind() 에서 이뤄진다.
 *
 * 동작 단계: 상태를 0 초기화해 잡고, config 헤더를 연결하고, 가상 버스
 * 번호 기본값 0xff 를 넣고, BAR 배정표를 전부 NO_BAR 로 채운다.
 * 마지막이 중요하다 — NO_BAR 가 "아직 정해지지 않음" 을 뜻해,
 * 사용자가 configfs 로 지정하지 않은 칸만 나중에 자동 배정된다.
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
	/* [한국어] 로그와 devm_ 할당에 쓸 device 포인터. */
	struct device *dev;
	/* [한국어] BAR 배정표를 초기화할 반복자. */
	int i;

	/* [한국어] EPF 디바이스의 struct device. devm_ 로 잡은 메모리는 이 디바이스가
	 * 사라질 때 자동으로 풀린다. */
	dev = &epf->dev;

	/* [한국어] 함수 전체 상태를 0 으로 채워 할당한다. 0 초기화 덕분에 db, linkup,
	 * msi_doorbell 이 모두 안전한 초기값으로 시작한다. */
	ntb = devm_kzalloc(dev, sizeof(*ntb), GFP_KERNEL);
	/* [한국어] 할당 실패는 그대로 상위로 올린다. */
	if (!ntb)
		return -ENOMEM;

	/* [한국어] 호스트에게 내보일 PCI config 헤더를 연결한다. */
	epf->header = &epf_ntb_header;
	/* [한국어] 역방향 포인터. 모든 하위 함수가 ntb->epf 로 EPC 에 닿는다. */
	ntb->epf = epf;
	/* [한국어] 가상 PCI 버스의 번호 기본값 0xff. 사용자가 configfs 의 vbus_number 로
	 * 바꿀 수 있다. 실제 PCI 버스와 겹치지 않을 큰 값을 고른 것이다. */
	ntb->vbus_number = 0xff;

	/* Initially, no bar is assigned */
	for (i = 0; i < VNTB_BAR_NUM; i++)
		/* [한국어] BAR 배정표를 NO_BAR 로 채운다. 이 값이 "아직 정해지지 않음" 을 뜻하며,
		 * epf_ntb_find_bar() 가 이 조건으로 자동 배정 여부를 판단한다.
		 * 사용자가 configfs 의 ctrl_bar/db_bar/mwN_bar 에 값을 쓰면 그 칸만
		 * 채워지고 나머지는 자동으로 채워진다. */
		ntb->epf_ntb_bar[i] = NO_BAR;

	/* [한국어] EPF 디바이스에 이 상태를 매단다. */
	epf_set_drvdata(epf, ntb);

	/* [한국어] 적재 사실을 남긴다. 상류가 dev_info 를 쓴다 — 사용자가 EP 쪽에서
	 * 동작을 확인할 단서가 필요하기 때문으로 읽힌다. */
	dev_info(dev, "pci-ep epf driver loaded\n");
	return 0;
}

/* [한국어] 이 드라이버가 맡을 EPF 이름 표. 사용자가 configfs 에서
 * mkdir pci_epf_vntb.0 을 하면 이 이름으로 매칭된다. */
static const struct pci_epf_device_id epf_ntb_ids[] = {
	{
		/* [한국어] 이름이 곧 매칭 키다. */
		.name = "pci_epf_vntb",
	},
	/* [한국어] 빈 항목이 표의 끝을 뜻한다. */
	{},
};

/* [한국어] EPF 가상 버스에 등록할 드라이버 서술자. */
static struct pci_epf_driver epf_ntb_driver = {
	/* [한국어] 드라이버 이름. sysfs 의 pci-epf 버스 아래 이 이름으로 보인다. */
	.driver.name    = "pci_epf_vntb",
	/* [한국어] probe: 위에서 정의한 상태 할당 함수. */
	.probe          = epf_ntb_probe,
	/* [한국어] id_table: 위 이름 표. */
	.id_table       = epf_ntb_ids,
	/* [한국어] ops: bind/unbind/add_cfs 콜백 묶음. */
	.ops            = &epf_ntb_ops,
	/* [한국어] owner: 바인딩된 함수가 있는 동안 모듈 언로드를 막는다. */
	.owner          = THIS_MODULE,
};

/* [한국어]
 * epf_ntb_init - 모듈 적재 시각. 폴링 작업 큐를 만들고 EPF 드라이버를 등록한다
 *
 * @return: 0 성공, -ENOMEM 이면 작업 큐 생성 실패, 그 밖에는 드라이버 등록 실패 코드
 *
 * 왜 필요한가: 명령/도어벨 폴링 워크가 돌 전용 큐가 있어야 하고,
 * EPF 가상 버스에 드라이버가 등록되어야 사용자가 만든 함수가 매칭된다.
 *
 * 작업 큐 플래그의 의미:
 *   WQ_MEM_RECLAIM  메모리 회수 경로에서도 진행이 보장되도록 전용 구조 워커 확보
 *   WQ_HIGHPRI      폴링이 밀리지 않도록 높은 우선순위
 *   WQ_PERCPU       CPU 를 넘나들지 않는 per-CPU 워커 풀 사용
 *
 * 실행 컨텍스트: insmod 문맥(프로세스 컨텍스트).
 *
 * 에러 경로: 드라이버 등록 실패 시 앞서 만든 작업 큐를 반드시 되돌린다.
 *
 * 호출 체인:
 *   module_init → [epf_ntb_init] → alloc_workqueue() → pci_epf_register_driver()
 */
static int __init epf_ntb_init(void)
{
	/* [한국어] 작업 큐 생성과 드라이버 등록의 실패를 담을 변수. */
	int ret;

	/* [한국어] 명령 폴링 워크가 돌 전용 작업 큐를 만든다.
	 * WQ_MEM_RECLAIM: 메모리 회수 경로에서도 진행이 보장되도록 전용 구조
	 *   워커를 확보한다. NTB 위에 네트워크 장치가 올라갈 수 있어
	 *   회수 경로와 얽히는 것을 피해야 한다.
	 * WQ_HIGHPRI: 폴링 주기가 밀리지 않도록 높은 우선순위 워커를 쓴다.
	 * WQ_PERCPU: CPU 를 넘나들지 않는 per-CPU 워커 풀을 쓴다.
	 * 마지막 0 은 max_active 기본값(무제한)이다. */
	kpcintb_workqueue = alloc_workqueue("kpcintb",
				    WQ_MEM_RECLAIM | WQ_HIGHPRI | WQ_PERCPU, 0);
	/* [한국어] 큐를 못 만들면 폴링을 돌릴 수 없으므로 모듈 적재를 포기한다. */
	if (!kpcintb_workqueue) {
		/* [한국어] 아직 드라이버가 없어 dev_err 를 쓸 수 없으므로 pr_err 를 쓴다. */
		pr_err("Failed to allocate kpcintb workqueue\n");
		return -ENOMEM;
	}

	/* [한국어] EPF 가상 버스에 드라이버를 등록한다. 이 시점부터 사용자가 configfs 로
	 * 만든 pci_epf_vntb 함수가 이 드라이버에 매칭된다. */
	ret = pci_epf_register_driver(&epf_ntb_driver);
	/* [한국어] 등록 실패 시 앞서 만든 작업 큐를 되돌린다. */
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
 * 함수들의 unbind 가 불려 워크가 멈추고 가상 PCI 드라이버도 등록 해제된다.
 * 그 뒤에야 큐를 없앨 수 있다.
 *
 * 실행 컨텍스트: rmmod 문맥(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   module_exit → [epf_ntb_exit] → pci_epf_unregister_driver() → destroy_workqueue()
 */
static void __exit epf_ntb_exit(void)
{
	/* [한국어] 드라이버를 먼저 내린다. 이 호출 안에서 아직 바인딩된 함수가 있으면
	 * 각각의 unbind 가 불려 워크가 멈추고 가상 PCI 드라이버도 등록 해제된다. */
	pci_epf_unregister_driver(&epf_ntb_driver);
	/* [한국어] 모든 워크가 사라진 뒤에야 작업 큐를 없앤다. */
	destroy_workqueue(kpcintb_workqueue);
}
/* [한국어] 모듈 해제 시각의 진입점 등록. */
module_exit(epf_ntb_exit);

/* [한국어] modinfo 에 보일 설명. */
MODULE_DESCRIPTION("PCI EPF NTB DRIVER");
/* [한국어] 원저자 표기. 상류 그대로 둔다. */
MODULE_AUTHOR("Frank Li <Frank.li@nxp.com>");
/* [한국어] 라이선스 표기. */
MODULE_LICENSE("GPL v2");
