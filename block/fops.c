// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 1991, 1992  Linus Torvalds
 * Copyright (C) 2001  Andrea Arcangeli <andrea@suse.de> SuSE
 * Copyright (C) 2016 - 2020 Christoph Hellwig
 */
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/blkdev.h>
#include <linux/blk-integrity.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/uio.h>
#include <linux/namei.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/falloc.h>
#include <linux/suspend.h>
#include <linux/fs.h>
#include <linux/iomap.h>
#include <linux/module.h>
#include <linux/io_uring/cmd.h>
#include "blk.h"
/*
 * NVMe 관점 파일 요약:
 *  - block/fops.c는 VFS의 struct file_operations(def_blk_fops)와
 *    block layer 사이의 블록 장치 파일 연산을 담당한다.
 *  - 응용의 read/write/fsync/fallocate/mmap 등을 bio로 변환하여
 *    submit_bio -> blk_mq_submit_bio -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *    경로로 NVMe 컨트롤러의 SQ/CQ에 도달하게 한다.
 *  - direct I/O, buffered I/O, readahead, flush, write-zeroes 등을
 *    처리하며, block/blk-core.c, block/mq.c, block/bdev.c,
 *    drivers/nvme/host/pci.c 등과 논리적으로 연결된다.
 *  - 본 파일은 NVMe 명령을 직접 만들지 않고, bio 형태로 요청을
 *    조립하여 하위 block layer와 NVMe 드라이버에 넘긴다.
 */

/*
 * bdev_file_inode - struct file에서 block device inode 추출
 *
 * NVMe namespace가 마운트/오픈될 때 생성된 bdev inode를 반환한다.
 */
static inline struct inode *bdev_file_inode(struct file *file)
{
	return file->f_mapping->host; /* block 장치 inode 획득; NVMe bdev의 inode */
}

/*
 * dio_bio_write_op - direct write용 bio opf 플래그 조합
 *
 * REQ_SYNC와 REQ_IDLE을 설정해 NVMe I/O가 낮은 지연으로 처리되도록
 * 힌트를 준다. IOCB_DSYNC가 설정되면 REQ_FUA를 추가하여, NVMe
 * Write 명령의 FUA 비트를 통해 휘발성 캐시를 강제로 플러시한다.
 *
 * 호출 경로:
 *   blkdev_direct_IO -> __blkdev_direct_IO[_simple/_async]
 *   -> bio_init/bio_alloc with opf -> submit_bio
 *   -> blk_mq_submit_bio -> nvme_queue_rq -> nvme_setup_cmd
 */
static blk_opf_t dio_bio_write_op(struct kiocb *iocb)
{
	blk_opf_t opf = REQ_OP_WRITE | REQ_SYNC | REQ_IDLE; /* NVMe Write, sync/idle 힌트 */

	/* avoid the need for a I/O completion work item */
	if (iocb_is_dsync(iocb))
		opf |= REQ_FUA; /* IOCB_DSYNC 시 NVMe FUA 플래그 추가 */
	return opf;
}

/*
 * blkdev_dio_invalid - 사용자 요청의 논리 블록 정렬 검사
 *
 * NVMe는 lba_shift(보통 512B 또는 4KiB) 단위로 LBA를 처리하므로,
 * 시작 offset과 길이가 bdev_logical_block_size로 정렬되어 있어야 한다.
 * 정렬 실패 시 NVMe 컨트롤러가 IOERR/NOTSUPP로 거절하기 전에 커널에서
 * 사전 차단한다.
 */
static bool blkdev_dio_invalid(struct block_device *bdev, struct kiocb *iocb,
				struct iov_iter *iter)
{
	return (iocb->ki_pos | iov_iter_count(iter)) & /* offset|len이 LBA 마스크와 겹치면 미정렬 */
			(bdev_logical_block_size(bdev) - 1);
}

/*
 * blkdev_iov_iter_get_pages - 사용자 버퍼의 물리 페이지를 bio에 연결
 *
 * bio_iov_iter_get_pages는 NVMe PRP/SGL 생성에 필요한 물리
 * 연속/불연속 페이지 리스트를 구성한다. NVMe 드라이버는 이 bio의
 * bvec을 기반으로 PRP list 또는 SGL을 빌드한다.
 */
static inline int blkdev_iov_iter_get_pages(struct bio *bio,
		struct iov_iter *iter, struct block_device *bdev)
{
	return bio_iov_iter_get_pages(bio, iter, /* NVMe SGL/PRP용 물리 페이지 목록 구성 */
			bdev_logical_block_size(bdev) - 1);
}

#define DIO_INLINE_BIO_VECS 4 /* 작은 I/O용 스택 내 bio_vec; NVMe SGL 엔트리 수와 무관 */

/*
 * __blkdev_direct_IO_simple - 단일 bio로 처리하는 동기 direct I/O
 *
 * 목적: 요청이 작아 단일 bio로 표현 가능할 때 사용한다.
 * 호출 경로:
 *   blkdev_direct_IO -> __blkdev_direct_IO_simple
 *   -> submit_bio_wait -> submit_bio -> blk_mq_submit_bio
 *   -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 * NVMe 연결: bio.bi_sector가 NVMe 명령의 SLBA로 변환되고,
 *            bio의 bvec은 PRP/SGL 엔트리로 변환된다.
 *            완료 시 NVMe CQ -> bio_endio -> submit_bio_wait가 깨어난다.
 */
static ssize_t __blkdev_direct_IO_simple(struct kiocb *iocb,
		struct iov_iter *iter, struct block_device *bdev,
		unsigned int nr_pages)
{
	struct bio_vec inline_vecs[DIO_INLINE_BIO_VECS], *vecs;
	loff_t pos = iocb->ki_pos;
	bool should_dirty = false;
	struct bio bio;
	ssize_t ret;

	if (nr_pages <= DIO_INLINE_BIO_VECS) /* 작은 요청은 스택 내 bio_vec, 큰 요청은 동적 할당 */
		vecs = inline_vecs;
	else {
		vecs = kmalloc_objs(struct bio_vec, nr_pages);
		if (!vecs)
			return -ENOMEM;
	}

	if (iov_iter_rw(iter) == READ) { /* READ: NVMe Read 준비 */
		bio_init(&bio, bdev, vecs, nr_pages, REQ_OP_READ);
		if (user_backed_iter(iter))
			should_dirty = true;
	} else {
		bio_init(&bio, bdev, vecs, nr_pages, dio_bio_write_op(iocb)); /* WRITE: NVMe Write(FUA 포함 가능) 준비 */
	}
	bio.bi_iter.bi_sector = pos >> SECTOR_SHIFT; /* 파일 offset -> 512B LBA; NVMe SLBA 기초 */
	bio.bi_write_hint = file_inode(iocb->ki_filp)->i_write_hint; /* NVMe Write Stream/Hint(직접 I/O 힌트) */
	bio.bi_write_stream = iocb->ki_write_stream; /* NVMe Write Stream 식별자 */
	bio.bi_ioprio = iocb->ki_ioprio; /* NVMe I/O 우선순위(ioprio) 전달 */
	if (iocb->ki_flags & IOCB_ATOMIC) /* IOCB_ATOMIC -> NVMe atomic write 요청 */
		bio.bi_opf |= REQ_ATOMIC;

	ret = blkdev_iov_iter_get_pages(&bio, iter, bdev); /* 사용자 페이지를 bio bvec에 연결 */
	if (unlikely(ret))
		goto out;
	ret = bio.bi_iter.bi_size;

	if (iov_iter_rw(iter) == WRITE) /* WRITE 시 쓰기 바이트 통계 집계 */
		task_io_account_write(ret);

	if (iocb->ki_flags & IOCB_NOWAIT) /* IOCB_NOWAIT -> NVMe non-blocking 큐잉 힌트 */
		bio.bi_opf |= REQ_NOWAIT;

	submit_bio_wait(&bio); /* bio 제출 후 NVMe CQ 완료까지 동기 대기 */

	bio_release_pages(&bio, should_dirty);
	if (unlikely(bio.bi_status)) /* NVMe CQ에서 보고된 bio.bi_status를 errno로 변환 */
		ret = blk_status_to_errno(bio.bi_status);

out:
	if (vecs != inline_vecs)
		kfree(vecs);

	bio_uninit(&bio);

	return ret;
}

