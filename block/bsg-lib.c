// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  BSG helper library
 *
 *  Copyright (C) 2008   James Smart, Emulex Corporation
 *  Copyright (C) 2011   Red Hat, Inc.  All rights reserved.
 *  Copyright (C) 2011   Mike Christie
 */

/*
 * 파일 상단 요약 (NVMe 관점):
 *
 * block/bsg-lib.c는 BSG(Block SCSI Generic) 요청을 blk-mq 위에서 처리하는
 * 범용 헬퍼 라이브러리이다. NVMe 스택에서는 주로 NVMe-oF/NVMe-FC 등
 * 패브릭 트랜스포트가 사용자공간의 passthrough 명령(SG_IO v4)을 받아
 * 컨트롤러의 Admin Queue나 I/O Queue로 전달하는 경로를 제공한다.
 * 사용자공간 -> /dev/bsg/* -> bsg_transport_sg_io_fn -> blk_mq_execute_rq ->
 * bsg_queue_rq -> driver job_fn(예: nvme_fc_queue_rq) -> doorbell 갱신
 * 의 흐름을 따라간다.
 * block/bsg.c, drivers/nvme/host/fc.c 등과 밀접하게 연결된다.
 */

#include <linux/bsg.h>
#include <linux/slab.h>
#include <linux/blk-mq.h>
#include <linux/delay.h>
#include <linux/scatterlist.h>
#include <linux/bsg-lib.h>
#include <linux/export.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/sg.h>

#define uptr64(val) ((void __user *)(uintptr_t)(val))
// SG_IO v4 사용자공간 주소를 커널 포인터로 변환; NVMe Admin 명령 구조체 복사 시에도 동일한 uptr64 패턴이 사용됨.

/*
 * struct bsg_set: BSG 장치별 blk-mq 태그 집합과 콜백을 묶는 컨테이너
 *
 * @tag_set: blk-mq가 요청(request)를 할당/관리할 때 사용하는 태그 집합.
 *           NVMe SQ/CQ의 queue depth, hctx 수 등을 결정한다.
 * @bd:      bsg_register_queue()로 등록된 bsg_device. 사용자공간 /dev/bsg/*
 *           노드가 이 구조체를 통해 접근한다.
 * @job_fn:  드라이버가 등록한 bsg job 핸들러. NVMe-oF/FC 드라이버라면
 *           이 콜백에서 CQE 수신을 기다리는 nvme_queue_rq 유사 경로로
 *           전달된다 (추정).
 * @timeout_fn: 요청 timeout 시 호출되는 드라이버 콜백. NVMe CID 단위
 *              abort나 AER 비슷한 처리를 수행할 수 있다.
 */
struct bsg_set {
	struct blk_mq_tag_set	tag_set;
	struct bsg_device	*bd;
	bsg_job_fn		*job_fn;
	bsg_timeout_fn		*timeout_fn;
};

/*
 * bsg_transport_sg_io_fn - 사용자공간 SG_IO v4 명령을 BSG job으로 변환
 *
 * 목적:
 *   /dev/bsg/* 장치로 들어온 SG_IO v4 요청을 받아 blk-mq request를 할당하고,
 *   bsg_job을 채운 뒤 blk_execute_rq()로 하위 드라이버에 전달한다.
 *
 * NVMe 연결:
 *   NVMe-oF/FC 등에서 이 함수를 통해 사용자공간의 Admin 명령이나 벤더 특화
 *   명령이 호스트 드라이버로 날아간다. 데이터 방향은 dout_xfer_len /
 *   din_xfer_len으로 결정되며, PRP/SGL 빌드는 하위 드라이버에서 수행한다.
 *
 * 호출 경로 (추정):
 *   sg_io -> bsg_transport_sg_io_fn -> blk_mq_alloc_request ->
 *   blk_execute_rq -> bsg_queue_rq -> job_fn -> nvme_submit_cmd(doorbell)
 */
