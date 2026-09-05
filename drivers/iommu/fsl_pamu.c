// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2013 Freescale Semiconductor, Inc.
 */

/*
 * [한국어 설명] Freescale PAMU 하드웨어 초기화와 PAACE 조작 (fsl_pamu.c)
 *
 * === 파일의 역할 ===
 * PAMU(Peripheral Access Management Unit) 하드웨어를 실제로 켜고, 그것이
 * 참조하는 세 개의 메모리 테이블(PAACT, SPAACT, OMT)을 만들어 등록하며,
 * LIODN별 PAACE 엔트리를 조작하는 저수준 구현이다.
 * fsl_pamu_domain.c가 리눅스 IOMMU API 쪽을 담당한다면, 이 파일은
 * 하드웨어 쪽을 담당한다 — 둘이 짝을 이룬다.
 *
 * 이 파일에서 다루는 일은 크게 넷이다.
 *
 * (1) **PAACE 조작**. pamu_enable_liodn()/disable_liodn()/config_ppaace()/
 *     update_paace_stash()가 그것으로, fsl_pamu_domain.c가 호출한다.
 *     모두 set_bf/get_bf 매크로로 압축된 비트 필드를 다루고, 끝에 mb()로
 *     하드웨어 가시성을 보장한다.
 *
 * (2) **스태시 목적지 탐색**. get_stash_id()가 디바이스 트리의 CPU 노드와
 *     캐시 계층을 거슬러 올라가며 "이 CPU의 L1/L2/L3 캐시의 스태시 ID"를
 *     찾는다. DPAA의 성능 최적화가 이 값에 달려 있다.
 *
 * (3) **부팅 초기화**. fsl_pamu_probe()가 테이블 메모리를 잡고, 각 PAMU
 *     인스턴스의 레지스터를 설정하고, OMT를 채우고, 디바이스 트리에 적힌
 *     모든 LIODN을 활성화한다. 마지막으로 GUTS의 bypass 비트를 내려
 *     PAMU가 실제로 동작하게 만든다.
 *
 * (4) **에라타 대응**. 이 파일의 상당 부분이 특정 실리콘의 버그를 우회하는
 *     코드다. create_csd()는 A-004510(테이블 메모리의 코히런시 문제)을
 *     피하려고 코히런시 서브도메인을 만들고, pamu_av_isr()의 한 분기는
 *     A-003638(비활성 LIODN에 대한 잘못된 접근 위반 보고)을 다룬다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [부팅] arch_initcall → fsl_pamu_init()
 *        ↓ 플랫폼 디바이스를 직접 만들어 probe를 앞당긴다
 *   [이 파일] fsl_pamu_probe()
 *        ↓ 테이블 메모리 할당 → 각 PAMU 레지스터 설정 → OMT 채우기
 *        ↓ setup_liodns()로 디바이스 트리의 LIODN 전부 활성화
 *        ↓ pamu_domain_init()으로 IOMMU 코어에 등록(fsl_pamu_domain.c)
 *   [PAMU 하드웨어] PAACT/SPAACT/OMT를 참조해 DMA를 검사
 *
 *   [런타임] fsl_pamu_domain.c의 attach/detach/stash 설정
 *        ↓
 *   [이 파일] pamu_config_ppaace() 등이 PAACE를 갱신
 *
 * 초기화 순서가 이 드라이버의 특이점이다. 보통의 OF probe는 시점이 늦어
 * QMan 같은 PAMU 클라이언트가 먼저 올라와 버린다. 그래서 arch_initcall에서
 * 플랫폼 디바이스를 손수 만들어 probe를 강제로 앞당긴다.
 *
 * 실행 컨텍스트: probe와 초기화는 부팅 초기 단일 스레드. PAACE 조작 함수들은
 * fsl_pamu_domain.c가 스핀락을 쥔 상태에서 호출한다. pamu_av_isr()은
 * 하드 인터럽트 컨텍스트다.
 *
 * === 타 모듈과의 연결 ===
 * - fsl_pamu.h: PAACE 레이아웃, 레지스터 오프셋, set_bf/get_bf 매크로.
 *   이 파일이 쓰는 모든 하드웨어 지식이 거기 있다.
 * - fsl_pamu_domain.c: 이 파일이 노출하는 pamu_enable_liodn() 등을
 *   호출하고, 반대로 이 파일의 probe가 pamu_domain_init()을 호출한다.
 * - linux/fsl/guts.h: struct ccsr_guts — GUTS(Global Utilities) 블록의
 *   pamubypenr 레지스터로 PAMU 우회를 켜고 끈다.
 * - asm/mpc85xx.h: SVR_P2040 등 실리콘 버전 상수. 에라타 대상 칩을
 *   식별하는 데 쓴다.
 * - drivers/soc/fsl/qbman: QMan/BMan. setup_qbman_paace()가 그들만을 위한
 *   특수 설정을 한다.
 * 데이터 흐름: 디바이스 트리의 fsl,liodn 프로퍼티 → setup_liodns()가
 * 각 LIODN의 PAACE를 채우고 활성화 → 이후 그 디바이스의 DMA가 통과.
 *
 * === 주요 함수/구조체 요약 ===
 * - fsl_pamu_probe(): 초기화의 전부. 테이블 할당, 레지스터 설정, OMT 구성,
 *   LIODN 활성화, bypass 해제까지.
 * - setup_one_pamu(): PAMU 인스턴스 하나에 테이블 위치를 알리고 켠다.
 * - setup_liodns(): 디바이스 트리를 훑어 모든 LIODN의 PAACE를 기본 구성으로
 *   채우고 활성화한다. QMan/BMan은 특별 처리한다.
 * - get_stash_id(): 캐시 계층을 거슬러 올라가며 스태시 목적지 ID를 찾는다.
 * - pamu_av_isr(): 접근 위반 인터럽트 핸들러. 레지스터를 덤프하고
 *   문제의 LIODN을 비활성화한다.
 * - create_csd(): 에라타 A-004510 우회. 테이블 메모리 영역에 코히런시
 *   서브도메인을 만들어 스누핑이 되게 한다.
 * - setup_omt(): 오퍼레이션 매핑 테이블을 채운다 — 어떤 I/O 연산을
 *   어떤 캐시 동작으로 바꿀지 정의한다.
 */

/* [한국어] 이 파일의 pr_* 출력 앞에 "fsl-pamu: <함수명>: "를 붙인다.
 * 접근 위반 핸들러가 여러 줄을 쏟아 내므로 출처를 분명히 하려는 것이다. */
#define pr_fmt(fmt)    "fsl-pamu: %s: " fmt, __func__

/* [한국어] PAACE 레이아웃과 레지스터 정의, set_bf/get_bf 매크로.
 * 이 파일의 하드웨어 지식이 전부 여기서 온다. */
#include "fsl_pamu.h"

/* [한국어] struct ccsr_guts — GUTS 블록의 pamubypenr 레지스터로
 * PAMU 우회를 제어한다. */
#include <linux/fsl/guts.h>
/* [한국어] request_irq()와 irqreturn_t — 접근 위반 인터럽트 처리용. */
#include <linux/interrupt.h>
/* [한국어] 범용 할당자 헤더. 현재 코드에서 직접 쓰이지는 않고,
 * 과거 서브윈도 할당에 쓰던 흔적이다. */
#include <linux/genalloc.h>
/* [한국어] of_iomap(), of_get_address() 등 디바이스 트리 주소 처리. */
#include <linux/of_address.h>
/* [한국어] irq_of_parse_and_map() — 디바이스 트리에서 IRQ를 얻는다. */
#include <linux/of_irq.h>
/* [한국어] platform_driver/platform_device — probe를 손수 앞당기려고
 * 디바이스를 직접 만드는 데 필요하다. */
#include <linux/platform_device.h>

/* [한국어] SVR_P2040 등 실리콘 버전(SVR) 상수와 mfspr(SPRN_SVR).
 * 에라타 대상 칩을 식별하는 데 쓴다. */
#include <asm/mpc85xx.h>

/* define indexes for each operation mapping scenario */
/* [한국어] OMT(오퍼레이션 매핑 테이블)의 인덱스들.
 * PAACE의 omi 필드에 이 값 중 하나가 들어가, 그 디바이스의 I/O 연산이
 * 어떤 캐시 동작으로 변환될지 결정한다.
 * OMI_QMAN — QMan 포털용. DQRR과 프레임 데이터를 캐시에 스태시한다. */
#define OMI_QMAN        0x00
/* [한국어] FMan(Frame Manager, 네트워크 가속기)용 매핑. */
#define OMI_FMAN        0x01
/* [한국어] QMan의 사설(private) 메모리 접근용 매핑. 포털과 다른 스태시
 * 동작이 필요해 별도 인덱스를 둔다. */
#define OMI_QMAN_PRIV   0x02
/* [한국어] CAAM(암호 가속기)용 매핑. */
#define OMI_CAAM        0x03

/* [한국어] 32비트 두 개를 64비트 하나로 합치는 매크로.
 * PAMU 레지스터가 64비트 주소를 상위/하위 32비트로 나눠 두므로,
 * 인터럽트 핸들러와 LAW 처리에서 이것으로 되붙인다. */
#define make64(high, low) (((u64)(high) << 32) | (low))

/* [한국어] 접근 위반 인터럽트 핸들러에 넘길 데이터.
 * 핸들러가 모든 PAMU 인스턴스를 순회해야 하므로 베이스 주소와 개수가 필요하다.
 * 수명: probe에서 kzalloc되고, 등록된 IRQ가 살아 있는 한 유지된다
 *       (성공 경로에서는 해제되지 않는다 — 부팅 후 영구적이라는 전제). */
struct pamu_isr_data {
	void __iomem *pamu_reg_base;	/* Base address of PAMU regs */
	/* [한국어] 모든 PAMU 인스턴스 레지스터가 매핑된 영역의 시작.
	 * 설정자: fsl_pamu_probe()가 of_iomap 결과를 저장.
	 * 읽는 자: pamu_av_isr()이 i * PAMU_OFFSET을 더해 각 인스턴스에 접근한다.
	 * 값 범위: ioremap된 __iomem 포인터.
	 * 왜 iounmap 하지 않는가: 원본 주석이 밝히듯 ISR이 이 주소를 계속
	 *                         써야 하므로 성공 경로에서는 해제하지 않는다. */

	unsigned int count;		/* The number of PAMUs */
	/* [한국어] 이 SoC에 있는 PAMU 인스턴스의 개수.
	 * 설정자: probe가 (레지스터 영역 크기 / PAMU_OFFSET)로 계산.
	 * 읽는 자: pamu_av_isr()의 순회 상한.
	 * 값 범위: 보통 소켓/버스 개수만큼(수 개).
	 * 동기화: probe 이후 불변. */
};

/* [한국어] PAACT(1차 테이블)의 커널 가상 주소.
 * 설정자: fsl_pamu_probe()가 할당한 큰 블록의 시작을 가리킨다.
 * 읽는 자: pamu_get_ppaace()가 LIODN으로 인덱싱하고, 오류 경로가 해제한다.
 * 값 범위: NULL(초기화 전/실패) 또는 유효한 테이블 포인터.
 * 왜 전역인가: 원본 주석이 밝히듯 모든 PAMU 인스턴스가 같은 LIODN 테이블을
 *              공유하므로, 인스턴스별로 둘 이유가 없다.
 * 동기화: 부팅 시 한 번 설정되고 이후 읽기 전용이다. */
static struct paace *ppaact;
/* [한국어] SPAACT(2차/서브윈도 테이블)의 커널 가상 주소.
 * 설정자: probe가 ppaact 블록 뒤쪽 오프셋으로 계산.
 * 읽는 자: setup_one_pamu()가 물리 주소를 레지스터에 등록할 때만 쓴다.
 * 왜 필요한가: 현재 드라이버는 서브윈도를 쓰지 않지만, 하드웨어가
 *              베이스/리밋 레지스터 설정을 요구하므로 테이블을 잡아 둔다. */
static struct paace *spaact;

/* [한국어] PAMU probe가 이미 실행됐는지 표시하는 플래그.
 * 설정자: fsl_pamu_probe()가 성공 직전에 세운다.
 * 읽는 자: 같은 함수 시작부의 WARN_ON(probed).
 * 왜 필요한가: 이 드라이버는 전역 테이블을 쓰므로 두 번 probe되면
 *              테이블이 덮어써져 이미 등록된 LIODN이 모두 무효화된다.
 *              디바이스 트리에 PAMU 노드가 하나뿐이라는 전제를 강제하는 장치다. */
static bool probed;			/* Has PAMU been probed? */

/*
 * Table for matching compatible strings, for device tree
 * guts node, for QorIQ SOCs.
 * "fsl,qoriq-device-config-2.0" corresponds to T4 & B4
 * SOCs. For the older SOCs "fsl,qoriq-device-config-1.0"
 * string would be used.
 */
/* [한국어] GUTS(Global Utilities) 블록을 찾기 위한 매칭 테이블.
 * GUTS의 pamubypenr 레지스터가 "각 PAMU를 우회할지"를 제어하므로,
 * 초기화를 마친 뒤 그 비트를 내려야 PAMU가 실제로 동작한다.
 * 세대에 따라 compatible 문자열이 다르므로 두 가지를 모두 나열한다. */
static const struct of_device_id guts_device_ids[] = {
	/* [한국어] 구세대 QorIQ(P 계열)의 디바이스 설정 블록. */
	{ .compatible = "fsl,qoriq-device-config-1.0", },
	/* [한국어] T4/B4 세대의 디바이스 설정 블록. */
	{ .compatible = "fsl,qoriq-device-config-2.0", },
	/* [한국어] 배열 끝을 알리는 빈 항목. */
	{}
};

/*
 * Table for matching compatible strings, for device tree
 * L3 cache controller node.
 * "fsl,t4240-l3-cache-controller" corresponds to T4,
 * "fsl,b4860-l3-cache-controller" corresponds to B4 &
 * "fsl,p4080-l3-cache-controller" corresponds to other,
 * SOCs.
 */
/* [한국어] L3(플랫폼) 캐시 컨트롤러를 찾기 위한 매칭 테이블.
 * get_stash_id()가 L3 스태시를 요청받으면 CPU 계층을 거슬러 오르는 대신
 * 이 노드를 곧바로 찾아 cache-stash-id를 읽는다 — L3는 모든 코어가
 * 공유하므로 CPU별로 다를 이유가 없기 때문이다.
 * SoC 세대마다 compatible 문자열이 달라 셋을 나열한다. */
static const struct of_device_id l3_device_ids[] = {
	/* [한국어] T4240 세대의 L3 캐시 컨트롤러. */
	{ .compatible = "fsl,t4240-l3-cache-controller", },
	/* [한국어] B4860 세대의 L3 캐시 컨트롤러. */
	{ .compatible = "fsl,b4860-l3-cache-controller", },
	/* [한국어] P4080 및 그 밖의 SoC의 L3 캐시 컨트롤러. */
	{ .compatible = "fsl,p4080-l3-cache-controller", },
	/* [한국어] 배열 끝. */
	{}
};

/* maximum subwindows permitted per liodn */
/* [한국어] LIODN 하나가 가질 수 있는 최대 서브윈도 개수.
 * 설정자: get_pamu_cap_values()가 PC3 레지스터에서 읽어 계산한다.
 * 읽는 자: 현재 아무도 읽지 않는다 — 서브윈도 지원 코드가 제거되면서
 *          값을 채우는 쪽만 남은 흔적이다.
 * 값 범위: 2^(1 + MWCE) 형태. */
static u32 max_subwindow_count;

/**
 * pamu_get_ppaace() - Return the primary PACCE
 * @liodn: liodn PAACT index for desired PAACE
 *
 * Returns the ppace pointer upon success else return
 * null.
 */
/*
 * [한국어]
 * pamu_get_ppaace - LIODN 번호로 PAACT의 엔트리 포인터를 얻는다
 *
 * @liodn: 찾을 LIODN 번호(= PAACT 배열의 인덱스).
 * @return: 해당 PAACE의 포인터, 테이블이 없거나 범위를 넘으면 NULL.
 *
 * 왜 이 함수를 거치는가: PAACT는 평면 배열이라 인덱싱 자체는 간단하지만,
 * 두 가지 검사가 필요하다 — 테이블이 이미 할당됐는지(probe 전에 불릴 수
 * 있다), 그리고 LIODN이 테이블 크기 안인지. 이것을 한 곳에 모아 두면
 * 모든 호출부가 같은 방어를 공짜로 얻는다.
 *
 * 실행 컨텍스트: PAACE를 만지는 모든 경로(프로세스, 인터럽트 양쪽).
 * 순수 계산이라 락이 필요 없다.
 *
 * 호출 체인:
 *   pamu_enable_liodn()/disable_liodn()/config_ppaace()/update_paace_stash()/
 *   setup_liodns()/pamu_av_isr() → [pamu_get_ppaace]
 */
static struct paace *pamu_get_ppaace(int liodn)
{
	/* [한국어] 테이블이 아직 없거나(probe 전) LIODN이 테이블 크기를 넘으면
	 * 배열 밖을 가리키게 되므로 반드시 걸러야 한다. */
	if (!ppaact || liodn >= PAACE_NUMBER_ENTRIES) {
		pr_debug("PPAACT doesn't exist\n");	/* [한국어] 테이블이 없거나 LIODN이 범위를 벗어났음을 남긴다. */
		return NULL;	/* [한국어] 인덱싱할 수 없으므로 NULL로 알린다. */
	}