/*
 * dio 플래그: DIO_SHOULD_DIRTY는 read 후 사용자 페이지를 dirty로 표시하고,
 * DIO_IS_SYNC는 동기 I/O임을 나타낸다.
 */
enum {
	DIO_SHOULD_DIRTY	= 1,
	DIO_IS_SYNC		= 2,
};

/*
 * struct blkdev_dio - direct I/O 요청의 생명주기를 관리하는 컨테이너
 *
 * 목적: 하나의 kiocb가 여러 개의 bio로 분할될 때, 모든 bio의 완료를
 *       모아서 VFS에 결과를 돌려주기 전까지 메모리를 유지한다.
 * NVMe 연결:
 *   - 각 bio는 blk_mq_submit_bio -> nvme_queue_rq를 통해 NVMe SQ에
 *     CID를 할당받고 doorbell이 울려 실행된다.
 *   - ref는 아직 NVMe 컨트롤러로부터 CQ 완료가 도착하지 않은 bio의
 *     개수(=미완료 NVMe 명령)를 의미한다.
 *   - size는 NVMe가 실제로 전송/수신한 총 바이트로, kiocb 위치 갱신에
 *     사용된다.
 */
struct blkdev_dio {
	union {
		struct kiocb		*iocb; /* 비동기 완료 시 호출할 kiocb; NVMe CQ 콜백에서 ki_complete */
		struct task_struct	*waiter; /* 동기 완료 시 대기 태스크; NVMe 인터럽트가 blk_wake_io_task로 깨움 */
	};
	size_t			size; /* NVMe I/O가 누적 완료한 총 크기 */
	atomic_t		ref; /* 미완료 bio/SQ 명령 개수; 0이 되면 최종 완료 */
	unsigned int		flags; /* DIO_SHOULD_DIRTY, DIO_IS_SYNC 등 */
	struct bio		bio ____cacheline_aligned_in_smp; /* blkdev_dio_pool에서 할당된 첫 번째 bio; NVMe PRP/SGL 변환 대상 */
};

static struct bio_set blkdev_dio_pool; /* direct I/O bio 객체 풀; NVMe 요청의 원료 bio가 여기서 할당됨 */

/*
 * blkdev_bio_end_io - 분할 direct I/O의 단일 bio 완료 콜백
 *
 * 호출 경로:
 *   NVMe CQ 인터럽트/폴 -> bio_endio -> blkdev_bio_end_io
 *   (최종) dio->ref 0 -> kiocb->ki_complete 또는 blk_wake_io_task
 * 목적: 각 SQ 명령에 대응하는 bio의 상태를 dio에 모은 뒤, 마지막
 *       bio가 끝나면 상위 kiocb/waiter를 완료시킨다.
 */
static void blkdev_bio_end_io(struct bio *bio)
{
	struct blkdev_dio *dio = bio->bi_private; /* 분할된 모든 bio 중 하나의 NVMe CQ 완료 처리 */
	bool should_dirty = dio->flags & DIO_SHOULD_DIRTY;
	bool is_sync = dio->flags & DIO_IS_SYNC;

	if (bio->bi_status && !dio->bio.bi_status) /* 개별 bio 오류를 dio 상태에 전파 */
		dio->bio.bi_status = bio->bi_status;

	if (bio_integrity(bio))
		bio_integrity_unmap_user(bio);

	if (atomic_dec_and_test(&dio->ref)) { /* 마지막 NVMe 명령 완료 여부 확인 */
		if (!is_sync) { /* 비동기: -EIOCBQUEUED 반환, NVMe 완료를 kiocb로 처리 */
			struct kiocb *iocb = dio->iocb;
			ssize_t ret;

			WRITE_ONCE(iocb->private, NULL); /* NVMe CQ 완료 시 iocb->private 정리 */

			if (likely(!dio->bio.bi_status)) {
				ret = dio->size;
				iocb->ki_pos += ret;
			} else {
				ret = blk_status_to_errno(dio->bio.bi_status);
			}

			dio->iocb->ki_complete(iocb, ret); /* 비동기: NVMe CQ 완료를 VFS kiocb에 전달 */
			bio_put(&dio->bio);
		} else {
			struct task_struct *waiter = dio->waiter;

			WRITE_ONCE(dio->waiter, NULL);
			blk_wake_io_task(waiter); /* 동기: NVMe 인터럽트 핸들러가 대기 태스크 깨움 */
		}
	}

	if (should_dirty) { /* READ 시 사용자 페이지 dirty 처리 또는 해제 */
		bio_check_pages_dirty(bio);
	} else {
		bio_release_pages(bio, false); /* 실패 시: bio 해제 및 plug 종료 */
		bio_put(bio); /* 초기화/매핑 실패 시 bio 해제 */
	}
}

/*
 * __blkdev_direct_IO - bio가 여러 개로 분할될 수 있는 direct I/O
 *
 * 목적: 큰 I/O 또는 여러 segment가 필요한 경우 bio를 연쇄적으로
 *       할당/제출하고, 모든 bio 완료를 하나의 dio 객체로 집계한다.
 * 호출 경로:
 *   blkdev_direct_IO -> __blkdev_direct_IO
 *   -> submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *   -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *   (완료) NVMe CQ -> bio_endio -> blkdev_bio_end_io
 * NVMe 연결:
 *   - 각 submit_bio는 NVMe SQ에 별도의 CID를 할당받을 수 있다.
 *   - dio->ref가 미완료 NVMe 명령 수를 카운트한다.
 *   - is_sync일 땐 dio->waiter를 통해 CQ 인터럽트를 기다린다.
 *   - 비동기일 땐 dio->iocb->ki_complete가 NVMe 완료 후 호출된다.
 */
static ssize_t __blkdev_direct_IO(struct kiocb *iocb, struct iov_iter *iter,
		struct block_device *bdev, unsigned int nr_pages)
{
	struct blk_plug plug;
	struct blkdev_dio *dio;
	struct bio *bio;
	bool is_read = (iov_iter_rw(iter) == READ), is_sync;
	blk_opf_t opf = is_read ? REQ_OP_READ : dio_bio_write_op(iocb);
	loff_t pos = iocb->ki_pos;
	int ret = 0;

