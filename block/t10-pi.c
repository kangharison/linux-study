// SPDX-License-Identifier: GPL-2.0
/*
 * t10_pi.c - Functions for generating and verifying T10 Protection
 *	      Information.
 */

/*
 * NVMe SSD 관점 파일 개요
 *
 * T10 Protection Information(PI)의 생성과 검증을 수행한다.
 * NVMe end-to-end data protection(PRACT/PRCHK)에서 guard/ref/app 태그를
 * 계산하거나 검증한다.
 * 호출 경로:
 *   submit_bio -> blk_mq_submit_bio -> ... -> bio_integrity_generate
 *   I/O 완료 경로 -> bio_integrity_verify
 * 연관 파일: block/bio-integrity.c, block/integrity.c
 */
#include <linux/t10-pi.h>
#include <linux/blk-integrity.h>
#include <linux/crc-t10dif.h>
#include <linux/crc64.h>
#include <net/checksum.h>
#include <linux/unaligned.h>
#include "blk.h"

#define APP_TAG_ESCAPE 0xffff

/* APP_TAG_ESCAPE: app 태그가 이 값이면 NVMe 컨트롤러는 PI 검사를
 * 수행하지 않는다 (T10 PI 규격, NVMe PRCHK와 연동, 추정). */
#define REF_TAG_ESCAPE 0xffffffff
/* REF_TAG_ESCAPE: ref 태그가 모두 1이면 PI 검사를 생략함을
 * 나타내는 표시자이다 (T10 PI 규격, NVMe PRCHK와 연동, 추정). */

/*
 * This union is used for onstack allocations when the pi field is split across
 * segments. blk_validate_integrity_limits() guarantees pi_tuple_size matches
 * the sizeof one of these two types.
 */
/*
 * NVMe PI 튜플 표현: NVMe E2E PI에서 guard/ref/app 태그를 담는
 * 메타데이터 단위이다. crc64_pi_tuple은 CRC64 guard를 사용하는 형식이고,
 * t10_pi_tuple은 CRC32/T10 DIF 형식에 해당한다.
 * NVMe PRACT/PRCHK 설정에 따라 선택적으로 생성되거나 검증된다 (추정).
 */
union pi_tuple {
	struct crc64_pi_tuple	crc64_pi;
	struct t10_pi_tuple	t10_pi;
};

/*
 * T10 PI 반복자. NVMe I/O 경로에서 데이터 버퍼와 무결성 메타데이터
 * 버퍼를 동시에 순회하며, 각 interval 마다 guard/ref/app 태그를
 * 생성하거나 검증한다.
 *
 * - bio/bip/bi: 상위 bio, 무결성 payload, 장치 무결성 profile
 * - data_iter: 실제 사용자 데이터(LBA 데이터)의 bvec iterator
 * - prot_iter: NVMe PI 메타데이터(DIF/DIX)가 저장된 bvec iterator
 * - interval_remaining: 현재 LBA interval 내 남은 바이트 수
 * - seed: NVMe ref tag 초기값 (보통 시작 섹터 번호)
 * - csum: 현재 interval까지 누적된 guard 체크섬
 */
struct blk_integrity_iter {
	struct bio			*bio;
	struct bio_integrity_payload	*bip;
	struct blk_integrity		*bi;
	struct bvec_iter		data_iter;
	struct bvec_iter		prot_iter;
	unsigned int			interval_remaining;
	u64				seed;
	u64				csum;
};

/*
 * blk_calculate_guard
 * 목적: 현재 데이터 구간의 guard 체크섬을 누적 계산한다.
 * 경로: blk_integrity_iterate -> blk_calculate_guard
 * NVMe 연결: NVMe PRCHK의 Guard Check가 활성화되면 컨트롤러가 동일한
 *   guard 값을 계산하여 비교한다 (추정). csum_type에 따라 CRC64, T10 DIF
 *   CRC, IP 체크섬 중 하나를 사용한다.
 */
