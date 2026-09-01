// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Peer 2 Peer DMA support.
 *
 * Copyright (c) 2016-2018, Logan Gunthorpe
 * Copyright (c) 2016-2017, Microsemi Corporation
 * Copyright (c) 2017, Christoph Hellwig
 * Copyright (c) 2018, Eideticom Inc.
 */

/*
 * [한국어 설명] 장치끼리 호스트 메모리를 거치지 않고 직접 DMA 하게 해 주는 계층 (p2pdma.c)
 *
 * === 파일의 역할 ===
 * 보통의 DMA 는 장치가 호스트 메모리를 읽고 쓴다. P2PDMA(Peer-to-Peer DMA)는
 * 그 대신 한 PCI 장치가 다른 PCI 장치의 메모리를 직접 읽고 쓰게 한다.
 * NVMe SSD 의 CMB 에 있는 데이터를 네트워크 카드가 곧바로 가져가는 식이다.
 * 호스트 메모리를 한 번 거치지 않으므로 지연이 줄고 메모리 대역폭이 절약된다.
 *
 * 이 파일이 하는 일은 셋이다.
 *
 *   1) 제공자(provider) 등록 - pci_p2pdma_add_resource() 가 어떤 장치의 BAR
 *      일부를 "다른 장치가 쓸 수 있는 메모리" 로 등록한다. 그 구간을
 *      genalloc(범용 할당자) 풀로 감싸 조각내어 나눠 줄 수 있게 만든다.
 *
 *   2) 경로 판정 - pci_p2pdma_map_type() 이 "이 두 장치 사이에 직접 경로가
 *      성립하는가" 를 판정한다. 이것이 이 파일에서 가장 어려운 부분이며,
 *      아래 별도 항목으로 설명한다.
 *
 *   3) 할당 - pci_alloc_p2pmem() / pci_free_p2pmem() 이 등록된 풀에서
 *      메모리를 떼어 주고 돌려받는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록:  NVMe 등 provider 드라이버
 *          -> [이 파일] pci_p2pdma_add_resource()
 *             -> devm_memremap_pages() 로 그 BAR 구간에 struct page 를 만든다
 *             -> gen_pool 로 감싸 할당 가능하게 한다
 *          -> pci_p2pmem_publish() 로 "남들이 써도 된다" 고 표시
 *
 * 사용:  소비자 드라이버(또는 같은 장치 자신)
 *          -> [이 파일] pci_alloc_p2pmem() 으로 한 조각을 얻고
 *          -> pci_p2pmem_virt_to_bus() 로 그 조각의 PCI 버스 주소를 구해
 *          -> 장치의 DMA 엔진에 그 주소를 넘긴다
 *
 * 매핑:  dma_map_sg 계열
 *          -> [이 파일] pci_p2pdma_map_type() 으로 경로를 판정하고
 *             BUS_ADDR 이면 IOMMU 를 거치지 않는 직행 주소를 쓴다
 *
 * 실행 컨텍스트: 등록과 해제는 프로세스 컨텍스트(메모리 할당과 devm 등록).
 * 할당(pci_alloc_p2pmem)은 gen_pool 이 내부 락만 쓰므로 더 가볍지만,
 * 역시 프로세스 컨텍스트에서 쓰는 것을 전제로 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽(이 파일의 함수를 부르는 쪽):
 *   drivers/nvme/host/pci.c — CMB 를 P2P 풀로 등록하고 SQ 를 거기에 잡는다.
 *     이 트리에서 확인된 가장 실질적인 소비자다(아래 NVMe 절 참조).
 *   drivers/vfio/pci/vfio_pci_core.c:2103 — pcim_p2pdma_init().
 *   drivers/vfio/pci/vfio_pci_dmabuf.c:181 과
 *   drivers/vfio/pci/nvgrace-gpu/main.c:916 — pcim_p2pdma_provider().
 *     VFIO 는 장치 BAR 를 DMABUF 로 내보내 다른 장치가 직접 읽고 쓰게 한다.
 *   block/blk-mq-dma.c:673 — pci_p2pdma_state()(헤더 인라인)를 거쳐
 *     이 파일의 __pci_p2pdma_update_state() 에 닿는다.
 * 아래쪽: mm/memremap.c 의 devm_memremap_pages()(BAR 구간에 struct page 를
 *   만들어 커널 메모리처럼 다룰 수 있게 한다), lib/genalloc.c 의 gen_pool
 *   (조각 할당), lib/percpu-refcount.c(풀 수명 관리).
 *   ACS 설정은 이 파일이 직접 config 를 읽어 확인한다 — pdev->acs_cap 에
 *   캐시된 오프셋만 빌려 쓰고(그 캐시는 열거 때 drivers/pci/pci.c 의
 *   pci_acs_init() 이 채운다) 조회 자체를 남에게 맡기지 않는다.
 *   버스 주소 변환은 pci_bus_address() 로 한다. 그 함수의 정의는
 *   include/linux/pci.h 에 있어 이 스파스 체크아웃에서는 확인하지 못했다.
 *   개념상 짝이 되는 것은 drivers/pci/host-bridge.c 의
 *   pcibios_resource_to_bus() 계열이다.
 * 옆쪽: dma-mapping 계층. P2PDMA 를 쓰는 DMA 요청은 결국 그쪽을 지나며,
 *   pci_p2pdma_map_type() 의 판정 결과에 따라 처리가 갈린다.
 * 공유 상태: struct pci_dev 의 p2pdma 포인터(RCU 로 보호되는
 *   struct pci_p2pdma). 그 안에 gen_pool 과 published 플래그가 있다.
 *   struct pci_dev 의 선언은 include/linux/pci.h 에 있고 이 트리에 없어,
 *   p2pdma/acs_cap 두 필드의 존재는 이 파일의 사용례로만 확인했다.
 *
 * === 주요 함수/구조체 요약 ===
 * pcim_p2pdma_init()          : 장치에 struct pci_p2pdma 를 하나 붙이고,
 *                               MMIO BAR 마다 provider 서술자를 채운다.
 *                               풀은 아직 만들지 않는다.
 * pcim_p2pdma_provider()      : BAR 번호로 그 provider 서술자를 얻는다.
 *                               VFIO 가 DMABUF 로 BAR 를 내보낼 때 쓴다.
 * pci_p2pdma_add_resource()   : BAR 의 한 구간을 P2P 메모리 풀로 등록한다.
 *                               devm_memremap_pages() 로 struct page 를 만들고
 *                               gen_pool 로 감싸는 것이 핵심이다.
 * pci_p2pdma_setup_pool()     : gen_pool 과 sysfs 의 p2pmem 그룹을 만든다.
 * pci_p2pmem_publish()        : 그 풀을 다른 장치에게 공개할지 표시한다.
 *                               공개하지 않으면 자기 자신만 쓴다.
 * pci_alloc_p2pmem()          : 풀에서 한 조각을 떼어 커널 가상 주소로 준다.
 * pci_free_p2pmem()           : 돌려준다.
 * pci_p2pmem_virt_to_bus()    : 그 가상 주소에 대응하는 PCI 버스 주소.
 *                               장치의 DMA 엔진에 넣을 값이다.
 * pci_p2pmem_alloc_sgl()      : 한 조각을 scatterlist 엔트리 하나로 감싸 준다.
 * pci_p2pmem_free_sgl()       : 그 반대.
 * pci_p2pdma_map_type()       : provider 와 대상 장치 사이의 매핑 종류를
 *                               돌려준다. 캐시(xarray)에 있으면 그것을 쓰고,
 *                               없으면 calc_map_type_and_dist() 로 계산한다.
 * __pci_p2pdma_update_state() : 반복자가 페이지를 하나 볼 때마다 provider 가
 *                               바뀌었는지 확인하고 매핑 종류를 갱신한다.
 * calc_map_type_and_dist()    : 실제 판정 로직. 두 장치의 공통 상위를 찾고,
 *                               그 경로가 스위치인지 호스트 브리지인지,
 *                               ACS 가 어떻게 설정됐는지를 본다.
 * pci_p2pdma_distance_many()  : 여러 소비자 각각에 대해 provider 까지의
 *                               거리를 재어 그 합을 돌려준다(가장 나쁜 값이
 *                               아니라 총합이다). 하나라도 경로가 성립하지
 *                               않으면 -1.
 * pci_p2pmem_find_many()      : 공개된 provider 중 그 소비자 목록에 가장
 *                               가까운 것을 고른다. 동점이면 무작위.
 * pci_p2pdma_enable_store() / pci_p2pdma_enable_show()
 *                             : "P2PDMA 를 쓸까, 쓴다면 어느 장치를" 을
 *                               문자열로 주고받는 sysfs/configfs 헬퍼.
 * p2pmem_alloc_mmap()         : 사용자 공간이 p2pmem/allocate 를 mmap 하면
 *                               풀에서 떼어 그 페이지들을 매핑해 준다.
 *
 * struct pci_p2pdma           : provider 장치에 매달리는 상태. gen_pool,
 *                               published 플래그, 매핑 종류 캐시(xarray),
 *                               BAR 별 provider 서술자를 갖는다. RCU 로
 *                               보호된다.
 * struct pci_p2pdma_pagemap   : ZONE_DEVICE 페이지맵 하나. 그 페이지들이
 *                               어느 provider 에서 왔는지를 되짚는 고리다.
 * struct p2pdma_provider      : BAR 하나를 가리키는 서술자(owner 와
 *                               bus_offset). 정의는 include/linux/pci-p2pdma.h
 *                               에 있고 이 트리에 없다 — 두 필드의 존재는
 *                               이 파일의 사용례로만 확인했다.
 * pci_p2pdma_whitelist[]      : Root Complex 를 거치는 P2P 를 허용할 호스트
 *                               브리지 목록.
 *
 * === 경로 판정이 왜 어려운가 ===
 * 두 장치가 같은 PCIe 스위치 아래 있으면 스위치가 트랜잭션을 곧바로 옆으로
 * 넘길 수 있어 직접 경로가 성립한다. 하지만 다음 세 가지가 걸림돌이다.
 *
 *   - Root Complex 를 거쳐야 하는 경우: 많은 Root Complex 가 P2P 트랜잭션을
 *     제대로 전달하지 못한다. 그래서 이 파일은 검증된 호스트 브리지 목록
 *     (화이트리스트)을 들고 있고, 목록에 없으면 직접 경로를 허용하지 않는다.
 *   - ACS(Access Control Services) redirect: 격리를 위해 스위치가 모든
 *     트랜잭션을 위로 올려보내도록 설정돼 있으면, 옆으로 가는 지름길이
 *     막힌다. 그래서 P2PDMA 를 쓰려면 경로상의 ACS 재지향을 꺼야 하고,
 *     그것은 IOMMU 격리를 약화시킨다 — 성능과 보안의 맞교환이다.
 *   - IOMMU: 켜져 있으면 장치가 내는 주소가 IOVA 라 상대 장치의 실제
 *     BAR 주소와 다르다. 그래서 매핑 종류(BUS_ADDR / THRU_HOST_BRIDGE)를
 *     구분해 각각 다르게 처리한다.
 *
 * === NVMe 드라이버가 실제로 쓰는 것 (drivers/nvme/ 전수 확인) ===
 * NVMe 는 이 파일의 함수를 다섯 개 직접 부른다. drivers/pci 에서
 * 이만큼 직접 얽힌 파일은 드물다. CMB(Controller Memory Buffer)가
 * P2PDMA 로 노출되기 때문이다.
 *
 *   nvme_map_cmb()                      [drivers/nvme/host/pci.c:3171]
 *     CMBSZ/CMBLOC 레지스터로 CMB 의 크기와 위치(어느 BAR 의 어느 오프셋)를
 *     읽은 뒤:
 *       -> pci_p2pdma_add_resource(pdev, bar, size, offset)  [같은 파일 :3226]
 *          그 BAR 구간을 P2PDMA 풀로 등록한다. 실패하면 CMBMSC 를 0 으로
 *          되돌려 CMB 자체를 포기한다.
 *       -> pci_p2pmem_publish(pdev, true)                    [같은 파일 :3238]
 *          다른 장치도 이 CMB 를 쓸 수 있다고 공개한다.
 *
 *   nvme_alloc_sq_cmds()   [drivers/nvme/host/pci.c:2736, 큐 하나마다]
 *       -> pci_alloc_p2pmem(pdev, SQ_SIZE(nvmeq))          [같은 파일 :2742]
 *          Submission Queue 링을 호스트 메모리가 아니라 CMB 에 잡는다.
 *       -> pci_p2pmem_virt_to_bus(pdev, nvmeq->sq_cmds)    [같은 파일 :2744]
 *          그 주소를 Create SQ 명령에 넣을 PCI 버스 주소로 바꾼다.
 *       -> 실패하면 pci_free_p2pmem()                      [같은 파일 :2751]
 *          으로 되돌리고 dma_alloc_coherent()(호스트 메모리)로 폴백한다.
 *
 *   nvme_free_queue()  -> pci_free_p2pmem()                [같은 파일 :2615]
 *
 *   (기존 주석은 이 함수를 nvme_alloc_sq() 로 적었으나 그런 이름은
 *    drivers/nvme 에 없다. 실제 이름은 nvme_alloc_sq_cmds() 다.)
 *
 * SQ 를 CMB 에 두면 무엇이 좋은가. 보통은 호스트가 SQ 엔트리를 호스트
 * 메모리에 쓰고, 도어벨을 두드리면 컨트롤러가 그것을 DMA 로 읽어 간다.
 * SQ 가 CMB(컨트롤러 안)에 있으면 호스트가 직접 써 넣으므로 그 읽기 DMA 가
 * 통째로 사라진다. use_cmb_sqes 모듈 파라미터로 켜고 끌 수 있다.
 *
 * 그리고 NVMe 는 enum pci_p2pdma_map_type 을 직접 쓴다
 * (drivers/nvme/host/pci.c:1242 와 :1286). I/O 버퍼가 P2P 메모리인지
 * 판정해 unmap 방식을 가르는 데 쓰며, 값은 PCI_P2PDMA_MAP_BUS_ADDR /
 * PCI_P2PDMA_MAP_THRU_HOST_BRIDGE / PCI_P2PDMA_MAP_NONE 셋이다.
 * 그 판정에 쓰는 is_pci_p2pdma_page() 도 같은 파일 :1597 과 :1777 에서 부른다.
 *
 * 다만 NVMe 가 부르는 것은 이 파일의 함수가 아니라 include/linux/pci-p2pdma.h
 * 의 인라인들이다(그 헤더는 이 스파스 체크아웃에 없다). 이 파일의 코드에
 * 닿는 경로는 블록 계층을 거친다 — block/blk-mq-dma.c:673 의
 * pci_p2pdma_state() 가 그 인라인이고, 그것이 이 파일의
 * __pci_p2pdma_update_state() 를 부른다.
 *
 * (기존 주석은 호출 경로로 "nvme_probe -> nvme_setup_pci_p2pdma" 를
 *  적었으나 nvme_setup_pci_p2pdma 라는 함수는 drivers/nvme/ 에 없다.
 *  실제 진입점은 nvme_map_cmb() 다. 또 "nvme_setup_prps/sgl 이
 *  pci_p2pdma_map_type 을 부른다", "dma_pci_p2pdma_supported 가
 *  pci_p2pdma_distance_many 를 부른다" 고 적었으나 그 함수 이름들도
 *  NVMe 쪽에 없다. 위 검증 결과로 대체했다.)
 */

/* [한국어] 이 파일의 모든 pr_ 계열 printk 앞에 "pci-p2pdma: " 를 붙인다.
 * dev_ 계열은 장치 이름이 이미 붙으므로 이 접두사와 무관하다 */
#define pr_fmt(fmt) "pci-p2pdma: " fmt
/* [한국어] iscntrl() — pci_p2pdma_enable_store() 가 "0" 뒤에 개행뿐인지 확인할 때 쓴다.
 * 그 한 글자 검사가 존재하지 않는 BDF 를 불리언으로 오해하는 것을 막는다 */
#include <linux/ctype.h>
/* [한국어] DMA 매핑 코어의 내부 인터페이스. 이 파일이 dma-mapping 계층과 매핑 종류를
 * 주고받는 계약이 여기서 온다 */
#include <linux/dma-map-ops.h>
/* [한국어] 이 파일이 구현하는 API 의 선언부. enum pci_p2pdma_map_type,
 * struct p2pdma_provider, struct pci_p2pdma_map_state, 그리고
 * __pci_p2pdma_update_state 를 감싸는 pci_p2pdma_state() 인라인이 있다.
 * (이 스파스 체크아웃에는 include/linux/pci-p2pdma.h 가 없어 내용을 직접
 * 확인하지는 못했다.) */
#include <linux/pci-p2pdma.h>
/* [한국어] EXPORT_SYMBOL_GPL — 이 파일의 API 대부분을 모듈에 공개한다 */
#include <linux/module.h>
/* [한국어] kmalloc/kfree/devm_kzalloc — 상태 구조체와 scatterlist 를 잡는다 */
#include <linux/slab.h>
/* [한국어] gen_pool 전체. BAR 구간을 조각내어 나눠 주는 범용 할당자이며,
 * 이 파일의 할당 정책이 통째로 여기에 얹혀 있다 */
#include <linux/genalloc.h>
/* [한국어] devm_memremap_pages / dev_pagemap / MEMORY_DEVICE_PCI_P2PDMA —
 * 장치 BAR 위에 struct page 를 만들어 커널이 평범한 메모리처럼 다루게 하는
 * 핵심 기능이 여기서 온다. 이 헤더 없이는 P2PDMA 가 성립하지 않는다 */
#include <linux/memremap.h>
/* [한국어] percpu_ref — 페이지맵의 수명을 세는 참조 계수. CPU 마다 카운터를 두어
 * 빈번한 get/put 이 캐시 라인을 다투지 않게 한다 */
#include <linux/percpu-refcount.h>
/* [한국어] get_random_u32_below() — pci_p2pmem_find_many() 가 동점 후보 중 하나를
 * 무작위로 고를 때 쓴다. 항상 첫 번째를 고르면 그 장치만 고갈되기 때문이다 */
#include <linux/random.h>
/* [한국어] seq_buf — ACS redirect 가 켜진 브리지 이름을 안전하게 이어 붙인다.
 * 고정 버퍼에 넘치지 않게 쓰는 것을 보장한다 */
#include <linux/seq_buf.h>
/* [한국어] xarray — 매핑 종류 캐시. (도메인, BDF) 키에 판정 결과를 저장해
 * 비싼 트리 순회를 되풀이하지 않게 한다 */
#include <linux/xarray.h>

/* [한국어] provider 장치 하나에 매달리는 P2PDMA 상태 전부.
 * pcim_p2pdma_init() 이 devm 으로 잡아 pdev->p2pdma 에 걸고,
 * pci_p2pdma_release() 가 포인터를 NULL 로 만든 뒤 devm 이 반납한다.
 * 
 * 동기화: 포인터 자체가 RCU 로 보호된다. 읽는 쪽은 rcu_read_lock() 구간
 * 안에서 rcu_dereference() 로 접근하고, 해제하는 쪽은 NULL 대입 후
 * synchronize_rcu() 로 기다린다 */
