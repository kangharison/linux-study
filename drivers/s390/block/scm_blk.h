/* SPDX-License-Identifier: GPL-2.0 */

/*
 * [한국어 설명] s390 SCM(Storage Class Memory) 블록 드라이버의 공용 헤더
 * (scm_blk.h)
 *
 * === 파일의 역할 ===
 * IBM 메인프레임의 SCM — 스토리지 클래스 메모리 — 을 리눅스 블록 장치로
 * 노출하는 드라이버의 내부 헤더다. 같은 디렉터리의 세 파일
 * (scm_blk.c, scm_drv.c, 그리고 필요한 곳)이 이것을 포함하며, 그 사이에서
 * 오가는 **자료구조 둘과 로그 매크로 셋** 을 정의한다.
 *
 * SCM 은 디스크가 아니라 **주소로 접근하는 영속 메모리** 다. 그래서 이
 * 드라이버는 회전 지연이나 탐색 같은 개념이 없고, 요청 하나를 AOB(Aob,
 * ASYNC Operation Block)라는 서술자로 바꿔 하드웨어에 넘기는 일에 집중한다.
 * 그 서술자 안에서 실제 메모리 조각들을 가리키는 것이 AIDAW 목록이다.
 *
 * 이 헤더가 정의하는 것이 정확히 그 두 계층에 대응한다.
 *   - struct scm_blk_dev : SCM 증분(increment) 하나 = 블록 장치 하나.
 *   - struct scm_request : 그 장치로 나가는 I/O 요청 하나. blk-mq 요청
 *                          여러 개를 한 AOB 에 묶어 보낼 수 있다.
 *
 * **이 트리에서 확인할 수 없는 것이 많다.** AOB, AIDAW, struct scm_device,
 * scm_driver 등록 규약, EADM(Extended Asynchronous Data Mover) 하위 채널
 * 인터페이스는 모두 asm/eadm.h 소관인데, 그 헤더는 arch/s390 에 있고 이
 * 트리는 sparse checkout 이라 arch/ 가 없다. 아래 주석에서 그런 대목은
 * 모두 "이 트리에서 확인 못 함" 으로 표시했으며, 호출 자리에서 읽히는
 * 쓰임새까지만 적었다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 장치가 붙을 때:
 *   EADM 버스 -> scm_drv.c 의 scm_probe()
 *     -> struct scm_blk_dev 를 0 으로 할당
 *       -> scm_blk_dev_setup() [scm_blk.c]
 *          -> blk-mq 태그 집합과 gendisk 를 만들어 블록 계층에 등록
 *
 * I/O 가 내려올 때:
 *   블록 계층 -> blk-mq -> scm_blk.c 의 큐 콜백
 *     -> 놀고 있는 struct scm_request 를 꺼내 온다
 *       -> 요청들을 AOB 의 MSB 목록에 채우고, 데이터 조각은
 *          scm_aidaw_fetch() 로 얻은 AIDAW 에 적는다
 *         -> eadm_start_aob() 로 하드웨어에 넘긴다
 *
 * 완료가 올라올 때:
 *   EADM 인터럽트 -> scm_blk_irq() [이 헤더가 선언, scm_blk.c 가 구현]
 *     -> 오류면 재시도 횟수를 깎아 다시 시도
 *       -> 아니면 묶여 있던 blk-mq 요청들을 차례로 완료 처리
 *
 * 실행 컨텍스트: 등록과 정리는 프로세스 컨텍스트, scm_blk_irq() 는
 * 인터럽트 문맥이다. 그래서 아래 struct scm_blk_dev 의 잠금이
 * spin_lock_irqsave 판으로 쓰인다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 블록 계층(blkdev, blk-mq). gendisk 와 태그 집합으로 이어진다.
 * 아래쪽: EADM 하위 채널. asm/eadm.h 가 그 접점이며 이 트리에 없다.
 * 옆쪽: s390 의 debug 기능(asm/debug.h). 아래 로그 매크로 셋이 그것을
 *   감싸며, 이 역시 트리 밖이다.
 *
 * 데이터 흐름:
 *   blk-mq 요청 -> struct scm_request 의 request 배열에 모임
 *     -> AOB 의 MSB 목록 + AIDAW 목록으로 번역 -> 하드웨어
 *   완료는 그 반대로, AOB 의 응답 블록 -> blk_status_t -> 각 요청
 *
 * 공유 상태: struct scm_blk_dev 하나가 장치마다 있고, scm_drv.c 가
 *   drvdata 에 매달아 둔다. 그 안에서 잠금이 지키는 것은 **state 필드
 *   하나뿐** 이며, 나머지는 원자 변수이거나 초기화 이후 불변이다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct scm_blk_dev  : SCM 증분 하나에 대응하는 블록 장치 상태.
 * struct scm_request  : 하드웨어로 나가는 요청 하나. blk-mq 요청 여러
 *                       개를 묶는다.
 * scm_blk_dev_setup() : blk-mq 와 gendisk 를 세워 장치를 등록한다.
 * scm_blk_irq()       : 완료 인터럽트 진입점. scm_driver 의 handler 로 걸린다.
 * scm_aidaw_fetch()   : 요청에 쓸 AIDAW 공간을 확보한다.
 * SCM_LOG 계열        : s390 debug 기능을 감싼 로그 매크로 셋.
 *
 * === 이 헤더에서 눈에 띄는 것 ===
 * 선언해 두고 쓰지 않는 것이 셋 있다 — scm_blk_dev 의 finished_requests
 * 필드, SCM_QUEUE_DELAY 매크로, 그리고 **한 번도 대입되지 않는 rq 필드**
 * 다. 각 자리의 [상류 코드 관찰]에 근거와 함께 적어 두었다.
 */
