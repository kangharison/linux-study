// SPDX-License-Identifier: GPL-2.0
/*
 * Virtio driver for the paravirtualized IOMMU
 *
 * Copyright (C) 2019 Arm Limited
 */

/*
 * [한국어 설명] 반가상화 IOMMU의 virtio 드라이버 (virtio-iommu.c)
 *
 * === 파일의 역할 ===
 * 게스트 커널이 하이퍼바이저에게 "이 주소를 매핑해 달라"고 **부탁하는**
 * IOMMU 드라이버다. 다른 드라이버들이 페이지 테이블을 만들고
 * 레지스터를 써서 하드웨어를 직접 부리는 것과 달리, 이 드라이버는
 * 요청 메시지를 virtqueue에 넣어 보낸다. 실제 변환 구조는
 * 하이퍼바이저(또는 QEMU 같은 VMM) 안에 있고 게스트는 볼 수 없다.
 *
 * 그래서 이 파일의 관심사는 "페이지 테이블을 어떻게 걷는가"가 아니라
 * **"요청을 어떻게 묶고, 언제 보내고, 언제 완료를 기다리는가"** 다.
 * 이해에 필요한 개념이 넷이다.
 *
 * (1) **두 개의 virtqueue.** request 큐는 게스트 → 장치 방향으로
 *     MAP/UNMAP/ATTACH/DETACH/PROBE 요청을 보낸다. event 큐는 반대
 *     방향으로, 장치가 폴트를 알려 온다. event 큐는 게스트가 미리
 *     빈 버퍼를 채워 넣어 두는 방식이다(viommu_fill_evtq).
 *
 * (2) **지연 전송.** viommu_add_req()는 큐에 넣기만 하고 kick 하지
 *     않는다. 실제 전송과 완료 대기는 viommu_sync_req()에서 일어나며,
 *     그것은 코어의 iotlb_sync 콜백에서 불린다. 즉 map/unmap 한 번의
 *     비용이 아니라 **한 묶음의 비용**을 내는 구조다. 큐가 꽉 찼을
 *     때만 예외적으로 중간에 동기화한다.
 *
 * (3) **매핑의 그림자 트리.** 도메인마다 interval tree에 매핑을 복제해
 *     둔다. 왜 필요한가: 장치는 도메인에서 마지막 엔드포인트가 떨어져
 *     나가면 그 도메인의 매핑을 **전부 지운다.** 그러나 드라이버 쪽
 *     도메인은 살아 있고 나중에 다시 붙을 수 있다. 그때
 *     viommu_replay_mappings()가 트리를 훑어 MAP 요청을 다시 보낸다.
 *     iova_to_phys()도 이 트리로 답한다 — 장치에 물어볼 방법이 없다.
 *
 * (4) **두 갈래의 identity 도메인.** 장치가 BYPASS_CONFIG 기능을
 *     알리면 전용 도메인 ID 하나를 예약해 "우회" 플래그로 붙인다.
 *     그 기능이 없으면 일반 페이징 도메인을 만들어 아perture 전체를
 *     1:1로 채운다 — 예약 영역만 건너뛰면서.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [게스트 디바이스 드라이버] dma_map_*()
 *        ↓
 *   [dma-iommu] IOVA 할당
 *        ↓
 *   [이 파일] viommu_map_pages() → 그림자 트리에 기록 + 요청을 큐에 적재
 *        ↓ iotlb_sync
 *   [이 파일] viommu_sync_req() → virtqueue_kick + 완료 대기
 *        ↓ virtqueue
 *   [하이퍼바이저 / VMM] 실제 IOMMU(또는 그 에뮬레이션)를 조작
 *        ↑ event 큐
 *   [이 파일] viommu_event_handler() → viommu_fault_handler()로 로그
 *
 * 실행 컨텍스트: map/unmap은 atomic일 수 있어 요청 버퍼를 GFP_ATOMIC으로
 * 잡는다. event 핸들러는 virtqueue 콜백(인터럽트 문맥)이다.
 * request 큐 접근은 전부 request_lock(irqsave)으로 직렬화된다.
 *
 * === 타 모듈과의 연결 ===
 * - uapi/linux/virtio_iommu.h: 이 파일이 주고받는 모든 요청/응답
 *   구조체와 상수. 게스트와 호스트가 공유하는 유일한 계약이다.
 * - linux/virtio*.h: virtqueue 조작(add_sgs, get_buf, kick)과
 *   기능 협상(virtio_has_feature), 설정 공간 읽기(virtio_cread_le).
 * - linux/interval_tree.h: 매핑 그림자 트리의 자료구조.
 * - dma-iommu.h: MSI 예약 영역 처리(iommu_dma_get_resv_regions).
 * 데이터 흐름: 코어의 map 요청 → 그림자 트리 + 요청 큐 → (sync 시점에)
 * 하이퍼바이저 → 실제 변환. 반대로 폴트는 event 큐로 올라온다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct viommu_dev: 이 virtio 장치 하나. 두 virtqueue, 진행 중인
 *   요청 목록, 그리고 장치가 알려 준 설정(페이지 크기, 도메인 ID 범위).
 * - struct viommu_domain: 도메인 하나. 장치 쪽 도메인 ID와 그림자
 *   매핑 트리, 붙어 있는 엔드포인트 수.
 * - __viommu_add_req() / __viommu_sync_req(): 이 드라이버의 심장.
 *   전자는 적재만, 후자는 kick과 완료 회수를 한다.
 * - viommu_replay_mappings(): 도메인이 되살아날 때 매핑을 다시 보낸다.
 * - viommu_probe_endpoint(): 엔드포인트별 속성(예약 영역, MSI 창)을
 *   장치에 물어본다.
 */

/* [한국어] 이 모듈의 로그에 "virtio_iommu: " 접두사를 붙인다. */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/* [한국어] 지연 관련 헬퍼. 현재 직접 쓰이지는 않는다. */
#include <linux/delay.h>
/* [한국어] DMA 매핑 연산 정의. dma-iommu와의 접점에 필요하다. */
#include <linux/dma-map-ops.h>
/* [한국어] 프리저 지원. 완료 대기 루프의 관례적 포함이다. */
#include <linux/freezer.h>
/* [한국어] 그림자 매핑을 담는 구간 트리 — 이 드라이버의 핵심 자료구조다. */
#include <linux/interval_tree.h>
/* [한국어] IOMMU 코어 계약 — iommu_ops, 도메인, 예약 영역 API. */
#include <linux/iommu.h>
/* [한국어] 모듈로 빌드되므로 MODULE_* 매크로가 필요하다. */
#include <linux/module.h>
/* [한국어] 디바이스 트리 연결(of_xlate)에 필요하다. */
#include <linux/of.h>
/* [한국어] PCI 디바이스 그룹 판별(pci_device_group)에 필요하다. */
#include <linux/pci.h>
/* [한국어] virtqueue 조작과 virtio 장치 모델. */
#include <linux/virtio.h>
/* [한국어] 기능 협상과 설정 공간 읽기 매크로. */
#include <linux/virtio_config.h>
/* [한국어] VIRTIO_ID_IOMMU — 이 드라이버가 붙을 장치 종류. */
#include <linux/virtio_ids.h>
/* [한국어] 대기 큐 관련. */
#include <linux/wait.h>

/* [한국어] 게스트와 호스트가 공유하는 프로토콜 정의.
 * 요청 구조체, 상태 코드, 기능 비트가 모두 여기서 온다 —
 * 이 파일의 하드웨어 지식은 전부 이 헤더가 근거다. */
#include <uapi/linux/virtio_iommu.h>

/* [한국어] iommu_dma_get_resv_regions() 등 DMA 계층의 예약 영역 처리. */
#include "dma-iommu.h"

/* [한국어] 소프트웨어 MSI 창의 시작 IOVA.
 * 장치가 자기 MSI 창을 알려 주지 않았을 때, 이 주소부터의 구간을
 * MSI 도어벨용으로 잡아 둔다. */
#define MSI_IOVA_BASE			0x8000000
/* [한국어] 그 소프트웨어 MSI 창의 길이(1MB).
 * MSI 도어벨을 담기에 넉넉하면서 IOVA 공간을 크게 잠식하지 않는 값이다. */
#define MSI_IOVA_LENGTH			0x100000

/* [한국어] 요청 큐(게스트 → 장치)의 인덱스.
 * MAP/UNMAP/ATTACH/DETACH/PROBE가 모두 이 큐로 나간다. */
#define VIOMMU_REQUEST_VQ		0
/* [한국어] 이벤트 큐(장치 → 게스트)의 인덱스.
 * 폴트 알림이 이 큐로 올라온다. */
#define VIOMMU_EVENT_VQ			1
/* [한국어] 큐의 총 개수. virtio_find_vqs()에 넘긴다. */
#define VIOMMU_NR_VQS			2

/* [한국어] virtio-iommu 장치 하나의 상태.
 * 수명: viommu_probe()에서 devm으로 만들어져 remove까지 산다. */
struct viommu_dev {
	struct iommu_device		iommu;
	/* [한국어] IOMMU 코어에 등록되는 부분(임베드).
	 * 설정자: viommu_probe()의 iommu_device_sysfs_add/register.
	 * 읽는 자: probe_device가 이 주소를 돌려주어 "이 디바이스는
	 *          이 IOMMU가 담당한다"고 코어에 알린다. */

	struct device			*dev;
	/* [한국어] virtio 장치의 struct device. 로그 출력과 devm 할당의
	 * 기준점이다.
	 * 설정자: viommu_probe().
	 * 읽는 자: dev_err/dev_dbg 계열과 devm_kmalloc_array. */

	struct virtio_device		*vdev;
	/* [한국어] virtio 계층의 장치 객체.
	 * 설정자: viommu_probe().
	 * 읽는 자: virtio_has_feature()로 기능 협상 결과를 확인할 때.
	 *          identity 도메인 할당이 이 값으로 우회 지원 여부를 본다. */

	struct ida			domain_ids;
	/* [한국어] 장치 쪽 도메인 ID를 나눠 주는 할당기.
	 * 설정자: probe의 ida_init, 그리고 도메인 생성/해제가 갱신.
	 * 값 범위: [first_domain, last_domain] — 장치가 설정 공간으로
	 *          알려 준 범위 안에서만 준다.
	 * 왜 필요한가: 도메인 ID는 게스트와 호스트가 공유하는 이름이라
	 *              충돌하면 엉뚱한 도메인에 매핑이 들어간다.
	 * 동기화: ida 자체가 내부 락을 갖는다. */

	struct virtqueue		*vqs[VIOMMU_NR_VQS];
	/* [한국어] 요청 큐와 이벤트 큐.
	 * 설정자: viommu_init_vqs()가 virtio 계층에서 받아 채운다.
	 * 읽는 자: 요청 경로는 [0], 이벤트 경로는 [1]을 쓴다.
	 * 동기화: 요청 큐는 request_lock으로, 이벤트 큐는 virtio의
	 *         콜백 직렬화로 보호된다. */

	spinlock_t			request_lock;
	/* [한국어] 요청 큐 조작 전체를 직렬화하는 락.
	 * 설정자: viommu_probe()가 초기화.
	 * 읽는 자: add_req/sync_req 계열이 irqsave로 잡는다.
	 * 왜 irqsave인가: map/unmap이 인터럽트가 꺼진 문맥에서 불릴 수
	 *                 있기 때문이다.
	 * 이름 없는 규칙: __ 접두사가 붙은 함수들은 이 락을 이미 잡았다고
	 *                 가정한다(assert_spin_locked로 확인한다). */

	struct list_head		requests;
	/* [한국어] 아직 완료되지 않은 요청들의 목록.
	 * 설정자: __viommu_add_req()가 꼬리에 붙이고,
	 *          __viommu_sync_req()가 완료된 것을 떼어 낸다.
	 * 왜 필요한가: 완료를 기다릴 때 "언제까지 기다려야 하는가"의
	 *              기준이 된다 — 이 목록이 빌 때까지다.
	 * 동기화: request_lock. */

	void				*evts;
	/* [한국어] 이벤트 큐에 미리 넣어 둔 버퍼 배열.
	 * 설정자: viommu_fill_evtq()가 devm으로 한 번에 할당한다.
	 * 읽는 자: 소유권 보관용이다 — 실제 접근은 virtqueue가 돌려주는
	 *          포인터로 이뤄진다.
	 * 왜 미리 채우는가: 장치가 폴트를 보낼 때 쓸 버퍼를 게스트가
	 *                   먼저 제공해야 하는 것이 virtio의 방식이다. */

	/* Device configuration */
	struct iommu_domain_geometry	geometry;
	/* [한국어] 장치가 알려 준 입력 주소 범위. 모든 도메인이 이 값을
	 * 그대로 물려받는다.
	 * 설정자: viommu_probe()가 설정 공간의 input_range로 채운다.
	 * 값 범위: INPUT_RANGE 기능이 없으면 [0, -1] 즉 무제한. */

	u64				pgsize_bitmap;
	/* [한국어] 장치가 지원하는 페이지 크기들의 비트맵.
	 * 설정자: viommu_probe()가 설정 공간에서 읽는다(필수 항목).
	 * 읽는 자: 도메인 생성이 이 값을 그대로 쓰고, 가장 작은 크기를
	 *          granule로 삼는다.
	 * 값 범위: 0이면 장치가 잘못된 것이라 probe가 실패한다. */

	u32				first_domain;
	/* [한국어] 쓸 수 있는 도메인 ID의 하한.
	 * 설정자: 설정 공간의 domain_range.start. 우회 도메인을 예약하면
	 *          그만큼 하나 올라간다.
	 * 읽는 자: ida_alloc_range의 하한. */

	u32				last_domain;
	/* [한국어] 쓸 수 있는 도메인 ID의 상한.
	 * 설정자: DOMAIN_RANGE 기능이 있으면 설정 공간에서, 없으면
	 *          ~0U(사실상 무제한).
	 * 읽는 자: ida_alloc_range의 상한. */

	u32				identity_domain_id;
	/* [한국어] 우회(bypass) 전용으로 떼어 둔 도메인 ID.
	 * 설정자: BYPASS_CONFIG 기능이 있을 때만 probe가 first_domain에서
	 *          하나를 떼어 여기에 넣는다.
	 * 읽는 자: viommu_attach_identity_domain()이 ATTACH 요청에 싣는다.
	 * 왜 별도 ID인가: 우회는 매핑이 없는 특별한 도메인이라
	 *                 일반 도메인과 섞이면 안 되기 때문이다. */

	/* Supported MAP flags */
	u32				map_flags;
	/* [한국어] MAP 요청에 실을 수 있는 플래그의 집합.
	 * 설정자: probe가 READ|WRITE로 시작해, MMIO 기능이 있으면 더한다.
	 * 읽는 자: viommu_map_pages()가 요청 권한이 이 집합을 벗어나면
	 *          거부한다 — 장치가 모르는 플래그를 보내지 않기 위함이다. */

	u32				probe_size;
	/* [한국어] PROBE 응답에 담길 속성 영역의 크기.
	 * 설정자: PROBE 기능이 있을 때만 설정 공간에서 읽는다(없으면 0).
	 * 읽는 자: probe_device가 0이 아닐 때만 엔드포인트 속성을
	 *          물어보고, 요청 버퍼 크기 계산에도 쓰인다. */
};

/* [한국어] 그림자 트리에 담기는 매핑 하나.
 * 장치에 보낸 MAP 요청 하나에 대응한다. */
struct viommu_mapping {
	phys_addr_t			paddr;
	/* [한국어] 이 구간이 가리키는 물리 주소의 시작.
	 * 설정자: viommu_add_mapping().
	 * 읽는 자: iova_to_phys()가 오프셋을 더해 답을 만들고,
	 *          replay가 MAP 요청에 다시 싣는다. */

	struct interval_tree_node	iova;
	/* [한국어] 이 매핑이 덮는 IOVA 구간([start, last])이자 트리 노드.
	 * 설정자: viommu_add_mapping()이 start/last를 채워 트리에 넣는다.
	 * 읽는 자: 구간 검색 전부. container_of의 기준점이기도 하다.
	 * 왜 구간 트리인가: 임의의 IOVA가 어느 매핑에 속하는지 빠르게
	 *                   찾아야 하기 때문이다. */

	u32				flags;
	/* [한국어] 이 매핑의 권한(READ/WRITE/MMIO).
	 * 설정자: map_pages가 prot을 프로토콜 플래그로 바꿔 넣는다.
	 * 읽는 자: replay가 MAP 요청을 재구성할 때 그대로 쓴다. */
};

/* [한국어] IOMMU 도메인 하나.
 * 수명: domain_alloc_paging에서 만들어져 domain_free에서 해제된다. */
struct viommu_domain {
	struct iommu_domain		domain;
	/* [한국어] 코어가 보는 도메인 부분(임베드).
	 * 설정자: 생성 시 장치의 pgsize_bitmap과 geometry를 물려받는다.
	 * 읽는 자: to_viommu_domain 매크로가 이 주소로 바깥을 복원한다. */

	struct viommu_dev		*viommu;
	/* [한국어] 이 도메인이 속한 virtio-iommu 장치.
	 * 설정자: domain_alloc_paging.
	 * 값 범위: 정적 identity 도메인에서는 NULL이다 — domain_free가
	 *          그것을 보고 ID 반납을 건너뛴다.
	 * 읽는 자: 요청을 보낼 때마다 이 값으로 큐를 찾는다. */

	unsigned int			id;
	/* [한국어] 장치가 아는 이 도메인의 ID.
	 * 설정자: domain_alloc_paging의 ida_alloc_range.
	 * 읽는 자: 모든 MAP/UNMAP/ATTACH 요청에 실린다 — 게스트와 호스트가
	 *          같은 도메인을 가리키게 하는 유일한 이름이다. */

	u32				map_flags;
	/* [한국어] 이 도메인에서 허용되는 MAP 플래그. 장치의 것을 그대로
	 * 물려받는다.
	 * 설정자: domain_alloc_paging.
	 * 읽는 자: map_pages의 권한 검사. */

	spinlock_t			mappings_lock;
	/* [한국어] 그림자 트리를 보호하는 락.
	 * 설정자: domain_alloc_paging의 spin_lock_init.
	 * 읽는 자: add/del/replay/iova_to_phys 모두 irqsave로 잡는다.
	 * 왜 irqsave인가: 매핑 경로가 인터럽트가 꺼진 문맥일 수 있다. */

	struct rb_root_cached		mappings;
	/* [한국어] 이 도메인의 매핑 그림자 트리.
	 * 설정자: 생성 시 비어 있고, add_mapping이 채운다.
	 * 왜 있어야 하는가: 장치는 마지막 엔드포인트가 떨어지면 이
	 *                   도메인의 매핑을 전부 지운다. 다시 붙을 때
	 *                   되살릴 근거가 이 트리뿐이다. iova_to_phys의
	 *                   답도 여기서 나온다 — 장치에 물어볼 수단이 없다.
	 * 동기화: mappings_lock. */

	unsigned long			nr_endpoints;
	/* [한국어] 이 도메인에 붙어 있는 엔드포인트의 수.
	 * 설정자: attach가 증가, detach가 감소.
	 * 읽는 자: 0이면 장치 쪽에 이 도메인이 존재하지 않는다는 뜻이라,
	 *          MAP 요청을 보내지 않고 트리에만 기록한다. 첫 엔드포인트가
	 *          붙는 순간(0 → 1) replay가 필요한지도 이 값으로 안다.
	 * 동기화: group->mutex가 attach/detach를 직렬화한다. */
};

/* [한국어] 엔드포인트(= IOMMU를 쓰는 디바이스) 하나의 드라이버 쪽 상태.
 * dev_iommu_priv에 매달린다. */
struct viommu_endpoint {
	struct device			*dev;
	/* [한국어] 이 엔드포인트에 해당하는 디바이스.
	 * 설정자: viommu_probe_device().
	 * 읽는 자: 로그 출력과 fwspec 조회. */

