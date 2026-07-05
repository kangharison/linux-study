/* SPDX-License-Identifier: GPL-2.0 */

/*
 * [한국어 설명] 블록 계층 request_queue의 runtime PM(전원 관리) 판정 인라인 헬퍼 (block/blk-pm.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 struct request_queue와 그에 연결된 struct device의 runtime PM
 * (Power Management) 상태를 "판정"만 하는 인라인 함수 2개,
 * blk_pm_resume_queue()와 blk_pm_mark_last_busy()를 선언한다. 여기서
 * "판정"이라 부르는 이유는 이 파일이 실제 suspend/resume 절차 자체를
 * 구현하지 않고 (그 구현은 block/blk-pm.c에 있다), "지금 이 큐에 I/O를
 * 들여보내도 되는가?"와 "이 I/O가 끝났으니 idle 타이머를 늦춰도 되는가?"
 * 라는 두 질문에만 답하기 때문이다. CONFIG_PM 커널 옵션이 켜져 있으면
 * struct request_queue에 dev/rpm_status 필드가 실제로 존재하여 두
 * 함수가 진짜 판정 로직을 수행하고, CONFIG_PM이 꺼져 있으면 두 필드
 * 자체가 컴파일되지 않으므로 이 헤더의 #else 분기가 아무 일도 하지 않는
 * 상수 반환/빈 함수로 대체되어 PM 관련 코드가 완전히 사라진다. 이런
 * 구조 덕분에 blk-core.c/blk-mq.c는 CONFIG_PM 여부를 신경 쓰지 않고
 * 항상 blk_pm_resume_queue()/blk_pm_mark_last_busy()를 그대로 호출할 수
 * 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 블록 계층 I/O 제출/완료 경로의 맨 앞과 맨 뒤에 위치하는 게이트키퍼다.
 * 제출 경로에서는 blk_mq_get_request()/bio_queue_enter()가 각각
 * blk_queue_enter()/__bio_queue_enter()(둘 다 block/blk-core.c)를 부르고,
 * 그 안의 wait_event(q->mq_freeze_wq, ... blk_pm_resume_queue(pm, q) ...)
 * 조건식에서 이 파일의 blk_pm_resume_queue()가 호출된다. 큐가 suspend
 * 상태이면 이 함수가 pm_request_resume()을 발행하고 0을 반환하여
 * wait_event()가 계속 잠들게 만들고, 이후 PM 코어가 디바이스를 깨워
 * block/blk-pm.c의 blk_post_runtime_resume()이 rpm_status를 RPM_ACTIVE로
 * 되돌리며 mq_freeze_wq를 깨우면 조건이 재평가되어 비로소 I/O가
 * 들어간다. 완료 경로에서는 blk_mq_end_request() 근처(block/blk-mq.c)에서
 * blk_pm_mark_last_busy()가 호출되어, I/O가 끝날 때마다 runtime PM의
 * idle(last_busy) 타이머를 현재 시각으로 리셋한다. 실행 컨텍스트는
 * 제출측(blk_queue_enter를 호출하는 임의의 프로세스 컨텍스트, NOWAIT가
 * 아니면 sleep 가능)과 완료측(인터럽트/softirq 또는 폴링 컨텍스트에서
 * 실행되는 blk_mq_end_request) 양쪽이며, 이 파일 자체는 커널 모듈이
 * 아니라 블록 계층 코어의 일부로서 항상 커널 스페이스에서 실행된다.
 * NVMe로 예를 들면, 컨트롤러가 PCIe D3/APST 같은 저전력 상태로 들어가
 * 있을 때 SQ(Submission Queue) tail doorbell을 두드리면 안 되므로,
 * blk_pm_resume_queue()가 그 전에 컨트롤러를 깨우는 문지기 역할을 한다.
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더가 의존하는 것은 <linux/pm_runtime.h>가 제공하는
 * pm_request_resume(), pm_runtime_mark_last_busy() 두 PM 코어 API와,
 * <linux/blkdev.h>가 정의하는 struct request_queue의 dev/rpm_status/
 * pm_only 필드, <linux/blk-mq.h>가 정의하는 struct request의 rq_flags
 * 중 RQF_PM 비트뿐이다. 이 헤더에 의존하는 쪽(호출자)은
 * block/blk-core.c(blk_queue_enter, __bio_queue_enter)와
 * block/blk-mq.c(blk_mq_end_request 부근)이며, 실제 suspend/resume
 * 시퀀스를 구현하는 block/blk-pm.c(blk_pre_runtime_suspend,
 * blk_post_runtime_resume, blk_set_pm_only/blk_clear_pm_only)와 짝을
 * 이루어 동작한다. 더 아래로는 NVMe/SCSI 등 블록 드라이버가
 * struct device에 등록한 runtime_suspend/runtime_resume PM 콜백
 * (drivers/nvme/host/pci.c의 nvme_runtime_suspend/nvme_runtime_resume
 * 등)이 blk_pre_runtime_suspend()/blk_post_runtime_resume()을 호출하는
 * 형태로 최종 연결된다. 데이터 흐름 관점에서 핵심 공유 상태는
 * q->dev(디바이스 포인터), q->rpm_status(enum rpm_status:
 * RPM_ACTIVE/RPM_RESUMING/RPM_SUSPENDED 등), q->pm_only(atomic_t
 * 카운터, blk_queue_pm_only()가 0보다 큰지 검사), rq->rq_flags의
 * RQF_PM 비트 네 가지이며, 이 헤더는 이 상태들을 "읽기만" 하거나 PM
 * 코어 API를 통해 변경을 "요청"할 뿐, NVMe 컨트롤러의 doorbell/CC/CSTS
 * 레지스터를 직접 건드리지는 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - blk_pm_resume_queue(pm, q) [CONFIG_PM]: I/O 진입 시도 시 큐가 PM
 *   대상이 아니거나 이미 활성 상태(혹은 자기 자신이 PM 요청)이면 1을
 *   반환해 즉시 통과시키고, RPM_SUSPENDED 상태의 큐에 일반 I/O가
 *   들어오려 하면 pm_request_resume()을 호출한 뒤 0을 반환해 대기시킨다.
 * - blk_pm_mark_last_busy(rq) [CONFIG_PM]: I/O 완료 시 RQF_PM이 아닌
 *   일반 request에 한해 pm_runtime_mark_last_busy()로 autosuspend
 *   idle 타이머를 리셋해, 진성 I/O 트래픽만 "바쁨" 신호로 반영한다.
 * - blk_pm_resume_queue(pm, q) [CONFIG_PM 미설정]: 판정 자체를 생략하고
 *   항상 1을 반환하는 no-op — PM 관련 필드가 컴파일되지 않으므로
 *   컴파일 타임에 오버헤드가 완전히 제거된다.
 * - blk_pm_mark_last_busy(rq) [CONFIG_PM 미설정]: 아무 것도 하지 않는
 *   빈 함수 — 링커가 호출부까지 통째로 최적화 제거할 수 있다.
 * - 이 파일이 새로 정의하는 구조체/enum은 없다. struct request_queue의
 *   dev/rpm_status/pm_only, struct request의 rq_flags(RQF_PM)를 판정
 *   대상으로 삼을 뿐이다.
 */