#ifndef SCM_BLK_H
#define SCM_BLK_H

/* [한국어] [상류 코드 관찰] irqreturn_t 나 request_irq 같은 이름을 이 헤더에서
 * 직접 쓰는 곳은 없다. 다만 완료 경로가 인터럽트 문맥에서 도는 드라이버라,
 * 포함하는 쪽(scm_blk.c)이 이 헤더를 통해 받도록 둔 것으로 보인다. */
#include <linux/interrupt.h>
/* [한국어] spinlock_t — 아래 struct scm_blk_dev 의 lock 필드가 이 타입이다. */
#include <linux/spinlock.h>
/* [한국어] struct request_queue, struct gendisk — 아래 구조체의 두 필드가
 * 이 타입이며, 이 드라이버가 블록 계층에 붙는 통로다. */
#include <linux/blkdev.h>
/* [한국어] struct blk_mq_tag_set 과 blk_status_t. 이 드라이버는 옛 요청 큐가
 * 아니라 **blk-mq(다중 큐)** 로 붙는다. */
#include <linux/blk-mq.h>
/* [한국어] struct list_head — 아래 두 구조체가 각각 하나씩 가지고 있다. */
#include <linux/list.h>

/* [한국어] s390 의 debug 기능. 아래 로그 매크로 셋이 이것을 감싼다.
 * arch/s390 소관이라 이 트리에서 확인 못 함. */
#include <asm/debug.h>
/* [한국어] EADM(Extended Asynchronous Data Mover) 인터페이스. **이 드라이버의
 * 하드웨어 쪽 접점 전부** 가 여기 있다 — struct scm_device, struct aob,
 * struct aidaw, struct aob_rq_header, scm_driver 등록 규약, eadm_start_aob().
 * arch/s390 소관이라 이 트리에서 확인 못 함. */
#include <asm/eadm.h>

/* [한국어] SCM 장치 하나가 가질 수 있는 파티션 수.
 * gendisk 의 부번호 배분에 쓰인다 — scm_blk.c:476 이 이 값으로 장치별
 * first_minor 를 띄우고, :477 이 minors 로 그대로 넣는다. 즉 장치 하나가
 * 부번호 8개를 차지한다. */
