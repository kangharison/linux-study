// SPDX-License-Identifier: GPL-2.0
/*
 * block/blk-map.c: 사용자/커널 버퍼를 request/bio에 매핑하는 블록 계층 헬퍼 모음.
 *
 * NVMe SSD 관점에서 본 파일은 상위 계층(파일 시스템, SG_IO, ioctl)이 넘긴
 * 데이터 버퍼를 DMA/PRP/SGL 형태로 변환할 수 있도록 bio를 준비하는 단계이다.
 * 완성된 request는 blk-mq를 거쳐 하드웨어 큐로 전달되며, NVMe 드라이버는
 * 최종적으로 SQ(Submission Queue)에 CID를 할당하고 doorbell을 갱신한다.
 * call chain: submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *             -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 * 관련 파일: block/blk-merge.c(bio 병합/분할), block/blk-mq.c(request 할당/완료)
 */
#include <linux/kernel.h>
#include <linux/sched/task_stack.h>
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/uio.h>

#include "blk.h"

/*
 * bio_map_data: NVMe passthrough 요청에서 사용자 버퍼를 복사/관리할 때 쓰이는
 *   메타데이터 구조체. bio->bi_private에 연결되며, 완료 시 사용자 공간으로의
 *   복사 여부나 페이지 회수 방식을 결정한다. 이러한 플래그들은 NVMe 컨트롤러가
 *   사용할 PRP/SGL 목록이 커널 할당 페이지인지, 원본 사용자 페이지인지를
 *   구분하는 데 참조된다.
 */
struct bio_map_data {
	bool is_our_pages : 1; // true면 block layer가 할당한 bounce page; NVMe PRP/SGL은 이 커널 페이지를 가리킨다.
	bool is_null_mapped : 1; // 데이터가 없는 passthrough(null mapped); NVMe 명령은 PRP/SGL 없이 CID만 SQ에 삽입된다(예: Flush, Write Zeroes)(추정).
	struct iov_iter iter; // 원본 사용자 iov_iter; READ 완료 후 CQ -> bio_uncopy_user에서 사용자 공간 복사에 사용.
	struct iovec iov[]; // 짧은 수명의 caller iovec 복사본; 각 항목은 NVMe PRP entry 후보가 되는 버퍼 조각.
};

/*
 * bio_alloc_map_data - iov_iter를 복사하기 위한 bio_map_data 할당
 * @data: 원본 iov_iter
 * @gfp_mask: 메모리 할당 플래그
 *
 * 목적:
 *   사용자가 전달한 iovec을 안전하게 보관할 메타데이터를 할당한다.
 *   호출 경로:
 *     blk_rq_map_user_iov -> bio_copy_user_iov -> bio_alloc_map_data
 *   NVMe 연결:
 *     이 메타데이터는 나중에 request에 연결된 bio->bi_private로 남아,
 *     nvme_completion 이후 blk_rq_unmap_user -> bio_uncopy_user에서
 *     사용자 공간으로 결과를 복사하거나 커널 페이지를 회수하는 데 쓰인다.
 */
static struct bio_map_data *bio_alloc_map_data(struct iov_iter *data,
					       gfp_t gfp_mask)
{
	struct bio_map_data *bmd; /* bio->bi_private로 붙어 NVMe CQ 완료 후 사용자 공간 복사/페이지 회수에 재사용됨 */

	if (data->nr_segs > UIO_MAXIOV) /* 사용자가 넘긴 iovec이 너무 많으면 NVMe SGL/PRP 제한을 초과할 수 있어 거부 */
		return NULL;

	bmd = kmalloc_flex(*bmd, iov, data->nr_segs, gfp_mask); /* bio_map_data + iov[] 가변 길이 할당; 이 메모리는 NVMe I/O 완료 시까지 생존 */
	if (!bmd) /* 메모리 부족 시 NVMe SQ에 CID 할당도, doorbell 갱신도 일어나지 않음 */
		return NULL;
	bmd->iter = *data; /* 원본 iov_iter를 복사해 놓아 NVMe READ 완료 후 사용자 공간으로 데이터를 되돌릴 수 있게 함 */
	if (iter_is_iovec(data)) { /* iovec 반복자인 경우에만 복사본 저장 */
		memcpy(bmd->iov, iter_iov(data), sizeof(struct iovec) * data->nr_segs); /* 짧은 수명의 caller iovec을 커널 힙으로 복사; NVMe PRP/SGL 후보 목록을 안전하게 보관 */
		bmd->iter.__iov = bmd->iov; /* 복사된 커널 내 iov 배열을 가리키도록 교체 */
	}
	return bmd; /* 이 포인터는 이후 bio->bi_private에 저장되어 NVMe completion path로 전달됨 */
}

/*
 * blk_mq_map_bio_put - 매핑 중 실패하거나 완료된 bio를 해제
 * @bio: 해제할 bio
 *
 * 목적:
 *   bio 참조 카운트를 감소시켜 bio를 반환한다.
 *   호출 경로(예):
 *     blk_rq_map_kern 실패 시, blk_rq_unmap_user 마지막 단계 등
 *   NVMe 연결:
 *     bio가 SQ에 들어가기 전에 매핑에 실패하면 doorbell 없이 bio만 정리된다.
 */
static inline void blk_mq_map_bio_put(struct bio *bio)
{
	bio_put(bio); /* bio 참조 카운트 감소; 0이 되면 fs_bio_set mempool으로 반환되어 NVMe SQ 제출 전 실패 경로에서 재사용 가능 */
}

/*
 * blk_rq_map_bio_alloc - request의 cmd_flags를 상속받아 bio 할당
 * @rq: 대상 request
 * @nr_vecs: 할당할 최대 bio_vec 개수
 * @gfp_mask: 메모리 할당 플래그
 *
 * 목적:
 *   passthrough 요청용 bio를 생성하며 rq->cmd_flags(읽기/쓰기 등)를 복사한다.
 *   호출 경로:
 *     bio_copy_user_iov / bio_map_user_iov / bio_map_kern / bio_copy_kern 등
 *   NVMe 연결:
 *     bio의 cmd_flags는 NVMe 명령의 opcode 방향(READ/WRITE)을 반영하며,
 *     이 bio가 nvme_queue_rq에서 PRP/SGL로 변환될 때 사용된다.
 */
static struct bio *blk_rq_map_bio_alloc(struct request *rq,
		unsigned int nr_vecs, gfp_t gfp_mask)
{
	struct block_device *bdev = rq->q->disk ? rq->q->disk->part0 : NULL; /* request가 속한 gendisk에서 block_device를 가져옴; NVMe는 nvme0n1 등의 디스크에 대응 */
	struct bio *bio;

	bio = bio_alloc_bioset(bdev, nr_vecs, rq->cmd_flags, gfp_mask, /* cmd_flags에 NVMe READ/WRITE 방향이 담김 */
				&fs_bio_set); /* fs_bio_set mempool에서 bio 할당; NVMe multi-queue 경쟁 상황에서도 메모리 고갈 방지 */
	if (!bio) /* bio 할당 실패는 NVMe SQ doorbell 이전에 처리되므로 하드웨어 큐 상태는 변하지 않음 */
		return NULL;

	return bio; /* 할당된 bio는 이후 PRP/SGL로 변환될 page/offset/len 후보를 담게 됨 */
}

/**
 * bio_copy_from_iter - copy all pages from iov_iter to bio
 * @bio: The &struct bio which describes the I/O as destination
 * @iter: iov_iter as source
 *
 * Copy all pages from iov_iter to bio.
 * Returns 0 on success, or error on failure.
 */
