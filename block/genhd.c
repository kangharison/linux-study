// SPDX-License-Identifier: GPL-2.0
/*
 *  gendisk handling
 *
 * Portions Copyright (C) 2020 Christoph Hellwig
 */

/*
 * [한국어 설명] genhd.c — gendisk 생명주기 및 블록 장치 등록 관리
 *
 * === 파일의 역할 ===
 * 이 파일은 struct gendisk의 할당(alloc), 등록(add_disk), 제거(del_gendisk)에
 * 이르는 전체 생명주기를 관리한다. 블록 장치 번호(major/minor) 할당, 파티션 스캔,
 * sysfs/proc 인터페이스 노출, 디스크 이벤트(미디어 변경, 꺼냄) 처리가 이 파일에서
 * 이루어진다. VFS/파일시스템에서 내려온 bio는 request_queue를 통해 gendisk 단위로
 * 라우팅되며, 이 파일은 그 gendisk 객체가 블록 클래스에 나타나고 사라지는 관문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 커널 블록 레이어의 중간 계층에 해당한다. 위로는 VFS(파일시스템)와 파티션 코드,
 * 아래로는 request_queue/blk-mq와 드라이버(NVMe, SCSI 등)를 연결한다.
 * 실행 컨텍스트: 커널 프로세스 컨텍스트 (device_add, sysfs probe, 드라이버 초기화).
 * 주요 호출 체인:
 *   [등록] NVMe 드라이버 → device_add_disk() → add_disk_fwnode() → __add_disk()
 *          → blk_register_queue() + add_disk_final() → bdev_add() + disk_scan_partitions()
 *   [삭제] NVMe 드라이버 → del_gendisk() → __del_gendisk() → blk_unregister_queue()
 *   [sysfs] /sys/block/nvmeXnY/stat → part_stat_show() → part_stat_read_all()
 *   [할당] __blk_alloc_disk() → blk_alloc_queue() + __alloc_disk_node()
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - block/bdev.c         : block_device 할당·해시·inode 관리 (bdev_alloc, bdev_add, bdev_drop)
 *   - block/blk-sysfs.c    : blk_register_queue / blk_unregister_queue (queue sysfs 등록)
 *   - block/blk-mq.c       : blk-mq 큐 초기화·해제 (blk_mq_exit_queue, blk_mq_cancel_work_sync)
 *   - block/partitions/core.c: 파티션 테이블 스캔 (GPT/MBR 해석, drop_partition)
 *   - block/disk-events.c  : 미디어 변경·꺼냄 이벤트 폴링 (disk_add_events, disk_del_events)
 *   - drivers/nvme/host/core.c: NVMe 네임스페이스 생명주기 (add_disk/del_gendisk 호출자)
 * 공유 핵심 자료구조:
 *   - struct gendisk (include/linux/blkdev.h): 블록 장치 전체를 나타내는 중심 객체
 *   - struct request_queue: I/O 요청 큐 (gendisk→queue로 참조)
 *   - struct block_device (part0): gendisk에 포함된 주 블록 장치 노드
 *   - struct blk_major_name: major 번호→드라이버 이름 매핑 해시 테이블 엔트리
 *
 * === 주요 함수/구조체 요약 ===
 * set_capacity()           — gendisk의 논리 용량(섹터 수)을 갱신; BLK_DEV_MAX_SECTORS로 클램프
 * set_capacity_and_notify()— 용량 변경 후 "RESIZE=1" uevent 발생; NVMe namespace resize에 대응
 * device_add_disk()        — gendisk를 커널 장치 계층에 등록하는 공개 API (→ add_disk_fwnode)
 * add_disk_fwnode()        — blk-mq nr_hw_queues 락을 잡고 __add_disk + add_disk_final 호출
 * __add_disk()             — major/minor 할당, device_add, queue·bdi sysfs 등록 수행
 * add_disk_final()         — uevent 허용, 파티션 스캔, bdi limits 적용으로 등록 완료
 * del_gendisk()            — gendisk 제거 공개 API; queue freeze/drain 후 sysfs 링크 제거
 * __del_gendisk()          — del_gendisk 실제 구현; 파티션 drop, queue 정리, device_del 수행
 * blk_mark_disk_dead()     — 갑작스러운 장치 제거 시 GD_DEAD 플래그 설정 및 새 I/O 차단
 * __alloc_disk_node()      — gendisk+part0 할당 및 하위 자원(bdi, part_tbl, blkcg) 초기화
 * __blk_alloc_disk()       — request_queue까지 함께 생성하는 고수준 gendisk 할당 함수
 * put_disk()               — gendisk 참조 카운트 감소; 0이 되면 disk_release() 호출
 * part_stat_show()         — /sys/block/XY/stat: read/write/discard/flush I/O 통계 출력
 * diskstats_show()         — /proc/diskstats 항목 출력; 전체 디스크·파티션 통계 집계
 */
#include <linux/module.h>       /* [한국어] EXPORT_SYMBOL, MODULE_* 매크로 — gendisk API를 다른 모듈에 노출 */
#include <linux/ctype.h>        /* [한국어] isdigit 등 문자 분류 함수 — 장치 이름 파싱에 사용 */
#include <linux/fs.h>           /* [한국어] file_system_type, inode, vfs_* API — sysfs·proc 통합에 필요 */
#include <linux/kdev_t.h>       /* [한국어] dev_t, MAJOR(), MINOR(), MKDEV() 매크로 — 장치 번호 조합 */
#include <linux/kernel.h>       /* [한국어] pr_info, WARN_ON, container_of 등 커널 기본 유틸리티 */
#include <linux/blkdev.h>       /* [한국어] struct gendisk, struct request_queue, blk_* API 핵심 헤더 */
#include <linux/backing-dev.h>  /* [한국어] struct backing_dev_info(bdi) — writeback 인프라 등록·해제 */
#include <linux/init.h>         /* [한국어] __init, subsys_initcall, module_init — 초기화 시점 지정 */
#include <linux/spinlock.h>     /* [한국어] spinlock_t, spin_lock/unlock — major_names_spinlock 보호 */
#include <linux/proc_fs.h>      /* [한국어] proc_create_seq — /proc/diskstats, /proc/partitions 생성 */
#include <linux/seq_file.h>     /* [한국어] seq_file, seq_printf 등 — /proc 출력 포맷팅 인터페이스 */
#include <linux/slab.h>         /* [한국어] kmalloc, kzalloc, kfree — blk_major_name 등 동적 할당 */
#include <linux/kmod.h>         /* [한국어] request_module — 레거시 블록 드라이버 자동 로드 */
#include <linux/major.h>        /* [한국어] BLOCK_EXT_MAJOR 등 major 번호 상수 정의 */
#include <linux/mutex.h>        /* [한국어] mutex_lock/unlock — major_names_lock, open_mutex 보호 */
#include <linux/idr.h>          /* [한국어] IDA(ID Allocator) — ext_devt_ida: 확장 minor 번호 동적 할당 */
#include <linux/log2.h>         /* [한국어] is_power_of_2, roundup_pow_of_two — 크기 정렬 계산 */
#include <linux/pm_runtime.h>   /* [한국어] pm_runtime_set_memalloc_noio — 런타임 PM 메모리 설정 */
#include <linux/badblocks.h>    /* [한국어] badblocks_show/store — /sys/block/XY/badblocks sysfs 구현 */
#include <linux/part_stat.h>    /* [한국어] part_stat_read_all, update_io_ticks — 파티션 I/O 통계 수집 */
#include <linux/blktrace_api.h> /* [한국어] blk_trace_remove, blk_trace_attr_group — I/O 추적 정리 */

#include "blk-throttle.h"  /* [한국어] blk_throtl_cancel_bios — 디스크 제거 시 throttle 바이오 취소 */
#include "blk.h"           /* [한국어] 블록 레이어 내부 헤더 — disk_live, bdev_unhash 등 내부 API */
#include "blk-mq-sched.h"  /* [한국어] blk_mq_sched_* — blk-mq I/O 스케줄러 초기화·해제 */
#include "blk-rq-qos.h"    /* [한국어] rq_qos_exit — 요청 QoS(블록 cgroup throttle 등) 정리 */
#include "blk-cgroup.h"    /* [한국어] blkcg_init_disk, blkcg_exit_disk — 블록 cgroup 연결·해제 */

/* [한국어] /sys/block 하위에 만들어지는 레거시 심볼릭 링크 디렉터리 kobject.
 * 설정자: genhd_device_init()에서 kobject_create_and_add("block", NULL)로 초기화.
 * 읽는 자: __add_disk()가 sysfs_create_link()로 디스크 링크를 이 아래에 만들고,
 *          __del_gendisk()가 sysfs_remove_link()로 제거.
 * 값 범위: NULL(초기화 전) 또는 유효한 kobject 포인터.
 * 동기화: 초기화 이후 읽기만 하며, 교체되지 않으므로 별도 락 불필요. */
static struct kobject *block_depr;

/*
 * Unique, monotonically increasing sequential number associated with block
 * devices instances (i.e. incremented each time a device is attached).
 * Associating uevents with block devices in userspace is difficult and racy:
 * the uevent netlink socket is lossy, and on slow and overloaded systems has
 * a very high latency.
 * Block devices do not have exclusive owners in userspace, any process can set
 * one up (e.g. loop devices). Moreover, device names can be reused (e.g. loop0
 * can be reused again and again).
 * A userspace process setting up a block device and watching for its events
 * cannot thus reliably tell whether an event relates to the device it just set
 * up or another earlier instance with the same name.
 * This sequential number allows userspace processes to solve this problem, and
 * uniquely associate an uevent to the lifetime to a device.
 */
/* [한국어] 블록 장치 인스턴스에 할당되는 단조 증가 일련번호 (diskseq).
 * 설정자: inc_diskseq() → atomic64_inc_return()으로 __alloc_disk_node() 내에서 증가.
 * 읽는 자: block_uevent()가 "DISKSEQ=%llu" 환경변수로 uevent에 포함; diskseq_show()가
 *          /sys/block/XY/diskseq로 노출; 유저스페이스가 장치 재사용을 구분하는 데 사용.
 * 값 범위: 0부터 시작해 장치 연결 시마다 증가, 64비트 범위 내에서 오버플로 없음.
 * 동기화: atomic64_t로 선언되어 64비트 원자 연산 보장; 락 불필요. */
static atomic64_t diskseq;

/* [한국어] 확장 동적 dev_t 할당을 위한 매크로: minor 비트 수(20)만큼의 슬롯을 제공.
 * 현재는 BLOCK_EXT_MAJOR 하나만 사용하므로 minor 공간 전체(약 100만 개)가 이 풀에 속한다. */
/* for extended dynamic devt allocation, currently only one major is used */
#define NR_EXT_DEVT		(1 << MINORBITS)
/* [한국어] 확장 minor 번호 동적 할당 풀.
 * 설정자: blk_alloc_ext_minor()가 ida_alloc_range()로 슬롯을 예약.
 * 읽는 자: blk_free_ext_minor()가 ida_free()로 슬롯 반환.
 * NVMe 드라이버처럼 고정 major 없이 등록되는 장치가 이 IDA를 통해 minor를 얻는다. */
static DEFINE_IDA(ext_devt_ida);

/*
 * [한국어]
 * set_capacity - gendisk의 논리 용량(섹터 수)을 갱신
 *
 * @disk:    대상 gendisk 포인터. NVMe 네임스페이스 1개에 대응하는 블록 장치 객체.
 * @sectors: 설정할 새 섹터 수. 섹터 크기는 항상 512B 단위(struct block_device 기준).
 * @return:  void. BLK_DEV_MAX_SECTORS 초과 시 내부에서 클램프 후 설정.
 *
 * NVMe 네임스페이스의 사용 가능한 논리 블록 수(NSZE/NCAP 필드)가 확정된 후
 * nvme_update_disk_info()가 이 함수를 호출해 상위 블록 레이어에 용량을 전달한다.
 * 파일시스템이 bio를 생성할 때 최종 LBA 범위 검증의 기준이 되며, 0으로 설정하면
 * 모든 쓰기 I/O가 차단된다(__blk_mark_disk_dead에서 활용).
 * 실행 컨텍스트: 프로세스 컨텍스트(드라이버 probe/재검증 경로).
 * 에러 경로: sectors가 BLK_DEV_MAX_SECTORS를 초과하면 pr_warn_once 후 클램프.
 *
 * 호출 체인:
 *   nvme_update_disk_info() → [set_capacity()] → bdev_set_nr_sectors()
 *   __blk_mark_disk_dead()  → [set_capacity(disk, 0)] (용량 0으로 I/O 차단)
 */
void set_capacity(struct gendisk *disk, sector_t sectors)
{
	if (sectors > BLK_DEV_MAX_SECTORS) { /* [한국어] 섹터 수가 커널 최대치(BLK_DEV_MAX_SECTORS)를 초과하면 클램프 */
		pr_warn_once("%s: truncate capacity from %lld to %lld\n",
				disk->disk_name, sectors,
				BLK_DEV_MAX_SECTORS); /* [한국어] 클램프 사실을 커널 로그에 1회만 경고 출력 */
		sectors = BLK_DEV_MAX_SECTORS; /* [한국어] 최대 허용 섹터 수로 강제 제한 */
	}

	bdev_set_nr_sectors(disk->part0, sectors); /* [한국어] part0(전체 디스크 block_device)의 bd_nr_sectors 갱신 */
}
EXPORT_SYMBOL(set_capacity);

/*
 * [한국어]
 * set_capacity_and_notify - 디스크 용량 변경 후 "RESIZE=1" uevent 발생
 *
 * @disk: 대상 gendisk 포인터.
 * @size: 설정할 새 섹터 수.
 * @return: uevent를 발생시키면 true, 그렇지 않으면 false.
 *
 * NVMe 네임스페이스가 온라인 중 동적으로 크기가 변경(namespace resize)되었을 때
 * 호출된다. set_capacity()로 새 용량을 설정한 후, 장치가 user-visible하고 살아있으며
 * 기존/신규 용량 모두 0이 아닌 경우에만 "RESIZE=1" uevent를 발생시킨다.
 * 이 uevent를 udev가 수신하면 파티션 테이블을 재스캔한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(드라이버 재검증 경로).
 * 에러 경로: 조건 불충족 시 false 반환, uevent 없음.
 *
 * 호출 체인:
 *   nvme_update_ns_info() → [set_capacity_and_notify()] → kobject_uevent_env()
 */
/*
 * Set disk capacity and notify if the size is not currently zero and will not
 * be set to zero.  Returns true if a uevent was sent, otherwise false.
 */
bool set_capacity_and_notify(struct gendisk *disk, sector_t size)
{
	sector_t capacity = get_capacity(disk); /* [한국어] 현재 용량을 저장해 변경 여부 비교에 사용 */
	char *envp[] = { "RESIZE=1", NULL };    /* [한국어] uevent 환경변수: RESIZE=1은 파티션 재스캔 신호 */

	set_capacity(disk, size); /* [한국어] 새 섹터 수를 part0에 반영 */

	/*
	 * Only print a message and send a uevent if the gendisk is user visible
	 * and alive.  This avoids spamming the log and udev when setting the
	 * initial capacity during probing.
	 */
	if (size == capacity ||        /* [한국어] 용량이 바뀌지 않으면 uevent 불필요 */
	    !disk_live(disk) ||        /* [한국어] GD_DEAD 또는 미등록 디스크이면 uevent 생략 */
	    (disk->flags & GENHD_FL_HIDDEN)) /* [한국어] hidden 디스크(dm 내부 등)는 uevent 생략 */
		return false;

	pr_info_ratelimited("%s: detected capacity change from %lld to %lld\n",
		disk->disk_name, capacity, size); /* [한국어] 용량 변경 사실을 rate-limited로 커널 로그에 출력 */

	/*
	 * Historically we did not send a uevent for changes to/from an empty
	 * device.
	 */
	if (!capacity || !size) /* [한국어] 0 ↔ 비0 전환(초기화/삭제)은 uevent 생략(역사적 관례) */
		return false;
	kobject_uevent_env(&disk_to_dev(disk)->kobj, KOBJ_CHANGE, envp); /* [한국어] KOBJ_CHANGE + RESIZE=1 uevent로 udev에 통지 */
	return true; /* [한국어] uevent를 실제로 발생시켰음을 호출자에 알림 */
}
EXPORT_SYMBOL_GPL(set_capacity_and_notify);

/*
 * [한국어]
 * part_stat_read_all - 전체 CPU의 파티션 I/O 통계를 집계
 *
 * @part: 통계를 수집할 block_device 포인터 (디스크 전체 또는 개별 파티션).
 * @stat: 결과를 저장할 disk_stats 포인터. 호출 전 내용은 덮어쓰여짐.
 * @return: void. @stat에 모든 CPU의 합산 값이 채워진다.
 *
 * per-CPU 통계(bd_stats)를 CPU마다 순회하며 read/write/discard/flush 그룹별
 * ios, merges, sectors, nsecs와 io_ticks를 합산한다. per-CPU 통계이므로
 * 읽기 도중 카운터가 변경될 수 있으나, part_stat_show에서 이미 inflight를
 * 확인하고 호출하므로 근사치 읽기로 충분하다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read, /proc/diskstats read).
 *
 * 호출 체인:
 *   part_stat_show() → [part_stat_read_all()] (sysfs /stat)
 *   diskstats_show() → [part_stat_read_all()] (/proc/diskstats)
 */
static void part_stat_read_all(struct block_device *part,
		struct disk_stats *stat)
{
	int cpu; /* [한국어] CPU 인덱스 — for_each_possible_cpu 루프 변수 */

	memset(stat, 0, sizeof(struct disk_stats)); /* [한국어] 결과 버퍼 초기화 — 누적 합산 전 0으로 클리어 */
	for_each_possible_cpu(cpu) { /* [한국어] 존재 가능한 모든 CPU를 순회하며 per-CPU 통계 합산 */
		struct disk_stats *ptr = per_cpu_ptr(part->bd_stats, cpu); /* [한국어] 해당 CPU의 per-CPU 통계 포인터 획득 */
		int group; /* [한국어] I/O 그룹 인덱스(READ/WRITE/DISCARD/FLUSH) */

		for (group = 0; group < NR_STAT_GROUPS; group++) { /* [한국어] 4개 I/O 그룹(READ, WRITE, DISCARD, FLUSH) 순회 */
			stat->nsecs[group] += ptr->nsecs[group];     /* [한국어] 그룹별 총 소요 나노초 누적 */
			stat->sectors[group] += ptr->sectors[group]; /* [한국어] 그룹별 전송 섹터 수 누적 */
			stat->ios[group] += ptr->ios[group];         /* [한국어] 그룹별 I/O 완료 횟수 누적 */
			stat->merges[group] += ptr->merges[group];   /* [한국어] 그룹별 I/O 병합 횟수 누적 */
		}

		stat->io_ticks += ptr->io_ticks; /* [한국어] 큐가 바쁜 상태의 총 jiffies(io_ticks) 누적 */
	}
}

/*
 * [한국어]
 * bdev_count_inflight_rw - block_device의 진행 중 I/O를 READ/WRITE별로 집계
 *
 * @part:      대상 block_device 포인터.
 * @inflight:  결과 저장 배열 [READ]=읽기 진행 수, [WRITE]=쓰기 진행 수.
 * @mq_driver: true이면 blk-mq 드라이버(NVMe 등), false이면 bio-based 드라이버.
 * @return:    void. inflight[] 배열에 결과가 채워진다.
 *
 * blk-mq 드라이버(NVMe)는 blk_mq_in_driver_rw()를 통해 태그 기반 요청 수를
 * 집계한다. bio-based 드라이버는 per-CPU in_flight 카운터를 CPU별로 합산한다.
 * CPU 순회 중 카운터가 음수가 될 수 있으므로(순회 도중 완료 처리) 0 이하면 0으로 클램프.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs/proc 읽기 경로).
 *
 * 호출 체인:
 *   bdev_count_inflight() → [bdev_count_inflight_rw(false)]
 *   part_inflight_show()  → [bdev_count_inflight_rw(queue_is_mq())]
 */
