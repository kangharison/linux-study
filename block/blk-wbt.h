/* SPDX-License-Identifier: GPL-2.0 */
/*
 * [한국어] WBT(Writeback Throttle, 쓰기 되돌림 스로틀링) 공개 인터페이스 선언 헤더 (block/blk-wbt.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 block/blk-wbt.c가 구현하는 WBT(Writeback Throttle) 기능을 블록
 * 계층의 나머지 코드에 노출하기 위한 함수 프로토타입 6개만을 선언한다. WBT는
 * rq-qos(request 품질 제어) 계층에 등록되는 정책 중 하나로, buffered writeback
 * bio가 request로 변환되어 blk-mq 하드웨어 큐(hctx)로 발행되기 전 단계에서
 * in-flight 쓰기 request 수를 동적으로 제한해 저장장치의 쓰기 버퍼/캐시 포화
 * (예: NVMe라면 NAND 쓰기 버퍼 및 컨트롤러 GC 부하)를 완화한다. 이 헤더 자체는
 * struct rq_wb 같은 실제 상태 구조체를 정의하지 않으며, "디스크 등록",
 * "다른 QoS 정책과의 상호 배제", "사용자 latency 설정" 이라는 3가지 외부 접점만
 * 노출한다. CONFIG_BLK_WBT가 꺼진 빌드에서는 그중 3개 함수(wbt_init_enable_default,
 * wbt_disable_default, wbt_enable_default)만 아무 동작도 하지 않는 static inline
 * stub으로 대체되어, 커널 전역에서 이 함수들에 대한 호출 자체는 항상 안전하게
 * 컴파일된다(WBT 미빌드 시에도 링크 에러 없음).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 헤더가 노출하는 함수들은 실제 bio 스로틀링 로직(wbt_wait, wbt_done,
 * wbt_cb/scale_up/scale_down 등, block/blk-wbt.c 내부 static 함수)을 감싸는
 * 바깥 껍데기로서, 다음 3개 지점에서만 관여한다.
 *   1) 디스크 등록 시점: add_disk() -> blk_register_queue()(block/blk-sysfs.c)
 *      -> wbt_init_enable_default() -> __wbt_enable_default()로 설치 조건
 *      (CONFIG_BLK_WBT_MQ, QUEUE_FLAG_DISABLE_WBT, blk-mq 여부) 확인
 *      -> 조건 충족 시 wbt_alloc()+wbt_init()으로 rq_wb를 rq-qos 체인에 연결.
 *      이후 실제 bio는 blk_mq_submit_bio -> __rq_qos_throttle -> wbt_wait
 *      경로로 흘러 이 헤더가 아닌 blk-wbt.c 내부 로직의 통제를 받는다.
 *   2) 정책 상호 배제 시점: block/bfq-iosched.c, block/blk-iocost.c가 자체
 *      latency/cost 기반 QoS 제어를 사용하는 동안 wbt_disable_default()로
 *      WBT를 끄고, 그 정책이 해제되면 wbt_enable_default()로 되돌려 이중
 *      스로틀링을 방지한다.
 *   3) 사용자 설정 시점: block/blk-sysfs.c의 "wbt_lat_usec" sysfs 속성이
 *      wbt_get_min_lat()/wbt_disabled()/wbt_set_lat()로 목표 latency(usec)를
 *      조회·변경한다.
 * 실행 컨텍스트: 위 세 접점 모두 프로세스 컨텍스트(디스크 등록 syscall 경로,
 * sysfs write(2), 스케줄러 전환 경로)에서만 호출되며, NVMe CQ 인터럽트나
 * softirq 컨텍스트에서는 이 헤더의 함수가 호출되지 않는다(그 컨텍스트는
 * wbt_done() 등 blk-wbt.c 내부 함수가 담당하며 이 헤더에는 노출되지 않는다).
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈: 이 헤더 자체는 구현부가 없는 순수 선언이므로 컴파일 시점 의존성은
 * 없으나, 링크 시 CONFIG_BLK_WBT=y라면 block/blk-wbt.c의 실제 정의를 필요로
 * 한다. struct gendisk / struct request_queue는 전방 선언에 의존하며, 이 헤더는
 * <linux/blkdev.h> 등을 직접 include하지 않고 먼저 포함하는 쪽(block/blk.h 등)이
 * 이미 선언했다고 가정한다.
 * 이 헤더를 포함하는(의존하는) 모듈:
 *   - block/blk-wbt.c   : 자기 자신의 프로토타입을 이 헤더와 일치시켜 정의하는
 *                          실질 구현 파일. struct rq_wb, wbt_wait/wbt_done 등
 *                          내부 로직은 모두 여기 있다.
 *   - block/blk-sysfs.c : 디스크 등록 시 wbt_init_enable_default() 호출,
 *                          "wbt_lat_usec" sysfs 속성에서 wbt_get_min_lat()/
 *                          wbt_disabled()/wbt_set_lat() 호출.
 *   - block/bfq-iosched.c, block/blk-iocost.c : 자체 QoS 정책과 WBT의 상호
 *                          배제를 위해 wbt_enable_default()/wbt_disable_default()
 *                          호출.
 *   - block/blk-mq-sched.c, block/blk-settings.c, block/elevator.c,
 *     block/blk-iocost.c : 큐 구성 변경/스케줄러 전환 시 WBT 상태를 함께
 *                          고려하기 위해 이 헤더를 포함.
 * 데이터 흐름: 여기 선언된 함수들은 "활성/비활성 상태"와 "latency 목표값(ns)"
 * 같은 스칼라 상태만 주고받을 뿐, bio/request 자체를 인자로 받지 않는다. 실제
 * bio 스로틀링에 쓰이는 rq_wait.inflight 카운터 증감, blk-stat latency 샘플
 * 수집 등의 데이터 경로는 모두 block/blk-wbt.c 내부에 캡슐화되어 있으며, 이
 * 헤더를 통해서는 노출되지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * wbt_init_enable_default() : 디스크 등록 시 설치 조건이 충족되면 WBT를 신규
 *                              할당·초기화하고 rq-qos 체인에 등록한다.
 * wbt_disable_default()     : WBT_STATE_ON_DEFAULT로 자동 활성화된 WBT를
 *                              WBT_STATE_OFF_DEFAULT로 끈다(수동 설정은 보존).
 * wbt_enable_default()      : 자동 활성화 조건을 재평가해 필요하면 상태만
 *                              ON_DEFAULT로 되돌린다(신규 rq_wb 할당은 하지
 *                              않는다 — 그 역할은 wbt_init_enable_default 몫).
 * wbt_get_min_lat()         : 현재 설정된 목표 read latency(ns)를 반환한다.
 * wbt_disabled()            : WBT가 미설치이거나 꺼져 있는지를 bool로 반환한다.
 * wbt_set_lat()              : 사용자가 지정한 latency(usec, -1이면 기본값)로
 *                              WBT를 재설정하고, 미설치 상태였다면 새로 설치한다.
 * 이 헤더에는 구조체 정의가 없다 — WBT의 per-device 상태 구조체인
 * struct rq_wb(rq_wait[], rq_depth, min_lat_nsec, cb 등)는 block/blk-wbt.c에
 * 정의되어 있으며, 이 헤더의 함수들은 모두 그 구조체를 캡슐화해 gendisk /
 * request_queue 단위의 핸들만 외부에 노출한다.
 */
