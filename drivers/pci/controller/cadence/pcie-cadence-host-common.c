// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence PCIe host controller library.
 *
 * Copyright (c) 2017 Cadence
 * Author: Cyrille Pitchen <cyrille.pitchen@free-electrons.com>
 */
/*
 * [한국어 설명] 구형·신형 호스트가 공유하는 링크·BAR 논리 (pcie-cadence-host-common.c)
 *
 * === 파일의 역할 ===
 * Cadence 호스트 구현이 두 벌(구형 pcie-cadence-host.c, 신형
 * pcie-cadence-host-hpa.c)인데, 그중 레지스터 배치와 무관한 부분을
 * 뽑아 놓은 파일이다. 크게 두 가지를 다룬다.
 *
 * 1) 링크 트레이닝 관리
 *    링크가 올라오기를 기다리고, 필요하면 재트레이닝을 건다.
 *    재트레이닝이 왜 필요한가 — 일부 하드웨어에 Gen2 트레이닝 결함이
 *    있어 처음에 2.5GT/s(Gen1)로만 협상되는 경우가 있다. 그때
 *    Retrain Link 비트를 세워 다시 협상시키면 제 속도가 나온다.
 *    아래 cdns_pcie_retrain() 이 그 처리이며, quirk 플래그가 켜진
 *    보드에서만 동작한다.
 *
 * 2) 인바운드 BAR 배정
 *    호스트 브리지도 BAR 를 갖는다. 엔드포인트의 BAR 와 방향이 반대인
 *    "인바운드" BAR 로, 저쪽에서 들어오는 DMA 가 이쪽 메모리의 어디에
 *    닿을지를 정한다.
 *
 *    디바이스 트리의 dma-ranges 가 "PCIe 쪽 주소 이 범위를 CPU 쪽
 *    주소 저기에 대응시켜라" 를 여러 개 적어 두는데, 그것들을 한정된
 *    수의 BAR 에 나눠 담아야 한다. BAR 마다 담을 수 있는 최대 크기가
 *    달라서(아래 bar_max_size) 그냥 순서대로 넣으면 낭비가 생긴다.
 *
 *    그래서 크기 순으로 정렬한 뒤(dma_ranges_cmp) 큰 것부터 배정하고,
 *    각 범위에는 그것을 담을 수 있는 가장 작은 BAR 를 고른다
 *    (find_min_bar). 큰 BAR 를 함부로 쓰지 않고 아껴 두는 전략이다.
 *
 * 공통화는 함수 포인터로 이뤄진다. 레지스터를 실제로 만지는 두 가지
 * (링크 상태 읽기, 인바운드 BAR 설정)만 호출자가 넘기고, 나머지 논리는
 * 이 파일 한 벌로 충분하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pcie-cadence-host.c (구형) / pcie-cadence-host-hpa.c (신형)
 *   -> cdns_pcie_host_start_link()   [이 파일] 링크를 올린다
 *   -> cdns_pcie_host_map_dma_ranges() [이 파일] dma-ranges 를 BAR 에 배정
 *      -> 호출자가 넘긴 ib_config 함수로 실제 레지스터를 쓴다
 *
 * 실행 컨텍스트: 전부 호스트 초기화 중 프로세스 컨텍스트.
 *   대기 루프에 usleep_range 가 있어 잠든다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 두 호스트 파일.
 * 아래쪽: pcie-cadence.h 의 레지스터 접근자, 커널의 list_sort,
 *   디바이스 트리 자원 파싱.
 * 공유 상태: struct cdns_pcie_rc 의 avail_ib_bar[] — 어느 BAR 가
 *   아직 비어 있는지를 나타내는 표. 이 파일이 배정하며 소비한다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * 다만 이 파일이 다루는 문제가 NVMe 성능과 무관하지는 않다. 인바운드
 * BAR 가 호스트 메모리 전체를 덮지 못하면, 그 밖에 있는 버퍼로는
 * 장치가 DMA 를 할 수 없어 바운스 버퍼를 거치게 된다. dma-ranges 를
 * 어떻게 배정하느냐가 그 범위를 정하며, 여기서 배정에 실패하면
 * 그 범위가 좁아진다.
 *
 * === 주요 함수/구조체 요약 ===
 * bar_max_size[]                : BAR 별 최대 크기 표.
 * cdns_pcie_host_training_complete() : 트레이닝 완료를 기다린다.
 * cdns_pcie_host_wait_for_link(): 링크가 올라오기를 기다린다.
 * cdns_pcie_retrain()           : Gen2 결함 우회를 위한 재트레이닝.
 * cdns_pcie_host_start_link()   : 위 둘을 묶은 진입점.
 * cdns_pcie_host_find_min_bar() / _find_max_bar() : BAR 고르기.
 * cdns_pcie_host_dma_ranges_cmp() : 크기 순 정렬 비교 함수.
 * cdns_pcie_host_bar_ib_config(): 구형 판의 인바운드 BAR 레지스터 설정.
 * cdns_pcie_host_bar_config()   : 범위 하나를 BAR 에 배정.
 * cdns_pcie_host_map_dma_ranges(): dma-ranges 전체를 배정.
 */