static void bdev_count_inflight_rw(struct block_device *part,
		unsigned int inflight[2], bool mq_driver)
{
	int write = 0; /* [한국어] 진행 중 쓰기 I/O 합산용 임시 변수 */
	int read = 0;  /* [한국어] 진행 중 읽기 I/O 합산용 임시 변수 */
	int cpu;       /* [한국어] CPU 인덱스 — per-CPU 통계 순회에 사용 */

	if (mq_driver) { /* [한국어] blk-mq 드라이버(NVMe 포함): 태그셋 기반 정확한 카운트 사용 */
		blk_mq_in_driver_rw(part, inflight); /* [한국어] NVMe SQ에 submit되어 CQ에서 아직 완료되지 않은 CID 수 집계 */
		return; /* [한국어] blk-mq 경로는 여기서 완료 — per-CPU 합산 불필요 */
	}

	for_each_possible_cpu(cpu) { /* [한국어] bio-based 드라이버: 전체 CPU의 in_flight 카운터 합산 */
		read += part_stat_local_read_cpu(part, in_flight[READ], cpu);   /* [한국어] 해당 CPU의 진행 중 읽기 I/O 수 누적 */
		write += part_stat_local_read_cpu(part, in_flight[WRITE], cpu); /* [한국어] 해당 CPU의 진행 중 쓰기 I/O 수 누적 */
	}

	/*
	 * While iterating all CPUs, some IOs may be issued from a CPU already
	 * traversed and complete on a CPU that has not yet been traversed,
	 * causing the inflight number to be negative.
	 */
	inflight[READ] = read > 0 ? read : 0;   /* [한국어] 음수 방지 클램프: 순회 도중 완료된 I/O로 인한 언더플로 처리 */
	inflight[WRITE] = write > 0 ? write : 0; /* [한국어] 쓰기도 동일하게 음수 방지 클램프 */
}

/*
 * [한국어]
 * bdev_count_inflight - block_device의 총 진행 중 I/O 개수 반환
 *
 * @part: 대상 block_device 포인터 (디스크 전체 또는 파티션).
 * @return: 현재 진행 중인 읽기+쓰기 I/O의 합계.
 *
 * bio-based 경로(mq_driver=false)로 bdev_count_inflight_rw를 호출하여
 * per-CPU in_flight 카운터를 합산하고, READ+WRITE 합계를 반환한다.
 * blk-mq 드라이버에서는 inflight_show()가 직접 queue_is_mq()를 전달하므로
 * 이 함수는 주로 bio-based 또는 범용 경로에서 사용된다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read, part_stat_show 내에서 호출).
 *
 * 호출 체인:
 *   part_stat_show() → [bdev_count_inflight()] → bdev_count_inflight_rw()
 *   diskstats_show() → [bdev_count_inflight()] → bdev_count_inflight_rw()
 */
/**
 * bdev_count_inflight - get the number of inflight IOs for a block device.
 *
 * @part: the block device.
 *
 * Inflight here means started IO accounting, from bdev_start_io_acct() for
 * bio-based block device, and from blk_account_io_start() for rq-based block
 * device.
 */
unsigned int bdev_count_inflight(struct block_device *part)
{
	unsigned int inflight[2] = {0}; /* [한국어] READ[0], WRITE[1] 진행 I/O 수 저장 버퍼, 0으로 초기화 */

	bdev_count_inflight_rw(part, inflight, false); /* [한국어] bio-based 경로(mq_driver=false)로 per-CPU 카운터 합산 */

	return inflight[READ] + inflight[WRITE]; /* [한국어] 읽기+쓰기 합산하여 총 진행 I/O 수 반환 */
}
EXPORT_SYMBOL_GPL(bdev_count_inflight);

/*
 * Can be deleted altogether. Later.
 *
 */
/* [한국어] major 번호 해시 테이블 크기: 1~254 범위의 major를 모두 수용하는 소수 크기.
 * major % 255로 해시하면 대부분 충돌 없이 분산된다. */
#define BLKDEV_MAJOR_HASH_SIZE 255
/*
 * [한국어]
 * struct blk_major_name — major 번호와 드라이버 이름의 매핑 엔트리
 *
 * major_names[] 해시 테이블의 각 버킷에 연결 리스트로 매달리는 엔트리.
 * NVMe 드라이버(drivers/nvme/host/core.c)는 probe 시 __register_blkdev()를
 * 통해 자신의 major("nvme")를 이 테이블에 등록한다. 이후 사용자가 /dev/nvmeXnY를
 * open하면 커널이 major로 이 테이블을 조회해 해당 드라이버를 찾는다.
 */
static struct blk_major_name {
	struct blk_major_name *next;
	/* [한국어] 해시 체인의 다음 엔트리 포인터.
	 * 설정자: __register_blkdev()가 체인 끝에 삽입.
	 * 읽는 자: major_names[idx]를 순회하는 for 루프.
	 * 값 범위: NULL(체인 끝) 또는 유효한 blk_major_name 포인터.
	 * 동기화: major_names_spinlock 보호 하에 접근. */

	int major;
	/* [한국어] 등록된 major 번호 (예: NVMe = BLOCK_EXT_MAJOR 또는 명시적 major).
	 * 설정자: __register_blkdev()에서 p->major = major로 설정.
	 * 읽는 자: blkdev_show(), major_to_index(), 장치 lookup 경로.
	 * 값 범위: 1 ~ BLKDEV_MAJOR_MAX-1.
	 * 동기화: 등록 후 읽기 전용이므로 스핀락 없이도 안전하게 읽기 가능. */

	char name[16];
	/* [한국어] 드라이버 이름 문자열 (예: "nvme", "sd", "loop").
	 * 설정자: __register_blkdev()에서 strscpy(p->name, name).
	 * 읽는 자: blkdev_show()가 /proc/devices에 출력; unregister_blkdev()가 이름 검증.
	 * 값 범위: 최대 15자 + NUL 종료.
	 * 동기화: 등록 후 읽기 전용. */

#ifdef CONFIG_BLOCK_LEGACY_AUTOLOAD
	void (*probe)(dev_t devt);
	/* [한국어] 레거시 자동 로드 콜백. devtmpfs 이전 방식으로 장치 노드를 프로브할 때 사용.
	 * 현대 NVMe 드라이버는 이 콜백을 NULL로 등록하며, 향후 제거 예정.
	 * 설정자: __register_blkdev()의 p->probe = probe.
	 * 읽는 자: blk_probe_dev() → if ((*n)->probe) (*n)->probe(devt).
	 * 동기화: 등록 후 읽기 전용. */
#endif
} *major_names[BLKDEV_MAJOR_HASH_SIZE]; /* [한국어] major % 255 버킷의 head 포인터 배열 — major→드라이버 매핑 해시 테이블 */
/* [한국어] major_names[] 테이블 전체 수정(삽입/삭제)을 직렬화하는 뮤텍스.
 * kmalloc 등 슬립 가능 연산을 감싸므로 mutex를 사용한다. */
static DEFINE_MUTEX(major_names_lock);
/* [한국어] major_names[] 개별 체인 읽기·수정을 보호하는 스핀락.
 * blkdev_show()처럼 인터럽트 컨텍스트에서 호출될 수 있어 spinlock을 사용한다. */
static DEFINE_SPINLOCK(major_names_spinlock);

/* [한국어] major 번호를 major_names[] 배열 인덱스로 변환하는 해시 함수.
 * 현재는 단순 모듈로 연산 사용(멀티-major 범위는 미지원). */
/* index in the above - for now: assume no multimajor ranges */
/*
 * [한국어]
 * major_to_index - major 번호를 해시 테이블 인덱스로 변환
 *
 * @major: 해시할 major 번호.
 * @return: major_names[] 배열 인덱스 (0 ~ BLKDEV_MAJOR_HASH_SIZE-1).
 *
 * major % BLKDEV_MAJOR_HASH_SIZE(255) 단순 나머지 연산으로 버킷 인덱스를 계산.
 * 실행 컨텍스트: 락 내외 모두 사용 가능한 인라인 함수.
 *
 * 호출 체인:
 *   __register_blkdev() → [major_to_index()] (삽입 위치 결정)
 *   unregister_blkdev()  → [major_to_index()] (삭제 위치 탐색)
 *   blkdev_show()        → [major_to_index()] (/proc/devices 출력)
 */
static inline int major_to_index(unsigned major)
{
	return major % BLKDEV_MAJOR_HASH_SIZE; /* [한국어] 나머지 연산으로 해시 버킷 인덱스 계산 */
}

#ifdef CONFIG_PROC_FS
/*
 * [한국어]
 * blkdev_show - /proc/devices에 특정 major의 블록 장치 이름을 출력
 *
 * @seqf:   seq_file 포인터 — /proc/devices 파일의 출력 스트림.
 * @offset: 조회할 major 번호 (off_t이지만 major 번호로 사용).
 * @return: void. seq_printf로 "NNN name\n" 형식 라인을 출력.
 *
 * /proc/devices 파일을 순회할 때 각 major에 대해 호출된다.
 * major_names_spinlock으로 해시 테이블을 보호하면서 체인을 탐색하고,
 * 일치하는 major가 있으면 seq_printf로 "major name" 라인을 출력한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (proc read 경로).
 *
 * 호출 체인:
 *   /proc/devices 읽기 → [blkdev_show()]
 */
void blkdev_show(struct seq_file *seqf, off_t offset)
{
	struct blk_major_name *dp; /* [한국어] 해시 체인 순회용 포인터 */

	spin_lock(&major_names_spinlock); /* [한국어] 해시 테이블 읽기 보호 — 등록/해제와 경쟁 방지 */
	for (dp = major_names[major_to_index(offset)]; dp; dp = dp->next) /* [한국어] 해당 버킷의 연결 리스트 순회 */
		if (dp->major == offset) /* [한국어] 해시 충돌 가능성이 있으므로 major 번호를 직접 비교 */
			seq_printf(seqf, "%3d %s\n", dp->major, dp->name); /* [한국어] "NNN name" 형식으로 /proc/devices에 출력 */
	spin_unlock(&major_names_spinlock); /* [한국어] 읽기 완료 후 스핀락 해제 */
}
#endif /* CONFIG_PROC_FS */

/*
 * [한국어]
 * __register_blkdev - 새로운 블록 장치 major 번호를 시스템에 등록
 *
 * @major: 요청 major 번호 [1..BLKDEV_MAJOR_MAX-1]. 0이면 빈 슬롯을 동적으로 할당.
 * @name:  장치 이름 문자열 (예: "nvme"). 시스템 내 고유해야 함.
 * @probe: 레거시 장치 자동 탐색 콜백. NULL이면 비활성화.
 * @return: 성공 시 0(또는 동적 할당된 major 번호), 실패 시 음수 에러 코드.
 *
 * NVMe 호스트 드라이버가 초기화될 때 자신의 major 번호를 major_names[] 해시 테이블에
 * 등록한다. 등록된 major는 /dev/nvmeXnY 장치 노드 생성의 근거가 되며, 이후 add_disk()에서
 * gendisk의 major 필드가 이 값으로 설정된다. major=0이면 배열 끝부터 역순으로 빈 슬롯을
 * 탐색해 동적 할당한다. 동일 major 중복 등록 시 -EBUSY를 반환한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (드라이버 초기화 경로, 슬립 가능).
 * 에러 경로: -EBUSY(이미 사용 중), -EINVAL(범위 초과), -ENOMEM(메모리 부족).
 *
 * 호출 체인:
 *   nvme_core_init() → register_blkdev() → [__register_blkdev()]
 */
/**
 * __register_blkdev - register a new block device
 *
 * @major: the requested major device number [1..BLKDEV_MAJOR_MAX-1]. If
 *         @major = 0, try to allocate any unused major number.
 * @name: the name of the new block device as a zero terminated string
 * @probe: pre-devtmpfs / pre-udev callback used to create disks when their
 *	   pre-created device node is accessed. When a probe call uses
 *	   add_disk() and it fails the driver must cleanup resources. This
 *	   interface may soon be removed.
 *
 * The @name must be unique within the system.
 *
 * The return value depends on the @major input parameter:
 *
 *  - if a major device number was requested in range [1..BLKDEV_MAJOR_MAX-1]
 *    then the function returns zero on success, or a negative error code
 *  - if any unused major number was requested with @major = 0 parameter
 *    then the return value is the allocated major number in range
 *    [1..BLKDEV_MAJOR_MAX-1] or a negative error code otherwise
 *
 * See Documentation/admin-guide/devices.txt for the list of allocated
 * major numbers.
 *
 * Use register_blkdev instead for any new code.
 */
int __register_blkdev(unsigned int major, const char *name,
		void (*probe)(dev_t devt))
{
	struct blk_major_name **n, *p; /* [한국어] n: 삽입 위치 포인터-투-포인터, p: 새 엔트리 */
	int index, ret = 0;            /* [한국어] index: 해시 버킷 인덱스, ret: 반환값(성공=0 또는 동적 major) */

	mutex_lock(&major_names_lock); /* [한국어] 해시 테이블 전체 수정 직렬화 — kmalloc을 감싸므로 mutex 사용 */

	/* temporary */
	if (major == 0) { /* [한국어] major=0: 빈 슬롯을 역순으로 탐색해 동적 할당 */
		for (index = ARRAY_SIZE(major_names)-1; index > 0; index--) { /* [한국어] 배열 끝에서 앞으로 빈 버킷 탐색 */
			if (major_names[index] == NULL) /* [한국어] 해당 버킷이 비어있으면 이 index를 major로 사용 */
				break;
		}

		if (index == 0) { /* [한국어] 빈 슬롯을 찾지 못한 경우 — 모든 major가 소진됨 */
			printk("%s: failed to get major for %s\n",
			       __func__, name); /* [한국어] 커널 로그에 할당 실패 기록 */
			ret = -EBUSY; /* [한국어] 슬롯 소진 — EBUSY 반환 */
			goto out;     /* [한국어] 뮤텍스 해제 후 반환 */
		}
		major = index; /* [한국어] 동적으로 찾은 빈 슬롯을 major 번호로 확정 */
		ret = major;   /* [한국어] 동적 할당 시 반환값은 할당된 major 번호 자체 */
	}

	if (major >= BLKDEV_MAJOR_MAX) { /* [한국어] major 번호가 허용 범위(BLKDEV_MAJOR_MAX-1)를 초과하면 거부 */
		pr_err("%s: major requested (%u) is greater than the maximum (%u) for %s\n",
		       __func__, major, BLKDEV_MAJOR_MAX-1, name); /* [한국어] 에러 메시지 출력 */

		ret = -EINVAL; /* [한국어] 범위 초과 — EINVAL 반환 */
		goto out;
	}

	p = kmalloc_obj(struct blk_major_name); /* [한국어] 새 해시 엔트리를 GFP_KERNEL로 동적 할당 */
	if (p == NULL) { /* [한국어] 메모리 부족 시 에러 처리 */
		ret = -ENOMEM;
		goto out;
	}

	p->major = major; /* [한국어] 엔트리에 major 번호 저장 */
#ifdef CONFIG_BLOCK_LEGACY_AUTOLOAD
	p->probe = probe; /* [한국어] 레거시 자동 로드 콜백 저장 (NULL이면 비활성) */
#endif
	strscpy(p->name, name, sizeof(p->name)); /* [한국어] 드라이버 이름을 최대 15자까지 안전하게 복사 */
	p->next = NULL;                          /* [한국어] 체인 삽입 전 next 초기화 */
	index = major_to_index(major);           /* [한국어] major 번호를 해시 인덱스로 변환 */

	spin_lock(&major_names_spinlock); /* [한국어] 체인 수정 보호 — 인터럽트 컨텍스트에서 읽는 경로와 경쟁 방지 */
	for (n = &major_names[index]; *n; n = &(*n)->next) { /* [한국어] 버킷 체인을 끝까지 또는 동일 major까지 순회 */
		if ((*n)->major == major) /* [한국어] 동일 major 번호가 이미 등록된 경우 체인 삽입 건너뜀 */
			break;
	}
	if (!*n)       /* [한국어] 체인 끝에 도달한 경우 — 새 엔트리를 삽입 */
		*n = p;
	else           /* [한국어] 동일 major가 이미 존재 — EBUSY로 중복 등록 거부 */
		ret = -EBUSY;
	spin_unlock(&major_names_spinlock); /* [한국어] 체인 수정 완료 후 스핀락 해제 */

	if (ret < 0) { /* [한국어] 삽입 실패(중복 또는 이전 에러) 시 동적 할당한 엔트리 해제 */
		printk("register_blkdev: cannot get major %u for %s\n",
		       major, name); /* [한국어] 중복 등록 시도 커널 로그 기록 */
		kfree(p); /* [한국어] 삽입되지 못한 엔트리 메모리 해제 */
	}
out:
	mutex_unlock(&major_names_lock); /* [한국어] 전체 수정 구간 뮤텍스 해제 */
	return ret; /* [한국어] 성공: 0(명시적 major) 또는 동적 major 번호; 실패: 음수 에러 코드 */
}
EXPORT_SYMBOL(__register_blkdev);

/*
 * [한국어]
 * unregister_blkdev - 블록 장치 major 번호 등록 해제
 *
 * @major: 해제할 major 번호 (등록 시 사용한 값과 동일해야 함).
 * @name:  등록 시 사용한 이름 (검증용).
 * @return: void. major/name 불일치 시 WARN_ON(1) 트리거.
 *
 * NVMe 드라이버 모듈이 언로드되거나 컨트롤러가 제거될 때 호출된다.
 * major_names[] 해시 테이블에서 해당 엔트리를 찾아 체인에서 제거하고 해제한다.
 * major와 name 모두 일치해야 제거하며, 불일치 시 WARN_ON으로 버그를 경고한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (드라이버 해제/언로드 경로, 슬립 가능).
 *
 * 호출 체인:
 *   nvme_core_exit() → [unregister_blkdev()] → major_names[] 체인 제거 + kfree()
 */
void unregister_blkdev(unsigned int major, const char *name)
{
	struct blk_major_name **n;          /* [한국어] 체인 순회용 포인터-투-포인터 (삭제 시 이전 next 갱신에 사용) */
	struct blk_major_name *p = NULL;    /* [한국어] 제거할 엔트리 저장 — kfree는 스핀락 밖에서 수행 */
	int index = major_to_index(major);  /* [한국어] major 번호를 해시 인덱스로 변환 */

	mutex_lock(&major_names_lock);      /* [한국어] 전체 해시 테이블 수정 직렬화 */
	spin_lock(&major_names_spinlock);   /* [한국어] 체인 수정 보호 */
	for (n = &major_names[index]; *n; n = &(*n)->next) /* [한국어] 해당 버킷 체인 순회 */
		if ((*n)->major == major)   /* [한국어] major 번호가 일치하는 엔트리를 찾으면 중단 */
			break;
	if (!*n || strcmp((*n)->name, name)) { /* [한국어] 엔트리가 없거나 이름이 불일치 — 등록/해제 짝이 맞지 않는 버그 */
		WARN_ON(1); /* [한국어] 잘못된 unregister 호출을 스택 트레이스와 함께 경고 */
	} else {
		p = *n;       /* [한국어] 제거할 엔트리 포인터 저장 */
		*n = p->next; /* [한국어] 이전 엔트리의 next를 다음 엔트리로 갱신하여 체인에서 제거 */
	}
	spin_unlock(&major_names_spinlock); /* [한국어] 체인 수정 완료 후 스핀락 해제 */
	mutex_unlock(&major_names_lock);    /* [한국어] 전체 수정 구간 뮤텍스 해제 */
	kfree(p); /* [한국어] 체인에서 분리된 엔트리 메모리 해제 (p==NULL이면 kfree는 no-op) */
}

EXPORT_SYMBOL(unregister_blkdev);

/*
 * [한국어]
 * blk_alloc_ext_minor - 확장 동적 minor 번호 할당
 *
 * @return: 성공 시 할당된 minor 번호 (0 ~ NR_EXT_DEVT-1), 실패 시 -EBUSY.
 *
 * BLOCK_EXT_MAJOR 아래에서 NVMe처럼 동적 major를 사용하지 않는 장치가
 * ext_devt_ida 풀에서 minor 번호를 할당받는다.
 * IDA 내부적으로 슬립 가능하므로 GFP_KERNEL 플래그 사용.
 * 실행 컨텍스트: 프로세스 컨텍스트 (__add_disk 내 major == 0 분기).
 *
 * 호출 체인:
 *   __add_disk() → [blk_alloc_ext_minor()] (major==0인 경우 동적 minor 확보)
 */
