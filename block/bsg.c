// SPDX-License-Identifier: GPL-2.0
/*
 * [한국어 설명] block 계층의 SCSI Generic v4(sg v4) 인터페이스 구현 (bsg.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 /dev/bsg/<name> 형태의 캐릭터 디바이스 노드를 통해 사용자 공간이
 * 임의의 SCSI/전달용(passthrough) 명령을 블록 계층의 request_queue로 직접
 * 전달할 수 있게 해주는 bsg(Block SCSI Generic) 드라이버를 구현한다.
 * 원래 sg 드라이버(drivers/scsi/sg.c)는 SCSI 호스트 어댑터 전용이었지만,
 * bsg는 request_queue만 있으면 SCSI뿐 아니라 NVMe 같은 비-SCSI 블록
 * 디바이스에도 동일한 passthrough 경로를 제공하기 위해 만들어졌다.
 * struct bsg_device가 하나의 /dev/bsg/* 노드를 표현하며, 실제 명령 변환과
 * 큐 제출은 이 파일이 아니라 하위 드라이버가 등록한 콜백(sg_io_fn,
 * uring_cmd_fn)이 담당한다 — 즉 bsg.c 자체는 "게이트웨이"이지 명령을
 * 해석하는 주체가 아니다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계층 구조상 bsg.c는 VFS(문자 장치 파일 연산)와 blk-mq 기반
 * request_queue 사이의 얇은 어댑터 계층이다.
 * 등록 경로: 하위 드라이버(예: drivers/scsi/scsi_transport_*.c,
 *   drivers/nvme/host/ioctl.c)가 자신의 request_queue를 만든 뒤
 *   bsg_register_queue()를 호출하여 /dev/bsg/<name> 노드를 생성하고
 *   sg_io_fn/uring_cmd_fn 콜백을 등록한다.
 * 사용 경로 1 (전통적 ioctl):
 *   userspace ioctl(fd, SG_IO, &sg_io_v4)
 *   -> bsg_ioctl() -> bsg_sg_io() -> bd->sg_io_fn(q, hdr, ...)
 *   -> (하위 드라이버가 명령을 구성해 request_queue에 제출)
 * 사용 경로 2 (io_uring passthrough, 최신 고성능 경로):
 *   userspace io_uring_enter(IORING_OP_URING_CMD)
 *   -> bsg_uring_cmd() -> bd->uring_cmd_fn(q, ioucmd, ...)
 * 실행 컨텍스트: 두 경로 모두 이 파일의 함수들은 호출한 사용자 프로세스의
 * 시스템 콜 컨텍스트(프로세스 컨텍스트)에서 동기적으로 실행되며, 인터럽트나
 * softirq 컨텍스트에서는 실행되지 않는다. 모듈 초기화 함수(bsg_init)만
 * 부팅/모듈 로드 시 device_initcall 컨텍스트에서 1회 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 하위 모듈: block/blk-core.c의 blk_get_queue()/blk_put_queue()
 *   (request_queue 참조 카운트), driver core(device/cdev/class 등록),
 *   io_uring/cmd.h의 struct io_uring_cmd(uring passthrough 프레임워크).
 * 이 파일에 의존하는 상위 모듈: SCSI 전달 클래스(scsi_transport_sas 등)와
 *   NVMe 드라이버가 bsg_register_queue()/bsg_unregister_queue()를 호출해
 *   자신의 request_queue를 /dev/bsg/*로 노출한다. 이들은 각자의 sg_io_fn/
 *   uring_cmd_fn을 등록해 sg_io_v4 <-> 자신의 명령 포맷(SCSI CDB, NVMe
 *   커맨드 등) 변환을 책임진다.
 * 데이터 흐름: 사용자 공간의 struct sg_io_v4(명령/데이터/센스 버퍼 포인터
 *   묶음)가 copy_from_user()로 커널에 들어오고, bd->sg_io_fn이 이를 해석해
 *   request_queue에 request를 만들어 제출한 뒤, 완료된 결과(상태/센스 데이터
 *   등)가 다시 hdr에 채워져 copy_to_user()로 사용자 공간에 돌아간다.
 * 핵심 공유 자료구조: struct bsg_device(이 파일 소유, 하위 드라이버는
 *   opaque 포인터로만 다룸), struct sg_io_v4(include/uapi/scsi/sg.h 정의,
 *   sg v3와 호환되지 않는 v4 전용 ABI), struct io_uring_cmd(io_uring 서브
 *   시스템 소유, bsg는 콜백만 등록).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct bsg_device: /dev/bsg/* 노드 하나를 표현하는 핵심 구조체.
 *   request_queue 포인터, 문자 장치/디바이스 등록 정보, 타임아웃/버퍼
 *   상한 설정값, 하위 드라이버 콜백 두 개를 담는다.
 * - bsg_register_queue()/bsg_unregister_queue(): 하위 드라이버가 호출하는
 *   공개 등록/해제 API. EXPORT_SYMBOL_GPL로 노출된다.
 * - bsg_open()/bsg_release(): open(2)/close(2) 시 request_queue 참조
 *   카운트를 증감시켜 fd가 열려 있는 동안 큐가 사라지지 않도록 보장.
 * - bsg_ioctl(): SG_IO를 포함한 전통적 sg ioctl 전체를 처리하는 디스패처.
 * - bsg_sg_io(): SG_IO ioctl의 실제 처리 — 사용자 헤더 복사, guard 검증,
 *   하위 드라이버 콜백 호출, 결과 헤더 복사.
 * - bsg_uring_cmd()/bsg_check_uring_features(): io_uring passthrough
 *   진입점과 SQE128/CQE32(빅 SQE/CQE) 기능 요구 검증.
 * - bsg_init(): 모듈/서브시스템 초기화 — bsg_class 등록과 chrdev major
 *   번호 할당.
 */

/*
 * bsg.c - block layer implementation of the sg v4 interface
 */
#include <linux/module.h>
/* [한국어] THIS_MODULE, MODULE_AUTHOR/DESCRIPTION/LICENSE 등 모듈 메타데이터 매크로 제공 - bsg가 로드 가능한 커널 모듈로 동작하기 위해 필요 */
#include <linux/init.h>
/* [한국어] __init 매크로와 device_initcall() 제공 - bsg_init()을 부팅/모듈 로드 시 1회만 실행되는 초기화 코드로 표시 */
#include <linux/file.h>
/* [한국어] struct file, file_inode() 등 파일 디스크립터 관련 헬퍼 제공 - bsg_ioctl/bsg_uring_cmd에서 struct file로부터 inode를 얻는 데 사용 */
#include <linux/blkdev.h>
/* [한국어] struct request_queue, blk_get_queue()/blk_put_queue(), queue_max_bytes() 등 블록 계층 핵심 API 제공 - bsg가 request_queue를 참조·조회하는 데 필요 */
#include <linux/cdev.h>
/* [한국어] struct cdev, cdev_init()/cdev_device_add()/cdev_device_del() 제공 - /dev/bsg/* 캐릭터 디바이스 등록/해제에 사용 */
#include <linux/jiffies.h>
/* [한국어] msecs_to_jiffies(), clock_t_to_jiffies(), jiffies_to_clock_t() 제공 - 사용자 공간의 ms/clock_t 단위 타임아웃을 커널 내부 jiffies로 변환 */
#include <linux/percpu.h>
/* [한국어] 퍼-CPU 변수 인프라 제공 - 이 파일에서 직접 퍼-CPU 변수를 선언하진 않지만 blkdev.h 등 하위 헤더가 의존 */
#include <linux/idr.h>
/* [한국어] IDA(ID Allocator) API(DEFINE_IDA, ida_alloc_max(), ida_free()) 제공 - /dev/bsg/* minor 번호를 충돌 없이 할당·회수하는 데 사용 */
#include <linux/bsg.h>
/* [한국어] bsg_sg_io_fn/bsg_uring_cmd_fn 콜백 타입과 bsg_register_queue()/bsg_unregister_queue() 공개 API 선언 - 하위 드라이버와의 계약(contract) 헤더 */
#include <linux/slab.h>
/* [한국어] kzalloc_obj(), kfree() 등 커널 힙 할당자 제공 - struct bsg_device 동적 할당/해제에 사용 */
#include <linux/io_uring/cmd.h>
/* [한국어] struct io_uring_cmd, IO_URING_F_SQE128/IO_URING_F_CQE32 플래그 정의 제공 - io_uring passthrough 경로(bsg_uring_cmd)의 기반 */

#include <scsi/scsi.h>
/* [한국어] SCSI 공통 상수/타입 정의 - sg v4 ioctl이 SCSI 계열 규약을 다수 재사용하므로 포함 */
#include <scsi/scsi_ioctl.h>
/* [한국어] SCSI_IOCTL_GET_IDLUN, SCSI_IOCTL_GET_BUS_NUMBER, SCSI_IOCTL_SEND_COMMAND 등 레거시 SCSI ioctl 번호 정의 - bsg_ioctl의 SCSI 호환 분기에서 사용 */
#include <scsi/sg.h>
/* [한국어] struct sg_io_v4, SG_IO 및 SG_GET_xxx / SG_SET_xxx 계열 ioctl 번호, SG_DEFAULT_TIMEOUT 관련 정의 제공 - bsg가 구현하는 sg v4 ABI의 핵심 헤더 */

#define BSG_DESCRIPTION	"Block layer SCSI generic (bsg) driver"
/* [한국어] 모듈 로드 시 printk와 MODULE_DESCRIPTION에 쓰이는 사람이 읽는 설명 문자열 - 코드 동작에는 영향 없는 메타데이터 */
#define BSG_VERSION	"0.4"
/* [한국어] 부팅 로그에 출력되는 bsg 드라이버 버전 문자열 - ABI 버전이 아니라 드라이버 구현 버전 표기용 */