	struct viommu_dev		*viommu;
	/* [한국어] 이 엔드포인트를 담당하는 virtio-iommu 장치.
	 * 설정자: viommu_probe_device()가 fwnode로 찾아 채운다.
	 * 읽는 자: attach가 도메인과 같은 장치인지 확인하는 근거. */

	struct viommu_domain		*vdomain;
	/* [한국어] 현재 붙어 있는 도메인.
	 * 설정자: attach 계열이 갱신, detach가 NULL로 만든다.
	 * 왜 필요한가: 새 도메인에 붙을 때 이전 도메인의 엔드포인트 수를
	 *              줄여야 하는데, 코어가 항상 old를 주지는 않는다.
	 * 동기화: group->mutex — 원본 주석이 명시하는 바다. */

	struct list_head		resv_regions;
	/* [한국어] PROBE로 받아 온 이 엔드포인트의 예약 영역들.
	 * 설정자: viommu_add_resv_mem()이 시작 주소 순으로 정렬해 넣는다.
	 * 읽는 자: get_resv_regions가 복사해 코어에 주고,
	 *          identity 매핑이 이 구간들을 건너뛴다. */
};

/* [한국어] 큐에 올라간 요청 하나의 추적 정보.
 * 요청 버퍼를 통째로 품고 있어, 호출자의 스택 버퍼가 사라져도 안전하다. */
struct viommu_request {
	struct list_head		list;
	/* [한국어] viommu_dev.requests 목록에 매다는 고리.
	 * 설정자/읽는 자: add_req가 붙이고 sync_req가 뗀다. */

	void				*writeback;
	/* [한국어] 완료 후 결과를 되돌려 쓸 호출자의 버퍼 주소.
	 * 설정자: writeback이 요청된 경우에만 add_req가 채운다.
	 * 값 범위: NULL이면 결과를 돌려주지 않는 비동기 요청이다.
	 * 주의: 호출자가 락을 놓았다 잡으면 이 주소가 무효해질 수 있어,
	 *       writeback 요청은 반드시 같은 락 구간에서 동기화해야 한다. */

	unsigned int			write_offset;
	/* [한국어] 요청 버퍼에서 "장치가 쓰는 부분"이 시작하는 오프셋.
	 * 설정자: add_req가 viommu_get_write_desc_offset()의 값을 넣는다.
	 * 읽는 자: 완료 시 이 지점부터를 호출자 버퍼로 복사한다.
	 * 왜 필요한가: virtqueue 서술자는 읽기 전용 부분과 쓰기 전용
	 *              부분으로 나뉘어야 하는데, 그 경계가 이 값이다. */

	unsigned int			len;
	/* [한국어] 요청 버퍼 전체의 길이.
	 * 설정자: add_req.
	 * 읽는 자: 완료 길이 검증과 상태 코드 위치 계산. */

	char				buf[] __counted_by(len);
	/* [한국어] 요청 본문. 구조체 뒤에 이어 붙는 가변 길이 배열이다.
	 * 설정자: add_req가 호출자의 버퍼에서 읽기 부분만 복사해 넣는다.
	 * 왜 복사하는가: 호출자가 스택에 만든 요청이 함수를 빠져나간 뒤에도
	 *                장치가 이 메모리를 읽기 때문이다.
	 * __counted_by: len 필드가 이 배열의 길이임을 컴파일러에 알려
	 *               경계 검사를 돕는다. */
};

/* [한국어] 폴트 이벤트에서 예약된(=0이어야 하는) 비트들.
 * 이벤트 헤더의 이 비트가 켜져 있으면 이 드라이버가 모르는 종류의
 * 이벤트라는 뜻이므로 조용히 무시한다 — 앞으로 추가될 이벤트 종류와의
 * 호환을 위한 장치다. */
#define VIOMMU_FAULT_RESV_MASK		0xffffff00

/* [한국어] 이벤트 큐로 올라오는 메시지 하나.
 * 공용체로 둔 이유는 아래 필드 주석 참조. */
struct viommu_event {
	union {	/* [한국어] 헤더로 볼지 폴트로 볼지를 같은 메모리에 겹쳐 둔다. */
		u32			head;
		/* [한국어] 이벤트의 첫 워드를 종류 판별용으로 보는 창.
		 * 읽는 자: event_handler가 예약 비트를 검사할 때.
		 * 왜 공용체인가: 아직 모르는 이벤트 종류가 와도 헤더만 보고
		 *                걸러 내야 하기 때문이다. */

		struct virtio_iommu_fault fault;
		/* [한국어] 폴트 이벤트로 해석했을 때의 내용.
		 * 읽는 자: 예약 비트 검사를 통과한 뒤 fault_handler에 넘긴다. */
	};
};

/* [한국어] 우회용 정적 도메인의 전방 선언.
 * 정의는 attach 콜백 아래에 있고, domain_alloc_identity가 그보다
 * 앞에서 이 주소를 돌려주므로 선언이 필요하다. */
static struct viommu_domain viommu_identity_domain;

/* [한국어] 코어의 iommu_domain 포인터에서 드라이버 쪽 도메인을 복원한다.
 * 함수가 아니라 매크로인 이유는 여러 곳에서 초기화 식으로도 쓰이기
 * 때문이다. */
#define to_viommu_domain(domain)	\
	container_of(domain, struct viommu_domain, domain)	/* [한국어] 임베드 멤버의 주소에서 바깥 구조체를 역산한다. */

/*
 * [한국어]
 * viommu_get_req_errno - 요청 응답의 상태 코드를 errno로 바꾼다
 *
 * @buf: 요청 버퍼(응답이 덮어써진 상태).
 * @len: 버퍼 전체 길이.
 * @return: 0 또는 음수 errno.
 *
 * 응답의 상태 바이트는 버퍼의 **맨 끝**에 있다. 요청과 응답이 같은
 * 버퍼를 쓰고, 앞쪽은 게스트가 쓰고 뒤쪽은 장치가 쓰는 구조라 그렇다.
 * 그래서 len에서 tail 크기를 빼 위치를 계산한다.
 *
 * 실행 컨텍스트: 동기 요청의 완료 처리.
 *
 * 호출 체인:
 *   viommu_send_req_sync() → [viommu_get_req_errno]
 */
static int viommu_get_req_errno(void *buf, size_t len)
{
	/* [한국어] 버퍼 끝에서 tail 구조체 크기만큼 앞이 상태 필드다. */
	struct virtio_iommu_req_tail *tail = buf + len - sizeof(*tail);

	/* [한국어] 프로토콜 상태 코드를 커널 errno로 옮긴다. */
	switch (tail->status) {
	/* [한국어] 정상 완료. */
	case VIRTIO_IOMMU_S_OK:
		return 0;
	/* [한국어] 장치가 모르는 요청 종류였다. */
	case VIRTIO_IOMMU_S_UNSUPP:
		return -ENOSYS;
	/* [한국어] 인자가 잘못됐다. */
	case VIRTIO_IOMMU_S_INVAL:
		return -EINVAL;
	/* [한국어] 주소나 크기가 허용 범위를 벗어났다. */
	case VIRTIO_IOMMU_S_RANGE:
		return -ERANGE;
	/* [한국어] 가리킨 도메인이나 엔드포인트가 없다. */
	case VIRTIO_IOMMU_S_NOENT:
		return -ENOENT;
	/* [한국어] 장치가 요청 버퍼에 접근하다 폴트를 만났다. */
	case VIRTIO_IOMMU_S_FAULT:
		return -EFAULT;
	/* [한국어] 장치 쪽 메모리가 부족하다. */
	case VIRTIO_IOMMU_S_NOMEM:
		return -ENOMEM;
	/* [한국어] I/O 오류와 장치 내부 오류, 그리고 알 수 없는 코드는
	 * 모두 하나로 묶는다 — 게스트가 달리 대응할 방법이 없다. */
	case VIRTIO_IOMMU_S_IOERR:
	case VIRTIO_IOMMU_S_DEVERR:	/* [한국어] 장치 내부 오류도 같은 갈래로 묶는다. */
	default:	/* [한국어] 이 드라이버가 모르는 새 상태 코드도 여기로 온다. */
		return -EIO;	/* [한국어] 게스트가 달리 대응할 수 없는 오류들을 하나로 묶는다. */
	}
}

/*
 * [한국어]
 * viommu_set_req_status - 요청 버퍼의 상태 필드를 직접 채운다
 *
 * @buf: 요청 버퍼.
 * @len: 버퍼 전체 길이.
 * @status: 써 넣을 프로토콜 상태 코드.
 * @return: 없음.
 *
 * 장치가 응답을 쓰지 못한 경우(완료 길이가 0)에 드라이버가 대신
 * 오류 상태를 만들어 넣는 데 쓴다. 그래야 위의 get_req_errno가
 * 쓰레기 값을 읽지 않는다.
 *
 * 실행 컨텍스트: 완료 회수 루프.
 *
 * 호출 체인:
 *   __viommu_sync_req() → [viommu_set_req_status]
 */
static void viommu_set_req_status(void *buf, size_t len, int status)
{
	/* [한국어] 상태 필드의 위치는 읽을 때와 같은 방식으로 구한다. */
	struct virtio_iommu_req_tail *tail = buf + len - sizeof(*tail);

	/* [한국어] 장치가 쓰지 못한 자리에 드라이버가 오류를 채워 넣는다. */
	tail->status = status;
}

/*
 * [한국어]
 * viommu_get_write_desc_offset - 요청 버퍼의 읽기/쓰기 경계를 구한다
 *
 * @viommu: 대상 장치(PROBE 응답 크기를 안다).
 * @req: 요청 헤더.
 * @len: 버퍼 전체 길이.
 * @return: 장치가 쓰기 시작할 오프셋.
 *
 * virtqueue의 서술자는 "게스트가 쓰고 장치가 읽는 부분"과 "장치가
 * 쓰고 게스트가 읽는 부분"으로 나뉘어야 한다. 그 경계가 이 값이다.
 *
 * 보통은 맨 끝의 상태 tail만 장치가 쓴다. 예외가 PROBE인데,
 * 응답 속성이 담길 probe_size만큼도 장치의 몫이라 경계가 그만큼
 * 앞으로 당겨진다.
 *
 * 실행 컨텍스트: 요청 적재 경로.
 *
 * 호출 체인:
 *   __viommu_add_req() → [viommu_get_write_desc_offset]
 */
static off_t viommu_get_write_desc_offset(struct viommu_dev *viommu,
					  struct virtio_iommu_req_head *req,
					  size_t len)
{
	/* [한국어] 모든 요청의 끝에 붙는 상태 tail의 크기. */
	size_t tail_size = sizeof(struct virtio_iommu_req_tail);

	/* [한국어] PROBE만 응답 본문이 있다 — 속성 영역과 tail 둘 다
	 * 장치가 쓰므로 경계를 그만큼 더 앞으로 당긴다. */
	if (req->type == VIRTIO_IOMMU_T_PROBE)
		return len - viommu->probe_size - tail_size;

	/* [한국어] 나머지 요청은 상태 tail만 장치의 몫이다. */
	return len - tail_size;
}

/*
 * __viommu_sync_req - Complete all in-flight requests
 *
 * Wait for all added requests to complete. When this function returns, all
 * requests that were in-flight at the time of the call have completed.
 */
/*
 * [한국어]
 * __viommu_sync_req - 큐에 쌓인 요청을 보내고 전부 완료될 때까지 기다린다
 *
 * @viommu: 대상 장치.
 * @return: 항상 0(현재 구현에서 실패 경로가 없다).
 *
 * 이 드라이버의 지연 전송 모델이 완성되는 지점이다. add_req는 큐에
 * 넣기만 했고, 여기서 비로소 kick 하고 완료를 회수한다.
 *
 * 대기 방식이 특이하다: 잠들지 않고 **바쁜 대기**를 한다.
 * virtqueue_get_buf()가 NULL이면 continue로 다시 시도할 뿐이다.
 * 매핑 경로가 인터럽트가 꺼진 문맥일 수 있어 잠들 수 없기 때문이다.
 *
 * 완료 처리에서 두 가지를 한다.
 *  - 장치가 아무것도 쓰지 않았으면(len == 0) 드라이버가 I/O 오류
 *    상태를 대신 채워 넣는다. 그래야 상태 읽기가 쓰레기를 보지 않는다.
 *  - writeback이 요청된 요청이면, 장치가 쓴 부분을 호출자의 버퍼로
 *    복사한다. 길이가 정확히 일치할 때만 복사하는 것이 안전 검사다.
 *
 * 실행 컨텍스트: request_lock을 **이미 잡은 상태**여야 한다
 * (assert_spin_locked이 확인한다). 잠들 수 없다.
 *
 * 호출 체인:
 *   viommu_sync_req() / viommu_send_req_sync() / __viommu_add_req()
 *   → [__viommu_sync_req] → virtqueue_kick() → virtqueue_get_buf()
 */
static int __viommu_sync_req(struct viommu_dev *viommu)
{
	/* [한국어] 장치가 실제로 쓴 바이트 수를 받을 곳. */
	unsigned int len;
	/* [한국어] 되돌려 복사할 길이(요청 길이에서 쓰기 시작점을 뺀 값). */
	size_t write_len;
	/* [한국어] 완료된 요청의 추적 구조체. */
	struct viommu_request *req;
	/* [한국어] 요청 큐. */
	struct virtqueue *vq = viommu->vqs[VIOMMU_REQUEST_VQ];

	/* [한국어] 호출자가 락을 잡았는지 확인한다. __ 접두사 함수의 규약이다. */
	assert_spin_locked(&viommu->request_lock);

	/* [한국어] 쌓아 둔 요청을 장치에 알린다 — 여기서 비로소 전송이 시작된다. */
	virtqueue_kick(vq);

	/* [한국어] 진행 중 목록이 빌 때까지 완료를 회수한다. */
	while (!list_empty(&viommu->requests)) {
		/* [한국어] 장치가 쓴 길이를 받을 변수를 매번 초기화한다 —
		 * 장치가 손대지 않았음을 0으로 구별하기 위함이다. */
		len = 0;
		/* [한국어] 완료된 요청 하나를 꺼낸다. */
		req = virtqueue_get_buf(vq, &len);
		/* [한국어] 아직 완료된 것이 없다. 잠들 수 없는 문맥이라
		 * 바쁜 대기로 다시 시도한다. */
		if (!req)
			continue;

		/* [한국어] 장치가 응답을 전혀 쓰지 않았다. 상태 필드가
		 * 초기화되지 않은 채로 읽히지 않도록 오류를 채워 넣는다. */
		if (!len)
			viommu_set_req_status(req->buf, req->len,
					      VIRTIO_IOMMU_S_IOERR);

		/* [한국어] 장치가 썼어야 할 길이를 계산한다. */
		write_len = req->len - req->write_offset;
		/* [한국어] 되돌려 줄 요청이고 길이도 정확히 맞을 때만 복사한다 —
		 * 길이가 다르면 응답을 신뢰할 수 없다. */
		if (req->writeback && len == write_len)
			memcpy(req->writeback, req->buf + req->write_offset,
			       write_len);

		/* [한국어] 진행 중 목록에서 뺀다. */
		list_del(&req->list);
		/* [한국어] 요청 버퍼를 품고 있던 구조체를 해제한다. */
		kfree(req);
	}

	return 0;	/* [한국어] 이 구현에는 실패 경로가 없어 항상 성공이다. */
}

/*
 * [한국어]
 * viommu_sync_req - 락을 잡고 요청들을 동기화한다
 *
 * @viommu: 대상 장치.
 * @return: __viommu_sync_req의 결과.
 *
 * 락 획득만 감싼 얇은 껍데기다. 코어의 iotlb_sync 계열 콜백이
 * 이 함수를 부르며, 그것이 이 드라이버에서 매핑 요청이 실제로
 * 하이퍼바이저에 전달되는 순간이다.
 *
 * 실행 컨텍스트: iotlb_sync 경로. irqsave로 락을 잡는다.
 *
 * 호출 체인:
 *   viommu_iotlb_sync() / sync_map() / flush_iotlb_all()
 *   → [viommu_sync_req] → __viommu_sync_req()
 */
static int viommu_sync_req(struct viommu_dev *viommu)
{
	/* [한국어] 동기화 결과. */
	int ret;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* [한국어] 요청 큐 조작 구간을 잠근다. */
	spin_lock_irqsave(&viommu->request_lock, flags);
	ret = __viommu_sync_req(viommu);
	/* [한국어] 실패를 로그로만 남긴다 — 호출자에게도 전하지만
	 * 대부분의 콜백은 반환값을 쓰지 않는다. */
	if (ret)
		dev_dbg(viommu->dev, "could not sync requests (%d)\n", ret);
	spin_unlock_irqrestore(&viommu->request_lock, flags);	/* [한국어] 큐 조작이 끝났으니 락과 인터럽트 상태를 되돌린다. */

	return ret;	/* [한국어] 동기화 결과를 호출자에게 전한다. */
}

/*
 * __viommu_add_request - Add one request to the queue
 * @buf: pointer to the request buffer
 * @len: length of the request buffer
 * @writeback: copy data back to the buffer when the request completes.
 *
 * Add a request to the queue. Only synchronize the queue if it's already full.
 * Otherwise don't kick the queue nor wait for requests to complete.
 *
 * When @writeback is true, data written by the device, including the request
 * status, is copied into @buf after the request completes. This is unsafe if
 * the caller allocates @buf on stack and drops the lock between add_req() and
 * sync_req().
 *
 * Return 0 if the request was successfully added to the queue.
 */
/*
 * [한국어]
 * __viommu_add_req - 요청 하나를 큐에 적재한다(보내지는 않는다)
 *
 * @viommu: 대상 장치.
 * @buf: 요청 버퍼(보통 호출자의 스택).
 * @len: 버퍼 길이.
 * @writeback: 완료 시 응답을 호출자 버퍼로 되돌려 쓸지 여부.
 * @return: 0 성공, -EINVAL(경계 계산 실패), -ENOMEM, virtqueue 오류.
 *
 * 이 드라이버의 성능 모델이 여기 담겨 있다. **kick 하지 않는다.**
 * 큐에 넣고 진행 중 목록에 매달 뿐이며, 전송은 나중에 sync가 한다.
 * 예외는 큐가 꽉 찼을 때뿐인데, 그때는 어쩔 수 없이 동기화해 자리를
 * 비우고 다시 시도한다.
 *
 * 호출자의 버퍼를 그대로 쓰지 않고 **복사본을 만든다.** 호출자가
 * 스택에 요청을 만들었을 수 있고, 그 스택 프레임은 sync 전에 사라질
 * 수 있기 때문이다. 다만 writeback 대상 주소만은 호출자 버퍼를
 * 가리키므로, 원본 주석이 경고하듯 그 사이 락을 놓아서는 안 된다.
 *
 * 산란 목록(scatterlist)이 둘인 이유: 앞부분은 장치가 읽고 뒷부분은
 * 장치가 쓴다. virtqueue_add_sgs(vq, sg, 1, 1, ...)의 두 1이
 * "읽기 서술자 1개, 쓰기 서술자 1개"를 뜻한다.
 *
 * 실행 컨텍스트: request_lock을 이미 잡은 상태. atomic일 수 있어
 * 할당이 모두 GFP_ATOMIC이다.
 *
 * 호출 체인:
 *   viommu_add_req() / viommu_send_req_sync() → [__viommu_add_req]
 *   → virtqueue_add_sgs()
 */
