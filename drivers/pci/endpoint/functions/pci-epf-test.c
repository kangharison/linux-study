// SPDX-License-Identifier: GPL-2.0
/*
 * Test driver to test endpoint functionality
 *
 * Copyright (C) 2017 Texas Instruments
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */
/* [한국어] PCI 엔드포인트 시험용 함수 드라이버 (pci-epf-test.c)
 * 
 * === 파일의 역할 ===
 * 보통의 PCI 드라이버는 '호스트(RC, Root Complex) 쪽에서 장치를 다루는'
 * 코드다. 이 파일은 정반대다 - SoC 를 'PCI 장치처럼 보이게' 만드는 쪽,
 * 즉 엔드포인트(EP) 쪽 코드다. 그중에서도 이것은 실제 기능을 흉내 내지
 * 않는 시험 전용 함수다. 호스트에게 BAR 몇 개를 내보이고, 그 첫 BAR 에
 * '명령 레지스터' 를 두어 호스트가 명령을 써 넣으면 그에 맞는 동작을
 * 수행한 뒤 인터럽트로 알린다. 그렇게 해서 EP 컨트롤러 드라이버가
 * inbound/outbound 주소 변환, MSI/MSI-X/INTx 발생, DMA, BAR 부분 매핑을
 * 제대로 구현했는지 검증한다. 호스트 쪽 짝은 pci_endpoint_test 드라이버
 * (drivers/misc/pci_endpoint_test.c)인데, ★ 그 파일도 Documentation 도
 * 이 부분 체크아웃에는 없어 직접 확인하지 못했다 - 아래에서 '호스트' 라
 * 부르는 동작은 이 파일 안의 명령/상태 비트 정의로부터 읽어 낸 것이다.
 * 
 * === 전체 아키텍처에서의 위치 ===
 * 실행 컨텍스트는 커널 모듈이며, 엔드포인트 쪽 SoC 에서 돈다.
 * 계층은 세 겹이다:
 *   EPC 컨트롤러 드라이버(예: pcie-designware-ep.c) - 실제 하드웨어
 *     ^
 *   EPC/EPF 코어(pci-epc-core.c, pci-epf-core.c) - 추상화 계층
 *     ^
 *   이 파일(EPF 함수 드라이버) - 무엇을 내보일지 결정
 * EPF 는 pci_epf_bus_type 이라는 가상 버스에 올라가고, configfs 로
 * 사용자가 만든 EPF 인스턴스의 이름("pci_epf_test")과 이 드라이버의
 * id_table 이름이 일치하면 바인딩된다. 즉 유저스페이스가 configfs 를
 * 통해 '어떤 함수를 어느 컨트롤러에 붙일지' 를 조립한다.
 * 동작 시점은 셋으로 나뉜다:
 *   - bind()      : EPC 특성을 조회하고 BAR 뒤에 쓸 메모리를 할당
 *   - epc_init()  : 링크가 준비되면 설정 헤더를 쓰고 BAR 를 실제로 노출
 *   - cmd_handler : 1ms 주기로 명령 레지스터를 폴링하는 지연 작업
 * 
 * === 타 모듈과의 연결 ===
 * 아래로 부르는 것들(이웃 파일에서 확인한 사실):
 *   - pci_epf_alloc_space() [pci-epf-core.c] : dma_alloc_coherent 로 BAR 뒤
 *     메모리를 잡고 하드웨어 제약에 맞게 크기를 보정한다(최소 128 바이트,
 *     Resizable BAR 는 1MB, 고정 크기 BAR 는 그 값, 나머지는 2의 거듭제곱).
 *     BAR 크기와 정렬 크기가 달라질 수 있어 결과를 둘로 나눠 돌려준다.
 *   - pci_epc_set_bar() [pci-epc-core.c] : BAR 를 실제로 호스트에 노출한다.
 *   - pci_epc_raise_irq() : 호스트에 인터럽트를 올린다(interrupt_num 은 1-기반).
 *   - pci_epc_mem_map() : 호스트 메모리에 접근할 창을 빌려 주며 하드웨어
 *     정렬 제약을 흡수한다. 반환된 map.pci_size 가 요청보다 작을 수 있어,
 *     이 파일의 READ/WRITE/COPY 가 모두 while 루프로 나눠 처리한다.
 *   - pci_epf_align_inbound_addr() [pci-epf-core.c] : 임의 주소를 BAR 크기에
 *     맞춰 정렬하고 그 안의 오프셋을 돌려준다. 도어벨이 이것을 쓴다.
 *   - pci_epf_alloc_doorbell() [pci-ep-msi.c] : 플랫폼 MSI 도어벨을 확보한다.
 *   - dmaengine API : 선택적 DMA 전송 경로.
 * 위에서 이 파일을 부르는 것: EPF 코어가 probe/bind/unbind/add_cfs 를,
 * EPC 코어가 pci_epc_event_ops(epc_init/epc_deinit/link_up/link_down)를 부른다.
 * ★ NVMe 와의 관계: 없다. drivers/nvme 트리에서 pci_epf_ 계열 함수를
 * 호출하는 코드는 한 줄도 없음을 확인했다.
 * 
 * === 주요 함수/구조체 요약 ===
 *   - pci_epf_test_cmd_handler() : 1ms 주기로 명령 레지스터를 읽어 분기하는
 *     이 드라이버의 심장. 명령을 처리한 뒤 자기 자신을 다시 예약한다.
 *   - pci_epf_test_read()/write()/copy() : 호스트 메모리를 읽고/쓰고/복사한다.
 *     pci_epc_mem_map 이 한 번에 매핑해 주는 크기가 제한적이라 모두
 *     while 루프로 조각내며, CRC32 로 무결성을 검증한다.
 *   - pci_epf_test_raise_irq() : INTx/MSI/MSI-X 중 하나로 호스트를 깨운다.
 *     상태를 먼저 쓰고 인터럽트를 나중에 올리는 순서가 중요하다.
 *   - pci_epf_test_enable_doorbell() : 호스트가 BAR 에 쓰기만 하면 EP 쪽에
 *     인터럽트가 뜨는 '초인종' 을 만든다. BAR 의 inbound 변환을 임시로
 *     MSI 컨트롤러 주소로 돌려놓는 기법을 쓴다.
 *   - pci_epf_test_bar_subrange_setup() : BAR 하나를 두 조각으로 나눠
 *     물리 주소를 뒤바꿔 매핑하고 서명 바이트를 채워, 호스트가 순서가
 *     실제로 적용됐는지 확인할 수 있게 한다.
 *   - struct pci_epf_test     : 이 EPF 인스턴스의 모든 상태.
 *   - struct pci_epf_test_reg : 호스트와 주고받는 공유 레지스터 배치.
 *     __packed + __le32/__le64 로 엔디안과 패딩을 고정한다. */

/* [한국어] crc32_le - READ/WRITE 명령에서 전송한 데이터의 무결성을 확인한다.
 * EP 가 계산한 CRC 와 호스트가 계산한 CRC 를 비교해야 '데이터가 제대로
 * 갔는가' 를 알 수 있으므로 시험 드라이버에 반드시 필요하다. */
#include <linux/crc32.h>
/* [한국어] usleep_range - WRITE 명령 끝에서 1ms 쉬는 데 쓴다. 아래 주석대로
 * 그 대기가 없으면 호스트 쪽에서 L3 오류가 관측되었다고 한다. */
#include <linux/delay.h>
/* [한국어] dmaengine API 전체(dma_request_channel, dmaengine_prep_*, dma_cookie_t 등).
 * DMA 전송 경로는 선택 사항이며, 채널을 못 잡으면 CPU 복사로 물러선다. */
#include <linux/dmaengine.h>
/* [한국어] memcpy_fromio/memcpy_toio - 매핑된 호스트 메모리 창은 I/O 메모리라
 * 일반 memcpy 로 다루면 안 된다. */
#include <linux/io.h>
/* [한국어] MODULE_* 매크로와 module_init/exit, THIS_MODULE. */
#include <linux/module.h>
/* [한국어] struct msi_msg - 도어벨이 쓸 MSI 메시지의 주소와 데이터를 담는다.
 * 호스트가 그 주소에 그 데이터를 쓰면 EP 쪽에 인터럽트가 뜬다. */
#include <linux/msi.h>
/* [한국어] kzalloc/kfree/devm_kzalloc, kzalloc_objs. */
#include <linux/slab.h>
/* [한국어] PCI_CLASS_OTHERS - 설정 헤더의 base class 로 쓴다. 어떤 표준
 * 장치 종류에도 속하지 않는다는 뜻이라 시험용 함수에 알맞다. */
#include <linux/pci_ids.h>
/* [한국어] get_random_bytes - WRITE 명령이 보낼 데이터를 난수로 채운다.
 * 0 이나 반복 패턴이면 전송이 실제로 일어났는지 구별하기 어렵기 때문이다. */
#include <linux/random.h>

/* [한국어] EPC(엔드포인트 컨트롤러) 계층의 API. pci_epc_set_bar, pci_epc_mem_map,
 * pci_epc_raise_irq, pci_epc_get_features 와 struct pci_epc_features,
 * enum pci_barno 가 여기 있다. */
#include <linux/pci-epc.h>
/* [한국어] EPF(엔드포인트 함수) 계층의 API. struct pci_epf, pci_epf_bar,
 * pci_epf_alloc_space, pci_epf_register_driver 가 여기 있다. */
#include <linux/pci-epf.h>
/* [한국어] 도어벨용 플랫폼 MSI 헬퍼 - pci_epf_alloc_doorbell/free_doorbell.
 * CONFIG_PCI_ENDPOINT_MSI_DOORBELL 이 꺼져 있으면 -ENODATA 를 돌려주는
 * 빈 껍데기가 대신 들어온다. */
#include <linux/pci-ep-msi.h>
/* [한국어] PCI 스펙이 정한 상수들. 이 파일은 PCI_MSIX_ENTRY_SIZE(16 바이트)를
 * MSI-X 표 크기 계산에 쓴다. */
#include <linux/pci_regs.h>

/* [한국어] IRQ_TYPE_* - 호스트가 irq_type 레지스터에 써 넣는 인터럽트 종류 값.
 * INTx 는 전통적인 레벨 트리거 핀 인터럽트다(엔드포인트가 INTA 핀을
 * 어서트한다). 아래 세 값은 연속된 0/1/2 라, cmd_handler 가
 * 'irq_type > IRQ_TYPE_MSIX' 하나로 범위를 검증할 수 있다. */
#define IRQ_TYPE_INTX			0
/* [한국어] MSI - 메시지 신호 인터럽트. 장치가 약속된 주소에 값을 써서 알린다. */
#define IRQ_TYPE_MSI			1
/* [한국어] MSI-X - MSI 의 확장판. 벡터마다 주소/데이터를 따로 둘 수 있어
 * 훨씬 많은 벡터를 쓸 수 있다. */
#define IRQ_TYPE_MSIX			2

/* [한국어] COMMAND_* - 호스트가 command 레지스터에 써 넣는 명령 비트.
 * 각 명령이 서로 다른 비트를 차지하지만 cmd_handler 의 switch 는 값
 * 전체를 비교하므로, 실제로는 한 번에 하나만 유효하다.
 * 이 비트는 INTx 인터럽트를 한 번 올려 달라는 뜻이다. */
#define COMMAND_RAISE_INTX_IRQ		BIT(0)
/* [한국어] MSI 인터럽트를 올려 달라 - 몇 번 벡터인지는 irq_number 로 온다. */
#define COMMAND_RAISE_MSI_IRQ		BIT(1)
/* [한국어] MSI-X 인터럽트를 올려 달라. */
#define COMMAND_RAISE_MSIX_IRQ		BIT(2)
/* [한국어] READ - 호스트 메모리(src_addr)에서 size 바이트를 읽어 CRC 를 검증하라. */
#define COMMAND_READ			BIT(3)
/* [한국어] WRITE - 난수 데이터를 만들어 호스트 메모리(dst_addr)에 쓰고
 * 그 CRC 를 checksum 레지스터에 남겨라. */
#define COMMAND_WRITE			BIT(4)
/* [한국어] COPY - 호스트 메모리 src_addr 에서 dst_addr 로 옮겨라. EP 를
 * 거치는 왕복 경로라 읽기와 쓰기를 한 번에 시험한다. */
#define COMMAND_COPY			BIT(5)
/* [한국어] 도어벨을 켜라 - 호스트가 BAR 에 쓰기만 하면 EP 에 인터럽트가
 * 뜨도록 설정한다. */
#define COMMAND_ENABLE_DOORBELL		BIT(6)
/* [한국어] 도어벨을 꺼라. */
#define COMMAND_DISABLE_DOORBELL	BIT(7)
/* [한국어] BAR 부분 매핑을 설정하라. ★ 이 두 명령에서만 size 레지스터가
 * 크기가 아니라 'BAR 번호' 를 나른다(아래 코드의 주석이 밝힌다). */
#define COMMAND_BAR_SUBRANGE_SETUP	BIT(8)
/* [한국어] BAR 부분 매핑을 되돌려라. */
#define COMMAND_BAR_SUBRANGE_CLEAR	BIT(9)

/* [한국어] STATUS_* - EP 가 status 레지스터에 세우는 결과 비트.
 * 호스트는 인터럽트를 받은 뒤 이 레지스터를 읽어 무엇이 어떻게 끝났는지
 * 판단한다. 성공과 실패에 따로 비트를 준 이유는 '아직 아무 일도 없음'(0)과
 * 구별하기 위해서다. 이 비트는 READ 명령이 CRC 검증까지 통과한 경우다. */
#define STATUS_READ_SUCCESS		BIT(0)
/* [한국어] READ 실패 - 매핑 실패, 메모리 부족, CRC 불일치를 모두 포함한다. */
#define STATUS_READ_FAIL		BIT(1)
/* [한국어] WRITE 성공. */
#define STATUS_WRITE_SUCCESS		BIT(2)
/* [한국어] WRITE 실패. */
#define STATUS_WRITE_FAIL		BIT(3)
/* [한국어] COPY 성공. */
#define STATUS_COPY_SUCCESS		BIT(4)
/* [한국어] COPY 실패. */
#define STATUS_COPY_FAIL		BIT(5)
/* [한국어] 인터럽트를 올렸음. raise_irq 가 인터럽트를 쏘기 '전에' 세운다. */
#define STATUS_IRQ_RAISED		BIT(6)
/* [한국어] src_addr 매핑 실패 - 호스트가 준 주소가 컨트롤러의 outbound 창으로
 * 매핑할 수 없는 값이었다는 뜻이다. */
#define STATUS_SRC_ADDR_INVALID		BIT(7)
/* [한국어] dst_addr 매핑 실패. */
#define STATUS_DST_ADDR_INVALID		BIT(8)
/* [한국어] 도어벨이 실제로 울렸음 - 도어벨 인터럽트 핸들러가 세운다. */
#define STATUS_DOORBELL_SUCCESS		BIT(9)
/* [한국어] 도어벨 켜기 성공. */
#define STATUS_DOORBELL_ENABLE_SUCCESS	BIT(10)
/* [한국어] 도어벨 켜기 실패. */
#define STATUS_DOORBELL_ENABLE_FAIL	BIT(11)
/* [한국어] 도어벨 끄기 성공. */
#define STATUS_DOORBELL_DISABLE_SUCCESS BIT(12)
/* [한국어] 도어벨 끄기 실패. */
#define STATUS_DOORBELL_DISABLE_FAIL	BIT(13)
/* [한국어] BAR 부분 매핑 설정 성공. */
#define STATUS_BAR_SUBRANGE_SETUP_SUCCESS	BIT(14)
/* [한국어] BAR 부분 매핑 설정 실패. */
#define STATUS_BAR_SUBRANGE_SETUP_FAIL		BIT(15)
/* [한국어] BAR 부분 매핑 해제 성공. */
#define STATUS_BAR_SUBRANGE_CLEAR_SUCCESS	BIT(16)
/* [한국어] BAR 부분 매핑 해제 실패. */
#define STATUS_BAR_SUBRANGE_CLEAR_FAIL		BIT(17)
/* [한국어] 자원 부족 - 부분 매핑에 필요한 inbound 창(iATU 등)이 모자랐다.
 * FAIL 과 함께 세워져, 실패 사유가 '설정 오류' 가 아니라 '하드웨어 자원
 * 한계' 임을 호스트에게 구별해 알린다. */
#define STATUS_NO_RESOURCE		BIT(18)

/* [한국어] FLAG_USE_DMA - 호스트가 flags 레지스터에 세우면 CPU 복사 대신
 * dmaengine 경로를 쓴다. 두 경로의 처리율을 비교할 수 있게 하는 스위치다. */
#define FLAG_USE_DMA			BIT(0)

/* [한국어] TIMER_RESOLUTION - ★ 이 파일 안에서 정의만 되어 있고 쓰이는 곳이
 * 한 군데도 없다(확인함). 예전에 타이머 주기를 나타내던 잔재로 보이며,
 * 현재 cmd_handler 재예약은 msecs_to_jiffies(1) 을 직접 쓴다. */
#define TIMER_RESOLUTION		1

/* [한국어] CAP_* - EP 가 caps 레지스터에 세워 호스트에게 알리는 '나는 무엇을
 * 할 수 있는가'. 호스트 시험 프로그램은 이 비트를 보고 어떤 시험을
 * 건너뛸지 정한다. 이 비트는 정렬되지 않은 주소로도 매핑할 수 있다는 뜻으로,
 * EPC 가 align_addr 연산을 구현했는지로 판정한다. */
#define CAP_UNALIGNED_ACCESS		BIT(0)
/* [한국어] MSI 를 쓸 수 있다. */
#define CAP_MSI				BIT(1)
/* [한국어] MSI-X 를 쓸 수 있다. */
#define CAP_MSIX			BIT(2)
/* [한국어] INTx 를 쓸 수 있다. */
#define CAP_INTX			BIT(3)
/* [한국어] BAR 부분 매핑을 쓸 수 있다. 아래 CAP_DYNAMIC_INBOUND_MAPPING 에
 * 의존하는 기능이라 두 조건을 함께 확인해야 세워진다. */
#define CAP_SUBRANGE_MAPPING		BIT(4)
/* [한국어] 이미 설정된 BAR 의 inbound 매핑을 clear_bar 없이 다시 바꿀 수 있다.
 * 도어벨 기능이 이 성질에 기대어 set_bar 를 두 번 부른다. */
#define CAP_DYNAMIC_INBOUND_MAPPING	BIT(5)
/* [한국어] BAR0 이 하드웨어 전용(BAR_RESERVED)이라 쓸 수 없다. */
#define CAP_BAR0_RESERVED		BIT(6)
/* [한국어] BAR1 이 예약됨. */
#define CAP_BAR1_RESERVED		BIT(7)
/* [한국어] BAR2 가 예약됨. */
#define CAP_BAR2_RESERVED		BIT(8)
/* [한국어] BAR3 이 예약됨. */
#define CAP_BAR3_RESERVED		BIT(9)
/* [한국어] BAR4 가 예약됨. */
#define CAP_BAR4_RESERVED		BIT(10)
/* [한국어] BAR5 가 예약됨. 호스트는 이 여섯 비트를 보고 어느 BAR 로
 * 시험할지 고른다. */
#define CAP_BAR5_RESERVED		BIT(11)

/* [한국어] BAR 부분 매핑 시험에서 BAR 하나를 몇 조각으로 나눌지. 2 로 두어
 * '앞 절반과 뒤 절반을 맞바꾼다' 는 가장 단순하면서도 순서가 실제로
 * 적용됐는지 확실히 드러나는 배치를 만든다. */
#define PCI_EPF_TEST_BAR_SUBRANGE_NSUB	2

/* [한국어] kpcitest_workqueue - 명령 폴링 작업이 도는 전용 워크큐.
 * 설정자: pci_epf_test_init() 이 모듈 적재 때 만들고,
 *         pci_epf_test_exit() 이 파괴한다.
 * 읽는 자: cmd_handler 를 예약하는 모든 지점(epc_init, link_up,
 *         cmd_handler 자신의 재예약).
 * 값 범위: 유효한 워크큐 포인터. 생성 실패면 모듈 적재 자체가 실패한다.
 * 동기화: 워크큐 내부가 알아서 처리한다. 전용 큐를 쓰는 이유는
 *         WQ_MEM_RECLAIM 과 WQ_HIGHPRI 가 필요해서다 - 1ms 주기의
 *         폴링이 다른 작업에 밀리면 시험 지연이 커진다. */
static struct workqueue_struct *kpcitest_workqueue;

/* [한국어] pci_epf_test - 이 EPF 인스턴스 하나의 모든 상태.
 * probe 에서 devm_kzalloc 으로 잡고 epf_set_drvdata 로 epf 에 매단다. */
struct pci_epf_test {
	/* [한국어] reg - BAR 마다 할당한 메모리의 커널 가상 주소.
	 * 설정자: pci_epf_test_alloc_space() 가 pci_epf_alloc_space 의 반환값으로
	 *         채우고, free_space/set_bar 실패 경로가 NULL 로 되돌린다.
	 * 읽는 자: test_reg_bar 자리는 pci_epf_test_reg 구조체로 캐스팅해
	 *         호스트와 주고받는 레지스터로 쓰고, 나머지는 부분 매핑 시험의
	 *         서명 바이트를 채우는 데 쓴다.
	 * 값 범위: 유효한 포인터이거나 NULL(그 BAR 를 쓰지 않음).
	 * 동기화: bind/unbind 시점에만 바뀌고, 그 사이에는 cmd_handler 만 읽는다. */
	void			*reg[PCI_STD_NUM_BARS];
	/* [한국어] epf - 이 상태 구조체가 붙어 있는 EPF 코어 객체.
	 * 설정자: pci_epf_test_probe().
	 * 읽는 자: 거의 모든 함수가 epf->epc, epf->bar[], epf->func_no 를 얻는 통로.
	 * 값 범위: 유효한 포인터. NULL 이 될 일이 없다.
	 * 동기화: probe 이후 바뀌지 않는다. */
	struct pci_epf		*epf;
	/* [한국어] group - configfs 그룹. bar0_size ~ bar5_size 속성이 여기 달린다.
	 * 설정자: pci_epf_test_add_cfs() 가 이름과 타입을 붙여 초기화한다.
	 * 읽는 자: configfs 코어. 속성 show/store 는 이 멤버에서 container_of 로
	 *         pci_epf_test 를 되찾는다.
	 * 값 범위: configfs 그룹 구조체.
	 * 동기화: configfs 코어가 관리한다. */
	struct config_group	group;
	/* [한국어] test_reg_bar - 호스트와 주고받는 레지스터를 담을 BAR 번호.
	 * 설정자: pci_epf_test_bind() 가 pci_epc_get_first_free_bar 로 고른다 -
	 *         하드웨어가 예약해 둔 BAR 를 피해 첫 번째 쓸 수 있는 것을 잡는다.
	 * 읽는 자: alloc_space, set_bar, cmd_handler, set_capabilities 등.
	 * 값 범위: BAR_0 ~ BAR_5. bind 가 실패하면 -EINVAL 로 끝나므로 NO_BAR 는 없다.
	 * 동기화: bind 시점에만 정해진다. */
	enum pci_barno		test_reg_bar;
	/* [한국어] msix_table_offset - test_reg_bar 안에서 MSI-X 표가 시작하는 오프셋.
	 * 설정자: pci_epf_test_alloc_space() 가 레지스터 구조체 크기(128 정렬)로 잡는다.
	 * 읽는 자: epc_init 이 pci_epc_set_msix 에 넘긴다 - 하드웨어에게
	 *         'MSI-X 표가 이 BAR 의 이 오프셋에 있다' 고 알리는 값이다.
	 * 값 범위: 0 이상. msix_capable 이 아니면 0 으로 남는다.
	 * 동기화: bind 시점에만 정해진다. */
	size_t			msix_table_offset;
	/* [한국어] cmd_handler - 1ms 주기로 명령 레지스터를 폴링하는 지연 작업.
	 * 설정자: probe 가 INIT_DELAYED_WORK 로 초기화. 예약은 epc_init/link_up/
	 *         자기 자신이, 취소는 epc_deinit/link_down/unbind 가 한다.
	 * 읽는 자: 워크큐 코어.
	 * 값 범위: 지연 작업 구조체.
	 * 동기화: ★ 취소는 반드시 _sync 판이어야 한다 - 실행 중인 핸들러가
	 *         끝나기를 기다리지 않으면 해제된 메모리를 계속 읽는다. */
	struct delayed_work	cmd_handler;
	/* [한국어] dma_chan_tx - EP -> 호스트 방향 DMA 채널.
	 * 설정자: pci_epf_test_init_dma_chan(). 해제는 clean_dma_chan.
	 * 읽는 자: pci_epf_test_data_transfer() 가 방향에 따라 고른다.
	 * 값 범위: 유효한 채널이거나 NULL. 전용 채널을 못 잡으면 범용 memcpy
	 *         채널 하나를 rx 와 공유하므로 두 포인터가 같은 값일 수 있다.
	 * 동기화: bind/unbind 시점에만 바뀐다. */
	struct dma_chan		*dma_chan_tx;
	/* [한국어] dma_chan_rx - 호스트 -> EP 방향 DMA 채널.
	 * 설정자/읽는 자/값 범위/동기화: dma_chan_tx 와 같다.
	 * ★ 두 필드가 같은 포인터일 수 있어, clean_dma_chan 이 그 경우를
	 * 따로 처리한다(한 번만 반납하고 둘 다 NULL 로). */
	struct dma_chan		*dma_chan_rx;
	/* [한국어] transfer_chan - 지금 진행 중인 전송이 쓰는 채널.
	 * 설정자: pci_epf_test_data_transfer() 가 전송을 띄우기 직전에.
	 * 읽는 자: 완료 콜백이 dmaengine_tx_status 를 물을 때.
	 * 값 범위: dma_chan_tx 또는 dma_chan_rx 중 하나.
	 * 동기화: 전송이 한 번에 하나뿐이므로 별도 락이 없다 - cmd_handler 가
	 *         단일 작업이라 동시에 두 전송이 뜨지 않는다. */
	struct dma_chan		*transfer_chan;
	/* [한국어] transfer_cookie - dmaengine 이 준 전송 식별자.
	 * 설정자: dmaengine_submit() 의 반환값.
	 * 읽는 자: dma_submit_error() 검사와 완료 콜백의 상태 조회.
	 * 값 범위: 양수 쿠키이거나 오류. dma_submit_error 로 판정한다.
	 * 동기화: 위와 같다. */
	dma_cookie_t		transfer_cookie;
	/* [한국어] transfer_status - 완료 콜백이 확인한 전송 결과.
	 * 설정자: pci_epf_test_dma_callback().
	 * 읽는 자: pci_epf_test_data_transfer() 가 대기에서 깨어난 뒤.
	 * 값 범위: DMA_COMPLETE/DMA_ERROR/DMA_IN_PROGRESS 등.
	 * 동기화: 콜백이 쓴 뒤 complete() 를 부르고, 읽는 쪽은 wait 에서
	 *         깨어난 뒤에 읽으므로 순서가 보장된다. */
	enum dma_status		transfer_status;
	/* [한국어] transfer_complete - 전송 완료를 기다리는 completion 객체.
	 * 설정자: init_dma_chan 이 초기화, 전송마다 reinit_completion 으로 되돌린다.
	 * 읽는 자: wait_for_completion_interruptible.
	 * 값 범위: completion 구조체.
	 * 동기화: completion 자체가 내부 스핀락을 갖는다. */
	struct completion	transfer_complete;
	/* [한국어] dma_supported - DMA 경로를 쓸 수 있는가.
	 * 설정자: epc_init 이 true 로 시작해, init_dma_chan 이 실패하면 false 로.
	 * 읽는 자: cmd_handler 가 FLAG_USE_DMA 요청을 거절할지 판단하고,
	 *         clean_dma_chan 이 정리할 것이 있는지 본다.
	 * 값 범위: true/false.
	 * 동기화: epc_init 시점에만 바뀐다. */
	bool			dma_supported;
	/* [한국어] dma_private - 전용(슬레이브) 채널을 잡았는가.
	 * 설정자: init_dma_chan 이 전용 tx/rx 를 모두 잡았을 때만 true.
	 * 읽는 자: data_transfer 가 슬레이브 전송(주소를 따로 설정)과
	 *         memcpy 전송 중 어느 쪽을 준비할지 가른다.
	 * 값 범위: true/false. 범용 채널로 물러섰다면 false 로 남는다.
	 * 동기화: epc_init 시점에만 바뀐다. */
	bool			dma_private;
	/* [한국어] epc_features - 이 EPC 가 무엇을 지원하는지 담은 읽기 전용 서술자.
	 * 설정자: pci_epf_test_bind() 가 pci_epc_get_features 로 얻는다.
	 * 읽는 자: set_capabilities, alloc_space, epc_init, 부분 매핑 명령 처리.
	 * 값 범위: EPC 드라이버가 소유한 const 구조체 포인터.
	 * 동기화: 컨트롤러가 살아 있는 동안 바뀌지 않는다. */
	const struct pci_epc_features *epc_features;
	/* [한국어] db_bar - 도어벨을 위해 임시로 덮어쓸 BAR 서술자.
	 * 설정자: pci_epf_test_enable_doorbell() 이 barno/size/flags 를 원래 BAR 에서
	 *         베끼고, phys_addr 만 MSI 컨트롤러 주소로 바꿔 채운다.
	 * 읽는 자: pci_epc_set_bar() 가 이 서술자로 inbound 변환을 다시 건다.
	 * 값 범위: 유효한 BAR 서술자. 도어벨을 켜지 않았으면 0 으로 남는다.
	 * 동기화: cmd_handler 안에서만 다뤄진다.
	 * ★ 이 필드가 따로 있는 이유: epf->bar[] 를 직접 고치면 원래의
	 *    phys_addr 을 잃어 도어벨을 끌 때 되돌릴 수 없다. */
	struct pci_epf_bar	db_bar;
	/* [한국어] bar_size - BAR 마다 할당할 크기. configfs 로 사용자가 바꿀 수 있다.
	 * 설정자: probe 가 default_bar_size[] 로 초기화하고,
	 *         configfs 의 barN_size store 가 덮어쓴다(2의 거듭제곱만 허용).
	 * 읽는 자: pci_epf_test_alloc_space().
	 * 값 범위: 2의 거듭제곱. 고정 크기 BAR 는 이 값이 무시된다.
	 * 동기화: ★ EPC 에 바인딩된 뒤에는 바꿀 수 없다 - store 함수가
	 *         epf->epc 가 있으면 -EOPNOTSUPP 로 거절한다. alloc_space 가
	 *         bind 에서 한 번만 불리기 때문이다. */
	size_t			bar_size[PCI_STD_NUM_BARS];
};