/*
 * [한국어]
 * struct bsg_device - /dev/bsg/<name> 노드 하나에 대응하는 인스턴스 구조체
 *
 * bsg_register_queue()가 이 구조체를 kzalloc_obj()로 할당해 초기화하며,
 * struct device의 참조 카운트가 0이 되는 시점(bsg_device_release)에
 * kfree()로 해제된다. 즉 이 구조체의 수명은 struct device(embedded)의
 * refcount에 종속되어 있어, open(2) 중인 fd가 있으면(blk_get_queue로
 * request_queue 참조를 잡고 있어도) device 자체의 refcount와는 별개로
 * 관리된다는 점에 유의해야 한다 — bsg_device 메모리 해제와 request_queue
 * 생존은 서로 다른 카운터로 보호된다.
 */
struct bsg_device {
	struct request_queue *queue;
	/* 이 bsg 노드가 노출하는 대상 request_queue.
	 * 설정자: bsg_register_queue()에서 인자로 받은 q를 그대로 저장.
	 * 읽는 자: bsg_open/bsg_release(참조 카운트 증감), bsg_ioctl(queue_max_bytes
	 *   조회), bsg_sg_io/bsg_uring_cmd(하위 드라이버 콜백에 전달).
	 * 값 범위: 유효한 request_queue 포인터. bsg_unregister_queue() 호출 후에도
	 *   이 필드 자체는 남아있지만, 그 시점 이후 이 큐로의 신규 제출은 상위
	 *   드라이버(NVMe/SCSI) 쪽에서 이미 차단되어 있어야 한다.
	 * 동기화: bsg 계층 자체는 이 포인터에 대한 락을 두지 않으며, request_queue의
	 *   생존은 blk_get_queue/blk_put_queue의 refcount로만 보장된다. */

	struct device device;
	/* /dev/bsg/ 아래에서 sysfs 노드와 디바이스 모델 참조 카운트를 담당하는
	 *   embedded struct device.
	 * 설정자: bsg_register_queue()가 device_initialize()/dev_set_name()/
	 *   cdev_device_add()로 초기화 및 sysfs 등록을 수행.
	 * 읽는 자: bsg_unregister_queue()가 cdev_device_del()/put_device()로
	 *   해제를 트리거하고, bsg_device_release()가 이 device의 refcount가
	 *   0이 되었을 때 콜백으로 호출되어 bsg_device 자체를 kfree한다.
	 * 값 범위: container_of(dev, struct bsg_device, device) 패턴으로 항상 이
	 *   구조체의 시작 주소를 역산할 수 있도록 struct bsg_device 내부에
	 *   포함(embedding)되어 있다.
	 * 동기화: 디바이스 모델의 kobject refcount(kref)가 동시 접근을 보호하며,
	 *   bsg 계층은 별도의 락을 추가하지 않는다. */

	struct cdev cdev;
	/* /dev/bsg/<name> 문자 장치(character device) 등록 정보 - VFS의
	 *   file_operations 테이블(bsg_fops)과 이 노드를 연결하는 커널 객체.
	 * 설정자: bsg_register_queue()의 cdev_init()/cdev_device_add()에서
	 *   초기화 및 커널에 등록.
	 * 읽는 자: to_bsg_device()가 open된 inode->i_cdev로부터 container_of로
	 *   이 필드를 거쳐 상위 bsg_device를 복원.
	 * 값 범위: cdev_device_add() 성공 후에만 유효한 커널 등록 상태를 가진다.
	 * 동기화: cdev 자체의 등록/해제는 device 구조체와 함께 cdev_device_add/del
	 *   API가 원자적으로 처리하므로 bsg 계층에서 추가 락이 필요 없다. */

	int max_queue;
	/* 사용자 공간에 보고되는 "최대 동시 명령 개수" 힌트 값 - 실제 큐 깊이를
	 *   강제하는 값이 아니라 SG_GET_COMMAND_Q/SG_SET_COMMAND_Q ioctl을 통해
	 *   레거시 sg 애플리케이션과의 호환을 위해 유지되는 메타데이터다.
	 * 설정자: bsg_register_queue()에서 BSG_DEFAULT_CMDS(64)로 초기화되고,
	 *   이후 bsg_set_command_q()가 SG_SET_COMMAND_Q ioctl 처리 중 갱신.
	 * 읽는 자: bsg_get_command_q()가 SG_GET_COMMAND_Q ioctl 응답으로 반환.
	 * 값 범위: 1 이상의 정수(0 이하는 bsg_set_command_q에서 -EINVAL로 거부).
	 * 동기화: READ_ONCE/WRITE_ONCE로 접근하여 단일 워드 읽기/쓰기의 데이터
	 *   레이스(data race)를 컴파일러 차원에서 방지하지만, 값 자체의 원자적
	 *   read-modify-write는 보장하지 않는다(동시 SET 호출 간 갱신 순서는
	 *   보장되지 않음). */

	unsigned int timeout;
	/* 이 bsg 노드에 설정된 기본 명령 타임아웃(jiffies 단위).
	 * 설정자: 초기값은 0(bsg_register_queue에서 명시적으로 대입하지 않아
	 *   kzalloc_obj의 zero-fill로 0이 됨), 이후 bsg_ioctl의 SG_SET_TIMEOUT
	 *   케이스가 clock_t_to_jiffies()로 변환한 값을 대입.
	 * 읽는 자: bsg_timeout()이 사용자가 SG_IO 헤더에 개별 timeout을 지정하지
	 *   않은 경우의 폴백 값으로 사용, bsg_ioctl의 SG_GET_TIMEOUT 케이스가
	 *   jiffies_to_clock_t()로 역변환하여 반환.
	 * 값 범위: 0이면 "미설정"을 의미하며 이 경우 bsg_timeout()이
	 *   BLK_DEFAULT_SG_TIMEOUT을 사용. 0이 아니면 jiffies 단위 값.
	 * 동기화: 별도 락 없이 직접 대입/읽기 - 각 open된 fd는 보통 단일
	 *   애플리케이션이 순차적으로 ioctl을 호출하므로 실질적 경쟁이 드물지만,
	 *   커널 차원의 강제 직렬화는 없다(추정). */

	unsigned int reserved_size;
	/* SG_IO 등에서 허용되는 데이터 버퍼 크기의 상한(바이트 단위) - 실제
	 *   상한은 이 값과 queue_max_bytes(q) 중 작은 쪽으로 클리핑된다.
	 * 설정자: bsg_register_queue()에서 INT_MAX로 초기화(사실상 무제한),
	 *   이후 bsg_ioctl의 SG_SET_RESERVED_SIZE 케이스가 min_t()로
	 *   queue_max_bytes(q)를 넘지 않도록 클램핑하여 갱신.
	 * 읽는 자: bsg_ioctl의 SG_GET_RESERVED_SIZE 케이스가 min(reserved_size,
	 *   queue_max_bytes(q))를 사용자 공간에 반환.
	 * 값 범위: 0 이상(음수는 SG_SET_RESERVED_SIZE에서 -EINVAL로 거부),
	 *   상한은 queue_max_bytes(q)로 재클리핑됨.
	 * 동기화: timeout과 동일하게 별도 락 없이 접근. */

	bsg_sg_io_fn *sg_io_fn;
	/* 하위 드라이버(SCSI 전달 클래스, NVMe 등)가 등록한 SG_IO 처리 콜백
	 *   함수 포인터 - sg_io_v4를 실제 명령으로 변환해 request_queue에
	 *   제출하는 책임을 진다.
	 * 설정자: bsg_register_queue()의 인자로 전달되어 그대로 저장.
	 * 읽는 자: bsg_sg_io()가 SG_IO ioctl 처리 중 이 함수 포인터를 호출.
	 * 값 범위: 유효한 함수 포인터(NULL 불가 - bsg_register_queue를 호출하는
	 *   드라이버가 반드시 제공해야 하는 필수 콜백으로 간주됨, 추정).
	 * 동기화: 등록 이후 값이 바뀌지 않는 불변(immutable) 필드이므로 락 불필요. */

	bsg_uring_cmd_fn *uring_cmd_fn;
	/* 하위 드라이버가 등록한 io_uring passthrough 처리 콜백 - io_uring
	 *   SQE(Submission Queue Entry)로 전달된 명령을 처리한다.
	 * 설정자: bsg_register_queue()의 인자로 전달되어 그대로 저장.
	 * 읽는 자: bsg_uring_cmd()가 io_uring_cmd 처리 중 NULL 여부를 검사한
	 *   뒤 호출.
	 * 값 범위: NULL 가능 - uring_cmd_fn을 지원하지 않는 드라이버는 NULL을
	 *   넘길 수 있으며, 이 경우 bsg_uring_cmd()가 -EOPNOTSUPP을 반환한다.
	 * 동기화: sg_io_fn과 동일하게 등록 후 불변. */
};

/*
 * [한국어]
 * to_bsg_device - inode로부터 상위 struct bsg_device를 역산하는 헬퍼
 *
 * @inode: /dev/bsg/<name> 노드를 open()할 때 VFS가 전달하는 inode. 이 inode의
 *         i_cdev 필드가 bsg_register_queue()에서 cdev_device_add()로 등록해둔
 *         struct cdev를 가리킨다.
 * @return: inode->i_cdev가 embedded된 struct bsg_device의 시작 주소.
 *
 * bsg는 struct cdev를 struct bsg_device 안에 직접 embedding하는 방식을 쓰기
 * 때문에(별도로 cdev만 동적 할당하지 않음), 커널 전역 관례인 container_of()
 * 매크로로 "필드 주소 - 필드 오프셋"을 계산해 상위 구조체 포인터를 복원한다.
 * 이 함수는 static inline이므로 각 호출 지점에 인라인 전개되어 함수 호출
 * 오버헤드가 없다.
 * 실행 컨텍스트: 호출자(open/ioctl/uring_cmd 경로)와 동일한 프로세스
 * 컨텍스트에서 실행되며 별도의 동기화가 필요 없는 순수 포인터 연산이다.
 *
 * 호출 체인:
 *   bsg_open/bsg_release/bsg_ioctl/bsg_uring_cmd → [to_bsg_device] → container_of
 */