	/* [한국어] 평면 배열 인덱싱 — 이것이 PAMU의 "테이블 워크"의 전부다. */
	return &ppaact[liodn];
}

/**
 * pamu_enable_liodn() - Set valid bit of PACCE
 * @liodn: liodn PAACT index for desired PAACE
 *
 * Returns 0 upon success else error code < 0 returned
 */
/*
 * [한국어]
 * pamu_enable_liodn - LIODN의 PAACE를 유효화해 DMA를 허용한다
 *
 * @liodn: 활성화할 LIODN 번호.
 * @return: 0 성공, -ENOENT(엔트리 없음), -EINVAL(아직 구성되지 않음).
 *
 * 왜 WSE를 먼저 확인하는가: 윈도 크기 인코딩(WSE)이 0이면 그 PAACE가
 * 아직 구성되지 않았다는 뜻이다. 구성 없이 V 비트만 세우면 하드웨어가
 * 크기 0짜리 윈도를 해석하게 되어 예측할 수 없는 동작이 된다.
 * 즉 이 검사가 "config → enable" 순서를 강제하는 장치다.
 *
 * 두 개의 mb()가 이 함수의 핵심이다:
 *  - 첫 번째: 앞서 config_ppaace()가 쓴 모든 필드가 메모리에 반영된 뒤에야
 *    V 비트를 세우도록 순서를 세운다. 이것이 없으면 하드웨어가 유효한
 *    엔트리를 보고 아직 채워지지 않은 필드를 읽을 수 있다.
 *  - 두 번째: V 비트 쓰기 자체가 메모리에 도달했음을 보장한다.
 *
 * 실행 컨텍스트: attach 경로(fsl_pamu_domain.c가 iommu_lock을 쥔 상태)와
 * setup_liodns()(부팅 초기).
 *
 * 호출 체인:
 *   fsl_pamu_domain.c의 attach / setup_liodns() → [pamu_enable_liodn]
 */
int pamu_enable_liodn(int liodn)
{
	/* [한국어] 활성화할 PAACE 엔트리. */
	struct paace *ppaace;

	/* [한국어] LIODN에 해당하는 엔트리를 찾는다. */
	ppaace = pamu_get_ppaace(liodn);
	/* [한국어] 테이블이 없거나 범위를 벗어난 LIODN이다. */
	if (!ppaace) {
		pr_debug("Invalid primary paace entry\n");	/* [한국어] 해당 LIODN의 엔트리를 얻지 못했다. */
		return -ENOENT;	/* [한국어] 존재하지 않는 엔트리이므로 활성화할 수 없다. */
	}

	/* [한국어] 윈도 크기가 설정되지 않았다면 이 엔트리는 아직 구성 전이다.
	 * 유효화하면 하드웨어가 의미 없는 윈도를 해석하게 되므로 거부한다. */
	if (!get_bf(ppaace->addr_bitfields, PPAACE_AF_WSE)) {
		pr_debug("liodn %d not configured\n", liodn);	/* [한국어] 윈도 크기가 0이면 아직 구성되지 않은 엔트리다. */
		return -EINVAL;	/* [한국어] 구성 없이 유효화하면 하드웨어가 의미 없는 윈도를 해석한다. */
	}

	/* Ensure that all other stores to the ppaace complete first */
	/* [한국어] 앞선 모든 PAACE 필드 쓰기가 메모리에 반영되도록 강제한다.
	 * 이 배리어가 없으면 하드웨어가 V=1을 먼저 보고 반쯤 채워진 엔트리로
	 * DMA를 통과시킬 수 있다. */
	mb();

	/* [한국어] 유효 비트를 세운다. 이 한 줄이 곧 "이 디바이스의 DMA를
	 * 허용한다"는 선언이다. */
	set_bf(ppaace->addr_bitfields, PAACE_AF_V, PAACE_V_VALID);
	/* [한국어] 그 쓰기가 메모리에 도달했음을 보장한다 — PAMU는 캐시된
	 * 엔트리를 쓰므로, 이후 캐시 무효화가 이 값을 보아야 한다. */
	mb();

	/* [한국어] 활성화 완료. */
	return 0;
}

/**
 * pamu_disable_liodn() - Clears valid bit of PACCE
 * @liodn: liodn PAACT index for desired PAACE
 *
 * Returns 0 upon success else error code < 0 returned
 */
/*
 * [한국어]
 * pamu_disable_liodn - LIODN의 PAACE를 무효화해 DMA를 차단한다
 *
 * @liodn: 비활성화할 LIODN 번호.
 * @return: 0 성공, -ENOENT(엔트리 없음).
 *
 * enable과 달리 앞쪽 mb()가 없는 이유: 차단은 "빨리 될수록 좋은" 방향이라
 * 순서를 지연시킬 이유가 없다. 뒤쪽 mb()만으로 "이 시점 이후 하드웨어가
 * 차단을 본다"를 보장하면 충분하다.
 *
 * 세 곳에서 쓰인다 — detach 경로, PAACE 재구성 직전(pamu_set_liodn),
 * 그리고 접근 위반 핸들러가 문제의 LIODN을 격리할 때.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:
 *   fsl_pamu_domain.c의 detach/set_liodn / pamu_av_isr()
 *   → [pamu_disable_liodn]
 */
int pamu_disable_liodn(int liodn)
{
	/* [한국어] 무효화할 PAACE 엔트리. */
	struct paace *ppaace;

	/* [한국어] LIODN에 해당하는 엔트리를 찾는다. */
	ppaace = pamu_get_ppaace(liodn);
	if (!ppaace) {	/* [한국어] 엔트리를 얻지 못했다면 무효화할 대상도 없다. */
		pr_debug("Invalid primary paace entry\n");	/* [한국어] 잘못된 LIODN임을 남긴다. */
		return -ENOENT;	/* [한국어] 존재하지 않는 엔트리를 알린다. */
	}

	/* [한국어] 유효 비트를 내린다. 이후 그 LIODN의 DMA는 접근 위반으로
	 * 처리된다. */
	set_bf(ppaace->addr_bitfields, PAACE_AF_V, PAACE_V_INVALID);
	/* [한국어] 차단이 메모리에 반영되었음을 보장한다. */
	mb();

	/* [한국어] 차단 완료. */
	return 0;
}

/* Derive the window size encoding for a particular PAACE entry */
/*
 * [한국어]
 * map_addrspace_size_to_wse - 윈도 크기(바이트)를 WSE 인코딩으로 바꾼다
 *
 * @addrspace_size: 윈도의 바이트 크기. 반드시 2의 거듭제곱이어야 한다.
 * @return: PAACE의 WSE 필드에 넣을 값.
 *
 * 계산 근거: 하드웨어가 정의하는 관계는 "실제 크기 = 2^(WSE+1)"이다.
 * fls64(x)는 최상위 1비트의 위치를 1부터 세어 반환하므로,
 * 2^n에 대해 n+1을 준다. 따라서 WSE = fls64(size) - 2가 된다.
 * 예: 4GB(2^32) → fls64 = 33 → WSE = 31 = PAACE_WSE_4G. 맞다.
 *
 * BUG_ON을 쓰는 이유: 2의 거듭제곱이 아닌 크기는 이 인코딩으로 표현할 수
 * 없다. 조용히 잘못된 윈도를 만드느니 즉시 멈추는 편이 안전하다 —
 * 호출부가 상수만 넘기므로 실제로는 발생하지 않는다.
 *
 * 실행 컨텍스트: PAACE 구성 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   pamu_config_ppaace() → [map_addrspace_size_to_wse]
 */
static unsigned int map_addrspace_size_to_wse(phys_addr_t addrspace_size)
{
	/* Bug if not a power of 2 */
	/* [한국어] x & (x-1)이 0이면 2의 거듭제곱이다. 아니면 이 인코딩으로
	 * 표현할 수 없으므로 즉시 멈춘다. */
	BUG_ON(addrspace_size & (addrspace_size - 1));

	/* window size is 2^(WSE+1) bytes */
	/* [한국어] fls64가 주는 "최상위 비트 위치 + 1"에서 2를 빼면
	 * 하드웨어가 기대하는 WSE 값이 된다. */
	return fls64(addrspace_size) - 2;
}

/*
 * Set the PAACE type as primary and set the coherency required domain
 * attribute
 */
/*
 * [한국어]
 * pamu_init_ppaace - PAACE의 공통 기본 속성을 설정한다
 *
 * @ppaace: 초기화할 엔트리.
 * @return: 없음.
 *
 * 모든 Primary PAACE가 공유하는 두 가지를 설정한다:
 *  - PT = PRIMARY: 이 엔트리가 PAACT(1차 테이블)의 것임을 표시한다.
 *    SPAACT 엔트리와 구분하는 비트다.
 *  - coherency_required: 이 디바이스의 접근이 캐시 코히런시 프로토콜에
 *    참여하게 한다. 그래야 드라이버가 명시적 캐시 플러시 없이 DMA를
 *    쓸 수 있다. QMan/BMan의 사설 메모리만 예외로 이 비트를 다시 내린다
 *    (setup_qbman_paace 참조).
 *
 * 실행 컨텍스트: PAACE 구성 경로.
 *
 * 호출 체인:
 *   pamu_config_ppaace() / setup_liodns() → [pamu_init_ppaace]
 */
static void pamu_init_ppaace(struct paace *ppaace)
{
	/* [한국어] 1차 테이블 엔트리임을 표시한다. */
	set_bf(ppaace->addr_bitfields, PAACE_AF_PT, PAACE_PT_PRIMARY);

	/* [한국어] 이 디바이스의 접근이 하드웨어 캐시 코히런시를 갖게 한다.
	 * 대부분의 디바이스에 적합한 기본값이다. */
	set_bf(ppaace->domain_attr.to_host.coherency_required, PAACE_DA_HOST_CR,
	       PAACE_M_COHERENCE_REQ);
}

/*
 * Function used for updating stash destination for the coressponding
 * LIODN.
 */
/*
 * [한국어]
 * pamu_update_paace_stash - LIODN의 스태시 목적지만 런타임에 바꾼다
 *
 * @liodn: 대상 LIODN.
 * @value: 새 스태시 캐시 ID(get_stash_id가 계산한 값).
 * @return: 0 성공, -ENOENT(엔트리 없음).
 *
 * 왜 별도 함수인가: 스태시 목적지는 실행 중에 바뀔 수 있다 — QMan 포털이
 * 어느 CPU에 바인딩되는지가 런타임에 정해지기 때문이다. 전체 PAACE를
 * 다시 쓰면 그 사이 DMA가 잘못된 설정을 볼 수 있으므로, CID 필드 하나만
 * 갱신하는 경로를 따로 둔다.
 *
 * V 비트를 건드리지 않는 점에 주목: 엔트리가 유효한 상태에서 CID만 바뀐다.
 * 스태시 목적지는 정확성이 아니라 성능에 관한 설정이라, 전환 도중에
 * 옛 목적지로 몇 번 스태시되어도 문제가 없기 때문이다.
 *
 * 실행 컨텍스트: fsl_pamu_domain.c가 iommu_lock을 쥔 상태.
 *
 * 호출 체인:
 *   fsl_pamu_configure_l1_stash() → update_domain_stash()
 *   → update_liodn_stash() → [pamu_update_paace_stash]
 */
int pamu_update_paace_stash(int liodn, u32 value)
{
	/* [한국어] 갱신할 PAACE 엔트리. */
	struct paace *paace;

	/* [한국어] LIODN에 해당하는 엔트리를 찾는다. */
	paace = pamu_get_ppaace(liodn);
	if (!paace) {	/* [한국어] 갱신할 엔트리를 얻지 못했다. */
		pr_debug("Invalid liodn entry\n");	/* [한국어] 잘못된 LIODN임을 남긴다. */
		return -ENOENT;	/* [한국어] 스태시 목적지를 바꿀 대상이 없다. */
	}
	/* [한국어] impl_attr의 CID(캐시 ID) 필드만 새 값으로 갈아 끼운다.
	 * set_bf가 나머지 필드(WCE/ATM/OTM)를 보존해 준다. */
	set_bf(paace->impl_attr, PAACE_IA_CID, value);

	/* [한국어] 갱신이 메모리에 반영되었음을 보장한다. */
	mb();

	/* [한국어] 갱신 완료. */
	return 0;
}

/**
 * pamu_config_ppaace() - Sets up PPAACE entry for specified liodn
 *
 * @liodn: Logical IO device number
 * @omi: Operation mapping index -- if ~omi == 0 then omi not defined
 * @stashid: cache stash id for associated cpu -- if ~stashid == 0 then
 *	     stashid not defined
 * @prot: window permissions
 *
 * Returns 0 upon success else error code < 0 returned
 */
/*
 * [한국어]
 * pamu_config_ppaace - LIODN 하나의 PAACE를 구성한다
 *
 * @liodn: 구성할 LIODN 번호.
 * @omi: OME 인덱스. ~omi == 0(즉 0xFFFFFFFF)이면 "지정하지 않음"을 뜻한다.
 * @stashid: 스태시 캐시 ID. 역시 ~stashid == 0이면 "지정하지 않음".
 * @prot: 윈도 접근 권한(PAACE_AP_PERMS_* 조합).
 * @return: 0 성공, -ENOENT(엔트리 없음), -ENODEV(잘못된 OME 인덱스).
 *
 * "~x == 0"이라는 관례가 이 함수의 인터페이스를 이해하는 열쇠다.
 * 0은 유효한 인덱스/ID이므로 "미지정"을 표현할 수 없다. 그래서 모든 비트가
 * 1인 값(~0)을 미지정 표식으로 쓰고, ~x == 0으로 그것을 판별한다.
 * fsl_pamu_domain.c의 pamu_set_liodn()이 두 번 호출하면서 이 관례를
 * 활용한다 — 첫 호출은 omi를 지정하고, 두 번째는 ~0으로 넘겨 이미 설정된
 * OME를 건드리지 않게 한다.
 *
 * 윈도를 64GB로 고정하는 이유: 이 드라이버는 주소 변환을 하지 않고 접근
 * 제어만 하므로, 윈도가 물리 주소 공간 전체(36비트 = 64GB)를 덮으면 된다.
 * 그러면서도 ATM을 WINDOW_XLATE로 두는데, 변환된 베이스(twbah/TWBAL)를
 * 0으로 두므로 결과적으로 항등 변환이 된다.
 *
 * 실행 컨텍스트: attach 경로. fsl_pamu_domain.c가 iommu_lock을 쥔 상태이며,
 * 호출 전에 pamu_disable_liodn()으로 엔트리가 무효화되어 있어야 안전하다.
 *
 * 호출 체인:
 *   fsl_pamu_domain.c의 pamu_set_liodn() → [pamu_config_ppaace]
 *   → map_addrspace_size_to_wse(), pamu_init_ppaace()
 */
