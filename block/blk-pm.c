// SPDX-License-Identifier: GPL-2.0

/*
 * [한국어 설명] 블록 계층 request_queue의 런타임 전원 관리(Runtime PM) 상태 전이 구현체 (block/blk-pm.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 struct request_queue 단위로 런타임 PM(Power Management, 시스템 전체는
 * 켜져 있는 상태에서 유휴한 개별 디바이스만 자동으로 저전력 상태로 전환하는 기능)
 * 상태 전이 시퀀스를 구현한다. 짝이 되는 헤더 block/blk-pm.h가 "지금 이 큐에 I/O를
 * 들여보내도 되는가"만 판정하는 인라인 헬퍼(blk_pm_resume_queue, blk_pm_mark_last_busy)를
 * 제공하는 반면, 이 파일은 실제로 q->rpm_status를 RPM_ACTIVE -> RPM_SUSPENDING ->
 * RPM_SUSPENDED -> RPM_RESUMING -> RPM_ACTIVE 순서로 옮기는 5개의 EXPORT_SYMBOL
 * 함수(blk_pm_runtime_init, blk_pre_runtime_suspend, blk_post_runtime_suspend,
 * blk_pre_runtime_resume, blk_post_runtime_resume)를 제공한다. 이 함수들은 디바이스
 * 드라이버의 struct dev_pm_ops.runtime_suspend/runtime_resume 콜백 안에서 호출되도록
 * 설계된 "블록 레이어 쪽 절반"이며, 실제 하드웨어 저전력 진입/탈출(레지스터 접근,
 * 링크 상태 전환 등)은 다루지 않는다. NVMe로 예를 들면, 컨트롤러가 PCIe D3/APST
 * (Autonomous Power State Transition) 같은 저전력 상태로 들어가기 전에, 이미
 * 제출되어 SQ(Submission Queue)/CQ(Completion Queue)를 오가는 중인 명령이 하나도
 * 남지 않았는지를 q->q_usage_counter로 확인하는 관문 역할을 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 순서는 "PM 코어 -> 디바이스 드라이버의 runtime_suspend/runtime_resume 콜백
 * -> 이 파일의 함수 -> blk-mq.c/blk-core.c의 큐 동결·pm_only 카운터 조작" 순으로
 * 이어진다. 드라이버는 큐 생성 직후(probe/reset 경로) blk_pm_runtime_init()을 호출해
 * q->dev를 연결하고 rpm_status를 RPM_ACTIVE로 초기화한다. 이후 시스템이 유휴해져
 * PM 코어가 runtime_suspend를 트리거하면, 드라이버 콜백이 맨 앞에서
 * blk_pre_runtime_suspend(q)를 호출해 "지금 이 큐를 통해 진행 중인 요청이 없는지"를
 * 확인하고(0이면 허용, -EBUSY면 보류), 실제 하드웨어를 내린 뒤 맨 끝에서
 * blk_post_runtime_suspend(q, err)를 호출해 성공(RPM_SUSPENDED)/실패(RPM_ACTIVE)를
 * 확정한다. 반대로 깨어날 때는 드라이버의 runtime_resume 콜백이 시작 부분에서
 * blk_pre_runtime_resume(q)로 RPM_RESUMING 표시를 남기고, 실제 하드웨어를 깨운 뒤
 * 끝에서 blk_post_runtime_resume(q)로 다시 RPM_ACTIVE로 되돌리며 pm_only 카운터를
 * 해제해 대기 중이던 일반 I/O를 재개시킨다. 실행 컨텍스트는 PM 코어의 워크큐
 * (pm_wq)에서 실행되는 드라이버 콜백 내부이며, sleep이 가능한 프로세스 컨텍스트다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 모듈: <linux/pm_runtime.h>가 제공하는
 * pm_runtime_set_autosuspend_delay(), pm_runtime_use_autosuspend(),
 * pm_runtime_mark_last_busy(), pm_request_autosuspend() 등 PM 코어 API. 같은
 * 디렉터리의 "blk-mq.h"가 제공하는 blk_freeze_queue_start(),
 * blk_mq_unfreeze_queue_nomemrestore()(둘 다 block/blk-mq.c 구현, q->q_usage_counter를
 * kill/resurrect). block/blk-core.c가 제공하는 blk_set_pm_only()/blk_clear_pm_only()
 * (q->pm_only 카운터 증감과 mq_freeze_wq wake_up_all). percpu_ref_switch_to_atomic_sync()/
 * percpu_ref_is_zero()는 percpu-refcount 서브시스템이 제공하는 q->q_usage_counter
 * 조작 API다. 이 파일에 의존하는 모듈: NVMe(drivers/nvme/host/pci.c의
 * nvme_runtime_suspend/nvme_runtime_resume), SCSI, USB 스토리지 등 request 기반
 * runtime PM을 지원하는 모든 블록 드라이버가 이 파일의 5개 EXPORT_SYMBOL 함수를
 * 자신의 struct dev_pm_ops 콜백 안에서 직접 호출한다. block/blk-pm.h의
 * blk_pm_resume_queue()/blk_pm_mark_last_busy()는 이 파일이 갱신하는
 * q->rpm_status/q->pm_only를 "읽기만" 하는 짝 헬퍼다. 공유하는 핵심 자료구조는
 * struct request_queue(include/linux/blkdev.h)의 dev(연결된 struct device),
 * rpm_status(enum rpm_status: RPM_ACTIVE/RPM_RESUMING/RPM_SUSPENDING/RPM_SUSPENDED),
 * pm_only(atomic_t), q_usage_counter(percpu_ref, blk_queue_enter()로 진입한 in-flight
 * 참조 집계), queue_lock(rpm_status 갱신을 보호하는 스핀락) 다섯 가지이며, 실제
 * NVMe doorbell/CC/CSTS 레지스터 조작은 이 파일이 아니라 drivers/nvme/host/
 * 아래의 드라이버에서 수행된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - blk_pm_runtime_init(q, dev): q->dev를 연결하고 rpm_status를 RPM_ACTIVE로
 *   초기화, autosuspend delay를 -1로 걸어 명시적으로 값이 설정되기 전까지는
 *   자동 suspend가 걸리지 않게 한다.
 * - blk_pre_runtime_suspend(q): q_usage_counter를 잠깐 kill(atomic 모드 전환) 후
 *   percpu_ref_is_zero()로 in-flight 참조가 0인지 확인하고 다시 resurrect한다.
 *   0이 아니면 -EBUSY로 되돌리고 RPM_ACTIVE 유지, 0이면 0을 반환하고
 *   RPM_SUSPENDING 상태로 진행을 허용한다.
 * - blk_post_runtime_suspend(q, err): 드라이버의 실제 suspend 결과(err)에 따라
 *   RPM_SUSPENDED(성공) 또는 RPM_ACTIVE(실패)로 rpm_status를 확정한다.
 * - blk_pre_runtime_resume(q): rpm_status를 RPM_RESUMING으로 표시해 resume이
 *   진행 중임을 알린다.
 * - blk_post_runtime_resume(q): resume의 성공/실패와 무관하게 RPM_ACTIVE로
 *   복귀시키고, 직전 상태가 ACTIVE가 아니었다면 pm_only를 해제해 새 I/O 진입을
 *   허용하며, autosuspend를 다시 예약한다.
 * 이 파일이 새로 정의하는 구조체/enum은 없다. struct request_queue의
 * dev/rpm_status/pm_only/q_usage_counter/queue_lock 필드를 조작 대상으로 삼을 뿐이다.
 */