static void blk_calculate_guard(struct blk_integrity_iter *iter, void *data,
				unsigned int len)
{
	switch (iter->bi->csum_type) {
	case BLK_INTEGRITY_CSUM_CRC64:
		iter->csum = crc64_nvme(iter->csum, data, len);
		break;
		/* NVMe CRC64 guard 체크섬 누적 (E2E PI 64비트 guard) */
	case BLK_INTEGRITY_CSUM_CRC:
		iter->csum = crc_t10dif_update(iter->csum, data, len);
		break;
		/* 전통적인 T10 DIF CRC16 guard 누적 */
	case BLK_INTEGRITY_CSUM_IP:
		iter->csum = (__force u32)csum_partial(data, len,
						(__force __wsum)iter->csum);
		/* IP 체크섬 기반 guard (SCSI 상위 경로에서 주로 사용, 추정) */
		break;
	default:
		WARN_ON_ONCE(1);
		iter->csum = U64_MAX;
		break;
	}
}

/*
 * blk_integrity_csum_finish
 * 목적: IP 체크섬 형식의 경우 최종 16비트 fold를 수행한다.
 * 경로: blk_integrity_interval -> blk_integrity_csum_offset -> ...
 * NVMe 연결: NVMe PI는 주로 CRC64/CRC32 기반이므로 IP 경로는 상위
 *   SCSI 계층에서 사용될 수 있다 (추정).
 */
static void blk_integrity_csum_finish(struct blk_integrity_iter *iter)
{
	switch (iter->bi->csum_type) {
	case BLK_INTEGRITY_CSUM_IP:
		iter->csum = (__force u16)csum_fold((__force __wsum)iter->csum);
		break;
	default:
		break;
	}
}

/*
 * Update the csum for formats that have metadata padding in front of the data
 * integrity field
 */
/*
 * blk_integrity_csum_offset
 * 목적: 메타데이터 앞쪽 padding을 skip하면서 padding 영역까지 체크섬에
 *   포함해야 하는 경우(일부 NVMe formatted metadata 모드) 처리한다.
 * 경로: blk_integrity_interval -> blk_integrity_csum_offset
 * NVMe 연결: NVMe formatted metadata에서 PI offset 이전 영역이 guard
 *   계산에 포함될 수 있다 (추정).
 */
static void blk_integrity_csum_offset(struct blk_integrity_iter *iter)
{
	unsigned int offset = iter->bi->pi_offset;
	struct bio_vec *bvec = iter->bip->bip_vec;

	while (offset > 0) {
		struct bio_vec pbv = bvec_iter_bvec(bvec, iter->prot_iter);
		unsigned int len = min(pbv.bv_len, offset);
		void *prot_buf = bvec_kmap_local(&pbv);

		blk_calculate_guard(iter, prot_buf, len);
		kunmap_local(prot_buf);
		offset -= len;
		bvec_iter_advance_single(bvec, &iter->prot_iter, len);
	}
	blk_integrity_csum_finish(iter);
}

static void blk_integrity_copy_from_tuple(struct bio_integrity_payload *bip,
					  struct bvec_iter *iter, void *tuple,
					  unsigned int tuple_size)
{
	while (tuple_size) {
		struct bio_vec pbv = bvec_iter_bvec(bip->bip_vec, *iter);
		unsigned int len = min(tuple_size, pbv.bv_len);
		void *prot_buf = bvec_kmap_local(&pbv);

		memcpy(prot_buf, tuple, len);
		kunmap_local(prot_buf);
		bvec_iter_advance_single(bip->bip_vec, iter, len);
		tuple_size -= len;
		tuple += len;
	}
}

static void blk_integrity_copy_to_tuple(struct bio_integrity_payload *bip,
					struct bvec_iter *iter, void *tuple,
					unsigned int tuple_size)
{
	while (tuple_size) {
		struct bio_vec pbv = bvec_iter_bvec(bip->bip_vec, *iter);
		unsigned int len = min(tuple_size, pbv.bv_len);
		void *prot_buf = bvec_kmap_local(&pbv);

		memcpy(tuple, prot_buf, len);
		kunmap_local(prot_buf);
		bvec_iter_advance_single(bip->bip_vec, iter, len);
		tuple_size -= len;
		tuple += len;
	}
}

