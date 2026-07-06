// SPDX-License-Identifier: GPL-2.0
/*
 * Disk events - monitor disk events like media change and eject request.
 */
/*
 * [한국어 설명] 디스크 미디어 변경/이탈(eject) 이벤트 감지 및 유저스페이스 통지 프레임워크 (disk-events.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 블록 계층(block layer)에서 "탈착 가능한 미디어를 가진 블록
 * 디바이스"의 상태 변화 - 미디어 삽입/제거(media change), 이젝트 버튼
 * 눌림(eject request) - 를 감지하고, 그 결과를 사용자 공간(userspace)의
 * udev에게 KOBJ_CHANGE uevent로 통지하는 범용 폴링(polling) 프레임워크를
 * 구현한다. CD/DVD 같은 광학 드라이브, USB 이동식 저장장치, 일부 외장
 * NVMe/이동식 컨트롤러처럼 미디어가 물리적으로 교체될 수 있는 디바이스
 * 드라이버는 각자 fops->check_events() 콜백만 구현하면, 이 파일이 제공하는
 * disk_events 인프라(주기적 delayed_work, sysfs 노드, 이벤트 마스크 관리)를
 * 그대로 재사용할 수 있다. 반대로 NVMe SSD처럼 비탈착형(non-removable)
 * 미디어를 쓰는 디바이스는 check_events 콜백을 아예 등록하지 않으므로
 * disk->ev가 NULL로 남고, 이 파일의 대부분 함수는 그런 디바이스에 대해
 * 사실상 no-op으로 동작한다(disk->ev가 NULL이면 즉시 반환하는 패턴 반복).
 *
 * === 전체 아키텍처에서의 위치 ===
 * gendisk(블록 디바이스 서술자) 하나가 add_disk()로 시스템에 등록되는
 * 과정에서, disk_alloc_events()가 disk_events 구조체를 할당하고(대상
 * 드라이버가 check_events를 구현한 경우에만), disk_add_events()가 전역
 * disk_events 리스트에 등록한 뒤 __disk_unblock_events()를 통해 최초의
 * delayed_work를 큐잉한다. 이 delayed_work(disk_events_workfn)는
 * system_freezable_power_efficient_wq 워크큐 위에서 주기적으로 실행되며,
 * 매 실행마다 disk_check_events()가 disk->fops->check_events()를 호출해
 * 드라이버로부터 현재 이벤트 상태를 받아온다. 감지된 이벤트는
 * disk->ev->pending 비트마스크에 누적되고, DISK_EVENT_FLAG_UEVENT 플래그가
 * 설정된 디바이스라면 disk_event_uevent()가 kobject_uevent_env()로
 * KOBJ_CHANGE uevent를 발생시켜 udev 등 사용자 공간 데몬에게 전달한다.
 * del_gendisk()/disk_release() 시점에는 반대로 disk_del_events()/
 * disk_release_events()가 폴링을 중지하고 자원을 반납한다. 이 모든 흐름은
 * 워크큐 워커 스레드 또는 open/close 시스템 콜을 처리하는 프로세스
 * 컨텍스트에서 실행되며, NVMe 드라이버의 blk_mq_run_hw_queue ->
 * nvme_queue_rq -> doorbell 기반 I/O 제출 경로와는 완전히 분리된
 * "제어 평면(control plane)" 이벤트 경로다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 상위 자료구조는 include/linux/blkdev.h에 정의된
 * struct gendisk(특히 ev, events, event_flags, fops 필드)이며, block/blk.h에
 * 이 파일이 export하는 함수 원형(disk_alloc_events, disk_add_events,
 * disk_del_events, disk_release_events, disk_block_events,
 * disk_unblock_events, disk_flush_events)이 선언되어 있다. block/genhd.c는
 * gendisk 등록/해제 경로(add_disk, del_gendisk, disk_release)에서 이 파일의
 * alloc/add/del/release 함수들을 호출하는 상위 호출자이며, block/bdev.c는
 * blkdev open/put 경로에서 disk_block_events()/disk_unblock_events()로 open
 * 처리 중 이벤트 폴링을 일시 정지시키는 호출자다. 드라이버 쪽에서는 각
 * 블록 디바이스 드라이버(예: drivers/block의 loop, floppy 등 탈착형 미디어
 * 드라이버)가 struct block_device_operations의 check_events 콜백을 구현하여
 * 이 파일의 disk_check_events()로부터 호출받고, 반대로
 * disk_check_media_change()/disk_force_media_change()는
 * include/linux/blkdev.h에 EXPORT_SYMBOL(_GPL)로 공개되어 드라이버가 미디어
 * 변경을 이 프레임워크에 보고하는 진입점 역할을 한다. 데이터 흐름 관점에서는
 * "드라이버 check_events() 반환값(이벤트 비트마스크) -> disk->ev->pending
 * 누적 -> disk_uevents[] 문자열 배열로 변환 -> kobject_uevent_env() -> udev"
 * 순으로 흐른다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct disk_events: gendisk 1개당 1개씩 존재하는 이벤트 추적 상태(리스트
 *   노드, 락, 차단 카운트, pending/clearing 비트마스크, 폴 주기, delayed_work).
 * - disk_events_poll_jiffies(): 디바이스별 poll_msecs 또는 전역 기본값을
 *   jiffies로 환산.
 * - disk_block_events()/disk_unblock_events()/__disk_unblock_events():
 *   참조 카운트 방식으로 이벤트 폴링을 중첩 차단/해제.
 * - disk_check_events()/disk_events_workfn(): 실제로 드라이버 콜백을 호출하고
 *   다음 폴을 예약하는 핵심 워크 함수.
 * - disk_clear_events()/disk_check_media_change(): 동기적으로 이벤트를
 *   확인해 pending 값을 즉시 반환하는 블로킹 인터페이스.
 * - disk_force_media_change(): 드라이버가 미디어 교체를 강제로 알릴 때 쓰는
 *   헬퍼.
 * - disk_alloc_events()/disk_add_events()/disk_del_events()/
 *   disk_release_events(): gendisk 생명주기와 맞물린 초기화/해제 4단계.
 * - sysfs show/store 함수들(disk_events_show 등): /sys/block/<disk>/events*
 *   노드 구현.
 */
#include <linux/export.h> /* [한국어] EXPORT_SYMBOL()/EXPORT_SYMBOL_GPL() 매크로 제공 - disk_check_media_change(), disk_force_media_change()를 모듈 경계 밖으로 공개하기 위해 필요 */
#include <linux/moduleparam.h> /* [한국어] module_param_cb(), struct kernel_param_ops 등 커널 모듈 파라미터(events_dfl_poll_msecs) 등록 인프라 제공 */
#include <linux/blkdev.h> /* [한국어] struct gendisk, DISK_EVENT_*, DISK_EVENT_FLAG_* 등 블록 계층 핵심 타입/상수 정의 */
#include "blk.h" /* [한국어] block 서브시스템 내부 전용 헤더 - 이 파일이 구현하는 함수 원형과 disk_check_media_change 등 관련 선언 포함 */

/*
 * [한국어]
 * struct disk_events - 디스크 1개(gendisk 1개)당 하나씩 존재하는 이벤트 폴링 상태
 *
 * gendisk->ev에 매달리는 사이드카(side-car) 구조체로, 탈착형 미디어를 가진
 * 디바이스에서만(disk->fops->check_events && disk->events가 모두 참일 때만)
 * disk_alloc_events()가 동적 할당한다. 폴링 주기, 아직 사용자공간에 보고되지
 * 않은 이벤트, 드라이버에 클리어를 요청 중인 이벤트, 그리고 이벤트 폴링을
 * 몇 겹으로 차단(block) 중인지를 추적한다.
 */
struct disk_events {
	struct list_head	node;		/* all disk_event's */
	/* [한국어] 전역 리스트 disk_events(이 파일의 static 변수)에 연결되는 노드.
	 * 설정자: disk_add_events()가 list_add_tail()로 등록, disk_del_events()가
	 *   list_del_init()으로 제거.
	 * 읽는 자: disk_events_set_dfl_poll_msecs()가 list_for_each_entry로 순회하며
	 *   시스템 전역 기본 폴 주기가 바뀔 때 모든 디스크에 즉시 재확인을 요청.
	 * 값 범위: 리스트에 연결되어 있거나(등록 상태) list_del_init 이후의 빈
	 *   리스트 헤드 상태.
	 * 동기화: disk_events_mutex(파일 정적 뮤텍스)로 리스트 전체가 보호됨. */
	struct gendisk		*disk;		/* the associated disk */
	/* [한국어] 이 disk_events가 속한 gendisk에 대한 역참조 포인터.
	 * 설정자: disk_alloc_events()에서 할당 직후 1회만 설정, 이후 불변.
	 * 읽는 자: disk_check_events(), disk_events_poll_jiffies() 등 거의 모든
	 *   함수가 disk->fops->check_events, disk->events, disk->event_flags 등을
	 *   읽기 위해 사용.
	 * 값 범위: 유효한 gendisk 포인터, disk_events가 살아있는 동안 NULL 아님.
	 * 동기화: 불변 필드이므로 별도 락 불필요. */
	spinlock_t		lock;
	/* [한국어] block/pending/clearing 필드를 보호하는 스핀락.
	 * 설정자: disk_alloc_events()에서 spin_lock_init()으로 초기화.
	 * 읽는 자/잠금자: disk_block_events(), __disk_unblock_events(),
	 *   disk_flush_events(), disk_check_events(), disk_clear_events() 등 이
	 *   구조체의 가변 필드를 만지는 모든 함수.
	 * 값 범위: 잠김/풀림 두 상태.
	 * 동기화: disk_flush_events()는 임의 컨텍스트(인터럽트 포함)에서도 호출될
	 *   수 있어 spin_lock_irq/irqsave 계열로 인터럽트를 함께 차단한다. */