#include <linux/blk-pm.h>
/* [한국어] enum rpm_status(RPM_ACTIVE/RPM_SUSPENDING/RPM_SUSPENDED/RPM_RESUMING
 * 등)와 blk_pm_resume_queue()/blk_pm_mark_last_busy() 인라인 판정 헬퍼를 선언하는
 * 짝 헤더. 이 파일이 갱신하는 rpm_status를 그 헤더가 "읽는" 관계다. */
#include <linux/blkdev.h>
/* [한국어] struct request_queue(dev/rpm_status/pm_only/q_usage_counter/queue_lock
 * 필드 포함), struct device, WARN_ON_ONCE 등 이 파일이 조작하는 핵심 자료구조
 * 정의를 가져온다. */
#include <linux/pm_runtime.h>
/* [한국어] pm_runtime_set_autosuspend_delay(), pm_runtime_use_autosuspend(),
 * pm_runtime_mark_last_busy(), pm_request_autosuspend() 등 PM 코어(런타임 전원
 * 관리 프레임워크, drivers/base/power/runtime.c) API 선언. 이 파일이 실제로
 * struct device의 전원 상태를 조작할 때 호출하는 모든 PM 코어 함수가 여기서
 * 선언된다. */
#include "blk-mq.h"
/* [한국어] 같은 디렉터리(block/)의 blk-mq 내부 헤더. blk_freeze_queue_start(),
 * blk_mq_unfreeze_queue_nomemrestore() 등 q->q_usage_counter(percpu_ref)를
 * kill/resurrect시켜 큐를 일시 동결/해제하는 내부 API가 여기서 선언된다. 이
 * 함수들은 blk-mq.c에 구현되어 있고, 공개 API가 아니므로 별도 EXPORT_SYMBOL
 * 헤더가 아니라 이 내부 헤더를 통해서만 접근 가능하다. */

/*
 * [한국어]
 * blk_pm_runtime_init - request_queue에 struct device를 연결하고 runtime PM 초기 상태를 설정한다
 *
 * @q: PM을 연동할 대상 request_queue. 드라이버가 blk_mq_init_queue() 등으로 이미
 *     할당을 마친 큐여야 하며, 가급적 이 큐를 통해 아직 I/O가 흐르기 전(probe
 *     초기 단계)에 호출되어야 한다. NVMe라면 네임스페이스에 대응하는 request_queue다.
 * @dev: @q가 속한 struct device. NVMe PCI 컨트롤러라면 &pdev->dev이며,
 *     pm_runtime_* API가 실제로 조작하는 대상은 이 dev->power 구조체다.
 * @return: 없음(void). 실패할 수 없는 초기화 함수다.
 *
 * 배경: 블록 계층의 runtime PM은 request 기반 드라이버(bio를 직접 다루는
 * 드라이버가 아니라 request_queue를 거쳐 dispatch되는 드라이버)에서만 동작한다.
 * 드라이버가 자신의 struct device와 request_queue를 이 함수로 미리 짝지어 두어야,
 * 이후 blk_pre_runtime_suspend()/blk_post_runtime_suspend()/blk_pre_runtime_resume()/
 * blk_post_runtime_resume() 및 block/blk-pm.h의 blk_pm_resume_queue()가 q->dev를
 * 통해 PM 코어와 통신할 수 있다.
 * 동작 순서:
 *   1) q->dev = dev로 두 객체를 연결한다.
 *   2) q->rpm_status = RPM_ACTIVE로 초기값을 설정해, 이 시점부터 큐가 PM 상태
 *      머신의 관리 대상이 되었음을 표시한다.
 *   3) autosuspend delay를 -1로 설정해, 드라이버나 사용자가 명시적으로 값을
 *      갱신하기 전까지는 유휴 시에도 자동 suspend가 걸리지 않게 한다.
 *   4) pm_runtime_use_autosuspend()로 이 device가 "즉시 suspend" 대신
 *      "autosuspend(지연 후 suspend)" 정책을 쓰도록 PM 코어에 등록한다.
 * 실행 컨텍스트: 드라이버 probe/reset 경로의 프로세스 컨텍스트에서, 아직 I/O가
 * 흐르지 않거나 흐르더라도 PM이 아직 비활성/usage_count>0이라 suspend가
 * 불가능한 시점에 호출되어야 한다. 락을 잡지 않는다.
 * 호출자: NVMe라면 nvme_probe() -> nvme_reset_work() -> ... -> 네임스페이스별
 * request_queue 생성 이후 드라이버가 직접 호출한다(이 파일 자체는 호출자를
 * 강제하지 않으며 EXPORT_SYMBOL로 외부 드라이버 모듈에 공개된다).
 * 피호출자: pm_runtime_set_autosuspend_delay(), pm_runtime_use_autosuspend()
 * (둘 다 PM 코어 구현).
 * 에러 경로: 없음 — 이 함수는 항상 성공하는 상태 설정 함수다.
 *
 * 호출 체인:
 *   (드라이버) probe/reset_work -> [blk_pm_runtime_init]
 *   -> pm_runtime_set_autosuspend_delay/pm_runtime_use_autosuspend (PM 코어)
 */