static bool ext_pi_ref_escape(const u8 ref_tag[6])
{
	static const u8 ref_escape[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	return memcmp(ref_tag, ref_escape, sizeof(ref_escape)) == 0;
}

/*
 * blk_verify_ext_pi
 * 목적: CRC64 PI 튜플을 읽어 guard/ref/app 태그를 검증한다.
 * 경로: blk_integrity_verify -> blk_verify_ext_pi
 * NVMe 연결: NVMe CRC64 PRCHK가 설정된 read/write 완료 후, 컨트롤러가
 *   저장한 태그와 호스트가 계산한 값을 비교한다.
 */
static blk_status_t blk_verify_ext_pi(struct blk_integrity_iter *iter,
				      struct crc64_pi_tuple *pi)
{
	u64 seed = lower_48_bits(iter->seed);
	u64 guard = get_unaligned_be64(&pi->guard_tag);
	u64 ref = get_unaligned_be48(pi->ref_tag);
	u16 app = get_unaligned_be16(&pi->app_tag);

	if (iter->bi->flags & BLK_INTEGRITY_REF_TAG) {
		if (app == APP_TAG_ESCAPE)
			return BLK_STS_OK;
		/* APP_TAG_ESCAPE 시 NVMe PRCHK는 app/guard/ref 검사를 생략한다 (추정) */
		if (ref != seed) {
			pr_err("%s: ref tag error at location %llu (rcvd %llu)\n",
				iter->bio->bi_bdev->bd_disk->disk_name, seed,
				ref);
			return BLK_STS_PROTECTION;
		}
	} else if (app == APP_TAG_ESCAPE && ext_pi_ref_escape(pi->ref_tag)) {
		return BLK_STS_OK;
	}

	if (guard != iter->csum) {
		pr_err("%s: guard tag error at sector %llu (rcvd %016llx, want %016llx)\n",
			iter->bio->bi_bdev->bd_disk->disk_name, iter->seed,
			guard, iter->csum);
		return BLK_STS_PROTECTION;
	}

	return BLK_STS_OK;
}

/*
 * blk_verify_pi
 * 목적: T10/IP 체크섬 PI 튜플을 읽어 guard/ref/app 태그를 검증한다.
 * 경로: blk_integrity_verify -> blk_verify_t10_pi / blk_verify_ip_pi
 * NVMe 연결: NVMe PRCHK의 Ref Tag/App Tag/Guard Check 설정에 따라
 *   각 태그를 개별적으로 확인한다.
 */
static blk_status_t blk_verify_pi(struct blk_integrity_iter *iter,
				      struct t10_pi_tuple *pi, u16 guard)
{
	u32 seed = lower_32_bits(iter->seed);
	u32 ref = get_unaligned_be32(&pi->ref_tag);
	u16 app = get_unaligned_be16(&pi->app_tag);

	if (iter->bi->flags & BLK_INTEGRITY_REF_TAG) {
		if (app == APP_TAG_ESCAPE)
			return BLK_STS_OK;
		if (ref != seed) {
			pr_err("%s: ref tag error at location %u (rcvd %u)\n",
				iter->bio->bi_bdev->bd_disk->disk_name, seed,
				ref);
			return BLK_STS_PROTECTION;
		}
	} else if (app == APP_TAG_ESCAPE && ref == REF_TAG_ESCAPE) {
		return BLK_STS_OK;
	}

	if (guard != (u16)iter->csum) {
		pr_err("%s: guard tag error at sector %llu (rcvd %04x, want %04x)\n",
			iter->bio->bi_bdev->bd_disk->disk_name, iter->seed,
			guard, (u16)iter->csum);
		return BLK_STS_PROTECTION;
	}

	return BLK_STS_OK;
}

static blk_status_t blk_verify_t10_pi(struct blk_integrity_iter *iter,
				      struct t10_pi_tuple *pi)
{
	u16 guard = get_unaligned_be16(&pi->guard_tag);

	return blk_verify_pi(iter, pi, guard);
}

static blk_status_t blk_verify_ip_pi(struct blk_integrity_iter *iter,
				     struct t10_pi_tuple *pi)
{
	u16 guard = get_unaligned((u16 *)&pi->guard_tag);

	return blk_verify_pi(iter, pi, guard);
}

/*
 * blk_integrity_verify
 * 목적: csum_type에 맞는 PI 검증 함수를 선택한다.
 * 경로: bio_integrity_verify -> blk_integrity_iterate -> blk_integrity_interval
 *   -> blk_integrity_verify
 * NVMe 연결: NVMe SSD로부터 수신된 PI 메타데이터가 완료 경로에서
 *   검증된다.
 */
static blk_status_t blk_integrity_verify(struct blk_integrity_iter *iter,
					 union pi_tuple *tuple)
{
	switch (iter->bi->csum_type) {
	case BLK_INTEGRITY_CSUM_CRC64:
		return blk_verify_ext_pi(iter, &tuple->crc64_pi);
	case BLK_INTEGRITY_CSUM_CRC:
		return blk_verify_t10_pi(iter, &tuple->t10_pi);
	case BLK_INTEGRITY_CSUM_IP:
		return blk_verify_ip_pi(iter, &tuple->t10_pi);
	default:
		return BLK_STS_OK;
	}
}

static void blk_set_ext_pi(struct blk_integrity_iter *iter,
			   struct crc64_pi_tuple *pi)
{
	put_unaligned_be64(iter->csum, &pi->guard_tag);
	put_unaligned_be16(0, &pi->app_tag);
	put_unaligned_be48(iter->seed, &pi->ref_tag);
}

static void blk_set_pi(struct blk_integrity_iter *iter,
		       struct t10_pi_tuple *pi, __be16 csum)
{
	put_unaligned(csum, &pi->guard_tag);
	put_unaligned_be16(0, &pi->app_tag);
	put_unaligned_be32(iter->seed, &pi->ref_tag);
}

static void blk_set_t10_pi(struct blk_integrity_iter *iter,
			   struct t10_pi_tuple *pi)
{
	blk_set_pi(iter, pi, cpu_to_be16((u16)iter->csum));
}

static void blk_set_ip_pi(struct blk_integrity_iter *iter,
			  struct t10_pi_tuple *pi)
{
	blk_set_pi(iter, pi, (__force __be16)(u16)iter->csum);
}

/*
 * blk_integrity_set
 * 목적: csum_type에 맞게 새로운 PI 튜플(guard/ref/app)을 기록한다.
 * 경로: blk_integrity_interval -> blk_integrity_set
 * NVMe 연결: NVMe I/O 발행 직전, 호스트가 생성한 PI 태그를 메타데이터
 *   버퍼에 쓰어 컨트롤러가 PRCHK로 검증할 수 있게 한다.
 */
static void blk_integrity_set(struct blk_integrity_iter *iter,
			      union pi_tuple *tuple)
{
	switch (iter->bi->csum_type) {
	case BLK_INTEGRITY_CSUM_CRC64:
		return blk_set_ext_pi(iter, &tuple->crc64_pi);
	case BLK_INTEGRITY_CSUM_CRC:
		return blk_set_t10_pi(iter, &tuple->t10_pi);
	case BLK_INTEGRITY_CSUM_IP:
		return blk_set_ip_pi(iter, &tuple->t10_pi);
	default:
		WARN_ON_ONCE(1);
		return;
	}
}

/*
 * blk_integrity_interval
 * 목적: 한 LBA interval에 대해 PI를 생성하거나 검증한다.
 * 경로: blk_integrity_iterate -> blk_integrity_interval
 * NVMe 연결: NVMe는 보통 512B 또는 4KB 단위의 논리 블록/보호
 *   interval에 매핑되므로, 이 단위로 guard/ref/app이 갱신된다.
 */
static blk_status_t blk_integrity_interval(struct blk_integrity_iter *iter,
					   bool verify)
{
	blk_status_t ret = BLK_STS_OK;
	union pi_tuple tuple;
	void *ptuple = &tuple;
	struct bio_vec pbv;

	blk_integrity_csum_offset(iter);
	pbv = bvec_iter_bvec(iter->bip->bip_vec, iter->prot_iter);
	/* PI 튜플이 bvec에 연속적으로 들어가 있으면 직접 매핑하고,
	 * 그렇지 않으면 스택 tuple에 복사하여 처리한다. */
	if (pbv.bv_len >= iter->bi->pi_tuple_size) {
		ptuple = bvec_kmap_local(&pbv);
		bvec_iter_advance_single(iter->bip->bip_vec, &iter->prot_iter,
				iter->bi->metadata_size - iter->bi->pi_offset);
	} else if (verify) {
		blk_integrity_copy_to_tuple(iter->bip, &iter->prot_iter,
				ptuple, iter->bi->pi_tuple_size);
	}

	/* 생성/검증 수행: verify==true 이면 NVMe로부터 받은 태그와 비교 */
	if (verify)
		ret = blk_integrity_verify(iter, ptuple);
	else
		blk_integrity_set(iter, ptuple);

	if (likely(ptuple != &tuple)) {
		kunmap_local(ptuple);
	} else if (!verify) {
		blk_integrity_copy_from_tuple(iter->bip, &iter->prot_iter,
				ptuple, iter->bi->pi_tuple_size);
	}

	iter->interval_remaining = 1 << iter->bi->interval_exp;
	iter->csum = 0;
	/* 다음 interval로 이동: seed는 NVMe ref tag로, LBA 증가분에
	 * 해당한다. */
	iter->seed++;
	return ret;
}

/*
 * blk_integrity_iterate
 * 목적: bio의 데이터 구간을 순회하며 각 interval의 PI를 생성/검증한다.
 * 경로: bio_integrity_generate / bio_integrity_verify -> blk_integrity_iterate
 * NVMe 연결: NVMe 요청이 SQ(Send Queue)로 push되기 전(생성) 또는 CQ
 *   완료 후(검증)에 실행된다.
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
 *   -> nvme_submit_cmd(doorbell) 전후로 위치한다.
 */
static blk_status_t blk_integrity_iterate(struct bio *bio,
					  struct bvec_iter *data_iter,
					  bool verify)
{
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);
	struct bio_integrity_payload *bip = bio_integrity(bio);
	struct blk_integrity_iter iter = {
		.bio = bio,
		.bip = bip,
		.bi = bi,
		.data_iter = *data_iter,
		.prot_iter = bip->bip_iter,
		.interval_remaining = 1 << bi->interval_exp,
		.seed = data_iter->bi_sector,
		.csum = 0,
	};
	blk_status_t ret = BLK_STS_OK;

	while (iter.data_iter.bi_size && ret == BLK_STS_OK) {
		struct bio_vec bv = bvec_iter_bvec(iter.bio->bi_io_vec,
						   iter.data_iter);
		void *kaddr = bvec_kmap_local(&bv);
		void *data = kaddr;
		unsigned int len;

		bvec_iter_advance_single(iter.bio->bi_io_vec, &iter.data_iter,
					 bv.bv_len);
		while (bv.bv_len && ret == BLK_STS_OK) {
			len = min(iter.interval_remaining, bv.bv_len);
			blk_calculate_guard(&iter, data, len);
			bv.bv_len -= len;
			data += len;
			iter.interval_remaining -= len;
			if (!iter.interval_remaining)
				ret = blk_integrity_interval(&iter, verify);
		}
		kunmap_local(kaddr);
	}

	return ret;
}