/*
 * bio_copy_from_iter - 사용자 공간 데이터를 bio의 커널 페이지로 복사
 * @bio: 복사 대상 bio
 * @iter: 사용자 공간 소스 iov_iter
 *
 * 목적:
 *   bounce buffer 방식으로 사용자 데이터를 커널 페이지에 복사한다.
 *   호출 경로:
 *     blk_rq_map_user_iov -> bio_copy_user_iov -> bio_copy_from_iter
 *   NVMe 연결:
 *     쓰기(WRITE) 경로에서 NVMe SQ에 제출되기 전에 데이터가 커널 메모리에
 *     존재해야 하므로, 이 단계에서 복사가 완료되어야 nvme_submit_cmd 전에
 *     DMA 매핑이 안정적이다.
 */
static int bio_copy_from_iter(struct bio *bio, struct iov_iter *iter)
{
	struct bio_vec *bvec; /* 현재 처리 중인 bio_vec; NVMe PRP/SGL의 한 entry에 대응하는 page/offset/len */
	struct bvec_iter_all iter_all; /* bio의 모든 bvec을 순회하기 위한 내부 iterator */

	bio_for_each_segment_all(bvec, bio, iter_all) { /* bio의 모든 segment를 순회하며 사용자 데이터를 페이지에 복사 */
		ssize_t ret;

		ret = copy_page_from_iter(bvec->bv_page, /* 커널 bounce page에 사용자 데이터를 복사; 이 페이지가 NVMe DMA/PRP 소스가 됨 */
					  bvec->bv_offset,
					  bvec->bv_len,
					  iter);

		if (!iov_iter_count(iter)) /* 사용자 공간에 남은 데이터가 없으면 복사 종료; NVMe WRITE 데이터 준비 완료 */
			break;

		if (ret < bvec->bv_len) /* 요청보다 적게 복사되면 -EFAULT; NVMe SQ 제출 실패, abort 없이 상위로 오류 반환 */
			return -EFAULT;
	}

	return 0; /* 모든 segment 복사 성공; 이제 bio는 nvme_queue_rq에서 DMA 매핑 가능한 상태 */
}

/**
 * bio_copy_to_iter - copy all pages from bio to iov_iter
 * @bio: The &struct bio which describes the I/O as source
 * @iter: iov_iter as destination
 *
 * Copy all pages from bio to iov_iter.
 * Returns 0 on success, or error on failure.
 */
/*
 * bio_copy_to_iter - bio의 커널 페이지 데이터를 사용자 공간으로 복사
 * @bio: 소스 bio
 * @iter: 사용자 공간 대상 iov_iter
 *
 * 목적:
 *   READ 완료 후 bounce buffer에 있는 데이터를 사용자 버퍼로 복사한다.
 *   호출 경로:
 *     nvme_completion -> blk_mq_end_request -> blk_rq_unmap_user
 *                       -> bio_uncopy_user -> bio_copy_to_iter
 *   NVMe 연결:
 *     NVMe 컨트롤러가 CQ에 완료 정보를 기록한 후, 상위 계층으로 데이터를
 *     전달하기 전에 수행된다.
 */
static int bio_copy_to_iter(struct bio *bio, struct iov_iter iter)
{
	struct bio_vec *bvec; /* NVMe READ로부터 채워진 커널 bounce page를 가리키는 bio_vec */
	struct bvec_iter_all iter_all; /* 모든 bvec 순회용 내부 상태 */

	bio_for_each_segment_all(bvec, bio, iter_all) { /* NVMe CQ entry 처리 후 사용자 공간으로 결과를 전달하는 루프 */
		ssize_t ret;

		ret = copy_page_to_iter(bvec->bv_page, /* bio 페이지 -> 사용자 공간 복사; NVMe READ 완료 후 데이터 전달 */
					bvec->bv_offset,
					bvec->bv_len,
					&iter);

		if (!iov_iter_count(&iter)) /* 사용자 공간에 모두 복사되면 종료; NVMe READ 결과가 상위 계층에 도달 완료 */
			break;

		if (ret < bvec->bv_len) /* READ 완료 후 사용자 공간 복사 실패; 상위 SG_IO는 -EFAULT 수신 */
			return -EFAULT;
	}

	return 0; /* NVMe READ 결과가 사용자 버퍼로 모두 전달됨; 이제 bio_map_data 해제 가능 */
}

/**
 *	bio_uncopy_user	-	finish previously mapped bio
 *	@bio: bio being terminated
 *
 *	Free pages allocated from bio_copy_user_iov() and write back data
 *	to user space in case of a read.
 */
/*
 * bio_uncopy_user - bio_map_data가 붙은 bio의 매핑을 해제
 * @bio: 종료 중인 bio
 *
 * 목적:
 *   bounce buffer 페이지를 사용자 공간에 복사하고(READ) 커널 페이지를 회수한다.
 *   호출 경로:
 *     blk_rq_unmap_user -> bio_uncopy_user
 *   NVMe 연결:
 *     NVMe I/O 완료(CQ entry 처리) 후 request 해제 단계에서 실행되며,
 *     READ 결과가 사용자 공간에 복사되고 PRP/SGL에 쓰인 커널 페이지가 반납된다.
 */
static int bio_uncopy_user(struct bio *bio)
{
	struct bio_map_data *bmd = bio->bi_private; /* bio_copy_user_iov에서 저장한 메타데이터; NVMe CQ 완료 후 복사/해제 정책 결정 */
	int ret = 0;

	if (!bmd->is_null_mapped) { /* null mapped 명령은 복사/해제 대상이 아님; NVMe Flush/Write Zeroes 등 데이터 없는 명령 */
		/*
		 * if we're in a workqueue, the request is orphaned, so
		 * don't copy into a random user address space, just free
		 * and return -EINTR so user space doesn't expect any data.
		 */
		if (!current->mm) /* workqueue 등에서 current->mm이 없으면 사용자 복사 불가; NVMe READ 결과도 폐기 */
			ret = -EINTR;
		else if (bio_data_dir(bio) == READ) /* READ면 NVMe CQ 완료 후 사용자 공간으로 결과 복사 */
			ret = bio_copy_to_iter(bio, bmd->iter);
		if (bmd->is_our_pages) /* block layer가 할당한 bounce 페이지라면 해제 필요; NVMe PRP/SGL에 쓰인 커널 메모리 반납 */
			bio_free_pages(bio);
	}
	kfree(bmd); /* bio_map_data 메모리 반환; 해당 NVMe CID에 대응하는 매핑 컨텍스트 종료 */
	return ret;
}

/*
 * bio_copy_user_iov - 사용자 버퍼를 bounce buffer bio로 구성
 * @rq: 대상 request
 * @map_data: 미리 준비된 페이지 풀(rq_map_data)
 * @iter: 사용자 버퍼 기술자
 * @gfp_mask: 메모리 할당 플래그
 *
 * 목적:
 *   DMA 정렬/가상 경계 등의 제약으로 직접 매핑할 수 없는 사용자 버퍼를
 *   커널 페이지에 복사하여 bio를 만든다.
 *   호출 경로:
 *     blk_rq_map_user_iov -> bio_copy_user_iov
 *                           -> blk_rq_map_bio_alloc -> bio_copy_from_iter
 *                           -> blk_rq_append_bio
 *   NVMe 연결:
 *     NVMe PRP는 페이지 경계를 따라야 하므로(추정), 정렬되지 않은 사용자
 *     버퍼는 이 함수에서 커널 페이지로 정리된 후 PRP list/SGL로 변환된다.
 */