#define SCM_NR_PARTS 8
/* [한국어] [상류 코드 관찰] 이름으로 보아 큐 재시도 지연(단위 불명)으로 보이나,
 * **이 트리 어디에서도 참조하지 않는다.** 원본(1f0e418bb6)에서 확인했으며
 * 코드는 고치지 않았다. */
#define SCM_QUEUE_DELAY 5

struct scm_blk_dev {
	/* [한국어] 이 장치의 블록 계층 요청 큐.
	 * 설정자: **없다.** 이 트리 어디에도 이 필드에 값을 넣는 코드가 없다.
	 * 읽는 자: scm_blk.c:245 의 blk_mq_kick_requeue_list(bdev->rq) 한 곳뿐.
	 * 값 범위: 구조체가 scm_drv.c:46 의 kzalloc_obj 로 0 채워 할당되므로
	 * **언제나 NULL** 이다.
	 * 동기화: 없다.
	 * [상류 코드 관찰] 즉 선언과 읽기만 있고 대입이 없다. 그 읽기가 있는
	 * scm_request_requeue() 는 두 자리에서만 불리는데(scm_blk.c:272 의
	 * eadm_start_aob() 실패, :397 의 쓰기 금지 응답 처리) 둘 다 오류 경로라,
	 * 정상 동작에서는 그 줄에 닿지 않는다. blk-mq 로 옮겨 오면서 큐를
	 * gendisk 안에 두게 됐는데 이 필드만 남은 것으로 보이나, 그 경위는
	 * 이 트리에서 확인 못 함. 원본(1f0e418bb6)에서 확인했으며 코드는
	 * 고치지 않았다. */
	struct request_queue *rq;
	/* [한국어] 이 장치의 gendisk — 사용자 공간에 보이는 블록 장치 그 자체다.
	 * 설정자: scm_blk_dev_setup() 이 blk_mq_alloc_disk() 로 만들고
	 * (scm_blk.c:468), 이어서 private_data·fops·major·first_minor·minors 를
	 * 채운다(:473~477).
	 * 읽는 자: scm_blk.c:180 이 private_data 를 거쳐 scm_device 를 되찾고,
	 * 정리 경로가 등록을 해제한다.
	 * 값 범위: 유효한 gendisk 포인터. 만들기 실패는 setup 이 걸러낸다.
	 * 동기화: 초기화 이후 이 드라이버가 바꾸지 않는다. */
	struct gendisk *gendisk;
	/* [한국어] blk-mq 태그 집합 — 요청 큐의 깊이와 하드웨어 큐 수를 정한다.
	 * 설정자: scm_blk_dev_setup() 이 ops, cmd_size, nr_hw_queues,
	 * queue_depth 를 차례로 채운다(scm_blk.c:458~461).
	 * 읽는 자: blk_mq_alloc_disk() 가 이것을 근거로 큐를 만든다.
	 * 값 범위: cmd_size 가 sizeof(blk_status_t) 인 것이 눈에 띄는데,
	 * 요청마다 딸린 사적 공간에 오류 코드 하나만 두겠다는 뜻이다 —
	 * 완료 시 scm_blk.c:256 이 그 자리에 결과를 적는다.
	 * 동기화: 초기화 이후 불변.
	 * **포인터가 아니라 값으로 품는다** — 태그 집합의 수명이 이 구조체와
	 * 같다는 뜻이다. */
	struct blk_mq_tag_set tag_set;
	/* [한국어] 이 블록 장치가 붙어 있는 SCM 장치.
	 * 설정자: scm_blk_dev_setup() 이 인자로 받아 넣는다(scm_blk.c:453).
	 * 읽는 자: 오류 기록이 장치 주소를 찍을 때(:384, :520) 등.
	 * 값 범위: 유효한 scm_device 포인터. 그 구조체의 내용은 asm/eadm.h
	 * 소관이라 이 트리에서 확인 못 함.
	 * 동기화: 초기화 이후 불변. */
	struct scm_device *scmdev;
	/* [한국어] 아래 state 필드를 지키는 스핀락.
	 * 설정자: scm_blk_dev_setup() 이 초기화한다(scm_blk.c:455).
	 * 읽는 자: 상태를 바꾸는 두 자리 — 쓰기 금지로 내리는 :381~386 과
	 * 정상으로 되돌리는 :517~522.
	 * 값 범위: 스핀락.
	 * 동기화: **이 잠금이 지키는 것은 state 하나뿐** 이다. 같은 구조체의
	 * queued_reqs 는 원자 변수로 따로 처리하고, 나머지는 초기화 이후
	 * 불변이라 잠금이 필요 없다.
	 * irqsave 판으로 쓰이는데, 상태를 내리는 쪽이 완료 인터럽트 경로에서
	 * 불리기 때문이다. */
	spinlock_t lock;
	/* [한국어] 이 장치에 지금 떠 있는 요청 수.
	 * 설정자·읽는 자: 요청을 하드웨어에 넘길 때 올리고(scm_blk.c:269),
	 * 완료(:261)와 재큐(:243)에서 내린다. 초기값은 setup 이 0 으로 둔다(:456).
	 * 값 범위: 0 이상.
	 * 동기화: **원자 변수라 위 잠금을 쓰지 않는다.** 세 자리 모두 단순한
	 * 증감이라 읽고-고치고-쓰기가 필요 없기 때문이다. */
	atomic_t queued_reqs;
	/* [한국어] 이 장치가 쓰기를 받을 수 있는 상태인지.
	 * 설정자: 하드웨어가 쓰기 금지 응답(EQC_WR_PROHIBIT)을 주면
	 * SCM_WR_PROHIBIT 로 내리고(scm_blk.c:385), 장치가 다시 쓸 수 있게
	 * 되면 SCM_OPER 로 되돌린다(:521). 초기값은 SCM_OPER 다(:454).
	 * 읽는 자: 같은 두 자리가 **바뀌는지 확인해 로그를 한 번만 찍는 데** 쓴다.
	 * 값 범위: SCM_OPER(정상) 또는 SCM_WR_PROHIBIT(쓰기 금지).
	 * 동기화: 위 lock 이 지킨다. 이 구조체에서 잠금이 필요한 유일한 필드다.
	 * **열거형에 이름을 붙이지 않고 필드 자리에 바로 쓴** 형태라, 다른 곳에서
	 * 이 타입을 이름으로 가리킬 수 없다. */
	enum {SCM_OPER, SCM_WR_PROHIBIT} state;
	/* [한국어] [상류 코드 관찰] 완료된 요청 목록으로 보이는 이름이지만,
	 * **이 트리 어디에서도 참조하지 않는다** — 선언만 있고 초기화조차 되지
	 * 않는다(구조체가 0 으로 할당되므로 두 포인터가 NULL 인 채로 남는다).
	 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	struct list_head finished_requests;
};

struct scm_request {
	/* [한국어] 이 요청이 속한 블록 장치.
	 * 설정자: 요청을 꺼내 쓸 때 scm_blk.c 가 넣는다.
	 * 읽는 자: 완료·재큐 경로가 여기서 queued_reqs 와 lock 에 닿는다
	 * (scm_blk.c:237, :250, :270).
	 * 값 범위: 유효한 포인터.
	 * 동기화: 요청 하나는 한 흐름에서만 다뤄지므로 잠금이 필요 없다. */
	struct scm_blk_dev *bdev;
	/* [한국어] 이 요청에서 **다음에 쓸 AIDAW 자리.**
	 * 설정자: 요청을 세울 때 AOB 의 MSB 목록 바로 뒤를 가리키게 두고
	 * (scm_blk.c:232), AIDAW 를 소비할 때마다 앞으로 민다(:206).
	 * 읽는 자: scm_aidaw_fetch() 가 남은 공간이 충분한지 볼 때(:168).
	 * 값 범위: AOB 안의 어느 지점. AIDAW 의 내용은 asm/eadm.h 소관이라
	 * 이 트리에서 확인 못 함.
	 * 동기화: 필요 없다.
	 * **AIDAW 를 따로 할당하지 않고 AOB 안의 남는 공간에 이어 적는** 구조라,
	 * 이 포인터가 그 커서 노릇을 한다. */
	struct aidaw *next_aidaw;
	/* [한국어] 이 AOB 하나에 묶인 blk-mq 요청들의 배열.
	 * 설정자: 요청을 채울 때 scm_blk.c 가 하나씩 넣는다.
	 * 읽는 자: 재큐(scm_blk.c:240)와 완료(:255~262)가 배열을 훑으며
	 * 각 요청을 블록 계층에 돌려준다.
	 * 값 범위: 원소 수는 모듈 매개변수 nr_requests_per_io 가 정한다.
	 * 훑는 쪽이 **NULL 을 만나면 멈추는** 방식이라, 다 차지 않은 배열도
	 * 그대로 다룰 수 있다.
	 * 동기화: 필요 없다. */
	struct request **request;
	/* [한국어] 이 요청에 대응하는 AOB — 하드웨어에 실제로 넘기는 서술자다.
	 * 설정자: 요청을 세울 때 scm_blk.c 가 붙인다.
	 * 읽는 자: eadm_start_aob() 에 넘기고, 완료 후 응답 블록을 읽어
	 * 오류 종류를 가린다(scm_blk.c:379).
	 * 값 범위: 유효한 AOB 포인터. 그 내용은 asm/eadm.h 소관이라
	 * 이 트리에서 확인 못 함.
	 * 동기화: 필요 없다. */
	struct aob *aob;
	/* [한국어] 놀고 있는 요청들의 목록에 매다는 고리.
	 * 설정자·읽는 자: 요청을 만들 때 초기화하고(scm_blk.c:84) 목록에
	 * 붙였다가(:86, :140), 꺼내 쓸 때 뗀다(:58, :117).
	 * 값 범위: 목록에 붙어 있는 동안 유효.
	 * 동기화: scm_blk.c 쪽의 목록 잠금이 지킨다 — 이 헤더의 lock 이 아니다. */
	struct list_head list;
	/* [한국어] 남은 재시도 횟수.
	 * 설정자: 요청을 세울 때 4 로 둔다(scm_blk.c:229).
	 * 읽는 자: 오류 기록을 남길지 판단할 때 보고(:363), 완료 인터럽트에서
	 * **후위 감소로 확인과 감소를 한 번에** 한다(:407) — 0 이 되면 더는
	 * 다시 시도하지 않고 오류로 끝낸다.
	 * 값 범위: 0~4.
	 * 동기화: 필요 없다. */
	u8 retries;
	/* [한국어] 이 요청의 결과.
	 * 설정자: 요청을 세울 때 BLK_STS_OK 로 두고(scm_blk.c:230), 완료
	 * 인터럽트가 하드웨어가 준 값으로 덮는다(:404).
	 * 읽는 자: 오류 종류를 가리는 자리들(:357, :375)과, 묶인 요청마다
	 * 결과를 적어 주는 완료 경로(:256).
	 * 값 범위: blk_status_t. 시간 초과와 입출력 오류를 따로 가려낸다.
	 * 동기화: 필요 없다. */
	blk_status_t error;
};

