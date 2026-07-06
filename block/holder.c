// SPDX-License-Identifier: GPL-2.0-only
/*
 * [한국어 설명] 블록 장치(holder)-슬레이브(slave) 간 sysfs 토폴로지 링크 관리 (block/holder.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 상위 블록 장치인 holder(예: device-mapper의 dm-0, md RAID 배열,
 * NVMe multipath의 상위 gendisk)와 그 아래에서 실제 I/O를 받아내는 slave
 * 블록 장치(bdev, 예: nvme0n1, sda1) 사이의 종속 관계를 sysfs 심볼릭 링크로
 * 노출하는 역할을 한다. 실제로 I/O 요청을 처리하거나 데이터를 옮기지는
 * 않으며, 순수하게 "누가 누구를 소유(holder)하고 있는가"라는 토폴로지
 * 정보를 사용자 공간(예: lsblk, udev, multipath-tools)이 조회할 수 있도록
 * /sys/block/<holder>/slaves/<slave> 및 /sys/block/<slave>/holders/<holder>
 * 링크를 생성/삭제한다. bd_holder_disk 구조체 하나가 holder-slave 쌍 하나를
 * 표현하며, 동일 쌍에 대한 중복 요청은 refcnt로 병합하여 링크가 중복
 * 생성되지 않도록 만든다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 블록 계층에서 gendisk/block_device가 등록·claim된 "이후" 단계에서 호출되는
 * 부가적인 sysfs 표현 계층이다. 실행 흐름상으로는:
 *   (1) 상위 드라이버(device-mapper, md, NVMe multipath 등)가 하위 bdev를
 *       blkdev_get_by_*() 등으로 open/claim하여 bdev->bd_holder를 설정한다.
 *   (2) 그 직후 상위 드라이버가 bd_link_disk_holder(bdev, disk)를 호출하여
 *       이 파일의 로직이 동작, sysfs 링크를 생성한다.
 *   (3) 상위 드라이버가 테이블을 재구성하거나 장치를 제거할 때
 *       bd_unlink_disk_holder(bdev, disk)를 호출해 링크를 해제한다.
 * 이 파일 자체는 nvme_queue_rq/blk_mq_submit_bio 같은 실제 I/O 제출 경로와는
 * 무관하며, 커널 스레드나 사용자 프로세스 컨텍스트(ioctl/open 처리 도중)에서
 * 동기적으로 호출된다. sleep 가능한 컨텍스트(mutex_lock, sysfs_create_link
 * 내부의 kernfs 락 등)에서만 호출되어야 한다(원본 커널독 주석의 "Might
 * sleep" 명시와 일치).
 *
 * === 타 모듈과의 연결 ===
 * - block/genhd.c: gendisk 등록/해제(del_gendisk) 시 bd_holder_dir의 최초
 *   kobject 참조를 관리하며, 이 파일은 del_gendisk 이후에도 살아있어야 하는
 *   자신만의 참조를 kobject_get()으로 별도로 유지한다.
 * - block/bdev.c (blkdev_get_by_dev 등): bdev->bd_holder를 설정하는 claim
 *   로직이 이 파일보다 먼저 실행되어야 하며, WARN_ON_ONCE(!bdev->bd_holder)로
 *   그 전제 조건을 검증한다.
 * - drivers/md/dm.c, drivers/md/md.c, NVMe multipath(drivers/nvme/host/multipath.c):
 *   실제 이 파일의 두 API(bd_link_disk_holder/bd_unlink_disk_holder)를 호출하는
 *   대표적인 상위 소비자. dm은 테이블 로드 시 slave를 추가, md는 rdev를
 *   배열에 bind할 때 호출한다.
 * - fs/sysfs, fs/kernfs: add_symlink/del_symlink이 내부적으로 사용하는
 *   sysfs_create_link/sysfs_remove_link가 실제 커널 파일시스템 오브젝트를
 *   생성/삭제하는 하위 계층이다.
 * - 데이터 흐름: gendisk(disk)->slave_dir, disk->slave_bdevs 리스트, 그리고
 *   block_device(bdev)->bd_holder_dir가 이 파일이 공유하는 핵심 상태이며,
 *   이들은 각각 disk/bdev 구조체에 내장되어 있고 blk_holder_mutex 하나로
 *   직렬화된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct bd_holder_disk: disk->slave_bdevs 리스트에 매달리는 노드. 하나의
 *   holder-slave 관계와 그 참조 카운트를 표현.
 * - bd_find_holder_disk(): 이미 등록된 holder-slave 관계를 찾는 내부 헬퍼.
 * - add_symlink()/del_symlink(): sysfs_create_link/remove_link의 얇은 래퍼.
 * - bd_link_disk_holder(): holder-slave 관계를 새로 만들거나(신규 시 링크
 *   2개 생성) 이미 있으면 refcnt만 증가시키는 공개 API.
 * - bd_unlink_disk_holder(): refcnt를 감소시키고 0이 되면 링크를 제거하고
 *   구조체를 해제하는 공개 API.
 */