static inline struct bsg_device *to_bsg_device(struct inode *inode)
{
	/* cdev를 포함하는 bsg_device의 시작 주소를 구한다 */
	return container_of(inode->i_cdev, struct bsg_device, cdev);
	/* [한국어] inode->i_cdev 포인터에서 struct bsg_device 내 cdev 필드의 오프셋을 빼서 struct bsg_device* 시작 주소를 계산 - 별도 자료구조 탐색 없이 O(1) 포인터 산술로 상위 구조체 복원 */
}

#define BSG_DEFAULT_CMDS	64
/* [한국어] bsg_register_queue()가 max_queue 초기값으로 사용하는 기본 동시 명령 힌트 - 실제 request_queue의 하드웨어 큐 깊이와는 무관한 sg 계층의 레거시 호환용 숫자 */
#define BSG_MAX_DEVS		(1 << MINORBITS)
/* [한국어] 문자 장치 minor 번호 비트 폭(MINORBITS)으로 계산한 /dev/bsg/* 최대 등록 가능 개수 - alloc_chrdev_region()에 그대로 전달되어 chrdev 영역 크기를 결정 */

static DEFINE_IDA(bsg_minor_ida);
/* [한국어] /dev/bsg/* minor 번호를 충돌 없이 순차 할당/회수하기 위한 IDA(ID Allocator) 인스턴스 - bsg_register_queue에서 ida_alloc_max, bsg_device_release에서 ida_free 호출 */
static const struct class bsg_class;
/* [한국어] 아래에서 완전히 정의될 bsg 클래스의 전방 선언 - bsg_register_queue()가 정의보다 앞서 이 심볼의 주소를 참조하기 위해 필요 */
static int bsg_major;
/* [한국어] bsg_init()에서 alloc_chrdev_region()으로 할당받아 저장하는 문자 장치 major 번호 - 이후 모든 bsg_register_queue() 호출에서 MKDEV(bsg_major, minor)로 devt를 구성하는 데 사용되는 전역 상태 */

/*
 * [한국어]
 * bsg_timeout - SG_IO 요청에 적용할 최종 타임아웃(jiffies)을 계산
 *
 * @bd: 현재 open된 bsg 노드. bd->timeout(SG_SET_TIMEOUT으로 설정된 노드별
 *      기본값)을 폴백으로 사용하기 위해 전달됨.
 * @hdr: 사용자 공간에서 복사되어 온 sg_io_v4 헤더. hdr->timeout(밀리초 단위,
 *       0이면 "지정 안 함")을 최우선으로 사용.
 * @return: 실제로 하위 드라이버에 전달될 타임아웃 값(jiffies 단위, 항상
 *          BLK_MIN_SG_TIMEOUT 이상으로 보정됨).
 *
 * 우선순위는 "요청별 지정값 > 노드별 기본값 > 전역 기본값" 순이다. 사용자가
 * 요청마다 다른 타임아웃을 줄 수 있게 하면서도, 지정하지 않았을 때 너무
 * 작은 값(0 등)으로 인해 하위 드라이버가 즉시 타임아웃 처리를 해버리는 것을
 * 막기 위해 max_t()로 최소 보장값을 강제한다.
 * 실행 컨텍스트: bsg_sg_io() 호출 중 동일한 프로세스 컨텍스트에서 실행되는
 * 순수 계산 함수로, 부작용(side effect)이 없고 락도 필요 없다.
 *
 * 호출 체인:
 *   bsg_sg_io → [bsg_timeout] → (max_t/msecs_to_jiffies 등 인라인 헬퍼)
 */
static unsigned int bsg_timeout(struct bsg_device *bd, struct sg_io_v4 *hdr)
{
	unsigned int timeout = BLK_DEFAULT_SG_TIMEOUT;
	/* [한국어] 아무 것도 지정되지 않았을 때 쓸 전역 기본 타임아웃으로 초기화 - scsi/sg.h가 정의하는 sg 서브시스템 공통 기본값 */

	if (hdr->timeout)
		/* [한국어] 사용자가 SG_IO 헤더에 0이 아닌 timeout(ms)을 명시했는지 확인 - 요청 단위 우선순위가 가장 높음 */
		timeout = msecs_to_jiffies(hdr->timeout);
		/* [한국어] 밀리초 단위 사용자 값을 커널 내부 시간 단위인 jiffies로 변환 - request timeout 필드/타이머 만료 계산은 모두 jiffies 기준이므로 단위 통일 필요 */
	else if (bd->timeout)
		/* [한국어] 요청별 지정이 없으면 이 bsg 노드에 SG_SET_TIMEOUT으로 설정된 노드별 기본값이 있는지 확인 */
		timeout = bd->timeout;
		/* [한국어] 노드별 기본 타임아웃(이미 jiffies 단위로 저장되어 있음)을 그대로 사용 */

	return max_t(unsigned int, timeout, BLK_MIN_SG_TIMEOUT);
	/* [한국어] 계산된 timeout이 지나치게 작아(심지어 0이어도) 하위 드라이버가 즉시 명령을 만료 처리하지 않도록 최소값 BLK_MIN_SG_TIMEOUT으로 보정해 반환 */
}

/*
 * [한국어]
 * bsg_sg_io - SG_IO ioctl 한 건을 처리해 하위 드라이버 콜백으로 전달
 *
 * @bd: open된 bsg 노드. bd->sg_io_fn이 실제 명령 변환/제출을 수행하고,
 *      bd->queue가 그 대상 request_queue다.
 * @open_for_write: open(2) 시 파일이 쓰기 가능 모드(FMODE_WRITE)로 열렸는지
 *                  여부. 하위 드라이버가 이 명령이 파괴적(destructive) 쓰기
 *                  명령을 포함해도 되는지 권한 판단에 사용한다(추정).
 * @uarg: 사용자 공간의 struct sg_io_v4* 포인터(ioctl의 세 번째 인자를
 *        캐스팅한 값).
 * @return: 0이면 성공(하위 드라이버가 명령을 처리하고 결과를 hdr에 채움),
 *          -EFAULT는 사용자 공간 복사 실패, -EINVAL은 guard 필드 불일치,
 *          그 외 값은 하위 드라이버(sg_io_fn)가 반환한 에러 코드.
 *
 * sg v4 ABI는 명령/데이터/센스 버퍼에 대한 포인터를 모두 하나의
 * struct sg_io_v4에 담아 유저-커널 간 한 번에 주고받도록 설계되어 있다.
 * 이 함수는 (1) 그 헤더 전체를 커널로 복사하고, (2) guard 필드로 v3/v4
 * 버전을 구분해 손상되거나 잘못된 버전의 요청을 조기에 거부한 뒤,
 * (3) 실제 처리를 하위 드라이버의 sg_io_fn 콜백에 위임하고,
 * (4) 콜백이 채워넣은 결과(상태/센스 데이터 등)를 다시 사용자 공간으로
 * 복사해 돌려준다.
 * 실행 컨텍스트: ioctl(2) 시스템 콜을 호출한 사용자 프로세스의 컨텍스트에서
 * 동기적으로 실행되며, sg_io_fn 콜백이 완료(명령 제출 및 완료 대기)될
 * 때까지 블로킹될 수 있다.
 *
 * 호출 체인:
 *   bsg_ioctl(SG_IO case) → [bsg_sg_io] → bd->sg_io_fn(하위 드라이버 콜백)
 */
static int bsg_sg_io(struct bsg_device *bd, bool open_for_write,
		     void __user *uarg)
{
	struct sg_io_v4 hdr;
	/* [한국어] 사용자 공간 sg_io_v4를 담을 커널 스택 지역 변수 - 이 구조체 크기만큼 복사해 온 뒤 검증·전달에 사용 */
	int ret;
	/* [한국어] 하위 드라이버 콜백의 반환값을 저장할 변수 */

	if (copy_from_user(&hdr, uarg, sizeof(hdr)))
		/* [한국어] 사용자 공간 포인터 uarg로부터 sizeof(hdr) 바이트를 커널 스택으로 복사 시도 - 페이지 폴트/권한 문제로 실패할 수 있어 반환값 검사 필수 */
		return -EFAULT;
		/* [한국어] 복사 실패 시 잘못된 사용자 포인터로 판단해 즉시 -EFAULT 반환 - 이후 로직 진행 안 함 */
	if (hdr.guard != 'Q')
		/* [한국어] sg_io_v4 구조체의 guard 필드가 ASCII 'Q'인지 검사 - sg v3(guard 다른 값 또는 다른 구조체 레이아웃)와 구분해 버전 오인식으로 인한 메모리 오염을 방지하는 프로토콜 가드 */
		return -EINVAL;
		/* [한국어] guard 불일치는 프로토콜 버전 오류로 간주해 -EINVAL 반환 */
	ret = bd->sg_io_fn(bd->queue, &hdr, open_for_write,
			   bsg_timeout(bd, &hdr));
	/* [한국어] 하위 드라이버 콜백 호출 - 대상 큐, 검증된 헤더, 쓰기 가능 여부, 계산된 타임아웃(jiffies)을 넘겨 실제 명령 구성/제출/완료 대기를 위임. 콜백이 hdr을 in-place로 갱신해 결과(상태/센스 등)를 채워 넣는다 */
	if (!ret && copy_to_user(uarg, &hdr, sizeof(hdr)))
		/* [한국어] 콜백이 성공(ret==0)한 경우에만 갱신된 hdr을 사용자 공간으로 되돌려 쓰기 시도 - 실패 시 이미 콜백은 성공했지만 결과 전달이 안 된 상태이므로 -EFAULT로 알림 */
		return -EFAULT;
	return ret;
	/* [한국어] 콜백의 원래 반환값을 그대로 호출자(ioctl 시스템 콜)에 전달 - 성공(0)이거나 콜백이 만든 에러 코드 */
}

