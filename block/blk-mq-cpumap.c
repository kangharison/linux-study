// SPDX-License-Identifier: GPL-2.0
/*
 * CPU <-> hardware queue mapping helpers
 *
 * Copyright (C) 2013-2014 Jens Axboe
 */

/*
 * 파일 상단 요약 (NVMe SSD 관점)
 * --------------------------------
 * 이 파일은 CPU와 blk-mq hardware queue 간 1:1/N:1 매핑을 만드는 헬퍼를 제공한다.
 * NVMe SSD 입장에서 각 hardware queue는 하나의 Submission Queue(SQ)와
 * Completion Queue(CQ) 쌍으로 표현되는 nvme_queue의 처리 단위이므로,
 * 여기서 만드는 매핑은 "어떤 CPU가 어떤 SQ에 doorbell을 울리며 CID를 할당받는가"를
 * 결정한다. blk-mq core와 NVMe 드라이버 사이의 연결 고리이며, 실제 SQ/CQ 생성은
 * drivers/nvme/host/pci.c 쪽 nvme_create_io_queues() -> blk_mq_alloc_tag_set()
 * 경로에서 이 매핑을 사용한다 (추정).
 *
 * 참조 흐름:
 * nvme_probe -> nvme_reset_work -> nvme_setup_io_queues ->
 * nvme_create_io_queues -> blk_mq_alloc_tag_set -> blk_mq_map_queues
 * 런타임 IO:
 * blk_mq_submit_bio -> blk_mq_get_request -> blk_mq_run_hw_queue ->
 * nvme_queue_rq -> nvme_submit_cmd(doorbell)
 */

#include <linux/kernel.h>	/* NVMe driver가 doorbell/SQ 엔트리 갱신 시 사용하는 기본 매크로 제공 (추정) */
#include <linux/threads.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/smp.h>		/* CPU 간 매핑과 SMP 메모리 모델; NVMe doorbell 가시성과 연결 (추정) */
#include <linux/cpu.h>
#include <linux/group_cpus.h>	/* CPU들을 NVMe SQ/CQ 쌍 수만큼 균등 그룹화 */
#include <linux/device/bus.h>	/* dev->bus->irq_get_affinity: PCIe MSI-X affinity 획득 인터페이스 */
#include <linux/sched/isolation.h>

#include "blk.h"
#include "blk-mq.h"		/* struct blk_mq_queue_map 및 blk-mq core 정의; NVMe queue set 할당 시 사용 */

/*
 * struct blk_mq_queue_map 필드와 NVMe 동작의 관계
 * -------------------------------------------------
 * @mq_map:       CPU 인덱스 -> hardware queue 인덱스 변환 테이블.
 *                NVMe에서는 이 인덱스가 nvme_queue 배열의 인덱스가 되어
 *                특정 CPU의 요청이 어느 SQ/CQ 쌍으로 향할지 결정한다.
 * @nr_queues:    사용할 hardware queue 개수. NVMe CAP.MQES와 드라이버가
 *                생성할 SQ/CQ 쌍의 상한을 반영한다.
 * @queue_offset: hardware queue 번호의 시작점. NVMe 드라이버는 읽기/쓰기/
 *                poll 등 hctx_type별로 별도의 queue set을 두고, 각 set이
 *                nvme_queue 배열에서 어느 위치부터 시작할지 표시한다.
 */

/*
 * blk_mq_num_queues - CPU 마스크와 최대 queue 수를 기반으로 사용할 queue 수 산출
 * @mask:       고려할 CPU 집합(cpu_possible_mask 또는 cpu_online_mask)
 * @max_queues: 하드웨어/드라이버가 지원하는 최대 queue 수
 *
 * 목적:
 *   NVMe 컨트롤러가 expose한 queue 개수보다 CPU 수가 많으면 queue 수로 제한하고,
 *   CPU 수가 더 적으면 CPU 수만큼만 queue를 만든다. 불필요한 빈 SQ/CQ 쌍 생성을
 *   막아 메모리와 doorbell 레지스터 낭비를 줄인다.
 *
 * 호출 경로 (추정):
 *   blk_mq_num_possible_queues/blk_mq_num_online_queues -> blk_mq_num_queues
 *   -> blk_mq_alloc_tag_set (NVMe queue set 할당 시)
 */
static unsigned int blk_mq_num_queues(const struct cpumask *mask,
				      unsigned int max_queues)
{
	/* SQ/CQ 쌍 개수 후보; 최종값은 NVMe tag set의 queue 수(queue depth 분모)로 반영됨 */
	unsigned int num;

	/* CPU 개수를 센다: 이 값이 NVMe에서 생성할 SQ/CQ 쌍의 후보 개수가 된다. */
	num = cpumask_weight(mask);
	/* 0이 아닌 값 중 작은 쪽 선택: NVMe max queue 수와 CPU 수 중 작은 값 사용 */
	return min_not_zero(num, max_queues);	/* queue 수가 적으면 CID/tag 경쟁 심화, 많으면 메모리/doorbell 레지스터 낭비 (추정) */
}