#include <linux/blkdev.h>	/* [한국어] struct block_device, struct gendisk, bdev_kobj(), disk_live() 등 블록 계층 핵심 타입/헬퍼 선언 - holder/slave 구조체 필드 접근에 필요 */
#include <linux/slab.h>	/* [한국어] kzalloc_obj()/kfree() 등 슬랩 할당자 인터페이스 - bd_holder_disk 구조체의 동적 할당/해제에 사용 */

/* [한국어] 하나의 holder(상위 디스크) - slave(하위 bdev) 관계를 표현하는 노드.
 * gendisk->slave_bdevs 연결 리스트의 엔트리로 존재하며, bd_link_disk_holder()가
 * 새로 만들고 bd_unlink_disk_holder()가 참조 카운트 소진 시 해제한다. */
struct bd_holder_disk {
	struct list_head	list;
	/* [한국어] 이 관계 노드를 holder gendisk의 disk->slave_bdevs 리스트에 연결하는 링크.
	 * 설정자: bd_link_disk_holder()가 새 holder를 만들 때 INIT_LIST_HEAD() 후
	 *         list_add()로 disk->slave_bdevs에 삽입.
	 * 읽는 자: bd_find_holder_disk()가 list_for_each_entry()로 순회하며 검색;
	 *          bd_unlink_disk_holder()가 list_del_init()으로 제거.
	 * 값 범위: 유효한 연결 리스트 노드. 리스트에서 빠진 뒤에는 kfree()로 해제되어
	 *          더 이상 유효하지 않다.
	 * 동기화: 이 필드에 대한 모든 접근은 blk_holder_mutex를 잡은 상태에서만
	 *          이루어져야 하며, 실제로 모든 호출자가 그렇게 하고 있다. */

	struct kobject		*holder_dir;
	/* [한국어] 이 관계가 가리키는 slave bdev의 sysfs "holders/" 디렉터리 kobject.
	 * 즉 /sys/block/<slave>/holders/ 에 대응하는 kobject 포인터를 저장해 두어,
	 * 나중에 링크 해제 시 원래 bdev 구조체를 몰라도 이 kobject만으로 심볼릭
	 * 링크를 제거할 수 있게 한다.
	 * 설정자: bd_link_disk_holder()에서 holder->holder_dir = bdev->bd_holder_dir로
	 *         최초 설정.
	 * 읽는 자: bd_find_holder_disk()가 bdev->bd_holder_dir와 동일한지 비교해
	 *          기존 관계를 식별; bd_unlink_disk_holder()가 del_symlink()/
	 *          kobject_put()에 사용.
	 * 값 범위: 유효한 kobject 포인터(NULL 아님). bd_link_disk_holder()가
	 *          kobject_get()으로 참조 카운트를 올려두므로 이 구조체가 살아있는
	 *          동안 최소 1개의 참조가 보장된다.
	 * 동기화: blk_holder_mutex로 보호되는 disk->slave_bdevs 리스트 내에서만
	 *          비교/접근되므로 별도의 락은 필요 없다. */

	int			refcnt;
	/* [한국어] 동일한 holder-slave 쌍에 대해 중복으로 요청된 연결 횟수를 세는
	 * 참조 카운트. 예를 들어 device-mapper 테이블에 동일 slave가 여러 타겟에서
	 * 참조되면 링크를 매번 새로 만드는 대신 이 카운트만 올린다.
	 * 설정자: bd_link_disk_holder()가 신규 생성 시 1로 초기화하고, 기존 관계를
	 *         찾으면 refcnt++로 증가.
	 * 읽는 자/감소자: bd_unlink_disk_holder()가 --holder->refcnt로 감소시키고,
	 *          0이 되는 순간에만 실제 sysfs 링크 제거와 kfree()를 수행.
	 * 값 범위: 1 이상의 정수. 0이 되면 즉시 구조체 자체가 해제되므로, 살아있는
	 *          객체에서는 항상 refcnt >= 1이다.
	 * 동기화: blk_holder_mutex 하에서만 증감되므로 별도의 원자적 연산 없이도
	 *          경쟁 조건 없이 안전하다. */
};

