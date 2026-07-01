/* SPDX-License-Identifier: GPL-2.0 */
/*
 * block/blk-wbt.h
 *
 * 이 파일은 blk-mq 기반 I/O 요청이 NVMe SSD로 전달되기 전, 쓰기(writeback) 흐름을
 * 제어하여 SSD 낸부 쓰기 버퍼/플래시의 혼잡(congestion)을 완화하는 wbt(Writeback
 * Throttle) 기능의 선언 헤더이다. 실제 정책과 상태 구조체(struct wbt_stat 등)는
 * include/linux/wbt.h 및 block/blk-wbt.c에 정의되어 있으며, 본 헤더는 상위
 * block 레이어(예: blk-mq, elevator, genhd)에서 NVMe 큐 상태를 점검하며 wbt를
 * 활성화/비활성화하거나 지연 목표값을 조정하는 인터페이스를 제공한다.
 * (추정) NVMe 관점에서 wbt는 SQ/CQ submission 속도보다는 사용자 쓰기 버퍼가
 * 컨트롤러의 garbage collection/쓰기 증폭에 과부하를 주지 않도록 조정한다.
 */
#ifndef WB_THROTTLE_H        /* nvme-core.c, nvme-pci.c 등 NVMe 드라이버에서 blk-wbt.h를 중복 포함하지 않도록 하는 include guard */
#define WB_THROTTLE_H        /* (추정) blk-mq 기반 NVMe 쓰기 지연 제어를 위한 wbt 심볼을 컴파일 단위에 노출 */

#ifdef CONFIG_BLK_WBT        /* 커널 설정에서 blk-wbt를 활성화한 경우에만 실제 wbt 인터페이스를 노출; NVMe SSD를 위한 쓰기 버퍼 혼잡 제어가 빌드에 포함됨 */

/*
 * wbt_init_enable_default(): gendisk 생성 시 기본 wbt를 활성화한다.
 * NVMe 장치 등록 과정에서 add_disk() -> blk_queue_init_defaults() -> ... ->
 * wbt_init_enable_default() 경로(추정)를 통해 NVMe namespace 큐에 초기 지연
 * 제어를 연결한다.
 */
void wbt_init_enable_default(struct gendisk *disk); /* add_disk() -> ... -> wbt_init_enable_default(): NVMe namespace에 해당하는 gendisk의 request_queue에 wbt를 연결하여, 향후 submit_bio() -> blk_mq_submit_bio() -> nvme_queue_rq() -> nvme_submit_cmd()/doorbell 로 흐르는 writeback stream의 초기 throttle 지점을 마련함 (추정) */

/*
 * wbt_disable_default(): 장치별 wbt를 비활성화한다.
 * NVMe reset/failover 또는 sysfs 튜닝에서 blk_wbt_disable() 등을 통해
 * 호출될 수 있다(추정).
 */
void wbt_disable_default(struct gendisk *disk); /* NVMe reset/failover/sysfs 튜닝 시 gendisk 단위로 wbt를 끄고, NVMe I/O 경로에서 추가적인 submit 지연 삽입을 중단 (추정) */

/*
 * wbt_enable_default(): 장치별 wbt를 다시 활성화한다.
 * NVMe 장치가 정상 상태로 돌아왔을 때 쓰기 지연 제어를 복원하는 데 사용된다.
 */
void wbt_enable_default(struct gendisk *disk); /* NVMe 장치가 정상 복귀한 후 gendisk에 wbt를 재활성화하여, NVMe 쓰기 버퍼/플래시의 혼잡 완화 제어 재개 */

/*
 * wbt_get_min_lat(): request_queue의 최소 지연 목표를 반환한다.
 * blk-mq 스케줄러나 wbt가 NVMe I/O 경로에서 latency 기반 throttle을 결정할
 * 때 사용된다.
 */
u64 wbt_get_min_lat(struct request_queue *q); /* request_queue는 NVMe blk-mq 큐의 상위 객체이며, 반환값(ns)은 NVMe 평균 완료 지연 목표로 사용되어, wbt가 SQ doorbell 발행 전에 throttle/sleep 여부를 결정하는 데 영향 (추정) */