static int __viommu_add_req(struct viommu_dev *viommu, void *buf, size_t len,
			    bool writeback)
{
	/* [한국어] 각 단계의 결과. */
	int ret;
	/* [한국어] 읽기/쓰기 서술자의 경계 오프셋. */
	off_t write_offset;
	/* [한국어] 요청 버퍼를 품을 추적 구조체. */
	struct viommu_request *req;
	/* [한국어] 읽기 부분과 쓰기 부분을 각각 가리킬 산란 목록 항목. */
	struct scatterlist top_sg, bottom_sg;
	/* [한국어] virtqueue에 넘길 두 항목의 배열. 순서가 곧 읽기 → 쓰기다. */
	struct scatterlist *sg[2] = { &top_sg, &bottom_sg };
	/* [한국어] 요청 큐. */
	struct virtqueue *vq = viommu->vqs[VIOMMU_REQUEST_VQ];

	/* [한국어] 호출자가 락을 잡았는지 확인한다. */
	assert_spin_locked(&viommu->request_lock);

	/* [한국어] 이 요청에서 장치가 쓰기 시작할 지점을 구한다. */
	write_offset = viommu_get_write_desc_offset(viommu, buf, len);
	/* [한국어] 0 이하라면 버퍼가 tail조차 담지 못할 만큼 짧다는 뜻이다. */
	if (write_offset <= 0)
		return -EINVAL;

	/* [한국어] 요청 본문을 뒤에 붙인 추적 구조체를 잡는다.
	 * ATOMIC인 이유: 매핑 경로가 잠들 수 없는 문맥일 수 있다. */
	req = kzalloc_flex(*req, buf, len, GFP_ATOMIC);
	if (!req)	/* [한국어] 요청 추적 구조체를 잡지 못했다. */
		return -ENOMEM;

	/* [한국어] 전체 길이를 기록한다(상태 필드 위치 계산에 쓰인다). */
	req->len = len;
	/* [한국어] 응답을 되돌려 줘야 하는 요청이면 목적지를 기억해 둔다. */
	if (writeback) {
		/* [한국어] 호출자 버퍼에서 장치가 쓸 부분의 주소. */
		req->writeback = buf + write_offset;
		req->write_offset = write_offset;	/* [한국어] 되돌려 쓸 시작 위치도 함께 기억한다. */
	}
	/* [한국어] 장치가 읽을 부분만 복사한다. 호출자의 스택이 사라져도
	 * 장치는 이 복사본을 읽으므로 안전하다. */
	memcpy(&req->buf, buf, write_offset);

	/* [한국어] 앞부분 = 장치가 읽을 영역. */
	sg_init_one(&top_sg, req->buf, write_offset);
	/* [한국어] 뒷부분 = 장치가 쓸 영역(응답과 상태). */
	sg_init_one(&bottom_sg, req->buf + write_offset, len - write_offset);

	/* [한국어] 읽기 1개, 쓰기 1개의 서술자로 큐에 넣는다. 마지막 인자
	 * req는 완료 시 돌려받을 표식이다. */
	ret = virtqueue_add_sgs(vq, sg, 1, 1, req, GFP_ATOMIC);
	if (ret == -ENOSPC) {	/* [한국어] 큐에 자리가 없다 — 지연 전송 원칙의 유일한 예외 구간. */
		/* If the queue is full, sync and retry */
		/* [한국어] 큐가 꽉 찼다. 지연 전송 원칙을 깨고 여기서
		 * 동기화해 자리를 비운 뒤 한 번 더 시도한다. */
		if (!__viommu_sync_req(viommu))
			ret = virtqueue_add_sgs(vq, sg, 1, 1, req, GFP_ATOMIC);
	}
	if (ret)	/* [한국어] 재시도까지 실패했다면 적재를 포기한다. */
		goto err_free;

	/* [한국어] 진행 중 목록에 매단다. sync가 이 목록이 빌 때까지
	 * 기다리므로, 이 등록이 곧 "완료를 추적하겠다"는 선언이다. */
	list_add_tail(&req->list, &viommu->requests);
	return 0;

/* [한국어] 큐 적재에 실패했다 — 복사본을 되돌린다. */
err_free:
	kfree(req);	/* [한국어] 큐에 넣지 못한 복사본을 해제한다. */
	return ret;	/* [한국어] 적재 실패 이유를 그대로 전한다. */
}

/*
 * [한국어]
 * viommu_add_req - 락을 잡고 요청을 적재한다(응답을 기다리지 않음)
 *
 * @viommu: 대상 장치.
 * @buf: 요청 버퍼.
 * @len: 버퍼 길이.
 * @return: 0 성공, 음수 오류.
 *
 * MAP/UNMAP처럼 결과를 즉시 알 필요가 없는 요청에 쓴다. writeback을
 * false로 넘기므로 호출자의 스택 버퍼가 사라져도 문제가 없다.
 * 실제 전송은 나중에 iotlb_sync에서 일어난다.
 *
 * 실행 컨텍스트: 매핑/해제 경로. atomic일 수 있다.
 *
 * 호출 체인:
 *   viommu_map_pages() / viommu_unmap_pages() → [viommu_add_req]
 */
static int viommu_add_req(struct viommu_dev *viommu, void *buf, size_t len)
{
	/* [한국어] 적재 결과. */
	int ret;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* [한국어] 큐 조작 구간을 잠근다. */
	spin_lock_irqsave(&viommu->request_lock, flags);
	/* [한국어] writeback=false — 응답을 되돌려 받지 않으므로
	 * 호출자의 버퍼가 곧 사라져도 된다. */
	ret = __viommu_add_req(viommu, buf, len, false);
	if (ret)	/* [한국어] 적재에 실패했다. */
		dev_dbg(viommu->dev, "could not add request: %d\n", ret);
	spin_unlock_irqrestore(&viommu->request_lock, flags);	/* [한국어] 큐 조작이 끝났으니 락을 놓는다. */

	return ret;	/* [한국어] 적재 결과를 전한다. */
}

/*
 * Send a request and wait for it to complete. Return the request status (as an
 * errno)
 */
/*
 * [한국어]
 * viommu_send_req_sync - 요청을 보내고 완료까지 기다린 뒤 결과를 돌려준다
 *
 * @viommu: 대상 장치.
 * @buf: 요청 버퍼(응답이 이 자리에 덮어써진다).
 * @len: 버퍼 길이.
 * @return: 장치가 준 상태를 errno로 바꾼 값.
 *
 * ATTACH/DETACH/PROBE처럼 결과를 알아야 진행할 수 있는 요청에 쓴다.
 * 적재와 동기화를 **같은 락 구간에서** 한다는 점이 핵심이다.
 * writeback 대상이 호출자의 스택이라, 중간에 락을 놓으면 그 주소가
 * 무효해질 수 있기 때문이다.
 *
 * 동기화가 실패해도 그대로 진행해 상태를 읽는 점에 주의 —
 * 큐 수준의 실패보다 장치가 준 실제 상태가 더 유용한 정보다.
 * 원본 주석의 "Fall-through"가 그 뜻이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트가 대부분이나 락 안에서 바쁜
 * 대기를 하므로 오래 잡지 않도록 주의해야 한다.
 *
 * 호출 체인:
 *   attach/detach/probe 경로 → [viommu_send_req_sync]
 *   → __viommu_add_req() → __viommu_sync_req() → viommu_get_req_errno()
 */
static int viommu_send_req_sync(struct viommu_dev *viommu, void *buf,
				size_t len)
{
	/* [한국어] 각 단계의 결과. */
	int ret;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* [한국어] 적재부터 완료 회수까지를 한 락 구간으로 묶는다 —
	 * writeback 주소가 유효한 채로 유지되어야 한다. */
	spin_lock_irqsave(&viommu->request_lock, flags);

	/* [한국어] writeback=true — 완료 시 응답이 호출자 버퍼로 복사된다. */
	ret = __viommu_add_req(viommu, buf, len, true);
	if (ret) {	/* [한국어] 적재부터 실패했다면 보낼 것이 없다. */
		dev_dbg(viommu->dev, "could not add request (%d)\n", ret);	/* [한국어] 실패 이유를 디버그 로그로 남긴다. */
		goto out_unlock;	/* [한국어] 락을 풀고 나가는 공통 자리로 간다. */
	}

	/* [한국어] 이 요청을 포함해 쌓인 것들을 모두 완료시킨다. */
	ret = __viommu_sync_req(viommu);
	if (ret) {	/* [한국어] 큐 수준의 동기화가 실패한 경우. */
		dev_dbg(viommu->dev, "could not sync requests (%d)\n", ret);	/* [한국어] 그래도 아래에서 실제 상태를 읽으므로 로그만 남긴다. */
		/* Fall-through (get the actual request status) */
		/* [한국어] 여기서 반환하지 않는다 — 큐 오류보다 장치가 준
		 * 실제 상태가 더 정확한 정보이기 때문이다. */
	}

	/* [한국어] 버퍼에 덮어써진 상태 코드를 errno로 바꿔 돌려준다. */
	ret = viommu_get_req_errno(buf, len);
/* [한국어] 락을 풀고 나가는 공통 자리. */
out_unlock:
	spin_unlock_irqrestore(&viommu->request_lock, flags);	/* [한국어] 적재와 동기화가 모두 끝났으니 락을 놓는다. */
	return ret;	/* [한국어] 장치가 준 상태를 errno로 바꾼 값을 돌려준다. */
}

/*
 * [한국어]
 * viommu_send_attach_req - 디바이스의 모든 스트림 ID에 ATTACH를 보낸다
 *
 * @viommu: 대상 장치.
 * @dev: 붙일 디바이스.
 * @req: 미리 채워 둔 ATTACH 요청(endpoint 필드만 비어 있다).
 * @return: 0 성공, 첫 실패의 errno.
 *
 * 한 디바이스가 여러 스트림 ID를 낼 수 있다(예: PCI 함수 여럿을
 * 가진 장치). 장치 쪽 도메인은 엔드포인트 단위로 붙이므로,
 * fwspec에 등록된 ID마다 요청을 하나씩 보내야 한다.
 *
 * 중간에 실패하면 앞서 성공한 것들을 되돌리지 않는다 — 호출자가
 * 곧이어 차단 도메인으로 옮기거나 실패를 위로 전하기 때문이다.
 *
 * 실행 컨텍스트: attach 경로.
 *
 * 호출 체인:
 *   viommu_attach_dev() / viommu_attach_identity_domain()
 *   → [viommu_send_attach_req] → viommu_send_req_sync()
 */
static int viommu_send_attach_req(struct viommu_dev *viommu, struct device *dev,
				  struct virtio_iommu_req_attach *req)
{
	/* [한국어] 각 요청의 결과. */
	int ret;
	/* [한국어] 스트림 ID 순회 인덱스. */
	unsigned int i;
	/* [한국어] 이 디바이스가 내는 스트림 ID들이 담긴 펌웨어 명세. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

	/* [한국어] ID마다 ATTACH를 한 번씩 보낸다. */
	for (i = 0; i < fwspec->num_ids; i++) {
		/* [한국어] 요청의 나머지 필드는 그대로 두고 대상만 바꾼다. */
		req->endpoint = cpu_to_le32(fwspec->ids[i]);
		/* [한국어] 결과를 알아야 하므로 동기 요청으로 보낸다. */
		ret = viommu_send_req_sync(viommu, req, sizeof(*req));
		/* [한국어] 하나라도 실패하면 즉시 접는다. 앞서 성공한 ID는
		 * 호출자의 오류 처리가 정리한다. */
		if (ret)
			return ret;
	}
	return 0;	/* [한국어] 모든 스트림 ID에 대해 붙이기가 끝났다. */
}

/*
 * viommu_add_mapping - add a mapping to the internal tree
 *
 * On success, return the new mapping. Otherwise return NULL.
 */
/*
 * [한국어]
 * viommu_add_mapping - 그림자 트리에 매핑 하나를 기록한다
 *
 * @vdomain: 대상 도메인.
 * @iova: 구간의 시작.
 * @end: 구간의 끝(포함).
 * @paddr: 대응하는 물리 주소의 시작.
 * @flags: 이 매핑의 권한 플래그.
 * @return: 0 성공, -ENOMEM.
 *
 * 장치에 요청을 보내는 것과 별개로, 드라이버도 매핑을 기억해 둔다.
 * 이 기록이 있어야 (1) 도메인이 되살아날 때 replay 할 수 있고,
 * (2) iova_to_phys에 답할 수 있다. 장치에 "지금 무엇이 매핑되어
 * 있는가"를 물어볼 방법이 프로토콜에 없기 때문이다.
 *
 * 실행 컨텍스트: 매핑 경로. atomic일 수 있어 GFP_ATOMIC을 쓴다.
 *
 * 호출 체인:
 *   viommu_map_pages() / viommu_domain_map_identity()
 *   → [viommu_add_mapping] → interval_tree_insert()
 */
static int viommu_add_mapping(struct viommu_domain *vdomain, u64 iova, u64 end,
			      phys_addr_t paddr, u32 flags)
{
	/* [한국어] 인터럽트 상태 저장용(변수명이 flags와 겹치지 않게 irqflags). */
	unsigned long irqflags;
	/* [한국어] 트리에 넣을 매핑 기록. */
	struct viommu_mapping *mapping;

	/* [한국어] 매핑 하나 분량을 잡는다. 잠들 수 없는 문맥일 수 있다. */
	mapping = kzalloc_obj(*mapping, GFP_ATOMIC);
	if (!mapping)	/* [한국어] 매핑 기록을 잡지 못했다. */
		return -ENOMEM;

	/* [한국어] 물리 주소를 기록한다 — iova_to_phys의 답이 된다. */
	mapping->paddr		= paddr;
	/* [한국어] 구간의 시작. 트리의 키다. */
	mapping->iova.start	= iova;
	/* [한국어] 구간의 끝(포함). 구간 트리는 닫힌 구간을 쓴다. */
	mapping->iova.last	= end;
	/* [한국어] 권한 플래그. replay가 MAP 요청을 재구성할 때 쓴다. */
	mapping->flags		= flags;

	/* [한국어] 트리 조작을 락으로 감싼다. */
	spin_lock_irqsave(&vdomain->mappings_lock, irqflags);
	interval_tree_insert(&mapping->iova, &vdomain->mappings);	/* [한국어] 구간 트리에 이 매핑을 넣는다. */
	spin_unlock_irqrestore(&vdomain->mappings_lock, irqflags);	/* [한국어] 트리 조작이 끝났으니 락을 놓는다. */

	return 0;	/* [한국어] 그림자 기록이 완료됐다. */
}

/*
 * viommu_del_mappings - remove mappings from the internal tree
 *
 * @vdomain: the domain
 * @iova: start of the range
 * @end: end of the range
 *
 * On success, returns the number of unmapped bytes
 */
/*
 * [한국어]
 * viommu_del_mappings - 그림자 트리에서 구간에 걸친 매핑들을 지운다
 *
 * @vdomain: 대상 도메인.
 * @iova: 구간의 시작.
 * @end: 구간의 끝(포함).
 * @return: 실제로 지운 바이트 수.
 *
 * 중요한 제약이 하나 있다: **virtio-iommu는 MAP 하나로 만든 매핑을
 * UNMAP으로 쪼갤 수 없다.** 그래서 요청 구간이 어떤 매핑의 중간부터
 * 시작하면(mapping->iova.start < iova) 그 지점에서 멈춘다.
 * 반대로 걸친 매핑 전체를 지우게 되므로, 반환된 바이트 수가 요청보다
 * 클 수도 작을 수도 있다 — 호출자가 그것을 검사한다.
 *
 * 순회 중에 노드를 지우므로 다음 노드를 **미리** 얻어 두는
 * 패턴을 쓴다.
 *
 * 실행 컨텍스트: 해제 경로와 도메인 해제. atomic일 수 있다.
 *
 * 호출 체인:
 *   viommu_unmap_pages() / viommu_domain_free() → [viommu_del_mappings]
 */
static size_t viommu_del_mappings(struct viommu_domain *vdomain,
				  u64 iova, u64 end)
{
	/* [한국어] 지운 총 바이트 수. */
	size_t unmapped = 0;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 현재 처리 중인 매핑. */
	struct viommu_mapping *mapping = NULL;
	/* [한국어] 현재 노드와, 지우기 전에 미리 얻어 둔 다음 노드. */
	struct interval_tree_node *node, *next;

	/* [한국어] 트리 조작 구간을 잠근다. */
	spin_lock_irqsave(&vdomain->mappings_lock, flags);
	/* [한국어] 요청 구간과 겹치는 첫 매핑을 찾는다. */
	next = interval_tree_iter_first(&vdomain->mappings, iova, end);
	while (next) {
		/* [한국어] 미리 얻어 둔 노드를 이번 대상으로 삼는다. */
		node = next;
		mapping = container_of(node, struct viommu_mapping, iova);
		/* [한국어] 지우기 **전에** 다음 노드를 얻어 둔다 — 지운 뒤에는
		 * 이 노드로 순회를 이어 갈 수 없다. */
		next = interval_tree_iter_next(node, iova, end);

		/* Trying to split a mapping? */
		/* [한국어] 요청이 이 매핑의 중간부터 시작한다 = 쪼개려는
		 * 시도다. 프로토콜이 허용하지 않으므로 여기서 멈춘다. */
		if (mapping->iova.start < iova)
			break;

		/*
		 * Virtio-iommu doesn't allow UNMAP to split a mapping created
		 * with a single MAP request, so remove the full mapping.
		 */
		/* [한국어] 걸친 매핑은 통째로 지운다. 그래서 요청 길이보다
		 * 많이 지울 수도 있고, 그 수치를 호출자가 검사한다. */
		unmapped += mapping->iova.last - mapping->iova.start + 1;

		/* [한국어] 트리에서 떼어 낸다. */
		interval_tree_remove(node, &vdomain->mappings);
		/* [한국어] 기록을 해제한다. */
		kfree(mapping);
	}
	spin_unlock_irqrestore(&vdomain->mappings_lock, flags);	/* [한국어] 트리 조작이 끝났으니 락을 놓는다. */

	return unmapped;	/* [한국어] 실제로 지운 바이트 수를 호출자에게 알린다 — 요청과 다를 수 있다. */
}

/*
 * Fill the domain with identity mappings, skipping the device's reserved
 * regions.
 */
/*
 * [한국어]
 * viommu_domain_map_identity - 도메인을 1:1 매핑으로 채운다
 *
 * @vdev: 예약 영역 목록을 가진 엔드포인트.
 * @vdomain: 채울 도메인.
 * @return: 0 성공, 음수 오류.
 *
 * 장치가 우회(bypass) 기능을 알리지 않았을 때의 통과 모드 구현이다.
 * 하드웨어 지원이 없으니 **소프트웨어로 흉내 낸다** — aperture 전체를
 * IOVA == 물리 주소로 매핑하는 것이다.
 *
 * 다만 예약 영역은 건너뛰어야 한다. MSI 도어벨 같은 구간을 1:1로
 * 덮어 버리면 인터럽트가 엉뚱한 곳으로 가기 때문이다. 그래서
 * 예약 목록(시작 주소 순으로 정렬되어 있다)을 훑으며 그 사이사이의
 * 빈틈만 매핑한다:
 *
 *   iova ──[매핑]── resv1 ──[매핑]── resv2 ──[매핑]── limit
 *
 * 예약 구간은 granule 경계로 넓혀서 계산한다. 페이지 단위로만
 * 매핑할 수 있으므로, 예약 영역이 페이지 일부만 차지해도 그 페이지
 * 전체를 비워 둬야 한다.
 *
 * 실행 컨텍스트: identity 도메인 생성. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   viommu_domain_alloc_identity() → [viommu_domain_map_identity]
 *   → viommu_add_mapping()
 */
static int viommu_domain_map_identity(struct viommu_endpoint *vdev,
				      struct viommu_domain *vdomain)
{
	/* [한국어] 매핑 결과. */
	int ret;
	/* [한국어] 예약 영역 순회 커서. */
	struct iommu_resv_region *resv;
	/* [한국어] 다음에 매핑할 구간의 시작. 빈틈을 지날 때마다 전진한다. */
	u64 iova = vdomain->domain.geometry.aperture_start;
	/* [한국어] 매핑할 수 있는 마지막 주소. */
	u64 limit = vdomain->domain.geometry.aperture_end;
	/* [한국어] 1:1 매핑의 권한 — 읽기와 쓰기 모두 허용한다. */
	u32 flags = VIRTIO_IOMMU_MAP_F_READ | VIRTIO_IOMMU_MAP_F_WRITE;
	/* [한국어] 매핑 단위. 지원하는 페이지 크기 중 가장 작은 것이다. */
	unsigned long granule = 1UL << __ffs(vdomain->domain.pgsize_bitmap);