struct pci_p2pdma {
	/* [한국어] BAR 구간을 조각내어 나눠 주는 범용 할당자.
	 * 설정자: pci_p2pdma_setup_pool() 이 만들고, pci_p2pdma_add_resource() 가
	 *   gen_pool_add_owner() 로 구간을 채운다.
	 * 읽는 자: pci_alloc_p2pmem()/pci_free_p2pmem()/p2pmem_alloc_mmap()/
	 *   p2pdma_folio_free() 와 sysfs 의 size/available.
	 * 값 범위: NULL 이면 아직 메모리를 등록하지 않은 상태(pcim_p2pdma_init 만
	 *   된 VFIO 같은 경우). 그래서 읽는 쪽마다 NULL 검사가 붙어 있다.
	 * 동기화: gen_pool 이 자체 스핀락을 갖는다 */
	struct gen_pool *pool;
	/* [한국어] 이 메모리를 다른 장치가 써도 되는가.
	 * 설정자: pci_p2pmem_publish().
	 * 읽는 자: pci_has_p2pmem() 과 sysfs 의 published.
	 * 값 범위: false 면 등록한 드라이버 전용이다. 그래도 그 드라이버 자신은
	 *   pci_alloc_p2pmem() 으로 얼마든지 쓸 수 있다 — 그 함수는 이 값을 보지 않는다.
	 * 동기화: bool 대입 하나뿐이라 락이 없다. 잘못 읽어도 후보 선정이 한 번
	 *   어긋날 뿐이고, 실제 사용 가능 여부는 경로 판정이 다시 거른다 */
	bool p2pmem_published;
	/* [한국어] 소비자 장치별 매핑 종류 캐시. 키는 map_types_idx()(도메인<<16 | BDF),
	 * 값은 xa_mk_value() 로 감싼 enum pci_p2pdma_map_type.
	 * 설정자: calc_map_type_and_dist() 의 done 라벨(GFP_ATOMIC).
	 * 읽는 자: pci_p2pdma_map_type().
	 * 값 범위: 없는 키를 읽으면 0 이 나오고 그것이 PCI_P2PDMA_MAP_UNKNOWN 이 되어
	 *   "아직 계산하지 않았다" 를 뜻한다. 이 관례 덕분에 별도 존재 표시가 필요 없다.
	 * 동기화: xarray 자체의 RCU + 스핀락. 저장이 실패해도(GFP_ATOMIC 이라
	 *   가능하다) 다음에 다시 계산할 뿐이라 정확성에는 영향이 없다 */
	struct xarray map_types;
	/* [한국어] 표준 BAR 여섯 개 각각의 provider 서술자. 배열로 미리 잡아 두어
	 * BAR 번호가 곧 인덱스가 된다.
	 * 설정자: pcim_p2pdma_init() 이 MMIO BAR 에 대해서만 owner 와 bus_offset 을
	 *   채운다. I/O 포트 BAR 자리는 0 으로 남는다.
	 * 읽는 자: pcim_p2pdma_provider() 가 BAR 번호로 꺼내 주고,
	 *   pci_p2pdma_add_resource() 가 페이지맵에 걸어 둔다.
	 * 동기화: 초기화 뒤 읽기 전용이라 별도 보호가 없다 */
	struct p2pdma_provider mem[PCI_STD_NUM_BARS];
};

/* [한국어] devm_memremap_pages() 에 넘길 dev_pagemap 을 감싼 구조체.
 * 왜 감싸는가: mm 계층은 dev_pagemap 만 알고 그 페이지가 어느 BAR 에서
 * 왔는지는 모른다. 이 파일이 그 정보를 덧붙여 두고 container_of 로
 * 되짚어 올라간다(to_p2p_pgmap).
 * 
 * 수명: pci_p2pdma_add_resource() 가 devm 으로 잡고, 장치가 사라질 때
 * devm 이 반납한다. BAR 를 여러 개 등록하면 이 구조체도 여러 개가 된다 */
struct pci_p2pdma_pagemap {
	/* [한국어] 커널 메모리 계층이 요구하는 페이지맵 서술자. 반드시 첫 멤버여야
	 * container_of 되짚기가 성립한다.
	 * 설정자: pci_p2pdma_add_resource() 가 range/nr_range/type/ops 를 채운다.
	 * 읽는 자: mm 계층(페이지 폴트와 해제 경로)과 devm_memremap_pages().
	 * 값 범위: type 은 MEMORY_DEVICE_PCI_P2PDMA 고정. 그 값 덕분에
	 *   is_pci_p2pdma_page() 가 이 페이지들을 알아본다.
	 * 동기화: pgmap->ref(percpu_ref)가 이 구조체의 수명을 지킨다 */
	struct dev_pagemap pgmap;
	/* [한국어] 이 페이지맵이 덮고 있는 BAR 의 provider 서술자.
	 * 설정자: pci_p2pdma_add_resource().
	 * 읽는 자: p2pdma_folio_free() 가 owner 장치를 되짚을 때,
	 *   __pci_p2pdma_update_state() 가 provider 가 바뀌었는지 볼 때.
	 * 값 범위: pci_p2pdma 의 mem[] 원소를 가리키므로 그 구조체보다 오래 살 수 없다.
	 * 동기화: 초기화 뒤 읽기 전용 */
	struct p2pdma_provider *mem;
};

/* [한국어]
 * to_p2p_pgmap - dev_pagemap 포인터에서 이 파일의 래퍼 구조체를 되찾는다
 *
 * @pgmap: 커널 메모리 계층이 돌려준 struct dev_pagemap 포인터
 * @return: 그것을 첫 멤버로 품고 있는 struct pci_p2pdma_pagemap
 *
 * ZONE_DEVICE 페이지에서 "이 페이지가 어느 provider 의 BAR 에서 왔는가" 를
 * 알아내는 첫 단계다. mm 계층은 struct dev_pagemap 만 알고 그 바깥은 모르므로,
 * 이 파일이 dev_pagemap 을 첫 멤버로 하는 구조체를 만들어 두고
 * container_of 로 되짚어 올라간다 — 커널 전체에서 쓰는 상속 흉내 관용구다.
 *
 * container_of(포인터, 바깥타입, 멤버이름) 은 멤버 오프셋만큼 포인터를
 * 빼는 매크로라 실행 비용이 사실상 0 이다.
 *
 * 실행 컨텍스트: 제한 없음. 순수 포인터 산술이다.
 *
 * 호출 체인:
 *   p2pdma_folio_free() / __pci_p2pdma_update_state() → [이 함수]
 */
static struct pci_p2pdma_pagemap *to_p2p_pgmap(struct dev_pagemap *pgmap)
{
	return container_of(pgmap, struct pci_p2pdma_pagemap, pgmap);
}

/* [한국어]
 * size_show - sysfs 의 p2pmem/size 를 읽는다
 *
 * @dev: 대상 장치.  @attr: 어느 속성인지(쓰지 않는다).  @buf: 출력 버퍼
 * @return: 버퍼에 쓴 바이트 수
 *
 * 이 장치가 P2P 로 내놓은 메모리의 총 크기를 십진수 한 줄로 찍는다.
 * NVMe 라면 등록된 CMB 구간의 크기가 된다.
 *
 * pdev->p2pdma 는 RCU 로 보호되는 포인터다. pci_p2pdma_release() 가
 * 그것을 NULL 로 만든 뒤 synchronize_rcu() 로 기다리므로, rcu_read_lock()
 * 구간 안에서는 역참조가 안전하다. 풀이 아직 없으면(pcim_p2pdma_init 은
 * 됐지만 add_resource 는 안 된 상태) 0 을 찍는다.
 *
 * 실행 컨텍스트: 사용자가 sysfs 파일을 read 할 때의 프로세스 컨텍스트.
 * rcu_read_lock() 구간이라 그 안에서 잠들면 안 되는데, gen_pool_size() 는
 * 단순 합계 조회라 잠들지 않는다.
 *
 * 호출 체인:
 *   (sysfs read) → [이 함수] → gen_pool_size()
 */
static ssize_t size_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	/* [한국어] RCU 로 보호되는 상태 구조체를 담을 지역 포인터 */
	struct pci_p2pdma *p2pdma;
	/* [한국어] 풀이 없으면 0 을 그대로 찍도록 미리 초기화한다 */
	size_t size = 0;

	rcu_read_lock();
	/* [한국어] RCU 읽기 구간 안에서 안전하게 역참조한다. pci_p2pdma_release() 가
	 * 이 포인터를 NULL 로 만든 뒤 synchronize_rcu() 로 기다리므로,
	 * 이 구간 안에서는 가리키는 대상이 살아 있다 */
	p2pdma = rcu_dereference(pdev->p2pdma);
	/* [한국어] 구조체가 있어도 풀은 아직 없을 수 있다 — pcim_p2pdma_init() 만 하고
	 * pci_p2pdma_add_resource() 는 안 한 상태(VFIO 가 그렇다) */
	if (p2pdma && p2pdma->pool)
		/* [한국어] 풀에 넣어 둔 전체 크기. 등록한 BAR 구간들의 합이다 */
		size = gen_pool_size(p2pdma->pool);
	rcu_read_unlock();

	/* [한국어] sysfs 전용 출력 함수. 페이지 경계를 넘지 않도록 커널이 검사해 준다 */
	return sysfs_emit(buf, "%zd\n", size);
}
static DEVICE_ATTR_RO(size);

/* [한국어]
 * available_show - sysfs 의 p2pmem/available 을 읽는다
 *
 * @dev: 대상 장치.  @attr: 쓰지 않는다.  @buf: 출력 버퍼
 * @return: 버퍼에 쓴 바이트 수
 *
 * size_show() 와 짝이다. 이쪽은 아직 할당되지 않고 남아 있는 크기를 찍는다.
 * 둘을 함께 보면 얼마나 쓰이고 있는지 알 수 있다 — NVMe 라면 SQ 를 CMB 에
 * 몇 개나 더 만들 수 있는지가 여기서 보인다.
 *
 * RCU 보호와 실행 컨텍스트는 size_show() 와 같다.
 *
 * 호출 체인:
 *   (sysfs read) → [이 함수] → gen_pool_avail()
 */
static ssize_t available_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	/* [한국어] 위 size_show 와 같은 지역 포인터 */
	struct pci_p2pdma *p2pdma;
	/* [한국어] 풀이 없으면 0 */
	size_t avail = 0;

	rcu_read_lock();
	/* [한국어] RCU 읽기 구간 안에서 역참조 */
	p2pdma = rcu_dereference(pdev->p2pdma);
	/* [한국어] 구조체와 풀이 모두 있어야 물어볼 수 있다 */
	if (p2pdma && p2pdma->pool)
		/* [한국어] 아직 할당되지 않고 남아 있는 크기. size 와 함께 보면 사용률이 나온다 */
		avail = gen_pool_avail(p2pdma->pool);
	rcu_read_unlock();

	/* [한국어] 십진수 한 줄로 찍는다 */
	return sysfs_emit(buf, "%zd\n", avail);
}
static DEVICE_ATTR_RO(available);

/* [한국어]
 * published_show - sysfs 의 p2pmem/published 를 읽는다
 *
 * @dev: 대상 장치.  @attr: 쓰지 않는다.  @buf: 출력 버퍼
 * @return: 버퍼에 쓴 바이트 수
 *
 * 이 장치의 P2P 메모리를 남들이 써도 되는지(1) 아니면 자기 전용인지(0)를
 * 찍는다. pci_p2pmem_publish() 가 세우는 값이며, 1 이어야
 * pci_p2pmem_find_many() 의 후보에 오른다.
 *
 * 위 두 함수와 달리 pool 존재 여부는 보지 않는다 — 공개 플래그는 풀보다
 * 먼저 세워질 수 있기 때문이다.
 *
 * 호출 체인:
 *   (sysfs read) → [이 함수]
 */
static ssize_t published_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	/* [한국어] 위 두 함수와 같은 지역 포인터 */
	struct pci_p2pdma *p2pdma;
	/* [한국어] 구조체가 없으면 공개되지 않은 것으로 본다 */
	bool published = false;

	rcu_read_lock();
	/* [한국어] RCU 읽기 구간 안에서 역참조 */
	p2pdma = rcu_dereference(pdev->p2pdma);
	/* [한국어] 여기서는 pool 존재 여부를 보지 않는다 — 공개 플래그는 풀보다 먼저
	 * 세워질 수 있기 때문이다 */
	if (p2pdma)
		/* [한국어] pci_p2pmem_publish() 가 세운 값 */
		published = p2pdma->p2pmem_published;
	rcu_read_unlock();

	/* [한국어] 0 또는 1 한 줄 */
	return sysfs_emit(buf, "%d\n", published);
}
static DEVICE_ATTR_RO(published);

/* [한국어]
 * p2pmem_alloc_mmap - 사용자 공간이 P2P 메모리를 직접 매핑하게 해 준다
 *
 * @filp: sysfs 파일.  @kobj: 대상 장치의 kobject.  @attr: 이 바이너리 속성
 * @vma: 커널이 준비한 가상 메모리 영역. 이 함수가 여기에 페이지를 꽂는다
 * @return: 0 = 성공, -EINVAL/-ENODEV/-ENOMEM = 실패
 *
 * /sys/bus/pci/devices/.../p2pmem/allocate 를 mmap 하면 불린다.
 * 사용자 공간 프로그램이 장치 BAR 메모리를 자기 주소 공간에 직접 얻어
 * 쓰게 하는 통로다.
 *
 * 거절하는 두 경우:
 *   - MAP_PRIVATE(VM_MAYSHARE 가 없는) 매핑. 사본을 만들어 쓰겠다는 뜻인데,
 *     장치 메모리에는 의미가 없고 CoW 처리도 곤란하다.
 *   - vm_pgoff 가 0 이 아닌 매핑. 이 인터페이스는 "새로 할당해 준다" 이지
 *     "이미 있는 것의 어느 지점을 보여 준다" 가 아니므로 오프셋 개념이 없다.
 *
 * 본체의 흐름과 그 안의 참조 계수 다루기가 이 함수의 핵심이다.
 *   1) RCU 락 안에서 gen_pool_alloc_owner() 로 한 덩어리를 떼어 낸다.
 *      owner 로 그 구간을 소유한 percpu_ref(페이지맵의 참조 계수)를 함께 받는다.
 *   2) 원문 영어 주석대로 vm_insert_page() 는 잠들 수 있는데 RCU 락 안에서는
 *      잠들면 안 된다. 그래서 percpu_ref_tryget_live_rcu() 로 참조를 하나
 *      먼저 잡아 두고 rcu_read_unlock() 을 한다. 그 참조가 페이지맵이
 *      사라지지 않게 붙들어 준다.
 *   3) 페이지 단위로 돌며 refcount 를 1 로 세우고 vm_insert_page() 로 꽂는다.
 *      갓 할당한 페이지라 아무도 안 쓰고 있음을 VM_WARN_ON_ONCE_PAGE 로 확인한다.
 *      꽂을 때마다 percpu_ref_get() 으로 참조를 하나씩 더 잡는다 — 사용자
 *      공간이 그 페이지를 놓을 때 p2pdma_folio_free() 가 하나씩 풀 것이다.
 *      put_page() 는 2)에서 세운 초기 1 을 상쇄한다.
 *   4) 마지막에 2)에서 잡아 둔 임시 참조를 percpu_ref_put() 으로 놓는다.
 *
 * 에러 경로가 미묘하다. 루프 도중 vm_insert_page() 가 실패하면 gen_pool 로
 * 되돌리고 refcount 를 0 으로 되돌린 뒤 참조를 놓는다. 원문 주석대로
 * put_page() 를 쓰지 않는 이유는, 그 경로가 p2pdma_folio_free() 를 불러
 * 이미 손으로 되돌린 gen_pool 해제를 한 번 더 하게 되기 때문이다.
 *
 * 실행 컨텍스트: mmap 시스템 호출의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   (mmap) → [이 함수] → gen_pool_alloc_owner() → vm_insert_page()
 */
static int p2pmem_alloc_mmap(struct file *filp, struct kobject *kobj,
		const struct bin_attribute *attr, struct vm_area_struct *vma)
{
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj));
	/* [한국어] 사용자가 요청한 매핑 길이. 아래 루프에서 페이지 단위로 줄여 나간다 */
	size_t len = vma->vm_end - vma->vm_start;
	/* [한국어] RCU 로 보호되는 상태 구조체 */
	struct pci_p2pdma *p2pdma;
	/* [한국어] 떼어 낸 구간의 owner — 페이지맵의 percpu_ref. gen_pool 이 함께 돌려준다 */
	struct percpu_ref *ref;
	/* [한국어] 매핑할 사용자 가상 주소. 페이지 단위로 전진한다 */
	unsigned long vaddr;
	/* [한국어] gen_pool 이 돌려준 커널 가상 주소. 역시 페이지 단위로 전진한다 */
	void *kaddr;
	/* [한국어] 실패 코드 */
	int ret;

	/* prevent private mappings from being established */
	if ((vma->vm_flags & VM_MAYSHARE) != VM_MAYSHARE) {
		pci_info_ratelimited(pdev,
				     "%s: fail, attempted private mapping\n",
				     current->comm);
		return -EINVAL;
	}

	/* [한국어] 이 인터페이스는 "새로 할당해 준다" 이지 "이미 있는 것의 어느 지점을
	 * 보여 준다" 가 아니므로 오프셋 개념이 없다. 0 이 아니면 사용자가
	 * 뭔가 오해한 것이므로 거절한다 */
	if (vma->vm_pgoff) {
		pci_info_ratelimited(pdev,
				     "%s: fail, attempted mapping with non-zero offset\n",
				     current->comm);
		return -EINVAL;
	}

	rcu_read_lock();
	/* [한국어] RCU 읽기 구간 안에서 상태 구조체를 얻는다 */
	p2pdma = rcu_dereference(pdev->p2pdma);
	/* [한국어] 장치가 이미 정리 중이면 매핑해 줄 것이 없다 */
	if (!p2pdma) {
		ret = -ENODEV;
		goto out;
	}

	/* [한국어] 풀에서 요청한 길이만큼 떼어 내면서, 그 구간의 owner(percpu_ref)도 함께 받는다.
	 * (void **)&ref 로 넘기는 것은 gen_pool 이 owner 를 void * 로 다루기 때문이다 */
	kaddr = (void *)gen_pool_alloc_owner(p2pdma->pool, len, (void **)&ref);
	/* [한국어] 풀에 남은 공간이 부족하다 */
	if (!kaddr) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * vm_insert_page() can sleep, so a reference is taken to mapping
	 * such that rcu_read_unlock() can be done before inserting the
	 * pages
	 */
	if (unlikely(!percpu_ref_tryget_live_rcu(ref))) {
		ret = -ENODEV;
		goto out_free_mem;
	}
	rcu_read_unlock();

	/* [한국어] 페이지 하나씩 사용자 주소 공간에 꽂는다. 한 번에 통째로 꽂는 API 가 없어
	 * 루프를 돈다 */
	for (vaddr = vma->vm_start; vaddr < vma->vm_end; vaddr += PAGE_SIZE) {
		/* [한국어] 커널 가상 주소에서 struct page 를 되찾는다. 이것이 가능한 이유는
		 * pci_p2pdma_add_resource() 가 devm_memremap_pages() 로 이 구간에
		 * struct page 를 만들어 두었기 때문이다 — 장치 BAR 인데도 평범한 페이지처럼
		 * 다룰 수 있는 것이 P2PDMA 의 핵심이다 */
		struct page *page = virt_to_page(kaddr);

		/*
		 * Initialise the refcount for the freshly allocated page. As
		 * we have just allocated the page no one else should be
		 * using it.
		 */
		VM_WARN_ON_ONCE_PAGE(page_ref_count(page), page);
		/* [한국어] 갓 할당한 페이지의 참조 계수를 1 로 세운다. 바로 위 원문 영어 주석대로
		 * 방금 떼어 낸 것이라 아무도 쓰고 있지 않음이 보장된다 */
		set_page_count(page, 1);
		/* [한국어] 페이지를 사용자 가상 주소에 매핑한다. 이 함수가 잠들 수 있어서,
		 * 위에서 percpu_ref 를 먼저 잡고 rcu_read_unlock() 을 해 둔 것이다 */
		ret = vm_insert_page(vma, vaddr, page);
		/* [한국어] 매핑 실패 — 보통 사용자 주소 공간 쪽 문제다 */
		if (ret) {
			/* [한국어] 떼어 냈던 것을 통째로 되돌린다. 이미 꽂은 페이지들은 사용자 공간이
			 * unmap 될 때 정리된다 */
			gen_pool_free(p2pdma->pool, (uintptr_t)kaddr, len);

			/*
			 * Reset the page count. We don't use put_page()
			 * because we don't want to trigger the
			 * p2pdma_folio_free() path.
			 */
			set_page_count(page, 0);
			percpu_ref_put(ref);
			return ret;
		}
		percpu_ref_get(ref);
		put_page(page);
		/* [한국어] 다음 페이지의 커널 주소 */
		kaddr += PAGE_SIZE;
		/* [한국어] 남은 길이를 줄인다. 다만 이 값은 루프 조건에 쓰이지 않고(vaddr 이 조건이다)
		 * 실패 시 gen_pool_free 에 넘길 길이도 원래 len 이 아니라 줄어든 값이 된다.
		 * 코드를 고치지 않는 원칙에 따라 이 관찰만 적어 둔다 */
		len -= PAGE_SIZE;
	}

	percpu_ref_put(ref);

	return 0;
