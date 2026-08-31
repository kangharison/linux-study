// SPDX-License-Identifier: GPL-2.0
/*
 * Microsemi Switchtec(tm) PCIe Management Driver
 * Copyright (c) 2017, Microsemi Corporation
 */
/* [한국어] Microsemi(현 Microchip) Switchtec PCIe 스위치 관리 드라이버 (switchtec.c)
 * 
 * === 파일의 역할 ===
 * Switchtec 은 하나의 호스트 포트를 여러 다운스트림 포트로 펼치는 PCIe
 * 스위치다. 이 파일은 그 스위치를 '데이터가 지나가는 길' 로서가 아니라
 * '관리 대상 장치' 로서 다룬다. 즉 이 드라이버가 없어도 스위치 아래
 * 달린 장치들은 정상 동작한다 - PCIe 스위치의 데이터 경로는 PCI 코어가
 * 투명하게 다루기 때문이다. 이 드라이버가 더해 주는 것은 스위치 내부를
 * 들여다보고 조작하는 통로다: 펌웨어 버전과 플래시 파티션 조회, 포트
 * 번호와 내부 PFF 인스턴스 번호의 상호 변환, 오류/핫플러그/링크 상태
 * 같은 이벤트의 통지와 마스킹, 그리고 임의의 펌웨어 명령(MRPC) 전달.
 * 드라이버 자신은 MRPC 명령의 의미를 전혀 해석하지 않는다 - 유저스페이스
 * 도구와 펌웨어 사이의 통로 역할만 하므로, 새 펌웨어 기능이 생겨도
 * 드라이버를 고칠 필요가 없다.
 * 
 * === 전체 아키텍처에서의 위치 ===
 * 실행 컨텍스트는 커널 모듈이며, 세 가지 문맥을 오간다.
 *   - 프로세스 문맥: 유저스페이스가 /dev/switchtecN 을 열고
 *     write/read/poll/ioctl 을 부르는 경로, 그리고 sysfs 속성 읽기.
 *   - 하드 인터럽트 문맥: MSI/MSI-X 벡터로 올라오는 이벤트와 MRPC 완료.
 *     switchtec_event_isr() 와 switchtec_dma_mrpc_isr() 두 개다.
 *   - 워크큐(커널 스레드) 문맥: 인터럽트가 넘긴 완료 처리
 *     (mrpc_event_work), 500ms 폴링 안전망(mrpc_timeout_work), 링크 상태
 *     통지(link_event_work).
 * 호출 방향으로 보면 위쪽은 PCI 코어(probe/remove)와 VFS(문자 디바이스,
 * sysfs)가 이 파일을 부르고, 아래쪽으로는 ioread32/iowrite32 계열의 MMIO
 * 접근과 dma_alloc_coherent, pci_alloc_irq_vectors 를 부른다.
 * MRPC 는 write 로 명령을 큐에 넣고 read 로 응답을 받는 두 단계 프로토콜
 * 이어서, 그 사이에 poll 로 기다리거나 O_NONBLOCK 으로 즉시 확인할 수 있다.
 * 
 * === 타 모듈과의 연결 ===
 * 의존하는 쪽: PCI 코어(pci_register_driver, pcim_enable_device,
 * pci_alloc_irq_vectors), 문자 디바이스 계층(cdev, alloc_chrdev_region),
 * device/class 모델(sysfs 속성 그룹), DMA API, 워크큐.
 * UAPI 는 include/uapi/linux/switchtec_ioctl.h 가, 레지스터 배치와
 * struct switchtec_dev 는 include/linux/switchtec.h 가 정의한다. 두 헤더는
 * 이 부분 체크아웃에는 없고 원본 스냅숏(1f0e418bb6)에서만 확인했다.
 * 이 파일에 의존하는 쪽: switchtec_class 를 EXPORT_SYMBOL_GPL 로 내보내고
 * 있으나, 이 트리 안에서 그 심볼을 참조하는 파일은 하나도 없다.
 * switchtec_pci_probe() 가 request_module_nowait("ntb_hw_switchtec") 로
 * 부르는 NTB 드라이버(drivers/ntb/)도 이 체크아웃에 없어 확인하지 못했다.
 * struct switchtec_dev 의 sndev 와 link_notifier 필드는 이 파일에서
 * '읽기만' 하고 설정하는 코드가 없는데, 그 설정자가 바로 그 NTB
 * 드라이버로 보이나 근거를 직접 확인하지는 못했다.
 * 같은 트리 안의 실제 참조는 drivers/pci/quirks.c 한 곳이다 - 그쪽은
 * <linux/switchtec.h> 를 포함해 NTB 의 Requester ID Table 을 읽어
 * DMA 별칭 quirk 를 건다. 이 파일의 함수를 부르지는 않는다.
 * ★ NVMe 와의 관계에 대한 사실 확인: Switchtec 스위치는 NVMe 백플레인에서
 * 호스트 포트 하나를 여러 드라이브로 펼치는 데 쓰이지만, 그 관계는
 * '토폴로지상' 의 것이다. drivers/nvme 트리에서 switchtec 을 참조하는
 * 코드는 한 줄도 없다 - 확인했다. NVMe 드라이버는 스위치 아래 붙은
 * 엔드포인트만 볼 뿐 스위치 자체는 알지 못한다.
 * 
 * === 주요 함수/구조체 요약 ===
 *   - switchtec_pci_probe() : 장치 객체를 만들고 BAR0 의 GAS 창을 매핑하고
 *     IRQ 를 걸고 /dev/switchtecN 을 등록하는 진입점.
 *   - switchtec_init_pci()  : GAS 를 두 조각으로 매핑한다 - MRPC 구역은
 *     write-combining 으로(대량 쓰기가 빠르다), 나머지는 uncached 로.
 *     그리고 세대에 따라 gen3/gen4 레지스터 레이아웃을 고른다.
 *   - mrpc_cmd_submit() / mrpc_complete_cmd() / mrpc_cleanup_cmd() :
 *     MRPC 명령의 제출-완료-정리 3단계. 하드웨어 창구가 하나뿐이라
 *     mrpc_busy 와 대기 큐로 직렬화한다.
 *   - switchtec_event_isr() : 이벤트를 해석하지 않고 '마스킹만' 한 뒤
 *     유저스페이스를 깨운다 - 인터럽트 폭주를 막는 이 드라이버의 핵심 모델.
 *   - event_regs[]         : 이벤트 ID -> (오프셋, 매핑 함수) 표. 함수
 *     포인터 값 자체를 비교해 이벤트가 전역/파티션/PFF 중 어디에 속하는지
 *     되묻는 관용구를 쓴다.
 *   - struct switchtec_dev  : 장치 하나의 모든 상태(헤더에 정의). mrpc_mutex
 *     하나가 mrpc_ 필드들, alive 플래그, stuser->state 를 모두 지킨다.
 *   - struct switchtec_user : 열린 fd 하나의 MRPC 문맥. kref 로 수명을
 *     관리해, 명령이 큐에 있는 동안 fd 가 닫혀도 살아남는다. */

/* [한국어] include/linux/switchtec.h - 이 드라이버의 중심 헤더. GAS 안의 레지스터
 * 블록 배치(mrpc_regs, sw_event_regs, sys_info_regs, part_cfg_regs,
 * pff_csr_regs 등), 세대 enum, 이벤트 비트 정의, 그리고 struct switchtec_dev
 * 자체가 여기 있다. 구조체 필드 하나하나가 실제 하드웨어 레지스터에
 * 대응하므로 __packed 로 패딩을 금지한다. */
#include <linux/switchtec.h>
/* [한국어] include/uapi/linux/switchtec_ioctl.h - 유저스페이스와의 계약.
 * ioctl 번호('W' 매직), 파티션 종류 상수, 이벤트 ID(0~31), 이벤트 플래그
 * 비트, 그리고 주고받는 구조체들이 정의되어 있다. UAPI 이므로 한번
 * 공개된 배치는 바꿀 수 없고, 이벤트 요약 구조체가 두 판으로 나뉜 것도
 * 그 때문이다. */
#include <linux/switchtec_ioctl.h>

/* [한국어] irqreturn_t, devm_request_irq 등 인터럽트 핸들러 등록에 필요하다.
 * 이 드라이버는 이벤트용과 DMA MRPC 완료용 두 개의 ISR 를 건다. */
#include <linux/interrupt.h>
/* [한국어] MODULE_* 매크로, module_param, THIS_MODULE. 모듈로 빌드될 때
 * 필요하며 module_init/module_exit 도 여기서 온다. */
#include <linux/module.h>
/* [한국어] struct file_operations, cdev, alloc_chrdev_region - /dev/switchtecN
 * 문자 디바이스를 만드는 데 필요한 것들이다. */
#include <linux/fs.h>
/* [한국어] copy_to_user/copy_from_user - 유저 버퍼와 커널 버퍼 사이의 안전한
 * 복사. write/read/ioctl 이 모두 쓴다. */
#include <linux/uaccess.h>
/* [한국어] poll_wait, __poll_t, EPOLL* 상수 - switchtec_dev_poll 이 쓴다. */
#include <linux/poll.h>
/* [한국어] wait_queue_head_t, wait_event_interruptible - MRPC 응답을 기다리는
 * 동안 잠들기 위한 대기 큐. */
#include <linux/wait.h>
/* [한국어] ★ ioread64/writeq 를 제공한다. 이름 그대로 '64 비트 원자성이
 * 필요 없는' 경우를 위한 헤더로, 64 비트 MMIO 접근이 없는 아키텍처에서는
 * 32 비트 두 번(하위 먼저, 상위 나중 - lo_hi)으로 자동 대체한다.
 * NTB 파티션 이벤트 비트맵 읽기와 DMA 주소 쓰기가 이것을 쓴다. */
#include <linux/io-64-nonatomic-lo-hi.h>
/* [한국어] array_index_nospec - Spectre v1(투기 실행을 이용한 경계 검사 우회)
 * 방어. ioctl_port_to_pff() 에서 유저가 준 포트 번호를 배열 인덱스로
 * 쓰기 직전에 한 번 쓰인다 - 이 헤더가 필요한 유일한 이유다. */
#include <linux/nospec.h>

/* [한국어] modinfo 로 보이는 모듈 설명. */
MODULE_DESCRIPTION("Microsemi Switchtec(tm) PCIe Management Driver");
/* [한국어] 모듈 버전 문자열. 상류에서 0.1 로 고정되어 있다. */
MODULE_VERSION("0.1");
/* [한국어] 라이선스 선언. GPL 이어야 EXPORT_SYMBOL_GPL 심볼을 쓸 수 있고,
 * 커널이 taint 표시를 하지 않는다. */
MODULE_LICENSE("GPL");
/* [한국어] 작성자. */
MODULE_AUTHOR("Microsemi Corporation");

/* [한국어] max_devices - 이 모듈이 만들 수 있는 문자 디바이스의 최대 개수.
 * switchtec_init() 이 이 개수만큼 (major, minor) 범위를 한 번에 예약하며,
 * 그 뒤로는 바꿔도 이미 예약된 범위에 영향이 없다. 기본 16 개. */
static int max_devices = 16;
/* [한국어] 0644 권한으로 /sys/module/switchtec/parameters/ 아래 노출된다 -
 * 읽기는 누구나, 쓰기는 root 만. */
module_param(max_devices, int, 0644);
/* [한국어] modinfo 에 보일 설명. */
MODULE_PARM_DESC(max_devices, "max number of switchtec device instances");

/* [한국어] use_dma_mrpc - MRPC 응답을 호스트 메모리로 DMA 받을지 여부.
 * 켜면(기본값) 응답을 memcpy_fromio 대신 평범한 memcpy 로 읽을 수 있어
 * 완료 처리가 가벼워진다. 펌웨어가 지원하지 않으면(dma_ver == 0) 이 값과
 * 무관하게 MMIO 방식으로 물러난다. 문제 진단 시 끄고 비교해 볼 수 있게
 * 런타임 파라미터로 두었다. */
static bool use_dma_mrpc = true;
/* [한국어] 역시 0644 로 노출. 다만 이미 붙어 있는 장치에는 영향이 없다 -
 * probe 시점에만 읽히기 때문이다. */
module_param(use_dma_mrpc, bool, 0644);
/* [한국어] modinfo 설명. */
MODULE_PARM_DESC(use_dma_mrpc,
		 "Enable the use of the DMA MRPC feature");

/* [한국어] nirqs - 확보를 시도할 MSI/MSI-X 벡터 개수. 이 드라이버 자신은
 * 이벤트용과 DMA MRPC 완료용 두 개면 충분하지만, 같은 장치를 쓰는 NTB
 * 드라이버가 도어벨을 벡터에 대응시키느라 더 많이 필요할 수 있다.
 * switchtec_init_isr() 이 4 미만이면 4 로 끌어올린다. */
static int nirqs = 32;
/* [한국어] 0644 로 노출. */
module_param(nirqs, int, 0644);
/* [한국어] 설명에 NTB 응용에서 더 유용하다는 점이 명시되어 있다. */
MODULE_PARM_DESC(nirqs, "number of interrupts to allocate (more may be useful for NTB applications)");

/* [한국어] switchtec_devt - alloc_chrdev_region 이 예약해 준 문자 디바이스
 * 번호의 시작점(major + 시작 minor).
 * 설정자: switchtec_init() 이 모듈 적재 때 한 번.
 * 읽는 자: stdev_create() 가 MAJOR() 를 뽑아 MKDEV 로 실제 번호를 만들고,
 *         switchtec_exit() 이 반납할 때 쓴다.
 * 값 범위: 유효한 dev_t. 초기화 전에는 0.
 * 동기화: 모듈 적재/해제 시점에만 바뀌므로 별도 락이 없다. */
static dev_t switchtec_devt;
/* [한국어] switchtec_minor_ida - 부 번호 할당기. IDA(ID Allocator)는 '가장
 * 작은 빈 정수' 를 내주는 자료구조다.
 * 설정자/읽는 자: stdev_create() 가 ida_alloc 으로 뽑고, remove 와 probe
 *         실패 경로가 ida_free 로 반납한다. switchtec_exit() 이 파괴한다.
 * 값 범위: 0 이상. 상한은 별도로 강제되지 않으므로, max_devices 를
 *         넘는 번호가 나오면 MKDEV 결과가 예약 범위를 벗어날 수 있다.
 * 동기화: IDA 자체가 내부 스핀락으로 동시 접근을 막아 준다. */
static DEFINE_IDA(switchtec_minor_ida);

/* [한국어] switchtec_class - /sys/class/switchtec 를 만드는 클래스 객체.
 * 설정자: 여기서 정적으로 이름만 채우고, switchtec_init() 이 등록한다.
 * 읽는 자: stdev_create() 가 dev->class 에 대입해 udev 가 /dev 노드를
 *         만들 근거를 준다.
 * 값 범위: 이름 "switchtec" 고정.
 * 동기화: 등록 후에는 읽기 전용으로만 쓰인다.
 * ★ EXPORT_SYMBOL_GPL 로 내보내지만 이 트리 안에 참조자가 없다 -
 * 외부 참조자는 NTB 드라이버로 보이나 확인하지 못했다. */
const struct class switchtec_class = {
	/* [한국어] 클래스 이름. 이것이 곧 /sys/class/ 아래의 디렉터리 이름이 된다. */
	.name = "switchtec",
};
/* [한국어] GPL 모듈만 쓸 수 있는 형태로 심볼을 내보낸다. */
EXPORT_SYMBOL_GPL(switchtec_class);

/* [한국어] mrpc_state - 한 fd 의 MRPC 진행 상태.
 * write -> read 로 이어지는 두 단계 프로토콜에서 '지금 어디까지 왔는가'
 * 를 나타내며, 상태에 따라 write/read 가 허용되거나 거절된다. */
enum mrpc_state {
	/* [한국어] MRPC_IDLE - 명령을 낸 적이 없거나 응답을 다 읽고 돌아온 상태.
	 * 설정자: 구조체 0 초기화(kzalloc)와 switchtec_dev_read() 의 마무리.
	 * 읽는 자: switchtec_dev_write() 는 이 상태여야 새 명령을 받고,
	 *         switchtec_dev_read() 는 이 상태면 -EBADE 로 거절한다.
	 * 값 범위: 0(명시적으로 0 을 지정해, 0 초기화가 곧 IDLE 이 되게 했다).
	 * 동기화: mrpc_mutex 아래에서만 읽고 쓴다. */
	MRPC_IDLE = 0,
	/* [한국어] MRPC_QUEUED - 큐에 들어갔으나 아직 하드웨어로 나가지 않은 상태.
	 * 설정자: mrpc_queue_cmd().
	 * 읽는 자: write 가 '비어 있지 않다' 고 판정하는 근거.
	 * 값 범위: 1.
	 * 동기화: mrpc_mutex. */
	MRPC_QUEUED,
	/* [한국어] MRPC_RUNNING - 하드웨어에 명령이 떠 있는 상태.
	 * 설정자: mrpc_cmd_submit().
	 * 읽는 자: read 는 이 상태면 아직 DONE 이 아니므로 기다리거나 -EBADE.
	 * 값 범위: 2.
	 * 동기화: mrpc_mutex. */
	MRPC_RUNNING,
	/* [한국어] MRPC_DONE - 응답이 도착해 읽어 갈 수 있는 상태.
	 * 설정자: mrpc_complete_cmd().
	 * 읽는 자: switchtec_dev_read() 가 이 상태여야 데이터를 넘겨준다.
	 * 값 범위: 3.
	 * 동기화: mrpc_mutex. */
	MRPC_DONE,
	/* [한국어] MRPC_IO_ERROR - 하드웨어가 죽어 완료 처리가 실패한 상태.
	 * 설정자: mrpc_error_complete_cmd()(타임아웃 작업이 펌웨어 정지를 감지).
	 * 읽는 자: switchtec_dev_read() 가 -EIO 를 돌려주는 근거.
	 * 값 범위: 4.
	 * 동기화: mrpc_mutex. */
	MRPC_IO_ERROR,
};

/* [한국어] switchtec_user - 열린 fd 하나의 MRPC 문맥.
 * 장치 하나를 여러 프로세스가 동시에 열 수 있으므로, '어떤 명령을 냈고
 * 응답이 왔는가' 는 장치가 아니라 fd 마다 따로 담아야 한다.
 * kref 로 수명을 관리해, 명령이 큐에 남아 있는 동안 유저가 fd 를 닫아도
 * 구조체가 사라지지 않는다. */
struct switchtec_user {
	/* [한국어] stdev - 이 사용자가 붙어 있는 장치.
	 * 설정자: stuser_create() 가 한 번 채운다(이때 get_device 로 참조를 올린다).
	 * 읽는 자: 거의 모든 진입점이 stuser->stdev 로 장치를 되찾는다.
	 * 값 범위: 유효한 포인터. NULL 이 될 일이 없다.
	 * 동기화: 만들어진 뒤 바뀌지 않으므로 락 없이 읽어도 된다.
	 *         가리키는 장치의 수명은 get_device 로 잡아 두었다. */
	struct switchtec_dev *stdev;

	/* [한국어] state - 위 enum 의 현재 값.
	 * 설정자: stuser_set_state() 를 통해서만 바뀐다.
	 * 읽는 자: write/read 가 진입 가능 여부를 판정한다.
	 * 값 범위: MRPC_IDLE ~ MRPC_IO_ERROR.
	 * 동기화: ★ mrpc_mutex 가 지킨다(헤더의 락 규약에 명시). */
	enum mrpc_state state;

	/* [한국어] cmd_comp - 응답 도착을 기다리는 대기 큐.
	 * 설정자: stuser_create() 가 초기화. 깨우는 곳은 mrpc_cleanup_cmd() 와
	 *         stdev_kill().
	 * 읽는 자: switchtec_dev_read() 의 wait_event_interruptible,
	 *         switchtec_dev_poll() 의 poll_wait.
	 * 값 범위: 대기 큐 헤드.
	 * 동기화: 대기 큐 자체가 내부 스핀락을 갖는다. */
	wait_queue_head_t cmd_comp;
	/* [한국어] kref - 참조 카운트.
	 * 설정자: stuser_create() 가 1 로 시작. mrpc_queue_cmd() 가 큐에 넣으며
	 *         올리고, mrpc_cleanup_cmd()/stdev_kill()/switchtec_dev_release()
	 *         가 내린다.
	 * 읽는 자: kref_put 이 0 이 되는 순간 stuser_free 를 부른다.
	 * 값 범위: 1 이상. 0 이 되면 곧바로 해제된다.
	 * 동기화: kref 는 원자 연산으로 구현되어 별도 락이 필요 없다. */
	struct kref kref;
	/* [한국어] list - 장치의 mrpc_queue 에 매달리는 연결 리스트 노드.
	 * 설정자: mrpc_queue_cmd() 가 list_add_tail 로 붙이고,
	 *         mrpc_cleanup_cmd()/stdev_kill() 이 list_del_init 로 뗀다.
	 * 읽는 자: mrpc_cmd_submit()/mrpc_complete_cmd() 가 큐의 첫 원소를 꺼낼 때.
	 * 값 범위: 큐에 있거나(양쪽 이웃이 유효) 떨어져 있다(자기 자신을 가리킴).
	 * 동기화: ★ mrpc_mutex 가 큐 전체를 지킨다. */
	struct list_head list;

	/* [한국어] cmd_done - '응답이 도착했다' 를 알리는 완료 표시.
	 * 설정자: mrpc_queue_cmd() 가 false 로 내리고, mrpc_cleanup_cmd() 와
	 *         stdev_kill() 이 true 로 올린다.
	 * 읽는 자: read 의 대기 조건, poll 의 EPOLLIN 판정.
	 * 값 범위: true/false.
	 * 동기화: 쓰기는 mrpc_mutex 아래에서 이뤄지고 깨우기가 뒤따른다.
	 *         poll 은 락 없이 읽지만, 값이 순간적으로 낡아도 대기 큐 등록이
	 *         먼저이므로 깨우기를 잃지 않는다. */
	bool cmd_done;
	/* [한국어] cmd - 유저가 낸 MRPC 명령 코드.
	 * 설정자: switchtec_dev_write() 가 유저 버퍼 앞 4 바이트에서 복사.
	 * 읽는 자: 권한 검사(MRPC_CMD_ID 로 하위 16 비트 비교)와
	 *         mrpc_cmd_submit() 의 명령 레지스터 쓰기.
	 * 값 범위: 임의의 32 비트. 드라이버는 의미를 해석하지 않는다.
	 * 동기화: mrpc_mutex. */
	u32 cmd;
	/* [한국어] status - 펌웨어가 보고한 MRPC 처리 상태.
	 * 설정자: mrpc_complete_cmd() 가 DMA 버퍼 또는 MMIO 에서 읽어 채운다.
	 * 읽는 자: switchtec_dev_read() 의 마지막 분기 - DONE/ERROR 면 성공,
	 *         INTERRUPTED 면 -ENXIO, 그 밖이면 -EBADMSG.
	 * 값 범위: SWITCHTEC_MRPC_STATUS_INPROGRESS/DONE/ERROR/INTERRUPTED.
	 * 동기화: mrpc_mutex. */
	u32 status;
	/* [한국어] return_code - 펌웨어가 준 명령별 반환 코드. 커널 errno 와 무관하다.
	 * 설정자: mrpc_complete_cmd() 가 0 으로 초기화한 뒤 채운다.
	 * 읽는 자: switchtec_dev_read() 가 유저 버퍼의 앞 4 바이트로 복사한다.
	 * 값 범위: 0 이면 성공, 그 밖은 명령별 오류 코드.
	 * 동기화: mrpc_mutex. */
	u32 return_code;
	/* [한국어] data_len - 유저가 보낸 페이로드의 길이.
	 * 설정자: switchtec_dev_write() 가 size - 4 로 계산.
	 * 읽는 자: mrpc_cmd_submit() 이 이만큼만 하드웨어에 복사한다.
	 * 값 범위: 0 ~ SWITCHTEC_MRPC_PAYLOAD_SIZE(1024). write 가 검증한다.
	 * 동기화: mrpc_mutex. */
	size_t data_len;
	/* [한국어] read_len - 완료 처리가 응답을 몇 바이트 거둘 것인가.
	 * 설정자: mrpc_queue_cmd() 가 일단 버퍼 전체 크기로 잡고,
	 *         switchtec_dev_read() 가 유저가 요청한 크기로 좁힌다.
	 * 읽는 자: mrpc_complete_cmd() 의 memcpy/memcpy_fromio 길이.
	 * 값 범위: 0 ~ 1024.
	 * 동기화: mrpc_mutex. ★ read 가 락을 풀기 '전에' 정해 두어야
	 *         완료 처리가 올바른 길이를 본다. */
	size_t read_len;
	/* [한국어] data - 명령 페이로드와 응답 데이터가 함께 쓰는 버퍼.
	 * 설정자: write 가 유저 페이로드를 채우고, mrpc_complete_cmd() 가
	 *         응답으로 덮어쓴다.
	 * 읽는 자: mrpc_cmd_submit() 이 하드웨어로 밀어 넣고,
	 *         switchtec_dev_read() 가 유저에게 복사한다.
	 * 값 범위: 1024 바이트 고정. MRPC 프로토콜이 정한 최대 페이로드 크기다.
	 * 동기화: mrpc_mutex. 한 fd 는 한 번에 한 명령만 진행하므로
	 *         입력과 출력이 같은 버퍼를 써도 겹치지 않는다. */
	unsigned char data[SWITCHTEC_MRPC_PAYLOAD_SIZE];
	/* [한국어] event_cnt - 이 fd 가 마지막으로 확인한 장치 이벤트 카운터 값.
	 * 설정자: stuser_create()(open 시점의 값으로 시작),
	 *         ioctl_event_summary()(요약을 읽어 갈 때 갱신).
	 * 읽는 자: switchtec_dev_poll() 이 장치의 현재 카운터와 비교해
	 *         EPOLLPRI 를 올릴지 정한다.
	 * 값 범위: 임의의 int. 절대값이 아니라 '변했는가' 만 의미가 있다.
	 * 동기화: 이 fd 만 쓰는 값이라 별도 락이 없다. 비교 대상인 장치
	 *         카운터는 atomic_t 로 읽는다. */
	int event_cnt;
};

/* [한국어] is_firmware_running - 펌웨어가 아직 살아 응답하는지 확인한다
 * 
 * @stdev: 대상 장치.
 * @return: 살아 있으면 참(0 이 아님), 죽었으면 0.
 * 
 * 아래 영어 주석이 근거를 정확히 밝힌다: device_id 레지스터는 언제나
 * 그 장치의 ID 를 돌려주어야 한다. 다른 값(보통 전 비트 1)이 나온다면
 * 펌웨어가 멎었거나, 펌웨어 리셋으로 BAR 와 Memory Space Enable 비트가
 * 지워져 MMIO 읽기 자체가 무효가 된 것이다.
 * 
 * ★ 이 확인이 필요한 지점은 하나뿐이다 - MRPC 타임아웃 작업. 명령이
 * 500ms 안에 끝나지 않았을 때 '펌웨어가 느린 것인가, 죽은 것인가' 를
 * 가려야 하는데, 그 판단을 이 한 번의 MMIO 읽기로 한다. 죽었다면
 * 더 기다려도 소용없으므로 대기자를 -EIO 로 내보낸다.
 * 
 * 실행 컨텍스트: 워크큐(프로세스 문맥), mrpc_mutex 를 쥔 상태.
 * 
 * 호출 체인:
 *   mrpc_timeout_work() -> [is_firmware_running] -> ioread32() */
/*
 * The MMIO reads to the device_id register should always return the device ID
 * of the device, otherwise the firmware is probably stuck or unreachable
 * due to a firmware reset which clears PCI state including the BARs and Memory
 * Space Enable bits.
 */
static int is_firmware_running(struct switchtec_dev *stdev)
{
	/* [한국어] 시스템 정보 블록의 device_id 를 읽는다. 이 필드는 공용체 바깥에
	 * 있어 세대와 무관하게 같은 위치이므로, 세대 분기 없이 읽을 수 있다. */
	u32 device = ioread32(&stdev->mmio_sys_info->device_id);

	/* [한국어] PCI 열거 때 얻은 장치 ID 와 비교한다. 두 값이 같아야 정상이다. */
	return stdev->pdev->device == device;
}

/* [한국어] stuser_create - 열린 fd 하나를 위한 MRPC 사용자 문맥을 만든다
 * 
 * @stdev: 이 사용자가 붙을 장치.
 * @return: 준비된 stuser 포인터. 메모리 부족이면 ERR_PTR(-ENOMEM).
 * 
 * ★ get_device 로 장치 참조를 올리는 것이 핵심이다. 이렇게 해야
 * 장치가 물리적으로 빠진 뒤에도, 열려 있는 fd 가 남아 있는 한
 * stdev 메모리가 유효하다. 하드웨어 접근은 alive 플래그로 따로
 * 막으므로, '메모리는 살아 있고 하드웨어만 죽은' 상태를 안전하게
 * 다룰 수 있다. 짝은 stuser_free() 의 put_device 다.
 * 
 * event_cnt 를 현재 값으로 시작하는 이유: 이 fd 가 열리기 전에 일어난
 * 이벤트까지 '새 이벤트' 로 보고하면, open 직후의 poll 이 곧바로
 * EPOLLPRI 를 올린다. 지금 값을 기준선으로 삼아 그 뒤의 변화만 본다.
 * 
 * 실행 컨텍스트: 프로세스 문맥(open 경로). GFP_KERNEL 이라 잠들 수 있다.
 * 
 * 호출 체인:
 *   switchtec_dev_open() -> [stuser_create] -> kzalloc_obj()/get_device() */
static struct switchtec_user *stuser_create(struct switchtec_dev *stdev)
{
	/* [한국어] 만들 사용자 문맥. */
	struct switchtec_user *stuser;

	/* [한국어] 구조체 하나 크기를 0 으로 채워 잡는다. 0 초기화 덕분에 state 가
	 * MRPC_IDLE(=0)로, cmd_done 이 false 로 시작한다. kzalloc_obj 는 타입에서
	 * 크기를 뽑아 주므로 sizeof 를 잘못 적는 실수가 원천적으로 없다. */
	stuser = kzalloc_obj(*stuser);
	/* [한국어] 메모리 부족. */
	if (!stuser)
		/* [한국어] 오류를 포인터 모양으로 실어 돌려준다 - 호출자는 IS_ERR 로 판정한다. */
		return ERR_PTR(-ENOMEM);

	/* [한국어] ★ 장치 참조를 올린다 - 이 fd 가 살아 있는 동안 stdev 가
	 * 해제되지 못하게 한다. 짝은 stuser_free 의 put_device 다. */
	get_device(&stdev->dev);
	/* [한국어] 어느 장치에 붙었는지 기억한다. 이 뒤로 바뀌지 않으므로
	 * 다른 진입점들이 락 없이 읽어도 된다. */
	stuser->stdev = stdev;
	/* [한국어] 참조 카운트를 1 로 시작한다 - fd 자신의 몫이다. */
	kref_init(&stuser->kref);
	/* [한국어] 아직 큐에 들어가지 않았으므로 리스트 노드를 자기 자신으로
	 * 초기화해 둔다. 나중에 list_del_init 를 두 번 불러도 안전해진다. */
	INIT_LIST_HEAD(&stuser->list);
	/* [한국어] 응답 도착을 기다릴 대기 큐 초기화. */
	init_waitqueue_head(&stuser->cmd_comp);
	/* [한국어] 현재 이벤트 카운터를 기준선으로 삼는다 - 위 설명의 그 이유다.
	 * 원자적으로 읽어야 ISR 가 동시에 올리는 값과 어긋나지 않는다. */
	stuser->event_cnt = atomic_read(&stdev->event_cnt);

	/* [한국어] 디버그 추적 - 어떤 stuser 가 만들어졌는지 포인터로 남긴다. */
	dev_dbg(&stdev->dev, "%s: %p\n", __func__, stuser);

	/* [한국어] 완성된 사용자 문맥. */
	return stuser;
}

/* [한국어] stuser_free - kref 가 0 이 되었을 때 불리는 해제 함수
 * 
 * @kref: 해제 대상의 kref 멤버. container_of 로 stuser 를 되찾는다.
 * @return: 없음
 * 
 * ★ 직접 부르면 안 된다 - kref_put 이 참조 카운트가 0 이 되었을 때만
 * 불러 주는 콜백이다. 그래서 인자가 stuser 가 아니라 kref 다.
 * 
 * put_device 순서에 유의: stuser 를 kfree 하기 '전에' 장치 참조를
 * 놓는다. 순서를 뒤집으면 이미 해제된 메모리에서 stdev 포인터를
 * 읽게 된다. 그리고 이 put_device 가 마지막 장치 참조였다면
 * stdev_release -> kfree(stdev) 까지 연쇄로 일어난다.
 * 
 * 실행 컨텍스트: 마지막 kref_put 을 부른 쪽의 문맥 - 프로세스 문맥
 * (close 경로)이거나 워크큐(완료 처리 경로)다.
 * 
 * 호출 체인:
 *   kref_put()(stuser_put 또는 stdev_kill 경로) -> [stuser_free]
 *     -> put_device() -> kfree() */
static void stuser_free(struct kref *kref)
{
	/* [한국어] 해제할 사용자 문맥. */
	struct switchtec_user *stuser;

	/* [한국어] kref 멤버 주소에서 바깥 구조체를 역산한다 - 콜백이 kref 포인터만
	 * 넘겨주기 때문이다. */
	stuser = container_of(kref, struct switchtec_user, kref);

	/* [한국어] 디버그 추적. ★ 아래에서 stdev 참조를 놓기 전에 찍어야 한다 -
	 * 그 뒤로는 stdev 가 해제되었을 수 있다. */
	dev_dbg(&stuser->stdev->dev, "%s: %p\n", __func__, stuser);

	/* [한국어] stuser_create 가 올렸던 장치 참조를 놓는다. 이것이 마지막
	 * 참조였다면 stdev_release 를 거쳐 stdev 도 해제된다. */
	put_device(&stuser->stdev->dev);
	/* [한국어] 사용자 문맥 자체를 반납한다. 반드시 위 put_device 뒤여야 한다. */
	kfree(stuser);
}

/* [한국어] stuser_put - 사용자 문맥의 참조를 하나 놓는다
 * 
 * @stuser: 참조를 놓을 사용자 문맥.
 * @return: 없음
 * 
 * kref_put 한 줄을 감싼 래퍼다. 이렇게 이름을 붙여 두면 호출 지점에서
 * '어떤 해제 함수가 불리는가' 를 매번 적지 않아도 되고, 해제 함수를
 * 바꿀 때 한 곳만 고치면 된다.
 * 
 * 호출 지점은 셋이다:
 *   - switchtec_dev_release() : fd 가 닫힐 때
 *   - mrpc_cleanup_cmd()      : 명령이 끝나 큐에서 뺄 때
 *   - stdev_kill()            : 장치가 사라져 큐를 비울 때
 * 마지막 참조였다면 이 안에서 stuser_free 가 불려 메모리가 반납되므로,
 * 호출자는 이 줄 이후로 stuser 를 만지지 않아야 한다.
 * 
 * 실행 컨텍스트: 프로세스 문맥과 워크큐 문맥 양쪽.
 * 
 * 호출 체인:
 *   switchtec_dev_release()/mrpc_cleanup_cmd()/stdev_kill() -> [stuser_put]
 *     -> kref_put() -> (0 이면) stuser_free() */
static void stuser_put(struct switchtec_user *stuser)
{
	/* [한국어] 참조를 내리고, 0 이 되면 stuser_free 를 부르라고 알려 준다. */
	kref_put(&stuser->kref, stuser_free);
}

/* [한국어] stuser_set_state - 사용자 문맥의 상태를 바꾸고 그 전이를 로그에 남긴다
 * 
 * @stuser: 상태를 바꿀 사용자 문맥.
 * @state: 새 상태.
 * @return: 없음
 * 
 * ★ 값 대입 한 줄을 함수로 감싼 이유는 오직 '디버그 추적' 이다.
 * MRPC 는 write -> 큐 -> 하드웨어 -> 인터럽트 -> 워크큐 -> read 로
 * 문맥을 여러 번 갈아타므로, 상태 전이만 따라가도 문제를 절반은
 * 좁힐 수 있다. 대입과 로그를 한데 묶어 두면 어느 경로로 상태가
 * 바뀌어도 기록이 빠지지 않는다.
 * 
 * 락 규약: 아래 영어 주석대로 호출 전에 mrpc_mutex 가 잡혀 있어야 한다.
 * state 는 write/read 경로와 완료 처리 경로가 함께 보는 값이기 때문이다.
 * 
 * 실행 컨텍스트: 프로세스 문맥(유저 경로) 또는 워크큐(완료/타임아웃 경로).
 * 인터럽트 문맥에서는 불리지 않는다.
 * 
 * 호출 체인:
 *   mrpc_queue_cmd()/mrpc_cmd_submit()/mrpc_complete_cmd()/
 *   mrpc_error_complete_cmd()/switchtec_dev_read() -> [stuser_set_state] */
static void stuser_set_state(struct switchtec_user *stuser,
			     enum mrpc_state state)
{
	/* requires the mrpc_mutex to already be held when called */

	/* [한국어] 상태 이름표. static const 라 호출마다 만들어지지 않고 읽기 전용
	 * 섹션에 한 번만 놓인다. 지정 초기화라 enum 값이 바뀌어도 이름이
	 * 어긋나지 않는다. */
	static const char * const state_names[] = {
		/* [한국어] 명령을 낸 적이 없거나 응답을 다 읽고 되돌아온 상태. */
		[MRPC_IDLE] = "IDLE",
		/* [한국어] 큐에 들어갔지만 아직 하드웨어로 보내지 않은 상태. */
		[MRPC_QUEUED] = "QUEUED",
		/* [한국어] 하드웨어에 명령이 떠 있는 상태. */
		[MRPC_RUNNING] = "RUNNING",
		/* [한국어] 응답이 도착해 읽어 갈 수 있는 상태. */
		[MRPC_DONE] = "DONE",
		/* [한국어] 하드웨어가 죽어 완료 처리가 실패한 상태. */
		[MRPC_IO_ERROR] = "IO_ERROR",
	};

	/* [한국어] 실제 상태 갱신. 락 아래에서만 이뤄지므로 read 경로가 중간 값을
	 * 보지 않는다. */
	stuser->state = state;

	/* [한국어] 전이를 추적 로그에 남긴다. 포인터를 함께 찍어 여러 fd 가 동시에
	 * 쓸 때 누구의 전이인지 구분할 수 있게 한다. */
	dev_dbg(&stuser->stdev->dev, "stuser state %p -> %s",
		stuser, state_names[state]);
}

/* [한국어] mrpc_complete_cmd 의 전방 선언. 이 함수의 정의는 아래에 있지만,
 * 그 사이의 함수들이 서로를 부르는 순환 구조(mrpc_cleanup_cmd 가
 * mrpc_cmd_submit 을 부르고, mrpc_complete_cmd 가 mrpc_cleanup_cmd 를
 * 부른다) 때문에 어느 한쪽은 미리 선언해 두어야 한다. */
static void mrpc_complete_cmd(struct switchtec_dev *stdev);

/* [한국어] flush_wc_buf - write-combining 버퍼를 강제로 비운다
 * 
 * @stdev: 대상 장치. mmio_ntb 가 유효해야 한다.
 * @return: 없음
 * 
 * ★ 왜 필요한가(이 드라이버에서 가장 하드웨어에 가까운 부분):
 * switchtec_init_pci() 는 MRPC 영역(GAS 0x0~0x1000)을 write-combining 으로
 * 매핑한다. WC 매핑에서 쓰기는 프로세서 안의 버퍼에 모였다가 한꺼번에
 * 나가므로, 여러 쓰기의 순서가 뒤바뀌거나 도착이 지연될 수 있다.
 * 그런데 MRPC 는 '입력 데이터를 다 채운 뒤 명령 레지스터를 쓴다' 는
 * 순서가 절대적이다. 명령이 먼저 도착하면 장치는 아직 채워지지 않은
 * 입력을 읽는다.
 * 
 * ★ 왜 '읽기' 로 비우는가: WC 버퍼는 같은 영역을 읽으면 강제로 비워진다.
 * 문제는 MRPC 영역을 읽으면 부작용이 있을 수 있다는 점인데, 아래
 * 영어 주석이 밝히듯 NTB 의 outbound doorbell(odb) 레지스터는 읽어도
 * 부작용이 없고 하드웨어가 저지연으로 처리해 준다. 그래서 전혀 다른
 * 블록(NTB)의 레지스터를 '버퍼 비우기용' 으로 빌려 쓴다.
 * 
 * 실행 컨텍스트: 프로세스 문맥과 워크큐 문맥에서 불린다. MMIO 읽기
 * 하나뿐이라 잠들지 않는다.
 * 
 * 호출 체인:
 *   mrpc_cmd_submit()/enable_dma_mrpc()/switchtec_exit_pci()
 *     -> [flush_wc_buf] -> ioread32() */
static void flush_wc_buf(struct switchtec_dev *stdev)
{
	/* [한국어] NTB 도어벨/메시지 레지스터 블록의 포인터. */
	struct ntb_dbmsg_regs __iomem *mmio_dbmsg;

	/*
	 * odb (outbound doorbell) register is processed by low latency
	 * hardware and w/o side effect
	 */
	/* [한국어] NTB 블록 시작(GAS 0x10000)에서 도어벨/메시지 영역(0x64000)까지
	 * 더한다. void __iomem * 로 캐스팅해 바이트 단위로 더한다. */
	mmio_dbmsg = (void __iomem *)stdev->mmio_ntb +
		SWITCHTEC_NTB_REG_DBMSG_OFFSET;
	/* [한국어] 읽기 자체가 목적이다 - 반환값을 쓰지 않는다. 이 읽기가
	 * WC 버퍼를 비우는 부작용을 낸다. */
	ioread32(&mmio_dbmsg->odb);
}

/* [한국어] mrpc_cmd_submit - 큐 맨 앞의 명령을 하드웨어에 실제로 띄운다
 * 
 * @stdev: 대상 장치.
 * @return: 없음
 * 
 * ★ 이 함수가 MRPC 직렬화의 심장이다. 하드웨어의 MRPC 창구는 하나뿐이라
 * 한 번에 한 명령만 처리할 수 있다. 그래서 이 함수는 두 조건을 확인한다:
 *   - mrpc_busy 가 0 인가(하드웨어가 한가한가)
 *   - 큐에 대기자가 있는가
 * 둘 다 참일 때만 명령을 띄운다. 이 함수는 명령을 낼 때(mrpc_queue_cmd)와
 * 명령이 끝났을 때(mrpc_cleanup_cmd) 양쪽에서 불리므로, '한가해지면
 * 자동으로 다음 명령이 나간다' 는 흐름이 만들어진다.
 * 
 * DMA MRPC 를 쓸 때 응답 버퍼를 미리 더럽히는 이유: 장치가 응답을
 * 써 넣기 전에는 그 자리에 이전 명령의 응답이 남아 있다. 상태를
 * INPROGRESS 로 되돌리고 데이터를 0xFF 로 덮어 두면, 장치가 실제로
 * 쓰기 전에 완료 처리가 돌더라도 옛 응답을 새 응답으로 착각하지 않는다.
 * 
 * 락 규약: mrpc_mutex 가 잡혀 있어야 한다.
 * 
 * 실행 컨텍스트: 프로세스 문맥(write 경로) 또는 워크큐(완료 처리 경로).
 * 
 * 호출 체인:
 *   mrpc_queue_cmd() / mrpc_cleanup_cmd() -> [mrpc_cmd_submit]
 *     -> memcpy_toio() -> flush_wc_buf() -> iowrite32() */
static void mrpc_cmd_submit(struct switchtec_dev *stdev)
{
	/* requires the mrpc_mutex to already be held when called */

	/* [한국어] 띄울 명령의 주인. */
	struct switchtec_user *stuser;

	/* [한국어] 이미 하드웨어에 명령이 떠 있으면 */
	if (stdev->mrpc_busy)
		/* [한국어] 아무것도 하지 않는다 - 그 명령이 끝날 때 다시 불린다. */
		return;

	/* [한국어] 대기자가 없으면 */
	if (list_empty(&stdev->mrpc_queue))
		/* [한국어] 역시 아무것도 하지 않는다. */
		return;

	/* [한국어] 큐의 첫 원소를 꺼낸다(꺼내기만 하고 빼지는 않는다 - 완료될 때까지
	 * 큐에 남아 있어야 mrpc_complete_cmd 가 찾을 수 있다). FIFO 순서라
	 * 먼저 낸 명령이 먼저 처리된다. */
	stuser = list_entry(stdev->mrpc_queue.next, struct switchtec_user,
			    list);

	/* [한국어] DMA MRPC 를 쓰는 구성이면. */
	if (stdev->dma_mrpc) {
		/* [한국어] 응답 버퍼의 상태를 '진행 중' 으로 되돌려 둔다. */
		stdev->dma_mrpc->status = SWITCHTEC_MRPC_STATUS_INPROGRESS;
		/* [한국어] 데이터 영역을 0xFF 로 채운다 - 장치가 덮어쓰기 전의 값이
		 * 이전 응답으로 오인되지 않게 하는 표식이다. */
		memset(stdev->dma_mrpc->data, 0xFF, SWITCHTEC_MRPC_PAYLOAD_SIZE);
	}

	/* [한국어] 상태를 RUNNING 으로 - 이제 하드웨어의 손에 넘어간다. */
	stuser_set_state(stuser, MRPC_RUNNING);
	/* [한국어] 하드웨어가 바쁘다고 표시. 이 값이 0 으로 돌아오기 전까지
	 * 다음 명령은 큐에서 기다린다. */
	stdev->mrpc_busy = 1;
	/* [한국어] 페이로드를 MRPC 입력 영역에 복사한다. WC 매핑이라 이 쓰기들은
	 * 프로세서 버퍼에 모인다. memcpy_toio 는 MMIO 전용 복사다. */
	memcpy_toio(&stdev->mmio_mrpc->input_data,
		    stuser->data, stuser->data_len);
	/* [한국어] ★ 위 쓰기가 모두 하드웨어에 도달하도록 WC 버퍼를 비운다.
	 * 이 줄이 없으면 아래 명령 쓰기가 먼저 도착할 수 있다. */
	flush_wc_buf(stdev);
	/* [한국어] 명령 레지스터에 명령 코드를 쓴다 - 이 쓰기가 장치를 깨우는
	 * 방아쇠다. 여기서부터 펌웨어가 명령을 처리하기 시작한다. */
	iowrite32(stuser->cmd, &stdev->mmio_mrpc->cmd);

	/* [한국어] 500ms 뒤에 타임아웃 작업을 예약한다. 완료 인터럽트를 놓치거나
	 * 펌웨어가 멎어도 사용자가 영원히 기다리지 않게 하는 안전망이다.
	 * 정상 완료 시에는 mrpc_event_work 가 이 예약을 취소한다. */
	schedule_delayed_work(&stdev->mrpc_timeout,
			      msecs_to_jiffies(500));
}

/* [한국어] mrpc_queue_cmd - 명령을 대기 큐에 넣고 가능하면 곧바로 띄운다
 * 
 * @stuser: 명령이 채워진 사용자 문맥(cmd, data, data_len 이 이미 유효).
 * @return: 항상 0. int 를 돌려주는 것은 장래 확장을 위한 자리로 보이며,
 *          호출자(switchtec_dev_write)는 이 값을 rc 로 받아 그대로 쓴다.
 * 
 * ★ kref_get 이 핵심이다. 명령이 큐에 있는 동안 유저가 fd 를 닫아도
 * stuser 가 사라지면 안 된다 - 완료 처리가 그 포인터를 통해 응답을
 * 쓰려 하기 때문이다. 그래서 큐에 넣을 때 참조를 하나 올리고,
 * 큐에서 뺄 때(mrpc_cleanup_cmd / stdev_kill) 놓는다.
 * 
 * read_len 을 일단 최대치로 잡는 이유: 아직 유저가 몇 바이트를 읽을지
 * 모른다. switchtec_dev_read 가 나중에 실제 요청 크기로 좁힌다.
 * 그 사이에 완료 처리가 돌면 최대치만큼 복사되는데, stuser->data 가
 * 그만한 크기라 넘치지 않는다.
 * 
 * 락 규약: mrpc_mutex 가 잡혀 있어야 한다.
 * 
 * 실행 컨텍스트: 프로세스 문맥(write 경로).
 * 
 * 호출 체인:
 *   switchtec_dev_write() -> [mrpc_queue_cmd] -> mrpc_cmd_submit() */
static int mrpc_queue_cmd(struct switchtec_user *stuser)
{
	/* requires the mrpc_mutex to already be held when called */

	/* [한국어] 이 사용자가 붙어 있는 장치. */
	struct switchtec_dev *stdev = stuser->stdev;

	/* [한국어] ★ 큐가 이 stuser 를 참조하게 되므로 참조 카운트를 올린다. */
	kref_get(&stuser->kref);
	/* [한국어] 읽을 길이를 일단 버퍼 전체로. read 가 나중에 좁힌다. */
	stuser->read_len = sizeof(stuser->data);
	/* [한국어] 상태를 QUEUED 로. 이 시점부터 write 는 -EBADE 로 거절된다. */
	stuser_set_state(stuser, MRPC_QUEUED);
	/* [한국어] 완료 표시를 내린다 - read 의 대기 조건이 거짓에서 시작해야 한다. */
	stuser->cmd_done = false;
	/* [한국어] 큐 끝에 붙인다. FIFO 순서 보장. */
	list_add_tail(&stuser->list, &stdev->mrpc_queue);

	/* [한국어] 하드웨어가 한가하면 곧바로 띄운다. 바쁘면 아무 일도 하지 않고,
	 * 앞 명령이 끝날 때 자동으로 이 명령이 나간다. */
	mrpc_cmd_submit(stdev);

	/* [한국어] 현재 구현은 실패할 수 없다. */
	return 0;
}

/* [한국어] mrpc_cleanup_cmd - 끝난 명령을 큐에서 빼고 대기자를 깨운 뒤 다음 명령을 띄운다
 * 
 * @stdev: 대상 장치. 큐가 비어 있지 않다고 전제한다 - 호출자가 이미
 *         확인했기 때문에 여기서는 검사하지 않는다.
 * @return: 없음
 * 
 * 정상 완료(mrpc_complete_cmd)와 오류 완료(mrpc_error_complete_cmd)가
 * 공통으로 부르는 마무리 함수다. 하는 일은 네 가지:
 *   1) cmd_done 을 세워 read 의 대기 조건을 참으로 만든다
 *   2) 기다리던 사용자를 깨운다
 *   3) 큐에서 빼고 참조를 놓는다
 *   4) 하드웨어를 한가하다고 표시하고 다음 명령을 띄운다
 * 
 * ★ (1) 과 (2) 의 순서가 중요하다. 먼저 깨우고 나중에 표시하면,
 * 깨어난 쪽이 조건을 거짓으로 보고 도로 잠들어 깨우기를 잃는다.
 * 
 * ★ (3) 의 참조 놓기가 stuser 를 해제할 수도 있다. 유저가 이미 fd 를
 * 닫았다면 여기가 마지막 참조다. 그래서 이 줄 뒤로 stuser 를 만지지
 * 않는다 - 실제로 아래는 stdev 만 건드린다.
 * 
 * 락 규약: mrpc_mutex 가 잡혀 있어야 한다.
 * 
 * 실행 컨텍스트: 워크큐(완료/타임아웃 경로).
 * 
 * 호출 체인:
 *   mrpc_complete_cmd() / mrpc_error_complete_cmd() -> [mrpc_cleanup_cmd]
 *     -> wake_up_interruptible() -> stuser_put() -> mrpc_cmd_submit() */
static void mrpc_cleanup_cmd(struct switchtec_dev *stdev)
{
	/* requires the mrpc_mutex to already be held when called */

	/* [한국어] 큐의 첫 원소 - 방금 끝난 그 명령의 주인이다. */
	struct switchtec_user *stuser = list_entry(stdev->mrpc_queue.next,
						   struct switchtec_user, list);

	/* [한국어] (1) 완료 표시. read 의 wait_event 조건이 여기서 참이 된다. */
	stuser->cmd_done = true;
	/* [한국어] (2) 기다리던 read 를 깨운다. poll 도 이 큐에 걸려 있어 함께 깨어난다. */
	wake_up_interruptible(&stuser->cmd_comp);
	/* [한국어] (3) 큐에서 빼고 노드를 자기 자신으로 재초기화한다. */
	list_del_init(&stuser->list);
	/* [한국어] 큐가 쥐고 있던 참조를 놓는다. 마지막 참조였다면 여기서 해제된다 -
	 * 그래서 이 줄 이후로 stuser 를 만지지 않는다. */
	stuser_put(stuser);
	/* [한국어] (4) 하드웨어가 한가해졌다. */
	stdev->mrpc_busy = 0;

	/* [한국어] 다음 대기자가 있으면 곧바로 띄운다. 이 재귀적 호출이 큐를
	 * 계속 흘러가게 하는 동력이다(재귀는 한 단계뿐 - 띄운 명령의 완료는
	 * 다음 인터럽트에서 처리된다). */
	mrpc_cmd_submit(stdev);
}

/* [한국어] mrpc_complete_cmd - 하드웨어/DMA 에서 응답을 거둬 사용자 버퍼에 옮긴다
 * 
 * @stdev: 대상 장치.
 * @return: 없음
 * 
 * ★ 이 함수가 MRPC 응답 경로의 본체다. 인터럽트가 아니라 워크큐에서
 * 도는 이유는 (a) mrpc_mutex 가 잠들 수 있는 락이고 (b) 최대 1KB 의
 * memcpy_fromio 가 인터럽트 문맥에 두기 무겁기 때문이다.
 * 
 * 동작 단계:
 *   1) 큐가 비었으면 할 일이 없다(가짜 인터럽트나 이미 처리된 경우)
 *   2) 상태를 읽는다 - DMA 를 쓰면 호스트 메모리에서, 아니면 MMIO 에서
 *   3) 아직 진행 중이면 그대로 돌아간다 - 이 경우 명령은 큐에 남고
 *      타임아웃 작업이 계속 지켜본다
 *   4) 상태를 DONE 으로 바꾸고 반환 코드와 데이터를 거둔다
 *   5) 어떤 경로로 끝났든 mrpc_cleanup_cmd 로 마무리한다
 * 
 * ★ 데이터를 복사하지 않고 건너뛰는 두 경우:
 *   - 상태가 DONE/ERROR 가 아니다(INTERRUPTED 등) -> 응답이 없다
 *   - 펌웨어 반환 코드가 0 이 아니다 -> 명령이 실패했으므로 데이터가 없다
 * 어느 쪽이든 반환 코드는 유저에게 전달되므로 실패 사유는 알 수 있다.
 * 
 * 락 규약: mrpc_mutex 가 잡혀 있어야 한다.
 * 
 * 실행 컨텍스트: 워크큐(mrpc_event_work, mrpc_timeout_work).
 * 
 * 호출 체인:
 *   mrpc_event_work() / mrpc_timeout_work() -> [mrpc_complete_cmd]
 *     -> mrpc_cleanup_cmd() */
static void mrpc_complete_cmd(struct switchtec_dev *stdev)
{
	/* requires the mrpc_mutex to already be held when called */

	/* [한국어] 응답을 받을 사용자. */
	struct switchtec_user *stuser;

	/* [한국어] 큐가 비었다 - 인터럽트가 늦게 도착했거나 stdev_kill 이 이미
	 * 큐를 비운 경우다. */
	if (list_empty(&stdev->mrpc_queue))
		/* [한국어] 할 일이 없다. */
		return;

	/* [한국어] 큐의 첫 원소가 지금 하드웨어에 떠 있는 명령의 주인이다. */
	stuser = list_entry(stdev->mrpc_queue.next, struct switchtec_user,
			    list);

	/* [한국어] DMA MRPC 를 쓰면 상태도 호스트 메모리에 온다. */
	if (stdev->dma_mrpc)
		/* [한국어] 코히런트 버퍼라 캐시 무효화 없이 그냥 읽으면 된다. */
		stuser->status = stdev->dma_mrpc->status;
	/* [한국어] 아니면 MMIO 상태 레지스터를 읽는다. */
	else
		/* [한국어] 이쪽은 uncached 매핑 구간이므로 매 읽기가 하드웨어에 닿는다. */
		stuser->status = ioread32(&stdev->mmio_mrpc->status);

	/* [한국어] 아직 처리 중이다 - 인터럽트가 다른 이유로 왔거나, 타임아웃
	 * 작업이 확인차 들어온 경우다. */
	if (stuser->status == SWITCHTEC_MRPC_STATUS_INPROGRESS)
		/* [한국어] 명령을 큐에 그대로 두고 물러난다. 타임아웃 작업이 다시 온다. */
		return;

	/* [한국어] 여기부터는 어떤 형태로든 끝난 것이다. 상태를 DONE 으로 바꾼다 -
	 * read 는 이 상태여야 데이터를 준다. */
	stuser_set_state(stuser, MRPC_DONE);
	/* [한국어] 반환 코드를 0 으로 초기화한다. 아래에서 거두지 못하고 빠져나가는
	 * 경로가 있으므로, 쓰레기 값이 유저에게 가지 않도록 미리 정해 둔다. */
	stuser->return_code = 0;

	/* [한국어] 정상 완료도 아니고 펌웨어 오류도 아니라면(INTERRUPTED 등) */
	if (stuser->status != SWITCHTEC_MRPC_STATUS_DONE &&
	    /* [한국어] 응답 자체가 없다. */
	    stuser->status != SWITCHTEC_MRPC_STATUS_ERROR)
		/* [한국어] 반환 코드도 데이터도 거두지 않고 마무리로 간다. read 는
		 * stuser->status 를 보고 -ENXIO 나 -EBADMSG 를 돌려준다. */
		goto out;

	/* [한국어] DMA 를 쓰면 반환 코드도 호스트 메모리에 있다. */
	if (stdev->dma_mrpc)
		/* [한국어] 펌웨어가 준 명령별 반환 코드 - 커널 errno 와는 다른 값이다. */
		stuser->return_code = stdev->dma_mrpc->rtn_code;
	/* [한국어] 아니면 MMIO 에서. */
	else
		/* [한국어] ret_value 레지스터를 읽는다. */
		stuser->return_code = ioread32(&stdev->mmio_mrpc->ret_value);
	/* [한국어] 펌웨어가 명령을 실패로 끝냈으면 */
	if (stuser->return_code != 0)
		/* [한국어] 출력 데이터가 없으므로 복사를 건너뛴다. 반환 코드는 이미
		 * 채워져 있어 유저가 사유를 알 수 있다. */
		goto out;

	/* [한국어] DMA 를 쓰면 응답 데이터도 호스트 메모리에 있다. */
	if (stdev->dma_mrpc)
		/* [한국어] 평범한 memcpy 로 충분하다 - 코히런트 버퍼이기 때문이다.
		 * read_len 은 유저가 요청한 크기이거나(read 가 이미 다녀갔다면)
		 * 버퍼 전체 크기다(아직이라면). */
		memcpy(stuser->data, &stdev->dma_mrpc->data,
			      stuser->read_len);
	/* [한국어] 아니면 MMIO 출력 영역에서. */
	else
		/* [한국어] memcpy_fromio 로 읽어 온다 - 최대 1KB 의 MMIO 읽기라
		 * 이 함수를 인터럽트 문맥에 두지 않는 주된 이유다. */
		memcpy_fromio(stuser->data, &stdev->mmio_mrpc->output_data,
			      stuser->read_len);
/* [한국어] 데이터를 거뒀든 건너뛰었든 함께 지나가는 마무리 지점. */
out:
	/* [한국어] 큐에서 빼고 대기자를 깨우고 다음 명령을 띄운다. */
	mrpc_cleanup_cmd(stdev);
}

/* [한국어] mrpc_event_work - MRPC 완료 인터럽트를 프로세스 문맥에서 마무리한다
 * 
 * @work: stdev->mrpc_work.
 * @return: 없음
 * 
 * ISR 가 schedule_work 로 띄운다. 하는 일은 셋뿐이다 - 락을 잡고,
 * 타임아웃 예약을 취소하고, 완료 처리를 부른다.
 * 
 * ★ cancel_delayed_work(_sync 가 아니다)를 쓰는 이유: _sync 판은
 * 이미 실행 중인 작업이 끝나기를 기다리는데, 그 작업(mrpc_timeout_work)이
 * 지금 내가 쥐고 있는 mrpc_mutex 를 잡으려 하고 있을 수 있다.
 * 그러면 서로를 기다리는 교착이 된다. 비동기 판은 '아직 시작하지
 * 않았으면 취소' 만 하고 돌아오므로 안전하다. 이미 실행 중이었다면
 * 그쪽도 같은 mrpc_complete_cmd 를 부르지만, 락 덕분에 순차로 실행되고
 * 두 번째 호출은 큐가 비었거나 상태가 이미 바뀐 것을 보고 조용히 끝난다.
 * 
 * guard(mutex) 를 쓰는 이유: 함수가 어디로 빠져나가든 락이 자동으로
 * 풀린다 - 이 함수는 빠져나가는 길이 하나뿐이지만, 실수를 원천적으로
 * 막는 최신 커널 관용구다.
 * 
 * 실행 컨텍스트: 시스템 워크큐의 커널 스레드(프로세스 문맥).
 * 
 * 호출 체인:
 *   switchtec_event_isr() / switchtec_dma_mrpc_isr() -> schedule_work()
 *     -> [mrpc_event_work] -> mrpc_complete_cmd() */
static void mrpc_event_work(struct work_struct *work)
{
	/* [한국어] 이 작업을 품은 장치. */
	struct switchtec_dev *stdev;

	/* [한국어] 멤버 포인터에서 바깥 구조체를 역산한다. */
	stdev = container_of(work, struct switchtec_dev, mrpc_work);

	/* [한국어] 디버그 추적. */
	dev_dbg(&stdev->dev, "%s\n", __func__);

	/* [한국어] 블록을 벗어날 때 자동으로 풀리는 락 획득. */
	guard(mutex)(&stdev->mrpc_mutex);
	/* [한국어] 타임아웃 예약 취소 - 위에 적은 이유로 비동기 판이어야 한다. */
	cancel_delayed_work(&stdev->mrpc_timeout);
	/* [한국어] 응답을 거두고 대기자를 깨운다. */
	mrpc_complete_cmd(stdev);
}

/* [한국어] mrpc_error_complete_cmd - 하드웨어가 죽었을 때 명령을 오류로 끝낸다
 * 
 * @stdev: 대상 장치.
 * @return: 없음
 * 
 * 정상 완료와 달리 응답을 전혀 읽지 않는다 - 읽을 하드웨어가 없기
 * 때문이다. 상태만 MRPC_IO_ERROR 로 바꾸고 마무리한다. 깨어난 read 는
 * 그 상태를 보고 -EIO 를 돌려준다.
 * 
 * 이 경로로 오는 유일한 조건은 타임아웃 작업에서 is_firmware_running()
 * 이 거짓인 경우다 - 즉 device_id 레지스터가 기대한 값을 돌려주지
 * 않는 상황으로, 펌웨어 리셋으로 BAR 와 Memory Space Enable 이
 * 지워졌거나 장치가 멎은 경우다.
 * 
 * 락 규약: mrpc_mutex 가 잡혀 있어야 한다.
 * 
 * 실행 컨텍스트: 워크큐(mrpc_timeout_work).
 * 
 * 호출 체인:
 *   mrpc_timeout_work() -> [mrpc_error_complete_cmd] -> mrpc_cleanup_cmd() */
static void mrpc_error_complete_cmd(struct switchtec_dev *stdev)
{
	/* requires the mrpc_mutex to already be held when called */

	/* [한국어] 오류로 끝낼 명령의 주인. */
	struct switchtec_user *stuser;

	/* [한국어] 큐가 비었으면 */
	if (list_empty(&stdev->mrpc_queue))
		/* [한국어] 할 일이 없다. */
		return;

	/* [한국어] 큐의 첫 원소. */
	stuser = list_entry(stdev->mrpc_queue.next,
			    struct switchtec_user, list);

	/* [한국어] 상태를 IO_ERROR 로. read 가 이 값을 보고 -EIO 를 돌려준다. */
	stuser_set_state(stuser, MRPC_IO_ERROR);

	/* [한국어] 응답 없이 마무리한다 - 대기자를 깨우고 큐에서 빼고
	 * 다음 명령을 띄운다. 다음 명령도 하드웨어가 죽어 있으면
	 * 곧 같은 경로로 끝난다. */
	mrpc_cleanup_cmd(stdev);
}

/* [한국어] mrpc_timeout_work - 500ms 마다 명령이 끝났는지 확인하는 안전망
 * 
 * @work: stdev->mrpc_timeout.work.
 * @return: 없음
 * 
 * ★ 왜 필요한가: 완료 인터럽트만 믿을 수 없다. 인터럽트를 놓칠 수도,
 * 펌웨어가 멎어 아예 오지 않을 수도 있다. 그러면 사용자는
 * wait_event_interruptible 에서 영원히 깨어나지 않는다.
 * 이 작업이 500ms 마다 깨어나 세 갈래로 판단한다:
 *   1) 펌웨어가 죽었다 -> 오류로 끝낸다(-EIO)
 *   2) 아직 진행 중이다 -> 500ms 뒤로 다시 예약하고 물러난다
 *   3) 끝났다(인터럽트를 놓쳤다) -> 정상 완료 처리를 한다
 * 
 * 즉 이 드라이버는 인터럽트와 폴링을 함께 쓴다 - 인터럽트가 오면
 * 빠르고, 오지 않아도 500ms 안에 폴링이 알아챈다.
 * 
 * ★ mutex_lock(_interruptible 이 아니다)을 쓰는 이유: 워크큐 스레드는
 * 시그널을 받지 않으므로 중단될 일이 없고, 중단되면 오히려 락을
 * 못 잡아 안전망이 무너진다.
 * 
 * 실행 컨텍스트: 시스템 워크큐의 커널 스레드(프로세스 문맥).
 * 
 * 호출 체인:
 *   schedule_delayed_work()(mrpc_cmd_submit 이 예약) -> [mrpc_timeout_work]
 *     -> is_firmware_running() -> mrpc_error_complete_cmd()/mrpc_complete_cmd() */
static void mrpc_timeout_work(struct work_struct *work)
{
	/* [한국어] 이 작업을 품은 장치. */
	struct switchtec_dev *stdev;
	/* [한국어] 읽어 온 MRPC 상태. */
	u32 status;

	/* [한국어] delayed_work 는 안에 work_struct 를 품고 있으므로 .work 를
	 * 거쳐 역산해야 한다. */
	stdev = container_of(work, struct switchtec_dev, mrpc_timeout.work);

	/* [한국어] 디버그 추적 - 타임아웃이 돌았다는 사실 자체가 정보다. */
	dev_dbg(&stdev->dev, "%s\n", __func__);

	/* [한국어] 락을 잡는다. 시그널로 중단되지 않는 판이어야 한다. */
	mutex_lock(&stdev->mrpc_mutex);

	/* [한국어] 1) 펌웨어가 살아 있는지 - device_id 레지스터를 읽어 기대한
	 * 값이 나오는지로 판정한다. */
	if (!is_firmware_running(stdev)) {
		/* [한국어] 죽었으면 명령을 -EIO 로 끝낸다. */
		mrpc_error_complete_cmd(stdev);
		/* [한국어] 락을 풀어야 하므로 라벨로. */
		goto out;
	}

	/* [한국어] 상태를 읽는다 - DMA 를 쓰면 호스트 메모리에서. */
	if (stdev->dma_mrpc)
		/* [한국어] 코히런트 버퍼에서 직접. */
		status = stdev->dma_mrpc->status;
	/* [한국어] 아니면 MMIO 에서. */
	else
		/* [한국어] 상태 레지스터를 읽는다. */
		status = ioread32(&stdev->mmio_mrpc->status);
	/* [한국어] 2) 아직 진행 중이면 */
	if (status == SWITCHTEC_MRPC_STATUS_INPROGRESS) {
		/* [한국어] 500ms 뒤로 다시 예약하고 */
		schedule_delayed_work(&stdev->mrpc_timeout,
				      msecs_to_jiffies(500));
		/* [한국어] 물러난다. 이 되풀이는 명령이 끝나거나 펌웨어가 죽을 때까지
		 * 이어지며, 상한이 따로 없다는 점에 유의 - 펌웨어가 살아 있는 한
		 * 계속 기다린다. */
		goto out;
	}

	/* [한국어] 3) 끝났는데 인터럽트를 놓친 경우 - 정상 완료 처리를 한다. */
	mrpc_complete_cmd(stdev);
/* [한국어] 세 갈래가 함께 지나가는 정리 지점. */
out:
	/* [한국어] 락을 푼다. */
	mutex_unlock(&stdev->mrpc_mutex);
}

/* [한국어] device_version_show - sysfs 의 device_version 파일을 읽을 때 불린다
 * 
 * @dev: 이 속성이 붙은 device. to_stdev 로 switchtec_dev 를 얻는다.
 * @attr: 어느 속성인지(하나뿐이라 쓰지 않는다).
 * @buf: 커널이 준 PAGE_SIZE 크기의 출력 버퍼.
 * @return: 채운 바이트 수. sysfs 코어가 그만큼을 유저에게 준다.
 * 
 * sysfs 속성 읽기는 락을 잡지 않는다는 점에 유의 - MMIO 를 한 번 읽어
 * 그대로 찍을 뿐이라 다른 상태를 건드리지 않고, 값이 순간적으로
 * 낡아도 무해하기 때문이다. 다만 장치가 제거되는 중이라면 사라진 매핑을
 * 읽을 수 있는데, sysfs 코어가 device_del 시점에 속성 파일을 먼저
 * 제거하고 진행 중인 읽기가 끝나기를 기다리므로 그 창은 닫혀 있다.
 * 
 * 실행 컨텍스트: 프로세스 문맥(cat /sys/class/switchtec/...).
 * 
 * 호출 체인:
 *   (유저스페이스) read(/sys/.../device_version) -> sysfs -> [device_version_show] */
static ssize_t device_version_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	/* [한국어] embedded device 에서 바깥 구조체를 역산하는 인라인 헬퍼. */
	struct switchtec_dev *stdev = to_stdev(dev);
	/* [한국어] 읽어 올 값. */
	u32 ver;

	/* [한국어] 시스템 정보 블록의 device_version 레지스터. 이 필드는 공용체 바깥에
	 * 있어 세대와 무관하게 같은 위치다. */
	ver = ioread32(&stdev->mmio_sys_info->device_version);

	/* [한국어] 16진수로 찍는다. sysfs_emit 은 버퍼 오버플로를 막아 주는
	 * sysfs 전용 snprintf 판이다. */
	return sysfs_emit(buf, "%x\n", ver);
}
/* [한국어] 읽기 전용 속성 dev_attr_device_version 을 만든다. 이름에서
 * device_version_show 를 자동으로 찾아 연결하므로 함수 이름이
 * 정확히 '<속성이름>_show' 여야 한다. */
static DEVICE_ATTR_RO(device_version);

/* [한국어] fw_version_show - sysfs 의 fw_version 파일 읽기 핸들러
 * 
 * @dev: 이 속성이 붙은 device.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 채운 바이트 수.
 * 
 * 펌웨어 버전을 8자리 16진수로 고정 폭 출력한다. 고정 폭인 이유는
 * 유저스페이스 도구가 자릿수를 세어 필드를 나누기 때문으로 보이며,
 * device_version 이 가변 폭인 것과 대비된다.
 * 
 * 실행 컨텍스트: 프로세스 문맥.
 * 
 * 호출 체인:
 *   (유저스페이스) read(/sys/.../fw_version) -> sysfs -> [fw_version_show] */
static ssize_t fw_version_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	/* [한국어] 장치 객체. */
	struct switchtec_dev *stdev = to_stdev(dev);
	/* [한국어] 읽어 올 값. */
	u32 ver;

	/* [한국어] 공용체 바깥의 firmware_version 레지스터 - 세대 무관. */
	ver = ioread32(&stdev->mmio_sys_info->firmware_version);

	/* [한국어] 앞을 0 으로 채운 8자리 16진수. */
	return sysfs_emit(buf, "%08x\n", ver);
}
/* [한국어] dev_attr_fw_version 생성. */
static DEVICE_ATTR_RO(fw_version);

/* [한국어] io_string_show - MMIO 에 박힌 고정 길이 문자열을 sysfs 로 찍는다
 * 
 * @buf: sysfs 출력 버퍼(PAGE_SIZE).
 * @attr: 읽어 올 MMIO 주소.
 * @len: 그 필드의 바이트 길이.
 * @return: 실제로 채운 문자열 길이.
 * 
 * ★ 왜 특별한 처리가 필요한가: 이 문자열들은 하드웨어 레지스터에
 * '고정 길이 필드' 로 박혀 있다. 예컨대 vendor_id 는 8 바이트 자리인데
 * 실제 내용이 "MICROSEM" 처럼 꽉 찰 수도, "PMC     " 처럼 뒤가 공백으로
 * 채워질 수도 있다. NUL 종료가 보장되지 않으므로 그냥 %s 로 찍으면
 * 버퍼를 넘어 읽는다.
 * 
 * 동작:
 *   1) 필드 전체를 그대로 복사한다(memcpy_fromio - MMIO 는 일반 memcpy 로
 *      읽으면 안 되고 전용 함수를 써야 한다)
 *   2) 그 뒤에 개행과 NUL 을 붙여 일단 완전한 문자열로 만든다
 *   3) 뒤에서부터 훑으며 공백을 만나는 동안 그 자리를 개행+NUL 로 덮는다
 *      - 즉 오른쪽 공백을 잘라 내는(rstrip) 동작이다
 *   4) 최종 길이를 돌려준다
 * 
 * 버퍼 안전성: buf 는 PAGE_SIZE(보통 4096)이고 len 은 최대 24 바이트
 * (product_id 의 gen4 판)이므로 len + 2 를 써도 넘칠 여지가 없다.
 * 
 * 경계 주의: 루프 조건이 i > 0 이라 0 번 바이트는 검사하지 않는다.
 * 필드 전체가 공백이면 빈 문자열이 아니라 "<공백>\n" 이 남는다.
 * 
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡지 않는다.
 * 
 * 호출 체인:
 *   DEVICE_ATTR_SYS_INFO_STR 로 만들어진 show 함수들, component_vendor_show
 *     -> [io_string_show] -> memcpy_fromio() */
static ssize_t io_string_show(char *buf, void __iomem *attr, size_t len)
{
	/* [한국어] 뒤에서부터 훑을 인덱스. */
	int i;

	/* [한국어] MMIO 전용 복사. 일반 memcpy 는 컴파일러가 최적화로 접근 크기나
	 * 순서를 바꿀 수 있어 MMIO 에 쓸 수 없다. */
	memcpy_fromio(buf, attr, len);
	/* [한국어] 문자열 끝에 개행을 붙인다 - sysfs 관례상 값 뒤에는 개행이 온다. */
	buf[len] = '\n';
	/* [한국어] 그 뒤에 NUL 을 두어 C 문자열로 완성한다. */
	buf[len + 1] = 0;

	/* [한국어] 마지막 바이트부터 앞으로 훑는다. 0 번은 검사하지 않아
	 * 최소 한 글자는 남는다. */
	for (i = len - 1; i > 0; i--) {
		/* [한국어] 공백이 아닌 글자를 만나면 */
		if (buf[i] != ' ')
			/* [한국어] 그 앞은 실제 내용이므로 멈춘다. */
			break;
		/* [한국어] 공백 자리를 개행으로 덮는다 - 이 자리가 새로운 끝이 된다. */
		buf[i] = '\n';
		/* [한국어] 그 뒤에 NUL 을 놓아 문자열을 잘라 낸다. */
		buf[i + 1] = 0;
	}

	/* [한국어] 잘라 낸 뒤의 실제 길이. */
	return strlen(buf);
}

/* [한국어] DEVICE_ATTR_SYS_INFO_STR - sys_info 안의 문자열 필드 하나를
 * sysfs 속성으로 만드는 매크로.
 * 
 * 같은 이름의 필드가 gen3 공용체와 gen4 공용체 양쪽에 있지만 오프셋도
 * 길이도 다르다(예: product_id 는 gen3 16 바이트, gen4 24 바이트).
 * 세 속성(vendor_id/product_id/product_revision)에 대해 똑같은
 * 세대 분기 코드를 세 번 쓰는 대신 매크로로 찍어 낸다.
 * 
 * ## 은 토큰 붙이기 연산자로, field 가 vendor_id 면 vendor_id_show 라는
 * 함수 이름을 만든다 - 그래야 아래 DEVICE_ATTR_RO 가 찾을 수 있다.
 * sizeof 를 매크로 안에서 쓰는 덕분에 세대별 길이가 자동으로 맞춰진다.
 * 
 * 주의: 매크로 본문은 역슬래시로 이어진 한 줄이라 그 사이에 주석을
 * 넣을 수 없다. 그래서 설명을 여기 한데 모았다.
 * 본문의 흐름은 (1) 장치 얻기 (2) sys_info 블록 얻기 (3) 세대에 따라
 * gen3/gen4 필드를 골라 io_string_show 에 넘기기 (4) 알 수 없는 세대면
 * -EOPNOTSUPP 이다. */
#define DEVICE_ATTR_SYS_INFO_STR(field) \
static ssize_t field ## _show(struct device *dev, \
	struct device_attribute *attr, char *buf) \
{ \
	struct switchtec_dev *stdev = to_stdev(dev); \
	struct sys_info_regs __iomem *si = stdev->mmio_sys_info; \
	if (stdev->gen == SWITCHTEC_GEN3) \
		return io_string_show(buf, &si->gen3.field, \
				      sizeof(si->gen3.field)); \
	else if (stdev->gen >= SWITCHTEC_GEN4) \
		return io_string_show(buf, &si->gen4.field, \
				      sizeof(si->gen4.field)); \
	else \
		return -EOPNOTSUPP; \
} \
\
static DEVICE_ATTR_RO(field)

/* [한국어] 벤더 문자열 속성 - gen3 는 8 바이트, gen4 도 8 바이트 자리다. */
DEVICE_ATTR_SYS_INFO_STR(vendor_id);
/* [한국어] 제품 문자열 속성 - gen3 16 바이트, gen4 24 바이트로 길이가 다르다.
 * 매크로 안의 sizeof 가 그 차이를 흡수한다. */
DEVICE_ATTR_SYS_INFO_STR(product_id);
/* [한국어] 제품 리비전 문자열 - gen3 4 바이트, gen4 2 바이트. */
DEVICE_ATTR_SYS_INFO_STR(product_revision);

/* [한국어] component_vendor_show - Gen3 에만 있는 component_vendor 를 찍는다
 * 
 * @dev: 이 속성이 붙은 device.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 채운 바이트 수.
 * 
 * ★ 위 매크로로 만들 수 없는 이유: 이 필드는 gen4 공용체에 아예 없다.
 * 매크로는 gen3/gen4 양쪽에 같은 이름의 필드가 있다고 전제하므로
 * 컴파일이 되지 않는다. 그래서 손으로 쓰되, Gen4 이상에서는 필드를
 * 읽지 않고 "none" 이라는 문자열을 낸다.
 * 
 * ★ 왜 속성을 아예 없애지 않고 "none" 을 내는가: sysfs 속성 목록은
 * switchtec_device_attrs[] 하나로 고정되어 있어 세대별로 파일 구성을
 * 바꾸지 않는다. 유저스페이스 도구가 파일이 없어 실패하는 것보다
 * "none" 을 읽고 판단하는 편이 호환에 낫다.
 * 
 * 실행 컨텍스트: 프로세스 문맥.
 * 
 * 호출 체인:
 *   (유저스페이스) read(/sys/.../component_vendor) -> sysfs
 *     -> [component_vendor_show] -> io_string_show() */
static ssize_t component_vendor_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	/* [한국어] 장치 객체. */
	struct switchtec_dev *stdev = to_stdev(dev);
	/* [한국어] 시스템 정보 블록. */
	struct sys_info_regs __iomem *si = stdev->mmio_sys_info;

	/* component_vendor field not supported after gen3 */
	/* [한국어] 아래 영어 주석대로 Gen3 이후에는 이 필드가 없다. */
	if (stdev->gen != SWITCHTEC_GEN3)
		/* [한국어] 없음을 문자열로 알린다. */
		return sysfs_emit(buf, "none\n");

	/* [한국어] Gen3 이면 8 바이트 문자열 필드를 공백 제거해 찍는다. */
	return io_string_show(buf, &si->gen3.component_vendor,
			      sizeof(si->gen3.component_vendor));
}
/* [한국어] dev_attr_component_vendor 생성. */
static DEVICE_ATTR_RO(component_vendor);

/* [한국어] component_id_show - Gen3 에만 있는 component_id 를 찍는다
 * 
 * @dev: 이 속성이 붙은 device.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 채운 바이트 수.
 * 
 * "PM%04X" 형식은 제조사의 부품 번호 표기를 그대로 재현한 것이다.
 * Gen4 이상에서는 해당 필드가 없어 "none" 을 낸다.
 * 
 * ★ 코드 순서의 특이점: 세대 검사보다 ioread16 이 먼저 온다. 즉
 * Gen4 장치에서도 gen3 공용체 자리를 한 번 읽는다. 공용체의 두 판이
 * 같은 메모리를 덮고 있고 그 오프셋이 매핑 범위 안이므로 안전하지만,
 * 읽은 값은 아래 분기에서 버려진다. 초기화식을 선언에 붙이는 스타일
 * 때문에 생긴 순서이지, 의도된 읽기는 아니다.
 * 
 * 실행 컨텍스트: 프로세스 문맥.
 * 
 * 호출 체인:
 *   (유저스페이스) read(/sys/.../component_id) -> sysfs -> [component_id_show] */
static ssize_t component_id_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	/* [한국어] 장치 객체. */
	struct switchtec_dev *stdev = to_stdev(dev);
	/* [한국어] gen3 배치의 component_id 를 16 비트로 읽는다. 위에 적었듯
	 * Gen4 에서도 일단 읽고 아래에서 버린다. */
	int id = ioread16(&stdev->mmio_sys_info->gen3.component_id);

	/* component_id field not supported after gen3 */
	/* [한국어] 아래 영어 주석대로 Gen3 이후에는 없는 필드다. */
	if (stdev->gen != SWITCHTEC_GEN3)
		/* [한국어] 없음을 알린다. */
		return sysfs_emit(buf, "none\n");

	/* [한국어] 제조사 표기 형식대로 찍는다 - 대문자 16진수 4자리. */
	return sysfs_emit(buf, "PM%04X\n", id);
}
/* [한국어] dev_attr_component_id 생성. */
static DEVICE_ATTR_RO(component_id);

/* [한국어] component_revision_show - Gen3 에만 있는 component_revision 을 찍는다
 * 
 * @dev: 이 속성이 붙은 device.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 채운 바이트 수.
 * 
 * ★ 지원하지 않을 때 "none" 이 아니라 "255" 를 내는 점이 앞의 둘과
 * 다르다. 이 값은 숫자로 파싱되는 필드라, 문자열을 내면 옛 유저스페이스
 * 도구의 파서가 깨질 수 있다. 그래서 8 비트 필드가 가질 수 있는
 * 최대값(0xFF)을 '무효' 를 뜻하는 값으로 골라 쓴다.
 * 
 * 실행 컨텍스트: 프로세스 문맥.
 * 
 * 호출 체인:
 *   (유저스페이스) read(/sys/.../component_revision) -> sysfs
 *     -> [component_revision_show] */
static ssize_t component_revision_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	/* [한국어] 장치 객체. */
	struct switchtec_dev *stdev = to_stdev(dev);
	/* [한국어] gen3 배치의 component_revision 을 8 비트로 읽는다. component_id 와
	 * 마찬가지로 세대 검사보다 먼저 읽고, Gen4 에서는 아래에서 버린다. */
	int rev = ioread8(&stdev->mmio_sys_info->gen3.component_revision);

	/* component_revision field not supported after gen3 */
	/* [한국어] Gen3 이후에는 없는 필드. */
	if (stdev->gen != SWITCHTEC_GEN3)
		/* [한국어] 숫자 파서를 깨지 않도록 무효값 255 를 낸다. */
		return sysfs_emit(buf, "255\n");

	/* [한국어] Gen3 이면 실제 리비전 숫자. */
	return sysfs_emit(buf, "%d\n", rev);
}
/* [한국어] dev_attr_component_revision 생성. */
static DEVICE_ATTR_RO(component_revision);

/* [한국어] partition_show - 이 관리 엔드포인트가 속한 파티션 번호를 찍는다
 * 
 * @dev: 이 속성이 붙은 device.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 채운 바이트 수.
 * 
 * MMIO 를 읽지 않고 probe 때 저장해 둔 값을 그대로 낸다 -
 * 파티션 배정은 런타임에 바뀌지 않기 때문이다. ioctl 의
 * SWITCHTEC_IOCTL_EVENT_LOCAL_PART_IDX(-1)가 가리키는 바로 그 번호이며,
 * 유저스페이스가 자기 파티션을 알아야 할 때 이 파일을 읽는다.
 * 
 * 실행 컨텍스트: 프로세스 문맥.
 * 
 * 호출 체인:
 *   (유저스페이스) read(/sys/.../partition) -> sysfs -> [partition_show] */
static ssize_t partition_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	/* [한국어] 장치 객체. */
	struct switchtec_dev *stdev = to_stdev(dev);

	/* [한국어] 저장된 파티션 번호를 10진수로. */
	return sysfs_emit(buf, "%d\n", stdev->partition);
}
/* [한국어] dev_attr_partition 생성. */
static DEVICE_ATTR_RO(partition);

/* [한국어] partition_count_show - 스위치 전체의 파티션 개수를 찍는다
 * 
 * @dev: 이 속성이 붙은 device.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 채운 바이트 수.
 * 
 * 역시 probe 때 읽어 둔 값이다(0 이면 1 로 보정된 값). ioctl 의
 * EVENT_CTL 에서 IDX_ALL 을 쓸 때 커널이 도는 횟수와 같은 값이라,
 * 유저스페이스가 순회 범위를 미리 알고 싶을 때 참고한다.
 * 
 * 실행 컨텍스트: 프로세스 문맥.
 * 
 * 호출 체인:
 *   (유저스페이스) read(/sys/.../partition_count) -> sysfs
 *     -> [partition_count_show] */
static ssize_t partition_count_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	/* [한국어] 장치 객체. */
	struct switchtec_dev *stdev = to_stdev(dev);

	/* [한국어] 저장된 파티션 개수를 10진수로. */
	return sysfs_emit(buf, "%d\n", stdev->partition_count);
}
/* [한국어] dev_attr_partition_count 생성. */
static DEVICE_ATTR_RO(partition_count);

/* [한국어] switchtec_device_attrs - /sys/class/switchtec/switchtecN/ 아래에 만들
 *                          속성 파일 목록.
 * 아래 각 원소는 DEVICE_ATTR_RO 매크로가 만든 dev_attr_<이름> 구조체의
 * attribute 멤버를 가리킨다. 순서는 표시 순서와 무관하다. */
static struct attribute *switchtec_device_attrs[] = {
	/* [한국어] device_version - 하드웨어 리비전(16진수). */
	&dev_attr_device_version.attr,
	/* [한국어] fw_version - 펌웨어 버전(8자리 16진수). */
	&dev_attr_fw_version.attr,
	/* [한국어] vendor_id - 벤더 문자열. Gen3/Gen4 배치 차이는 매크로가 흡수한다. */
	&dev_attr_vendor_id.attr,
	/* [한국어] product_id - 제품 문자열. */
	&dev_attr_product_id.attr,
	/* [한국어] product_revision - 제품 리비전 문자열. */
	&dev_attr_product_revision.attr,
	/* [한국어] component_vendor - Gen3 전용. 그 뒤 세대에서는 "none" 을 낸다. */
	&dev_attr_component_vendor.attr,
	/* [한국어] component_id - Gen3 전용. "PM%04X" 형식으로 낸다. */
	&dev_attr_component_id.attr,
	/* [한국어] component_revision - Gen3 전용. 그 뒤 세대에서는 255 를 낸다. */
	&dev_attr_component_revision.attr,
	/* [한국어] partition - 이 관리 엔드포인트가 속한 파티션 번호. */
	&dev_attr_partition.attr,
	/* [한국어] partition_count - 스위치 전체의 파티션 개수. */
	&dev_attr_partition_count.attr,
	/* [한국어] 배열의 끝 표식. sysfs 코어가 NULL 을 만날 때까지 순회한다. */
	NULL,
};

/* [한국어] 위 배열을 switchtec_device_groups 라는 attribute_group 배열로
 * 감싸는 매크로. stdev_create() 가 dev->groups 에 이 이름을 대입하면,
 * device_add 시점에 코어가 파일들을 자동으로 만들고 device_del 시점에
 * 자동으로 지운다 - 드라이버가 파일 생성/삭제를 직접 다루지 않아도
 * 되게 하는 표준 방식이다. */
ATTRIBUTE_GROUPS(switchtec_device);

/* [한국어] switchtec_dev_open - /dev/switchtecN 을 열 때 사용자 문맥을 만든다
 * 
 * @inode: 열리는 노드. i_cdev 로 어느 장치인지 알아낸다.
 * @filp: 만들어지는 파일 객체.
 * @return: 0 성공, -ENOMEM 등 stuser_create 의 오류.
 * 
 * ★ fd 마다 stuser 를 따로 만드는 이유: MRPC 는 '명령을 쓰고 응답을
 * 읽는' 두 단계로 나뉜 프로토콜이다. 그 사이의 상태(어떤 명령을 냈는지,
 * 응답이 왔는지, 몇 바이트를 읽을 것인지)를 어딘가 담아야 하는데,
 * 장치 하나를 여러 프로세스가 동시에 열 수 있으므로 장치가 아니라
 * fd 마다 담아야 한다.
 * 
 * stream_open 을 쓰는 이유: 이 파일은 파일 오프셋(f_pos)이 의미가 없는
 * 스트림이다. 이 표시를 해 두면 커널이 read/write 시 f_pos 락을 잡지
 * 않아, 같은 fd 를 여러 스레드가 동시에 쓸 수 있다.
 * 
 * 실행 컨텍스트: 프로세스 문맥(open 시스템 호출).
 * 
 * 호출 체인:
 *   (유저스페이스) open() -> vfs -> [switchtec_dev_open] -> stuser_create() */
static int switchtec_dev_open(struct inode *inode, struct file *filp)
{
	/* [한국어] 어느 장치를 여는지. */
	struct switchtec_dev *stdev;
	/* [한국어] 이 fd 전용 사용자 문맥. */
	struct switchtec_user *stuser;

	/* [한국어] inode 에 실려 온 cdev 포인터에서 그것을 품고 있는 switchtec_dev 를
	 * 역산한다. cdev 는 stdev 안에 값으로 박혀 있으므로 이 역산이 성립한다. */
	stdev = container_of(inode->i_cdev, struct switchtec_dev, cdev);

	/* [한국어] 사용자 문맥을 만든다 - 이 안에서 장치 참조가 하나 올라간다. */
	stuser = stuser_create(stdev);
	/* [한국어] 메모리 부족 등. */
	if (IS_ERR(stuser))
		/* [한국어] 포인터에 실린 오류 코드를 꺼내 돌려준다. */
		return PTR_ERR(stuser);

	/* [한국어] 이후 read/write/poll/ioctl 이 이 필드로 사용자 문맥을 되찾는다. */
	filp->private_data = stuser;
	/* [한국어] 파일 오프셋이 없는 스트림임을 표시한다. */
	stream_open(inode, filp);

	/* [한국어] 디버그 추적 - 어떤 stuser 가 만들어졌는지. */
	dev_dbg(&stdev->dev, "%s: %p\n", __func__, stuser);

	/* [한국어] 성공. */
	return 0;
}

/* [한국어] switchtec_dev_release - 파일의 마지막 참조가 닫힐 때 불린다
 * 
 * @inode: 닫히는 노드(쓰지 않는다).
 * @filp: 닫히는 파일.
 * @return: 항상 0. close(2)의 반환값이 되지만 실패할 일이 없다.
 * 
 * ★ 여기서 곧바로 해제되지 않을 수 있다는 점이 중요하다. 이 fd 가
 * 낸 MRPC 명령이 아직 큐에 남아 있으면 mrpc_queue_cmd 가 올려 둔 참조가
 * 살아 있으므로, 실제 해제는 그 명령이 완료되거나 stdev_kill 이
 * 큐를 비울 때 일어난다. 그때까지 stuser 는 '주인 없는' 상태로 남아
 * 응답을 받고 조용히 버려진다.
 * 
 * 실행 컨텍스트: 프로세스 문맥. 프로세스가 종료될 때도 여기로 온다.
 * 
 * 호출 체인:
 *   (유저스페이스) close()/exit() -> vfs -> [switchtec_dev_release]
 *     -> stuser_put() -> (참조가 0 이면) stuser_free() */
static int switchtec_dev_release(struct inode *inode, struct file *filp)
{
	/* [한국어] open 때 매달아 둔 사용자 문맥. */
	struct switchtec_user *stuser = filp->private_data;

	/* [한국어] 참조를 하나 놓는다. 마지막이면 stuser_free 가 장치 참조까지
	 * 놓고 메모리를 반납한다. */
	stuser_put(stuser);

	/* [한국어] close 는 실패를 알릴 방법이 마땅치 않아 항상 성공으로 처리한다. */
	return 0;
}

/* [한국어] lock_mutex_and_test_alive - 락을 잡고 하드웨어 생존을 함께 확인한다
 * 
 * @stdev: 대상 장치.
 * @return: 0 이면 락을 '쥔 채' 돌아온다. 실패면 락을 쥐지 않은 상태다.
 *          -EINTR 시그널로 대기가 끊김, -ENODEV 장치가 이미 제거됨.
 * 
 * ★ 왜 두 일을 한 함수로 묶었는가: 이 둘을 따로 하면 그 사이에 장치가
 * 사라질 수 있다. 락을 먼저 잡고 그 안에서 확인해야, 확인이 참인 동안
 * 장치가 사라지지 않음을 보장할 수 있다(stdev_kill 도 같은 락 안에서
 * alive 를 내리기 때문이다).
 * 
 * ★ 반환값에 따라 락 상태가 다르다는 점이 이 함수의 사용상 함정이다.
 * 성공하면 호출자가 풀어야 하고, 실패하면 풀면 안 된다. 이 파일의 모든
 * 호출자가 그 규약을 지키고 있다.
 * 
 * _interruptible 판을 쓰는 이유: 이 락은 최대 500ms 단위로 하드웨어를
 * 기다리는 경로가 잡으므로 대기가 길어질 수 있다. 그동안 Ctrl-C 조차
 * 듣지 않으면 곤란하므로 시그널로 깨어날 수 있게 한다.
 * 
 * 실행 컨텍스트: 프로세스 문맥 전용. 잠들 수 있으므로 인터럽트 문맥에서
 * 불러서는 안 된다.
 * 
 * 호출 체인:
 *   switchtec_dev_write()/read()/poll()/ioctl() -> [lock_mutex_and_test_alive] */
static int lock_mutex_and_test_alive(struct switchtec_dev *stdev)
{
	/* [한국어] 락을 잡되 시그널로 깨어날 수 있게 한다. 0 이 아닌 반환은
	 * 시그널로 중단되었다는 뜻이며, 이때 락은 잡혀 있지 않다. */
	if (mutex_lock_interruptible(&stdev->mrpc_mutex))
		/* [한국어] 유저스페이스에는 -EINTR 로 알린다 - 시스템 호출을 다시 시도하라는
		 * 표준 신호다. */
		return -EINTR;

	/* [한국어] stdev_kill() 이 이미 다녀갔다면 하드웨어가 없다. */
	if (!stdev->alive) {
		/* [한국어] 락을 풀고 나간다 - 실패 시 락을 쥐지 않는다는 규약을 지킨다. */
		mutex_unlock(&stdev->mrpc_mutex);
		/* [한국어] 장치가 사라졌음을 알린다. */
		return -ENODEV;
	}

	/* [한국어] 성공 - 락을 쥔 채 돌아간다. */
	return 0;
}

/* [한국어] switchtec_dev_write - MRPC 명령을 큐에 넣는다 (write(2) 진입점)
 * 
 * @filp: 열린 파일.
 * @data: 유저 버퍼. 형식은 [4바이트 명령 코드][페이로드...] 다.
 * @size: 버퍼 길이. 최소 4(명령만), 최대 4 + 1024.
 * @off: 파일 오프셋(쓰지 않는다 - stream_open 으로 오프셋 없는 파일이다).
 * @return: 성공하면 소비한 바이트 수(=size). 음수 errno 실패.
 * 
 * ★ MRPC(Management Remote Procedure Call)란: Switchtec 펌웨어에
 * '명령 번호 + 인자' 를 보내고 '반환 코드 + 결과' 를 받는 프로토콜이다.
 * 드라이버는 명령의 내용을 전혀 해석하지 않는다 - 그저 유저스페이스와
 * 펌웨어 사이의 통로 역할만 한다. 그래서 새 펌웨어 기능이 생겨도
 * 드라이버를 고칠 필요가 없다.
 * 
 * ★ 왜 write 와 read 로 나뉘어 있는가: 명령을 내고(write) 응답을
 * 받는(read) 것이 별개의 시스템 호출이라, 그 사이에 poll 로 기다리거나
 * O_NONBLOCK 으로 즉시 확인할 수 있다. ioctl 하나로 묶었다면 항상
 * 동기적으로 기다려야 했을 것이다.
 * 
 * ★ 권한 검사가 여기 있는 이유(중요한 보안 경계): MRPC 명령 중
 * GAS_READ(0x29)/GAS_WRITE(0x87)는 스위치의 전체 주소 공간을 임의로
 * 읽고 쓰는 명령이다. 이것을 일반 사용자에게 허용하면 스위치를 통해
 * 시스템 전체를 망가뜨릴 수 있으므로 CAP_SYS_ADMIN 을 요구한다.
 * 나머지 명령은 /dev 노드의 파일 권한으로만 통제된다.
 * MRPC_CMD_ID 로 하위 16 비트만 떼어 비교하는 이유는 상위 비트에
 * 다른 의미(비동기 플래그 등)가 실릴 수 있기 때문이다.
 * 
 * 실행 컨텍스트: 프로세스 문맥. 락 안에서 copy_from_user 를 부르므로
 * 페이지 폴트로 잠들 수 있다.
 * 
 * 호출 체인:
 *   (유저스페이스) write() -> vfs -> [switchtec_dev_write]
 *     -> lock_mutex_and_test_alive() -> mrpc_queue_cmd() -> mrpc_cmd_submit() */
static ssize_t switchtec_dev_write(struct file *filp, const char __user *data,
				   size_t size, loff_t *off)
{
	/* [한국어] 이 fd 의 사용자 문맥. */
	struct switchtec_user *stuser = filp->private_data;
	/* [한국어] 대상 장치. */
	struct switchtec_dev *stdev = stuser->stdev;
	/* [한국어] 반환값. */
	int rc;

	/* [한국어] ★ 길이 검증. 최소한 명령 코드 4 바이트는 있어야 하고, */
	if (size < sizeof(stuser->cmd) ||
	    /* [한국어] 페이로드가 버퍼(1KB)를 넘으면 안 된다. 이 검사가 없으면 아래
	     * copy_from_user 가 stuser->data 를 넘겨 써 커널 메모리를 훼손한다. */
	    size > sizeof(stuser->cmd) + sizeof(stuser->data))
		/* [한국어] 길이가 잘못됨. */
		return -EINVAL;

	/* [한국어] 명령 코드를 뺀 나머지가 페이로드 길이다. mrpc_cmd_submit 이
	 * 이 값만큼만 하드웨어에 밀어 넣는다. */
	stuser->data_len = size - sizeof(stuser->cmd);

	/* [한국어] 락을 잡고 장치 생존을 확인한다. */
	rc = lock_mutex_and_test_alive(stdev);
	/* [한국어] 실패하면 락을 쥐지 않았으므로 그대로 반환한다. */
	if (rc)
		return rc;

	/* [한국어] 이 fd 가 이미 명령을 내고 응답을 안 읽은 상태라면. */
	if (stuser->state != MRPC_IDLE) {
		/* [한국어] -EBADE('잘못된 교환') 로 거절한다. 한 fd 는 한 번에 하나의
		 * 명령만 진행할 수 있다 - stuser 에 명령/응답 자리가 하나뿐이기 때문이다. */
		rc = -EBADE;
		/* [한국어] 락을 풀어야 하므로 라벨로 간다. */
		goto out;
	}

	/* [한국어] 명령 코드 4 바이트를 먼저 읽어 온다. 권한 검사를 하려면
	 * 명령 번호를 알아야 하므로 페이로드보다 먼저 가져온다. */
	rc = copy_from_user(&stuser->cmd, data, sizeof(stuser->cmd));
	/* [한국어] copy_from_user 는 '복사하지 못한 바이트 수' 를 돌려준다 -
	 * 0 이 아니면 실패다. */
	if (rc) {
		/* [한국어] 유저스페이스에는 -EFAULT 로 알린다. */
		rc = -EFAULT;
		goto out;
	}
	/* [한국어] ★ 위 설명의 권한 경계. GAS_WRITE 는 */
	if (((MRPC_CMD_ID(stuser->cmd) == MRPC_GAS_WRITE) ||
	     /* [한국어] 스위치 주소 공간에 임의로 쓰고, GAS_READ 는 임의로 읽는다. */
	     (MRPC_CMD_ID(stuser->cmd) == MRPC_GAS_READ)) &&
	    /* [한국어] 그러므로 CAP_SYS_ADMIN 이 없으면 허용할 수 없다. */
	    !capable(CAP_SYS_ADMIN)) {
		/* [한국어] 권한 없음. */
		rc = -EPERM;
		goto out;
	}

	/* [한국어] 유저 포인터를 명령 코드 다음으로 옮긴다 - 이제부터 페이로드다. */
	data += sizeof(stuser->cmd);
	/* [한국어] 페이로드를 stuser->data 로 복사한다. 길이는 위에서 검증된 값이라
	 * 1KB 버퍼를 넘지 않는다. */
	rc = copy_from_user(&stuser->data, data, size - sizeof(stuser->cmd));
	/* [한국어] 복사 실패. */
	if (rc) {
		/* [한국어] -EFAULT. */
		rc = -EFAULT;
		goto out;
	}

	/* [한국어] 대기 큐에 넣고, 하드웨어가 한가하면 곧바로 띄운다. 이 함수는
	 * 현재 언제나 0 을 돌려준다. */
	rc = mrpc_queue_cmd(stuser);

/* [한국어] 성공/실패가 함께 지나가는 정리 지점. */
out:
	/* [한국어] 반드시 락을 푼다. */
	mutex_unlock(&stdev->mrpc_mutex);

	/* [한국어] 오류가 있었으면 */
	if (rc)
		/* [한국어] 그대로 전한다. */
		return rc;

	/* [한국어] write 규약대로 '소비한 바이트 수' 를 돌려준다. 부분 쓰기는 없다 -
	 * 전부 받거나 오류다. */
	return size;
}

/* [한국어] switchtec_dev_read - MRPC 응답을 받아 간다 (read(2) 진입점)
 * 
 * @filp: 열린 파일.
 * @data: 유저 버퍼. 형식은 [4바이트 반환 코드][응답 데이터...] 다.
 * @size: 버퍼 길이. 최소 4, 최대 4 + 1024.
 * @off: 파일 오프셋(쓰지 않는다).
 * @return: 성공하면 size. 음수 errno 실패.
 * 
 * ★ 이 함수의 락 사용이 이 파일에서 가장 까다롭다 - 락을 잡았다 풀고,
 * 기다린 뒤, 다시 잡는다. 이유는 명확하다: 응답을 기다리는 동안
 * mrpc_mutex 를 쥐고 있으면 완료 처리(mrpc_event_work)가 같은 락을
 * 잡지 못해 영원히 진행되지 않는다 - 자기 자신을 막는 교착이다.
 * 그래서 (1) 락 안에서 상태를 확인하고 read_len 을 정한 뒤 (2) 락을
 * 풀고 기다리고 (3) 다시 잡아 결과를 읽는다.
 * 
 * ★ 세 갈래의 상태 판정:
 *   - MRPC_IDLE     : 명령을 낸 적이 없다 -> -EBADE
 *   - MRPC_IO_ERROR : 하드웨어가 죽어 완료 처리가 실패했다 -> -EIO
 *   - MRPC_DONE     : 정상 완료 -> 데이터를 준다
 * 그리고 정상 완료라도 펌웨어가 준 status 에 따라 반환값이 갈린다.
 * 
 * ★ read_len 계산의 미묘함: size 에서 빼는 것이 sizeof(cmd)가 아니라
 * sizeof(return_code)다. 둘 다 u32 라 값은 같지만, '유저 버퍼의 앞
 * 4 바이트는 반환 코드 자리' 라는 의미를 정확히 드러낸다.
 * 
 * ★ O_NONBLOCK 처리: 논블로킹이면 완료를 기다리지 않고 -EAGAIN 을
 * 돌려준다. 유저스페이스는 poll 로 기다렸다 다시 부르면 된다.
 * 
 * 실행 컨텍스트: 프로세스 문맥. 기다리는 구간에서 잠든다.
 * 
 * 호출 체인:
 *   (유저스페이스) read() -> vfs -> [switchtec_dev_read]
 *     -> wait_event_interruptible() (mrpc_event_work 가 깨운다) */
static ssize_t switchtec_dev_read(struct file *filp, char __user *data,
				  size_t size, loff_t *off)
/* [한국어] 이 fd 의 사용자 문맥. */
{
	/* [한국어] 대상 장치. */
	struct switchtec_user *stuser = filp->private_data;
	/* [한국어] 반환값. */
	struct switchtec_dev *stdev = stuser->stdev;
	int rc;
/* [한국어] 길이 검증 - 최소한 반환 코드 4 바이트는 담을 수 있어야 하고, */

	/* [한국어] 응답 버퍼(1KB)보다 크게 요구할 수는 없다. 이 검사가 없으면
	 * 아래 copy_to_user 가 stuser->data 를 넘겨 읽어 인접 커널 메모리를
	 * 유저스페이스에 유출한다. */
	if (size < sizeof(stuser->cmd) ||
	    /* [한국어] 길이가 잘못됨. */
	    size > sizeof(stuser->cmd) + sizeof(stuser->data))
		return -EINVAL;
/* [한국어] (1) 락을 잡고 생존 확인. */

	/* [한국어] 실패면 락 없이 반환. */
	rc = lock_mutex_and_test_alive(stdev);
	if (rc)
		/* [한국어] 명령을 낸 적이 없는데 읽으려 한다. */
		return rc;
/* [한국어] 락을 풀고 */

	/* [한국어] -EBADE 로 거절한다. */
	if (stuser->state == MRPC_IDLE) {
		mutex_unlock(&stdev->mrpc_mutex);
		return -EBADE;
	/* [한국어] ★ 유저가 원하는 응답 길이를 기록한다. mrpc_complete_cmd 가
	 * 이 값만큼만 하드웨어/DMA 버퍼에서 복사하므로, 락을 푸는 이 시점
	 * 이전에 정해 두어야 한다. mrpc_queue_cmd 는 일단 최대치로 잡아 두고,
	 * 여기서 유저가 실제로 원하는 크기로 좁힌다. */
	}

	/* [한국어] (2) 락을 푼다 - 이것이 없으면 완료 처리가 진행되지 못한다. */
	stuser->read_len = size - sizeof(stuser->return_code);

	/* [한국어] 논블로킹 모드. */
	mutex_unlock(&stdev->mrpc_mutex);
/* [한국어] 아직 완료되지 않았으면 */

	/* [한국어] 기다리지 않고 '다시 시도하라' 고 알린다. */
	if (filp->f_flags & O_NONBLOCK) {
		if (!stuser->cmd_done)
			/* [한국어] 블로킹 모드 - 완료될 때까지 잠든다. */
			return -EAGAIN;
	/* [한국어] cmd_done 이 참이 될 때까지 잠든다. 깨우는 쪽은
	 * mrpc_cleanup_cmd()(정상 완료)와 stdev_kill()(장치 소멸)이다.
	 * _interruptible 이라 시그널로도 깨어난다. */
	} else {
		rc = wait_event_interruptible(stuser->cmd_comp,
					      /* [한국어] 시그널로 중단되었다 - -ERESTARTSYS 등이 그대로 전해져
					       * libc 가 재시도하거나 EINTR 로 보고한다. */
					      stuser->cmd_done);
		/* [한국어] 그대로 반환. 명령은 여전히 큐에 남아 있으므로 나중에 다시
		 * read 할 수 있다. */
		if (rc < 0)
			return rc;
	}
/* [한국어] (3) 다시 락을 잡는다. 기다리는 동안 장치가 사라졌을 수 있으므로
 * 생존 확인을 다시 한다. */

	/* [한국어] 사라졌으면 -ENODEV. */
	rc = lock_mutex_and_test_alive(stdev);
	if (rc)
		return rc;
/* [한국어] 완료 처리가 하드웨어 오류로 끝난 경우(mrpc_error_complete_cmd). */

	/* [한국어] 락을 풀고 */
	if (stuser->state == MRPC_IO_ERROR) {
		/* [한국어] -EIO 로 알린다. */
		mutex_unlock(&stdev->mrpc_mutex);
		return -EIO;
	}
/* [한국어] 깨어났는데 완료 상태가 아니다 - stdev_kill 이 강제로 깨운
 * 경우가 여기에 해당한다. */

	/* [한국어] 락을 풀고 */
	if (stuser->state != MRPC_DONE) {
		/* [한국어] -EBADE 로 알린다. */
		mutex_unlock(&stdev->mrpc_mutex);
		return -EBADE;
	}
/* [한국어] 응답의 앞 4 바이트는 펌웨어가 준 반환 코드다. 명령 자체의
 * 성공/실패는 이 값으로 판단하며, 커널의 errno 와는 별개다. */

	rc = copy_to_user(data, &stuser->return_code,
			  /* [한국어] 복사 실패. */
			  sizeof(stuser->return_code));
	/* [한국어] 락을 풀고 */
	if (rc) {
		/* [한국어] -EFAULT. */
		mutex_unlock(&stdev->mrpc_mutex);
		return -EFAULT;
	}
/* [한국어] 유저 포인터를 반환 코드 다음으로 옮긴다. */

	/* [한국어] 응답 데이터를 복사한다. 길이는 앞에서 검증된 값이라
	 * 1KB 버퍼를 넘지 않는다. */
	data += sizeof(stuser->return_code);
	rc = copy_to_user(data, &stuser->data,
			  /* [한국어] 복사 실패. */
			  size - sizeof(stuser->return_code));
	/* [한국어] 락을 풀고 */
	if (rc) {
		/* [한국어] -EFAULT. */
		mutex_unlock(&stdev->mrpc_mutex);
		return -EFAULT;
	}
/* [한국어] ★ 상태를 IDLE 로 되돌린다. 이 fd 가 다음 명령을 낼 수 있게 되는
 * 지점이며, write 의 'MRPC_IDLE 이 아니면 -EBADE' 검사와 짝을 이룬다. */

	stuser_set_state(stuser, MRPC_IDLE);
/* [한국어] 마지막으로 락을 푼다. */

	mutex_unlock(&stdev->mrpc_mutex);
/* [한국어] 펌웨어가 명령을 정상 처리했거나(DONE) */

	/* [한국어] 오류로 끝냈다면(ERROR) - 어느 쪽이든 응답이 유효하다.
	 * ERROR 도 성공으로 보는 이유는, 그 실패 사유가 이미 반환 코드로
	 * 유저에게 전달되었기 때문이다. */
	if (stuser->status == SWITCHTEC_MRPC_STATUS_DONE ||
	    /* [한국어] 읽은 바이트 수를 돌려준다. */
	    stuser->status == SWITCHTEC_MRPC_STATUS_ERROR)
		return size;
	/* [한국어] 명령이 중간에 끊긴 경우(INTERRUPTED). */
	else if (stuser->status == SWITCHTEC_MRPC_STATUS_INTERRUPTED)
		/* [한국어] -ENXIO 로 알린다. */
		return -ENXIO;
	/* [한국어] 그 밖의 알 수 없는 상태 - 펌웨어가 예상 밖의 값을 남겼다. */
	else
		/* [한국어] -EBADMSG 로 알린다. */
		return -EBADMSG;
}

/* [한국어] switchtec_dev_poll - poll(2)/select(2)/epoll 진입점
 * 
 * @filp: 열린 파일.
 * @wait: poll 테이블. 커널이 '어느 대기 큐를 감시할지' 를 모으는 그릇이다.
 * @return: 준비된 이벤트 비트마스크.
 * 
 * ★ 이 드라이버의 poll 은 두 가지 서로 다른 사건을 한 fd 로 감시한다.
 *   - 보통 읽기(EPOLLIN)  : 내가 낸 MRPC 명령의 응답이 도착했다
 *   - 우선 읽기(EPOLLPRI) : 스위치에서 이벤트가 발생했다
 * 유저스페이스는 POLLPRI 를 골라 감시하면 이벤트만, POLLIN 을 감시하면
 * 명령 완료만 기다릴 수 있다. 그래서 대기 큐도 두 개를 등록한다.
 * 
 * ★ poll_wait 를 조건 검사보다 '먼저' 부르는 것이 규칙이다. 순서를
 * 뒤집으면 (조건 검사 -> 그 직후 사건 발생 -> 대기 큐 등록) 순으로
 * 진행돼 방금 일어난 깨우기를 놓치고 영원히 잠들 수 있다. 대기 큐에
 * 먼저 이름을 올려 두면 그 사이에 일어난 깨우기도 받는다.
 * 
 * 장치가 사라진 경우: 잡을 수 있는 오류 비트를 모두 올려 돌려준다.
 * EPOLLIN 까지 함께 올리는 이유는, 오류 비트만 감시하지 않는 프로그램도
 * poll 에서 깨어나 read 를 시도하고 거기서 -ENODEV 를 받게 하기 위해서다.
 * 
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡았다 곧바로 푼다 - 여기서는
 * '살아 있는가' 확인만이 목적이고, 이후 읽는 두 값은 원자적으로 읽어도
 * 충분한 값들이다.
 * 
 * 호출 체인:
 *   (유저스페이스) poll()/epoll_wait() -> vfs -> [switchtec_dev_poll] */
static __poll_t switchtec_dev_poll(struct file *filp, poll_table *wait)
{
	/* [한국어] 이 fd 의 사용자 문맥. */
	struct switchtec_user *stuser = filp->private_data;
	/* [한국어] 그 사용자가 붙어 있는 장치. */
	struct switchtec_dev *stdev = stuser->stdev;
	/* [한국어] 돌려줄 이벤트 마스크. 아무 사건도 없으면 0 이다. */
	__poll_t ret = 0;

	/* [한국어] 명령 완료 대기 큐에 이름을 올린다. mrpc_cleanup_cmd() 나
	 * stdev_kill() 이 이 큐를 깨운다. */
	poll_wait(filp, &stuser->cmd_comp, wait);
	/* [한국어] 이벤트 대기 큐에도 올린다. switchtec_event_isr() 가 깨운다. */
	poll_wait(filp, &stdev->event_wq, wait);

	/* [한국어] 락을 잡으며 장치 생존을 확인한다. */
	if (lock_mutex_and_test_alive(stdev))
		/* [한국어] 장치가 사라졌거나 시그널을 받았다. 실패했으므로 락은 잡혀 있지
		 * 않다 - 그래서 여기서 unlock 하지 않는다. EPOLLRDHUP 와 EPOLLHUP 로
		 * '상대가 끊겼다' 를, EPOLLERR 로 오류를 알린다. */
		return EPOLLIN | EPOLLRDHUP | EPOLLOUT | EPOLLERR | EPOLLHUP;

	/* [한국어] 생존 확인만이 목적이었으므로 곧바로 푼다. 아래 두 값은 락 없이
	 * 읽어도 되는 성질의 것이다. */
	mutex_unlock(&stdev->mrpc_mutex);

	/* [한국어] 응답이 도착했으면(또는 stdev_kill 이 강제로 세웠으면). */
	if (stuser->cmd_done)
		/* [한국어] 읽을 것이 있다고 알린다. EPOLLRDNORM 은 EPOLLIN 의 세분화된
		 * 형태로, 둘 다 올려 주는 것이 관례다. */
		ret |= EPOLLIN | EPOLLRDNORM;

	/* [한국어] 이 fd 가 마지막으로 확인한 이벤트 카운터와 장치의 현재 카운터가
	 * 다르면 그 사이에 새 이벤트가 있었다는 뜻이다. 카운터 값 자체가 아니라
	 * '변했는가' 만 보므로 되감김이 문제되지 않는다. stuser->event_cnt 는
	 * open 때와 ioctl(EVENT_SUMMARY) 때 갱신된다. */
	if (stuser->event_cnt != atomic_read(&stdev->event_cnt))
		/* [한국어] 우선 순위 데이터가 있다고 알린다 - 유저스페이스는 이 신호를 받고
		 * ioctl(EVENT_SUMMARY) 로 무슨 이벤트인지 조회한다. */
		ret |= EPOLLPRI | EPOLLRDBAND;

	/* [한국어] 모아 둔 마스크를 돌려준다. */
	return ret;
}

/* [한국어] ioctl_flash_info - 플래시 전체 크기와 파티션 개수를 알려 준다
 * 
 * @stdev: 대상 장치.
 * @uinfo: 유저 버퍼(출력 전용).
 * @return: 0 성공, -EOPNOTSUPP 알 수 없는 세대, -EFAULT 복사 실패.
 * 
 * Switchtec 은 펌웨어 이미지와 설정을 온보드 플래시에 여러 '파티션' 으로
 * 나눠 담는다. 유저스페이스의 펌웨어 갱신 도구는 먼저 이 ioctl 로
 * 전체 크기와 파티션 개수를 알아낸 뒤, 파티션마다
 * FLASH_PART_INFO 를 불러 상세를 조회한다.
 * 
 * 세대별 차이: 파티션 개수가 Gen3 는 13, Gen4 이상은 19 로 고정되어
 * 있다(UAPI 상수). 하드웨어에서 읽는 것이 아니라 세대로 결정된다.
 * 
 * 실행 컨텍스트: 프로세스 문맥, mrpc_mutex 를 쥔 상태.
 * 
 * 호출 체인:
 *   switchtec_dev_ioctl() -> [ioctl_flash_info] -> ioread32()/copy_to_user() */
static int ioctl_flash_info(struct switchtec_dev *stdev,
			    struct switchtec_ioctl_flash_info __user *uinfo)
{
	/* [한국어] 커널 쪽 출력 버퍼. {0} 으로 통째로 0 초기화하는 것이 중요하다 -
	 * 구조체에 padding 필드가 있어, 초기화하지 않으면 커널 스택의
	 * 잔여 내용이 유저스페이스로 새어 나간다(정보 누출). */
	struct switchtec_ioctl_flash_info info = {0};
	/* [한국어] 플래시 정보 레지스터 블록. */
	struct flash_info_regs __iomem *fi = stdev->mmio_flash_info;

	/* [한국어] Gen3 레이아웃. */
	if (stdev->gen == SWITCHTEC_GEN3) {
		/* [한국어] gen3 공용체 쪽의 flash_length 를 읽는다. */
		info.flash_length = ioread32(&fi->gen3.flash_length);
		/* [한국어] Gen3 의 파티션 개수는 13 으로 고정. */
		info.num_partitions = SWITCHTEC_NUM_PARTITIONS_GEN3;
	/* [한국어] Gen4 이상(GEN5 포함). */
	} else if (stdev->gen >= SWITCHTEC_GEN4) {
		/* [한국어] gen4 공용체 쪽의 flash_length - 구조체 안 위치가 gen3 와 다르다. */
		info.flash_length = ioread32(&fi->gen4.flash_length);
		/* [한국어] Gen4 는 MAP/KEY/BL2 파티션이 늘어 19 개다. */
		info.num_partitions = SWITCHTEC_NUM_PARTITIONS_GEN4;
	/* [한국어] 알 수 없는 세대 - 현재 enum 상 도달할 수 없는 방어 분기다. */
	} else {
		/* [한국어] 해석할 레이아웃을 모른다. */
		return -EOPNOTSUPP;
	}

	/* [한국어] 채운 구조체를 유저 버퍼로 복사한다. */
	if (copy_to_user(uinfo, &info, sizeof(info)))
		/* [한국어] 잘못된 유저 주소. */
		return -EFAULT;

	/* [한국어] 성공. */
	return 0;
}

/* [한국어] set_fw_info_part - 파티션 정보 레지스터에서 주소와 길이를 옮겨 담는다
 * 
 * @info: 채울 유저 응답 구조체(커널 쪽 사본).
 * @pi: 읽어 올 partition_info 레지스터의 MMIO 주소.
 * @return: 없음
 * 
 * 한 줄짜리 두 번의 읽기지만 별도 함수로 뽑은 이유는 호출 지점이
 * Gen3 13 곳 + Gen4 19 곳으로 매우 많기 때문이다. 같은 두 줄을 서른
 * 번 넘게 반복하는 대신 이름을 붙여 의도를 드러낸다.
 * 
 * 실행 컨텍스트: 프로세스 문맥, mrpc_mutex 를 쥔 상태. MMIO 읽기만 한다.
 * 
 * 호출 체인:
 *   flash_part_info_gen3() / flash_part_info_gen4() -> [set_fw_info_part] */
static void set_fw_info_part(struct switchtec_ioctl_flash_part_info *info,
			     struct partition_info __iomem *pi)
{
	/* [한국어] 파티션이 플래시 안에서 시작하는 주소. */
	info->address = ioread32(&pi->address);
	/* [한국어] 그 파티션의 길이(바이트). */
	info->length = ioread32(&pi->length);
}

/* [한국어] flash_part_info_gen3 - Gen3 레이아웃에서 파티션 상세를 채운다
 * 
 * @stdev: 대상 장치.
 * @info: 입력으로 flash_partition(어느 파티션인지)을, 출력으로
 *        address/length/active 를 담는 구조체.
 * @return: 0 성공, -EINVAL 알 수 없는 파티션 번호.
 * 
 * ★ 'active' 와 'running' 은 다른 개념이다.
 *   - ACTIVE  : 다음 부팅 때 쓰이도록 선택되어 있는 판
 *   - RUNNING : 지금 실제로 돌고 있는 판
 * 펌웨어를 갱신하면 새 판이 ACTIVE 가 되지만, 재부팅 전까지 RUNNING 은
 * 여전히 옛 판이다. 두 비트가 따로 있는 이유가 그것이다.
 * 
 * 판정 방법도 둘이 다르다:
 *   - RUNNING : sys_info 의 cfg_running/img_running 값이 이 파티션을
 *               가리키는 약속된 상수와 같은가로 판정
 *   - ACTIVE  : flash_info 의 active_cfg/active_img 에 적힌 '주소' 가
 *               이 파티션의 시작 주소와 같은가로 판정. 그래서 switch
 *               바깥에서 한 번에 비교한다.
 * 
 * Gen3 에는 MAP/KEY/BL2 파티션이 없다 - 그 종류는 Gen4 에서 도입되었다.
 * 그런 번호가 오면 default 로 떨어져 -EINVAL 이 된다.
 * 
 * 실행 컨텍스트: 프로세스 문맥, mrpc_mutex 를 쥔 상태.
 * 
 * 호출 체인:
 *   switchtec_dev_ioctl() -> ioctl_flash_part_info() -> [flash_part_info_gen3]
 *     -> set_fw_info_part() */
static int flash_part_info_gen3(struct switchtec_dev *stdev,
		struct switchtec_ioctl_flash_part_info *info)
{
	/* [한국어] Gen3 배치의 플래시 정보 블록. */
	struct flash_info_regs_gen3 __iomem *fi =
		&stdev->mmio_flash_info->gen3;
	/* [한국어] Gen3 배치의 시스템 정보 블록 - running 판정에 쓰인다. */
	struct sys_info_regs_gen3 __iomem *si = &stdev->mmio_sys_info->gen3;
	/* [한국어] 현재 ACTIVE 로 선택된 판의 주소. -1(전 비트 1)로 시작하는 이유는
	 * '아직 못 읽음' 을 뜻하기 위해서다 - NVLOG 나 VENDOR 파티션은 ACTIVE
	 * 개념이 없어 이 값이 갱신되지 않는데, 실제 파티션 주소가 0xFFFFFFFF 일
	 * 수는 없으므로 아래 비교가 결코 참이 되지 않는다. */
	u32 active_addr = -1;

	/* [한국어] 유저가 요청한 파티션 종류로 분기. */
	switch (info->flash_partition) {
	/* [한국어] 설정 파티션 0. */
	case SWITCHTEC_IOCTL_PART_CFG0:
		/* [한국어] ACTIVE 로 선택된 설정 판의 주소를 읽는다. active_cfg 는 세 워드짜리
		 * 구조체지만 첫 워드가 address 이므로, 그 시작 주소에서 32 비트를 읽으면
		 * 바로 주소 값이 나온다. */
		active_addr = ioread32(&fi->active_cfg);
		/* [한국어] 이 파티션의 주소와 길이를 채운다. */
		set_fw_info_part(info, &fi->cfg0);
		/* [한국어] 지금 돌고 있는 설정이 CFG0 인지 - 펌웨어가 약속한 상수 값과 비교. */
		if (ioread16(&si->cfg_running) == SWITCHTEC_GEN3_CFG0_RUNNING)
			/* [한국어] RUNNING 비트를 세운다. */
			info->active |= SWITCHTEC_IOCTL_PART_RUNNING;
		break;
	/* [한국어] 설정 파티션 1 - 위와 같은 구조다. */
	case SWITCHTEC_IOCTL_PART_CFG1:
		/* [한국어] ACTIVE 주소는 CFG0/CFG1 이 같은 레지스터를 본다. */
		active_addr = ioread32(&fi->active_cfg);
		/* [한국어] cfg1 의 주소/길이. */
		set_fw_info_part(info, &fi->cfg1);
		/* [한국어] CFG1 이 돌고 있는지. */
		if (ioread16(&si->cfg_running) == SWITCHTEC_GEN3_CFG1_RUNNING)
			info->active |= SWITCHTEC_IOCTL_PART_RUNNING;
		break;
	/* [한국어] 펌웨어 이미지 파티션 0. */
	case SWITCHTEC_IOCTL_PART_IMG0:
		/* [한국어] 이미지 쪽 ACTIVE 주소는 별도 레지스터다. */
		active_addr = ioread32(&fi->active_img);
		/* [한국어] img0 의 주소/길이. */
		set_fw_info_part(info, &fi->img0);
		/* [한국어] IMG0 이 돌고 있는지. */
		if (ioread16(&si->img_running) == SWITCHTEC_GEN3_IMG0_RUNNING)
			info->active |= SWITCHTEC_IOCTL_PART_RUNNING;
		break;
	/* [한국어] 펌웨어 이미지 파티션 1. */
	case SWITCHTEC_IOCTL_PART_IMG1:
		/* [한국어] 같은 ACTIVE 이미지 주소 레지스터. */
		active_addr = ioread32(&fi->active_img);
		/* [한국어] img1 의 주소/길이. */
		set_fw_info_part(info, &fi->img1);
		/* [한국어] IMG1 이 돌고 있는지. */
		if (ioread16(&si->img_running) == SWITCHTEC_GEN3_IMG1_RUNNING)
			info->active |= SWITCHTEC_IOCTL_PART_RUNNING;
		break;
	/* [한국어] 비휘발성 로그 영역. ACTIVE/RUNNING 개념이 없어 주소/길이만 채운다. */
	case SWITCHTEC_IOCTL_PART_NVLOG:
		/* [한국어] 주소와 길이만. */
		set_fw_info_part(info, &fi->nvlog);
		break;
	/* [한국어] 벤더 전용 파티션 0~7. 보드 제조사가 자유롭게 쓰는 영역이라
	 * 드라이버는 위치만 알려 주고 내용은 해석하지 않는다. */
	case SWITCHTEC_IOCTL_PART_VENDOR0:
		/* [한국어] 벤더 영역 0. */
		set_fw_info_part(info, &fi->vendor[0]);
		break;
	/* [한국어] 벤더 영역 1. */
	case SWITCHTEC_IOCTL_PART_VENDOR1:
		set_fw_info_part(info, &fi->vendor[1]);
		break;
	/* [한국어] 벤더 영역 2. */
	case SWITCHTEC_IOCTL_PART_VENDOR2:
		set_fw_info_part(info, &fi->vendor[2]);
		break;
	/* [한국어] 벤더 영역 3. */
	case SWITCHTEC_IOCTL_PART_VENDOR3:
		set_fw_info_part(info, &fi->vendor[3]);
		break;
	/* [한국어] 벤더 영역 4. */
	case SWITCHTEC_IOCTL_PART_VENDOR4:
		set_fw_info_part(info, &fi->vendor[4]);
		break;
	/* [한국어] 벤더 영역 5. */
	case SWITCHTEC_IOCTL_PART_VENDOR5:
		set_fw_info_part(info, &fi->vendor[5]);
		break;
	/* [한국어] 벤더 영역 6. */
	case SWITCHTEC_IOCTL_PART_VENDOR6:
		set_fw_info_part(info, &fi->vendor[6]);
		break;
	/* [한국어] 벤더 영역 7 - Gen3 파티션 목록의 마지막이다. */
	case SWITCHTEC_IOCTL_PART_VENDOR7:
		set_fw_info_part(info, &fi->vendor[7]);
		break;
	/* [한국어] Gen3 에 없는 파티션 번호(MAP/KEY/BL2)나 정의되지 않은 값. */
	default:
		/* [한국어] 거절한다. */
		return -EINVAL;
	}

	/* [한국어] ★ ACTIVE 판정. 이 파티션의 시작 주소가 하드웨어가 가리키는
	 * ACTIVE 주소와 같으면 이 판이 선택되어 있다는 뜻이다. active_addr 이
	 * 초기값 -1 로 남은 경우(NVLOG/VENDOR)는 결코 일치하지 않는다. */
	if (info->address == active_addr)
		/* [한국어] ACTIVE 비트를 세운다. 위에서 세운 RUNNING 비트와 OR 로 합쳐진다. */
		info->active |= SWITCHTEC_IOCTL_PART_ACTIVE;

	/* [한국어] 성공. */
	return 0;
}

/* [한국어] flash_part_info_gen4 - Gen4/Gen5 레이아웃에서 파티션 상세를 채운다
 * 
 * @stdev: 대상 장치.
 * @info: Gen3 판과 같은 입출력 구조체.
 * @return: 0 성공, -EINVAL 알 수 없는 파티션 번호.
 * 
 * ★ Gen3 판과의 차이:
 *   1) 파티션 종류가 늘었다 - MAP(플래시 배치도), KEY(서명 검증 키),
 *      BL2(2단계 부트로더)가 추가되어 13 -> 19 가 되었다. 보안 부팅을
 *      위한 구성 요소들이다.
 *   2) ACTIVE 판정 방식이 바뀌었다. Gen3 는 '주소를 비교' 했지만 Gen4 는
 *      active_flag 레지스터에 '0 번이냐 1 번이냐' 를 바이트로 적어 둔다.
 *      그래서 switch 바깥의 공통 주소 비교가 없고, 각 case 안에서
 *      ACTIVE 를 직접 판정한다.
 *   3) 그 결과 이 함수에는 active_addr 변수 자체가 없다.
 * 
 * 실행 컨텍스트: 프로세스 문맥, mrpc_mutex 를 쥔 상태.
 * 
 * 호출 체인:
 *   switchtec_dev_ioctl() -> ioctl_flash_part_info() -> [flash_part_info_gen4]
 *     -> set_fw_info_part() */
static int flash_part_info_gen4(struct switchtec_dev *stdev,
		struct switchtec_ioctl_flash_part_info *info)
{
	/* [한국어] Gen4 배치의 플래시 정보 블록. */
	struct flash_info_regs_gen4 __iomem *fi = &stdev->mmio_flash_info->gen4;
	/* [한국어] Gen4 배치의 시스템 정보 블록 - RUNNING 판정용. */
	struct sys_info_regs_gen4 __iomem *si = &stdev->mmio_sys_info->gen4;
	/* [한국어] ACTIVE 선택 플래그 묶음. bl2/cfg/img/key 각각 1 바이트에
	 * '0 번이 활성' 또는 '1 번이 활성' 이 들어 있다. */
	struct active_partition_info_gen4 __iomem *af = &fi->active_flag;

	/* [한국어] 요청 파티션으로 분기. */
	switch (info->flash_partition) {
	/* [한국어] 플래시 배치도 0 - 어느 영역이 무엇인지 적어 둔 표다.
	 * ACTIVE/RUNNING 개념이 없어 위치만 알려 준다. */
	case SWITCHTEC_IOCTL_PART_MAP_0:
		set_fw_info_part(info, &fi->map0);
		break;
	/* [한국어] 플래시 배치도 1(이중화 사본). */
	case SWITCHTEC_IOCTL_PART_MAP_1:
		set_fw_info_part(info, &fi->map1);
		break;
	/* [한국어] 서명 검증 키 파티션 0. */
	case SWITCHTEC_IOCTL_PART_KEY_0:
		/* [한국어] 주소/길이. */
		set_fw_info_part(info, &fi->key0);
		/* [한국어] active_flag 의 key 바이트가 '0 번' 을 가리키는지. */
		if (ioread8(&af->key) == SWITCHTEC_GEN4_KEY0_ACTIVE)
			/* [한국어] ACTIVE 표시. */
			info->active |= SWITCHTEC_IOCTL_PART_ACTIVE;
		/* [한국어] 지금 돌고 있는 키가 0 번인지 - RUNNING 은 별도 레지스터로 본다. */
		if (ioread16(&si->key_running) == SWITCHTEC_GEN4_KEY0_RUNNING)
			/* [한국어] RUNNING 표시. */
			info->active |= SWITCHTEC_IOCTL_PART_RUNNING;
		break;
	/* [한국어] 서명 검증 키 파티션 1 - 위와 대칭이다. */
	case SWITCHTEC_IOCTL_PART_KEY_1:
		/* [한국어] 주소/길이. */
		set_fw_info_part(info, &fi->key1);
		/* [한국어] 1 번이 활성인지. */
		if (ioread8(&af->key) == SWITCHTEC_GEN4_KEY1_ACTIVE)
			info->active |= SWITCHTEC_IOCTL_PART_ACTIVE;
		/* [한국어] 1 번이 돌고 있는지. */
		if (ioread16(&si->key_running) == SWITCHTEC_GEN4_KEY1_RUNNING)
			info->active |= SWITCHTEC_IOCTL_PART_RUNNING;
		break;
	/* [한국어] 2단계 부트로더 파티션 0. */
	case SWITCHTEC_IOCTL_PART_BL2_0:
		/* [한국어] 주소/길이. */
		set_fw_info_part(info, &fi->bl2_0);
		/* [한국어] active_flag 의 bl2 바이트. */
		if (ioread8(&af->bl2) == SWITCHTEC_GEN4_BL2_0_ACTIVE)
			info->active |= SWITCHTEC_IOCTL_PART_ACTIVE;
		/* [한국어] 돌고 있는 BL2 가 0 번인지. */
		if (ioread16(&si->bl2_running) == SWITCHTEC_GEN4_BL2_0_RUNNING)
			info->active |= SWITCHTEC_IOCTL_PART_RUNNING;
		break;
	/* [한국어] 2단계 부트로더 파티션 1. */
	case SWITCHTEC_IOCTL_PART_BL2_1:
		/* [한국어] 주소/길이. */
		set_fw_info_part(info, &fi->bl2_1);
		/* [한국어] 1 번이 활성인지. */
		if (ioread8(&af->bl2) == SWITCHTEC_GEN4_BL2_1_ACTIVE)
			info->active |= SWITCHTEC_IOCTL_PART_ACTIVE;
		/* [한국어] 1 번이 돌고 있는지. */
		if (ioread16(&si->bl2_running) == SWITCHTEC_GEN4_BL2_1_RUNNING)
			info->active |= SWITCHTEC_IOCTL_PART_RUNNING;
		break;
	/* [한국어] 설정 파티션 0 - Gen3 에도 있던 종류지만 판정 방식이 다르다. */
	case SWITCHTEC_IOCTL_PART_CFG0:
		/* [한국어] 주소/길이. */
		set_fw_info_part(info, &fi->cfg0);
		/* [한국어] 주소 비교 대신 active_flag 의 cfg 바이트로 판정한다. */
		if (ioread8(&af->cfg) == SWITCHTEC_GEN4_CFG0_ACTIVE)
			info->active |= SWITCHTEC_IOCTL_PART_ACTIVE;
		/* [한국어] 돌고 있는 설정이 0 번인지. */
		if (ioread16(&si->cfg_running) == SWITCHTEC_GEN4_CFG0_RUNNING)
			info->active |= SWITCHTEC_IOCTL_PART_RUNNING;
		break;
	/* [한국어] 설정 파티션 1. */
	case SWITCHTEC_IOCTL_PART_CFG1:
		/* [한국어] 주소/길이. */
		set_fw_info_part(info, &fi->cfg1);
		/* [한국어] 1 번이 활성인지. */
		if (ioread8(&af->cfg) == SWITCHTEC_GEN4_CFG1_ACTIVE)
			info->active |= SWITCHTEC_IOCTL_PART_ACTIVE;
		/* [한국어] 1 번이 돌고 있는지. */
		if (ioread16(&si->cfg_running) == SWITCHTEC_GEN4_CFG1_RUNNING)
			info->active |= SWITCHTEC_IOCTL_PART_RUNNING;
		break;
	/* [한국어] 펌웨어 이미지 파티션 0. */
	case SWITCHTEC_IOCTL_PART_IMG0:
		/* [한국어] 주소/길이. */
		set_fw_info_part(info, &fi->img0);
		/* [한국어] active_flag 의 img 바이트. */
		if (ioread8(&af->img) == SWITCHTEC_GEN4_IMG0_ACTIVE)
			info->active |= SWITCHTEC_IOCTL_PART_ACTIVE;
		/* [한국어] 돌고 있는 이미지가 0 번인지. */
		if (ioread16(&si->img_running) == SWITCHTEC_GEN4_IMG0_RUNNING)
			info->active |= SWITCHTEC_IOCTL_PART_RUNNING;
		break;
	/* [한국어] 펌웨어 이미지 파티션 1. */
	case SWITCHTEC_IOCTL_PART_IMG1:
		/* [한국어] 주소/길이. */
		set_fw_info_part(info, &fi->img1);
		/* [한국어] 1 번이 활성인지. */
		if (ioread8(&af->img) == SWITCHTEC_GEN4_IMG1_ACTIVE)
			info->active |= SWITCHTEC_IOCTL_PART_ACTIVE;
		/* [한국어] 1 번이 돌고 있는지. */
		if (ioread16(&si->img_running) == SWITCHTEC_GEN4_IMG1_RUNNING)
			info->active |= SWITCHTEC_IOCTL_PART_RUNNING;
		break;
	/* [한국어] 비휘발성 로그 - 위치만. */
	case SWITCHTEC_IOCTL_PART_NVLOG:
		/* [한국어] 주소/길이. */
		set_fw_info_part(info, &fi->nvlog);
		break;
	/* [한국어] 벤더 전용 파티션 0~7. Gen3 와 마찬가지로 위치만 알려 준다. */
	case SWITCHTEC_IOCTL_PART_VENDOR0:
		/* [한국어] 벤더 영역 0. */
		set_fw_info_part(info, &fi->vendor[0]);
		break;
	/* [한국어] 벤더 영역 1. */
	case SWITCHTEC_IOCTL_PART_VENDOR1:
		set_fw_info_part(info, &fi->vendor[1]);
		break;
	/* [한국어] 벤더 영역 2. */
	case SWITCHTEC_IOCTL_PART_VENDOR2:
		set_fw_info_part(info, &fi->vendor[2]);
		break;
	/* [한국어] 벤더 영역 3. */
	case SWITCHTEC_IOCTL_PART_VENDOR3:
		set_fw_info_part(info, &fi->vendor[3]);
		break;
	/* [한국어] 벤더 영역 4. */
	case SWITCHTEC_IOCTL_PART_VENDOR4:
		set_fw_info_part(info, &fi->vendor[4]);
		break;
	/* [한국어] 벤더 영역 5. */
	case SWITCHTEC_IOCTL_PART_VENDOR5:
		set_fw_info_part(info, &fi->vendor[5]);
		break;
	/* [한국어] 벤더 영역 6. */
	case SWITCHTEC_IOCTL_PART_VENDOR6:
		set_fw_info_part(info, &fi->vendor[6]);
		break;
	/* [한국어] 벤더 영역 7 - Gen4 파티션 목록의 마지막. */
	case SWITCHTEC_IOCTL_PART_VENDOR7:
		set_fw_info_part(info, &fi->vendor[7]);
		break;
	/* [한국어] 정의되지 않은 파티션 번호. */
	default:
		/* [한국어] 거절. */
		return -EINVAL;
	}

	/* [한국어] 성공. Gen3 판과 달리 여기서 ACTIVE 를 따로 판정하지 않는다 -
	 * 각 case 가 이미 처리했기 때문이다. */
	return 0;
}

/* [한국어] ioctl_flash_part_info - FLASH_PART_INFO ioctl 의 진입 헬퍼
 * 
 * @stdev: 대상 장치.
 * @uinfo: 유저 버퍼. 입력은 flash_partition, 출력은 address/length/active.
 * @return: 0 성공, -EFAULT 복사 실패, -EOPNOTSUPP 알 수 없는 세대,
 *          -EINVAL 알 수 없는 파티션.
 * 
 * 유저 버퍼를 커널 사본으로 들여오고, 세대에 맞는 구현으로 넘긴 뒤,
 * 결과를 돌려주는 세 단계뿐이다. 세대 분기를 이 층에 두어 아래 두
 * 구현이 세대를 신경 쓰지 않게 했다.
 * 
 * ★ 유저가 준 flash_partition 값의 검증은 여기서 하지 않는다 - 아래
 * switch 문의 default 가 곧 검증이다. 열거된 상수 중 하나가 아니면
 * -EINVAL 로 떨어지므로, 별도의 범위 검사가 필요 없는 구조다.
 * 
 * 실행 컨텍스트: 프로세스 문맥, mrpc_mutex 를 쥔 상태.
 * 
 * 호출 체인:
 *   switchtec_dev_ioctl() -> [ioctl_flash_part_info]
 *     -> flash_part_info_gen3() 또는 flash_part_info_gen4() */
static int ioctl_flash_part_info(struct switchtec_dev *stdev,
		struct switchtec_ioctl_flash_part_info __user *uinfo)
{
	/* [한국어] 하위 구현의 반환값. */
	int ret;
	/* [한국어] 커널 쪽 사본. {0} 초기화로 유저가 채우지 않은 필드(active)가
	 * 0 에서 시작하도록 보장한다 - 아래 구현들이 |= 로만 비트를 세우므로
	 * 초기화가 없으면 스택 쓰레기 위에 OR 을 하게 된다. */
	struct switchtec_ioctl_flash_part_info info = {0};

	/* [한국어] 유저가 지정한 파티션 번호를 읽어 온다. 구조체 전체를 복사하지만
	 * 실제로 입력으로 쓰이는 것은 flash_partition 하나다. */
	if (copy_from_user(&info, uinfo, sizeof(info)))
		/* [한국어] 잘못된 유저 주소. */
		return -EFAULT;

	/* [한국어] Gen3 레이아웃. */
	if (stdev->gen == SWITCHTEC_GEN3) {
		/* [한국어] Gen3 구현으로 넘긴다. */
		ret = flash_part_info_gen3(stdev, &info);
		/* [한국어] 알 수 없는 파티션 번호였다면 */
		if (ret)
			/* [한국어] 그대로 유저에게 전한다. */
			return ret;
	/* [한국어] Gen4 이상(GEN5 포함). */
	} else if (stdev->gen >= SWITCHTEC_GEN4) {
		/* [한국어] Gen4 구현으로 넘긴다. */
		ret = flash_part_info_gen4(stdev, &info);
		/* [한국어] 오류가 났으면 */
		if (ret)
			/* [한국어] 그대로 전한다. */
			return ret;
	/* [한국어] 알 수 없는 세대 - 방어 분기. */
	} else {
		/* [한국어] 해석할 레이아웃을 모른다. */
		return -EOPNOTSUPP;
	}

	/* [한국어] 채워진 결과를 유저 버퍼로 되돌려 준다. */
	if (copy_to_user(uinfo, &info, sizeof(info)))
		/* [한국어] 잘못된 유저 주소. */
		return -EFAULT;

	/* [한국어] 성공. */
	return 0;
}

/* [한국어] ioctl_event_summary - 모든 이벤트 요약 비트맵을 한 번에 긁어 준다
 * 
 * @stdev: 대상 장치.
 * @stuser: 이 fd 의 사용자 문맥. 이벤트 카운터 스냅숏을 여기에 갱신한다.
 * @usum: 유저 버퍼.
 * @size: 복사할 바이트 수. 구형/신형 구조체를 구분하는 유일한 수단이다.
 * @return: 0 성공, -ENOMEM 임시 버퍼 부족, -EFAULT 복사 실패.
 * 
 * ★ 이 함수의 역할 - poll 이 EPOLLPRI 로 '무언가 일어났다' 만 알려 주므로,
 * 유저스페이스는 '무엇이 일어났는지' 를 이걸로 조회한다. 요약 레지스터는
 * 비트맵이라, 어느 파티션/어느 포트에 이벤트가 있는지를 한눈에 보여 준다.
 * 그 뒤 구체적인 내용은 EVENT_CTL 로 하나씩 읽어 간다.
 * 
 * ★ size 인자가 있는 이유(ABI 호환): 처음 설계된 구조체는 pff 배열이
 * 48 개였는데, 실제 하드웨어의 PFF 는 최대 255 개다. 그래서 배열을 넓힌
 * 새 구조체가 도입되었으나, _IOR 의 명령 번호(0x42)는 그대로 두었다 -
 * 번호를 바꾸면 옛 프로그램이 -ENOTTY 를 받기 때문이다. 대신 커널은
 * 호출한 ioctl 상수에 따라 '몇 바이트만 복사할지' 를 달리한다.
 * 큰 구조체를 항상 만들고 앞부분만 잘라 복사하므로, 구형 프로그램은
 * 48 개까지만 받아 간다.
 * 
 * ★ kzalloc 을 쓰는 이유: switchtec_ioctl_event_summary 는 1KB 를 훌쩍
 * 넘는다. 커널 스택(보통 16KB)에 그만한 배열을 두는 것은 위험하므로
 * 힙에 잡는다. 그리고 kzalloc(0 초기화)이어야 한다 - partition_count 와
 * pff_csr_count 가 배열 크기보다 작을 때 나머지 원소가 그대로 유저에게
 * 가는데, 초기화하지 않으면 커널 힙의 잔여 내용이 새어 나간다.
 * 
 * 실행 컨텍스트: 프로세스 문맥, mrpc_mutex 를 쥔 상태. GFP_KERNEL 이라
 * 잠들 수 있다.
 * 
 * 호출 체인:
 *   switchtec_dev_ioctl() -> [ioctl_event_summary] -> ioread32()/ioread64()
 *     -> copy_to_user() */
static int ioctl_event_summary(struct switchtec_dev *stdev,
	struct switchtec_user *stuser,
	struct switchtec_ioctl_event_summary __user *usum,
	size_t size)
{
	/* [한국어] 힙에 잡을 큰 요약 구조체. */
	struct switchtec_ioctl_event_summary *s;
	/* [한국어] 파티션/PFF 순회 인덱스. */
	int i;
	/* [한국어] MMIO 에서 읽은 값의 임시 보관. */
	u32 reg;
	/* [한국어] 반환값. goto 로 빠질 때를 위해 0 으로 시작한다. */
	int ret = 0;

	/* [한국어] 구조체 하나 크기를 0 으로 채워 잡는다. kzalloc_obj(*s) 는
	 * sizeof(*s) 를 컴파일러가 타입에서 뽑아내게 하는 최신 헬퍼로,
	 * 크기와 타입이 어긋나는 실수를 막는다. */
	s = kzalloc_obj(*s);
	/* [한국어] 메모리 부족. */
	if (!s)
		return -ENOMEM;

	/* [한국어] 스위치 전역 이벤트 요약 비트맵. UAPI 구조체의 global 은 __u64 지만
	 * 레지스터는 32 비트라 상위 32 비트는 0 으로 남는다. */
	s->global = ioread32(&stdev->mmio_sw_event->global_summary);
	/* [한국어] 파티션별 이벤트가 있는 파티션을 비트로 표시한 64 비트 맵.
	 * ioread64 는 <linux/io-64-nonatomic-lo-hi.h> 덕분에 64 비트 MMIO 읽기가
	 * 없는 아키텍처에서도 32 비트 두 번으로 자동 대체된다. */
	s->part_bitmap = ioread64(&stdev->mmio_sw_event->part_event_bitmap);
	/* [한국어] 내 파티션의 이벤트 요약 - 위 비트맵에서 내 자리만 따로 뽑아 준
	 * 셈이라 유저가 파티션 번호를 몰라도 자기 이벤트를 볼 수 있다. */
	s->local_part = ioread32(&stdev->mmio_part_cfg->part_event_summary);

	/* [한국어] 모든 파티션의 요약을 배열로 채운다. 상한은 실제 파티션 개수이며,
	 * UAPI 배열 크기 48 보다 작으므로 넘칠 일이 없다. */
	for (i = 0; i < stdev->partition_count; i++) {
		/* [한국어] 각 파티션 설정 블록의 요약 레지스터. */
		reg = ioread32(&stdev->mmio_part_cfg_all[i].part_event_summary);
		/* [한국어] 배열에 담는다. */
		s->part[i] = reg;
	}

	/* [한국어] 모든 PFF 의 요약도 마찬가지로 채운다. 상한은 init_pff 가 센
	 * 실제 개수이며, 신형 UAPI 배열 크기 255 와 같은 SWITCHTEC_MAX_PFF_CSR
	 * 로 묶여 있어 넘치지 않는다. */
	for (i = 0; i < stdev->pff_csr_count; i++) {
		/* [한국어] 각 PFF 의 이벤트 요약 레지스터. */
		reg = ioread32(&stdev->mmio_pff_csr[i].pff_event_summary);
		/* [한국어] 배열에 담는다. */
		s->pff[i] = reg;
	}

	/* [한국어] ★ size 만큼만 복사한다 - 구형 호출이면 pff 배열 48 개까지에서
	 * 잘린다. 큰 구조체를 채워 두고 앞부분만 주는 방식이라, 두 ABI 를
	 * 한 코드로 지원할 수 있다. */
	if (copy_to_user(usum, s, size)) {
		/* [한국어] 잘못된 유저 주소. */
		ret = -EFAULT;
		/* [한국어] 힙 버퍼를 반드시 반납해야 하므로 곧바로 return 하지 않고 라벨로 간다. */
		goto error_case;
	}

	/* [한국어] ★ 유저가 이벤트 요약을 확인했으므로, 이 fd 의 '마지막으로 본
	 * 카운터' 를 현재 값으로 갱신한다. 이렇게 해야 다음 poll 이 곧바로
	 * EPOLLPRI 를 다시 올리지 않는다 - 즉 이 한 줄이 이벤트 알림의
	 * '읽음 표시' 다. 복사가 실패한 경로에서는 갱신하지 않는 점에 유의 -
	 * 유저가 내용을 못 받았으므로 알림을 유지하는 것이 맞다. */
	stuser->event_cnt = atomic_read(&stdev->event_cnt);

/* [한국어] 복사 실패와 정상 종료가 함께 지나가는 정리 지점. */
error_case:
	/* [한국어] 힙 버퍼 반납. */
	kfree(s);
	/* [한국어] 0 또는 -EFAULT. */
	return ret;
}

/* [한국어] global_ev_reg - 스위치 전역 이벤트 레지스터의 주소를 구한다
 * 
 * @stdev: 대상 장치.
 * @offset: sw_event_regs 안에서 그 이벤트 헤더가 갖는 오프셋(offsetof 값).
 * @index: 쓰이지 않는다. 전역 이벤트는 인스턴스가 하나뿐이기 때문이다.
 *         그럼에도 인자로 받는 이유는 세 매핑 함수가 같은 함수 포인터
 *         타입이어야 event_regs[] 표에 함께 담기기 때문이다.
 * @return: 해당 헤더 레지스터의 MMIO 주소.
 * 
 * 왜 함수 포인터로 만드는가(★ 이 표 설계의 핵심): 이벤트마다 헤더가
 * 어느 레지스터 블록에 있는지가 다르다 - 전역 블록, 파티션별 블록,
 * PFF 별 블록. 그리고 뒤 두 종류는 인덱스로 배열을 골라야 한다.
 * 이 차이를 조건문으로 흩뿌리는 대신 '오프셋 + 매핑 함수' 한 쌍으로
 * 표에 담아 두면, 이벤트 처리 코드는 종류를 몰라도 주소를 얻을 수 있다.
 * 게다가 함수 포인터 값 자체를 비교해(map_reg == part_ev_reg) 그
 * 이벤트가 어느 범주인지 되물을 수도 있다 - mask_all_events 와
 * ioctl_event_ctl 이 실제로 그렇게 쓴다.
 * 
 * 실행 컨텍스트: 인터럽트 문맥과 프로세스 문맥 양쪽에서 불린다.
 * 계산만 하므로 어느 쪽이든 안전하다.
 * 
 * 호출 체인:
 *   mask_event() / event_hdr_addr() -> event_regs[].map_reg -> [global_ev_reg] */
static u32 __iomem *global_ev_reg(struct switchtec_dev *stdev,
				  size_t offset, int index)
{
	/* [한국어] 전역 이벤트 블록의 시작 주소에 오프셋을 더한다. void __iomem * 로
	 * 캐스팅하는 이유는 포인터 산술을 바이트 단위로 하기 위해서다 -
	 * struct 포인터에 그냥 더하면 구조체 크기 단위로 뛴다. */
	return (void __iomem *)stdev->mmio_sw_event + offset;
}

/* [한국어] part_ev_reg - 파티션별 이벤트 레지스터의 주소를 구한다
 * 
 * @stdev: 대상 장치.
 * @offset: part_cfg_regs 안에서의 오프셋.
 * @index: 파티션 번호. 호출자가 미리 0 <= index < partition_count 를
 *         보장해야 한다 - 이 함수는 검사하지 않는다.
 * @return: 해당 파티션의 헤더 레지스터 MMIO 주소.
 * 
 * 파티션이란: 하나의 물리 스위치를 여러 논리 스위치처럼 쪼갠 단위로,
 * 각각 다른 호스트에 연결될 수 있다. 파티션마다 설정 레지스터 블록이
 * 한 벌씩 있고, GAS 의 PART_CFG 영역에 연속 배열로 놓여 있다.
 * 
 * ★ 경계 검사가 없는 것이 의도적인 이유: 이 함수는 표를 통해서만
 * 불리고, 유저 입력이 들어오는 경로(event_hdr_addr)가 앞에서 이미
 * 검사한다. 반면 ISR 경로(mask_all_events)는 커널이 만든 인덱스라
 * 검사가 불필요하므로, 검사를 이쪽에 두면 뜨거운 경로에 군더더기가 된다.
 * 
 * 실행 컨텍스트: 인터럽트/프로세스 양쪽.
 * 
 * 호출 체인:
 *   mask_event() / event_hdr_addr() -> event_regs[].map_reg -> [part_ev_reg] */
static u32 __iomem *part_ev_reg(struct switchtec_dev *stdev,
				size_t offset, int index)
{
	/* [한국어] 파티션 배열에서 index 번째 블록의 주소를 구한 뒤 바이트 오프셋을
	 * 더한다. 여기서 &...[index] 는 구조체 크기 곱셈을 컴파일러가 해 주는
	 * 부분이고, 그 결과를 void __iomem * 로 바꿔 오프셋은 바이트로 더한다. */
	return (void __iomem *)&stdev->mmio_part_cfg_all[index] + offset;
}

/* [한국어] pff_ev_reg - PFF(포트)별 이벤트 레지스터의 주소를 구한다
 * 
 * @stdev: 대상 장치.
 * @offset: pff_csr_regs 안에서의 오프셋.
 * @index: PFF 인스턴스 번호. 호출자가 0 <= index < pff_csr_count 를
 *         보장해야 한다.
 * @return: 해당 PFF 의 헤더 레지스터 MMIO 주소.
 * 
 * PFF 는 스위치의 물리 포트 하나가 내보이는 설정 공간 한 벌이다.
 * AER, DPC, 핫플러그, 링크 상태처럼 '포트에서 일어나는' 이벤트가
 * 여기에 모인다.
 * 
 * 실행 컨텍스트: 인터럽트/프로세스 양쪽.
 * 
 * 호출 체인:
 *   mask_event() / event_hdr_addr() -> event_regs[].map_reg -> [pff_ev_reg] */
static u32 __iomem *pff_ev_reg(struct switchtec_dev *stdev,
			       size_t offset, int index)
{
	/* [한국어] PFF 배열의 index 번째 블록 + 바이트 오프셋. */
	return (void __iomem *)&stdev->mmio_pff_csr[index] + offset;
}

/* [한국어] EV_GLB/EV_PAR/EV_PFF - 아래 event_regs[] 표의 한 항목을 만드는 매크로.
 * [i] = {...} 는 C99 의 지정 초기화(designated initializer)로, 배열의
 * i 번째 자리에 값을 놓는다는 뜻이다. i 자리에 UAPI 상수를 넣으므로
 * 표의 인덱스가 곧 이벤트 ID 가 되고, 소스에 적는 순서와 무관해진다.
 * offsetof 로 구조체 안 위치를 컴파일 시점에 계산해 두므로, 헤더의
 * 레지스터 배치가 바뀌어도 이 표는 자동으로 따라간다.
 * 이 줄은 전역 이벤트용 - sw_event_regs 안의 오프셋과 global_ev_reg 쌍. */
#define EV_GLB(i, r)[i] = {offsetof(struct sw_event_regs, r), global_ev_reg}
/* [한국어] 파티션 이벤트용 - part_cfg_regs 안의 오프셋과 part_ev_reg 쌍. */
#define EV_PAR(i, r)[i] = {offsetof(struct part_cfg_regs, r), part_ev_reg}
/* [한국어] PFF 이벤트용 - pff_csr_regs 안의 오프셋과 pff_ev_reg 쌍. */
#define EV_PFF(i, r)[i] = {offsetof(struct pff_csr_regs, r), pff_ev_reg}

/* [한국어] event_reg / event_regs[] - 이벤트 ID 를 '레지스터 위치' 로 바꾸는 표.
 * 구조체를 익명으로 정의하면서 곧바로 배열을 만든다. const 이므로
 * 읽기 전용 섹션에 놓이고 런타임에 바뀌지 않는다 - 그래서 인터럽트
 * 문맥에서 락 없이 읽어도 안전하다. */
static const struct event_reg {
	/* [한국어] offset - 이벤트 헤더 레지스터가 자기 블록 안에서 갖는 바이트 오프셋.
	 * 설정자: EV_* 매크로가 offsetof 로 컴파일 시점에 채운다.
	 * 읽는 자: mask_event(), event_hdr_addr() 가 매핑 함수에 넘긴다.
	 * 값 범위: 해당 레지스터 구조체 크기 안의 오프셋.
	 * 동기화: const 데이터라 변경이 없어 동기화가 필요 없다. */
	size_t offset;
	/* [한국어] map_reg - 오프셋과 인덱스를 실제 MMIO 주소로 바꾸는 함수 포인터.
	 * 설정자: EV_* 매크로가 global_ev_reg/part_ev_reg/pff_ev_reg 중 하나로 채운다.
	 * 읽는 자: mask_event()/event_hdr_addr() 가 호출하고, mask_all_events()와
	 *         ioctl_event_ctl() 은 '값 자체를 비교' 해 이벤트의 범주를 판별한다.
	 * 값 범위: 위 세 함수 중 하나. NULL 은 없다 - 표의 모든 자리가 채워져 있다.
	 * 동기화: const 데이터. */
	u32 __iomem *(*map_reg)(struct switchtec_dev *stdev,
				size_t offset, int index);
} event_regs[] = {
	/* [한국어] 여기부터 전역(스위치 전체에 하나뿐인) 이벤트들.
	 * 스택 오류, PPU 오류, ISP 오류 - 스위치 내부 하드웨어 블록의 오류다.
	 * 이름의 원어 표기는 이 트리 안에서 확인할 근거가 없어 적지 않는다. */
	EV_GLB(SWITCHTEC_IOCTL_EVENT_STACK_ERROR, stack_error_event_hdr),
	EV_GLB(SWITCHTEC_IOCTL_EVENT_PPU_ERROR, ppu_error_event_hdr),
	EV_GLB(SWITCHTEC_IOCTL_EVENT_ISP_ERROR, isp_error_event_hdr),
	/* [한국어] 시스템 리셋, 펌웨어 예외 - 펌웨어가 재시작했거나 예외를 만난 경우.
	 * 드라이버는 내용을 해석하지 않고 유저스페이스에 그대로 넘긴다. */
	EV_GLB(SWITCHTEC_IOCTL_EVENT_SYS_RESET, sys_reset_event_hdr),
	EV_GLB(SWITCHTEC_IOCTL_EVENT_FW_EXC, fw_exception_hdr),
	/* [한국어] 펌웨어 NMI, 비치명 오류 - 심각도별로 항목이 나뉘어 있다. */
	EV_GLB(SWITCHTEC_IOCTL_EVENT_FW_NMI, fw_nmi_hdr),
	EV_GLB(SWITCHTEC_IOCTL_EVENT_FW_NON_FATAL, fw_non_fatal_hdr),
	/* [한국어] 펌웨어 치명 오류, 그리고 TWI(2선식 직렬 버스) 경유 MRPC 완료.
	 * MRPC 는 이 드라이버의 PCIe 경로 말고도 TWI 나 CLI 로도 들어올 수 있고,
	 * 그 완료들이 각각 별도 이벤트로 보고된다. */
	EV_GLB(SWITCHTEC_IOCTL_EVENT_FW_FATAL, fw_fatal_hdr),
	EV_GLB(SWITCHTEC_IOCTL_EVENT_TWI_MRPC_COMP, twi_mrpc_comp_hdr),
	/* [한국어] TWI 경유 비동기 MRPC 완료. */
	EV_GLB(SWITCHTEC_IOCTL_EVENT_TWI_MRPC_COMP_ASYNC,
	       twi_mrpc_comp_async_hdr),
	/* [한국어] CLI(명령행 콘솔) 경유 MRPC 완료. */
	EV_GLB(SWITCHTEC_IOCTL_EVENT_CLI_MRPC_COMP, cli_mrpc_comp_hdr),
	/* [한국어] CLI 경유 비동기 MRPC 완료. */
	EV_GLB(SWITCHTEC_IOCTL_EVENT_CLI_MRPC_COMP_ASYNC,
	       cli_mrpc_comp_async_hdr),
	/* [한국어] GPIO 인터럽트, 그리고 GFMS 이벤트 - 전역 이벤트 목록의 끝이다. */
	EV_GLB(SWITCHTEC_IOCTL_EVENT_GPIO_INT, gpio_interrupt_hdr),
	EV_GLB(SWITCHTEC_IOCTL_EVENT_GFMS, gfms_event_hdr),
	/* [한국어] 여기부터 파티션별 이벤트. 파티션 리셋 - 논리 스위치 하나가
	 * 재설정되었음을 알린다. */
	EV_PAR(SWITCHTEC_IOCTL_EVENT_PART_RESET, part_reset_hdr),
	/* [한국어] ★ 이 항목이 이 드라이버의 주 경로다. mrpc_comp_hdr 은 내 파티션의
	 * MRPC 완료 헤더이며, switchtec_event_isr 가 표를 거치지 않고 직접
	 * 읽는 바로 그 레지스터다. 그래서 ISR 의 일반 마스킹 루프에서는
	 * 이 ID 를 명시적으로 건너뛴다. */
	EV_PAR(SWITCHTEC_IOCTL_EVENT_MRPC_COMP, mrpc_comp_hdr),
	/* [한국어] 비동기 MRPC 완료 - 명령을 낸 쪽이 기다리지 않는 형태. */
	EV_PAR(SWITCHTEC_IOCTL_EVENT_MRPC_COMP_ASYNC, mrpc_comp_async_hdr),
	/* [한국어] 동적 파티션 바인딩 완료 - 파티션 구성을 런타임에 바꿀 때 쓰인다. */
	EV_PAR(SWITCHTEC_IOCTL_EVENT_DYN_PART_BIND_COMP, dyn_binding_hdr),
	/* [한국어] 파티션 간 통신 요청 알림 - 파티션별 이벤트 목록의 끝. */
	EV_PAR(SWITCHTEC_IOCTL_EVENT_INTERCOMM_REQ_NOTIFY,
	       intercomm_notify_hdr),
	/* [한국어] 여기부터 PFF(포트)별 이벤트. AER(Advanced Error Reporting)이
	 * P2P 브리지 쪽에서 잡힌 경우와 */
	EV_PFF(SWITCHTEC_IOCTL_EVENT_AER_IN_P2P, aer_in_p2p_hdr),
	/* [한국어] VEP(가상 엔드포인트) 쪽에서 잡힌 경우로 나뉜다. */
	EV_PFF(SWITCHTEC_IOCTL_EVENT_AER_IN_VEP, aer_in_vep_hdr),
	/* [한국어] DPC(Downstream Port Containment) - 오류가 난 하위 포트를 즉시
	 * 차단해 오염이 퍼지지 않게 하는 PCIe 기능이 발동한 경우. */
	EV_PFF(SWITCHTEC_IOCTL_EVENT_DPC, dpc_hdr),
	/* [한국어] CTS(Completion Timeout Synthesis 계열) 와 */
	EV_PFF(SWITCHTEC_IOCTL_EVENT_CTS, cts_hdr),
	/* [한국어] UEC - 포트에서 잡힌 오류 종류들이다. 이 약어들의 정확한 원어는
	 * 이 트리 안에 근거가 없어 적지 않는다. */
	EV_PFF(SWITCHTEC_IOCTL_EVENT_UEC, uec_hdr),
	/* [한국어] 핫플러그 - 하위 포트에 장치가 꽂히거나 빠진 경우. NVMe 백플레인에
	 * 드라이브를 넣고 빼는 것이 여기에 해당한다. */
	EV_PFF(SWITCHTEC_IOCTL_EVENT_HOTPLUG, hotplug_hdr),
	/* [한국어] IER, 임계값 이벤트. */
	EV_PFF(SWITCHTEC_IOCTL_EVENT_IER, ier_hdr),
	EV_PFF(SWITCHTEC_IOCTL_EVENT_THRESH, threshold_hdr),
	/* [한국어] 전력 관리 상태 변화, 그리고 TLP 스로틀링 - 트래픽이 과할 때
	 * 스위치가 흐름을 줄인 경우. */
	EV_PFF(SWITCHTEC_IOCTL_EVENT_POWER_MGMT, power_mgmt_hdr),
	EV_PFF(SWITCHTEC_IOCTL_EVENT_TLP_THROTTLING, tlp_throttling_hdr),
	/* [한국어] 링크 속도 강제, 크레딧 타임아웃. */
	EV_PFF(SWITCHTEC_IOCTL_EVENT_FORCE_SPEED, force_speed_hdr),
	EV_PFF(SWITCHTEC_IOCTL_EVENT_CREDIT_TIMEOUT, credit_timeout_hdr),
	/* [한국어] ★ 링크 상태 변화. 이 ID 도 ISR 의 일반 마스킹 루프에서 제외되며,
	 * 대신 check_link_state_events() 가 헤더 안의 변화 횟수를 추적한다. */
	EV_PFF(SWITCHTEC_IOCTL_EVENT_LINK_STATE, link_state_hdr),
};

/* [한국어] event_hdr_addr - 유저스페이스가 준 (이벤트 ID, 인덱스)를 검증하고
 *                  헤더 레지스터 주소로 바꾼다
 * 
 * @stdev: 대상 장치.
 * @event_id: 유저스페이스가 준 이벤트 종류. 신뢰할 수 없는 값이다.
 * @index: 유저스페이스가 준 인스턴스 번호. 역시 신뢰할 수 없다.
 *         SWITCHTEC_IOCTL_EVENT_LOCAL_PART_IDX(-1)는 '내 파티션' 을 뜻하는
 *         특수 값이다.
 * @return: 유효하면 MMIO 주소. 잘못된 입력이면 ERR_PTR(-EINVAL) 을
 *          주소 타입으로 캐스팅해 돌려준다. 호출자는 IS_ERR 로 판정한다.
 * 
 * ★ 이 함수가 존재하는 이유는 오직 '검증' 이다. 표를 인덱싱하고 매핑
 * 함수를 부르는 일은 mask_event 도 하지만, 그쪽은 커널이 만든 인덱스라
 * 검사가 없다. 유저스페이스에서 온 값은 반드시 여기를 거쳐야 한다 -
 * 검사를 빠뜨리면 event_regs[] 배열 밖을 읽거나, MMIO 매핑 범위를
 * 벗어난 주소를 만들어 커널이 잘못된 물리 주소를 건드리게 된다.
 * 
 * 세 가지 검사를 한다:
 *   1) event_id 가 표의 범위 안인가
 *   2) 파티션 이벤트라면 index 가 파티션 개수 안인가(-1 은 내 파티션으로 치환)
 *   3) PFF 이벤트라면 index 가 PFF 개수 안인가
 * 전역 이벤트는 index 를 쓰지 않으므로 검사가 필요 없다.
 * 
 * ERR_PTR 을 주소 타입으로 캐스팅하는 관용구: 커널은 커널 주소 공간의
 * 마지막 4KB 를 오류 코드 전용으로 비워 두므로, 유효 포인터와 오류를
 * 같은 워드에 담아도 구분할 수 있다.
 * 
 * 실행 컨텍스트: 프로세스 문맥(ioctl 경로), mrpc_mutex 를 쥔 상태.
 * 
 * 호출 체인:
 *   switchtec_dev_ioctl() -> ioctl_event_ctl() -> event_ctl()
 *     -> [event_hdr_addr] -> event_regs[].map_reg() */
static u32 __iomem *event_hdr_addr(struct switchtec_dev *stdev,
				   int event_id, int index)
{
	/* [한국어] 표에서 꺼낼 오프셋. */
	size_t off;

	/* [한국어] ★ 1) 표 범위 검사. 아래에서 event_regs[event_id] 로 인덱싱하기
	 * 전에 반드시 해야 한다. 음수 검사도 함께 하는 이유는 int 인자라
	 * 유저스페이스가 음수를 넣을 수 있기 때문이다. */
	if (event_id < 0 || event_id >= SWITCHTEC_IOCTL_MAX_EVENTS)
		/* [한국어] 잘못된 ID - 오류를 포인터 모양으로 실어 돌려준다. */
		return (u32 __iomem *)ERR_PTR(-EINVAL);

	/* [한국어] 검증을 통과했으니 이제 안전하게 표를 읽는다. */
	off = event_regs[event_id].offset;

	/* [한국어] ★ 2) 이 이벤트가 파티션별인가를 매핑 함수의 '주소 비교' 로 판별한다.
	 * 표에 종류 태그를 따로 두지 않고 함수 포인터 값 자체를 식별자로 쓰는
	 * 관용구다. */
	if (event_regs[event_id].map_reg == part_ev_reg) {
		/* [한국어] -1 은 '내 파티션' 을 뜻하는 약속된 특수 값이다. 유저스페이스는
		 * 자기가 몇 번 파티션에 붙어 있는지 몰라도 이 값을 쓸 수 있다. */
		if (index == SWITCHTEC_IOCTL_EVENT_LOCAL_PART_IDX)
			/* [한국어] 드라이버가 알고 있는 실제 파티션 번호로 바꾼다. */
			index = stdev->partition;
		/* [한국어] 특수 값이 아니라면 실제 범위 안이어야 한다. 음수 검사도 필수다 -
		 * __s32 필드라 유저가 음수를 넣을 수 있고, 그대로 배열 인덱스가 되면
		 * 매핑 범위 앞쪽의 엉뚱한 주소를 만든다. */
		else if (index < 0 || index >= stdev->partition_count)
			/* [한국어] 범위를 벗어난 파티션 번호. */
			return (u32 __iomem *)ERR_PTR(-EINVAL);
	/* [한국어] ★ 3) PFF 별 이벤트인 경우. */
	} else if (event_regs[event_id].map_reg == pff_ev_reg) {
		/* [한국어] 역시 음수와 상한을 함께 검사한다. */
		if (index < 0 || index >= stdev->pff_csr_count)
			/* [한국어] 범위를 벗어난 PFF 번호. */
			return (u32 __iomem *)ERR_PTR(-EINVAL);
	}

	/* [한국어] 세 검사를 모두 통과했다. 이제 표의 매핑 함수가 안전하게 주소를
	 * 계산할 수 있다. 전역 이벤트라면 index 는 무시된다. */
	return event_regs[event_id].map_reg(stdev, off, index);
}

/* [한국어] event_ctl - 이벤트 하나의 상태를 읽고, 요청된 활성 플래그를 반영한다
 * 
 * @stdev: 대상 장치.
 * @ctl: 유저스페이스가 채워 보낸 요청이자, 결과를 돌려줄 버퍼.
 *       입력으로는 event_id/index/flags 를, 출력으로는 occurred/count/
 *       data[5]/flags 를 쓴다.
 * @return: 0 성공. -EINVAL 잘못된 ID/인덱스, -EOPNOTSUPP 하드웨어가
 *          지원하지 않는 이벤트.
 * 
 * ★ 이 함수의 구조는 '읽기 -> 수정 -> 쓰기 -> 되읽어 보고' 다.
 *   (a) 헤더와 뒤따르는 data[5] 를 읽어 유저에게 줄 정보를 채운다
 *   (b) 요청된 플래그에 따라 헤더 값을 비트 단위로 고친다
 *   (c) 요청이 하나라도 있었으면 되쓴다
 *   (d) 최종 상태를 flags 로 다시 채워 유저에게 알려 준다
 * (d) 가 중요한 이유: 유저는 '켜 달라' 고만 요청하고, 실제로 어떤 상태가
 * 되었는지는 이 반환 플래그로 확인한다.
 * 
 * ★ CLEAR 비트의 미묘함: SWITCHTEC_EVENT_CLEAR 와 SWITCHTEC_EVENT_OCCURRED
 * 는 둘 다 BIT(0) 인 같은 비트다. 읽으면 '발생함', 1 을 쓰면 '지움'.
 * 그래서 유저가 CLEAR 를 요청하지 않았다면, 읽어 온 값에 그 비트가 서
 * 있더라도 되쓰기 전에 반드시 0 으로 내려야 한다 - 그러지 않으면
 * 되쓰기가 의도치 않게 이벤트를 지워 버린다. 이것이 아래 첫 if 의 정체다.
 * 
 * 실행 컨텍스트: 프로세스 문맥, mrpc_mutex 를 쥔 상태.
 * 
 * 호출 체인:
 *   switchtec_dev_ioctl() -> ioctl_event_ctl() -> [event_ctl]
 *     -> event_hdr_addr() -> ioread32()/iowrite32() */
static int event_ctl(struct switchtec_dev *stdev,
		     struct switchtec_ioctl_event_ctl *ctl)
{
	/* [한국어] data[] 를 채울 루프 인덱스. */
	int i;
	/* [한국어] 헤더 레지스터의 MMIO 주소. */
	u32 __iomem *reg;
	/* [한국어] 읽어 와 고칠 헤더 값. */
	u32 hdr;

	/* [한국어] 유저가 준 ID/인덱스를 검증하고 주소로 바꾼다. */
	reg = event_hdr_addr(stdev, ctl->event_id, ctl->index);
	/* [한국어] 검증 실패는 오류가 실린 포인터로 온다. */
	if (IS_ERR(reg))
		/* [한국어] 포인터에 실린 오류 코드를 꺼내 그대로 반환한다. */
		return PTR_ERR(reg);

	/* [한국어] 헤더를 한 번 읽어 둔다. 이후 모든 판단이 이 스냅숏 위에서 이뤄진다. */
	hdr = ioread32(reg);
	/* [한국어] BIT(31) - 이 하드웨어/펌웨어가 지원하지 않는 이벤트. */
	if (hdr & SWITCHTEC_EVENT_NOT_SUPP)
		/* [한국어] 지원하지 않음을 알린다. 아래 IDX_ALL 순회는 이 오류만은
		 * 치명적으로 보지 않고 계속 진행한다. */
		return -EOPNOTSUPP;

	/* [한국어] 헤더 바로 뒤에 이어지는 이벤트 데이터 워드들. UAPI 구조체가
	 * data[5] 로 정해 두었고, 레지스터 구조체들도 hdr 뒤에 data[5] 를
	 * 두고 있어 개수가 맞는다. */
	for (i = 0; i < ARRAY_SIZE(ctl->data); i++)
		/* [한국어] reg[0] 이 헤더이므로 데이터는 reg[1] 부터다. u32 포인터라
		 * [i+1] 이 4 바이트씩 뛴다. */
		ctl->data[i] = ioread32(&reg[i + 1]);

	/* [한국어] 발생 비트를 그대로 유저에게 알린다. 값은 0 또는 BIT(0). */
	ctl->occurred = hdr & SWITCHTEC_EVENT_OCCURRED;
	/* [한국어] 발생 횟수 - 비트 5~12 에 8 비트로 들어 있다. 링크 상태 이벤트에서
	 * check_link_state_events 가 쓰는 것과 같은 자리다. */
	ctl->count = (hdr >> 5) & 0xFF;

	/* [한국어] ★ 위 설명의 그 검사. 유저가 '지워 달라' 고 하지 않았다면 */
	if (!(ctl->flags & SWITCHTEC_IOCTL_EVENT_FLAG_CLEAR))
		/* [한국어] 되쓸 값에서 그 비트를 반드시 내려, 되쓰기가 이벤트를 지우지
		 * 않게 한다. 반대로 CLEAR 를 요청했다면 읽어 온 1 이 그대로 남아
		 * 되쓰기가 곧 지우기가 된다. */
		hdr &= ~SWITCHTEC_EVENT_CLEAR;
	/* [한국어] '폴링(인터럽트) 활성' 요청. */
	if (ctl->flags & SWITCHTEC_IOCTL_EVENT_FLAG_EN_POLL)
		/* [한국어] IRQ 활성 비트(BIT(3))를 켠다. ISR 가 마스킹으로 내려 둔 것을
		 * 유저가 이렇게 다시 켠다 - 이 드라이버 이벤트 모델의 되살리기 경로다. */
		hdr |= SWITCHTEC_EVENT_EN_IRQ;
	/* [한국어] '폴링 비활성' 요청. */
	if (ctl->flags & SWITCHTEC_IOCTL_EVENT_FLAG_DIS_POLL)
		/* [한국어] IRQ 활성 비트를 내린다. */
		hdr &= ~SWITCHTEC_EVENT_EN_IRQ;
	/* [한국어] '로그 기록 활성' 요청 - 펌웨어가 이 이벤트를 자체 로그에 남긴다. */
	if (ctl->flags & SWITCHTEC_IOCTL_EVENT_FLAG_EN_LOG)
		/* [한국어] 로그 비트(BIT(1))를 켠다. */
		hdr |= SWITCHTEC_EVENT_EN_LOG;
	/* [한국어] '로그 비활성' 요청. */
	if (ctl->flags & SWITCHTEC_IOCTL_EVENT_FLAG_DIS_LOG)
		/* [한국어] 로그 비트를 내린다. */
		hdr &= ~SWITCHTEC_EVENT_EN_LOG;
	/* [한국어] 'CLI 통지 활성' 요청 - 펌웨어의 명령행 콘솔에 알린다. */
	if (ctl->flags & SWITCHTEC_IOCTL_EVENT_FLAG_EN_CLI)
		/* [한국어] CLI 비트(BIT(2))를 켠다. */
		hdr |= SWITCHTEC_EVENT_EN_CLI;
	/* [한국어] 'CLI 비활성' 요청. */
	if (ctl->flags & SWITCHTEC_IOCTL_EVENT_FLAG_DIS_CLI)
		/* [한국어] CLI 비트를 내린다. */
		hdr &= ~SWITCHTEC_EVENT_EN_CLI;
	/* [한국어] '치명 표시 활성' 요청 - 이 이벤트를 치명적 오류로 다루게 한다. */
	if (ctl->flags & SWITCHTEC_IOCTL_EVENT_FLAG_EN_FATAL)
		/* [한국어] 치명 비트(BIT(4))를 켠다. */
		hdr |= SWITCHTEC_EVENT_FATAL;
	/* [한국어] '치명 표시 비활성' 요청. */
	if (ctl->flags & SWITCHTEC_IOCTL_EVENT_FLAG_DIS_FATAL)
		/* [한국어] 치명 비트를 내린다. */
		hdr &= ~SWITCHTEC_EVENT_FATAL;

	/* [한국어] 플래그 요청이 하나도 없었다면 순수한 조회이므로 쓰지 않는다.
	 * 불필요한 MMIO 쓰기를 피하는 것뿐 아니라, 되쓰기 자체가 부작용을
	 * 가질 수 있으므로(위 CLEAR 비트) 의미 있는 절약이다. */
	if (ctl->flags)
		/* [한국어] 고친 값을 되쓴다. */
		iowrite32(hdr, reg);

	/* [한국어] 이제 flags 를 '요청' 에서 '현재 상태' 로 갈아 끼운다. 먼저 비운다. */
	ctl->flags = 0;
	/* [한국어] 되쓴 값 기준으로 IRQ 가 켜져 있으면 */
	if (hdr & SWITCHTEC_EVENT_EN_IRQ)
		/* [한국어] EN_POLL 로 알린다. */
		ctl->flags |= SWITCHTEC_IOCTL_EVENT_FLAG_EN_POLL;
	/* [한국어] 로그가 켜져 있으면 */
	if (hdr & SWITCHTEC_EVENT_EN_LOG)
		/* [한국어] EN_LOG 로 알린다. */
		ctl->flags |= SWITCHTEC_IOCTL_EVENT_FLAG_EN_LOG;
	/* [한국어] CLI 가 켜져 있으면 */
	if (hdr & SWITCHTEC_EVENT_EN_CLI)
		/* [한국어] EN_CLI 로 알린다. */
		ctl->flags |= SWITCHTEC_IOCTL_EVENT_FLAG_EN_CLI;
	/* [한국어] 치명 표시가 켜져 있으면 */
	if (hdr & SWITCHTEC_EVENT_FATAL)
		/* [한국어] EN_FATAL 로 알린다. */
		ctl->flags |= SWITCHTEC_IOCTL_EVENT_FLAG_EN_FATAL;

	/* [한국어] 성공. 호출자가 이 ctl 을 유저스페이스로 복사해 돌려준다. */
	return 0;
}

/* [한국어] ioctl_event_ctl - EVENT_CTL ioctl 의 진입 헬퍼. 유저 버퍼를 다루고
 *                   '전체 인덱스' 요청을 풀어 준다
 * 
 * @stdev: 대상 장치.
 * @uctl: 유저스페이스 버퍼.
 * @return: 0 성공, -EFAULT 복사 실패, -EINVAL 잘못된 ID/플래그,
 *          그 외 event_ctl 이 낸 오류.
 * 
 * ★ SWITCHTEC_IOCTL_EVENT_IDX_ALL(-2) 처리가 이 함수의 존재 이유다.
 * 유저스페이스가 '이 종류의 모든 인스턴스에 같은 설정을 걸어 달라' 고
 * 할 수 있는데, 인스턴스가 몇 개인지는 커널만 안다(파티션 수, PFF 수).
 * 그래서 여기서 개수를 알아내 루프를 돌린다.
 * 
 * ★ 루프 안에서 ctl.flags 를 매번 되살리는 이유(미묘한 버그 방지):
 * event_ctl 은 flags 를 '요청' 으로 읽고 '현재 상태' 로 덮어쓴다. 즉 첫
 * 반복이 끝나면 ctl.flags 는 이미 요청이 아니라 결과다. 그대로 두 번째
 * 반복에 넘기면 엉뚱한 설정을 걸게 되므로, 원래 요청을 event_flags 에
 * 따로 보관해 두고 매 반복 앞에서 복원한다.
 * 
 * -EOPNOTSUPP 를 넘기는 이유: 인스턴스마다 지원 여부가 다를 수 있다.
 * 어떤 포트가 그 이벤트를 지원하지 않는다고 해서 전체 요청을 실패로
 * 만들 이유는 없으므로, 그 오류만은 무시하고 계속 진행한다.
 * 
 * 유저에게 돌려주는 ctl 은 '마지막 반복의 결과' 라는 점에 유의.
 * 
 * 실행 컨텍스트: 프로세스 문맥, mrpc_mutex 를 쥔 상태.
 * 
 * 호출 체인:
 *   switchtec_dev_ioctl() -> [ioctl_event_ctl] -> event_ctl() */
static int ioctl_event_ctl(struct switchtec_dev *stdev,
	struct switchtec_ioctl_event_ctl __user *uctl)
{
	/* [한국어] event_ctl 의 반환값. */
	int ret;
	/* [한국어] IDX_ALL 일 때 돌 인스턴스 개수. */
	int nr_idxs;
	/* [한국어] 원래 요청 플래그의 보관소 - 위 설명의 그 변수다. */
	unsigned int event_flags;
	/* [한국어] 커널 쪽 사본. 유저 포인터를 직접 역참조하지 않기 위한 것으로,
	 * 검사와 사용 사이에 유저가 값을 바꾸는 TOCTOU 를 원천 차단한다. */
	struct switchtec_ioctl_event_ctl ctl;

	/* [한국어] 유저 버퍼를 커널 사본으로 복사한다. 이 시점 이후 uctl 은
	 * 마지막 복사 때까지 건드리지 않는다. */
	if (copy_from_user(&ctl, uctl, sizeof(ctl)))
		/* [한국어] 잘못된 유저 주소. */
		return -EFAULT;

	/* [한국어] 표 범위 검사. __u32 필드라 음수는 없고 상한만 보면 된다.
	 * 아래에서 event_regs[ctl.event_id] 를 인덱싱하기 전에 반드시 필요하다. */
	if (ctl.event_id >= SWITCHTEC_IOCTL_MAX_EVENTS)
		/* [한국어] 범위를 벗어난 이벤트 ID. */
		return -EINVAL;

	/* [한국어] UNUSED 는 정의된 9 개 플래그를 뺀 나머지 비트 전부(~0x1ff)를
	 * 가리키는 마스크다. 미정의 비트가 하나라도 서 있으면 거절한다 -
	 * 장래에 그 비트에 의미가 생겼을 때 옛 프로그램이 우연히 새 기능을
	 * 켜 버리는 사고를 막는 ABI 방어다. */
	if (ctl.flags & SWITCHTEC_IOCTL_EVENT_FLAG_UNUSED)
		/* [한국어] 알 수 없는 플래그. */
		return -EINVAL;

	/* [한국어] -2 는 '이 종류의 모든 인스턴스' 를 뜻한다. */
	if (ctl.index == SWITCHTEC_IOCTL_EVENT_IDX_ALL) {
		/* [한국어] 전역 이벤트라면 */
		if (event_regs[ctl.event_id].map_reg == global_ev_reg)
			/* [한국어] 인스턴스가 하나뿐이다. */
			nr_idxs = 1;
		/* [한국어] 파티션 이벤트라면 */
		else if (event_regs[ctl.event_id].map_reg == part_ev_reg)
			/* [한국어] 파티션 개수만큼. */
			nr_idxs = stdev->partition_count;
		/* [한국어] PFF 이벤트라면 */
		else if (event_regs[ctl.event_id].map_reg == pff_ev_reg)
			/* [한국어] PFF 개수만큼. */
			nr_idxs = stdev->pff_csr_count;
		/* [한국어] 세 범주 어디에도 속하지 않는 경우 - 표가 완전하므로 현재는
		 * 도달할 수 없지만, 표에 빈 자리가 생기면 map_reg 가 NULL 이 되어
		 * 여기로 온다. 그 NULL 을 호출하기 전에 막는 방어다. */
		else
			/* [한국어] 알 수 없는 범주. */
			return -EINVAL;

		/* [한국어] ★ 원래 요청 플래그를 보관한다. 아래에서 매 반복마다 복원한다. */
		event_flags = ctl.flags;
		/* [한국어] ctl.index 를 루프 변수로 그대로 쓴다 - 마지막 반복의 인덱스가
		 * 유저에게 돌아간다는 뜻이기도 하다. */
		for (ctl.index = 0; ctl.index < nr_idxs; ctl.index++) {
			/* [한국어] ★ 요청 플래그 복원. 이것이 없으면 두 번째 반복부터
			 * '직전 결과' 를 요청으로 착각한다. */
			ctl.flags = event_flags;
			/* [한국어] 이 인스턴스에 설정을 건다. */
			ret = event_ctl(stdev, &ctl);
			/* [한국어] 오류가 났고, 그것이 '지원하지 않음' 이 아니라면 진짜 오류다. */
			if (ret < 0 && ret != -EOPNOTSUPP)
				/* [한국어] 중단하고 오류를 전한다. -EOPNOTSUPP 였다면 무시하고 다음 인스턴스로. */
				return ret;
		}
	/* [한국어] 인덱스가 구체적으로 지정된 보통의 경우. */
	} else {
		/* [한국어] 그 하나만 처리한다. 인덱스 검증은 event_ctl -> event_hdr_addr 이
		 * 한다 - 여기서 미리 검사하지 않는 이유는 -1(내 파티션) 같은 특수 값
		 * 해석이 그쪽에 있기 때문이다. */
		ret = event_ctl(stdev, &ctl);
		/* [한국어] 단일 요청에서는 -EOPNOTSUPP 도 그대로 오류로 돌려준다 -
		 * 유저가 특정 인스턴스를 콕 집었으니 지원 여부를 알려 주는 것이 맞다. */
		if (ret < 0)
			return ret;
	}

	/* [한국어] 결과(occurred/count/data/flags 가 채워진 ctl)를 유저에게 돌려준다. */
	if (copy_to_user(uctl, &ctl, sizeof(ctl)))
		/* [한국어] 복사 실패. */
		return -EFAULT;

	/* [한국어] 성공. */
	return 0;
}

/* [한국어] ioctl_pff_to_port - PFF 인스턴스 번호로 (파티션, 포트 번호)를 역조회한다
 * 
 * @stdev: 대상 장치.
 * @up: 유저 버퍼. 입력은 pff 필드, 출력은 partition 과 port 필드다.
 * @return: 0 성공(찾지 못해도 성공이다), -EFAULT 복사 실패.
 * 
 * 왜 필요한가: 이벤트 요약이나 EVENT_CTL 은 PFF 인스턴스 번호로 말한다.
 * 하지만 사람이 보는 것은 '몇 번 파티션의 몇 번 포트' 다. 이 변환표는
 * 파티션 설정 레지스터 안에 흩어져 있고(usp/vep/dsp 인스턴스 ID), 역방향
 * 조회 수단이 하드웨어에 없으므로 드라이버가 전수 조사로 찾는다.
 * 
 * 포트 번호 규약:
 *   0                        = USP(업스트림, 호스트 쪽)
 *   SWITCHTEC_IOCTL_PFF_VEP(100) = VEP(이 관리 엔드포인트 자신)
 *   1..47                    = DSP(다운스트림) 포트, 배열 인덱스 + 1
 *   -1                       = 찾지 못함
 * 
 * ★ 찾지 못해도 0 을 돌려주는 점에 유의. 유저스페이스는 port 가 -1 인지
 * 보고 판단해야 한다. 다만 partition 필드에는 마지막으로 훑은 파티션
 * 번호가 남는다는 점도 알아 둘 만하다 - 못 찾았을 때 그 값은 의미가 없다.
 * 
 * 실행 컨텍스트: 프로세스 문맥, mrpc_mutex 를 쥔 상태. MMIO 읽기만 한다.
 * 
 * 호출 체인:
 *   switchtec_dev_ioctl() -> [ioctl_pff_to_port] -> ioread32() */
static int ioctl_pff_to_port(struct switchtec_dev *stdev,
			     struct switchtec_ioctl_pff_port __user *up)
{
	/* [한국어] DSP 배열 인덱스와 파티션 인덱스. */
	int i, part;
	/* [한국어] 읽어 온 인스턴스 ID. */
	u32 reg;
	/* [한국어] 현재 훑는 파티션의 설정 블록. */
	struct part_cfg_regs __iomem *pcfg;
	/* [한국어] 커널 쪽 사본. */
	struct switchtec_ioctl_pff_port p;

	/* [한국어] 유저가 준 PFF 번호를 읽어 온다. */
	if (copy_from_user(&p, up, sizeof(p)))
		/* [한국어] 잘못된 유저 주소. */
		return -EFAULT;

	/* [한국어] '못 찾음' 으로 초기화. 아래 루프가 찾으면 덮어쓴다. */
	p.port = -1;
	/* [한국어] 모든 파티션을 훑는다 - PFF 는 스위치 전체에서 유일한 번호지만
	 * 어느 파티션에 속하는지는 모르기 때문이다. */
	for (part = 0; part < stdev->partition_count; part++) {
		/* [한국어] 이 파티션의 설정 블록. */
		pcfg = &stdev->mmio_part_cfg_all[part];
		/* [한국어] 찾았을 때 알려 줄 파티션 번호를 미리 채워 둔다. */
		p.partition = part;

		/* [한국어] 업스트림 포트의 PFF 인스턴스 ID. */
		reg = ioread32(&pcfg->usp_pff_inst_id);
		/* [한국어] 찾는 번호와 같으면 */
		if (reg == p.pff) {
			/* [한국어] 포트 0 - 업스트림이다. */
			p.port = 0;
			/* [한국어] 더 볼 필요 없이 루프를 빠진다. */
			break;
		}

		/* [한국어] 가상 엔드포인트의 PFF 인스턴스 ID. 하위 8 비트만 ID 다. */
		reg = ioread32(&pcfg->vep_pff_inst_id) & 0xFF;
		/* [한국어] 일치하면 */
		if (reg == p.pff) {
			/* [한국어] 약속된 특수 번호 100 으로 알린다 - 실제 포트 번호와 겹치지
			 * 않도록 DSP 최대치(47)보다 훨씬 큰 값을 골랐다. */
			p.port = SWITCHTEC_IOCTL_PFF_VEP;
			/* [한국어] 루프 종료. */
			break;
		}

		/* [한국어] 다운스트림 포트들을 훑는다. 개수는 레지스터 구조체가 정한 47. */
		for (i = 0; i < ARRAY_SIZE(pcfg->dsp_pff_inst_id); i++) {
			/* [한국어] 각 하위 포트의 PFF 인스턴스 ID. */
			reg = ioread32(&pcfg->dsp_pff_inst_id[i]);
			/* [한국어] 찾는 번호가 아니면 */
			if (reg != p.pff)
				/* [한국어] 다음 포트로. */
				continue;

			/* [한국어] 찾았다. DSP 배열 인덱스 0 이 포트 1 이므로 1 을 더한다 -
			 * 포트 0 은 업스트림 자리이기 때문이다. */
			p.port = i + 1;
			/* [한국어] 안쪽 루프 종료. */
			break;
		}

		/* [한국어] 안쪽 루프에서 찾았다면 port 가 -1 이 아니다. */
		if (p.port != -1)
			/* [한국어] 바깥 파티션 루프도 종료한다. break 가 안쪽 루프만 빠져나오므로
			 * 이 검사가 필요하다. */
			break;
	}

	/* [한국어] 찾은 결과(또는 못 찾음)를 유저에게 돌려준다. */
	if (copy_to_user(up, &p, sizeof(p)))
		/* [한국어] 복사 실패. */
		return -EFAULT;

	/* [한국어] 성공. 못 찾은 경우도 여기로 온다. */
	return 0;
}

/* [한국어] ioctl_port_to_pff - (파티션, 포트 번호)로 PFF 인스턴스 번호를 구한다
 * 
 * @stdev: 대상 장치.
 * @up: 유저 버퍼. 입력은 partition 과 port, 출력은 pff 다.
 * @return: 0 성공, -EINVAL 잘못된 파티션/포트, -EFAULT 복사 실패.
 * 
 * 위 함수의 정방향이다. 이쪽은 하드웨어가 표를 그 방향으로 갖고 있어
 * 전수 조사 없이 한 번의 MMIO 읽기로 끝난다.
 * 
 * ★ array_index_nospec 이 여기 있는 이유(Spectre v1 방어):
 * p.port 는 유저스페이스가 준 값이고, 바로 아래에서 배열 인덱스로 쓰인다.
 * CPU 는 위 범위 검사를 '통과할 것' 이라고 투기적으로 예측해, 실제로는
 * 범위를 벗어난 인덱스로도 메모리를 미리 읽어 캐시에 남길 수 있다.
 * 그 캐시 흔적을 측정하면 커널 메모리 내용이 새어 나간다.
 * array_index_nospec 은 분기 예측에 기대지 않는 산술 마스크로 인덱스를
 * 범위 안에 강제로 가둬, 투기 실행 중에도 범위를 벗어나지 않게 만든다.
 * 헤더 <linux/nospec.h> 를 포함한 유일한 이유가 이 한 줄이다.
 * 
 * 경계 조건에 주의: 검사는 '>' 이고 인덱스는 'port - 1' 이다. port 가
 * 정확히 47 이면 통과해 인덱스 46 이 되어 배열 마지막 원소를 읽는다.
 * port 가 0 이면 위 case 0 이 먼저 잡으므로 여기 오지 않는다.
 * 
 * 실행 컨텍스트: 프로세스 문맥, mrpc_mutex 를 쥔 상태.
 * 
 * 호출 체인:
 *   switchtec_dev_ioctl() -> [ioctl_port_to_pff] -> ioread32() */
static int ioctl_port_to_pff(struct switchtec_dev *stdev,
			     struct switchtec_ioctl_pff_port __user *up)
{
	/* [한국어] 커널 쪽 사본. */
	struct switchtec_ioctl_pff_port p;
	/* [한국어] 대상 파티션의 설정 블록. */
	struct part_cfg_regs __iomem *pcfg;

	/* [한국어] 유저 요청을 읽어 온다. */
	if (copy_from_user(&p, up, sizeof(p)))
		/* [한국어] 잘못된 유저 주소. */
		return -EFAULT;

	/* [한국어] -1 은 '내 파티션' 을 뜻하는 특수 값이다. */
	if (p.partition == SWITCHTEC_IOCTL_EVENT_LOCAL_PART_IDX)
		/* [한국어] 미리 좁혀 둔 내 파티션 블록을 쓴다. */
		pcfg = stdev->mmio_part_cfg;
	/* [한국어] 그 밖에는 실제 파티션 개수 안이어야 한다. __u32 필드라 음수는
	 * 없으므로 상한만 본다. */
	else if (p.partition < stdev->partition_count)
		/* [한국어] 해당 파티션의 블록을 고른다. */
		pcfg = &stdev->mmio_part_cfg_all[p.partition];
	/* [한국어] 범위를 벗어난 파티션 번호. */
	else
		/* [한국어] 거절한다. */
		return -EINVAL;

	/* [한국어] 포트 번호로 분기. */
	switch (p.port) {
	/* [한국어] 0 은 업스트림 포트. */
	case 0:
		/* [한국어] 업스트림의 PFF 인스턴스 ID 를 읽는다. */
		p.pff = ioread32(&pcfg->usp_pff_inst_id);
		break;
	/* [한국어] 100 은 가상 엔드포인트. */
	case SWITCHTEC_IOCTL_PFF_VEP:
		/* [한국어] 하위 8 비트만 ID 이므로 잘라 낸다. */
		p.pff = ioread32(&pcfg->vep_pff_inst_id) & 0xFF;
		break;
	/* [한국어] 나머지는 다운스트림 포트 번호로 본다. */
	default:
		/* [한국어] 배열 크기(47)를 넘으면 잘못된 포트다. ARRAY_SIZE 는 size_t 라
		 * 비교가 부호 없이 이뤄지고, p.port 도 __u32 라 음수 문제는 없다. */
		if (p.port > ARRAY_SIZE(pcfg->dsp_pff_inst_id))
			/* [한국어] 거절. */
			return -EINVAL;
		/* [한국어] ★ 위 설명의 Spectre v1 방어. 상한을 47 + 1 로 주는 이유는
		 * 이 함수가 인덱스를 'port - 1' 로 쓰기 때문이다 - 허용되는 port 값의
		 * 상한이 47 이므로 배타적 상한은 48 이 된다. */
		p.port = array_index_nospec(p.port,
					ARRAY_SIZE(pcfg->dsp_pff_inst_id) + 1);
		/* [한국어] 가둬진 포트 번호로 배열을 읽는다. 배열 인덱스 0 이 포트 1 이므로
		 * 1 을 뺀다. */
		p.pff = ioread32(&pcfg->dsp_pff_inst_id[p.port - 1]);
		break;
	}

	/* [한국어] 결과를 유저에게 돌려준다. */
	if (copy_to_user(up, &p, sizeof(p)))
		/* [한국어] 복사 실패. */
		return -EFAULT;

	/* [한국어] 성공. */
	return 0;
}

/* [한국어] switchtec_dev_ioctl - /dev/switchtecN 의 ioctl 진입점
 * 
 * @filp: 열린 파일. private_data 에 open 때 만든 stuser 가 들어 있다.
 * @cmd: _IOR/_IOWR 로 인코딩된 명령 번호(switchtec_ioctl.h 의 'W' 매직).
 * @arg: 유저 포인터를 정수로 실어 온 값.
 * @return: 0 성공, 음수 errno. 모르는 명령은 -ENOTTY 다 - ioctl 규약상
 *          '이 파일은 그 명령을 모른다' 를 뜻하며, 유저스페이스는 이를
 *          기능 탐지에 쓴다.
 * 
 * ★ 락 구조: 이 함수가 앞머리에서 mrpc_mutex 를 잡고 끝에서 푼다.
 * 개별 ioctl_* 헬퍼는 락을 잡지 않으며, 잡혀 있다고 전제한다. 이렇게 한
 * 이유는 두 가지다 - (1) 하드웨어가 살아 있는지(alive) 확인하는 것과 실제
 * MMIO 접근 사이에 장치가 사라지면 안 되므로 그 구간을 하나의 락으로 덮고,
 * (2) 이벤트 레지스터를 읽고 고쳐 쓰는 read-modify-write 를 다른 사용자와
 * 겹치지 않게 한다.
 * 
 * copy_from_user/copy_to_user 를 락 안에서 부르는 점에 유의 - 페이지
 * 폴트로 잠들 수 있으나 mutex 는 잠들 수 있는 락이라 허용된다. 다만 그
 * 동안 다른 사용자의 MRPC 가 막힌다.
 * 
 * 실행 컨텍스트: 프로세스 문맥(유저스페이스 시스템 호출). 여러 fd 가
 * 동시에 들어올 수 있으므로 위 mutex 로 직렬화된다.
 * 
 * 호출 체인:
 *   (유저스페이스) ioctl() -> vfs -> [switchtec_dev_ioctl]
 *     -> ioctl_flash_info() / ioctl_flash_part_info() / ioctl_event_summary()
 *     -> ioctl_event_ctl() / ioctl_pff_to_port() / ioctl_port_to_pff() */
static long switchtec_dev_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	/* [한국어] 이 fd 전용 사용자 문맥. 이벤트 카운터 스냅숏을 여기에 갱신하는
	 * ioctl(EVENT_SUMMARY)이 있어 stdev 뿐 아니라 stuser 도 필요하다. */
	struct switchtec_user *stuser = filp->private_data;
	/* [한국어] 이 fd 가 가리키는 장치. */
	struct switchtec_dev *stdev = stuser->stdev;
	/* [한국어] 각 핸들러의 반환값. */
	int rc;
	/* [한국어] 정수로 실려 온 유저 포인터를 __user 표시가 붙은 포인터로 되돌린다.
	 * __user 는 sparse 정적 검사기가 '이 주소는 직접 역참조하면 안 된다' 고
	 * 잡아 주기 위한 표시다. */
	void __user *argp = (void __user *)arg;

	/* [한국어] 락을 잡고 동시에 장치가 아직 살아 있는지 확인한다. 성공하면
	 * 락을 쥔 채 돌아온다. */
	rc = lock_mutex_and_test_alive(stdev);
	/* [한국어] 장치가 이미 제거되었거나(-ENODEV) 시그널을 받았다(-EINTR). */
	if (rc)
		return rc;

	/* [한국어] 명령 번호로 분기. 각 case 는 유저 버퍼 복사까지 헬퍼에 맡긴다. */
	switch (cmd) {
	/* [한국어] 플래시 전체 크기와 파티션 개수를 알려 준다. */
	case SWITCHTEC_IOCTL_FLASH_INFO:
		rc = ioctl_flash_info(stdev, argp);
		break;
	/* [한국어] 특정 플래시 파티션의 주소/길이/활성 여부를 알려 준다. */
	case SWITCHTEC_IOCTL_FLASH_PART_INFO:
		rc = ioctl_flash_part_info(stdev, argp);
		break;
	/* [한국어] 구형 이벤트 요약. 아래 EVENT_SUMMARY 와 _IOR 번호가 0x42 로 같지만
	 * 구조체 크기가 다르다 - pff 배열이 48 개냐 255 개냐의 차이다. */
	case SWITCHTEC_IOCTL_EVENT_SUMMARY_LEGACY:
		/* [한국어] 그래서 같은 헬퍼에 '몇 바이트만 복사할지' 를 인자로 넘겨
		 * 구형 유저스페이스와의 호환을 지킨다. */
		rc = ioctl_event_summary(stdev, stuser, argp,
					 sizeof(struct switchtec_ioctl_event_summary_legacy));
		break;
	/* [한국어] 이벤트 하나의 상태를 읽고 활성/로그/CLI/치명 플래그를 고친다. */
	case SWITCHTEC_IOCTL_EVENT_CTL:
		rc = ioctl_event_ctl(stdev, argp);
		break;
	/* [한국어] PFF 인스턴스 번호로 (파티션, 포트) 를 역조회한다. */
	case SWITCHTEC_IOCTL_PFF_TO_PORT:
		rc = ioctl_pff_to_port(stdev, argp);
		break;
	/* [한국어] 반대로 (파티션, 포트) 로 PFF 인스턴스 번호를 구한다. */
	case SWITCHTEC_IOCTL_PORT_TO_PFF:
		rc = ioctl_port_to_pff(stdev, argp);
		break;
	/* [한국어] 현재 이벤트 요약. 위 legacy 와 같은 함수를 부르되 */
	case SWITCHTEC_IOCTL_EVENT_SUMMARY:
		/* [한국어] 255 개 PFF 를 담는 큰 구조체 크기를 넘긴다. */
		rc = ioctl_event_summary(stdev, stuser, argp,
					 sizeof(struct switchtec_ioctl_event_summary));
		break;
	/* [한국어] 정의되지 않은 명령. */
	default:
		/* [한국어] -ENOTTY 로 '이 장치는 그런 ioctl 을 모른다' 고 알린다. */
		rc = -ENOTTY;
		break;
	}

	/* [한국어] 어느 경로로 왔든 반드시 락을 푼다. */
	mutex_unlock(&stdev->mrpc_mutex);
	/* [한국어] 핸들러의 결과를 그대로 유저스페이스에 전한다. */
	return rc;
}

/* [한국어] switchtec_fops - /dev/switchtecN 의 파일 연산 표.
 * stdev_create() 의 cdev_init 이 이 표를 cdev 에 붙인다. 아래 각 필드가
 * 유저스페이스의 시스템 호출 하나에 대응한다. */
static const struct file_operations switchtec_fops = {
	/* [한국어] 이 모듈이 소유자. 파일이 열려 있는 동안 모듈 언로드를 막는다. */
	.owner = THIS_MODULE,
	/* [한국어] open(2) - fd 마다 stuser 를 하나 만들어 private_data 에 매단다. */
	.open = switchtec_dev_open,
	/* [한국어] close(2)의 마지막 참조 - stuser 참조를 놓는다. */
	.release = switchtec_dev_release,
	/* [한국어] write(2) - [4바이트 명령 코드][페이로드] 형식으로 MRPC 명령을 큐에 넣는다. */
	.write = switchtec_dev_write,
	/* [한국어] read(2) - [4바이트 반환 코드][응답] 형식으로 결과를 받아 간다. */
	.read = switchtec_dev_read,
	/* [한국어] poll(2)/select(2) - 명령 완료와 이벤트 발생을 함께 감시한다. */
	.poll = switchtec_dev_poll,
	/* [한국어] ioctl(2). 'unlocked' 는 옛 BKL(Big Kernel Lock) 없이 불린다는
	 * 역사적 이름이며, 락은 드라이버가 스스로 잡는다. */
	.unlocked_ioctl = switchtec_dev_ioctl,
	/* [한국어] 32 비트 유저스페이스가 64 비트 커널에 부르는 경우. 이 드라이버의
	 * ioctl 구조체는 모두 __u32/__u64 로 정의되어 두 ABI 에서 배치가 같으므로,
	 * 포인터만 compat_ptr 로 확장해 주면 되는 표준 헬퍼로 충분하다. */
	.compat_ioctl = compat_ptr_ioctl,
};

/* [한국어] link_event_work - 링크 상태 변화를 상위 드라이버에 알리는 작업 함수
 * 
 * @work: stdev->link_event_work. container_of 로 stdev 를 되찾는다.
 * @return: 없음
 * 
 * 왜 워크큐인가: 통지 콜백(link_notifier)이 무엇을 할지 이 드라이버는
 * 모른다 - 잠들 수도 있고 길어질 수도 있다. 그래서 ISR 에서 직접 부르지
 * 않고 프로세스 문맥으로 옮긴다.
 * 
 * ★ 확인된 사실: link_notifier 를 '설정' 하는 코드는 이 부분 체크아웃
 * 안에 없다. 읽는 곳은 여기 한 곳뿐이다. 필드 이름과
 * switchtec_pci_probe() 가 ntb_hw_switchtec 모듈을 요청하는 것으로 보아
 * NTB 드라이버가 채우도록 마련된 자리로 보이지만, 그 드라이버가 이
 * 트리에 없어 직접 확인하지는 못했다. 그래서 실제로는 NULL 검사에
 * 걸려 아무 일도 하지 않는 경우가 대부분이다.
 * 
 * 실행 컨텍스트: 시스템 워크큐의 커널 스레드(프로세스 문맥).
 * 같은 work 는 동시에 두 번 실행되지 않음이 워크큐가 보장하는 성질이다.
 * 
 * 호출 체인:
 *   switchtec_event_isr() -> check_link_state_events() -> schedule_work()
 *     -> [link_event_work] -> stdev->link_notifier() */
static void link_event_work(struct work_struct *work)
{
	/* [한국어] 이 작업을 품고 있는 장치. */
	struct switchtec_dev *stdev;

	/* [한국어] 구조체 멤버 주소에서 바깥 구조체 주소를 역산한다 - 워크큐가
	 * 넘겨주는 것은 멤버 포인터뿐이기 때문이다. */
	stdev = container_of(work, struct switchtec_dev, link_event_work);

	/* [한국어] 등록된 통지자가 있을 때만. 위에 적었듯 이 트리에서는 설정자를
	 * 찾지 못했다. */
	if (stdev->link_notifier)
		/* [한국어] 상위 드라이버에게 '링크 상태가 변했다' 고 알린다. */
		stdev->link_notifier(stdev);
}

/* [한국어] check_link_state_events - 모든 PFF 의 링크 상태 변화를 조사한다
 * 
 * @stdev: 대상 장치.
 * @return: 없음. 변화가 있으면 통지 작업을 예약하는 것이 부수 효과다.
 * 
 * ★ 다른 이벤트와 규칙이 다른 이유: 링크 상태는 '떴다/안 떴다' 가 아니라
 * '몇 번 변했나' 를 헤더의 5~12 비트에 8 비트 카운터로 담는다. 링크가
 * 빠르게 붙었다 떨어지면 발생 비트만 봐서는 몇 번 변했는지 알 수 없으나,
 * 카운터를 이전 값과 비교하면 놓친 변화까지 알아챌 수 있다. 그래서 이
 * 이벤트만은 마스킹하지 않고(=인터럽트를 계속 받고) 카운터를 추적한다.
 * 
 * 실행 컨텍스트: 인터럽트 문맥(ISR 안). link_event_count[] 를 갱신하는데,
 * 이 배열을 만지는 곳이 여기 한 곳뿐이고 같은 벡터가 재진입하지 않으므로
 * 락 없이도 안전하다.
 * 
 * 호출 체인:
 *   switchtec_event_isr() -> [check_link_state_events] -> schedule_work() */
static void check_link_state_events(struct switchtec_dev *stdev)
{
	/* [한국어] PFF 인덱스. */
	int idx;
	/* [한국어] 읽어 온 헤더 값. */
	u32 reg;
	/* [한국어] 이번에 읽은 변화 횟수. */
	int count;
	/* [한국어] 하나라도 변했는지 기록하는 플래그. 여러 포트가 동시에 변해도
	 * 통지 작업은 한 번만 띄우면 되기 때문에 개수가 아니라 불린이다. */
	int occurred = 0;

	/* [한국어] 유효한 PFF 전부를 훑는다. 여기서는 pff_local 을 보지 않는데,
	 * 읽기만 할 뿐 레지스터를 고치지 않으므로 남의 파티션에 영향이 없다. */
	for (idx = 0; idx < stdev->pff_csr_count; idx++) {
		/* [한국어] 각 포트의 링크 상태 이벤트 헤더를 읽는다. */
		reg = ioread32(&stdev->mmio_pff_csr[idx].link_state_hdr);
		/* [한국어] 디버그 추적 - 어느 포트가 어떤 값인지. */
		dev_dbg(&stdev->dev, "link_state: %d->%08x\n", idx, reg);
		/* [한국어] 헤더에서 변화 횟수를 꺼낸다. 5 비트 오른쪽으로 밀고 하위 8 비트만
		 * 취한다 - 즉 비트 5~12 가 카운터 자리다. 8 비트라 255 를 넘으면
		 * 0 으로 되감기지만, '이전 값과 다른가' 만 보므로 문제가 되지 않는다. */
		count = (reg >> 5) & 0xFF;

		/* [한국어] 지난번에 본 값과 다르면 그 사이에 링크가 변했다는 뜻이다. */
		if (count != stdev->link_event_count[idx]) {
			/* [한국어] 통지가 필요함을 표시. */
			occurred = 1;
			/* [한국어] 새 값을 기억해 둔다 - 다음 인터럽트의 비교 기준이 된다. */
			stdev->link_event_count[idx] = count;
		}
	}

	/* [한국어] 어느 포트든 하나라도 변했으면. */
	if (occurred)
		/* [한국어] 통지 작업을 예약한다. 이미 예약되어 실행 대기 중이면 워크큐가
		 * 중복 예약을 무시하므로, 여러 포트가 함께 변해도 통지는 한 번이다. */
		schedule_work(&stdev->link_event_work);
}

/* [한국어] enable_link_state_events - 모든 PFF 의 링크 상태 인터럽트를 켠다
 * 
 * @stdev: 대상 장치. init_pff() 가 끝나 pff_csr_count 가 유효해야 한다.
 * @return: 없음
 * 
 * probe 의 마지막 단계에서 한 번 불린다. 다른 이벤트들은 유저스페이스가
 * ioctl(EVENT_CTL)로 직접 켜야 하지만, 링크 상태만은 드라이버가 스스로
 * 켠다 - NTB 통지 경로가 유저스페이스 개입 없이 동작해야 하기 때문이다.
 * 
 * 실행 컨텍스트: probe 의 프로세스 문맥. 아직 ISR 가 이 레지스터들을
 * 읽기 전이므로 경쟁이 없다.
 * 
 * 호출 체인:
 *   switchtec_pci_probe() -> [enable_link_state_events] */
static void enable_link_state_events(struct switchtec_dev *stdev)
{
	/* [한국어] PFF 인덱스. */
	int idx;

	/* [한국어] 유효한 PFF 전부에 대해. */
	for (idx = 0; idx < stdev->pff_csr_count; idx++) {
		/* [한국어] (발생 비트 지움 | IRQ 활성) 을 쓴다. CLEAR 와 OCCURRED 가 같은
		 * BIT(0) 인데, 쓸 때는 '지움' 으로 해석되는 RW1C 비트라 이 한 번의
		 * 쓰기로 밀린 이벤트를 지우고 앞으로의 인터럽트를 켜는 두 일이 함께
		 * 이루어진다. */
		iowrite32(SWITCHTEC_EVENT_CLEAR |
			  SWITCHTEC_EVENT_EN_IRQ,
			  &stdev->mmio_pff_csr[idx].link_state_hdr);
	}
}

/* [한국어] enable_dma_mrpc - 장치에 응답 버퍼 주소를 알려 주고 DMA MRPC 를 켠다
 * 
 * @stdev: dma_mrpc 버퍼가 이미 확보된 장치.
 * @return: 없음
 * 
 * DMA MRPC 를 켜면 펌웨어는 MRPC 응답을 MMIO 출력 영역이 아니라
 * 여기서 알려 준 호스트 메모리에 직접 써 넣는다. 최대 1KB 를
 * memcpy_fromio 로 읽어 오던 것을 평범한 memcpy 로 바꾸는 효과라
 * 완료 처리가 눈에 띄게 가벼워진다.
 * 
 * ★ 순서가 중요하다 - 주소를 먼저 알려 주고, WC 버퍼를 비운 뒤,
 * 마지막에 기능 비트를 켠다. 이 두 레지스터는 모두 WC 로 매핑된
 * 0x0~0x1000 구간에 있어서, 쓰기가 프로세서 안에 머물다 순서가 뒤집힐
 * 수 있다. 기능 비트가 주소보다 먼저 도달하면 장치가 아직 0 인
 * 주소로 DMA 를 쏘게 된다.
 * 
 * 실행 컨텍스트: probe 의 프로세스 문맥.
 * 
 * 호출 체인:
 *   switchtec_pci_probe() -> [enable_dma_mrpc] -> flush_wc_buf() */
static void enable_dma_mrpc(struct switchtec_dev *stdev)
{
	/* [한국어] 64 비트 버스 주소를 한 번에 쓴다. writeq 는 <linux/io-64-nonatomic-lo-hi.h>
	 * 덕분에 64 비트 MMIO 쓰기가 없는 아키텍처에서도 하위 32 비트를 먼저,
	 * 상위를 나중에 쓰는 방식으로 자동 대체된다 - 하위를 먼저 쓰는 순서가
	 * 중요한 하드웨어를 위해 lo-hi 판을 고른 것이다. */
	writeq(stdev->dma_mrpc_dma_addr, &stdev->mmio_mrpc->dma_addr);
	/* [한국어] WC 버퍼를 비워, 위 주소 쓰기가 아래 기능 비트 쓰기보다 먼저
	 * 하드웨어에 도달하도록 강제한다. */
	flush_wc_buf(stdev);
	/* [한국어] DMA MRPC 기능 비트(BIT(0))를 켠다. 이 뒤로 응답은 호스트 메모리로 온다. */
	iowrite32(SWITCHTEC_DMA_MRPC_EN, &stdev->mmio_mrpc->dma_en);
}

/* [한국어] stdev_release - device 의 마지막 참조가 사라졌을 때 불리는 해제 함수
 * 
 * @dev: 해제될 device. stdev->dev 이다.
 * @return: 없음
 * 
 * 왜 이 함수가 따로 필요한가: 커널 device 모델은 '언제' 객체를 해제할지를
 * 드라이버가 정하지 못하게 한다. remove 가 끝나도 열린 fd 나 sysfs 접근이
 * 남아 있으면 참조가 살아 있고, 그것들이 모두 사라진 뒤에야 이 콜백이
 * 불린다. 그래서 kfree 를 여기에 두어야만 use-after-free 가 없다.
 * 
 * 실행 컨텍스트: 마지막 put_device 를 부른 쪽의 문맥. 프로세스 문맥일
 * 수도, 다른 곳일 수도 있으므로 잠들 수 있는 일을 해서는 안 된다.
 * 
 * 호출 체인:
 *   put_device() (switchtec_pci_remove / switchtec_dev_release 경로)
 *     -> (device 코어) -> [stdev_release] -> kfree() */
static void stdev_release(struct device *dev)
{
	/* [한국어] embedded device 포인터에서 바깥 switchtec_dev 를 역산한다. */
	struct switchtec_dev *stdev = to_stdev(dev);

	/* [한국어] 구조체 자체를 반납한다. MMIO 매핑과 IRQ 는 devm 이, DMA 버퍼는
	 * switchtec_exit_pci 가 이미 정리한 뒤다. */
	kfree(stdev);
}

/* [한국어] stdev_kill - 하드웨어를 '죽은 것' 으로 표시하고 대기자를 모두 내보낸다
 * 
 * @stdev: 정리 대상 장치.
 * @return: 없음
 * 
 * ★ 이 함수가 푸는 문제: 장치가 물리적으로 빠져도 유저스페이스는 여전히
 * fd 를 쥐고 있다. 누군가는 MRPC 응답을 기다리며 wait_event_interruptible
 * 에 잠들어 있을 수 있는데, 그 응답은 영원히 오지 않는다. 그대로 두면
 * 프로세스가 영구히 매달린다.
 * 
 * 해결: (1) 버스 마스터를 꺼 장치가 더 이상 DMA 를 쏘지 못하게 하고,
 * (2) 타임아웃 작업을 취소해 없어진 하드웨어를 폴링하지 않게 하고,
 * (3) alive 를 내려 이후의 모든 진입점이 -ENODEV 를 돌려주게 하고,
 * (4) 대기 중인 사용자를 전부 깨워 큐에서 빼낸다. 깨어난 쪽은
 * stuser->state 가 MRPC_DONE 이 아니므로 -EBADE 를 받는다.
 * 
 * 실행 컨텍스트: probe 실패 경로 또는 remove 의 프로세스 문맥.
 * mutex 를 잡으므로 잠들 수 있어야 한다.
 * 
 * 호출 체인:
 *   switchtec_pci_probe()(err_devadd) / switchtec_pci_remove() -> [stdev_kill] */
static void stdev_kill(struct switchtec_dev *stdev)
{
	/* [한국어] 리스트를 순회하며 원소를 지울 것이므로, 다음 원소를 미리 담아 둘
	 * tmpuser 가 함께 필요하다(_safe 순회의 요구). */
	struct switchtec_user *stuser, *tmpuser;

	/* [한국어] 버스 마스터 비트를 끈다. 이 순간부터 장치는 호스트 메모리에
	 * 쓸 수 없다 - 곧 해제될 DMA MRPC 버퍼에 늦은 쓰기가 도착하는 사고를
	 * 막는 첫 번째 방벽이다. */
	pci_clear_master(stdev->pdev);

	/* [한국어] MRPC 타임아웃 지연 작업을 취소하고, 이미 실행 중이면 끝날 때까지
	 * 기다린다(_sync). 이 작업은 mrpc_mutex 를 잡으므로, 아래에서 같은
	 * 락을 잡기 전에 반드시 끝내 두어야 순환 대기가 생기지 않는다. */
	cancel_delayed_work_sync(&stdev->mrpc_timeout);

	/* Mark the hardware as unavailable and complete all completions */
	/* [한국어] 블록을 벗어날 때 자동으로 풀리는 mutex 획득(scoped_guard).
	 * 아래에서 alive 와 대기 큐를 함께 바꾸므로, 그 사이에 새 명령이
	 * 큐에 들어오지 못하게 락으로 덮는다. */
	scoped_guard (mutex, &stdev->mrpc_mutex) {
		/* [한국어] 이후 lock_mutex_and_test_alive() 를 거치는 모든 진입점 -
		 * read/write/poll/ioctl - 이 이 값을 보고 -ENODEV 를 돌려준다. */
		stdev->alive = false;

		/* Wake up and kill any users waiting on an MRPC request */
		/* [한국어] 대기 큐를 순회한다. 순회 도중 원소를 리스트에서 빼내므로
		 * _safe 판을 써야 한다. */
		list_for_each_entry_safe(stuser, tmpuser, &stdev->mrpc_queue, list) {
			/* [한국어] '완료됨' 으로 표시. 깨어난 쪽이 wait_event 의 조건을 통과해
			 * 다시 잠들지 않게 하는 핵심이다 - 이 표시가 없으면 깨워도 조건이
			 * 거짓이라 도로 잠든다. */
			stuser->cmd_done = true;
			/* [한국어] 기다리던 사용자를 깨운다. 시그널로도 깨울 수 있는 대기라
			 * _interruptible 판이다. */
			wake_up_interruptible(&stuser->cmd_comp);
			/* [한국어] 대기 큐에서 빼고 리스트 헤드를 자기 자신으로 다시 초기화한다 -
			 * 나중에 누가 이 노드를 또 지우려 해도 안전하도록. */
			list_del_init(&stuser->list);
			/* [한국어] 큐에 넣을 때 mrpc_queue_cmd 가 올렸던 참조를 놓는다. 사용자가
			 * fd 를 이미 닫았다면 여기서 stuser 가 해제된다. */
			stuser_put(stuser);
		}

	}

	/* Wake up any users waiting on event_wq */
	/* [한국어] 이벤트를 기다리던 poll 사용자도 깨운다. 이들은 mrpc_queue 에
	 * 들어 있지 않으므로 위 순회로는 깨워지지 않는다. 깨어난 poll 은
	 * lock_mutex_and_test_alive 가 실패해 EPOLLERR|EPOLLHUP 를 받는다.
	 * 락 밖에서 부르는 이유는 poll 이 깨어나자마자 같은 락을 잡으려 하기
	 * 때문에 굳이 락을 쥔 채로 깨울 이유가 없어서다. */
	wake_up_interruptible(&stdev->event_wq);
}

/* [한국어] stdev_create - 장치 상태 객체를 만들고 문자 디바이스 껍데기까지 준비한다
 * 
 * @pdev: 이 관리 장치가 얹힐 PCI 장치.
 * @return: 준비된 switchtec_dev 포인터. 실패 시 ERR_PTR(-ENOMEM 또는 ida 오류).
 * 
 * 왜 필요한가: probe 가 하드웨어를 만지기 전에, 그 하드웨어의 상태를 담을
 * 그릇과 유저스페이스 노출 통로(cdev)를 먼저 만들어 둔다. 여기서는
 * '등록' 까지 가지 않는다 - cdev_init 만 하고 cdev_device_add 는 probe 의
 * 맨 마지막에 한다. 하드웨어 초기화가 끝나기 전에 /dev 노드가 열리면
 * 아직 세워지지 않은 MMIO 포인터를 참조하기 때문이다.
 * 
 * 수명 관리(★): 반환된 객체는 참조 카운트로 산다.
 *   - stdev->dev 자체의 참조: device_initialize 가 1 로 시작
 *   - 마지막 참조가 사라지면 dev->release(=stdev_release)가 kfree 한다
 *   - stuser 하나가 만들어질 때마다 get_device 로 참조를 올린다
 *   그래서 장치가 물리적으로 빠져도 열려 있는 fd 가 남아 있는 한
 *   stdev 메모리는 유효하다 - 대신 alive 플래그로 하드웨어 접근만 막는다.
 * 
 * 실행 컨텍스트: probe 의 프로세스 문맥. GFP_KERNEL 을 쓰므로 잠들 수 있다.
 * 
 * 에러 경로: minor 할당 실패만 되돌릴 것이 있다. 그때 pci_dev_get 으로
 * 올린 pdev 참조를 놓고, put_device 로 stdev 를 해제시킨다.
 * 
 * 호출 체인:
 *   switchtec_pci_probe() -> [stdev_create] -> kzalloc_node()/device_initialize()
 *     -> ida_alloc() -> cdev_init() */
static struct switchtec_dev *stdev_create(struct pci_dev *pdev)
{
	/* [한국어] 만들 장치 객체. */
	struct switchtec_dev *stdev;
	/* [한국어] ida 로 뽑을 부 번호. 음수면 실패이므로 int 다. */
	int minor;
	/* [한국어] stdev->dev 를 짧게 가리키는 별칭 - 아래 설정이 길어지는 것을 줄인다. */
	struct device *dev;
	/* [한국어] stdev->cdev 별칭. */
	struct cdev *cdev;
	/* [한국어] 실패 사유 보관. */
	int rc;

	/* [한국어] 장치가 붙어 있는 NUMA 노드에서 메모리를 잡는다. 이 구조체는
	 * 인터럽트 처리와 MMIO 접근 때마다 만져지므로, 그 장치를 다루는 CPU 와
	 * 가까운 노드에 두는 편이 캐시/메모리 지연에 유리하다. */
	stdev = kzalloc_node(sizeof(*stdev), GFP_KERNEL,
			     dev_to_node(&pdev->dev));
	/* [한국어] 메모리 부족이면 포인터 대신 오류 코드를 실어 돌려준다 - 호출자는
	 * IS_ERR 로 판정한다. */
	if (!stdev)
		return ERR_PTR(-ENOMEM);

	/* [한국어] 아직 하드웨어를 만지기 전이지만 '살아 있음' 으로 시작한다.
	 * 이 값이 false 로 내려가는 것은 stdev_kill() 한 곳뿐이다. */
	stdev->alive = true;
	/* [한국어] PCI 장치 참조를 하나 올려 쥔다. 이렇게 해야 remove 뒤에도 남아 있는
	 * fd 가 stdev->pdev 를 통해 죽은 메모리를 보지 않는다. 짝은
	 * switchtec_pci_remove()/에러 경로의 pci_dev_put 이다. */
	stdev->pdev = pci_dev_get(pdev);
	/* [한국어] MRPC 대기 큐 초기화. 한 번에 한 명령만 하드웨어로 보내므로,
	 * 나머지는 이 리스트에서 순서를 기다린다. */
	INIT_LIST_HEAD(&stdev->mrpc_queue);
	/* [한국어] ★ 이 드라이버의 단일 직렬화 지점. 헤더의 주석대로 mrpc_ 필드들,
	 * alive 플래그, stuser->state 를 모두 이 mutex 가 지킨다.
	 * ISR 는 이 락을 잡을 수 없으므로(잠들 수 있는 락이다) 실제 완료 처리는
	 * 워크큐로 넘긴다 - mrpc_event_work 가 그 일을 한다. */
	mutex_init(&stdev->mrpc_mutex);
	/* [한국어] '지금 하드웨어에 명령이 떠 있는가'. 0 이어야 다음 명령을 낼 수 있다. */
	stdev->mrpc_busy = 0;
	/* [한국어] MRPC 완료 처리를 프로세스 문맥으로 옮길 작업 항목. ISR 가
	 * schedule_work 로 띄운다. */
	INIT_WORK(&stdev->mrpc_work, mrpc_event_work);
	/* [한국어] 명령이 500ms 안에 끝나지 않을 때를 대비한 지연 작업. 인터럽트를
	 * 놓치거나 펌웨어가 멎어도 사용자가 영원히 매달리지 않게 한다. */
	INIT_DELAYED_WORK(&stdev->mrpc_timeout, mrpc_timeout_work);
	/* [한국어] 링크 상태 변화를 상위(NTB) 드라이버에 알리는 작업 항목. */
	INIT_WORK(&stdev->link_event_work, link_event_work);
	/* [한국어] 이벤트 알림용 대기 큐. poll 이 여기에 걸린다. */
	init_waitqueue_head(&stdev->event_wq);
	/* [한국어] 이벤트 발생 횟수. poll 은 자기가 마지막에 본 값과 이 값을 비교해
	 * 새 이벤트가 있는지 판단하므로, 값 자체보다 '변했는가' 가 중요하다.
	 * ISR 에서 증가하고 여러 사용자 문맥에서 읽으므로 atomic_t 다. */
	atomic_set(&stdev->event_cnt, 0);

	/* [한국어] 아래 설정을 짧게 쓰기 위한 별칭. */
	dev = &stdev->dev;
	/* [한국어] 장치 객체를 초기화한다 - 참조 카운트를 1 로 세우고 kobject 를 준비.
	 * 이 시점부터 put_device 로 해제하는 것이 정석이 되므로, 이 줄 이후의
	 * 실패 경로는 kfree 가 아니라 put_device 를 써야 한다. */
	device_initialize(dev);
	/* [한국어] /sys/class/switchtec 에 속하게 한다 - udev 가 /dev/switchtecN 을
	 * 만들 근거가 된다. */
	dev->class = &switchtec_class;
	/* [한국어] sysfs 트리에서 PCI 장치 아래에 놓이게 한다. */
	dev->parent = &pdev->dev;
	/* [한국어] device_version, fw_version, vendor_id ... 같은 sysfs 속성 묶음.
	 * ATTRIBUTE_GROUPS 매크로가 만든 switchtec_device_groups 이며,
	 * 장치가 추가될 때 코어가 알아서 파일들을 만들어 준다. */
	dev->groups = switchtec_device_groups;
	/* [한국어] 마지막 참조가 사라졌을 때 불릴 해제 함수. 이것을 지정하지 않으면
	 * 커널이 경고를 낸다. */
	dev->release = stdev_release;

	/* [한국어] 부 번호를 하나 뽑는다. ida 는 '가장 작은 빈 번호' 를 준다 -
	 * 장치가 빠졌다 붙으면 같은 번호를 재사용한다. */
	minor = ida_alloc(&switchtec_minor_ida, GFP_KERNEL);
	/* [한국어] 번호 공간이 고갈되었거나 메모리 부족. */
	if (minor < 0) {
		/* [한국어] ida 가 돌려준 음수 오류를 그대로 실패 사유로 삼는다. */
		rc = minor;
		goto err_put;
	}

	/* [한국어] 모듈 초기화 때 예약한 major 와 방금 얻은 minor 를 합쳐 실제
	 * 디바이스 번호를 만든다. */
	dev->devt = MKDEV(MAJOR(switchtec_devt), minor);
	/* [한국어] 장치 이름 - 이것이 /sys/class/switchtec/switchtecN 이 되고,
	 * udev 는 같은 이름으로 /dev 노드를 만든다. */
	dev_set_name(dev, "switchtec%d", minor);

	/* [한국어] 문자 디바이스 구조체 별칭. */
	cdev = &stdev->cdev;
	/* [한국어] 파일 연산 표를 붙여 cdev 를 초기화한다. 등록(add)은 아직이다. */
	cdev_init(cdev, &switchtec_fops);
	/* [한국어] 이 모듈이 소유자임을 밝힌다 - 열린 fd 가 있는 동안 모듈이
	 * 언로드되지 않게 참조 카운트를 잡아 주는 근거다. */
	cdev->owner = THIS_MODULE;

	/* [한국어] 여기까지 오면 그릇이 완성됐다. */
	return stdev;

/* [한국어] ida_alloc 실패 지점. */
err_put:
	/* [한국어] 올려 뒀던 pdev 참조를 놓는다. */
	pci_dev_put(stdev->pdev);
	/* [한국어] device_initialize 가 세운 참조를 놓는다 - 이 호출이 stdev_release
	 * 를 거쳐 kfree 까지 이어진다. 그래서 여기서 kfree 를 직접 부르면 안 된다. */
	put_device(&stdev->dev);
	return ERR_PTR(rc);
}

/* [한국어] mask_event - 이벤트 하나가 떠 있으면 그 인터럽트를 끄고 발생 비트를 지운다
 * 
 * @stdev: 대상 장치.
 * @eid: 이벤트 종류. event_regs[] 의 인덱스이며 UAPI 의
 *       SWITCHTEC_IOCTL_EVENT_* 값과 같다.
 * @idx: 그 종류 안에서 몇 번째 인스턴스인가(파티션 번호 또는 PFF 번호).
 *       전역 이벤트는 무시된다.
 * @return: 실제로 마스킹한 이벤트가 있으면 1, 없으면 0. 호출자는 이 값을
 *          더해 '이번 인터럽트로 처리한 이벤트가 몇 개인가' 를 센다.
 * 
 * 왜 '마스킹' 인가(★ 이 드라이버의 이벤트 모델): 이 이벤트들은 레벨
 * 트리거처럼 동작한다 - 발생 비트가 서 있고 IRQ 활성 비트도 서 있으면
 * 인터럽트가 계속 올라온다. 드라이버는 이벤트의 내용을 커널에서
 * 해석하지 않고 유저스페이스(ioctl EVENT_CTL)에 맡기므로, ISR 는
 * '인터럽트를 멈추게만' 하고 물러난다. 그 방법이 IRQ 활성 비트와 발생
 * 비트를 함께 내리는 것이다. 유저스페이스가 나중에 EVENT_CTL 로 다시
 * 켜 줄 때까지 그 이벤트는 조용해진다 - 인터럽트 폭주를 막는 장치다.
 * 
 * 실행 컨텍스트: 인터럽트 문맥(switchtec_event_isr 안). 잠들 수 없고,
 * MMIO 읽기/쓰기만 한다. 락은 잡지 않는다 - 건드리는 것이 하드웨어
 * 레지스터뿐이고, 같은 레지스터를 만지는 유저 경로(event_ctl)는
 * mrpc_mutex 아래에서 돌지만 하드웨어의 원자적 RW1C 특성에 기댄다.
 * 
 * 호출 체인:
 *   switchtec_event_isr() -> mask_all_events() -> [mask_event] */
static int mask_event(struct switchtec_dev *stdev, int eid, int idx)
{
	/* [한국어] 이 이벤트 종류의 헤더 레지스터가 자기 블록 안에서 갖는 오프셋.
	 * 컴파일 시점에 offsetof 로 계산된 상수다. */
	size_t off = event_regs[eid].offset;
	/* [한국어] 실제 헤더 레지스터의 MMIO 주소. */
	u32 __iomem *hdr_reg;
	/* [한국어] 읽어 온 헤더 값. */
	u32 hdr;

	/* [한국어] 오프셋을 어느 블록에 더할지는 이벤트 종류마다 다르다 - 전역이면
	 * sw_event 블록, 파티션이면 part_cfg_all[idx], PFF 면 pff_csr[idx].
	 * 그 선택을 표에 담긴 함수 포인터가 대신 해 준다. */
	hdr_reg = event_regs[eid].map_reg(stdev, off, idx);
	/* [한국어] 헤더 레지스터를 읽는다. 이 한 워드에 지원 여부, 발생 여부,
	 * 발생 횟수, 각종 활성 비트가 모두 들어 있다. */
	hdr = ioread32(hdr_reg);

	/* [한국어] BIT(31) - 이 하드웨어/펌웨어가 지원하지 않는 이벤트. */
	if (hdr & SWITCHTEC_EVENT_NOT_SUPP)
		/* [한국어] 지원하지 않으면 마스킹할 것도 없다. */
		return 0;

	/* [한국어] 발생 비트(BIT(0))와 IRQ 활성 비트(BIT(3))가 '둘 다' 서 있어야
	 * 이 인터럽트의 원인일 수 있다. 참고: 여기 쓰인 && 는 비트 연산이 아니라
	 * 논리 연산이라 결과가 0/1 이고, 그것을 다시 ! 로 뒤집는다 - 즉
	 * '둘 중 하나라도 아니면' 이라는 뜻이다. */
	if (!(hdr & SWITCHTEC_EVENT_OCCURRED && hdr & SWITCHTEC_EVENT_EN_IRQ))
		/* [한국어] 이번 인터럽트의 원인이 아니므로 건드리지 않는다. */
		return 0;

	/* [한국어] 디버그 빌드에서만 남는 추적 - 어떤 이벤트를 마스킹했는지. */
	dev_dbg(&stdev->dev, "%s: %d %d %x\n", __func__, eid, idx, hdr);
	/* [한국어] IRQ 활성 비트와 발생 비트를 함께 내린다. 발생 비트는 RW1C 라
	 * '1 을 쓰면 지워진다' - 지금은 읽은 값에서 그 자리를 0 으로 만들었으니
	 * 그 비트는 유지되지 않고 그대로 지워진다. */
	hdr &= ~(SWITCHTEC_EVENT_EN_IRQ | SWITCHTEC_EVENT_OCCURRED);
	/* [한국어] 수정한 값을 되쓴다. 이 순간 이 이벤트의 인터럽트가 멈춘다. */
	iowrite32(hdr, hdr_reg);

	/* [한국어] 하나 처리했음을 알린다. */
	return 1;
}

/* [한국어] mask_all_events - 한 이벤트 종류의 모든 인스턴스를 마스킹한다
 * 
 * @stdev: 대상 장치.
 * @eid: 이벤트 종류.
 * @return: 마스킹한 인스턴스 개수 합.
 * 
 * 같은 종류의 이벤트라도 인스턴스가 여러 개일 수 있다 - 파티션마다,
 * 또는 PFF(포트)마다 하나씩. 이 함수는 그 종류가 어느 범주에 속하는지를
 * 표의 map_reg 함수 포인터로 판별해 알맞은 개수만큼 훑는다.
 * 
 * ★ pff_local 검사가 왜 여기 있는가: 스위치 하나를 여러 호스트가
 * 파티션으로 나눠 쓸 수 있다. 다른 파티션 소유의 PFF 이벤트까지 내가
 * 마스킹해 버리면 남의 호스트가 받아야 할 인터럽트를 없애는 셈이 된다.
 * 그래서 init_pff() 가 미리 표시해 둔 '내 파티션 소유' PFF 만 만진다.
 * 파티션 이벤트 쪽에는 이런 필터가 없는데, 파티션 이벤트 레지스터는
 * 설계상 내 파티션에서 접근 가능한 것들이기 때문으로 보이나 이 트리
 * 안에서 그 근거를 확인하지는 못했다.
 * 
 * 실행 컨텍스트: 인터럽트 문맥.
 * 
 * 호출 체인:
 *   switchtec_event_isr() -> [mask_all_events] -> mask_event() */
static int mask_all_events(struct switchtec_dev *stdev, int eid)
{
	/* [한국어] 인스턴스 인덱스. */
	int idx;
	/* [한국어] 마스킹한 개수 누계. */
	int count = 0;

	/* [한국어] 표의 매핑 함수가 part_ev_reg 이면 파티션마다 하나씩 있는 이벤트다.
	 * 함수 포인터 값 자체를 비교해 범주를 알아내는 방식이다. */
	if (event_regs[eid].map_reg == part_ev_reg) {
		/* [한국어] 파티션 개수만큼 훑는다. */
		for (idx = 0; idx < stdev->partition_count; idx++)
			/* [한국어] 각 파티션의 인스턴스를 마스킹. */
			count += mask_event(stdev, eid, idx);
	/* [한국어] pff_ev_reg 이면 PFF(포트)마다 하나씩 있는 이벤트다. */
	} else if (event_regs[eid].map_reg == pff_ev_reg) {
		/* [한국어] 유효한 PFF 개수만큼 훑는다. */
		for (idx = 0; idx < stdev->pff_csr_count; idx++) {
			/* [한국어] 내 파티션 소유가 아닌 PFF. */
			if (!stdev->pff_local[idx])
				/* [한국어] 건너뛴다 - 남의 파티션 인터럽트를 지우지 않기 위해서다. */
				continue;

			/* [한국어] 내 것만 마스킹. */
			count += mask_event(stdev, eid, idx);
		}
	/* [한국어] 나머지는 global_ev_reg - 스위치 전체에 하나뿐인 이벤트다. */
	} else {
		/* [한국어] 인덱스는 의미가 없으므로 0 을 넘긴다 - global_ev_reg 는 index 를
		 * 아예 무시한다. */
		count += mask_event(stdev, eid, 0);
	}

	/* [한국어] 이번 종류에서 처리한 개수. */
	return count;
}

/* [한국어] switchtec_event_isr - 이벤트 벡터의 인터럽트 서비스 루틴
 * 
 * @irq: 커널이 준 IRQ 번호(여기서는 쓰지 않는다).
 * @dev: devm_request_irq 에 넘겼던 stdev 포인터.
 * @return: IRQ_HANDLED(내가 처리함) 또는 IRQ_NONE(내 인터럽트가 아님).
 *          MSI/MSI-X 는 공유되지 않지만, 코어가 가짜 인터럽트를 감지하도록
 *          정확히 보고해 주는 것이 규칙이다.
 * 
 * ★ 이 ISR 가 하는 일은 셋뿐이다 - 잠들 수 없는 문맥이므로 무거운 일은
 * 모두 워크큐로 넘긴다.
 *   1) MRPC 완료 이벤트가 떴으면 완료 처리 작업을 예약하고 비트를 지운다
 *      (실제 응답 복사는 mrpc_event_work 가 mutex 를 잡고 한다)
 *   2) 링크 상태 변화를 조사해, 바뀌었으면 통지 작업을 예약한다
 *   3) 나머지 모든 이벤트는 내용을 보지 않고 마스킹만 한 뒤, 대기 중인
 *      poll 사용자를 깨운다 - 해석은 유저스페이스의 몫이다
 * 
 * 실행 컨텍스트: 하드 인터럽트 문맥. 잠들 수 없고 mutex 를 잡을 수 없다.
 * 같은 벡터가 CPU 한 곳에서만 재진입 없이 돈다.
 * 
 * 호출 체인:
 *   (하드웨어 MSI/MSI-X) -> [switchtec_event_isr]
 *     -> schedule_work(mrpc_work) -> mrpc_event_work()
 *     -> check_link_state_events() / mask_all_events() */
static irqreturn_t switchtec_event_isr(int irq, void *dev)
{
	/* [한국어] 등록 때 넘긴 장치 포인터를 되찾는다. */
	struct switchtec_dev *stdev = dev;
	/* [한국어] 읽어 온 레지스터 값. */
	u32 reg;
	/* [한국어] 기본값은 '내 것이 아님'. 아래에서 처리한 것이 있으면 올린다. */
	irqreturn_t ret = IRQ_NONE;
	/* [한국어] 이벤트 종류 인덱스와, 이번에 마스킹한 총 개수. */
	int eid, event_count = 0;

	/* [한국어] 내 파티션의 MRPC 완료 이벤트 헤더를 읽는다. */
	reg = ioread32(&stdev->mmio_part_cfg->mrpc_comp_hdr);
	/* [한국어] 발생 비트가 서 있으면 방금 낸 MRPC 명령이 끝났다는 뜻이다. */
	if (reg & SWITCHTEC_EVENT_OCCURRED) {
		/* [한국어] 디버그 추적. */
		dev_dbg(&stdev->dev, "%s: mrpc comp\n", __func__);
		/* [한국어] 내 인터럽트가 맞다. */
		ret = IRQ_HANDLED;
		/* [한국어] 완료 처리를 워크큐로 넘긴다. 여기서 직접 처리할 수 없는 이유는
		 * mrpc_mutex 가 잠들 수 있는 락이고, 응답 복사가 최대 1KB 의
		 * memcpy_fromio 라 인터럽트 문맥에 두기에 무겁기 때문이다. */
		schedule_work(&stdev->mrpc_work);
		/* [한국어] 읽은 값을 그대로 되쓴다. 발생 비트가 RW1C 이므로 '읽은 값에 서
		 * 있던 1' 을 되쓰는 것이 곧 그 비트를 지우는 동작이다. IRQ 활성 비트는
		 * 읽은 값 그대로라 유지되어, 다음 명령의 완료도 인터럽트로 온다. */
		iowrite32(reg, &stdev->mmio_part_cfg->mrpc_comp_hdr);
	}

	/* [한국어] 링크 상태는 마스킹하지 않고 따로 다룬다 - 발생 비트 대신 헤더 안의
	 * '발생 횟수' 를 이전 값과 비교하는 방식이라 아래 일반 루프와 규칙이
	 * 다르기 때문이다. */
	check_link_state_events(stdev);

	/* [한국어] 정의된 모든 이벤트 종류를 훑는다. 상한은 UAPI 가 정한 32 다. */
	for (eid = 0; eid < SWITCHTEC_IOCTL_MAX_EVENTS; eid++) {
		/* [한국어] 링크 상태는 바로 위에서 이미 다뤘고, */
		if (eid == SWITCHTEC_IOCTL_EVENT_LINK_STATE ||
		    /* [한국어] MRPC 완료는 이 함수 앞머리에서 다뤘다. */
		    eid == SWITCHTEC_IOCTL_EVENT_MRPC_COMP)
			/* [한국어] 그러므로 이 둘은 일반 마스킹 루프에서 제외한다. 특히 MRPC 완료를
			 * 마스킹해 버리면 IRQ 활성 비트가 내려가 다음 명령의 완료 인터럽트가
			 * 영영 오지 않는다. */
			continue;

		/* [한국어] 나머지 종류는 인스턴스마다 마스킹하고 개수를 누적한다. */
		event_count += mask_all_events(stdev, eid);
	}

	/* [한국어] 마스킹한 이벤트가 하나라도 있었다면 유저스페이스에 알릴 거리가 있다. */
	if (event_count) {
		/* [한국어] 이벤트 카운터를 원자적으로 올린다. poll 은 자기가 저장해 둔 값과
		 * 이 값이 다른지를 보고 EPOLLPRI 를 돌려준다 - 값의 크기는 의미가 없고
		 * '변했다' 는 사실만 쓰인다. 인터럽트 문맥에서 올리고 사용자 문맥에서
		 * 읽으므로 원자 연산이 필요하다. */
		atomic_inc(&stdev->event_cnt);
		/* [한국어] event_wq 에서 기다리던 poll 사용자를 모두 깨운다. */
		wake_up_interruptible(&stdev->event_wq);
		/* [한국어] 몇 개를 처리했는지 디버그 추적. */
		dev_dbg(&stdev->dev, "%s: %d events\n", __func__,
			event_count);
		/* [한국어] 이벤트를 처리했으므로 확실히 내 인터럽트다. */
		return IRQ_HANDLED;
	}

	/* [한국어] MRPC 완료만 있었으면 IRQ_HANDLED, 아무것도 없었으면 IRQ_NONE. */
	return ret;
}


/* [한국어] switchtec_dma_mrpc_isr - DMA MRPC 전용 벡터의 인터럽트 서비스 루틴
 * 
 * @irq: IRQ 번호(쓰지 않는다).
 * @dev: stdev 포인터.
 * @return: 항상 IRQ_HANDLED - 이 벡터는 오직 DMA MRPC 완료에만 쓰이므로
 *          올라왔다면 원인이 하나뿐이다.
 * 
 * DMA MRPC 를 켜면 펌웨어가 응답을 MMIO 출력 영역이 아니라 호스트
 * 메모리(stdev->dma_mrpc)에 직접 써 넣고, 전용 벡터로 알린다. 그 벡터
 * 번호는 펌웨어가 mmio_mrpc->dma_vector 에 적어 둔 값이며
 * switchtec_init_isr() 이 그것을 읽어 이 핸들러를 걸어 둔다.
 * 
 * 일반 이벤트 ISR 와 달리 여기서는 아무것도 조사하지 않는다 -
 * 곧바로 이벤트 비트를 지우고 완료 처리 작업만 띄운다.
 * 
 * 실행 컨텍스트: 하드 인터럽트 문맥.
 * 
 * 호출 체인:
 *   (하드웨어 MSI/MSI-X) -> [switchtec_dma_mrpc_isr]
 *     -> schedule_work(mrpc_work) -> mrpc_event_work() -> mrpc_complete_cmd() */
static irqreturn_t switchtec_dma_mrpc_isr(int irq, void *dev)
{
	/* [한국어] 등록 때 넘긴 장치 포인터. */
	struct switchtec_dev *stdev = dev;

	/* [한국어] MRPC 완료 이벤트 헤더에 (지움 | IRQ 활성)을 쓴다. 앞의 ISR 와 달리
	 * 읽지 않고 상수를 그대로 쓰는데, 이 벡터는 원인이 하나뿐이라 헤더를
	 * 읽어 확인할 필요가 없고, 다음 명령을 위해 IRQ 활성 비트를 다시 세워
	 * 두어야 하기 때문이다. */
	iowrite32(SWITCHTEC_EVENT_CLEAR |
		  SWITCHTEC_EVENT_EN_IRQ,
		  &stdev->mmio_part_cfg->mrpc_comp_hdr);
	/* [한국어] 완료 처리를 워크큐로 넘긴다 - 응답은 이미 호스트 메모리에 와 있고,
	 * mrpc_complete_cmd 가 dma_mrpc->status 를 보고 판단한다. */
	schedule_work(&stdev->mrpc_work);

	/* [한국어] 이 벡터의 유일한 원인이므로 무조건 처리했다고 보고한다. */
	return IRQ_HANDLED;
}

/* [한국어] switchtec_init_isr - MSI/MSI-X 벡터를 확보하고 두 개의 ISR 를 건다
 * 
 * @stdev: BAR0 매핑이 끝난 장치. 펌웨어가 알려 주는 벡터 번호를 읽어야
 *         하므로 mmio_part_cfg 와 mmio_mrpc 가 이미 유효해야 한다.
 * @return: 0 성공. 음수 errno 실패(-EFAULT 는 펌웨어가 알려 준 벡터
 *          번호가 확보한 범위를 벗어난 경우).
 * 
 * ★ 이 함수의 특징 - 벡터 번호를 드라이버가 고르지 않는다. 보통의
 * 드라이버는 '내가 0 번 벡터를 쓴다' 고 정하지만, Switchtec 은 펌웨어가
 * '이벤트는 이 벡터로, DMA MRPC 완료는 저 벡터로 보낸다' 고 레지스터에
 * 적어 두고 드라이버가 그것을 읽어 따라간다. 그래서 반드시 범위 검사를
 * 해야 한다 - 펌웨어가 이상한 값을 적었을 때 pci_irq_vector 에 넘기면
 * 안 되기 때문이다.
 * 
 * nirqs 를 크게 잡는 이유: 모듈 파라미터 설명대로 NTB 응용에서 더 많은
 * 벡터가 쓸모 있기 때문이다. 이 드라이버 자신은 두 개면 충분하지만,
 * 같은 장치를 쓰는 NTB 드라이버가 나머지를 나눠 쓴다.
 * 
 * 실행 컨텍스트: probe 의 프로세스 문맥. devm_ 이라 실패해도 개별 해제가
 * 필요 없다.
 * 
 * 호출 체인:
 *   switchtec_pci_probe() -> [switchtec_init_isr]
 *     -> pci_alloc_irq_vectors() -> pci_irq_vector() -> devm_request_irq() */
static int switchtec_init_isr(struct switchtec_dev *stdev)
{
	/* [한국어] 실제로 확보된 벡터 개수. */
	int nvecs;
	/* [한국어] 이벤트용 벡터 - 처음에는 펌웨어가 알려 준 '인덱스', 나중에는
	 * 커널의 IRQ 번호가 담긴다. */
	int event_irq;
	/* [한국어] DMA MRPC 완료용 벡터 - 같은 방식. */
	int dma_mrpc_irq;
	/* [한국어] 반환값. */
	int rc;

	/* [한국어] 모듈 파라미터가 4 보다 작게 설정된 경우. */
	if (nirqs < 4)
		/* [한국어] 최소 4 로 끌어올린다. 전역 변수를 직접 고쳐 쓰므로 이 보정은
		 * 이후 붙는 모든 장치에 남는다. */
		nirqs = 4;

	/* [한국어] 벡터를 최소 1 개, 최대 nirqs 개 확보한다. MSI-X 를 우선하고,
	 * 없으면 MSI 로 물러선다. PCI_IRQ_VIRTUAL 은 include/linux/pci.h 의
	 * 설명대로 '장치가 실제로 가진 인터럽트 수보다 많은 MSI-X 벡터를
	 * 확보' 하게 해 준다 - 그렇게 얻은 여분 벡터는 장치의 MSI-X 표에
	 * 기록되지 않으므로, 하드웨어가 쏘는 것이 아니라 드라이버가 다른
	 * 수단으로 다뤄야 한다. nirqs 를 크게 잡는 것과 짝이 되는 플래그인데,
	 * 이 드라이버 자신은 그 여분 벡터를 쓰지 않는다 - 실제 소비자는
	 * 모듈 파라미터 설명이 가리키는 NTB 쪽으로 보이나, 그 드라이버가
	 * 이 트리에 없어 확인하지 못했다. INTx 는 목록에 없다 -
	 * 이 장치는 MSI 계열을 전제로 한다. */
	nvecs = pci_alloc_irq_vectors(stdev->pdev, 1, nirqs,
				      PCI_IRQ_MSIX | PCI_IRQ_MSI |
				      PCI_IRQ_VIRTUAL);
	/* [한국어] 벡터를 하나도 못 잡았으면 실패. */
	if (nvecs < 0)
		return nvecs;

	/* [한국어] 펌웨어가 내 파티션 설정 블록에 적어 둔 '이벤트용 벡터 인덱스'.
	 * VEP(Virtual Endpoint) 는 이 관리 함수 자신을 가리킨다. */
	event_irq = ioread16(&stdev->mmio_part_cfg->vep_vector_number);
	/* [한국어] ★ 경계 검사. 16 비트 읽기라 음수가 될 수 없지만 방어적으로 함께
	 * 검사하고, 확보한 개수 이상이면 펌웨어 값이 잘못된 것이다. */
	if (event_irq < 0 || event_irq >= nvecs)
		/* [한국어] 잘못된 값을 아래 변환 함수에 넘기지 않고 실패로 끝낸다. */
		return -EFAULT;

	/* [한국어] 장치 안에서의 벡터 인덱스를 커널의 IRQ 번호로 바꾼다. */
	event_irq = pci_irq_vector(stdev->pdev, event_irq);
	/* [한국어] 변환 실패. */
	if (event_irq < 0)
		return event_irq;

	/* [한국어] 이벤트 ISR 를 건다. flags 0 은 공유하지 않는 인터럽트라는 뜻이고,
	 * 마지막 인자 stdev 가 ISR 의 dev 인자로 돌아온다. devm_ 이므로
	 * 장치가 떨어질 때 자동으로 해제된다. */
	rc = devm_request_irq(&stdev->pdev->dev, event_irq,
				switchtec_event_isr, 0,
				KBUILD_MODNAME, stdev);

	/* [한국어] IRQ 등록 실패. */
	if (rc)
		return rc;

	/* [한국어] DMA MRPC 를 쓰지 않는 구성이면 두 번째 벡터가 필요 없다. */
	if (!stdev->dma_mrpc)
		/* [한국어] 이 시점의 rc 는 0 이므로 성공 반환이다. */
		return rc;

	/* [한국어] 펌웨어가 MRPC 레지스터에 적어 둔 'DMA 완료용 벡터 인덱스'. */
	dma_mrpc_irq = ioread32(&stdev->mmio_mrpc->dma_vector);
	/* [한국어] 위와 같은 경계 검사 - 32 비트 읽기라 큰 값이 음수로 보일 수 있어
	 * 음수 검사도 실제로 의미가 있다. */
	if (dma_mrpc_irq < 0 || dma_mrpc_irq >= nvecs)
		/* [한국어] 범위를 벗어난 값이면 실패. */
		return -EFAULT;

	/* [한국어] IRQ 번호로 변환. */
	dma_mrpc_irq  = pci_irq_vector(stdev->pdev, dma_mrpc_irq);
	/* [한국어] 변환 실패. */
	if (dma_mrpc_irq < 0)
		return dma_mrpc_irq;

	/* [한국어] DMA MRPC 전용 ISR 를 건다. */
	rc = devm_request_irq(&stdev->pdev->dev, dma_mrpc_irq,
				switchtec_dma_mrpc_isr, 0,
				KBUILD_MODNAME, stdev);

	/* [한국어] 두 ISR 가 모두 걸렸으면 0 이다. */
	return rc;
}

/* [한국어] init_pff - PFF CSR 블록의 개수를 세고, 그중 '내 파티션 것'을 표시한다
 * 
 * @stdev: 이제 막 GAS 매핑이 끝난 장치. mmio_pff_csr 과 mmio_part_cfg 가
 *         이미 유효해야 한다.
 * @return: 없음. 결과는 stdev->pff_csr_count 와 stdev->pff_local[] 에 남는다.
 * 
 * PFF 란: Port Function Field 의 약자로, 스위치 안의 물리 포트 하나가
 * 호스트 쪽에 내보이는 PCI 설정 공간 한 벌(struct pff_csr_regs)이다.
 * 스위치는 이런 블록을 여러 개 갖고 있고, GAS 의 PFF_CSR 영역에
 * 연속 배열로 놓여 있다. 배열의 실제 길이는 모델마다 다르므로
 * 펌웨어에게 물어볼 수단이 없다 - 그래서 이 함수가 직접 센다.
 * 
 * 세는 방법: 배열을 앞에서부터 훑으며 vendor_id 를 읽어 본다. 유효한
 * 블록이면 Microsemi 벤더 ID 가 나오고, 배열이 끝나면 다른 값(보통
 * 0xFFFF)이 나온다. 그 지점의 인덱스가 곧 개수다. 상한은 헤더의
 * SWITCHTEC_MAX_PFF_CSR(255)로 묶여 있어 무한 루프가 되지 않는다.
 * 
 * '내 것' 표시가 필요한 이유: 스위치 하나는 여러 논리 파티션으로 쪼개져
 * 서로 다른 호스트에 붙을 수 있다. 이벤트를 마스킹할 때(mask_all_events)
 * 다른 파티션 소유의 PFF 까지 건드리면 남의 호스트에 영향을 준다.
 * 그래서 내 파티션 설정(part_cfg)이 가리키는 PFF 인스턴스 ID 들만
 * pff_local[] 에 1 로 찍어 두고, 마스킹 루프는 그 표를 보고 건너뛴다.
 * 
 * 실행 컨텍스트: probe 경로의 프로세스 문맥. 장치당 한 번, 인터럽트
 * 등록 전에 불리므로 경쟁 상대가 없어 락이 필요 없다.
 * 
 * 호출 체인:
 *   switchtec_pci_probe() -> switchtec_init_pci() -> [init_pff]
 *     -> ioread16()/ioread32() (MMIO 읽기) */
static void init_pff(struct switchtec_dev *stdev)
{
	/* [한국어] PFF 배열을 훑는 인덱스. 루프가 끝난 뒤에도 값을 쓰므로 루프 밖에
	 * 선언되어 있다 - 이 값이 곧 유효 블록 개수다. */
	int i;
	/* [한국어] MMIO 에서 읽어 온 값을 잠시 담는 임시 변수. */
	u32 reg;
	/* [한국어] 내 파티션의 설정 레지스터 블록. mmio_part_cfg 는 전체 파티션 배열에서
	 * stdev->partition 번째를 가리키도록 switchtec_init_pci 에서 이미 좁혀 놨다. */
	struct part_cfg_regs __iomem *pcfg = stdev->mmio_part_cfg;

	/* [한국어] 배열 상한(255)까지만 훑는다. 헤더가 정한 하드웨어 최대치이며,
	 * pff_local[] 배열 크기도 같은 상수라 인덱스가 넘칠 수 없다. */
	for (i = 0; i < SWITCHTEC_MAX_PFF_CSR; i++) {
		/* [한국어] 각 PFF 블록의 첫 필드인 vendor_id 를 16 비트로 읽는다. 이 필드는
		 * 그 포트가 호스트에 내보이는 PCI 설정 공간의 벤더 ID 자리다. */
		reg = ioread16(&stdev->mmio_pff_csr[i].vendor_id);
		/* [한국어] Microsemi ID(0x11f8)가 아니면 유효한 PFF 블록이 아니다 - 배열의 끝에
		 * 닿았다는 뜻이므로 멈춘다. 이때 i 가 유효 블록 개수가 된다. */
		if (reg != PCI_VENDOR_ID_MICROSEMI)
			break;
	}

	/* [한국어] 센 결과를 저장. 이후 이벤트 요약(ioctl_event_summary), 링크 상태
	 * 검사(check_link_state_events), 인덱스 검증(event_hdr_addr)이 전부
	 * 이 값을 상한으로 쓴다. */
	stdev->pff_csr_count = i;

	/* [한국어] USP(Upstream Port) - 호스트 쪽으로 향하는 포트 - 의 PFF 인스턴스 ID. */
	reg = ioread32(&pcfg->usp_pff_inst_id);
	/* [한국어] 읽어 온 ID 가 센 개수 범위 안일 때만 유효하다. 펌웨어가 '없음'을
	 * 뜻하는 큰 값을 넣어 둘 수 있으므로 반드시 걸러야 한다 -
	 * 거르지 않으면 아래 배열 쓰기가 범위를 벗어난다. */
	if (reg < stdev->pff_csr_count)
		/* [한국어] 그 PFF 를 '내 파티션 소유' 로 표시. */
		stdev->pff_local[reg] = 1;

	/* [한국어] VEP(Virtual Endpoint) - 이 관리 드라이버가 붙어 있는 그 엔드포인트 -
	 * 의 PFF 인스턴스 ID. 하위 8 비트만 ID 이고 나머지 비트는 다른 뜻이라
	 * 0xFF 로 잘라 낸다. */
	reg = ioread32(&pcfg->vep_pff_inst_id) & 0xFF;
	/* [한국어] 위와 같은 범위 검증. */
	if (reg < stdev->pff_csr_count)
		/* [한국어] 유효하면 내 것으로 표시. */
		stdev->pff_local[reg] = 1;

	/* [한국어] DSP(Downstream Port) - 드라이브나 다른 장치가 붙는 아래쪽 포트들 -
	 * 의 PFF 인스턴스 ID 배열. 길이는 레지스터 구조체가 정한 47 이다. */
	for (i = 0; i < ARRAY_SIZE(pcfg->dsp_pff_inst_id); i++) {
		/* [한국어] 각 하위 포트의 PFF 인스턴스 ID 를 읽는다. */
		reg = ioread32(&pcfg->dsp_pff_inst_id[i]);
		/* [한국어] 역시 범위 안일 때만 유효한 ID 로 본다. */
		if (reg < stdev->pff_csr_count)
			/* [한국어] 내 파티션에 속한 하위 포트로 표시. */
			stdev->pff_local[reg] = 1;
	}
}

/* [한국어] switchtec_init_pci - PCI 장치를 켜고 GAS 창을 매핑해 드라이버가 쓸
 *                     레지스터 포인터를 모두 세운다
 * 
 * @stdev: stdev_create() 가 막 만든 장치 객체. gen 필드는 이미 채워져 있다.
 * @pdev: PCI 코어가 넘겨준 물리 장치.
 * @return: 0 성공. 음수 errno 실패(-EBUSY 영역 충돌, -ENOMEM 매핑/할당 실패,
 *          -EOPNOTSUPP 알 수 없는 세대).
 * 
 * GAS 란: Global Address Space. Switchtec 은 자기 내부 레지스터 전체를
 * BAR0 하나에 펼쳐 놓는다. 그 안에서 오프셋으로 구역이 나뉘며(헤더의
 * SWITCHTEC_GAS_*_OFFSET), MRPC 는 0x0, 시스템 정보는 0x2000,
 * 파티션 설정은 0x4000, PFF CSR 은 0x134000 하는 식이다.
 * 
 * 왜 매핑을 두 번 하는가(★ 이 함수의 핵심):
 *   - 0x0~0x1000 의 MRPC 구역은 devm_ioremap_wc 로 'write combining' 매핑한다.
 *     MRPC 입력 데이터는 최대 1KB 를 통째로 밀어 넣는 대량 쓰기라, 쓰기를
 *     모아 보내는 WC 가 훨씬 빠르다. 대신 쓰기가 하드웨어에 도달하는 시점이
 *     미뤄지므로, 명령을 띄우기 직전에 flush_wc_buf() 로 버퍼를 비워야 한다.
 *   - 0x1000 이후는 보통의 devm_ioremap(uncached)으로 잡는다. 이쪽은 상태
 *     레지스터라 매 접근이 실제로 하드웨어에 닿아야 한다.
 *   그리고 stdev->mmio 를 '두 번째 매핑 주소 - 0x1000' 으로 되돌려 두어,
 *   이후 코드가 GAS 절대 오프셋을 그대로 더해 쓸 수 있게 만든다.
 * 
 * 실행 컨텍스트: probe 의 프로세스 문맥. devm_ 계열을 쓰므로 실패해도
 * 개별 해제 코드가 필요 없다 - 장치가 떨어질 때 코어가 되돌린다.
 * 
 * 에러 경로: 각 단계에서 곧바로 반환한다. 호출자(probe)는 err_put 으로
 * 가서 minor 반납과 put_device 를 한다.
 * 
 * 호출 체인:
 *   switchtec_pci_probe() -> [switchtec_init_pci] -> init_pff() */
static int switchtec_init_pci(struct switchtec_dev *stdev,
			      struct pci_dev *pdev)
{
	/* [한국어] 단계별 반환값. */
	int rc;
	/* [한국어] 두 번째(0x1000 이후) 매핑의 반환 주소를 잠시 받는 변수. */
	void __iomem *map;
	/* [한국어] BAR0 의 물리 시작 주소와 길이. */
	unsigned long res_start, res_len;
	/* [한국어] 파티션 ID 레지스터의 주소. 세대에 따라 구조체 안 위치가 달라
	 * 포인터로 한 번 받아 둔 뒤 읽는다. */
	u32 __iomem *part_id;

	/* [한국어] 장치를 켠다(Memory Space Enable 등). pcim_ 접두사는 devm 관리형이라
	 * 장치가 떨어질 때 자동으로 꺼진다. */
	rc = pcim_enable_device(pdev);
	/* [한국어] 켜지 못했으면 더 진행할 수 없다. */
	if (rc)
		return rc;

	/* [한국어] DMA 마스크를 64 비트로 넓힌다. 아래에서 잡는 dma_mrpc 버퍼의 주소를
	 * 장치의 dma_addr 레지스터에 그대로 실어야 하므로, 64 비트 주소를 쓸 수
	 * 있는지 미리 확인하고 커널에 알려 두는 것이다. */
	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	/* [한국어] 플랫폼이 64 비트 DMA 를 지원하지 않으면 실패로 처리한다. */
	if (rc)
		return rc;

	/* [한국어] 버스 마스터 비트를 켠다. 장치가 스스로 호스트 메모리에 쓰려면
	 * (DMA MRPC 응답 기록, MSI/MSI-X 메시지 쓰기) 반드시 필요하다.
	 * stdev_kill() 이 반대로 pci_clear_master 로 끄는 짝이다. */
	pci_set_master(pdev);

	/* [한국어] BAR0 의 물리 주소. */
	res_start = pci_resource_start(pdev, 0);
	/* [한국어] BAR0 의 크기. 모델에 따라 다르며, GAS 전체를 덮는다. */
	res_len = pci_resource_len(pdev, 0);

	/* [한국어] BAR0 영역을 이 드라이버 이름으로 예약한다. 다른 드라이버가 같은
	 * 물리 영역을 동시에 매핑하는 사고를 막는 표식이다. */
	if (!devm_request_mem_region(&pdev->dev, res_start,
				     res_len, KBUILD_MODNAME))
		/* [한국어] 이미 누가 쓰고 있으면 -EBUSY. */
		return -EBUSY;

	/* [한국어] GAS 앞머리 0x0~0x1000(SWITCHTEC_GAS_TOP_CFG_OFFSET) 만 WC 로 매핑.
	 * 이 구간이 곧 struct mrpc_regs - 입력 1KB, 출력 1KB, 그리고 cmd/status
	 * 레지스터들이다. */
	stdev->mmio_mrpc = devm_ioremap_wc(&pdev->dev, res_start,
					   SWITCHTEC_GAS_TOP_CFG_OFFSET);
	/* [한국어] 매핑 실패는 주소 공간 부족이므로 -ENOMEM. */
	if (!stdev->mmio_mrpc)
		return -ENOMEM;

	/* [한국어] 나머지 구간(0x1000 부터 BAR 끝까지)을 uncached 로 매핑. */
	map = devm_ioremap(&pdev->dev,
			   res_start + SWITCHTEC_GAS_TOP_CFG_OFFSET,
			   res_len - SWITCHTEC_GAS_TOP_CFG_OFFSET);
	/* [한국어] 역시 실패하면 -ENOMEM. */
	if (!map)
		return -ENOMEM;

	/* [한국어] ★ 포인터 산술의 핵심. map 은 GAS 오프셋 0x1000 에 대응하는 가상
	 * 주소다. 여기서 0x1000 을 빼 두면 stdev->mmio 는 'GAS 오프셋 0 에
	 * 해당하는 가상 주소' 가 된다 - 실제로 그 주소를 역참조하면 안 되지만
	 * (그 구간은 이 매핑에 없다), 아래처럼 오프셋을 더해 쓰기에는 편하다. */
	stdev->mmio = map - SWITCHTEC_GAS_TOP_CFG_OFFSET;
	/* [한국어] 스위치 전역 이벤트 레지스터 블록(0x1800). */
	stdev->mmio_sw_event = stdev->mmio + SWITCHTEC_GAS_SW_EVENT_OFFSET;
	/* [한국어] 시스템 정보 블록(0x2000) - device_id, 펌웨어 버전, 벤더 문자열 등. */
	stdev->mmio_sys_info = stdev->mmio + SWITCHTEC_GAS_SYS_INFO_OFFSET;
	/* [한국어] 플래시 파티션 정보 블록(0x2200). */
	stdev->mmio_flash_info = stdev->mmio + SWITCHTEC_GAS_FLASH_INFO_OFFSET;
	/* [한국어] NTB 레지스터 블록(0x10000). 이 드라이버는 그중 partition_count 와
	 * flush_wc_buf 가 쓰는 outbound doorbell 만 건드린다. */
	stdev->mmio_ntb = stdev->mmio + SWITCHTEC_GAS_NTB_OFFSET;

	/* [한국어] 세대에 따라 sys_info_regs 의 공용체 중 어느 쪽을 볼지 갈린다.
	 * Gen3 레이아웃과 Gen4 레이아웃은 필드 배치가 완전히 다르다. */
	if (stdev->gen == SWITCHTEC_GEN3)
		/* [한국어] Gen3 배치의 partition_id 위치. */
		part_id = &stdev->mmio_sys_info->gen3.partition_id;
	/* [한국어] Gen4 이상(GEN5 포함) 배치. 판정이 '>=' 인 덕분에 새 세대가 추가돼도
	 * Gen4 레이아웃을 그대로 쓴다. */
	else if (stdev->gen >= SWITCHTEC_GEN4)
		part_id = &stdev->mmio_sys_info->gen4.partition_id;
	/* [한국어] 세 값(GEN3/GEN4/GEN5) 중 어느 것도 아닌 경우 - 현재 enum 상
	 * 도달할 수 없지만, 새 세대가 추가될 때를 대비한 방어다. */
	else
		/* [한국어] 해석할 레이아웃을 모르므로 지원하지 않는다고 알린다. */
		return -EOPNOTSUPP;

	/* [한국어] 이 관리 엔드포인트가 속한 파티션 번호. 8 비트 읽기다. */
	stdev->partition = ioread8(part_id);
	/* [한국어] 스위치 전체의 파티션 개수. NTB 정보 블록의 첫 바이트에 있다. */
	stdev->partition_count = ioread8(&stdev->mmio_ntb->partition_count);
	/* [한국어] 파티션 설정 블록 배열의 시작(0x4000). 파티션마다 한 벌씩 이어진다. */
	stdev->mmio_part_cfg_all = stdev->mmio + SWITCHTEC_GAS_PART_CFG_OFFSET;
	/* [한국어] 그 배열에서 '내 파티션' 항목만 따로 가리켜 둔다. 이후 MRPC 완료
	 * 이벤트나 이벤트 요약을 볼 때 매번 인덱싱하지 않으려는 편의다. */
	stdev->mmio_part_cfg = &stdev->mmio_part_cfg_all[stdev->partition];
	/* [한국어] PFF CSR 배열의 시작(0x134000). */
	stdev->mmio_pff_csr = stdev->mmio + SWITCHTEC_GAS_PFF_CSR_OFFSET;

	/* [한국어] 펌웨어가 0 을 보고했거나 읽기가 실패한 경우에 대한 보정.
	 * partition_count 는 아래 여러 루프의 상한이라 0 이면 정보를 하나도
	 * 못 읽고, 배열 인덱스 계산에도 쓰이므로 최소 1 로 올려 둔다. */
	if (stdev->partition_count < 1)
		/* [한국어] 최소 한 개는 있는 것으로 간주. */
		stdev->partition_count = 1;

	/* [한국어] PFF 블록 개수를 세고 내 파티션 소유를 표시한다. 위에서 mmio_pff_csr
	 * 과 mmio_part_cfg 가 세워진 뒤여야 하므로 순서가 중요하다. */
	init_pff(stdev);

	/* [한국어] pdev 에 stdev 를 매달아 둔다. remove 와 ISR 가 pci_get_drvdata 로
	 * 되찾는다. */
	pci_set_drvdata(pdev, stdev);

	/* [한국어] 모듈 파라미터로 DMA MRPC 를 꺼 뒀으면 여기서 끝 - 이후 MRPC 는
	 * MMIO 폴링/인터럽트 방식으로만 동작한다. */
	if (!use_dma_mrpc)
		/* [한국어] DMA 없이도 정상 동작하므로 성공 반환. */
		return 0;

	/* [한국어] 펌웨어가 DMA MRPC 를 지원하는지 확인. dma_ver 이 0 이면 그 기능이
	 * 없는 펌웨어다. */
	if (ioread32(&stdev->mmio_mrpc->dma_ver) == 0)
		/* [한국어] 지원하지 않아도 정상이므로 성공 반환. */
		return 0;

	/* [한국어] MRPC 응답을 장치가 직접 써 넣을 코히런트 버퍼를 잡는다.
	 * dma_alloc_coherent 는 CPU 가상 주소(반환값)와 장치가 쓸 버스 주소
	 * (dma_mrpc_dma_addr)를 함께 준다. '코히런트' 라서 장치가 쓴 내용을
	 * CPU 가 읽기 전에 캐시 무효화를 따로 할 필요가 없다 - mrpc_complete_cmd
	 * 가 memcpy 로 바로 읽는 이유다. */
	stdev->dma_mrpc = dma_alloc_coherent(&stdev->pdev->dev,
					     sizeof(*stdev->dma_mrpc),
					     &stdev->dma_mrpc_dma_addr,
					     GFP_KERNEL);
	/* [한국어] 버퍼를 못 잡았으면 실패. 이 경우 앞서 켠 것들은 devm 이 정리한다. */
	if (stdev->dma_mrpc == NULL)
		return -ENOMEM;

	/* [한국어] 모든 초기화 성공. */
	return 0;
}

/* [한국어] switchtec_exit_pci - DMA MRPC 를 끄고 코히런트 버퍼를 반납한다
 * 
 * @stdev: 정리 대상 장치.
 * @return: 없음
 * 
 * 왜 별도 함수인가: devm_ 으로 잡은 것들은 커널이 알아서 되돌리지만,
 * dma_alloc_coherent 로 잡은 버퍼는 명시적으로 풀어야 한다. 게다가
 * 그냥 풀면 안 된다 - 장치가 아직 그 주소로 DMA 를 쏘고 있을 수 있으므로,
 * 반드시 (1) DMA 기능 비트를 끄고 (2) 주소 레지스터를 0 으로 지운 뒤에
 * (3) 버퍼를 반납해야 한다. 순서를 뒤집으면 해제된 메모리에 장치가
 * 쓰는 use-after-free 가 된다.
 * 
 * 실행 컨텍스트: probe 실패 경로 또는 remove 의 프로세스 문맥.
 * remove 에서는 이미 stdev_kill() 이 pci_clear_master 로 버스 마스터를
 * 꺼 둔 뒤라 이중으로 안전하다.
 * 
 * 호출 체인:
 *   switchtec_pci_probe()(실패 시) / switchtec_pci_remove() -> [switchtec_exit_pci] */
static void switchtec_exit_pci(struct switchtec_dev *stdev)
{
	/* [한국어] DMA MRPC 를 쓰지 않았으면 반납할 것이 없다. */
	if (stdev->dma_mrpc) {
		/* [한국어] (1) DMA 기능 비트를 끈다. 이 레지스터는 uncached 매핑이 아니라
		 * WC 매핑 구간(0x0~0x1000)에 있으므로, 쓰기가 아직 버퍼에 머물 수 있다. */
		iowrite32(0, &stdev->mmio_mrpc->dma_en);
		/* [한국어] 그래서 곧바로 flush_wc_buf 로 WC 버퍼를 비운다 - 아래 주소 지우기와
		 * 버퍼 해제가 실제 하드웨어에 도달한 '끄기' 뒤에 오도록 순서를 강제한다. */
		flush_wc_buf(stdev);
		/* [한국어] (2) 장치가 기억하고 있던 64 비트 DMA 주소를 지운다. 이제 장치는
		 * 해제될 버퍼를 가리키지 않는다. */
		writeq(0, &stdev->mmio_mrpc->dma_addr);
		/* [한국어] (3) 코히런트 버퍼 반납. 가상 주소와 버스 주소를 둘 다 넘겨야 한다. */
		dma_free_coherent(&stdev->pdev->dev, sizeof(*stdev->dma_mrpc),
				  stdev->dma_mrpc, stdev->dma_mrpc_dma_addr);
		/* [한국어] 포인터를 비워 둔다. 이 값이 NULL 인지가 곧 'DMA MRPC 를 쓰는가' 의
		 * 판정이므로, 반납 후에도 남아 있으면 이후 경로가 죽은 버퍼를 읽는다. */
		stdev->dma_mrpc = NULL;
	}
}

/* [한국어] switchtec_pci_probe - 장치가 나타났을 때 관리 인터페이스를 세운다
 * 
 * @pdev: PCI 코어가 넘긴 장치.
 * @id: id_table 에서 일치한 항목. driver_data 에 세대가 들어 있다.
 * @return: 0 성공, 음수 errno 실패.
 * 
 * 동작 단계:
 *   1) NTB 클래스로 나타난 함수라면 ntb_hw_switchtec 모듈 적재를 요청한다
 *   2) stdev_create() 로 장치 객체/문자 디바이스 껍데기를 만든다
 *   3) id->driver_data 의 세대를 stdev 에 옮긴다 - 아래 init_pci 가 이 값으로
 *      레지스터 레이아웃을 고르므로 반드시 먼저 해야 한다
 *   4) switchtec_init_pci() 로 BAR0 를 매핑하고 레지스터 포인터를 세운다
 *   5) switchtec_init_isr() 로 인터럽트를 건다
 *   6) MRPC 완료 이벤트와 링크 상태 이벤트의 인터럽트를 켠다
 *   7) DMA MRPC 를 쓸 수 있으면 장치에 버퍼 주소를 알려 준다
 *   8) cdev_device_add() 로 /dev/switchtecN 을 실제로 노출한다 - 이 줄이
 *      지나는 순간부터 유저스페이스가 open/ioctl 을 걸 수 있으므로,
 *      그 전에 모든 상태가 완성되어 있어야 한다
 * 
 * 실행 컨텍스트: 프로세스 문맥. 장치별로 한 번. 5) 이후에는 ISR 가
 * 동시에 돌 수 있으므로 그 뒤의 상태 변경은 이미 락이 필요한 영역이다.
 * 
 * 에러 경로: 세 개의 라벨로 단계별 역순 정리를 한다.
 * 
 * 호출 체인:
 *   (PCI 코어) pci_register_driver()/장치 열거 -> [switchtec_pci_probe]
 *     -> stdev_create() -> switchtec_init_pci() -> switchtec_init_isr()
 *     -> cdev_device_add() */
static int switchtec_pci_probe(struct pci_dev *pdev,
			       const struct pci_device_id *id)
{
	/* [한국어] 이 장치의 상태를 모두 담을 객체. */
	struct switchtec_dev *stdev;
	/* [한국어] 단계별 반환값. */
	int rc;

	/* [한국어] 이 PCI 함수가 브리지 클래스(0x0680)로 나타났다면 관리 함수가 아니라
	 * NTB 함수다. id_table 이 두 클래스를 모두 잡으므로 여기서 갈린다. */
	if (pdev->class == (PCI_CLASS_BRIDGE_OTHER << 8))
		/* [한국어] NTB 드라이버 모듈을 비동기로 적재 요청한다. _nowait 라 결과를
		 * 기다리지 않고, 실패해도 이 드라이버의 동작에는 영향이 없다.
		 * 그 모듈(ntb_hw_switchtec)은 이 부분 체크아웃에 포함되어 있지 않아
		 * 동작을 직접 확인하지는 못했다. */
		request_module_nowait("ntb_hw_switchtec");

	/* [한국어] 장치 객체, mutex, 작업 큐 항목, minor 번호, cdev 를 준비한다. */
	stdev = stdev_create(pdev);
	/* [한국어] minor 고갈이나 메모리 부족으로 실패할 수 있다. */
	if (IS_ERR(stdev))
		return PTR_ERR(stdev);

	/* [한국어] 표에서 일치한 항목이 실어 보낸 세대 값. 이후 모든 레지스터 해석의
	 * 기준이 되므로 init_pci 보다 반드시 앞에 있어야 한다. */
	stdev->gen = id->driver_data;

	/* [한국어] BAR0 매핑과 레지스터 포인터 세팅, PFF 계수, DMA 버퍼 확보. */
	rc = switchtec_init_pci(stdev, pdev);
	/* [한국어] 실패하면 아직 IRQ 도 cdev 도 없으므로 객체만 되돌리면 된다. */
	if (rc)
		goto err_put;

	/* [한국어] MSI/MSI-X 벡터를 잡고 이벤트 ISR(와 필요하면 DMA MRPC ISR)를 건다. */
	rc = switchtec_init_isr(stdev);
	/* [한국어] IRQ 확보 실패. */
	if (rc) {
		/* [한국어] 원인을 로그에 남긴다 - 이 실패는 벡터 부족이나 펌웨어가 알려 준
		 * 벡터 번호가 범위를 벗어난 경우다. */
		dev_err(&stdev->dev, "failed to init isr.\n");
		goto err_exit_pci;
	}

	/* [한국어] 내 파티션의 MRPC 완료 이벤트 헤더에 (지움 | IRQ 활성) 을 쓴다.
	 * SWITCHTEC_EVENT_CLEAR 와 SWITCHTEC_EVENT_OCCURRED 는 둘 다 BIT(0) 로
	 * 같은 비트인데, 쓸 때는 '지움' 으로, 읽을 때는 '발생함' 으로 해석되는
	 * RW1C 성격의 비트다. 즉 이 한 번의 쓰기가 '밀린 이벤트를 지우고
	 * 앞으로는 인터럽트로 알려 달라' 는 뜻이 된다. */
	iowrite32(SWITCHTEC_EVENT_CLEAR |
		  SWITCHTEC_EVENT_EN_IRQ,
		  &stdev->mmio_part_cfg->mrpc_comp_hdr);
	/* [한국어] 모든 PFF 의 링크 상태 이벤트에도 같은 설정을 건다. */
	enable_link_state_events(stdev);

	/* [한국어] DMA MRPC 버퍼를 확보한 경우에만. */
	if (stdev->dma_mrpc)
		/* [한국어] 장치에 버퍼 주소를 알려 주고 DMA 기능 비트를 켠다. 이 뒤로는 MRPC
		 * 응답이 MMIO 출력 영역이 아니라 호스트 메모리로 온다. */
		enable_dma_mrpc(stdev);

	/* [한국어] 문자 디바이스를 등록하고 sysfs 장치를 함께 추가한다. 이 호출이
	 * 성공한 직후부터 /dev/switchtecN 이 열릴 수 있다. */
	rc = cdev_device_add(&stdev->cdev, &stdev->dev);
	/* [한국어] 등록 실패. */
	if (rc)
		goto err_devadd;

	/* [한국어] 준비 완료를 알린다. */
	dev_info(&stdev->dev, "Management device registered.\n");

	/* [한국어] probe 성공. */
	return 0;

/* [한국어] cdev_device_add 실패 지점. */
err_devadd:
	/* [한국어] 대기 중인 MRPC 사용자를 모두 깨워 보내고 하드웨어를 죽은 것으로
	 * 표시한다. 여기서는 아직 사용자가 있을 수 없지만, IRQ 가 이미 걸려
	 * 있으므로 alive 를 내려 ISR 뒤에 오는 작업들이 조용히 끝나게 한다. */
	stdev_kill(stdev);
/* [한국어] switchtec_init_isr 실패 지점(또는 위에서 흘러 내려온 경우). */
err_exit_pci:
	/* [한국어] DMA 버퍼를 반납한다. IRQ 자체는 devm 이 되돌린다. */
	switchtec_exit_pci(stdev);
/* [한국어] switchtec_init_pci 실패 지점(또는 위에서 흘러 내려온 경우). */
err_put:
	/* [한국어] stdev_create 가 잡은 minor 번호를 반납한다. dev.devt 에 이미
	 * MKDEV 로 합쳐 넣었으므로 MINOR 매크로로 되꺼낸다. */
	ida_free(&switchtec_minor_ida, MINOR(stdev->dev.devt));
	/* [한국어] 장치 참조를 놓는다. 마지막 참조였다면 stdev_release 가 불려
	 * stdev 자체가 kfree 된다. */
	put_device(&stdev->dev);
	return rc;
}

/* [한국어] switchtec_pci_remove - 장치가 사라질 때 probe 의 역순으로 정리한다
 * 
 * @pdev: 떨어져 나가는 PCI 장치.
 * @return: 없음
 * 
 * 핫플러그로 스위치가 빠지거나 rmmod 로 드라이버를 뗄 때 불린다.
 * 가장 중요한 것은 순서다: 먼저 /dev 노드를 없애 새 사용자가 들어오지
 * 못하게 막고, 그 다음 이미 들어와 있는 사용자를 깨워 내보내고,
 * 마지막에 하드웨어 자원을 반납한다.
 * 
 * ★ 주의할 점: cdev_device_del 뒤에도 이미 파일을 열어 둔 프로세스는
 * 여전히 read/write/ioctl 을 부를 수 있다. 그래서 stdev_kill() 이
 * stdev->alive 를 false 로 내려 두고, 모든 진입점이 앞머리에서
 * lock_mutex_and_test_alive() 로 그 값을 확인해 -ENODEV 를 돌려준다.
 * 장치 객체 자체는 참조 카운트로 살아 있다가 마지막 fd 가 닫힐 때
 * 비로소 해제된다.
 * 
 * 실행 컨텍스트: 프로세스 문맥.
 * 
 * 호출 체인:
 *   (PCI 코어) -> [switchtec_pci_remove] -> stdev_kill() -> switchtec_exit_pci() */
static void switchtec_pci_remove(struct pci_dev *pdev)
{
	/* [한국어] probe 에서 매달아 둔 장치 객체를 되찾는다. */
	struct switchtec_dev *stdev = pci_get_drvdata(pdev);

	/* [한국어] 연결을 먼저 끊는다. 이 뒤에 pci_get_drvdata 를 부르는 경로가
	 * 죽은 포인터를 보지 않게 하려는 것. */
	pci_set_drvdata(pdev, NULL);

	/* [한국어] 문자 디바이스와 sysfs 장치를 제거한다. 새로운 open 은 이제 실패한다. */
	cdev_device_del(&stdev->cdev, &stdev->dev);
	/* [한국어] minor 번호 반납 - 다른 장치가 재사용할 수 있게 된다. */
	ida_free(&switchtec_minor_ida, MINOR(stdev->dev.devt));
	/* [한국어] 제거를 로그에 남긴다. 아직 dev 가 살아 있어야 하므로
	 * put_device 보다 앞에 있어야 한다. */
	dev_info(&stdev->dev, "unregistered.\n");
	/* [한국어] 버스 마스터를 끄고, MRPC 를 기다리던 사용자를 전부 깨워
	 * -EIO 로 내보내고, alive 를 내린다. */
	stdev_kill(stdev);
	/* [한국어] DMA MRPC 를 끄고 코히런트 버퍼를 반납한다. 위에서 버스 마스터가
	 * 이미 꺼져 있어 장치가 그 사이 DMA 를 쏠 수 없다. */
	switchtec_exit_pci(stdev);
	/* [한국어] stdev_create 에서 pci_dev_get 으로 올려 둔 pdev 참조를 놓는다. */
	pci_dev_put(stdev->pdev);
	/* [한국어] 포인터를 지운다 - 남아 있는 fd 가 이 필드를 통해 사라진 장치에
	 * 접근하지 못하게 하는 방어다. */
	stdev->pdev = NULL;
	/* [한국어] 드라이버가 쥐고 있던 장치 참조를 놓는다. 열린 fd 가 없으면
	 * 여기서 refcount 가 0 이 되어 stdev_release -> kfree 로 이어진다. */
	put_device(&stdev->dev);
}

/* [한국어] SWITCHTEC_PCI_DEVICE - Microsemi(현 Microchip) 벤더 ID 를 쓰는 스위치
 * 한 모델을 pci_device_id 표의 '두 항목'으로 펼치는 매크로.
 * 왜 두 개인가: 같은 실리콘이 두 가지 PCI 클래스 코드로 나타난다.
 *  - PCI_CLASS_MEMORY_OTHER(0x0580) : 관리용 엔드포인트. 이 드라이버가
 *    BAR0 의 GAS(Global Address Space) 창을 잡아 MRPC 를 주고받는 그 함수다.
 *  - PCI_CLASS_BRIDGE_OTHER(0x0680) : NTB(Non-Transparent Bridge) 엔드포인트.
 *    switchtec_pci_probe() 는 이 클래스를 보면 request_module_nowait 로
 *    ntb_hw_switchtec 모듈을 부른다(그 모듈은 이 부분 체크아웃에 없다).
 * subvendor/subdevice 를 PCI_ANY_ID 로 두어 보드 제조사가 무엇이든 잡고,
 * class_mask 0xFFFFFFFF 로 클래스 코드는 정확히 일치할 때만 잡는다.
 * driver_data 에 세대(enum switchtec_gen)를 실어 두면 probe 가 그대로
 * stdev->gen 에 옮겨 담고, 이후 sys_info/flash_info 레지스터 레이아웃을
 * gen3 용과 gen4 용 중 어느 쪽으로 해석할지가 그 값으로 갈린다.
 * 주의: 매크로 본문은 역슬래시로 이어진 한 줄이므로 그 사이에는 주석을
 * 끼워 넣을 수 없다 - 필드 설명을 여기 한데 모아 둔 이유다. */
#define SWITCHTEC_PCI_DEVICE(device_id, gen) \
	{ \
		.vendor     = PCI_VENDOR_ID_MICROSEMI, \
		.device     = device_id, \
		.subvendor  = PCI_ANY_ID, \
		.subdevice  = PCI_ANY_ID, \
		.class      = (PCI_CLASS_MEMORY_OTHER << 8), \
		.class_mask = 0xFFFFFFFF, \
		.driver_data = gen, \
	}, \
	{ \
		.vendor     = PCI_VENDOR_ID_MICROSEMI, \
		.device     = device_id, \
		.subvendor  = PCI_ANY_ID, \
		.subdevice  = PCI_ANY_ID, \
		.class      = (PCI_CLASS_BRIDGE_OTHER << 8), \
		.class_mask = 0xFFFFFFFF, \
		.driver_data = gen, \
	}

/* [한국어] SWITCHTEC_PCI100X_DEVICE - 위와 완전히 같은 모양이되 벤더 ID 만 다르다.
 * PCI1001~PCI1006 계열은 PCI_VENDOR_ID_MICROSEMI(0x11f8)가 아니라
 * PCI_VENDOR_ID_EFAR(0x1055) 로 나타난다. 0x1055 는 원래 SMSC 계열에
 * 할당됐던 벤더 ID 이며, 같은 회사군으로 편입된 뒤 이 소형 스위치들이
 * 그 ID 를 그대로 쓴다. 클래스 두 개(관리용/NTB)로 펼치는 것도, 정확히
 * 일치를 요구하는 class_mask 도, driver_data 로 세대를 넘기는 것도 동일하다. */
#define SWITCHTEC_PCI100X_DEVICE(device_id, gen) \
	{ \
		.vendor     = PCI_VENDOR_ID_EFAR, \
		.device     = device_id, \
		.subvendor  = PCI_ANY_ID, \
		.subdevice  = PCI_ANY_ID, \
		.class      = (PCI_CLASS_MEMORY_OTHER << 8), \
		.class_mask = 0xFFFFFFFF, \
		.driver_data = gen, \
	}, \
	{ \
		.vendor     = PCI_VENDOR_ID_EFAR, \
		.device     = device_id, \
		.subvendor  = PCI_ANY_ID, \
		.subdevice  = PCI_ANY_ID, \
		.class      = (PCI_CLASS_BRIDGE_OTHER << 8), \
		.class_mask = 0xFFFFFFFF, \
		.driver_data = gen, \
	}

/* [한국어] switchtec_pci_tbl - 이 드라이버가 붙을 장치 목록.
 * MODULE_DEVICE_TABLE 이 이 배열을 modules.alias 로 뽑아 주므로,
 * udev/kmod 가 새 장치를 보면 이 모듈을 자동으로 적재한다.
 * 표기 규약: 뒤의 '24xG3' 는 '레인(포트) 수 x PCIe 세대' 를 뜻한다 -
 * 24xG3 이면 Gen3 24 레인, 100XG5 면 Gen5 100 레인이다. 앞의 알파벳
 * (PFX/PSX/PAX/PFXL/PFXI/PFXA/PSXA/PAXA)은 제조사의 제품군 이름인데,
 * 그 약어의 정확한 원어 표기는 이 트리 안에서 근거를 찾지 못했으므로
 * 여기서는 '제품군이 다르다'는 사실만 적는다. 드라이버 관점에서 제품군
 * 차이는 의미가 없다 - 전부 같은 probe 경로를 타고, 오직 driver_data 의
 * 세대 값만이 레지스터 해석을 가른다. */
static const struct pci_device_id switchtec_pci_tbl[] = {
	/* [한국어] Gen3 제품군 첫 묶음. 각 줄이 매크로를 거쳐 관리용(0x0580)/NTB(0x0680)
	 * 두 항목으로 펼쳐지므로, 실제 표 크기는 줄 수의 두 배가 된다. */
	SWITCHTEC_PCI_DEVICE(0x8531, SWITCHTEC_GEN3),  /* PFX 24xG3 */
	SWITCHTEC_PCI_DEVICE(0x8532, SWITCHTEC_GEN3),  /* PFX 32xG3 */
	SWITCHTEC_PCI_DEVICE(0x8533, SWITCHTEC_GEN3),  /* PFX 48xG3 */
	/* [한국어] 같은 Gen3 묶음의 나머지 - 레인 수만 다르고 드라이버 동작은 동일하다. */
	SWITCHTEC_PCI_DEVICE(0x8534, SWITCHTEC_GEN3),  /* PFX 64xG3 */
	SWITCHTEC_PCI_DEVICE(0x8535, SWITCHTEC_GEN3),  /* PFX 80xG3 */
	SWITCHTEC_PCI_DEVICE(0x8536, SWITCHTEC_GEN3),  /* PFX 96xG3 */
	/* [한국어] Gen3 두 번째 제품군. driver_data 가 여전히 SWITCHTEC_GEN3 이므로
	 * sys_info_regs 의 gen3 공용체 쪽을 읽게 된다. */
	SWITCHTEC_PCI_DEVICE(0x8541, SWITCHTEC_GEN3),  /* PSX 24xG3 */
	SWITCHTEC_PCI_DEVICE(0x8542, SWITCHTEC_GEN3),  /* PSX 32xG3 */
	SWITCHTEC_PCI_DEVICE(0x8543, SWITCHTEC_GEN3),  /* PSX 48xG3 */
	/* [한국어] 같은 제품군의 큰 레인 수 모델들. */
	SWITCHTEC_PCI_DEVICE(0x8544, SWITCHTEC_GEN3),  /* PSX 64xG3 */
	SWITCHTEC_PCI_DEVICE(0x8545, SWITCHTEC_GEN3),  /* PSX 80xG3 */
	SWITCHTEC_PCI_DEVICE(0x8546, SWITCHTEC_GEN3),  /* PSX 96xG3 */
	/* [한국어] Gen3 세 번째 제품군. */
	SWITCHTEC_PCI_DEVICE(0x8551, SWITCHTEC_GEN3),  /* PAX 24XG3 */
	SWITCHTEC_PCI_DEVICE(0x8552, SWITCHTEC_GEN3),  /* PAX 32XG3 */
	SWITCHTEC_PCI_DEVICE(0x8553, SWITCHTEC_GEN3),  /* PAX 48XG3 */
	/* [한국어] 위 제품군의 나머지 레인 수 변형. */
	SWITCHTEC_PCI_DEVICE(0x8554, SWITCHTEC_GEN3),  /* PAX 64XG3 */
	SWITCHTEC_PCI_DEVICE(0x8555, SWITCHTEC_GEN3),  /* PAX 80XG3 */
	SWITCHTEC_PCI_DEVICE(0x8556, SWITCHTEC_GEN3),  /* PAX 96XG3 */
	/* [한국어] Gen3 네 번째 제품군. */
	SWITCHTEC_PCI_DEVICE(0x8561, SWITCHTEC_GEN3),  /* PFXL 24XG3 */
	SWITCHTEC_PCI_DEVICE(0x8562, SWITCHTEC_GEN3),  /* PFXL 32XG3 */
	SWITCHTEC_PCI_DEVICE(0x8563, SWITCHTEC_GEN3),  /* PFXL 48XG3 */
	/* [한국어] 위 제품군의 나머지 레인 수 변형. */
	SWITCHTEC_PCI_DEVICE(0x8564, SWITCHTEC_GEN3),  /* PFXL 64XG3 */
	SWITCHTEC_PCI_DEVICE(0x8565, SWITCHTEC_GEN3),  /* PFXL 80XG3 */
	SWITCHTEC_PCI_DEVICE(0x8566, SWITCHTEC_GEN3),  /* PFXL 96XG3 */
	/* [한국어] Gen3 다섯 번째 제품군 - Gen3 목록의 마지막이다. */
	SWITCHTEC_PCI_DEVICE(0x8571, SWITCHTEC_GEN3),  /* PFXI 24XG3 */
	SWITCHTEC_PCI_DEVICE(0x8572, SWITCHTEC_GEN3),  /* PFXI 32XG3 */
	SWITCHTEC_PCI_DEVICE(0x8573, SWITCHTEC_GEN3),  /* PFXI 48XG3 */
	/* [한국어] Gen3 목록의 끝. 여기까지가 SWITCHTEC_GEN3 로 표시된 항목이다. */
	SWITCHTEC_PCI_DEVICE(0x8574, SWITCHTEC_GEN3),  /* PFXI 64XG3 */
	SWITCHTEC_PCI_DEVICE(0x8575, SWITCHTEC_GEN3),  /* PFXI 80XG3 */
	SWITCHTEC_PCI_DEVICE(0x8576, SWITCHTEC_GEN3),  /* PFXI 96XG3 */
	/* [한국어] 여기서부터 driver_data 가 SWITCHTEC_GEN4 로 바뀐다. 세대가 바뀌면
	 * sys_info_regs/flash_info_regs 의 gen4 공용체 쪽을 읽어야 하고,
	 * 파티션 종류도 GEN3 의 13 개에서 GEN4 의 19 개로 늘어난다
	 * (SWITCHTEC_NUM_PARTITIONS_GEN3/GEN4). */
	SWITCHTEC_PCI_DEVICE(0x4000, SWITCHTEC_GEN4),  /* PFX 100XG4 */
	SWITCHTEC_PCI_DEVICE(0x4084, SWITCHTEC_GEN4),  /* PFX 84XG4 */
	SWITCHTEC_PCI_DEVICE(0x4068, SWITCHTEC_GEN4),  /* PFX 68XG4 */
	/* [한국어] Gen4 첫 제품군의 작은 레인 수 모델들. */
	SWITCHTEC_PCI_DEVICE(0x4052, SWITCHTEC_GEN4),  /* PFX 52XG4 */
	SWITCHTEC_PCI_DEVICE(0x4036, SWITCHTEC_GEN4),  /* PFX 36XG4 */
	SWITCHTEC_PCI_DEVICE(0x4028, SWITCHTEC_GEN4),  /* PFX 28XG4 */
	/* [한국어] Gen4 두 번째 제품군. */
	SWITCHTEC_PCI_DEVICE(0x4100, SWITCHTEC_GEN4),  /* PSX 100XG4 */
	SWITCHTEC_PCI_DEVICE(0x4184, SWITCHTEC_GEN4),  /* PSX 84XG4 */
	SWITCHTEC_PCI_DEVICE(0x4168, SWITCHTEC_GEN4),  /* PSX 68XG4 */
	/* [한국어] 위 제품군의 작은 레인 수 모델들. */
	SWITCHTEC_PCI_DEVICE(0x4152, SWITCHTEC_GEN4),  /* PSX 52XG4 */
	SWITCHTEC_PCI_DEVICE(0x4136, SWITCHTEC_GEN4),  /* PSX 36XG4 */
	SWITCHTEC_PCI_DEVICE(0x4128, SWITCHTEC_GEN4),  /* PSX 28XG4 */
	/* [한국어] Gen4 세 번째 제품군. */
	SWITCHTEC_PCI_DEVICE(0x4200, SWITCHTEC_GEN4),  /* PAX 100XG4 */
	SWITCHTEC_PCI_DEVICE(0x4284, SWITCHTEC_GEN4),  /* PAX 84XG4 */
	SWITCHTEC_PCI_DEVICE(0x4268, SWITCHTEC_GEN4),  /* PAX 68XG4 */
	/* [한국어] 위 제품군의 작은 레인 수 모델들. */
	SWITCHTEC_PCI_DEVICE(0x4252, SWITCHTEC_GEN4),  /* PAX 52XG4 */
	SWITCHTEC_PCI_DEVICE(0x4236, SWITCHTEC_GEN4),  /* PAX 36XG4 */
	SWITCHTEC_PCI_DEVICE(0x4228, SWITCHTEC_GEN4),  /* PAX 28XG4 */
	/* [한국어] Gen4 파생 제품군 - 이 묶음은 세 모델뿐이다. */
	SWITCHTEC_PCI_DEVICE(0x4352, SWITCHTEC_GEN4),  /* PFXA 52XG4 */
	SWITCHTEC_PCI_DEVICE(0x4336, SWITCHTEC_GEN4),  /* PFXA 36XG4 */
	SWITCHTEC_PCI_DEVICE(0x4328, SWITCHTEC_GEN4),  /* PFXA 28XG4 */
	/* [한국어] Gen4 파생 제품군 두 번째. */
	SWITCHTEC_PCI_DEVICE(0x4452, SWITCHTEC_GEN4),  /* PSXA 52XG4 */
	SWITCHTEC_PCI_DEVICE(0x4436, SWITCHTEC_GEN4),  /* PSXA 36XG4 */
	SWITCHTEC_PCI_DEVICE(0x4428, SWITCHTEC_GEN4),  /* PSXA 28XG4 */
	/* [한국어] Gen4 파생 제품군 세 번째 - 여기까지가 Gen4 표시 항목이다. */
	SWITCHTEC_PCI_DEVICE(0x4552, SWITCHTEC_GEN4),  /* PAXA 52XG4 */
	SWITCHTEC_PCI_DEVICE(0x4536, SWITCHTEC_GEN4),  /* PAXA 36XG4 */
	SWITCHTEC_PCI_DEVICE(0x4528, SWITCHTEC_GEN4),  /* PAXA 28XG4 */
	/* [한국어] 여기서부터 SWITCHTEC_GEN5. 이 드라이버에서 GEN5 는 코드 경로상
	 * GEN4 와 같이 취급된다 - 판정이 모두 'gen >= SWITCHTEC_GEN4' 형태라
	 * GEN5 도 gen4 레지스터 레이아웃을 쓴다. */
	SWITCHTEC_PCI_DEVICE(0x5000, SWITCHTEC_GEN5),  /* PFX 100XG5 */
	SWITCHTEC_PCI_DEVICE(0x5084, SWITCHTEC_GEN5),  /* PFX 84XG5 */
	SWITCHTEC_PCI_DEVICE(0x5068, SWITCHTEC_GEN5),  /* PFX 68XG5 */
	/* [한국어] Gen5 첫 제품군의 작은 레인 수 모델들. */
	SWITCHTEC_PCI_DEVICE(0x5052, SWITCHTEC_GEN5),  /* PFX 52XG5 */
	SWITCHTEC_PCI_DEVICE(0x5036, SWITCHTEC_GEN5),  /* PFX 36XG5 */
	SWITCHTEC_PCI_DEVICE(0x5028, SWITCHTEC_GEN5),  /* PFX 28XG5 */
	/* [한국어] Gen5 두 번째 제품군. */
	SWITCHTEC_PCI_DEVICE(0x5100, SWITCHTEC_GEN5),  /* PSX 100XG5 */
	SWITCHTEC_PCI_DEVICE(0x5184, SWITCHTEC_GEN5),  /* PSX 84XG5 */
	SWITCHTEC_PCI_DEVICE(0x5168, SWITCHTEC_GEN5),  /* PSX 68XG5 */
	/* [한국어] 위 제품군의 작은 레인 수 모델들. */
	SWITCHTEC_PCI_DEVICE(0x5152, SWITCHTEC_GEN5),  /* PSX 52XG5 */
	SWITCHTEC_PCI_DEVICE(0x5136, SWITCHTEC_GEN5),  /* PSX 36XG5 */
	SWITCHTEC_PCI_DEVICE(0x5128, SWITCHTEC_GEN5),  /* PSX 28XG5 */
	/* [한국어] Gen5 세 번째 제품군. */
	SWITCHTEC_PCI_DEVICE(0x5200, SWITCHTEC_GEN5),  /* PAX 100XG5 */
	SWITCHTEC_PCI_DEVICE(0x5284, SWITCHTEC_GEN5),  /* PAX 84XG5 */
	SWITCHTEC_PCI_DEVICE(0x5268, SWITCHTEC_GEN5),  /* PAX 68XG5 */
	/* [한국어] 위 제품군의 작은 레인 수 모델들. */
	SWITCHTEC_PCI_DEVICE(0x5252, SWITCHTEC_GEN5),  /* PAX 52XG5 */
	SWITCHTEC_PCI_DEVICE(0x5236, SWITCHTEC_GEN5),  /* PAX 36XG5 */
	SWITCHTEC_PCI_DEVICE(0x5228, SWITCHTEC_GEN5),  /* PAX 28XG5 */
	/* [한국어] Gen5 파생 제품군 - Gen4 때와 달리 여섯 모델을 모두 갖췄다. */
	SWITCHTEC_PCI_DEVICE(0x5300, SWITCHTEC_GEN5),  /* PFXA 100XG5 */
	SWITCHTEC_PCI_DEVICE(0x5384, SWITCHTEC_GEN5),  /* PFXA 84XG5 */
	SWITCHTEC_PCI_DEVICE(0x5368, SWITCHTEC_GEN5),  /* PFXA 68XG5 */
	/* [한국어] 위 제품군의 작은 레인 수 모델들. */
	SWITCHTEC_PCI_DEVICE(0x5352, SWITCHTEC_GEN5),  /* PFXA 52XG5 */
	SWITCHTEC_PCI_DEVICE(0x5336, SWITCHTEC_GEN5),  /* PFXA 36XG5 */
	SWITCHTEC_PCI_DEVICE(0x5328, SWITCHTEC_GEN5),  /* PFXA 28XG5 */
	/* [한국어] Gen5 파생 제품군 두 번째. */
	SWITCHTEC_PCI_DEVICE(0x5400, SWITCHTEC_GEN5),  /* PSXA 100XG5 */
	SWITCHTEC_PCI_DEVICE(0x5484, SWITCHTEC_GEN5),  /* PSXA 84XG5 */
	SWITCHTEC_PCI_DEVICE(0x5468, SWITCHTEC_GEN5),  /* PSXA 68XG5 */
	/* [한국어] 위 제품군의 작은 레인 수 모델들. */
	SWITCHTEC_PCI_DEVICE(0x5452, SWITCHTEC_GEN5),  /* PSXA 52XG5 */
	SWITCHTEC_PCI_DEVICE(0x5436, SWITCHTEC_GEN5),  /* PSXA 36XG5 */
	SWITCHTEC_PCI_DEVICE(0x5428, SWITCHTEC_GEN5),  /* PSXA 28XG5 */
	/* [한국어] Gen5 파생 제품군 세 번째 - Microsemi 벤더 ID 목록의 마지막이다. */
	SWITCHTEC_PCI_DEVICE(0x5500, SWITCHTEC_GEN5),  /* PAXA 100XG5 */
	SWITCHTEC_PCI_DEVICE(0x5584, SWITCHTEC_GEN5),  /* PAXA 84XG5 */
	SWITCHTEC_PCI_DEVICE(0x5568, SWITCHTEC_GEN5),  /* PAXA 68XG5 */
	/* [한국어] Gen5 목록의 끝. */
	SWITCHTEC_PCI_DEVICE(0x5552, SWITCHTEC_GEN5),  /* PAXA 52XG5 */
	SWITCHTEC_PCI_DEVICE(0x5536, SWITCHTEC_GEN5),  /* PAXA 36XG5 */
	SWITCHTEC_PCI_DEVICE(0x5528, SWITCHTEC_GEN5),  /* PAXA 28XG5 */
	/* [한국어] 여기부터는 매크로가 SWITCHTEC_PCI100X_DEVICE 로 바뀐다 - 벤더 ID 가
	 * PCI_VENDOR_ID_EFAR(0x1055) 인 소형 스위치들이며, 세대는 GEN4 다. */
	SWITCHTEC_PCI100X_DEVICE(0x1001, SWITCHTEC_GEN4),  /* PCI1001 16XG4 */
	SWITCHTEC_PCI100X_DEVICE(0x1002, SWITCHTEC_GEN4),  /* PCI1002 12XG4 */
	SWITCHTEC_PCI100X_DEVICE(0x1003, SWITCHTEC_GEN4),  /* PCI1003 16XG4 */
	/* [한국어] PCI100X 계열의 나머지. 16/12 레인급 소형 팬아웃 스위치다. */
	SWITCHTEC_PCI100X_DEVICE(0x1004, SWITCHTEC_GEN4),  /* PCI1004 16XG4 */
	SWITCHTEC_PCI100X_DEVICE(0x1005, SWITCHTEC_GEN4),  /* PCI1005 16XG4 */
	SWITCHTEC_PCI100X_DEVICE(0x1006, SWITCHTEC_GEN4),  /* PCI1006 16XG4 */
	/* [한국어] 표의 끝을 알리는 빈 항목(sentinel). PCI 코어는 vendor 가 0 인 항목을
	 * 만나면 순회를 멈추므로, 이 줄이 없으면 매칭 루프가 배열 밖으로 나간다. */
	{0}
};
/* [한국어] 빌드 시 이 표를 모듈 별칭(MODULE_ALIAS)으로 뽑아 준다. 그래야
 * 부팅 중 커널이 장치를 열거했을 때 udev 가 switchtec 모듈을 자동
 * 적재할 수 있다. 표 자체를 런타임에 쓰는 것과는 별개의 용도다. */
MODULE_DEVICE_TABLE(pci, switchtec_pci_tbl);

/* [한국어] switchtec_pci_driver - PCI 버스에 등록할 드라이버 서술자.
 * 아래 네 필드만 채운다 - 이 드라이버는 suspend/resume 나 error handler
 * 콜백을 제공하지 않는다(전원 관리는 PCI 코어의 기본 동작에 맡긴다). */
static struct pci_driver switchtec_pci_driver = {
	/* [한국어] 드라이버 이름. KBUILD_MODNAME 은 빌드 시스템이 넣어 주는 모듈 이름
	 * 문자열("switchtec")이며, devm_request_irq 의 IRQ 이름과
	 * devm_request_mem_region 의 영역 이름으로도 같은 값을 쓴다 -
	 * /proc/interrupts 와 /proc/iomem 에서 같은 이름으로 보이게 하려는 것. */
	.name		= KBUILD_MODNAME,
	/* [한국어] 위에서 만든 장치 ID 표. PCI 코어가 열거한 장치마다 이 표를 훑어
	 * 일치하는 항목을 찾으면 probe 를 호출하고, 그때 일치한 항목의 포인터를
	 * probe 의 id 인자로 넘겨 준다 - driver_data(세대)를 그렇게 전달받는다. */
	.id_table	= switchtec_pci_tbl,
	/* [한국어] 장치가 나타났을 때 호출. 문자 디바이스 등록까지 여기서 끝낸다. */
	.probe		= switchtec_pci_probe,
	/* [한국어] 장치가 사라질 때 호출. probe 의 역순으로 정리한다. */
	.remove		= switchtec_pci_remove,
};

/* [한국어] switchtec_init - 모듈 적재 진입점
 * 
 * @return: 0 성공. 실패 시 음수 errno 를 그대로 modprobe 에 돌려준다.
 * 
 * 왜 필요한가: 이 드라이버는 장치마다 /dev/switchtecN 문자 디바이스를
 * 하나씩 만든다. 문자 디바이스를 만들려면 (a) 주 번호(major)와 부 번호
 * 범위를 미리 예약해 두어야 하고, (b) udev 가 노드를 만들 수 있도록
 * 장치 클래스가 등록되어 있어야 한다. 이 둘은 장치별이 아니라 모듈
 * 전체에 한 번만 필요하므로 여기서 처리한 뒤 PCI 드라이버를 등록한다.
 * 
 * 동작 단계:
 *   1) alloc_chrdev_region 으로 max_devices 개 분량의 (major, minor) 를 예약
 *   2) class_register 로 /sys/class/switchtec 를 만든다
 *   3) pci_register_driver 로 PCI 버스에 드라이버를 붙인다 - 이 호출 안에서
 *      이미 열거된 장치들에 대해 probe 가 동기적으로 불릴 수 있다.
 *      그래서 1)2) 를 반드시 먼저 끝내 두어야 한다.
 * 
 * 실행 컨텍스트: 프로세스 문맥(insmod/modprobe). 모듈당 한 번만 실행되므로
 * 재진입이나 동시성 고려는 없다.
 * 
 * 에러 경로: 뒤 단계가 실패하면 goto 로 앞 단계를 역순으로 되돌린다.
 * 
 * 호출 체인:
 *   module_init(switchtec_init) -> [switchtec_init]
 *     -> alloc_chrdev_region() / class_register() / pci_register_driver()
 *     -> (PCI 코어) -> switchtec_pci_probe() */
static int __init switchtec_init(void)
{
	/* [한국어] 반환값 보관용. goto 로 빠져나갈 때 실패 사유를 유지해야 한다. */
	int rc;

	/* [한국어] 문자 디바이스 번호 공간을 통째로 예약한다. 첫 부 번호 0 부터
	 * max_devices(모듈 파라미터, 기본 16) 개를 잡고, 할당된 dev_t 를
	 * 전역 switchtec_devt 에 적어 둔다. stdev_create() 가 여기서 얻은
	 * major 와 ida 로 뽑은 minor 를 MKDEV 로 합쳐 실제 노드 번호를 만든다.
	 * 이름 "switchtec" 은 /proc/devices 에 나타난다. */
	rc = alloc_chrdev_region(&switchtec_devt, 0, max_devices,
				 "switchtec");
	/* [한국어] 번호 공간을 못 잡았으면 되돌릴 것이 없으니 그대로 반환한다. */
	if (rc)
		return rc;

	/* [한국어] /sys/class/switchtec 를 만든다. 장치의 dev->class 를 이 클래스로
	 * 설정해 두면 udev 가 클래스 규칙에 따라 /dev/switchtecN 노드를 생성한다.
	 * switchtec_class 는 EXPORT_SYMBOL_GPL 로 내보내져 있는데, 이 부분
	 * 체크아웃 안에서 그 심볼을 참조하는 다른 파일은 없다 - 외부 참조자는
	 * NTB 쪽 드라이버로 보이나 이 트리에 없어 확인하지 못했다. */
	rc = class_register(&switchtec_class);
	/* [한국어] 클래스 등록 실패 - 앞서 잡은 문자 디바이스 번호를 반납해야 한다. */
	if (rc)
		goto err_create_class;

	/* [한국어] PCI 버스에 드라이버를 등록한다. 이 호출은 이미 열거되어 있는 모든
	 * 장치에 대해 id_table 매칭을 돌리므로, 반환 전에 probe 가 여러 번
	 * 불릴 수 있다. 곧 probe 안에서 쓰이는 switchtec_devt 와 클래스가
	 * 이 시점에 이미 준비되어 있어야 하는 이유다. */
	rc = pci_register_driver(&switchtec_pci_driver);
	/* [한국어] 등록 실패 - 클래스와 번호 공간을 모두 되돌린다. */
	if (rc)
		goto err_pci_register;

	/* [한국어] 적재 성공을 커널 로그에 남긴다. 아직 장치가 없어도 출력된다. */
	pr_info(KBUILD_MODNAME ": loaded.\n");

	/* [한국어] 여기까지 오면 세 자원이 모두 살아 있다 - 성공. */
	return 0;

/* [한국어] pci_register_driver 실패 지점. 아래 라벨로 계속 흘러 내려가
 * 클래스와 번호 공간을 차례로 반납한다. */
err_pci_register:
	/* [한국어] 2) 단계 되돌리기 - /sys/class/switchtec 제거. */
	class_unregister(&switchtec_class);

/* [한국어] class_register 실패 지점(또는 위에서 흘러 내려온 경우). */
err_create_class:
	/* [한국어] 1) 단계 되돌리기 - 예약했던 (major, minor) 범위 반납. */
	unregister_chrdev_region(switchtec_devt, max_devices);

	/* [한국어] 저장해 둔 실패 사유를 modprobe 에 전한다. */
	return rc;
}
/* [한국어] 이 함수를 모듈 진입점으로 등록한다. 내장 빌드(=y)일 때는
 * 부팅 중 initcall 로 불린다. */
module_init(switchtec_init);

/* [한국어] switchtec_exit - 모듈 해제 진입점
 * 
 * @return: 없음
 * 
 * switchtec_init 이 잡은 자원을 정확히 역순으로 반납한다. 순서가
 * 중요하다 - pci_unregister_driver 가 먼저 돌아야 남아 있던 장치들의
 * remove 가 모두 끝나고, 그래야 클래스와 번호 공간을 안전하게 지울 수
 * 있다. 반대로 하면 아직 살아 있는 장치가 이미 사라진 클래스를 참조한다.
 * 
 * 실행 컨텍스트: 프로세스 문맥(rmmod). 모듈 참조 카운트가 0 이어야
 * 호출되므로 열려 있는 /dev/switchtecN 이 남아 있는 상태로는 오지 않는다.
 * 
 * 호출 체인:
 *   module_exit(switchtec_exit) -> [switchtec_exit]
 *     -> pci_unregister_driver() -> switchtec_pci_remove() (장치마다) */
static void __exit switchtec_exit(void)
{
	/* [한국어] 드라이버를 버스에서 뗀다. 이 안에서 바인딩된 장치마다
	 * switchtec_pci_remove() 가 동기적으로 호출되어 문자 디바이스가
	 * 지워지고 minor 가 ida 로 반납된다. 이 호출이 끝나야 아래 정리가
	 * 안전해진다. */
	pci_unregister_driver(&switchtec_pci_driver);
	/* [한국어] /sys/class/switchtec 제거. */
	class_unregister(&switchtec_class);
	/* [한국어] 예약했던 문자 디바이스 번호 범위 반납. */
	unregister_chrdev_region(switchtec_devt, max_devices);
	/* [한국어] minor 번호 할당기의 내부 자료구조 해제. 위에서 모든 장치가
	 * 반납을 마쳤으므로 이 시점에 ida 는 비어 있어야 한다. */
	ida_destroy(&switchtec_minor_ida);

	/* [한국어] 해제 완료를 로그에 남긴다. */
	pr_info(KBUILD_MODNAME ": unloaded.\n");
}
/* [한국어] 이 함수를 모듈 해제 진입점으로 등록한다. */
module_exit(switchtec_exit);
