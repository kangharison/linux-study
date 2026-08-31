/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cadence PCIe Host controller driver.
 *
 * Copyright (c) 2017 Cadence
 * Author: Cyrille Pitchen <cyrille.pitchen@free-electrons.com>
 */
/*
 * [한국어 설명] 구형·신형 호스트 코드가 공유하는 선언 (pcie-cadence-host-common.h)
 *
 * === 파일의 역할 ===
 * Cadence IP 의 호스트(RC) 모드 구현이 두 벌이다 — 구형 레지스터 배치를
 * 쓰는 pcie-cadence-host.c 와 신형을 쓰는 pcie-cadence-host-hpa.c.
 * 그런데 두 벌이 하는 일 중 상당수는 레지스터 배치와 무관하다.
 * 링크가 올라오기를 기다리고, 재트레이닝을 걸고, DMA 범위를 BAR 에
 * 배정할 크기별로 정렬하는 일 같은 것이다.
 *
 * 그 공통 부분을 pcie-cadence-host-common.c 로 뽑아냈고, 이 헤더가
 * 그 선언을 담는다.
 *
 * 공통화의 핵심 기법이 함수 포인터 타입 둘이다. 레지스터를 실제로
 * 만지는 부분만 호출자가 넘기게 하면, 나머지 논리는 한 벌로 충분해진다.
 *   cdns_pcie_linkup_func — 링크 상태를 읽는 방법. 구형은
 *     cdns_pcie_linkup(), 신형은 cdns_pcie_hpa_link_up() 을 넘긴다.
 *   cdns_pcie_host_bar_ib_cfg — 인바운드 BAR 를 설정하는 방법.
 * 이 덕에 대기 루프나 BAR 배정 알고리즘 같은 복잡한 부분을 두 번
 * 쓰지 않아도 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pcie-cadence-host.c (구형)     ─┐
 *                                 ├─> [이 헤더] ─> pcie-cadence-host-common.c
 * pcie-cadence-host-hpa.c (신형) ─┘      의 공통 구현
 *                                        (호출자가 넘긴 함수 포인터로
 *                                         레지스터 접근 부분만 갈라진다)
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더를 include 하는 곳: 위 두 호스트 파일과 구현 파일.
 * 의존하는 정의: struct cdns_pcie_rc 와 enum cdns_pcie_rp_bar 는
 *   pcie-cadence.h 에 있다. 이 헤더가 그것을 직접 include 하지 않는데,
 *   호출자가 먼저 pcie-cadence.h 를 포함할 것을 전제한 것으로 보인다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 여기 선언된 함수를 부르지 않는다(전수 확인).
 * 이 헤더가 다루는 BAR 크기 배정은 호스트 브리지가 자기 인바운드
 * 창을 여는 문제이며, 그 아래에 붙는 NVMe 의 BAR 와는 다른 층위다.
 *
 * === 주요 함수/구조체 요약 ===
 * bar_max_size[]           : BAR 번호별 최대 크기 표. 구현 파일에 정의.
 * cdns_pcie_linkup_func    : 링크 상태 읽기 함수 포인터 타입.
 * cdns_pcie_host_bar_ib_cfg: 인바운드 BAR 설정 함수 포인터 타입.
 * cdns_pcie_host_training_complete() : 트레이닝이 끝났는지 확인.
 * cdns_pcie_host_wait_for_link()     : 링크를 기다린다.
 * cdns_pcie_retrain()                : 재트레이닝을 건다.
 * cdns_pcie_host_start_link()        : 링크를 올린다.
 * cdns_pcie_host_find_min_bar() / _find_max_bar() : 주어진 크기에 맞는
 *                            BAR 를 고른다.
 * cdns_pcie_host_dma_ranges_cmp()    : DMA 범위를 정렬하는 비교 함수.
 * cdns_pcie_host_bar_ib_config()     : 구형 판의 인바운드 BAR 설정.
 * cdns_pcie_host_bar_config()        : 범위 하나를 BAR 에 배정한다.
 * cdns_pcie_host_map_dma_ranges()    : dma-ranges 전체를 BAR 들에 배정.
 */

/* [한국어] 헤더 중복 포함 방지 가드. */
#ifndef _PCIE_CADENCE_HOST_COMMON_H
#define _PCIE_CADENCE_HOST_COMMON_H

/* [한국어] 기본 커널 매크로. */
#include <linux/kernel.h>
/* [한국어] struct resource_entry 와 PCI 관련 타입. 아래
 * cdns_pcie_host_bar_config() 의 인자에 쓰인다. */
#include <linux/pci.h>

/* [한국어] BAR 번호별로 이 IP 가 지원하는 최대 크기를 담은 표.
 * 정의는 pcie-cadence-host-common.c 에 있고, 두 호스트 파일이
 * BAR 를 고를 때 참조한다.
 * extern 인 것은 배열 실체가 구현 파일에 하나만 있기 때문이다. */
extern u64 bar_max_size[];

/* [한국어] 인바운드 BAR 를 설정하는 함수의 형태.
 * 구형과 신형이 레지스터를 다르게 만지므로, 공통 코드가 이 타입의
 * 포인터를 받아 그때그때 맞는 구현을 부른다.
 * 인자는 순서대로 컨트롤러, BAR 번호, CPU 주소, 크기, 자원 플래그다.
 * 이름을 붙이지 않은 것은 커널 코드에서 흔한 축약 표기다. */