/*
 * [한국어]
 * bsg_open - /dev/bsg/<name> open(2) 처리
 *
 * @inode: open되는 파일의 inode. to_bsg_device()로 상위 bsg_device를 얻는 데
 *         사용.
 * @file: VFS가 전달하는 struct file. 이 함수 자체에서는 사용하지 않지만
 *        file_operations.open 시그니처를 맞추기 위해 존재.
 * @return: 0이면 open 허용, -ENXIO는 대상 request_queue가 이미 죽어(dying)
 *          있어 참조를 얻을 수 없는 경우.
 *
 * open() 시점에 request_queue의 참조 카운트를 미리 증가시켜, 이 fd가 열려
 * 있는 동안 하위 드라이버(NVMe 제거, SCSI 디바이스 detach 등)가 큐를
 * 해제하지 못하도록 막는다. 그렇지 않으면 open은 성공했는데 이후 ioctl
 * 처리 중 큐가 이미 free된 use-after-free 상황이 발생할 수 있다.
 * 실행 컨텍스트: open(2) 시스템 콜을 호출한 프로세스 컨텍스트에서 실행.
 *
 * 호출 체인:
 *   VFS do_dentry_open → bsg_fops.open → [bsg_open] → blk_get_queue
 */
static int bsg_open(struct inode *inode, struct file *file)
{
	if (!blk_get_queue(to_bsg_device(inode)->queue))
		/* [한국어] to_bsg_device(inode)로 bsg_device를 얻고 그 queue에 대해 참조 카운트를 원자적으로 증가 시도 - 큐가 이미 QUEUE_FLAG_DYING 등으로 소멸 진행 중이면 false를 반환 */
		return -ENXIO;
		/* [한국어] 참조 획득 실패는 "장치가 더 이상 존재하지 않음"을 뜻하는 -ENXIO로 사용자에게 알림 - open 자체를 거부 */
	return 0;
	/* [한국어] 참조 획득 성공 - open을 허용하고 이후 이 fd에 대한 release까지 큐 참조가 유지됨 */
}

/*
 * [한국어]
 * bsg_release - /dev/bsg/<name> close(2)/release 처리
 *
 * @inode: 닫히는 파일의 inode. to_bsg_device()로 bsg_device 조회에 사용.
 * @file: VFS가 전달하는 struct file(미사용, 시그니처 일치 목적).
 * @return: 항상 0(release는 실패할 수 없는 연산으로 설계됨).
 *
 * bsg_open()에서 증가시킨 request_queue 참조 카운트를 정확히 대응하여
 * 감소시킨다. 이 짝(open/release ↔ get/put)이 깨지면 큐 참조 카운트
 * 누수(release 없이 get만 반복)로 큐가 영원히 해제되지 못하거나, 반대로
 * 이중 put으로 조기 해제 및 use-after-free가 발생할 수 있다.
 * 실행 컨텍스트: close(2)/파일 마지막 참조 해제 시 프로세스 컨텍스트에서
 * 실행.
 *
 * 호출 체인:
 *   VFS __fput → bsg_fops.release → [bsg_release] → blk_put_queue
 */
static int bsg_release(struct inode *inode, struct file *file)
{
	blk_put_queue(to_bsg_device(inode)->queue);
	/* [한국어] bsg_open에서 증가시킨 request_queue 참조 카운트를 감소 - 카운트가 0이 되면 큐 자체의 해제 절차가 진행될 수 있음 */
	return 0;
	/* [한국어] release는 항상 성공으로 처리 - VFS 관례상 release 콜백은 실패를 보고할 방법이 마땅치 않음 */
}

/*
 * [한국어]
 * bsg_get_command_q - SG_GET_COMMAND_Q ioctl 처리: 현재 max_queue 값을 반환
 *
 * @bd: 조회 대상 bsg 노드.
 * @uarg: 결과(int)를 받을 사용자 공간 포인터.
 * @return: put_user()의 반환값 - 0이면 성공, -EFAULT면 사용자 공간 쓰기
 *          실패.
 *
 * 레거시 sg 애플리케이션이 "지금 이 장치가 동시에 몇 개의 명령을 받아줄
 * 준비가 되어 있는지"를 조회하는 호환용 ioctl이다. 값 자체는 bsg 계층의
 * 힌트일 뿐 실제 request_queue의 태그/큐 깊이를 강제하지 않는다.
 * 실행 컨텍스트: ioctl(2) 프로세스 컨텍스트, bsg_ioctl 내부에서 호출.
 *
 * 호출 체인:
 *   bsg_ioctl(SG_GET_COMMAND_Q) → [bsg_get_command_q] → put_user
 */
static int bsg_get_command_q(struct bsg_device *bd, int __user *uarg)
{
	return put_user(READ_ONCE(bd->max_queue), uarg);
	/* [한국어] READ_ONCE로 max_queue를 한 번에 읽어 컴파일러의 재정렬/캐싱 최적화로 인한 데이터 레이스를 방지하고, put_user로 사용자 공간 int 변수에 기록 - 실패 시 put_user가 -EFAULT를 반환 */
}

/*
 * [한국어]
 * bsg_set_command_q - SG_SET_COMMAND_Q ioctl 처리: max_queue 값을 갱신
 *
 * @bd: 갱신 대상 bsg 노드.
 * @uarg: 사용자 공간에서 새 값을 읽어올 int 포인터.
 * @return: 0이면 성공, -EFAULT는 사용자 공간 읽기 실패, -EINVAL은 1 미만의
 *          값을 지정한 경우.
 *
 * 사용자 공간이 이 bsg 노드의 "동시 명령 개수 힌트"를 원하는 값으로
 * 재설정할 수 있게 한다. 0 이하의 값은 의미가 없으므로(최소 1개는 항상
 * 처리 가능해야 함) 사전에 거부한다.
 * 실행 컨텍스트: ioctl(2) 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   bsg_ioctl(SG_SET_COMMAND_Q) → [bsg_set_command_q] → get_user/WRITE_ONCE
 */
static int bsg_set_command_q(struct bsg_device *bd, int __user *uarg)
{
	int max_queue;
	/* [한국어] 사용자 공간에서 읽어올 새 max_queue 후보값을 담을 지역 변수 */

	if (get_user(max_queue, uarg))
		/* [한국어] 사용자 포인터 uarg가 가리키는 int 값을 max_queue로 복사 시도 */
		return -EFAULT;
		/* [한국어] 사용자 공간 접근 실패 시 즉시 반환 */
	if (max_queue < 1)
		/* [한국어] 0 이하 값은 "동시 명령을 하나도 받지 않겠다"는 의미가 되어 무효하므로 검증 */
		return -EINVAL;
		/* [한국어] 유효하지 않은 값에 대해 -EINVAL로 거부 - bd->max_queue는 변경하지 않음 */
	WRITE_ONCE(bd->max_queue, max_queue);
	/* [한국어] 검증을 통과한 값을 WRITE_ONCE로 원자적 단일 기록 - bsg_get_command_q의 READ_ONCE와 짝을 이루어 컴파일러 재정렬로 인한 값 찢어짐(tearing) 방지 */
	return 0;
	/* [한국어] 갱신 성공 */
}

/*
 * [한국어]
 * bsg_ioctl - /dev/bsg/<name>에 대한 unlocked_ioctl 진입점 (전통적 sg ioctl 디스패처)
 *
 * @file: ioctl(2)이 호출된 struct file. file_inode()로 inode를, f_mode로
 *        쓰기 가능 여부를 얻는다.
 * @cmd: 사용자 공간이 지정한 ioctl 번호(SG_IO, SG_SET_TIMEOUT 등).
 * @arg: ioctl의 세 번째 인자 - 대부분의 케이스에서 사용자 공간 포인터로
 *       재해석되어 사용된다.
 * @return: 각 케이스별로 상이 - 성공 시 0 또는 조회 값, 실패 시 음수 errno.
 *          알 수 없는 cmd는 -ENOTTY.
 *
 * 이 함수는 bsg가 지원하는 모든 ioctl 번호를 하나의 switch 문으로 처리하는
 * 중앙 디스패처다. 크게 (1) bsg 자체 확장 ioctl(SG_GET/SET_COMMAND_Q),
 * (2) 레거시 SCSI/sg 호환 ioctl(SG_GET_VERSION_NUM, SCSI_IOCTL_GET_IDLUN 등,
 * 대부분 SCSI 전용 개념이 없는 장치에서는 더미 값을 반환), (3) 실제 명령
 * 전달의 핵심인 SG_IO로 나뉜다.
 * 실행 컨텍스트: ioctl(2) 시스템 콜을 호출한 프로세스 컨텍스트, unlocked이므로
 * BKL(Big Kernel Lock) 없이 여러 fd에 대해 병렬 호출될 수 있다(각 bsg_device
 * 인스턴스는 독립적이므로 인스턴스 간 경쟁은 없음).
 *
 * 호출 체인:
 *   VFS vfs_ioctl → bsg_fops.unlocked_ioctl → [bsg_ioctl]
 *     → bsg_get_command_q / bsg_set_command_q / bsg_sg_io / (인라인 처리)
 */