	struct mutex		block_mutex;	/* protects blocking */
	/* [한국어] disk_block_events()의 "차단 카운트 증가 + 워크 취소" 절차 전체를
	 * 직렬화하는 뮤텍스(잠들 수 있음).
	 * 설정자: disk_alloc_events()에서 mutex_init()으로 초기화.
	 * 읽는 자/잠금자: disk_block_events()만 사용.
	 * 값 범위: 잠김/풀림.
	 * 동기화: 여러 스레드가 동시에 disk_block_events()를 호출해도 첫 번째
	 *   호출자만 cancel_delayed_work_sync()를 실제로 수행하고 나머지는 그
	 *   완료를 기다리도록(뮤텍스로 순서를 강제) 보장한다 - 스핀락만으로는
	 *   "취소 완료까지 대기"라는 잠들 수 있는 대기를 표현할 수 없어 별도의
	 *   뮤텍스가 필요하다. */
	int			block;		/* event blocking depth */
	/* [한국어] 이벤트 폴링이 몇 겹으로 차단되어 있는지 나타내는 중첩 카운터.
	 * 설정자: disk_alloc_events()가 1로 초기화(등록 전에는 폴링 금지 상태로
	 *   시작), disk_block_events()가 증가, __disk_unblock_events()가 감소.
	 * 읽는 자: __disk_unblock_events()가 0에 도달했는지 검사해 실제로 워크를
	 *   재큐잉할지 결정하고, disk_flush_events()/disk_check_events()가
	 *   "차단 중이 아닌지" 검사할 때 사용.
	 * 값 범위: 1 이상이 정상(초기값 1 포함); 0 이하로 내려가면
	 *   WARN_ON_ONCE(ev->block <= 0)가 버그로 간주해 경고.
	 * 동기화: ev->lock(스핀락)으로 보호. */
	unsigned int		pending;	/* events already sent out */
	/* [한국어] 드라이버가 감지했지만 아직 disk_clear_events()/
	 * disk_check_media_change() 호출자에게 전달(소비)되지 않은 이벤트
	 * 비트마스크(DISK_EVENT_MEDIA_CHANGE, DISK_EVENT_EJECT_REQUEST).
	 * 설정자: disk_check_events()가 OR 연산으로 새로 감지된 이벤트를 누적.
	 * 읽는 자: disk_clear_events()가 요청한 mask와 AND하여 반환값을 만들고
	 *   동시에 그만큼 제거(소비).
	 * 값 범위: DISK_EVENT_* 비트의 OR 조합(이 파일 기준 비트0=MEDIA_CHANGE,
	 *   비트1=EJECT_REQUEST).
	 * 동기화: ev->lock으로 보호 - disk_check_events()의 생산자 측과
	 *   disk_clear_events()의 소비자 측이 서로 다른 컨텍스트(워크큐 vs
	 *   open/ioctl 프로세스)에서 동시에 접근할 수 있기 때문. */
	unsigned int		clearing;	/* events being cleared */
	/* [한국어] 다음 disk_check_events() 호출 시 드라이버의 check_events(disk,
	 * clearing) 두 번째 인자로 넘겨줄 클리어 요청 비트마스크. 드라이버는 이
	 * 마스크에 포함된 이벤트에 한해 내부 상태를 초기화(예: eject 플래그
	 * 리셋)해도 된다는 뜻으로 해석한다.
	 * 설정자: disk_flush_events()가 OR 연산으로 요청 비트를 추가,
	 *   disk_clear_events()가 disk_check_events() 호출 직전에 지역 변수로
	 *   옮겨 담고 0으로 리셋.
	 * 읽는 자: disk_events_workfn()이 &ev->clearing의 주소를 그대로
	 *   disk_check_events()에 넘겨 드라이버 호출 인자로 사용.
	 * 값 범위: DISK_EVENT_* 비트 조합.
	 * 동기화: ev->lock으로 보호. disk_clear_events()는 스택 지역 변수로 값을
	 *   복사해 둠으로써 disk_check_events() 실행 도중 disk_flush_events()가
	 *   끼어들어도(경쟁) 그 변경분을 잃지 않고 나중에 되돌려 받는다. */

	long			poll_msecs;	/* interval, -1 for default */
	/* [한국어] 이 디스크 전용 폴링 주기(밀리초). sysfs의
	 * events_poll_msecs 속성으로 사용자가 직접 조정 가능.
	 * 설정자: disk_alloc_events()가 -1(기본값 사용)로 초기화,
	 *   disk_events_poll_msecs_store()가 sysfs write로 갱신.
	 * 읽는 자: disk_events_poll_jiffies()가 이 값이 0 이상이면 무조건 이
	 *   값을 우선 사용, 음수(-1)이면 전역 기본값/POLL 플래그를 대신 확인.
	 * 값 범위: -1(시스템 기본값 사용) 또는 0 이상(밀리초 단위 명시적 주기,
	 *   0은 폴링 비활성화).
	 * 동기화: disk_events_poll_msecs_store()는 값을 바꾸기 전에
	 *   disk_block_events()로 폴링을 잠시 멈추고 바꾼 뒤 다시 풀어주므로
	 *   ev->lock 없이도 안전 - 폴링 워크 자체가 그 구간 동안 실행되지 않기
	 *   때문. */
	struct delayed_work	dwork;
	/* [한국어] 폴링을 구현하는 지연 워크(delayed_work). 만료되면
	 * disk_events_workfn()이 실행된다.
	 * 설정자: disk_alloc_events()가 INIT_DELAYED_WORK()로 콜백을 등록.
	 * 읽는 자/큐잉자: __disk_unblock_events(), disk_flush_events(),
	 *   disk_check_events()가 queue_delayed_work()/mod_delayed_work()로
	 *   system_freezable_power_efficient_wq에 큐잉.
	 * 값 범위: 커널 워크큐 코어가 관리하는 불투명 상태(대기/실행/취소).
	 * 동기화: 워크큐 자체의 내부 동기화에 의존하며, disk_block_events()의
	 *   cancel_delayed_work_sync()가 현재 실행 중인 워크 함수의 완료까지
	 *   보장해 준다(잠들 수 있음, 커널독의 "CONTEXT: Might sleep"과 일치). */
};

/*
 * [한국어] 이벤트 비트 인덱스(ilog2로 계산한 비트 위치) -> sysfs "events"
 * 속성에 노출할, 사람이 읽을 수 있는 이름 문자열 매핑 테이블. 지정
 * 초기화자([idx] = ...)를 사용해 DISK_EVENT_MEDIA_CHANGE(비트0)/
 * DISK_EVENT_EJECT_REQUEST(비트1) 값 자체가 바뀌어도 매핑이 깨지지 않도록
 * 했다. __disk_events_show()가 이 배열을 순회하며 문자열을 조립한다.
 */
static const char *disk_events_strs[] = {
	[ilog2(DISK_EVENT_MEDIA_CHANGE)]	= "media_change", /* [한국어] 비트0(DISK_EVENT_MEDIA_CHANGE)에 대응하는 sysfs 표시 이름 */
	[ilog2(DISK_EVENT_EJECT_REQUEST)]	= "eject_request", /* [한국어] 비트1(DISK_EVENT_EJECT_REQUEST)에 대응하는 sysfs 표시 이름 */
};

/*
 * [한국어] 이벤트 비트 인덱스 -> uevent 환경변수(envp) 문자열 매핑 테이블.
 * "KEY=VALUE" 형식은 kobject_uevent_env()가 그대로 netlink 메시지의
 * 환경변수 라인으로 사용하는 관례를 따른다. disk_event_uevent()가 이
 * 배열에서 문자열을 골라 envp[]를 채운다. char* (const 아님)인 이유는
 * uevent 코어 API가 non-const char* 배열을 요구하기 때문(문자열 리터럴
 * 자체는 여전히 읽기 전용 메모리에 위치).
 */
static char *disk_uevents[] = {
	[ilog2(DISK_EVENT_MEDIA_CHANGE)]	= "DISK_MEDIA_CHANGE=1", /* [한국어] 미디어 변경 시 udev에 전달할 환경변수 문자열 */
	[ilog2(DISK_EVENT_EJECT_REQUEST)]	= "DISK_EJECT_REQUEST=1", /* [한국어] 이젝트 요청 시 udev에 전달할 환경변수 문자열 */
};

/* list of all disk_events */
/* [한국어] 시스템에 등록된 모든 disk_events 인스턴스를 연결하는 전역
 * 리스트와 그 리스트를 보호하는 뮤텍스. disk_add_events()/
 * disk_del_events()가 등록/해제하며, disk_events_set_dfl_poll_msecs()가
 * 전역 기본 주기 변경 시 전체 순회에 사용한다. 뮤텍스(스핀락이 아님)인
 * 이유는 리스트 자체의 추가/제거가 프로세스 컨텍스트(add_disk/
 * del_gendisk)에서만 일어나 잠들어도 문제 없기 때문이다. */
static DEFINE_MUTEX(disk_events_mutex); /* [한국어] disk_events 전역 리스트 전용 뮤텍스 정의 및 초기화(초기 상태: 풀림) */
static LIST_HEAD(disk_events); /* [한국어] 전역 disk_events 연결 리스트 헤드 정의 및 초기화(비어있는 상태로 시작) */

/* disable in-kernel polling by default */
static unsigned long disk_events_dfl_poll_msecs; /* [한국어] 시스템 전역 기본 폴 주기(ms) - module_param_cb(events_dfl_poll_msecs, ...)의 백킹 변수. BSS 영역이라 0으로 암묵 초기화되어 "기본 폴링 비활성화" 상태로 시작하며, disk_events_poll_jiffies()가 디바이스별 값이 -1(미설정)일 때 이 값을 대신 사용 */

/*
 * [한국어]
 * disk_events_poll_jiffies - 이 디스크에 적용할 폴링 주기를 jiffies 단위로 계산
 *
 * @disk: 대상 gendisk (disk->ev가 유효하다고 가정)
 * @return: msecs_to_jiffies()로 환산된 폴링 간격(jiffies). 폴링이 필요 없으면 0.
 *
 * 디바이스별 poll_msecs가 명시적으로 설정되어 있으면(0 이상) 그 값을 그대로
 * 쓰고, 아직 기본값(-1)이라면 disk->event_flags에 DISK_EVENT_FLAG_POLL이
 * 켜져 있을 때만 시스템 전역 기본값(disk_events_dfl_poll_msecs)을 사용한다 -
 * 즉 드라이버가 "폴링이 꼭 필요하다"고 명시적으로 표시하지 않는 한, 전역
 * 기본값이 0(비활성화)인 상태에서는 폴링이 돌지 않는다.
 * 실행 컨텍스트: 호출자의 락(ev->lock)이 이미 걸린 상태에서 불리는 것이
 *   일반적이며, 이 함수 자체는 disk->ev를 읽기만 하고 잠그지 않는다.
 * 호출자: __disk_unblock_events(), disk_check_events() 등 다음 워크
 *   큐잉 시각을 정해야 하는 모든 지점.
 * 피호출자: msecs_to_jiffies().
 * 에러 경로: 없음(실패할 수 없는 순수 계산 함수).
 *
 * 호출 체인:
 *   __disk_unblock_events / disk_check_events -> [disk_events_poll_jiffies]
 *   -> msecs_to_jiffies
 */
static unsigned long disk_events_poll_jiffies(struct gendisk *disk)
{
	struct disk_events *ev = disk->ev; /* [한국어] 이 gendisk에 매달린 이벤트 상태 구조체 포인터를 지역 변수로 캐시 */
	long intv_msecs = 0; /* [한국어] 폴링 주기(ms) 계산 결과 초기값 - "폴링 불필요"를 의미하는 0으로 시작 */

	/*
	 * If device-specific poll interval is set, always use it.  If
	 * the default is being used, poll if the POLL flag is set.
	 */
	if (ev->poll_msecs >= 0) /* [한국어] sysfs로 사용자가 디바이스별 주기를 명시적으로 설정한 경우(0 이상) */
		intv_msecs = ev->poll_msecs; /* [한국어] 전역 기본값보다 이 디바이스 전용 값을 우선 적용 */
	else if (disk->event_flags & DISK_EVENT_FLAG_POLL) /* [한국어] 디바이스별 값이 -1(미설정)이면, 드라이버가 폴링을 요구하는지(FLAG_POLL) 확인 */
		intv_msecs = disk_events_dfl_poll_msecs; /* [한국어] 요구한다면 시스템 전역 기본 주기(모듈 파라미터)를 사용 */

	return msecs_to_jiffies(intv_msecs); /* [한국어] ms 단위를 커널 타이머 단위인 jiffies로 변환해 반환 - 0이면 호출자가 "폴링 안 함"으로 해석 */
}

/**
 * disk_block_events - block and flush disk event checking
 * @disk: disk to block events for
 *
 * On return from this function, it is guaranteed that event checking
 * isn't in progress and won't happen until unblocked by
 * disk_unblock_events().  Events blocking is counted and the actual
 * unblocking happens after the matching number of unblocks are done.
 *
 * Note that this intentionally does not block event checking from
 * disk_clear_events().
 *
 * CONTEXT:
 * Might sleep.
 */
