// SPDX-License-Identifier: GPL-2.0
/*
 * CPU <-> hardware queue mapping helpers
 *
 * Copyright (C) 2013-2014 Jens Axboe
 */

/*
 * [한국어 설명] CPU와 blk-mq 하드웨어 큐(hctx) 간 매핑 테이블을 만드는 헬퍼 모음 (blk-mq-cpumap.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 blk-mq(Multi-Queue 블록 계층)가 요청을 제출하는 CPU(소프트웨어 큐,
 * struct blk_mq_ctx)를 실제 하드웨어 큐(struct blk_mq_hw_ctx, 이하 hctx)로 연결하는
 * struct blk_mq_queue_map.mq_map[] 배열을 채우는 헬퍼 함수들을 제공한다. NVMe 컨트롤러를
 * 예로 들면 hctx 하나는 보통 nvme_queue(SQ/CQ 한 쌍)에 대응하므로, 이 파일이 만드는
 * 매핑은 "어떤 CPU가 어느 SQ에 명령을 적재하고 doorbell을 울릴지"를 사실상 결정한다.
 * 매핑을 잘못하거나 균등하지 않게 하면 특정 SQ/CQ에 여러 CPU의 요청이 몰려 tag(NVMe라면
 * CID, Command Identifier) 경쟁과 doorbell/락 경합이 발생하므로, 이 파일은 성능에 직접
 * 영향을 미치는 초기화 단계 코드다. 사용할 hctx 개수를 CPU 개수/하드웨어 상한에 맞춰
 * 정하는 계산 헬퍼(blk_mq_num_*_queues)와 실제 CPU→hctx 배정을 수행하는 헬퍼
 * (blk_mq_map_queues, blk_mq_map_hw_queues), 그리고 hctx가 어느 NUMA 노드에 속하는지
 * 역추적하는 헬퍼(blk_mq_hw_queue_to_node)로 구성된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일의 함수들은 큐 초기화 시점(디바이스 probe 후 blk_mq_alloc_tag_set() 호출) 또는
 * 하드웨어 큐 개수가 바뀌는 시점(__blk_mq_update_nr_hw_queues(), 예: 컨트롤러 reset 이나
 * CPU 토폴로지 변경 대응)에 프로세스 컨텍스트에서 실행되며, 런타임 IO 제출/완료 경로에서는
 * 호출되지 않는다(원본 주석이 명시하는 "queue init time"). 호출 체인(초기화 경로):
 *   (예: NVMe) nvme_probe -> nvme_reset_work -> nvme_setup_io_queues ->
 *   blk_mq_alloc_tag_set -> blk_mq_update_queue_map(blk-mq.c) ->
 *     - set->ops->map_queues 미설정 시: blk_mq_map_queues() (본 파일)
 *     - set->ops->map_queues 설정 시(예: nvme_pci_map_queues):
 *         blk_mq_map_hw_queues() 또는 blk_mq_map_queues() (본 파일)
 * 매핑이 채워진 이후에는 blk_mq_map_swqueue()(blk-mq.c)가 qmap->mq_map[cpu] 값을 읽어
 * ctx->hctxs[]/hctx->cpumask를 구성하고, 그 결과를 런타임에 blk_mq_map_queue_type()
 * (blk-mq.h)이 "이 CPU + 이 hctx_type -> 이 hctx" 조회에 사용한다. 즉 이 파일은 매핑
 * 테이블의 "작성자"이고, blk-mq.c/blk-mq.h는 그 테이블을 읽어 쓰는 "소비자"다.
 *
 * === 타 모듈과의 연결 ===
 * 의존(포함/참조):
 *   include/linux/blk-mq.h : struct blk_mq_queue_map { mq_map, nr_queues,
 *     queue_offset } 정의를 제공한다. mq_map은 CPU 인덱스 -> hctx 인덱스 변환
 *     테이블(nr_cpu_ids개 원소, 값 범위는 [queue_offset, queue_offset+nr_queues)),
 *     nr_queues는 이 map이 다뤄야 할 hctx 개수, queue_offset은 여러 hctx_type
 *     (HCTX_TYPE_DEFAULT/READ/POLL)이 하나의 queue_hw_ctx[] 배열을 구간별로 나눠
 *     쓸 때 이 map이 담당하는 구간의 시작 인덱스다.
 *   blk.h, blk-mq.h(본 디렉터리) : 블록 계층/blk-mq 내부 전용 선언 포함.
 *   linux/group_cpus.h : group_cpus_evenly() — CPU들을 지정한 그룹 수만큼
 *     topology(코어/캐시/NUMA 근접성)를 고려해 균등 분할하는 함수를 선언한다.
 *     IRQ 벡터를 CPU에 분배할 때 쓰는 것과 같은 계열의 알고리즘으로 알려져 있어,
 *     이 파일의 기본 매핑 정책도 그 결과를 그대로 물려받는다.
 *   linux/device/bus.h : struct bus_type.irq_get_affinity 콜백 타입 선언.
 *     PCI 버스는 이를 pci_device_irq_get_affinity()(drivers/pci/pci-driver.c)로
 *     구현하며, 내부적으로 pci_irq_get_affinity()(drivers/pci/msi/api.c)를 호출해
 *     MSI-X 벡터별 CPU affinity 마스크를 반환한다.
 * 피의존(이 파일의 함수를 호출하는 쪽):
 *   block/blk-mq.c : blk_mq_update_queue_map()이 드라이버 ->map_queues 콜백이
 *     없을 때 blk_mq_map_queues()를 직접 호출하고, blk_mq_get_hctx_node()가
 *     blk_mq_hw_queue_to_node()를 호출해 hctx용 메모리의 NUMA 노드를 결정한다.
 *   drivers/nvme/host/pci.c : nvme_pci_map_queues()(->map_queues 콜백)가
 *     HCTX_TYPE_POLL 타입이거나 IRQ 벡터 오프셋이 없을 때는 blk_mq_map_queues()를,
 *     그 외에는 blk_mq_map_hw_queues()를 호출한다. nvme_max_io_queues()와
 *     io_queue_count_set()은 blk_mq_num_possible_queues()로 possible CPU 수를
 *     상한으로 사용한다.
 *   drivers/block/virtio_blk.c : init_vqs()가 blk_mq_num_possible_queues()로
 *     virtqueue 개수를 CPU 수 이하로 제한한다.
 * 데이터 흐름: CPU 토폴로지(cpu_possible_mask/cpu_online_mask, group_cpus_evenly가
 *   계산하는 NUMA 그룹)와 디바이스의 MSI-X IRQ affinity 정보가 입력으로 들어오고,
 *   출력으로 qmap->mq_map[cpu] 테이블이 채워진다. 이 테이블은 이후
 *   blk_mq_map_swqueue()가 읽어 ctx/hctx 자료구조에 반영하며, 최종적으로 런타임 IO
 *   제출 경로(blk_mq_submit_bio -> blk_mq_get_ctx -> blk_mq_map_queue)가 이를 참조한다.
 *
 * === 주요 함수/구조체 요약 ===
 *   blk_mq_num_queues()          : CPU 마스크의 weight와 max_queues 중 0이 아닌
 *                                  작은 값을 골라 사용할 hctx 개수를 정한다(내부 헬퍼).
 *   blk_mq_num_possible_queues() : cpu_possible_mask 기준으로 위 헬퍼를 호출(공개 API).
 *   blk_mq_num_online_queues()   : cpu_online_mask 기준으로 위 헬퍼를 호출(공개 API).
 *   blk_mq_map_queues()          : group_cpus_evenly()로 CPU를 nr_queues개 그룹으로
 *                                  나눠 각 그룹을 대응하는 hctx에 매핑하는 기본 구현.
 *                                  그룹화 실패 시 모든 CPU를 hctx 0에 매핑(fallback).
 *   blk_mq_hw_queue_to_node()    : mq_map[]을 선형 검색해 특정 hctx 인덱스에 매핑된
 *                                  첫 CPU의 NUMA 노드를 반환(역방향 조회, 초기화 전용).
 *   blk_mq_map_hw_queues()       : PCI MSI-X IRQ affinity를 조회해 CPU-hctx를
 *                                  정렬하고, affinity 정보가 없으면
 *                                  blk_mq_map_queues()로 대체(fallback)한다.
 */