	bio = bio_alloc_bioset(bdev, nr_pages, opf, GFP_KERNEL, /* bio 객체를 blkdev_dio_pool에서 할당 */
			       &blkdev_dio_pool);
	dio = container_of(bio, struct blkdev_dio, bio); /* bio가 포함된 blkdev_dio 구조체 포인터 획득 */
	atomic_set(&dio->ref, 1); /* 미완료 bio(SQ 명령) 카운터 초기화 */
	/*
	 * Grab an extra reference to ensure the dio structure which is embedded
	 * into the first bio stays around.
	 */
	bio_get(bio); /* dio는 첫 bio에 임베드되므로 해제 방지용 추가 참조 */

	is_sync = is_sync_kiocb(iocb); /* 동기 kiocb 여부 판별; NVMe 완료 방식 결정 */
	if (is_sync) {
		dio->flags = DIO_IS_SYNC;
		dio->waiter = current; /* 동기: 현재 태스크가 NVMe CQ 인터럽트를 대기 */
	} else {
		dio->flags = 0;
		dio->iocb = iocb; /* 비동기: NVMe CQ 완료 시 호출할 kiocb 저장 */
	}

	dio->size = 0;
	if (is_read && user_backed_iter(iter))
		dio->flags |= DIO_SHOULD_DIRTY;

	blk_start_plug(&plug); /* writeback plug 시작: NVMe SQ batch 제출(추정) */

	for (;;) {
		bio->bi_iter.bi_sector = pos >> SECTOR_SHIFT; /* READ: NVMe Read / WRITE: NVMe Write(FUA) */
		bio->bi_write_hint = file_inode(iocb->ki_filp)->i_write_hint;
		bio->bi_write_stream = iocb->ki_write_stream;
		bio->bi_private = dio; /* 각 분할 bio에 dio 컨텍스트 연결 */
		bio->bi_end_io = blkdev_bio_end_io; /* NVMe CQ 완료 시 호출할 end_io 설정 */
		bio->bi_ioprio = iocb->ki_ioprio;

		ret = blkdev_iov_iter_get_pages(bio, iter, bdev); /* 일반 iter면 사용자 페이지를 bio에 매핑 */
		if (unlikely(ret)) {
			bio->bi_status = BLK_STS_IOERR; /* 페이지 매핑 실패 시 NVMe IOERR 상태로 bio 완료 */
			bio_endio(bio);
			break;
		}
		if (iocb->ki_flags & IOCB_NOWAIT) { /* NOWAIT 요청이 NVMe 큐에서 블로킹되는 분할을 피하기 위해 부분 매핑 시 EAGAIN */
			/*
			 * This is nonblocking IO, and we need to allocate
			 * another bio if we have data left to map. As we
			 * cannot guarantee that one of the sub bios will not
			 * fail getting issued FOR NOWAIT and as error results
			 * are coalesced across all of them, be safe and ask for
			 * a retry of this from blocking context.
			 */
			if (unlikely(iov_iter_count(iter))) {
				ret = -EAGAIN;
				goto fail;
			}
			bio->bi_opf |= REQ_NOWAIT; /* IOCB_NOWAIT -> NVMe non-blocking */
		}
		if (iocb->ki_flags & IOCB_HAS_METADATA) { /* IOCB_HAS_METADATA: NVMe integrity 메타데이터 */
			ret = bio_integrity_map_iter(bio, iocb->private); /* IOCB_HAS_METADATA: NVMe PI/DIF 메타데이터 매핑 */
			if (unlikely(ret))
				goto fail;
		}

		if (is_read) { /* READ 시 dirty 페이지 표시, WRITE 시 통계 집계 */
			if (dio->flags & DIO_SHOULD_DIRTY)
				bio_set_pages_dirty(bio); /* READ 시 dirty 페이지 표시 */
		} else {
			task_io_account_write(bio->bi_iter.bi_size);
		}
		dio->size += bio->bi_iter.bi_size; /* 누적 크기 및 파일 위치 갱신 */
		pos += bio->bi_iter.bi_size;

		nr_pages = bio_iov_vecs_to_alloc(iter, BIO_MAX_VECS);
		if (!nr_pages) {
			submit_bio(bio);
			break;
		}
		atomic_inc(&dio->ref); /* 남은 페이지가 있으면 다음 bio 할당; ref 증가 후 NVMe SQ로 제출 */
		submit_bio(bio);
		bio = bio_alloc(bdev, nr_pages, opf, GFP_KERNEL);
	}

	blk_finish_plug(&plug); /* bio batch 제출 종료 */

	if (!is_sync)
		return -EIOCBQUEUED;

	for (;;) {
		set_current_state(TASK_UNINTERRUPTIBLE); /* 모든 NVMe CQ 완료가 도착할 때까지 TASK_UNINTERRUPTIBLE 대기 */
		if (!READ_ONCE(dio->waiter))
			break;
		blk_io_schedule();
	}
	__set_current_state(TASK_RUNNING);

	if (!ret) /* 모든 bio 완료 후 dio->size 반환 */
		ret = blk_status_to_errno(dio->bio.bi_status);
	if (likely(!ret))
		ret = dio->size;

	bio_put(&dio->bio);
	return ret;
fail:
	bio_release_pages(bio, false);
	bio_clear_flag(bio, BIO_REFFED);
	bio_put(bio);
	blk_finish_plug(&plug);
	return ret;
}

/*
 * blkdev_bio_end_io_async - 비동기 direct I/O bio 완료 콜백
 *
 * 호출 경로:
 *   NVMe CQ -> bio_endio -> blkdev_bio_end_io_async
 *   -> kiocb->ki_complete
 * 목적: 단일 bio 비동기 요청의 완료를 VFS kiocb에 즉시 전달한다.
 */
static void blkdev_bio_end_io_async(struct bio *bio)
{
	struct blkdev_dio *dio = container_of(bio, struct blkdev_dio, bio);
	struct kiocb *iocb = dio->iocb;
	ssize_t ret;

	WRITE_ONCE(iocb->private, NULL);

	if (likely(!bio->bi_status)) {
		ret = dio->size;
		iocb->ki_pos += ret;
	} else {
		ret = blk_status_to_errno(bio->bi_status); /* bio.bi_status를 errno로 변환 후 VFS에 전달 */
	}

	if (bio_integrity(bio))
		bio_integrity_unmap_user(bio);

	iocb->ki_complete(iocb, ret);

	if (dio->flags & DIO_SHOULD_DIRTY) {
		bio_check_pages_dirty(bio);
	} else {
		bio_release_pages(bio, false);
		bio_put(bio);
	}
}

/*
 * __blkdev_direct_IO_async - 단일 bio 비동기 direct I/O
 *
 * 목적: sync가 아닌 kiocb에 대해 단일 bio로 요청을 만들고
 *       -EIOCBQUEUED를 반환하며 NVMe 완료를 기다린다.
 * 호출 경로:
 *   blkdev_direct_IO -> __blkdev_direct_IO_async
 *   -> submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *   -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *   -> NVMe CQ -> bio_endio -> blkdev_bio_end_io_async
 *       -> kiocb->ki_complete
 * NVMe 연결:
 *   - IOCB_HIPRI 시 REQ_POLLED로 폴 모드 CQ를 사용하여 인터럽트
 *     없이 NVMe 완료를 수집한다.
 *   - iocb->private에 bio를 저장하면 iocb_bio_iopoll이
 *     blk_poll -> nvme_poll_cq로 연결된다.
 */
