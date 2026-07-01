// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *  Copyright (C) 2001  Andrea Arcangeli <andrea@suse.de> SuSE
 *  Copyright (C) 2016 - 2020 Christoph Hellwig
 */

/*
 * NVMe SSD 관점 파일 요약:
 *
 * 이 파일은 struct block_device(bdev) 객체의 생명주기, 열기/닫기, 캐시 관리,
 * 그리고 VFS와의 연결을 담당한다. NVMe SSD 입장에서 볼 때 bdev는 파일시스템이
 * 생성한 read/write IO 요청이 request_queue를 거쳐 nvme_queue로 도달하기 전의
 * 마지막 추상화 계층이다. 즉, blk_mq_submit_bio -> blk_mq_get_request ->
 * nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로에서 bdev는 bio가 매핑되는
 * 대상 디바이스를 식별하는 핵심 핸들 역할을 한다.
 *
 * 이 파일은 block/genhd.c, block/blk-core.c, block/blk-mq.c 등과 논리적으로
 * 연결되며, bdev_open() 이후의 IO 경로는 blk_mq_submit_bio() 쪽으로 이어진다.
 */

#include <linux/init.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/kmod.h>
#include <linux/major.h>
#include <linux/device_cgroup.h>
#include <linux/blkdev.h>
#include <linux/blk-integrity.h>
#include <linux/backing-dev.h>
#include <linux/module.h>
#include <linux/blkpg.h>
#include <linux/magic.h>
#include <linux/buffer_head.h>
#include <linux/swap.h>
#include <linux/writeback.h>
#include <linux/mount.h>
#include <linux/pseudo_fs.h>
#include <linux/uio.h>
#include <linux/namei.h>
#include <linux/security.h>
#include <linux/part_stat.h>
#include <linux/uaccess.h>
#include <linux/stat.h>
#include "../fs/internal.h"
#include "blk.h"

/* NVMe: 마운트된 bdev에 대한 쓰기 허용 여부. 기본적으로는 커널 설정에 따륾. */

/* Should we allow writing to mounted block devices? */
static bool bdev_allow_write_mounted = IS_ENABLED(CONFIG_BLK_DEV_WRITE_MOUNTED);
// NVMe: 마운트된 NVMe namespace 에 대한 raw 쓰기 허용 여부. false 면 상위 VFS/FS 레벨에서 쓰기가 차단되어 NVMe SQ 로 날아가지 않음.

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
// NVMe: namespace 또는 파티션에 해당하는 실제 block_device.
	struct inode vfs_inode;
// NVMe: VFS inode -> address_space, bio 가 매핑되는 페이지 캐시의 출발점.
};

/* VFS inode 로부터 bdev_inode(container_of) 를 얻는다. */

static inline struct bdev_inode *BDEV_I(struct inode *inode)
{
	return container_of(inode, struct bdev_inode, vfs_inode);
// NVMe: VFS inode -> bdev_inode -> bdev 경로. bio->bi_bdev 는 이 값을 직접 참조.
}

/* block_device 로부터 VFS inode 를 얻는다. 페이지 캐시 매핑의 출발점. */

static inline struct inode *BD_INODE(struct block_device *bdev)
{
	return &container_of(bdev, struct bdev_inode, bdev)->vfs_inode;
// NVMe: bdev -> VFS inode, 페이지 캐시 매핑의 출발점.
}

/*
 * I_BDEV - VFS inode 로부터 struct block_device 를 얻는다.
 * 파일시스템이 생성한 bio 가 전달되는 대상 NVMe 네임스페이스를 식별할 때
 * bio->bi_bdev 를 통해 이 함수가 사용된다(실제로는 직접 포인터 사용).
 */

struct block_device *I_BDEV(struct inode *inode)
{
	return &BDEV_I(inode)->bdev;
// NVMe: VFS inode 로부터 bdev 획득. 파일시스템이 생성한 bio 의 대상 namespace 식별.
}
EXPORT_SYMBOL(I_BDEV);

/* 열린 파일 구조체로부터 block_device 를 얻는다. /dev/nvme0n1 열기 시 사용. */

struct block_device *file_bdev(struct file *bdev_file)
{
	return I_BDEV(bdev_file->f_mapping->host);
// NVMe: /dev/nvme0n1 등을 open 한 struct file 에서 bdev 획득.
}
EXPORT_SYMBOL(file_bdev);

/*
 * bdev_write_inode - bdev 의 VFS inode 가 dirty 상태면 즉시 writeback 한다.
 *
 * 목적: bdev 를 닫거나 캐시를 비울 때, inode 메타데이터를 디스크로 날린다.
 * NVMe 연결점: NVMe 플러시 명령(Flush)이 실제 NAND 쓰기가 아닌 메타데이터
 *            동기화가 필요한 경우 이 경로를 통해 dirty inode 가 정리된다.
 *            (추정) NVMe flush 명령은 이 함수 직접 호출보다는 block layer 의
 *            플러시 경로에서 별도로 처리된다.
 */

static void bdev_write_inode(struct block_device *bdev)
{
	struct inode *inode = BD_INODE(bdev);
	int ret;

	spin_lock(&inode->i_lock);
	while (inode_state_read(inode) & I_DIRTY) {
// NVMe: dirty inode 가 남아 있으면 writeback 경로를 통해 NVMe Flush/Write 명령으로 최종 동기화.
		spin_unlock(&inode->i_lock);
		ret = write_inode_now(inode, true);
// NVMe: sync writeback. 이 메타데이터가 NVMe volatile write cache -> NAND 로 기록되는 여기서 시작됨.
		if (ret)
// NVMe: writeback 실패 시 NVMe 컨트롤러는 메타데이터 업데이트를 보장하지 못할 수 있음.
			pr_warn_ratelimited(
	"VFS: Dirty inode writeback failed for block device %pg (err=%d).\n",
				bdev, ret);
		spin_lock(&inode->i_lock);
// NVMe: I_DIRTY 가 클리어될 때까지 재시도, NVMe 메타데이터 일관성을 위해.
	}
	spin_unlock(&inode->i_lock);
}

/*
 * kill_bdev - bdev 의 모든 버퍼와 페이지 캐시를 제거한다(dirty 여부 무관).
 * NVMe 연결점: NVMe SSD가 detachable 미디어를 가진 경우(예: 외장 NVMe) 미디어
 *            제거 전 캐시를 완전히 버려서 오래된 데이터가 노출되지 않도록 한다.
 */

/* Kill _all_ buffers and pagecache , dirty or not.. */
static void kill_bdev(struct block_device *bdev)
{
	struct address_space *mapping = bdev->bd_mapping;
// NVMe: 이 bdev 의 페이지 캐시(address_space). NVMe IO 의 버퍼링 계층.

	if (mapping_empty(mapping))
// NVMe: 캐시가 비어 있으면 NVMe 로 flush 할 dirty 데이터도 없음.
		return;

	invalidate_bh_lrus();
// NVMe: 버퍼 헤드 LRU 를 무효화하여 NVMe read cache 일관성 확보.
	truncate_inode_pages(mapping, 0);
// NVMe: 모든 페이지 캐시 제거. NVMe SQ/CQ 와는 직접 관계없지만 캐시 드레인 효과.
}

/*
 * invalidate_bdev - 깨끗하고 사용하지 않는 버퍼/페이지 캐시만 무효화한다.
 * NVMe 연결점: NVMe 디바이스가 예상치 못한 제거(surprise removal)를 겪었을 때
 *            캐시된 read 데이터가 실제 NAND 와 일치하지 않을 수 있으므로
 *            이 함수를 통해 정리한다( dirty 데이터는 제거하지 않음 ).
 */

/* Invalidate clean unused buffers and pagecache. */
void invalidate_bdev(struct block_device *bdev)
{
	struct address_space *mapping = bdev->bd_mapping;

	if (mapping->nrpages) {
// NVMe: 캐시 페이지가 존재할 때만 무효화를 시도, 불필요한 NVMe 캐시 스캔 회피.
		invalidate_bh_lrus();
		lru_add_drain_all();	/* make sure all lru add caches are flushed */
// NVMe: 버퍼 헤드 LRU 를 무효화.
		invalidate_mapping_pages(mapping, 0, -1);
// NVMe: per-cpu LRU 드레인. NVMe IO 완료 후 재활용 중인 페이지 상태를 정리.
	}
// NVMe: clean 페이지만 무효화. dirty 페이지는 보호하여 NVMe writeback 데이터 손실 방지.
}
EXPORT_SYMBOL(invalidate_bdev);

/*
 * Drop all buffers & page cache for given bdev range. This function bails
 * with error if bdev has other exclusive owner (such as filesystem).
 */
/*
 * truncate_bdev_range - 지정한 bdev 영역의 버퍼와 페이지 캐시를 버린다.
 *
 * 목적: 디스카드(discard/trim)나 캐시 재구성 전에 영역별 캐시를 정리.
 * 호출 경로: 파일시스템 discard ioctl -> blkdev_issue_discard -> ...
 *           -> truncate_bdev_range (필요시).
 * NVMe 연결점: NVMe Deallocate(Dataset Management) 명령 전에 이 함수로
 *            관련 페이지 캐시를 날리면, 호스트 메모리와 NAND 의 불일치를
 *            방지할 수 있다.
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
// NVMe: BLK_OPEN_EXCL 이 아닌 discard ioctl 등은 임시 exclusive claim 이 필요.
		int err = bd_prepare_to_claim(bdev, truncate_bdev_range, NULL);
// NVMe: NVMe namespace 를 잠시 exclusive 로 claim 해 캐시와 NAND 간 불일치를 방지.
		if (err)
			goto invalidate;
	}

	truncate_inode_pages_range(bdev->bd_mapping, lstart, lend);
// NVMe: discard/write-zeroes 대상 영역의 페이지 캐시 제거. NVMe Deallocate 전에 수행.
	if (!(mode & BLK_OPEN_EXCL))
		bd_abort_claiming(bdev, truncate_bdev_range);
// NVMe: 임시 claim 해제 후 일반 NVMe read/write IO 가 다시 가능해짐.
	return 0;

invalidate:
	/*
	 * Someone else has handle exclusively open. Try invalidating instead.
	 * The 'end' argument is inclusive so the rounding is safe.
	 */
	return invalidate_inode_pages2_range(bdev->bd_mapping,
// NVMe: exclusive claim 실패 시 fallback 으로 clean 페이지만 무효화, dirty 데이터는 NVMe writeback 유지.
					     lstart >> PAGE_SHIFT,
					     lend >> PAGE_SHIFT);
}