/* [한국어] pci_epf_test_reg - 호스트와 EP 가 공유하는 레지스터 배치.
 * test_reg_bar 의 맨 앞에 놓이며, 호스트는 이 BAR 를 ioremap 해서
 * 필드를 읽고 쓴다. 즉 이것이 두 CPU 사이의 ABI 다.
 * ★ 그래서 두 가지 장치가 반드시 필요하다:
 *   - __packed : 컴파일러가 패딩을 넣으면 양쪽 배치가 어긋난다.
 *   - __le32/__le64 : PCI 는 리틀엔디안이므로, 빅엔디안 SoC 에서도
 *     같은 바이트 순서가 되도록 타입으로 강제하고 접근 시마다
 *     le32_to_cpu/cpu_to_le32 로 변환한다. */
struct pci_epf_test_reg {
	/* [한국어] magic - 이 구조체가 유효한지 호스트가 확인하는 표식.
	 * 설정자: ★ 이 파일 안에는 없다. 선언 말고는 등장하지 않음을 확인했다.
	 * 읽는 자: 호스트 쪽 드라이버로 보이나 그 파일이 이 트리에 없어
	 *         확인하지 못했다.
	 * 값 범위: 확인 불가.
	 * 동기화: 해당 없음. 여기서는 배치를 맞추기 위한 자리 차지 역할만 한다. */
	__le32 magic;
	/* [한국어] command - 호스트가 요청할 명령(COMMAND_* 비트).
	 * 설정자: 호스트가 쓰고, cmd_handler 가 처리 직후 0 으로 지운다.
	 * 읽는 자: cmd_handler 가 READ_ONCE 로 읽는다.
	 * 값 범위: COMMAND_* 중 하나. 0 이면 할 일 없음.
	 * 동기화: ★ 락이 없다 - 호스트와 EP 가 서로 다른 CPU 라 커널 락으로는
	 *         보호할 수 없다. 대신 READ_ONCE/WRITE_ONCE 로 컴파일러가
	 *         접근을 쪼개거나 합치지 못하게 막고, '호스트는 명령을 쓰고
	 *         EP 가 지울 때까지 기다린다' 는 프로토콜 규약에 기댄다. */
	__le32 command;
	/* [한국어] status - EP 가 알리는 결과(STATUS_* 비트).
	 * 설정자: 각 명령 처리 함수와 raise_irq.
	 * 읽는 자: 호스트가 인터럽트를 받은 뒤 읽는다.
	 * 값 범위: STATUS_* 의 OR 조합.
	 * 동기화: command 와 같다. ★ raise_irq 가 인터럽트를 올리기 '전에'
	 *         이 값을 쓰는 순서가 프로토콜상 중요하다. */
	__le32 status;
	/* [한국어] src_addr - READ/COPY 의 원본이 되는 호스트 물리 주소.
	 * 설정자: 호스트.
	 * 읽는 자: pci_epf_test_read()/copy() 가 le64_to_cpu 로 읽어
	 *         pci_epc_mem_map 에 넘긴다.
	 * 값 범위: 호스트가 준 64 비트 PCI 주소. EP 는 유효성을 검사하지 않고
	 *         매핑 실패로 판별한다(STATUS_SRC_ADDR_INVALID).
	 * 동기화: command 와 같다. */
	__le64 src_addr;
	/* [한국어] dst_addr - WRITE/COPY 의 목적지 호스트 물리 주소.
	 * 설정자: 호스트.
	 * 읽는 자: pci_epf_test_write()/copy().
	 * 값 범위: src_addr 과 같다.
	 * 동기화: command 와 같다. */
	__le64 dst_addr;
	/* [한국어] size - 전송할 바이트 수. ★ 단 BAR_SUBRANGE_* 명령에서는
	 * 'BAR 번호' 를 나른다(코드 안 주석이 그렇게 밝힌다) - 레지스터를
	 * 새로 늘리지 않으려고 필드를 겸용한 것이다.
	 * 설정자: 호스트.
	 * 읽는 자: read/write/copy 는 전송 길이로, 부분 매핑 두 함수는 BAR 번호로.
	 * 값 범위: 전송 길이일 때는 kzalloc 이 감당할 크기, BAR 번호일 때는
	 *         0 ~ PCI_STD_NUM_BARS-1(코드가 검사한다).
	 * 동기화: command 와 같다. */
	__le32 size;
	/* [한국어] checksum - 전송 데이터의 CRC32.
	 * 설정자: READ 에서는 호스트가 미리 채워 두고, WRITE 에서는 EP 가
	 *         보낸 데이터의 CRC 를 여기에 남긴다 - 방향에 따라 쓰는 쪽이 다르다.
	 * 읽는 자: READ 에서는 EP 가 비교용으로, WRITE 에서는 호스트가.
	 * 값 범위: crc32_le(~0, ...) 의 결과.
	 * 동기화: command 와 같다. */
	__le32 checksum;
	/* [한국어] irq_type - 어떤 종류의 인터럽트를 올릴지(IRQ_TYPE_*).
	 * 설정자: 호스트.
	 * 읽는 자: cmd_handler 가 범위를 검증하고 raise_irq 가 분기한다.
	 * 값 범위: 0~2. 그보다 크면 cmd_handler 가 명령 자체를 버린다.
	 * 동기화: command 와 같다. */
	__le32 irq_type;
	/* [한국어] irq_number - MSI/MSI-X 의 몇 번 벡터를 쓸지. 1-기반이다.
	 * 설정자: 호스트.
	 * 읽는 자: raise_irq 가 pci_epc_get_msi/msix 로 얻은 개수와 비교해
	 *         범위를 검증한 뒤 pci_epc_raise_irq 에 넘긴다.
	 * 값 범위: 1 ~ 설정된 벡터 수. INTx 에서는 무시된다.
	 * 동기화: command 와 같다. */
	__le32 irq_number;
	/* [한국어] flags - 전송 방식 옵션(FLAG_USE_DMA).
	 * 설정자: 호스트.
	 * 읽는 자: cmd_handler 가 DMA 가능 여부와 대조하고, read/write/copy 가
	 *         경로를 고른다.
	 * 값 범위: FLAG_USE_DMA 또는 0.
	 * 동기화: command 와 같다. */
	__le32 flags;
	/* [한국어] caps - EP 가 알려 주는 능력 비트(CAP_*).
	 * 설정자: pci_epf_test_set_capabilities() 가 epc_init 에서 한 번 채운다.
	 * 읽는 자: 호스트.
	 * 값 범위: CAP_* 의 OR 조합.
	 * 동기화: 링크가 올라오기 전에 한 번만 쓰이므로 경쟁이 없다. */
	__le32 caps;
	/* [한국어] doorbell_bar - 도어벨이 배정된 BAR 번호.
	 * 설정자: enable_doorbell 이 채우고, doorbell_cleanup 이 NO_BAR(-1)로 되돌린다.
	 * 읽는 자: 호스트가 어느 BAR 에 초인종을 누를지 알아내고,
	 *         disable_doorbell 이 어느 BAR 를 되돌릴지 판단한다.
	 * 값 범위: BAR_0~BAR_5 또는 NO_BAR.
	 * 동기화: command 와 같다. */
	__le32 doorbell_bar;
	/* [한국어] doorbell_offset - 그 BAR 안에서 초인종을 누를 오프셋.
	 * 설정자: enable_doorbell 이 pci_epf_align_inbound_addr 의 결과로 채운다.
	 * 읽는 자: 호스트.
	 * 값 범위: BAR 크기 안의 오프셋. MSI 컨트롤러 주소를 BAR 크기로
	 *         내림 정렬했을 때의 나머지다.
	 * 동기화: command 와 같다. */
	__le32 doorbell_offset;
	/* [한국어] doorbell_data - 초인종을 누를 때 써야 하는 값(MSI 메시지 데이터).
	 * 설정자: enable_doorbell 이 epf->db_msg[0].msg.data 에서 베낀다.
	 * 읽는 자: 호스트. 이 값을 위 오프셋에 써야 EP 쪽 인터럽트가 뜬다.
	 * 값 범위: 플랫폼 MSI 컨트롤러가 정한 값.
	 * 동기화: command 와 같다. */
	__le32 doorbell_data;
} __packed;

/* [한국어] test_header - 호스트에게 내보일 PCI 설정 공간 헤더.
 * epc_init 이 pci_epc_write_header 로 하드웨어에 써 넣는다.
 * 설정자: 여기서 정적으로. 읽는 자: EPC 드라이버.
 * 동기화: 링크가 올라오기 전에 한 번 쓰이므로 경쟁이 없다. */
static struct pci_epf_header test_header = {
	/* [한국어] 벤더 ID 를 PCI_ANY_ID 로 둔다. 시험용이라 특정 벤더를 사칭하지
	 * 않으며, 실제 값은 configfs 로 사용자가 지정하거나 EPC 드라이버가
	 * 정하도록 남겨 둔 것이다. */
	.vendorid	= PCI_ANY_ID,
	/* [한국어] 장치 ID 도 마찬가지. */
	.deviceid	= PCI_ANY_ID,
	/* [한국어] base class 0xFF - '기타'. 어떤 표준 장치 종류에도 속하지 않으므로
	 * 호스트의 일반 드라이버가 우연히 이 함수에 바인딩되지 않는다. */
	.baseclass_code = PCI_CLASS_OTHERS,
	/* [한국어] INTx 를 쓸 때 어느 핀을 어서트할지 - INTA 다. MSI/MSI-X 만
	 * 쓴다면 의미가 없지만, INTx 시험을 위해 채워 둔다. */
	.interrupt_pin	= PCI_INTERRUPT_INTA,
};

/* default BAR sizes, can be overridden by the user using configfs */
/* [한국어] default_bar_size - BAR 0~5 의 기본 할당 크기. 앞의 다섯은 128KB,
 * BAR5 만 1MB 다. BAR5 가 큰 이유는 이 트리 안에서 근거를 찾지 못했다 -
 * Resizable BAR 의 최소 단위가 1MB 라는 점(pci_epf_alloc_space 가 그렇게
 * 보정한다)과 관련이 있어 보이나 확인하지는 못했다.
 * 설정자: 컴파일 시점 상수. 읽는 자: probe 가 인스턴스별 bar_size[] 로 복사.
 * 동기화: 읽기 전용으로만 쓰인다. */
static size_t default_bar_size[] = { 131072, 131072, 131072, 131072, 131072, 1048576 };

/* [한국어] pci_epf_test_dma_callback - DMA 전송이 끝났을 때 dmaengine 이 부르는 콜백
 * 
 * @param: 전송을 띄울 때 tx->callback_param 에 넣어 둔 pci_epf_test 포인터.
 * @return: 없음
 * 
 * ★ 왜 상태를 다시 물어보는가: 콜백이 불렸다는 사실만으로는 성공인지
 * 실패인지 알 수 없다. dmaengine 은 완료와 오류 모두에서 같은 콜백을
 * 부르므로, dmaengine_tx_status() 로 쿠키의 최종 상태를 물어야 한다.
 * 
 * ★ 왜 조건을 걸고 complete() 를 부르는가: 상태가 아직
 * DMA_IN_PROGRESS 라면 진짜 완료가 아니다(콜백이 중간 단계에서 불릴
 * 여지를 남긴 방어다). 그 경우 대기자를 깨우지 않으므로, 실제 완료
 * 콜백이 다시 올 때까지 기다린다.
 * 
 * 실행 컨텍스트: dmaengine 드라이버가 정하는데, 보통 인터럽트 문맥이나
 * tasklet 이다. 잠들 수 없으므로 여기서는 상태 저장과 complete() 만 한다.
 * 
 * 호출 체인:
 *   (DMA 컨트롤러 인터럽트) -> dmaengine 드라이버 -> [pci_epf_test_dma_callback]
 *     -> complete() -> pci_epf_test_data_transfer() 의 대기를 깨운다 */
static void pci_epf_test_dma_callback(void *param)
{
	/* [한국어] 콜백 인자로 넘겨받은 EPF 상태 구조체. */
	struct pci_epf_test *epf_test = param;
	/* [한국어] dmaengine_tx_status 가 채워 줄 진행 상태 구조체. 이 파일은
	 * 잔여 바이트 수 같은 세부 정보를 쓰지 않지만, API 가 포인터를
	 * 요구하므로 자리만 마련한다. */
	struct dma_tx_state state;

	/* [한국어] 전송이 어떻게 끝났는지 물어 구조체에 저장한다. 기다리는 쪽이
	 * 깨어난 뒤 이 값을 읽어 성공/실패를 판정한다. */
	epf_test->transfer_status =
		dmaengine_tx_status(epf_test->transfer_chan,
				    epf_test->transfer_cookie, &state);
	/* [한국어] 정상 완료거나 */
	if (epf_test->transfer_status == DMA_COMPLETE ||
	    /* [한국어] 오류로 끝났다면 - 어느 쪽이든 '끝났다' 는 사실은 같다. */
	    epf_test->transfer_status == DMA_ERROR)
		/* [한국어] 기다리던 pci_epf_test_data_transfer 를 깨운다. 실패도 깨워야
		 * 한다 - 그러지 않으면 오류 시 영원히 매달린다. */
		complete(&epf_test->transfer_complete);
}

/**
 * pci_epf_test_data_transfer() - Function that uses dmaengine API to transfer
 *				  data between PCIe EP and remote PCIe RC
 * @epf_test: the EPF test device that performs the data transfer operation
 * @dma_dst: The destination address of the data transfer. It can be a physical
 *	     address given by pci_epc_mem_alloc_addr or DMA mapping APIs.
 * @dma_src: The source address of the data transfer. It can be a physical
 *	     address given by pci_epc_mem_alloc_addr or DMA mapping APIs.
 * @len: The size of the data transfer
 * @dma_remote: remote RC physical address
 * @dir: DMA transfer direction
 *
 * Function that uses dmaengine API to transfer data between PCIe EP and remote
 * PCIe RC. The source and destination address can be a physical address given
 * by pci_epc_mem_alloc_addr or the one obtained using DMA mapping APIs.
 *
 * The function returns '0' on success and negative value on failure.
 */
/* [한국어] 위 kernel-doc 이 인자와 반환값을 이미 설명하고 있다.
 * 여기서는 그 설명이 다루지 않는 '왜 이런 구조인가' 를 덧붙인다.
 * 
 * ★ 두 가지 전송 방식이 있는 이유:
 *   - 전용(슬레이브) 채널 : EP 컨트롤러에 내장된 DMA 엔진처럼, 한쪽 끝이
 *     'PCI 버스 너머의 주소' 로 고정된 채널이다. 그래서 상대 주소를
 *     dmaengine_slave_config 로 따로 알려 주고, 전송은 로컬 주소 하나만
 *     지정하는 prep_slave_single 로 띄운다. dma_remote 인자가 그 상대
 *     주소이며, 이 방식에서만 의미가 있다.
 *   - 범용 memcpy 채널 : 두 물리 주소 사이를 그냥 복사하는 채널이다.
 *     호스트 메모리가 이미 EP 의 물리 주소 공간에 매핑되어 있으므로
 *     (pci_epc_mem_map 이 해 준 일이다) 평범한 memcpy 로 다룰 수 있다.
 *   어느 쪽인지는 dma_private 플래그가 가른다.
 * 
 * ★ 방향에 따라 채널과 로컬 주소를 고르는 규칙:
 *   DMA_MEM_TO_DEV(EP -> 호스트) 이면 tx 채널, 로컬은 src.
 *   그 밖(호스트 -> EP, 또는 MEM_TO_MEM) 이면 rx 채널, 로컬은 dst.
 *   COPY 명령은 DMA_MEM_TO_MEM 을 넘기는데, 이 경우 rx 채널이 쓰이고
 *   dma_private 는 보통 false 이므로 memcpy 경로를 탄다.
 * 
 * ★ 동기 함수라는 점에 유의: 전송을 띄우고 완료를 '기다린' 뒤 돌아온다.
 * cmd_handler 워크큐 문맥에서 불리므로 잠들어도 된다.
 * 
 * 에러 경로: 준비 실패는 곧바로 반환하고, 제출 이후의 실패는 모두
 * terminate 라벨로 모여 채널을 정리한 뒤 반환한다.
 * 
 * 호출 체인:
 *   pci_epf_test_read()/write()/copy() -> [pci_epf_test_data_transfer]
 *     -> dmaengine_prep_* -> dmaengine_submit() -> wait_for_completion() */
static int pci_epf_test_data_transfer(struct pci_epf_test *epf_test,
				      dma_addr_t dma_dst, dma_addr_t dma_src,
				      size_t len, dma_addr_t dma_remote,
				      enum dma_transfer_direction dir)
{
	/* [한국어] 방향으로 채널을 고른다. EP -> 호스트면 tx, 아니면 rx.
	 * 전용 채널을 못 잡았다면 두 포인터가 같은 채널을 가리키므로
	 * 이 선택은 무의미해진다. */
	struct dma_chan *chan = (dir == DMA_MEM_TO_DEV) ?
				 epf_test->dma_chan_tx : epf_test->dma_chan_rx;
	/* [한국어] 로컬(EP 쪽) 주소. 슬레이브 전송에서 '내 쪽 끝' 이 어디인지를
	 * 가리키며, 방향에 따라 src 와 dst 중 하나가 그 역할을 한다. */
	dma_addr_t dma_local = (dir == DMA_MEM_TO_DEV) ? dma_src : dma_dst;
	/* [한국어] 전송 서술자 플래그. DMA_CTRL_ACK 는 '이 서술자를 다 쓰고 나면
	 * dmaengine 이 재사용해도 좋다' 는 표시이고, DMA_PREP_INTERRUPT 는
	 * 완료 시 인터럽트를 걸어 콜백을 부르라는 요청이다 - 이 플래그가
	 * 없으면 위 콜백이 불리지 않아 영원히 기다린다. */
	enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
	/* [한국어] 로그 출력용 EPF 객체. */
	struct pci_epf *epf = epf_test->epf;
	/* [한국어] dmaengine 이 돌려줄 전송 서술자. */
	struct dma_async_tx_descriptor *tx;
	/* [한국어] 슬레이브 설정. = {} 로 0 초기화해야 쓰지 않는 필드가
	 * 쓰레기 값이 되지 않는다. */
	struct dma_slave_config sconf = {};
	/* [한국어] dev_err 대상 장치. */
	struct device *dev = &epf->dev;
	/* [한국어] 반환값. */
	int ret;

	/* [한국어] 채널이 없거나 오류 포인터다. dma_supported 가 true 인데도 이럴
	 * 수 있는지는 분명치 않으나, 방어적으로 검사한다. */
	if (IS_ERR_OR_NULL(chan)) {
		/* [한국어] 원인을 남긴다. */
		dev_err(dev, "Invalid DMA memcpy channel\n");
		/* [한국어] 잘못된 인자로 처리한다. */
		return -EINVAL;
	}

	/* [한국어] 전용 채널을 잡은 경우 - 슬레이브 전송을 준비한다. */
	if (epf_test->dma_private) {
		/* [한국어] 전송 방향을 알려 준다. */
		sconf.direction = dir;
		/* [한국어] EP -> 호스트 방향이면 */
		if (dir == DMA_MEM_TO_DEV)
			/* [한국어] 상대(호스트) 주소가 목적지다. */
			sconf.dst_addr = dma_remote;
		/* [한국어] 반대 방향이면 */
		else
			/* [한국어] 상대 주소가 원본이다. */
			sconf.src_addr = dma_remote;

		/* [한국어] 채널에 상대 주소와 방향을 설정한다. 슬레이브 채널은 한쪽 끝이
		 * 고정되어 있어, 전송을 띄우기 전에 이렇게 미리 알려 주어야 한다. */
		if (dmaengine_slave_config(chan, &sconf)) {
			/* [한국어] 설정 실패를 남긴다. */
			dev_err(dev, "DMA slave config fail\n");
			/* [한국어] 입출력 오류로 보고한다. */
			return -EIO;
		}
		/* [한국어] 로컬 주소 하나와 길이로 슬레이브 전송을 준비한다. 상대 주소는
		 * 방금 설정으로 이미 알려 두었다. */
		tx = dmaengine_prep_slave_single(chan, dma_local, len, dir,
						 flags);
	/* [한국어] 범용 채널인 경우 - 두 물리 주소 사이의 memcpy 전송을 준비한다. */
	} else {
		/* [한국어] 출발지와 목적지를 모두 명시한다. */
		tx = dmaengine_prep_dma_memcpy(chan, dma_dst, dma_src, len,
					       flags);
	}

	/* [한국어] 준비 실패 - 채널이 그 크기나 주소를 다룰 수 없는 경우다. */
	if (!tx) {
		/* [한국어] 원인을 남긴다. */
		dev_err(dev, "Failed to prepare DMA memcpy\n");
		/* [한국어] 입출력 오류. */
		return -EIO;
	}

	/* [한국어] completion 을 '아직 완료되지 않음' 으로 되돌린다. 매 전송마다
	 * 필요하다 - 앞선 전송이 남긴 완료 상태를 그대로 두면 이번 대기가
	 * 곧바로 통과해 버린다. */
	reinit_completion(&epf_test->transfer_complete);
	/* [한국어] 콜백이 상태를 물을 때 쓸 채널을 기록한다. 반드시 제출 전에
	 * 채워야 한다 - 제출 직후 콜백이 불릴 수 있기 때문이다. */
	epf_test->transfer_chan = chan;
	/* [한국어] 완료 콜백을 건다. */
	tx->callback = pci_epf_test_dma_callback;
	/* [한국어] 콜백에 넘길 인자 - 이 EPF 상태 구조체다. */
	tx->callback_param = epf_test;
	/* [한국어] 전송을 큐에 넣고 쿠키를 받는다. 이 시점에는 아직 실제로 시작되지
	 * 않았다 - issue_pending 이 방아쇠다. */
	epf_test->transfer_cookie = dmaengine_submit(tx);

	/* [한국어] 쿠키가 오류를 담고 있는지 확인한다. 쿠키는 양수 식별자와 음수
	 * 오류를 같은 타입에 담으므로 전용 판정 함수가 필요하다. */
	ret = dma_submit_error(epf_test->transfer_cookie);
	/* [한국어] 제출 실패. */
	if (ret) {
		/* [한국어] 원인을 남긴다. */
		dev_err(dev, "Failed to do DMA tx_submit %d\n", ret);
		/* [한국어] 이미 큐에 무언가 들어갔을 수 있으므로 채널을 정리하고 나간다. */
		goto terminate;
	}

	/* [한국어] 큐에 쌓인 전송을 실제로 시작시킨다. dmaengine 이 제출과 시작을
	 * 나눠 둔 이유는 여러 전송을 모아 한 번에 띄울 수 있게 하기 위해서다. */
	dma_async_issue_pending(chan);
	/* [한국어] 완료 콜백이 complete() 를 부를 때까지 잠든다. _interruptible
	 * 판이라 시그널로도 깨어난다 - 워크큐 스레드는 보통 시그널을 받지
	 * 않지만, 영원히 매달리지 않도록 안전한 쪽을 골랐다. */
	ret = wait_for_completion_interruptible(&epf_test->transfer_complete);
	/* [한국어] 시그널로 중단됨. */
	if (ret < 0) {
		/* [한국어] 원인을 남긴다. */
		dev_err(dev, "DMA wait_for_completion interrupted\n");
		/* [한국어] ★ 반드시 채널을 정리해야 한다 - 우리가 기다리기를 포기했을 뿐
		 * 전송은 아직 진행 중일 수 있고, 그대로 두면 곧 해제될 버퍼에
		 * DMA 가 쓴다. */
		goto terminate;
	}

	/* [한국어] 전송은 끝났지만 오류로 끝난 경우. */
	if (epf_test->transfer_status == DMA_ERROR) {
		/* [한국어] 원인을 남긴다. */
		dev_err(dev, "DMA transfer failed\n");
		/* [한국어] 입출력 오류로 보고한다. 여기서는 goto 하지 않고 그대로 흘러
		 * 내려가 아래 정리를 거친다. */
		ret = -EIO;
	}

/* [한국어] 성공/실패가 함께 지나가는 정리 지점. */
terminate:
	/* [한국어] 채널에 남은 전송을 모두 끝내고 '진행 중인 것이 없음' 을 보장한다.
	 * _sync 판이라 실행 중인 콜백이 끝나기를 기다린다 - 이 함수가
	 * 돌아간 뒤 호출자가 버퍼를 해제해도 안전해지는 근거다.
	 * 정상 완료 경로에서도 부르는 이유는 채널 상태를 매번 같은
	 * 출발점으로 되돌려 두기 위해서다. */
	dmaengine_terminate_sync(chan);

	/* [한국어] 0(성공) 또는 음수 오류. */
	return ret;
}

/* [한국어] epf_dma_filter - dma_request_channel 에 넘길 채널 선별 조건.
 * 전용 DMA 채널을 찾을 때 '어느 장치의, 어느 방향을 지원하는' 채널인지를
 * 이 구조체에 담아 필터 함수로 전달한다. */
struct epf_dma_filter {
	/* [한국어] dev - 채널이 속해야 하는 장치.
	 * 설정자: pci_epf_test_init_dma_chan() 이 epf->epc->dev.parent 로 채운다 -
	 *        즉 EP 컨트롤러 하드웨어 자신이다.
	 * 읽는 자: epf_dma_filter_fn().
	 * 값 범위: 유효한 device 포인터.
	 * 동기화: 스택 변수라 다른 문맥과 공유되지 않는다. */
	struct device *dev;
	/* [한국어] dma_mask - 요구하는 전송 방향을 비트로 표시한 것.
	 * 설정자: init_dma_chan 이 BIT(DMA_DEV_TO_MEM) 또는 BIT(DMA_MEM_TO_DEV)로
	 *        두 번 나눠 설정한다.
	 * 읽는 자: epf_dma_filter_fn() 이 채널의 caps.directions 와 AND 한다.
	 * 값 범위: BIT(enum dma_transfer_direction 값)의 조합.
	 * 동기화: 스택 변수. */
	u32 dma_mask;
};

/* [한국어] epf_dma_filter_fn - 전용 DMA 채널을 고르는 선별 함수
 * 
 * @chan: dmaengine 이 후보로 제시한 채널.
 * @node: dma_request_channel 에 넘긴 epf_dma_filter 포인터.
 * @return: 이 채널을 쓰겠으면 true, 아니면 false.
 * 
 * ★ 왜 필터가 필요한가: 시스템에는 여러 DMA 컨트롤러가 있을 수 있는데,
 * EP 의 inbound/outbound 경로를 통해 호스트 메모리에 닿을 수 있는 것은
 * EP 컨트롤러에 붙은 채널뿐이다. 그래서 (1) 채널의 부모 장치가 EP
 * 컨트롤러인지, (2) 원하는 방향을 지원하는지 두 조건을 본다.
 * 
 * 실행 컨텍스트: dma_request_channel 안에서 동기적으로 불린다.
 * 프로세스 문맥이며, dmaengine 이 내부 락을 쥔 채 부를 수 있으므로
 * 가벼운 검사만 해야 한다.
 * 
 * 호출 체인:
 *   pci_epf_test_init_dma_chan() -> dma_request_channel() -> [epf_dma_filter_fn] */
static bool epf_dma_filter_fn(struct dma_chan *chan, void *node)
{
	/* [한국어] void 포인터로 온 조건을 원래 타입으로 되돌린다. */
	struct epf_dma_filter *filter = node;
	/* [한국어] 채널이 지원하는 능력을 담을 구조체. */
	struct dma_slave_caps caps;

	/* [한국어] dma_get_slave_caps 가 모든 필드를 채운다는 보장이 없으므로
	 * 먼저 0 으로 지워 둔다 - 그러지 않으면 스택 쓰레기를
	 * '지원하는 방향' 으로 오해할 수 있다. */
	memset(&caps, 0, sizeof(caps));
	/* [한국어] 채널의 슬레이브 능력을 조회한다. 실패해도 반환값을 보지 않는데,
	 * 그 경우 caps 가 0 인 채로 남아 아래 AND 가 거짓이 되므로
	 * 결과적으로 이 채널을 거르게 된다. */
	dma_get_slave_caps(chan, &caps);

	/* [한국어] 조건 1 - 채널의 부모 장치가 EP 컨트롤러인가. */
	return chan->device->dev == filter->dev
		/* [한국어] 조건 2 - 원하는 방향을 지원하는가. directions 는 방향마다 한
		 * 비트인 비트맵이라 AND 결과가 0 이 아니면 지원한다는 뜻이다. */
		&& (filter->dma_mask & caps.directions);
}

/**
 * pci_epf_test_init_dma_chan() - Function to initialize EPF test DMA channel
 * @epf_test: the EPF test device that performs data transfer operation
 *
 * Function to initialize EPF test DMA channel.
 */