static int bio_copy_user_iov(struct request *rq, struct rq_map_data *map_data,
		struct iov_iter *iter, gfp_t gfp_mask)
{
	struct bio_map_data *bmd; /* NVMe READ 완료 후 사용자 공간 복사 여부를 결정하는 메타데이터 */
	struct page *page; /* bio에 추가될 페이지; NVMe PRP/SGL entry의 물리 페이지 후보 */
	struct bio *bio; /* 조립 중인 bio; 최종적으로 request에 붙여 blk_mq -> NVMe SQ로 전달 */
	int i = 0, ret; /* i: map_data 풀 내 페이지 인덱스; ret: NVMe 명령 생성 성공/실패 상태 */
	int nr_pages; /* 필요한 페이지/segment 수; NVMe PRP entry 개수 상한을 초과하지 않도록 조절 */
	unsigned int len = iter->count; /* 전송할 총 바이트 수; NVMe 명령의 데이터 길이(NLBA*sector_size)가 됨 */
	unsigned int offset = map_data ? offset_in_page(map_data->offset) : 0; /* map_data가 주어지면 페이지 내 오프셋 사용; NVMe PRP는 페이지 정렬 선호(추정) */

	bmd = bio_alloc_map_data(iter, gfp_mask); /* 메타데이터 할당 실패 시 -ENOMEM */
	if (!bmd) /* 메모리 부족으로 NVMe SQ doorbell 이전에 실패; 하드웨어 큐 상태는 변함없음 */
		return -ENOMEM;

	/*
	 * We need to do a deep copy of the iov_iter including the iovecs.
	 * The caller provided iov might point to an on-stack or otherwise
	 * shortlived one.
	 */
	bmd->is_our_pages = !map_data; /* map_data가 없을 때만 페이지 소유권을 가짐; 소유권에 따라 완료 시 페이지 회수 여부 결정 */
	bmd->is_null_mapped = (map_data && map_data->null_mapped); /* null mapped 명령(예: NVMe Flush/Write Zeroes) 플래그 기록; 데이터 길이 0으로 SQ에 삽입 */

	nr_pages = bio_max_segs(DIV_ROUND_UP(offset + len, PAGE_SIZE)); /* 필요한 최대 segment 수; NVMe PRP/SGL entry 수 상한과 관련 */

	ret = -ENOMEM;
	bio = blk_rq_map_bio_alloc(rq, nr_pages, gfp_mask); /* rq->cmd_flags를 상속받아 NVMe READ/WRITE 방향이 설정된 bio 할당 */
	if (!bio)
		goto out_bmd;

	if (map_data) { /* 미리 할당된 페이지 풀(map_data)을 사용하는 경우; SG_IO에서 미리 잡아 둔 페이지 활용 */
		nr_pages = 1U << map_data->page_order; /* page_order에 따른 페이지 묶음 크기 */
		i = map_data->offset / PAGE_SIZE; /* offset에서 시작 페이지 인덱스 계산 */
	}
	while (len) { /* 남은 데이터를 페이지 단위로 분할하여 bio에 추가; 각 반복은 잠재적 NVMe PRP entry 하나를 생성 */
		unsigned int bytes = PAGE_SIZE;

		bytes -= offset; /* 현재 페이지의 시작 오프셋만큼 usable byte 감소 */

		if (bytes > len) /* 남은 길이보다 크면 맞춤; NVMe 명령 길이와 정확히 일치 */
			bytes = len;

		if (map_data) {
			if (i == map_data->nr_entries * nr_pages) { /* 페이지 풀의 끝에 도달하면 실패 */
				ret = -ENOMEM;
				goto cleanup; /* NVMe SQ 제출 실패; 이미 추가된 페이지들만 cleanup에서 정리 */
			}

			page = map_data->pages[i / nr_pages]; /* map_data 페이지 배열에서 해당 페이지 획득 */
			page += (i % nr_pages); /* page_order 묶음 내 페이지 오프셋 적용 */

			i++;
		} else {
			page = alloc_page(GFP_NOIO | gfp_mask); /* bounce buffer용 커널 페이지 할당; NVMe DMA에 사용할 커널 메모리 */
			if (!page) {
				ret = -ENOMEM;
				goto cleanup;
			}
		}

		if (bio_add_page(bio, page, bytes, offset) < bytes) { /* bio에 페이지/오프셋/길이 추가; NVMe PRP entry 후보 */
			if (!map_data) /* 추가 실패 시 직접 할당한 페이지만 해제; map_data 페이지는 caller가 관리 */
				__free_page(page);
			break; /* bio capacity 초과; 이후 상위에서 분할/복사로 폴백 */
		}

		len -= bytes; /* 남은 길이 감소, 이후 페이지는 오프셋 0부터 시작 */
		offset = 0;
	}

	if (map_data)
		map_data->offset += bio->bi_iter.bi_size; /* map_data 사용 시 다음 offset 갱신; 연속된 SG_IO 요청 간 상태 유지 */

	/*
	 * success
	 */
	if (iov_iter_rw(iter) == WRITE && /* WRITE이고 null mapped가 아니면 사용자 데이터를 bio로 복사 */
	     (!map_data || !map_data->null_mapped)) {
		ret = bio_copy_from_iter(bio, iter); /* NVMe WRITE: SQ 제출 전 데이터를 커널 페이지에 준비; DMA 일관성 확보 */
		if (ret)
			goto cleanup;
	} else if (map_data && map_data->from_user) { /* SG_DXFER_TO_FROM_DEV 양방향 복사 처리 */
		struct iov_iter iter2 = *iter;

		/* This is the copy-in part of SG_DXFER_TO_FROM_DEV. */
		iter2.data_source = ITER_SOURCE; /* 데이터 소스로 설정하여 bio로 복사 */
		ret = bio_copy_from_iter(bio, &iter2);
		if (ret)
			goto cleanup;
	} else {
		if (bmd->is_our_pages) /* READ가 아닌데 bounce 페이지가 있으면 0으로 채움(관리용 명령 등) */
			zero_fill_bio(bio);
		iov_iter_advance(iter, bio->bi_iter.bi_size); /* 사용자 반복자를 진행시켜 이미 처리한 길이만큼 걸너뜀 */
	}

	bio->bi_private = bmd; /* bio와 bio_map_data 연결; NVMe CQ 수신 후 bio_uncopy_user가 복사/해제 수행 */

	ret = blk_rq_append_bio(rq, bio); /* 구성된 bio를 request에 연결; 이후 blk_mq 제출 단계로 진행 */
	if (ret)
		goto cleanup;
	return 0; /* bio가 request에 성공적으로 연결; 이제 blk_mq_get_request -> nvme_queue_rq 가능 */
cleanup:
	if (!map_data) /* 실패 시 직접 할당한 bounce 페이지 해제 */
		bio_free_pages(bio);
	blk_mq_map_bio_put(bio); /* bio 참조 해제; NVMe SQ에는 doorbell 없음 */
out_bmd:
	kfree(bmd); /* bio_map_data 메모리 반환; NVMe CID와 매핑 컨텍스트가 소멸 */
	return ret;
}