/*
 * [한국어]
 * disk_block_events - 이벤트 폴링을 차단하고, 이미 실행 중인 워크가 있다면
 * 그 완료를 기다린 뒤 반환
 *
 * @disk: 대상 gendisk
 * @return: 없음
 *
 * 이 함수가 반환하면 "현재 이벤트 체크가 진행 중이지 않고,
 * disk_unblock_events() 호출 전까지는 새로 시작되지도 않는다"는 것이
 * 보장된다. block 카운트를 두어 중첩 호출을 허용하며, 마지막
 * disk_unblock_events()가 호출되어 카운트가 0이 될 때만 실제로 폴링이
 * 재개된다. 다만 disk_clear_events() 경로의 동기적 이벤트 체크는 의도적으로
 * 이 차단의 영향을 받지 않는다(위 커널독 Note 참고). 이 함수는 open/close,
 * suspend/resume처럼 이벤트 상태가 잠깐 동안 일관되게 멈춰 있어야 하는
 * 임계 구간 진입 시 호출된다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 잠들 수 있음(mutex_lock,
 *   cancel_delayed_work_sync 모두 블로킹 가능) - 커널독의 "CONTEXT: Might
 *   sleep"과 일치.
 * 호출자: block/bdev.c(open 처리 중), disk_del_events(), disk_clear_events(),
 *   disk_events_poll_msecs_store() 등.
 * 피호출자: cancel_delayed_work_sync() - 실행 중인 disk_events_workfn()의
 *   완료를 기다림.
 * 에러 경로: disk->ev가 NULL(이벤트 미사용 디바이스)이면 그냥 반환.
 *
 * 호출 체인:
 *   bdev_open / disk_del_events / disk_clear_events -> [disk_block_events]
 *   -> cancel_delayed_work_sync
 */
void disk_block_events(struct gendisk *disk)
{
	struct disk_events *ev = disk->ev; /* [한국어] 이 디스크의 이벤트 상태 구조체 - NULL이면 이벤트 미사용 디바이스 */
	unsigned long flags; /* [한국어] spin_lock_irqsave가 저장할 이전 인터럽트 플래그 */
	bool cancel; /* [한국어] 내가 이 호출에서 "최초의 차단자"라서 실제 취소를 수행해야 하는지 여부 */

	if (!ev) /* [한국어] 이벤트 프레임워크를 쓰지 않는 디바이스(예: 대부분의 NVMe) */
		return; /* [한국어] 할 일이 없으므로 즉시 반환 */

	/*
	 * Outer mutex ensures that the first blocker completes canceling
	 * the event work before further blockers are allowed to finish.
	 */
	mutex_lock(&ev->block_mutex); /* [한국어] 여러 스레드가 동시에 block을 호출해도 취소 절차를 한 명씩 순서대로 처리하도록 직렬화 */

	spin_lock_irqsave(&ev->lock, flags); /* [한국어] block 카운터를 인터럽트로부터도 보호하며 잠금(disk_flush_events가 인터럽트 컨텍스트에서 올 수 있으므로) */
	cancel = !ev->block++; /* [한국어] 증가 전 값이 0이었다면(이번이 첫 차단자) cancel=true로 표시, 그 후 카운트 증가 */
	spin_unlock_irqrestore(&ev->lock, flags); /* [한국어] 카운터 갱신이 끝났으므로 스핀락 해제 및 인터럽트 상태 복원 */

	if (cancel) /* [한국어] 내가 첫 차단자였을 때만 실제로 워크 취소를 수행(중첩 호출자는 생략) */
		cancel_delayed_work_sync(&disk->ev->dwork); /* [한국어] 예약된/실행 중인 disk_events_workfn을 취소하고, 이미 실행 중이라면 완료까지 대기(sync) */

	mutex_unlock(&ev->block_mutex); /* [한국어] 취소 절차 완료 - 다음 대기자(다른 스레드의 disk_block_events 호출)를 진행시킴 */
}

/*
 * [한국어]
 * __disk_unblock_events - block 카운트를 1 감소시키고, 0에 도달하면 폴링을
 * 즉시 또는 주기적으로 재개
 *
 * @disk: 대상 gendisk
 * @check_now: true면 지연 없이 즉시 워크를 실행(지연 0), false면 폴 주기만큼
 *   지연 후 실행
 * @return: 없음
 *
 * disk_unblock_events()(공개 API)와 disk_add_events()/disk_clear_events()가
 * 공유하는 내부 구현이다. disk_unblock_events()는 항상 check_now=false로
 * 부르는 반면, disk_add_events()는 최초 등록 직후 즉시 한 번 체크하도록
 * check_now=true로, disk_clear_events()는 그 사이 disk_flush_events()가
 * 끼어들었는지에 따라 동적으로 결정한다.
 * WARN_ON_ONCE(ev->block <= 0)는 disk_block_events()와 짝이 맞지 않게
 * unblock을 더 많이 호출한 프로그래밍 오류를 잡기 위한 방어적 점검이다.
 * 실행 컨텍스트: "Don't care. Safe to call from irq context"(공개 API
 *   disk_unblock_events 문서 참고) - 스핀락+irqsave만 쓰고 잠들지 않으므로
 *   인터럽트 컨텍스트에서도 안전.
 * 호출자: disk_unblock_events(), disk_add_events(), disk_clear_events(),
 *   disk_events_poll_msecs_store().
 * 피호출자: disk_events_poll_jiffies(), queue_delayed_work().
 * 에러 경로: block 카운트가 이미 0 이하인 비정상 상태면 WARN만 남기고
 *   out_unlock으로 건너뛰어 더 이상의 손상을 막는다.
 *
 * 호출 체인:
 *   disk_unblock_events / disk_add_events / disk_clear_events
 *   -> [__disk_unblock_events] -> disk_events_poll_jiffies / queue_delayed_work
 */
static void __disk_unblock_events(struct gendisk *disk, bool check_now)
{
	struct disk_events *ev = disk->ev; /* [한국어] 호출자가 이미 disk->ev != NULL임을 보장한 상태로 호출(각 공개 wrapper에서 확인) */
	unsigned long intv; /* [한국어] 다음 폴링까지의 간격(jiffies) - disk_events_poll_jiffies() 결과 저장 */
	unsigned long flags; /* [한국어] spin_lock_irqsave가 저장하는 이전 인터럽트 상태 */

	spin_lock_irqsave(&ev->lock, flags); /* [한국어] block 카운터 갱신과 워크 큐잉 결정을 인터럽트로부터 보호 */

	if (WARN_ON_ONCE(ev->block <= 0)) /* [한국어] block/unblock 짝이 맞지 않는 프로그래밍 오류 방어 - 이미 0 이하인데 또 unblock 호출됨 */
		goto out_unlock; /* [한국어] 더 감소시키지 않고 잠금만 풀고 반환(카운터 손상 방지) */

	if (--ev->block) /* [한국어] 감소시킨 후에도 아직 0이 아니면(다른 차단자가 더 남아있음) */
		goto out_unlock; /* [한국어] 아직 완전히 풀리지 않았으므로 폴링을 재개하지 않고 반환 */

	intv = disk_events_poll_jiffies(disk); /* [한국어] block 카운트가 0에 도달(완전 해제)했으므로 이제 적용할 폴 주기를 계산 */
	if (check_now) /* [한국어] 호출자가 즉시 한 번 체크를 요청한 경우(disk_add_events, 또는 clearing 대기 중이던 disk_clear_events) */
		queue_delayed_work(system_freezable_power_efficient_wq,
				&ev->dwork, 0); /* [한국어] 지연 0으로 큐잉 - 사실상 즉시 워크큐 워커에서 disk_events_workfn 실행 */
	else if (intv) /* [한국어] 즉시 체크가 아니고, 계산된 주기가 0보다 크면(폴링이 실제로 필요한 경우) */
		queue_delayed_work(system_freezable_power_efficient_wq,
				&ev->dwork, intv); /* [한국어] intv 만큼 지연시켜 다음 폴 시각에 워크를 실행하도록 예약 */
out_unlock: /* [한국어] 위 두 조기 종료(goto) 지점이 합류하는 공통 정리 라벨 */
	spin_unlock_irqrestore(&ev->lock, flags); /* [한국어] 스핀락 해제 및 인터럽트 상태 복원 */
}

/**
 * disk_unblock_events - unblock disk event checking
 * @disk: disk to unblock events for
 *
 * Undo disk_block_events().  When the block count reaches zero, it
 * starts events polling if configured.
 *
 * CONTEXT:
 * Don't care.  Safe to call from irq context.
 */
/*
 * [한국어]
 * disk_unblock_events - disk_block_events()로 건 차단을 1단계 해제
 *
 * @disk: 대상 gendisk
 * @return: 없음
 *
 * disk_block_events()와 1:1로 짝을 맞춰 호출해야 하는 공개 API의 "해제"
 * 절반이다. 내부적으로 check_now=false로 __disk_unblock_events()를 호출해,
 * 즉시 체크가 아니라 평소 폴링 주기를 따르도록 한다.
 * 실행 컨텍스트: "Don't care. Safe to call from irq context" - 잠들지 않는
 *   __disk_unblock_events()만 호출하므로 인터럽트 컨텍스트에서도 안전.
 * 호출자: block/bdev.c의 open 처리 완료 경로 등, disk_block_events()로
 *   차단을 걸었던 지점과 짝을 이루는 정리 코드.
 * 피호출자: __disk_unblock_events(disk, false).
 * 에러 경로: disk->ev가 NULL이면(이벤트 미사용 디바이스) 아무 것도 하지 않음.
 *
 * 호출 체인:
 *   (disk_block_events로 걸어둔 임계 구간 종료) -> [disk_unblock_events]
 *   -> __disk_unblock_events
 */
void disk_unblock_events(struct gendisk *disk)
{
	if (disk->ev) /* [한국어] 이벤트 프레임워크를 사용하는 디바이스일 때만 */
		__disk_unblock_events(disk, false); /* [한국어] 즉시 체크는 요구하지 않고(false), 평소 폴 주기로 재개 */
}

/**
 * disk_flush_events - schedule immediate event checking and flushing
 * @disk: disk to check and flush events for
 * @mask: events to flush
 *
 * Schedule immediate event checking on @disk if not blocked.  Events in
 * @mask are scheduled to be cleared from the driver.  Note that this
 * doesn't clear the events from @disk->ev.
 *
 * CONTEXT:
 * If @mask is non-zero must be called with disk->open_mutex held.
 */