int pamu_config_ppaace(int liodn, u32 omi, u32 stashid, int prot)
{
	/* [한국어] 구성할 PAACE 엔트리. */
	struct paace *ppaace;

	/* [한국어] LIODN에 해당하는 엔트리를 찾는다. */
	ppaace = pamu_get_ppaace(liodn);
	if (!ppaace)	/* [한국어] 구성할 엔트리가 없으므로 더 진행할 수 없다. */
		return -ENOENT;

	/* window size is 2^(WSE+1) bytes */
	/* [한국어] 윈도 크기를 2^36(64GB)으로 설정한다 — QorIQ의 36비트 물리
	 * 주소 공간 전체를 덮는다는 뜻이라 사실상 "제한 없음"이다.
	 * 이 필드가 0이 아니게 되는 것이 pamu_enable_liodn()의 전제 조건이기도 하다. */
	set_bf(ppaace->addr_bitfields, PPAACE_AF_WSE,
	       map_addrspace_size_to_wse(1ULL << 36));

	/* [한국어] 1차 엔트리 표시와 코히런시 요구를 설정한다. */
	pamu_init_ppaace(ppaace);

	/* [한국어] 윈도 베이스 주소의 상위 비트를 0으로 — 윈도가 물리 주소
	 * 0에서 시작한다. */
	ppaace->wbah = 0;
	/* [한국어] 윈도 베이스의 하위 비트도 0으로. 위 wbah와 합쳐
	 * "0부터 64GB까지"라는 윈도가 완성된다. */
	set_bf(ppaace->addr_bitfields, PPAACE_AF_WBAL, 0);

	/* set up operation mapping if it's configured */
	/* [한국어] 유효한 OME 인덱스가 주어졌다면 indexed 오퍼레이션 변환
	 * 모드를 켜고 그 인덱스를 기록한다 — 이것이 스태시를 가능하게 하는 설정이다. */
	if (omi < OME_NUMBER_ENTRIES) {
		set_bf(ppaace->impl_attr, PAACE_IA_OTM, PAACE_OTM_INDEXED);	/* [한국어] indexed 오퍼레이션 변환 모드를 켜 OMT를 참조하게 한다. */
		ppaace->op_encode.index_ot.omi = omi;
	/* [한국어] 미지정 표식(~0)도 아니고 유효 범위도 아니라면 잘못된 인덱스다.
	 * 그대로 두면 하드웨어가 OMT 밖을 읽게 되므로 거부한다. */
	} else if (~omi != 0) {
		pr_debug("bad operation mapping index: %d\n", omi);	/* [한국어] 미지정 표식도 아니고 유효 범위도 아닌 인덱스임을 남긴다. */
		return -ENODEV;	/* [한국어] 그대로 두면 하드웨어가 OMT 밖을 읽으므로 거부한다. */
	}

	/* configure stash id */
	/* [한국어] 스태시 ID가 지정되었다면 CID 필드에 기록한다.
	 * 미지정(~0)이면 건드리지 않아 기존 값이 유지된다. */
	if (~stashid != 0)
		set_bf(ppaace->impl_attr, PAACE_IA_CID, stashid);

	/* [한국어] 주소 변환 모드를 "윈도 변환"으로 설정한다. 아래에서
	 * 변환 베이스를 0으로 두므로 결과적으로 항등 변환이 된다. */
	set_bf(ppaace->impl_attr, PAACE_IA_ATM, PAACE_ATM_WINDOW_XLATE);
	/* [한국어] 변환된 윈도 베이스의 상위 비트를 0으로. */
	ppaace->twbah = 0;
	/* [한국어] 변환된 윈도 베이스의 하위 비트도 0으로 — 입력 주소가
	 * 그대로 출력 주소가 된다. */
	set_bf(ppaace->win_bitfields, PAACE_WIN_TWBAL, 0);
	/* [한국어] 접근 권한을 설정한다. 호출자가 0(전면 거부) 또는
	 * QUERY|UPDATE(읽기+쓰기)를 넘긴다. */
	set_bf(ppaace->addr_bitfields, PAACE_AF_AP, prot);
	/* [한국어] 서브윈도 개수를 0으로 — 이 드라이버는 서브윈도를 쓰지 않는다. */
	set_bf(ppaace->impl_attr, PAACE_IA_WCE, 0);
	/* [한국어] 다중 윈도 비트도 내려 SPAACT를 참조하지 않게 한다. */
	set_bf(ppaace->addr_bitfields, PPAACE_AF_MW, 0);
	/* [한국어] 모든 필드 쓰기가 메모리에 반영되었음을 보장한다.
	 * 이후 pamu_enable_liodn()이 V 비트를 세울 때 완성된 엔트리를 보게 된다. */
	mb();

	/* [한국어] 구성 완료. 아직 V 비트는 서지 않았으므로 DMA는 차단 상태다. */
	return 0;
}

/**
 * get_ome_index() - Returns the index in the operation mapping table
 *                   for device.
 * @omi_index: pointer for storing the index value
 * @dev: target device
 *
 */
/*
 * [한국어]
 * get_ome_index - 디바이스에 맞는 OME 인덱스를 고른다
 *
 * @omi_index: 출력 인자 — 선택된 인덱스를 여기 저장한다.
 * @dev: 대상 디바이스.
 * @return: 없음.
 *
 * 왜 QMan만 특별한가: OMT는 "이 디바이스의 I/O 연산을 어떤 캐시 동작으로
 * 바꿀지"를 정의한다. 대부분의 디바이스는 평범한 읽기/쓰기면 충분하지만,
 * QMan은 DPAA의 큐 관리자로서 프레임 데이터를 CPU 캐시에 미리 밀어 넣어야
 * 네트워크 처리 지연이 줄어든다. 그 특수 동작이 OMI_QMAN과 OMI_QMAN_PRIV에
 * 정의되어 있다(setup_omt 참조).
 *
 * 주의: 매칭되지 않으면 *omi_index를 건드리지 않는다. 호출자
 * (pamu_set_liodn)가 ~0으로 초기화해 두므로, 그대로 "OME 미지정"이 되어
 * pamu_config_ppaace()가 OMT를 쓰지 않는다.
 *
 * 실행 컨텍스트: attach 경로.
 *
 * 호출 체인:
 *   fsl_pamu_domain.c의 pamu_set_liodn() → [get_ome_index]
 */
void get_ome_index(u32 *omi_index, struct device *dev)
{
	/* [한국어] QMan 포털(코어가 큐에 접근하는 창구)이면 DQRR과 프레임
	 * 스태시가 정의된 OMI_QMAN을 쓴다. */
	if (of_device_is_compatible(dev->of_node, "fsl,qman-portal"))
		*omi_index = OMI_QMAN;
	/* [한국어] QMan 본체(사설 메모리에 접근)라면 다른 스태시 동작이
	 * 필요하므로 OMI_QMAN_PRIV를 쓴다.
	 * else가 아닌 별도 if인데, 두 compatible이 동시에 참일 수 없어
	 * 실질적으로는 배타적이다. */
	if (of_device_is_compatible(dev->of_node, "fsl,qman"))
		*omi_index = OMI_QMAN_PRIV;
}

/**
 * get_stash_id - Returns stash destination id corresponding to a
 *                cache type and vcpu.
 * @stash_dest_hint: L1, L2 or L3
 * @vcpu: vpcu target for a particular cache type.
 *
 * Returs stash on success or ~(u32)0 on failure.
 *
 */
/*
 * [한국어]
 * get_stash_id - 캐시 계층을 거슬러 올라가며 스태시 목적지 ID를 찾는다
 *
 * @stash_dest_hint: 원하는 캐시 레벨(PAMU_ATTR_CACHE_L1/L2/L3).
 * @vcpu: 대상 CPU 번호(L1/L2에서만 의미가 있다).
 * @return: 스태시 캐시 ID, 찾지 못하면 ~(u32)0.
 *
 * 왜 디바이스 트리를 뒤지는가: 스태시 목적지 ID는 하드웨어가 캐시마다
 * 부여한 번호이고, 그 값은 SoC마다 다르다. 리눅스는 그것을 알 방법이 없어
 * 디바이스 트리의 "cache-stash-id" 프로퍼티에 적어 두고 읽는다.
 *
 * 두 갈래로 나뉜다:
 *  - L3(플랫폼 캐시): 모든 코어가 공유하므로 CPU와 무관하다. l3_device_ids로
 *    캐시 컨트롤러 노드를 직접 찾아 읽고 끝낸다(빠른 경로).
 *  - L1/L2: CPU마다 다르므로, 먼저 vcpu에 해당하는 CPU 노드를 찾고,
 *    거기서 "next-level-cache" 링크를 따라 원하는 레벨까지 올라간다.
 *    CPU 노드 자체가 L1을 표현하고, 그 next-level-cache가 L2다.
 *
 * 참조 카운트 처리에 주의: for_each_of_cpu_node는 순회 중 자동으로
 * 참조를 관리하지만, goto로 빠져나오면 마지막 노드의 참조가 잡힌 채로
 * 남는다. 그 뒤의 코드가 of_node_put으로 그것을 내린다.
 *
 * 알려진 취약점: CPU를 찾지 못하면(found == 0) 아래 루프가 실행되지 않고
 * 곧바로 실패 메시지로 가는데, 이때 node는 순회가 끝난 상태라 NULL이다 —
 * 다행히 그 경로에서는 node를 쓰지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(attach)와 부팅 초기(setup_qbman_paace).
 *
 * 호출 체인:
 *   fsl_pamu_domain.c의 configure_l1_stash() / setup_qbman_paace()
 *   → [get_stash_id]
 */
u32 get_stash_id(u32 stash_dest_hint, u32 vcpu)
{
	/* [한국어] 디바이스 트리에서 읽은 프로퍼티 값(빅엔디언 u32 배열). */
	const u32 *prop;
	/* [한국어] 순회 중인 디바이스 트리 노드. */
	struct device_node *node;
	/* [한국어] 캐시 계층을 올라가며 세는 현재 레벨. */
	u32 cache_level;
	/* [한국어] len은 프로퍼티 바이트 길이, found는 CPU 노드를 찾았는지 표시. */
	int len, found = 0;
	/* [한국어] reg 프로퍼티 배열 순회 인덱스. */
	int i;

	/* Fastpath, exit early if L3/CPC cache is target for stashing */
	/* [한국어] L3(플랫폼 캐시)는 모든 코어가 공유하므로 CPU 계층을
	 * 거슬러 오를 필요가 없다. 캐시 컨트롤러 노드를 직접 찾는다. */
	if (stash_dest_hint == PAMU_ATTR_CACHE_L3) {
		/* [한국어] 세 가지 compatible 중 하나에 맞는 L3 컨트롤러를 찾는다. */
		node = of_find_matching_node(NULL, l3_device_ids);
		if (node) {
			/* [한국어] 그 노드에서 스태시 ID를 읽는다. */
			prop = of_get_property(node, "cache-stash-id", NULL);
			/* [한국어] 프로퍼티가 없으면 이 SoC의 트리가 스태시를
			 * 지원하지 않는다는 뜻이다. 노드 참조를 내리고 실패를 알린다. */
			if (!prop) {
				pr_debug("missing cache-stash-id at %pOF\n",	/* [한국어] 이 SoC의 트리가 L3 스태시 ID를 기술하지 않았음을 남긴다. */
					 node);
				of_node_put(node);	/* [한국어] 실패 경로에서도 노드 참조를 반드시 내린다. */
				return ~(u32)0;	/* [한국어] 스태시 목적지를 찾지 못했음을 관례값으로 알린다. */
			}
			/* [한국어] 값을 읽기 전에 참조를 내려도 안전하다 —
			 * prop이 가리키는 데이터는 트리 자체의 메모리라
			 * 노드 참조와 별개로 유효하다. */
			of_node_put(node);
			/* [한국어] 디바이스 트리는 빅엔디언이므로 CPU 엔디언으로 변환한다. */
			return be32_to_cpup(prop);
		}
		/* [한국어] L3 컨트롤러 노드 자체가 없다 — 이 SoC에는 L3가 없거나
		 * 트리에 기술되지 않았다. */
		return ~(u32)0;
	}

	/* [한국어] L1/L2는 CPU마다 다르므로, 먼저 vcpu에 해당하는 CPU 노드를
	 * 찾아야 한다. 모든 CPU 노드를 순회한다. */
	for_each_of_cpu_node(node) {
		/* [한국어] CPU 노드의 reg 프로퍼티가 그 CPU의 하드웨어 번호다.
		 * 배열일 수 있어(스레드가 여럿인 코어) 전부 확인한다. */
		prop = of_get_property(node, "reg", &len);
		for (i = 0; i < len / sizeof(u32); i++) {
			/* [한국어] 찾는 vcpu와 일치하면 이 노드가 출발점이다. */
			if (be32_to_cpup(&prop[i]) == vcpu) {
				found = 1;	/* [한국어] 찾는 vcpu에 해당하는 CPU 노드를 발견했다. */
				goto found_cpu_node;	/* [한국어] 이 노드를 출발점으로 캐시 계층을 오르러 간다. */
			}
		}
	}
/* [한국어] CPU 노드를 찾았을 때 도달하는 지점. 찾지 못하면 순회가 자연히
 * 끝나 found가 0인 채로 여기 도달하고, 아래 루프가 실행되지 않는다. */
found_cpu_node:

	/* find the hwnode that represents the cache */
	/* [한국어] CPU 노드(=L1)에서 시작해 원하는 레벨까지 캐시 계층을 올라간다.
	 * found가 0이면 루프 조건이 처음부터 거짓이라 아래 실패 경로로 간다. */
	for (cache_level = PAMU_ATTR_CACHE_L1; (cache_level < PAMU_ATTR_CACHE_L3) && found; cache_level++) {
		/* [한국어] 원하는 레벨에 도달했다면 그 노드에서 스태시 ID를 읽는다. */
		if (stash_dest_hint == cache_level) {
			prop = of_get_property(node, "cache-stash-id", NULL);
			/* [한국어] 이 레벨의 캐시가 스태시를 지원하지 않는다. */
			if (!prop) {
				pr_debug("missing cache-stash-id at %pOF\n",	/* [한국어] 이 레벨의 캐시가 스태시 ID를 기술하지 않았음을 남긴다. */
					 node);
				of_node_put(node);	/* [한국어] 실패 경로의 노드 참조 해제. */
				return ~(u32)0;	/* [한국어] 스태시 목적지를 찾지 못했음을 알린다. */
			}
			/* [한국어] 노드 참조를 내리고 값을 반환한다. */
			of_node_put(node);
			return be32_to_cpup(prop);	/* [한국어] 빅엔디언 트리 값을 CPU 엔디언으로 바꿔 반환한다. */
		}

		/* [한국어] 아직 원하는 레벨이 아니므로 다음 레벨로 올라간다.
		 * next-level-cache 프로퍼티가 상위 캐시 노드의 phandle을 담는다. */
		prop = of_get_property(node, "next-level-cache", NULL);
		/* [한국어] 링크가 없으면 이 캐시가 계층의 끝이다 — 원하는 레벨에
		 * 도달하기 전에 막혔으므로 실패다. */
		if (!prop) {
			pr_debug("can't find next-level-cache at %pOF\n", node);	/* [한국어] 상위 캐시로 가는 링크가 없어 계층의 끝에 닿았다. */
			of_node_put(node);	/* [한국어] 더 오를 수 없으니 노드 참조를 내린다. */
			return ~(u32)0;  /* can't traverse any further */	/* [한국어] 원하는 레벨에 도달하지 못했음을 알린다. */
		}
		/* [한국어] 다음 노드로 옮기기 전에 현재 노드의 참조를 내린다. */
		of_node_put(node);

		/* advance to next node in cache hierarchy */
		/* [한국어] phandle로 상위 캐시 노드를 찾는다. 참조가 하나 올라간다. */
		node = of_find_node_by_phandle(*prop);
		/* [한국어] phandle이 가리키는 노드가 없다면 트리가 손상된 것이다. */
		if (!node) {
			pr_debug("Invalid node for cache hierarchy\n");	/* [한국어] phandle이 가리키는 노드가 없다면 트리가 손상된 것이다. */
			return ~(u32)0;	/* [한국어] 계층을 더 따라갈 수 없으므로 실패를 알린다. */
		}
	}

	/* [한국어] 루프를 다 돌았는데도 원하는 레벨을 만나지 못했거나,
	 * 애초에 CPU를 찾지 못한 경우다. */
	pr_debug("stash dest not found for %d on vcpu %d\n",
		 stash_dest_hint, vcpu);
	return ~(u32)0;	/* [한국어] CPU를 찾지 못했거나 원하는 레벨을 만나지 못했다. */
}

/* Identify if the PAACT table entry belongs to QMAN, BMAN or QMAN Portal */
/* [한국어] setup_qbman_paace()에 넘기는 디바이스 종류 구분자.
 * DPAA의 세 블록만 특별 취급이 필요해 별도 상수로 두었다.
 * QMAN_PAACE — QMan 본체(사설 메모리 접근). */
#define QMAN_PAACE 1
/* [한국어] QMan 포털 — 코어가 큐에 접근하는 창구. 프레임 데이터 스태시가 필요하다. */
#define QMAN_PORTAL_PAACE 2
/* [한국어] BMan — 버퍼 관리자. 코히런시만 끄면 되고 스태시는 쓰지 않는다. */
#define BMAN_PAACE 3

/*
 * Setup operation mapping and stash destinations for QMAN and QMAN portal.
 * Memory accesses to QMAN and BMAN private memory need not be coherent, so
 * clear the PAACE entry coherency attribute for them.
 */
/*
 * [한국어]
 * setup_qbman_paace - DPAA 블록(QMan/BMan)에 특화된 PAACE 설정을 적용한다
 *
 * @ppaace: 이미 기본 구성이 끝난 PAACE 엔트리.
 * @paace_type: QMAN_PAACE / QMAN_PORTAL_PAACE / BMAN_PAACE 중 하나.
 * @return: 없음.
 *
 * 왜 이 세 블록만 특별한가: DPAA(Data Path Acceleration Architecture)는
 * 네트워크 패킷을 코어 개입 없이 처리하는 하드웨어 경로다. 그 성능이
 * 두 가지에 달려 있다 —
 *  1) **스태시**: QMan이 프레임 디스크립터와 데이터를 미리 L3 캐시에
 *     넣어 두면, 코어가 그것을 처리할 때 메모리 지연을 겪지 않는다.
 *     그래서 QMan과 그 포털에 OMT 인덱스와 스태시 목적지를 지정한다.
 *  2) **코히런시 해제**: QMan/BMan의 사설 메모리는 하드웨어만 쓰고
 *     CPU가 보지 않는다. 코히런시 프로토콜에 참여시키면 불필요한
 *     스누핑 트래픽만 생기므로 원본 주석대로 그 비트를 내린다.
 *     포털은 코어가 직접 읽고 쓰므로 코히런시를 유지한다.
 *
 * L3를 스태시 목적지로 쓰는 이유: 이 시점(부팅 초기)에는 어느 코어가
 * 그 큐를 처리할지 모른다. L3는 모든 코어가 공유하므로 안전한 기본값이며,
 * 나중에 qman_portal이 CPU를 정하면 fsl_pamu_configure_l1_stash()가
 * L1으로 좁혀 준다.
 *
 * 실행 컨텍스트: 부팅 초기 setup_liodns()에서만 호출된다.
 *
 * 호출 체인:
 *   setup_liodns() → [setup_qbman_paace] → get_stash_id()
 */
