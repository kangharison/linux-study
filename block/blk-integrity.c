// SPDX-License-Identifier: GPL-2.0
/*
 * blk-integrity.c - Block layer data integrity extensions
 *
 * Copyright (C) 2007, 2008 Oracle Corporation
 * Written by: Martin K. Petersen <martin.petersen@oracle.com>
 *
 * ============================================================================
 * NVMe 관점 파일 요약
 * ----------------------------------------------------------------------------
 * 이 파일은 블록 계층(block layer)의 데이터 무결성(integrity) 인프라를 구성한다.
 * NVMe SSD 입장에서 본 I/O 경로는 다음과 같이 연결된다:
 *
 *     submit_bio() / submit_bio_noacct()
 *         -> blk_mq_submit_bio()
 *             -> blk_mq_get_request()
 *             -> nvme_queue_rq() (drivers/nvme/host/pci.c 등)
 *                 -> nvme_setup_cmd() -> nvme_submit_cmd(doorbell)
 *
 * 본 파일은 위 경로에서 bio/ request에 연결된 무결성 메타데이터(보호 정보,
 * Protection Information, PI)를 관리한다. NVMe의 PI 기능(End-to-end Data
 * Protection, LBA range 타입, PRP/SGL에 따른 메타데이터 배치)은 컨트롤러
 * 낮은 계층에서 처리되지만, 메타데이터의 존재 여부, 세그먼트 수, 병합 가능
 * 여부, checksum 프로파일 등은 blk-integrity.c에서 블록 계층 차원에서
 * 준비된다. 따라서 본 파일은 NVMe 데이터 경로의 "무결성 프론트엔드"에 해당.
 *
 * 논리적으로는 blk-mq(blk-mq.c, blk-mq-sched.c)에서 생성/병합된 request의
 * integrity 속성을 검증하고, drivers/nvme/host/의 nvme_queue_rq()가
 * 실제 PRP/SGL과 doorbell 갱신을 수행하기 전 단계를 담당한다.
 * ============================================================================
 */

#include <linux/blk-integrity.h>
#include <linux/backing-dev.h>
#include <linux/mempool.h>
#include <linux/bio.h>
#include <linux/scatterlist.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/t10-pi.h>

#include "blk.h"

/**
 * blk_rq_count_integrity_sg - Count number of integrity scatterlist elements
 * @q:		request queue
 * @bio:	bio with integrity metadata attached
 *
 * Description: Returns the number of elements required in a
 * scatterlist corresponding to the integrity metadata in a bio.
 *
 * NVMe 연결:
 *  - NVMe I/O 처리 흐름에서 blk_mq_get_request() 이후 request에 bio가
 *    연결되면, nvme_queue_rq() -> nvme_setup_rw()에서 PRP/SGL 엔트리를
 *    생성하기 위해 무결성 메타데이터의 sg 개수가 필요하다.
 *  - NVMe SQ(Submission Queue)에 기록되는 명령은 데이터 버퍼 뿐 아니라
 *    선택적 PI 메타데이터 버퍼도 PRP/SGL로 묘사될 수 있다(Format with
 *    metadata). 이 함수는 그 sg 엔트리 수를 계산한다.
 *  - 호출 경로 예시: bio_integrity_prep() -> blk_rq_count_integrity_sg()
 *    -> request->nr_integrity_segments 갱신 -> nvme_queue_rq에서 SGL/PRP
 *    구성에 참고.
 */