static int bsg_transport_sg_io_fn(struct request_queue *q, struct sg_io_v4 *hdr,
		bool open_for_write, unsigned int timeout)
{
	struct bsg_job *job;
	struct request *rq;
	struct bio *bio;
	void *reply;
	int ret;

	/* NVMe는 SCSI 프로토콜이 아니므로 일반적으로 이 함수는 NVMe-oF/FC
	 * 트랜스포트용 BSG 경로에서만 사용된다 (추정). */
	if (hdr->protocol != BSG_PROTOCOL_SCSI  ||
	    hdr->subprotocol != BSG_SUB_PROTOCOL_SCSI_TRANSPORT)
		return -EINVAL; // NVMe는 SCSI 프로토콜이 아니므로, NVMe-oF/FC 등 SCSI transport wrapper를 통한 passthrough 경로에서만 의미 있음 (추정).
	if (!capable(CAP_SYS_RAWIO))
		return -EPERM; // CAP_SYS_RAWIO 권한은 NVMe 벤더 특화/Admin 명령처럼 하드웨어에 직접 영향을 주는 raw I/O를 제한함.

	/* 데이터 전송 방향에 따라 REQ_OP_DRV_OUT(쓰기) 또는 REQ_OP_DRV_IN(읽기)
	 * 플래그를 선택한다. NVMe Admin 명령의 OPC와 유사한 개념이다. */
	rq = blk_mq_alloc_request(q, hdr->dout_xfer_len ?
			     REQ_OP_DRV_OUT : REQ_OP_DRV_IN, 0);
			     // blk-mq가 tag(CID)를 할당하여 NVMe SQ 슬롯을 확보; queue_depth 초과 시 -EAGAIN 또는 블록됨 (추정).
	if (IS_ERR(rq))
		return PTR_ERR(rq); // SQ 엔트리(tag) 확보 실패; NVMe 명령을 아직 생성하지 않고 상위에 반납함.
	rq->timeout = timeout; /* NVMe 명령 timeout과 동일한 역할 */
	// NVMe 명령의 timeout 값을 request에 복사; 이 값은 bsg_timeout -> timeout_fn에서 사용됨.

	/* request의 private data 영역(PDU)에서 bsg_job을 가져온다. */
	job = blk_mq_rq_to_pdu(rq);
	// request PDU에서 bsg_job을 가져옴; NVMe-oF 드라이버는 이 영역 뒤에 nvme_fc_fcp_op 등 private 데이터를 둠.
	reply = job->reply;
	// init_request에서 미리 할당한 NVMe CQE/response/status 버퍼 포인터를 보존함.
	memset(job, 0, sizeof(*job));
	// bsg_job 영역을 초기화; 기존 reply 포인터는 따로 복원해야 함.
	job->reply = reply;	/* reply 버퍼는 bsg_init_rq에서 미리 할당됨 */
	// reply 버퍼 복원; NVMe CQE의 DW3/DW0 status 영역이 담길 메모리.
	job->reply_len = SCSI_SENSE_BUFFERSIZE;
	// SCSI sense 버퍼 크기만큼 response 공간 확보; NVMe-oF/FC는 이를 CQE payload로 매핑 가능 (추정).
	job->dd_data = job + 1;	/* 드라이버 전용 데이터는 bsg_job 바로 뒤에 위치 */
	// bsg_job 바로 뒤에 위치한 driver private data; NVMe 트랜스포트의 queue_rq_private 구조체가 이 위치에 배치됨.

	job->request_len = hdr->request_len;
	// 사용자공간에서 복사할 NVMe 명령(예: Admin Command)의 길이 설정.
	job->request = memdup_user(uptr64(hdr->request), hdr->request_len);
	// userspace 명령 구조체(CDW0~CDW15 등)를 커널 메모리로 복사; 이후 job_fn에서 NVMe SQ 엔트리로 조립됨.
	if (IS_ERR(job->request)) {
		ret = PTR_ERR(job->request);
		goto out_free_rq; // NVMe command payload 복사 실패; CID/tag 반납 전 cleanup으로 진입.
	}

	/* 양방향(BIDI) 전송이 필요한 경우 din_xfer에 대해 별도 request를
	 * 할당한다. NVMe에는 일반적인 SGL보다 복잡한 형태지만, SCSI BSG
	 * 호환성을 위해 지원한다 (추정). */
	if (hdr->dout_xfer_len && hdr->din_xfer_len) {
		job->bidi_rq = blk_mq_alloc_request(rq->q, REQ_OP_DRV_IN, 0);
		// 양방향 전송 시 응답용 request를 추가 할당; NVMe SGL의 data-in 영역으로 매핑됨 (추정).
		if (IS_ERR(job->bidi_rq)) {
			ret = PTR_ERR(job->bidi_rq);
			goto out_free_job_request; // 응답용 request(tag) 할당 실패; 원본 request도 반납해야 함.
		}

		ret = blk_rq_map_user(rq->q, job->bidi_rq, NULL,
				uptr64(hdr->din_xferp), hdr->din_xfer_len,
				GFP_KERNEL);
				// 사용자공간 read 버퍼를 응답용 request bio에 매핑; NVMe PRP/SGL data-in의 소스가 됨.
		if (ret)
			goto out_free_bidi_rq; // 응답 버퍼 매핑 실패; bidi request를 해제하는 error path.

		job->bidi_bio = job->bidi_rq->bio;
		// bidi_rq의 bio를 저장; 완료 후 unmap 및 해제에 사용.
	} else {
		job->bidi_rq = NULL;
		job->bidi_bio = NULL;
		// 단방향 전송 시 bidi request 없음; NVMe Admin 명령의 일반적인 경우.
	}

	ret = 0;
	/* 사용자공간 버퍼를 request bio에 매핑한다. 이 매핑 결과는 NVMe
	 * PRP/SGL 생성의 입력이 된다. */
	if (hdr->dout_xfer_len) {
		ret = blk_rq_map_user(rq->q, rq, NULL, uptr64(hdr->dout_xferp),
				hdr->dout_xfer_len, GFP_KERNEL);
				// 사용자공간 write 버퍼를 request bio에 매핑; NVMe PRP/SGL data-out의 소스가 됨.
	} else if (hdr->din_xfer_len) {
		ret = blk_rq_map_user(rq->q, rq, NULL, uptr64(hdr->din_xferp),
				hdr->din_xfer_len, GFP_KERNEL);
				// read 방향 단방향 매핑; NVMe Admin 명령의 data-in buffer로 연결됨.
	}

	if (ret)
		goto out_unmap_bidi_rq; // DMA/PRP/SGL 매핑 실패; doorbell 전에 error completion으로 회복함.

	bio = rq->bio;
	// 매핑된 첫 번째 bio; blk_rq_map_sg()의 입력이 되어 NVMe SGL/PRP List를 구성함.
	/* request를 실제로 하위 드라이버로 제출한다. 여기부터가
	 * blk-mq -> nvme_queue_rq -> doorbell 갱신 단계로 진입하는 지점이다. */
	blk_execute_rq(rq, !(hdr->flags & BSG_FLAG_Q_AT_TAIL));
	// blk-mq dispatch 경로 진입: bsg_queue_rq -> job_fn -> nvme_queue_rq -> nvme_submit_cmd(doorbell). scheduler plug/batch를 거치지 않으므로 NVMe multi-queue parallelism은 하위 드라이버의 queue_rq에서 결정됨 (추정).

	/*
	 * The assignments below don't make much sense, but are kept for
	 * bug by bug backwards compatibility:
	 */
	hdr->device_status = job->result & 0xff;
	hdr->transport_status = host_byte(job->result);
	hdr->driver_status = 0;
	hdr->info = 0;
	if (hdr->device_status || hdr->transport_status || hdr->driver_status)
		hdr->info |= SG_INFO_CHECK; // job->result의 하위 바이트를 SCSI-style status로 매핑; NVMe CQE의 SC(DW3)을 사용자공간에 노출하는 호환 코드 (추정).
	hdr->response_len = 0;

	if (job->result < 0) {
		/* we're only returning the result field in the reply */
		job->reply_len = sizeof(u32);
		ret = job->result;
	}
	// NVMe 명령 실패 시 result를 그대로 반환하며 reply 길이를 최소화함.

	if (job->reply_len && hdr->response) {
		int len = min(hdr->max_response_len, job->reply_len);

		if (copy_to_user(uptr64(hdr->response), job->reply, len))
			ret = -EFAULT;
		else
			hdr->response_len = len;
	}
	// NVMe controller가 돌려준 response/status를 사용자공간 버퍼에 복사; copy_to_user 실패 시 -EFAULT.

	/* we assume all request payload was transferred, residual == 0 */
	hdr->dout_resid = 0;
	// data-out 잔여량은 0으로 가정; NVMe 명령이 전체 payload를 전송했다고 간주함.

	if (job->bidi_rq) {
		unsigned int rsp_len = job->reply_payload.payload_len;

		if (WARN_ON(job->reply_payload_rcv_len > rsp_len))
			hdr->din_resid = 0;
		else
			hdr->din_resid = rsp_len - job->reply_payload_rcv_len;
	} else {
		hdr->din_resid = 0;
	}
	// bidi 응답의 잔여량은 reply_payload_rcv_len 기준; NVMe CQE의 transferred length와 유사한 개념 (추정).

	blk_rq_unmap_user(bio);
	// data-out bio에 매핑된 사용자공간 버퍼를 해제; NVMe DMA 어드레스 정리.
out_unmap_bidi_rq:
	if (job->bidi_rq)
		blk_rq_unmap_user(job->bidi_bio);
		// data-in/bidi bio의 매핑 해제; PRP/SGL 자원 반납.
out_free_bidi_rq:
	if (job->bidi_rq)
		blk_mq_free_request(job->bidi_rq);
		// 응답용 request를 blk-mq에 반납; tag/CID를 해제하여 NVMe queue depth를 복원.
out_free_job_request:
	kfree(job->request);
	// 사용자공간에서 복사한 NVMe 명령 구조체 메모리 해제.
out_free_rq:
	blk_mq_free_request(rq);
	// 원본 request 반납; blk-mq tag/SQ slot을 해제함.
	return ret;
}