	/* [한국어] 시작을 페이지 경계로 올린다. */
	iova = ALIGN(iova, granule);
	/* [한국어] 끝을 페이지 경계로 내린다 — 마지막 불완전한 페이지는 버린다. */
	limit = ALIGN_DOWN(limit + 1, granule) - 1;

	/* [한국어] 시작 주소 순으로 정렬된 예약 영역들을 차례로 지난다. */
	list_for_each_entry(resv, &vdev->resv_regions, list) {
		/* [한국어] 예약 구간을 페이지 경계까지 넓힌다 — 일부만
		 * 겹쳐도 그 페이지 전체를 비워 둬야 하기 때문이다. */
		u64 resv_start = ALIGN_DOWN(resv->start, granule);
		u64 resv_end = ALIGN(resv->start + resv->length, granule) - 1;	/* [한국어] 예약 구간의 끝도 페이지 경계까지 넓힌다. */

		/* No overlap */
		/* [한국어] 아직 지나온 구간이거나 aperture 밖이면 무시한다. */
		if (resv_end < iova || resv_start > limit)
			continue;

		/* [한국어] 현재 위치와 예약 구간 사이에 빈틈이 있으면
		 * 그 부분을 1:1로 매핑한다. */
		if (resv_start > iova) {
			ret = viommu_add_mapping(vdomain, iova, resv_start - 1,	/* [한국어] 현재 위치부터 예약 구간 직전까지를 1:1로 매핑한다. */
						 (phys_addr_t)iova, flags);
			if (ret)	/* [한국어] 매핑에 실패하면 지금까지 만든 것을 되돌린다. */
				goto err_unmap;
		}

		/* [한국어] 이 예약 구간이 aperture 끝까지 덮는다면 더 매핑할
		 * 것이 없다. */
		if (resv_end >= limit)
			return 0;

		/* [한국어] 예약 구간 다음으로 커서를 옮긴다. */
		iova = resv_end + 1;
	}

	/* [한국어] 마지막 예약 구간 이후부터 끝까지를 매핑한다.
	 * 예약이 하나도 없었다면 이 한 번이 전체를 덮는다. */
	ret = viommu_add_mapping(vdomain, iova, limit, (phys_addr_t)iova,
				 flags);
	if (ret)	/* [한국어] 마지막 구간 매핑에 실패했다. */
		goto err_unmap;
	return 0;

/* [한국어] 도중에 실패했다 — 지금까지 만든 매핑을 모두 지운다. */
err_unmap:
	/* [한국어] 0부터 현재 위치까지가 지금까지 만든 범위다. */
	viommu_del_mappings(vdomain, 0, iova);
	return ret;	/* [한국어] 되돌리기를 마치고 실패 이유를 전한다. */
}

/*
 * viommu_replay_mappings - re-send MAP requests
 *
 * When reattaching a domain that was previously detached from all endpoints,
 * mappings were deleted from the device. Re-create the mappings available in
 * the internal tree.
 */
/*
 * [한국어]
 * viommu_replay_mappings - 그림자 트리의 매핑을 장치에 다시 보낸다
 *
 * @vdomain: 되살릴 도메인.
 * @return: 0 성공, 첫 실패의 errno.
 *
 * 그림자 트리가 왜 필요한지를 보여 주는 함수다. 장치는 도메인에서
 * 마지막 엔드포인트가 떨어져 나가면 그 도메인의 매핑을 전부 지우고
 * 도메인 자체도 없앤다. 그러나 게스트 쪽 도메인 객체는 살아 있고,
 * 나중에 다른 엔드포인트가 붙을 수 있다. 그때 트리를 처음부터 훑어
 * MAP 요청을 다시 보내는 것이 이 함수다.
 *
 * 락을 잡은 채로 동기 요청을 보내는 점에 주의 — 트리가 바뀌면
 * 순회가 깨지므로 어쩔 수 없다. 그만큼 이 경로는 비싸다.
 *
 * 실행 컨텍스트: attach 경로. mappings_lock을 잡은 채 요청을 보낸다.
 *
 * 호출 체인:
 *   viommu_attach_dev() → [viommu_replay_mappings]
 *   → viommu_send_req_sync()
 */
static int viommu_replay_mappings(struct viommu_domain *vdomain)
{
	/* [한국어] 요청 결과. */
	int ret = 0;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 현재 매핑 기록. */
	struct viommu_mapping *mapping;
	/* [한국어] 트리 순회 커서. */
	struct interval_tree_node *node;
	/* [한국어] 매번 새로 조립할 MAP 요청. */
	struct virtio_iommu_req_map map;

	/* [한국어] 순회 중 트리가 바뀌지 않도록 잠근다. */
	spin_lock_irqsave(&vdomain->mappings_lock, flags);
	/* [한국어] 주소 공간 전체를 대상으로 첫 매핑부터 훑는다. */
	node = interval_tree_iter_first(&vdomain->mappings, 0, -1UL);
	while (node) {
		/* [한국어] 노드에서 매핑 기록을 복원한다. */
		mapping = container_of(node, struct viommu_mapping, iova);
		/* [한국어] 기록된 값 그대로 MAP 요청을 재구성한다.
		 * 모든 수치를 리틀엔디언으로 바꾸는 것이 virtio의 규약이다. */
		map = (struct virtio_iommu_req_map) {
			.head.type	= VIRTIO_IOMMU_T_MAP,	/* [한국어] 요청 종류를 MAP으로 지정한다. */
			.domain		= cpu_to_le32(vdomain->id),
			.virt_start	= cpu_to_le64(mapping->iova.start),
			.virt_end	= cpu_to_le64(mapping->iova.last),
			.phys_start	= cpu_to_le64(mapping->paddr),
			.flags		= cpu_to_le32(mapping->flags),
		};

		/* [한국어] 하나씩 동기로 보낸다 — 실패를 즉시 알아야
		 * 이후 매핑을 헛되이 보내지 않는다. */
		ret = viommu_send_req_sync(vdomain->viommu, &map, sizeof(map));
		if (ret)	/* [한국어] 하나라도 실패하면 나머지를 보내지 않는다. */
			break;

		/* [한국어] 다음 매핑으로 넘어간다. */
		node = interval_tree_iter_next(node, 0, -1UL);
	}
	spin_unlock_irqrestore(&vdomain->mappings_lock, flags);	/* [한국어] 순회가 끝났으니 락을 놓는다. */

	return ret;	/* [한국어] 되살리기 결과를 전한다. */
}

/*
 * [한국어]
 * viommu_add_resv_mem - PROBE 응답의 예약 메모리 속성을 목록에 넣는다
 *
 * @vdev: 대상 엔드포인트.
 * @mem: 장치가 준 예약 메모리 속성.
 * @len: 그 속성의 길이.
 * @return: 0 성공, -EOVERFLOW/-EINVAL/-ENOMEM.
 *
 * 장치가 "이 엔드포인트는 이 구간을 건드리면 안 된다" 또는 "이
 * 구간이 MSI 도어벨이다"라고 알려 준 것을 커널의 예약 영역 객체로
 * 옮긴다.
 *
 * 오버플로 검사가 꼼꼼한 이유: 이 값들은 **호스트가 준 값**이라
 * 신뢰할 수 없다. 64비트 값을 phys_addr_t로 좁힐 때 잘릴 수 있고,
 * end - start + 1이 0으로 감길 수도 있다. 셋 다 검사한다.
 *
 * 모르는 subtype은 경고만 하고 일반 예약으로 다룬다 — 앞으로 추가될
 * 종류를 안전한 쪽으로 처리하려는 것이다(fallthrough).
 *
 * 목록을 시작 주소 순으로 유지하는 것도 중요하다.
 * viommu_domain_map_identity()가 정렬을 전제로 빈틈을 계산한다.
 *
 * 실행 컨텍스트: 엔드포인트 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   viommu_probe_endpoint() → [viommu_add_resv_mem]
 *   → iommu_alloc_resv_region()
 */
static int viommu_add_resv_mem(struct viommu_endpoint *vdev,
			       struct virtio_iommu_probe_resv_mem *mem,
			       size_t len)
{
	/* [한국어] 구간의 길이. */
	size_t size;
	/* [한국어] 장치가 준 원본 64비트 값. */
	u64 start64, end64;
	/* [한국어] phys_addr_t로 좁힌 값. 좁히면서 잘렸는지 비교에 쓴다. */
	phys_addr_t start, end;
	/* [한국어] 만들 예약 영역과, 삽입 위치를 찾는 커서. */
	struct iommu_resv_region *region = NULL, *next;
	/* [한국어] MSI 창의 권한 — 쓰기만 필요하고, 실행은 막고, MMIO다. */
	unsigned long prot = IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO;

	/* [한국어] 리틀엔디언 값을 읽어 64비트와 phys_addr_t 양쪽에 담는다. */
	start = start64 = le64_to_cpu(mem->start);
	end = end64 = le64_to_cpu(mem->end);	/* [한국어] 끝 주소도 64비트와 phys_addr_t 양쪽에 담는다. */
	size = end64 - start64 + 1;	/* [한국어] 구간 길이를 구한다(양 끝 포함이라 +1). */

	/* Catch any overflow, including the unlikely end64 - start64 + 1 = 0 */
	/* [한국어] 세 가지를 한 번에 본다: 주소를 좁히며 잘리지 않았는지
	 * 둘, 그리고 길이 계산이 감기지 않았는지 하나.
	 * 호스트가 준 값을 그대로 믿을 수 없기 때문이다. */
	if (start != start64 || end != end64 || size < end64 - start64)
		return -EOVERFLOW;

	/* [한국어] 속성 길이가 구조체보다 짧으면 잘린 응답이다. */
	if (len < sizeof(*mem))
		return -EINVAL;

	/* [한국어] 예약의 종류에 따라 만들 영역이 달라진다. */
	switch (mem->subtype) {
	/* [한국어] 모르는 종류 — 경고를 남기고 아래의 일반 예약으로
	 * 처리한다. 안전한 쪽(더 많이 예약)으로 기우는 선택이다. */
	default:
		dev_warn(vdev->dev, "unknown resv mem subtype 0x%x\n",	/* [한국어] 모르는 종류임을 알리되 처리는 계속한다. */
			 mem->subtype);
		fallthrough;
	/* [한국어] 일반 예약 — 이 구간에는 아무것도 매핑하지 않는다. */
	case VIRTIO_IOMMU_RESV_MEM_T_RESERVED:
		region = iommu_alloc_resv_region(start, size, 0,	/* [한국어] 권한 없는 일반 예약 영역으로 만든다. */
						 IOMMU_RESV_RESERVED,
						 GFP_KERNEL);
		break;
	/* [한국어] MSI 창 — 인터럽트 도어벨이 이 구간을 통과해야 하므로
	 * 쓰기 권한과 MMIO 속성을 준다. */
	case VIRTIO_IOMMU_RESV_MEM_T_MSI:
		region = iommu_alloc_resv_region(start, size, prot,	/* [한국어] MSI 도어벨이 통과해야 하므로 쓰기 권한을 준다. */
						 IOMMU_RESV_MSI,
						 GFP_KERNEL);
		break;
	}
	if (!region)	/* [한국어] 예약 영역 객체를 잡지 못했다. */
		return -ENOMEM;

	/* Keep the list sorted */
	/* [한국어] 시작 주소 순으로 넣을 자리를 찾는다. identity 매핑이
	 * 이 정렬을 전제로 빈틈을 계산하므로 순서가 중요하다. */
	list_for_each_entry(next, &vdev->resv_regions, list) {
		if (next->start > region->start)	/* [한국어] 자기보다 시작 주소가 큰 첫 항목을 찾으면 그 앞이 제자리다. */
			break;
	}
	/* [한국어] 찾은 항목의 **앞**에 넣는다. 루프가 끝까지 돌았다면
	 * next는 목록 머리이므로 결국 맨 뒤에 붙는다. */
	list_add_tail(&region->list, &next->list);
	return 0;	/* [한국어] 정렬된 위치에 삽입을 마쳤다. */
}

/*
 * [한국어]
 * viommu_probe_endpoint - 엔드포인트별 속성을 장치에 물어본다
 *
 * @viommu: 대상 장치.
 * @dev: 물어볼 디바이스.
 * @return: 0 성공, 음수 오류.
 *
 * PROBE 요청 하나를 보내면, 장치가 응답 버퍼의 뒷부분에 속성들을
 * 줄줄이 채워 준다. 각 속성은 [타입, 길이, 내용] 구조라 길이를
 * 따라가며 파싱한다. 타입이 NONE이거나 probe_size를 다 소진하면 끝이다.
 *
 * 여러 스트림 ID를 내는 엔드포인트라도 첫 번째만 물어본다.
 * 원본 주석이 밝히듯 "속성이 일관적이라고 가정"하는 단순화다.
 *
 * 실행 컨텍스트: 디바이스 probe. 프로세스 컨텍스트(GFP_KERNEL).
 *
 * 호출 체인:
 *   viommu_probe_device() → [viommu_probe_endpoint]
 *   → viommu_send_req_sync() → viommu_add_resv_mem()
 */
static int viommu_probe_endpoint(struct viommu_dev *viommu, struct device *dev)
{
	/* [한국어] 각 단계의 결과. */
	int ret;
	/* [한국어] 현재 속성의 타입과 길이. */
	u16 type, len;
	/* [한국어] 속성 영역에서 지금까지 소비한 바이트 수. */
	size_t cur = 0;
	/* [한국어] 요청 버퍼 전체의 길이. */
	size_t probe_len;
	/* [한국어] 요청 겸 응답 버퍼. */
	struct virtio_iommu_req_probe *probe;
	/* [한국어] 응답 속성 영역을 훑는 커서. */
	struct virtio_iommu_probe_property *prop;
	/* [한국어] 이 디바이스의 스트림 ID들. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] 속성을 받아 넣을 엔드포인트 상태. */
	struct viommu_endpoint *vdev = dev_iommu_priv_get(dev);

	/* [한국어] ID가 하나도 없으면 물어볼 대상이 없다. */
	if (!fwspec->num_ids)
		return -EINVAL;

	/* [한국어] 헤더 + 장치가 채울 속성 영역 + 상태 tail. */
	probe_len = sizeof(*probe) + viommu->probe_size +
		    sizeof(struct virtio_iommu_req_tail);
	/* [한국어] 0으로 초기화해 잡는다 — 응답으로 덮이지 않은 부분이
	 * 쓰레기 값으로 읽히지 않게 한다. */
	probe = kzalloc(probe_len, GFP_KERNEL);
	if (!probe)	/* [한국어] 요청 버퍼를 잡지 못했다. */
		return -ENOMEM;

	/* [한국어] 요청 종류를 PROBE로 지정한다. */
	probe->head.type = VIRTIO_IOMMU_T_PROBE;
	/*
	 * For now, assume that properties of an endpoint that outputs multiple
	 * IDs are consistent. Only probe the first one.
	 */
	/* [한국어] 첫 스트림 ID만 물어본다. 같은 디바이스의 ID들은 속성이
	 * 같다고 가정하는 단순화다. */
	probe->endpoint = cpu_to_le32(fwspec->ids[0]);

	/* [한국어] 응답을 받아야 하므로 동기로 보낸다. */
	ret = viommu_send_req_sync(viommu, probe, probe_len);
	if (ret)	/* [한국어] PROBE 요청 자체가 실패했다 — 파싱할 응답이 없다. */
		goto out_free;

	/* [한국어] 속성 영역의 시작으로 커서를 놓는다. */
	prop = (void *)probe->properties;
	/* [한국어] 타입 필드에서 타입 비트만 뽑는다(상위 비트는 다른 용도). */
	type = le16_to_cpu(prop->type) & VIRTIO_IOMMU_PROBE_T_MASK;

	/* [한국어] 종료 표식을 만나거나 영역을 다 소비할 때까지 훑는다. */
	while (type != VIRTIO_IOMMU_PROBE_T_NONE &&
	       cur < viommu->probe_size) {
		/* [한국어] 이 속성이 차지하는 전체 크기 = 헤더 + 본문. */
		len = le16_to_cpu(prop->length) + sizeof(*prop);

		/* [한국어] 속성 종류별 처리. */
		switch (type) {
		/* [한국어] 예약 메모리 구간 — 목록에 정렬해 넣는다. */
		case VIRTIO_IOMMU_PROBE_T_RESV_MEM:
			ret = viommu_add_resv_mem(vdev, (void *)prop, len);	/* [한국어] 예약 메모리 속성을 목록에 넣는다. */
			break;
		/* [한국어] 모르는 속성은 건너뛴다. 길이를 알고 있으므로
		 * 파싱을 이어 갈 수 있다. */
		default:
			dev_err(dev, "unknown viommu prop 0x%x\n", type);	/* [한국어] 모르는 속성이지만 길이를 알므로 파싱은 이어 간다. */
		}

		/* [한국어] 처리에 실패해도 파싱은 계속한다 — 나머지 속성은
		 * 여전히 쓸 만할 수 있다. */
		if (ret)
			dev_err(dev, "failed to parse viommu prop 0x%x\n", type);

		/* [한국어] 소비한 만큼 커서를 전진시킨다. */
		cur += len;
		/* [한국어] 영역을 다 소비했으면 더 읽으면 안 된다 —
		 * 버퍼 밖을 읽지 않기 위한 경계 검사다. */
		if (cur >= viommu->probe_size)
			break;

		/* [한국어] 다음 속성의 위치로 커서를 옮긴다. */
		prop = (void *)probe->properties + cur;
		type = le16_to_cpu(prop->type) & VIRTIO_IOMMU_PROBE_T_MASK;	/* [한국어] 다음 속성의 타입 비트를 뽑는다. */
	}

/* [한국어] 성공/실패 공통 정리 지점. */
out_free:
	/* [한국어] 요청 버퍼를 해제한다. 속성은 이미 목록으로 옮겨졌다. */
	kfree(probe);
	return ret;	/* [한국어] 마지막 속성 처리의 결과를 전한다. */
}

/*
 * [한국어]
 * viommu_fault_handler - 장치가 알려 온 폴트를 로그로 남긴다
 *
 * @viommu: 폴트를 보고한 장치.
 * @fault: 폴트 내용.
 * @return: 항상 0.
 *
 * 아직은 로그만 남긴다. 원본의 TODO가 말하듯, 엔드포인트 ID로
 * 디바이스를 찾아 report_iommu_fault()로 상위에 올리는 것이
 * 다음 단계다. 지금은 폴트가 나면 사람이 로그를 보고 판단해야 한다.
 *
 * 주소가 유효한 경우와 아닌 경우로 출력이 갈린다. 도메인 폴트
 * (엔드포인트가 어느 도메인에도 안 붙어 있음) 같은 경우는 주소가
 * 의미 없기 때문이다.
 *
 * ratelimited를 쓰는 이유: 잘못된 드라이버 하나가 초당 수천 번
 * 폴트를 낼 수 있어, 로그가 그것으로 가득 차면 안 된다.
 *
 * 실행 컨텍스트: 이벤트 큐 콜백(인터럽트 문맥).
 *
 * 호출 체인:
 *   viommu_event_handler() → [viommu_fault_handler]
 */
static int viommu_fault_handler(struct viommu_dev *viommu,
				struct virtio_iommu_fault *fault)
{
	/* [한국어] 폴트 원인을 사람이 읽을 문자열로 옮길 자리. */
	char *reason_str;