typedef int (*cdns_pcie_host_bar_ib_cfg)(struct cdns_pcie_rc *,
					 enum cdns_pcie_rp_bar,
					 u64,
					 u64,
					 unsigned long);
/* [한국어] 링크 상태를 읽는 함수의 형태.
 * 구형은 cdns_pcie_linkup(), 신형은 cdns_pcie_hpa_link_up() 이 이
 * 형태를 만족한다. 대기 루프가 이 포인터를 받아 반복 호출한다. */
typedef bool (*cdns_pcie_linkup_func)(struct cdns_pcie *);

/* [한국어] cdns_pcie_host_training_complete - 링크 트레이닝이 끝났는지 확인한다.
 * 링크가 "올라왔는가"(linkup)와는 다른 물음이다. 트레이닝이 진행 중인
 * 동안에도 링크 비트가 잠깐 서는 경우가 있어, 협상이 완전히 끝났는지를
 * 따로 확인해야 속도와 폭이 확정된 상태를 보장할 수 있다.
 * 이 함수는 함수 포인터를 받지 않는데, 확인하는 레지스터가 구형·신형
 * 공통이기 때문으로 보인다 — 그 근거를 이 트리에서 확인하지는 못했다. */
int cdns_pcie_host_training_complete(struct cdns_pcie *pcie);
/* [한국어] cdns_pcie_host_wait_for_link - 링크가 올라올 때까지 기다린다.
 * 두 번째 인자로 링크 상태를 읽는 방법을 받아, 구형·신형 모두에서
 * 같은 대기 루프를 쓸 수 있게 한다. 타임아웃이 있어 영원히 기다리지 않는다. */
int cdns_pcie_host_wait_for_link(struct cdns_pcie *pcie,
				 cdns_pcie_linkup_func pcie_link_up);
/* [한국어] cdns_pcie_retrain - 링크 재트레이닝을 건다. 링크가 올라왔지만
 * 속도나 폭이 기대에 못 미칠 때 다시 협상하게 하는 데 쓴다. */
int cdns_pcie_retrain(struct cdns_pcie *pcie, cdns_pcie_linkup_func pcie_linkup_func);
/* [한국어] cdns_pcie_host_start_link - 링크 트레이닝을 시작하고 완료를 기다린다.
 * 위 두 함수를 묶은 상위 진입점이다. */
int cdns_pcie_host_start_link(struct cdns_pcie_rc *rc,
			      cdns_pcie_linkup_func pcie_link_up);
/* [한국어] 반환 타입이 줄을 따로 차지한다. 함수 이름이 길어 커널 스타일에 맞춰
 * 줄을 나눈 것이다. */
enum cdns_pcie_rp_bar
/* [한국어] cdns_pcie_host_find_min_bar - 주어진 크기를 담을 수 있는 BAR 중
 * 가장 작은 것을 고른다. 큰 BAR 를 아껴 두려는 배정 전략이다. */
cdns_pcie_host_find_min_bar(struct cdns_pcie_rc *rc, u64 size);
/* [한국어] 여기도 반환 타입이 별도 줄이다. */
enum cdns_pcie_rp_bar
/* [한국어] cdns_pcie_host_find_max_bar - 반대로 가장 큰 BAR 를 고른다.
 * 큰 범위를 한 BAR 로 덮어야 할 때 쓴다. */
cdns_pcie_host_find_max_bar(struct cdns_pcie_rc *rc, u64 size);
/* [한국어] cdns_pcie_host_dma_ranges_cmp - 디바이스 트리의 dma-ranges 항목들을
 * 정렬할 때 쓰는 비교 함수. list_sort 에 넘겨진다.
 * 크기 순으로 정렬해야 큰 범위부터 큰 BAR 에 배정할 수 있다. */
int cdns_pcie_host_dma_ranges_cmp(void *priv, const struct list_head *a,
				  const struct list_head *b);
/* [한국어] cdns_pcie_host_bar_ib_config - 구형 레지스터 배치에서 인바운드 BAR 를
 * 설정한다. 위 cdns_pcie_host_bar_ib_cfg 타입을 만족하며,
 * 구형 호스트 파일이 이것을 공통 코드에 넘긴다. */
int cdns_pcie_host_bar_ib_config(struct cdns_pcie_rc *rc,
				 enum cdns_pcie_rp_bar bar,
				 u64 cpu_addr,
				 u64 size,
				 unsigned long flags);
/* [한국어] cdns_pcie_host_bar_config - dma-ranges 항목 하나를 적당한 BAR 에
 * 배정한다. 마지막 인자로 실제 레지스터 설정 방법을 받아
 * 구형·신형 공통으로 쓴다. */
int cdns_pcie_host_bar_config(struct cdns_pcie_rc *rc,
			      struct resource_entry *entry,
			      cdns_pcie_host_bar_ib_cfg pci_host_ib_config);
/* [한국어] cdns_pcie_host_map_dma_ranges - dma-ranges 전체를 순회하며 BAR 들에
 * 배정한다. 호스트 초기화에서 부르는 상위 진입점이다. */
int cdns_pcie_host_map_dma_ranges(struct cdns_pcie_rc *rc,
				  cdns_pcie_host_bar_ib_cfg pci_host_ib_config);

#endif /* _PCIE_CADENCE_HOST_COMMON_H */