static DEFINE_MUTEX(blk_holder_mutex);
/* [한국어] 이 파일 전역에서 disk->slave_bdevs 리스트와 bd_holder_disk 구조체의
 * 생성/조회/참조카운트 증감/해제를 직렬화하는 전용 뮤텍스.
 * bdev->bd_disk->open_mutex(디스크 open/close 직렬화용)와는 별개의 락으로,
 * holder-slave 토폴로지 자료구조만을 보호하는 좁은 범위의 락이다.
 * bd_link_disk_holder()/bd_unlink_disk_holder()/bd_find_holder_disk() 모두
 * 이 뮤텍스를 잡은 상태에서만 disk->slave_bdevs를 건드린다. */

/*
 * [한국어]
 * bd_find_holder_disk - 주어진 (bdev, disk) 쌍에 대응하는 기존 holder 관계를 검색
 *
 * @bdev: 하위(slave) 블록 장치. 이 bdev의 bd_holder_dir 값과 동일한
 *        holder_dir을 가진 리스트 노드를 찾는 데 사용된다.
 * @disk: 상위(holder) gendisk. disk->slave_bdevs 리스트를 순회 대상으로 삼는다.
 * @return: 이미 등록된 struct bd_holder_disk * (같은 slave에 대응하는 노드),
 *          없으면 NULL. 호출자는 NULL이면 "새로 만들어야 함"으로 해석한다.
 *
 * 이 함수가 왜 필요한가: 동일한 (holder, slave) 쌍에 대해 sysfs 링크를 두 번
 * 만들면 sysfs_create_link()가 -EEXIST로 실패하므로, 링크 생성 전에 반드시
 * 기존 관계가 있는지 확인해서 있으면 refcnt만 올리고 없으면 새로 만들어야 한다.
 * 동작 과정: disk->slave_bdevs 리스트를 순서대로 순회하면서, 각 노드의
 * holder_dir 포인터가 인자로 받은 bdev->bd_holder_dir와 동일한 kobject인지
 * 비교한다. bd_holder_dir는 bdev 하나당 유일하게 존재하는 kobject이므로 포인터
 * 비교만으로 동일 slave 여부를 판별할 수 있다.
 * 실행 컨텍스트: 호출자(bd_link_disk_holder, bd_unlink_disk_holder)가 이미
 * blk_holder_mutex를 획득한 상태에서 호출하므로, 이 함수 자체는 락을 잡지
 * 않고 리스트를 안전하게 순회한다 (재진입/락 없음이 전제 조건).
 * 호출자(caller context): bd_link_disk_holder(), bd_unlink_disk_holder().
 * 피호출자(callee): list_for_each_entry() 매크로만 사용 (별도 함수 호출 없음).
 * 에러 처리: 별도 에러 코드 없이 NULL 반환으로 "없음"을 표현한다.
 *
 * 호출 체인:
 *   bd_link_disk_holder()/bd_unlink_disk_holder() → [bd_find_holder_disk] → list_for_each_entry()
 */
static struct bd_holder_disk *bd_find_holder_disk(struct block_device *bdev,
						  struct gendisk *disk)
{
	struct bd_holder_disk *holder;	/* [한국어] 순회 중 발견한 기존 관계 노드를 담을 지역 포인터; 못 찾으면 NULL로 반환 */

	list_for_each_entry(holder, &disk->slave_bdevs, list)	/* [한국어] disk->slave_bdevs에 매달린 모든 holder 관계 노드를 순서대로 순회 */
		if (holder->holder_dir == bdev->bd_holder_dir)	/* [한국어] 이 노드가 가리키는 slave의 holders 디렉터리 kobject가 인자 bdev의 것과 같은지 비교 - 동일하면 같은 slave에 대한 기존 관계 */
			return holder;	/* [한국어] 일치하는 관계를 찾았으므로 즉시 반환 - 순회 중단 */
	return NULL;	/* [한국어] 리스트 전체를 순회했지만 일치하는 관계가 없음 - 신규 등록이 필요함을 호출자에게 알림 */
}