#ifndef WB_THROTTLE_H /* [한국어] 여러 소스 파일이 blk-wbt.h를 중복 include해도 아래 선언이 한 번만 해석되도록 막는 표준 include guard 시작 */
#define WB_THROTTLE_H /* [한국어] 위 #ifndef과 짝을 이루는 guard 심볼 정의 — 이후 재포함 시 #ifndef 조건이 거짓이 되어 본문 전체를 건너뜀 */

#ifdef CONFIG_BLK_WBT /* [한국어] 커널 빌드 설정(.config)에서 CONFIG_BLK_WBT=y일 때만 실제 WBT 함수 프로토타입 6개를 노출 */

/*
 * [한국어]
 * wbt_init_enable_default - 디스크 등록 시 설치 조건이 충족되면 WBT를 신규 설치한다
 *
 * @disk: 등록 중인 gendisk. QUEUE_FLAG_REGISTERED가 막 설정된 직후의 디스크로,
 *        disk->queue가 완전히 구성되어 있는 상태여야 한다.
 * @return: 없음 (void) — 설치 실패/조건 미충족 시에도 조용히 반환하며, 이 경우
 *          디스크는 WBT 없이(스로틀링 없이) 계속 동작한다.
 *
 * 정의는 block/blk-wbt.c에 있다. 내부적으로 __wbt_enable_default()를 호출해
 * CONFIG_BLK_WBT_MQ 컴파일 옵션, blk_queue_disable_wbt() 플래그, 이미 설치된
 * WBT 존재 여부, 디스크 등록 상태, blk-mq 큐 여부를 순서대로 검사한다. 설치가
 * 필요하다고 판단되면 wbt_alloc()으로 struct rq_wb와 blk-stat 콜백을 할당하고
 * wbt_init()으로 rq_qos 체인에 등록한 뒤, blk_mq_debugfs_register_rq_qos()로
 * debugfs 노드를 추가한다. 메모리 부족(-ENOMEM)이나 rq_qos 등록 실패 시에는
 * pr_warn() 경고만 남기고 할당했던 rq_wb를 wbt_free()로 되돌린 후 반환한다.
 * 실행 컨텍스트: add_disk() 경로(프로세스 컨텍스트), 디스크당 최초 1회만 의미
 * 있게 동작한다(이미 설치돼 있으면 __wbt_enable_default가 false를 반환해
 * 조기 반환).
 *
 * 호출 체인:
 *   add_disk -> blk_register_queue(block/blk-sysfs.c) -> [wbt_init_enable_default]
 *     -> __wbt_enable_default -> wbt_alloc -> wbt_init
 *     -> blk_mq_debugfs_register_rq_qos
 */