/**
 * bsg_teardown_job - routine to teardown a bsg job
 * @kref: kref inside bsg_job that is to be torn down
 */
/*
 * bsg_teardown_job - bsg_job의 생명주기 종료 및 자원 해제
 *
 * 목적:
 *   bsg_job의 참조 카운트가 0이 되면 호출되어, scatterlist, device
 *   참조, request를 정리한다.
 *
 * NVMe 연결:
 *   NVMe-oF/FC에서 CQE 수신 완료 후, CID를 해제하고 해당 request를
 *   blk-mq에 반납하기 전에 이 함수가 호출된다.
 */
static void bsg_teardown_job(struct kref *kref)
{
	struct bsg_job *job = container_of(kref, struct bsg_job, kref);
	// kref가 0이 되면 bsg_job을 역참조하여 생명주기를 종료함.
	struct request *rq = blk_mq_rq_from_pdu(job);
	// job이 속한 blk-mq request; NVMe CQ 인터럽트 완료 후 최종 해제 대상.

	put_device(job->dev);	/* release reference for the request */
	// controller/transport 장치에 대한 참조를 해제; NVMe controller remove 시 queue drain과 연결됨.

	kfree(job->request_payload.sg_list);
	kfree(job->reply_payload.sg_list);
	// 요청/응답 scatterlist 메모리 해제; NVMe SGL/PRP List의 intermediate 버퍼 정리.

	blk_mq_end_request(rq, BLK_STS_OK);
	// request를 blk-mq에 최종 반납; tag(CID) 해제 -> NVMe SQ slot 재사용 가능.
}

