// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2016 Thomas Gleixner.
 * Copyright (C) 2016-2017 Christoph Hellwig.
 */
/*
 * [한국어 설명] 멀티큐 장치의 인터럽트를 CPU 에 고르게 펴 주는 계산기 (affinity.c)
 *
 * === 파일의 역할 ===
 * NVMe 나 최신 NIC 처럼 큐를 여러 개 갖는 장치가 인터럽트를 요청할 때, 그
 * 인터럽트들을 어느 CPU 에 배정할지 미리 계산해 준다. 실제로 배정을 수행하지는
 * 않고, "이 벡터는 이 CPU 집합" 이라는 마스크 배열을 만들어 돌려주는 것이 전부다.
 *
 * 왜 미리 계산하는가: 큐가 CPU 수만큼 있는 장치에서는 각 큐를 한 CPU 에
 * 전담시키는 것이 가장 좋다. 그러면 큐의 자료구조가 그 CPU 의 캐시에 머물고,
 * 완료 처리도 요청을 낸 CPU 에서 일어나 캐시 라인이 CPU 사이를 오가지 않는다.
 * 그 배분을 장치마다 손으로 짜는 대신 여기서 한 번에 계산한다.
 *
 * 이렇게 배정된 인터럽트를 managed 인터럽트라 부른다. 사용자가 친화도를
 * 바꿀 수 없고(바꾸면 큐와 CPU 의 대응이 깨진다), 대상 CPU 가 모두 오프라인이
 * 되면 인터럽트를 끄고 CPU 가 돌아오면 자동으로 되살린다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 인터럽트 할당의 앞단에 있다:
 *
 *   드라이버가 pci_alloc_irq_vectors_affinity(..., affd) 호출
 *     ↓ affd 에 pre/post 벡터 수와 큐 집합 구성을 담아 넘긴다
 *   irq_calc_affinity_vectors()      ← **이 파일** — 몇 개가 적당한지 계산
 *     ↓ 그 수만큼 MSI-X 벡터를 하드웨어에서 확보
 *   irq_create_affinity_masks()      ← **이 파일** — 벡터마다 CPU 마스크 생성
 *     ↓ 그 마스크를 붙여
 *   msi_domain_alloc_irqs() → irq_domain 계층 → 실제 서술자 생성
 *     ↓ desc->irq_common_data.affinity 에 마스크가 들어감
 *   irq_startup() 이 그 마스크대로 chip->irq_set_affinity() 호출
 *
 * 실행 컨텍스트: 장치 프로브, 프로세스 문맥. kzalloc 을 쓰므로 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것:
 *   lib/group_cpus.c 의 group_cpus_evenly() — 실제로 CPU 를 나누는 알고리즘.
 *     NUMA 노드와 물리 코어 구조를 고려해 균등하게 묶어 준다. 이 파일은
 *     그것을 부르고 결과를 벡터 배열에 옮기는 껍데기에 가깝다.
 *   irq_default_affinity — 친화도가 필요 없는 벡터에 넣을 기본 마스크.
 *
 * 이 파일에 의존하는 곳:
 *   drivers/pci/msi/ — pci_alloc_irq_vectors_affinity() 가 두 함수를 모두 부른다.
 *   drivers/virtio, drivers/nvme 등 — 그 PCI API 를 통해 간접적으로.
 *
 * 데이터 흐름: struct irq_affinity(요구사항) → struct irq_affinity_desc 배열
 * (계산 결과). 후자는 호출자가 kfree 로 반납할 책임을 진다.
 *
 * === 주요 함수/구조체 요약 ===
 * default_calc_sets()         — 집합 구성을 지정하지 않은 단순한 호출을 위한 기본값.
 * irq_create_affinity_masks() — 이 파일의 본체. 벡터마다의 CPU 마스크를 만든다.
 * irq_calc_affinity_vectors() — 몇 개의 벡터를 요청하는 것이 적당한지 계산한다.
 *
 * 벡터가 세 구간으로 나뉜다는 것이 이 파일을 읽는 열쇠다:
 *   [0, pre_vectors)                     — 친화도가 필요 없는 앞쪽 (관리 인터럽트 등)
 *   [pre_vectors, nvecs - post_vectors)  — 큐마다 CPU 를 배정할 구간 (managed)
 *   [nvecs - post_vectors, nvecs)        — 친화도가 필요 없는 뒤쪽
 * 가운데 구간만이 이 계산의 대상이며, 앞뒤는 기본 마스크를 받는다.
 */