/**
 * blk_mq_num_possible_queues - Calc nr of queues for multiqueue devices
 * @max_queues:	The maximum number of queues the hardware/driver
 *		supports. If max_queues is 0, the argument is
 *		ignored.
 *
 * Calculates the number of queues to be used for a multiqueue
 * device based on the number of possible CPUs.
 *
 * NVMe 관점:
 *   boot 가능한 모든 CPU(possible CPUs)를 기준으로 SQ/CQ 쌍 수를 계산한다.
 *   hotplug로 CPU가 추가될 경우에도 컨트롤러가 감당할 수 있는 queue 수를
 *   미리 확보하기 위해 사용된다.
 *
 * 호출 경로 (추정):
 *   nvme_setup_io_queues -> blk_mq_alloc_tag_set ->
 *   blk_mq_num_possible_queues
 */
unsigned int blk_mq_num_possible_queues(unsigned int max_queues)
{
	/* 가능한 모든 CPU 기준으로 SQ/CQ 쌍 수 산출; NVMe 컨트롤러 초기화 시 queue 자원 확보에 사용 */
	return blk_mq_num_queues(cpu_possible_mask, max_queues);
}
EXPORT_SYMBOL_GPL(blk_mq_num_possible_queues);

/**
 * blk_mq_num_online_queues - Calc nr of queues for multiqueue devices
 * @max_queues:	The maximum number of queues the hardware/driver
 *		supports. If max_queues is 0, the argument is
 *		ignored.
 *
 * Calculates the number of queues to be used for a multiqueue
 * device based on the number of online CPUs.
 *
 * NVMe 관점:
 *   현재 online인 CPU만 기준으로 SQ/CQ 쌍 수를 계산한다. CPU hotplug로
 *   offline된 코어에 대해서는 불필요한 nvme_queue를 할당하지 않는다.
 */
unsigned int blk_mq_num_online_queues(unsigned int max_queues)
{
	/* 현재 online CPU 기준; offline CPU에는 doorbell/CID 경쟁을 줄이기 위해 별도 SQ를 두지 않음 */
	return blk_mq_num_queues(cpu_online_mask, max_queues);
}
EXPORT_SYMBOL_GPL(blk_mq_num_online_queues);

/*
 * blk_mq_map_queues - CPU들을 hardware queue에 고르게 분배
 * @qmap: CPU -> hardware queue 매핑 구조체
 *
 * 목적:
 *   CPU 부하를 가능한 한 균등하게 여러 SQ에 분산시켜, 특정 nvme_queue의
 *   doorbell/SQ 엔트리에 병목이 생기지 않도록 한다.
 *
 * NVMe 연결:
 *   결과로 채워진 qmap->mq_map[cpu] 값이 nvme_queue 인덱스가 된다.
 *   이후 blk_mq_submit_bio -> blk_mq_get_request 시 현재 CPU가 이 테이블을
 *   통해 자신의 blk_mq_hw_ctx를 찾고, nvme_queue_rq에서 대응하는 SQ로
 *   CID를 할당해 doorbell을 울린다.
 *
 * 호출 경로:
 *   blk_mq_map_hw_queues(IRQ affinity 실패 시 fallback) -> blk_mq_map_queues
 *   또는 blk_mq_alloc_tag_set -> blk_mq_map_swqueue -> blk_mq_map_queues (추정)
 */