/*
 * set_init_blocksize - bdev 의 초기 블록 크기와 folio min_order 를 설정.
 *
 * NVMe 연결점: NVMe 네임스페이스의 LBAF(Logical Block Format)에 따른
 *            logical_block_size(일반적으로 512B 또는 4KiB)를 기준으로 시작.
 *            페이지 크기보다 작은 경우 2의 거듭제곱으로 정렬된 블록 크기를
 *            선택하여, NVMe PRP/SGL 정렬 요구사항과 호환되도록 한다.
 */

static void set_init_blocksize(struct block_device *bdev)
{
	unsigned int bsize = bdev_logical_block_size(bdev);
// NVMe: namespace 의 LBAF(logical block format)에 따른 logical_block_size (일반 512B 또는 4KiB).
	loff_t size = i_size_read(BD_INODE(bdev));
// NVMe: Identify Namespace 로 보고된 총 LBA 수에서 변환한 바이트 크기.

	while (bsize < PAGE_SIZE) {
// NVMe: 페이지보다 작은 블록 크기를 2의 거듭제곱으로 키워 NVMe PRP/SGL 정렬 요구사항에 맞춤.
		if (size & bsize)
// NVMe: 디바이스 용량이 현재 bsize 의 배수가 아니면 더 이상 키우지 않음(주소 정렬 유지).
			break;
		bsize <<= 1;
// NVMe: 정렬 단위를 2배로 증가. PRP entry 는 메모리 주소/오프셋 정렬을 요구함.
	}
	BD_INODE(bdev)->i_blkbits = blksize_bits(bsize);
// NVMe: 파일시스템이 생성하는 bio 의 최소 단위가 NVMe LBA 단위와 일치하도록 설정.
	mapping_set_folio_min_order(BD_INODE(bdev)->i_mapping,
// NVMe: 페이지 캐시 folio 의 최소 order 를 NVMe 블록 크기에 맞춰 DMA/PRP fragmentation 방지.
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
 * bdev_validate_blocksize - 사용자가 설정하려는 블록 크기가 디바이스가
 *                           지원하는 크기 이상인지 검증.
 * NVMe 연결점: NVMe namespace 의 logical_block_size 보다 작은 블록 크기는
 *            PRP 엔트리 정렬이나 섹터 어드레싱에 문제를 일으킬 수 있으므로
 *            여기서 거부한다.
 */

int bdev_validate_blocksize(struct block_device *bdev, int block_size)
{
	if (blk_validate_block_size(block_size))
// NVMe: 커널이 허용하는 블록 크기(512B, 4KiB 등)인지 검증.
		return -EINVAL;

	/* Size cannot be smaller than the size supported by the device */
	if (block_size < bdev_logical_block_size(bdev))
// NVMe: 설정하려는 블록 크기가 NVMe logical_block_size 보다 작으면 PRP/SGL 정렬 및 LBA 어드레싱 위반 가능.
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_GPL(bdev_validate_blocksize);

/*
 * set_blocksize - 블록 크기를 변경하고 페이지 캐시를 재구성.
 *
 * NVMe 연결점: NVMe SSD에 대해 4KiB 섹터 모드로 전환할 때 사용될 수 있으며,
 *            변경 전 sync_blockdev() 와 kill_bdev() 로 캐시를 모두 플러시.
 *            이는 NVMe 커맨드의 LBA 단위와 페이지 캐시 단위를 재맞추는
 *            중요한 단계다.
 */

int set_blocksize(struct file *file, int size)
{
	struct inode *inode = file->f_mapping->host;
	struct block_device *bdev = I_BDEV(inode);
	int ret;

	ret = bdev_validate_blocksize(bdev, size);
// NVMe: NVMe namespace 가 지원하는 블록 크기인지 먼저 검증.
	if (ret)
		return ret;

	if (!file->private_data)
// NVMe: bdev_inode 가 아닌 파일이면 block_device 와의 IO 경로가 잘못될 수 있음.
		return -EINVAL;

	/* Don't change the size if it is same as current */
	if (inode->i_blkbits != blksize_bits(size)) {
// NVMe: 실제로 블록 크기가 변경될 때만 캐시를 재구성.
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
		inode_lock(inode);
		filemap_invalidate_lock(inode->i_mapping);

		sync_blockdev(bdev);
// NVMe: NVMe Flush/Write 명령으로 dirty 데이터를 비휘발성 NAND 에 먼저 기록.
		kill_bdev(bdev);
// NVMe: 캐시를 완전히 비워 NVMe PRP/SGL 재구성 시 stale 데이터가 섞이지 않도록 함.

		inode->i_blkbits = blksize_bits(size);
// NVMe: 파일시스템 bio 단위와 NVMe LBA 단위를 재맞춤.
		mapping_set_folio_min_order(inode->i_mapping, get_order(size));
// NVMe: 페이지 캐시 folio 크기를 새 블록 단위로 재설정.
		filemap_invalidate_unlock(inode->i_mapping);
		inode_unlock(inode);
	}
	return 0;
}

EXPORT_SYMBOL(set_blocksize);

/* 파일시스템이 큰 블록 크기(페이지 크기 초과)를 지원하는지 검증. */

static int sb_validate_large_blocksize(struct super_block *sb, int size)
{
	const char *err_str = NULL;

	if (!(sb->s_type->fs_flags & FS_LBS))
// NVMe: 파일시스템이 PAGE_SIZE 초과 블록(예: 4KiB 섹터 NVMe)을 지원하는지 확인.
		err_str = "not supported by filesystem";
	else if (!IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE))
// NVMe: 큰 folio 는 TRANSPARENT_HUGEPAGE 가 필요.
		err_str = "is only supported with CONFIG_TRANSPARENT_HUGEPAGE";

	if (!err_str)
		return 0;

	pr_warn_ratelimited("%s: block size(%d) > page size(%lu) %s\n",
				sb->s_type->name, size, PAGE_SIZE, err_str);
	return -EINVAL;
}

/*
 * sb_set_blocksize - 슈퍼블록의 블록 크기를 bdev 크기에 맞춰 설정.
 * NVMe 연결점: ext4/xfs 등이 NVMe namespace 를 마운트할 때 호출되며,
 *            이 블록 크기가 곧 파일시스템이 생성하는 bio 의 단위가 된다.
 */

int sb_set_blocksize(struct super_block *sb, int size)
{
	if (size > PAGE_SIZE && sb_validate_large_blocksize(sb, size))
// NVMe: 페이지보다 큰 블록은 THP 및 FS_LBS 기능이 필요. NVMe 4KiB 섹터 모드에서도 동일.
		return 0;
	if (set_blocksize(sb->s_bdev_file, size))
// NVMe: 실제 bdev 블록 크기를 변경.
		return 0;
	/* If we get here, we know size is validated */
	sb->s_blocksize = size;
// NVMe: 파일시스템이 생성하는 bio 의 크기가 NVMe LBA 단위가 됨.
	sb->s_blocksize_bits = blksize_bits(size);
	return sb->s_blocksize;
}

EXPORT_SYMBOL(sb_set_blocksize);

/* 파일시스템이 요청한 블록 크기가 NVMe logical block size 보다 작으면
 * 디바이스 최소 블록 크기로 올린다. */

int __must_check sb_min_blocksize(struct super_block *sb, int size)
{
	int minsize = bdev_logical_block_size(sb->s_bdev);
// NVMe: namespace 의 최소 logical_block_size 를 하한으로 설정.
	if (size < minsize)
// NVMe: FS 요청이 NVMe LBAF 보다 작으면 PRP/SGL 정렬을 위해 강제 올림.
		size = minsize;
	return sb_set_blocksize(sb, size);
}

EXPORT_SYMBOL(sb_min_blocksize);

/*
 * sync_blockdev_nowait - bdev 의 dirty 페이지를 비동기로 플러시 시작.
 * NVMe 연결점: filemap_flush -> writepages -> blk_mq_submit_bio -> ...
 *            -> nvme_queue_rq, NVMe Flush 명령으로 최종적으로 매핑될 수 있다.
 */

int sync_blockdev_nowait(struct block_device *bdev)
{
	if (!bdev)
// NVMe: NULL bdev 체크. 제거된 namespace 에 대한 flush 는 no-op.
		return 0;
	return filemap_flush(bdev->bd_mapping);
// NVMe: 비동기 writeback 시작 -> blk_mq_submit_bio -> nvme_queue_rq -> NVMe SQ Write 명령 삽입.
}
EXPORT_SYMBOL_GPL(sync_blockdev_nowait);

/*
 * sync_blockdev - bdev 의 모든 dirty 데이터를 디스크로 쓰고 완료 대기.
 * NVMe 연결점: fsync() 수행 시 이 경로를 타고 NVMe Flush 명령이 발행되어
 *            volatile write cache 의 데이터를 비휘발성 NAND 에 기록.
 */

/*
 * Write out and wait upon all the dirty data associated with a block
 * device via its mapping.  Does not take the superblock lock.
 */
int sync_blockdev(struct block_device *bdev)
{
	if (!bdev)
// NVMe: NULL bdev 이면 동기화할 대상 없음.
		return 0;
	return filemap_write_and_wait(bdev->bd_mapping);
// NVMe: dirty 페이지를 모두 NVMe Write/Flush 명령으로 날리고 완료까지 대기.
}
EXPORT_SYMBOL(sync_blockdev);

/*
 * sync_blockdev_range - 지정 범위의 dirty 데이터를 플러시하고 대기.
 * NVMe 연결점: range 단위 flush 가 NVMe Native 명령셋에는 없으므로,
 *            커널은 일반 Write 명령으로 해당 범위를 먼저 날리고 완료를
 *            기다린다(추정).
 */

int sync_blockdev_range(struct block_device *bdev, loff_t lstart, loff_t lend)
{
	return filemap_write_and_wait_range(bdev->bd_mapping,
// NVMe: range 단위 flush 는 NVMe Native 명령셋에 없으므로 해당 범위 Write 명령 완료로 대체(추정).
			lstart, lend);
}
EXPORT_SYMBOL(sync_blockdev_range);

/*
 * bdev_freeze - 파일시스템을 동결하고 일관된 상태로 만든다(snapshot 전).
 *
 * 호출 경로: LVM snapshot ioctl -> bdev_freeze -> bd_holder_ops->freeze
 *           또는 sync_blockdev -> filemap_write_and_wait.
 * NVMe 연결점: 동결 시점까지의 dirty 데이터를 NVMe SSD에 모두 기록하여
 *            스냅샷이 생성되는 시점의 블록 이미지가 일관되게 만든다.
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

	mutex_lock(&bdev->bd_fsfreeze_mutex);
// NVMe: freeze 카운터와 관련된 상태 보호.

	if (atomic_inc_return(&bdev->bd_fsfreeze_count) > 1) {
// NVMe: NVMe namespace 동결 중첩 카운트. 1보다 크면 이미 freeze 중.
		mutex_unlock(&bdev->bd_fsfreeze_mutex);
		return 0;
// NVMe: 이미 freeze 되어 있으면 추가 NVMe flush 는 불필요.
	}

	mutex_lock(&bdev->bd_holder_lock);
// NVMe: holder ops(freeze 콜백) 보호.
	if (bdev->bd_holder_ops && bdev->bd_holder_ops->freeze) {
		error = bdev->bd_holder_ops->freeze(bdev);
// NVMe: LVM 등 holder 의 freeze 콜백에서 NVMe 캐시 동기화 정책 수행.
		lockdep_assert_not_held(&bdev->bd_holder_lock);
	} else {
		mutex_unlock(&bdev->bd_holder_lock);
		error = sync_blockdev(bdev);
// NVMe: holder 가 없으면 직접 sync_blockdev() 로 NVMe Flush/Write 완료.
	}

	if (error)
		atomic_dec(&bdev->bd_fsfreeze_count);
// NVMe: freeze 실패 시 카운트를 원복하여 NVMe thaw 상태 복구.

	mutex_unlock(&bdev->bd_fsfreeze_mutex);
	return error;
}
EXPORT_SYMBOL(bdev_freeze);

/*
 * bdev_thaw - bdev_freeze() 로 동결된 파일시스템을 다시 쓰기 가능하게 푼다.
 * NVMe 연결점: 동결이 풀린 후 다시 일반 read/write/trim NVMe 커맨드가
 *            request_queue 를 통해 흐를 수 있게 된다.
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

	mutex_lock(&bdev->bd_fsfreeze_mutex);
// NVMe: freeze 카운터 보호.

	/*
	 * If this returns < 0 it means that @bd_fsfreeze_count was
	 * already 0 and no decrement was performed.
	 */
	nr_freeze = atomic_dec_if_positive(&bdev->bd_fsfreeze_count);
// NVMe: NVMe namespace 동결 중첩 카운트 감소.
	if (nr_freeze < 0)
// NVMe: 카운트가 이미 0이면 thaw 호출이 잘못됨.
		goto out;

	error = 0;
	if (nr_freeze > 0)
// NVMe: 중첩 freeze 가 남아 있으면 실제 NVMe thaw 는 아직 수행 안 함.
		goto out;

	mutex_lock(&bdev->bd_holder_lock);
	if (bdev->bd_holder_ops && bdev->bd_holder_ops->thaw) {
// NVMe: holder 의 thaw 콜백에서 NVMe 캐시 잠금 해제.
		error = bdev->bd_holder_ops->thaw(bdev);
		lockdep_assert_not_held(&bdev->bd_holder_lock);
	} else {
		mutex_unlock(&bdev->bd_holder_lock);
	}

	if (error)
		atomic_inc(&bdev->bd_fsfreeze_count);
// NVMe: thaw 실패 시 freeze 카운트 복구.
out:
	mutex_unlock(&bdev->bd_fsfreeze_mutex);
	return error;
}
EXPORT_SYMBOL(bdev_thaw);

/*
 * pseudo-fs - bdev 전용 익명 슈퍼블록/마운트.
 *
 * /dev/nvme0n1 과 같은 블록 장치 노드는 실제 파일시스템이 아닌 bdevfs 에
 * 속한다. 여기서 inode 가 할당되고, bdev->bd_mapping 이 설정되어
 * NVMe IO 의 페이지 캐시 기반이 마련된다.
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

static  __cacheline_aligned_in_smp DEFINE_MUTEX(bdev_lock);
// NVMe: bdev claim 전역 뮤텍스. NVMe exclusive open(mkfs, LVM) 직렬화.
static struct kmem_cache *bdev_cachep __ro_after_init;
// NVMe: bdev_inode 슬래브. NVMe namespace 마다 하나씩 할당/재활용.

/*
 * bdev_alloc_inode - bdevfs 슈퍼블록에 새로운 bdev_inode 를 할당.
 * NVMe 연결점: NVMe 컨트롤러가 namespace 를 검색하면 여기에 대응하는
 *            bdev_inode 가 생성되고, 이후 nvme_queue 와 연결된다.
 */

static struct inode *bdev_alloc_inode(struct super_block *sb)
{
	struct bdev_inode *ei = alloc_inode_sb(sb, bdev_cachep, GFP_KERNEL);
// NVMe: bdevfs 슈퍼블록으로부터 새 bdev_inode 할당.

	if (!ei)
		return NULL;
	memset(&ei->bdev, 0, sizeof(ei->bdev));
// NVMe: block_device 필드(bd_queue, bd_disk 등)를 0으로 초기화.

	if (security_bdev_alloc(&ei->bdev)) {
// NVMe: LSM 보안 레이블 할당. 보안 정책에 따라 NVMe 접근 제어.
		kmem_cache_free(bdev_cachep, ei);
// NVMe: 보안 초기화 실패 시 슬래브를 즉시 반환.
		return NULL;
	}
	return &ei->vfs_inode;
}

/*
 * bdev_free_inode - bdev_inode 와 남은 자원을 해제.
 *
 * @bd_stats: 디스크 I/O 통계(per-cpu). NVMe 성능 모니터링의 원천 데이터.
 * @bd_meta_info: 파티션 메타데이터.
 * @bd_disk: gendisk, NVMe namespace 의 gendisk 가 여기 연결됨.
 */

static void bdev_free_inode(struct inode *inode)
{
	struct block_device *bdev = I_BDEV(inode);
// NVMe: 해제할 block_device 획득.

	free_percpu(bdev->bd_stats); 	/* NVMe: per-cpu IO 통계 구조체 해제. /sys/block/nvme0n1/stat 원본. */
// NVMe: per-cpu IO 통계 구조체 해제. /sys/block/nvme0n1/stat 의 원본 데이터.
	kfree(bdev->bd_meta_info);
// NVMe: 파티션 메타데이터 해제.
	security_bdev_free(bdev);
// NVMe: LSM 보안 레이블 해제.

	if (!bdev_is_partition(bdev)) {
// NVMe: 파티션이 아닌 전체 NVMe namespace 일 때만 gendisk/bdi 해제.
		if (bdev->bd_disk && bdev->bd_disk->bdi)
// NVMe: bdi(backing dev info) 참조가 있으면 해제. writeback 인프라.
			bdi_put(bdev->bd_disk->bdi);
		kfree(bdev->bd_disk);
// NVMe: gendisk 객체 해제. 이후 NVMe request_queue 와의 연결이 끊어짐.
	}

	if (MAJOR(bdev->bd_dev) == BLOCK_EXT_MAJOR)
// NVMe: BLOCK_EXT_MAJOR 를 사용한 확장 minor 일 경우 반환.
		blk_free_ext_minor(MINOR(bdev->bd_dev));
// NVMe: 확장 minor 번호를 풀에 반환.

	kmem_cache_free(bdev_cachep, BDEV_I(inode));
// NVMe: bdev_inode 슬래브 반환. 이 참조가 마지막이면 메모리 해제.
}

/* kmem_cache 에서 새 객체를 받을 때 VFS inode 초기화. */

static void init_once(void *data)
{
	struct bdev_inode *ei = data;

	inode_init_once(&ei->vfs_inode);
// NVMe: VFS inode 를 한 번 초기화. 새 bdev_inode 재활용 시 사용.
}

/*
 * bdev_sops - bdevfs 슈퍼블록 연산.
 * .alloc_inode/.free_inode 는 위 함수들로 bdev_inode 생명주기를 관리.
 */

static const struct super_operations bdev_sops = {
	.statfs = simple_statfs,
	.alloc_inode = bdev_alloc_inode,
	.free_inode = bdev_free_inode,
	.drop_inode = inode_just_drop,
};

/* bdevfs 마운트 컨텍스트를 초기화. cgroup writeback 플래그 설정. */

static int bd_init_fs_context(struct fs_context *fc)
{
	struct pseudo_fs_context *ctx = init_pseudo(fc, BDEVFS_MAGIC);
	if (!ctx)
		return -ENOMEM;
	fc->s_iflags |= SB_I_CGROUPWB;
// NVMe: cgroup writeback 플래그. NVMe IO 에 대한 cgroup 제어 및 쓰기 회계 가능.
	ctx->ops = &bdev_sops;
	return 0;
}

/*
 * bd_type - "bdev" 가상 파일시스템 타입.
 * /dev/nvme0n1 노드는 이 bdevfs 위에 존재하는 inode 이다.
 */

static struct file_system_type bd_type = {
	.name		= "bdev",
	.init_fs_context = bd_init_fs_context,
	.kill_sb	= kill_anon_super,
};

/*
 * blockdev_superblock - bdevfs 의 슈퍼블록.
 * 모든 block_device 의 VFS inode 는 이 슈퍼블록의 inode list 에 연결됨.
 * NVMe 관점: sync_bdevs() 시 이 리스트를 순회하며 NVMe 디바이스들의
 *          dirty 캐시를 플러시한다.
 */

struct super_block *blockdev_superblock __ro_after_init;
static struct vfsmount *blockdev_mnt __ro_after_init;
EXPORT_SYMBOL_GPL(blockdev_superblock);

/*
 * bdev_cache_init - 부팅 시 bdev_cache 와 bdevfs 를 초기화.
 * NVMe 연결점: 이 초기화가 완료된 후 NVMe 드라이버가 로드되어 namespace
 *            별 bdev_inode 를 할당할 수 있다.
 */

void __init bdev_cache_init(void)
{
	int err;

	bdev_cachep = kmem_cache_create("bdev_cache", sizeof(struct bdev_inode),
// NVMe: bdev_inode 를 위한 kmem_cache 생성.
			0, (SLAB_HWCACHE_ALIGN|SLAB_RECLAIM_ACCOUNT|
// NVMe: 하드웨어 캐시라인 정렬, false sharing 감소.
				SLAB_ACCOUNT|SLAB_PANIC),
// NVMe: 메모리 회계 및 초기화 실패 시 panic.
			init_once);
	err = register_filesystem(&bd_type);
// NVMe: "bdev" pseudo 파일시스템 등록.
	if (err)
		panic("Cannot register bdev pseudo-fs");
	blockdev_mnt = kern_mount(&bd_type);
// NVMe: 커널 내부에서 bdevfs 마운트.
	if (IS_ERR(blockdev_mnt))
		panic("Cannot create bdev pseudo-fs");
	blockdev_superblock = blockdev_mnt->mnt_sb;   /* For writeback */
// NVMe: writeback 시 모든 bdev inode 리스트를 순회하는 출발점.
}

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

struct block_device *bdev_alloc(struct gendisk *disk, u8 partno)
{
	struct block_device *bdev;
	struct inode *inode;

	inode = new_inode(blockdev_superblock);
// NVMe: bdevfs 슈퍼블록에서 새 inode 할당.
	if (!inode)
		return NULL;
	inode->i_mode = S_IFBLK;
// NVMe: 블록 장치 노드로 설정.
	inode->i_rdev = 0;
// NVMe: 초기 major/minor 0, bdev_add() 에서 실제 dev_t 설정.
	inode->i_data.a_ops = &def_blk_aops;
// NVMe: 기본 블록 address_space 연산.
	mapping_set_gfp_mask(&inode->i_data, GFP_USER);
// NVMe: 사용자 페이지 할당 마스크 설정.

	bdev = I_BDEV(inode);
	mutex_init(&bdev->bd_fsfreeze_mutex);
// NVMe: freeze/thaw 상태 보호 뮤텍스.
	spin_lock_init(&bdev->bd_size_lock);
// NVMe: bd_nr_sectors 와 i_size 동시 갱신 보호.
	mutex_init(&bdev->bd_holder_lock);
	atomic_set(&bdev->__bd_flags, partno);
// NVMe: 파티션 번호 및 BD_* 플래그 원자적 설정.
	bdev->bd_mapping = &inode->i_data;
// NVMe: 페이지 캐시 address_space 연결.
	bdev->bd_queue = disk->queue; 	/* NVMe: bdev->bd_queue 가 NVMe request_queue 를 가리킴. */
// NVMe: bdev->bd_queue 가 NVMe request_queue 를 가리킴. bio 는 이 queue 로 진입.
	if (partno && bdev_test_flag(disk->part0, BD_HAS_SUBMIT_BIO)) 	/* NVMe: disk 가 직접 submit_bio 를 구현하면 파티션에도 플래그 상속. */
// NVMe: NVMe multipath 등에서 disk 가 직접 submit_bio 를 구현하면 파티션에도 상속.
		bdev_set_flag(bdev, BD_HAS_SUBMIT_BIO);
	bdev->bd_stats = alloc_percpu(struct disk_stats);
// NVMe: per-cpu IO 통계 할당. 추후 /sys/block/nvme0n1/stat 등에 노출.
	if (!bdev->bd_stats) {
		iput(inode);
		return NULL;
	}
	bdev->bd_disk = disk;
// NVMe: NVMe namespace 를 표현하는 gendisk 역참조.
	return bdev;
}

/*
 * bdev_set_nr_sectors - bdev 의 논리적 크기(섹터 수)를 설정.
 * NVMe 연결점: NVMe Identify Namespace 에서 보고한 NN/NLBAF 에 기반한
 *            총 LBA 수를 여기에 반영. 상위 파일시스템은 이 크기를 보고
 *            주소 범위를 제한한다.
 */

void bdev_set_nr_sectors(struct block_device *bdev, sector_t sectors)
{
	spin_lock(&bdev->bd_size_lock);
// NVMe: bd_nr_sectors 와 i_size 동시 갱신 시 race 방지.
	i_size_write(BD_INODE(bdev), (loff_t)sectors << SECTOR_SHIFT);
// NVMe: Identify Namespace 의 총 LBA 수를 inode 크기로 반영.
	bdev->bd_nr_sectors = sectors;
// NVMe: 총 LBA 수. 파일시스템은 이 값으로 주소 범위 제한.
	spin_unlock(&bdev->bd_size_lock);
}

/*
 * bdev_add - bdev 에 dev_t 를 부여하고 inode 해시에 삽입.
 * NVMe 연결점: /dev/nvme0n1 의 major/minor 가 inode->i_rdev 에 설정되어
 *            사용자 공간 open() 과 커널의 block_device 가 연결된다.
 */

void bdev_add(struct block_device *bdev, dev_t dev)
{
	struct inode *inode = BD_INODE(bdev);
	if (bdev_stable_writes(bdev))
// NVMe: NVMe namespace 가 stable writes(쓰기 완료 보장)를 필요로 하면 플래그 설정.
		mapping_set_stable_writes(bdev->bd_mapping);
// NVMe: stable writes 플래그는 NVMe flush/write cache 정책과 연결.
	bdev->bd_dev = dev;
// NVMe: 커널 내부 dev_t 설정.
	inode->i_rdev = dev;
// NVMe: 사용자 공간 major/minor 와 매핑.
	inode->i_ino = dev;
// NVMe: inode 번호를 dev_t 로 설정.
	insert_inode_hash(inode);
// NVMe: inode 해시에 삽입하여 /dev/nvme0n1 경로 조회 가능.
}

/* bdev 를 inode 해시에서 제거. NVMe namespace 가 사라질 때 호출. */

void bdev_unhash(struct block_device *bdev)
{
	remove_inode_hash(BD_INODE(bdev));
// NVMe: namespace 제거 시 inode 해시에서 제거. 이후 open 실패.
}

/* bdev 의 inode 참조를 해제. 참조 카운트가 0이면 bdev_free_inode 호출. */

void bdev_drop(struct block_device *bdev)
{
	iput(BD_INODE(bdev));
// NVMe: inode 참조 해제. 마지막 참조면 bdev_free_inode -> gendisk/queue 정리.
}

/*
 * nr_blockdev_pages - 시스템 전체 bdev 페이지 캐시 페이지 수 합계.
 * NVMe 연결점: drop_caches 나 메모리 부족 시 NVMe SSD 의 캐시가 얼마나
 *            많은 시스템 메모리를 차지하는지 파악하는 데 사용.
 */

long nr_blockdev_pages(void)
{
	struct inode *inode;
	long ret = 0;

	spin_lock(&blockdev_superblock->s_inode_list_lock);
// NVMe: 전체 bdev inode 리스트 보호.
	list_for_each_entry(inode, &blockdev_superblock->s_inodes, i_sb_list)
// NVMe: 시스템에 등록된 모든 NVMe namespace 의 inode 를 순회.
		ret += inode->i_mapping->nrpages;
// NVMe: 각 namespace 의 페이지 캐시 페이지 수를 합산.
	spin_unlock(&blockdev_superblock->s_inode_list_lock);

	return ret;
}

/*
 * bd_may_claim - bdev 를 @holder 가 exclusive 로 claim 할 수 있는지 검사.
 * NVMe 연결점: NVMe 디바이스를 mkfs 로 포맷하거나 LVM 으로 잡을 때
 *            exclusive claim 이 필요. 다른 holder 가 있으면 -EBUSY.
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
// NVMe: 파티션인 경우 전체 namespace 를 가져옴.

	lockdep_assert_held(&bdev_lock);
// NVMe: lockdep: bdev_lock 보유 검증. exclusive claim 직렬화.

	if (bdev->bd_holder) {
// NVMe: 이미 다른 holder 가 exclusive claim 중이면 거부.
		/*
		 * The same holder can always re-claim.
		 */
		if (bdev->bd_holder == holder) {
// NVMe: 동일 holder 의 재claim 은 허용.
			if (WARN_ON_ONCE(bdev->bd_holder_ops != hops))
				return false;
// NVMe: holder ops 가 바뀌면 안 됨.
			return true;
		}
		return false;
	}

	/*
	 * If the whole devices holder is set to bd_may_claim, a partition on
	 * the device is claimed, but not the whole device.
	 */
	if (whole != bdev &&
// NVMe: 파티션 claim 시 전체 namespace 가 다른 holder 에 의해 claim 되었는지 확인.
	    whole->bd_holder && whole->bd_holder != bd_may_claim)
		return false;
	return true;
}