static long bsg_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct bsg_device *bd = to_bsg_device(file_inode(file));
	/* [한국어] file로부터 inode를 얻고 이를 다시 bsg_device로 역산 - 이 ioctl 호출이 어느 /dev/bsg/* 노드에 대한 것인지 식별 */
	struct request_queue *q = bd->queue;
	/* [한국어] 이후 queue_max_bytes() 조회 등에 반복 사용할 request_queue 포인터를 지역 변수로 캐시 */
	void __user *uarg = (void __user *) arg;
	/* [한국어] ioctl의 raw unsigned long 인자를 사용자 공간 포인터 타입으로 캐스팅 - 대부분의 케이스가 포인터로 데이터를 주고받으므로 공통 변환 */
	int __user *intp = uarg;
	/* [한국어] 다수의 케이스가 단일 int 값을 주고받으므로 int 전용 포인터 타입도 별도로 준비 */
	int val;
	/* [한국어] SG_SET_TIMEOUT/SG_SET_RESERVED_SIZE 등에서 사용자로부터 읽어온 값을 담을 임시 변수 */

	switch (cmd) {
	/* [한국어] ioctl 번호(cmd)에 따라 분기 - 아래 각 case가 사실상 bsg가 지원하는 ioctl ABI 전체를 나열 */
	/*
	 * Our own ioctls
	 */
	case SG_GET_COMMAND_Q:
		/* [한국어] bsg 확장 ioctl: 현재 max_queue 조회 요청 */
		return bsg_get_command_q(bd, uarg);
		/* [한국어] 실제 처리를 bsg_get_command_q()에 위임하고 그 반환값을 그대로 ioctl 반환값으로 사용 */
	case SG_SET_COMMAND_Q:
		/* [한국어] bsg 확장 ioctl: max_queue 갱신 요청 */
		return bsg_set_command_q(bd, uarg);
		/* [한국어] 실제 처리를 bsg_set_command_q()에 위임 */

	/*
	 * SCSI/sg ioctls
	 */
	case SG_GET_VERSION_NUM:
		/* [한국어] sg 드라이버의 프로토콜 버전 번호를 조회하는 레거시 ioctl */
		return put_user(30527, intp);
		/* [한국어] sg v4를 지원함을 나타내는 고정 버전 번호(30527)를 사용자 공간에 기록 - 이 값은 sg 드라이버 히스토리상 정해진 매직 넘버 */
	case SCSI_IOCTL_GET_IDLUN:
		/* [한국어] SCSI ID/LUN 정보를 조회하는 레거시 ioctl */
		return put_user(0, intp);
		/* [한국어] bsg가 노출하는 장치(NVMe 등 포함)에는 SCSI ID/LUN 개념이 없거나 의미가 없을 수 있어 0(고정값)으로 응답해 애플리케이션 호환성만 유지 */
	case SCSI_IOCTL_GET_BUS_NUMBER:
		/* [한국어] SCSI 버스 번호를 조회하는 레거시 ioctl */
		return put_user(0, intp);
		/* [한국어] 위와 동일한 이유로 고정값 0 응답 */
	case SG_SET_TIMEOUT:
		/* [한국어] 이 bsg 노드의 기본 타임아웃을 설정하는 ioctl */
		if (get_user(val, intp))
			/* [한국어] 사용자 공간에서 clock_t 단위 타임아웃 값을 읽어옴 */
			return -EFAULT;
			/* [한국어] 읽기 실패 시 -EFAULT */
		bd->timeout = clock_t_to_jiffies(val);
		/* [한국어] 사용자 공간 clock_t 단위 값을 커널 jiffies 단위로 변환해 노드별 기본 타임아웃으로 저장 - 이후 bsg_timeout()의 폴백 값으로 쓰임 */
		return 0;
		/* [한국어] 설정 성공 */
	case SG_GET_TIMEOUT:
		/* [한국어] 현재 설정된 기본 타임아웃을 조회하는 ioctl */
		return jiffies_to_clock_t(bd->timeout);
		/* [한국어] 내부적으로 jiffies 단위로 저장된 값을 사용자 공간이 이해하는 clock_t 단위로 변환해 반환 - 이 ioctl은 값 자체를 반환값으로 사용(포인터 아님)하는 특이 케이스 */
	case SG_GET_RESERVED_SIZE:
		/* [한국어] 허용되는 최대 데이터 버퍼 크기를 조회하는 ioctl */
		return put_user(min(bd->reserved_size, queue_max_bytes(q)),
				intp);
		/* [한국어] 사용자가 설정한 상한(reserved_size)과 하드웨어/큐가 실제로 지원하는 상한(queue_max_bytes) 중 더 작은 값을 실질적인 한도로 계산해 반환 */
	case SG_SET_RESERVED_SIZE:
		/* [한국어] 최대 데이터 버퍼 크기 상한을 설정하는 ioctl */
		if (get_user(val, intp))
			/* [한국어] 사용자 공간에서 새 상한 후보값을 읽어옴 */
			return -EFAULT;
		if (val < 0)
			/* [한국어] 음수 크기는 무의미하므로 검증 */
			return -EINVAL;
		bd->reserved_size =
			min_t(unsigned int, val, queue_max_bytes(q));
		/* [한국어] 사용자가 요청한 값이라도 큐가 지원하는 queue_max_bytes를 초과하지 못하도록 즉시 클램핑해 저장 - 이후 하위 드라이버가 이 값을 넘는 버퍼를 준비하지 않도록 보장 */
		return 0;
		/* [한국어] 설정 성공 */
	case SG_EMULATED_HOST:
		/* [한국어] 이 장치가 에뮬레이션된 SCSI 호스트인지 여부를 조회하는 레거시 ioctl */
		return put_user(1, intp);
		/* [한국어] bsg가 노출하는 장치는 실제 SCSI 호스트 어댑터가 아니라 request_queue 기반 추상화이므로 항상 "에뮬레이션됨"(1)으로 응답 */
	case SG_IO:
		/* [한국어] 실제 명령 전달의 핵심 ioctl */
		return bsg_sg_io(bd, file->f_mode & FMODE_WRITE, uarg);
		/* [한국어] file->f_mode에서 FMODE_WRITE 비트를 검사해 이 fd가 쓰기 가능하게 열렸는지 판단하고, bsg_sg_io()에 위임해 실제 SG_IO 처리를 수행 */
	case SCSI_IOCTL_SEND_COMMAND:
		/* [한국어] 매우 오래된 레거시 SCSI 명령 전달 ioctl - bsg에서는 지원하지 않음 */
		pr_warn_ratelimited("%s: calling unsupported SCSI_IOCTL_SEND_COMMAND\n",
				current->comm);
		/* [한국어] 레이트 리밋된 경고 로그로 어떤 프로세스(current->comm)가 지원하지 않는 ioctl을 호출했는지 커널 로그에 남김 - 반복 호출 시 로그 폭주 방지를 위해 ratelimited 버전 사용 */
		return -EINVAL;
		/* [한국어] 지원하지 않는 ioctl로 -EINVAL 반환 */
	default:
		/* [한국어] 위 어느 케이스에도 해당하지 않는 알 수 없는 ioctl 번호 */
		return -ENOTTY;
		/* [한국어] POSIX 관례상 알 수 없는 ioctl에 대해 -ENOTTY("적절하지 않은 ioctl") 반환 */
	}
}

/*
 * [한국어]
 * bsg_check_uring_features - io_uring passthrough에 필요한 빅 SQE/CQE 지원 여부 검증
 *
 * @issue_flags: io_uring 코어가 이 명령을 실행하며 전달하는 플래그 비트마스크.
 *               IO_URING_F_SQE128/IO_URING_F_CQE32 비트가 이 명령을 위해
 *               128바이트 SQE와 32바이트 CQE가 준비되어 있는지를 나타낸다.
 * @return: 0이면 필요한 기능이 모두 갖춰짐, -EOPNOTSUPP은 둘 중 하나라도
 *          빠진 경우.
 *
 * bsg passthrough 명령(특히 SCSI CDB나 NVMe 커맨드처럼 큰 페이로드가 필요한
 * 경우)은 io_uring의 기본 64바이트 SQE/16바이트 CQE로는 담을 공간이 부족할
 * 수 있어, 확장된 128바이트 SQE와 32바이트 CQE(SQE128/CQE32) 지원을
 * 전제로 한다. 이 함수는 그 전제 조건이 실제로 이 io_uring 인스턴스에서
 * 활성화되어 있는지 확인하는 게이트 역할을 한다.
 * 실행 컨텍스트: io_uring 워커/제출 컨텍스트(호출자인 bsg_uring_cmd와 동일).
 *
 * 호출 체인:
 *   bsg_uring_cmd → [bsg_check_uring_features]
 */
static int bsg_check_uring_features(unsigned int issue_flags)
{
	/* BSG passthrough requires big SQE/CQE support */
	if ((issue_flags & (IO_URING_F_SQE128|IO_URING_F_CQE32)) !=
	    (IO_URING_F_SQE128|IO_URING_F_CQE32))
		/* [한국어] issue_flags에서 SQE128과 CQE32 두 비트를 마스킹한 결과가 두 비트를 모두 켠 값과 다르면(즉 둘 중 하나라도 꺼져 있으면) 조건 참 - AND 마스크 후 완전 일치 비교로 "둘 다 켜짐"을 검사하는 관용적 비트 연산 패턴 */
		return -EOPNOTSUPP;
		/* [한국어] 빅 SQE/CQE 중 하나라도 지원되지 않으면 이 io_uring 인스턴스로는 bsg passthrough를 사용할 수 없음을 알림 */
	return 0;
	/* [한국어] 두 기능이 모두 활성화되어 있어 이후 uring_cmd_fn 호출을 진행해도 안전함 */
}