#ifndef _BLOCK_BLK_PM_H_
/* [한국어] 헤더 중복 include 방지 매크로. block/blk-core.c와
 * block/blk-mq.c 양쪽에서 "blk-pm.h"를 include하므로, 매크로가 없으면
 * 구조체/함수 재정의 컴파일 에러가 발생할 수 있다. */
#define _BLOCK_BLK_PM_H_
/* [한국어] 위 #ifndef의 가드 심벌을 정의해, 두 번째 include부터는
 * 아래 내용 전체가 전처리기에 의해 건너뛰어지도록 한다. */

#include <linux/pm_runtime.h>
/* [한국어] pm_request_resume(), pm_runtime_mark_last_busy(),
 * pm_runtime_use_autosuspend() 등 PM 코어(runtime PM framework) API와
 * enum rpm_status(RPM_ACTIVE/RPM_RESUMING/RPM_SUSPENDED 등) 정의를
 * 가져온다. 이 헤더의 두 함수가 실제로 호출하는 PM 코어 API가 모두
 * 여기서 선언되므로 반드시 필요하다. */

#ifdef CONFIG_PM
/* [한국어] CONFIG_PM: 커널 전역 전원 관리(런타임 PM + 시스템 슬립) 지원
 * 여부를 결정하는 컴파일 옵션. 이 옵션이 켜진 커널에서만
 * struct request_queue의 dev/rpm_status 필드(include/linux/blkdev.h,
 * #ifdef CONFIG_PM 블록 내부)가 실제로 존재하므로, 이 분기에서만
 * 진짜 PM 판정 로직을 컴파일한다. */