out_free_mem:
	gen_pool_free(p2pdma->pool, (uintptr_t)kaddr, len);
out:
	rcu_read_unlock();
	return ret;
}

/* [한국어] p2pmem/allocate 바이너리 속성. mmap 만 지원하고 read/write 는 없다 —
 * 이 파일은 장치 메모리를 사용자 주소 공간에 꽂아 주는 일만 한다.
 * 모드 0660 이라 소유자와 그룹만 접근할 수 있다.
 * 바로 아래 원문 영어 주석이 size 를 SZ_1T 로 두는 이유를 밝힌다 —
 * Python 처럼 mmap 전에 파일 크기를 확인하는 구현이 있어서, 크기를 아주
 * 크게 잡아 그 검사를 무해하게 통과시키는 우회다 */
static const struct bin_attribute p2pmem_alloc_attr = {
	.attr = { .name = "allocate", .mode = 0660 },
	.mmap = p2pmem_alloc_mmap,
	/*
	 * Some places where we want to call mmap (ie. python) will check
	 * that the file size is greater than the mmap size before allowing
	 * the mmap to continue. To work around this, just set the size
	 * to be very large.
	 */
	.size = SZ_1T,
};

/* [한국어] size/available/published 세 텍스트 속성 */
static struct attribute *p2pmem_attrs[] = {
	&dev_attr_size.attr,
	&dev_attr_available.attr,
	&dev_attr_published.attr,
	NULL,
};

/* [한국어] 바이너리 속성은 별도 배열로 등록해야 한다. sysfs 가 텍스트 속성과
 * 다른 경로로 다루기 때문이다 */
static const struct bin_attribute *const p2pmem_bin_attrs[] = {
	&p2pmem_alloc_attr,
	NULL,
};

/* [한국어] 위 둘을 묶어 장치 디렉터리의 "p2pmem/" 하위에 만든다.
 * pci_p2pdma_setup_pool() 이 만들고 pci_p2pdma_release() 가 없앤다 */
static const struct attribute_group p2pmem_group = {
	.attrs = p2pmem_attrs,
	.bin_attrs = p2pmem_bin_attrs,
	.name = "p2pmem",
};

/* [한국어]
 * p2pdma_folio_free - ZONE_DEVICE 페이지의 마지막 참조가 사라질 때 불린다
 *
 * @folio: 해제되는 폴리오(여기서는 단일 페이지).  @return: 없음
 *
 * dev_pagemap_ops 의 .folio_free 콜백이다. 보통의 페이지는 버디 할당기로
 * 돌아가지만, 이 페이지들은 장치 BAR 위에 얹힌 것이라 돌아갈 곳이 gen_pool 이다.
 * 그 반납을 여기서 한다.
 *
 * 되짚어 올라가는 경로가 세 단계다.
 *   page -> page_pgmap(page) 로 dev_pagemap
 *        -> to_p2p_pgmap() 으로 이 파일의 래퍼
 *        -> pgmap->mem->owner 로 provider 장치(struct device)
 *        -> to_pci_dev() 로 pci_dev, 그 안의 p2pdma
 * 원문 주석대로 이 시점에는 percpu_ref 를 하나 쥐고 있는 상태라
 * rcu_dereference_protected(..., 1) 로 락 없이 역참조해도 안전하다.
 *
 * gen_pool_free_owner() 로 한 페이지를 돌려주고, 그 구간의 owner 로 등록해 둔
 * percpu_ref 를 하나 놓는다. 그 참조는 p2pmem_alloc_mmap() 이나
 * pci_alloc_p2pmem() 이 잡아 둔 것이다.
 *
 * 실행 컨텍스트: 페이지 해제 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   (mm 의 페이지 해제) → [이 함수] → gen_pool_free_owner() → percpu_ref_put()
 */
static void p2pdma_folio_free(struct folio *folio)
{
	struct page *page = &folio->page;
	struct pci_p2pdma_pagemap *pgmap = to_p2p_pgmap(page_pgmap(page));
	/* safe to dereference while a reference is held to the percpu ref */
	struct pci_p2pdma *p2pdma = rcu_dereference_protected(
		to_pci_dev(pgmap->mem->owner)->p2pdma, 1);
	/* [한국어] gen_pool 이 돌려줄 owner(percpu_ref)를 받을 지역 변수 */
	struct percpu_ref *ref;

	/* [한국어] 이 페이지 하나를 풀에 되돌리고, 그 구간의 owner 를 받아 온다.
	 * page_to_virt 로 커널 가상 주소를 되찾는데, 풀에 넣을 때 쓴 것이
	 * 그 주소이기 때문이다 */
	gen_pool_free_owner(p2pdma->pool, (uintptr_t)page_to_virt(page),
			    PAGE_SIZE, (void **)&ref);
	percpu_ref_put(ref);
}

/* [한국어] 페이지맵 콜백 표. folio_free 하나만 채운다 — 이 페이지들이 해제될 때
 * 버디 할당기가 아니라 gen_pool 로 돌아가야 하기 때문이다.
 * pci_p2pdma_add_resource() 가 pgmap->ops 에 걸어 둔다 */
static const struct dev_pagemap_ops p2pdma_pgmap_ops = {
	.folio_free = p2pdma_folio_free,
};

/* [한국어]
 * pci_p2pdma_release - 장치가 사라질 때 P2PDMA 상태를 정리한다
 *
 * @data: devm_add_action_or_reset 에 넘긴 struct pci_dev.  @return: 없음
 *
 * devm 액션으로 등록되어, 드라이버가 언바인드되거나 probe 가 실패하면
 * 자동으로 불린다.
 *
 * 순서가 이 함수의 전부다.
 *   1) pdev->p2pdma 를 NULL 로 만든다. 이 시점 이후 rcu_read_lock() 을
 *      새로 잡는 쪽은 NULL 을 보게 되어 pci_alloc_p2pmem() 이 곧바로 실패한다.
 *   2) 풀이 있었으면 synchronize_rcu() 로 이미 읽기 구간에 들어가 있는
 *      쪽이 다 빠져나오기를 기다린다. 이것을 건너뛰면 누군가 방금 읽은
 *      포인터로 해제된 풀을 만지게 된다. 풀이 없으면 기다릴 이유도 없다.
 *   3) 매핑 종류 캐시(xarray)를 비운다.
 *   4) gen_pool 을 없애고 sysfs 그룹을 떼어 낸다.
 *
 * struct pci_p2pdma 자체는 devm_kzalloc 으로 잡았으므로 여기서 kfree 하지
 * 않는다 — devm 이 이 액션 뒤에 알아서 반납한다.
 *
 * 실행 컨텍스트: 언바인드 경로의 프로세스 컨텍스트. synchronize_rcu() 로
 * 잠들 수 있다.
 *
 * 호출 체인:
 *   (devm 정리) → [이 함수] → synchronize_rcu() → gen_pool_destroy()
 */
static void pci_p2pdma_release(void *data)
{
	struct pci_dev *pdev = data;
	/* [한국어] 정리할 상태 구조체 */
	struct pci_p2pdma *p2pdma;

	/* [한국어] devm 정리 경로라 다른 쪽과 경쟁하지 않는다. 그래서 RCU 락 없이
	 * 역참조해도 된다는 표시로 ..., 1 을 준다 */
	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);
	/* [한국어] pcim_p2pdma_init() 이 실패했거나 애초에 부르지 않은 장치 */
	if (!p2pdma)
		return;

	/* Flush and disable pci_alloc_p2p_mem() */
	pdev->p2pdma = NULL;
	/* [한국어] 풀이 있었을 때만 기다린다. 읽는 쪽(pci_alloc_p2pmem 등)이 이미 잡아 둔
	 * RCU 읽기 구간에서 빠져나오기를 기다리는 것이며, 이것을 건너뛰면
	 * 방금 읽은 포인터로 곧 해제될 풀을 만지게 된다 */
	if (p2pdma->pool)
		synchronize_rcu();
	xa_destroy(&p2pdma->map_types);

	/* [한국어] 풀이 없으면 아래 해제 대상도 없다. xarray 만 비우고 끝 */
	if (!p2pdma->pool)
		return;

	gen_pool_destroy(p2pdma->pool);
	/* [한국어] sysfs 그룹을 떼어 낸다. 이 시점에는 사용자 매핑이 이미
	 * pci_p2pdma_unmap_mappings() 로 끊겨 있다 */
	sysfs_remove_group(&pdev->dev.kobj, &p2pmem_group);
}

/**
 * pcim_p2pdma_init - Initialise peer-to-peer DMA providers
 * @pdev: The PCI device to enable P2PDMA for
 *
 * This function initializes the peer-to-peer DMA infrastructure
 * for a PCI device. It allocates and sets up the necessary data
 * structures to support P2PDMA operations, including mapping type
 * tracking.
 */
/* [한국어]
 * pcim_p2pdma_init - 장치에 P2PDMA 뼈대를 세운다
 *
 * @pdev: 대상 PCI 장치.  @return: 0 = 성공(이미 되어 있어도 0), -ENOMEM
 *
 * 두 번 불러도 안전하다 — 이미 있으면 그대로 0 을 돌려준다. 그래서
 * pci_p2pdma_add_resource() 가 매번 앞에서 무심코 부를 수 있다.
 *
 * 하는 일:
 *   1) struct pci_p2pdma 를 devm 으로 잡는다.
 *   2) 매핑 종류 캐시(xarray)를 초기화한다.
 *   3) 표준 BAR 여섯 개를 훑으며 MMIO BAR 마다 provider 서술자를 채운다.
 *      원문 영어 주석이 밝히듯 I/O 포트 BAR 는 건너뛴다 — P2P 트랜잭션의
 *      대상이 될 수 없기 때문이다.
 *      bus_offset 은 "PCI 버스에서 본 주소 - CPU 에서 본 주소" 다. 나중에
 *      물리 주소에 이 값을 더하면 상대 장치가 쓸 버스 주소가 나온다.
 *      두 주소가 같은 아키텍처에서는 0 이 된다.
 *   4) 정리 액션을 등록한다. 이것을 rcu_assign_pointer 보다 먼저 하는 것이
 *      중요하다 — 포인터를 먼저 공개하면 그 사이에 실패했을 때 정리할
 *      방법이 없다.
 *   5) rcu_assign_pointer 로 pdev->p2pdma 를 공개한다. 이 대입은 앞선
 *      초기화가 다른 CPU 에도 보이도록 쓰기 장벽을 동반한다.
 *
 * 여기서는 gen_pool 을 만들지 않는다. 풀은 실제로 메모리를 등록할 때
 * pci_p2pdma_setup_pool() 이 만든다 — VFIO 처럼 provider 서술자만 필요하고
 * 풀은 쓰지 않는 이용자가 있기 때문이다.
 *
 * 확인한 외부 호출자: drivers/vfio/pci/vfio_pci_core.c:2103.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   vfio_pci_core_register_device() / pci_p2pdma_add_resource() → [이 함수]
 */
int pcim_p2pdma_init(struct pci_dev *pdev)
{
	struct pci_p2pdma *p2p;
	/* [한국어] i = BAR 번호, ret = devm 액션 등록 결과 */
	int i, ret;

	/* [한국어] 이미 초기화되어 있는지 본다. probe 경로라 경쟁이 없으므로 RCU 락이 필요 없다 */
	p2p = rcu_dereference_protected(pdev->p2pdma, 1);
	/* [한국어] 이미 있으면 아무 일도 하지 않고 성공으로 돌아간다. 그래서
	 * pci_p2pdma_add_resource() 가 BAR 마다 무심코 불러도 된다 */
	if (p2p)
		return 0;

	/* [한국어] 장치 수명에 묶어 잡는다. 언바인드 때 devm 이 알아서 반납한다 */
	p2p = devm_kzalloc(&pdev->dev, sizeof(*p2p), GFP_KERNEL);
	/* [한국어] 할당 실패 */
	if (!p2p)
		return -ENOMEM;

	xa_init(&p2p->map_types);
	/*
	 * Iterate over all standard PCI BARs and record only those that
	 * correspond to MMIO regions. Skip non-memory resources (e.g. I/O
	 * port BARs) since they cannot be used for peer-to-peer (P2P)
	 * transactions.
	 */
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		/* [한국어] I/O 포트 BAR 나 없는 BAR 는 건너뛴다. 바로 위 원문 영어 주석대로
		 * P2P 트랜잭션의 대상이 될 수 있는 것은 MMIO 뿐이다 */
		if (!(pci_resource_flags(pdev, i) & IORESOURCE_MEM))
			continue;

		/* [한국어] 이 BAR 를 소유한 장치. p2pdma_folio_free() 가 이 값으로 provider 를 되짚는다 */
		p2p->mem[i].owner = &pdev->dev;
		/* [한국어] PCI 버스에서 본 주소와 CPU 에서 본 주소의 차이.
		 * 나중에 물리 주소에 이 값을 더하면 상대 장치가 쓸 버스 주소가 나온다.
		 * 두 주소 공간이 같은 아키텍처(대부분의 x86)에서는 0 이 된다 */
		p2p->mem[i].bus_offset =
			pci_bus_address(pdev, i) - pci_resource_start(pdev, i);
	}

	/* [한국어] 정리 액션을 먼저 등록한다. 아래 rcu_assign_pointer 보다 앞이어야 하는
	 * 이유는, 포인터를 먼저 공개해 놓고 액션 등록이 실패하면 그것을
	 * 정리할 방법이 없어지기 때문이다.
	 * _or_reset 판은 등록에 실패하면 그 자리에서 액션을 한 번 실행해 준다 */
	ret = devm_add_action_or_reset(&pdev->dev, pci_p2pdma_release, pdev);
	/* [한국어] 등록 실패 */
	if (ret)
		goto out_p2p;

	/* [한국어] 이제서야 포인터를 공개한다. rcu_assign_pointer 는 쓰기 장벽을 동반해,
	 * 다른 CPU 가 이 포인터를 보는 시점에는 위 초기화가 모두 보이도록 보장한다 */
	rcu_assign_pointer(pdev->p2pdma, p2p);
	return 0;

out_p2p:
	devm_kfree(&pdev->dev, p2p);
	return ret;
}
EXPORT_SYMBOL_GPL(pcim_p2pdma_init);

/**
 * pcim_p2pdma_provider - Get peer-to-peer DMA provider
 * @pdev: The PCI device to enable P2PDMA for
 * @bar: BAR index to get provider
 *
 * This function gets peer-to-peer DMA provider for a PCI device. The lifetime
 * of the provider (and of course the MMIO) is bound to the lifetime of the
 * driver. A driver calling this function must ensure that all references to the
 * provider, and any DMA mappings created for any MMIO, are all cleaned up
 * before the driver remove() completes.
 *
 * Since P2P is almost always shared with a second driver this means some system
 * to notify, invalidate and revoke the MMIO's DMA must be in place to use this
 * function. For example a revoke can be built using DMABUF.
 */
/* [한국어]
 * pcim_p2pdma_provider - BAR 하나에 대응하는 provider 서술자를 얻는다
 *
 * @pdev: 대상 장치.  @bar: BAR 번호(0~5).  @return: 서술자, 또는 NULL
 *
 * 위 원문 영어 주석이 수명 규약을 길게 설명한다. 요점은 이렇다 —
 * 돌려주는 서술자와 그것으로 만든 DMA 매핑의 수명이 드라이버의 수명에
 * 묶여 있으므로, remove() 가 끝나기 전에 모두 정리되어야 한다. P2P 는
 * 거의 항상 두 드라이버가 얽히므로, 상대에게 "이제 못 쓴다" 를 알리고
 * 회수하는 장치가 따로 있어야 한다는 것이다. 원문은 그 예로 DMABUF 를 든다.
 *
 * NULL 을 돌려주는 두 경우:
 *   - 그 BAR 가 MMIO 가 아니다(I/O 포트이거나 아예 없다).
 *   - pcim_p2pdma_init() 을 아직 부르지 않았다. 이쪽은 호출자의 실수이므로
 *     WARN_ON 으로 알린다.
 *
 * 확인한 외부 호출자: drivers/vfio/pci/vfio_pci_dmabuf.c:181,
 * drivers/vfio/pci/nvgrace-gpu/main.c:916.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. rcu_dereference_protected(..., 1) 을
 * 쓰므로 호출자가 장치를 붙들고 있는 상태여야 한다.
 *
 * 호출 체인:
 *   vfio_pci_dma_buf_get_provider() 등 → [이 함수]
 */
struct p2pdma_provider *pcim_p2pdma_provider(struct pci_dev *pdev, int bar)
{
	struct pci_p2pdma *p2p;

	/* [한국어] MMIO BAR 가 아니면 provider 가 될 수 없다 */
	if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM))
		return NULL;

	/* [한국어] 호출자가 장치를 붙들고 있는 상태를 전제로 락 없이 역참조한다 */
	p2p = rcu_dereference_protected(pdev->p2pdma, 1);
	if (WARN_ON(!p2p))
		/* Someone forgot to call to pcim_p2pdma_init() before */
		return NULL;

	/* [한국어] BAR 번호가 곧 배열 인덱스다. 초기화되지 않은 BAR 자리라도 주소는
	 * 유효하며, owner 가 NULL 인 것으로 구분된다 */
	return &p2p->mem[bar];
}
EXPORT_SYMBOL_GPL(pcim_p2pdma_provider);

/* [한국어]
 * pci_p2pdma_setup_pool - gen_pool 과 sysfs 그룹을 만든다
 *
 * @pdev: 대상 장치.  @return: 0 = 성공(이미 있어도 0), -ENOMEM 또는 sysfs 오류
 *
 * pcim_p2pdma_init() 과 마찬가지로 두 번 불러도 안전하다. BAR 를 여러 개
 * 등록하는 장치라도 풀은 하나만 만든다.
 *
 * gen_pool_create(PAGE_SHIFT, node) 의 첫 인자는 할당 단위의 로그값이다.
 * PAGE_SHIFT 를 주는 이유는 이 메모리가 struct page 를 갖고 사용자 공간에
 * mmap 될 수 있어, 페이지보다 잘게 쪼갤 수 없기 때문이다.
 * 두 번째 인자 dev_to_node() 는 NUMA 노드 힌트다 — 풀 관리 자료구조를
 * 장치와 가까운 노드에 두려는 것이다.
 *
 * 그 다음 sysfs 의 p2pmem 그룹(size/available/published 와 allocate)을 만든다.
 * 실패하면 방금 만든 풀을 되돌리고 포인터도 NULL 로 되돌린다 — 그래야
 * 다음 호출이 다시 시도할 수 있고, pci_p2pdma_release() 도 없는 풀을
 * 없애려 들지 않는다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_p2pdma_add_resource() → [이 함수] → gen_pool_create() → sysfs_create_group()
 */
static int pci_p2pdma_setup_pool(struct pci_dev *pdev)
{
	struct pci_p2pdma *p2pdma;
	/* [한국어] sysfs 그룹 생성 결과 */
	int ret;

	/* [한국어] probe 경로라 경쟁이 없다 */
	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);
	if (p2pdma->pool)
		/* We already setup pools, do nothing, */
		return 0;

	/* [한국어] 첫 인자는 할당 단위의 로그값이다. PAGE_SHIFT 를 주는 이유는 이 메모리가
	 * struct page 를 갖고 사용자 공간에 mmap 될 수 있어 페이지보다 잘게
	 * 쪼갤 수 없기 때문이다. 둘째 인자는 NUMA 노드 힌트로, 풀 관리
	 * 자료구조를 장치와 가까운 노드에 두려는 것이다 */
	p2pdma->pool = gen_pool_create(PAGE_SHIFT, dev_to_node(&pdev->dev));
	/* [한국어] 할당 실패 */
	if (!p2pdma->pool)
		return -ENOMEM;

	/* [한국어] size/available/published 와 allocate 를 장치 디렉터리의 p2pmem/ 아래에 만든다 */
	ret = sysfs_create_group(&pdev->dev.kobj, &p2pmem_group);
	/* [한국어] sysfs 등록 실패 */
	if (ret)
		goto out_pool_destroy;

	return 0;