int blk_rq_count_integrity_sg(struct request_queue *q, struct bio *bio)
{
	struct bio_vec iv, ivprv = { NULL };	/* 현재/이전 integrity 벡터; 각 bvec은 PI 메타데이터 물리 페이지를 가리키며 NVMe PRP/SGL 엔트리 후보 */
	unsigned int segments = 0;		/* 최종 sg 세그먼트 수 (NVMe PRP/SGL 엔트리 수 추정) */
	unsigned int seg_size = 0;		/* 현재 누적 세그먼트 크기; NVMe SGL segment 한도 추적용 */
	struct bvec_iter iter;			/* bio_integrity 벡터 순회용; NVMe DMA sg 엔트리 생성 전 단계 */
	int prev = 0;				/* 첫 번째 integrity bvec 여부; 연속성 판단 상태 */

	/* bio의 무결성 메타데이터 영역을 순회; 데이터 영역과 별도의 bvec이다. */
	bio_for_each_integrity_vec(iv, bio, iter) {

		if (prev) {
			/* 이전 bvec과 현재 bvec의 물리 연속성 검사;
			 * 연속이면 NVMe SGL segment 하나로 합쳐진다. */
			if (!biovec_phys_mergeable(q, &ivprv, &iv))
				goto new_segment;
			/* queue_max_segment_size()는 DMA/PCIe 메모리 매핑 단위를
			 * 고려한 값; NVMe 컨트롤러의 max sdata segment 크기와
			 * 관련될 수 있다(추정). */
			if (seg_size + iv.bv_len > queue_max_segment_size(q))
				goto new_segment;

			seg_size += iv.bv_len;	/* 현재 segment의 누적 크기 갱신 */
		} else {
new_segment:
			segments++;		/* 새로운 sg/PRP/SGL 엔트리 추가 (추정) */
			seg_size = iv.bv_len;	/* 새 segment 시작; NVMe PRP/SGL의 다음 엔트리 크기 */
		}

		prev = 1;			/* 다음 루프부터 연속성 비교 가능 */
		ivprv = iv;			/* 이전 벡터 저장; 다음 iter에서 biovec_phys_mergeable() 입력 */
	}

	return segments;
}

/**
 * blk_get_meta_cap - 파일 시스템이 볼 수 있는 블록 디바이스의 무결성 능력 조회
 * @bdev:	블록 디바이스
 * @cmd:	ioctl 명령
 * @argp:	사용자 공간 구조체 포인터
 *
 * NVMe 연결:
 *  - NVMe 컨트롤러는 Identify Namespace/Controller 데이터를 통해
 *    LBA Format, metadata size, PI type(end-to-end data protection)을
 *    보고한다. nvme driver는 이 정보를 request_queue->limits.integrity에
 *    매핑하여 본 함수가 사용자 공간에 노출한다.
 *  - LBMD_PI_CSUM_CRC64_NVME는 NVMe CRC64 guard 타입을 의미하며,
 *    NVMe 2.0 PI 확장(e.g., 64-bit Guard)과 직결된다.
 */
int blk_get_meta_cap(struct block_device *bdev, unsigned int cmd,
		     struct logical_block_metadata_cap __user *argp)
{
	struct blk_integrity *bi;	/* NVMe Identify Namespace의 PI 능력이 매핑된 블록 계층 포인터 */
	struct logical_block_metadata_cap meta_cap = {};	/* 사용자 공간으로 복사할 capability 구조체; NVMe LBA Format의 PI 필드를 추상화 */
	size_t usize = _IOC_SIZE(cmd);	/* ioctl 크기 검증; compatibility ioctl 경로 */

	if (!extensible_ioctl_valid(cmd, FS_IOC_GETLBMD_CAP, LBMD_SIZE_VER0))
		return -ENOIOCTLCMD;

	/* bdev의 request_queue에서 무결성 프로파일 획득; NVMe namespace
	 * 등록 시 nvme driver가 설정한 값이다. */
	bi = blk_get_integrity(bdev->bd_disk);
	if (!bi)
		goto out;			/* NVMe namespace가 PI를 노출하지 않는 경우(NVMe 1.0 / PI 미지원 / disabled) */

	/* NVMe 컨트롤러가 End-to-end Data Protection을 지원하는지 여부
	 * (ID_CTRL.DPS bit 연동 추정). */
	if (bi->flags & BLK_INTEGRITY_DEVICE_CAPABLE)
		meta_cap.lbmd_flags |= LBMD_PI_CAP_INTEGRITY;
	/* NVMe Reference Tag(Logical Block Reference Tag) 검사 지원. */
	if (bi->flags & BLK_INTEGRITY_REF_TAG)
		meta_cap.lbmd_flags |= LBMD_PI_CAP_REFTAG;
	/* interval_exp: 2의 거듭제곱 형태의 보호 구간 크기;
	 * NVMe PI는 일반적으로 논리 블록 단위(LBA Data Size)의 배수로
	 * 구간을 설정한다(추정). */
	meta_cap.lbmd_interval = 1 << bi->interval_exp;
	/* metadata_size: NVMe LBA Format에서 보고하는 메타데이터 바이트 수;
	 * PI tuple(Guard/App/Ref) + opaque 영역을 포함할 수 있다. */
	meta_cap.lbmd_size = bi->metadata_size;
	/* NVMe PRCHK/PRACT가 참조하는 Guard/App/Ref tuple 크기. */
	meta_cap.lbmd_pi_size = bi->pi_tuple_size;
	/* 메타데이터 내 PI tuple 위치; NVMe Format에 따른 메타데이터 배치(처음/끝/별도 버퍼)와 연관. */
	meta_cap.lbmd_pi_offset = bi->pi_offset;
	/* PI tuple 외 NVMe namespace별 추가 메타데이터 영역. */
	meta_cap.lbmd_opaque_size = bi->metadata_size - bi->pi_tuple_size;
	/* PI tuple이 메타데이터 선두가 아닌 경우 opaque offset 조정. */
	if (meta_cap.lbmd_opaque_size && !bi->pi_offset)
		meta_cap.lbmd_opaque_offset = bi->pi_tuple_size;