int blk_alloc_ext_minor(void)
{
	int idx; /* [한국어] IDA에서 할당받은 인덱스(= minor 번호) 저장 */

	idx = ida_alloc_range(&ext_devt_ida, 0, NR_EXT_DEVT - 1, GFP_KERNEL); /* [한국어] 0~NR_EXT_DEVT-1 범위에서 미사용 슬롯 할당 */
	if (idx == -ENOSPC) /* [한국어] 풀이 소진된 경우 -ENOSPC가 반환됨 */
		return -EBUSY;  /* [한국어] 커널 관례상 -EBUSY로 변환하여 반환 */
	return idx; /* [한국어] 성공 시 할당된 minor 번호 반환 */
}

/*
 * [한국어]
 * blk_free_ext_minor - 확장 동적 minor 번호 반환
 *
 * @minor: 반환할 minor 번호 (blk_alloc_ext_minor가 반환한 값).
 * @return: void.
 *
 * 장치 제거 시(__add_disk 실패 경로 또는 __del_gendisk에서) 동적으로
 * 할당했던 minor를 ext_devt_ida 풀에 반환한다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   __add_disk 실패 경로 → [blk_free_ext_minor()]
 *   __del_gendisk()      → [blk_free_ext_minor()] (disk->major == BLOCK_EXT_MAJOR 조건)
 */
void blk_free_ext_minor(unsigned int minor)
{
	ida_free(&ext_devt_ida, minor); /* [한국어] IDA 풀에 minor 슬롯 반환 */
}

/*
 * [한국어]
 * disk_uevent - gendisk의 모든 파티션(+전체 디스크)에 uevent 발생
 *
 * @disk:   uevent를 발생시킬 gendisk 포인터.
 * @action: kobject 액션 (KOBJ_ADD, KOBJ_REMOVE, KOBJ_CHANGE 등).
 * @return: void.
 *
 * part_tbl의 모든 block_device를 RCU로 순회하며 각각에 kobject_uevent를 발생시킨다.
 * 크기가 0인 파티션은 건너뛰고, kobject 참조 카운트를 원자적으로 확보한 후 uevent를
 * 보내고 참조를 해제한다. RCU 락 내에서는 uevent를 보내지 않고 락 밖에서 전송하여
 * 블로킹 경로와의 데드락을 방지한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (add_disk_final, __del_gendisk 내).
 *
 * 호출 체인:
 *   add_disk_final() → [disk_uevent(disk, KOBJ_ADD)]
 *   del_gendisk()    → (간접적으로) [disk_uevent()]
 */
void disk_uevent(struct gendisk *disk, enum kobject_action action)
{
	struct block_device *part; /* [한국어] part_tbl 순회 중 현재 파티션 포인터 */
	unsigned long idx;         /* [한국어] xa_for_each 인덱스(파티션 번호) */

	rcu_read_lock(); /* [한국어] part_tbl XArray를 RCU로 안전하게 읽기 시작 */
	xa_for_each(&disk->part_tbl, idx, part) { /* [한국어] 디스크의 모든 파티션(+part0) 순회 */
		if (bdev_is_partition(part) && !bdev_nr_sectors(part)) /* [한국어] 크기가 0인 파티션은 uevent 생략 */
			continue;
		if (!kobject_get_unless_zero(&part->bd_device.kobj)) /* [한국어] kobject 참조 카운트가 0이면 이미 소멸 중 — 건너뜀 */
			continue;

		rcu_read_unlock(); /* [한국어] uevent 전송 전 RCU 락 해제 — 블로킹 가능한 경로이므로 락 내 수행 불가 */
		kobject_uevent(bdev_kobj(part), action); /* [한국어] udev에 action(ADD/REMOVE/CHANGE) uevent 전송 */
		put_device(&part->bd_device); /* [한국어] kobject_get_unless_zero로 높인 참조 카운트 해제 */
		rcu_read_lock(); /* [한국어] 다음 파티션 순회를 위해 RCU 락 재획득 */
	}
	rcu_read_unlock(); /* [한국어] 전체 순회 완료 후 RCU 락 해제 */
}
EXPORT_SYMBOL_GPL(disk_uevent);

/*
 * [한국어]
 * disk_scan_partitions - gendisk의 파티션 테이블 스캔
 *
 * @disk: 파티션을 스캔할 gendisk 포인터 (NVMe 네임스페이스 1개에 대응).
 * @mode: 블록 장치 open 모드 (BLK_OPEN_READ 등). 배타적 오픈 여부 포함.
 * @return: 성공 시 0, 실패 시 음수 에러 코드 (-EINVAL, -EBUSY 등).
 *
 * NVMe 네임스페이스가 add_disk_final()에서 등록 완료 직전에 호출되어,
 * GPT/MBR 파티션 테이블을 해석하고 disk->part_tbl에 파티션을 추가한다.
 * GD_NEED_PART_SCAN 플래그를 설정 후 bdev_file_open_by_dev()로 장치를 열면
 * 파티션 스캔 경로가 자동으로 진입된다. 비배타적 오픈 모드에서는
 * bd_prepare_to_claim()으로 다른 배타적 오프너와 동기화한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (add_disk_final 내, 슬립 가능).
 * 에러 경로: partscan 불가(-EINVAL), 파티션 오픈 중(-EBUSY), open 실패(PTR_ERR).
 *
 * 호출 체인:
 *   add_disk_final() → [disk_scan_partitions(disk, BLK_OPEN_READ)]
 *   사용자 ioctl      → [disk_scan_partitions()] (BLKRRPART 등)
 */
int disk_scan_partitions(struct gendisk *disk, blk_mode_t mode)
{
	struct file *file; /* [한국어] bdev_file_open_by_dev 반환 파일 핸들 — open/close 트리거로만 사용 */
	int ret = 0;       /* [한국어] 반환값 초기화 */

	if (!disk_has_partscan(disk)) /* [한국어] GENHD_FL_NO_PART 등 파티션 스캔 불가 플래그 확인 */
		return -EINVAL;
	if (disk->open_partitions) /* [한국어] 이미 파티션이 열려있으면 스캔 불가 */
		return -EBUSY;

	/*
	 * If the device is opened exclusively by current thread already, it's
	 * safe to scan partitons, otherwise, use bd_prepare_to_claim() to
	 * synchronize with other exclusive openers and other partition
	 * scanners.
	 */
	if (!(mode & BLK_OPEN_EXCL)) { /* [한국어] 비배타적 모드: 다른 배타적 오프너와 동기화 필요 */
		ret = bd_prepare_to_claim(disk->part0, disk_scan_partitions,
					  NULL); /* [한국어] 배타적 점유를 선점하여 동시 스캔 방지 */
		if (ret) /* [한국어] 배타적 점유 실패 시 즉시 반환 */
			return ret;
	}

	set_bit(GD_NEED_PART_SCAN, &disk->state); /* [한국어] GD_NEED_PART_SCAN 플래그 설정: open 시 파티션 스캔 진입 신호 */
	file = bdev_file_open_by_dev(disk_devt(disk), mode & ~BLK_OPEN_EXCL,
				     NULL, NULL); /* [한국어] 장치를 열어 파티션 스캔 경로(blkdev_open→blk_partpick) 진입 */
	if (IS_ERR(file))  /* [한국어] open 실패 시 에러 코드 추출 */
		ret = PTR_ERR(file);
	else
		fput(file); /* [한국어] 스캔 목적 달성 후 파일 핸들 즉시 해제 */

	/*
	 * If blkdev_get_by_dev() failed early, GD_NEED_PART_SCAN is still set,
	 * and this will cause that re-assemble partitioned raid device will
	 * creat partition for underlying disk.
	 */
	clear_bit(GD_NEED_PART_SCAN, &disk->state); /* [한국어] 스캔 완료(또는 실패) 후 GD_NEED_PART_SCAN 플래그 해제 */
	if (!(mode & BLK_OPEN_EXCL)) /* [한국어] 비배타적 모드에서만 bd_prepare_to_claim으로 획득한 점유 해제 */
		bd_abort_claiming(disk->part0, disk_scan_partitions); /* [한국어] 점유 해제 — 대기 중인 배타적 오프너 진입 허용 */
	return ret; /* [한국어] 성공 시 0, 실패 시 음수 에러 코드 */
}

/*
 * [한국어]
 * add_disk_final - gendisk 등록의 마무리 단계 수행
 *
 * @disk: 등록을 완료할 gendisk 포인터.
 * @return: void.
 *
 * __add_disk()로 sysfs 구조와 major/minor가 확보된 후, 이 함수가 나머지 단계를
 * 수행한다. 구체적으로: (1) bdev_add로 block_device를 inode에 등록,
 * (2) 파티션 스캔 실행, (3) uevent 억제 해제 및 KOBJ_ADD uevent 발생,
 * (4) bdi writeback 한계 적용, (5) 이벤트 폴링 시작, (6) GD_ADDED 플래그 설정.
 * NVMe 관점에서 이 단계가 완료되어야 /dev/nvmeXnY가 udev에 의해 노출되고
 * 파일시스템이 마운트하여 I/O를 발행할 수 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (add_disk_fwnode에서 nr_hw_queues 락 밖).
 * 에러 경로: 이 함수 자체는 에러를 반환하지 않는다.
 *
 * 호출 체인:
 *   add_disk_fwnode() → [add_disk_final()] → bdev_add() + disk_scan_partitions()
 *                                           + disk_uevent() + blk_apply_bdi_limits()
 */
static void add_disk_final(struct gendisk *disk)
{
	struct device *ddev = disk_to_dev(disk); /* [한국어] gendisk에 내장된 struct device 포인터 획득 */

	if (!(disk->flags & GENHD_FL_HIDDEN)) { /* [한국어] hidden 디스크(dm 내부 등)는 sysfs 노출·uevent 생략 */
		/* Make sure the first partition scan will be proceed */
		if (get_capacity(disk) && disk_has_partscan(disk)) /* [한국어] 용량이 있고 파티션 스캔 가능한 경우에만 플래그 설정 */
			set_bit(GD_NEED_PART_SCAN, &disk->state); /* [한국어] 첫 open 시 파티션 스캔이 진입되도록 플래그 사전 설정 */

		bdev_add(disk->part0, ddev->devt); /* [한국어] block_device를 bdev 해시에 등록하여 open 가능한 상태로 만듦 */
		if (get_capacity(disk))            /* [한국어] 용량이 있는 경우에만 파티션 스캔 실행 */
			disk_scan_partitions(disk, BLK_OPEN_READ); /* [한국어] GPT/MBR 파티션 테이블 읽기 및 파티션 추가 */

		/*
		 * Announce the disk and partitions after all partitions are
		 * created. (for hidden disks uevents remain suppressed forever)
		 */
		dev_set_uevent_suppress(ddev, 0); /* [한국어] __add_disk에서 설정한 uevent 억제 해제 — 이후 uevent 전송 허용 */
		disk_uevent(disk, KOBJ_ADD);      /* [한국어] 디스크와 파티션 모두에 KOBJ_ADD uevent 발생 → udev가 /dev 노드 생성 */
	}

	blk_apply_bdi_limits(disk->bdi, &disk->queue->limits); /* [한국어] NVMe queue limits(최대 전송 크기 등)를 bdi writeback 한계에 반영 */
	disk_add_events(disk); /* [한국어] 미디어 변경·꺼냄 이벤트 폴링 시작 (disk_events.c의 타이머 등록) */
	set_bit(GD_ADDED, &disk->state); /* [한국어] GD_ADDED 플래그 설정: 등록 완료 표시, disk_release에서 free_disk 호출 여부 결정 */
}

/*
 * [한국어]
 * __add_disk - gendisk를 커널 장치 계층에 등록 (내부 구현)
 *
 * @parent: 부모 장치 포인터 (NVMe: &ctrl->device 등 컨트롤러 device).
 * @disk:   등록할 gendisk 포인터.
 * @groups: 추가 sysfs 속성 그룹 배열 (드라이버별 확장 속성).
 * @fwnode: 연결할 firmware node (ACPI/DT 연동, 없으면 NULL).
 * @return: 성공 시 0, 실패 시 음수 에러 코드.
 *
 * add_disk_fwnode()에서 호출되는 실제 등록 구현체. 다음을 순서대로 수행한다:
 * (1) blk-mq 드라이버 검증(submit_bio 미제공 확인),
 * (2) major가 있으면 minor 범위 검증, 없으면 BLOCK_EXT_MAJOR+동적 minor 할당,
 * (3) uevent 억제 설정 후 device_add 호출,
 * (4) disk_alloc_events, /sys/block 심볼릭 링크 생성,
 * (5) holders/slaves kobject 디렉터리 생성,
 * (6) blk_register_queue로 queue sysfs 등록,
 * (7) bdi(Backing Device Info) 등록 및 sysfs 링크.
 * NVMe 드라이버는 blk-mq를 사용하므로 submit_bio가 없고, BLOCK_EXT_MAJOR를 통해
 * 동적 minor를 얻는다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (nr_hw_queues read 락 내부).
 * 에러 경로: 각 단계 실패 시 역순 정리 후 에러 반환 (goto out_* 체인).
 *
 * 호출 체인:
 *   add_disk_fwnode() → [__add_disk()] → device_add() + blk_register_queue()
 */
static int __add_disk(struct device *parent, struct gendisk *disk,
		      const struct attribute_group **groups,
		      struct fwnode_handle *fwnode)

{
	struct device *ddev = disk_to_dev(disk); /* [한국어] gendisk에 내장된 struct device 포인터 */
	int ret;                                 /* [한국어] 반환값 저장 변수 */

	if (WARN_ON_ONCE(bdev_nr_sectors(disk->part0) > BLK_DEV_MAX_SECTORS)) /* [한국어] 등록 시점에 이미 초과 용량이면 버그 경고 */
		return -EINVAL;

	if (queue_is_mq(disk->queue)) { /* [한국어] blk-mq 드라이버(NVMe 포함): submit_bio를 제공하면 안 됨 */
		/*
		 * ->submit_bio and ->poll_bio are bypassed for blk-mq drivers.
		 */
		if (disk->fops->submit_bio || disk->fops->poll_bio) /* [한국어] blk-mq 드라이버가 submit_bio/poll_bio를 제공하면 등록 거부 */
			return -EINVAL;
	} else { /* [한국어] bio-based 드라이버: submit_bio가 필수 */
		if (!disk->fops->submit_bio) /* [한국어] bio-based 드라이버에 submit_bio가 없으면 등록 거부 */
			return -EINVAL;
		bdev_set_flag(disk->part0, BD_HAS_SUBMIT_BIO); /* [한국어] bio-based 경로 표시 — blk_submit_bio 대신 fops->submit_bio 사용 */
	}

	/*
	 * If the driver provides an explicit major number it also must provide
	 * the number of minors numbers supported, and those will be used to
	 * setup the gendisk.
	 * Otherwise just allocate the device numbers for both the whole device
	 * and all partitions from the extended dev_t space.
	 */
	ret = -EINVAL; /* [한국어] 이후 goto out 시 기본 에러 코드로 EINVAL 설정 */
	if (disk->major) { /* [한국어] 드라이버가 명시적 major를 제공한 경우 minor 범위 검증 */
		if (WARN_ON(!disk->minors)) /* [한국어] major가 있으면 minors도 반드시 있어야 함 */
			goto out;

		if (disk->minors > DISK_MAX_PARTS) { /* [한국어] minors가 최대 파티션 수를 초과하면 클램프 */
			pr_err("block: can't allocate more than %d partitions\n",
				DISK_MAX_PARTS); /* [한국어] 최대치 초과 경고 출력 */
			disk->minors = DISK_MAX_PARTS; /* [한국어] DISK_MAX_PARTS로 강제 제한 */
		}
		if (disk->first_minor > MINORMASK ||           /* [한국어] first_minor가 minor 비트 마스크 초과 */
		    disk->minors > MINORMASK + 1 ||             /* [한국어] minors 수가 최대 minor 공간 초과 */
		    disk->first_minor + disk->minors > MINORMASK + 1) /* [한국어] first_minor+minors 합계가 minor 공간 초과 */
			goto out;
	} else { /* [한국어] major == 0: BLOCK_EXT_MAJOR에서 동적 minor 할당 */
		if (WARN_ON(disk->minors)) /* [한국어] 동적 할당 경우 드라이버가 minors를 직접 설정하면 안 됨 */
			goto out;

		ret = blk_alloc_ext_minor(); /* [한국어] ext_devt_ida에서 동적 minor 번호 할당 */
		if (ret < 0) /* [한국어] 풀 소진(-EBUSY) 또는 메모리 부족 시 에러 처리 */
			goto out;
		disk->major = BLOCK_EXT_MAJOR; /* [한국어] NVMe 등 동적 장치: BLOCK_EXT_MAJOR 사용 */
		disk->first_minor = ret;       /* [한국어] 동적 할당된 minor 번호를 first_minor로 설정 */
	}

	/* delay uevents, until we scanned partition table */
	dev_set_uevent_suppress(ddev, 1); /* [한국어] 파티션 스캔 완료 전 uevent 억제 — add_disk_final에서 해제 */

	ddev->parent = parent;               /* [한국어] NVMe 컨트롤러 device를 부모로 설정 */
	ddev->groups = groups;               /* [한국어] 드라이버별 추가 sysfs 속성 그룹 등록 */
	dev_set_name(ddev, "%s", disk->disk_name); /* [한국어] sysfs 경로명을 disk_name(예: "nvme0n1")으로 설정 */
	if (fwnode)                          /* [한국어] ACPI/DT firmware node가 있으면 장치에 연결 */
		device_set_node(ddev, fwnode);
	if (!(disk->flags & GENHD_FL_HIDDEN)) /* [한국어] hidden이 아닌 경우만 devt를 설정하여 /dev 노드 생성 가능 */
		ddev->devt = MKDEV(disk->major, disk->first_minor); /* [한국어] major+minor로 dev_t 조합하여 장치 번호 설정 */
	ret = device_add(ddev); /* [한국어] 장치 드라이버 코어에 등록 — /sys/block/nvmeXnY 디렉터리 생성 */
	if (ret) /* [한국어] device_add 실패 시 ext_minor 해제 경로로 이동 */
		goto out_free_ext_minor;

	ret = disk_alloc_events(disk); /* [한국어] 미디어 변경·꺼냄 이벤트를 위한 poll 타이머 자원 할당 */
	if (ret)
		goto out_device_del;

	ret = sysfs_create_link(block_depr, &ddev->kobj,
				kobject_name(&ddev->kobj)); /* [한국어] /sys/block/nvmeXnY → 실제 경로로 레거시 심볼릭 링크 생성 */
	if (ret)
		goto out_device_del;

	/*
	 * avoid probable deadlock caused by allocating memory with
	 * GFP_KERNEL in runtime_resume callback of its all ancestor
	 * devices
	 */
	pm_runtime_set_memalloc_noio(ddev, true); /* [한국어] 런타임 resume 콜백에서 GFP_KERNEL 할당 금지 — 데드락 방지 */

	disk->part0->bd_holder_dir = /* [한국어] NVMe 디스크의 holder 디렉터리 — dm/lvm 등이 여기에 링크를 걸어 의존성 추적 */
		kobject_create_and_add("holders", &ddev->kobj); /* [한국어] /sys/block/nvmeXnY/holders 디렉터리 생성 */
	if (!disk->part0->bd_holder_dir) { /* [한국어] 메모리 부족으로 direcory 생성 실패 */
		ret = -ENOMEM;
		goto out_del_block_link;
	}
	disk->slave_dir = kobject_create_and_add("slaves", &ddev->kobj); /* [한국어] /sys/block/nvmeXnY/slaves 디렉터리 생성 */
	if (!disk->slave_dir) { /* [한국어] slaves 디렉터리 생성 실패 */
		ret = -ENOMEM;
		goto out_put_holder_dir;
	}

	ret = blk_register_queue(disk); /* [한국어] NVMe request_queue를 /sys/block/nvmeXnY/queue에 sysfs 등록 */
	if (ret)
		goto out_put_slave_dir;

