// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  BSG helper library
 *
 *  Copyright (C) 2008   James Smart, Emulex Corporation
 *  Copyright (C) 2011   Red Hat, Inc.  All rights reserved.
 *  Copyright (C) 2011   Mike Christie
 */
/*
 * [한국어 설명] BSG(Block SCSI Generic) 헬퍼 라이브러리 (bsg-lib.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SCSI/전달(passthrough) 계열 명령을 사용자공간이 블록 디바이스가 아닌
 * 문자 디바이스(/dev/bsg 아래 노드)를 통해 하드웨어로 직접 전달할 수 있게 해주는 BSG
 * 프레임워크의 blk-mq 기반 공용 헬퍼 라이브러리이다. FC/SAS HBA, NVMe-oF/FC
 * 호스트 드라이버 등 표준 SCSI 커맨드셋을 벗어난 관리/벤더 명령이 필요한
 * LLD(Low-Level Driver, 저수준 드라이버)들은 bsg_setup_queue() 한 번의 호출로
 * 전용 request_queue와 문자 디바이스 노드를 동시에 얻을 수 있다. blk_mq_tag_set/
 * blk_mq_ops 배선, struct bsg_job의 할당·초기화·완료·해제, sg_io_v4 기반
 * 사용자공간 프로토콜 처리를 모두 이 파일이 대신 구현해 준다. 이 파일이 없다면
 * 각 LLD가 blk-mq tag set 설정, request<->job 변환, kref 기반 비동기 참조
 * 카운팅, SG_IO v4 ioctl 파싱을 개별적으로 재구현해야 하므로 코드 중복과 버그
 * 표면적이 크게 늘어난다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (사용자공간 -> 하드웨어, 매 I/O 마다):
 *   사용자공간 ioctl(fd, SG_IO, &sg_io_v4)
 *     -> block/bsg.c (문자 디바이스 파일 오퍼레이션, 이 웨이브에서 다른 에이전트가
 *        동시 작업 중일 수 있어 본 파일 작업에서는 직접 읽지 않았다)
 *     -> [이 파일] bsg_transport_sg_io_fn()   SG_IO v4 -> blk-mq request 변환
 *     -> blk_mq_alloc_request() / blk_rq_map_user()   사용자 버퍼 pin
 *     -> blk_execute_rq()                     동기 제출(완료까지 프로세스를 블록)
 *     -> blk-mq 디스패치 -> [이 파일] bsg_queue_rq()  (bsg_mq_ops.queue_rq)
 *     -> [이 파일] bsg_prepare_job()            request -> bsg_job 변환/scatterlist 매핑
 *     -> LLD의 job_fn (예: FC transport class 디스패처)  실제 하드웨어 제출
 *     -> (하드웨어 인터럽트) LLD 완료 핸들러
 *     -> [이 파일] bsg_job_done()               job->result 기록 + blk_mq_complete_request()
 *     -> [이 파일] bsg_complete() (bsg_mq_ops.complete) -> bsg_job_put()
 *        -> (참조카운트 0) bsg_teardown_job() -> blk_mq_end_request()
 *     -> blk_execute_rq() 의 대기자가 깨어남
 *     -> bsg_transport_sg_io_fn() 로 복귀 -> hdr(sg_io_v4) 필드 채움 -> 사용자공간 반환
 * LLD 초기화(probe) 시점에는 bsg_setup_queue() -> blk_mq_alloc_tag_set()/
 * blk_mq_alloc_queue() -> bsg_register_queue()(block/bsg.c) 순으로 큐와 문자
 * 디바이스가 함께 준비된다.
 * 실행 컨텍스트: bsg_transport_sg_io_fn/bsg_prepare_job/bsg_queue_rq 는 ioctl을
 * 호출한 사용자 프로세스 컨텍스트에서 실행되며(BLK_MQ_F_BLOCKING 이므로 sleep
 * 가능), bsg_job_done()은 LLD의 인터럽트/워커 컨텍스트, bsg_complete()/
 * bsg_teardown_job()은 blk-mq 완료 경로, bsg_timeout()은 blk-mq 타임아웃
 * 워크큐 컨텍스트에서 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - block/bsg.c: bsg_register_queue()/bsg_unregister_queue()로 /dev/bsg 아래의 문자
 *     디바이스 노드를 생성/제거한다. 사용자공간의 SG_IO ioctl은 bsg.c가 먼저 받아
 *     sg_io_v4를 파싱한 뒤 이 파일의 bsg_transport_sg_io_fn을 호출한다.
 *   - include/linux/blk-mq.h: struct blk_mq_tag_set/blk_mq_ops/request_queue,
 *     blk_mq_alloc_request()/blk_mq_alloc_tag_set()/blk_mq_alloc_queue()/
 *     blk_mq_rq_to_pdu()/blk_mq_complete_request()/blk_mq_end_request() 등
 *     이 파일이 구현하는 모든 blk-mq 콜백과 헬퍼의 타입/원형을 제공한다.
 *   - include/linux/bsg-lib.h (본 저장소에는 미체크아웃): struct bsg_job과
 *     struct bsg_buffer, bsg_job_fn/bsg_timeout_fn 타입, bsg_job_done()/
 *     bsg_job_get()/bsg_job_put()/bsg_setup_queue()/bsg_remove_queue()의 공개
 *     선언이 위치한 헤더. struct bsg_job 자체의 필드 정의는 이 헤더에 있으므로
 *     본 파일에서는 실제 사용 맥락(설정자/읽는 자)을 기준으로 주석을 단다.
 *   - scsi/scsi_cmnd.h, scsi/sg.h: SCSI_SENSE_BUFFERSIZE, struct sg_io_v4,
 *     BSG_PROTOCOL_*, BSG_SUB_PROTOCOL_*, BSG_FLAG_Q_AT_TAIL, SG_INFO_CHECK,
 *     host_byte() 등 SCSI/BSG 사용자공간 ABI 상수와 매크로.
 * 이 파일에 의존하는 모듈(LLD 측):
 *   - FC/SAS transport class(drivers/scsi/scsi_transport_fc.c 유형), NVMe-oF/FC
 *     호스트 드라이버 등이 probe 시 bsg_setup_queue(dev, name, lim, job_fn,
 *     timeout_fn, dd_job_size)를 호출해 자신만의 job_fn/timeout_fn을 등록하고,
 *     remove 시 bsg_remove_queue()를 호출한다. 완료 인터럽트 핸들러에서는
 *     bsg_job_done()을 호출해 결과를 통지한다.
 * 데이터 흐름: 사용자공간 버퍼(요청 CDB, dout/din 데이터, response 버퍼)
 *   -> sg_io_v4 구조체 -> bsg_job(request/request_payload/reply/reply_payload)
 *   -> LLD가 채우는 job->reply/job->result -> 다시 sg_io_v4 필드로 역변환되어
 *   사용자공간에 반환된다.
 * 공유 핵심 자료구조: struct bsg_job(이 파일 밖 bsg-lib.h에 정의, request와
 *   1:1로 blk-mq PDU 영역에 내장), struct bsg_set(이 파일이 정의, LLD별
 *   tag_set+콜백 컨테이너), struct request(blk-mq 코어가 관리, bsg_job은 그
 *   PDU 바로 뒤에 위치).
 *
 * === 주요 함수/구조체 요약 ===
 * struct bsg_set            — LLD 하나당 하나씩 존재하는 tag_set+bsg_device+
 *   job_fn/timeout_fn 컨테이너.
 * bsg_setup_queue()          — LLD probe에서 호출: tag_set 초기화 -> request_queue
 *   생성 -> bsg 문자 디바이스 등록.
 * bsg_transport_sg_io_fn()   — SG_IO v4 사용자공간 요청을 blk-mq request+bsg_job
 *   으로 변환, 동기 실행 후 결과를 사용자공간에 회신.
 * bsg_queue_rq()             — blk-mq queue_rq 콜백: request를 job으로 준비
 *   (bsg_prepare_job) 한 뒤 LLD의 job_fn 호출.
 * bsg_prepare_job()/bsg_map_buffer() — request의 bio를 scatterlist로 변환해
 *   job->request_payload/reply_payload에 채움.
 * bsg_job_done()/bsg_job_get()/bsg_job_put()/bsg_teardown_job() — LLD 완료
 *   통지부터 kref 기반 job 해제까지의 수명주기.
 * bsg_init_rq()/bsg_exit_rq() — tag_set 슬롯 단위로 reply(sense) 버퍼를 미리
 *   할당/해제.
 * bsg_remove_queue()          — bsg_setup_queue()의 역순 해제.
 */
#include <linux/bsg.h> /* [한국어] bsg_register_queue()/bsg_unregister_queue(), struct bsg_device: /dev/bsg 아래의 문자 디바이스 노드 등록·해제 API (구현은 block/bsg.c) */
#include <linux/slab.h> /* [한국어] kzalloc()/kmalloc()/kfree(): bsg_job/bsg_set/scatterlist 배열 등 이 파일 전역의 동적 할당에 사용 */
#include <linux/blk-mq.h> /* [한국어] struct blk_mq_tag_set/blk_mq_ops/request_queue, blk_mq_alloc_request/alloc_tag_set/alloc_queue/rq_to_pdu 등 이 파일이 구현하는 모든 blk-mq 인터페이스의 원형 */
#include <linux/delay.h> /* [한국어] msleep 계열 지연 함수 선언 - 이 파일에서 직접 호출하는 곳은 없으나 관례적으로 포함(과거 폴링 대기 코드의 잔재로 추정) */
#include <linux/scatterlist.h> /* [한국어] struct scatterlist, sg_init_table(): bsg_map_buffer()가 request의 bio를 LLD에 넘길 scatter-gather 리스트로 변환할 때 사용 */
#include <linux/bsg-lib.h> /* [한국어] struct bsg_job/bsg_buffer, bsg_job_fn/bsg_timeout_fn 타입과 bsg_job_done()/get()/put()/setup_queue()/remove_queue()의 공개 선언 - 이 파일이 구현하는 API 표면 그 자체 */
#include <linux/export.h> /* [한국어] EXPORT_SYMBOL_GPL 매크로 - bsg_job_put/get/done, bsg_setup_queue/remove_queue를 모듈로 빌드된 LLD(HBA 드라이버)에서 호출 가능하게 함 */
#include <scsi/scsi_cmnd.h> /* [한국어] SCSI_SENSE_BUFFERSIZE(고정 sense/reply 버퍼 크기), host_byte() 매크로: SCSI result 인코딩(driver_byte<<24|host_byte<<16|status_byte) 해석에 사용 */
#include <scsi/sg.h> /* [한국어] struct sg_io_v4, BSG_PROTOCOL_*, BSG_SUB_PROTOCOL_*, BSG_FLAG_Q_AT_TAIL, SG_INFO_CHECK: SG_IO v4 사용자공간 ABI 구조체와 상수 */

/* [한국어] uptr64(val) - sg_io_v4가 64비트 정수로 저장하는 사용자공간 포인터 필드를
 *   실제 __user 포인터 타입으로 되돌리는 매크로. sg_io_v4는 32/64비트 호환성을 위해
 *   포인터 필드를 항상 __u64로 저장하므로(예: hdr->request, hdr->dout_xferp), 커널
 *   내부에서 memdup_user()/copy_to_user()/blk_rq_map_user() 등에 넘기기 전에 이
 *   매크로로 (void __user *)로 캐스팅해야 한다. uintptr_t를 경유하는 이유는 64비트
 *   정수를 포인터 폭이 다른 아키텍처(예: 32비트)에서도 안전하게 절단·확장하기 위함. */