/*
 * [한국어]
 * disk_flush_events - 지정한 이벤트를 드라이버에서 클리어하도록 요청하고,
 * 차단 중이 아니라면 즉시 재확인을 예약
 *
 * @disk: 대상 gendisk
 * @mask: 드라이버에게 클리어를 요청할 이벤트 비트마스크(0이면 클리어 요청
 *   없이 즉시 재확인만 트리거)
 * @return: 없음
 *
 * mask에 담긴 이벤트는 ev->clearing에 누적되었다가, 다음 disk_check_events()
 * 실행 시 drv->check_events(disk, clearing) 인자로 전달되어 드라이버가 해당
 * 이벤트의 내부 상태(예: eject 플래그)를 리셋할 기회를 준다. 동시에 현재
 * 차단 중(block)이 아니라면 mod_delayed_work()로 지연 없이(0) 워크를
 * 앞당겨 실행시킨다 - 이미 예약된 워크가 있어도 그 타이머를 다시 세팅한다.
 * 예: eject 버튼 인터럽트, 전역 기본 폴 주기 변경
 * (disk_events_set_dfl_poll_msecs) 등 "다음 정기 폴링을 기다릴 필요 없이
 * 지금 당장 확인해야 하는" 이벤트에서 사용된다.
 * 실행 컨텍스트: mask가 0이 아니면 disk->open_mutex를 쥔 채로 호출해야
 *   함(커널독 CONTEXT). 스핀락만 사용하므로 임의 컨텍스트(인터럽트
 *   포함)에서 호출 가능.
 * 호출자: block/bdev.c(eject 계열 ioctl 처리), disk_events_set_dfl_poll_msecs()
 *   (전역 기본 주기 변경 시 모든 디스크에 대해 mask=0으로 호출).
 * 피호출자: mod_delayed_work().
 * 에러 경로: disk->ev가 NULL이면 아무 것도 하지 않고 반환.
 *
 * 호출 체인:
 *   (eject ioctl) / disk_events_set_dfl_poll_msecs -> [disk_flush_events]
 *   -> mod_delayed_work
 */
void disk_flush_events(struct gendisk *disk, unsigned int mask)
{
	struct disk_events *ev = disk->ev; /* [한국어] 이 디스크의 이벤트 상태 - NULL이면 이벤트 미사용 */

	if (!ev) /* [한국어] 이벤트 프레임워크 미사용 디바이스 */
		return; /* [한국어] 할 일 없음 */

	spin_lock_irq(&ev->lock); /* [한국어] clearing/block 필드를 인터럽트로부터 보호하며 잠금(disk_flush_events는 irq 컨텍스트에서도 호출 가능) */
	ev->clearing |= mask; /* [한국어] 요청받은 이벤트 비트를 클리어 대기 집합에 누적(기존 대기분과 OR) */
	if (!ev->block) /* [한국어] 현재 차단 중이 아니라면(폴링이 정상적으로 동작 중이라면) */
		mod_delayed_work(system_freezable_power_efficient_wq,
				&ev->dwork, 0); /* [한국어] 예약된 지연 워크의 타이머를 0(즉시)으로 다시 세팅해 다음 스케줄러 실행 시 바로 돌게 함 */
	spin_unlock_irq(&ev->lock); /* [한국어] 잠금 해제 및 인터럽트 복원 */
}

/*
 * Tell userland about new events.  Only the events listed in @disk->events are
 * reported, and only if DISK_EVENT_FLAG_UEVENT is set.  Otherwise, events are
 * processed internally but never get reported to userland.
 */
/*
 * [한국어]
 * disk_event_uevent - 감지된 이벤트를 KOBJ_CHANGE uevent로 사용자 공간에 통지
 *
 * @disk: 대상 gendisk
 * @events: 이번에 새로 감지된(또는 강제로 통지할) 이벤트 비트마스크
 * @return: 없음
 *
 * disk->events(이 디바이스가 지원한다고 선언한 이벤트 종류)와 실제 발생한
 * events의 교집합만 골라, disk_uevents[] 문자열 배열(예:
 * "DISK_MEDIA_CHANGE=1") 형태의 환경변수 배열 envp[]를 만든 뒤
 * kobject_uevent_env()로 넘긴다. 이 함수를 호출하는 상위 함수
 * (disk_check_events, disk_force_media_change)가 DISK_EVENT_FLAG_UEVENT
 * 플래그를 미리 확인하므로, 이 함수 자체는 그 플래그를 다시 검사하지 않고
 * events와 disk->events의 교집합만 필터링한다.
 * 실행 컨텍스트: 호출자에 따라 워크큐 컨텍스트(disk_check_events ->
 *   disk_events_workfn) 또는 임의의 드라이버 컨텍스트
 *   (disk_force_media_change).
 * 호출자: disk_check_events(), disk_force_media_change().
 * 피호출자: kobject_uevent_env() - kobject/sysfs 코어가 netlink를 통해
 *   udev 등 사용자 공간 데몬에게 uevent를 전달.
 * 에러 경로: 해당하는 이벤트가 하나도 없으면(nr_events == 0) uevent를 아예
 *   보내지 않는다(불필요한 이벤트 폭주 방지).
 *
 * 호출 체인:
 *   disk_check_events / disk_force_media_change -> [disk_event_uevent]
 *   -> kobject_uevent_env
 */
static void disk_event_uevent(struct gendisk *disk, unsigned int events)
{
	char *envp[ARRAY_SIZE(disk_uevents) + 1] = { }; /* [한국어] kobject_uevent_env에 넘길 NULL 종료 문자열 배열 - +1은 마지막 NULL 종료자를 위한 여유 슬롯, 중괄호 초기화로 전체 NULL 초기화 */
	int nr_events = 0, i; /* [한국어] nr_events: envp에 실제로 채워 넣은 문자열 개수, i: 비트 인덱스 루프 변수 */

	for (i = 0; i < ARRAY_SIZE(disk_uevents); i++) /* [한국어] 이 파일이 아는 모든 이벤트 종류(현재 2가지)를 비트 인덱스로 순회 */
		if (events & disk->events & (1 << i)) /* [한국어] "이번에 실제 발생"했고 동시에 "이 디스크가 지원한다고 선언"한 이벤트 비트만 선택(둘 다 아니면 통지 안 함) */
			envp[nr_events++] = disk_uevents[i]; /* [한국어] 해당 비트에 대응하는 "KEY=1" 형태 문자열을 envp에 채우고 인덱스 전진 */

	if (nr_events) /* [한국어] 실제로 통지할 이벤트가 하나라도 있었다면 */
		kobject_uevent_env(&disk_to_dev(disk)->kobj, KOBJ_CHANGE, envp); /* [한국어] 이 gendisk에 대응하는 struct device의 kobject를 통해 KOBJ_CHANGE 타입 uevent를 envp 환경변수와 함께 netlink로 브로드캐스트 */
}

/*
 * [한국어]
 * disk_check_events - 드라이버 콜백으로 실제 이벤트를 확인하고, pending에
 * 누적한 뒤 다음 폴을 예약, 필요하면 uevent까지 통지
 *
 * @ev: 대상 disk_events
 * @clearing_ptr: 드라이버에 클리어를 요청할 이벤트 마스크를 가리키는
 *   포인터(입출력 겸용) - 함수 종료 시 이번에 요청 완료된 비트만큼 차감된
 *   값으로 갱신됨
 * @return: 없음
 *
 * 이 파일의 이벤트 폴링에서 실제로 하드웨어/드라이버와 상호작용하는 유일한
 * 지점이다. disk->fops->check_events()를 호출해 현재 이벤트 상태를 받아온
 * 뒤, 이미 pending에 있던 것과 중복되지 않는 "새로" 감지된 이벤트만
 * ev->pending에 추가한다. 그런 다음 다음 폴링 시각을 계산해 필요하면(차단
 * 중이 아니고 주기가 0보다 크면) 다음 delayed_work를 예약한다. 마지막으로
 * MEDIA_CHANGE가 감지되면 diskseq(디스크 인스턴스 시퀀스 번호)를 증가시켜
 * "이전 미디어와는 다른 인스턴스"임을 알리고, UEVENT 플래그가 켜져 있으면
 * disk_event_uevent()로 사용자 공간에 통지한다.
 * 이 함수는 disk_events_workfn()(주기 폴링 경로)과 disk_clear_events()
 * (동기 확인 경로) 양쪽에서 공유되므로, clearing_ptr을 파라미터로 받아
 * 두 호출자가 서로 다른 저장소(ev->clearing 자체 vs 지역 스택 변수)를
 * 넘길 수 있게 되어 있다.
 * 실행 컨텍스트: 워크큐 워커 스레드(disk_events_workfn 경유) 또는
 *   disk_clear_events()를 호출한 프로세스 컨텍스트. disk->fops->
 *   check_events() 콜백은 드라이버 구현에 따라 잠들 수 있다(예: I/O를
 *   통한 미디어 상태 질의).
 * 호출자: disk_events_workfn(), disk_clear_events().
 * 피호출자: disk->fops->check_events(), disk_events_poll_jiffies(),
 *   queue_delayed_work(), inc_diskseq(), disk_event_uevent().
 * 에러 경로: 별도 없음 - check_events() 반환값 자체가 "감지된 이벤트
 *   없음(0)"을 포함해 항상 유효한 비트마스크로 취급된다.
 *
 * 호출 체인:
 *   disk_events_workfn / disk_clear_events -> [disk_check_events]
 *   -> disk->fops->check_events / disk_event_uevent
 */
static void disk_check_events(struct disk_events *ev,
			      unsigned int *clearing_ptr)
{
	struct gendisk *disk = ev->disk; /* [한국어] 이 이벤트 상태가 속한 gendisk - fops/이벤트 플래그 접근용 */
	unsigned int clearing = *clearing_ptr; /* [한국어] 호출자가 넘긴 클리어 요청 마스크를 지역 변수로 스냅샷(락 없이 드라이버 호출 인자로 쓰기 위함) */
	unsigned int events; /* [한국어] 드라이버 check_events()가 반환할 "현재 감지된 이벤트" 비트마스크 */
	unsigned long intv; /* [한국어] 다음 폴링까지의 간격(jiffies) */

	/* check events */
	events = disk->fops->check_events(disk, clearing); /* [한국어] 드라이버 콜백 호출 - clearing에 포함된 이벤트는 드라이버가 내부 상태를 리셋해도 된다는 힌트로 사용하고, 현재 감지된 전체 이벤트 비트마스크를 반환받음 */

	/* accumulate pending events and schedule next poll if necessary */
	spin_lock_irq(&ev->lock); /* [한국어] pending/clearing/block 필드 갱신을 인터럽트로부터 보호 */

	events &= ~ev->pending; /* [한국어] 이미 pending에 들어가 아직 소비되지 않은 이벤트는 중복 반영하지 않도록 제거 */
	ev->pending |= events; /* [한국어] 새로 감지된(중복 아닌) 이벤트만 pending에 누적 */
	*clearing_ptr &= ~clearing; /* [한국어] 이번에 드라이버에 클리어 요청을 완료한 비트만큼 clearing 요청 마스크에서 제거(호출자에게 "요청 처리됨"을 알림) */

	intv = disk_events_poll_jiffies(disk); /* [한국어] 다음 폴 주기를 다시 계산(사용자가 그 사이 poll_msecs를 바꿨을 수도 있으므로 매번 재계산) */
	if (!ev->block && intv) /* [한국어] 현재 차단 중이 아니고(폴링이 허용된 상태) 계산된 주기가 0보다 크면(폴링이 실제로 필요하면) */
		queue_delayed_work(system_freezable_power_efficient_wq,
				&ev->dwork, intv); /* [한국어] intv 만큼 뒤에 다시 disk_events_workfn이 실행되도록 다음 폴을 예약 */

	spin_unlock_irq(&ev->lock); /* [한국어] pending/clearing 갱신 완료, 잠금 해제 */

	if (events & DISK_EVENT_MEDIA_CHANGE) /* [한국어] 이번에 미디어 변경이 감지되었다면 */
		inc_diskseq(disk); /* [한국어] 디스크 시퀀스 번호를 증가시켜, 이전 미디어와 이번 미디어를 사용자 공간이 구분할 수 있게 함 */

	if (disk->event_flags & DISK_EVENT_FLAG_UEVENT) /* [한국어] 이 디스크가 이벤트를 uevent로 사용자 공간에 통지하도록 설정되어 있다면 */
		disk_event_uevent(disk, events); /* [한국어] 이번에 감지된 이벤트를 KOBJ_CHANGE uevent로 통지 */
}