static ssize_t __blkdev_direct_IO_async(struct kiocb *iocb,
					struct iov_iter *iter,
					struct block_device *bdev,
					unsigned int nr_pages)
{
	bool is_read = iov_iter_rw(iter) == READ;
	blk_opf_t opf = is_read ? REQ_OP_READ : dio_bio_write_op(iocb);
	struct blkdev_dio *dio;
	struct bio *bio;
	loff_t pos = iocb->ki_pos;
	int ret = 0;

	bio = bio_alloc_bioset(bdev, nr_pages, opf, GFP_KERNEL,
			       &blkdev_dio_pool);
	dio = container_of(bio, struct blkdev_dio, bio);
	dio->flags = 0;
	dio->iocb = iocb;
	bio->bi_iter.bi_sector = pos >> SECTOR_SHIFT;
	bio->bi_write_hint = file_inode(iocb->ki_filp)->i_write_hint;
	bio->bi_write_stream = iocb->ki_write_stream;
	bio->bi_end_io = blkdev_bio_end_io_async; /* bio 완료 콜백 등록 */
	bio->bi_ioprio = iocb->ki_ioprio;

	if (iov_iter_is_bvec(iter)) { /* bvec 기반 iter면 페이지 복사 없이 NVMe SGL/PRP 원본 사용 */
		/*
		 * Users don't rely on the iterator being in any particular
		 * state for async I/O returning -EIOCBQUEUED, hence we can
		 * avoid expensive iov_iter_advance(). Bypass
		 * bio_iov_iter_get_pages() and set the bvec directly.
		 */
		bio_iov_bvec_set(bio, iter);
	} else {
		ret = blkdev_iov_iter_get_pages(bio, iter, bdev);
		if (unlikely(ret))
			goto out_bio_put;
	}
	dio->size = bio->bi_iter.bi_size; /* 단일 bio 총 크기 확정 */

	if (is_read) {
		if (user_backed_iter(iter)) {
			dio->flags |= DIO_SHOULD_DIRTY;
			bio_set_pages_dirty(bio);
		}
	} else {
		task_io_account_write(bio->bi_iter.bi_size);
	}

	if (iocb->ki_flags & IOCB_HAS_METADATA) {
		ret = bio_integrity_map_iter(bio, iocb->private);
		WRITE_ONCE(iocb->private, NULL);
		if (unlikely(ret))
			goto out_bio_put;
	}

	if (iocb->ki_flags & IOCB_ATOMIC)
		bio->bi_opf |= REQ_ATOMIC; /* IOCB_ATOMIC -> NVMe atomic write */

	if (iocb->ki_flags & IOCB_NOWAIT)
		bio->bi_opf |= REQ_NOWAIT;

	if (iocb->ki_flags & IOCB_HIPRI) { /* IOCB_HIPRI -> REQ_POLLED, NVMe polled CQ 경로 활성화 */
		bio->bi_opf |= REQ_POLLED;
		submit_bio(bio);
		WRITE_ONCE(iocb->private, bio); /* 폴 CQ용 bio를 iocb->private에 저장 */
	} else {
		submit_bio(bio);
	}
	return -EIOCBQUEUED;

out_bio_put:
	bio_put(bio);
	return ret;
}

/*
 * blkdev_direct_IO - VFS direct I/O 진입점
 *
 * 호출 경로:
 *   vfs_read/vfs_write -> kiocb->read_iter/write_iter
 *   -> blkdev_read_iter/blkdev_write_iter -> blkdev_direct_IO
 *   -> (__blkdev_direct_IO_simple/__blkdev_direct_IO_async/__blkdev_direct_IO)
 *   -> submit_bio -> blk_mq_submit_bio -> nvme_queue_rq
 *   -> nvme_submit_cmd(doorbell)
 * 목적: 요청 크기/플래그에 따라 단순/비동기/분할 direct I/O 경로를
 *       선택하고, NVMe가 처리할 bio 형태로 변환한다.
 * NVMe 연결: IOCB_DIRECT 플래그가 있을 때만 이 경로로 들어오며,
 *            NVMe의 PRP/SGL, FUA, NOWAIT, POLLED, ATOMIC 등의
 *            특성은 bio.bi_opf를 통해 드라이버에 전달된다.
 */
static ssize_t blkdev_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
	struct block_device *bdev = I_BDEV(iocb->ki_filp->f_mapping->host);
	unsigned int nr_pages;

	if (!iov_iter_count(iter))
		return 0;

	if (blkdev_dio_invalid(bdev, iocb, iter)) /* offset/len이 NVMe LBA 정렬에 맞는지 검사 */
		return -EINVAL;

	if (iov_iter_rw(iter) == WRITE) { /* NVMe Directives/Streams 활용을 위한 write_stream 힌트 설정 */
		u16 max_write_streams = bdev_max_write_streams(bdev);

		if (iocb->ki_write_stream) {
			if (iocb->ki_write_stream > max_write_streams)
				return -EINVAL;
		} else if (max_write_streams) {
			enum rw_hint write_hint =
				file_inode(iocb->ki_filp)->i_write_hint;

			/*
			 * Just use the write hint as write stream for block
			 * device writes.  This assumes no file system is
			 * mounted that would use the streams differently.
			 */
			if (write_hint <= max_write_streams)
				iocb->ki_write_stream = write_hint;
		}
	}

	nr_pages = bio_iov_vecs_to_alloc(iter, BIO_MAX_VECS + 1); /* 요청 크기에 따라 단순/비동기/분할 경로 선택 */
	if (likely(nr_pages <= BIO_MAX_VECS &&
		   !(iocb->ki_flags & IOCB_HAS_METADATA))) {
		if (is_sync_kiocb(iocb))
			return __blkdev_direct_IO_simple(iocb, iter, bdev,
							nr_pages);
		return __blkdev_direct_IO_async(iocb, iter, bdev, nr_pages);
	} else if (iocb->ki_flags & IOCB_ATOMIC) { /* atomic write는 분할 경로를 지원하지 않음 */
		return -EINVAL;
	}
	return __blkdev_direct_IO(iocb, iter, bdev, bio_max_segs(nr_pages));
}

/*
 * blkdev_iomap_begin - 파일 offset을 block device의 LBA 영역으로 매핑
 *
 * 호출 경로: buffered read/write/readahead -> iomap -> blkdev_iomap_begin
 * NVMe 연결: 반환된 iomap->bdev/offset/length가 이후 bio를 만들 때
 *            bio.bi_sector의 기초가 되며, NVMe SLBA로 변환된다.
 */
static int blkdev_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
		unsigned int flags, struct iomap *iomap, struct iomap *srcmap)
{
	struct block_device *bdev = I_BDEV(inode);
	loff_t isize = i_size_read(inode);

	if (offset >= isize)
		return -EIO;

	iomap->bdev = bdev; /* bdev 및 논리 블록 정렬 offset 설정 */
	iomap->offset = ALIGN_DOWN(offset, bdev_logical_block_size(bdev));
	iomap->type = IOMAP_MAPPED; /* IOMAP_MAPPED: bdev의 LBA 영역이 이미 매핑됨 */
	iomap->addr = iomap->offset;
	iomap->length = isize - iomap->offset;
	iomap->flags |= IOMAP_F_BUFFER_HEAD; /* noop for !CONFIG_BUFFER_HEAD */ /* IOMAP_F_BUFFER_HEAD: buffer_head 사용 시 플래그 */
	return 0;
}