/*
 * bd_prepare_to_claim - exclusive claim 을 시도하고, 경쟁 시 대기.
 * 호출 경로: bdev_open(..., holder=..., ...) -> bd_prepare_to_claim.
 * NVMe 연결점: NVMe 캐릭터 디바이스나 블록 디바이스가 동시에 exclusive
 *            open 되는 것을 막아, NVMe 컨트롤러/namespace 상태 충돌을
 *            방지한다.
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
// NVMe: 파티션일 경우 전체 namespace 기준으로 claim 관리.

	if (WARN_ON_ONCE(!holder))
// NVMe: holder 없이 exclusive claim 시도는 버그.
		return -EINVAL;
retry:
	mutex_lock(&bdev_lock);
// NVMe: bdev_lock 획득. NVMe exclusive open 직렬화.
	/* if someone else claimed, fail */
	if (!bd_may_claim(bdev, holder, hops)) {
// NVMe: claim 가능 여부 검사.
		mutex_unlock(&bdev_lock);
		return -EBUSY;
// NVMe: 이미 다른 holder 가 있으면 -EBUSY. NVMe namespace 동시 exclusive 사용 불가.
	}

	/* if claiming is already in progress, wait for it to finish */
	if (whole->bd_claiming) {
// NVMe: 다른 프로세스가 claim 진행 중이면 대기.
		wait_queue_head_t *wq = __var_waitqueue(&whole->bd_claiming);
		DEFINE_WAIT(wait);

		prepare_to_wait(wq, &wait, TASK_UNINTERRUPTIBLE);
// NVMe: claim 완료 대기.
		mutex_unlock(&bdev_lock);
		schedule();
// NVMe: 스케줄링으로 대기.
		finish_wait(wq, &wait);
		goto retry;
// NVMe: claim 상태가 바뀌었으므로 재시도.
	}

	/* yay, all mine */
	whole->bd_claiming = holder;