/*
 * [한국어]
 * add_symlink - 두 kobject 사이에 sysfs 심볼릭 링크를 생성하는 얇은 래퍼
 *
 * @from: 링크가 위치할 디렉터리에 대응하는 kobject (예: holder의 slave_dir
 *        또는 slave bdev의 bd_holder_dir).
 * @to: 링크가 가리킬 대상 kobject (예: slave bdev의 kobject 또는 holder
 *      disk의 device kobject). 링크의 이름은 이 kobject의 이름을 그대로 사용한다.
 * @return: sysfs_create_link()의 반환값 그대로 - 0이면 성공, 음수 errno이면
 *          실패(예: 이미 같은 이름의 링크가 있으면 -EEXIST).
 *
 * 왜 필요한가: bd_link_disk_holder()에서 "slaves/" 방향과 "holders/" 방향,
 * 두 종류의 링크를 동일한 방식으로 만들어야 하므로 중복 코드를 줄이기 위한
 * 헬퍼. 단순 래퍼처럼 보이지만 두 호출 지점의 인자 순서/의미를 명확히 하는
 * 역할을 한다.
 * 동작: kobject_name(to)로 대상 kobject의 이름을 얻어 그 이름 그대로
 * from 디렉터리 아래에 to를 가리키는 심볼릭 링크를 생성한다.
 * 실행 컨텍스트: sysfs_create_link()는 kernfs 내부 락을 잡을 수 있어 sleep
 * 가능한 컨텍스트에서만 호출 가능 (Might sleep).
 * 호출자: bd_link_disk_holder().
 * 피호출자: sysfs_create_link() (fs/sysfs/symlink.c).
 * 에러 처리: 반환값을 그대로 호출자에 전달하며, 호출자가 실패 시 앞서 만든
 * 링크를 되돌리는 goto 체인을 수행한다.
 *
 * 호출 체인:
 *   bd_link_disk_holder() → [add_symlink] → sysfs_create_link()
 */
static int add_symlink(struct kobject *from, struct kobject *to)
{
	return sysfs_create_link(from, to, kobject_name(to));	/* [한국어] from 디렉터리 아래에 to를 가리키며 이름은 to 자신의 kobject 이름을 쓰는 심볼릭 링크 생성 */
}

/*
 * [한국어]
 * del_symlink - add_symlink()가 생성한 sysfs 심볼릭 링크를 제거하는 얇은 래퍼
 *
 * @from: 링크가 위치한 디렉터리에 대응하는 kobject.
 * @to: 링크가 가리키던 대상 kobject. 이름만 사용되고 실제 링크 삭제 시
 *      대상 자체에 접근하지는 않는다.
 * @return: 없음 (void). sysfs_remove_link()는 실패해도 별도 값을 반환하지
 *          않는 fire-and-forget 형태.
 *
 * 왜 필요한가: add_symlink()와 대칭을 이루는 헬퍼로, bd_unlink_disk_holder()가
 * 참조 카운트가 0이 되었을 때 두 방향의 링크를 동일한 방식으로 제거하도록 한다.
 * 동작: kobject_name(to)로 삭제할 링크의 이름을 얻어 from 디렉터리에서 그
 * 이름의 엔트리를 제거한다.
 * 실행 컨텍스트: sysfs_remove_link() 역시 kernfs 락으로 인해 sleep 가능한
 * 컨텍스트에서 호출해야 한다.
 * 호출자: bd_unlink_disk_holder().
 * 피호출자: sysfs_remove_link() (fs/sysfs/symlink.c).
 * 에러 처리: 반환값이 없으므로 실패를 감지할 수 없다 - 상위에서 이미 존재가
 * 확인된 링크만 제거 대상으로 삼는 것을 전제로 한다.
 *
 * 호출 체인:
 *   bd_unlink_disk_holder() → [del_symlink] → sysfs_remove_link()
 */
static void del_symlink(struct kobject *from, struct kobject *to)
{
	sysfs_remove_link(from, kobject_name(to));	/* [한국어] from 디렉터리에서 to의 이름과 같은 심볼릭 링크 엔트리를 제거 */
}