out_pool_destroy:
	gen_pool_destroy(p2pdma->pool);
	/* [한국어] 포인터도 되돌린다. 그래야 다음 호출이 다시 시도할 수 있고,
	 * pci_p2pdma_release() 도 없는 풀을 없애려 들지 않는다 */
	p2pdma->pool = NULL;
	return ret;
}

/* [한국어]
 * pci_p2pdma_unmap_mappings - 사용자 공간 매핑을 끊고 새 매핑을 막는다
 *
 * @data: devm 액션에 넘긴 struct pci_p2pdma_pagemap.  @return: 없음
 *
 * devm 액션이며, 장치가 사라질 때 devm_memunmap_pages() 보다 *먼저* 불려야
 * 한다. 사용자 공간이 아직 그 BAR 를 매핑하고 있는데 페이지맵을 없애면
 * 곧바로 커널이 무너지기 때문이다.
 *
 * 방법이 간접적이다. 원문 영어 주석이 그 이유를 밝힌다 — sysfs 에서
 * allocate 속성을 떼어 내면 sysfs 가 그 inode 에 대해
 * unmap_mapping_range() 를 부르고, 그것이 기존 사용자 매핑을 통째로
 * 끊으면서 새 매핑도 막는다. 직접 순회하며 끊는 코드를 쓰지 않고
 * sysfs 의 기존 동작에 얹은 것이다.
 *
 * 등록 순서가 곧 해제 순서를 정한다. pci_p2pdma_add_resource() 에서
 * devm_memremap_pages() 다음에 이 액션을 등록하므로, 해제 때는 역순으로
 * 이것이 먼저 불린다.
 *
 * 실행 컨텍스트: 언바인드 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (devm 정리) → [이 함수] → sysfs_remove_file_from_group()
 */
static void pci_p2pdma_unmap_mappings(void *data)
{
	struct pci_p2pdma_pagemap *p2p_pgmap = data;

	/*
	 * Removing the alloc attribute from sysfs will call
	 * unmap_mapping_range() on the inode, teardown any existing userspace
	 * mappings and prevent new ones from being created.
	 */
	sysfs_remove_file_from_group(&p2p_pgmap->mem->owner->kobj,
				     &p2pmem_alloc_attr.attr,
				     p2pmem_group.name);
}

/**
 * pci_p2pdma_add_resource - add memory for use as p2p memory
 * @pdev: the device to add the memory to
 * @bar: PCI BAR to add
 * @size: size of the memory to add, may be zero to use the whole BAR
 * @offset: offset into the PCI BAR
 *
 * The memory will be given ZONE_DEVICE struct pages so that it may
 * be used with any DMA request.
 */
/* [한국어]
 * pci_p2pdma_add_resource - BAR 의 한 구간을 P2P 메모리로 등록한다
 *
 * @pdev: 메모리를 내놓는 장치(provider)
 * @bar: 어느 BAR 인가.  @size: 크기(0 이면 오프셋부터 BAR 끝까지)
 * @offset: BAR 안에서의 시작 오프셋
 * @return: 0 = 성공, -EINVAL = BAR/범위가 잘못됨, -ENOMEM,
 *          그 밖에는 devm_memremap_pages() 의 실패값
 *
 * 이 파일의 진입점이자 가장 중요한 함수다. NVMe 라면 nvme_map_cmb() 이
 * CMBLOC/CMBSZ 레지스터로 CMB 의 위치와 크기를 알아낸 뒤 이 함수를
 * 부른다(drivers/nvme/host/pci.c:3226).
 *
 * 핵심은 "장치 BAR 메모리에 struct page 를 붙인다" 는 것이다. 위 원문
 * 영어 주석이 그 목적을 말한다 — ZONE_DEVICE 페이지를 갖게 되면 그 메모리를
 * 보통의 DMA 요청에 그대로 실을 수 있다. scatterlist 도, bio 도, 페이지
 * 단위로 도는 모든 커널 코드가 그것을 평범한 메모리처럼 다룰 수 있게 된다.
 *
 * 절차:
 *   1) 인자 검증 — MMIO BAR 인가, 오프셋이 BAR 안인가, size 를 0 으로 주면
 *      나머지 전부로 채우고, 합이 BAR 를 넘지 않는가.
 *   2) pcim_p2pdma_init() 과 pci_p2pdma_setup_pool() 을 부른다. 둘 다
 *      두 번 불러도 안전하므로 BAR 를 여러 개 등록해도 문제가 없다.
 *   3) 그 BAR 의 provider 서술자를 얻는다. 1)에서 이미 MMIO 임을 확인했으니
 *      NULL 일 수 없고, 그래서 WARN_ON 으로만 방어한다.
 *   4) struct pci_p2pdma_pagemap 을 잡고 dev_pagemap 을 채운다.
 *      range 는 CPU 물리 주소 범위이고, type 을 MEMORY_DEVICE_PCI_P2PDMA 로
 *      두어 is_pci_p2pdma_page() 가 나중에 이 페이지들을 알아볼 수 있게 한다.
 *      ops 로 p2pdma_pgmap_ops 를 걸어 페이지 해제를 이 파일이 받는다.
 *   5) devm_memremap_pages() — 여기서 실제로 struct page 배열이 만들어진다.
 *      돌려주는 것은 그 구간의 커널 가상 주소다.
 *   6) 사용자 매핑 정리 액션을 등록한다(5 다음이어야 역순 해제가 맞는다).
 *   7) gen_pool_add_owner() 로 풀에 넣는다. 여기서 중요한 것은 두 번째
 *      인자와 세 번째 인자다 — 가상 주소는 5)가 준 것을 쓰고, "물리 주소"
 *      자리에는 CPU 물리 주소가 아니라 PCI 버스 주소를 넣는다. 그래서
 *      나중에 gen_pool_virt_to_phys() 가 곧바로 버스 주소를 돌려주게 된다
 *      (pci_p2pmem_virt_to_bus() 의 주석이 이 사정을 밝힌다).
 *      owner 로 페이지맵의 percpu_ref 를 등록해, 할당한 쪽이 참조를
 *      함께 잡을 수 있게 한다.
 *
 * 에러 경로는 goto 두 단계로 역순으로 되돌린다. devm 으로 잡은 것도
 * 여기서는 손으로 되돌리는데, probe 가 계속 진행될 수 있으므로 실패한
 * 자원을 장치 수명 끝까지 들고 있을 이유가 없기 때문이다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   nvme_map_cmb() [drivers/nvme/host/pci.c:3226] → [이 함수]
 *     → devm_memremap_pages() → gen_pool_add_owner()
 */
int pci_p2pdma_add_resource(struct pci_dev *pdev, int bar, size_t size,
			    u64 offset)
{
	struct pci_p2pdma_pagemap *p2p_pgmap;
	/* [한국어] 이 BAR 의 provider 서술자 */
	struct p2pdma_provider *mem;
	/* [한국어] 아래 p2p_pgmap 안의 dev_pagemap 을 가리키는 지름길 */
	struct dev_pagemap *pgmap;
	/* [한국어] gen_pool 에 넣을 때 쓸 상태 구조체 */
	struct pci_p2pdma *p2pdma;
	/* [한국어] devm_memremap_pages() 가 돌려줄 커널 가상 주소 */
	void *addr;
	/* [한국어] 에러 코드 */
	int error;

	/* [한국어] MMIO BAR 가 아니면 P2P 메모리가 될 수 없다 */
	if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM))
		return -EINVAL;

	/* [한국어] 오프셋이 BAR 바깥이면 잘못된 요청이다 */
	if (offset >= pci_resource_len(pdev, bar))
		return -EINVAL;

	/* [한국어] size 를 0 으로 주면 */
	if (!size)
		/* [한국어] 오프셋부터 BAR 끝까지 전부를 뜻한다. NVMe 는 CMB 크기를 알고 있어
		 * 명시적으로 넘기지만, BAR 전체를 내놓는 드라이버는 0 을 준다 */
		size = pci_resource_len(pdev, bar) - offset;

	/* [한국어] 합이 BAR 를 넘으면 잘못된 요청이다. size 를 계산해 넣은 뒤에
	 * 다시 확인하는 것이라 위 검사와 중복이 아니다 */
	if (size + offset > pci_resource_len(pdev, bar))
		return -EINVAL;

	/* [한국어] 상태 구조체가 없으면 만든다. 이미 있으면 그냥 0 을 돌려주므로
	 * BAR 를 여러 개 등록해도 문제가 없다 */
	error = pcim_p2pdma_init(pdev);
	/* [한국어] 메모리 부족 */
	if (error)
		return error;

	/* [한국어] 풀과 sysfs 그룹도 없으면 만든다. 역시 두 번 불러도 안전하다 */
	error = pci_p2pdma_setup_pool(pdev);
	/* [한국어] 실패 */
	if (error)
		return error;

	mem = pcim_p2pdma_provider(pdev, bar);
	/*
	 * We checked validity of BAR prior to call
	 * to pcim_p2pdma_provider. It should never return NULL.
	 */
	if (WARN_ON(!mem))
		return -EINVAL;

	/* [한국어] 이 BAR 구간 하나에 대응하는 페이지맵 래퍼. BAR 를 여러 개 등록하면
	 * 이것도 여러 개가 된다 */
	p2p_pgmap = devm_kzalloc(&pdev->dev, sizeof(*p2p_pgmap), GFP_KERNEL);
	/* [한국어] 메모리 부족 */
	if (!p2p_pgmap)
		return -ENOMEM;

	/* [한국어] 래퍼 안의 dev_pagemap 을 가리킨다. 첫 멤버라 주소가 같지만,
	 * 의도를 드러내려고 멤버 이름으로 접근한다 */
	pgmap = &p2p_pgmap->pgmap;
	/* [한국어] 덮을 CPU 물리 주소 범위의 시작. BAR 의 물리 시작 주소에 오프셋을 더한다 */
	pgmap->range.start = pci_resource_start(pdev, bar) + offset;
	/* [한국어] 끝 주소. -1 을 하는 것은 이 range 가 닫힌 구간(양 끝 포함)이기 때문이다 */
	pgmap->range.end = pgmap->range.start + size - 1;
	/* [한국어] 구간이 하나뿐이다. 흩어진 여러 구간을 한 페이지맵으로 묶는 이용자도 있지만
	 * 여기서는 BAR 안의 연속된 한 덩어리만 다룬다 */
	pgmap->nr_range = 1;
	/* [한국어] 이 페이지들의 종류를 표시한다. is_pci_p2pdma_page() 가 이 값으로
	 * P2P 페이지를 알아보고, 블록 계층과 NVMe 가 그 판정에 기대어
	 * 매핑 방식을 가른다(drivers/nvme/host/pci.c:1597, :1777) */
	pgmap->type = MEMORY_DEVICE_PCI_P2PDMA;
	/* [한국어] 페이지 해제 콜백을 이 파일로 돌린다. 이 페이지들은 버디 할당기가 아니라
	 * gen_pool 로 돌아가야 하기 때문이다 */
	pgmap->ops = &p2pdma_pgmap_ops;
	/* [한국어] 이 페이지맵이 어느 BAR 에서 왔는지를 기록한다. p2pdma_folio_free() 와
	 * __pci_p2pdma_update_state() 가 이 고리를 거꾸로 따라간다 */
	p2p_pgmap->mem = mem;

	/* [한국어] 여기서 실제로 struct page 배열이 만들어진다. 장치 BAR 라는 물리 주소
	 * 구간에 커널 메모리 관리 구조를 얹는 것이며, 이 한 줄이 P2PDMA 를
	 * 가능하게 하는 근본이다. 돌려주는 것은 그 구간의 커널 가상 주소다 */
	addr = devm_memremap_pages(&pdev->dev, pgmap);
	/* [한국어] 실패는 ERR_PTR 로 온다 */
	if (IS_ERR(addr)) {
		/* [한국어] 오류 코드를 꺼내 아래 goto 로 넘긴다 */
		error = PTR_ERR(addr);
		goto pgmap_free;
	}

	/* [한국어] 사용자 매핑 정리 액션을 등록한다. devm_memremap_pages() 뒤에 등록해야
	 * 해제 때 역순으로 이것이 먼저 불려, 페이지맵이 사라지기 전에
	 * 사용자 매핑이 끊긴다 */
	error = devm_add_action_or_reset(&pdev->dev, pci_p2pdma_unmap_mappings,
					 p2p_pgmap);
	/* [한국어] 등록 실패 */
	if (error)
		goto pages_free;

	/* [한국어] 풀에 넣기 위해 상태 구조체를 꺼낸다 */
	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);
	/* [한국어] 풀에 구간을 등록한다. 두 번째 인자는 커널 가상 주소이고,
	 * 세 번째 인자 자리(gen_pool 이 "물리 주소" 로 다루는 자리)에는
	 * CPU 물리 주소가 아니라 PCI 버스 주소를 넣는다. 그래야 나중에
	 * gen_pool_virt_to_phys() 가 곧바로 버스 주소를 돌려준다 —
	 * pci_p2pmem_virt_to_bus() 의 원문 주석이 이 사정을 설명한다.
	 * owner 로 페이지맵의 percpu_ref 를 넘겨, 할당하는 쪽이 그 참조를
	 * 함께 잡을 수 있게 한다 */
	error = gen_pool_add_owner(p2pdma->pool, (unsigned long)addr,
			pci_bus_address(pdev, bar) + offset,
			range_len(&pgmap->range), dev_to_node(&pdev->dev),
			&pgmap->ref);
	/* [한국어] 풀 등록 실패 */
	if (error)
		goto pages_free;

	/* [한국어] 성공 로그. dmesg 에서 등록된 물리 주소 범위를 확인할 수 있다 */
	pci_info(pdev, "added peer-to-peer DMA memory %#llx-%#llx\n",
		 pgmap->range.start, pgmap->range.end);

	return 0;

pages_free:
	devm_memunmap_pages(&pdev->dev, pgmap);
pgmap_free:
	devm_kfree(&pdev->dev, p2p_pgmap);
	return error;
}
EXPORT_SYMBOL_GPL(pci_p2pdma_add_resource);

/*
 * Note this function returns the parent PCI device with a
 * reference taken. It is the caller's responsibility to drop
 * the reference.
 */
/* [한국어]
 * find_parent_pci_dev - 임의의 device 에서 위로 올라가 PCI 장치를 찾는다
 *
 * @dev: 시작점. PCI 장치가 아니어도 된다
 * @return: 찾은 pci_dev(참조를 잡은 채로), 못 찾으면 NULL
 *
 * P2P 소비자로 넘어오는 struct device 는 PCI 장치 자신이 아닐 수 있다.
 * 예를 들어 NVMe 블록 장치나 RDMA 장치는 PCI 장치의 자식으로 달려 있다.
 * 경로 판정은 PCI 트리 위에서 해야 하므로, 먼저 그 조상 PCI 장치를 찾는다.
 *
 * 참조 계수를 다루는 방식이 요점이다. 위 원문 주석이 경고하듯 돌려주는
 * pci_dev 는 참조가 잡힌 상태이고, 호출자가 pci_dev_put() 으로 놓아야 한다.
 * 루프 안에서는 부모의 참조를 먼저 잡고(get_device) 자식의 것을 놓는
 * (put_device) 순서를 지킨다 — 반대로 하면 자식이 사라지면서 부모 포인터가
 * 함께 무효가 될 수 있다.
 *
 * 맨 앞의 get_device(dev) 는 인자로 받은 것에도 참조를 하나 잡아,
 * 루프 안의 put_device 와 짝을 맞춘다. 그래서 첫 판정에서 바로 반환할 때도
 * 참조가 하나 잡힌 상태가 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_p2pdma_distance_many() → [이 함수] → get_device()/put_device()
 */
static struct pci_dev *find_parent_pci_dev(struct device *dev)
{
	struct device *parent;

	/* [한국어] 인자로 받은 것에도 참조를 하나 잡아, 아래 루프의 put_device 와 짝을 맞춘다.
	 * 그래서 첫 판정에서 바로 반환할 때도 참조가 하나 잡힌 상태가 된다 */
	dev = get_device(dev);

	/* [한국어] PCI 장치를 만날 때까지 부모 방향으로 올라간다 */
	while (dev) {
		/* [한국어] 이 device 가 PCI 장치인가 */
		if (dev_is_pci(dev))
			/* [한국어] 맞으면 참조를 잡은 채로 돌려준다. 호출자가 pci_dev_put() 으로 놓아야 한다 */
			return to_pci_dev(dev);

		/* [한국어] 부모의 참조를 먼저 잡는다. 순서가 중요하다 — 자식을 먼저 놓으면
		 * 자식이 사라지면서 부모 포인터가 함께 무효가 될 수 있다 */
		parent = get_device(dev->parent);
		put_device(dev);
		/* [한국어] 한 칸 올라간다 */
		dev = parent;
	}

	return NULL;
}

/*
 * Check if a PCI bridge has its ACS redirection bits set to redirect P2P
 * TLPs upstream via ACS. Returns 1 if the packets will be redirected
 * upstream, 0 otherwise.
 */
/* [한국어]
 * pci_bridge_has_acs_redir - 이 브리지가 P2P 트래픽을 위로 되돌리는가
 *
 * @pdev: 검사할 브리지.  @return: 1 = 되돌린다(P2P 지름길이 막혔다), 0 = 아니다
 *
 * ACS(Access Control Services)는 스위치가 "옆으로 바로 보내지 말고 위로
 * 올려보내라" 를 강제하는 기능이다. IOMMU 격리를 위해 존재한다 — 두 장치가
 * 스위치 안에서 몰래 주고받으면 IOMMU 가 그것을 볼 수 없기 때문이다.
 * 그런데 P2PDMA 가 원하는 것이 바로 그 "옆으로 바로" 이므로, 둘은 정면으로
 * 부딪친다. 성능과 격리의 맞교환이며, 이 함수가 그 상태를 읽는다.
 *
 * 세 비트 중 하나라도 서 있으면 1 이다.
 *   PCI_ACS_RR - Request Redirect: 요청 TLP 를 위로 되돌린다.
 *   PCI_ACS_CR - Completion Redirect: 완료 TLP 를 위로 되돌린다.
 *   PCI_ACS_EC - Egress Control: 나가는 포트를 제한한다.
 * 하나라도 걸리면 두 장치 사이의 트래픽이 호스트 브리지를 지나게 되고,
 * 그러면 BUS_ADDR 직행이 성립하지 않는다.
 *
 * ACS capability 자체가 없으면(pos 가 0) 되돌릴 수단이 없으므로 0 이다.
 * pdev->acs_cap 은 열거 때 drivers/pci/pci.c 의 pci_acs_init() 이 캐시해 둔
 * 오프셋이다(그 사정은 pci.c:1848 의 주석에 있다).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. config 읽기를 한다.
 *
 * 호출 체인:
 *   calc_map_type_and_dist() → [이 함수] → pci_read_config_ 계열
 */
static int pci_bridge_has_acs_redir(struct pci_dev *pdev)
{
	int pos;
	/* [한국어] ACS Control 레지스터 값 */
	u16 ctrl;

	/* [한국어] 열거 때 drivers/pci/pci.c 의 pci_acs_init() 이 캐시해 둔 ACS capability 오프셋 */
	pos = pdev->acs_cap;
	/* [한국어] ACS capability 가 없으면 되돌릴 수단 자체가 없다 */
	if (!pos)
		return 0;

	/* [한국어] ACS Control 을 읽는다. 브리지마다 매번 config 읽기가 일어나므로
	 * 경로가 길수록 이 함수가 비싸진다 — 결과를 캐시하는 이유다 */
	pci_read_config_word(pdev, pos + PCI_ACS_CTRL, &ctrl);

	/* [한국어] RR(Request Redirect) / CR(Completion Redirect) / EC(Egress Control)
	 * 셋 중 하나라도 서 있으면 이 브리지는 P2P 트래픽을 옆으로 넘기지 않고
	 * 위로 올려보낸다 */
	if (ctrl & (PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_EC))
		/* [한국어] 그러면 직행 경로가 성립하지 않는다 */
		return 1;

	return 0;
}