/* [한국어] 위 kernel-doc 이 인자를 설명하고 있으므로, 여기서는
 * 구조와 의도를 덧붙인다.
 * 
 * ★ 두 단계 전략(이 함수의 전부):
 *   1단계 - '전용(슬레이브) 채널' 두 개를 EP 컨트롤러에서 찾는다.
 *      하나는 호스트 -> EP(DEV_TO_MEM, rx), 하나는 EP -> 호스트
 *      (MEM_TO_DEV, tx). 둘 다 잡히면 dma_private 를 세우고 끝.
 *   2단계 - 둘 중 하나라도 실패하면 전부 물러서서, 시스템 어디에나
 *      있는 범용 memcpy 채널 하나를 잡아 tx 와 rx 가 함께 쓴다.
 * 전용 채널이 더 나은 이유는 EP 컨트롤러 내장 DMA 라 PCI 버스 너머
 * 주소를 직접 다룰 수 있기 때문이다.
 * 
 * ★ goto 라벨의 흐름이 독특하다. fail_back_rx 는 이미 잡은 rx 를
 * 반납한 뒤 fail_back_tx 로 '흘러 내려간다'. 그래서 두 라벨이
 * 사실상 하나의 물러서기 경로를 이룬다. 그리고 이 경로는 오류
 * 경로가 아니라 '정상적인 대안' 이므로 끝에서 0 을 돌려준다.
 * 
 * ★ -EPROBE_DEFER 를 조용히 다루는 이유: DMA 컨트롤러 드라이버가
 * 아직 probe 되지 않았다는 뜻일 뿐 진짜 오류가 아니다. 커널이
 * 나중에 다시 시도하므로 오류 로그를 남기지 않는다. 다만 이 함수의
 * 호출자(epc_init)는 그 값을 받아도 다시 시도하지 않고 그냥
 * dma_supported 를 false 로 내린다.
 * 
 * 실행 컨텍스트: epc_init 의 프로세스 문맥. 채널 요청은 잠들 수 있다.
 * 
 * 호출 체인:
 *   pci_epf_test_epc_init() -> [pci_epf_test_init_dma_chan]
 *     -> dma_request_channel() / dma_request_chan_by_mask() */
static int pci_epf_test_init_dma_chan(struct pci_epf_test *epf_test)
{
	/* [한국어] EPF 객체. */
	struct pci_epf *epf = epf_test->epf;
	/* [한국어] 로그 대상. */
	struct device *dev = &epf->dev;
	/* [한국어] 채널 선별 조건 - 스택에 두고 필터 함수에 포인터로 넘긴다. */
	struct epf_dma_filter filter;
	/* [한국어] 요청 결과를 받을 채널 포인터. */
	struct dma_chan *dma_chan;
	/* [한국어] 요구하는 채널 능력 비트마스크. */
	dma_cap_mask_t mask;
	/* [한국어] 반환값. */
	int ret;

	/* [한국어] 채널이 속해야 할 장치는 EP 컨트롤러 하드웨어다. epf->epc->dev 는
	 * EPC 코어가 만든 가상 device 이므로, 실제 하드웨어인 그 부모를 쓴다. */
	filter.dev = epf->epc->dev.parent;
	/* [한국어] 먼저 호스트 -> EP(수신) 방향을 요구한다. */
	filter.dma_mask = BIT(DMA_DEV_TO_MEM);

	/* [한국어] 능력 마스크를 비운다. */
	dma_cap_zero(mask);
	/* [한국어] 슬레이브 전송 능력을 요구한다 - 한쪽 끝이 장치에 고정된 전송이다. */
	dma_cap_set(DMA_SLAVE, mask);
	/* [한국어] 조건에 맞는 채널을 요청한다. 후보마다 필터 함수가 불린다. */
	dma_chan = dma_request_channel(mask, epf_dma_filter_fn, &filter);
	/* [한국어] 전용 rx 채널이 없다. */
	if (!dma_chan) {
		/* [한국어] 정보 수준 로그 - 오류가 아니라 대안으로 물러선다는 뜻이므로
		 * dev_err 이 아니라 dev_info 다. */
		dev_info(dev, "Failed to get private DMA rx channel. Falling back to generic one\n");
		/* [한국어] 아직 아무것도 잡지 않았으므로 반납할 것 없이 곧바로 범용 경로로. */
		goto fail_back_tx;
	}

	/* [한국어] 전용 rx 채널 확보. */
	epf_test->dma_chan_rx = dma_chan;

	/* [한국어] 이번에는 EP -> 호스트(송신) 방향으로 조건을 바꾼다. 능력
	 * 마스크(mask)는 그대로 DMA_SLAVE 를 요구한다. */
	filter.dma_mask = BIT(DMA_MEM_TO_DEV);
	/* [한국어] 전용 tx 채널을 요청한다. */
	dma_chan = dma_request_channel(mask, epf_dma_filter_fn, &filter);

	/* [한국어] 전용 tx 채널이 없다. */
	if (!dma_chan) {
		/* [한국어] 역시 정보 수준 로그. */
		dev_info(dev, "Failed to get private DMA tx channel. Falling back to generic one\n");
		/* [한국어] 이미 잡은 rx 를 반납해야 하므로 그쪽 라벨로. */
		goto fail_back_rx;
	}

	/* [한국어] 전용 tx 채널 확보. */
	epf_test->dma_chan_tx = dma_chan;
	/* [한국어] ★ 두 전용 채널을 모두 잡았을 때만 세운다. 이 플래그가
	 * data_transfer 에서 슬레이브 전송과 memcpy 전송을 가른다. */
	epf_test->dma_private = true;

	/* [한국어] 완료 대기 객체를 초기화한다. 첫 전송 전에 반드시 필요하다. */
	init_completion(&epf_test->transfer_complete);

	/* [한국어] 전용 채널 경로 성공. */
	return 0;

/* [한국어] tx 를 못 잡은 경우 - rx 만 잡혀 있다. */
fail_back_rx:
	/* [한국어] 잡아 둔 rx 를 반납한다. 전용 두 개를 다 갖추지 못하면 하나만
	 * 쓰는 것보다 범용으로 통일하는 편이 낫기 때문이다. */
	dma_release_channel(epf_test->dma_chan_rx);
	/* [한국어] 포인터를 지운다 - 아래에서 범용 채널로 덮어쓰지만, 중간에
	 * 실패해 반환하는 경로가 있으므로 죽은 포인터를 남기면 안 된다. */
	epf_test->dma_chan_rx = NULL;

/* [한국어] 아무것도 못 잡았거나 위에서 흘러 내려온 지점 - 범용 채널을 찾는다. */
fail_back_tx:
	/* [한국어] 능력 마스크를 다시 비우고 */
	dma_cap_zero(mask);
	/* [한국어] 이번에는 단순 메모리 복사 능력을 요구한다. */
	dma_cap_set(DMA_MEMCPY, mask);

	/* [한국어] 필터 없이 능력만으로 아무 채널이나 하나 요청한다 - 어느
	 * 컨트롤러의 채널이든 상관없다는 뜻이다. */
	dma_chan = dma_request_chan_by_mask(&mask);
	/* [한국어] 이쪽 API 는 NULL 이 아니라 오류 포인터를 돌려준다. */
	if (IS_ERR(dma_chan)) {
		/* [한국어] 오류 코드를 꺼낸다. */
		ret = PTR_ERR(dma_chan);
		/* [한국어] 아직 DMA 드라이버가 준비되지 않았다는 뜻이면 */
		if (ret != -EPROBE_DEFER)
			/* [한국어] 로그를 남기지 않는다 - 곧 다시 시도될 정상적인 상황이라
			 * 로그가 시끄러워질 뿐이다. 그 밖의 오류만 기록한다. */
			dev_err(dev, "Failed to get DMA channel\n");
		/* [한국어] DMA 를 전혀 쓸 수 없다. 호출자가 dma_supported 를 내린다. */
		return ret;
	}
	/* [한국어] 완료 대기 객체 초기화. */
	init_completion(&epf_test->transfer_complete);

	/* [한국어] ★ tx 와 rx 가 같은 채널을 가리킨다. 그래서 clean_dma_chan 이
	 * '두 포인터가 같으면 한 번만 반납' 하는 특별 처리를 갖는다.
	 * dma_private 는 false 로 남으므로 data_transfer 는 memcpy 경로를 탄다. */
	epf_test->dma_chan_tx = epf_test->dma_chan_rx = dma_chan;

	/* [한국어] 범용 채널 경로 성공. */
	return 0;
}

/**
 * pci_epf_test_clean_dma_chan() - Function to cleanup EPF test DMA channel
 * @epf_test: the EPF test device that performs data transfer operation
 *
 * Helper to cleanup EPF test DMA channel.
 */
/* [한국어] 위 kernel-doc 이 인자를 설명한다. 여기서는 정리 규칙을 덧붙인다.
 * 
 * ★ 이 함수가 조심해야 하는 것은 '이중 반납' 이다. 범용 채널로
 * 물러선 경우 dma_chan_tx 와 dma_chan_rx 가 같은 포인터인데, 그것을
 * 두 번 반납하면 참조 카운트가 무너진다. 그래서 tx 를 반납한 뒤
 * 두 포인터가 같았는지 확인해, 같으면 둘 다 지우고 곧바로 돌아간다.
 * 
 * dma_supported 를 앞에서 확인하는 이유: DMA 초기화 자체가 실패한
 * 경우 두 포인터 모두 NULL 이라 아래 검사로도 안전하지만, 의도를
 * 드러내려고 명시적으로 걸러 낸다.
 * 
 * ★ 이 함수는 두 번 불릴 수 있다(epc_deinit 과 unbind). 포인터를
 * NULL 로 지우는 덕분에 두 번째 호출이 아무 일도 하지 않는다.
 * 
 * 실행 컨텍스트: 프로세스 문맥. 채널 반납은 잠들 수 있다.
 * 
 * 호출 체인:
 *   pci_epf_test_epc_deinit() / pci_epf_test_unbind()
 *     -> [pci_epf_test_clean_dma_chan] -> dma_release_channel() */
static void pci_epf_test_clean_dma_chan(struct pci_epf_test *epf_test)
{
	/* [한국어] DMA 를 아예 쓰지 못하는 구성이면 */
	if (!epf_test->dma_supported)
		/* [한국어] 반납할 채널도 없다. */
		return;

	/* [한국어] tx 채널이 있으면 */
	if (epf_test->dma_chan_tx) {
		/* [한국어] 반납한다. */
		dma_release_channel(epf_test->dma_chan_tx);
		/* [한국어] ★ 범용 채널로 물러선 경우 rx 가 같은 채널을 가리킨다. */
		if (epf_test->dma_chan_tx == epf_test->dma_chan_rx) {
			/* [한국어] tx 포인터를 지우고 */
			epf_test->dma_chan_tx = NULL;
			/* [한국어] rx 포인터도 지운다 - 이미 반납했으므로 다시 반납하면 안 된다. */
			epf_test->dma_chan_rx = NULL;
			/* [한국어] 곧바로 돌아가 아래 rx 반납을 건너뛴다. */
			return;
		}
		/* [한국어] 서로 다른 채널이었다면 tx 만 지우고 아래로 내려간다. */
		epf_test->dma_chan_tx = NULL;
	}

	/* [한국어] rx 채널이 남아 있으면(전용 채널 두 개를 잡았던 경우) */
	if (epf_test->dma_chan_rx) {
		/* [한국어] 따로 반납한다. */
		dma_release_channel(epf_test->dma_chan_rx);
		/* [한국어] 포인터를 지운다. */
		epf_test->dma_chan_rx = NULL;
	}
}

/* [한국어] pci_epf_test_print_rate - 전송 처리율을 커널 로그에 남긴다
 * 
 * @epf_test: 로그 대상 EPF.
 * @op: 어떤 명령이었는지("READ"/"WRITE"/"COPY").
 * @size: 전송한 총 바이트 수.
 * @start: 전송 시작 시각(ktime_get_ts64 로 찍은 것).
 * @end: 전송 종료 시각.
 * @dma: DMA 경로를 썼는지 - 두 경로의 성능을 비교할 수 있게 함께 찍는다.
 * @return: 없음
 * 
 * 시험 드라이버의 존재 이유 중 하나가 성능 측정이므로, 결과를
 * 사람이 바로 읽을 수 있는 형태로 남긴다.
 * 
 * ★ 나눗셈에 주의할 점 두 가지:
 *   - ns 가 0 이면(시계 해상도보다 짧은 전송) 0 으로 나누게 되므로
 *     미리 걸러 rate 를 0 으로 둔다.
 *   - 64 비트 나눗셈을 그냥 '/' 로 쓰면 32 비트 아키텍처에서 링크
 *     오류가 난다. 그래서 div64_u64 헬퍼를 쓴다.
 *   - ns * 1000 으로 나누는 것은 결과 단위를 KB/s 로 만들기 위해서다
 *     (바이트/초를 1000 으로 나눈다).
 * 
 * ★ 시각 측정 범위의 한계: read/write/copy 는 while 루프로 조각을
 * 나눠 전송하는데, start 와 end 는 루프 안에서 매번 덮어써진다.
 * 그래서 여기 전달되는 것은 '마지막 조각' 의 시각이고 size 는
 * '전체' 크기다. 즉 여러 조각으로 나뉜 큰 전송의 처리율은
 * 실제보다 크게 나온다 - 이 트리의 코드가 그렇게 되어 있다는
 * 사실만 적어 둔다.
 * 
 * 실행 컨텍스트: cmd_handler 의 워크큐 문맥.
 * 
 * 호출 체인:
 *   pci_epf_test_read()/write()/copy() -> [pci_epf_test_print_rate] */
static void pci_epf_test_print_rate(struct pci_epf_test *epf_test,
				    const char *op, u64 size,
				    struct timespec64 *start,
				    struct timespec64 *end, bool dma)
{
	/* [한국어] 걸린 시간을 구한다. timespec64 끼리의 뺄셈은 초와 나노초를
	 * 각각 빼고 자리내림을 처리해야 하므로 전용 헬퍼를 쓴다. */
	struct timespec64 ts = timespec64_sub(*end, *start);
	/* [한국어] 처리율과 나노초. rate 를 0 으로 초기화해 두어, 아래 0 나눗셈
	 * 방지 분기에 걸렸을 때 쓰레기 값이 찍히지 않게 한다. */
	u64 rate = 0, ns;

	/* calculate the rate */
	/* [한국어] 걸린 시간을 나노초 단위 정수로 바꾼다. */
	ns = timespec64_to_ns(&ts);
	/* [한국어] 0 이면 나눌 수 없다 - 시계 해상도보다 빠른 전송이었다는 뜻이다. */
	if (ns)
		/* [한국어] 바이트/초를 구한 뒤 1000 으로 나눠 KB/s 로 만든다.
		 * div64_u64 는 32 비트 아키텍처에서도 64 비트 나눗셈이 되게 하는 헬퍼다. */
		rate = div64_u64(size * NSEC_PER_SEC, ns * 1000);

	/* [한국어] 결과를 정보 수준 로그로 남긴다. %ptSp 는 timespec64 포인터를
	 * 사람이 읽을 수 있는 시간 문자열로 찍는 커널 printf 확장이다. */
	dev_info(&epf_test->epf->dev,
		 "%s => Size: %llu B, DMA: %s, Time: %ptSp s, Rate: %llu KB/s\n",
		 op, size, dma ? "YES" : "NO", &ts, rate);
}

/* [한국어] pci_epf_test_copy - 호스트 메모리 안에서 src_addr -> dst_addr 로 복사한다
 * 
 * @epf_test: 이 EPF 인스턴스.
 * @reg: 호스트와 공유하는 레지스터. src_addr/dst_addr/size/flags 를 읽고
 *       status 를 쓴다.
 * @return: 없음. 결과는 reg->status 로 호스트에 전달된다.
 * 
 * ★ 이 명령이 시험하는 것: 호스트 메모리 두 곳을 EP 가 동시에 매핑해
 * 한쪽에서 읽어 다른 쪽에 쓴다. 즉 outbound 경로를 읽기와 쓰기 양쪽으로
 * 동시에 쓰는 가장 까다로운 시나리오다. 창(window)을 두 개 잡아야 하므로
 * 컨트롤러의 매핑 자원이 부족하면 여기서 드러난다.
 * 
 * ★ while 루프가 필요한 이유(이 파일 세 전송 함수의 공통 구조):
 * pci_epc_mem_map() 은 요청한 크기를 다 매핑해 주지 못할 수 있다.
 * 컨트롤러의 outbound 창 크기와 정렬 제약 때문이며, 실제로 매핑된
 * 크기는 map.pci_size 로 돌아온다. 그래서 '요청 -> 매핑된 만큼 처리 ->
 * 주소를 그만큼 전진 -> 다시 요청' 을 남은 크기가 0 이 될 때까지 반복한다.
 * 여기서는 창이 둘이므로 둘 중 작은 쪽을 이번 조각 크기로 삼는다.
 * 
 * ★ 두 경로의 차이:
 *   - DMA : 매핑된 물리 주소끼리 직접 옮긴다. 중간 버퍼가 필요 없다.
 *   - CPU : 원본 창에서 커널 버퍼로 읽고, 그 버퍼에서 목적지 창으로 쓴다.
 *     버퍼를 전체 크기로 한 번에 잡고 조각마다 앞으로 밀며 쓴다.
 * 
 * 에러 경로가 세 라벨로 나뉜 이유: 되돌릴 것이 단계마다 다르기 때문이다.
 * unmap 은 창 두 개, free_buf 는 버퍼, set_status 는 상태 보고다.
 * 아래로 흘러 내려가며 필요한 만큼만 정리된다.
 * 
 * 실행 컨텍스트: cmd_handler 의 워크큐 문맥. 잠들 수 있다.
 * 
 * 호출 체인:
 *   pci_epf_test_cmd_handler() -> [pci_epf_test_copy]
 *     -> pci_epc_mem_map() -> pci_epf_test_data_transfer() 또는 memcpy_*io() */
static void pci_epf_test_copy(struct pci_epf_test *epf_test,
			      struct pci_epf_test_reg *reg)
{
	/* [한국어] 반환값 겸 오류 표시. 0 으로 시작해, 끝에서 성공/실패 상태를 가른다. */
	int ret = 0;
	/* [한국어] 처리율 계산용 시각. 루프 안에서 매 조각마다 덮어써진다. */
	struct timespec64 start, end;
	/* [한국어] EPF 객체. */
	struct pci_epf *epf = epf_test->epf;
	/* [한국어] EP 컨트롤러 - 매핑과 인터럽트가 모두 이것을 거친다. */
	struct pci_epc *epc = epf->epc;
	/* [한국어] 로그 대상. */
	struct device *dev = &epf->dev;
	/* [한국어] 원본과 목적지 각각의 매핑 결과. 물리 주소, 가상 주소, 실제로
	 * 매핑된 크기가 여기 담겨 돌아온다. */
	struct pci_epc_map src_map, dst_map;
	/* [한국어] 호스트가 준 원본 주소. 리틀엔디안 필드이므로 변환해 읽는다. */
	u64 src_addr = le64_to_cpu(reg->src_addr);
	/* [한국어] 목적지 주소. */
	u64 dst_addr = le64_to_cpu(reg->dst_addr);
	/* [한국어] 전체 크기와 '아직 남은' 크기. 전자는 처리율 계산에, 후자는
	 * 루프 조건에 쓴다. */
	size_t orig_size, copy_size;
	/* [한국어] 이번 조각에서 실제로 매핑된 크기. ★ 0 으로 초기화하는 것이
	 * 중요하다 - 아래 unmap 라벨이 이 값으로 '창이 잡혀 있는가' 를
	 * 판단하기 때문에, 매핑 전에 실패하면 0 이어야 한다. */
	ssize_t map_size = 0;
	/* [한국어] 전송 옵션(DMA 사용 여부). */
	u32 flags = le32_to_cpu(reg->flags);
	/* [한국어] 호스트에 돌려줄 상태 비트. 0 으로 시작해 필요할 때 OR 한다. */
	u32 status = 0;
	/* [한국어] CPU 경로의 중간 버퍼와, 그 안에서 전진하는 커서.
	 * copy_buf 를 NULL 로 초기화하는 이유는 DMA 경로에서 할당하지 않아도
	 * 끝에서 kfree(NULL) 이 안전하게 아무 일도 하지 않게 하기 위해서다. */
	void *copy_buf = NULL, *buf;

	/* [한국어] 호스트가 요청한 크기를 두 변수에 함께 넣는다. */
	orig_size = copy_size = le32_to_cpu(reg->size);

	/* [한국어] DMA 를 쓰라는 요청인 경우. */
	if (flags & FLAG_USE_DMA) {
		/* [한국어] ★ 채널이 실제로 메모리-메모리 복사를 지원하는지 확인한다.
		 * COPY 는 양쪽이 모두 호스트 메모리라 슬레이브 전송이 아니라
		 * memcpy 능력이 필요하기 때문이다. 전용 슬레이브 채널만 잡힌
		 * 구성이라면 여기서 걸린다. */
		if (!dma_has_cap(DMA_MEMCPY, epf_test->dma_chan_tx->device->cap_mask)) {
			/* [한국어] 지원하지 않음을 남긴다. */
			dev_err(dev, "DMA controller doesn't support MEMCPY\n");
			/* [한국어] 잘못된 요청으로 처리한다. */
			ret = -EINVAL;
			/* [한국어] 아직 아무것도 잡지 않았으므로 상태 보고로 곧장 간다. */
			goto set_status;
		}
	/* [한국어] CPU 복사 경로. */
	} else {
		/* [한국어] 전체 크기의 중간 버퍼를 잡는다. kzalloc 이라 0 으로 채워지는데,
		 * 어차피 곧 덮어쓰므로 그 자체가 목적은 아니다. */
		copy_buf = kzalloc(copy_size, GFP_KERNEL);
		/* [한국어] 메모리 부족. */
		if (!copy_buf) {
			ret = -ENOMEM;
			/* [한국어] 버퍼가 없으므로 정리 없이 상태 보고로. */
			goto set_status;
		}
		/* [한국어] 커서를 버퍼 앞에 둔다 - 조각마다 앞으로 민다. */
		buf = copy_buf;
	}

	/* [한국어] 남은 크기가 0 이 될 때까지 조각내어 처리한다. */
	while (copy_size) {
		/* [한국어] 원본 주소를 EP 의 물리 주소 공간으로 매핑한다. func_no/vfunc_no 는
		 * 어느 함수(PF/VF)의 창을 쓸지 가리킨다. */
		ret = pci_epc_mem_map(epc, epf->func_no, epf->vfunc_no,
				      src_addr, copy_size, &src_map);
		/* [한국어] 매핑 실패 - 보통 호스트가 준 주소가 창으로 덮을 수 없는 값이다. */
		if (ret) {
			/* [한국어] 원인을 남긴다. */
			dev_err(dev, "Failed to map source address\n");
			/* [한국어] 호스트에게 '원본 주소가 잘못됐다' 고 구체적으로 알린다. */
			status = STATUS_SRC_ADDR_INVALID;
			/* [한국어] 창을 하나도 잡지 못했으므로 버퍼만 반납하면 된다. */
			goto free_buf;
		}

		/* [한국어] 목적지 주소도 매핑한다. epc 대신 epf->epc 를 쓰지만 같은 값이다. */
		ret = pci_epc_mem_map(epf->epc, epf->func_no, epf->vfunc_no,
					   dst_addr, copy_size, &dst_map);
		/* [한국어] 목적지 매핑 실패. */
		if (ret) {
			/* [한국어] 원인을 남긴다. */
			dev_err(dev, "Failed to map destination address\n");
			/* [한국어] '목적지 주소가 잘못됐다' 고 알린다. */
			status = STATUS_DST_ADDR_INVALID;
			/* [한국어] ★ 이미 잡은 원본 창을 반드시 풀어야 한다. 아래 free_buf 로
			 * 가면 unmap 라벨을 건너뛰므로 여기서 직접 푼다. */
			pci_epc_mem_unmap(epc, epf->func_no, epf->vfunc_no,
					  &src_map);
			/* [한국어] 버퍼 반납으로. */
			goto free_buf;
		}

		/* [한국어] 두 창 중 작은 쪽이 이번에 처리할 수 있는 크기다. 어느 한쪽이
		 * 요청보다 작게 매핑되었다면 그만큼만 다뤄야 한다. */
		map_size = min_t(size_t, dst_map.pci_size, src_map.pci_size);

		/* [한국어] 이번 조각의 시작 시각. */
		ktime_get_ts64(&start);
		/* [한국어] DMA 경로. */
		if (flags & FLAG_USE_DMA) {
			/* [한국어] 두 물리 주소 사이를 직접 옮긴다. dma_remote 인자에 0 을 넘기는
			 * 이유는 MEM_TO_MEM 이라 슬레이브 상대 주소 개념이 없기 때문이다. */
			ret = pci_epf_test_data_transfer(epf_test,
					dst_map.phys_addr, src_map.phys_addr,
					map_size, 0, DMA_MEM_TO_MEM);
			/* [한국어] 전송 실패. */
			if (ret) {
				/* [한국어] 원인을 남긴다. */
				dev_err(dev, "Data transfer failed\n");
				/* [한국어] ★ 창 두 개가 잡혀 있으므로 unmap 라벨로 가야 한다. */
				goto unmap;
			}
		/* [한국어] CPU 경로. */
		} else {
			/* [한국어] 원본 창(I/O 메모리)에서 커널 버퍼로 읽는다. */
			memcpy_fromio(buf, src_map.virt_addr, map_size);
			/* [한국어] 커널 버퍼에서 목적지 창(I/O 메모리)으로 쓴다. */
			memcpy_toio(dst_map.virt_addr, buf, map_size);
			/* [한국어] 버퍼 커서를 이번 조각만큼 전진시킨다 - 다음 조각이 이어 붙는다. */
			buf += map_size;
		}
		/* [한국어] 이번 조각의 종료 시각. */
		ktime_get_ts64(&end);

		/* [한국어] 남은 크기를 줄인다. */
		copy_size -= map_size;
		/* [한국어] 원본 주소를 전진시킨다. */
		src_addr += map_size;
		/* [한국어] 목적지 주소도 전진시킨다. */
		dst_addr += map_size;

		/* [한국어] 목적지 창을 푼다. */
		pci_epc_mem_unmap(epc, epf->func_no, epf->vfunc_no, &dst_map);
		/* [한국어] 원본 창을 푼다. 다음 반복이 새 창을 잡으므로 매 조각마다
		 * 반드시 풀어야 창 자원이 고갈되지 않는다. */
		pci_epc_mem_unmap(epc, epf->func_no, epf->vfunc_no, &src_map);
		/* [한국어] ★ 창을 다 풀었음을 표시한다. 이 값이 0 이어야 아래 unmap
		 * 라벨이 이미 푼 창을 다시 풀지 않는다. */
		map_size = 0;
	}

	/* [한국어] 전체 크기와 마지막 조각의 시각으로 처리율을 남긴다. */
	pci_epf_test_print_rate(epf_test, "COPY", orig_size, &start, &end,
				flags & FLAG_USE_DMA);

/* [한국어] 전송 중 실패해 창이 아직 잡혀 있는 경우가 지나가는 지점. */
unmap:
	/* [한국어] map_size 가 0 이 아니면 창 두 개가 아직 잡혀 있다는 뜻이다.
	 * 정상 종료 경로는 위에서 0 으로 만들었으므로 여기를 건너뛴다. */
	if (map_size) {
		/* [한국어] 목적지 창을 푼다. */
		pci_epc_mem_unmap(epc, epf->func_no, epf->vfunc_no, &dst_map);
		/* [한국어] 원본 창을 푼다. */
		pci_epc_mem_unmap(epc, epf->func_no, epf->vfunc_no, &src_map);
	}

/* [한국어] 버퍼 반납 지점. */
free_buf:
	/* [한국어] DMA 경로였다면 NULL 이라 아무 일도 하지 않는다. */
	kfree(copy_buf);

/* [한국어] 상태 보고 지점 - 모든 경로가 여기로 모인다. */
set_status:
	/* [한국어] 오류 없이 끝났으면 */
	if (!ret)
		/* [한국어] 성공 비트를 세운다. */
		status |= STATUS_COPY_SUCCESS;
	/* [한국어] 오류가 있었으면 */
	else
		/* [한국어] 실패 비트를 세운다. 매핑 실패였다면 SRC/DST_ADDR_INVALID 도
		 * 함께 서 있어 호스트가 사유를 구분할 수 있다. */
		status |= STATUS_COPY_FAIL;
	/* [한국어] ★ 상태를 레지스터에 반영한다. 이 쓰기가 지금까지 status 변수에
	 * 모아 둔 비트를 한 번에 내보내는 지점이며, 곧이어 cmd_handler 가
	 * raise_irq 를 불러 호스트를 깨운다. 리틀엔디안으로 변환해야 호스트가
	 * 올바로 읽는다. */
	reg->status = cpu_to_le32(status);
}

/* [한국어] pci_epf_test_read - 호스트 메모리에서 읽어 CRC 로 무결성을 검증한다
 * 
 * @epf_test: 이 EPF 인스턴스.
 * @reg: 공유 레지스터. src_addr/size/flags/checksum 을 읽고 status 를 쓴다.
 * @return: 없음. 결과는 reg->status 로 전달된다.
 * 
 * ★ 이 명령이 시험하는 것: EP 가 호스트 메모리를 '읽는' 경로다.
 * 호스트가 미리 버퍼를 난수로 채우고 그 CRC 를 checksum 레지스터에
 * 적어 둔 뒤 READ 를 요청하면, EP 가 같은 데이터를 읽어 CRC 를 계산해
 * 비교한다. 값이 다르면 전송 경로 어딘가가 데이터를 망가뜨린 것이다.
 * 그래서 이 명령은 '동작했는가' 뿐 아니라 '올바로 동작했는가' 까지 본다.
 * 
 * ★ COPY 와 다른 점: 창이 하나뿐이고(호스트 쪽만 매핑) 목적지는 EP 의
 * 커널 버퍼다. 그래서 DMA 경로에서 목적지를 dma_map_single 로 따로
 * DMA 주소로 바꿔야 한다 - 커널 가상 주소를 DMA 엔진이 그대로 쓸 수는
 * 없기 때문이다. DMA_FROM_DEVICE 방향은 '장치가 쓰고 CPU 가 읽는다' 는
 * 뜻으로, 커널이 알맞은 캐시 무효화를 해 준다.
 * 
 * ★ DMA 경로의 오류 처리 순서가 미묘하다: 전송이 실패해도 곧바로
 * 빠져나가지 않고, 먼저 dma_unmap_single 로 매핑을 되돌린 뒤에
 * ret 를 다시 검사해 나간다. 매핑을 남긴 채 나가면 DMA 매핑이 샌다.
 * 
 * 전송에 슬레이브 방향 DMA_DEV_TO_MEM 을 쓰고 dma_remote 로 src_addr
 * (호스트 주소)을 넘기는 점에 유의 - 전용 채널 구성에서 그 값이 쓰인다.
 * 
 * 실행 컨텍스트: cmd_handler 의 워크큐 문맥.
 * 
 * 호출 체인:
 *   pci_epf_test_cmd_handler() -> [pci_epf_test_read]
 *     -> pci_epc_mem_map() -> pci_epf_test_data_transfer() 또는 memcpy_fromio()
 *     -> crc32_le() */