/**
 * blk_pm_runtime_init - Block layer runtime PM initialization routine
 * @q: the queue of the device
 * @dev: the device the queue belongs to
 *
 * Description:
 *    Initialize runtime-PM-related fields for @q and start auto suspend for
 *    @dev. Drivers that want to take advantage of request-based runtime PM
 *    should call this function after @dev has been initialized, and its
 *    request queue @q has been allocated, and runtime PM for it can not happen
 *    yet(either due to disabled/forbidden or its usage_count > 0). In most
 *    cases, driver should call this function before any I/O has taken place.
 *
 *    This function takes care of setting up using auto suspend for the device,
 *    the autosuspend delay is set to -1 to make runtime suspend impossible
 *    until an updated value is either set by user or by driver. Drivers do
 *    not need to touch other autosuspend settings.
 *
 *    The block layer runtime PM is request based, so only works for drivers
 *    that use request as their IO unit instead of those directly use bio's.
 */
/*
 * [한국어]
 * blk_pm_runtime_init - request_queue를 런타임 전원 관리(runtime PM) 대상으로 등록
 *
 * @q:   등록할 request_queue
 * @dev: 이 큐를 소유한 장치. NVMe PCIe라면 nvme_dev의 pci_dev에 대응한다.
 * @return: 없음
 *
 * === 블록 계층 runtime PM이란 ===
 * 장치가 일정 시간 놀고 있으면 저전력 상태로 내리고, 새 I/O가 오면 다시
 * 깨우는 기능이다. 문제는 "지금 정말 놀고 있는가"를 블록 계층만이 알 수
 * 있다는 점이다 — 큐에 대기 중인 request가 있는데 장치를 재우면 그 I/O가
 * 영원히 완료되지 않는다. 그래서 블록 계층이 q->nr_pending을 추적하며
 * suspend 가부를 판단하는 훅을 제공한다.
 *
 * 위 영문 주석이 밝히는 중요한 한계: 이 기능은 request 기반이라 bio를 직접
 * 처리하는 드라이버(예: 일부 가상 블록 장치)에서는 동작하지 않는다.
 * NVMe는 blk-mq 기반이므로 대상이 된다.
 *
 * autosuspend delay를 -1로 두는 이유도 중요하다. 이 함수는 "관리할 준비"만
 * 하고 실제 자동 suspend는 켜지 않는다. 드라이버가 자신의 정책에 맞는 지연
 * 값을 sysfs나 코드로 설정해야 비로소 동작한다 — 블록 계층이 임의로 장치를
 * 재우기 시작하면 곤란하기 때문이다.
 *
 * 실행 컨텍스트: 드라이버 probe 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   드라이버 probe → [blk_pm_runtime_init]
 *     → pm_runtime_set_autosuspend_delay / pm_runtime_use_autosuspend
 */
void blk_pm_runtime_init(struct request_queue *q, struct device *dev)
{
	q->dev = dev;
	/* [한국어] NVMe 컨트롤러(또는 기타 블록 드라이버)의 struct device를 이
	 * request_queue에 연결 — 이후 이 파일의 모든 함수가 q->dev != NULL을
	 * "이 큐가 runtime PM 관리 대상"이라는 판단 기준으로 사용한다. */
	q->rpm_status = RPM_ACTIVE;
	/* [한국어] 큐의 runtime PM 상태를 Active로 초기화 — 아직 한 번도
	 * suspend된 적이 없으므로 I/O 제출이 곧바로 허용되는 상태. */
	pm_runtime_set_autosuspend_delay(q->dev, -1);
	/* [한국어] autosuspend delay를 -1(무한대)로 설정 — 이 값이 유지되는
	 * 한 pm_request_autosuspend()가 나중에 호출되어도 실제 suspend 타이머는
	 * 걸리지 않는다. 드라이버가 이후 별도로 양의 delay 값을 설정해야만
	 * 자동 suspend가 실제로 동작하게 된다. */
	pm_runtime_use_autosuspend(q->dev);
	/* [한국어] 이 device에 대해 "즉시 suspend" 대신 "autosuspend(지연 후
	 * suspend)" 정책을 사용하도록 PM 코어에 등록한다. */
}
EXPORT_SYMBOL(blk_pm_runtime_init);
/* [한국어] 모듈로 빌드된 블록 드라이버(NVMe 등)에서도 이 심볼을 링크할 수
 * 있도록 익스포트 — 이 함수를 호출하지 않는 드라이버는 request_queue의
 * dev 필드가 NULL로 남아 이 파일의 나머지 함수들이 모두 즉시 반환(no-op)한다. */