/* [한국어]
 * seq_buf_print_bus_devfn - ACS 를 끄라고 안내할 장치 이름을 버퍼에 덧붙인다
 *
 * @buf: 문자열을 모으는 seq_buf. NULL 이면 아무 일도 하지 않는다
 * @pdev: 이름을 적을 장치.  @return: 없음
 *
 * calc_map_type_and_dist() 가 ACS redirect 가 켜진 브리지를 만날 때마다
 * 이 함수로 이름을 모은다. 마지막에 그 목록이
 * "pci=disable_acs_redir=0000:00:1c.0;0000:00:1d.0" 같은 부팅 인자 안내로
 * 사용자에게 그대로 제시된다. 사람이 lspci 로 직접 찾아내야 할 일을
 * 커널이 대신해 주는 셈이다.
 *
 * 세미콜론을 뒤에 붙여 이어 가고, 마지막 하나는 호출자가
 * acs_list.buffer[acs_list.len-1] = 0 으로 잘라 낸다.
 *
 * buf 가 NULL 일 수 있는 이유: 판정만 필요하고 안내가 필요 없는 호출
 * (verbose 가 거짓인 경로)에서도 같은 코드를 지나가기 때문이다.
 * seq_buf 는 넘치면 조용히 잘라 내므로 버퍼 넘침 걱정이 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   calc_map_type_and_dist() → [이 함수] → seq_buf_printf()
 */
static void seq_buf_print_bus_devfn(struct seq_buf *buf, struct pci_dev *pdev)
{
	if (!buf)
		return;

	seq_buf_printf(buf, "%s;", pci_name(pdev));
}

/* [한국어]
 * cpu_supports_p2pdma - 이 CPU 의 Root Complex 가 P2P 를 제대로 전달하는가
 *
 * @return: true 면 화이트리스트를 보지 않고도 호스트 브리지 경유를 허용한다
 *
 * 화이트리스트는 개별 호스트 브리지의 device ID 를 하나씩 적어 두는 방식이라
 * 새 하드웨어가 나올 때마다 항목을 추가해야 한다. 이 함수는 그보다 넓은
 * 예외를 둔다 — 어떤 CPU 계열 전체가 P2P 를 지원한다고 알려져 있으면
 * 목록을 볼 필요가 없다.
 *
 * 지금 있는 항목은 하나다. 위 원문 영어 주석대로 AMD 의 Zen 이후
 * (family ID 0x17 이상) 전 제품이 여기 든다.
 *
 * CONFIG_X86 이 아닌 아키텍처에서는 항상 false 다. 다른 아키텍처의
 * Root Complex 에 대해 이 파일이 아는 바가 없기 때문이며, 그런 시스템은
 * 화이트리스트를 통과하거나 같은 스위치 아래에 있어야 P2P 를 쓸 수 있다.
 *
 * 실행 컨텍스트: 제한 없음. cpu_data(0) 조회뿐이다.
 *
 * 호출 체인:
 *   calc_map_type_and_dist() → [이 함수]
 */
static bool cpu_supports_p2pdma(void)
{
/* [한국어] 이 판정은 x86 의 CPU 계열 정보에 기대므로 그 아키텍처에서만 컴파일한다.
 * 다른 아키텍처에서는 아래 return false 만 남아, 화이트리스트를 통과하거나
 * 같은 스위치 아래에 있어야 P2P 를 쓸 수 있게 된다 — 이 파일이 그 Root
 * Complex 들에 대해 아는 바가 없기 때문이다 */
#ifdef CONFIG_X86
	/* [한국어] 부트 CPU 의 정보. 이 판정은 CPU 계열 전체에 대한 것이라
	 * 어느 CPU 를 봐도 결과가 같다 */
	struct cpuinfo_x86 *c = &cpu_data(0);

	/* Any AMD CPU whose family ID is Zen or newer supports p2pdma */
	if (c->x86_vendor == X86_VENDOR_AMD && c->x86 >= 0x17)
		return true;
#endif

	return false;
}

/* [한국어] Root Complex 를 거치는 P2P 를 허용할 호스트 브리지 목록.
 * 왜 목록인가: 많은 Root Complex 가 P2P 트랜잭션을 옆으로 넘겨 주지 못하거나,
 * 넘기더라도 성능이 처참하다. 스펙이 그것을 요구하지 않기 때문이다.
 * 그래서 실제로 검증된 하드웨어만 골라 적어 둔다.
 * 읽는 자: __host_bridge_whitelist().
 * 동기화: const 정적 배열이라 보호가 필요 없다 */
static const struct pci_p2pdma_whitelist_entry {
	/* [한국어] 허용할 호스트 브리지의 PCI Vendor ID.
	 * 설정자: 아래 초기화자(정적 상수).
	 * 읽는 자: __host_bridge_whitelist() 가 대표 장치의 root->vendor 와 비교한다.
	 * 값 범위: 0 이면 표의 끝을 뜻한다. 그래서 순회 루프가 entry->vendor 를
	 *   종료 조건으로 쓰고 개수를 따로 세지 않는다.
	 * 동기화: const 정적 데이터라 보호가 필요 없다 */
	unsigned short vendor;
	/* [한국어] 허용할 호스트 브리지의 PCI Device ID.
	 * 설정자: 아래 초기화자.
	 * 읽는 자: __host_bridge_whitelist() 가 root->device 와 비교한다.
	 * 값 범위: PCI_ANY_ID 면 그 vendor 의 모든 장치를 허용한다(Google SoC 항목).
	 *   타입이 unsigned short 가 아니라 int 인 것은 PCI_ANY_ID 가 16비트
	 *   범위를 벗어난 표식 값이라서다.
	 * 동기화: const 정적 데이터 */
	int device;
	/* [한국어] 항목마다 붙는 조건 플래그의 열거형. 지금은 값이 하나뿐이라 익명 열거형으로
	 * 구조체 안에 직접 정의했다 — 이 표 밖에서 쓸 일이 없기 때문이다 */
	enum {
		/* [한국어] 이 하드웨어는 같은 호스트 브리지 안에서의 P2P 만 검증되었다는 표시.
		 * 1 << 0 으로 비트 하나를 차지하며, 나중에 조건이 늘면 그 옆 비트를 쓴다.
		 * 읽는 자: __host_bridge_whitelist() 가 same_host_bridge 인자와 함께 본다.
		 *   플래그가 서 있는데 두 장치가 서로 다른 브리지에 있으면 거절한다.
		 * 값 범위: 이 표에서는 옛 Intel Xeon E5/E7 계열 네 항목에만 붙어 있다 */
		REQ_SAME_HOST_BRIDGE	= 1 << 0,
	/* [한국어] 위 열거형 값들의 비트 묶음.
	 * 설정자: 아래 초기화자. 0 이면 조건 없이 허용한다는 뜻이다.
	 * 읽는 자: __host_bridge_whitelist().
	 * 동기화: const 정적 데이터 */
	} flags;
/* [한국어] 표의 실제 항목들. 원문 주석이 각 줄의 하드웨어 이름을 밝힌다.
 * 앞의 Xeon E5/E7 계열 넷에만 REQ_SAME_HOST_BRIDGE 가 붙어 있는데,
 * 그 하드웨어는 같은 호스트 브리지 안에서의 P2P 만 검증되었다는 뜻이다.
 * Skylake-E 이후와 Google SoC 는 플래그가 0 이라 서로 다른 브리지 사이도
 * 허용된다. PCI_ANY_ID 는 vendor 만 맞으면 통과시킨다 */
} pci_p2pdma_whitelist[] = {
	/* Intel Xeon E5/Core i7 */
	{PCI_VENDOR_ID_INTEL,	0x3c00, REQ_SAME_HOST_BRIDGE},
	{PCI_VENDOR_ID_INTEL,	0x3c01, REQ_SAME_HOST_BRIDGE},
	/* Intel Xeon E7 v3/Xeon E5 v3/Core i7 */
	{PCI_VENDOR_ID_INTEL,	0x2f00, REQ_SAME_HOST_BRIDGE},
	{PCI_VENDOR_ID_INTEL,	0x2f01, REQ_SAME_HOST_BRIDGE},
	/* Intel Skylake-E */
	{PCI_VENDOR_ID_INTEL,	0x2030, 0},
	{PCI_VENDOR_ID_INTEL,	0x2031, 0},
	{PCI_VENDOR_ID_INTEL,	0x2032, 0},
	{PCI_VENDOR_ID_INTEL,	0x2033, 0},
	{PCI_VENDOR_ID_INTEL,	0x2020, 0},
	{PCI_VENDOR_ID_INTEL,	0x09a2, 0},
	/* Google SoCs. */
	{PCI_VENDOR_ID_GOOGLE,	PCI_ANY_ID, 0},
	{}
};

/*
 * If the first device on host's root bus is either devfn 00.0 or a PCIe
 * Root Port, return it.  Otherwise return NULL.
 *
 * We often use a devfn 00.0 "host bridge" in the pci_p2pdma_whitelist[]
 * (though there is no PCI/PCIe requirement for such a device).  On some
 * platforms, e.g., Intel Skylake, there is no such host bridge device, and
 * pci_p2pdma_whitelist[] may contain a Root Port at any devfn.
 *
 * This function is similar to pci_get_slot(host->bus, 0), but it does
 * not take the pci_bus_sem lock since __host_bridge_whitelist() must not
 * sleep.
 *
 * For this to be safe, the caller should hold a reference to a device on the
 * bridge, which should ensure the host_bridge device will not be freed
 * or removed from the head of the devices list.
 */
/* [한국어]
 * pci_host_bridge_dev - 화이트리스트와 대조할 대표 장치를 고른다
 *
 * @host: 호스트 브리지.  @return: 대표 장치, 또는 NULL
 *
 * 위 원문 영어 주석이 이 함수의 사정을 자세히 설명한다. 요약하면 —
 * 화이트리스트는 보통 devfn 00.0 의 "호스트 브리지 장치" 를 적어 두지만
 * PCI/PCIe 스펙이 그런 장치의 존재를 요구하지 않는다. Intel Skylake 처럼
 * 그런 장치가 없고 임의의 devfn 에 Root Port 만 있는 플랫폼도 있다.
 * 그래서 둘 중 하나를 받아들인다.
 *
 * 그리고 pci_get_slot(host->bus, 0) 을 쓰지 않는 이유도 원문이 밝힌다 —
 * 그 함수는 pci_bus_sem 을 잡는데, __host_bridge_whitelist() 는 잠들면
 * 안 되는 문맥에서도 불릴 수 있다. 그래서 락 없이 루트 버스의 첫 장치만
 * 꺼내 본다.
 *
 * 락 없이 리스트를 만지는 것이 안전한 근거도 원문에 있다 — 호출자가 그
 * 브리지 아래 장치에 대한 참조를 쥐고 있으므로, 리스트 머리의 호스트
 * 브리지 장치가 사라지거나 빠져나가지 않는다.
 *
 * 실행 컨텍스트: 잠들지 않는다. 그것이 이 구현의 존재 이유다.
 *
 * 호출 체인:
 *   __host_bridge_whitelist() → [이 함수] → list_first_entry_or_null()
 */
static struct pci_dev *pci_host_bridge_dev(struct pci_host_bridge *host)
{
	struct pci_dev *root;

	/* [한국어] 루트 버스의 첫 장치를 꺼낸다. 바로 위 원문 영어 주석대로
	 * pci_get_slot() 을 쓰지 않는 이유는 그 함수가 pci_bus_sem 을 잡기 때문이며,
	 * 이 경로는 잠들면 안 된다 */
	root = list_first_entry_or_null(&host->bus->devices,
					struct pci_dev, bus_list);

	/* [한국어] 루트 버스에 장치가 하나도 없는 경우 */
	if (!root)
		return NULL;

	/* [한국어] devfn 00.0 이면 관례적인 "호스트 브리지 장치" 다 */
	if (root->devfn == PCI_DEVFN(0, 0))
		/* [한국어] 그것을 대표로 쓴다 */
		return root;

	/* [한국어] 그런 장치가 없는 플랫폼(원문이 예로 드는 Intel Skylake)에서는
	 * 임의 devfn 의 Root Port 를 대표로 받아들인다 */
	if (pci_pcie_type(root) == PCI_EXP_TYPE_ROOT_PORT)
		/* [한국어] 그것을 대표로 쓴다 */
		return root;

	return NULL;
}

/* [한국어]
 * __host_bridge_whitelist - 이 호스트 브리지가 P2P 허용 목록에 있는가
 *
 * @host: 검사할 호스트 브리지
 * @same_host_bridge: 두 장치가 같은 호스트 브리지 아래에 있는가
 * @warn: 목록에 없을 때 경고를 찍을 것인가
 * @return: true 면 이 브리지를 거치는 P2P 를 허용한다
 *
 * 대표 장치의 vendor/device ID 를 pci_p2pdma_whitelist[] 와 대조한다.
 * PCI_ANY_ID 항목(Google SoC)은 vendor 만 맞으면 통과시킨다.
 *
 * REQ_SAME_HOST_BRIDGE 플래그가 붙은 항목은 조건이 하나 더 붙는다.
 * 그 하드웨어는 같은 호스트 브리지 안에서의 P2P 만 검증되었다는 뜻이라,
 * 두 장치가 서로 다른 호스트 브리지에 매달려 있으면 거절한다.
 * 목록을 보면 옛 Intel Xeon E5/E7 계열이 그렇고, Skylake-E 이후는
 * 플래그가 0 이라 서로 다른 브리지 사이도 허용된다.
 *
 * 대표 장치를 못 찾으면(root 가 NULL) 대조할 ID 가 없으므로 false 다.
 *
 * @warn 이 참일 때만 경고를 찍는 이유: pci_p2pmem_find_many() 가 후보를
 * 훑을 때는 대부분이 탈락하는 것이 정상이라, 그때마다 경고를 찍으면
 * 로그가 의미 없이 쌓인다.
 *
 * 실행 컨텍스트: 잠들지 않는다(pci_host_bridge_dev 주석 참조).
 *
 * 호출 체인:
 *   host_bridge_whitelist() → [이 함수] → pci_host_bridge_dev()
 */
static bool __host_bridge_whitelist(struct pci_host_bridge *host,
				    bool same_host_bridge, bool warn)
{
	struct pci_dev *root = pci_host_bridge_dev(host);
	/* [한국어] 화이트리스트를 훑을 반복자 */
	const struct pci_p2pdma_whitelist_entry *entry;
	/* [한국어] 대표 장치의 ID. 매번 root-> 를 쓰지 않으려고 지역 변수에 담는다 */
	unsigned short vendor, device;

	/* [한국어] 대표 장치를 못 찾았으면 대조할 ID 가 없다 */
	if (!root)
		return false;

	/* [한국어] vendor ID */
	vendor = root->vendor;
	/* [한국어] device ID */
	device = root->device;

	/* [한국어] 표의 끝은 vendor 가 0 인 빈 항목이다. 그래서 개수를 따로 세지 않아도 된다 */
	for (entry = pci_p2pdma_whitelist; entry->vendor; entry++) {
		/* [한국어] vendor 가 다르면 볼 것도 없다 */
		if (vendor != entry->vendor)
			continue;

		/* [한국어] PCI_ANY_ID 항목(Google SoC)은 device 를 보지 않는다 */
		if (entry->device != PCI_ANY_ID && device != entry->device)
			continue;

		/* [한국어] 이 하드웨어는 같은 호스트 브리지 안에서의 P2P 만 검증되었다는 표시다.
		 * 두 장치가 서로 다른 브리지에 매달려 있으면 거절한다 */
		if (entry->flags & REQ_SAME_HOST_BRIDGE && !same_host_bridge)
			return false;

		return true;
	}

	/* [한국어] 경고를 찍기로 한 호출에서만.
	 * pci_p2pmem_find_many() 처럼 후보 대부분이 탈락하는 것이 정상인
	 * 경로에서는 이 경고를 끈다 — 안 그러면 로그가 의미 없이 쌓인다 */
	if (warn)
		/* [한국어] 어떤 호스트 브리지가 목록에 없는지 ID 로 알려 준다. 새 하드웨어를
		 * 목록에 추가하려는 사람에게 필요한 정보다 */
		pci_warn(root, "Host bridge not in P2PDMA whitelist: %04x:%04x\n",
			 vendor, device);

	return false;
}

/*
 * If we can't find a common upstream bridge take a look at the root
 * complex and compare it to a whitelist of known good hardware.
 */
/* [한국어]
 * host_bridge_whitelist - 두 장치의 호스트 브리지를 모두 확인한다
 *
 * @a: provider.  @b: client.  @warn: 경고를 찍을 것인가
 * @return: true 면 호스트 브리지를 거치는 P2P 를 허용한다
 *
 * 위 원문 영어 주석대로, 공통 상위 브리지를 찾지 못했을 때 마지막으로
 * 기대는 판정이다.
 *
 * 두 갈래다.
 *   - 같은 호스트 브리지라면 한 번만 확인하되 same_host_bridge 를 참으로
 *     넘긴다. REQ_SAME_HOST_BRIDGE 항목도 이 경우에는 통과한다.
 *   - 서로 다른 브리지라면 양쪽 모두가 목록에 있어야 하고, 둘 다
 *     same_host_bridge 를 거짓으로 넘겨 검사한다. 그래서 옛 Xeon 처럼
 *     플래그가 붙은 하드웨어는 이 경우 반드시 탈락한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   calc_map_type_and_dist() → [이 함수] → __host_bridge_whitelist()
 */
static bool host_bridge_whitelist(struct pci_dev *a, struct pci_dev *b,
				  bool warn)
{
	struct pci_host_bridge *host_a = pci_find_host_bridge(a->bus);
	/* [한국어] b 쪽 호스트 브리지 */
	struct pci_host_bridge *host_b = pci_find_host_bridge(b->bus);

	/* [한국어] 두 장치가 같은 호스트 브리지 아래에 있으면 */
	if (host_a == host_b)
		/* [한국어] 한 번만 확인하되 same_host_bridge 를 참으로 넘긴다.
		 * REQ_SAME_HOST_BRIDGE 항목도 이 경우에는 통과한다 */
		return __host_bridge_whitelist(host_a, true, warn);

	/* [한국어] 서로 다른 브리지면 양쪽 모두가 목록에 있어야 하고, 둘 다
	 * same_host_bridge 를 거짓으로 넘겨 검사한다. 그래서 플래그가 붙은
	 * 하드웨어는 이 경우 반드시 탈락한다 */
	if (__host_bridge_whitelist(host_a, false, warn) &&
	    __host_bridge_whitelist(host_b, false, warn))
		return true;

	return false;
}

/* [한국어]
 * map_types_idx - 소비자 장치를 매핑 종류 캐시의 키로 바꾼다
 *
 * @client: 소비자 PCI 장치.  @return: xarray 인덱스
 *
 * (도메인 번호 << 16) | (버스<<8 | devfn) 으로 32비트 키를 만든다.
 * BDF 만으로는 도메인이 여럿인 시스템에서 서로 다른 장치가 같은 키를
 * 갖게 되므로 도메인을 상위에 얹는다. pci_dev_id() 가 돌려주는 값이
 * 정확히 16비트라 겹치지 않는다.
 *
 * 이 키로 provider 의 xarray 에 매핑 종류를 저장해 두면, 같은 두 장치
 * 사이의 판정을 두 번 하지 않아도 된다. calc_map_type_and_dist() 는
 * 트리를 두 번 훑고 config 를 여러 번 읽는 비싼 함수라, 이 캐시가
 * 데이터 경로의 비용을 실질적으로 없앤다.
 *
 * 실행 컨텍스트: 제한 없음. 순수 계산이다.
 *
 * 호출 체인:
 *   calc_map_type_and_dist() / pci_p2pdma_map_type() → [이 함수]
 */
static unsigned long map_types_idx(struct pci_dev *client)
{
	return (pci_domain_nr(client->bus) << 16) | pci_dev_id(client);
}

