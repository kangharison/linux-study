// SPDX-License-Identifier: GPL-2.0
/*
 * Block driver for s390 storage class memory.
 *
 * Copyright IBM Corp. 2012
 * Author(s): Sebastian Ott <sebott@linux.vnet.ibm.com>
 */

/*
 * [한국어 설명] s390 SCM(Storage Class Memory) 블록 드라이버의 blk-mq 구현부
 * (scm_blk.c)
 *
 * === 파일의 역할 ===
 * 이 드라이버의 알맹이다. 같은 디렉터리의 scm_drv.c 가 EADM 버스와의 결합만
 * 담당하는 데 비해, 이 파일은 블록 계층에서 내려온 요청을 하드웨어가 읽는
 * 서술자로 번역해 넘기고, 완료 인터럽트를 받아 다시 블록 계층에 돌려주는 일
 * 전부를 한다. 모듈의 적재·해제 진입점도 여기 있다.
 *
 * 하는 일은 크게 세 겹의 번역으로 볼 수 있다.
 *   1) blk-mq 요청(struct request) 여러 개를 struct scm_request 하나에 모은다.
 *   2) 그 scm_request 안의 AOB(Aob, ASYNC Operation Block) 하나를 채운다 —
 *      요청 하나가 MSB(Move Specification Block) 항목 하나가 된다.
 *   3) 각 요청의 데이터 조각(페이지)들을 AIDAW 목록에 한 칸씩 적는다.
 * 다 채운 AOB 를 eadm_start_aob() 로 하드웨어에 넘기면, 완료는 scm_blk_irq()
 * 로 되돌아온다.
 *
 * 또 하나의 축은 자원 관리다. 모듈이 올라올 때 struct scm_request 를
 * nr_requests 개 미리 만들어 아래 inactive_requests 목록에 쌓아 두고, I/O 마다
 * 하나씩 꺼내 쓰고 완료 때 돌려놓는다. 요청 경로에서는 잠들 수 없으므로 미리
 * 만들어 두는 것이다. AIDAW 도 마찬가지로, AOB 페이지의 남는 자리를 먼저
 * 쓰고 모자랄 때만 mempool 에서 페이지를 꺼낸다.
 *
 * 이 트리에서 확인할 수 없는 것이 많다. struct aob / aidaw / msb /
 * aob_rq_header 의 실제 배치, MSB_ 계열과 ARQB_ 계열과 EQC_ 계열 상수,
 * eadm_start_aob(), debug 기능 함수들은 모두 asm/eadm.h 와 asm/debug.h
 * 소관인데 이 트리는 sparse checkout 이라 arch/s390 이 없다. 반대로 blk-mq
 * 쪽은 include/linux/blk-mq.h 와 block/ 이 있어 규약을 그대로 확인할 수 있고,
 * 아래 주석에서 blk-mq 관련 설명은 모두 그 두 곳에서 확인한 것이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 모듈이 올라올 때(프로세스 컨텍스트):
 *   module_init -> scm_blk_init()
 *     -> scm_blk_params_valid() 로 모듈 매개변수 확인
 *     -> register_blkdev() 로 주번호를 얻는다
 *     -> scm_alloc_rqs() 로 요청 풀과 AIDAW mempool 을 만든다
 *     -> debug_register() 로 기록 핸들을 만든다
 *     -> scm_drv_init() [scm_drv.c] 로 EADM 버스에 등록 — 이 순간부터 probe 가
 *        들어올 수 있으므로 준비가 모두 끝난 뒤 마지막에 부른다
 *
 * 장치가 붙을 때(프로세스 컨텍스트):
 *   scm_probe() [scm_drv.c] -> scm_blk_dev_setup() [이 파일]
 *     -> 장치 번호를 배분하고 blk-mq 태그 집합을 채운다
 *     -> blk_mq_alloc_tag_set() -> blk_mq_alloc_disk() -> device_add_disk()
 *
 * I/O 가 내려올 때(프로세스 컨텍스트, 인터럽트를 켠 상태):
 *   블록 계층 -> blk-mq 의 요청 배분 경로 -> scm_mq_ops.queue_rq ==
 *   scm_blk_request()
 *     -> scm_permit_request() 로 쓰기 금지 상태를 거른다
 *     -> sq->scmrq 가 없으면 scm_request_fetch() 로 놀고 있는 요청을 꺼내
 *        scm_request_init() 로 초기화한다
 *     -> scm_request_set() 으로 요청 배열에 꽂고 scm_request_prepare() 로
 *        MSB 와 AIDAW 를 채운다
 *     -> 배치가 끝났거나(qd->last) 꽉 찼으면 scm_request_start() 로 넘긴다
 *
 * 완료가 올라올 때(인터럽트 문맥):
 *   EADM 하위 채널 -> scm_driver.handler == scm_blk_irq() [이 파일]
 *     -> 오류면 __scmrq_log_error() 로 기록하고, 남은 재시도가 있으면
 *        scm_blk_handle_error() 가 같은 AOB 를 다시 넣거나 요청을 되돌린다
 *     -> 아니면 scm_request_finish() 가 묶인 요청마다 결과를 적고
 *        blk_mq_complete_request() 로 완료를 알린다
 *     -> 그 뒤 blk-mq 가 softirq 에서 scm_mq_ops.complete ==
 *        scm_blk_request_done() 을 불러 blk_mq_end_request() 로 끝낸다
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 블록 계층. blk_mq_ops 표(scm_mq_ops)와 block_device_operations
 *   표(scm_blk_devops)로 이어진다. 요청은 queue_rq 로 들어오고, 완료는
 *   blk_mq_complete_request() 로 나간다.
 * 아래쪽: EADM 하위 채널. eadm_start_aob() 한 함수로만 나가며, 그 인터페이스는
 *   asm/eadm.h 소관이라 이 트리에서 확인 못 함.
 * 옆쪽: scm_drv.c. 이 파일이 구현한 네 함수(scm_blk_dev_setup,
 *   scm_blk_dev_cleanup, scm_blk_set_available, scm_blk_irq)를 그쪽이 부르고,
 *   이 파일은 그쪽의 scm_drv_init()/scm_drv_cleanup() 을 부른다. 즉 의존이
 *   양방향이며 그 접점의 선언은 모두 scm_blk.h 에 있다.
 *
 * 데이터 흐름:
 *   bio -> blk-mq 요청 -> scm_request 의 request 배열
 *     -> AOB 의 MSB 목록(요청 하나당 하나) + AIDAW 목록(데이터 페이지 하나당
 *        하나) -> eadm_start_aob() -> 하드웨어
 *   완료는 그 반대로, AOB 의 응답 블록 -> blk_status_t -> 요청마다 딸린
 *   사적 공간(blk_mq_rq_to_pdu) -> blk_mq_end_request()
 *
 * 공유 상태: 이 파일의 전역은 아래 여덟 개다. 그중 실제로 경쟁이 있는 것은
 *   놀고 있는 요청 목록(inactive_requests)과 그것을 지키는 list_lock,
 *   AIDAW mempool(aidaw_pool), 장치 번호 배분용 nr_devices 뿐이다. 장치별
 *   상태는 struct scm_blk_dev 안에 있고 그것은 scm_blk.h 가 정의한다.
 *
 * === 주요 함수/구조체 요약 ===
 * scm_blk_request()     : blk-mq 의 queue_rq 콜백. 이 파일의 정문이다.
 * scm_request_prepare() : 요청 하나를 MSB 와 AIDAW 로 번역한다.
 * scm_blk_irq()         : 완료 인터럽트 진입점. 재시도 여부를 여기서 가른다.
 * scm_request_finish()  : 묶인 blk-mq 요청들을 한꺼번에 완료 처리한다.
 * scm_blk_dev_setup()   : blk-mq 태그 집합과 gendisk 를 만들어 등록한다.
 * struct scm_queue      : 하드웨어 큐 하나가 조립 중인 scm_request 를 담는 자리.
 * scm_mq_ops            : blk-mq 콜백 표. 이 파일이 블록 계층에 보이는 얼굴이다.
 *
 * === 잠금의 지도 ===
 * 이 파일에는 서로 다른 잠금이 셋 나온다. 지키는 것과 문맥이 각각 다르다.
 *   list_lock (이 파일의 전역): 놀고 있는 요청 목록을 지킨다. 요청을 꺼내는
 *     쪽은 프로세스 컨텍스트라 spin_lock_irq 로 잡고, 돌려놓는 쪽은 완료
 *     인터럽트에서도 불리므로 spin_lock_irqsave 로 잡는다.
 *   sq->lock (하드웨어 큐마다 하나): 조립 중인 scm_request 를 지킨다. 완료
 *     경로가 건드리지 않으므로 인터럽트를 끄지 않는 그냥 spin_lock 이다.
 *   bdev->lock (장치마다 하나, scm_blk.h 가 정의): 쓰기 금지 상태를 지킨다.
 *     내리는 쪽이 인터럽트 문맥이라 irqsave 판을 쓴다.
 */

/* [한국어] 커널 로그 접두사를 정한다. 이 파일의 모든 pr_ 계열 출력 앞에
 * "scm_block: " 이 붙어 다른 s390 드라이버의 로그와 구별된다.
 * scm_drv.c 도 같은 정의를 따로 두고 있다. */
#define pr_fmt(fmt) "scm_block: " fmt

/* [한국어] [상류 코드 관찰] 이 파일에서 irqreturn_t 나 request_irq 를 직접 쓰는 곳은
 * 없다. 완료 경로가 인터럽트 문맥에서 도는 드라이버라 관례로 포함한 것으로
 * 보인다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
#include <linux/interrupt.h>
/* [한국어] spinlock_t 와 DEFINE_SPINLOCK() — 아래 목록 잠금과 큐별 잠금이 이 타입이다. */
#include <linux/spinlock.h>
/* [한국어] mempool_create_page_pool() 등 — AIDAW 용 페이지 mempool 을 만들고 쓴다. */
#include <linux/mempool.h>
/* [한국어] module_param(), MODULE_ 계열 선언, __init/__exit 표시. */
#include <linux/module.h>
/* [한국어] struct gendisk, queue_limits, register_blkdev() 등 블록 장치 등록에 필요한 것들. */
#include <linux/blkdev.h>
/* [한국어] blk-mq(다중 큐) 인터페이스. struct blk_mq_ops, blk_mq_queue_data,
 * blk_mq_start_request() 등 이 파일이 블록 계층과 주고받는 것 대부분이 여기 있다. */
#include <linux/blk-mq.h>
/* [한국어] kzalloc()/kfree() 계열 — 요청 머리와 요청 포인터 배열을 잡고 푼다. */
#include <linux/slab.h>
/* [한국어] list_for_each_safe(), list_add(), list_del() 등 — 놀고 있는 요청 목록을 다룬다. */
#include <linux/list.h>
/* [한국어] dma64_to_virt()/virt_to_dma64() — 커널 가상 주소와 하드웨어가 읽는
 * 64비트 주소를 오간다. AIDAW 와 MSB 의 주소 자리를 채울 때 쓴다. */
#include <linux/io.h>
/* [한국어] EADM 인터페이스. struct aob, aidaw, msb, aob_rq_header 와
 * eadm_start_aob(), 그리고 이 파일이 쓰는 하드웨어 상수 전부가 여기 있다.
 * arch/s390 소관이라 이 트리에서 확인 못 함. */
#include <asm/eadm.h>
/* [한국어] 같은 디렉터리의 공용 헤더. struct scm_blk_dev 와 struct scm_request,
 * 로그 매크로, 그리고 scm_drv.c 쪽 두 함수의 선언이 여기 있다. */
#include "scm_blk.h"

/* [한국어] s390 debug 기능 핸들. 이 파일이 만들고, scm_blk.h 가 extern 으로 내보내
 * 로그 매크로 셋이 모두 이것을 쓴다. static 이 아닌 유일한 전역이다. */
debug_info_t *scm_debug;
/* [한국어] 이 드라이버가 얻은 블록 장치 주번호. 적재 때 커널이 골라 준 값이며,
 * 장치마다 gendisk 의 major 로 들어간다. */
static int scm_major;
/* [한국어] AIDAW 용 페이지 mempool. AOB 페이지의 남는 자리로 모자랄 때만 쓰는
 * 예비 창구이며, 미리 잡아 둔 예비분이 있어 메모리가 말라도 최소한의
 * 진행이 보장된다. */
static mempool_t *aidaw_pool;
/* [한국어] 위 목록을 지키는 스핀락. 요청을 만들고 꺼내고 풀 때는 프로세스
 * 컨텍스트라 spin_lock_irq 로 잡고, 돌려놓을 때는 완료 인터럽트에서도
 * 불리므로 spin_lock_irqsave 로 잡는다. 어느 쪽이든 인터럽트를 끄는 것이
 * 핵심인데, 잠금을 잡은 채로 완료 인터럽트가 끼어들면 교착이 되기 때문이다. */
static DEFINE_SPINLOCK(list_lock);
/* [한국어] 놀고 있는 struct scm_request 들의 목록. 모듈 적재 때 채우고, I/O 마다
 * 꺼내 쓰고 돌려놓는다. 요청 경로에서 잠들 수 없으므로 미리 만들어 두는
 * 구조이며, 이 목록이 비면 블록 계층에 자원 부족을 알린다.
 * 장치별이 아니라 모듈 전역이라, 장치가 여럿이면 같은 풀을 나눠 쓴다. */
static LIST_HEAD(inactive_requests);
/* [한국어] 만들어 둘 struct scm_request 개수이자 장치마다 세울 하드웨어 큐 수.
 * 큐 하나가 언제나 최대 하나의 AOB 만 조립하므로 두 값이 같게 맞춰져 있다. */
static unsigned int nr_requests = 64;
/* [한국어] AOB 하나에 묶을 요청 수. 요청 포인터 배열의 칸 수이자, AIDAW 를 놓을
 * 자리를 정하는 값이다. 8 이면 MSB 여덟 칸을 쓰고 그 뒤가 AIDAW 몫이 된다. */
static unsigned int nr_requests_per_io = 8;
/* [한국어] 지금까지 배분한 장치 수. 장치 이름과 부번호를 여기서 뽑는다.
 * 원자 변수라 장치가 동시에 여럿 붙어도 같은 번호가 두 번 나오지 않으며,
 * 그래서 별도 잠금이 필요 없다. */
static atomic_t nr_devices = ATOMIC_INIT(0);
/* [한국어] 요청 수를 모듈 매개변수로 노출한다. 권한이 읽기 전용이라 sysfs 로 볼 수는
 * 있어도 적재 뒤에 바꿀 수는 없다 — 적재 때 이 값으로 자원을 이미 잡았기
 * 때문에 나중에 바꾸면 앞뒤가 맞지 않는다. */
module_param(nr_requests, uint, S_IRUGO);
/* [한국어] 위 매개변수의 설명. modinfo 로 보인다. */
MODULE_PARM_DESC(nr_requests, "Number of parallel requests.");

/* [한국어] AOB 하나에 묶을 요청 수를 모듈 매개변수로 노출한다. 이 값이 요청 포인터
 * 배열의 칸 수이자 AIDAW 커서의 시작 위치를 정한다. */
module_param(nr_requests_per_io, uint, S_IRUGO);
/* [한국어] 위 매개변수의 설명. modinfo 로 보인다. */
MODULE_PARM_DESC(nr_requests_per_io, "Number of requests per IO.");

/* [한국어] modinfo 에 보일 모듈 설명. */
MODULE_DESCRIPTION("Block driver for s390 storage class memory.");
/* [한국어] 라이선스. 이 선언이 있어야 GPL 전용 커널 심볼을 쓸 수 있고, 커널이
 * 오염 표시를 붙이지 않는다. */
MODULE_LICENSE("GPL");
/* [한국어] 이 모듈이 다룰 장치의 별칭. 사용자 공간의 모듈 적재 도구가 SCM 장치를
 * 보고 이 모듈을 자동으로 올릴 때 쓴다. 별표는 뒤에 무엇이 오든 맞는다는 뜻이다. */
MODULE_ALIAS("scm:scmdev*");

/* [한국어]
 * __scm_free_rq - 요청 하나가 붙들고 있던 메모리 세 덩어리를 푼다
 *
 * @scmrq: 풀 struct scm_request. 놀고 있는 목록에서 이미 떼어 낸 것이거나,
 *         아직 목록에 붙이기 전의 반쯤 만들어진 것이다.
 *
 * struct scm_request 하나는 세 덩어리로 이루어져 있다 — aob_rq_header 와 그
 * 뒤에 얹힌 scm_request 를 함께 담은 덩어리, AOB 용 페이지 하나, 그리고
 * blk-mq 요청 포인터 배열 하나다. 이 함수는 __scm_alloc_rq() 가 만든 순서의
 * 역순으로 그 셋을 되돌린다.
 *
 * 첫 줄이 이 함수의 요점이다. 할당은 aob_rq_header 를 잡고 그 뒤 data 자리에
 * scm_request 를 얹는 형태였으므로, 풀 때는 to_aobrq() 로 머리를 되찾아야
 * kfree() 에 넘길 수 있는 진짜 할당 시작 주소가 나온다. scmrq 를 그대로
 * kfree() 에 넘기면 할당 중간을 가리키게 된다.
 *
 * 반쯤 만들어진 요청에도 그대로 불린다 — __scm_alloc_rq() 의 free 라벨이
 * 그 경로다. 그때 aob 가 0 이거나 request 가 NULL 일 수 있는데, free_page()
 * 와 kfree() 가 그런 값을 견디는지는 mm 쪽 규약이고 이 트리에는 mm/ 이 없어
 * 확인 못 함.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 모듈 적재 실패 경로와 모듈 해제 경로에서만
 * 불린다. scm_free_rqs() 는 목록 잠금을 잡은 채로 이 함수를 부르므로, 여기서
 * 잠들면 안 된다.
 *
 * 에러 경로: 없다. 반환형이 void 다.
 *
 * 호출 체인:
 *   scm_free_rqs() / __scm_alloc_rq() → [이 함수]
 *     → to_aobrq() → free_page() → kfree()
 */
static void __scm_free_rq(struct scm_request *scmrq)
{
	/* [한국어] 요청 머리를 되찾는다. 할당은 머리를 잡고 그 뒤에 요청을 얹는 형태였으므로,
	 * 푸는 쪽은 이렇게 거슬러 올라가야 kfree() 에 넘길 진짜 시작 주소가 나온다.
	 * scmrq 를 그대로 넘기면 할당 중간을 가리키게 된다. */
	struct aob_rq_header *aobrq = to_aobrq(scmrq);

	/* [한국어] AOB 페이지를 푼다. 아직 얻지 못한 경우 0 이 넘어간다. */
	free_page((unsigned long) scmrq->aob);
	/* [한국어] 요청 포인터 배열을 푼다. 아직 만들지 못한 경우 NULL 이 넘어간다. */
	kfree(scmrq->request);
	/* [한국어] 마지막으로 요청 머리를 푼다. 이것이 할당의 진짜 시작 주소다. */
	kfree(aobrq);
}