#define uptr64(val) ((void __user *)(uintptr_t)(val))

/*
 * [한국어]
 * struct bsg_set - LLD(저수준 드라이버) 하나가 bsg_setup_queue()로 만든 bsg
 *   큐 하나에 대응하는 컨테이너 구조체
 *
 * bsg_setup_queue()가 kzalloc_obj()로 힙에 동적 할당하며, 내부에 blk_mq_tag_set을
 * '포인터가 아니라 구조체 자체'로 embed하고 있어 tag_set과 bset은 항상 같은 메모리
 * 블록 안에 함께 존재한다. 이 덕분에 blk-mq 콜백(bsg_queue_rq/bsg_timeout 등)에서
 * container_of(q->tag_set, struct bsg_set, tag_set) 한 번으로 bset 전체를 역참조할
 * 수 있다 - q->tag_set은 blk_mq_alloc_queue(set, lim, dev) 호출 시 &bset->tag_set
 * 주소로 설정되기 때문이다(block/blk-mq.c blk_mq_alloc_queue 참고).
 */
struct bsg_set {
	struct blk_mq_tag_set	tag_set;
	/* [한국어] 이 bsg 큐 전용 blk-mq 태그 집합(요청 슬롯 풀 + 콜백 vtable).
	 * 설정자: bsg_setup_queue()가 ops=&bsg_mq_ops, nr_hw_queues=1, queue_depth=128,
	 *   numa_node=NUMA_NO_NODE, cmd_size=sizeof(struct bsg_job)+dd_job_size,
	 *   flags=BLK_MQ_F_BLOCKING을 채운 뒤 blk_mq_alloc_tag_set()으로 초기화한다.
	 * 읽는 자: bsg_queue_rq()/bsg_timeout()이 q->tag_set(블록 코어가 blk_mq_alloc_queue
	 *   호출 시 &bset->tag_set으로 설정)을 container_of로 역참조해 bset을 복원할 때
	 *   이 필드의 시작 주소가 기준점이 된다. bsg_remove_queue()도 동일하게 역참조 후
	 *   blk_mq_free_tag_set()으로 해제한다.
	 * 값 범위: blk_mq_alloc_tag_set() 성공 후에는 유효한 태그 비트맵/요청 풀을 가리키는
	 *   완전히 초기화된 상태. 실패 시 bset 자체가 kfree되므로 이 필드도 함께 사라진다.
	 * 동기화: 태그 할당/해제 자체는 blk-mq 내부 스핀락으로 보호되며, 이 필드의 내용을
	 *   이 파일에서 직접 락을 걸어 보호하지는 않는다(구조가 아니라 위치만 참조). */
	struct bsg_device	*bd;
	/* [한국어] bsg_register_queue()가 반환한, /dev/bsg/<name> 문자 디바이스 노드를
	 *   표현하는 불투명 포인터(block/bsg.c가 실제 정의를 소유).
	 * 설정자: bsg_setup_queue()가 bsg_register_queue(q, dev, name,
	 *   bsg_transport_sg_io_fn, NULL)의 반환값을 저장.
	 * 읽는 자: bsg_remove_queue()가 bsg_unregister_queue(bset->bd)로 문자 디바이스와
	 *   그 sysfs/devfs 노드를 제거할 때 사용.
	 * 값 범위: 등록 성공 시 유효한 포인터. 등록이 실패하면 이 필드가 채워지기 전에
	 *   bsg_setup_queue()가 이미 에러 경로로 빠져 bset 자체를 해제하므로, bset이
	 *   살아있는 동안에는 항상 유효한 값을 가진다.
	 * 동기화: 등록/해제는 각각 probe/remove 컨텍스트에서 한 번씩만 일어나므로 별도
	 *   락이 필요 없다. */
	bsg_job_fn		*job_fn;
	/* [한국어] 실제 하드웨어 제출을 담당하는 LLD 콜백 함수 포인터.
	 * 설정자: bsg_setup_queue() 인자로 LLD가 넘긴 함수를 그대로 저장.
	 * 읽는 자: bsg_queue_rq()가 매 요청마다 bset->job_fn(blk_mq_rq_to_pdu(req)) 형태로
	 *   호출해 이제 막 bsg_prepare_job()으로 채워진 bsg_job을 LLD에 넘긴다.
	 * 값 범위: LLD가 등록한 유효한 함수 포인터. NULL 여부를 이 파일이 검사하지 않으므로
	 *   호출자가 반드시 유효한 함수를 넘겨야 한다.
	 * 동기화: 등록 이후 불변이므로 필드 자체에는 락이 필요 없다. 다만 job_fn은
	 *   BLK_MQ_F_BLOCKING 하에서 여러 사용자 프로세스 컨텍스트에서 동시에(재진입)
	 *   호출될 수 있으므로 LLD가 자체적으로 내부 동시성을 보장해야 한다. */
	bsg_timeout_fn		*timeout_fn;
	/* [한국어] 요청 타임아웃 시 호출되는 LLD 콜백. NULL이면 타임아웃을 이 파일이
	 *   기본 처리(BLK_EH_DONE)한다.
	 * 설정자: bsg_setup_queue() 인자로 LLD가 넘긴 함수(NULL 허용).
	 * 읽는 자: bsg_timeout()이 NULL이 아니면 bset->timeout_fn(rq)를 호출해 위임한다.
	 * 값 범위: NULL(기본 처리) 또는 LLD가 제공한 유효한 콜백.
	 * 동기화: 등록 이후 불변. bsg_timeout() 자체는 blk-mq 타임아웃 워크큐 컨텍스트에서
	 *   실행되므로, LLD의 timeout_fn 구현도 그 컨텍스트 제약(블로킹 최소화)을 지켜야
	 *   한다. */
};

/*
 * [한국어]
 * bsg_transport_sg_io_fn - SG_IO v4 사용자공간 요청을 blk-mq request/bsg_job으로
 *                          변환해 동기 실행하고 결과를 사용자공간에 회신한다
 *
 * @q: 이 bsg 큐의 request_queue. bsg_setup_queue()가 만들었으며 hctx/tag_set이
 *     이미 구성되어 있다.
 * @hdr: 사용자공간이 ioctl(SG_IO)로 전달한 sg_io_v4 구조체(block/bsg.c가 미리
 *       사용자공간에서 복사해 넘긴다). 요청/응답 버퍼 포인터·길이, 프로토콜/
 *       서브프로토콜, 타임아웃, 플래그 등을 담는다.
 * @open_for_write: 파일 디스크립터가 쓰기 가능하게 열렸는지 여부. 이 함수 본문에서는
 *       사용하지 않으며, block/bsg.c가 기대하는 sg_io_fn 콜백 시그니처를 맞추기
 *       위해서만 존재한다.
 * @timeout: block/bsg.c가 hdr->timeout을 jiffies 등 커널 내부 단위로 변환해 넘긴 값.
 * @return: 0(정상 전달) 또는 음수 errno. hdr 자체에도 device_status/transport_status/
 *          response 등 SCSI 결과가 채워지므로, 반환값은 "명령이 전달·실행됐는지"를,
 *          hdr 필드는 "명령이 어떻게 끝났는지"를 나타낸다.
 *
 * bsg_setup_queue()가 bsg_register_queue()에 등록하는 sg_io_fn 콜백으로,
 * /dev/bsg/<name>에 대한 ioctl(SG_IO)가 들어올 때마다 block/bsg.c를 거쳐 호출된다.
 * BSG_PROTOCOL_SCSI + BSG_SUB_PROTOCOL_SCSI_TRANSPORT 조합만 지원하며(다른
 * 서브프로토콜은 block/bsg.c의 다른 경로가 처리), CAP_SYS_RAWIO가 없는 프로세스는
 * 거부한다. blk_mq_alloc_request()로 request를 얻고 bsg_job(request의 PDU 영역)을
 * 초기화한 뒤, 필요하면 BIDI(양방향 전송) 보조 request까지 만들어 blk_rq_map_user()로
 * 사용자 버퍼를 pin한다. blk_execute_rq()로 동기 제출해 완료까지 블록한 뒤,
 * job->result/job->reply를 sg_io_v4 필드로 되돌려 사용자공간에 반환한다.
 * 실행 컨텍스트: ioctl을 호출한 사용자 프로세스 컨텍스트. bsg 큐가
 * BLK_MQ_F_BLOCKING으로 만들어져 있어 blk_execute_rq() 안에서 완료까지 잠들 수
 * 있다(인터럽트 비활성 컨텍스트에서 호출하면 안 됨 - blk_execute_rq 내부에
 * WARN_ON(irqs_disabled()) 존재).
 * 에러 처리: 각 단계 실패마다 그때까지 획득한 자원만 정확히 되돌리는 labeled goto
 * 체인(out_free_rq/out_free_job_request/out_free_bidi_rq/out_unmap_bidi_rq)을
 * 사용한다. 아래 각 goto 지점에서 왜 그 레이블로 뛰는지 인라인 주석으로 설명한다.
 *
 * 호출 체인:
 *   사용자 ioctl(SG_IO) -> block/bsg.c -> [bsg_transport_sg_io_fn]
 *     -> blk_mq_alloc_request -> blk_rq_map_user -> blk_execute_rq
 *     -> (blk-mq 디스패치) bsg_queue_rq -> bsg_prepare_job -> LLD job_fn
 *     -> (완료) bsg_job_done -> blk_mq_complete_request -> bsg_complete
 *        -> blk_mq_end_request
 *     -> [bsg_transport_sg_io_fn]로 복귀 -> hdr 채움 -> 사용자공간
 */