/* [한국어] blk-mq 요청 포인터에서 그것을 감싸는 AOB 요청 머리로 되돌린다.
 * `data` 라는 이름의 유연 배열 뒤에 요청이 붙어 있다는 전제이며,
 * 그 배치는 asm/eadm.h 소관이라 이 트리에서 확인 못 함.
 * void 로 한 번 캐스팅하는 것은 원래 타입과 무관하게 오프셋 계산만
 * 하겠다는 뜻이다. */
#define to_aobrq(rq) container_of((void *) rq, struct aob_rq_header, data)

/* [한국어] 블록 장치를 세워 등록한다. scm_drv.c 의 probe 가 부르는 유일한
 * 초기화 진입점이며, 구현은 scm_blk.c 에 있다. */
int scm_blk_dev_setup(struct scm_blk_dev *, struct scm_device *);
/* [한국어] 그 짝. scm_drv.c 의 remove 가 부른다. */
void scm_blk_dev_cleanup(struct scm_blk_dev *);
/* [한국어] 장치가 다시 쓸 수 있게 됐음을 알린다. scm_drv.c 가 SCM_AVAIL
 * 알림을 받았을 때 부르며, 안에서 state 를 SCM_OPER 로 되돌린다. */
void scm_blk_set_available(struct scm_blk_dev *);
/* [한국어] 완료 인터럽트 진입점. scm_drv.c 의 scm_driver 표에 handler 로
 * 걸려, EADM 계층이 완료 때마다 부른다. */