/* [한국어]
 * scm_free_rqs - 놀고 있는 요청을 모두 풀고 AIDAW mempool 도 없앤다
 *
 * scm_alloc_rqs() 가 만들어 둔 것을 통째로 되돌린다. 모듈이 내려갈 때와,
 * 모듈 적재가 도중에 실패했을 때 불린다.
 *
 * 목록을 훑으며 하나씩 떼어 내고 __scm_free_rq() 로 푸는데, 훑는 도중에
 * 원소를 지우므로 list_for_each_safe() 를 쓴다 — 다음 원소 포인터를 미리
 * 잡아 두는 판이라, 지금 원소를 풀어도 루프가 이어진다. 보통의
 * list_for_each() 였다면 방금 푼 메모리에서 다음 포인터를 읽게 된다.
 *
 * 잠금을 잡은 채로 메모리를 푸는 것이 눈에 띄는데, free_page() 와 kfree()
 * 는 잠들지 않으므로 스핀락 안이어도 된다. 반면 mempool 을 없애는 일은
 * 잠금을 놓은 뒤로 미뤄 두었다 — 목록과 무관한 일이기도 하고,
 * mempool_destroy() 의 잠금 규약은 이 트리에 mm/ 이 없어 확인 못 함.
 *
 * 여기서 푸는 것은 목록에 있는 요청뿐이다. 밖에 나가 있는 요청(하드웨어에
 * 떠 있거나 어느 하드웨어 큐가 조립 중인 것)은 손대지 않는데, 이 함수가
 * 불리는 시점에는 이미 드라이버가 버스에서 빠지고 모든 장치가 정리된 뒤라
 * 밖에 나가 있는 요청이 없다.
 *
 * [상류 코드 관찰] aidaw_pool 이 NULL 인 채로 이 함수에 닿는 경로가 있다 —
 * scm_alloc_rqs() 가 mempool 을 만들지 못해 -ENOMEM 을 돌려주면
 * scm_blk_init() 이 out_free 로 뛰어 이 함수를 부른다. mempool_destroy() 가
 * NULL 을 견디는지는 이 트리에 mm/ 이 없어 확인 못 함. 또 없앤 뒤 전역
 * 포인터를 NULL 로 되돌리지 않는다. 원본(1f0e418bb6)에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. spin_lock_irq 판을 쓰는 것이 그 증거다 —
 * 인터럽트가 켜져 있음을 전제로 잡고, 놓을 때 무조건 다시 켠다.
 *
 * 에러 경로: 없다. 반환형이 void 다.
 *
 * 호출 체인:
 *   scm_blk_init() 의 실패 경로 / scm_blk_cleanup() → [이 함수]
 *     → list_for_each_safe() → __scm_free_rq() → mempool_destroy()
 */