static int bsg_transport_sg_io_fn(struct request_queue *q, struct sg_io_v4 *hdr,
		bool open_for_write, unsigned int timeout)
{
	struct bsg_job *job; /* [한국어] 이 요청의 PDU 영역에서 나중에 캐스팅해 채울 bsg_job 포인터 - 아직 미확정(request 할당 후 결정) */
	struct request *rq; /* [한국어] 새로 할당할 주(primary) blk-mq request 포인터 - dout/din 중 하나의 방향으로 사용됨 */
	struct bio *bio; /* [한국어] 주 request에 매핑된 사용자 버퍼의 bio - blk_execute_rq 이후 unmap 시 사용하려고 미리 저장해 둘 변수 */
	void *reply; /* [한국어] job->reply(sense/응답 버퍼) 포인터를 memset 전에 임시 보관하는 변수 */
	int ret; /* [한국어] 각 하위 단계의 반환값/에러 코드를 담는 공용 변수 */

	/* [한국어] 이 sg_io_fn은 "SCSI 트랜스포트" 서브프로토콜 전용이다. BSG는 여러
	 *   프로토콜/서브프로토콜을 지원할 수 있는 범용 골격이지만, 이 함수가 실제로
	 *   처리하는 건 SCSI 트랜스포트 계열 명령뿐이므로 그 외 조합은 즉시 거부한다. */
	if (hdr->protocol != BSG_PROTOCOL_SCSI  ||
	    hdr->subprotocol != BSG_SUB_PROTOCOL_SCSI_TRANSPORT) /* [한국어] 조건의 두 번째 절: SCSI 트랜스포트 서브프로토콜인지 검사 */
		return -EINVAL; /* [한국어] 지원하지 않는 protocol/subprotocol 조합 - 사용자공간에 -EINVAL 반환, request 할당 이전이라 되돌릴 자원 없음 */
	/* [한국어] CAP_SYS_RAWIO: 하드웨어에 직접 영향을 주는 raw I/O 권한 검사.
	 *   BSG 트랜스포트 명령은 SCSI 계층의 검증을 우회해 벤더 특화/관리 명령을 그대로
	 *   하드웨어로 전달하므로, 일반 사용자에게 허용하면 장치를 손상시키거나 다른
	 *   컨테이너/사용자의 I/O를 관찰·방해할 수 있다. */
	if (!capable(CAP_SYS_RAWIO))
		return -EPERM; /* [한국어] 권한 없음 - -EPERM 반환 */

	/* [한국어] 전송 방향 결정: dout_xfer_len(호스트->장치로 보낼 데이터 길이)이
	 *   0보다 크면 REQ_OP_DRV_OUT(쓰기 방향 passthrough), 아니면 REQ_OP_DRV_IN(읽기
	 *   방향 또는 무전송 명령-전용)으로 request를 할당한다. 이 시점에는 아직 실제
	 *   사용자 버퍼를 매핑하지 않고 blk-mq 태그(슬롯)만 확보한다. 마지막 인자 0은
	 *   blk_mq_alloc_request()의 flags(예: BLK_MQ_REQ_NOWAIT)로, 여기서는 필요한
	 *   플래그가 없어 0을 넘긴다. */
	rq = blk_mq_alloc_request(q, hdr->dout_xfer_len ?
			     REQ_OP_DRV_OUT : REQ_OP_DRV_IN, 0); /* [한국어] 태그 고갈/큐 미가동 등으로 실패하면 rq에 ERR_PTR이 담김 */
	if (IS_ERR(rq)) /* [한국어] request 할당 결과 확인 - 실패 시 ERR_PTR 인코딩된 포인터 */
		return PTR_ERR(rq); /* [한국어] request 할당 자체가 실패 - 아직 아무 자원도 잡지 않았으므로 즉시 반환 */
	rq->timeout = timeout; /* [한국어] 이 명령 전용 타임아웃 설정 - bsg_setup_queue()가 blk_queue_rq_timeout()으로 건 큐 기본값(BLK_DEFAULT_SG_TIMEOUT)을 이 요청에 한해 사용자가 지정한 값으로 덮어씀 */

	job = blk_mq_rq_to_pdu(rq); /* [한국어] request 구조체 바로 뒤에 위치한 PDU(Per-Data Unit) 영역을 struct bsg_job*으로 캐스팅 - bsg_setup_queue()가 tag_set->cmd_size를 sizeof(struct bsg_job)+dd_job_size로 설정해 이 레이아웃을 보장 */
	reply = job->reply; /* [한국어] bsg_init_rq()가 tag_set 슬롯 초기화 시 한 번만 kzalloc해 둔 reply(sense) 버퍼 포인터를 memset 이전에 백업 - 이 버퍼는 요청마다 재할당하지 않고 슬롯 수명 내내 재사용됨 */
	memset(job, 0, sizeof(*job)); /* [한국어] 직전 요청이 남긴 bsg_job 필드를 전부 0으로 초기화 - dd_data가 가리키는 job 뒤쪽 LLD 전용 영역은 sizeof(*job) 범위 밖이라 여기서 지워지지 않음(LLD 자체 관리) */
	job->reply = reply; /* [한국어] memset으로 지워진 reply 포인터를 위에서 백업한 값으로 복원 - bsg_init_rq에서 할당한 버퍼를 그대로 재사용 */
	job->reply_len = SCSI_SENSE_BUFFERSIZE; /* [한국어] reply 버퍼의 기본 용량 - bsg_init_rq()가 kzalloc한 크기(SCSI_SENSE_BUFFERSIZE)와 동일하게 초기값 설정, 이후 job->result<0 분기에서 sizeof(u32)로 축소될 수 있음 */
	job->dd_data = job + 1; /* [한국어] LLD 전용 스크래치 영역은 bsg_job 구조체 바로 뒤(job+1)에 위치 - tag_set->cmd_size = sizeof(struct bsg_job)+dd_job_size 로 이 공간이 이미 확보되어 있음 */

	job->request_len = hdr->request_len; /* [한국어] 사용자공간이 보낸 명령(CDB 등) 버퍼의 바이트 길이를 그대로 저장 */
	job->request = memdup_user(uptr64(hdr->request), hdr->request_len); /* [한국어] 사용자공간의 명령 버퍼를 커널 메모리로 복사(memdup_user는 kmalloc+copy_from_user 결합) - 이후 LLD의 job_fn이 job->request를 커맨드로 해석 */
	/* [한국어] memdup_user 실패(주로 -ENOMEM 또는 -EFAULT) - 지금까지 확보한 자원은
	 *   request(rq) 태그 하나뿐이므로 out_free_rq로 바로 뛰어 그것만 반환한다. */
	if (IS_ERR(job->request)) {
		ret = PTR_ERR(job->request); /* [한국어] memdup_user가 반환한 음수 errno(ERR_PTR로 인코딩)를 ret에 저장 */
		goto out_free_rq; /* [한국어] out_free_rq로 이동 - job->request가 유효 포인터가 아니라 ERR_PTR이므로 kfree 대상이 아님(중간 레이블들을 건너뜀) */
	}

	/* [한국어] dout_xfer_len과 din_xfer_len이 모두 0보다 크면 BIDI(양방향 전송,
	 *   예: SCSI XDWRITEREAD류 명령)이다. 주 request(rq)는 이미 REQ_OP_DRV_OUT으로
	 *   할당돼 있으므로 dout(쓰기) 데이터를 담당하고, din(읽기) 데이터는 별도의
	 *   보조 request(job->bidi_rq)를 만들어 그 bio에 담는다. */
	if (hdr->dout_xfer_len && hdr->din_xfer_len) {
		job->bidi_rq = blk_mq_alloc_request(rq->q, REQ_OP_DRV_IN, 0); /* [한국어] din 데이터 전용 보조 request 할당(REQ_OP_DRV_IN 고정) - blk-mq 디스패치 경로로는 절대 제출되지 않고, bio/페이지 컨테이너로만 쓰인다 */
		/* [한국어] 보조 request 할당 실패 - 지금까지 확보한 자원은 rq 태그와 방금
		 *   복사한 job->request 버퍼이므로 out_free_job_request로 뛰어 그 둘을 정리. */
		if (IS_ERR(job->bidi_rq)) {
			ret = PTR_ERR(job->bidi_rq); /* [한국어] blk_mq_alloc_request 실패의 음수 errno를 ret에 저장 */
			goto out_free_job_request; /* [한국어] out_free_job_request로 이동 - rq 태그와 job->request 복사본만 되돌리면 됨(bidi_rq는 아직 없음) */
		}

		/* [한국어] 사용자공간의 din_xferp 버퍼(길이 din_xfer_len)를 페이지 단위로
		 *   pin하여 job->bidi_rq의 bio 체인으로 매핑한다. map_data(3번째 인자)로 NULL을
		 *   넘겨 세그먼트별 커스텀 매핑 없이 기본 방식을 사용. */
		ret = blk_rq_map_user(rq->q, job->bidi_rq, NULL,
				uptr64(hdr->din_xferp), hdr->din_xfer_len, /* [한국어] din_xferp/din_xfer_len: 사용자 버퍼 주소와 길이(계속) */
				GFP_KERNEL); /* [한국어] GFP_KERNEL: 블로킹 허용 할당 플래그(계속) */
		if (ret) /* [한국어] bidi 매핑 결과 확인 */
			goto out_free_bidi_rq; /* [한국어] 매핑 실패 - bidi_rq는 할당됐지만 아직 아무것도 매핑되지 않았으므로 unmap 없이 바로 request만 반환 */

		job->bidi_bio = job->bidi_rq->bio; /* [한국어] 이후 완료 후 unmap(blk_rq_unmap_user)에 쓸 bio 포인터를 미리 저장 - bidi_rq 필드 자체를 나중에 참조하는 대신 bio를 별도 보관 */
	} else { /* [한국어] BIDI가 아닌 일반(단방향) 전송 - bidi 관련 필드를 명시적으로 NULL 처리 */
		job->bidi_rq = NULL; /* [한국어] 이후 코드 전반의 if (job->bidi_rq) 분기가 이 값으로 BIDI 여부를 판별 */
		job->bidi_bio = NULL; /* [한국어] unmap 단계에서 job->bidi_rq가 NULL이므로 이 필드는 실제로 참조되지 않지만 일관성을 위해 초기화 */
	}

	ret = 0; /* [한국어] dout/din 둘 다 0(순수 명령-전용, 데이터 전송 없음)인 경우 아래 두 매핑 분기 모두 건너뛰므로, 여기서 ret을 성공값으로 리셋해 둠 */
	if (hdr->dout_xfer_len) { /* [한국어] 쓰기 방향 데이터가 있는 경우 - rq는 이미 REQ_OP_DRV_OUT으로 할당돼 있음 */
		/* [한국어] 사용자공간의 dout_xferp 버퍼를 주 request(rq)의 bio 체인에 매핑 -
		 *   장치로 내려보낼 쓰기 데이터. */
		ret = blk_rq_map_user(rq->q, rq, NULL, uptr64(hdr->dout_xferp),
				hdr->dout_xfer_len, GFP_KERNEL); /* [한국어] dout_xfer_len/GFP_KERNEL 인자(계속) - 블로킹 허용 */
	} else if (hdr->din_xfer_len) { /* [한국어] 단방향 읽기 - rq는 REQ_OP_DRV_IN으로 할당돼 있었음(위쪽 blk_mq_alloc_request의 방향 결정 삼항 연산 결과) */
		/* [한국어] 사용자공간의 din_xferp 버퍼를 주 request(rq)의 bio 체인에 매핑 -
		 *   장치가 채워줄 읽기 데이터를 받을 자리. */
		ret = blk_rq_map_user(rq->q, rq, NULL, uptr64(hdr->din_xferp),
				hdr->din_xfer_len, GFP_KERNEL); /* [한국어] din_xfer_len/GFP_KERNEL 인자(계속) - 블로킹 허용 */
	}

	/* [한국어] 주 request의 dout/din 매핑 실패 - rq 자체의 bio는 아직 매핑되지 않았으므로
	 *   (아래에서 bio = rq->bio 를 실행하기 전) out_unmap_bidi_rq부터 시작해
	 *   bidi 쪽만 이미 매핑됐다면 그것부터 되돌린다. */
	if (ret)
		goto out_unmap_bidi_rq; /* [한국어] out_unmap_bidi_rq로 이동 - 주 request는 아직 매핑 전이므로 bidi 쪽부터 되돌림 */

	bio = rq->bio; /* [한국어] blk_execute_rq() 실행 전에 bio 포인터를 미리 저장 - 완료 후 blk_rq_unmap_user(bio)로 사용자 페이지 unpin 시 사용 */
	blk_execute_rq(rq, !(hdr->flags & BSG_FLAG_Q_AT_TAIL)); /* [한국어] 동기 제출: 완료까지 현재 프로세스를 재움. BSG_FLAG_Q_AT_TAIL 미설정 시 at_head=true(디스패치 리스트 맨 앞에 삽입, 우선 처리) - 반환값(blk_status_t)은 사용하지 않고 job->result(LLD가 bsg_job_done으로 채움)만 신뢰함 */

	/*
	 * The assignments below don't make much sense, but are kept for
	 * bug by bug backwards compatibility:
	 */
	/* [한국어] 위 원본 주석이 말하는 "bug by bug 호환"의 의미: 아래에서
	 *   hdr->driver_status를 항상 0으로 고정하는 것은 논리적으로는 이상하지만
	 *   (드라이버 계층 자체 에러 상태를 반영해야 자연스러움), 과거 sg 드라이버가
	 *   실제로 그렇게 동작해 왔기 때문에 사용자공간 애플리케이션들이 이 필드를
	 *   항상 0으로 기대하도록 굳어졌다. 지금 와서 "고쳐도" 기존 애플리케이션과의
	 *   ABI 호환이 깨지므로 일부러 버그를 그대로 유지한다. */
	hdr->device_status = job->result & 0xff; /* [한국어] job->result 하위 1바이트 = SCSI status byte(GOOD/CHECK_CONDITION 등, scsi/scsi.h의 status_byte 인코딩) */
	hdr->transport_status = host_byte(job->result); /* [한국어] host_byte() 매크로로 job->result의 중간 비트(호스트 어댑터 상태, SCSI DID_* 코드류)를 추출 */
	hdr->driver_status = 0; /* [한국어] 바로 위 원본 영어 주석대로 항상 0 고정 - bug-for-bug ABI 호환 */
	hdr->info = 0; /* [한국어] info 플래그를 우선 초기화 - 아래에서 필요 시 SG_INFO_CHECK 비트만 추가로 세움 */
	if (hdr->device_status || hdr->transport_status || hdr->driver_status) /* [한국어] 세 상태 필드 중 하나라도 이상이 있는지 검사(모두 0이면 건너뜀) */
		hdr->info |= SG_INFO_CHECK; /* [한국어] 세 상태 필드 중 하나라도 0이 아니면 SG_INFO_CHECK 비트를 세워 사용자공간에 "상태 필드를 확인하라"고 알림 */
	hdr->response_len = 0; /* [한국어] 실제 응답 복사 전 기본값 - 아래 copy_to_user 성공 블록에서만 갱신됨 */

	if (job->result < 0) { /* [한국어] job->result이 음수 = LLD가 SCSI 상태가 아니라 전송 자체의 실패(errno)를 보고한 경우 */
		/* we're only returning the result field in the reply */
		/* [한국어] job->result가 음수인 경우 job->reply에는 원래의 sense 데이터 대신
		 *   errno 자체(4바이트)만 의미가 있다고 간주하고, 아래에서 reply_len을
		 *   sizeof(u32)로 줄여 그만큼만 사용자공간에 복사되게 한다. */
		job->reply_len = sizeof(u32); /* [한국어] reply 버퍼 중 실제로 의미 있는 길이를 4바이트(u32 하나)로 축소 */
		ret = job->result; /* [한국어] 이 함수의 반환값 자체도 LLD가 보고한 음수 errno로 설정 - 사용자공간 ioctl 호출도 실패로 보고됨 */
	}

	if (job->reply_len && hdr->response) { /* [한국어] reply_len이 0이 아니고 사용자공간이 response 버퍼 포인터를 제공한 경우에만 복사 시도 */
		int len = min(hdr->max_response_len, job->reply_len); /* [한국어] 사용자가 제공한 버퍼 용량(max_response_len)과 실제 reply 길이 중 작은 쪽으로 복사량 클램프 - 사용자 버퍼 오버플로 방지 */

		/* [한국어] job->reply(sense/응답 버퍼)를 사용자공간 hdr->response로 복사.
		 *   실패 시 -EFAULT로 덮어써 위 job->result<0 분기에서 설정됐을 수도 있는 ret을
		 *   무조건 페이지 폴트 에러로 교체한다. */
		if (copy_to_user(uptr64(hdr->response), job->reply, len))
			ret = -EFAULT; /* [한국어] copy_to_user 실패 - 사용자 버퍼 접근 오류로 ret을 -EFAULT로 덮어씀 */
		else /* [한국어] copy_to_user 성공 */
			hdr->response_len = len; /* [한국어] 실제로 복사된 바이트 수를 사용자공간에 보고 */
	}

	/* we assume all request payload was transferred, residual == 0 */
	/* [한국어] 이 전송 경로는 부분 전송(잔여 바이트 계산)을 추적하지 않으므로
	 *   dout 방향은 항상 "전부 전송됨"으로 가정하고 잔여량을 0으로 고정 보고한다. */
	hdr->dout_resid = 0; /* [한국어] 쓰기 방향 잔여 바이트는 항상 0으로 보고 - 위 주석 참고 */

	if (job->bidi_rq) { /* [한국어] BIDI 요청이었던 경우에만 din(읽기) 방향 잔여량을 실제로 계산 */
		unsigned int rsp_len = job->reply_payload.payload_len; /* [한국어] bsg_prepare_job()->bsg_map_buffer()가 job->bidi_rq를 매핑하며 blk_rq_bytes()로 채워 둔 reply_payload 총 용량(요청한 din 버퍼 크기) */

		/* [한국어] LLD가 bsg_job_done()에 넘긴 reply_payload_rcv_len(실제 장치가 채운
		 *   바이트 수)이 버퍼 용량(rsp_len)보다 크면 이는 LLD 버그(버퍼 오버런 보고)이다.
		 *   WARN_ON으로 커널 로그에 경고를 남기고, 방어적으로 잔여량을 0으로 고정. */
		if (WARN_ON(job->reply_payload_rcv_len > rsp_len))
			hdr->din_resid = 0; /* [한국어] 버퍼 용량 초과 보고를 방어적으로 0 처리(WARN_ON 조건 성립 시) */
		else /* [한국어] 정상 케이스(WARN_ON 조건 불성립) */
			hdr->din_resid = rsp_len - job->reply_payload_rcv_len; /* [한국어] 정상 케이스: 버퍼 용량에서 실제로 받은 바이트 수를 뺀 나머지가 잔여(읽지 못한/장치가 채우지 않은) 바이트 수 */
	} else { /* [한국어] BIDI가 아닌 경우 - din_resid은 이 경로에서 별도로 추적하지 않음 */
		hdr->din_resid = 0; /* [한국어] 단방향 요청은 잔여량을 0으로 고정 보고(dout_resid과 동일한 단순화) */
	}

	blk_rq_unmap_user(bio); /* [한국어] 성공 경로 계속 진행: 위에서 bio = rq->bio로 저장해 둔 주 request의 bio를 unmap해 사용자 페이지 unpin(참조 해제) */
out_unmap_bidi_rq: /* [한국어] 레이블: 주 request 매핑 실패(if (ret) goto 문) 시 여기로 점프 - 성공 경로도 자연 낙하로 도달 */
	if (job->bidi_rq) /* [한국어] BIDI 요청이었는지 확인 - unmap 대상이 존재하는지 검사 */
		blk_rq_unmap_user(job->bidi_bio); /* [한국어] BIDI였다면 위에서 job->bidi_bio = job->bidi_rq->bio로 저장해 둔 bidi_bio도 unmap - BIDI가 아니면 job->bidi_rq가 NULL이라 건너뜀 */
out_free_bidi_rq: /* [한국어] 레이블: bidi 매핑 실패(if (ret) goto out_free_bidi_rq) 시 여기로 점프(unmap 불필요하므로 위 블록을 건너뜀) - 성공/다른 실패 경로는 자연 낙하로 도달 */
	if (job->bidi_rq) /* [한국어] BIDI 요청이었는지 확인 - free 대상이 존재하는지 검사 */
		blk_mq_free_request(job->bidi_rq); /* [한국어] BIDI 보조 request의 태그를 반환 - BIDI가 아니었다면 job->bidi_rq가 NULL이라 건너뜀 */
out_free_job_request: /* [한국어] 레이블: bidi_rq 할당 실패(goto out_free_job_request) 시 여기로 점프 - 성공/다른 실패 경로는 자연 낙하로 도달 */
	kfree(job->request); /* [한국어] 위에서 memdup_user로 복사해 둔 사용자 명령 버퍼 해제 */
out_free_rq: /* [한국어] 레이블: job->request 복사 실패(goto out_free_rq) 시 여기로 점프 - 모든 경로가 최종적으로 이 지점에 도달 */
	blk_mq_free_request(rq); /* [한국어] 주 request의 태그를 blk-mq에 반환 - 이 함수 안에서 획득한 마지막 자원 */
	return ret; /* [한국어] 누적된 결과 코드 반환 - 성공 시 0(또는 job->result<0였다면 그 음수 값), 실패 시 각 단계에서 설정된 음수 errno */
}