	switch (bi->csum_type) {
	case BLK_INTEGRITY_CSUM_NONE:
		meta_cap.lbmd_guard_tag_type = LBMD_PI_CSUM_NONE;
		break;
	case BLK_INTEGRITY_CSUM_IP:
		/* NVMe IP checksum guard 지원 시(추정). */
		meta_cap.lbmd_guard_tag_type = LBMD_PI_CSUM_IP;
		break;
	case BLK_INTEGRITY_CSUM_CRC:
		/* NVMe CRC16/T10-DIF guard; 가장 일반적인 NVMe PI-16 모드. */
		meta_cap.lbmd_guard_tag_type = LBMD_PI_CSUM_CRC16_T10DIF;
		break;
	case BLK_INTEGRITY_CSUM_CRC64:
		/* NVMe CRC64 guard; T10-DIF 64-bit 확장 또는 NVMe PI-64
		 * 모드와 매핑(추정). */
		meta_cap.lbmd_guard_tag_type = LBMD_PI_CSUM_CRC64_NVME;
		break;
	}

	/* PI 검사가 활성화된 NVMe namespace에 대해 App Tag 필드 존재. */
	if (bi->csum_type != BLK_INTEGRITY_CSUM_NONE)
		meta_cap.lbmd_app_tag_size = 2;	/* NVMe APP TAG는 2바이트(T10 PI 표준) */

	/* Ref Tag 크기는 checksum 종류에 따라 다름; NVMe PI Type1/Type2/Type3와 매핑. */
	if (bi->flags & BLK_INTEGRITY_REF_TAG) {
		switch (bi->csum_type) {
		case BLK_INTEGRITY_CSUM_CRC64:
			meta_cap.lbmd_ref_tag_size =
				sizeof_field(struct crc64_pi_tuple, ref_tag);
			break;
		case BLK_INTEGRITY_CSUM_CRC:
		case BLK_INTEGRITY_CSUM_IP:
			meta_cap.lbmd_ref_tag_size =
				sizeof_field(struct t10_pi_tuple, ref_tag);
			break;
		default:
			break;
		}
	}

out:
	/* 사용자 공간 ioctl 결과 복사; 실패 시 -EFAULT. */
	return copy_struct_to_user(argp, usize, &meta_cap, sizeof(meta_cap),
				   NULL);
}

/**
 * blk_rq_integrity_map_user - 사용자 공간 버퍼의 무결성 메타데이터를 bio에 매핑
 * @rq:		블록 요청(request)
 * @ubuf:	사용자 공간 메타데이터 버퍼
 * @bytes:	매핑할 바이트 수
 *
 * NVMe 연결:
 *  - NVMe passthrough(ioctl) 경로에서 사용자가 직접 PI 메타데이터를
 *    제공할 때 사용된다(추정).
 *  - bio_integrity_map_user()로 매핑 후 request->nr_integrity_segments를
 *    갱신; 이 값은 nvme_queue_rq()가 PRP/SGL을 만들 때 필요한 엔트리 수를
 *    판단하는 데 참고된다.
 *  - REQ_INTEGRITY 플래그가 설정되면 이후 NVMe 커맨드(READ/WRITE)에
 *    PRACT/PRCHK 비트를 설정할지 여부를 결정하는 데 영향을 준다(추정).
 */