void wbt_init_enable_default(struct gendisk *disk); /* [한국어] block/blk-sysfs.c의 blk_register_queue()가 디스크 등록 마지막 단계에서 호출 */

/*
 * [한국어]
 * wbt_disable_default - 기본(자동) 활성화된 WBT를 비활성화한다
 *
 * @disk: 대상 gendisk.
 * @return: 없음 (void).
 *
 * 정의는 block/blk-wbt.c에 있다. WBT의 enable_state가 WBT_STATE_ON_DEFAULT일
 * 때만 blk_stat_deactivate()로 latency 샘플링 타이머를 멈추고
 * WBT_STATE_OFF_DEFAULT로 전환한다. 사용자가 sysfs로 수동 설정한
 * WBT_STATE_ON_MANUAL/OFF_MANUAL 상태는 건드리지 않는다 — 즉 "자동으로 켜진
 * 것만 자동으로 끈다"는 원칙을 지킨다. disk->rqos_state_mutex로 보호되어
 * wbt_enable_default()/wbt_set_lat()과의 동시 상태 전이 경쟁을 막는다.
 * 이 함수는 block/bfq-iosched.c와 block/blk-iocost.c처럼, 자체적으로 write
 * latency나 비용(cost)을 조절하는 다른 rq-qos 정책이 활성화될 때 WBT와의
 * 이중 스로틀링을 피하려고 호출된다. WBT가 아예 설치되지 않은 디스크에서는
 * (wbt_rq_qos(q)가 NULL이면) 아무 것도 하지 않고 반환한다.
 *
 * 호출 체인:
 *   block/bfq-iosched.c(BFQ 초기화) / block/blk-iocost.c(iocost 활성화)
 *     -> [wbt_disable_default] -> RQWB -> blk_stat_deactivate
 */