/*
 * Calculate the P2PDMA mapping type and distance between two PCI devices.
 *
 * If the two devices are the same PCI function, return
 * PCI_P2PDMA_MAP_BUS_ADDR and a distance of 0.
 *
 * If they are two functions of the same device, return
 * PCI_P2PDMA_MAP_BUS_ADDR and a distance of 2 (one hop up to the bridge,
 * then one hop back down to another function of the same device).
 *
 * In the case where two devices are connected to the same PCIe switch,
 * return a distance of 4. This corresponds to the following PCI tree:
 *
 *     -+  Root Port
 *      \+ Switch Upstream Port
 *       +-+ Switch Downstream Port 0
 *       + \- Device A
 *       \-+ Switch Downstream Port 1
 *         \- Device B
 *
 * The distance is 4 because we traverse from Device A to Downstream Port 0
 * to the common Switch Upstream Port, back down to Downstream Port 1 and
 * then to Device B. The mapping type returned depends on the ACS
 * redirection setting of the ports along the path.
 *
 * If ACS redirect is set on any port in the path, traffic between the
 * devices will go through the host bridge, so return
 * PCI_P2PDMA_MAP_THRU_HOST_BRIDGE; otherwise return
 * PCI_P2PDMA_MAP_BUS_ADDR.
 *
 * Any two devices that have a data path that goes through the host bridge
 * will consult a whitelist. If the host bridge is in the whitelist, return
 * PCI_P2PDMA_MAP_THRU_HOST_BRIDGE with the distance set to the number of
 * ports per above. If the device is not in the whitelist, return
 * PCI_P2PDMA_MAP_NOT_SUPPORTED.
 */
/* [한국어]
 * calc_map_type_and_dist - 두 장치 사이의 P2P 경로를 실제로 판정한다
 *
 * @provider: P2P 메모리를 내놓는 쪽.  @client: 그것으로 DMA 하려는 쪽
 * @dist: 두 장치 사이의 홉 수가 채워진다(출력)
 * @verbose: 실패 사유와 ACS 안내를 경고로 찍을 것인가
 * @return: BUS_ADDR / THRU_HOST_BRIDGE / NOT_SUPPORTED
 *
 * 이 파일에서 가장 어려운 함수다. 바로 위 원문 영어 주석이 거리 계산의
 * 근거를 그림까지 곁들여 설명하므로 그것과 함께 읽어야 한다.
 *
 * 거리의 뜻: 같은 함수면 0, 같은 장치의 다른 함수면 2(브리지까지 한 번
 * 올라갔다 다시 내려온다), 같은 스위치 아래 두 장치면 4 다. 즉 "PCI 트리
 * 위에서 몇 번 건너뛰는가" 이며, 작을수록 가깝다.
 *
 * 판정은 두 단계다.
 *
 *   1단계 — 공통 상위 찾기. 이중 루프로, provider 에서 한 칸씩 올라가며
 *      (바깥 루프) 매번 client 에서 위로 훑어(안쪽 루프) 같은 장치를 만나는지
 *      본다. 만나면 그것이 공통 상위이고, 그때까지 올라온 횟수의 합이 거리다.
 *      끝까지 못 만나면 두 장치는 서로 다른 호스트 브리지에 있다는 뜻이므로
 *      곧바로 map_through_host_bridge 로 뛴다.
 *      바깥 루프를 도는 동안 provider 쪽 경로의 ACS redirect 를 세어 둔다.
 *      안쪽 루프에서는 세지 않는데, client 쪽 경로는 공통 상위를 찾은 뒤
 *      check_b_path_acs 에서 따로 훑기 때문이다. 그렇게 나눈 이유는 공통
 *      상위 위쪽 구간은 P2P 경로가 아니므로 세면 안 되기 때문이다.
 *
 *   2단계 — ACS 확인. 두 경로 모두에서 ACS redirect 가 하나도 없으면
 *      BUS_ADDR 이다. 하나라도 있으면 트래픽이 호스트 브리지를 지나므로
 *      map_through_host_bridge 로 내려간다. 이때 verbose 면 어느 브리지들이
 *      문제인지와 함께 "pci=disable_acs_redir=..." 부팅 인자를 그대로
 *      제시한다. acs_list 의 마지막 세미콜론을 잘라 내는 한 줄이 그 문자열을
 *      완성한다.
 *
 * map_through_host_bridge 라벨에서는 호스트 브리지 경유가 실제로 통하는지
 * 확인한다. cpu_supports_p2pdma()(AMD Zen 이상)이거나
 * host_bridge_whitelist() 를 통과하면 THRU_HOST_BRIDGE 로 남고, 아니면
 * NOT_SUPPORTED 가 된다. map_type 의 초기값이 THRU_HOST_BRIDGE 인 것이
 * 여기서 의미를 갖는다 — 이 검사만 통과하면 그 값이 그대로 쓰인다.
 *
 * done 라벨에서는 결과를 provider 의 xarray 캐시에 넣는다. 이 계산이
 * 트리를 두 번 훑고 config 를 여러 번 읽는 비싼 일이라, 두 번째부터는
 * pci_p2pdma_map_type() 이 캐시만 보고 답한다. GFP_ATOMIC 을 쓰는 이유는
 * rcu_read_lock() 구간 안이라 잠들 수 없기 때문이고, 그래서 저장이
 * 실패할 수도 있는데 그 반환값을 보지 않는다 — 실패하면 다음에 다시
 * 계산할 뿐 정확성에는 영향이 없다.
 *
 * 참조 계수: 위 원문 주석대로 pci_upstream_bridge() 가 돌려주는 장치에
 * 참조를 잡지 않는다. 호출자가 자식 장치의 참조를 쥐고 있고, 그것이
 * 상위 브리지의 참조를 이미 붙들고 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. config 읽기를 여러 번 하므로 느리다.
 *
 * 호출 체인:
 *   pci_p2pdma_map_type() / pci_p2pdma_distance_many() → [이 함수]
 *     → pci_bridge_has_acs_redir() → host_bridge_whitelist() → xa_store()
 */
static enum pci_p2pdma_map_type
calc_map_type_and_dist(struct pci_dev *provider, struct pci_dev *client,
		int *dist, bool verbose)
{
	enum pci_p2pdma_map_type map_type = PCI_P2PDMA_MAP_THRU_HOST_BRIDGE;
	/* [한국어] a 는 provider 쪽 커서, b 는 client 고정, bb 는 client 쪽 커서.
	 * 짧은 이름이지만 바로 위 원문 그림 주석의 Device A/B 와 대응한다 */
	struct pci_dev *a = provider, *b = client, *bb;
	/* [한국어] ACS 때문에 호스트 브리지를 거치게 되었는지. 아래에서 경고 출력 여부를
	 * 가르는 데 쓴다 */
	bool acs_redirects = false;
	/* [한국어] 결과를 캐시에 넣을 때 쓸 상태 구조체 */
	struct pci_p2pdma *p2pdma;
	/* [한국어] ACS redirect 가 켜진 브리지 이름을 모을 버퍼 서술자 */
	struct seq_buf acs_list;
	/* [한국어] 그 개수. 0 이면 직행 경로가 성립한다 */
	int acs_cnt = 0;
	/* [한국어] provider 쪽에서 공통 상위까지 올라간 횟수 */
	int dist_a = 0;
	/* [한국어] client 쪽에서 공통 상위까지 올라간 횟수 */
	int dist_b = 0;
	/* [한국어] 이름 목록을 담을 고정 버퍼. 128바이트를 넘으면 seq_buf 가 조용히 잘라 낸다 */
	char buf[128];

	/* [한국어] seq_buf 를 그 고정 버퍼에 묶는다. 이후 seq_buf_printf 가 넘침 없이 이어 쓴다 */
	seq_buf_init(&acs_list, buf, sizeof(buf));

	/*
	 * Note, we don't need to take references to devices returned by
	 * pci_upstream_bridge() seeing we hold a reference to a child
	 * device which will already hold a reference to the upstream bridge.
	 */
	while (a) {
		/* [한국어] 바깥 루프를 한 바퀴 돌 때마다 client 쪽 거리를 다시 센다. a 가 한 칸
		 * 올라갔으므로 이전 값이 의미를 잃기 때문이다 */
		dist_b = 0;

		/* [한국어] provider 쪽 경로의 이 브리지가 ACS redirect 를 켜 두었는가 */
		if (pci_bridge_has_acs_redir(a)) {
			/* [한국어] 그렇다면 이름을 목록에 모은다 */
			seq_buf_print_bus_devfn(&acs_list, a);
			/* [한국어] 개수도 센다 */
			acs_cnt++;
		}

		/* [한국어] 안쪽 루프는 매번 client 자신에서 다시 시작한다 */
		bb = b;

		/* [한국어] client 에서 위로 올라가며 a 와 같은 장치를 만나는지 본다 */
		while (bb) {
			/* [한국어] 만났다면 그것이 공통 상위다 */
			if (a == bb)
				goto check_b_path_acs;

			/* [한국어] 한 칸 올라간다 */
			bb = pci_upstream_bridge(bb);
			/* [한국어] 거리 하나 증가 */
			dist_b++;
		}

		/* [한국어] client 쪽 전체를 훑어도 못 만났으면 provider 쪽을 한 칸 올린다 */
		a = pci_upstream_bridge(a);
		/* [한국어] 거리 하나 증가 */
		dist_a++;
	}

	*dist = dist_a + dist_b;
	goto map_through_host_bridge;

check_b_path_acs:
	bb = b;

	/* [한국어] 공통 상위까지의 client 쪽 경로만 다시 훑어 ACS 를 센다.
	 * 바깥 루프에서 세지 않은 이유는, 공통 상위 위쪽 구간은 P2P 경로가
	 * 아니므로 세면 안 되기 때문이다 */
	while (bb) {
		/* [한국어] 공통 상위에 닿으면 멈춘다 */
		if (a == bb)
			break;

		/* [한국어] client 쪽 경로의 이 브리지가 ACS redirect 를 켜 두었는가 */
		if (pci_bridge_has_acs_redir(bb)) {
			/* [한국어] 이름을 목록에 모은다 */
			seq_buf_print_bus_devfn(&acs_list, bb);
			/* [한국어] 개수도 센다 */
			acs_cnt++;
		}

		/* [한국어] 한 칸 위로 올라가 다음 브리지를 본다.
		 * 여기서는 거리를 세지 않는다 — 거리는 위 이중 루프에서 이미 구했고,
		 * 이 루프는 ACS 만 세러 다시 훑는 것이다 */
		bb = pci_upstream_bridge(bb);
	}

	*dist = dist_a + dist_b;

	/* [한국어] 양쪽 경로 어디에도 ACS redirect 가 없다 */
	if (!acs_cnt) {
		/* [한국어] 그러면 스위치가 트랜잭션을 옆으로 곧바로 넘긴다. IOMMU 를 거치지 않고
		 * 상대의 BAR 버스 주소를 그대로 쓸 수 있다 — 가장 빠른 경로다 */
		map_type = PCI_P2PDMA_MAP_BUS_ADDR;
		goto done;
	}

	/* [한국어] 사용자에게 안내할 때만 */
	if (verbose) {
		acs_list.buffer[acs_list.len-1] = 0; /* drop final semicolon */
		pci_warn(client, "ACS redirect is set between the client and provider (%s)\n",
			 pci_name(provider));
		pci_warn(client, "to disable ACS redirect for this path, add the kernel parameter: pci=disable_acs_redir=%s\n",
			 acs_list.buffer);
	}
	/* [한국어] ACS 때문에 호스트 브리지를 거치게 되었음을 표시한다. 아래
	 * host_bridge_whitelist() 에 warn 인자로 넘어가, 목록에도 없으면
	 * 그때 다시 경고를 찍게 한다 */
	acs_redirects = true;

map_through_host_bridge:
	if (!cpu_supports_p2pdma() &&
	    !host_bridge_whitelist(provider, client, acs_redirects)) {
		/* [한국어] 안내할 때만 */
		if (verbose)
			/* [한국어] 왜 안 되는지 사람이 읽을 수 있게 알려 준다. 공통 상위도 없고
			 * 호스트 브리지도 검증되지 않았다는 뜻이다 */
			pci_warn(client, "cannot be used for peer-to-peer DMA as the client and provider (%s) do not share an upstream bridge or whitelisted host bridge\n",
				 pci_name(provider));
		/* [한국어] P2P 를 포기한다. 호출자는 일반 호스트 메모리 경로로 돌아간다 */
		map_type = PCI_P2PDMA_MAP_NOT_SUPPORTED;
	}
done:
	rcu_read_lock();
	/* [한국어] 결과를 캐시에 넣기 위해 provider 의 상태 구조체를 얻는다 */
	p2pdma = rcu_dereference(provider->p2pdma);
	/* [한국어] provider 가 정리 중이면 캐시할 곳이 없다 */
	if (p2pdma)
		/* [한국어] 판정 결과를 소비자별로 저장해 둔다. 이 계산이 트리를 두 번 훑고
		 * config 를 여러 번 읽는 비싼 일이라, 두 번째부터는
		 * pci_p2pdma_map_type() 이 캐시만 보고 답한다.
		 * GFP_ATOMIC 인 이유는 RCU 읽기 구간 안이라 잠들 수 없기 때문이고,
		 * 그래서 실패할 수도 있는데 그 반환값을 보지 않는다 — 실패하면
		 * 다음에 다시 계산할 뿐 정확성에는 영향이 없다 */
		xa_store(&p2pdma->map_types, map_types_idx(client),
			 xa_mk_value(map_type), GFP_ATOMIC);
	rcu_read_unlock();
	/* [한국어] 세 값 중 하나를 돌려준다 */
	return map_type;
}

/**
 * pci_p2pdma_distance_many - Determine the cumulative distance between
 *	a p2pdma provider and the clients in use.
 * @provider: p2pdma provider to check against the client list
 * @clients: array of devices to check (NULL-terminated)
 * @num_clients: number of clients in the array
 * @verbose: if true, print warnings for devices when we return -1
 *
 * Returns -1 if any of the clients are not compatible, otherwise returns a
 * positive number where a lower number is the preferable choice. (If there's
 * one client that's the same as the provider it will return 0, which is best
 * choice).
 *
 * "compatible" means the provider and the clients are either all behind
 * the same PCI root port or the host bridges connected to each of the devices
 * are listed in the 'pci_p2pdma_whitelist'.
 */
/* [한국어]
 * pci_p2pdma_distance_many - provider 와 소비자들 사이의 거리 총합을 잰다
 *
 * @provider: 후보 provider
 * @clients: 소비자 device 배열.  @num_clients: 그 개수
 * @verbose: 실패 사유를 경고로 찍을 것인가
 * @return: 0 이상이면 거리의 총합(작을수록 좋다), -1 이면 하나 이상이
 *          이 provider 와 P2P 를 할 수 없다
 *
 * 위 원문 영어 주석이 "compatible" 의 뜻을 정의한다 — provider 와 모든
 * 소비자가 같은 Root Port 아래에 있거나, 각자의 호스트 브리지가 모두
 * 화이트리스트에 있어야 한다.
 *
 * 소비자마다 find_parent_pci_dev() 로 조상 PCI 장치를 찾고(PCI 장치가
 * 아니면 곧바로 -1), calc_map_type_and_dist() 로 거리와 매핑 종류를 잰다.
 * 참조는 잰 직후 바로 놓는다.
 *
 * 반환값이 총합인 점에 주의한다 — 가장 나쁜 값이 아니라 모두 더한 값이다.
 * pci_p2pmem_find_many() 가 이 값이 가장 작은 provider 를 고르므로,
 * "전체적으로 가장 가까운" 후보가 뽑히게 된다.
 *
 * not_supported 를 보고도 곧바로 반환하지 않고 계속 도는 경우가 있다.
 * verbose 가 참이면 남은 소비자들의 실패 사유까지 모두 찍어 주기 위해서다.
 * verbose 가 거짓이면 첫 실패에서 루프를 깬다.
 *
 * EXPORT_SYMBOL_GPL 이지만 이 스파스 체크아웃 안에는 호출자가 없다
 * (같은 파일의 pci_p2pmem_find_many() 를 빼면).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_p2pmem_find_many() → [이 함수] → calc_map_type_and_dist()
 */
int pci_p2pdma_distance_many(struct pci_dev *provider, struct device **clients,
			     int num_clients, bool verbose)
{
	enum pci_p2pdma_map_type map;
	/* [한국어] 하나라도 경로가 성립하지 않았는가 */
	bool not_supported = false;
	/* [한국어] 소비자 device 에서 되짚어 올라간 PCI 장치 */
	struct pci_dev *pci_client;
	/* [한국어] 거리의 합. 가장 나쁜 값이 아니라 총합이다 */
	int total_dist = 0;
	/* [한국어] i = 반복자, distance = 이번 소비자의 거리 */
	int i, distance;

	/* [한국어] 소비자가 하나도 없으면 잴 것이 없다 */
	if (num_clients == 0)
		/* [한국어] -1 로 "쓸 수 없다" 를 알린다 */
		return -1;

	/* [한국어] 소비자마다 따로 잰다 */
	for (i = 0; i < num_clients; i++) {
		/* [한국어] PCI 장치가 아닐 수 있으므로 조상을 찾는다. 참조를 잡은 채로 돌아온다 */
		pci_client = find_parent_pci_dev(clients[i]);
		/* [한국어] PCI 트리 위에 없는 장치는 P2P 대상이 될 수 없다 */
		if (!pci_client) {
			/* [한국어] 안내할 때만 */
			if (verbose)
				dev_warn(clients[i],
					 "cannot be used for peer-to-peer DMA as it is not a PCI device\n");
			/* [한국어] 여기서는 참조를 놓을 것이 없다 — find_parent_pci_dev 가 NULL 을 줬으므로 */
			return -1;
		}

		/* [한국어] 실제 판정. 결과가 provider 의 캐시에도 들어간다 */
		map = calc_map_type_and_dist(provider, pci_client, &distance,
				     verbose);

		pci_dev_put(pci_client);

		/* [한국어] 하나라도 안 되면 */
		if (map == PCI_P2PDMA_MAP_NOT_SUPPORTED)
			/* [한국어] 표시해 둔다. 곧바로 반환하지 않는 이유는 아래에 있다 */
			not_supported = true;

		/* [한국어] 안내가 필요 없는 호출에서는 첫 실패에서 멈춘다. 안내가 필요하면
		 * 남은 소비자들의 실패 사유까지 모두 찍어 주려고 계속 돈다 */
		if (not_supported && !verbose)
			break;

		/* [한국어] 거리를 더한다. 이 합이 작을수록 전체적으로 가까운 provider 다 */
		total_dist += distance;
	}

	/* [한국어] 하나라도 안 되면 */
	if (not_supported)
		/* [한국어] 총합이 아무리 작아도 쓸 수 없다 */
		return -1;

	/* [한국어] 모두 성립하면 거리의 합을 돌려준다 */
	return total_dist;
}
EXPORT_SYMBOL_GPL(pci_p2pdma_distance_many);

/**
 * pci_has_p2pmem - check if a given PCI device has published any p2pmem
 * @pdev: PCI device to check
 */
/* [한국어]
 * pci_has_p2pmem - 이 장치가 P2P 메모리를 공개해 두었는가
 *
 * @pdev: 검사할 장치.  @return: true 면 남이 써도 된다고 공개된 상태다
 *
 * p2pdma 구조체가 있고, 그 안의 p2pmem_published 가 참일 때만 true 다.
 * 공개하지 않은 메모리는 등록한 드라이버 자신만 쓴다(pci_p2pmem_publish()
 * 위의 원문 주석이 그 규약을 밝힌다). NVMe 는 CMB 를 등록한 뒤
 * pci_p2pmem_publish(pdev, true) 로 공개한다.
 *
 * RCU 읽기 구간 안에서 포인터를 역참조한다 — pci_p2pdma_release() 와
 * 경쟁할 수 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들지 않는 짧은 구간이다.
 *
 * 호출 체인:
 *   pci_p2pmem_find_many() / pci_p2pdma_enable_store() → [이 함수]
 */
static bool pci_has_p2pmem(struct pci_dev *pdev)
{
	struct pci_p2pdma *p2pdma;
	/* [한국어] 결과. RCU 구간 밖에서 돌려주려고 지역 변수에 담는다 */
	bool res;

	rcu_read_lock();
	/* [한국어] RCU 읽기 구간 안에서 역참조 */
	p2pdma = rcu_dereference(pdev->p2pdma);
	/* [한국어] 구조체가 있고 공개 플래그가 서 있어야 한다 */
	res = p2pdma && p2pdma->p2pmem_published;
	rcu_read_unlock();

	/* [한국어] 구간을 벗어난 뒤 값을 돌려준다. 포인터를 밖으로 내보내지 않는 것이
	 * RCU 사용의 기본이다 */
	return res;
}