static void pci_epf_test_read(struct pci_epf_test *epf_test,
			      struct pci_epf_test_reg *reg)
{
	/* [한국어] 오류 표시. */
	int ret = 0;
	/* [한국어] 읽어 들일 커널 버퍼와 그 안에서 전진하는 커서. */
	void *src_buf, *buf;
	/* [한국어] EP 가 계산한 CRC. */
	u32 crc32;
	/* [한국어] 호스트 메모리 매핑 결과 - 창이 하나뿐이라 COPY 와 달리 단수다. */
	struct pci_epc_map map;
	/* [한국어] DMA 경로에서 목적지 버퍼의 DMA 주소. */
	phys_addr_t dst_phys_addr;
	/* [한국어] 처리율 계산용 시각. */
	struct timespec64 start, end;
	/* [한국어] EPF 객체. */
	struct pci_epf *epf = epf_test->epf;
	/* [한국어] EP 컨트롤러. */
	struct pci_epc *epc = epf->epc;
	/* [한국어] 로그 대상. */
	struct device *dev = &epf->dev;
	/* [한국어] ★ DMA 매핑의 기준 장치는 EPF 가상 device 가 아니라 실제
	 * 하드웨어인 EP 컨트롤러다. DMA 주소는 그 하드웨어가 보는 주소이므로
	 * 반드시 그 장치로 매핑해야 한다. */
	struct device *dma_dev = epf->epc->dev.parent;
	/* [한국어] 호스트가 준 원본 주소. */
	u64 src_addr = le64_to_cpu(reg->src_addr);
	/* [한국어] 전체 크기와 남은 크기. */
	size_t orig_size, src_size;
	/* [한국어] 이번 조각의 매핑 크기. 0 초기화의 의미는 COPY 와 같다. */
	ssize_t map_size = 0;
	/* [한국어] 전송 옵션. */
	u32 flags = le32_to_cpu(reg->flags);
	/* [한국어] 호스트가 미리 계산해 둔 기대 CRC 값. */
	u32 checksum = le32_to_cpu(reg->checksum);
	/* [한국어] 호스트에 돌려줄 상태. */
	u32 status = 0;

	/* [한국어] 요청 크기를 두 변수에 넣는다. */
	orig_size = src_size = le32_to_cpu(reg->size);

	/* [한국어] 읽어 들일 버퍼를 전체 크기로 잡는다. CRC 를 전체에 대해 한 번에
	 * 계산해야 하므로 조각별 버퍼로는 안 되고 통짜 버퍼가 필요하다. */
	src_buf = kzalloc(src_size, GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!src_buf) {
		ret = -ENOMEM;
		/* [한국어] 정리할 것이 없으므로 상태 보고로. */
		goto set_status;
	}
	/* [한국어] 커서를 버퍼 앞에 둔다. */
	buf = src_buf;

	/* [한국어] 남은 크기가 0 이 될 때까지 조각내어 읽는다. */
	while (src_size) {
		/* [한국어] 호스트 원본 주소를 EP 물리 주소 공간으로 매핑한다. */
		ret = pci_epc_mem_map(epc, epf->func_no, epf->vfunc_no,
					   src_addr, src_size, &map);
		/* [한국어] 매핑 실패. */
		if (ret) {
			/* [한국어] 원인을 남긴다. */
			dev_err(dev, "Failed to map address\n");
			/* [한국어] '원본 주소가 잘못됐다' 고 알린다. */
			status = STATUS_SRC_ADDR_INVALID;
			/* [한국어] 창을 잡지 못했으므로 버퍼만 반납한다. */
			goto free_buf;
		}

		/* [한국어] 이번에 실제로 매핑된 크기 - 요청보다 작을 수 있다. */
		map_size = map.pci_size;
		/* [한국어] DMA 경로. */
		if (flags & FLAG_USE_DMA) {
			/* [한국어] 커널 버퍼를 DMA 엔진이 쓸 수 있는 주소로 매핑한다.
			 * DMA_FROM_DEVICE 는 '장치 -> 메모리' 방향이라는 뜻으로, 커널이
			 * 전송 후 캐시를 무효화해 CPU 가 새 데이터를 보게 한다. */
			dst_phys_addr = dma_map_single(dma_dev, buf, map_size,
						       DMA_FROM_DEVICE);
			/* [한국어] DMA 매핑 실패 - IOMMU 자원 부족이나 주소 범위 초과다.
			 * 평범한 NULL 검사로는 안 되고 전용 판정 함수를 써야 한다. */
			if (dma_mapping_error(dma_dev, dst_phys_addr)) {
				/* [한국어] 원인을 남긴다. */
				dev_err(dev,
					"Failed to map destination buffer addr\n");
				/* [한국어] 메모리 부족으로 처리한다. */
				ret = -ENOMEM;
				/* [한국어] ★ 창이 잡혀 있으므로 unmap 라벨로 간다. */
				goto unmap;
			}

			/* [한국어] 전송 시작 시각. */
			ktime_get_ts64(&start);
			/* [한국어] 호스트 창에서 커널 버퍼로 옮긴다. 슬레이브 채널 구성에서는
			 * dma_remote(=src_addr, 호스트 주소)가 상대 끝 주소로 쓰인다. */
			ret = pci_epf_test_data_transfer(epf_test,
					dst_phys_addr, map.phys_addr,
					map_size, src_addr, DMA_DEV_TO_MEM);
			/* [한국어] 전송 실패. */
			if (ret)
				/* [한국어] 원인을 남긴다. ★ 여기서 곧바로 나가지 않는 것이 핵심이다. */
				dev_err(dev, "Data transfer failed\n");
			/* [한국어] 실패했더라도 시각은 찍는다 - 아래 매핑 해제까지 한 흐름으로
			 * 지나가기 위한 배치다. */
			ktime_get_ts64(&end);

			/* [한국어] ★ 성공이든 실패든 DMA 매핑을 반드시 되돌린다.
			 * DMA_FROM_DEVICE 방향이므로 이 호출이 캐시 무효화를 수행해,
			 * 장치가 쓴 내용을 CPU 가 볼 수 있게 만든다. */
			dma_unmap_single(dma_dev, dst_phys_addr, map_size,
					 DMA_FROM_DEVICE);

			/* [한국어] 이제서야 실패를 처리한다 - 매핑을 이미 되돌렸으므로 안전하다. */
			if (ret)
				/* [한국어] 창을 풀러 간다. */
				goto unmap;
		/* [한국어] CPU 경로. */
		} else {
			/* [한국어] 시작 시각. */
			ktime_get_ts64(&start);
			/* [한국어] 호스트 창(I/O 메모리)에서 커널 버퍼로 읽는다. */
			memcpy_fromio(buf, map.virt_addr, map_size);
			/* [한국어] 종료 시각. */
			ktime_get_ts64(&end);
		}

		/* [한국어] 남은 크기를 줄인다. */
		src_size -= map_size;
		/* [한국어] 원본 주소를 전진시킨다. */
		src_addr += map_size;
		/* [한국어] 버퍼 커서도 전진시킨다 - 조각들이 버퍼에 이어 붙는다. */
		buf += map_size;

		/* [한국어] 창을 푼다. */
		pci_epc_mem_unmap(epc, epf->func_no, epf->vfunc_no, &map);
		/* [한국어] 창을 다 풀었음을 표시 - 아래 unmap 라벨이 중복으로 풀지 않게 한다. */
		map_size = 0;
	}

	/* [한국어] 처리율을 남긴다. */
	pci_epf_test_print_rate(epf_test, "READ", orig_size, &start, &end,
				flags & FLAG_USE_DMA);

	/* [한국어] ★ 무결성 검증. 시드 ~0(전 비트 1)은 CRC32 의 표준 초기값이며,
	 * 호스트도 같은 시드로 계산해야 값이 맞는다. 조각 단위가 아니라
	 * 전체 버퍼에 대해 한 번에 계산한다. */
	crc32 = crc32_le(~0, src_buf, orig_size);
	/* [한국어] 호스트가 알려 준 기대값과 다르면 */
	if (crc32 != checksum)
		/* [한국어] 데이터가 전송 중 망가진 것이므로 입출력 오류로 처리한다.
		 * 이 검사가 이 시험 드라이버의 존재 이유 중 하나다. */
		ret = -EIO;

/* [한국어] 창이 아직 잡혀 있는 실패 경로가 지나가는 지점. */
unmap:
	/* [한국어] 정상 종료 경로는 위에서 0 으로 만들었으므로 건너뛴다. */
	if (map_size)
		/* [한국어] 남아 있던 창을 푼다. */
		pci_epc_mem_unmap(epc, epf->func_no, epf->vfunc_no, &map);

/* [한국어] 버퍼 반납 지점. */
free_buf:
	/* [한국어] 읽기 버퍼를 반납한다. */
	kfree(src_buf);

/* [한국어] 상태 보고 지점. */
set_status:
	/* [한국어] 매핑도 전송도 CRC 도 모두 통과했으면 */
	if (!ret)
		/* [한국어] 성공 비트. */
		status |= STATUS_READ_SUCCESS;
	/* [한국어] 아니면 */
	else
		/* [한국어] 실패 비트. CRC 불일치도 여기로 온다 - 호스트는 별도 비트가
		 * 없으므로 '주소 무효' 비트가 없는 실패로 미루어 짐작한다. */
		status |= STATUS_READ_FAIL;
	/* [한국어] 상태를 레지스터에 반영한다. */
	reg->status = cpu_to_le32(status);
}

/* [한국어] pci_epf_test_write - 난수 데이터를 만들어 호스트 메모리에 쓴다
 * 
 * @epf_test: 이 EPF 인스턴스.
 * @reg: 공유 레지스터. dst_addr/size/flags 를 읽고, checksum 과 status 를 쓴다.
 * @return: 없음. 결과는 reg->status 로 전달된다.
 * 
 * ★ READ 와 대칭이지만 CRC 를 쓰는 쪽이 반대다. 여기서는 EP 가 데이터를
 * 만들었으므로 EP 가 CRC 를 계산해 checksum 레지스터에 남기고, 검증은
 * 호스트가 한다. 그래서 이 함수 안에는 비교 코드가 없다.
 * 
 * ★ 난수를 쓰는 이유: 0 이나 반복 패턴이면 '실제로 전송이 일어났는지'
 * 와 '원래 그 값이었는지' 를 구별할 수 없다. 매번 다른 난수를 쓰면
 * CRC 일치가 곧 전송 성공의 증거가 된다.
 * 
 * ★ CRC 를 전송 '전에' 계산해 두는 점에 유의. 전송 도중 버퍼가 바뀔
 * 일은 없지만, 순서상 호스트가 인터럽트를 받았을 때 checksum 이
 * 이미 유효해야 하므로 앞에서 채워 둔다.
 * 
 * ★ 끝의 1ms 대기: 아래 영어 주석이 근거를 밝힌다 - 이 대기가 없으면
 * 호스트 쪽에서 L3 오류가 관측되었다고 한다. 마지막 쓰기가 호스트
 * 메모리에 실제로 도달하기 전에 인터럽트가 먼저 도착해 호스트가
 * 아직 오지 않은 데이터를 읽는 상황을 막는 것으로 읽히나, 그
 * 구체적 기전은 이 트리 안에서 확인하지 못했다.
 * 
 * DMA 방향이 DMA_TO_DEVICE 인 점에 유의 - 'CPU 가 쓰고 장치가 읽는다'
 * 는 뜻이라, 매핑 시점에 캐시를 밀어내(flush) 장치가 최신 값을 보게 한다.
 * 
 * 실행 컨텍스트: cmd_handler 의 워크큐 문맥.
 * 
 * 호출 체인:
 *   pci_epf_test_cmd_handler() -> [pci_epf_test_write]
 *     -> get_random_bytes() -> crc32_le() -> pci_epc_mem_map()
 *     -> pci_epf_test_data_transfer() 또는 memcpy_toio() */
static void pci_epf_test_write(struct pci_epf_test *epf_test,
			       struct pci_epf_test_reg *reg)
{
	/* [한국어] 오류 표시. */
	int ret = 0;
	/* [한국어] 보낼 데이터 버퍼와 커서. */
	void *dst_buf, *buf;
	/* [한국어] 호스트 메모리 매핑 결과. */
	struct pci_epc_map map;
	/* [한국어] DMA 경로에서 원본 버퍼의 DMA 주소. */
	phys_addr_t src_phys_addr;
	/* [한국어] 처리율 계산용 시각. */
	struct timespec64 start, end;
	/* [한국어] EPF 객체. */
	struct pci_epf *epf = epf_test->epf;
	/* [한국어] EP 컨트롤러. */
	struct pci_epc *epc = epf->epc;
	/* [한국어] 로그 대상. */
	struct device *dev = &epf->dev;
	/* [한국어] DMA 매핑의 기준이 되는 실제 하드웨어 장치. */
	struct device *dma_dev = epf->epc->dev.parent;
	/* [한국어] 호스트가 준 목적지 주소. */
	u64 dst_addr = le64_to_cpu(reg->dst_addr);
	/* [한국어] 전체 크기와 남은 크기. */
	size_t orig_size, dst_size;
	/* [한국어] 이번 조각의 매핑 크기. 0 초기화로 unmap 라벨의 판단 근거가 된다. */
	ssize_t map_size = 0;
	/* [한국어] 전송 옵션. */
	u32 flags = le32_to_cpu(reg->flags);
	/* [한국어] 호스트에 돌려줄 상태. */
	u32 status = 0;

	/* [한국어] 요청 크기를 두 변수에 넣는다. */
	orig_size = dst_size = le32_to_cpu(reg->size);

	/* [한국어] 보낼 데이터를 담을 버퍼를 전체 크기로 잡는다. */
	dst_buf = kzalloc(dst_size, GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!dst_buf) {
		ret = -ENOMEM;
		/* [한국어] 정리할 것 없이 상태 보고로. */
		goto set_status;
	}
	/* [한국어] ★ 버퍼를 난수로 채운다 - 전송이 실제로 일어났음을 CRC 로
	 * 증명할 수 있게 하는 핵심이다. */
	get_random_bytes(dst_buf, dst_size);
	/* [한국어] 그 난수 데이터의 CRC 를 계산해 호스트가 읽을 레지스터에 남긴다.
	 * 시드 ~0 은 READ 쪽과 같은 표준 초기값이라 양쪽 계산이 맞아떨어진다. */
	reg->checksum = cpu_to_le32(crc32_le(~0, dst_buf, dst_size));
	/* [한국어] 커서를 버퍼 앞에 둔다. */
	buf = dst_buf;

	/* [한국어] 남은 크기가 0 이 될 때까지 조각내어 쓴다. */
	while (dst_size) {
		/* [한국어] 호스트 목적지 주소를 EP 물리 주소 공간으로 매핑한다. */
		ret = pci_epc_mem_map(epc, epf->func_no, epf->vfunc_no,
					   dst_addr, dst_size, &map);
		/* [한국어] 매핑 실패. */
		if (ret) {
			/* [한국어] 원인을 남긴다. */
			dev_err(dev, "Failed to map address\n");
			/* [한국어] '목적지 주소가 잘못됐다' 고 알린다. */
			status = STATUS_DST_ADDR_INVALID;
			/* [한국어] 창을 잡지 못했으므로 버퍼만 반납한다. */
			goto free_buf;
		}

		/* [한국어] 이번에 실제로 매핑된 크기. */
		map_size = map.pci_size;
		/* [한국어] DMA 경로. */
		if (flags & FLAG_USE_DMA) {
			/* [한국어] 보낼 커널 버퍼를 DMA 주소로 매핑한다. DMA_TO_DEVICE 는
			 * 'CPU -> 장치' 방향이라, 이 호출이 캐시를 밀어내 장치가 최신
			 * 데이터를 읽게 한다. */
			src_phys_addr = dma_map_single(dma_dev, buf, map_size,
						       DMA_TO_DEVICE);
			/* [한국어] DMA 매핑 실패. */
			if (dma_mapping_error(dma_dev, src_phys_addr)) {
				/* [한국어] 원인을 남긴다. */
				dev_err(dev,
					"Failed to map source buffer addr\n");
				/* [한국어] 메모리 부족으로 처리한다. */
				ret = -ENOMEM;
				/* [한국어] 창이 잡혀 있으므로 unmap 라벨로. */
				goto unmap;
			}

			/* [한국어] 전송 시작 시각. */
			ktime_get_ts64(&start);

			/* [한국어] 커널 버퍼에서 호스트 창으로 옮긴다. 목적지가 첫 인자,
			 * 원본이 둘째 인자다. */
			ret = pci_epf_test_data_transfer(epf_test,
						map.phys_addr, src_phys_addr,
						/* [한국어] dma_remote 로 dst_addr(호스트 주소)을 넘긴다 - 전용 슬레이브
						 * 채널 구성에서 상대 끝 주소로 쓰인다. */
						map_size, dst_addr,
						/* [한국어] 방향은 EP -> 호스트. 이 값이 data_transfer 에서 tx 채널을
						 * 고르게 하고, 로컬 주소를 src 쪽으로 해석하게 한다. */
						DMA_MEM_TO_DEV);
			/* [한국어] 전송 실패. */
			if (ret)
				/* [한국어] 원인을 남긴다. READ 와 마찬가지로 여기서 곧바로 나가지 않는다. */
				dev_err(dev, "Data transfer failed\n");
			/* [한국어] 실패해도 시각을 찍고 아래 매핑 해제로 이어 간다. */
			ktime_get_ts64(&end);

			/* [한국어] ★ 성공이든 실패든 DMA 매핑을 되돌린다. TO_DEVICE 방향이라
			 * 이 호출 자체는 캐시 무효화를 하지 않지만, 매핑 자원은 반드시
			 * 반납해야 한다. */
			dma_unmap_single(dma_dev, src_phys_addr, map_size,
					 DMA_TO_DEVICE);

			/* [한국어] 이제서야 실패를 처리한다. */
			if (ret)
				/* [한국어] 창을 풀러 간다. */
				goto unmap;
		/* [한국어] CPU 경로. */
		} else {
			/* [한국어] 시작 시각. */
			ktime_get_ts64(&start);
			/* [한국어] 커널 버퍼에서 호스트 창(I/O 메모리)으로 쓴다. */
			memcpy_toio(map.virt_addr, buf, map_size);
			/* [한국어] 종료 시각. */
			ktime_get_ts64(&end);
		}

		/* [한국어] 남은 크기를 줄인다. */
		dst_size -= map_size;
		/* [한국어] 목적지 주소를 전진시킨다. */
		dst_addr += map_size;
		/* [한국어] 버퍼 커서도 전진시킨다. */
		buf += map_size;

		/* [한국어] 창을 푼다. */
		pci_epc_mem_unmap(epc, epf->func_no, epf->vfunc_no, &map);
		/* [한국어] 창을 다 풀었음을 표시한다. */
		map_size = 0;
	}

	/* [한국어] 처리율을 남긴다. */
	pci_epf_test_print_rate(epf_test, "WRITE", orig_size, &start, &end,
				flags & FLAG_USE_DMA);

	/*
	 * wait 1ms inorder for the write to complete. Without this delay L3
	 * error in observed in the host system.
	 */
	/* [한국어] 아래 영어 주석이 밝힌 대로 1ms 쉰다. usleep_range 는 1000~2000us
	 * 사이에서 깨어나도 좋다고 알려 주어, 커널이 다른 타이머와 묶어
	 * 처리할 여지를 준다(정확한 1ms 를 요구하는 udelay 보다 시스템에 가볍다). */
	usleep_range(1000, 2000);

/* [한국어] 창이 아직 잡혀 있는 실패 경로가 지나가는 지점. */
unmap:
	/* [한국어] 정상 경로는 위에서 0 으로 만들었으므로 건너뛴다. */
	if (map_size)
		/* [한국어] 남아 있던 창을 푼다. */
		pci_epc_mem_unmap(epc, epf->func_no, epf->vfunc_no, &map);

/* [한국어] 버퍼 반납 지점. */
free_buf:
	/* [한국어] 보낸 데이터 버퍼를 반납한다. */
	kfree(dst_buf);

/* [한국어] 상태 보고 지점. */
set_status:
	/* [한국어] 오류가 없었으면 */
	if (!ret)
		/* [한국어] 성공 비트. */
		status |= STATUS_WRITE_SUCCESS;
	/* [한국어] 아니면 */
	else
		/* [한국어] 실패 비트. */
		status |= STATUS_WRITE_FAIL;
	/* [한국어] 상태를 레지스터에 반영한다. checksum 은 이미 위에서 채워 두었다. */
	reg->status = cpu_to_le32(status);
}

/* [한국어] pci_epf_test_raise_irq - 호스트에게 인터럽트를 올린다
 * 
 * @epf_test: 이 EPF 인스턴스.
 * @reg: 공유 레지스터. status/irq_number/irq_type 을 읽고 status 를 쓴다.
 * @return: 없음. 실패해도 호스트에 따로 알릴 방법이 없어 로그만 남긴다.
 * 
 * ★ 이 함수는 명령 처리의 마지막 단계다. cmd_handler 의 거의 모든
 * case 가 '동작 수행 -> raise_irq' 순으로 짝지어져 있다. 호스트는
 * 인터럽트를 받고 나서야 status 레지스터를 읽으므로, 인터럽트가
 * 곧 '결과가 준비됐다' 는 신호다.
 * 
 * ★ 순서가 프로토콜의 핵심이다 - 아래 영어 주석이 그것을 명시한다.
 * 상태를 먼저 쓰고 인터럽트를 나중에 올려야 호스트가 갱신된 값을 본다.
 * 반대로 하면 호스트가 인터럽트를 받고 status 를 읽었을 때 아직 옛
 * 값이 남아 있을 수 있다. WRITE_ONCE 를 쓰는 것도 같은 이유로,
 * 컴파일러가 이 쓰기를 아래 pci_epc_raise_irq 호출 뒤로 옮기거나
 * 여러 번으로 쪼개지 못하게 막는다.
 * 
 * ★ 벡터 번호 검증: irq_number 는 호스트가 준 값이고 1-기반이다.
 * 설정된 벡터 수보다 크면 하드웨어에 넘겨서는 안 된다. count <= 0 은
 * 'MSI/MSI-X 가 아예 설정되지 않았다' 는 뜻이므로 함께 거른다.
 * 검증에 걸리면 인터럽트를 올리지 않고 그냥 돌아가는데, 상태에는
 * 이미 IRQ_RAISED 가 서 있어 호스트는 인터럽트를 기다리다 시간
 * 초과로 실패를 알게 된다.
 * 
 * ★ INTx 에서 벡터 번호로 0 을 넘기는 점에 유의 - INTx 는 벡터 개념이
 * 없으므로 그 인자가 무시된다.
 * 
 * 실행 컨텍스트: cmd_handler 의 워크큐 문맥, 또는 도어벨 인터럽트
 * 핸들러(스레드 IRQ)의 문맥.
 * 
 * 호출 체인:
 *   pci_epf_test_cmd_handler() / pci_epf_test_doorbell_handler()
 *     -> [pci_epf_test_raise_irq] -> pci_epc_raise_irq() */
static void pci_epf_test_raise_irq(struct pci_epf_test *epf_test,
				   struct pci_epf_test_reg *reg)
{
	/* [한국어] EPF 객체. */
	struct pci_epf *epf = epf_test->epf;
	/* [한국어] 로그 대상. */
	struct device *dev = &epf->dev;
	/* [한국어] EP 컨트롤러 - 인터럽트를 실제로 쏘는 주체다. */
	struct pci_epc *epc = epf->epc;
	/* [한국어] 현재 상태 비트를 읽어 온다. 각 명령 처리 함수가 이미 채워 둔
	 * 결과 비트에 아래에서 IRQ_RAISED 를 더한다. */
	u32 status = le32_to_cpu(reg->status);
	/* [한국어] 호스트가 요청한 벡터 번호(1-기반). */
	u32 irq_number = le32_to_cpu(reg->irq_number);
	/* [한국어] 호스트가 요청한 인터럽트 종류. */
	u32 irq_type = le32_to_cpu(reg->irq_type);
	/* [한국어] 설정된 벡터 개수를 받을 변수. */
	int count;

	/*
	 * Set the status before raising the IRQ to ensure that the host sees
	 * the updated value when it gets the IRQ.
	 */
	/* [한국어] '인터럽트를 올렸다' 는 비트를 더한다. */
	status |= STATUS_IRQ_RAISED;
	/* [한국어] ★ 아래 영어 주석의 그 순서. WRITE_ONCE 로 컴파일러 재배치를
	 * 막고, 인터럽트를 올리기 전에 상태를 확정한다. */
	WRITE_ONCE(reg->status, cpu_to_le32(status));

	/* [한국어] 요청한 인터럽트 종류로 분기. cmd_handler 가 이미 0~2 범위를
	 * 검증했으므로 default 는 사실상 도달하지 않는다. */
	switch (irq_type) {
	/* [한국어] INTx - 전통적인 핀 인터럽트. */
	case IRQ_TYPE_INTX:
		/* [한국어] 컨트롤러에게 INTx 를 어서트하라고 요청한다. */
		pci_epc_raise_irq(epc, epf->func_no, epf->vfunc_no,
				  /* [한국어] INTx 는 벡터가 없으므로 번호 자리에 0 을 넘긴다. */
				  PCI_IRQ_INTX, 0);
		break;
	/* [한국어] MSI. */
	case IRQ_TYPE_MSI:
		/* [한국어] 현재 설정된 MSI 벡터 개수를 묻는다. epc_init 에서
		 * pci_epc_set_msi 로 요청한 값을 하드웨어가 실제로 얼마나
		 * 허용했는지가 여기 반영된다. */
		count = pci_epc_get_msi(epc, epf->func_no, epf->vfunc_no);
		/* [한국어] ★ 범위 검증. 1-기반이므로 count 와 같은 값까지 유효하고,
		 * count 가 0 이하면 MSI 자체가 설정되지 않은 것이다. */
		if (irq_number > count || count <= 0) {
			/* [한국어] 요청 번호와 실제 개수를 함께 남겨 진단을 돕는다. */
			dev_err(dev, "Invalid MSI IRQ number %d / %d\n",
				irq_number, count);
			/* [한국어] 인터럽트를 올리지 않고 돌아간다. */
			return;
		}
		/* [한국어] 컨트롤러에게 지정한 MSI 벡터를 쏘라고 요청한다. */
		pci_epc_raise_irq(epc, epf->func_no, epf->vfunc_no,
				  /* [한국어] 벡터 번호는 1-기반 그대로 넘긴다 - EPC 코어의 규약이다. */
				  PCI_IRQ_MSI, irq_number);
		break;
	/* [한국어] MSI-X. */
	case IRQ_TYPE_MSIX:
		/* [한국어] 설정된 MSI-X 벡터 개수를 묻는다. */
		count = pci_epc_get_msix(epc, epf->func_no, epf->vfunc_no);
		/* [한국어] MSI 와 같은 범위 검증. */
		if (irq_number > count || count <= 0) {
			/* [한국어] 요청 번호와 실제 개수를 남긴다. */
			dev_err(dev, "Invalid MSI-X IRQ number %d / %d\n",
				irq_number, count);
			/* [한국어] 인터럽트를 올리지 않고 돌아간다. */
			return;
		}
		/* [한국어] 지정한 MSI-X 벡터를 쏜다. */
		pci_epc_raise_irq(epc, epf->func_no, epf->vfunc_no,
				  /* [한국어] 역시 1-기반 번호를 그대로 넘긴다. */
				  PCI_IRQ_MSIX, irq_number);
		break;
	/* [한국어] 알 수 없는 종류 - cmd_handler 의 검증을 통과했다면 오지 않는다. */
	default:
		/* [한국어] 방어적으로 로그만 남긴다. */
		dev_err(dev, "Failed to raise IRQ, unknown type\n");
		break;
	}
}

/* [한국어] pci_epf_test_doorbell_handler - 호스트가 초인종을 눌렀을 때의 핸들러
 * 
 * @irq: 도어벨에 배정된 가상 IRQ 번호(쓰지 않는다).
 * @data: request_threaded_irq 에 넘긴 pci_epf_test 포인터.
 * @return: 항상 IRQ_HANDLED - 이 IRQ 는 도어벨 전용이라 원인이 하나뿐이다.
 * 
 * ★ 도어벨이란: 보통 호스트는 명령 레지스터에 값을 쓰고, EP 는 1ms
 * 주기로 그것을 폴링해 알아챈다. 도어벨은 그 폴링을 없애는 장치다 -
 * 호스트가 특정 BAR 오프셋에 특정 값을 쓰면, 그 쓰기가 EP 쪽 플랫폼
 * MSI 컨트롤러에 도달해 곧바로 인터럽트가 뜬다. 즉 '호스트의 메모리
 * 쓰기' 를 'EP 의 인터럽트' 로 바꾸는 하드웨어 경로다.
 * 
 * 하는 일은 두 가지뿐이다 - 상태에 도어벨 성공 비트를 세우고,
 * 호스트에게 되알림 인터럽트를 올린다. 그래야 호스트가 '초인종이
 * 실제로 EP 에 도달했다' 를 확인할 수 있다.
 * 
 * 실행 컨텍스트: request_threaded_irq 에 IRQF_ONESHOT 로 등록된
 * 스레드 핸들러라 프로세스 문맥이다. 그래서 raise_irq 처럼 잠들 수
 * 있는 일을 해도 된다 - 하드 IRQ 문맥이었다면 위험했을 것이다.
 * 
 * 호출 체인:
 *   (호스트의 BAR 쓰기) -> 플랫폼 MSI 컨트롤러 -> IRQ 코어
 *     -> [pci_epf_test_doorbell_handler] -> pci_epf_test_raise_irq() */