	/* [한국어] 폴트의 원인 코드. */
	u8 reason	= fault->reason;
	/* [한국어] 부가 정보 비트들(주소 유효 여부, 접근 종류). */
	u32 flags	= le32_to_cpu(fault->flags);
	/* [한국어] 폴트를 낸 엔드포인트의 스트림 ID. */
	u32 endpoint	= le32_to_cpu(fault->endpoint);
	/* [한국어] 폴트가 난 IOVA(유효할 때만 의미가 있다). */
	u64 address	= le64_to_cpu(fault->address);

	/* [한국어] 원인 코드를 문자열로 옮긴다. */
	switch (reason) {
	/* [한국어] 엔드포인트가 어느 도메인에도 붙어 있지 않다. */
	case VIRTIO_IOMMU_FAULT_R_DOMAIN:
		reason_str = "domain";	/* [한국어] 엔드포인트가 어느 도메인에도 붙어 있지 않다는 뜻이다. */
		break;
	/* [한국어] 도메인은 있으나 그 IOVA에 매핑이 없거나 권한이 부족하다. */
	case VIRTIO_IOMMU_FAULT_R_MAPPING:
		reason_str = "page";	/* [한국어] 매핑이 없거나 권한이 모자란 접근이었다. */
		break;
	/* [한국어] 장치가 원인을 특정하지 못했거나, 이 드라이버가 모르는
	 * 새 원인 코드다 — 둘 다 같은 문자열로 묶는다. */
	case VIRTIO_IOMMU_FAULT_R_UNKNOWN:
	default:	/* [한국어] 이 드라이버가 모르는 새 원인 코드도 여기로 온다. */
		reason_str = "unknown";	/* [한국어] 원인을 특정할 수 없을 때의 표시. */
		break;
	}

	/* TODO: find EP by ID and report_iommu_fault */
	/* [한국어] 주소가 유효할 때만 그것과 접근 종류(RWX)를 함께 찍는다.
	 * 삼항 연산자로 켜진 비트에 해당하는 글자만 이어 붙인다. */
	if (flags & VIRTIO_IOMMU_FAULT_F_ADDRESS)
		dev_err_ratelimited(viommu->dev, "%s fault from EP %u at %#llx [%s%s%s]\n",
				    reason_str, endpoint, address,
				    flags & VIRTIO_IOMMU_FAULT_F_READ ? "R" : "",
				    flags & VIRTIO_IOMMU_FAULT_F_WRITE ? "W" : "",
				    flags & VIRTIO_IOMMU_FAULT_F_EXEC ? "X" : "");
	else
		/* [한국어] 주소가 의미 없는 폴트 — 원인과 엔드포인트만 남긴다. */
		dev_err_ratelimited(viommu->dev, "%s fault from EP %u\n",
				    reason_str, endpoint);
	return 0;	/* [한국어] 로그만 남기는 현재 구현에서는 항상 성공이다. */
}

/*
 * [한국어]
 * viommu_event_handler - 이벤트 큐 콜백. 올라온 이벤트를 처리하고 버퍼를 되돌린다
 *
 * @vq: 이벤트 큐.
 * @return: 없음.
 *
 * virtio의 이벤트 방향은 게스트가 먼저 빈 버퍼를 넣어 두고, 장치가
 * 그것을 채워 돌려주는 방식이다. 그래서 이 핸들러는 **처리와 재공급을
 * 한 몸으로** 한다 — 꺼낸 버퍼를 처리한 뒤 곧바로 다시 큐에 넣는다.
 * 그러지 않으면 이벤트 버퍼가 하나씩 줄어들다 결국 폴트를 못 받는다.
 *
 * 두 가지 방어가 있다. 길이가 구조체보다 크면 장치가 잘못 쓴 것이고,
 * 헤더의 예약 비트가 켜져 있으면 이 드라이버가 모르는 종류의
 * 이벤트다. 둘 다 처리하지 않고 버퍼만 되돌린다.
 *
 * 실행 컨텍스트: virtqueue 콜백 — 인터럽트 문맥이다. 그래서
 * 재공급 할당이 GFP_ATOMIC이다.
 *
 * 호출 체인:
 *   virtio 인터럽트 → [viommu_event_handler] → viommu_fault_handler()
 */
static void viommu_event_handler(struct virtqueue *vq)
{
	/* [한국어] 재공급 결과. */
	int ret;
	/* [한국어] 장치가 쓴 길이. */
	unsigned int len;
	/* [한국어] 버퍼를 되돌릴 때 쓸 산란 목록 항목. */
	struct scatterlist sg[1];
	/* [한국어] 꺼낸 이벤트 버퍼. */
	struct viommu_event *evt;
	/* [한국어] virtio 장치의 private에 걸어 둔 이 드라이버의 상태. */
	struct viommu_dev *viommu = vq->vdev->priv;

	/* [한국어] 올라온 이벤트를 모두 소비한다. */
	while ((evt = virtqueue_get_buf(vq, &len)) != NULL) {
		/* [한국어] 장치가 버퍼보다 많이 썼다 = 프로토콜 위반이다.
		 * 내용을 신뢰할 수 없으니 처리하지 않는다. */
		if (len > sizeof(*evt)) {
			dev_err(viommu->dev,	/* [한국어] 장치가 프로토콜을 어겼음을 알린다. */
				"invalid event buffer (len %u != %zu)\n",
				len, sizeof(*evt));
		/* [한국어] 예약 비트가 모두 0일 때만 폴트로 해석한다.
		 * 앞으로 추가될 이벤트 종류를 조용히 무시하기 위한 장치다. */
		} else if (!(evt->head & VIOMMU_FAULT_RESV_MASK)) {
			viommu_fault_handler(viommu, &evt->fault);	/* [한국어] 예약 비트가 깨끗한 이벤트만 폴트로 해석한다. */
		}

		/* [한국어] 같은 버퍼를 다시 장치에 내어 준다 — 그러지 않으면
		 * 이벤트를 받을 자리가 점점 줄어든다. */
		sg_init_one(sg, evt, sizeof(*evt));
		/* [한국어] 인터럽트 문맥이라 ATOMIC 할당을 쓴다. */
		ret = virtqueue_add_inbuf(vq, sg, 1, evt, GFP_ATOMIC);
		if (ret)	/* [한국어] 버퍼를 되돌리지 못하면 다음 폴트를 놓칠 수 있다 — 알려 둔다. */
			dev_err(viommu->dev, "could not add event buffer\n");
	}

	/* [한국어] 되돌린 버퍼들을 장치에 알린다. */
	virtqueue_kick(vq);
}

/* IOMMU API */

/*
 * [한국어]
 * viommu_domain_alloc_paging - 페이징 도메인을 만든다
 *
 * @dev: 이 도메인을 쓸 디바이스(어느 virtio-iommu 장치인지의 근거).
 * @return: 새 도메인, 실패하면 ERR_PTR.
 *
 * 도메인의 성질을 장치에서 그대로 물려받는다: 페이지 크기 비트맵,
 * 입력 주소 범위, 허용 MAP 플래그. 이 드라이버에는 스스로 정할
 * 여지가 거의 없다 — 실제 변환은 호스트가 하기 때문이다.
 *
 * 첫 검사가 중요하다. 장치의 최소 페이지 크기가 게스트의 PAGE_SIZE보다
 * 크면, 게스트가 페이지 하나만 매핑하려 해도 그보다 넓은 영역이
 * 열려 버린다. 그런 구성은 안전하지 않으므로 거부한다.
 *
 * 도메인 ID는 장치가 알려 준 범위 안에서 ida로 받는다. 이 ID가
 * 게스트와 호스트가 같은 도메인을 가리키는 유일한 이름이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL).
 *
 * 호출 체인:
 *   IOMMU 코어 domain_alloc_paging → [viommu_domain_alloc_paging]
 */
static struct iommu_domain *viommu_domain_alloc_paging(struct device *dev)
{
	/* [한국어] 이 디바이스의 엔드포인트 상태. */
	struct viommu_endpoint *vdev = dev_iommu_priv_get(dev);
	/* [한국어] 담당 virtio-iommu 장치. */
	struct viommu_dev *viommu = vdev->viommu;
	/* [한국어] 장치가 지원하는 가장 작은 페이지 크기. */
	unsigned long viommu_page_size;
	/* [한국어] 만들 도메인. */
	struct viommu_domain *vdomain;
	/* [한국어] ID 할당 결과. */
	int ret;

	/* [한국어] 비트맵에서 가장 낮은 비트가 최소 페이지 크기다. */
	viommu_page_size = 1UL << __ffs(viommu->pgsize_bitmap);
	/* [한국어] 장치의 단위가 게스트 페이지보다 크면, 한 페이지만
	 * 열려던 매핑이 이웃 페이지까지 노출시킨다 — 쓸 수 없다. */
	if (viommu_page_size > PAGE_SIZE) {
		dev_err(vdev->dev,	/* [한국어] 게스트 페이지보다 큰 단위는 안전하게 쓸 수 없다. */
			"granule 0x%lx larger than system page size 0x%lx\n",
			viommu_page_size, PAGE_SIZE);
		return ERR_PTR(-ENODEV);	/* [한국어] 이 장치로는 도메인을 만들 수 없다고 알린다. */
	}

	/* [한국어] 도메인 뼈대를 0으로 초기화해 받는다. */
	vdomain = kzalloc_obj(*vdomain);
	if (!vdomain)	/* [한국어] 도메인 구조체를 잡지 못했다. */
		return ERR_PTR(-ENOMEM);

	/* [한국어] 그림자 트리를 보호할 락. */
	spin_lock_init(&vdomain->mappings_lock);
	/* [한국어] 빈 구간 트리로 시작한다. */
	vdomain->mappings = RB_ROOT_CACHED;

	/* [한국어] 장치가 허용한 범위 안에서 도메인 ID를 받는다.
	 * 이 ID가 곧 호스트와 공유하는 도메인의 이름이다. */
	ret = ida_alloc_range(&viommu->domain_ids, viommu->first_domain,
			      viommu->last_domain, GFP_KERNEL);
	if (ret < 0) {
		/* [한국어] ID가 동나면 도메인을 만들 수 없다. */
		kfree(vdomain);
		return ERR_PTR(ret);	/* [한국어] ID가 동나면 도메인을 만들 수 없다. */
	}

	/* [한국어] 양수 반환값이 곧 할당된 ID다. */
	vdomain->id = (unsigned int)ret;

	/* [한국어] 지원 페이지 크기를 장치에서 그대로 물려받는다. */
	vdomain->domain.pgsize_bitmap = viommu->pgsize_bitmap;
	/* [한국어] 입력 주소 범위도 장치가 정한 대로 쓴다. */
	vdomain->domain.geometry = viommu->geometry;

	/* [한국어] 허용되는 MAP 플래그 집합을 물려받는다. */
	vdomain->map_flags = viommu->map_flags;
	/* [한국어] 요청을 보낼 때 쓸 장치를 기억해 둔다. */
	vdomain->viommu = viommu;

	/* [한국어] 코어에는 임베드된 부분만 돌려준다. */
	return &vdomain->domain;
}

/*
 * [한국어]
 * viommu_domain_free - 도메인을 해제한다
 *
 * @domain: 해제할 도메인.
 * @return: 없음.
 *
 * 장치에 "이 도메인을 없애라"는 요청을 보내지 않는 점에 주목.
 * 그럴 필요가 없다 — 장치는 마지막 엔드포인트가 떨어져 나가는
 * 순간 이미 도메인을 정리했다. 여기서 하는 일은 게스트 쪽 흔적,
 * 즉 그림자 트리와 도메인 ID를 되돌리는 것뿐이다.
 *
 * viommu가 NULL인 경우를 검사하는 이유: 정적 identity 도메인은
 * ida로 받은 ID가 없어 반납하면 안 되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 free → [viommu_domain_free] → viommu_del_mappings()
 */
static void viommu_domain_free(struct iommu_domain *domain)
{
	/* [한국어] 드라이버 쪽 도메인을 복원한다. */
	struct viommu_domain *vdomain = to_viommu_domain(domain);

	/* Free all remaining mappings */
	/* [한국어] 주소 공간 전체를 대상으로 그림자 트리를 비운다.
	 * 장치 쪽 매핑은 이미 사라졌으므로 요청은 보내지 않는다. */
	viommu_del_mappings(vdomain, 0, ULLONG_MAX);

	/* [한국어] 정적 identity 도메인은 viommu가 NULL이라 ID도 없다 —
	 * 그런 도메인에 ida_free를 부르면 안 된다. */
	if (vdomain->viommu)
		ida_free(&vdomain->viommu->domain_ids, vdomain->id);

	kfree(vdomain);	/* [한국어] 그림자 트리와 ID를 되돌린 뒤 도메인 자체를 해제한다. */
}

/*
 * [한국어]
 * viommu_domain_alloc_identity - 통과(identity) 도메인을 만든다
 *
 * @dev: 대상 디바이스.
 * @return: identity 도메인, 실패하면 ERR_PTR.
 *
 * 두 갈래로 갈리는 것이 이 함수의 전부다.
 *
 *  - 장치가 BYPASS_CONFIG 기능을 알렸다면, 하드웨어(호스트) 차원의
 *    우회를 쓸 수 있다. 전역 정적 도메인 하나를 그대로 돌려주면
 *    되고, 매핑을 하나도 만들지 않는다.
 *  - 그 기능이 없다면 소프트웨어로 흉내 낸다. 일반 페이징 도메인을
 *    만들어 aperture 전체를 1:1로 채운다. 비싸지만 동작은 같다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 domain_alloc_identity → [viommu_domain_alloc_identity]
 *   → viommu_domain_alloc_paging() → viommu_domain_map_identity()
 */
static struct iommu_domain *viommu_domain_alloc_identity(struct device *dev)
{
	/* [한국어] 이 디바이스의 엔드포인트 상태(예약 영역 목록을 갖는다). */
	struct viommu_endpoint *vdev = dev_iommu_priv_get(dev);
	/* [한국어] 소프트웨어 경로에서 만들 도메인. */
	struct iommu_domain *domain;
	/* [한국어] 1:1 채우기 결과. */
	int ret;

	/* [한국어] 호스트가 우회를 지원하면 전역 정적 도메인이면 충분하다 —
	 * 상태도 매핑도 필요 없다. */
	if (virtio_has_feature(vdev->viommu->vdev,
			       VIRTIO_IOMMU_F_BYPASS_CONFIG))
		return &viommu_identity_domain.domain;

	/* [한국어] 우회 지원이 없다 — 진짜 도메인을 만들어 1:1로 채운다. */
	domain = viommu_domain_alloc_paging(dev);
	if (IS_ERR(domain))	/* [한국어] 페이징 도메인 생성이 실패했다 — 그 오류를 그대로 전한다. */
		return domain;

	/* [한국어] aperture 전체를 IOVA == 물리 주소로 덮되, 예약 영역은
	 * 건너뛴다. */
	ret = viommu_domain_map_identity(vdev, to_viommu_domain(domain));
	if (ret) {
		/* [한국어] 채우다 실패하면 반쯤 매핑된 도메인을 남기지 않는다. */
		viommu_domain_free(domain);
		return ERR_PTR(ret);	/* [한국어] 1:1 채우기에 실패한 이유를 전한다. */
	}
	return domain;	/* [한국어] 예약 영역을 피해 1:1로 채운 도메인을 돌려준다. */
}

/*
 * [한국어]
 * viommu_attach_dev - 디바이스를 페이징 도메인에 붙인다
 *
 * @domain: 붙일 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인(이 드라이버는 vdev->vdomain을 쓴다).
 * @return: 0 성공, 음수 오류.
 *
 * 이 드라이버에서 가장 미묘한 함수다. 원본 주석이 설명하는 비대칭
 * 때문이다: **장치와 드라이버의 도메인 수명이 다르다.**
 *
 * 장치 쪽에서는 엔드포인트를 새 도메인에 붙이는 순간 옛 도메인에서
 * 떨어져 나가고, 옛 도메인에 남은 엔드포인트가 없으면 그 도메인의
 * 매핑이 전부 지워지고 도메인 자체가 사라진다.
 * 드라이버 쪽에서는 그 도메인 객체가 그대로 살아 있다. 그래서
 * 다시 붙을 때 매핑을 되살려야 하고, 그 판단이 nr_endpoints가
 * 0인지로 이뤄진다.
 *
 * 순서에 유의: ATTACH를 보내기 **전에** 옛 도메인의 카운트를 줄인다.
 * 장치가 붙이는 즉시 떼어 내기 때문에, 그 사실을 먼저 장부에
 * 반영하는 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. vdev->vdomain은 group->mutex가
 * 보호한다.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev → [viommu_attach_dev]
 *   → viommu_send_attach_req() → viommu_replay_mappings()
 */
static int viommu_attach_dev(struct iommu_domain *domain, struct device *dev,
			     struct iommu_domain *old)
{
	/* [한국어] 각 단계의 결과. */
	int ret = 0;
	/* [한국어] 보낼 ATTACH 요청(스택에 조립한다). */
	struct virtio_iommu_req_attach req;
	/* [한국어] 대상 엔드포인트. */
	struct viommu_endpoint *vdev = dev_iommu_priv_get(dev);
	/* [한국어] 붙일 도메인. */
	struct viommu_domain *vdomain = to_viommu_domain(domain);

	/* [한국어] 도메인과 디바이스가 서로 다른 virtio-iommu 장치에
	 * 속하면 도메인 ID가 통하지 않는다. */
	if (vdomain->viommu != vdev->viommu)
		return -EINVAL;

	/*
	 * In the virtio-iommu device, when attaching the endpoint to a new
	 * domain, it is detached from the old one and, if as a result the
	 * old domain isn't attached to any endpoint, all mappings are removed
	 * from the old domain and it is freed.
	 *
	 * In the driver the old domain still exists, and its mappings will be
	 * recreated if it gets reattached to an endpoint. Otherwise it will be
	 * freed explicitly.
	 *
	 * vdev->vdomain is protected by group->mutex
	 */
	/* [한국어] 장치는 새 도메인에 붙이는 순간 옛 도메인에서 떼어 낸다.
	 * 그 사실을 요청을 보내기 전에 장부에 반영한다 — 옛 도메인의
	 * 카운트가 0이 되면 장치 쪽에서 그 도메인은 사라진 것이다. */
	if (vdev->vdomain)
		vdev->vdomain->nr_endpoints--;

	/* [한국어] ATTACH 요청을 조립한다. 엔드포인트 ID는
	 * send_attach_req가 ID마다 채워 넣는다. */
	req = (struct virtio_iommu_req_attach) {
		.head.type	= VIRTIO_IOMMU_T_ATTACH,	/* [한국어] 요청 종류를 ATTACH로 지정한다. */
		.domain		= cpu_to_le32(vdomain->id),
	};

	/* [한국어] 이 디바이스의 모든 스트림 ID에 대해 붙인다. */
	ret = viommu_send_attach_req(vdomain->viommu, dev, &req);
	if (ret)	/* [한국어] 붙이기에 실패하면 여기서 접는다. */
		return ret;

	/* [한국어] 이 도메인에 붙는 첫 엔드포인트라면, 장치 쪽 도메인은
	 * 방금 새로 생긴 빈 도메인이다. */
	if (!vdomain->nr_endpoints) {
		/*
		 * This endpoint is the first to be attached to the domain.
		 * Replay existing mappings (e.g. SW MSI).
		 */
		/* [한국어] 그림자 트리에 있던 매핑들을 다시 보낸다.
		 * 소프트웨어 MSI 창처럼 attach 전에 만들어진 매핑이
		 * 여기서 되살아난다. */
		ret = viommu_replay_mappings(vdomain);
		if (ret)	/* [한국어] 되살리기에 실패하면 붙이기 전체를 실패로 본다. */
			return ret;
	}

	/* [한국어] 붙은 엔드포인트 수를 늘린다 — 이제 map이 실제 요청을
	 * 보내기 시작한다. */
	vdomain->nr_endpoints++;
	/* [한국어] 다음 attach에서 옛 도메인을 알아볼 수 있도록 기록한다. */
	vdev->vdomain = vdomain;