/**
 * disk_clear_events - synchronously check, clear and return pending events
 * @disk: disk to fetch and clear events from
 * @mask: mask of events to be fetched and cleared
 *
 * Disk events are synchronously checked and pending events in @mask
 * are cleared and returned.  This ignores the block count.
 *
 * CONTEXT:
 * Might sleep.
 */
/*
 * [한국어]
 * disk_clear_events - 이벤트를 동기적으로 확인/클리어하고, mask에 해당하는
 * pending 이벤트를 즉시 반환
 *
 * @disk: 대상 gendisk
 * @mask: 조회 및 클리어 대상 이벤트 비트마스크
 * @return: mask 중 실제로 pending 상태였던 이벤트 비트마스크(호출 후 그만큼
 *   ev->pending에서 제거됨)
 *
 * 다음 정기 폴 주기를 기다리지 않고 지금 당장 드라이버에게 이벤트 상태를
 * 물어봐야 하는 경로(예: disk_check_media_change())를 위한 동기 인터페이스다.
 * block 카운트는 무시한다는 커널독 설명대로, disk_block_events()로 폴링이
 * 차단되어 있어도 이 함수는 자체적으로 disk_block_events()/
 * __disk_unblock_events()를 짝지어 호출해 동기 체크를 하는 동안에는 배경
 * 폴링 워크가 끼어들지 않도록 만든 뒤 disk_check_events()를 직접 호출한다.
 * mask와 기존 ev->clearing을 스택 지역 변수(clearing)에 합쳐 두는 이유는,
 * disk_check_events() 실행 도중 다른 스레드의 disk_flush_events()가
 * ev->clearing을 다시 채우더라도(경쟁) 이번 호출에서 요청한 클리어 범위가
 * 유실되지 않게 하기 위함이다. 함수 말미의 WARN_ON_ONCE(clearing & mask)는
 * 이번에 클리어를 요청했는데 아직도 미완료로 남아있는 비정상 상황을
 * 잡아낸다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 잠들 수 있음(disk_block_events가
 *   뮤텍스와 cancel_delayed_work_sync를 사용) - 커널독 "CONTEXT: Might
 *   sleep"과 일치.
 * 호출자: disk_check_media_change().
 * 피호출자: disk_block_events(), disk_check_events(), __disk_unblock_events().
 * 에러 경로: disk->ev가 NULL이면(이벤트 미사용 디바이스) 0(이벤트 없음)을
 *   즉시 반환.
 *
 * 호출 체인:
 *   disk_check_media_change -> [disk_clear_events] -> disk_block_events
 *   -> disk_check_events -> __disk_unblock_events
 */
static unsigned int disk_clear_events(struct gendisk *disk, unsigned int mask)
{
	struct disk_events *ev = disk->ev; /* [한국어] 이 디스크의 이벤트 상태 - NULL이면 이벤트 미사용 */
	unsigned int pending; /* [한국어] 최종적으로 반환할 "mask 중 실제 pending이었던" 이벤트 비트마스크 */
	unsigned int clearing = mask; /* [한국어] 이번에 드라이버에 클리어 요청할 마스크의 지역 스냅샷 - 우선 호출자가 요청한 mask로 초기화 */

	if (!ev) /* [한국어] 이벤트 프레임워크 미사용 디바이스 */
		return 0; /* [한국어] 항상 "이벤트 없음"으로 취급 */

	disk_block_events(disk); /* [한국어] 배경 폴링 워크가 지금부터 이 동기 체크와 겹쳐 실행되지 않도록 차단(및 이미 실행 중이던 워크의 완료 대기) */

	/*
	 * store the union of mask and ev->clearing on the stack so that the
	 * race with disk_flush_events does not cause ambiguity (ev->clearing
	 * can still be modified even if events are blocked).
	 */
	spin_lock_irq(&ev->lock); /* [한국어] ev->clearing을 읽고 리셋하는 구간을 보호 */
	clearing |= ev->clearing; /* [한국어] 이미 다른 경로(disk_flush_events)가 요청해 둔 클리어 비트까지 이번 요청에 합침 - 누락 방지 */
	ev->clearing = 0; /* [한국어] 합쳐서 지역 변수로 옮겼으니 원본은 리셋(이번 disk_check_events 호출이 책임지고 처리) */
	spin_unlock_irq(&ev->lock); /* [한국어] 잠금 해제 */

	disk_check_events(ev, &clearing); /* [한국어] 실제 드라이버 콜백 호출 및 pending 갱신 - clearing은 함수 내부에서 처리 완료분만큼 차감되어 갱신됨 */
	/*
	 * if ev->clearing is not 0, the disk_flush_events got called in the
	 * middle of this function, so we want to run the workfn without delay.
	 */
	__disk_unblock_events(disk, ev->clearing ? true : false); /* [한국어] 차단 해제 - 그 사이 disk_flush_events가 다시 ev->clearing을 채워놓았다면(경쟁 발생) 지연 없이 즉시 재확인 워크를 예약(true), 아니면 평소 주기로(false) */

	/* then, fetch and clear pending events */
	spin_lock_irq(&ev->lock); /* [한국어] pending을 읽고 소비하는 구간을 보호 */
	pending = ev->pending & mask; /* [한국어] 원래 호출자가 관심 있던 mask 범위 내에서, 지금까지 쌓인 pending 값을 추출 */
	ev->pending &= ~mask; /* [한국어] 추출한 만큼 pending에서 제거(이번 호출로 소비 처리) */
	spin_unlock_irq(&ev->lock); /* [한국어] 잠금 해제 */
	WARN_ON_ONCE(clearing & mask); /* [한국어] 방금 disk_check_events가 처리했어야 할 mask 범위의 클리어 요청이 아직도 남아있다면(드라이버가 요청을 무시) 버그로 경고 */

	return pending; /* [한국어] mask 중 실제로 대기 중이었던 이벤트를 호출자에게 반환 */
}

/**
 * disk_check_media_change - check if a removable media has been changed
 * @disk: gendisk to check
 *
 * Returns %true and marks the disk for a partition rescan whether a removable
 * media has been changed, and %false if the media did not change.
 */
/*
 * [한국어]
 * disk_check_media_change - 탈착형 미디어가 교체되었는지 확인하고, 교체되었다면
 * 파티션 재스캔 플래그를 세움
 *
 * @disk: 대상 gendisk
 * @return: 미디어가 교체되었으면 true(GD_NEED_PART_SCAN도 함께 설정됨),
 *   아니면 false
 *
 * disk_clear_events()를 MEDIA_CHANGE와 EJECT_REQUEST 두 이벤트 모두에 대해
 * 동기적으로 호출한 뒤, 그중 MEDIA_CHANGE가 실제로 있었는지만 검사한다.
 * EJECT_REQUEST를 함께 clear 대상에 넣는 이유는 미디어 변경 여부를 물을 때
 * eject 관련 pending 상태도 함께 소비해 두 이벤트가 서로 얽혀 중복 보고되지
 * 않도록 하기 위함이다(EJECT_REQUEST 자체의 결과값은 이 함수에서 사용하지
 * 않는다). 미디어 변경이 확인되면 GD_NEED_PART_SCAN 비트를 disk->state에
 * 세팅해, 호출자 또는 이후 open 경로가 파티션 테이블을 다시 읽도록
 * 유도한다(단, 실제 재스캔은 이 함수의 책임이 아니라 커널독 설명대로
 * 호출자가 별도로 처리해야 한다).
 * 실행 컨텍스트: disk_clear_events()를 호출하므로 프로세스 컨텍스트,
 *   잠들 수 있음.
 * 호출자: 블록 디바이스 open 경로, ioctl(BLKRRPART 등)에서 지금 미디어가
 *   바뀌었는지 확인이 필요한 지점(EXPORT_SYMBOL로 공개된 안정 API).
 * 피호출자: disk_clear_events(), set_bit().
 * 에러 경로: 없음(항상 true/false 중 하나를 반환).
 *
 * 호출 체인:
 *   (open/ioctl 경로) -> [disk_check_media_change] -> disk_clear_events
 *   -> disk_block_events -> disk_check_events
 */
bool disk_check_media_change(struct gendisk *disk)
{
	unsigned int events; /* [한국어] disk_clear_events()가 돌려주는, 실제로 pending이었던 이벤트 비트마스크 */

	events = disk_clear_events(disk, DISK_EVENT_MEDIA_CHANGE | /* [한국어] MEDIA_CHANGE와 EJECT_REQUEST 두 이벤트를 함께 동기적으로 확인/클리어 요청(첫 줄) */
				   DISK_EVENT_EJECT_REQUEST); /* [한국어] 요청 마스크의 나머지 절반(EJECT_REQUEST) - 두 줄에 걸친 하나의 호출 */
	if (events & DISK_EVENT_MEDIA_CHANGE) { /* [한국어] 이번 확인에서 실제로 미디어 변경이 있었다면 */
		set_bit(GD_NEED_PART_SCAN, &disk->state); /* [한국어] disk->state에 "파티션 재스캔 필요" 비트를 원자적으로 세팅 - 이후 open 경로가 이 비트를 보고 재스캔 수행 */
		return true; /* [한국어] 호출자에게 미디어 변경이 있었음을 알림 */
	}
	return false; /* [한국어] 미디어 변경이 없었음(EJECT_REQUEST만 있었거나 아무 이벤트도 없었던 경우 포함) */
}
EXPORT_SYMBOL(disk_check_media_change); /* [한국어] 모듈/드라이버가 링크할 수 있도록 심볼 공개(GPL 제한 없음) - 파일시스템/블록 코어 등 다양한 호출자가 사용 */

/**
 * disk_force_media_change - force a media change event
 * @disk: the disk which will raise the event
 *
 * Should be called when the media changes for @disk.  Generates a uevent
 * and attempts to free all dentries and inodes and invalidates all block
 * device page cache entries in that case.
 *
 * Callers that need a partition re-scan should arrange for one explicitly.
 */
/*
 * [한국어]
 * disk_force_media_change - 폴링 결과와 무관하게 강제로 media change 이벤트를
 * 발생시키고 관련 캐시를 무효화
 *
 * @disk: 대상 gendisk
 * @return: 없음
 *
 * disk_check_events() 경로(드라이버 콜백을 통한 감지)를 거치지 않고,
 * 드라이버가 미디어가 바뀌었다는 것을 이미 확실히 알고 있는 상황(예:
 * 컨트롤러 리셋, 강제 이젝트, hot-remove 처리)에서 직접 부르는 헬퍼다.
 * disk_event_uevent()로 사용자 공간에 KOBJ_CHANGE를 통지하고,
 * inc_diskseq()로 디스크 인스턴스 시퀀스를 증가시킨 뒤, bdev_mark_dead()로
 * 해당 block_device의 열려 있는 dentry/inode/페이지 캐시 항목을 모두
 * 무효화한다. 커널독에 명시된 대로 파티션 재스캔 자체는 이 함수의 책임이
 * 아니며, 필요하다면 호출자가 별도로 트리거해야 한다.
 * 실행 컨텍스트: 드라이버 호출 컨텍스트에 의존(대개 프로세스 컨텍스트로
 *   가정 - bdev_mark_dead()가 락을 잡고 캐시를 정리하므로 잠들 수 있음).
 * 호출자: 드라이버가 미디어 강제 교체를 알려야 하는 지점(EXPORT_SYMBOL_GPL로
 *   공개된 API, GPL 모듈만 사용 가능).
 * 피호출자: disk_event_uevent(), inc_diskseq(), bdev_mark_dead().
 * 에러 경로: 없음(반환값 없는 통지성 함수).
 *
 * 호출 체인:
 *   (드라이버의 강제 미디어 변경 처리) -> [disk_force_media_change]
 *   -> disk_event_uevent / bdev_mark_dead
 */