static void setup_qbman_paace(struct paace *ppaace, int  paace_type)
{
	/* [한국어] 디바이스 종류에 따라 갈라 처리한다. 인자 앞의 공백 두 칸은
	 * 원본 그대로다. */
	switch (paace_type) {
	/* [한국어] QMan 본체 — 사설 메모리 접근용 설정. */
	case QMAN_PAACE:
		/* [한국어] indexed 오퍼레이션 변환을 켜 OMT를 참조하게 한다. */
		set_bf(ppaace->impl_attr, PAACE_IA_OTM, PAACE_OTM_INDEXED);
		/* [한국어] 사설 메모리용 매핑(읽기는 RSA, 쓰기는 WWSA로 변환)을 쓴다. */
		ppaace->op_encode.index_ot.omi = OMI_QMAN_PRIV;
		/* setup QMAN Private data stashing for the L3 cache */
		/* [한국어] 스태시 목적지를 L3 캐시로 지정한다. vcpu 0을 넘기지만
		 * L3 경로는 CPU를 보지 않으므로 값이 무의미하다. */
		set_bf(ppaace->impl_attr, PAACE_IA_CID, get_stash_id(PAMU_ATTR_CACHE_L3, 0));
		/* [한국어] 코히런시 요구를 해제한다 — 사설 메모리는 CPU가 보지
		 * 않으므로 스누핑이 낭비다. pamu_init_ppaace()가 세워 둔 것을
		 * 여기서 되돌리는 셈이다. */
		set_bf(ppaace->domain_attr.to_host.coherency_required, PAACE_DA_HOST_CR,
		       0);
		break;
	/* [한국어] QMan 포털 — 코어가 직접 접근하는 창구. */
	case QMAN_PORTAL_PAACE:
		/* [한국어] indexed 변환을 켠다. */
		set_bf(ppaace->impl_attr, PAACE_IA_OTM, PAACE_OTM_INDEXED);
		/* [한국어] 포털용 매핑(DQRR과 프레임 스태시가 정의된 것)을 쓴다. */
		ppaace->op_encode.index_ot.omi = OMI_QMAN;
		/* Set DQRR and Frame stashing for the L3 cache */
		/* [한국어] 역시 L3를 기본 스태시 목적지로 둔다. 나중에
		 * qman_portal이 CPU에 바인딩되면 L1으로 좁혀진다.
		 * 코히런시는 그대로 유지하는 점이 QMan 본체와 다르다 —
		 * 포털 메모리는 코어가 직접 읽고 쓰기 때문이다. */
		set_bf(ppaace->impl_attr, PAACE_IA_CID, get_stash_id(PAMU_ATTR_CACHE_L3, 0));
		break;
	/* [한국어] BMan — 버퍼 관리자. */
	case BMAN_PAACE:
		/* [한국어] 코히런시만 해제한다. 스태시는 쓰지 않으므로 OMT
		 * 설정이 없다 — BMan은 버퍼 포인터만 다뤄 캐시에 미리 넣을
		 * 데이터가 없기 때문이다. */
		set_bf(ppaace->domain_attr.to_host.coherency_required, PAACE_DA_HOST_CR,
		       0);
		break;
	}
}

/*
 * Setup the operation mapping table for various devices. This is a static
 * table where each table index corresponds to a particular device. PAMU uses
 * this table to translate device transaction to appropriate corenet
 * transaction.
 */
/*
 * [한국어]
 * setup_omt - 오퍼레이션 매핑 테이블(OMT)을 채운다
 *
 * @omt: 채울 OMT 배열의 시작(OME_NUMBER_ENTRIES개).
 * @return: 없음.
 *
 * 이 함수가 PAMU 스태시의 실체다. OMT는 "디바이스가 낸 I/O 연산을
 * 코히런시 패브릭에 어떤 연산으로 내보낼지"를 정의하는 룩업 테이블이고,
 * 그 변환에서 평범한 읽기를 "스태시 할당을 동반한 읽기"로 바꾸는 것이
 * 곧 DMA 데이터를 캐시에 미리 넣는 동작이 된다.
 *
 * 네 인덱스의 성격이 뚜렷하다:
 *  - OMI_QMAN(포털): 읽기는 그대로, 확장 읽기(EREAD0)는 RSA(스태시 할당
 *    동반 읽기)로, 확장 쓰기(EWRITE0)는 WWSAO(스태시 할당만, 메모리
 *    쓰기 없음)로 바꾼다. 디렉티브 두 개는 외부 캐시 적재(LDEC/LDECPE)로
 *    바꿔, QMan이 명시적으로 "이것을 캐시에 올려 두라"고 지시할 수 있게 한다.
 *  - OMI_FMAN(네트워크 가속기): 읽기를 READI(무효화 동반 읽기)로 바꾼다.
 *    수신 버퍼를 읽으면서 다른 캐시의 사본을 없애 이후 쓰기를 빠르게 한다.
 *  - OMI_QMAN_PRIV: 확장 쓰기를 WWSA(스태시 할당 + 메모리 쓰기)로 바꾼다.
 *    포털의 WWSAO와 달리 메모리에도 쓰는데, 사설 데이터는 나중에
 *    다시 읽힐 수 있기 때문이다.
 *  - OMI_CAAM(암호 가속기): FMan과 같은 구성.
 *
 * EOE_VALID를 항상 OR 하는 이유: 그 비트가 없으면 하드웨어가 그 칸을
 * "매핑 없음"으로 보고 변환하지 않는다.
 *
 * 실행 컨텍스트: 부팅 초기 probe에서 한 번.
 *
 * 호출 체인:
 *   fsl_pamu_probe() → [setup_omt]
 */
static void setup_omt(struct ome *omt)
{
	/* [한국어] 현재 채우는 OME를 가리키는 커서. */
	struct ome *ome;

	/* Configure OMI_QMAN */
	/* [한국어] QMan 포털용 엔트리를 채운다. */
	ome = &omt[OMI_QMAN];

	/* [한국어] 평범한 읽기는 변환 없이 그대로 내보낸다. */
	ome->moe[IOE_READ_IDX] = EOE_VALID | EOE_READ;
	/* [한국어] 확장 읽기 0을 RSA로 바꾼다 — 읽으면서 그 데이터를
	 * CID가 지정한 캐시에 할당한다. 프레임 데이터를 미리 캐시에
	 * 올리는 핵심 동작이다. */
	ome->moe[IOE_EREAD0_IDX] = EOE_VALID | EOE_RSA;
	/* [한국어] 평범한 쓰기는 그대로. */
	ome->moe[IOE_WRITE_IDX] = EOE_VALID | EOE_WRITE;
	/* [한국어] 확장 쓰기 0을 WWSAO로 바꾼다 — 캐시에만 할당하고
	 * 메모리에는 쓰지 않는다. 곧 코어가 읽을 데이터라 메모리 왕복이 낭비이기 때문이다. */
	ome->moe[IOE_EWRITE0_IDX] = EOE_VALID | EOE_WWSAO;

	/* [한국어] 디렉티브 0을 외부 캐시 적재(LDEC)로 바꾼다 — QMan이
	 * "이 라인을 캐시에 올려 두라"고 명시적으로 지시할 수 있게 한다. */
	ome->moe[IOE_DIRECT0_IDX] = EOE_VALID | EOE_LDEC;
	/* [한국어] 디렉티브 1은 선호 배타(preferred exclusive) 적재로 바꾼다 —
	 * 곧 쓰기가 예상되는 라인에 쓴다. */
	ome->moe[IOE_DIRECT1_IDX] = EOE_VALID | EOE_LDECPE;

	/* Configure OMI_FMAN */
	/* [한국어] 네트워크 가속기(FMan)용 엔트리. */
	ome = &omt[OMI_FMAN];
	/* [한국어] 읽기를 READI(무효화 동반 읽기)로 바꾼다 — 수신 버퍼를
	 * 읽으면서 다른 캐시의 사본을 없애, 이후 그 버퍼에 쓰는 동작이
	 * 코히런시 트래픽 없이 진행되게 한다. */
	ome->moe[IOE_READ_IDX]  = EOE_VALID | EOE_READI;
	/* [한국어] 쓰기는 변환하지 않는다. */
	ome->moe[IOE_WRITE_IDX] = EOE_VALID | EOE_WRITE;

	/* Configure OMI_QMAN private */
	/* [한국어] QMan 본체(사설 메모리)용 엔트리. */
	ome = &omt[OMI_QMAN_PRIV];
	/* [한국어] 평범한 읽기는 그대로. */
	ome->moe[IOE_READ_IDX]  = EOE_VALID | EOE_READ;
	/* [한국어] 평범한 쓰기도 그대로. */
	ome->moe[IOE_WRITE_IDX] = EOE_VALID | EOE_WRITE;
	/* [한국어] 확장 읽기를 스태시 할당 동반 읽기로 바꾼다. */
	ome->moe[IOE_EREAD0_IDX] = EOE_VALID | EOE_RSA;
	/* [한국어] 확장 쓰기를 WWSA로 바꾼다 — 포털의 WWSAO와 달리 메모리에도
	 * 쓴다. 사설 데이터는 캐시에서 축출된 뒤 다시 읽힐 수 있기 때문이다. */
	ome->moe[IOE_EWRITE0_IDX] = EOE_VALID | EOE_WWSA;

	/* Configure OMI_CAAM */
	/* [한국어] 암호 가속기용 엔트리. FMan과 같은 구성이다 —
	 * 대량의 데이터를 한 번 읽고 처리하는 성격이 비슷하기 때문이다. */
	ome = &omt[OMI_CAAM];
	/* [한국어] 읽기를 무효화 동반 읽기로. */
	ome->moe[IOE_READ_IDX]  = EOE_VALID | EOE_READI;
	/* [한국어] 쓰기는 변환하지 않는다. */
	ome->moe[IOE_WRITE_IDX] = EOE_VALID | EOE_WRITE;
}

/*
 * Get the maximum number of PAACT table entries
 * and subwindows supported by PAMU
 */
/*
 * [한국어]
 * get_pamu_cap_values - PAMU의 능력 레지스터를 읽어 한계값을 파악한다
 *
 * @pamu_reg_base: PAMU 인스턴스 하나의 레지스터 베이스(정수형 주소).
 * @return: 없음.
 *
 * 현재는 서브윈도 최대 개수만 읽는다. 함수 이름과 원본 주석이 "PAACT 엔트리
 * 개수"도 읽는다고 하지만, 그 코드는 제거되었다 — PAACT 크기를 하드코딩
 * (PAACE_NUMBER_ENTRIES)하는 쪽으로 바뀌었기 때문이다(fsl_pamu.h 참조).
 *
 * 계산 근거: MWCE(Maximum Window Count Encoding) 필드가 n이면
 * 최대 서브윈도 개수는 2^(n+1)이다.
 *
 * 정수형 주소를 u32*로 캐스팅해 in_be32에 넘기는 점에 주목: 이 코드는
 * ioremap된 주소를 unsigned long으로 들고 다니는 옛 스타일을 쓴다.
 * 현대적인 __iomem 표기와 어긋나지만 PowerPC에서는 동작한다.
 *
 * 실행 컨텍스트: 부팅 초기 probe에서 한 번.
 *
 * 호출 체인:
 *   fsl_pamu_probe() → [get_pamu_cap_values] → in_be32()
 */
static void get_pamu_cap_values(unsigned long pamu_reg_base)
{
	/* [한국어] PC3 레지스터에서 읽은 값. */
	u32 pc_val;

	/* [한국어] 능력 레지스터 3을 빅엔디언으로 읽는다. PowerPC의
	 * 하드웨어 레지스터는 빅엔디언이므로 in_be32를 써야 한다. */
	pc_val = in_be32((u32 *)(pamu_reg_base + PAMU_PC3));
	/* Maximum number of subwindows per liodn */
	/* [한국어] MWCE 필드를 뽑아 2^(MWCE+1)로 최대 서브윈도 개수를 구한다.
	 * 현재 이 값을 읽는 코드는 없지만, 하드웨어 능력을 파악하는 절차로
	 * 남아 있다. */
	max_subwindow_count = 1 << (1 + PAMU_PC3_MWCE(pc_val));
}

/* Setup PAMU registers pointing to PAACT, SPAACT and OMT */
/*
 * [한국어]
 * setup_one_pamu - PAMU 인스턴스 하나에 테이블 위치를 알리고 활성화한다
 *
 * @pamu_reg_base: 이 인스턴스의 레지스터 베이스.
 * @pamu_reg_size: 레지스터 영역 크기. 현재 쓰이지 않는다(호출부가
 *                 오프셋을 넘기고 있어 이름과도 어긋난다).
 * @ppaact_phys: PAACT 테이블의 물리 주소.
 * @spaact_phys: SPAACT 테이블의 물리 주소.
 * @omt_phys: OMT 테이블의 물리 주소.
 * @return: 항상 0(실패할 수 있는 동작이 없다).
 *
 * 세 테이블마다 베이스와 리밋을 한 쌍으로 등록하는 것이 이 함수의 전부다.
 * 리밋은 베이스 + 테이블 크기로 계산하는데, 인자로 받은 물리 주소 변수를
 * 그 자리에서 더해 재사용한다 — 지역 변수라 호출자에 영향이 없다.
 *
 * 리밋을 왜 등록하는가: 하드웨어가 인덱싱 결과가 테이블 범위를 벗어나는지
 * 스스로 검사할 수 있게 된다. 잘못된 LIODN이 엉뚱한 메모리를 PAACE로
 * 해석하는 사고를 하드웨어 차원에서 막는 장치다.
 *
 * 마지막 두 줄이 실제 활성화다:
 *  - PICS에 ACCESS_VIOLATION_ENABLE: 접근 위반 시 인터럽트를 내게 한다.
 *  - PC에 PE|OCE|SPCC|PPCC: PAMU를 켜고, OMT/SPAACE/PPAACE 캐시를 모두
 *    활성화한다. 캐시가 켜지므로 이후 PAACE를 고칠 때 무효화가 필요해진다.
 *
 * 실행 컨텍스트: 부팅 초기 probe. 인스턴스마다 한 번씩 호출된다.
 *
 * 호출 체인:
 *   fsl_pamu_probe()의 인스턴스 순회 루프 → [setup_one_pamu] → out_be32()
 */
static int setup_one_pamu(unsigned long pamu_reg_base, unsigned long pamu_reg_size,
			  phys_addr_t ppaact_phys, phys_addr_t spaact_phys,
			  phys_addr_t omt_phys)
{
	/* [한국어] PAMU 제어 레지스터를 가리키는 포인터. */
	u32 *pc;
	/* [한국어] 테이블 베이스/리밋 레지스터 묶음. */
	struct pamu_mmap_regs *pamu_regs;

	/* [한국어] 제어 레지스터의 주소를 계산한다. */
	pc = (u32 *) (pamu_reg_base + PAMU_PC);
	/* [한국어] 테이블 주소 레지스터들이 모여 있는 구조체의 주소를 계산한다.
	 * 오프셋 0이므로 사실상 베이스 그 자체다. */
	pamu_regs = (struct pamu_mmap_regs *)
		(pamu_reg_base + PAMU_MMAP_REGS_BASE);

	/* set up pointers to corenet control blocks */

	/* [한국어] PAACT 베이스 주소의 상위 32비트를 기록한다.
	 * 빅엔디언 하드웨어이므로 out_be32를 쓴다. */
	out_be32(&pamu_regs->ppbah, upper_32_bits(ppaact_phys));
	/* [한국어] PAACT 베이스의 하위 32비트. */
	out_be32(&pamu_regs->ppbal, lower_32_bits(ppaact_phys));
	/* [한국어] 리밋 계산을 위해 지역 변수에 테이블 크기를 더한다.
	 * 지역 복사본이라 호출자의 값은 그대로다. */
	ppaact_phys = ppaact_phys + PAACT_SIZE;
	/* [한국어] PAACT 리밋의 상위 32비트를 기록한다. */
	out_be32(&pamu_regs->pplah, upper_32_bits(ppaact_phys));
	/* [한국어] PAACT 리밋의 하위 32비트. 이 범위를 벗어나는 LIODN
	 * 인덱싱은 하드웨어가 접근 위반으로 잡아낸다. */
	out_be32(&pamu_regs->pplal, lower_32_bits(ppaact_phys));

	/* [한국어] SPAACT 베이스의 상위 32비트. 서브윈도를 쓰지 않아도
	 * 하드웨어가 설정을 요구하므로 등록한다. */
	out_be32(&pamu_regs->spbah, upper_32_bits(spaact_phys));
	/* [한국어] SPAACT 베이스의 하위 32비트. */
	out_be32(&pamu_regs->spbal, lower_32_bits(spaact_phys));
	/* [한국어] SPAACT 리밋 계산. */
	spaact_phys = spaact_phys + SPAACT_SIZE;
	/* [한국어] SPAACT 리밋의 상위 32비트. */
	out_be32(&pamu_regs->splah, upper_32_bits(spaact_phys));
	/* [한국어] SPAACT 리밋의 하위 32비트. */
	out_be32(&pamu_regs->splal, lower_32_bits(spaact_phys));

	/* [한국어] OMT 베이스의 상위 32비트. */
	out_be32(&pamu_regs->obah, upper_32_bits(omt_phys));
	/* [한국어] OMT 베이스의 하위 32비트. */
	out_be32(&pamu_regs->obal, lower_32_bits(omt_phys));
	/* [한국어] OMT 리밋 계산. */
	omt_phys = omt_phys + OMT_SIZE;
	/* [한국어] OMT 리밋의 상위 32비트. */
	out_be32(&pamu_regs->olah, upper_32_bits(omt_phys));
	/* [한국어] OMT 리밋의 하위 32비트. 잘못된 omi 인덱스가 테이블 밖을
	 * 가리키는 것을 막는다. */
	out_be32(&pamu_regs->olal, lower_32_bits(omt_phys));

	/*
	 * set PAMU enable bit,
	 * allow ppaact & omt to be cached
	 * & enable PAMU access violation interrupts.
	 */

	/* [한국어] 접근 위반 인터럽트를 활성화한다. 이것이 없으면 위반이
	 * 조용히 무시되어 잘못된 DMA를 발견할 수 없다. */
	out_be32((u32 *)(pamu_reg_base + PAMU_PICS),
		 PAMU_ACCESS_VIOLATION_ENABLE);
	/* [한국어] PAMU를 켜고 세 가지 캐시를 모두 활성화한다.
	 * PE: 검사 활성화. OCE: OMT 캐시. SPCC/PPCC: SPAACE/PPAACE 캐시.
	 * 캐시를 켜므로 이후 PAACE를 고칠 때 하드웨어가 옛 값을 볼 수 있고,
	 * 그래서 PAACE 조작 함수들이 mb()로 가시성을 보장하는 것이다. */
	out_be32(pc, PAMU_PC_PE | PAMU_PC_OCE | PAMU_PC_SPCC | PAMU_PC_PPCC);
	/* [한국어] 실패할 수 있는 동작이 없어 항상 성공을 반환한다.
	 * 호출부도 반환값을 검사하지 않는다. */
	return 0;
}