/**
 * bd_link_disk_holder - create symlinks between holding disk and slave bdev
 * @bdev: the claimed slave bdev
 * @disk: the holding disk
 *
 * DON'T USE THIS UNLESS YOU'RE ALREADY USING IT.
 *
 * This functions creates the following sysfs symlinks.
 *
 * - from "slaves" directory of the holder @disk to the claimed @bdev
 * - from "holders" directory of the @bdev to the holder @disk
 *
 * For example, if /dev/dm-0 maps to /dev/sda and disk for dm-0 is
 * passed to bd_link_disk_holder(), then:
 *
 *   /sys/block/dm-0/slaves/sda --> /sys/block/sda
 *   /sys/block/sda/holders/dm-0 --> /sys/block/dm-0
 *
 * The caller must have claimed @bdev before calling this function and
 * ensure that both @bdev and @disk are valid during the creation and
 * lifetime of these symlinks.
 *
 * CONTEXT:
 * Might sleep.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
/*
 * [한국어]
 * bd_link_disk_holder - holder gendisk와 slave bdev 사이에 sysfs 심볼릭 링크 생성
 *
 * @bdev: 이미 claim된(즉 bdev->bd_holder가 설정된) slave 블록 장치. 호출자가
 *        blkdev_get_by_*() 등으로 미리 열어 둔 상태여야 한다.
 * @disk: slave를 소유하는 holder gendisk (예: dm-0, md0). disk->slave_dir가
 *        유효해야 하며(NULL이면 -EINVAL), disk->slave_bdevs 리스트에 새 관계가
 *        추가된다.
 * @return: 성공 시 0. 실패 시 음수 errno -
 *          -EINVAL(disk->slave_dir 없음 또는 bdev와 disk가 동일 디스크),
 *          -ENODEV(bdev->bd_disk가 이미 제거 중이라 disk_live()가 거짓),
 *          -ENOMEM(구조체 할당 실패), 또는 add_symlink()가 반환하는 값
 *          (예: 이미 같은 이름의 링크가 존재하면 -EEXIST).
 *
 * 왜 필요한가: device-mapper, md RAID, NVMe multipath 같은 상위 계층이 여러
 * 개의 하위 블록 장치를 하나의 논리 장치로 묶을 때, 사용자 공간 도구가
 * "이 디스크가 무엇으로 구성되어 있는지"를 /sys 만 보고 알 수 있어야 하므로
 * 이 함수가 그 토폴로지 링크를 생성한다.
 * 동작 과정:
 *   1) disk->slave_dir와 bdev/disk 동일 여부를 검사해 잘못된 호출을 조기에 거른다.
 *   2) bdev->bd_disk->open_mutex를 잡고 disk_live()로 slave가 아직 살아있는지
 *      확인한 뒤, bd_holder_dir에 대한 참조를 하나 더 얻어 둔다(del_gendisk가
 *      이후 자신의 참조를 반납해도 이 함수가 만든 링크가 안전하게 남도록).
 *   3) blk_holder_mutex를 잡고 bd_find_holder_disk()로 기존 관계가 있는지 확인 -
 *      있으면 방금 얻은 참조를 되돌리고 refcnt만 증가시킨 뒤 끝낸다.
 *   4) 없으면 새 bd_holder_disk를 할당하고 두 방향의 심볼릭 링크
 *      (slaves/<slave>, holders/<holder>)를 순서대로 만든 뒤 disk->slave_bdevs에
 *      등록한다. 중간 단계 실패 시 goto 체인으로 이미 만든 자원을 역순으로 정리한다.
 * 실행 컨텍스트: 사용자 컨텍스트에서 동기적으로 호출되며 mutex_lock()과
 * sysfs_create_link() 양쪽 모두 sleep 가능하므로 인터럽트/원자적 컨텍스트에서는
 * 호출할 수 없다(원본 커널독 "Might sleep" 명시). 동시에 여러 스레드가
 * 호출해도 open_mutex와 blk_holder_mutex가 각 단계를 직렬화하므로 안전하다.
 * 호출자(caller context): device-mapper(dm_get_device 경로), md(bind_rdev_to_array
 * 경로) 등 하위 장치를 claim한 직후 호출.
 * 피호출자(callee): disk_live(), kobject_get()/kobject_put(), bd_find_holder_disk(),
 * kzalloc_obj(), add_symlink(), del_symlink(), list_add(), kfree().
 * 에러 처리: 각 실패 지점마다 이미 획득한 자원을 정확히 역순으로 되돌리는
 * goto 체인(out_del_symlink → out_free_holder → out_unlock)을 사용하며, 최종
 * 실패 시에는 앞서 kobject_get()으로 올려둔 bd_holder_dir 참조도 kobject_put()
 * 으로 되돌린다.
 *
 * 호출 체인:
 *   dm_get_device()/bind_rdev_to_array() 등 상위 claim 로직 → [bd_link_disk_holder]
 *   → disk_live() / bd_find_holder_disk() / add_symlink() → sysfs_create_link()
 */