	return 0;	/* [한국어] 붙이기와 되살리기가 모두 끝났다. */
}

/*
 * [한국어]
 * viommu_attach_identity_domain - 디바이스를 우회 도메인에 붙인다
 *
 * @domain: 정적 identity 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인.
 * @return: 0 성공, 음수 오류.
 *
 * BYPASS_CONFIG 기능이 있을 때만 쓰이는 경로다. 예약해 둔 전용
 * 도메인 ID에 BYPASS 플래그를 얹어 ATTACH를 보내면, 호스트가
 * 그 엔드포인트의 DMA를 변환 없이 통과시킨다.
 *
 * 페이징 attach와 달리 replay가 없다 — 우회 도메인에는 매핑이라는
 * 개념 자체가 없기 때문이다. 카운트 관리만 같은 방식으로 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev(identity) → [viommu_attach_identity_domain]
 *   → viommu_send_attach_req()
 */
static int viommu_attach_identity_domain(struct iommu_domain *domain,
					 struct device *dev,
					 struct iommu_domain *old)
{
	/* [한국어] 요청 결과. */
	int ret = 0;
	/* [한국어] 보낼 ATTACH 요청. */
	struct virtio_iommu_req_attach req;
	/* [한국어] 대상 엔드포인트. */
	struct viommu_endpoint *vdev = dev_iommu_priv_get(dev);
	/* [한국어] 정적 identity 도메인(카운트 관리용). */
	struct viommu_domain *vdomain = to_viommu_domain(domain);

	/* [한국어] 예약해 둔 우회 전용 ID에 BYPASS 플래그를 얹는다 —
	 * 이 조합이 "변환하지 말고 통과시켜라"는 뜻이다. */
	req = (struct virtio_iommu_req_attach) {
		.head.type	= VIRTIO_IOMMU_T_ATTACH,	/* [한국어] 요청 종류를 ATTACH로 지정한다. */
		.domain		= cpu_to_le32(vdev->viommu->identity_domain_id),
		.flags          = cpu_to_le32(VIRTIO_IOMMU_ATTACH_F_BYPASS),
	};

	/* [한국어] 이 디바이스의 모든 스트림 ID를 우회로 돌린다. */
	ret = viommu_send_attach_req(vdev->viommu, dev, &req);
	if (ret)	/* [한국어] 우회로 돌리기에 실패했다. */
		return ret;

	/* [한국어] 옛 도메인에서 떨어져 나갔음을 장부에 반영한다. */
	if (vdev->vdomain)
		vdev->vdomain->nr_endpoints--;
	/* [한국어] 우회 도메인의 카운트를 올린다. */
	vdomain->nr_endpoints++;
	/* [한국어] 현재 도메인을 기록한다. */
	vdev->vdomain = vdomain;
	return 0;	/* [한국어] 우회 붙이기가 끝났다. */
}

/* [한국어] 시스템 전체가 공유하는 우회 도메인.
 * 앞에서 전방 선언된 그 변수다. viommu 필드가 NULL로 남는 것이
 * 중요한데, domain_free가 그것을 보고 ID 반납을 건너뛴다.
 * 매핑 트리도 쓰이지 않는다 — 우회에는 매핑이 없다. */
static struct viommu_domain viommu_identity_domain = {
	.domain = {	/* [한국어] 코어가 보는 도메인 부분을 정적으로 채운다. */
		.type = IOMMU_DOMAIN_IDENTITY,
		/* [한국어] 코어가 이 도메인을 통과 모드로 인식하게 하는 표시.
		 * 읽는 자: 코어의 도메인 종류 판별 로직. */

		.ops = &(const struct iommu_domain_ops) {
			.attach_dev = viommu_attach_identity_domain,
			/* [한국어] 붙이기 콜백 하나만 있다. map/unmap이 없는
			 * 이유는 우회 도메인에 매핑할 것이 없기 때문이다.
			 * 익명 구조체로 인라인 정의한 것은 이 도메인 말고
			 * 쓸 곳이 없어서다. */
		},
	},
};

/*
 * [한국어]
 * viommu_detach_dev - 디바이스를 현재 도메인에서 떼어 낸다
 *
 * @vdev: 대상 엔드포인트.
 * @return: 없음.
 *
 * release_device 경로에서만 불린다. 일반적인 도메인 전환은
 * attach가 "옛 도메인에서 빼고 새 도메인에 붙이기"를 한꺼번에
 * 처리하므로 이 함수가 필요 없다.
 *
 * WARN_ON으로 감싼 이유: 여기서 실패하면 되돌릴 방법이 없다.
 * 디바이스는 이미 사라지는 중이고, 장치 쪽에 엔드포인트가 남아
 * 있으면 나중에 폴트가 날 수 있다 — 알려는 두어야 한다.
 *
 * 실행 컨텍스트: 디바이스 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   viommu_release_device() → [viommu_detach_dev]
 *   → viommu_send_req_sync()
 */
static void viommu_detach_dev(struct viommu_endpoint *vdev)
{
	/* [한국어] 스트림 ID 순회 인덱스. */
	int i;
	/* [한국어] 보낼 DETACH 요청. */
	struct virtio_iommu_req_detach req;
	/* [한국어] 현재 붙어 있는 도메인. */
	struct viommu_domain *vdomain = vdev->vdomain;
	/* [한국어] 이 디바이스의 스트림 ID들. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(vdev->dev);

	/* [한국어] 어디에도 붙어 있지 않으면 할 일이 없다. */
	if (!vdomain)
		return;

	/* [한국어] DETACH 요청을 조립한다. */
	req = (struct virtio_iommu_req_detach) {
		.head.type	= VIRTIO_IOMMU_T_DETACH,	/* [한국어] 요청 종류를 DETACH로 지정한다. */
		.domain		= cpu_to_le32(vdomain->id),
	};

	/* [한국어] 붙일 때와 마찬가지로 ID마다 한 번씩 보낸다. */
	for (i = 0; i < fwspec->num_ids; i++) {
		req.endpoint = cpu_to_le32(fwspec->ids[i]);
		/* [한국어] 실패해도 되돌릴 수 없다 — 디바이스는 이미
		 * 사라지는 중이다. 흔적만 남긴다. */
		WARN_ON(viommu_send_req_sync(vdev->viommu, &req, sizeof(req)));
	}
	/* [한국어] 장부에서 이 엔드포인트를 뺀다. 0이 되면 장치 쪽
	 * 도메인은 매핑과 함께 사라진다. */
	vdomain->nr_endpoints--;
	/* [한국어] 더 이상 어느 도메인에도 붙어 있지 않다. */
	vdev->vdomain = NULL;
}

/*
 * [한국어]
 * viommu_map_pages - IOVA 범위에 물리 페이지를 매핑한다
 *
 * @domain: 대상 도메인.
 * @iova: 매핑할 IOVA의 시작.
 * @paddr: 물리 주소의 시작.
 * @pgsize: 페이지 크기.
 * @pgcount: 페이지 수.
 * @prot: 요청 권한.
 * @gfp: 할당 플래그.
 * @mapped: 실제 매핑된 바이트 수를 돌려줄 곳.
 * @return: 0 성공, 음수 오류.
 *
 * **한 번의 MAP 요청으로 전체 범위를 덮는다.** 페이지마다 요청을
 * 보내지 않는다 — 프로토콜이 구간 단위 매핑을 지원하기 때문이며,
 * 이것이 반가상화 IOMMU의 큰 이점이다.
 *
 * 두 곳에 기록한다: 그림자 트리와 요청 큐. 순서가 중요한데,
 * 트리에 먼저 넣어야 요청 적재가 실패했을 때 되돌릴 대상이 명확하다.
 *
 * nr_endpoints가 0이면 요청을 보내지 않고 트리에만 기록한다.
 * 장치 쪽에 이 도메인이 아직 존재하지 않기 때문이며, 나중에
 * 첫 엔드포인트가 붙을 때 replay가 이 기록을 보내 준다.
 *
 * 실행 컨텍스트: DMA 매핑 경로. atomic일 수 있다.
 *
 * 호출 체인:
 *   IOMMU 코어 map_pages → [viommu_map_pages]
 *   → viommu_add_mapping() → viommu_add_req()
 */
static int viommu_map_pages(struct iommu_domain *domain, unsigned long iova,
			    phys_addr_t paddr, size_t pgsize, size_t pgcount,
			    int prot, gfp_t gfp, size_t *mapped)
{
	/* [한국어] 각 단계의 결과. */
	int ret;
	/* [한국어] 프로토콜 권한 플래그. */
	u32 flags;
	/* [한국어] 매핑할 전체 크기. */
	size_t size = pgsize * pgcount;
	/* [한국어] 구간의 끝(포함). 프로토콜이 닫힌 구간을 쓴다. */
	u64 end = iova + size - 1;
	/* [한국어] 보낼 MAP 요청. */
	struct virtio_iommu_req_map map;
	/* [한국어] 대상 도메인. */
	struct viommu_domain *vdomain = to_viommu_domain(domain);

	/* [한국어] 커널의 prot 비트를 프로토콜 플래그로 옮긴다.
	 * 셋 다 독립적이라 OR로 합친다. */
	flags = (prot & IOMMU_READ ? VIRTIO_IOMMU_MAP_F_READ : 0) |
		(prot & IOMMU_WRITE ? VIRTIO_IOMMU_MAP_F_WRITE : 0) |
		(prot & IOMMU_MMIO ? VIRTIO_IOMMU_MAP_F_MMIO : 0);

	/* [한국어] 장치가 모르는 플래그를 보내면 요청 전체가 거부된다.
	 * 미리 걸러 내는 편이 낫다. */
	if (flags & ~vdomain->map_flags)
		return -EINVAL;

	/* [한국어] 그림자 트리에 먼저 기록한다 — 이것이 있어야 replay와
	 * iova_to_phys가 동작한다. */
	ret = viommu_add_mapping(vdomain, iova, end, paddr, flags);
	if (ret)	/* [한국어] 그림자 기록에 실패하면 요청도 보내지 않는다. */
		return ret;

	/* [한국어] 붙은 엔드포인트가 있을 때만 실제 요청을 보낸다.
	 * 없으면 장치 쪽에 이 도메인이 없으므로 보낼 곳이 없고,
	 * 나중에 replay가 대신 보내 준다. */
	if (vdomain->nr_endpoints) {
		/* [한국어] 구간 하나를 통째로 매핑하는 요청을 조립한다. */
		map = (struct virtio_iommu_req_map) {
			.head.type	= VIRTIO_IOMMU_T_MAP,	/* [한국어] 요청 종류를 MAP으로 지정한다. */
			.domain		= cpu_to_le32(vdomain->id),
			.virt_start	= cpu_to_le64(iova),
			.phys_start	= cpu_to_le64(paddr),
			.virt_end	= cpu_to_le64(end),
			.flags		= cpu_to_le32(flags),
		};

		/* [한국어] 적재만 하고 보내지는 않는다 — 전송은 나중에
		 * iotlb_sync_map이 한다. */
		ret = viommu_add_req(vdomain->viommu, &map, sizeof(map));
		if (ret) {
			/* [한국어] 요청을 못 넣었으면 트리 기록도 되돌린다.
			 * 그러지 않으면 장치에 없는 매핑이 트리에 남는다. */
			viommu_del_mappings(vdomain, iova, end);
			return ret;	/* [한국어] 적재 실패 이유를 전한다. */
		}
	}
	/* [한국어] 요청 전량이 매핑된 것으로 보고한다. */
	if (mapped)
		*mapped = size;

	return 0;
}

/*
 * [한국어]
 * viommu_unmap_pages - IOVA 범위의 매핑을 해제한다
 *
 * @domain: 대상 도메인.
 * @iova: 해제할 시작 IOVA.
 * @pgsize: 페이지 크기.
 * @pgcount: 페이지 수.
 * @gather: 무효화 모으기(이 드라이버는 쓰지 않는다).
 * @return: 실제로 해제한 바이트 수, 실패하면 0.
 *
 * 그림자 트리를 먼저 정리하고, 거기서 나온 실제 길이로 UNMAP
 * 요청을 만든다. 순서가 map과 반대인 이유: **얼마나 지워지는지를
 * 트리가 알려 주기 때문**이다. 프로토콜이 매핑 쪼개기를 금지하므로
 * 요청 범위와 실제 범위가 다를 수 있다.
 *
 * unmapped < size면 쪼개기를 시도한 것이라 0을 돌려준다.
 * 코어는 그것을 "아무것도 해제되지 않았다"로 이해한다.
 *
 * nr_endpoints가 0이면 요청을 보내지 않는다 — 원본 주석이 밝히듯
 * 장치가 detach 시점에 이미 매핑을 모두 지웠기 때문이다.
 *
 * 실행 컨텍스트: DMA 해제 경로. atomic일 수 있다.
 *
 * 호출 체인:
 *   IOMMU 코어 unmap_pages → [viommu_unmap_pages]
 *   → viommu_del_mappings() → viommu_add_req()
 */
static size_t viommu_unmap_pages(struct iommu_domain *domain, unsigned long iova,
				 size_t pgsize, size_t pgcount,
				 struct iommu_iotlb_gather *gather)
{
	/* [한국어] 요청 적재 결과. */
	int ret = 0;
	/* [한국어] 트리에서 실제로 지워진 바이트 수. */
	size_t unmapped;
	/* [한국어] 보낼 UNMAP 요청. */
	struct virtio_iommu_req_unmap unmap;
	/* [한국어] 대상 도메인. */
	struct viommu_domain *vdomain = to_viommu_domain(domain);
	/* [한국어] 요청된 해제 크기. */
	size_t size = pgsize * pgcount;

	/* [한국어] 그림자 트리를 먼저 정리한다. 지워진 양이 곧 장치에
	 * 보낼 범위이기도 하다. */
	unmapped = viommu_del_mappings(vdomain, iova, iova + size - 1);
	/* [한국어] 요청보다 적게 지워졌다 = 매핑을 쪼개려 한 것이다.
	 * 프로토콜이 허용하지 않으므로 실패로 보고한다. */
	if (unmapped < size)
		return 0;

	/* Device already removed all mappings after detach. */
	/* [한국어] 장치 쪽에 이 도메인이 없으면 보낼 요청도 없다.
	 * 트리 정리만으로 해제가 끝난 것이다. */
	if (!vdomain->nr_endpoints)
		return unmapped;

	/* [한국어] 실제로 지워진 범위로 UNMAP 요청을 조립한다.
	 * 요청 크기가 아니라 unmapped를 쓰는 것이 핵심이다. */
	unmap = (struct virtio_iommu_req_unmap) {
		.head.type	= VIRTIO_IOMMU_T_UNMAP,	/* [한국어] 요청 종류를 UNMAP으로 지정한다. */
		.domain		= cpu_to_le32(vdomain->id),
		.virt_start	= cpu_to_le64(iova),
		.virt_end	= cpu_to_le64(iova + unmapped - 1),
	};

	/* [한국어] 적재만 한다. 실제 전송은 iotlb_sync가 한다. */
	ret = viommu_add_req(vdomain->viommu, &unmap, sizeof(unmap));
	/* [한국어] 적재에 실패했으면 해제 실패로 보고한다. */
	return ret ? 0 : unmapped;
}

/*
 * [한국어]
 * viommu_iova_to_phys - IOVA를 물리 주소로 바꾼다
 *
 * @domain: 대상 도메인.
 * @iova: 조회할 IOVA.
 * @return: 물리 주소, 매핑이 없으면 0.
 *
 * **그림자 트리가 유일한 정보원이다.** 프로토콜에 "이 IOVA가
 * 무엇에 매핑되어 있느냐"를 묻는 요청이 없기 때문이다. 이것이
 * 매핑을 두 벌로 유지하는 또 하나의 이유다.
 *
 * 길이 0짜리 구간으로 검색하면 그 주소를 포함하는 매핑이 나온다.
 * 찾았으면 구간 시작으로부터의 거리를 물리 주소에 더해 답을 만든다.
 *
 * 실행 컨텍스트: 조회 경로. atomic일 수 있어 irqsave를 쓴다.
 *
 * 호출 체인:
 *   IOMMU 코어 iova_to_phys → [viommu_iova_to_phys]
 */
static phys_addr_t viommu_iova_to_phys(struct iommu_domain *domain,
				       dma_addr_t iova)
{
	/* [한국어] 결과 물리 주소. 0이 "매핑 없음"을 뜻한다. */
	u64 paddr = 0;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 찾은 매핑 기록. */
	struct viommu_mapping *mapping;
	/* [한국어] 트리 검색 결과 노드. */
	struct interval_tree_node *node;
	/* [한국어] 대상 도메인. */
	struct viommu_domain *vdomain = to_viommu_domain(domain);

	/* [한국어] 트리를 읽는 동안 변경을 막는다. */
	spin_lock_irqsave(&vdomain->mappings_lock, flags);
	/* [한국어] [iova, iova] 즉 그 한 점을 포함하는 매핑을 찾는다. */
	node = interval_tree_iter_first(&vdomain->mappings, iova, iova);
	if (node) {
		/* [한국어] 노드에서 매핑 기록을 복원한다. */
		mapping = container_of(node, struct viommu_mapping, iova);
		/* [한국어] 구간 시작으로부터의 거리를 물리 주소에 더한다 —
		 * 매핑이 구간 단위라 오프셋 계산이 필요하다. */
		paddr = mapping->paddr + (iova - mapping->iova.start);
	}
	spin_unlock_irqrestore(&vdomain->mappings_lock, flags);	/* [한국어] 트리 읽기가 끝났으니 락을 놓는다. */

	return paddr;	/* [한국어] 찾았으면 물리 주소, 못 찾았으면 0이다. */
}

/*
 * [한국어]
 * viommu_iotlb_sync - 모아 둔 요청들을 실제로 보내고 완료를 기다린다
 *
 * @domain: 대상 도메인.
 * @gather: 코어가 모은 무효화 범위(이 드라이버는 쓰지 않는다).
 * @return: 없음.
 *
 * unmap이 적재해 둔 UNMAP 요청들이 이 시점에 하이퍼바이저로 나간다.
 * gather의 범위를 보지 않는 이유: 요청 자체에 이미 범위가 담겨
 * 있어, 여기서는 "쌓인 것을 다 보내라"는 신호만 필요하다.
 *
 * 실행 컨텍스트: 코어의 무효화 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 iotlb_sync → [viommu_iotlb_sync] → viommu_sync_req()
 */
static void viommu_iotlb_sync(struct iommu_domain *domain,
			      struct iommu_iotlb_gather *gather)
{
	/* [한국어] 대상 도메인. */
	struct viommu_domain *vdomain = to_viommu_domain(domain);

	/* [한국어] 큐에 쌓인 모든 요청을 보내고 완료까지 기다린다. */
	viommu_sync_req(vdomain->viommu);
}

/*
 * [한국어]
 * viommu_iotlb_sync_map - 매핑 요청들을 실제로 보낸다
 *
 * @domain: 대상 도메인.
 * @iova: 새 매핑의 시작(이 드라이버는 쓰지 않는다).
 * @size: 새 매핑의 크기(이 드라이버는 쓰지 않는다).
 * @return: 0 또는 동기화 오류.
 *
 * map_pages가 적재한 MAP 요청들이 여기서 나간다. 이 드라이버에서
 * "매핑이 실제로 유효해지는 순간"이 바로 이 지점이다.
 *
 * nr_endpoints가 0이면 보낼 것이 없다. 원본 주석이 밝히듯,
 * 직접 매핑을 만드는 초기화 단계처럼 도메인이 아직 어디에도
 * 붙지 않은 상태에서도 이 콜백이 불릴 수 있기 때문이다.
 *
 * 실행 컨텍스트: 매핑 직후.
 *
 * 호출 체인:
 *   IOMMU 코어 iotlb_sync_map → [viommu_iotlb_sync_map]
 *   → viommu_sync_req()
 */