#include <linux/kernel.h>	/* [한국어] min_not_zero() 등 커널 공통 매크로 제공 — blk_mq_num_queues()의 상한 비교에 사용 */
#include <linux/threads.h>	/* [한국어] NR_CPUS 등 커널이 지원하는 CPU 개수 상한 매크로 정의 — CPU 마스크/배열 크기와 연관 */
#include <linux/module.h>	/* [한국어] EXPORT_SYMBOL_GPL() 매크로 정의 — 이 파일의 공개 함수를 GPL 모듈(NVMe 등 드라이버)에 노출하기 위해 필요 */
#include <linux/mm.h>		/* [한국어] kfree() 등 메모리 해제 API 선언 — group_cpus_evenly()가 반환한 임시 그룹 배열 해제에 사용 */
#include <linux/smp.h>		/* [한국어] cpu_to_node() 등 CPU->NUMA 노드 조회 API 선언 — blk_mq_hw_queue_to_node()가 사용 */
#include <linux/cpu.h>		/* [한국어] cpu_possible_mask/cpu_online_mask 등 전역 CPU 마스크 선언 — 큐 개수 산정과 기본 매핑의 기준 집합 */
#include <linux/group_cpus.h>	/* [한국어] group_cpus_evenly() 선언 — CPU를 지정한 그룹 수만큼 topology 기반으로 균등 분할하는 blk_mq_map_queues()의 핵심 알고리즘 */
#include <linux/device/bus.h>	/* [한국어] struct bus_type.irq_get_affinity 콜백 타입 선언 — blk_mq_map_hw_queues()가 PCI 등 버스의 IRQ affinity를 조회할 때 사용 */
#include <linux/sched/isolation.h>	/* [한국어] CPU isolation(isolcpus/nohz_full) 관련 선언 — 이 파일에서 직접 호출하는 심볼은 없으나 그룹화 정책과 연관되어 포함된 것으로 보임(추정) */