/**
 * bsg_teardown_job - routine to teardown a bsg job
 * @kref: kref inside bsg_job that is to be torn down
 */
/*
 * [한국어]
 * bsg_teardown_job - bsg_job의 참조 카운트(kref)가 0이 되었을 때 호출되는 최종 해제 루틴
 *
 * @kref: struct bsg_job 안에 내장된 kref. kref_put()이 참조 카운트를 감소시키다가
 *        0에 도달하면 이 함수를 콜백으로 호출한다.
 * @return: 없음(void). kref_put의 release 콜백 시그니처(void (*)(struct kref *))를
 *          따른다.
 *
 * job->kref는 bsg_prepare_job()에서 kref_init()으로 1로 초기화된 뒤, LLD가
 * bsg_job_get()으로 추가 참조를 잡았다가 나중에 bsg_job_put()으로 반납하는 식으로
 * 수명이 연장될 수 있다(예: 완료 통지 이후에도 LLD가 job 데이터를 잠시 더 참조해야
 * 하는 비동기 처리 단계가 있는 경우). 참조가 모두 반납되어 0이 되는 순간, 이 함수가
 * 실제 자원 회수를 수행한다: job->dev의 디바이스 참조 반납, request/reply
 * scatterlist 배열 해제, 그리고 마지막으로 blk_mq_end_request()를 호출해 밑바탕의
 * blk-mq request를 진짜로 완료시킨다. 이 blk_mq_end_request() 호출이 있어야
 * blk_execute_rq()에서 기다리던 대기자(ioctl 호출 프로세스)가 비로소 깨어난다 -
 * 즉 "request의 완료"는 bsg_job_done() 시점이 아니라 마지막 kref가 반납되는
 * 이 시점까지 지연될 수 있다.
 * 실행 컨텍스트: bsg_job_put()을 호출하는 쪽의 컨텍스트를 그대로 물려받는다
 * (일반적으로 bsg_complete()를 통한 blk-mq 완료 경로, 또는 LLD가 추가 참조를 늦게
 * 반납하는 임의의 컨텍스트).
 *
 * 호출 체인:
 *   bsg_job_put() -> kref_put(&job->kref, bsg_teardown_job) -> (참조=0) [이 함수]
 *   -> blk_mq_end_request() -> blk_execute_rq()의 대기자 wake
 */
static void bsg_teardown_job(struct kref *kref)
{
	struct bsg_job *job = container_of(kref, struct bsg_job, kref); /* [한국어] kref 포인터로부터 이를 내장한 bsg_job 전체를 역산(container_of 관용구) */
	struct request *rq = blk_mq_rq_from_pdu(job); /* [한국어] bsg_job(PDU)로부터 그 앞에 위치한 blk-mq request를 역산 - blk_mq_rq_to_pdu()의 역방향 연산 */

	put_device(job->dev);	/* release reference for the request */ /* [한국어] bsg_prepare_job()에서 get_device(job->dev)로 잡았던 참조를 여기서 반납 - job의 전체 수명(비동기 확장 포함) 동안 디바이스가 살아있음을 보장했던 참조 */

	kfree(job->request_payload.sg_list); /* [한국어] bsg_map_buffer()가 kmalloc한 request(dout) 방향 scatterlist 배열 해제 */
	kfree(job->reply_payload.sg_list); /* [한국어] bsg_map_buffer()가 kmalloc한 reply(din/BIDI) 방향 scatterlist 배열 해제 - BIDI가 아니었다면 sg_list는 NULL이라 kfree(NULL)은 안전하게 no-op */

	blk_mq_end_request(rq, BLK_STS_OK); /* [한국어] 밑바탕 blk-mq request를 최종 완료 처리 - blk_execute_rq()의 대기자를 깨우고 태그를 회수 가능 상태로 전환. 상태를 무조건 BLK_STS_OK로 넘기는 이유는 실제 SCSI/전송 결과는 이미 job->result에 담겨 hdr로 반환되었고, 이 blk_status_t는 blk-mq 계층 자체의 성패만 의미하기 때문 */
}