/*
 * [한국어]
 * blk_pre_runtime_suspend - 런타임 suspend 진입 전, 진행 중인 요청이 없는지 확인한다
 *
 * @q: suspend를 시도할 request_queue. q->dev가 NULL이면(즉 blk_pm_runtime_init()이
 *     호출된 적 없는 큐라면) 이 함수는 즉시 0(허용)을 반환한다.
 * @return: 0이면 지금 바로 디바이스를 runtime suspend해도 안전하다는 뜻이고,
 *     -EBUSY면 아직 큐를 통해 진행 중인 요청이 남아 있어 suspend를 미뤄야 한다는
 *     뜻이다. 호출자(드라이버의 runtime_suspend 콜백)는 -EBUSY를 받으면 그대로
 *     자신의 콜백에서 -EBUSY를 반환해 PM 코어에게 suspend 시도를 그만두도록 알려야 한다.
 *
 * 배경: PCIe/NVMe 컨트롤러를 저전력 상태(APST/RTD3 등)로 내리기 전에는 이미
 * SQ(Submission Queue)에 올라가 CQ(Completion Queue) 완료를 기다리는 명령이
 * 하나도 없어야 한다. 이 함수는 request_queue의 q->q_usage_counter(percpu_ref)를
 * 잠깐 정지시켜 검사함으로써, blk_queue_enter()로 큐에 진입한 뒤 아직
 * blk_queue_exit()하지 않은 참조(=진행 중인 요청)가 있는지 확인하는 "드레인
 * (drain) 완료 확인" 역할을 한다.
 * 동작 순서:
 *   1) q->dev가 없으면 PM 관리 대상이 아니므로 즉시 0(허용)을 반환한다.
 *   2) 진입 시점에는 반드시 RPM_ACTIVE 상태여야 함을 WARN_ON_ONCE로 검증한다
 *      (다른 경로에서 이미 SUSPENDING/SUSPENDED로 바뀌어 있다면 상태 머신이
 *      깨진 것이므로 최소 1회는 경고를 남긴다).
 *   3) queue_lock 아래에서 rpm_status를 RPM_SUSPENDING으로 옮겨 "지금 suspend
 *      판정이 진행 중"임을 표시한다.
 *   4) blk_set_pm_only()로 q->pm_only 카운터를 올려, 이 시점 이후 새로 시도되는
 *      비PM(RQF_PM이 아닌) blk_queue_enter() 호출은 blk-pm.h의
 *      blk_pm_resume_queue() 판정에 걸려 대기(wait_event)하게 만든다. 아래
 *      드레인 검사보다 먼저 이 카운터를 올려야, 검사 도중 새 일반 I/O가
 *      끼어들어 카운트가 다시 늘어나는 경쟁을 막을 수 있다(원문 커널 주석 참고).
 *   5) blk_freeze_queue_start()를 호출한다 — 내부적으로 percpu_ref_kill()로
 *      q->q_usage_counter를 dead 상태로 전환하기 "시작"하며, 이 순간부터
 *      새로운 blk_queue_enter()는 (PM 여부와 무관하게) percpu_ref_tryget_live_rcu()가
 *      실패해 즉시 -ENODEV로 거부된다. 원문 커널 주석은 이를 "percpu에서
 *      atomic 모드로 전환"이라 표현하는데, percpu_ref_kill이 dead 표시와 함께
 *      비동기(call_rcu)로 atomic 모드 전환도 함께 시작하기 때문이다.
 *   6) percpu_ref_switch_to_atomic_sync()로 5)에서 시작된 atomic 모드 전환이
 *      실제로 끝날 때까지(= call_rcu로 예약된 콜백이 실행될 때까지) 동기적으로
 *      대기한다. 이 대기가 끝난 뒤에 호출되는 blk_queue_enter()는 RCU grace
 *      period를 거쳤으므로 반드시 dead 상태와 위 4)에서 올려 둔 pm_only 상태를
 *      함께 관측하게 된다(그렇지 않으면 아직 갱신을 못 본 CPU에서 경쟁이 생길
 *      수 있다).
 *   7) percpu_ref_is_zero()로 atomic 카운터가 정확히 0인지, 즉 blk_queue_enter()로
 *      들어와 아직 blk_queue_exit()하지 않은 참조가 하나도 없는지 확인한다.
 *      0이면 ret을 0(suspend 허용)으로 되돌린다.
 *   8) blk_mq_unfreeze_queue_nomemrestore()로 percpu_ref_resurrect()를 호출해
 *      5)에서 죽였던 카운터를 부활시킨다 — 이후 blk_queue_enter()가 다시
 *      성공할 수 있게 되며(단 pm_only가 아직 올라가 있으므로 비PM 요청은
 *      계속 걸린다), "_nomemrestore" 접미사는 이 짧은 점검용 freeze/unfreeze
 *      쌍에는 진짜 큐 freeze에 딸린 메모리 회수 후처리가 필요 없어 그 부분을
 *      건너뛴다는 뜻이다.
 *   9) ret이 여전히 음수(-EBUSY)라면: rpm_status를 다시 RPM_ACTIVE로 되돌리고,
 *      pm_runtime_mark_last_busy()로 "지금 막 바빴다"고 PM 코어에 알려 나중에
 *      다시 autosuspend를 시도하게 하며, 위 4)에서 올렸던 pm_only 카운터를
 *      blk_clear_pm_only()로 내려 대기 중이던 일반 I/O를 재개시킨다.
 * 실행 컨텍스트: 드라이버의 struct dev_pm_ops.runtime_suspend 콜백 안, PM
 * 코어의 pm_wq 워크큐 컨텍스트(프로세스 컨텍스트, sleep 가능 — percpu_ref
 * 전환 대기와 spin_lock_irq 모두 이 컨텍스트에서 안전하게 쓸 수 있다).
 * 호출자: 드라이버의 runtime_suspend 콜백(NVMe라면 nvme_runtime_suspend())이
 * 콜백 시작 부분에서 호출한다.
 * 피호출자: blk_set_pm_only()/blk_clear_pm_only(), blk_freeze_queue_start(),
 * percpu_ref_switch_to_atomic_sync(), percpu_ref_is_zero(),
 * blk_mq_unfreeze_queue_nomemrestore().
 * 에러 경로: 요청이 아직 드레인되지 않았으면 -EBUSY를 반환하고 상태를
 * RPM_ACTIVE로 복원한다 — 호출자는 이를 자신의 runtime_suspend 콜백 실패로
 * 그대로 전달해 PM 코어가 나중에 다시 시도하도록 한다.
 *
 * 호출 체인:
 *   (드라이버) runtime_suspend 콜백 -> [blk_pre_runtime_suspend]
 *   -> blk_set_pm_only/blk_freeze_queue_start/percpu_ref_switch_to_atomic_sync
 *   -> blk_mq_unfreeze_queue_nomemrestore
 */

/**
 * blk_pre_runtime_suspend - Pre runtime suspend check
 * @q: the queue of the device
 *
 * Description:
 *    This function will check if runtime suspend is allowed for the device
 *    by examining if there are any requests pending in the queue. If there
 *    are requests pending, the device can not be runtime suspended; otherwise,
 *    the queue's status will be updated to SUSPENDING and the driver can
 *    proceed to suspend the device.
 *
 *    For the not allowed case, we mark last busy for the device so that
 *    runtime PM core will try to autosuspend it some time later.
 *
 *    This function should be called near the start of the device's
 *    runtime_suspend callback.
 *
 * Return:
 *    0		- OK to runtime suspend the device
 *    -EBUSY	- Device should not be runtime suspended
 */