void scm_blk_irq(struct scm_device *, void *, blk_status_t);

/* [한국어] 이 요청에 쓸 AIDAW 공간을 확보한다. 남은 공간이 모자라면 새
 * 자리를 마련하는 것으로 보이며, 구현은 scm_blk.c 에 있다. */
struct aidaw *scm_aidaw_fetch(struct scm_request *scmrq, unsigned int bytes);

/* [한국어] SCM 드라이버를 EADM 버스에 등록한다. 구현이 scm_drv.c 에 있고
 * 부르는 쪽은 scm_blk.c 의 모듈 초기화라, **이 헤더가 두 파일을 잇는
 * 방향이 여기서는 반대** 다. */
int scm_drv_init(void);
/* [한국어] 그 짝. 모듈이 내려갈 때 불린다. */
void scm_drv_cleanup(void);

/* [한국어] 이 드라이버의 debug 기능 핸들.
 * 설정자: scm_blk.c 의 모듈 초기화가 만든다.
 * 읽는 자: 아래 로그 매크로 셋 전부.
 * 값 범위: 유효한 핸들. 만들지 못하면 모듈이 올라오지 않는다.
 * 동기화: 초기화 이후 불변이며, debug 기능 자체가 내부 잠금을 갖는
 * 것으로 보이나 근거는 이 트리에서 확인 못 함. */