/*
 * [한국어]
 * bsg_job_put - bsg_job의 참조 카운트를 하나 반납한다
 *
 * @job: 참조를 반납할 bsg_job. bsg_job_get()으로 추가 참조를 잡았거나, blk-mq
 *       완료 경로(bsg_complete)가 최초 참조를 반납할 때 호출한다.
 * @return: 없음(void).
 *
 * kref_put()의 얇은 래퍼로, 참조 카운트가 0이 되면 bsg_teardown_job()이 자동으로
 * 호출되어 job과 연계된 blk-mq request를 최종 완료·해제한다. LLD가 완료 통지
 * (bsg_job_done) 이후에도 job 데이터를 더 참조해야 한다면 bsg_job_get()으로 미리
 * 참조를 늘려 두고, 다 쓴 뒤 이 함수로 반납해야 한다 - 그래야 실제 request 완료가
 * 그 시점까지 지연되어 use-after-free를 피할 수 있다.
 * 실행 컨텍스트: 호출자에 따라 임의의 컨텍스트(LLD 완료 인터럽트, 워커, blk-mq
 * 완료 softirq 등)에서 호출될 수 있다. kref_put은 원자적(atomic) 카운터 연산이므로
 * 별도 락 없이 어느 컨텍스트에서도 안전하다.
 *
 * 호출 체인:
 *   bsg_complete() -> [이 함수] (최초 참조 반납)
 *   LLD의 확장 완료 처리 -> [이 함수] (추가 참조 반납)
 *   -> (참조=0이면) kref_put 내부에서 bsg_teardown_job() 호출
 */
void bsg_job_put(struct bsg_job *job)
{
	kref_put(&job->kref, bsg_teardown_job); /* [한국어] 원자적으로 참조 카운트 감소, 0에 도달하면 bsg_teardown_job을 자동 호출 */
}
EXPORT_SYMBOL_GPL(bsg_job_put); /* [한국어] GPL 모듈(FC/SAS HBA 드라이버 등)에서 호출 가능하도록 심볼 공개 */

/*
 * [한국어]
 * bsg_job_get - bsg_job의 참조 카운트를 0이 아닐 때에만 원자적으로 증가시킨다
 *
 * @job: 참조를 추가로 얻으려는 bsg_job.
 * @return: 참조 획득에 성공하면 1(true), 이미 참조 카운트가 0(해제 진행 중)이면
 *          0(false).
 *
 * kref_get_unless_zero()의 얇은 래퍼. 일반 kref_get()과 달리 "이미 0으로 떨어져
 * bsg_teardown_job()이 진행 중이거나 진행 예정인 객체"에 대해서는 참조를 얻지
 * 않고 실패를 반환한다. 이 덕분에 LLD가 비동기 컨텍스트(다른 타이머/워커 등)에서
 * job 포인터를 들고 있다가 그 시점에 이미 완료·해제가 진행 중인지 안전하게 검사할
 * 수 있다 - TOCTOU(검사 시점과 사용 시점 사이의 경쟁) 없이 "참조를 얻었다면 최소한
 * 그 순간까지는 유효했다"는 보장을 얻는다.
 * 실행 컨텍스트: 임의의 컨텍스트에서 호출 가능(원자적 CAS 기반이라 락 불필요).
 *
 * 호출 체인:
 *   LLD의 비동기 콜백/타이머 등 -> [이 함수] -> (성공 시) job 계속 사용 후 bsg_job_put()
 */
int bsg_job_get(struct bsg_job *job)
{
	return kref_get_unless_zero(&job->kref); /* [한국어] 0이 아닐 때만 원자적으로 +1 하고 성공 여부(1/0)를 그대로 반환 */
}
EXPORT_SYMBOL_GPL(bsg_job_get); /* [한국어] GPL 모듈에서 호출 가능하도록 심볼 공개 */

/**
 * bsg_job_done - completion routine for bsg requests
 * @job: bsg_job that is complete
 * @result: job reply result
 * @reply_payload_rcv_len: length of payload recvd
 *
 * The LLD should call this when the bsg job has completed.
 */
/*
 * [한국어]
 * bsg_job_done - LLD가 하드웨어 완료(인터럽트 등)를 감지했을 때 호출하는 완료 통지 함수
 *
 * @job: 완료된 bsg_job. bsg_queue_rq()가 LLD의 job_fn에 넘겼던 바로 그 포인터.
 * @result: LLD가 보고하는 결과 코드. 0/양수는 SCSI status 인코딩(job->result에
 *          그대로 저장되어 이후 bsg_transport_sg_io_fn에서 device_status/
 *          transport_status로 분해됨), 음수는 전송 자체의 실패(errno)를 의미.
 * @reply_payload_rcv_len: 장치가 실제로 채워 보낸 응답(reply/din) 페이로드의
 *          바이트 수. BIDI 요청의 din_resid 계산에 사용됨.
 * @return: 없음(void).
 *
 * job->result/job->reply_payload_rcv_len에 결과를 기록한 뒤,
 * blk_mq_complete_request()를 호출해 blk-mq에게 "이 request는 이제 완료 처리
 * 단계로 넘어갈 준비가 됐다"고 알린다. blk_mq_complete_request()는 내부적으로
 * bsg_mq_ops.complete(즉 bsg_complete())를 적절한 컨텍스트(같은 CPU면 직접,
 * 아니면 IPI를 통해)에서 호출해 준다. blk_should_fake_timeout()은 blktrace/fault
 * injection 테스트에서 의도적으로 타임아웃을 재현하기 위한 훅으로, 테스트 모드가
 * 아니면 항상 false라 정상적으로 완료가 진행된다.
 * 실행 컨텍스트: LLD의 인터럽트 핸들러 또는 완료 워커 - 이 파일이 실행되는 여러
 * 컨텍스트 중 유일하게 "이 파일 밖(LLD)에서 호출을 시작하는" 진입점이다.
 *
 * 호출 체인:
 *   LLD 인터럽트 핸들러(하드웨어 완료 감지) -> [이 함수]
 *   -> blk_mq_complete_request -> (같은 CPU/IPI) bsg_complete -> bsg_job_put
 */
void bsg_job_done(struct bsg_job *job, int result,
		  unsigned int reply_payload_rcv_len)
{
	struct request *rq = blk_mq_rq_from_pdu(job); /* [한국어] job이 속한 blk-mq request를 역산 - blk_mq_complete_request()에 넘기기 위함 */

	job->result = result; /* [한국어] LLD가 보고한 결과 코드를 저장 - 이후 bsg_transport_sg_io_fn이 hdr->device_status 등으로 분해 */
	job->reply_payload_rcv_len = reply_payload_rcv_len; /* [한국어] 실제 수신된 응답 페이로드 길이 저장 - BIDI din_resid 계산에 사용 */
	/* [한국어] blk_should_fake_timeout(): fault-injection 테스트 훅으로, 테스트
	 *   설정이 활성화되어 있으면 일부러 완료 통지를 누락시켜 blk-mq 타임아웃 경로
	 *   (bsg_timeout)를 강제로 검증할 수 있게 한다. likely()로 표시된 것은 실제
	 *   운영 환경에서는 거의 항상 false이기 때문. */
	if (likely(!blk_should_fake_timeout(rq->q)))
		blk_mq_complete_request(rq); /* [한국어] 실제 완료 통지: blk-mq가 CPU 친화도에 따라 bsg_mq_ops.complete(bsg_complete)를 직접 또는 IPI로 호출 */
}
EXPORT_SYMBOL_GPL(bsg_job_done); /* [한국어] LLD(모듈)가 완료 인터럽트 핸들러에서 직접 호출해야 하므로 심볼 공개 */

/**
 * bsg_complete - softirq done routine for destroying the bsg requests
 * @rq: BSG request that holds the job to be destroyed
 */
/*
 * [한국어]
 * bsg_complete - bsg_mq_ops.complete 콜백: blk-mq 완료 경로에서 request당 한 번
 *                호출되어 job의 최초 참조를 반납한다
 *
 * @rq: 완료된 blk-mq request. bsg_job_done()이 blk_mq_complete_request(rq)로
 *      전달한 바로 그 request.
 * @return: 없음(void). bsg_mq_ops.complete의 시그니처(void (*)(struct request *))를
 *          따른다.
 *
 * request의 PDU에서 bsg_job을 복원한 뒤 bsg_job_put()을 호출해, bsg_prepare_job()
 * 이 kref_init()으로 세팅해 둔 최초 참조(=1)를 반납한다. 만약 LLD가 그 사이
 * bsg_job_get()으로 추가 참조를 잡아 두지 않았다면 이 반납으로 참조 카운트가
 * 0이 되어 즉시 bsg_teardown_job()이 실행되고, request가 최종 완료된다. LLD가
 * 추가 참조를 쥐고 있었다면 이 시점에는 아직 request가 완료되지 않고, LLD가
 * 나중에 자신의 참조를 마저 반납할 때까지 지연된다.
 * 실행 컨텍스트: blk-mq 완료 경로(bsg_job_done -> blk_mq_complete_request가
 * 선택한 컨텍스트 - 보통 softirq 또는 원래 CPU로의 IPI 콜백).
 *
 * 호출 체인:
 *   bsg_job_done -> blk_mq_complete_request -> [이 함수] -> bsg_job_put
 *   -> (참조=0이면) bsg_teardown_job
 */
static void bsg_complete(struct request *rq)
{
	struct bsg_job *job = blk_mq_rq_to_pdu(rq); /* [한국어] request의 PDU에서 bsg_job을 복원 */

	bsg_job_put(job); /* [한국어] bsg_prepare_job()이 잡아둔 최초 참조를 반납 - 다른 참조가 없다면 여기서 bsg_teardown_job까지 연쇄 실행됨 */
}

/*
 * [한국어]
 * bsg_map_buffer - request의 bio 체인을 scatterlist 배열로 변환해 LLD에 넘길
 *                  bsg_buffer를 채운다
 *
 * @buf: 채워질 대상. job->request_payload(dout 방향) 또는 job->reply_payload
 *       (din/BIDI 방향) 중 하나가 전달된다.
 * @req: 매핑된 bio를 가진 request. 주 request(req) 자신이거나 BIDI의 보조
 *       request(job->bidi_rq)일 수 있다.
 * @return: 0(성공) 또는 -ENOMEM(scatterlist 배열 할당 실패).
 *
 * blk_rq_map_user()가 이미 사용자 버퍼 페이지를 request의 bio 체인에 매핑해
 * 두었으므로, 이 함수는 그 bio 체인을 LLD가 이해하는 형태인 struct scatterlist
 * 배열로 한 번 더 변환하는 역할만 한다. nr_phys_segments(물리적으로 분리된
 * 세그먼트 수)만큼 scatterlist 엔트리를 할당하고, blk_rq_map_sg()로 실제 채운다.
 * BUG_ON은 세그먼트가 0인 request가 들어오는 것은 호출자(bsg_prepare_job)가
 * req->bio 존재 여부를 미리 검사했어야 하는 불변조건 위반이므로 방어적으로
 * 커널 패닉을 유발한다.
 * 실행 컨텍스트: bsg_prepare_job()을 통해 ioctl 호출 프로세스 컨텍스트(또는 LLD가
 * 직접 request를 만든 경우 그 컨텍스트)에서 실행. GFP_KERNEL을 쓰므로 블로킹 가능.
 *
 * 호출 체인:
 *   bsg_prepare_job() -> [이 함수] (request_payload 또는 reply_payload 채움)
 */