int bd_link_disk_holder(struct block_device *bdev, struct gendisk *disk)
{
	struct bd_holder_disk *holder;	/* [한국어] 기존 관계를 찾거나 새로 할당할 holder 노드를 담을 지역 포인터 */
	int ret = 0;	/* [한국어] 함수 반환값 누적 변수; 기본값 0(성공)으로 초기화 후 실패 지점에서 갱신 */

	if (WARN_ON_ONCE(!disk->slave_dir))	/* [한국어] holder gendisk가 slave_dir(슬레이브 디렉터리 kobject)를 갖고 있지 않으면 이 API를 쓸 수 없는 디스크 - 커널 경고 발생 후 방어 */
		return -EINVAL;	/* [한국어] 전제 조건 위반이므로 즉시 잘못된 인자 에러로 반환 */

	if (bdev->bd_disk == disk)	/* [한국어] 자기 자신을 자신의 slave로 등록하려는 잘못된 호출인지 검사 (holder == slave 인 순환 방지) */
		return -EINVAL;	/* [한국어] 자기 참조는 허용하지 않으므로 잘못된 인자 에러로 반환 */

	/*
	 * del_gendisk drops the initial reference to bd_holder_dir, so we
	 * need to keep our own here to allow for cleanup past that point.
	 */
	mutex_lock(&bdev->bd_disk->open_mutex);	/* [한국어] bdev의 open/close 상태(disk_live)와 bd_holder_dir 참조 조작을 이 디스크의 open_mutex로 직렬화 - del_gendisk와의 경쟁 방지 */
	if (!disk_live(bdev->bd_disk)) {	/* [한국어] slave gendisk가 이미 GENHD_FL_UP을 잃고 제거 절차(del_gendisk)를 타고 있는 중인지 확인 */
		mutex_unlock(&bdev->bd_disk->open_mutex);	/* [한국어] 에러로 빠지기 전 반드시 먼저 획득한 open_mutex를 해제 */
		return -ENODEV;	/* [한국어] 이미 제거 중인 장치는 slave로 새로 연결할 수 없으므로 디바이스 없음 에러 반환 */
	}
	kobject_get(bdev->bd_holder_dir);	/* [한국어] del_gendisk가 나중에 자신의 초기 참조를 반납하더라도 이 함수가 만들 링크가 유효하게 남도록 별도의 참조를 미리 확보 (위 원본 주석에서 설명하는 의도) */
	mutex_unlock(&bdev->bd_disk->open_mutex);	/* [한국어] disk_live 확인과 참조 획득이 끝났으므로 open_mutex 해제 - 이 뒤로는 blk_holder_mutex로 보호 범위 전환 */

	mutex_lock(&blk_holder_mutex);	/* [한국어] 이후 disk->slave_bdevs 리스트 조회/수정을 보호하는 전용 락 획득 */
	WARN_ON_ONCE(!bdev->bd_holder);	/* [한국어] 호출 전 bdev가 이미 claim되어 bd_holder가 설정돼 있어야 한다는 전제 조건을 검증 (실패해도 계속 진행하는 경고성 체크) */

	holder = bd_find_holder_disk(bdev, disk);	/* [한국어] 동일 (bdev, disk) 쌍에 대한 관계가 이미 등록되어 있는지 검색 */
	if (holder) {	/* [한국어] 이미 등록된 관계를 찾은 경우 - 링크를 새로 만들 필요 없이 참조 카운트만 올리면 됨 */
		kobject_put(bdev->bd_holder_dir);	/* [한국어] 위에서 미리 얻어둔 참조가 이번엔 불필요하므로(기존 holder가 이미 참조를 갖고 있음) 되돌림 */
		holder->refcnt++;	/* [한국어] 동일 쌍에 대한 중복 요청 횟수를 하나 증가 */
		goto out_unlock;	/* [한국어] 새 링크 생성 없이 잠금 해제 및 성공(ret==0) 반환 경로로 이동 */
	}

	holder = kzalloc_obj(*holder);	/* [한국어] 새 holder-slave 관계를 표현할 bd_holder_disk 구조체를 0으로 초기화하여 할당 */
	if (!holder) {	/* [한국어] 슬랩 할당 실패 - 메모리 부족 상황 */
		ret = -ENOMEM;	/* [한국어] 반환값을 메모리 부족 에러로 설정 */
		goto out_unlock;	/* [한국어] 아직 아무 자원도 추가로 만들지 않았으므로 바로 잠금 해제 경로로 이동 (앞서 얻은 kobject 참조는 out_unlock에서 ret!=0이므로 반납됨) */
	}

	INIT_LIST_HEAD(&holder->list);	/* [한국어] 새 노드를 disk->slave_bdevs에 연결하기 전, 노드 자신의 list_head를 자기 참조 상태로 초기화 */
	holder->refcnt = 1;	/* [한국어] 새로 만드는 관계이므로 참조 카운트를 1로 시작 */
	holder->holder_dir = bdev->bd_holder_dir;	/* [한국어] 이후 bd_find_holder_disk()의 비교 기준이자 del_symlink 대상이 될 slave의 holders 디렉터리 kobject를 저장 */

	ret = add_symlink(disk->slave_dir, bdev_kobj(bdev));	/* [한국어] /sys/block/<holder>/slaves/<slave> 링크 생성 - holder 쪽에서 slave를 나열하는 방향 */
	if (ret)	/* [한국어] 첫 번째 링크 생성이 실패한 경우 (예: 이미 동일 이름의 slave 링크가 존재) */
		goto out_free_holder;	/* [한국어] 아직 두 번째 링크는 만들지 않았으므로 방금 할당한 holder 구조체만 해제하면 됨 */
	ret = add_symlink(bdev->bd_holder_dir, &disk_to_dev(disk)->kobj);	/* [한국어] /sys/block/<slave>/holders/<holder> 링크 생성 - slave 쪽에서 holder를 나열하는 반대 방향 */
	if (ret)	/* [한국어] 두 번째 링크 생성이 실패한 경우 */
		goto out_del_symlink;	/* [한국어] 방금 성공한 첫 번째 링크를 되돌려야 하므로 out_del_symlink로 이동 */
	list_add(&holder->list, &disk->slave_bdevs);	/* [한국어] 두 링크 모두 생성에 성공했으므로 새 관계 노드를 holder의 slave_bdevs 리스트에 등록 - 이후 bd_find_holder_disk()가 찾을 수 있게 됨 */

	mutex_unlock(&blk_holder_mutex);	/* [한국어] 정상 경로 성공 - 리스트 보호 락 해제 */
	return 0;	/* [한국어] 신규 링크 생성 성공을 알림 */

out_del_symlink:	/* [한국어] 두 번째 링크 생성 실패 시 진입하는 롤백 라벨 */
	del_symlink(disk->slave_dir, bdev_kobj(bdev));	/* [한국어] 앞서 성공한 slaves/ 방향 링크를 롤백하여 sysfs에 절반만 존재하는 링크가 남지 않도록 함 */
out_free_holder:	/* [한국어] 두 링크 생성 시도 전(add_symlink 첫 호출 실패) 또는 위 out_del_symlink 이후 도달하는 라벨 */
	kfree(holder);	/* [한국어] 링크 생성 실패로 더 이상 쓸 일이 없어진 holder 구조체 메모리 해제 */
out_unlock:	/* [한국어] holder 검색/할당 단계에서 실패했거나(ENOMEM) 정상적으로 refcnt만 올린 경우 공통으로 도달하는 라벨 */
	mutex_unlock(&blk_holder_mutex);	/* [한국어] 성공/실패 모든 경로가 마지막에 거치는 공통 언락 지점 */
	if (ret)	/* [한국어] 함수가 결국 실패로 끝나는 경우인지 검사 (신규 생성 실패 경로들; 기존 holder를 찾아 refcnt만 올린 성공 경로는 ret==0이라 여기서 건너뜀) */
		kobject_put(bdev->bd_holder_dir);	/* [한국어] 함수 시작 부분에서 미리 얻어둔 bd_holder_dir 참조를 실패로 인해 되돌림 - 참조 누수 방지 */
	return ret;	/* [한국어] 최종 결과 코드 반환 - 0(성공) 또는 음수 errno */
}
EXPORT_SYMBOL_GPL(bd_link_disk_holder);	/* [한국어] 이 함수를 GPL 라이선스 모듈(예: dm-mod, md, nvme-core)에서도 심볼로 호출할 수 있도록 익스포트 */