/*
 * [한국어]
 * bsg_uring_cmd - /dev/bsg/<name>에 대한 io_uring IORING_OP_URING_CMD 진입점
 *
 * @ioucmd: io_uring 서브시스템이 전달하는 명령 컨텍스트 - ioucmd->file로
 *          이 명령이 어느 fd(=bsg 노드)에 대한 것인지 식별할 수 있고,
 *          내부에 SQE 페이로드(예: NVMe 커맨드/SCSI CDB)를 담고 있다.
 * @issue_flags: 이 명령이 처음 제출(issue)되는지, 재시도(re-issue)되는지 등
 *               실행 상황을 나타내는 플래그. bsg_check_uring_features가 이
 *               중 SQE128/CQE32 비트를 검사한다.
 * @return: 0 또는 하위 드라이버가 반환한 값이면 성공/처리 중, -EOPNOTSUPP은
 *          빅 SQE/CQE 미지원이거나 이 bsg 노드가 uring_cmd_fn을 등록하지
 *          않은 경우.
 *
 * SG_IO ioctl 경로와 달리, io_uring passthrough는 매 명령마다 시스템 콜
 * 진입/컨텍스트 스위치 오버헤드 없이 SQE를 커널 링 버퍼에 쌓아 배치로
 * 제출할 수 있어 고성능 NVMe passthrough 등에 적합하다. 이 함수는 그
 * io_uring 명령을 (1) 빅 SQE/CQE 지원 여부 검증 후 (2) 하위 드라이버가
 * 등록한 uring_cmd_fn 콜백으로 그대로 전달하는 얇은 브리지 역할만 한다.
 * 실행 컨텍스트: io_uring 제출 경로(io_uring_enter 시스템 콜 컨텍스트 또는
 * io_uring 워커 스레드 컨텍스트) - IORING_OP_URING_CMD의 .uring_cmd
 * file_operations 콜백으로 호출된다.
 *
 * 호출 체인:
 *   io_uring core → bsg_fops.uring_cmd → [bsg_uring_cmd]
 *     → bsg_check_uring_features
 *     → bd->uring_cmd_fn(하위 드라이버 콜백, 예: NVMe uring passthrough)
 */
static int bsg_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
	struct bsg_device *bd = to_bsg_device(file_inode(ioucmd->file));
	/* [한국어] io_uring_cmd가 들고 있는 struct file로부터 inode를 얻고, to_bsg_device()로 이 명령이 속한 bsg_device를 역산 */
	bool open_for_write = ioucmd->file->f_mode & FMODE_WRITE;
	/* [한국어] 이 명령이 제출된 fd가 쓰기 가능 모드로 열려 있는지 확인 - SG_IO 경로의 open_for_write와 동일한 의미로 하위 드라이버에 전달 */
	struct request_queue *q = bd->queue;
	/* [한국어] 이후 uring_cmd_fn 호출에 넘길 대상 request_queue를 지역 변수로 캐시 */
	int ret;
	/* [한국어] 중간 단계(feature 검증)의 반환값을 담을 변수 */

	ret = bsg_check_uring_features(issue_flags);
	/* [한국어] 이 io_uring 인스턴스가 빅 SQE(128바이트)/CQE(32바이트)를 지원하는지 먼저 검증 */
	if (ret)
		/* [한국어] 검증 실패(0이 아닌 반환값) 시 */
		return ret;
		/* [한국어] 하위 드라이버 콜백을 호출하지 않고 즉시 에러(-EOPNOTSUPP) 반환 */

	if (!bd->uring_cmd_fn)
		/* [한국어] 이 bsg 노드를 등록한 하위 드라이버가 io_uring passthrough 콜백을 아예 제공하지 않았는지 확인 */
		return -EOPNOTSUPP;
		/* [한국어] 콜백이 없으면 이 fd에서는 uring_cmd를 지원하지 않음을 알림 - SG_IO ioctl 경로는 여전히 사용 가능 */

	return bd->uring_cmd_fn(q, ioucmd, issue_flags, open_for_write);
	/* [한국어] 실제 명령 해석/제출을 하위 드라이버 콜백에 위임 - 대상 큐, io_uring 명령 컨텍스트, 실행 플래그, 쓰기 가능 여부를 전달. 콜백은 필요 시 비동기로 완료를 io_uring에 통지할 수 있음(추정) */
}

/*
 * [한국어]
 * bsg_fops - /dev/bsg/<name>에 연결되는 file_operations 테이블
 *
 * 이 파일이 구현하는 모든 사용자 공간 진입점(open/release/ioctl/uring_cmd/
 * llseek)을 한데 묶어 cdev_init()에 전달하기 위한 정적 테이블이다.
 * SG_IO(전통적 ioctl 경로)와 uring_cmd(io_uring passthrough 경로)가
 * NVMe/SCSI passthrough의 두 가지 실질적 진입점이며, 나머지는 리소스
 * 수명 관리(open/release)와 32비트 호환(compat_ioctl), seek 지원이다.
 */
static const struct file_operations bsg_fops = {
	.open		=	bsg_open,
	/* [한국어] open(2) 시 request_queue 참조 획득 */
	.release	=	bsg_release,
	/* [한국어] close(2)/최종 fput 시 request_queue 참조 해제 */
	.unlocked_ioctl	=	bsg_ioctl,
	/* [한국어] ioctl(2) 처리 - BKL 없이(unlocked) 병렬 호출 가능 */
	.compat_ioctl	=	compat_ptr_ioctl,
	/* [한국어] 32비트 사용자 공간 프로세스가 64비트 커널에서 ioctl을 호출할 때, 인자 포인터를 적절히 변환한 뒤 unlocked_ioctl(bsg_ioctl)로 위임하는 커널 공통 헬퍼 - sg_io_v4 구조체 자체는 32/64비트 레이아웃이 호환되도록 설계되어 별도 변환 로직이 불필요함(추정) */
	.uring_cmd	=	bsg_uring_cmd,
	/* [한국어] IORING_OP_URING_CMD 처리 - io_uring passthrough 진입점 */
	.owner		=	THIS_MODULE,
	/* [한국어] 이 file_operations를 소유한 모듈을 THIS_MODULE(bsg 자신)로 지정 - fd가 열려 있는 동안 모듈 언로드를 막기 위해 커널이 참조 카운트에 사용 */
	.llseek		=	default_llseek,
	/* [한국어] 캐릭터 디바이스이지만 표준 llseek 동작(파일 오프셋 이동)을 그대로 제공 - bsg 자체는 오프셋 개념을 쓰지 않지만 일부 유틸리티/이식성을 위해 기본 구현 유지(추정) */
};

/*
 * [한국어]
 * bsg_device_release - struct device 참조 카운트가 0이 될 때 호출되는 해제 콜백
 *
 * @dev: 참조 카운트가 0이 된 embedded struct device(bsg_device.device).
 *
 * 디바이스 모델은 struct device의 마지막 참조가 해제(put_device)되면
 * device->release 콜백을 호출해 상위 구조체를 정리하도록 강제한다(release
 * 콜백을 지정하지 않으면 커널이 경고를 낸다). 이 함수가 그 계약을 이행해
 * IDA에서 minor 번호를 반납하고, bsg_device 메모리 자체를 kfree한다.
 * 실행 컨텍스트: put_device() 호출 경로(대개 bsg_unregister_queue 또는
 * 등록 실패 시 에러 처리 경로)의 컨텍스트를 그대로 물려받아 실행되며,
 * 별도의 스레드나 워크큐로 지연되지 않는다(참조 카운트가 실제로 0이 되는
 * 마지막 put 호출 스택에서 동기적으로 실행).
 *
 * 호출 체인:
 *   put_device(&bd->device) → (kobject release) → [bsg_device_release]
 *     → ida_free / kfree
 */
static void bsg_device_release(struct device *dev)
{
	struct bsg_device *bd = container_of(dev, struct bsg_device, device);
	/* [한국어] 참조 카운트가 0이 된 struct device 포인터로부터 container_of로 상위 bsg_device 포인터를 복원 */

	ida_free(&bsg_minor_ida, MINOR(bd->device.devt));
	/* [한국어] 이 장치에 할당됐던 minor 번호를 MINOR() 매크로로 devt에서 추출한 뒤 전역 IDA에 반납 - 이후 다른 bsg_register_queue() 호출이 같은 minor 번호를 재사용할 수 있게 됨 */
	kfree(bd);
	/* [한국어] bsg_device 구조체 자체의 메모리를 해제 - 이 시점 이후 bd 포인터는 더 이상 유효하지 않음(dangling) */
}

/*
 * [한국어]
 * bsg_unregister_queue - 하위 드라이버가 호출하는 /dev/bsg/<name> 해제 API
 *
 * @bd: bsg_register_queue()가 반환했던 bsg_device 포인터. 호출 이후 bd는
 *      더 이상 유효하다고 가정할 수 없다(put_device가 마지막 참조를
 *      해제하면 bsg_device_release에서 즉시 kfree될 수 있음).
 *
 * 하위 드라이버(NVMe 컨트롤러 제거, SCSI 디바이스 detach 등)가 자신의
 * request_queue를 실제로 해제하기 전에 반드시 먼저 호출해야 하는 정리
 * 함수다. sysfs 심볼릭 링크 제거, cdev/device 등록 해제(이후 open() 시도는
 * ENXIO/ENODEV로 실패), 마지막으로 참조 해제(put_device) 순으로 진행해,
 * 새로운 open을 차단한 뒤 기존 참조가 모두 없어지면 메모리를 회수하는
 * 순서를 보장한다.
 * 실행 컨텍스트: 하위 드라이버의 제거/정리 경로(대개 프로세스 컨텍스트,
 * 디바이스 모델의 device_del 흐름과 유사한 시점)에서 호출된다.
 * EXPORT_SYMBOL_GPL로 노출되어 GPL 호환 모듈만 링크해 호출할 수 있다.
 *
 * 호출 체인:
 *   (NVMe/SCSI 하위 드라이버의 큐 해제 경로) → [bsg_unregister_queue]
 *     → sysfs_remove_link / cdev_device_del / put_device(→ bsg_device_release)
 */