#include "blk.h"		/* [한국어] 블록 계층 내부 공용 선언(block/blk.h) — request_queue 내부 헬퍼 등, 이 파일이 직접 참조하는 특정 심볼은 없으나 관례상 포함(추정) */
#include "blk-mq.h"		/* [한국어] blk-mq 내부 전용 헤더(block/blk-mq.h) — struct blk_mq_hw_ctx, blk_mq_map_queue_type() 등 이 파일이 채우는 매핑 테이블을 소비하는 쪽의 선언 */

/*
 * [한국어]
 * blk_mq_num_queues - CPU 마스크의 weight와 상한값 중 사용할 hctx 개수 결정 (내부 헬퍼)
 *
 * @mask:       개수를 셀 CPU 집합. 호출자가 cpu_possible_mask 또는
 *              cpu_online_mask를 넘긴다.
 * @max_queues: 하드웨어/드라이버가 지원하는 hctx(하드웨어 큐) 개수 상한.
 *              0이면 "상한 없음"으로 취급되어 무시된다(min_not_zero 특성상
 *              한쪽이 0이면 다른 쪽 값을 그대로 반환).
 * @return:     실제로 만들 hctx 개수 — mask의 CPU 수와 max_queues 중 0이 아닌
 *              쪽에서 더 작은 값.
 *
 * blk_mq_num_possible_queues()/blk_mq_num_online_queues() 두 공개 API가
 * 공유하는 계산 로직을 모아 둔 static 헬퍼다. CPU 수보다 하드웨어가 더 많은
 * hctx를 지원하더라도 CPU 수만큼만 만들어 불필요한 빈 hctx(NVMe라면 아무
 * CPU도 쓰지 않는 SQ/CQ 쌍) 생성을 막고, 반대로 하드웨어 상한이 CPU 수보다
 * 작으면 상한에 맞춰 hctx 수를 줄인다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 큐/태그셋 초기화 시점에만 호출되는 순수
 * 계산 함수라 동시성 문제가 없다.
 *
 * 호출 체인:
 *   blk_mq_num_possible_queues() -> [blk_mq_num_queues]
 *   blk_mq_num_online_queues()   -> [blk_mq_num_queues]
 */