/**
 * bd_unlink_disk_holder - destroy symlinks created by bd_link_disk_holder()
 * @bdev: the calimed slave bdev
 * @disk: the holding disk
 *
 * DON'T USE THIS UNLESS YOU'RE ALREADY USING IT.
 *
 * CONTEXT:
 * Might sleep.
 */
/*
 * [한국어]
 * bd_unlink_disk_holder - bd_link_disk_holder()가 생성한 심볼릭 링크와 관계 해제
 *
 * @bdev: 관계를 해제할 slave 블록 장치 (bd_link_disk_holder() 호출 시 사용한
 *        것과 동일한 bdev여야 bd_find_holder_disk()가 올바르게 찾는다).
 * @disk: 관계를 해제할 holder gendisk.
 * @return: 없음 (void). 내부적으로 WARN_ON_ONCE로 짝이 맞지 않는 호출(관계를
 *          못 찾음)을 알릴 뿐, 호출자에게 에러를 돌려주지 않는다.
 *
 * 왜 필요한가: bd_link_disk_holder()로 만든 링크와 참조 카운트를 대칭적으로
 * 해제해야 하며, 그렇지 않으면 sysfs에 죽은 디스크를 가리키는 링크가 남거나
 * kobject 참조가 누수된다. device-mapper 테이블 재구성, md에서 rdev 제거,
 * NVMe multipath 경로 실패 시 상위 계층이 이 함수를 호출해 토폴로지를 정리한다.
 * 동작 과정:
 *   1) disk->slave_dir가 없으면(애초에 링크를 만들 수 없는 디스크) 그냥 반환.
 *   2) blk_holder_mutex를 잡고 bd_find_holder_disk()로 관계 노드를 찾는다.
 *      못 찾으면 WARN_ON_ONCE로 프로그래밍 오류를 알린다(정상적으로는 link와
 *      unlink가 항상 짝을 이뤄야 하므로 발생하면 안 되는 상황).
 *   3) 찾았다면 refcnt를 하나 감소시키고, 그 결과가 0이 될 때만(마지막 참조가
 *      해제될 때만) 실제로 두 방향의 심볼릭 링크를 제거하고 bd_holder_dir
 *      참조를 반납한 뒤 리스트에서 빼고 구조체를 해제한다. refcnt가 아직
 *      0보다 크면(다른 곳에서 여전히 참조 중) 아무 것도 지우지 않는다.
 * 실행 컨텍스트: bd_link_disk_holder()와 마찬가지로 sleep 가능한 사용자
 * 컨텍스트에서 호출되어야 하며(원본 커널독 "Might sleep" 명시), blk_holder_mutex로
 * 리스트 조작이 직렬화되어 동시 호출에도 안전하다.
 * 호출자(caller context): device-mapper의 dm_put_device 경로, md의
 * md_kick_rdev_from_array 경로 등 slave 해제/제거 로직.
 * 피호출자(callee): bd_find_holder_disk(), del_symlink(), kobject_put(),
 * list_del_init(), kfree().
 * 에러 처리: 반환값이 없는 void 함수이므로 실패를 알릴 방법이 WARN_ON_ONCE
 * 뿐이며, 이는 "짝이 맞지 않는 link/unlink 호출"이라는 프로그래밍 버그를
 * 커널 로그로 드러내는 용도다.
 *
 * 호출 체인:
 *   dm_put_device()/md_kick_rdev_from_array() 등 상위 해제 로직 →
 *   [bd_unlink_disk_holder] → bd_find_holder_disk() / del_symlink() → sysfs_remove_link()
 */