static irqreturn_t pci_epf_test_doorbell_handler(int irq, void *data)
{
	/* [한국어] 등록 때 넘긴 EPF 상태 구조체. */
	struct pci_epf_test *epf_test = data;
	/* [한국어] 공유 레지스터가 들어 있는 BAR 번호. */
	enum pci_barno test_reg_bar = epf_test->test_reg_bar;
	/* [한국어] 그 BAR 의 커널 가상 주소를 레지스터 구조체로 본다. */
	struct pci_epf_test_reg *reg = epf_test->reg[test_reg_bar];
	/* [한국어] 현재 상태를 읽어 온다. */
	u32 status = le32_to_cpu(reg->status);

	/* [한국어] 도어벨이 울렸음을 표시한다. */
	status |= STATUS_DOORBELL_SUCCESS;
	/* [한국어] 레지스터에 반영한다. 여기서는 WRITE_ONCE 를 쓰지 않는데,
	 * 바로 다음 줄의 raise_irq 안에서 다시 WRITE_ONCE 로 상태를
	 * 확정하므로 순서가 결과적으로 보장된다. */
	reg->status = cpu_to_le32(status);
	/* [한국어] 호스트에게 인터럽트를 올려 결과를 확인하게 한다. */
	pci_epf_test_raise_irq(epf_test, reg);

	/* [한국어] 이 IRQ 의 유일한 원인이므로 무조건 처리했다고 보고한다. */
	return IRQ_HANDLED;
}

/* [한국어] pci_epf_test_doorbell_cleanup - 도어벨 자원을 반납하고 호스트에 알린다
 * 
 * @epf_test: 이 EPF 인스턴스.
 * @return: 없음
 * 
 * 도어벨을 끌 때와, 켜다가 실패했을 때 공통으로 부르는 정리 함수다.
 * 두 가지를 한다 - (1) 공유 레지스터의 doorbell_bar 를 NO_BAR(-1)로
 * 되돌려 호스트에게 '이제 초인종이 없다' 고 알리고, (2) 플랫폼 MSI
 * 도어벨을 반납한다.
 * 
 * ★ IRQ 해제(free_irq)는 여기 없다는 점에 유의. 호출자가 먼저
 * free_irq 를 한 뒤 이 함수를 부른다 - 순서가 반대면 IRQ 가 아직
 * 걸려 있는 상태에서 도어벨이 사라져 위험하다.
 * 
 * 실행 컨텍스트: cmd_handler 의 워크큐 문맥.
 * 
 * 호출 체인:
 *   pci_epf_test_enable_doorbell()(실패 경로) /
 *   pci_epf_test_disable_doorbell() -> [pci_epf_test_doorbell_cleanup]
 *     -> pci_epf_free_doorbell() */
static void pci_epf_test_doorbell_cleanup(struct pci_epf_test *epf_test)
{
	/* [한국어] 공유 레지스터의 주소를 구한다. */
	struct pci_epf_test_reg *reg = epf_test->reg[epf_test->test_reg_bar];
	/* [한국어] EPF 객체. */
	struct pci_epf *epf = epf_test->epf;

	/* [한국어] 호스트에게 '초인종 없음' 을 알린다. NO_BAR 는 enum pci_barno 의
	 * -1 이며, 호스트는 이 값을 보고 도어벨 시험을 건너뛴다. */
	reg->doorbell_bar = cpu_to_le32(NO_BAR);

	/* [한국어] 플랫폼 MSI 도어벨을 반납한다. 이 안에서 epf->db_msg 와
	 * num_db 가 정리된다. */
	pci_epf_free_doorbell(epf);
}

/* [한국어] pci_epf_test_enable_doorbell - 호스트가 쓰기만 하면 EP 에 인터럽트가
 *                                뜨는 '초인종' 을 만든다
 * 
 * @epf_test: 이 EPF 인스턴스.
 * @reg: 공유 레지스터. doorbell_bar/offset/data 와 status 를 쓴다.
 * @return: 없음. 성공/실패는 reg->status 로 알린다.
 * 
 * ★ 이 기능의 원리(이 파일에서 가장 하드웨어에 가까운 부분):
 * EP 쪽 SoC 에는 플랫폼 MSI 컨트롤러가 있고, 그 컨트롤러의 특정
 * 물리 주소에 특정 값을 쓰면 인터럽트가 뜬다. 한편 BAR 의 inbound
 * 변환은 '호스트가 이 BAR 에 쓴 것이 EP 의 어느 물리 주소에 닿는가'
 * 를 정한다. 그렇다면 어떤 BAR 의 inbound 변환을 그 MSI 컨트롤러
 * 주소로 돌려놓으면, 호스트가 그 BAR 에 쓰는 순간 EP 에 인터럽트가
 * 뜬다. 그것이 도어벨이다.
 * 
 * 동작 단계:
 *   1) 플랫폼 MSI 도어벨을 하나 확보한다 - 주소와 데이터, 그리고
 *      EP 쪽에서 그것을 받을 가상 IRQ 번호를 얻는다.
 *   2) test_reg_bar 다음으로 비어 있는 BAR 를 고른다. 공유 레지스터가
 *      든 BAR 를 쓸 수는 없으므로 그 다음부터 찾는다.
 *   3) 그 IRQ 에 핸들러를 건다.
 *   4) MSI 컨트롤러 주소를 BAR 크기에 맞춰 내림 정렬하고, 그 안에서의
 *      오프셋을 구한다. BAR 는 크기 단위로만 정렬될 수 있기 때문이다.
 *   5) 원래 BAR 의 크기/플래그는 그대로 두고 물리 주소만 바꾼 서술자로
 *      set_bar 를 다시 부른다. clear_bar 를 부르지 않는 이유는 아래
 *      disable 함수의 영어 주석이 밝힌다 - 호스트가 배정한 PCI 주소가
 *      지워지기 때문이다. 이 '다시 걸기' 가 성립하려면 EPC 가
 *      dynamic_inbound_mapping 을 지원해야 한다.
 *   6) 호스트에게 BAR 번호, 오프셋, 써야 할 데이터를 알려 준다.
 * 
 * 에러 경로가 세 라벨로 나뉜 이유: 확보한 자원이 단계마다 다르다.
 * IRQ 를 걸었다면 free_irq 부터, 도어벨만 잡았다면 cleanup 부터,
 * 아무것도 없으면 상태 보고만.
 * 
 * 실행 컨텍스트: cmd_handler 의 워크큐 문맥.
 * 
 * 호출 체인:
 *   pci_epf_test_cmd_handler() -> [pci_epf_test_enable_doorbell]
 *     -> pci_epf_alloc_doorbell() -> request_threaded_irq()
 *     -> pci_epf_align_inbound_addr() -> pci_epc_set_bar() */
static void pci_epf_test_enable_doorbell(struct pci_epf_test *epf_test,
					 struct pci_epf_test_reg *reg)
{
	/* [한국어] 현재 상태 비트. */
	u32 status = le32_to_cpu(reg->status);
	/* [한국어] EPF 객체. */
	struct pci_epf *epf = epf_test->epf;
	/* [한국어] EP 컨트롤러. */
	struct pci_epc *epc = epf->epc;
	/* [한국어] 도어벨의 MSI 메시지(주소 + 데이터). */
	struct msi_msg *msg;
	/* [한국어] 도어벨을 배정할 BAR 번호. */
	enum pci_barno bar;
	/* [한국어] 정렬된 BAR 시작 주소로부터 실제 MSI 주소까지의 거리. */
	size_t offset;
	/* [한국어] 반환값. */
	int ret;

	/* [한국어] 1) 도어벨 하나를 확보한다. 이 안에서 플랫폼 MSI IRQ 가 할당되고
	 * epf->db_msg[] 에 주소/데이터/virq 가 채워진다.
	 * CONFIG_PCI_ENDPOINT_MSI_DOORBELL 이 꺼져 있으면 -ENODATA 가 온다. */
	ret = pci_epf_alloc_doorbell(epf, 1);
	/* [한국어] 확보 실패 - 아직 잡은 것이 없다. */
	if (ret)
		/* [한국어] 상태 보고로 곧장 간다. */
		goto set_status_err;

	/* [한국어] 첫 도어벨의 MSI 메시지를 가리킨다. */
	msg = &epf->db_msg[0].msg;
	/* [한국어] 2) 쓸 수 있는 BAR 를 찾는다. 시작점을 test_reg_bar + 1 로 두어
	 * 공유 레지스터가 든 BAR 를 건너뛴다 - 그 BAR 의 inbound 변환을
	 * 바꿔 버리면 호스트가 레지스터를 읽지 못하게 된다. */
	bar = pci_epc_get_next_free_bar(epf_test->epc_features, epf_test->test_reg_bar + 1);
	/* [한국어] NO_BAR(-1)가 돌아왔다는 뜻 - 쓸 수 있는 BAR 가 없다. */
	if (bar < BAR_0)
		/* [한국어] 도어벨을 반납해야 하므로 cleanup 라벨로. */
		goto err_doorbell_cleanup;

	/* [한국어] 3) 도어벨 IRQ 에 핸들러를 건다. 첫 인자 다음의 NULL 은
	 * '하드 IRQ 핸들러 없음' 이라는 뜻이고, 그 다음이 스레드 핸들러다. */
	ret = request_threaded_irq(epf->db_msg[0].virq, NULL,
				   /* [한국어] IRQF_ONESHOT 는 스레드 핸들러가 끝날 때까지 인터럽트를 마스킹해
				    * 둔다 - 하드 핸들러가 없을 때 필수인 플래그다. */
				   pci_epf_test_doorbell_handler, IRQF_ONESHOT,
				   /* [한국어] /proc/interrupts 에 보일 이름과, 핸들러에 넘길 인자. */
				   "pci-ep-test-doorbell", epf_test);
	/* [한국어] IRQ 등록 실패. */
	if (ret) {
		/* [한국어] 어느 virq 에서 실패했는지 남긴다. */
		dev_err(&epf->dev,
			"Failed to request doorbell IRQ: %d\n",
			epf->db_msg[0].virq);
		/* [한국어] 도어벨을 반납한다. */
		goto err_doorbell_cleanup;
	}

	/* [한국어] 호스트에게 '이 값을 써야 초인종이 울린다' 고 알린다. */
	reg->doorbell_data = cpu_to_le32(msg->data);
	/* [한국어] '이 BAR 에 써야 한다' 고 알린다. */
	reg->doorbell_bar = cpu_to_le32(bar);

	/* [한국어] 메시지 포인터를 다시 얻는다. 위에서 이미 같은 값을 담았으므로
	 * 이 줄은 실질적인 변화를 만들지 않는다 - 중간에 alloc_doorbell 이
	 * 다시 불리는 코드가 있었던 흔적으로 보이나, 이 트리에서 그
	 * 근거를 확인하지는 못했다. */
	msg = &epf->db_msg[0].msg;
	/* [한국어] 4) 64 비트 MSI 주소를 상위/하위 32 비트를 합쳐 만든 뒤,
	 * BAR 크기에 맞춰 내림 정렬한다. 이 헬퍼는 정렬된 시작 주소를
	 * db_bar.phys_addr 에, 그 안에서의 오프셋을 offset 에 채운다.
	 * 정렬이 필요한 이유는 대부분의 EP 컨트롤러가 BAR 시작 주소의
	 * 하위 비트를 잘라 버리기 때문이다. */
	ret = pci_epf_align_inbound_addr(epf, bar, ((u64)msg->address_hi << 32) | msg->address_lo,
					 &epf_test->db_bar.phys_addr, &offset);

	/* [한국어] 정렬 실패(현재 구현은 실패하지 않지만 방어적으로 검사한다). */
	if (ret)
		/* [한국어] IRQ 를 먼저 풀어야 하므로 그쪽 라벨로. */
		goto err_free_irq;

	/* [한국어] 호스트에게 'BAR 안의 이 오프셋에 써야 한다' 고 알린다.
	 * BAR 시작을 내림 정렬했으므로 실제 MSI 주소는 그만큼 뒤에 있다. */
	reg->doorbell_offset = cpu_to_le32(offset);

	/* [한국어] 5) 도어벨용 BAR 서술자를 채운다 - 번호는 방금 고른 것. */
	epf_test->db_bar.barno = bar;
	/* [한국어] 크기는 원래 BAR 와 같아야 한다. 호스트가 이미 그 크기로
	 * 주소를 배정해 두었기 때문이다. */
	epf_test->db_bar.size = epf->bar[bar].size;
	/* [한국어] 플래그(32/64 비트, prefetchable 등)도 원래 것을 그대로 쓴다.
	 * 바뀌는 것은 오직 phys_addr 하나뿐이다. */
	epf_test->db_bar.flags = epf->bar[bar].flags;

	/* [한국어] BAR 의 inbound 변환을 MSI 컨트롤러 주소로 다시 건다.
	 * ★ clear_bar 없이 set_bar 를 두 번 부르는 것이라, EPC 가
	 * dynamic_inbound_mapping 을 지원해야 성립한다. */
	ret = pci_epc_set_bar(epc, epf->func_no, epf->vfunc_no, &epf_test->db_bar);
	/* [한국어] 다시 걸기 실패. */
	if (ret)
		/* [한국어] IRQ 를 풀고 도어벨을 반납한다. */
		goto err_free_irq;

	/* [한국어] 성공 비트. */
	status |= STATUS_DOORBELL_ENABLE_SUCCESS;
	/* [한국어] 상태를 반영하고 */
	reg->status = cpu_to_le32(status);
	/* [한국어] 정상 종료한다. */
	return;

/* [한국어] IRQ 를 이미 건 뒤의 실패가 오는 지점. */
err_free_irq:
	/* [한국어] 핸들러를 뗀다. 두 번째 인자는 등록 때 넘긴 것과 같아야
	 * 어느 핸들러인지 식별된다. */
	free_irq(epf->db_msg[0].virq, epf_test);
/* [한국어] 도어벨만 잡은 상태의 실패가 오는 지점(또는 위에서 흘러온 경우). */
err_doorbell_cleanup:
	/* [한국어] 도어벨을 반납하고 호스트에게 NO_BAR 를 알린다. */
	pci_epf_test_doorbell_cleanup(epf_test);
/* [한국어] 아무것도 잡지 못한 실패가 오는 지점. */
set_status_err:
	/* [한국어] 실패 비트. */
	status |= STATUS_DOORBELL_ENABLE_FAIL;
	/* [한국어] 상태를 반영한다. */
	reg->status = cpu_to_le32(status);
}

/* [한국어] pci_epf_test_disable_doorbell - 도어벨을 끄고 BAR 를 원래대로 되돌린다
 * 
 * @epf_test: 이 EPF 인스턴스.
 * @reg: 공유 레지스터. doorbell_bar 를 읽고 status 를 쓴다.
 * @return: 없음.
 * 
 * ★ 되돌리기의 핵심은 아래 영어 주석이 설명하는 그대로다:
 * 도어벨을 켤 때 clear_bar 를 부르지 않고 set_bar 만 다시 불렀으므로,
 * 끌 때도 clear_bar 가 아니라 '원래 서술자로 set_bar 를 다시' 불러야
 * 한다. clear_bar 를 부르면 호스트가 그 BAR 에 배정해 둔 PCI 주소까지
 * 지워져, 호스트 쪽에서 BAR 가 사라진 것처럼 보이게 된다.
 * 
 * 되돌릴 목적지는 epf->bar[bar] 다 - alloc_space 가 잡아 둔 원래
 * 메모리를 가리키는 서술자이며, 도어벨을 켤 때 건드리지 않고
 * db_bar 라는 별도 사본에 작업한 덕분에 그대로 남아 있다.
 * 
 * 순서에 유의: IRQ 를 먼저 풀고, 도어벨을 반납하고, 마지막에 BAR 를
 * 되돌린다. BAR 를 먼저 되돌리면 그 사이 호스트의 쓰기가 엉뚱한 곳에
 * 닿는다.
 * 
 * 실행 컨텍스트: cmd_handler 의 워크큐 문맥.
 * 
 * 호출 체인:
 *   pci_epf_test_cmd_handler() -> [pci_epf_test_disable_doorbell]
 *     -> free_irq() -> pci_epf_test_doorbell_cleanup() -> pci_epc_set_bar() */
static void pci_epf_test_disable_doorbell(struct pci_epf_test *epf_test,
					  struct pci_epf_test_reg *reg)
{
	/* [한국어] 도어벨이 배정됐던 BAR 번호를 공유 레지스터에서 읽는다.
	 * enable 이 채워 둔 값이며, 켠 적이 없으면 NO_BAR 다. */
	enum pci_barno bar = le32_to_cpu(reg->doorbell_bar);
	/* [한국어] 현재 상태 비트. */
	u32 status = le32_to_cpu(reg->status);
	/* [한국어] EPF 객체. */
	struct pci_epf *epf = epf_test->epf;
	/* [한국어] EP 컨트롤러. */
	struct pci_epc *epc = epf->epc;
	/* [한국어] 반환값. */
	int ret;

	/* [한국어] NO_BAR(-1) 이면 도어벨이 켜져 있지 않다 - 끌 것이 없다.
	 * ★ 이 검사가 곧 '켜지 않고 끄기' 요청에 대한 방어다. */
	if (bar < BAR_0)
		/* [한국어] 실패로 보고한다. */
		goto set_status_err;

	/* [한국어] 도어벨 IRQ 핸들러를 뗀다. 이 뒤로는 호스트가 초인종을 눌러도
	 * EP 쪽에서 아무 일도 일어나지 않는다. */
	free_irq(epf->db_msg[0].virq, epf_test);
	/* [한국어] 도어벨을 반납하고 호스트에게 NO_BAR 를 알린다. */
	pci_epf_test_doorbell_cleanup(epf_test);

	/*
	 * The doorbell feature temporarily overrides the inbound translation
	 * to point to the address stored in epf_test->db_bar.phys_addr, i.e.,
	 * it calls set_bar() twice without ever calling clear_bar(), as
	 * calling clear_bar() would clear the BAR's PCI address assigned by
	 * the host. Thus, when disabling the doorbell, restore the inbound
	 * translation to point to the memory allocated for the BAR.
	 */
	/* [한국어] ★ 위 영어 주석의 그 되돌리기. 원래 서술자로 set_bar 를 다시
	 * 불러 inbound 변환이 alloc_space 가 잡아 둔 메모리를 가리키게 한다. */
	ret = pci_epc_set_bar(epc, epf->func_no, epf->vfunc_no, &epf->bar[bar]);
	/* [한국어] 되돌리기 실패 - 이 경우 BAR 는 여전히 MSI 컨트롤러를 가리키는
	 * 상태로 남아, 호스트가 그 BAR 에 쓰면 인터럽트가 뜨지만 핸들러가
	 * 없으므로 아무 일도 일어나지 않는다. */
	if (ret)
		/* [한국어] 실패로 보고한다. */
		goto set_status_err;

	/* [한국어] 성공 비트. */
	status |= STATUS_DOORBELL_DISABLE_SUCCESS;
	/* [한국어] 상태를 반영하고 */
	reg->status = cpu_to_le32(status);

	/* [한국어] 정상 종료한다. */
	return;

/* [한국어] 실패가 모이는 지점. */
set_status_err:
	/* [한국어] 실패 비트. */
	status |= STATUS_DOORBELL_DISABLE_FAIL;
	/* [한국어] 상태를 반영한다. */
	reg->status = cpu_to_le32(status);
}

/* [한국어] pci_epf_test_subrange_sig_byte - BAR 부분 매핑 시험의 서명 바이트를 만든다
 * 
 * @barno: 어느 BAR 인지(0~5).
 * @subno: 그 BAR 안의 몇 번째 조각인지(0 또는 1).
 * @return: (BAR, 조각) 조합마다 유일한 8 비트 값.
 * 
 * ★ 왜 이런 값이 필요한가: 부분 매핑 시험은 'BAR 의 앞 절반과 뒤
 * 절반이 실제로 뒤바뀌어 매핑되었는가' 를 확인한다. 그러려면 호스트가
 * BAR 를 읽었을 때 '지금 보고 있는 것이 어느 조각인가' 를 알 수 있어야
 * 하는데, 조각마다 서로 다른 바이트로 채워 두면 그 판별이 가능해진다.
 * 
 * 값 설계: 0x50 을 기준점으로 삼고 BAR 마다 8 씩 벌려 두었다.
 * 조각이 최대 8 개까지 늘어나도 BAR 사이에 충돌이 없다는 뜻이다.
 * 0x50 을 고른 이유는 이 트리 안에서 근거를 찾지 못했다 - 0 이나
 * 0xFF 같은 '우연히 나올 법한' 값을 피하려는 선택으로 보인다.
 * 
 * 실행 컨텍스트: cmd_handler 의 워크큐 문맥. 계산만 한다.
 * 
 * 호출 체인:
 *   pci_epf_test_bar_subrange_setup() -> [pci_epf_test_subrange_sig_byte] */
static u8 pci_epf_test_subrange_sig_byte(enum pci_barno barno,
					 unsigned int subno)
{
	/* [한국어] 기준값 0x50 에 BAR 번호 * 8 과 조각 번호를 더한다. 반환 타입이
	 * u8 이라 넘치면 잘리지만, BAR 5 조각 1 이어도 0x79 라 여유가 있다. */
	return 0x50 + (barno * 8) + subno;
}

/* [한국어] pci_epf_test_bar_subrange_setup - BAR 하나를 조각내고 순서를 뒤바꿔 매핑한다
 * 
 * @epf_test: 이 EPF 인스턴스.
 * @reg: 공유 레지스터. ★ size 필드가 크기가 아니라 'BAR 번호' 를 나른다.
 *       status 를 쓴다.
 * @return: 없음. 결과는 reg->status 로 전달된다.
 * 
 * ★ 부분 매핑(subrange mapping)이란: 보통 BAR 하나는 EP 의 연속된
 * 물리 메모리 한 덩어리에 대응한다. 부분 매핑은 그것을 여러 조각으로
 * 나눠, 조각마다 다른 물리 주소를 지정할 수 있게 하는 기능이다.
 * 컨트롤러가 BAR 하나에 여러 inbound 창(iATU 등)을 걸 수 있어야 한다.
 * 
 * ★ 이 시험의 영리한 점: 두 조각의 물리 주소를 '맞바꿔' 매핑한다.
 *   BAR 오프셋 0..half   -> 물리 half..end
 *   BAR 오프셋 half..end -> 물리 0..half
 * 그리고 각 물리 영역을 서로 다른 서명 바이트로 채운다. 호스트가
 * BAR 를 읽었을 때 앞 절반에서 '뒤 조각의 서명' 이 나오면 순서가
 * 실제로 적용된 것이고, 그냥 연속 매핑이었다면 서명이 뒤바뀌지 않는다.
 * i ^ 1 은 0<->1 을 맞바꾸는 XOR 관용구다.
 * 
 * ★ 실패 시 원상 복구가 이 함수의 어려운 부분: set_bar 가 실패하면
 * BAR 가 어떤 상태로 남았는지 알 수 없으므로, 옛 submap 을 되돌리고
 * set_bar 를 한 번 더 불러 원래 매핑을 복원한다. 그 복원마저 실패하면
 * 경고만 남긴다 - 더 할 수 있는 일이 없다.
 * 
 * ★ -ENOSPC 를 따로 다루는 이유: 설정이 잘못된 것이 아니라 컨트롤러의
 * inbound 창이 모자란 것이므로, 호스트에게 STATUS_NO_RESOURCE 로
 * 구별해 알린다. 그러면 호스트는 '이 컨트롤러에서는 이 시험을
 * 할 수 없다' 고 판단할 수 있다.
 * 
 * 실행 컨텍스트: cmd_handler 의 워크큐 문맥.
 * 
 * 호출 체인:
 *   pci_epf_test_cmd_handler() -> [pci_epf_test_bar_subrange_setup]
 *     -> kzalloc_objs() -> pci_epc_set_bar() -> memset() */
static void pci_epf_test_bar_subrange_setup(struct pci_epf_test *epf_test,
					    struct pci_epf_test_reg *reg)
{
	/* [한국어] 새로 만들 조각 서술자 배열과, 되돌리기용 옛 배열. */
	struct pci_epf_bar_submap *submap, *old_submap;
	/* [한국어] EPF 객체. */
	struct pci_epf *epf = epf_test->epf;
	/* [한국어] EP 컨트롤러. */
	struct pci_epc *epc = epf->epc;
	/* [한국어] 대상 BAR 서술자. */
	struct pci_epf_bar *bar;
	/* [한국어] 나눌 조각 수(2)와, 되돌리기용 옛 조각 수. */
	unsigned int nsub = PCI_EPF_TEST_BAR_SUBRANGE_NSUB, old_nsub;
	/* reg->size carries BAR number for BAR_SUBRANGE_* commands. */
	/* [한국어] 아래 영어 주석대로 size 필드를 BAR 번호로 해석한다.
	 * 호스트가 준 값이므로 아래에서 반드시 검증해야 한다. */
	enum pci_barno barno = le32_to_cpu(reg->size);
	/* [한국어] 현재 상태 비트. */
	u32 status = le32_to_cpu(reg->status);
	/* [한국어] 루프 인덱스와, 그 조각이 가리킬 물리 조각의 인덱스. */
	unsigned int i, phys_idx;
	/* [한국어] 조각 하나의 크기. */
	size_t sub_size;
	/* [한국어] 서명을 채울 커널 가상 주소. */
	u8 *addr;
	/* [한국어] 반환값. */
	int ret;

	/* [한국어] ★ 범위 검증. 아래에서 epf->bar[barno] 로 인덱싱하므로 필수다.
	 * enum 타입이지만 값 자체는 호스트가 준 것이라 신뢰할 수 없다. */
	if (barno >= PCI_STD_NUM_BARS) {
		/* [한국어] 잘못된 BAR 번호를 남긴다. */
		dev_err(&epf->dev, "Invalid barno: %d\n", barno);
		/* [한국어] 실패 보고로. */
		goto err;
	}

	/* Host side should've avoided test_reg_bar, this is a safeguard. */
	/* [한국어] 아래 영어 주석대로 호스트가 피했어야 하지만, 공유 레지스터가
	 * 든 BAR 를 조각내면 레지스터 자체를 읽지 못하게 되므로 방어한다. */
	if (barno == epf_test->test_reg_bar) {
		/* [한국어] 원인을 남긴다. */
		dev_err(&epf->dev, "test_reg_bar cannot be used for subrange test\n");
		/* [한국어] 실패 보고로. */
		goto err;
	}

	/* [한국어] 컨트롤러가 이미 설정된 BAR 를 다시 걸 수 있어야 하고, */
	if (!epf_test->epc_features->dynamic_inbound_mapping ||
	    /* [한국어] 부분 매핑도 지원해야 한다. 헤더에 명시된 대로 후자는
	     * 전자에 의존하는 기능이라 둘을 함께 확인한다. */
	    !epf_test->epc_features->subrange_mapping) {
		/* [한국어] 지원하지 않음을 남긴다. */
		dev_err(&epf->dev, "epc driver does not support subrange mapping\n");
		/* [한국어] 실패 보고로. */
		goto err;
	}

	/* [한국어] 대상 BAR 서술자를 가리킨다. */
	bar = &epf->bar[barno];
	/* [한국어] alloc_space 가 이 BAR 에 메모리를 잡지 못했다면 크기나 주소가
	 * 비어 있다 - 조각낼 대상이 없다. */
	if (!bar->size || !bar->addr) {
		/* [한국어] 무엇이 비었는지 남긴다. */
		dev_err(&epf->dev, "bar size/addr (%zu/%p) is invalid\n",
			bar->size, bar->addr);
		/* [한국어] 실패 보고로. */
		goto err;
	}

	/* [한국어] 조각 수로 정확히 나누어떨어져야 한다. 나머지가 생기면 BAR
	 * 전체를 덮지 못해 매핑에 구멍이 생긴다. */
	if (bar->size % nsub) {
		/* [한국어] 나눌 수 없음을 남긴다. */
		dev_err(&epf->dev, "BAR size %zu is not divisible by %u\n",
			bar->size, nsub);
		/* [한국어] 실패 보고로. */
		goto err;
	}

	/* [한국어] 조각 하나의 크기 - 위에서 나누어떨어짐을 확인했으므로 정확하다. */
	sub_size = bar->size / nsub;

	/* [한국어] 조각 서술자 배열을 잡는다. kzalloc_objs 는 타입에서 크기를
	 * 뽑고 개수를 곱해 주므로 곱셈 넘침 실수가 없다. */
	submap = kzalloc_objs(*submap, nsub);
	/* [한국어] 메모리 부족. */
	if (!submap)
		/* [한국어] 실패 보고로. */
		goto err;

	/* [한국어] 조각마다 어느 물리 영역을 가리킬지 정한다. */
	for (i = 0; i < nsub; i++) {
		/* Swap the two halves so RC can verify ordering. */
		/* [한국어] ★ 아래 영어 주석의 그 맞바꾸기. XOR 1 로 0<->1 을 뒤집는다. */
		phys_idx = i ^ 1;
		/* [한국어] BAR 오프셋 i 번째 조각이 물리 phys_idx 번째 영역을 가리키게 한다. */
		submap[i].phys_addr = bar->phys_addr + (phys_idx * sub_size);
		/* [한국어] 모든 조각의 크기는 같다. */
		submap[i].size = sub_size;
	}