// NVMe: 현재 프로세스가 claim 진행 중임을 표시.
	mutex_unlock(&bdev_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(bd_prepare_to_claim); /* only for the loop driver */

/*
 * bd_clear_claiming - claim 진행 중 플래그를 해제하고 대기자들을 깨운다.
 * lockdep_assert_held 으로 bdev_lock 보유를 검증.
 */

static void bd_clear_claiming(struct block_device *whole, void *holder)
{
	lockdep_assert_held(&bdev_lock);
// NVMe: bdev_lock 보유 검증.
	/* tell others that we're done */
	BUG_ON(whole->bd_claiming != holder);
// NVMe: claim holder 불일치는 치명적 버그.
	whole->bd_claiming = NULL;
// NVMe: claim 진행 종료 표시.
	wake_up_var(&whole->bd_claiming);
// NVMe: claim 대기자 깨움.
}

/*
 * bd_finish_claiming - exclusive claim 을 완료하여 holder 를 등록.
 * NVMe 연결점: claim 이 완료되면 해당 NVMe bdev 에 대한 독점적 IO
 *            (예: 포맷, secure erase 준비)가 허용된다.
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
// NVMe: 파티션/전체 구분.

	mutex_lock(&bdev_lock);
// NVMe: bdev_lock 획득.
	BUG_ON(!bd_may_claim(bdev, holder, hops));
	/*
	 * Note that for a whole device bd_holders will be incremented twice,
	 * and bd_holder will be set to bd_may_claim before being set to holder
	 */
	whole->bd_holders++;
// NVMe: 전체 namespace 의 holder 카운트 증가.
	whole->bd_holder = bd_may_claim;
// NVMe: 임시 holder 표시.
	bdev->bd_holders++;
// NVMe: 파티션/전체 디바이스 holder 카운트 증가.
	mutex_lock(&bdev->bd_holder_lock);
// NVMe: holder ops 보호.
	bdev->bd_holder = holder;
// NVMe: 실제 exclusive holder 등록.
	bdev->bd_holder_ops = hops;
// NVMe: holder 연산 등록.
	mutex_unlock(&bdev->bd_holder_lock);
	bd_clear_claiming(whole, holder);
	mutex_unlock(&bdev_lock);
}