/*
 * bio_integrity_generate
 * 목적: bio 발행 전 PI 메타데이터(guard/ref/app)를 생성한다.
 * 경로: submit_bio -> blk_mq_submit_bio -> ... -> bio_integrity_generate
 *   bio_integrity_generate -> blk_integrity_iterate -> blk_integrity_interval
 * NVMe 연결: NVMe controller로 명령을 doorbell로 제출하기 전, 호스트가
 *   PI 태그를 계산하여 PRACT=0/PRCHK=1 등의 설정과 함께 NVMe에 전달한다.
 */
void bio_integrity_generate(struct bio *bio)
{
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);

	switch (bi->csum_type) {
	case BLK_INTEGRITY_CSUM_CRC64:
	case BLK_INTEGRITY_CSUM_CRC:
	case BLK_INTEGRITY_CSUM_IP:
		blk_integrity_iterate(bio, &bio->bi_iter, false);
		break;
	default:
		break;
	}
}

/*
 * bio_integrity_verify
 * 목적: NVMe SSD에서 돌아온 완료 I/O의 PI를 검증한다.
 * 경로: I/O 완료 핸들러 -> bio_integrity_verify
 *   bio_integrity_verify -> blk_integrity_iterate -> blk_integrity_interval
 * NVMe 연결: NVMe CQ 완료 인터럽트 처리 후, DMA로 받은 데이터의
 *   guard/ref/app 태그를 다시 계산하여 비교한다.
 */