#include <linux/interrupt.h>	/* [한국어] struct irq_affinity 와 irq_affinity_desc 정의 */
#include <linux/kernel.h>	/* [한국어] WARN_ON_ONCE, min() 등 기본 매크로 */
#include <linux/slab.h>	/* [한국어] kzalloc_objs/kfree — 마스크 배열 할당 */
#include <linux/cpu.h>	/* [한국어] cpu_possible_mask — 아래 벡터 수 계산에 쓴다 */
#include <linux/group_cpus.h>	/* [한국어] group_cpus_evenly() — CPU 를 균등하게 묶는 실제 알고리즘 */

/*
 * [한국어]
 * default_calc_sets - 집합 구성을 지정하지 않은 호출을 위한 기본 계산
 *
 * @affd:    친화도 요구사항 구조체. 결과를 여기에 써 넣는다.
 * @affvecs: 친화도를 배정할 벡터의 수 (앞뒤 예약분을 뺀 나머지).
 *
 * 집합(set)이란: 한 장치가 성격이 다른 큐 묶음을 여러 개 가질 수 있다.
 * 예를 들어 NVMe 는 기본 I/O 큐와 폴링 전용 큐를 따로 두는데, 각각을
 * 독립적으로 CPU 에 펴야 한다. 그 묶음 하나가 집합이다.
 *
 * 대부분의 장치는 그런 구분이 없어 큐가 한 종류뿐이다. 그런 호출자는
 * calc_sets 콜백을 채우지 않고, 아래 create_affinity_masks 가 이 함수를
 * 대신 꽂아 준다. 결과는 "집합 하나에 전부" 다.
 *
 * 왜 콜백으로 두는가: 집합의 크기는 실제로 확보된 벡터 수에 따라 달라진다.
 * 드라이버가 요청한 만큼 못 받을 수 있어, 그때 어떻게 나눌지는 드라이버만
 * 안다. 그래서 계산 시점에 드라이버에게 되물어보는 구조다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   irq_create_affinity_masks() → affd->calc_sets → [default_calc_sets]
 */
static void default_calc_sets(struct irq_affinity *affd, unsigned int affvecs)
{
	affd->nr_sets = 1;	/* [한국어] 집합은 하나뿐이다 */
	affd->set_size[0] = affvecs;	/* [한국어] 그 하나에 배정 가능한 벡터를 전부 넣는다 */
}

/**
 * irq_create_affinity_masks - Create affinity masks for multiqueue spreading
 * @nvecs:	The total number of vectors
 * @affd:	Description of the affinity requirements
 *
 * Returns the irq_affinity_desc pointer or NULL if allocation failed.
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_create_affinity_masks - 벡터마다의 CPU 친화도 마스크 배열을 만든다
 *
 * @nvecs:  전체 벡터 수 (앞뒤 예약분 포함).
 * @affd:   친화도 요구사항. pre_vectors, post_vectors, 집합 구성이 들어 있고,
 *          calc_sets 콜백이 비어 있으면 이 함수가 기본값을 꽂는다 — 즉
 *          입력이면서 동시에 출력이기도 하다.
 * @return: nvecs 개짜리 irq_affinity_desc 배열, 또는 실패 시 NULL.
 *          호출자가 kfree 로 반납해야 한다.
 *
 * 벡터를 세 구간으로 나누어 처리한다:
 *   앞쪽 pre_vectors 개   — 기본 친화도. 관리용 인터럽트처럼 특정 큐에
 *                           속하지 않는 것들이다.
 *   가운데 affvecs 개     — group_cpus_evenly() 로 CPU 를 고르게 나눠 배정.
 *                           이 구간만 managed 로 표시된다.
 *   뒤쪽 post_vectors 개  — 다시 기본 친화도.
 *
 * NULL 을 돌려주는 경우가 셋이라는 점에 주의한다. 할당 실패뿐 아니라
 * "배정할 벡터가 없다"와 "집합 수가 한도를 넘었다"도 NULL 이다. 호출자
 * (PCI MSI 계층)는 NULL 을 오류로 다루지 않고 "친화도 지정 없이 진행" 으로
 * 해석한다 — 그래서 세 경우를 구분할 필요가 없다.
 *
 * 실행 컨텍스트: 장치 프로브, 프로세스 문맥. kzalloc 으로 잠들 수 있다.
 *
 * 에러 경로: 중간에 group_cpus_evenly() 가 실패하면 이미 잡은 masks 를
 * 반납하고 NULL 을 준다. 부분적으로 채워진 배열을 넘기지 않는다.
 *
 * 호출 체인:
 *   pci_alloc_irq_vectors_affinity() → __pci_enable_msix_range()
 *     → [irq_create_affinity_masks] → group_cpus_evenly() (lib/group_cpus.c)
 */