	if (!(disk->flags & GENHD_FL_HIDDEN)) { /* [한국어] visible 디스크만 bdi를 sysfs에 등록 */
		ret = bdi_register(disk->bdi, "%u:%u", /* [한국어] bdi를 "major:minor" 이름으로 /sys/class/bdi/에 등록 */
				   disk->major, disk->first_minor);
		if (ret)
			goto out_unregister_queue;
		bdi_set_owner(disk->bdi, ddev); /* [한국어] bdi의 소유 장치를 gendisk device로 설정 */
		ret = sysfs_create_link(&ddev->kobj,
					&disk->bdi->dev->kobj, "bdi"); /* [한국어] /sys/block/nvmeXnY/bdi → bdi 장치로 심볼릭 링크 */
		if (ret)
			goto out_unregister_bdi;
	} else {
		/*
		 * Even if the block_device for a hidden gendisk is not
		 * registered, it needs to have a valid bd_dev so that the
		 * freeing of the dynamic major works.
		 */
		disk->part0->bd_dev = MKDEV(disk->major, disk->first_minor); /* [한국어] hidden 디스크도 동적 major 해제를 위해 bd_dev 설정 */
	}
	return 0; /* [한국어] 모든 등록 단계 성공 */

out_unregister_bdi:
	if (!(disk->flags & GENHD_FL_HIDDEN)) /* [한국어] visible 디스크만 bdi를 등록했으므로 조건부 해제 */
		bdi_unregister(disk->bdi);
out_unregister_queue:
	blk_unregister_queue(disk); /* [한국어] queue sysfs 등록 해제 */
	rq_qos_exit(disk->queue);   /* [한국어] request QoS 정리 */
out_put_slave_dir:
	kobject_put(disk->slave_dir); /* [한국어] slaves 디렉터리 kobject 참조 해제 */
	disk->slave_dir = NULL;
out_put_holder_dir:
	kobject_put(disk->part0->bd_holder_dir); /* [한국어] holders 디렉터리 kobject 참조 해제 */
out_del_block_link:
	sysfs_remove_link(block_depr, dev_name(ddev)); /* [한국어] /sys/block 레거시 심볼릭 링크 제거 */
	pm_runtime_set_memalloc_noio(ddev, false);     /* [한국어] 런타임 PM 메모리 제한 해제 */
out_device_del:
	device_del(ddev); /* [한국어] 장치 드라이버 코어에서 제거 */
out_free_ext_minor:
	if (disk->major == BLOCK_EXT_MAJOR) /* [한국어] 동적 minor를 할당했던 경우에만 반환 */
		blk_free_ext_minor(disk->first_minor); /* [한국어] ext_devt_ida 풀에 minor 번호 반환 */
out:
	return ret; /* [한국어] 성공 시 0, 실패 시 음수 에러 코드 반환 */
}

/*
 * [한국어]
 * add_disk_fwnode - firmware node 포함 gendisk를 커널 장치 계층에 등록
 *
 * @parent: 부모 장치 포인터 (NVMe: 컨트롤러 device).
 * @disk:   등록할 gendisk 포인터.
 * @groups: 추가 sysfs 속성 그룹.
 * @fwnode: 연결할 firmware node (ACPI/DT). 없으면 NULL.
 * @return: 성공 시 0, 실패 시 음수 에러 코드.
 *
 * blk-mq 드라이버의 경우 tag_set->update_nr_hwq_lock 읽기 락을 잡고 __add_disk를
 * 호출하여, nvme_update_nr_queues() 등의 nr_hw_queues 변경과 등록 경로의 경쟁을 방지한다.
 * add_disk_final()은 open_mutex 데드락 방지를 위해 이 락 밖에서 호출된다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (드라이버 probe/ns_add 경로, 슬립 가능).
 * 에러 경로: __add_disk 실패 시 add_disk_final 호출 생략.
 *
 * 호출 체인:
 *   device_add_disk() → [add_disk_fwnode(parent, disk, groups, NULL)]
 *   NVMe 드라이버       → [add_disk_fwnode(parent, disk, groups, fwnode)]
 *                       → __add_disk() + add_disk_final()
 */
/**
 * add_disk_fwnode - add disk information to kernel list with fwnode
 * @parent: parent device for the disk
 * @disk: per-device partitioning information
 * @groups: Additional per-device sysfs groups
 * @fwnode: attached disk fwnode
 *
 * This function registers the partitioning information in @disk
 * with the kernel. Also attach a fwnode to the disk device.
 */
int __must_check add_disk_fwnode(struct device *parent, struct gendisk *disk,
				 const struct attribute_group **groups,
				 struct fwnode_handle *fwnode)
{
	struct blk_mq_tag_set *set; /* [한국어] blk-mq 태그셋 포인터 — nr_hw_queues 락 접근에 사용 */
	unsigned int memflags;      /* [한국어] memalloc_noio_save 이전 플래그 저장 */
	int ret;                    /* [한국어] __add_disk 반환값 */

	if (queue_is_mq(disk->queue)) { /* [한국어] blk-mq 드라이버(NVMe): nr_hw_queues 변경과 동기화 필요 */
		set = disk->queue->tag_set; /* [한국어] NVMe blk_mq_tag_set — update_nr_hwq_lock 소유자 */
		memflags = memalloc_noio_save(); /* [한국어] GFP_NOIO 상태로 전환 — 등록 중 재귀 I/O 방지 */
		down_read(&set->update_nr_hwq_lock); /* [한국어] nr_hw_queues 변경(nvme_update_nr_queues 등)과 경쟁 방지 */
		ret = __add_disk(parent, disk, groups, fwnode); /* [한국어] 실제 등록 수행 */
		up_read(&set->update_nr_hwq_lock); /* [한국어] nr_hw_queues 읽기 락 해제 */
		memalloc_noio_restore(memflags); /* [한국어] 이전 메모리 할당 플래그 복원 */
	} else { /* [한국어] bio-based 드라이버: 락 없이 직접 등록 */
		ret = __add_disk(parent, disk, groups, fwnode); /* [한국어] 실제 등록 수행 */
	}

	/*
	 * add_disk_final() needn't to read `nr_hw_queues`, so move it out
	 * of read lock `set->update_nr_hwq_lock` for avoiding unnecessary
	 * lock dependency on `disk->open_mutex` from scanning partition.
	 */
	if (!ret) /* [한국어] __add_disk 성공한 경우에만 최종 단계(파티션 스캔, uevent) 수행 */
		add_disk_final(disk); /* [한국어] 락 밖에서 호출 — open_mutex와 nr_hw_queues 락 간 데드락 방지 */
	return ret; /* [한국어] 성공 시 0, 실패 시 음수 에러 코드 */
}
EXPORT_SYMBOL_GPL(add_disk_fwnode);

/*
 * [한국어]
 * device_add_disk - gendisk를 커널에 등록하는 공개 API
 *
 * @parent: 부모 장치 포인터 (NVMe: 컨트롤러 device).
 * @disk:   등록할 gendisk 포인터.
 * @groups: 추가 sysfs 속성 그룹.
 * @return: 성공 시 0, 실패 시 음수 에러 코드.
 *
 * firmware node 없이 gendisk를 등록하는 단순화된 공개 진입점.
 * add_disk_fwnode(fwnode=NULL)의 래퍼이다. NVMe 드라이버는 이 함수를 통해
 * 네임스페이스를 블록 서브시스템에 노출시킨다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   NVMe 드라이버 → [device_add_disk()] → add_disk_fwnode(fwnode=NULL)
 */
/**
 * device_add_disk - add disk information to kernel list
 * @parent: parent device for the disk
 * @disk: per-device partitioning information
 * @groups: Additional per-device sysfs groups
 *
 * This function registers the partitioning information in @disk
 * with the kernel.
 */
int __must_check device_add_disk(struct device *parent, struct gendisk *disk,
				 const struct attribute_group **groups)
{
	return add_disk_fwnode(parent, disk, groups, NULL); /* [한국어] fwnode=NULL로 add_disk_fwnode 위임 */
}
EXPORT_SYMBOL(device_add_disk);

/*
 * [한국어]
 * blk_report_disk_dead - 모든 파티션에 장치 소멸(dead) 통보
 *
 * @disk:     소멸을 통보할 gendisk 포인터.
 * @surprise: true이면 갑작스러운 제거(hot-unplug), false이면 정상 종료.
 * @return:   void.
 *
 * 디스크의 part_tbl에 있는 모든 block_device에 bdev_mark_dead()를 호출하여
 * 파일시스템이 더티 페이지를 플러시하고 종료하도록 유도한다. surprise=true일 때는
 * 파일시스템 경로(bdev_mark_dead)에서 슬립할 수 있으므로 open_mutex를 잡지 않음을
 * lockdep_assert_not_held로 검증한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (open_mutex 보유 금지).
 *
 * 호출 체인:
 *   blk_mark_disk_dead() → [blk_report_disk_dead(disk, true)]
 *   __del_gendisk()      → [blk_report_disk_dead(disk, false)]
 */
static void blk_report_disk_dead(struct gendisk *disk, bool surprise)
{
	struct block_device *bdev; /* [한국어] part_tbl 순회 중 현재 block_device */
	unsigned long idx;         /* [한국어] xa_for_each 인덱스 */

	/*
	 * On surprise disk removal, bdev_mark_dead() may call into file
	 * systems below. Make it clear that we're expecting to not hold
	 * disk->open_mutex.
	 */
	lockdep_assert_not_held(&disk->open_mutex); /* [한국어] open_mutex 보유 중이면 lockdep 경고 — 파일시스템 콜백과 데드락 방지 */

	rcu_read_lock(); /* [한국어] part_tbl RCU 읽기 시작 */
	xa_for_each(&disk->part_tbl, idx, bdev) { /* [한국어] 모든 파티션(+part0) 순회 */
		if (!kobject_get_unless_zero(&bdev->bd_device.kobj)) /* [한국어] 이미 소멸 중인 bdev 건너뜀 */
			continue;
		rcu_read_unlock(); /* [한국어] bdev_mark_dead이 슬립 가능하므로 RCU 락 밖에서 호출 */

		bdev_mark_dead(bdev, surprise); /* [한국어] 파일시스템에 장치 소멸 통보(더티 페이지 플러시 및 종료 유도) */

		put_device(&bdev->bd_device); /* [한국어] kobject_get_unless_zero로 높인 참조 해제 */
		rcu_read_lock(); /* [한국어] 다음 순회를 위해 RCU 락 재획득 */
	}
	rcu_read_unlock(); /* [한국어] 전체 순회 완료 후 RCU 락 해제 */
}

/*
 * [한국어]
 * __blk_mark_disk_dead - gendisk를 dead 상태로 표시하고 큐 드레인 시작
 *
 * @disk: dead 처리할 gendisk 포인터.
 * @return: true이면 큐 드레인을 새로 시작함, false이면 이미 dead였음.
 *
 * GD_DEAD 비트를 원자적으로 설정하고(이미 설정된 경우 false 반환),
 * GD_OWNS_QUEUE이면 QUEUE_FLAG_DYING을 설정하며, 용량을 0으로 설정해
 * 버퍼드 라이터의 더티 페이지 생성을 막고, blk_queue_start_drain()으로
 * bio_queue_enter()를 통한 새 I/O 진입을 차단한다.
 * 실행 컨텍스트: open_mutex를 보유한 상태에서 호출됨.
 *
 * 호출 체인:
 *   blk_mark_disk_dead()  → [__blk_mark_disk_dead()]
 *   __del_gendisk()       → [__blk_mark_disk_dead()]
 */
static bool __blk_mark_disk_dead(struct gendisk *disk)
{
	/*
	 * Fail any new I/O.
	 */
	if (test_and_set_bit(GD_DEAD, &disk->state)) /* [한국어] GD_DEAD 비트를 원자적으로 설정; 이미 설정된 경우 false 반환 */
		return false; /* [한국어] 이미 dead 처리됨 — 중복 호출이므로 종료 */

	if (test_bit(GD_OWNS_QUEUE, &disk->state)) /* [한국어] 디스크가 queue를 소유하는 경우(blk_alloc_disk 경로) */
		blk_queue_flag_set(QUEUE_FLAG_DYING, disk->queue); /* [한국어] QUEUE_FLAG_DYING: 새 요청이 -EIO로 즉시 실패하도록 설정 */

	/*
	 * Stop buffered writers from dirtying pages that can't be written out.
	 */
	set_capacity(disk, 0); /* [한국어] 용량 0으로 설정 — 범위 밖 쓰기를 거부하여 더티 페이지 생성 차단 */

	/*
	 * Prevent new I/O from crossing bio_queue_enter().
	 */
	return blk_queue_start_drain(disk->queue); /* [한국어] bio_queue_enter() 드레인 시작 — 새 I/O 진입 차단; 성공 시 true */
}

/*
 * [한국어]
 * blk_mark_disk_dead - 디스크를 dead 상태로 표시하고 파일시스템에 통보
 *
 * @disk: dead 처리할 gendisk 포인터.
 * @return: void.
 *
 * 갑작스러운 NVMe 컨트롤러 제거(hot-unplug) 또는 치명적 오류 발생 시 호출된다.
 * __blk_mark_disk_dead()로 GD_DEAD·QUEUE_FLAG_DYING을 설정하고 새 I/O를 차단한 후,
 * blk_report_disk_dead(surprise=true)로 모든 파티션의 파일시스템에 소멸을 통보한다.
 * 파일시스템은 더티 페이지를 플러시하고 에러 상태를 기록한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (open_mutex 보유 금지).
 *
 * 호출 체인:
 *   nvme 드라이버 error/timeout 핸들러 → [blk_mark_disk_dead()]
 *                → __blk_mark_disk_dead() + blk_report_disk_dead(true)
 */
/**
 * blk_mark_disk_dead - mark a disk as dead
 * @disk: disk to mark as dead
 *
 * Mark as disk as dead (e.g. surprise removed) and don't accept any new I/O
 * to this disk.
 */
void blk_mark_disk_dead(struct gendisk *disk)
{
	__blk_mark_disk_dead(disk);          /* [한국어] GD_DEAD 설정, 용량 0, 큐 드레인 시작 */
	blk_report_disk_dead(disk, true);    /* [한국어] surprise=true: 모든 파티션 파일시스템에 갑작스러운 소멸 통보 */
}
EXPORT_SYMBOL_GPL(blk_mark_disk_dead);

/*
 * [한국어]
 * __del_gendisk - gendisk 제거의 실제 구현체
 *
 * @disk: 제거할 gendisk 포인터.
 * @return: void.
 *
 * del_gendisk()에서 호출되는 실제 구현. 다음을 순서대로 수행한다:
 * (1) 이벤트 폴링 중단 (disk_del_events),
 * (2) open_mutex 아래 모든 파티션 inode를 해시에서 제거하여 새 open 차단,
 * (3) GD_DEAD 미설정 시 파일시스템에 정상 종료 통보 (blk_report_disk_dead),
 * (4) GD_DEAD 설정, 파티션 drop, 큐 드레인 시작,
 * (5) sysfs bdi 링크 제거, bdi 등록 해제,
 * (6) blk_unregister_queue: queue sysfs 제거,
 * (7) holders/slaves kobject 해제,
 * (8) blk_mq_freeze_queue_wait: 모든 진행 중 I/O 완료 대기,
 * (9) throtl·sync·integrity 정리,
 * (10) blk-mq work 취소, rq_qos 정리,
 * (11) queue 동결 상태 처리(blk_mq_exit_queue 또는 unfreeze).
 * 실행 컨텍스트: 프로세스 컨텍스트 (슬립 가능, might_sleep 명시).
 *
 * 호출 체인:
 *   del_gendisk() → [__del_gendisk()] → blk_unregister_queue()
 *                                       + blk_mq_freeze_queue_wait()
 *                                       + blk_mq_exit_queue()
 */
static void __del_gendisk(struct gendisk *disk)
{
	struct request_queue *q = disk->queue; /* [한국어] 디스크의 request_queue 포인터 캐시 */
	struct block_device *part;             /* [한국어] part_tbl 순회용 파티션 포인터 */
	unsigned long idx;                     /* [한국어] xa_for_each 인덱스 */
	bool start_drain;                      /* [한국어] __blk_mark_disk_dead이 큐 드레인을 새로 시작했는지 여부 */

	might_sleep(); /* [한국어] 이 함수는 슬립 가능한 컨텍스트에서 호출되어야 함을 명시 */

	if (WARN_ON_ONCE(!disk_live(disk) && !(disk->flags & GENHD_FL_HIDDEN))) /* [한국어] 이미 dead이고 hidden도 아닌 경우 버그 경고 */
		return;

	disk_del_events(disk); /* [한국어] 미디어 변경·꺼냄 이벤트 폴링 타이머 해제 */

	/*
	 * Prevent new openers by unlinked the bdev inode.
	 */
	mutex_lock(&disk->open_mutex); /* [한국어] open과 동기화하여 새 open이 부분적으로 제거된 디스크를 보지 않도록 */
	xa_for_each(&disk->part_tbl, idx, part) /* [한국어] 모든 파티션 순회 */
		bdev_unhash(part); /* [한국어] block_device를 bdev 해시에서 제거 — 이후 lookup 불가 */
	mutex_unlock(&disk->open_mutex);

	/*
	 * Tell the file system to write back all dirty data and shut down if
	 * it hasn't been notified earlier.
	 */
	if (!test_bit(GD_DEAD, &disk->state)) /* [한국어] blk_mark_disk_dead가 이미 호출되지 않은 경우에만 통보 */
		blk_report_disk_dead(disk, false); /* [한국어] surprise=false: 정상 종료 경로로 파일시스템 통보 */

	/*
	 * Drop all partitions now that the disk is marked dead.
	 */
	mutex_lock(&disk->open_mutex); /* [한국어] 파티션 drop과 새 open 간 경쟁 방지 */
	start_drain = __blk_mark_disk_dead(disk); /* [한국어] GD_DEAD 설정, 용량 0, 큐 드레인 시작 */
	if (start_drain) /* [한국어] 이 호출이 드레인을 시작한 경우에만 freeze 락 획득 */
		blk_freeze_acquire_lock(q); /* [한국어] 큐 동결 상태 확인을 위한 freeze 락 획득 */
	xa_for_each_start(&disk->part_tbl, idx, part, 1) /* [한국어] idx=1부터: part0 제외하고 파티션만 순회 */
		drop_partition(part); /* [한국어] 각 파티션 block_device를 XArray에서 제거하고 bdev_drop */
	mutex_unlock(&disk->open_mutex);

	if (!(disk->flags & GENHD_FL_HIDDEN)) { /* [한국어] hidden이 아닌 경우만 visible sysfs 자원 제거 */
		sysfs_remove_link(&disk_to_dev(disk)->kobj, "bdi"); /* [한국어] /sys/block/nvmeXnY/bdi 심볼릭 링크 제거 */

		/*
		 * Unregister bdi before releasing device numbers (as they can
		 * get reused and we'd get clashes in sysfs).
		 */
		bdi_unregister(disk->bdi); /* [한국어] bdi sysfs 등록 해제 — 장치 번호 재사용 전에 먼저 해제 */
	}

	blk_unregister_queue(disk); /* [한국어] /sys/block/nvmeXnY/queue sysfs 디렉터리 제거 */

	kobject_put(disk->part0->bd_holder_dir); /* [한국어] holders kobject 참조 해제 */
	kobject_put(disk->slave_dir);            /* [한국어] slaves kobject 참조 해제 */
	disk->slave_dir = NULL;                  /* [한국어] 해제된 포인터 NULL로 초기화 */

	part_stat_set_all(disk->part0, 0); /* [한국어] part0의 모든 I/O 통계 카운터 0으로 리셋 */
	disk->part0->bd_stamp = 0;         /* [한국어] 타임스탬프 초기화 — 재사용 시 오래된 값 방지 */
	sysfs_remove_link(block_depr, dev_name(disk_to_dev(disk))); /* [한국어] /sys/block 레거시 심볼릭 링크 제거 */
	pm_runtime_set_memalloc_noio(disk_to_dev(disk), false);     /* [한국어] 런타임 PM 메모리 제한 해제 */
	device_del(disk_to_dev(disk)); /* [한국어] 장치 드라이버 코어에서 제거 — /sys/block/nvmeXnY 디렉터리 삭제 */

	blk_mq_freeze_queue_wait(q); /* [한국어] 모든 진행 중 I/O가 완료될 때까지 대기 — 안전한 자원 해제 보장 */

	blk_throtl_cancel_bios(disk); /* [한국어] throttle 지연 큐에 남은 bio들을 취소하여 더 이상 dispatch 안 되게 함 */

	blk_sync_queue(q);    /* [한국어] 큐의 지연 작업(delayed_work)이 완전히 종료될 때까지 동기화 대기 */
	blk_flush_integrity(); /* [한국어] 무결성 플러시 요청 완료 대기 */

	if (queue_is_mq(q)) /* [한국어] NVMe: blk-mq 경로에서만 work sync 수행 */
		blk_mq_cancel_work_sync(q); /* [한국어] NVMe timeout/retry work를 취소하고 이미 실행 중인 work 완료 대기 */

	rq_qos_exit(q); /* [한국어] request QoS(iocost, wbt, iolatency 등) 자원 해제 */

	/*
	 * If the disk does not own the queue, allow using passthrough requests
	 * again.  Else leave the queue frozen to fail all I/O.
	 */
	if (!test_bit(GD_OWNS_QUEUE, &disk->state)) /* [한국어] queue를 소유하지 않는 경우(multi-disk 공유 queue): passthrough 허용 복원 */
		__blk_mq_unfreeze_queue(q, true); /* [한국어] 큐 동결 해제(lock=true: 큐 동결 카운터만 감소) */
	else if (queue_is_mq(q)) /* [한국어] queue를 소유하는 blk-mq 드라이버: queue 자원 완전 해제 */
		blk_mq_exit_queue(q); /* [한국어] hctx, 태그셋 바인딩 등 blk-mq queue 내부 자원 전체 정리 */

	if (start_drain) /* [한국어] __blk_mark_disk_dead에서 드레인을 새로 시작한 경우에만 */
		blk_unfreeze_release_lock(q); /* [한국어] freeze 드레인 락 해제 */
}