static unsigned int blk_mq_num_queues(const struct cpumask *mask,
				      unsigned int max_queues)
{
	unsigned int num;	/* [한국어] mask 안의 CPU 개수를 담을 변수 — 아래에서 max_queues와 비교해 최종 hctx 개수를 정한다 */

	num = cpumask_weight(mask);	/* [한국어] cpumask_weight: mask 비트맵에서 set된 비트(=CPU) 개수를 센다 */
	return min_not_zero(num, max_queues);	/* [한국어] min_not_zero: num/max_queues 중 0이 아닌 값들 중 최솟값 반환 — CPU 수와 하드웨어 상한 중 더 제한적인 쪽 채택 */
}

/**
 * blk_mq_num_possible_queues - Calc nr of queues for multiqueue devices
 * @max_queues:	The maximum number of queues the hardware/driver
 *		supports. If max_queues is 0, the argument is
 *		ignored.
 *
 * Calculates the number of queues to be used for a multiqueue
 * device based on the number of possible CPUs.
 */
/*
 * [한국어]
 * blk_mq_num_possible_queues - possible CPU 수 기준으로 사용할 hctx 개수 계산
 *
 * @max_queues: 하드웨어/드라이버가 지원하는 hctx 개수 상한(0이면 무시).
 * @return:     possible CPU 개수와 max_queues 중 0이 아닌 더 작은 값.
 *
 * cpu_possible_mask(부팅 시점에 이론상 활성화될 수 있는 모든 CPU 집합으로,
 * CPU hotplug로 나중에 online될 CPU까지 포함)를 기준으로 삼기 때문에, 아직
 * online되지 않은 CPU에 대해서도 미리 hctx 자원을 확보해 둘 수 있다. 실제로
 * drivers/nvme/host/pci.c의 nvme_max_io_queues()와 io_queue_count_set()이 이
 * 함수로 possible CPU 수를 상한으로 삼아 컨트롤러가 만들 SQ/CQ 쌍 개수를
 * 제한하며, drivers/block/virtio_blk.c의 init_vqs()도 동일한 용도로 virtqueue
 * 개수를 제한한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(디바이스 probe/모듈 파라미터 검증 시점).
 *
 * 호출 체인:
 *   nvme_max_io_queues()/io_queue_count_set()(drivers/nvme/host/pci.c),
 *   virtio_blk init_vqs() 등 드라이버 초기화 코드 -> [blk_mq_num_possible_queues]
 *     -> blk_mq_num_queues(cpu_possible_mask, ...)
 */
unsigned int blk_mq_num_possible_queues(unsigned int max_queues)
{
	return blk_mq_num_queues(cpu_possible_mask, max_queues);	/* [한국어] cpu_possible_mask: hotplug로 나중에 켜질 수 있는 CPU까지 포함한 전체 집합 — 이 집합 기준으로 hctx 개수 산정 */
}
EXPORT_SYMBOL_GPL(blk_mq_num_possible_queues);	/* [한국어] EXPORT_SYMBOL_GPL: GPL 라이선스 모듈(NVMe/virtio_blk 등 블록 드라이버)에서 이 심볼을 사용할 수 있도록 공개 */

/**
 * blk_mq_num_online_queues - Calc nr of queues for multiqueue devices
 * @max_queues:	The maximum number of queues the hardware/driver
 *		supports. If max_queues is 0, the argument is
 *		ignored.
 *
 * Calculates the number of queues to be used for a multiqueue
 * device based on the number of online CPUs.
 */