blk_status_t bio_integrity_verify(struct bio *bio, struct bvec_iter *saved_iter)
{
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);

	switch (bi->csum_type) {
	case BLK_INTEGRITY_CSUM_CRC64:
	case BLK_INTEGRITY_CSUM_CRC:
	case BLK_INTEGRITY_CSUM_IP:
		return blk_integrity_iterate(bio, saved_iter, true);
	default:
		break;
	}

	return BLK_STS_OK;
}

/*
 * Advance @iter past the protection offset for protection formats that
 * contain front padding on the metadata region.
 */
/*
 * blk_pi_advance_offset
 * 목적: 메타데이터 앞쪽 padding을 skip하여 실제 PI 필드 위치로 iter를
 *   이동한다.
 * 경로: blk_tuple_remap_begin -> blk_pi_advance_offset
 * NVMe 연결: NVMe formatted metadata에서 PI는 metadata_size 내
 *   pi_offset 위치에 있다 (추정).
 */
static void blk_pi_advance_offset(struct blk_integrity *bi,
				  struct bio_integrity_payload *bip,
				  struct bvec_iter *iter)
{
	unsigned int offset = bi->pi_offset;

	while (offset > 0) {
		struct bio_vec bv = mp_bvec_iter_bvec(bip->bip_vec, *iter);
		unsigned int len = min(bv.bv_len, offset);

		bvec_iter_advance_single(bip->bip_vec, iter, len);
		offset -= len;
	}
}