/* wbt_disabled(): wbt가 비활성화되었는지 확인한다. */
bool wbt_disabled(struct request_queue *q); /* NVMe I/O 처리 루틴에서 if (wbt_disabled(q)) 조걸문으로 wbt 활성/비활성을 검사하며, false일 때 wbt 지연 경로가 동작하여 SQ/CQ submission 속도를 간접 제어 (추정) */

/*
 * wbt_set_lat(): 사용자/sysfs가 원하는 평균 지연을 설정한다.
 * NVMe 장치의 qos_latency_ns를 갱신하여, 향후 nvme_submit_cmd(doorbell)
 * 발행 빈도에 영향을 줄 수 있다(추정).
 */
int wbt_set_lat(struct gendisk *disk, s64 val); /* sysfs 쓰기 경로 -> wbt_set_lat(): NVMe gendisk의 wbt 상태에 val(ns) 지연 목표를 갱신; 비정상 값은 -EINVAL 등을 반환(추정)하고, 이후 NVMe 쓰기 I/O의 평균 latency 목표로 사용 */

#else /* CONFIG_BLK_WBT */        /* CONFIG_BLK_WBT가 꺼진 빌드에서는 wbt 로직이 제거되므로, NVMe 쓰기 스트림은 wbt 지연 없이 SQ로 직접 전달됨 */

/*
 * CONFIG_BLK_WBT가 비활성화되면 wbt 함수는 아무 동작도 하지 않는 inline stub으로
 * 대체된다. 이 경우 NVMe SQ/CQ 제어 경로에서도 추가 지연 제어는 발생하지 않는다.
 */
static inline void wbt_init_enable_default(struct gendisk *disk) /* NVMe namespace 등록 단계에서도 wbt 컨텍스트를 할당/초기화하지 않는 inline stub */
{
} /* no-op: NVMe gendisk의 request_queue에 wbt가 연결되지 않으므로, SQ doorbell 발행 전 wbt 지연은 발생하지 않음 */

static inline void wbt_disable_default(struct gendisk *disk) /* wbt가 빌드되지 않았으므로 NVMe 장치의 throttle 해제 시에도 아무 동작 안 함 */
{
} /* no-op: NVMe SQ/CQ 제어는 wbt 없이 blk-mq 스케줄러 및 nvme_queue_rq()가 담당 */
static inline void wbt_enable_default(struct gendisk *disk) /* wbt가 빌드되지 않았으므로 NVMe 장치 복귀 시에도 throttle을 켜지 않음 */
{
} /* no-op: NVMe 쓰기 버퍼 혼잡 완화는 상위 스케줄러나 장치 캐시 플러시 정책에 의존 */

#endif /* CONFIG_BLK_WBT */        /* CONFIG_BLK_WBT 조걸부 컴파일 종료; 이 지점 이후의 코드는 wbt 활성화 여부와 무관하게 NVMe 및 blk-mq에 공통 적용 */

/* NVMe 관점 핵심 요약 */
/*
 * - blk-wbt.h는 NVMe I/O의 전송 단(SQ/CQ, doorbell)이 아닌, 상위 쓰기 버퍼 조절
 *   계층에서 동작하며, NVMe SSD의 평균 쓰기 지연 목표를 설정하는 인터페이스를
 *   노출한다.
 * - 실제 wbt 상태(struct wbt_stat), 지연 측정, 슬립/깨움 로직은 block/blk-wbt.c와
 *   include/linux/wbt.h에서 구현되며, 본 파일은 그 선언부이다.
 * - 콜 경로 예시: submit_bio -> blk_mq_submit_bio -> ... -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell) 로 이어지는 I/O가 wbt에 의해 요청 발급 전
 *   지연될 수 있다(추정).
 * - (추정) NVMe 컨트롤러의 쓰기 캐시 플러시, GC, wear-leveling 부하를 완화하기
 *   위해 사용자 공간 쓰기 stream을 throttle하는 목적으로 사용된다.
 * - (추정) 장기적으로는 wbt가 NVMe queue depth 조절과 함께 동작하여 SQ가
 *   과도하게 채워지는 것을 막는 보조 수단이 될 수 있다.
 */

#endif /* WB_THROTTLE_H */        /* blk-wbt.h include guard 종료; NVMe 드라이버 소스에서 다시 포함되어도 위 선언들은 한 번만 해석됨 */