/*
 * [한국어]
 * blk_pm_resume_queue - request_queue가 속한 디바이스의 runtime PM 상태를 확인하고, suspend 상태면 resume을 요청한다
 *
 * @pm: 이번 진입 시도 자체가 PM(전원 관리) 경로 요청인지 여부.
 *      blk_queue_enter()가 flags & BLK_MQ_REQ_PM을 뽑아 전달하는 값으로,
 *      true면 이 요청은 곧 rq->rq_flags에 RQF_PM이 세팅될 "PM 내부
 *      요청"이므로 큐가 suspend 상태여도 rpm_status 검사 없이 통과시켜야
 *      한다(그래야 resume 시퀀스 자체가 진행될 수 있다). false면 일반
 *      I/O이므로 rpm_status가 RPM_SUSPENDED가 아닌지 확인해야 한다.
 * @q:  I/O가 진입하려는 request_queue. CONFIG_PM이 켜진 커널에서는
 *      struct request_queue에 dev(연결된 struct device)와 rpm_status
 *      (현재 runtime PM 상태) 필드가 실제로 존재한다
 *      (include/linux/blkdev.h). NVMe라면 q는 네임스페이스의
 *      request_queue이고 q->dev는 NVMe 컨트롤러의 PCI struct device다.
 * @return: 1이면 지금 바로 I/O 진입을 허용해도 됨(호출자의 wait_event
 *          조건이 즉시 참이 되어 대기 없이 진행), 0이면 아직 resume이
 *          필요한 상태이므로 호출자가 계속 대기(wait_event에서 잠듦)해야
 *          함.
 *
 * 배경: 이 함수는 자기 자신이 잠들지 않는다. block/blk-core.c의
 * blk_queue_enter()/__bio_queue_enter()가 wait_event(q->mq_freeze_wq, ...)
 * 의 "깨어날 조건"의 일부로 이 함수를 직접 호출한다. 즉 mq_freeze_wq가
 * 깨어날 때마다(예: block/blk-pm.c의 blk_clear_pm_only()가 pm_only
 * 카운터를 0으로 내리며 wake_up_all()할 때) 이 함수가 다시 평가되는
 * 구조이며, 이 함수는 그 순간의 상태만 보고 즉시 값을 반환한다.
 *
 * 동작 순서:
 *   1) q->dev가 NULL이거나 blk_queue_pm_only(q)(pm_only 카운터가 0)이면,
 *      이 큐는 애초에 runtime PM 대상이 아니거나 지금 PM 전용 모드가
 *      아니므로 더 볼 것 없이 1을 반환한다.
 *   2) pm이 true(PM 내부 요청)이거나 rpm_status가 RPM_SUSPENDED가
 *      아니면(RPM_ACTIVE 또는 RPM_RESUMING 등) 1을 반환해 진입을
 *      허용한다.
 *   3) 그 외(=일반 I/O가 RPM_SUSPENDED 상태의 큐로 들어오려는 상황)에는
 *      pm_request_resume(q->dev)로 PM 코어에 비동기 runtime resume을
 *      예약하고 0을 반환해, 호출자가 resume 완료까지 대기하게 만든다.
 *
 * NVMe 관점: 컨트롤러가 저전력(runtime suspend, 예: PCIe D3/APST) 상태일
 * 때 곧바로 SQ(Submission Queue) tail doorbell을 두드리면 컨트롤러가
 * 응답하지 못하므로, 이 함수의 pm_request_resume() 호출이
 * PM 코어 -> (드라이버의 runtime_resume 콜백) -> block/blk-pm.c의
 * blk_post_runtime_resume()을 거쳐 컨트롤러가 CC.EN=1/CSTS.RDY=1로
 * 돌아온 뒤에야 doorbell 경로가 열리도록 순서를 강제하는 문지기 역할을
 * 한다.
 *
 * 실행 컨텍스트: blk_queue_enter()/__bio_queue_enter() 호출자와 동일한
 * 임의의 프로세스 컨텍스트에서 실행된다(NOWAIT가 아니면 그 컨텍스트가
 * sleep 가능해야 함). 이 함수 자체는 락을 잡지 않으며, rpm_status
 * 갱신 쪽의 동기화는 block/blk-pm.c에서 q->queue_lock으로 보호한다.
 * 에러 경로: 이 함수는 실패를 표현하지 않는다(항상 0 또는 1) — resume
 * 요청 실패 시의 처리는 PM 코어와 blk_pre_runtime_suspend() 쪽 책임이다.
 *
 * 호출 체인:
 *   blk_mq_get_request/bio_queue_enter → blk_queue_enter/__bio_queue_enter
 *   → wait_event 조건식 → [blk_pm_resume_queue] → pm_request_resume(q->dev)
 *   → (PM 코어 비동기 워크) → 드라이버 runtime_resume 콜백
 *   → blk_post_runtime_resume → blk_clear_pm_only → wake_up_all(mq_freeze_wq)
 *   → wait_event 조건 재평가 → [blk_pm_resume_queue] (이번엔 1 반환)
 */