/*
 * bio_map_user_iov - 사용자 페이지를 직접 bio에 매핑(제로 카피)
 * @rq: 대상 request
 * @iter: 사용자 버퍼 기술자
 * @gfp_mask: 메모리 할당 플래그
 *
 * 목적:
 *   정렬/제약을 만족하는 사용자 페이지를 pin 하여 bio에 직접 연결한다.
 *   호출 경로:
 *     blk_rq_map_user_iov -> bio_map_user_iov
 *                           -> blk_rq_map_bio_alloc -> bio_iov_iter_get_pages
 *                           -> blk_rq_append_bio
 *   NVMe 연결:
 *     이 bio의 페이지들은 nvme_queue_rq에서 DMA 매핑되어 PRP entry나 SGL
 *     segment로 사용된다. 사용자 버퍼가 물리적으로 불연속이면 PRP list로,
 *     SGL을 지원하면 SGL로 기술될 수 있다(추정).
 */
static int bio_map_user_iov(struct request *rq, struct iov_iter *iter,
		gfp_t gfp_mask)
{
	unsigned int nr_vecs = iov_iter_npages(iter, BIO_MAX_VECS); /* 사용자 버퍼를 구성하는 페이지 수; BIO_MAX_VECS로 제한; NVMe PRP entry 수 상한과 연관 */
	struct bio *bio;
	int ret;

	if (!iov_iter_count(iter)) /* 전송할 데이터가 없으면 오류; NVMe 컨트롤러는 데이터 길이 0이 아닌 이상 명령 거부 */
		return -EINVAL;

	bio = blk_rq_map_bio_alloc(rq, nr_vecs, gfp_mask); /* 제로 카피 bio 할당; 사용자 페이지를 pin 한 뒤 PRP/SGL로 직접 변환 */
	if (!bio)
		return -ENOMEM;
	/*
	 * No alignment requirements on our part to support arbitrary
	 * passthrough commands.
	 */
	ret = bio_iov_iter_get_pages(bio, iter, 0); /* 사용자 페이지를 pin 하여 bio에 직접 연결; 이 페이지들이 NVMe DMA 매핑 대상 */
	if (ret)
		goto out_put;
	ret = blk_rq_append_bio(rq, bio); /* bio를 request에 추가; NVMe PRP/SGL 변환 준비 완료 */
	if (ret)
		goto out_release;
	return 0; /* 제로 카피 성공; NVMe SQ 제출 전 DMA 매핑만 남음 */

out_release:
	bio_release_pages(bio, false); /* 매핑 실패 시 pin 한 사용자 페이지 해제; NVMe 명령은 SQ에 도달하지 않음 */
out_put:
	blk_mq_map_bio_put(bio); /* bio 반환; NVMe doorbell은 갱신되지 않음 */
	return ret;
}

/*
 * bio_invalidate_vmalloc_pages - vmalloc 영역의 캐시 일관성 정리
 * @bio: vmalloc 주소를 담은 bio
 *
 * 목적:
 *   ARCH_IMPLEMENTS_FLUSH_KERNEL_VMAP_RANGE가 정의된 아키텍처에서
 *   vmalloc로 매핑된 커널 버퍼의 캐시를 무효화한다.
 *   호출 경로:
 *     bio_map_kern_endio -> bio_invalidate_vmalloc_pages
 *   NVMe 연결:
 *     NVMe DMA가 vmalloc 주소를 사용할 때(추정), I/O 완료 후 CPU가 해당
 *     영역을 다시 읽기 전에 캐시 일관성을 맞춘다.
 */
static void bio_invalidate_vmalloc_pages(struct bio *bio)
{
#ifdef ARCH_IMPLEMENTS_FLUSH_KERNEL_VMAP_RANGE
	if (bio->bi_private && !op_is_write(bio_op(bio))) { /* vmalloc DMA 사용 아키텍처에서만 캐시 무효화(추정); NVMe READ 완료 후 CPU가 최신 데이터를 볼 수 있도록 */
		unsigned long i, len = 0; /* i: bvec 인덱스; len: 무효화할 총 바이트; NVMe READ 데이터 길이와 일치 */

		for (i = 0; i < bio->bi_vcnt; i++) /* bio의 모든 vec 길이 합산; 각 vec은 NVMe PRP/SGL로 변환된 버퍼 조각 */
			len += bio->bi_io_vec[i].bv_len;
		invalidate_kernel_vmap_range(bio->bi_private, len); /* vmalloc 영역 캐시 무효화; NVMe DMA가 쓴 데이터를 CPU 캐시에 반영 */
	}
#endif
}

/*
 * bio_map_kern_endio - bio_map_kern에서 생성된 bio의 완료 처리
 * @bio: 완료된 bio
 *
 * 목적:
 *   vmalloc 버퍼 캐시 정리 후 bio를 반환한다.
 *   호출 경로:
 *     I/O 완료 -> bio->bi_end_io(bio_map_kern_endio)
 *   NVMe 연결:
 *     NVMe CQ 처리 후 request 완료 콜백에서 호출되며, DMA 매핑 해제 전
 *     후처리를 수행한다.
 */
static void bio_map_kern_endio(struct bio *bio)
{
	bio_invalidate_vmalloc_pages(bio); /* NVMe READ 완료 후 vmalloc 캐시 일관성 정리(추정) */
	blk_mq_map_bio_put(bio); /* bio 참조 해제; NVMe CID에 연결된 커널 버퍼 매핑 종료 */
}

/*
 * bio_map_kern - 커널 가상 주소를 bio에 직접 매핑
 * @rq: 대상 request
 * @data: 커널 버퍼
 * @len: 길이
 * @gfp_mask: 메모리 할당 플래그
 *
 * 목적:
 *   물리적으로 연속적이거나 vmalloc인 커널 버퍼를 bio로 만든다.
 *   호출 경로:
 *     blk_rq_map_kern -> bio_map_kern -> blk_rq_append_bio
 *   NVMe 연결:
 *     커널에서 발행하는 admin/IO passthrough 명령의 데이터 버퍼를
 *     PRP/SGL로 연결하기 위해 사용된다.
 */
static struct bio *bio_map_kern(struct request *rq, void *data, unsigned int len,
		gfp_t gfp_mask)
{
	unsigned int nr_vecs = bio_add_max_vecs(data, len); /* 커널 버퍼에 필요한 최대 bio_vec 개수 추정; NVMe PRP/SGL entry 수 상한 */
	struct bio *bio;

	bio = blk_rq_map_bio_alloc(rq, nr_vecs, gfp_mask); /* NVMe READ/WRITE 방향이 설정된 bio 할당 */
	if (!bio)
		return ERR_PTR(-ENOMEM); /* 메모리 부족; NVMe SQ doorbell 이전에 실패 */

	if (is_vmalloc_addr(data)) { /* vmalloc 주소는 별도의 vmap 처리 필요; NVMe DMA가 vmalloc 영역을 사용할 때(추정) */
		bio->bi_private = data; /* vmalloc 기준 주소를 bi_private에 보관; 완료 시 캐시 무효화에 사용 */
		if (!bio_add_vmalloc(bio, data, len)) { /* vmalloc 페이지를 bio에 추가; NVMe PRP/SGL용 페이지 목록 구성 */
			blk_mq_map_bio_put(bio);
			return ERR_PTR(-EINVAL); /* vmalloc bio 구성 실패; NVMe SQ에 도달하지 않음 */
		}
	} else {
		bio_add_virt_nofail(bio, data, len); /* 물리적으로 연속/고정된 커널 버퍼는 직접 추가; NVMe PRP entry로 직접 사용 가능(추정) */
	}
	bio->bi_end_io = bio_map_kern_endio; /* I/O 완료 시 bio_map_kern_endio 호출; NVMe CQ 처리 후 vmalloc 캐시 정리 및 bio 반환 */
	return bio; /* 커널 버퍼가 담긴 bio; 이후 blk_rq_append_bio -> blk_mq -> nvme_queue_rq */
}