/*
 * [한국어]
 * disable_elv_switch - I/O 스케줄러 교체 비활성화
 *
 * @q: 스케줄러 교체를 비활성화할 request_queue 포인터.
 * @return: void.
 *
 * del_gendisk 시작 시점에 호출하여, 제거 진행 중 I/O 스케줄러가 교체되는
 * 경쟁 상태를 방지한다. update_nr_hwq_lock의 쓰기 락을 잡고
 * QUEUE_FLAG_NO_ELV_SWITCH 플래그를 설정한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (del_gendisk 내).
 *
 * 호출 체인:
 *   del_gendisk() → [disable_elv_switch()] → QUEUE_FLAG_NO_ELV_SWITCH 설정
 */
static void disable_elv_switch(struct request_queue *q)
{
	struct blk_mq_tag_set *set = q->tag_set; /* [한국어] queue와 연결된 blk_mq_tag_set — update_nr_hwq_lock 소유자 */
	WARN_ON_ONCE(!queue_is_mq(q)); /* [한국어] blk-mq 아닌 queue에서 호출하면 버그 경고 */

	down_write(&set->update_nr_hwq_lock); /* [한국어] 쓰기 락 획득 — nr_hw_queues 변경과 배타적으로 실행 */
	blk_queue_flag_set(QUEUE_FLAG_NO_ELV_SWITCH, q); /* [한국어] 스케줄러 교체 금지 플래그 설정 */
	up_write(&set->update_nr_hwq_lock); /* [한국어] 쓰기 락 해제 */
}

/*
 * [한국어]
 * del_gendisk - gendisk 제거의 공개 API
 *
 * @disk: 제거할 gendisk 포인터. device_add_disk()로 등록한 것과 동일해야 함.
 * @return: void. 이 함수는 동기적으로 완료됨 (드라이버가 동기 완료에 의존).
 *
 * device_add_disk()의 역방향 호출. blk-mq 드라이버(NVMe)는 스케줄러 교체를
 * 비활성화하고, memalloc_noio 플래그를 설정한 뒤, nr_hw_queues 읽기 락 아래
 * __del_gendisk를 호출한다. 이 gendisk의 실제 메모리 해제는 마지막 put_disk()
 * 호출 시 disk_release()에서 이루어지며, del_gendisk 자체는 해제하지 않는다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (슬립 가능).
 *
 * 호출 체인:
 *   NVMe 드라이버 → [del_gendisk()] → disable_elv_switch()
 *                                    + __del_gendisk() (nr_hw_queues 락 내)
 */
/**
 * del_gendisk - remove the gendisk
 * @disk: the struct gendisk to remove
 *
 * Removes the gendisk and all its associated resources. This deletes the
 * partitions associated with the gendisk, and unregisters the associated
 * request_queue.
 *
 * This is the counter to the respective device_add_disk() call.
 *
 * The final removal of the struct gendisk happens when its refcount reaches 0
 * with put_disk(), which should be called after del_gendisk(), if
 * device_add_disk() was used.
 *
 * Drivers exist which depend on the release of the gendisk to be synchronous,
 * it should not be deferred.
 *
 * Context: can sleep
 */
void del_gendisk(struct gendisk *disk)
{
	struct blk_mq_tag_set *set; /* [한국어] blk-mq 태그셋 — nr_hw_queues 락 접근에 사용 */
	unsigned int memflags;      /* [한국어] memalloc_noio 이전 플래그 저장 */

	if (!queue_is_mq(disk->queue)) { /* [한국어] bio-based 드라이버: 락 없이 직접 제거 */
		__del_gendisk(disk);
	} else { /* [한국어] blk-mq 드라이버(NVMe 포함) */
		set = disk->queue->tag_set; /* [한국어] queue에 연결된 태그셋 포인터 획득 */

		disable_elv_switch(disk->queue); /* [한국어] 제거 진행 중 I/O 스케줄러 교체 방지 */

		memflags = memalloc_noio_save();    /* [한국어] GFP_NOIO 상태 전환 — 제거 중 재귀 I/O 방지 */
		down_read(&set->update_nr_hwq_lock); /* [한국어] nvme_update_nr_queues와 제거의 경쟁 방지 */
		__del_gendisk(disk);               /* [한국어] 실제 제거 수행 */
		up_read(&set->update_nr_hwq_lock); /* [한국어] nr_hw_queues 읽기 락 해제 */
		memalloc_noio_restore(memflags);   /* [한국어] 이전 메모리 할당 플래그 복원 */
	}
}
EXPORT_SYMBOL(del_gendisk);

/*
 * [한국어]
 * invalidate_disk - 디스크의 버퍼/페이지 캐시 무효화 및 내부 상태 초기화
 *
 * @disk: 무효화할 gendisk 포인터.
 * @return: void.
 *
 * NVMe 네임스페이스 제거 후 재사용(또는 revalidate) 시 기존 페이지 캐시와
 * 버퍼 캐시를 모두 무효화하고, writeback 에러 플래그와 용량을 0으로 리셋한다.
 * 이후 드라이버가 새 용량으로 set_capacity를 호출하면 깨끗한 상태에서 재사용된다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (슬립 가능).
 *
 * 호출 체인:
 *   NVMe 드라이버 revalidate 경로 → [invalidate_disk()]
 *                                  → invalidate_bdev() + set_capacity(0)
 */
/**
 * invalidate_disk - invalidate the disk
 * @disk: the struct gendisk to invalidate
 *
 * A helper to invalidates the disk. It will clean the disk's associated
 * buffer/page caches and reset its internal states so that the disk
 * can be reused by the drivers.
 *
 * Context: can sleep
 */
void invalidate_disk(struct gendisk *disk)
{
	struct block_device *bdev = disk->part0; /* [한국어] 전체 디스크를 나타내는 block_device(part0) 획득 */

	invalidate_bdev(bdev);        /* [한국어] part0의 모든 페이지 캐시·버퍼 캐시를 무효화 */
	bdev->bd_mapping->wb_err = 0; /* [한국어] address_space의 writeback 에러 플래그 초기화 */
	set_capacity(disk, 0);        /* [한국어] 논리 용량을 0으로 설정 — 재사용 전 용량 초기화 */
}
EXPORT_SYMBOL(invalidate_disk);

/* sysfs access to bad-blocks list. */
/*
 * [한국어]
 * disk_badblocks_show - /sys/block/XY/badblocks 읽기 핸들러
 *
 * @dev:  gendisk에 대응하는 struct device.
 * @attr: sysfs 속성 (dev_attr_badblocks).
 * @page: 출력 버퍼 (PAGE_SIZE).
 * @return: 출력 바이트 수.
 *
 * 디스크의 불량 섹터(bad block) 목록을 sysfs를 통해 노출한다.
 * disk->bb가 없으면 빈 줄을 반환하고, 있으면 badblocks_show()로 목록 출력.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read).
 *
 * 호출 체인: sysfs read → [disk_badblocks_show()] → badblocks_show()
 */
static ssize_t disk_badblocks_show(struct device *dev,
					struct device_attribute *attr,
					char *page)
{
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] device 포인터에서 gendisk 역참조 */

	if (!disk->bb) /* [한국어] bad block 테이블이 없으면 빈 줄 반환 */
		return sysfs_emit(page, "\n");

	return badblocks_show(disk->bb, page, 0); /* [한국어] bad block 목록을 텍스트 형식으로 page에 출력 */
}

/*
 * [한국어]
 * disk_badblocks_store - /sys/block/XY/badblocks 쓰기 핸들러
 *
 * @dev:  gendisk에 대응하는 struct device.
 * @attr: sysfs 속성 (dev_attr_badblocks).
 * @page: 사용자에서 전달된 입력 버퍼.
 * @len:  입력 길이.
 * @return: 성공 시 처리된 바이트 수, 실패 시 음수 에러 코드.
 *
 * 불량 섹터를 수동으로 추가/제거한다. disk->bb가 없으면 -ENXIO.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs write).
 *
 * 호출 체인: sysfs write → [disk_badblocks_store()] → badblocks_store()
 */
static ssize_t disk_badblocks_store(struct device *dev,
					struct device_attribute *attr,
					const char *page, size_t len)
{
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] device 포인터에서 gendisk 역참조 */

	if (!disk->bb) /* [한국어] bad block 테이블 미설정 시 -ENXIO 반환 */
		return -ENXIO;

	return badblocks_store(disk->bb, page, len, 0); /* [한국어] 입력을 파싱하여 bad block 목록에 추가/제거 */
}

#ifdef CONFIG_BLOCK_LEGACY_AUTOLOAD
/*
 * [한국어]
 * blk_probe_dev - 레거시 probe 콜백을 통한 장치 탐색
 *
 * @devt: 탐색할 장치 번호 (dev_t).
 * @return: probe 콜백을 호출했으면 true, 없으면 false.
 *
 * major_names 해시에서 major에 해당하는 probe 콜백을 찾아 호출한다.
 * devtmpfs/udev 이전 방식의 레거시 자동 로드 경로이다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (blk_request_module에서 호출).
 *
 * 호출 체인:
 *   blk_request_module() → [blk_probe_dev()]
 */
static bool blk_probe_dev(dev_t devt)
{
	unsigned int major = MAJOR(devt); /* [한국어] devt에서 major 번호 추출 */
	struct blk_major_name **n;        /* [한국어] 해시 체인 순회용 포인터 */

	mutex_lock(&major_names_lock); /* [한국어] major_names 테이블 읽기 보호 */
	for (n = &major_names[major_to_index(major)]; *n; n = &(*n)->next) { /* [한국어] 버킷 체인 순회 */
		if ((*n)->major == major && (*n)->probe) { /* [한국어] major 일치 + probe 콜백 존재 확인 */
			(*n)->probe(devt); /* [한국어] 레거시 probe 콜백 호출 — 드라이버가 add_disk 등을 수행 */
			mutex_unlock(&major_names_lock);
			return true; /* [한국어] probe 호출 완료 */
		}
	}
	mutex_unlock(&major_names_lock);
	return false; /* [한국어] 해당 major에 probe 콜백 없음 */
}

/*
 * [한국어]
 * blk_request_module - major 번호에 대응하는 블록 드라이버 모듈 자동 로드
 *
 * @devt: 로드할 장치 번호 (dev_t).
 * @return: void.
 *
 * 장치를 open할 때 해당 major를 담당하는 드라이버가 없으면 이 함수로 모듈을
 * 동적 로드한다. 먼저 probe 콜백을 시도하고, 없으면 request_module로
 * "block-major-NNN-MMM" 형식의 모듈 이름으로 modprobe를 호출한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (슬립 가능).
 *
 * 호출 체인:
 *   blkdev_open() → [blk_request_module()] → blk_probe_dev() 또는 request_module()
 */
void blk_request_module(dev_t devt)
{
	int error; /* [한국어] request_module 반환값 저장 */

	if (blk_probe_dev(devt)) /* [한국어] probe 콜백이 있으면 먼저 시도 */
		return;

	error = request_module("block-major-%d-%d", MAJOR(devt), MINOR(devt)); /* [한국어] "block-major-NNN-MMM" 모듈 로드 시도 */
	/* Make old-style 2.4 aliases work */
	if (error > 0) /* [한국어] 모듈 로드 실패 시 레거시 2.4 형식("block-major-NNN")으로 재시도 */
		error = request_module("block-major-%d", MAJOR(devt));
	if (!error) /* [한국어] 모듈 로드 성공 시 probe 콜백 재시도 */
		blk_probe_dev(devt);
}
#endif /* CONFIG_BLOCK_LEGACY_AUTOLOAD */

#ifdef CONFIG_PROC_FS
/* iterator */
/*
 * [한국어]
 * disk_seqf_start - /proc/diskstats, /proc/partitions seq_file 이터레이터 시작
 *
 * @seqf: seq_file 포인터 — 출력 스트림 및 private 저장소.
 * @pos:  현재 위치 포인터 (몇 번째 디스크에서 시작할지).
 * @return: 첫 번째 gendisk 포인터, 비었으면 NULL, 에러면 ERR_PTR.
 *
 * block_class의 device를 순회하는 class_dev_iter를 할당하고, *pos만큼 건너뛴 후
 * 첫 번째 gendisk를 반환한다. seqf->private에 iter를 저장하여 next/stop에서 재사용.
 * 실행 컨텍스트: 프로세스 컨텍스트 (proc read).
 *
 * 호출 체인:
 *   /proc/diskstats 또는 /proc/partitions 읽기 → seq_file core → [disk_seqf_start()]
 */
static void *disk_seqf_start(struct seq_file *seqf, loff_t *pos)
{
	loff_t skip = *pos;          /* [한국어] 건너뛸 디스크 수 — *pos번째부터 출력 */
	struct class_dev_iter *iter; /* [한국어] block_class 디바이스 이터레이터 */
	struct device *dev;          /* [한국어] 현재 순회 중인 device */

	iter = kmalloc_obj(*iter); /* [한국어] GFP_KERNEL로 이터레이터 동적 할당 */
	if (!iter)
		return ERR_PTR(-ENOMEM); /* [한국어] 메모리 부족 시 에러 반환 */

	seqf->private = iter; /* [한국어] iter를 seqf->private에 저장하여 next/stop에서 재사용 */
	class_dev_iter_init(iter, &block_class, NULL, &disk_type); /* [한국어] block_class의 disk_type 장치만 순회하도록 초기화 */
	do {
		dev = class_dev_iter_next(iter); /* [한국어] 다음 블록 장치 순회 */
		if (!dev) /* [한국어] 더 이상 디스크 없음 */
			return NULL;
	} while (skip--); /* [한국어] *pos만큼 건너뜀 */

	return dev_to_disk(dev); /* [한국어] device에서 gendisk 역참조하여 반환 */
}

/*
 * [한국어]
 * disk_seqf_next - seq_file 이터레이터 다음 항목 반환
 *
 * @seqf: seq_file 포인터 (private에 이터레이터 저장됨).
 * @v:    현재 항목 포인터 (사용하지 않음).
 * @pos:  현재 위치 포인터 (증가시켜야 함).
 * @return: 다음 gendisk 포인터, 없으면 NULL.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 (proc read).
 *
 * 호출 체인: seq_file core → [disk_seqf_next()]
 */
static void *disk_seqf_next(struct seq_file *seqf, void *v, loff_t *pos)
{
	struct device *dev; /* [한국어] 다음 순회할 device */

	(*pos)++; /* [한국어] seq_file 위치 증가 — /proc lseek/pread 정확성 유지 */
	dev = class_dev_iter_next(seqf->private); /* [한국어] 이터레이터로 다음 블록 장치 획득 */
	if (dev)
		return dev_to_disk(dev); /* [한국어] gendisk 역참조하여 반환 */

	return NULL; /* [한국어] 더 이상 디스크 없음 */
}

/*
 * [한국어]
 * disk_seqf_stop - seq_file 이터레이터 종료 및 자원 해제
 *
 * @seqf: seq_file 포인터 (private에 이터레이터 저장됨).
 * @v:    현재 항목 포인터.
 * @return: void.
 *
 * start가 실패한 경우에도 stop이 호출되므로 iter NULL 체크 후 해제.
 * 실행 컨텍스트: 프로세스 컨텍스트 (proc read 완료 또는 에러).
 *
 * 호출 체인: seq_file core → [disk_seqf_stop()]
 */
static void disk_seqf_stop(struct seq_file *seqf, void *v)
{
	struct class_dev_iter *iter = seqf->private; /* [한국어] start에서 저장한 이터레이터 포인터 획득 */

	/* stop is called even after start failed :-( */
	if (iter) { /* [한국어] start가 실패하면 iter가 NULL일 수 있으므로 체크 */
		class_dev_iter_exit(iter); /* [한국어] 이터레이터가 획득한 참조 해제 */
		kfree(iter);               /* [한국어] start에서 할당한 이터레이터 메모리 해제 */
		seqf->private = NULL;      /* [한국어] 해제된 포인터 NULL로 초기화 */
	}
}

/*
 * [한국어]
 * show_partition_start - /proc/partitions seq_file 시작 및 헤더 출력
 *
 * @seqf: seq_file 포인터.
 * @pos:  위치 포인터.
 * @return: 첫 번째 gendisk 또는 NULL/ERR_PTR.
 *
 * disk_seqf_start()를 호출하고, 위치 0에서만 헤더("major minor #blocks name")를 출력.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인: /proc/partitions 읽기 → seq_file core → [show_partition_start()]
 */
static void *show_partition_start(struct seq_file *seqf, loff_t *pos)
{
	void *p; /* [한국어] disk_seqf_start의 반환값 저장 */

	p = disk_seqf_start(seqf, pos); /* [한국어] 첫 번째 gendisk 포인터 획득 */
	if (!IS_ERR_OR_NULL(p) && !*pos) /* [한국어] 첫 번째 항목(pos==0)이고 유효한 포인터이면 헤더 출력 */
		seq_puts(seqf, "major minor  #blocks  name\n\n"); /* [한국어] /proc/partitions 헤더 라인 출력 */
	return p; /* [한국어] 첫 번째 gendisk 포인터 반환 */
}

/*
 * [한국어]
 * show_partition - /proc/partitions의 각 디스크 항목 출력
 *
 * @seqf: seq_file 포인터 (출력 스트림).
 * @v:    현재 gendisk 포인터.
 * @return: 0 (항상).
 *
 * 각 gendisk의 파티션 목록을 "major minor #blocks name" 형식으로 출력한다.
 * 용량 0이거나 hidden 디스크는 건너뛴다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (proc read).
 *
 * 호출 체인: /proc/partitions 읽기 → seq_file core → [show_partition()]
 */
static int show_partition(struct seq_file *seqf, void *v)
{
	struct gendisk *sgp = v;   /* [한국어] 현재 순회 중인 gendisk */
	struct block_device *part; /* [한국어] 파티션 순회용 포인터 */
	unsigned long idx;         /* [한국어] xa_for_each 인덱스 */

	if (!get_capacity(sgp) || (sgp->flags & GENHD_FL_HIDDEN)) /* [한국어] 용량 0이거나 hidden 디스크는 출력 생략 */
		return 0;

	rcu_read_lock(); /* [한국어] part_tbl RCU 읽기 시작 */
	xa_for_each(&sgp->part_tbl, idx, part) { /* [한국어] 모든 파티션 순회 */
		if (!bdev_nr_sectors(part)) /* [한국어] 크기 0인 파티션은 /proc/partitions에서 생략 */
			continue;
		seq_printf(seqf, "%4d  %7d %10llu %pg\n",
			   MAJOR(part->bd_dev), MINOR(part->bd_dev),
			   bdev_nr_sectors(part) >> 1, part); /* [한국어] "major minor blocks(KB) name" 형식 출력; >>1로 섹터→KB 변환 */
	}
	rcu_read_unlock(); /* [한국어] RCU 읽기 종료 */
	return 0; /* [한국어] seq_file show 함수는 항상 0 반환 */
}

/* [한국어] /proc/partitions 출력용 seq_operations 테이블 */
static const struct seq_operations partitions_op = {
	.start	= show_partition_start, /* [한국어] 헤더 출력 + 첫 디스크 */
	.next	= disk_seqf_next,       /* [한국어] 다음 디스크 */
	.stop	= disk_seqf_stop,       /* [한국어] 이터레이터 해제 */
	.show	= show_partition        /* [한국어] 각 디스크의 파티션 라인 출력 */
};
#endif