/*
 * bd_abort_claiming - exclusive claim 을 중단. truncate_bdev_range 등이
 *                    일시적으로 exclusive 상태를 필요로 할 때 사용.
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
// NVMe: bdev_lock 획득.
	bd_clear_claiming(bdev_whole(bdev), holder);
// NVMe: claim 진행 중 플래그 해제.
	mutex_unlock(&bdev_lock);
}
EXPORT_SYMBOL(bd_abort_claiming);

/*
 * bd_end_claim - holder 가 더 이상 bdev 를 사용하지 않을 때 claim 해제.
 * NVMe 연결점: exclusive open 된 NVMe 디바이스가 닫히면 이 함수를 통해
 *            다른 프로세스가 해당 namespace 를 claim 할 수 있게 된다.
 */

static void bd_end_claim(struct block_device *bdev, void *holder)
{
	struct block_device *whole = bdev_whole(bdev);
// NVMe: 파티션인 경우 전체 namespace 참조.
	bool unblock = false;

	/*
	 * Release a claim on the device.  The holder fields are protected with
	 * bdev_lock.  open_mutex is used to synchronize disk_holder unlinking.
	 */
	mutex_lock(&bdev_lock);
// NVMe: bdev_lock 획득.
	WARN_ON_ONCE(bdev->bd_holder != holder);
// NVMe: holder 불일치 경고.
	WARN_ON_ONCE(--bdev->bd_holders < 0);
// NVMe: holder 카운트 언더플로우 방지.
	WARN_ON_ONCE(--whole->bd_holders < 0);
	if (!bdev->bd_holders) {
// NVMe: 마지막 holder 이면 ops 해제.
		mutex_lock(&bdev->bd_holder_lock);
// NVMe: holder ops 보호.
		bdev->bd_holder = NULL;
// NVMe: holder 해제.
		bdev->bd_holder_ops = NULL;
		mutex_unlock(&bdev->bd_holder_lock);
		if (bdev_test_flag(bdev, BD_WRITE_HOLDER))
// NVMe: write holder 였으면 이벤트 차단 해제가 필요.
			unblock = true;
	}
	if (!whole->bd_holders)
// NVMe: 전체 namespace 의 마지막 holder 이면 전체 holder 해제.
		whole->bd_holder = NULL;
	mutex_unlock(&bdev_lock);

	/*
	 * If this was the last claim, remove holder link and unblock evpoll if
	 * it was a write holder.
	 */
	if (unblock) {
		disk_unblock_events(bdev->bd_disk);
// NVMe: 디스크 이벤트(미디어 변경 등) 차단 해제.
		bdev_clear_flag(bdev, BD_WRITE_HOLDER);
// NVMe: BD_WRITE_HOLDER 플래그 클리어.
	}
}

/*
 * blkdev_flush_mapping - bdev 닫기 전 캐시 플러시와 inode writeback.
 * NVMe 연결점: close() -> blkdev_put_whole -> blkdev_flush_mapping ->
 *            sync_blockdev -> NVMe Flush/Write 명령 완료 대기.
 */

static void blkdev_flush_mapping(struct block_device *bdev)
{
	WARN_ON_ONCE(bdev->bd_holders);
// NVMe: holder 가 남아 있으면 flush 는 안전하지 않음.
	sync_blockdev(bdev);
// NVMe: NVMe Flush/Write 명령으로 dirty 데이터 동기화.
	kill_bdev(bdev);
// NVMe: 캐시 제거로 NVMe IO 완료 후 stale 데이터 노출 방지.
	bdev_write_inode(bdev);
// NVMe: inode 메타데이터 writeback.
}

/*
 * blkdev_put_whole - 전체 디바이스의 opener 카운트를 감소시키고,
 *                   마지막 opener 면 캐시를 비우고 ->release() 호출.
 * NVMe 연결점: NVMe 디바이스 드라이버의 release 콜백에서 SQ/CQ 자원을
 *            정리할 수 있다.
 */

static void blkdev_put_whole(struct block_device *bdev)
{
	if (atomic_dec_and_test(&bdev->bd_openers))
// NVMe: 마지막 opener 이면 flush 수행.
		blkdev_flush_mapping(bdev);
// NVMe: dirty 페이지를 NVMe 에 기록하고 캐시 제거.
	if (bdev->bd_disk->fops->release)
// NVMe: NVMe 드라이버 release 콜백이 있으면 호출.
		bdev->bd_disk->fops->release(bdev->bd_disk);
// NVMe: release 콜백에서 SQ/CQ/PRP pool 등 NVMe 자원 정리 가능.
}

/*
 * blkdev_get_whole - 전체 NVMe bdev 를 오픈. 디스크 ->open() 콜백 호출.
 *
 * @disk: NVMe namespace gendisk.
 * @mode: BLK_OPEN_READ/WRITE 등.
 *
 * NVMe 연결점: 첫 opener 일 때 set_init_blocksize() 로 블록 크기를 설정.
 *            GD_NEED_PART_SCAN 비트가 있으면 파티션 테이블을 재스캔.
 */

static int blkdev_get_whole(struct block_device *bdev, blk_mode_t mode)
{
	struct gendisk *disk = bdev->bd_disk;
	int ret;

	if (disk->fops->open) {
// NVMe: NVMe 드라이버 open 콜백.
		ret = disk->fops->open(disk, mode);
// NVMe: 컨트롤러 초기화 및 request_queue 준비.
		if (ret) {
			/* avoid ghost partitions on a removed medium */
			if (ret == -ENOMEDIUM &&
// NVMe: 미디어 없음(예: 외장 NVMe 분리).
			     test_bit(GD_NEED_PART_SCAN, &disk->state))
// NVMe: 파티션 스캔 필요 비트.
				bdev_disk_changed(disk, true);
// NVMe: 디스크 변경 처리(ghost partition 제거).
			return ret;
		}
	}

	if (!atomic_read(&bdev->bd_openers)) 	/* NVMe: 첫 open 일 때만 set_init_blocksize() 로 LBA 정렬 단위 설정. */
// NVMe: 첫 open 일 때만 LBA 정렬 단위 설정.
		set_init_blocksize(bdev);
	atomic_inc(&bdev->bd_openers);
// NVMe: opener 원자적 증가.
	if (test_bit(GD_NEED_PART_SCAN, &disk->state)) {
// NVMe: 파티션 테이블 재스캔 필요 여부.
		/*
		 * Only return scanning errors if we are called from contexts
		 * that explicitly want them, e.g. the BLKRRPART ioctl.
		 */
		ret = bdev_disk_changed(disk, false);
// NVMe: 파티션 테이블 재스캔.
		if (ret && (mode & BLK_OPEN_STRICT_SCAN)) {
// NVMe: STRICT_SCAN 모드에서 스캔 실패 시 open 실패.
			blkdev_put_whole(bdev);
			return ret;
		}
	}
	return 0;
}

/*
 * blkdev_get_part - NVMe 파티션 bdev 를 오픈. 우선 전체 디바이스를 오픈.
 * NVMe 연결점: /dev/nvme0n1p1 열기 시, 실제 NVMe IO 는 전체 namespace 의
 *            request_queue 를 공유하지만, bdev 의 시작 섹터/크기로 LBA
 *            범위가 제한된다.
 */

static int blkdev_get_part(struct block_device *part, blk_mode_t mode)
{
	struct gendisk *disk = part->bd_disk;
	int ret;

	ret = blkdev_get_whole(bdev_whole(part), mode);
// NVMe: /dev/nvme0n1p1 열기 시 전체 namespace 먼저 open.
	if (ret)
		return ret;

	ret = -ENXIO;
// NVMe: 파티션 크기가 0이면 invalid.
	if (!bdev_nr_sectors(part))
// NVMe: 파티션의 섹터 수가 0이면 NVMe IO 불가.
		goto out_blkdev_put;

	if (!atomic_read(&part->bd_openers)) {
// NVMe: 첫 파티션 open.
		disk->open_partitions++;
		set_init_blocksize(part);
// NVMe: 파티션 블록 크기 설정.
	}
	atomic_inc(&part->bd_openers);
// NVMe: 파티션 opener 카운트 증가.
	return 0;

out_blkdev_put:
	blkdev_put_whole(bdev_whole(part));
// NVMe: 파티션 open 실패 시 전체 namespace 닫기.
	return ret;
}

/*
 * bdev_permission - cgroup 및 exclusive 제약 조건을 검사.
 * NVMe 연결점: 컨테이너 환경에서 /dev/nvme0n1 접근이 허용되는지
 *            devcgroup 정책으로 제어.
 */

int bdev_permission(dev_t dev, blk_mode_t mode, void *holder)
{
	int ret;

	ret = devcgroup_check_permission(DEVCG_DEV_BLOCK,
// NVMe: cgroup 장치 접근 제어.
			MAJOR(dev), MINOR(dev),
// NVMe: major(예: 259) / minor.
			((mode & BLK_OPEN_READ) ? DEVCG_ACC_READ : 0) |
// NVMe: 읽기 권한 매핑.
			((mode & BLK_OPEN_WRITE) ? DEVCG_ACC_WRITE : 0));
// NVMe: 쓰기 권한 매핑.
	if (ret)
		return ret;

	/* Blocking writes requires exclusive opener */
	if (mode & BLK_OPEN_RESTRICT_WRITES && !holder)
// NVMe: restrict writes 모드는 exclusive holder 필요.
		return -EINVAL;

	/*
	 * We're using error pointers to indicate to ->release() when we
	 * failed to open that block device. Also this doesn't make sense.
	 */
	if (WARN_ON_ONCE(IS_ERR(holder)))
// NVMe: error pointer 를 holder 로 사용하는 것은 금지.
		return -EINVAL;

	return 0;
}

/*
 * blkdev_put_part - NVMe 파티션 bdev 를 닫음. 캐시 플러시 후
 *                  open_partitions 카운트 감소, 전체 디바이스 닫기로 이어짐.
 */

static void blkdev_put_part(struct block_device *part)
{
	struct block_device *whole = bdev_whole(part);

	if (atomic_dec_and_test(&part->bd_openers)) {
// NVMe: 마지막 파티션 opener.
		blkdev_flush_mapping(part);
// NVMe: 파티션 캐시 flush.
		whole->bd_disk->open_partitions--;
// NVMe: open 중인 파티션 수 감소.
	}
	blkdev_put_whole(whole);
}