/*
 * bio_copy_kern_endio - bio_copy_kern에서 할당한 페이지 회수
 * @bio: 완료된 bio
 *
 * 목적:
 *   bounce buffer로 사용된 커널 페이지를 해제하고 bio를 반환한다.
 *   호출 경로:
 *     I/O 완료 -> bio->bi_end_io
 *   NVMe 연결:
 *     NVMe 명령이 완료된 후 해당 CID의 데이터 버퍼 페이지를 정리한다.
 */
static void bio_copy_kern_endio(struct bio *bio)
{
	bio_free_pages(bio); /* NVMe 명령 완료 후 bounce buffer 커널 페이지 회수; PRP/SGL에 쓰인 임시 메모리 반납 */
	blk_mq_map_bio_put(bio); /* bio 반환; 해당 NVMe CID의 생명 주기 종료 */
}

/*
 * bio_copy_kern_endio_read - READ용 bounce buffer 결과를 원래 커널 버퍼로 복사
 * @bio: 완료된 bio
 *
 * 목적:
 *   bio_copy_kern이 할당한 페이지에서 원래 커널 버퍼로 데이터를 복사한다.
 *   호출 경로:
 *     I/O 완료 -> bio_copy_kern_endio_read -> bio_copy_kern_endio
 *   NVMe 연결:
 *     NVMe READ 완료(CQ) 후 커널 호출자가 요청한 kbuf에 결과를 돌려준다.
 */
static void bio_copy_kern_endio_read(struct bio *bio)
{
	char *p = bio->bi_private; /* 원래 커널 버퍼의 시작 주소; NVMe READ 결과를 돌려줄 대상 */
	struct bio_vec *bvec; /* NVMe READ로 채워진 bounce buffer 페이지 조각 */
	struct bvec_iter_all iter_all; /* 모든 bvec 순회용 */

	bio_for_each_segment_all(bvec, bio, iter_all) { /* NVMe CQ 완료 후 bounce buffer에서 원래 kbuf로 복사 */
		memcpy_from_bvec(p, bvec); /* bvec의 page/offset/len에서 원래 커널 버퍼로 복사 */
		p += bvec->bv_len; /* 다음 대상 위치로 이동; NVMe READ 데이터의 연속적인 복원 */
	}

	bio_copy_kern_endio(bio); /* bounce 페이지 해제 및 bio 반환 */
}

/**
 *	bio_copy_kern	-	copy kernel address into bio
 *	@rq: request to fill
 *	@data: pointer to buffer to copy
 *	@len: length in bytes
 *	@op: bio/request operation
 *	@gfp_mask: allocation flags for bio and page allocation
 *
 *	copy the kernel address into a bio suitable for io to a block
 *	device. Returns an error pointer in case of error.
 */
/*
 * bio_copy_kern - 정렬되지 않은 커널 버퍼를 bounce buffer bio로 복사
 * @rq: 대상 request
 * @data: 커널 버퍼
 * @len: 길이
 * @gfp_mask: 메모리 할당 플래그
 *
 * 목적:
 *   스택 버퍼 또는 DMA 정렬/연속 조건을 만족하지 않는 커널 버퍼를 위해
 *   새 커널 페이지를 할당해 복사한다.
 *   호출 경로:
 *     blk_rq_map_kern -> bio_copy_kern -> blk_rq_append_bio
 *   NVMe 연결:
 *     NVMe PRP는 보통 페이지 정렬된 물리 주소가 필요하므로(추정), 이 함수를
 *     통해 적합한 페이지로 정리한 뒤 PRP/SGL로 연결한다.
 */
static struct bio *bio_copy_kern(struct request *rq, void *data, unsigned int len,
		gfp_t gfp_mask)
{
	enum req_op op = req_op(rq); /* request의 operation(NVMe opcode 방향) 획득; READ/WRITE/FLUSH 등 */
	unsigned long kaddr = (unsigned long)data; /* 커널 버퍼 가상 주소; DMA 정렬 검사에 사용 */
	unsigned long end = (kaddr + len + PAGE_SIZE - 1) >> PAGE_SHIFT; /* 버퍼 끝이 속한 페이지 인덱스(올림) */
	unsigned long start = kaddr >> PAGE_SHIFT; /* 버퍼 시작이 속한 페이지 인덱스(내림) */
	struct bio *bio;
	void *p = data; /* 원본 커널 버퍼 내 현재 복사 위치; NVMe WRITE 시 bounce page 채우기용 */
	int nr_pages = 0; /* 필요한 페이지 수; NVMe PRP entry 수 상한 */

	/*
	 * Overflow, abort
	 */
	if (end < start) /* 오버플로우 시 잘못된 길이; NVMe 컨트롤러에 잘못된 명령 길이를 복사하지 않도록 방어 */
		return ERR_PTR(-EINVAL);

	nr_pages = end - start; /* 필요한 페이지 수; NVMe PRP entry 수 상한 */
	bio = blk_rq_map_bio_alloc(rq, nr_pages, gfp_mask); /* NVMe READ/WRITE 방향이 설정된 bio 할당 */
	if (!bio)
		return ERR_PTR(-ENOMEM); /* 메모리 부족; NVMe SQ doorbell 이전에 실패 */

	while (len) { /* 남은 데이터를 페이지 단위로 bounce buffer에 복사; 각 페이지는 NVMe PRP/SGL 후보 */
		struct page *page;
		unsigned int bytes = PAGE_SIZE;

		if (bytes > len)
			bytes = len; /* 마지막 페이지는 실제 남은 길이만큼만 사용; NVMe 명령 길이와 정확히 일치 */

		page = alloc_page(GFP_NOIO | __GFP_ZERO | gfp_mask); /* 쓰기면 0으로 초기화된 커널 페이지 할당; NVMe DMA에 안전한 메모리 */
		if (!page)
			goto cleanup; /* 메모리 부족; 이미 할당된 bounce 페이지들은 cleanup에서 해제 */

		if (op_is_write(op)) /* 쓰기면 원본 커널 버퍼를 페이지에 복사; NVMe SQ 제출 전 데이터가 커널 메모리에 존재해야 함 */
			memcpy(page_address(page), p, bytes);

		__bio_add_page(bio, page, bytes, 0); /* 커널 페이지를 bio에 추가; PRP/SGL용 페이지 후보 */

		len -= bytes; /* 처리한 바이트만큼 감소 */
		p += bytes; /* 원본 버퍼 포인터 전진 */
	}

	if (op_is_write(op)) { /* WRITE는 복사된 페이지만 해제 */
		bio->bi_end_io = bio_copy_kern_endio;
	} else { /* READ는 완료 후 원래 kbuf로 복사할 수 있도록 bi_private 저장 */
		bio->bi_end_io = bio_copy_kern_endio_read;
		bio->bi_private = data; /* NVMe CQ 완료 후 원래 커널 버퍼로 결과 복사할 때 사용 */
	}

	return bio; /* bounce buffer bio; blk_rq_append_bio -> blk_mq -> nvme_queue_rq로 진행 */

cleanup:
	bio_free_pages(bio); /* 실패 시 이미 추가된 bounce 페이지 회수; NVMe SQ에는 도달하지 않음 */
	blk_mq_map_bio_put(bio); /* bio 반환 */
	return ERR_PTR(-ENOMEM);
}

/*
 * Append a bio to a passthrough request.  Only works if the bio can be merged
 * into the request based on the driver constraints.
 */