int blk_rq_integrity_map_user(struct request *rq, void __user *ubuf,
			      ssize_t bytes)
{
	int ret;				/* bio_integrity_map_user() 반환값; NVMe passthrough 메타데이터 매핑 성공/실패 */
	struct iov_iter iter;			/* 사용자 공간 버퍼를 기술; NVMe SQ 명령의 PI 메타데이터 소스 */

	/* READ/WRITE 방향에 따라 사용자 버퍼 iterator 초기화;
	 * NVMe opcode 방향과 일치해야 함. */
	iov_iter_ubuf(&iter, rq_data_dir(rq), ubuf, bytes);
	/* PI 메타데이터를 bio integrity payload로 매핑; NVMe passthrough에서만 사용(추정). */
	ret = bio_integrity_map_user(rq->bio, &iter);
	if (ret)
		return ret;			/* 매핑 실패 시 NVMe 명령 생성 전 단계에서 리턴; -ENOMEM/-EINVAL 등 */

	/* 무결성 세그먼트 수를 계산하여 request에 기록;
	 * nvme_setup_prps()/nvme_setup_sgl() 등에서 DMA sg 매핑 시 활용
	 * (NVMe driver 낮은 계층으로 전달됨). */
	rq->nr_integrity_segments = blk_rq_count_integrity_sg(rq->q, rq->bio);
	rq->cmd_flags |= REQ_INTEGRITY;	/* 이 request에 PI 메타데이터가 있음을 표시 */
	return 0;
}
EXPORT_SYMBOL_GPL(blk_rq_integrity_map_user);

/**
 * blk_integrity_merge_rq - 두 request를 병합할 때 무결성 호환성 검사
 * @q:		request queue
 * @req:		기존 request
 * @next:	병합 후보 request
 *
 * NVMe 연결:
 *  - NVMe SSD는 I/O scheduler/plug-merge 단계에서 request 병합으로
 *    SQ 명령 수를 줄이고 throughput을 높인다.
 *  - 병합 시 PI 메타데이터가 서로 다륾면 NVMe 컨트롤러의 PRCHK/PRACT
 *    정책이 달라져 하나의 SQ 명령(CID)으로 처리할 수 없으므로 병합 불가.
 *  - max_integrity_segments는 NVMe PRP/SGL 최대 세그먼트 수 제한과
 *    연동될 수 있다(추정).
 *  - 호출 경로: blk_attempt_req_merge() -> blk_integrity_merge_rq()
 */
bool blk_integrity_merge_rq(struct request_queue *q, struct request *req,
			    struct request *next)
{
	struct bio_integrity_payload *bip, *bip_next;	/* 각 request의 bio에 부착된 PI payload; NVMe PRCHK/PRACT 설정의 근원 */

	/* 양쪽 모두 PI 메타데이터가 없으면 병합 가능; NVMe에서는 일반
	 * READ/WRITE 명령으로 처리. */
	if (blk_integrity_rq(req) == 0 && blk_integrity_rq(next) == 0)
		return true;

	/* 한쪽에만 PI가 있으면 동일한 CID 하나로는 서로 다른 PI 정책을
	 * 처리할 수 없어 병합 거부. */
	if (blk_integrity_rq(req) == 0 || blk_integrity_rq(next) == 0)
		return false;

	bip = bio_integrity(req->bio);		/* req의 첫 bio에서 PI payload 획득; NVMe PRCHK 설정 원본 */
	bip_next = bio_integrity(next->bio);	/* next request의 PI payload 획득 */
	/* BIPFlags: BIP_CHECK_APPTAG 등 NVMe PRCHK 비트와 대응되는
	 * 블록 계층 무결성 검사 플래그(추정). */
	if (bip->bip_flags != bip_next->bip_flags)
		return false;

	/* app_tag가 다륾면 NVMe APP TAG 검사 정책이 달라 병합 불가. */
	if (bip->bip_flags & BIP_CHECK_APPTAG &&
	    bip->app_tag != bip_next->app_tag)
		return false;

	/* integrity 세그먼트 수 합이 queue 한도를 초과하면 NVMe SGL/PRP
	 * 엔트리 한도를 넘을 수 있으므로 병합 불가(추정). */
	if (req->nr_integrity_segments + next->nr_integrity_segments >
	    q->limits.max_integrity_segments)
		return false;

	/* 두 request 사이에 PI 메타데이터의 논리적 연속성이 없으면 병합
	 * 불가; NVMe Ref Tag 연속성과 관련. */
	if (integrity_req_gap_back_merge(req, next->bio))
		return false;