static inline int blk_pm_resume_queue(const bool pm, struct request_queue *q)
{
	/* [한국어] q->dev: 이 request_queue에 연결된 struct device 포인터
	 * (CONFIG_PM 블록에서만 존재하는 필드). NULL이면
	 * blk_pm_runtime_init()이 아직 호출되지 않았거나(초기화 이전) 애초에
	 * runtime PM을 쓰지 않는 큐라는 뜻이다 — NVMe라면 probe 초기 단계나
	 * PM을 지원하지 않는 구성일 수 있다.
	 * [한국어] blk_queue_pm_only(q): #define blk_queue_pm_only(q)
	 * atomic_read(&(q)->pm_only) — pm_only 카운터가 0보다 큰지 검사한다.
	 * 이 카운터는 block/blk-pm.c의 blk_set_pm_only()/blk_clear_pm_only()
	 * 가 증감시키며, 0보다 크면 "지금은 PM 전용 요청(RQF_PM)만 통과시켜야
	 * 하는 구간"이라는 뜻이다. */
	if (!q->dev || !blk_queue_pm_only(q))
		return 1;	/* Nothing to do */
		/* [한국어] PM 대상이 아니거나(q->dev NULL) 지금 PM 전용 모드가
		 * 아니면(pm_only == 0) 검사할 것이 없으므로 즉시 통과시킨다.
		 * NVMe 관점에서 이 경로는 doorbell 접근에 아무 제약이 없는
		 * 평상시 I/O 흐름이다. */

	/* [한국어] pm: 위에서 설명한 대로 "이 진입 자체가 PM 요청인지" 여부.
	 * [한국어] q->rpm_status != RPM_SUSPENDED: enum rpm_status 값이
	 * RPM_SUSPENDED가 아닌지(RPM_ACTIVE/RPM_RESUMING 등) 검사한다.
	 * pm이 false이면 좌변이 거짓이 되어 이 if 전체를 건너뛰고 아래로
	 * 내려간다(즉 rpm_status 값과 무관하게 재검사가 필요한 경로로
	 * 진행). */
	if (pm && q->rpm_status != RPM_SUSPENDED)
		return 1;	/* Request allowed */
		/* [한국어] 이번 요청이 PM 내부 요청(pm==true)이면서 큐가 이미
		 * suspend 상태가 아니면(활성/재개중) 곧바로 통과시킨다. PM
		 * 내부 요청은 resume 시퀀스 자체를 구성하는 요청일 수 있으므로
		 * 여기서 막으면 데드락(자기 자신을 깨우는 요청이 자기 자신
		 * 때문에 막히는 상황)이 될 수 있다. */

	/* [한국어] 여기까지 내려왔다는 것은: 큐가 PM 대상이고 PM 전용
	 * 모드이며(pm_only>0), 그리고 (pm이 false인 일반 I/O이거나) 큐가
	 * 여전히 RPM_SUSPENDED 상태라는 뜻이다 — 즉 "일반 I/O가 잠든 큐를
	 * 깨워야 하는" 상황이다. */
	pm_request_resume(q->dev);
	/* [한국어] PM 코어에 비동기 runtime resume을 예약한다. 이 호출은
	 * 즉시 반환하며(여기서 sleep하지 않는다), 실제 resume은 PM 코어의
	 * 워크큐에서 디바이스의 runtime_resume 콜백(NVMe라면
	 * nvme_runtime_resume 계열)을 통해 별도로 수행된다. */
	return 0;
	/* [한국어] resume이 끝나기 전까지 이번 진입은 보류시킨다. 호출자인
	 * blk_queue_enter()/__bio_queue_enter()의 wait_event() 조건식이 이
	 * 반환값(0)을 보고 계속 잠들어 있다가, resume 완료 후
	 * blk_post_runtime_resume()이 rpm_status를 RPM_ACTIVE로 되돌리고
	 * pm_only 카운터가 0이 되어 mq_freeze_wq를 깨우면 이 함수가 다시
	 * 호출되어 이번엔 1을 반환하게 된다. */
}

