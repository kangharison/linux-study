// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *  Copyright (C) 2001  Andrea Arcangeli <andrea@suse.de> SuSE
 *  Copyright (C) 2016 - 2020 Christoph Hellwig
 */

/*
 * [한국어 설명] block_device(bdev) 객체의 생명주기/열기·닫기/캐시 관리 (block/bdev.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 struct block_device(이하 bdev) 객체의 전체 생명주기를 관리한다.
 * bdev 는 커널이 하나의 블록 디바이스 또는 그 위의 파티션 하나하나를 표현하는
 * 자료구조로서, gendisk(물리/논리 디스크 자체)와 그 위에서 사용자가 실제로
 * open()/read()/write() 하는 "핸들" 사이의 다리 역할을 한다. 구체적으로는
 * (1) bdev 인스턴스 할당/해제(bdev_alloc, bdev_add, bdev_unhash, bdev_drop),
 * (2) 사용자/커널 클라이언트의 open/close 경로(bdev_open, bdev_release,
 *     bdev_fput, blkdev_get_whole/part, blkdev_put_whole/part),
 * (3) 배타적 소유권(exclusive claim) 관리 — 파일시스템 마운트나 LVM 등이
 *     한 디바이스를 독점적으로 사용하려 할 때 경쟁을 막는 bd_prepare_to_claim
 *     ~ bd_finish_claiming/bd_abort_claiming/bd_end_claim 계열 함수,
 * (4) 페이지/버퍼 캐시 동기화 및 무효화 — sync_blockdev(), kill_bdev(),
 *     invalidate_bdev(), truncate_bdev_range(), bdev_freeze()/bdev_thaw(),
 *     sync_bdevs(), bdev_mark_dead(),
 * (5) 블록 크기 협상 — set_blocksize(), sb_set_blocksize(), sb_min_blocksize(),
 *     set_init_blocksize(), bdev_validate_blocksize(),
 * (6) bdev 전용 익명 가상 파일시스템(bdevfs) 부트스트랩 — bdev_cache_init(),
 *     bdev_alloc_inode(), bdev_free_inode(), bd_type, bdev_sops
 * 을 모두 담당한다. 즉 이 파일은 "블록 디바이스라는 개념"을 커널의 나머지
 * 부분(VFS, 파일시스템, 사용자 공간)에 노출시키는 단일 진입점이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 블록 계층의 전체 흐름은 대략 다음과 같다:
 *   사용자 open("/dev/sda1") 또는 mount()
 *     -> lookup_bdev()/bdev_file_open_by_path()/bdev_file_open_by_dev() (본 파일)
 *     -> bdev_open() (본 파일, 실제 오픈/클레임/카운팅 수행)
 *     -> blkdev_get_whole()/blkdev_get_part() (본 파일, ->fops->open 콜백 호출)
 *     -> (이 시점부터) 파일시스템/사용자가 read()/write()로 bio 생성
 *     -> submit_bio() (block/blk-core.c) -> blk_mq_submit_bio() (block/blk-mq.c)
 *     -> 디바이스 드라이버(NVMe 등)의 request_queue 로 전달
 *   닫을 때는 반대로 fput()/bdev_fput() -> bdev_release() -> blkdev_put_whole/part()
 *   -> ->fops->release 콜백 순서로 정리된다.
 * 이 파일 자체는 항상 "호스트 커널 컨텍스트"에서 실행되며, 대부분의 함수는
 * 프로세스 컨텍스트(시스템 콜 경로, 예: open(2)/close(2)/ioctl(2)/mount(2))에서
 * 슬립 가능한 상태로 호출된다. 단, bdev_write_inode()/kill_bdev() 등 캐시
 * 정리 계열은 writeback 커널 스레드나 메모리 회수 경로에서도 호출될 수 있다.
 * genhd.c 가 gendisk(디스크 자체, 파티션 테이블, 이벤트)를 관리하고,
 * blk-core.c/blk-mq.c 가 bio/request 를 큐잉·디스패치한다면, 이 bdev.c 는
 * 그 사이에서 "누가 이 디바이스를 열었는가/독점하고 있는가/캐시 상태가
 * 어떤가"를 추적하는 계층이라고 볼 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * - block/genhd.c: struct gendisk 생명주기, disk_block_events()/
 *   disk_unblock_events()/disk_flush_events(), bdev_disk_changed()(파티션
 *   재스캔), disk_live() 등을 제공하며, 본 파일의 bdev_open()/bdev_release()가
 *   이들을 직접 호출한다. bdev->bd_disk 필드가 이 연결의 핵심 포인터다.
 * - block/blk-core.c, block/blk-mq.c: bdev_open() 이 성공적으로 반환된 이후,
 *   실제 read/write IO(bio)는 이 두 파일이 정의하는 submit_bio()/
 *   blk_mq_submit_bio() 경로를 통해 bdev->bd_queue(=disk->queue)로 전달된다.
 *   본 파일은 그 IO 자체를 만들지는 않고, IO 가 흐르기 위한 "핸들"만 관리한다.
 * - fs/*, mm/filemap.c, mm/truncate.c: bdev 는 자신만의 address_space
 *   (bdev->bd_mapping, 실제로는 bdev 전용 pseudo-inode 의 i_data)를 가지며,
 *   버퍼 캐시(buffer_head)/페이지 캐시 IO 는 이 address_space 를 통해
 *   흘러간다. sync_blockdev()/kill_bdev()/invalidate_bdev() 는 filemap.c,
 *   truncate.c 의 범용 캐시 함수를 감싸는 얇은 래퍼다.
 * - security/*: security_bdev_alloc()/security_bdev_free() 로 LSM(Linux
 *   Security Module) 훅을 통해 bdev 접근 제어 레이블을 붙이고 뗀다.
 * - kernel/cgroup/, drivers/base/core.c: devcgroup_check_permission() 으로
 *   컨테이너의 디바이스 접근 제어를, kobject/struct device 참조 카운팅으로
 *   bdev 소멸 시점을 조율한다.
 * - 공유 핵심 자료구조: struct block_device(전역, include/linux/blkdev.h 에
 *   정의) 가 이 파일 전역에서 다뤄지는 대상이다. struct bdev_inode(본 파일
 *   정의)는 VFS inode 와 block_device 를 하나의 슬래브 객체로 묶어, 두
 *   자료구조 사이를 container_of() 로 O(1) 변환할 수 있게 해준다.
 *
 * === 주요 함수/구조체 요약 ===
 * - bdev_alloc()/bdev_add()/bdev_unhash()/bdev_drop(): gendisk 등록 시점에
 *   bdev 를 만들고 dev_t 를 부여하고, 제거 시점에 되돌리는 4단계.
 * - bdev_open()/bdev_release(): 이 파일에서 가장 복잡한 두 함수로, open
 *   모드 검증, exclusive claim, opener 카운트 증가/감소, ->fops->open/release
 *   콜백 호출을 모두 조율한다.
 * - bd_prepare_to_claim()/bd_finish_claiming()/bd_abort_claiming()/
 *   bd_end_claim()/bd_may_claim(): 하나의 bdev(또는 그 전체 디바이스)를 한
 *   holder 가 배타적으로 사용하도록 보장하는 상태 기계. bdev_lock 전역
 *   뮤텍스와 whole->bd_claiming/bd_holder 필드로 구현된다.
 * - sync_blockdev()/sync_blockdev_range()/kill_bdev()/invalidate_bdev()/
 *   truncate_bdev_range()/bdev_freeze()/bdev_thaw()/sync_bdevs()/
 *   bdev_mark_dead(): 캐시 동기화·무효화·동결 계열. dirty 데이터를 디스크로
 *   내보내거나(sync), 캐시를 버리거나(invalidate/kill), 파일시스템을
 *   일관된 상태로 잠그는(freeze/thaw) 다양한 강도의 캐시 제어를 제공한다.
 * - set_blocksize()/sb_set_blocksize()/sb_min_blocksize()/
 *   set_init_blocksize()/bdev_validate_blocksize(): 페이지 캐시 folio 크기와
 *   파일시스템 블록 크기를 디바이스의 logical_block_size 에 맞춰 정렬한다.
 * - struct bdev_inode { bdev, vfs_inode }: bdev 와 VFS inode 를 하나의 슬래브
 *   할당으로 묶는 컨테이너. BDEV_I()/BD_INODE()/I_BDEV() 세 헬퍼로 양방향
 *   변환한다.
 */

#include <linux/init.h>
// [한국어] __init/__exit 매크로 - 초기화 코드/데이터를 표시하는 용도로 쓰이며, 이 조각 범위 밖의 bdev 관련 부트 초기화 함수에서 사용될 수 있다.
#include <linux/mm.h>
// [한국어] PAGE_SIZE, get_order() 등 메모리 관리 기본 정의 - set_init_blocksize() 가 folio 최소 order 를 계산할 때 사용.
#include <linux/slab.h>
// [한국어] kmem_cache 슬랩 할당자 API - struct bdev_inode 를 슬랩 객체로 할당/해제할 때 사용(이 조각 밖의 alloc_inode/destroy_inode 콜백).
#include <linux/kmod.h>
// [한국어] request_module() 등 커널 모듈 온디맨드 로드 API - 필요한 블록 드라이버(예: NVMe 드라이버)를 자동으로 적재할 때 사용.
#include <linux/major.h>
// [한국어] 블록 디바이스 메이저 번호 상수 정의 - 레거시 디바이스 번호 매칭이나 device_cgroup 권한 체크에 사용.
#include <linux/device_cgroup.h>
// [한국어] devcgroup(디바이스 cgroup) 접근 제어 API - 컨테이너 환경에서 특정 블록 디바이스 노드로의 open 을 제한할 때 사용.
#include <linux/blkdev.h>
// [한국어] struct block_device, struct gendisk, request_queue 등 블록 계층 핵심 타입 정의 - 이 파일 전체가 의존하는 가장 중요한 헤더.
#include <linux/blk-integrity.h>
// [한국어] 블록 무결성(DIF/DIX, Data Integrity Field/eXtension) 관련 API - NVMe/SCSI 의 end-to-end 데이터 보호 기능과 연동.
#include <linux/backing-dev.h>
// [한국어] backing_dev_info - 페이지 캐시 writeback 정책/큐를 표현하는 구조체로, invalidate_bdev() 류의 캐시 정리 로직과 연관.
#include <linux/module.h>
// [한국어] EXPORT_SYMBOL()/EXPORT_SYMBOL_GPL() 매크로 정의 - 이 파일의 여러 함수를 다른 커널 모듈에 공개할 때 사용.
#include <linux/blkpg.h>
// [한국어] blkpg_ioctl_arg 등 파티션 관리 ioctl 데이터 구조 정의 - BLKPG 계열 ioctl 처리에 사용(이 조각 밖).
#include <linux/magic.h>
// [한국어] BDEVFS_MAGIC 등 가상 파일시스템 매직 넘버 - bdev 전용 pseudo 파일시스템 슈퍼블록 식별에 사용.
#include <linux/buffer_head.h>
// [한국어] struct buffer_head, invalidate_bh_lrus() 등 버퍼 캐시 API - kill_bdev()/invalidate_bdev() 가 직접 호출.
#include <linux/swap.h>
// [한국어] lru_add_drain_all() 등 LRU/스왑 관련 API - invalidate_bdev() 가 per-CPU LRU 추가 캐시를 강제로 비울 때 사용.
#include <linux/writeback.h>
// [한국어] write_inode_now() 등 writeback API - bdev_write_inode() 가 dirty inode 를 동기적으로 디스크에 기록할 때 사용.
#include <linux/mount.h>
// [한국어] struct vfsmount 등 마운트 관련 정의 - bdev 전용 pseudo 파일시스템을 커널 내부적으로 마운트할 때 사용(이 조각 밖).
#include <linux/pseudo_fs.h>
// [한국어] 가상(pseudo) 파일시스템 구성 헬퍼 - struct bdev_inode 를 담는 전용 슈퍼블록을 만들 때 사용(이 조각 밖의 init 함수).
#include <linux/uio.h>
// [한국어] struct iov_iter 등 벡터 입출력 정의 - bdev 문자/블록 디바이스 read/write 시스템 호출 경로에서 사용(이 조각 밖).
#include <linux/namei.h>
// [한국어] lookup_bdev() 등 경로명(pathname) 조회 API - 장치 파일 경로 문자열로부터 block_device 를 찾을 때 사용(이 조각 밖).
#include <linux/security.h>
// [한국어] LSM(Linux Security Module) 보안 훅 - 블록 디바이스 open/ioctl 등에 대한 보안 정책 검사에 사용(이 조각 밖).
#include <linux/part_stat.h>
// [한국어] 파티션 단위 입출력 통계 카운터 매크로(part_stat_add 등) - 이 조각 밖의 통계 갱신 코드에서 사용.
#include <linux/uaccess.h>
// [한국어] copy_to_user()/copy_from_user() 등 유저 공간 접근 API - ioctl 인자 전달 등에 사용(이 조각 밖).
#include <linux/stat.h>
// [한국어] S_ISBLK() 등 파일 모드 매크로 - inode 가 블록 디바이스 노드인지 판별할 때 사용(이 조각 밖).
#include "../fs/internal.h"
// [한국어] VFS 내부 전용(비공개) 헬퍼 선언 - fs 트리 내부에서만 공유되는 심볼(예: 마운트 내부 함수)에 접근하기 위함.
#include "blk.h"
// [한국어] 블록 계층 내부(비공개) 헤더 - block 디렉터리 내부에서만 공유되는 선언(bd_prepare_to_claim, bd_abort_claiming 등)에 접근.

/*
 * [한국어] 마운트된 블록 디바이스에 대해 파일시스템을 우회하는 raw 쓰기를 허용할지
 * 정하는 전역 스위치. 커널 설정 CONFIG_BLK_DEV_WRITE_MOUNTED 의 초기값을 그대로
 * 반영한다. NVMe 관점에서 이 값이 false 이면, 마운트된 NVMe 네임스페이스(예:
 * /dev/nvme0n1p1)에 파일시스템을 거치지 않고 직접 쓰기 위해 여는 open() 이 상위
 * VFS/블록 계층에서 거부되어, 그 쓰기 요청이 NVMe 제출 큐(SQ, Submission Queue)
 * 까지 도달하지 못한다. 반대로 true 이면 관리자가 위험을 감수하고 마운트된
 * 디바이스에도 직접 쓰기를 허용한 것이므로, 파일시스템 모르게 NAND 상의 데이터가
 * 바뀔 수 있어 데이터 손상 위험이 있다.
 */

/* Should we allow writing to mounted block devices? */
static bool bdev_allow_write_mounted = IS_ENABLED(CONFIG_BLK_DEV_WRITE_MOUNTED);
/* 설정자: 커널 빌드 시 CONFIG_BLK_DEV_WRITE_MOUNTED Kconfig 값으로 고정 초기화된다.
 *        이 조각 범위 밖에서 모듈 파라미터 등을 통해 부팅 후 변경되도록 확장될 수도
 *        있으나, 이 스냅샷 시점에는 단순 정적 초기값으로만 존재한다.
 * 읽는 자: 블록 디바이스를 쓰기 모드로 여는 open 경로(예: blkdev_get_by_dev() 계열,
 *        이 조각 밖)가 대상 bdev 가 이미 파일시스템에 마운트되어 있는지 검사한 뒤
 *        이 플래그를 참조하여 쓰기 오픈 허용 여부를 최종 결정한다.
 * 값 범위: bool. true = 마운트된 bdev 에도 쓰기 오픈 허용(위험, raw 쓰기로 인한
 *        데이터 불일치 가능), false = 마운트된 bdev 의 쓰기 오픈을 거부(기본값,
 *        안전).
 * 동기화: 커널 부팅 이후 사실상 읽기 전용 상수로 취급되어 별도의 락이나 원자적
 *        접근 없이 그대로 참조한다. */

/*
 * struct bdev_inode - VFS inode와 block_device를 하나의 슬래브 객체로 묶은 구조체.
 *
 * @bdev: 실제 블록 디바이스. NVMe SSD의 한 네임스페이스(또는 파티션)에 해당.
 *        bdev->bd_queue 가 해당 NVMe 컨트롤러의 request_queue 를 가리킨다.
 * @vfs_inode: VFS 계층이 사용하는 inode. 블록 장치 노드(/dev/nvme0n1 등)와
 *             매핑된다. 파일시스템 입장에서는 여기서 bio 를 만들어 bdev 로 전달.
 */

struct bdev_inode {
	struct block_device bdev;
/* [한국어] 이 inode 슬랩 객체가 감싸고 있는 실제 struct block_device 본체(포인터가
 * 아니라 임베디드 값). NVMe 관점에서는 하나의 네임스페이스 전체(/dev/nvme0n1)
 * 또는 그 파티션(/dev/nvme0n1p1) 하나에 대응한다.
 * 설정자: bdev 전용 pseudo 파일시스템의 alloc_inode 콜백(이 조각 밖)이 이 슬랩
 *        객체를 할당할 때 bdev 부분을 초기화하고, 이후 bd_dev/bd_queue 등 필드를
 *        채워 넣는다.
 * 읽는 자: 블록 계층 전역에서 struct block_device * 포인터로 참조되는 거의 모든
 *        코드(예: 이 파일의 BDEV_I()/I_BDEV(), bio->bi_bdev 를 따라가는
 *        파일시스템/블록 드라이버 전체).
 * 값 범위: 유효한 block_device 구조체 상태. bd_openers 참조 카운트가 0이 되어
 *        gendisk 가 해제되기 전까지는 살아있는 것으로 취급된다.
 * 동기화: 개별 필드마다 서로 다른 락(bd_size_lock, holder 관련 뮤텍스 등)을
 *        사용하며, 이 구조체 자체의 존재 여부는 inode 참조 카운트(i_count)로
 *        보호된다(마지막 iput 시 슬랩 회수). */
	struct inode vfs_inode;
/* [한국어] 이 block_device 와 짝을 이루는 VFS(Virtual File System) inode 본체
 * (임베디드 값). bdev 전용 pseudo 파일시스템이 관리하는 inode 로, 사용자가
 * /dev/nvme0n1 을 open() 하면 실제로 이 inode 가 VFS 계층에서 참조된다.
 * 설정자: bdev 전용 슈퍼블록의 alloc_inode 콜백이 이 필드를 포함한 bdev_inode
 *        전체를 슬랩(kmem_cache)에서 할당하며 초기화한다. i_mapping 은 곧
 *        bdev->bd_mapping 과 연결되어 페이지 캐시를 공유하도록 설정된다.
 * 읽는 자: 이 파일의 BD_INODE()/BDEV_I() 가 상호 변환에 사용하고,
 *        set_init_blocksize() 는 i_blkbits/i_mapping 을 직접 갱신하며,
 *        bdev_write_inode() 는 이 inode 의 dirty 상태(i_state 의 I_DIRTY 비트)를
 *        검사한다.
 * 값 범위: 표준 VFS inode 필드 규약을 따른다(i_mapping, i_state, i_size 등).
 * 동기화: i_state 는 inode->i_lock 스핀락으로 보호되며(bdev_write_inode 참고),
 *        i_mapping 의 페이지 트리는 mapping 자체의 내부 락(xarray 락)으로
 *        보호된다. */
};

/*
 * [한국어]
 * BDEV_I - VFS(Virtual File System) inode 포인터로부터, 그 inode 를 담고 있는
 *          struct bdev_inode 컨테이너의 시작 주소를 역산해서 얻는다.
 *
 * @inode: 반드시 struct bdev_inode.vfs_inode 필드로 임베드되어 있는 inode
 *         포인터여야 한다. 일반 파일시스템의 inode 를 넘기면 잘못된 주소가
 *         계산되므로, 호출자가 "이 inode 는 블록 디바이스 inode 이다"라는
 *         사실을 스스로 보장해야 한다.
 * @return: 해당 inode 를 포함하는 struct bdev_inode 의 시작 주소.
 *
 * VFS 계층은 block_device 의 존재를 몰라도 되고, 블록 계층은 VFS inode 의
 * 내부 구조를 몰라도 되도록, 두 세계를 슬랩 할당 시점에 하나의 객체
 * (bdev_inode)로 묶어 두었다. container_of() 매크로는 "필드의 메모리 오프셋
 * 만큼 주소를 빼면 컨테이너의 시작 주소가 나온다"는 성질을 이용하는 컴파일
 * 타임 포인터 산술이며, 런타임 비용이 전혀 없다(주소 값 뺄셈 한 번).
 * 실행 컨텍스트: 프로세스 컨텍스트에서 주로 호출되며 락을 요구하지 않는다
 * (포인터 산술만 수행하므로 재진입/동시성 문제가 없음).
 * 호출자: 이 파일의 I_BDEV(), 그리고 블록 계층 전역에서 struct inode * 를
 *         struct block_device * 로 바꾸어야 하는 코드(이 조각 밖).
 * 피호출자: container_of() 매크로(실제 함수 호출이 아닌 컴파일 타임 계산).
 * 에러 경로: 없음 - inode 가 실제로 bdev_inode 안에 있지 않다면 정의되지 않은
 *          동작(UB)이 되므로, 애초에 그런 inode 를 넘기지 않는 것이 호출자의
 *          책임이다.
 *
 * 호출 체인:
 *   I_BDEV()/블록 계층 코드(이 조각 밖) → [BDEV_I] → container_of() 매크로
 */

static inline struct bdev_inode *BDEV_I(struct inode *inode)
{
	return container_of(inode, struct bdev_inode, vfs_inode);
// [한국어] vfs_inode 필드의 메모리 오프셋만큼 주소를 되돌려 bdev_inode 시작 주소를 구한다 - 이 결과에서 .bdev 멤버를 취하면 최종적으로 struct block_device 를 얻는다(I_BDEV 참고).
}

/*
 * [한국어]
 * BD_INODE - struct block_device 포인터로부터, 그것과 짝을 이루는 VFS inode
 *            포인터를 역산해서 얻는다(BDEV_I() 의 반대 방향 변환).
 *
 * @bdev: struct bdev_inode.bdev 필드로 임베드되어 있는 block_device 포인터.
 *        bdev_alloc() 이 만든 정상적인 block_device 여야 하며, 스택 등에 임시로
 *        만든 block_device 를 넘기면 안 된다.
 * @return: 이 bdev 와 짝을 이루는 struct inode 의 주소. 이 inode 의 i_mapping 이
 *          곧 이 bdev 의 페이지 캐시(주소 공간)이다.
 *
 * 페이지 캐시 계층(주소 공간 및 folio)은 VFS inode 단위로 동작하므로, 블록
 * 계층 코드가 캐시를 다루려면 결국 이 inode 를 거쳐야 한다. 이 함수는
 * container_of() 로 bdev 필드의 오프셋을 역산해 bdev_inode 컨테이너를 찾은 뒤
 * 그 vfs_inode 필드의 주소를 반환한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 락 불필요(순수 포인터 산술).
 * 호출자: 이 파일의 bdev_write_inode(), set_init_blocksize(), bdev_validate_blocksize()
 *         등 bdev 의 캐시나 inode 메타데이터를 직접 다뤄야 하는 함수들.
 * 피호출자: container_of() 매크로.
 * 에러 경로: 없음 - bdev 가 실제 bdev_inode 안에 있지 않으면 UB.
 *
 * 호출 체인:
 *   bdev_write_inode()/set_init_blocksize()(같은 파일) → [BD_INODE] → container_of() 매크로
 */

static inline struct inode *BD_INODE(struct block_device *bdev)
{
	return &container_of(bdev, struct bdev_inode, bdev)->vfs_inode;
// [한국어] bdev 필드의 오프셋만큼 주소를 되돌려 bdev_inode 를 구한 뒤 vfs_inode 멤버의 주소를 반환 - 페이지 캐시(i_mapping)와 dirty 상태(i_state)에 접근하는 진입점이 된다.
}

/*
 * [한국어]
 * I_BDEV - VFS inode 포인터로부터 그것이 나타내는 struct block_device 를 얻는다.
 *          커널 전역에 공개된(EXPORT_SYMBOL) 헬퍼로, 블록 디바이스 파일의 inode
 *          만 가진 코드가 실제 디바이스 객체에 접근하기 위한 공식 경로다.
 *
 * @inode: 블록 디바이스 노드(예: /dev/nvme0n1)를 open() 했을 때 만들어지는,
 *         bdev 전용 pseudo 파일시스템이 관리하는 inode. 반드시 BDEV_I() 가
 *         가정하는 대로 struct bdev_inode 안에 임베드되어 있어야 한다.
 * @return: 해당 inode 가 나타내는 struct block_device 포인터. 실패 개념이 없고
 *          항상 유효한 포인터를 돌려준다(입력이 잘못된 경우는 호출자 책임의 UB).
 *
 * 파일시스템/드라이버가 struct file 이나 struct inode 만 들고 있을 때, 그 안에
 * 숨어 있는 실제 block_device 를 꺼내기 위해 존재한다. 예를 들어 이 파일의
 * file_bdev() 가 struct file -> inode -> block_device 순서로 변환할 때 마지막
 * 단계에서 이 함수를 사용한다.
 * 실행 컨텍스트: 프로세스 컨텍스트에서 흔히 호출되며 락이 필요 없다(BDEV_I()
 * 와 동일하게 순수 포인터 산술).
 * 호출자: 이 파일의 file_bdev(), 그리고 EXPORT_SYMBOL 로 공개되어 있으므로
 *         버퍼 캐시 코드(fs/buffer.c 등, 이 조각 밖)를 비롯한 커널 전역의 블록
 *         디바이스 관련 코드가 폭넓게 호출한다.
 * 피호출자: 이 파일의 BDEV_I().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   file_bdev()/버퍼 캐시 등 커널 전역 호출자(이 조각 밖) → [I_BDEV] → BDEV_I()
 */

struct block_device *I_BDEV(struct inode *inode)
{
	return &BDEV_I(inode)->bdev;
// [한국어] BDEV_I() 로 컨테이너를 찾은 뒤 그 bdev 필드의 주소를 반환 - 파일시스템이 만든 bio 가 실제로 어느 NVMe 네임스페이스/파티션(block_device)을 향하는지 여기서 확정된다.
}
EXPORT_SYMBOL(I_BDEV);
// [한국어] 이 심볼을 모든 커널 모듈에 공개(EXPORT_SYMBOL, GPL 무관) - 파일시스템/드라이버 모듈이 링크 타임에 I_BDEV() 를 호출할 수 있게 한다.

/*
 * [한국어]
 * file_bdev - 열려 있는 struct file 로부터 그것이 가리키는 struct block_device
 *             를 얻는다. 블록 디바이스 노드를 open() 한 struct file 에 대해서만
 *             유효하다(EXPORT_SYMBOL 로 커널 전역에 공개).
 *
 * @bdev_file: 블록 디바이스 노드(예: /dev/nvme0n1)를 open() 하여 얻은 struct
 *             file. f_mapping->host 가 해당 블록 디바이스의 VFS inode 를
 *             가리키고 있어야 한다(blkdev_open() 계열이 이렇게 설정, 이 조각
 *             밖).
 * @return: 이 파일이 나타내는 struct block_device 포인터.
 *
 * ioctl 이나 read/write 시스템 호출 핸들러가 struct file * 만 받았을 때, 그
 * 배후의 실제 block_device 를 얻기 위한 표준 경로다. f_mapping->host 로 VFS
 * inode 를 얻은 뒤 I_BDEV() 로 한 번 더 변환한다.
 * 실행 컨텍스트: 시스템 호출 처리 중인 프로세스 컨텍스트, 락 불필요(포인터
 * 산술만 수행).
 * 호출자: 블록 디바이스 파일에 대한 ioctl/read/write 등을 처리하는 커널 전역
 *         코드(예: block/ioctl.c, 이 조각 밖) - EXPORT_SYMBOL 이므로 모듈에서도
 *         호출 가능.
 * 피호출자: 이 파일의 I_BDEV().
 * 에러 경로: 없음 - bdev_file 이 블록 디바이스를 가리키지 않는 경우는 호출자가
 *          보장해야 하는 사전조건 위반(UB).
 *
 * 호출 체인:
 *   ioctl/read/write 시스템 호출 핸들러(이 조각 밖) → [file_bdev] → I_BDEV()
 */

struct block_device *file_bdev(struct file *bdev_file)
{
	return I_BDEV(bdev_file->f_mapping->host);
// [한국어] struct file 의 f_mapping->host 로 VFS inode 를 얻은 뒤 I_BDEV() 로 block_device 로 변환 - 결과적으로 struct file -> inode -> bdev_inode -> block_device 경로를 한 번에 수행한다.
}
EXPORT_SYMBOL(file_bdev);
// [한국어] 이 심볼을 모든 커널 모듈에 공개(EXPORT_SYMBOL) - open 된 struct file 만 가진 코드가 이 함수로 block_device 를 얻을 수 있게 한다.

/*
 * [한국어]
 * bdev_write_inode - bdev 의 VFS inode 가 dirty 상태이면 그 메타데이터를 즉시
 *                    (synchronous) writeback 하여 클린 상태로 만든다.
 *
 * @bdev: writeback 대상 block_device. 이 함수는 반환형이 void 이므로 실패를
 *        직접 호출자에게 알리지 않고 pr_warn_ratelimited() 로 로그만 남긴다.
 *
 * bdev 를 닫거나(close), 캐시를 통째로 비우거나(kill_bdev 등), 파티션 테이블을
 * 다시 읽는 등 "이 디바이스의 캐시된 메타데이터를 이제부터 신뢰할 수 없게
 * 만드는" 작업 직전에, 아직 디스크에 반영되지 않은 inode 메타데이터(파일 크기,
 * 타임스탬프 등)를 먼저 디스크로 흘려보내기 위해 존재한다. 이 메타데이터
 * writeback 은 결국 NVMe 컨트롤러로 Write 커맨드(경우에 따라 Flush 커맨드)가
 * 제출되는 결과로 이어진다.
 * 동작 과정: inode->i_lock 스핀락을 잡고 i_state 의 I_DIRTY 비트(I_DIRTY_SYNC,
 * I_DIRTY_DATASYNC, I_DIRTY_PAGES 등을 포함하는 마스크)가 서 있는 동안 while
 * 루프를 돈다. 매 반복마다: (1) write_inode_now() 를 호출하려면 잠들 수
 * 있으므로 스핀락을 반드시 먼저 풀고, (2) write_inode_now(inode, true) 로
 * "wait=true" 동기 writeback 을 수행하고, (3) 실패하면 경고 로그를 남기고,
 * (4) 재검사를 위해 다시 스핀락을 잡는다. 다른 CPU 가 writeback 도중에 inode
 * 를 다시 dirty 로 만들 수 있으므로(예: 동시에 파일 크기를 바꾸는 경로), 한
 * 번의 write_inode_now() 로 끝내지 않고 I_DIRTY 비트가 완전히 사라질 때까지
 * 반복하는 것이 이 while 루프의 핵심이다.
 * 실행 컨텍스트: 프로세스 컨텍스트(주로 close/discard 등 동기 경로). 함수
 * 시작 시 락 없는 상태로 들어와서, 함수 종료 시에도 락 없는 상태로 나간다
 * (내부적으로 lock/unlock 을 짝을 맞춰 반복).
 * 호출자: 이 조각 범위 밖의 bdev.c 코드(추정: 디바이스를 닫거나 크기 변경을
 *         감지해 캐시를 무효화하기 직전의 경로)에서, dirty 메타데이터가 남은
 *         채로 캐시를 버리는 것을 막기 위해 호출.
 * 피호출자: BD_INODE(), write_inode_now(), pr_warn_ratelimited().
 * 에러 경로: write_inode_now() 가 실패해도 함수는 계속 진행하며(단, 다음
 * 반복에서 I_DIRTY 가 여전히 서 있으면 다시 시도), 최종적으로는 경고 로그만
 * 남기고 조용히 반환한다 - 이 함수가 void 이기 때문에 실패를 상위로 전파할
 * 방법이 없다.
 *
 * 호출 체인:
 *   (추정) bdev 종료/무효화 경로(이 조각 밖) → [bdev_write_inode] → write_inode_now() → NVMe Write/Flush 커맨드 제출
 */

static void bdev_write_inode(struct block_device *bdev)
{
	struct inode *inode = BD_INODE(bdev);
// [한국어] 이 bdev 와 짝을 이루는 VFS inode 획득 - 이후 i_lock/i_state 검사와 write_inode_now() 호출 대상이 된다.
	int ret;
// [한국어] write_inode_now() 의 반환값(에러 코드)을 담을 지역 변수 - 0이면 성공, 음수면 errno 스타일 에러 코드.

	spin_lock(&inode->i_lock);
// [한국어] i_state 필드(I_DIRTY 비트 포함)를 검사하기 전에 잠금 - 다른 CPU 가 동시에 이 inode 를 dirty 로 표시하거나 writeback 상태를 바꾸는 경쟁을 막는다.
	while (inode_state_read(inode) & I_DIRTY) {
// [한국어] I_DIRTY 마스크(I_DIRTY_SYNC/I_DIRTY_DATASYNC/I_DIRTY_PAGES 등)에 해당하는 비트가 하나라도 서 있으면 아직 디스크에 반영되지 않은 메타데이터가 있다는 뜻 - 전부 사라질 때까지 반복한다.
		spin_unlock(&inode->i_lock);
// [한국어] write_inode_now() 는 실제 입출력을 기다리며 잠들 수 있으므로(sleep 가능), 스핀락을 쥔 채로는 호출할 수 없어 여기서 미리 해제한다.
		ret = write_inode_now(inode, true);
// [한국어] wait 인자를 true 로 주어 동기(synchronous) writeback 수행 - 이 inode 의 더티 메타데이터가 실제로 디스크(NVMe 라면 volatile write cache 를 거쳐 NAND)에 기록될 때까지 이 호출이 반환하지 않는다.
		if (ret)
// [한국어] write_inode_now() 가 음수(에러 코드)를 반환한 경우에만 진입 - 이 함수는 반환형이 void 라서 실패를 호출자에게 전달할 방법이 없으므로, pr_warn_ratelimited() 로 rate-limit 된 경고만 남기고 계속 진행한다. %pg 포맷은 block_device 포인터를 사람이 읽을 수 있는 디바이스 이름(예: nvme0n1)으로 출력하는 printk 확장 지정자이고, ret 은 write_inode_now() 가 돌려준 음수 errno 값이다.
			pr_warn_ratelimited(
	"VFS: Dirty inode writeback failed for block device %pg (err=%d).\n",
				bdev, ret);
		spin_lock(&inode->i_lock);
// [한국어] while 조건을 다시 검사하기 위해 락을 재획득 - writeback 도중 다른 CPU 가 다시 dirty 로 만들었을 가능성이 있으므로 재확인이 필요하다.
// [한국어] 이 지점에서 락을 든 채로 while 조건을 재평가한다 - I_DIRTY 가 여전히 서 있다면(동시 dirty 발생) 위 과정을 처음부터 반복하고, 완전히 클린해질 때까지 루프를 빠져나가지 않는다.
	}
	spin_unlock(&inode->i_lock);
// [한국어] while 루프를 빠져나왔다는 것은 I_DIRTY 비트가 전부 클리어된 상태 - 마지막으로 잠금을 풀어 이 함수 진입 이전과 동일한 unlocked 상태로 되돌린다.
}

/*
 * [한국어]
 * kill_bdev - bdev 의 버퍼 캐시와 페이지 캐시를 dirty 여부에 관계없이 전부
 *             제거한다(가장 강력한 캐시 드레인 함수).
 *
 * @bdev: 캐시를 완전히 비울 대상 block_device.
 *
 * 디스크 미디어 자체가 바뀌었거나(예: 파티션 재구성, 다른 디스크로의 물리적
 * 교체가 감지된 경우, 또는 외장 NVMe 처럼 탈부착 가능한 미디어가 제거되는
 * 경우) 캐시된 내용이 더 이상 실제 미디어 내용과 대응하지 않게 되는 상황에서,
 * dirty 데이터까지 포함해 캐시를 통째로 버려야 할 때 사용한다. invalidate_bdev()
 * 와 달리 dirty 페이지도 그냥 버린다는 점에서 데이터 유실을 감수하는, 훨씬
 * 공격적인 함수다.
 * 동작 과정: (1) mapping_empty() 로 캐시가 이미 비어 있는지 빠르게 확인해
 * 비어 있으면 아무 일도 하지 않고 반환(불필요한 락/스캔 비용 회피), (2)
 * invalidate_bh_lrus() 로 버퍼 헤드 LRU 캐시를 무효화, (3)
 * truncate_inode_pages(mapping, 0) 으로 오프셋 0부터 끝까지 모든 페이지를
 * dirty 여부와 상관없이 잘라낸다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 별도의 락을 이 함수 자신이 잡지는 않는다
 * (truncate_inode_pages() 내부에서 필요한 mapping 락을 처리).
 * 호출자: 이 조각 범위 밖의 디스크 제거/미디어 변경 경로(추정: 파티션 재검사,
 *         del_gendisk() 계열, 또는 미디어 체인지 처리).
 * 피호출자: mapping_empty(), invalidate_bh_lrus(), truncate_inode_pages().
 * 에러 경로: 없음(반환형 void) - 캐시 제거는 항상 성공한다고 가정한다.
 *
 * 호출 체인:
 *   (추정) 디스크 제거/미디어 변경 경로(이 조각 밖) → [kill_bdev] → truncate_inode_pages()
 */

/* Kill _all_ buffers and pagecache , dirty or not.. */
static void kill_bdev(struct block_device *bdev)
{
	struct address_space *mapping = bdev->bd_mapping;
// [한국어] bdev->bd_mapping - 이 블록 디바이스의 페이지 캐시(주소 공간)를 로컬 변수로 캐싱 - 이후 mapping_empty() 검사와 truncate_inode_pages() 호출에 재사용한다.

	if (mapping_empty(mapping))
// [한국어] 캐시에 페이지가 하나도 없으면(처음부터 비어있거나 이미 다른 경로로 비워진 상태) 더 할 일이 없으므로 즉시 반환 - 불필요한 LRU 무효화/페이지 스캔 비용을 아낀다.
		return;
// [한국어] 캐시가 비어 있는 경우의 조기 반환(early return) - 아래의 실제 캐시 제거 로직은 실행하지 않는다.

	invalidate_bh_lrus();
// [한국어] 각 CPU 의 버퍼 헤드(buffer_head) LRU 캐시를 무효화 - truncate 전에 이걸 먼저 해야, 다른 CPU 의 LRU 에 캐싱된 buffer_head 가 살아남아 이후 재사용되는 것을 막는다.
	truncate_inode_pages(mapping, 0);
// [한국어] 오프셋 0부터 매핑 끝까지 모든 페이지를 dirty 여부와 무관하게 캐시에서 제거 - invalidate_*() 계열과 달리 dirty 페이지도 예외 없이 버리므로, 반드시 디스크와의 불일치를 감수해도 되는 상황에서만 호출해야 한다.
}

/*
 * [한국어]
 * invalidate_bdev - 깨끗하고(clean) 현재 사용되지 않는 버퍼/페이지 캐시만
 *                   선택적으로 무효화한다(dirty 데이터는 절대 건드리지 않음).
 *                   EXPORT_SYMBOL 로 커널 전역/모듈에 공개.
 *
 * @bdev: 캐시를 정리할 대상 block_device.
 *
 * kill_bdev() 가 "무조건 다 버리는" 함수라면, 이 함수는 "안전하게 버릴 수 있는
 * 것만 버리는" 함수다. NVMe 디바이스가 예상치 못하게 제거되었다가(surprise
 * removal) 다시 나타났을 때, 혹은 디바이스 매퍼/MD 레이드가 하위 디바이스의
 * 캐시를 정리하고 싶을 때, dirty 데이터(아직 디스크에 반영되지 않은 사용자
 * 쓰기)까지 잃어버리면 안 되므로 이 함수를 사용한다.
 * 동작 과정: mapping->nrpages 로 캐시된 페이지가 하나라도 있는지 먼저 확인해
 * 없으면 아무 일도 하지 않고(불필요한 LRU 드레인/스캔 회피), 있으면 (1)
 * invalidate_bh_lrus() 로 버퍼 헤드 LRU 를 무효화, (2) lru_add_drain_all() 로
 * 모든 CPU 의 LRU 추가 캐시를 강제로 비우고(각 CPU 의 pagevec 에 대기 중인
 * 페이지를 실제 LRU 리스트에 반영), (3) invalidate_mapping_pages(mapping, 0, -1)
 * 로 매핑 전체 범위(0부터 끝까지, -1 은 "끝까지"를 의미)에서 클린 페이지만
 * 골라 제거한다. dirty 페이지나 잠겨(locked) 있거나 참조 중인 페이지는
 * invalidate_mapping_pages() 내부적으로 건너뛰므로 안전하다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 이 함수 자체는 락을 잡지 않는다(호출하는
 * 각 헬퍼 내부에서 필요한 락을 처리).
 * 호출자: EXPORT_SYMBOL 이므로 커널 전역/모듈에서 폭넓게 호출(추정: 미디어
 *         변경 감지, MD/DM 리사이즈 등, 이 조각 밖).
 * 피호출자: invalidate_bh_lrus(), lru_add_drain_all(), invalidate_mapping_pages().
 * 에러 경로: 없음(반환형 void) - 무효화 가능한 페이지만 선택적으로 처리하므로
 * 실패 개념이 없다.
 *
 * 호출 체인:
 *   (추정) 미디어 변경/리사이즈 감지 경로(이 조각 밖) → [invalidate_bdev] → invalidate_mapping_pages()
 */

/* Invalidate clean unused buffers and pagecache. */
void invalidate_bdev(struct block_device *bdev)
{
	struct address_space *mapping = bdev->bd_mapping;
// [한국어] 이 bdev 의 페이지 캐시(주소 공간) 획득 - 아래 nrpages 검사와 invalidate_mapping_pages() 호출의 대상이 된다.

	if (mapping->nrpages) {
// [한국어] 캐시된 페이지가 하나 이상 있을 때만 아래 무효화 로직을 실행 - nrpages 가 0이면 이미 깨끗하므로 LRU 드레인 등 비용을 아예 건너뛴다.
		invalidate_bh_lrus();
// [한국어] 각 CPU 의 버퍼 헤드 LRU 를 무효화 - 이후 invalidate_mapping_pages() 가 스캔할 때 최신 상태를 보도록 선행 정리한다.
		lru_add_drain_all();	/* make sure all lru add caches are flushed */
// [한국어] 모든 CPU 의 pagevec(페이지 추가 대기열)을 실제 LRU 리스트로 강제 반영 - 그래야 뒤이은 invalidate_mapping_pages() 가 방금 캐시에 들어온 페이지까지 놓치지 않고 검사할 수 있다.
		invalidate_mapping_pages(mapping, 0, -1);
// [한국어] 매핑의 페이지 오프셋 0부터 -1(끝까지)까지 범위에서 클린 페이지만 골라 무효화 - dirty 이거나 잠겨 있거나 참조 카운트가 남아 있는 페이지는 내부적으로 건너뛰어 데이터 손실이 없다.
	}
// [한국어] 이 if 블록 전체의 결과 요약: 캐시된 clean 페이지만 무효화되고, dirty 페이지는 그대로 보존되어 NVMe writeback 이 예정대로 진행될 수 있다 - kill_bdev() 와 달리 데이터 손실이 없는 안전한 정리.
}
EXPORT_SYMBOL(invalidate_bdev);
// [한국어] 이 심볼을 모든 커널 모듈에 공개(EXPORT_SYMBOL) - 파일시스템/드라이버/디바이스 매퍼 등이 미디어 변경 시 클린 캐시만 정리하고 싶을 때 호출한다.

/*
 * Drop all buffers & page cache for given bdev range. This function bails
 * with error if bdev has other exclusive owner (such as filesystem).
 */
/*
 * [한국어]
 * truncate_bdev_range - 지정한 바이트 범위에 대해 bdev 의 버퍼/페이지 캐시를
 *                       미리 정리하여, 뒤이은 discard/trim/zero-out 작업과
 *                       캐시 내용이 어긋나지 않도록 한다.
 *
 * @bdev: 캐시를 정리할 대상 block_device.
 * @mode: 이 bdev 를 연 open 모드 플래그(blk_mode_t). BLK_OPEN_EXCL 비트가 서
 *        있으면 이미 배타적으로 열려 있다는 뜻이므로 별도로 클레임을 새로 잡을
 *        필요가 없다.
 * @lstart: 정리할 범위의 시작 바이트 오프셋(inclusive).
 * @lend: 정리할 범위의 끝 바이트 오프셋(inclusive) - '끝' 인자가 포함
 *        (inclusive)이라서 반올림을 해도 안전하다는 점이 아래 폴백 경로에서
 *        중요하다.
 * @return: 성공 시 0. 배타적 클레임에 실패했더라도 invalidate 로 폴백해
 *          성공하면 그 결과(보통 0)를 반환하고, invalidate 마저 실패하면 그
 *          함수의 음수 에러 코드를 그대로 반환한다.
 *
 * discard(trim)나 write-zeroes 같은 명령은 디바이스에 "이 영역의 내용을 신경
 * 쓰지 않는다"고 알리는 것이므로, 그 전에 호스트 페이지 캐시에 남아 있는 옛
 * 내용을 반드시 지워야 한다 - 그렇지 않으면 discard 이후에도 캐시가 옛
 * 데이터를 계속 돌려줄 수 있다. 다만 살아있는 파일시스템이 이 bdev 를
 * 배타적으로 물고 있는 상태에서 함부로 페이지를 지우면 그 파일시스템이 알지
 * 못하는 사이 캐시가 사라지는 사고가 날 수 있으므로, 이 함수는 먼저 "내가
 * 지금 이 디바이스를 잠시 배타적으로 쓸 수 있는가"를 확인한다.
 * 동작 과정: (1) 호출자가 이미 BLK_OPEN_EXCL 로 열지 않았다면
 * bd_prepare_to_claim() 으로 임시 배타적 클레임을 시도한다. 실패하면(이미 다른
 * 소유자, 예: 마운트된 파일시스템이 있다는 뜻) invalidate 레이블로 점프해 더
 * 보수적인 방법으로 전환한다. (2) 클레임에 성공했으면(또는 원래부터
 * BLK_OPEN_EXCL 이었으면) truncate_inode_pages_range() 로 해당 범위의 캐시를
 * dirty 여부와 무관하게 강제로 잘라낸다(discard 대상이므로 내용을 보존할
 * 필요가 없다). (3) 임시로 잡았던 클레임을 bd_abort_claiming() 으로 해제하고
 * 성공(0)을 반환한다. (4) 클레임 획득에 실패했을 경우의 invalidate: 레이블
 * 에서는, 다른 소유자가 있으므로 truncate 대신 invalidate_inode_pages2_range()
 * 로 "지울 수 있는 클린 페이지만" 조심스럽게 정리하고(dirty 페이지/파일
 * 시스템이 보고 있는 페이지는 건드리지 않음), 바이트 오프셋을 PAGE_SHIFT 만큼
 * 오른쪽으로 시프트해 페이지 인덱스로 변환해 전달한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 처리 등 동기 경로), 이 함수 자체는
 * 락을 직접 잡지 않고 bd_prepare_to_claim()/bd_abort_claiming() 내부의
 * bd_holder 관련 보호에 의존한다.
 * 호출자: (추정) BLKDISCARD/BLKZEROOUT 계열 블록 디바이스 ioctl 핸들러, 그리고
 *         fallocate() 로 구멍을 뚫는(hole-punch) 블록 디바이스 파일 연산
 *         경로(이 조각 밖).
 * 피호출자: bd_prepare_to_claim(), truncate_inode_pages_range(),
 *          bd_abort_claiming(), invalidate_inode_pages2_range().
 * 에러 경로: 클레임 실패(err != 0) 시 goto invalidate 로 점프해
 * invalidate_inode_pages2_range() 의 반환값을 그대로 호출자에게 돌려준다.
 *
 * 호출 체인:
 *   (추정) BLKDISCARD/BLKZEROOUT ioctl 또는 fallocate hole-punch(이 조각 밖)
 *   → [truncate_bdev_range] → truncate_inode_pages_range() 또는(폴백)
 *   invalidate_inode_pages2_range()
 */

int truncate_bdev_range(struct block_device *bdev, blk_mode_t mode,
			loff_t lstart, loff_t lend)
{
	/*
	 * If we don't hold exclusive handle for the device, upgrade to it
	 * while we discard the buffer cache to avoid discarding buffers
	 * under live filesystem.
	 */
	if (!(mode & BLK_OPEN_EXCL)) {
// [한국어] 호출자가 이미 BLK_OPEN_EXCL 로 배타적 오픈을 갖고 있지 않다면(즉 공유 오픈이라면), discard 로 캐시를 지우는 동안 다른 사용자가 끼어들지 못하도록 잠시 배타적 클레임을 새로 잡아야 한다.
		int err = bd_prepare_to_claim(bdev, truncate_bdev_range, NULL);
// [한국어] truncate_bdev_range 자기 자신의 함수 포인터를 클레임 소유자(holder) 식별자로 넘겨 이 bdev 를 잠시 배타적으로 예약 - 다른 프로세스가 동시에 배타적으로 열려고 하면 이 클레임과 충돌해 대기하거나 실패하게 되어, discard 준비 중인 캐시가 또 다른 파일시스템에 의해 어지럽혀지는 것을 막는다.
		if (err)
// [한국어] bd_prepare_to_claim() 이 실패(err != 0)했다는 것은 이미 다른 소유자(예: 마운트된 파일시스템)가 이 bdev 를 배타적으로 쓰고 있다는 뜻이다. 무리하게 truncate 하지 않고 invalidate 레이블로 점프해, 클린 페이지만 조심스럽게 정리하는 더 보수적인 경로로 전환한다.
			goto invalidate;
	}

	truncate_inode_pages_range(bdev->bd_mapping, lstart, lend);
// [한국어] 지정된 [lstart, lend] 바이트 범위의 페이지 캐시를 dirty 여부와 무관하게 강제로 잘라낸다 - discard/write-zeroes 대상 영역이므로 기존 캐시 내용을 보존할 이유가 없고, 오히려 남겨두면 디바이스의 실제 내용(0 또는 정의되지 않은 값)과 캐시가 어긋나게 된다.
	if (!(mode & BLK_OPEN_EXCL))
// [한국어] 처음에 우리가 직접 bd_prepare_to_claim() 으로 임시 클레임을 잡았을 때만(즉 BLK_OPEN_EXCL 이 아니었을 때) 그 클레임을 되돌려줘야 한다 - 원래부터 BLK_OPEN_EXCL 로 배타적으로 연 호출자라면 이 클레임은 우리가 잡은 것이 아니므로 abort 할 필요가 없다.
		bd_abort_claiming(bdev, truncate_bdev_range);
// [한국어] 임시로 잡았던 배타적 클레임을 반납 - 이 시점 이후로는 다른 프로세스/파일시스템도 다시 이 bdev 를 정상적으로 열거나 마운트할 수 있고, NVMe 로의 일반 read/write 요청도 다시 자유롭게 오갈 수 있다.
	return 0;
// [한국어] 지정한 범위의 캐시 정리(discard/trim 전 준비)가 성공적으로 끝났음을 호출자에게 알림 - 이 반환값을 받은 호출자는 이어서 실제 디바이스에 discard/write-zeroes 커맨드를 내려보낸다.

invalidate:
// [한국어] goto 목적지 레이블 - 배타적 클레임을 얻지 못했을 때(위의 if(err) 분기) 이리로 건너뛰어, 아래의 더 보수적인 invalidate_inode_pages2_range() 경로로 대체 처리한다.
	/*
	 * Someone else has handle exclusively open. Try invalidating instead.
	 * The 'end' argument is inclusive so the rounding is safe.
	 */
	return invalidate_inode_pages2_range(bdev->bd_mapping,
// [한국어] 다른 소유자가 이미 배타적으로 열고 있어 클레임에 실패한 경우의 폴백(fallback) - truncate 대신 무효화(invalidate) 를 시도해 그 소유자(파일시스템)가 아직 참조/캐시 중인 페이지는 건드리지 않고, 지울 수 있는 클린 페이지만 골라 제거한다. 바이트 오프셋인 lstart/lend 를 PAGE_SHIFT(전형적으로 12, 즉 4KiB 페이지) 만큼 오른쪽으로 시프트해 페이지 인덱스로 변환해서 넘기며, lend 가 inclusive 라는 규약 덕분에 이 시프트 변환이 경계에서도 안전하다.
					     lstart >> PAGE_SHIFT,
					     lend >> PAGE_SHIFT);
}

/*
 * [한국어]
 * set_init_blocksize - bdev 의 초기 블록 크기(i_blkbits)와 페이지 캐시 folio 의
 *                      최소 order 를 디바이스의 논리 블록 크기에 맞추어
 *                      설정한다.
 *
 * @bdev: 초기화 대상 block_device. bdev_logical_block_size(bdev) 가 이미 NVMe
 *        Identify Namespace 등으로부터 채워져 있어야 한다(이 함수가 호출되는
 *        시점에는 gendisk/queue 초기화가 끝나 있다고 가정).
 *
 * 디바이스를 처음 열거나(open) 크기가 바뀌었을 때, VFS inode 의 블록 크기
 * (i_blkbits)와 페이지 캐시가 다루는 최소 folio 크기를 디바이스의 논리 블록
 * 크기와 일치시켜야, 그 위에서 만들어지는 buffer_head/bio 들이 디바이스가
 * 요구하는 정렬을 항상 만족한다. 이 함수는 그 초기값을 계산해서 실제로
 * 반영한다.
 * 동작 과정: (1) bdev_logical_block_size() 로 디바이스의 최소 블록 크기
 * (bsize, 2의 거듭제곱)를 얻고, (2) i_size_read() 로 디바이스의 전체 바이트
 * 크기(size)를 얻는다. (3) bsize 가 PAGE_SIZE 보다 작은 동안 반복해서 두
 * 배씩 키우려고 시도하는데, 이때 "size & bsize" 라는 비트 트릭으로 "지금의
 * 두 배(2*bsize)로 키워도 되는가"를 판단한다: bsize 는 항상 2의 k제곱 형태의
 * 거듭제곱이고, 루프에 들어올 때마다 size 는 이미 현재 bsize 의 배수라는
 * 불변식이 유지된다고 가정하면, size 의 하위 k 비트는 전부 0이다. 이 상태
 * 에서 "size & bsize" 는 정확히 k번째 비트(값으로는 bsize 자신)만 검사하는
 * 것과 같다 - 그 비트가 1이면 size 를 2의 (k+1)제곱으로 나눈 나머지가 정확히
 * bsize 가 되어 0 이 아니므로, size 는 2*bsize 의 배수가 아니다(더 키우면
 * 정렬이 깨짐) → break. 그 비트가 0이면 하위 k+1비트가 모두 0이 되어 size 가
 * 2*bsize 의 배수임이 보장되므로 안전하게 bsize 를 두 배로 키우고 다음
 * 자리수에 대해 같은 검사를 반복한다. (4) 이렇게 정해진 최종 bsize 를
 * blksize_bits() 로 비트 수(i_blkbits)로 변환해 inode 에 저장하고, (5)
 * mapping_set_folio_min_order() 로 페이지 캐시가 이 블록 크기보다 잘게
 * 쪼개진 folio 를 만들지 않도록 최소 order 를 지정한다(작은 folio 가 섞이면
 * 하나의 논리 블록이 여러 folio 에 걸쳐 DMA 단편화를 일으킬 수 있음).
 * 실행 컨텍스트: 프로세스 컨텍스트(디바이스 open 경로), 락 불필요 - 이
 * 시점에는 아직 다른 코드가 이 bdev 의 i_blkbits 를 동시에 바꿀 수 없다고
 * 가정(open 시퀀스 상에서 단독 소유).
 * 호출자: (추정) bdev_alloc() 또는 blkdev_get_whole() 계열의 bdev 최초
 *         open/초기화 경로(이 조각 밖).
 * 피호출자: bdev_logical_block_size(), i_size_read(), BD_INODE(),
 *          blksize_bits(), mapping_set_folio_min_order(), get_order().
 * 에러 경로: 없음(반환형 void) - 항상 계산 가능한 값으로 수렴한다(최악의
 * 경우 최초 bsize 그대로 유지).
 *
 * 호출 체인:
 *   (추정) bdev_alloc()/blkdev_get_whole()(이 조각 밖) → [set_init_blocksize] → mapping_set_folio_min_order()
 */

static void set_init_blocksize(struct block_device *bdev)
{
	unsigned int bsize = bdev_logical_block_size(bdev);
// [한국어] 디바이스의 논리 블록 크기(logical_block_size) 획득 - NVMe 라면 네임스페이스의 LBAF(Logical Block Address Format)에 따라 보통 512바이트 또는 4096바이트(4KiB)이며, 항상 2의 거듭제곱이라는 것이 아래 비트 트릭의 전제 조건이다.
	loff_t size = i_size_read(BD_INODE(bdev));
// [한국어] 이 bdev 의 VFS inode 로부터 전체 바이트 크기를 읽는다 - NVMe 라면 Identify Namespace 로 보고된 총 LBA(Logical Block Address) 개수에 logical_block_size 를 곱한 값이 여기 반영되어 있다.

	while (bsize < PAGE_SIZE) {
// [한국어] 현재 bsize 가 아직 한 페이지(PAGE_SIZE)보다 작은 동안에만 반복 - 목표는 folio 최소 단위를 페이지 크기 이하의 범위 안에서 가능한 한 크게 만드는 것이다.
		if (size & bsize)
// [한국어] size 가 2*bsize 의 배수가 아니면(즉 다음 자리수 비트가 서 있으면) 더 이상 키울 수 없다는 뜻 - size & bsize 는 bsize 가 2의 거듭제곱이고 size 가 이미 bsize 의 배수라는 불변식 하에서, 정확히 '그 다음 비트'가 서 있는지만 검사하는 관용적 트릭이다(자세한 유도 과정은 위 함수 설명 참고).
			break;
// [한국어] 정렬이 깨지는 지점을 찾았으므로 루프를 즉시 종료 - 지금까지의 bsize 를 최종값으로 확정한다.
		bsize <<= 1;
// [한국어] 정렬 단위를 2배로 늘려 다음 자리수에 대해 같은 검사를 반복 - bsize 는 여전히 2의 거듭제곱이므로 다음 반복에서도 동일한 비트 트릭이 성립한다. PRP(Physical Region Page) 엔트리는 메모리 주소가 특정 경계에 정렬되어 있을 것을 요구하므로, 이렇게 정렬된 블록 크기를 쓰는 것이 유리하다.
	}
	BD_INODE(bdev)->i_blkbits = blksize_bits(bsize);
// [한국어] 최종 확정된 bsize 를 비트 수(log2)로 변환해 inode->i_blkbits 에 저장 - 파일시스템/블록 계층이 만드는 buffer_head 나 bio 의 최소 입출력 단위가 이 값을 기준으로 정해지므로, NVMe LBA 경계와 어긋나지 않게 된다.
	mapping_set_folio_min_order(BD_INODE(bdev)->i_mapping,
// [한국어] 페이지 캐시가 이 블록 크기보다 더 작은 folio 를 만들지 않도록 최소 order 를 지정 - get_order(bsize) 는 bsize 를 담기 위해 필요한 2의 거듭제곱 페이지 개수의 지수(order)를 계산하며, 이렇게 하면 하나의 논리 블록이 여러 개의 작은 folio 에 걸쳐 조각나는 것을 막아 DMA/PRP 단편화를 예방한다.
				    get_order(bsize));
}

/**
 * bdev_validate_blocksize - check that this block size is acceptable
 * @bdev:	blockdevice to check
 * @block_size:	block size to check
 *
 * For block device users that do not use buffer heads or the block device
 * page cache, make sure that this block size can be used with the device.
 *
 * Return: On success zero is returned, negative error code on failure.
 */
/*
 * [한국어]
 * bdev_validate_blocksize - 사용자가 지정한 블록 크기가 이 디바이스에서 사용
 *                           가능한 크기인지 검증한다.
 *
 * @bdev: 검증 대상 block_device. bdev_logical_block_size(bdev) 가 이 디바이스가
 *        지원하는 최소 블록 크기를 알려준다.
 * @block_size: 사용자가 설정하려는 블록 크기(바이트 단위). 보통 512, 1024,
 *              2048, 4096 등 2의 거듭제곱 값이 들어온다.
 * @return: 검증 통과 시 0, 실패 시 음수 에러 코드(-EINVAL) - 호출자는 음수를
 *          받으면 해당 블록 크기로의 설정을 포기하고 에러를 상위로 전파해야
 *          한다.
 *
 * 버퍼 헤드(buffer head)나 블록 디바이스 자체의 페이지 캐시를 쓰지 않는(예:
 * iomap 기반의 최신 파일시스템처럼 자체적으로 블록 크기를 관리하는) 사용자를
 * 위한 함수다. 그런 사용자는 set_blocksize() 를 거치지 않고 직접 자신이
 * 원하는 블록 크기가 이 디바이스에서 유효한지 미리 확인해야 하는데, 그 확인
 * 로직을 여기 한 곳에 모아 중복을 없앤다.
 * 동작 과정: (1) blk_validate_block_size() 로 커널이 일반적으로 허용하는
 * 블록 크기 범위(2의 거듭제곱, 최소/최대 범위)인지 검사하고, (2) 그 값이 이
 * 디바이스의 실제 logical_block_size 보다 작지는 않은지 검사한다. NVMe
 * 네임스페이스라면 logical_block_size 보다 작은 블록 크기를 쓰는 파일시스템은
 * 하나의 논리 블록이 여러 LBA 에 걸쳐 있다고 착각하게 되어 PRP(Physical
 * Region Page)/SGL(Scatter-Gather List) 정렬이나 섹터 어드레싱이 깨질 수
 * 있으므로 반드시 거부해야 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 락 불필요(순수 값 비교).
 * 호출자: set_blocksize()(이 조각 밖, 추정: fs/buffer.c 또는 bdev.c 의 다른
 *         위치) 및 버퍼 헤드를 쓰지 않는 파일시스템이 자신의 블록 크기를 정할
 *         때 직접 호출(EXPORT_SYMBOL_GPL 이므로 GPL 호환 모듈만 호출 가능).
 * 피호출자: blk_validate_block_size(), bdev_logical_block_size().
 * 에러 경로: 두 검사 중 하나라도 실패하면 즉시 -EINVAL 을 반환하고 그 자리에서
 * 함수가 끝난다 - 별도의 정리(cleanup) 작업은 필요 없다(부작용이 없는 순수
 * 검증 함수이므로).
 *
 * 호출 체인:
 *   set_blocksize()/iomap 기반 파일시스템(이 조각 밖) → [bdev_validate_blocksize] → blk_validate_block_size()
 */

int bdev_validate_blocksize(struct block_device *bdev, int block_size)
{
	if (blk_validate_block_size(block_size))
// [한국어] 커널이 블록 계층 전반에서 허용하는 블록 크기 범위(2의 거듭제곱, 512바이트 ~ PAGE_SIZE 등)에 맞는지 일반 검증 - 이 디바이스 고유의 제약과는 별개인 공통 규칙을 먼저 확인한다.
		return -EINVAL;
// [한국어] 커널이 허용하는 일반 블록 크기 범위를 벗어난 경우의 에러 반환 - -EINVAL(Invalid argument)을 돌려주어 호출자가 이 block_size 설정을 포기하도록 한다.

	/* Size cannot be smaller than the size supported by the device */
	if (block_size < bdev_logical_block_size(bdev))
// [한국어] 요청한 block_size 가 이 디바이스의 logical_block_size 보다 작으면 안 된다 - NVMe LBA 보다 작은 단위로 접근을 시도하면 PRP/SGL 정렬이나 섹터 어드레싱 규칙을 위반할 수 있다.
		return -EINVAL;
// [한국어] 디바이스가 지원하는 최소 크기보다 작은 block_size 를 요청한 경우의 에러 반환 - 역시 -EINVAL 로 거부한다.

	return 0;
// [한국어] 두 검증을 모두 통과했다는 뜻 - 이 block_size 는 이 디바이스에서 안전하게 사용할 수 있음을 호출자에게 알린다.
}
EXPORT_SYMBOL_GPL(bdev_validate_blocksize);
// [한국어] 이 심볼을 GPL 호환 모듈에만 공개(EXPORT_SYMBOL_GPL) - EXPORT_SYMBOL 과 달리 독점(proprietary) 모듈에서는 이 함수를 직접 링크해 호출할 수 없다.

/*
 * [한국어]
 * set_blocksize - 파일이 속한 block_device 의 논리 블록 크기(i_blkbits)를
 *                 변경하고, 필요하면 페이지 캐시(folio)를 완전히 비우고 재구성한다.
 *
 * @file: 블록 디바이스 특수 파일(bdev special file)의 struct file. file->f_mapping->host
 *        가 곧 block_device 를 감싸는 bdev_inode 이며, 이 inode 의 i_blkbits 값이
 *        실제로 변경 대상이 된다. file->private_data 가 NULL 이면 (bdev_inode 를 통해
 *        열린 파일이 아니라는 뜻이므로) 잘못된 호출로 간주하고 에러를 반환한다.
 * @size: 새로 설정하려는 블록 크기(바이트 단위). 반드시 2의 거듭제곱이어야 하며
 *        bdev_validate_blocksize() 가 512바이트부터 PAGE_SIZE 사이 범위 및 디바이스의
 *        logical_block_size 이상인지 등을 먼저 검증한다.
 * @return: 성공 시 0. bdev_validate_blocksize() 가 거부하면 그 음수 에러코드
 *          (예: -EINVAL) 를 그대로 전달하고, file->private_data 가 없으면 -EINVAL.
 *          블록 크기가 이미 동일하면 아무 것도 하지 않고 0을 반환(no-op 최적화).
 *
 * 파일시스템(ext4, xfs 등)이 마운트 시점에 sb_set_blocksize() 를 거쳐 이 함수를
 * 호출하여, 슈퍼블록이 사용할 블록 크기와 실제 block_device 의 페이지 캐시
 * 매핑 단위(i_blkbits, folio min_order)를 일치시킨다. NVMe 관점에서는 파일시스템이
 * 만들어내는 bio 의 최소 단위가 여기서 정해지므로, 이후 blk_mq_submit_bio() 가
 * 생성하는 요청의 PRP(Physical Region Page)/SGL(Scatter Gather List) 정렬 단위와도
 * 직결된다.
 *
 * 동작 순서:
 *   1) bdev_validate_blocksize() 로 요청한 크기가 디바이스가 허용하는 범위인지 검증.
 *   2) file->private_data 확인으로 이 file 이 진짜 bdev 특수 파일인지 확인.
 *   3) 현재 i_blkbits 와 다를 때만(즉 실제 변경이 필요할 때만) 아래 무거운 작업 수행:
 *      - inode_lock() + filemap_invalidate_lock() 으로 해당 inode 에 대한 읽기/쓰기/
 *        fallocate 와의 동시 접근을 완전히 차단. folio 크기가 가변(mapping_set_
 *        folio_min_order)이 되었기 때문에, 락 없이 min_order 를 바꾸면 이미 작은
 *        folio 를 들고 있는 리더가 readahead 를 호출할 때 폴리오당 블록 수가 0이라고
 *        오판해 커널이 크래시할 수 있다 (원문 영어 주석 참조).
 *      - sync_blockdev() 로 dirty 데이터를 디스크(NVMe라면 Write/Flush 커맨드)에
 *        모두 내려보내고, kill_bdev() 로 페이지 캐시를 완전히 비워 stale 데이터가
 *        새 블록 크기 매핑과 섞이지 않게 한다.
 *      - i_blkbits 와 folio min_order 를 새 크기로 갱신.
 *      - 락 해제.
 * 실행 컨텍스트: 프로세스 컨텍스트(마운트 시스템 콜 또는 ioctl BLKBSZSET 경로).
 * 재진입은 inode_lock 으로 직렬화되므로 안전하다.
 * 호출자: sb_set_blocksize() (파일시스템 마운트 경로), 또는 블록 디바이스 ioctl 핸들러.
 * 피호출자: bdev_validate_blocksize(), inode_lock()/filemap_invalidate_lock(),
 *          sync_blockdev(), kill_bdev(), blksize_bits(), get_order(),
 *          mapping_set_folio_min_order().
 * 에러 경로: 검증 실패 시 즉시 반환하며 락을 잡지 않은 상태이므로 별도 롤백 불필요.
 *
 * 호출 체인:
 *   sb_set_blocksize() -> [set_blocksize] -> bdev_validate_blocksize()/sync_blockdev()/kill_bdev()
 */

int set_blocksize(struct file *file, int size)
{
	struct inode *inode = file->f_mapping->host;
	// [한국어] file 이 가리키는 bdev 특수 파일의 address_space 로부터 호스트 inode 를 얻는다.
	// 이 inode 는 실제 데이터 파일이 아니라 block_device 를 감싸는 bdev_inode 이며,
	// i_blkbits 필드가 이 블록 디바이스의 논리 블록 크기(2의 지수, 비트값)를 저장한다.
	struct block_device *bdev = I_BDEV(inode);
	// [한국어] bdev_inode 컨테이너로부터 실제 struct block_device 포인터를 꺼낸다
	// (container_of 매크로 기반). 이후 sync_blockdev()/kill_bdev() 등 bdev 단위
	// 캐시 조작에 사용된다.
	int ret;
	// [한국어] bdev_validate_blocksize() 의 반환값(0 또는 음수 에러코드)을 담을 임시 변수.

	ret = bdev_validate_blocksize(bdev, size);
// [한국어] 요청한 size 가 이 block_device 가 허용하는 블록 크기 범위(512바이트부터
// PAGE_SIZE 사이, 2의 거듭제곱)인지, 디바이스의 logical_block_size 이상인지를 검증한다.
// NVMe 라면 namespace 의 LBAF(Logical Block Address Format)가 정의하는 최소 논리
// 블록 크기보다 작은 값은 여기서 걸러진다.
	if (ret)
		// [한국어] 검증 실패(0이 아닌 에러코드) 시 이후 캐시 조작을 전혀 수행하지 않고
		// 즉시 반환 - 아직 아무 락도 잡지 않았고 아무 상태도 바꾸지 않았으므로 롤백이
		// 필요 없다.
		return ret;
		// [한국어] bdev_validate_blocksize() 가 반환한 음수 에러코드(예: -EINVAL)를
		// 호출자에게 그대로 전달.

	if (!file->private_data)
// [한국어] file->private_data 가 NULL 이라는 것은 이 file 이 blkdev_open() 을 거쳐
// 정상적으로 연 bdev 특수 파일이 아니라는 의미. 이런 파일에 대해 블록 크기를 바꾸면
// 이후 I/O 경로가 이 block_device 를 제대로 찾지 못해 오동작할 수 있다.
		return -EINVAL;
		// [한국어] 잘못된 인자 에러를 호출자에게 반환.

	/* Don't change the size if it is same as current */
	if (inode->i_blkbits != blksize_bits(size)) {
// [한국어] 현재 inode 의 i_blkbits(비트 단위 블록 크기)와 요청한 size 를 비트 단위로
// 환산한 값이 다를 때만 아래의 무거운 캐시 재구성 작업을 수행한다. 같으면 no-op
// 이므로 굳이 락을 잡고 캐시를 플러시할 필요가 없다 - 반복 호출 시 불필요한
// I/O 폭풍을 막아준다.
		/*
		 * Flush and truncate the pagecache before we reconfigure the
		 * mapping geometry because folio sizes are variable now.  If a
		 * reader has already allocated a folio whose size is smaller
		 * than the new min_order but invokes readahead after the new
		 * min_order becomes visible, readahead will think there are
		 * "zero" blocks per folio and crash.  Take the inode and
		 * invalidation locks to avoid racing with
		 * read/write/fallocate.
		 */
		// [한국어] (위 원문 영어 주석 번역) 매핑 지오메트리를 재구성하기 전에 페이지
		// 캐시를 반드시 flush 하고 비워야 한다. 이제 folio 크기가 가변이기 때문이다.
		// 만약 어떤 리더가 이미 새 min_order 보다 작은 folio 를 할당해 둔 상태에서,
		// 새 min_order 가 (아래에서) 반영된 뒤 readahead 를 호출하면, readahead 로직은
		// 폴리오 하나당 블록이 0개라고 계산하게 되어 크래시한다. 이 경쟁을 막기 위해
		// inode_lock 과 invalidate_lock 을 모두 잡아 read/write/fallocate 경로와의
		// 동시 접근을 차단한 상태에서만 아래 작업을 수행한다.
		inode_lock(inode);
		// [한국어] inode 에 대한 배타적 락(i_rwsem 쓰기 잠금) 획득. 이 inode 를 대상으로
		// 하는 다른 read()/write()/fallocate() 계열 시스템 콜과 완전히 직렬화되어,
		// 블록 크기 변경 도중 다른 스레드가 파일을 건드리지 못하게 막는다.
		filemap_invalidate_lock(inode->i_mapping);
		// [한국어] address_space 의 invalidate_lock 을 잡아, 페이지 캐시 무효화/재구성과
		// readahead 삽입 경로가 서로 경쟁하지 않도록 한다. inode_lock 만으로는 일부
		// readahead 경로(예: 페이지 폴트를 통한 non-write 경로)를 막지 못하므로
		// 이중으로 필요하다.

		sync_blockdev(bdev);
// [한국어] 이 bdev 의 매핑에 있는 모든 dirty 페이지를 동기적으로 writeback 하고
// 완료까지 대기한다 - NVMe 라면 결과적으로 Write 커맨드(및 필요 시 Flush)가
// 발행되어 캐시에 남아있던 변경사항이 비휘발성 NAND 에 반영된다. 아래에서
// min_order 를 바꾸기 전에 반드시 먼저 실행되어야 dirty 데이터 유실이 없다.
		kill_bdev(bdev);
// [한국어] sync 로 이미 클린해진 페이지 캐시의 모든 folio 를 truncate_inode_pages()
// 로 강제로 비운다 - 기존 블록 크기 기준으로 만들어진 folio 들을 완전히 제거해야,
// 이후 새 min_order 로 할당되는 folio 들과 섞이지 않는다.

		inode->i_blkbits = blksize_bits(size);
// [한국어] size(바이트)를 log2 비트값으로 변환(blksize_bits)하여 inode->i_blkbits 에
// 대입 - 이 값이 이후 bio 생성 시 섹터/블록 환산 기준이 되고, NVMe 로 보면
// 파일시스템 bio 단위와 namespace LBA 단위를 재정렬하는 지점이다.
		mapping_set_folio_min_order(inode->i_mapping, get_order(size));
// [한국어] get_order(size) 로 새 블록 크기에 대응하는 페이지 오더(2의 order 승 페이지)를
// 구하고, 이를 address_space 의 최소 folio order 로 설정 - 이후 페이지 캐시가
// 이 order 이상의 folio 만 할당하도록 강제해, 블록 크기와 folio 크기 불일치를
// 방지한다. 위에서 캐시를 이미 kill_bdev() 로 비웠으므로 안전하게 갱신 가능하다.
		filemap_invalidate_unlock(inode->i_mapping);
		// [한국어] invalidate_lock 해제 - 캐시 재구성이 끝났으므로 readahead/무효화 경로의
		// 진입을 다시 허용한다.
		inode_unlock(inode);
		// [한국어] inode_lock 해제 - 다른 read/write/fallocate 호출이 다시 진행될 수
		// 있게 한다.
	}
	return 0;
	// [한국어] 검증 통과 및 (필요했다면) 캐시 재구성까지 모두 성공했다는 뜻으로 0 반환.
}

EXPORT_SYMBOL(set_blocksize);
// [한국어] 이 함수를 커널의 다른 빌트인 코드(파일시스템 모듈 포함)에서 링크해 쓸 수
// 있도록 심볼 테이블에 노출한다 - GPL 여부와 무관하게 호출 가능한 일반 EXPORT_SYMBOL.

/*
 * [한국어]
 * sb_validate_large_blocksize - PAGE_SIZE 를 초과하는 큰 블록 크기(LBS, Large
 *                               Block Size)를 이 파일시스템과 커널 설정이
 *                               지원하는지 검증한다.
 *
 * @sb: 블록 크기를 설정하려는 슈퍼블록. sb->s_type->fs_flags 로 파일시스템 드라이버가
 *      큰 블록 크기를 지원하도록 만들어졌는지(FS_LBS 플래그) 확인한다.
 * @size: 요청된 블록 크기(바이트). 이 함수가 호출될 때는 이미 size 가 PAGE_SIZE 를
 *        초과하는 경우로 한정된다(호출자 sb_set_blocksize() 에서 그 조건일 때만 호출).
 * @return: 지원 가능하면 0. 파일시스템이 FS_LBS 를 선언하지 않았거나 커널이
 *          CONFIG_TRANSPARENT_HUGEPAGE 로 빌드되지 않았다면 -EINVAL 과 함께
 *          경고 로그를 남긴다.
 *
 * PAGE_SIZE 보다 큰 블록 크기를 쓰려면 페이지 캐시가 여러 페이지를 묶은 큰 folio
 * (Transparent Huge Page 메커니즘 활용)를 할당할 수 있어야 한다. 이 요구사항을
 * 만족하지 못하는 파일시스템/커널 설정에서 큰 블록 크기를 시도하면 이후 folio
 * 할당/무효화 로직이 가정을 위반해 오동작하므로, 미리 이 함수에서 걸러낸다.
 * 실행 컨텍스트: 프로세스 컨텍스트(마운트 경로), 별도 락 없이 정적 플래그만 검사.
 * 호출자: sb_set_blocksize().
 * 피호출자: pr_warn_ratelimited() (경고 로그, 초당 출력 횟수 제한됨).
 * 에러 경로: err_str 이 설정된 경우에만 경고를 출력하고 -EINVAL 반환; 그 외에는 0.
 *
 * 호출 체인:
 *   sb_set_blocksize() -> [sb_validate_large_blocksize] -> pr_warn_ratelimited()
 */

static int sb_validate_large_blocksize(struct super_block *sb, int size)
{
	const char *err_str = NULL;
	// [한국어] 에러 사유 문자열 포인터. NULL 이면 "문제 없음"을 의미하는 센티널로 쓰인다.

	if (!(sb->s_type->fs_flags & FS_LBS))
// [한국어] 파일시스템 타입의 fs_flags 비트마스크에 FS_LBS(Large Block Size 지원)
// 비트가 꺼져 있으면, 이 파일시스템 드라이버 자체가 PAGE_SIZE 초과 블록을 다룰
// 준비가 안 된 것이므로 에러 사유를 기록한다.
		err_str = "not supported by filesystem";
		// [한국어] 이후 pr_warn_ratelimited() 에서 그대로 출력될 사유 문자열.
	else if (!IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE))
// [한국어] 파일시스템은 FS_LBS 를 지원하더라도, 커널이 CONFIG_TRANSPARENT_HUGEPAGE 로
// 빌드되지 않았다면 여러 페이지를 묶은 큰 folio 를 할당할 수 없어 큰 블록 크기를
// 실제로 구현할 수 없다 - 컴파일 타임 상수(IS_ENABLED)로 판정.
		err_str = "is only supported with CONFIG_TRANSPARENT_HUGEPAGE";
		// [한국어] 커널 설정 부족을 알리는 사유 문자열.

	if (!err_str)
	// [한국어] 위 두 조건 중 어느 것도 걸리지 않았다면(err_str 이 여전히 NULL) 지원 가능.
		return 0;
		// [한국어] 정상 - 큰 블록 크기 사용 가능.

	pr_warn_ratelimited("%s: block size(%d) > page size(%lu) %s\n",
	// [한국어] 초당 출력 횟수가 제한된(ratelimited) 커널 경고 로그 - 관리자가 마운트
	// 실패 원인을 dmesg 에서 확인할 수 있도록 파일시스템 이름, 요청 크기, PAGE_SIZE,
	// 사유를 출력한다.
				sb->s_type->name, size, PAGE_SIZE, err_str);
			// [한국어] 파일시스템 이름 문자열, 요청 블록 크기, 현재 아키텍처의
			// PAGE_SIZE, 위에서 결정된 실패 사유 문자열을 각각 포맷 인자로 전달.
	return -EINVAL;
	// [한국어] 지원 불가 - 잘못된 인자 에러코드 반환. 호출자 sb_set_blocksize() 는 이 경우
	// 블록 크기 변경 자체를 포기하고 0(실패로 간주되는 특수 반환값)을 반환한다.
}

/*
 * [한국어]
 * sb_set_blocksize - 슈퍼블록의 블록 크기를 검증 후 확정하고, 연결된
 *                    block_device 의 페이지 캐시 매핑 단위까지 함께 갱신한다.
 *
 * @sb: 블록 크기를 설정할 슈퍼블록. sb->s_bdev_file 을 통해 실제 하부 block_device 의
 *      set_blocksize() 도 함께 호출되어, 슈퍼블록 값과 bdev 의 i_blkbits 가 항상
 *      일치하도록 보장한다.
 * @size: 요청 블록 크기(바이트, 2의 거듭제곱). PAGE_SIZE 를 넘는 값이면 먼저
 *        sb_validate_large_blocksize() 검증을 통과해야 한다.
 * @return: 성공 시 새로 설정된 블록 크기(sb->s_blocksize, 항상 0이 아닌 양수)를 그대로
 *          반환 - 즉 "0이 아니면 성공, 0이면 실패"로 해석하는 int-as-bool 관용구다.
 *          큰 블록 크기 검증 실패 또는 set_blocksize() 실패 시 0을 반환한다.
 *
 * ext4/xfs 등 파일시스템이 마운트 시 자신이 원하는 블록 크기를 이 함수를 통해
 * 슈퍼블록과 하부 block_device 양쪽에 동시에 반영하기 위해 사용한다. NVMe
 * namespace 위에 마운트되는 경우, 여기서 확정된 블록 크기가 곧 이 파일시스템이
 * 만들어내는 모든 bio 의 최소 단위가 되어 NVMe 커맨드의 LBA(Logical Block
 * Addressing) 단위와 정렬되어야 한다.
 *
 * 동작 순서:
 *   1) size 가 PAGE_SIZE 초과이면 sb_validate_large_blocksize() 로 FS_LBS/THP 지원
 *      여부 확인 - 실패 시 0 반환(호출자는 이를 실패로 해석해야 함).
 *   2) set_blocksize() 로 실제 하부 block_device 의 i_blkbits/folio 캐시를 재구성 -
 *      실패(음수 에러코드, 즉 조건식이 참) 시 0 반환.
 *   3) 여기까지 왔다면 size 는 이미 유효성이 확인된 것이므로 sb->s_blocksize 와
 *      sb->s_blocksize_bits 를 갱신하고 새 블록 크기를 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트(마운트 경로). set_blocksize() 내부에서 필요 시
 * inode_lock/filemap_invalidate_lock 을 잡으므로 락 관리는 그쪽에 위임된다.
 * 호출자: sb_min_blocksize(), 각 파일시스템의 fill_super()/마운트 경로.
 * 피호출자: sb_validate_large_blocksize(), set_blocksize(), blksize_bits().
 * 에러 경로: 실패 지점마다 0을 반환하고 이후 필드 갱신은 건너뛴다.
 *
 * 호출 체인:
 *   fill_super()/sb_min_blocksize() -> [sb_set_blocksize] -> set_blocksize()
 */

int sb_set_blocksize(struct super_block *sb, int size)
{
	if (size > PAGE_SIZE && sb_validate_large_blocksize(sb, size))
// [한국어] size 가 PAGE_SIZE 를 초과하면서(&&) sb_validate_large_blocksize() 가
// 0이 아닌 값(-EINVAL)을 반환했다면(즉 큰 블록 크기를 지원하지 못하면) 이 분기로
// 진입한다 - 짧은 회로 평가(short-circuit)로 size 가 PAGE_SIZE 이하이면 뒤의
// 검증 함수는 아예 호출되지 않는다.
		return 0;
		// [한국어] 큰 블록 크기를 지원하지 않으므로 설정을 포기하고 0(실패)을 반환한다.
	if (set_blocksize(sb->s_bdev_file, size))
// [한국어] set_blocksize() 는 성공 시 0, 실패 시 음수 에러코드를 반환하므로 이 if 는
// "0이 아니면(즉 에러코드가 나오면) 참"이 되어 실패 케이스에서 진입한다. 하부
// block_device 의 i_blkbits/folio 재구성이 실패했다는 뜻이다.
		return 0;
		// [한국어] bdev 블록 크기 변경 실패 - 슈퍼블록 필드는 건드리지 않고 0을 반환한다.
	/* If we get here, we know size is validated */
	sb->s_blocksize = size;
// [한국어] 위 두 검증을 모두 통과했으므로 슈퍼블록의 블록 크기 필드를 최종 확정한다.
// 이 값이 이후 파일시스템 계층이 만드는 bio/버퍼의 크기 단위로 쓰인다.
	sb->s_blocksize_bits = blksize_bits(size);
	// [한국어] size 를 비트 단위(log2)로 환산해 s_blocksize_bits 에 저장 - 오프셋을
	// 블록 인덱스로 변환할 때(시프트 연산) 자주 쓰이는 캐시된 값이다.
	return sb->s_blocksize;
	// [한국어] 방금 설정한(항상 0이 아닌 양수) 블록 크기를 그대로 반환 - 호출자에게는
	// "성공적으로 설정된 블록 크기" 자체가 성공 신호(0이면 실패)로 쓰인다.
}

EXPORT_SYMBOL(sb_set_blocksize);
// [한국어] 파일시스템 모듈들이 마운트 시 호출할 수 있도록 심볼을 외부에 공개한다.

/*
 * [한국어]
 * sb_min_blocksize - 파일시스템이 요청한 블록 크기를 디바이스의 논리 블록 크기
 *                    이상으로 보정한 뒤 sb_set_blocksize() 를 호출한다.
 *
 * @sb: 대상 슈퍼블록. sb->s_bdev 로부터 하부 block_device 의 논리 블록 크기를 얻는다.
 * @size: 파일시스템이 원래 원하는 블록 크기(바이트). 디바이스의 최소 단위보다
 *        작을 수 있으므로 이 함수가 보정한다.
 * @return: sb_set_blocksize() 의 반환값을 그대로 전달 - 0이면 실패, 그 외에는
 *          실제로 설정된(보정된) 블록 크기. __must_check 속성이 붙어 있어 호출자가
 *          반환값을 무시하면 컴파일 경고가 발생한다(설정 실패를 놓치지 않도록 강제).
 *
 * 일부 파일시스템은 자신의 기본 블록 크기(예: 1KiB)를 요청하지만, 그 값이 실제
 * 블록 디바이스(NVMe namespace 등)의 논리 블록 크기보다 작으면 그 디바이스에는
 * 그런 작은 단위로 I/O 를 낼 수 없다. 이 함수는 그런 경우 자동으로 디바이스가
 * 지원하는 최소 크기로 끌어올려 sb_set_blocksize() 를 호출해 준다.
 * 동작: bdev_logical_block_size() 로 하한을 구하고, size 가 그보다 작으면 하한값으로
 * 대체한 뒤 sb_set_blocksize() 에 위임한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(마운트 경로), 별도 락 없음(호출된 함수들이 필요한
 * 락을 각자 관리).
 * 호출자: 각 파일시스템의 마운트/fill_super() 경로 중 최소 블록 크기 보장이
 * 필요한 곳(예: 레거시 파일시스템).
 * 피호출자: bdev_logical_block_size(), sb_set_blocksize().
 * 에러 경로: sb_set_blocksize() 가 0을 반환하면 그대로 0 전달 - 호출자가 반드시
 * 확인해야 하므로 __must_check 로 강제된다.
 *
 * 호출 체인:
 *   fill_super() -> [sb_min_blocksize] -> sb_set_blocksize() -> set_blocksize()
 */

int __must_check sb_min_blocksize(struct super_block *sb, int size)
{
	int minsize = bdev_logical_block_size(sb->s_bdev);
// [한국어] 하부 block_device(NVMe 라면 namespace)가 지원하는 논리 블록 크기의
// 최솟값을 질의 - queue_limits 의 logical_block_size 를 바이트 단위로 반환하며,
// 이 값 미만으로는 이 디바이스에 정렬된 I/O 를 낼 수 없다.
	if (size < minsize)
// [한국어] 파일시스템이 요청한 size 가 디바이스 최소 단위보다 작다면 - 그대로 두면
// 이후 bio 가 디바이스 정렬 요구를 어겨 거부되거나(-EINVAL) NVMe 컨트롤러가
// PRP/SGL 정렬 오류를 낼 수 있으므로 강제로 끌어올린다.
		size = minsize;
		// [한국어] size 를 디바이스 최소 논리 블록 크기로 대체한다.
	return sb_set_blocksize(sb, size);
	// [한국어] (필요시 보정된) size 로 실제 블록 크기 설정을 위임하고 그 결과를 그대로
	// 반환한다.
}

EXPORT_SYMBOL(sb_min_blocksize);
// [한국어] 파일시스템 모듈에서 호출할 수 있도록 심볼을 공개한다.

/*
 * [한국어]
 * sync_blockdev_nowait - block_device 의 dirty 페이지 writeback 을 비동기로
 *                        "시작만" 시키고, 완료를 기다리지 않고 즉시 반환한다.
 *
 * @bdev: writeback 을 시작할 대상 block_device. NULL 이 전달될 수도 있는데(예:
 *        디바이스가 이미 제거된 뒤 정리 경로에서 호출되는 경우), 이 경우 안전하게
 *        아무 것도 하지 않는다.
 * @return: bdev 가 NULL 이면 0. 그 외에는 filemap_flush() 의 반환값 - writeback
 *          요청 자체를 큐에 넣는 과정에서 발생한 에러(음수) 또는 0(성공적으로 큐잉).
 *          이 반환값은 "쓰기가 끝났다"는 보장이 전혀 아니라 "쓰기 시작을 요청했다"
 *          는 뜻일 뿐이다.
 *
 * sync_blockdev() 과 달리 완료를 기다리지 않기 때문에, 호출자가 다른 작업을
 * 계속 진행하면서 백그라운드로 dirty 데이터를 밀어내고 싶을 때 사용한다(예:
 * 주기적 writeback, 메모리 회수 경로에서의 힌트성 flush).
 * 동작: bdev NULL 체크 후 filemap_flush() 로 위임 - 내부적으로 writeback_control 을
 * WB_SYNC_NONE 모드로 구성해 ->writepages() 를 호출하고 바로 리턴한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 블로킹 없이 빠르게 반환(디스크 I/O 완료를
 * 기다리지 않음). 재진입 시 각 writeback 요청은 페이지 캐시의 dirty 태그를 통해
 * 독립적으로 관리되므로 별도 락 불필요.
 * 호출자: 블록 계층의 비동기 flush 가 필요한 경로(예: 디바이스 제거 전 힌트성 flush).
 * 피호출자: filemap_flush() -> ... -> ->writepages() -> blk_mq_submit_bio() ->
 * (NVMe 라면) nvme_queue_rq().
 * 에러 경로: filemap_flush() 의 에러를 그대로 전달 - 별도 처리 없음(비동기 특성상
 * 실제 쓰기 실패는 이 반환값에 나타나지 않을 수 있음).
 *
 * 호출 체인:
 *   (블록 계층/디바이스 제거 경로) -> [sync_blockdev_nowait] -> filemap_flush() -> nvme_queue_rq()
 */

int sync_blockdev_nowait(struct block_device *bdev)
{
	if (!bdev)
// [한국어] bdev 포인터가 NULL 이면 (이미 해제되었거나 존재하지 않는 디바이스)
// 플러시할 대상 자체가 없으므로 안전하게 성공(0)으로 처리하고 종료한다.
		return 0;
	return filemap_flush(bdev->bd_mapping);
// [한국어] bdev 의 address_space(bd_mapping)에 대해 비동기 writeback 을 시작한다 -
// 내부적으로 WB_SYNC_NONE 모드의 writeback_control 로 ->writepages() 를 호출해
// dirty 페이지들을 bio 로 만들어 블록 계층에 제출하고, 완료를 기다리지 않고
// 바로 반환한다.
}
EXPORT_SYMBOL_GPL(sync_blockdev_nowait);
// [한국어] GPL 라이선스 모듈에서만 링크 가능하도록 제한하여 공개한다 - 블록 계층
// 내부 writeback 제어와 밀접하므로 독점 모듈의 오남용을 막기 위한 전형적인
// GPL 심볼 정책이다.

/*
 * [한국어]
 * sync_blockdev - block_device 매핑에 걸린 모든 dirty 데이터를 디스크에 쓰고,
 *                 그 writeback 이 끝날 때까지 동기적으로 대기한다.
 *
 * @bdev: 대상 block_device. NULL 이면 아무 것도 하지 않고 성공으로 처리한다.
 * @return: bdev 가 NULL 이면 0. 그 외에는 filemap_write_and_wait() 의 반환값 -
 *          모든 writeback 이 에러 없이 끝나면 0, writeback 도중 에러가 있었다면
 *          음수 에러코드(예: -EIO).
 *
 * sync_blockdev_nowait() 과 달리 "완료까지 대기"한다는 점이 핵심이다. fsync()/
 * fdatasync() 나 파일시스템 마운트 해제, 블록 크기 변경(set_blocksize()) 처럼
 * "이 시점 이후에는 캐시에 dirty 데이터가 전혀 없어야 한다"는 강한 보장이 필요한
 * 경로에서 사용된다. NVMe 관점에서는 이 호출 하나로 여러 개의 Write 커맨드가
 * 발행되고, 컨트롤러의 volatile write cache 에 남은 데이터까지 비휘발성 NAND 에
 * 반영되도록(필요 시 Flush 커맨드까지) 보장한다.
 * 동작: bdev NULL 체크 후 filemap_write_and_wait() 에 위임 - 내부적으로 dirty
 * 페이지들에 대해 ->writepages() 를 호출한 뒤, 아직 writeback 중(PG_writeback)인
 * 페이지들이 모두 끝날 때까지 대기한다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 이 함수 자체는 슈퍼블록 락(s_umount 등)을
 * 잡지 않으므로(원문 영어 주석 참고), 호출자가 필요하면 별도로 락을 잡아야 한다.
 * 완료까지 블로킹되므로 호출 스레드는 디스크 I/O 지연시간만큼 대기하게 된다.
 * 호출자: set_blocksize(), bdev_freeze() (holder ops 가 없을 때의 fallback),
 * fsync 계열 시스템 콜 경로.
 * 피호출자: filemap_write_and_wait() -> ->writepages() -> blk_mq_submit_bio() ->
 * (NVMe 라면) nvme_queue_rq() -> 완료 인터럽트 대기.
 * 에러 경로: writeback 중 에러가 있었다면 그 에러코드가 그대로 전달되며, 별도
 * 재시도 로직은 이 함수에 없다(호출자가 필요시 재시도).
 *
 * 호출 체인:
 *   set_blocksize()/bdev_freeze()/fsync() -> [sync_blockdev] -> filemap_write_and_wait()
 */

/*
 * Write out and wait upon all the dirty data associated with a block
 * device via its mapping.  Does not take the superblock lock.
 */
int sync_blockdev(struct block_device *bdev)
{
	if (!bdev)
// [한국어] bdev 가 NULL 이면 동기화할 매핑 자체가 없으므로 즉시 성공 처리한다.
		return 0;
	return filemap_write_and_wait(bdev->bd_mapping);
// [한국어] bd_mapping 의 모든 dirty 페이지에 대해 writeback 을 수행하고, 그 writeback
// 이 전부 끝날 때까지(디스크 I/O 완료 인터럽트까지) 블로킹 대기한 뒤 결과를
// 반환한다.
}
EXPORT_SYMBOL(sync_blockdev);
// [한국어] GPL 여부와 무관하게 모든 커널 모듈/코드에서 호출 가능하도록 심볼을 공개한다.

/*
 * [한국어]
 * sync_blockdev_range - block_device 매핑 중 [lstart, lend] 범위에 해당하는
 *                       dirty 데이터만 골라 쓰고 완료까지 대기한다.
 *
 * @bdev: 대상 block_device. sync_blockdev() 와 달리 여기서는 NULL 체크가 없으므로
 *        호출자가 유효한 bdev 를 보장해야 한다.
 * @lstart: 플러시할 범위의 시작 바이트 오프셋(포함).
 * @lend: 플러시할 범위의 끝 바이트 오프셋(포함, loff_t 이므로 파일 오프셋과 동일한
 *        부호 있는 64비트 값. -1 을 넘기면 파일 끝까지를 의미하는 관용구가 흔하다).
 * @return: filemap_write_and_wait_range() 의 반환값 - 해당 범위 writeback 이 모두
 *          성공하면 0, 에러가 있었다면 음수 에러코드.
 *
 * sync_blockdev() 이 매핑 전체를 대상으로 하는 반면, 이 함수는 특정 바이트 범위만
 * 골라 writeback 하고 싶을 때 사용한다(예: O_DIRECT 와 버퍼드 I/O 를 섞어 쓰는
 * 파일시스템이 특정 구간만 동기화해야 할 때). NVMe 관점에서는 range 단위 flush
 * 커맨드가 NVMe 표준 명령셋에 별도로 존재하지 않으므로(추정), 커널은 해당 범위에
 * 걸친 dirty 페이지들에 대해서만 일반 Write 커맨드를 발행하고 그 완료를 기다리는
 * 방식으로 range flush 를 흉내낸다.
 * 동작: filemap_write_and_wait_range() 한 줄로 위임 - 내부적으로 해당 범위의 dirty
 * 태그가 붙은 페이지만 순회하며 ->writepages() 를 호출한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 완료까지 블로킹.
 * 호출자: 범위 한정 동기화가 필요한 파일시스템/블록 계층 코드.
 * 피호출자: filemap_write_and_wait_range().
 * 에러 경로: 내부 함수의 에러코드를 그대로 전달한다.
 *
 * 호출 체인:
 *   (범위 한정 flush 필요 경로) -> [sync_blockdev_range] -> filemap_write_and_wait_range()
 */

int sync_blockdev_range(struct block_device *bdev, loff_t lstart, loff_t lend)
{
	return filemap_write_and_wait_range(bdev->bd_mapping,
		// [한국어] bdev 의 address_space 에서 [lstart, lend] 범위에 해당하는 dirty
		// 페이지만 골라 writeback 을 수행하고, 그 범위의 writeback 완료까지 대기한다.
		// NVMe 표준 명령셋에는 range 단위 flush 커맨드가 없으므로(추정), 이 범위에
		// 걸친 Write 커맨드들의 완료로 range flush 효과를 대신한다.
			lstart, lend);
			// [한국어] 플러시할 바이트 범위의 시작/끝 오프셋을 그대로 전달한다.
}
EXPORT_SYMBOL(sync_blockdev_range);
// [한국어] 다른 커널 코드에서 링크해 쓸 수 있도록 심볼을 공개한다.

/*
 * [한국어]
 * bdev_freeze - 이 block_device 위의 파일시스템을 동결(freeze)하여, 스냅샷을
 *               뜨거나 일관된 이미지를 확보해야 하는 동안 쓰기를 막고 dirty
 *               데이터를 모두 디스크에 반영한 일관 상태로 만든다.
 *
 * @bdev: 동결할 block_device. bd_fsfreeze_mutex/bd_fsfreeze_count/bd_holder_lock/
 *        bd_holder_ops 필드를 통해 중첩 freeze 카운트와 holder(예: LVM,
 *        device-mapper)의 커스텀 freeze 콜백을 관리한다.
 * @return: 성공 시 0(이미 freeze 되어 있어 카운트만 올리고 반환하는 경우도 0).
 *          holder 의 freeze 콜백 또는 sync_blockdev() 가 실패하면 그 음수
 *          에러코드를 반환하고, 이 경우 방금 올렸던 freeze 카운트는 원복한다.
 *
 * LVM 스냅샷 생성, 파일시스템 백업 도구, fsfreeze ioctl 등이 "이 순간 디스크
 * 위의 데이터가 절대 바뀌지 않는다"는 보장을 얻기 위해 사용한다. 여러 주체가
 * 동시에 freeze 를 요청할 수 있으므로(예: 중첩된 스냅샷 도구), bd_fsfreeze_count
 * 라는 참조 카운트로 "마지막으로 thaw 하는 호출만 실제로 동결을 해제"하도록
 * 관리한다 - 카운트는 이 함수에서 증가하고 bdev_thaw() 에서 감소한다.
 *
 * 동작 순서:
 *   1) bd_fsfreeze_mutex 로 카운터와 관련 상태를 잠근다(여러 스레드의 동시
 *      freeze/thaw 요청을 직렬화).
 *   2) bd_fsfreeze_count 를 원자적으로 증가시키고 결과가 1보다 크면(이미 누군가
 *      먼저 freeze 해 둔 상태) 추가 작업 없이 바로 성공 반환 - 실제 동결 작업은
 *      최초 1회만 수행하면 되기 때문이다.
 *   3) 처음 freeze 하는 경우라면 bd_holder_lock 으로 holder_ops 포인터를 보호하며
 *      확인 - holder(예: dm/LVM)가 자신만의 freeze 콜백을 등록해 두었다면 그것을
 *      호출해 holder 특화 동결 정책(예: dm-thin 메타데이터 동기화)을 수행시키고,
 *      없다면 범용 fallback 으로 sync_blockdev() 를 호출해 dirty 데이터를 모두
 *      디스크(NVMe라면 Write/Flush 커맨드)에 내려보낸다.
 *   4) 실패했다면 방금 올렸던 카운트를 되돌려(atomic_dec) 다음 시도가 다시 처음부터
 *      진행되도록 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 핸들러 등). bd_fsfreeze_mutex 로
 * freeze/thaw 시퀀스를 직렬화하고, bd_holder_lock 은 holder_ops 포인터 자체의
 * 교체(예: holder 등록/해제)와의 경쟁만 짧게 보호한다.
 * 호출자: fsfreeze ioctl 핸들러(FIFREEZE), LVM 스냅샷 생성 경로 등.
 * 피호출자: mutex_lock/unlock, atomic_inc_return/atomic_dec, bd_holder_ops->freeze(),
 * sync_blockdev(), lockdep_assert_not_held().
 * 에러 경로: holder 콜백 또는 sync_blockdev() 실패 시 카운트를 원복하고 에러
 * 코드를 그대로 호출자에게 반환한다.
 *
 * 호출 체인:
 *   fsfreeze ioctl -> [bdev_freeze] -> bd_holder_ops->freeze() 또는 sync_blockdev()
 */

/**
 * bdev_freeze - lock a filesystem and force it into a consistent state
 * @bdev:	blockdevice to lock
 *
 * If a superblock is found on this device, we take the s_umount semaphore
 * on it to make sure nobody unmounts until the snapshot creation is done.
 * The reference counter (bd_fsfreeze_count) guarantees that only the last
 * unfreeze process can unfreeze the frozen filesystem actually when multiple
 * freeze requests arrive simultaneously. It counts up in bdev_freeze() and
 * count down in bdev_thaw(). When it becomes 0, thaw_bdev() will unfreeze
 * actually.
 *
 * Return: On success zero is returned, negative error code on failure.
 */
int bdev_freeze(struct block_device *bdev)
{
	int error = 0;
	// [한국어] holder 콜백 또는 sync_blockdev() 의 결과를 담을 변수. 기본값 0(성공)으로
	// 초기화 - "이미 freeze 되어 있어 바로 반환"하는 경로에서는 이 초기값이 그대로
	// 쓰인다.

	mutex_lock(&bdev->bd_fsfreeze_mutex);
// [한국어] freeze/thaw 시퀀스 전체를 직렬화하는 뮤텍스 획득 - 여러 스레드가 동시에
// bdev_freeze()/bdev_thaw() 를 호출해도 bd_fsfreeze_count 증감과 holder 콜백
// 호출이 원자적인 하나의 트랜잭션처럼 보이도록 보장한다. 잡고 있는 동안
// sync_blockdev() 처럼 블로킹될 수 있는 호출도 있으므로 뮤텍스(슬립 가능 락)를
// 사용한다.

	if (atomic_inc_return(&bdev->bd_fsfreeze_count) > 1) {
// [한국어] 카운트를 원자적으로 1 증가시키고 증가된 후의 값을 받는다
// (atomic_inc_return). 결과가 1보다 크다는 것은 이미 최소 한 번 이상 freeze
// 되어 있었다는 뜻이므로, 실제 동결 작업(holder 콜백/sync_blockdev)은 중복
// 수행할 필요가 없다.
		mutex_unlock(&bdev->bd_fsfreeze_mutex);
		// [한국어] 추가 작업 없이 바로 반환하기 전에 뮤텍스를 반드시 풀어줘야 다른
		// 스레드의 freeze/thaw 요청이 진행될 수 있다.
		return 0;
// [한국어] 이미 동결된 상태를 그대로 유지하며 카운트만 늘린 채 성공 반환한다.
	}

	mutex_lock(&bdev->bd_holder_lock);
// [한국어] bd_holder_ops 포인터 자체를 읽는 동안 holder 등록/해제와 경쟁하지 않도록
// 짧게 보호하는 락 - bd_fsfreeze_mutex 와는 별개의 더 세분화된 락이다.
	if (bdev->bd_holder_ops && bdev->bd_holder_ops->freeze) {
		// [한국어] 이 block_device 에 holder(예: device-mapper, LVM)가 등록되어 있고,
		// 그 holder 가 자신만의 freeze 콜백을 제공하는 경우에만 진입 - holder 가
		// 있으면 범용 sync_blockdev() 대신 holder 특화 동결 정책을 우선시킨다.
		error = bdev->bd_holder_ops->freeze(bdev);
// [한국어] holder 가 제공한 freeze 콜백 호출 - 내부적으로 파일시스템 s_umount
// 세마포어를 잡거나 자체 캐시 동기화(sync_blockdev 포함)를 수행할 수 있다.
		lockdep_assert_not_held(&bdev->bd_holder_lock);
		// [한국어] 런타임 락 검증 도구(lockdep)에게 "이 시점에는 bd_holder_lock 을
		// 들고 있으면 안 된다"고 단언한다 - holder 콜백이 규약대로 자체적으로
		// bd_holder_lock 을 풀고 나왔는지 개발 빌드에서 검증하기 위함이다.
	} else {
		mutex_unlock(&bdev->bd_holder_lock);
		// [한국어] holder 콜백이 없는 경우이므로, holder_ops 포인터를 다 확인했으니
		// bd_holder_lock 을 여기서 직접 풀어준다(위 if 분기와 달리 콜백에게 락 해제를
		// 위임할 수 없으므로 이 경로에서 명시적으로 unlock 한다).
		error = sync_blockdev(bdev);
// [한국어] holder 특화 정책이 없는 범용 block_device 라면, 기본 동작으로
// sync_blockdev() 를 호출해 dirty 데이터를 모두 디스크에 쓰고 완료까지 대기한다 -
// 이렇게 해야 스냅샷 생성 시점의 블록 이미지가 일관된 상태가 된다.
	}

	if (error)
	// [한국어] holder 콜백 또는 sync_blockdev() 가 실패(음수 반환)했다면 - freeze 가
	// 완전히 성립하지 못했으므로 카운트 증가를 되돌려야 한다.
		atomic_dec(&bdev->bd_fsfreeze_count);
// [한국어] 위에서 미리 올려두었던 카운트를 다시 감소시켜, 실패한 freeze 시도가
// 카운트에 흔적을 남기지 않도록 한다 - 그래야 다음 재시도가 "최초 freeze"로
// 다시 인식되어 holder 콜백/sync_blockdev() 를 다시 시도할 수 있다.

	mutex_unlock(&bdev->bd_fsfreeze_mutex);
	// [한국어] 함수 진입 시 잡았던 시퀀스 보호 뮤텍스를 해제한다 - 성공/실패와
	// 무관하게 항상 이 지점에서 풀어준다.
	return error;
	// [한국어] holder 콜백/sync_blockdev() 의 결과(0=성공, 음수=실패)를 그대로
	// 호출자에게 전달한다.
}
EXPORT_SYMBOL(bdev_freeze);
// [한국어] fsfreeze ioctl 핸들러 등 다른 커널 코드에서 호출할 수 있도록 심볼을
// 공개한다.

/*
 * [한국어]
 * bdev_thaw - bdev_freeze() 로 걸어 둔 동결을 중첩 카운트가 0이 될 때 실제로
 *             해제하여 파일시스템을 다시 쓰기 가능한 상태로 되돌린다.
 *
 * @bdev: 동결을 해제할 block_device. bdev_freeze() 와 동일한 bd_fsfreeze_mutex/
 *        bd_fsfreeze_count/bd_holder_lock/bd_holder_ops 상태를 공유한다.
 * @return: 성공 시 0. bd_fsfreeze_count 가 이미 0이었다면(짝이 맞지 않는 thaw
 *          호출) -EINVAL. holder 의 thaw 콜백이 실패하면 그 음수 에러코드를
 *          반환하고, 이 경우 freeze 카운트를 다시 올려 상태를 복구한다.
 *
 * bdev_freeze() 가 중첩 호출을 허용하므로(bd_fsfreeze_count 참조 카운트), thaw
 * 도 대칭적으로 "마지막 thaw 호출일 때만" 실제 동결 해제 작업(holder 콜백 호출)을
 * 수행해야 한다. 이 함수는 그 대칭을 구현한다.
 *
 * 동작 순서:
 *   1) bd_fsfreeze_mutex 로 시퀀스를 잠근다.
 *   2) atomic_dec_if_positive() 로 카운트를 감소시키되, 이미 0 이하였다면 감소를
 *      수행하지 않고 음수를 반환하는 특수 원자 연산을 사용한다 - 이렇게 해야
 *      "짝이 맞지 않는 thaw"(freeze 하지 않은 상태에서 thaw 호출)를 카운트를
 *      음수로 만들지 않고 안전하게 감지할 수 있다. 결과가 음수면 -EINVAL 로
 *      바로 반환.
 *   3) 감소 후 값이 0보다 크면(아직 다른 freeze 가 남아있으면) 실제 thaw 콜백은
 *      건너뛰고 카운트만 줄인 채 성공 반환.
 *   4) 감소 후 값이 정확히 0이 되었을 때만 진짜 마지막 thaw 이므로, holder 의
 *      thaw 콜백을 호출하고(holder 가 없으면 아무 것도 하지 않음), 실패 시
 *      카운트를 다시 올려(atomic_inc) 상태를 복구한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 핸들러 등), bd_fsfreeze_mutex 로
 * freeze/thaw 시퀀스 전체가 직렬화된다.
 * 호출자: fsfreeze ioctl 의 해제(FITHAW) 핸들러, 스냅샷 생성 완료 후 정리 경로.
 * 피호출자: mutex_lock/unlock, atomic_dec_if_positive/atomic_inc,
 * bd_holder_ops->thaw(), lockdep_assert_not_held().
 * 에러 경로: 카운트 불일치(-EINVAL) 또는 holder 콜백 실패 시 그 에러코드를
 * 반환하며, 후자의 경우 카운트를 원복해 다음 thaw 재시도가 가능하게 한다.
 *
 * 호출 체인:
 *   fsfreeze ioctl(해제) -> [bdev_thaw] -> bd_holder_ops->thaw()
 */

/**
 * bdev_thaw - unlock filesystem
 * @bdev:	blockdevice to unlock
 *
 * Unlocks the filesystem and marks it writeable again after bdev_freeze().
 *
 * Return: On success zero is returned, negative error code on failure.
 */
int bdev_thaw(struct block_device *bdev)
{
	int error = -EINVAL, nr_freeze;
	// [한국어] error 는 기본적으로 -EINVAL 로 초기화한다 - "카운트가 이미 0이라 감소를
	// 수행하지 못한" 비정상 케이스를 별도 처리 없이 기본값 그대로 반환하기 위함이다.
	// nr_freeze 는 atomic_dec_if_positive() 가 반환하는 "감소 후 값"을 담을 변수.

	mutex_lock(&bdev->bd_fsfreeze_mutex);
// [한국어] bdev_freeze() 와 동일한 뮤텍스를 잡아 freeze/thaw 시퀀스를 직렬화한다 -
// 카운트 확인과 holder 콜백 호출이 하나의 원자적 트랜잭션처럼 보이게 한다.

	/*
	 * If this returns < 0 it means that @bd_fsfreeze_count was
	 * already 0 and no decrement was performed.
	 */
	nr_freeze = atomic_dec_if_positive(&bdev->bd_fsfreeze_count);
// [한국어] "현재 값이 양수일 때만 감소시키고, 감소 후 값을 반환한다. 이미 0 이하
// 였다면 감소하지 않고 음수를 반환한다"는 원자 연산이다 - 일반 atomic_dec 을
// 쓰면 카운트가 음수로 내려가 다음 freeze 판정(> 1 비교)이 깨지므로 이 전용
// 연산을 사용한다.
	if (nr_freeze < 0)
// [한국어] 반환값이 음수라는 것은 감소 자체가 일어나지 않았다는 뜻, 즉 freeze
// 되어 있지 않은 상태에서 thaw 를 호출한 짝 안 맞는 호출이므로 에러로 처리한다.
		goto out;
		// [한국어] error 는 이미 -EINVAL 로 초기화되어 있으므로 그대로 두고 뮤텍스 해제
		// 구간으로 점프한다.

	error = 0;
	// [한국어] 카운트 감소가 정상적으로 일어났으므로(짝이 맞는 호출) 일단 성공으로
	// 간주한다.
	if (nr_freeze > 0)
// [한국어] 감소 후에도 카운트가 여전히 0보다 크다면 아직 다른 freeze 요청이
// 남아있다는 뜻이므로, 실제 파일시스템 잠금 해제(holder 콜백)는 아직 수행하지
// 않고 카운트만 줄인 채 종료한다 - 마지막 thaw 가 아니면 동결 상태를 유지해야
// 한다.
		goto out;
		// [한국어] error=0(성공) 상태로 바로 뮤텍스 해제 구간으로 점프한다.

	mutex_lock(&bdev->bd_holder_lock);
	// [한국어] 카운트가 정확히 0이 된, 즉 마지막 thaw 인 경우에만 여기 도달한다 -
	// holder_ops 포인터를 안전하게 읽기 위해 짧게 락을 잡는다.
	if (bdev->bd_holder_ops && bdev->bd_holder_ops->thaw) {
// [한국어] holder(예: device-mapper/LVM)가 자신만의 thaw 콜백을 등록해 두었다면
// 그것을 호출해 holder 특화 잠금 해제 정책을 수행시킨다.
		error = bdev->bd_holder_ops->thaw(bdev);
		// [한국어] holder 의 thaw 콜백 호출 결과(0=성공, 음수=실패)를 error 에 저장한다.
		lockdep_assert_not_held(&bdev->bd_holder_lock);
		// [한국어] 콜백이 규약대로 bd_holder_lock 을 자체적으로 풀고 반환했는지 lockdep
		// 으로 검증한다(개발/디버그 빌드에서 락 오용을 조기에 잡아내기 위함).
	} else {
		mutex_unlock(&bdev->bd_holder_lock);
		// [한국어] holder 의 thaw 콜백이 없는 일반적인 경우 - freeze 때와 달리 별도의
		// "범용 thaw 동작"은 필요 없으므로(캐시를 다시 채우는 동작은 자연스럽게
		// 이후 read 경로에서 일어남), 그냥 잡았던 락만 풀어준다.
	}

	if (error)
	// [한국어] holder 의 thaw 콜백이 실패했다면 - 아직 완전히 풀리지 않은 상태이므로
	// 카운트를 다시 freeze 상태로 되돌려야 한다.
		atomic_inc(&bdev->bd_fsfreeze_count);
// [한국어] 방금 0으로 만들었던 카운트를 다시 1로 되돌려, 다음 thaw 재시도가
// "마지막 thaw"로 다시 인식되어 holder 콜백을 재시도할 수 있게 한다.
out:
	mutex_unlock(&bdev->bd_fsfreeze_mutex);
	// [한국어] 정상/조기 반환 경로 모두 이 레이블로 모여 뮤텍스를 반드시 해제한다.
	return error;
	// [한국어] 카운트 불일치(-EINVAL), holder 콜백 실패(음수), 또는 성공(0)을
	// 그대로 반환한다.
}
EXPORT_SYMBOL(bdev_thaw);
// [한국어] fsfreeze 해제 ioctl 핸들러 등에서 호출할 수 있도록 심볼을 공개한다.

/*
 * pseudo-fs - bdev 전용 익명 슈퍼블록/마운트.
 *
 * /dev/nvme0n1 과 같은 블록 장치 노드는 실제 파일시스템이 아닌 bdevfs 에
 * 속한다. 여기서 inode 가 할당되고, bdev->bd_mapping 이 설정되어
 * NVMe IO 의 페이지 캐시 기반이 마련된다.
 */
/*
 * [한국어] bdev 전용 pseudo-fs(가상 파일시스템) 아키텍처 개요.
 *
 * 리눅스는 "블록 장치 자체"를 표현하기 위해 VFS(가상 파일시스템)의
 * inode 캐시/페이지 캐시 인프라를 재사용한다. 예를 들어 /dev/nvme0n1 같은
 * 블록 장치 노드는 사용자가 마운트하는 실제 파일시스템(ext4, xfs 등)에
 * 속한 inode 가 아니라, 커널 내부에만 존재하는 전용 슈퍼블록(bdevfs,
 * 매직넘버 BDEVFS_MAGIC)에 속한 inode(struct bdev_inode)로 표현된다.
 *
 * 별도의 pseudo-fs 를 두는 이유:
 *   1) VFS 의 struct inode 가 이미 갖고 있는 struct address_space(페이지
 *      캐시), i_mapping, dirty 추적, writeback 인프라를 NVMe/SATA 등
 *      "raw" 블록 장치의 캐시 계층으로 그대로 재사용하기 위함.
 *   2) inode 해시(insert_inode_hash/remove_inode_hash)를 이용해 같은
 *      dev_t 로 여러 open() 요청이 들어와도 항상 같은 block_device 를
 *      찾아올 수 있도록 하기 위함(캐시 일관성 보장).
 *   3) 실제 파일시스템 마운트 네임스페이스와 완전히 분리되어 사용자
 *      공간에서는 이 pseudo-fs 를 직접 mount()로 노출하지 않는다
 *      (kern_mount() 로만 커널 내부에서 마운트됨).
 *
 * 이 chunk(D)는 bdevfs 슈퍼블록 연산(super_operations), 파일시스템 타입
 * 등록, 그리고 struct block_device 자체의 할당(bdev_alloc)/크기 설정
 * (bdev_set_nr_sectors)/해시 등록(bdev_add)/해제(bdev_drop) 경로를 다룬다.
 * bdev->bd_mapping 이 여기서 이 pseudo-fs inode 의 i_data 로 연결되어야
 * 비로소 상위 파일시스템/블록 I/O 계층이 페이지 캐시를 사용할 수 있다.
 */

/*
 * pseudo-fs
 */

/*
 * bdev_lock - bdev claim 전역 뮤텍스. NVMe 디바이스의 exclusive open
 *            (예: mkfs, LVM) 시 경쟁을 방지.
 * bdev_cachep - bdev_inode 객체를 위한 kmem_cache. NVMe namespace 마다
 *               하나의 bdev_inode 가 생성/재활용된다.
 */
/*
 * [한국어] 아래 두 전역 자원은 bdevfs 전체에서 공유되는 핵심 상태다.
 *  - bdev_lock: block_device 의 "claim"(배타적 open)을 직렬화하는 뮤텍스.
 *  - bdev_cachep: struct bdev_inode 전용 kmem_cache(슬래브 캐시).
 * 두 값 모두 부팅 시 bdev_cache_init() 에서 한 번 설정된 뒤 커널 종료까지
 * 바뀌지 않는 "부팅 시 1회 초기화, 이후 읽기 전용" 패턴을 따른다.
 */

static  __cacheline_aligned_in_smp DEFINE_MUTEX(bdev_lock);
/* [한국어] bdev claim(배타적 열기) 전용 전역 뮤텍스.
 * 역할: bd_prepare_to_claim() 등에서 한 block_device 를 특정 holder 가
 *       배타적으로 소유(O_EXCL open, 예: mkfs, LVM, mdadm 등)하도록 할 때,
 *       여러 CPU 가 동시에 같은 bdev 를 claim 하려는 경쟁을 막기 위해
 *       사용한다.
 * 설정자: DEFINE_MUTEX 매크로가 컴파일 타임에 mutex_init 과 동일한 효과로
 *         초기화(unlocked 상태)한다. 런타임에는 이 값 자체를 재설정하지
 *         않고 mutex_lock()/mutex_unlock() 으로 상태(잠김/풀림)만 바뀐다.
 * 읽는 자: bdev claim/release 경로(이 chunk 밖의 __blkdev_get/
 *         bd_may_claim 등)에서 이 뮤텍스를 획득해 bd_holder/bd_holders
 *         필드를 읽고 쓴다.
 * 값 범위: 잠금/풀림 두 상태만 가지는 뮤텍스. __cacheline_aligned_in_smp
 *         속성으로 다른 자주 쓰이는 전역 변수와 캐시라인을 공유하지 않게
 *         정렬되어, SMP 환경에서 false sharing 으로 인한 성능 저하를 막는다.
 * 동기화: 이 변수 자체가 동기화 primitive 이다. 전역적으로 단 하나만
 *         존재하므로, 모든 CPU/모든 block_device 의 claim 시도가 이
 *         뮤텍스 하나를 통해 직렬화된다(세밀한 per-bdev 락이 아니라
 *         전역 락). */
static struct kmem_cache *bdev_cachep __ro_after_init;
/* [한국어] struct bdev_inode 전용 슬래브 캐시(kmem_cache) 포인터.
 * 역할: bdev_alloc_inode()/bdev_free_inode() 가 각각 이 캐시에서
 *       bdev_inode 객체를 할당(alloc_inode_sb)/반환(kmem_cache_free)하는
 *       데 사용. VFS 의 범용 inode 캐시 대신 전용 캐시를 쓰는 이유는
 *       struct bdev_inode 가 struct inode 보다 크고(내부에 struct
 *       block_device 를 통째로 포함), 재사용 시 init_once() 로 특수
 *       초기화가 필요하기 때문이다.
 * 설정자: 부팅 시 bdev_cache_init() 의 kmem_cache_create() 호출 결과가
 *         단 한 번 대입된다. __ro_after_init 속성 때문에 커널은 초기화
 *         구간(init) 종료 후 이 변수가 있는 페이지를 읽기 전용으로
 *         재매핑하여, 이후 어떤 코드(버그나 공격)도 이 포인터를 변조하지
 *         못하게 막는다.
 * 읽는 자: bdev_alloc_inode()/bdev_free_inode() 가 매 inode 할당/해제마다
 *         읽어서 kmem_cache 계열 API 의 첫 인자로 사용.
 * 값 범위: 부팅 초기에는 NULL, kmem_cache_create() 성공 후에는 유효한
 *         kmem_cache 포인터(NULL 이면 SLAB_PANIC 플래그로 인해 애초에
 *         부팅이 panic 으로 중단되므로 런타임에 NULL 을 볼 수 없다).
 * 동기화: __ro_after_init 이후에는 불변이므로 별도 락 불필요. 초기화
 *         자체는 단일 부팅 스레드에서 한 번만 실행되어 경쟁이 없다. */

/*
 * bdev_alloc_inode - bdevfs 슈퍼블록에 새로운 bdev_inode 를 할당.
 * NVMe 연결점: NVMe 컨트롤러가 namespace 를 검색하면 여기에 대응하는
 *            bdev_inode 가 생성되고, 이후 nvme_queue 와 연결된다.
 */
/*
 * [한국어]
 * bdev_alloc_inode - bdevfs 전용 슈퍼블록에 새 bdev_inode 를 할당하는
 *                    super_operations->alloc_inode 콜백.
 *
 * @sb: 이 inode 가 속할 슈퍼블록. bdev_alloc_inode 는 오직 bdevfs 전용
 *      슈퍼블록(blockdev_mnt->mnt_sb, 즉 blockdev_superblock)에 대해서만
 *      호출되도록 bdev_sops.alloc_inode 로 등록되어 있다.
 * @return: 성공 시 새로 할당된 struct bdev_inode 의 vfs_inode 필드(즉
 *          struct inode *) 포인터. 슬래브 할당 실패 또는 LSM(Linux
 *          Security Module) 보안 초기화 실패 시 NULL. 호출자인 VFS 의
 *          alloc_inode()/new_inode() 는 NULL 을 받으면 그대로 -ENOMEM 류
 *          에러로 상위에 전파한다.
 *
 * VFS 는 inode 를 새로 만들어야 할 때(new_inode(), iget_locked() 등) 항상
 * 슈퍼블록의 s_op->alloc_inode 콜백을 호출해 실제 할당을 위임한다.
 * bdevfs 는 일반적인 struct inode 크기가 아니라, 그 안에 통째로 struct
 * block_device 를 내장한 struct bdev_inode 를 써야 하므로, 범용
 * alloc_inode_sb() 대신 전용 콜백을 등록해 bdev_cachep 슬래브에서 할당한다.
 *
 * 동작 순서:
 *  1) alloc_inode_sb() 로 bdev_cachep 슬래브에서 메모리를 받는다(내부적으로
 *     kmem_cache_alloc + 최소한의 통계/cgroup 계정 처리를 겸한다).
 *  2) 실패하면 즉시 NULL 반환.
 *  3) 새로 받은 메모리 중 struct block_device 부분(ei->bdev)만 memset 으로
 *     0 클리어한다. vfs_inode 부분은 슬래브의 constructor(init_once)가
 *     이미 초기화했으므로 다시 지우지 않는다(재활용 객체이기 때문에 매번
 *     생성자를 돌리지 않고 알맹이만 초기화하는 최적화).
 *  4) LSM 보안 레이블을 security_bdev_alloc() 으로 붙인다. SELinux/AppArmor
 *     등이 이 객체에 보안 컨텍스트를 부착하는 지점.
 *  5) 실패하면 슬래브를 즉시 kmem_cache_free() 로 반환하고 NULL.
 *  6) 성공하면 struct inode 포인터(&ei->vfs_inode)를 반환.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. new_inode()/iget_locked() 호출 경로를
 * 통해 open(2) 시스템 호출 처리 중이거나, bdev_alloc() 이 새 block_device
 * 를 만들 때 호출된다. 재진입 가능(각 호출이 독립된 슬래브 객체를 다룸).
 * 별도의 락을 들지 않고 호출되며, 이 함수 자체도 전역 락을 잡지 않는다
 * (kmem_cache_alloc 내부의 슬래브 락은 slab allocator 가 알아서 처리).
 *
 * caller: VFS 의 alloc_inode()(fs/inode.c) -> new_inode()/iget_locked() 등이
 *         호출하며, 이 chunk 안에서는 bdev_alloc() 이 new_inode() 를 통해
 *         간접적으로 이 콜백을 트리거한다.
 * callee: alloc_inode_sb(), memset(), security_bdev_alloc(),
 *         kmem_cache_free().
 * 에러 경로: 슬래브 부족(ei == NULL) 또는 LSM 거부
 *          (security_bdev_alloc() != 0) 시 NULL 반환 -> 상위 new_inode() 가
 *          이를 그대로 전파해 결국 open() 이 -ENOMEM 등으로 실패한다.
 *
 * 호출 체인:
 *   new_inode()/iget_locked() (fs/inode.c) → [bdev_alloc_inode] →
 *   alloc_inode_sb() / security_bdev_alloc()
 */

static struct inode *bdev_alloc_inode(struct super_block *sb)
{
	struct bdev_inode *ei = alloc_inode_sb(sb, bdev_cachep, GFP_KERNEL);
	/* [한국어] bdev_cachep 슬래브에서 bdev_inode 객체 1개를 할당.
	 * GFP_KERNEL: 프로세스 컨텍스트에서 필요하면 잠들어(sleep) 페이지
	 * 회수/재활용까지 시도하는 일반 할당 플래그(인터럽트 컨텍스트에서는
	 * 사용 불가). alloc_inode_sb() 는 내부적으로 kmem_cache_alloc_lru()
	 * 등을 호출하며 cgroup 메모리 계정까지 처리한다. */
// NVMe: bdevfs 슈퍼블록으로부터 새 bdev_inode 할당.

	if (!ei)
		/* [한국어] 슬래브 할당 실패(메모리 부족) 시 더 진행하지 않고
		 * 즉시 NULL 을 반환해 VFS 에 알린다. */
		return NULL;
	memset(&ei->bdev, 0, sizeof(ei->bdev));
	/* [한국어] struct bdev_inode 중 struct block_device 부분(ei->bdev)만
	 * 0 으로 초기화. 슬래브 재활용 객체이므로 vfs_inode 부분은 init_once()
	 * 생성자가 이미 처리했고, 여기서는 새로 채워야 할 block_device 쪽
	 * 필드(bd_queue, bd_disk, bd_openers 등)만 깨끗한 상태로 만든다. */
// NVMe: block_device 필드(bd_queue, bd_disk 등)를 0으로 초기화.

	if (security_bdev_alloc(&ei->bdev)) {
		/* [한국어] LSM(SELinux/AppArmor 등) 훅. 이 block_device 에
		 * 보안 레이블/컨텍스트를 부여할 수 있는지 검사·초기화한다.
		 * 0이 아닌 값(실패)을 반환하면 보안 정책 위반 또는 메모리
		 * 부족으로 보고 이 inode 를 폐기해야 한다. */
// NVMe: LSM 보안 레이블 할당. 보안 정책에 따라 NVMe 접근 제어.
		kmem_cache_free(bdev_cachep, ei);
		/* [한국어] 보안 초기화 실패로 이 객체를 쓸 수 없으므로, 방금
		 * 할당받은 슬래브 메모리를 즉시 캐시에 반환해 누수를 막는다. */
// NVMe: 보안 초기화 실패 시 슬래브를 즉시 반환.
		return NULL;
		/* [한국어] 상위 VFS 호출자에게 할당 실패를 알린다. */
	}
	return &ei->vfs_inode;
	/* [한국어] 성공 경로: bdev_inode 내부에 임베드된 struct inode 의
	 * 주소를 반환. container_of 관계상 이 포인터로부터 다시
	 * BDEV_I()/I_BDEV() 매크로를 통해 bdev_inode/block_device 를 역산할
	 * 수 있다. */
}

/*
 * bdev_free_inode - bdev_inode 와 남은 자원을 해제.
 *
 * @bd_stats: 디스크 I/O 통계(per-cpu). NVMe 성능 모니터링의 원천 데이터.
 * @bd_meta_info: 파티션 메타데이터.
 * @bd_disk: gendisk, NVMe namespace 의 gendisk 가 여기 연결됨.
 */
/*
 * [한국어]
 * bdev_free_inode - bdev_inode 에 딸린 부가 자원을 정리하는
 *                   super_operations->free_inode 콜백.
 *
 * @inode: 참조 카운트가 0이 되어 해제 중인 VFS inode. I_BDEV(inode) 로
 *         이 inode 를 내장하고 있는 struct block_device 를 얻는다.
 * @return: void. 이 함수는 실패할 수 없다(자원 해제만 수행).
 *
 * VFS 의 evict_inode 경로에서 inode 가 완전히 해제될 때 마지막 단계로
 * s_op->free_inode 가 호출된다(RCU 유예 이후, destroy_inode 경유). bdev
 * 는 slab 재사용을 위해 실제 슬래브 반환 전에 block_device 가 들고 있던
 * 부가 자원(percpu 통계, 파티션 메타데이터, LSM 레이블, gendisk, bdi,
 * 확장 minor 번호)을 모두 되돌려줘야 한다.
 *
 * 동작 순서:
 *  1) I_BDEV() 로 이 inode 에 대응하는 block_device 를 얻는다.
 *  2) bd_stats(percpu 디스크 I/O 통계)를 free_percpu() 로 해제.
 *  3) bd_meta_info(파티션 UUID/볼륨 이름 등 메타데이터)를 kfree().
 *  4) security_bdev_free() 로 LSM 보안 레이블 해제.
 *  5) 파티션이 아닌 "전체 디스크" bdev 라면(gendisk 의 소유자이므로)
 *     bdi(backing_dev_info) 참조 반환과 gendisk 자체의 메모리 해제까지
 *     책임진다. 파티션 bdev 는 gendisk 를 소유하지 않고 전체 디스크의
 *     gendisk 를 공유 참조만 하므로 여기서 건드리지 않는다.
 *  6) BLOCK_EXT_MAJOR(동적 확장 major 259번대)를 쓰는 minor 번호였다면
 *     blk_free_ext_minor() 로 idr/ida 풀에 번호를 반환.
 *  7) 마지막으로 bdev_inode 자체를 슬래브(bdev_cachep)에 반환.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(대개 iput() 의 마지막 참조 해제 또는
 * evict 워크). 이 시점에는 이미 아무도 이 inode/bdev 를 참조하지 않음이
 * 보장되므로(참조 카운트 0) 락 없이 필드에 접근해도 안전하다.
 *
 * caller: VFS destroy_inode()(fs/inode.c) -> sb->s_op->free_inode.
 * callee: I_BDEV(), free_percpu(), kfree(), security_bdev_free(),
 *         bdev_is_partition(), bdi_put(), blk_free_ext_minor(),
 *         kmem_cache_free().
 * 에러 경로: 없음(void, 실패할 수 없는 정리 루틴).
 *
 * 호출 체인:
 *   iput()/evict() (fs/inode.c) → destroy_inode() → [bdev_free_inode] →
 *   free_percpu()/kfree()/bdi_put()/blk_free_ext_minor()/kmem_cache_free()
 */

static void bdev_free_inode(struct inode *inode)
{
	struct block_device *bdev = I_BDEV(inode);
	/* [한국어] I_BDEV(): bdev_inode 는 struct inode 를 첫 멤버로 갖는
	 * container 구조체이므로, inode 포인터에서 container_of 매크로로
	 * struct block_device 의 주소를 역산한다(포인터 연산, 오프셋 0). */
// NVMe: 해제할 block_device 획득.

	/* [한국어] percpu 통계 구조체(struct disk_stats __percpu *) 해제.
	 * bdev_alloc() 에서 alloc_percpu() 로 CPU 코어 수만큼 할당했던
	 * 메모리를 free_percpu() 로 모두 반환한다. /sys/block/<disk>/stat
	 * 등으로 노출되던 읽기/쓰기 횟수·바이트 수 등의 원본 데이터가
	 * 사라지므로 반드시 마지막 참조 해제 시점에서만 호출돼야 한다. */
	free_percpu(bdev->bd_stats); 	/* NVMe: per-cpu IO 통계 구조체 해제. 요약. */
// NVMe: per-cpu IO 통계 구조체 해제. /sys/block/nvme0n1/stat 의 원본 데이터.
	/* [한국어] 파티션 UUID/PARTUUID/볼륨 라벨 등을 담는
	 * bd_meta_info(struct partition_meta_info) 동적 메모리 해제. 파티션이
	 * 아니면 애초에 NULL 이었을 수 있으나 kfree(NULL) 은 안전(no-op)하다. */
	kfree(bdev->bd_meta_info);
// NVMe: 파티션 메타데이터 해제.
	/* [한국어] LSM(SELinux 등)이 이 block_device 에 부착했던 보안
	 * 컨텍스트/레이블 자원을 해제. security_bdev_alloc() 의 짝. */
	security_bdev_free(bdev);
// NVMe: LSM 보안 레이블 해제.

	if (!bdev_is_partition(bdev)) {
		/* [한국어] 진입 조건: 이 bdev 가 파티션이 아니라 디스크 전체를
		 * 표현하는 "본체" bdev 일 때만 gendisk/bdi 를 실제로 해제한다.
		 * 파티션 bdev 들은 같은 gendisk 를 공유 참조하므로 각 파티션이
		 * 해제될 때마다 gendisk 를 지우면 다른 파티션/본체가 쓰는
		 * 메모리를 이중 해제(double free)하게 되어 반드시 구분해야
		 * 한다. */
// NVMe: 파티션이 아닌 전체 NVMe namespace 일 때만 gendisk/bdi 해제.
		if (bdev->bd_disk && bdev->bd_disk->bdi)
			/* [한국어] gendisk 가 아직 유효하고 bdi(backing device
			 * info, writeback/회수 정책을 담당하는 구조체) 참조를
			 * 들고 있으면 bdi_put() 으로 참조 카운트를 감소시켜
			 * 필요 시 bdi 자체도 해제되게 한다. */
// NVMe: bdi(backing dev info) 참조가 있으면 해제. writeback 인프라.
			bdi_put(bdev->bd_disk->bdi);
		kfree(bdev->bd_disk);
		/* [한국어] gendisk 구조체 메모리 자체를 반환. 이 시점 이후
		 * 이 disk 를 통한 request_queue 접근은 불가능해지므로, 이
		 * 코드에 도달했다는 것은 이미 상위에서 disk_release() 등을
		 * 통해 request_queue/디바이스 자원 정리가 끝났음을 전제한다. */
// NVMe: gendisk 객체 해제. 이후 NVMe request_queue 와의 연결이 끊어짐.
	}

	if (MAJOR(bdev->bd_dev) == BLOCK_EXT_MAJOR)
		/* [한국어] 진입 조건: 이 dev_t 의 major 번호가 고정 major 가
		 * 아니라 커널이 동적으로 배정하는 BLOCK_EXT_MAJOR(확장 블록
		 * 디바이스 major, 다중 파티션을 위해 minor 공간을 idr/ida 로
		 * 동적 관리)를 사용하는 경우에만 minor 번호를 반환해야 한다. */
// NVMe: BLOCK_EXT_MAJOR 를 사용한 확장 minor 일 경우 반환.
		blk_free_ext_minor(MINOR(bdev->bd_dev));
		/* [한국어] MINOR() 로 dev_t 에서 minor 번호를 추출해 확장
		 * minor 할당 풀(idr)에 반환, 다른 디바이스가 재사용 가능하게
		 * 한다. */
// NVMe: 확장 minor 번호를 풀에 반환.

	kmem_cache_free(bdev_cachep, BDEV_I(inode));
	/* [한국어] 이 함수의 마지막 단계: bdev_inode 자체를 슬래브 캐시로
	 * 반환. BDEV_I(inode) 는 I_BDEV() 와 마찬가지로 container_of 로
	 * struct bdev_inode * 를 얻는 매크로(다만 block_device 가 아니라
	 * bdev_inode 전체를 가리킨다). 이 호출이 반환되면 inode/bdev 메모리는
	 * 더 이상 유효하지 않다 - 이후 어떤 포인터도 이 메모리를 참조해서는
	 * 안 된다. */
// NVMe: bdev_inode 슬래브 반환. 이 참조가 마지막이면 메모리 해제.
}

/* kmem_cache 에서 새 객체를 받을 때 VFS inode 초기화. */
/*
 * [한국어]
 * init_once - bdev_cachep 슬래브의 kmem_cache constructor(생성자) 콜백.
 *
 * @data: kmem_cache_create() 가 슬래브 페이지에서 막 확보한, 아직 어떤
 *        내용도 채워지지 않은 raw 메모리 블록의 시작 주소(여기서는
 *        struct bdev_inode 하나가 들어갈 크기). 슬래브 페이지가 처음
 *        시스템 메모리에서 생성될 때 그 안의 각 객체 슬롯마다 한 번씩
 *        호출된다.
 * @return: 없음(void). 실패할 수 없는 초기화 루틴.
 *
 * SLAB/SLUB allocator 는 "constructor(생성자)" 개념을 지원해서, 객체가
 * 슬래브 페이지에 처음 만들어질 때 한 번만 특정 필드를 초기화해 두고,
 * 이후 kmem_cache_alloc()/kmem_cache_free() 로 재사용될 때는 그 초기화를
 * 반복하지 않아도 되게 한다. bdev_inode 안의 struct inode 부분은 링크드
 * 리스트 헤드(i_lru, i_sb_list 등)나 락 등 "제자리에서 한 번만 세팅하면
 * 되는" 필드가 많아, 매번 alloc_inode_sb() 가 호출될 때마다 다시 초기화할
 * 필요가 없다. 이 함수가 바로 그 1회성 초기화를 담당한다.
 *
 * 동작: data 를 struct bdev_inode * 로 캐스팅한 뒤, 그 안의 vfs_inode
 * 필드(struct inode)에 대해 inode_init_once() 를 호출해 VFS 가 요구하는
 * 리스트/락/타임스탬프 등의 초기 상태를 세팅한다. block_device 부분(ei
 * ->bdev)은 여기서 건드리지 않는다 - 그 부분은 매 할당마다
 * bdev_alloc_inode() 의 memset() 이 새로 초기화한다(재사용 시 이전
 * block_device 내용이 새 객체에 남아있으면 안 되기 때문).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 슬래브 페이지가 새로 확장될 때
 * kmem_cache_create() 에 등록해 둔 이 콜백을 slab allocator 가 내부적으로
 * 호출한다(bdev_cache_init() 의 kmem_cache_create() 마지막 인자로 전달됨).
 * 동시성: 슬래브 확장은 allocator 내부 락으로 보호되므로 이 함수이 재진입
 * 되거나 동시에 같은 객체에 대해 두 번 호출될 일은 없다.
 *
 * caller: SLAB/SLUB allocator 내부(슬래브 페이지 신규 할당 시).
 * callee: inode_init_once().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   kmem_cache_create() 등록 -> slab allocator 내부 페이지 확장 로직 →
 *   [init_once] → inode_init_once()
 */

static void init_once(void *data)
{
	struct bdev_inode *ei = data;
	/* [한국어] void * 로 전달된 raw 슬롯 주소를 bdev_inode 타입으로
	 * 재해석. 이 시점에는 아직 아무 필드도 유효한 값이 아니다(막
	 * 페이지가 만들어진 직후의 메모리). */

	inode_init_once(&ei->vfs_inode);
	/* [한국어] VFS 공통 inode 초기화 루틴: i_lock 스핀락, i_lru/i_sb_list
	 * 리스트 헤드, i_data(address_space) 등을 최초 1회 세팅한다. 이후
	 * 이 슬롯이 alloc/free 를 반복해도 이 리스트 헤드들의 무결성은
	 * 유지된 채 재사용된다(재사용 시 unlink 만 이루어지고 매번
	 * 새로 만들지 않음). */
// NVMe: VFS inode 를 한 번 초기화. 새 bdev_inode 재활용 시 사용.
}

/*
 * bdev_sops - bdevfs 슈퍼블록 연산.
 * .alloc_inode/.free_inode 는 위 함수들로 bdev_inode 생명주기를 관리.
 */
/*
 * [한국어] bdev_sops - bdevfs 전용 struct super_operations 테이블.
 *
 * VFS 는 각 슈퍼블록마다 이 연산 테이블(sb->s_op)을 통해 inode 할당/해제,
 * statfs(2) 응답, drop 정책 등 파일시스템(여기서는 pseudo-fs 인 bdevfs)
 * 고유의 동작을 위임받는다. bdevfs 는 실제 디스크에 저장되는 파일시스템이
 * 아니므로 대부분의 콜백(write_inode, sync_fs 등)을 구현하지 않고, inode
 * 생명주기 관리에 필요한 최소한의 4개만 채운다.
 */

static const struct super_operations bdev_sops = {
	.statfs = simple_statfs,
	/* [한국어] statfs(2)/fstatfs(2) 시스템 호출이 이 슈퍼블록에 대해
	 * 파일시스템 통계(블록 크기, 전체/여유 블록 수 등)를 요청할 때 호출.
	 * 설정자: 이 정적 구조체 리터럴 초기화가 유일한 설정자(런타임에
	 *         바뀌지 않음).
	 * 읽는 자: VFS 의 vfs_statfs()/statfs 시스템 호출 경로가
	 *         sb->s_op->statfs 를 통해 호출. 다만 bdevfs 는
	 *         kern_mount() 로만 마운트되어 사용자 공간에서 직접 이
	 *         슈퍼블록에 statfs(2) 를 걸 수 있는 마운트 지점이 없으므로
	 *         실질적으로는 거의 호출되지 않는다.
	 * 값 범위: simple_statfs 는 libfs 가 제공하는 범용 구현으로,
	 *         매직넘버(BDEVFS_MAGIC)와 기본 블록 크기만 채워 반환한다.
	 * 동기화: simple_statfs 내부에서 별도 락 없이 상수성 정보만 복사하므로
	 *         동기화가 필요 없다. */
	.alloc_inode = bdev_alloc_inode,
	/* [한국어] 새 inode 가 필요할 때(new_inode(), iget_locked() 등) VFS 가
	 * 호출해 실제 메모리 할당을 위임하는 콜백.
	 * 설정자: 이 정적 초기화 자체가 유일한 설정자.
	 * 읽는 자: VFS 의 alloc_inode()(fs/inode.c) 가 sb->s_op->alloc_inode
	 *         함수 포인터를 통해 호출. 이 chunk 의 bdev_alloc() 이
	 *         new_inode(blockdev_superblock) 을 호출하면 이 경로를 거쳐
	 *         결국 bdev_alloc_inode() 가 실행된다.
	 * 값 범위: 항상 bdev_alloc_inode 함수 포인터(NULL 이 될 수 없음 -
	 *         NULL 이면 VFS 가 범용 alloc_inode 폴백을 쓰지만 bdevfs 는
	 *         반드시 전용 콜백이 필요하므로 이 필드를 채운다).
	 * 동기화: 함수 포인터 자체는 불변(정적 데이터, .rodata 유사 섹션).
	 *         호출된 함수 내부의 동기화는 bdev_alloc_inode 주석 참고. */
	.free_inode = bdev_free_inode,
	/* [한국어] inode 가 완전히 소멸될 때(destroy_inode()) VFS 가 호출해
	 * bdev_inode 부가 자원을 정리시키는 콜백.
	 * 설정자: 이 정적 초기화 자체가 유일한 설정자.
	 * 읽는 자: VFS destroy_inode()(fs/inode.c) 가 sb->s_op->free_inode 로
	 *         호출(RCU-지연 콜백 evict_inode 이후 최종 해제 단계).
	 * 값 범위: 항상 bdev_free_inode 함수 포인터.
	 * 동기화: 이 시점에는 inode 참조 카운트가 이미 0이므로 별도 락
	 *         없이 안전하게 필드에 접근할 수 있다(bdev_free_inode 주석
	 *         참고). */
	.drop_inode = inode_just_drop,
	/* [한국어] 마지막 참조가 사라졌을 때(iput_final()) 이 inode 를 dcache
	 * 에 남겨둘지, 즉시 버릴지를 결정하는 정책 콜백.
	 * 설정자: 이 정적 초기화 자체가 유일한 설정자.
	 * 읽는 자: VFS 의 iput_final()(fs/inode.c) 이 sb->s_op->drop_inode 를
	 *         호출해 반환값(1이면 즉시 drop)에 따라 evict 여부를 결정.
	 * 값 범위: inode_just_drop 은 항상 "무조건 즉시 버림" 정책(일반
	 *         generic_drop_inode() 와 달리 dentry 캐시 히트 여부와
	 *         무관하게 매번 evict). bdev inode 는 dentry 를 통해 경로로
	 *         조회되는 것이 아니라 dev_t 해시로 조회되므로, 캐시에 남겨둘
	 *         유인이 없어 이 정책을 쓴다.
	 * 동기화: iput_final() 이 inode->i_lock 을 든 상태에서 호출하므로
	 *         이 콜백 자체는 별도 락을 새로 잡지 않는다(호출자가 이미
	 *         락 보유 상태로 진입). */
};

/* bdevfs 마운트 컨텍스트를 초기화. cgroup writeback 플래그 설정. */
/*
 * [한국어]
 * bd_init_fs_context - bdevfs 마운트 시 struct fs_context 를 초기화하는
 *                      file_system_type->init_fs_context 콜백.
 *
 * @fc: 마운트 요청을 표현하는 새 fs_context. bd_type.init_fs_context 로
 *      등록되어 있어, bdevfs 를 마운트하려는 모든 시도(이 파일에서는
 *      bdev_cache_init() 의 kern_mount() 단 한 번)에서 커널이 자동으로
 *      호출한다.
 * @return: 성공 시 0. init_pseudo() 가 pseudo_fs_context 할당에 실패하면
 *          -ENOMEM. 호출자(vfs_get_tree() 등 마운트 공통 경로)는 음수
 *          반환값을 그대로 상위(mount 시스템 호출 또는 kern_mount())에
 *          에러로 전파한다.
 *
 * bdevfs 는 디스크에서 슈퍼블록을 읽어오는 실제 파일시스템이 아니라
 * 커널이 메모리 안에서만 구성하는 "pseudo(가짜)" 파일시스템이므로,
 * generic 마운트 로직 대신 libfs 의 init_pseudo() 헬퍼로 최소한의
 * superblock/fs_context 골격만 만든다.
 *
 * 동작 순서:
 *  1) init_pseudo(fc, BDEVFS_MAGIC) 으로 pseudo_fs_context 를 만들고,
 *     매직넘버를 BDEVFS_MAGIC 으로 설정(statfs(2) 의 f_type 필드 등에
 *     노출되어 이 fs 를 식별 가능하게 함).
 *  2) 실패 시 -ENOMEM 반환.
 *  3) fc->s_iflags 에 SB_I_CGROUPWB 비트를 OR 로 추가 설정 - 이 슈퍼블록
 *     아래 inode 들의 writeback(더티 페이지 기록)이 cgroup(메모리/IO
 *     제어 그룹) 단위로 계정/제어될 수 있음을 VFS 에 알리는 플래그.
 *     bdev 의 페이지 캐시(예: O_DIRECT 를 쓰지 않는 블록 장치 직접 접근)
 *     쓰기가 올바른 cgroup 에 회계되도록 하기 위해 필요하다.
 *  4) ctx->ops 에 bdev_sops(앞서 정의한 super_operations)를 연결해,
 *     이후 만들어질 슈퍼블록이 bdev_alloc_inode 등을 쓰도록 한다.
 *  5) 성공(0) 반환.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 커널 부팅 초기, bdev_cache_init() 이
 * kern_mount() 를 호출하는 단 한 번의 경로에서만 실행되므로 동시성 걱정이
 * 없다(그 외에는 이 pseudo-fs 를 사용자가 다시 마운트할 방법이 없음).
 *
 * caller: kern_mount(&bd_type) 내부의 vfs_get_tree() 등 마운트 공통 경로가
 *         fc->fs_type->init_fs_context 함수 포인터를 통해 호출.
 * callee: init_pseudo().
 * 에러 경로: init_pseudo() 실패(-ENOMEM) 시 그대로 상위에 전파, 이 경우
 *          bdev_cache_init() 의 kern_mount() 가 IS_ERR() 로 감지해 결국
 *          panic() 으로 이어진다(부팅 필수 인프라이므로).
 *
 * 호출 체인:
 *   bdev_cache_init() → kern_mount() → vfs_get_tree() →
 *   [bd_init_fs_context] → init_pseudo()
 */

static int bd_init_fs_context(struct fs_context *fc)
{
	struct pseudo_fs_context *ctx = init_pseudo(fc, BDEVFS_MAGIC);
	/* [한국어] libfs 헬퍼: fs_context 안에 pseudo_fs_context 를 할당하고
	 * fc->s_magic 등을 BDEVFS_MAGIC 으로 세팅한 뒤 그 컨텍스트 포인터를
	 * 반환한다. bdevfs 전용 매직넘버로 statfs(2) 의 f_type, /proc/mounts
	 * 등에서 이 pseudo-fs 를 식별할 수 있게 된다. */
	if (!ctx)
		/* [한국어] init_pseudo() 내부의 kzalloc 등이 실패해 NULL 을
		 * 반환한 경우 - 메모리 부족으로 더 진행할 수 없으므로 즉시
		 * -ENOMEM 을 반환. */
		return -ENOMEM;
	fc->s_iflags |= SB_I_CGROUPWB;
	/* [한국어] 비트 OR 연산으로 s_iflags 에 SB_I_CGROUPWB 플래그 비트를
	 * 추가(다른 플래그 비트는 그대로 유지). 이 플래그가 켜져 있으면
	 * VFS/cgroup 서브시스템이 이 슈퍼블록 소속 inode 의 더티 페이지를
	 * writeback 할 때 해당 페이지를 더럽힌 cgroup 에 정확히 회계하고,
	 * cgroup 별 IO 쓰기 제한(cgroup v2 io controller)이 블록 장치
	 * 자체의 페이지 캐시 쓰기에도 적용되도록 한다. */
// NVMe: cgroup writeback 플래그. NVMe IO 에 대한 cgroup 제어 및 쓰기 회계 가능.
	ctx->ops = &bdev_sops;
	/* [한국어] 방금 만든 pseudo_fs_context 에 앞서 정의한 bdev_sops
	 * 테이블을 연결. 이후 fill_super 단계에서 이 ctx->ops 값이
	 * sb->s_op 로 복사되어, 이 슈퍼블록 아래 모든 inode 할당/해제가
	 * bdev_alloc_inode/bdev_free_inode 를 거치게 된다. */
	return 0;
	/* [한국어] 성공 반환 - 마운트 공통 로직이 계속 fill_super 등 다음
	 * 단계로 진행하도록 허용. */
}

/*
 * bd_type - "bdev" 가상 파일시스템 타입.
 * /dev/nvme0n1 노드는 이 bdevfs 위에 존재하는 inode 이다.
 */
/*
 * [한국어] bd_type - "bdev" pseudo 파일시스템의 struct file_system_type
 * 등록 정보. register_filesystem() 으로 커널의 전역 파일시스템 목록에
 * 등록되고, kern_mount() 가 이 타입을 이름으로 찾아 bdevfs 인스턴스를
 * 만든다. 사용자가 mount(2) 로 "bdev" 타입을 직접 마운트하는 것을 막는
 * 별도 플래그는 없지만, 실제로는 커널 내부에서 kern_mount() 로만
 * 사용되도록 설계된 내부 전용 pseudo-fs 이다.
 */

static struct file_system_type bd_type = {
	.name		= "bdev",
	/* [한국어] 이 파일시스템 타입의 이름 문자열.
	 * 설정자: 이 정적 초기화가 유일한 설정자.
	 * 읽는 자: register_filesystem() 이 전역 file_systems 리스트에서 이
	 *         이름으로 중복 등록을 검사하고, kern_mount()/mount(2) 가
	 *         "-t bdev" 처럼 타입 이름으로 이 항목을 찾을 때 비교 대상이
	 *         된다. /proc/filesystems 에도 이 이름이 노출된다.
	 * 값 범위: 정적 문자열 리터럴 "bdev" (읽기 전용 rodata, 변경 불가).
	 * 동기화: 불변 데이터이므로 락 불필요. */
	.init_fs_context = bd_init_fs_context,
	/* [한국어] 마운트 시 fs_context 초기화를 위임할 콜백.
	 * 설정자: 이 정적 초기화가 유일한 설정자.
	 * 읽는 자: 마운트 공통 경로(fs/fs_context.c 의 vfs_get_tree() 계열)가
	 *         fc->fs_type->init_fs_context 를 통해 호출. 이 chunk 안에서는
	 *         bdev_cache_init() 의 kern_mount(&bd_type) 이 트리거한다.
	 * 값 범위: 항상 bd_init_fs_context 함수 포인터.
	 * 동기화: 함수 포인터 자체는 불변. */
	.kill_sb	= kill_anon_super,
	/* [한국어] 이 타입의 슈퍼블록을 마지막으로 해제(언마운트/정리)할 때
	 * 호출되는 콜백.
	 * 설정자: 이 정적 초기화가 유일한 설정자.
	 * 읽는 자: deactivate_locked_super()(fs/super.c) 가 sb->s_type
	 *         ->kill_sb 를 통해 호출. bdevfs 는 실제 블록 장치 위에
	 *         마운트되는 것이 아니라 메모리 전용(anonymous) 슈퍼블록이므로
	 *         kill_block_super() 가 아니라 kill_anon_super() 를 쓴다
	 *         (block device 를 파트너로 갖지 않는 슈퍼블록 해제 루틴).
	 * 값 범위: 항상 kill_anon_super 함수 포인터.
	 * 동기화: 함수 포인터 자체는 불변. 실제 호출 시점 동기화는
	 *         deactivate_locked_super 의 s_umount 세마포어가 보장. */
};

/*
 * blockdev_superblock - bdevfs 의 슈퍼블록.
 * 모든 block_device 의 VFS inode 는 이 슈퍼블록의 inode list 에 연결됨.
 * NVMe 관점: sync_bdevs() 시 이 리스트를 순회하며 NVMe 디바이스들의
 *          dirty 캐시를 플러시한다.
 */
/*
 * [한국어] 아래 두 전역은 bdev_cache_init() 에서 부팅 시 한 번 설정되는
 * bdevfs 마운트 인스턴스를 가리킨다. blockdev_mnt 는 마운트 지점(vfsmount)
 * 자체를, blockdev_superblock 은 그 마운트가 가리키는 슈퍼블록을 각각
 * 나타내며, 이후 이 파일의 다른 함수들(bdev_alloc, nr_blockdev_pages 등)
 * 과 blk-core 의 sync_bdevs() 등이 이 슈퍼블록을 통해 "시스템에 존재하는
 * 모든 block_device 의 inode 리스트"에 접근한다.
 */

struct super_block *blockdev_superblock __ro_after_init;
/* [한국어] bdevfs 슈퍼블록 포인터. 모든 block_device 에 대응하는
 * VFS inode(struct bdev_inode)가 이 슈퍼블록의 s_inodes 리스트에
 * 걸린다 - 즉 "커널에 존재하는 모든 블록 장치"의 카탈로그 역할을 하는
 * 슈퍼블록이다.
 * 설정자: bdev_cache_init() 에서 blockdev_mnt->mnt_sb 값을 단 한 번
 *         대입(부팅 시퀀스 중 1회). __ro_after_init 이므로 그 이후에는
 *         메모리 보호(읽기 전용 재매핑)로 인해 어떤 코드도 이 값을 다시
 *         바꿀 수 없다.
 * 읽는 자: 이 chunk 의 bdev_alloc()(new_inode() 의 인자로), nr_
 *         blockdev_pages()(s_inodes 리스트 순회), 그리고 이 파일 밖의
 *         sync_bdevs()/fs/sync.c 등이 "전체 bdev 목록"을 순회할 때 이
 *         포인터를 시작점으로 사용한다.
 * 값 범위: 부팅 초기(초기화 전)에는 NULL 이었다가, bdev_cache_init() 이
 *         panic 없이 끝나면 항상 유효한 슈퍼블록 포인터. EXPORT_SYMBOL_GPL
 *         로 내보내져 GPL 라이선스 모듈에서도 참조 가능.
 * 동기화: __ro_after_init 이후 값 자체는 불변. 다만 이 슈퍼블록이 가리키는
 *         s_inodes 리스트는 계속 변하므로, 그 리스트를 순회할 때는 별도로
 *         s_inode_list_lock 스핀락이 필요하다(nr_blockdev_pages() 참고). */
static struct vfsmount *blockdev_mnt __ro_after_init;
/* [한국어] bdevfs 를 커널 내부에서 마운트한 결과인 vfsmount(마운트
 * 인스턴스) 포인터.
 * 설정자: bdev_cache_init() 에서 kern_mount(&bd_type) 의 반환값을 단
 *         한 번 대입.
 * 읽는 자: bdev_cache_init() 자신이 이 값에서 ->mnt_sb 를 꺼내
 *         blockdev_superblock 을 채우는 데 쓴 뒤로는, 이 chunk 안에서는
 *         더 이상 직접 읽히지 않는다(마운트 자체를 언마운트하거나 다시
 *         참조할 필요가 없는 "영구 마운트"이기 때문 - static 이라 이
 *         파일 밖에서도 접근 불가).
 * 값 범위: 초기화 전 NULL/미정의, kern_mount() 성공 후 유효한 vfsmount
 *         포인터(실패 시 bdev_cache_init() 이 panic() 하므로 런타임에
 *         에러 포인터 상태로 남는 경우가 없다).
 * 동기화: __ro_after_init, static 이므로 이 파일 밖 접근도 없고 값도
 *         불변 - 별도 동기화 불필요. */
EXPORT_SYMBOL_GPL(blockdev_superblock);
/* [한국어] blockdev_superblock 심볼을 GPL 라이선스 모듈에 한해 커널 심볼
 * 테이블에 노출(export)한다. 이 매크로는 커널 빌드 시 System.map/심볼
 * 테이블에 항목을 추가해, GPL 호환 모듈(예: 일부 파일시스템/블록 계층
 * 모듈)이 모듈 로드 시점에 이 전역 변수를 링크해 사용할 수 있게
 * 해준다(비 GPL 모듈은 링크 시 거부됨 - MODULE_LICENSE 검사). */

/*
 * bdev_cache_init - 부팅 시 bdev_cache 와 bdevfs 를 초기화.
 * NVMe 연결점: 이 초기화가 완료된 후 NVMe 드라이버가 로드되어 namespace
 *            별 bdev_inode 를 할당할 수 있다.
 */
/*
 * [한국어]
 * bdev_cache_init - 부팅 시퀀스에서 bdev 전용 슬래브 캐시와 bdevfs
 *                   pseudo-파일시스템을 1회성으로 초기화하는 함수.
 *
 * @return: 없음(void). 내부적으로 실패 시 panic() 으로 부팅을 중단시키므로
 *          "실패해서 리턴"하는 경로 자체가 없다 - 이 초기화가 실패하면
 *          이후 어떤 block_device 도 만들 수 없어 시스템이 무의미해지기
 *          때문에 정상적인 에러 반환 대신 panic 을 택한 것.
 *
 * struct block_device/bdev_inode 인프라는 커널의 다른 모든 서브시스템
 * (파일시스템 마운트, 블록 드라이버 프로브 등)보다 먼저 준비되어 있어야
 * 한다 - 어떤 디스크 드라이버든 자신의 gendisk 를 위해 bdev_alloc() 을
 * 호출하려면 bdev_cachep 슬래브와 blockdev_superblock 이 이미 존재해야
 * 하기 때문이다. 이 함수는 그 부팅 순서를 보장하기 위해 커널 초기화
 * 시퀀스(fs_initcall 류)에서 이른 시점에 한 번 호출된다.
 *
 * 동작 순서:
 *  1) kmem_cache_create() 로 "bdev_cache" 라는 이름의 슬래브를 생성.
 *     크기는 sizeof(struct bdev_inode), 정렬 요구는 0(기본), 플래그는
 *     SLAB_HWCACHE_ALIGN(캐시라인 정렬)|SLAB_RECLAIM_ACCOUNT(회수 가능
 *     메모리로 계정)|SLAB_ACCOUNT(cgroup 메모리 계정 대상)|SLAB_PANIC
 *     (생성 실패 시 즉시 panic), 생성자는 init_once().
 *  2) register_filesystem(&bd_type) 으로 "bdev" 타입을 전역 파일시스템
 *     목록에 등록. 실패하면(이미 같은 이름이 등록되어 있는 등) panic.
 *  3) kern_mount(&bd_type) 으로 사용자 공간 개입 없이 커널 스스로
 *     bdevfs 를 마운트. 실패(IS_ERR)하면 panic.
 *  4) 마운트 결과에서 슈퍼블록을 꺼내 blockdev_superblock 에 저장 -
 *     이후 writeback 코드가 "전체 bdev inode 리스트"를 순회하는 시작점.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 커널 부팅 중 단 한 번(__init 속성 -
 * 이 함수의 코드 자체가 초기화 완료 후 메모리에서 회수되는 init 섹션에
 * 위치). 동시 호출/재진입 없음이 설계상 보장된다.
 *
 * caller: 커널 초기화 시퀀스(예: fs/inode.c 의 vfs_caches_init() 이 이
 *         함수를 호출하는 경로 - 이 chunk 범위 밖).
 * callee: kmem_cache_create(), register_filesystem(), kern_mount(),
 *         panic().
 * 에러 경로: register_filesystem 실패 또는 kern_mount 실패 시 모두
 *          panic() 으로 부팅 자체를 중단 - 복구 불가능한 필수 인프라이기
 *          때문에 일반적인 에러 반환 대신 강경한 처리를 택함.
 *
 * 호출 체인:
 *   vfs_caches_init()(fs/inode.c, 부팅 초기화) → [bdev_cache_init] →
 *   kmem_cache_create() / register_filesystem() / kern_mount()
 */

void __init bdev_cache_init(void)
{
	int err;
	/* [한국어] register_filesystem() 의 반환값(성공 0 / 실패 음수
	 * errno)을 담을 지역 변수. */

	bdev_cachep = kmem_cache_create("bdev_cache", sizeof(struct bdev_inode),
	/* [한국어] "bdev_cache" 라는 이름으로 이 슬래브가 /proc/slabinfo
	 * 등에 노출된다. 객체 크기는 struct bdev_inode 전체(내장된 struct
	 * block_device 포함)이며, 이 크기만큼의 슬롯을 슬래브 페이지에서
	 * 잘라 쓴다. */
// NVMe: bdev_inode 를 위한 kmem_cache 생성.
			0, (SLAB_HWCACHE_ALIGN|SLAB_RECLAIM_ACCOUNT|
			/* [한국어] SLAB_HWCACHE_ALIGN: 각 객체를 하드웨어
			 * 캐시라인 경계에 맞춰 정렬해, 인접 객체와 같은
			 * 캐시라인을 공유해 발생하는 false sharing(서로 다른
			 * CPU 가 실제로는 다른 필드를 쓰는데 캐시라인이
			 * 겹쳐 캐시 무효화가 반복되는 현상)을 줄인다. 여러
			 * block_device 가 서로 다른 CPU 에서 자주 갱신되므로
			 * (예: bd_stats 갱신) 정렬이 중요.
			 * SLAB_RECLAIM_ACCOUNT: 이 슬래브가 사용하는 메모리를
			 * "회수 가능(reclaimable)" 메모리로 계정 - 메모리
			 * 압박 시 커널이 회수 가능한 캐시로 인식하고
			 * /proc/meminfo 의 SReclaimable 등에 반영. */
// NVMe: 하드웨어 캐시라인 정렬, false sharing 감소.
				SLAB_ACCOUNT|SLAB_PANIC),
				/* [한국어] SLAB_ACCOUNT: 이 슬래브에서 만든
				 * 객체 할당을 cgroup(kmemcg) 메모리 계정
				 * 대상에 포함시켜, 어떤 cgroup 이 얼마나 많은
				 * bdev_inode 를 만들었는지 메모리 컨트롤러가
				 * 추적/제한할 수 있게 한다.
				 * SLAB_PANIC: 이 kmem_cache_create() 호출
				 * 자체가 실패하면(매우 이른 부팅 단계에서
				 * 메모리 부족 등) 정상적인 NULL 반환 대신
				 * 커널 내부에서 즉시 panic() 을 일으킨다 -
				 * 이 슬래브 없이는 어떤 block_device 도 만들
				 * 수 없어 부팅을 계속할 의미가 없기 때문. */
// NVMe: 메모리 회계 및 초기화 실패 시 panic.
			init_once);
			/* [한국어] 슬래브 페이지가 새로 생성될 때마다 각
			 * 객체 슬롯에 대해 한 번씩 호출될 생성자 콜백. */
	err = register_filesystem(&bd_type);
	/* [한국어] 전역 file_systems 연결 리스트에 "bdev" 타입을 등록.
	 * 반환값 0 은 성공, 음수는 실패(예: 이름 중복). */
// NVMe: "bdev" pseudo 파일시스템 등록.
	if (err)
		/* [한국어] 진입 조건: 등록 실패. bdevfs 없이는 어떤
		 * block_device 도 inode 를 가질 수 없으므로 복구를 시도하지
		 * 않고 즉시 panic. */
		panic("Cannot register bdev pseudo-fs");
	blockdev_mnt = kern_mount(&bd_type);
	/* [한국어] 방금 등록한 "bdev" 타입으로 실제 마운트를 수행해
	 * vfsmount 인스턴스를 얻는다. 사용자 마운트 네임스페이스와 무관한
	 * 커널 내부 전용 마운트. */
// NVMe: 커널 내부에서 bdevfs 마운트.
	if (IS_ERR(blockdev_mnt))
		/* [한국어] 진입 조건: kern_mount() 가 에러 포인터(예: 슈퍼블록
		 * 할당 실패, fill_super 실패)를 반환한 경우. 이 역시 복구
		 * 불가능한 부팅 실패로 간주해 panic. */
		panic("Cannot create bdev pseudo-fs");
	blockdev_superblock = blockdev_mnt->mnt_sb;   /* For writeback */
	/* [한국어] 마운트 인스턴스에서 슈퍼블록 포인터를 꺼내 전역
	 * blockdev_superblock 에 저장. 원본 주석 "For writeback" 이 의미하는
	 * 바: 이후 파일시스템 writeback 코드(예: sync_bdevs(),
	 * fs/fs-writeback.c 의 wakeup_flusher_threads() 등)가 이 슈퍼블록의
	 * s_inodes 리스트를 순회하며 dirty 한 block_device inode 들을 찾아
	 * flush 하기 위한 진입점으로 이 값을 사용한다. */
// NVMe: writeback 시 모든 bdev inode 리스트를 순회하는 출발점.
}

/*
 *  - bdev->bd_stats: per-cpu IO 통계. 이후 /sys/block/nvme0n1/stat 등에 노출.
 */
/*
 * bdev_alloc - gendisk 와 파티션 번호를 바탕으로 block_device 를 할당/초기화.
 *
 * @disk: NVMe namespace 를 표현하는 gendisk. disk->queue 가 NVMe request_queue.
 * @partno: 0이면 전체 디바이스(/dev/nvme0n1), 양수면 파티션(/dev/nvme0n1p1).
 *
 * 주요 필드-NVMe 연결:
 *  - bdev->bd_queue = disk->queue: bio 가 최종적으로 도달하는 NVMe request_queue.
 *  - BD_HAS_SUBMIT_BIO: disk 가 커스텀 submit_bio 를 사용하면 파티션에도
 *                       동일 플래그를 상속. NVMe multipath 등에서 의미.
 *  - bdev->bd_stats: per-cpu IO 통계. 이후 /sys/block/nvme0n1/stat 등에 노출.
 */
/*
 * [한국어]
 * bdev_alloc - 주어진 gendisk 와 파티션 번호에 대응하는 새
 *              struct block_device 를 할당하고 필수 필드를 초기화.
 *
 * @disk: 이 block_device 가 속할 디스크를 표현하는 gendisk. partno==0 이면
 *        disk 자체(전체 디바이스, 예: /dev/nvme0n1)를, partno>0 이면 disk
 *        의 한 파티션(예: /dev/nvme0n1p1)을 만드는 것이다. disk->queue 가
 *        이후 이 bdev 로 들어오는 모든 bio 가 최종적으로 전달될
 *        request_queue 이다.
 * @partno: 파티션 번호. 0=디스크 전체 자신, 1 이상=몇 번째 파티션인지.
 *          u8(부호 없는 1바이트) 이므로 0~255 범위.
 * @return: 성공 시 새로 초기화된 struct block_device 포인터. inode 할당
 *          실패 또는 percpu 통계 할당 실패 시 NULL. 호출자(add_disk()/
 *          blk_add_partitions() 등, 이 chunk 밖)는 NULL 을 받으면 디스크/
 *          파티션 등록 자체를 실패로 처리한다.
 *
 * gendisk(디스크 전체를 표현하는 커널 객체)가 새로 만들어지거나(add_disk())
 * 파티션 테이블을 스캔해 파티션이 발견될 때마다, 그 각각을 사용자 공간에
 * /dev/xxx 노드로 노출하려면 대응하는 struct block_device + VFS inode 쌍이
 * 필요하다. 이 함수가 그 쌍을 만드는 유일한 생성 지점이다.
 *
 * 동작 순서:
 *  1) new_inode(blockdev_superblock) 으로 bdevfs 슈퍼블록에 새 inode 를
 *     받는다(내부적으로 이 chunk 의 bdev_alloc_inode() 콜백이 실제 메모리
 *     를 할당).
 *  2) 실패 시 NULL.
 *  3) inode 의 기본 필드(i_mode=S_IFBLK, i_rdev=0(임시), i_data.a_ops=
 *     def_blk_aops, gfp 마스크)를 설정해 이 inode 가 "블록 장치 노드"이며
 *     기본 블록 캐시 연산을 쓰도록 표시한다. 실제 dev_t 는 아직 모르므로
 *     i_rdev 는 0 - 나중에 bdev_add() 가 채운다.
 *  4) I_BDEV(inode) 로 block_device 부분을 얻어 그 안의 락들(뮤텍스/
 *     스핀락)을 초기화한다.
 *  5) atomic_set(&bdev->__bd_flags, partno) 로 파티션 번호와 상태 플래그를
 *     함께 담는 원자적 워드를 초기화(아래 인라인 주석 참고).
 *  6) bd_mapping/bd_queue 를 각각 이 inode 의 address_space, disk 의
 *     request_queue 로 연결 - 이후 이 bdev 로 들어오는 I/O 의 캐시/큐
 *     경로가 확정된다.
 *  7) partno 가 0이 아니고(파티션이고) disk->part0(디스크 전체 bdev)가
 *     BD_HAS_SUBMIT_BIO 플래그(gendisk 가 자체 submit_bio 콜백을 구현함,
 *     예: 멀티패스/스택 드라이버)를 갖고 있다면 이 파티션에도 동일 플래그
 *     를 상속시켜, 파티션에 대한 I/O 도 같은 커스텀 제출 경로를 타게 한다.
 *  8) bd_stats(per-cpu I/O 통계)를 할당. 실패하면 iput() 으로 방금 만든
 *     inode 를 되돌리고 NULL.
 *  9) bd_disk = disk 로 역참조를 설정하고 bdev 를 반환.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(디스크 프로브/파티션 스캔 경로). 이
 * 시점의 bdev 는 아직 어디에도 공개(insert_inode_hash 되지 않음)되지
 * 않았으므로 다른 스레드가 동시에 이 bdev 를 볼 수 없다 - 초기화 중
 * 락(mutex_init 등)은 "앞으로 쓰일 락을 준비"하는 것이지, 지금 경쟁을
 * 막기 위한 것이 아니다.
 *
 * caller: add_disk()/디스크 파티션 스캔 경로(bdev_disk_changed() 등, 이
 *         chunk 밖)가 gendisk 마다, 그리고 파티션마다 이 함수를 호출.
 * callee: new_inode(), mutex_init(), spin_lock_init(), atomic_set(),
 *         bdev_test_flag()/bdev_set_flag(), alloc_percpu(), iput().
 * 에러 경로: inode 할당 실패 또는 bd_stats 할당 실패 시 NULL 반환(후자는
 *          이미 만든 inode 를 iput() 으로 반드시 반납해 누수 방지).
 *
 * 호출 체인:
 *   add_disk()/파티션 스캔 (이 chunk 밖) → [bdev_alloc] →
 *   new_inode() → bdev_alloc_inode()
 */

struct block_device *bdev_alloc(struct gendisk *disk, u8 partno)
{
	struct block_device *bdev;
	/* [한국어] 이 함수가 최종적으로 채워 반환할 block_device 포인터.
	 * inode 할당 이후 I_BDEV() 로 얻는다. */
	struct inode *inode;
	/* [한국어] bdevfs 에서 새로 받을 VFS inode. block_device 는 이 inode
	 * 안에 내장(bdev_inode 를 통해)되어 있으므로 결국 같은 메모리
	 * 블록을 가리키게 된다. */

	inode = new_inode(blockdev_superblock);
	/* [한국어] bdevfs 전용 슈퍼블록에서 새 inode 를 요청. 내부적으로
	 * sb->s_op->alloc_inode(bdev_alloc_inode)를 호출해 bdev_cachep
	 * 슬래브에서 메모리를 받는다. */
// NVMe: bdevfs 슈퍼블록에서 새 inode 할당.
	if (!inode)
		/* [한국어] 슬래브/LSM 초기화 실패로 inode 를 못 받은 경우,
		 * 더 진행할 게 없으므로 즉시 NULL. */
		return NULL;
	inode->i_mode = S_IFBLK;
	/* [한국어] 이 inode 의 파일 타입 비트를 "블록 특수 파일"로 설정.
	 * stat(2) 의 st_mode 에 S_ISBLK() 로 확인되는 값이며, 사용자 공간의
	 * mknod/udev 가 만드는 /dev 노드의 타입과 대응한다. */
// NVMe: 블록 장치 노드로 설정.
	inode->i_rdev = 0;
	/* [한국어] 아직 major/minor(dev_t)를 모르므로 0으로 임시 설정.
	 * 실제 dev_t 는 이후 bdev_add() 가 채운다 - bdev_alloc() 은 "메모리
	 * 상 객체 준비"만 담당하고 "식별자 부여"는 별개 단계이기 때문이다. */
// NVMe: 초기 major/minor 0, bdev_add() 에서 실제 dev_t 설정.
	inode->i_data.a_ops = &def_blk_aops;
	/* [한국어] 이 inode 의 페이지 캐시(address_space)가 사용할 기본
	 * 블록 장치용 address_space_operations(def_blk_aops, readpage/
	 * writepage 등 블록 I/O 경로)를 연결한다. */
// NVMe: 기본 블록 address_space 연산.
	mapping_set_gfp_mask(&inode->i_data, GFP_USER);
	/* [한국어] 이 address_space 가 페이지 캐시 페이지를 할당할 때 쓸
	 * GFP 마스크를 GFP_USER(사용자 프로세스에 준하는 회수/재활용 정책)
	 * 로 설정 - 커널 내부 전용 긴급 메모리 풀을 쓰지 않도록 제한. */
// NVMe: 사용자 페이지 할당 마스크 설정.

	bdev = I_BDEV(inode);
	/* [한국어] 방금 받은 inode 로부터 container_of 로 struct
	 * block_device * 를 얻는다(bdev_inode 안에서 vfs_inode 바로 뒤에
	 * bdev 필드가 이어지는 레이아웃). */
	mutex_init(&bdev->bd_fsfreeze_mutex);
	/* [한국어] freeze_bdev()/thaw_bdev() 가 이 장치의 freeze 상태(파일
	 * 시스템 freeze 중 쓰기 차단)를 보호하는 데 쓸 뮤텍스를 초기화. */
// NVMe: freeze/thaw 상태 보호 뮤텍스.
	spin_lock_init(&bdev->bd_size_lock);
	/* [한국어] bdev_set_nr_sectors() 가 bd_nr_sectors 와 inode 크기를
	 * 함께 갱신할 때 쓰는 스핀락을 초기화 - 아래 bdev_set_nr_sectors
	 * 주석 참고(리더가 절반만 갱신된 크기를 보지 않도록 하는 락). */
// NVMe: bd_nr_sectors 와 i_size 동시 갱신 보호.
	mutex_init(&bdev->bd_holder_lock);
	/* [한국어] bd_holder(이 bdev 를 배타적으로 연 소유자, 예: 파일시스템/
	 * DM/LVM)와 관련 필드를 보호하는 뮤텍스 초기화(이 chunk 밖의 claim/
	 * release 경로에서 사용). */
	atomic_set(&bdev->__bd_flags, partno);
	/* [한국어] __bd_flags 는 파티션 번호(BD_PARTNO)와 BD_* 상태 플래그
	 * (BD_HAS_SUBMIT_BIO, BD_RO 등)를 한 워드(atomic_t)에 함께 패킹해
	 * 담는 필드다. 별도의 int partno + 별도의 flags 필드로 나누지 않고
	 * 하나의 atomic_t 로 합쳐 둔 이유는: (1) 두 값을 별도 필드로 두면
	 * 갱신 시 두 번의 원자적 연산이 필요해 그 사이에 다른 CPU 가 절반만
	 * 갱신된 상태를 볼 여지가 생기지만, 한 워드로 합치면 단일
	 * atomic_set/atomic_or 등으로 항상 일관된 스냅샷을 유지할 수 있고,
	 * (2) block_device 구조체 크기를 줄여 캐시 효율을 높이기 위함이다.
	 * 여기서는 partno 값을 그대로 초기값으로 설정(플래그 비트는 아직
	 * 전부 0), 이후 bdev_set_flag()/bdev_clear_flag() 가 각각 정해진
	 * 비트 위치에 대해 atomic_or/atomic_andnot 으로 플래그만 갱신하고
	 * bdev_test_flag()/bdev_partno() 류 헬퍼가 마스킹으로 각 부분을
	 * 추출한다. */
// NVMe: 파티션 번호 및 BD_* 플래그 원자적 설정.
	bdev->bd_mapping = &inode->i_data;
	/* [한국어] 이 bdev 의 페이지 캐시 address_space 를 방금 설정한 inode
	 * ->i_data 로 연결. 이후 파일시스템/버퍼 캐시 계층이 bdev->bd_mapping
	 * 을 통해 페이지 캐시에 접근하게 되는 핵심 연결점. */
// NVMe: 페이지 캐시 address_space 연결.
	bdev->bd_queue = disk->queue; 	/* NVMe: bd_queue = NVMe request_queue. */
	/* [한국어] 이 bdev 로 들어오는 모든 bio 가 최종적으로 제출될
	 * request_queue 를 disk->queue 로 연결한다. submit_bio() 가 이
	 * 필드를 통해 큐를 찾아 블록 드라이버(NVMe 등)에 요청을 전달한다. */
// NVMe: bdev->bd_queue 가 NVMe request_queue 를 가리킴. bio 는 이 queue 로 진입.
	if (partno && bdev_test_flag(disk->part0, BD_HAS_SUBMIT_BIO)) 	/* NVMe: 파티션에도 플래그 상속 조건. */
		/* [한국어] 진입 조건: (1) 이것이 파티션이고(partno != 0),
		 * (2) 디스크 전체 bdev(disk->part0)가 이미 BD_HAS_SUBMIT_BIO
		 * (드라이버가 표준 request_queue 대신 자체 submit_bio 콜백을
		 * 구현, 예: 소프트웨어 RAID/멀티패스처럼 bio 를 재라우팅하는
		 * 스택형 드라이버)를 갖고 있을 때만 진입. 파티션은 독자적으로
		 * submit_bio 를 재정의하지 않으므로, 디스크 전체의 속성을
		 * 그대로 물려받아야 파티션 I/O 도 같은 경로를 타게 된다. */
// NVMe: NVMe multipath 등에서 disk 가 직접 submit_bio 를 구현하면 파티션에도 상속.
		bdev_set_flag(bdev, BD_HAS_SUBMIT_BIO);
		/* [한국어] BD_HAS_SUBMIT_BIO 비트를 __bd_flags 에 원자적으로
		 * OR 설정 - 위에서 만든 partno 값은 그대로 둔 채 플래그
		 * 비트만 추가된다. */
	bdev->bd_stats = alloc_percpu(struct disk_stats);
	/* [한국어] CPU 코어마다 독립된 struct disk_stats(읽기/쓰기 횟수,
	 * 바이트, 소요 시간 등 누적 카운터) 인스턴스를 할당. CPU 마다 별도
	 * 카운터를 두는 이유는 각 CPU 가 자신의 카운터만 갱신해 캐시라인
	 * 경합(false sharing) 없이 빠르게 통계를 쌓기 위함(합산은 조회 시
	 * part_stat_read_all 류 함수가 수행). */
// NVMe: per-cpu IO 통계 할당. 추후 /sys/block/nvme0n1/stat 등에 노출.
	if (!bdev->bd_stats) {
		/* [한국어] 진입 조건: percpu 할당 실패(메모리 부족). 이미
		 * 만들어둔 inode 를 그대로 두면 누수이므로 반드시 반납해야
		 * 한다. */
		iput(inode);
		/* [한국어] inode 참조 카운트를 감소시켜, 마지막 참조라면
		 * bdev_free_inode() 까지 이어져 지금까지 초기화한 자원을
		 * 정리한다. */
		return NULL;
		/* [한국어] 호출자에게 할당 실패를 알린다. */
	}
	bdev->bd_disk = disk;
	/* [한국어] 이 bdev 가 속한 gendisk 로의 역참조를 설정 - 이후
	 * bdev->bd_disk 를 통해 디스크 이름, 파티션 테이블, request_queue
	 * 등 상위 정보에 접근 가능해진다. */
// NVMe: NVMe namespace 를 표현하는 gendisk 역참조.
	return bdev;
	/* [한국어] 초기화가 모두 끝난 block_device 를 호출자에게 반환.
	 * 이 시점까지는 아직 insert_inode_hash() 되지 않아 dev_t 로 조회
	 * 불가능한 "비공개" 상태이며, 공개는 이어지는 bdev_add() 가 담당. */
}

/*
 * bdev_set_nr_sectors - bdev 의 논리적 크기(섹터 수)를 설정.
 * NVMe 연결점: NVMe Identify Namespace 에서 보고한 NN/NLBAF 에 기반한
 *            총 LBA 수를 여기에 반영. 상위 파일시스템은 이 크기를 보고
 *            주소 범위를 제한한다.
 */
/*
 * [한국어]
 * bdev_set_nr_sectors - block_device 의 논리적 크기(섹터 수)를 설정.
 *
 * @bdev: 크기를 갱신할 대상 block_device. 디스크 전체일 수도, 파티션일
 *        수도 있다.
 * @sectors: 새 총 섹터 수(512바이트 단위 섹터, sector_t 타입 - 보통
 *           64비트). 예: NVMe 라면 Identify Namespace 응답의 NSZE(네임
 *           스페이스 크기, LBA 개수)에 LBA 크기/512 배수를 반영한 값.
 * @return: void.
 *
 * block_device 의 "크기"는 두 곳에 중복 저장된다 - bdev->bd_nr_sectors
 * (섹터 단위, 빠른 산술 비교용)와 그 inode 의 i_size(바이트 단위, VFS
 * generic 코드가 파일 크기처럼 다루기 위함). 두 값은 항상 서로의 배수
 * 관계(i_size = bd_nr_sectors << SECTOR_SHIFT)를 유지해야 하는데, 별도의
 * 두 번의 대입으로 나누어 하면 그 사이에 다른 CPU 가 "절반만 갱신된"
 * (예: i_size 는 새 값인데 bd_nr_sectors 는 아직 예전 값인) 상태를 읽어
 * 크기 불일치로 인한 버그(예: 파티션 경계를 넘는 I/O 허용)를 일으킬 수
 * 있다. 이 함수는 bd_size_lock 스핀락으로 두 갱신을 하나의 임계구역으로
 * 묶어 그런 tearing(찢어짐)을 막는다.
 *
 * 동작 순서:
 *  1) bd_size_lock 을 잡아 이 bdev 의 크기 필드에 대한 배타적 접근을
 *     확보.
 *  2) i_size_write() 로 inode 크기를 (섹터 수 << SECTOR_SHIFT, 즉 섹터당
 *     512바이트 변환) 바이트로 갱신.
 *  3) bd_nr_sectors 필드도 같은 락 아래에서 갱신.
 *  4) 락 해제.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(디스크 프로브, resize, revalidate 등).
 * bd_size_lock 은 spinlock 이므로 이 함수가 실행되는 동안(짧은 구간)
 * 다른 CPU 가 같은 락을 잡으려 하면 스핀 대기한다. 다른 CPU 에서 동시에
 * bdev_nr_sectors()(읽기 경로, 이 chunk 밖)가 같은 락을 잡고 읽으면 이
 * 함수가 끝날 때까지 대기한다.
 *
 * caller: 디스크 크기 변경을 감지하는 경로(예: set_capacity()/
 *         bdev_disk_changed(), NVMe 라면 네임스페이스 재검증 시).
 * callee: spin_lock()/spin_unlock(), i_size_write(), BD_INODE().
 * 에러 경로: 없음(void, 실패할 수 없는 단순 필드 갱신).
 *
 * 호출 체인:
 *   set_capacity()/디스크 재검증 경로 → [bdev_set_nr_sectors] →
 *   i_size_write()
 */

void bdev_set_nr_sectors(struct block_device *bdev, sector_t sectors)
{
	spin_lock(&bdev->bd_size_lock);
	/* [한국어] i_size 와 bd_nr_sectors 두 필드를 하나의 원자적 단위로
	 * 갱신하기 위해 배타적 구간 시작. 이 락이 없으면 리더(예: 파일시스템
	 * 이 파일 크기를 확인하는 코드)가 i_size 는 새 값, bd_nr_sectors 는
	 * 옛 값(또는 그 반대)인 불일치 스냅샷을 볼 수 있다. */
// NVMe: bd_nr_sectors 와 i_size 동시 갱신 시 race 방지.
	i_size_write(BD_INODE(bdev), (loff_t)sectors << SECTOR_SHIFT);
	/* [한국어] 섹터 수를 바이트 단위로 변환(SECTOR_SHIFT=9, 즉 섹터당
	 * 512바이트이므로 좌측 시프트 9비트 = *512)해 inode 의 크기 필드에
	 * 기록. i_size_write() 는 32비트 아키텍처에서 64비트 i_size 를 쓸
	 * 때의 seqcount 기반 tearing 방지까지 내부적으로 처리한다. */
// NVMe: Identify Namespace 의 총 LBA 수를 inode 크기로 반영.
	bdev->bd_nr_sectors = sectors;
	/* [한국어] 섹터 단위의 크기를 별도 필드에도 저장 - 블록 계층
	 * 코드(bio 범위 검사 등)가 바이트/섹터 변환 없이 빠르게 비교할 수
	 * 있도록 캐시된 값. */
// NVMe: 총 LBA 수. 파일시스템은 이 값으로 주소 범위 제한.
	spin_unlock(&bdev->bd_size_lock);
	/* [한국어] 임계구역 종료 - 이후 다른 CPU 의 리더/라이터가 락을 잡을
	 * 수 있다. */
}

/*
 * bdev_add - bdev 에 dev_t 를 부여하고 inode 해시에 삽입.
 * NVMe 연결점: /dev/nvme0n1 의 major/minor 가 inode->i_rdev 에 설정되어
 *            사용자 공간 open() 과 커널의 block_device 가 연결된다.
 */
/*
 * [한국어]
 * bdev_add - block_device 에 dev_t(major/minor)를 부여하고 inode 해시에
 *            등록해 시스템에 "공개"하는 함수.
 *
 * @bdev: bdev_alloc() 이 만든, 아직 dev_t 가 없는 block_device.
 * @dev: 이 bdev 에 부여할 dev_t(major<<20 | minor 형태로 인코딩된 장치
 *       번호). 파티션이면 디스크 전체와 같은 major 에 다른 minor.
 * @return: void.
 *
 * bdev_alloc() 은 메모리 상의 객체만 준비할 뿐 아직 어떤 dev_t 로도
 * 조회되지 않는 "비공개" 상태였다. bdev_add() 는 이 마지막 단계로,
 * dev_t 를 확정하고 inode 해시(전역 해시테이블, dev_t 를 키로 함)에
 * 넣어 이후 open(2) 시스템 호출이 이 dev_t 로 이 block_device 를
 * 찾아낼 수 있게 만든다 - 즉 사용자 공간의 /dev/nvme0n1 open() 이
 * 커널의 struct block_device 와 실제로 연결되는 지점이다.
 *
 * 동작 순서:
 *  1) BD_INODE() 로 이 bdev 를 감싸는 VFS inode 를 얻는다.
 *  2) bdev_stable_writes() 로 이 장치가 "stable writes"(쓰기 요청 제출
 *     이후 완료 보고 전까지 버퍼 내용이 변경되지 않아야 함 - 일부
 *     체크섬/무결성 보호 장치나 통합 캐시 장치가 요구)를 필요로 하는지
 *     검사하고, 필요하면 그 inode 의 페이지 캐시에 해당 속성을 설정.
 *  3) bd_dev(커널 내부용)와 inode->i_rdev(VFS/사용자 공간에 노출되는
 *     dev_t), inode->i_ino(inode 번호로도 dev_t 값을 재사용)를 모두
 *     같은 dev 값으로 채운다.
 *  4) insert_inode_hash() 로 전역 inode 해시에 삽입 - 이제 iget5_locked()
 *     류 조회가 이 dev_t 로 이 inode 를 찾을 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(디스크/파티션 등록 경로, add_disk()
 * 계열). 이 시점 이후로 다른 스레드가 이 dev_t 로 open() 을 시도할 수
 * 있으므로, 이 함수 호출 전에 bdev 의 다른 초기화(bdev_alloc())가 모두
 * 끝나 있어야 한다(공개 후 초기화 필드를 건드리면 경쟁 상태 위험).
 *
 * caller: add_disk()/파티션 추가 경로(이 chunk 밖)가 dev_t 를 확정한
 *         직후 호출.
 * callee: BD_INODE(), bdev_stable_writes(), mapping_set_stable_writes(),
 *         insert_inode_hash().
 * 에러 경로: 없음(void, 실패 없는 필드 대입 + 해시 삽입).
 *
 * 호출 체인:
 *   add_disk()/파티션 스캔 → [bdev_add] → insert_inode_hash()
 */

void bdev_add(struct block_device *bdev, dev_t dev)
{
	struct inode *inode = BD_INODE(bdev);
	/* [한국어] bdev 를 감싸는 VFS inode 를 얻는다(bdev_alloc 에서 이미
	 * 만든 것과 동일 객체, container_of 로 역산). */
	if (bdev_stable_writes(bdev))
		/* [한국어] 진입 조건: 이 디스크가 "안정된 쓰기"를 요구하는
		 * 경우(예: 일부 무결성 검사/통합 캐시 계층을 쓰는 디바이스는
		 * 쓰기 요청을 낸 뒤 완료 전에 버퍼 내용이 바뀌면 체크섬이
		 * 깨지므로 이를 방지해야 함). */
// NVMe: NVMe namespace 가 stable writes(쓰기 완료 보장)를 필요로 하면 플래그 설정.
		mapping_set_stable_writes(bdev->bd_mapping);
		/* [한국어] 이 bdev 의 페이지 캐시 address_space 에
		 * AS_STABLE_WRITES 플래그를 설정 - 이후 writeback 경로가
		 * dirty 페이지를 그대로 재사용하지 않고 필요 시 복사본을
		 * 만들어 제출하도록(또는 페이지 잠금을 유지하도록) 유도한다. */
// NVMe: stable writes 플래그는 NVMe flush/write cache 정책과 연결.
	bdev->bd_dev = dev;
	/* [한국어] 커널 내부에서 이 block_device 를 식별할 dev_t 를 저장. */
// NVMe: 커널 내부 dev_t 설정.
	inode->i_rdev = dev;
	/* [한국어] VFS 계층/사용자 공간에서 stat(2) 의 st_rdev 등으로 보이는
	 * 값으로도 같은 dev_t 를 저장 - open(2) 이 major/minor 로 이
	 * block_device 를 찾을 때 이 필드가 대응된다. */
// NVMe: 사용자 공간 major/minor 와 매핑.
	inode->i_ino = dev;
	/* [한국어] bdevfs 안에서는 inode 번호로 dev_t 값 자체를 재사용한다
	 * (일반 파일시스템처럼 별도 inode 번호 할당기를 두지 않고, dev_t 를
	 * 그대로 고유 키로 사용) - insert_inode_hash() 의 해시 키가 바로
	 * 이 i_ino 이다. */
// NVMe: inode 번호를 dev_t 로 설정.
	insert_inode_hash(inode);
	/* [한국어] 전역 inode 해시 테이블에 이 inode 를 삽입. 해시 버킷은
	 * i_ino(=dev_t) 기반으로 계산되며, 이후 bdev_open()/
	 * blkdev_get_by_dev() 류 경로의 ilookup5()/iget5_locked() 가 이
	 * 해시를 통해 동일 dev_t 에 대해 항상 같은 block_device 인스턴스를
	 * 재사용하도록 보장한다. */
// NVMe: inode 해시에 삽입하여 /dev/nvme0n1 경로 조회 가능.
}

/* bdev 를 inode 해시에서 제거. NVMe namespace 가 사라질 때 호출. */
/*
 * [한국어]
 * bdev_unhash - block_device 의 inode 를 전역 inode 해시에서 제거.
 *
 * @bdev: 해시에서 제거할 block_device. 대상 디스크/파티션이 시스템에서
 *        사라지는 중(예: NVMe 네임스페이스 삭제, 파티션 재스캔으로 인한
 *        파티션 소멸)이다.
 * @return: void.
 *
 * bdev_add() 가 insert_inode_hash() 로 이 bdev 를 조회 가능하게 만들었던
 * 것의 반대 동작이다. 디바이스가 제거되는 중에는 더 이상 새로운 open(2)
 * 이 이 dev_t 로 이 block_device 를 찾아서는 안 되므로, 제거 절차의
 * 이른 단계에서 해시부터 끊어 "더 이상 발견되지 않게" 만든다(실제 메모리
 * 해제는 마지막 참조가 사라질 때 bdev_free_inode() 가 담당 - 이 함수는
 * 조회 경로만 차단할 뿐 아직 살아있는 참조자들에게는 영향을 주지 않는다).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(del_gendisk() 등 디바이스 제거 경로,
 * 이 chunk 밖). 이 함수 자체는 별도 락을 잡지 않으며, remove_inode_hash()
 * 내부에서 전역 inode 해시 락(예: RCU 및 해시 버킷 스핀락)을 사용한다.
 *
 * caller: del_gendisk()/파티션 제거 경로.
 * callee: remove_inode_hash(), BD_INODE().
 * 에러 경로: 없음(void).
 *
 * 호출 체인:
 *   del_gendisk()/파티션 제거 → [bdev_unhash] → remove_inode_hash()
 */

void bdev_unhash(struct block_device *bdev)
{
	remove_inode_hash(BD_INODE(bdev));
	/* [한국어] 이 bdev 의 inode 를 전역 inode 해시 테이블에서 제거.
	 * 이후 ilookup5()/iget5_locked() 류 조회는 이 dev_t 에 대해 더
	 * 이상 이 inode 를 찾지 못하므로, 새로운 open(2) 시도는 "장치
	 * 없음" 류 에러로 실패하게 된다(이미 열려 있던 fd 는 이 함수와
	 * 무관하게 계속 유효). */
// NVMe: namespace 제거 시 inode 해시에서 제거. 이후 open 실패.
}

/* bdev 의 inode 참조를 해제. 참조 카운트가 0이면 bdev_free_inode 호출. */
/*
 * [한국어]
 * bdev_drop - block_device 가 들고 있던 inode 참조를 하나 반환.
 *
 * @bdev: 참조를 반환할 block_device. 디바이스 제거 절차의 최종 단계에서
 *        (보통 bdev_unhash() 이후) 호출되어, gendisk/디스크 생성 시
 *        bdev_alloc() 이 암묵적으로 만든 초기 참조를 되돌린다.
 * @return: void.
 *
 * struct block_device 의 수명은 결국 내장된 VFS inode 의 참조 카운트로
 * 관리된다. 이 함수는 iput() 을 호출해 그 카운트를 하나 감소시킬 뿐이며,
 * 실제 메모리 해제는 그 결과 참조 카운트가 0이 되었을 때만
 * bdev_free_inode()(bdev_sops.free_inode)가 트리거되어 이루어진다 - 즉
 * 여러 참조자(열려 있는 fd, 마운트된 파일시스템 등)가 아직 남아 있다면
 * 이 호출은 단지 카운트만 줄이고 조용히 반환한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. iput() 이 마지막 참조를 감지하면
 * 내부적으로 evict_inode() 등을 거쳐 결국 bdev_free_inode() 까지 호출될
 * 수 있으므로, 이 함수 호출 이후 bdev 포인터가 여전히 유효한지는
 * "다른 참조가 남아있었는가"에 달려 있다(제거 경로에서는 보통 이 호출을
 * 마지막으로 더 이상 bdev 를 참조하지 않도록 설계됨).
 *
 * caller: del_gendisk()/디스크 제거 경로(이 chunk 밖), 보통 bdev_unhash()
 *         직후.
 * callee: iput(), BD_INODE().
 * 에러 경로: 없음(void, iput() 은 실패하지 않는다).
 *
 * 호출 체인:
 *   del_gendisk() → bdev_unhash() → [bdev_drop] → iput() →
 *   (마지막 참조라면) bdev_free_inode()
 */

void bdev_drop(struct block_device *bdev)
{
	iput(BD_INODE(bdev));
	/* [한국어] 이 inode 의 참조 카운트를 1 감소. 결과가 0이 되면 VFS 가
	 * 내부적으로 evict 경로를 거쳐 sb->s_op->free_inode, 즉
	 * bdev_free_inode() 를 호출해 percpu 통계/메타데이터/gendisk/bdi/
	 * 확장 minor 번호까지 모두 정리하게 된다. 아직 다른 참조자가
	 * 있다면 이 호출은 카운트만 줄이고 아무 부수효과도 내지 않는다. */
// NVMe: inode 참조 해제. 마지막 참조면 bdev_free_inode -> gendisk/queue 정리.
}

/*
 * nr_blockdev_pages - 시스템 전체 bdev 페이지 캐시 페이지 수 합계.
 * NVMe 연결점: drop_caches 나 메모리 부족 시 NVMe SSD 의 캐시가 얼마나
 *            많은 시스템 메모리를 차지하는지 파악하는 데 사용.
 */
/*
 * [한국어]
 * nr_blockdev_pages - 시스템에 존재하는 모든 block_device 의 페이지
 *                     캐시 페이지 수를 합산해 반환.
 *
 * @return: 모든 bdev inode 의 i_mapping->nrpages 합(long). 페이지 캐시가
 *          비어 있으면 0.
 *
 * 커널은 "블록 장치 자체"(raw 디바이스, 예: 파일시스템을 통하지 않고
 * 직접 여는 /dev/nvme0n1)의 페이지 캐시가 전체 시스템 메모리에서 차지하는
 * 비중을 알아야 할 때가 있다(예: 메모리 회수 통계, drop_caches 계열
 * 진단). 이 함수는 blockdev_superblock 의 inode 리스트(bdevfs 에 속한
 * 모든 inode - 즉 모든 block_device)를 순회하며 각 inode 의 페이지 캐시
 * 페이지 수를 더한다.
 *
 * 동작 순서:
 *  1) s_inode_list_lock 스핀락으로 슈퍼블록의 s_inodes 리스트를 보호.
 *  2) list_for_each_entry() 로 리스트의 모든 inode 를 순회.
 *  3) 각 inode->i_mapping->nrpages(그 inode 의 페이지 캐시에 현재 들어
 *     있는 페이지 개수)를 누적.
 *  4) 락 해제 후 합계를 반환.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. s_inode_list_lock 은 스핀락이므로
 * 이 함수가 실행되는 동안 다른 CPU 가 blockdev_superblock 의 inode
 * 리스트에 삽입/제거(new_inode/evict)를 수행하려면 대기해야 한다 - 즉
 * 이 순회 도중 리스트가 변경되지 않음을 스핀락이 보장한다. 다만 각
 * inode->i_mapping->nrpages 자체는 이 락과 무관하게 계속 변할 수 있는
 * 카운터이므로, 반환값은 "그 순간의 근사치"이지 완벽한 스냅샷은 아니다.
 *
 * caller: 메모리 회수/통계 경로(예: /proc 인터페이스 또는 drop_caches
 *         구현, 이 chunk 밖).
 * callee: spin_lock()/spin_unlock(), list_for_each_entry().
 * 에러 경로: 없음(항상 성공, 실패할 수 없는 순수 집계 함수).
 *
 * 호출 체인:
 *   drop_caches/메모리 통계 경로 (이 chunk 밖) → [nr_blockdev_pages] →
 *   list_for_each_entry() 순회
 */

long nr_blockdev_pages(void)
{
	struct inode *inode;
	/* [한국어] list_for_each_entry() 순회 중 현재 항목을 담을 커서. */
	long ret = 0;
	/* [한국어] 누적 합계. 오버플로 방지를 위해 long(플랫폼 워드 크기)
	 * 사용 - 페이지 수가 매우 많은 대형 시스템에서도 32비트로는 부족할
	 * 수 있기 때문. */

	spin_lock(&blockdev_superblock->s_inode_list_lock);
	/* [한국어] blockdev_superblock->s_inodes 리스트를 순회하는 동안
	 * 다른 CPU 가 같은 리스트에 insert_inode_hash()/evict 등으로 노드를
	 * 추가/제거하지 못하게 막는다 - 링크드 리스트를 잠금 없이 순회하면
	 * use-after-free 나 무한루프(다른 스레드가 리스트를 변경하는 도중
	 * next 포인터를 따라가다 깨진 상태를 밟는 경우) 위험이 있다. */
// NVMe: 전체 bdev inode 리스트 보호.
	list_for_each_entry(inode, &blockdev_superblock->s_inodes, i_sb_list)
		/* [한국어] 이 슈퍼블록(bdevfs)에 속한 모든 inode, 즉 시스템에
		 * 존재하는 모든 block_device 를 하나씩 순회. i_sb_list 는
		 * inode 를 자신이 속한 슈퍼블록의 리스트에 연결하는
		 * list_head 필드. */
// NVMe: 시스템에 등록된 모든 NVMe namespace 의 inode 를 순회.
		ret += inode->i_mapping->nrpages;
		/* [한국어] 이 inode(=이 block_device)의 페이지 캐시에 현재
		 * 상주하는 페이지 개수를 누적 합계에 더한다. i_mapping 은
		 * bdev_alloc() 에서 bd_mapping 과 함께 같은 address_space 를
		 * 가리키도록 설정되었던 그 필드다. */
// NVMe: 각 namespace 의 페이지 캐시 페이지 수를 합산.
	spin_unlock(&blockdev_superblock->s_inode_list_lock);
	/* [한국어] 순회가 끝났으므로 락 해제 - 이후 다른 CPU 가 리스트를
	 * 변경할 수 있다. */

	return ret;
	/* [한국어] 집계된 총 페이지 수를 호출자에게 반환. */
}

/*
 * [한국어]
 * bd_may_claim - block_device를 @holder가 exclusive(배타적)로 claim할 수 있는지 검사
 *
 * @bdev: claim 가능 여부를 검사할 대상 block_device. 파티션 디바이스일 수도 있고
 *        전체(whole) 디스크 디바이스일 수도 있다.
 * @holder: claim을 시도하는 주체를 식별하는 opaque(불투명) 포인터. 파일시스템
 *          super_block, device-mapper 타겟, loop 디바이스 구조체 등 호출자가
 *          자신을 식별할 수 있는 아무 포인터나 넘길 수 있으며, bdev->bd_holder에
 *          그대로 저장되어 이후 "누가 이 디바이스를 잡고 있는가"를 비교하는 키로 쓰인다.
 * @hops: holder가 등록하는 콜백 테이블(struct blk_holder_ops *). 미디어 제거 통지 등
 *        exclusive open 중인 holder에게 이벤트를 알려줄 때 사용된다.
 * @return: true면 @holder가 @bdev를 claim할 수 있다(자기 자신이 이미 claim한 경우
 *          재claim 허용 포함). false면 다른 holder가 이미 배타적으로 사용 중이므로
 *          claim이 거부되어야 함을 의미하며, 호출자인 bd_prepare_to_claim()이 이를
 *          -EBUSY로 변환해 상위로 전파한다.
 *
 * exclusive claim은 "이 블록 디바이스를 나만 열어서 쓰겠다"는 상호배제 계약이다.
 * 예를 들어 마운트된 파일시스템이 있는 디바이스를 다른 프로세스가 mkfs로 덮어쓰는
 * 사고를 막으려면, 파일시스템이 그 디바이스를 exclusive하게 claim해 두고 다른
 * holder의 claim 시도를 전부 거부해야 한다. 이 함수는 그 "거부해야 하는가"를
 * 순수하게 판정만 하는 술어(predicate) 함수이며, 실제 상태 변경(카운트 증가 등)은
 * bd_finish_claiming()이 담당한다.
 *
 * 판정 순서: (1) bdev->bd_holder가 이미 설정되어 있으면, 그것이 @holder 자신인지
 * 검사해 자기 자신이면 재claim을 허용(단 hops가 최초 등록 시와 달라졌다면 버그이므로
 * WARN_ON_ONCE 후 거부), 다른 누군가면 거부한다. (2) bdev->bd_holder가 비어 있어도,
 * @bdev가 파티션이라면 그 파티션이 속한 whole 디바이스가 "진짜 holder"(즉
 * bd_may_claim 자기 자신이 아닌 다른 값)에 의해 이미 claim되어 있는지 추가로
 * 검사한다 — whole 디바이스 전체가 다른 누군가에게 배타적으로 잡혀 있다면 그
 * 하위 파티션 하나만 별도로 claim하는 것은 허용하지 않는다는 규칙이다. 여기서
 * whole->bd_holder == bd_may_claim (이 함수 자신의 주소)이라는 특수 값은
 * "claim 절차가 진행 중이라 whole->bd_holders는 증가했지만 whole 자체를
 * 실제로 쓰는 holder는 아직 없다"는 임시 sentinel 상태를 뜻하므로 이 경우는
 * 예외적으로 파티션 claim을 막지 않는다(자세한 설정 지점은 bd_finish_claiming 참고).
 *
 * 실행 컨텍스트: 호출자가 bdev_lock 뮤텍스를 이미 들고 호출해야 한다
 * (lockdep_assert_held로 강제). 이 함수 자체는 상태를 변경하지 않고 읽기만
 * 하므로 재진입 안전하지만, bdev->bd_holder 등의 필드가 bdev_lock으로
 * 보호되는 값이라 락 없이 호출하면 다른 CPU의 동시 claim/release와 경쟁한다.
 * 호출자: bd_prepare_to_claim() (claim 시도 시 가능 여부 사전 검사),
 *         bd_finish_claiming() (claim 확정 직전 BUG_ON으로 불변조건 재확인).
 * 호출 대상: bdev_whole() (whole 디바이스 포인터 획득) 외에는 순수 필드 비교뿐.
 * 에러 처리: 이 함수는 에러코드를 반환하지 않고 bool 판정만 반환한다. false를
 *           받은 호출자가 각자 -EBUSY 등으로 변환해 처리한다.
 *
 * 호출 체인:
 *   bd_prepare_to_claim() / bd_finish_claiming() → [bd_may_claim] → bdev_whole()
 */

/**
 * bd_may_claim - test whether a block device can be claimed
 * @bdev: block device of interest
 * @holder: holder trying to claim @bdev
 * @hops: holder ops
 *
 * Test whether @bdev can be claimed by @holder.
 *
 * RETURNS:
 * %true if @bdev can be claimed, %false otherwise.
 */
static bool bd_may_claim(struct block_device *bdev, void *holder,
		const struct blk_holder_ops *hops)
{
	struct block_device *whole = bdev_whole(bdev);
	/* [한국어] @bdev가 파티션이면 그 파티션이 속한 전체(whole) 디스크 block_device를
	 * 얻는다. @bdev 자체가 이미 whole 디바이스면 bdev_whole()은 자기 자신을 반환한다.
	 * whole 디바이스 기준으로 claim 상태를 관리해야 "전체 디스크가 다른 holder에게
	 * 통째로 잡혀 있는지" 여부를 파티션 여러 개에 걸쳐 일관되게 판단할 수 있다. */

	lockdep_assert_held(&bdev_lock);
	/* [한국어] 런타임 lockdep 검사: 이 함수를 호출하는 시점에 반드시 bdev_lock
	 * 뮤텍스를 이미 보유하고 있어야 한다는 것을 표명한다. bd_holder/bd_claiming
	 * 필드들이 bdev_lock으로 보호되므로, 락 없이 이 함수를 호출하면 다른 CPU의
	 * bd_prepare_to_claim()/bd_finish_claiming()과 경쟁해 잘못된 판정을 내릴 수
	 * 있다. CONFIG_PROVE_LOCKING이 켜져 있을 때만 실제로 체크되며, 락이 없으면
	 * "possible unsafe locking" 형태의 커널 경고를 발생시킨다. */

	if (bdev->bd_holder) {
		/* [한국어] bdev->bd_holder가 NULL이 아니라는 것은 이미 어떤 holder가
		 * 이 block_device(파티션이든 whole이든)를 exclusive claim한 상태라는
		 * 뜻이다. 이 분기 안에서 "그 holder가 지금 claim을 시도하는 @holder와
		 * 같은가"를 판정해 재claim 허용 여부를 가른다. */
		/*
		 * The same holder can always re-claim.
		 */
		if (bdev->bd_holder == holder) {
			/* [한국어] 이미 등록된 holder와 지금 claim을 요청한 holder가
			 * 동일한 포인터라면, 같은 주체가 같은 디바이스를 한 번 더
			 * claim하려는 것이므로 거부할 이유가 없다. */
			if (WARN_ON_ONCE(bdev->bd_holder_ops != hops))
				/* [한국어] 같은 holder인데 넘어온 hops(콜백 테이블)가
				 * 최초 등록 때와 다른 포인터라면 이는 호출자 코드의
				 * 버그다 — 한 holder는 생애주기 동안 하나의 hops만
				 * 사용해야 한다는 불변조건이 깨진 것이므로 WARN_ON_ONCE로
				 * 커널 로그에 스택트레이스를 남기고 claim을 거부한다. */
				return false;
			return true;
			/* [한국어] hops가 일치하면 재claim을 허용 — bd_holders
			 * 카운트는 이 반환 이후 bd_finish_claiming()에서 증가된다. */
		}
		return false;
		/* [한국어] holder가 다르면 이미 다른 주체가 배타적으로 사용 중이므로
		 * 무조건 거부 — 호출자는 이를 -EBUSY로 변환해 반환한다. */
	}

	/*
	 * If the whole devices holder is set to bd_may_claim, a partition on
	 * the device is claimed, but not the whole device.
	 */
	if (whole != bdev &&
	    whole->bd_holder && whole->bd_holder != bd_may_claim)
		/* [한국어] @bdev가 파티션(whole != bdev)이고, 그 전체 디스크
		 * whole->bd_holder가 설정되어 있으며, 그 값이 bd_may_claim 함수
		 * 자기 자신의 주소(=claim 진행 중을 나타내는 sentinel)가 아니라면,
		 * 이는 "전체 디스크가 이미 다른 진짜 holder에게 exclusive로 잡혀
		 * 있다"는 뜻이다. 이 경우 그 위의 개별 파티션 하나만 별도로 다른
		 * holder가 claim하는 것은 허용하지 않는다 — 전체 디스크를 배타적으로
		 * 쓰는 주체가 있으면 그 아래 파티션도 함께 보호되어야 하기 때문이다.
		 * 반대로 whole->bd_holder == bd_may_claim이면 이는 "다른 파티션에
		 * 대한 claim 절차가 진행 중이라 whole 카운트만 증가했고 whole 자체의
		 * 진짜 holder는 없다"는 임시 상태이므로 이 파티션의 claim은 막지 않는다. */
		return false;
	return true;
	/* [한국어] 위의 모든 거부 조건에 해당하지 않으면 claim 가능 — 아무도 이
	 * block_device(및 그 whole 디바이스)를 배타적으로 쓰고 있지 않다는 뜻이다. */
}

/*
 * [한국어]
 * bd_prepare_to_claim - block_device에 대한 exclusive claim을 시도한다
 *
 * @bdev: claim을 시도할 대상 block_device (whole 디스크 또는 파티션).
 * @holder: claim 주체를 식별하는 opaque 포인터. NULL은 허용되지 않는다
 *          (claim은 반드시 누군가의 이름으로 이루어져야 하기 때문).
 * @hops: holder가 등록하는 블록 디바이스 이벤트 콜백 테이블.
 * @return: 0이면 claim 준비 성공 — 호출자는 이제 bd_claiming과 bd_holder[s]에
 *          대한 소유권을 가지며, 곧이어 bd_finish_claiming()으로 확정하거나
 *          실패 시 bd_abort_claiming()으로 되돌려야 한다. -EBUSY면 이미 다른
 *          holder가 claim 중이라 이번 시도는 실패한 것이고, -EINVAL이면
 *          @holder가 NULL로 호출된 프로그래밍 오류다.
 *
 * exclusive open(예: O_EXCL 플래그, 또는 파일시스템 마운트 시 내부적으로 요구하는
 * 배타 접근)을 구현하기 위한 핵심 진입점이다. 여러 프로세스/스레드가 동시에 같은
 * 디바이스를 exclusive claim하려고 경쟁할 수 있으므로, 이 함수는 단순히 한 번
 * 검사하고 끝내는 것이 아니라 "지금 다른 누군가가 claim 절차를 진행 중이면
 * 그 절차가 끝날 때까지 기다렸다가 다시 시도"하는 재시도(retry) 루프로 동작한다.
 *
 * 동작 과정: (1) holder가 NULL이면 즉시 -EINVAL. (2) retry 레이블로 진입해
 * bdev_lock을 잡고 bd_may_claim()으로 claim 가능 여부를 검사 — 불가능하면 락을
 * 풀고 -EBUSY 반환. (3) 가능하더라도 whole->bd_claiming이 이미 다른 주체에 의해
 * 설정되어 있다면(=다른 스레드가 지금 막 claim 절차를 진행 중) 이 스레드는
 * __var_waitqueue()로 얻은 대기열에 스스로를 등록하고 TASK_UNINTERRUPTIBLE
 * 상태로 잠든다. bdev_lock을 풀고 schedule()로 CPU를 양보한 뒤, 깨어나면
 * finish_wait()로 대기열에서 스스로를 제거하고 retry로 되돌아가 처음부터
 * 다시 검사한다. (4) 아무도 claim 진행 중이 아니면 whole->bd_claiming = holder로
 * 설정해 "이제 내가 claim 절차를 진행 중"임을 표시하고 성공(0)을 반환한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트에서만 호출 가능(schedule()로 잠들 수 있으므로
 * 인터럽트/원자적 컨텍스트에서는 호출 불가). 호출 시점에는 bdev_lock을 들고
 * 있지 않아야 하며, 반환 시점에도 bdev_lock은 풀려 있다(성공/실패 모두). 여러
 * 스레드가 동시에 호출해도 안전하도록 bdev_lock으로 bd_may_claim 검사와
 * bd_claiming 설정을 원자적으로 묶는다.
 * 호출자: bdev_open()/blkdev_get_by_dev() 등 exclusive open을 요청하는 상위 경로,
 *         그리고 EXPORT_SYMBOL_GPL로 내보내져 loop 드라이버(loop_configure() 등)가
 *         직접 호출한다(주석에 명시된 대로 loop 드라이버 전용 용도로 export됨).
 * 호출 대상: bdev_whole(), bd_may_claim(), __var_waitqueue(), prepare_to_wait(),
 *           schedule(), finish_wait().
 * 에러 처리: holder가 NULL이면 -EINVAL, 이미 다른 holder가 claim 중이면 -EBUSY.
 *           claim 진행 중 경쟁은 에러가 아니라 대기 후 재시도로 처리된다.
 *
 * 호출 체인:
 *   bdev_open()/blkdev_get_by_dev() 또는 loop 드라이버 → [bd_prepare_to_claim] → bd_may_claim() / schedule()
 */

/**
 * bd_prepare_to_claim - claim a block device
 * @bdev: block device of interest
 * @holder: holder trying to claim @bdev
 * @hops: holder ops.
 *
 * Claim @bdev.  This function fails if @bdev is already claimed by another
 * holder and waits if another claiming is in progress. return, the caller
 * has ownership of bd_claiming and bd_holder[s].
 *
 * RETURNS:
 * 0 if @bdev can be claimed, -EBUSY otherwise.
 */
int bd_prepare_to_claim(struct block_device *bdev, void *holder,
		const struct blk_holder_ops *hops)
{
	struct block_device *whole = bdev_whole(bdev);
	/* [한국어] 파티션이면 그 전체 디스크 block_device를 얻는다. claim 진행 상태
	 * (bd_claiming)와 대기열은 whole 디바이스 단위로 관리되므로, 파티션을
	 * claim하려는 요청도 결국 whole 기준으로 경쟁/대기 여부를 판단해야 한다. */

	if (WARN_ON_ONCE(!holder))
		/* [한국어] holder가 NULL인 채로 exclusive claim을 시도하는 것은
		 * 호출자 쪽의 프로그래밍 오류다 — claim한 주체를 식별할 수 없으면
		 * 나중에 bd_end_claim()에서 누가 반납하는지도 알 수 없게 된다.
		 * WARN_ON_ONCE로 커널 로그에 스택트레이스를 남기고 즉시 실패시킨다. */
		return -EINVAL;
retry:
	/* [한국어] claim 진행 경쟁으로 대기했다가 깨어난 뒤 다시 처음부터 검사하기
	 * 위한 재시도 지점. 아래쪽 goto retry의 목적지가 되는 레이블. */
	mutex_lock(&bdev_lock);
	/* [한국어] bd_holder/bd_holders/bd_claiming 필드들을 보호하는 전역 뮤텍스를
	 * 획득 — 이 뮤텍스를 잡아야 bd_may_claim()의 판정과 이어지는 bd_claiming
	 * 설정이 다른 CPU의 동시 claim 시도와 경쟁하지 않고 원자적으로 이루어진다. */
	/* if someone else claimed, fail */
	if (!bd_may_claim(bdev, holder, hops)) {
		/* [한국어] 이미 다른 holder가 이 디바이스(또는 그 whole 디바이스)를
		 * 배타적으로 잡고 있어 claim이 불가능하다고 판정된 경우. */
		mutex_unlock(&bdev_lock);
		/* [한국어] 실패로 반환하기 전에 반드시 뮤텍스를 풀어야 한다 —
		 * 그렇지 않으면 다른 스레드가 영원히 bdev_lock을 기다리게 된다. */
		return -EBUSY;
		/* [한국어] 이미 다른 holder가 exclusive claim 중이므로 이번 시도는
		 * 실패 — 호출자(예: exclusive open 경로)는 이 -EBUSY를 그대로
		 * 사용자 공간에 반환하거나 open 실패로 처리한다. */
	}

	/* if claiming is already in progress, wait for it to finish */
	if (whole->bd_claiming) {
		/* [한국어] bd_may_claim()은 통과했지만(=최종 holder 충돌은 없음),
		 * 지금 다른 스레드가 이 whole 디바이스에 대해 claim 절차를 진행
		 * 중(bd_prepare_to_claim의 앞선 호출이 아직 bd_finish_claiming/
		 * bd_abort_claiming으로 마무리되지 않은 상태)이다. 이 경쟁을
		 * 해결하기 위해 그 절차가 끝날 때까지 잠들어 기다린다. */
		wait_queue_head_t *wq = __var_waitqueue(&whole->bd_claiming);
		/* [한국어] whole->bd_claiming 필드의 주소를 해시해 전역 waitqueue
		 * 테이블에서 대응하는 대기열을 얻는다 — 필드마다 별도의 waitqueue를
		 * 두지 않고 주소 기반 해시 테이블을 공유해 메모리를 절약하는
		 * "var waitqueue" 기법(wait_var_event류 API의 기반)이다. */
		DEFINE_WAIT(wait);
		/* [한국어] 이 스레드를 위한 struct wait_queue_entry를 스택에
		 * 선언하고 초기화 — 아래 prepare_to_wait()로 wq에 등록된다. */

		prepare_to_wait(wq, &wait, TASK_UNINTERRUPTIBLE);
		/* [한국어] wait 엔트리를 대기열 wq에 추가하고 현재 태스크 상태를
		 * TASK_UNINTERRUPTIBLE로 바꾼다 — 시그널로 깨어나지 않도록 해
		 * claim 대기 도중 어중간하게 깨어나 상태 기계가 꼬이는 것을 막는다.
		 * 아직 실제로 스케줄 아웃되지는 않고, 상태만 바뀐 채로 스케줄
		 * 시점을 기다린다. */
		mutex_unlock(&bdev_lock);
		/* [한국어] 잠들기 전에 반드시 bdev_lock을 풀어야 한다 — 그렇지
		 * 않으면 claim을 마무리하려는(bd_finish_claiming/bd_abort_claiming)
		 * 다른 스레드가 이 락을 못 잡아 영원히 깨워줄 수 없는 데드락이
		 * 발생한다. */
		schedule();
		/* [한국어] CPU를 다른 태스크에게 양보하고 실제로 잠든다 — 앞서
		 * TASK_UNINTERRUPTIBLE로 표시했으므로 claim을 마친 쪽이
		 * wake_up_var(&whole->bd_claiming)(bd_clear_claiming() 참고)를
		 * 호출해줄 때까지 깨어나지 않는다. */
		finish_wait(wq, &wait);
		/* [한국어] 깨어난 뒤 wait 엔트리를 대기열 wq에서 제거하고 태스크
		 * 상태를 TASK_RUNNING으로 되돌린다 — 정리하지 않으면 다음 번
		 * 대기 시 중복 등록되거나 이미 사라진 스택 변수를 참조하게 된다. */
		goto retry;
		/* [한국어] claim 진행 상태가 바뀌었으니(다른 쪽이 끝났거나 다른
		 * holder로 확정됐거나) 처음부터(bd_may_claim 재검사부터) 다시
		 * 시도한다 — 깨어났다고 무조건 claim 가능하다고 가정하지 않고
		 * 반드시 재검증한다(spurious wakeup 및 경쟁 재확인을 위함). */
	}

	/* yay, all mine */
	whole->bd_claiming = holder;
	/* [한국어] 아무도 claim 진행 중이 아님을 확인했으므로, 이 스레드가 지금부터
	 * claim 절차를 진행한다는 것을 whole->bd_claiming에 표시한다 — 이후 다른
	 * 스레드가 같은 whole 디바이스를 claim하려 하면 위 if(whole->bd_claiming)
	 * 분기에서 이 값을 보고 대기하게 된다. 이 필드는 bd_finish_claiming() 또는
	 * bd_abort_claiming()이 bd_clear_claiming()을 통해 NULL로 되돌릴 때까지
	 * 유지된다. */
	mutex_unlock(&bdev_lock);
	/* [한국어] bd_claiming 설정을 마쳤으니 락을 반납 — 이 시점부터 호출자는
	 * bdev_lock 없이도 bd_claiming에 대한 "소유권"을 갖는다(자신만 클리어할
	 * 수 있는 값이 되었으므로). */
	return 0;
	/* [한국어] claim 준비 성공. 호출자는 이제 실제 open 작업을 수행한 뒤
	 * bd_finish_claiming()으로 확정하거나, 실패 시 bd_abort_claiming()으로
	 * bd_claiming을 반드시 되돌려야 한다(안 그러면 다른 스레드가 영원히 대기). */
}
EXPORT_SYMBOL_GPL(bd_prepare_to_claim); /* only for the loop driver */
/* [한국어] 이 심볼을 GPL 라이선스 모듈에 한해 외부로 내보낸다 — 원본 주석대로
 * 실제로는 loop 블록 드라이버가 loop 디바이스를 backing file에 연결할 때
 * exclusive claim을 직접 수행하기 위해 사용하는 용도로만 export되어 있다. */

/*
 * [한국어]
 * bd_clear_claiming - claim 진행 중 표시를 해제하고 대기 중인 스레드들을 깨운다
 *
 * @whole: claim 진행 표시(bd_claiming)를 갖고 있는 전체(whole) 디스크 block_device.
 *         파티션이 아니라 항상 whole 디바이스여야 한다(bd_claiming은 whole에만
 *         의미 있는 필드).
 * @holder: 지금 claim 절차를 마무리하는 주체 — whole->bd_claiming에 저장된 값과
 *          반드시 일치해야 한다.
 *
 * bd_prepare_to_claim()이 whole->bd_claiming = holder로 표시해 둔 "진행 중" 상태를
 * 정상적으로 마무리(성공 확정 또는 포기)할 때 공통으로 쓰이는 내부 헬퍼다.
 * bd_finish_claiming()(성공 경로)과 bd_abort_claiming()(포기 경로) 양쪽에서
 * 호출되어 코드 중복을 없앤다. 이 필드를 기다리며 잠들어 있는 다른 스레드들
 * (bd_prepare_to_claim의 schedule() 대기 지점)을 wake_up_var()로 깨워, 그들이
 * retry 레이블로 돌아가 claim 상태를 다시 검사하게 만든다.
 *
 * 실행 컨텍스트: 호출자가 bdev_lock을 이미 들고 있어야 한다(lockdep_assert_held로
 * 강제). bd_claiming 필드에 대한 쓰기가 bdev_lock으로 직렬화되어야 다른 스레드의
 * bd_prepare_to_claim() 재시도 루프와 경쟁하지 않는다.
 * 호출자: bd_finish_claiming() (claim 성공 확정 시), bd_abort_claiming()
 *         (claim 포기 시).
 * 호출 대상: wake_up_var() (var waitqueue 기반 대기자 깨우기).
 * 에러 처리: whole->bd_claiming이 @holder와 다르면 BUG_ON으로 커널을 즉시
 *           패닉시킨다 — claim 소유권 추적 로직 자체의 불변조건이 깨진
 *           것이라 복구 불가능한 버그로 취급한다.
 *
 * 호출 체인:
 *   bd_finish_claiming() / bd_abort_claiming() → [bd_clear_claiming] → wake_up_var()
 */
static void bd_clear_claiming(struct block_device *whole, void *holder)
{
	lockdep_assert_held(&bdev_lock);
	/* [한국어] bd_claiming 필드를 수정하기 전, 호출자가 반드시 bdev_lock을
	 * 들고 있어야 한다는 것을 런타임에 검증 — 락 없이 호출되면 다른 CPU가
	 * 동시에 bd_claiming을 읽고 있는 bd_prepare_to_claim()과 경쟁한다. */
	/* tell others that we're done */
	BUG_ON(whole->bd_claiming != holder);
	/* [한국어] 지금 claim 절차를 끝내겠다는 holder가, 애초에 bd_claiming에
	 * 등록되어 있던 holder와 다르면 claim 소유권 추적이 어딘가에서 잘못된
	 * 것이다 — 서로 다른 두 holder가 동시에 자신이 claim 진행자라고 착각하는
	 * 상황은 데이터 무결성을 해칠 수 있으므로, 복구를 시도하지 않고 BUG_ON으로
	 * 커널을 즉시 중단시켜 조기에 발견한다. */
	whole->bd_claiming = NULL;
	/* [한국어] claim 진행 중 표시를 해제 — 이제 이 whole 디바이스에 대해
	 * 새로운 bd_prepare_to_claim() 호출이 (다른 조건이 맞으면) 바로 진행할
	 * 수 있는 상태가 된다. */
	wake_up_var(&whole->bd_claiming);
	/* [한국어] &whole->bd_claiming 주소를 키로 하는 var waitqueue에서 잠자고
	 * 있는 모든 대기자(bd_prepare_to_claim의 schedule() 지점)를 깨운다 —
	 * 깨어난 스레드들은 finish_wait() 후 retry로 돌아가 claim 상태를 처음부터
	 * 다시 검사한다(이 함수가 값을 NULL로 바꾸긴 했지만, 최종적으로 claim이
	 * 가능한지는 bd_may_claim()이 다시 판단한다). */
}

/*
 * [한국어]
 * bd_finish_claiming - exclusive claim을 확정하고 holder를 정식으로 등록한다
 *
 * @bdev: claim이 확정될 block_device (whole 또는 파티션).
 * @holder: claim을 확정하는 주체 — bd_prepare_to_claim()에 넘겼던 것과 동일한 포인터.
 * @hops: 이 holder가 사용할 이벤트 콜백 테이블. bdev->bd_holder_ops에 등록된다.
 * @return: 없음(void). 이 함수는 bd_prepare_to_claim()이 이미 -EBUSY를 걸러낸
 *          뒤에만 호출된다는 전제하에 동작하므로 실패를 반환하지 않는다.
 *
 * bd_prepare_to_claim()으로 "진행 중" 표시만 해 둔 claim을, 실제 open 작업
 * (blkdev_get_whole() 등)이 성공적으로 끝난 뒤 최종적으로 확정하는 함수다.
 * 확정 단계에서는 (1) whole->bd_holders와 bdev->bd_holders를 모두 증가시키고
 * (파티션을 claim해도 그 파티션이 속한 whole 디스크의 holder 카운트도 함께
 * 올라간다 — "이 물리 디스크를 쓰고 있는 holder가 최소 하나 있다"는 사실을
 * whole 관점에서도 추적해야 하기 때문에 카운트가 이중으로 올라간다),
 * (2) whole->bd_holder를 bd_may_claim 함수 주소라는 특수 sentinel 값으로
 * 잠시 설정해 두는데, 이는 "whole 자체를 실제로 연 holder는 아직 없지만
 * 그 하위 어떤 파티션은 claim되어 있다"는 중간 상태를 bd_may_claim()이
 * 구분할 수 있게 하기 위함이다(@bdev가 파티션인 경우 whole->bd_holder는
 * 실제 holder 값이 아니라 이 sentinel로 유지된다), (3) 마지막으로
 * bdev->bd_holder_lock으로 보호되는 bdev->bd_holder/bd_holder_ops 두 필드에
 * 실제 holder와 hops를 등록한다 — 이 두 필드는 bdev_lock보다 더 세분화된
 * 락(bd_holder_lock)으로 보호되는데, 이벤트 통지 콜백 호출부처럼 더 자주
 * 실행되는 경로에서 굳이 전역 bdev_lock을 잡지 않고도 holder/hops만 안전하게
 * 조회할 수 있게 하기 위한 세분화(lock splitting)다.
 *
 * 실행 컨텍스트: 호출 전에는 bdev_lock을 들고 있지 않아야 한다(이 함수 내부에서
 * 직접 mutex_lock(&bdev_lock)을 호출). 호출자는 반드시 bd_prepare_to_claim()이
 * 0을 반환한 직후, 그리고 실제 open이 성공한 뒤에만 이 함수를 호출해야 한다.
 * 호출자: blkdev_get_by_dev()/blkdev_get_by_path() 등 exclusive open 성공 경로.
 * 호출 대상: bdev_whole(), bd_may_claim()(BUG_ON을 통한 불변조건 재확인),
 *           bd_clear_claiming().
 * 에러 처리: bd_may_claim()이 false를 반환하면(있어서는 안 되는 상황) BUG_ON으로
 *           즉시 커널을 중단시킨다 — bd_prepare_to_claim() 성공 이후 이 함수
 *           호출 사이에 claim 조건이 깨졌다는 것은 락 프로토콜 위반이기 때문이다.
 *
 * 호출 체인:
 *   blkdev_get_by_dev()/blkdev_get_by_path() (exclusive open 성공 경로) → [bd_finish_claiming] → bd_clear_claiming()
 */

/**
 * bd_finish_claiming - finish claiming of a block device
 * @bdev: block device of interest
 * @holder: holder that has claimed @bdev
 * @hops: block device holder operations
 *
 * Finish exclusive open of a block device. Mark the device as exlusively
 * open by the holder and wake up all waiters for exclusive open to finish.
 */
static void bd_finish_claiming(struct block_device *bdev, void *holder,
		const struct blk_holder_ops *hops)
{
	struct block_device *whole = bdev_whole(bdev);
	/* [한국어] @bdev가 파티션이면 그 전체 디스크를, whole이면 자기 자신을 얻는다.
	 * 아래에서 whole 기준 카운트(whole->bd_holders)와 whole->bd_holder sentinel을
	 * 함께 갱신해야 하므로 미리 확보해 둔다. */

	mutex_lock(&bdev_lock);
	/* [한국어] bd_holder/bd_holders/bd_claiming 필드를 보호하는 전역 뮤텍스 획득 —
	 * claim 확정 과정 전체를 다른 스레드의 claim 시도/해제와 원자적으로 만든다. */
	BUG_ON(!bd_may_claim(bdev, holder, hops));
	/* [한국어] bd_prepare_to_claim()이 성공했다면 이 시점에도 claim이 여전히
	 * 가능해야 한다는 불변조건을 재확인한다 — bdev_lock을 계속 들고 있었다면
	 * 이 검사는 항상 참이어야 하므로, 실패한다면 그 사이 락 없이 상태가
	 * 바뀌었다는 뜻이라 복구 불가능한 버그로 보고 커널을 중단시킨다. */
	/*
	 * Note that for a whole device bd_holders will be incremented twice,
	 * and bd_holder will be set to bd_may_claim before being set to holder
	 */
	whole->bd_holders++;
	/* [한국어] 전체 디스크 관점의 holder 카운트를 증가 — @bdev가 파티션이든
	 * whole이든, "이 물리 디스크의 어느 한 부분을 배타적으로 쓰는 holder가
	 * 하나 늘었다"는 사실을 whole 단위로도 집계해야 나중에 whole 자체를
	 * 열거나 파티션 구성을 재확인할 때 충돌 여부를 판단할 수 있다. */
	whole->bd_holder = bd_may_claim;
	/* [한국어] whole->bd_holder를 실제 holder 포인터가 아니라 이 파일의
	 * static 함수 bd_may_claim의 코드 주소로 설정한다 — 이는 "whole 자체를
	 * 연 holder는 없지만 그 하위 파티션 하나가 claim되어 있다"는 중간 상태를
	 * 나타내는 sentinel 값이다. bd_may_claim() 안에서
	 * "whole->bd_holder != bd_may_claim"으로 이 sentinel을 구분해, 진짜
	 * holder가 있는 경우에만 파티션 claim을 추가로 막는다. @bdev 자체가
	 * whole이라면 바로 아래에서 bdev->bd_holder = holder로 실제 값으로
	 * 덮어써지므로 이 sentinel은 그 경우 순간적으로만 존재한다. */
	bdev->bd_holders++;
	/* [한국어] @bdev(파티션 또는 whole) 자신의 holder 카운트를 증가 — whole 쪽
	 * 카운트(whole->bd_holders)와는 별개의 값으로, "이 특정 block_device를
	 * 직접 claim한 holder 수"를 추적한다. @bdev가 whole이면 whole->bd_holders와
	 * bdev->bd_holders가 같은 필드이므로 결과적으로 두 번 증가한 것처럼 보이는데,
	 * 이것이 바로 위 원문 주석이 설명하는 "whole 디바이스는 두 번 증가한다"는
	 * 동작이다. */
	mutex_lock(&bdev->bd_holder_lock);
	/* [한국어] bd_holder/bd_holder_ops 두 필드만 보호하는 세분화된(per-bdev) 락을
	 * 추가로 획득 — 이 락은 bdev_lock보다 더 가볍고 자주 호출되는 조회 경로
	 * (예: 이벤트 통지 시 holder 콜백을 부를 때)에서 전역 락을 거치지 않고
	 * holder/hops를 읽을 수 있게 하기 위한 세분화다. */
	bdev->bd_holder = holder;
	/* [한국어] 이 시점에 비로소 실제 holder 포인터를 등록 — @bdev가 whole이라면
	 * 바로 위에서 설정했던 bd_may_claim sentinel을 진짜 holder 값으로 덮어쓴다. */
	bdev->bd_holder_ops = hops;
	/* [한국어] holder가 등록한 이벤트 콜백 테이블을 저장 — 이후 미디어 변경 등
	 * 이벤트가 발생하면 이 hops를 통해 holder에게 통지된다. */
	mutex_unlock(&bdev->bd_holder_lock);
	/* [한국어] holder/hops 갱신이 끝났으므로 세분화 락을 반납 — 이 시점부터
	 * 다른 스레드가 bd_holder_lock만 잡고도 방금 설정된 값을 안전하게 읽을 수 있다. */
	bd_clear_claiming(whole, holder);
	/* [한국어] bd_prepare_to_claim()이 남겨 둔 whole->bd_claiming "진행 중" 표시를
	 * 해제하고, 그 사이 대기하던 다른 claim 시도자들을 깨운다 — 이제 claim이
	 * 완전히 확정되어 bd_holder(s)로 상태가 넘어갔으므로 더 이상 bd_claiming으로
	 * 진행 상태를 표시할 필요가 없다. */
	mutex_unlock(&bdev_lock);
	/* [한국어] claim 확정 절차 전체를 마쳤으므로 전역 뮤텍스를 반납. */
}

/*
 * [한국어]
 * bd_abort_claiming - 진행 중이던 exclusive claim을 포기한다
 *
 * @bdev: claim을 포기할 block_device (whole 또는 파티션) — 실제로 사용되는 것은
 *        이 값에서 얻어지는 whole 디바이스이다.
 * @holder: bd_prepare_to_claim()에 넘겼던 것과 동일한 holder 포인터.
 * @return: 없음(void).
 *
 * bd_prepare_to_claim()이 성공(0 반환)해 whole->bd_claiming = holder로 "진행 중"
 * 표시까지는 해 두었지만, 그 뒤 실제 open이 실패했거나 애초에 exclusive open이
 * 필요 없어져 claim을 확정(bd_finish_claiming)하지 않기로 한 경우에 사용하는
 * 롤백 함수다. 커널독 주석이 설명하듯, 실제로 exclusive하게 열려는 것이 아니라
 * "잠깐 동안만 다른 exclusive opener를 막아 두고 싶은" 용도(예: truncate_bdev_range()가
 * 파일시스템 크기 변경 도중 일시적으로 다른 exclusive open을 차단하는 경우)로도
 * 쓰인다 — 이 경우 bd_prepare_to_claim()으로 막아 두었다가 작업이 끝나면 바로
 * 이 함수로 되돌린다.
 *
 * 실행 컨텍스트: 호출 전 bdev_lock을 들고 있지 않아야 한다(내부에서 직접 획득).
 * bd_may_claim()의 최종 판정을 다시 거치지 않는다는 점이 bd_finish_claiming()과의
 * 차이 — 확정이 아니라 단순 취소이므로 holder/holders 카운트는 전혀 건드리지
 * 않고 bd_claiming 표시만 되돌린다.
 * 호출자: exclusive open 시도가 open() 콜백 실패 등으로 무산된 경로,
 *         truncate_bdev_range() 같은 일시적 배타 잠금 사용자.
 * 호출 대상: bdev_whole(), bd_clear_claiming().
 * 에러 처리: 없음 — bd_clear_claiming() 내부의 BUG_ON이 holder 불일치를 잡아낸다.
 *
 * 호출 체인:
 *   blkdev_get_by_dev() 실패 경로 / truncate_bdev_range() 등 → [bd_abort_claiming] → bd_clear_claiming()
 */

/**
 * bd_abort_claiming - abort claiming of a block device
 * @bdev: block device of interest
 * @holder: holder that has claimed @bdev
 *
 * Abort claiming of a block device when the exclusive open failed. This can be
 * also used when exclusive open is not actually desired and we just needed
 * to block other exclusive openers for a while.
 */
void bd_abort_claiming(struct block_device *bdev, void *holder)
{
	mutex_lock(&bdev_lock);
	/* [한국어] bd_claiming 필드를 안전하게 되돌리기 위해 전역 뮤텍스 획득. */
	bd_clear_claiming(bdev_whole(bdev), holder);
	/* [한국어] @bdev의 whole 디바이스에 대해 진행 중이던 claim 표시를 해제하고
	 * 대기 중이던 다른 claim 시도자들을 깨운다 — bd_holders 카운트는 애초에
	 * bd_finish_claiming()에서만 올라가므로 여기서는 건드릴 것이 없다(즉
	 * "확정되지 않은" claim은 카운트에 아무 흔적도 남기지 않는다). */
	mutex_unlock(&bdev_lock);
	/* [한국어] 롤백을 마쳤으므로 락 반납. */
}
EXPORT_SYMBOL(bd_abort_claiming);
/* [한국어] 이 심볼은 (GPL 전용이 아닌) 일반 EXPORT_SYMBOL로 내보내진다 —
 * bd_prepare_to_claim()과 달리 GPL 라이선스 제한 없이 더 넓은 범위의
 * 호출자(파일시스템 등)가 실패 롤백 경로에서 사용할 수 있어야 하기 때문이다. */

/*
 * [한국어]
 * bd_end_claim - holder가 더 이상 block_device를 사용하지 않을 때 claim을 해제한다
 *
 * @bdev: claim을 반납할 block_device (whole 또는 파티션).
 * @holder: 반납하는 주체 — bdev->bd_holder에 등록되어 있던 값과 일치해야 한다.
 * @return: 없음(void).
 *
 * bd_finish_claiming()으로 확정되었던 exclusive claim을 되돌리는 짝 함수다.
 * exclusive open된 디바이스를 닫을 때(예: 파일시스템 언마운트, exclusive fd
 * close) 호출되어, holder 카운트를 감소시키고 마지막 holder였다면
 * bdev->bd_holder/bd_holder_ops를 비워 다른 주체가 다시 claim할 수 있게 만든다.
 *
 * 동작 과정: (1) bdev_lock을 잡고 holder 일치 여부를 WARN_ON_ONCE로 확인,
 * bdev->bd_holders와 whole->bd_holders를 각각 감소(둘 다 언더플로우 방지를 위해
 * 감소 후 값이 음수인지 WARN_ON_ONCE로 검사 — bd_finish_claiming()이 두 카운트를
 * 함께 올렸으므로 반납도 항상 짝을 맞춰 함께 내려야 한다). (2) bdev->bd_holders가
 * 0이 되면(이 bdev를 잡고 있던 마지막 holder라면) bd_holder_lock으로 보호된
 * bd_holder/bd_holder_ops를 NULL로 비우고, 만약 이 holder가 BD_WRITE_HOLDER
 * 플래그(쓰기 목적 exclusive holder였음을 나타냄)를 갖고 있었다면 나중에
 * 이벤트 폴링을 다시 열어줘야 하므로 unblock 플래그를 세워 둔다. (3)
 * whole->bd_holders도 0이 되면 whole->bd_holder(sentinel 또는 실제 holder)도
 * NULL로 비운다. (4) bdev_lock을 반납한 "이후에" unblock이 필요하면
 * disk_unblock_events()와 bdev_clear_flag()를 호출한다 — 이벤트 언블록 콜백이
 * 디스크 상태를 다시 조회하면서 재귀적으로 같은 락을 요구할 수 있어, 락을 쥔
 * 채로 호출하면 데드락 위험이 있기 때문에 반드시 mutex_unlock 이후로 미룬다.
 *
 * 실행 컨텍스트: 호출 전 bdev_lock을 들고 있지 않아야 한다(내부에서 직접 획득 후
 * 반납). open_mutex는 disk_holder 링크 해제와의 동기화에 쓰이지만(원문 주석
 * 참고) 이 함수 자체는 bdev_lock만 사용한다.
 * 호출자: blkdev_put() (exclusive close 경로).
 * 호출 대상: disk_unblock_events(), bdev_clear_flag() (unblock 필요 시에 한해,
 *           그리고 반드시 mutex_unlock 이후).
 * 에러 처리: holder 불일치나 카운트 언더플로우는 WARN_ON_ONCE로 로그만 남기고
 *           계속 진행한다(치명적 BUG는 아님 — claim 카운트 자체의 불일치는
 *           추적 목적의 방어적 점검이지 반드시 즉시 패닉할 사안은 아니라고
 *           판단한 것).
 *
 * 호출 체인:
 *   blkdev_put() → [bd_end_claim] → disk_unblock_events()
 */
static void bd_end_claim(struct block_device *bdev, void *holder)
{
	struct block_device *whole = bdev_whole(bdev);
	/* [한국어] @bdev가 파티션이면 그 전체 디스크를 얻는다 — holder 카운트를
	 * whole 기준으로도 함께 감소시켜야 하므로 미리 확보. */
	bool unblock = false;
	/* [한국어] 이 claim 해제가 "마지막 holder이면서 write holder였는지" 여부에
	 * 따라 함수 뒷부분에서 disk_unblock_events()를 호출할지 결정하는 플래그.
	 * 기본값 false로 시작해 아래 조건을 만족할 때만 true로 바뀐다. */

	/*
	 * Release a claim on the device.  The holder fields are protected with
	 * bdev_lock.  open_mutex is used to synchronize disk_holder unlinking.
	 */
	mutex_lock(&bdev_lock);
	/* [한국어] bd_holder(s)/bd_claiming을 보호하는 전역 뮤텍스 획득 — 카운트
	 * 감소와 holder 필드 클리어를 다른 스레드의 claim/release 시도와 원자적으로
	 * 만든다. */
	WARN_ON_ONCE(bdev->bd_holder != holder);
	/* [한국어] 지금 반납하는 holder가 실제로 등록되어 있던 holder와 다르면
	 * 호출자 쪽에서 짝이 맞지 않는 claim/release를 한 것 — 방어적으로
	 * 경고만 남기고(치명적으로 중단하지는 않음) 아래 카운트 감소는 그대로
	 * 진행한다. */
	WARN_ON_ONCE(--bdev->bd_holders < 0);
	/* [한국어] @bdev 자신의 holder 카운트를 먼저 감소시킨 뒤(전위 감소이므로
	 * 감소된 이후 값으로 검사), 그 결과가 음수라는 것은 bd_finish_claiming()으로
	 * 올린 적보다 더 많이 내렸다는 뜻이라 카운트 관리 로직의 버그다 —
	 * WARN_ON_ONCE로 남긴다. */
	WARN_ON_ONCE(--whole->bd_holders < 0);
	/* [한국어] whole 디바이스 관점의 holder 카운트도 짝을 맞춰 함께 감소 —
	 * bd_finish_claiming()이 whole->bd_holders와 bdev->bd_holders를 함께
	 * 올렸으므로 반납도 항상 두 카운트를 함께 내려야 한다. */
	if (!bdev->bd_holders) {
		/* [한국어] 방금 감소시킨 결과 @bdev의 holder 카운트가 0이 되었다면
		 * 이 claim 해제가 "이 bdev를 잡고 있던 마지막 holder"였다는 뜻이다
		 * — 이 경우에만 실제 holder 필드들을 비워 다른 주체가 다시 claim할
		 * 수 있게 만든다. */
		mutex_lock(&bdev->bd_holder_lock);
		/* [한국어] bd_holder/bd_holder_ops를 보호하는 세분화 락 획득 —
		 * 이벤트 통지 경로 등이 이 두 필드를 bdev_lock 없이 조회할 수
		 * 있으므로, 필드를 비우는 동안에도 이 락으로 일관성을 보장한다. */
		bdev->bd_holder = NULL;
		/* [한국어] holder 포인터를 비워 "이제 아무도 이 bdev를 exclusive로
		 * 잡고 있지 않다"는 것을 표시 — 이후 bd_may_claim()이 새 claim을
		 * 허용하게 된다. */
		bdev->bd_holder_ops = NULL;
		/* [한국어] 더 이상 유효한 holder가 없으므로 그 콜백 테이블 포인터도
		 * 함께 비운다 — 비워두지 않으면 이미 해제되었을 수 있는 holder의
		 * 콜백을 잘못 호출할 위험이 있다. */
		mutex_unlock(&bdev->bd_holder_lock);
		/* [한국어] holder/hops 클리어를 마쳤으므로 세분화 락 반납. */
		if (bdev_test_flag(bdev, BD_WRITE_HOLDER))
			/* [한국어] 이 holder가 쓰기 목적의 exclusive holder였음을
			 * 나타내는 BD_WRITE_HOLDER 플래그가 세워져 있는지 확인 —
			 * write holder는 open 중 미디어 이벤트 폴링을 차단해
			 * 두었으므로, 이제 claim이 완전히 풀렸으니 그 차단을
			 * 되돌려야 한다(실제 호출은 뮤텍스 반납 이후로 미룬다). */
			unblock = true;
	}
	if (!whole->bd_holders)
		/* [한국어] whole 디바이스 관점의 holder 카운트도 0이 되었다면
		 * (파티션 holder였던 경우 whole 카운트가 0이 되는 시점이 bdev
		 * 자신의 카운트와 다를 수 있어 별도로 검사한다), whole->bd_holder도
		 * 함께 비워 bd_may_claim()이 "전체 디스크가 통째로 잡혀 있다"고
		 * 잘못 판단하지 않게 한다. */
		whole->bd_holder = NULL;
	mutex_unlock(&bdev_lock);
	/* [한국어] 카운트 감소와 필드 클리어를 모두 마쳤으므로 전역 뮤텍스 반납 —
	 * 아래 disk_unblock_events() 호출은 이 락을 놓은 뒤에 이루어져야
	 * 데드락을 피할 수 있다(다음 주석 참고). */

	/*
	 * If this was the last claim, remove holder link and unblock evpoll if
	 * it was a write holder.
	 */
	if (unblock) {
		/* [한국어] 위에서 "마지막 holder이면서 write holder였다"고 표시해
		 * 둔 경우에만 진입 — 이벤트 폴링 차단을 해제하는 작업을 실제로
		 * 마지막 holder가 반납될 때만 수행한다. */
		disk_unblock_events(bdev->bd_disk);
		/* [한국어] open() 시점에 걸어 두었던 미디어 변경 이벤트 폴링
		 * 차단을 해제한다 — bdev_lock을 놓은 뒤 호출하는 이유는, 이
		 * 함수가 내부적으로 디스크 상태를 다시 확인하며 다른 락이나
		 * 콜백을 거칠 수 있어 bdev_lock을 쥔 채 부르면 락 순서 역전으로
		 * 인한 데드락 가능성이 있기 때문이다. */
		bdev_clear_flag(bdev, BD_WRITE_HOLDER);
		/* [한국어] write holder였음을 나타내던 플래그를 클리어 — 이 claim이
		 * 끝났으므로 다음에 이 bdev를 여는 새 holder는 자신이 다시 write
		 * holder인지 여부를 스스로 판단해 새로 설정해야 한다. */
	}
}

/*
 * [한국어]
 * blkdev_flush_mapping - block_device를 닫기 전 캐시를 비우고 inode를 기록한다
 *
 * @bdev: 캐시를 플러시할 block_device (whole 디바이스 — blkdev_put_whole()에서
 *        whole에 대해서만 호출된다).
 * @return: 없음(void).
 *
 * 디바이스를 마지막으로 닫기 직전에, 그 디바이스의 페이지 캐시에 남아있는 dirty
 * 데이터를 디스크에 실제로 내려쓰고(sync_blockdev), 캐시된 페이지들을 모두
 * 무효화하며(kill_bdev), 마지막으로 bdev 자신의 inode 메타데이터도 writeback
 * 한다(bdev_write_inode). 이렇게 해야 다음에 같은 디바이스를 여는 주체(같은
 * 프로세스든 다른 프로세스든)가 stale(오래된) 캐시 데이터를 보지 않고 항상
 * 디스크의 최신 상태를 읽게 된다.
 *
 * WARN_ON_ONCE(bdev->bd_holders)는 이 함수가 호출되는 시점에 아직 남아있는
 * exclusive holder가 있으면 안 된다는 불변조건을 검사한다 — holder가 남아있는
 * 상태에서 캐시를 통째로 비우면, 그 holder가 캐시된 페이지를 통해 여전히
 * 접근 중일 수 있는 데이터를 강제로 날려버려 안전하지 않기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트에서 blkdev_put_whole()이 마지막 opener를
 * 감지했을 때만 호출된다. 이 시점에는 이미 bd_openers 카운트가 0이 되었으므로
 * 새로운 open이 이 디바이스로 끼어들 수 없다(open_mutex로 직렬화됨을 전제).
 * 호출자: blkdev_put_whole() (마지막 opener가 닫힐 때).
 * 호출 대상: sync_blockdev(), kill_bdev(), bdev_write_inode().
 * 에러 처리: 이 함수들의 반환값을 검사하지 않는다 — 닫는 경로에서 flush 실패를
 *           되돌릴 방법이 마땅치 않아 best-effort로 수행된다.
 *
 * 호출 체인:
 *   blkdev_put_whole() → [blkdev_flush_mapping] → sync_blockdev() / kill_bdev()
 */
static void blkdev_flush_mapping(struct block_device *bdev)
{
	WARN_ON_ONCE(bdev->bd_holders);
	/* [한국어] 아직 exclusive holder가 남아있는 채로 캐시를 통째로 비우는 것은
	 * 안전하지 않다는 불변조건 검사 — holder는 blkdev_put_whole()이 호출되기
	 * 전에 bd_end_claim()으로 이미 전부 반납되어 있어야 정상이다. 위반되어도
	 * 계속 진행하지만(경고만), 호출 순서를 잘못 지킨 버그를 조기에 발견하기
	 * 위한 방어적 점검이다. */
	sync_blockdev(bdev);
	/* [한국어] 이 block_device의 페이지 캐시에 있는 dirty 페이지를 전부 실제
	 * 저장장치로 내려쓴다 — 내부적으로 filemap_write_and_wait()류를 호출해
	 * 쓰기가 완료될 때까지 기다린다. 이 단계를 건너뛰면 아직 반영되지 않은
	 * 데이터가 유실될 수 있다. */
	kill_bdev(bdev);
	/* [한국어] sync 이후 남아있는(이제는 clean한) 페이지 캐시 페이지들을 모두
	 * truncate하여 매핑에서 제거한다 — 다음에 이 디바이스가 다시 열릴 때
	 * 오래된 캐시 내용이 아니라 항상 디스크에서 새로 읽어오도록 보장한다. */
	bdev_write_inode(bdev);
	/* [한국어] block_device를 감싸는 특수 inode 자체의 메타데이터(크기, 시각 등)를
	 * writeback한다 — 페이지 캐시 데이터와 별개로 inode 구조체에 걸린 dirty
	 * 상태도 정리해야 완전히 닫을 준비가 끝난다. */
}

/*
 * [한국어]
 * blkdev_put_whole - 전체(whole) block_device의 opener 카운트를 감소시키고,
 *                    마지막 opener라면 캐시를 정리하고 드라이버 release 콜백을 호출한다
 *
 * @bdev: 닫으려는 whole block_device (파티션이 아니라 전체 디스크를 나타내는 bdev).
 * @return: 없음(void).
 *
 * blkdev_get_whole()의 짝이 되는 함수로, atomic_dec_and_test()로 bd_openers를
 * 원자적으로 감소시키면서 동시에 "이 감소로 카운트가 0이 되었는가"(=이 호출이
 * 마지막 opener를 닫는 것인가)를 판정한다. 마지막 opener였다면
 * blkdev_flush_mapping()으로 캐시와 inode를 정리해 다음 open이 stale 데이터를
 * 보지 않게 한다. 그 뒤 opener 수와 무관하게, gendisk의 ->fops->release 콜백이
 * 등록되어 있으면 항상 호출해 드라이버(예: NVMe 드라이버)가 자신의 참조 카운트나
 * 자원을 정리할 기회를 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, open_mutex 등 상위 락으로 직렬화된 상태에서
 * 호출된다고 가정. atomic_dec_and_test() 자체는 락-프리(lock-free) 원자 연산이라
 * 다른 CPU의 atomic_inc(&bdev->bd_openers)(blkdev_get_whole 참고)와 안전하게
 * 경쟁한다.
 * 호출자: blkdev_put() (whole 디바이스를 닫는 경로).
 * 호출 대상: blkdev_flush_mapping(), ->fops->release().
 * 에러 처리: 이 함수와 ->fops->release()는 반환값이 없다(void) — 드라이버
 *           release 콜백은 실패할 수 없는 정리 작업만 수행한다는 커널 관례를
 *           따른다.
 *
 * 호출 체인:
 *   blkdev_put() → [blkdev_put_whole] → ->fops->release()
 */
static void blkdev_put_whole(struct block_device *bdev)
{
	if (atomic_dec_and_test(&bdev->bd_openers))
		/* [한국어] bd_openers를 원자적으로 1 감소시키고, 그 결과가 정확히
		 * 0이 되었는지 검사 — 0이 되었다는 것은 이 호출이 "마지막으로
		 * 열려 있던 opener"를 닫는 것이라는 뜻이다. atomic 연산을 쓰는
		 * 이유는 여러 스레드가 동시에 close를 호출해도 정확히 한 스레드만
		 * "마지막"으로 판정되도록 보장하기 위함이다(경쟁 상태에서 두
		 * 스레드가 동시에 flush를 수행하는 것을 막는다). */
		blkdev_flush_mapping(bdev);
		/* [한국어] 마지막 opener였으므로 캐시된 dirty 데이터를 내려쓰고
		 * 페이지 캐시를 비운다 — 아직 다른 opener가 남아있다면 이 단계는
		 * 건너뛴다(다른 opener가 여전히 캐시를 쓰고 있을 수 있으므로). */
	if (bdev->bd_disk->fops->release)
		/* [한국어] gendisk의 block_device_operations 테이블에 release
		 * 콜백이 등록되어 있는지 확인 — 모든 드라이버가 이 콜백을 구현하는
		 * 것은 아니므로 NULL 체크 후 호출한다. */
		bdev->bd_disk->fops->release(bdev->bd_disk);
		/* [한국어] 드라이버 고유의 정리 작업을 수행할 기회를 준다 — 예를
		 * 들어 NVMe 드라이버라면 이 콜백에서 네임스페이스 참조 카운트를
		 * 낮추거나, 마지막 참조였다면 관련 자원(요청 큐 태그, 통계 등)을
		 * 해제할 수 있다. opener 수와 무관하게(마지막이 아니어도) 매번
		 * 호출된다는 점에 유의 — release는 "이 fd 하나를 닫는다"는
		 * 의미이지 "디바이스 전체를 닫는다"는 의미가 아니다. */
}

/*
 * [한국어]
 * blkdev_get_whole - 전체(whole) block_device를 연다 (드라이버 ->open() 콜백 호출)
 *
 * @disk: 열려는 whole block_device가 속한 gendisk.
 * @mode: BLK_OPEN_READ/BLK_OPEN_WRITE 등 open 모드 플래그의 조합
 *        (blk_mode_t). BLK_OPEN_STRICT_SCAN이 포함되어 있으면 파티션 재스캔
 *        실패를 open 실패로 전파해야 한다는 것을 의미한다.
 * @return: 0이면 성공적으로 열림. 음수 errno면 실패 — ->fops->open()이 실패했거나,
 *          STRICT_SCAN 모드에서 파티션 재스캔이 실패한 경우. 실패 시 내부적으로
 *          늘렸던 opener 카운트는 blkdev_put_whole()로 이미 되돌려져 있다.
 *
 * blkdev_put_whole()의 짝이 되는, whole 디바이스를 여는 핵심 함수다. 드라이버의
 * ->fops->open() 콜백(NVMe라면 컨트롤러/네임스페이스 초기화, 참조 카운트 증가
 * 등을 수행)을 호출한 뒤, 미디어가 없는 상태(-ENOMEDIUM)이면서 파티션 재스캔이
 * 필요하다고 표시되어 있으면(GD_NEED_PART_SCAN) 미디어가 없는 상태에서 남아있는
 * "유령 파티션"들을 정리하기 위해 bdev_disk_changed(disk, true)를 호출한다.
 * open 콜백이 성공(또는 콜백 자체가 없음)하면, 이 디바이스의 첫 opener인 경우에만
 * set_init_blocksize()로 논리 블록 크기를 초기화하고, bd_openers를 증가시킨다.
 * 마지막으로 GD_NEED_PART_SCAN 비트가 서 있으면(디스크 내용이 바뀌어 파티션
 * 테이블을 다시 읽어야 하는 상태) bdev_disk_changed(disk, false)로 실제 재스캔을
 * 수행하고, 이 재스캔이 실패했을 때 BLK_OPEN_STRICT_SCAN 모드(예: BLKRRPART
 * ioctl처럼 재스캔 결과를 명시적으로 신경 쓰는 호출자)에서만 그 에러를 open
 * 실패로 전파한다 — 일반적인 open 경로에서는 재스캔 실패가 있어도 open 자체는
 * 성공시켜, 파티션 테이블이 깨진 디스크라도 whole 디바이스는 열 수 있게 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 상위(blkdev_get_by_dev 등)에서 open_mutex로
 * 직렬화된 상태를 전제로 한다. atomic_read/atomic_inc로 bd_openers를 다루지만
 * "첫 opener인지" 판정과 증가 사이에 원자성이 보장되지는 않으므로(상위 락으로
 * 이미 직렬화되어 있어 문제없음), 락 없이 이 함수만 여러 스레드에서 동시에
 * 부르면 안전하지 않다.
 * 호출자: blkdev_get_by_dev()/blkdev_open() 등 whole 디바이스 open 경로.
 * 호출 대상: ->fops->open(), bdev_disk_changed(), set_init_blocksize(),
 *           blkdev_put_whole()(재스캔 실패로 롤백할 때).
 * 에러 처리: ->fops->open() 실패 시 그 에러코드를 그대로 반환(단 -ENOMEDIUM +
 *           재스캔 필요 조합이면 먼저 유령 파티션 정리 시도). 재스캔 실패는
 *           STRICT_SCAN 모드에서만 blkdev_put_whole()로 opener를 되돌리고
 *           에러를 전파한다.
 *
 * 호출 체인:
 *   blkdev_get_by_dev()/blkdev_open() → [blkdev_get_whole] → ->fops->open() / bdev_disk_changed()
 */
static int blkdev_get_whole(struct block_device *bdev, blk_mode_t mode)
{
	struct gendisk *disk = bdev->bd_disk;
	/* [한국어] bdev가 속한 gendisk(범용 디스크 객체)를 얻는다 — ->fops (block
	 * device operations 테이블)와 ->state(GD_* 비트) 필드에 접근하기 위해
	 * 반복적으로 필요하므로 지역 변수로 캐시해 둔다. */
	int ret;
	/* [한국어] 아래에서 드라이버 open 콜백과 bdev_disk_changed()의 반환값을
	 * 담을 지역 변수 — 성공(0)/실패(음수 errno) 판정에 재사용된다. */

	if (disk->fops->open) {
		/* [한국어] 이 gendisk의 드라이버가 open 콜백을 등록해 두었는지 확인 —
		 * 모든 드라이버가 open을 구현하는 것은 아니므로(예: 일부 가상
		 * 디바이스는 필요 없음) NULL 체크 후에만 호출한다. */
		ret = disk->fops->open(disk, mode);
		/* [한국어] 드라이버 고유의 open 처리를 위임 — 예를 들어 NVMe라면
		 * 네임스페이스 참조 카운트 증가, 컨트롤러 상태 확인 등을 수행하고
		 * 실패 시 음수 errno를 반환한다. */
		if (ret) {
			/* [한국어] 드라이버 open이 실패한 경우의 에러 처리 분기. */
			/* avoid ghost partitions on a removed medium */
			if (ret == -ENOMEDIUM &&
			     test_bit(GD_NEED_PART_SCAN, &disk->state))
				/* [한국어] 미디어가 물리적으로 빠져 있어(-ENOMEDIUM,
				 * 예: 이동식 저장장치가 분리됨) open이 실패했고,
				 * 동시에 "파티션 재스캔 필요" 비트가 서 있다면 —
				 * 이는 이전 미디어가 있던 시절의 파티션 정보가
				 * 아직 커널 안에 남아있을 수 있다는 뜻이다. 이런
				 * "유령 파티션"을 방치하면 사라진 미디어의 파티션이
				 * 여전히 존재하는 것처럼 보여 사용자 공간을
				 * 혼란시킬 수 있다. */
				bdev_disk_changed(disk, true);
				/* [한국어] invalidate=true로 파티션 재스캔을
				 * 실행해, 더 이상 유효하지 않은 파티션들을
				 * 제거한다 — 미디어가 없으므로 새 파티션을 읽어
				 * 들이는 것이 아니라 기존 파티션 구조를 지우는
				 * 용도로 쓰인다. */
			return ret;
			/* [한국어] open 콜백이 실패했으므로 그 에러코드를 그대로
			 * 호출자에게 전파 — 유령 파티션 정리는 부수 효과일 뿐
			 * open 자체의 성공 여부에는 영향을 주지 않는다. */
		}
	}

	if (!atomic_read(&bdev->bd_openers)) 	/* [한국어] opener 카운트가 아직 0이면(atomic_read == 0) 이 open이 첫 opener라는 뜻 — 이때만 블록 크기를 초기화한다. */
		set_init_blocksize(bdev);
		/* [한국어] 디바이스의 물리/논리 섹터 크기를 조회해 bdev의 초기
		 * 블록 크기 필드를 설정한다 — 이후 파일시스템/블록 계층이 이
		 * 크기를 기준으로 I/O를 정렬한다. 두 번째 이후의 open에서는
		 * 이미 초기화되어 있으므로 다시 계산하지 않는다 — 진행 중인
		 * I/O의 블록 크기 가정을 흔들지 않기 위해서다. */
	atomic_inc(&bdev->bd_openers);
	/* [한국어] opener 카운트를 원자적으로 1 증가 — blkdev_put_whole()의
	 * atomic_dec_and_test()와 짝을 이루어, 몇 명이 이 whole 디바이스를
	 * 열고 있는지를 락 없이도 정확히 추적한다. */
	if (test_bit(GD_NEED_PART_SCAN, &disk->state)) {
		/* [한국어] 디스크 내용이 바뀌어(예: 다른 프로세스가 파티션 테이블을
		 * 재작성했거나, 미디어가 재삽입됨) 파티션 테이블을 다시 읽어야
		 * 한다고 표시된 상태인지 확인. */
		/*
		 * Only return scanning errors if we are called from contexts
		 * that explicitly want them, e.g. the BLKRRPART ioctl.
		 */
		ret = bdev_disk_changed(disk, false);
		/* [한국어] invalidate=false로 실제 파티션 재스캔을 수행 — 새로운
		 * 파티션 테이블을 읽어 파티션 bdev들을 갱신/생성한다. 이 결과를
		 * 곧바로 open 실패로 만들지 여부는 호출 모드에 따라 다르므로
		 * 반환값을 우선 저장만 해 둔다. */
		if (ret && (mode & BLK_OPEN_STRICT_SCAN)) {
			/* [한국어] 재스캔이 실패했고(ret != 0), 호출자가
			 * BLK_OPEN_STRICT_SCAN 모드로 열어 재스캔 실패를 반드시
			 * 알아야 하는 경우(예: BLKRRPART ioctl로 명시적으로
			 * 파티션 재읽기를 요청한 사용자)에만 이 분기로 진입한다 —
			 * 일반적인 open()에서는 파티션 테이블이 깨져 있어도 whole
			 * 디바이스는 열 수 있어야 하므로 이 조건이 없으면 open이
			 * 불필요하게 실패하게 된다. */
			blkdev_put_whole(bdev);
			/* [한국어] 이미 위에서 증가시킨 bd_openers를 되돌리기 위해
			 * blkdev_put_whole()을 호출 — open을 실패로 처리하기로
			 * 했으므로 방금 늘린 opener 카운트도 원상복구해야 opener
			 * 수 불일치가 발생하지 않는다. */
			return ret;
			/* [한국어] 재스캔 실패를 open 실패로 그대로 전파 —
			 * 호출자(예: BLKRRPART ioctl 처리 코드)는 이 에러를 보고
			 * 사용자에게 실패를 알린다. */
		}
	}
	return 0;
	/* [한국어] 여기까지 도달했다면 드라이버 open 성공, opener 카운트 증가,
	 * (필요했다면) 파티션 재스캔까지 모두 문제없이 끝난 것 — whole 디바이스
	 * open을 최종 성공으로 반환한다. */
}

/*
 * [한국어]
 * blkdev_get_part - 파티션 block_device 를 오픈한다 (전체 디스크를 먼저 열고 파티션별 카운터를 관리)
 *
 * @part: 오픈 대상 파티션을 표현하는 block_device. bd_start_sect/bd_nr_sectors
 *        로 표현되는 시작 LBA(Logical Block Address, 논리 블록 주소)와 길이만
 *        가질 뿐, 독립된 request_queue 는 갖지 않는다. 실제 큐/디스패치 경로는
 *        항상 bdev_whole(part), 즉 전체 디스크가 소유한다.
 * @mode: BLK_OPEN_READ/BLK_OPEN_WRITE/BLK_OPEN_RESTRICT_WRITES 등의 비트 조합.
 *        blkdev_get_whole() 호출 시 그대로 전달되어 전체 디스크에도 동일한
 *        권한 검사가 적용된다.
 * @return: 0 이면 성공. 이 파티션의 bd_openers 카운트가 1 증가한 상태로 반환한다.
 *          음수 errno(-ENXIO) 면 실패. 실패 시 앞서 열었던 전체 디스크도 함수
 *          내부에서 이미 되돌려(close) 놓았으므로 호출자는 추가 정리 없이
 *          그대로 에러를 전파하면 된다.
 *
 * 파티션은 그 자체로 독립된 디바이스가 아니라 전체 디스크(gendisk) 위의 한
 * LBA 구간을 가리키는 뷰일 뿐이다. 따라서 파티션을 열려면 먼저 그 파티션이
 * 속한 전체 디스크가 열려 있어야 하고(참조 카운트를 공유), 파티션 자신도
 * 별도의 오프너 카운터(bd_openers)로 몇 명이 이 파티션을 열었는지 추적해야
 * 한다.
 * 동작 순서:
 *   1) blkdev_get_whole() 로 전체 디스크를 먼저 연다. 내부적으로 gendisk
 *      참조 카운트를 올리고 필요하면 fops open 콜백을 호출한다.
 *   2) 파티션의 섹터 수가 0이면(파티션 테이블이 아직 스캔되지 않았거나 더
 *      이상 유효하지 않은 파티션) -ENXIO 로 실패시키고, 방금 연 전체
 *      디스크를 out_blkdev_put 레이블에서 되돌려 닫는다.
 *   3) 이 파티션의 첫 오프너라면(bd_openers 가 0이었다면) 부모 디스크의
 *      open_partitions 카운트를 증가시키고 set_init_blocksize() 로 파티션
 *      크기 기준의 초기 블록 크기를 재계산한다.
 *   4) 파티션 오프너 카운트를 원자적으로 1 증가시키고 성공을 반환한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(open(2)/mount(2) 시스템 콜 경로)에서
 * 슬립 가능한 상태로 호출된다. 상위 호출자인 bdev_open() 이 open_mutex 를
 * 들고 이 함수를 부르므로, bd_openers 의 read(atomic_read) 와 이어지는
 * open_partitions++ 갱신 사이에 다른 오프너가 끼어들 수 없다. atomic_inc
 * 자체도 원자적이라 뮤텍스 없이도 카운터 증가는 안전하지만, 첫 오프너
 * 판별 로직은 뮤텍스가 보장하는 직렬화에 의존한다.
 * 호출자: bdev_open()(본 파일). 오픈 대상 경로가 파티션 디바이스 노드일 때.
 * 피호출자: blkdev_get_whole(), bdev_nr_sectors(), set_init_blocksize(),
 *          실패 시 blkdev_put_whole().
 * 에러 처리: 유일한 실패 경로는 파티션 섹터 수 0 뿐이며, goto
 * out_blkdev_put 으로 점프해 이미 열어둔 전체 디스크를 대칭적으로 닫아
 * 참조 카운트 누수를 막는다.
 *
 * 호출 체인:
 *   bdev_open() -> [blkdev_get_part] -> blkdev_get_whole() / set_init_blocksize()
 */

static int blkdev_get_part(struct block_device *part, blk_mode_t mode)
{
	struct gendisk *disk = part->bd_disk;
// [한국어] 파티션이 속한 전체 디스크(gendisk) 포인터를 지역 변수로 캐시.
// 아래에서 open_partitions 카운트를 갱신할 때 매번 part->bd_disk 를 다시
// 역참조하지 않도록 미리 꺼내둔다.
	int ret;
// [한국어] blkdev_get_whole()/섹터 수 검사의 결과(0=성공, 음수=errno)를
// 담을 지역 변수. 이후 실패 경로(out_blkdev_put)에서도 그대로 반환값으로
// 재사용된다.

	ret = blkdev_get_whole(bdev_whole(part), mode);
// [한국어] 파티션의 실제 request_queue 를 소유한 전체 디스크를 먼저
// 오픈한다. 파티션 자체는 큐가 없으므로 IO 가 가능하려면 전체 디스크가
// 반드시 먼저(그리고 최소 이 파티션이 닫힐 때까지) 열려 있어야 한다.
	if (ret)
// [한국어] 전체 디스크 open 자체가 실패한 경우(exclusive 충돌, cgroup
// 거부 등). 이 시점에는 파티션 쪽에서 아직 아무 상태도 바꾸지 않았으므로
// 별도 롤백 없이 곧바로 에러를 전파해도 안전하다.
		return ret;
// [한국어] 전체 디스크 open 실패 결과를 그대로 상위 호출자에 전달.

	ret = -ENXIO;
// [한국어] 이후 실패 시 사용할 기본 에러코드를 -ENXIO(해당 장치/주소
// 없음)로 미리 설정. 아래 섹터 수 검사에서 goto 로 빠질 때 이 값이
// 그대로 반환된다.
	if (!bdev_nr_sectors(part))
// [한국어] 파티션의 섹터 수가 0인지 확인. 파티션 테이블이 아직
// 스캔되지 않았거나(GD_NEED_PART_SCAN) 이미 삭제된 파티션이면 크기가
// 0이 되어 유효한 LBA 범위가 존재하지 않는다.
		goto out_blkdev_put;
// [한국어] 섹터 수 0 이면 이 파티션은 열 수 없으므로, 방금 성공시켜
// 놓은 전체 디스크 open 을 대칭적으로 되돌리는 정리 경로로 점프한다.

	if (!atomic_read(&part->bd_openers)) {
// [한국어] 현재 오프너 수를 원자적으로 읽어 0인지 검사. 이 파티션에
// 대한 첫 open 인지 판별한다. 호출측이 open_mutex 를 들고 있어 이
// read 와 아래의 open_partitions++ 사이에 다른 오프너의 개입이 없음이
// 보장된다.
		disk->open_partitions++;
// [한국어] 부모 디스크 기준으로 현재 열려 있는 파티션 개수를 증가시킨다.
// 파티션 재스캔이나 디스크 제거 로직이 이 카운트로 사용 중인 파티션이
// 있는지 판단한다.
		set_init_blocksize(part);
// [한국어] 파티션의 초기 논리 블록 크기를 파티션 자신의 섹터 수 기준으로
// 재계산한다. 전체 디스크와 파티션은 섹터 범위가 다르므로 블록 크기
// 협상을 파티션 단위로 다시 수행해야 한다.
	}
	atomic_inc(&part->bd_openers);
// [한국어] 이 파티션의 오프너 카운트를 원자적으로 1 증가시킨다. 여러
// 프로세스가 동시에 같은 파티션을 열 수 있어 단순 증가 연산이 아닌
// 원자 연산으로 경쟁을 방지한다.
	return 0;
// [한국어] 파티션 open 성공을 알린다.

out_blkdev_put:
	blkdev_put_whole(bdev_whole(part));
// [한국어] 파티션 open 이 실패로 확정되었으므로, 이 함수 시작 부분에서
// 성공시켰던 전체 디스크 open 을 대칭적으로 되돌려(opener 카운트 감소)
// 참조 카운트 누수를 막는다.
	return ret;
// [한국어] -ENXIO 를 호출자에게 반환한다.
}

/*
 * [한국어]
 * bdev_permission - dev_t 에 대한 open 을 허용할지 cgroup 및 배타적 접근 제약으로 검사
 *
 * @dev: 오픈하려는 블록 디바이스의 dev_t(주/부 번호 조합, MAJOR()/MINOR() 로
 *       분해). 아직 struct block_device 를 찾기 전 단계이므로 dev_t 값만으로
 *       검사한다.
 * @mode: BLK_OPEN_READ/BLK_OPEN_WRITE/BLK_OPEN_RESTRICT_WRITES 등 open 요청
 *        플래그. cgroup 권한 매핑과 exclusive-write 제약 판단에 모두 사용된다.
 * @holder: 배타적 소유권을 주장할 주체를 식별하는 불투명 포인터(보통 호출자
 *          모듈의 어떤 구조체 주소). NULL 이면 배타적 클레임 없음을 의미하며,
 *          BLK_OPEN_RESTRICT_WRITES 와 함께 쓰일 때는 반드시 non-NULL 이어야
 *          한다.
 * @return: 0 이면 허용, 음수 errno 면 거부. devcgroup 거부 시 그 코드를 그대로,
 *          exclusive 제약 위반 시 -EINVAL 을 반환한다. 호출자(bdev_open())는
 *          0이 아니면 이후 open 절차를 진행하지 않고 그대로 에러를 반환한다.
 *
 * 이 함수는 아직 block_device 를 실제로 찾거나 참조를 얻기 전, dev_t 값만
 * 가지고도 미리 걸러낼 수 있는 두 종류의 정책을 검사한다. (1) cgroup v1/v2
 * 의 devices 컨트롤러(DEVCG_DEV_BLOCK)가 이 프로세스에게 해당 major/minor
 * 장치에 대한 읽기/쓰기를 허용하는지. (2) BLK_OPEN_RESTRICT_WRITES(다른 쓰기
 * 오프너를 모두 배제하는 배타적 쓰기 모드)를 요청했는데 holder(소유권
 * 주체)가 없어서 애초에 배타성을 보장할 수 없는 모순된 요청은 아닌지.
 * 동작 순서:
 *   1) devcgroup_check_permission() 으로 DEVCG_DEV_BLOCK 클래스에 대해 이
 *      major/minor 장치의 읽기/쓰기 권한을 cgroup 정책과 대조한다. mode 의
 *      BLK_OPEN_READ/WRITE 비트를 DEVCG_ACC_READ/WRITE 비트로 매핑해서
 *      넘긴다.
 *   2) cgroup 이 거부하면(ret != 0) 그 errno 를 그대로 반환.
 *   3) BLK_OPEN_RESTRICT_WRITES 가 켜져 있는데 holder 가 NULL 이면, 다른
 *      쓰기를 전부 막겠다는 요청인데 정작 누가 막는지 신원이 없으므로
 *      -EINVAL 로 거부한다(원본 주석대로 "Blocking writes requires
 *      exclusive opener").
 *   4) holder 가 IS_ERR() 값이면, 이는 ->release() 시점에 open 이 실패했다는
 *      것을 알리기 위해 에러 포인터를 holder 자리에 넣는 내부 관례가 open
 *      경로까지 잘못 흘러든 호출자 버그 상황이다. WARN_ON_ONCE() 로 커널
 *      로그에 1회 경고를 남기고 -EINVAL 로 방어적으로 거부한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(open(2)/mount(2) 등)에서 호출되며, 아직
 * 어떤 bdev 락도 잡지 않은 상태다. dev_t 값만으로 판단 가능한 정책 검사이므로
 * block_device 자체에 대한 락이 필요 없다. devcgroup_check_permission() 내부
 * 에서 cgroup 서브시스템 자체의 락을 사용하지만 이는 이 함수 바깥의 관심사다.
 * 호출자: bdev_open()(본 파일). 실제 block_device 탐색/오픈 이전, 가장 먼저
 * 수행되는 정책 게이트.
 * 피호출자: devcgroup_check_permission()(security/device_cgroup.c), MAJOR()/
 *          MINOR() 매크로, WARN_ON_ONCE()/IS_ERR().
 * 에러 처리: 세 가지 실패 경로(cgroup 거부, restrict-writes without holder,
 * holder 가 에러 포인터) 모두 함수 초반에 즉시 반환되며, 이 시점에는 아직
 * 아무 자원도 획득하지 않았으므로 별도 롤백이 필요 없다.
 *
 * 호출 체인:
 *   bdev_open() -> [bdev_permission] -> devcgroup_check_permission()
 */

int bdev_permission(dev_t dev, blk_mode_t mode, void *holder)
{
	int ret;
// [한국어] devcgroup 검사 결과(0=허용, 음수=errno)를 담을 지역 변수.

	ret = devcgroup_check_permission(DEVCG_DEV_BLOCK,
// [한국어] cgroup devices 컨트롤러에 블록 디바이스 클래스 접근을 질의한다.
// 컨테이너/cgroup 정책으로 특정 major/minor 조합의 블록 디바이스 접근
// 자체를 차단할 수 있다(예: 컨테이너에는 호스트의 특정 블록 디바이스
// 노출을 금지).
			MAJOR(dev), MINOR(dev),
// [한국어] dev_t 를 주 번호(드라이버 종류 식별, 예: 259 는 blkext)와 부
// 번호(같은 드라이버 내 개별 디바이스/파티션 식별)로 분해해 cgroup 정책
// 테이블과 대조할 키로 사용.
			((mode & BLK_OPEN_READ) ? DEVCG_ACC_READ : 0) |
// [한국어] open 요청에 읽기 플래그가 있으면 cgroup 쪽 읽기 접근 비트
// (DEVCG_ACC_READ)로 매핑. 없으면 0(읽기 권한 질의 안 함)이 되어 비트
// OR 에 영향을 주지 않는다.
			((mode & BLK_OPEN_WRITE) ? DEVCG_ACC_WRITE : 0));
// [한국어] 마찬가지로 쓰기 플래그를 DEVCG_ACC_WRITE 로 매핑해 읽기
// 비트와 OR. 최종적으로 이 프로세스가 요청한 접근 종류 집합이 cgroup
// 정책과 대조된다.
	if (ret)
// [한국어] cgroup 정책이 이 접근을 거부하면(예: -EPERM) 그 즉시 반환.
// 아직 아무 자원도 획득하지 않은 상태라 롤백이 필요 없다.
		return ret;
// [한국어] devcgroup 거부 코드를 그대로 호출자에 전달.

	/* Blocking writes requires exclusive opener */
	if (mode & BLK_OPEN_RESTRICT_WRITES && !holder)
// [한국어] 다른 모든 쓰기를 차단하는 배타적 쓰기 모드를 요청했는데
// holder(배타적 소유권 주체)가 NULL 이면 모순된 요청이다. 누구의 이름
// 으로 다른 쓰기를 막을지 특정할 수 없으므로 거부한다.
		return -EINVAL;
// [한국어] holder 없는 restrict-writes 요청을 -EINVAL 로 거부.

	/*
	 * We're using error pointers to indicate to ->release() when we
	 * failed to open that block device. Also this doesn't make sense.
	 */
	if (WARN_ON_ONCE(IS_ERR(holder)))
// [한국어] holder 자리에 에러 포인터(IS_ERR() 가 true 인 값)가 들어온
// 경우는 정상적인 호출 경로에서는 나타나면 안 되는 내부 버그 상황이다.
// WARN_ON_ONCE() 로 커널 로그에 스택트레이스와 함께 1회만 경고를 남겨
// 개발자가 원인을 추적할 수 있게 한다(매 호출마다 반복 경고로 로그가
// 도배되는 것을 방지).
		return -EINVAL;
// [한국어] 에러 포인터 holder 는 방어적으로 -EINVAL 거부.

	return 0;
// [한국어] 모든 정책 검사를 통과했다. open 을 진행해도 좋다는 의미.
}

/*
 * [한국어]
 * blkdev_put_part - 파티션 block_device 를 닫는다 (마지막 오프너면 캐시 플러시 후 전체 디스크도 닫음)
 *
 * @part: blkdev_get_part() 로 열었던 파티션 block_device. 이미 최소 1 이상의
 *        bd_openers 를 가지고 있는 상태로 호출된다(대칭적으로 open 이 선행).
 * @return: 없음(void).
 *
 * blkdev_get_part() 의 정확한 역연산이다. 파티션은 자신만의 오프너 카운트를
 * 가지므로, 이 카운트가 0이 되는(마지막으로 닫히는) 시점에만 파티션 관련
 * 캐시 정리와 open_partitions 감소를 수행하고, 그 외에는 그저 오프너 카운트
 * 만 줄인다. 전체 디스크(bdev_whole)에 대한 blkdev_put_whole() 호출은 오프너
 * 수와 무관하게 항상 수행되는데, 이는 open 시 blkdev_get_part() 가 항상 먼저
 * blkdev_get_whole() 을 호출해 전체 디스크의 오프너 카운트도 함께 증가시켰기
 * 때문이다(대칭적 카운팅).
 * 동작 순서:
 *   1) bdev_whole(part) 로 부모(전체 디스크) block_device 를 구한다.
 *   2) atomic_dec_and_test() 로 파티션 오프너 카운트를 원자적으로 감소시키고,
 *      결과가 0인지(즉 방금이 마지막 오프너였는지) 동시에 검사한다.
 *   3) 마지막 오프너였다면 blkdev_flush_mapping() 으로 페이지 캐시를 플러시
 *      하고 open_partitions 를 감소시킨다.
 *   4) 오프너 수와 무관하게 항상 blkdev_put_whole() 을 호출해 전체 디스크
 *      쪽 오프너 카운트도 대칭적으로 감소시킨다.
 * 실행 컨텍스트: 프로세스 컨텍스트(close(2)/umount(2) 경로)에서 open_mutex
 * 를 든 상태로 호출된다고 가정한다. bd_openers 검사-후-감소의 원자성은
 * atomic_dec_and_test 자체가 보장하지만, open_partitions-- 갱신은 뮤텍스가
 * 보장하는 직렬화에 의존한다.
 * 호출자: bdev_release()/bdev_open() 의 에러 롤백 경로(본 파일). 파티션
 * block_device 를 닫는 유일한 경로.
 * 피호출자: bdev_whole(), blkdev_flush_mapping(), blkdev_put_whole().
 * 에러 처리: 반환값이 없는(void) 함수이며 실패할 수 있는 연산이 없다. 모든
 * 하위 호출은 상태 정리이지 실패 가능한 자원 획득이 아니다.
 *
 * 호출 체인:
 *   bdev_release() -> [blkdev_put_part] -> blkdev_flush_mapping() / blkdev_put_whole()
 */

static void blkdev_put_part(struct block_device *part)
{
	struct block_device *whole = bdev_whole(part);
// [한국어] 이 파티션이 속한 전체 디스크의 block_device 포인터를 미리
// 구해둔다. open_partitions 감소와 blkdev_put_whole() 호출 양쪽에서
// 재사용한다.

	if (atomic_dec_and_test(&part->bd_openers)) {
// [한국어] 파티션 오프너 카운트를 원자적으로 1 감소시키고, 그 결과가
// 0이 되었는지(즉 마지막으로 이 파티션을 닫는 오프너였는지)를 동시에
// 검사한다. 감소와 0 검사가 하나의 원자 연산으로 묶여 있어 두 오프너가
// 동시에 자신이 마지막이라고 착각하는 경쟁을 방지한다.
		blkdev_flush_mapping(part);
// [한국어] 마지막 오프너이므로 이 파티션의 페이지 캐시를 동기화(dirty
// 데이터 기록)하고 무효화한다. 다음에 이 파티션이 다시 열렸을 때 stale
// 캐시를 보지 않도록 보장.
		whole->bd_disk->open_partitions--;
// [한국어] 부모 디스크 기준 열려 있는 파티션 개수를 감소시킨다.
// blkdev_get_part() 에서 첫 open 시 증가시켰던 것의 대칭 연산.
	}
	blkdev_put_whole(whole);
// [한국어] 파티션 오프너 수와 무관하게, open 시 항상 함께 증가시켰던
// 전체 디스크 쪽 오프너 카운트를 여기서 항상 감소시킨다. 대칭성을
// 유지해야 전체 디스크의 참조 카운트가 정확히 맞아떨어진다.
}

/*
 * [한국어]
 * blkdev_get_no_open - dev_t 로 이미 등록된 block_device 를 찾아 device 참조만 얻는다 (열지는 않음)
 *
 * @dev: 찾으려는 블록 디바이스의 dev_t(주/부 번호 조합).
 * @autoload: true 면 inode 가 아직 없을 때(디바이스가 한 번도 열리지 않아
 *            bdev 캐시에 등록되지 않은 상태) 레거시 모듈 자동 로드를 시도한다.
 *            일반적으로 open(2) 경로에서는 true, 단순 조회 목적일 때는 false.
 * @return: 성공 시 참조가 증가된 struct block_device 포인터(사용 후 반드시
 *          blkdev_put_no_open() 으로 반납해야 함). 해당 dev_t 가 존재하지
 *          않거나, 존재하더라도 마침 제거(removal) 중이라 kobject 참조를
 *          더 이상 얻을 수 없는 경우 NULL 을 반환한다.
 *
 * 이 함수는 아직 실제로 열지는 않지만, dev_t 에 대응하는 block_device 객체가
 * 존재하는지 확인하고 그 존재를 참조로 붙잡아 두기 위한 저수준 헬퍼다.
 * block_device 는 bdev 전용 익명 파일시스템(bdevfs)의 inode 로 캐시되어
 * 있으므로, 먼저 그 inode 캐시(ilookup)에서 찾고, 찾으면 inode 참조를 곧바로
 * device 참조(kobject)로 갈아탄다.
 * 동작 순서:
 *   1) ilookup(blockdev_superblock, dev) 로 bdev 전용 슈퍼블록 안에서 이
 *      dev_t 에 대응하는 inode 를 찾는다. 찾으면 inode 참조 카운트가 1
 *      증가한 채로 반환된다.
 *   2) 못 찾았고 autoload 가 true 이며 CONFIG_BLOCK_LEGACY_AUTOLOAD 가
 *      활성화되어 있다면, blk_request_module() 로 major 번호에 대응하는
 *      드라이버 모듈의 로드를 커널에 요청한 뒤 다시 한 번 ilookup 을
 *      시도한다. 이번에 성공하면 autoloading is deprecated 경고를 남긴다
 *      (최신 커널은 devtmpfs/udev 기반 로딩을 기대하며, 이 경로는 레거시
 *      호환용이다).
 *   3) 그래도 못 찾으면 NULL 반환.
 *   4) 찾은 inode 로부터 BDEV_I() 매크로로 감싸고 있는 block_device 를
 *      꺼내고, kobject_get_unless_zero() 로 device 참조를 시도한다. 만약
 *      이 bdev 가 이미 제거(hot-unplug 등) 절차를 시작해 kobject 참조가
 *      0까지 떨어진 상태라면 참조를 얻지 못하므로 bdev 를 NULL 로 만든다
 *      (제거 중인 디바이스를 되살리지 않기 위함).
 *   5) 애초에 얻어두었던 inode 참조는 iput() 으로 해제한다. 우리가 실제로
 *      필요한 것은 inode 참조가 아니라 device(kobject) 참조이므로, inode
 *      참조에서 device 참조로 소유권을 전환하는 패턴이다.
 * 실행 컨텍스트: 프로세스 컨텍스트에서 호출되며 슬립 가능(ilookup, 모듈
 * 로드 요청 모두 슬립 가능). ilookup 자체가 내부적으로 inode 해시 락을
 * 잠깐 들지만 이 함수 차원에서 별도 락을 들지 않는다.
 * 호출자: bdev_open()/lookup_bdev() 등 dev_t 로부터 block_device 를 얻어야
 * 하는 상위 경로(본 파일).
 * 피호출자: ilookup(), blk_request_module(), pr_warn_ratelimited(), BDEV_I(),
 *          kobject_get_unless_zero(), iput().
 * 에러 처리: 존재하지 않음/제거 중인 두 경우 모두 NULL 을 반환하며 별도의
 * errno 는 없다. 호출자가 NULL 을 보고 자체적으로 -ENXIO 등으로 변환한다.
 *
 * 호출 체인:
 *   bdev_open() -> [blkdev_get_no_open] -> ilookup() / blk_request_module() / kobject_get_unless_zero()
 */

struct block_device *blkdev_get_no_open(dev_t dev, bool autoload)
{
	struct block_device *bdev;
// [한국어] 최종적으로 반환할, device 참조가 걸린 block_device 포인터.
	struct inode *inode;
// [한국어] bdev 전용 슈퍼블록에서 찾은(또는 못 찾은) inode 포인터.
// block_device 는 이 inode 를 감싸는 형태(BDEV_I)로 구현되어 있다.

	inode = ilookup(blockdev_superblock, dev);
// [한국어] bdev 전용 익명 슈퍼블록(blockdev_superblock)의 inode 캐시에서
// 이 dev_t 에 해당하는 inode 를 검색한다. 찾으면 inode 참조 카운트가 1
// 증가한 채로 반환되고, 없으면 NULL.
	if (!inode && autoload && IS_ENABLED(CONFIG_BLOCK_LEGACY_AUTOLOAD)) {
// [한국어] inode 를 못 찾았고(디바이스가 아직 한 번도 등록되지 않음),
// 호출자가 autoload 를 허용했고, 커널이 레거시 모듈 자동 로드 기능을
// 빌드에 포함한 경우에만 아래 자동 로드 경로로 진입한다. 최신 배포판은
// 보통 이 옵션을 끄고 devtmpfs 나 udev 로 대체한다.
		blk_request_module(dev);
// [한국어] dev_t 의 주 번호에 대응하는 블록 드라이버 커널 모듈을 로드
// 하도록 요청(request_module() 래퍼)한다. 예를 들어 필요한 드라이버가
// 모듈로만 빌드되어 있고 아직 로드되지 않은 경우 이 시점에 modprobe 가
// 트리거된다.
		inode = ilookup(blockdev_superblock, dev);
// [한국어] 모듈 로드가 성공해 드라이버가 디바이스를 등록했다면 이번에는
// ilookup 이 성공할 수 있으므로 재시도한다.
		if (inode)
// [한국어] 재시도로 실제 inode 를 찾았다면, autoload 로 인해 디바이스가
// 방금 등장했다는 의미이므로 아래에서 경고를 남긴다.
			pr_warn_ratelimited(
"block device autoloading is deprecated and will be removed.\n");
// [한국어] 이 경로(모듈 이름 기반 블록 디바이스 자동 로드)는 더 이상
// 권장되지 않고 향후 제거될 예정이라는 경고를 rate-limit 을 걸어(로그
// 폭주 방지) 커널 로그에 남긴다.
	}
	if (!inode)
// [한국어] autoload 를 시도했음에도(혹은 애초에 시도하지 않았음에도)
// 여전히 inode 를 찾지 못했다면 이 dev_t 에 대응하는 디바이스가 존재
// 하지 않는다는 뜻이다.
		return NULL;
// [한국어] 존재하지 않는 디바이스이므로 NULL 반환. 호출자가 -ENXIO 등
// 으로 변환한다.

	/* switch from the inode reference to a device mode one: */
	bdev = &BDEV_I(inode)->bdev;
// [한국어] BDEV_I() 매크로로 inode 를 감싸고 있는 bdev_inode 컨테이너를
// 얻고(container_of 패턴), 그 안의 block_device 필드 주소를 취한다.
// inode 와 block_device 는 하나의 할당 블록(bdev_inode) 안에 함께
// 존재하므로 포인터 산술만으로 서로 변환 가능하다.
	if (!kobject_get_unless_zero(&bdev->bd_device.kobj))
// [한국어] device(kobject) 참조 카운트가 이미 0으로 떨어진 상태가 아닌
// 경우에만 원자적으로 참조를 1 증가시킨다. 만약 이미 0이라면(예: 다른
// CPU 에서 마지막 참조가 막 해제되어 device_del/release 절차가 진행
// 중) 참조를 얻지 못한 것이므로, 이미 죽어가는 디바이스를 되살리는
// use-after-free 형태의 경쟁을 피해야 한다.
		bdev = NULL;
// [한국어] 참조 획득에 실패했으므로 죽어가는 디바이스를 반환하지 않도록
// bdev 를 NULL 로 무효화한다.
	iput(inode);
// [한국어] 애초에 ilookup() 이 잡아두었던 inode 참조를 해제한다. 이제
// 필요한 것은(성공했다면) device/kobject 참조뿐이므로, inode 참조에서
// device 참조로 소유권을 갈아탄 것이다. bdev 가 NULL 이 된 경우에도
// inode 참조는 여전히 해제해야 하므로 이 줄은 항상 실행된다.
	return bdev;
// [한국어] 성공 시 참조가 걸린 block_device, 실패(제거 중) 시 NULL 반환.
}

/*
 * [한국어]
 * blkdev_put_no_open - blkdev_get_no_open() 이 얻은 device 참조를 반납한다
 *
 * @bdev: blkdev_get_no_open() 이 반환했던, kobject 참조가 걸린 block_device.
 *
 * blkdev_get_no_open() 은 실제로 디바이스를 열지는 않고 device(kobject)
 * 참조만 얻어 존재 여부를 확인하는 용도였다. 그 짝이 되는 반납 함수로,
 * put_device() 를 통해 kobject 참조 카운트를 감소시킨다. 참조가 0이 되면
 * 디바이스의 release 콜백(bdev 의 경우 결국 bdev_free_inode() 등으로 이어짐)
 * 이 트리거될 수 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 별도 락 불필요(put_device 내부에서
 * kobject 참조 카운트에 대한 원자 연산으로 스스로 동시성을 보장한다).
 * 호출자: blkdev_get_no_open() 으로 참조를 얻었다가 실제 open 까지는 가지
 * 않고 포기하는 경로(예: 사전 조회만 하는 코드, 본 파일 또는 호출부).
 * 피호출자: put_device().
 * 에러 처리: 실패할 수 없는 연산이며 반환값도 없다.
 *
 * 호출 체인:
 *   (blkdev_get_no_open() 호출자) -> [blkdev_put_no_open] -> put_device()
 */

void blkdev_put_no_open(struct block_device *bdev)
{
	put_device(&bdev->bd_device);
// [한국어] blkdev_get_no_open() 에서 kobject_get_unless_zero() 로 얻은
// device 참조를 반납한다. 참조 카운트를 원자적으로 감소시키고, 0이 되면
// 디바이스 release 경로가 트리거될 수 있다.
}

/*
 * [한국어]
 * bdev_writes_blocked - 이 block_device 에 대한 쓰기가 현재 차단된 상태인지 검사
 *
 * @bdev: 검사할 block_device.
 * @return: true 면 쓰기가 차단된 상태(더 이상 새로운 쓰기 open 을 허용하면
 *          안 됨), false 면 쓰기 가능.
 *
 * bd_writers 는 부호 있는(signed) 정수 카운터로, 이 파일의 쓰기 카운팅
 * 체계는 부호를 상태 플래그처럼 사용하는 트릭을 쓴다. 값이 양수(0보다 큼)
 * 이면 그 값은 현재 열려 있는 공유 쓰기 오프너 수를 의미하고, 값이 음수(0
 * 보다 작음)면 배타적 쓰기 차단(restrict-writes) 모드가 걸려 있다는 의미다
 * (음수 자체의 절대값은 의미가 없고 단지 누군가 bdev_block_writes() 를
 * 호출해 차단 상태로 만들었다는 부호만 의미가 있다). 0이면 쓰기 오프너도
 * 없고 차단도 아닌 중립 상태다. 이 함수는 그 부호만 검사해 차단 여부라는
 * 불리언으로 변환해 준다.
 * 실행 컨텍스트: bdev_may_open() 등 open_mutex 를 든 프로세스 컨텍스트에서
 * 호출된다고 가정한다. bd_writers 자체는 open_mutex 로 보호되는 값이라
 * 별도 원자 연산 없이 단순 비교로 읽는다.
 * 호출자: bdev_may_open()(본 파일). BLK_OPEN_WRITE 요청을 허용할지 검사할 때.
 * 피호출자: 없음(단순 비교).
 * 에러 처리: 해당 없음(순수 조회 함수).
 *
 * 호출 체인:
 *   bdev_may_open() -> [bdev_writes_blocked]
 */

static bool bdev_writes_blocked(struct block_device *bdev)
{
	return bdev->bd_writers < 0;
// [한국어] bd_writers 가 음수이면 배타적 쓰기 차단(restrict-writes) 상태가
// 걸려 있다는 뜻이다. 이 부호 트릭 덕분에 차단 여부와 공유 쓰기 오프너
// 수를 하나의 정수 필드로 함께 표현할 수 있다.
}

/*
 * [한국어]
 * bdev_block_writes - 공유 쓰기 카운터를 감소시켜 쓰기 차단 상태로 진입시킨다
 *
 * @bdev: 차단할 block_device.
 * @return: 없음(void).
 *
 * BLK_OPEN_RESTRICT_WRITES(배타적 쓰기 차단) 모드로 open 할 때, 이후의 모든
 * 공유 쓰기 open 시도를 막기 위해 bd_writers 를 음수로 만드는 함수다.
 * bd_writers-- 한 번으로 0에서 -1로 넘어가면 bdev_writes_blocked() 가 true
 * 를 반환하게 되어 부호 트릭이 성립한다. 여러 번 호출되면(중첩된
 * restrict-writes claim) 더 음수로 내려가며, bdev_unblock_writes() 가 그만큼
 * 다시 호출되어야 정확히 균형이 맞는다.
 * 실행 컨텍스트: bdev_claim_write_access() 가 open_mutex 를 든 상태로 호출.
 * bd_writers 갱신 자체는 단순 감소 연산이며 원자 연산이 아니므로 반드시 이
 * 뮤텍스로 직렬화된 컨텍스트에서만 호출되어야 한다(동시에 두 open 이 이
 * 값을 건드리면 카운트가 어긋난다).
 * 호출자: bdev_claim_write_access()(본 파일). BLK_OPEN_RESTRICT_WRITES
 * 모드로 open 이 확정될 때.
 * 피호출자: 없음(단순 산술 연산).
 * 에러 처리: 해당 없음. 실패할 수 없는 연산이다.
 *
 * 호출 체인:
 *   bdev_claim_write_access() -> [bdev_block_writes]
 */

static void bdev_block_writes(struct block_device *bdev)
{
	bdev->bd_writers--;
// [한국어] bd_writers 를 1 감소시켜 값을 음수 쪽으로 이동시킨다. 0
// 이었다면 -1 이 되어 bdev_writes_blocked() 가 true 를 반환하게 된다.
// open_mutex 로 보호되는 컨텍스트에서만 호출되어야 카운트가 정확하다.
}

/*
 * [한국어]
 * bdev_unblock_writes - bdev_block_writes() 의 역연산으로 쓰기 차단을 해제한다
 *
 * @bdev: 차단을 해제할 block_device.
 * @return: 없음(void).
 *
 * BLK_OPEN_RESTRICT_WRITES 로 열었던 오프너가 파일을 닫을 때, 자신이
 * bdev_block_writes() 로 걸어두었던 차단을 정확히 되돌리기 위해 호출된다.
 * bd_writers++ 로 음수를 0(또는 중첩 claim 이 있었다면 여전히 음수지만
 * 절대값이 줄어든 상태) 쪽으로 되돌린다.
 * 실행 컨텍스트: bdev_yield_write_access() 가 open_mutex 를 든 상태로 호출.
 * bdev_block_writes() 와 정확히 대칭되는 락 가정을 공유한다.
 * 호출자: bdev_yield_write_access()(본 파일). FMODE_WRITE_RESTRICTED
 * 플래그가 설정된 파일이 닫힐 때.
 * 피호출자: 없음(단순 산술 연산).
 * 에러 처리: 해당 없음.
 *
 * 호출 체인:
 *   bdev_yield_write_access() -> [bdev_unblock_writes]
 */

static void bdev_unblock_writes(struct block_device *bdev)
{
	bdev->bd_writers++;
// [한국어] bd_writers 를 1 증가시켜 bdev_block_writes() 가 걸었던 차단을
// 정확히 되돌린다. open 시 감소시킨 만큼 close 시 증가시켜야 하는
// 카운팅 규약의 반쪽이다.
}

/*
 * [한국어]
 * bdev_may_open - 현재 bdev 의 쓰기 상태와 요청된 open mode 를 비교해 open 허용 여부를 판단
 *
 * @bdev: open 하려는 block_device.
 * @mode: 요청된 BLK_OPEN_* 플래그 조합.
 * @return: true 면 open 진행 가능, false 면 거부(호출자가 -EBUSY 등으로 변환).
 *
 * 이 함수는 쓰기 배타성에 관한 두 가지 상호 배제 규칙을 검사한다. (1) 일반
 * 쓰기(BLK_OPEN_WRITE) 요청인데 이미 누군가 restrict-writes 로 쓰기를 차단해
 * 둔 상태라면 거부한다. (2) 반대로 배타적 쓰기(BLK_OPEN_RESTRICT_WRITES)
 * 요청인데 이미 공유 쓰기 오프너가 하나 이상(bd_writers > 0) 있다면, 그 공유
 * 쓰기 오프너를 쫓아낼 수 없으므로 배타성을 보장할 수 없어 거부한다. 즉
 * 배타적 쓰기와 공유 쓰기는 동시에 존재할 수 없다는 상호 배제 불변조건을
 * 이 함수가 지킨다. 다만 전역 스위치 bdev_allow_write_mounted(보통 sysctl
 * 이나 모듈 파라미터로 제어하며, 마운트된 디바이스에 대한 직접 쓰기를
 * 허용할지 결정)가 켜져 있으면 이 모든 검사를 건너뛰고 무조건 허용한다.
 * 이는 관리자가 의도적으로 안전장치를 끄는 것을 허용하는 탈출구다.
 * 동작 순서:
 *   1) bdev_allow_write_mounted 이면 즉시 true.
 *   2) 쓰기 요청인데 이미 차단 상태(bdev_writes_blocked)면 false.
 *   3) 배타적 쓰기 요청인데 공유 쓰기 오프너가 있으면 false.
 *   4) 그 외에는 true.
 * 실행 컨텍스트: bdev_open() 이 open_mutex 를 든 상태에서 호출한다. bd_writers
 * 읽기 자체는 이 뮤텍스로 보호되는 스냅샷이다.
 * 호출자: bdev_open()(본 파일). 실제 오프너 카운트를 갱신(bdev_claim_write_access)
 * 하기 직전에 허용 여부를 먼저 확인한다.
 * 피호출자: bdev_writes_blocked().
 * 에러 처리: false 반환 시 호출자가 -EBUSY 로 변환해 open 을 실패시킨다.
 *
 * 호출 체인:
 *   bdev_open() -> [bdev_may_open] -> bdev_writes_blocked()
 */

static bool bdev_may_open(struct block_device *bdev, blk_mode_t mode)
{
	if (bdev_allow_write_mounted)
// [한국어] 관리자가 마운트된 디바이스에 대한 직접 쓰기 허용 전역 스위치를
// 켜둔 경우, 아래의 모든 배타성 검사를 건너뛰고 무조건 허용한다(의도적
// 으로 안전장치를 해제한 상태).
		return true;
// [한국어] 전역 허용 스위치가 켜져 있으므로 검사 없이 통과.
	/* Writes blocked? */
	if (mode & BLK_OPEN_WRITE && bdev_writes_blocked(bdev))
// [한국어] 일반(공유) 쓰기를 요청했는데 이미 누군가 restrict-writes 로
// 쓰기를 차단해 둔 상태(bd_writers < 0)라면, 이 새 쓰기 open 은 배타적
// 쓰기 오프너와 공존할 수 없으므로 거부해야 한다.
		return false;
// [한국어] 차단 상태이므로 이 open 요청을 거부한다.
	if (mode & BLK_OPEN_RESTRICT_WRITES && bdev->bd_writers > 0)
// [한국어] 배타적 쓰기(restrict-writes)를 요청했는데 이미 공유 쓰기
// 오프너가 하나 이상(bd_writers > 0) 존재한다면, 그 오프너들을 강제로
// 내쫓을 수 없어 배타성을 보장할 수 없으므로 거부한다.
		return false;
// [한국어] 이미 공유 쓰기 오프너가 있어 배타적 쓰기 요청을 거부한다.
	return true;
// [한국어] 위 두 상호 배제 조건에 모두 해당하지 않으므로 open 을 허용한다.
}

/*
 * [한국어]
 * bdev_claim_write_access - open 이 확정된 후, 실제 쓰기 모드에 맞춰 카운터를 조정한다
 *
 * @bdev: open 을 진행 중인 block_device.
 * @mode: 확정된 BLK_OPEN_* 플래그 조합. bdev_may_open() 이 이미 허용 판단을
 *        마친 뒤이므로, 여기서는 다시 거부하지 않고 카운터만 갱신한다.
 * @return: 없음(void).
 *
 * bdev_may_open() 이 이 모드로 open 해도 되는가를 검사만 하는 함수였다면,
 * 이 함수는 그 판단 이후 실제로 open 이 확정되었을 때 bd_writers 카운터에
 * 그 결과를 반영하는 함수다. 두 모드를 구분한다. BLK_OPEN_RESTRICT_WRITES,
 * 즉 배타적 쓰기 차단 모드는 bdev_block_writes() 로 bd_writers 를 음수
 * 쪽으로 이동시켜 이후 다른 쓰기 open 을 막는다. 일반 BLK_OPEN_WRITE, 즉
 * 공유 쓰기 모드는 bd_writers 를 그냥 1 증가시켜 쓰기 오프너 수를 늘린다.
 * 이 두 경로는 상호 배타적(else if)이며, 각각 bdev_yield_write_access() 에서
 * FMODE_WRITE_RESTRICTED/FMODE_WRITE 플래그를 보고 정확히 대칭되는 방식으로
 * 되돌려진다.
 * 동작 순서:
 *   1) bdev_allow_write_mounted 이면 애초에 카운팅 체계 자체를 쓰지 않는
 *      전역 모드이므로 아무 것도 하지 않고 반환.
 *   2) BLK_OPEN_RESTRICT_WRITES 면 bdev_block_writes() 호출.
 *   3) 그렇지 않고 BLK_OPEN_WRITE 면 bd_writers 를 직접 증가.
 *   4) 둘 다 아니면(읽기 전용 open) 아무 것도 하지 않는다.
 * 실행 컨텍스트: bdev_open() 이 open_mutex 를 든 상태로, bdev_may_open() 이
 * 허용을 반환한 직후에만 호출되어야 한다. 이 함수 자체는 재검사를 하지
 * 않으므로 순서가 뒤바뀌면 카운터가 잘못될 수 있다.
 * 호출자: bdev_open()(본 파일).
 * 피호출자: bdev_block_writes().
 * 에러 처리: 해당 없음(반환값도 없고 실패할 연산도 없음). 실패 여부 판단은
 * 이미 bdev_may_open() 이 끝냈다.
 *
 * 호출 체인:
 *   bdev_open() -> [bdev_claim_write_access] -> bdev_block_writes()
 */

static void bdev_claim_write_access(struct block_device *bdev, blk_mode_t mode)
{
	if (bdev_allow_write_mounted)
// [한국어] 전역 스위치가 켜져 있으면 bd_writers 기반 카운팅 체계 자체가
// 무의미하므로(bdev_may_open() 도 항상 true 였음) 아무 것도 하지 않고
// 그냥 반환한다.
		return;
// [한국어] 카운터 갱신 없이 종료.

	/* Claim exclusive or shared write access. */
	if (mode & BLK_OPEN_RESTRICT_WRITES)
// [한국어] 배타적 쓰기 차단 모드로 open 이 확정되었으므로 아래에서 차단
// 카운터를 걸어야 한다.
		bdev_block_writes(bdev);
// [한국어] bd_writers 를 음수 쪽으로 이동시켜, 이후의 다른 쓰기 open
// (bdev_may_open() 검사)을 막는다.
	else if (mode & BLK_OPEN_WRITE)
// [한국어] 배타적 모드는 아니지만 일반 쓰기 모드로 open 이 확정된 경우,
// 공유 쓰기 오프너 수를 늘려야 한다.
		bdev->bd_writers++;
// [한국어] 공유 쓰기 오프너 카운트를 1 증가시킨다. 이 값이 나중에
// bdev_may_open() 에서 이미 공유 쓰기 오프너가 있다는 판단 근거로
// 쓰인다.
}

/*
 * [한국어]
 * bdev_unclaimed - 이 struct file 이 holder claim 없이 열린 일반 open 인지 검사
 *
 * @bdev_file: 검사할 struct file(파일 디스크립터에 대응하는 커널 객체).
 * @return: true 면 holder claim 이 없는 일반 open(예: 단순 조회 목적의
 *          open), false 면 bd_finish_claiming() 등을 통해 holder 가 지정된
 *          배타적/추적 가능한 open.
 *
 * bdev open 경로는 두 갈래로 나뉜다. (a) 특정 holder(예: 파일시스템, LVM)가
 * 자신을 식별하며 배타적으로 여는 경우. 이때 file->private_data 는 그
 * holder 관련 컨텍스트(bd_holder 클레임 정보를 담은 별도 구조체 등)를
 * 가리킨다. (b) holder 없이 그냥 여는 일반적인 open. 이 경우 커널은
 * private_data 를 특별한 값이 없다는 표시로 그 파일이 속한 bdev_inode
 * 자기 자신의 주소로 채워 넣는 관례(sentinel 값 트릭)를 쓴다. 이 함수는
 * private_data 가 정확히 자기 자신의 bdev_inode 주소와 같은지 비교해서 이
 * sentinel 패턴에 해당하는지, 즉 holder claim 이 없는 open 인지를 판별한다.
 * 실행 컨텍스트: bdev_yield_write_access() 등 close(2) 경로에서 호출되며,
 * 별도 락 불필요(file->private_data 는 open 시점에 고정되어 이후 바뀌지
 * 않는 불변 필드다).
 * 호출자: bdev_yield_write_access()(본 파일). claim 없는 open 이었다면 쓰기
 * 카운터 반납 로직 자체를 건너뛰기 위해 사용.
 * 피호출자: BDEV_I() 매크로(inode 를 bdev_inode 컨테이너로 변환).
 * 에러 처리: 해당 없음(순수 조회, 실패 불가).
 *
 * 호출 체인:
 *   bdev_yield_write_access() -> [bdev_unclaimed]
 */

static inline bool bdev_unclaimed(const struct file *bdev_file)
{
	return bdev_file->private_data == BDEV_I(bdev_file->f_mapping->host);
// [한국어] private_data 가 이 파일이 속한 bdev_inode 자기 자신의 주소와
// 같다면(open 시 holder 를 지정하지 않은 일반 open 이라는 sentinel
// 표시), holder claim 이 없는 open 이라고 판단한다. f_mapping->host 로
// 이 파일의 address_space 가 속한 inode 를 얻고, BDEV_I() 로 그 inode
// 를 감싸는 bdev_inode 컨테이너 주소로 변환해 private_data 와 비교한다
// (container_of 역할).
}

/*
 * [한국어]
 * bdev_yield_write_access - close 시점에 open 때 반영했던 쓰기 접근 카운터를 정확히 되돌린다
 *
 * @bdev_file: 닫히는 중인 struct file. f_mode 플래그로 이 파일이 어떤 모드로
 *             열려 있었는지(FMODE_WRITE/FMODE_WRITE_RESTRICTED)를 판별한다.
 * @return: 없음(void).
 *
 * bdev_claim_write_access() 가 open 시점에 bd_writers 카운터에 반영했던
 * 변화를, close 시점에 정확히 역산해서 되돌리는 대칭 함수다. open 쪽이
 * mode(BLK_OPEN_*)를 보고 분기했다면, close 쪽은 file->f_mode 에 새겨진
 * FMODE_* 플래그(open 결과가 VFS 계층에 기록해 둔 실제 모드)를 보고
 * 분기한다는 점이 다르다. open 시점의 요청(mode)이 최종적으로 f_mode 로
 * 변환되어 파일에 고정되기 때문에, close 시점에는 mode 대신 f_mode 를
 * 참조하는 것이 정확하다.
 * 동작 순서:
 *   1) bdev_allow_write_mounted 전역 스위치가 켜져 있으면 애초에 open
 *      시점에 카운팅을 하지 않았으므로 되돌릴 것도 없이 반환.
 *   2) bdev_unclaimed() 로 이 파일이 holder claim 없는 일반 open 이었는지
 *      확인한다. 그렇다면 아래의 f_mode 기반 반환 로직을 건너뛴다.
 *   3) file_bdev() 로 이 파일이 가리키는 실제 block_device 를 얻는다.
 *   4) f_mode 에 FMODE_WRITE_RESTRICTED 가 있으면(open 시 배타적 쓰기 차단
 *      모드로 확정되었던 파일) bdev_unblock_writes() 로 그 차단을 해제한다.
 *      bdev_block_writes() 의 정확한 역연산이다.
 *   5) 그렇지 않고 FMODE_WRITE 가 있으면(open 시 일반 공유 쓰기 모드로
 *      확정되었던 파일) bd_writers 를 직접 감소시킨다. bdev_claim_write_access()
 *      의 "bd_writers++" 의 정확한 역연산이다.
 *   6) 둘 다 아니면(읽기 전용으로 열렸던 파일) 아무 것도 하지 않는다.
 * 실행 컨텍스트: bdev_release()/bdev_fput() 등 close(2) 경로에서 open_mutex
 * 를 든 상태로 호출된다. bd_writers 갱신은 이 뮤텍스로 직렬화된 컨텍스트
 * 에서만 안전하다(bdev_block_writes/bdev_unblock_writes 자체가 원자 연산이
 * 아닌 단순 증감이므로).
 * 호출자: bdev_release()(본 파일). 파일이 실제로 닫히기 직전.
 * 피호출자: bdev_unclaimed(), file_bdev(), bdev_unblock_writes().
 * 에러 처리: 해당 없음(반환값 없는 정리 함수, 실패 불가). 다만 open 쪽과
 * 대칭이 깨지면(예: open 때는 claim 했는데 close 때 이 함수가 호출되지
 * 않으면) bd_writers 카운터가 영구히 어긋나므로, 이 함수는 반드시 파일이
 * 닫히는 모든 경로에서 정확히 한 번 호출되어야 한다.
 *
 * 호출 체인:
 *   bdev_release() -> [bdev_yield_write_access] -> bdev_unclaimed() / bdev_unblock_writes()
 */

static void bdev_yield_write_access(struct file *bdev_file)
{
	struct block_device *bdev;
// [한국어] 이 파일이 가리키는 실제 block_device 를 담을 지역 변수. 아래
// 에서 file_bdev() 로 채워진다.

	if (bdev_allow_write_mounted)
// [한국어] 전역 허용 스위치가 켜져 있던 동안에는 open 시점에도 bd_writers
// 카운팅을 건드리지 않았으므로(bdev_claim_write_access() 참조), close
// 시점에도 되돌릴 상태가 없다.
		return;
// [한국어] 카운터를 건드리지 않고 종료.

	if (bdev_unclaimed(bdev_file))
// [한국어] holder claim 없이 열렸던 일반 open 이었는지 확인한다. 이
// sentinel 패턴에 해당하면 open 카운팅 대상이 아니었을 가능성이 높으
// 므로 아래의 f_mode 기반 반환 로직을 건너뛴다.
		return;
// [한국어] claim 없는 open 이었으므로 카운터 반납 없이 종료.

	bdev = file_bdev(bdev_file);
// [한국어] struct file 로부터 실제 block_device 포인터를 얻는다. 이후
// f_mode 플래그에 따라 이 bdev 의 bd_writers 를 갱신한다.

	if (bdev_file->f_mode & FMODE_WRITE_RESTRICTED)
// [한국어] 이 파일이 open 시 배타적 쓰기 차단(restrict-writes) 모드로
// 확정되어 FMODE_WRITE_RESTRICTED 플래그가 f_mode 에 새겨져 있는 경우,
// bdev_claim_write_access() 가 bdev_block_writes() 를 호출했던 것과
// 정확히 대칭인 반대 연산이 필요하다.
		bdev_unblock_writes(bdev);
// [한국어] bd_writers 를 1 증가시켜 배타적 쓰기 차단을 해제한다.
// bdev_block_writes() 가 걸었던 차단을 정확히 되돌린다.
	else if (bdev_file->f_mode & FMODE_WRITE)
// [한국어] 배타적 모드는 아니었지만 일반 공유 쓰기(FMODE_WRITE)로 열려
// 있던 경우, bdev_claim_write_access() 의 "bd_writers++" 를 되돌려야
// 한다.
		bdev->bd_writers--;
// [한국어] 공유 쓰기 오프너 카운트를 1 감소시켜, open 때 증가시켰던
// 것을 정확히 상쇄한다.
}

/*
 * [한국어]
 * bdev_open - block_device 를 열고, holder 가 주어지면 exclusive(배타적) claim 까지 수행한다.
 *
 * @bdev: 열려는 대상 block_device. NVMe namespace 전체(gendisk 의 part0)일
 *        수도 있고, 그 위에 만들어진 파티션(bd_partno != 0)일 수도 있다.
 *        호출자가 이미 blkdev_get_no_open() 등으로 struct block_device 자체에
 *        대한 참조(디바이스 kobject 참조)를 올려서 넘겨준다 — 이 함수는 그
 *        참조를 소비하지 않고, 별도로 "open 카운터"(bd_openers 등)만 올린다.
 * @mode: BLK_OPEN_READ / BLK_OPEN_WRITE / BLK_OPEN_NDELAY / BLK_OPEN_EXCL /
 *        BLK_OPEN_RESTRICT_WRITES / BLK_OPEN_WRITE_IOCTL 의 비트 조합.
 *        holder 가 NULL 이 아니면 함수 내부에서 BLK_OPEN_EXCL 이 강제로
 *        추가되므로, 호출자가 미리 EXCL 비트를 직접 세팅해서 넘길 필요는
 *        없다(오히려 그러면 버그로 간주되어 -EIO).
 * @holder: exclusive claim 을 요청하는 주체를 가리키는 임의의 불투명(opaque)
 *          포인터 — 보통 호출자 서브시스템의 정적 심볼 주소나 struct 포인터를
 *          그대로 사용해 "누가 이 디바이스를 배타적으로 쓰고 있는지"를
 *          식별한다. NULL 이면 배타적 접근이 아닌 일반(공유) open.
 * @hops: holder 전용 콜백 테이블(예: 미디어 변경/제거 통지를 받는
 *        ->mark_dead 등). holder 가 NULL 이면 사용되지 않는다.
 * @bdev_file: 호출자가 이미 할당했지만 아직 필드가 채워지지 않은 struct
 *             file. 이 함수가 f_flags/f_mode/f_mapping/f_wb_err/private_data
 *             를 채워 완성한다.
 * @return: 0 이면 open 성공 — 이후 @bdev_file 을 통해 실제 read/write/ioctl
 *          이 NVMe 큐까지 흘러갈 수 있다. 음수 errno(-EIO/-EBUSY/-ENXIO 등)면
 *          실패 — 호출자(bdev_file_open_by_dev)는 이 값을 ERR_PTR 로 감싸
 *          반환하고, 이미 할당해 둔 @bdev_file 은 private_data 에 에러를
 *          표시한 뒤 fput() 으로 반드시 정리해야 한다(이 함수 자체는
 *          @bdev_file 을 해제하지 않는다).
 *
 * 이 함수는 파일 전체에서 가장 복잡한 함수로, block_device 를 실제로
 * "연다"는 의미를 부여하는 단 하나의 지점이다: exclusive claim 시도,
 * 미디어 변경 이벤트 폴링 차단/해제, disk 생존 여부/드라이버 모듈 참조
 * 검사, 쓰기 가능 여부 검사, open 카운터 증가(파티션/전체 구분),
 * 쓰기 카운터 반영, claim 확정, struct file 필드 초기화까지 모두 이
 * 함수 하나가 정해진 순서로 수행해야 한다. 이 순서가 뒤바뀌면(예: claim이
 * 끝나기 전에 이벤트 차단을 풀어버리는 경우) 다른 프로세스가 아직 완전히
 * 열리지 않은 디바이스의 중간 상태를 관찰할 수 있어 위험하다.
 *
 * 동작 순서:
 *  a) holder 가 있으면 mode 에 BLK_OPEN_EXCL 을 강제로 세팅하고
 *     bd_prepare_to_claim() 으로 exclusive claim 을 "예약"한다(다른 holder 가
 *     이미 있으면 -EBUSY, claim 진행 중이면 그 완료까지 대기 후 재시도).
 *     holder 가 없는데 BLK_OPEN_EXCL 이 이미 설정돼 들어오면 그 자체가
 *     호출자 버그이므로 WARN_ON_ONCE 로 커널 로그에 남기고 -EIO 로 즉시
 *     반환한다(이 시점은 아직 어떤 자원도 잡지 않았으므로 정리할 것이 없다).
 *  b) disk_block_events() 로, open 처리가 끝날 때까지 이 gendisk 에 대한
 *     미디어 변경 등 이벤트 폴링 워크를 일시 차단한다 — open 도중 디스크
 *     상태가 아직 확정되지 않았는데 이벤트 핸들러가 끼어드는 것을 막는다.
 *  c) disk->open_mutex 를 잡아 같은 디스크에 대한 동시 open/close 를
 *     직렬화한 뒤, disk_live() 로 gendisk 가 아직 GD_DEAD 상태로 빠지지
 *     않았는지(즉 디스크가 제거되는 중이 아닌지) 확인하고,
 *     try_module_get() 으로 드라이버 모듈이 언로드되지 않도록 참조를
 *     하나 얻는다. 이 둘 중 하나라도 실패하면 아직 module 참조를 얻지
 *     못한 상태이므로 abort_claiming 레이블로 바로 이동한다.
 *  d) bdev_may_open() 으로 현재 쓰기 카운터 상태와 요청한 mode 가 충돌하지
 *     않는지 검사하고(-EBUSY), 파티션이면 blkdev_get_part(), 전체
 *     디바이스면 blkdev_get_whole() 로 실제 open 카운터(bd_openers 등)를
 *     증가시킨다. 이 단계의 실패는 이미 module 참조를 얻은 뒤이므로
 *     put_module 레이블로 이동한다.
 *  e) 성공하면 bdev_claim_write_access() 로 쓰기 카운터(bd_writers)를
 *     반영하고, holder 가 있으면 bd_finish_claiming() 으로 a)에서 예약해
 *     둔 claim 을 확정(bd_holder/bd_holders 확정)한다. 이어서, 이번 open 이
 *     "쓰기 모드"이고 아직 이 bdev 에 BD_WRITE_HOLDER 플래그가 없으며,
 *     디스크가 DISK_EVENT_FLAG_BLOCK_ON_EXCL_WRITE 를 요청한 경우에 한해
 *     BD_WRITE_HOLDER 플래그를 세우고 unblock_events 를 false 로 바꾼다.
 *     즉, "배타적 쓰기 holder가 하나라도 존재하는 동안은, 함수가 끝난
 *     뒤에도 계속 이벤트 폴링 차단 상태를 유지"하겠다는 의미다(쓰기
 *     배타적 소유자가 활동 중인 동안 미디어 변경 이벤트로 인해 상태가
 *     흔들리는 것을 막기 위함). 이 플래그는 최초의 write holder 에게만
 *     세팅되고, 이후 같은 디바이스에 다른 write holder 가 더 붙어도
 *     다시 세팅되지 않는다(정확한 참조 카운팅 대신 "누군가 하나라도
 *     있으면 유지"라는 근사치를 택함 — 아래 원문 주석 참고).
 *  f) open_mutex 를 풀고, unblock_events 가 여전히 true 인 경우에만
 *     disk_unblock_events() 로 이벤트 폴링을 재개한다(e)에서 false 로
 *     바뀌었다면 재개하지 않고 차단 상태를 유지한 채 반환).
 *  g) @bdev_file 의 f_flags 에 O_LARGEFILE 을, f_mode 에 FMODE_CAN_ODIRECT 를
 *     무조건 세팅하고, bdev_nowait() 이 참이면(하위 큐가 BLK_FEAT_NOWAIT 를
 *     지원하면) FMODE_NOWAIT 를, BLK_OPEN_RESTRICT_WRITES 이면
 *     FMODE_WRITE_RESTRICTED 를 추가로 세팅한다. f_mapping 을
 *     bdev->bd_mapping(페이지 캐시)에 연결해 이후 buffered I/O 가 이
 *     address_space 를 쓰게 하고, f_wb_err 를 filemap_sample_wb_err() 로
 *     현재 시점 기준 샘플링해 이후 fsync 가 "이 open 이후 발생한" writeback
 *     에러만 보고하게 하며, private_data 에 holder 를 저장해 release() 가
 *     같은 holder 로 claim 을 해제할 수 있게 한다.
 *
 * 에러 경로(goto 레이블)와 자원 해제 순서 — "획득의 정확히 역순"으로만
 * 되돌린다(더 앞서 얻지 않은 자원은 절대 건드리지 않는다):
 *  - put_module: d) 단계, 즉 bdev_may_open()/blkdev_get_part()/
 *    blkdev_get_whole() 실패 시에만 도달. 이 시점엔 이미 c)에서 module
 *    참조를 얻었으므로 module_put() 으로 그 참조 하나만 되돌리고, 코드
 *    흐름은 그대로 아래 abort_claiming 레이블의 코드로 이어진다(별도
 *    goto 없이 순차 진행 — 즉 put_module 은 abort_claiming 의 앞부분
 *    역할도 겸한다).
 *  - abort_claiming: c) 단계, 즉 disk_live() 실패 또는 try_module_get()
 *    실패 시 여기로 직접 도달한다(이 갈래는 module 참조를 아직 얻지
 *    못했으므로 module_put 이 필요 없어 put_module 을 거치지 않는다).
 *    holder 가 있으면 bd_abort_claiming() 으로 a)에서 예약해 둔 claim
 *    진행 상태를 취소해, 다른 대기자가 진행할 수 있게 한다.
 *  - 공통 꼬리: put_module/abort_claiming 두 갈래 모두 마지막에
 *    mutex_unlock(&disk->open_mutex) 과 disk_unblock_events(disk) 를
 *    실행한다 — 이는 각각 c)의 mutex_lock, b)의 disk_block_events 를
 *    정확히 되돌리는 것이다. 정리하면 획득 순서는
 *    [claim 예약 -> event-block -> mutex -> module -> claim 확정] 이고,
 *    실패 지점 이전에 얻은 자원만 정확히 역순으로 풀린다 — 예를 들어
 *    d) 단계 실패는 [module, mutex, event-block, claim 예약] 네 가지를
 *    모두 되돌려야 하므로 module_put -> (abort_claiming 코드 계속 실행) ->
 *    bd_abort_claiming -> mutex_unlock -> disk_unblock_events 순서로
 *    정확히 반대 순서로 풀린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트에서만 호출되며 sleep 가능하다
 * (mutex_lock, bd_prepare_to_claim 내부의 스케줄링 대기, 모듈 로드 등).
 * disk->open_mutex 가 같은 gendisk 에 대한 동시 open/close 를 직렬화하고,
 * bdev_lock(bd_prepare_to_claim/bd_finish_claiming/bd_abort_claiming 내부에서
 * 사용하는 전역 락)이 claim 상태 전이를 보호한다. 이 함수 자체는 재진입
 * 불가 — 이미 open_mutex 를 든 채로 재호출하면 자기 자신과 교착(deadlock)한다.
 *
 * caller: bdev_file_open_by_dev()(본 파일, dev_t 로 여는 경로),
 * 그리고 blkdev_open()(fs 쪽 def_blk_fops.open 콜백, 사용자 open(2)
 * 시스템호출 경로)이 내부적으로 이 함수와 동등한 절차를 거친다.
 * callee: bd_prepare_to_claim, disk_block_events, disk_live,
 * try_module_get, bdev_may_open, blkdev_get_part, blkdev_get_whole,
 * bdev_claim_write_access, bd_finish_claiming, disk_unblock_events,
 * bdev_nowait, filemap_sample_wb_err, module_put, bd_abort_claiming.
 * 에러 발생 시 위 "에러 경로" 절에서 설명한 두 레이블 중 하나로 진입해
 * 정리한 뒤 음수 errno 를 반환한다.
 *
 * 호출 체인:
 *   bdev_file_open_by_dev → [bdev_open] → blkdev_get_whole / blkdev_get_part
 */

/**
 * bdev_open - open a block device
 * @bdev: block device to open
 * @mode: open mode (BLK_OPEN_*)
 * @holder: exclusive holder identifier
 * @hops: holder operations
 * @bdev_file: file for the block device
 *
 * Open the block device. If @holder is not %NULL, the block device is opened
 * with exclusive access.  Exclusive opens may nest for the same @holder.
 *
 * CONTEXT:
 * Might sleep.
 *
 * RETURNS:
 * zero on success, -errno on failure.
 */
int bdev_open(struct block_device *bdev, blk_mode_t mode, void *holder,
	      const struct blk_holder_ops *hops, struct file *bdev_file)
{
	bool unblock_events = true;
// [한국어] 이 open 처리가 끝난 뒤 disk_unblock_events() 로 이벤트 폴링을
// 재개할지 여부를 담는 지역 플래그. 기본값 true(재개함) — 아래 e) 단계에서
// "최초의 배타적 쓰기 holder" 조건을 만족하면 false 로 바뀌어, 함수가
// 끝나도 이벤트 차단 상태가 유지된다. 이 함수 안에서만 쓰이는 스택 변수
// 이므로 별도 동기화 없이 안전.
	struct gendisk *disk = bdev->bd_disk;
// [한국어] bdev 가 속한 gendisk(NVMe namespace/디스크 전체를 표현하는
// 커널 객체)를 미리 꺼내 둔다. 파티션을 열더라도 bd_disk 는 항상 전체
// 디스크를 가리키므로, 아래에서 disk->open_mutex/disk->fops 등을 파티션
// 여부와 무관하게 일관되게 사용할 수 있다.
	int ret;
// [한국어] 각 단계의 성공/실패 코드를 담는 지역 변수. 아래에서 -ENXIO
// 기본값, -EBUSY 로 재설정, blkdev_get_part/whole() 반환값 대입 등으로
// 계속 갱신되며, 최종적으로 이 값이 함수 자체의 반환값이 된다.

	if (holder) {
// [한국어] holder 가 지정됐다는 것은 배타적(exclusive) open 을 요청한
// 것이다. 일반 공유 open(holder == NULL)과 달리, 아래에서 claim 절차를
// 먼저 밟아야 한다.
		mode |= BLK_OPEN_EXCL;
// [한국어] 배타적 open 임을 나타내는 BLK_OPEN_EXCL 비트를 강제로 세팅.
// 호출자가 깜빡하고 EXCL 을 안 세팅해도 holder 가 있으면 항상 배타적
// open 으로 취급되도록 보장한다 — 이후 bdev_may_open()/
// bdev_claim_write_access() 가 이 비트를 보고 판단한다.
		ret = bd_prepare_to_claim(bdev, holder, hops);
// [한국어] 전역 bdev_lock 아래에서 이 holder 로 claim 이 가능한지 검사하고,
// 가능하면 whole->bd_claiming 에 holder 를 표시해 "claim 진행 중" 상태로
// 만든다. 이미 다른 holder 가 점유 중이면 -EBUSY, 다른 프로세스가 claim
// 진행 중이면 그 완료를 기다렸다가 재시도한다(내부적으로 스케줄링 대기).
		if (ret)
// [한국어] claim 자체가 실패(주로 -EBUSY) — 아직 disk_block_events 도
// 부르지 않았고 어떤 자원도 잡지 않았으므로 별도 정리 없이 즉시 반환.
			return ret;
// [한국어] claim 예약 실패를 그대로 호출자에게 전달. 이 지점 이후의
// module/mutex/event-block 정리 로직은 전혀 거치지 않는다.
	} else {
// [한국어] holder 가 NULL — 배타적 접근을 요구하지 않는 일반 open 경로.
		if (WARN_ON_ONCE(mode & BLK_OPEN_EXCL))
// [한국어] holder 없이 BLK_OPEN_EXCL 이 세팅돼 들어오는 것은 호출자가
// claim 절차 없이 배타적 접근을 요청한 모순 상태 — 커널 버그이므로
// WARN_ON_ONCE 로 1회만 콜스택을 커널 로그에 남긴다.
			return -EIO;
// [한국어] 호출자 버그를 알리는 -EIO 즉시 반환. 아직 아무 자원도 잡지
// 않았으므로 정리할 것이 없다.
	}

	disk_block_events(disk); 	/* [한국어] open 처리 도중 미디어 변경 등 디스크 이벤트 폴링을 일시 차단 — 아직 상태가 확정되지 않은 open 중간 과정을 이벤트 핸들러가 관찰하지 못하게 한다. */
// [한국어] 이 시점부터 disk_unblock_events() 가 호출되기 전까지(아래
// f) 단계 또는 에러 경로의 공통 꼬리) 이 gendisk 에 대한 이벤트 폴링
// 워크(disk_check_events 등)가 새로 예약되지 않는다. bd_disk 단위로
// 걸리므로 파티션을 열어도 전체 디스크의 이벤트가 함께 차단된다.

	mutex_lock(&disk->open_mutex);
// [한국어] 같은 gendisk 에 대한 동시 open/close 를 직렬화하는 뮤텍스.
// disk_live()/try_module_get() 검사와 blkdev_get_part/whole() 에 의한
// open 카운터 증가가 이 뮤텍스 아래에서 원자적으로 일어나야, "디스크가
// 막 제거되는 도중에 open 카운터만 증가해버리는" 경쟁을 막을 수 있다.
	ret = -ENXIO;
// [한국어] disk_live() 검사가 실패할 경우를 대비한 기본 에러 코드.
// ENXIO(No such device or address)는 "디바이스가 이미 사라졌다"는 의미.
	if (!disk_live(disk))
// [한국어] gendisk 가 아직 GD_DEAD 로 표시되지 않았는지(즉 등록 해제나
// 제거가 진행 중이 아닌지) 확인. bdev_unhash()/del_gendisk() 등이 이미
// 이 디스크를 제거했다면 false 를 반환한다.
		goto abort_claiming;
// [한국어] 디스크가 이미 죽은 상태 — 이 시점엔 아직 module 참조를 얻지
// 않았으므로 module_put 없이 바로 claim 취소 + mutex/event 정리만
// 수행하는 abort_claiming 으로 이동.
	if (!try_module_get(disk->fops->owner))
// [한국어] 드라이버 모듈(NVMe 드라이버 등)의 참조 카운트를 하나 올려,
// open 이 끝나기 전에 모듈이 언로드되는 것을 방지한다. 모듈이 이미
// 언로드 진행 중(refcount 가 0으로 수렴)이면 실패한다.
		goto abort_claiming;
// [한국어] 모듈 참조 획득 실패 — 역시 아직 module_put 할 것이 없으므로
// abort_claiming 으로 바로 이동한다(module_put 을 거치는 put_module 이
// 아님에 유의).
	ret = -EBUSY;
// [한국어] 이후 bdev_may_open()/blkdev_get_part()/blkdev_get_whole() 실패
// 시 사용할 기본 에러 코드로 갱신. EBUSY(Device or resource busy)는
// "이미 다른 쓰기 상태와 충돌한다"는 의미.
	if (!bdev_may_open(bdev, mode))
// [한국어] 요청한 mode(특히 BLK_OPEN_WRITE/BLK_OPEN_RESTRICT_WRITES)가
// 현재 bdev->bd_writers 로 표현되는 기존 쓰기 상태와 충돌하지 않는지
// 검사한다(예: 이미 restrict-write 로 열려 있는데 또 쓰기로 열려는 경우).
		goto put_module;
// [한국어] 여기서부터는 이미 module 참조를 얻은 뒤이므로, module_put()
// 을 거치는 put_module 레이블로 이동해야 참조가 새지 않는다.
	if (bdev_is_partition(bdev))
// [한국어] bdev 가 파티션(bd_partno != 0)인지, 전체 디스크(part0)인지에
// 따라 open 카운터를 증가시키는 하위 함수가 갈린다.
		ret = blkdev_get_part(bdev, mode);
// [한국어] 파티션 전용 open 처리 — 파티션 자체의 bd_openers 와, 필요하면
// 상위 전체 디바이스의 open_partitions/bd_openers 도 함께 갱신한다.
	else
// [한국어] 파티션이 아니라 전체 NVMe namespace/디스크 자체를 여는 경우.
		ret = blkdev_get_whole(bdev, mode);
// [한국어] 전체 디바이스 open 처리 — bd_openers 증가, 최초 open 이면
// 드라이버의 ->open() 콜백 호출 등을 수행한다(정의는 이 파일의 다른 곳).
	if (ret)
// [한국어] blkdev_get_part/whole() 이 반환한 에러(예: 드라이버 ->open()
// 실패)를 검사한다.
		goto put_module;
// [한국어] open 카운터를 아직 이 호출에서 성공적으로 올리지 못했으므로
// module 참조만 되돌리면 되는 put_module 로 이동한다.
	bdev_claim_write_access(bdev, mode);
// [한국어] 실제로 open 카운터가 올라간 뒤, mode 에 따라 bd_writers
// 카운터를 조정해 "이 open 이 쓰기 접근을 하나 더 차지했음"을 전역
// 상태에 반영한다(RESTRICT_WRITES 면 음수 방향, 일반 WRITE 면 양수 방향).
	if (holder) {
// [한국어] 배타적 open 이었다면 a) 단계에서 claim 을 "예약"만 해 뒀으므로,
// open 카운터 반영까지 모두 끝난 지금 시점에 claim 을 최종 확정한다.
		bd_finish_claiming(bdev, holder, hops);
// [한국어] bdev_lock 아래에서 whole->bd_holders/bdev->bd_holders 를
// 증가시키고 bdev->bd_holder/bd_holder_ops 를 실제 holder/hops 로
// 확정한 뒤, bd_clear_claiming() 으로 claim-진행-중 표시를 해제하고
// 대기자를 깨운다.

		/*
		 * Block event polling for write claims if requested.  Any write
		 * holder makes the write_holder state stick until all are
		 * released.  This is good enough and tracking individual
		 * writeable reference is too fragile given the way @mode is
		 * used in blkdev_get/put().
		 */
// [한국어] 위 영어 원문 설명: 쓰기 목적의 claim 이면 요청에 따라 이벤트
// 폴링을 계속 차단한다. 어떤 write holder 든 하나라도 있으면
// "write_holder" 상태가 계속 유지되며, 모든 holder 가 해제되어야 풀린다.
// 개별 writable 참조를 정교하게 추적하는 대신 이 정도 "존재 여부"만 보는
// 근사치로 충분하다고 판단한 이유는, mode 값이 blkdev_get()/blkdev_put()
// 전반에서 재사용되는 방식이 그런 정교한 참조 추적을 하기엔 너무
// 취약(fragile)하기 때문이다. 요컨대 "배타적 쓰기 holder 가 존재하는 한,
// 미디어 변경 이벤트 폴링이 그 holder 의 관찰을 방해하지 않도록 계속
// 막아 둔다"는 정책이다.
		if ((mode & BLK_OPEN_WRITE) &&
// [한국어] 이번 open 이 쓰기 모드를 포함하는지 검사 — 읽기 전용 claim
// 이라면 아래 이벤트 차단 유지 로직 자체가 적용되지 않는다.
		    !bdev_test_flag(bdev, BD_WRITE_HOLDER) &&
// [한국어] 이 bdev 에 이미 BD_WRITE_HOLDER 플래그가 서 있지 않은지 확인
// — 즉 "최초의" 쓰기 holder 인 경우에만 아래 로직을 한 번 적용하기 위한
// 조건이다(이미 세팅돼 있으면 다시 세팅할 필요도, unblock_events 를
// 다시 false 로 만들 필요도 없다).
		    (disk->event_flags & DISK_EVENT_FLAG_BLOCK_ON_EXCL_WRITE)) {
// [한국어] 드라이버/디스크가 "배타적 쓰기 open 동안 이벤트 폴링을 막아
// 달라"는 정책 플래그를 요청했는지 확인한다. 세 조건이 모두 참일 때만
// 아래 블록에 진입한다.
			bdev_set_flag(bdev, BD_WRITE_HOLDER);
// [한국어] "쓰기 holder 가 존재함" 플래그를 세운다. 이후 다른 open 이
// 들어와도 이 플래그가 이미 서 있으면 위 두 번째 조건에서 걸러져
// 재적용되지 않는다.
			unblock_events = false;
// [한국어] 함수 종료 시 disk_unblock_events() 를 호출하지 않도록 표시
// — 즉 이 open 이 끝난 뒤에도 disk_block_events() 로 걸어 둔 이벤트
// 차단 상태를 그대로 유지한다.
		}
	}
	mutex_unlock(&disk->open_mutex);
// [한국어] open_mutex 해제 — disk_live/try_module_get 검사부터 claim
// 확정까지의 임계 구간이 끝났음을 의미한다. 이 시점 이후로는 다른
// 스레드가 같은 disk 에 대해 open_mutex 를 잡고 진행할 수 있다.

	if (unblock_events)
// [한국어] e) 단계에서 false 로 바뀌지 않았다면(즉 배타적 쓰기 holder
// 유지 정책이 적용되지 않았다면) 정상적으로 이벤트 폴링을 재개한다.
		disk_unblock_events(disk);
// [한국어] disk_block_events() 로 걸어 둔 차단을 해제해 미디어 변경 등
// 이벤트 폴링 워크가 다시 예약될 수 있게 한다.

	bdev_file->f_flags |= O_LARGEFILE;
// [한국어] 블록 디바이스는 항상 큰 오프셋(64비트 LBA 기반)을 다루므로
// O_LARGEFILE 을 무조건 세팅해, 32비트 오프셋 제한에 걸리지 않게 한다.
	bdev_file->f_mode |= FMODE_CAN_ODIRECT;
// [한국어] 이 파일이 O_DIRECT(페이지 캐시를 우회하는 직접 I/O)를 지원함을
// VFS 에 알린다 — NVMe 는 애초에 direct I/O 를 자연스럽게 지원하는 계층.
	if (bdev_nowait(bdev)) 	/* [한국어] 하위 요청 큐가 BLK_FEAT_NOWAIT 를 광고하면(즉시 -EAGAIN 반환이 가능한 큐) FMODE_NOWAIT 를 세팅해, io_uring 등 IOCB_NOWAIT 제출 경로가 이 파일에서도 논블로킹 제출을 시도할 수 있게 한다. */
// [한국어] bdev_nowait() 는 bdev->bd_disk->queue->limits.features 에
// BLK_FEAT_NOWAIT 비트가 서 있는지를 확인하는 인라인 함수(대부분의
// NVMe 큐는 이 특성을 가진다) — 큐가 즉시 처리 불가능한 상황에서 블로킹
// 대신 -EAGAIN 을 돌려줄 수 있음을 의미한다.
		bdev_file->f_mode |= FMODE_NOWAIT;
// [한국어] FMODE_NOWAIT 가 세팅되면 이후 read/write 경로에서
// IOCB_NOWAIT 플래그를 존중해, 블로킹이 필요한 상황에서 즉시 -EAGAIN
// 을 반환하도록 fops 쪽 로직이 분기한다.
	if (mode & BLK_OPEN_RESTRICT_WRITES)
// [한국어] 이 open 이 "다른 공유 쓰기 open 을 막는" restrict-writes
// 모드였는지 확인한다 — bdev_permission() 단계에서 holder 필수 조건까지
// 이미 검증된 값이다.
		bdev_file->f_mode |= FMODE_WRITE_RESTRICTED;
// [한국어] 파일에 FMODE_WRITE_RESTRICTED 를 표시해, 이 파일을 닫을 때
// (bdev_yield_write_access) 일반 bd_writers-- 가 아니라
// bdev_unblock_writes() 로 반대 방향 카운팅을 되돌리게 한다.
	bdev_file->f_mapping = bdev->bd_mapping;
// [한국어] struct file 의 address_space 를 bdev 전용 페이지 캐시
// (bd_mapping)에 연결한다 — 이후 buffered read/write, mmap, writeback
// 이 모두 이 address_space 를 거쳐 NVMe I/O 로 이어진다.
	bdev_file->f_wb_err = filemap_sample_wb_err(bdev_file->f_mapping);
// [한국어] 현재 시점의 writeback 에러 시퀀스를 샘플링해 둔다. 이후
// fsync()/close() 시 filemap_check_wb_err() 로 "이 open 이후 새로 발생한"
// writeback(예: NVMe Flush/Write 실패) 에러만 감지하기 위한 기준점 —
// open 이전에 이미 나 있던 에러를 중복 보고하지 않기 위함이다.
	bdev_file->private_data = holder;
// [한국어] holder 를 private_data 에 저장한다 — release() 시점에 같은
// holder 값으로 claim 을 해제할 수 있게 하고, holder 가 NULL 이면(일반
// open) bdev_unclaimed() 판별에도 쓰인다.

	return 0;
// [한국어] 모든 단계가 성공적으로 끝났음을 알리는 성공 반환. 여기서
// 함수가 끝나므로 아래 put_module/abort_claiming 레이블은 이 정상 경로
// 에서는 실행되지 않는다(return 으로 fall-through 를 명시적으로 막음).
put_module:
// [한국어] d) 단계(bdev_may_open 실패, 또는 blkdev_get_part/whole 실패)
// 에서만 도달하는 레이블 — 이 지점 이전에 module 참조를 이미 얻었으므로
// 아래에서 그 참조를 되돌린다.
	module_put(disk->fops->owner);
// [한국어] c) 단계의 try_module_get() 을 정확히 상쇄한다 — 드라이버
// 모듈이 다시 언로드 가능한 상태로 돌아간다.
abort_claiming:
// [한국어] c) 단계 실패(disk_live/try_module_get) 시 직접 도달하거나,
// put_module 레이블의 코드를 실행한 뒤 자연스럽게 이어져 도달하는 공통
// 정리 레이블이다. 이 지점부터는 module 참조 유무와 무관하게 항상
// 수행해야 하는 뒷정리(claim 취소, mutex 해제, 이벤트 재개)만 남는다.
	if (holder)
// [한국어] 배타적 open 을 시도했었다면 a) 단계에서 bd_prepare_to_claim
// 으로 claim 을 예약해 뒀으므로, 실패했으니 그 예약을 취소해야 한다.
		bd_abort_claiming(bdev, holder);
// [한국어] whole->bd_claiming 을 지우고 대기 중인 다른 claim 시도자를
// 깨워, 이 실패한 open 때문에 다른 프로세스가 영원히 대기하지 않게 한다.
	mutex_unlock(&disk->open_mutex);
// [한국어] c) 단계에서 잡은 open_mutex 를 해제한다 — 정상 경로의
// mutex_unlock(위쪽)과 정확히 대칭되는 실패 경로용 해제.
	disk_unblock_events(disk);
// [한국어] b) 단계에서 disk_block_events() 로 걸어 둔 차단을 반드시
// 해제한다 — 실패 경로에서는 write_holder 유지 로직 자체를 타지 않았
// 으므로(claim 이 실패했으니 unblock_events 를 조작할 기회가 없었다)
// 이 경로는 무조건 이벤트를 재개한다.
	return ret;
// [한국어] 실패 지점에서 설정된 errno(-EIO/-EBUSY/-ENXIO 또는 하위
// 함수가 반환한 값)를 그대로 호출자에게 전달한다.
}

/*
 * [한국어]
 * blk_to_file_flags - blk_mode_t(블록 레이어 전용 open 모드 비트마스크)를
 *                     VFS/POSIX 파일 open 플래그(O_RDONLY/O_WRONLY/O_RDWR/
 *                     O_NDELAY 등)로 변환한다.
 *
 * @mode: BLK_OPEN_READ/BLK_OPEN_WRITE/BLK_OPEN_WRITE_IOCTL/BLK_OPEN_NDELAY
 *        비트 조합. bdev_open() 계열이 호출되기 전, bdev_file_open_by_dev()
 *        에서 struct file 을 할당할 때 필요한 f_flags 값을 만들기 위해
 *        전달된다.
 * @return: O_RDONLY(값 0)/O_WRONLY/O_RDWR 중 하나에 O_NDELAY 가 조건부로
 *          OR 된 값. alloc_file_pseudo_noaccount() 의 flags 인자로 그대로
 *          쓰인다.
 *
 * 커널 내부적으로 block_device 는 BLK_OPEN_* 비트마스크로 열림 모드를
 * 표현하지만, struct file->f_flags 는 표준 POSIX O_* 플래그를 기대하는
 * 코드도 있으므로 이 둘을 상호 변환하는 다리 역할을 한다. mode 의
 * READ/WRITE 비트 조합을 진리표처럼 순서대로 검사해 매칭되는 O_* 조합을
 * 고르고, BLK_OPEN_NDELAY 이면 O_NDELAY 를 추가로 얹는다. BLK_OPEN_WRITE_IOCTL
 * 은 플로피 드라이버가 남긴 역사적 quirk(아래 원문 주석 참고)로, "쓰기로
 * 열었지만 실제 읽기/쓰기는 허용하지 않고 ioctl 만 허용"하는 특수 케이스를
 * O_RDWR | O_WRONLY 라는 다소 비정상적인 조합으로 표시해 이 quirk 를
 * f_flags 에도 반영한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 순수 계산 함수로 sleep 하지 않고 락도
 * 사용하지 않는다(재진입·동시 호출 모두 안전).
 * caller: bdev_file_open_by_dev()(본 파일).
 * callee: 없음(단순 비트 연산과 WARN_ON_ONCE 뿐).
 * 에러 경로: 정상적인 mode 라면 READ 또는 WRITE 비트 중 하나는 반드시
 * 서 있어야 한다(bdev_permission() 등에서 이미 보장). 어느 쪽도 없으면
 * WARN_ON_ONCE(true) 로 커널 버그를 로그에 남기고 flags 는 0인 채로 진행한다
 * (반환형이 에러코드가 아닌 플래그 값이므로 별도의 에러 반환 경로는 없다).
 *
 * 호출 체인:
 *   bdev_file_open_by_dev → [blk_to_file_flags] → (없음, 값만 반환)
 */

/*
 * If BLK_OPEN_WRITE_IOCTL is set then this is a historical quirk
 * associated with the floppy driver where it has allowed ioctls if the
 * file was opened for writing, but does not allow reads or writes.
 * Make sure that this quirk is reflected in @f_flags.
 *
 * It can also happen if a block device is opened as O_RDWR | O_WRONLY.
 */
static unsigned blk_to_file_flags(blk_mode_t mode)
{
	unsigned int flags = 0;
// [한국어] 아직 아무 O_* 비트도 세팅되지 않은 초기값 — O_RDONLY 는 0 이므로
// 이 초기값 자체가 이미 "읽기 전용"을 나타낼 수 있다는 점에 유의(아래
// else if 분기가 실제로 이 성질을 이용한다).

	if ((mode & (BLK_OPEN_READ | BLK_OPEN_WRITE)) ==
// [한국어] READ 와 WRITE 비트가 "둘 다" 서 있는지 검사한다 — 읽기/쓰기
// 겸용으로 연 경우를 가려낸다.
	    (BLK_OPEN_READ | BLK_OPEN_WRITE))
		flags |= O_RDWR;
// [한국어] 읽기+쓰기 겸용이면 표준 O_RDWR 플래그를 세팅한다.
	else if (mode & BLK_OPEN_WRITE_IOCTL)
// [한국어] BLK_OPEN_WRITE_IOCTL 은 위 함수 주석에서 설명한 플로피 드라이버
// 전용 historical quirk — "쓰기로 연 것처럼 ioctl 은 허용하되 실제
// read/write 는 막는다"는 특수 상태를 나타낸다.
		flags |= O_RDWR | O_WRONLY;
// [한국어] 표준적이지 않은 O_RDWR | O_WRONLY 조합을 그대로 세팅해, 이
// quirk 상태를 f_flags 에도 동일하게 반영한다(원문 주석: O_RDWR |
// O_WRONLY 로 열린 것처럼 보이는 상태가 실제로도 발생할 수 있다는 의미).
	else if (mode & BLK_OPEN_WRITE)
// [한국어] 쓰기 전용으로 연 일반적인 경우(WRITE_IOCTL 이 아닌 경우).
		flags |= O_WRONLY;
// [한국어] 표준 O_WRONLY 를 세팅한다.
	else if (mode & BLK_OPEN_READ)
// [한국어] 읽기 전용으로 연 경우 — 위 세 조건(RDWR, WRITE_IOCTL, WRITE)
// 에 모두 해당하지 않고 READ 비트만 서 있을 때 이 분기로 들어온다.
		flags |= O_RDONLY; /* homeopathic, because O_RDONLY is 0 */
// [한국어] O_RDONLY 는 리눅스에서 0 으로 정의되므로 flags |= O_RDONLY 는
// 실제로 flags 값을 하나도 바꾸지 않는다. 원문 주석의 "homeopathic"
// (동종요법적, 즉 실질적 효력 없이 형식만 갖춘 처방)이라는 표현은 이
// 대입이 "약효 없는 처방"처럼 실질적 효과가 없음을 유머러스하게 짚은
// 것이다 — 그럼에도 코드 의도(O_RDONLY 케이스임)를 명시적으로 남기기
// 위한 문서화 목적의 대입으로 남겨 둔 것이다.
	else
// [한국어] READ 도 WRITE 도 아닌 경우 — bdev_permission()/호출자 계약상
// 정상적으로는 도달할 수 없어야 하는 분기이다.
		WARN_ON_ONCE(true);
// [한국어] READ/WRITE 둘 다 없는 mode 가 여기까지 온 것은 호출자 버그
// 이므로 커널 로그에 경고를 1회 남긴다. flags 는 0(O_RDONLY 와 동일한
// 비트 패턴)인 채로 그대로 진행한다 — 이 함수는 별도의 에러 반환 수단이
// 없다.

	if (mode & BLK_OPEN_NDELAY)
// [한국어] BLK_OPEN_NDELAY(넌블로킹 open 요청)가 서 있는지 검사한다 —
// O_NONBLOCK 과 동일한 개념을 블록 레이어 전용 비트로 표현한 것이다.
		flags |= O_NDELAY;
// [한국어] 위에서 결정된 RDONLY/WRONLY/RDWR 값에 O_NDELAY(O_NONBLOCK 과
// 같은 값의 별칭)를 추가로 OR 한다 — 두 값은 배타적이지 않고 함께
// 세팅될 수 있다.

	return flags;
// [한국어] 최종 조합된 O_* 플래그 값을 반환한다 — 호출자
// (bdev_file_open_by_dev)가 이 값을 alloc_file_pseudo_noaccount() 의
// flags 인자로 그대로 사용한다.
}

/*
 * [한국어]
 * bdev_file_open_by_dev - dev_t(주/부번호)로 이미 등록된 block_device 를
 *                        찾아 그것을 감싸는 struct file 을 새로 만들고 연다.
 *
 * @dev: 열려는 block_device 의 dev_t(예: NVMe namespace 라면 major 259,
 *       minor 는 namespace 순번에 해당). 이미 커널에 등록되어 있는
 *       디바이스여야 한다(등록 자체는 add_disk() 등이 별도로 수행한다).
 * @mode: BLK_OPEN_* 비트 조합 — bdev_open() 에 그대로 전달되고,
 *        blk_to_file_flags() 를 거쳐 struct file 의 f_flags 로도 변환된다.
 * @holder: exclusive claim 을 요청하는 주체 식별자. NULL 이면 일반 open.
 * @hops: holder 전용 콜백 테이블(holder 가 NULL 이면 미사용).
 * @return: 성공 시 완전히 초기화된 struct file 포인터(호출자가 이후 이
 *          파일로 직접 read/write/ioctl 을 수행하거나 fd_install() 로
 *          유저 공간에 fd 를 노출할 수 있다). 실패 시 ERR_PTR(-errno) —
 *          호출자는 IS_ERR() 로 반드시 검사해야 한다.
 *
 * 이 함수는 "이미 존재하는 dev_t 로부터 커널 내부용 struct file 핸들을
 * 새로 만드는" 진입점이다. 실제 경로 조회 없이 major/minor 만으로 bdev 를
 * 찾기 때문에, 파일시스템 마운트 코드(superblock 이 자신의 백엔드
 * block_device 를 열 때)나 md/dm 같은 조합 디바이스가 하위 디바이스를 열
 * 때 주로 사용된다.
 * 동작 순서: bdev_permission() 으로 cgroup devcgroup 권한과
 * RESTRICT_WRITES 제약을 먼저 검사한다 -> blkdev_get_no_open() 으로 dev_t
 * 에 해당하는 bdev 를 찾아 kobject 참조를 얻는다 -> blk_to_file_flags() 로
 * O_* 플래그를 계산한다 -> alloc_file_pseudo_noaccount() 로 struct file
 * 자체를 할당한다(이 bdev 의 VFS inode 를 그대로 재사용) -> ihold() 로 그
 * inode 에 대한 추가 참조를 확보한다 -> bdev_open() 으로 실제 open/claim
 * 을 수행한다 -> 실패 시 private_data 에 에러를 표시하고 fput() 으로
 * 정리한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, sleep 가능(bdev_open() 내부의
 * mutex_lock 등). 별도의 자체 락은 없고, 하위 함수들이 각자의 락
 * (bdev_lock, disk->open_mutex)을 사용한다.
 * caller: 파일시스템/디바이스 매퍼 등 커널 내부에서 dev_t 만 알고 있는
 * 코드, 그리고 bdev_file_open_by_path()(본 파일, 경로명을 dev_t 로 바꾼
 * 뒤 이 함수를 호출).
 * callee: bdev_permission, blkdev_get_no_open, blk_to_file_flags,
 * alloc_file_pseudo_noaccount, blkdev_put_no_open, ihold, bdev_open, fput.
 * 에러 발생 시: 각 단계 실패마다 그 단계 이전에 얻은 자원만 정리하고
 * ERR_PTR 을 반환한다(아래 인라인 주석에서 각 실패 지점별로 상세 설명).
 *
 * 호출 체인:
 *   bdev_file_open_by_path → [bdev_file_open_by_dev] → bdev_open
 */

struct file *bdev_file_open_by_dev(dev_t dev, blk_mode_t mode, void *holder,
				   const struct blk_holder_ops *hops)
{
	struct file *bdev_file;
// [한국어] 최종적으로 반환할 struct file 포인터. alloc_file_pseudo_noaccount()
// 가 성공하기 전까지는 유효한 값이 아니다.
	struct block_device *bdev;
// [한국어] dev_t 로 찾아낸 block_device. blkdev_get_no_open() 이 채운다.
	unsigned int flags;
// [한국어] blk_to_file_flags() 가 계산한 O_* 플래그 — struct file 할당 시
// f_flags 초기값으로 쓰인다.
	int ret;
// [한국어] bdev_permission()/bdev_open() 의 반환값을 담는 임시 변수.

	ret = bdev_permission(dev, mode, holder);
// [한국어] devcgroup(DEVCG_DEV_BLOCK) 정책으로 이 dev_t 에 대한 READ/WRITE
// 접근이 허용되는지 검사하고, BLK_OPEN_RESTRICT_WRITES 인데 holder 가
// 없는 모순, holder 가 에러 포인터인 이상 상태 등을 걸러낸다. 아직 실제
// bdev 를 찾지도 않은 가장 이른 단계의 권한 검사이다.
	if (ret)
// [한국어] 권한 검사 실패(-EPERM/-EINVAL 등) — 아직 아무 자원도 얻지
// 않았으므로 정리할 것이 없다.
		return ERR_PTR(ret);
// [한국어] 에러 코드를 포인터 타입에 인코딩해 반환한다(ERR_PTR/IS_ERR/
// PTR_ERR 관례) — 호출자는 IS_ERR() 로 이 값을 정상 포인터와 구분해야
// 한다.

	bdev = blkdev_get_no_open(dev, true);
// [한국어] dev_t 로 blockdev_superblock 안의 inode 를 ilookup() 해 이미
// 등록된 bdev 를 찾고, kobject_get_unless_zero() 로 디바이스 참조를
// 하나 올린다. 두 번째 인자 true 는 "찾지 못하면 레거시 모듈 autoload
// (blk_request_module())를 시도하라"는 뜻 — 구식 드라이버가 모듈
// 형태로만 존재할 때 major 번호로 자동 로드를 유도한다.
	if (!bdev)
// [한국어] 등록되지 않은 dev_t 이거나, 디바이스가 막 제거되는 중이라
// kobject 참조를 얻지 못한 경우이다.
		return ERR_PTR(-ENXIO);
// [한국어] "그런 디바이스가 없다"는 의미의 -ENXIO 를 반환한다 — 여기까지는
// bdev_permission() 외에 잡은 자원이 없으므로 추가 정리가 필요 없다.

	flags = blk_to_file_flags(mode);
// [한국어] BLK_OPEN_* 비트마스크를 struct file 이 이해하는 O_* 플래그로
// 변환한다 — 아래 alloc_file_pseudo_noaccount() 호출에 바로 사용된다.
	bdev_file = alloc_file_pseudo_noaccount(BD_INODE(bdev),
// [한국어] 실제 경로 조회 없이 struct file 을 직접 만든다. 첫 인자
// BD_INODE(bdev) 는 이 bdev 를 감싸는 bdev_inode 의 VFS inode 를 그대로
// 재사용한다 — 새 inode 를 만들지 않고 기존 것을 공유한다.
			blockdev_mnt, "", flags | O_LARGEFILE, &def_blk_fops);
// [한국어] blockdev_mnt 는 블록 디바이스 전용 pseudo 마운트(모듈 초기화
// 시 kern_mount() 로 한 번만 생성해 둔 내부 마운트이며, 실제 파일시스템이
// 아니다). 빈 이름("")은 실제 경로가 없기 때문이다. flags 에
// O_LARGEFILE 을 추가로 OR 해 64비트 오프셋을 항상 허용하고,
// &def_blk_fops 를 연결해 이 struct file 의 모든 연산(read_iter/
// write_iter/ioctl 등)이 블록 계층 공용 연산 테이블을 거치도록 한다.
// "_noaccount" 접미사는 이 할당이 시스템 전역 nr_files/RLIMIT_NOFILE
// 카운팅에 포함되지 않음을 의미한다 — 이 struct file 은 유저 fd 테이블에
// 바로 등록되는 것이 아니라 커널 내부 서브시스템이 직접 들고 쓰는
// 경우가 많기 때문이다.
	if (IS_ERR(bdev_file)) {
// [한국어] 파일 구조체 할당 자체가 실패(메모리 부족 등)한 경우이다.
		blkdev_put_no_open(bdev);
// [한국어] 위 blkdev_get_no_open() 에서 얻은 디바이스 kobject 참조를
// put_device() 로 되돌린다 — 파일 할당 실패로 더 이상 이 bdev 를 쓸
// 일이 없기 때문이다.
		return bdev_file;
// [한국어] bdev_file 자체가 이미 ERR_PTR 값이므로 그대로 반환한다
// (추가 ERR_PTR 래핑이 불필요하다).
	}
	ihold(BD_INODE(bdev));
// [한국어] alloc_file_pseudo_noaccount() 가 dentry 를 이 inode 에
// 연결(d_instantiate 계열)하긴 하지만, bdev 의 VFS inode 는 새로 만든
// 것이 아니라 gendisk 등록 시점부터 존재해 온 "공유" inode 이므로, 이
// struct file 자신의 수명 동안 별도로 참조를 하나 더 쥐고 있어야 한다.
// 그래야 이후 release 경로(fput -> ... -> iput)에서 이 파일이 자기 몫의
// 참조를 정확히 하나 반납하더라도, bdev 본연의 inode 참조 카운트 체계와
// 충돌 없이 안전하게 맞아떨어진다.

	ret = bdev_open(bdev, mode, holder, hops, bdev_file);
// [한국어] 지금까지는 "그릇"(struct file, dev_t 로 찾은 bdev)만 준비한
// 상태이고, 실제 open 카운터 증가/claim/이벤트 차단 처리/f_mode 세팅은
// 이 한 호출이 전부 수행한다(위 bdev_open() 함수 주석 참고).
	if (ret) {
// [한국어] bdev_open() 이 실패(-ENXIO/-EBUSY/-EIO 등)한 경우이다.
		/* We failed to open the block device. Let ->release() know. */
// [한국어] 위 원문 주석: block_device 열기에 실패했음을 ->release()
// 콜백이 알 수 있게 표시해 둔다는 뜻이다 — 아래에서 private_data 에
// 에러 포인터를 심어 두면, fput() 이 유발하는 def_blk_fops.release
// (blkdev_release)가 private_data 를 보고 "이 파일은 정상적으로 열린
// 적이 없다"는 것을 판별할 수 있다.
		bdev_file->private_data = ERR_PTR(ret);
// [한국어] private_data 를 에러 포인터로 덮어써 release 콜백에게 실패를
// 알린다 — 정상 open 이었다면 여기엔 holder(또는 NULL)가 들어갔을
// 자리이다.
		fput(bdev_file);
// [한국어] 방금 할당한 struct file 을 정리한다(참조 카운트 감소, 0이
// 되면 ->release 호출 후 최종 해제) — bdev_open() 실패로 이 파일은 더
// 이상 유효하지 않으므로 여기서 반드시 반납해야 메모리/참조 누수가
// 없다.
		return ERR_PTR(ret);
// [한국어] bdev_open() 이 돌려준 에러 코드를 그대로 호출자에게 전달한다.
	}
	return bdev_file;
// [한국어] 모든 단계가 성공 — 완전히 초기화된 struct file 을 호출자에게
// 반환한다. 이후 이 파일을 통한 모든 I/O 는 def_blk_fops 를 거쳐 이
// bdev(및 그 뒤의 NVMe 등 드라이버)로 흘러간다.
}
EXPORT_SYMBOL(bdev_file_open_by_dev);
// [한국어] GPL 여부와 무관하게 모든 커널 모듈이 링크해 쓸 수 있도록
// 심볼을 공개한다(EXPORT_SYMBOL_GPL 이 아님) — md, dm, 파일시스템, NVMe
// 관련 서브시스템 등 트리 안팎의 다양한 소비자가 이 함수를 통해 bdev 를
// struct file 형태로 얻어 간다.

/*
 * [한국어]
 * bdev_file_open_by_path - 경로명(예: "/dev/nvme0n1", "/dev/nvme0n1p1")으로
 *                          block_device 를 찾아 그것을 감싸는 struct file
 *                          을 만들고 연다.
 *
 * @path: 블록 디바이스 노드를 가리키는 파일시스템 경로 문자열(널 종료).
 *        devtmpfs 등에 이미 존재하는 노드여야 하며, 심볼릭 링크를 따라가
 *        최종적으로 S_ISBLK 인 inode 를 가리켜야 한다.
 * @mode: BLK_OPEN_* 비트 조합. bdev_file_open_by_dev() 로 그대로 전달된다.
 * @holder: exclusive claim 을 요청하는 주체 식별자. NULL 이면 일반 open.
 * @hops: holder 전용 콜백 테이블.
 * @return: 성공 시 struct file 포인터. 실패 시 ERR_PTR(-errno) — 경로
 *          조회 실패(-ENOENT 등), 블록 노드 아님(-ENOTBLK), 권한 없음
 *          (-EACCES), bdev_file_open_by_dev() 의 각종 실패, 또는 쓰기
 *          모드인데 read-only 미디어라 되돌리는 -EACCES 중 하나이다.
 *
 * bdev_file_open_by_dev() 가 "dev_t 를 이미 알고 있는" 커널 내부 소비자를
 * 위한 것이라면, 이 함수는 "경로 문자열만 아는" 소비자(예: 파일시스템
 * 마운트 시 사용자가 지정한 블록 디바이스 경로 등)를 위한 진입점이다.
 * lookup_bdev() 로 경로를 dev_t 로 변환한 뒤 나머지는 그대로
 * bdev_file_open_by_dev() 에 위임하고, 추가로 "쓰기 모드로 열렸는데 실제
 * 미디어가 read-only"인 경우를 한 번 더 걸러낸다(bdev_open() 내부의
 * bdev_may_open() 은 bd_writers 카운터 충돌만 보고 미디어 자체의 물리적
 * read-only 속성은 별도로 검사하지 않기 때문에 이 함수 레벨에서 추가
 * 검사가 필요하다).
 * 실행 컨텍스트: 프로세스 컨텍스트, sleep 가능(kern_path() 경로 탐색,
 * bdev_open() 내부 mutex 등). 자체 락 없음.
 * caller: 파일시스템 마운트 경로(예: get_tree_bdev 계열), 또는 사용자
 * 공간 도구가 커널 API 를 경유해 경로 기반으로 블록 디바이스를 여는
 * 경우.
 * callee: lookup_bdev, bdev_file_open_by_dev, bdev_read_only, file_bdev,
 * fput.
 * 에러 발생 시: lookup_bdev 실패 시 그 자리에서 즉시 반환한다(잡은
 * 자원 없음). bdev_file_open_by_dev 실패 시 그 함수가 이미 모든 정리를
 * 마친 ERR_PTR 을 그대로 전달한다. read-only 검사 실패 시에는 이미
 * 성공적으로 열린 파일을 fput() 으로 되돌리고 -EACCES 로 대체한다.
 *
 * 호출 체인:
 *   (파일시스템 마운트 등 경로 기반 호출자) → [bdev_file_open_by_path] → bdev_file_open_by_dev
 */

struct file *bdev_file_open_by_path(const char *path, blk_mode_t mode,
				    void *holder,
				    const struct blk_holder_ops *hops)
{
	struct file *file;
// [한국어] 반환할 struct file. bdev_file_open_by_dev() 가 성공적으로
// 채워 주기 전까지는 유효하지 않다.
	dev_t dev;
// [한국어] lookup_bdev() 가 경로로부터 알아낼 major/minor 식별자.
	int error;
// [한국어] lookup_bdev() 의 반환값을 담는 임시 변수.

	error = lookup_bdev(path, &dev);
// [한국어] 경로를 kern_path() 로 조회해 dentry/inode 를 얻고, S_ISBLK 로
// 실제 블록 디바이스 노드인지 확인한 뒤 may_open_dev() 로 접근 권한을
// 검사하고, 최종적으로 inode->i_rdev 를 dev 에 담아 반환한다.
	if (error)
// [한국어] 경로가 없거나(-ENOENT), 블록 디바이스 노드가 아니거나
// (-ENOTBLK), 접근 권한이 없는(-EACCES) 경우이다.
		return ERR_PTR(error);
// [한국어] 이 시점에서는 아직 아무 자원도 얻지 않았으므로 즉시 에러
// 반환만으로 충분하다.

	file = bdev_file_open_by_dev(dev, mode, holder, hops);
// [한국어] 이제 dev_t 를 확보했으니 나머지 open 절차(권한 재검사, bdev
// 조회, struct file 할당, 실제 claim/open)는 그대로
// bdev_file_open_by_dev() 에 위임한다.
	if (!IS_ERR(file) && (mode & BLK_OPEN_WRITE)) {
// [한국어] open 자체가 성공했고, 이번 open 이 쓰기 모드를 포함하는
// 경우에만 추가로 미디어의 물리적 read-only 속성을 검사한다(open 이
// 실패했으면 file 이 이미 ERR_PTR 이므로 아래 블록에 들어갈 필요가
// 없다).
		if (bdev_read_only(file_bdev(file))) {
// [한국어] file_bdev() 로 방금 연 struct file 에서 다시 block_device 를
// 꺼내고, bdev_read_only() 로 BD_READ_ONLY 플래그나 디스크 자체의
// GD_READ_ONLY 상태(예: 물리적으로 쓰기 보호된 미디어, 관리자가 강제로
// read-only 처리한 디스크)를 검사한다.
			fput(file);
// [한국어] 이미 성공적으로 열려 있던 파일을 되돌린다(참조 감소 -> 0이면
// release 호출 -> claim/open 카운터 등 원상 복구) — 쓰기 요청과
// read-only 미디어가 충돌하므로 이 open 자체를 무효화해야 한다.
			file = ERR_PTR(-EACCES);
// [한국어] 반환할 file 값을 -EACCES 로 교체한다 — 호출자에게는 "이미
// 열렸다가 나중에 취소됐다"는 사실이 아니라 "애초에 권한이 없어 열 수
// 없었다"는 형태로 보이게 된다.
		}
	}

	return file;
// [한국어] 성공 시 완전히 초기화된 struct file, 실패 시(lookup_bdev
// 실패/bdev_file_open_by_dev 실패/read-only 충돌) ERR_PTR 값을 그대로
// 반환한다.
}
EXPORT_SYMBOL(bdev_file_open_by_path);
// [한국어] 파일시스템 마운트 코드 등 트리 전역의 다양한(비 GPL 포함)
// 커널 모듈이 경로 문자열만으로 블록 디바이스를 열 수 있도록 심볼을
// 공개한다.

/*
 * [한국어]
 * bd_yield_claim - 이 struct file 이 들고 있던 exclusive claim(독점 사용권)을
 * 반납한다.
 *
 * @bdev_file: exclusive claim 을 반납할 대상의 struct file (block device 를
 *             open() 한 file 객체). private_data 필드에 holder 포인터가
 *             들어 있는 상태여야 이 함수를 호출하는 의미가 있다.
 *
 * blkdev_get_by_dev()/blkdev_get_by_path() 등으로 O_EXCL 방식 open 을 하면
 * bd_holder/bd_holders 에 claim 정보가 기록된다. 이 claim 은 file 이 닫히거나
 * (bdev_release) claim 을 다른 곳으로 미리 넘겨야 할 때(bdev_fput) 반드시
 * 반납되어야 한다. 그러지 않으면 bd_holders 카운트가 남아 다른 프로세스
 * (예: LVM, device-mapper multipath, 파티션 재스캔 도구)가 같은 NVMe
 * namespace/파티션을 절대 열 수 없게 되어 디바이스가 사실상 영구히
 * 잠기는 문제가 생긴다. 그래서 반납 로직을 별도 함수로 분리해
 * bdev_release/bdev_fput 양쪽에서 재사용한다.
 * 동작 순서: (1) file 에서 bdev 를 역참조하고 private_data 에 저장된 holder
 * 포인터를 꺼낸다. (2) 호출자가 open_mutex 를 이미 획득했는지 lockdep 으로
 * 검증한다 - 이 함수 자체는 락을 걸지 않고, 반드시 잠긴 상태에서 불려야
 * 한다는 전제 조건만 확인한다. (3) holder 가 IS_ERR_OR_NULL 이면(원래 open
 * 이 실패했거나 claim 없이 연 경우인데 잘못 호출된 상황) 커널 버그이므로
 * 경고만 남기고 즉시 리턴한다. (4) bdev_unclaimed() 로 실제 claim 이
 * 존재하는지(다른 opener 가 이미 넘겨받지 않았는지) 확인한 뒤, claim 이
 * 있을 때만 bd_end_claim() 으로 실제 반납을 수행한다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 &bdev->bd_disk->open_mutex 를
 * 이미 잡은 상태에서 호출되어야 하며(lockdep_assert_held 로 강제), 이 함수
 * 내부에서는 추가로 락을 걸거나 풀지 않는다.
 * 호출자: bdev_release(), bdev_fput() - 둘 다 open_mutex 를 먼저 잡은 뒤
 * 이 함수를 부른다.
 * 피호출자: bdev_unclaimed(), bd_end_claim().
 * 에러 처리: holder 상태가 비정상(IS_ERR_OR_NULL)이면 WARN_ON_ONCE 로 커널
 * 로그에 스택트레이스를 한 번만 남기고 그대로 리턴한다 - 별도의 복구
 * 로직 없이 "이 경로에 오면 안 된다"는 개발자용 경고만 수행한다.
 *
 * 호출 체인:
 *   bdev_release()/bdev_fput() → [bd_yield_claim] → bd_end_claim()
 */

static inline void bd_yield_claim(struct file *bdev_file)
{
	struct block_device *bdev = file_bdev(bdev_file);
	/* [한국어] struct file 이 가리키는 struct block_device 를 얻는다.
	 * file_bdev() 는 file->f_mapping->host 를 BDEV_I() 로 캐스팅해 얻은
	 * bdev_inode 컨테이너에서 bdev 필드를 꺼내는 inline 변환 헬퍼로,
	 * 별도의 조회(lookup)나 참조 카운트 증가는 일어나지 않는다. */
	void *holder = bdev_file->private_data;
	/* [한국어] 이 file 을 열 때 blkdev_get_by_*(..., holder) 로 전달된
	 * holder 포인터를 꺼낸다. exclusive claim 이 있는 open 이면 실제
	 * holder 객체 포인터가, 일반 open 이면 BDEV_I(...) 자기 참조 값이,
	 * open 자체가 실패했던 경로라면 ERR_PTR() 인코딩 값이 들어 있을 수
	 * 있다. */

	lockdep_assert_held(&bdev->bd_disk->open_mutex);
	/* [한국어] 이 함수는 반드시 open_mutex 를 쥔 채로 호출되어야 한다 -
	 * 그렇지 않으면 bd_holder/bd_holders 등 claim 관련 상태에 대한 동시
	 * 수정 경쟁 조건이 생길 수 있다. CONFIG_PROVE_LOCKING(lockdep) 이
	 * 켜진 디버그 커널에서 실제 락 보유 여부를 검사해, 위반 시 즉시
	 * 경고를 띄워 락 누락 버그를 조기에 잡아낸다. */

	if (WARN_ON_ONCE(IS_ERR_OR_NULL(holder)))
		/* [한국어] holder 가 NULL 이거나(애초에 claim 없이 호출될 리
		 * 없는 이 함수가 잘못 불린 경우) ERR_PTR 로 인코딩된 에러
		 * 값이면(open 자체가 실패했던 file 인데 잘못 호출된 경우)
		 * 호출자 쪽 로직 결함이다. WARN_ON_ONCE 로 한 번만 경고를
		 * 남겨(반복 호출 시 로그 폭주 방지) 문제를 드러내고, 잘못된
		 * 포인터로 bd_end_claim() 을 호출하는 사고를 막기 위해 그냥
		 * 리턴한다. */
		return;

	if (!bdev_unclaimed(bdev_file))
		/* [한국어] bdev_unclaimed() 는 이 file 이 실제로 exclusive
		 * claim 을 보유 중인지(다른 opener 가 먼저 claim 을 가져가
		 * 이 file 은 claim 없는 상태로 남지 않았는지) 확인한다. 부정
		 * 조건으로 걸러내어, 진짜 claim 을 들고 있을 때만 아래 반납
		 * 처리를 수행하게 한다. */
		bd_end_claim(bdev, holder);
		/* [한국어] 실제 exclusive claim 반납을 수행한다 - bd_holder
		 * 를 지우고 bd_holders 카운트를 감소시켜, 다른 프로세스가
		 * 이 블록 장치(예: NVMe namespace 또는 그 파티션)를 exclusive
		 * 로 열 수 있는 길을 터준다. */
}

/*
 * [한국어]
 * bdev_release - struct file 이 마지막 참조를 잃을 때(파일 디스크립터가 모두
 * 닫혀 fput 경로를 타는 시점) 호출되는 block device 전용 release 콜백.
 * def_blk_fops.release 로 등록되어 있다.
 *
 * @bdev_file: 닫히는 중인 block device 의 struct file. f_mapping->host 로
 *             struct block_device 를 찾을 수 있고, private_data 에는 open
 *             시 넘겨졌던 holder(정상 값/ERR_PTR/BDEV_I 자기참조 값 중 하나)
 *             가 들어 있다.
 *
 * blkdev_open() 으로 연 file 의 마지막 참조가 사라질 때 반드시 짝을 맞춰
 * 호출되어야 하는 정리 로직으로, dirty 캐시를 디스크(NVMe 라면 Write/Flush
 * 명령)로 내려보내고, exclusive claim 과 쓰기 권한을 반납하고, 파티션/전체
 * 디스크 참조 카운트를 감소시키는 일을 전부 담당한다. 이 정리가 누락되면
 * bd_openers 카운트가 남아 다음 open 이 실패하거나 dirty 데이터가 디스크에
 * 반영되지 않고 유실될 수 있다.
 * 동작 순서: (1) file 로부터 bdev/holder/disk 를 구한다. (2) holder 가
 * ERR_PTR 이면(open() 자체가 실패했지만 file 객체는 만들어졌던 특수 경로)
 * claim/이벤트 처리를 전부 건너뛰고 put_no_open 레이블로 바로 점프한다.
 * (3) bd_openers 가 1(내가 마지막 opener)이면 open_mutex 를 잡기 전에 미리
 * sync_blockdev() 로 dirty 페이지를 동기화한다 - 수 분씩 걸릴 수 있는 동기화
 * 작업을 mutex 를 쥔 채로 오래 수행하는 것을 피하기 위한 선제 최적화이며,
 * 이 시점과 실제 bd_openers 감소 시점 사이에 다른 프로세스가 끼어들어 새로
 * 열면 이 sync 가 불필요했던 셈이 되지만 정확성에는 영향이 없다는 트레이드
 * 오프를 감수한다. (4) open_mutex 를 잡고 bdev_yield_write_access() 로 쓰기
 * 권한을, holder 가 있으면 bd_yield_claim() 으로 exclusive claim 을
 * 반납한다. (5) disk_flush_events(MEDIA_CHANGE) 로 eject(1) 같은 사용자
 * 명령에 의한 미디어 제거를 드라이버가 감지하도록 트리거한다. (6) 파티션
 * 이면 blkdev_put_part(), 전체 디스크면 blkdev_put_whole() 로 각각의 참조
 * 카운트를 정리한다. (7) module_put() 으로 드라이버 모듈 참조를 감소시켜
 * 모듈이 필요 시 언로드될 수 있게 한다. (8) 마지막으로 open_mutex 밖에서
 * blkdev_put_no_open() 으로 bdev 객체 자체의 참조를 해제한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(fput 은 태스크가 파일을 닫거나 종료할
 * 때, 혹은 워크큐 상의 지연된 fput 에서 실행될 수 있다). 함수 내부에서
 * disk->open_mutex 를 직접 획득/해제한다.
 * 호출자: fput() → __fput() 이 file_operations.release 콜백으로 이 함수를
 * 호출한다.
 * 피호출자: sync_blockdev(), bdev_yield_write_access(), bd_yield_claim(),
 * disk_flush_events(), blkdev_put_part()/blkdev_put_whole(), module_put(),
 * blkdev_put_no_open().
 * 에러 처리: 반환형이 void 이므로 실패를 알릴 수 없다 - open 이 애초에
 * 실패했던 경우(holder 가 ERR_PTR)만 별도 경로(goto put_no_open)로 최소한의
 * 정리(bdev 참조 해제)만 수행하고 나머지는 건너뛴다.
 *
 * 호출 체인:
 *   fput() → __fput() → [bdev_release] → sync_blockdev()/bd_yield_claim()/
 *   blkdev_put_part()/blkdev_put_whole()/blkdev_put_no_open()
 */

void bdev_release(struct file *bdev_file)
{
	struct block_device *bdev = file_bdev(bdev_file);
	/* [한국어] file 로부터 대상 struct block_device 를 얻는다(단순 캐스팅
	 * 변환, 참조 카운트 변화 없음). */
	void *holder = bdev_file->private_data;
	/* [한국어] open 시 저장된 holder 값을 꺼낸다 - 정상 holder 포인터,
	 * BDEV_I(...) 자기참조(일반 open 표식), 또는 ERR_PTR(open 실패 표식)
	 * 중 하나일 수 있다. */
	struct gendisk *disk = bdev->bd_disk;
	/* [한국어] 이 bdev 가 속한 gendisk(일반 디스크, 예: NVMe namespace)를
	 * 얻는다. open_mutex 와 fops 등이 gendisk 단위로 관리되므로 이후
	 * 코드에서 반복 사용한다. */

	/* We failed to open that block device. */
	/* [한국어] 원문 설명: 이 블록 디바이스를 여는 데 실패한 경우다.
	 * private_data 가 ERR_PTR 로 인코딩되어 있다면 blkdev_open() 경로
	 * 자체가 실패했지만 struct file 객체는 이미 만들어져 fput 경로를
	 * 타게 된 특수한 상황을 의미한다 - claim/write access 등 반납할
	 * 것이 애초에 없으므로 뒤의 모든 정리 절차를 건너뛰어야 한다. */
	if (IS_ERR(holder))
		/* [한국어] holder 가 에러 포인터면(open 실패) claim 정리,
		 * sync, 이벤트 트리거 등을 전부 건너뛰고 bdev 참조 해제만
		 * 수행하는 put_no_open 레이블로 점프한다. */
		goto put_no_open;

	/*
	 * Sync early if it looks like we're the last one.  If someone else
	 * opens the block device between now and the decrement of bd_openers
	 * then we did a sync that we didn't need to, but that's not the end
	 * of the world and we want to avoid long (could be several minute)
	 * syncs while holding the mutex.
	 */
	/* [한국어] 원문 설명(번역): 우리가 마지막 opener 로 보이면 미리
	 * 동기화한다. 지금부터 bd_openers 를 실제로 감소시키는 시점 사이에
	 * 다른 누군가 이 블록 장치를 다시 열면 방금 한 동기화가 불필요했던
	 * 셈이 되지만, 그건 큰 문제가 아니며 우리는 open_mutex 를 쥔 채로
	 * (수 분이 걸릴 수도 있는) 긴 동기화를 하는 상황을 피하고 싶다는
	 * 것이 핵심이다 - 즉 "약간의 낭비 가능성"과 "락 보유 시간 최소화"를
	 * 맞바꾸는 의도적 트레이드오프다. */
	if (atomic_read(&bdev->bd_openers) == 1)
		/* [한국어] bd_openers(현재 이 bdev 를 연 프로세스/파일 수)가
		 * 정확히 1이면 나(지금 release 되는 이 file)가 유일한
		 * opener 였다는 뜻이므로, 곧 0이 되어 디바이스가 완전히
		 * 닫히기 전에 dirty 데이터를 먼저 내려보낸다. atomic_read
		 * 는 그 순간의 스냅샷일 뿐이며, 아래에서 실제 opener 카운트
		 * 감소는 open_mutex 를 잡은 뒤 blkdev_put_part/whole 안에서
		 * 이루어지므로 이 사이에 경쟁이 있어도(위 주석 설명대로)
		 * 안전하다. */
		sync_blockdev(bdev);
		/* [한국어] 이 블록 디바이스의 page cache 에 있는 dirty
		 * 페이지들을 디스크로 강제 기록(writeback)한다. NVMe
		 * 장치라면 결과적으로 NVMe Write 명령들과 뒤이은 Flush
		 * 명령이 SQ(Submission Queue)에 제출되어, 캐시에만 있던
		 * 데이터가 실제 미디어에 반영되도록 보장한다. */

	mutex_lock(&disk->open_mutex);
	/* [한국어] 이 gendisk 의 open/claim 상태(bd_holder, bd_holders,
	 * bd_openers 등)를 보호하는 뮤텍스를 획득한다. 이 뮤텍스 없이
	 * claim 을 반납하거나 opener 카운트를 바꾸면 동시에 open/close
	 * 하는 다른 스레드와 경쟁이 생겨 카운트가 어긋날 수 있다. */
	bdev_yield_write_access(bdev_file);
	/* [한국어] 이 file 이 open 시 획득했던 쓰기 접근 권한(예: 배타적
	 * 쓰기 제한 관련 카운트)을 반납한다. 이후 다른 프로세스가 쓰기
	 * 접근을 다시 요청할 수 있게 된다. */

	if (holder)
		/* [한국어] holder 가 NULL 이 아니면(즉 exclusive claim 을
		 * 가진 open 이었다면) bd_yield_claim() 으로 claim 반납
		 * 절차를 진행한다. holder 가 NULL 이면 애초에 claim 없는
		 * 일반 open 이었으므로 반납할 것이 없다. */
		bd_yield_claim(bdev_file);

	/*
	 * Trigger event checking and tell drivers to flush MEDIA_CHANGE
	 * event.  This is to ensure detection of media removal commanded
	 * from userland - e.g. eject(1).
	 */
	/* [한국어] 원문 설명(번역): 이벤트 검사를 트리거하고 드라이버에게
	 * MEDIA_CHANGE 이벤트를 플러시하라고 알린다. 이는 사용자 공간에서
	 * 내려진 미디어 제거 명령(예: eject(1))의 감지를 보장하기 위함이다
	 * - 즉 파일을 닫는 시점에 강제로 한 번 더 미디어 상태를 확인해,
	 * 사용자가 방금 꺼낸 미디어에 대한 이벤트가 누락되지 않게 한다. */
	disk_flush_events(disk, DISK_EVENT_MEDIA_CHANGE);
	/* [한국어] 드라이버의 media-change 감지 로직을 동기적으로 한 번
	 * 실행시켜, 미디어 제거/교체 이벤트가 있었다면 즉시 처리(uevent
	 * 발행 등)하도록 한다. NVMe 자체는 보통 고정 namespace 라 실질
	 * 영향이 적지만, USB-NVMe 인클로저나 이동식 스토리지류에서는 이
	 * 경로가 실제로 의미를 가진다. */

	if (bdev_is_partition(bdev))
		/* [한국어] 이 bdev 가 파티션(예: nvme0n1p1)이면 파티션 전용
		 * 정리 함수로 분기한다 - 전체 디스크 open_mutex 정리와는
		 * 다른 파티션별 참조 카운트를 다룬다. */
		blkdev_put_part(bdev);
	else
		/* [한국어] 파티션이 아니라 전체 디스크(whole disk, 예:
		 * nvme0n1 자체)를 닫는 경우 아래 blkdev_put_whole() 분기로
		 * 진입한다. */
		blkdev_put_whole(bdev);
		/* [한국어] 전체 디스크 단위의 open 카운트(disk->open_partitions
		 * 등)를 감소시키고 필요하면 드라이버의 .release 콜백을
		 * 호출해 NVMe namespace 자체를 최종적으로 놓아준다. */
	mutex_unlock(&disk->open_mutex);
	/* [한국어] claim/opener 상태 보호 뮤텍스를 해제한다 - 이 시점
	 * 이후로는 다른 스레드가 이 gendisk 를 자유롭게 열거나 닫을 수
	 * 있다. */

	module_put(disk->fops->owner);
	/* [한국어] 이 file_operations 를 제공한 드라이버 모듈(예: nvme_core)
	 * 의 참조 카운트를 감소시킨다. open 시 try_module_get() 으로 잡았던
	 * 참조와 짝을 맞추는 호출로, 이게 없으면 드라이버 모듈이 rmmod 로
	 * 제거되지 못하고 영원히 남게 된다. */
put_no_open:
	/* [한국어] open 자체가 실패했던 경우(holder 가 ERR_PTR) 여기로 바로
	 * 점프한다 - claim/이벤트/모듈 참조 등은 애초에 획득한 적이 없으므로
	 * 손댈 필요가 없고, bdev 자체의 참조 카운트만 정리하면 된다. */
	blkdev_put_no_open(bdev);
	/* [한국어] blkdev_get_no_open() 등으로 늘어난 bdev 구조체 자체의
	 * 참조 카운트(open 성공 여부와 무관하게 잡혀 있던 기본 참조)를
	 * 감소시킨다. 이 참조가 0이 되면 bdev inode 가 해제될 수 있다. */
}

/*
 * [한국어]
 * bdev_fput - claim 과 쓰기 접근 권한을 먼저 반납한 뒤 실제 fput() 은 지연
 * 실행되도록 넘기는 헬퍼.
 * NVMe 연결점: LVM/device-mapper multipath 등이 NVMe bdev 를 다른 목적으로
 * 재-claim 하기 전에, 지금 들고 있던 claim 을 안전하게 먼저 내려놓아야
 * 할 때 사용한다.
 */

/**
 * bdev_fput - yield claim to the block device and put the file
 * @bdev_file: open block device
 *
 * Yield claim on the block device and put the file. Ensure that the
 * block device can be reclaimed before the file is closed which is a
 * deferred operation.
 */
/*
 * [한국어]
 * bdev_fput - block device 에 대한 claim 을 반납한 뒤 struct file 을 놓는다.
 *
 * @bdev_file: EXPORT_SYMBOL 로 공개된 인터페이스를 통해 넘어오는, 열려 있는
 *             block device 의 struct file. def_blk_fops 로 열린 파일이어야
 *             한다.
 *
 * 일반적인 fput() 은 참조 카운트가 0이 될 때 즉시(또는 태스크 종료 시
 * task_work/워크큐로) __fput() → bdev_release() 를 호출해 claim 반납까지
 * 한 번에 처리한다. 그런데 어떤 호출자(예: 파일시스템 마운트 코드, 혹은
 * LVM/multipath 같은 상위 계층)는 file 을 "당장" 닫지는 않지만(참조 카운트가
 * 남아 fput 이 지연 실행될 수 있음) block device 를 즉시 다른 목적으로
 * 재사용하고 싶어할 수 있다. 이때 claim/write access 를 fput 이 실제로
 * 실행될 때까지 붙들고 있으면 재-claim 이 막히므로, 이 함수는 그 claim/
 * write access 부분만 미리 떼어내 즉시 반납하고, 이후 진짜 __fput()이
 * 실행되어 bdev_release() 가 불려도 중복으로 반납하지 않도록 표식을 바꿔
 * 둔다.
 * 동작 순서: (1) 이 file 이 정말 def_blk_fops 로 열린 블록 디바이스 파일인지
 * 확인한다(아니면 잘못된 호출이므로 경고하고 리턴). (2) private_data 가
 * 설정되어 있으면(즉 이 file 이 실제 open 성공 상태라면) open_mutex 를 잡고
 * bdev_yield_write_access() 와 bd_yield_claim() 으로 쓰기 권한과 exclusive
 * claim 을 지금 즉시 반납한다. (3) private_data 를 BDEV_I(파일이 가리키는
 * inode) 자기참조 값으로 바꿔치기해, "claim 이 없는 일반 open" 상태로
 * 위장시킨다 - 이렇게 하면 나중에 실제 __fput() → bdev_release() 가 실행될
 * 때 holder 가 truthy 값(BDEV_I(...))이더라도 bd_yield_claim() 내부의
 * bdev_unclaimed() 검사에서 이미 claim 이 없다고 판단되어 중복 반납을
 * 시도하지 않는다. (4) 마지막으로 fput() 을 호출해 실제 파일 정리(즉시
 * 또는 지연)를 정상 경로에 맡긴다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 함수 내부에서 disk->open_mutex 를
 * 직접 획득/해제한다(bdev_release 와 동일한 락을 사용하므로 서로 경쟁
 * 하지 않도록 순서를 맞춘다).
 * 호출자: 파일을 소유한 상위 서브시스템이 block device 를 즉시 재-claim
 * 가능하게 만들고 싶을 때 fput() 대신 이 함수를 호출한다(EXPORT_SYMBOL).
 * 피호출자: bdev_yield_write_access(), bd_yield_claim(), fput().
 * 에러 처리: f_op 가 def_blk_fops 가 아니면(호출자가 블록 디바이스가 아닌
 * file 에 실수로 이 함수를 호출한 버그 상황) WARN_ON_ONCE 로 경고만 남기고
 * 아무 것도 하지 않은 채 리턴한다 - fput() 조차 호출하지 않으므로 호출자가
 * 직접 정상적인 fput() 을 다시 호출해야 한다는 점에 유의.
 *
 * 호출 체인:
 *   (LVM/multipath 등 상위 계층) → [bdev_fput] → bd_yield_claim()/fput()
 *   → (지연 시) __fput() → bdev_release()
 */
void bdev_fput(struct file *bdev_file)
{
	if (WARN_ON_ONCE(bdev_file->f_op != &def_blk_fops))
		/* [한국어] 이 file 의 file_operations 가 block device 전용
		 * def_blk_fops 가 아니면 잘못된 호출이다(블록 디바이스가
		 * 아닌 파일에 대해 클레임 반납을 시도한 버그). 한 번만
		 * 경고를 남기고 아무 정리도 하지 않은 채 즉시 리턴한다 -
		 * 이 경우 fput() 조차 호출되지 않으므로 호출자가 별도로
		 * 처리해야 한다. */
		return;

	if (bdev_file->private_data) {
		/* [한국어] private_data 가 설정되어 있다는 것은 이 file 이
		 * 정상적으로 open 되어 holder 정보를 갖고 있다는 뜻이다
		 * (ERR_PTR/NULL 이 아닌 상태). 이 경우에만 claim/write
		 * access 를 지금 즉시 반납하는 절차를 수행한다. */
		struct block_device *bdev = file_bdev(bdev_file);
		/* [한국어] file 로부터 대상 struct block_device 를 얻는다
		 * (캐스팅 변환, 참조 카운트 변화 없음). */
		struct gendisk *disk = bdev->bd_disk;
		/* [한국어] open_mutex 등을 관리하는 gendisk 를 얻는다 -
		 * bdev_release() 와 동일한 락 객체를 사용해야 하므로 반드시
		 * bdev->bd_disk 경로로 접근한다. */

		mutex_lock(&disk->open_mutex);
		/* [한국어] claim/opener 상태를 보호하는 뮤텍스를 획득한다.
		 * bdev_release() 가 동시에 같은 bdev 를 정리하려는 경쟁을
		 * 막기 위해 동일한 락을 사용한다. */
		bdev_yield_write_access(bdev_file);
		/* [한국어] 이 file 이 갖고 있던 쓰기 접근 권한을 즉시
		 * 반납한다 - bdev_release() 에서 하던 것과 동일한 절차를
		 * 여기서 미리 수행하는 것. */
		bd_yield_claim(bdev_file);
		/* [한국어] exclusive claim 을 즉시 반납한다(내부적으로
		 * bdev_unclaimed() 로 실제 claim 존재 여부를 확인한 뒤
		 * bd_end_claim() 수행). 이 시점부터 다른 프로세스가 이
		 * 블록 디바이스를 exclusive 로 재claim 할 수 있게 된다. */
		/*
		 * Tell release we already gave up our hold on the
		 * device and if write restrictions are available that
		 * we already gave up write access to the device.
		 */
		/* [한국어] 원문 설명(번역): 이 디바이스에 대한 우리의
		 * hold(점유)를 이미 포기했고, 쓰기 제한 기능이 있다면 쓰기
		 * 접근 권한도 이미 포기했다는 사실을 나중의 release 에게
		 * 알린다 - 즉 아래 대입은 "이미 다 반납했다"는 표식을
		 * private_data 에 남겨, 나중에 진짜 __fput()이 실행되어
		 * bdev_release() 가 호출되어도 그 함수가 holder 를 다시
		 * 반납하려 들지 않도록 만드는 것이 목적이다. */
		bdev_file->private_data = BDEV_I(bdev_file->f_mapping->host);
		/* [한국어] private_data 를 "claim 없는 일반 open" 을 뜻하는
		 * BDEV_I(inode) 자기참조 값으로 바꿔친다. bdev_release() 는
		 * holder 가 이 자기참조 값이면 bd_yield_claim() 을 부르더라도
		 * 내부의 bdev_unclaimed() 검사에서 "claim 없음" 으로 판정해
		 * bd_end_claim() 을 다시 호출하지 않는다 - 이렇게 이중 반납
		 * (double free 유사 문제)을 방지하는 트릭이다. */
		mutex_unlock(&disk->open_mutex);
		/* [한국어] claim/opener 상태 보호 뮤텍스를 해제한다. */
	}

	fput(bdev_file);
	/* [한국어] 실제 파일 참조 카운트를 감소시킨다. 참조가 0이 되면 정상
	 * __fput() 경로로 이어져 bdev_release() 가 호출되지만, 위에서 이미
	 * claim/write access 를 반납하고 private_data 도 바꿔 두었으므로
	 * bdev_release() 는 claim 관련 처리를 건너뛰고 나머지 정리(파티션/
	 * 전체 디스크 참조 해제 등)만 수행하게 된다. 참조가 남아 있다면
	 * fput() 은 태스크 종료 시점 등으로 실제 해제를 지연시킬 수 있다. */
}
EXPORT_SYMBOL(bdev_fput);
/* [한국어] 이 심볼을 모든 커널 모듈에서 사용할 수 있도록 공개한다 - GPL
 * 전용이 아닌 일반 EXPORT_SYMBOL 이므로 비 GPL 모듈에서도 호출 가능하다. */

/*
 * [한국어]
 * lookup_bdev - 파일시스템 경로명으로부터 block_device 의 dev_t(디바이스
 * 번호: major/minor 조합)를 조회한다.
 * NVMe 연결점: 예를 들어 "/dev/nvme0n1" 같은 경로가 실제로 블록 디바이스
 * 노드인지 S_ISBLK() 로 확인하고, 그 노드가 가리키는 major/minor 번호를
 * 반환한다.
 */

/**
 * lookup_bdev() - Look up a struct block_device by name.
 * @pathname: Name of the block device in the filesystem.
 * @dev: Pointer to the block device's dev_t, if found.
 *
 * Lookup the block device's dev_t at @pathname in the current
 * namespace if possible and return it in @dev.
 *
 * Context: May sleep.
 * Return: 0 if succeeded, negative errno otherwise.
 */
/*
 * [한국어]
 * lookup_bdev - 경로명 문자열로부터 block_device 의 dev_t 를 조회한다.
 *
 * @pathname: 조회할 블록 디바이스 노드의 경로 문자열(예: "/dev/nvme0n1").
 *            현재 마운트 네임스페이스 기준으로 해석된다.
 * @dev: 조회에 성공하면 결과 dev_t(디바이스 번호)를 채워 넣을 출력 파라미터.
 * @return: 성공 시 0, 실패 시 음수 errno(-EINVAL, -ENOTBLK, -EACCES 등
 *          경로 조회 관련 에러 코드도 그대로 전파될 수 있다). 호출자는
 *          0이 아니면 *dev 값을 신뢰해서는 안 된다.
 *
 * 파일시스템 경로만 아는 상태(예: 사용자가 문자열로 넘긴 디바이스 경로)에서
 * 실제 block_device 의 dev_t 를 얻어야 하는 다양한 상위 코드(마운트 처리,
 * 블록 디바이스 open 경로 등)를 위해 존재한다. 경로 → dentry → inode 순으로
 * 내려가 그 inode 가 정말 블록 디바이스 노드(S_ISBLK)인지 검증하고, 접근
 * 권한까지 확인한 뒤 i_rdev 를 꺼내 반환한다.
 * 동작 순서: (1) 경로 문자열이 비어있지 않은지 확인한다. (2) kern_path() 로
 * 경로를 심볼릭 링크까지 따라가며(LOOKUP_FOLLOW) dentry 로 변환한다. (3)
 * d_backing_inode() 로 실제 backing inode 를 얻는다. (4) S_ISBLK() 로 블록
 * 디바이스 노드가 맞는지 검증하고 아니면 -ENOTBLK 로 실패 처리한다. (5)
 * may_open_dev() 로 이 디바이스 노드를 열 권한(예: namespace 격리, devtmpfs
 * 정책)이 있는지 검사하고 아니면 -EACCES 로 실패 처리한다. (6) inode->i_rdev
 * 를 *dev 에 복사한다. (7) path_put() 으로 조회 중 잡았던 dentry/vfsmount
 * 참조를 해제한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, "May sleep"(경로 탐색 중 디스크 I/O 나
 * 락 대기로 슬립 가능) - 인터럽트 컨텍스트에서 호출 금지.
 * 호출자: 파일시스템 마운트 코드, loop 디바이스 설정 등 경로 문자열로부터
 * 블록 디바이스를 식별해야 하는 커널 서브시스템(EXPORT_SYMBOL 로 모듈에도
 * 공개).
 * 피호출자: kern_path(), d_backing_inode(), may_open_dev(), path_put().
 * 에러 처리: 각 단계 실패 시 out_path_put 레이블로 점프해 path_put() 으로
 * 참조를 정리한 뒤 에러 코드를 반환한다 - 이렇게 단일 정리 지점으로 모아
 * 참조 누수를 방지한다.
 *
 * 호출 체인:
 *   (마운트/디바이스 설정 코드) → [lookup_bdev] → kern_path()/may_open_dev()
 */
int lookup_bdev(const char *pathname, dev_t *dev)
{
	struct inode *inode;
	/* [한국어] 경로 조회 후 얻게 될 backing inode 를 담을 지역 변수 -
	 * 아직 초기화되지 않은 상태. */
	struct path path;
	/* [한국어] kern_path() 가 채워줄 struct path(dentry + vfsmount 쌍) -
	 * 사용 후 반드시 path_put() 으로 참조를 해제해야 한다. */
	int error;
	/* [한국어] 각 단계의 성공/실패 코드를 누적해 나가는 지역 변수. */

	if (!pathname || !*pathname)
		/* [한국어] 경로 포인터 자체가 NULL 이거나 빈 문자열("")이면
		 * 조회할 대상이 없는 잘못된 인자이므로 즉시 -EINVAL 로
		 * 실패 처리한다(경로 탐색을 시도할 필요조차 없음). */
		return -EINVAL;

	error = kern_path(pathname, LOOKUP_FOLLOW, &path);
	/* [한국어] 커널 내부에서 사용하는 경로 탐색 API - 사용자 공간의
	 * path_lookup 과 유사하게 pathname 문자열을 dentry/vfsmount 쌍으로
	 * 변환한다. LOOKUP_FOLLOW 플래그는 경로의 마지막 컴포넌트가
	 * 심볼릭 링크여도 그 링크를 따라가 최종 대상을 얻으라는 의미다
	 * (예: /dev/disk/by-id/... 심볼릭 링크가 실제 /dev/nvme0n1 을
	 * 가리키는 경우). */
	if (error)
		/* [한국어] 경로 자체를 찾지 못했거나(ENOENT) 권한이 없는 등
		 * (EACCES) 탐색 단계에서 실패하면, 아직 path 를 획득하지
		 * 못했으므로 path_put() 없이 바로 에러를 반환한다. */
		return error;

	inode = d_backing_inode(path.dentry);
	/* [한국어] dentry 가 가리키는 실제 backing inode 를 얻는다(오버레이
	 * 파일시스템 등에서 dentry 자체의 inode 와 실제 backing inode 가
	 * 다를 수 있어 이 헬퍼를 사용). 이 inode 가 진짜 검사 대상이다. */
	error = -ENOTBLK;
	/* [한국어] 아래 S_ISBLK 검사가 실패했을 때 반환할 기본 에러 코드를
	 * 미리 설정해 둔다 - "블록 디바이스가 아니다"를 의미. */
	if (!S_ISBLK(inode->i_mode))
		/* [한국어] i_mode 의 파일 타입 비트를 검사해 이 inode 가
		 * 블록 디바이스 특수 파일(예: mknod 로 만든 /dev/nvme0n1)
		 * 인지 확인한다. 일반 파일/디렉터리/문자 디바이스 등이면
		 * 대상이 아니므로 실패 처리한다. */
		goto out_path_put;
	error = -EACCES;
	/* [한국어] 아래 권한 검사가 실패했을 때 반환할 에러 코드를 미리
	 * 설정한다 - "접근 권한 없음"을 의미. */
	if (!may_open_dev(&path))
		/* [한국어] 현재 마운트 네임스페이스/보안 정책(예: nodev 마운트
		 * 옵션, 컨테이너의 디바이스 접근 제한)상 이 디바이스 노드를
		 * 열 수 있는지 검사한다. 컨테이너 안에서 호스트의 NVMe
		 * 디바이스 노드에 접근하는 것을 막는 등의 격리에 쓰인다. */
		goto out_path_put;

	*dev = inode->i_rdev;
	/* [한국어] 검증을 통과했으므로 이 블록 디바이스 노드의 실제
	 * major/minor 번호(dev_t)를 출력 파라미터에 복사한다. */
	error = 0;
	/* [한국어] 여기까지 도달했다면 모든 검사를 통과한 것이므로 성공
	 * 코드로 확정한다. */
out_path_put:
	/* [한국어] 성공/실패와 무관하게 공통으로 거치는 정리 지점 - 경로
	 * 탐색 중 획득한 dentry/vfsmount 참조를 여기서 반드시 해제한다. */
	path_put(&path);
	/* [한국어] kern_path() 가 잡았던 dentry 와 vfsmount 참조 카운트를
	 * 감소시킨다 - 호출하지 않으면 참조 누수로 해당 dentry/마운트가
	 * 영원히 해제되지 않는다. */
	return error;
	/* [한국어] 누적된 최종 결과(0 또는 음수 errno)를 호출자에게
	 * 반환한다. */
}
EXPORT_SYMBOL(lookup_bdev);
/* [한국어] 모든 커널 모듈에서 호출 가능하도록 심볼을 공개한다(비 GPL
 * 심볼). */

/*
 * [한국어]
 * bdev_mark_dead - block_device 와 그 위에 마운트된 파일시스템을 "죽은"
 * 상태로 표시한다.
 *
 * @surprise: true 면 미디어가 이미 예고 없이 제거된 상태(surprise removal),
 * false 면 정상적인(orderly) 제거를 준비 중인 상태.
 *
 * NVMe 연결점: NVMe SSD 가 예고 없이 제거되었을 때(예: 핫플러그 카드가
 * 물리적으로 뽑히거나 컨트롤러가 리셋되어 namespace 가 사라질 때) 호출된다.
 * sync_blockdev() 로 남은 dirty 데이터를 최대한 기록 시도하고 invalidate_bdev
 * 로 page cache 를 무효화하여, 더 이상의 접근이 이미 사라진 NVMe 하드웨어의
 * stale(오래된/무효한) 데이터를 반환하지 않도록 한다.
 */

/**
 * bdev_mark_dead - mark a block device as dead
 * @bdev: block device to operate on
 * @surprise: indicate a surprise removal
 *
 * Tell the file system that this devices or media is dead.  If @surprise is set
 * to %true the device or media is already gone, if not we are preparing for an
 * orderly removal.
 *
 * This calls into the file system, which then typicall syncs out all dirty data
 * and writes back inodes and then invalidates any cached data in the inodes on
 * the file system.  In addition we also invalidate the block device mapping.
 */
/*
 * [한국어]
 * bdev_mark_dead - block_device 를 "죽은(dead)" 상태로 표시하고, 그 위의
 * 파일시스템/캐시를 정리한다.
 *
 * @bdev: 죽은 상태로 표시할 대상 struct block_device(예: 제거된 NVMe
 *        namespace 를 나타내는 bdev).
 * @surprise: true 면 미디어/디바이스가 이미 사라진 상태(예: 핫플러그
 *            서프라이즈 리무브), false 면 정상 절차로 제거를 준비 중인
 *            상태(예: 관리자가 명령으로 미리 unbind 하는 경우).
 *
 * 디바이스나 미디어가 죽었음을 그 위에 마운트된 파일시스템에게 알려주기
 * 위해 존재한다. 파일시스템은 이 콜백을 받으면 통상 남은 dirty 데이터를
 * 동기화하고 inode 를 writeback 한 뒤 캐시된 데이터를 무효화하며, 추가로
 * 이 함수 자체도 블록 디바이스 매핑을 무효화한다. NVMe 관점에서는 컨트롤러
 * 리셋/핫언플러그로 실제 하드웨어에 더 이상 I/O 를 보낼 수 없는 상황에서,
 * 상위 계층이 stale 데이터를 계속 읽거나 쓰다가 알 수 없는 에러를 겪는 것을
 * 막기 위한 마지막 정리 지점이다.
 * 동작 순서: (1) bd_holder_lock 을 잡아 holder_ops 필드에 대한 동시 접근을
 * 막는다. (2) bd_holder_ops 가 등록되어 있고 그 안에 mark_dead 콜백이
 * 있으면(예: 파일시스템이 자체 정지 로직을 등록해 둔 경우) 그 콜백을
 * 호출한다 - 이 콜백은 자신이 필요한 만큼 처리한 뒤 반드시 스스로
 * bd_holder_lock 을 풀어야 한다는 계약이 있다(함수가 lock 을 쥔 채로
 * 콜백에 진입하고, 콜백이 unlock 책임을 진다). (3) mark_dead 콜백이 없으면
 * 이 함수가 직접 bd_holder_lock 을 풀고 sync_blockdev() 로 남은 dirty
 * 데이터를 최대한 기록한다. (4) 마지막으로 invalidate_bdev() 로 block
 * device 의 page cache 를 무효화해 이후 어떤 경로로도 stale 데이터가
 * 노출되지 않게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(디바이스 제거 처리 경로, 예: PCIe 핫플러그
 * 이벤트 처리나 관리자 명령 처리에서 호출). bd_holder_lock 뮤텍스를 함수
 * 내부에서 획득/해제하되, mark_dead 콜백 경로에서는 unlock 책임이 콜백으로
 * 넘어간다는 점이 이례적이므로 주의해야 한다.
 * 호출자: 드라이버의 디바이스 제거 경로(예: NVMe 컨트롤러 제거/리셋 처리,
 * 또는 미디어 제거 처리)에서 이 GPL 전용 심볼을 호출한다.
 * 피호출자: bd_holder_ops->mark_dead()(파일시스템/상위 계층 콜백),
 * sync_blockdev(), invalidate_bdev().
 * 에러 처리: 반환형이 void 이며 실패를 알리지 않는다 - "최선을 다해"
 * dirty 데이터를 내려보내고 캐시를 무효화하는 정리 함수이므로, 이미 죽은
 * 디바이스에 대한 I/O 실패는 내부적으로 무시되거나 로깅될 뿐 이 함수의
 * 반환값에는 반영되지 않는다.
 *
 * 호출 체인:
 *   (드라이버 제거/리셋 경로) → [bdev_mark_dead] → mark_dead 콜백 또는
 *   sync_blockdev() → invalidate_bdev()
 */
void bdev_mark_dead(struct block_device *bdev, bool surprise)
{
	mutex_lock(&bdev->bd_holder_lock);
	/* [한국어] bd_holder_ops 포인터에 대한 동시 접근(다른 스레드가 동시에
	 * holder ops 를 등록/해제하는 경쟁)을 막기 위해 잠근다. */
	if (bdev->bd_holder_ops && bdev->bd_holder_ops->mark_dead)
		/* [한국어] 이 bdev 에 holder 가 등록한 연산 테이블이 있고,
		 * 그 안에 mark_dead 콜백이 구현되어 있으면(예: 파일시스템이
		 * 자신만의 정지 절차를 원하는 경우) 범용 sync_blockdev 대신
		 * 그 콜백에 위임한다. */
		bdev->bd_holder_ops->mark_dead(bdev, surprise);
		/* [한국어] holder(대개 마운트된 파일시스템)가 등록해 둔
		 * 콜백을 호출한다. 이 콜백은 반드시 bd_holder_lock 을
		 * 스스로 풀어야 하는 계약을 갖는다 - 즉 락을 쥔 채로 콜백에
		 * 진입시켜 콜백이 필요한 만큼(예: 저널 flush, superblock
		 * 동결)을 안전하게 수행하고 나서 unlock 하도록 설계되었다. */
	else {
		mutex_unlock(&bdev->bd_holder_lock);
		/* [한국어] 별도 mark_dead 콜백이 없으므로 이 함수가 직접
		 * 뮤텍스를 해제한다 - sync_blockdev() 는 슬립 가능한 I/O 를
		 * 유발하므로 짧게 유지해야 하는 bd_holder_lock 을 굳이 쥔
		 * 채로 부를 필요가 없다. */
		sync_blockdev(bdev);
		/* [한국어] 남아 있는 dirty 페이지를 최대한 디스크로 내려
		 *보낸다 - NVMe 라면 아직 살아있을 수도 있는 컨트롤러에
		 * Write/Flush 명령을 마지막으로 시도하는 것에 해당한다.
		 * 이미 하드웨어가 사라졌다면 이 I/O 는 실패로 끝나겠지만,
		 * 실패해도 이 함수는 계속 진행해 캐시 무효화로 넘어간다. */
	}

	invalidate_bdev(bdev);
	/* [한국어] block device 의 page cache 에 남아 있는 클린 페이지들을
	 * 모두 버려 무효화한다 - 디바이스가 죽었으므로 캐시된 내용을 더 이상
	 * 신뢰할 수 없고, 이후 재삽입되는 새 디바이스(같은 major/minor 를
	 * 재사용하는 경우)가 이전 디바이스의 stale 데이터를 보게 되는 것을
	 * 막는다. */
}
/*
 * New drivers should not use this directly.  There are some drivers however
 * that needs this for historical reasons. For example, the DASD driver has
 * historically had a shutdown to offline mode that doesn't actually remove the
 * gendisk that otherwise looks a lot like a safe device removal.
 */
/* [한국어] 원문 설명(번역): 새 드라이버는 이 함수를 직접 사용해서는 안
 * 된다. 다만 역사적인 이유로 이 함수가 필요한 일부 드라이버가 있다 - 예를
 * 들어 DASD 드라이버는 역사적으로 gendisk 를 실제로 제거하지는 않으면서도
 * 안전한 디바이스 제거처럼 보이는 "오프라인 모드로의 shutdown" 을 갖고
 * 있었다. 즉 이 심볼은 신규 코드가 아니라 이런 레거시 드라이버 호환을
 * 위해서만 남아 있는 예외적인 공개 API 라는 경고성 주석이다. */
EXPORT_SYMBOL_GPL(bdev_mark_dead);
/* [한국어] GPL 라이선스 모듈에서만 호출 가능하도록 심볼을 공개한다 - 위
 * 경고 주석대로 새 드라이버의 사용을 사실상 억제하려는 의도도 겸한다. */

/*
 * [한국어]
 * sync_bdevs - blockdev_superblock 에 등록된 모든 block_device 의 dirty
 * 데이터를 디스크로 플러시한다.
 *
 * @wait: true 면 writeback 완료까지 대기, false 면 writeback 을 시작만
 *        시키고 대기하지 않는다.
 *
 * NVMe 연결점: 시스템 종료(reboot/poweroff)나 sync(1) 유틸리티 호출 시,
 * 시스템에 존재하는 모든 NVMe namespace(및 다른 블록 디바이스)의 dirty
 * 페이지를 NVMe Write/Flush 명령으로 밀어낸다. 이 순회는 개별 디바이스를
 * 하나씩 추적하지 않고, 모든 bdev 가 공유하는 가상 superblock 인
 * blockdev_superblock 의 inode 리스트(s_inodes)를 순회하는 방식으로
 * 구현되어 있다 - 즉 시스템에 존재하는 모든 bdev inode 를 한 번에 훑는다.
 */

void sync_bdevs(bool wait)
{
	struct inode *inode, *old_inode = NULL;
	/* [한국어] 순회 중인 현재 inode 와, "아직 iput 하지 않고 들고 있는
	 * 이전 inode" 를 함께 선언한다. old_inode 를 NULL 로 초기화해 첫
	 * 반복에서 불필요한 iput(NULL) 이 안전하게 no-op 이 되도록 한다. */

	spin_lock(&blockdev_superblock->s_inode_list_lock);
	/* [한국어] blockdev_superblock->s_inodes 리스트(시스템의 모든 bdev
	 * inode 를 매달아 두는 전역 리스트)를 순회하는 동안, 다른 CPU 가
	 * 리스트에 inode 를 추가/제거하는 것을 막기 위해 스핀락을 건다.
	 * 리스트 조작은 짧게 끝나야 하므로 슬립 불가능한 스핀락을 사용한다. */
	list_for_each_entry(inode, &blockdev_superblock->s_inodes, i_sb_list) {
		/* [한국어] blockdev_superblock 에 매달린 모든 bdev inode 를
		 * i_sb_list 연결 고리를 따라 순회한다 - 이 superblock 은
		 * 실제 마운트된 파일시스템이 아니라, 커널이 모든 block
		 * device inode 를 관리하기 위해 내부적으로 쓰는 가상
		 * superblock 이다. 따라서 이 순회는 사실상 "시스템에 열려
		 * 있는 모든 블록 디바이스"를 순회하는 것과 같다. */
		struct address_space *mapping = inode->i_mapping;
		/* [한국어] 이 bdev inode 의 page cache 를 관리하는
		 * address_space 를 얻는다 - dirty 페이지 존재 여부와
		 * writeback 요청이 모두 이 구조체를 통해 이루어진다. */
		struct block_device *bdev;
		/* [한국어] 아래에서 I_BDEV() 로 채워질 지역 변수 - 아직
		 * 초기화되지 않은 상태로 선언만 해 둔다. */

		spin_lock(&inode->i_lock);
		/* [한국어] 이 inode 의 상태 플래그(i_state)와 mapping 페이지
		 * 수를 검사하는 동안 다른 경로(예: 동시에 진행 중인 inode
		 * 회수/해제)가 상태를 바꾸지 못하도록 inode 개별 락을 건다. */
		if (inode_state_read(inode) & (I_FREEING | I_WILL_FREE | I_NEW) ||
			/* [한국어] 이 inode 가 지금 해제 중(I_FREEING)이거나
			 * 곧 해제될 예정(I_WILL_FREE)이거나 아직 초기화가
			 * 끝나지 않은 새 inode(I_NEW)라면, writeback 대상으로
			 * 삼기에 안전하지 않은 과도기 상태이므로 건너뛰어야
			 * 한다(이 상태의 inode 를 건드리면 use-after-free 나
			 * 초기화되지 않은 필드 접근 위험이 있다). */
		    mapping->nrpages == 0) {
			/* [한국어] 위 상태 플래그 조건이 아니더라도, page
			 * cache 에 페이지가 하나도 없다면(nrpages == 0) 애초에
			 * writeback 으로 내려보낼 dirty 데이터가 있을 수
			 * 없으므로 굳이 아래의 무거운 처리(참조 증가, 락
			 * 재획득, open_mutex 등)를 할 필요가 없다. */
			spin_unlock(&inode->i_lock);
			/* [한국어] 건너뛰기로 결정했으므로 inode 개별 락을
			 * 즉시 풀어 다른 스레드가 이 inode 에 접근할 수 있게
			 * 한다. */
			continue;
			/* [한국어] 이 inode 는 처리 대상이 아니므로 나머지
			 * 로직(참조 획득, writeback 등)을 모두 건너뛰고
			 * list_for_each_entry 의 다음 inode 로 넘어간다. */
		}
		__iget(inode);
		/* [한국어] 이 inode 의 참조 카운트를 증가시켜, 아래에서
		 * s_inode_list_lock 을 놓더라도 이 inode 가 리스트에서
		 * 제거되거나 해제되지 않도록 붙잡아 둔다(__iget 은 이미
		 * i_lock 을 쥔 상태에서 참조를 증가시키는 저수준 버전). */
		spin_unlock(&inode->i_lock);
		/* [한국어] 참조를 안전하게 늘렸으므로 inode 개별 락은 더
		 * 이상 필요 없어 해제한다. */
		spin_unlock(&blockdev_superblock->s_inode_list_lock);
		/* [한국어] 이제부터는 sync_blockdev 류의 오래 걸릴 수 있는
		 * writeback 작업을 수행해야 하므로, 전체 리스트를 막는
		 * 스핀락은 미리 풀어 다른 CPU 가 리스트를 계속 순회/수정할
		 * 수 있게 한다(스핀락을 쥔 채로 오래 걸리는 I/O 를 하면 안
		 * 되기 때문). */
		/*
		 * We hold a reference to 'inode' so it couldn't have been
		 * removed from s_inodes list while we dropped the
		 * s_inode_list_lock  We cannot iput the inode now as we can
		 * be holding the last reference and we cannot iput it under
		 * s_inode_list_lock. So we keep the reference and iput it
		 * later.
		 */
		/* [한국어] 원문 설명(번역): 우리가 'inode' 에 대한 참조를
		 * 쥐고 있으므로, s_inode_list_lock 을 놓은 사이에도 이
		 * inode 가 s_inodes 리스트에서 제거되었을 리 없다. 그렇다고
		 * 지금 당장 이 inode 를 iput 할 수는 없는데, 우리가 이
		 * inode 의 마지막 참조를 쥐고 있을 수도 있고, 그렇다면
		 * s_inode_list_lock 을 쥔 채로(더 정확히는 그 리스트 락이
		 * 다시 걸리는 다음 반복 시작 시점 근처에서) iput 하면 안
		 * 되기 때문이다 - 마지막 참조 해제는 슬립 가능한 정리 루틴
		 * (예: inode 파괴, 관련 superblock 콜백)을 유발할 수 있는데
		 * 이는 스핀락 보유 컨텍스트와 양립할 수 없다. 그래서 참조를
		 * 계속 쥐고 있다가, 다음 반복에서 새 inode 를 처리하기 직전
		 * 시점(스핀락 밖)에서 이전 inode 를 iput 하는 방식으로
		 * "한 박자 늦게" 해제한다. */
		iput(old_inode);
		/* [한국어] 지난 반복에서 붙잡아 두었던 이전 inode 의 참조를
		 * 이제 안전하게(스핀락 밖에서) 해제한다. 첫 반복에서는
		 * old_inode 가 NULL 이므로 iput(NULL) 은 아무 일도 하지
		 * 않는 안전한 no-op 이다. */
		old_inode = inode;
		/* [한국어] 지금 막 __iget() 으로 참조를 늘린 현재 inode 를
		 * "다음 반복에서 늦게 iput 할 대상"으로 기록해 둔다. */
		bdev = I_BDEV(inode);
		/* [한국어] bdev 전용 inode(bdev_inode 컨테이너)로부터 실제
		 * struct block_device 포인터를 꺼낸다 - 이 순회 리스트의
		 * 모든 inode 는 blockdev_superblock 소속이므로 전부 이
		 * 변환이 유효하다. */

		mutex_lock(&bdev->bd_disk->open_mutex);
		/* [한국어] 이 bdev 의 opener 카운트(bd_openers)를 안정적으로
		 * 읽기 위해 open_mutex 를 잡는다 - 동시에 열리거나 닫히는
		 * 중일 수 있는 카운트를 검사 시점에 고정시킨다. */
		if (!atomic_read(&bdev->bd_openers)) {
			/* [한국어] 현재 이 블록 디바이스를 연 opener 가
			 * 하나도 없다면(bd_openers == 0), 애초에 이 디바이스
			 * 를 통해 쓰기가 이루어질 수 없었으므로 writeback
			 * 대상으로 취급할 이유가 없다 - 그냥 건너뛴다. */
			; /* skip */
			/* [한국어] 원문에 이미 존재하던 null 문 + 주석 -
			 * opener 가 없는 경우 아무 동작도 하지 않고 아래
			 * open_mutex 해제 단계로 곧장 넘어간다는 것을
			 * 명시적으로 표현한 빈 분기다. */
		} else if (wait) {
			/* [한국어] opener 가 있고(즉 writeback 대상일 수
			 * 있고) 호출자가 완료까지 대기하라고(wait == true)
			 * 요청한 경우 - 예를 들어 시스템 종료 전 마지막
			 * sync 처럼 반드시 디스크에 반영된 것을 확인해야
			 * 하는 상황에 이 분기를 탄다. */
			/*
			 * We keep the error status of individual mapping so
			 * that applications can catch the writeback error using
			 * fsync(2). See filemap_fdatawait_keep_errors() for
			 * details.
			 */
			/* [한국어] 원문 설명(번역): 개별 mapping 의 에러
			 * 상태를 유지해, 애플리케이션이 fsync(2) 를 통해
			 * writeback 에러를 확인할 수 있게 한다. 자세한 내용은
			 * filemap_fdatawait_keep_errors() 를 참고하라 - 즉
			 * 여기서 에러를 흡수해 버리지 않고 매핑에 남겨 두어야
			 * 나중에 사용자 공간이 fsync 실패를 올바르게 관찰할
			 * 수 있다는 뜻이다. */
			filemap_fdatawait_keep_errors(inode->i_mapping);
			/* [한국어] 이미 진행 중인(또는 방금 시작한) writeback
			 * 이 전부 끝날 때까지 대기한다 - NVMe 관점에서는 해당
			 * namespace 로 나간 Write 명령들의 완료 큐(CQ) 엔트리가
			 * 전부 들어올 때까지 기다리는 것에 대응한다. 일반
			 * filemap_fdatawait() 과 달리 에러가 나도 매핑의 에러
			 * 플래그를 지우지 않고 보존한다(keep_errors). */
		} else {
			/* [한국어] wait 가 false 면(예: 주기적 백그라운드
			 * sync 처럼 완료를 굳이 기다릴 필요가 없는 경우)
			 * writeback 을 시작만 시키고 곧바로 다음 inode 로
			 * 넘어간다. */
			filemap_fdatawrite(inode->i_mapping);
			/* [한국어] dirty 페이지들에 대한 writeback 을
			 * 비동기로 시작시킨다 - NVMe 관점에서는 Write 명령을
			 * SQ 에 제출하되 완료(CQ)를 기다리지 않고 반환하는
			 * 것에 대응한다. */
		}
		mutex_unlock(&bdev->bd_disk->open_mutex);
		/* [한국어] bd_openers 검사와 writeback 트리거가 끝났으므로
		 * open_mutex 를 해제한다. */

		spin_lock(&blockdev_superblock->s_inode_list_lock);
		/* [한국어] 다음 list_for_each_entry 반복을 계속하려면 리스트
		 * 순회를 보호하는 스핀락을 다시 잡아야 한다 - 매크로 내부의
		 * inode = list_next_entry(...) 같은 다음 포인터 계산이 이
		 * 락 아래에서 이루어지는 것을 전제로 한다. */
	}
	spin_unlock(&blockdev_superblock->s_inode_list_lock);
	/* [한국어] 순회가 끝났으므로(리스트 끝에 도달) 리스트 보호 스핀락을
	 * 최종적으로 해제한다. */
	iput(old_inode);
	/* [한국어] 마지막 반복에서 __iget() 으로 붙잡아 두었던 마지막
	 * inode 의 참조를 여기서 해제한다 - 루프 안에서는 항상 "한 박자
	 * 늦게" iput 했으므로, 루프가 끝난 뒤 남아 있는 마지막 참조를
	 * 이렇게 별도로 정리해 주어야 참조 누수가 없다. */
}

/*
 * [한국어]
 * bdev_statx - block_device 노드에 대한 statx(2) 확장 필드 중
 * STATX_DIOALIGN(direct I/O 정렬 요구사항)과 STATX_WRITE_ATOMIC(원자적
 * 쓰기 지원 정보)을 채워 준다.
 *
 * NVMe 연결점: dio_offset_align 은 bdev_logical_block_size() 즉 NVMe LBA
 * (Logical Block Address) 하나의 크기(예: 512바이트 또는 4096바이트)로
 * 설정된다. STATX_WRITE_ATOMIC 을 지원하는 경우 queue_atomic_write_unit_*
 * 값들이 채워지는데, 이는 NVMe 의 (제조사별, 선택적) atomic write 단위
 * 기능과 연결된 정보다.
 */

/*
 * Handle STATX_{DIOALIGN, WRITE_ATOMIC} for block devices.
 */
/* [한국어] 원문 설명(번역): 블록 디바이스에 대한 STATX_DIOALIGN 과
 * STATX_WRITE_ATOMIC 요청을 처리한다. */
/*
 * [한국어]
 * bdev_statx - 블록 디바이스 노드에 대한 statx(2) 확장 정렬/원자적 쓰기
 * 정보를 채운다.
 *
 * @path: statx(2) 시스템 호출 대상이 된 경로(블록 디바이스 노드 자체를
 *        가리키는 dentry 를 포함) - 파일시스템이 아니라 devtmpfs 등에 있는
 *        디바이스 노드의 경로다.
 * @stat: 결과를 채워 넣을 struct kstat - dio_mem_align, dio_offset_align,
 *        result_mask, blksize, 그리고 atomic write 관련 필드들이 이
 *        함수에서 설정된다.
 * @request_mask: 사용자가 실제로 요청한 statx 필드 마스크(STATX_* 비트
 *                조합) - 요청하지 않은 값은 계산하지 않아 불필요한 비용을
 *                피한다.
 *
 * 일반 파일에 대한 statx() 처리 경로는 파일시스템별 getattr 을 사용하지만,
 * 블록 디바이스 노드(예: /dev/nvme0n1)에 대해 direct I/O 정렬 요구사항이나
 * 원자적 쓰기 지원 여부를 물어보는 STATX_DIOALIGN/STATX_WRITE_ATOMIC 요청은
 * 그 디바이스 노드 뒤에 있는 실제 struct block_device 의 큐 제한 값을
 * 조회해야 답할 수 있다. 이 함수가 그 특수 경로를 담당한다.
 * 동작 순서: (1) dentry 의 backing inode(디바이스 노드 inode)에서 i_rdev
 * 를 얻어 blkdev_get_no_open() 으로 struct block_device 를 조회한다(open
 * 카운트를 늘리지 않는 조회 전용 참조). (2) 조회에 실패하면(디바이스가
 * 이미 제거된 경우 등) 아무 것도 채우지 않고 리턴한다. (3) STATX_DIOALIGN
 * 이 요청되었으면 DMA 정렬 요구사항과 논리 블록 크기 기반의 오프셋 정렬
 * 요구사항을 채운다. (4) STATX_WRITE_ATOMIC 이 요청되었고 이 디바이스가
 * 실제로 atomic write 를 지원하면 최소/최대 원자적 쓰기 단위를 채운다.
 * (5) blksize 를 최소 I/O 크기로 설정한다. (6) blkdev_get_no_open() 으로
 * 늘렸던 참조를 blkdev_put_no_open() 으로 반드시 되돌려준다.
 * 실행 컨텍스트: 프로세스 컨텍스트(statx(2) 시스템 호출 처리 경로). 별도
 * 락을 직접 걸지 않으며, blkdev_get_no_open/put_no_open 쌍으로 참조만
 * 관리한다.
 * 호출자: VFS 의 statx 처리 경로가 대상이 블록 디바이스 노드일 때 이
 * 함수로 위임한다.
 * 피호출자: blkdev_get_no_open(), bdev_dma_alignment(), bdev_logical_block_size(),
 * bdev_can_atomic_write(), queue_atomic_write_unit_min_bytes()/
 * queue_atomic_write_unit_max_bytes(), generic_fill_statx_atomic_writes(),
 * bdev_io_min(), blkdev_put_no_open().
 * 에러 처리: bdev 조회 자체가 실패하면(디바이스가 없어졌거나 유효하지 않은
 * 경우) 그냥 조용히 리턴하며, 이 경우 result_mask 에 STATX_DIOALIGN 등이
 * 세팅되지 않으므로 호출자는 해당 정보가 채워지지 않았음을 알 수 있다.
 *
 * 호출 체인:
 *   (VFS statx(2) 처리 경로) → [bdev_statx] → blkdev_get_no_open()/
 *   generic_fill_statx_atomic_writes()
 */
void bdev_statx(const struct path *path, struct kstat *stat, u32 request_mask)
{
	struct block_device *bdev;
	/* [한국어] 아래에서 blkdev_get_no_open() 이 채워줄 지역 변수 - 아직
	 * 초기화되지 않은 상태. */

	/*
	 * Note that d_backing_inode() returns the block device node inode, not
	 * the block device's internal inode.  Therefore it is *not* valid to
	 * use I_BDEV() here; the block device has to be looked up by i_rdev
	 * instead.
	 */
	/* [한국어] 원문 설명(번역, 그대로 옮김): d_backing_inode() 는 블록
	 * 디바이스 노드 inode 를 반환하는 것이지, 블록 디바이스 자신의
	 * 내부(internal) inode 를 반환하는 것이 아님에 유의하라. 따라서
	 * 여기서 I_BDEV() 를 쓰는 것은 유효하지 않다 - 블록 디바이스는
	 * 대신 i_rdev 값으로 조회해야 한다. 즉 devtmpfs 등에 있는
	 * "/dev/nvme0n1" 파일 자체의 inode(디바이스 노드 inode)와, bdev
	 * 내부적으로 캐시를 관리하기 위해 쓰는 bdev_inode(블록 디바이스의
	 * internal inode)는 서로 다른 객체이므로, 전자에서 후자를 얻으려면
	 * I_BDEV() 캐스팅이 아니라 i_rdev 로 blkdev_get_no_open() 을 통해
	 * 별도로 조회해야 한다는 경고다. */
	bdev = blkdev_get_no_open(d_backing_inode(path->dentry)->i_rdev, false);
	/* [한국어] 디바이스 노드 inode 의 i_rdev(major/minor 번호)로 실제
	 * struct block_device 를 조회한다. blkdev_get_no_open() 은 이름
	 * 그대로 "open" 하지 않는(bd_openers 를 늘리지 않는) 조회 전용
	 * 참조를 반환하며, 두 번째 인자 false 는 이 조회가 exclusive 접근을
	 * 요구하지 않음을 의미한다. */
	if (!bdev)
		/* [한국어] 해당 major/minor 에 대응하는 block_device 를 찾지
		 * 못했다면(예: 디바이스가 이미 제거된 경우) 채워 줄 정보가
		 * 없으므로 아무 필드도 건드리지 않고 그냥 리턴한다 - 이 경우
		 * stat->result_mask 에 DIOALIGN 관련 비트가 세팅되지 않은
		 * 채로 호출자에게 돌아간다. */
		return;

	if (request_mask & STATX_DIOALIGN) {
		/* [한국어] 사용자가 statx() 호출 시 STATX_DIOALIGN 비트를
		 * 요청 마스크에 포함시켰을 때만 direct I/O 정렬 정보를
		 * 계산한다 - 요청하지 않았다면 계산 비용을 아낀다. */
		stat->dio_mem_align = bdev_dma_alignment(bdev) + 1;
		/* [한국어] 이 블록 디바이스의 DMA 정렬 요구사항을 바이트
		 * 단위로 채운다. bdev_dma_alignment() 는 정렬 마스크(예:
		 * 0x3, 즉 4바이트 정렬이면 하위 비트가 011)를 반환하므로,
		 * 실제 "정렬 단위 값"으로 변환하려면 여기에 1을 더한다
		 * (마스크 0x3 → 정렬 단위 4). NVMe PRP(Physical Region
		 * Page)/SGL(Scatter Gather List) 구성 시 사용자 버퍼 주소가
		 * 만족해야 하는 정렬 조건에 대응한다. */
		stat->dio_offset_align = bdev_logical_block_size(bdev);
		/* [한국어] direct I/O 요청의 파일 오프셋/길이가 맞춰야 하는
		 * 정렬 단위를 이 NVMe namespace 의 논리 블록 크기(logical
		 * block size, 예: 512 또는 4096바이트)로 설정한다 - NVMe
		 * LBA 경계에 맞지 않는 direct I/O 는 커널이 거부하거나
		 * bounce buffer 를 통해야 하므로, 사용자 공간이 이 값을
		 * 미리 알고 정렬된 I/O 를 구성하게 하기 위함이다. */
		stat->result_mask |= STATX_DIOALIGN;
		/* [한국어] dio_mem_align/dio_offset_align 을 실제로 채웠음을
		 * 호출자에게 알리기 위해 result_mask 에 STATX_DIOALIGN
		 * 비트를 켠다 - 사용자 공간은 요청 마스크가 아니라 이
		 * result_mask 를 보고 어떤 필드가 실제로 유효한지 판단한다. */
	}

	if (request_mask & STATX_WRITE_ATOMIC && bdev_can_atomic_write(bdev)) {
		/* [한국어] 사용자가 STATX_WRITE_ATOMIC 정보를 요청했고,
		 * 동시에 이 블록 디바이스가 실제로 atomic write(하나의 쓰기
		 * 명령이 부분 쓰기 없이 전부 성공하거나 전부 실패함을
		 * 보장하는 기능 - 일부 NVMe 드라이브가 지원하는 선택적
		 * 기능)를 지원할 때만 이 블록으로 진입한다. 둘 중 하나라도
		 * 아니면 atomic write 관련 필드는 채우지 않는다. */
		struct request_queue *bd_queue = bdev->bd_queue;
		/* [한국어] 이 bdev 가 속한 request_queue(블록 계층 큐 - 최종
		 * 적으로 NVMe 드라이버의 nvme_queue/블록 계층 제한 값들이
		 * 저장되는 곳)를 얻어, 아래에서 atomic write 관련 제한 값을
		 * 조회하는 데 사용한다. */

		generic_fill_statx_atomic_writes(stat,
			/* [한국어] kstat 의 atomic write 관련 필드들(예:
			 * atomic_write_unit_min/max, atomic_write_segments_max
			 * 등)을 공통 로직으로 채워주는 범용 헬퍼를 호출한다.
			 * 이 큐/디바이스별 값들을 그대로 statx 응답 형식에
			 * 맞게 변환해 넣는 역할이다. */
			queue_atomic_write_unit_min_bytes(bd_queue),
			/* [한국어] 이 디바이스가 지원하는 최소 원자적 쓰기
			 * 단위(바이트)를 조회한다 - NVMe 컨트롤러/네임스페이스
			 * 가 보고하는 Namespace Atomic Write Unit 관련 정보에
			 * 기반한 제조사별 값이다. */
			queue_atomic_write_unit_max_bytes(bd_queue),
			/* [한국어] 이 디바이스가 지원하는 최대 원자적 쓰기
			 * 단위(바이트)를 조회한다 - 한 번의 쓰기 명령으로
			 * 원자성을 보장받을 수 있는 상한 크기다. */
			0);
			/* [한국어] 네 번째 인자 0 - 이 커널 버전의
			 * generic_fill_statx_atomic_writes() 시그니처에서
			 * 현재 사용하지 않거나 기본값(예: 추가 세그먼트 제한
			 * 없음)을 뜻하는 예약/미사용 인자로, 향후 필드 확장을
			 * 위한 자리로 0이 전달된다. */
	}

	stat->blksize = bdev_io_min(bdev);
	/* [한국어] statx 결과의 권장 I/O 블록 크기를 이 디바이스의 최소
	 * I/O 크기(logical block size 에 기반해 드라이버가 보고하는 최소
	 * 효율적 I/O 단위)로 설정한다 - 사용자 공간 툴(예: cp, dd)이 이
	 * 값을 참고해 버퍼 크기를 정하면 NVMe 로의 I/O 가 더 효율적으로
	 * 정렬된다. */

	blkdev_put_no_open(bdev);
	/* [한국어] 위 blkdev_get_no_open() 으로 늘렸던 참조 카운트를
	 * 되돌린다 - open/close 와 무관한 순수 조회 참조였으므로 이 함수가
	 * 반드시 짝을 맞춰 반납해야 참조 누수가 없다. */
}

/*
 * [한국어]
 * disk_live - gendisk 가 아직 완전히 제거되지 않고 살아 있는지(그 대표
 * inode 가 해시 테이블에 남아 있는지) 확인한다.
 * NVMe 연결점: NVMe namespace 가 제거되어 del_gendisk() 경로에서
 * bdev_unhash() 가 호출되면 이 함수는 false 를 반환하게 되고, 이는 이후
 * 새로운 open 시도가 실패해야 함을 의미하는 신호로 쓰인다.
 */

/*
 * [한국어]
 * disk_live - gendisk 가 여전히 살아있는(아직 unhash 되지 않은) 상태인지
 * 검사한다.
 *
 * @disk: 살아있는지 확인할 대상 gendisk(예: NVMe namespace 를 표현하는
 *        gendisk).
 * @return: true 면 아직 살아있어 정상적으로 open 가능, false 면 이미
 *          unhash 되어 더 이상 새로운 open 을 허용해서는 안 되는 상태.
 *
 * gendisk 가 del_gendisk() 로 제거되는 과정에서 bdev_unhash() 를 통해
 * 대표 inode(part0 의 BD_INODE)를 전역 inode 해시에서 빼낸다. 이 함수는
 * 그 해시 등록 여부를 검사함으로써, gendisk 구조체 자체는 아직 메모리
 * 상에 남아 있더라도(참조가 있어 즉시 kfree 되지 않은 경우) 논리적으로는
 * 이미 제거 절차가 시작되어 새로 열면 안 되는 상태인지를 값싸게 판별하는
 * 용도로 쓰인다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 별도 락 없이 inode_unhashed() 의 원자적
 * 플래그 검사에 의존한다(inode 해시 상태는 자체적으로 동기화된다).
 * 호출자: 디바이스가 여전히 유효한지 미리 확인하고 싶은 open 경로/드라이버
 * 코드(EXPORT_SYMBOL_GPL 로 공개).
 * 피호출자: inode_unhashed(), BD_INODE().
 * 에러 처리: 별도 에러 코드 없이 단순 불리언만 반환 - 호출자가 false 를
 * 보면 open 시도를 -ENODEV 등으로 실패 처리하는 책임을 진다.
 *
 * 호출 체인:
 *   (open 경로/드라이버) → [disk_live] → inode_unhashed()
 */
bool disk_live(struct gendisk *disk)
{
	return !inode_unhashed(BD_INODE(disk->part0));
	/* [한국어] disk->part0(이 gendisk 의 대표 파티션, 즉 전체 디스크
	 * 자체를 나타내는 0번 파티션)의 BD_INODE 가 여전히 전역 inode
	 * 해시 테이블에 남아 있는지 검사한다. inode_unhashed() 가 true 면
	 * 이미 빠진 것이므로 그 부정(!)을 반환해, "해시에 남아 있음 =
	 * 살아있음(true)" 의미가 되도록 뒤집는다. */
}
EXPORT_SYMBOL_GPL(disk_live);
/* [한국어] GPL 모듈에서만 호출 가능하도록 심볼을 공개한다. */

/*
 * [한국어]
 * block_size - block_device 의 논리 블록 크기(logical block size)를
 * 바이트 단위로 반환한다.
 * NVMe 연결점: NVMe namespace 가 보고하는 logical block size(예: 512 또는
 * 4096바이트, NVMe Identify Namespace 의 LBA Format 정보에서 유래)에
 * 대응한다.
 */

/*
 * [한국어]
 * block_size - block_device 의 논리 블록 크기를 바이트 단위로 반환한다.
 *
 * @bdev: 블록 크기를 조회할 대상 struct block_device.
 * @return: 이 블록 디바이스의 논리 블록 크기(바이트 단위, 2의 거듭제곱
 *          - 예: 512, 4096). 호출자는 이 값을 I/O 정렬/버퍼 크기 계산에
 *          사용한다.
 *
 * i_blkbits(블록 크기를 나타내는 지수, 즉 블록 크기 = 2^i_blkbits)를
 * 그대로 노출하지 않고 실제 바이트 크기로 변환해 반환하는 얇은 헬퍼로
 * 존재한다 - 많은 호출자가 지수가 아니라 실제 바이트 값을 필요로 하기
 * 때문에, 비트 시프트 변환을 이 함수 하나로 모아 중복을 없앤다.
 * 동작: BD_INODE(bdev) 로 이 bdev 의 내부(internal) inode 를 얻고, 그
 * inode 의 i_blkbits 필드(설정 시 blk_queue_logical_block_size() 등에
 * 의해 결정됨)를 지수로 하여 1을 왼쪽으로 그만큼 시프트한다 - 예를 들어
 * i_blkbits 가 9면 1 << 9 = 512, 12면 1 << 12 = 4096.
 * 실행 컨텍스트: 프로세스 컨텍스트, 락 없이 단순 필드 읽기 - i_blkbits 는
 * 디바이스 초기화 시점에 설정된 뒤 런타임에는 거의 바뀌지 않는 값이므로
 * 별도 동기화 없이 읽는다.
 * 호출자: 파일시스템/블록 계층에서 논리 블록 크기가 필요한 다양한 코드
 * (EXPORT_SYMBOL_GPL 로 공개).
 * 피호출자: BD_INODE().
 * 에러 처리: 실패할 수 없는 단순 계산이므로 에러 처리 없음.
 *
 * 호출 체인:
 *   (파일시스템/블록 계층 코드) → [block_size] → BD_INODE()
 */
unsigned int block_size(struct block_device *bdev)
{
	return 1 << BD_INODE(bdev)->i_blkbits;
	/* [한국어] bdev 의 internal inode 에 저장된 i_blkbits(블록 크기의
	 * 밑이 2인 로그 값)를 지수로 1을 시프트해 실제 바이트 단위 블록
	 * 크기를 계산해 반환한다 - 예: i_blkbits == 12 라면 1 << 12 == 4096,
	 * 즉 NVMe namespace 의 LBA 크기가 4096바이트임을 의미한다. */
}
EXPORT_SYMBOL_GPL(block_size);
/* [한국어] GPL 모듈에서만 호출 가능하도록 심볼을 공개한다. */

/*
 * [한국어]
 * setup_bdev_allow_write_mounted - 커널 커맨드라인 파라미터
 * "bdev_allow_write_mounted=" 를 파싱해 전역 정책 플래그를 설정하는
 * __setup 콜백.
 * NVMe 연결점: 예를 들어 테스트/디버깅 목적으로 마운트되어 있는
 * /dev/nvme0n1 같은 NVMe namespace 에 대해서도(정상적으로는 금지되는)
 * raw 쓰기를 부팅 시점부터 허용할지 여부를 결정하는 스위치다.
 */

/*
 * [한국어]
 * setup_bdev_allow_write_mounted - "bdev_allow_write_mounted=" 커널
 * 커맨드라인 파라미터를 파싱해 bdev_allow_write_mounted 전역 변수를
 * 설정한다.
 *
 * @str: 커널 커맨드라인에서 "bdev_allow_write_mounted=" 뒤에 오는 값
 *       문자열(예: "1", "0", "true", "false" 등 kstrtobool 이 인식하는
 *       형태). 부트로더가 커널에 넘긴 cmdline 을 파싱하는 과정에서
 *       전달된다.
 * @return: __setup 콜백 관례상 1을 반환하면 "이 파라미터를 인식하고
 *          처리했다"는 의미이고, 0을 반환하면 커널이 이 옵션을 처리되지
 *          않은 것으로 간주해 이후 init 프로세스의 argv/envp 로 넘기려
 *          시도한다. 이 함수는 파싱 성공 여부와 무관하게 항상 1을
 *          반환해, 문법이 틀렸더라도 이 파라미터를 "처리됨"으로 소비한다.
 *
 * 부팅 시점에 관리자가 이 옵션으로 bdev_allow_write_mounted 전역 플래그를
 * 미리 켜 두면, 이미 마운트되어 있는 블록 디바이스(예: 루트로 마운트된
 * NVMe namespace)에 대해서도 원래는 안전을 위해 금지되는 직접(raw) 쓰기
 * 접근을 허용할 수 있게 된다 - 주로 테스트/디버깅/복구 시나리오를 위한
 * 스위치다.
 * 동작: kstrtobool() 로 문자열을 불리언으로 파싱해 bdev_allow_write_mounted
 * 에 대입한다. 파싱이 실패하면(인식할 수 없는 문자열) 관리자가 오타를
 * 냈다는 뜻이므로 pr_warn() 으로 경고 메시지를 남기되, 커널 부팅 자체를
 * 막지는 않는다.
 * 실행 컨텍스트: 커널 초기화(init) 단계, 아직 일반적인 프로세스 스케줄링
 * 이전의 이른 부팅 시점에 커맨드라인 파서에 의해 동기적으로 한 번만 호출
 * 된다. 동시성 문제 없음(단일 부팅 스레드).
 * 호출자: 커널 초기화 코드가 __setup 매크로로 등록된 콜백 테이블을 순회
 * 하며 "bdev_allow_write_mounted=" 로 시작하는 커맨드라인 토큰을 만나면
 * 이 함수를 호출한다.
 * 피호출자: kstrtobool(), pr_warn().
 * 에러 처리: 파싱 실패 시 경고만 출력하고 부팅을 계속 진행한다 - 이 경우
 * bdev_allow_write_mounted 는 kstrtobool 이 실패 시 건드리지 않은 이전
 * 값(기본값)을 유지한다.
 *
 * 호출 체인:
 *   (커널 커맨드라인 파서) → [setup_bdev_allow_write_mounted] → kstrtobool()
 */
static int __init setup_bdev_allow_write_mounted(char *str)
{
	if (kstrtobool(str, &bdev_allow_write_mounted))
		/* [한국어] 커맨드라인에서 넘어온 문자열 str 을 불리언 값으로
		 * 해석해 전역 변수 bdev_allow_write_mounted 에 저장한다.
		 * kstrtobool() 은 "y"/"n", "1"/"0", "true"/"false" 등 다양한
		 * 표기를 인식하며, 인식하지 못하는 문자열이면 음수 errno 를
		 * 반환한다(대입은 하지 않음) - 그 실패 여부를 if 조건으로
		 * 검사한다. */
		pr_warn("Invalid option string for bdev_allow_write_mounted:"
			" '%s'\n", str);
			/* [한국어] 파싱에 실패했을 때 관리자가 커맨드라인에
			 * 잘못된 문자열(str)을 넘겼음을 커널 로그에 경고로
			 * 남긴다 - 어떤 값이 문제였는지 %s 로 그대로
			 * 노출해 디버깅을 돕는다. */
	return 1;
	/* [한국어] __setup 콜백 관례에 따라 항상 1을 반환해, 파싱 성공/
	 * 실패와 무관하게 이 커맨드라인 토큰을 "이미 처리됨"으로 표시한다
	 * - 그래야 커널이 이 옵션을 알 수 없는 파라미터로 취급해 init
	 * 프로세스에 그대로 전달하려는 시도를 하지 않는다. */
}
__setup("bdev_allow_write_mounted=", setup_bdev_allow_write_mounted);
/* [한국어] __setup 매크로는 ("문자열", 콜백함수) 쌍을 커널 이미지의
 * 특수 섹션(.init.setup)에 등록해 두는 역할을 한다. 부팅 초기에 커널이
 * 커맨드라인을 토큰 단위로 순회하면서, 각 토큰이 여기 등록된 문자열로
 * 시작하면 대응하는 콜백(setup_bdev_allow_write_mounted)을 호출해 준다
 * - 즉 이 한 줄이 "bdev_allow_write_mounted=" 옵션과 위 파서 함수를
 * 커널 부팅 파라미터 처리 인프라에 연결하는 등록 지점이다. */

/*
 * NVMe 관점 핵심 요약
 *
 * - block/bdev.c 는 struct block_device 의 생명주기(open/close/claim/flush)를
 *   관리하며, NVMe SSD 의 namespace 나 파티션에 대한 VFS 진입점 역할을 한다.
 * - bio -> bdev -> bd_queue -> request_queue -> nvme_queue -> doorbell 의 경로에서
 *   bdev 는 bio 가 NVMe SQ/CID/PRP/SGL 로 변환되기 직전의 마지막 추상화 객체다.
 * - bdev_freeze/thaw, sync_blockdev, bdev_mark_dead 등은 NVMe Flush/Write
 *   명령 발행과 밀접하게 연결된 캐시 동기화/무효화 지점이다.
 * - exclusive claim(bd_prepare_to_claim ... bd_finish_claiming)은 NVMe 디바이스를
 *   포맷하거나 LVM 으로 잡을 때 namespace 단위 동시 접근을 방어한다.
 * - 이 파일은 block/genhd.c(gendisk 관리), block/blk-core.c(bio 경로),
 *   block/blk-mq.c(request 할당/스케줄링)와 논리적으로 연결된다.
 */