/*
 * blk_rq_append_bio - passthrough request에 bio를 추가/병합
 * @rq: 대상 request
 * @bio: 추가할 bio
 *
 * 목적:
 *   단일 bio를 request에 연결하거나, 기존 request 뒤에 back-merge한다.
 *   호출 경로:
 *     blk_rq_map_user_iov / blk_rq_map_kern 등 -> blk_rq_append_bio
 *     -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
 *   NVMe 연결:
 *     queue_limits.max_hw_sectors는 NVMe Max Data Transfer Size(MDTS)에
 *     대응하는 상한(추정)이며, bio_split_io_at으로 초과 여부를 검사한다.
 *     rq->nr_phys_segments는 NVMe PRP/SGL entry 개수와 관련이 있다.
 */
int blk_rq_append_bio(struct request *rq, struct bio *bio)
{
	const struct queue_limits *lim = &rq->q->limits; /* request queue의 한계값; NVMe MDTS/segment 제약 반영 */
	unsigned int max_bytes = lim->max_hw_sectors << SECTOR_SHIFT; /* 최대 전송 바이트; NVMe Max Data Transfer Size 대응(추정) */
	unsigned int nr_segs = 0; /* bio의 물리 segment 수; NVMe PRP/SGL entry 개수 추정치 */
	int ret;

	/* check that the data layout matches the hardware restrictions */
	ret = bio_split_io_at(bio, lim, &nr_segs, max_bytes, 0); /* bio가 하드웨어 한계를 초과하면 분할 또는 복사 필요 */
	if (ret) {
		/* if we would have to split the bio, copy instead */
		if (ret > 0) /* 분할이 필요하면 EREMOTEIO로 표시 후 상위에서 복사로 폴백; NVMe SQ에는 doorbell 없음 */
			ret = -EREMOTEIO;
		return ret; /* NVMe 명령 생성 실패; 상위에서 복사 경로로 재시도 */
	}

	if (rq->bio) {
		if (!ll_back_merge_fn(rq, bio, nr_segs)) /* 기존 request 뒤에 back-merge 가능한지 검사; NVMe PRP/SGL 연속성/MDTS 위반 시 거부 */
			return -EINVAL;
		rq->phys_gap_bit = bio_seg_gap(rq->q, rq->biotail, bio, /* segment 간 물리적 간격을 기록; IOMMU/NVMe SGL/PRP에 영향(추정) */
					       rq->phys_gap_bit);
		rq->biotail->bi_next = bio; /* bio를 request의 bio 리스트 끝에 연결; NVMe 명령 하나로 처리될 데이터 청크 추가 */
		rq->biotail = bio;
		rq->__data_len += bio->bi_iter.bi_size; /* request의 총 데이터 길이 누적; NVMe 명령 길이와 일치 */
		bio_crypt_free_ctx(bio); /* 암호화 컨텍스트 해제; NVMe inline encryption은 하위 계층에서 별도 처리(추정) */
		return 0;
	}

	rq->nr_phys_segments = nr_segs; /* request의 첫 bio 설정; NVMe PRP/SGL entry 개수 초기화 */
	rq->bio = rq->biotail = bio; /* request의 bio 리스트를 이 bio로 시작 */
	rq->__data_len = bio->bi_iter.bi_size; /* request의 총 데이터 길이; NVMe 명령의 데이터 길이와 일치 */
	rq->phys_gap_bit = bio->bi_bvec_gap_bit; /* bio 내부 segment 간 물리적 gap 기록 */
	return 0; /* bio가 request에 성공적으로 연결; 이후 blk_mq -> nvme_queue_rq 진행 가능 */
}
EXPORT_SYMBOL(blk_rq_append_bio);

/* Prepare bio for passthrough IO given ITER_BVEC iter */
/*
 * blk_rq_map_user_bvec - ITER_BVEC 기반 사용자/커널 bvec을 bio로 재사용
 * @rq: 대상 request
 * @iter: bvec 반복자
 *
 * 목적:
 *   이미 존재하는 bvec을 그대로 bio에 연결하여 할당을 줄인다.
 *   호출 경로:
 *     blk_rq_map_user_iov -> blk_rq_map_user_bvec -> blk_rq_append_bio
 *   NVMe 연결:
 *     bvec의 각 page/offset/len이 NVMe PRP entry로 변환될 수 있도록
 *     request에 등록된다.
 */
static int blk_rq_map_user_bvec(struct request *rq, const struct iov_iter *iter)
{
	unsigned int max_bytes = rq->q->limits.max_hw_sectors << SECTOR_SHIFT; /* passthrough bio의 최대 허용 바이트; NVMe Max Data Transfer Size(MDTS) 대응(추정) */
	struct bio *bio;
	int ret;

	if (!iov_iter_count(iter) || iov_iter_count(iter) > max_bytes) /* 길이가 0이거나 max를 초과하면 NVMe 컨트롤러가 처리할 수 없음 */
		return -EINVAL;

	/* reuse the bvecs from the iterator instead of allocating new ones */
	bio = blk_rq_map_bio_alloc(rq, 0, GFP_KERNEL); /* bvec을 재사용하므로 추가 할당 없음(nr_vecs=0); NVMe PRP/SGL용 페이지 목록은 이미 iter에 존재 */
	if (!bio)
		return -ENOMEM;
	bio_iov_bvec_set(bio, iter); /* 반복자의 bvec을 bio에 직접 설정; 각 bvec이 NVMe PRP entry로 변환 가능 */

	ret = blk_rq_append_bio(rq, bio); /* 재사용한 bvec bio를 request에 추가 */
	if (ret)
		blk_mq_map_bio_put(bio); /* request 추가 실패 시 bio 반환; NVMe SQ doorbell 없음 */
	return ret; /* 0이면 blk_mq -> nvme_queue_rq로 진행 가능 */
}

/

	ret = blk_rq_append_bio(rq, bio); /* 재사용한 bvec bio를 request에 추가 */
	if (ret)
		blk_mq_map_bio_put(bio); /* request 추가 실패 시 bio 반환; NVMe SQ doorbell 없음 */
	return ret; /* 0이면 blk_mq -> nvme_queue_rq로 진행 가능 */
}

/**
 * blk_rq_map_user_iov - map user data to a request, for passthrough requests
 * @q:		request queue where request should be inserted
 * @rq:		request to map data to
 * @map_data:   pointer to the rq_map_data holding pages (if necessary)
 * @iter:	iovec iterator
 * @gfp_mask:	memory allocation flags
 *
 * Description:
 *    Data will be mapped directly for zero copy I/O, if possible. Otherwise
 *    a kernel bounce buffer is used.
 *
 *    A matching blk_rq_unmap_user() must be issued at the end of I/O, while
 *    still in process context.
 */
/*
 * blk_rq_map_user_iov - 사용자 iov를 request에 매핑하는 최상위 함수
 * @q: request queue
 * @rq: 대상 request
 * @map_data: 페이지 풀(rq_map_data)
 * @iter: 사용자 버퍼 기술자
 * @gfp_mask: 메모리 할당 플래그
 *
 * 목적:
 *   zero-copy 또는 bounce-buffer 중 적절한 경로를 선택해 bio를 구성한다.
 *   호출 경로:
 *     SG_IO ioctl -> blk_rq_map_user / blk_rq_map_user_io
 *                 -> blk_rq_map_user_iov
 *                 -> (bio_map_user_iov | bio_copy_user_iov | blk_rq_map_user_bvec)
 *                 -> blk_rq_append_bio
 *                 -> blk_mq_submit_bio -> blk_mq_get_request
 *                 -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *   NVMe 연결:
 *     DMA 정렬, 가상 경계, 최대 전송 크기 등 NVMe 컨트롤러 제약을 반영해
 *     bio를 준비한다. PRP/SGL 변환은 nvme_queue_rq에서 이루어진다.
 */