/*
 * [한국어]
 * blk_pm_mark_last_busy - I/O 완료 시 디바이스의 마지막 활동(last_busy) 시각을 갱신해 autosuspend 타이머를 늦춘다
 *
 * @rq: 방금 완료 처리된 struct request. blk_mq_end_request() 등
 *      완료 경로에서 전달되며, rq->q로 소속 request_queue를,
 *      rq->rq_flags로 RQF_PM 여부를 확인할 수 있다. NVMe에서는 CID
 *      (Command Identifier)로 CQ(Completion Queue) 엔트리와 매핑된
 *      request다.
 * @return: 없음(void). 성공/실패 개념이 없는 상태 갱신용 함수다.
 *
 * 배경: runtime PM의 autosuspend는 "마지막으로 바빴던 시각(last_busy)"
 * 로부터 일정 시간(autosuspend_delay)이 지나면 자동으로 suspend를
 * 시도한다. I/O가 실제로 처리되고 있는 동안에도 이 시각을 갱신해주지
 * 않으면, 트래픽이 있어도 디바이스가 중간에 suspend를 시도해 버릴 수
 * 있다. 이 함수는 매 I/O 완료마다 그 시각을 지금(now)으로 되돌려,
 * 실제로 idle해질 때까지 autosuspend가 미뤄지도록 한다.
 *
 * 동작 순서:
 *   1) rq->q->dev가 없으면(runtime PM 대상이 아닌 큐) 아무 것도 하지
 *      않는다.
 *   2) rq->rq_flags에 RQF_PM 비트가 서 있으면(이 request 자체가 PM
 *      경로에서 발행된 요청, block/blk-mq.c의
 *      "if (data->flags & BLK_MQ_REQ_PM) data->rq_flags |= RQF_PM;"
 *      로 설정됨) last_busy 갱신에서 제외한다 — PM 내부 요청까지
 *      "바쁨"으로 집계하면 유휴 판정이 왜곡될 수 있기 때문이다.
 *   3) 그 외의 일반 I/O 완료라면 pm_runtime_mark_last_busy(rq->q->dev)
 *      로 PM 코어의 last_busy 타임스탬프를 현재 시각으로 갱신한다.
 *
 * NVMe 관점: 컨트롤러로부터 CQ 완료 인터럽트를 받아 nvme_complete_rq()
 * 가 request를 끝낼 때마다 이 함수가 호출되므로, I/O가 계속 들어오는
 * 동안에는 last_busy가 끊임없이 갱신되어 APST(Autonomous Power State
 * Transition)/RTD3 같은 저전력 전환이 지연된다. I/O가 끊기면 그 순간의
 * last_busy를 기준으로 autosuspend_delay 후 suspend가 시도된다.
 *
 * 실행 컨텍스트: 완료 경로이므로 인터럽트 컨텍스트, softirq(블록
 * 계층 IRQ-safe completion), 또는 폴링(io_uring/blk_rq_poll) 컨텍스트
 * 등 완료 처리가 이루어지는 다양한 컨텍스트에서 호출될 수 있다.
 * pm_runtime_mark_last_busy() 내부는 이런 컨텍스트에서 호출 가능하도록
 * 설계되어 있다(락 없이 타임스탬프만 갱신).
 * 에러 경로: 없음 — 실패할 수 없는 상태 갱신이다.
 *
 * 호출 체인:
 *   nvme_irq → nvme_process_cq → nvme_complete_rq → blk_mq_end_request
 *   → [blk_pm_mark_last_busy] → pm_runtime_mark_last_busy(rq->q->dev)
 */