/*
 * bsg_job_put - bsg_job의 참조 카운트를 감소
 *
 * NVMe 관점:
 *   nvme_queue_rq 등에서 request를 마지막으로 참조하는 주체가 줄어들 때
 *   호출되며, 참조가 0이 되면 bsg_teardown_job -> blk_mq_end_request로
 *   연결된다.
 */
void bsg_job_put(struct bsg_job *job)
{
	kref_put(&job->kref, bsg_teardown_job);
	// atomic하게 참조 카운트를 감소; NVMe request의 마지막 참조 해제 시 teardown으로 진입.
}
EXPORT_SYMBOL_GPL(bsg_job_put);

/*
 * bsg_job_get - bsg_job의 참조 카운트를 안전하게 증가
 */
int bsg_job_get(struct bsg_job *job)
{
	return kref_get_unless_zero(&job->kref);
	// 참조 카운트를 안전하게 증가; NVMe completion 경로와 timeout 경로가 동시에 접근하는 것을 방지함.
}
EXPORT_SYMBOL_GPL(bsg_job_get);

/**
 * bsg_job_done - completion routine for bsg requests
 * @job: bsg_job that is complete
 * @result: job reply result
 * @reply_payload_rcv_len: length of payload recvd
 *
 * The LLD should call this when the bsg job has completed.
 */
/*
 * bsg_job_done - 하위 드라이버가 BSG job 완료를 알리는 함수
 *
 * 목적:
 *   LLD(예: nvme_fc_queue_rq 이후의 완료 처리기)가 job 처리가 끝났을 때
 *   호출하여 result와 수신 payload 길이를 기록하고, blk-mq 완료 경로로
 *   넘긴다.
 *
 * 호출 경로 (추정):
 *   nvme_fc_complete_rq(CQE 처리) -> bsg_job_done ->
 *   blk_mq_complete_request -> bsg_complete -> bsg_job_put
 */
void bsg_job_done(struct bsg_job *job, int result,
		  unsigned int reply_payload_rcv_len)
{
	struct request *rq = blk_mq_rq_from_pdu(job);
	// 완료될 blk-mq request; NVMe CQE 처리 후 이 request에 대한 softirq complete를 트리거함.

	job->result = result;
	job->reply_payload_rcv_len = reply_payload_rcv_len;
	// 실제 수신된 payload 길이를 기록; NVMe CQE의 DW0(transferred bytes)에 대응 (추정).
	if (likely(!blk_should_fake_timeout(rq->q)))
		blk_mq_complete_request(rq);
		// blk-mq softirq 완료 경로로 진입; bsg_complete -> bsg_job_put -> bsg_teardown_job 순으로 이어짐.
}
EXPORT_SYMBOL_GPL(bsg_job_done);

/**
 * bsg_complete - softirq done routine for destroying the bsg requests
 * @rq: BSG request that holds the job to be destroyed
 */