/*
 * blkdev_get_no_open - dev_t 로 이미 존재하는 bdev 를 찾아 참조 획득.
 * NVMe 연결점: /dev/nvme0n1 가 이미 커널에 등록되어 있다면 ilookup 으로
 *            찾고, kobject_get_unless_zero 로 참조를 증가. 존재하지 않으면
 *            autoload 가능 시 모듈 로드 시도.
 */

struct block_device *blkdev_get_no_open(dev_t dev, bool autoload)
{
	struct block_device *bdev;
	struct inode *inode;

	inode = ilookup(blockdev_superblock, dev);
// NVMe: dev_t 로 기존 bdev inode 검색.
	if (!inode && autoload && IS_ENABLED(CONFIG_BLOCK_LEGACY_AUTOLOAD)) {
// NVMe: 존재하지 않으면 레거시 모듈 autoload 시도.
		blk_request_module(dev);
// NVMe: NVMe 모듈 로드 요청.
		inode = ilookup(blockdev_superblock, dev);
		if (inode)
			pr_warn_ratelimited(
"block device autoloading is deprecated and will be removed.\n");
	}
	if (!inode)
		return NULL;

	/* switch from the inode reference to a device mode one: */
	bdev = &BDEV_I(inode)->bdev;
	if (!kobject_get_unless_zero(&bdev->bd_device.kobj))
// NVMe: kobject 참조가 0이면 제거 중인 namespace 이므로 skip.
		bdev = NULL;
	iput(inode);
// NVMe: ilookup 로 획득한 inode 참조 해제.
	return bdev;
}

/* blkdev_get_no_open() 에서 획득한 bdev 의 device 참조를 해제. */

void blkdev_put_no_open(struct block_device *bdev)
{
	put_device(&bdev->bd_device);
}

/*
 * bdev_writes_blocked - bdev 에 대한 쓰기가 현재 차단되었는지 확인.
 * NVMe 연결점: NVMe namespace 가 read-only 로 마운트되었거나 restrict
 *            writes 모드일 때 true.
 */

static bool bdev_writes_blocked(struct block_device *bdev)
{
	return bdev->bd_writers < 0;
// NVMe: bd_writers < 0 이면 쓰기가 차단된 상태(read-only 또는 restrict).
}

/*
 * bdev_block_writes - 공유 쓰기 카운터를 감소시켜 쓰기 차단.
 * NVMe 연결점: restrict writes 모드에서 추가 공유 쓰기 open 을 막음.
 */

static void bdev_block_writes(struct block_device *bdev)
{
	bdev->bd_writers--;
// NVMe: 공유 쓰기 차단. 추가 NVMe 쓰기 open 방지.
}

/* bdev_block_writes() 의 역연산. 쓰기 차단을 해제. */

static void bdev_unblock_writes(struct block_device *bdev)
{
	bdev->bd_writers++;
// NVMe: 쓰기 차단 해제.
}

/*
 * bdev_may_open - 현재 쓰기 상태와 open 모드를 비교해 open 허용 여부.
 * NVMe 연결점: BLK_OPEN_WRITE 요청이 들어올 때 NVMe SSD 의 read-only
 *            상태나 마운트 제약을 검사.
 */

static bool bdev_may_open(struct block_device *bdev, blk_mode_t mode)
{
	if (bdev_allow_write_mounted)
// NVMe: 커널 파라미터로 마운트된 bdev 쓰기 허용 시 모든 open 허용.
		return true;
	/* Writes blocked? */
	if (mode & BLK_OPEN_WRITE && bdev_writes_blocked(bdev))
// NVMe: 쓰기 요청인데 차단 상태면 NVMe SQ 로 쓰기가 흐를 수 없음.
		return false;
	if (mode & BLK_OPEN_RESTRICT_WRITES && bdev->bd_writers > 0)
// NVMe: restrict writes 와 공유 쓰기가 충돌.
		return false;
	return true;
}

/*
 * bdev_claim_write_access - 쓰기 open 시 카운터를 조정.
 * NVMe 연결점: 쓰기 모드 open 카운트가 정확하면 NVMe 플러시/캐시 정책이
 *            안정적으로 동작.
 */

static void bdev_claim_write_access(struct block_device *bdev, blk_mode_t mode)
{
	if (bdev_allow_write_mounted)
// NVMe: bdev_allow_write_mounted 이면 추가 카운팅 불필요.
		return;

	/* Claim exclusive or shared write access. */
	if (mode & BLK_OPEN_RESTRICT_WRITES)
// NVMe: exclusive 쓰기 차단 모드.
		bdev_block_writes(bdev);
// NVMe: 공유 쓰기를 차단하여 NVMe 쓰기 open 직렬화.
	else if (mode & BLK_OPEN_WRITE)
// NVMe: 일반 쓰기 모드.
		bdev->bd_writers++;
// NVMe: 공유 쓰기 카운트 증가.
}

/*
 * bdev_unclaimed - file->private_data 가 bdev_inode 를 가리키면
 *                 holder claim 이 없는 상태(일반 open).
 */

static inline bool bdev_unclaimed(const struct file *bdev_file)
{
	return bdev_file->private_data == BDEV_I(bdev_file->f_mapping->host);
// NVMe: private_data 가 bdev_inode 이면 holder claim 이 없는 일반 open.
}

/*
 * bdev_yield_write_access - 파일 닫기 시 쓰기 접근 권한을 반납.
 * NVMe 연결점: close() 시점에 쓰기 카운터를 원상복구하여, 추후 다른
 *            NVMe 쓰기 open 이 정상적으로 가능하게 함.
 */

static void bdev_yield_write_access(struct file *bdev_file)
{
	struct block_device *bdev;

	if (bdev_allow_write_mounted)
// NVMe: bdev_allow_write_mounted 이면 반납 불필요.
		return;

	if (bdev_unclaimed(bdev_file))
// NVMe: holder claim 없는 일반 open 이면 skip.
		return;

	bdev = file_bdev(bdev_file);

	if (bdev_file->f_mode & FMODE_WRITE_RESTRICTED)
// NVMe: restricted write 모드 해제.
		bdev_unblock_writes(bdev);
// NVMe: 쓰기 차단 카운트 감소.
	else if (bdev_file->f_mode & FMODE_WRITE)
// NVMe: 일반 쓰기 모드.
		bdev->bd_writers--;
// NVMe: 공유 쓰기 카운트 감소.
}

/*
 * bdev_open - block_device 를 열고 필요시 exclusive claim 을 설정.
 *
 * @bdev: 대상 NVMe namespace 또는 파티션의 block_device.
 * @mode: BLK_OPEN_READ/WRITE/EXCL 등.
 * @holder: exclusive claim 을 원하는 주체. NULL 이면 일반 open.
 * @bdev_file: 생성될 struct file.
 *
 * 호출 경로: bdev_file_open_by_dev -> bdev_open
 *           또는 blkdev_open -> bdev_file_open_by_path -> bdev_open.
 *
 * NVMe 연결점:
 *  - disk->open_mutex 아래에서 disk_live 와 module 참조를 검증.
 *  - blkdev_get_whole/part 를 통해 NVMe 디바이스의 open 카운터 증가.
 *  - bdev_claim_write_access 로 쓰기 권한 기록.
 *  - bdev->bd_mapping 이 bdev_file->f_mapping 에 연결되어, 이후 bio 의
 *    대상 address_space 가 된다.
 *  - bdev_nowait() 가 true 면 FMODE_NOWAIT 설정, NVMe poll queue 사용
 *    가능성을 열어준다(추정).
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
// NVMe: 이벤트 차단 해제 여부.
	struct gendisk *disk = bdev->bd_disk;
// NVMe: NVMe namespace 를 표현하는 gendisk.
	int ret;

	if (holder) {
// NVMe: exclusive open 요청.
		mode |= BLK_OPEN_EXCL;
// NVMe: BLK_OPEN_EXCL 강제 설정.
		ret = bd_prepare_to_claim(bdev, holder, hops);
// NVMe: exclusive claim 시도.
		if (ret)
			return ret;
	} else {
		if (WARN_ON_ONCE(mode & BLK_OPEN_EXCL))
			return -EIO;
	}

	disk_block_events(disk); 	/* NVMe: open 중 디스크 이벤트(미디어 변경 등)를 일시 차단. */
// NVMe: open 중 디스크 이벤트(미디어 변경) 차단.

	mutex_lock(&disk->open_mutex);
// NVMe: 디스크 단위 open 직렬화.
	ret = -ENXIO;
	if (!disk_live(disk))
// NVMe: gendisk 가 아직 live(bdev_unhash 안 됨)인지 확인.
		goto abort_claiming;
// NVMe: 제거된 namespace 이면 open 실패.
	if (!try_module_get(disk->fops->owner))
// NVMe: NVMe 모듈 참조 증가.
		goto abort_claiming;
// NVMe: 모듈 언로드 방지.
	ret = -EBUSY;
	if (!bdev_may_open(bdev, mode))
// NVMe: read-only, restrict writes 등 상태 검사.
		goto put_module;
	if (bdev_is_partition(bdev))
// NVMe: 파티션/전체 디바이스 구분.
		ret = blkdev_get_part(bdev, mode);
	else
		ret = blkdev_get_whole(bdev, mode);
// NVMe: 전체 namespace open.
	if (ret)
// NVMe: open 실패 시 module_put 으로 이동.
		goto put_module;
	bdev_claim_write_access(bdev, mode);
// NVMe: 쓰기 접근 권한 기록.
	if (holder) {
		bd_finish_claiming(bdev, holder, hops);
// NVMe: exclusive claim 완료.

		/*
		 * Block event polling for write claims if requested.  Any write
		 * holder makes the write_holder state stick until all are
		 * released.  This is good enough and tracking individual
		 * writeable reference is too fragile given the way @mode is
		 * used in blkdev_get/put().
		 */
		if ((mode & BLK_OPEN_WRITE) &&
// NVMe: write holder 이면서.
		    !bdev_test_flag(bdev, BD_WRITE_HOLDER) &&
		    (disk->event_flags & DISK_EVENT_FLAG_BLOCK_ON_EXCL_WRITE)) {
// NVMe: 디스크가 exclusive write 시 이벤트 차단을 요청.
			bdev_set_flag(bdev, BD_WRITE_HOLDER);
// NVMe: BD_WRITE_HOLDER 플래그 설정.
			unblock_events = false;
// NVMe: 이벤트 차단 유지.
		}
	}
	mutex_unlock(&disk->open_mutex);

	if (unblock_events)
// NVMe: 이벤트 차단 해제.
		disk_unblock_events(disk);
// NVMe: 디스크 이벤트 폴링 재개.

	bdev_file->f_flags |= O_LARGEFILE;
	bdev_file->f_mode |= FMODE_CAN_ODIRECT;
	if (bdev_nowait(bdev)) 	/* NVMe: bdev_nowait() true 시 폴큐(poll queue) IO 가 가능(추정). */