/* Enable all device LIODNS */
/*
 * [한국어]
 * setup_liodns - 디바이스 트리에 적힌 모든 LIODN을 기본 구성으로 활성화한다
 *
 * @return: 없음.
 *
 * 왜 부팅 시 전부 활성화하는가: 이 시점에는 아직 IOMMU 도메인이 없고,
 * QMan 같은 클라이언트는 곧바로 DMA를 시작한다. 그래서 모든 LIODN을
 * "전체 주소 공간 허용, 변환 없음"으로 열어 두어 부팅이 진행되게 한다.
 * 나중에 fsl_pamu_domain.c가 특정 디바이스를 도메인에 붙이면 그 LIODN의
 * PAACE가 다시 구성된다.
 *
 * pamu_config_ppaace()와 설정이 다른 점에 주목:
 *  - WSE를 35로 직접 넣는다. 2^36 = 64GB로 같은 결과지만, 여기서는
 *    상수를 하드코딩했다.
 *  - ATM을 NO_XLATE로 둔다(변환 자체를 끈다). config_ppaace는
 *    WINDOW_XLATE에 베이스 0을 넣어 항등 변환을 만드는데, 결과는 같다.
 *  - 권한을 처음부터 PERMS_ALL로 연다. config_ppaace가 2단계로 나눠
 *    권한을 나중에 여는 것과 대비된다 — 여기서는 경쟁 상대가 없어
 *    그럴 필요가 없다.
 *
 * 마지막으로 QMan/BMan이면 특수 설정을 덧붙이고, mb() 후 활성화한다.
 *
 * 실행 컨텍스트: 부팅 초기 probe. 단일 스레드라 락이 없다.
 *
 * 호출 체인:
 *   fsl_pamu_probe() → [setup_liodns] → pamu_init_ppaace(),
 *   setup_qbman_paace(), pamu_enable_liodn()
 */
static void setup_liodns(void)
{
	/* [한국어] i는 LIODN 배열 인덱스, len은 프로퍼티 바이트 길이. */
	int i, len;
	/* [한국어] 구성할 PAACE 엔트리. */
	struct paace *ppaace;
	/* [한국어] 순회 중인 디바이스 트리 노드. */
	struct device_node *node = NULL;
	/* [한국어] fsl,liodn 프로퍼티의 값(빅엔디언 u32 배열). */
	const u32 *prop;

	/* [한국어] "fsl,liodn" 프로퍼티를 가진 모든 노드를 순회한다.
	 * u-boot이 각 DMA 마스터에 이 프로퍼티를 심어 두었다. */
	for_each_node_with_property(node, "fsl,liodn") {
		/* [한국어] 그 노드의 LIODN 배열을 읽는다. */
		prop = of_get_property(node, "fsl,liodn", &len);
		/* [한국어] 한 디바이스가 여러 LIODN을 가질 수 있으므로 전부 처리한다. */
		for (i = 0; i < len / sizeof(u32); i++) {
			/* [한국어] 현재 처리 중인 LIODN 번호. */
			int liodn;

			/* [한국어] 디바이스 트리는 빅엔디언이므로 변환해 읽는다. */
			liodn = be32_to_cpup(&prop[i]);
			/* [한국어] PAACT 크기를 넘는 LIODN은 테이블 밖을 가리키므로
			 * 건너뛴다. u-boot 설정 오류에 대한 방어다. */
			if (liodn >= PAACE_NUMBER_ENTRIES) {
				pr_debug("Invalid LIODN value %d\n", liodn);	/* [한국어] PAACT 범위를 벗어난 LIODN임을 남긴다. */
				continue;	/* [한국어] 이 LIODN은 건너뛰고 다음 항목을 처리한다. */
			}
			/* [한국어] 해당 엔트리를 얻는다. 위에서 범위를 검사했으므로
			 * NULL이 아님이 보장된다. */
			ppaace = pamu_get_ppaace(liodn);
			/* [한국어] 1차 엔트리 표시와 코히런시 요구를 설정한다. */
			pamu_init_ppaace(ppaace);
			/* window size is 2^(WSE+1) bytes */
			/* [한국어] 윈도 크기를 2^36(64GB)으로 — 물리 주소 공간
			 * 전체를 덮어 사실상 제한을 두지 않는다. */
			set_bf(ppaace->addr_bitfields, PPAACE_AF_WSE, 35);
			/* [한국어] 윈도 베이스의 상위 비트를 0으로. */
			ppaace->wbah = 0;
			/* [한국어] 윈도 베이스의 하위 비트도 0으로 — 0부터 시작한다. */
			set_bf(ppaace->addr_bitfields, PPAACE_AF_WBAL, 0);
			/* [한국어] 주소 변환을 아예 끈다 — 디바이스가 낸 주소가
			 * 그대로 물리 주소가 된다. */
			set_bf(ppaace->impl_attr, PAACE_IA_ATM,
			       PAACE_ATM_NO_XLATE);
			/* [한국어] 읽기와 쓰기를 모두 허용한다. 부팅 중에는
			 * 격리보다 동작이 우선이다. */
			set_bf(ppaace->addr_bitfields, PAACE_AF_AP,
			       PAACE_AP_PERMS_ALL);
			/* [한국어] QMan 포털이면 프레임 스태시 설정을 덧붙인다. */
			if (of_device_is_compatible(node, "fsl,qman-portal"))
				setup_qbman_paace(ppaace, QMAN_PORTAL_PAACE);
			/* [한국어] QMan 본체면 사설 메모리용 설정을 덧붙인다. */
			if (of_device_is_compatible(node, "fsl,qman"))
				setup_qbman_paace(ppaace, QMAN_PAACE);
			/* [한국어] BMan이면 코히런시만 해제한다. */
			if (of_device_is_compatible(node, "fsl,bman"))
				setup_qbman_paace(ppaace, BMAN_PAACE);
			/* [한국어] 모든 필드 쓰기가 메모리에 반영되도록 보장한다.
			 * pamu_enable_liodn()도 내부에 mb()를 갖고 있어 중복이지만,
			 * 명시적으로 순서를 드러내는 편이 읽기 좋다. */
			mb();
			/* [한국어] V 비트를 세워 이 LIODN의 DMA를 허용한다. */
			pamu_enable_liodn(liodn);
		}
	}
}

/*
 * [한국어]
 * pamu_av_isr - 접근 위반(access violation) 인터럽트 핸들러
 *
 * @irq: 발생한 IRQ 번호(사용하지 않는다).
 * @arg: request_irq에 넘긴 struct pamu_isr_data 포인터.
 * @return: 항상 IRQ_HANDLED.
 *
 * 왜 pr_emerg인가: 접근 위반은 디바이스가 허용되지 않은 메모리에 DMA를
 * 시도했다는 뜻이다. 시스템 메모리가 이미 손상되었을 수 있고, 계속 두면
 * 더 손상될 수 있으므로 가장 높은 로그 레벨을 쓴다.
 *
 * 동작 과정:
 *  1) 모든 PAMU 인스턴스를 순회하며 PICS에서 위반 상태를 확인한다.
 *  2) 위반이 있으면 관련 레지스터를 전부 덤프한다 — 오류 상태(POES1/2),
 *     위반 상태(AVS1/2), 위반 주소(AVA), 그리고 문제의 PAACE 주소(POEA).
 *  3) POEA가 PAACE를 가리킨다고 가정하고 그 엔트리의 앞 네 워드도 덤프한다.
 *     "가정"인 이유는 하드웨어가 항상 그렇다고 보장하지 않기 때문이다.
 *  4) 위반 상태 비트를 지운다(write-1-to-clear).
 *  5) AVS1의 상위 비트에서 문제의 LIODN을 뽑아 그 엔트리를 확인한다.
 *     - 이미 무효한 LIODN이었다면 에라타 A-003638에 해당한다. 하드웨어가
 *       잘못 보고한 것이므로, 인터럽트 폭주를 막기 위해 이 PAMU의
 *       위반 보고 자체를 꺼 버린다.
 *     - 유효했다면 진짜 위반이므로 그 LIODN을 비활성화해 격리한다.
 *  6) 갱신된 PICS를 기록한다.
 *
 * BUG_ON 두 개: PAACE를 찾지 못하거나 비활성화가 실패하면 시스템 상태를
 * 신뢰할 수 없다고 보고 즉시 멈춘다. 메모리 손상이 진행 중일 수 있는
 * 상황이라 계속 도는 것보다 멈추는 편이 안전하다는 판단이다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. 여러 줄의 pr_emerg가 콘솔을
 * 오래 붙잡을 수 있지만, 이 상황에서는 진단이 우선이다.
 *
 * 호출 체인:
 *   하드웨어 IRQ → [pamu_av_isr] → pamu_get_ppaace(), pamu_disable_liodn()
 */
static irqreturn_t pamu_av_isr(int irq, void *arg)
{
	/* [한국어] probe가 넘긴 레지스터 베이스와 인스턴스 개수. */
	struct pamu_isr_data *data = arg;
	/* [한국어] POEA 레지스터에서 읽은, 문제의 PAACE 물리 주소. */
	phys_addr_t phys;
	/* [한국어] i는 PAMU 인스턴스 인덱스, j는 PAACE 워드 인덱스,
	 * ret은 비활성화 결과. */
	unsigned int i, j, ret;

	/* [한국어] 가장 높은 로그 레벨로 알린다 — 메모리 손상 가능성이 있는
	 * 심각한 상황이기 때문이다. */
	pr_emerg("access violation interrupt\n");

	/* [한국어] 모든 PAMU 인스턴스를 확인한다. IRQ가 공유되므로 어느
	 * 인스턴스가 위반을 보고했는지 찾아야 한다. */
	for (i = 0; i < data->count; i++) {
		/* [한국어] i번째 인스턴스의 레지스터 베이스를 계산한다. */
		void __iomem *p = data->pamu_reg_base + i * PAMU_OFFSET;
		/* [한국어] 인터럽트 제어/상태 레지스터를 읽는다. */
		u32 pics = in_be32(p + PAMU_PICS);

		/* [한국어] 이 인스턴스가 위반을 보고했는지 확인한다. */
		if (pics & PAMU_ACCESS_VIOLATION_STAT) {
			/* [한국어] 위반 상태 레지스터를 읽어 둔다. 원인 비트와
			 * 위반 LIODN이 함께 들어 있어 여러 번 쓰인다. */
			u32 avs1 = in_be32(p + PAMU_AVS1);
			/* [한국어] 아래에서 문제의 PAACE를 담을 포인터. */
			struct paace *paace;

			/* [한국어] 오퍼레이션 오류 상태 1. */
			pr_emerg("POES1=%08x\n", in_be32(p + PAMU_POES1));
			/* [한국어] 오퍼레이션 오류 상태 2. */
			pr_emerg("POES2=%08x\n", in_be32(p + PAMU_POES2));
			/* [한국어] 접근 위반 상태 1 — 원인 비트와 LIODN이 여기 있다. */
			pr_emerg("AVS1=%08x\n", avs1);
			/* [한국어] 접근 위반 상태 2 — 추가 상태 비트. */
			pr_emerg("AVS2=%08x\n", in_be32(p + PAMU_AVS2));
			/* [한국어] 위반을 일으킨 접근 주소(64비트를 두 레지스터에서
			 * 합쳐 만든다). 어느 주소에 접근하려 했는지 알려 준다. */
			pr_emerg("AVA=%016llx\n",
				 make64(in_be32(p + PAMU_AVAH),
					in_be32(p + PAMU_AVAL)));
			/* [한국어] 사용자 정의 속성 감지 레지스터. */
			pr_emerg("UDAD=%08x\n", in_be32(p + PAMU_UDAD));
			/* [한국어] 오류를 일으킨 오퍼레이션의 주소 — 대개 문제의
			 * PAACE가 있는 위치다. */
			pr_emerg("POEA=%016llx\n",
				 make64(in_be32(p + PAMU_POEAH),
					in_be32(p + PAMU_POEAL)));

			/* [한국어] 그 주소를 따로 담아 아래에서 덤프에 쓴다. */
			phys = make64(in_be32(p + PAMU_POEAH),
				      in_be32(p + PAMU_POEAL));

			/* Assume that POEA points to a PAACE */
			/* [한국어] POEA가 유효한 주소라면 그것을 PAACE로 가정하고
			 * 내용을 덤프한다. "가정"인 이유는 하드웨어가 항상 PAACE를
			 * 가리킨다고 보장하지 않기 때문이다 — 진단 목적이라
			 * 틀려도 큰 문제가 없다는 판단이다. */
			if (phys) {
				/* [한국어] 물리 주소를 커널 가상 주소로 바꾼다.
				 * PAACT가 lowmem에 있으므로 phys_to_virt가 통한다. */
				u32 *paace = phys_to_virt(phys);

				/* Only the first four words are relevant */
				/* [한국어] PAACE는 16워드지만 진단에 필요한 정보
				 * (주소 비트필드, 도메인 속성, 구현 속성)가
				 * 앞 네 워드에 모여 있다. */
				for (j = 0; j < 4; j++)
					pr_emerg("PAACE[%u]=%08x\n",
						 j, in_be32(paace + j));
			}

			/* clear access violation condition */
			/* [한국어] 위반 상태 비트들을 지운다. write-1-to-clear이므로
			 * 현재 값과 마스크를 AND 해서 쓰면 선 비트만 지워진다. */
			out_be32(p + PAMU_AVS1, avs1 & PAMU_AV_MASK);
			/* [한국어] AVS1의 상위 16비트에서 위반 LIODN을 뽑아
			 * 그 엔트리를 얻는다. */
			paace = pamu_get_ppaace(avs1 >> PAMU_AVS1_LIODN_SHIFT);
			/* [한국어] 하드웨어가 보고한 LIODN이 테이블 범위를 벗어났다면
			 * 시스템 상태를 신뢰할 수 없다 — 즉시 멈춘다. */
			BUG_ON(!paace);
			/* check if we got a violation for a disabled LIODN */
			/* [한국어] 이미 무효한 LIODN에 대한 위반 보고인지 확인한다. */
			if (!get_bf(paace->addr_bitfields, PAACE_AF_V)) {
				/*
				 * As per hardware erratum A-003638, access
				 * violation can be reported for a disabled
				 * LIODN. If we hit that condition, disable
				 * access violation reporting.
				 */
				/* [한국어] 에라타 A-003638: 비활성 LIODN에 대해
				 * 하드웨어가 위반을 잘못 보고할 수 있다. 이 경우
				 * 인터럽트가 끝없이 반복될 수 있으므로, 이 PAMU의
				 * 위반 보고 자체를 꺼 버린다. 진단 능력을 잃는
				 * 대가로 시스템이 멈추지 않게 하는 절충이다. */
				pics &= ~PAMU_ACCESS_VIOLATION_ENABLE;
			} else {
				/* Disable the LIODN */
				/* [한국어] 진짜 위반이다. 그 디바이스를 격리해
				 * 더 이상의 메모리 손상을 막는다. */
				ret = pamu_disable_liodn(avs1 >> PAMU_AVS1_LIODN_SHIFT);
				/* [한국어] 방금 유효함을 확인한 엔트리이므로
				 * 비활성화가 실패할 이유가 없다. 실패했다면
				 * 자료구조가 손상된 것이라 즉시 멈춘다. */
				BUG_ON(ret);
				pr_emerg("Disabling liodn %x\n",	/* [한국어] 격리한 LIODN 번호를 남겨 어느 디바이스가 문제였는지 알린다. */
					 avs1 >> PAMU_AVS1_LIODN_SHIFT);
			}
			/* [한국어] 갱신된 PICS를 기록한다. 에라타 경로였다면
			 * 위반 보고가 꺼진 값이, 아니면 원래 값이 그대로 쓰인다. */
			out_be32((p + PAMU_PICS), pics);
		}
	}

	/* [한국어] 위반을 발견하지 못했더라도 IRQ_HANDLED를 반환한다 —
	 * 이 IRQ는 PAMU 전용이므로 다른 핸들러가 없기 때문이다. */
	return IRQ_HANDLED;
}