static int viommu_iotlb_sync_map(struct iommu_domain *domain,
				 unsigned long iova, size_t size)
{
	/* [한국어] 대상 도메인. */
	struct viommu_domain *vdomain = to_viommu_domain(domain);

	/*
	 * May be called before the viommu is initialized including
	 * while creating direct mapping
	 */
	/* [한국어] 아직 어느 엔드포인트에도 붙지 않은 도메인이면
	 * 큐에 넣은 요청도 없다 — vdomain->viommu가 NULL일 수도 있어
	 * 이 검사가 안전장치 역할도 한다. */
	if (!vdomain->nr_endpoints)
		return 0;
	return viommu_sync_req(vdomain->viommu);	/* [한국어] 쌓인 매핑 요청을 실제로 보내고 완료를 기다린다. */
}

/*
 * [한국어]
 * viommu_flush_iotlb_all - 쌓인 요청을 모두 보낸다(전체 무효화 대응)
 *
 * @domain: 대상 도메인.
 * @return: 없음.
 *
 * 다른 드라이버에서는 "TLB를 통째로 비워라"에 해당하지만, 여기서는
 * 비울 캐시가 게스트 쪽에 없다. 대신 큐에 남은 요청을 모두 보내
 * 장치 쪽 상태를 게스트의 장부와 일치시킨다.
 *
 * 실행 컨텍스트: 코어의 전체 무효화 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 flush_iotlb_all → [viommu_flush_iotlb_all]
 *   → viommu_sync_req()
 */
static void viommu_flush_iotlb_all(struct iommu_domain *domain)
{
	/* [한국어] 대상 도메인. */
	struct viommu_domain *vdomain = to_viommu_domain(domain);

	/*
	 * May be called before the viommu is initialized including
	 * while creating direct mapping
	 */
	/* [한국어] 붙은 엔드포인트가 없으면 장치 쪽에 반영할 것도 없다. */
	if (!vdomain->nr_endpoints)
		return;
	viommu_sync_req(vdomain->viommu);	/* [한국어] 쌓인 요청을 모두 보내 장치 쪽 상태를 장부와 맞춘다. */
}

/*
 * [한국어]
 * viommu_get_resv_regions - 이 디바이스의 예약 영역을 코어에 알린다
 *
 * @dev: 대상 디바이스.
 * @head: 예약 영역을 매달 목록.
 * @return: 없음.
 *
 * PROBE로 받아 둔 목록을 **복사해서** 준다. 코어가 이 목록의
 * 소유권을 가져가 나중에 해제하기 때문에, 드라이버가 계속 들고
 * 있어야 하는 원본을 넘길 수는 없다.
 *
 * 장치가 MSI 창을 알려 주지 않았다면 소프트웨어 MSI 창을 만들어
 * 준다. 이것이 없으면 MSI 도어벨을 매핑할 자리가 정해지지 않아
 * 인터럽트가 동작하지 않는다.
 *
 * 실행 컨텍스트: 디바이스 probe 이후. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 get_resv_regions → [viommu_get_resv_regions]
 *   → iommu_dma_get_resv_regions()
 */
static void viommu_get_resv_regions(struct device *dev, struct list_head *head)
{
	/* [한국어] 원본 순회 커서, 만든 복사본, 그리고 찾은 MSI 창. */
	struct iommu_resv_region *entry, *new_entry, *msi = NULL;
	/* [한국어] PROBE로 받은 예약 목록을 가진 엔드포인트. */
	struct viommu_endpoint *vdev = dev_iommu_priv_get(dev);
	/* [한국어] 소프트웨어 MSI 창에 줄 권한. */
	int prot = IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO;

	/* [한국어] 받아 둔 예약 영역을 하나씩 복사해 넘긴다. */
	list_for_each_entry(entry, &vdev->resv_regions, list) {
		/* [한국어] 장치가 알려 준 MSI 창이 있는지 기억해 둔다. */
		if (entry->type == IOMMU_RESV_MSI)
			msi = entry;

		/* [한국어] 코어가 소유권을 가져가므로 원본이 아니라 사본을 준다. */
		new_entry = kmemdup(entry, sizeof(*entry), GFP_KERNEL);
		if (!new_entry)	/* [한국어] 사본을 만들지 못하면 지금까지 넘긴 것만 남긴다. */
			return;
		list_add_tail(&new_entry->list, head);	/* [한국어] 코어의 목록에 사본을 매단다. */
	}

	/*
	 * If the device didn't register any bypass MSI window, add a
	 * software-mapped region.
	 */
	/* [한국어] 장치가 MSI 창을 알려 주지 않았다 — 게스트가 임의의
	 * 구간을 정해 도어벨을 그리로 매핑해야 한다. */
	if (!msi) {
		msi = iommu_alloc_resv_region(MSI_IOVA_BASE, MSI_IOVA_LENGTH,	/* [한국어] 게스트가 정한 구간을 MSI 도어벨용으로 예약한다. */
					      prot, IOMMU_RESV_SW_MSI,
					      GFP_KERNEL);
		if (!msi)	/* [한국어] 예약 영역을 만들지 못했다. */
			return;

		list_add_tail(&msi->list, head);	/* [한국어] 만든 소프트웨어 MSI 창을 코어에 넘긴다. */
	}

	/* [한국어] DMA 계층이 추가로 요구하는 예약 영역까지 덧붙인다. */
	iommu_dma_get_resv_regions(dev, head);
}

/* [한국어] virtio 버스 타입을 담아 두는 전역.
 * 설정자: viommu_probe()가 처음 붙을 때 "빌려" 온다.
 * 읽는 자: viommu_get_by_fwnode()의 bus_find_device.
 * 왜 이렇게 하는가: 이 드라이버가 virtio 버스 타입 심볼을 직접
 *                   참조하지 않고도 그 버스를 훑을 수 있게 하려는
 *                   실용적인 편법이다. */
static const struct bus_type *virtio_bus_type;

/*
 * [한국어]
 * viommu_match_node - fwnode로 virtio-iommu 장치를 알아본다
 *
 * @dev: 검사할 virtio 디바이스.
 * @data: 찾는 fwnode.
 * @return: 일치하면 참.
 *
 * 부모를 보는 것이 요점이다. virtio 디바이스 자체가 아니라 그것을
 * 실어 나르는 전송 계층 디바이스(예: virtio-pci, virtio-mmio)가
 * 디바이스 트리 노드를 갖기 때문이다.
 *
 * 실행 컨텍스트: 버스 순회 콜백.
 *
 * 호출 체인:
 *   bus_find_device() → [viommu_match_node]
 */
static int viommu_match_node(struct device *dev, const void *data)
{
	/* [한국어] fwnode를 가진 쪽은 전송 계층인 부모 디바이스다. */
	return device_match_fwnode(dev->parent, data);
}

/*
 * [한국어]
 * viommu_get_by_fwnode - fwnode로 담당 virtio-iommu 장치를 찾는다
 *
 * @fwnode: 디바이스 트리 등에서 온 IOMMU 노드.
 * @return: 해당 장치의 상태, 없으면 NULL.
 *
 * put_device를 곧바로 부르는 것이 눈에 띈다. 참조를 유지하지 않는
 * 것인데, 이 IOMMU 장치는 자신을 쓰는 디바이스보다 오래 살도록
 * 상위 계층이 보장하므로 문제가 되지 않는다.
 *
 * 실행 컨텍스트: 디바이스 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   viommu_probe_device() → [viommu_get_by_fwnode] → bus_find_device()
 */
static struct viommu_dev *viommu_get_by_fwnode(struct fwnode_handle *fwnode)
{
	/* [한국어] virtio 버스를 훑어 fwnode가 맞는 디바이스를 찾는다. */
	struct device *dev = bus_find_device(virtio_bus_type, NULL, fwnode,
					     viommu_match_node);

	/* [한국어] 찾기가 올려 준 참조를 곧바로 놓는다 — 이 장치의
	 * 수명은 IOMMU 코어의 등록 관계가 보장한다. */
	put_device(dev);

	/* [한국어] virtio 디바이스의 private에 걸어 둔 드라이버 상태를 꺼낸다. */
	return dev ? dev_to_virtio(dev)->priv : NULL;
}

/*
 * [한국어]
 * viommu_probe_device - 디바이스를 이 IOMMU에 등록한다
 *
 * @dev: 검사할 디바이스.
 * @return: 담당 iommu_device, 담당하지 않으면 ERR_PTR.
 *
 * fwspec이 가리키는 IOMMU 노드로 담당 장치를 찾고, 엔드포인트 상태를
 * 만들어 디바이스에 매단다. PROBE 기능이 있으면 이어서 엔드포인트별
 * 속성까지 물어본다 — 예약 영역과 MSI 창을 그때 알게 된다.
 *
 * 실행 컨텍스트: 디바이스 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 probe_device → [viommu_probe_device]
 *   → viommu_probe_endpoint()
 */
static struct iommu_device *viommu_probe_device(struct device *dev)
{
	/* [한국어] 엔드포인트 probe 결과. */
	int ret;
	/* [한국어] 만들 엔드포인트 상태. */
	struct viommu_endpoint *vdev;
	/* [한국어] 담당 virtio-iommu 장치. */
	struct viommu_dev *viommu = NULL;
	/* [한국어] 어느 IOMMU가 이 디바이스를 담당하는지 알려 주는 명세. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

	/* [한국어] 명세의 노드로 담당 장치를 찾는다. */
	viommu = viommu_get_by_fwnode(fwspec->iommu_fwnode);
	/* [한국어] 우리가 담당하는 디바이스가 아니다. */
	if (!viommu)
		return ERR_PTR(-ENODEV);

	/* [한국어] 엔드포인트 상태를 0으로 초기화해 잡는다. */
	vdev = kzalloc_obj(*vdev);
	if (!vdev)	/* [한국어] 엔드포인트 상태를 잡지 못했다. */
		return ERR_PTR(-ENOMEM);

	/* [한국어] 로그와 fwspec 조회에 쓸 디바이스. */
	vdev->dev = dev;
	/* [한국어] 요청을 보낼 장치. */
	vdev->viommu = viommu;
	/* [한국어] 예약 영역 목록을 빈 상태로 준비한다. */
	INIT_LIST_HEAD(&vdev->resv_regions);
	/* [한국어] 이후 모든 콜백이 이 상태를 꺼내 쓴다. */
	dev_iommu_priv_set(dev, vdev);

	/* [한국어] PROBE 기능이 있을 때만 엔드포인트 속성을 물어본다.
	 * probe_size가 0이면 그 기능이 없다는 뜻이다. */
	if (viommu->probe_size) {
		/* Get additional information for this endpoint */
		ret = viommu_probe_endpoint(viommu, dev);	/* [한국어] 예약 영역과 MSI 창 정보를 장치에 물어본다. */
		if (ret)	/* [한국어] 속성 조회가 실패하면 등록을 접는다. */
			goto err_free_dev;
	}

	/* [한국어] 이 디바이스를 담당하는 IOMMU 인스턴스를 코어에 알린다. */
	return &viommu->iommu;

/* [한국어] 엔드포인트 probe가 실패했다 — 만든 것을 되돌린다. */
err_free_dev:
	/* [한국어] 부분적으로 채워졌을 수 있는 예약 목록을 비운다. */
	iommu_put_resv_regions(dev, &vdev->resv_regions);
	kfree(vdev);	/* [한국어] 엔드포인트 상태를 해제한다. */

	return ERR_PTR(ret);	/* [한국어] 실패 이유를 오류 포인터로 감싸 전한다. */
}

/*
 * [한국어]
 * viommu_release_device - 디바이스를 이 IOMMU에서 걷어낸다
 *
 * @dev: 대상 디바이스.
 * @return: 없음.
 *
 * probe_device의 역순이다. 떼어 내기를 먼저 하는 것이 중요한데,
 * 엔드포인트 상태를 해제한 뒤에는 DETACH 요청을 보낼 정보가
 * 남지 않기 때문이다.
 *
 * 실행 컨텍스트: 디바이스 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 release_device → [viommu_release_device]
 *   → viommu_detach_dev()
 */
static void viommu_release_device(struct device *dev)
{
	/* [한국어] 걷어낼 엔드포인트 상태. */
	struct viommu_endpoint *vdev = dev_iommu_priv_get(dev);

	/* [한국어] 먼저 장치 쪽에서 떼어 낸다 — 상태를 해제한 뒤에는
	 * 요청을 만들 수 없다. */
	viommu_detach_dev(vdev);
	/* [한국어] PROBE로 받아 둔 예약 영역들을 해제한다. */
	iommu_put_resv_regions(dev, &vdev->resv_regions);
	kfree(vdev);	/* [한국어] 엔드포인트 상태를 해제한다. */
}

/*
 * [한국어]
 * viommu_device_group - 이 디바이스가 속할 IOMMU 그룹을 정한다
 *
 * @dev: 대상 디바이스.
 * @return: 그룹.
 *
 * 격리 단위를 정하는 일이다. PCI에서는 ACS 능력과 브리지 구조에
 * 따라 여러 함수가 한 그룹에 묶일 수 있어 전용 함수가 필요하다.
 * 그 밖의 버스에서는 디바이스마다 하나씩 떼어 놓는다.
 *
 * 실행 컨텍스트: 디바이스 probe 이후.
 *
 * 호출 체인:
 *   IOMMU 코어 device_group → [viommu_device_group]
 */
static struct iommu_group *viommu_device_group(struct device *dev)
{
	/* [한국어] PCI는 ACS와 토폴로지를 따져야 해 전용 판별을 쓴다. */
	if (dev_is_pci(dev))
		return pci_device_group(dev);
	else
		/* [한국어] 그 밖에는 디바이스마다 독립 그룹으로 둔다. */
		return generic_device_group(dev);
}

/*
 * [한국어]
 * viommu_of_xlate - 디바이스 트리의 iommus 속성을 스트림 ID로 옮긴다
 *
 * @dev: 대상 디바이스.
 * @args: "iommus = <&viommu ID>"에서 파싱된 인자.
 * @return: 0 성공, 음수 오류.
 *
 * 인자 하나가 곧 엔드포인트 ID다. 이렇게 등록된 ID들이
 * fwspec->ids에 쌓이고, attach/detach가 그 전부에 요청을 보낸다.
 *
 * 실행 컨텍스트: 디바이스 트리 파싱. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 of_xlate → [viommu_of_xlate] → iommu_fwspec_add_ids()
 */
static int viommu_of_xlate(struct device *dev,
			   const struct of_phandle_args *args)
{
	/* [한국어] 인자 하나를 엔드포인트 ID로 등록한다. */
	return iommu_fwspec_add_ids(dev, args->args, 1);
}

/*
 * [한국어]
 * viommu_capable - IOMMU 코어의 능력 질의에 답한다
 *
 * @dev: 질의 대상 디바이스.
 * @cap: 묻는 능력.
 * @return: 지원하면 참.
 *
 * 둘 다 참이다. 캐시 일관성은 호스트가 보장하고, 지연 플러시는
 * 이 드라이버의 기본 동작 방식 그 자체다 — 요청을 모았다가
 * sync 시점에 한 번에 보내므로 오히려 그것이 유리하다.
 *
 * 실행 컨텍스트: 능력 질의.
 *
 * 호출 체인:
 *   IOMMU 코어 capable → [viommu_capable]
 */
static bool viommu_capable(struct device *dev, enum iommu_cap cap)
{
	/* [한국어] 질의된 능력에 따라 답한다. */
	switch (cap) {
	/* [한국어] 변환을 호스트가 하므로 캐시 일관성 문제는 게스트의
	 * 몫이 아니다. */
	case IOMMU_CAP_CACHE_COHERENCY:
		return true;
	/* [한국어] 지연 플러시는 이 드라이버의 기본 모델이다 —
	 * 요청을 모아 보낼수록 virtqueue 왕복이 줄어든다. */
	case IOMMU_CAP_DEFERRED_FLUSH:
		return true;
	/* [한국어] 나머지는 지원하지 않는다. */
	default:
		return false;	/* [한국어] 묻지 않은 능력은 지원하지 않는다고 답한다. */
	}
}

/* [한국어] 이 드라이버가 IOMMU 코어에 제공하는 연산 테이블.
 * identity와 paging 두 종류의 도메인을 모두 만들 수 있고,
 * 매핑 계열은 default_domain_ops에 모여 있다. */
static const struct iommu_ops viommu_ops = {
	.capable		= viommu_capable,
	/* [한국어] 능력 질의. 캐시 일관성과 지연 플러시 둘 다 지원한다고 답한다. */

	.domain_alloc_identity	= viommu_domain_alloc_identity,
	/* [한국어] 통과 도메인 생성. 호스트의 우회 지원 여부에 따라
	 * 정적 도메인을 주거나 1:1로 채운 도메인을 만든다. */

	.domain_alloc_paging	= viommu_domain_alloc_paging,
	/* [한국어] 일반 도메인 생성. 장치의 설정을 그대로 물려받는다. */

	.probe_device		= viommu_probe_device,
	/* [한국어] 디바이스 등록. 엔드포인트 상태를 만들고 속성을 물어본다. */

	.release_device		= viommu_release_device,
	/* [한국어] 디바이스 제거. 떼어 내기와 상태 해제. */

	.device_group		= viommu_device_group,
	/* [한국어] 격리 단위 결정. PCI만 전용 판별을 쓴다. */

	.get_resv_regions	= viommu_get_resv_regions,
	/* [한국어] 예약 영역 통지. 필요하면 소프트웨어 MSI 창을 만들어 준다. */

	.of_xlate		= viommu_of_xlate,
	/* [한국어] 디바이스 트리의 iommus 속성을 엔드포인트 ID로 옮긴다. */

	.owner			= THIS_MODULE,
	/* [한국어] 모듈 참조 계수의 주인. 이 IOMMU를 쓰는 디바이스가
	 * 있는 동안 모듈이 내려가지 않게 한다. */

	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev		= viommu_attach_dev,
		/* [한국어] 붙이기. 옛 도메인 카운트를 줄이고 replay 여부를 판단한다. */

		.map_pages		= viommu_map_pages,
		/* [한국어] 매핑. 그림자 트리 기록 + MAP 요청 적재. */

		.unmap_pages		= viommu_unmap_pages,
		/* [한국어] 해제. 트리 정리 + UNMAP 요청 적재. */

		.iova_to_phys		= viommu_iova_to_phys,
		/* [한국어] 조회. 그림자 트리로만 답한다. */

		.flush_iotlb_all	= viommu_flush_iotlb_all,
		/* [한국어] 전체 무효화 요청 시에도 결국 큐를 비우는 일이다. */

		.iotlb_sync		= viommu_iotlb_sync,
		/* [한국어] 해제 요청들을 실제로 보내는 지점. */

		.iotlb_sync_map		= viommu_iotlb_sync_map,
		/* [한국어] 매핑 요청들을 실제로 보내는 지점 — 매핑이
		 * 유효해지는 순간이 여기다. */

		.free			= viommu_domain_free,
		/* [한국어] 도메인 해제. 그림자 트리와 ID를 되돌린다. */
	}
};

/*
 * [한국어]
 * viommu_init_vqs - 요청 큐와 이벤트 큐를 찾아 연다
 *
 * @viommu: 대상 장치.
 * @return: 0 성공, 음수 오류.
 *
 * 두 큐의 이름과 콜백을 지정해 virtio 계층에 요청한다.
 * 요청 큐에 콜백이 없는 것이 중요한 설계다 — 완료를 인터럽트로
 * 받지 않고 sync에서 직접 회수하기 때문이다. 이벤트 큐만
 * 콜백을 등록해 폴트가 올라오면 즉시 처리한다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   viommu_probe() → [viommu_init_vqs] → virtio_find_vqs()
 */