static const struct iomap_ops blkdev_iomap_ops = { /* buffered I/O가 bio를 만들기 위한 bdev/LBA 정보 제공 */
	.iomap_begin		= blkdev_iomap_begin,
};

#ifdef CONFIG_BUFFER_HEAD
/*
 * blkdev_get_block - buffer_head용 block 번호 매핑
 *
 * 호출 경로: block_read_full_folio/block_write_full_folio ->
 *           blkdev_get_block
 * NVMe 연결: bh->b_blocknr가 bio.bi_sector로 변환되어 NVMe Read/Write
 *            명령의 SLBA가 된다.
 */
static int blkdev_get_block(struct inode *inode, sector_t iblock,
		struct buffer_head *bh, int create)
{
	bh->b_bdev = I_BDEV(inode); /* buffer_head에 bdev 및 block 번호 설정 */
	bh->b_blocknr = iblock; /* inode offset에서 LBA(blocknr) 결정 */
	set_buffer_mapped(bh);
	return 0;
}

/*
 * We cannot call mpage_writepages() as it does not take the buffer lock.
 * We must use block_write_full_folio() directly which holds the buffer
 * lock.  The buffer lock provides the synchronisation with writeback
 * that filesystems rely on when they use the blockdev's mapping.
 */
/*
 * blkdev_writepages (buffer_head) - 페이지 캐시 더티 페이지를 NVMe Write로 writeback
 *
 * 호출 경로: writeback -> blkdev_writepages -> block_write_full_folio
 *           -> submit_bio -> blk_mq_submit_bio -> nvme_queue_rq
 */
static int blkdev_writepages(struct address_space *mapping,
		struct writeback_control *wbc)
{
	struct folio *folio = NULL;
	struct blk_plug plug;
	int err;

	blk_start_plug(&plug);
	while ((folio = writeback_iter(mapping, wbc, folio, &err)))
		err = block_write_full_folio(folio, wbc, blkdev_get_block); /* 각 더티 folio를 block_write_full_folio로 NVMe Write bio 변환 */
	blk_finish_plug(&plug);

	return err;
}

/*
 * blkdev_read_folio - 단일 folio를 NVMe Read로 채움
 *
 * block_read_full_folio가 bio를 조립해 NVMe Read로 전달한다.
 */
static int blkdev_read_folio(struct file *file, struct folio *folio)
{
	return block_read_full_folio(folio, blkdev_get_block);
}

/*
 * blkdev_readahead - 순차 읽기를 NVMe Read로 prefetch
 *
 * mpage_readahead가 여러 bio를 묶어 NVMe SQ에 제출한다.
 */
static void blkdev_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, blkdev_get_block);
}

/*
 * blkdev_write_begin - buffered write를 위한 페이지 준비
 *
 * block_write_begin이 더티 folio를 생성하고 이후 writeback에서
 * NVMe Write로 변환된다.
 */
static int blkdev_write_begin(const struct kiocb *iocb,
			      struct address_space *mapping, loff_t pos,
			      unsigned len, struct folio **foliop,
			      void **fsdata)
{
	return block_write_begin(mapping, pos, len, foliop, blkdev_get_block);
}

/*
 * blkdev_write_end - buffered write folio 정리
 *
 * 페이지 캐시 쓰기가 완료되면 더티 상태로 남겨 writeback 시 NVMe Write 제출.
 */
static int blkdev_write_end(const struct kiocb *iocb,
			    struct address_space *mapping,
			    loff_t pos, unsigned len, unsigned copied,
			    struct folio *folio, void *fsdata)
{
	int ret;
	ret = block_write_end(pos, len, copied, folio);

	folio_unlock(folio);
	folio_put(folio);

	return ret;
}

/*
 * def_blk_aops (CONFIG_BUFFER_HEAD) - 페이지 캐시 경유 block 장치 연산
 *
 * readahead/read/writepages 등이 bio를 생성하여
 * submit_bio -> blk_mq_submit_bio -> nvme_queue_rq로 전달된다.
 * NVMe 연결: readahead는 NVMe 순차 prefetch, writepages는 더티 페이지를
 *            NVMe Write 명령으로 묶어 SQ에 제출한다.
 */
const struct address_space_operations def_blk_aops = {
	.dirty_folio	= block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
	.read_folio	= blkdev_read_folio,
	.readahead	= blkdev_readahead,
	.writepages	= blkdev_writepages,
	.write_begin	= blkdev_write_begin,
	.write_end	= blkdev_write_end,
	.migrate_folio	= buffer_migrate_folio_norefs,
	.is_dirty_writeback = buffer_check_dirty_writeback,
};
#else /* CONFIG_BUFFER_HEAD */
/*
 * blkdev_read_folio (iomap) - iomap을 통한 folio read
 *
 * iomap_bio_read_folio가 bio를 만들어 NVMe Read로 전달한다.
 */
static int blkdev_read_folio(struct file *file, struct folio *folio)
{
	iomap_bio_read_folio(folio, &blkdev_iomap_ops);
	return 0;
}

/*
 * blkdev_readahead (iomap) - iomap 기반 prefetch
 *
 * iomap_bio_readahead가 NVMe Read bio를 생성한다.
 */
static void blkdev_readahead(struct readahead_control *rac)
{
	iomap_bio_readahead(rac, &blkdev_iomap_ops);
}

/*
 * blkdev_writeback_range - iomap writeback에서 한 folio 범위를 NVMe I/O로 추가
 *
 * iomap_add_to_ioend가 bio를 모아 NVMe Write 제출 단위로 만든다.
 */
static ssize_t blkdev_writeback_range(struct iomap_writepage_ctx *wpc,
		struct folio *folio, u64 offset, unsigned int len, u64 end_pos)
{
	loff_t isize = i_size_read(wpc->inode);

	if (WARN_ON_ONCE(offset >= isize)) /* folio offset이 NVMe namespace 크기를 벗어나면 오류 */
		return -EIO;

	if (offset < wpc->iomap.offset || /* iomap 캐시가 유효하지 않으면 blkdev_iomap_begin으로 LBA 재매핑 */
	    offset >= wpc->iomap.offset + wpc->iomap.length) {
		int error;

		error = blkdev_iomap_begin(wpc->inode, offset, isize - offset,
				IOMAP_WRITE, &wpc->iomap, NULL);
		if (error)
			return error;
	}

	return iomap_add_to_ioend(wpc, folio, offset, end_pos, len);
}

static const struct iomap_writeback_ops blkdev_writeback_ops = { /* iomap writeback이 NVMe Write bio를 조립/제출 */
	.writeback_range	= blkdev_writeback_range,
	.writeback_submit	= iomap_ioend_writeback_submit,
};

/*
 * blkdev_writepages (iomap) - iomap 기반 페이지 캐시 writeback
 *
 * iomap_writepages가 더티 folio를 bio로 변환 후 NVMe Write로 제출한다.
 */
static int blkdev_writepages(struct address_space *mapping,
		struct writeback_control *wbc)
{
	struct iomap_writepage_ctx wpc = {
		.inode		= mapping->host,
		.wbc		= wbc,
		.ops		= &blkdev_writeback_ops
	};

	return iomap_writepages(&wpc);
}

/*
 * def_blk_aops (iomap) - iomap 기반 페이지 캐시 연산
 *
 * dirty_folio/read_folio/readahead/writepages 등이 bio를 생성하여
 * submit_bio -> blk_mq_submit_bio -> nvme_queue_rq로 전달된다.
 */