/*
 * blk_tuple_remap_begin
 * 목적: remapping 작업을 위해 현재 위치의 PI 튜플을 가져온다.
 * 경로: __blk_reftag_remap -> blk_tuple_remap_begin
 * NVMe 연결: NVMe ref tag remapping은 write 시점의 가상 주소와 물리
 *   주소 간 변환이 필요한 경우 사용된다 (추정).
 */
static void *blk_tuple_remap_begin(union pi_tuple *tuple,
				   struct blk_integrity *bi,
				   struct bio_integrity_payload *bip,
				   struct bvec_iter *iter)
{
	struct bvec_iter titer;
	struct bio_vec pbv;

	blk_pi_advance_offset(bi, bip, iter);
	pbv = bvec_iter_bvec(bip->bip_vec, *iter);
	if (likely(pbv.bv_len >= bi->pi_tuple_size))
		return bvec_kmap_local(&pbv);

	/*
	 * We need to preserve the state of the original iter for the
	 * copy_from_tuple at the end, so make a temp iter for here.
	 */
	titer = *iter;
	blk_integrity_copy_to_tuple(bip, &titer, tuple, bi->pi_tuple_size);
	return tuple;
}

/*
 * blk_tuple_remap_end
 * 목적: blk_tuple_remap_begin에서 매핑한 PI 튜플을 해제하고 iter를
 *   다음 interval로 진행시킨다.
 * 경로: __blk_reftag_remap -> blk_tuple_remap_end
 */
static void blk_tuple_remap_end(union pi_tuple *tuple, void *ptuple,
				struct blk_integrity *bi,
				struct bio_integrity_payload *bip,
				struct bvec_iter *iter)
{
	unsigned int len = bi->metadata_size - bi->pi_offset;