/*
 * bsg_complete - blk-mq softirq 완료 콜백
 *
 * 목적:
 *   blk-mq complete 단계에서 request를 직접 종료하지 않고, bsg_job_put을
 *   통해 참조 카운트 기반으로 해제한다.
 *
 * NVMe 연결:
 *   NVMe CQ 인터럽트 핸들러 -> blk_mq_complete_request -> bsg_complete
 *   순서로 도달한다 (추정).
 */
static void bsg_complete(struct request *rq)
{
	struct bsg_job *job = blk_mq_rq_to_pdu(rq);
	// 완료된 request의 bsg_job을 가져와 참조 카운트를 감소시킴.

	bsg_job_put(job);
	// NVMe ISR/softirq 완료 시점에서 request 참조를 해제하고 메모리 정리를 시작함.
}

/*
 * bsg_map_buffer - request의 bio를 scatterlist로 변환
 *
 * 목적:
 *   request에 연결된 bio를 sg_list로 변환하여, 하위 드라이버가 DMA
 *   맵핑이나 PRP/SGL 생성에 사용할 수 있게 한다.
 *
 * NVMe 연결:
 *   NVMe PRP List나 SGL을 만들기 전 단계로, req->nr_phys_segments만큼
 *   scatterlist를 할당하고 blk_rq_map_sg()로 채운다.
 */
static int bsg_map_buffer(struct bsg_buffer *buf, struct request *req)
{
	size_t sz = (sizeof(struct scatterlist) * req->nr_phys_segments);
	// scatterlist 크기는 req->nr_phys_segments에 의존; NVMe PRP/SGL segment 수의 상한을 결정함.

	BUG_ON(!req->nr_phys_segments);
	// 물리 세그먼트가 0이면 NVMe 데이터 전송이 없음에도 sg_list를 할당한 불가능한 경우.

	buf->sg_list = kmalloc(sz, GFP_KERNEL);
	if (!buf->sg_list)
		return -ENOMEM; // sg_list 메모리 할당; NVMe SGL/PRP List 생성을 위한 intermediate buffer.
	sg_init_table(buf->sg_list, req->nr_phys_segments);
	// scatterlist 테이블 초기화; 후속 blk_rq_map_sg()로 bio bvec을 채움.
	buf->sg_cnt = blk_rq_map_sg(req, buf->sg_list);
	// req->bio의 bvec들을 scatterlist로 변환; NVMe PRP List 또는 SGL 구성의 직접 입력.
	buf->payload_len = blk_rq_bytes(req);
	// request의 총 전송 바이트 수; NVMe command CDW10-CDW13 length 필드에 반영됨. integrity, crypto, discard/write-zeroes 경로는 bsg-lib에서 직접 다루지 않으며 NVMe-oF/FC 드라이버가 별도 처리 (추정).
	return 0;
}

/**
 * bsg_prepare_job - create the bsg_job structure for the bsg request
 * @dev: device that is being sent the bsg request
 * @req: BSG request that needs a job structure
 */
/*
 * bsg_prepare_job - request에 bsg_job을 구성
 *
 * 목적:
 *   blk-mq request의 PDU 영역을 bsg_job으로 초기화하고, 요청/응답
 *   scatterlist를 구성한 뒤 device 참조를 획득한다.
 *
 * 호출 경로:
 *   bsg_queue_rq -> bsg_prepare_job -> bsg_map_buffer -> blk_rq_map_sg
 */
static bool bsg_prepare_job(struct device *dev, struct request *req)
{
	struct bsg_job *job = blk_mq_rq_to_pdu(req);
	int ret;

	job->timeout = req->timeout;
	// blk-mq request의 timeout을 bsg_job에 복사; NVMe command timeout 처리의 기준.

	if (req->bio) {
		ret = bsg_map_buffer(&job->request_payload, req);
		if (ret)
			goto failjob_rls_job;
	}
	// 데이터 전송이 있는 경우에만 request bio를 sg_list로 변환; NVMe data-out/data-in SGL 빌드 시작.
	if (job->bidi_rq) {
		ret = bsg_map_buffer(&job->reply_payload, job->bidi_rq);
		if (ret)
			goto failjob_rls_rqst_payload;
	}
	// 양방향 응답 request도 sg_list로 변환; NVMe Admin/vendor command의 별도 response buffer에 해당 (추정).
	job->dev = dev;
	/* take a reference for the request */
	get_device(job->dev);
	// request 처리 동안 controller/transport 장치가 제거되지 않도록 참조 획득.
	kref_init(&job->kref);
	// bsg_job의 참조 카운트를 1로 초기화; NVMe request의 생명주기 시작.
	return true;

failjob_rls_rqst_payload:
	kfree(job->request_payload.sg_list);
	// request_payload sg_list 할당 실패 시 정리; doorbell 이전에 실패하므로 abort 없이 반납.
failjob_rls_job:
	job->result = -ENOMEM;
	return false;
	// NVMe 명령 생성 단계에서 자원 부족; bsg_queue_rq는 BLK_STS_IOERR를 반환함.
}