static inline void blk_pm_mark_last_busy(struct request *rq)
{
	/* [한국어] rq->q->dev: 이 request가 속한 request_queue에 연결된
	 * struct device(NULL이면 runtime PM 대상이 아닌 큐).
	 * [한국어] RQF_PM: <linux/blk-mq.h>에 정의된 request 플래그 비트로,
	 * blk_mq_rq_ctx_init()에서 "BLK_MQ_REQ_PM 플래그로 할당된 request"
	 * 에 한해 세팅된다 — 즉 이 request 자체가 PM 절차의 일부임을
	 * 나타낸다. */
	if (rq->q->dev && !(rq->rq_flags & RQF_PM))
		/* [한국어] dev가 있고(runtime PM 대상) RQF_PM이 아닌(일반
		 * I/O) 경우에만 진입 — PM 내부 요청은 idle 판정을 왜곡하지
		 * 않도록 여기서 걸러낸다. */
		pm_runtime_mark_last_busy(rq->q->dev); /* autosuspend 타이머 리셋 */
		/* [한국어] PM 코어의 dev->power.last_busy를 ktime_get()
		 * 기준 현재 시각으로 갱신한다. 이후 autosuspend 타이머가
		 * 이 시각 + autosuspend_delay를 기준으로 재계산되어,
		 * 새 I/O가 계속 들어오는 한 실제 suspend 시도가 미뤄진다. */
}
#else
/* [한국어] CONFIG_PM이 꺼진 커널: struct request_queue에 dev/rpm_status
 * 필드 자체가 존재하지 않으므로(위 CONFIG_PM 블록 참고), 이 필드들을
 * 참조하는 판정 로직도 전혀 컴파일할 수 없다. 그래서 이 분기는 필드
 * 접근 없이 "PM은 신경 쓸 필요 없다"는 결과만 즉시 돌려주는 상수 버전
 * 함수로 대체된다. 임베디드 등 runtime PM이 필요 없는 구성에서 이
 * 옵션을 끄면, 호출부(blk-core.c/blk-mq.c)는 코드 변경 없이 그대로
 * 컴파일되고 이 판정에 드는 오버헤드만 사라진다. */