void disk_force_media_change(struct gendisk *disk)
{
	disk_event_uevent(disk, DISK_EVENT_MEDIA_CHANGE); /* [한국어] 폴링 결과와 무관하게 무조건 MEDIA_CHANGE uevent를 사용자 공간에 통지 */
	inc_diskseq(disk); /* [한국어] 디스크 인스턴스 시퀀스 번호 증가 - 이전 미디어와 구분되는 새 인스턴스로 표시 */
	bdev_mark_dead(disk->part0, true); /* [한국어] 이 디스크의 대표 block_device(part0, 파티션 없는 전체 디바이스)를 죽은 것으로 표시해 페이지 캐시/버퍼를 무효화 - true 인자는 표면적으로 "동기적으로 처리"를 의미(구현은 block/bdev.c) */
}
EXPORT_SYMBOL_GPL(disk_force_media_change); /* [한국어] GPL 라이선스 모듈에만 공개되는 심볼로 export - 미디어 강제 교체는 코어 정책에 가까워 GPL 제한을 둠 */

/*
 * Separate this part out so that a different pointer for clearing_ptr can be
 * passed in for disk_clear_events.
 */
/*
 * [한국어]
 * disk_events_workfn - 워크큐가 주기적으로(또는 즉시) 실행하는 폴링 워크 콜백
 *
 * @work: 워크큐 코어가 넘겨주는 struct work_struct 포인터(내부적으로
 *   struct delayed_work에 embed된 work 필드)
 * @return: 없음
 *
 * INIT_DELAYED_WORK(&ev->dwork, disk_events_workfn)로 등록된 콜백으로,
 * queue_delayed_work()/mod_delayed_work()에 의해 예약된 시각이 되면
 * system_freezable_power_efficient_wq의 워커 스레드에서 실행된다.
 * to_delayed_work()/container_of()로 원래의 struct disk_events를 역산해낸
 * 뒤, disk_check_events(ev, &ev->clearing)를 호출한다 - 여기서
 * &ev->clearing을 그대로 넘기는 것이 disk_clear_events()가 스택 지역
 * 변수를 넘기는 것과 다른 점이며, 주기 폴링 경로에서는 이번에 처리하지
 * 못한 클리어 요청이 ev->clearing에 그대로 남아 다음 폴 때 다시 시도된다.
 * 실행 컨텍스트: 워크큐 워커 스레드(프로세스 컨텍스트, freezable하여
 *   시스템 suspend 시 함께 멈춤, power_efficient이므로 타이머 병합으로
 *   wakeup 최소화).
 * 호출자: 커널 워크큐 코어(직접 호출되지 않고 queue_delayed_work()가
 *   만료될 때 스케줄러가 호출).
 * 피호출자: disk_check_events().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   (워크큐 코어의 지연 타이머 만료) -> [disk_events_workfn]
 *   -> disk_check_events -> disk->fops->check_events
 */
static void disk_events_workfn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work); /* [한국어] embedded work_struct 포인터로부터 바깥의 delayed_work 포인터를 offset 계산으로 복원 */
	struct disk_events *ev = container_of(dwork, struct disk_events, dwork); /* [한국어] delayed_work가 embed된 struct disk_events의 시작 주소를 container_of 관용구로 역산 */

	disk_check_events(ev, &ev->clearing); /* [한국어] 실제 이벤트 확인/누적/다음 폴 예약 수행 - clearing_ptr로 ev->clearing 자체의 주소를 넘겨 미완료 클리어 요청이 구조체에 영구 보존되게 함 */
}

/*
 * A disk events enabled device has the following sysfs nodes under
 * its /sys/block/X/ directory.
 *
 * events		: list of all supported events
 * events_async		: list of events which can be detected w/o polling
 *			  (always empty, only for backwards compatibility)
 * events_poll_msecs	: polling interval, 0: disable, -1: system default
 */
/*
 * [한국어]
 * __disk_events_show - 이벤트 비트마스크를 사람이 읽을 수 있는 문자열로 변환
 *
 * @events: 문자열로 변환할 이벤트 비트마스크
 * @buf: 결과 문자열을 쓸 출력 버퍼(sysfs가 제공하는 PAGE_SIZE 버퍼로 가정)
 * @return: buf에 쓴 바이트 수(개행 포함), sprintf 반환값 누적
 *
 * disk_events_strs[]에 정의된 이름("media_change", "eject_request")을
 * 비트 인덱스 순서대로 공백으로 구분해 나열한다. disk_events_show()(아래,
 * "events" 속성 자체)에서 이 함수를 호출해 disk->events(지원 이벤트 목록)를
 * 문자열화하는 데 재사용한다.
 * 실행 컨텍스트: sysfs read 시스템 콜을 처리하는 프로세스 컨텍스트.
 * 호출자: disk_events_show().
 * 피호출자: sprintf().
 * 에러 경로: 없음(버퍼 오버플로는 sysfs가 PAGE_SIZE 버퍼를 보장하고 이벤트
 *   종류가 소수이므로 실질적으로 발생하지 않음).
 *
 * 호출 체인:
 *   disk_events_show -> [__disk_events_show] -> sprintf
 */
static ssize_t __disk_events_show(unsigned int events, char *buf)
{
	const char *delim = ""; /* [한국어] 첫 항목 앞에는 구분자를 넣지 않기 위한 상태 변수 - 첫 출력 후 공백 문자로 바뀜 */
	ssize_t pos = 0; /* [한국어] buf에 현재까지 쓴 바이트 오프셋(누적 길이) */
	int i; /* [한국어] 이벤트 비트 인덱스 루프 변수 */

	for (i = 0; i < ARRAY_SIZE(disk_events_strs); i++) /* [한국어] 이 파일이 아는 모든 이벤트 이름 슬롯을 순회 */
		if (events & (1 << i)) { /* [한국어] i번째 비트가 events 마스크에 켜져 있으면(해당 이벤트가 목록에 포함) */
			pos += sprintf(buf + pos, "%s%s", /* [한국어] 이전까지 쓴 위치(buf+pos) 뒤에 구분자+이벤트이름을 이어붙임(첫 줄) */
				       delim, disk_events_strs[i]); /* [한국어] 두 번째 서식 인자들 - 반환된 길이만큼 pos 전진 */
			delim = " "; /* [한국어] 다음 항목부터는 공백으로 구분하도록 갱신 */
		}
	if (pos) /* [한국어] 하나라도 항목을 썼다면(pos가 0보다 크면) */
		pos += sprintf(buf + pos, "\n"); /* [한국어] sysfs 텍스트 파일 관례대로 마지막에 개행 문자 추가 */
	return pos; /* [한국어] 최종적으로 buf에 쓴 총 바이트 수 반환(sysfs read 크기로 사용됨) */
}

/*
 * [한국어]
 * disk_events_show - sysfs "events" 속성(읽기 전용)의 show 콜백
 *
 * @dev: /sys/block/<disk>/ 디렉터리에 대응하는 struct device
 * @attr: 이 속성 자체를 가리키는 device_attribute(사용하지 않음)
 * @buf: 결과를 쓸 sysfs 제공 출력 버퍼
 * @return: buf에 쓴 바이트 수, 이벤트 통지 기능이 꺼져 있으면 0
 *
 * DEVICE_ATTR(events, 0444, disk_events_show, NULL)로 등록되어 사용자가
 * /sys/block/<disk>/events를 읽으면 호출된다. DISK_EVENT_FLAG_UEVENT가
 * 꺼져 있는 디바이스는 애초에 이벤트를 사용자 공간에 보고하지 않으므로 빈
 * 문자열(0바이트)을 반환해 지원 목록 없음으로 보이게 한다.
 * 실행 컨텍스트: sysfs read를 처리하는 프로세스 컨텍스트.
 * 호출자: sysfs/kobject 코어(device_attribute의 show 콜백 디스패치).
 * 피호출자: dev_to_disk(), __disk_events_show().
 * 에러 경로: 별도 errno 없음(항상 0 이상의 길이 반환).
 *
 * 호출 체인:
 *   (sysfs read) -> [disk_events_show] -> __disk_events_show
 */
static ssize_t disk_events_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] sysfs가 넘긴 struct device로부터 원래의 gendisk를 역산(container_of 기반) */

	if (!(disk->event_flags & DISK_EVENT_FLAG_UEVENT)) /* [한국어] 이 디스크가 uevent 통지를 사용하지 않도록 설정되어 있다면 */
		return 0; /* [한국어] 지원 이벤트가 없는 것처럼 빈 문자열(길이 0) 반환 */
	return __disk_events_show(disk->events, buf); /* [한국어] disk->events(이 디스크가 지원한다고 선언한 이벤트 목록)를 문자열로 변환해 반환 */
}

/*
 * [한국어]
 * disk_events_async_show - sysfs "events_async" 속성(읽기 전용)의 show 콜백
 *
 * @dev: 대상 struct device(사용하지 않음)
 * @attr: 속성 서술자(사용하지 않음)
 * @buf: 출력 버퍼(사용하지 않음)
 * @return: 항상 0
 *
 * 과거에는 폴링 없이도 감지 가능한 이벤트 목록을 보여주는 속성이었으나,
 * 현재 커널에는 그런 비동기 감지 메커니즘이 없어 항상 빈 목록(0바이트)만
 * 반환한다. 원문 주석에도 명시되어 있듯 이 속성은 오직 하위 호환
 * (backwards compatibility)만을 위해 남아 있다 - 이 속성을 읽는 구식
 * 사용자 공간 도구가 있어도 sysfs 파일 자체가 사라지지 않도록 유지한다.
 * 실행 컨텍스트: sysfs read를 처리하는 프로세스 컨텍스트.
 * 호출자: sysfs/kobject 코어(DEVICE_ATTR(events_async, ...)의 show 콜백).
 * 피호출자: 없음.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   (sysfs read) -> [disk_events_async_show]
 */
static ssize_t disk_events_async_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return 0; /* [한국어] 항상 빈 목록 - 비동기 감지 이벤트는 더 이상 존재하지 않음(하위 호환용 스텁) */
}

/*
 * [한국어]
 * disk_events_poll_msecs_show - sysfs "events_poll_msecs" 속성의 show 콜백
 *
 * @dev: /sys/block/<disk>/ 대응 struct device
 * @attr: 속성 서술자(사용하지 않음)
 * @buf: 출력 버퍼
 * @return: buf에 쓴 바이트 수
 *
 * disk->ev가 아예 없는(이벤트 미사용) 디바이스에서는 -1(시스템 기본값
 * 사용 중이라는 의미의 관례값)을 출력하고, disk->ev가 있으면 실제
 * poll_msecs 값을 그대로 십진수로 출력한다.
 * 실행 컨텍스트: sysfs read를 처리하는 프로세스 컨텍스트.
 * 호출자: sysfs/kobject 코어.
 * 피호출자: dev_to_disk(), sprintf().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   (sysfs read) -> [disk_events_poll_msecs_show]
 */