/**
 * pci_p2pmem_find_many - find a peer-to-peer DMA memory device compatible with
 *	the specified list of clients and shortest distance
 * @clients: array of devices to check (NULL-terminated)
 * @num_clients: number of client devices in the list
 *
 * If multiple devices are behind the same switch, the one "closest" to the
 * client devices in use will be chosen first. (So if one of the providers is
 * the same as one of the clients, that provider will be used ahead of any
 * other providers that are unrelated). If multiple providers are an equal
 * distance away, one will be chosen at random.
 *
 * Returns a pointer to the PCI device with a reference taken (use pci_dev_put
 * to return the reference) or NULL if no compatible device is found. The
 * found provider will also be assigned to the client list.
 */
/* [한국어]
 * pci_p2pmem_find_many - 소비자들에게 가장 가까운 provider 를 고른다
 *
 * @clients: 소비자 device 배열.  @num_clients: 그 개수
 * @return: 고른 provider(참조를 잡은 채로), 없으면 NULL
 *
 * 위 원문 영어 주석이 선택 규칙을 정한다 — 가장 가까운 것을 고르고,
 * 동점이면 그중 하나를 무작위로 고른다. 소비자 중 하나가 곧 provider 인
 * 경우 거리가 0 이라 언제나 그것이 뽑힌다.
 *
 * 왜 무작위인가: 동점 후보가 여럿일 때 항상 첫 번째를 고르면 그 장치의
 * P2P 메모리만 고갈되고 나머지는 놀게 된다. 무작위로 고르면 부하가 저절로
 * 흩어진다.
 *
 * 구현의 요점은 "지금까지의 최선" 을 모아 두는 배열이다.
 *   - 한 페이지만큼 할당해 최대 max_devs 개를 담는다. 그 이상은 무시한다.
 *   - 더 가까운 후보가 나오면 모아 둔 것들의 참조를 모두 놓고 처음부터
 *     다시 모은다.
 *   - 같은 거리면 배열에 덧붙인다.
 *   - 다 훑은 뒤 그중 하나를 get_random_u32_below() 로 뽑고, 나머지의
 *     참조를 놓는다. 뽑은 것에는 참조를 한 번 더 잡아(pci_dev_get)
 *     호출자에게 넘긴다.
 *
 * for_each_pci_dev() 는 시스템의 모든 PCI 장치를 훑으며 참조를 넘겨주는
 * 매크로다. 루프 안에서 continue 로 건너뛰어도 매크로가 참조를 알아서
 * 정리한다.
 *
 * EXPORT_SYMBOL_GPL 이지만 이 스파스 체크아웃 안에는 호출자가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. kmalloc(GFP_KERNEL)로 잠들 수 있다.
 *
 * 호출 체인:
 *   (트리 밖 소비자 드라이버) → [이 함수] → pci_p2pdma_distance_many()
 */
struct pci_dev *pci_p2pmem_find_many(struct device **clients, int num_clients)
{
	struct pci_dev *pdev = NULL;
	/* [한국어] 이번 후보까지의 거리 */
	int distance;
	/* [한국어] 지금까지 본 가장 가까운 거리. INT_MAX 로 시작해 첫 후보가 반드시
	 * 갱신하게 한다 */
	int closest_distance = INT_MAX;
	/* [한국어] 동점 후보들을 모아 둘 배열 */
	struct pci_dev **closest_pdevs;
	/* [한국어] 그 개수 */
	int dev_cnt = 0;
	/* [한국어] 한 페이지에 담을 수 있는 포인터 개수. 그 이상 동점이 나오면 무시한다 —
	 * 현실적으로 그만큼 많은 provider 가 있을 리 없고, 배열을 키우려고
	 * 재할당하는 복잡도를 감수할 이유가 없다 */
	const int max_devs = PAGE_SIZE / sizeof(*closest_pdevs);
	/* [한국어] 정리 루프용 반복자 */
	int i;

	/* [한국어] 한 페이지를 통째로 잡는다. 크기를 미리 정해 두면 재할당이 필요 없다 */
	closest_pdevs = kmalloc(PAGE_SIZE, GFP_KERNEL);
	/* [한국어] 메모리 부족이면 provider 를 찾지 못한 것으로 처리한다 */
	if (!closest_pdevs)
		return NULL;

	/* [한국어] 시스템의 모든 PCI 장치를 훑는다. 이 매크로는 참조를 잡아 넘겨주고,
	 * continue 로 건너뛰어도 다음 바퀴에서 알아서 정리한다 */
	for_each_pci_dev(pdev) {
		/* [한국어] 공개하지 않은 장치는 후보가 아니다 */
		if (!pci_has_p2pmem(pdev))
			continue;

		/* [한국어] 이 provider 와 소비자들 사이의 거리 총합. verbose 를 거짓으로 주는 이유는
		 * 후보 대부분이 탈락하는 것이 정상이라 경고를 찍으면 로그가 쌓이기 때문이다 */
		distance = pci_p2pdma_distance_many(pdev, clients,
					    num_clients, false);
		/* [한국어] 쓸 수 없거나 지금까지의 최선보다 멀면 버린다 */
		if (distance < 0 || distance > closest_distance)
			continue;

		/* [한국어] 동점인데 배열이 이미 찼으면 더 담지 않는다 */
		if (distance == closest_distance && dev_cnt >= max_devs)
			continue;

		/* [한국어] 지금까지의 최선보다 가까우면 */
		if (distance < closest_distance) {
			/* [한국어] 모아 둔 후보들의 */
			for (i = 0; i < dev_cnt; i++)
				pci_dev_put(closest_pdevs[i]);

			/* [한국어] 개수를 0 으로 되돌리고 */
			dev_cnt = 0;
			/* [한국어] 기준 거리를 갱신한다. 이제부터 이 거리와 동점인 것만 모은다 */
			closest_distance = distance;
		}

		/* [한국어] 참조를 잡아 배열에 담는다. 아래에서 하나만 남기고 나머지는 놓는다 */
		closest_pdevs[dev_cnt++] = pci_dev_get(pdev);
	}

	/* [한국어] 후보가 하나라도 있으면 */
	if (dev_cnt)
		/* [한국어] 그중 하나를 무작위로 고르고 참조를 한 번 더 잡는다. 무작위인 이유는
		 * 항상 첫 번째를 고르면 그 장치의 P2P 메모리만 고갈되고 나머지는 놀기 때문이다.
		 * pdev 를 여기서 덮어쓰는데, for_each_pci_dev 가 끝난 뒤라 NULL 이 들어
		 * 있는 상태다 */
		pdev = pci_dev_get(closest_pdevs[get_random_u32_below(dev_cnt)]);

	/* [한국어] 모아 둔 후보 전부의 */
	for (i = 0; i < dev_cnt; i++)
		pci_dev_put(closest_pdevs[i]);

	kfree(closest_pdevs);
	/* [한국어] 고른 것(참조가 하나 남아 있다) 또는 NULL 을 돌려준다 */
	return pdev;
}
EXPORT_SYMBOL_GPL(pci_p2pmem_find_many);

/**
 * pci_alloc_p2pmem - allocate peer-to-peer DMA memory
 * @pdev: the device to allocate memory from
 * @size: number of bytes to allocate
 *
 * Returns the allocated memory or NULL on error.
 */
/* [한국어]
 * pci_alloc_p2pmem - P2P 풀에서 한 조각을 떼어 낸다
 *
 * @pdev: provider 장치.  @size: 바이트 수
 * @return: 커널 가상 주소, 실패하면 NULL
 *
 * NVMe 가 SQ 링을 CMB 에 잡을 때 부르는 함수다
 * (drivers/nvme/host/pci.c:2742). 돌려주는 것은 커널 가상 주소이고,
 * 장치에 넘길 버스 주소는 pci_p2pmem_virt_to_bus() 로 따로 구한다.
 *
 * 참조 계수 다루기가 이 함수의 핵심이다.
 *   1) 위 원문 주석대로 rcu_read_lock() 은 pci_p2pdma_release() 의
 *      synchronize_rcu() 와 짝을 이뤄, 이 구간 동안 pdev->p2pdma 가
 *      살아 있음을 보장한다.
 *   2) gen_pool_alloc_owner() 로 떼어 내면서 그 구간의 owner —
 *      페이지맵의 percpu_ref — 를 함께 받는다.
 *   3) percpu_ref_tryget_live_rcu() 로 그 참조를 하나 잡는다. 실패하면
 *      페이지맵이 이미 죽어 가는 중이라는 뜻이므로, 방금 떼어 낸 것을
 *      돌려주고 NULL 을 반환한다. 이 검사가 없으면 해제 중인 BAR 위의
 *      메모리를 넘겨주게 된다.
 *
 * 잡은 참조를 여기서 놓지 않는 것에 주의한다. 그것은 pci_free_p2pmem()
 * (또는 페이지 경로라면 p2pdma_folio_free())이 놓는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. RCU 읽기 구간이라 그 안에서 잠들면
 * 안 되는데, gen_pool 할당은 자체 스핀락만 쓰므로 잠들지 않는다.
 *
 * 호출 체인:
 *   nvme_alloc_sq_cmds() [drivers/nvme/host/pci.c:2742] → [이 함수]
 *     → gen_pool_alloc_owner() → percpu_ref_tryget_live_rcu()
 */
void *pci_alloc_p2pmem(struct pci_dev *pdev, size_t size)
{
	void *ret = NULL;
	/* [한국어] gen_pool 이 함께 돌려줄 owner(페이지맵의 percpu_ref) */
	struct percpu_ref *ref;
	/* [한국어] RCU 로 보호되는 상태 구조체 */
	struct pci_p2pdma *p2pdma;

	/*
	 * Pairs with synchronize_rcu() in pci_p2pdma_release() to
	 * ensure pdev->p2pdma is non-NULL for the duration of the
	 * read-lock.
	 */
	rcu_read_lock();
	/* [한국어] 바로 위 원문 영어 주석대로, 이 읽기 구간이
	 * pci_p2pdma_release() 의 synchronize_rcu() 와 짝을 이뤄
	 * 구간 동안 pdev->p2pdma 가 살아 있음을 보장한다 */
	p2pdma = rcu_dereference(pdev->p2pdma);
	/* [한국어] 장치가 이미 정리 중이면 할당해 줄 수 없다. unlikely 는 흔한 경우가
	 * 아니라는 힌트로, 분기 예측과 코드 배치에 반영된다 */
	if (unlikely(!p2pdma))
		goto out;

	/* [한국어] 풀에서 떼어 내면서 그 구간의 owner 를 함께 받는다 */
	ret = (void *)gen_pool_alloc_owner(p2pdma->pool, size, (void **) &ref);
	/* [한국어] 풀에 남은 공간이 부족하다 */
	if (!ret)
		goto out;

	/* [한국어] 페이지맵이 아직 살아 있을 때만 참조를 잡는다. 실패하면 이미 죽어 가는
	 * 중이라는 뜻이므로, 이 검사가 없으면 해제 중인 BAR 위의 메모리를
	 * 넘겨주게 된다. _rcu 판은 RCU 읽기 구간 안이라는 전제로 더 가볍게 동작한다 */
	if (unlikely(!percpu_ref_tryget_live_rcu(ref))) {
		/* [한국어] 잡지 못했으면 방금 떼어 낸 것을 되돌린다 */
		gen_pool_free(p2pdma->pool, (unsigned long) ret, size);
		ret = NULL;
	}
out:
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(pci_alloc_p2pmem);

/**
 * pci_free_p2pmem - free peer-to-peer DMA memory
 * @pdev: the device the memory was allocated from
 * @addr: address of the memory that was allocated
 * @size: number of bytes that were allocated
 */
/* [한국어]
 * pci_free_p2pmem - pci_alloc_p2pmem() 으로 떼어 낸 조각을 돌려준다
 *
 * @pdev: provider 장치.  @addr: 받았던 커널 가상 주소.  @size: 그때의 크기
 * @return: 없음
 *
 * gen_pool_free_owner() 로 풀에 되돌리면서 그 구간의 owner(percpu_ref)를
 * 받아, pci_alloc_p2pmem() 이 잡아 둔 참조를 놓는다. 두 함수가 참조 하나를
 * 주고받는 짝이다.
 *
 * rcu_dereference_protected(pdev->p2pdma, 1) 로 락 없이 역참조한다.
 * 그것이 안전한 근거는 호출자가 이 풀에서 떼어 낸 메모리를 아직 쥐고
 * 있다는 사실이다 — 그 참조가 있는 한 페이지맵도 p2pdma 구조체도
 * 사라질 수 없다.
 *
 * NVMe 는 두 곳에서 부른다. drivers/nvme/host/pci.c:2751 은 버스 주소
 * 변환이 실패했을 때의 롤백이고, :2615 는 큐를 없앨 때의 정상 반납이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   nvme_free_queue() / nvme_alloc_sq_cmds() 의 실패 경로 → [이 함수]
 *     → gen_pool_free_owner() → percpu_ref_put()
 */
void pci_free_p2pmem(struct pci_dev *pdev, void *addr, size_t size)
{
	struct percpu_ref *ref;
	/* [한국어] 호출자가 이 풀에서 떼어 낸 메모리를 아직 쥐고 있으므로,
	 * 그 참조가 p2pdma 구조체도 붙들고 있다. 그래서 RCU 락 없이 역참조해도 된다 */
	struct pci_p2pdma *p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);

	/* [한국어] 풀에 되돌리면서 그 구간의 owner 를 받아 온다. 아래에서 그 참조를
	 * 놓아 pci_alloc_p2pmem() 이 잡은 것과 짝을 맞춘다 */
	gen_pool_free_owner(p2pdma->pool, (uintptr_t)addr, size,
			    (void **) &ref);
	percpu_ref_put(ref);
}
EXPORT_SYMBOL_GPL(pci_free_p2pmem);

/**
 * pci_p2pmem_virt_to_bus - return the PCI bus address for a given virtual
 *	address obtained with pci_alloc_p2pmem()
 * @pdev: the device the memory was allocated from
 * @addr: address of the memory that was allocated
 */
/* [한국어]
 * pci_p2pmem_virt_to_bus - 커널 가상 주소를 상대 장치가 쓸 버스 주소로 바꾼다
 *
 * @pdev: provider 장치.  @addr: pci_alloc_p2pmem() 이 준 주소
 * @return: PCI 버스 주소. addr 이 NULL 이거나 p2pdma 가 없으면 0
 *
 * NVMe 가 CMB 에 잡은 SQ 의 주소를 Create SQ 명령에 넣기 전에 부른다
 * (drivers/nvme/host/pci.c:2744). 커널이 쓰는 가상 주소는 다른 장치에게
 * 아무 의미가 없으므로, PCIe 버스에서 통하는 주소로 바꿔야 한다.
 *
 * 구현이 한 줄인 이유를 바로 위 원문 영어 주석이 밝힌다 —
 * pci_p2pdma_add_resource() 가 gen_pool 에 넣을 때 "물리 주소" 자리에
 * 일부러 PCI 버스 주소를 넣어 두었다. 그래서 gen_pool_virt_to_phys() 가
 * 이름과 달리 버스 주소를 돌려준다. 이름이 오해를 부른다는 것을 원문도
 * 인정하고 있다.
 *
 * 0 을 돌려주는 것이 오류 표시인 점에 주의한다. 호출자(NVMe)는 0 이면
 * P2P 를 포기하고 호스트 메모리로 폴백한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 위 free 와 같은 이유로 RCU 락 없이
 * 역참조한다.
 *
 * 호출 체인:
 *   nvme_alloc_sq_cmds() [drivers/nvme/host/pci.c:2744] → [이 함수]
 *     → gen_pool_virt_to_phys()
 */
pci_bus_addr_t pci_p2pmem_virt_to_bus(struct pci_dev *pdev, void *addr)
{
	struct pci_p2pdma *p2pdma;

	/* [한국어] NULL 주소는 "할당하지 않았다" 를 뜻하므로 0 을 돌려준다.
	 * 호출자(NVMe)는 0 을 실패로 보고 호스트 메모리로 폴백한다 */
	if (!addr)
		return 0;

	/* [한국어] 위 free 와 같은 이유로 락 없이 역참조한다 */
	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);
	/* [한국어] 장치가 정리 중이면 0 */
	if (!p2pdma)
		return 0;

	/*
	 * Note: when we added the memory to the pool we used the PCI
	 * bus address as the physical address. So gen_pool_virt_to_phys()
	 * actually returns the bus address despite the misleading name.
	 */
	return gen_pool_virt_to_phys(p2pdma->pool, (unsigned long)addr);
}
EXPORT_SYMBOL_GPL(pci_p2pmem_virt_to_bus);

/**
 * pci_p2pmem_alloc_sgl - allocate peer-to-peer DMA memory in a scatterlist
 * @pdev: the device to allocate memory from
 * @nents: the number of SG entries in the list
 * @length: number of bytes to allocate
 *
 * Return: %NULL on error or &struct scatterlist pointer and @nents on success
 */
/* [한국어]
 * pci_p2pmem_alloc_sgl - P2P 메모리 한 덩어리를 scatterlist 로 감싸 준다
 *
 * @pdev: provider 장치.  @nents: 성공 시 1 이 채워진다(출력)
 * @length: 바이트 수.  @return: scatterlist 포인터, 실패하면 NULL
 *
 * 커널의 많은 I/O 인터페이스가 scatterlist 를 요구한다. 이 함수는
 * pci_alloc_p2pmem() 으로 얻은 연속된 한 덩어리를 엔트리 하나짜리
 * scatterlist 로 포장해, 그런 인터페이스에 그대로 넣을 수 있게 한다.
 *
 * 엔트리가 항상 하나인 것은 gen_pool 이 연속된 구간을 돌려주기 때문이다.
 * 그래서 @nents 에는 언제나 1 이 들어간다.
 *
 * sg_init_table() 로 체인 종료 표시까지 제대로 세운 뒤 sg_set_buf() 로
 * 주소와 길이를 채운다. sg_set_buf 는 가상 주소에서 struct page 를
 * 역산하는데, 그것이 가능한 이유는 pci_p2pdma_add_resource() 가
 * devm_memremap_pages() 로 이 구간에 struct page 를 만들어 두었기 때문이다.
 *
 * 에러 경로: 메모리 할당이 실패하면 방금 잡은 sg 를 kfree 하고 NULL.
 *
 * EXPORT_SYMBOL_GPL 이지만 이 스파스 체크아웃 안에는 호출자가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. kmalloc_obj 로 잠들 수 있다.
 *
 * 호출 체인:
 *   (트리 밖 소비자 드라이버) → [이 함수] → pci_alloc_p2pmem()
 */
struct scatterlist *pci_p2pmem_alloc_sgl(struct pci_dev *pdev,
					 unsigned int *nents, u32 length)
{
	struct scatterlist *sg;
	/* [한국어] 떼어 낸 커널 가상 주소 */
	void *addr;

	/* [한국어] scatterlist 엔트리 하나를 잡는다. 크기는 대상 포인터의 타입에서 뽑는다 */
	sg = kmalloc_obj(*sg);
	/* [한국어] 메모리 부족 */
	if (!sg)
		return NULL;

	/* [한국어] 체인 종료 표시까지 제대로 세운다. 이것을 빠뜨리면 for_each_sg 가
	 * 배열 밖으로 넘어간다 */
	sg_init_table(sg, 1);

	/* [한국어] 실제 P2P 메모리를 떼어 낸다 */
	addr = pci_alloc_p2pmem(pdev, length);
	/* [한국어] 풀에 공간이 없으면 방금 잡은 sg 를 되돌린다 */
	if (!addr)
		goto out_free_sg;

	sg_set_buf(sg, addr, length);
	*nents = 1;
	return sg;

out_free_sg:
	kfree(sg);
	return NULL;
}
EXPORT_SYMBOL_GPL(pci_p2pmem_alloc_sgl);

/**
 * pci_p2pmem_free_sgl - free a scatterlist allocated by pci_p2pmem_alloc_sgl()
 * @pdev: the device to allocate memory from
 * @sgl: the allocated scatterlist
 */