extern debug_info_t *scm_debug;

/* [한국어] 문자열 하나를 debug 기능에 기록한다.
 * do-while(0) 로 감싸는 것은 이 매크로를 if 문의 본문 자리에 그대로
 * 써도 안전하게 하기 위한 관용이다 — 그러지 않으면 뒤따르는 else 가
 * 짝을 잃는다.
 * 줄 끝이 역슬래시로 이어지므로 이 설명을 매크로 위에 블록으로 둔다. */
#define SCM_LOG(imp, txt) do {					\
		debug_text_event(scm_debug, imp, txt);		\
	} while (0)

/* [한국어]
 * SCM_LOG_HEX - 임의의 바이트 덩어리를 s390 debug 기능에 기록한다
 *
 * @level: 기록 수준. 값이 클수록 상세한 기록이며, 낮은 수준만 남기도록
 *         설정된 시스템에서는 걸러진다.
 * @data: 기록할 바이트의 시작 주소.
 * @length: 기록할 바이트 수.
 *
 * 바로 위의 SCM_LOG 매크로가 **문자열** 을 남기는 데 비해, 이쪽은
 * **바이너리 덩어리** 를 그대로 남긴다. 구조체 하나를 통째로 찍고 싶을 때
 * 쓰며, 실제로 아래 SCM_LOG_STATE 가 그 용도로 이 함수를 부른다.
 *
 * 매크로가 아니라 static inline 함수인 이유는 인자가 셋이고 그중 둘이
 * 값으로 넘어가기 때문으로 보인다 — 부작용이 있는 식을 인자로 받아도
 * 한 번만 평가된다.
 *
 * s390 의 debug 기능은 커널 안에 순환 버퍼를 두고 나중에 debugfs 로 꺼내
 * 보는 구조인데, 그 구현은 asm/debug.h 소관이라 이 트리에서 확인 못 함.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 문맥 양쪽에서 불린다.
 * debug_event() 자체가 두 문맥을 모두 견디는 것으로 보이나, 근거는
 * 이 트리에서 확인 못 함.
 *
 * 에러 경로: 없다. debug_event() 의 반환값을 확인하지 않는다.
 *
 * 호출 체인:
 *   SCM_LOG_STATE() / scm_blk.c 의 오류 기록 → [이 함수] → debug_event()
 */