struct irq_affinity_desc *
irq_create_affinity_masks(unsigned int nvecs, struct irq_affinity *affd)
{
	unsigned int affvecs, curvec, usedvecs, i;	/* [한국어] 차례로: 친화도 배정 대상 벡터 수, 지금 채우는 위치, 실제로 배정한 수, 순회 변수 */
	struct irq_affinity_desc *masks = NULL;	/* [한국어] 돌려줄 결과 배열. 실패 시 NULL 로 남는다 */

	/*
	 * Determine the number of vectors which need interrupt affinities
	 * assigned. If the pre/post request exhausts the available vectors
	 * then nothing to do here except for invoking the calc_sets()
	 * callback so the device driver can adjust to the situation.
	 */
	/* [한국어] (위 영어 주석에 이어) 가운데 구간의 크기를 구한다.
	 * 앞뒤 예약분이 전체보다 크거나 같으면 배정할 것이 없다. 그래도 아래
	 * calc_sets 콜백은 불러 준다 — 영어 주석대로, 드라이버가 그 상황을 알고
	 * 큐 구성을 조정할 기회를 주기 위해서다. */
	if (nvecs > affd->pre_vectors + affd->post_vectors)	/* [한국어] 예약분을 빼고도 남는 벡터가 있는가 */
		affvecs = nvecs - affd->pre_vectors - affd->post_vectors;	/* [한국어] 그 나머지가 배정 대상이다 */
	else
		affvecs = 0;	/* [한국어] 예약분이 전부를 차지했다. 배정할 것이 없지만 콜백은 부른다 */

	/*
	 * Simple invocations do not provide a calc_sets() callback. Install
	 * the generic one.
	 */
	/* [한국어] (위 영어 주석에 이어) 콜백이 없으면 기본 구현을 꽂는다.
	 * affd 를 고쳐 쓴다는 점에 주의 — 이 함수는 인자를 읽기만 하지 않는다.
	 * 호출자가 같은 affd 를 다시 쓰면 콜백이 이미 채워진 상태로 넘어간다. */
	if (!affd->calc_sets)	/* [한국어] 집합 구성을 스스로 정하지 않는 단순한 호출인가 */
		affd->calc_sets = default_calc_sets;	/* [한국어] "집합 하나에 전부" 를 꽂아 준다 */

	/* Recalculate the sets */
	/* [한국어] (위 영어 주석) 실제로 확보된 벡터 수에 맞춰 집합 크기를 다시 정한다.
	 * 드라이버가 요청한 만큼 벡터를 받지 못했을 수 있어, 이 시점의 affvecs 로
	 * 다시 물어보는 것이다. */
	affd->calc_sets(affd, affvecs);	/* [한국어] 드라이버(또는 위 기본 구현)가 nr_sets 와 set_size[] 를 채운다 */

	if (WARN_ON_ONCE(affd->nr_sets > IRQ_AFFINITY_MAX_SETS))	/* [한국어] 콜백이 한도를 넘는 집합 수를 돌려주면 드라이버 버그다. set_size 배열이 그만큼밖에 없어 넘으면 배열 밖을 읽는다 */
		return NULL;	/* [한국어] 한 번만 경고하고 친화도 지정을 포기한다 */

	/* Nothing to assign? */
	if (!affvecs)	/* [한국어] (위 영어 주석) 배정할 벡터가 없으면 */
		return NULL;	/* [한국어] 마스크 배열을 만들 이유가 없다. 호출자는 이것을 오류가 아니라 "친화도 없이 진행" 으로 읽는다 */

	masks = kzalloc_objs(*masks, nvecs);	/* [한국어] 전체 벡터 수만큼 잡는다. 가운데 구간만 계산하더라도 앞뒤에도 기본 마스크를 채워야 해서 전부 필요하다 */
	if (!masks)	/* [한국어] 할당 실패 */
		return NULL;	/* [한국어] 여기서는 아직 잡은 것이 없어 그냥 돌아가면 된다 */

	/* Fill out vectors at the beginning that don't need affinity */
	/* [한국어] (위 영어 주석) 앞쪽 예약 구간을 기본 친화도로 채운다.
	 * 이 벡터들은 특정 큐에 속하지 않는 관리용 인터럽트라, 어느 CPU 로 가도
	 * 상관없다. irq_default_affinity 는 부트 인자나 sysfs 로 정해지는 전역 기본값이다. */
	for (curvec = 0; curvec < affd->pre_vectors; curvec++)	/* [한국어] curvec 이 여기서 시작해 아래까지 이어진다 — 채운 위치를 계속 따라간다 */
		cpumask_copy(&masks[curvec].mask, irq_default_affinity);	/* [한국어] 전역 기본 마스크를 복사한다. is_managed 는 kzalloc 덕에 0 으로 남아, 사용자가 나중에 바꿀 수 있다 */

	/*
	 * Spread on present CPUs starting from affd->pre_vectors. If we
	 * have multiple sets, build each sets affinity mask separately.
	 */
	/* [한국어] (위 영어 주석에 이어) 집합마다 따로 CPU 를 나눈다.
	 * 왜 집합마다 따로인가: 각 집합이 독립된 큐 묶음이라, 각각이 모든 CPU 를
	 * 덮어야 한다. 전체를 한 번에 나누면 집합 하나가 일부 CPU 만 담당하게 된다. */
	for (i = 0, usedvecs = 0; i < affd->nr_sets; i++) {	/* [한국어] 집합을 하나씩. usedvecs 는 실제로 마스크를 만든 벡터 수를 누적한다 */
		unsigned int nr_masks, this_vecs = affd->set_size[i];	/* [한국어] 이 집합에 배정된 벡터 수. nr_masks 는 아래 호출이 채워 준다 */
		struct cpumask *result = group_cpus_evenly(this_vecs, &nr_masks);	/* [한국어] CPU 를 this_vecs 개 그룹으로 나눈다. NUMA 노드와 물리 코어를 고려해 균등하게 묶는다 */

		if (!result) {	/* [한국어] 할당 실패 */
			kfree(masks);	/* [한국어] 이미 잡아 둔 결과 배열을 반납한다 — 부분적으로 채워진 것을 넘기지 않는다 */
			return NULL;	/* [한국어] 호출자는 친화도 없이 진행한다 */
		}

		for (int j = 0; j < nr_masks; j++)	/* [한국어] nr_masks 가 this_vecs 보다 작을 수 있다 — CPU 수가 적으면 그룹이 덜 만들어진다 */
			cpumask_copy(&masks[curvec + j].mask, &result[j]);	/* [한국어] 그룹 하나를 벡터 하나에 대응시킨다 */
		kfree(result);	/* [한국어] group_cpus_evenly 가 잡아 준 임시 배열을 반납한다 */

		curvec += nr_masks;	/* [한국어] 채운 만큼 위치를 민다 */
		usedvecs += nr_masks;	/* [한국어] 실제 사용량을 누적한다. 아래에서 뒤쪽 구간의 시작을 정하는 데 쓴다 */
	}

	/* Fill out vectors at the end that don't need affinity */
	/* [한국어] (위 영어 주석) 뒤쪽 구간을 기본 친화도로 채운다.
	 * 시작 위치를 두 갈래로 정하는 이유: CPU 수가 요청한 벡터 수보다 적으면
	 * 위 루프가 벡터를 다 쓰지 못한다(usedvecs < affvecs). 그러면 남은 가운데
	 * 벡터들도 기본 친화도로 채워야 하므로 시작을 앞당긴다. */
	if (usedvecs >= affvecs)	/* [한국어] 가운데 구간을 다 썼는가 */
		curvec = affd->pre_vectors + affvecs;	/* [한국어] 다 썼으면 뒤쪽 구간의 진짜 시작부터 */
	else
		curvec = affd->pre_vectors + usedvecs;	/* [한국어] 남았으면 그 남은 자리부터 함께 채운다 */
	for (; curvec < nvecs; curvec++)	/* [한국어] 끝까지 */
		cpumask_copy(&masks[curvec].mask, irq_default_affinity);	/* [한국어] 기본 마스크를 복사한다 */

	/* Mark the managed interrupts */
	/* [한국어] (위 영어 주석) 가운데 구간을 managed 로 표시한다.
	 *
	 * managed 의 뜻: 커널이 친화도를 관리하며 사용자가 바꿀 수 없다. 큐와
	 * CPU 의 대응이 성능의 전제라, 사용자가 옮기면 그 전제가 깨지기 때문이다.
	 * 또 대상 CPU 가 전부 오프라인이 되면 인터럽트를 끄고(managed shutdown),
	 * CPU 가 돌아오면 자동으로 되살린다.
	 *
	 * 위 usedvecs 와 무관하게 pre_vectors 부터 post_vectors 앞까지 전부
	 * 표시한다는 점에 주의한다. 위에서 기본 친화도를 받은 남은 벡터도
	 * managed 가 되는데, 그것들도 큐에 속한 벡터라는 사실은 변하지 않기 때문이다. */
	for (i = affd->pre_vectors; i < nvecs - affd->post_vectors; i++)	/* [한국어] 앞뒤 예약분을 뺀 가운데 전부 */
		masks[i].is_managed = 1;	/* [한국어] 사용자가 친화도를 바꿀 수 없고, CPU 핫플러그에 자동으로 대응하는 인터럽트가 된다 */

	return masks;	/* [한국어] 호출자가 kfree 로 반납할 책임을 진다 */
}