static ssize_t disk_events_poll_msecs_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] struct device로부터 gendisk 역산 */

	if (!disk->ev) /* [한국어] 이 디바이스가 이벤트 프레임워크 자체를 쓰지 않는다면(disk->ev == NULL) */
		return sprintf(buf, "-1\n"); /* [한국어] "설정 불가/시스템 기본값" 의미의 관례적인 -1을 출력하고 개행 추가 */
	return sprintf(buf, "%ld\n", disk->ev->poll_msecs); /* [한국어] 이 디바이스에 실제 설정된 폴 주기(ms)를 그대로 출력하고 개행 추가 - -1이면 전역 기본값 사용 중을 의미 */
}

/*
 * [한국어]
 * disk_events_poll_msecs_store - sysfs "events_poll_msecs" 속성의 store(쓰기)
 * 콜백 - 사용자가 이 디바이스 전용 폴 주기를 직접 설정
 *
 * @dev: 대상 struct device
 * @attr: 속성 서술자(사용하지 않음)
 * @buf: 사용자 공간이 write한 문자열(정수 하나를 담고 있어야 함)
 * @count: buf의 바이트 길이
 * @return: 성공 시 count(정상 소비한 바이트 수, sysfs 관례), 실패 시 음수 errno
 *
 * 입력값을 파싱해 유효성(0 이상 또는 정확히 -1)을 검사한 뒤, 폴링을 잠시
 * disk_block_events()로 멈추고 poll_msecs를 갱신하고 나서
 * __disk_unblock_events(disk, true)로 즉시 한 번 재확인을 트리거하며 폴링을
 * 재개한다 - "블록 후 갱신 후 즉시 체크로 재개"라는 패턴은
 * disk_events_poll_msecs_store 특유의 것으로, 사용자가 주기를 줄였을 때
 * 즉각 반영되는 것처럼 보이게 한다.
 * 실행 컨텍스트: sysfs write를 처리하는 프로세스 컨텍스트, 잠들 수 있음
 *   (disk_block_events가 뮤텍스/취소 대기를 사용).
 * 호출자: sysfs/kobject 코어.
 * 피호출자: dev_to_disk(), sscanf(), disk_block_events(),
 *   __disk_unblock_events().
 * 에러 경로: count==0이거나 sscanf 파싱 실패 시 -EINVAL, 파싱된 값이
 *   음수이면서 -1도 아니면 -EINVAL, disk->ev가 NULL(이벤트 미사용
 *   디바이스)이면 -ENODEV.
 *
 * 호출 체인:
 *   (sysfs write) -> [disk_events_poll_msecs_store]
 *   -> disk_block_events -> __disk_unblock_events
 */
static ssize_t disk_events_poll_msecs_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] struct device로부터 gendisk 역산 */
	long intv; /* [한국어] 사용자가 입력한 새 폴 주기(ms)를 파싱해 담을 변수 */

	if (!count || !sscanf(buf, "%ld", &intv)) /* [한국어] 빈 입력이거나(count==0), 정수 하나로 파싱되지 않으면 */
		return -EINVAL; /* [한국어] 잘못된 인자로 거부 */

	if (intv < 0 && intv != -1) /* [한국어] 음수인데 유일하게 허용되는 특수값 -1(시스템 기본값 사용)도 아니라면 */
		return -EINVAL; /* [한국어] 허용되지 않는 값으로 거부(0 이상 또는 -1만 허용) */

	if (!disk->ev) /* [한국어] 이 디바이스가 애초에 이벤트 프레임워크를 쓰지 않는다면(disk_alloc_events가 ev를 만들지 않음) */
		return -ENODEV; /* [한국어] 설정할 대상 자체가 없으므로 "그런 장치 없음"에 준하는 에러 */

	disk_block_events(disk); /* [한국어] poll_msecs를 바꾸는 동안 배경 폴링 워크가 옛 주기로 다시 스케줄되지 않도록 일시 차단 */
	disk->ev->poll_msecs = intv; /* [한국어] 새 폴 주기를 반영 - 이후 disk_events_poll_jiffies()가 이 값을 우선 사용 */
	__disk_unblock_events(disk, true); /* [한국어] 차단 해제와 동시에 새 주기를 반영한 즉시 재확인을 트리거(check_now=true) - 설정이 바로 적용된 것처럼 보이게 함 */
	return count; /* [한국어] sysfs 관례대로 정상 소비한 바이트 수를 그대로 반환해 성공을 알림 */
}

DEVICE_ATTR(events, 0444, disk_events_show, NULL); /* [한국어] "events" 속성 등록: 0444=소유자/그룹/전체 읽기전용, show만 있고 store 없음(쓰기 불가) - dev_attr_events라는 전역 device_attribute를 매크로가 생성 */
DEVICE_ATTR(events_async, 0444, disk_events_async_show, NULL); /* [한국어] "events_async" 속성 등록: 읽기 전용, 항상 빈 목록을 반환하는 하위 호환용 스텁 */
DEVICE_ATTR(events_poll_msecs, 0644, disk_events_poll_msecs_show, /* [한국어] "events_poll_msecs" 속성 등록(첫 줄): 0644=소유자 쓰기+전체 읽기 가능 */
	    disk_events_poll_msecs_store); /* [한국어] show/store 콜백을 모두 지정해 조회/설정 둘 다 지원(두 번째 줄) */

/*
 * The default polling interval can be specified by the kernel
 * parameter block.events_dfl_poll_msecs which defaults to 0
 * (disable).  This can also be modified runtime by writing to
 * /sys/module/block/parameters/events_dfl_poll_msecs.
 */
/*
 * [한국어]
 * disk_events_set_dfl_poll_msecs - 모듈 파라미터
 * block.events_dfl_poll_msecs의 kernel_param_ops.set 콜백
 *
 * @val: 사용자가 write한 문자열(부호 없는 정수)
 * @kp: 이 모듈 파라미터를 서술하는 struct kernel_param
 * @return: 0=성공, param_set_ulong()이 실패했다면 그 음수 errno를 그대로 전달
 *
 * 부팅 커맨드라인(block.events_dfl_poll_msecs=N) 또는 런타임에
 * /sys/module/block/parameters/events_dfl_poll_msecs로 시스템 전역 기본
 * 폴 주기를 바꿀 때 호출된다. 먼저 표준 param_set_ulong()으로
 * disk_events_dfl_poll_msecs 변수 자체를 갱신한 뒤, 전역 disk_events
 * 리스트(disk_events_mutex로 보호)에 등록된 기본값을 쓰는 모든 디스크에
 * 대해 disk_flush_events(ev->disk, 0)를 호출해 새 기본 주기가 즉시 반영된
 * 것처럼 한 번씩 재확인을 트리거한다(mask=0이므로 드라이버에 클리어
 * 요청은 하지 않고 재스케줄만 유발).
 * 실행 컨텍스트: sysfs/모듈 파라미터 write 또는 커널 커맨드라인 파싱
 *   시점(부팅 초기, 이때는 disk_events 리스트가 비어 있어 루프가 즉시
 *   종료).
 * 호출자: 커널 파라미터 코어(module_param_cb로 등록된 set 콜백 디스패치).
 * 피호출자: param_set_ulong(), disk_flush_events().
 * 에러 경로: param_set_ulong()이 실패(예: 파싱 불가)하면 그 errno를 그대로
 *   반환하고 disk_events 리스트 순회는 수행하지 않음.
 *
 * 호출 체인:
 *   (모듈 파라미터 write / 커맨드라인 파싱) -> [disk_events_set_dfl_poll_msecs]
 *   -> param_set_ulong / disk_flush_events
 */
static int disk_events_set_dfl_poll_msecs(const char *val,
					  const struct kernel_param *kp)
{
	struct disk_events *ev; /* [한국어] 전역 리스트 순회용 커서 - list_for_each_entry가 매 반복마다 가리키는 disk_events */
	int ret; /* [한국어] param_set_ulong()의 반환값(성공/실패) 저장 */

	ret = param_set_ulong(val, kp); /* [한국어] 문자열 val을 파싱해 kp가 가리키는 disk_events_dfl_poll_msecs 변수에 반영(커널 파라미터 코어 표준 헬퍼) */
	if (ret < 0) /* [한국어] 파싱 실패(예: 숫자가 아님) */
		return ret; /* [한국어] 값 자체가 갱신되지 않았으므로 리스트 순회 없이 즉시 에러 전파 */

	mutex_lock(&disk_events_mutex); /* [한국어] 전역 disk_events 리스트를 다른 add/del과 경합 없이 순회하기 위해 잠금 */
	list_for_each_entry(ev, &disk_events, node) /* [한국어] 현재 등록된 모든 disk_events(=이벤트 프레임워크를 쓰는 모든 디스크)를 순회 */
		disk_flush_events(ev->disk, 0); /* [한국어] mask=0으로 호출 - 드라이버에 클리어를 요청하지 않고, 새로 바뀐 기본 주기를 반영해 다음 폴을 즉시(차단 중이 아니라면) 재예약만 유도 */
	mutex_unlock(&disk_events_mutex); /* [한국어] 리스트 순회 종료, 잠금 해제 */
	return 0; /* [한국어] 정상 처리 완료 */
}

/*
 * [한국어] events_dfl_poll_msecs 모듈 파라미터의 동작을 정의하는 콜백
 * 테이블 - module_param_cb()에 전달되어 sysfs/커맨드라인 파싱 시 사용된다.
 */
static const struct kernel_param_ops disk_events_dfl_poll_msecs_param_ops = {
	.set	= disk_events_set_dfl_poll_msecs, /* [한국어] write 시 호출될 커스텀 set 콜백 - 값 갱신 후 전체 디스크에 즉시 반영까지 수행 */
	.get	= param_get_ulong, /* [한국어] read 시에는 커스터마이징 없이 표준 unsigned long 출력 헬퍼를 그대로 사용 */
};

#undef MODULE_PARAM_PREFIX /* [한국어] 이 파일 앞부분(다른 헤더)에서 이미 정의되어 있었을 MODULE_PARAM_PREFIX를 해제 - 아래에서 이 파일 전용 접두사로 재정의하기 위한 준비 */
#define MODULE_PARAM_PREFIX	"block." /* [한국어] 이후 module_param_cb() 등이 생성하는 sysfs 경로에 "block." 접두사를 붙임 - 실제 경로는 /sys/module/block/parameters/events_dfl_poll_msecs가 됨(모듈명이 아니라 이 접두사가 경로를 결정) */

module_param_cb(events_dfl_poll_msecs, &disk_events_dfl_poll_msecs_param_ops, /* [한국어] "events_dfl_poll_msecs" 파라미터 등록(첫 줄) - 위에서 정의한 커스텀 set/get 콜백을 사용 */
		&disk_events_dfl_poll_msecs, 0644); /* [한국어] 실제 백킹 변수는 disk_events_dfl_poll_msecs, 권한은 0644(소유자 쓰기+전체 읽기)로 노출(두 번째 줄) */

/*
 * disk_{alloc|add|del|release}_events - initialize and destroy disk_events.
 */