// NVMe: poll queue 지원 시 FMODE_NOWAIT 설정(추정).
		bdev_file->f_mode |= FMODE_NOWAIT;
// NVMe: 이어서 NVMe poll queue(poller hctx) 경로 사용 가능.
	if (mode & BLK_OPEN_RESTRICT_WRITES)
		bdev_file->f_mode |= FMODE_WRITE_RESTRICTED;
	bdev_file->f_mapping = bdev->bd_mapping;
// NVMe: 파일의 address_space 를 bdev 페이지 캐시와 연결.
	bdev_file->f_wb_err = filemap_sample_wb_err(bdev_file->f_mapping);
// NVMe: writeback 에러 샘플. NVMe flush/write 오류 감지의 시작점.
	bdev_file->private_data = holder;
// NVMe: holder 정보 저장.

	return 0;
put_module:
	module_put(disk->fops->owner);
// NVMe: NVMe 모듈 참조 감소.
abort_claiming:
	if (holder)
// NVMe: open 실패 시 claim 중단.
		bd_abort_claiming(bdev, holder);
	mutex_unlock(&disk->open_mutex);
	disk_unblock_events(disk);
// NVMe: 이벤트 차단 해제.
	return ret;
}

/*
 * blk_to_file_flags - blk_mode_t 를 파일 open 플래그로 변환.
 * NVMe 연결점: 사용자가 O_RDWR 로 /dev/nvme0n1 를 연 것이
 *            BLK_OPEN_READ | BLK_OPEN_WRITE 로 변환됨.
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

	if ((mode & (BLK_OPEN_READ | BLK_OPEN_WRITE)) ==
// NVMe: O_RDWR 에 해당.
	    (BLK_OPEN_READ | BLK_OPEN_WRITE))
		flags |= O_RDWR;
	else if (mode & BLK_OPEN_WRITE_IOCTL)
		flags |= O_RDWR | O_WRONLY;
	else if (mode & BLK_OPEN_WRITE)
		flags |= O_WRONLY;
	else if (mode & BLK_OPEN_READ)
// NVMe: O_RDONLY 에 해당.
		flags |= O_RDONLY; /* homeopathic, because O_RDONLY is 0 */
	else
		WARN_ON_ONCE(true);

	if (mode & BLK_OPEN_NDELAY)
// NVMe: O_NONBLOCK 에 해당.
		flags |= O_NDELAY;

	return flags;
}

/*
 * bdev_file_open_by_dev - dev_t 로 block_device 를 찾아 struct file 생성.
 *
 * 호출 경로: lookup_bdev -> bdev_file_open_by_dev -> bdev_open.
 * NVMe 연결점: major/minor (예: 259:0) 로 NVMe bdev 를 찾아 사용자 공간
 *            파일 디스크립터와 연결. 이후 read/write/flush/fsync 가
 *            해당 bdev 를 경유하여 NVMe queue 로 전달.
 */

struct file *bdev_file_open_by_dev(dev_t dev, blk_mode_t mode, void *holder,
				   const struct blk_holder_ops *hops)
{
	struct file *bdev_file;
	struct block_device *bdev;
	unsigned int flags;
	int ret;

	ret = bdev_permission(dev, mode, holder);
// NVMe: cgroup 및 권한 검사.
	if (ret)
		return ERR_PTR(ret);

	bdev = blkdev_get_no_open(dev, true);
// NVMe: major/minor 로 이미 등록된 NVMe bdev 획득.
	if (!bdev)
		return ERR_PTR(-ENXIO);

	flags = blk_to_file_flags(mode);
	bdev_file = alloc_file_pseudo_noaccount(BD_INODE(bdev),
// NVMe: bdev 에 대한 struct file 할당.
			blockdev_mnt, "", flags | O_LARGEFILE, &def_blk_fops);
// NVMe: bdev 파일 연산(def_blk_fops) 연결.
	if (IS_ERR(bdev_file)) {
		blkdev_put_no_open(bdev);
		return bdev_file;
	}
	ihold(BD_INODE(bdev));
// NVMe: inode 참조 증가.

	ret = bdev_open(bdev, mode, holder, hops, bdev_file);
// NVMe: 실제 block_device open.
	if (ret) {
		/* We failed to open the block device. Let ->release() know. */
		bdev_file->private_data = ERR_PTR(ret);
// NVMe: open 실패 표시.
		fput(bdev_file);
// NVMe: 할당한 파일 해제.
		return ERR_PTR(ret);
	}
	return bdev_file;
}
EXPORT_SYMBOL(bdev_file_open_by_dev);

/*
 * bdev_file_open_by_path - 경로명(/dev/nvme0n1)으로 block_device 파일 생성.
 * NVMe 연결점: 사용자 공간의 open("/dev/nvme0n1") 이 커널에서 이 함수로
 *            매핑되어 NVMe SSD 와 연결.
 */

struct file *bdev_file_open_by_path(const char *path, blk_mode_t mode,
				    void *holder,
				    const struct blk_holder_ops *hops)
{
	struct file *file;
	dev_t dev;
	int error;

	error = lookup_bdev(path, &dev);
// NVMe: /dev/nvme0n1 경로 -> dev_t 변환.
	if (error)
		return ERR_PTR(error);

	file = bdev_file_open_by_dev(dev, mode, holder, hops);
	if (!IS_ERR(file) && (mode & BLK_OPEN_WRITE)) {
// NVMe: 쓰기 모드 open 이고.
		if (bdev_read_only(file_bdev(file))) {
// NVMe: NVMe namespace 가 read-only 이면 접근 거부.
			fput(file);
			file = ERR_PTR(-EACCES);
		}
	}

	return file;
}
EXPORT_SYMBOL(bdev_file_open_by_path);

/*
 * bd_yield_claim - 이미 잡힌 exclusive claim 을 해제(holder 종료 시).
 * open_mutex 아래에서 호출.
 */

static inline void bd_yield_claim(struct file *bdev_file)
{
	struct block_device *bdev = file_bdev(bdev_file);
// NVMe: 파일에서 bdev 획득.
	void *holder = bdev_file->private_data;
// NVMe: holder 정보.

	lockdep_assert_held(&bdev->bd_disk->open_mutex);
// NVMe: open_mutex 보유 검증.

	if (WARN_ON_ONCE(IS_ERR_OR_NULL(holder)))
		return;

	if (!bdev_unclaimed(bdev_file))
// NVMe: holder claim 이 있을 때만 해제.
		bd_end_claim(bdev, holder);
// NVMe: exclusive claim 해제.
}

/*
 * bdev_release - struct file 가 마지막 참조를 잃을 때 호출되는 release.
 *
 * 호출 경로: fput -> __fput -> bdev_release.
 *
 * NVMe 연결점:
 *  - 마지막 opener 면 sync_blockdev() 로 dirty 페이지를 NVMe 에 플러시.
 *  - bdev_yield_write_access, bd_yield_claim 로 권한/claim 정리.
 *  - disk_flush_events() 로 MEDIA_CHANGE 이벤트를 NVMe 드라이버에 전달.
 *  - blkdev_put_whole/part -> module_put 으로 NVMe 모듈 참조 해제.
 */

void bdev_release(struct file *bdev_file)
{
	struct block_device *bdev = file_bdev(bdev_file);
// NVMe: 파일에서 bdev 획득.
	void *holder = bdev_file->private_data;
// NVMe: holder 정보.
	struct gendisk *disk = bdev->bd_disk;
// NVMe: gendisk.

	/* We failed to open that block device. */
	if (IS_ERR(holder))
// NVMe: open 실패 시 private_data 는 ERR_PTR.
		goto put_no_open;

	/*
	 * Sync early if it looks like we're the last one.  If someone else
	 * opens the block device between now and the decrement of bd_openers
	 * then we did a sync that we didn't need to, but that's not the end
	 * of the world and we want to avoid long (could be several minute)
	 * syncs while holding the mutex.
	 */
	if (atomic_read(&bdev->bd_openers) == 1) 	/* NVMe: 마지막 opener 라면 NVMe 플러시 전 dirty 데이터 동기화. */
// NVMe: 마지막 opener 이면 NVMe flush 수행.
		sync_blockdev(bdev);
// NVMe: dirty 페이지를 NVMe Write/Flush 명령으로 동기화.

	mutex_lock(&disk->open_mutex);
// NVMe: open_mutex 획득.
	bdev_yield_write_access(bdev_file);
// NVMe: 쓰기 접근 권한 반납.

	if (holder)
// NVMe: exclusive claim 있으면 해제.
		bd_yield_claim(bdev_file);

	/*
	 * Trigger event checking and tell drivers to flush MEDIA_CHANGE
	 * event.  This is to ensure detection of media removal commanded
	 * from userland - e.g. eject(1).
	 */
	disk_flush_events(disk, DISK_EVENT_MEDIA_CHANGE); 	/* NVMe: 사용자 공간 eject(1) 같은 미디어 변경 이벤트를 드라이버에 전달. */
// NVMe: MEDIA_CHANGE 이벤트를 NVMe 드라이버에 전달(eject 등).

	if (bdev_is_partition(bdev))
// NVMe: 파티션 닫기.
		blkdev_put_part(bdev);
	else
		blkdev_put_whole(bdev);
// NVMe: 전체 namespace 닫기.
	mutex_unlock(&disk->open_mutex);

	module_put(disk->fops->owner);
// NVMe: NVMe 모듈 참조 감소.
put_no_open:
	blkdev_put_no_open(bdev);
// NVMe: bdev device 참조 해제.
}

/*
 * bdev_fput - claim 과 write access 를 먼저 반납한 뒤 fput 을 지연 실행.
 * NVMe 연결점: LVM/multipath 등에서 NVMe bdev 를 재claim 하기 전
 *            안전하게 기존 claim 을 해제할 때 사용.
 */

/**
 * bdev_fput - yield claim to the block device and put the file
 * @bdev_file: open block device
 *
 * Yield claim on the block device and put the file. Ensure that the
 * block device can be reclaimed before the file is closed which is a
 * deferred operation.
 */
void bdev_fput(struct file *bdev_file)
{
	if (WARN_ON_ONCE(bdev_file->f_op != &def_blk_fops))
// NVMe: def_blk_fops 파일이 아니면 경고.
		return;

	if (bdev_file->private_data) {
// NVMe: holder 가 있을 때만 claim 해제 처리.
		struct block_device *bdev = file_bdev(bdev_file);
		struct gendisk *disk = bdev->bd_disk;

		mutex_lock(&disk->open_mutex);
// NVMe: open_mutex 획득.
		bdev_yield_write_access(bdev_file);
		bd_yield_claim(bdev_file);
// NVMe: claim 해제.
		/*
		 * Tell release we already gave up our hold on the
		 * device and if write restrictions are available that
		 * we already gave up write access to the device.
		 */
		bdev_file->private_data = BDEV_I(bdev_file->f_mapping->host);
// NVMe: 일반 open 상태로 표시하여 release 시 중복 claim 해제 방지.
		mutex_unlock(&disk->open_mutex);
	}

	fput(bdev_file);
// NVMe: fput 지연 실행.
}
EXPORT_SYMBOL(bdev_fput);