/**
 * irq_calc_affinity_vectors - Calculate the optimal number of vectors
 * @minvec:	The minimum number of vectors available
 * @maxvec:	The maximum number of vectors available
 * @affd:	Description of the affinity requirements
 */
/*
 * [한국어] (위 kernel-doc 에 이어)
 * irq_calc_affinity_vectors - 요청하기에 적당한 벡터 수를 계산한다
 *
 * @minvec: 최소한 이만큼은 있어야 한다는 하한.
 * @maxvec: 하드웨어가 줄 수 있는 상한.
 * @affd:   친화도 요구사항.
 * @return: 요청할 벡터 수. 0 이면 요구를 만족할 수 없다는 뜻이다.
 *
 * 왜 이 계산이 따로 필요한가: MSI-X 벡터는 유한한 자원이다. 무작정 최대치를
 * 요청하면 다른 장치가 쓸 것이 없어지고, 너무 적게 요청하면 큐가 CPU 를
 * 다 덮지 못한다. 적당한 지점이 "CPU 수만큼" 이며, 그것을 여기서 구한다.
 *
 * 위 create_affinity_masks 와 짝을 이루어 쓰인다. 이쪽으로 몇 개를 요청할지
 * 정하고, 하드웨어에서 실제로 확보한 뒤, 그 수로 저쪽을 불러 마스크를 만든다.
 *
 * calc_sets 유무로 갈리는 것이 이 함수의 유일한 분기다. 그 이유는 아래 주석에 있다.
 *
 * 실행 컨텍스트: 장치 프로브, 프로세스 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   pci_alloc_irq_vectors_affinity() (drivers/pci/msi/api.c) → [이 함수]
 */