	/* [한국어] 되돌리기를 위해 현재 조각 배열을 보관한다. 처음이면 NULL 이다. */
	old_submap = bar->submap;
	/* [한국어] 현재 조각 수도 보관한다. 처음이면 0 이다. */
	old_nsub = bar->num_submap;

	/* [한국어] 새 조각 배열을 서술자에 건다. */
	bar->submap = submap;
	/* [한국어] 조각 수도 채운다. num_submap 이 0 보다 크면 submap 이
	 * BAR 전체 배치를 서술한다는 것이 헤더의 규약이다. */
	bar->num_submap = nsub;

	/* [한국어] 새 배치로 BAR 를 다시 건다. clear_bar 없이 set_bar 를 다시
	 * 부르는 것이라 dynamic_inbound_mapping 이 필요하다. */
	ret = pci_epc_set_bar(epc, epf->func_no, epf->vfunc_no, bar);
	/* [한국어] 다시 걸기 실패. */
	if (ret) {
		/* [한국어] 원인을 남긴다. */
		dev_err(&epf->dev, "pci_epc_set_bar() failed: %d\n", ret);
		/* [한국어] inbound 창이 모자란 경우라면 */
		if (ret == -ENOSPC)
			/* [한국어] '자원 부족' 을 따로 알려, 설정 오류와 구별되게 한다. */
			status |= STATUS_NO_RESOURCE;
		/* [한국어] 옛 조각 배열로 되돌린다. */
		bar->submap = old_submap;
		/* [한국어] 옛 조각 수도 되돌린다. */
		bar->num_submap = old_nsub;
		/* [한국어] 원래 배치로 BAR 를 다시 건다 - 실패한 set_bar 가 하드웨어를
		 * 어떤 상태로 남겼는지 알 수 없으므로 명시적으로 복원한다. */
		ret = pci_epc_set_bar(epc, epf->func_no, epf->vfunc_no, bar);
		/* [한국어] 복원마저 실패하면 */
		if (ret)
			/* [한국어] 경고만 남긴다. 더 할 수 있는 일이 없고, BAR 는 쓸 수 없는
			 * 상태로 남는다. */
			dev_warn(&epf->dev, "Failed to restore the original BAR mapping: %d\n",
				 ret);

		/* [한국어] 쓰이지 못한 새 배열을 반납한다. */
		kfree(submap);
		/* [한국어] 실패 보고로. */
		goto err;
	}
	/* [한국어] 새 배치가 성공했으므로 옛 배열은 이제 필요 없다.
	 * 처음이면 NULL 이라 아무 일도 하지 않는다. */
	kfree(old_submap);

	/*
	 * Fill deterministic signatures into the physical regions that
	 * each BAR subrange maps to. RC verifies these to ensure the
	 * submap order is really applied.
	 */
	/* [한국어] 서명을 채울 시작 주소. bar->addr 는 alloc_space 가 잡은 커널
	 * 가상 주소이며, 이것이 곧 물리 영역의 CPU 쪽 창이다. */
	addr = (u8 *)bar->addr;
	/* [한국어] 조각마다 서명을 채운다. */
	for (i = 0; i < nsub; i++) {
		/* [한국어] ★ 여기서도 같은 맞바꾸기를 적용한다. i 번째 BAR 오프셋 조각이
		 * 가리키는 물리 영역이 phys_idx 번째이므로, 그 물리 영역에
		 * 'BAR 오프셋 i 의 서명' 을 써야 호스트가 앞 절반을 읽었을 때
		 * 조각 0 의 서명을 보게 된다. */
		phys_idx = i ^ 1;
		/* [한국어] 물리 영역의 시작 위치를 계산해 */
		memset(addr + (phys_idx * sub_size),
		       /* [한국어] 그 (BAR, 조각) 조합의 서명 바이트로 */
		       pci_epf_test_subrange_sig_byte(barno, i),
		       /* [한국어] 조각 전체를 채운다. */
		       sub_size);
	}

	/* [한국어] 성공 비트. */
	status |= STATUS_BAR_SUBRANGE_SETUP_SUCCESS;
	/* [한국어] 상태를 반영하고 */
	reg->status = cpu_to_le32(status);
	/* [한국어] 정상 종료한다. */
	return;

/* [한국어] 모든 실패가 모이는 지점. */
err:
	/* [한국어] 실패 비트. -ENOSPC 였다면 STATUS_NO_RESOURCE 도 함께 서 있어
	 * 호스트가 자원 부족과 설정 오류를 구별할 수 있다. */
	status |= STATUS_BAR_SUBRANGE_SETUP_FAIL;
	/* [한국어] 상태를 반영한다. */
	reg->status = cpu_to_le32(status);
}

/* [한국어] pci_epf_test_bar_subrange_clear - 부분 매핑을 걷어 내고 원래대로 되돌린다
 * 
 * @epf_test: 이 EPF 인스턴스.
 * @reg: 공유 레지스터. ★ size 필드가 BAR 번호를 나른다. status 를 쓴다.
 * @return: 없음. 결과는 reg->status 로 전달된다.
 * 
 * setup 의 정확한 역이다. submap 을 NULL, num_submap 을 0 으로 되돌리고
 * set_bar 를 다시 부르면, 컨트롤러가 BAR 전체를 bar->phys_addr 하나로
 * 매핑하는 원래 방식으로 돌아간다.
 * 
 * ★ 순서에 유의: 서술자를 먼저 비우고 set_bar 를 부른 뒤, 성공했을
 * 때만 옛 배열을 kfree 한다. 반대로 하면 set_bar 가 실패했을 때
 * 되돌릴 배열이 이미 해제되어 있다.
 * 
 * ★ 되돌리기가 setup 보다 단순한 이유: 실패해도 하드웨어는 여전히
 * 부분 매핑 상태이므로, 서술자만 원래대로 복구하면 커널의 인식과
 * 하드웨어의 상태가 다시 일치한다. setup 처럼 set_bar 를 한 번 더
 * 부를 필요가 없다.
 * 
 * 실행 컨텍스트: cmd_handler 의 워크큐 문맥.
 * 
 * 호출 체인:
 *   pci_epf_test_cmd_handler() -> [pci_epf_test_bar_subrange_clear]
 *     -> pci_epc_set_bar() -> kfree() */
static void pci_epf_test_bar_subrange_clear(struct pci_epf_test *epf_test,
					    struct pci_epf_test_reg *reg)
{
	/* [한국어] EPF 객체. */
	struct pci_epf *epf = epf_test->epf;
	/* [한국어] 걷어 낼 조각 배열. */
	struct pci_epf_bar_submap *submap;
	/* [한국어] EP 컨트롤러. */
	struct pci_epc *epc = epf->epc;
	/* reg->size carries BAR number for BAR_SUBRANGE_* commands. */
	/* [한국어] 아래 영어 주석대로 size 필드를 BAR 번호로 해석한다. */
	enum pci_barno barno = le32_to_cpu(reg->size);
	/* [한국어] 현재 상태 비트. */
	u32 status = le32_to_cpu(reg->status);
	/* [한국어] 대상 BAR 서술자. */
	struct pci_epf_bar *bar;
	/* [한국어] 걷어 낼 조각 수. */
	unsigned int nsub;
	/* [한국어] 반환값. */
	int ret;

	/* [한국어] ★ 범위 검증 - setup 과 같은 이유로 필수다. */
	if (barno >= PCI_STD_NUM_BARS) {
		/* [한국어] 잘못된 BAR 번호를 남긴다. */
		dev_err(&epf->dev, "Invalid barno: %d\n", barno);
		/* [한국어] 실패 보고로. */
		goto err;
	}

	/* [한국어] 대상 BAR 서술자. */
	bar = &epf->bar[barno];
	/* [한국어] 현재 걸려 있는 조각 배열을 보관한다 - 실패 시 되돌리고
	 * 성공 시 반납할 대상이다. */
	submap = bar->submap;
	/* [한국어] 조각 수도 보관한다. */
	nsub = bar->num_submap;

	/* [한국어] 부분 매핑이 걸려 있지 않다 - setup 없이 clear 를 부른 경우다. */
	if (!submap || !nsub)
		/* [한국어] 걷어 낼 것이 없으므로 실패로 보고한다. */
		goto err;

	/* [한국어] 서술자에서 조각 배열을 뗀다. */
	bar->submap = NULL;
	/* [한국어] 조각 수도 0 으로 - 이 둘이 함께 비어야 '부분 매핑 없음' 이 된다. */
	bar->num_submap = 0;

	/* [한국어] 원래 방식(BAR 전체를 한 물리 주소로)으로 다시 건다. */
	ret = pci_epc_set_bar(epc, epf->func_no, epf->vfunc_no, bar);
	/* [한국어] 되돌리기 실패. */
	if (ret) {
		/* [한국어] 서술자를 원래대로 복구한다 - 하드웨어는 여전히 부분 매핑
		 * 상태이므로 커널의 인식을 그에 맞춘다. */
		bar->submap = submap;
		/* [한국어] 조각 수도 복구한다. */
		bar->num_submap = nsub;
		/* [한국어] 원인을 남긴다. */
		dev_err(&epf->dev, "pci_epc_set_bar() failed: %d\n", ret);
		/* [한국어] ★ 배열을 해제하지 않고 나간다 - 아직 하드웨어가 쓰고 있다. */
		goto err;
	}
	/* [한국어] 성공했으므로 이제 안전하게 반납한다. */
	kfree(submap);

	/* [한국어] 성공 비트. */
	status |= STATUS_BAR_SUBRANGE_CLEAR_SUCCESS;
	/* [한국어] 상태를 반영하고 */
	reg->status = cpu_to_le32(status);
	/* [한국어] 정상 종료한다. */
	return;

/* [한국어] 실패가 모이는 지점. */
err:
	/* [한국어] 실패 비트. */
	status |= STATUS_BAR_SUBRANGE_CLEAR_FAIL;
	/* [한국어] 상태를 반영한다. */
	reg->status = cpu_to_le32(status);
}

/* [한국어] pci_epf_test_cmd_handler - 명령 레지스터를 폴링해 분기하는 이 드라이버의 심장
 * 
 * @work: epf_test->cmd_handler.work. container_of 로 상태 구조체를 되찾는다.
 * @return: 없음
 * 
 * ★ 이 드라이버의 동작 모형: 호스트와 EP 는 서로의 인터럽트를 쓸 수
 * 없다(도어벨을 켜기 전까지는). 그래서 EP 는 공유 BAR 의 command
 * 레지스터를 1ms 마다 읽어 본다. 값이 0 이 아니면 호스트가 무언가를
 * 요청한 것이다. 처리를 마치면 자기 자신을 1ms 뒤로 다시 예약해,
 * 링크가 끊기거나 언바인드될 때까지 끝없이 되풀이된다.
 * 
 * ★ 명령을 읽자마자 0 으로 지우는 이유: 그것이 호스트에게 보내는
 * '접수했다' 는 신호다. 호스트는 command 가 0 으로 돌아오는 것을
 * 보고 EP 가 명령을 가져갔음을 안다. status 도 함께 0 으로 지워
 * 지난 명령의 결과가 새 명령의 결과로 오인되지 않게 한다.
 * 
 * ★ READ_ONCE/WRITE_ONCE 를 쓰는 이유: 이 메모리는 호스트가 언제든
 * 바꿀 수 있는데, 컴파일러는 그 사실을 모른다. 최적화로 읽기를
 * 없애거나 여러 번으로 쪼개면 프로토콜이 깨진다. 커널 락으로는
 * 다른 CPU 도 아닌 '다른 컴퓨터' 를 막을 수 없으므로, 컴파일러
 * 장벽과 프로토콜 규약이 유일한 보호 수단이다.
 * 
 * ★ 두 가지 사전 검증을 명령 분기 전에 한다:
 *   - DMA 를 요구했는데 지원하지 않으면 명령 자체를 버린다.
 *   - irq_type 이 0~2 를 벗어나면 버린다. raise_irq 가 default 로
 *     떨어지는 것을 막아, 호스트가 인터럽트를 영영 못 받는 대신
 *     아무 일도 일어나지 않게 한다.
 * 버릴 때 status 를 남기지 않으므로 호스트는 시간 초과로 알게 된다.
 * 
 * ★ 거의 모든 case 가 '처리 -> raise_irq' 짝으로 되어 있다. 예외는
 * RAISE_*_IRQ 세 개로, 인터럽트를 올리는 것 자체가 명령이다.
 * 
 * 실행 컨텍스트: kpcitest_workqueue 의 커널 스레드(프로세스 문맥).
 * 같은 지연 작업은 동시에 두 번 실행되지 않으므로, 명령 처리가
 * 겹치는 일이 없어 별도 락이 필요 없다.
 * 
 * 호출 체인:
 *   queue_delayed_work()(epc_init/link_up/자기 자신) -> [pci_epf_test_cmd_handler]
 *     -> pci_epf_test_read()/write()/copy()/enable_doorbell()/
 *        bar_subrange_setup() 등 -> pci_epf_test_raise_irq() */
static void pci_epf_test_cmd_handler(struct work_struct *work)
{
	/* [한국어] 읽어 온 명령 값. */
	u32 command;
	/* [한국어] 지연 작업 멤버에서 바깥 상태 구조체를 역산한다. delayed_work 는
	 * 안에 work_struct 를 품고 있으므로 .work 를 거쳐야 한다. */
	struct pci_epf_test *epf_test = container_of(work, struct pci_epf_test,
						     cmd_handler.work);
	/* [한국어] EPF 객체. */
	struct pci_epf *epf = epf_test->epf;
	/* [한국어] 로그 대상. */
	struct device *dev = &epf->dev;
	/* [한국어] 공유 레지스터가 든 BAR 번호. */
	enum pci_barno test_reg_bar = epf_test->test_reg_bar;
	/* [한국어] 그 BAR 의 커널 가상 주소를 레지스터 구조체로 본다. 호스트가
	 * 쓴 값이 이 주소를 통해 보인다. */
	struct pci_epf_test_reg *reg = epf_test->reg[test_reg_bar];
	/* [한국어] 인터럽트 종류를 미리 읽어 둔다. 아래 검증에 쓰인다. */
	u32 irq_type = le32_to_cpu(reg->irq_type);

	/* [한국어] ★ 명령을 읽는다. READ_ONCE 로 컴파일러가 이 읽기를 없애거나
	 * 쪼개지 못하게 막는다 - 호스트가 언제든 바꾸는 메모리이기 때문이다. */
	command = le32_to_cpu(READ_ONCE(reg->command));
	/* [한국어] 0 이면 호스트가 아직 아무것도 요청하지 않았다. */
	if (!command)
		/* [한국어] 아무 일도 하지 않고 다음 폴링을 예약하러 간다. */
		goto reset_handler;

	/* [한국어] ★ 명령을 접수했음을 호스트에게 알린다. 호스트는 이 값이 0 으로
	 * 돌아오는 것을 보고 EP 가 명령을 가져갔음을 안다. WRITE_ONCE 로
	 * 컴파일러가 이 쓰기를 뒤로 미루지 못하게 막는다. */
	WRITE_ONCE(reg->command, 0);
	/* [한국어] 지난 명령의 결과를 지운다. 이렇게 해야 호스트가 새 인터럽트를
	 * 받고 status 를 읽었을 때 옛 결과를 새 결과로 오인하지 않는다. */
	WRITE_ONCE(reg->status, 0);

	/* [한국어] DMA 를 쓰라는 요청인데 */
	if ((le32_to_cpu(READ_ONCE(reg->flags)) & FLAG_USE_DMA) &&
	    /* [한국어] DMA 채널을 확보하지 못한 구성이면 */
	    !epf_test->dma_supported) {
		/* [한국어] 원인을 남기고 */
		dev_err(dev, "Cannot transfer data using DMA\n");
		/* [한국어] ★ 명령을 버린다. status 를 남기지 않으므로 호스트는 인터럽트를
		 * 기다리다 시간 초과로 실패를 알게 된다. */
		goto reset_handler;
	}

	/* [한국어] ★ 인터럽트 종류 범위 검증. IRQ_TYPE_* 가 0/1/2 로 연속이라
	 * 이 한 번의 비교로 충분하다. u32 이므로 음수는 없다.
	 * 이 검사가 raise_irq 의 default 분기를 사실상 도달 불가로 만든다. */
	if (irq_type > IRQ_TYPE_MSIX) {
		/* [한국어] 원인을 남기고 */
		dev_err(dev, "Failed to detect IRQ type\n");
		/* [한국어] 명령을 버린다. */
		goto reset_handler;
	}

	/* [한국어] 명령 값으로 분기한다. 비트 정의지만 값 전체를 비교하므로
	 * 두 명령을 동시에 요청할 수는 없다. */
	switch (command) {
	/* [한국어] INTx 를 올려 달라. */
	case COMMAND_RAISE_INTX_IRQ:
	/* [한국어] MSI 를 올려 달라. */
	case COMMAND_RAISE_MSI_IRQ:
	/* [한국어] MSI-X 를 올려 달라. 세 명령은 처리가 같다 - 어떤 종류로 올릴지는
	 * command 가 아니라 irq_type 레지스터가 정하기 때문이다.
	 * 세 명령을 따로 둔 것은 호스트 쪽 시험 항목을 구분하기 위해서로 보인다. */
	case COMMAND_RAISE_MSIX_IRQ:
		/* [한국어] 인터럽트를 올리는 것 자체가 명령이다 - 다른 case 와 달리
		 * 앞에 처리 함수가 없다. */
		pci_epf_test_raise_irq(epf_test, reg);
		break;
	/* [한국어] 호스트 메모리에 쓰라는 명령. */
	case COMMAND_WRITE:
		/* [한국어] 난수를 만들어 보내고 CRC 를 남긴다. */
		pci_epf_test_write(epf_test, reg);
		/* [한국어] 결과를 알린다. */
		pci_epf_test_raise_irq(epf_test, reg);
		break;
	/* [한국어] 호스트 메모리에서 읽으라는 명령. */
	case COMMAND_READ:
		/* [한국어] 읽어서 CRC 를 검증한다. */
		pci_epf_test_read(epf_test, reg);
		/* [한국어] 결과를 알린다. */
		pci_epf_test_raise_irq(epf_test, reg);
		break;
	/* [한국어] 호스트 메모리 안에서 복사하라는 명령. */
	case COMMAND_COPY:
		/* [한국어] 두 창을 잡아 옮긴다. */
		pci_epf_test_copy(epf_test, reg);
		/* [한국어] 결과를 알린다. */
		pci_epf_test_raise_irq(epf_test, reg);
		break;
	/* [한국어] 도어벨을 켜라는 명령. */
	case COMMAND_ENABLE_DOORBELL:
		/* [한국어] BAR 하나의 inbound 변환을 MSI 컨트롤러로 돌려놓는다. */
		pci_epf_test_enable_doorbell(epf_test, reg);
		/* [한국어] 결과를 알린다 - 호스트는 이 인터럽트를 받고 doorbell_bar/
		 * offset/data 를 읽어 어디에 무엇을 쓸지 알아낸다. */
		pci_epf_test_raise_irq(epf_test, reg);
		break;
	/* [한국어] 도어벨을 끄라는 명령. */
	case COMMAND_DISABLE_DOORBELL:
		/* [한국어] IRQ 를 풀고 BAR 를 원래대로 되돌린다. */
		pci_epf_test_disable_doorbell(epf_test, reg);
		/* [한국어] 결과를 알린다. */
		pci_epf_test_raise_irq(epf_test, reg);
		break;
	/* [한국어] BAR 부분 매핑을 걸라는 명령. ★ 이때 reg->size 는 크기가 아니라
	 * BAR 번호를 나른다. */
	case COMMAND_BAR_SUBRANGE_SETUP:
		/* [한국어] BAR 를 두 조각으로 나눠 순서를 뒤바꿔 매핑하고 서명을 채운다. */
		pci_epf_test_bar_subrange_setup(epf_test, reg);
		/* [한국어] 결과를 알린다. */
		pci_epf_test_raise_irq(epf_test, reg);
		break;
	/* [한국어] 부분 매핑을 걷어 내라는 명령. */
	case COMMAND_BAR_SUBRANGE_CLEAR:
		/* [한국어] 원래의 통짜 매핑으로 되돌린다. */
		pci_epf_test_bar_subrange_clear(epf_test, reg);
		/* [한국어] 결과를 알린다. */
		pci_epf_test_raise_irq(epf_test, reg);
		break;
	/* [한국어] 정의되지 않은 명령 값. */
	default:
		/* [한국어] 값을 남긴다. 인터럽트를 올리지 않으므로 호스트는 시간 초과로
		 * 알게 된다. */
		dev_err(dev, "Invalid command 0x%x\n", command);
		break;
	}

/* [한국어] 모든 경로가 모이는 재예약 지점 - 명령을 처리했든, 버렸든,
 * 아예 없었든 여기로 온다. */
reset_handler:
	/* [한국어] 1ms 뒤에 자기 자신을 다시 예약한다. ★ 이 재예약이 폴링 순환을
	 * 이어 가는 유일한 동력이며, epc_deinit/link_down/unbind 의
	 * cancel_delayed_work_sync 가 그 고리를 끊는다. */
	queue_delayed_work(kpcitest_workqueue, &epf_test->cmd_handler,
			   msecs_to_jiffies(1));
}

/* [한국어] pci_epf_test_set_bar - 할당해 둔 BAR 들을 실제로 호스트에게 노출한다
 * 
 * @epf: 대상 EPF.
 * @return: 0 성공. test_reg_bar 를 노출하지 못했을 때만 음수 오류.
 * 
 * ★ 실패를 다루는 방식이 특이하다. BAR 하나를 노출하지 못해도 곧바로
 * 포기하지 않고, 그 BAR 만 정리한 뒤 나머지를 계속 시도한다. 시험
 * 드라이버라 BAR 가 몇 개 빠져도 나머지 시험은 할 수 있기 때문이다.
 * 단 하나, 공유 레지스터가 든 test_reg_bar 만은 예외다 - 그것이 없으면
 * 호스트와 대화할 수단 자체가 없으므로 오류로 끝낸다.
 * 
 * 실패한 BAR 의 메모리를 곧바로 반납하고 reg[bar] 를 NULL 로 만드는
 * 이유: 이후 모든 코드가 'reg[bar] 가 NULL 이면 그 BAR 는 없다' 는
 * 규약을 따르므로(clear_bar, free_space 의 루프), 그 규약을 여기서
 * 바로 맞춰 두어야 한다.
 * 
 * 실행 컨텍스트: epc_init 의 프로세스 문맥. 링크가 준비된 뒤 불린다.
 * 
 * 호출 체인:
 *   pci_epf_test_epc_init() -> [pci_epf_test_set_bar] -> pci_epc_set_bar() */
static int pci_epf_test_set_bar(struct pci_epf *epf)
{
	/* [한국어] BAR 인덱스와 반환값. */
	int bar, ret;
	/* [한국어] EP 컨트롤러. */
	struct pci_epc *epc = epf->epc;
	/* [한국어] 로그 대상. */
	struct device *dev = &epf->dev;
	/* [한국어] 이 EPF 의 상태 구조체를 되찾는다. */
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);
	/* [한국어] 공유 레지스터가 든 BAR - 아래에서 특별 취급한다. */
	enum pci_barno test_reg_bar = epf_test->test_reg_bar;

	/* [한국어] 표준 BAR 여섯 개를 모두 훑는다. */
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		/* [한국어] 메모리를 할당하지 못한 BAR 는 노출할 것이 없다. */
		if (!epf_test->reg[bar])
			/* [한국어] 건너뛴다. */
			continue;

		/* [한국어] BAR 를 실제로 호스트에게 노출한다. epf->bar[bar] 에는
		 * alloc_space 가 채워 둔 물리 주소, 크기, 플래그가 들어 있다. */
		ret = pci_epc_set_bar(epc, epf->func_no, epf->vfunc_no,
				      &epf->bar[bar]);
		/* [한국어] 노출 실패 - 컨트롤러가 그 크기나 배치를 다룰 수 없는 경우다. */
		if (ret) {
			/* [한국어] 쓸 수 없게 된 BAR 의 메모리를 반납한다. PRIMARY_INTERFACE 는
			 * '주 EPC 쪽 BAR' 라는 뜻으로, 보조 EPC(sec_epc)와 구별하는 인자다. */
			pci_epf_free_space(epf, epf_test->reg[bar], bar,
					   PRIMARY_INTERFACE);
			/* [한국어] ★ NULL 로 만들어 '이 BAR 는 없다' 는 규약을 지킨다.
			 * 이 줄이 없으면 나중에 clear_bar 나 free_space 가 이미 해제된
			 * 메모리를 다시 만진다. */
			epf_test->reg[bar] = NULL;
			/* [한국어] 어느 BAR 가 실패했는지 남긴다. */
			dev_err(dev, "Failed to set BAR%d\n", bar);
			/* [한국어] ★ 공유 레지스터 BAR 였다면 치명적이다 - 호스트와 대화할
			 * 수단이 사라졌으므로 더 진행할 수 없다. */
			if (bar == test_reg_bar)
				/* [한국어] 오류를 그대로 올려보낸다. epc_init 이 이 값을 받고 중단한다. */
				return ret;
		}
	}

	/* [한국어] 나머지 BAR 는 몇 개가 실패했든 성공으로 본다. */
	return 0;
}

/* [한국어] pci_epf_test_clear_bar - 노출했던 BAR 들을 거둬들인다
 * 
 * @epf: 대상 EPF.
 * @return: 없음. clear_bar 의 실패는 무시한다 - 되돌릴 방법이 없고,
 *          이 함수는 이미 정리 경로에서만 불리기 때문이다.
 * 
 * set_bar 의 역이다. 메모리는 반납하지 않고 노출만 거둔다 -
 * 메모리 반납은 free_space 가 따로 하며, 링크가 끊겼다 다시 붙으면
 * 같은 메모리를 다시 노출할 수 있어야 하기 때문이다.
 * 
 * ★ reg[bar] 가 NULL 인 BAR 를 건너뛰는 것이 중요하다. set_bar 가
 * 실패해 이미 정리한 BAR 를 다시 거두면 컨트롤러가 걸지도 않은
 * 매핑을 지우려 든다.
 * 
 * 실행 컨텍스트: epc_deinit(링크 끊김) 또는 unbind 의 프로세스 문맥.
 * 두 번 불릴 수 있으나, 두 번째 호출은 하드웨어 관점에서 무해하다.
 * 
 * 호출 체인:
 *   pci_epf_test_epc_deinit() / pci_epf_test_unbind()
 *     -> [pci_epf_test_clear_bar] -> pci_epc_clear_bar() */
static void pci_epf_test_clear_bar(struct pci_epf *epf)
{
	/* [한국어] 이 EPF 의 상태 구조체. */
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);
	/* [한국어] EP 컨트롤러. */
	struct pci_epc *epc = epf->epc;
	/* [한국어] BAR 인덱스. */
	int bar;

	/* [한국어] 표준 BAR 여섯 개를 훑는다. */
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		/* [한국어] 노출한 적이 없는 BAR 는 */
		if (!epf_test->reg[bar])
			/* [한국어] 건너뛴다. */
			continue;

		/* [한국어] BAR 노출을 거둔다. 호스트 쪽에서는 그 BAR 가 응답하지 않게 된다. */
		pci_epc_clear_bar(epc, epf->func_no, epf->vfunc_no,
				  &epf->bar[bar]);
	}
}

/* [한국어] pci_epf_test_set_capabilities - EP 가 할 수 있는 일을 호스트에게 알린다
 * 
 * @epf: 대상 EPF.
 * @return: 없음. 결과는 공유 레지스터의 caps 필드에 남는다.
 * 
 * ★ 왜 필요한가: 호스트 쪽 시험 프로그램은 어떤 EP 컨트롤러에 붙을지
 * 모른다. 어떤 컨트롤러는 MSI-X 가 없고, 어떤 것은 BAR0 를 하드웨어가
 * 선점하고 있으며, 부분 매핑을 못 하는 것도 있다. 지원하지 않는 기능을
 * 시험하면 그냥 실패로 보고될 뿐 유용한 정보가 아니다. 그래서 EP 가
 * 자기 능력을 비트맵으로 미리 알려 주고, 호스트는 그에 맞춰 시험
 * 항목을 고른다.
 * 
 * ★ 두 가지 정보원을 함께 쓴다:
 *   - epc->ops->align_addr : 연산 함수 포인터가 있는지로 정렬 없는
 *     접근 지원 여부를 판정한다. epc_features 에는 이 항목이 없다.
 *   - epc_features         : 나머지는 모두 EPC 드라이버가 선언한
 *     특성 구조체에서 읽는다.
 * 
 * ★ CAP_SUBRANGE_MAPPING 의 조건이 둘인 이유: 헤더에 명시된 대로
 * subrange_mapping 은 dynamic_inbound_mapping 에 의존하는 기능이다.
 * EPC 드라이버가 실수로 후자 없이 전자만 선언했더라도, 여기서
 * 두 조건을 함께 확인해 잘못된 능력을 광고하지 않는다.
 * 
 * 실행 컨텍스트: epc_init 의 프로세스 문맥. 링크가 올라오기 전에
 * 한 번만 불리므로 경쟁이 없다.
 * 
 * 호출 체인:
 *   pci_epf_test_epc_init() -> [pci_epf_test_set_capabilities] */