	if (likely(ptuple != tuple)) {
		kunmap_local(ptuple);
	} else {
		blk_integrity_copy_from_tuple(bip, iter, ptuple,
				bi->pi_tuple_size);
		len -= bi->pi_tuple_size;
	}

	bvec_iter_advance(bip->bip_vec, iter, len);
}

static void blk_set_ext_unmap_ref(struct crc64_pi_tuple *pi, u64 virt,
				  u64 ref_tag)
{
	u64 ref = get_unaligned_be48(&pi->ref_tag);

	if (ref == lower_48_bits(ref_tag) && ref != lower_48_bits(virt))
		put_unaligned_be48(virt, pi->ref_tag);
}

static void blk_set_t10_unmap_ref(struct t10_pi_tuple *pi, u32 virt,
				  u32 ref_tag)
{
	u32 ref = get_unaligned_be32(&pi->ref_tag);

	if (ref == ref_tag && ref != virt)
		put_unaligned_be32(virt, &pi->ref_tag);
}

/*
 * blk_reftag_remap_complete
 * 목적: I/O 완료 후 ref tag를 NVMe에 제출했던 물리 LBA에서
 *   호스트 가상 주소 기준으로 복원한다.
 * 경로: __blk_reftag_remap(..., prep=false) -> blk_reftag_remap_complete
 * NVMe 연결: 완료 시점의 ref tag는 NVMe가 사용한 물리 LBA와
 *   일치해야 한다 (추정).
 */
static void blk_reftag_remap_complete(struct blk_integrity *bi,
				      union pi_tuple *tuple, u64 virt, u64 ref)
{
	switch (bi->csum_type) {
	case BLK_INTEGRITY_CSUM_CRC64:
		blk_set_ext_unmap_ref(&tuple->crc64_pi, virt, ref);
		break;
	case BLK_INTEGRITY_CSUM_CRC:
	case BLK_INTEGRITY_CSUM_IP:
		blk_set_t10_unmap_ref(&tuple->t10_pi, virt, ref);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}
}

static void blk_set_ext_map_ref(struct crc64_pi_tuple *pi, u64 virt,
				u64 ref_tag)
{
	u64 ref = get_unaligned_be48(&pi->ref_tag);

	if (ref == lower_48_bits(virt) && ref != ref_tag)
		put_unaligned_be48(ref_tag, pi->ref_tag);
}

static void blk_set_t10_map_ref(struct t10_pi_tuple *pi, u32 virt, u32 ref_tag)
{
	u32 ref = get_unaligned_be32(&pi->ref_tag);

	if (ref == virt && ref != ref_tag)
		put_unaligned_be32(ref_tag, &pi->ref_tag);
}

/*
 * blk_reftag_remap_prepare
 * 목적: I/O 발행 전 ref tag를 호스트 가상 주소에서
 *   NVMe가 기대하는 물리 LBA 기준으로 변환한다.
 * 경로: __blk_reftag_remap(..., prep=true) -> blk_reftag_remap_prepare
 * NVMe 연결: NVMe PRCHK의 ref tag 검사가 활성화된 경우,
 *   명령에 실릴 ref tag를 물리 주소에 맞춘다.
 */
static void blk_reftag_remap_prepare(struct blk_integrity *bi,
				     union pi_tuple *tuple,
				     u64 virt, u64 ref)
{
	switch (bi->csum_type) {
	case BLK_INTEGRITY_CSUM_CRC64:
		blk_set_ext_map_ref(&tuple->crc64_pi, virt, ref);
		break;
	case BLK_INTEGRITY_CSUM_CRC:
	case BLK_INTEGRITY_CSUM_IP:
		blk_set_t10_map_ref(&tuple->t10_pi, virt, ref);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}
}

/*
 * __blk_reftag_remap
 * 목적: bio 내 모든 interval의 ref tag를 가상 주소와 물리 주소 간에
 *   변환한다(prepare: virt->ref, complete: ref->virt).
 * 경로: blk_integrity_remap -> __blk_reftag_remap
 * NVMe 연결: NVMe I/O에서는 LBA(ref tag)와 호스트 버퍼 주소(virt)가
 *   1:1로 대응하지만, DIX/remap이 필요한 상황에서 조정된다 (추정).
 */