void bsg_unregister_queue(struct bsg_device *bd)
{
	struct gendisk *disk = bd->queue->disk;
	/* [한국어] 이 bsg 노드가 연결된 request_queue에 매핑된 gendisk(범용 디스크 객체)를 얻음 - sysfs 심볼릭 링크 제거 시 그 kobject(queue_kobj)가 필요하기 때문 */

	if (disk && disk->queue_kobj.sd)
		/* [한국어] 이 queue가 gendisk에 속해 있고, 그 queue_kobj가 실제로 sysfs에 등록(sd 필드가 채워짐)되어 있는지 확인 - 등록되지 않았다면 제거할 링크 자체가 없음 */
		sysfs_remove_link(&disk->queue_kobj, "bsg");
		/* [한국어] bsg_register_queue()에서 만들었던 "bsg"라는 이름의 sysfs 심볼릭 링크(queue -> bsg 디바이스)를 제거 */
	cdev_device_del(&bd->cdev, &bd->device);
	/* [한국어] 문자 장치와 디바이스 모델 등록을 함께 해제 - 이 호출 이후 사용자 공간의 신규 open(/dev/bsg/<name>)은 실패하게 됨 */
	put_device(&bd->device);
	/* [한국어] bsg_register_queue()가 device_initialize() 시점에 잡아둔 초기 참조를 반납 - 다른 참조(예: 열려 있는 fd)가 남아있지 않다면 이 호출이 마지막 참조가 되어 bsg_device_release가 즉시 실행됨 */
}
EXPORT_SYMBOL_GPL(bsg_unregister_queue);
/* [한국어] bsg_unregister_queue 심볼을 GPL 라이선스 모듈에 한해 커널 외부(다른 모듈)에서 호출 가능하도록 익스포트 - NVMe/SCSI 등 별도로 빌드되는 모듈이 이 함수를 링크해 쓸 수 있게 함 */

/*
 * [한국어]
 * bsg_register_queue - 하위 드라이버가 호출하는 /dev/bsg/<name> 등록 API
 *
 * @q: 이 bsg 노드가 노출할 대상 request_queue. 호출자(하위 드라이버)가
 *     이미 초기화해 둔 상태여야 한다.
 * @parent: 새로 생성될 struct device의 부모 디바이스(sysfs 트리 상속용,
 *          예: NVMe 컨트롤러/네임스페이스의 struct device).
 * @name: /dev/bsg/<name>으로 노출될 이름 문자열(예: "nvme0n1").
 * @sg_io_fn: SG_IO ioctl 처리를 위임받을 하위 드라이버 콜백(필수로 간주,
 *            추정).
 * @uring_cmd_fn: io_uring passthrough 처리를 위임받을 콜백(NULL 허용 -
 *                미지원 시 bsg_uring_cmd가 -EOPNOTSUPP 반환).
 * @return: 성공 시 새로 할당된 struct bsg_device 포인터(추후
 *          bsg_unregister_queue에 그대로 전달해야 함), 실패 시
 *          ERR_PTR(-ENOMEM)/ERR_PTR(-ENOSPC 등).
 *
 * 이 함수는 (1) bsg_device 구조체 할당 및 필드 초기화, (2) 전역 IDA에서
 * minor 번호 할당, (3) devt/class/parent/release 콜백 설정 후
 * device_initialize()로 디바이스 모델 초기화, (4) cdev_init()으로 문자
 * 장치와 bsg_fops 연결, (5) cdev_device_add()로 실제 커널에 등록(이 시점부터
 * open(2) 가능), (6) 선택적으로 큐의 sysfs kobject에 "bsg"라는 심볼릭
 * 링크 생성, 순서로 진행된다. 각 단계 실패 시 이미 수행된 앞 단계를
 * 역순으로 되돌리는 goto 기반 에러 처리를 사용한다.
 * 실행 컨텍스트: 하위 드라이버의 프로브(probe)/초기화 경로(프로세스
 * 컨텍스트)에서 호출된다. EXPORT_SYMBOL_GPL로 노출된다.
 *
 * 호출 체인:
 *   (NVMe/SCSI 하위 드라이버의 프로브 경로) → [bsg_register_queue]
 *     → kzalloc_obj → ida_alloc_max → device_initialize
 *     → cdev_init → cdev_device_add → sysfs_create_link
 */
struct bsg_device *bsg_register_queue(struct request_queue *q,
		struct device *parent, const char *name, bsg_sg_io_fn *sg_io_fn,
		bsg_uring_cmd_fn *uring_cmd_fn)
{
	struct bsg_device *bd;
	/* [한국어] 새로 생성할 bsg_device를 가리킬 포인터 */
	int ret;
	/* [한국어] 각 하위 단계(ida 할당, cdev 등록, sysfs 링크 생성)의 반환값을 임시 저장할 변수 */

	bd = kzalloc_obj(*bd);
	/* [한국어] *bd(즉 struct bsg_device)의 타입/크기에 맞춰 0으로 채워진 메모리를 할당하는 타입-세이프 kzalloc 래퍼 - 명시적으로 sizeof(struct bsg_device)를 적지 않아도 컴파일러가 *bd의 타입에서 크기를 추론 */
	if (!bd)
		/* [한국어] 할당 실패(메모리 부족) 여부 확인 */
		return ERR_PTR(-ENOMEM);
		/* [한국어] 실패 시 에러 코드를 포인터로 인코딩하는 ERR_PTR 관용구로 -ENOMEM 반환 - 호출자는 IS_ERR()로 판별 */
	bd->max_queue = BSG_DEFAULT_CMDS;
	/* [한국어] 동시 명령 개수 힌트를 기본값 64로 초기화 */
	bd->reserved_size = INT_MAX;
	/* [한국어] 데이터 버퍼 상한을 사실상 무제한(INT_MAX)으로 초기화 - 실사용 시 queue_max_bytes(q)로 재클리핑됨 */
	bd->queue = q;
	/* [한국어] 이 bsg 노드가 대상으로 삼을 request_queue를 저장 - 이후 모든 ioctl/uring_cmd 처리에서 이 포인터를 통해 큐에 접근 */
	bd->sg_io_fn = sg_io_fn;
	/* [한국어] 호출자가 넘긴 SG_IO 처리 콜백을 저장 - bsg_sg_io()가 이후 이 포인터를 호출 */
	bd->uring_cmd_fn = uring_cmd_fn;
	/* [한국어] 호출자가 넘긴 io_uring passthrough 콜백을 저장(NULL일 수 있음) - bsg_uring_cmd()가 NULL 여부를 검사 후 호출 */

	ret = ida_alloc_max(&bsg_minor_ida, BSG_MAX_DEVS - 1, GFP_KERNEL);
	/* [한국어] 전역 IDA에서 0부터 BSG_MAX_DEVS-1(minor 번호 공간 최대값) 사이의 미사용 정수를 하나 할당 - GFP_KERNEL로 필요 시 슬립 가능한 할당 수행 */
	if (ret < 0) {
		/* [한국어] IDA 할당 실패(음수 반환) - 대개 -ENOMEM 또는 사용 가능한 minor 소진(-ENOSPC) */
		if (ret == -ENOSPC)
			/* [한국어] 특별히 "minor 번호 공간 고갈"인 경우를 구분 */
			dev_err(parent, "bsg: too many bsg devices\n");
			/* [한국어] 부모 디바이스의 로그 컨텍스트로 "bsg 장치가 너무 많다"는 에러 메시지 출력 - 관리자가 원인을 진단할 수 있도록 안내 */
		kfree(bd);
		/* [한국어] 앞서 할당했던 bsg_device 메모리를 해제 - 아직 device_initialize 전이므로 put_device 경로가 아니라 직접 kfree로 되돌림 */
		return ERR_PTR(ret);
		/* [한국어] IDA가 반환한 에러 코드를 그대로 ERR_PTR로 감싸 반환 */
	}
	bd->device.devt = MKDEV(bsg_major, ret);
	/* [한국어] bsg_init()에서 할당받은 major 번호와 방금 얻은 minor 번호(ret)를 조합해 이 장치의 device number(devt)를 구성 */
	bd->device.class = &bsg_class;
	/* [한국어] 이 디바이스가 속할 클래스를 bsg_class로 지정 - /sys/class/bsg/ 아래에 노출되고 devnode 콜백(bsg_devnode)이 경로 생성에 쓰임 */
	bd->device.parent = parent;
	/* [한국어] 호출자가 넘긴 부모 디바이스를 설정 - sysfs 트리 상에서 이 bsg 노드가 부모(예: NVMe 컨트롤러) 아래에 위치하게 됨 */
	bd->device.release = bsg_device_release;
	/* [한국어] 참조 카운트가 0이 될 때 호출될 release 콜백을 등록 - 디바이스 모델은 release 콜백이 없으면 경고를 내므로 필수 설정 */
	dev_set_name(&bd->device, "%s", name);
	/* [한국어] 호출자가 넘긴 name 문자열을 그대로 이 디바이스의 sysfs/디바이스 이름으로 설정 (예: "nvme0n1") */
	device_initialize(&bd->device);
	/* [한국어] embedded struct device의 kobject를 초기화하고 초기 참조 카운트를 1로 설정 - 이 시점부터 put_device()로 참조를 관리할 수 있는 상태가 됨 */

	cdev_init(&bd->cdev, &bsg_fops);
	/* [한국어] 문자 장치 구조체를 초기화하며 이 파일이 정의한 bsg_fops(open/release/ioctl/uring_cmd 등)를 연결 */
	bd->cdev.owner = THIS_MODULE;
	/* [한국어] cdev의 소유 모듈을 THIS_MODULE로 지정 - fd가 열려 있는 동안 bsg 모듈이 언로드되지 않도록 참조 카운트 보호에 사용 */
	ret = cdev_device_add(&bd->cdev, &bd->device);
	/* [한국어] cdev와 device를 함께 커널에 등록 - 성공하면 이 시점부터 /dev/bsg/<name>에 대한 open(2)이 가능해짐 */
	if (ret)
		/* [한국어] 등록 실패 시 */
		goto out_put_device;
		/* [한국어] 아직 cdev 자체는 등록되지 않았으므로 device 참조만 반납하는 경로로 점프 */

	if (q->disk && q->disk->queue_kobj.sd) {
		/* [한국어] 대상 큐가 gendisk에 속해 있고 그 queue_kobj가 sysfs에 실제로 등록되어 있는지 확인 - 그래야 심볼릭 링크를 걸 대상 kobject가 존재함 */
		ret = sysfs_create_link(&q->disk->queue_kobj, &bd->device.kobj,
					"bsg");
		/* [한국어] queue의 sysfs 디렉터리 아래에 "bsg"라는 이름으로 이 bsg 디바이스 kobject를 가리키는 심볼릭 링크 생성 - 사용자가 sysfs를 통해 큐 ↔ bsg 노드 연관관계를 탐색할 수 있게 함 */
		if (ret)
			/* [한국어] 심볼릭 링크 생성 실패 시 */
			goto out_device_del;
			/* [한국어] 이미 cdev_device_add까지 성공했으므로 그것까지 되돌리는 경로로 점프 */
	}

	return bd;
	/* [한국어] 모든 초기화 단계가 성공 - 호출자(하위 드라이버)에게 완성된 bsg_device 포인터 반환 */

out_device_del:
	cdev_device_del(&bd->cdev, &bd->device);
	/* [한국어] sysfs 링크 생성 실패 시 앞서 성공했던 cdev_device_add()를 되돌려 문자 장치/디바이스 등록을 해제 */
out_put_device:
	put_device(&bd->device);
	/* [한국어] device_initialize()에서 얻은 초기 참조를 반납 - cdev_device_add 이전에 실패했다면 이 호출이 사실상 유일한 참조 해제이며 bsg_device_release를 트리거해 IDA 반납과 kfree까지 이어짐 */
	return ERR_PTR(ret);
	/* [한국어] 실패 원인 에러 코드를 ERR_PTR로 감싸 반환 - 호출자는 IS_ERR()로 실패를 감지해야 함 */
}
EXPORT_SYMBOL_GPL(bsg_register_queue);
/* [한국어] bsg_register_queue 심볼을 GPL 호환 모듈에서 호출 가능하도록 익스포트 - NVMe/SCSI 등 트리 외부(또는 별도 빌드) 모듈이 프로브 시점에 이 함수를 호출해 /dev/bsg/*를 생성 */