/* [한국어] usleep_range — 링크 대기 루프에서 CPU 를 놓아 준다. */
#include <linux/delay.h>
/* [한국어] time_before 등 jiffies 비교 매크로와 기본 커널 매크로. */
#include <linux/kernel.h>
/* [한국어] list_sort — dma-ranges 를 크기 순으로 정렬한다. 일반
 * 배열 정렬이 아니라 연결 리스트 정렬이 필요해서 이 헤더를 쓴다. */
#include <linux/list_sort.h>
/* [한국어] 디바이스 트리의 주소 범위 파싱. */
#include <linux/of_address.h>
/* [한국어] PCI 관련 디바이스 트리 헬퍼. */
#include <linux/of_pci.h>
/* [한국어] 플랫폼 장치 관련. 호스트 브리지가 플랫폼 장치로 등록되기 때문이다. */
#include <linux/platform_device.h>

/* [한국어] struct cdns_pcie_rc, 레지스터 접근자, RP_BAR* 열거값. */
#include "pcie-cadence.h"
/* [한국어] 이 파일이 구현하는 함수들의 선언과 두 함수 포인터 타입. */
#include "pcie-cadence-host-common.h"

/* [한국어] 재트레이닝 완료를 기다리는 최대 시간. HZ 는 1초에 해당하는
 * jiffies 수이므로 곧 1초다.
 * 링크 트레이닝은 보통 수 밀리초 안에 끝나므로 1초면 충분히 넉넉하며,
 * 그래도 안 되면 하드웨어 문제로 보고 포기한다. */
#define LINK_RETRAIN_TIMEOUT HZ

/* [한국어] BAR 번호별로 담을 수 있는 최대 크기.
 * 이 IP 의 인바운드 BAR 는 크기가 제각각이라 아무 데나 넣을 수 없다.
 *   RP_BAR0  — 128 * 2GB = 256GB. 가장 크다.
 *   RP_BAR1  — 2GB.
 *   RP_NO_BAR— 2^63. "BAR 를 쓰지 않고 통과시킨다" 는 특별한 항목으로,
 *              사실상 무제한이라 어떤 범위든 받아들인다.
 * _ULL 과 _BITULL 을 쓰는 것은 32비트 아키텍처에서도 이 값들이
 * 64비트로 계산되게 하려는 것이다 — 그냥 곱하면 오버플로가 난다.
 * EXPORT 하는 이유는 두 호스트 파일이 이 표를 직접 참조하기 때문이다. */
u64 bar_max_size[] = {
	[RP_BAR0] = _ULL(128 * SZ_2G),
	[RP_BAR1] = SZ_2G,
	[RP_NO_BAR] = _BITULL(63),
};
EXPORT_SYMBOL_GPL(bar_max_size);

/* [한국어]
 * cdns_pcie_host_training_complete - 링크 트레이닝이 끝나기를 기다린다
 *
 * @pcie: 대상 컨트롤러.
 * @return: 0 이면 트레이닝 완료. 1초 안에 안 끝나면 -ETIMEDOUT.
 *
 * "링크가 올라왔는가" 와는 다른 물음이다. 트레이닝이 진행 중인 동안에도
 * 링크 비트가 잠깐 설 수 있으므로, 속도와 폭이 확정된 상태를 보장하려면
 * Link Training 비트가 내려가기를 따로 확인해야 한다.
 *
 * PCI_EXP_LNKSTA_LT 는 PCIe 규격이 정한 표준 비트라, 이 IP 전용 레지스터가
 * 아니라 루트 포트의 PCIe capability 를 읽는다. 그래서 구형·신형 공통으로
 * 쓸 수 있고 함수 포인터를 받지 않는다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 호출 체인:
 *   cdns_pcie_retrain() → [이 함수] → cdns_pcie_rp_readw()
 */