void wbt_disable_default(struct gendisk *disk); /* [한국어] EXPORT_SYMBOL_GPL — BFQ/iocost가 자체 QoS 활성화 시 WBT를 잠시 끔 */

/*
 * [한국어]
 * wbt_enable_default - 기본(자동) WBT 활성화 조건을 재평가해 상태를 되돌린다
 *
 * @disk: 대상 gendisk.
 * @return: 없음 (void) — 내부 __wbt_enable_default()의 bool 반환값은 버려진다.
 *
 * 정의는 block/blk-wbt.c에 있다. __wbt_enable_default()만 호출하고 결과를
 * 사용하지 않는 얇은 래퍼다. WBT가 이미 설치돼 있는 경우(가장 흔한 호출
 * 시나리오): enable_state가 WBT_STATE_OFF_DEFAULT였다면 WBT_STATE_ON_DEFAULT로
 * 되돌리기만 하고, 신규 rq_wb 할당은 수행하지 않는다(그 역할은 오직
 * wbt_init_enable_default()만 한다). 따라서 이 함수는 "한 번도 WBT가 설치된
 * 적 없는 디스크에 새로 WBT를 붙이는" 용도가 아니라, wbt_disable_default()로
 * 잠시 꺼두었던 WBT를 원래 자동 모드로 복귀시키는 용도로 쓰인다.
 * block/bfq-iosched.c가 BFQ 스케줄러를 해제할 때, block/blk-iocost.c가 iocost
 * 컨트롤을 끌 때 각각 호출해 WBT 스로틀링을 다시 넘겨받는다.
 *
 * 호출 체인:
 *   block/bfq-iosched.c(BFQ 해제) / block/blk-iocost.c(iocost 비활성화)
 *     -> [wbt_enable_default] -> __wbt_enable_default
 */
void wbt_enable_default(struct gendisk *disk); /* [한국어] EXPORT_SYMBOL_GPL — BFQ/iocost 해제 시 WBT 자동 모드를 복귀시킴 (신규 설치는 하지 않음) */

/*
 * [한국어]
 * wbt_get_min_lat - 현재 설정된 목표 read latency(ns)를 조회한다
 *
 * @q: 조회 대상 request_queue.
 * @return: rq_wb->min_lat_nsec 값(ns 단위). q에 WBT가 설치되어 있지 않으면 0.
 *
 * 정의는 block/blk-wbt.c에 있다. wbt_rq_qos(q)로 rq_qos 핸들을 찾고, 없으면
 * 즉시 0을 반환한다. 있으면 RQWB(rqos)->min_lat_nsec를 그대로 반환한다 —
 * 이 값은 blk-stat이 측정한 NVMe read 완료 latency와 비교되어 scale up/down
 * 판단의 기준이 되는 임계값이다. block/blk-sysfs.c의 queue_wb_lat_show()가
 * sysfs "wbt_lat_usec" 읽기 요청 시 이 값을 1000으로 나눠(ns -> usec) 사용자
 * 공간에 보여준다. wbt_set_lat() 내부에서도 "변경할 값과 현재 값이 같은지"를
 * 비교해 불필요한 blk_mq_quiesce_queue()를 회피하는 용도로 재사용된다.
 *
 * 호출 체인:
 *   block/blk-sysfs.c: queue_wb_lat_show (sysfs read) -> [wbt_get_min_lat]
 *   block/blk-wbt.c: wbt_set_lat -> [wbt_get_min_lat] (변경 여부 비교)
 */
u64 wbt_get_min_lat(struct request_queue *q); /* [한국어] sysfs "wbt_lat_usec" 읽기 경로에서 ns 단위 목표 latency를 조회 */