/*
 * [한국어]
 * genhd_device_init - 블록 장치 클래스 및 /sys/block 초기화
 *
 * @return: 성공 시 0, class_register 실패 시 음수 에러 코드.
 *
 * 커널 부팅 시 subsys_initcall 단계에서 실행된다. block_class를 등록하고
 * 블록 서브시스템 내부를 초기화하며, BLOCK_EXT_MAJOR를 "blkext"로 예약하고
 * /sys/block 디렉터리를 생성한다. 이 이후에 NVMe 드라이버가 로드되어
 * gendisk를 등록할 수 있다.
 * 실행 컨텍스트: 커널 초기화 단계 (single-thread, 슬립 가능).
 *
 * 호출 체인:
 *   subsys_initcall → [genhd_device_init()] → class_register() + blk_dev_init()
 */
static int __init genhd_device_init(void)
{
	int error; /* [한국어] class_register 반환값 저장 */

	error = class_register(&block_class); /* [한국어] block_class를 /sys/class/block에 등록 */
	if (unlikely(error)) /* [한국어] 등록 실패 시 초기화 중단 */
		return error;
	blk_dev_init(); /* [한국어] blk-mq, elevator, throttle 등 블록 서브시스템 내부 초기화 */

	register_blkdev(BLOCK_EXT_MAJOR, "blkext"); /* [한국어] BLOCK_EXT_MAJOR를 "blkext"로 예약 — 확장 동적 minor 풀 소유자 */

	/* create top-level block dir */
	block_depr = kobject_create_and_add("block", NULL); /* [한국어] /sys/block 레거시 디렉터리 kobject 생성(최상위 NULL 아래) */
	return 0; /* [한국어] 초기화 성공 */
}

subsys_initcall(genhd_device_init);

/*
 * [한국어]
 * disk_range_show - /sys/block/XY/range 출력: 이 디스크가 지원하는 minor 수
 *
 * @dev: gendisk의 struct device.
 * @attr: sysfs 속성.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read).
 * 호출 체인: sysfs read → [disk_range_show()]
 */
static ssize_t disk_range_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] device → gendisk 역참조 */

	return sysfs_emit(buf, "%d\n", disk->minors); /* [한국어] 이 디스크가 지원하는 minor 번호 수 출력 */
}

/*
 * [한국어]
 * disk_ext_range_show - /sys/block/XY/ext_range 출력: 최대 파티션 수
 *
 * @dev: gendisk의 struct device.
 * @attr: sysfs 속성.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * GENHD_FL_NO_PART 설정 시 1, 아니면 DISK_MAX_PARTS를 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read).
 * 호출 체인: sysfs read → [disk_ext_range_show()]
 */
static ssize_t disk_ext_range_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] device → gendisk 역참조 */

	return sysfs_emit(buf, "%d\n",
		(disk->flags & GENHD_FL_NO_PART) ? 1 : DISK_MAX_PARTS); /* [한국어] 파티션 불가 플래그 유무에 따라 1 또는 최대 파티션 수 출력 */
}

/*
 * [한국어]
 * disk_removable_show - /sys/block/XY/removable 출력: 이동식 장치 여부
 *
 * @dev: gendisk의 struct device.
 * @attr: sysfs 속성.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * GENHD_FL_REMOVABLE 플래그 여부를 1/0으로 출력.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read).
 * 호출 체인: sysfs read → [disk_removable_show()]
 */
static ssize_t disk_removable_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] device → gendisk 역참조 */

	return sysfs_emit(buf, "%d\n",
		       (disk->flags & GENHD_FL_REMOVABLE ? 1 : 0)); /* [한국어] REMOVABLE 플래그 유무를 1/0으로 출력 */
}

/*
 * [한국어]
 * disk_hidden_show - /sys/block/XY/hidden 출력: hidden 디스크 여부
 *
 * @dev: gendisk의 struct device.
 * @attr: sysfs 속성.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * GENHD_FL_HIDDEN 플래그 여부를 1/0으로 출력.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read).
 * 호출 체인: sysfs read → [disk_hidden_show()]
 */
static ssize_t disk_hidden_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] device → gendisk 역참조 */

	return sysfs_emit(buf, "%d\n",
		       (disk->flags & GENHD_FL_HIDDEN ? 1 : 0)); /* [한국어] hidden 플래그 유무를 1/0으로 출력 */
}

/*
 * [한국어]
 * disk_ro_show - /sys/block/XY/ro 출력: 읽기 전용 여부
 *
 * @dev: gendisk의 struct device.
 * @attr: sysfs 속성.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * GD_READ_ONLY 플래그(set_disk_ro로 설정됨) 여부를 1/0으로 출력.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read).
 * 호출 체인: sysfs read → [disk_ro_show()]
 */
static ssize_t disk_ro_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] device → gendisk 역참조 */

	return sysfs_emit(buf, "%d\n", get_disk_ro(disk) ? 1 : 0); /* [한국어] GD_READ_ONLY 또는 하드웨어 쓰기 방지 상태를 1/0으로 출력 */
}

/*
 * [한국어]
 * part_size_show - /sys/block/XY/size 출력: 파티션/디스크 섹터 수
 *
 * @dev: block_device의 struct device.
 * @attr: sysfs 속성.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * 파티션 또는 전체 디스크의 512B 섹터 수를 출력한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read).
 * 호출 체인: sysfs read → [part_size_show()]
 */
ssize_t part_size_show(struct device *dev,
		       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", bdev_nr_sectors(dev_to_bdev(dev))); /* [한국어] block_device의 512B 섹터 수를 정수로 출력 */
}

/*
 * [한국어]
 * part_stat_show - /sys/block/XY/stat 출력: I/O 통계 전체
 *
 * @dev: block_device의 struct device.
 * @attr: sysfs 속성.
 * @buf: 출력 버퍼 (PAGE_SIZE).
 * @return: 출력 바이트 수.
 *
 * 진행 중인 I/O가 있으면 io_ticks를 먼저 갱신하고, 전 CPU 통계를 합산한다.
 * 출력: "ios merges sectors ms" (read/write/discard/flush 순). NVMe doorbell 왕복
 * 지연, PRP/SGL 준비 시간이 nsecs에 반영된다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read).
 *
 * 호출 체인: sysfs read → [part_stat_show()] → bdev_count_inflight()
 *                                              + part_stat_read_all()
 */
ssize_t part_stat_show(struct device *dev,
		       struct device_attribute *attr, char *buf)
{
	struct block_device *bdev = dev_to_bdev(dev); /* [한국어] device → block_device 역참조 */
	struct disk_stats stat;   /* [한국어] 합산 I/O 통계 저장 버퍼 */
	unsigned int inflight;    /* [한국어] 현재 진행 중인 I/O 수 */

	inflight = bdev_count_inflight(bdev); /* [한국어] 진행 중 I/O 수 확인 */
	if (inflight) { /* [한국어] 진행 중 I/O가 있으면 io_ticks를 현재 시간으로 갱신 */
		part_stat_lock();  /* [한국어] per-CPU 통계 읽기 전 preemption 비활성화 */
		update_io_ticks(bdev, jiffies, true); /* [한국어] 큐 점유 시간(io_ticks) 갱신 */
		part_stat_unlock(); /* [한국어] preemption 재활성화 */
	}
	/* [한국어] 모든 CPU의 per-CPU 통계를 하나의 구조체로 합산한다. 통계는
	 * 갱신 비용을 줄이려고 CPU마다 흩어져 누적되므로, 읽을 때 모아야 한다.
	 * CPU가 많으면 이 합산 자체가 비싸기 때문에, 이 파일을 초당 수십 번
	 * 읽는 모니터링 도구는 그 자체로 부하가 된다는 점에 유의해야 한다. */
	part_stat_read_all(bdev, &stat);
	/* [한국어] ★ /sys/block/nvme0n1/stat 출력 형식 ★
	 * 공백으로 구분된 17개 필드를 한 줄로 낸다. /proc/diskstats의 뒷부분과
	 * 같은 순서이며, iostat이 이 값들의 시간당 변화율로 지표를 계산한다.
	 * 아래 인자들이 그 17개 필드에 순서대로 대응한다. */
	return sysfs_emit(buf,
		"%8lu %8lu %8llu %8u "
		"%8lu %8lu %8llu %8u "
		"%8u %8u %8u "
		"%8lu %8lu %8llu %8u "
		"%8lu %8u"
		"\n",
		/* [한국어] 1) 완료된 읽기 I/O 수 → iostat의 r/s */
		stat.ios[STAT_READ],
		/* [한국어] 2) 병합된 읽기 수 → iostat의 rrqm/s.
		 * 이 값이 크면 순차 읽기가 잘 합쳐져 NVMe 커맨드 수가 절약되고 있다는 뜻이다. */
		stat.merges[STAT_READ],
		/* [한국어] 3) 읽은 섹터 수(512B 단위) → iostat의 rkB/s 근거.
		 * 4Kn NVMe라도 이 값은 항상 512B 단위다. */
		(unsigned long long)stat.sectors[STAT_READ],
		/* [한국어] 4) 읽기에 소비된 누적 시간(ms). ns 누적값을 밀리초로 나눈다.
		 * 이 값 ÷ 읽기 수 = 평균 지연(iostat의 r_await).
		 * div_u64를 쓰는 이유: 32비트 아키텍처에서 u64를 그대로 나누면
		 * 링크 에러가 나므로 전용 헬퍼가 필요하다. */
		(unsigned int)div_u64(stat.nsecs[STAT_READ], NSEC_PER_MSEC),
		/* [한국어] 5~8) 쓰기에 대한 같은 네 항목 → w/s, wrqm/s, wkB/s, w_await */
		stat.ios[STAT_WRITE],
		stat.merges[STAT_WRITE],
		(unsigned long long)stat.sectors[STAT_WRITE],
		(unsigned int)div_u64(stat.nsecs[STAT_WRITE], NSEC_PER_MSEC),
		/* [한국어] 9) 현재 진행 중인 I/O 수 → iostat의 aqu-sz 근거.
		 * NVMe 관점에서는 SQ에 제출되어 아직 CQ 완료가 오지 않은 커맨드 수다. */
		inflight,
		/* [한국어] 10) 장치가 바빴던 누적 시간(ms) → iostat의 %util 근거.
		 * 주의: NVMe처럼 큐 깊이가 깊은 장치에서 %util은 포화도를 뜻하지 않는다.
		 * 커맨드 하나만 진행 중이어도 100%로 표시되므로, 이 값이 100%라고
		 * 해서 장치가 한계에 도달한 것이 아니다. */
		jiffies_to_msecs(stat.io_ticks),
		/* [한국어] 11) 모든 방향의 누적 대기 시간 합(ms). 읽기·쓰기·discard·
		 * flush를 전부 더한 값으로, 큐에 쌓인 총 대기 시간을 나타낸다. */
		(unsigned int)div_u64(stat.nsecs[STAT_READ] +
				      stat.nsecs[STAT_WRITE] +
				      stat.nsecs[STAT_DISCARD] +
				      stat.nsecs[STAT_FLUSH],
						NSEC_PER_MSEC),
		/* [한국어] 12~15) discard 통계 4항목. NVMe에서는 Dataset Management
		 * (옵코드 0x09) 커맨드에 해당한다. sectors는 실제로 전송된 데이터가
		 * 아니라 "무효화를 요청한 LBA 범위의 크기"임에 유의. */
		stat.ios[STAT_DISCARD],
		stat.merges[STAT_DISCARD],
		(unsigned long long)stat.sectors[STAT_DISCARD],
		(unsigned int)div_u64(stat.nsecs[STAT_DISCARD], NSEC_PER_MSEC),
		/* [한국어] 16~17) flush 통계 2항목. NVMe Flush(옵코드 0x00)에 해당하며,
		 * 데이터 전송이 없어 sectors/merges 항목이 없다.
		 * fsync가 잦은 워크로드에서 이 값과 지연이 함께 커진다. */
		stat.ios[STAT_FLUSH],
		(unsigned int)div_u64(stat.nsecs[STAT_FLUSH], NSEC_PER_MSEC));
}

/*
 * [한국어]
 * part_inflight_show - /sys/block/XY/inflight 출력: 진행 중 I/O 수
 *
 * @dev: block_device의 struct device.
 * @attr: sysfs 속성.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * 현재 드라이버에 제출된 진행 중인 I/O 수를 read/write 별로 출력한다.
 * NVMe: blk_mq_in_driver_rw()로 SQ에 있으나 CQ에서 미완료된 CID 수.
 * bio-based: bdev_start_io_acct()로 시작된 I/O의 per-CPU 카운터 합산.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read).
 *
 * 호출 체인: sysfs read → [part_inflight_show()] → bdev_count_inflight_rw()
 */
/*
 * Show the number of IOs issued to driver.
 * For bio-based device, started from bdev_start_io_acct();
 * For rq-based device, started from blk_mq_start_request();
 */
ssize_t part_inflight_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct block_device *bdev = dev_to_bdev(dev);     /* [한국어] device → block_device 역참조 */
	struct request_queue *q = bdev_get_queue(bdev);   /* [한국어] block_device의 request_queue 획득 */
	unsigned int inflight[2] = {0};                   /* [한국어] [0]=READ, [1]=WRITE 진행 수 버퍼 */

	bdev_count_inflight_rw(bdev, inflight, queue_is_mq(q)); /* [한국어] blk-mq면 태그 기반, 아니면 per-CPU 합산 */

	return sysfs_emit(buf, "%8u %8u\n", inflight[READ], inflight[WRITE]); /* [한국어] "read write" 형식으로 출력 */
}

/*
 * [한국어]
 * disk_capability_show - /sys/block/XY/capability 출력 (deprecated)
 *
 * @dev: gendisk의 struct device.
 * @attr: sysfs 속성.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * 레거시 capability 속성. 항상 0을 반환하며 deprecated 경고를 출력.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read).
 * 호출 체인: sysfs read → [disk_capability_show()]
 */
static ssize_t disk_capability_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	dev_warn_once(dev, "the capability attribute has been deprecated.\n"); /* [한국어] capability 속성이 제거 예정임을 1회 경고 */
	return sysfs_emit(buf, "0\n"); /* [한국어] 레거시 호환을 위해 항상 0 반환 */
}

/*
 * [한국어]
 * disk_alignment_offset_show - /sys/block/XY/alignment_offset 출력
 *
 * @dev: gendisk의 struct device.
 * @attr: sysfs 속성.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * 블록 장치의 논리 섹터 정렬 오프셋(바이트 단위)을 출력한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read).
 * 호출 체인: sysfs read → [disk_alignment_offset_show()]
 */
static ssize_t disk_alignment_offset_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] device → gendisk 역참조 */

	return sysfs_emit(buf, "%d\n", bdev_alignment_offset(disk->part0)); /* [한국어] part0의 논리 섹터 정렬 오프셋(바이트) 출력 */
}

/*
 * [한국어]
 * disk_discard_alignment_show - /sys/block/XY/discard_alignment 출력
 *
 * @dev: gendisk의 struct device.
 * @attr: sysfs 속성.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * discard(TRIM) 명령의 정렬 오프셋을 출력한다 (현재 alignment_offset과 동일).
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read).
 * 호출 체인: sysfs read → [disk_discard_alignment_show()]
 */
static ssize_t disk_discard_alignment_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] device → gendisk 역참조 */

	return sysfs_emit(buf, "%d\n", bdev_alignment_offset(disk->part0)); /* [한국어] discard 정렬 오프셋(바이트) 출력 — 현재 alignment_offset과 동일 */
}

/*
 * [한국어]
 * diskseq_show - /sys/block/XY/diskseq 출력: 장치 일련번호
 *
 * @dev: gendisk의 struct device.
 * @attr: sysfs 속성.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * gendisk의 단조 증가 일련번호(diskseq)를 출력한다. 유저스페이스가
 * 동일 이름으로 재사용된 장치를 구분하는 데 사용한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read).
 * 호출 체인: sysfs read → [diskseq_show()]
 */
static ssize_t diskseq_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] device → gendisk 역참조 */

	return sysfs_emit(buf, "%llu\n", disk->diskseq); /* [한국어] 단조 증가 장치 일련번호 출력 */
}

/*
 * [한국어]
 * partscan_show - /sys/block/XY/partscan 출력: 파티션 스캔 가능 여부
 *
 * @dev: gendisk의 struct device.
 * @attr: sysfs 속성.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * GENHD_FL_NO_PART 플래그 등에 따라 파티션 스캔이 가능한지 여부를 1/0으로 출력.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read).
 * 호출 체인: sysfs read → [partscan_show()]
 */
static ssize_t partscan_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", disk_has_partscan(dev_to_disk(dev))); /* [한국어] 파티션 스캔 가능 여부를 1/0으로 출력 */
}

/* [한국어] 각 sysfs 속성을 DEVICE_ATTR 매크로로 선언: /sys/block/XY/ 아래 노출 */
static DEVICE_ATTR(range, 0444, disk_range_show, NULL);            /* [한국어] /range: 지원하는 minor 수 */
static DEVICE_ATTR(ext_range, 0444, disk_ext_range_show, NULL);    /* [한국어] /ext_range: 최대 파티션 수 */
static DEVICE_ATTR(removable, 0444, disk_removable_show, NULL);    /* [한국어] /removable: 이동식 장치 여부 */
static DEVICE_ATTR(hidden, 0444, disk_hidden_show, NULL);          /* [한국어] /hidden: hidden 디스크 여부 */
static DEVICE_ATTR(ro, 0444, disk_ro_show, NULL);                  /* [한국어] /ro: 읽기 전용 여부 */
static DEVICE_ATTR(size, 0444, part_size_show, NULL);              /* [한국어] /size: 섹터 수 */
static DEVICE_ATTR(alignment_offset, 0444, disk_alignment_offset_show, NULL); /* [한국어] /alignment_offset: 정렬 오프셋 */
static DEVICE_ATTR(discard_alignment, 0444, disk_discard_alignment_show, NULL); /* [한국어] /discard_alignment: discard 정렬 오프셋 */
static DEVICE_ATTR(capability, 0444, disk_capability_show, NULL);  /* [한국어] /capability: deprecated 속성, 항상 0 */
static DEVICE_ATTR(stat, 0444, part_stat_show, NULL);              /* [한국어] /stat: I/O 통계 */
static DEVICE_ATTR(inflight, 0444, part_inflight_show, NULL);      /* [한국어] /inflight: 진행 중 I/O 수 */
static DEVICE_ATTR(badblocks, 0644, disk_badblocks_show, disk_badblocks_store); /* [한국어] /badblocks: 불량 섹터 목록 (읽기/쓰기) */
static DEVICE_ATTR(diskseq, 0444, diskseq_show, NULL);             /* [한국어] /diskseq: 장치 일련번호 */
static DEVICE_ATTR(partscan, 0444, partscan_show, NULL);           /* [한국어] /partscan: 파티션 스캔 가능 여부 */

#ifdef CONFIG_FAIL_MAKE_REQUEST
/*
 * [한국어]
 * part_fail_show - /sys/block/XY/make-it-fail 읽기: I/O 강제 실패 여부
 *
 * @dev: block_device의 struct device.
 * @attr: sysfs 속성.
 * @buf: 출력 버퍼.
 * @return: 출력 바이트 수.
 *
 * BD_MAKE_IT_FAIL 플래그 상태를 출력한다. 설정 시 모든 I/O가 -EIO로 실패.
 * fault injection 테스트에 사용. CONFIG_FAIL_MAKE_REQUEST 빌드 옵션 필요.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs read).
 * 호출 체인: sysfs read → [part_fail_show()]
 */
ssize_t part_fail_show(struct device *dev,
		       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n",
		       bdev_test_flag(dev_to_bdev(dev), BD_MAKE_IT_FAIL)); /* [한국어] BD_MAKE_IT_FAIL 플래그 상태(0/1) 출력 */
}

/*
 * [한국어]
 * part_fail_store - /sys/block/XY/make-it-fail 쓰기: I/O 강제 실패 설정
 *
 * @dev: block_device의 struct device.
 * @attr: sysfs 속성.
 * @buf: 입력 버퍼.
 * @count: 입력 바이트 수.
 * @return: count.
 *
 * "1" 쓰면 BD_MAKE_IT_FAIL 설정, "0" 쓰면 해제.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs write).
 * 호출 체인: sysfs write → [part_fail_store()]
 */