int cdns_pcie_host_training_complete(struct cdns_pcie *pcie)
{
	/* [한국어] 루트 포트의 PCIe capability 시작 오프셋. 이 IP 에서는
	 * 위치가 고정이라 상수로 정의되어 있다. */
	u32 pcie_cap_off = CDNS_PCIE_RP_CAP_OFFSET;
	unsigned long end_jiffies;
	u16 lnk_stat;

	/* Wait for link training to complete. Exit after timeout. */
	/* [한국어] 마감 시각을 먼저 정해 둔다. 루프 안에서 매번 계산하지
	 * 않으려는 것이고, jiffies 가 도는 동안 기준이 흔들리지 않게 한다. */
	end_jiffies = jiffies + LINK_RETRAIN_TIMEOUT;
	do {
		/* [한국어] Link Status 레지스터를 읽는다. */
		lnk_stat = cdns_pcie_rp_readw(pcie, pcie_cap_off + PCI_EXP_LNKSTA);
		/* [한국어] LT(Link Training) 비트가 내려갔으면 협상이 끝났다. */
		if (!(lnk_stat & PCI_EXP_LNKSTA_LT))
			break;
		/* [한국어] 최소 0, 최대 1ms 를 잔다. 하한이 0 인 것은
		 * 커널이 다른 타이머와 묶어 처리할 여지를 주려는 것으로,
		 * 정밀한 간격이 필요 없는 폴링에서 흔한 방식이다. */
		usleep_range(0, 1000);
	} while (time_before(jiffies, end_jiffies));

	/* [한국어] 루프를 빠져나온 이유가 둘이라 다시 확인한다 —
	 * 완료됐거나, 시간이 다 됐거나. 마지막으로 읽은 값으로 판단한다. */
	if (!(lnk_stat & PCI_EXP_LNKSTA_LT))
		return 0;

	return -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(cdns_pcie_host_training_complete);

/* [한국어]
 * cdns_pcie_host_wait_for_link - 링크가 올라오기를 기다린다
 *
 * @pcie: 대상 컨트롤러.
 * @pcie_link_up: 링크 상태를 읽는 방법. 구형은 cdns_pcie_linkup(),
 *   신형은 cdns_pcie_hpa_link_up() 을 넘긴다.
 * @return: 0 이면 링크가 올라왔다. 재시도 한도를 넘기면 -ETIMEDOUT.
 *
 * 함수 포인터를 받는 이유가 이 파일의 공통화 기법 자체다. 대기 루프의
 * 논리는 구형·신형이 같고 상태를 읽는 레지스터만 다르므로, 그 부분만
 * 호출자가 넘기게 했다.
 *
 * 링크가 안 올라오는 것이 반드시 오류는 아니다 — 슬롯이 비어 있으면
 * 당연히 링크가 없다. 호출자가 그 사정을 알고 판단한다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 호출 체인:
 *   cdns_pcie_host_start_link() → [이 함수] → pcie_link_up 콜백
 *   cdns_pcie_retrain() → [이 함수]
 */
int cdns_pcie_host_wait_for_link(struct cdns_pcie *pcie,
				 cdns_pcie_linkup_func pcie_link_up)
{
	struct device *dev = pcie->dev;
	int retries;

	/* Check if the link is up or not */
	/* [한국어] 최대 재시도 횟수까지 반복한다. 상수들은 PCI 코어의
	 * 공통 헤더에 정의되어 있어 다른 컨트롤러 드라이버와 같은 값을 쓴다. */
	for (retries = 0; retries < LINK_WAIT_MAX_RETRIES; retries++) {
		/* [한국어] 호출자가 넘긴 방법으로 링크 상태를 읽는다. */
		if (pcie_link_up(pcie)) {
			/* [한국어] dev_dbg 가 아니라 dev_info 인 것은 이 메시지가
			 * 부팅 로그에서 링크가 올라온 시점을 알려 주는 실마리라
			 * 늘 보이는 편이 낫기 때문이다. */
			dev_info(dev, "Link up\n");
			return 0;
		}
		/* [한국어] 다음 확인까지 잔다. 범위로 주면 커널이 다른
		 * 타이머와 묶어 깨울 수 있어 효율적이다. */
		usleep_range(LINK_WAIT_USLEEP_MIN, LINK_WAIT_USLEEP_MAX);
	}

	/* [한국어] 시간 안에 안 올라왔다. 슬롯이 비었을 수도 있으므로
	 * 여기서 메시지를 남기지 않고 판단을 호출자에게 맡긴다. */
	return -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(cdns_pcie_host_wait_for_link);

/* [한국어] cdns_pcie_retrain - Gen2 트레이닝 결함을 우회하기 위해 재협상시킨다
 * 
 * @pcie: 대상 컨트롤러.
 * @pcie_link_up: 링크 상태를 읽는 방법(구형·신형 구분).
 * @return: 0 이면 성공 또는 재트레이닝이 필요 없는 경우. 실패 시 음수.
 * 
 * 일부 하드웨어에 결함이 있어 링크가 2.5GT/s(Gen1)로만 협상되는 일이
 * 있다. 그때 Retrain Link 비트를 세우면 다시 협상해 제 속도가 나온다.
 * 
 * 두 조건을 모두 확인한 뒤에만 재트레이닝을 건다.
 *   루트 포트가 2.5GT/s 보다 빠른 속도를 지원하는가(LNKCAP)
 *   그런데 지금 2.5GT/s 로 동작 중인가(LNKSTA)
 * 둘 다 참이면 결함에 걸린 상황이므로 다시 시도할 값어치가 있다.
 * 
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트. 안에서 잠든다.
 * 
 * 호출 체인:
 *   cdns_pcie_host_start_link() → [이 함수]
 *     → cdns_pcie_host_training_complete() → cdns_pcie_host_wait_for_link() */
int cdns_pcie_retrain(struct cdns_pcie *pcie,
		      cdns_pcie_linkup_func pcie_link_up)
{
	/* [한국어] lnk_cap_sls 는 루트 포트가 지원하는 최대 속도, pcie_cap_off 는
	 * PCIe capability 의 위치다. */
	u32 lnk_cap_sls, pcie_cap_off = CDNS_PCIE_RP_CAP_OFFSET;
	/* [한국어] 현재 상태와 제어 레지스터 값. */
	u16 lnk_stat, lnk_ctl;
	/* [한국어] 0 으로 초기화한다. 재트레이닝이 필요 없으면 이 값이 그대로 반환된다. */
	int ret = 0;

	/*
	 * Set retrain bit if current speed is 2.5 GB/s,
	 * but the PCIe root port support is > 2.5 GB/s.
	 */

	/* [한국어] Link Capabilities 를 읽는다. rp_readl 이 아니라 readl 에 RP_BASE 를
	 * 더해 쓰는데, 32비트 접근이 필요해서다 — 아래 LNKSTA/LNKCTL 은 16비트라
	 * 전용 접근자를 쓴다. */
	lnk_cap_sls = cdns_pcie_readl(pcie, (CDNS_PCIE_RP_BASE + pcie_cap_off +
					     PCI_EXP_LNKCAP));
	/* [한국어] 지원 최대 속도가 2.5GT/s 이하라면 이미 최선이므로 할 일이 없다.
	 * 결함이 아니라 원래 그런 하드웨어다. */
	if ((lnk_cap_sls & PCI_EXP_LNKCAP_SLS) <= PCI_EXP_LNKCAP_SLS_2_5GB)
		return ret;

	/* [한국어] 현재 협상된 속도를 읽는다. */
	lnk_stat = cdns_pcie_rp_readw(pcie, pcie_cap_off + PCI_EXP_LNKSTA);
	/* [한국어] 지원은 더 빠른데 현재 2.5GT/s 라면 결함에 걸린 상황이다. */
	if ((lnk_stat & PCI_EXP_LNKSTA_CLS) == PCI_EXP_LNKSTA_CLS_2_5GB) {
		/* [한국어] Link Control 을 읽어 */
		lnk_ctl = cdns_pcie_rp_readw(pcie,
					     pcie_cap_off + PCI_EXP_LNKCTL);
		/* [한국어] Retrain Link 비트를 세운다. 이 비트는 쓰면 자동으로 지워지는
		 * 트리거 방식이라 나중에 되돌릴 필요가 없다. */
		lnk_ctl |= PCI_EXP_LNKCTL_RL;
		/* [한국어] 쓰는 순간 재트레이닝이 시작된다. */
		cdns_pcie_rp_writew(pcie, pcie_cap_off + PCI_EXP_LNKCTL,
				    lnk_ctl);

		/* [한국어] 먼저 트레이닝이 끝나기를 기다린다. */
		ret = cdns_pcie_host_training_complete(pcie);
		/* [한국어] 시간 안에 안 끝나면 포기한다. */
		if (ret)
			return ret;

		/* [한국어] 그다음 링크가 실제로 올라왔는지 확인한다. 트레이닝이 끝났다고
		 * 반드시 링크가 서는 것은 아니기 때문이다. */
		ret = cdns_pcie_host_wait_for_link(pcie, pcie_link_up);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(cdns_pcie_retrain);

/* [한국어] cdns_pcie_host_start_link - 링크를 올리고 필요하면 재트레이닝한다
 * 
 * @rc: 루트 컴플렉스.
 * @pcie_link_up: 링크 상태를 읽는 방법.
 * @return: 0 이면 링크가 올라왔다.
 * 
 * 두 호스트 파일이 부르는 진입점이다. 링크를 기다린 뒤, 이 보드에
 * Gen2 결함 quirk 가 걸려 있으면 재트레이닝까지 한다.
 * 
 * 실행 컨텍스트: 호스트 초기화 중 프로세스 컨텍스트.
 * 
 * 호출 체인:
 *   cdns_pcie_host_setup() 계열 → [이 함수]
 *     → cdns_pcie_host_wait_for_link() → cdns_pcie_retrain() */
int cdns_pcie_host_start_link(struct cdns_pcie_rc *rc,
			      cdns_pcie_linkup_func pcie_link_up)
{
	/* [한국어] rc 안에 박혀 있는 공통 컨트롤러 구조체를 꺼낸다. */
	struct cdns_pcie *pcie = &rc->pcie;
	/* [한국어] 대기와 재트레이닝의 결과. */
	int ret;

	/* [한국어] 먼저 링크가 올라오기를 기다린다. */
	ret = cdns_pcie_host_wait_for_link(pcie, pcie_link_up);

	/*
	 * Retrain link for Gen2 training defect
	 * if quirk flag is set.
	 */
	if (!ret && rc->quirk_retrain_flag)
		/* [한국어] 링크가 올라왔고 quirk 가 걸린 보드라면 재트레이닝을 시도한다.
		 * 순서가 중요하다 — 링크가 없는 상태에서 재트레이닝을 걸어 봐야
		 * 의미가 없다. */
		ret = cdns_pcie_retrain(pcie, pcie_link_up);

	return ret;
}
EXPORT_SYMBOL_GPL(cdns_pcie_host_start_link);

/* [한국어]
 * cdns_pcie_host_find_min_bar - 이 크기를 담을 수 있는 가장 작은 BAR 를 고른다
 *
 * @rc: 루트 컴플렉스. avail_ib_bar[] 로 어느 BAR 가 비어 있는지 본다.
 * @size: 담아야 할 크기.
 * @return: 고른 BAR 번호. 담을 수 있는 BAR 가 없으면 RP_BAR_UNDEFINED.
 *
 * "가장 작은" 것을 고르는 이유가 이 배정 전략의 핵심이다. BAR 마다
 * 최대 크기가 다른데(RP_BAR0 는 256GB, RP_BAR1 은 2GB), 작은 범위에
 * 큰 BAR 를 써 버리면 나중에 큰 범위가 들어왔을 때 담을 곳이 없어진다.
 * 그래서 충분한 것 중 가장 작은 것을 쓴다 — 흔히 best-fit 이라 부르는
 * 방식이다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트. 순수 탐색.
 *
 * 호출 체인:
 *   cdns_pcie_host_bar_config() → [이 함수]
 */
enum cdns_pcie_rp_bar
cdns_pcie_host_find_min_bar(struct cdns_pcie_rc *rc, u64 size)
{
	enum cdns_pcie_rp_bar bar, sel_bar;

	/* [한국어] 아직 아무것도 못 골랐음을 나타내는 값으로 시작한다. */
	sel_bar = RP_BAR_UNDEFINED;
	/* [한국어] RP_NO_BAR 까지 포함해 순회한다. 그것도 후보 중 하나이며,
	 * 최대 크기가 2^63 이라 어떤 범위든 받아들인다. */
	for (bar = RP_BAR0; bar <= RP_NO_BAR; bar++) {
		/* [한국어] 이미 다른 범위에 배정된 BAR 는 건너뛴다. */
		if (!rc->avail_ib_bar[bar])
			continue;

		/* [한국어] 이 BAR 가 요청 크기를 담을 수 있는가. */
		if (size <= bar_max_size[bar]) {
			/* [한국어] 첫 후보라면 무조건 채택하고 다음으로. */
			if (sel_bar == RP_BAR_UNDEFINED) {
				sel_bar = bar;
				continue;
			}

			/* [한국어] 이미 후보가 있으면 더 작은 쪽으로 바꾼다.
			 * 이 비교가 best-fit 을 만든다. */
			if (bar_max_size[bar] < bar_max_size[sel_bar])
				sel_bar = bar;
		}
	}

	/* [한국어] 찾지 못했으면 RP_BAR_UNDEFINED 가 그대로 나간다. */
	return sel_bar;
}
EXPORT_SYMBOL_GPL(cdns_pcie_host_find_min_bar);

/* [한국어]
 * cdns_pcie_host_find_max_bar - 이 크기 안에 들어가는 가장 큰 BAR 를 고른다
 *
 * @rc: 루트 컴플렉스.
 * @size: 담아야 할 크기.
 * @return: 고른 BAR 번호. 조건에 맞는 것이 없으면 RP_BAR_UNDEFINED.
 *
 * 위 find_min_bar 와 부등호가 반대다. 여기서는 "요청 크기보다 작거나
 * 같은" BAR 중 가장 큰 것을 고른다.
 *
 * 언제 쓰는가 — 한 BAR 로 다 담을 수 없을 만큼 큰 범위를 나눠 담을 때다.
 * 가능한 한 큰 BAR 로 잘라 내야 남는 조각이 작아지고, 그래야 BAR 를
 * 적게 쓴다. bar_config() 가 min 을 먼저 시도하고 실패하면 이것을
 * 쓰는 흐름이 그 판단이다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트. 순수 탐색.
 *
 * 호출 체인:
 *   cdns_pcie_host_bar_config() → [이 함수]
 */
enum cdns_pcie_rp_bar
cdns_pcie_host_find_max_bar(struct cdns_pcie_rc *rc, u64 size)
{
	enum cdns_pcie_rp_bar bar, sel_bar;

	/* [한국어] 아직 후보가 없음을 나타내는 값으로 시작한다. */
	sel_bar = RP_BAR_UNDEFINED;
	/* [한국어] RP_NO_BAR 까지 포함해 순회한다. */
	for (bar = RP_BAR0; bar <= RP_NO_BAR; bar++) {
		if (!rc->avail_ib_bar[bar])
			continue;

		/* [한국어] 부등호 방향이 min 판과 반대다 — 요청 크기 안에
		 * 들어가는 BAR 만 후보로 삼는다. */
		if (size >= bar_max_size[bar]) {
			if (sel_bar == RP_BAR_UNDEFINED) {
				sel_bar = bar;
				continue;
			}

			/* [한국어] 더 큰 쪽으로 바꾼다. 잘라 낼 조각을
			 * 최대한 크게 하려는 것이다. */
			if (bar_max_size[bar] > bar_max_size[sel_bar])
				sel_bar = bar;
		}
	}

	/* [한국어] 조건에 맞는 BAR 가 없으면 RP_BAR_UNDEFINED 를 돌려준다. */
	return sel_bar;
}
EXPORT_SYMBOL_GPL(cdns_pcie_host_find_max_bar);

/* [한국어]
 * cdns_pcie_host_dma_ranges_cmp - dma-ranges 를 크기 내림차순으로 정렬한다
 *
 * @priv: list_sort 가 전달하는 사용자 데이터. 여기서는 쓰지 않는다.
 * @a, @b: 비교할 두 리스트 노드.
 * @return: a 가 앞에 와야 하면 음수, 뒤면 양수, 같으면 0.
 *
 * list_sort 에 넘기는 비교 함수다. 큰 범위가 앞에 오도록 정렬한다 —
 * 크기가 클 때 -1(앞으로)을 돌려주므로 내림차순이다.
 *
 * 왜 큰 것부터인가. BAR 는 개수가 정해져 있고 크기 제한도 제각각이라,
 * 작은 것부터 배정하면 큰 범위가 나중에 왔을 때 남은 BAR 로는 담지
 * 못하는 일이 생긴다. 큰 것부터 자리를 잡아 두면 그 위험이 준다.
 *
 * 실행 컨텍스트: 정렬 중 프로세스 컨텍스트. 순수 비교.
 *
 * 호출 체인:
 *   cdns_pcie_host_map_dma_ranges() → list_sort() → [이 함수]
 */
int cdns_pcie_host_dma_ranges_cmp(void *priv, const struct list_head *a,
				  const struct list_head *b)
{
	struct resource_entry *entry1, *entry2;
	u64 size1, size2;

	/* [한국어] 리스트 노드에서 바깥 자원 항목을 되짚는다.
	 * list_sort 는 노드만 넘겨주므로 이 변환이 필요하다. */
	entry1 = container_of(a, struct resource_entry, node);
	entry2 = container_of(b, struct resource_entry, node);

	/* [한국어] 각 항목이 담는 주소 범위의 크기를 구한다. */
	size1 = resource_size(entry1->res);
	size2 = resource_size(entry2->res);

	/* [한국어] 큰 쪽이 앞으로. 음수가 "a 를 앞에" 를 뜻한다. */
	if (size1 > size2)
		return -1;

	if (size1 < size2)
		return 1;

	/* [한국어] 크기가 같으면 순서를 바꾸지 않는다. list_sort 는
	 * 안정 정렬이라 원래 순서가 유지된다. */
	return 0;
}
EXPORT_SYMBOL_GPL(cdns_pcie_host_dma_ranges_cmp);

/* [한국어]
 * cdns_pcie_host_bar_config - dma-ranges 항목 하나를 BAR 들에 배정한다
 *
 * @rc: 루트 컴플렉스.
 * @entry: 배정할 주소 범위 하나.
 * @pci_host_ib_config: 실제 레지스터를 쓰는 방법. 구형·신형이 다르므로
 *   호출자가 넘긴다.
 * @return: 0 이면 성공. 담을 BAR 가 없으면 -EINVAL, 설정 실패 시 그 오류.
 *
 * 이 파일에서 논리가 가장 복잡한 함수다. 범위 하나가 BAR 하나에 딱
 * 들어가지 않을 수 있어 나눠 담아야 하기 때문이다.
 *
 * 상류 주석이 알고리즘을 잘 설명하고 있고, 요약하면 이렇다.
 *   1) 남은 크기를 통째로 담을 수 있는 가장 작은 BAR 를 찾는다.
 *      찾으면 거기 넣고 끝.
 *   2) 못 찾으면 — 남은 크기가 어떤 BAR 보다도 크다는 뜻이다.
 *      그러면 남은 크기 안에 들어가는 가장 큰 BAR 를 찾아 그만큼만
 *      잘라 담고, 나머지를 들고 1번으로 돌아간다.
 *   3) 그것마저 못 찾으면 담을 방법이 없으므로 오류.
 *
 * 왜 이렇게 하는가. BAR 마다 크기 상한이 정해져 있고 개수도 적어서,
 * 큰 범위를 담으려면 여러 BAR 에 쪼개야 한다. 그러면서도 BAR 를
 * 되도록 적게 쓰려면 매번 최선의 크기를 골라야 한다.
 *
 * 실행 컨텍스트: 호스트 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cdns_pcie_host_map_dma_ranges() → [이 함수]
 *     → cdns_pcie_host_find_min_bar() / _find_max_bar()
 *     → pci_host_ib_config 콜백 (구형: cdns_pcie_host_bar_ib_config)
 */
int cdns_pcie_host_bar_config(struct cdns_pcie_rc *rc,
			      struct resource_entry *entry,
			      cdns_pcie_host_bar_ib_cfg pci_host_ib_config)
{
	struct cdns_pcie *pcie = &rc->pcie;
	struct device *dev = pcie->dev;
	/* [한국어] cpu_addr 과 size 는 루프를 돌며 줄어든다.
	 * winsize 는 이번 회차에 잘라 담을 크기다. */
	u64 cpu_addr, size, winsize;
	enum cdns_pcie_rp_bar bar;
	unsigned long flags;
	int ret;

	/* [한국어] 이 범위의 CPU 쪽 시작 주소. */
	cpu_addr = entry->res->start;
	/* [한국어] 자원 플래그(메모리/prefetchable 등). 콜백에 그대로 넘긴다. */
	flags = entry->res->flags;
	/* [한국어] 담아야 할 전체 크기. 루프가 이 값을 0 으로 만들 때까지 돈다. */
	size = resource_size(entry->res);

	while (size > 0) {
		/*
		 * Try to find a minimum BAR whose size is greater than
		 * or equal to the remaining resource_entry size. This will
		 * fail if the size of each of the available BARs is less than
		 * the remaining resource_entry size.
		 *
		 * If a minimum BAR is found, IB ATU will be configured and
		 * exited.
		 */
		/* [한국어] 남은 크기를 통째로 담을 수 있는 가장 작은 BAR. */
		bar = cdns_pcie_host_find_min_bar(rc, size);
		if (bar != RP_BAR_UNDEFINED) {
			/* [한국어] 찾았으면 남은 전부를 여기 넣고 끝난다.
			 * 호출자가 넘긴 방법으로 실제 레지스터를 쓴다. */
			ret = pci_host_ib_config(rc, bar, cpu_addr, size, flags);
			if (ret)
				dev_err(dev, "IB BAR: %d config failed\n", bar);
			/* [한국어] 성공이든 실패든 여기서 함수가 끝난다 —
			 * 남은 크기가 0 이 되었기 때문이다. */
			return ret;
		}

		/*
		 * If the control reaches here, it would mean the remaining
		 * resource_entry size cannot be fitted in a single BAR. So we
		 * find a maximum BAR whose size is less than or equal to the
		 * remaining resource_entry size and split the resource entry
		 * so that part of resource entry is fitted inside the maximum
		 * BAR. The remaining size would be fitted during the next
		 * iteration of the loop.
		 *
		 * If a maximum BAR is not found, there is no way we can fit
		 * this resource_entry, so we error out.
		 */
		/* [한국어] 통째로는 안 되므로 잘라 담는다. 남은 크기 안에
		 * 들어가는 가장 큰 BAR 를 골라 그만큼만 처리한다. */
		bar = cdns_pcie_host_find_max_bar(rc, size);
		if (bar == RP_BAR_UNDEFINED) {
			/* [한국어] 쓸 수 있는 BAR 가 하나도 남지 않았다.
			 * dma-ranges 가 하드웨어 능력을 넘어선 것이므로
			 * 디바이스 트리를 고쳐야 한다. */
			dev_err(dev, "No free BAR to map cpu_addr %llx\n",
				cpu_addr);
			return -EINVAL;
		}

		/* [한국어] 그 BAR 의 최대 크기만큼 잘라 담는다.
		 * 남은 크기가 아니라 BAR 크기를 쓰는 점이 중요하다 —
		 * 이 BAR 는 남은 크기보다 작으므로 꽉 채워 쓴다. */
		winsize = bar_max_size[bar];
		ret = pci_host_ib_config(rc, bar, cpu_addr, winsize, flags);
		if (ret) {
			/* [한국어] 어느 BAR 설정이 실패했는지 번호와 함께 남긴다. */
			dev_err(dev, "IB BAR: %d config failed\n", bar);
			return ret;
		}

		/* [한국어] 담은 만큼 빼고 주소를 앞으로 옮긴 뒤 다시 시도한다.
		 * 다음 회차에는 남은 크기가 작아졌으므로 min 탐색이 성공할
		 * 가능성이 커진다. */
		size -= winsize;
		cpu_addr += winsize;
	}

	return 0;
}

/* [한국어]
 * cdns_pcie_host_map_dma_ranges - dma-ranges 전체를 인바운드 BAR 에 배정한다
 *
 * @rc: 루트 컴플렉스.
 * @pci_host_ib_config: 실제 레지스터를 쓰는 방법(구형·신형 구분).
 * @return: 0 이면 성공.
 *
 * 호스트 초기화에서 부르는 상위 진입점이다. 두 갈래로 갈린다.
 *
 * dma-ranges 가 없으면 — 디바이스 트리가 아무 제약도 걸지 않았다는
 *   뜻이다. 그러면 넓은 범위를 통째로 여는 특별 항목(RP_NO_BAR)에
 *   배정한다. 그 폭은 cdns,no-bar-match-nbits 속성으로 정하고,
 *   없으면 기본 32비트(4GB)다.
 *
 * dma-ranges 가 있으면 — 크기 내림차순으로 정렬한 뒤 큰 것부터
 *   BAR 에 배정한다. 큰 것부터 하는 이유는 앞서 설명한 대로,
 *   작은 것부터 하면 큰 범위가 나중에 담길 곳을 잃기 때문이다.
 *
 * 실행 컨텍스트: 호스트 초기화 중 프로세스 컨텍스트.
 *
 * 에러 경로: 한 항목이라도 실패하면 곧바로 물러난다. 앞서 배정한
 *   BAR 들을 되돌리지 않는데, 이 실패는 초기화 실패로 이어져
 *   컨트롤러 전체가 정리되기 때문이다.
 *
 * 호출 체인:
 *   cdns_pcie_host_init_address_translation() 계열 → [이 함수]
 *     → list_sort() → cdns_pcie_host_bar_config()
 */
int cdns_pcie_host_map_dma_ranges(struct cdns_pcie_rc *rc,
				  cdns_pcie_host_bar_ib_cfg pci_host_ib_config)
{
	struct cdns_pcie *pcie = &rc->pcie;
	/* [한국어] 오류 메시지를 낼 device. */
	struct device *dev = pcie->dev;
	struct device_node *np = dev->of_node;
	struct pci_host_bridge *bridge;
	struct resource_entry *entry;
	/* [한국어] dma-ranges 가 없을 때 열 범위의 폭(비트). 기본 32비트는
	 * 4GB 를 뜻하며, 32비트 DMA 만 하는 장치를 염두에 둔 값으로 읽힌다. */
	u32 no_bar_nbits = 32;
	int err;

	/* [한국어] rc 는 브리지의 private 영역에 들어 있으므로 거꾸로 찾는다. */
	bridge = pci_host_bridge_from_priv(rc);
	if (!bridge)
		return -ENOMEM;

	if (list_empty(&bridge->dma_ranges)) {
		/* [한국어] 디바이스 트리가 범위를 지정하지 않았다.
		 * 반환값을 확인하지 않는 것은, 속성이 없으면 no_bar_nbits 가
		 * 초기값 32 로 남아 그것이 곧 기본 동작이기 때문이다. */
		of_property_read_u32(np, "cdns,no-bar-match-nbits",
				     &no_bar_nbits);
		/* [한국어] RP_NO_BAR 에 2^nbits 크기로 통째로 연다.
		 * CPU 주소를 0 으로 주는 것은 "이 범위 전체를 그대로
		 * 대응시켜라" 는 뜻이다. */
		err = pci_host_ib_config(rc, RP_NO_BAR, 0x0, (u64)1 << no_bar_nbits, 0);
		if (err)
			dev_err(dev, "IB BAR: %d config failed\n", RP_NO_BAR);
		return err;
	}

	/* [한국어] 크기 내림차순으로 정렬한다. 첫 인자 NULL 은 비교 함수에
	 * 넘길 사용자 데이터가 없다는 뜻이다. */
	list_sort(NULL, &bridge->dma_ranges, cdns_pcie_host_dma_ranges_cmp);

	/* [한국어] 큰 것부터 차례로 배정한다. */
	resource_list_for_each_entry(entry, &bridge->dma_ranges) {
		err = cdns_pcie_host_bar_config(rc, entry, pci_host_ib_config);
		if (err) {
			/* [한국어] 어느 항목에서 실패했는지까지는 알리지 않는다.
			 * bar_config() 가 이미 더 구체적인 메시지를 남겼다. */
			dev_err(dev, "Fail to configure IB using dma-ranges\n");
			return err;
		}
	}

	return 0;
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cadence PCIe host controller driver");