/*
 * [한국어]
 * blk_pre_runtime_suspend - 장치를 재워도 되는지 블록 계층 관점에서 판정
 *
 * @q: 판정 대상 request_queue
 * @return: 0 = suspend 진행 가능, -EBUSY = 아직 처리 중인 I/O가 있어 불가
 *
 * 드라이버의 runtime_suspend 콜백이 실제로 장치를 재우기 "전에" 호출해,
 * 블록 계층에 남은 I/O가 없는지 확인받는다.
 *
 * 판정 방식이 흥미롭다. 단순히 카운터를 세는 것이 아니라 큐를 freeze한 뒤
 * percpu_ref 값을 확인한다. freeze는 새 I/O 진입을 막으므로, 그 상태에서
 * 참조가 남아 있다면 "진행 중인 I/O가 실제로 있다"는 뜻이 확정된다.
 * 카운터만 읽으면 읽는 순간과 판정 사이에 새 I/O가 들어올 수 있어 경쟁이
 * 발생한다.
 *
 * -EBUSY를 반환하면 PM 코어가 suspend를 취소하고, 나중에 다시 시도한다.
 *
 * NVMe 관점: 이 판정을 통과해야 nvme_suspend()가 컨트롤러를 저전력 상태
 * (APST 또는 D3)로 내릴 수 있다. 진행 중인 커맨드가 있는데 컨트롤러를
 * 내리면 그 커맨드는 완료되지 않고 타임아웃으로 이어진다.
 *
 * 실행 컨텍스트: PM 코어의 프로세스 컨텍스트. 큐 freeze가 잠들 수 있다.
 *
 * 호출 체인:
 *   PM 코어 → 드라이버의 runtime_suspend → [blk_pre_runtime_suspend]
 *     → blk_freeze_queue_start / blk_mq_unfreeze_queue
 */
int blk_pre_runtime_suspend(struct request_queue *q)
{
	int ret = 0;
	/* [한국어] 기본값 0 = "suspend 진행 가능" — 아래에서 in-flight 참조가
	 * 남아 있다고 판명되면 -EBUSY로 덮어쓴다. */

	if (!q->dev)
		/* [한국어] q->dev가 NULL이면 blk_pm_runtime_init()이 호출된 적
		 * 없는 큐 — runtime PM 관리 대상이 아니므로 판정할 것이 없다. */
		return ret;
		/* [한국어] PM 비대상 큐는 항상 "suspend 가능"(0)으로 간주하고 반환. */

	WARN_ON_ONCE(q->rpm_status != RPM_ACTIVE);
	/* [한국어] 이 함수 진입 시점에는 반드시 RPM_ACTIVE여야 정상 — 그렇지
	 * 않다면 runtime PM 상태 머신을 어긴 이중 suspend 시도 등 버그이므로
	 * 최초 1회만 경고를 남긴다(반복 스팸 방지). */

	spin_lock_irq(&q->queue_lock);
	/* [한국어] rpm_status 갱신을 다른 CPU/인터럽트 컨텍스트의 접근으로부터
	 * 보호하기 위해 queue_lock 획득, 동시에 로컬 IRQ 비활성화. */
	q->rpm_status = RPM_SUSPENDING;
	/* [한국어] "suspend 판정이 진행 중"임을 표시 — 아직 실제로 suspend된
	 * 것은 아니며, 판정 결과(ret)에 따라 다시 ACTIVE로 되돌아갈 수도 있는
	 * 과도 상태다. */
	spin_unlock_irq(&q->queue_lock);
	/* [한국어] rpm_status 갱신 완료, queue_lock 해제 및 IRQ 복원. */

	/*
	 * Increase the pm_only counter before checking whether any
	 * non-PM blk_queue_enter() calls are in progress to avoid that any
	 * new non-PM blk_queue_enter() calls succeed before the pm_only
	 * counter is decreased again.
	 */
	blk_set_pm_only(q);
	/* [한국어] q->pm_only 카운터를 원자적으로 증가 — 이 시점부터 새로
	 * 들어오는 비PM(RQF_PM이 아닌) blk_queue_enter() 호출은 block/blk-pm.h의
	 * blk_pm_resume_queue() 판정에 걸려 대기하게 된다. 아래 드레인 검사보다
	 * 먼저 이 카운터를 올려야, 검사 도중 새 일반 I/O가 몰래 끼어들어
	 * q_usage_counter가 다시 늘어나는 경쟁(race)을 막을 수 있다. */
	ret = -EBUSY;
	/* [한국어] 일단 "아직 진행 중인 참조가 있다"고 비관적으로 가정 —
	 * 아래에서 실제로 카운터가 0임이 확인되면 다시 0으로 바뀐다. */
	/* Switch q_usage_counter from per-cpu to atomic mode. */
	blk_freeze_queue_start(q);
	/* [한국어] q->q_usage_counter(percpu_ref)를 percpu_ref_kill()로
	 * dead 상태로 전환하기 시작 — percpu 모드에서는 CPU마다 로컬 카운터를
	 * 쓰므로 "정확히 0인지"를 값싸게 확인할 방법이 없어, kill과 함께
	 * 시작되는 atomic 모드 전환을 거쳐야 전역 카운터를 직접 읽어 0 여부를
	 * 판정할 수 있다. 이 시점부터 새 blk_queue_enter()는 PM 여부와 무관하게
	 * percpu_ref_tryget_live_rcu() 실패로 즉시 거부된다. */
	/*
	 * Wait until atomic mode has been reached. Since that
	 * involves calling call_rcu(), it is guaranteed that later
	 * blk_queue_enter() calls see the pm-only state. See also
	 * http://lwn.net/Articles/573497/.
	 */
	percpu_ref_switch_to_atomic_sync(&q->q_usage_counter);
	/* [한국어] 위에서 시작된 percpu -> atomic 전환이 실제로(call_rcu 콜백까지)
	 * 완료될 때까지 동기적으로 대기 — 이 대기가 끝난 뒤에 호출되는
	 * blk_queue_enter()는 RCU grace period 덕분에 반드시 위에서 올린
	 * pm_only 상태와 dead 상태를 함께 관측하게 된다(그렇지 않으면 아직
	 * 갱신을 못 본 CPU에서 새 일반 I/O가 몰래 들어올 수 있다). */
	if (percpu_ref_is_zero(&q->q_usage_counter))
		/* [한국어] atomic 모드로 전환된 카운터가 정확히 0인지 확인 —
		 * 0이면 blk_queue_enter()로 들어와 아직 blk_queue_exit()하지
		 * 않은 참조(=진행 중인 요청)가 하나도 없다는 뜻. */
		ret = 0;
		/* [한국어] in-flight 참조가 없으므로 suspend 허용으로 갱신. */
	/* Switch q_usage_counter back to per-cpu mode. */
	blk_mq_unfreeze_queue_nomemrestore(q);
	/* [한국어] percpu_ref_resurrect()로 카운터를 dead 상태에서 되살려
	 * 다시 percpu 모드로 복귀시킨다 — 이후 일반 I/O 경로의
	 * blk_queue_enter()/blk_queue_exit()가 다시 저비용 percpu 연산으로
	 * 동작할 수 있게 된다. "_nomemrestore" 접미사는 이 짧은 점검용
	 * freeze/unfreeze 쌍에는 진짜 큐 freeze에 딸린 메모리 회수 후처리가
	 * 필요 없어 그 부분을 건너뛴다는 뜻이다. */

	if (ret < 0) {
		/* [한국어] 여전히 -EBUSY라면(위 드레인 검사에서 진행 중인 요청이
		 * 남아 있었다면) 상태를 롤백해야 한다. */
		spin_lock_irq(&q->queue_lock);
		/* [한국어] 상태 롤백을 위해 queue_lock 재획득. */
		q->rpm_status = RPM_ACTIVE;
		/* [한국어] suspend를 진행할 수 없으므로 RPM_SUSPENDING에서 다시
		 * RPM_ACTIVE로 되돌림 — 디바이스는 계속 활성 상태로 남는다. */
		pm_runtime_mark_last_busy(q->dev);
		/* [한국어] "지금 막 바빴다"는 타임스탬프를 갱신해, PM 코어가
		 * autosuspend_delay 이후 다시 suspend를 시도하도록 유도한다. */
		spin_unlock_irq(&q->queue_lock);
		/* [한국어] 상태 롤백 완료, queue_lock 해제. */

		blk_clear_pm_only(q);
		/* [한국어] 위에서 올렸던 pm_only 카운터를 다시 내려, 대기 중이던
		 * 일반 I/O의 blk_queue_enter()가 통과되도록 허용한다(내부적으로
		 * 카운터가 0이 되면 wake_up_all(&q->mq_freeze_wq)로 대기자를 깨움). */
	}

	return ret;
	/* [한국어] 0이면 호출자(드라이버)가 실제 하드웨어 suspend를 진행해도
	 * 되고, -EBUSY면 호출자가 자신의 runtime_suspend 콜백에서 이 값을
	 * 그대로 반환해 PM 코어에 재시도를 맡겨야 한다. */
}
EXPORT_SYMBOL(blk_pre_runtime_suspend);
/* [한국어] 외부 블록 드라이버(NVMe 등)가 자신의 runtime_suspend 콜백에서
 * 직접 호출할 수 있도록 익스포트. */