/**
 * bsg_queue_rq - generic handler for bsg requests
 * @hctx: hardware queue
 * @bd: queue data
 *
 * On error the create_bsg_job function should return a -Exyz error value
 * that will be set to ->result.
 *
 * Drivers/subsys should pass this to the queue init function.
 */
/*
 * bsg_queue_rq - blk-mq의 queue_rq 콜백: BSG request를 드라이버로 전달
 *
 * 목적:
 *   blk-mq가 request를 꺼내 bsg_job을 준비하고, 드라이버가 등록한
 *   job_fn을 호출한다.
 *
 * NVMe 연결:
 *   NVMe-oF/FC 호스트 드라이버는 이 콜백을 통해 SQ 엔트리를 채우고
 *   doorbell을 갱신한다. 일반 NVMe 블록 장치(/dev/nvme*)는 이 경로를
 *   직접 사용하지 않는다 (추정).
 *
 * 호출 경로 (추정):
 *   blk_mq_submit_bio -> blk_mq_get_request -> bsg_queue_rq ->
 *   bset->job_fn -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 */
static blk_status_t bsg_queue_rq(struct blk_mq_hw_ctx *hctx,
				 const struct blk_mq_queue_data *bd)
{
	struct request_queue *q = hctx->queue;
	// hardware queue context; NVMe-oF/FC에서 이 q는 Admin/SQ에 해당하는 단일 큐 (추정).
	struct device *dev = q->queuedata;
	// queue의 queuedata는 NVMe controller/transport 구조체; controller 생존 여부 확인에 사용.
	struct request *req = bd->rq;
	// dispatch할 blk-mq request; tag(CID)가 이미 할당된 상태.
	struct bsg_set *bset =
		container_of(q->tag_set, struct bsg_set, tag_set);
		// struct bsg_set는 tag_set, job_fn, timeout_fn을 묶음; NVMe SQ depth, completion, abort 콜백의 집합체.
	blk_status_t sts = BLK_STS_IOERR;
	int ret;

	blk_mq_start_request(req);
	// request의 timeout 타이머 시작 및 tag 사용 표시; NVMe doorbell 전 상태 기록.

	if (!get_device(dev))
		return BLK_STS_IOERR; // controller가 dying/removed 상태이면 request를 즉시 거부; NVMe queue drain 중인 상황.

	if (!bsg_prepare_job(dev, req))
		goto out; // sg_list 생성/장치 참조 실패; doorbell 갱신 없이 BLK_STS_IOERR 반환.

	ret = bset->job_fn(blk_mq_rq_to_pdu(req));
	// 드라이버의 queue_rq 핸들러 호출; NVMe-oF/FC라면 nvme_fc_queue_rq -> SQ 엔트리 채우기 (추정).
	if (!ret)
		sts = BLK_STS_OK; // NVMe 명령이 SQ에 성공적으로 기록되었고, doorbell 갱신까지 마침; 비동기 완료를 대기.

out:
	put_device(dev);
	return sts; // job_fn 실행 후 controller 참조 해제; 이후 completion/timeout에서 별도 참조를 유지함.
}

/* called right after the request is allocated for the request_queue */
/*
 * bsg_init_rq - request 할당 직후 호출되는 초기화 콜백
 *
 * NVMe 관점:
 *   request가 할당될 때마다 SCSI sense 버퍼 크기만큼 reply 버퍼를
 *   미리 할당해 둔다. NVMe-oF/FC에서도 CQE의 status 영역을 담는 버퍼로
 *   활용될 수 있다 (추정).
 */
static int bsg_init_rq(struct blk_mq_tag_set *set, struct request *req,
		       unsigned int hctx_idx, unsigned int numa_node)
{
	struct bsg_job *job = blk_mq_rq_to_pdu(req);
	// 초기화할 request의 PDU 영역에서 bsg_job 획득.

	job->reply = kzalloc(SCSI_SENSE_BUFFERSIZE, GFP_KERNEL);
	if (!job->reply)
		return -ENOMEM; // NVMe CQE status/response를 저장할 버퍼를 미리 할당; completion 시 copy_to_user로 전달.
	return 0;
}

/*
 * bsg_exit_rq - request 해제 직전 호출되는 정리 콜백
 */
static void bsg_exit_rq(struct blk_mq_tag_set *set, struct request *req,
		       unsigned int hctx_idx)
{
	struct bsg_job *job = blk_mq_rq_to_pdu(req);

	kfree(job->reply);
	// request 해제 전 NVMe response 버퍼 반납.
}