static int bsg_map_buffer(struct bsg_buffer *buf, struct request *req)
{
	size_t sz = (sizeof(struct scatterlist) * req->nr_phys_segments); /* [한국어] 필요한 scatterlist 배열의 총 바이트 크기 = 세그먼트당 크기 * 세그먼트 수 */

	BUG_ON(!req->nr_phys_segments); /* [한국어] 세그먼트 0인 request가 들어오면 호출자의 불변조건 위반 - 방어적 패닉(호출자가 req->bio 유무를 이미 검사했어야 함) */

	buf->sg_list = kmalloc(sz, GFP_KERNEL); /* [한국어] scatterlist 배열 동적 할당 - GFP_KERNEL이므로 이 컨텍스트는 블로킹 가능해야 함 */
	if (!buf->sg_list) /* [한국어] scatterlist 배열 kmalloc 결과 확인 */
		return -ENOMEM; /* [한국어] 할당 실패 - 호출자(bsg_prepare_job)가 이 -ENOMEM을 받아 실패 경로로 진입 */
	sg_init_table(buf->sg_list, req->nr_phys_segments); /* [한국어] scatterlist 배열을 사슬(체인) 형태로 초기화 - blk_rq_map_sg가 이어서 채울 수 있도록 준비 */
	buf->sg_cnt = blk_rq_map_sg(req, buf->sg_list); /* [한국어] request의 bio/bvec들을 순회하며 인접 페이지를 병합해 실제 scatterlist 엔트리를 채움 - 반환값은 실제 사용된 엔트리 수(nr_phys_segments 이하일 수 있음) */
	buf->payload_len = blk_rq_bytes(req); /* [한국어] request 전체의 총 바이트 길이 저장 - BIDI din_resid 계산 시 reply_payload.payload_len으로 재사용됨 */
	return 0; /* [한국어] 성공 */
}

/**
 * bsg_prepare_job - create the bsg_job structure for the bsg request
 * @dev: device that is being sent the bsg request
 * @req: BSG request that needs a job structure
 */
/*
 * [한국어]
 * bsg_prepare_job - blk-mq request를 LLD에 넘길 수 있는 완전한 bsg_job으로 준비한다
 *
 * @dev: 이 요청을 받을 디바이스. bsg_queue_rq()가 q->queuedata에서 꺼내 넘긴다.
 * @req: 방금 blk-mq가 디스패치한 request. bsg_transport_sg_io_fn() 등이
 *       blk_execute_rq()로 제출한 바로 그 request(또는 LLD가 직접 만든 request).
 * @return: true(준비 성공, job_fn 호출 가능) 또는 false(준비 실패 - 호출자는 바로
 *          에러로 완료 처리해야 함).
 *
 * request의 bio(들)를 bsg_map_buffer()로 scatterlist에 매핑해 job->request_payload
 * (dout)와 job->reply_payload(din/BIDI, job->bidi_rq가 있을 때만)를 채우고,
 * job->dev에 디바이스 참조를 하나 잡은 뒤 kref_init()으로 job의 참조 카운트를
 * 1로 시작시킨다. 이 kref는 이후 bsg_complete()/bsg_job_get()/bsg_job_put()이
 * 다루는 바로 그 참조 카운트다. 실패 시에는 이미 매핑된 request_payload만 골라서
 * 되돌리는 labeled goto를 사용하며, job->dev/kref는 성공 경로 맨 끝에서만
 * 설정되므로 실패 경로에서는 애초에 존재하지 않아 되돌릴 필요가 없다.
 * 실행 컨텍스트: bsg_queue_rq() 호출자의 컨텍스트를 그대로 물려받는다(blk-mq
 * 디스패치, BLK_MQ_F_BLOCKING 하의 프로세스 컨텍스트).
 *
 * 호출 체인:
 *   bsg_queue_rq() -> [이 함수] -> bsg_map_buffer() (최대 2회: request/reply)
 */