ssize_t part_fail_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int i; /* [한국어] 파싱된 정수값(0 또는 1) 저장 */

	if (count > 0 && sscanf(buf, "%d", &i) > 0) { /* [한국어] 입력에서 정수 파싱 */
		if (i) /* [한국어] 1이면 강제 실패 플래그 설정 */
			bdev_set_flag(dev_to_bdev(dev), BD_MAKE_IT_FAIL); /* [한국어] BD_MAKE_IT_FAIL 설정 — 이후 I/O가 -EIO로 실패 */
		else /* [한국어] 0이면 플래그 해제 */
			bdev_clear_flag(dev_to_bdev(dev), BD_MAKE_IT_FAIL); /* [한국어] BD_MAKE_IT_FAIL 해제 — 정상 I/O 복원 */
	}
	return count; /* [한국어] 성공 시 입력 바이트 수 반환 */
}

static struct device_attribute dev_attr_fail =
	__ATTR(make-it-fail, 0644, part_fail_show, part_fail_store);
#endif /* CONFIG_FAIL_MAKE_REQUEST */

#ifdef CONFIG_FAIL_IO_TIMEOUT
static struct device_attribute dev_attr_fail_timeout =
	__ATTR(io-timeout-fail, 0644, part_timeout_show, part_timeout_store);
#endif

/*
 * [한국어] /sys/block/<disk>/ 아래에 만들어질 속성 파일 목록.
 * 이 배열이 곧 사용자 공간이 디스크에 대해 볼 수 있는 정보의 전부다
 * (queue/ 하위 디렉터리는 blk-sysfs.c가 따로 만든다).
 * NULL로 끝나야 하며, disk_visible() 콜백이 일부를 조건부로 숨긴다.
 */
static struct attribute *disk_attrs[] = {
	/* [한국어] range: 이 디스크가 가질 수 있는 최대 파티션 수(minor 개수).
	 * NVMe는 보통 GENHD_FL_EXT_DEVT를 쓰므로 1이 나오고, 실제 파티션은
	 * 확장 devt 공간에서 할당된다. */
	&dev_attr_range.attr,
	/* [한국어] ext_range: 확장 devt를 포함한 파티션 상한. */
	&dev_attr_ext_range.attr,
	/* [한국어] removable: 매체를 뺄 수 있는 장치인가(CD/USB는 1).
	 * NVMe SSD는 0이다 — 컨트롤러 자체를 뽑는 것은 hot-unplug이지
	 * "매체 교체"가 아니다. */
	&dev_attr_removable.attr,
	/* [한국어] hidden: 사용자에게 노출하지 않을 디스크인가.
	 * NVMe 멀티패스에서 개별 경로 디스크(nvme0c0n1 등)가 이 플래그를 갖는다 —
	 * 사용자는 통합된 헤드 디스크(nvme0n1)만 봐야 하기 때문이다. */
	&dev_attr_hidden.attr,
	/* [한국어] ro: 읽기 전용 여부. NVMe에서는 네임스페이스가 write-protected
	 * 이거나 호스트가 NVME_NS_FORCE_RO를 설정한 경우 1이 된다. */
	&dev_attr_ro.attr,
	/* [한국어] size: 512B 섹터 단위 용량. NVMe Identify Namespace의 NSZE와
	 * LBA 크기에서 유도된다. */
	&dev_attr_size.attr,
	/* [한국어] alignment_offset: 논리 블록 0이 물리 블록 경계에서 얼마나
	 * 밀려 있는가. 파티션 도구가 이 값을 보고 파티션 시작을 정렬한다. */
	&dev_attr_alignment_offset.attr,
	/* [한국어] discard_alignment: discard granularity 경계 기준의 오프셋.
	 * NVMe에서는 NPDA/NPDAL에서 유도된다. */
	&dev_attr_discard_alignment.attr,
	/* [한국어] capability: 장치 능력 비트마스크(레거시 인터페이스). */
	&dev_attr_capability.attr,
	/* [한국어] stat: 위 disk_stat_show()가 내는 17개 필드 I/O 통계.
	 * iostat과 모니터링 도구의 주 데이터 소스다. */
	&dev_attr_stat.attr,
	/* [한국어] inflight: 진행 중인 읽기/쓰기 수. NVMe 관점에서는 SQ에 제출되어
	 * 아직 완료가 오지 않은 커맨드 수에 해당한다. */
	&dev_attr_inflight.attr,
	/* [한국어] badblocks: 불량 블록 목록. disk->bb가 없으면
	 * disk_visible()이 이 항목만 숨긴다(NVMe는 보통 설정하지 않는다). */
	&dev_attr_badblocks.attr,
	/* [한국어] events / events_async / events_poll_msecs: 매체 변경 이벤트
	 * 폴링 설정. 제거 가능 매체용이라 NVMe에서는 거의 쓰이지 않는다. */
	&dev_attr_events.attr,
	&dev_attr_events_async.attr,
	&dev_attr_events_poll_msecs.attr,
	/* [한국어] diskseq: 디스크 인스턴스의 고유 일련번호. 같은 이름(nvme0n1)이
	 * 재사용되더라도 이 값은 달라지므로, 사용자 공간이 "같은 이름의 다른
	 * 장치"를 구분할 수 있다. hot-plug가 잦은 환경에서 중요하다. */
	&dev_attr_diskseq.attr,
	/* [한국어] partscan: 파티션 스캔이 활성화되어 있는가. */
	&dev_attr_partscan.attr,
#ifdef CONFIG_FAIL_MAKE_REQUEST
	/* [한국어] make-it-fail: 이 디스크에 fault injection을 걸지 여부.
	 * 여기에 1을 쓰고 debugfs에서 확률을 설정해야 실제로 주입된다
	 * (block/blk-core.c의 should_fail_request 참고). */
	&dev_attr_fail.attr,
#endif
#ifdef CONFIG_FAIL_IO_TIMEOUT
	/* [한국어] io-timeout-fail: 타임아웃 경로 fault injection.
	 * NVMe의 nvme_timeout → Abort → 컨트롤러 리셋 복구 경로를 실제 고장
	 * 없이 시험할 때 쓴다. */
	&dev_attr_fail_timeout.attr,
#endif
	NULL
};

/*
 * [한국어]
 * disk_visible - 특정 sysfs 속성의 가시성 결정 콜백
 *
 * @kobj: gendisk의 kobject.
 * @a: 확인할 sysfs 속성 포인터.
 * @n: 속성 인덱스 (사용하지 않음).
 * @return: 속성 파일 모드 (0이면 숨김).
 *
 * disk_attr_group.is_visible 콜백으로, 조건에 따라 일부 속성을 숨긴다.
 * 현재는 bad block 테이블이 없으면 badblocks 속성을 숨긴다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (sysfs 초기화 시).
 *
 * 호출 체인: sysfs kobject 등록 → [disk_visible()]
 */
static umode_t disk_visible(struct kobject *kobj, struct attribute *a, int n)
{
	struct device *dev = container_of(kobj, typeof(*dev), kobj); /* [한국어] kobject에서 device 역참조 */
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] device에서 gendisk 역참조 */

	if (a == &dev_attr_badblocks.attr && !disk->bb) /* [한국어] bad block 테이블 미설정 시 badblocks 속성 숨김 */
		return 0; /* [한국어] 0 반환 시 sysfs에서 속성 파일 생성 안 함 */
	return a->mode; /* [한국어] 그 외 속성은 정의된 모드(0444 등)로 노출 */
}

/* [한국어] gendisk의 sysfs 속성 그룹 — disk_attrs 배열과 disk_visible 가시성 콜백을 묶음 */
static struct attribute_group disk_attr_group = {
	.attrs = disk_attrs,       /* [한국어] /sys/block/XY/ 아래에 생성할 속성 포인터 배열 */
	.is_visible = disk_visible, /* [한국어] 조건부 속성 숨김 콜백 */
};

/* [한국어] gendisk device_type에 등록할 속성 그룹 배열 — NULL 종료 */
static const struct attribute_group *disk_attr_groups[] = {
	&disk_attr_group,       /* [한국어] 기본 디스크 속성 그룹 */
#ifdef CONFIG_BLK_DEV_IO_TRACE
	&blk_trace_attr_group,  /* [한국어] I/O 추적(blktrace) 속성 그룹 */
#endif
#ifdef CONFIG_BLK_DEV_INTEGRITY
	&blk_integrity_attr_group, /* [한국어] 무결성(T10 DIF/PI) 속성 그룹 */
#endif
	NULL /* [한국어] 배열 종료 마커 */
};

/*
 * [한국어]
 * disk_release - gendisk의 모든 자원 해제 (device_release 콜백)
 *
 * @dev: gendisk에 내장된 struct device 포인터.
 * @return: void.
 *
 * gendisk의 참조 카운트가 0이 될 때 device 코어가 이 함수를 호출한다.
 * blk-mq 기반 NVMe 드라이버의 경우 request_queue refcount도 여기서 0이 되어
 * queue가 해제된다. probe 실패로 add_disk가 호출되지 않은 경우에는
 * blk_mq_exit_queue를 여기서 직접 호출한다(태그셋이 아직 유효한 동안).
 * 드라이버가 add_disk+del_gendisk를 거친 경우 이 해제는 del_gendisk에서 이미
 * 수행되었으므로 이 함수에서는 생략한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (슬립 가능).
 *
 * 호출 체인:
 *   put_disk() → put_device() → device_release() → [disk_release()]
 *                              → blk_put_queue() → (queue 해제)
 */
/**
 * disk_release - releases all allocated resources of the gendisk
 * @dev: the device representing this disk
 *
 * This function releases all allocated resources of the gendisk.
 *
 * Drivers which used device_add_disk() have a gendisk with a request_queue
 * assigned. Since the request_queue sits on top of the gendisk for these
 * drivers we also call blk_put_queue() for them, and we expect the
 * request_queue refcount to reach 0 at this point, and so the request_queue
 * will also be freed prior to the disk.
 *
 * Context: can sleep
 */
static void disk_release(struct device *dev)
{
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] device → gendisk 역참조 */

	might_sleep(); /* [한국어] 이 함수는 슬립 가능한 컨텍스트에서 호출되어야 함을 명시 */
	WARN_ON_ONCE(disk_live(disk)); /* [한국어] 아직 살아있는 디스크가 release되면 버그 경고 */

	blk_trace_remove(disk->queue); /* [한국어] blktrace I/O 추적 자원 해제 */

	/*
	 * To undo the all initialization from blk_mq_init_allocated_queue in
	 * case of a probe failure where add_disk is never called we have to
	 * call blk_mq_exit_queue here. We can't do this for the more common
	 * teardown case (yet) as the tagset can be gone by the time the disk
	 * is released once it was added.
	 */
	if (queue_is_mq(disk->queue) && /* [한국어] blk-mq 드라이버(NVMe 포함)이고 */
	    test_bit(GD_OWNS_QUEUE, &disk->state) && /* [한국어] queue를 소유하며 */
	    !test_bit(GD_ADDED, &disk->state)) /* [한국어] add_disk가 호출되지 않은 probe 실패 경우 */
		blk_mq_exit_queue(disk->queue); /* [한국어] 태그셋이 아직 유효할 때 blk-mq queue 내부 자원 정리 */

	blkcg_exit_disk(disk); /* [한국어] 블록 cgroup 연결 해제 */

	bioset_exit(&disk->bio_split); /* [한국어] bio 분할 mempool 해제 */

	disk_release_events(disk); /* [한국어] 이벤트 폴링 자원 해제 */
	kfree(disk->random);       /* [한국어] 엔트로피 시드 메모리 해제 */
	disk_free_zone_resources(disk); /* [한국어] zoned block device 자원 해제 */
	xa_destroy(&disk->part_tbl); /* [한국어] 파티션 XArray 해제 */

	kobject_put(&disk->queue_kobj); /* [한국어] queue kobject 참조 해제 */
	disk->queue->disk = NULL;       /* [한국어] queue의 역참조 gendisk 포인터 제거 */
	blk_put_queue(disk->queue);     /* [한국어] request_queue 참조 해제 — 0이 되면 queue 메모리 해제 */

	if (test_bit(GD_ADDED, &disk->state) && disk->fops->free_disk) /* [한국어] 정상 등록된 디스크이고 드라이버가 free_disk 콜백을 제공한 경우 */
		disk->fops->free_disk(disk); /* [한국어] 드라이버별 추가 자원 해제(예: NVMe ns 구조체) */

	bdev_drop(disk->part0);	/* frees the disk */ /* [한국어] part0 block_device inode 해제 — gendisk 메모리도 여기서 해제 */
}

/*
 * [한국어]
 * block_uevent - block_class의 uevent 환경변수 추가 콜백
 *
 * @dev: gendisk의 struct device.
 * @env: uevent 환경변수 버퍼.
 * @return: 성공 시 0, 실패 시 음수 에러 코드.
 *
 * 블록 장치 uevent에 "DISKSEQ=NNN" 환경변수를 추가한다. 유저스페이스(udev)가
 * 이 변수로 장치 재사용을 구분하여 ADD/REMOVE 이벤트를 정확히 연결한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (kobject_uevent 경로).
 *
 * 호출 체인: kobject_uevent() → block_class.dev_uevent() → [block_uevent()]
 */
static int block_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct gendisk *disk = dev_to_disk(dev); /* [한국어] device → gendisk 역참조 */

	return add_uevent_var(env, "DISKSEQ=%llu", disk->diskseq); /* [한국어] "DISKSEQ=NNN" 환경변수를 uevent에 추가 */
}

/* [한국어] block_class: 모든 블록 장치를 /sys/class/block 아래에 노출하는 장치 클래스 */
const struct class block_class = {
	.name		= "block",        /* [한국어] /sys/class/block 디렉터리 이름 */
	.dev_uevent	= block_uevent, /* [한국어] uevent 시 DISKSEQ 환경변수 추가 콜백 */
};

/*
 * [한국어]
 * block_devnode - 장치 노드 경로 반환 콜백
 *
 * @dev: gendisk의 struct device.
 * @mode: 반환할 장치 노드 파일 권한.
 * @uid, @gid: 반환할 소유자/그룹.
 * @return: 장치 노드 경로 문자열(동적 할당) 또는 NULL(기본 이름 사용).
 *
 * devtmpfs/udev가 /dev 아래에 장치 노드를 생성할 때 이름을 결정하는 콜백.
 * 드라이버가 fops->devnode를 제공하면 그 값을 사용하고, 없으면 NULL을 반환하여
 * 기본 디스크 이름(disk->disk_name)을 사용한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (devtmpfs 경로).
 *
 * 호출 체인: devtmpfs → disk_type.devnode() → [block_devnode()]
 */
static char *block_devnode(const struct device *dev, umode_t *mode,
			   kuid_t *uid, kgid_t *gid)
{
	struct gendisk *disk = dev_to_disk(dev); /* [한국어] device → gendisk 역참조 */

	if (disk->fops->devnode) /* [한국어] 드라이버가 커스텀 devnode 경로를 제공하면 사용 */
		return disk->fops->devnode(disk, mode); /* [한국어] 드라이버 콜백에서 동적 경로 생성 */
	return NULL; /* [한국어] NULL 반환 시 devtmpfs는 기본 디스크 이름(disk_name)으로 노드 생성 */
}

/* [한국어] disk_type: gendisk device의 타입 정보 — 이름, 속성 그룹, 해제 콜백, devnode 콜백을 묶음 */
const struct device_type disk_type = {
	.name		= "disk",              /* [한국어] sysfs에서 "disk" 타입으로 식별 */
	.groups		= disk_attr_groups,    /* [한국어] /sys/block/XY/ 아래 노출할 속성 그룹 배열 */
	.release	= disk_release,        /* [한국어] refcount 0시 호출되는 자원 해제 콜백 */
	.devnode	= block_devnode,       /* [한국어] /dev 노드 경로 결정 콜백 */
};

#ifdef CONFIG_PROC_FS
/*
 * [한국어]
 * diskstats_show - /proc/diskstats 각 항목 출력
 *
 * @seqf: seq_file 포인터 (출력 스트림).
 * @v:    현재 gendisk 포인터.
 * @return: 0 (항상).
 *
 * /proc/diskstats의 각 디스크/파티션 행을 출력한다. part_stat_show와 동일한
 * 통계를 사용하지만 하나의 seq_file로 모든 디스크를 순회한다.
 * 크기 0인 파티션은 건너뛰고, 진행 중 I/O가 있으면 io_ticks를 먼저 갱신한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (/proc/diskstats 읽기).
 *
 * 호출 체인: /proc/diskstats 읽기 → diskstats_op.show → [diskstats_show()]
 */
/*
 * aggregate disk stat collector.  Uses the same stats that the sysfs
 * entries do, above, but makes them available through one seq_file.
 *
 * The output looks suspiciously like /proc/partitions with a bunch of
 * extra fields.
 */
static int diskstats_show(struct seq_file *seqf, void *v)
{
	struct gendisk *gp = v;     /* [한국어] 현재 순회 중인 gendisk */
	struct block_device *hd;    /* [한국어] 파티션 순회용 block_device */
	unsigned int inflight;      /* [한국어] 현재 진행 중인 I/O 수 */
	struct disk_stats stat;     /* [한국어] 합산 I/O 통계 저장 버퍼 */
	unsigned long idx;          /* [한국어] xa_for_each 인덱스 */

	/*
	if (&disk_to_dev(gp)->kobj.entry == block_class.devices.next)
		seq_puts(seqf,	"major minor name"
				"     rio rmerge rsect ruse wio wmerge "
				"wsect wuse running use aveq"
				"\n\n");
	*/

	rcu_read_lock(); /* [한국어] part_tbl RCU 읽기 시작 */
	xa_for_each(&gp->part_tbl, idx, hd) { /* [한국어] 모든 파티션(+part0) 순회 */
		if (bdev_is_partition(hd) && !bdev_nr_sectors(hd)) /* [한국어] 크기 0인 파티션은 /proc/diskstats에서 생략 */
			continue;

		inflight = bdev_count_inflight(hd); /* [한국어] 진행 중 I/O 수 확인 */
		if (inflight) { /* [한국어] 진행 중 I/O가 있으면 io_ticks 갱신 */
			part_stat_lock();  /* [한국어] per-CPU 통계 읽기 보호 */
			update_io_ticks(hd, jiffies, true); /* [한국어] 큐 점유 시간 최신화 */
			part_stat_unlock(); /* [한국어] 보호 해제 */
		}
		part_stat_read_all(hd, &stat); /* [한국어] 전 CPU 통계 합산 */
		seq_put_decimal_ull_width(seqf, "",  MAJOR(hd->bd_dev), 4); /* [한국어] major 번호 (4자리 고정폭) */
		seq_put_decimal_ull_width(seqf, " ", MINOR(hd->bd_dev), 7); /* [한국어] minor 번호 (7자리 고정폭) */
		seq_printf(seqf, " %pg", hd); /* [한국어] 장치 이름(예: nvme0n1p1) */	
		seq_put_decimal_ull(seqf, " ", stat.ios[STAT_READ]);     /* [한국어] 읽기 완료 I/O 수 */
		seq_put_decimal_ull(seqf, " ", stat.merges[STAT_READ]);   /* [한국어] 읽기 병합 수 */
		seq_put_decimal_ull(seqf, " ", stat.sectors[STAT_READ]);  /* [한국어] 읽기 섹터 수 */
		seq_put_decimal_ull(seqf, " ", (unsigned int)div_u64(stat.nsecs[STAT_READ],
								     NSEC_PER_MSEC)); /* [한국어] 읽기 총 시간(ms) */
		seq_put_decimal_ull(seqf, " ", stat.ios[STAT_WRITE]);     /* [한국어] 쓰기 완료 I/O 수 */
		seq_put_decimal_ull(seqf, " ", stat.merges[STAT_WRITE]);  /* [한국어] 쓰기 병합 수 */
		seq_put_decimal_ull(seqf, " ", stat.sectors[STAT_WRITE]); /* [한국어] 쓰기 섹터 수 */
		seq_put_decimal_ull(seqf, " ", (unsigned int)div_u64(stat.nsecs[STAT_WRITE],
								     NSEC_PER_MSEC)); /* [한국어] 쓰기 총 시간(ms) */
		seq_put_decimal_ull(seqf, " ", inflight);                 /* [한국어] 현재 진행 중 I/O 수 */
		seq_put_decimal_ull(seqf, " ", jiffies_to_msecs(stat.io_ticks)); /* [한국어] 큐 점유 시간(ms) */
		seq_put_decimal_ull(seqf, " ", (unsigned int)div_u64(stat.nsecs[STAT_READ] +
								     stat.nsecs[STAT_WRITE] +
								     stat.nsecs[STAT_DISCARD] +
								     stat.nsecs[STAT_FLUSH],
								     NSEC_PER_MSEC)); /* [한국어] 전체 I/O 누적 대기 시간(ms) */
		seq_put_decimal_ull(seqf, " ", stat.ios[STAT_DISCARD]);   /* [한국어] discard(TRIM) 완료 수 */
		seq_put_decimal_ull(seqf, " ", stat.merges[STAT_DISCARD]); /* [한국어] discard 병합 수 */
		seq_put_decimal_ull(seqf, " ", stat.sectors[STAT_DISCARD]); /* [한국어] discard 섹터 수 */
		seq_put_decimal_ull(seqf, " ", (unsigned int)div_u64(stat.nsecs[STAT_DISCARD],
								     NSEC_PER_MSEC)); /* [한국어] discard 총 시간(ms) */
		seq_put_decimal_ull(seqf, " ", stat.ios[STAT_FLUSH]);     /* [한국어] flush 완료 수 */
		seq_put_decimal_ull(seqf, " ", (unsigned int)div_u64(stat.nsecs[STAT_FLUSH],
								     NSEC_PER_MSEC)); /* [한국어] flush 총 시간(ms) */
		seq_putc(seqf, '\n'); /* [한국어] 행 종료 */
	}
	rcu_read_unlock(); /* [한국어] RCU 읽기 종료 */

	return 0; /* [한국어] seq_file show 함수는 항상 0 반환 */
}