/*
 * [한국어]
 * wbt_disabled - 현재 WBT가 비활성 상태인지 확인한다
 *
 * @q: 확인 대상 request_queue.
 * @return: true면 WBT가 미설치이거나 꺼져 있음(스로틀링 없음). false면 WBT가
 *          켜져 있어 wbt_wait 경로가 실제로 in-flight 수를 제한하는 중.
 *
 * 정의는 block/blk-wbt.c에 있다. wbt_rq_qos(q)로 rq_qos가 없으면(WBT 미설치)
 * true, 있으면 rwb_enabled()로 enable_state가 ON 계열(ON_DEFAULT/ON_MANUAL)인지
 * 확인해 그 반대를 반환한다. block/blk-sysfs.c의 queue_wb_lat_show()가 sysfs
 * "wbt_lat_usec" 읽기에서 이 함수가 true면 "0"을 그대로 출력해 사용자에게
 * "스로틀링 꺼짐"을 알린다.
 *
 * 호출 체인:
 *   block/blk-sysfs.c: queue_wb_lat_show (sysfs read) -> [wbt_disabled]
 */
bool wbt_disabled(struct request_queue *q); /* [한국어] sysfs "wbt_lat_usec" 읽기 경로에서 WBT 꺼짐 여부(0 출력 여부)를 판단 */

/*
 * [한국어]
 * wbt_set_lat - 사용자 지정 latency 값으로 WBT 목표를 (재)설정한다
 *
 * @disk: 대상 gendisk.
 * @val: 목표 latency. usec 단위 값(0 이상)이거나, -1이면 wbt_default_latency_nsec()
 *       가 계산하는 장치 기본값(NVMe SSD 2ms, 회전식 HDD 75ms 등)을 사용한다.
 *       -1보다 작은 값은 호출자(block/blk-sysfs.c의 queue_wb_lat_store())가
 *       -EINVAL로 걸러내므로 이 함수에는 도달하지 않는다.
 * @return: 0이면 성공. 음수면 오류(예: 신규 rq_wb 할당 실패 시 -ENOMEM, 또는
 *          wbt_init() 등록 실패 시 그 반환값).
 *
 * 정의는 block/blk-wbt.c에 있다. WBT가 미설치 상태(wbt_rq_qos(q) == NULL)라면
 * wbt_alloc()으로 새 rq_wb를 만든다. blk_mq_freeze_queue()로 큐를 동결해 latency
 * 변경 도중 WBT가 켜지거나 꺼지는 전환이 일어나도 in-flight request가 존재하지
 * 않음을 보장한 뒤, 신규 설치라면 wbt_init()으로 rq_qos 체인에 등록한다. val이
 * -1이면 wbt_default_latency_nsec()로 기본값을 구하고, 0 이상이면 usec를 ns로
 * 환산(*1000)한다. wbt_get_min_lat()로 현재 값과 같으면 불필요한
 * blk_mq_quiesce_queue()를 건너뛰고 바로 unfreeze한다. 값이 다르면
 * blk_mq_quiesce_queue()로 request 처리를 잠시 멈추고, disk->rqos_state_mutex
 * 보호 아래 wbt_set_min_lat()으로 min_lat_nsec와 enable_state(ON_MANUAL/
 * OFF_MANUAL)를 갱신한 뒤 blk_mq_unquiesce_queue()/blk_mq_unfreeze_queue()로
 * 정상 처리를 재개한다.
 * 실행 컨텍스트: sysfs write(2) 프로세스 컨텍스트. blk_mq_freeze_queue/
 * blk_mq_quiesce_queue를 사용하므로 다른 in-flight I/O 완료를 기다릴 수 있어
 * 블로킹 호출이다.
 *
 * 호출 체인:
 *   block/blk-sysfs.c: queue_wb_lat_store (sysfs write) -> [wbt_set_lat]
 *     -> wbt_alloc(필요 시) -> blk_mq_freeze_queue -> wbt_init(필요 시)
 *     -> wbt_get_min_lat -> blk_mq_quiesce_queue -> wbt_set_min_lat
 *     -> blk_mq_unquiesce_queue -> blk_mq_unfreeze_queue
 */