	return true;
}

/**
 * blk_integrity_merge_bio - request에 새로운 bio를 병합할 때 무결성 검사
 * @q:		request queue
 * @req:		기존 request
 * @bio:		병합 후보 bio
 *
 * NVMe 연결:
 *  - bio-based I/O 경로에서 blk_mq_submit_bio() 또는 scheduler가
 *    request에 bio를 붙이기 전에 호출.
 *  - blk_integrity_merge_rq()와 동일한 원리로, PI 정책이 다륾면
 *    동일한 NVMe SQ 명령으로 묶을 수 없다.
 *  - 병합 가능하면 최종적으로 nvme_queue_rq()가 하나의 request를
 *    하나의 CID로 변환.
 */
bool blk_integrity_merge_bio(struct request_queue *q, struct request *req,
			     struct bio *bio)
{
	struct bio_integrity_payload *bip, *bip_bio = bio_integrity(bio);	/* req/bio의 PI payload; NVMe PRCHK/PRACT 정책 비교용 */
	int nr_integrity_segs;							/* 추가 bio가 기여할 sg/PRP/SGL 엔트리 수 */

	/* 양쪽 모두 PI 없음: 일반 NVMe READ/WRITE로 통합 가능. */
	if (blk_integrity_rq(req) == 0 && bip_bio == NULL)
		return true;

	/* PI 정책 불일치: 동일 CID 명령 처리 불가. */
	if (blk_integrity_rq(req) == 0 || bip_bio == NULL)
		return false;

	bip = bio_integrity(req->bio);		/* 기존 req의 PI 정책 획득 */
	if (bip->bip_flags != bip_bio->bip_flags)
		return false;

	if (bip->bip_flags & BIP_CHECK_APPTAG &&
	    bip->app_tag != bip_bio->app_tag)
		return false;

	/* 추가될 bio의 integrity sg 수를 계산하여 NVMe SGL/PRP 한도
	 * 초과 여부 판단(추정). */
	nr_integrity_segs = blk_rq_count_integrity_sg(q, bio);
	if (req->nr_integrity_segments + nr_integrity_segs >
	    q->limits.max_integrity_segments)
		return false;

	return true;
}

/* device 구조체에서 request_queue의 limits.integrity 포인터를 얻음;
 * NVMe namespace block device 등록 시 이 구조체가 초기화된다(추정). */
static inline struct blk_integrity *dev_to_bi(struct device *dev)
{
	return &dev_to_disk(dev)->queue->limits.integrity;	/* gendisk -> request_queue -> integrity limits 체인 */
}

/**
 * blk_integrity_profile_name - 무결성 checksum 프로파일의 문자열 이름 반환
 * @bi:		blk_integrity 구조체
 *
 * NVMe 연결:
 *  - NVMe 컨트롤러는 Identify Controller/DPRS(End-to-end Data Protection
 *    Support) 필드를 통해 CRC16/CRC64/IP guard를 지원 여부를 보고.
 *  - 본 함수는 sysfs를 통해 사용자 공간에 "CRC64" 등의 프로파일을
 *    노출하며, NVMe namespace의 PI format과 일치해야 한다(추정).
 *  - EXT-DIF-TYPE1-CRC64는 NVMe 64-bit guard PI 모드를 의미.
 */
const char *blk_integrity_profile_name(struct blk_integrity *bi)
{
	switch (bi->csum_type) {
	case BLK_INTEGRITY_CSUM_IP:
		if (bi->flags & BLK_INTEGRITY_REF_TAG)
			return "T10-DIF-TYPE1-IP";	/* NVMe Type1 + IP checksum PI(추정) */
		return "T10-DIF-TYPE3-IP";		/* NVMe Type3 + IP checksum PI(추정) */
	case BLK_INTEGRITY_CSUM_CRC:
		if (bi->flags & BLK_INTEGRITY_REF_TAG)
			return "T10-DIF-TYPE1-CRC";	/* NVMe Type1 + CRC16 guard; PRCHK.Guard 활성 */
		return "T10-DIF-TYPE3-CRC";		/* NVMe Type3 + CRC16 guard */
	case BLK_INTEGRITY_CSUM_CRC64:
		/* NVMe PI-64(CRC64 guard)에 대응하는 확장 DIF 프로파일. */
		if (bi->flags & BLK_INTEGRITY_REF_TAG)
			return "EXT-DIF-TYPE1-CRC64";	/* NVMe Type1 + CRC64 guard(추정) */
		return "EXT-DIF-TYPE3-CRC64";		/* NVMe Type3 + CRC64 guard(추정) */
	case BLK_INTEGRITY_CSUM_NONE:
		break;
	}

	return "nop";	/* NVMe PI 미사용 namespace; PRCHK/PRACT 무효 */
}
EXPORT_SYMBOL_GPL(blk_integrity_profile_name);