/* [한국어] 아래는 LAW(Local Access Window) 레지스터를 다루는 정의들이다.
 * LAW는 CoreNet 패브릭에서 "어느 물리 주소 범위를 어느 타깃(메모리
 * 컨트롤러 등)으로 보낼지"를 정하는 창인데, 그 속성 레지스터에
 * 코히런시 서브도메인 ID를 함께 넣을 수 있다. create_csd()가 그것을
 * 이용해 에라타를 우회한다.
 * LAWAR_EN — 이 LAW 항목이 활성 상태임을 나타내는 비트. */
#define LAWAR_EN		0x80000000
/* [한국어] 타깃 필드의 마스크(비트 20~27) — 이 주소 범위를 처리할
 * 하드웨어 블록을 지정한다. */
#define LAWAR_TARGET_MASK	0x0FF00000
/* [한국어] 타깃 필드의 시프트량. */
#define LAWAR_TARGET_SHIFT	20
/* [한국어] 크기 필드의 마스크(하위 6비트). 실제 크기는 2^(값+1)이다. */
#define LAWAR_SIZE_MASK		0x0000003F
/* [한국어] 코히런시 서브도메인 ID 필드의 마스크(비트 12~19).
 * 문서화되지 않은 필드이지만 에라타 우회에 필요하다. */
#define LAWAR_CSDID_MASK	0x000FF000
/* [한국어] CSDID 필드의 시프트량. */
#define LAWAR_CSDID_SHIFT	12

/* [한국어] LAW 크기 인코딩에서 4KB에 해당하는 값. get_order()가 준
 * 페이지 차수를 여기 더하면 원하는 크기의 인코딩이 된다. */
#define LAW_SIZE_4K		0xb

/* [한국어] LAW 레지스터 하나의 레이아웃(16바이트).
 * 하드웨어가 이 배열을 그대로 읽으므로 필드 순서와 크기를 바꿀 수 없다.
 * 설정자/읽는 자: create_csd()가 직접 MMIO로 읽고 쓴다. */
struct ccsr_law {
	u32	lawbarh;	/* LAWn base address high */
	/* [한국어] 이 창이 덮는 물리 주소 범위의 시작(상위 32비트).
	 * 읽는 자: create_csd()가 버퍼가 어느 창에 속하는지 찾을 때.
	 * 설정자: 새 창을 만들 때 대상 버퍼의 주소를 기록한다. */

	u32	lawbarl;	/* LAWn base address low */
	/* [한국어] 범위 시작의 하위 32비트. lawbarh와 합쳐 64비트 주소가 된다. */

	u32	lawar;		/* LAWn attributes */
	/* [한국어] 속성 워드 — 활성 비트(EN), 타깃, CSDID, 크기가 모두 여기 있다.
	 * 이 레지스터를 마지막에 쓰는 것이 중요한데, EN 비트가 여기 있어
	 * 주소를 먼저 확정한 뒤 활성화해야 하기 때문이다. */

	u32	reserved;
	/* [한국어] 예약 워드 — 구조체를 16바이트로 맞춰 배열 인덱싱이
	 * 시프트로 끝나게 한다. 0으로 두어야 한다. */
};

/*
 * Create a coherence subdomain for a given memory block.
 */
/*
 * [한국어]
 * create_csd - 지정한 메모리 블록에 코히런시 서브도메인을 만든다
 *
 * @phys: 대상 메모리 블록의 물리 주소(PAACT/SPAACT/OMT를 담은 블록).
 * @size: 그 블록의 크기.
 * @csd_port_id: 이 영역을 스누핑할 포트들의 비트맵(SVR별로 다르다).
 * @return: 0 성공, -ENODEV/-ENOMEM/-ENOENT(각 단계 실패).
 *
 * 왜 이런 것이 필요한가: 에라타 A-004510 때문이다. 영향을 받는 실리콘에서는
 * PAMU가 읽는 테이블 메모리가 CPU 캐시와 제대로 코히런시를 유지하지
 * 못한다. 소프트웨어가 PAACE를 고쳐도 PAMU가 옛 값을 볼 수 있다는 뜻이다.
 * 우회책은 그 메모리 영역을 덮는 LAW 창을 새로 만들고, 그 창에 "이
 * 영역은 이 포트들이 스누핑한다"는 코히런시 서브도메인 ID를 붙이는 것이다.
 * 그러면 하드웨어가 그 범위의 접근에 대해 스누핑을 강제해 일관성이 회복된다.
 *
 * 동작 과정:
 *  1) corenet-law 노드에서 LAW 개수를 읽고 레지스터를 매핑한다.
 *  2) corenet-cf 노드에서 CSD 개수를 읽고 CSDID 레지스터를 매핑한다.
 *  3) 비어 있는 CSD ID를 하나 찾아 포트 비트맵을 등록한다.
 *  4) 대상 버퍼를 덮는 기존 DDR LAW를 찾아 그 타깃 값을 알아낸다.
 *  5) 그 LAW보다 **우선순위가 높은**(인덱스가 작은) 빈 슬롯을 찾는다.
 *     LAW는 인덱스가 작을수록 우선하므로, 기존 창을 덮어쓰려면
 *     반드시 앞쪽 슬롯이어야 한다.
 *  6) 새 창에 주소와 속성을 기록한다. 주소를 먼저 쓰고 wmb() 뒤에
 *     속성(EN 포함)을 쓰는 순서가 중요하다 — 반대로 하면 활성화된
 *     창이 잠시 엉뚱한 주소를 덮는다.
 *
 * 알려진 문제: 이 함수는 여러 곳에서 np를 of_node_put 하고 다시 할당하는데,
 * error 레이블도 np를 해제한다. 중간에 np = NULL로 지우는 줄이 그
 * 이중 해제를 막는 장치다. 또 num_laws/num_csds가 있음에도 새 LAW를
 * 찾지 못하는 경우의 처리가 다소 성기다(i == 0 검사).
 *
 * 실행 컨텍스트: 부팅 초기 probe. 잠들 수 있다.
 *
 * 호출 체인:
 *   fsl_pamu_probe() → [create_csd] → of_iomap(), of_get_property()
 */
static int create_csd(phys_addr_t phys, size_t size, u32 csd_port_id)
{
	/* [한국어] 순회/조회 중인 디바이스 트리 노드. */
	struct device_node *np;
	/* [한국어] 트리에서 읽은 빅엔디언 프로퍼티 값. */
	const __be32 *iprop;
	/* [한국어] LAC(Local Access Control) 레지스터 블록의 매핑 주소.
	 * NULL 초기화는 error 레이블의 조건부 iounmap을 위한 것이다. */
	void __iomem *lac = NULL;	/* Local Access Control registers */
	/* [한국어] LAW 레지스터 배열(LAC 안의 오프셋 0xC00). */
	struct ccsr_law __iomem *law;
	/* [한국어] CCM(CoreNet Coherency Manager) 레지스터 블록의 매핑 주소. */
	void __iomem *ccm = NULL;
	/* [한국어] CSDID 레지스터 배열(CCM 안의 오프셋 0x600). */
	u32 __iomem *csdids;
	/* [한국어] i는 순회 인덱스, num_laws/num_csds는 각 배열의 크기. */
	unsigned int i, num_laws, num_csds;
	/* [한국어] 대상 버퍼를 덮는 기존 LAW의 타깃 값. 새 창에도 같은
	 * 타깃을 써야 접근이 올바른 메모리 컨트롤러로 간다. */
	u32 law_target = 0;
	/* [한국어] 새로 할당할 코히런시 서브도메인 ID. */
	u32 csd_id = 0;
	/* [한국어] 결과 코드. */
	int ret = 0;

	/* [한국어] LAW 레지스터를 가진 노드를 찾는다. */
	np = of_find_compatible_node(NULL, NULL, "fsl,corenet-law");
	/* [한국어] 이 SoC에 CoreNet LAW가 없다면 우회책을 쓸 수 없다. */
	if (!np)
		return -ENODEV;

	/* [한국어] LAW 항목이 몇 개인지 읽는다 — 순회 상한이 된다. */
	iprop = of_get_property(np, "fsl,num-laws", NULL);
	if (!iprop) {	/* [한국어] LAW 개수 프로퍼티가 없다. */
		ret = -ENODEV;	/* [한국어] 이 SoC에서는 우회책을 쓸 수 없다. */
		goto error;	/* [한국어] 매핑을 정리하러 간다. */
	}

	/* [한국어] 빅엔디언 값을 CPU 엔디언으로 변환한다. */
	num_laws = be32_to_cpup(iprop);
	/* [한국어] 0개라면 쓸 수 있는 창이 없다는 뜻이다. */
	if (!num_laws) {
		ret = -ENODEV;	/* [한국어] LAW가 하나도 없다면 창을 만들 수 없다. */
		goto error;	/* [한국어] 정리 경로로 간다. */
	}

	/* [한국어] LAC 레지스터 블록을 매핑한다. */
	lac = of_iomap(np, 0);
	if (!lac) {	/* [한국어] LAC 레지스터 매핑에 실패했다. */
		ret = -ENODEV;	/* [한국어] 레지스터에 접근할 수 없으므로 포기한다. */
		goto error;	/* [한국어] 정리 경로로 간다. */
	}

	/* LAW registers are at offset 0xC00 */
	/* [한국어] LAW 배열은 LAC 블록 안 고정 오프셋에 있다. */
	law = lac + 0xC00;

	/* [한국어] LAW 노드는 더 필요 없으므로 참조를 내린다.
	 * 아래에서 np를 다른 노드로 재사용한다. */
	of_node_put(np);

	/* [한국어] 코히런시 관리자(CoreNet Fabric) 노드를 찾는다. */
	np = of_find_compatible_node(NULL, NULL, "fsl,corenet-cf");
	if (!np) {	/* [한국어] 코히런시 관리자 노드를 찾지 못했다. */
		ret = -ENODEV;	/* [한국어] 서브도메인을 만들 수 없다. */
		goto error;	/* [한국어] 앞서 매핑한 LAC를 정리하러 간다. */
	}

	/* [한국어] 사용 가능한 코히런시 서브도메인 ID 개수를 읽는다. */
	iprop = of_get_property(np, "fsl,ccf-num-csdids", NULL);
	if (!iprop) {	/* [한국어] CSD 개수 프로퍼티가 없다. */
		ret = -ENODEV;	/* [한국어] 서브도메인 ID를 할당할 수 없다. */
		goto error;	/* [한국어] 정리 경로로 간다. */
	}

	/* [한국어] 빅엔디언 변환. */
	num_csds = be32_to_cpup(iprop);
	/* [한국어] 0개면 서브도메인을 만들 수 없다. */
	if (!num_csds) {
		ret = -ENODEV;	/* [한국어] 사용 가능한 CSD가 하나도 없다. */
		goto error;	/* [한국어] 정리 경로로 간다. */
	}

	/* [한국어] CCM 레지스터 블록을 매핑한다. */
	ccm = of_iomap(np, 0);
	if (!ccm) {	/* [한국어] CCM 레지스터 매핑에 실패했다. */
		ret = -ENOMEM;	/* [한국어] 메모리 부족으로 보고한다. */
		goto error;	/* [한국어] 정리 경로로 간다. */
	}

	/* The undocumented CSDID registers are at offset 0x600 */
	/* [한국어] CSDID 레지스터 배열의 위치. 원본 주석이 밝히듯 문서화되지
	 * 않은 레지스터라, 이 오프셋은 하드웨어 팀의 정보에 의존한다. */
	csdids = ccm + 0x600;

	/* [한국어] CCF 노드도 더 필요 없으므로 참조를 내린다. */
	of_node_put(np);
	/* [한국어] np를 NULL로 지운다 — error 레이블이 조건부로 of_node_put을
	 * 부르므로, 이것이 없으면 이중 해제가 된다. */
	np = NULL;

	/* Find an unused coherence subdomain ID */
	/* [한국어] 값이 0인(=사용되지 않은) CSD ID를 찾는다. */
	for (csd_id = 0; csd_id < num_csds; csd_id++) {
		if (!csdids[csd_id])	/* [한국어] 값이 0인 슬롯이 아직 쓰이지 않은 CSD ID다. */
			break;
	}

	/* Store the Port ID in the (undocumented) proper CIDMRxx register */
	/* [한국어] 그 슬롯에 포트 비트맵을 기록해 서브도메인을 정의한다.
	 * 주의: 위 루프가 빈 슬롯을 찾지 못하고 끝나면 csd_id == num_csds가
	 * 되어 배열 밖에 쓰게 된다 — 기존 코드의 허점이지만, CSD가 소진되는
	 * 상황은 실무에서 일어나지 않는다는 전제다. */
	csdids[csd_id] = csd_port_id;

	/* Find the DDR LAW that maps to our buffer. */
	/* [한국어] 대상 버퍼가 어느 기존 LAW 창에 속하는지 찾는다.
	 * 새 창에도 같은 타깃을 써야 접근이 올바른 메모리 컨트롤러로 간다. */
	for (i = 0; i < num_laws; i++) {
		/* [한국어] 활성 상태인 창만 검사한다. */
		if (law[i].lawar & LAWAR_EN) {
			/* [한국어] 이 창이 덮는 주소 범위. */
			phys_addr_t law_start, law_end;

			/* [한국어] 상위/하위 레지스터를 합쳐 시작 주소를 만든다. */
			law_start = make64(law[i].lawbarh, law[i].lawbarl);
			/* [한국어] 크기 필드가 n이면 실제 크기는 2^(n+1)이다.
			 * 2ULL << n이 정확히 그 값을 준다. */
			law_end = law_start +
				(2ULL << (law[i].lawar & LAWAR_SIZE_MASK));

			/* [한국어] 대상 버퍼가 이 창 안에 있다면 그 타깃을 기억하고
			 * 순회를 멈춘다. i 값도 아래에서 재사용된다. */
			if (law_start <= phys && phys < law_end) {
				law_target = law[i].lawar & LAWAR_TARGET_MASK;	/* [한국어] 이 창의 타깃을 기억해 새 창에도 같은 값을 쓴다. */
				break;
			}
		}
	}

	/* [한국어] 두 가지 비정상 상황을 함께 거른다.
	 * i == num_laws: 버퍼를 덮는 창을 찾지 못했다.
	 * i == 0: 찾긴 했는데 0번 창이라, 그보다 우선순위 높은 슬롯이 없다.
	 * 원본 주석대로 둘 다 일어나서는 안 되는 상황이다. */
	if (i == 0 || i == num_laws) {
		/* This should never happen */
		ret = -ENOENT;	/* [한국어] 덮는 창이 없거나 0번이라 앞쪽 슬롯을 확보할 수 없다. */
		goto error;	/* [한국어] 정리 경로로 간다. */
	}

	/* Find a free LAW entry */
	/* [한국어] 찾은 창보다 앞쪽(=우선순위가 높은) 슬롯 중 비어 있는 것을
	 * 역순으로 찾는다. LAW는 인덱스가 작을수록 우선하므로, 기존 DDR 창을
	 * 부분적으로 덮어쓰려면 반드시 그보다 앞이어야 한다. */
	while (law[--i].lawar & LAWAR_EN) {
		/* [한국어] 0번까지 왔는데도 모두 사용 중이면 끼워 넣을 자리가 없다. */
		if (i == 0) {
			/* No higher priority LAW slots available */
			ret = -ENOENT;	/* [한국어] 모든 앞쪽 슬롯이 사용 중이라 끼워 넣을 자리가 없다. */
			goto error;	/* [한국어] 정리 경로로 간다. */
		}
	}

	/* [한국어] 새 창의 시작 주소를 기록한다(상위 32비트). */
	law[i].lawbarh = upper_32_bits(phys);
	/* [한국어] 시작 주소의 하위 32비트. */
	law[i].lawbarl = lower_32_bits(phys);
	/* [한국어] 주소 쓰기가 확정된 뒤에 속성을 쓰도록 순서를 세운다.
	 * 반대 순서라면 활성화된 창이 잠시 엉뚱한 주소를 덮어 시스템이
	 * 오동작할 수 있다. */
	wmb();
	/* [한국어] 속성을 기록해 창을 활성화한다.
	 * EN으로 켜고, 앞서 찾은 타깃을 그대로 쓰고, 새로 만든 CSD ID를 붙이고,
	 * 크기를 (4KB 인코딩 + 페이지 차수)로 지정한다.
	 * 이 한 줄이 완료되는 순간부터 그 메모리 영역이 스누핑 대상이 된다. */
	law[i].lawar = LAWAR_EN | law_target | (csd_id << LAWAR_CSDID_SHIFT) |
		(LAW_SIZE_4K + get_order(size));
	/* [한국어] 속성 쓰기가 하드웨어에 도달했음을 보장한다. */
	wmb();

/* [한국어] 성공과 모든 실패가 함께 도달하는 정리 지점.
 * 성공 경로도 여기로 흘러 들어와 매핑을 해제한다 — 레지스터는 한 번
 * 설정하면 되므로 계속 매핑해 둘 이유가 없다. */
error:
	/* [한국어] CCM 매핑을 해제한다. NULL이면 매핑 전에 실패한 것이다. */
	if (ccm)
		iounmap(ccm);

	/* [한국어] LAC 매핑을 해제한다. */
	if (lac)
		iounmap(lac);

	/* [한국어] 아직 붙들고 있는 노드 참조가 있으면 내린다.
	 * 성공 경로에서는 위에서 NULL로 지웠으므로 건너뛴다. */
	if (np)
		of_node_put(np);

	/* [한국어] 성공이면 0, 실패면 그 오류 코드. */
	return ret;
}