/* [한국어]
 * pci_p2pmem_free_sgl - 위에서 만든 scatterlist 와 그 메모리를 모두 놓는다
 *
 * @pdev: provider 장치.  @sgl: pci_p2pmem_alloc_sgl() 이 준 scatterlist
 * @return: 없음
 *
 * 엔트리마다 sg_virt() 로 가상 주소를 되찾아 pci_free_p2pmem() 으로
 * 돌려준 뒤, scatterlist 자체를 kfree 한다.
 *
 * for_each_sg(sgl, sg, INT_MAX, count) 에 INT_MAX 를 주고 sg 가 NULL 이
 * 되면 깨는 방식이 눈에 띈다. 엔트리 수를 인자로 받지 않으므로, 체인
 * 종료 표시를 만날 때까지 도는 것이다. 실제로는 alloc 쪽이 항상 하나만
 * 만들므로 한 바퀴에 끝난다.
 *
 * EXPORT_SYMBOL_GPL 이지만 이 스파스 체크아웃 안에는 호출자가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (트리 밖 소비자 드라이버) → [이 함수] → pci_free_p2pmem()
 */
void pci_p2pmem_free_sgl(struct pci_dev *pdev, struct scatterlist *sgl)
{
	struct scatterlist *sg;
	/* [한국어] 훑은 엔트리 수. 여기서는 쓰지 않지만 매크로가 요구한다 */
	int count;

	/* [한국어] 엔트리 수를 인자로 받지 않으므로 INT_MAX 를 주고 체인 끝까지 돈다.
	 * 실제로는 alloc 쪽이 항상 하나만 만들므로 한 바퀴에 끝난다 */
	for_each_sg(sgl, sg, INT_MAX, count) {
		/* [한국어] 체인 종료 표시를 만나면 */
		if (!sg)
			break;

		/* [한국어] 각 엔트리의 메모리를 풀에 돌려준다. sg_virt 는 페이지와 오프셋에서
		 * 커널 가상 주소를 되살린다 */
		pci_free_p2pmem(pdev, sg_virt(sg), sg->length);
	}
	kfree(sgl);
}
EXPORT_SYMBOL_GPL(pci_p2pmem_free_sgl);

/**
 * pci_p2pmem_publish - publish the peer-to-peer DMA memory for use by
 *	other devices with pci_p2pmem_find()
 * @pdev: the device with peer-to-peer DMA memory to publish
 * @publish: set to true to publish the memory, false to unpublish it
 *
 * Published memory can be used by other PCI device drivers for
 * peer-2-peer DMA operations. Non-published memory is reserved for
 * exclusive use of the device driver that registers the peer-to-peer
 * memory.
 */
/* [한국어]
 * pci_p2pmem_publish - 이 장치의 P2P 메모리를 남에게 공개할지 정한다
 *
 * @pdev: provider 장치.  @publish: true = 공개, false = 비공개
 * @return: 없음
 *
 * 위 원문 영어 주석이 규약을 정한다 — 공개된 메모리는 다른 PCI 드라이버가
 * P2P 에 쓸 수 있고, 공개하지 않은 메모리는 등록한 드라이버 전용이다.
 *
 * NVMe 는 CMB 를 등록한 직후 true 로 부른다
 * (drivers/nvme/host/pci.c:3238). 그래야 RDMA NIC 같은 다른 장치가
 * pci_p2pmem_find_many() 로 이 CMB 를 찾아낼 수 있다.
 *
 * 플래그 하나를 세우는 것이 전부다. 공개하지 않아도 등록한 드라이버
 * 자신은 pci_alloc_p2pmem() 으로 얼마든지 쓸 수 있다 — 그 함수는
 * published 를 보지 않는다. 이 플래그를 보는 곳은 pci_has_p2pmem() 과
 * sysfs 의 published 속성뿐이다.
 *
 * (기존 주석은 이 함수를 "nvme_setup_pci_p2pdma() 에서 호출" 이라 적었으나
 *  그런 이름의 함수는 drivers/nvme 에 없다. 실제 호출부는 nvme_map_cmb() 다.)
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   nvme_map_cmb() [drivers/nvme/host/pci.c:3238] → [이 함수]
 */
void pci_p2pmem_publish(struct pci_dev *pdev, bool publish)
{
	struct pci_p2pdma *p2pdma;

	rcu_read_lock();
	/* [한국어] RCU 읽기 구간 안에서 역참조 */
	p2pdma = rcu_dereference(pdev->p2pdma);
	/* [한국어] 장치가 정리 중이면 세울 곳이 없다 */
	if (p2pdma)
		/* [한국어] 공개 여부를 바꾼다. 대입 하나뿐이라 락이 없다 — 잘못 읽어도 후보
		 * 선정이 한 번 어긋날 뿐이고, 실제 사용 가능 여부는 경로 판정이 다시 거른다 */
		p2pdma->p2pmem_published = publish;
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(pci_p2pmem_publish);

/**
 * pci_p2pdma_map_type - Determine the mapping type for P2PDMA transfers
 * @provider: P2PDMA provider structure
 * @dev: Target device for the transfer
 *
 * Determines how peer-to-peer DMA transfers should be mapped between
 * the provider and the target device. The mapping type indicates whether
 * the transfer can be done directly through PCI switches or must go
 * through the host bridge.
 */
/* [한국어]
 * pci_p2pdma_map_type - provider 와 대상 장치 사이의 매핑 종류를 돌려준다
 *
 * @provider: P2P 메모리를 내놓은 쪽의 서술자
 * @dev: 그 메모리로 DMA 를 하려는 대상 장치
 * @return: PCI_P2PDMA_MAP_BUS_ADDR(직행) /
 *          PCI_P2PDMA_MAP_THRU_HOST_BRIDGE(호스트 브리지 경유) /
 *          PCI_P2PDMA_MAP_NOT_SUPPORTED(불가)
 *
 * 데이터 경로에서 불리는 함수라 빠르게 답하는 것이 중요하다. 그래서
 * 먼저 provider 의 xarray 캐시를 본다. 캐시에 있으면 그것으로 끝이고,
 * PCI_P2PDMA_MAP_UNKNOWN(캐시에 없을 때 xa_load 가 0 을 주고
 * xa_to_value 가 그것을 이 값으로 옮긴다)이면 그제야
 * calc_map_type_and_dist() 로 계산한다. 계산 결과는 그 함수가 캐시에
 * 넣어 두므로 다음부터는 다시 계산하지 않는다.
 *
 * 세 매핑 종류의 뜻:
 *   BUS_ADDR          - 두 장치 사이에 직행 경로가 있다. IOMMU 를 거치지
 *                       않고 상대의 BAR 버스 주소를 그대로 쓴다.
 *   THRU_HOST_BRIDGE  - 트래픽이 호스트 브리지를 지난다. 일반 DMA 매핑
 *                       경로를 쓰되 대상이 RAM 이 아니라 MMIO 라
 *                       캐시 동기화를 건너뛰어야 한다
 *                       (block/blk-mq-dma.c:463 이 그 처리를 한다).
 *   NOT_SUPPORTED     - 경로가 성립하지 않는다. 호출자는 P2P 를 포기한다.
 *
 * 앞의 두 검사가 방어적이다. pdev->p2pdma 가 아예 없거나 대상이 PCI
 * 장치가 아니면 곧바로 NOT_SUPPORTED 다. 첫 검사는 RCU 락 밖에서 하는데,
 * 아래 락 안의 재검사가 실제 판정이고 이것은 흔한 경우를 빨리 걸러 내는
 * 지름길이다.
 *
 * calc_map_type_and_dist() 를 verbose=true 로 부르는 점에 주의한다.
 * 처음 판정할 때 한 번은 사용자에게 왜 안 되는지(어느 브리지의 ACS 를
 * 꺼야 하는지) 알려 주자는 것이고, 캐시 덕분에 그 경고가 되풀이되지 않는다.
 *
 * 실행 컨텍스트: DMA 매핑 경로. 캐시 적중이면 잠들지 않는다.
 *
 * 호출 체인:
 *   __pci_p2pdma_update_state() → [이 함수] → calc_map_type_and_dist()
 */
enum pci_p2pdma_map_type pci_p2pdma_map_type(struct p2pdma_provider *provider,
					     struct device *dev)
{
	enum pci_p2pdma_map_type type = PCI_P2PDMA_MAP_NOT_SUPPORTED;
	/* [한국어] provider 서술자의 owner 에서 pci_dev 를 되찾는다 */
	struct pci_dev *pdev = to_pci_dev(provider->owner);
	/* [한국어] 대상 장치를 pci_dev 로 본 것 */
	struct pci_dev *client;
	/* [한국어] 캐시를 담고 있는 상태 구조체 */
	struct pci_p2pdma *p2pdma;
	/* [한국어] calc_map_type_and_dist 가 요구하는 출력 인자. 여기서는 거리를 쓰지 않지만
	 * 버릴 곳이 필요하다 */
	int dist;

	/* [한국어] P2PDMA 가 아예 초기화되지 않은 장치. RCU 락 밖의 이 검사는 흔한 경우를
	 * 빨리 걸러 내는 지름길이고, 실제 판정은 아래 락 안의 재검사가 한다 */
	if (!pdev->p2pdma)
		/* [한국어] P2P 불가 */
		return PCI_P2PDMA_MAP_NOT_SUPPORTED;

	/* [한국어] 대상이 PCI 장치가 아니면 PCI 트리 위에서 경로를 잴 수 없다 */
	if (!dev_is_pci(dev))
		/* [한국어] P2P 불가 */
		return PCI_P2PDMA_MAP_NOT_SUPPORTED;

	/* [한국어] 이제 안전하게 캐스팅한다 */
	client = to_pci_dev(dev);

	rcu_read_lock();
	/* [한국어] RCU 읽기 구간 안에서 상태 구조체를 얻는다 */
	p2pdma = rcu_dereference(pdev->p2pdma);

	/* [한국어] 정리 중이 아니면 */
	if (p2pdma)
		/* [한국어] 캐시를 본다. 없는 키를 읽으면 0 이 나오고 xa_to_value 가 그것을
		 * PCI_P2PDMA_MAP_UNKNOWN 으로 옮겨 준다 — 그 관례 덕분에 별도
		 * 존재 표시가 필요 없다 */
		type = xa_to_value(xa_load(&p2pdma->map_types,
					   map_types_idx(client)));
	rcu_read_unlock();

	/* [한국어] 캐시에 없었다는 뜻이다 */
	if (type == PCI_P2PDMA_MAP_UNKNOWN)
		/* [한국어] 그제야 비싼 계산을 한다. verbose 를 참으로 주는 이유는, 처음 판정할 때
		 * 한 번은 사용자에게 왜 안 되는지(어느 브리지의 ACS 를 꺼야 하는지)
		 * 알려 주자는 것이고, 캐시 덕분에 그 경고가 되풀이되지 않기 때문이다 */
		return calc_map_type_and_dist(pdev, client, &dist, true);

	return type;
}

/* [한국어]
 * __pci_p2pdma_update_state - 반복자의 P2P 상태를 이 페이지에 맞춰 갱신한다
 *
 * @state: 매핑 반복자가 들고 다니는 P2P 상태(mem 과 map)
 * @dev: DMA 를 수행할 장치
 * @page: 이번에 처리할 페이지.  @return: 없음
 *
 * 한 요청 안의 페이지들이 모두 같은 provider 에서 왔다는 보장이 없으므로,
 * 페이지를 볼 때마다 provider 가 바뀌었는지 확인해야 한다. 다만 대부분은
 * 바뀌지 않으므로, 같으면 곧바로 돌아서는 것이 이 함수의 첫 줄이다.
 * 바뀌었을 때만 pci_p2pdma_map_type() 을 부른다.
 *
 * 이 함수 이름 앞의 밑줄 둘은 "인라인 래퍼가 따로 있다" 는 표시다.
 * 실제 호출자는 include/linux/pci-p2pdma.h 의 pci_p2pdma_state() 인라인이고
 * (그 헤더는 이 스파스 체크아웃에 없다), 그것을 부르는 곳은
 * block/blk-mq-dma.c:673 의 blk_dma_map_iter_start() 다.
 * 흔한 경우(P2P 페이지가 아님)를 헤더 인라인에서 걸러 내고, 실제 판정이
 * 필요할 때만 이 함수로 넘어오는 구조다.
 *
 * 실행 컨텍스트: 블록 I/O 매핑 경로. 캐시가 채워진 뒤에는 잠들지 않는다.
 *
 * 호출 체인:
 *   blk_dma_map_iter_start() [block/blk-mq-dma.c:673]
 *     → pci_p2pdma_state()(헤더 인라인) → [이 함수] → pci_p2pdma_map_type()
 */
void __pci_p2pdma_update_state(struct pci_p2pdma_map_state *state,
		struct device *dev, struct page *page)
{
	struct pci_p2pdma_pagemap *p2p_pgmap = to_p2p_pgmap(page_pgmap(page));

	/* [한국어] 직전 페이지와 같은 provider 면 다시 판정할 것이 없다.
	 * 한 요청 안의 페이지들은 대개 같은 곳에서 오므로 이 지름길이 거의 항상 걸린다 */
	if (state->mem == p2p_pgmap->mem)
		return;

	/* [한국어] 바뀌었으면 새 provider 를 기록하고 */
	state->mem = p2p_pgmap->mem;
	/* [한국어] 매핑 종류를 다시 판정한다. 캐시가 채워져 있으면 이것도 곧 끝난다 */
	state->map = pci_p2pdma_map_type(p2p_pgmap->mem, dev);
}

/**
 * pci_p2pdma_enable_store - parse a configfs/sysfs attribute store
 *		to enable p2pdma
 * @page: contents of the value to be stored
 * @p2p_dev: returns the PCI device that was selected to be used
 *		(if one was specified in the stored value)
 * @use_p2pdma: returns whether to enable p2pdma or not
 *
 * Parses an attribute value to decide whether to enable p2pdma.
 * The value can select a PCI device (using its full BDF device
 * name) or a boolean (in any format kstrtobool() accepts). A false
 * value disables p2pdma, a true value expects the caller
 * to automatically find a compatible device and specifying a PCI device
 * expects the caller to use the specific provider.
 *
 * pci_p2pdma_enable_show() should be used as the show operation for
 * the attribute.
 *
 * Returns 0 on success
 */
/* [한국어]
 * pci_p2pdma_enable_store - "P2PDMA 를 쓸까" 설정 문자열을 해석한다
 *
 * @page: 사용자가 쓴 문자열
 * @p2p_dev: 특정 장치를 지정했으면 그 pci_dev 가 채워진다(출력, 참조를 잡은 채)
 * @use_p2pdma: P2PDMA 를 쓸 것인지가 채워진다(출력)
 * @return: 0 = 해석 성공, -ENODEV = 그런 장치가 없거나 P2P 메모리가 없다
 *
 * 위 원문 영어 주석이 받아들이는 값의 형식을 정한다. 세 가지다.
 *   - BDF 이름("0000:01:00.0") : 그 장치를 provider 로 쓴다.
 *   - 참("1", "y", "on" 등)    : 쓰되 provider 는 호출자가 알아서 찾는다.
 *   - 거짓("0", "n", "off" 등) : 쓰지 않는다.
 *
 * 가운데 else-if 갈래가 흥미롭다. 본문이 비어 있고 주석만 있는데, 그
 * 주석이 이유를 밝힌다 — 사용자가 존재하지 않는 BDF("0000:01:00.1")를
 * 입력했을 때 kstrtobool 이 첫 글자 '0' 만 보고 거짓으로 해석해 버리면,
 * 사용자는 장치를 지정했다고 믿는데 실제로는 P2PDMA 가 꺼진다. 그래서
 * '0' 이나 '1' 로 시작하면서 두 글자 이상인 입력은 불리언 해석에서
 * 제외하고 아래 오류 경로로 떨어뜨린다. iscntrl(page[1]) 검사가
 * "한 글자 뒤에 개행뿐인가" 를 보는 부분이다.
 *
 * BDF 로 찾았더라도 그 장치가 P2P 메모리를 공개하지 않았으면 -ENODEV 다.
 * 그때는 잡았던 참조를 놓고 나간다.
 *
 * EXPORT_SYMBOL_GPL 이지만 이 스파스 체크아웃 안에는 호출자가 없다
 * (원래는 NVMe target 의 configfs 가 쓴다).
 *
 * 실행 컨텍스트: sysfs/configfs write 의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (트리 밖 configfs 속성) → [이 함수] → bus_find_device_by_name() → pci_has_p2pmem()
 */
int pci_p2pdma_enable_store(const char *page, struct pci_dev **p2p_dev,
			    bool *use_p2pdma)
{
	struct device *dev;

	/* [한국어] "0000:01:00.0" 같은 이름으로 PCI 장치를 찾는다. 찾으면 참조를 잡아 준다 */
	dev = bus_find_device_by_name(&pci_bus_type, NULL, page);
	if (dev) {
		*use_p2pdma = true;
		*p2p_dev = to_pci_dev(dev);

		/* [한국어] 찾았더라도 그 장치가 P2P 메모리를 공개하지 않았으면 쓸 수 없다 */
		if (!pci_has_p2pmem(*p2p_dev)) {
			pci_err(*p2p_dev,
				"PCI device has no peer-to-peer memory: %s\n",
				page);
			pci_dev_put(*p2p_dev);
			return -ENODEV;
		}

		return 0;
	} else if ((page[0] == '0' || page[0] == '1') && !iscntrl(page[1])) {
		/*
		 * If the user enters a PCI device that  doesn't exist
		 * like "0000:01:00.1", we don't want kstrtobool to think
		 * it's a '0' when it's clearly not what the user wanted.
		 * So we require 0's and 1's to be exactly one character.
		 */
	} else if (!kstrtobool(page, use_p2pdma)) {
		return 0;
	}

	/* [한국어] 위 어느 형식에도 맞지 않는 입력. strcspn(page, "\n") 으로 개행 앞까지만
	 * 찍어, 사용자가 친 그대로를 한 줄로 보여 준다 */
	pr_err("No such PCI device: %.*s\n", (int)strcspn(page, "\n"), page);
	return -ENODEV;
}
EXPORT_SYMBOL_GPL(pci_p2pdma_enable_store);

/**
 * pci_p2pdma_enable_show - show a configfs/sysfs attribute indicating
 *		whether p2pdma is enabled
 * @page: contents of the stored value
 * @p2p_dev: the selected p2p device (NULL if no device is selected)
 * @use_p2pdma: whether p2pdma has been enabled
 *
 * Attributes that use pci_p2pdma_enable_store() should use this function
 * to show the value of the attribute.
 *
 * Returns 0 on success
 */
/* [한국어]
 * pci_p2pdma_enable_show - 위 설정의 현재 값을 문자열로 돌려준다
 *
 * @page: 출력 버퍼.  @p2p_dev: 선택된 provider(없으면 NULL)
 * @use_p2pdma: P2PDMA 를 쓰는 상태인가
 * @return: 버퍼에 쓴 바이트 수
 *
 * pci_p2pdma_enable_store() 가 받아들이는 형식 그대로 되돌려 준다.
 * 그래서 읽은 값을 다시 써 넣어도 같은 설정이 된다.
 *   "0"   - 쓰지 않음
 *   "1"   - 쓰되 provider 자동 선택
 *   BDF   - 그 장치를 provider 로 지정
 *
 * EXPORT_SYMBOL_GPL 이지만 이 스파스 체크아웃 안에는 호출자가 없다.
 *
 * 실행 컨텍스트: sysfs/configfs read 의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (트리 밖 configfs 속성) → [이 함수]
 */
ssize_t pci_p2pdma_enable_show(char *page, struct pci_dev *p2p_dev,
			       bool use_p2pdma)
{
	if (!use_p2pdma)
		/* [한국어] 쓰지 않는 상태 */
		return sprintf(page, "0\n");

	/* [한국어] 쓰되 provider 를 지정하지 않은 상태 */
	if (!p2p_dev)
		/* [한국어] "1" 로 돌려준다. 이 값을 그대로 다시 써 넣어도 같은 설정이 된다 */
		return sprintf(page, "1\n");

	/* [한국어] 지정한 provider 가 있으면 그 BDF 이름을 돌려준다. store 가 받아들이는
	 * 형식과 같으므로 읽고 쓰기가 대칭이 된다 */
	return sprintf(page, "%s\n", pci_name(p2p_dev));
}
EXPORT_SYMBOL_GPL(pci_p2pdma_enable_show);