/*
 * [한국어]
 * blk_post_runtime_suspend - 드라이버의 실제 suspend 결과를 반영해 큐 상태를 확정한다
 *
 * @q: blk_pre_runtime_suspend()로 RPM_SUSPENDING 판정을 받았던 request_queue.
 * @err: 드라이버의 실제 runtime_suspend 하드웨어 동작이 반환한 값. 0이면 성공,
 *     음수 errno면 실패(예: 컨트롤러가 여전히 명령을 처리 중이어서 하드웨어
 *     차원의 suspend 자체가 실패한 경우).
 * @return: 없음(void). q->rpm_status를 갱신하는 부수효과만 있다.
 *
 * 배경: blk_pre_runtime_suspend()는 "블록 계층 관점에서" suspend해도 되는지만
 * 판정했을 뿐, 실제 하드웨어를 저전력 상태로 내리는 것은 드라이버의 몫이다.
 * 이 함수는 그 실제 시도 결과(err)를 받아 큐의 최종 상태를 확정하는 후처리
 * 단계다.
 * 동작 순서:
 *   1) q->dev가 없으면(PM 비대상) 아무 것도 하지 않고 반환한다.
 *   2) queue_lock을 잡고, err가 0(성공)이면 rpm_status를 RPM_SUSPENDED로
 *      확정한다 — 이제 이 큐를 통한 새 일반 I/O는 block/blk-pm.h의
 *      blk_pm_resume_queue()가 pm_request_resume()을 걸며 막는다.
 *   3) err가 0이 아니면(실패) rpm_status를 RPM_ACTIVE로 되돌리고,
 *      pm_runtime_mark_last_busy()로 나중에 다시 시도하도록 힌트를 남긴다.
 *   4) err가 있었을 때만(suspend 실패 시에만) blk_clear_pm_only()를 호출해,
 *      blk_pre_runtime_suspend()가 올려 두었던 pm_only 카운터를 내려 대기
 *      중이던 일반 I/O를 재개시킨다 — 성공(err==0)했을 때는 큐가 정말로
 *      SUSPENDED 상태이므로 pm_only를 계속 유지해야 blk_pm_resume_queue()가
 *      새 I/O를 계속 막을 수 있다.
 * 실행 컨텍스트: 드라이버의 runtime_suspend 콜백 맨 끝, PM 코어 워크큐의
 * 프로세스 컨텍스트.
 * 호출자: 드라이버의 runtime_suspend 콜백(NVMe라면 nvme_runtime_suspend())이
 * 실제 하드웨어 suspend를 마친 직후 호출한다.
 * 피호출자: pm_runtime_mark_last_busy(), blk_clear_pm_only().
 * 에러 경로: err가 음수면 rpm_status를 ACTIVE로 되돌리고 pm_only를 해제해
 * 일반 I/O가 계속 흐르게 한다 — 별도의 반환값으로 실패를 전달하지 않으며,
 * 드라이버는 자신의 콜백에서 err를 이미 알고 있다.
 *
 * 호출 체인:
 *   (드라이버) runtime_suspend 콜백 (실제 하드웨어 suspend 이후)
 *   -> [blk_post_runtime_suspend] -> pm_runtime_mark_last_busy/blk_clear_pm_only
 */