/*
 * [한국어]
 * blk_pm_resume_queue - (CONFIG_PM 미설정) runtime PM 판정을 생략하고 항상 진입을 허용한다
 *
 * @pm: CONFIG_PM 버전과 동일한 자리의 매개변수이지만, 본문에서 전혀
 *      참조되지 않는다 — PM 판정 자체가 없으므로 값과 무관하다.
 * @q:  CONFIG_PM 버전과 동일한 자리의 매개변수이지만, 본문에서 전혀
 *      참조되지 않는다 — dev/rpm_status 필드가 존재하지 않아 q의 PM
 *      관련 필드를 볼 수 없다.
 * @return: 항상 1. "즉시 진입 허용"이라는 뜻으로, 호출자의 wait_event
 *          조건식이 이 함수 호출만으로 곧바로 참이 된다.
 *
 * 배경: CONFIG_PM이 꺼지면 struct request_queue에 dev/rpm_status
 * 필드가 없으므로, 이 함수는 그 필드들을 참조하지 않고 무조건 1을
 * 반환하는 상수 함수가 된다. 컴파일러는 이 인라인 함수를 호출부에서
 * 사실상 리터럴 1로 치환할 수 있어, PM 판정에 따른 런타임 오버헤드가
 * 완전히 사라진다.
 * 실행 컨텍스트: CONFIG_PM 버전과 동일하게 blk_queue_enter()/
 * __bio_queue_enter()의 wait_event 조건식에서 호출되지만, 이 버전에서는
 * 결과가 항상 참이므로 wait_event가 사실상 즉시 통과한다.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   blk_mq_get_request/bio_queue_enter → blk_queue_enter/__bio_queue_enter
 *   → wait_event 조건식 → [blk_pm_resume_queue] (즉시 1 반환, 대기 없음)
 */
static inline int blk_pm_resume_queue(const bool pm, struct request_queue *q)
{
	return 1;
	/* [한국어] CONFIG_PM=n 이므로 runtime PM/APST/ASPM 판정 대상 필드가
	 * 아예 없다. 디바이스는 probe 시 설정된 전원 상태를 그대로 유지하며,
	 * doorbell/SQ/CQ 접근에 이 헤더로 인한 추가 제약이 없다는 뜻으로
	 * 항상 1(즉시 허용)을 반환한다. */
}

/*
 * [한국어]
 * blk_pm_mark_last_busy - (CONFIG_PM 미설정) autosuspend 타이머가 없으므로 아무 것도 하지 않는다
 *
 * @rq: CONFIG_PM 버전과 동일한 자리의 매개변수이지만, 본문에서 전혀
 *      참조되지 않는다 — last_busy 개념 자체가 없으므로 rq의 내용을
 *      볼 필요가 없다.
 * @return: 없음(void). 아무 부수효과도 없다.
 *
 * 배경: CONFIG_PM이 꺼지면 runtime PM 프레임워크의 autosuspend
 * 타이머 개념 자체가 존재하지 않으므로, "마지막 활동 시각을 갱신"할
 * 대상이 없다. 이 함수는 몸체가 완전히 비어 있어 컴파일러가 호출부를
 * 통째로 최적화 제거할 수 있다.
 * 실행 컨텍스트: CONFIG_PM 버전과 동일한 완료 경로(인터럽트/softirq/
 * 폴링)에서 호출될 수 있지만, 본문이 없으므로 실제로는 아무 지연이나
 * 부수효과도 발생시키지 않는다.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   nvme_irq → nvme_process_cq → nvme_complete_rq → blk_mq_end_request
 *   → [blk_pm_mark_last_busy] (아무 동작 없이 즉시 반환)
 */
static inline void blk_pm_mark_last_busy(struct request *rq)
{
	/* [한국어] CONFIG_PM=n 환경에서는 완료 시점에 조정할 autosuspend
	 * 타이머가 없으므로 no-op. nvme_complete_rq -> blk_mq_end_request
	 * 경로에서 추가 PM 지연 없이 request 해제 및 CID/tag 반환이
	 * 곧바로 이어진다. */
}
#endif
/* [한국어] #ifdef CONFIG_PM 분기의 끝. 이 지점 이후로는 CONFIG_PM 값에
 * 관계없이 blk_pm_resume_queue()/blk_pm_mark_last_busy()라는 동일한
 * 이름과 시그니처의 인라인 함수 한 쌍만 남아 있으므로, 이 헤더를
 * include하는 blk-core.c/blk-mq.c는 CONFIG_PM 여부를 몰라도 된다. */

#endif /* _BLOCK_BLK_PM_H_ */