/*
 * [한국어]
 * bsg_devnode - bsg_class의 devnode 콜백: /dev/bsg/<name> 경로 문자열 생성
 *
 * @dev: 경로를 생성할 대상 struct device(embedded된 bd->device).
 * @mode: 생성될 디바이스 노드의 권한 모드를 조정할 수 있는 출력 포인터
 *        (이 구현에서는 사용하지 않아 커널 기본 모드가 적용됨).
 * @return: kasprintf()로 새로 할당된 "bsg/<name>" 형태의 문자열(devtmpfs가
 *          이 문자열을 /dev/ 아래 경로로 사용). 메모리 부족 시 NULL(추정).
 *
 * devtmpfs/udev가 디바이스 노드를 자동 생성할 때, class의 기본 이름
 * 규칙("bsg") 대신 하위 디렉터리(bsg/) 아래에 dev_name(dev)(예:
 * "nvme0n1")을 파일명으로 쓰도록 지정하기 위한 콜백이다. 이 콜백이 없다면
 * devtmpfs는 보통 클래스 이름을 그대로 최상위 파일명으로 사용하려 든다.
 * 실행 컨텍스트: 디바이스 등록(device_add, cdev_device_add 내부에서 호출)
 * 시점에 커널 디바이스 모델이 호출하는 콜백.
 *
 * 호출 체인:
 *   device_add (cdev_device_add 내부) → devtmpfs 경로 생성 로직 → [bsg_devnode]
 */
static char *bsg_devnode(const struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "bsg/%s", dev_name(dev));
	/* [한국어] "bsg/" 접두사와 이 디바이스의 이름(dev_name, 예: nvme0n1)을 조합한 문자열을 GFP_KERNEL로 새로 할당해 반환 - 결과 경로는 /dev/bsg/nvme0n1과 같은 형태가 됨 */
}

/*
 * bsg 클래스 정의. /sys/class/bsg/ 아래에 NVMe bsg 장치들이 생성된다.
 */
static const struct class bsg_class = {
	.name		= "bsg",
	/* [한국어] /sys/class/bsg/로 노출될 클래스 이름 - 이 파일 상단에서 전방 선언했던 심볼의 실제 정의 */
	.devnode	= bsg_devnode,
	/* [한국어] devtmpfs가 이 클래스에 속한 디바이스의 노드 경로를 만들 때 호출할 콜백으로 bsg_devnode를 등록 - "bsg/<name>" 하위 경로 규칙 적용 */
};

/*
 * [한국어]
 * bsg_init - bsg 서브시스템 모듈 초기화 함수
 *
 * @return: 0이면 성공, 그 외 값은 class_register() 또는
 *          alloc_chrdev_region() 실패에서 비롯된 음수 errno.
 *
 * bsg가 동작하기 위한 두 가지 전역 사전조건 - (1) bsg_class를 디바이스
 * 모델에 등록해 /sys/class/bsg/가 존재하게 만들고, (2) BSG_MAX_DEVS개의
 * minor 번호를 위한 문자 장치 major 번호 영역을 할당 - 을 준비한다.
 * 이 초기화가 끝나야 하위 드라이버가 이후 아무 때나 bsg_register_queue()를
 * 안전하게 호출할 수 있다. 두 번째 단계가 실패하면 첫 번째 단계에서 등록한
 * 클래스를 반드시 되돌려야(class_unregister) 부분적으로 초기화된 상태가
 * 남지 않는다.
 * 실행 컨텍스트: device_initcall 매크로에 의해 커널 부팅 과정(또는 모듈로
 * 빌드된 경우 modprobe 시점)에 프로세스 컨텍스트에서 정확히 1회 실행된다.
 *
 * 호출 체인:
 *   device_initcall 프레임워크 → [bsg_init]
 *     → class_register → alloc_chrdev_region
 */
static int __init bsg_init(void)
{
	dev_t devid;
	/* [한국어] alloc_chrdev_region()이 채워줄 시작 device number(major+minor 인코딩)를 담을 변수 */
	int ret;
	/* [한국어] 각 하위 단계의 반환값을 저장할 변수 */

	ret = class_register(&bsg_class);
	/* [한국어] bsg_class를 커널 디바이스 모델에 등록 - 성공해야 이후 device_initialize()에서 bd->device.class = &bsg_class가 유효한 클래스를 참조하게 됨 */
	if (ret)
		/* [한국어] 클래스 등록 실패 시 */
		return ret;
		/* [한국어] 아직 아무 것도 추가로 할당하지 않았으므로 별도 롤백 없이 바로 반환 */

	ret = alloc_chrdev_region(&devid, 0, BSG_MAX_DEVS, "bsg");
	/* [한국어] "bsg"라는 이름으로 minor 0부터 BSG_MAX_DEVS개 연속된 문자 장치 번호 영역을 커널에 요청해 devid에 결과(할당된 시작 devt)를 받음 */
	if (ret)
		/* [한국어] chrdev 영역 할당 실패 시 */
		goto destroy_bsg_class;
		/* [한국어] 이미 등록했던 bsg_class를 되돌리는 경로로 점프 */
	bsg_major = MAJOR(devid);
	/* [한국어] 할당받은 devid에서 MAJOR() 매크로로 major 번호만 추출해 전역 변수에 저장 - 이후 모든 bsg_register_queue() 호출이 MKDEV(bsg_major, minor)로 devt를 구성하는 데 사용 */

	printk(KERN_INFO BSG_DESCRIPTION " version " BSG_VERSION
	       " loaded (major %d)\n", bsg_major);
	/* [한국어] 모듈 로드 성공을 커널 로그(KERN_INFO 수준)에 기록 - 드라이버 설명, 버전, 할당받은 major 번호를 함께 출력해 부팅 로그만으로 진단 가능하게 함 */
	return 0;
	/* [한국어] 초기화 전체 성공 */

destroy_bsg_class:
	class_unregister(&bsg_class);
	/* [한국어] chrdev 영역 할당이 실패했으므로 앞서 등록했던 bsg_class를 다시 해제해 부분 초기화 상태가 남지 않도록 정리 */
	return ret;
	/* [한국어] 실패 원인 에러 코드를 그대로 반환 - 모듈 로드 자체가 실패 처리됨 */
}

MODULE_AUTHOR("Jens Axboe");
/* [한국어] 모듈 메타데이터: 원작자 정보 - modinfo 등에서 조회 가능 */
MODULE_DESCRIPTION(BSG_DESCRIPTION);
/* [한국어] 모듈 메타데이터: 위에서 정의한 설명 문자열을 그대로 사용 */
MODULE_LICENSE("GPL");
/* [한국어] 모듈 라이선스를 GPL로 명시 - GPL 전용 심볼(EXPORT_SYMBOL_GPL로 노출된 다른 커널 심볼들) 사용 및 링크 가능 여부에 영향 */

device_initcall(bsg_init);
/* [한국어] bsg_init()을 디바이스 드라이버 초기화 단계(다른 late initcall들과 유사한 우선순위)에 등록 - 블록/디바이스 모델 인프라가 준비된 이후, 실제 블록 드라이버들의 probe보다는 앞서 실행되어야 그 드라이버들이 bsg_register_queue()를 호출할 때 bsg_class/chrdev 영역이 이미 준비되어 있음이 보장됨(추정) */