/*
 * [한국어]
 * disk_alloc_events - gendisk에 대한 disk_events 구조체를 할당하고 초기 상태로
 * 세팅(단, 아직 전역 리스트에 등록하거나 폴링을 시작하지는 않음)
 *
 * @disk: 대상 gendisk(fops/events 필드가 이미 설정되어 있어야 함)
 * @return: 0=성공(이벤트 미사용 디바이스이면 ev를 만들지 않고도 0), 음수=errno
 *
 * disk->fops->check_events 콜백이 없거나 disk->events(지원 이벤트 목록)가
 * 0이면, 애초에 이 디바이스는 이벤트 프레임워크를 쓰지 않는 것으로 보고
 * disk->ev를 NULL로 남긴 채 조용히 성공(0) 반환한다 - 이후 이 파일의 거의
 * 모든 다른 함수가 "ev가 NULL이면 반환" 패턴으로 이 상태를 감지해 no-op으로
 * 동작한다. 실제로 이벤트를 쓰는 디바이스라면 disk_events를 kzalloc_obj로
 * 할당하고, 리스트 노드/스핀락/뮤텍스를 초기화하며, block을 1로 시작해
 * 아직 disk_add_events()가 명시적으로 unblock하기 전까지는 폴링 금지 상태로
 * 만든다. poll_msecs는 -1(시스템 기본값 사용)로, dwork는 disk_events_workfn을
 * 콜백으로 하는 지연 워크로 초기화한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(add_disk() 경로), 잠들 수 있음
 *   (kzalloc_obj가 GFP_KERNEL 계열 할당이라 가정).
 * 호출자: add_disk() 계열(block/genhd.c) - gendisk 등록 초기 단계.
 * 피호출자: kzalloc_obj(), spin_lock_init(), mutex_init(),
 *   INIT_LIST_HEAD(), INIT_DELAYED_WORK().
 * 에러 경로: 할당 실패 시 pr_warn()으로 경고 로그를 남기고 -ENOMEM 반환 -
 *   호출자는 이벤트 기능 없이도 디스크 등록 자체는 계속 진행하는 것이
 *   일반적이다.
 *
 * 호출 체인:
 *   add_disk -> [disk_alloc_events] -> kzalloc_obj / INIT_DELAYED_WORK
 */
int disk_alloc_events(struct gendisk *disk)
{
	struct disk_events *ev; /* [한국어] 새로 할당할 disk_events를 가리킬 지역 포인터 */

	if (!disk->fops->check_events || !disk->events) /* [한국어] 드라이버가 이벤트 체크 콜백을 구현하지 않았거나(check_events==NULL), 지원하는 이벤트 종류가 하나도 없다면(events==0) */
		return 0; /* [한국어] 이 디바이스는 이벤트 프레임워크가 필요 없음 - disk->ev는 NULL로 남고 이후 관련 함수들이 모두 no-op으로 동작 */

	ev = kzalloc_obj(*ev); /* [한국어] struct disk_events 하나를 0으로 채워 동적 할당(sizeof(*ev) 자동 추론, 인자 생략 시 GFP_KERNEL 기본 적용 - include/linux/slab.h, 이 저장소 트리에는 헤더 파일 자체가 포함되어 있지 않음) */
	if (!ev) { /* [한국어] 메모리 할당 실패(OOM 등) */
		pr_warn("%s: failed to initialize events\n", disk->disk_name); /* [한국어] 이벤트 기능이 비활성화된 채로 계속 진행됨을 커널 로그로 경고 */
		return -ENOMEM; /* [한국어] 표준 메모리 부족 에러 코드 반환 */
	}

	INIT_LIST_HEAD(&ev->node); /* [한국어] 아직 전역 리스트에 연결되지 않은, 자기 자신을 가리키는 빈 리스트 헤드로 초기화(disk_add_events가 실제 등록) */
	ev->disk = disk; /* [한국어] 역참조 포인터 설정 - 이후 이 disk_events가 어떤 gendisk 소속인지 알아내는 유일한 통로 */
	spin_lock_init(&ev->lock); /* [한국어] pending/clearing/block을 보호할 스핀락 초기화 */
	mutex_init(&ev->block_mutex); /* [한국어] disk_block_events()의 취소 절차를 직렬화할 뮤텍스 초기화 */
	ev->block = 1; /* [한국어] 초기값 1 - disk_add_events()가 명시적으로 __disk_unblock_events(true)를 호출하기 전까지는 폴링이 시작되지 않도록 차단된 상태로 시작 */
	ev->poll_msecs = -1; /* [한국어] "디바이스별 값 미설정, 시스템 기본값 사용" 의미의 관례값으로 초기화 */
	INIT_DELAYED_WORK(&ev->dwork, disk_events_workfn); /* [한국어] 폴 만료 시 실행될 콜백을 disk_events_workfn으로 등록(아직 큐잉되지는 않음) */

	disk->ev = ev; /* [한국어] 새로 초기화한 구조체를 gendisk에 연결 - 이 대입 이후부터 disk->ev가 NULL이 아니게 되어 다른 함수들이 실제 동작을 수행하기 시작 */
	return 0; /* [한국어] 정상 완료 */
}

/*
 * [한국어]
 * disk_add_events - disk_alloc_events()로 준비된 disk_events를 전역 리스트에
 * 등록하고 실제로 폴링을 시작
 *
 * @disk: 대상 gendisk
 * @return: 없음
 *
 * disk->ev가 없으면(이벤트 미사용 디바이스) 아무 것도 하지 않는다. 있다면
 * disk_events_mutex로 보호되는 전역 disk_events 리스트에 꼬리로 추가해,
 * 이후 disk_events_set_dfl_poll_msecs() 같은 "모든 디스크에 대해" 동작하는
 * 함수가 이 디스크도 순회 대상에 포함하게 만든다. 마지막으로
 * __disk_unblock_events(disk, true)를 호출해, disk_alloc_events()가 1로
 * 세팅해 둔 block 카운트를 0으로 낮추고(정확히 한 번 감소) check_now=true로
 * 즉시 최초의 이벤트 체크를 예약한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(add_disk() 경로).
 * 호출자: add_disk()(block/genhd.c), disk_alloc_events() 바로 다음 단계.
 * 피호출자: list_add_tail(), __disk_unblock_events().
 * 에러 경로: 없음(반환값 없는 등록 절차).
 *
 * 호출 체인:
 *   add_disk -> disk_alloc_events -> [disk_add_events]
 *   -> __disk_unblock_events -> queue_delayed_work
 */
void disk_add_events(struct gendisk *disk)
{
	if (!disk->ev) /* [한국어] disk_alloc_events()가 이벤트 미사용으로 판단해 ev를 만들지 않은 디바이스라면 */
		return; /* [한국어] 등록할 것도, 시작할 폴링도 없으므로 즉시 반환 */

	mutex_lock(&disk_events_mutex); /* [한국어] 전역 리스트를 다른 add/del/순회와 경합 없이 수정하기 위해 잠금 */
	list_add_tail(&disk->ev->node, &disk_events); /* [한국어] 이 디스크의 이벤트 상태를 전역 disk_events 리스트 끝에 연결 - 이제부터 시스템 전역 기본 주기 변경 등의 대상에 포함됨 */
	mutex_unlock(&disk_events_mutex); /* [한국어] 리스트 수정 완료, 잠금 해제 */

	/*
	 * Block count is initialized to 1 and the following initial
	 * unblock kicks it into action.
	 */
	__disk_unblock_events(disk, true); /* [한국어] disk_alloc_events()가 세팅해 둔 block==1을 0으로 낮추고, check_now=true로 최초의 이벤트 체크를 지연 없이 즉시 예약 - 이 호출이 사실상 폴링 시작 스위치 역할 */
}

/*
 * [한국어]
 * disk_del_events - 폴링을 완전히 차단하고 전역 리스트에서 제거
 *
 * @disk: 대상 gendisk
 * @return: 없음
 *
 * del_gendisk() 경로에서 호출되어, 이 디스크가 시스템에서 사라지기 전에
 * 더 이상 이벤트 폴링 워크가 돌지 않도록 disk_block_events()로 확실히
 * 멈춘 뒤(block 카운트를 증가시켜 취소하고, 이후 다시 unblock되지 않으므로
 * 사실상 영구 차단), 전역 리스트에서 list_del_init()으로 제거한다.
 * disk_release_events()가 나중에 이 block 카운트가 정확히 1인지(즉 정확히
 * 한 번의 disk_del_events 차단만 남아 있는지) 검증한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(del_gendisk() 경로), 잠들 수 있음
 *   (disk_block_events 경유).
 * 호출자: del_gendisk()(block/genhd.c) - gendisk 해제의 초기 단계.
 * 피호출자: disk_block_events(), list_del_init().
 * 에러 경로: disk->ev가 NULL이면(이벤트 미사용) 아무 것도 하지 않음.
 *
 * 호출 체인:
 *   del_gendisk -> [disk_del_events] -> disk_block_events
 *   -> cancel_delayed_work_sync
 */
void disk_del_events(struct gendisk *disk)
{
	if (disk->ev) { /* [한국어] 이벤트 프레임워크를 사용 중인 디바이스라면(disk->ev != NULL) */
		disk_block_events(disk); /* [한국어] 진행 중이던/예약된 폴링 워크를 취소하고 완료까지 대기 - 이후 다시 unblock하지 않으므로 사실상 영구 정지 */

		mutex_lock(&disk_events_mutex); /* [한국어] 전역 리스트 수정을 위해 잠금 */
		list_del_init(&disk->ev->node); /* [한국어] 전역 disk_events 리스트에서 이 노드를 제거하고, node 자신은 다시 빈 리스트 헤드로 초기화(재사용/이중 제거 안전) */
		mutex_unlock(&disk_events_mutex); /* [한국어] 리스트 수정 완료, 잠금 해제 */
	}
}

/*
 * [한국어]
 * disk_release_events - disk_alloc_events()로 할당했던 disk_events 메모리를
 * 최종 해제
 *
 * @disk: 대상 gendisk
 * @return: 없음
 *
 * gendisk 자체가 완전히 해제되는 마지막 단계(disk_release())에서 호출된다.
 * disk_del_events()가 이미 block 카운트를 1 증가시켜 놓았어야 하므로(그리고
 * 이 시점까지 다른 unblock이 없었어야 하므로), WARN_ON_ONCE로 block이
 * 정확히 1인지 검증해 짝이 맞지 않는 block/unblock 버그를 조기에 잡아낸다.
 * 검증 후에는 조건 없이 kfree()로 메모리를 반환한다(disk->ev가 NULL이어도
 * kfree(NULL)은 안전하므로 별도 NULL 검사가 필요 없음).
 * 실행 컨텍스트: 프로세스 컨텍스트(gendisk 참조 카운트가 0이 되어 release
 *   콜백이 실행되는 시점).
 * 호출자: disk_release()(block/genhd.c) - gendisk kobject의 최종 release
 *   콜백 경로.
 * 피호출자: kfree().
 * 에러 경로: block 카운트가 1이 아니면 WARN_ON_ONCE가 커널 로그에 경고를
 *   남기지만, 그래도 메모리 해제 자체는 계속 진행한다(메모리 누수 방지가
 *   우선).
 *
 * 호출 체인:
 *   disk_release -> [disk_release_events] -> kfree
 */
void disk_release_events(struct gendisk *disk)
{
	/* the block count should be 1 from disk_del_events() */
	WARN_ON_ONCE(disk->ev && disk->ev->block != 1); /* [한국어] disk->ev가 존재하는데 block이 1이 아니면(disk_del_events가 제대로 호출되지 않았거나 짝이 안 맞는 unblock이 있었으면) 버그로 경고만 남김 - disk->ev가 NULL이면 단락 평가로 뒤 조건은 검사되지 않음 */
	kfree(disk->ev); /* [한국어] disk_alloc_events()가 할당했던 메모리를 반환 - disk->ev가 NULL이어도 kfree(NULL)은 안전하게 무시됨 */
}