/*
 * Table of SVRs and the corresponding PORT_ID values. Port ID corresponds to a
 * bit map of snoopers for a given range of memory mapped by a LAW.
 *
 * All future CoreNet-enabled SOCs will have this erratum(A-004510) fixed, so this
 * table should never need to be updated.  SVRs are guaranteed to be unique, so
 * there is no worry that a future SOC will inadvertently have one of these
 * values.
 */
/* [한국어] 에라타 A-004510의 영향을 받는 실리콘 목록과, 각 칩에서 쓸
 * 포트 비트맵.
 * 포트 ID는 "이 메모리 영역을 어떤 포트들이 스누핑해야 하는가"를 나타내는
 * 비트맵이며, 칩마다 코어와 캐시의 배치가 달라 값이 다르다.
 * 예: P2040/P2041/P3041은 0xFF000000(상위 8포트), P5010은 0xFC000000.
 *
 * 원본 주석의 요지: 이후에 나올 CoreNet SoC는 이 에라타가 수정되므로
 * 이 표를 갱신할 일이 없고, SVR 값은 유일하게 부여되므로 미래의 칩이
 * 우연히 이 목록에 걸릴 걱정도 없다. 즉 이 표는 영원히 이대로 남는다.
 *
 * SVR 값을 8비트 왼쪽 시프트한 뒤 리비전을 OR 하는 형태인데,
 * SVR 레지스터의 하위 8비트가 리비전이기 때문이다 — 같은 칩이라도
 * 리비전에 따라 에라타 유무가 다를 수 있어 이렇게 구분한다.
 *
 * 익명 구조체 배열을 쓰는 이유: 이 파일 안에서만, 그것도 한 곳에서만
 * 쓰이는 표라 이름을 붙일 이유가 없다. */
static const struct {
	u32 svr;
	/* [한국어] 실리콘 버전 레지스터 값(칩 종류 + 리비전).
	 * 읽는 자: fsl_pamu_probe()가 mfspr(SPRN_SVR)과 비교한다.
	 * 값 범위: SVR_xxx << 8 | 리비전. */

	u32 port_id;
	/* [한국어] 그 칩에서 쓸 스누퍼 포트 비트맵.
	 * 읽는 자: create_csd()에 csd_port_id로 전달되어 CSDID 레지스터에 기록된다.
	 * 값 범위: 칩의 포트 구성에 따라 0xFC000000 ~ 0xFFF80000 등. */
} port_id_map[] = {
	/* [한국어] P2040 리비전 1.0 — 상위 8개 포트가 스누핑한다. */
	{(SVR_P2040 << 8) | 0x10, 0xFF000000},	/* P2040 1.0 */
	/* [한국어] P2040 리비전 1.1 — 같은 포트 구성. */
	{(SVR_P2040 << 8) | 0x11, 0xFF000000},	/* P2040 1.1 */
	/* [한국어] P2041 리비전 1.0. */
	{(SVR_P2041 << 8) | 0x10, 0xFF000000},	/* P2041 1.0 */
	/* [한국어] P2041 리비전 1.1. */
	{(SVR_P2041 << 8) | 0x11, 0xFF000000},	/* P2041 1.1 */
	/* [한국어] P3041 리비전 1.0. */
	{(SVR_P3041 << 8) | 0x10, 0xFF000000},	/* P3041 1.0 */
	/* [한국어] P3041 리비전 1.1. */
	{(SVR_P3041 << 8) | 0x11, 0xFF000000},	/* P3041 1.1 */
	/* [한국어] P4040 리비전 2.0 — 포트가 더 많아 비트맵이 넓다. */
	{(SVR_P4040 << 8) | 0x20, 0xFFF80000},	/* P4040 2.0 */
	/* [한국어] P4080 리비전 2.0 — 8코어 칩이라 가장 넓은 비트맵을 쓴다. */
	{(SVR_P4080 << 8) | 0x20, 0xFFF80000},	/* P4080 2.0 */
	/* [한국어] P5010 리비전 1.0 — 코어가 적어 비트맵이 좁다. */
	{(SVR_P5010 << 8) | 0x10, 0xFC000000},	/* P5010 1.0 */
	/* [한국어] P5010 리비전 2.0. */
	{(SVR_P5010 << 8) | 0x20, 0xFC000000},	/* P5010 2.0 */
	/* [한국어] P5020 리비전 1.0. */
	{(SVR_P5020 << 8) | 0x10, 0xFC000000},	/* P5020 1.0 */
	/* [한국어] P5021 리비전 1.0. */
	{(SVR_P5021 << 8) | 0x10, 0xFF800000},	/* P5021 1.0 */
	/* [한국어] P5040 리비전 1.0 — 이 표의 마지막 항목. */
	{(SVR_P5040 << 8) | 0x10, 0xFF800000},	/* P5040 1.0 */
};

/* [한국어] SVR의 보안(E) 비트. 같은 칩이라도 보안 기능이 있는 변형은
 * 이 비트가 서 있어 SVR 값이 달라진다. 에라타 여부는 보안 기능과
 * 무관하므로, 비교 전에 이 비트를 지워야 한다. */
#define SVR_SECURITY	0x80000	/* The Security (E) bit */

/*
 * [한국어]
 * fsl_pamu_probe - PAMU 하드웨어 전체를 초기화한다
 *
 * @pdev: fsl_pamu_init()이 손수 만든 플랫폼 디바이스.
 * @return: 0 성공, 음수 errno(각 단계 실패).
 *
 * 이 함수 하나가 PAMU 초기화의 전부다. 순서가 중요한 단계들이 이어진다:
 *  1) 중복 probe 방지(전역 테이블을 쓰므로 두 번 실행되면 안 된다).
 *  2) PAMU 레지스터 영역을 매핑하고 크기를 읽는다 — 그 크기를
 *     PAMU_OFFSET으로 나누면 인스턴스 개수가 나온다.
 *  3) 접근 위반 IRQ 핸들러를 등록한다.
 *  4) GUTS 블록을 찾아 매핑한다(마지막에 bypass를 풀기 위해).
 *  5) PAACT/SPAACT/OMT를 **하나의 연속 블록**으로 할당한다. 원본 주석이
 *     밝히듯 메모리를 낭비하지만, 코히런시 도메인을 하나만 만들면 되어
 *     에라타 우회가 단순해진다.
 *  6) 이 칩이 에라타 대상이면 create_csd()로 그 블록에 코히런시
 *     서브도메인을 만든다.
 *  7) 각 PAMU 인스턴스에 테이블 위치를 알리고 켠다. 동시에 그 인스턴스의
 *     bypass 비트를 지울 준비를 한다.
 *  8) OMT를 채운다.
 *  9) GUTS에 bypass 값을 써서 PAMU들을 실제로 활성화한다. 이 한 줄이
 *     "PAMU가 동작하기 시작하는" 순간이다.
 * 10) 디바이스 트리의 모든 LIODN을 열어 부팅이 계속되게 한다.
 *
 * 자연 정렬 검사가 흥미롭다: alloc_pages는 차수(order)만큼 정렬된 블록을
 * 주지만, 명시적으로 확인한다 — 하드웨어가 테이블 정렬을 요구하기 때문이다.
 *
 * 에러 경로의 특징: 하나의 error 레이블에 모두 모이고, 각 자원을
 * 조건부로 정리한다. 다만 irq를 free_irq에 넘기는데, 그 시점에 아직
 * request_irq가 성공하지 않았을 수 있다 — irq가 0이 아니면 무조건
 * 해제를 시도하는 허점이다.
 *
 * 실행 컨텍스트: arch_initcall 시점(부팅 초기). 잠들 수 있다.
 *
 * 호출 체인:
 *   fsl_pamu_init() → platform_device_add() → driver->probe
 *   → [fsl_pamu_probe] → setup_one_pamu(), setup_omt(), setup_liodns()
 */
static int fsl_pamu_probe(struct platform_device *pdev)
{
	/* [한국어] 로깅에 쓸 device 포인터. */
	struct device *dev = &pdev->dev;
	/* [한국어] 모든 PAMU 인스턴스 레지스터가 매핑된 영역. */
	void __iomem *pamu_regs = NULL;
	/* [한국어] GUTS 블록의 매핑 주소 — pamubypenr 레지스터를 위해 필요하다. */
	struct ccsr_guts __iomem *guts_regs = NULL;
	/* [한국어] pamubypenr의 현재 값과, 각 인스턴스에 대응하는 비트 마스크. */
	u32 pamubypenr, pamu_counter;
	/* [한국어] 인스턴스 순회에 쓰는 레지스터 오프셋. */
	unsigned long pamu_reg_off;
	/* [한국어] 현재 인스턴스의 레지스터 베이스(정수형). */
	unsigned long pamu_reg_base;
	/* [한국어] IRQ 핸들러에 넘길 데이터. */
	struct pamu_isr_data *data = NULL;
	/* [한국어] GUTS 노드. */
	struct device_node *guts_node;
	/* [한국어] PAMU 레지스터 영역의 크기 — 인스턴스 개수를 여기서 구한다. */
	u64 size;
	/* [한국어] 테이블 블록으로 받은 페이지들. */
	struct page *p;
	/* [한국어] 결과 코드. */
	int ret = 0;
	/* [한국어] 접근 위반 IRQ 번호. */
	int irq;
	/* [한국어] PAACT의 물리 주소. */
	phys_addr_t ppaact_phys;
	/* [한국어] SPAACT의 물리 주소. */
	phys_addr_t spaact_phys;
	/* [한국어] OMT의 커널 가상 주소. */
	struct ome *omt;
	/* [한국어] OMT의 물리 주소. */
	phys_addr_t omt_phys;
	/* [한국어] 세 테이블을 담을 블록의 총 크기. */
	size_t mem_size = 0;
	/* [한국어] 그 크기에 해당하는 페이지 할당 차수. */
	unsigned int order = 0;
	/* [한국어] 에라타 우회에 쓸 포트 비트맵. 0이면 이 칩은 대상이 아니다. */
	u32 csd_port_id = 0;
	/* [한국어] SVR 표 순회 인덱스. */
	unsigned i;
	/*
	 * enumerate all PAMUs and allocate and setup PAMU tables
	 * for each of them,
	 * NOTE : All PAMUs share the same LIODN tables.
	 */

	/* [한국어] 전역 테이블을 쓰므로 두 번 probe되면 이미 등록된 LIODN이
	 * 모두 무효화된다. 디바이스 트리에 PAMU 노드가 하나뿐이라는 전제를
	 * 여기서 강제한다. */
	if (WARN_ON(probed))
		return -EBUSY;

	/* [한국어] PAMU 레지스터 영역 전체를 매핑한다. 여러 인스턴스가
	 * PAMU_OFFSET 간격으로 이어져 있어 한 번에 매핑한다. */
	pamu_regs = of_iomap(dev->of_node, 0);
	if (!pamu_regs) {	/* [한국어] PAMU 레지스터 영역을 매핑하지 못했다. */
		dev_err(dev, "ioremap of PAMU node failed\n");	/* [한국어] 매핑 실패를 로그로 남긴다. */
		return -ENOMEM;	/* [한국어] 레지스터 없이는 아무것도 할 수 없다. */
	}
	/* [한국어] 그 영역의 크기를 읽는다. 반환값을 검사하지 않는데,
	 * of_iomap이 성공했다면 주소 정보가 유효하다는 전제다. */
	of_get_address(dev->of_node, 0, &size, NULL);

	/* [한국어] 접근 위반 인터럽트 번호를 얻는다. */
	irq = irq_of_parse_and_map(dev->of_node, 0);
	/* [한국어] 인터럽트가 없으면 위반을 감지할 수 없다. 경고를 남기고
	 * 정리 경로로 가는데, ret이 0인 채라 성공을 반환하게 되는
	 * 미묘한 지점이다(원본 그대로 둔다). */
	if (!irq) {
		dev_warn(dev, "no interrupts listed in PAMU node\n");	/* [한국어] 인터럽트 없이는 접근 위반을 감지할 수 없음을 경고한다. */
		goto error;	/* [한국어] 정리 경로로 간다. */
	}

	/* [한국어] ISR에 넘길 데이터를 할당한다. */
	data = kzalloc_obj(*data);
	if (!data) {	/* [한국어] ISR 데이터를 할당하지 못했다. */
		ret = -ENOMEM;	/* [한국어] 메모리 부족을 기록한다. */
		goto error;	/* [한국어] 정리 경로로 간다. */
	}
	/* [한국어] ISR이 레지스터에 접근할 수 있도록 베이스를 저장한다. */
	data->pamu_reg_base = pamu_regs;
	/* [한국어] 인스턴스 개수를 계산한다. 영역 크기를 인스턴스 간격으로
	 * 나누면 나오는데, 디바이스 트리가 그렇게 기술되어 있기 때문이다. */
	data->count = size / PAMU_OFFSET;

	/* The ISR needs access to the regs, so we won't iounmap them */
	/* [한국어] 접근 위반 핸들러를 등록한다. 원본 주석대로 ISR이 레지스터를
	 * 계속 써야 하므로, 성공 경로에서는 pamu_regs를 해제하지 않는다. */
	ret = request_irq(irq, pamu_av_isr, 0, "pamu", data);
	if (ret < 0) {	/* [한국어] IRQ 등록에 실패했다. */
		dev_err(dev, "error %i installing ISR for irq %i\n", ret, irq);	/* [한국어] 어느 IRQ가 왜 실패했는지 남긴다. */
		goto error;	/* [한국어] 정리 경로로 간다. */
	}

	/* [한국어] GUTS 노드를 찾는다 — bypass 제어를 위해 필요하다. */
	guts_node = of_find_matching_node(NULL, guts_device_ids);
	if (!guts_node) {	/* [한국어] GUTS 노드를 찾지 못했다. */
		dev_err(dev, "could not find GUTS node %pOF\n", dev->of_node);	/* [한국어] bypass를 풀 수 없으므로 원인을 남긴다. */
		ret = -ENODEV;	/* [한국어] 디바이스를 찾을 수 없음으로 보고한다. */
		goto error;	/* [한국어] 정리 경로로 간다. */
	}

	/* [한국어] GUTS 레지스터를 매핑한다. */
	guts_regs = of_iomap(guts_node, 0);
	/* [한국어] 매핑 성공 여부와 무관하게 노드 참조는 곧바로 내린다. */
	of_node_put(guts_node);
	if (!guts_regs) {	/* [한국어] GUTS 레지스터 매핑에 실패했다. */
		dev_err(dev, "ioremap of GUTS node failed\n");	/* [한국어] 매핑 실패를 남긴다. */
		ret = -ENODEV;	/* [한국어] 디바이스를 쓸 수 없음으로 보고한다. */
		goto error;	/* [한국어] 정리 경로로 간다. */
	}

	/* read in the PAMU capability registers */
	/* [한국어] 첫 인스턴스의 능력 레지스터를 읽는다. 모든 인스턴스가
	 * 같은 능력을 가진다는 전제다. */
	get_pamu_cap_values((unsigned long)pamu_regs);
	/*
	 * To simplify the allocation of a coherency domain, we allocate the
	 * PAACT and the OMT in the same memory buffer.  Unfortunately, this
	 * wastes more memory compared to allocating the buffers separately.
	 */
	/* Determine how much memory we need */
	/* [한국어] 세 테이블을 하나의 블록에 담기 위한 총 크기를 계산한다.
	 * 각 테이블을 페이지 단위로 올림해 더하므로 실제 필요량보다 크다 —
	 * 원본 주석이 인정하는 낭비다. 그 대가로 코히런시 서브도메인을
	 * 하나만 만들면 되어 create_csd()가 단순해진다. */
	mem_size = (PAGE_SIZE << get_order(PAACT_SIZE)) +
		(PAGE_SIZE << get_order(SPAACT_SIZE)) +
		(PAGE_SIZE << get_order(OMT_SIZE));
	/* [한국어] 그 크기를 한 번에 받기 위한 페이지 할당 차수. */
	order = get_order(mem_size);

	/* [한국어] 연속 페이지 블록을 0 초기화해 받는다. 0으로 시작해야
	 * 모든 PAACE가 무효 상태가 된다. */
	p = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!p) {	/* [한국어] 연속 페이지 블록을 확보하지 못했다. */
		dev_err(dev, "unable to allocate PAACT/SPAACT/OMT block\n");	/* [한국어] 어느 테이블 블록이 실패했는지 남긴다. */
		ret = -ENOMEM;	/* [한국어] 메모리 부족으로 보고한다. */
		goto error;	/* [한국어] 정리 경로로 간다. */
	}

	/* [한국어] 블록의 시작이 곧 PAACT다. 전역 변수에 저장해
	 * pamu_get_ppaace()가 쓸 수 있게 한다. */
	ppaact = page_address(p);
	/* [한국어] 그 물리 주소 — 레지스터에 등록할 값이다. */
	ppaact_phys = page_to_phys(p);

	/* Make sure the memory is naturally aligned */
	/* [한국어] 하드웨어가 테이블 정렬을 요구하므로 확인한다.
	 * alloc_pages가 차수만큼 정렬된 블록을 주므로 통과하는 것이 정상이지만,
	 * 조용히 잘못 동작하느니 명시적으로 검사한다. */
	if (ppaact_phys & ((PAGE_SIZE << order) - 1)) {
		dev_err(dev, "PAACT/OMT block is unaligned\n");	/* [한국어] 하드웨어가 요구하는 정렬을 만족하지 못했다. */
		ret = -ENOMEM;	/* [한국어] 정렬되지 않은 블록은 쓸 수 없다. */
		goto error;	/* [한국어] 정리 경로로 간다. */
	}

	/* [한국어] 블록 안에서 SPAACT의 위치를 계산한다 — PAACT 다음이다. */
	spaact = (void *)ppaact + (PAGE_SIZE << get_order(PAACT_SIZE));
	/* [한국어] OMT는 그 다음이다. 세 테이블이 하나의 연속 블록에 나란히 놓인다. */
	omt = (void *)spaact + (PAGE_SIZE << get_order(SPAACT_SIZE));

	/* [한국어] 디버깅용으로 PAACT의 가상/물리 주소를 남긴다. */
	dev_dbg(dev, "ppaact virt=%p phys=%pa\n", ppaact, &ppaact_phys);

	/* Check to see if we need to implement the work-around on this SOC */

	/* Determine the Port ID for our coherence subdomain */
	/* [한국어] 이 칩이 에라타 A-004510의 영향을 받는지 표에서 찾는다.
	 * SVR에서 보안 비트를 지우고 비교하는데, 보안 변형이라고 에라타
	 * 유무가 달라지지는 않기 때문이다. */
	for (i = 0; i < ARRAY_SIZE(port_id_map); i++) {
		if (port_id_map[i].svr == (mfspr(SPRN_SVR) & ~SVR_SECURITY)) {	/* [한국어] 보안 비트를 지운 SVR이 에라타 대상 목록에 있는지 본다. */
			csd_port_id = port_id_map[i].port_id;	/* [한국어] 그 칩에서 쓸 스누퍼 포트 비트맵을 기억한다. */
			dev_dbg(dev, "found matching SVR %08x\n",	/* [한국어] 어느 실리콘으로 판별됐는지 디버그 로그로 남긴다. */
				port_id_map[i].svr);
			break;
		}
	}

	/* [한국어] 표에 있는 칩이라면 우회책을 적용한다. 아니면 이 블록을
	 * 통째로 건너뛴다 — 수정된 실리콘에서는 필요 없기 때문이다. */
	if (csd_port_id) {
		dev_dbg(dev, "creating coherency subdomain at address %pa, size %zu, port id 0x%08x",	/* [한국어] 어느 영역에 어떤 포트 비트맵으로 서브도메인을 만드는지 남긴다. */
			&ppaact_phys, mem_size, csd_port_id);

		/* [한국어] 테이블 블록 전체를 덮는 코히런시 서브도메인을 만든다.
		 * 실패하면 테이블 일관성을 보장할 수 없으므로 probe를 포기한다. */
		ret = create_csd(ppaact_phys, mem_size, csd_port_id);
		if (ret) {	/* [한국어] 서브도메인 생성이 실패했다. */
			dev_err(dev, "could not create coherence subdomain\n");	/* [한국어] 테이블 일관성을 보장할 수 없음을 남긴다. */
			goto error;	/* [한국어] 정리 경로로 간다. */
		}
	}

	/* [한국어] SPAACT의 물리 주소를 구한다. 블록 안의 오프셋이므로
	 * virt_to_phys로 계산할 수 있다. */
	spaact_phys = virt_to_phys(spaact);
	/* [한국어] OMT의 물리 주소도 마찬가지로 구한다. */
	omt_phys = virt_to_phys(omt);

	/* [한국어] 현재 bypass 설정을 읽는다. 각 비트가 한 PAMU 인스턴스의
	 * 우회 여부를 나타내며, 부팅 시에는 모두 켜져(우회) 있다. */
	pamubypenr = in_be32(&guts_regs->pamubypenr);

	/* [한국어] 모든 PAMU 인스턴스를 순회하며 설정한다.
	 * pamu_counter가 최상위 비트에서 시작해 오른쪽으로 이동하는데,
	 * bypass 레지스터의 비트 순서가 인스턴스 순서와 반대이기 때문이다. */
	for (pamu_reg_off = 0, pamu_counter = 0x80000000; pamu_reg_off < size;
	     pamu_reg_off += PAMU_OFFSET, pamu_counter >>= 1) {

		/* [한국어] 이 인스턴스의 레지스터 베이스를 계산한다. */
		pamu_reg_base = (unsigned long)pamu_regs + pamu_reg_off;
		/* [한국어] 테이블 위치를 알리고 이 인스턴스를 켠다.
		 * 두 번째 인자로 크기가 아니라 오프셋을 넘기는데, 그 인자가
		 * 쓰이지 않아 드러나지 않는 불일치다. */
		setup_one_pamu(pamu_reg_base, pamu_reg_off, ppaact_phys,
			       spaact_phys, omt_phys);
		/* Disable PAMU bypass for this PAMU */
		/* [한국어] 이 인스턴스의 bypass 비트를 지울 준비를 한다.
		 * 실제 기록은 모든 인스턴스 설정이 끝난 뒤 한 번에 한다 —
		 * 중간에 켜면 아직 설정되지 않은 인스턴스가 DMA를 차단할 수 있다. */
		pamubypenr &= ~pamu_counter;
	}

	/* [한국어] 오퍼레이션 매핑 테이블을 채운다. LIODN을 활성화하기 전에
	 * 해야 하는데, PAACE의 omi가 가리키는 곳이 채워져 있어야 하기 때문이다. */
	setup_omt(omt);

	/* Enable all relevant PAMU(s) */
	/* [한국어] bypass를 해제해 모든 PAMU를 실제로 동작시킨다.
	 * 이 한 줄이 "PAMU가 DMA를 검사하기 시작하는" 순간이다. */
	out_be32(&guts_regs->pamubypenr, pamubypenr);

	/* [한국어] GUTS 레지스터는 더 필요 없으므로 매핑을 해제한다.
	 * PAMU 레지스터와 달리 ISR이 쓰지 않기 때문이다. */
	iounmap(guts_regs);

	/* Enable DMA for the LIODNs in the device tree */

	/* [한국어] 디바이스 트리의 모든 LIODN을 기본 구성으로 열어 부팅이
	 * 계속되게 한다. bypass를 이미 풀었으므로, 이 호출 전에는 모든 DMA가
	 * 차단된 상태다 — 그래서 이 단계가 늦으면 안 된다. */
	setup_liodns();

	/* [한국어] 초기화 완료를 기록해 두 번째 probe를 막는다. */
	probed = true;

	/* [한국어] 성공. */
	return 0;