/*
 * [한국어]
 * blk_mq_num_online_queues - 현재 online CPU 수 기준으로 사용할 hctx 개수 계산
 *
 * @max_queues: 하드웨어/드라이버가 지원하는 hctx 개수 상한(0이면 무시).
 * @return:     online CPU 개수와 max_queues 중 0이 아닌 더 작은 값.
 *
 * cpu_possible_mask 대신 cpu_online_mask(현재 실제로 스케줄링 가능한 CPU
 * 집합)를 사용하므로, blk_mq_num_possible_queues()보다 "지금 이 순간"의
 * 상황에 맞춰 더 적게 hctx 개수를 정할 수 있다. 이후 CPU가 online되면 이미
 * 만들어진 hctx 배치가 최신 토폴로지를 반영하지 못할 수 있어, hotplug를
 * 적극적으로 고려해야 하는 드라이버는 보통 blk_mq_num_possible_queues()를
 * 선호한다. 이 저장소에 포함된 드라이버(NVMe, virtio_blk) 중 이 함수를 직접
 * 호출하는 곳은 확인되지 않았으며, possible_queues와 대칭적인 공개 API로
 * 다른 멀티큐 블록 드라이버가 사용하도록 제공된다.
 * 실행 컨텍스트: 프로세스 컨텍스트(디바이스 초기화 시점).
 *
 * 호출 체인:
 *   (멀티큐 블록 드라이버의 큐 개수 결정 코드) -> [blk_mq_num_online_queues]
 *     -> blk_mq_num_queues(cpu_online_mask, ...)
 */
unsigned int blk_mq_num_online_queues(unsigned int max_queues)
{
	return blk_mq_num_queues(cpu_online_mask, max_queues);	/* [한국어] cpu_online_mask: 현재 스케줄링 가능한 CPU 집합 — possible_mask의 부분집합(또는 동일) */
}
EXPORT_SYMBOL_GPL(blk_mq_num_online_queues);	/* [한국어] GPL 모듈에 공개되는 심볼 — possible_queues와 대칭을 이루는 공개 API */

/*
 * [한국어]
 * blk_mq_map_queues - CPU들을 hctx에 topology 기반으로 균등 매핑 (기본 구현)
 *
 * @qmap: 채워야 할 CPU->hctx 매핑 테이블. 호출 전에 qmap->nr_queues(이 map이
 *        다룰 hctx 개수)와 qmap->queue_offset(hctx 인덱스 시작점)이 이미
 *        설정되어 있어야 한다.
 * @return: 없음(void). qmap->mq_map[cpu] 배열 전체가 채워진다.
 *
 * 드라이버가 하드웨어 IRQ affinity 등 자체적인 매핑 정책을 갖지 않을 때
 * 사용하는 기본(generic) 매핑 구현이다. group_cpus_evenly()(lib/group_cpus.c
 * 선언은 linux/group_cpus.h)를 호출해 nr_queues개 그룹으로 CPU를 나누는데,
 * 이 함수는 NUMA 노드/코어 근접성을 고려해 각 그룹이 가능한 한 캐시/메모리
 * 지역성이 높은 CPU들로 구성되도록 분할한다. 그룹화에 실패하면(예: 메모리
 * 부족) 모든 CPU를 hctx 0(queue_offset 그대로)에 몰아 넣는 fallback으로
 * 최소한의 동작을 보장한다. NVMe 관점에서는 이 함수가 만든 매핑에 따라, 같은
 * 그룹에 속한 CPU들이 제출한 요청이 전부 같은 nvme_queue(SQ/CQ 쌍)로
 * 모이게 된다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 큐 초기화/재구성 시점(freeze 상태)에만
 * 호출되어 동시 호출에 대한 별도 동기화가 필요 없다.
 *
 * 호출 체인:
 *   blk_mq_update_queue_map()(blk-mq.c, set->ops->map_queues 미설정 시) ->
 *     [blk_mq_map_queues]
 *   nvme_pci_map_queues()(drivers/nvme/host/pci.c, POLL 타입이거나 IRQ
 *     offset이 없을 때) -> [blk_mq_map_queues]
 *   blk_mq_map_hw_queues()(본 파일, IRQ affinity 조회 실패 시 fallback) ->
 *     [blk_mq_map_queues]
 */