const struct address_space_operations def_blk_aops = {
	.dirty_folio	= filemap_dirty_folio,
	.release_folio		= iomap_release_folio,
	.invalidate_folio	= iomap_invalidate_folio,
	.read_folio		= blkdev_read_folio,
	.readahead		= blkdev_readahead,
	.writepages		= blkdev_writepages,
	.is_partially_uptodate  = iomap_is_partially_uptodate,
	.error_remove_folio	= generic_error_remove_folio,
	.migrate_folio		= filemap_migrate_folio,
};
#endif /* CONFIG_BUFFER_HEAD */

/*
 * for a block special file file_inode(file)->i_size is zero
 * so we compute the size by hand (just as in block_read/write above)
 */
/*
 * blkdev_llseek - 블록 장치의 파일 위치 변경
 *
 * block special file의 i_size는 0이므로 bdev 크기를 기준으로 계산.
 * NVMe 연결: 잘못된 offset은 NVMe 범위를 벗어난 LBA로 전달될 수
 *            있으므로, VFS 단계에서 제한한다.
 */
static loff_t blkdev_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *bd_inode = bdev_file_inode(file);
	loff_t retval;

	inode_lock(bd_inode);
	retval = fixed_size_llseek(file, offset, whence, i_size_read(bd_inode)); /* bdev 크기를 기준으로 유효 범위 제한 */
	inode_unlock(bd_inode);
	return retval;
}

/*
 * blkdev_fsync - 블록 장치 fsync
 *
 * 호출 경로: vfs_fsync -> blkdev_fsync -> blkdev_issue_flush
 * NVMe 연결: blkdev_issue_flush는 NVMe Flush 명령을 해당
 *            namespace의 SQ에 제출하고, CQ 완료까지 대기한다.
 *            EOPNOTSUPP 시 NVMe 플러시가 지원되지 않는 것으로 간주.
 */
static int blkdev_fsync(struct file *filp, loff_t start, loff_t end,
		int datasync)
{
	struct block_device *bdev = I_BDEV(filp->f_mapping->host);
	int error;

	error = file_write_and_wait_range(filp, start, end); /* 더티 페이지를 먼저 기록 */
	if (error)
		return error;

	/*
	 * There is no need to serialise calls to blkdev_issue_flush with
	 * i_mutex and doing so causes performance issues with concurrent
	 * O_SYNC writers to a block device.
	 */
	error = blkdev_issue_flush(bdev); /* NVMe Flush 명령 제출 */
	if (error == -EOPNOTSUPP) /* NVMe Flush 미지원 시 무시 */
		error = 0;

	return error;
}

/**
 * file_to_blk_mode - get block open flags from file flags
 * @file: file whose open flags should be converted
 *
 * Look at file open flags and generate corresponding block open flags from
 * them. The function works both for file just being open (e.g. during ->open
 * callback) and for file that is already open. This is actually non-trivial
 * (see comment in the function).
 */
/*
 * file_to_blk_mode - VFS 파일 열기 모드를 block open 모드로 변환
 *
 * open/release는 block/bdev.c의 bdev_open/bdev_release와 연결된다.
 * NVMe 연결: BLK_OPEN_EXCL/READ/WRITE가 NVMe namespace 사용 권한을
 *            결정한다.
 */
blk_mode_t file_to_blk_mode(struct file *file)
{
	blk_mode_t mode = 0;

	if (file->f_mode & FMODE_READ) /* 파일 읽기 모드 -> NVMe namespace 읽기 권한 */
		mode |= BLK_OPEN_READ;
	if (file->f_mode & FMODE_WRITE) /* 파일 쓰기 모드 -> NVMe namespace 쓰기 권한 */
		mode |= BLK_OPEN_WRITE;
	/*
	 * do_dentry_open() clears O_EXCL from f_flags, use file->private_data
	 * to determine whether the open was exclusive for already open files.
	 */
	if (file->private_data) /* 배타적 오픈 여부 */
		mode |= BLK_OPEN_EXCL;
	else if (file->f_flags & O_EXCL)
		mode |= BLK_OPEN_EXCL;
	if (file->f_flags & O_NDELAY)
		mode |= BLK_OPEN_NDELAY;

	/*
	 * If all bits in O_ACCMODE set (aka O_RDWR | O_WRONLY), the floppy
	 * driver has historically allowed ioctls as if the file was opened for
	 * writing, but does not allow and actual reads or writes.
	 */
	if ((file->f_flags & O_ACCMODE) == (O_RDWR | O_WRONLY))
		mode |= BLK_OPEN_WRITE_IOCTL;

	return mode;
}

/*
 * blkdev_open - 블록 장치 파일 열기
 *
 * 호출 경로: vfs_open -> def_blk_fops.open -> blkdev_open
 *           -> bdev_permission -> blkdev_get_no_open -> bdev_open
 * NVMe 연결: bdev_open이 성공하면 NVMe namespace에 대한 참조가
 *            확복되며, 이후 read/write에서 해당 nvme_ns의 bdev를
 *            통해 NVMe 큐로 요청이 흘러간다.
 */
static int blkdev_open(struct inode *inode, struct file *filp)
{
	struct block_device *bdev;
	blk_mode_t mode;
	int ret;

	mode = file_to_blk_mode(filp); /* VFS 모드를 block open 모드로 변환 */
	/* Use the file as the holder. */
	if (mode & BLK_OPEN_EXCL) /* exclusive open 시 파일을 holder로 사용 */
		filp->private_data = filp;
	ret = bdev_permission(inode->i_rdev, mode, filp->private_data); /* bdev_permission으로 NVMe namespace 접근 권한 검사 */
	if (ret)
		return ret;

	bdev = blkdev_get_no_open(inode->i_rdev, true); /* bdev 객체 획득; NVMe namespace와 연결 */
	if (!bdev)
		return -ENXIO;

	if (bdev_can_atomic_write(bdev)) /* NVMe atomic write 지원 시 플래그 설정 */
		filp->f_mode |= FMODE_CAN_ATOMIC_WRITE;
	if (blk_get_integrity(bdev->bd_disk)) /* NVMe PI/DIF 메타데이터 지원 시 플래그 설정 */
		filp->f_mode |= FMODE_HAS_METADATA;

	ret = bdev_open(bdev, mode, filp->private_data, NULL, filp); /* bdev_open이 성공하면 NVMe namespace가 사용 가능해짐 */
	if (ret)
		blkdev_put_no_open(bdev); /* open 실패 시 bdev 참조 해제 */
	return ret;
}

/*
 * blkdev_release - 블록 장치 파일 닫기
 *
 * bdev_release를 호출하여 NVMe namespace 참조 및 관련 자원을
 * 해제한다.
 */
static int blkdev_release(struct inode *inode, struct file *filp)
{
	bdev_release(filp);
	return 0;
}

/*
 * blkdev_direct_write - direct write 공통 처리
 *
 * 호출 경로: blkdev_write_iter -> blkdev_direct_write -> blkdev_direct_IO
 * NVMe 연결: O_DIRECT로 제출된 write가 NVMe Write 명령으로 변환된다.
 */