/**
 * blk_post_runtime_suspend - Post runtime suspend processing
 * @q: the queue of the device
 * @err: return value of the device's runtime_suspend function
 *
 * Description:
 *    Update the queue's runtime status according to the return value of the
 *    device's runtime suspend function and mark last busy for the device so
 *    that PM core will try to auto suspend the device at a later time.
 *
 *    This function should be called near the end of the device's
 *    runtime_suspend callback.
 */
void blk_post_runtime_suspend(struct request_queue *q, int err)
{
	if (!q->dev)
		/* [한국어] PM 비대상 큐면 확정할 상태가 없음. */
		return;
		/* [한국어] 아무 부수효과 없이 반환. */

	spin_lock_irq(&q->queue_lock);
	/* [한국어] rpm_status 갱신 보호를 위해 queue_lock 획득. */
	if (!err) {
		/* [한국어] err == 0: 드라이버의 실제 하드웨어 suspend 성공. */
		q->rpm_status = RPM_SUSPENDED;
		/* [한국어] 큐를 최종 SUSPENDED 상태로 확정 — 이후
		 * block/blk-pm.h의 blk_pm_resume_queue()가 이 값을 보고 일반
		 * I/O 진입 시 pm_request_resume()을 트리거하게 된다. */
	} else {
		/* [한국어] err != 0: 하드웨어 suspend 실패, 디바이스는 여전히
		 * 동작 중인 상태. */
		q->rpm_status = RPM_ACTIVE;
		/* [한국어] suspend가 실패했으므로 큐 상태를 Active로 되돌림 —
		 * SUSPENDING 상태로 남겨두면 이후 판정들이 꼬이게 된다. */
		pm_runtime_mark_last_busy(q->dev);
		/* [한국어] 실패 직후를 "바빴던 시각"으로 기록해, PM 코어가
		 * autosuspend_delay 이후 다시 suspend를 재시도하도록 유도한다. */
	}
	spin_unlock_irq(&q->queue_lock);
	/* [한국어] 상태 확정 완료, queue_lock 해제. */

	if (err)
		/* [한국어] suspend가 실패했을 때만 진입. */
		blk_clear_pm_only(q);
		/* [한국어] blk_pre_runtime_suspend()가 올려 두었던 pm_only
		 * 카운터를 내려 대기 중이던 일반 I/O를 재개시킨다 — 성공
		 * (err==0)한 경우에는 큐가 진짜 SUSPENDED이므로 pm_only를
		 * 계속 유지해 blk_pm_resume_queue()가 새 I/O를 계속 막게 둔다. */
}
EXPORT_SYMBOL(blk_post_runtime_suspend);
/* [한국어] 드라이버의 runtime_suspend 콜백 맨 끝에서 직접 호출할 수 있도록
 * 익스포트. */

/*
 * [한국어]
 * blk_pre_runtime_resume - 런타임 resume 시작 직전 큐 상태를 RESUMING으로 표시한다
 *
 * @q: 곧 하드웨어 resume이 시작될 request_queue. 직전까지 RPM_SUSPENDED
 *     상태였던 큐다.
 * @return: 없음(void).
 *
 * 배경: 하드웨어가 아직 완전히 깨어나지 않은 짧은 구간에도 rpm_status를
 * 명확히 "복귀 중"으로 남겨 두어야, 그 사이 다른 경로(예: 동시에 들어오는
 * block/blk-pm.h의 blk_pm_resume_queue() 판정)가 큐 상태를 RPM_SUSPENDED로
 * 오해하지 않는다.
 * 동작 순서:
 *   1) q->dev가 없으면(PM 비대상) 아무 것도 하지 않는다.
 *   2) queue_lock 아래에서 rpm_status를 RPM_RESUMING으로 설정한다.
 * 실행 컨텍스트: 드라이버의 runtime_resume 콜백 시작 부분, PM 코어 워크큐의
 * 프로세스 컨텍스트.
 * 호출자: 드라이버의 runtime_resume 콜백(NVMe라면 nvme_runtime_resume())이
 * 실제 하드웨어를 깨우기 직전에 호출한다.
 * 피호출자: 없음(스핀락 조작 외에는 다른 함수를 호출하지 않는다).
 * 에러 경로: 없음 — 실패할 수 없는 상태 표시 함수다.
 *
 * 호출 체인:
 *   (드라이버) runtime_resume 콜백 -> [blk_pre_runtime_resume]
 *   -> (드라이버가 이어서 실제 하드웨어 resume 수행)
 */

/**
 * blk_pre_runtime_resume - Pre runtime resume processing
 * @q: the queue of the device
 *
 * Description:
 *    Update the queue's runtime status to RESUMING in preparation for the
 *    runtime resume of the device.
 *
 *    This function should be called near the start of the device's
 *    runtime_resume callback.
 */
void blk_pre_runtime_resume(struct request_queue *q)
{
	if (!q->dev)
		/* [한국어] PM 비대상 큐면 표시할 상태가 없음. */
		return;
		/* [한국어] 아무 부수효과 없이 반환. */

	spin_lock_irq(&q->queue_lock);
	/* [한국어] rpm_status 갱신 보호를 위해 queue_lock 획득. */
	q->rpm_status = RPM_RESUMING;
	/* [한국어] "하드웨어 resume이 진행 중"임을 표시 — 아직 RPM_ACTIVE는
	 * 아니지만 RPM_SUSPENDED도 아닌 과도 상태로 전환. */
	spin_unlock_irq(&q->queue_lock);
	/* [한국어] 상태 표시 완료, queue_lock 해제. */
}
EXPORT_SYMBOL(blk_pre_runtime_resume);
/* [한국어] 드라이버의 runtime_resume 콜백 시작 부분에서 직접 호출할 수
 * 있도록 익스포트. */