void blk_mq_map_queues(struct blk_mq_queue_map *qmap)
{
	const struct cpumask *masks;	/* [한국어] group_cpus_evenly()가 반환하는, 그룹별 CPU 집합 배열의 시작 포인터 — 그룹 수는 nr_masks에 채워짐 */
	unsigned int queue, cpu, nr_masks;	/* [한국어] queue: hctx 인덱스(0..nr_queues-1), cpu: 순회 중인 CPU 번호, nr_masks: group_cpus_evenly가 실제로 만든 그룹 수 */

	/*
	 * [한국어] group_cpus_evenly: CPU 전체를 qmap->nr_queues개(요청한 hctx 개수)
	 * 그룹으로 topology 기반 균등 분할 시도. 성공 시 masks[i]가 i번째 그룹의
	 * CPU 비트맵이고, nr_masks는 실제 생성된 그룹 수(요청한 nr_queues와 다를
	 * 수 있음 — 예: CPU 수가 nr_queues보다 적으면 그룹 수가 줄어든다)다.
	 */
	masks = group_cpus_evenly(qmap->nr_queues, &nr_masks);	/* [한국어] 실패 시 NULL 반환(예: 그룹 배열 kcalloc 실패) */
	if (!masks) {	/* [한국어] 그룹화 자체가 불가능했던 경우 진입 — topology 정보 없이 fallback 매핑 필요 */
		for_each_possible_cpu(cpu)	/* [한국어] 존재 가능한 모든 CPU를 순회 */
			qmap->mq_map[cpu] = qmap->queue_offset;	/* [한국어] 모든 CPU를 이 map의 첫 번째 hctx(queue_offset 번)로 몰아넣음 — 성능은 저하되지만 매핑 누락으로 인한 오동작은 방지 */
		return;	/* [한국어] fallback 매핑을 마쳤으므로 즉시 반환 — 아래 정상 분배 로직은 건너뜀 */
	}

	for (queue = 0; queue < qmap->nr_queues; queue++) {	/* [한국어] 이 map이 담당하는 hctx 인덱스(0..nr_queues-1)를 순서대로 처리 */
		for_each_cpu(cpu, &masks[queue % nr_masks])	/* [한국어] masks[queue % nr_masks]: nr_masks가 nr_queues보다 작을 수 있어 나머지 연산으로 그룹을 순환 재사용 — 그 그룹에 속한 CPU들을 순회 */
			qmap->mq_map[cpu] = qmap->queue_offset + queue;	/* [한국어] 이 CPU를 (queue_offset + queue)번 hctx에 배정 — offset은 여러 hctx_type이 queue_hw_ctx[] 배열을 나눠 쓸 때 이 map이 담당하는 구간의 시작점 */
	}
	kfree(masks);	/* [한국어] group_cpus_evenly가 할당한 임시 그룹 배열 해제 — qmap->mq_map 자체는 호출자가 소유하므로 여기서 해제하지 않는다 */
}
EXPORT_SYMBOL_GPL(blk_mq_map_queues);	/* [한국어] GPL 모듈(NVMe 등 블록 드라이버)이 자체 map_queues 콜백에서 fallback으로 직접 호출할 수 있도록 공개 */

/**
 * blk_mq_hw_queue_to_node - Look up the memory node for a hardware queue index
 * @qmap: CPU to hardware queue map.
 * @index: hardware queue index.
 *
 * We have no quick way of doing reverse lookups. This is only used at
 * queue init time, so runtime isn't important.
 */