/* sysfs 통해 integrity 플래그를 반전(inverted)하여 저장:
 * val=1이면 플래그 클리어, val=0이면 플래그 설정. */
static ssize_t flag_store(struct device *dev, const char *page, size_t count,
		unsigned char flag)
{
	struct request_queue *q = dev_to_disk(dev)->queue;	/* NVMe namespace의 request_queue; limits.integrity 갱신 대상 */
	struct queue_limits lim;				/* 임시 queue limits; integrity.flags 외에도 다른 한도 보존 */
	unsigned long val;					/* sysfs에서 읽은 0/1 값; NVMe PI 소프트웨어 검증 정책 토글 */
	int err;

	err = kstrtoul(page, 10, &val);
	if (err)
		return err;

	/* note that the flags are inverted vs the values in the sysfs files */
	lim = queue_limits_start_update(q);		/* queue_limits 업데이트 시작; NVMe queue depth/segment 등 다른 한도는 그대로 */
	if (val)
		lim.integrity.flags &= ~flag;		/* 플래그 클리어: NVMe PI 커널 검증/생성 비활성화(추정) */
	else
		lim.integrity.flags |= flag;		/* 플래그 설정: NVMe PI 커널 검증/생성 활성화(추정) */

	err = queue_limits_commit_update_frozen(q, &lim);	/* 업데이트 커밋; NVMe I/O 경로에 새로운 PI 정책 반영(추정) */
	if (err)
		return err;
	return count;
}

/* sysfs 플래그 값을 읽어옴; 저장 시 반전되었으므로 읽을 때도 반전. */
static ssize_t flag_show(struct device *dev, char *page, unsigned char flag)
{
	struct blk_integrity *bi = dev_to_bi(dev);

	return sysfs_emit(page, "%d\n", !(bi->flags & flag));
}

/* 현재 설정된 무결성 format 이름을 sysfs에 노출; NVMe namespace의
 * PI format과 대응. */
static ssize_t format_show(struct device *dev, struct device_attribute *attr,
			   char *page)
{
	struct blk_integrity *bi = dev_to_bi(dev);

	if (!bi->metadata_size)
		return sysfs_emit(page, "none\n");	/* NVMe PI 미지원 또는 metadata_size=0 namespace */
	return sysfs_emit(page, "%s\n", blk_integrity_profile_name(bi));
}

/* NVMe PI의 APP TAG 크기(바이트)를 sysfs에 노출; Type1/Type2/Type3에
 * 따라 달라질 수 있다(추정). */
static ssize_t tag_size_show(struct device *dev, struct device_attribute *attr,
			     char *page)
{
	struct blk_integrity *bi = dev_to_bi(dev);

	return sysfs_emit(page, "%u\n", bi->tag_size);
}

/* 보호 구간(protection interval) 크기를 sysfs에 노출;
 * NVMe LBA Data Size의 배수 형태. */
static ssize_t protection_interval_bytes_show(struct device *dev,
					      struct device_attribute *attr,
					      char *page)
{
	struct blk_integrity *bi = dev_to_bi(dev);

	return sysfs_emit(page, "%u\n",
			  bi->interval_exp ? 1 << bi->interval_exp : 0);
}

/* READ 시 커널이 PI 검증을 수행할지 여부 설정; NVMe 컨트롤러가 이미
 * PRCHK로 검증을 수행하더라도 상위에서 추가 검증 가능. */
static ssize_t read_verify_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *page, size_t count)
{
	return flag_store(dev, page, count, BLK_INTEGRITY_NOVERIFY);
}

static ssize_t read_verify_show(struct device *dev,
				struct device_attribute *attr, char *page)
{
	return flag_show(dev, page, BLK_INTEGRITY_NOVERIFY);
}