/*
 * [한국어]
 * blk_post_runtime_resume - 하드웨어 resume 완료 후 큐를 Active로 되돌리고 새 I/O를 재개한다
 *
 * @q: resume 시퀀스를 마친 request_queue.
 * @return: 없음(void).
 *
 * 배경: resume은 성공/실패 여부와 무관하게 큐를 다시 사용 가능한 상태로
 * 되돌려야 한다 — 하드웨어 resume이 실패하더라도 드라이버나 에러 핸들러가
 * 디바이스와 계속 통신(예: 리셋 명령 전송)해야 하므로, 블록 계층이 계속
 * 큐를 막아 두면 오히려 복구가 불가능해진다. 그래서 이 함수는 별도의 err
 * 파라미터 없이 무조건 RPM_ACTIVE로 복귀시킨다.
 * 동작 순서:
 *   1) q->dev가 없으면(PM 비대상) 아무 것도 하지 않는다.
 *   2) queue_lock 아래에서 되돌리기 전의 상태(old_status)를 저장한 뒤,
 *      rpm_status를 RPM_ACTIVE로 설정한다.
 *   3) pm_runtime_mark_last_busy()로 지금을 "바빴던 시각"으로 기록한다.
 *   4) pm_request_autosuspend()로 PM 코어에 "나중에 유휴 상태가 되면
 *      autosuspend delay 후 다시 자동으로 suspend를 시도하라"고 예약한다.
 *   5) old_status가 RPM_ACTIVE가 아니었다면(즉 직전까지 RPM_SUSPENDED나
 *      RPM_RESUMING이었다면) blk_clear_pm_only()로 pm_only 카운터를 내려
 *      대기 중이던 일반 I/O의 blk_queue_enter()를 통과시킨다. old_status가
 *      이미 RPM_ACTIVE였다면 이 경로에서 pm_only가 올라간 적이 없을 수
 *      있으므로 중복 해제를 피하기 위해 건너뛴다.
 * 실행 컨텍스트: 드라이버의 runtime_resume 콜백 맨 끝, PM 코어 워크큐의
 * 프로세스 컨텍스트.
 * 호출자: 드라이버의 runtime_resume 콜백(NVMe라면 nvme_runtime_resume())이
 * 실제 하드웨어 resume을 마친 직후(성공/실패 무관) 호출한다.
 * 피호출자: pm_runtime_mark_last_busy(), pm_request_autosuspend(),
 * blk_clear_pm_only().
 * 에러 경로: 이 함수 자체는 실패를 표현하지 않는다 — 하드웨어 resume의
 * 성공/실패는 호출자가 이미 처리했으며, 이 함수는 그 결과와 무관하게 큐만
 * 재개시킨다.
 *
 * 호출 체인:
 *   (드라이버) runtime_resume 콜백 (실제 하드웨어 resume 이후, 성공/실패 무관)
 *   -> [blk_post_runtime_resume] -> pm_runtime_mark_last_busy/pm_request_autosuspend/
 *      blk_clear_pm_only -> wake_up_all(q->mq_freeze_wq) (blk_clear_pm_only 내부)
 *   -> 대기 중이던 blk_queue_enter()의 wait_event 조건 재평가
 */

/**
 * blk_post_runtime_resume - Post runtime resume processing
 * @q: the queue of the device
 *
 * Description:
 *    Restart the queue of a runtime suspended device. It does this regardless
 *    of whether the device's runtime-resume succeeded; even if it failed the
 *    driver or error handler will need to communicate with the device.
 *
 *    This function should be called near the end of the device's
 *    runtime_resume callback to correct queue runtime PM status and re-enable
 *    peeking requests from the queue.
 */
void blk_post_runtime_resume(struct request_queue *q)
{
	int old_status;
	/* [한국어] rpm_status를 RPM_ACTIVE로 바꾸기 "전"의 값을 잠시 저장 —
	 * 되돌리기 전 상태가 이미 ACTIVE였는지(즉 pm_only를 내려야 하는
	 * 상황인지)를 뒤에서 판단하는 데 사용한다. */

	if (!q->dev)
		/* [한국어] PM 비대상 큐면 재개할 상태가 없음. */
		return;
		/* [한국어] 아무 부수효과 없이 반환. */

	spin_lock_irq(&q->queue_lock);
	/* [한국어] rpm_status 조회·갱신을 원자적으로 하기 위해 queue_lock 획득. */
	old_status = q->rpm_status;
	/* [한국어] 갱신 전 상태를 저장 — RPM_SUSPENDED/RPM_RESUMING/RPM_ACTIVE
	 * 중 하나였을 것이다. */
	q->rpm_status = RPM_ACTIVE;
	/* [한국어] 큐를 다시 Active로 전환 — 이제 rpm_status 자체는 새 일반
	 * I/O의 진입 조건을 만족하는 상태가 된다. */
	pm_runtime_mark_last_busy(q->dev);
	/* [한국어] resume이 막 끝난 지금 시각을 "바빴던 시각"으로 기록 —
	 * 곧바로 다시 idle 판정이 나서 재차 suspend가 시도되지 않도록 한다. */
	pm_request_autosuspend(q->dev);
	/* [한국어] PM 코어에 비동기 autosuspend 요청을 예약 — 이후 실제로
	 * autosuspend_delay만큼 유휴 상태가 지속되면 다시 runtime_suspend
	 * 콜백이 트리거된다. */
	spin_unlock_irq(&q->queue_lock);
	/* [한국어] rpm_status 갱신 완료, queue_lock 해제. */

	if (old_status != RPM_ACTIVE)
		/* [한국어] resume 직전 상태가 RPM_SUSPENDED 또는 RPM_RESUMING
		 * 이었다면(즉 blk_pre_runtime_suspend()나 다른 경로에서
		 * pm_only가 올라가 있었을 가능성이 있다면) 진입. */
		blk_clear_pm_only(q);
		/* [한국어] pm_only 카운터를 내려 대기 중이던 일반 I/O의
		 * blk_queue_enter()를 통과시키고, 내부적으로 카운터가 0이
		 * 되면 wake_up_all(&q->mq_freeze_wq)로 대기자를 깨운다. */
}
EXPORT_SYMBOL(blk_post_runtime_resume);
/* [한국어] 드라이버의 runtime_resume 콜백 맨 끝에서 직접 호출할 수 있도록
 * 익스포트. 이 시점 이후로 큐는 RPM_ACTIVE 상태이며, block/blk-pm.h의
 * blk_pm_resume_queue()는 별도의 resume 요청 없이 즉시 I/O 진입을 허용한다. */