int wbt_set_lat(struct gendisk *disk, s64 val); /* [한국어] sysfs "wbt_lat_usec" 쓰기 경로 — 사용자 지정 목표 latency로 WBT를 재설정(필요 시 신규 설치) */

#else /* CONFIG_BLK_WBT */ /* [한국어] CONFIG_BLK_WBT가 꺼진 빌드 — 커널 설정에서 WBT 기능 자체를 컴파일 대상에서 제외한 경우 */

/*
 * [한국어]
 * wbt_init_enable_default - CONFIG_BLK_WBT=n 빌드용 no-op 스텁
 *
 * @disk: 대상 gendisk. 실제로는 사용되지 않는다(파라미터 이름만 유지되어 호출부
 *        수정 없이 컴파일되도록 함).
 * @return: 없음 (void).
 *
 * WBT 기능이 아예 빌드되지 않았으므로 block/blk-sysfs.c의 blk_register_queue()가
 * 디스크 등록 시 호출해도 아무 상태 변화가 없다. 이 스텁이 없다면 CONFIG_BLK_WBT=n
 * 빌드에서 blk-sysfs.c/bfq-iosched.c/blk-iocost.c가 이 심볼을 참조할 때마다
 * 매번 #ifdef로 호출부를 감싸야 하므로, 헤더 차원에서 빈 구현을 제공해 호출부
 * 코드를 단순하게 유지한다(컴파일러가 인라인 후 완전히 제거).
 *
 * 호출 체인:
 *   add_disk -> blk_register_queue -> [wbt_init_enable_default(no-op)]
 */
static inline void wbt_init_enable_default(struct gendisk *disk) /* [한국어] CONFIG_BLK_WBT=n: 디스크 등록 시 WBT 설치를 시도조차 하지 않음 */
{
} /* [한국어] no-op — rq_wb가 존재하지 않으므로 disk->queue에는 WBT rq_qos가 결코 연결되지 않고, writeback bio는 스로틀링 없이 곧바로 request로 변환됨 */

/*
 * [한국어]
 * wbt_disable_default - CONFIG_BLK_WBT=n 빌드용 no-op 스텁
 *
 * @disk: 대상 gendisk. 실제로는 사용되지 않는다.
 * @return: 없음 (void).
 *
 * WBT가 빌드되지 않았으므로 "끌 대상" 자체가 없다. block/bfq-iosched.c나
 * block/blk-iocost.c가 자체 QoS 활성화 시 이 함수를 호출해도 아무 일도
 * 일어나지 않으며, 그 경로들은 원래부터 WBT와 상호작용할 필요가 없는
 * 상태(WBT 미빌드)로 취급된다.
 *
 * 호출 체인:
 *   block/bfq-iosched.c / block/blk-iocost.c -> [wbt_disable_default(no-op)]
 */
static inline void wbt_disable_default(struct gendisk *disk) /* [한국어] CONFIG_BLK_WBT=n: 끌 WBT 상태 자체가 존재하지 않으므로 아무 동작 없음 */
{
} /* [한국어] no-op — BFQ/iocost와의 이중 스로틀링 우려도 WBT가 없으므로 원천적으로 발생하지 않음 */
static inline void wbt_enable_default(struct gendisk *disk) /* [한국어] CONFIG_BLK_WBT=n: 되돌릴 WBT 자동 모드 자체가 존재하지 않으므로 아무 동작 없음 */
{
} /* [한국어] no-op — WBT 미빌드 상태에서는 이 함수가 호출되어도 writeback 스로틀링이 다시 켜지는 일이 없음(애초에 켜진 적이 없음) */

#endif /* CONFIG_BLK_WBT */ /* [한국어] CONFIG_BLK_WBT 조건부 컴파일 종료 — 이 지점부터는 실구현(6개 선언) 또는 스텁(3개 정의) 중 하나로 이미 확정된 심볼만 남는다 */

#endif /* [한국어] WB_THROTTLE_H include guard 종료 — 최상단 #ifndef WB_THROTTLE_H와 짝을 이룸 */