/*
 * bsg_remove_queue - BSG 장치와 blk-mq 큐를 제거
 *
 * NVMe 연결:
 *   NVMe-oF/FC 컨트롤러 제거 시 호출되며, /dev/bsg/* 노드 해제와
 *   tag_set, request_queue 메모리를 반납한다.
 */
void bsg_remove_queue(struct request_queue *q)
{
	if (q) {
		struct bsg_set *bset =
			container_of(q->tag_set, struct bsg_set, tag_set);
			// tag_set으로부터 bsg_set 복원; NVMe queue depth, timeout, complete 콜백이 포함됨.

		bsg_unregister_queue(bset->bd);
		// /dev/bsg/* 노드를 제거; 사용자공간의 추가 NVMe passthrough 명령 차단.
		blk_mq_destroy_queue(q);
		// blk-mq request_queue를 파괴; 진행 중인 request를 drain하고 tag를 반납함.
		blk_put_queue(q);
		// request_queue의 마지막 참조를 해제; NVMe controller remove 시 queue 생명주기 종료.
		blk_mq_free_tag_set(&bset->tag_set);
		// tag_set 메모리 반납; NVMe SQ/CQ의 CID/tag pool을 해제.
		kfree(bset);
		// bsg_set 컨테이너 해제; NVMe-oF/FC BSG 인터페이스 최종 정리.
	}
}
EXPORT_SYMBOL_GPL(bsg_remove_queue);

/*
 * bsg_timeout - blk-mq timeout 콜백
 *
 * NVMe 연결:
 *   NVMe 명령이 일정 시간 내 CQE를 돌려주지 않으면 blk-mq가 이 함수를
 *   호출한다. timeout_fn이 등록되어 있으면 드라이버가 Abort나 Reset 등의
 *   recovery를 수행할 수 있다.
 */
static enum blk_eh_timer_return bsg_timeout(struct request *rq)
{
	struct bsg_set *bset =
		container_of(rq->q->tag_set, struct bsg_set, tag_set);
		// timeout_fn이 등록되어 있으면 controller-level abort/recovery 수행.

	if (!bset->timeout_fn)
		return BLK_EH_DONE; // 별도 abort 핸들러가 없으면 blk-mq가 request를 즉시 종료; NVMe 명령이 orphan 될 수 있음 (추정).
	return bset->timeout_fn(rq);
	// NVMe timeout handler 호출; CID 단위 abort 또는 controller reset 등 recovery 진행 (추정).
}

/*
 * bsg_mq_ops - blk-mq에 등록되는 연산자 구조체
 *
 * 각 필드는 blk-mq가 BSG 요청의 생명주기에 맞춰 호출하는 콜백을
 * 가리킨다. NVMe SQ/CQ의 queue depth, timeout, complete 경로를
 * 사용자공간 BSG 인터페이스에 노출하는 핵심 구조체이다.
 */
static const struct blk_mq_ops bsg_mq_ops = {
	.queue_rq		= bsg_queue_rq,
	.init_request		= bsg_init_rq,
	.exit_request		= bsg_exit_rq,
	.complete		= bsg_complete,
	.timeout		= bsg_timeout,
};

/**
 * bsg_setup_queue - Create and add the bsg hooks so we can receive requests
 * @dev: device to attach bsg device to
 * @name: device to give bsg device
 * @lim: queue limits for the bsg queue
 * @job_fn: bsg job handler
 * @timeout: timeout handler function pointer
 * @dd_job_size: size of LLD data needed for each job
 */
/*
 * bsg_setup_queue - BSG 장치와 blk-mq 큐를 생성하고 등록
 *
 * 목적:
 *   NVMe-oF/FC 등의 드라이버가 컨트롤러나 큐를 초기화할 때 BSG 인터페이스를
 *   만든다. tag_set, request_queue, bsg_device를 순서대로 생성하고 연결한다.
 *
 * NVMe 연결:
 *   queue_depth = 128은 NVMe SQ/CQ 엔트리 수와 직결된다 (추정).
 *   cmd_size에 sizeof(struct bsg_job) + dd_job_size를 더해, request PDU
 *   안에 bsg_job과 드라이버 전용 데이터를 함께 배치한다.
 *
 * 연결 파일:
 *   block/bsg.c의 bsg_register_queue와 함께 /dev/bsg/* 노드를 만든다.
 */