int blk_rq_map_user_iov(struct request_queue *q, struct request *rq,
			struct rq_map_data *map_data,
			const struct iov_iter *iter, gfp_t gfp_mask)
{
	bool copy = false, map_bvec = false; /* copy: bounce buffer 경로; map_bvec: 기존 bvec 재사용 경로 */
	unsigned long align = blk_lim_dma_alignment_and_pad(&q->limits); /* DMA 정렬/패딩 요구사항; NVMe PRP/SGL 정렬과 관련(추정) */
	struct bio *bio = NULL; /* 첫 번째 bio를 기록; 실패 시 blk_rq_unmap_user로 정리 */
	struct iov_iter i;
	int ret = -EINVAL;

	if (map_data) /* map_data가 있으면 미리 준비된 페이지 풀 사용 -> 복사 경로; NVMe passthrough 페이지 풀 모드 */
		copy = true;
	else if (iov_iter_alignment(iter) & align) /* DMA 정렬 미충족 시 bounce buffer 복사 필요; NVMe PRP는 정렬된 물리 주소 선호(추정) */
		copy = true;
	else if (iov_iter_is_bvec(iter)) /* ITER_BVEC는 bvec 재사용 경로로 전환; NVMe PRP/SGL 페이지 핀 없이 빠른 매핑 */
		map_bvec = true;
	else if (!user_backed_iter(iter)) /* 사용자 페이지가 아닌 경우(예: pipe) 복사 필요; NVMe DMA를 위한 커널 페이지 준비 */
		copy = true;
	else if (queue_virt_boundary(q)) /* 가상 경계를 넘는 gap이 있으면 NVMe DMA/PRP 처리가 어려워 복사(추정) */
		copy = queue_virt_boundary(q) & iov_iter_gap_alignment(iter);

	if (map_bvec) { /* bvec 재사용 시도; NVMe SGL/PRP entry가 이미 준비된 경우 */
		ret = blk_rq_map_user_bvec(rq, iter);
		if (!ret)
			return 0; /* bvec 재사용 성공; blk_mq -> nvme_queue_rq로 진행 */
		if (ret != -EREMOTEIO) /* bvec 재사용이 한계 초과 외의 이유로 실패면 즉시 종료 */
			goto fail;
		/* fall back to copying the data on limits mismatches */
		copy = true; /* 한계 초과 시 복사로 폴백; NVMe MDTS/segment 제약 우회 */
	}

	i = *iter; /* 남은 iov_iter가 있을 때까지 bio를 생성/추가 */
	do {
		if (copy)
			ret = bio_copy_user_iov(rq, map_data, &i, gfp_mask);
		else
			ret = bio_map_user_iov(rq, &i, gfp_mask);
		if (ret) {
			if (ret == -EREMOTEIO) /* EREMOTEIO는 상위에서 -EINVAL로 변환; NVMe SQ 도달 전 매핑 실패 */
				ret = -EINVAL;
			goto unmap_rq; /* 이미 연결된 bio들을 정리; NVMe doorbell은 갱신되지 않음 */
		}
		if (!bio) /* 첫 bio를 기록해 실패 시 언매핑에 사용 */
			bio = rq->bio;
	} while (iov_iter_count(&i)); /* 처리하지 않은 사용자 데이터가 남아 있으면 반복; NVMe 명령은 bio 단위로 분할 연결 */

	return 0; /* 모든 bio가 request에 연결; blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq 가능 */

unmap_rq:
	blk_rq_unmap_user(bio); /* 실패 시 기존에 연결된 bio들을 언매핑; pin된 페이지나 bounce buffer 회수 */
fail:
	rq->bio = NULL; /* request와 bio의 연결을 끊어 상위가 재시도/정리 가능하게 함; NVMe SQ에는 도달하지 않음 */
}
EXPORT_SYMBOL(blk_rq_map_user_iov);

/*
 * blk_rq_map_user - 단일 사용자 버퍼를 request에 매핑
 * @q: request queue
 * @rq: 대상 request
 * @map_data: 페이지 풀
 * @ubuf: 사용자 버퍼 주소
 * @len: 길이
 * @gfp_mask: 메모리 할당 플래그
 *
 * 목적:
 *   import_ubuf()로 iov_iter를 만든 뒤 blk_rq_map_user_iov()를 호출한다.
 *   호출 경로:
 *     SG_IO / ioctl -> blk_rq_map_user -> blk_rq_map_user_iov -> ...
 *   NVMe 연결:
 *     NVMe admin/ioctl passthrough 명령의 데이터 버퍼 처리 시작점.
 */
int blk_rq_map_user(struct request_queue *q, struct request *rq,
		    struct rq_map_data *map_data, void __user *ubuf,
		    unsigned long len, gfp_t gfp_mask)
{
	struct iov_iter i;
	int ret = import_ubuf(rq_data_dir(rq), ubuf, len, &i); /* 사용자 단일 버퍼를 iov_iter로 변환; rq_data_dir은 NVMe READ/WRITE 방향 */

	if (unlikely(ret < 0)) /* 사용자 공간 버퍼 import 실패; NVMe SQ 도달 전 상위로 오류 반환 */
		return ret;

	return blk_rq_map_user_iov(q, rq, map_data, &i, gfp_mask); /* zero-copy/bounce 경로 선택 후 bio 구성 -> blk_mq -> nvme_queue_rq */
}
EXPORT_SYMBOL(blk_rq_map_user);

/*
 * blk_rq_map_user_io - iovec 기반 SG_IO 버퍼를 request에 매핑
 * @req: 대상 request
 * @map_data: 페이지 풀
 * @ubuf: 사용자 iovec 또는 단일 버퍼
 * @buf_len: 버퍼 길이
 * @gfp_mask: 메모리 할당 플래그
 * @vec: iovec 사용 여부
 * @iov_count: iovec 개수
 * @check_iter_count: 반복자 크기 검사 여부
 * @rw: 데이터 방향
 *
 * 목적:
 *   SG_IO 인터페이스에서 전달된 벡터/비벡터 버퍼를 import_iovec/import_ubuf로
 *   변환 후 blk_rq_map_user_iov에 위임한다.
 *   호출 경로:
 *     sg_io -> blk_rq_map_user_io -> blk_rq_map_user_iov -> ...
 *   NVMe 연결:
 *     NVMe character/passthrough 장치를 통한 사용자 명령의 데이터 경로.
 */
int blk_rq_map_user_io(struct request *req, struct rq_map_data *map_data,
		void __user *ubuf, unsigned long buf_len, gfp_t gfp_mask,
		bool vec, int iov_count, bool check_iter_count, int rw)
{
	int ret = 0;

	if (vec) {
		struct iovec fast_iov[UIO_FASTIOV];
		struct iovec *iov = fast_iov;
		struct iov_iter iter;

		ret = import_iovec(rw, ubuf, iov_count ? iov_count : buf_len, /* 사용자 iovec을 가져와 iov_iter 생성; rw는 NVMe READ/WRITE 방향 */
				UIO_FASTIOV, &iov, &iter);
		if (ret < 0)
			return ret; /* iovec import 실패; NVMe SQ 도달 전 종료 */

		if (iov_count) {
			/* SG_IO howto says that the shorter of the two wins */
			iov_iter_truncate(&iter, buf_len);
			if (check_iter_count && !iov_iter_count(&iter)) {
				kfree(iov);
				return -EINVAL; /* 빈 반복자는 NVMe 컨트롤러가 처리할 데이터 없음 */
			}
		}

		ret = blk_rq_map_user_iov(req->q, req, map_data, &iter, /* 변환된 iov_iter를 blk_rq_map_user_iov로 전달 */
				gfp_mask);
		kfree(iov);
	} else if (buf_len) {
		ret = blk_rq_map_user(req->q, req, map_data, ubuf, buf_len,
				gfp_mask);
	}
	return ret; /* 성공 시 blk_mq -> nvme_queue_rq 진행; 실패 시 NVMe SQ 도달 안 함 */
}
EXPORT_SYMBOL(blk_rq_map_user_io);