void bd_unlink_disk_holder(struct block_device *bdev, struct gendisk *disk)
{
	struct bd_holder_disk *holder;	/* [한국어] bd_find_holder_disk()가 찾아줄, 해제 대상 관계 노드를 담을 지역 포인터 */

	if (WARN_ON_ONCE(!disk->slave_dir))	/* [한국어] bd_link_disk_holder()와 대칭적으로, slave_dir가 없는 디스크는 애초에 이 관계를 만들 수 없었어야 함 - 짝이 안 맞는 호출 경고 */
		return;	/* [한국어] 전제 조건 위반이므로 아무 것도 하지 않고 반환 */

	mutex_lock(&blk_holder_mutex);	/* [한국어] disk->slave_bdevs 리스트 조회/수정을 보호하는 락 획득 - bd_link_disk_holder()와 동일한 락으로 상호 배제 */
	holder = bd_find_holder_disk(bdev, disk);	/* [한국어] 이 (bdev, disk) 쌍에 대해 등록된 관계 노드를 검색 */
	if (!WARN_ON_ONCE(holder == NULL) && !--holder->refcnt) {	/* [한국어] 관계를 못 찾으면 프로그래밍 오류로 경고하고 이 블록은 건너뜀; 찾았다면 refcnt를 먼저 감소시키고 그 결과가 0인 경우에만(마지막 참조 해제) 실제 정리 수행 */
		del_symlink(disk->slave_dir, bdev_kobj(bdev));	/* [한국어] /sys/block/<holder>/slaves/<slave> 링크 제거 - bd_link_disk_holder()가 만든 첫 번째 링크의 역연산 */
		del_symlink(holder->holder_dir, &disk_to_dev(disk)->kobj);	/* [한국어] /sys/block/<slave>/holders/<holder> 링크 제거 - 두 번째 링크의 역연산 (holder->holder_dir를 통해 slave kobject에 접근) */
		kobject_put(holder->holder_dir);	/* [한국어] bd_link_disk_holder()가 kobject_get()으로 올려두었던 slave의 bd_holder_dir 참조를 반납 */
		list_del_init(&holder->list);	/* [한국어] holder gendisk의 slave_bdevs 리스트에서 이 노드를 제거하고 노드 자신은 재사용 가능한 빈 리스트 상태로 초기화 */
		kfree(holder);	/* [한국어] 더 이상 참조되지 않는 bd_holder_disk 구조체 메모리 해제 */
	}
	mutex_unlock(&blk_holder_mutex);	/* [한국어] 관계를 찾았든 못 찾았든, refcnt가 0이 되었든 아니든 항상 마지막에 락 해제 */
}
EXPORT_SYMBOL_GPL(bd_unlink_disk_holder);	/* [한국어] GPL 모듈(dm-mod, md, nvme-core 등)에서 호출 가능하도록 심볼 익스포트 */