static ssize_t
blkdev_direct_write(struct kiocb *iocb, struct iov_iter *from)
{
	size_t count = iov_iter_count(from);
	ssize_t written;

	written = kiocb_invalidate_pages(iocb, count); /* 페이지 캐시 무효화 */
	if (written) {
		if (written == -EBUSY)
			return 0;
		return written;
	}

	written = blkdev_direct_IO(iocb, from); /* blkdev_direct_IO를 통해 NVMe Write 제출 */
	if (written > 0) {
		kiocb_invalidate_post_direct_write(iocb, count);
		iocb->ki_pos += written;
		count -= written;
	}
	if (written != -EIOCBQUEUED)
		iov_iter_revert(from, count - iov_iter_count(from));
	return written;
}

/*
 * blkdev_buffered_write - 페이지 캐시를 경유하는 buffered write
 *
 * 호출 경로: blkdev_write_iter -> blkdev_buffered_write
 *           -> iomap_file_buffered_write -> iomap -> submit_bio
 * NVMe 연결: 더티 페이지는 writeback 시점에 NVMe Write로 제출되거나,
 *            fsync/flush 시 NVMe Flush 명령과 연계된다.
 */
static ssize_t blkdev_buffered_write(struct kiocb *iocb, struct iov_iter *from)
{
	return iomap_file_buffered_write(iocb, from, &blkdev_iomap_ops, NULL,
			NULL);
}

/*
 * Write data to the block device.  Only intended for the block device itself
 * and the raw driver which basically is a fake block device.
 *
 * Does not take i_mutex for the write and thus is not for general purpose
 * use.
 */
/*
 * blkdev_write_iter - 블록 장치 write_iter 진입점
 *
 * 호출 경로: vfs_write -> blkdev_write_iter
 *           -> blkdev_direct_write / blkdev_buffered_write
 *           -> blkdev_direct_IO / iomap_file_buffered_write
 *           -> submit_bio -> blk_mq_submit_bio -> nvme_queue_rq
 * NVMe 연결: IOCB_DIRECT가 설정되면 NVMe SQ로 직접, 그렇지 않으면
 *            페이지 캐시를 거쳐 writeback 단계에서 NVMe SQ로 전달.
 */
static ssize_t blkdev_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *bd_inode = bdev_file_inode(file);
	struct block_device *bdev = I_BDEV(bd_inode);
	bool atomic = iocb->ki_flags & IOCB_ATOMIC;
	loff_t size = bdev_nr_bytes(bdev);
	size_t shorted = 0;
	ssize_t ret;

	if (bdev_read_only(bdev)) /* read-only NVMe namespace에는 쓰기 금지 */
		return -EPERM;

	if (IS_SWAPFILE(bd_inode) && !is_hibernate_resume_dev(bd_inode->i_rdev)) /* swapfile 보호 */
		return -ETXTBSY;

	if (!iov_iter_count(from))
		return 0;

	if (iocb->ki_pos >= size)
		return -ENOSPC;

	if ((iocb->ki_flags & (IOCB_NOWAIT | IOCB_DIRECT)) == IOCB_NOWAIT) /* NOWAIT는 DIRECT 없이는 NVMe non-blocking 경로를 사용할 수 없음 */
		return -EOPNOTSUPP;

	if (atomic) { /* NVMe atomic write 유효성 검사 */
		ret = generic_atomic_write_valid(iocb, from);
		if (ret)
			return ret;
	}

	size -= iocb->ki_pos;
	if (iov_iter_count(from) > size) { /* NVMe namespace 크기를 초과하면 truncate */
		if (atomic)
			return -EINVAL;
		shorted = iov_iter_count(from) - size;
		iov_iter_truncate(from, size);
	}

	ret = file_update_time(file);
	if (ret)
		return ret;

	if (iocb->ki_flags & IOCB_DIRECT) { /* O_DIRECT: NVMe Read 직접 수행 */
		ret = blkdev_direct_write(iocb, from);
		if (ret >= 0 && iov_iter_count(from))
			ret = direct_write_fallback(iocb, from, ret, /* 일부만 쓰여지면 buffered write로 fallback */
					blkdev_buffered_write(iocb, from));
	} else {
		/*
		 * Take i_rwsem and invalidate_lock to avoid racing with
		 * set_blocksize changing i_blkbits/folio order and punching
		 * out the pagecache.
		 */
		inode_lock_shared(bd_inode); /* buffered: 페이지 캐시에서 읽고 미스 시 NVMe Read/prefetch */
		ret = blkdev_buffered_write(iocb, from);
		inode_unlock_shared(bd_inode);
	}

	if (ret > 0) /* fsync 힌트를 통해 NVMe Flush 연계 */
		ret = generic_write_sync(iocb, ret);
	iov_iter_reexpand(from, iov_iter_count(from) + shorted);
	return ret;
}

/*
 * blkdev_read_iter - 블록 장치 read_iter 진입점
 *
 * 호출 경로: vfs_read -> blkdev_read_iter -> blkdev_direct_IO 또는
 *           filemap_read -> readahead -> blkdev_read_folio
 * NVMe 연결: O_DIRECT 시 NVMe Read, buffered 시 페이지 캐시 미스 시
 *            NVMe Read로 prefetch/fill.
 */
static ssize_t blkdev_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *bd_inode = bdev_file_inode(iocb->ki_filp);
	struct block_device *bdev = I_BDEV(iocb->ki_filp->f_mapping->host);
	loff_t size = bdev_nr_bytes(bdev);
	loff_t pos = iocb->ki_pos;
	size_t shorted = 0;
	ssize_t ret = 0;
	size_t count;

	if (unlikely(pos + iov_iter_count(to) > size)) { /* bdev 크기를 초과하는 read는 잘라냄 */
		if (pos >= size)
			return 0;
		size -= pos;
		shorted = iov_iter_count(to) - size;
		iov_iter_truncate(to, size);
	}

	count = iov_iter_count(to);
	if (!count)
		goto reexpand; /* skip atime */

	if (iocb->ki_flags & IOCB_DIRECT) {
		ret = kiocb_write_and_wait(iocb, count);
		if (ret < 0)
			goto reexpand;
		file_accessed(iocb->ki_filp);

		ret = blkdev_direct_IO(iocb, to);
		if (ret > 0) {
			iocb->ki_pos += ret;
			count -= ret;
		}
		if (ret != -EIOCBQUEUED)
			iov_iter_revert(to, count - iov_iter_count(to));
		if (ret < 0 || !count)
			goto reexpand;
	}

	/*
	 * Take i_rwsem and invalidate_lock to avoid racing with set_blocksize
	 * changing i_blkbits/folio order and punching out the pagecache.
	 */
	inode_lock_shared(bd_inode);
	ret = filemap_read(iocb, to, ret);
	inode_unlock_shared(bd_inode);

reexpand:
	if (unlikely(shorted))
		iov_iter_reexpand(to, iov_iter_count(to) + shorted);
	return ret;
}

#define	BLKDEV_FALLOC_FL_SUPPORTED					\
		(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE |		\
		 FALLOC_FL_ZERO_RANGE | FALLOC_FL_WRITE_ZEROES)

/*
 * blkdev_fallocate - 블록 장치 fallocate 구현
 *
 * 호출 경로: vfs_fallocate -> blkdev_fallocate -> blkdev_issue_zeroout
 * NVMe 연결:
 *   - FALLOC_FL_WRITE_ZEROES는 NVMe Write Zeroes 명령으로 매핑될 수 있음.
 *   - FALLOC_FL_PUNCH_HOLE는 NVMe Dataset Management/Deallocate(Trim)로
 *     매핑될 수 있음.
 *   - FALLOC_FL_ZERO_RANGE는 zero-fill write로 처리.
 */