/**
 * blk_rq_unmap_user - unmap a request with user data
 * @bio:	       start of bio list
 *
 * Description:
 *    Unmap a rq previously mapped by blk_rq_map_user(). The caller must
 *    supply the original rq->bio from the blk_rq_map_user() return, since
 *    the I/O completion may have changed rq->bio.
 */
/*
 * blk_rq_unmap_user - 사용자 데이터가 매핑된 request를 언매핑
 * @bio: bio 리스트의 시작
 *
 * 목적:
 *   I/O 완료 후 사용자 페이지를 해제하거나 bounce buffer를 정리한다.
 *   호출 경로:
 *     nvme_completion -> blk_mq_end_request -> blk_rq_unmap_user
 *                       -> (bio_uncopy_user | bio_release_pages)
 *   NVMe 연결:
 *     NVMe CQ entry가 완료되고 request가 해제될 때, PRP/SGL에 사용된
 *     페이지들을 반납한다.
 */
int blk_rq_unmap_user(struct bio *bio)
{
	struct bio *next_bio;
	int ret = 0, ret2; /* ret: 첫 번째 오류 저장; ret2: 각 bio 언매핑 결과 */

	while (bio) { /* bio 리스트를 순회하며 모두 정리; NVMe CQ entry 처리 후 request 해제 단계 */
		if (bio->bi_private) { /* bounce buffer bio는 bio_uncopy_user로 해제/복사 */
			ret2 = bio_uncopy_user(bio);
			if (ret2 && !ret)
				ret = ret2; /* 첫 오류만 보존; 상위 SG_IO는 이 오류를 수신 */
		} else {
			bio_release_pages(bio, bio_data_dir(bio) == READ); /* 직접 매핑된 사용자 페이지는 해제; NVMe DMA mapping 해제(추정) */
		}

		if (bio_integrity(bio)) /* 데이터 무결성 메타데이터가 있으면 별도 언매핑; NVMe DIF/DIX 후처리(추정) */
			bio_integrity_unmap_user(bio);

		next_bio = bio;
		bio = bio->bi_next; /* 다음 bio로 이동; NVMe 명령 하나가 여러 bio로 구성될 수 있음 */
		blk_mq_map_bio_put(next_bio); /* bio 참조 해제; NVMe CID에 대응하는 버퍼 자원 반납 */
	}

	return ret; /* 0이면 정상 해제; 비0이면 NVMe READ 사용자 공간 복사 등에서 문제 발생 */
}
EXPORT_SYMBOL(blk_rq_unmap_user);

/**
 * blk_rq_map_kern - map kernel data to a request, for passthrough requests
 * @rq:		request to fill
 * @kbuf:	the kernel buffer
 * @len:	length of user data
 * @gfp_mask:	memory allocation flags
 *
 * Description:
 *    Data will be mapped directly if possible. Otherwise a bounce
 *    buffer is used. Can be called multiple times to append multiple
 *    buffers.
 */
/*
 * blk_rq_map_kern - 커널 버퍼를 passthrough request에 매핑
 * @rq: 대상 request
 * @kbuf: 커널 버퍼
 * @len: 길이
 * @gfp_mask: 메모리 할당 플래그
 *
 * 목적:
 *   커널 영역의 버퍼를 조건에 따라 직접 매핑하거나 bounce buffer로 복사한다.
 *   호출 경로:
 *     커널 passthrough -> blk_rq_map_kern
 *                     -> (bio_map_kern | bio_copy_kern) -> blk_rq_append_bio
 *                     -> blk_mq_submit_bio -> blk_mq_get_request
 *                     -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *   NVMe 연결:
 *     커널 드라이버나 nvme-cli가 발행하는 admin/IO 명령의 데이터 버퍼를
 *     NVMe 명령의 PRP/SGL로 연결하기 위한 진입점.
 */
int blk_rq_map_kern(struct request *rq, void *kbuf, unsigned int len,
		gfp_t gfp_mask)
{
	unsigned long addr = (unsigned long) kbuf; /* 커널 버퍼 가상 주소; DMA 정렬 및 가상 경계 검사에 사용 */
	struct bio *bio;
	int ret;

	if (len > (queue_max_hw_sectors(rq->q) << SECTOR_SHIFT)) /* NVMe Max Data Transfer Size(MDTS)를 초과하면 거부(추정) */
		return -EINVAL;
	if (!len || !kbuf) /* 데이터 길이 0 또는 NULL 버퍼는 유효하지 않음; NVMe 컨트롤러가 데이터 없는 명령 외에는 거부 */
		return -EINVAL;

	if (!blk_rq_aligned(rq->q, addr, len) || object_is_on_stack(kbuf)) /* DMA 정렬/스택 버퍼는 직접 매핑 불가 -> bounce buffer */
		bio = bio_copy_kern(rq, kbuf, len, gfp_mask); /* 복사 경로: 커널 페이지에 데이터를 복사한 bio 생성; NVMe PRP/SGL에 적합한 메모리 */
	else
		bio = bio_map_kern(rq, kbuf, len, gfp_mask); /* 직접 매핑 경로: 커널 주소를 bio에 연결; NVMe DMA가 직접 접근(추정) */

	if (IS_ERR(bio))
		return PTR_ERR(bio); /* bio 구성 실패; NVMe SQ doorbell 이전에 상위로 오류 */

	ret = blk_rq_append_bio(rq, bio); /* bio를 request에 추가 -> blk_mq -> nvme_queue_rq로 진행 */
	if (unlikely(ret))
		blk_mq_map_bio_put(bio); /* request 추가 실패 시 bio 반환; NVMe SQ에 도달하지 않음 */
	return ret; /* 0이면 NVMe SQ 제출 가능; 비0이면 매핑 실패 */
}
EXPORT_SYMBOL(blk_rq_map_kern);

/* NVMe 관점 핵심 요약 */

/*
 * - 본 파일은 상위 계층의 사용자/커널 버퍼를 bio로 변환하여 NVMe PRP/SGL의
 *   기반이 되는 페이지 목록을 준비한다.
 * - DMA 정렬, 가상 경계, max_hw_sectors 등의 제약을 검사/우회하여
 *   NVMe 컨트롤러가 처리할 수 있는 형태로 bio를 정리한다.
 * - blk_rq_append_bio를 거쳐 request에 bio가 연결되면, 이후
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq 순으로
 *   NVMe SQ에 CID가 할당되고 doorbell이 갱신된다.
 * - READ 완료 시 CQ 처리 후 blk_rq_unmap_user -> bio_uncopy_user를 통해
 *   사용자 공간으로 데이터가 복사되거나 페이지가 반납된다.
 * - block/blk-merge.c의 bio 병합/분할과 밀접하게 연결되며, 본 파일이
 *   생성한 bio는 이후 병합 단계에서 추가 정제될 수 있다.
 */