struct request_queue *bsg_setup_queue(struct device *dev, const char *name,
		struct queue_limits *lim, bsg_job_fn *job_fn,
		bsg_timeout_fn *timeout, int dd_job_size)
		// @lim: queue_limits는 max_segments, max_segment_size, integrity 등 NVMe PRP/SGL/보안 규칙을 담음 (추정).
{
	struct bsg_set *bset;
	struct blk_mq_tag_set *set;
	struct request_queue *q;
	int ret = -ENOMEM;

	bset = kzalloc_obj(*bset);
	if (!bset) // bsg_set 컨테이너 할당 실패; NVMe-oF/FC BSG 인터페이스 초기화 중단.
		return ERR_PTR(-ENOMEM);

	bset->job_fn = job_fn;
	// NVMe-oF/FC 드라이버의 queue_rq job 핸들러 등록; bsg_queue_rq -> job_fn 호출.
	bset->timeout_fn = timeout;
	// NVMe timeout/abort handler 등록; blk-mq timeout 시 abort/recovery 경로.

	set = &bset->tag_set;
	set->ops = &bsg_mq_ops;
	// blk-mq ops 등록; queue_rq/init_request/exit_request/complete/timeout 콜백을 NVMe 생명주기에 연결.
	set->nr_hw_queues = 1; /* NVMe-oF/FC는 단일 hctx로 충분한 경우가 많음 (추정) */
	// passthrough/Admin 경로용 단일 hctx; NVMe-oF/FC Admin Queue 한 쌍에 대응 (추정).
	set->queue_depth = 128; /* NVMe SQ/CQ queue depth 후보 값 */
	// NVMe SQ/CQ의 엔트리 수(queue depth)를 128로 설정; CID/tag 할당 상한.
	set->numa_node = NUMA_NO_NODE;
	// NUMA node 미지정; NVMe-oF/FC 패브릭 환경에서는 노드 바인딩이 불분명할 수 있음.
	set->cmd_size = sizeof(struct bsg_job) + dd_job_size;
	// request PDU 안에 bsg_job + driver private data 배치; NVMe command context가 tag와 함께 pre-allocate됨.
	set->flags = BLK_MQ_F_BLOCKING;
	// job_fn이 sleep 가능; NVMe-oF/FC에서는 fabric 연결/재전송 등 blocking 작업 가능 (추정).
	if (blk_mq_alloc_tag_set(set))
		goto out_tag_set; // tag_set(CID pool) 할당 실패; NVMe queue 생성 실패.

	q = blk_mq_alloc_queue(set, lim, dev);
	if (IS_ERR(q)) {
		ret = PTR_ERR(q);
		goto out_queue; // request_queue 할당 실패; NVMe SQ/CQ 소프트웨어 큐 생성 실패.
	}

	blk_queue_rq_timeout(q, BLK_DEFAULT_SG_TIMEOUT);
	// 기본 SG timeout을 request_queue에 설정; NVMe 명령의 기본 timeout 값.

	bset->bd = bsg_register_queue(q, dev, name, bsg_transport_sg_io_fn, NULL);
	if (IS_ERR(bset->bd)) {
		ret = PTR_ERR(bset->bd);
		goto out_cleanup_queue;
	}
	// /dev/bsg/* 노드 등록 실패; 사용자공간 NVMe passthrough 접점이 생성되지 않음.

	return q;
out_cleanup_queue:
	blk_mq_destroy_queue(q);
	blk_put_queue(q);
out_queue:
	blk_mq_free_tag_set(set);
out_tag_set:
	kfree(bset);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(bsg_setup_queue);

/* NVMe 관점 핵심 요약
 *
 * - block/bsg-lib.c는 사용자공간 SG_IO v4 요청을 blk-mq 기반 BSG job으로
 *   변환하여 NVMe-oF/FC 등의 패브릭 트랜스포트가 Admin/passthrough 명령을
 *   처리할 수 있게 한다.
 * - blk_mq_submit_bio -> blk_mq_get_request -> bsg_queue_rq ->
 *   job_fn -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 순으로 NVMe
 *   명령이 전달된다 (추정).
 * - struct bsg_set은 blk_mq_tag_set, bsg_device, job_fn, timeout_fn을
 *   묶어 NVMe SQ/CQ의 queue depth, timeout, 완료 경로를 사용자공간에
 *   노출하는 컨테이너 역할을 한다.
 * - block/bsg.c의 bsg_register_queue/unregister_queue와 짝을 이루어
 *   /dev/bsg/* 노드를 생성/제거하며, drivers/nvme/host/fc.c 등에서
 *   실제 NVMe-oF/FC 동작을 구현한다.
 * - 본 파일은 NVMe 블록 I/O의 일반 경로(block/blk-mq.c, block/mq-deadline.c
 *   등)와는 별개의 passthrough/관리 경로이며, NVMe-oF/FC 환경에서만
 *   직접적으로 활용된다 (추정).
 */