/*
 * [한국어]
 * blk_mq_hw_queue_to_node - hctx 인덱스로부터 대표 NUMA 노드 역추적
 *
 * @qmap:  조회할 CPU->hctx 매핑 테이블.
 * @index: NUMA 노드를 알고 싶은 hctx 인덱스(queue_offset이 더해진 절대 인덱스).
 * @return: 그 hctx에 매핑된 CPU 중 처음 찾은 CPU의 NUMA 노드. 매핑된 CPU가
 *          하나도 없으면 NUMA_NO_NODE.
 *
 * qmap->mq_map[]은 "CPU -> hctx" 순방향 매핑만 담고 있어 "hctx -> CPU" 역방향
 * 조회를 빠르게 하는 별도 자료구조가 없다. 이 함수는 possible CPU 전체를
 * 선형 탐색해 index와 일치하는 첫 CPU를 찾는 O(nr_cpu_ids) 방식이지만, 원본
 * 주석대로 큐 초기화 시점에만 호출되므로 성능이 문제되지 않는다. 실제로는
 * blk_mq_get_hctx_node()(blk-mq.c)를 거쳐 blk_mq_alloc_rq_map()이 호출하며,
 * 그 hctx(NVMe라면 SQ/CQ 쌍)를 위한 tag/request 배열(sbitmap, rqs[])을
 * kzalloc_node()로 할당할 때 이 함수가 반환한 노드를 사용해, 그 hctx를
 * 다루는 CPU들과 같은 NUMA 노드에 메모리를 배치한다(캐시/메모리 대역폭 이득).
 * 실행 컨텍스트: 프로세스 컨텍스트, 큐/태그셋 초기화 시점에만 호출.
 *
 * 호출 체인:
 *   blk_mq_alloc_rq_map()(blk-mq.c) -> blk_mq_get_hctx_node()(blk-mq.c) ->
 *     [blk_mq_hw_queue_to_node]
 */
int blk_mq_hw_queue_to_node(struct blk_mq_queue_map *qmap, unsigned int index)
{
	int i;	/* [한국어] for_each_possible_cpu 순회에 쓰이는 CPU 인덱스 */

	for_each_possible_cpu(i) {	/* [한국어] 가능한 모든 CPU를 순서대로 검사 — hctx->CPU 역매핑 테이블이 없으므로 선형 탐색 */
		if (index == qmap->mq_map[i])	/* [한국어] CPU i가 바로 찾고 있는 hctx(index)에 매핑되어 있는지 확인 */
			return cpu_to_node(i);	/* [한국어] 일치하는 첫 CPU를 찾자마자 그 CPU가 속한 NUMA 노드를 반환하고 종료 */
	}

	return NUMA_NO_NODE;	/* [한국어] 이 index에 매핑된 CPU를 하나도 못 찾음 — 노드 힌트 없이 호출자가 기본 노드(set->numa_node 등)를 쓰도록 함 */
}

/**
 * blk_mq_map_hw_queues - Create CPU to hardware queue mapping
 * @qmap:	CPU to hardware queue map
 * @dev:	The device to map queues
 * @offset:	Queue offset to use for the device
 *
 * Create a CPU to hardware queue mapping in @qmap. The struct bus_type
 * irq_get_affinity callback will be used to retrieve the affinity.
 */
/*
 * [한국어]
 * blk_mq_map_hw_queues - 디바이스의 IRQ affinity를 이용해 CPU-hctx 매핑 생성
 *
 * @qmap:   채워야 할 CPU->hctx 매핑 테이블. nr_queues/queue_offset은 호출
 *          전에 설정되어 있어야 한다.
 * @dev:    매핑 대상 하드웨어를 나타내는 struct device (PCI 장치라면
 *          dev->bus가 pci_bus_type을 가리킨다).
 * @offset: dev의 IRQ 벡터 번호 중 이 매핑이 사용할 벡터들의 시작 오프셋
 *          (NVMe라면 admin queue가 벡터 0을 쓰는 경우 IO 큐용 오프셋은 1).
 * @return: 없음(void).
 *
 * dev->bus->irq_get_affinity 콜백(PCI라면 pci_device_irq_get_affinity() ->
 * pci_irq_get_affinity(), drivers/pci/pci-driver.c와 drivers/pci/msi/api.c)을
 * 통해 각 hctx(=하나의 MSI-X 벡터, NVMe라면 하나의 SQ/CQ 쌍)에 연결된
 * 인터럽트가 어느 CPU에서 처리되는지 알아내고, 그 CPU들을 동일 hctx에
 * 매핑한다. 이렇게 하면 어떤 CPU에서 요청을 제출(submit)하든, 그 요청의 완료
 * 인터럽트(NVMe라면 CQ 완료 인터럽트)도 같은 CPU(또는 인접 코어)에서 처리되어
 * 캐시 지역성과 인터럽트 스티어링 효율이 올라간다. affinity 콜백 자체가
 * 없거나(!dev->bus->irq_get_affinity) 특정 벡터의 affinity 조회가 실패하면
 * (poll 큐처럼 애초에 IRQ가 없는 경우 포함) blk_mq_map_queues()의 topology
 * 기반 균등 분배로 대체(fallback)한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 큐 초기화/재구성 시점.
 *
 * 호출 체인:
 *   nvme_pci_map_queues()(drivers/nvme/host/pci.c, ->map_queues 콜백,
 *   HCTX_TYPE_POLL이 아니고 IRQ offset이 있을 때) -> [blk_mq_map_hw_queues]
 *     -> (성공 시) dev->bus->irq_get_affinity == pci_device_irq_get_affinity
 *     -> (실패 시) blk_mq_map_queues()
 */