void blk_mq_map_queues(struct blk_mq_queue_map *qmap)
{
	/* 각 그룹은 하나의 NVMe SQ/CQ 쌍을 공유할 CPU 집합 */
	const struct cpumask *masks;
	/* queue: SQ/CQ 인덱스, cpu: doorbell 발행자 및 CQ ISR 처리자, nr_masks: 그룹 수 */
	unsigned int queue, cpu, nr_masks;

	/*
	 * group_cpus_evenly: CPU들을 nr_queues개 hardware queue에 균등 분할.
	 * NVMe 입장에서는 SQ/CQ 쌍 개수만큼 CPU 그룹을 만들어 각 그룹이 하나의
	 * nvme_queue를 공유하도록 한다.
	 */
	masks = group_cpus_evenly(qmap->nr_queues, &nr_masks);	/* nr_masks == qmap->nr_queues 또는 그 인수; SQ/CQ당 doorbell 부하 분산 목표 */
	if (!masks) {
		/*
		 * 균등 분배 실패(예: 메모리 부족) 시 모든 CPU를 offset이 가리키는
		 * 첫 번째 hardware queue(SQ 0번)에 매핑한다. 성능은 저하되지만
		 * NVMe 동작은 계속 유지된다 (fallback).
		 */
		/* 모든 가능 CPU를 SQ 0으로 집중시키므로 doorbell/CID 경쟁 급증 (추정) */
		for_each_possible_cpu(cpu)
			qmap->mq_map[cpu] = qmap->queue_offset;	/* SQ 0에 대한 doorbell/CID 경쟁이 급증하지만 동작 보장 */
		return;
	}

	/*
	 * masks[queue % nr_masks]에 속한 CPU들을 queue번 hardware queue에 배정.
	 * queue_offset이 더해지면 NVMe 드라이버가 읽기/쓰기/POLL queue set을
	 * 분리했을 때 올바른 nvme_queue 인덱스를 얻는다.
	 */
	/* 각 SQ/CQ 쌍에 CPU 그룹 할당; 그룹 내 CPU는 동일 nvme_queue 공유 */
	for (queue = 0; queue < qmap->nr_queues; queue++) {
		/* 이 그룹의 CPU들은 동일 nvme_queue로 IO 집중 -> 동일 SQ doorbell, 동일 CQ 완료 */
		for_each_cpu(cpu, &masks[queue % nr_masks])
			qmap->mq_map[cpu] = qmap->queue_offset + queue;	/* CPU -> nvme_queue 인덱스 매핑 확정; 이 값으로 SQ 선택 및 CID/tag 공간 결정 */
	}
	kfree(masks);	/* 임시 CPU 그룹 메모리 해제; 매핑 테이블 qmap->mq_map은 NVMe 드라이버가 계속 참조 */
}
EXPORT_SYMBOL_GPL(blk_mq_map_queues);

/**
 * blk_mq_hw_queue_to_node - Look up the memory node for a hardware queue index
 * @qmap: CPU to hardware queue map.
 * @index: hardware queue index.
 *
 * We have no quick way of doing reverse lookups. This is only used at
 * queue init time, so runtime isn't important.
 *
 * NVMe 관점:
 *   hardware queue(SQ/CQ 쌍)에 해당하는 NUMA node를 찾아, NVMe driver가
 *   해당 nvme_queue의 메모리(SQ/CQ ring, PRP list 등)를 로컬 NUMA node에
 *   할당할 수 있도록 힌트를 제공한다. 로컬 메모리 할당은 doorbell/PRP/SGL
 *   접근 지연을 줄여준다.
 *
 * 호출 경로 (추정):
 *   nvme_alloc_queue -> blk_mq_hw_queue_to_node
 */
int blk_mq_hw_queue_to_node(struct blk_mq_queue_map *qmap, unsigned int index)
{
	/* NUMA node를 역추적하기 위해 매핑된 CPU를 선형 검색 */
	int i;

	/* CPU 순회하며 해당 hardware queue에 매핑된 첫 CPU의 NUMA node 반환 */
	/* 가능 CPU 중 이 SQ/CQ를 사용하는 첫 CPU 탐색 */
	for_each_possible_cpu(i) {
		/* CPU i가 index번 nvme_queue(SQ/CQ)에 배정되었는가? */
		if (index == qmap->mq_map[i])
			/* 해당 CPU의 NUMA node 반환 -> nvme_alloc_queue가 SQ/CQ ring, PRP list 할당 시 local node 우선 사용 */
			return cpu_to_node(i);
	}

	/* 매핑된 CPU가 없으면 NUMA node 정보 없음 */
	/* 로컬 NUMA 힌트 없이 기본 노드에 SQ/CQ 메모리 할당; doorbell/PRP 접근 지연 증가 가능 (추정) */
	return NUMA_NO_NODE;
}

/**
 * blk_mq_map_hw_queues - Create CPU to hardware queue mapping
 * @qmap:	CPU to hardware queue map
 * @dev:	The device to map queues
 * @offset:	Queue offset to use for the device
 *
 * Create a CPU to hardware queue mapping in @qmap. The struct bus_type
 * irq_get_affinity callback will be used to retrieve the affinity.
 *
 * NVMe 관점:
 *   PCIe MSI-X 벡터의 IRQ affinity 마스크를 이용해 CPU와 SQ/CQ 쌍을 정렬한다.
 *   이로 인해 특정 CPU에서 발행한 IO는 동일 CPU에서 처리되는 CQ MSI-X
 *   인터럽트를 받게 되어 cache locality와 interrupt steering 효율이 향상된다.
 *   MSI-X affinity 정보를 얻을 수 없으면 blk_mq_map_queues()로 fallback한다.
 *
 * 호출 경로 (추정):
 *   nvme_setup_io_queues -> nvme_setup_irqs -> pci_irq_vector ->
 *   blk_mq_map_hw_queues
 *
 * 런타임 완료 경로:
 *   nvme_irq -> nvme_poll_cq -> nvme_complete_rq -> blk_mq_end_request
 */