/*
 * lookup_bdev - 경로명으로 block_device 의 dev_t 를 조회.
 * NVMe 연결점: /dev/nvme0n1 경로가 실제 블록 장치 노드인지 S_ISBLK 로
 *            확인하고 major/minor 를 반환.
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
int lookup_bdev(const char *pathname, dev_t *dev)
{
	struct inode *inode;
	struct path path;
	int error;

	if (!pathname || !*pathname)
// NVMe: 빈 경로 거부.
		return -EINVAL;

	error = kern_path(pathname, LOOKUP_FOLLOW, &path);
// NVMe: 경로 -> dentry 변환.
	if (error)
		return error;

	inode = d_backing_inode(path.dentry);
// NVMe: dentry 의 inode 획득.
	error = -ENOTBLK;
	if (!S_ISBLK(inode->i_mode))
// NVMe: 블록 장치 노드가 아니면 거부.
		goto out_path_put;
	error = -EACCES;
	if (!may_open_dev(&path))
// NVMe: 노드 접근 권한 검사.
		goto out_path_put;

	*dev = inode->i_rdev;
// NVMe: dev_t 획득.
	error = 0;
out_path_put:
	path_put(&path);
	return error;
}
EXPORT_SYMBOL(lookup_bdev);

/*
 * bdev_mark_dead - bdev 와 그 위의 파일시스템을 "죽은" 상태로 표시.
 *
 * @surprise: true 면 이미 미디어가 제거됨, false 면 정리 중.
 *
 * NVMe 연결점: NVMe SSD 가 surprise removal 되었을 때 호출. sync_blockdev
 *            로 남은 dirty 데이터를 최대한 기록하고 invalidate_bdev 로
 *            캐시를 무효화하여, 더 이상의 NVMe IO 가 stale 데이터를
 *            반환하지 않도록 한다.
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
void bdev_mark_dead(struct block_device *bdev, bool surprise)
{
	mutex_lock(&bdev->bd_holder_lock);
// NVMe: holder ops 보호.
	if (bdev->bd_holder_ops && bdev->bd_holder_ops->mark_dead)
// NVMe: NVMe surprise removal 콜백이 있으면 호출.
		bdev->bd_holder_ops->mark_dead(bdev, surprise);
	else {
		mutex_unlock(&bdev->bd_holder_lock);
		sync_blockdev(bdev);
// NVMe: 남은 dirty 데이터를 NVMe Flush/Write 로 최대한 기록.
	}

	invalidate_bdev(bdev);
// NVMe: 캐시 무효화로 stale 데이터 노출 방지.
}
/*
 * New drivers should not use this directly.  There are some drivers however
 * that needs this for historical reasons. For example, the DASD driver has
 * historically had a shutdown to offline mode that doesn't actually remove the
 * gendisk that otherwise looks a lot like a safe device removal.
 */
EXPORT_SYMBOL_GPL(bdev_mark_dead);

/*
 * sync_bdevs - 등록된 모든 block_device 의 dirty 데이터를 플러시.
 *
 * @wait: true 면 완료 대기, false 면 시작만.
 *
 * NVMe 연결점: 시스템 종료나 sync(1) 호출 시 모든 NVMe namespace 의
 *            dirty 페이지를 NVMe Write/Flush 명령으로 날림. 이 리스트
 *            순회는 blockdev_superblock 의 inode list 를 사용.
 */

void sync_bdevs(bool wait)
{
	struct inode *inode, *old_inode = NULL;

	spin_lock(&blockdev_superblock->s_inode_list_lock);
// NVMe: bdev inode 리스트 보호.
	list_for_each_entry(inode, &blockdev_superblock->s_inodes, i_sb_list) { 	/* NVMe: blockdev_superblock 의 모든 bdev inode 를 순회하며 플러시. */
// NVMe: blockdev_superblock 에 등록된 모든 NVMe bdev inode 순회.
		struct address_space *mapping = inode->i_mapping;
		struct block_device *bdev;

		spin_lock(&inode->i_lock);
// NVMe: inode 상태 보호.
		if (inode_state_read(inode) & (I_FREEING | I_WILL_FREE | I_NEW) ||
// NVMe: 해제 중이거나 새 inode, 캐시 페이지 없으면 skip.
		    mapping->nrpages == 0) {
// NVMe: 캐시 페이지가 없으면 NVMe flush 대상 없음.
			spin_unlock(&inode->i_lock);
			continue;
		}
		__iget(inode);
// NVMe: inode 참조 증가.
		spin_unlock(&inode->i_lock);
		spin_unlock(&blockdev_superblock->s_inode_list_lock);
		/*
		 * We hold a reference to 'inode' so it couldn't have been
		 * removed from s_inodes list while we dropped the
		 * s_inode_list_lock  We cannot iput the inode now as we can
		 * be holding the last reference and we cannot iput it under
		 * s_inode_list_lock. So we keep the reference and iput it
		 * later.
		 */
		iput(old_inode);
// NVMe: 이전 inode 참조 해제.
		old_inode = inode;
		bdev = I_BDEV(inode);
// NVMe: inode -> bdev 변환.

		mutex_lock(&bdev->bd_disk->open_mutex);
// NVMe: open_mutex 획득.
		if (!atomic_read(&bdev->bd_openers)) {
// NVMe: opener 가 없으면 flush 대상이 아님.
			; /* skip */
		} else if (wait) {
			/*
			 * We keep the error status of individual mapping so
			 * that applications can catch the writeback error using
			 * fsync(2). See filemap_fdatawait_keep_errors() for
			 * details.
			 */
			filemap_fdatawait_keep_errors(inode->i_mapping);
// NVMe: writeback 완료 대기. NVMe CQ 완료까지 대기하는 것에 대응.
		} else {
			filemap_fdatawrite(inode->i_mapping);
// NVMe: writeback 시작. NVMe SQ 에 Write 명령 삽입.
		}
		mutex_unlock(&bdev->bd_disk->open_mutex);
// NVMe: open_mutex 해제.

		spin_lock(&blockdev_superblock->s_inode_list_lock);
// NVMe: 다음 순회를 위해 리스트 lock 재획득.
	}
	spin_unlock(&blockdev_superblock->s_inode_list_lock);
	iput(old_inode);
// NVMe: 마지막 inode 참조 해제.
}

/*
 * bdev_statx - block_device 의 STATX_DIOALIGN/WRITE_ATOMIC 정보 제공.
 *
 * NVMe 연결점:
 *  - dio_offset_align = bdev_logical_block_size: NVMe LBA 정렬 단위.
 *  - STATX_WRITE_ATOMIC 지원 시 queue_atomic_write_unit_* 값 반환.
 *    이는 NVMe atomic write 단위(제조사별, 선택적 기능)와 관련.
 */

/*
 * Handle STATX_{DIOALIGN, WRITE_ATOMIC} for block devices.
 */
void bdev_statx(const struct path *path, struct kstat *stat, u32 request_mask)
{
	struct block_device *bdev;

	/*
	 * Note that d_backing_inode() returns the block device node inode, not
	 * the block device's internal inode.  Therefore it is *not* valid to
	 * use I_BDEV() here; the block device has to be looked up by i_rdev
	 * instead.
	 */
	bdev = blkdev_get_no_open(d_backing_inode(path->dentry)->i_rdev, false);
// NVMe: dev_t 로 bdev 획득.
	if (!bdev)
		return;

	if (request_mask & STATX_DIOALIGN) {
// NVMe: STATX_DIOALIGN 요청.
		stat->dio_mem_align = bdev_dma_alignment(bdev) + 1;
// NVMe: NVMe DMA/PRP 정렬 요구사항.
		stat->dio_offset_align = bdev_logical_block_size(bdev);
// NVMe: NVMe LBA offset 정렬 단위.
		stat->result_mask |= STATX_DIOALIGN;
	}

	if (request_mask & STATX_WRITE_ATOMIC && bdev_can_atomic_write(bdev)) {
// NVMe: NVMe atomic write 기능 지원 여부.
		struct request_queue *bd_queue = bdev->bd_queue;
// NVMe: NVMe request_queue 획득.

		generic_fill_statx_atomic_writes(stat,
// NVMe: NVMe atomic write 최소 단위(제조사별).
			queue_atomic_write_unit_min_bytes(bd_queue),
// NVMe: NVMe atomic write 최대 단위.
			queue_atomic_write_unit_max_bytes(bd_queue),
			0);
	}

	stat->blksize = bdev_io_min(bdev);
// NVMe: NVMe 최소 IO 크기(logical_block_size 기반).

	blkdev_put_no_open(bdev);
}

/*
 * disk_live - gendisk 가 아직 해제되지 않았는지((inode 가 hash 에 있는지) 확인.
 * NVMe 연결점: NVMe namespace 가 제거되어 bdev_unhash 되면 false 가 되어
 *            새로운 open 실패.
 */

bool disk_live(struct gendisk *disk)
{
	return !inode_unhashed(BD_INODE(disk->part0));
// NVMe: gendisk 가 bdev_unhash 되어 제거되었는지 확인. false 면 새 open 실패.
}
EXPORT_SYMBOL_GPL(disk_live);

/*
 * block_size - bdev 의 논리 블록 크기를 바이트로 반환.
 * NVMe 연결점: NVMe namespace 의 logical_block_size(예: 512 또는 4096).
 */

unsigned int block_size(struct block_device *bdev)
{
	return 1 << BD_INODE(bdev)->i_blkbits;
// NVMe: NVMe namespace 의 logical_block_size 반환.
}
EXPORT_SYMBOL_GPL(block_size);

/*
 * setup_bdev_allow_write_mounted - 커널 파라미터로 마운트된 bdev 쓰기 허용.
 * NVMe 연결점: 테스트 목적으로 /dev/nvme0n1 를 마운트 상태에서도 raw 쓰기
 *            를 허용할지 결정.
 */

static int __init setup_bdev_allow_write_mounted(char *str)
{
	if (kstrtobool(str, &bdev_allow_write_mounted))
// NVMe: 커널 부팅 파라미터로 마운트된 NVMe namespace raw 쓰기 허용 여부 설정.
		pr_warn("Invalid option string for bdev_allow_write_mounted:"
			" '%s'\n", str);
	return 1;
}
__setup("bdev_allow_write_mounted=", setup_bdev_allow_write_mounted);

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