void blk_mq_map_hw_queues(struct blk_mq_queue_map *qmap,
			  struct device *dev, unsigned int offset)

{
	const struct cpumask *mask;	/* [한국어] 현재 검사 중인 hctx(벡터)의 IRQ affinity CPU 집합 — irq_get_affinity 실패 시 NULL */
	unsigned int queue, cpu;	/* [한국어] queue: hctx 인덱스(0..nr_queues-1), cpu: affinity 마스크를 순회하는 CPU 번호 */

	if (!dev->bus->irq_get_affinity)	/* [한국어] 이 버스 타입(dev->bus)이 IRQ affinity 조회 콜백 자체를 제공하지 않는 경우 — PCI가 아니거나 콜백 미등록 */
		goto fallback;	/* [한국어] affinity 정보를 얻을 방법이 없으므로 바로 균등 분배 fallback으로 이동 */

	for (queue = 0; queue < qmap->nr_queues; queue++) {	/* [한국어] 이 map이 담당하는 hctx 개수만큼 반복하며 각 벡터의 affinity를 조회 */
		mask = dev->bus->irq_get_affinity(dev, queue + offset);	/* [한국어] (queue+offset)번째 IRQ 벡터의 CPU affinity 마스크 획득 — PCI라면 pci_device_irq_get_affinity를 거쳐 MSI-X 벡터 affinity 반환 */
		if (!mask)	/* [한국어] 이 벡터에 대한 affinity 정보가 없음(예: poll 큐처럼 애초에 IRQ 미할당) */
			goto fallback;	/* [한국어] 이후 벡터까지 포함해 전부 균등 분배로 재시도 — 일부만 affinity 매핑하고 나머지만 fallback하지 않고 통째로 전환 */

		for_each_cpu(cpu, mask)	/* [한국어] 이 벡터의 인터럽트를 받을 수 있는 CPU들을 순회 */
			qmap->mq_map[cpu] = qmap->queue_offset + queue;	/* [한국어] 그 CPU를 (queue_offset+queue)번 hctx에 배정 — 이 CPU가 제출한 요청은 자신이 완료 인터럽트를 받는 hctx로 향하게 됨 */
	}

	return;	/* [한국어] 모든 벡터에 대해 affinity 기반 매핑을 성공적으로 마쳤으므로 fallback을 거치지 않고 정상 반환 */

fallback:	/* [한국어] goto fallback 도착 지점 — affinity 콜백 부재/조회 실패 시 이 레이블로 점프해 균등 분배로 대체 */
	blk_mq_map_queues(qmap);	/* [한국어] affinity 정보를 얻지 못한 경우의 대체 경로 — topology 기반 균등 분배로 최소한의 매핑을 보장 */
}
EXPORT_SYMBOL_GPL(blk_mq_map_hw_queues);	/* [한국어] GPL 모듈(NVMe 등 PCI 기반 블록 드라이버)의 ->map_queues 콜백에서 사용할 수 있도록 공개 */