static void pci_epf_test_set_capabilities(struct pci_epf *epf)
{
	/* [한국어] 이 EPF 의 상태 구조체. */
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);
	/* [한국어] 공유 레지스터가 든 BAR. */
	enum pci_barno test_reg_bar = epf_test->test_reg_bar;
	/* [한국어] 그 BAR 의 커널 가상 주소를 레지스터 구조체로 본다. */
	struct pci_epf_test_reg *reg = epf_test->reg[test_reg_bar];
	/* [한국어] EP 컨트롤러 - align_addr 연산 유무를 보려면 features 가 아니라
	 * 컨트롤러 자체를 봐야 한다. */
	struct pci_epc *epc = epf->epc;
	/* [한국어] 모아 갈 능력 비트맵. 0 에서 시작해 OR 로 쌓는다. */
	u32 caps = 0;

	/* [한국어] 컨트롤러가 주소 정렬 연산을 제공하면, 정렬되지 않은 호스트
	 * 주소도 매핑할 수 있다는 뜻이다. */
	if (epc->ops->align_addr)
		/* [한국어] 정렬 없는 접근 가능. */
		caps |= CAP_UNALIGNED_ACCESS;

	/* [한국어] MSI 능력이 선언되어 있으면 */
	if (epf_test->epc_features->msi_capable)
		/* [한국어] MSI 가능. */
		caps |= CAP_MSI;

	/* [한국어] MSI-X 능력이 선언되어 있으면 */
	if (epf_test->epc_features->msix_capable)
		/* [한국어] MSI-X 가능. */
		caps |= CAP_MSIX;

	/* [한국어] INTx 능력이 선언되어 있으면 */
	if (epf_test->epc_features->intx_capable)
		/* [한국어] INTx 가능. */
		caps |= CAP_INTX;

	/* [한국어] 이미 설정된 BAR 를 clear 없이 다시 걸 수 있으면 */
	if (epf_test->epc_features->dynamic_inbound_mapping)
		/* [한국어] 동적 inbound 매핑 가능 - 도어벨 기능이 이것에 기댄다. */
		caps |= CAP_DYNAMIC_INBOUND_MAPPING;

	/* [한국어] ★ 부분 매핑은 동적 매핑에 의존하므로 */
	if (epf_test->epc_features->dynamic_inbound_mapping &&
	    /* [한국어] 두 조건을 함께 확인한다. */
	    epf_test->epc_features->subrange_mapping)
		/* [한국어] 부분 매핑 가능. */
		caps |= CAP_SUBRANGE_MAPPING;

	/* [한국어] BAR0 가 하드웨어 전용으로 예약되어 있으면 */
	if (epf_test->epc_features->bar[BAR_0].type == BAR_RESERVED)
		/* [한국어] 호스트에게 그 BAR 를 시험에 쓰지 말라고 알린다. */
		caps |= CAP_BAR0_RESERVED;

	/* [한국어] BAR1 이 예약되어 있으면 */
	if (epf_test->epc_features->bar[BAR_1].type == BAR_RESERVED)
		/* [한국어] 알린다. */
		caps |= CAP_BAR1_RESERVED;

	/* [한국어] BAR2 가 예약되어 있으면 */
	if (epf_test->epc_features->bar[BAR_2].type == BAR_RESERVED)
		/* [한국어] 알린다. */
		caps |= CAP_BAR2_RESERVED;

	/* [한국어] BAR3 이 예약되어 있으면 */
	if (epf_test->epc_features->bar[BAR_3].type == BAR_RESERVED)
		/* [한국어] 알린다. */
		caps |= CAP_BAR3_RESERVED;

	/* [한국어] BAR4 가 예약되어 있으면 */
	if (epf_test->epc_features->bar[BAR_4].type == BAR_RESERVED)
		/* [한국어] 알린다. */
		caps |= CAP_BAR4_RESERVED;

	/* [한국어] BAR5 가 예약되어 있으면 */
	if (epf_test->epc_features->bar[BAR_5].type == BAR_RESERVED)
		/* [한국어] 알린다. */
		caps |= CAP_BAR5_RESERVED;

	/* [한국어] 모아 둔 비트맵을 공유 레지스터에 쓴다. 링크가 올라오기 전이라
	 * 호스트는 링크 후에 이 값을 읽게 된다. */
	reg->caps = cpu_to_le32(caps);
}

/* [한국어] pci_epf_test_epc_init - 링크가 준비되면 함수를 실제로 구성한다
 * 
 * @epf: 대상 EPF.
 * @return: 0 성공, 음수 errno 실패.
 * 
 * ★ bind 와 epc_init 이 나뉘어 있는 이유: bind 는 EPF 가 EPC 에
 * 연결되는 순간(configfs 조립 시점) 불리고, 그때는 아직 컨트롤러가
 * 설정 헤더를 쓸 준비가 되지 않았을 수 있다. epc_init 은 컨트롤러가
 * '이제 구성해도 된다' 고 알려 줄 때 불린다. 그래서 메모리 할당 같은
 * 하드웨어와 무관한 일은 bind 에서, 하드웨어에 실제로 쓰는 일은
 * 여기서 한다. 링크가 끊겼다 붙으면 epc_deinit -> epc_init 이 다시
 * 불릴 수 있으므로, 이 함수는 여러 번 실행돼도 괜찮아야 한다.
 * 
 * 동작 단계:
 *   1) DMA 채널을 잡는다 - 실패해도 치명적이지 않아 dma_supported 만 내린다
 *   2) 설정 헤더를 쓴다(VF 두 번째부터는 건너뛴다)
 *   3) 능력 비트를 공유 레지스터에 채운다
 *   4) BAR 들을 노출한다
 *   5) MSI/MSI-X 벡터 수를 설정한다
 *   6) 링크 업 통지가 없는 컨트롤러라면 폴링을 지금 시작한다
 * 
 * ★ 마지막 단계의 조건이 미묘하다. 컨트롤러가 링크 업을 알려 줄 수
 * 있으면(linkup_notifier) link_up 콜백이 폴링을 시작하므로 여기서는
 * 하지 않는다. 알려 줄 수 없으면 링크가 언제 올라오는지 알 방법이
 * 없으므로 지금 바로 시작한다. 두 곳에서 모두 시작하면 지연 작업이
 * 중복 예약되는데, 워크큐가 그것을 무시하므로 치명적이지는 않으나
 * 의도를 분명히 하려고 갈라 두었다.
 * 
 * 실행 컨텍스트: EPC 코어가 부르는 프로세스 문맥.
 * 
 * 호출 체인:
 *   (EPC 코어) pci_epc_event_ops.epc_init -> [pci_epf_test_epc_init]
 *     -> pci_epf_test_init_dma_chan() -> pci_epc_write_header()
 *     -> pci_epf_test_set_capabilities() -> pci_epf_test_set_bar() */
static int pci_epf_test_epc_init(struct pci_epf *epf)
{
	/* [한국어] 이 EPF 의 상태 구조체. */
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);
	/* [한국어] probe 에서 매달아 둔 설정 헤더(test_header). */
	struct pci_epf_header *header = epf->header;
	/* [한국어] bind 에서 조회해 둔 컨트롤러 특성. */
	const struct pci_epc_features *epc_features = epf_test->epc_features;
	/* [한국어] EP 컨트롤러. */
	struct pci_epc *epc = epf->epc;
	/* [한국어] 로그 대상. */
	struct device *dev = &epf->dev;
	/* [한국어] 링크 업 통지 지원 여부를 담을 변수. false 로 초기화하지만
	 * 아래에서 곧 특성 값으로 덮어쓴다. */
	bool linkup_notifier = false;
	/* [한국어] 반환값. */
	int ret;

	/* [한국어] 일단 DMA 를 쓸 수 있다고 가정한다 - 아래 초기화가 실패하면 내린다. */
	epf_test->dma_supported = true;

	/* [한국어] 1) DMA 채널을 잡아 본다. */
	ret = pci_epf_test_init_dma_chan(epf_test);
	/* [한국어] 실패해도(-EPROBE_DEFER 포함) 오류로 끝내지 않는다. */
	if (ret)
		/* [한국어] ★ DMA 없이도 CPU 복사로 모든 시험을 할 수 있으므로, 능력만
		 * 내리고 계속 진행한다. cmd_handler 가 이 값을 보고 DMA 요청을 거절한다. */
		epf_test->dma_supported = false;

	/* [한국어] 2) ★ VF(가상 함수) 번호가 0 이나 1 일 때만 헤더를 쓴다.
	 * 0 은 물리 함수 자신이고, 그 뒤로는 여러 VF 가 하나의 설정 헤더를
	 * 공유하는 SR-IOV 구조라 두 번째 VF 부터는 쓸 필요가 없다.
	 * (이 조건이 '<= 1' 인 정확한 근거는 이 트리 안에서 확인하지 못했다.) */
	if (epf->vfunc_no <= 1) {
		/* [한국어] 벤더 ID, 클래스 코드 등을 하드웨어 설정 공간에 쓴다.
		 * 이것이 있어야 호스트가 열거할 때 이 함수를 PCI 장치로 인식한다. */
		ret = pci_epc_write_header(epc, epf->func_no, epf->vfunc_no, header);
		/* [한국어] 헤더 쓰기 실패 - 컨트롤러가 아직 준비되지 않은 경우다. */
		if (ret) {
			/* [한국어] 원인을 남긴다. */
			dev_err(dev, "Configuration header write failed\n");
			/* [한국어] 더 진행할 수 없다. */
			return ret;
		}
	}

	/* [한국어] 3) 능력 비트를 공유 레지스터에 채운다. BAR 를 노출하기 전에
	 * 채워 두므로, 호스트가 BAR 를 읽을 수 있게 되는 순간 이미 유효하다. */
	pci_epf_test_set_capabilities(epf);

	/* [한국어] 4) 할당해 둔 BAR 들을 노출한다. */
	ret = pci_epf_test_set_bar(epf);
	/* [한국어] 공유 레지스터 BAR 를 노출하지 못했다면 */
	if (ret)
		/* [한국어] 호스트와 대화할 수 없으므로 중단한다. */
		return ret;

	/* [한국어] 5) MSI 를 지원하는 컨트롤러라면 */
	if (epc_features->msi_capable) {
		/* [한국어] 요청할 벡터 수를 설정한다. epf->msi_interrupts 는 configfs 로
		 * 사용자가 정한 값이며, 하드웨어가 그보다 적게 줄 수 있다 -
		 * raise_irq 가 pci_epc_get_msi 로 실제 개수를 다시 묻는 이유다. */
		ret = pci_epc_set_msi(epc, epf->func_no, epf->vfunc_no,
				      epf->msi_interrupts);
		/* [한국어] 설정 실패. */
		if (ret) {
			/* [한국어] 원인을 남긴다. */
			dev_err(dev, "MSI configuration failed\n");
			/* [한국어] 중단한다. */
			return ret;
		}
	}

	/* [한국어] MSI-X 를 지원하는 컨트롤러라면 */
	if (epc_features->msix_capable) {
		/* [한국어] 벡터 수와 함께 'MSI-X 표가 어느 BAR 의 어느 오프셋에 있는지' 를
		 * 알려 준다 - MSI-X 는 벡터마다 주소/데이터를 표에 담아 두므로
		 * 그 표의 위치를 하드웨어가 알아야 한다. */
		ret = pci_epc_set_msix(epc, epf->func_no, epf->vfunc_no,
				       epf->msix_interrupts,
				       /* [한국어] 표가 놓인 BAR 는 공유 레지스터와 같은 BAR 다. */
				       epf_test->test_reg_bar,
				       /* [한국어] 그 BAR 안에서 레지스터 구조체 바로 뒤가 표의 시작이다 -
				        * alloc_space 가 계산해 둔 값이다. */
				       epf_test->msix_table_offset);
		/* [한국어] 설정 실패. */
		if (ret) {
			/* [한국어] 원인을 남긴다. */
			dev_err(dev, "MSI-X configuration failed\n");
			/* [한국어] 중단한다. */
			return ret;
		}
	}

	/* [한국어] 6) 컨트롤러가 링크 업을 알려 줄 수 있는지 확인한다. */
	linkup_notifier = epc_features->linkup_notifier;
	/* [한국어] 알려 줄 수 없다면 링크가 언제 올라오는지 알 방법이 없으므로 */
	if (!linkup_notifier)
		/* [한국어] ★ 지금 바로 폴링을 시작한다. queue_work(지연 없음)을 쓰되
		 * 지연 작업의 .work 멤버를 직접 넘기는데, 이는 '첫 폴링은 즉시' 라는
		 * 뜻이다. 알려 줄 수 있는 컨트롤러라면 link_up 콜백이 대신 시작한다. */
		queue_work(kpcitest_workqueue, &epf_test->cmd_handler.work);

	/* [한국어] 구성 완료. */
	return 0;
}

/* [한국어] pci_epf_test_epc_deinit - 링크가 끊기거나 컨트롤러가 물러날 때 정리한다
 * 
 * @epf: 대상 EPF.
 * @return: 없음
 * 
 * epc_init 의 역이되 메모리 할당은 되돌리지 않는다 - 그것은 bind 가
 * 잡았으므로 unbind 가 반납한다. 여기서는 '하드웨어와 연결된 것' 만
 * 끊는다.
 * 
 * ★ 순서가 중요하다:
 *   1) 폴링을 먼저 멈춘다. _sync 판이라 실행 중인 핸들러가 끝나기를
 *      기다리므로, 이 줄이 지난 뒤에는 아무도 BAR 나 DMA 를 만지지 않는다.
 *   2) 그제서야 DMA 채널을 반납한다.
 *   3) 마지막으로 BAR 노출을 거둔다.
 * 순서를 뒤집으면 아직 돌고 있는 cmd_handler 가 이미 반납된 채널이나
 * 거둬들인 BAR 를 건드린다.
 * 
 * ★ 이 함수는 epc_init 과 짝을 이루어 여러 번 반복될 수 있다 -
 * 링크가 붙었다 끊길 때마다. 그래서 clean_dma_chan 과 clear_bar 가
 * 모두 '두 번 불려도 안전' 하게 쓰여 있다.
 * 
 * 실행 컨텍스트: EPC 코어가 부르는 프로세스 문맥. _sync 대기가 있으므로
 * 잠들 수 있어야 한다.
 * 
 * 호출 체인:
 *   (EPC 코어) pci_epc_event_ops.epc_deinit -> [pci_epf_test_epc_deinit] */
static void pci_epf_test_epc_deinit(struct pci_epf *epf)
{
	/* [한국어] 이 EPF 의 상태 구조체. */
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);

	/* [한국어] 1) 폴링을 멈추고 실행 중인 핸들러가 끝나기를 기다린다. */
	cancel_delayed_work_sync(&epf_test->cmd_handler);
	/* [한국어] 2) DMA 채널을 반납한다. */
	pci_epf_test_clean_dma_chan(epf_test);
	/* [한국어] 3) BAR 노출을 거둔다. */
	pci_epf_test_clear_bar(epf);
}

/* [한국어] pci_epf_test_link_up - 호스트와 링크가 올라왔을 때 폴링을 시작한다
 * 
 * @epf: 대상 EPF.
 * @return: 항상 0. EPC 코어가 int 를 요구하지만 실패할 일이 없다.
 * 
 * 링크가 올라오기 전에는 호스트가 BAR 에 쓸 수 없으므로 명령
 * 레지스터를 폴링할 이유가 없다. 이 콜백이 있는 컨트롤러에서는
 * epc_init 이 폴링을 시작하지 않고 여기에 맡긴다.
 * 
 * 1ms 지연을 두는 이유: 링크가 막 올라온 직후에는 호스트가 아직
 * BAR 주소를 배정하지 않았을 수 있어, 곧바로 읽으면 의미 없는 값을
 * 본다. 어차피 폴링 주기가 1ms 이므로 첫 회만 같은 간격을 둔 것이다.
 * 
 * 실행 컨텍스트: EPC 코어가 부르는 문맥. 예약만 하므로 가볍다.
 * 
 * 호출 체인:
 *   (EPC 코어) pci_epc_event_ops.link_up -> [pci_epf_test_link_up]
 *     -> queue_delayed_work() */
static int pci_epf_test_link_up(struct pci_epf *epf)
{
	/* [한국어] 이 EPF 의 상태 구조체. */
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);

	/* [한국어] 1ms 뒤 첫 폴링을 예약한다. 이미 예약되어 있으면 워크큐가
	 * 중복을 무시하므로, epc_init 이 이미 시작한 경우에도 안전하다. */
	queue_delayed_work(kpcitest_workqueue, &epf_test->cmd_handler,
			   msecs_to_jiffies(1));

	/* [한국어] 성공. */
	return 0;
}

/* [한국어] pci_epf_test_link_down - 링크가 끊겼을 때 폴링을 멈춘다
 * 
 * @epf: 대상 EPF.
 * @return: 항상 0.
 * 
 * 링크가 없으면 호스트가 명령을 쓸 수 없으므로 폴링은 CPU 낭비일
 * 뿐이다. 게다가 링크가 끊긴 상태에서 호스트 메모리 매핑을 시도하면
 * 컨트롤러가 오류를 낼 수 있다.
 * 
 * ★ _sync 판을 쓰는 이유: 이 함수가 돌아간 뒤에는 cmd_handler 가
 * 확실히 멈춰 있어야 한다. 비동기 판은 '이미 실행 중인 것' 을
 * 그대로 두므로, 링크가 없는 상태에서 전송을 시도하는 핸들러가
 * 계속 돌 수 있다.
 * 
 * 링크가 다시 올라오면 link_up 이 폴링을 재개한다 - 이 짝이
 * 링크 상태에 따라 폴링을 켜고 끄는 순환을 이룬다.
 * 
 * 실행 컨텍스트: EPC 코어가 부르는 프로세스 문맥. _sync 대기가
 * 있으므로 잠들 수 있어야 한다.
 * 
 * 호출 체인:
 *   (EPC 코어) pci_epc_event_ops.link_down -> [pci_epf_test_link_down]
 *     -> cancel_delayed_work_sync() */
static int pci_epf_test_link_down(struct pci_epf *epf)
{
	/* [한국어] 이 EPF 의 상태 구조체. */
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);

	/* [한국어] 폴링을 멈추고 실행 중인 핸들러가 끝나기를 기다린다. */
	cancel_delayed_work_sync(&epf_test->cmd_handler);

	/* [한국어] 성공. */
	return 0;
}

/* [한국어] pci_epf_test_event_ops - EPC 코어가 알려 주는 사건에 대한 콜백 묶음.
 * probe 가 epf->event_ops 에 이 표를 매달아 두면, 컨트롤러 쪽에서
 * 일어나는 네 가지 사건이 이 함수들로 전달된다. */
static const struct pci_epc_event_ops pci_epf_test_event_ops = {
	/* [한국어] 컨트롤러가 '이제 함수를 구성해도 된다' 고 알릴 때. */
	.epc_init = pci_epf_test_epc_init,
	/* [한국어] 그 반대 - 구성을 거둬들여야 할 때. */
	.epc_deinit = pci_epf_test_epc_deinit,
	/* [한국어] 호스트와 링크가 올라왔을 때. linkup_notifier 를 선언한
	 * 컨트롤러만 이 콜백을 부른다. */
	.link_up = pci_epf_test_link_up,
	/* [한국어] 링크가 끊겼을 때. */
	.link_down = pci_epf_test_link_down,
};

/* [한국어] pci_epf_test_alloc_space - BAR 뒤에 놓일 메모리를 모두 잡는다
 * 
 * @epf: 대상 EPF.
 * @return: 0 성공, -ENOMEM 공유 레지스터 BAR 의 메모리를 못 잡은 경우.
 * 
 * ★ 이웃 파일에서 확인한 사실: pci_epf_alloc_space() 는 내부에서
 * dma_alloc_coherent 로 메모리를 잡고, 하드웨어 제약에 맞게 크기를
 * 보정한다 - 최소 128 바이트, Resizable BAR 는 1MB, 고정 크기 BAR 는
 * 그 값, 나머지는 2의 거듭제곱으로 올림. 그래서 여기서 요청한 크기와
 * 실제 BAR 크기가 다를 수 있으며, 결과는 epf->bar[bar] 에 채워진다.
 * 
 * ★ 공유 레지스터 BAR 의 크기 계산이 이 함수의 핵심이다. 세 부분이
 * 한 BAR 안에 이어 놓인다:
 *   [pci_epf_test_reg (128 정렬)][MSI-X 표][MSI-X PBA]
 *   - 레지스터 구조체를 128 바이트로 올림 정렬하는 이유는 MSI-X 표가
 *     그 뒤에 오는데 표의 정렬 요구를 넉넉히 만족시키기 위해서다.
 *   - MSI-X 표는 벡터당 16 바이트(PCI_MSIX_ENTRY_SIZE).
 *   - PBA(Pending Bit Array)는 벡터당 1 비트라 8 로 나눠 올림하고,
 *     아래 영어 주석대로 다시 8 바이트(QWORD)로 정렬한다.
 * MSI-X 를 지원하지 않는 컨트롤러라면 뒤 두 부분이 0 이라 레지스터
 * 구조체만 남는다.
 * 
 * ★ 나머지 BAR 는 실패해도 그냥 넘어간다. 시험 드라이버라 BAR 가
 * 몇 개 없어도 나머지 시험은 할 수 있기 때문이며, 실패한 자리는
 * reg[bar] 가 NULL 로 남아 이후 모든 루프가 건너뛴다.
 * 
 * ★ 루프 변수를 루프 안에서 덮어쓰는 관용구에 주의: bar 를 증가시킨
 * 뒤 곧바로 get_next_free_bar 로 '그 이후의 첫 빈 BAR' 를 다시 구한다.
 * 64 비트 BAR 는 두 자리를 차지하므로 단순 증가로는 건너뛸 자리를
 * 놓치기 때문이다. NO_BAR(-1)가 오면 더 쓸 BAR 가 없다는 뜻이다.
 * 
 * 실행 컨텍스트: bind 의 프로세스 문맥. 잠들 수 있다.
 * 
 * 호출 체인:
 *   pci_epf_test_bind() -> [pci_epf_test_alloc_space]
 *     -> pci_epf_alloc_space() -> (내부) dma_alloc_coherent() */
static int pci_epf_test_alloc_space(struct pci_epf *epf)
{
	/* [한국어] 이 EPF 의 상태 구조체. */
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);
	/* [한국어] 로그 대상. */
	struct device *dev = &epf->dev;
	/* [한국어] MSI-X 표의 바이트 크기. 0 으로 초기화해, MSI-X 를 쓰지 않는
	 * 경우 아래 합계에서 자동으로 빠지게 한다. */
	size_t msix_table_size = 0;
	/* [한국어] 레지스터 구조체가 차지할 크기(128 정렬 후). */
	size_t test_reg_bar_size;
	/* [한국어] PBA 의 바이트 크기. 역시 0 초기화. */
	size_t pba_size = 0;
	/* [한국어] 할당 결과 커널 가상 주소. */
	void *base;
	/* [한국어] 공유 레지스터가 들어갈 BAR 번호 - bind 가 이미 골라 두었다. */
	enum pci_barno test_reg_bar = epf_test->test_reg_bar;
	/* [한국어] 나머지 BAR 를 훑을 인덱스. */
	enum pci_barno bar;
	/* [한국어] 컨트롤러 특성 - BAR 종류와 고정 크기를 여기서 읽는다. */
	const struct pci_epc_features *epc_features = epf_test->epc_features;
	/* [한국어] 각 BAR 에 요청할 크기를 담는 임시 변수. 이름이 test_reg_size 지만
	 * 아래 루프에서 일반 BAR 크기로도 재사용된다. */
	size_t test_reg_size;

	/* [한국어] 레지스터 구조체 크기를 128 바이트로 올림 정렬한다. 그 뒤에
	 * MSI-X 표가 붙으므로 정렬 여유를 두는 것이다. */
	test_reg_bar_size = ALIGN(sizeof(struct pci_epf_test_reg), 128);

	/* [한국어] MSI-X 를 지원하는 컨트롤러라면 표와 PBA 자리도 잡아야 한다. */
	if (epc_features->msix_capable) {
		/* [한국어] 벡터마다 16 바이트(주소 8, 데이터 4, 벡터 제어 4). */
		msix_table_size = PCI_MSIX_ENTRY_SIZE * epf->msix_interrupts;
		/* [한국어] 표가 시작하는 오프셋을 기억해 둔다 - epc_init 이
		 * pci_epc_set_msix 에 넘겨 하드웨어에게 알려 준다. */
		epf_test->msix_table_offset = test_reg_bar_size;
		/* Align to QWORD or 8 Bytes */
		/* [한국어] PBA - 벡터마다 1 비트씩이라 8 로 나눠 올림해 바이트 수를 구한 뒤,
		 * 아래 영어 주석대로 8 바이트 경계로 다시 정렬한다. */
		pba_size = ALIGN(DIV_ROUND_UP(epf->msix_interrupts, 8), 8);
	}
	/* [한국어] 세 부분을 합친 것이 이 BAR 에 요청할 크기다. */
	test_reg_size = test_reg_bar_size + msix_table_size + pba_size;

	/* [한국어] 공유 레지스터 BAR 의 메모리를 잡는다. PRIMARY_INTERFACE 는
	 * 보조 EPC 가 아니라 주 EPC 쪽 BAR 라는 뜻이다. */
	base = pci_epf_alloc_space(epf, test_reg_size, test_reg_bar,
				   epc_features, PRIMARY_INTERFACE);
	/* [한국어] 할당 실패. */
	if (!base) {
		/* [한국어] 원인을 남긴다. */
		dev_err(dev, "Failed to allocated register space\n");
		/* [한국어] ★ 이 BAR 만은 없으면 안 된다 - 호스트와 대화할 수단이므로
		 * 곧바로 오류로 끝낸다. */
		return -ENOMEM;
	}
	/* [한국어] 커널 가상 주소를 기억해 둔다. 이 주소를 pci_epf_test_reg 로
	 * 캐스팅한 것이 곧 공유 레지스터다. */
	epf_test->reg[test_reg_bar] = base;

	/* [한국어] 나머지 BAR 를 훑는다. 이 for 문의 증가식은 아래 재할당 때문에
	 * 사실상 '건너뛰기의 출발점' 역할만 한다. */
	for (bar = BAR_0; bar < PCI_STD_NUM_BARS; bar++) {
		/* [한국어] ★ bar 이후의 첫 '쓸 수 있는' BAR 를 구한다. 예약된 BAR 와
		 * 64 비트 BAR 의 상위 절반을 건너뛰는 일을 이 헬퍼가 대신한다. */
		bar = pci_epc_get_next_free_bar(epc_features, bar);
		/* [한국어] 더 쓸 수 있는 BAR 가 없으면 */
		if (bar == NO_BAR)
			/* [한국어] 루프를 끝낸다. */
			break;

		/* [한국어] 공유 레지스터 BAR 는 이미 위에서 처리했으므로 */
		if (bar == test_reg_bar)
			/* [한국어] 건너뛴다. */
			continue;

		/* [한국어] 크기가 하드웨어에 고정된 BAR 라면 */
		if (epc_features->bar[bar].type == BAR_FIXED)
			/* [한국어] 사용자 설정을 무시하고 그 값을 써야 한다. */
			test_reg_size = epc_features->bar[bar].fixed_size;
		/* [한국어] 아니면 */
		else
			/* [한국어] configfs 로 사용자가 정한 크기(또는 기본값)를 쓴다. */
			test_reg_size = epf_test->bar_size[bar];

		/* [한국어] 이 BAR 의 메모리를 잡는다. */
		base = pci_epf_alloc_space(epf, test_reg_size, bar,
					   epc_features, PRIMARY_INTERFACE);
		/* [한국어] 실패해도 */
		if (!base)
			/* [한국어] 로그만 남기고 계속 진행한다 - 시험 항목이 줄 뿐 치명적이지 않다. */
			dev_err(dev, "Failed to allocate space for BAR%d\n",
				bar);
		/* [한국어] 성공이면 주소를, 실패면 NULL 을 기록한다. NULL 이 곧
		 * '이 BAR 는 없다' 는 표시가 되어 이후 모든 루프가 건너뛴다. */
		epf_test->reg[bar] = base;
	}

	/* [한국어] 공유 레지스터 BAR 만 확보되었다면 성공이다. */
	return 0;
}

/* [한국어] pci_epf_test_free_space - 잡아 둔 BAR 메모리를 모두 반납한다
 * 
 * @epf: 대상 EPF.
 * @return: 없음
 * 
 * alloc_space 의 역이다. unbind 에서만 불린다 - epc_deinit 은 노출만
 * 거두고 메모리는 그대로 두는데, 링크가 다시 붙으면 같은 메모리를
 * 다시 노출할 수 있어야 하기 때문이다.
 * 
 * ★ reg[bar] 를 NULL 로 만드는 것이 중요하다. unbind 가 두 번 불리는
 * 일은 없지만, set_bar 실패 경로도 같은 규약을 쓰므로 코드 전체가
 * 'NULL 이면 없다' 는 한 가지 규칙으로 일관되게 동작한다.
 * 
 * 실행 컨텍스트: unbind 의 프로세스 문맥.
 * 
 * 호출 체인:
 *   pci_epf_test_unbind() -> [pci_epf_test_free_space]
 *     -> pci_epf_free_space() -> (내부) dma_free_coherent() */
static void pci_epf_test_free_space(struct pci_epf *epf)
{
	/* [한국어] 이 EPF 의 상태 구조체. */
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);
	/* [한국어] BAR 인덱스. */
	int bar;

	/* [한국어] 표준 BAR 여섯 개를 훑는다. */
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		/* [한국어] 잡지 못했거나 이미 반납한 BAR 는 */
		if (!epf_test->reg[bar])
			/* [한국어] 건너뛴다. */
			continue;

		/* [한국어] 메모리를 반납한다. 커널 가상 주소를 넘기면 코어가 짝이 되는
		 * DMA 주소를 찾아 dma_free_coherent 를 부른다. */
		pci_epf_free_space(epf, epf_test->reg[bar], bar,
				   PRIMARY_INTERFACE);
		/* [한국어] '없음' 으로 표시한다. */
		epf_test->reg[bar] = NULL;
	}
}