/* [한국어] /proc/diskstats 출력용 seq_operations 테이블 */
static const struct seq_operations diskstats_op = {
	.start	= disk_seqf_start,    /* [한국어] 이터레이터 초기화 */
	.next	= disk_seqf_next,     /* [한국어] 다음 디스크 */
	.stop	= disk_seqf_stop,     /* [한국어] 이터레이터 해제 */
	.show	= diskstats_show      /* [한국어] 각 디스크의 통계 라인 출력 */
};

/*
 * [한국어]
 * proc_genhd_init - /proc/diskstats 및 /proc/partitions 파일 등록
 *
 * @return: 0 (항상).
 *
 * 커널 모듈 초기화 시 proc 파일시스템에 diskstats와 partitions 파일을 생성한다.
 * 이후 cat /proc/diskstats 또는 /proc/partitions로 I/O 통계와 파티션 목록을 조회 가능.
 * 실행 컨텍스트: 커널 초기화 단계.
 *
 * 호출 체인: module_init → [proc_genhd_init()] → proc_create_seq()
 */
static int __init proc_genhd_init(void)
{
	proc_create_seq("diskstats", 0, NULL, &diskstats_op);   /* [한국어] /proc/diskstats 파일 생성 */
	proc_create_seq("partitions", 0, NULL, &partitions_op); /* [한국어] /proc/partitions 파일 생성 */
	return 0; /* [한국어] 항상 성공 */
}
module_init(proc_genhd_init);
#endif /* CONFIG_PROC_FS */

/*
 * [한국어]
 * part_devt - 파티션 번호로 dev_t 조회
 *
 * @disk:   조회할 gendisk 포인터.
 * @partno: 파티션 번호 (0=전체 디스크, 1~N=파티션).
 * @return: 해당 파티션의 dev_t, 없으면 0.
 *
 * part_tbl XArray에서 partno에 해당하는 block_device를 RCU로 읽어 bd_dev를 반환한다.
 * 실행 컨텍스트: RCU read 가능한 모든 컨텍스트.
 *
 * 호출 체인: 파티션 스캔 코드 → [part_devt()]
 */
dev_t part_devt(struct gendisk *disk, u8 partno)
{
	struct block_device *part; /* [한국어] 파티션 block_device 포인터 */
	dev_t devt = 0;            /* [한국어] 반환할 dev_t, 기본값 0(없음) */

	rcu_read_lock(); /* [한국어] part_tbl RCU 읽기 시작 */
	part = xa_load(&disk->part_tbl, partno); /* [한국어] partno 인덱스로 XArray에서 block_device 조회 */
	if (part) /* [한국어] 해당 파티션이 존재하면 */
		devt = part->bd_dev; /* [한국어] block_device의 dev_t 값 복사 */
	rcu_read_unlock(); /* [한국어] RCU 읽기 종료 */

	return devt; /* [한국어] dev_t 반환 (없으면 0) */
}

/*
 * [한국어]
 * __alloc_disk_node - 지정 NUMA 노드에 gendisk 할당 및 초기화
 *
 * @q:       연결할 request_queue 포인터 (NVMe blk_mq_tag_set에서 파생됨).
 * @node_id: 메모리 할당에 사용할 NUMA 노드 번호.
 * @lkclass: lockdep 클래스 키 (bio completion 락 추적용).
 * @return:  성공 시 초기화된 gendisk 포인터, 실패 시 NULL.
 *
 * NVMe 드라이버가 네임스페이스당 gendisk를 하나씩 생성할 때 호출된다.
 * gendisk 구조체, bio_split mempool, bdi, part0 block_device, part_tbl XArray,
 * blkcg, zone 자원, 난수 시드를 순서대로 초기화한다. 실패 시 역순으로 정리.
 * 실행 컨텍스트: 프로세스 컨텍스트 (GFP_KERNEL 할당 가능).
 *
 * 호출 체인:
 *   __blk_alloc_disk() → [__alloc_disk_node()] (gendisk+queue 패키지)
 *   nvme 드라이버 직접  → [__alloc_disk_node()] (외부 queue 사용)
 */
struct gendisk *__alloc_disk_node(struct request_queue *q, int node_id,
		struct lock_class_key *lkclass)
{
	struct gendisk *disk; /* [한국어] 할당할 gendisk 포인터 */

	disk = kzalloc_node(sizeof(struct gendisk), GFP_KERNEL, node_id); /* [한국어] NUMA 노드에 gendisk 0-초기화 할당 */
	if (!disk) /* [한국어] 메모리 부족 시 NULL 반환 */
		return NULL;

	if (bioset_init(&disk->bio_split, BIO_POOL_SIZE, 0, 0)) /* [한국어] bio 분할 mempool 초기화 (big bio 분할에 사용) */
		goto out_free_disk;

	disk->bdi = bdi_alloc(node_id); /* [한국어] backing_dev_info 할당 — writeback 관리용 */
	if (!disk->bdi) /* [한국어] bdi 할당 실패 */
		goto out_free_bioset;

	/* bdev_alloc() might need the queue, set before the first call */
	disk->queue = q; /* [한국어] bdev_alloc 호출 전 queue를 먼저 설정해야 함 */

	disk->part0 = bdev_alloc(disk, 0); /* [한국어] 파티션 번호 0(전체 디스크)의 block_device 할당 */
	if (!disk->part0) /* [한국어] part0 할당 실패 */
		goto out_free_bdi;

	disk->node_id = node_id;           /* [한국어] NUMA 노드 번호 저장 */
	mutex_init(&disk->open_mutex);     /* [한국어] 동시 open/close/scan 직렬화 뮤텍스 초기화 */
	xa_init(&disk->part_tbl);          /* [한국어] 파티션 테이블 XArray 초기화 */
	if (xa_insert(&disk->part_tbl, 0, disk->part0, GFP_KERNEL)) /* [한국어] part0을 인덱스 0에 삽입 */
		goto out_destroy_part_tbl;

	if (blkcg_init_disk(disk)) /* [한국어] 블록 cgroup 초기화 — throttle/iolatency 등에 필요 */
		goto out_erase_part0;

	disk_init_zone_resources(disk); /* [한국어] zoned block device 자원 초기화 */
	rand_initialize_disk(disk);     /* [한국어] 엔트로피 시드 초기화 */
	disk_to_dev(disk)->class = &block_class; /* [한국어] 장치를 block_class에 속하게 설정 */
	disk_to_dev(disk)->type = &disk_type;    /* [한국어] 장치 타입을 disk_type으로 설정 */
	device_initialize(disk_to_dev(disk));    /* [한국어] kobject, 락, uevent 큐 등 device 내부 초기화 */
	inc_diskseq(disk); /* [한국어] 단조 증가 일련번호(diskseq) 할당 */
	q->disk = disk; /* [한국어] request_queue가 이 gendisk를 역참조할 수 있도록 연결 */
	lockdep_init_map(&disk->lockdep_map, "(bio completion)", lkclass, 0); /* [한국어] bio 완료 락 lockdep 추적 맵 초기화 */
#ifdef CONFIG_BLOCK_HOLDER_DEPRECATED
	INIT_LIST_HEAD(&disk->slave_bdevs); /* [한국어] (deprecated) slave block device 연결 리스트 초기화 */
#endif
	mutex_init(&disk->rqos_state_mutex); /* [한국어] request QoS 상태 보호 뮤텍스 초기화 */
	kobject_init(&disk->queue_kobj, &blk_queue_ktype); /* [한국어] queue kobject 초기화 — /sys/block/XY/queue 생성용 */
	return disk; /* [한국어] 초기화 완료된 gendisk 반환 */

out_erase_part0:
	xa_erase(&disk->part_tbl, 0);  /* [한국어] part_tbl에서 part0 제거 */
out_destroy_part_tbl:
	xa_destroy(&disk->part_tbl);   /* [한국어] XArray 자원 해제 */
	disk->part0->bd_disk = NULL;   /* [한국어] part0의 gendisk 역참조 해제 */
	bdev_drop(disk->part0);        /* [한국어] part0 block_device 해제 */
out_free_bdi:
	bdi_put(disk->bdi);            /* [한국어] bdi 참조 해제 */
out_free_bioset:
	bioset_exit(&disk->bio_split); /* [한국어] bio_split mempool 해제 */
out_free_disk:
	kfree(disk);                   /* [한국어] gendisk 구조체 해제 */
	return NULL; /* [한국어] 초기화 실패 시 NULL 반환 */
}

/*
 * [한국어]
 * __blk_alloc_disk - queue limits를 포함하여 request_queue + gendisk를 함께 할당
 *
 * @lim:     queue limits 포인터 (NVMe: 물리 섹터 크기·max_segments 등).
 *           NULL이면 기본값(512B 섹터, 제한 없음) 사용.
 * @node:    NUMA 노드 번호.
 * @lkclass: lockdep 클래스 키 (bio completion 락 추적용).
 * @return:  성공 시 초기화된 gendisk 포인터(GD_OWNS_QUEUE 설정됨),
 *           실패 시 ERR_PTR(-ENOMEM).
 *
 * NVMe 드라이버처럼 blk-mq request_queue를 자체 소유하는 드라이버용 고수준 래퍼.
 * blk_alloc_queue()로 queue를 먼저 생성한 뒤, __alloc_disk_node()에 queue를 넘겨
 * gendisk를 초기화한다. GD_OWNS_QUEUE 비트를 설정하여 del_gendisk/put_disk 경로에서
 * queue를 함께 해제할 책임이 gendisk에 있음을 표시한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (GFP_KERNEL 할당 가능).
 *
 * 호출 체인:
 *   nvme_alloc_ns() → [__blk_alloc_disk()] → blk_alloc_queue() + __alloc_disk_node()
 */
struct gendisk *__blk_alloc_disk(struct queue_limits *lim, int node,
		struct lock_class_key *lkclass)
{
	struct queue_limits default_lim = { }; /* [한국어] lim=NULL 시 사용할 기본값 구조체 */
	struct request_queue *q;               /* [한국어] 새로 할당할 request_queue 포인터 */
	struct gendisk *disk;                  /* [한국어] 새로 할당할 gendisk 포인터 */

	q = blk_alloc_queue(lim ? lim : &default_lim, node); /* [한국어] queue limits 적용하여 blk-mq queue 할당 */
	if (IS_ERR(q)) /* [한국어] queue 할당 실패 시 에러 포인터 전파 */
		return ERR_CAST(q);

	disk = __alloc_disk_node(q, node, lkclass); /* [한국어] 위에서 만든 queue로 gendisk 초기화 */
	if (!disk) { /* [한국어] gendisk 할당 실패 */
		blk_put_queue(q);                    /* [한국어] queue 참조 해제 (leak 방지) */
		return ERR_PTR(-ENOMEM);             /* [한국어] -ENOMEM 에러 포인터 반환 */
	}
	set_bit(GD_OWNS_QUEUE, &disk->state); /* [한국어] gendisk가 queue 생명주기를 소유함을 표시 */
	return disk; /* [한국어] 초기화 완료된 gendisk 반환 */
}
EXPORT_SYMBOL(__blk_alloc_disk);

/*
 * [한국어]
 * put_disk - gendisk의 참조 카운트 감소; 마지막 참조 해제 시 disk_release() 호출
 *
 * @disk: 참조를 해제할 gendisk 포인터 (NULL 허용 — NULL이면 무시).
 *
 * gendisk는 struct device kobject 기반 참조 계수를 사용한다.
 * put_device()를 통해 refcount를 1 감소시키며, 0이 되면 disk_release()가
 * 트리거되어 queue(GD_OWNS_QUEUE 설정 시), bdi, part_tbl, blkcg 등이 해제된다.
 * NVMe 네임스페이스 제거 경로(nvme_ns_remove)에서 del_gendisk() 이후 반드시 호출.
 * 프로브 실패 시 add_disk() 전에 호출하면 tag_set이 유효한 시점에 queue 정리 가능.
 * 실행 컨텍스트: 임의 컨텍스트 허용, 단 마지막 참조 해제는 원자적 컨텍스트 불가.
 *
 * 호출 체인:
 *   nvme_ns_remove() → del_gendisk() → [put_disk()] → put_device() → disk_release()
 */
/**
 * put_disk - decrements the gendisk refcount
 * @disk: the struct gendisk to decrement the refcount for
 *
 * This decrements the refcount for the struct gendisk. When this reaches 0
 * we'll have disk_release() called.
 *
 * Note: for blk-mq disk put_disk must be called before freeing the tag_set
 * when handling probe errors (that is before add_disk() is called).
 *
 * Context: Any context, but the last reference must not be dropped from
 *          atomic context.
 */
void put_disk(struct gendisk *disk)
{
	if (disk) /* [한국어] NULL 가드 — NULL이면 호출 무시 */
		put_device(disk_to_dev(disk)); /* [한국어] kobject refcount 감소; 0이면 disk_release() 트리거 */
}
EXPORT_SYMBOL(put_disk);

/*
 * [한국어]
 * set_disk_ro_uevent - 읽기 전용 상태 변경을 udev에 uevent로 통지
 *
 * @gd: 대상 gendisk.
 * @ro: 1이면 읽기 전용(DISK_RO=1), 0이면 읽기/쓰기(DISK_RO=0).
 *
 * udev 규칙이 DISK_RO 환경변수를 보고 장치 퍼미션을 재조정할 수 있도록
 * KOBJ_CHANGE uevent를 발생시킨다. set_disk_ro()에서만 호출된다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   set_disk_ro() → [set_disk_ro_uevent()] → kobject_uevent_env()
 */
static void set_disk_ro_uevent(struct gendisk *gd, int ro)
{
	char event[] = "DISK_RO=1";    /* [한국어] 기본값은 읽기 전용(DISK_RO=1) */
	char *envp[] = { event, NULL }; /* [한국어] uevent 환경변수 배열 (NULL 종료) */

	if (!ro) /* [한국어] ro=0이면 읽기/쓰기 상태 — 마지막 문자를 '0'으로 패치 */
		event[8] = '0'; /* [한국어] "DISK_RO=1" → "DISK_RO=0" (인덱스 8이 '1'/'0') */
	kobject_uevent_env(&disk_to_dev(gd)->kobj, KOBJ_CHANGE, envp); /* [한국어] KOBJ_CHANGE + DISK_RO 환경변수로 udev에 통지 */
}

/*
 * [한국어]
 * set_disk_ro - gendisk의 읽기 전용(GD_READ_ONLY) 상태를 원자적으로 설정/해제
 *
 * @disk:      대상 gendisk.
 * @read_only: true이면 GD_READ_ONLY 비트 설정(쓰기 불가),
 *             false이면 GD_READ_ONLY 비트 해제(읽기/쓰기 가능).
 *
 * NVMe 네임스페이스가 write-protected 상태이거나, 물리적으로 쓰기 불가능한
 * 미디어일 때 드라이버가 호출한다. 상태가 실제로 변경된 경우에만 uevent를
 * 발생시키며, 이미 같은 상태이면 조기 반환한다.
 * test_and_set_bit / test_and_clear_bit 은 원자 비트 연산이므로 별도 락 불필요.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   nvme_update_ns_info() → [set_disk_ro()] → set_disk_ro_uevent() → kobject_uevent_env()
 */
/**
 * set_disk_ro - set a gendisk read-only
 * @disk:	gendisk to operate on
 * @read_only:	%true to set the disk read-only, %false set the disk read/write
 *
 * This function is used to indicate whether a given disk device should have its
 * read-only flag set. set_disk_ro() is typically used by device drivers to
 * indicate whether the underlying physical device is write-protected.
 */
void set_disk_ro(struct gendisk *disk, bool read_only)
{
	if (read_only) { /* [한국어] 읽기 전용으로 설정 요청 */
		if (test_and_set_bit(GD_READ_ONLY, &disk->state)) /* [한국어] 이미 읽기 전용이면 uevent 없이 조기 반환 */
			return;
	} else { /* [한국어] 읽기/쓰기로 해제 요청 */
		if (!test_and_clear_bit(GD_READ_ONLY, &disk->state)) /* [한국어] 이미 읽기/쓰기이면 uevent 없이 조기 반환 */
			return;
	}
	set_disk_ro_uevent(disk, read_only); /* [한국어] 상태가 변경된 경우에만 DISK_RO uevent 발생 */
}
EXPORT_SYMBOL(set_disk_ro);

/*
 * [한국어]
 * inc_diskseq - 전역 diskseq 카운터를 증가시켜 이 gendisk에 새 일련번호 할당
 *
 * @disk: 일련번호를 할당할 gendisk.
 *
 * 블록 장치 인스턴스마다 고유한 단조 증가 64비트 번호(diskseq)를 부여한다.
 * 동일한 dev_t(major:minor)가 재사용되더라도 diskseq가 다르면 다른 장치 인스턴스임을
 * 구별할 수 있어 udev의 uevent 매칭과 장치 교체 감지에 사용된다.
 * atomic64_inc_return은 원자 연산이므로 여러 드라이버가 동시에 gendisk를 할당해도
 * 번호 중복 없이 유일성이 보장된다.
 * 실행 컨텍스트: __alloc_disk_node() 내 프로세스 컨텍스트에서 호출.
 *
 * 호출 체인:
 *   __alloc_disk_node() → [inc_diskseq()] → atomic64_inc_return(&diskseq)
 */
void inc_diskseq(struct gendisk *disk)
{
	disk->diskseq = atomic64_inc_return(&diskseq); /* [한국어] 전역 diskseq 원자 증가 후 이 gendisk에 부여 */
}

/*
 * ============================================================================
 * NVMe 관점 핵심 요약
 * ----------------------------------------------------------------------------
 * - 이 파일은 gendisk 생명주기를 관리하며, NVMe 네임스페이스를 블록 서브시스템에
 *   노출/제거하는 관문이다. 실제 I/O 처리는 request_queue -> blk-mq -> NVMe
 *   드라이버가 담당한다.
 *
 * - add_disk_fwnode/device_add_disk -> __add_disk -> blk_register_queue 경로를
 *   통해 NVMe 디스크가 /dev, /sys/block, /proc/diskstats에 등록되고, 이후
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd
 *   (doorbell)로 I/O가 전달된다.
 *
 * - del_gendisk/__del_gendisk은 NVMe 컨트롤러/네임스페이스 제거 시 queue를
 *   freeze/drain하고 모든 파티션과 sysfs 링크를 정리한다.
 *
 * - blk_mark_disk_dead은 갑작스러운 NVMe 제거(hot-unplug) 시 새 I/O 진입을
 *   차단하며, 하위 NVMe 레이어에서 진행 중인 CID의 완료/취소를 유도한다.
 *
 * - 이 파일은 block/bdev.c(block_device 관리), block/blk-mq.c(다중 큐 I/O),
 *   drivers/nvme/host/core.c(네임스페이스 생명주기)와 글로벌하게 연결된다.
 * ============================================================================
 */