/* WRITE 시 커널이 PI를 생성할지 여부 설정; NVMe 컨트롤러의 PRACT=1
 * 모드에서 커널이 Guard/App/Ref 태그를 채우는 경우와 관련(추정). */
static ssize_t write_generate_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *page, size_t count)
{
	return flag_store(dev, page, count, BLK_INTEGRITY_NOGENERATE);
}

static ssize_t write_generate_show(struct device *dev,
				   struct device_attribute *attr, char *page)
{
	return flag_show(dev, page, BLK_INTEGRITY_NOGENERATE);
}

/* NVMe 컨트롤러가 End-to-end Data Protection(PI)을 지원하는지 여부를
 * sysfs에 노출; ID_CTRL.DPS/ID_NS.dps 참고(추정). */
static ssize_t device_is_integrity_capable_show(struct device *dev,
						struct device_attribute *attr,
						char *page)
{
	struct blk_integrity *bi = dev_to_bi(dev);

	return sysfs_emit(page, "%u\n",
			  !!(bi->flags & BLK_INTEGRITY_DEVICE_CAPABLE));
}

static DEVICE_ATTR_RO(format);		/* /sys/block/nvme*n*/integrity/format: NVMe PI 프로파일 문자열 */
static DEVICE_ATTR_RO(tag_size);	/* /sys/block/nvme*n*/integrity/tag_size: NVMe APP TAG 크기(바이트) */
static DEVICE_ATTR_RO(protection_interval_bytes);	/* /sys/block/nvme*n*/integrity/protection_interval_bytes: NVMe PI 보호 구간 */
static DEVICE_ATTR_RW(read_verify);	/* /sys/block/nvme*n*/integrity/read_verify: NVMe READ 후 커널 PI 검증 토글 */
static DEVICE_ATTR_RW(write_generate);	/* /sys/block/nvme*n*/integrity/write_generate: NVMe WRITE 전 커널 PI 생성 토글 */
static DEVICE_ATTR_RO(device_is_integrity_capable);	/* /sys/block/nvme*n*/integrity/device_is_integrity_capable: NVMe PI 하드웨어 지원 여부 */

/* integrity 관련 sysfs 속성 그룹; NVMe namespace block device의
 * /sys/block/nvme*n*/integrity/ 아래 항목으로 노출. */
static struct attribute *integrity_attrs[] = {
	&dev_attr_format.attr,
	&dev_attr_tag_size.attr,
	&dev_attr_protection_interval_bytes.attr,
	&dev_attr_read_verify.attr,
	&dev_attr_write_generate.attr,
	&dev_attr_device_is_integrity_capable.attr,
	NULL
};

const struct attribute_group blk_integrity_attr_group = {
	.name = "integrity",		/* sysfs 디렉터리 이름; NVMe namespace별 PI 노출 경로 */
	.attrs = integrity_attrs,	/* 위에서 정의한 NVMe PI sysfs 속성 배열 */
};

/*
 * NVMe 관점 핵심 요약
 *
 * - 이 파일은 NVMe I/O 경로에서 bio/request 수준의 무결성 메타데이터(PI)
 *   를 준비하며, 실제 NVMe SQ/CID/doorbell 단계는 drivers/nvme/host/에서
 *   처리된다. 즉, blk-integrity.c는 NVMe PI의 "블록 계층 프론트엔드"다.
 *
 * - 주요 호출 경로: blk_mq_submit_bio -> blk_mq_get_request ->
 *   (bio 병합 시 blk_integrity_merge_rq/bio) -> nvme_queue_rq ->
 *   nvme_setup_cmd -> nvme_submit_cmd(doorbell). 본 파일은 병합/세그먼트
 *   수/capability 조회 단계를 담당한다.
 *
 * - blk_integrity 구조체의 csum_type(CRC/CRC64/IP)과 flags(DEVICE_CAPABLE,
 *   REF_TAG 등)는 NVMe Identify Controller/Namespace의 PI 능력과 직결되며,
 *   sysfs를 통해 사용자 공간에 노출된다.
 *
 * - blk_rq_count_integrity_sg()로 구한 sg 수는 NVMe PRP List 또는 SGL
 *   엔트리 수를 결정하는 데 참고된다(추정).
 *
 * - blk_integrity_merge_rq/bio()는 PI 정책이 다른 request/bio가 하나의
 *   NVMe 명령(CID)으로 묶이지 않도록 병합을 차단한다.
 */