unsigned int irq_calc_affinity_vectors(unsigned int minvec, unsigned int maxvec,
				       const struct irq_affinity *affd)
{
	unsigned int resv = affd->pre_vectors + affd->post_vectors;	/* [한국어] 친화도 배정 대상이 아닌 앞뒤 예약분의 합 */
	unsigned int set_vecs;	/* [한국어] 큐에 배정할 벡터 수 */

	if (resv > minvec)	/* [한국어] 예약분만으로도 하한을 넘어서면 */
		return 0;	/* [한국어] 큐에 줄 벡터가 남지 않는다. 0 은 "이 요구를 만족할 수 없다"는 뜻이며 호출자가 요청을 포기한다 */

	/* [한국어] 아래 분기가 이 함수의 핵심이다.
	 *
	 * calc_sets 가 있으면 드라이버가 집합 구성을 스스로 정한다는 뜻이고,
	 * 그런 드라이버는 CPU 수와 다른 기준(큐 종류별 개수 등)으로 벡터를
	 * 나눈다. 그래서 커널이 CPU 수로 제한할 근거가 없어 최대치를 다 준다.
	 *
	 * 없으면 단순한 "큐 하나에 CPU 하나" 구성이므로, CPU 수보다 많은
	 * 벡터는 낭비다. 그래서 cpu_possible_mask 의 크기로 제한한다.
	 *
	 * possible 을 쓰는 것에 주의 — online 이 아니다. 지금 꺼져 있는 CPU 도
	 * 나중에 켜질 수 있고, 그때 담당할 큐가 있어야 하기 때문이다. */
	if (affd->calc_sets)	/* [한국어] 드라이버가 집합 구성을 직접 정하는가 */
		set_vecs = maxvec - resv;	/* [한국어] 그렇다면 커널이 제한할 근거가 없어 최대치를 준다 */
	else
		set_vecs = cpumask_weight(cpu_possible_mask);	/* [한국어] 단순 구성이면 CPU 수가 상한이다. online 이 아니라 possible 인 이유는 위 주석에 있다 */

	return resv + min(set_vecs, maxvec - resv);	/* [한국어] 예약분에 큐 몫을 더한다. 큐 몫은 하드웨어 상한을 넘을 수 없어 min 으로 자른다 */
}