static bool bsg_prepare_job(struct device *dev, struct request *req)
{
	struct bsg_job *job = blk_mq_rq_to_pdu(req); /* [한국어] request의 PDU에서 아직 job_fn에 넘겨지지 않은 bsg_job을 꺼냄 */
	int ret; /* [한국어] bsg_map_buffer() 호출 결과 임시 저장용 */

	job->timeout = req->timeout; /* [한국어] request에 이미 설정된 타임아웃 값(bsg_transport_sg_io_fn의 rq->timeout=timeout 등)을 job에도 복사 - LLD가 job->timeout으로 조회 가능하게 함 */

	/* [한국어] req->bio가 있다는 것은 이 request에 실제로 매핑된(dout 방향) 데이터가
	 *   있다는 뜻 - 명령만 있고 데이터 전송이 없는 요청은 bio가 NULL일 수 있으므로
	 *   건너뛴다. 매핑 실패 시에는 아직 request_payload/reply_payload 어느 것도
	 *   확정되지 않았으므로 failjob_rls_job(sg_list kfree 없이 result만 설정)로 바로
	 *   뛴다. */
	if (req->bio) {
		ret = bsg_map_buffer(&job->request_payload, req); /* [한국어] request(dout) payload를 scatterlist로 매핑 시도 */
		if (ret) /* [한국어] 매핑 결과 확인 */
			goto failjob_rls_job; /* [한국어] failjob_rls_job으로 이동 - 아직 아무 것도 매핑되지 않았으므로 result만 설정 */
	}
	/* [한국어] BIDI 요청이었다면(job->bidi_rq != NULL, bsg_transport_sg_io_fn이 설정)
	 *   보조 request의 bio도 scatterlist로 매핑해 reply_payload를 채운다. 이 단계가
	 *   실패하면 request_payload는 이미 성공적으로 매핑된 상태이므로
	 *   failjob_rls_rqst_payload로 뛰어 그 sg_list부터 정리해야 한다. */
	if (job->bidi_rq) {
		ret = bsg_map_buffer(&job->reply_payload, job->bidi_rq); /* [한국어] reply(din/BIDI) payload를 scatterlist로 매핑 시도 */
		if (ret) /* [한국어] 매핑 결과 확인 */
			goto failjob_rls_rqst_payload; /* [한국어] failjob_rls_rqst_payload로 이동 - request_payload는 이미 매핑되어 있으므로 그것부터 해제 필요 */
	}
	job->dev = dev; /* [한국어] 성공 경로 확정 - 이 시점부터 job이 유효한 디바이스에 연결됨 */
	/* take a reference for the request */ /* [한국어] 이 참조는 bsg_teardown_job()의 put_device(job->dev)와 짝을 이루며, job의 전체 수명(LLD의 추가 kref 연장 포함) 동안 디바이스가 사라지지 않도록 보장 */
	get_device(job->dev); /* [한국어] 디바이스 참조 카운트 +1 */
	kref_init(&job->kref); /* [한국어] job 자체의 참조 카운트를 1로 초기화 - 이 참조는 bsg_complete()가 반납할 "최초 참조" */
	return true; /* [한국어] 준비 완료 - 호출자(bsg_queue_rq)가 이제 job_fn(job)을 호출해도 안전 */

failjob_rls_rqst_payload: /* [한국어] 레이블: reply_payload 매핑 실패(goto failjob_rls_rqst_payload) 시 진입 - request_payload는 이미 매핑되어 있어 먼저 그것부터 해제 */
	kfree(job->request_payload.sg_list); /* [한국어] 앞서 성공했던 request_payload의 scatterlist 배열 해제 */
failjob_rls_job: /* [한국어] 레이블: request_payload 매핑 실패(goto failjob_rls_job) 시 바로 진입, 또는 위에서 자연 낙하 - 아직 job->dev/kref는 설정 전이므로 되돌릴 것이 없음 */
	job->result = -ENOMEM; /* [한국어] 실패 원인을 job->result에 남김 - bsg_queue_rq()가 이 값을 직접 읽지는 않지만(자체 sts 변수 사용) LLD 관례상 결과 필드를 채워 둠 */
	return false; /* [한국어] 준비 실패 - 호출자(bsg_queue_rq)는 job_fn을 호출하지 않고 바로 에러로 완료 처리 */
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
 * [한국어]
 * bsg_queue_rq - bsg_mq_ops.queue_rq 콜백: blk-mq가 이 bsg 큐에 디스패치하는 모든
 *                request에 대해 호출되는 진입점
 *
 * @hctx: 이 request가 발행되는 하드웨어 컨텍스트. bsg 큐는 nr_hw_queues=1이므로
 *        항상 단 하나의 hctx만 존재한다.
 * @bd: 실제로 디스패치할 request(bd->rq)를 담은 blk-mq 큐 데이터.
 * @return: BLK_STS_OK(job_fn이 성공적으로 하드웨어에 제출) 또는 BLK_STS_IOERR
 *          (디바이스 참조 획득 실패/job 준비 실패/job_fn 자체 실패).
 *
 * 이 함수의 반환값은 "성공적으로 하드웨어에 제출했는지"만 의미하며, 실제 SCSI/
 * 전송 결과는 아니다 - job_fn이 0을 반환해도 실제 완료는 나중에 LLD가
 * bsg_job_done()을 호출할 때 결정된다(비동기 제출-완료 분리). 순서는:
 * blk_mq_start_request()로 타임아웃 타이머를 무장 -> get_device()로 짧은 수명의
 * 디바이스 참조 획득(핫플러그 경쟁 대비) -> bsg_prepare_job()으로 request를 job으로
 * 변환 -> LLD의 job_fn(job) 호출. 여기서 잡는 get_device(dev)/put_device(dev)
 * 참조는 job->dev의 get_device/put_device(bsg_prepare_job/bsg_teardown_job)와는
 * 별개의, 이 함수 호출 구간에만 한정된 참조다.
 * 실행 컨텍스트: blk-mq 디스패치 워커 또는 제출 태스크 - bsg 큐가
 * BLK_MQ_F_BLOCKING으로 만들어져 있으므로 job_fn 안에서 블로킹이 허용된다.
 *
 * 호출 체인:
 *   blk_execute_rq (blk_mq_insert_request 경유) -> blk-mq 디스패치 -> [이 함수]
 *   -> bsg_prepare_job -> LLD job_fn (실제 하드웨어 제출, 비동기)
 */
static blk_status_t bsg_queue_rq(struct blk_mq_hw_ctx *hctx,
				 const struct blk_mq_queue_data *bd)
{
	struct request_queue *q = hctx->queue; /* [한국어] 이 hctx가 속한 request_queue - bsg 큐는 hctx가 하나뿐이라 항상 동일한 q */
	struct device *dev = q->queuedata; /* [한국어] bsg_setup_queue()에서 blk_mq_alloc_queue(set, lim, dev)로 저장해 둔 디바이스 포인터를 복원 */
	struct request *req = bd->rq; /* [한국어] 실제로 디스패치할 request */
	struct bsg_set *bset = /* [한국어] q->tag_set으로부터 컨테이너 bset을 역산(계속) */
		container_of(q->tag_set, struct bsg_set, tag_set); /* [한국어] q->tag_set(=&bset->tag_set, blk_mq_alloc_queue가 설정)으로부터 컨테이너 bset 전체를 역산 */
	blk_status_t sts = BLK_STS_IOERR; /* [한국어] 기본값은 비관적으로 IOERR - 아래에서 job_fn이 성공(0)을 반환해야만 OK로 바뀜 */
	int ret; /* [한국어] job_fn의 반환값 저장용 */

	blk_mq_start_request(req); /* [한국어] request를 '시작됨' 상태로 전이 - 타임아웃 타이머 무장 등 blk-mq 내부 부기(book-keeping). 실패 가능성과 무관하게 항상 먼저 호출해야 함 */

	/* [한국어] 디바이스가 이미 참조 카운트 0으로 떨어져 제거 진행 중인 경우(핫플러그
	 *   경쟁) get_device가 실패(false)를 반환 - 이 경우 job_fn조차 호출하지 않고 바로
	 *   IOERR로 완료 처리한다(blk_mq_start_request는 이미 호출됐으므로 request 자체는
	 *   정상적으로 완료 처리됨). */
	if (!get_device(dev))
		return BLK_STS_IOERR; /* [한국어] 디바이스 참조 획득 실패 - job_fn 호출 없이 즉시 IOERR */

	if (!bsg_prepare_job(dev, req)) /* [한국어] job 준비 결과 확인 */
		goto out; /* [한국어] 준비 실패(주로 -ENOMEM) - job_fn을 호출하지 않고 out으로 뛰어 디바이스 참조만 반납하고 IOERR 반환 */

	ret = bset->job_fn(blk_mq_rq_to_pdu(req)); /* [한국어] LLD가 등록한 실제 처리 함수 호출 - 반환 0은 '하드웨어 제출 성공'을 의미할 뿐 완료를 의미하지 않음(완료는 추후 bsg_job_done) */
	if (!ret) /* [한국어] job_fn 반환값 확인 */
		sts = BLK_STS_OK; /* [한국어] job_fn이 성공(0)했을 때만 sts를 OK로 승격 */

out: /* [한국어] 레이블: prepare_job 실패 시 여기로 점프, 성공/job_fn 완료 후에도 자연 낙하로 도달 - 두 경로 모두 디바이스 참조 반납이 필요하기 때문 */
	put_device(dev); /* [한국어] 위에서 get_device(dev)로 잡은 이 함수 한정 참조 반납 - job->dev의 장기 참조와는 별개 */
	return sts; /* [한국어] blk-mq에게 이 request의 디스패치 결과를 보고 - BLK_STS_OK가 아니면 blk-mq가 즉시 해당 상태로 request를 완료 처리 */
}

/* called right after the request is allocated for the request_queue */
/*
 * [한국어]
 * bsg_init_rq - bsg_mq_ops.init_request 콜백: tag_set의 request 슬롯이 (재)할당될
 *               때 슬롯당 한 번만 호출되는 초기화 루틴
 *
 * @set: 이 슬롯이 속한 blk_mq_tag_set(&bset->tag_set).
 * @req: 지금 막 슬롯 메모리가 확보된 request. 아직 어떤 I/O에도 쓰이지 않은 상태.
 * @hctx_idx: 이 슬롯이 속한 하드웨어 큐 인덱스(bsg는 nr_hw_queues=1이라 항상 0).
 * @numa_node: 이 슬롯을 할당할 NUMA 노드 힌트.
 * @return: 0(성공) 또는 -ENOMEM(reply 버퍼 할당 실패 - 전체 tag_set 할당이 실패로
 *          처리됨).
 *
 * 이 콜백은 "매 I/O마다"가 아니라 "태그 슬롯이 만들어질 때"(즉 tag_set 크기만큼)
 * 딱 한 번만 실행된다. 여기서 job->reply(sense/응답 버퍼)를 미리 kzalloc해 두면,
 * 실제 I/O 경로(bsg_transport_sg_io_fn)에서는 매번 새로 할당하지 않고 이 버퍼를
 * memset으로 초기화만 하며 재사용할 수 있어 핫패스 할당 비용을 없앤다. 이 버퍼는
 * bsg_exit_rq()가 슬롯을 반납할 때 함께 해제된다.
 * 실행 컨텍스트: blk_mq_alloc_tag_set()/태그셋 크기 조정 경로 - probe 시점 등
 * 드물게만 실행되며 I/O 핫패스가 아니다.
 *
 * 호출 체인:
 *   bsg_setup_queue() -> blk_mq_alloc_tag_set() -> (슬롯마다) [이 함수]
 */
static int bsg_init_rq(struct blk_mq_tag_set *set, struct request *req,
		       unsigned int hctx_idx, unsigned int numa_node)
{
	struct bsg_job *job = blk_mq_rq_to_pdu(req); /* [한국어] 새로 확보된 슬롯의 PDU 영역을 bsg_job으로 캐스팅 */

	job->reply = kzalloc(SCSI_SENSE_BUFFERSIZE, GFP_KERNEL); /* [한국어] 이 슬롯 전용 reply(sense) 버퍼를 슬롯 수명 동안 단 한 번만 할당 - 이후 매 I/O에서는 이 포인터를 재사용(bsg_transport_sg_io_fn의 memset/restore 패턴 참고) */
	if (!job->reply) /* [한국어] reply 버퍼 kzalloc 결과 확인 */
		return -ENOMEM; /* [한국어] 할당 실패 - blk_mq_alloc_tag_set() 전체가 실패로 처리되어 bsg_setup_queue()의 out_tag_set 경로로 전파됨 */
	return 0; /* [한국어] 성공 */
}

/*
 * [한국어]
 * bsg_exit_rq - bsg_mq_ops.exit_request 콜백: bsg_init_rq()의 역순 정리, tag_set이
 *               해제되거나 슬롯 수가 줄어들 때 슬롯당 한 번 호출
 *
 * @set: 이 슬롯이 속했던 blk_mq_tag_set.
 * @req: 반납될 request 슬롯.
 * @hctx_idx: 이 슬롯이 속했던 하드웨어 큐 인덱스.
 * @return: 없음(void).
 *
 * bsg_init_rq()가 할당한 job->reply 버퍼를 해제한다. 이 콜백이 없다면 tag_set이
 * 해제될 때 슬롯마다 kzalloc해 둔 reply 버퍼가 그대로 새어나간다(메모리 누수).
 * 실행 컨텍스트: blk_mq_free_tag_set() 경로 - bsg_remove_queue()가 호출하는
 * blk_mq_free_tag_set(&bset->tag_set) 내부에서 슬롯마다 실행된다.
 *
 * 호출 체인:
 *   bsg_remove_queue() -> blk_mq_free_tag_set() -> (슬롯마다) [이 함수]
 */
static void bsg_exit_rq(struct blk_mq_tag_set *set, struct request *req,
		       unsigned int hctx_idx)
{
	struct bsg_job *job = blk_mq_rq_to_pdu(req); /* [한국어] 반납될 슬롯의 PDU 영역을 bsg_job으로 캐스팅 */

	kfree(job->reply); /* [한국어] bsg_init_rq()가 할당했던 reply 버퍼 해제 */
}

/*
 * [한국어]
 * bsg_remove_queue - bsg_setup_queue()로 만든 bsg 큐와 그 문자 디바이스를 모두
 *                    제거한다
 *
 * @q: bsg_setup_queue()가 반환했던 request_queue. NULL이 허용된다(아래 참고).
 * @return: 없음(void).
 *
 * LLD의 remove/probe-실패 경로에서 호출되어, bsg_setup_queue()가 만든 모든 자원을
 * 역순으로 해제한다: 문자 디바이스 노드 해제(bsg_unregister_queue) -> request_queue
 * 종료(blk_mq_destroy_queue - 이후 모든 신규 request는 -ENODEV) -> request_queue
 * 자체 참조 반납(blk_put_queue) -> tag_set 해제(blk_mq_free_tag_set - 슬롯마다
 * bsg_exit_rq 실행) -> 컨테이너(bset) 해제. q가 NULL이면 아무 것도 하지 않는데,
 * 이는 LLD의 probe 함수가 bsg_setup_queue() 실패로 q를 얻지 못한 경우에도 동일한
 * 정리 코드 경로(예: goto err 스타일)에서 bsg_remove_queue(q)를 조건 없이 호출할
 * 수 있게 해 주는 편의를 위함이다.
 * 실행 컨텍스트: LLD의 remove()/probe 실패 경로 - 일반적으로 프로세스 컨텍스트,
 * 드물게 실행되는 비-핫패스 함수.
 *
 * 호출 체인:
 *   LLD의 device remove() 또는 probe() 실패 처리 -> [이 함수]
 *   -> bsg_unregister_queue -> blk_mq_destroy_queue -> blk_put_queue
 *   -> blk_mq_free_tag_set(-> bsg_exit_rq 슬롯마다) -> kfree(bset)
 */
void bsg_remove_queue(struct request_queue *q)
{
	if (q) { /* [한국어] NULL 허용: bsg_setup_queue() 실패로 q를 못 받은 LLD도 조건 없이 이 함수를 호출할 수 있게 함 */
		struct bsg_set *bset = /* [한국어] q->tag_set으로부터 컨테이너 bset을 역산(선언 시작) */
			container_of(q->tag_set, struct bsg_set, tag_set); /* [한국어] q->tag_set으로부터 컨테이너 bset 전체를 역산 */

		bsg_unregister_queue(bset->bd); /* [한국어] /dev/bsg/<name> 문자 디바이스 노드와 관련 sysfs 엔트리 제거(block/bsg.c) */
		blk_mq_destroy_queue(q); /* [한국어] request_queue를 종료 상태로 전환 - 이후 모든 신규 요청은 -ENODEV로 실패, 진행 중인 요청은 드레인 */
		blk_put_queue(q); /* [한국어] blk_mq_alloc_queue()가 잡았던 request_queue 자체의 kobject 참조 반납(최종 해제 시점 결정) */
		blk_mq_free_tag_set(&bset->tag_set); /* [한국어] 태그 비트맵/요청 풀 해제 - 슬롯마다 bsg_exit_rq()가 호출되어 job->reply 버퍼들도 함께 해제됨 */
		kfree(bset); /* [한국어] 컨테이너 자체 해제 - tag_set이 이미 해제된 뒤이므로 마지막에 수행 */
	}
}
EXPORT_SYMBOL_GPL(bsg_remove_queue); /* [한국어] GPL 모듈(LLD)의 remove() 경로에서 호출 가능하도록 심볼 공개 */

/*
 * [한국어]
 * bsg_timeout - bsg_mq_ops.timeout 콜백: request의 타임아웃 타이머가 만료됐을 때
 *               blk-mq 타임아웃 워커가 호출
 *
 * @rq: 타임아웃이 발생한 request. rq->timeout(bsg_transport_sg_io_fn에서 설정한
 *      사용자 지정 값, 또는 bsg_setup_queue()의 BLK_DEFAULT_SG_TIMEOUT 기본값)
 *      만큼 완료되지 않은 상태.
 * @return: BLK_EH_DONE(추가 조치 없이 완료 처리 진행) 또는 LLD의 timeout_fn이
 *          결정하는 값(예: 어보트 진행 중이면 BLK_EH_RESET_TIMER로 타이머 연장).
 *
 * LLD가 timeout_fn을 등록하지 않았다면(NULL) 이 함수는 아무 하드웨어 조치도 하지
 * 않고 BLK_EH_DONE을 반환해 blk-mq의 기본 타임아웃 처리(request를 실패로 완료)에
 * 맡긴다. 등록했다면 LLD의 콜백에 위임하여 하드웨어 특화 어보트(예: NVMe라면 CID
 * 단위 abort, FC라면 별도 ABTS 유사 절차)를 수행할 기회를 준다.
 * 실행 컨텍스트: blk-mq 타임아웃 워크큐 - 일반 I/O 경로와 동시에 실행될 수 있으므로
 * LLD의 timeout_fn 구현은 진행 중인 job_fn/완료 경로와의 경쟁을 스스로 처리해야
 * 한다.
 *
 * 호출 체인:
 *   blk-mq 타임아웃 워커(rq->timeout 만료 감지) -> [이 함수] -> bset->timeout_fn(rq)
 */
static enum blk_eh_timer_return bsg_timeout(struct request *rq)
{
	struct bsg_set *bset = /* [한국어] rq->q->tag_set으로부터 컨테이너 bset을 역산(선언 시작) */
		container_of(rq->q->tag_set, struct bsg_set, tag_set); /* [한국어] request가 속한 큐의 tag_set으로부터 컨테이너 bset을 역산 */

	if (!bset->timeout_fn) /* [한국어] LLD의 timeout_fn 등록 여부 확인 */
		return BLK_EH_DONE; /* [한국어] LLD가 커스텀 타임아웃 처리를 등록하지 않음 - blk-mq 기본 처리(요청을 타임아웃 실패로 완료)에 맡김 */
	return bset->timeout_fn(rq); /* [한국어] LLD 콜백에 위임 - 반환값을 그대로 blk-mq에 전달(BLK_EH_DONE 또는 BLK_EH_RESET_TIMER 등) */
}

/* [한국어] bsg_mq_ops - 이 파일이 구현하는 모든 blk-mq 콜백을 하나로 묶는 vtable.
 *   bsg_setup_queue()가 set->ops = &bsg_mq_ops로 등록하며, 이후 이 bsg 큐에서
 *   벌어지는 모든 이벤트(디스패치/슬롯 초기화·해제/완료/타임아웃)는 반드시 이
 *   테이블에 등록된 함수를 거쳐간다 - 이 파일의 "진입점 총람"에 해당한다. */
static const struct blk_mq_ops bsg_mq_ops = {
	.queue_rq		= bsg_queue_rq, /* [한국어] blk-mq가 request를 디스패치할 때마다 호출 - request를 job으로 변환해 LLD job_fn에 전달 */
	.init_request		= bsg_init_rq, /* [한국어] tag_set 슬롯 생성 시 1회 호출 - reply 버퍼 사전 할당 */
	.exit_request		= bsg_exit_rq, /* [한국어] tag_set 슬롯 해제 시 1회 호출 - reply 버퍼 해제 */
	.complete		= bsg_complete, /* [한국어] blk_mq_complete_request() 호출 시 실행 - job의 최초 kref 참조 반납 */
	.timeout		= bsg_timeout, /* [한국어] 타임아웃 타이머 만료 시 실행 - LLD의 timeout_fn에 위임하거나 기본 처리 */
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
 * [한국어]
 * bsg_setup_queue - LLD probe에서 호출되어 bsg 큐(request_queue)와 그에 연결된
 *                   /dev/bsg/<name> 문자 디바이스를 함께 생성한다
 *
 * @dev: bsg 디바이스를 붙일 부모 디바이스(예: HBA의 struct device).
 * @name: /dev/bsg/ 아래 생성될 이름.
 * @lim: 이 큐의 queue_limits(최대 전송 크기, 세그먼트 수 등). NULL이면
 *       blk_mq_alloc_queue() 내부 기본값 사용.
 * @job_fn: 실제 명령 처리를 담당하는 LLD 콜백(bsg_queue_rq가 매 요청마다 호출).
 * @timeout: 타임아웃 시 호출될 LLD 콜백(NULL이면 기본 처리).
 * @dd_job_size: bsg_job 뒤에 이어붙일 LLD 전용 스크래치 영역의 바이트 크기
 *       (job->dd_data가 가리키는 공간).
 * @return: 성공 시 사용 준비가 끝난 request_queue, 실패 시 ERR_PTR(errno).
 *
 * bset(struct bsg_set) 컨테이너를 할당하고, 그 안에 내장된 blk_mq_tag_set을
 * bsg_mq_ops/queue_depth=128/BLK_MQ_F_BLOCKING 등으로 구성해
 * blk_mq_alloc_tag_set()으로 초기화한 뒤, blk_mq_alloc_queue()로 request_queue를
 * 만들고 마지막으로 bsg_register_queue()로 문자 디바이스를 등록한다. 각 단계는
 * 실패 시 그 이전 단계까지 획득한 자원만 정확히 되돌리는 labeled goto 체인
 * (out_cleanup_queue/out_queue/out_tag_set)을 공유한다(성공 경로는 각 레이블을
 * 건너뛰고 곧바로 return q).
 * 실행 컨텍스트: LLD의 probe() - 블로킹 가능한 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   LLD의 device probe() -> [이 함수]
 *     -> blk_mq_alloc_tag_set -> blk_mq_alloc_queue -> bsg_register_queue
 *   (실패 시) [이 함수] -> blk_mq_destroy_queue/blk_put_queue/blk_mq_free_tag_set/kfree
 */
struct request_queue *bsg_setup_queue(struct device *dev, const char *name,
		struct queue_limits *lim, bsg_job_fn *job_fn,
		bsg_timeout_fn *timeout, int dd_job_size)
{
	struct bsg_set *bset; /* [한국어] 이 큐 전용 컨테이너 - 아래에서 동적 할당 */
	struct blk_mq_tag_set *set; /* [한국어] &bset->tag_set을 가리킬 편의용 별칭 포인터 */
	struct request_queue *q; /* [한국어] 새로 생성될 request_queue */
	int ret = -ENOMEM; /* [한국어] 에러 코드 누적 변수 - 기본값 -ENOMEM은 이후 어떤 단계도 성공하지 못하고 out_tag_set으로 직행하는 경우(bset 할당 실패)를 위한 것 */

	bset = kzalloc_obj(*bset); /* [한국어] *bset 크기만큼 0으로 채워 동적 할당하는 타입-안전 kzalloc 매크로(kzalloc(sizeof(*bset), GFP_KERNEL)와 동등) */
	if (!bset) /* [한국어] bset 할당 결과 확인 */
		return ERR_PTR(-ENOMEM); /* [한국어] 할당 실패 - 아직 아무 자원도 잡지 않았으므로 즉시 반환(레이블 경유 불필요) */

	bset->job_fn = job_fn; /* [한국어] LLD의 실제 처리 콜백을 저장 - bsg_queue_rq()가 매 요청마다 호출 */
	bset->timeout_fn = timeout; /* [한국어] LLD의 타임아웃 콜백을 저장(NULL 허용) - bsg_timeout()이 위임 */

	set = &bset->tag_set; /* [한국어] 이후 코드를 간결하게 하기 위한 별칭 */
	set->ops = &bsg_mq_ops; /* [한국어] 이 파일이 구현하는 콜백 vtable 연결 */
	set->nr_hw_queues = 1; /* [한국어] bsg 큐는 항상 단일 하드웨어 큐 - 실제 다중 큐 하드웨어 제출은 LLD의 job_fn 내부에서 별도로 처리 */
	set->queue_depth = 128; /* [한국어] 동시에 진행 가능한 passthrough 명령 수 상한(고정값) - 일반 I/O 큐보다 훨씬 작은, 관리/패스스루 용도에 맞춘 값 */
	set->numa_node = NUMA_NO_NODE; /* [한국어] 관리성 명령 큐이므로 특정 NUMA 노드에 고정하지 않음 */
	set->cmd_size = sizeof(struct bsg_job) + dd_job_size; /* [한국어] 슬롯당 PDU 크기 = bsg_job 헤더 + LLD 전용 데이터 - job->dd_data(=job+1)가 이 레이아웃을 전제로 함 */
	set->flags = BLK_MQ_F_BLOCKING; /* [한국어] job_fn/bsg_transport_sg_io_fn이 블로킹 가능해야 하므로(하드웨어 메일박스 대기, 사용자 버퍼 pin 등) 블로킹 허용 플래그 설정 */
	/* [한국어] 위 설정대로 실제 태그 비트맵/요청 풀(슬롯마다 bsg_init_rq 호출 포함)을
	 *   할당. 실패하면 아직 bset만 할당된 상태이므로 out_tag_set(kfree(bset))으로
	 *   충분하다. */
	if (blk_mq_alloc_tag_set(set))
		goto out_tag_set; /* [한국어] 태그셋 할당 실패 - out_tag_set으로 이동해 bset만 해제 */

	q = blk_mq_alloc_queue(set, lim, dev); /* [한국어] request_queue 생성 - dev는 q->queuedata로 저장되어(blk-mq 코어) 이후 bsg_queue_rq()가 q->queuedata로 다시 꺼내 씀 */
	if (IS_ERR(q)) { /* [한국어] request_queue 생성 결과 확인 */
		ret = PTR_ERR(q); /* [한국어] PTR_ERR로 실제 에러 코드 추출 */
		goto out_queue; /* [한국어] 큐 생성 실패 - out_queue로 이동해 이미 만든 tag_set까지 함께 해제(bset은 그 다음 out_tag_set에서) */
	}

	blk_queue_rq_timeout(q, BLK_DEFAULT_SG_TIMEOUT); /* [한국어] 이 큐의 기본 요청 타임아웃 설정 - bsg_transport_sg_io_fn()이 개별 요청마다 rq->timeout을 다른 값으로 덮어쓰지 않는 한 이 기본값이 적용됨 */

	bset->bd = bsg_register_queue(q, dev, name, bsg_transport_sg_io_fn, NULL); /* [한국어] /dev/bsg/<name> 문자 디바이스 노드 등록, SG_IO ioctl 핸들러로 bsg_transport_sg_io_fn 지정 - 마지막 NULL은 이 경로에서 쓰지 않는 선택적 콜백(release 등으로 추정) */
	if (IS_ERR(bset->bd)) { /* [한국어] 문자 디바이스 등록 결과 확인 */
		ret = PTR_ERR(bset->bd); /* [한국어] PTR_ERR로 실제 에러 코드 추출 */
		goto out_cleanup_queue; /* [한국어] 등록 실패 - out_cleanup_queue로 이동해 이미 만든 request_queue까지 함께 해제 */
	}

	return q; /* [한국어] 전체 성공 - LLD에게 사용 준비가 끝난 request_queue 반환(문자 디바이스까지 이미 등록됨) */
out_cleanup_queue: /* [한국어] 레이블: bsg_register_queue 실패(goto out_cleanup_queue) 시 진입 - request_queue/tag_set/bset을 역순으로 모두 해제 */
	blk_mq_destroy_queue(q); /* [한국어] request_queue 종료 */
	blk_put_queue(q); /* [한국어] request_queue 자체의 참조 반납 */
out_queue: /* [한국어] 레이블: blk_mq_alloc_queue 실패(goto out_queue) 시 바로 진입, 또는 위에서 자연 낙하 */
	blk_mq_free_tag_set(set); /* [한국어] 태그셋 해제(슬롯마다 bsg_exit_rq 호출) */
out_tag_set: /* [한국어] 레이블: blk_mq_alloc_tag_set 실패(goto out_tag_set) 시 바로 진입, 또는 위에서 자연 낙하 */
	kfree(bset); /* [한국어] 컨테이너 자체 해제 - 모든 실패 경로가 최종적으로 도달하는 지점 */
	return ERR_PTR(ret); /* [한국어] 누적된 에러 코드를 ERR_PTR로 감싸 반환 */
}
EXPORT_SYMBOL_GPL(bsg_setup_queue); /* [한국어] GPL 모듈(LLD)의 probe() 경로에서 호출 가능하도록 심볼 공개 */