static inline void SCM_LOG_HEX(int level, void *data, int length)
{
	debug_event(scm_debug, level, data, length);
}

/* [한국어]
 * SCM_LOG_STATE - SCM 장치의 현재 상태를 한 줄 기록으로 남긴다
 *
 * @level: 기록 수준. 위 SCM_LOG_HEX 로 그대로 넘어간다.
 * @scmdev: 상태를 찍을 SCM 장치.
 *
 * 장치의 상태를 사람이 읽는 문자열이 아니라 **10바이트 이진 기록** 으로
 * 남긴다. 나중에 debugfs 로 꺼내 볼 때 고정 폭이라 해석하기 쉽고, 기록
 * 공간도 적게 든다.
 *
 * 핵심은 **함수 안에서 임시 구조체를 만들어 세 값을 모으는 것** 이다.
 * 장치 구조체를 통째로 찍으면 필요 없는 부분까지 들어가므로, 관심 있는
 * 셋만 골라 붙여 담는다.
 *   - address    : 이 SCM 증분의 주소. 어느 장치인지 가려 준다.
 *   - oper_state : 동작 상태.
 *   - rank       : 등급.
 *
 * `__packed` 가 붙어 있는 것이 중요하다. 그것이 없으면 컴파일러가 u64
 * 뒤에 정렬용 빈 공간을 넣어, 기록 길이가 10 이 아니라 16 이 되고
 * 그 빈 공간에 스택의 옛 내용이 실려 나간다.
 *
 * 뒤의 두 필드의 값이 각각 무엇을 뜻하는지는 asm/eadm.h 소관이라
 * 이 트리에서 확인 못 함. 다만 scm_drv.c 의 probe 가 oper_state 를
 * OP_STATE_GOOD 과 비교하는 것으로 보아, 장치가 쓸 만한지를 나타내는
 * 열거값임은 확인된다.
 *
 * 실행 컨텍스트: scm_drv.c 의 알림 처리와 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   scm_notify() / scm_probe() → [이 함수] → SCM_LOG_HEX() → debug_event()
 */
static inline void SCM_LOG_STATE(int level, struct scm_device *scmdev)
{
	/* [한국어] 기록에 담을 세 값을 모을 **이름 없는 임시 구조체.** 장치 구조체를
	 * 통째로 찍지 않고 관심 있는 셋만 골라 붙여 담는다. */
	struct {
		/* [한국어] 이 SCM 증분의 주소. 기록을 보고 어느 장치인지 가리는 열쇠다. */
		u64 address;
		/* [한국어] 동작 상태. scm_drv.c 의 probe 가 OP_STATE_GOOD 과 비교하는 것으로
		 * 보아 장치가 쓸 만한지를 나타내는 열거값이며, 값의 목록은 asm/eadm.h
		 * 소관이라 이 트리에서 확인 못 함. */
		u8 oper_state;
		/* [한국어] 등급. 뜻은 이 트리에서 확인 못 함. */
		u8 rank;
	/* [한국어] `__packed` 가 핵심이다. 없으면 컴파일러가 u64 뒤에 정렬용 빈
	 * 공간을 넣어 기록 길이가 10 이 아니라 16 이 되고, 그 빈 공간에 스택의
	 * 옛 내용이 실려 나간다. */
	} __packed data = {
		/* [한국어] 장치 주소를 그대로 담는다. */
		.address = scmdev->address,
		/* [한국어] 동작 상태를 담는다. 장치 구조체의 attrs 안에 있다. */
		.oper_state = scmdev->attrs.oper_state,
		/* [한국어] 등급도 같은 자리에서 가져온다. */
		.rank = scmdev->attrs.rank,
	};

	/* [한국어] 모은 10바이트를 통째로 기록한다. sizeof 로 길이를 구하므로,
	 * 위 구조체에 필드를 더해도 이 줄은 그대로다. */
	SCM_LOG_HEX(level, &data, sizeof(data));
}

#endif /* SCM_BLK_H */