static void scm_free_rqs(void)
{
	/* [한국어] 훑기용 반복자와, 지금 원소를 지워도 안전하도록 다음 원소를 미리 담아 둘 자리. */
	struct list_head *iter, *safe;
	/* [한국어] 목록 고리에서 되찾은 요청을 담을 자리. */
	struct scm_request *scmrq;

	/* [한국어] 목록 잠금을 잡는다. 이 함수는 모듈 적재 실패와 모듈 해제에서만 불리는
	 * 프로세스 컨텍스트라, 인터럽트가 켜져 있음을 전제로 하는 판을 쓴다. */
	spin_lock_irq(&list_lock);
	/* [한국어] 목록을 훑는다. 훑는 도중에 원소를 지우므로 다음 원소 포인터를 미리 잡아
	 * 두는 판을 쓴다 — 보통의 순회였다면 방금 푼 메모리에서 다음 포인터를 읽게 된다. */
	list_for_each_safe(iter, safe, &inactive_requests) {
		/* [한국어] 목록 고리에서 그것을 품은 struct scm_request 로 되돌린다. */
		scmrq = list_entry(iter, struct scm_request, list);
		/* [한국어] 목록에서 떼어 낸다. 아래에서 이 요청 자체를 풀 것이므로 먼저 떼어야 한다. */
		list_del(&scmrq->list);
		/* [한국어] 이 요청이 붙들고 있던 메모리 세 덩어리를 푼다. 스핀락을 잡은 채이지만
		 * free_page 와 kfree 는 잠들지 않으므로 문제가 없다. */
		__scm_free_rq(scmrq);
	}
	/* [한국어] 잠금을 놓는다. */
	spin_unlock_irq(&list_lock);

	/* [한국어] AIDAW mempool 을 없앤다. 목록과 무관한 일이라 잠금 밖으로 뺐다.
	 * [상류 코드 관찰] scm_alloc_rqs() 가 mempool 을 못 만들고 물러난 경우에도
	 * scm_blk_init() 이 out_free 로 뛰어 이 함수를 부르므로, 전역이 NULL 인 채로
	 * 이 줄에 닿을 수 있다. mempool_destroy() 가 NULL 을 견디는지는 이 트리에
	 * mm/ 이 없어 확인 못 함. 없앤 뒤 전역을 NULL 로 되돌리지도 않는다.
	 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	mempool_destroy(aidaw_pool);
}

/* [한국어]
 * __scm_alloc_rq - struct scm_request 하나를 만들어 놀고 있는 목록에 넣는다
 *
 * @return: 0 = 성공, -ENOMEM = 세 덩어리 중 하나라도 못 얻음.
 *
 * 요청 풀을 채우는 단위 작업이다. 한 번 부를 때마다 요청 하나가 늘어난다.
 *
 * 세 덩어리를 차례로 잡는다.
 *  1. aob_rq_header + scm_request 를 한 덩어리로 잡는다. 두 크기를 더해
 *     한 번에 잡는 것이 요점인데, EADM 계층이 요청을 되돌려줄 때 머리 쪽
 *     포인터를 주기 때문에 둘이 이어져 있어야 to_aobrq() 로 오갈 수 있다.
 *     그 배치의 정확한 규약은 asm/eadm.h 소관이라 이 트리에서 확인 못 함.
 *  2. AOB 용 페이지 하나를 0 으로 채워 잡는다. GFP_DMA 를 쓰는 것은 하드웨어가
 *     닿을 수 있는 낮은 주소여야 한다는 뜻인데, s390 에서 그 경계가 어디인지는
 *     arch/s390 소관이라 이 트리에서 확인 못 함.
 *  3. blk-mq 요청 포인터 배열을 nr_requests_per_io 칸으로 잡는다.
 *
 * 되감기가 한 라벨로 모여 있다. 2 또는 3 에서 실패하면 free 로 뛰어
 * __scm_free_rq() 를 부르는데, 그 함수가 아직 NULL 인 필드도 그대로 넘기게
 * 된다. 첫 덩어리를 0 으로 채워 잡았기 때문에 성립하는 배치다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. GFP_KERNEL 을 쓰므로 잠들 수 있다.
 * 목록에 붙이는 짧은 구간만 spin_lock_irq 로 감싼다.
 *
 * 에러 경로: 세 할당 중 하나라도 실패하면 -ENOMEM. 이미 잡은 것은 이 함수가
 * 풀고 나가지만, 앞선 호출들이 이미 목록에 넣어 둔 요청들은 그대로 남는다 —
 * 그쪽 정리는 호출자 위쪽의 scm_free_rqs() 몫이다.
 *
 * 호출 체인:
 *   scm_alloc_rqs() → [이 함수]
 *     → kzalloc() → get_zeroed_page() → kzalloc_objs() → list_add()
 */
static int __scm_alloc_rq(void)
{
	/* [한국어] 할당의 시작 주소가 될 요청 머리. */
	struct aob_rq_header *aobrq;
	/* [한국어] 머리 뒤에 얹힐 요청 본체. 위 할당의 크기 계산에도 쓰인다. */
	struct scm_request *scmrq;

	/* [한국어] 요청 머리와 요청 본체를 한 덩어리로 잡는다. 둘의 크기를 더해 한 번에
	 * 잡는 것이 요점인데, EADM 계층이 완료 때 머리 쪽을 돌려주므로 둘이 이어져
	 * 있어야 하기 때문이다. 0 으로 채워 잡는 것이 아래 되감기의 전제가 된다.
	 * GFP_KERNEL 이라 잠들 수 있으며, 여기는 모듈 적재 중이라 문제가 없다. */
	aobrq = kzalloc(sizeof(*aobrq) + sizeof(*scmrq), GFP_KERNEL);
	/* [한국어] 할당 실패. */
	if (!aobrq)
		/* [한국어] 메모리 부족으로 물러난다. 아직 아무것도 잡지 않았으므로 되감을 것이 없다. */
		return -ENOMEM;

	/* [한국어] 머리 뒤의 data 자리가 곧 struct scm_request 다. 이 배치 덕에 to_aobrq() 로
	 * 양쪽을 오갈 수 있으며, 그 규약은 asm/eadm.h 소관이라 이 트리에서 확인 못 함. */
	scmrq = (void *) aobrq->data;
	/* [한국어] AOB 용 페이지 하나를 0 으로 채워 잡는다. GFP_DMA 는 하드웨어가 닿을 수
	 * 있는 낮은 주소여야 한다는 표시인데, s390 에서 그 경계가 어디인지는
	 * arch/s390 소관이라 이 트리에서 확인 못 함. 페이지 하나를 통째로 쓰는
	 * 이유는 AOB 뒤의 남는 자리를 AIDAW 로 쓰기 때문이다. */
	scmrq->aob = (void *) get_zeroed_page(GFP_DMA);
	/* [한국어] 페이지를 못 얻었다. */
	if (!scmrq->aob)
		/* [한국어] 되감기로 간다. */
		goto free;

	/* [한국어] blk-mq 요청 포인터 배열을 잡는다. 칸 수는 모듈 매개변수가 정하며,
	 * 이 값이 곧 한 AOB 에 묶을 수 있는 요청 수다. */
	scmrq->request = kzalloc_objs(scmrq->request[0], nr_requests_per_io);
	/* [한국어] 할당 실패. */
	if (!scmrq->request)
		/* [한국어] 되감기로 간다. 이 시점에 AOB 는 이미 잡혀 있으므로 그것도 함께 풀린다. */
		goto free;

	/* [한국어] 목록 고리를 초기화한다. 바로 아래에서 목록에 붙일 것이므로 형식적인
	 * 절차이지만, 붙이기 전 상태를 정의된 값으로 두는 관용이다. */
	INIT_LIST_HEAD(&scmrq->list);
	/* [한국어] 목록 잠금을 잡는다. 프로세스 컨텍스트이므로 인터럽트가 켜져 있음을
	 * 전제로 하는 판을 쓴다. */
	spin_lock_irq(&list_lock);
	/* [한국어] 목록 앞에 넣는다. 이 순간부터 I/O 경로가 이 요청을 꺼내 쓸 수 있다. */
	list_add(&scmrq->list, &inactive_requests);
	/* [한국어] 잠금을 놓는다. 목록을 건드리는 구간만 감싼 짧은 임계 구역이다. */
	spin_unlock_irq(&list_lock);

	/* [한국어] 요청 하나가 풀에 늘었다. */
	return 0;
/* [한국어] 되감기 라벨. 두 번째와 세 번째 할당이 실패했을 때 모두 여기로 온다. */
free:
	/* [한국어] 이미 잡은 것을 푼다. 첫 덩어리를 0 으로 채워 잡았기 때문에, 아직 NULL 인
	 * 필드가 있어도 그대로 넘길 수 있다. */
	__scm_free_rq(scmrq);
	/* [한국어] 메모리 부족으로 물러난다. 앞서 만들어 둔 요청들은 그대로 목록에 남으며,
	 * 그쪽 정리는 scm_free_rqs() 의 몫이다. */
	return -ENOMEM;
}

/* [한국어]
 * scm_alloc_rqs - 요청 풀과 AIDAW mempool 을 한꺼번에 마련한다
 *
 * @nrqs: 만들 struct scm_request 개수. 모듈 매개변수 nr_requests 가 그대로
 *        들어온다.
 * @return: 0 = 전부 성공, -ENOMEM = mempool 을 못 만들었거나 요청을 만들다
 *          실패함.
 *
 * 모듈이 올라올 때 딱 한 번 불려, I/O 경로가 쓸 자원을 미리 다 잡아 둔다.
 * 요청 경로는 인터럽트를 끈 채로 돌거나 스핀락을 잡은 채로 도는 자리가
 * 있어 잠들 수 없으므로, 잠들 수 있는 이 자리에서 미리 만들어 두는 것이다.
 *
 * 먼저 AIDAW 용 페이지 mempool 을 만든다. 크기가 nrqs/8 인 것이 눈에 띄는데,
 * 요청 여덟에 하나꼴로만 AOB 안의 남는 자리가 모자랄 것으로 본 것이다.
 * 0 이 되지 않도록 1 로 바닥을 받친다.
 *
 * 그다음 요청을 nrqs 개 만든다. 반복 조건이 후위 감소와 오류 확인을 한데
 * 묶은 형태라, nrqs 번 돌되 한 번이라도 실패하면 그 자리에서 멈춘다.
 *
 * 실행 컨텍스트: 모듈 적재. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 실패해도 이미 만들어 둔 것을 여기서 풀지 않는다. 호출자인
 * scm_blk_init() 이 out_free 로 뛰어 scm_free_rqs() 를 부르는 것으로 정리가
 * 끝나기 때문이다.
 *
 * 호출 체인:
 *   scm_blk_init() → [이 함수]
 *     → mempool_create_page_pool() → __scm_alloc_rq()
 */
static int scm_alloc_rqs(unsigned int nrqs)
{
	/* [한국어] 반복문의 종료 조건에 쓰이므로 0 에서 시작해야 한다. */
	int ret = 0;

	/* [한국어] AIDAW 용 페이지 mempool 을 만든다. 예비분을 요청 여덟에 하나꼴로 잡는데,
	 * AOB 안의 남는 자리로 대개 충분해 그 정도면 된다고 본 것이다. 0 이 되지
	 * 않도록 1 로 바닥을 받친다. 두 번째 인자는 페이지 할당 표시로, 0 이면
	 * 기본값을 쓴다는 뜻으로 보이나 그 규약은 이 트리에 mm/ 이 없어 확인 못 함. */
	aidaw_pool = mempool_create_page_pool(max(nrqs/8, 1U), 0);
	/* [한국어] 만들기 실패. */
	if (!aidaw_pool)
		/* [한국어] mempool 을 못 만들었다. 아직 요청을 하나도 만들지 않았다. */
		return -ENOMEM;

	/* [한국어] nrqs 번 돌되 한 번이라도 실패하면 멈춘다. 후위 감소라 nrqs 가 0 이 되는
	 * 순간 조건이 거짓이 되어, 정확히 처음 값만큼 돈다. */
	while (nrqs-- && !ret)
		/* [한국어] 요청을 하나 만들어 목록에 넣는다. */
		ret = __scm_alloc_rq();

	/* [한국어] 성공이면 0, 중간에 실패했으면 그 오류가 담겨 있다. */
	return ret;
}

/* [한국어]
 * scm_request_fetch - 놀고 있는 요청 하나를 목록에서 꺼내 온다
 *
 * @return: 꺼낸 struct scm_request 포인터. 목록이 비어 있으면 NULL.
 *
 * I/O 경로가 새 AOB 를 조립하려 할 때 부르는 유일한 조달 창구다. 여기서
 * 새로 할당하지 않는다는 점이 중요한데, 이 자리는 blk-mq 의 queue_rq 안이라
 * 잠들 수 없기 때문이다. 모듈 적재 때 미리 만들어 둔 것을 꺼내 쓸 뿐이다.
 *
 * 목록이 비어 있으면 NULL 을 돌려주고, 호출자는 그것을 자원 부족으로 보고
 * 블록 계층에 BLK_STS_RESOURCE 를 돌려준다. 즉 요청 풀의 크기가 곧 이
 * 드라이버가 동시에 조립할 수 있는 AOB 수의 상한이다.
 *
 * 성공하면 목록에서 떼어 내는 것까지 여기서 한다. 돌려놓는 것은 완료 경로의
 * scm_request_done() 몫이다.
 *
 * out 라벨을 두어 잠금 해제를 한 자리로 모았다 — 목록이 비었을 때와 꺼냈을
 * 때가 같은 마무리를 거친다.
 *
 * 실행 컨텍스트: blk-mq 의 queue_rq 콜백 안. 프로세스 컨텍스트이며 인터럽트가
 * 켜져 있으므로 spin_lock_irq 로 잡는다. 이 목록은 완료 인터럽트에서도
 * 건드리므로(scm_request_done) 인터럽트를 꺼야 교착이 없다.
 *
 * 에러 경로: 없다. 자원 부족을 NULL 로만 알린다.
 *
 * 호출 체인:
 *   scm_blk_request() → [이 함수] → list_first_entry() → list_del()
 */
static struct scm_request *scm_request_fetch(void)
{
	/* [한국어] 못 꺼냈을 때 그대로 나갈 수 있도록 NULL 로 시작한다. */
	struct scm_request *scmrq = NULL;

	/* [한국어] 목록 잠금을 잡는다. 이 자리는 프로세스 컨텍스트라 인터럽트가 켜져 있음을
	 * 전제로 하는 판을 쓰는데, 같은 목록을 완료 인터럽트도 건드리므로
	 * 인터럽트를 꺼야 그 사이에 끼어드는 일이 없다. */
	spin_lock_irq(&list_lock);
	/* [한국어] 놀고 있는 요청이 하나도 없는지 본다. 즉 풀의 크기가 동시에 조립할 수
	 * 있는 AOB 수의 상한이다. */
	if (list_empty(&inactive_requests))
		/* [한국어] NULL 인 채로 나간다. 호출자는 그것을 자원 부족으로 본다. */
		goto out;
	/* [한국어] 목록의 첫 원소를 꺼낸다. 돌려놓을 때도 앞에 넣으므로 방금 쓴 것이 먼저 나온다. */
	scmrq = list_first_entry(&inactive_requests, struct scm_request, list);
	/* [한국어] 목록에서 떼어 낸다. 이 뒤로 이 요청은 목록 밖의 것이 되며, 돌려놓는 일은
	 * scm_request_done() 의 몫이다. */
	list_del(&scmrq->list);
/* [한국어] 잠금 해제를 한 자리로 모으기 위한 라벨. */
out:
	/* [한국어] 잠금을 놓는다. 목록이 비었을 때와 꺼냈을 때가 같은 마무리를 거친다. */
	spin_unlock_irq(&list_lock);
	/* [한국어] 꺼낸 요청 또는 NULL 을 돌려준다. */
	return scmrq;
}

/* [한국어]
 * scm_request_done - 다 쓴 요청의 AIDAW 페이지를 회수하고 요청을 목록에 돌려놓는다
 *
 * @scmrq: 하드웨어 처리가 끝난(또는 되돌려진) struct scm_request.
 *
 * 이 파일의 자원 회수 지점이다. 완료와 재큐 양쪽이 마지막에 여기로 모인다.
 *
 * 앞의 반복문이 이 함수의 핵심이고, 읽는 요령이 하나 있다. AIDAW 는 두 곳에
 * 놓일 수 있다 — AOB 페이지 안의 남는 자리이거나, mempool 에서 꺼낸 별도
 * 페이지다. 앞의 것은 AOB 를 풀 때 함께 사라지므로 여기서 건드리면 안 되고,
 * 뒤의 것만 mempool 에 돌려줘야 한다.
 *
 * 그 둘을 페이지 정렬로 가른다. mempool 에서 꺼낸 페이지는 그 시작 주소가
 * 페이지 경계에 정확히 맞고, AOB 안의 자리는 msb 배열 뒤에 있어 맞을 수 없다.
 * 게다가 한 페이지를 여러 MSB 가 나눠 쓸 수 있는데, 그중 페이지 첫머리를
 * 가리키는 것은 그 페이지를 처음 쓴 MSB 하나뿐이다. 그래서 이 검사는 페이지
 * 하나가 정확히 한 번만 반납되게 하는 구실도 겸한다.
 *
 * msb 배열의 실제 오프셋은 asm/eadm.h 소관이라 이 트리에서 확인 못 함.
 *
 * 반복은 request 배열에서 NULL 을 만나면 멈춘다. AOB 를 다 채우지 못한 채
 * 내보낸 경우를 그렇게 다룬다.
 *
 * 마지막에 요청을 놀고 있는 목록의 앞쪽에 되돌려 놓는다. 앞에 넣는 것은
 * 방금 쓴 것을 다시 꺼내 쓰게 해 캐시에 유리한 배치다.
 *
 * 실행 컨텍스트: 완료 인터럽트 문맥에서도 불리고, 재큐 경로에서도 불린다.
 * 그래서 이 파일에서 유일하게 list_lock 을 irqsave 판으로 잡는다 — 이미
 * 인터럽트가 꺼져 있을 수 있으므로 원래 상태를 저장했다가 되돌려야 한다.
 *
 * 에러 경로: 없다. 반환형이 void 다.
 *
 * 호출 체인:
 *   scm_request_finish() / scm_request_requeue() → [이 함수]
 *     → dma64_to_virt() → mempool_free() → list_add()
 */
static void scm_request_done(struct scm_request *scmrq)
{
	/* [한국어] 인터럽트 상태를 저장해 둘 자리. 아래 잠금이 irqsave 판이라 필요하다. */
	unsigned long flags;
	/* [한국어] 지금 보고 있는 MSB. */
	struct msb *msb;
	/* [한국어] AIDAW 목록의 주소를 정수로 받을 자리. 정렬을 따져야 해서 포인터가 아니다. */
	u64 aidaw;
	/* [한국어] 훑기용 첨자. */
	int i;

	/* [한국어] 묶여 있던 요청 수만큼 훑는다. NULL 을 만나면 멈추므로 AOB 를 다 채우지
	 * 못한 채 내보낸 경우도 그대로 다룬다. */
	for (i = 0; i < nr_requests_per_io && scmrq->request[i]; i++) {
		/* [한국어] 이 칸의 MSB. 요청 배열의 i 번째와 MSB 의 i 번째가 같은 요청을 가리킨다는
		 * 조립 규칙 덕에 첨자를 그대로 쓸 수 있다. */
		msb = &scmrq->aob->msb[i];
		/* [한국어] MSB 가 가리키는 AIDAW 목록의 주소를 커널 가상 주소로 되돌린다.
		 * 정렬을 따지려고 정수로 받는다. */
		aidaw = (u64)dma64_to_virt(msb->data_addr);

		/* [한국어] AIDAW 목록을 쓰는 MSB 이고 주소가 유효한지 먼저 본다. 채우지 않은 칸을
		 * 걸러내기 위한 확인이다. */
		if ((msb->flags & MSB_FLAG_IDA) && aidaw &&
		    /* [한국어] 페이지 경계에 정확히 맞는지 본다. 이것이 두 종류의 AIDAW 를 가르는
		     * 기준이다 — mempool 에서 꺼낸 페이지는 그 시작이 경계에 맞고, AOB 안의
		     * 자리는 msb 배열 뒤에 있어 맞을 수 없다. 게다가 한 페이지를 여러 MSB 가
		     * 나눠 쓸 때 그 첫머리를 가리키는 것은 처음 쓴 MSB 하나뿐이라, 이 검사가
		     * 페이지 하나를 정확히 한 번만 반납하게 하는 구실도 겸한다. */
		    IS_ALIGNED(aidaw, PAGE_SIZE))
			/* [한국어] 이 페이지를 mempool 에 돌려준다. 커널 가상 주소를 struct page 로 되돌려
			 * 넘긴다. */
			mempool_free(virt_to_page((void *)aidaw), aidaw_pool);
	}

	/* [한국어] 목록 잠금을 잡는다. 이 파일에서 유일하게 irqsave 판을 쓰는 자리인데,
	 * 이 함수가 완료 인터럽트 문맥에서도 불려 이미 인터럽트가 꺼져 있을 수
	 * 있기 때문이다. 나머지 세 자리(요청 만들기·꺼내기·풀기)는 모두 프로세스
	 * 컨텍스트라 그냥 spin_lock_irq 를 쓴다. */
	spin_lock_irqsave(&list_lock, flags);
	/* [한국어] 요청을 목록 앞쪽에 되돌려 놓는다. 앞에 넣는 것은 방금 쓴 것을 다시 꺼내
	 * 쓰게 해 캐시에 유리한 배치다. */
	list_add(&scmrq->list, &inactive_requests);
	/* [한국어] 잠금을 놓고 인터럽트 상태를 원래대로 돌린다. */
	spin_unlock_irqrestore(&list_lock, flags);
}

/* [한국어]
 * scm_permit_request - 이 요청을 지금 받아도 되는지 한 줄로 판정한다
 *
 * @bdev: 요청이 향하는 블록 장치.
 * @req: 판정할 blk-mq 요청.
 * @return: true = 받아도 된다, false = 지금은 받으면 안 된다.
 *
 * SCM 증분은 하드웨어 사정으로 쓰기만 막힌 상태가 될 수 있다. 그 상태에서
 * 읽기는 계속 받아야 하므로, 방향과 상태를 함께 보는 판정이 필요하다.
 *
 * 판정식이 짧지만 두 갈래를 담고 있다.
 *   - 읽기면 상태와 무관하게 언제나 통과한다.
 *   - 쓰기면 상태가 쓰기 금지가 아닐 때만 통과한다.
 *
 * false 를 받은 호출자는 요청을 하드웨어에 넣지 않고 블록 계층에
 * BLK_STS_RESOURCE 를 돌려준다. 그러면 blk-mq 가 잠시 뒤 다시 시도하므로,
 * 증분이 돌아오면 저절로 흐름이 이어진다.
 *
 * [상류 코드 관찰] bdev->lock 을 잡지 않고 state 를 읽는다. 그 잠금은 상태를
 * 바꾸는 두 자리(쓰기 금지로 내리는 곳과 되돌리는 곳)에서만 쓰인다. 여기서
 * 한 박자 묵은 값을 읽더라도, 잘못 통과한 쓰기는 하드웨어가 다시 쓰기 금지
 * 응답을 주어 걸러지고 잘못 막힌 쓰기는 다음 재시도에서 통과하므로 손해가
 * 없다는 판단으로 보인다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지
 * 않았다.
 *
 * 실행 컨텍스트: scm_blk_request() 안, sq->lock 을 잡은 채로 불린다.
 * 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   scm_blk_request() → [이 함수] → rq_data_dir()
 */
static bool scm_permit_request(struct scm_blk_dev *bdev, struct request *req)
{
	/* [한국어] 읽기면 상태와 무관하게 통과하고, 쓰기면 쓰기 금지 상태가 아닐 때만
	 * 통과한다. 두 갈래를 논리합 하나로 적은 형태다.
	 * [상류 코드 관찰] bdev->lock 을 잡지 않고 state 를 읽는다.
	 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	return rq_data_dir(req) != WRITE || bdev->state != SCM_WR_PROHIBIT;
}

/* [한국어]
 * scm_aidaw_alloc - AIDAW 로 쓸 페이지 하나를 mempool 에서 꺼낸다
 *
 * @return: 페이지의 커널 가상 주소. 못 얻으면 NULL.
 *
 * AOB 페이지 안의 남는 자리로 모자랄 때만 불리는 예비 조달 창구다.
 *
 * GFP_ATOMIC 이 이 함수의 전부라고 해도 된다. 이 함수가 불리는 자리는
 * scm_blk_request() 가 sq->lock 을 잡고 있는 안쪽이라 잠들 수 없고, 그래서
 * 잠들지 않는 할당 표시를 쓴다. 대신 실패할 수 있으며, 실패는 NULL 로 나가
 * 결국 블록 계층에 BLK_STS_RESOURCE 로 전해진다.
 *
 * 일반 페이지 할당이 아니라 mempool 을 쓰는 이유도 같은 맥락이다. mempool 은
 * 미리 잡아 둔 예비분을 갖고 있어, 메모리가 말라도 최소한의 진행을 보장한다.
 *
 * mempool 이 돌려주는 것은 struct page 이므로 page_address() 로 커널 가상
 * 주소로 바꿔 돌려준다.
 *
 * 실행 컨텍스트: queue_rq 경로, sq->lock 을 잡은 채. 프로세스 컨텍스트.
 *
 * 에러 경로: NULL 반환. 호출자가 그대로 위로 올린다.
 *
 * 호출 체인:
 *   scm_aidaw_fetch() → [이 함수] → mempool_alloc() → page_address()
 */
static inline struct aidaw *scm_aidaw_alloc(void)
{
	/* [한국어] AIDAW 용 페이지를 mempool 에서 꺼낸다. GFP_ATOMIC 인 것이 중요한데,
	 * 이 자리는 scm_blk_request() 가 sq->lock 을 잡고 있는 안쪽이라 잠들 수
	 * 없기 때문이다. 일반 페이지 할당이 아니라 mempool 을 쓰는 것도 같은
	 * 맥락으로, 미리 잡아 둔 예비분이 있어 메모리가 말라도 진행이 보장된다. */
	struct page *page = mempool_alloc(aidaw_pool, GFP_ATOMIC);

	/* [한국어] mempool 이 돌려주는 것은 struct page 이므로 커널 가상 주소로 바꿔 준다.
	 * 못 얻었으면 NULL 을 그대로 올린다. */
	return page ? page_address(page) : NULL;
}

/* [한국어]
 * scm_aidaw_bytes - 이 자리부터 페이지 끝까지의 AIDAW 로 몇 바이트를 실을 수 있는지 센다
 *
 * @aidaw: 다음에 쓸 AIDAW 자리. 보통 scmrq->next_aidaw 가 들어온다.
 * @return: 이 자리부터 페이지 경계 전까지 남은 AIDAW 칸으로 다룰 수 있는
 *          데이터 바이트 수.
 *
 * AIDAW 목록은 페이지 하나를 넘어갈 수 없다. 하드웨어가 그 목록을 연속된
 * 메모리로 읽기 때문인데, 그 규약은 asm/eadm.h 소관이라 이 트리에서 확인
 * 못 함. 그래서 새 요청을 얹기 전에 이 자리에서 페이지 끝까지 남은 칸이
 * 충분한지 미리 세어야 한다.
 *
 * 계산이 두 단계다.
 *   1. 지금 주소를 페이지 크기로 올림한 값에서 지금 주소를 뺀다 — 페이지
 *      경계까지 남은 바이트 수다.
 *   2. 그 바이트를 AIDAW 한 칸의 크기로 나눠 남은 칸 수를 얻고, 칸 하나가
 *      데이터 한 페이지를 가리키므로 페이지 크기를 곱한다.
 *
 * 즉 돌려주는 값은 남은 메모리 크기가 아니라 다룰 수 있는 데이터 크기다.
 *
 * 경계에 정확히 걸린 주소가 들어오면 1 단계의 올림이 제자리라 0 이 나온다.
 * "남은 칸이 없다" 는 뜻이니 옳은 답이고, 그 경우 호출자는 새 페이지를
 * 얻으러 간다.
 *
 * 실행 컨텍스트: queue_rq 경로. 프로세스 컨텍스트. 순수 계산이라 부작용이 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   scm_aidaw_fetch() → [이 함수]
 */
static inline unsigned long scm_aidaw_bytes(struct aidaw *aidaw)
{
	/* [한국어] 포인터 산술 대신 정수 산술을 하려고 부호 없는 긴 정수로 옮긴다. */
	unsigned long _aidaw = (unsigned long) aidaw;
	/* [한국어] 지금 주소를 페이지 크기로 올림한 값에서 지금 주소를 뺀다 — 페이지 경계까지
	 * 남은 바이트 수다. 주소가 이미 경계에 맞아 있으면 0 이 나오는데, 남은 칸이
	 * 없다는 뜻이라 옳은 답이다. */
	unsigned long bytes = ALIGN(_aidaw, PAGE_SIZE) - _aidaw;

	/* [한국어] 남은 칸 수에 칸 하나가 가리키는 데이터 크기(페이지 하나)를 곱한다.
	 * 즉 돌려주는 값은 남은 메모리 크기가 아니라 다룰 수 있는 데이터 크기다. */
	return (bytes / sizeof(*aidaw)) * PAGE_SIZE;
}

/* [한국어]
 * scm_aidaw_fetch - 요청 하나를 담을 만한 AIDAW 자리를 마련한다
 *
 * @scmrq: 조립 중인 요청. 이 안의 next_aidaw 커서를 본다.
 * @bytes: 이번에 실어야 할 데이터 바이트 수. 호출자가 blk_rq_bytes() 로 구해
 *         넘긴다.
 * @return: 쓸 수 있는 AIDAW 시작 자리. 못 얻으면 NULL.
 *
 * AIDAW 를 어디에 둘지 정하는 두 갈래 결정이다.
 *   - 지금 커서 자리부터 페이지 끝까지로 충분하면 그 자리를 그대로 준다.
 *     AOB 페이지의 남는 자리를 쓰는 경우가 여기 해당하며, 추가 할당이 없다.
 *   - 모자라면 mempool 에서 페이지를 하나 꺼내 그 첫머리를 준다.
 *
 * 새로 꺼낸 페이지를 0 으로 지우는 것이 눈에 띈다. mempool 이 돌려주는
 * 페이지에는 앞서 쓰던 내용이 남아 있으므로, 쓰지 않은 칸에 옛 주소가 남아
 * 하드웨어에 넘어가는 일을 막는다.
 *
 * [상류 코드 관찰] 이 함수만 static 이 아니고 scm_blk.h 에도 선언이 있는데,
 * 정작 부르는 곳은 같은 파일 안의 scm_request_prepare() 한 곳뿐이다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 돌려준 자리를 어디까지 썼는지 이 함수는 모른다. 커서를 앞으로 미는 일은
 * 실제로 칸을 채운 scm_request_prepare() 가 맡는다.
 *
 * 실행 컨텍스트: queue_rq 경로, sq->lock 안. 프로세스 컨텍스트이며 잠들 수
 * 없다.
 *
 * 에러 경로: 새 페이지를 못 얻으면 NULL. 호출자가 -ENOMEM 으로 바꿔 올린다.
 *
 * 호출 체인:
 *   scm_request_prepare() → [이 함수]
 *     → scm_aidaw_bytes() → scm_aidaw_alloc() → memset()
 */
struct aidaw *scm_aidaw_fetch(struct scm_request *scmrq, unsigned int bytes)
{
	/* [한국어] 돌려줄 AIDAW 시작 자리. */
	struct aidaw *aidaw;

	/* [한국어] 지금 커서 자리부터 페이지 끝까지로 이번 요청을 다 실을 수 있는지 본다. */
	if (scm_aidaw_bytes(scmrq->next_aidaw) >= bytes)
		/* [한국어] 그 자리를 그대로 준다. 추가 할당이 없는 흔한 경우이며, AOB 페이지의
		 * 남는 자리를 쓰는 것이 대개 여기 해당한다. */
		return scmrq->next_aidaw;

	/* [한국어] 모자라니 mempool 에서 페이지를 하나 꺼낸다. 잠들지 않는 할당이라 실패할 수 있다. */
	aidaw = scm_aidaw_alloc();
	/* [한국어] 얻었을 때만 지운다. 못 얻었으면 지울 곳이 없다. */
	if (aidaw)
		/* [한국어] 새로 꺼낸 페이지를 0 으로 지운다. mempool 이 돌려주는 페이지에는 앞서
		 * 쓰던 내용이 남아 있으므로, 쓰지 않은 칸에 옛 주소가 남아 하드웨어에
		 * 넘어가는 일을 막는다. */
		memset(aidaw, 0, PAGE_SIZE);
	/* [한국어] 새로 얻은 페이지의 첫머리를 준다. 못 얻었으면 NULL 이 그대로 나가고,
	 * 호출자가 그것을 -ENOMEM 으로 바꾼다. */
	return aidaw;
}

/* [한국어]
 * scm_request_prepare - blk-mq 요청 하나를 MSB 한 칸과 AIDAW 목록으로 번역한다
 *
 * @scmrq: 조립 중인 요청. 이 안의 AOB 에 새 MSB 한 칸이 채워진다.
 * @return: 0 = 성공, -ENOMEM = AIDAW 자리를 못 얻음.
 *
 * 이 파일에서 하드웨어 서술자를 실제로 쓰는 유일한 함수이며, 드라이버의
 * 번역 규칙이 여기 다 들어 있다.
 *
 * 어느 요청을 다룰지는 인자로 받지 않는다. AOB 안의 msb_count 가 곧 커서라,
 * 그 값으로 MSB 칸과 요청 배열 칸을 동시에 집는다. 호출자가 바로 앞에서
 * scm_request_set() 으로 그 칸에 요청을 꽂아 두었기 때문에 성립하는 방식이다.
 *
 * 채우는 순서는 다음과 같다.
 *  1. 이 요청의 전체 바이트 수만큼 실을 AIDAW 자리를 먼저 확보한다. 실패하면
 *     아무것도 건드리지 않은 채로 물러나므로, 되감을 것이 없다. 특히 커서인
 *     msb_count 를 아직 올리지 않은 시점이라는 점이 중요하다.
 *  2. 블록 크기를 4K 로 지정한다.
 *  3. 커서를 한 칸 올린다. 이 줄부터 이 MSB 는 이 요청의 것이 된다.
 *  4. 장치 주소에 요청의 시작 섹터를 바이트로 바꿔 더해, 이 전송의 SCM 주소를
 *     만든다. SCM 은 디스크가 아니라 주소로 접근하는 영속 메모리라, 섹터가
 *     아니라 주소가 필요하다.
 *  5. 읽기/쓰기 연산 부호를 넣는다.
 *  6. 데이터 주소가 직접 버퍼가 아니라 AIDAW 목록임을 표시하는 깃발을 켠다.
 *  7. 데이터 주소 자리에 AIDAW 목록의 시작을 넣는다.
 *  8. 요청의 조각들을 훑으며 AIDAW 를 한 칸씩 채우고, 4K 블록 수를 센다.
 *
 * 마지막에 커서를 다음 요청이 이어 쓸 자리로 옮긴다. 이 한 줄이 있어서 다음
 * 요청이 같은 페이지의 남은 자리를 이어 쓸 수 있다.
 *
 * MSB 의 각 필드가 하드웨어에서 어떤 뜻인지, 상수들의 실제 값이 무엇인지는
 * asm/eadm.h 소관이라 이 트리에서 확인 못 함. 쓰임새로 읽히는 것까지만 적었다.
 *
 * 실행 컨텍스트: blk-mq 의 queue_rq 콜백 안, sq->lock 을 잡은 채로 불린다.
 * 프로세스 컨텍스트이며 잠들 수 없다.
 *
 * 에러 경로: AIDAW 자리를 못 얻은 경우뿐이다. 호출자가 그것을 받아 이미
 * 채워 둔 MSB 들만이라도 하드웨어에 내보내고, 이 요청은 블록 계층에 되돌린다.
 *
 * 호출 체인:
 *   scm_blk_request() → [이 함수]
 *     → scm_aidaw_fetch() → rq_for_each_segment() → virt_to_dma64()
 */
static int scm_request_prepare(struct scm_request *scmrq)
{
	/* [한국어] 장치 상태를 거쳐 SCM 장치에 닿기 위해 꺼내 둔다. */
	struct scm_blk_dev *bdev = scmrq->bdev;
	/* [한국어] gendisk 에 매달아 둔 SCM 장치를 되찾는다. 장치의 시작 주소가 필요해서다. */
	struct scm_device *scmdev = bdev->gendisk->private_data;
	/* [한국어] AOB 의 msb_count 가 곧 커서다. 이 값으로 MSB 칸과 요청 배열 칸을 동시에 집는다. */
	int pos = scmrq->aob->request.msb_count;
	/* [한국어] 그 칸의 MSB. 아래에서 이 요청의 전송 명세를 여기에 적는다. */
	struct msb *msb = &scmrq->aob->msb[pos];
	/* [한국어] 이 칸에 해당하는 blk-mq 요청. 호출자가 바로 앞에서 꽂아 둔 것이다. */
	struct request *req = scmrq->request[pos];
	/* [한국어] 조각 훑기용 반복자. include/linux/blk-mq.h 의 struct req_iterator 다. */
	struct req_iterator iter;
	/* [한국어] AIDAW 칸을 채워 나갈 커서. */
	struct aidaw *aidaw;
	/* [한국어] 훑기 중 현재 조각을 받을 자리. */
	struct bio_vec bv;

	/* [한국어] 이 요청의 전체 바이트 수만큼 실을 수 있는 AIDAW 자리를 먼저 확보한다.
	 * AOB 페이지의 남는 자리로 충분하면 그 자리를, 모자라면 새 페이지를 준다. */
	aidaw = scm_aidaw_fetch(scmrq, blk_rq_bytes(req));
	/* [한국어] 확보 실패. */
	if (!aidaw)
		/* [한국어] AIDAW 자리를 못 얻었다. 아직 아무것도 건드리지 않았으므로 그대로 물러난다. */
		return -ENOMEM;

	/* [한국어] 블록 크기를 4K 로 지정한다. 큐 한계의 논리 블록 크기와 같은 값이다.
	 * 상수의 실제 값은 asm/eadm.h 소관이라 이 트리에서 확인 못 함. */
	msb->bs = MSB_BS_4K;
	/* [한국어] 커서를 한 칸 올린다. 이 줄부터 이 MSB 는 이 요청의 것이 된다. 위의 실패
	 * 반환이 이 줄보다 앞에 있어서, 실패했을 때 되감을 것이 없다. */
	scmrq->aob->request.msb_count++;
	/* [한국어] 이 전송의 SCM 주소를 만든다. 요청의 시작 섹터를 9 비트 왼쪽으로 밀어
	 * 512바이트 섹터를 바이트로 바꾼 뒤, 장치의 시작 주소에 더한다. SCM 은
	 * 디스크가 아니라 주소로 접근하는 영속 메모리라 섹터가 아니라 주소가 필요하다. */
	msb->scm_addr = scmdev->address + ((u64) blk_rq_pos(req) << 9);
	/* [한국어] 읽기/쓰기 연산 부호를 넣는다. 방향은 blk-mq 요청에서 가져온다. */
	msb->oc = (rq_data_dir(req) == READ) ? MSB_OC_READ : MSB_OC_WRITE;
	/* [한국어] 데이터 주소가 직접 버퍼가 아니라 AIDAW 목록임을 표시한다. 이 드라이버는
	 * 언제나 목록을 쓰므로 이 깃발이 늘 켜진다. 초기화 때 0 으로 지웠으므로
	 * 논리합이지만 결과는 대입과 같다. */
	msb->flags |= MSB_FLAG_IDA;
	/* [한국어] 데이터 주소 자리에 AIDAW 목록의 시작을 넣는다. 위 깃발과 짝이 되는 값이다. */
	msb->data_addr = virt_to_dma64(aidaw);

	/* [한국어] 요청의 데이터 조각들을 훑는다. include/linux/blk-mq.h 의 이 매크로는
	 * 요청에 달린 bio 들을 돌며 bio_for_each_segment() 로 조각을 하나씩 준다.
	 * 그 조각은 페이지 경계를 넘지 않으므로 조각 하나가 AIDAW 칸 하나가 된다. */
	rq_for_each_segment(bv, req, iter) {
		/* [한국어] 조각이 페이지 첫머리에서 시작하지 않으면 경고한다. AIDAW 칸 하나가 페이지
		 * 하나를 통째로 가리키므로, 페이지 중간에서 시작하는 조각은 표현할 수 없다.
		 * 논리 블록 크기를 4K 로 잡아 두었기 때문에 평소에는 걸리지 않는다. */
		WARN_ON(bv.bv_offset);
		/* [한국어] 이 조각이 몇 개의 4K 블록인지 더한다. 12 비트 오른쪽 이동이 4096 으로
		 * 나누기이며, 위에서 블록 크기를 4K 로 정한 것과 짝을 이룬다.
		 * 초기화 때 AOB 를 0 으로 지웠으므로 누적이 0 에서 시작한다. */
		msb->blk_count += bv.bv_len >> 12;
		/* [한국어] 이 조각이 든 페이지의 주소를 AIDAW 칸에 적는다. struct page 를 커널 가상
		 * 주소로 바꾼 뒤 하드웨어가 읽는 64비트 주소 형으로 바꾼다. */
		aidaw->data_addr = virt_to_dma64(page_address(bv.bv_page));
		/* [한국어] 다음 AIDAW 칸으로 옮긴다. 포인터 산술이라 칸 하나 크기만큼 움직인다. */
		aidaw++;
	}

	/* [한국어] 커서를 다음 요청이 이어 쓸 자리로 옮긴다. 위 반복문이 aidaw 를 쓴 칸
	 * 수만큼 밀어 놓았으므로, 그 값이 그대로 다음 시작점이 된다. 이 한 줄이
	 * 있어서 다음 요청이 같은 페이지의 남은 자리를 이어 쓸 수 있다. */
	scmrq->next_aidaw = aidaw;
	/* [한국어] 번역 성공. */
	return 0;
}

/* [한국어]
 * scm_request_set - 조립 중인 AOB 의 현재 칸에 blk-mq 요청을 꽂는다
 *
 * @scmrq: 조립 중인 요청.
 * @req: 꽂을 blk-mq 요청. NULL 을 넣으면 방금 꽂은 것을 지우는 셈이 된다.
 *
 * 한 줄짜리 함수지만 이 파일의 조립 규칙 하나를 담고 있다 — 요청 배열의
 * 칸 번호가 AOB 의 msb_count 와 같다는 규칙이다. 그래서 요청 배열의 i 번째와
 * MSB 의 i 번째가 언제나 같은 요청을 가리킨다.
 *
 * 이 함수는 커서를 올리지 않는다. 커서는 scm_request_prepare() 가 MSB 를 다
 * 채운 뒤에 올린다. 그래서 이 함수와 그 함수 사이에서 실패하면, 커서가
 * 그대로라 아무 일도 없던 것이 된다.
 *
 * NULL 을 넣는 쓰임이 실제로 있다. scm_request_prepare() 가 실패했을 때
 * 호출자가 방금 꽂은 요청을 지우는 데 쓴다 — 그러면 배열을 훑는 쪽이 그
 * 자리에서 멈추므로, 준비되지 않은 요청이 완료 처리되지 않는다.
 *
 * 실행 컨텍스트: queue_rq 경로, sq->lock 안. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환형이 void 다.
 *
 * 호출 체인:
 *   scm_blk_request() → [이 함수]
 */
static inline void scm_request_set(struct scm_request *scmrq,
				   struct request *req)
{
	/* [한국어] 요청을 AOB 의 현재 칸에 꽂는다. 칸 번호로 msb_count 를 쓰는 것이 이 파일의
	 * 조립 규칙이다 — 요청 배열의 i 번째와 MSB 의 i 번째가 같은 요청을 가리킨다.
	 * 커서를 여기서 올리지 않는 것이 중요한데, 아래 번역이 실패하면 아무 일도
	 * 없던 것이 되어야 하기 때문이다. */
	scmrq->request[scmrq->aob->request.msb_count] = req;
}

/* [한국어]
 * scm_request_init - 목록에서 갓 꺼낸 요청을 새 AOB 조립을 위해 초기화한다
 *
 * @bdev: 이 요청이 향할 블록 장치.
 * @scmrq: 방금 scm_request_fetch() 로 꺼내 온 요청.
 *
 * 요청은 풀에서 돌려 쓰는 물건이라 앞서 쓰던 내용이 남아 있다. 이 함수가
 * 그것을 지우고 새 조립을 시작할 수 있는 상태로 만든다.
 *
 * 지우는 것이 둘, 새로 넣는 것이 다섯이다.
 *   지운다: 요청 포인터 배열 전체와 AOB 전체. 배열을 0 으로 지우는 것이
 *     중요한데, 이 파일의 모든 순회가 NULL 을 만나면 멈추는 방식이라 그
 *     NULL 이 배열의 끝 표시 노릇을 하기 때문이다.
 *   넣는다: 완료 때 되찾을 장치 포인터, AOB 의 명령 부호, 완료 때 EADM 이
 *     돌려줄 표식, 이 요청이 향할 장치, 재시도 횟수, 초기 결과값.
 *
 * AOB 의 data 자리에 aob_rq_header 주소를 넣는 것이 완료 경로의 열쇠다.
 * 하드웨어 처리가 끝나면 그 값을 근거로 EADM 계층이 이 요청을 되찾아
 * scm_blk_irq() 의 data 인자로 넘겨 준다. 그 되찾는 과정 자체는 asm/eadm.h
 * 소관이라 이 트리에서 확인 못 함.
 *
 * 마지막 줄이 이 파일의 메모리 배치를 결정한다. AIDAW 커서를 msb 배열의
 * nr_requests_per_io 번째 자리, 즉 실제로 쓸 MSB 들 바로 뒤에 둔다. AOB
 * 페이지에서 남는 뒤쪽을 AIDAW 로 쓰겠다는 뜻이며, 그 덕에 대부분의 요청은
 * 따로 페이지를 얻지 않아도 된다. 상류 영어 주석이 그 의도를 밝히고 있다.
 *
 * 실행 컨텍스트: queue_rq 경로, sq->lock 안. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환형이 void 다.
 *
 * 호출 체인:
 *   scm_blk_request() → [이 함수] → to_aobrq() → memset()
 */
static inline void scm_request_init(struct scm_blk_dev *bdev,
				    struct scm_request *scmrq)
{
	/* [한국어] 요청 머리를 되찾는다. 할당이 머리 뒤에 요청을 얹는 형태였으므로 이렇게
	 * 거슬러 올라갈 수 있다. */
	struct aob_rq_header *aobrq = to_aobrq(scmrq);
	/* [한국어] AOB 를 자주 쓰므로 지역 변수에 꺼내 둔다. */
	struct aob *aob = scmrq->aob;

	/* [한국어] 요청 포인터 배열을 0 으로 지운다. 이 파일의 모든 순회가 NULL 을 만나면
	 * 멈추는 방식이라, 그 0 이 배열의 끝 표시 노릇을 한다. */
	memset(scmrq->request, 0,
	       /* [한국어] 칸 수와 칸 하나의 크기를 곱해 배열 전체 길이를 구한다. */
	       nr_requests_per_io * sizeof(scmrq->request[0]));
	/* [한국어] AOB 전체를 0 으로 지운다. 앞서 쓰던 MSB 와 msb_count 가 함께 지워져,
	 * 커서가 0 에서 다시 시작한다. */
	memset(aob, 0, sizeof(*aob));
	/* [한국어] 요청 머리에 이 요청이 향할 SCM 장치를 적는다. EADM 계층이 어느 장치로
	 * 보낼지 여기서 읽는 것으로 보이나, 그 규약은 이 트리에서 확인 못 함. */
	aobrq->scmdev = bdev->scmdev;
	/* [한국어] AOB 의 명령 부호를 데이터 이동으로 정한다. 이 드라이버가 내는 명령은
	 * 이 하나뿐이다 — 상수의 실제 값은 asm/eadm.h 소관이라 이 트리에서 확인 못 함. */
	aob->request.cmd_code = ARQB_CMD_MOVE;
	/* [한국어] AOB 의 data 자리에 요청 머리의 주소를 심는다. 완료 경로의 열쇠인데,
	 * 하드웨어 처리가 끝나면 EADM 계층이 이 값을 근거로 이 요청을 되찾아
	 * scm_blk_irq() 의 data 인자로 넘겨 준다. 그 되찾는 과정은 asm/eadm.h
	 * 소관이라 이 트리에서 확인 못 함. */
	aob->request.data = (u64) aobrq;
	/* [한국어] 이 요청이 향할 블록 장치를 기억한다. 완료·재큐 경로가 여기서 계수와 잠금에 닿는다. */
	scmrq->bdev = bdev;
	/* [한국어] 재시도 횟수를 4 로 채운다. 완료 인터럽트가 오류를 볼 때마다 하나씩 깎는다. */
	scmrq->retries = 4;
	/* [한국어] 결과를 정상으로 초기화한다. 완료 인터럽트가 하드웨어가 준 값으로 덮는다. */
	scmrq->error = BLK_STS_OK;
	/* [한국어] 아래 줄의 상류 영어 주석 — msb 배열을 다 쓰지 않으므로 남는 뒤쪽에
	 * AIDAW 를 둔다는 뜻이다. */
	/* We don't use all msbs - place aidaws at the end of the aob page. */
	/* [한국어] AIDAW 커서를 실제로 쓸 MSB 들 바로 뒤에 둔다. AOB 페이지에서 남는 뒤쪽을
	 * AIDAW 로 쓰겠다는 뜻이며, 그 덕에 대부분의 요청은 따로 페이지를 얻지 않아도
	 * 된다. msb 배열이 실제로 몇 칸인지, 그 뒤에 얼마나 남는지는 asm/eadm.h
	 * 소관이라 이 트리에서 확인 못 함. */
	scmrq->next_aidaw = (void *) &aob->msb[nr_requests_per_io];
}

/* [한국어]
 * scm_request_requeue - 묶여 있던 요청들을 블록 계층에 되돌리고 자원을 반납한다
 *
 * @scmrq: 되돌릴 요청. 하드웨어에 넣지 못했거나, 넣었으나 다시 시도할 수
 *         없게 된 것이다.
 *
 * 오류 경로의 마무리다. 이 함수가 불리는 자리는 두 곳뿐이며 둘 다 오류다 —
 * eadm_start_aob() 이 하위 채널을 못 잡은 경우와, 하드웨어가 쓰기 금지
 * 응답을 준 경우다.
 *
 * 완료(scm_request_finish)와 대비하면 성격이 분명해진다. 완료는 각 요청을
 * 끝난 것으로 처리하지만, 이쪽은 각 요청을 아직 안 한 것으로 되돌린다.
 * 그래서 사용자에게는 오류가 보이지 않고, 블록 계층이 나중에 다시 내려보낸다.
 *
 * 되돌릴 때 두 번째 인자를 false 로 주어 요청마다 큐를 깨우지 않는다. 대신
 * 다 되돌린 뒤 마지막에 한 번만 깨우는 배치인데, 요청 여러 개를 묶어 다루는
 * 이 드라이버에서는 그편이 낫다.
 *
 * [상류 코드 관찰] 그 마지막 한 줄이 넘기는 bdev->rq 는 이 트리 어디에서도
 * 값이 대입되지 않는 필드다. struct scm_blk_dev 는 scm_drv.c 의 probe 가
 * 0 으로 채워 할당하므로 언제나 NULL 이며, blk_mq_kick_requeue_list() 는
 * block/blk-mq.c 에서 받은 큐의 requeue_work 를 그대로 건드린다(NULL 검사가
 * 없다). 즉 이 줄에 닿으면 NULL 을 따라가게 된다. 이 함수 자체가 두 오류
 * 경로에서만 불리므로 정상 동작에서는 닿지 않는다. 원본(1f0e418bb6)에서
 * 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 두 호출 자리의 문맥이 다르다. eadm_start_aob() 실패 쪽은
 * queue_rq 경로(프로세스 컨텍스트)이고, 쓰기 금지 쪽은 완료 인터럽트
 * 문맥이다. 그래서 이 함수가 부르는 것들은 모두 인터럽트 문맥을 견뎌야 한다.
 *
 * 에러 경로: 없다. 이 함수 자체가 에러 경로다.
 *
 * 호출 체인:
 *   scm_request_start() / scm_blk_handle_error() → [이 함수]
 *     → blk_mq_requeue_request() → scm_request_done()
 *     → blk_mq_kick_requeue_list()
 */
static void scm_request_requeue(struct scm_request *scmrq)
{
	/* [한국어] 계수와 잠금에 닿기 위해 장치를 꺼내 둔다. */
	struct scm_blk_dev *bdev = scmrq->bdev;
	/* [한국어] 훑기용 첨자. */
	int i;

	/* [한국어] 묶여 있던 요청들을 차례로 훑는다. NULL 을 만나면 멈춘다. */
	for (i = 0; i < nr_requests_per_io && scmrq->request[i]; i++)
		/* [한국어] 요청 하나를 블록 계층에 되돌린다. 두 번째 인자가 거짓이라 여기서는 큐를
		 * 깨우지 않는다 — 요청마다 깨우는 대신 다 되돌린 뒤 아래에서 한 번만 깨우는
		 * 배치다. block/blk-mq.c 의 이 함수는 요청을 큐의 되돌림 목록 끝에 붙인다. */
		blk_mq_requeue_request(scmrq->request[i], false);

	/* [한국어] 떠 있는 요청 수를 내린다. scm_request_start() 가 올린 것의 짝이다. */
	atomic_dec(&bdev->queued_reqs);
	/* [한국어] AIDAW 페이지를 회수하고 이 요청을 놀고 있는 목록에 돌려놓는다. */
	scm_request_done(scmrq);
	/* [한국어] 블록 계층에 되돌린 요청들을 다시 내보내라고 큐를 깨운다.
	 * [상류 코드 관찰] 여기 넘기는 bdev->rq 는 이 트리 어디에서도 값이 대입되지
	 * 않는 필드다. struct scm_blk_dev 를 scm_drv.c 가 0 으로 채워 할당하므로
	 * 언제나 NULL 이고, block/blk-mq.c 의 blk_mq_kick_requeue_list() 는 받은 큐의
	 * requeue_work 를 NULL 검사 없이 그대로 건드린다. 이 함수 자체가 두 오류
	 * 경로에서만 불리므로 정상 동작에서는 이 줄에 닿지 않는다.
	 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	blk_mq_kick_requeue_list(bdev->rq);
}

/* [한국어]
 * scm_request_finish - 묶여 있던 요청들을 한꺼번에 완료 처리한다
 *
 * @scmrq: 하드웨어 처리가 끝난 요청.
 *
 * 정상 완료의 마무리다. AOB 하나에 묶여 있던 blk-mq 요청들을 차례로 블록
 * 계층에 돌려준다.
 *
 * 요청마다 딸린 사적 공간에 결과를 적는 것이 이 함수의 요점이다. 그 공간은
 * 태그 집합의 cmd_size 를 sizeof(blk_status_t) 로 잡아 확보해 둔 것이고,
 * blk_mq_rq_to_pdu() 가 요청 바로 뒤의 그 자리를 돌려준다. 나중에
 * scm_blk_request_done() 이 같은 자리를 읽어 최종 완료에 쓴다. 결과를 요청에
 * 바로 실어 보내지 않고 이렇게 한 겹 두는 이유는, 완료가 인터럽트 문맥에서
 * 시작해 softirq 로 넘어가기 때문이다.
 *
 * 한 AOB 에 묶인 요청들은 결과를 공유한다는 점이 중요하다. 오류가 나면
 * 그 배치에 든 요청이 모두 같은 오류로 끝난다.
 *
 * fake timeout 검사가 끼어 있는 것은 블록 계층의 결함 주입 기능 때문이다.
 * 그 기능이 켜져 있고 이 큐가 대상이면 완료를 일부러 건너뛰어, 블록 계층의
 * 시간 초과 처리가 도는지 시험한다. 평소에는 언제나 거짓이므로 likely 로
 * 표시되어 있다.
 *
 * 실행 컨텍스트: 완료 인터럽트 문맥. blk_mq_complete_request() 는 이어지는
 * 처리를 softirq 나 다른 CPU 로 넘기므로, 여기서는 무거운 일을 하지 않는다.
 *
 * 에러 경로: 없다. 오류 자체는 scmrq->error 에 담겨 각 요청으로 전달된다.
 *
 * 호출 체인:
 *   scm_blk_irq() → [이 함수]
 *     → blk_mq_rq_to_pdu() → blk_mq_complete_request() → scm_request_done()
 */
static void scm_request_finish(struct scm_request *scmrq)
{
	/* [한국어] 떠 있는 요청 수를 내릴 장치를 꺼내 둔다. */
	struct scm_blk_dev *bdev = scmrq->bdev;
	/* [한국어] 요청마다 딸린 결과 자리를 가리킬 포인터. */
	blk_status_t *error;
	/* [한국어] 훑기용 첨자. */
	int i;

	/* [한국어] 묶여 있던 요청들을 차례로 훑는다. NULL 을 만나면 멈추므로, AOB 를 다
	 * 채우지 못한 채 내보낸 경우도 그대로 다룬다. */
	for (i = 0; i < nr_requests_per_io && scmrq->request[i]; i++) {
		/* [한국어] 요청 바로 뒤에 딸린 사적 공간을 가리킨다. 그 크기가 정확히 blk_status_t
		 * 하나인 것은 태그 집합의 cmd_size 를 그렇게 잡았기 때문이다. */
		error = blk_mq_rq_to_pdu(scmrq->request[i]);
		/* [한국어] 요청의 사적 공간에 결과를 적는다. 한 AOB 에 묶인 요청은 모두 같은 값을
		 * 받으므로, 오류가 나면 그 배치가 통째로 같은 오류로 끝난다. */
		*error = scmrq->error;
		/* [한국어] 블록 계층의 결함 주입 기능이 이 큐를 시간 초과 대상으로 골랐는지 본다.
		 * 골랐다면 완료를 일부러 건너뛰어 시간 초과 처리가 도는지 시험한다.
		 * include/linux/blk-mq.h 의 이 인라인 함수는 해당 설정이 꺼져 있으면 언제나
		 * 거짓이라, likely 로 평소 경로를 표시해 두었다. */
		if (likely(!blk_should_fake_timeout(scmrq->request[i]->q)))
			/* [한국어] 이 요청이 끝났음을 블록 계층에 알린다. block/blk-mq.c 의 이 함수는 완료를
			 * 처리할 CPU 를 정한 뒤 scm_mq_ops.complete, 즉 scm_blk_request_done() 을
			 * 부른다. 지금은 인터럽트 문맥이므로 무거운 마무리를 여기서 하지 않는다. */
			blk_mq_complete_request(scmrq->request[i]);
	}

	/* [한국어] 떠 있는 요청 수를 내린다. scm_request_start() 가 올린 것의 짝이다. */
	atomic_dec(&bdev->queued_reqs);
	/* [한국어] AIDAW 페이지를 회수하고 이 요청을 놀고 있는 목록에 돌려놓는다. */
	scm_request_done(scmrq);
}

/* [한국어]
 * scm_request_start - 다 조립한 AOB 를 하드웨어에 넘긴다
 *
 * @scmrq: 조립이 끝난 요청.
 *
 * 이 드라이버가 하드웨어로 나가는 유일한 문이다. 이 함수를 지나면 그 뒤의
 * 일은 완료 인터럽트가 이어받는다.
 *
 * 떠 있는 요청 수를 먼저 올리고 나서 넘기는 순서가 중요하다. 반대였다면
 * 아주 빠른 완료 인터럽트가 아직 올리지 않은 계수를 내리는 경우가 생긴다.
 *
 * 넘기기가 실패하는 경우는 쓸 하위 채널이 없을 때다. 그때는 짧은 기록을
 * 남기고 요청들을 블록 계층에 되돌린다 — 되돌리는 쪽에서 계수도 다시
 * 내리므로 짝이 맞는다.
 *
 * 성공하면 여기서 할 일이 끝난다. 반환값이 void 인 것은 호출자가 이 시점에
 * 할 수 있는 일이 없기 때문이다 — 실패해도 이미 요청을 블록 계층에
 * 되돌려 두었다.
 *
 * eadm_start_aob() 이 실제로 무엇을 하는지, 어떤 조건에서 실패하는지는
 * asm/eadm.h 소관이라 이 트리에서 확인 못 함. 기록 문구가 "no subchannel"
 * 인 것으로 보아 하위 채널 확보 실패가 주된 이유로 보인다.
 *
 * 실행 컨텍스트: queue_rq 경로, sq->lock 을 잡은 채. 프로세스 컨텍스트.
 *
 * 에러 경로: 넘기기 실패 시 scm_request_requeue(). 그 안에서 요청 자원까지
 * 모두 반납된다.
 *
 * 호출 체인:
 *   scm_blk_request() → [이 함수]
 *     → eadm_start_aob() → scm_request_requeue()
 */
static void scm_request_start(struct scm_request *scmrq)
{
	/* [한국어] 계수를 올릴 장치를 꺼내 둔다. */
	struct scm_blk_dev *bdev = scmrq->bdev;

	/* [한국어] 떠 있는 요청 수를 먼저 올린다. 넘긴 뒤에 올렸다면, 아주 빠른 완료
	 * 인터럽트가 아직 올리지 않은 계수를 먼저 내리는 경우가 생긴다.
	 * [상류 코드 관찰] 이 계수는 올리고 내리기만 할 뿐, 값을 읽어 보는 곳이
	 * 이 트리에 없다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	atomic_inc(&bdev->queued_reqs);
	/* [한국어] AOB 를 하드웨어에 넘긴다. 0 이 아니면 실패이며, 기록 문구로 보아 쓸 하위
	 * 채널이 없는 경우다. 이 함수의 규약은 asm/eadm.h 소관이라 이 트리에서
	 * 확인 못 함. */
	if (eadm_start_aob(scmrq->aob)) {
		/* [한국어] 하위 채널을 못 잡았음을 기록한다. 수준 5 라 평소 설정에서는 걸러진다. */
		SCM_LOG(5, "no subchannel");
		/* [한국어] 요청들을 블록 계층에 되돌린다. 그 안에서 위에서 올린 계수도 다시 내려가고
		 * 요청 자체도 풀로 돌아간다. */
		scm_request_requeue(scmrq);
	}
}

/* [한국어] 하드웨어 큐 하나가 조립 중인 상태를 담는 구조체.
 * 이 파일에서만 쓰는 형이라 헤더가 아니라 여기에 있다. blk-mq 의
 * hctx->driver_data 에 매달려 하드웨어 큐마다 하나씩 존재하며,
 * scm_blk_init_hctx() 가 만들고 scm_blk_exit_hctx() 가 푼다.
 * 요청 여럿을 한 AOB 에 모으는 이 드라이버의 방식이 이 구조체 하나로
 * 성립한다 — 모으는 자리와 그것을 지키는 잠금이 전부다. */
struct scm_queue {
	/* [한국어] 지금 이 하드웨어 큐가 조립 중인 요청. NULL 이면 아직 시작 전이다.
	 * 설정자: scm_blk_request() 가 풀에서 꺼내 넣고(:993), 하드웨어에 넘긴
	 * 뒤 NULL 로 되돌린다(:1004, :1012).
	 * 읽는 자: 같은 함수가 조립을 이어갈지 새로 시작할지 정할 때(:984).
	 * 값 범위: NULL 이거나 유효한 struct scm_request 포인터.
	 * 동기화: 아래 lock 이 지킨다. 이 자리가 있어야 요청 여럿을 한 AOB 에
	 * 모을 수 있고, 큐마다 따로 있어 큐끼리 경쟁하지 않는다. */
	struct scm_request *scmrq;
	/* [한국어] 이 자리를 지키는 스핀락.
	 * 설정자: scm_blk_init_hctx() 가 초기화한다.
	 * 읽는 자: scm_blk_request() 가 함수 전체를 이 잠금으로 감싼다.
	 * 값 범위: 스핀락.
	 * 동기화: 지키는 것은 바로 위 scmrq 필드 하나다. 인터럽트를 끄지 않는
	 * 판으로 잡는데, 완료 인터럽트 경로가 이 잠금을 건드리지 않기 때문이다.
	 * 하드웨어 큐마다 따로 있어 큐끼리는 서로를 기다리지 않는다. */
	spinlock_t lock;
};

/* [한국어]
 * scm_blk_request - blk-mq 의 queue_rq 콜백. 요청 하나를 받아 AOB 에 쌓는다
 *
 * @hctx: 이 요청이 배정된 하드웨어 큐. 여기에 이 드라이버가 매달아 둔
 *        struct scm_queue 가 driver_data 로 들어 있다.
 * @qd: 블록 계층이 넘기는 요청과 배치 정보. include/linux/blk-mq.h 의
 *      struct blk_mq_queue_data 이며, rq 와 last 두 필드뿐이다.
 * @return: BLK_STS_OK = 받았다, BLK_STS_RESOURCE = 지금은 못 받겠다.
 *
 * 이 파일의 정문이다. 블록 계층에서 내려오는 모든 I/O 가 여기로 들어온다.
 *
 * 이 드라이버의 특징은 요청을 하나씩 하드웨어에 넘기지 않고 여럿을 한 AOB
 * 에 모아 넘긴다는 점이다. 그 모으는 상태가 하드웨어 큐마다 하나씩 있는
 * sq->scmrq 이고, 이 함수는 그 조립을 한 걸음씩 진행시킨다.
 *
 * 진행은 다섯 단계다.
 *  1. 쓰기 금지 상태를 확인한다. 걸리면 아무것도 하지 않고 자원 부족으로
 *     돌려보낸다.
 *  2. 조립 중인 요청이 없으면 풀에서 하나 꺼내 초기화한다. 풀이 비어 있으면
 *     역시 자원 부족으로 돌려보낸다.
 *  3. 요청을 현재 칸에 꽂고 MSB 와 AIDAW 로 번역한다.
 *  4. 번역이 성공하면 블록 계층에 이 요청의 처리를 시작했다고 알린다. 이
 *     호출이 있어야 시간 초과 감시가 걸린다.
 *  5. 이번이 배치의 마지막이거나 AOB 가 꽉 찼으면 하드웨어에 넘긴다.
 *
 * 3 이 실패했을 때의 처리가 이 함수에서 가장 조심스러운 대목이다. 방금 꽂은
 * 요청을 지우고, 그때까지 모아 둔 것이 하나라도 있으면 그것만이라도 하드웨어에
 * 내보낸 뒤, 조립 상태를 비우고 자원 부족을 돌려준다. 이 요청 자신은
 * blk_mq_start_request() 를 거치지 않았으므로 블록 계층이 그대로 다시
 * 내려보낸다.
 *
 * BLK_STS_RESOURCE 를 돌려주면 블록 계층이 어떻게 하는지는 block/blk-mq.c
 * 에서 확인된다. 그 요청을 목록 앞으로 되돌리고 드라이버 태그를 반납한 뒤,
 * 잠시 뒤 큐를 다시 돌린다. 즉 이 반환값은 실패가 아니라 나중에 다시 달라는
 * 뜻이다.
 *
 * [상류 코드 관찰] 3 의 실패 갈래에서 모아 둔 것이 하나도 없으면
 * (msb_count 가 0), 조립 중이던 요청은 하드웨어로도 가지 않고 놀고 있는
 * 목록으로도 돌아가지 않은 채 sq->scmrq 에서만 떨어져 나간다. 그 요청을
 * 가리키던 곳이 sq->scmrq 뿐이었으므로 풀에서 하나가 줄어든다. 그 갈래는
 * 방금 꺼낸 요청의 첫 번역에서 AIDAW 페이지를 못 얻었을 때만 성립한다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * [상류 코드 관찰] 이 함수는 qd->last 를 보고 하드웨어에 넘길 시점을
 * 정하는데, 아래 scm_mq_ops 표에는 commit_rqs 항목이 없다.
 * include/linux/blk-mq.h 의 struct blk_mq_ops 설명은 bd->last 로 제출 시점을
 * 판단하는 드라이버는 그 콜백을 두어야 한다고 적고 있다. 원본(1f0e418bb6)에서
 * 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트이며 인터럽트가 켜져 있다. sq->lock 을
 * 인터럽트를 끄지 않는 판으로 잡는 것이 그 근거인데, 완료 인터럽트가 그
 * 잠금을 건드리지 않기 때문에 가능한 선택이다. 잠들면 안 되는 자리라 안쪽의
 * 할당은 모두 잠들지 않는 표시를 쓴다.
 *
 * 에러 경로: 세 갈래 모두 BLK_STS_RESOURCE 다 — 쓰기 금지, 풀 고갈, AIDAW
 * 고갈. 어느 쪽도 요청을 잃지 않고 블록 계층에 되돌린다.
 *
 * 호출 체인:
 *   블록 계층의 요청 배분 경로 → scm_mq_ops.queue_rq == [이 함수]
 *     → scm_permit_request() → scm_request_fetch() → scm_request_init()
 *     → scm_request_set() → scm_request_prepare() → blk_mq_start_request()
 *     → scm_request_start()
 */
static blk_status_t scm_blk_request(struct blk_mq_hw_ctx *hctx,
			   const struct blk_mq_queue_data *qd)
{
	/* [한국어] 큐의 queuedata 에서 SCM 장치를 되찾는다. scm_blk_dev_setup() 이
	 * blk_mq_alloc_disk() 의 세 번째 인자로 넣어 둔 것이다. */
	struct scm_device *scmdev = hctx->queue->queuedata;
	/* [한국어] SCM 장치에서 이 드라이버의 장치 상태를 되찾는다. scm_drv.c 의 probe 가
	 * drvdata 에 매달아 둔 값이다. */
	struct scm_blk_dev *bdev = dev_get_drvdata(&scmdev->dev);
	/* [한국어] 이 하드웨어 큐에 매달아 둔 조립 상태. scm_blk_init_hctx() 가 붙여 둔 것이다. */
	struct scm_queue *sq = hctx->driver_data;
	/* [한국어] 이번에 처리할 blk-mq 요청. */
	struct request *req = qd->rq;
	/* [한국어] 이 하드웨어 큐가 조립 중인 요청. 아래에서 채운다. */
	struct scm_request *scmrq;

	/* [한국어] 이 하드웨어 큐의 조립 상태를 지키는 잠금을 잡는다. 인터럽트를 끄지 않는
	 * 판인 것은 완료 인터럽트가 이 잠금을 건드리지 않기 때문이다 — 완료 경로가
	 * 쓰는 것은 list_lock 과 bdev->lock 이지 이 잠금이 아니다. */
	spin_lock(&sq->lock);
	/* [한국어] 쓰기 금지 상태에서 들어온 쓰기인지 본다. 걸리면 아무것도 하지 않는다. */
	if (!scm_permit_request(bdev, req)) {
		/* [한국어] 잠금을 놓고 물러난다. */
		spin_unlock(&sq->lock);
		/* [한국어] 지금은 못 받겠다고 알린다. block/blk-mq.c 의 dispatch 는 이 값을 받으면
		 * 요청을 목록 앞으로 되돌리고 드라이버 태그를 반납한 뒤 잠시 뒤 큐를 다시
		 * 돌린다. 즉 실패가 아니라 나중에 다시 달라는 뜻이다. */
		return BLK_STS_RESOURCE;
	}

	/* [한국어] 이 하드웨어 큐가 조립 중이던 요청을 꺼낸다. NULL 이면 아직 시작 전이다. */
	scmrq = sq->scmrq;
	/* [한국어] 아직 조립을 시작하지 않았다면 새로 시작해야 한다. */
	if (!scmrq) {
		/* [한국어] 놀고 있는 요청을 하나 꺼내 온다. 새로 만들지 않고 미리 만들어 둔 것을 쓴다. */
		scmrq = scm_request_fetch();
		/* [한국어] 풀이 비었다. 여기서 새로 할당할 수는 없다 — 잠금을 잡은 채라 잠들 수 없다. */
		if (!scmrq) {
			/* [한국어] 풀이 비었음을 기록한다. 수준 5 라 평소 설정에서는 걸러진다. */
			SCM_LOG(5, "no request");
			/* [한국어] 잠금을 놓는다. */
			spin_unlock(&sq->lock);
			/* [한국어] 자원 부족을 알린다. 블록 계층이 잠시 뒤 다시 내려보낸다. */
			return BLK_STS_RESOURCE;
		}
		/* [한국어] 앞서 쓰던 내용을 지우고 새 AOB 조립을 시작할 수 있게 만든다. */
		scm_request_init(bdev, scmrq);
		/* [한국어] 이 하드웨어 큐의 조립 자리에 매단다. 다음 요청부터는 이것을 이어 쓴다. */
		sq->scmrq = scmrq;
	}
	/* [한국어] 요청을 AOB 의 현재 칸에 꽂는다. 칸 번호는 AOB 의 msb_count 이며,
	 * 아래 번역이 그 값을 커서로 삼는다. */
	scm_request_set(scmrq, req);

	/* [한국어] 요청을 MSB 한 칸과 AIDAW 목록으로 번역한다. 실패는 AIDAW 자리를 못 얻은
	 * 경우뿐이다. */
	if (scm_request_prepare(scmrq)) {
		/* [한국어] AIDAW 를 못 얻어 번역이 실패했다. */
		SCM_LOG(5, "aidaw alloc failed");
		/* [한국어] 방금 꽂은 요청을 지운다. 배열을 훑는 쪽이 NULL 에서 멈추므로, 준비되지
		 * 않은 이 요청이 완료 처리되는 일이 없다. msb_count 가 올라가지 않았기
		 * 때문에 같은 칸을 다시 가리킨다. */
		scm_request_set(scmrq, NULL);

		/* [한국어] 이미 채워 둔 MSB 가 하나라도 있는지 본다. 방금 실패한 번역은
		 * msb_count 를 올리기 전에 물러났으므로, 이 값은 앞서 성공한 요청 수를
		 * 그대로 나타낸다. */
		if (scmrq->aob->request.msb_count)
			/* [한국어] 그때까지 모아 둔 것만이라도 하드웨어에 넘긴다. 이 요청은 빠진 채로 나간다. */
			scm_request_start(scmrq);

		/* [한국어] 조립 자리를 비운다.
		 * [상류 코드 관찰] 바로 위 확인이 거짓이었다면, 즉 모아 둔 MSB 가 하나도
		 * 없었다면, 이 요청은 하드웨어로도 가지 않고 놀고 있는 목록으로도 돌아가지
		 * 못한 채 여기서 sq 와의 연결이 끊긴다. 그 요청을 가리키던 곳이 sq->scmrq
		 * 하나뿐이라 풀에서 하나가 줄어든다. 방금 꺼낸 요청의 첫 번역에서 AIDAW
		 * 페이지를 못 얻었을 때만 성립하는 갈래다. 원본(1f0e418bb6)에서 확인했으며
		 * 코드는 고치지 않았다. */
		sq->scmrq = NULL;
		/* [한국어] 잠금을 놓는다. */
		spin_unlock(&sq->lock);
		/* [한국어] 자원 부족을 알린다. 이 요청은 아직 시작되지 않았으므로 잃지 않는다. */
		return BLK_STS_RESOURCE;
	}
	/* [한국어] 블록 계층에 이 요청의 처리를 시작했다고 알린다. 이 호출이 있어야 시간 초과
	 * 감시가 걸리고 통계가 시작된다. 위의 실패 갈래에서는 이 줄을 지나지 않으므로,
	 * 그 요청은 시작되지 않은 것으로 남아 블록 계층이 그대로 다시 내려보낸다. */
	blk_mq_start_request(req);

	/* [한국어] 내보낼 때가 됐는지 본다. 두 조건 중 하나면 된다 — 블록 계층이 이번이
	 * 배치의 마지막이라고 알렸거나(qd->last), AOB 의 MSB 칸이 다 찼거나.
	 * qd->last 의 뜻은 include/linux/blk-mq.h 의 struct blk_mq_queue_data 에서
	 * 확인된다: 이 요청 뒤로 내려보낼 것이 남지 않았다는 표시다. */
	if (qd->last || scmrq->aob->request.msb_count == nr_requests_per_io) {
		/* [한국어] 모아 둔 AOB 를 하드웨어에 넘긴다. */
		scm_request_start(scmrq);
		/* [한국어] 조립 자리를 비운다. 다음 요청은 풀에서 새 scm_request 를 꺼내 시작한다. */
		sq->scmrq = NULL;
	}
	/* [한국어] 잠금을 놓는다. 여기부터 sq->scmrq 는 다른 흐름이 건드릴 수 있다. */
	spin_unlock(&sq->lock);
	/* [한국어] 요청을 받았다. 블록 계층은 이 요청의 완료를 기다린다 — 아직 하드웨어에
	 * 넘기지 않았더라도 sq->scmrq 에 쌓여 있으니 언젠가 나간다. */
	return BLK_STS_OK;
}

/* [한국어]
 * scm_blk_init_hctx - 하드웨어 큐 하나에 이 드라이버의 조립 상태를 매단다
 *
 * @hctx: 블록 계층이 막 세운 하드웨어 큐.
 * @data: 태그 집합을 만들 때 넘긴 드라이버 자료. 이 드라이버는 쓰지 않는다.
 * @idx: 이 하드웨어 큐의 번호. 이 드라이버는 쓰지 않는다.
 * @return: 0 = 성공, -ENOMEM = 할당 실패.
 *
 * blk-mq 는 하드웨어 큐를 세울 때마다 이 콜백을 불러 드라이버가 큐마다
 * 따로 둘 상태를 만들 기회를 준다. 이 드라이버가 큐마다 두는 것은 조립 중인
 * scm_request 한 자리와 그것을 지키는 잠금뿐이다.
 *
 * 큐마다 따로 두기 때문에 서로 다른 하드웨어 큐가 동시에 각자의 AOB 를
 * 조립할 수 있다. 그래서 잠금의 범위도 큐 하나로 좁아진다.
 *
 * 0 으로 채워 할당하므로 scmrq 는 NULL 에서 시작한다. 그 NULL 이 "아직
 * 조립을 시작하지 않았다" 는 표시로 쓰인다.
 *
 * 인자 셋 중 뒤의 둘을 쓰지 않는 것은 콜백 규약을 맞추기 위한 것이다.
 * 그 규약은 include/linux/blk-mq.h 의 struct blk_mq_ops 에서 확인된다.
 *
 * 실행 컨텍스트: 큐를 세우는 동안, 즉 blk_mq_alloc_disk() 안쪽이다.
 * 프로세스 컨텍스트이며 GFP_KERNEL 로 잠들 수 있다.
 *
 * 에러 경로: 할당 실패 시 -ENOMEM. 블록 계층이 큐 만들기를 실패시키고,
 * 그것이 scm_blk_dev_setup() 의 오류로 이어진다.
 *
 * 호출 체인:
 *   blk_mq_alloc_disk() → scm_mq_ops.init_hctx == [이 함수]
 *     → kzalloc_obj() → spin_lock_init()
 */
static int scm_blk_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
			     unsigned int idx)
{
	/* [한국어] 큐별 조립 상태를 0 으로 채워 잡는다. 그 0 덕에 scmrq 가 NULL 로 시작하고,
	 * 그 NULL 이 "아직 조립을 시작하지 않았다" 는 표시로 쓰인다. */
	struct scm_queue *qd = kzalloc_obj(*qd);

	/* [한국어] 할당 실패. */
	if (!qd)
		/* [한국어] 블록 계층이 큐 만들기를 실패시키고, 그것이 scm_blk_dev_setup() 의 오류가 된다. */
		return -ENOMEM;

	/* [한국어] 조립 중인 요청을 지킬 잠금을 초기화한다. 큐마다 따로 있어 서로 다른
	 * 하드웨어 큐가 동시에 각자의 AOB 를 조립할 수 있다. */
	spin_lock_init(&qd->lock);
	/* [한국어] 만든 상태를 이 하드웨어 큐에 매단다. 이 줄 뒤로 scm_blk_request() 가
	 * hctx->driver_data 로 이것을 되찾는다. */
	hctx->driver_data = qd;

	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * scm_blk_exit_hctx - 하드웨어 큐에 매달아 둔 조립 상태를 푼다
 *
 * @hctx: 정리되는 하드웨어 큐.
 * @idx: 하드웨어 큐 번호. 이 드라이버는 쓰지 않는다.
 *
 * scm_blk_init_hctx() 의 짝이다.
 *
 * 푸는 것보다 그 앞의 확인이 더 중요하다. 조립 중인 요청이 남아 있으면
 * 경고를 찍는데, 이 시점에 남아 있다는 것은 어딘가에서 요청을 하드웨어에도
 * 넘기지 않고 풀에도 돌려주지 않은 채 큐가 내려갔다는 뜻이기 때문이다.
 * 경고만 찍고 그 요청을 회수하지는 않는다.
 *
 * 푼 뒤 포인터를 NULL 로 되돌린다. 이미 해제된 곳을 가리키는 포인터를 큐에
 * 남기지 않으려는 배치다.
 *
 * 실행 컨텍스트: 큐를 내리는 동안, 즉 blk_mq_free_tag_set() 이나 디스크
 * 정리 경로 안쪽이다. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환형이 void 다.
 *
 * 호출 체인:
 *   blk_mq_free_tag_set() → scm_mq_ops.exit_hctx == [이 함수] → kfree()
 */
static void scm_blk_exit_hctx(struct blk_mq_hw_ctx *hctx, unsigned int idx)
{
	/* [한국어] 이 큐에 매달아 둔 조립 상태를 꺼낸다. */
	struct scm_queue *qd = hctx->driver_data;

	/* [한국어] 조립 중인 요청이 남아 있으면 경고한다. 이 시점에 남아 있다는 것은 어딘가
	 * 요청을 하드웨어에도 넘기지 않고 풀에도 돌려주지 않은 채 큐가 내려갔다는
	 * 뜻이다. 경고만 찍고 그 요청을 회수하지는 않는다. */
	WARN_ON(qd->scmrq);
	/* [한국어] 큐별 조립 상태를 푼다. */
	kfree(hctx->driver_data);
	/* [한국어] 이미 푼 곳을 가리키는 포인터를 큐에 남기지 않는다. */
	hctx->driver_data = NULL;
}

/* [한국어]
 * __scmrq_log_error - 실패한 요청의 정황을 기록에 남긴다
 *
 * @scmrq: 오류로 끝난 요청. error 필드에 하드웨어가 준 결과가 이미 들어 있다.
 *
 * 완료 인터럽트가 오류를 보았을 때 가장 먼저 부르는 함수다. 기록만 남기고
 * 어떤 결정도 하지 않는다 — 재시도할지 말지는 부르는 쪽이 정한다.
 *
 * 두 갈래로 나눠 기록한다.
 *   - 시간 초과면 짧은 문구 하나만 남긴다. 이 경우 응답 블록에 쓸 만한
 *     내용이 없기 때문이다.
 *   - 그 밖의 오류면 문구와 함께 응답 블록을 통째로 이진 기록으로 남긴다.
 *     나중에 문제를 되짚을 때 하드웨어가 무엇이라 답했는지 알 수 있다.
 *
 * 그다음 남은 재시도 횟수를 보고, 아직 남았으면 다시 시도한다는 기록만
 * 남기고 끝난다. 다 썼을 때만 커널 로그에 오류를 찍는다. 재시도로 넘어갈
 * 오류까지 커널 로그를 채우지 않으려는 배치다.
 *
 * 여기서 보는 retries 는 아직 줄이기 전의 값이다. 부르는 쪽이 이 함수를
 * 먼저 부르고 그다음에 줄이기 때문인데, 덕분에 "마지막 시도였다" 를
 * 0 으로 판정할 수 있다.
 *
 * [상류 코드 관찰] 마지막 줄이 blk_status_t 값을 rc 라는 이름으로 %d 에
 * 넣어 찍는다. 그 값은 errno 가 아니라 블록 계층의 상태 코드이므로, 로그에
 * 나오는 숫자를 errno 로 읽으면 안 된다. blk_status_t 의 정의는 이 트리에
 * include/linux/blk_types.h 가 없어 확인 못 함. 원본(1f0e418bb6)에서
 * 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 완료 인터럽트 문맥. 그래서 기록은 s390 debug 기능과
 * pr_err 로만 남기고 잠들 수 있는 일은 하지 않는다.
 *
 * 에러 경로: 없다. 반환형이 void 다.
 *
 * 호출 체인:
 *   scm_blk_irq() → [이 함수] → SCM_LOG_HEX() → pr_err()
 */
static void __scmrq_log_error(struct scm_request *scmrq)
{
	/* [한국어] 응답 블록을 꺼내기 위해 AOB 를 잡아 둔다. */
	struct aob *aob = scmrq->aob;

	/* [한국어] 시간 초과와 그 밖의 오류를 가른다. */
	if (scmrq->error == BLK_STS_TIMEOUT)
		/* [한국어] 시간 초과면 문구 하나로 끝낸다. 이 경우 응답 블록에 쓸 만한 내용이 없다. */
		SCM_LOG(1, "Request timeout");
	else {
		/* [한국어] 그 밖의 오류. 짧은 문구를 남긴다. */
		SCM_LOG(1, "Request error");
		/* [한국어] 응답 블록을 통째로 이진 기록에 남긴다. 나중에 하드웨어가 무엇이라
		 * 답했는지 되짚을 수 있다. sizeof 로 길이를 구하므로 블록 구조가 바뀌어도
		 * 이 줄은 그대로다. */
		SCM_LOG_HEX(1, &aob->response, sizeof(aob->response));
	}
	/* [한국어] 남은 재시도 횟수를 본다. 부르는 쪽이 아직 깎기 전에 이 함수를 불렀으므로,
	 * 0 이라는 것은 이번이 마지막 시도였다는 뜻이다. */
	if (scmrq->retries)
		/* [한국어] 아직 재시도가 남았다는 것만 기록한다. 커널 로그에는 남기지 않는다. */
		SCM_LOG(1, "Retry request");
	else
		/* [한국어] 재시도를 다 썼으므로 이제야 커널 로그에 오류를 남긴다. 재시도로 넘어갈
		 * 오류까지 커널 로그를 채우지 않으려는 배치다. */
		pr_err("An I/O operation to SCM failed with rc=%d\n",
		       /* [한국어] blk_status_t 값을 그대로 찍는다.
		        * [상류 코드 관찰] 이름은 rc 지만 errno 가 아니라 블록 계층의 상태 코드다.
		        * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
		       scmrq->error);
}

/* [한국어]
 * scm_blk_handle_error - 오류 난 요청을 다시 넣을지, 블록 계층에 되돌릴지 정한다
 *
 * @scmrq: 오류로 끝났고 아직 재시도 횟수가 남아 있는 요청.
 *
 * 재시도 판정의 본체다. 부르는 쪽이 이미 "재시도할 횟수는 남았다" 까지
 * 확인했으므로, 여기서는 "그대로 다시 넣어도 되는 오류인가" 만 가린다.
 *
 * 두 갈래다.
 *   - 입출력 오류가 아니면(시간 초과 등) 응답 블록을 볼 것 없이 곧바로
 *     다시 넣는다.
 *   - 입출력 오류면 응답 블록의 오류 부호를 본다. 쓰기 금지라면 다시 넣어도
 *     같은 결과일 것이므로, 장치 상태를 쓰기 금지로 내리고 요청을 블록
 *     계층에 되돌린다. 그 밖의 부호는 갈래를 빠져나가 다시 넣는 쪽으로 간다.
 *
 * 상류 영어 주석이 밝히듯, 응답 블록의 내용이 의미가 있는 것은 입출력 오류일
 * 때뿐이다. 그래서 그 확인이 switch 앞에 있다.
 *
 * 쓰기 금지 갈래에서 상태를 바꾸기 전에 이미 그 상태였는지 보는 것은 로그
 * 때문이다. 요청이 여러 개 연달아 같은 응답을 받아도 관리자에게 보이는 줄은
 * 한 번만 나온다.
 *
 * 다시 넣기가 성공하면 그대로 물러난다. 그 요청의 완료는 나중에 새 인터럽트로
 * 다시 들어오고, 그때 재시도 횟수가 하나 더 줄어 있다. 실패하면 아래로
 * 흘러 요청을 되돌린다 — restart 라벨과 requeue 라벨이 이어져 있는 배치가
 * 그 흐름을 만든다.
 *
 * 오류 부호와 그 값들의 뜻은 asm/eadm.h 소관이라 이 트리에서 확인 못 함.
 *
 * 실행 컨텍스트: 완료 인터럽트 문맥. 그래서 장치 잠금을 irqsave 판으로
 * 잡는다 — 이 자리에서는 이미 인터럽트가 꺼져 있을 수 있으므로 원래 상태를
 * 저장했다가 그대로 되돌려야 한다.
 *
 * 에러 경로: 다시 넣기 실패와 쓰기 금지, 둘 다 scm_request_requeue() 로
 * 모인다.
 *
 * 호출 체인:
 *   scm_blk_irq() → [이 함수]
 *     → eadm_start_aob() → scm_request_requeue()
 */
static void scm_blk_handle_error(struct scm_request *scmrq)
{
	struct scm_blk_dev *bdev = scmrq->bdev;
	/* [한국어] 인터럽트 상태를 저장해 둘 자리. 아래 잠금이 irqsave 판이라 필요하다. */
	unsigned long flags;

	/* [한국어] 입출력 오류가 아니면(시간 초과 등) 응답 블록에 볼 것이 없다. */
	if (scmrq->error != BLK_STS_IOERR)
		/* [한국어] 응답 블록을 볼 것 없이 그대로 다시 넣는다. */
		goto restart;

	/* [한국어] 아래 줄의 상류 영어 주석 — 응답 블록을 믿고 읽을 수 있는 것은 입출력
	 * 오류일 때뿐이라는 뜻이며, 바로 위의 확인이 그 근거다. */
	/* For -EIO the response block is valid. */
	/* [한국어] 응답 블록의 오류 부호로 갈린다. */
	switch (scmrq->aob->response.eqc) {
	/* [한국어] 쓰기 금지 응답. 이 증분에 지금은 쓸 수 없다는 뜻이다.
	 * 부호의 실제 값은 asm/eadm.h 소관이라 이 트리에서 확인 못 함. */
	case EQC_WR_PROHIBIT:
		/* [한국어] 장치 잠금을 잡는다. 이 자리는 인터럽트 문맥이라 이미 인터럽트가 꺼져
		 * 있을 수 있으므로, 원래 상태를 저장했다가 그대로 되돌리는 irqsave 판을 쓴다. */
		spin_lock_irqsave(&bdev->lock, flags);
		/* [한국어] 이미 쓰기 금지 상태였는지 본다. */
		if (bdev->state != SCM_WR_PROHIBIT)
			/* [한국어] 관리자에게 쓰기가 막혔음을 알린다. 아래에서 어차피 덮으므로 이 확인은
			 * 오직 로그를 한 번만 찍기 위한 것이다 — 요청이 연달아 같은 응답을 받아도
			 * 줄이 한 번만 나온다. */
			pr_info("%lx: Write access to the SCM increment is suspended\n",
				/* [한국어] 어느 증분인지 장치 주소로 알린다. */
				(unsigned long) bdev->scmdev->address);
		/* [한국어] 상태를 쓰기 금지로 내린다. 이 뒤로 scm_permit_request() 가 쓰기를 막는다. */
		bdev->state = SCM_WR_PROHIBIT;
		/* [한국어] 잠금을 놓고 인터럽트 상태를 원래대로 돌린다. */
		spin_unlock_irqrestore(&bdev->lock, flags);
		/* [한국어] 다시 넣어도 같은 결과일 것이므로 요청을 블록 계층에 되돌린다. */
		goto requeue;
	/* [한국어] 쓰기 금지가 아닌 나머지 전부. */
	default:
		/* [한국어] 그 밖의 오류 부호는 특별히 다룰 것이 없어 switch 를 빠져나간다.
		 * 빠져나가면 아래 restart 라벨로 흘러 다시 넣기를 시도한다. */
		break;
	}

/* [한국어] 다시 넣기 라벨. 입출력 오류가 아니거나, 응답 부호가 쓰기 금지가 아닐 때 온다. */
restart:
	/* [한국어] 같은 AOB 를 그대로 다시 하드웨어에 넣는다. 0 이면 성공이다. */
	if (!eadm_start_aob(scmrq->aob))
		/* [한국어] 다시 넣기 성공. 그 요청의 완료는 새 인터럽트로 다시 들어온다. */
		return;

/* [한국어] 되돌리기 라벨. 위 갈래에서 뛰어 들어오기도 하고, 바로 위 줄에서 흘러 들어오기도 한다. */
requeue:
	/* [한국어] 요청들을 블록 계층에 되돌린다. 다시 넣기가 실패했거나 쓰기 금지인 경우다.
	 * 사용자에게는 오류가 보이지 않고, 나중에 블록 계층이 다시 내려보낸다. */
	scm_request_requeue(scmrq);
}

/* [한국어]
 * scm_blk_irq - 완료 인터럽트 진입점. 재시도와 완료를 가른다
 *
 * @scmdev: 완료를 낸 SCM 장치. 이 함수는 쓰지 않는다.
 * @data: EADM 계층이 돌려주는 표식. scm_request_init() 이 AOB 에 심어 둔
 *        값을 거쳐 이 요청으로 되돌아온 것이다.
 * @error: 하드웨어 처리 결과. 0 이면 정상, 아니면 블록 계층의 오류 코드다.
 *
 * 이 드라이버가 하드웨어에서 무언가를 되받는 유일한 자리다. scm_drv.c 의
 * 드라이버 표에 handler 로 걸려 있어, EADM 계층이 완료 때마다 직접 부른다 —
 * 중간에 결합부를 한 겹도 두지 않은 것은 이 경로가 성능 경로이기 때문이다.
 *
 * 먼저 결과를 요청에 새긴다. 이 값이 뒤이어 묶인 blk-mq 요청들에 그대로
 * 복사되므로, 한 AOB 에 든 요청은 결과를 공유한다.
 *
 * 정상이면 곧바로 완료 처리로 간다. 오류면 기록을 남기고 재시도 여부를
 * 가리는데, 그 판정이 이 함수에서 가장 눈여겨볼 줄이다. 후위 감소를 비교에
 * 그대로 써서 "남았는지 보기" 와 "하나 깎기" 를 한 번에 한다. 그래서 남아
 * 있으면 깎은 채로 재시도로 가고, 다 썼으면 그대로 완료(=실패) 처리로 간다.
 *
 * [상류 코드 관찰] 후위 감소이므로 값이 0 일 때도 깎기가 일어나, u8 필드가
 * 255 로 넘어간다. 다만 그 뒤 이 요청은 scm_request_done() 을 거쳐 풀로
 * 돌아가고, 다음에 꺼내 쓸 때 scm_request_init() 이 4 로 다시 채우므로
 * 그 값이 쓰이는 일은 없다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지
 * 않았다.
 *
 * 재시도로 갈 때 여기서 곧바로 물러나는 것이 중요하다. 그 요청은 아직
 * 살아 있고, 나중에 새 완료 인터럽트로 다시 이 함수에 들어온다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 이 함수 아래로 이어지는 모든 경로가 잠들지
 * 않아야 한다는 제약이 여기서 나온다.
 *
 * 에러 경로: 오류는 두 갈래로 나뉜다 — 재시도가 남았으면
 * scm_blk_handle_error(), 다 썼으면 scm_request_finish() 가 각 요청에 오류를
 * 실어 완료시킨다.
 *
 * 호출 체인:
 *   EADM 하위 채널 → scm_driver.handler == [이 함수]
 *     → __scmrq_log_error() → scm_blk_handle_error() / scm_request_finish()
 */
void scm_blk_irq(struct scm_device *scmdev, void *data, blk_status_t error)
{
	/* [한국어] EADM 계층이 돌려준 표식이 곧 이 요청이다. scm_request_init() 이 AOB 의
	 * data 자리에 aob_rq_header 주소를 심어 둔 것이 여기까지 되돌아온 것이며,
	 * 그 중간 과정은 asm/eadm.h 소관이라 이 트리에서 확인 못 함. */
	struct scm_request *scmrq = data;

	/* [한국어] 하드웨어가 준 결과를 요청에 새긴다. 이 값이 아래에서 묶인 blk-mq 요청마다
	 * 복사되므로, 한 AOB 에 든 요청은 결과를 공유하게 된다. */
	scmrq->error = error;
	/* [한국어] 0 이 아니면 오류다. blk_status_t 는 정상이 0 이라 이렇게 쓸 수 있다. */
	if (error) {
		/* [한국어] 실패한 정황을 기록에 남긴다. 여기서 보는 재시도 횟수는 아직 깎기 전의 값이다. */
		__scmrq_log_error(scmrq);
		/* [한국어] 후위 감소를 비교에 그대로 써서 "남았는지 보기" 와 "하나 깎기" 를 한 번에 한다.
		 * [상류 코드 관찰] 후위 감소라 값이 0 일 때도 깎여 u8 필드가 255 가 된다.
		 * 다만 그 뒤 이 요청은 풀로 돌아가고 다음에 꺼내 쓸 때 4 로 다시 채워지므로
		 * 그 값이 쓰이지 않는다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
		if (scmrq->retries-- > 0) {
			/* [한국어] 다시 넣을 수 있는 오류인지 가려, 다시 넣거나 블록 계층에 되돌린다. */
			scm_blk_handle_error(scmrq);
			/* [한국어] 재시도를 걸어 두었으므로 여기서 물러난다. 이 요청은 아직 살아 있고,
			 * 나중에 새 완료 인터럽트로 다시 이 함수에 들어온다. */
			return;
		}
	}

	/* [한국어] 정상이거나, 오류지만 재시도를 다 쓴 경우다. 묶여 있던 blk-mq 요청들을
	 * 결과와 함께 블록 계층에 돌려주고 이 요청을 풀에 반납한다. */
	scm_request_finish(scmrq);
}

/* [한국어]
 * scm_blk_request_done - blk-mq 의 complete 콜백. 요청 하나를 최종 완료시킨다
 *
 * @req: 완료시킬 blk-mq 요청.
 *
 * 완료의 마지막 단계다. scm_request_finish() 가 요청마다 딸린 사적 공간에
 * 적어 둔 결과를 여기서 꺼내 블록 계층에 넘긴다.
 *
 * 두 줄뿐이지만 blk-mq 의 완료 규약을 그대로 보여 준다. 드라이버는 인터럽트
 * 문맥에서 blk_mq_complete_request() 로 "이 요청 끝났다" 고만 알리고,
 * 블록 계층이 적절한 CPU 의 softirq 로 옮겨 이 콜백을 부른다. 무거운 마무리를
 * 인터럽트 문맥 밖으로 빼는 구조다.
 *
 * 사적 공간은 태그 집합의 cmd_size 로 확보한 것이고, 그 크기가 정확히
 * blk_status_t 하나다. 이 드라이버가 요청마다 기억해야 할 것이 결과 하나뿐이기
 * 때문이다.
 *
 * blk_mq_end_request() 가 요청의 모든 바이트를 완료 처리하고 태그를 반납한다.
 * 이 줄을 지나면 그 요청은 더 이상 유효하지 않다.
 *
 * 실행 컨텍스트: softirq. block/blk-mq.c 의 blk_mq_complete_request() 가
 * 완료 CPU 를 정한 뒤 이 콜백을 부른다.
 *
 * 에러 경로: 없다. 오류 자체는 넘기는 값에 담겨 있다.
 *
 * 호출 체인:
 *   scm_request_finish() → blk_mq_complete_request() →
 *   scm_mq_ops.complete == [이 함수] → blk_mq_end_request()
 */
static void scm_blk_request_done(struct request *req)
{
	/* [한국어] 요청 바로 뒤의 사적 공간을 가리킨다. scm_request_finish() 가 인터럽트
	 * 문맥에서 적어 둔 결과가 거기 들어 있다. */
	blk_status_t *error = blk_mq_rq_to_pdu(req);

	/* [한국어] 요청의 모든 바이트를 완료 처리하고 태그를 반납한다. 이 줄을 지나면 그
	 * 요청은 더 이상 유효하지 않으므로 뒤에서 건드리면 안 된다. */
	blk_mq_end_request(req, *error);
}

/* [한국어] 장치 파일 연산 표.
 * [상류 코드 관찰] open/release/ioctl 이 하나도 없다. 이 장치에는 특별한
 * 제어 연산이 없어 블록 계층의 기본 동작만으로 충분하다는 뜻이다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
static const struct block_device_operations scm_blk_devops = {
	/* [한국어] 소유 모듈만 밝힌다. 장치가 열려 있는 동안 모듈이 내려가지 않게 하는 표시다. */
	.owner = THIS_MODULE,
/* [한국어] 표 끝. */
};

/* [한국어] blk-mq 콜백 표. 이 드라이버가 블록 계층에 보이는 얼굴이며, 네 항목뿐이다.
 * const 로 두어 읽기 전용 구역에 놓이게 한다. */
static const struct blk_mq_ops scm_mq_ops = {
	/* [한국어] 요청을 받을 함수. 이 파일의 정문이다. */
	.queue_rq = scm_blk_request,
	/* [한국어] 완료 마무리를 softirq 에서 할 함수.
	 * [상류 코드 관찰] 이 표에 commit_rqs 항목이 없다. scm_blk_request() 는
	 * qd->last 로 제출 시점을 정하는데, include/linux/blk-mq.h 의 struct blk_mq_ops
	 * 설명은 그렇게 하는 드라이버는 그 콜백을 두어야 한다고 적고 있다.
	 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	.complete = scm_blk_request_done,
	/* [한국어] 하드웨어 큐를 세울 때 부를 함수. */
	.init_hctx = scm_blk_init_hctx,
	/* [한국어] 하드웨어 큐를 내릴 때 부를 함수. */
	.exit_hctx = scm_blk_exit_hctx,
/* [한국어] 표 끝. */
};

/* [한국어]
 * scm_blk_dev_setup - SCM 증분 하나를 블록 장치로 만들어 등록한다
 *
 * @bdev: scm_drv.c 의 probe 가 0 으로 채워 할당한 장치 상태. 이 함수가 채운다.
 * @scmdev: 붙은 SCM 장치.
 * @return: 0 = 성공, -ENODEV = 장치 번호가 동났음, 그 밖은 태그 집합·디스크
 *          만들기의 오류.
 *
 * 이 드라이버가 블록 계층에 장치를 내놓는 유일한 자리다. 단계가 많지만
 * 크게 넷으로 읽으면 된다 — 큐 한계 정하기, 장치 번호 배분, blk-mq 세우기,
 * gendisk 만들어 이름 붙이고 등록하기.
 *
 * 큐 한계에서 두 값이 서로 묶여 있다. 한 요청에 넣을 수 있는 조각 수는
 * 하드웨어가 허용하는 수와 AIDAW 목록 한 페이지에 들어가는 칸 수 중 작은
 * 쪽이고, 최대 전송 크기는 그 조각 수에 조각 하나의 크기(4K = 512바이트
 * 섹터 여덟)를 곱한 값이다. 즉 AIDAW 목록이 페이지 하나를 넘지 못한다는
 * 제약이 그대로 큐 한계가 된다.
 *
 * 장치 번호는 전역 계수기에서 배분하며 701 까지만 쓴다. 이름이 scma 부터
 * scmz 까지 26 개와 scmaa 부터 scmzz 까지 676 개, 합쳐 702 개이기 때문이다.
 *
 * blk-mq 설정에서 눈에 띄는 것은 요청마다 딸린 사적 공간의 크기가 정확히
 * blk_status_t 하나라는 점이다. 이 드라이버가 요청마다 기억할 것이 결과
 * 하나뿐이라는 설계가 여기 드러난다.
 *
 * 되감기는 라벨 셋으로 층을 이룬다. 디스크 등록에 실패하면 디스크를 풀고
 * 태그 집합도 풀고 장치 번호도 돌려주며, 태그 집합에서 실패하면 장치 번호만
 * 돌려준다.
 *
 * [상류 코드 관찰] 성공한 장치가 나중에 scm_blk_dev_cleanup() 으로 정리될
 * 때는 전역 계수기를 되돌리지 않는다. 즉 장치를 붙였다 뗐다 반복하면 번호가
 * 계속 늘어, 702 번째부터는 -ENODEV 로 물러난다. 되돌리는 곳은 이 함수의
 * 실패 경로 하나뿐이다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * [상류 코드 관찰] 같은 scmdev 포인터를 두 곳에 심는다 — 큐의 queuedata 와
 * gendisk 의 private_data 다. 앞의 것은 scm_blk_request() 가, 뒤의 것은
 * scm_request_prepare() 가 읽는다. 원본(1f0e418bb6)에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * 실행 컨텍스트: scm_drv.c 의 probe 안. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 위의 셋. 어느 경우에도 블록 계층에 흔적을 남기지 않고
 * 장치 번호도 돌려준다.
 *
 * 호출 체인:
 *   scm_probe() → [이 함수]
 *     → blk_mq_alloc_tag_set() → blk_mq_alloc_disk() → snprintf()
 *     → set_capacity() → device_add_disk()
 */
int scm_blk_dev_setup(struct scm_blk_dev *bdev, struct scm_device *scmdev)
{
	/* [한국어] 큐 한계를 담을 구조체. 나머지 항목은 0 으로 남아 블록 계층의 기본값이 쓰인다. */
	struct queue_limits lim = {
		/* [한국어] 논리 블록 크기를 4K 로 고정한다. MSB 의 블록 크기 지정과 짝이 되는 값이며,
		 * 이 드라이버가 4K 단위로만 전송한다는 뜻이다. */
		.logical_block_size	= 1 << 12,
	};
	/* [한국어] 이 장치에 배분될 번호. 이름과 부번호를 여기서 뽑는다. */
	unsigned int devindex;
	/* [한국어] 이름 만들기에 쓸 길이와, 반환할 오류 코드. */
	int len, ret;

	/* [한국어] 한 요청에 넣을 수 있는 조각 수. 하드웨어가 허용하는 수와 AIDAW 쪽 상한 중
	 * 작은 쪽을 고른다. */
	lim.max_segments = min(scmdev->nr_max_block,
		/* [한국어] AIDAW 목록은 페이지 하나를 넘을 수 없으므로, 페이지에 들어가는 AIDAW
		 * 칸 수가 곧 조각 수의 상한이다. AIDAW 한 칸이 데이터 페이지 하나를 가리킨다. */
		(unsigned int) (PAGE_SIZE / sizeof(struct aidaw)));
	/* [한국어] 최대 전송 크기는 조각 수에 조각 하나의 섹터 수(4K = 512바이트 여덟)를
	 * 곱한 값이다. 3 비트 왼쪽 이동이 그 곱하기 8 이며, 상류 주석이 그 근거를 밝힌다. */
	lim.max_hw_sectors = lim.max_segments << 3; /* 8 * 512 = blk_size */

	/* [한국어] 장치 번호를 배분한다. 원자적으로 올린 뒤 1 을 빼서 0 부터 시작하는 번호를
	 * 얻는다. 원자 연산이라 여러 장치가 동시에 붙어도 같은 번호가 두 번 나오지 않는다. */
	devindex = atomic_inc_return(&nr_devices) - 1;
	/* [한국어] 아래 줄의 상류 영어 주석 — 이름이 scma 부터 scmz 까지 26 개와
	 * scmaa 부터 scmzz 까지 676 개, 합쳐 702 개라는 뜻이다. */
	/* scma..scmz + scmaa..scmzz */
	/* [한국어] 이름으로 표현할 수 있는 범위를 넘었는지 본다. 0 부터 701 까지가 유효하다. */
	if (devindex > 701) {
		/* [한국어] 쓸 수 있는 장치 번호가 없다는 뜻으로 물러난다. */
		ret = -ENODEV;
		/* [한국어] 번호가 동났다. 위에서 이미 계수기를 올렸으므로 out 라벨에서 되돌린다. */
		goto out;
	}

	/* [한국어] 이 블록 장치가 붙어 있는 SCM 장치를 기억한다. 오류 기록이 장치 주소를 찍을 때 쓴다. */
	bdev->scmdev = scmdev;
	/* [한국어] 쓰기를 받을 수 있는 상태로 시작한다. */
	bdev->state = SCM_OPER;
	/* [한국어] 장치 잠금을 초기화한다. 이 잠금이 지키는 것은 바로 위의 state 하나뿐이다. */
	spin_lock_init(&bdev->lock);
	/* [한국어] 떠 있는 요청 수를 0 에서 시작한다. */
	atomic_set(&bdev->queued_reqs, 0);

	/* [한국어] blk-mq 콜백 표를 건다. 이 줄로 블록 계층이 이 파일의 네 함수를 알게 된다. */
	bdev->tag_set.ops = &scm_mq_ops;
	/* [한국어] 요청마다 딸릴 사적 공간의 크기. 이 드라이버가 요청마다 기억할 것은 결과
	 * 하나뿐이라 blk_status_t 하나면 충분하다. blk_mq_rq_to_pdu() 가 요청 바로
	 * 뒤의 이 자리를 돌려준다. */
	bdev->tag_set.cmd_size = sizeof(blk_status_t);
	/* [한국어] 하드웨어 큐 수를 요청 수 매개변수로 정한다. 큐 하나가 언제나 최대 하나의
	 * AOB 만 조립하므로, 이 값이 곧 동시에 조립할 수 있는 AOB 수가 된다.
	 * 다만 그 AOB 를 담을 struct scm_request 풀은 장치별이 아니라 모듈 전역이라,
	 * 장치가 여럿이면 같은 풀을 나눠 쓴다. */
	bdev->tag_set.nr_hw_queues = nr_requests;
	/* [한국어] 큐 전체의 태그 수. 하드웨어 큐 하나가 AOB 하나를 조립하고 그 AOB 에
	 * 요청이 nr_requests_per_io 개 들어가므로, 둘을 곱한 값이 동시에 떠 있을 수
	 * 있는 요청 수의 상한이 된다. */
	bdev->tag_set.queue_depth = nr_requests_per_io * nr_requests;
	/* [한국어] NUMA 노드를 지정하지 않는다. s390 은 NUMA 를 쓰지 않는 구성이 보통이고,
	 * 지정하지 않으면 블록 계층이 알아서 고른다. */
	bdev->tag_set.numa_node = NUMA_NO_NODE;

	/* [한국어] 태그 집합을 만든다. block/blk-mq.c 의 이 함수는 하드웨어 큐 수나 큐 깊이가
	 * 0 이면 -EINVAL 로 물러난다 — 위 두 값이 모듈 매개변수에서 그대로 오므로
	 * 0 인 매개변수는 여기서 걸린다. */
	ret = blk_mq_alloc_tag_set(&bdev->tag_set);
	/* [한국어] 실패 확인. */
	if (ret)
		/* [한국어] 태그 집합 만들기 실패. 장치 번호만 되돌리면 된다. */
		goto out;

	/* [한국어] 요청 큐와 gendisk 를 한 번에 만든다. 세 번째 인자가 큐의 queuedata 로
	 * 들어가며, scm_blk_request() 가 hctx->queue->queuedata 로 그것을 되찾는다.
	 * 이 호출 안에서 하드웨어 큐마다 scm_blk_init_hctx() 가 불린다. */
	bdev->gendisk = blk_mq_alloc_disk(&bdev->tag_set, &lim, scmdev);
	/* [한국어] 만들기 실패는 NULL 이 아니라 오류 포인터로 온다. */
	if (IS_ERR(bdev->gendisk)) {
		/* [한국어] 오류 포인터에서 오류 코드를 꺼낸다. */
		ret = PTR_ERR(bdev->gendisk);
		/* [한국어] 태그 집합만 되돌리면 된다. */
		goto out_tag;
	}
	/* [한국어] gendisk 에도 SCM 장치를 매단다. scm_request_prepare() 가 여기서 장치 주소를 되찾는다. */
	bdev->gendisk->private_data = scmdev;
	/* [한국어] 장치 파일 연산 표를 건다. 아래 scm_blk_devops 는 소유 모듈만 담고 있다. */
	bdev->gendisk->fops = &scm_blk_devops;
	/* [한국어] 적재 때 얻어 둔 주번호를 넣는다. */
	bdev->gendisk->major = scm_major;
	/* [한국어] 이 장치의 첫 부번호. 장치마다 SCM_NR_PARTS 개씩 띄워 배분한다. */
	bdev->gendisk->first_minor = devindex * SCM_NR_PARTS;
	/* [한국어] 이 장치가 차지하는 부번호 개수. 파티션 수와 같다. */
	bdev->gendisk->minors = SCM_NR_PARTS;

	/* [한국어] 이름을 "scm" 으로 시작한다. snprintf 는 실제로 쓴 길이가 아니라 쓰려 했던
	 * 길이를 돌려주지만, 여기서는 잘릴 일이 없어 언제나 3 이다. */
	len = snprintf(bdev->gendisk->disk_name, DISK_NAME_LEN, "scm");
	/* [한국어] 26 개를 넘어서면 이름이 두 글자가 된다. */
	if (devindex > 25) {
		/* [한국어] 두 글자 이름의 첫 글자를 이어 붙이고, 그만큼 길이를 늘린다. */
		len += snprintf(bdev->gendisk->disk_name + len,
				/* [한국어] 남은 공간을 정확히 계산해 넘긴다. 이름 배열을 넘어 쓰지 않게 하는 안전장치다. */
				DISK_NAME_LEN - len, "%c",
				/* [한국어] 첫 글자를 만든다. 26 으로 나눈 몫에서 1 을 빼는 것은 26 번째 장치가
				 * "scmaa" 가 되도록 맞추기 위한 것이다 — 몫이 1 일 때 'a' 가 나온다. */
				'a' + (devindex / 26) - 1);
		/* [한국어] 두 번째 글자를 위해 나머지만 남긴다. 이 대입 뒤로 devindex 는 부번호 계산에
		 * 쓸 수 없게 되지만, 그 계산은 위에서 이미 끝났다. */
		devindex = devindex % 26;
	}
	/* [한국어] 마지막 글자를 붙인다. 한 글자짜리 이름이면 앞의 if 를 건너뛰어 len 이 3 인
	 * 채로 들어오고, 두 글자짜리면 4 가 되어 그 뒤에 붙는다. */
	snprintf(bdev->gendisk->disk_name + len, DISK_NAME_LEN - len, "%c",
		 /* [한국어] 26 으로 나눈 나머지가 마지막 글자가 된다. */
		 'a' + devindex);

	/* [한국어] 아래 줄의 상류 영어 주석 — 용량 단위가 512바이트 섹터임을 밝힌다. */
	/* 512 byte sectors */
	/* [한국어] 용량을 512바이트 섹터 수로 알린다. 장치 크기를 9 비트 오른쪽으로 밀어
	 * 바이트를 섹터로 바꾼다. 논리 블록 크기는 4K 로 잡았지만 블록 계층의
	 * 용량 단위는 언제나 512바이트라, 두 단위가 다르다는 점이 상류 주석이
	 * 짚고 있는 대목이다. */
	set_capacity(bdev->gendisk, scmdev->size >> 9);
	/* [한국어] 디스크를 시스템에 등록한다. 첫 인자로 SCM 장치를 주어 sysfs 에서 그 아래
	 * 달리게 하고, 마지막 인자가 NULL 이라 추가 속성 묶음은 붙이지 않는다.
	 * 이 줄이 성공하면 그 즉시 I/O 가 내려올 수 있다. */
	ret = device_add_disk(&scmdev->dev, bdev->gendisk, NULL);
	/* [한국어] 등록 실패 확인. */
	if (ret)
		/* [한국어] 등록 실패. 만들어 둔 디스크부터 되돌린다. */
		goto out_cleanup_disk;

	/* [한국어] 여기까지 오면 블록 장치가 사용자 공간에 보인다. */
	return 0;

out_cleanup_disk:
	/* [한국어] 디스크를 푼다. 아직 계층에 등록하기 전이므로 떼어 내는 절차 없이 참조만 놓으면 된다. */
	put_disk(bdev->gendisk);
out_tag:
	/* [한국어] 태그 집합을 푼다. 디스크 만들기가 실패했을 때와 그 아래에서 실패했을 때 모두 지난다. */
	blk_mq_free_tag_set(&bdev->tag_set);
out:
	/* [한국어] 장치 번호를 돌려준다. 세 라벨 모두 이 줄을 지나므로 실패한 시도가 번호를 축내지 않는다. */
	atomic_dec(&nr_devices);
	/* [한국어] 성공이면 여기 오지 않으므로 ret 에는 언제나 실패 원인이 담겨 있다. */
	return ret;
}

/* [한국어]
 * scm_blk_dev_cleanup - 블록 장치를 내리고 blk-mq 자원을 푼다
 *
 * @bdev: 정리할 장치 상태. 이 구조체 자체는 호출자가 푼다.
 *
 * scm_blk_dev_setup() 의 짝이며, 세 줄이 정확히 그 역순이다.
 *
 * 순서가 중요하다. 먼저 디스크를 계층에서 떼어 내야 새 I/O 가 들어오지
 * 않는다. 그다음 디스크 참조를 놓고, 마지막에 태그 집합을 푼다. 태그 집합을
 * 먼저 풀면 아직 살아 있는 큐가 이미 사라진 태그를 쓰게 된다.
 *
 * 태그 집합을 푸는 과정에서 하드웨어 큐마다 scm_blk_exit_hctx() 가 불려
 * 큐별 조립 상태도 함께 정리된다.
 *
 * [상류 코드 관찰] setup 이 올려 둔 전역 장치 계수기를 여기서 되돌리지
 * 않는다. 그 결과 장치 번호는 다시 쓰이지 않는다. 원본(1f0e418bb6)에서
 * 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: scm_drv.c 의 remove 안. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 없다. 반환형이 void 이고 세 호출 모두 실패하지 않는다.
 *
 * 호출 체인:
 *   scm_remove() → [이 함수]
 *     → del_gendisk() → put_disk() → blk_mq_free_tag_set()
 */
void scm_blk_dev_cleanup(struct scm_blk_dev *bdev)
{
	/* [한국어] 먼저 디스크를 블록 계층에서 떼어 낸다. 이 줄이 돌아온 뒤로는 새 I/O 가
	 * 들어오지 않으므로, 아래 두 줄이 안전해진다. */
	del_gendisk(bdev->gendisk);
	/* [한국어] 디스크 참조를 놓는다. 위에서 계층에서 떼어 냈으므로 여기서 마지막 참조가 사라진다. */
	put_disk(bdev->gendisk);
	/* [한국어] 마지막으로 태그 집합을 푼다. 이 과정에서 하드웨어 큐마다
	 * scm_blk_exit_hctx() 가 불려 큐별 조립 상태도 함께 정리된다.
	 * 큐가 아직 살아 있는 동안 태그를 먼저 풀면 안 되므로 이것이 맨 뒤다. */
	blk_mq_free_tag_set(&bdev->tag_set);
}

/* [한국어]
 * scm_blk_set_available - 쓰기 금지 상태를 풀어 준다
 *
 * @bdev: 다시 쓸 수 있게 된 장치.
 *
 * scm_blk_handle_error() 가 쓰기 금지 응답을 보고 내려 둔 상태를 되돌리는
 * 유일한 자리다. EADM 버스가 증분을 다시 쓸 수 있다고 알리면 scm_drv.c 의
 * 알림 처리가 이 함수를 부른다.
 *
 * 되돌리기 전에 지금이 쓰기 금지 상태였는지 보는 것은 로그 때문이다. 이미
 * 정상이었다면 아무 일도 없었던 것이므로 관리자에게 알릴 것이 없다. 상태를
 * 내릴 때의 배치와 대칭을 이룬다.
 *
 * 이 한 줄이 있어야 쓰기가 다시 흐른다. scm_permit_request() 가 이 필드를
 * 보고 쓰기를 막고 있었기 때문이다.
 *
 * 잠금을 irqsave 판으로 잡는 것은 이 함수 때문이 아니라 짝이 되는
 * scm_blk_handle_error() 때문이다. 그쪽이 인터럽트 문맥에서 같은 잠금을
 * 잡으므로, 이 잠금을 쓰는 모든 자리가 인터럽트를 꺼야 교착이 없다.
 *
 * 실행 컨텍스트: scm_drv.c 의 알림 처리 안. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환형이 void 다.
 *
 * 호출 체인:
 *   scm_notify() → [이 함수] → pr_info()
 */
void scm_blk_set_available(struct scm_blk_dev *bdev)
{
	/* [한국어] 인터럽트 상태를 저장해 둘 자리. */
	unsigned long flags;

	/* [한국어] 장치 잠금을 잡는다. 짝이 되는 scm_blk_handle_error() 가 인터럽트 문맥에서
	 * 같은 잠금을 잡으므로, 여기서도 인터럽트를 끄지 않으면 교착이 생긴다.
	 * 이 함수 자체는 프로세스 컨텍스트다. */
	spin_lock_irqsave(&bdev->lock, flags);
	/* [한국어] 지금이 쓰기 금지 상태였는지 본다. */
	if (bdev->state == SCM_WR_PROHIBIT)
		/* [한국어] 쓰기가 막혀 있었음을 관리자에게 알린다. 아래에서 어차피 정상으로 덮으므로
		 * 이 확인은 오직 로그를 한 번만 찍기 위한 것이다. */
		pr_info("%lx: Write access to the SCM increment is restored\n",
			/* [한국어] 장치 주소를 찍어 어느 증분인지 알린다. 부호 없는 긴 정수로 캐스팅하는 것은
			 * 장치 주소의 실제 타입과 무관하게 %lx 서식에 맞추기 위한 것이다. */
			(unsigned long) bdev->scmdev->address);
	/* [한국어] 상태를 정상으로 되돌린다. 이 한 줄이 있어야 scm_permit_request() 가 쓰기를 다시 통과시킨다. */
	bdev->state = SCM_OPER;
	/* [한국어] 잠금을 놓고 인터럽트 상태를 원래대로 돌린다. */
	spin_unlock_irqrestore(&bdev->lock, flags);
}

/* [한국어]
 * scm_blk_params_valid - 모듈 매개변수가 쓸 만한 값인지 본다
 *
 * @return: true = 쓸 만하다, false = 쓸 수 없다.
 *
 * 모듈 적재의 첫 관문이다. 사용자가 준 값으로 자원을 잡기 전에 걸러낸다.
 *
 * 보는 것은 한 요청에 묶을 수 있는 개수 하나뿐이다. 0 이면 AOB 를 하나도
 * 채울 수 없어 요청 배열이 빈 채로 만들어지고, 상한을 넘으면 AOB 의 msb
 * 배열 밖을 쓰게 된다. 그 상한이 64 인 근거는 asm/eadm.h 소관이라 이
 * 트리에서 확인 못 함.
 *
 * [상류 코드 관찰] 또 하나의 매개변수인 nr_requests 는 보지 않는다. 그 값이
 * 0 이면 요청을 하나도 만들지 않고, 하드웨어 큐 수도 0 이 되는데,
 * block/blk-mq.c 의 blk_mq_alloc_tag_set() 이 그 경우 -EINVAL 로 물러난다.
 * 즉 모듈은 올라오지만 장치가 하나도 붙지 못하게 된다. 원본(1f0e418bb6)에서
 * 확인했으며 코드는 고치지 않았다.
 *
 * __init 표시가 붙어 있어 초기화가 끝나면 이 코드는 메모리에서 버려진다.
 *
 * 실행 컨텍스트: 모듈 적재. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 판정만 돌려준다.
 *
 * 호출 체인:
 *   scm_blk_init() → [이 함수]
 */
static bool __init scm_blk_params_valid(void)
{
	/* [한국어] 0 이면 AOB 에 MSB 를 하나도 넣을 수 없고, 64 를 넘으면 AOB 의 msb 배열
	 * 밖을 쓰게 된다. 그 상한 64 의 근거는 asm/eadm.h 소관이라 이 트리에서 확인 못 함. */
	if (!nr_requests_per_io || nr_requests_per_io > 64)
		/* [한국어] 쓸 수 없는 값이다. 호출자가 모듈 적재를 -EINVAL 로 실패시킨다. */
		return false;

	/* [한국어] 여기까지 왔으면 쓸 만한 값이다. */
	return true;
}

/* [한국어]
 * scm_blk_init - 모듈 적재 진입점. 자원을 순서대로 세우고 버스에 등록한다
 *
 * @return: 0 = 성공, 음수 = 어느 단계에서 실패했는지에 따른 오류.
 *
 * 이 모듈의 시작점이다. 다섯 단계를 순서대로 밟는데, 그 순서 자체가 의미를
 * 갖는다.
 *  1. 매개변수를 확인한다. 아무것도 잡기 전에 거르는 것이 가장 싸다.
 *  2. 블록 장치 주번호를 얻는다. 0 을 넘기면 커널이 남는 번호를 골라
 *     돌려주므로, 반환값이 곧 주번호다. block/genhd.c 의 설명에서 확인된다.
 *  3. 요청 풀과 AIDAW mempool 을 만든다. I/O 경로가 잠들 수 없으므로 여기서
 *     미리 잡아 둔다.
 *  4. s390 debug 기능 핸들을 만들고 보기 방식과 기록 수준을 정한다.
 *  5. 마지막으로 EADM 버스에 드라이버를 등록한다. 이것이 맨 뒤인 이유는
 *     등록하는 즉시 probe 가 들어올 수 있고, 그때 앞의 네 가지가 모두 준비돼
 *     있어야 하기 때문이다.
 *
 * 되감기는 라벨 셋이 층을 이루며, 각 라벨이 그 시점까지 잡은 것을 역순으로
 * 푼다. debug 핸들을 푸는 라벨은 아래로 흘러 요청 풀과 주번호까지 이어서
 * 푼다.
 *
 * [상류 코드 관찰] 마지막 성공 반환이 0 이 아니라 ret 을 돌려준다. 그
 * 자리에서 ret 은 반드시 0 이므로 결과는 같다. 원본(1f0e418bb6)에서
 * 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 모듈 적재. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 네 갈래(매개변수, 주번호, 요청 풀, debug 핸들, 버스 등록)가
 * 모두 라벨로 모여 그때까지 잡은 것만 정확히 되돌린다.
 *
 * 호출 체인:
 *   module_init → [이 함수]
 *     → scm_blk_params_valid() → register_blkdev() → scm_alloc_rqs()
 *     → debug_register() → scm_drv_init()
 */
static int __init scm_blk_init(void)
{
	/* [한국어] 실패 반환값의 기본값. 아래 첫 관문이 매개변수 검사라, 그 실패를 이 초기값으로 나타낸다. */
	int ret = -EINVAL;

	/* [한국어] 가장 먼저 모듈 매개변수를 거른다. 아무 자원도 잡기 전이라 되감을 것이 없는 자리다. */
	if (!scm_blk_params_valid())
		/* [한국어] 매개변수가 잘못됐다. ret 이 -EINVAL 로 초기화돼 있어 그대로 나간다. */
		goto out;

	/* [한국어] 블록 장치 주번호를 얻는다. 첫 인자가 0 이면 커널이 남는 번호를 골라 돌려준다.
	 * 이름 "scm" 은 /proc/devices 에 보이는 이름이다. */
	ret = register_blkdev(0, "scm");
	/* [한국어] 음수면 실패다. 0 을 넘긴 경우의 반환값 규약은 block/genhd.c 의 설명에서 확인된다. */
	if (ret < 0)
		/* [한국어] 주번호를 못 얻었다. 아직 잡은 것이 없으므로 그대로 물러난다. */
		goto out;

	/* [한국어] 0 을 넘겨 받은 번호가 곧 이 드라이버의 주번호다. 아래 gendisk 의 major 로 들어간다. */
	scm_major = ret;
	/* [한국어] 요청 풀과 AIDAW mempool 을 만든다. I/O 경로는 잠들 수 없으므로 여기서 미리 잡아 둔다. */
	ret = scm_alloc_rqs(nr_requests);
	/* [한국어] 실패 확인. */
	if (ret)
		/* [한국어] 요청 풀 마련이 실패했다. 이미 만들어 둔 것이 있을 수 있으므로 out_free 로 간다. */
		goto out_free;

	/* [한국어] s390 debug 기능 핸들을 만든다. 이름은 debugfs 에 보일 이름이고, 뒤의 세
	 * 숫자는 버퍼 구성이다. 그 뜻은 asm/debug.h 소관이라 이 트리에서 확인 못 함.
	 * 이 전역은 scm_blk.h 가 extern 으로 내보내며, 로그 매크로 셋이 모두 이것을 쓴다. */
	scm_debug = debug_register("scm_log", 16, 1, 16);
	/* [한국어] 핸들을 못 얻었다. */
	if (!scm_debug) {
		/* [한국어] 핸들 만들기는 오류 코드를 주지 않으므로 메모리 부족으로 본다. */
		ret = -ENOMEM;
		/* [한국어] 요청 풀과 주번호를 되돌린다. */
		goto out_free;
	}

	/* [한국어] 기록을 16진수와 아스키로 함께 보여 주는 방식을 붙인다. 이 파일이 남기는
	 * 기록에는 문자열과 이진 덩어리가 섞여 있어 두 가지로 함께 보는 것이 편하다.
	 * debug 기능의 보기 방식은 asm/debug.h 소관이라 이 트리에서 확인 못 함. */
	debug_register_view(scm_debug, &debug_hex_ascii_view);
	/* [한국어] 기록 수준을 2 로 정한다. 이보다 큰 수준으로 남긴 기록은 버려지므로,
	 * 오류 기록(수준 1)과 상태 기록(수준 2)은 남고 I/O 경로의 잦은 기록(수준 5)은 걸러진다. */
	debug_set_level(scm_debug, 2);

	/* [한국어] 마지막 단계 — EADM 버스에 드라이버를 등록한다. 이 줄이 성공하는 순간부터
	 * probe 가 들어올 수 있으므로, 앞의 네 준비가 모두 끝난 뒤에 놓여 있다. */
	ret = scm_drv_init();
	/* [한국어] 등록 실패 확인. */
	if (ret)
		/* [한국어] 등록이 실패했으니 debug 핸들부터 되돌린다. */
		goto out_dbf;

	/* [한국어] 여기 오면 ret 은 0 이다.
	 * [상류 코드 관찰] 0 을 직접 쓰지 않고 ret 을 돌려주지만 결과는 같다.
	 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	return ret;

out_dbf:
	/* [한국어] debug 핸들만 되돌린다. 이 라벨은 버스 등록이 실패했을 때만 닿으며,
	 * 아래 out_free 로 흘러 나머지도 이어서 푼다. */
	debug_unregister(scm_debug);
out_free:
	/* [한국어] 요청 풀과 mempool 을 푼다. 요청을 만들다 실패한 경우에는 그때까지 만든 것만 들어 있다. */
	scm_free_rqs();
	/* [한국어] 주번호를 반납한다. 위 register_blkdev 가 성공한 뒤에만 이 라벨에 닿으므로
	 * scm_major 에는 유효한 번호가 들어 있다. */
	unregister_blkdev(scm_major, "scm");
out:
	/* [한국어] 여기까지 온 ret 은 실패 원인을 담고 있다. 그대로 올려보내면 모듈 적재가 실패한다. */
	return ret;
}
/* [한국어] 모듈 적재 진입점을 등록한다. insmod 때 커널이 이 함수를 부른다.
 * 커널에 내장하는 경우에는 초기화 순서에 맞춰 불린다. */
module_init(scm_blk_init);

/* [한국어]
 * scm_blk_cleanup - 모듈 해제 진입점. 적재의 정확한 역순으로 되돌린다
 *
 * scm_blk_init() 이 세운 것을 네 줄로 되돌린다. 순서가 적재의 역순이라는
 * 점이 이 함수의 전부라고 해도 된다.
 *
 * 먼저 버스에서 드라이버를 뗀다. 그 과정에서 붙어 있던 장치마다
 * scm_blk_dev_cleanup() 이 불려 블록 장치가 모두 내려간다. 이것이 맨 앞에
 * 와야 하는 이유는, 뒤의 세 줄이 아직 쓰이고 있을지 모르는 자원을 풀기
 * 때문이다. 특히 요청 풀을 먼저 풀었다면 아직 떠 있는 I/O 가 이미 해제된
 * 요청을 건드리게 된다.
 *
 * 그다음 debug 핸들, 요청 풀과 mempool, 주번호를 차례로 놓는다.
 *
 * __exit 표시가 붙어 있어, 모듈로 만들지 않고 커널에 넣으면 이 코드는
 * 아예 빠진다.
 *
 * 실행 컨텍스트: 모듈 제거. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 없다. 반환형이 void 이고 네 호출 모두 실패하지 않는다.
 *
 * 호출 체인:
 *   module_exit → [이 함수]
 *     → scm_drv_cleanup() → debug_unregister() → scm_free_rqs()
 *     → unregister_blkdev()
 */
static void __exit scm_blk_cleanup(void)
{
	/* [한국어] 먼저 EADM 버스에서 드라이버를 뗀다. 그 안에서 붙어 있던 장치마다
	 * scm_drv.c 의 remove 가 불려 블록 장치가 모두 내려간다. 아래 세 줄이 푸는
	 * 자원을 그 장치들이 아직 쓰고 있을 수 있으므로 이것이 반드시 먼저다. */
	scm_drv_cleanup();
	/* [한국어] debug 기능 핸들을 반납한다. 이 줄 뒤로는 로그 매크로를 쓰면 안 된다. */
	debug_unregister(scm_debug);
	/* [한국어] 요청 풀과 AIDAW mempool 을 푼다. 위에서 장치를 모두 내린 뒤라 밖에 나가 있는 요청이 없다. */
	scm_free_rqs();
	/* [한국어] 마지막으로 주번호를 반납한다. 적재 때 가장 먼저 얻었던 것이라 가장 나중에 놓는다. */
	unregister_blkdev(scm_major, "scm");
}
/* [한국어] 모듈 해제 진입점을 등록한다. rmmod 때 커널이 이 함수를 부른다. */
module_exit(scm_blk_cleanup);