/* [한국어] 모든 실패가 모이는 정리 지점. 확보한 자원을 역순에 가깝게 되돌린다. */
error:
	/* [한국어] IRQ를 해제한다. 다만 request_irq가 성공하기 전에 실패한
	 * 경우에도 irq가 0이 아니면 해제를 시도하는 허점이 있다. */
	if (irq)
		free_irq(irq, data);

	/* [한국어] ISR 데이터를 반납한다. kfree_sensitive는 해제 전에 내용을
	 * 0으로 지우는데, 여기서는 민감한 정보가 없어 과한 선택이다. */
	kfree_sensitive(data);

	/* [한국어] PAMU 레지스터 매핑을 해제한다. 성공 경로와 달리 여기서는
	 * ISR이 등록되지 않았거나 해제되었으므로 안전하다. */
	if (pamu_regs)
		iounmap(pamu_regs);

	/* [한국어] GUTS 매핑도 해제한다. */
	if (guts_regs)
		iounmap(guts_regs);

	/* [한국어] 테이블 블록을 반납한다. order가 할당 때와 같아야 한다. */
	if (ppaact)
		free_pages((unsigned long)ppaact, order);

	/* [한국어] 전역 포인터를 지워 pamu_get_ppaace()가 해제된 메모리를
	 * 가리키지 않게 한다. */
	ppaact = NULL;

	/* [한국어] 실패 원인을 반환한다. */
	return ret;
}

/* [한국어] PAMU의 플랫폼 드라이버 정의.
 * of_match_table이 없는 점에 주목 — 이 드라이버는 디바이스 트리 매칭으로
 * 바인딩되지 않고, fsl_pamu_init()이 이름이 일치하는 플랫폼 디바이스를
 * 손수 만들어 붙인다. 그렇게 하는 이유는 아래 init 함수의 주석에 있다. */
static struct platform_driver fsl_of_pamu_driver = {
	/* [한국어] 드라이버 공통 정보. */
	.driver = {
		/* [한국어] 이 이름이 곧 매칭 기준이다 — init이 같은 이름으로
		 * 플랫폼 디바이스를 만든다. */
		.name = "fsl-of-pamu",
	},
	/* [한국어] 초기화 진입점. */
	.probe = fsl_pamu_probe,
};

/*
 * [한국어]
 * fsl_pamu_init - PAMU 드라이버를 등록하고 probe를 강제로 앞당긴다
 *
 * @return: 0 성공, 음수 errno(각 단계 실패).
 *
 * 왜 이런 우회를 하는가: 원본 주석이 그 이유를 명확히 밝힌다.
 * 보통의 OF probe는 대부분의 드라이버가 올라온 뒤 불확실한 시점에
 * 실행된다. 그런데 QMan 같은 PAMU 클라이언트는 PAMU가 이미 초기화되어
 * 있다고 가정하고 동작하므로, 그 시점으로는 너무 늦다.
 * 그래서 arch_initcall(아주 이른 단계)에서 플랫폼 디바이스를 직접 만들어
 * probe를 즉시 유발한다.
 *
 * PAMU 노드가 하나뿐이라는 전제도 원본 주석이 설명한다: 하나의 PAMU
 * 노드가 SoC의 모든 PAMU 인스턴스를 대표하며, 바인딩상 부모-자식 관계도
 * 허용하지 않는다. 여러 노드를 지원하려면 많은 코드를 고쳐야 한다.
 *
 * 순서에 주목: pamu_domain_init()(IOMMU 코어 등록)을
 * platform_device_add()(= probe 유발)보다 **먼저** 호출한다.
 * probe가 끝나면 곧바로 LIODN이 열리고 클라이언트가 붙을 수 있으므로,
 * 그전에 IOMMU 코어 등록이 끝나 있어야 하기 때문이다.
 *
 * 실행 컨텍스트: arch_initcall(부팅 초기). 잠들 수 있다.
 *
 * 호출 체인:
 *   arch_initcall → [fsl_pamu_init] → platform_driver_register(),
 *   pamu_domain_init(), platform_device_add() → fsl_pamu_probe()
 */
static __init int fsl_pamu_init(void)
{
	/* [한국어] 손수 만들 플랫폼 디바이스. */
	struct platform_device *pdev = NULL;
	/* [한국어] PAMU 디바이스 트리 노드. */
	struct device_node *np;
	/* [한국어] 각 단계의 결과. */
	int ret;

	/*
	 * The normal OF process calls the probe function at some
	 * indeterminate later time, after most drivers have loaded.  This is
	 * too late for us, because PAMU clients (like the Qman driver)
	 * depend on PAMU being initialized early.
	 *
	 * So instead, we "manually" call our probe function by creating the
	 * platform devices ourselves.
	 */

	/*
	 * We assume that there is only one PAMU node in the device tree.  A
	 * single PAMU node represents all of the PAMU devices in the SOC
	 * already.   Everything else already makes that assumption, and the
	 * binding for the PAMU nodes doesn't allow for any parent-child
	 * relationships anyway.  In other words, support for more than one
	 * PAMU node would require significant changes to a lot of code.
	 */

	/* [한국어] 디바이스 트리에서 PAMU 노드를 찾는다. 위 주석대로
	 * 하나뿐이라고 가정하므로 첫 번째만 쓴다. */
	np = of_find_compatible_node(NULL, NULL, "fsl,pamu");
	/* [한국어] 이 SoC에 PAMU가 없다면 조용히 물러난다. */
	if (!np) {
		pr_err("could not find a PAMU node\n");	/* [한국어] 이 SoC에는 PAMU가 없다는 뜻이다. */
		return -ENODEV;	/* [한국어] 초기화할 대상이 없으므로 물러난다. */
	}

	/* [한국어] 드라이버를 먼저 등록한다. 디바이스를 추가하면 곧바로
	 * 매칭이 일어나야 하므로 순서가 이렇다. */
	ret = platform_driver_register(&fsl_of_pamu_driver);
	if (ret) {	/* [한국어] 드라이버 등록에 실패했다. */
		pr_err("could not register driver (err=%i)\n", ret);	/* [한국어] 실패 원인을 남긴다. */
		goto error_driver_register;	/* [한국어] 노드 참조를 내리러 간다. */
	}

	/* [한국어] 드라이버 이름과 같은 플랫폼 디바이스를 만든다.
	 * 아직 버스에 추가하지 않았으므로 probe는 일어나지 않는다. */
	pdev = platform_device_alloc("fsl-of-pamu", 0);
	if (!pdev) {	/* [한국어] 플랫폼 디바이스를 할당하지 못했다. */
		pr_err("could not allocate device %pOF\n", np);	/* [한국어] 어느 노드에 대한 디바이스였는지 남긴다. */
		ret = -ENOMEM;	/* [한국어] 메모리 부족으로 보고한다. */
		goto error_device_alloc;	/* [한국어] 드라이버 등록을 되돌리러 간다. */
	}
	/* [한국어] 디바이스에 PAMU 노드를 연결한다. probe가 of_iomap 등으로
	 * 이 노드를 읽게 된다. 참조를 하나 올려 디바이스가 살아 있는 동안
	 * 노드가 사라지지 않게 한다. */
	pdev->dev.of_node = of_node_get(np);

	/* [한국어] IOMMU 코어 등록(fsl_pamu_domain.c)을 probe보다 먼저 한다.
	 * probe가 끝나면 LIODN이 열리고 클라이언트가 붙을 수 있으므로,
	 * 그전에 도메인 계층이 준비되어 있어야 한다. */
	ret = pamu_domain_init();
	if (ret)	/* [한국어] 도메인 계층 초기화 실패 — probe를 유발하기 전에 멈춘다. */
		goto error_device_add;

	/* [한국어] 디바이스를 버스에 추가한다. 이름이 일치하는 드라이버가
	 * 이미 등록되어 있으므로, 이 호출 안에서 곧바로 probe가 실행된다 —
	 * 이것이 "probe를 앞당기는" 방법의 핵심이다. */
	ret = platform_device_add(pdev);
	if (ret) {	/* [한국어] 디바이스 추가(=probe)가 실패했다. */
		pr_err("could not add device %pOF (err=%i)\n", np, ret);	/* [한국어] 어느 노드에서 왜 실패했는지 남긴다. */
		goto error_device_add;	/* [한국어] 디바이스와 드라이버를 역순으로 되돌리러 간다. */
	}

	/* [한국어] 초기화 완료. 이 시점에는 PAMU가 이미 동작 중이고
	 * 모든 LIODN이 열려 있다. */
	return 0;

/* [한국어] 디바이스 추가나 도메인 초기화가 실패했을 때의 되감기. */
error_device_add:
	/* [한국어] 디바이스에 붙여 둔 노드 참조를 내린다. */
	of_node_put(pdev->dev.of_node);
	/* [한국어] 포인터도 지워 아래 platform_device_put이 다시 해제하지
	 * 않게 한다. */
	pdev->dev.of_node = NULL;

	/* [한국어] 아직 버스에 추가되지 않은 디바이스를 반납한다. */
	platform_device_put(pdev);

/* [한국어] 디바이스 할당이 실패했을 때의 되감기. */
error_device_alloc:
	/* [한국어] 등록해 둔 드라이버를 해제한다. */
	platform_driver_unregister(&fsl_of_pamu_driver);

/* [한국어] 드라이버 등록이 실패했을 때의 되감기. */
error_driver_register:
	/* [한국어] of_find_compatible_node가 올린 노드 참조를 내린다. */
	of_node_put(np);

	/* [한국어] 실패 원인을 반환한다. initcall의 반환값은 부팅 로그에만
	 * 남고 부팅을 멈추지는 않는다. */
	return ret;
}
/* [한국어] arch_initcall로 등록해 아주 이른 단계에서 실행되게 한다.
 * subsys_initcall보다도 앞서므로, QMan 같은 클라이언트가 올라오기 전에
 * PAMU가 준비된다 — 위 함수 주석이 설명하는 순서 문제의 해법이다. */
arch_initcall(fsl_pamu_init);