static int viommu_init_vqs(struct viommu_dev *viommu)
{
	/* [한국어] virtio 계층의 장치 객체. */
	struct virtio_device *vdev = dev_to_virtio(viommu->dev);
	/* [한국어] 큐 두 개의 이름과 콜백. 순서가 곧 vqs 배열의 인덱스다.
	 * 요청 큐에 콜백이 없는 이유: 완료를 인터럽트가 아니라
	 * sync가 직접 회수하는 방식이기 때문이다. */
	struct virtqueue_info vqs_info[] = {
		{ "request" },	/* [한국어] 요청 큐 — 콜백이 없다. 완료를 sync가 직접 회수하기 때문이다. */
		{ "event", viommu_event_handler },
	};

	/* [한국어] 두 큐를 찾아 viommu->vqs에 채워 넣는다. */
	return virtio_find_vqs(vdev, VIOMMU_NR_VQS, viommu->vqs,
			       vqs_info, NULL);
}

/*
 * [한국어]
 * viommu_fill_evtq - 이벤트 큐를 빈 버퍼로 가득 채운다
 *
 * @viommu: 대상 장치.
 * @return: 0 성공, 음수 오류.
 *
 * virtio에서 장치 → 게스트 방향의 메시지는 게스트가 미리 넣어 둔
 * 버퍼에 실려 온다. 그래서 폴트를 받으려면 먼저 자리를 마련해야
 * 한다. 큐의 빈 슬롯 수만큼 버퍼를 만들어 전부 채워 넣는다.
 *
 * devm으로 한 번에 배열을 잡는 점에 주목 — 개별 해제를 신경 쓸
 * 필요가 없고, 각 버퍼의 주소는 그 배열 안의 원소다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트(GFP_KERNEL).
 *
 * 호출 체인:
 *   viommu_probe() → [viommu_fill_evtq] → virtqueue_add_inbuf()
 */
static int viommu_fill_evtq(struct viommu_dev *viommu)
{
	/* [한국어] 순회 인덱스와 적재 결과. */
	int i, ret;
	/* [한국어] 버퍼 하나를 가리킬 산란 목록 항목. */
	struct scatterlist sg[1];
	/* [한국어] 버퍼 배열. */
	struct viommu_event *evts;
	/* [한국어] 이벤트 큐. */
	struct virtqueue *vq = viommu->vqs[VIOMMU_EVENT_VQ];
	/* [한국어] 큐가 담을 수 있는 만큼을 이벤트 수로 삼는다 —
	 * 큐를 가득 채워 두는 것이 폴트를 놓치지 않는 길이다. */
	size_t nr_evts = vq->num_free;

	/* [한국어] 버퍼 전부를 한 덩어리로 잡는다. devm이라 remove 때
	 * 자동으로 해제되고, 소유권은 viommu->evts가 갖는다. */
	viommu->evts = evts = devm_kmalloc_array(viommu->dev, nr_evts,
						 sizeof(*evts), GFP_KERNEL);
	if (!evts)	/* [한국어] 이벤트 버퍼 배열을 잡지 못했다. */
		return -ENOMEM;

	/* [한국어] 배열의 원소 하나하나를 큐에 등록한다. */
	for (i = 0; i < nr_evts; i++) {
		sg_init_one(sg, &evts[i], sizeof(*evts));
		/* [한국어] 마지막 인자가 완료 시 돌려받을 표식이라,
		 * 이벤트 핸들러가 그 주소로 버퍼를 알아본다. */
		ret = virtqueue_add_inbuf(vq, sg, 1, &evts[i], GFP_KERNEL);
		if (ret)	/* [한국어] 하나라도 등록에 실패하면 더 진행하지 않는다. */
			return ret;
	}

	return 0;	/* [한국어] 큐를 이벤트 버퍼로 가득 채웠다. */
}

/*
 * [한국어]
 * viommu_probe - virtio-iommu 장치 하나를 초기화한다
 *
 * @vdev: 붙을 virtio 장치.
 * @return: 0 성공, 음수 오류.
 *
 * 이 드라이버의 모든 설정이 결정되는 곳이다. 순서에 이유가 있다.
 *
 *  1) 필수 기능(VERSION_1, MAP_UNMAP)을 확인한다. 없으면 이 장치는
 *     이 드라이버가 다룰 수 있는 것이 아니다.
 *  2) 큐를 연다. 설정 공간을 읽기 전에 여는 이유는 실패 시
 *     되돌릴 것을 최소화하기 위함이다.
 *  3) 설정 공간에서 필수 항목(페이지 크기)과 선택 항목(주소 범위,
 *     도메인 ID 범위, PROBE 크기)을 읽는다. 선택 항목은
 *     virtio_cread_le_feature가 기능 비트를 함께 확인해 주므로,
 *     기능이 없으면 기본값이 그대로 남는다.
 *  4) 우회를 지원하면 도메인 ID 하나를 그 용도로 떼어 낸다.
 *  5) virtio_device_ready()로 장치를 가동한 **뒤에** 이벤트 큐를
 *     채운다. 순서가 반대면 아직 준비되지 않은 장치에 버퍼를
 *     넣는 셈이 된다.
 *  6) 마지막으로 IOMMU 코어에 등록한다. 이 순간부터 디바이스들의
 *     probe_device가 불리기 시작한다.
 *
 * 실행 컨텍스트: 장치 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   virtio 코어 → [viommu_probe] → viommu_init_vqs()
 *   → viommu_fill_evtq() → iommu_device_register()
 */
static int viommu_probe(struct virtio_device *vdev)
{
	/* [한국어] 전송 계층 디바이스(virtio-pci 등). IOMMU 등록 시
	 * "실제 하드웨어"로 알릴 대상이다. */
	struct device *parent_dev = vdev->dev.parent;
	/* [한국어] 만들 드라이버 상태. */
	struct viommu_dev *viommu = NULL;
	/* [한국어] 로그와 devm 할당의 기준 디바이스. */
	struct device *dev = &vdev->dev;
	/* [한국어] 입력 주소 범위의 기본값 — 기능이 없으면 무제한이다. */
	u64 input_start = 0;
	u64 input_end = -1UL;
	/* [한국어] 각 단계의 결과. */
	int ret;

	/* [한국어] 이 드라이버가 전제하는 최소 기능 둘. 매핑조차 못 하는
	 * 장치라면 붙을 이유가 없다. */
	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1) ||
	    !virtio_has_feature(vdev, VIRTIO_IOMMU_F_MAP_UNMAP))
		return -ENODEV;

	/* [한국어] 장치 수명에 묶인 할당 — remove 때 자동으로 해제된다. */
	viommu = devm_kzalloc(dev, sizeof(*viommu), GFP_KERNEL);
	if (!viommu)	/* [한국어] 드라이버 상태를 잡지 못했다. */
		return -ENOMEM;

	/* Borrow this for easy lookups later */
	/* [한국어] virtio 버스 타입을 전역에 담아 둔다. 나중에
	 * fwnode로 장치를 찾을 때 이 버스를 훑는다. */
	virtio_bus_type = dev->bus;

	/* [한국어] 요청 큐 조작을 직렬화할 락. */
	spin_lock_init(&viommu->request_lock);
	/* [한국어] 도메인 ID 할당기를 초기화한다. */
	ida_init(&viommu->domain_ids);
	/* [한국어] 로그와 할당의 기준 디바이스를 기억한다. */
	viommu->dev = dev;
	/* [한국어] 기능 협상 결과를 확인할 때 쓸 virtio 장치. */
	viommu->vdev = vdev;
	/* [한국어] 진행 중 요청 목록을 빈 상태로 준비한다. */
	INIT_LIST_HEAD(&viommu->requests);

	/* [한국어] 두 개의 virtqueue를 연다. */
	ret = viommu_init_vqs(viommu);
	if (ret)	/* [한국어] 큐를 열지 못하면 아무 요청도 보낼 수 없다. */
		return ret;

	/* [한국어] 지원 페이지 크기 비트맵을 읽는다. 이것은 선택이 아니라
	 * 필수 항목이라 기능 비트 확인 없이 바로 읽는다. */
	virtio_cread_le(vdev, struct virtio_iommu_config, page_size_mask,
			&viommu->pgsize_bitmap);

	/* [한국어] 지원 페이지 크기가 하나도 없다면 매핑을 만들 수 없다 —
	 * 장치가 잘못 구성된 것이다. */
	if (!viommu->pgsize_bitmap) {
		ret = -EINVAL;	/* [한국어] 지원하는 페이지 크기가 하나도 없는 장치다. */
		goto err_free_vqs;	/* [한국어] 열어 둔 큐를 닫고 나간다. */
	}

	/* [한국어] 기본 허용 플래그는 읽기와 쓰기다. MMIO는 아래에서
	 * 기능이 있을 때만 더한다. */
	viommu->map_flags = VIRTIO_IOMMU_MAP_F_READ | VIRTIO_IOMMU_MAP_F_WRITE;
	/* [한국어] 도메인 ID 상한의 기본값 — 사실상 무제한이다. */
	viommu->last_domain = ~0U;

	/* Optional features */
	/* [한국어] 아래 네 묶음은 모두 "기능 비트가 있을 때만 읽는다".
	 * 없으면 변수에 든 기본값이 그대로 남는다. */
	virtio_cread_le_feature(vdev, VIRTIO_IOMMU_F_INPUT_RANGE,
				struct virtio_iommu_config, input_range.start,
				&input_start);

	/* [한국어] 입력 주소 범위의 상한. */
	virtio_cread_le_feature(vdev, VIRTIO_IOMMU_F_INPUT_RANGE,
				struct virtio_iommu_config, input_range.end,
				&input_end);

	/* [한국어] 쓸 수 있는 도메인 ID의 하한. */
	virtio_cread_le_feature(vdev, VIRTIO_IOMMU_F_DOMAIN_RANGE,
				struct virtio_iommu_config, domain_range.start,
				&viommu->first_domain);

	/* [한국어] 쓸 수 있는 도메인 ID의 상한. */
	virtio_cread_le_feature(vdev, VIRTIO_IOMMU_F_DOMAIN_RANGE,
				struct virtio_iommu_config, domain_range.end,
				&viommu->last_domain);

	/* [한국어] PROBE 응답의 속성 영역 크기. 0이면 PROBE를 쓰지 않는다. */
	virtio_cread_le_feature(vdev, VIRTIO_IOMMU_F_PROBE,
				struct virtio_iommu_config, probe_size,
				&viommu->probe_size);

	/* [한국어] 읽어 온 범위를 도메인 기하 정보로 굳힌다.
	 * force_aperture로 코어가 범위 밖 IOVA를 주지 않게 한다. */
	viommu->geometry = (struct iommu_domain_geometry) {
		.aperture_start	= input_start,	/* [한국어] 장치가 알려 준 입력 주소의 하한. */
		.aperture_end	= input_end,
		.force_aperture	= true,
	};

	/* [한국어] MMIO 매핑을 지원하는 장치라면 그 플래그도 허용 집합에
	 * 넣는다 — map_pages가 이 집합으로 요청을 검사한다. */
	if (virtio_has_feature(vdev, VIRTIO_IOMMU_F_MMIO))
		viommu->map_flags |= VIRTIO_IOMMU_MAP_F_MMIO;

	/* Reserve an ID to use as the bypass domain */
	/* [한국어] 우회를 지원하면 도메인 ID 하나를 그 용도로 떼어 낸다.
	 * first_domain을 올려 두면 ida가 그 ID를 다시 주지 않는다. */
	if (virtio_has_feature(viommu->vdev, VIRTIO_IOMMU_F_BYPASS_CONFIG)) {
		viommu->identity_domain_id = viommu->first_domain;	/* [한국어] 우회 전용으로 쓸 도메인 ID를 지목한다. */
		viommu->first_domain++;	/* [한국어] 하한을 하나 올려 그 ID가 다시 할당되지 않게 한다. */
	}

	/* [한국어] 장치를 가동 상태로 만든다. 이 전에는 큐에 버퍼를
	 * 넣어도 장치가 보지 않는다. */
	virtio_device_ready(vdev);

	/* Populate the event queue with buffers */
	/* [한국어] 이제 이벤트 큐를 채운다 — 순서가 반대면 준비되지 않은
	 * 장치에 버퍼를 넘기는 셈이 된다. */
	ret = viommu_fill_evtq(viommu);
	if (ret)	/* [한국어] 이벤트 큐를 채우지 못했다. */
		goto err_free_vqs;

	/* [한국어] sysfs에 IOMMU 인스턴스를 노출한다. 이름은 virtio
	 * 버스에서의 이름을 그대로 쓴다. */
	ret = iommu_device_sysfs_add(&viommu->iommu, dev, NULL, "%s",
				     virtio_bus_name(vdev));
	if (ret)	/* [한국어] sysfs 등록이 실패했다. */
		goto err_free_vqs;

	/* [한국어] 이벤트 핸들러와 fwnode 조회가 이 값으로 드라이버
	 * 상태를 되찾는다. 등록 직전에 채우는 이유는 그때부터
	 * 외부에서 참조되기 시작하기 때문이다. */
	vdev->priv = viommu;

	/* [한국어] 코어에 등록한다. 이 순간부터 디바이스들의
	 * probe_device가 불리기 시작한다. 실제 하드웨어로는
	 * 전송 계층 부모를 알린다. */
	iommu_device_register(&viommu->iommu, &viommu_ops, parent_dev);

	/* [한국어] 입력 주소 범위를 비트 수로 환산해 알린다 —
	 * 사용자가 게스트의 IOVA 공간 크기를 가늠할 수 있게 한다. */
	dev_info(dev, "input address: %u bits\n",
		 order_base_2(viommu->geometry.aperture_end));
	/* [한국어] 지원 페이지 크기 비트맵을 그대로 찍는다. */
	dev_info(dev, "page mask: %#llx\n", viommu->pgsize_bitmap);

	return 0;

/* [한국어] 큐를 연 뒤의 실패 경로 — 큐를 닫고 나간다.
 * viommu 자체는 devm이라 따로 해제하지 않는다. */
err_free_vqs:
	vdev->config->del_vqs(vdev);	/* [한국어] 열어 둔 큐를 닫는다. viommu 자체는 devm이라 자동 해제된다. */

	return ret;	/* [한국어] 실패 이유를 virtio 코어에 전한다. */
}

/*
 * [한국어]
 * viommu_remove - virtio-iommu 장치를 걷어낸다
 *
 * @vdev: 제거할 virtio 장치.
 * @return: 없음.
 *
 * 순서가 중요하다. 코어에서 먼저 빼야 새 요청이 들어오지 않고,
 * 그다음에 장치를 리셋해 큐를 멈춘다. 리셋 전에 등록을 풀지 않으면
 * 이미 죽은 큐에 요청을 보내려는 경로가 남는다.
 *
 * 실행 컨텍스트: 장치 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   virtio 코어 → [viommu_remove]
 */
static void viommu_remove(struct virtio_device *vdev)
{
	/* [한국어] 드라이버 상태를 되찾는다. */
	struct viommu_dev *viommu = vdev->priv;

	/* [한국어] sysfs 항목을 지운다. */
	iommu_device_sysfs_remove(&viommu->iommu);
	/* [한국어] 코어에서 뺀다 — 이후로는 새 콜백이 오지 않는다. */
	iommu_device_unregister(&viommu->iommu);

	/* Stop all virtqueues */
	/* [한국어] 장치를 리셋해 진행 중인 모든 큐 활동을 멈춘다.
	 * 등록을 먼저 푼 뒤라 새 요청이 들어올 일이 없다. */
	virtio_reset_device(vdev);
	/* [한국어] 큐 자원을 반납한다. */
	vdev->config->del_vqs(vdev);

	dev_info(&vdev->dev, "device removed\n");	/* [한국어] 제거가 끝났음을 알린다. */
}

/*
 * [한국어]
 * viommu_config_changed - 장치가 설정 변경을 알려 왔다
 *
 * @vdev: 알림을 보낸 장치.
 * @return: 없음.
 *
 * 이 드라이버는 설정 공간을 probe 때 한 번만 읽고 그것을 전제로
 * 도메인을 만든다. 중간에 바뀌면 이미 만들어진 도메인들과 어긋나므로
 * 대응할 방법이 없다 — 경고만 남긴다.
 *
 * 실행 컨텍스트: virtio 설정 변경 인터럽트.
 *
 * 호출 체인:
 *   virtio 코어 → [viommu_config_changed]
 */
static void viommu_config_changed(struct virtio_device *vdev)
{
	/* [한국어] 대응할 수단이 없으므로 알리기만 한다. */
	dev_warn(&vdev->dev, "config changed\n");
}

/* [한국어] 이 드라이버가 협상하겠다고 알리는 기능 목록.
 * virtio 코어가 이 목록과 장치가 제공하는 기능의 교집합을 확정한다.
 * 여기 없는 기능은 장치가 제공해도 쓰이지 않는다. */
static unsigned int features[] = {
	VIRTIO_IOMMU_F_MAP_UNMAP,	/* [한국어] 매핑과 해제 — 이 드라이버가 전제하는 필수 기능이다. */
	VIRTIO_IOMMU_F_INPUT_RANGE,	/* [한국어] 입력 주소 범위를 설정 공간으로 알려 준다. 없으면 무제한으로 가정한다. */
	VIRTIO_IOMMU_F_DOMAIN_RANGE,	/* [한국어] 쓸 수 있는 도메인 ID의 범위를 알려 준다. */
	VIRTIO_IOMMU_F_PROBE,	/* [한국어] 엔드포인트별 속성(예약 영역, MSI 창)을 물어볼 수 있다. */
	VIRTIO_IOMMU_F_MMIO,	/* [한국어] MMIO 속성의 매핑을 지원한다. */
	VIRTIO_IOMMU_F_BYPASS_CONFIG,	/* [한국어] 우회 도메인을 지원한다 — 이것이 있어야 통과 모드가 싸진다. */
};

/* [한국어] 이 드라이버가 붙을 장치 종류.
 * VIRTIO_ID_IOMMU 하나이고 벤더는 가리지 않는다. */
static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_IOMMU, VIRTIO_DEV_ANY_ID },	/* [한국어] IOMMU 종류의 virtio 장치라면 벤더를 가리지 않고 붙는다. */
	{ 0 },
};
/* [한국어] 위 표를 모듈 별칭으로 내보내 자동 로딩이 되게 한다. */
MODULE_DEVICE_TABLE(virtio, id_table);

/* [한국어] virtio 코어에 등록할 드라이버 서술자.
 * 위에서 정의한 기능 목록과 콜백들을 묶는다. */
static struct virtio_driver virtio_iommu_drv = {
	.driver.name		= KBUILD_MODNAME,
	/* [한국어] 드라이버 이름. pr_fmt 접두사와 같은 문자열이다. */

	.id_table		= id_table,
	/* [한국어] 붙을 장치 종류 표. */

	.feature_table		= features,
	/* [한국어] 협상할 기능 목록. */

	.feature_table_size	= ARRAY_SIZE(features),
	/* [한국어] 그 목록의 길이. 배열과 따로 관리되지 않도록
	 * ARRAY_SIZE로 자동 계산한다. */

	.probe			= viommu_probe,
	/* [한국어] 장치 초기화 진입점. */

	.remove			= viommu_remove,
	/* [한국어] 장치 제거 진입점. */

	.config_changed		= viommu_config_changed,
	/* [한국어] 설정 변경 알림. 경고만 남긴다. */
};

/* [한국어] 모듈 init/exit 상용구를 대신 생성해 주는 매크로.
 * virtio_register_driver와 unregister를 감싼다. */
module_virtio_driver(virtio_iommu_drv);

/* [한국어] 모듈 정보 — modinfo로 보이는 설명. */
MODULE_DESCRIPTION("Virtio IOMMU driver");
/* [한국어] 원저자. */
MODULE_AUTHOR("Jean-Philippe Brucker <jean-philippe.brucker@arm.com>");
/* [한국어] 라이선스. GPL 심볼을 쓰기 위해 필요하다. */
MODULE_LICENSE("GPL v2");