void blk_mq_map_hw_queues(struct blk_mq_queue_map *qmap,
			  struct device *dev, unsigned int offset)

{
	/* 특정 NVMe CQ에 연결된 MSI-X 인터럽트의 affinity CPU 집합 */
	const struct cpumask *mask;
	/* queue: SQ/CQ(==MSI-X vector) 인덱스, cpu: submit/complete 처리 CPU */
	unsigned int queue, cpu;

	/* bus 드라이버가 IRQ affinity를 제공하지 않으면 균등 분배 fallback */
	/* PCIe bus가 MSI-X affinity를 노출하지 않으면 submit CPU와 CQ ISR CPU가 일치하지 않을 수 있음 (추정) */
	if (!dev->bus->irq_get_affinity)
		goto fallback;

	/*
	 * 각 hardware queue(SQ/CQ 쌍)에 할당된 MSI-X 인터럽트의 affinity 마스크를
	 * 읽어와, 해당 인터럽트를 받을 CPU들을 같은 nvme_queue로 묶는다.
	 */
	/* 각 SQ/CQ 쌍에 대해 MSI-X affinity 기반 매핑 수행 */
	for (queue = 0; queue < qmap->nr_queues; queue++) {
		/* queue+offset번 MSI-X vector의 affinity 마스크 획득 -> 해당 CQ 완료 인터럽트가 도달할 CPU 결정 */
		mask = dev->bus->irq_get_affinity(dev, queue + offset);
		/* affinity 마스크 획득 실패 시 균등 분배로 대체; CQ ISR이 다른 NUMA/코어에서 처리될 수 있음 */
		if (!mask)
			goto fallback;

		/* affinity 마스크에 포함된 CPU들을 현재 queue에 매핑 */
		/* 이 CPU들의 IO는 현재 queue의 SQ로 submit되고 동일/근접 CPU에서 CQ ISR 수신 */
		for_each_cpu(cpu, mask)
			qmap->mq_map[cpu] = qmap->queue_offset + queue;	/* CPU를 CQ MSI-X affinity와 동일한 SQ/CQ 쌍에 고정 -> nvme_irq -> nvme_poll_cq 완료 경로의 캐시 지역성 향상 */
	}

	return;

fallback:
	/* MSI-X affinity 없이 균등 분배; NVMe submit 경로와 CQ 완료 경로가 다른 CPU에 퍼질 가능성 (추정) */
	blk_mq_map_queues(qmap);
}
EXPORT_SYMBOL_GPL(blk_mq_map_hw_queues);

/*
 * 이 파일은 논리적으로 block/blk-mq.c(태그 세트 및 queue 할당),
 * block/blk-mq-sched.c(scheduler와 hw_ctx 연결), 그리고
 * drivers/nvme/host/pci.c(NVMe SQ/CQ 및 MSI-X 설정) 다음 단계에서
 * 실행된다. 즉, blk-mq core가 queue 수와 CPU 배치를 결정한 뒤 NVMe 드라이버가
 * 실제 nvme_queue/SQ/CQ/doorbell 자원을 생성하기 직전의 매핑 단계이다.
 */

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 CPU -> blk_mq_hw_ctx -> NVMe nvme_queue(SQ/CQ) 매핑을 결정한다.
 *   매핑 결과는 어떤 CPU가 어느 SQ에 doorbell을 울리는지를 직접 좌우한다.
 *
 * - blk_mq_map_hw_queues()는 PCIe MSI-X IRQ affinity를 존중하여 CPU와
 *   인터럽트(CQ 완료) 처리를 같은 NUMA/코어 영역에 배치해 locality를 극대화한다.
 *   MSI-X 정보를 얻지 못하면 blk_mq_map_queues()로 고르게 분배한다.
 *
 * - blk_mq_hw_queue_to_node()는 각 SQ/CQ 쌍의 메모리를 로컬 NUMA node에
 *   할당할 수 있도록 힌트를 제공하며, PRP/SGL 및 doorbell 레지스터 접근
 *   지연에 영향을 준다.
 *
 * - blk_mq_num_*_queues()는 CPU 수와 NVMe 컨트롤러 최대 queue 수(CAP.MQES
 *   기반 드라이버 상한) 중 작은 값을 선택해 불필요한 SQ/CQ 쌍 생성을 방지한다.
 *
 * - 전체 IO 흐름에서 이 파일은 요청 경로의 시작점(Submit)과 완료 경로의
 *   인터럽트 affinity 모두에 영향을 주는 blk-mq core의 핵심 구성 요소이다.
 */