/* [한국어] pci_epf_test_bind - EPF 가 EPC 에 연결될 때 컨트롤러 특성을 조사하고
 *                    메모리를 준비한다
 * 
 * @epf: 대상 EPF. epf->epc 가 이미 채워져 있어야 한다.
 * @return: 0 성공, -EINVAL 쓸 수 있는 BAR 가 없음, -EOPNOTSUPP 컨트롤러가
 *          특성을 알려 주지 않음, alloc_space 의 오류.
 * 
 * ★ bind 시점에 하는 일은 '하드웨어에 쓰지 않는 준비' 뿐이다.
 * 설정 헤더 쓰기와 BAR 노출은 epc_init 이 맡는다. 그렇게 나눈 이유는
 * bind 시점에 컨트롤러가 아직 준비되지 않았을 수 있기 때문이다.
 * 
 * 동작 단계:
 *   1) epc 포인터를 방어적으로 확인한다
 *   2) 컨트롤러 특성을 조회해 보관한다 - 이 값이 이후 거의 모든
 *      결정(BAR 선택, 능력 광고, 부분 매핑 가능 여부)의 근거가 된다
 *   3) 공유 레지스터를 담을 첫 번째 쓸 수 있는 BAR 를 고른다
 *   4) 모든 BAR 의 메모리를 잡는다
 * 
 * ★ test_reg_bar 를 BAR_0 로 초기화해 두지만 곧 덮어쓴다 - 컨트롤러가
 * BAR0 를 예약해 두었을 수 있어 반드시 조회해서 골라야 한다.
 * 
 * ★ WARN_ON_ONCE 를 쓰는 이유: epc 가 NULL 인 채 bind 가 불리는 것은
 * EPF 코어의 버그이지 정상적인 실패가 아니다. 그래서 조용히 오류를
 * 돌려주는 대신 한 번은 스택 추적을 남겨 알린다.
 * 
 * 실행 컨텍스트: EPF 코어가 부르는 프로세스 문맥(configfs 조립 시점).
 * 
 * 호출 체인:
 *   (EPF 코어) pci_epf_ops.bind -> [pci_epf_test_bind]
 *     -> pci_epc_get_features() -> pci_epc_get_first_free_bar()
 *     -> pci_epf_test_alloc_space() */
static int pci_epf_test_bind(struct pci_epf *epf)
{
	/* [한국어] 반환값. */
	int ret;
	/* [한국어] 이 EPF 의 상태 구조체 - probe 에서 매달아 둔 것이다. */
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);
	/* [한국어] 조회할 컨트롤러 특성. */
	const struct pci_epc_features *epc_features;
	/* [한국어] 공유 레지스터 BAR. 곧 덮어쓰지만 초기화해 둔다. */
	enum pci_barno test_reg_bar = BAR_0;
	/* [한국어] 이 EPF 가 연결된 컨트롤러. */
	struct pci_epc *epc = epf->epc;

	/* [한국어] 1) 코어의 버그로 epc 가 비어 있는 경우 - 한 번만 경고를 남긴다. */
	if (WARN_ON_ONCE(!epc))
		/* [한국어] 더 진행할 수 없다. */
		return -EINVAL;

	/* [한국어] 2) 이 함수(PF/VF)에 대한 컨트롤러 특성을 조회한다. */
	epc_features = pci_epc_get_features(epc, epf->func_no, epf->vfunc_no);
	/* [한국어] 특성을 구현하지 않은 컨트롤러라면 */
	if (!epc_features) {
		/* [한국어] 원인을 남긴다. */
		dev_err(&epf->dev, "epc_features not implemented\n");
		/* [한국어] ★ 이 드라이버는 BAR 선택부터 능력 광고까지 모두 특성에
		 * 의존하므로, 없으면 아무것도 할 수 없다. */
		return -EOPNOTSUPP;
	}

	/* [한국어] 3) 하드웨어가 예약하지 않은 첫 BAR 를 고른다. */
	test_reg_bar = pci_epc_get_first_free_bar(epc_features);
	/* [한국어] NO_BAR(-1)면 쓸 수 있는 BAR 가 하나도 없다는 뜻이다. */
	if (test_reg_bar < 0)
		/* [한국어] 잘못된 구성으로 처리한다. */
		return -EINVAL;

	/* [한국어] 고른 BAR 를 기억한다. */
	epf_test->test_reg_bar = test_reg_bar;
	/* [한국어] 특성 포인터도 보관한다 - 컨트롤러가 소유한 const 데이터이므로
	 * 복사하지 않고 포인터만 들고 있어도 안전하다. */
	epf_test->epc_features = epc_features;

	/* [한국어] 4) 모든 BAR 의 메모리를 잡는다. */
	ret = pci_epf_test_alloc_space(epf);
	/* [한국어] 공유 레지스터 BAR 를 못 잡았으면 */
	if (ret)
		/* [한국어] 바인딩 실패. */
		return ret;

	/* [한국어] 준비 완료. 실제 하드웨어 구성은 epc_init 이 이어받는다. */
	return 0;
}

/* [한국어] pci_epf_test_unbind - EPF 가 EPC 에서 떨어질 때 모든 것을 정리한다
 * 
 * @epf: 대상 EPF.
 * @return: 없음
 * 
 * bind 의 역이자 마지막 정리다. 유저스페이스가 configfs 에서 연결을
 * 끊거나 EPF 를 지울 때 불린다.
 * 
 * ★ epc->init_complete 검사가 핵심이다. epc_init 이 아직 불리지
 * 않았거나 이미 epc_deinit 이 다녀간 상태라면 DMA 채널도 BAR 노출도
 * 없다. 그때 clean/clear 를 부르면 걸지도 않은 것을 거두려 든다.
 * 그래서 컨트롤러가 '구성이 끝난 상태' 라고 표시했을 때만 부른다.
 * 반면 메모리 반납은 조건 없이 한다 - bind 가 잡았으므로 구성 여부와
 * 무관하게 항상 존재하기 때문이다.
 * 
 * ★ 폴링 취소를 가장 먼저, 그리고 _sync 판으로 하는 이유: 이 뒤로
 * BAR 메모리가 반납되므로, 아직 돌고 있는 cmd_handler 가 해제된
 * 메모리를 읽으면 안 된다.
 * 
 * 실행 컨텍스트: EPF 코어가 부르는 프로세스 문맥.
 * 
 * 호출 체인:
 *   (EPF 코어) pci_epf_ops.unbind -> [pci_epf_test_unbind]
 *     -> cancel_delayed_work_sync() -> pci_epf_test_free_space() */
static void pci_epf_test_unbind(struct pci_epf *epf)
{
	/* [한국어] 이 EPF 의 상태 구조체. */
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);
	/* [한국어] 연결되어 있던 컨트롤러 - 구성 완료 여부를 여기서 읽는다. */
	struct pci_epc *epc = epf->epc;

	/* [한국어] 폴링을 멈추고 실행 중인 핸들러가 끝나기를 기다린다. */
	cancel_delayed_work_sync(&epf_test->cmd_handler);
	/* [한국어] ★ 컨트롤러 구성이 실제로 이뤄진 상태일 때만 되돌린다. */
	if (epc->init_complete) {
		/* [한국어] DMA 채널을 반납한다. */
		pci_epf_test_clean_dma_chan(epf_test);
		/* [한국어] BAR 노출을 거둔다. */
		pci_epf_test_clear_bar(epf);
	}
	/* [한국어] 메모리는 조건 없이 반납한다 - bind 가 잡은 것이기 때문이다. */
	pci_epf_test_free_space(epf);
}

/* [한국어] PCI_EPF_TEST_BAR_SIZE_R - barN_size configfs 속성의 '읽기' 함수를
 * 찍어 내는 매크로.
 * 
 * configfs 는 sysfs 와 비슷하지만 유저스페이스가 디렉터리를 만들어
 * 객체를 '조립' 할 수 있다는 점이 다르다. 이 EPF 는
 * /sys/kernel/config/pci_ep/functions/pci_epf_test/<이름>/ 아래에
 * bar0_size ~ bar5_size 파일을 두어, EPC 에 붙이기 전에 BAR 크기를
 * 바꿀 수 있게 한다.
 * 
 * ## 는 토큰 붙이기 연산자로, _name 이 bar0_size 면
 * pci_epf_test_bar0_size_show 라는 이름을 만든다 - 아래 CONFIGFS_ATTR
 * 가 그 이름을 찾아 연결하므로 명명 규칙이 정확해야 한다.
 * _id 는 배열 인덱스로 쓸 BAR 번호다.
 * 
 * 본문의 흐름: config_item 에서 config_group 을 얻고, 그 group 이
 * pci_epf_test 안에 값으로 박혀 있으므로 container_of 로 바깥 구조체를
 * 역산한 뒤, 해당 BAR 의 크기를 찍는다.
 * 
 * 주의: 매크로 본문은 역슬래시로 이어진 한 줄이라 그 사이에 주석을
 * 넣을 수 없다 - 그래서 설명을 여기 모았다. */
#define PCI_EPF_TEST_BAR_SIZE_R(_name, _id)				\
static ssize_t pci_epf_test_##_name##_show(struct config_item *item,	\
					   char *page)			\
{									\
	struct config_group *group = to_config_group(item);		\
	struct pci_epf_test *epf_test =					\
		container_of(group, struct pci_epf_test, group);	\
									\
	return sysfs_emit(page, "%zu\n", epf_test->bar_size[_id]);	\
}

/* [한국어] PCI_EPF_TEST_BAR_SIZE_W - barN_size 속성의 '쓰기' 함수를 찍어 내는 매크로.
 * 
 * ★ 본문의 세 가지 검증이 중요하다(본문 안 영어 주석이 첫째를 밝힌다):
 *   1) epf->epc 가 이미 있으면 -EOPNOTSUPP. 크기는 bind 안의
 *      alloc_space 에서 한 번만 쓰이므로, 붙은 뒤에 바꿔 봐야 반영되지
 *      않는다. 조용히 무시하는 대신 명확히 거절한다.
 *   2) kstrtouint 로 문자열을 정수로 바꾼다. 진법 인자 0 은 '0x 면
 *      16진수, 0 으로 시작하면 8진수, 아니면 10진수' 로 자동 판별하라는 뜻이다.
 *   3) 2의 거듭제곱이 아니면 -EINVAL. PCI BAR 는 크기가 2의 거듭제곱
 *      이어야 한다는 스펙 요구를 여기서 미리 거른다.
 * 통과하면 값을 저장하고 '전부 소비했다' 는 뜻으로 len 을 돌려준다.
 * 
 * 미묘한 점: val 이 int 로 선언되어 있는데 kstrtouint 는 unsigned int
 * 포인터를 받는다. 타입이 어긋나지만 컴파일러 경고 수준에서 통과하며,
 * is_power_of_2 검사가 매우 큰 값을 걸러 내므로 실질적인 문제는
 * 드러나지 않는다 - 이 트리의 코드가 그렇다는 사실만 적어 둔다.
 * 
 * 역슬래시 연결 때문에 본문 안에는 주석을 넣을 수 없다. */
#define PCI_EPF_TEST_BAR_SIZE_W(_name, _id)				\
static ssize_t pci_epf_test_##_name##_store(struct config_item *item,	\
					    const char *page,		\
					    size_t len)			\
{									\
	struct config_group *group = to_config_group(item);		\
	struct pci_epf_test *epf_test =					\
		container_of(group, struct pci_epf_test, group);	\
	int val, ret;							\
									\
	/*								\
	 * BAR sizes can only be modified before binding to an EPC,	\
	 * because pci_epf_test_alloc_space() is called in .bind().	\
	 */								\
	if (epf_test->epf->epc)						\
		return -EOPNOTSUPP;					\
									\
	ret = kstrtouint(page, 0, &val);				\
	if (ret)							\
		return ret;						\
									\
	if (!is_power_of_2(val))					\
		return -EINVAL;						\
									\
	epf_test->bar_size[_id] = val;					\
									\
	return len;							\
}

/* [한국어] BAR0 크기 읽기 함수 생성. */
PCI_EPF_TEST_BAR_SIZE_R(bar0_size, BAR_0)
/* [한국어] BAR0 크기 쓰기 함수 생성. 읽기/쓰기를 따로 찍어 내는 이유는
 * 매크로 하나에 두 함수를 담으면 이름 규칙이 복잡해지기 때문이다. */
PCI_EPF_TEST_BAR_SIZE_W(bar0_size, BAR_0)
/* [한국어] BAR1 읽기. */
PCI_EPF_TEST_BAR_SIZE_R(bar1_size, BAR_1)
/* [한국어] BAR1 쓰기. */
PCI_EPF_TEST_BAR_SIZE_W(bar1_size, BAR_1)
/* [한국어] BAR2 읽기. */
PCI_EPF_TEST_BAR_SIZE_R(bar2_size, BAR_2)
/* [한국어] BAR2 쓰기. */
PCI_EPF_TEST_BAR_SIZE_W(bar2_size, BAR_2)
/* [한국어] BAR3 읽기. */
PCI_EPF_TEST_BAR_SIZE_R(bar3_size, BAR_3)
/* [한국어] BAR3 쓰기. */
PCI_EPF_TEST_BAR_SIZE_W(bar3_size, BAR_3)
/* [한국어] BAR4 읽기. */
PCI_EPF_TEST_BAR_SIZE_R(bar4_size, BAR_4)
/* [한국어] BAR4 쓰기. */
PCI_EPF_TEST_BAR_SIZE_W(bar4_size, BAR_4)
/* [한국어] BAR5 읽기. */
PCI_EPF_TEST_BAR_SIZE_R(bar5_size, BAR_5)
/* [한국어] BAR5 쓰기. */
PCI_EPF_TEST_BAR_SIZE_W(bar5_size, BAR_5)

/* [한국어] CONFIGFS_ATTR - 위에서 만든 show/store 함수 쌍을 하나의 읽고 쓸 수
 * 있는 configfs 속성으로 묶는다. 첫 인자가 함수 이름의 접두어이고
 * 둘째가 속성 이름이라, pci_epf_test_bar0_size_show/_store 를 찾아
 * pci_epf_test_attr_bar0_size 라는 구조체를 만든다. */
CONFIGFS_ATTR(pci_epf_test_, bar0_size);
/* [한국어] BAR1 속성. */
CONFIGFS_ATTR(pci_epf_test_, bar1_size);
/* [한국어] BAR2 속성. */
CONFIGFS_ATTR(pci_epf_test_, bar2_size);
/* [한국어] BAR3 속성. */
CONFIGFS_ATTR(pci_epf_test_, bar3_size);
/* [한국어] BAR4 속성. */
CONFIGFS_ATTR(pci_epf_test_, bar4_size);
/* [한국어] BAR5 속성. */
CONFIGFS_ATTR(pci_epf_test_, bar5_size);

/* [한국어] pci_epf_test_attrs - 이 EPF 의 configfs 디렉터리에 나타날 속성 목록.
 * 아래 그룹 타입에 실려 add_cfs 가 만드는 디렉터리에 파일로 나타난다. */
static struct configfs_attribute *pci_epf_test_attrs[] = {
	/* [한국어] bar0_size 파일. */
	&pci_epf_test_attr_bar0_size,
	/* [한국어] bar1_size 파일. */
	&pci_epf_test_attr_bar1_size,
	/* [한국어] bar2_size 파일. */
	&pci_epf_test_attr_bar2_size,
	/* [한국어] bar3_size 파일. */
	&pci_epf_test_attr_bar3_size,
	/* [한국어] bar4_size 파일. */
	&pci_epf_test_attr_bar4_size,
	/* [한국어] bar5_size 파일. */
	&pci_epf_test_attr_bar5_size,
	/* [한국어] 목록의 끝 표식 - configfs 코어가 NULL 까지 순회한다. */
	NULL,
};

/* [한국어] pci_epf_test_group_type - 이 EPF 의 configfs 디렉터리 타입.
 * 어떤 속성 파일들이 그 안에 나타날지와, 그 파일들을 다루는 코드를
 * 소유한 모듈이 무엇인지를 알려 준다. */
static const struct config_item_type pci_epf_test_group_type = {
	/* [한국어] 위에서 만든 속성 목록을 붙인다. */
	.ct_attrs	= pci_epf_test_attrs,
	/* [한국어] 이 모듈이 소유자. 속성 파일이 열려 있는 동안 모듈이
	 * 언로드되지 않게 막는 근거다. */
	.ct_owner	= THIS_MODULE,
};

/* [한국어] pci_epf_test_add_cfs - 이 EPF 전용 configfs 그룹을 만들어 돌려준다
 * 
 * @epf: 대상 EPF.
 * @group: 부모 그룹(이 함수는 쓰지 않는다). EPF 코어가 만든 상위
 *         디렉터리이며, 반환한 그룹이 그 아래에 놓인다.
 * @return: 이 EPF 의 configfs 그룹.
 * 
 * ★ 왜 이 콜백이 있는가: EPF 코어는 모든 EPF 에 공통인 속성
 * (vendorid, deviceid, msi_interrupts 등)을 스스로 만든다. 함수마다
 * 고유한 속성이 더 필요하면 이 콜백으로 알린다. 이 드라이버의 고유
 * 속성이 barN_size 여섯 개다.
 * 
 * 그룹 이름을 dev_name(dev) 으로 짓는 이유: EPF 인스턴스가 여럿일 수
 * 있으므로(pci_epf_test.0, pci_epf_test.1 ...) 이름이 겹치지 않아야 한다.
 * 
 * ★ 그룹 구조체는 pci_epf_test 안에 값으로 박혀 있으므로 따로 할당할
 * 필요가 없고, 속성 함수들이 container_of 로 바깥 구조체를 되찾을 수
 * 있다. 해제도 devm 이 pci_epf_test 를 반납할 때 함께 사라진다.
 * 
 * 실행 컨텍스트: EPF 코어가 부르는 프로세스 문맥(configfs 에서 EPF 를
 * 만들 때).
 * 
 * 호출 체인:
 *   (EPF 코어) pci_epf_ops.add_cfs -> [pci_epf_test_add_cfs]
 *     -> config_group_init_type_name() */
static struct config_group *pci_epf_test_add_cfs(struct pci_epf *epf,
						 struct config_group *group)
{
	/* [한국어] 이 EPF 의 상태 구조체. */
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);
	/* [한국어] 그 안에 박혀 있는 configfs 그룹을 가리킨다. */
	struct config_group *epf_group = &epf_test->group;
	/* [한국어] 이름을 지을 때 쓸 device. */
	struct device *dev = &epf->dev;

	/* [한국어] 그룹을 초기화하면서 이름과 타입을 붙인다. 이름은 EPF 장치
	 * 이름이라 인스턴스마다 다르고, 타입은 barN_size 속성 목록을 나른다. */
	config_group_init_type_name(epf_group, dev_name(dev),
				    &pci_epf_test_group_type);

	/* [한국어] 코어가 이 그룹을 부모 아래에 등록한다. */
	return epf_group;
}

/* [한국어] pci_epf_test_ids - 이 드라이버가 맡을 EPF 이름 목록.
 * ★ EPF 는 pci_epf_bus_type 이라는 가상 버스에 올라가고, PCI 처럼
 * 벤더/장치 ID 가 아니라 '이름' 으로 드라이버와 짝지어진다. 유저스페이스가
 * configfs 에서 pci_epf_test 라는 이름으로 디렉터리를 만들면 그 이름이
 * 이 표와 대조되어 이 드라이버의 probe 가 불린다. */
static const struct pci_epf_device_id pci_epf_test_ids[] = {
	/* [한국어] 단 하나의 항목. */
	{
		/* [한국어] configfs 에서 쓸 이름. 아래 드라이버의 driver.name 과 같은
		 * 문자열이라는 점에 유의 - 둘 다 이 이름으로 조회된다. */
		.name = "pci_epf_test",
	},
	/* [한국어] 목록의 끝 표식(빈 항목). */
	{},
};

/* [한국어] pci_epf_test_probe - EPF 인스턴스가 만들어질 때 상태 구조체를 준비한다
 * 
 * @epf: 새로 만들어진 EPF.
 * @id: 일치한 id_table 항목(이 함수는 쓰지 않는다).
 * @return: 0 성공, -ENOMEM 메모리 부족.
 * 
 * ★ probe 는 '아직 어떤 컨트롤러에도 붙지 않은' 시점에 불린다.
 * 그래서 하드웨어와 관련된 일은 아무것도 하지 않고, 순수하게
 * 소프트웨어 상태만 준비한다:
 *   - 상태 구조체를 devm 으로 잡는다(EPF 가 사라질 때 자동 반납)
 *   - 설정 헤더를 매단다
 *   - BAR 크기를 기본값으로 채운다 - 사용자가 configfs 로 바꿀 수 있다
 *   - 폴링 작업을 초기화한다(예약은 하지 않는다 - 붙은 뒤에 한다)
 *   - 이벤트 콜백 표를 매단다
 *   - 상태 구조체를 EPF 에 매달아 이후 모든 콜백이 되찾을 수 있게 한다
 * 
 * 실행 컨텍스트: EPF 코어가 부르는 프로세스 문맥.
 * 
 * 호출 체인:
 *   (EPF 코어, 이름 매칭 후) pci_epf_driver.probe -> [pci_epf_test_probe] */
static int pci_epf_test_probe(struct pci_epf *epf,
			      const struct pci_epf_device_id *id)
{
	/* [한국어] 만들 상태 구조체. */
	struct pci_epf_test *epf_test;
	/* [한국어] devm 의 기준 장치이자 로그 대상. */
	struct device *dev = &epf->dev;
	/* [한국어] BAR 크기 초기화 루프의 인덱스. */
	enum pci_barno bar;

	/* [한국어] EPF device 에 묶인 관리형 할당 - EPF 가 해제될 때 커널이
	 * 자동으로 반납하므로 remove 콜백이 따로 필요 없다. */
	epf_test = devm_kzalloc(dev, sizeof(*epf_test), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!epf_test)
		return -ENOMEM;
/* [한국어] epc_init 이 하드웨어에 쓸 설정 헤더를 매단다. */

	/* [한국어] 상태 구조체에서 EPF 로 되짚어 갈 수 있게 한다. */
	epf->header = &test_header;
	epf_test->epf = epf;
	/* [한국어] 표준 BAR 여섯 개의 크기를 */
	for (bar = BAR_0; bar < PCI_STD_NUM_BARS; bar++)
		/* [한국어] 기본값으로 채운다. 인스턴스마다 사본을 두므로, 한 인스턴스에서
		 * 바꿔도 다른 인스턴스에 영향이 없다. */
		epf_test->bar_size[bar] = default_bar_size[bar];

	/* [한국어] 폴링 작업을 초기화하고 핸들러 함수를 연결한다. 예약은 하지
	 * 않는다 - 컨트롤러에 붙어 링크가 준비된 뒤에 시작해야 한다. */
	INIT_DELAYED_WORK(&epf_test->cmd_handler, pci_epf_test_cmd_handler);

	/* [한국어] epc_init/epc_deinit/link_up/link_down 콜백 표를 매단다.
	 * EPC 코어가 이 표를 통해 사건을 알려 준다. */
	epf->event_ops = &pci_epf_test_event_ops;

	/* [한국어] EPF 에 상태 구조체를 매단다. bind/unbind 를 비롯한 모든
	 * 콜백이 epf_get_drvdata 로 이것을 되찾는다. */
	epf_set_drvdata(epf, epf_test);
	/* [한국어] 준비 완료. */
	return 0;
}

/* [한국어] ops - EPF 코어가 부르는 함수 표. 이벤트 콜백(event_ops)과
 * 달리 이쪽은 EPF 자체의 수명 주기를 다룬다. */
static const struct pci_epf_ops ops = {
	/* [한국어] EPC 에서 떨어질 때 - 모든 자원을 정리한다. */
	.unbind	= pci_epf_test_unbind,
	/* [한국어] EPC 에 붙을 때 - 특성을 조사하고 메모리를 잡는다. */
	.bind	= pci_epf_test_bind,
	/* [한국어] configfs 디렉터리를 만들 때 - 고유 속성을 더한다. */
	.add_cfs = pci_epf_test_add_cfs,
};

/* [한국어] test_driver - EPF 가상 버스에 등록할 드라이버 서술자. */
static struct pci_epf_driver test_driver = {
	/* [한국어] 드라이버 이름. id_table 의 이름과 같은 문자열이라, configfs 가
	 * 이 이름으로 드라이버를 찾을 수 있다. */
	.driver.name	= "pci_epf_test",
	/* [한국어] 이름이 일치하는 EPF 가 만들어질 때 불린다. */
	.probe		= pci_epf_test_probe,
	/* [한국어] 맡을 EPF 이름 목록. */
	.id_table	= pci_epf_test_ids,
	/* [한국어] bind/unbind/add_cfs 함수 표. */
	.ops		= &ops,
	/* [한국어] 이 모듈이 소유자. */
	.owner		= THIS_MODULE,
};

/* [한국어] pci_epf_test_init - 모듈 적재 진입점
 * 
 * @return: 0 성공, -ENOMEM 워크큐 생성 실패, 그 밖에 드라이버 등록 오류.
 * 
 * 두 가지 전역 자원을 준비한다 - 폴링 작업이 돌 워크큐와, EPF 버스
 * 등록. 순서가 중요하다: 워크큐를 먼저 만들어야 한다. 드라이버를
 * 등록하면 곧바로 probe 와 bind 가 불릴 수 있고, 그 경로가 워크큐를
 * 쓰기 때문이다.
 * 
 * ★ 워크큐 플래그의 의미:
 *   - WQ_MEM_RECLAIM : 메모리 회수 압박 상황에서도 진행이 보장되도록
 *     전용 구조 스레드를 확보한다. 이 작업이 멈추면 호스트가 응답을
 *     영원히 기다리게 되므로 안전장치가 필요하다.
 *   - WQ_HIGHPRI     : 다른 작업보다 먼저 실행한다. 1ms 주기의 폴링이
 *     밀리면 시험 지연이 그대로 커진다.
 *   - WQ_PERCPU      : CPU 마다 작업 풀을 두어, 작업이 예약된 CPU 에서
 *     실행되게 한다(CPU 간 이동에 따른 지연을 줄인다).
 *   마지막 인자 0 은 동시 실행 개수 제한 없음(기본값)을 뜻한다.
 * 
 * 실행 컨텍스트: 프로세스 문맥(insmod/modprobe).
 * 
 * 호출 체인:
 *   module_init(pci_epf_test_init) -> [pci_epf_test_init]
 *     -> alloc_workqueue() -> pci_epf_register_driver() */
static int __init pci_epf_test_init(void)
{
	/* [한국어] 반환값. */
	int ret;

	/* [한국어] 폴링 전용 워크큐를 만든다. 이름 "kpcitest" 가 ps 에서 보이는
	 * 커널 스레드 이름이 된다. */
	kpcitest_workqueue = alloc_workqueue("kpcitest",
				    WQ_MEM_RECLAIM | WQ_HIGHPRI | WQ_PERCPU, 0);
	/* [한국어] 생성 실패. */
	if (!kpcitest_workqueue) {
		/* [한국어] 원인을 남긴다. 아직 장치가 없으므로 dev_err 이 아니라 pr_err 이다. */
		pr_err("Failed to allocate the kpcitest work queue\n");
		/* [한국어] 메모리 부족. */
		return -ENOMEM;
	}

	/* [한국어] EPF 가상 버스에 드라이버를 등록한다. 이 호출 뒤로 이름이
	 * 맞는 EPF 가 만들어지면 probe 가 불린다. */
	ret = pci_epf_register_driver(&test_driver);
	/* [한국어] 등록 실패. */
	if (ret) {
		/* [한국어] ★ 앞서 만든 워크큐를 반드시 반납해야 한다 - 그러지 않으면
		 * 모듈 적재가 실패했는데도 커널 스레드가 남는다. */
		destroy_workqueue(kpcitest_workqueue);
		/* [한국어] 원인과 오류 코드를 남긴다. */
		pr_err("Failed to register pci epf test driver --> %d\n", ret);
		/* [한국어] 실패를 modprobe 에 전한다. */
		return ret;
	}

	/* [한국어] 두 자원이 모두 준비되었다. */
	return 0;
}
/* [한국어] 이 함수를 모듈 진입점으로 등록한다. */
module_init(pci_epf_test_init);

/* [한국어] pci_epf_test_exit - 모듈 해제 진입점
 * 
 * @return: 없음
 * 
 * ★ 순서에 주의: 여기서는 워크큐를 먼저 파괴하고 드라이버를 나중에
 * 등록 해제한다 - init 의 역순이 아니다. 일반적인 규칙(역순 정리)을
 * 따르자면 드라이버를 먼저 떼어 모든 EPF 의 unbind 가 끝나게 한 뒤
 * 워크큐를 파괴하는 편이 안전하다. 이 트리의 코드가 그 반대로 되어
 * 있다는 사실만 적어 둔다 - 그 배치의 의도는 확인하지 못했다.
 * (destroy_workqueue 자체는 큐에 남은 작업이 끝나기를 기다리므로,
 * 실행 중인 cmd_handler 가 중간에 잘리지는 않는다.)
 * 
 * 실행 컨텍스트: 프로세스 문맥(rmmod).
 * 
 * 호출 체인:
 *   module_exit(pci_epf_test_exit) -> [pci_epf_test_exit]
 *     -> destroy_workqueue() -> pci_epf_unregister_driver() */
static void __exit pci_epf_test_exit(void)
{
	/* [한국어] init 이 실패한 뒤에는 여기 오지 않지만, 방어적으로 확인한다. */
	if (kpcitest_workqueue)
		/* [한국어] 큐에 남은 작업이 모두 끝나기를 기다린 뒤 워크큐를 없앤다. */
		destroy_workqueue(kpcitest_workqueue);
	/* [한국어] EPF 버스에서 드라이버를 뗀다. 이 안에서 바인딩된 EPF 들의
	 * unbind 가 불린다. */
	pci_epf_unregister_driver(&test_driver);
}
/* [한국어] 이 함수를 모듈 해제 진입점으로 등록한다. */
module_exit(pci_epf_test_exit);

/* [한국어] modinfo 로 보이는 모듈 설명. */
MODULE_DESCRIPTION("PCI EPF TEST DRIVER");
/* [한국어] 원작자. */
MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
/* [한국어] 라이선스 선언. "GPL v2" 도 GPL 호환으로 인정되어 커널이
 * taint 표시를 하지 않는다. */
MODULE_LICENSE("GPL v2");