static void __blk_reftag_remap(struct bio *bio, struct blk_integrity *bi,
			       unsigned *intervals, u64 *ref, bool prep)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);
	struct bvec_iter iter = bip->bip_iter;
	u64 virt = bip_get_seed(bip);
	union pi_tuple *ptuple;
	union pi_tuple tuple;

	if (prep && bip->bip_flags & BIP_MAPPED_INTEGRITY) {
		*ref += bio->bi_iter.bi_size >> bi->interval_exp;
		return;
	}
	/* 이미 remapping이 완료된 경우 interval 개수만큼 ref tag를
	 * 건드리지 않고 skip한다. */

	while (iter.bi_size && *intervals) {
		ptuple = blk_tuple_remap_begin(&tuple, bi, bip, &iter);

		if (prep)
			blk_reftag_remap_prepare(bi, ptuple, virt, *ref);
		else
			blk_reftag_remap_complete(bi, ptuple, virt, *ref);

		blk_tuple_remap_end(&tuple, ptuple, bi, bip, &iter);
		(*intervals)--;
		(*ref)++;
		virt++;
	}

	if (prep)
		bip->bip_flags |= BIP_MAPPED_INTEGRITY;
}

/*
 * blk_integrity_remap
 * 목적: request에 연결된 모든 bio의 ref tag를 remapping한다.
 * 경로: blk_integrity_prepare / blk_integrity_complete -> blk_integrity_remap
 * NVMe 연결: NVMe PRACT/PRCHK에서 ref tag는 시작 LBA에서 유도되며,
 *   interval_exp 단위로 증가한다.
 */
static void blk_integrity_remap(struct request *rq, unsigned int nr_bytes,
				bool prep)
{
	struct blk_integrity *bi = &rq->q->limits.integrity;
	u64 ref = blk_rq_pos(rq) >> (bi->interval_exp - SECTOR_SHIFT);
	unsigned intervals = nr_bytes >> bi->interval_exp;
	struct bio *bio;
	/* BLK_INTEGRITY_REF_TAG가 설정되어야 ref tag 검증/생성이 의미 있다. */

	if (!(bi->flags & BLK_INTEGRITY_REF_TAG))
		return;

	__rq_for_each_bio(bio, rq) {
		__blk_reftag_remap(bio, bi, &intervals, &ref, prep);
		if (!intervals)
			break;
	}
}

/*
 * blk_integrity_prepare
 * 목적: request 발행 전 ref tag를 물리 주소 기준으로 remapping한다.
 * 경로: request 제출 직전 -> blk_integrity_prepare
 * NVMe 연결: nvme_queue_rq -> nvme_submit_cmd(doorbell) 이전에 호출되어
 *   PI 메타데이터를 준비한다.
 */
void blk_integrity_prepare(struct request *rq)
{
	blk_integrity_remap(rq, blk_rq_bytes(rq), true);
}

/*
 * blk_integrity_complete
 * 목적: I/O 완료 후 ref tag를 다시 가상 주소 기준으로 복원한다.
 * 경로: request 완료 핸들러 -> blk_integrity_complete
 * NVMe 연결: NVMe CQ 완료 후, 상위 계층에 반환하기 전 PI 상태를
 *   정리한다.
 */
void blk_integrity_complete(struct request *rq, unsigned int nr_bytes)
{
	blk_integrity_remap(rq, nr_bytes, false);
}

/* NVMe 관점 핵심 요약 */
/*
 * - 이 파일은 NVMe end-to-end PI의 guard/ref/app 태그를 생성하고 검증한다.
 * - 생성 경로는 I/O 발행 전 bio_integrity_generate에서, 완료 검증은
 *   bio_integrity_verify에서 수행된다.
 * - NVMe SSD의 PRACT/PRCHK 비트가 어떤 태그를 검사할지 결정하며,
 *   이 파일은 호스트 측에서 필요한 태그를 사전에 계산한다 (추정).
 * - block/bio-integrity.c, block/integrity.c와 함께 동작하여
 *   block layer에서 NVMe PI를 지원한다.
 */