static long blkdev_fallocate(struct file *file, int mode, loff_t start,
			     loff_t len)
{
	struct inode *inode = bdev_file_inode(file);
	struct block_device *bdev = I_BDEV(inode);
	loff_t end = start + len - 1;
	loff_t isize;
	unsigned int flags;
	int error;

	/* Fail if we don't recognize the flags. */
	if (mode & ~BLKDEV_FALLOC_FL_SUPPORTED) /* 지원하지 않는 fallocate 플래그 거부 */
		return -EOPNOTSUPP;
	/*
	 * Don't allow writing zeroes if the device does not enable the
	 * unmap write zeroes operation.
	 */
	if ((mode & FALLOC_FL_WRITE_ZEROES) && /* WRITE_ZEROES는 NVMe Write Zeroes/Deallocate 지원 필요 */
	    !bdev_write_zeroes_unmap_sectors(bdev))
		return -EOPNOTSUPP;

	/* Don't go off the end of the device. */
	isize = bdev_nr_bytes(bdev); /* NVMe namespace 크기 범위 검사 */
	if (start >= isize)
		return -EINVAL;
	if (end >= isize) {
		if (mode & FALLOC_FL_KEEP_SIZE) {
			len = isize - start;
			end = start + len - 1;
		} else
			return -EINVAL;
	}

	/*
	 * Don't allow IO that isn't aligned to logical block size.
	 */
	if ((start | len) & (bdev_logical_block_size(bdev) - 1)) /* NVMe LBA 정렬 검사 */
		return -EINVAL;

	inode_lock(inode);
	filemap_invalidate_lock(inode->i_mapping);

	switch (mode) { /* fallocate 모드에 따른 NVMe zero/trim/unmap 플래그 선택 */
	case FALLOC_FL_ZERO_RANGE:
	case FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE:
		flags = BLKDEV_ZERO_NOUNMAP;
		break;
	case FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE:
		flags = BLKDEV_ZERO_NOFALLBACK;
		break;
	case FALLOC_FL_WRITE_ZEROES:
		flags = 0;
		break;
	default:
		error = -EOPNOTSUPP;
		goto fail;
	}

	/*
	 * Invalidate the page cache, including dirty pages, for valid
	 * de-allocate mode calls to fallocate().
	 */
	error = truncate_bdev_range(bdev, file_to_blk_mode(file), start, end);
	if (error)
		goto fail;

	error = blkdev_issue_zeroout(bdev, start >> SECTOR_SHIFT, /* blkdev_issue_zeroout -> NVMe Write Zeroes/Deallocate 제출 */
				     len >> SECTOR_SHIFT, GFP_KERNEL, flags);
 fail:
	filemap_invalidate_unlock(inode->i_mapping);
	inode_unlock(inode);
	return error;
}

/*
 * blkdev_mmap_prepare - mmap을 위한 block device 준비
 *
 * read-only 장치는 read-only mmap, 그 외는 일반 mmap.
 * NVMe 연결: mmap된 페이지에 대한 read/write fault는 페이지 캐시를
 *            통해 간접적으로 NVMe I/O로 이어진다.
 */
static int blkdev_mmap_prepare(struct vm_area_desc *desc)
{
	struct file *file = desc->file;

	if (bdev_read_only(I_BDEV(bdev_file_inode(file))))
		return generic_file_readonly_mmap_prepare(desc);

	return generic_file_mmap_prepare(desc);
}

/*
 * def_blk_fops - 블록 장치용 struct file_operations
 *
 * VFS가 이 구조체를 통해 block layer로 진입한다.
 * NVMe 관련 연결:
 *   - .read_iter / .write_iter: 위에서 설명한 bio -> NVMe 경로
 *   - .iopoll: iocb_bio_iopoll -> blk_poll -> NVMe polled CQ
 *   - .fsync: blkdev_issue_flush -> NVMe Flush
 *   - .fallocate: NVMe Write Zeroes / Deallocate
 *   - .uring_cmd: blkdev_uring_cmd -> NVMe passthrough I/O
 */
const struct file_operations def_blk_fops = {
	.open		= blkdev_open, /* NVMe namespace 열기 */
	.release	= blkdev_release, /* NVMe namespace 닫기 */
	.llseek		= blkdev_llseek, /* NVMe LBA 범위 내 seek */
	.read_iter	= blkdev_read_iter, /* VFS read -> blkdev_direct_IO / filemap_read -> NVMe Read */
	.write_iter	= blkdev_write_iter, /* VFS write -> blkdev_direct_IO / buffered write -> NVMe Write */
	.iopoll		= iocb_bio_iopoll, /* IOCB_HIPRI 폴 CQ 경로 */
	.mmap_prepare	= blkdev_mmap_prepare, /* mmap 페이지 fault는 페이지 캐시를 경유 */
	.fsync		= blkdev_fsync, /* NVMe Flush 명령 제출 */
	.unlocked_ioctl	= blkdev_ioctl, /* block device ioctl */
#ifdef CONFIG_COMPAT
	.compat_ioctl	= compat_blkdev_ioctl, /* compat ioctl */
#endif
	.splice_read	= filemap_splice_read, /* splice read */
	.splice_write	= iter_file_splice_write, /* splice write */
	.fallocate	= blkdev_fallocate, /* NVMe Write Zeroes / Deallocate */
	.uring_cmd	= blkdev_uring_cmd, /* NVMe passthrough / uring cmd */
	.fop_flags	= FOP_BUFFER_RASYNC, /* async buffered read 플래그 */
};

/*
 * blkdev_init - blkdev_dio_pool 초기화
 *
 * direct I/O에서 사용하는 bio_set을 초기화. NVMe I/O를 처리할 bio
 * 객체가 이 풀에서 할당되며, bio가 곧바로 nvme_queue_rq의 입력이 된다.
 */
static __init int blkdev_init(void)
{
	return bioset_init(&blkdev_dio_pool, 4,
				offsetof(struct blkdev_dio, bio), /* dio 구조체 내 bio 오프셋 지정 */
				BIOSET_NEED_BVECS|BIOSET_PERCPU_CACHE);
}
module_init(blkdev_init);

/* NVMe 관점 핵심 요약 */
/*
 * - block/fops.c는 VFS 요청을 bio 단위로 조립하여 blk-mq가 NVMe SQ/CQ를
 *   통해 처리할 수 있도록 준비하는 마지막 공통 계층이다.
 * - direct I/O 경로에서는 kiocb -> bio -> blk_mq_submit_bio ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell) 순으로 NVMe 명령이 생성된다.
 * - 분할 bio/IOCB_HIPRI/IOCB_NOWAIT/IOCB_ATOMIC 등의 플래그는 NVMe
 *   폴 CQ, non-blocking, atomic write, FUA 등의 하드웨어 특성과 직결된다.
 * - fsync/fallocate/write-zeroes는 NVMe Flush, Dataset Management,
 *   Write Zeroes 명령으로 변환될 수 있다.
 * - buffered I/O는 페이지 캐시를 거쳐 writeback/readahead 단계에서
 *   NVMe Read/Write로 전환된다.
 */
