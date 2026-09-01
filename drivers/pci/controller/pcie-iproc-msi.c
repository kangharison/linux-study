// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Broadcom Corporation
 */

/* [한국어] irq_desc / irq_data / IRQ 처리 관련 기본 정의. 이 파일은 인터럽트 컨트롤러
 * 드라이버이므로 이 헤더가 가장 근본이 된다. */
/*
 * [한국어 설명] iProc PCIe 이벤트 큐 기반 MSI 컨트롤러 (pcie-iproc-msi.c)
 *
 * === 파일의 역할 ===
 * Broadcom iProc 계열 SoC 에서 PCIe MSI(Message Signaled Interrupt)를 받아
 * 리눅스 IRQ 로 바꿔 주는 인터럽트 컨트롤러 드라이버다. 존재 이유는 구조체
 * 주석이 밝히듯 "GIC 에 MSI 지원이 통합되지 않은 플랫폼" 이기 때문이다.
 * ARM 의 GIC-500 이상은 ITS 로 MSI 를 직접 처리하지만, 이 SoC 들의 GIC 에는
 * 그 기능이 없어 PCIe 컨트롤러 쪽에 별도의 MSI 수집 장치가 붙어 있다.
 * 그 장치의 동작 원리는 이렇다. 장치가 MSI 로 쓰는 값이 메모리의 링 버퍼
 * (이벤트 큐)에 u32 하나로 쌓이고, 큐가 비어 있지 않으면 하드웨어가 GIC
 * 인터럽트 하나를 올린다. 이 파일은 그 인터럽트를 체인 핸들러로 받아 큐를
 * 비우면서 각 MSI 를 해당 장치 드라이버의 핸들러로 나눠 준다.
 * 하드웨어가 제공하는 것은 최대 6개의 그룹이고 그룹마다 큐 하나·GIC IRQ 하나이며
 * 큐 하나가 64개 항목을 담는다. 여기에 더해 이 드라이버는 CPU 어피니티를
 * 소프트웨어로 구현한다 — 벡터를 CPU 수만큼 복제해 두고 어느 사본을 쓰느냐로
 * 목적지 CPU 를 고르는 방식이며, 그 대가로 실제 사용 가능한 벡터 수가
 * (그룹수 × 64) / CPU수 로 줄어든다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 인터럽트 계층으로 보면 위에서부터 장치 드라이버 → PCI MSI 코어 →
 * 이 파일의 IRQ 도메인 → GIC → CPU 다. PCI 계층으로 보면 iProc PCIe 공용 코어
 * (pcie-iproc.c)의 선택적 부속으로, CONFIG_PCIE_IPROC_MSI 가 꺼지면 헤더의
 * 인라인 더미가 -ENODEV 를 돌려주고 이 파일은 빌드되지 않는다.
 * 진입 경로는 셋이다. (1) probe 때 iproc_pcie_setup() 이 iproc_pcie_msi_enable()
 * 을 거쳐 iproc_msi_init() 을 부른다(pcie-iproc.c:1367). 실패해도 치명적이지
 * 않다 — 다른 MSI 컨트롤러를 쓰는 시스템일 수 있다는 상류 주석이 그 자리에 있다.
 * (2) 장치가 MSI 를 요청하면 IRQ 도메인의 alloc/free 콜백이 불려 비트맵에서
 * 벡터를 예약하고 MSI 메시지를 조립한다. (3) 인터럽트가 오면 체인 핸들러가
 * 큐를 비운다. 제거는 iproc_pcie_remove() → iproc_msi_exit() 이다.
 * 실행 컨텍스트는 두 갈래로 뚜렷이 나뉜다. init/exit 와 도메인 alloc/free 는
 * 프로세스 컨텍스트로 뮤텍스를 잡고 잠들 수 있는 반면, 체인 핸들러는 인터럽트
 * 컨텍스트라 어떤 락도 잡지 않는다. 그 둘이 공유하는 자료는 큐 메모리와
 * 레지스터뿐이며, 그룹이 CPU 에 고정되어 있어 같은 큐를 두 CPU 가 동시에
 * 처리하는 일이 없다는 것이 락 없는 설계의 근거다.
 *
 * === 타 모듈과의 연결 ===
 * 옆쪽: pcie-iproc.h 의 struct iproc_pcie 에서 base(레지스터 창), base_addr
 * (MSI 목적지 기준 물리 주소), type(SoC 변종), dev(로그·devm 기준)를 가져오고,
 * 완성된 struct iproc_msi 를 pcie->msi 에 심어 둔다. 반대로 pcie-iproc.c 는
 * iproc_msi_init()/iproc_msi_exit() 두 심볼만 쓴다 — EXPORT_SYMBOL 로 공개되어
 * 있어 두 파일이 별도 모듈로 빌드되어도 링크된다.
 * 아래쪽: IRQ 서브시스템(irq_domain, irq_chip, chained_irq_enter/exit,
 * generic_handle_domain_irq, irq_set_affinity), MSI 코어(msi_parent_ops,
 * msi_create_parent_irq_domain, msi_lib_init_dev_msi_info), DMA API
 * (dma_alloc_coherent), DT(of_irq_count, irq_of_parse_and_map), 비트맵.
 * 데이터 흐름은 양방향이다. 나가는 쪽: hwirq → 그룹 번호 → 주소 오프셋 →
 * struct msi_msg → 장치의 MSI capability 레지스터. 들어오는 쪽: 장치의 MSI 쓰기 →
 * 이벤트 큐 메모리 → 체인 핸들러가 읽어 hwirq 복원 → 대표 번호로 변환 →
 * IRQ 도메인 → 장치 드라이버 핸들러.
 * 공유 상태: MSI 벡터 비트맵(뮤텍스로 보호), 이벤트 큐 메모리(하드웨어가 쓰고
 * 소프트웨어가 읽으며 coherent DMA 로 잡는다), 그리고 정적 전역
 * iproc_msi_parent_ops — 마지막 것은 인스턴스별이 아니라는 점에 주의해야 한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct iproc_msi: 컨트롤러 하나. 그룹 배열, 벡터 비트맵과 그 뮤텍스,
 *   레지스터 오프셋 테이블, 큐 메모리(가상·DMA 주소), IRQ 도메인을 담는다.
 * - struct iproc_msi_grp: 그룹 하나 = GIC IRQ 하나 + 이벤트 큐 하나.
 *   체인 핸들러가 irq_desc 의 handler_data 로 이 구조체를 받아 문맥을 복원한다.
 * - iproc_msi_init(): 진입점. DT 확인 → 그룹 수 결정(CPU 수의 배수로 조정) →
 *   변종별 레지스터 테이블 선택 → 비트맵·그룹·큐 메모리 할당 → IRQ 도메인 생성
 *   → CPU 별 체인 핸들러 등록 → 하드웨어 활성화. 세 개의 되감기 라벨을 갖는다.
 * - iproc_msi_handler(): 체인 핸들러. head/tail 링 버퍼를 비우고, 처리 중 새로
 *   들어온 이벤트까지 소진할 때까지 tail 을 다시 읽는다. 탈출 조건은 "처리할
 *   이벤트가 0개" 하나뿐이다.
 * - iproc_msi_irq_domain_alloc()/free(): 비트맵에서 nr_cpus × nr_irqs 개를
 *   2의 거듭제곱 단위로 예약·반납한다. 해제는 반드시 대표 hwirq 로 해야 짝이 맞는다.
 * - iproc_msi_irq_set_affinity(): hwirq 를 다른 CPU 사본으로 바꾸는 것만으로
 *   목적지를 옮긴다. 하드웨어에 어피니티 레지스터가 없어 나온 우회 설계다.
 * - iproc_msi_irq_compose_msi_msg() / decode_msi_hwirq(): MSI 데이터의 인코딩과
 *   디코딩 짝. hwirq << 5 로 보내고 (data >> 5) + (data & 0x1f) 로 되돌린다.
 *   하위 5비트는 다중 MSI 에서 장치가 더하는 벡터 인덱스 자리다.
 * - iproc_msi_reg_paxb[] / iproc_msi_reg_paxc[]: 변종별 [그룹][레지스터] 오프셋
 *   테이블. PAXB 는 큐 메모리와 MSI 주소를 그룹들이 공유하고, PAXC 는 그룹마다
 *   따로 갖는다 — 그 차이가 nr_eq_region / nr_msi_region 값으로 나타난다.
 * - [상류 코드 관찰, 수정하지 않음] init 의 이른 실패 경로들이 pcie->msi 를
 *   NULL 로 되돌리지 않아, 이후 iproc_msi_exit() 이 반쯤 초기화된 구조체를
 *   iproc_msi_disable() 에 넘겨 NULL 인 reg_offsets 를 역참조할 수 있다.
 *   또 PAXC 테이블은 행이 4개, 각 행의 초기화 값이 7개뿐이라 나머지가 0 으로
 *   채워지며, INTS_EN 열이 0 이 된다.
 */

#include <linux/interrupt.h>
/* [한국어] chained_irq_enter()/chained_irq_exit() — 상위 인터럽트 컨트롤러(GIC)의 IRQ
 * 하나를 이 드라이버가 "체인 핸들러"로 가로채 여러 하위 인터럽트로 분배할 때 쓰는 짝.
 * 체인 방식은 request_irq() 로 등록하는 일반 핸들러와 달리 상위 컨트롤러의 흐름 제어
 * (eoi/mask)를 직접 다루므로 이 두 함수로 진입·이탈을 감싸야 한다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] msi_lib_init_dev_msi_info() 등 MSI 상위 도메인 공통 구현. 최근 커널이
 * 컨트롤러마다 반복되던 MSI 도메인 설정 코드를 이 라이브러리로 모았다. */
#include <linux/irqchip/irq-msi-lib.h>
/* [한국어] irq_domain / irq_domain_ops / irq_domain_set_info() 등 IRQ 도메인 API.
 * MSI 벡터 번호(hwirq)를 리눅스 virq 로 사상하는 뼈대다. */
#include <linux/irqdomain.h>
/* [한국어] struct msi_msg, msi_parent_ops, MSI_FLAG_* 플래그. MSI 메시지(주소·데이터)를
 * 조립하는 compose 콜백이 이 타입을 쓴다. */
#include <linux/msi.h>
/* [한국어] of_irq_count() / irq_of_parse_and_map() — DT 의 interrupts 속성에서 GIC IRQ 를
 * 몇 개나 받았는지 세고 실제 virq 로 매핑한다. */
#include <linux/of_irq.h>
/* [한국어] of_pci 계열 헬퍼. 이 파일이 직접 쓰는 심볼은 없지만 MSI 노드 처리 경로가 의존한다. */
#include <linux/of_pci.h>
/* [한국어] PCI 코어 공개 API. DOMAIN_BUS_PCI_MSI 토큰 등 PCI MSI 관련 정의를 끌어온다. */
#include <linux/pci.h>

/* [한국어] iProc PCIe 공용 헤더. struct iproc_pcie(base/base_addr/type/msi 필드),
 * enum iproc_pcie_type, 그리고 이 파일이 구현하는 iproc_msi_init()/iproc_msi_exit()
 * 선언(:118~119)이 여기에 있다. CONFIG_PCIE_IPROC_MSI 가 꺼지면 그 자리에
 * -ENODEV 를 돌려주는 인라인 더미가 들어간다(:121~128). */
#include "pcie-iproc.h"

/* [한국어] MSI_CTRL 레지스터에서 인터럽트 허용 비트의 위치(11). */
#define IPROC_MSI_INTR_EN_SHIFT        11
/* [한국어] 그 비트 자체. CTRL 에 이 비트를 세워야 이벤트 큐가 GIC 인터럽트를 올린다. */
#define IPROC_MSI_INTR_EN              BIT(IPROC_MSI_INTR_EN_SHIFT)
/* [한국어] "이벤트가 N개 쌓이면 인터럽트" 동작을 고르는 비트의 위치(1). */
#define IPROC_MSI_INT_N_EVENT_SHIFT    1
/* [한국어] 그 비트. 아래 enable 경로에서 항상 함께 세운다. */
#define IPROC_MSI_INT_N_EVENT          BIT(IPROC_MSI_INT_N_EVENT_SHIFT)
/* [한국어] 이벤트 큐 자체를 활성화하는 비트의 위치(0). */
#define IPROC_MSI_EQ_EN_SHIFT          0
/* [한국어] 그 비트. 세 비트를 OR 로 묶어 한 번에 CTRL 에 쓴다. */
#define IPROC_MSI_EQ_EN                BIT(IPROC_MSI_EQ_EN_SHIFT)

/* [한국어] EQ_HEAD/EQ_TAIL 레지스터에서 실제 인덱스가 차지하는 하위 6비트 마스크.
 * 큐 길이가 64(EQ_LEN)이므로 0~63 을 표현하는 데 6비트면 충분하며,
 * 상위 비트에 다른 정보가 실려 있을 수 있어 읽을 때마다 마스크를 씌운다. */
#define IPROC_MSI_EQ_MASK              0x3f

/* Max number of GIC interrupts */
/* [한국어] 이 하드웨어가 지원하는 최대 GIC 인터럽트(= MSI 그룹) 개수.
 * 아래 레지스터 오프셋 테이블의 행 수와 같아야 한다. */
#define NR_HW_IRQS                     6

/* Number of entries in each event queue */
/* [한국어] 이벤트 큐 하나에 들어가는 항목 수. 각 항목은 u32 하나(MSI 데이터)다.
 * 그룹 하나가 64개 MSI 벡터를 감당한다는 뜻이며, 총 벡터 수 계산의 기준이 된다. */
#define EQ_LEN                         64

/* Size of each event queue memory region */
/* [한국어] PAXC 처럼 큐마다 별도 메모리 영역이 필요한 경우의 영역 크기(4KB).
 * 64 × 4바이트 = 256바이트만 실제로 쓰지만, 하드웨어가 4KB 정렬을 요구한다. */
#define EQ_MEM_REGION_SIZE             SZ_4K

/* Size of each MSI address region */
/* [한국어] MSI 포스티드 라이트가 향할 주소 영역의 크기(4KB). 마찬가지로 정렬 요구 때문이다. */
#define MSI_MEM_REGION_SIZE            SZ_4K

enum iproc_msi_reg {
	/* [한국어] 이벤트 큐 메모리의 물리 주소 하위 32비트를 담는 레지스터. */
	IPROC_MSI_EQ_PAGE = 0,
	/* [한국어] 그 상위 32비트. 64비트 DMA 주소를 두 레지스터로 나눠 싣는다. */
	IPROC_MSI_EQ_PAGE_UPPER,
	/* [한국어] MSI 포스티드 라이트가 향할 주소의 하위 32비트. */
	IPROC_MSI_PAGE,
	/* [한국어] 그 상위 32비트. */
	IPROC_MSI_PAGE_UPPER,
	/* [한국어] 이벤트 큐 제어 레지스터. 위 세 비트(INTR_EN/INT_N_EVENT/EQ_EN)를 담는다. */
	IPROC_MSI_CTRL,
	/* [한국어] 소프트웨어가 다음에 읽을 항목의 인덱스. 소프트웨어가 갱신한다. */
	IPROC_MSI_EQ_HEAD,
	/* [한국어] 하드웨어가 다음에 쓸 항목의 인덱스. 하드웨어가 갱신한다.
	 * HEAD 와 TAIL 사이가 아직 처리하지 않은 MSI 데이터다. */
	IPROC_MSI_EQ_TAIL,
	/* [한국어] 일부 구형 플랫폼에만 있는 별도 인터럽트 허용 레지스터.
	 * DT 의 brcm,pcie-msi-inten 속성이 있을 때만 쓴다. */
	IPROC_MSI_INTS_EN,
	/* [한국어] enum 의 끝 — 실제 레지스터가 아니라 배열 크기(8)로 쓰이는 보초 값이다. */
	IPROC_MSI_REG_SIZE,
};

/* [한국어] 전방 선언. 바로 아래 struct iproc_msi_grp 이 struct iproc_msi 포인터를 필드로 갖는데
 * 정작 struct iproc_msi 의 정의는 그 뒤에 오기 때문에, 컴파일러에게 "그런 타입이
 * 있긴 하다"고 먼저 알려 주는 것이다. 두 구조체가 서로를 가리키는 순환 참조라
 * 어느 한쪽은 반드시 전방 선언이 필요하다. */
struct iproc_msi;

/**
 * struct iproc_msi_grp - iProc MSI group
 *
 * One MSI group is allocated per GIC interrupt, serviced by one iProc MSI
 * event queue.
 *
 * @msi: pointer to iProc MSI data
 * @gic_irq: GIC interrupt
 * @eq: Event queue number
 */
struct iproc_msi_grp {
	/* [한국어] 이 그룹이 속한 컨트롤러로 거슬러 올라가는 역포인터.
	 * 설정자: iproc_msi_init() 의 그룹 초기화 루프.
	 * 읽는 자: iproc_msi_handler() 가 irq_desc 의 handler_data 로 이 구조체를 받은 뒤
	 *   여기서 msi 를 꺼내 레지스터에 접근한다.
	 * 값 범위: 항상 유효(NULL 불가).
	 * 동기화: 설정 후 읽기 전용. */
	struct iproc_msi *msi;
	/* [한국어] 이 그룹에 배정된 GIC 인터럽트의 리눅스 virq 번호.
	 * 설정자: iproc_msi_init() 이 irq_of_parse_and_map(node, i) 결과로 채운다.
	 * 읽는 자: irq_set_chained_handler_and_data()/irq_set_affinity()/
	 *   irq_dispose_mapping() 이 이 번호를 쓴다.
	 * 값 범위: 0 이 아닌 유효 virq. 0 은 매핑 실패를 뜻해 초기화가 중단된다.
	 * 동기화: 설정 후 읽기 전용. */
	int gic_irq;
	/* [한국어] 이 그룹이 쓰는 이벤트 큐 번호. 그룹 i 가 큐 i 를 쓰므로 인덱스와 같다.
	 * 설정자: iproc_msi_init() 의 그룹 초기화 루프(msi->grps[i].eq = i).
	 * 읽는 자: iproc_msi_handler() 가 어느 큐의 HEAD/TAIL 을 읽을지 정할 때.
	 * 값 범위: 0 이상 nr_irqs 미만.
	 * 동기화: 설정 후 읽기 전용. */
	unsigned int eq;
};

/**
 * struct iproc_msi - iProc event queue based MSI
 *
 * Only meant to be used on platforms without MSI support integrated into the
 * GIC.
 *
 * @pcie: pointer to iProc PCIe data
 * @reg_offsets: MSI register offsets
 * @grps: MSI groups
 * @nr_irqs: number of total interrupts connected to GIC
 * @nr_cpus: number of toal CPUs
 * @has_inten_reg: indicates the MSI interrupt enable register needs to be
 * set explicitly (required for some legacy platforms)
 * @bitmap: MSI vector bitmap
 * @bitmap_lock: lock to protect access to the MSI bitmap
 * @nr_msi_vecs: total number of MSI vectors
 * @inner_domain: inner IRQ domain
 * @nr_eq_region: required number of 4K aligned memory region for MSI event
 * queues
 * @nr_msi_region: required number of 4K aligned address region for MSI posted
 * writes
 * @eq_cpu: pointer to allocated memory region for MSI event queues
 * @eq_dma: DMA address of MSI event queues
 * @msi_addr: MSI address
 */
struct iproc_msi {
	/* [한국어] 이 MSI 컨트롤러가 붙어 있는 iProc PCIe 컨트롤러.
	 * 설정자: iproc_msi_init().
	 * 읽는 자: 레지스터 접근자 두 개가 pcie->base 를 얻는 데 쓰고, 로그가 pcie->dev 를 쓴다.
	 * 값 범위: 항상 유효.
	 * 동기화: 설정 후 읽기 전용. */
	struct iproc_pcie *pcie;
	/* [한국어] 이 SoC 변종에 맞는 레지스터 오프셋 2차원 테이블([그룹][레지스터]).
	 * 설정자: iproc_msi_init() 의 switch(pcie->type) 가 paxb 또는 paxc 테이블을 가리키게 한다.
	 * 읽는 자: iproc_msi_read_reg()/iproc_msi_write_reg() 가 [eq][reg] 로 색인한다.
	 * 값 범위: 두 정적 테이블 중 하나. switch 의 default 갈래로 빠지면 NULL 인 채
	 *   함수가 실패로 끝난다(아래 관찰 참조).
	 * 동기화: 설정 후 읽기 전용. */
	const u16 (*reg_offsets)[IPROC_MSI_REG_SIZE];
	/* [한국어] 그룹 배열. 원소 수는 nr_irqs 다.
	 * 설정자: iproc_msi_init() 이 devm_kcalloc 으로 할당하고 루프에서 채운다.
	 * 읽는 자: irq_setup/irq_free/exit 의 순회.
	 * 값 범위: 유효 포인터(할당 실패 시 -ENOMEM 으로 중단).
	 * 동기화: probe/remove 경로 전용. */
	struct iproc_msi_grp *grps;
	/* [한국어] GIC 에 연결된 전체 인터럽트 수 = MSI 그룹 수 = 이벤트 큐 수.
	 * 설정자: iproc_msi_init() 이 of_irq_count() 로 얻은 뒤 NR_HW_IRQS 로 자르고,
	 *   nr_cpus 의 배수가 되도록 다시 줄인다.
	 * 읽는 자: 벡터 총수 계산, 그룹 순회, hwirq_to_group() 의 나눗셈.
	 * 값 범위: nr_cpus 이상 NR_HW_IRQS 이하이며 nr_cpus 의 배수.
	 * 동기화: 설정 후 읽기 전용. */
	int nr_irqs;
	/* [한국어] 온라인 여부와 무관한 전체 CPU 수.
	 * 설정자: iproc_msi_init() 이 num_possible_cpus() 로 채운다.
	 * 읽는 자: hwirq_to_cpu()/hwirq_to_canonical_hwirq() 의 나머지 연산과
	 *   벡터 할당 시 order_base_2(nr_cpus * nr_irqs) 계산.
	 * 값 범위: 1 이상. 1 이면 어피니티를 위한 벡터 복제가 필요 없어
	 *   MSI_FLAG_MULTI_PCI_MSI 를 켠다.
	 * 동기화: 설정 후 읽기 전용. */
	int nr_cpus;
	/* [한국어] 별도 인터럽트 허용 레지스터(IPROC_MSI_INTS_EN)를 명시적으로 세워야 하는
	 * 구형 플랫폼인지 여부.
	 * 설정자: iproc_msi_init() 이 DT 의 brcm,pcie-msi-inten 속성 유무로 정한다.
	 * 읽는 자: iproc_msi_enable()/iproc_msi_disable() 의 조건 분기.
	 * 값 범위: true/false. kzalloc 이라 기본값 false.
	 * 동기화: 설정 후 읽기 전용. */
	bool has_inten_reg;
	/* [한국어] MSI 벡터 사용 여부를 나타내는 비트맵. 비트 하나가 hwirq 하나에 대응한다.
	 * 설정자: devm_bitmap_zalloc 으로 nr_msi_vecs 비트를 0 으로 할당하고,
	 *   이후 domain alloc/free 가 bitmap_find_free_region/bitmap_release_region 으로 갱신한다.
	 * 읽는 자: 같은 두 함수.
	 * 값 범위: nr_msi_vecs 비트.
	 * 동기화: 아래 bitmap_lock 뮤텍스가 반드시 필요하다 — 여러 장치가 동시에
	 *   MSI 를 요청하면 같은 벡터를 두 번 내줄 수 있기 때문이다. */
	unsigned long *bitmap;
	/* [한국어] 위 비트맵을 보호하는 뮤텍스.
	 * 설정자: iproc_msi_init() 의 mutex_init().
	 * 읽는 자: iproc_msi_irq_domain_alloc()/free() 가 잡고 푼다.
	 * 값 범위: 뮤텍스이므로 잠들 수 있는 문맥에서만 잡아야 한다 — 실제로 두 호출부는
	 *   모두 프로세스 컨텍스트다. 인터럽트 핸들러는 이 락을 잡지 않는다.
	 * 동기화: 이 파일에서 유일한 락이다. */
	struct mutex bitmap_lock;
	/* [한국어] 소프트웨어가 관리하는 총 MSI 벡터 수 = nr_irqs × EQ_LEN.
	 * 설정자: iproc_msi_init().
	 * 읽는 자: 비트맵 크기와 IRQ 도메인 크기.
	 * 값 범위: nr_irqs 가 최대 6 이므로 최대 384.
	 * 동기화: 설정 후 읽기 전용. */
	unsigned int nr_msi_vecs;
	/* [한국어] 이 컨트롤러의 IRQ 도메인. hwirq(MSI 벡터 번호)를 리눅스 virq 로 사상한다.
	 * 설정자: iproc_msi_alloc_domains() 의 msi_create_parent_irq_domain().
	 * 읽는 자: iproc_msi_handler() 가 generic_handle_domain_irq() 에 넘기고,
	 *   free_domains() 가 제거한다.
	 * 값 범위: 유효 포인터 또는 NULL(아직 만들지 않았거나 이미 제거함).
	 * 동기화: IRQ 코어가 관리한다. */
	struct irq_domain *inner_domain;
	/* [한국어] 이벤트 큐용으로 잡아야 할 4KB 영역의 개수.
	 * 설정자: iproc_msi_init() 의 switch — PAXB 는 1, PAXC 는 nr_irqs.
	 * 읽는 자: dma_alloc_coherent 크기 계산, iproc_msi_eq_offset() 의 분기,
	 *   enable 의 EQ_PAGE 프로그래밍 루프.
	 * 값 범위: 1 또는 nr_irqs.
	 * 동기화: 설정 후 읽기 전용. */
	unsigned int nr_eq_region;
	/* [한국어] MSI 포스티드 라이트 주소 영역의 개수. eq 쪽과 같은 규칙으로 정해진다.
	 * 설정자: iproc_msi_init() 의 switch.
	 * 읽는 자: iproc_msi_addr_offset() 의 분기와 enable 의 MSI_PAGE 루프.
	 * 값 범위: 1 또는 nr_irqs.
	 * 동기화: 설정 후 읽기 전용. */
	unsigned int nr_msi_region;
	/* [한국어] 이벤트 큐 메모리의 CPU 가상 주소.
	 * 설정자: iproc_msi_init() 의 dma_alloc_coherent().
	 * 읽는 자: decode_msi_hwirq() 가 여기서 MSI 데이터를 읽는다.
	 * 값 범위: 유효 포인터. coherent 할당이라 하드웨어가 쓴 값을 배리어 없이 볼 수 있다.
	 * 동기화: 하드웨어가 쓰고 소프트웨어가 읽는 공유 영역이며, 순서 보장은
	 *   "TAIL 갱신 전에 데이터가 메모리에 들어간다"는 하드웨어 보장에 의존한다. */
	void *eq_cpu;
	/* [한국어] 같은 메모리의 DMA(버스) 주소. 하드웨어에 알려 줄 값이다.
	 * 설정자: dma_alloc_coherent() 의 출력 인자.
	 * 읽는 자: iproc_msi_enable() 이 EQ_PAGE/EQ_PAGE_UPPER 에 나눠 쓴다.
	 * 값 범위: 유효한 DMA 주소.
	 * 동기화: 설정 후 읽기 전용. */
	dma_addr_t eq_dma;
	/* [한국어] MSI 포스티드 라이트가 향할 물리 주소의 기준점.
	 * 설정자: iproc_msi_init() 이 pcie->base_addr(컨트롤러 레지스터 창의 물리 주소)를
	 *   그대로 쓴다 — MSI 쓰기를 자기 레지스터 영역으로 되돌려 받는 구조다.
	 * 읽는 자: compose_msi_msg 가 오프셋을 더해 장치에 알려 줄 주소를 만들고,
	 *   enable 이 MSI_PAGE 레지스터에 프로그래밍한다.
	 * 값 범위: 유효한 물리 주소.
	 * 동기화: 설정 후 읽기 전용. */
	phys_addr_t msi_addr;
};

/* [한국어] PAXB 계열의 레지스터 오프셋 테이블. 행 = 그룹(0~5), 열 = enum iproc_msi_reg 순서.
 * 여섯 행 모두 EQ_PAGE(0x200)/EQ_PAGE_UPPER(0x2c0)/PAGE(0x204)/PAGE_UPPER(0x2c4)/
 * INTS_EN(0x208)이 같고 CTRL/EQ_HEAD/EQ_TAIL 만 그룹마다 다르다. 즉 큐 메모리와
 * MSI 주소는 그룹들이 공유하고(그래서 nr_eq_region = nr_msi_region = 1),
 * 큐 제어와 헤드·테일 포인터만 그룹별로 따로 존재한다. */
static const u16 iproc_msi_reg_paxb[NR_HW_IRQS][IPROC_MSI_REG_SIZE] = {
	{ 0x200, 0x2c0, 0x204, 0x2c4, 0x210, 0x250, 0x254, 0x208 },
	{ 0x200, 0x2c0, 0x204, 0x2c4, 0x214, 0x258, 0x25c, 0x208 },
	{ 0x200, 0x2c0, 0x204, 0x2c4, 0x218, 0x260, 0x264, 0x208 },
	{ 0x200, 0x2c0, 0x204, 0x2c4, 0x21c, 0x268, 0x26c, 0x208 },
	{ 0x200, 0x2c0, 0x204, 0x2c4, 0x220, 0x270, 0x274, 0x208 },
	{ 0x200, 0x2c0, 0x204, 0x2c4, 0x224, 0x278, 0x27c, 0x208 },
};

/* [한국어] PAXC 계열의 테이블. PAXB 와 정반대로 EQ_PAGE/PAGE 까지 행마다 다르다 —
 * 그래서 큐와 MSI 주소 영역을 그룹 수만큼 따로 잡는다(nr_eq_region = nr_irqs).
 * [상류 코드 관찰, 수정하지 않음] 행이 4개뿐이라 나머지 2개 행은 0 으로 채워지고,
 * 각 행의 초기화 값도 7개뿐이라 여덟 번째 열(IPROC_MSI_INTS_EN)이 0 이 된다.
 * PAXC 에서 has_inten_reg 가 참이 되면 오프셋 0 을 INTS_EN 으로 접근하게 되지만,
 * 그 조합이 실제로 존재하는지는 이 트리의 코드만으로 확인할 수 없다. */
static const u16 iproc_msi_reg_paxc[NR_HW_IRQS][IPROC_MSI_REG_SIZE] = {
	{ 0xc00, 0xc04, 0xc08, 0xc0c, 0xc40, 0xc50, 0xc60 },
	{ 0xc10, 0xc14, 0xc18, 0xc1c, 0xc44, 0xc54, 0xc64 },
	{ 0xc20, 0xc24, 0xc28, 0xc2c, 0xc48, 0xc58, 0xc68 },
	{ 0xc30, 0xc34, 0xc38, 0xc3c, 0xc4c, 0xc5c, 0xc6c },
};

/* [한국어]
 * iproc_msi_read_reg - 그룹별 MSI 레지스터를 32비트 읽는다
 *
 * @msi: MSI 컨트롤러. msi->pcie->base 가 레지스터 창의 시작이고
 *       msi->reg_offsets 가 [그룹][레지스터] 오프셋 테이블이다.
 * @reg: 읽을 레지스터의 논리 번호(enum iproc_msi_reg).
 * @eq: 그룹(= 이벤트 큐) 번호. 같은 논리 레지스터라도 그룹마다 주소가 다르므로 필수다.
 * @return: 읽은 32비트 값.
 *
 * 왜 2차원 색인인가: PAXB 는 여섯 그룹이 EQ_PAGE/PAGE/INTS_EN 을 공유하고
 * CTRL/EQ_HEAD/EQ_TAIL 만 그룹별로 갖는 반면, PAXC 는 거의 모든 레지스터가
 * 그룹별로 따로 있다. 두 배치를 하나의 코드로 다루려면 오프셋을 테이블로 빼고
 * [그룹][레지스터] 로 색인하는 수밖에 없다.
 *
 * _relaxed 판을 쓰는 이유: 이 함수는 인터럽트 핸들러의 뜨거운 경로(EQ_TAIL 을
 * 매 반복마다 읽는다)에서 불린다. 필요한 순서 보장은 큐 데이터를 읽는 쪽의
 * readl() 이 담당하므로 여기서는 배리어 비용을 아낀다.
 *
 * 실행 컨텍스트: probe 경로와 인터럽트 핸들러 양쪽에서 불린다. 락은 없다 —
 * 그룹마다 다른 레지스터를 만지고, 같은 그룹은 같은 CPU 에 고정되어 있기 때문이다.
 *
 * 에러 경로: 없다. [상류 코드 관찰] reg_offsets 가 NULL 인지 확인하지 않는다.
 *
 * 호출 체인:
 *   iproc_msi_handler() / iproc_msi_enable() / iproc_msi_disable()
 *     → [iproc_msi_read_reg] → readl_relaxed()
 */
static inline u32 iproc_msi_read_reg(struct iproc_msi *msi,
				     enum iproc_msi_reg reg,
				     unsigned int eq)
{
	/* [한국어] 레지스터 창의 기준 주소를 얻기 위해 컨트롤러를 꺼낸다. */
	struct iproc_pcie *pcie = msi->pcie;

	/* [한국어] [그룹][레지스터] 2차원 색인으로 오프셋을 구해 읽는다. 같은 이름의 레지스터라도
	 * 그룹마다 주소가 다를 수 있어 eq 를 반드시 함께 넘겨야 한다.
	 * _relaxed 판이라 배리어가 없다 — 인터럽트 핸들러의 뜨거운 경로라 비용을 아낀다. */
	return readl_relaxed(pcie->base + msi->reg_offsets[eq][reg]);
}

/* [한국어]
 * iproc_msi_write_reg - 그룹별 MSI 레지스터에 32비트를 쓴다
 *
 * @msi: MSI 컨트롤러.
 * @reg: 쓸 레지스터의 논리 번호.
 * @eq: 그룹 번호. 읽기 쪽과 달리 int 로 선언되어 있으나 의미는 같다.
 * @val: 쓸 값.
 *
 * 읽기 쪽과 완전히 대칭이며 같은 2차원 색인을 쓴다. 인터럽트 핸들러가
 * 처리한 만큼 EQ_HEAD 를 갱신할 때도 쓰이므로 역시 뜨거운 경로에 있다.
 *
 * 실행 컨텍스트: probe 경로와 인터럽트 핸들러.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   iproc_msi_handler() / iproc_msi_enable() / iproc_msi_disable()
 *     → [iproc_msi_write_reg] → writel_relaxed()
 */
static inline void iproc_msi_write_reg(struct iproc_msi *msi,
				       enum iproc_msi_reg reg,
				       int eq, u32 val)
{
	/* [한국어] 쓰기 쪽도 같은 방식으로 기준 주소를 얻는다. */
	struct iproc_pcie *pcie = msi->pcie;

	/* [한국어] [그룹][레지스터] 오프셋에 값을 쓴다. 배리어 없는 판이다. */
	writel_relaxed(val, pcie->base + msi->reg_offsets[eq][reg]);
}

/* [한국어]
 * hwirq_to_group - MSI 벡터가 어느 그룹(이벤트 큐)에 속하는지 구한다
 *
 * @msi: MSI 컨트롤러.
 * @hwirq: MSI 벡터 번호(0 ~ nr_msi_vecs-1).
 * @return: 그룹 번호(0 ~ nr_irqs-1).
 *
 * 단순히 그룹 수로 나눈 나머지다. 이 한 줄이 벡터를 그룹에 라운드로빈으로
 * 배분하는 정책 전부이며, 그 결과 연속된 벡터가 서로 다른 큐 — 따라서 서로 다른
 * GIC IRQ, 결국 서로 다른 CPU — 로 흩어진다. 부하 분산과 어피니티 구현이 모두
 * 이 나머지 연산 위에 세워져 있다.
 *
 * 실행 컨텍스트: MSI 메시지 조립 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   iproc_msi_addr_offset() → [hwirq_to_group]
 */
static inline u32 hwirq_to_group(struct iproc_msi *msi, unsigned long hwirq)
{
	/* [한국어] hwirq 를 그룹 수로 나눈 나머지가 그 벡터를 담당하는 그룹 번호다.
	 * 벡터를 그룹에 라운드로빈으로 배분하는 셈이며, 그 덕분에 연속된 벡터가
	 * 서로 다른 큐(=서로 다른 GIC IRQ, 결국 서로 다른 CPU)로 흩어진다. */
	return (hwirq % msi->nr_irqs);
}

/* [한국어]
 * iproc_msi_addr_offset - 이 벡터의 MSI 쓰기가 향할 주소 오프셋을 구한다
 *
 * @msi: MSI 컨트롤러.
 * @hwirq: MSI 벡터 번호.
 * @return: msi->msi_addr 에 더할 바이트 오프셋.
 *
 * 왜 필요한가: MSI 는 "정해진 주소에 정해진 값을 쓰는" 것으로 인터럽트를 표현한다.
 * 이 하드웨어에서는 쓰기 주소가 곧 목적지 이벤트 큐를 결정하므로, 벡터마다
 * 올바른 오프셋을 계산해 장치에 알려 줘야 한다.
 *
 * 동작: 그룹 번호를 구한 뒤 변종에 따라 두 가지로 나뉜다.
 *   - MSI 주소 영역이 여럿인 경우(PAXC): 그룹마다 4KB 씩 떨어진 주소.
 *   - 하나인 경우(PAXB): 한 4KB 영역 안에서 그룹마다 u32 한 칸(4바이트)씩만 떨어진 주소.
 *     주소가 서로 다르기만 하면 큐 구분이 되므로 촘촘히 배치해도 문제가 없다.
 *
 * 실행 컨텍스트: compose_msi_msg 콜백 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   iproc_msi_irq_compose_msi_msg() → [iproc_msi_addr_offset] → hwirq_to_group()
 */
static inline unsigned int iproc_msi_addr_offset(struct iproc_msi *msi,
						 unsigned long hwirq)
{
	/* [한국어] 큐마다 별도 4KB 영역을 쓰는 PAXC 인지 확인한다. */
	if (msi->nr_msi_region > 1)
		/* [한국어] 영역이 여러 개면 그룹마다 4KB 씩 떨어진 주소를 쓴다. */
		return hwirq_to_group(msi, hwirq) * MSI_MEM_REGION_SIZE;
	else
		/* [한국어] 영역이 하나면 한 4KB 안에서 그룹마다 u32 한 칸씩만 떨어진 주소를 쓴다.
		 * MSI 는 주소 자체가 어느 큐로 갈지를 결정하므로, 주소가 다르기만 하면 된다. */
		return hwirq_to_group(msi, hwirq) * sizeof(u32);
}

/* [한국어]
 * iproc_msi_eq_offset - 이벤트 큐의 메모리 시작 오프셋을 구한다
 *
 * @msi: MSI 컨트롤러.
 * @eq: 그룹(큐) 번호.
 * @return: msi->eq_cpu 에 더할 바이트 오프셋.
 *
 * addr_offset 과 같은 구조의 분기다.
 *   - 큐 영역이 여럿이면(PAXC) 4KB 단위로 건너뛴다 — 하드웨어가 4KB 정렬을 요구한다.
 *   - 하나면(PAXB) 큐 하나가 실제로 차지하는 크기(EQ_LEN × 4바이트 = 256바이트)만큼만
 *     건너뛰어 한 4KB 안에 여러 큐를 촘촘히 배치한다.
 *
 * 실행 컨텍스트: 인터럽트 핸들러 경로(decode_msi_hwirq)와 enable 경로.
 *
 * 호출 체인:
 *   decode_msi_hwirq() → [iproc_msi_eq_offset]
 */
static inline unsigned int iproc_msi_eq_offset(struct iproc_msi *msi, u32 eq)
{
	/* [한국어] 큐 메모리가 그룹마다 따로인지 확인한다. */
	if (msi->nr_eq_region > 1)
		/* [한국어] 따로면 4KB 단위로 건너뛴다. */
		return eq * EQ_MEM_REGION_SIZE;
	else
		/* [한국어] 공유면 큐 하나가 차지하는 실제 크기(64 × 4바이트 = 256바이트)만큼만 건너뛴다. */
		return eq * EQ_LEN * sizeof(u32);
}

/* [한국어] MSI 상위 도메인이 반드시 갖춰야 할 플래그 — 기본 도메인 연산과 기본 칩 연산을 쓰겠다는 선언. */
#define IPROC_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				  MSI_FLAG_USE_DEF_CHIP_OPS)
/* [한국어] 이 컨트롤러가 지원할 수 있는 플래그 — 일반 MSI 기능 전체와 MSI-X.
 * MSI_FLAG_MULTI_PCI_MSI 는 여기에 없고, CPU 가 하나일 때만 런타임에 추가된다.
 * 이유는 어피니티를 위해 벡터를 CPU 수만큼 복제해 쓰는 구조라
 * 다중 MSI 의 "연속된 벡터" 요구와 충돌하기 때문이다. */
#define IPROC_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK	| \
				   MSI_FLAG_PCI_MSIX)

static struct msi_parent_ops iproc_msi_parent_ops = {
	/* [한국어] 위에서 정의한 필수 플래그. */
	.required_flags		= IPROC_MSI_FLAGS_REQUIRED,
	/* [한국어] 지원 플래그. init 에서 이 정적 구조체의 필드를 직접 수정한다는 점에 주의 —
	 * 인스턴스별이 아니라 전역이므로, 한 시스템에 이 컨트롤러가 여럿 있어도
	 * 먼저 초기화된 쪽의 판단이 전체에 적용된다. */
	.supported_flags	= IPROC_MSI_FLAGS_SUPPORTED,
	/* [한국어] 이 도메인이 PCI MSI 버스용임을 알리는 토큰. 상위 계층이 도메인을 고를 때 쓴다. */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	/* [한국어] IRQ 도메인 이름 앞에 붙일 접두사 — /proc/interrupts 등에서 "iProc-" 로 시작한다. */
	.prefix			= "iProc-",
	/* [한국어] 장치별 MSI 정보 초기화를 공용 라이브러리 구현에 맡긴다. */
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};
/*
 * In iProc PCIe core, each MSI group is serviced by a GIC interrupt and a
 * dedicated event queue.  Each MSI group can support up to 64 MSI vectors.
 *
 * The number of MSI groups varies between different iProc SoCs.  The total
 * number of CPU cores also varies.  To support MSI IRQ affinity, we
 * distribute GIC interrupts across all available CPUs.  MSI vector is moved
 * from one GIC interrupt to another to steer to the target CPU.
 *
 * Assuming:
 * - the number of MSI groups is M
 * - the number of CPU cores is N
 * - M is always a multiple of N
 *
 * Total number of raw MSI vectors = M * 64
 * Total number of supported MSI vectors = (M * 64) / N
 */
/* [한국어]
 * hwirq_to_cpu - 이 벡터 사본이 어느 CPU 를 겨냥하는지 구한다
 *
 * @msi: MSI 컨트롤러.
 * @hwirq: MSI 벡터 번호.
 * @return: CPU 번호(0 ~ nr_cpus-1).
 *
 * 바로 위 영어 주석이 설명하는 어피니티 모델의 핵심이다. 하나의 논리 벡터를
 * CPU 수만큼 복제해 연속된 hwirq 로 할당해 두고, 그중 어느 사본을 쓰느냐로
 * 목적지 CPU 를 고른다. 그래서 총 지원 벡터 수가 (그룹수 × 64) / CPU수 로 줄어든다.
 * CPU 수로 나눈 나머지가 곧 그 사본의 CPU 번호가 되는 것은, 할당이 항상
 * nr_cpus 의 배수 크기로 정렬되어 이루어지기 때문이다.
 *
 * 실행 컨텍스트: 어피니티 설정 경로와 벡터 해제 경로.
 *
 * 호출 체인:
 *   iproc_msi_irq_set_affinity() / hwirq_to_canonical_hwirq() → [hwirq_to_cpu]
 */
static inline int hwirq_to_cpu(struct iproc_msi *msi, unsigned long hwirq)
{
	/* [한국어] hwirq 를 CPU 수로 나눈 나머지가 그 벡터를 처리할 CPU 번호다.
	 * 어피니티 구현의 핵심 아이디어 — 같은 논리 벡터를 CPU 수만큼 복제해 두고,
	 * 그중 어느 사본을 쓰느냐로 목적지 CPU 를 고른다. */
	return (hwirq % msi->nr_cpus);
}

/* [한국어]
 * hwirq_to_canonical_hwirq - 벡터 사본들의 대표(CPU 0) 번호를 구한다
 *
 * @msi: MSI 컨트롤러.
 * @hwirq: 임의의 사본 번호.
 * @return: 같은 논리 벡터의 CPU 0 사본 번호.
 *
 * hwirq 에서 그 CPU 번호를 빼면 CPU 0 사본이 된다. 이 대표 번호가 필요한 이유는
 * 세 가지다. (1) 비트맵 할당·해제는 반드시 같은 시작 번호로 짝을 맞춰야 한다.
 * (2) IRQ 도메인의 hwirq → virq 사상은 대표 번호로만 등록되어 있어, 어피니티를
 * 바꿔도 virq 가 흔들리지 않는다. (3) 인터럽트 수신 시 큐에서 읽은 벡터 번호는
 * 실제 사본 번호이므로, 도메인에 넘기기 전에 대표 번호로 되돌려야 한다.
 *
 * 실행 컨텍스트: 어피니티 설정, 벡터 해제, 인터럽트 디스패치 세 경로 모두.
 *
 * 호출 체인:
 *   iproc_msi_irq_set_affinity() / iproc_msi_irq_domain_free() / decode_msi_hwirq()
 *     → [hwirq_to_canonical_hwirq] → hwirq_to_cpu()
 */
static inline unsigned long hwirq_to_canonical_hwirq(struct iproc_msi *msi,
						     unsigned long hwirq)
{
	/* [한국어] 복제본 중 CPU 0 에 해당하는 대표(canonical) hwirq 를 구한다.
	 * hwirq 에서 그 CPU 번호를 빼면 CPU 0 사본이 된다. 비트맵 할당·해제와
	 * 인터럽트 디스패치는 모두 이 대표 번호를 기준으로 하므로,
	 * 어피니티를 바꿔도 virq 사상이 흔들리지 않는다. */
	return (hwirq - hwirq_to_cpu(msi, hwirq));
}

/* [한국어]
 * iproc_msi_irq_set_affinity - MSI 벡터를 다른 CPU 로 옮긴다
 *
 * @data: 대상 벡터의 irq_data. chip_data 에 컨트롤러가, hwirq 에 현재 사본 번호가 있다.
 * @mask: 사용자나 커널이 요청한 목적지 CPU 집합.
 * @force: 강제 설정 여부. 이 구현은 무시한다 — 어차피 어떤 CPU 든 옮길 수 있기 때문이다.
 * @return: IRQ_SET_MASK_OK_DONE = 이미 그 CPU 라 바꾼 것이 없다.
 *       IRQ_SET_MASK_OK = hwirq 를 바꿨으니 코어가 MSI 메시지를 다시 조립해
 *       장치에 써 주어야 한다.
 *
 * 왜 필요한가: 이 하드웨어에는 "이 벡터를 저 CPU 로 보내라"는 레지스터가 없다.
 * 대신 각 GIC IRQ(= 이벤트 큐)가 특정 CPU 에 고정되어 있으므로, 벡터를 다른 큐로
 * 보내면 결과적으로 다른 CPU 가 처리하게 된다. 그리고 어느 큐로 갈지는 장치가
 * 쓰는 MSI 주소가 결정한다. 그래서 어피니티 변경은 곧 "장치가 쓸 주소를 바꾸는 일"이며,
 * 그 주소는 hwirq 에서 계산되므로 결국 hwirq 사본을 바꾸는 것으로 귀결된다.
 *
 * 동작 과정:
 *   1) 요청 마스크의 첫 CPU 를 목적지로 고른다. 한 벡터를 여러 CPU 에 분산할 수
 *      없으므로 하나만 고른다.
 *   2) 현재 사본이 이미 그 CPU 면 IRQ_SET_MASK_OK_DONE 으로 끝낸다.
 *   3) 아니면 대표 번호 + 목표 CPU 번호로 hwirq 를 바꾸고 IRQ_SET_MASK_OK 를 돌려준다.
 *      이 반환값을 받은 코어가 compose_msi_msg 를 다시 불러 장치의 MSI 주소를 갱신한다.
 *   4) 두 경로 모두에서 실제 어피니티를 목표 CPU 하나로 기록한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(irq_set_affinity 계열 호출 경로).
 * 비트맵을 건드리지 않으므로 락이 필요 없다 — hwirq 값만 바꾸는데,
 * 그 값은 이미 이 벡터에 할당된 사본 범위 안에서만 움직이기 때문이다.
 *
 * 에러 경로: 없다. 어떤 CPU 든 유효하므로 실패할 수 없다.
 *
 * 호출 체인:
 *   irq_set_affinity() / IRQ 코어 → irq_chip.irq_set_affinity == [이 함수]
 *     → hwirq_to_cpu() / hwirq_to_canonical_hwirq()
 *     → (반환 후) 코어가 iproc_msi_irq_compose_msi_msg() 를 다시 호출
 */
static int iproc_msi_irq_set_affinity(struct irq_data *data,
				      const struct cpumask *mask, bool force)
{
	/* [한국어] 이 벡터의 컨트롤러를 irq_data 의 chip_data 에서 꺼낸다. 그 값은
	 * irq_domain_set_info() 에 넘긴 domain->host_data 다. */
	struct iproc_msi *msi = irq_data_get_irq_chip_data(data);
	/* [한국어] 요청된 CPU 마스크에서 첫 번째 CPU 를 목적지로 고른다. 이 하드웨어는
	 * 한 벡터를 여러 CPU 에 분산할 수 없어 하나만 고를 수밖에 없다. */
	int target_cpu = cpumask_first(mask);
	int curr_cpu;
	int ret;

	/* [한국어] 현재 이 벡터가 어느 CPU 사본을 쓰고 있는지 계산한다. */
	curr_cpu = hwirq_to_cpu(msi, data->hwirq);
	/* [한국어] 이미 원하는 CPU 라면 바꿀 것이 없다. */
	if (curr_cpu == target_cpu)
		/* [한국어] IRQ_SET_MASK_OK_DONE 은 "처리했고 코어가 추가로 할 일이 없다"는 뜻이다. */
		ret = IRQ_SET_MASK_OK_DONE;
	else {
		/* steer MSI to the target CPU */
		/* [한국어] 핵심 조작: hwirq 를 대표 번호로 되돌린 뒤 목표 CPU 번호를 더해 다른 사본을 가리키게 한다.
		 * hwirq 가 바뀌면 compose_msi_msg 가 만드는 MSI 주소도 달라져, 장치의 다음 MSI 쓰기가
		 * 다른 이벤트 큐로 들어가고 결국 다른 GIC IRQ 를 통해 목표 CPU 에서 처리된다.
		 * 다만 이 대입은 이미 만들어진 메시지를 바꾸지 않는다 — 코어가 IRQ_SET_MASK_OK 를
		 * 받으면 compose 를 다시 불러 장치에 새 주소를 써 준다. */
		data->hwirq = hwirq_to_canonical_hwirq(msi, data->hwirq) + target_cpu;
		/* [한국어] IRQ_SET_MASK_OK 는 "바꿨으니 코어가 메시지를 다시 쓰라"는 뜻이다. */
		ret = IRQ_SET_MASK_OK;
	}

	/* [한국어] 실제 어피니티를 목표 CPU 하나로 기록한다. 두 경로 모두에서 불러야
	 * /proc/irq/ 아래 effective_affinity 가 정확해진다. */
	irq_data_update_effective_affinity(data, cpumask_of(target_cpu));

	/* [한국어] 위에서 정한 두 반환값 중 하나를 그대로 돌려준다. */
	return ret;
}

/* [한국어]
 * iproc_msi_irq_compose_msi_msg - 장치에 써 넣을 MSI 주소와 데이터를 만든다
 *
 * @data: 대상 벡터의 irq_data.
 * @msg: 결과를 채울 MSI 메시지 구조체. 코어가 이 내용을 장치의 MSI/MSI-X
 *       capability 레지스터에 실제로 써 넣는다.
 *
 * 왜 필요한가: MSI 는 인터럽트를 "메모리 쓰기"로 표현한다. 장치가 어디에 무엇을
 * 쓸지 알려 주는 것이 이 콜백의 일이며, 이 하드웨어에서는 주소가 목적지 큐를,
 * 데이터가 벡터 번호를 나타낸다.
 *
 * 동작 과정:
 *   1) 기준 주소(msi->msi_addr)에 이 벡터가 속한 그룹의 오프셋을 더한다.
 *      그룹이 곧 큐이므로 이 계산 하나가 목적지 큐를 결정한다.
 *   2) 64비트 주소를 하위·상위 32비트로 나눠 담는다.
 *   3) 데이터는 hwirq 를 5비트 왼쪽으로 민 값이다. 하위 5비트를 비워 두는 이유는
 *      다중 MSI 에서 장치가 그 자리에 벡터 인덱스(0~31)를 OR 해 보내기 때문이며,
 *      수신 쪽 decode_msi_hwirq() 가 (data >> 5) + (data & 0x1f) 로 되돌린다.
 *
 * 실행 컨텍스트: 벡터 할당 직후와 어피니티 변경 직후, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   IRQ 코어(벡터 활성화 또는 어피니티 변경 후)
 *     → irq_chip.irq_compose_msi_msg == [이 함수] → iproc_msi_addr_offset()
 */
static void iproc_msi_irq_compose_msi_msg(struct irq_data *data,
					  struct msi_msg *msg)
{
	/* [한국어] 이 벡터의 컨트롤러를 꺼낸다. */
	struct iproc_msi *msi = irq_data_get_irq_chip_data(data);
	/* [한국어] 장치에 알려 줄 MSI 목적지 주소를 담을 변수. */
	dma_addr_t addr;

	/* [한국어] 기준 주소에 이 벡터가 속한 그룹의 오프셋을 더한다. 주소가 곧 큐 선택이므로,
	 * 이 계산 하나가 "어느 이벤트 큐로 갈지"를 결정한다. */
	addr = msi->msi_addr + iproc_msi_addr_offset(msi, data->hwirq);
	/* [한국어] 64비트 주소의 하위 32비트. */
	msg->address_lo = lower_32_bits(addr);
	/* [한국어] 상위 32비트. MSI 는 주소를 두 워드로 나눠 전달한다. */
	msg->address_hi = upper_32_bits(addr);
	/* [한국어] MSI 데이터에 hwirq 를 5비트 왼쪽으로 밀어 넣는다. 하위 5비트를 비워 두는 이유는
	 * 다중 MSI 에서 장치가 그 자리에 벡터 인덱스(0~31)를 OR 해 보내기 때문이다.
	 * 수신 쪽 decode_msi_hwirq() 가 (data >> 5) + (data & 0x1f) 로 되돌린다. */
	msg->data = data->hwirq << 5;
}

static struct irq_chip iproc_msi_bottom_irq_chip = {
	/* [한국어] /proc/interrupts 에 표시될 이름. */
	.name = "MSI",
	/* [한국어] 어피니티 변경 콜백 — 이 컨트롤러의 특징적인 기능이다. */
	.irq_set_affinity = iproc_msi_irq_set_affinity,
	/* [한국어] MSI 메시지 조립 콜백. */
	.irq_compose_msi_msg = iproc_msi_irq_compose_msi_msg,
};

/* [한국어]
 * iproc_msi_irq_domain_alloc - MSI 벡터를 예약하고 virq 에 묶는다
 *
 * @domain: 이 컨트롤러의 IRQ 도메인. host_data 에 struct iproc_msi 가 들어 있다.
 * @virq: 코어가 미리 잡아 둔 첫 리눅스 IRQ 번호.
 * @nr_irqs: 요청받은 벡터 개수.
 * @args: 상위 계층이 넘기는 인자. 이 구현은 쓰지 않는다.
 * @return: 0 = 성공. -EINVAL = 다중 CPU 환경에서 다중 벡터를 요청함.
 *       -ENOSPC = 비트맵에 충분한 빈 구간이 없음(코어가 더 적은 수로 재시도할 수 있다).
 *
 * 왜 필요한가: 장치가 MSI 를 요청하면 결국 이 콜백이 실제 하드웨어 벡터를 골라
 * 리눅스 IRQ 번호에 묶어야 한다. 이 컨트롤러의 특수성은 어피니티를 위해 벡터를
 * CPU 수만큼 복제해 한꺼번에 예약한다는 점이다.
 *
 * 동작 과정:
 *   1) nr_cpus > 1 이면서 nr_irqs > 1 이면 거절한다. 어피니티용 복제와 다중 MSI 의
 *      "연속된 벡터" 요구를 동시에 만족시킬 수 없기 때문이며, 그래서 nr_cpus == 1 일
 *      때만 MSI_FLAG_MULTI_PCI_MSI 를 지원 목록에 넣는다.
 *   2) 뮤텍스를 잡고 bitmap_find_free_region() 으로 nr_cpus × nr_irqs 개를
 *      2의 거듭제곱 단위로 예약한다. order 정렬이 보장되어야 (대표 번호 + CPU 번호)로
 *      사본을 가리키는 계산이 성립한다.
 *   3) 락을 풀고, 실패면 -ENOSPC.
 *   4) 요청 개수만큼 virq + i 를 hwirq + i 에 묶으면서 아래쪽 irq_chip 과
 *      chip_data(= msi), 흐름 핸들러 handle_simple_irq 를 지정한다.
 *      MSI 는 본질적으로 에지 트리거라 마스킹·EOI 가 필요 없어 가장 단순한 핸들러를 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡으므로 잠들 수 있는 곳에서만 불려야 하고,
 * 실제로 MSI 할당은 장치 probe 경로에서 일어난다.
 *
 * 에러 경로: 두 갈래 모두 아무것도 할당하지 않은 상태이므로 정리할 것이 없다.
 *
 * 호출 체인:
 *   pci_alloc_irq_vectors() → MSI 코어 → irq_domain_alloc_irqs()
 *     → irq_domain_ops.alloc == [이 함수]
 *     → bitmap_find_free_region() / irq_domain_set_info()
 */
static int iproc_msi_irq_domain_alloc(struct irq_domain *domain,
				      unsigned int virq, unsigned int nr_irqs,
				      void *args)
{
	/* [한국어] 도메인의 host_data 에 심어 둔 컨트롤러. */
	struct iproc_msi *msi = domain->host_data;
	/* [한국어] 할당할 첫 hwirq 와 순회 인덱스. */
	int hwirq, i;

	/* [한국어] CPU 가 여럿인데 벡터를 여러 개 한 번에 달라고 하면 거절한다.
	 * 어피니티를 위해 벡터를 CPU 수만큼 복제해야 하는데, 다중 MSI 는 연속된
	 * 벡터 번호를 요구하므로 두 요구를 동시에 만족시킬 수 없기 때문이다.
	 * 그래서 nr_cpus == 1 일 때만 MSI_FLAG_MULTI_PCI_MSI 를 켠다. */
	if (msi->nr_cpus > 1 && nr_irqs > 1)
		/* [한국어] 요청 자체가 이 하드웨어로는 불가능하다는 뜻의 -EINVAL. */
		return -EINVAL;

	/* [한국어] 비트맵은 여러 장치가 동시에 건드릴 수 있으므로 뮤텍스로 보호한다. */
	mutex_lock(&msi->bitmap_lock);

	/*
	 * Allocate 'nr_irqs' multiplied by 'nr_cpus' number of MSI vectors
	 * each time
	 */
	/* [한국어] nr_cpus × nr_irqs 개를 2의 거듭제곱 단위로 한 번에 예약한다.
	 * order_base_2() 는 그 개수를 담을 수 있는 최소 2의 거듭제곱 지수를 준다.
	 * bitmap_find_free_region() 은 그 크기에 정렬된 빈 구간을 찾아 표시하고
	 * 시작 비트 번호를 돌려준다 — 정렬이 보장되어야 hwirq + cpu 로 사본을
	 * 가리키는 계산이 성립한다. */
	hwirq = bitmap_find_free_region(msi->bitmap, msi->nr_msi_vecs,
					order_base_2(msi->nr_cpus * nr_irqs));

	/* [한국어] 락 해제. 아래 오류 처리와 도메인 설정은 락 없이 해도 안전하다. */
	mutex_unlock(&msi->bitmap_lock);

	/* [한국어] 음수는 빈 구간을 못 찾았다는 뜻이다. */
	if (hwirq < 0)
		/* [한국어] -ENOSPC 로 벡터 고갈을 알린다. 코어는 더 적은 수로 재시도할 수 있다. */
		return -ENOSPC;

	/* [한국어] 요청받은 개수만큼 virq 를 설정한다. */
	for (i = 0; i < nr_irqs; i++) {
		/* [한국어] virq + i 를 hwirq + i 에 묶고, 아래쪽 irq_chip 과 chip_data(= msi),
		 * 그리고 흐름 핸들러 handle_simple_irq 를 지정한다. handle_simple_irq 는
		 * 마스킹·EOI 가 필요 없는 에지 성격의 인터럽트에 쓰는 가장 단순한 핸들러로,
		 * MSI 가 본질적으로 에지 트리거이기 때문에 적합하다. */
		irq_domain_set_info(domain, virq + i, hwirq + i,
				    &iproc_msi_bottom_irq_chip,
				    domain->host_data, handle_simple_irq,
				    NULL, NULL);
	}

	/* [한국어] 모든 virq 설정 성공. */
	return 0;
}

/* [한국어]
 * iproc_msi_irq_domain_free - 예약했던 MSI 벡터를 되돌린다
 *
 * @domain: 이 컨트롤러의 IRQ 도메인.
 * @virq: 해제할 첫 리눅스 IRQ 번호.
 * @nr_irqs: 해제할 개수. 할당 때와 같은 값이어야 한다.
 *
 * alloc 과 정확히 대칭이다. 주의할 점 하나는 반드시 대표(canonical) hwirq 로
 * 해제해야 한다는 것이다. 어피니티 변경으로 data->hwirq 가 다른 사본을 가리키고
 * 있을 수 있는데, 비트맵 예약은 대표 번호에서 시작했기 때문이다. 이 변환을
 * 빠뜨리면 엉뚱한 구간을 해제해 비트맵이 서서히 오염된다.
 *
 * 동작 과정:
 *   1) 첫 virq 의 irq_data 에서 컨트롤러와 현재 hwirq 를 얻는다.
 *   2) 뮤텍스를 잡고 대표 hwirq 를 구해 할당 때와 같은 order 로 반납한다.
 *   3) 락을 풀고 상위 도메인에 virq 반납을 위임한다(계층형 도메인의 관례).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡는다.
 *
 * 에러 경로: 없다. 해제는 실패할 수 없다.
 *
 * 호출 체인:
 *   pci_free_irq_vectors() → MSI 코어 → irq_domain_free_irqs()
 *     → irq_domain_ops.free == [이 함수]
 *     → bitmap_release_region() / irq_domain_free_irqs_parent()
 */
static void iproc_msi_irq_domain_free(struct irq_domain *domain,
				      unsigned int virq, unsigned int nr_irqs)
{
	/* [한국어] 해제할 첫 virq 의 irq_data 를 얻는다. */
	struct irq_data *data = irq_domain_get_irq_data(domain, virq);
	/* [한국어] 거기서 컨트롤러를 꺼낸다. */
	struct iproc_msi *msi = irq_data_get_irq_chip_data(data);
	/* [한국어] 해제할 시작 비트 번호를 담을 변수. */
	unsigned int hwirq;

	/* [한국어] 비트맵 보호. */
	mutex_lock(&msi->bitmap_lock);

	/* [한국어] 할당 때와 같은 기준으로 대표 hwirq 를 구한다. 할당은 대표 번호에서 시작했으므로
	 * 해제도 반드시 대표 번호로 해야 짝이 맞는다. */
	hwirq = hwirq_to_canonical_hwirq(msi, data->hwirq);
	/* [한국어] 할당과 대칭으로 같은 order 만큼 비트를 되돌린다. */
	bitmap_release_region(msi->bitmap, hwirq,
			      order_base_2(msi->nr_cpus * nr_irqs));

	/* [한국어] 락 해제. */
	mutex_unlock(&msi->bitmap_lock);

	/* [한국어] 상위 도메인에 virq 반납을 위임한다. 계층형 도메인의 관례다. */
	irq_domain_free_irqs_parent(domain, virq, nr_irqs);
}

static const struct irq_domain_ops msi_domain_ops = {
	/* [한국어] 벡터 할당 콜백. */
	.alloc = iproc_msi_irq_domain_alloc,
	/* [한국어] 벡터 해제 콜백. 이 두 개만 구현하면 도메인이 성립한다. */
	.free = iproc_msi_irq_domain_free,
};

/* [한국어]
 * decode_msi_hwirq - 이벤트 큐 항목에서 MSI 벡터 번호를 복원한다
 *
 * @msi: MSI 컨트롤러.
 * @eq: 읽을 큐 번호.
 * @head: 큐 안에서 읽을 항목의 인덱스(0 ~ EQ_LEN-1).
 * @return: 도메인에 넘길 수 있는 대표(CPU 0 기준) hwirq.
 *
 * 왜 필요한가: 장치가 보낸 MSI 데이터는 하드웨어가 이벤트 큐 메모리에 u32 하나로
 * 적어 둔다. 그 값을 리눅스가 아는 벡터 번호로 되돌리는 것이 이 함수의 일이다.
 *
 * 동작 과정:
 *   1) 큐 시작 오프셋 + 항목 인덱스 × 4바이트로 항목의 주소를 구한다.
 *   2) readl() 로 값을 읽는다. 큐 메모리는 coherent DMA 로 잡혀 있어 캐시 무효화가
 *      필요 없고, readl 의 배리어가 TAIL 을 읽은 뒤 데이터를 읽는 순서를 지켜 준다.
 *      (__iomem 캐스팅은 MMIO 라는 뜻이 아니라 readl 을 쓰기 위한 상류의 표기다.)
 *   3) compose 가 만든 데이터의 역변환: 상위 27비트가 기준 hwirq, 하위 5비트가
 *      장치가 더한 벡터 인덱스이므로 둘을 더하면 실제 사본 번호가 된다.
 *   4) 위 영어 주석대로 여러 사본이 한 논리 벡터를 가리키므로, 도메인이 아는
 *      대표 번호로 되돌려 돌려준다.
 *
 * 실행 컨텍스트: 인터럽트 핸들러(체인 핸들러) 안. 잠들 수 없다.
 *
 * 에러 경로: 없다. 큐에 들어 있는 값을 그대로 신뢰한다.
 *
 * 호출 체인:
 *   iproc_msi_handler() → [decode_msi_hwirq]
 *     → iproc_msi_eq_offset() / readl() / hwirq_to_canonical_hwirq()
 */
static inline u32 decode_msi_hwirq(struct iproc_msi *msi, u32 eq, u32 head)
{
	/* [한국어] 이벤트 큐 항목을 가리킬 포인터. */
	u32 __iomem *msg;
	/* [한국어] 복원한 벡터 번호. */
	u32 hwirq;
	/* [한국어] 큐 메모리 안에서의 바이트 오프셋. */
	unsigned int offs;

	/* [한국어] 큐 시작 오프셋에 항목 인덱스 × 4바이트를 더한다. */
	offs = iproc_msi_eq_offset(msi, eq) + head * sizeof(u32);
	/* [한국어] coherent DMA 로 잡은 큐 메모리의 해당 위치. __iomem 으로 캐스팅하지만 실제로는
	 * MMIO 가 아니라 일반 메모리다 — 상류가 readl() 로 읽기 위해 붙인 표기다. */
	msg = (u32 __iomem *)(msi->eq_cpu + offs);
	/* [한국어] 하드웨어가 써 넣은 MSI 데이터를 읽는다. readl 은 배리어를 포함한 판이라,
	 * TAIL 을 먼저 읽고 나서 데이터를 읽는 순서가 뒤집히지 않는다. */
	hwirq = readl(msg);
	/* [한국어] compose 가 만든 데이터의 역변환. 상위 27비트가 기준 hwirq 이고
	 * 하위 5비트가 장치가 더한 벡터 인덱스이므로, 둘을 더하면 실제 벡터가 된다. */
	hwirq = (hwirq >> 5) + (hwirq & 0x1f);

	/*
	 * Since we have multiple hwirq mapped to a single MSI vector,
	 * now we need to derive the hwirq at CPU0.  It can then be used to
	 * mapped back to virq.
	 */
	/* [한국어] 여러 hwirq 사본이 한 논리 벡터를 가리키므로, CPU 0 기준 대표 번호로 되돌려
	 * 도메인이 아는 번호와 맞춘다. */
	return hwirq_to_canonical_hwirq(msi, hwirq);
}

/* [한국어]
 * iproc_msi_handler - GIC 인터럽트 하나를 받아 쌓인 MSI 들을 모두 디스패치한다
 *
 * @desc: 이 GIC IRQ 의 irq_desc. handler_data 에 struct iproc_msi_grp 이 들어 있어,
 *       어느 그룹(= 어느 이벤트 큐)의 인터럽트인지 알 수 있다.
 *
 * 왜 필요한가: 이 컨트롤러는 최대 64개의 MSI 벡터를 GIC 인터럽트 하나로 모아
 * 올린다. 그 하나를 받아 큐에 쌓인 MSI 데이터를 꺼내 각 장치 드라이버의 핸들러로
 * 나눠 주는 것이 이 함수이며, 그래서 "체인(chained) 핸들러" 라고 부른다.
 * request_irq() 로 등록하는 일반 핸들러와 달리 상위 컨트롤러의 흐름 제어를 직접
 * 다루므로 chained_irq_enter()/exit() 로 감싸야 한다.
 *
 * 동작 과정:
 *   1) chained_irq_enter() 로 진입 — 상위 컨트롤러에 처리 시작을 알린다.
 *   2) irq_desc 에서 그룹과 컨트롤러, 큐 번호를 꺼낸다.
 *   3) EQ_HEAD 를 한 번 읽어 두고 무한 루프에 들어간다.
 *   4) 매 반복마다 EQ_TAIL 을 다시 읽어 처리할 개수를 계산한다.
 *      tail < head 면 링 버퍼가 한 바퀴 돌아간 것이므로 EQ_LEN 에서 빼 보정한다.
 *   5) 개수가 0 이면 루프를 빠져나간다 — 이것이 유일한 탈출구다.
 *   6) 그렇지 않으면 그만큼 항목을 꺼내 벡터 번호를 복원하고
 *      generic_handle_domain_irq() 로 해당 장치의 핸들러를 부른다.
 *   7) 처리한 만큼 EQ_HEAD 를 하드웨어에 알려 그 자리를 재사용할 수 있게 한다.
 *   8) 다시 4)로 돌아가 처리 중 새로 들어온 이벤트까지 소진한다.
 *      이 재검사가 없으면 처리 도중 도착한 MSI 가 다음 인터럽트까지 지연된다.
 *   9) chained_irq_exit() 로 이탈 — EOI 를 보내고 마스킹을 되돌린다.
 *
 * 위 영어 주석이 밝히듯 head 는 소프트웨어가, tail 은 하드웨어가 갱신하며,
 * "tail 이 갱신되기 전에 데이터가 메모리에 들어가 있다"는 것이 하드웨어의 보장이다.
 * 그 보장 덕분에 별도의 배리어 없이 tail 만 보고 데이터를 읽어도 안전하다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. 잠들 수 없고, 이 파일의 유일한 락인
 * bitmap_lock 을 여기서 잡지 않는 것도 그 때문이다. 그룹이 특정 CPU 에 고정되어
 * 있으므로 같은 큐에 대해 이 함수가 동시에 두 번 실행되지 않는다.
 *
 * 에러 경로: 없다. 큐가 비어 있으면 그냥 빠져나간다.
 *
 * 호출 체인:
 *   하드웨어 MSI 쓰기 → 이벤트 큐 적재 → GIC 인터럽트
 *     → 체인 핸들러 == [iproc_msi_handler]
 *     → decode_msi_hwirq() → generic_handle_domain_irq() → 장치 드라이버 핸들러
 */
static void iproc_msi_handler(struct irq_desc *desc)
{
	/* [한국어] 이 GIC IRQ 를 소유한 상위 인터럽트 칩. 체인 진입·이탈에 필요하다. */
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] 이 IRQ 에 묶인 MSI 그룹. */
	struct iproc_msi_grp *grp;
	/* [한국어] 그 그룹의 컨트롤러. */
	struct iproc_msi *msi;
	/* [한국어] 큐 번호와 헤드·테일 인덱스, 처리할 이벤트 수. */
	u32 eq, head, tail, nr_events;
	/* [한국어] 복원한 벡터 번호. */
	unsigned long hwirq;

	/* [한국어] 체인 핸들러 진입 — 상위 컨트롤러에 "내가 처리 중"임을 알리고 필요한 마스킹을 한다.
	 * 이 짝을 빠뜨리면 인터럽트가 무한히 재진입하거나 영영 다시 오지 않는다. */
	chained_irq_enter(chip, desc);

	/* [한국어] irq_desc 의 handler_data 로 심어 둔 그룹을 꺼낸다.
	 * irq_set_chained_handler_and_data() 가 넣어 둔 값이다. */
	grp = irq_desc_get_handler_data(desc);
	/* [한국어] 그룹에서 컨트롤러로. */
	msi = grp->msi;
	/* [한국어] 이 그룹이 담당하는 큐 번호. */
	eq = grp->eq;

	/*
	 * iProc MSI event queue is tracked by head and tail pointers.  Head
	 * pointer indicates the next entry (MSI data) to be consumed by SW in
	 * the queue and needs to be updated by SW.  iProc MSI core uses the
	 * tail pointer as the next data insertion point.
	 *
	 * Entries between head and tail pointers contain valid MSI data.  MSI
	 * data is guaranteed to be in the event queue memory before the tail
	 * pointer is updated by the iProc MSI core.
	 */
	/* [한국어] 소프트웨어가 다음에 읽을 위치를 하드웨어 레지스터에서 읽는다.
	 * 마스크를 씌우는 이유는 상위 비트에 다른 정보가 있을 수 있어서다. */
	head = iproc_msi_read_reg(msi, IPROC_MSI_EQ_HEAD,
				  eq) & IPROC_MSI_EQ_MASK;
	/* [한국어] 바깥 루프 — 처리 도중 새로 들어온 이벤트까지 모두 소진할 때까지 반복한다. */
	do {
		/* [한국어] 하드웨어가 다음에 쓸 위치. 매 반복마다 다시 읽어야 새 이벤트를 발견할 수 있다. */
		tail = iproc_msi_read_reg(msi, IPROC_MSI_EQ_TAIL,
					  eq) & IPROC_MSI_EQ_MASK;

		/*
		 * Figure out total number of events (MSI data) to be
		 * processed.
		 */
		/* [한국어] 링 버퍼의 두 포인터 차이로 처리할 개수를 구한다. tail 이 head 보다 작으면
		 * 한 바퀴 돌아간 것이므로 EQ_LEN 에서 빼는 방식으로 보정한다. */
		nr_events = (tail < head) ?
			(EQ_LEN - (head - tail)) : (tail - head);
		/* [한국어] 새 이벤트가 없으면 바깥 루프를 빠져나간다 — 이것이 유일한 탈출구다. */
		if (!nr_events)
			break;

		/* process all outstanding events */
		/* [한국어] 발견한 이벤트를 모두 처리한다. */
		while (nr_events--) {
			/* [한국어] 큐 항목에서 벡터 번호를 복원한다. */
			hwirq = decode_msi_hwirq(msi, eq, head);
			/* [한국어] 그 벡터에 묶인 리눅스 virq 의 핸들러를 부른다. 도메인이 hwirq → virq 사상을
			 * 알고 있으므로 여기서 장치 드라이버의 인터럽트 핸들러까지 이어진다. */
			generic_handle_domain_irq(msi->inner_domain, hwirq);

			/* [한국어] 다음 항목으로. */
			head++;
			/* [한국어] 링 버퍼이므로 EQ_LEN 에서 되돌아간다. */
			head %= EQ_LEN;
		}

		/*
		 * Now all outstanding events have been processed.  Update the
		 * head pointer.
		 */
		/* [한국어] 처리한 만큼 헤드를 하드웨어에 알린다. 이 쓰기가 있어야 하드웨어가
		 * 그 자리를 다시 쓸 수 있다. */
		iproc_msi_write_reg(msi, IPROC_MSI_EQ_HEAD, eq, head);

		/*
		 * Now go read the tail pointer again to see if there are new
		 * outstanding events that came in during the above window.
		 */
	/* [한국어] 바깥 루프의 조건은 항상 참이고, 탈출은 위의 nr_events == 0 검사뿐이다. */
	} while (true);

	/* [한국어] 체인 핸들러 이탈 — 상위 컨트롤러에 EOI 를 보내고 마스킹을 되돌린다. */
	chained_irq_exit(chip, desc);
}

/* [한국어]
 * iproc_msi_enable - 큐·주소 영역을 하드웨어에 알리고 모든 이벤트 큐를 켠다
 *
 * @msi: MSI 컨트롤러. 이 시점에는 모든 필드가 채워져 있고 IRQ 도메인과
 *       체인 핸들러까지 준비된 상태다.
 *
 * 왜 마지막에 부르는가: 이 함수가 CTRL 비트를 세우는 순간부터 하드웨어가
 * MSI 를 받아 큐에 쌓고 GIC 인터럽트를 올릴 수 있다. 그 전에 큐 메모리 주소와
 * 핸들러가 모두 준비되어 있어야 하므로, iproc_msi_init() 의 가장 마지막 단계다.
 *
 * 동작 과정:
 *   1) 이벤트 큐 메모리의 DMA 주소를 EQ_PAGE / EQ_PAGE_UPPER 에 나눠 쓴다.
 *      영역이 하나면(PAXB) 한 번, 그룹마다 따로면(PAXC) 그룹 수만큼 반복한다.
 *   2) MSI 포스티드 라이트가 향할 물리 주소를 MSI_PAGE / MSI_PAGE_UPPER 에 쓴다.
 *   3) 그룹마다 CTRL 에 INTR_EN | INT_N_EVENT | EQ_EN 세 비트를 한 번에 세운다.
 *   4) 구형 플랫폼(has_inten_reg)이면 별도 허용 레지스터에서 이 그룹의 비트만
 *      읽기-수정-쓰기로 세운다. 다른 그룹의 설정을 지우지 않기 위해서다.
 *
 * 64비트 주소를 32비트 레지스터 쌍에 나눠 싣는 것은 이 계열 하드웨어의 관례이며,
 * lower_32_bits()/upper_32_bits() 가 그 분해를 담당한다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트. 락은 없다 —
 * 아직 인터럽트가 올라오지 않는 시점이기 때문이다.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   iproc_msi_init() → [iproc_msi_enable] → iproc_msi_write_reg() / iproc_msi_read_reg()
 */
static void iproc_msi_enable(struct iproc_msi *msi)
{
	/* [한국어] 영역 인덱스와 큐 번호. */
	int i, eq;
	/* [한국어] CTRL 레지스터에 쓸 값. */
	u32 val;

	/* Program memory region for each event queue */
	/* [한국어] 큐 메모리 영역을 하드웨어에 알린다. 영역이 하나면 한 번, PAXC 처럼 여럿이면
	 * 그룹 수만큼 반복한다. */
	for (i = 0; i < msi->nr_eq_region; i++) {
		/* [한국어] i 번째 영역의 DMA 주소. */
		dma_addr_t addr = msi->eq_dma + (i * EQ_MEM_REGION_SIZE);

		/* [한국어] 하위 32비트를 EQ_PAGE 에. */
		iproc_msi_write_reg(msi, IPROC_MSI_EQ_PAGE, i,
				    lower_32_bits(addr));
		/* [한국어] 상위 32비트를 EQ_PAGE_UPPER 에. 두 레지스터로 나누는 것은 64비트 주소를
		 * 32비트 레지스터 쌍에 싣는 흔한 방식이다. */
		iproc_msi_write_reg(msi, IPROC_MSI_EQ_PAGE_UPPER, i,
				    upper_32_bits(addr));
	}

	/* Program address region for MSI posted writes */
	/* [한국어] MSI 포스티드 라이트가 향할 주소 영역을 알린다. */
	for (i = 0; i < msi->nr_msi_region; i++) {
		/* [한국어] i 번째 영역의 물리 주소. */
		phys_addr_t addr = msi->msi_addr + (i * MSI_MEM_REGION_SIZE);

		/* [한국어] 하위 32비트. */
		iproc_msi_write_reg(msi, IPROC_MSI_PAGE, i,
				    lower_32_bits(addr));
		/* [한국어] 상위 32비트. */
		iproc_msi_write_reg(msi, IPROC_MSI_PAGE_UPPER, i,
				    upper_32_bits(addr));
	}

	/* [한국어] 그룹(=큐)마다 제어 레지스터를 설정한다. */
	for (eq = 0; eq < msi->nr_irqs; eq++) {
		/* Enable MSI event queue */
		/* [한국어] 세 비트를 한 번에 세운다 — 인터럽트 허용, N개 이벤트 시 통지, 큐 활성화. */
		val = IPROC_MSI_INTR_EN | IPROC_MSI_INT_N_EVENT |
			IPROC_MSI_EQ_EN;
		/* [한국어] CTRL 에 쓴다. 이 쓰기로 해당 큐가 살아난다. */
		iproc_msi_write_reg(msi, IPROC_MSI_CTRL, eq, val);

		/*
		 * Some legacy platforms require the MSI interrupt enable
		 * register to be set explicitly.
		 */
		/* [한국어] 구형 플랫폼만 별도 허용 레지스터를 갖는다. */
		if (msi->has_inten_reg) {
			/* [한국어] 현재 값을 읽고, */
			val = iproc_msi_read_reg(msi, IPROC_MSI_INTS_EN, eq);
			/* [한국어] 이 그룹의 비트만 세워, */
			val |= BIT(eq);
			/* [한국어] 되쓴다. 다른 그룹의 설정을 지우지 않기 위한 읽기-수정-쓰기다. */
			iproc_msi_write_reg(msi, IPROC_MSI_INTS_EN, eq, val);
		}
	}
}

/* [한국어]
 * iproc_msi_disable - 모든 이벤트 큐를 끈다
 *
 * @msi: MSI 컨트롤러.
 *
 * enable 과 대칭이되 순서가 반대다. 별도 허용 레지스터를 먼저 끄고 CTRL 을 나중에
 * 끄는데, 이는 "바깥쪽 관문부터 닫는다"는 자연스러운 순서다.
 *
 * 동작 과정: 그룹마다 (1) has_inten_reg 면 별도 허용 레지스터에서 이 그룹의
 * 비트를 지우고, (2) CTRL 에서 세 활성화 비트를 모두 지운다. 둘 다 읽기-수정-쓰기라
 * 다른 그룹의 설정을 건드리지 않는다.
 *
 * 이 함수가 돌아온 뒤에는 하드웨어가 더 이상 인터럽트를 올리지 않으므로,
 * 호출자가 안심하고 체인 핸들러를 떼고 큐 메모리를 반납할 수 있다.
 * iproc_msi_exit() 이 이 함수를 가장 먼저 부르는 이유가 그것이다.
 *
 * [상류 코드 관찰, 수정하지 않음] msi->reg_offsets 가 NULL 인지 확인하지 않는다.
 * iproc_msi_init() 이 nr_irqs 를 정한 뒤 reg_offsets 를 정하기 전에 실패해 돌아가면
 * (nr_irqs < nr_cpus 로 -EINVAL 을 내는 경로, 또는 switch 의 default 갈래),
 * pcie->msi 는 여전히 그 반쯤 초기화된 구조체를 가리킨다. 그 뒤 iproc_pcie_remove()
 * 가 iproc_msi_exit() 을 조건 없이 부르면 여기서 NULL 을 역참조하게 된다.
 * 실패 경로 가운데 pcie->msi 를 NULL 로 되돌리는 곳은 free_irqs 라벨뿐이다.
 *
 * 실행 컨텍스트: remove 경로 또는 init 실패 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   iproc_msi_exit() → [iproc_msi_disable] → iproc_msi_read_reg() / iproc_msi_write_reg()
 */
static void iproc_msi_disable(struct iproc_msi *msi)
{
	/* [한국어] 큐 번호와 임시 값. */
	u32 eq, val;

	/* [한국어] 모든 그룹을 순회하며 끈다. */
	for (eq = 0; eq < msi->nr_irqs; eq++) {
		/* [한국어] 별도 허용 레지스터가 있으면 먼저 그 비트를 지운다. */
		if (msi->has_inten_reg) {
			/* [한국어] 현재 값을 읽고, */
			val = iproc_msi_read_reg(msi, IPROC_MSI_INTS_EN, eq);
			/* [한국어] 이 그룹의 비트를 지우고, */
			val &= ~BIT(eq);
			/* [한국어] 되쓴다. */
			iproc_msi_write_reg(msi, IPROC_MSI_INTS_EN, eq, val);
		}

		/* [한국어] CTRL 을 읽어, */
		val = iproc_msi_read_reg(msi, IPROC_MSI_CTRL, eq);
		/* [한국어] 세 활성화 비트를 모두 지우고, */
		val &= ~(IPROC_MSI_INTR_EN | IPROC_MSI_INT_N_EVENT |
			 IPROC_MSI_EQ_EN);
		/* [한국어] 되쓴다. enable 과 정확히 대칭이며, 순서도 반대(허용 레지스터 먼저, CTRL 나중)다.
		 * [상류 코드 관찰, 수정하지 않음] 이 함수는 msi->reg_offsets 가 NULL 인지 확인하지
		 * 않는다. iproc_msi_init() 이 nr_irqs 를 정한 뒤 reg_offsets 를 정하기 전에
		 * 실패해 돌아가면(예: nr_irqs < nr_cpus 로 -EINVAL), pcie->msi 는 여전히
		 * 이 반쯤 초기화된 구조체를 가리킨다. 그 상태에서 iproc_pcie_remove() 가
		 * iproc_msi_exit() 를 부르면 여기서 NULL 을 역참조하게 된다.
		 * 실패 경로 중 free_irqs 라벨만 pcie->msi 를 NULL 로 되돌린다. */
		iproc_msi_write_reg(msi, IPROC_MSI_CTRL, eq, val);
	}
}

/* [한국어]
 * iproc_msi_alloc_domains - MSI 부모 IRQ 도메인을 만든다
 *
 * @node: 이 MSI 컨트롤러의 DT 노드. 도메인을 식별할 fwnode 의 출처다.
 * @msi: MSI 컨트롤러. 도메인의 host_data 로 심어져 콜백들이 되찾는다.
 * @return: 0 = 성공, -ENOMEM = 도메인 생성 실패.
 *
 * 왜 필요한가: 리눅스에서 MSI 는 계층형 IRQ 도메인으로 표현된다. 장치가 MSI 를
 * 요청하면 상위 도메인이 아래로 내려오며 각 계층이 자기 몫을 할당하는데,
 * 이 함수가 그 계층 중 하나를 등록한다. 등록이 끝나는 순간부터
 * iproc_msi_irq_domain_alloc() 이 불릴 수 있다.
 *
 * 동작 과정: irq_domain_info 에 네 가지를 채워 넘긴다 —
 * DT 노드에서 만든 fwnode(식별자), alloc/free 구현(msi_domain_ops),
 * 콜백이 되찾을 host_data(= msi), 그리고 이 도메인이 다룰 hwirq 개수(nr_msi_vecs).
 * msi_create_parent_irq_domain() 이 그 정보와 iproc_msi_parent_ops 를 묶어
 * PCI MSI 용 부모 도메인을 만든다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 실패 시 -ENOMEM. 호출자는 free_eq_dma 라벨로 되감는다.
 *
 * 호출 체인:
 *   iproc_msi_init() → [iproc_msi_alloc_domains] → msi_create_parent_irq_domain()
 */
static int iproc_msi_alloc_domains(struct device_node *node,
				   struct iproc_msi *msi)
{
	/* [한국어] 도메인 생성 정보. fwnode 는 DT 노드에서 만든 식별자, ops 는 alloc/free 구현,
	 * host_data 는 콜백들이 되찾을 컨트롤러, size 는 이 도메인이 다룰 hwirq 개수다. */
	struct irq_domain_info info = {
		.fwnode		= of_fwnode_handle(node),
		.ops		= &msi_domain_ops,
		.host_data	= msi,
		.size		= msi->nr_msi_vecs,
	};

	/* [한국어] MSI 부모 도메인을 만든다. 이 시점부터 하위 PCI 장치의 MSI 요청이
	 * 위에서 정의한 alloc/free 콜백으로 들어온다. */
	msi->inner_domain = msi_create_parent_irq_domain(&info, &iproc_msi_parent_ops);
	/* [한국어] 생성 실패. */
	if (!msi->inner_domain)
		/* [한국어] -ENOMEM 으로 알린다. */
		return -ENOMEM;

	/* [한국어] 도메인 생성 성공. */
	return 0;
}

/* [한국어]
 * iproc_msi_free_domains - IRQ 도메인을 제거한다
 *
 * @msi: MSI 컨트롤러.
 *
 * alloc 과 대칭이다. inner_domain 이 NULL 인지 먼저 확인하는 것은,
 * 이 함수가 init 의 실패 경로와 exit 경로 양쪽에서 불릴 수 있어
 * 도메인이 아직 만들어지지 않은 상태로 들어올 수 있기 때문이다.
 *
 * 제거 후 inner_domain 을 NULL 로 되돌리지는 않는다. 두 호출부 모두
 * 이 함수를 한 번씩만 부르므로 이중 해제가 일어나지 않는다는 전제다.
 *
 * 실행 컨텍스트: probe 실패 경로 또는 remove 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   iproc_msi_init()(실패 시) / iproc_msi_exit() → [iproc_msi_free_domains]
 *     → irq_domain_remove()
 */
static void iproc_msi_free_domains(struct iproc_msi *msi)
{
	/* [한국어] 이미 제거했거나 아직 만들지 않았을 수 있으므로 확인한다. */
	if (msi->inner_domain)
		/* [한국어] 도메인을 제거한다. 여기서 NULL 로 되돌리지 않는 점에 주의 —
		 * 이 함수는 init 실패 경로와 exit 경로에서 각각 한 번씩만 불린다. */
		irq_domain_remove(msi->inner_domain);
}

/* [한국어]
 * iproc_msi_irq_free - 한 CPU 몫의 GIC 인터럽트에서 체인 핸들러를 뗀다
 *
 * @msi: MSI 컨트롤러.
 * @cpu: 대상 CPU 번호.
 *
 * 그룹 i 는 CPU (i % nr_cpus) 가 담당하므로, cpu 에서 시작해 nr_cpus 씩 건너뛰면
 * 그 CPU 의 그룹만 정확히 골라낼 수 있다. 이 간격 순회가 이 파일에서 CPU 와 그룹을
 * 연결하는 방식이며, setup 쪽도 똑같은 순회를 쓴다.
 *
 * irq_set_chained_handler_and_data() 에 NULL 을 두 번 넘기면 등록이 해제된다.
 * 이 호출 이후 그 GIC IRQ 가 올라와도 이 파일의 핸들러는 불리지 않는다.
 *
 * 실행 컨텍스트: probe 실패 경로 또는 remove 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 아직 등록되지 않은 IRQ 에 대해 불려도 안전하다 —
 * setup 이 부분적으로 실패했을 때 이 함수가 무조건 전체를 훑는 것이 그 전제다.
 *
 * 호출 체인:
 *   iproc_msi_irq_setup()(실패 시) / iproc_msi_init()(실패 시) / iproc_msi_exit()
 *     → [iproc_msi_irq_free] → irq_set_chained_handler_and_data(NULL, NULL)
 */
static void iproc_msi_irq_free(struct iproc_msi *msi, unsigned int cpu)
{
	/* [한국어] 순회 인덱스. */
	int i;

	/* [한국어] 이 CPU 가 담당하는 그룹만 골라 순회한다. 그룹 i 는 CPU (i % nr_cpus) 가 맡으므로,
	 * cpu 에서 시작해 nr_cpus 씩 건너뛰면 그 CPU 의 그룹만 나온다. */
	for (i = cpu; i < msi->nr_irqs; i += msi->nr_cpus) {
		/* [한국어] 체인 핸들러와 데이터를 모두 NULL 로 되돌려 등록을 해제한다.
		 * 이 호출 이후 그 GIC IRQ 가 와도 이 파일의 핸들러는 불리지 않는다. */
		irq_set_chained_handler_and_data(msi->grps[i].gic_irq,
						 NULL, NULL);
	}
}

/* [한국어]
 * iproc_msi_irq_setup - 한 CPU 몫의 GIC 인터럽트에 체인 핸들러를 걸고 CPU 에 고정한다
 *
 * @msi: MSI 컨트롤러.
 * @cpu: 이 그룹들을 담당할 CPU 번호.
 * @return: 0 = 성공. 음수 = 어피니티 설정 실패 또는 CPU 마스크 할당 실패.
 *
 * 왜 필요한가: 이 드라이버의 어피니티 모델은 "각 GIC IRQ 가 특정 CPU 에 고정되어
 * 있다"는 전제 위에 세워져 있다. 벡터를 다른 큐로 옮기는 것만으로 목적지 CPU 가
 * 바뀌려면, 그 고정이 반드시 성립해야 한다. 이 함수가 그 고정을 만든다.
 *
 * 동작 과정: 이 CPU 가 담당할 그룹들을 nr_cpus 간격으로 순회하며,
 *   1) 그 GIC IRQ 에 iproc_msi_handler 와 그룹 포인터를 체인 핸들러로 등록한다.
 *      이 등록으로 GIC IRQ 하나가 최대 64개 MSI 의 통로가 된다.
 *   2) CPU 마스크를 할당해 이 CPU 하나만 켠 뒤 irq_set_affinity() 로 고정한다.
 *      cpumask_var_t 는 CPU 수가 많은 커널에서 동적 할당되는 타입이라
 *      alloc/free 짝이 필요하다.
 *   3) 어느 단계든 실패하면 이 CPU 몫으로 지금까지 건 핸들러를 모두 해제하고
 *      오류를 돌려준다 — 부분 등록 상태를 남기지 않는다.
 *
 * [상류 코드 관찰, 수정하지 않음] CPU 마스크 할당 실패에 -EINVAL 을 쓴다.
 * 메모리 부족이므로 -ENOMEM 이 의미상 더 맞지만 상류 그대로 둔다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트. alloc_cpumask_var(GFP_KERNEL) 이
 * 잠들 수 있다.
 *
 * 에러 경로: 위 3)의 정리를 거친 뒤 오류를 전달한다. 호출자는 다시
 * free_msi_irq 라벨에서 모든 CPU 에 대해 정리를 반복하므로 이중 해제가 되지만,
 * irq_set_chained_handler_and_data(NULL, NULL) 은 여러 번 불려도 안전하다.
 *
 * 호출 체인:
 *   iproc_msi_init() → [iproc_msi_irq_setup]
 *     → irq_set_chained_handler_and_data() / alloc_cpumask_var()
 *     → irq_set_affinity() / free_cpumask_var()
 */
static int iproc_msi_irq_setup(struct iproc_msi *msi, unsigned int cpu)
{
	/* [한국어] 순회 인덱스와 반환값. */
	int i, ret;
	/* [한국어] 어피니티 설정에 쓸 CPU 마스크. cpumask_var_t 는 CPU 수가 많은 시스템에서
	 * 동적 할당이 되는 타입이라 alloc/free 짝이 필요하다. */
	cpumask_var_t mask;
	/* [한국어] 로그에 쓸 컨트롤러. */
	struct iproc_pcie *pcie = msi->pcie;

	/* [한국어] 이 CPU 가 담당할 그룹만 순회한다. */
	for (i = cpu; i < msi->nr_irqs; i += msi->nr_cpus) {
		/* [한국어] 그 GIC IRQ 에 이 파일의 체인 핸들러와 그룹 포인터를 건다.
		 * 이 등록으로 GIC IRQ 하나가 최대 64개의 MSI 벡터를 분배하는 통로가 된다. */
		irq_set_chained_handler_and_data(msi->grps[i].gic_irq,
						 iproc_msi_handler,
						 &msi->grps[i]);
		/* Dedicate GIC interrupt to each CPU core */
		/* [한국어] 마스크 할당 성공 여부를 먼저 확인한다. */
		if (alloc_cpumask_var(&mask, GFP_KERNEL)) {
			/* [한국어] 마스크를 비우고, */
			cpumask_clear(mask);
			/* [한국어] 이 CPU 만 켠다. */
			cpumask_set_cpu(cpu, mask);
			/* [한국어] 그 GIC IRQ 를 이 CPU 에만 배정한다. 이렇게 그룹을 CPU 에 고정해 두어야,
			 * hwirq 사본을 바꾸는 것만으로 목적지 CPU 를 바꾸는 어피니티 구현이 성립한다. */
			ret = irq_set_affinity(msi->grps[i].gic_irq, mask);
			/* [한국어] 실패해도 치명적이지는 않지만 로그로 남긴다. */
			if (ret)
				dev_err(pcie->dev,
					"failed to set affinity for IRQ%d\n",
					msi->grps[i].gic_irq);
			/* [한국어] 마스크는 곧바로 반납한다. */
			free_cpumask_var(mask);
		} else {
			/* [한국어] 마스크 할당 자체가 실패한 경우. */
			dev_err(pcie->dev, "failed to alloc CPU mask\n");
			/* [한국어] 메모리 부족 로그. */
			ret = -EINVAL;
		/* [한국어] [상류 코드 관찰] 이 경우 -EINVAL 을 쓴다. 메모리 부족이므로 -ENOMEM 이
		 * 의미상 더 맞지만 상류 그대로 둔다. */
		}

		if (ret) {
			/* [한국어] 어느 쪽이든 실패면 정리하고 중단한다. */
			/* Free all configured/unconfigured IRQs */
			iproc_msi_irq_free(msi, cpu);
			/* [한국어] 이 CPU 몫으로 지금까지 건 핸들러를 모두 해제한다. 부분 등록 상태를 남기지 않는다. */
			return ret;
		/* [한국어] 오류 전달. */
		}
	}

	return 0;
/* [한국어] 이 CPU 의 모든 그룹을 성공적으로 설정했다. */
}

/* [한국어]
 * iproc_msi_init - iProc 이벤트 큐 기반 MSI 컨트롤러를 초기화한다
 *
 * @pcie: 이 MSI 컨트롤러를 소유할 iProc PCIe 컨트롤러. 여기서 dev(로그·devm 기준),
 *       base(레지스터 창), base_addr(MSI 목적지 기준 주소), type(SoC 변종)을 가져오고,
 *       완성된 struct iproc_msi 를 pcie->msi 에 심는다.
 * @node: DT 의 MSI 컨트롤러 노드. compatible / msi-controller / interrupts /
 *       brcm,pcie-msi-inten 속성을 읽는다.
 * @return: 0 = 성공. -ENODEV = 이 드라이버가 다룰 노드가 아니거나 GIC 인터럽트가 없음.
 *       -EBUSY = 이미 초기화됨. -EINVAL = CPU 수보다 그룹이 적거나 지원하지 않는 SoC 변종.
 *       -ENOMEM = 각종 할당 실패.
 *       호출자 iproc_pcie_msi_enable() 은 실패를 치명적으로 보지 않는다 —
 *       다른 MSI 컨트롤러(GIC 내장 ITS 등)를 쓰는 시스템일 수 있기 때문이다.
 *
 * 왜 필요한가: 이 파일의 진입점이다. GIC 에 MSI 지원이 통합되지 않은 iProc SoC 에서
 * PCIe MSI 를 쓰려면 이 이벤트 큐 기반 컨트롤러가 필요하다.
 *
 * 동작 과정:
 *   1) DT 노드가 이 드라이버 것인지 세 가지로 확인한다 — compatible 문자열,
 *      msi-controller 속성, 그리고 중복 초기화 여부.
 *   2) 컨트롤러 객체를 devm 으로 할당하고 양방향 연결(msi->pcie, pcie->msi)을 만든다.
 *   3) 그룹 수를 정한다. DT 의 인터럽트 개수에서 시작해 NR_HW_IRQS 로 자르고,
 *      CPU 수보다 적으면 거절하고, CPU 수의 배수가 되도록 다시 줄인다.
 *      이 세 조정이 모두 어피니티 모델(벡터를 CPU 수만큼 복제)을 성립시키기 위한 것이다.
 *   4) SoC 변종에 따라 레지스터 오프셋 테이블과 영역 개수를 정한다.
 *   5) 비트맵과 그룹 배열을 할당하고, DT 인터럽트를 virq 로 매핑해 그룹을 채운다.
 *   6) 이벤트 큐 메모리를 coherent DMA 로 잡는다(0 으로 초기화된 채로 온다).
 *   7) IRQ 도메인을 만들고, 온라인 CPU 마다 체인 핸들러를 걸고 어피니티를 고정한다.
 *   8) 마지막으로 하드웨어를 켠다.
 *
 * [상류 코드 관찰, 수정하지 않음] pcie->msi 는 2)에서 일찍 설정되지만, 3)과 4)의
 * 이른 return 들은 그것을 NULL 로 되돌리지 않는다. 되돌리는 곳은 free_irqs 라벨뿐이다.
 * 호출자 iproc_pcie_setup() 은 실패를 무시하고 진행하고, 나중에 iproc_pcie_remove() 가
 * 조건 없이 iproc_msi_exit() 을 부르므로, 반쯤 초기화된 구조체가 iproc_msi_disable() 에
 * 들어가 msi->reg_offsets(NULL)를 역참조할 수 있다.
 * 또 하나: nr_cpus == 1 일 때 정적 전역 iproc_msi_parent_ops 의 supported_flags 를
 * 직접 수정한다. 인스턴스별 설정이 아니므로 이 컨트롤러가 여럿인 시스템에서는
 * 먼저 초기화된 쪽의 판단이 전체에 적용된다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트. DMA 할당과 IRQ 매핑이 잠들 수 있다.
 *
 * 에러 경로: 세 개의 되감기 라벨(free_msi_irq → free_eq_dma → free_irqs)이 계단식으로
 * 이어져 잡은 것만 정확히 되돌린다. devm 자원(msi 구조체, bitmap, grps)은
 * 드라이버 코어가 처리한다.
 *
 * 호출 체인:
 *   iproc_pcie_setup() → iproc_pcie_msi_enable()(pcie-iproc.c:1339)
 *     → [iproc_msi_init]
 *     → iproc_msi_alloc_domains() → iproc_msi_irq_setup() → iproc_msi_enable()
 */
int iproc_msi_init(struct iproc_pcie *pcie, struct device_node *node)
{
	/* [한국어] 할당할 컨트롤러 객체. */
	struct iproc_msi *msi;
	/* [한국어] 순회 인덱스와 반환값. */
	int i, ret;
	/* [한국어] 온라인 CPU 순회 변수. */
	unsigned int cpu;

	/* [한국어] DT 노드가 이 드라이버가 다루는 MSI 컨트롤러인지 확인한다. */
	if (!of_device_is_compatible(node, "brcm,iproc-msi"))
		/* [한국어] 아니면 다른 MSI 컨트롤러(예: GIC 내장 ITS)를 쓰는 시스템이므로 물러난다.
		 * 호출자 iproc_pcie_msi_enable() 의 상류 주석이 "실패해도 괜찮다"고 적어 둔 이유다. */
		return -ENODEV;

	/* [한국어] msi-controller 속성이 있어야 실제로 MSI 컨트롤러로 동작한다. */
	if (!of_property_read_bool(node, "msi-controller"))
		/* [한국어] 없으면 이 노드는 대상이 아니다. */
		return -ENODEV;

	/* [한국어] 이미 초기화된 적이 있으면 중복 초기화를 막는다. */
	if (pcie->msi)
		/* [한국어] -EBUSY 로 알린다. */
		return -EBUSY;

	/* [한국어] 컨트롤러 객체를 0 초기화 할당한다. devm 이라 자동 해제된다. */
	msi = devm_kzalloc(pcie->dev, sizeof(*msi), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!msi)
		/* [한국어] -ENOMEM 전달. */
		return -ENOMEM;

	/* [한국어] 양방향 연결을 만든다 — 아래 pcie->msi 대입과 짝이다. */
	msi->pcie = pcie;
	/* [한국어] 이 시점부터 pcie->msi 가 NULL 이 아니게 된다. 아래 실패 경로들이 이 값을
	 * 되돌리지 않는 것이 위에서 지적한 관찰의 원인이다. */
	pcie->msi = msi;
	/* [한국어] MSI 포스티드 라이트의 목적지 기준 주소로 컨트롤러 레지스터 창의 물리 주소를 쓴다. */
	msi->msi_addr = pcie->base_addr;
	/* [한국어] 비트맵 뮤텍스 초기화. */
	mutex_init(&msi->bitmap_lock);
	/* [한국어] 가능한 전체 CPU 수를 얻는다. 온라인 수가 아니라 possible 인 것은
	 * 나중에 CPU 가 올라와도 벡터 배치가 흔들리지 않게 하기 위함이다. */
	msi->nr_cpus = num_possible_cpus();

	/* [한국어] 단일 CPU 시스템에서는 어피니티를 위한 벡터 복제가 필요 없다. */
	if (msi->nr_cpus == 1)
		/* [한국어] 그래서 다중 MSI 를 지원 목록에 추가한다. 이 대입이 정적 전역 구조체를 바꾼다는
		 * 점에 주의해야 한다 — 인스턴스별 설정이 아니다. */
		iproc_msi_parent_ops.supported_flags |= MSI_FLAG_MULTI_PCI_MSI;

	/* [한국어] DT 의 interrupts 속성에서 GIC IRQ 개수를 센다. */
	msi->nr_irqs = of_irq_count(node);
	/* [한국어] 하나도 없으면 MSI 를 받을 통로가 없다. */
	if (!msi->nr_irqs) {
		/* [한국어] 오류 로그. */
		dev_err(pcie->dev, "found no MSI GIC interrupt\n");
		/* [한국어] -ENODEV. 이 시점에는 nr_irqs 가 0 이라 이후 disable 루프가 돌지 않는다. */
		return -ENODEV;
	}

	/* [한국어] 하드웨어가 지원하는 그룹 수를 넘으면, */
	if (msi->nr_irqs > NR_HW_IRQS) {
		/* [한국어] 경고만 남기고, */
		dev_warn(pcie->dev, "too many MSI GIC interrupts defined %d\n",
			 msi->nr_irqs);
		/* [한국어] 최대값으로 자른다. DT 가 과하게 서술한 경우를 관용적으로 처리한다. */
		msi->nr_irqs = NR_HW_IRQS;
	}

	/* [한국어] 어피니티를 구현하려면 CPU 마다 최소 하나의 그룹이 필요하다. */
	if (msi->nr_irqs < msi->nr_cpus) {
		/* [한국어] 부족하면 진행할 수 없다. */
		dev_err(pcie->dev,
			"not enough GIC interrupts for MSI affinity\n");
		/* [한국어] -EINVAL. [상류 코드 관찰] 이 경로에서 nr_irqs 는 0 이 아니고 reg_offsets 는
		 * 아직 NULL 이며 pcie->msi 는 이 객체를 가리킨 상태로 남는다. */
		return -EINVAL;
	}

	/* [한국어] 그룹 수가 CPU 수의 배수가 아니면 사본 계산이 어긋난다. */
	if (msi->nr_irqs % msi->nr_cpus != 0) {
		/* [한국어] 나머지를 잘라 배수로 맞춘다. */
		msi->nr_irqs -= msi->nr_irqs % msi->nr_cpus;
		/* [한국어] 몇 개로 줄였는지 알린다. */
		dev_warn(pcie->dev, "Reducing number of interrupts to %d\n",
			 msi->nr_irqs);
	}

	/* [한국어] SoC 변종에 따라 레지스터 테이블과 영역 개수를 정한다. */
	switch (pcie->type) {
	/* [한국어] BCMA 결합형 PAXB. */
	case IPROC_PCIE_PAXB_BCMA:
	/* [한국어] 플랫폼 결합형 PAXB. 두 변종이 같은 레지스터 배치를 쓴다. */
	case IPROC_PCIE_PAXB:
		/* [한국어] PAXB 용 테이블. */
		msi->reg_offsets = iproc_msi_reg_paxb;
		/* [한국어] 큐 메모리를 그룹들이 공유하므로 영역 하나면 된다. */
		msi->nr_eq_region = 1;
		/* [한국어] MSI 주소 영역도 하나. */
		msi->nr_msi_region = 1;
		break;
	/* [한국어] PAXC 변종. */
	case IPROC_PCIE_PAXC:
		/* [한국어] PAXC 용 테이블. */
		msi->reg_offsets = iproc_msi_reg_paxc;
		/* [한국어] PAXC 는 그룹마다 큐 영역이 따로 필요하다. */
		msi->nr_eq_region = msi->nr_irqs;
		/* [한국어] MSI 주소 영역도 그룹 수만큼. */
		msi->nr_msi_region = msi->nr_irqs;
		break;
	/* [한국어] 그 밖의 변종은 이 MSI 컨트롤러와 맞지 않는다. */
	default:
		/* [한국어] 오류 로그. */
		dev_err(pcie->dev, "incompatible iProc PCIe interface\n");
		/* [한국어] -EINVAL. 이 경로도 reg_offsets 가 NULL 인 채 돌아간다. */
		return -EINVAL;
	}

	/* [한국어] 구형 플랫폼용 별도 허용 레지스터가 필요한지 DT 속성으로 판별한다. */
	msi->has_inten_reg = of_property_read_bool(node, "brcm,pcie-msi-inten");

	/* [한국어] 그룹마다 큐 하나, 큐마다 64 항목이므로 총 벡터 수는 그 곱이다. */
	msi->nr_msi_vecs = msi->nr_irqs * EQ_LEN;
	/* [한국어] 벡터 사용 여부 비트맵을 0 으로 할당한다. */
	msi->bitmap = devm_bitmap_zalloc(pcie->dev, msi->nr_msi_vecs,
					 GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!msi->bitmap)
		/* [한국어] -ENOMEM. */
		return -ENOMEM;

	/* [한국어] 그룹 배열을 할당한다. kcalloc 이라 0 으로 초기화되어,
	 * 아래 오류 정리에서 gic_irq 가 0 인 항목을 건너뛸 수 있다. */
	msi->grps = devm_kcalloc(pcie->dev, msi->nr_irqs, sizeof(*msi->grps),
				 GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!msi->grps)
		/* [한국어] -ENOMEM. */
		return -ENOMEM;

	/* [한국어] 그룹마다 GIC IRQ 를 매핑하고 역포인터를 채운다. */
	for (i = 0; i < msi->nr_irqs; i++) {
		/* [한국어] DT 의 i 번째 인터럽트를 리눅스 virq 로 매핑한다. */
		unsigned int irq = irq_of_parse_and_map(node, i);

		/* [한국어] 0 은 매핑 실패를 뜻한다. */
		if (!irq) {
			/* [한국어] 오류 로그. */
			dev_err(pcie->dev, "unable to parse/map interrupt\n");
			/* [한국어] -ENODEV 로 기록하고, */
			ret = -ENODEV;
			/* [한국어] 이미 매핑한 것들을 되돌리는 정리 구간으로. */
			goto free_irqs;
		}
		/* [한국어] 매핑된 virq 를 기록한다. */
		msi->grps[i].gic_irq = irq;
		/* [한국어] 핸들러가 컨트롤러로 거슬러 올라갈 역포인터. */
		msi->grps[i].msi = msi;
		/* [한국어] 그룹 i 는 큐 i 를 쓴다. */
		msi->grps[i].eq = i;
	}

	/* Reserve memory for event queue and make sure memories are zeroed */
	/* [한국어] 이벤트 큐 메모리를 coherent DMA 로 잡는다. coherent 인 이유는 하드웨어가 쓴 값을
	 * 소프트웨어가 캐시 무효화 없이 곧바로 읽어야 하기 때문이다.
	 * 위 영어 주석대로 dma_alloc_coherent 는 0 으로 초기화된 메모리를 준다. */
	msi->eq_cpu = dma_alloc_coherent(pcie->dev,
					 msi->nr_eq_region * EQ_MEM_REGION_SIZE,
					 &msi->eq_dma, GFP_KERNEL);
	/* [한국어] 할당 실패. */
	if (!msi->eq_cpu) {
		/* [한국어] -ENOMEM 기록. */
		ret = -ENOMEM;
		/* [한국어] IRQ 매핑 정리 구간으로. */
		goto free_irqs;
	}

	/* [한국어] IRQ 도메인을 만든다. 이제부터 MSI 요청을 받을 수 있다. */
	ret = iproc_msi_alloc_domains(node, msi);
	/* [한국어] 실패 검사. */
	if (ret) {
		/* [한국어] 오류 로그. */
		dev_err(pcie->dev, "failed to create MSI domains\n");
		/* [한국어] DMA 메모리 해제 구간으로. */
		goto free_eq_dma;
	}

	/* [한국어] 온라인 CPU 마다 그 CPU 몫의 그룹에 체인 핸들러를 걸고 어피니티를 고정한다. */
	for_each_online_cpu(cpu) {
		ret = iproc_msi_irq_setup(msi, cpu);
		/* [한국어] 한 CPU 라도 실패하면, */
		if (ret)
			/* [한국어] 이미 건 것을 모두 해제하는 구간으로. */
			goto free_msi_irq;
	}

	/* [한국어] 모든 준비가 끝났으니 하드웨어를 켠다. 큐 주소·MSI 주소 프로그래밍과
	 * CTRL 비트 설정이 여기서 일어난다. */
	iproc_msi_enable(msi);

	/* [한국어] 초기화 성공. */
	return 0;

/* [한국어] iproc_msi_irq_setup 실패 전용 라벨. */
free_msi_irq:
	/* [한국어] 모든 온라인 CPU 에 대해, */
	for_each_online_cpu(cpu)
		/* [한국어] 건 핸들러를 해제한다. 실패한 CPU 이전 것들까지 확실히 되돌린다. */
		iproc_msi_irq_free(msi, cpu);
	/* [한국어] 도메인도 제거한다. */
	iproc_msi_free_domains(msi);

/* [한국어] 도메인 생성 실패 전용 라벨. */
free_eq_dma:
	/* [한국어] 큐 메모리를 반납한다. */
	dma_free_coherent(pcie->dev, msi->nr_eq_region * EQ_MEM_REGION_SIZE,
			  msi->eq_cpu, msi->eq_dma);

/* [한국어] IRQ 매핑 실패와 DMA 할당 실패가 공유하는 라벨. */
free_irqs:
	/* [한국어] 모든 그룹을 순회하며, */
	for (i = 0; i < msi->nr_irqs; i++) {
		/* [한국어] 매핑된 것만 골라, */
		if (msi->grps[i].gic_irq)
			/* [한국어] virq 매핑을 해제한다. 0 인 항목은 아직 매핑하지 않은 것이라 건너뛴다. */
			irq_dispose_mapping(msi->grps[i].gic_irq);
	}
	/* [한국어] 여기서만 pcie->msi 를 NULL 로 되돌린다. 위쪽의 이른 return 들은 이 정리를
	 * 거치지 않는다는 점이 앞서 지적한 관찰의 핵심이다. */
	pcie->msi = NULL;
	/* [한국어] 기록해 둔 오류를 전달한다. */
	return ret;
}
/* [한국어] 공용 코어(pcie-iproc.c)가 모듈 경계를 넘어 호출하므로 export 한다. */
EXPORT_SYMBOL(iproc_msi_init);

/* [한국어]
 * iproc_msi_exit - MSI 컨트롤러를 끄고 자원을 반납한다
 *
 * @pcie: 이 MSI 컨트롤러를 소유한 iProc PCIe 컨트롤러.
 *
 * 왜 순서가 중요한가: 하드웨어를 먼저 끄고(더 이상 인터럽트가 올라오지 않게 한 뒤)
 * 핸들러를 떼고, 도메인을 제거하고, 마지막에 큐 메모리를 반납한다. 이 순서를
 * 뒤집으면 이미 해제된 메모리나 핸들러를 인터럽트가 건드릴 수 있다.
 *
 * 동작 과정:
 *   1) pcie->msi 가 NULL 이면 이 컨트롤러를 쓰지 않는 시스템이므로 조용히 돌아간다.
 *      호출자가 조건 없이 부르기 때문에 이 검사가 반드시 필요하다.
 *   2) iproc_msi_disable() 로 모든 큐를 끈다.
 *   3) 온라인 CPU 마다 체인 핸들러를 뗀다.
 *   4) IRQ 도메인을 제거한다.
 *   5) 큐 메모리를 반납한다. 크기 계산이 할당 때와 정확히 같아야 한다.
 *   6) 매핑된 virq 를 모두 해제한다.
 * devm 이 아닌 자원은 이 셋(큐 메모리, IRQ 도메인, virq 매핑)뿐이고,
 * msi 구조체와 grps 배열, bitmap 은 드라이버 코어가 되돌린다.
 *
 * 실행 컨텍스트: remove 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값이 void 다.
 *
 * 호출 체인:
 *   iproc_pcie_remove() → iproc_pcie_msi_disable()(pcie-iproc.c:1374)
 *     → [iproc_msi_exit] → iproc_msi_disable() / iproc_msi_irq_free()
 *     → iproc_msi_free_domains() / dma_free_coherent() / irq_dispose_mapping()
 */
void iproc_msi_exit(struct iproc_pcie *pcie)
{
	/* [한국어] probe 때 심어 둔 MSI 컨트롤러를 꺼낸다. */
	struct iproc_msi *msi = pcie->msi;
	/* [한국어] 순회 변수 두 개. */
	unsigned int i, cpu;

	/* [한국어] 이 컨트롤러가 iProc MSI 를 쓰지 않으면 할 일이 없다. */
	if (!msi)
		/* [한국어] 조용히 돌아간다. 호출자가 조건 없이 부르기 때문에 이 검사가 필요하다. */
		return;

	/* [한국어] 하드웨어를 먼저 끈다. 핸들러를 떼기 전에 인터럽트 발생을 멈추는 순서가 중요하다. */
	iproc_msi_disable(msi);

	/* [한국어] 모든 온라인 CPU 에 대해, */
	for_each_online_cpu(cpu)
		/* [한국어] 체인 핸들러를 해제한다. */
		iproc_msi_irq_free(msi, cpu);

	/* [한국어] IRQ 도메인을 제거한다. */
	iproc_msi_free_domains(msi);

	/* [한국어] 큐 메모리를 반납한다. 크기 계산이 할당 때와 정확히 같아야 한다. */
	dma_free_coherent(pcie->dev, msi->nr_eq_region * EQ_MEM_REGION_SIZE,
			  msi->eq_cpu, msi->eq_dma);

	/* [한국어] 모든 그룹의, */
	for (i = 0; i < msi->nr_irqs; i++) {
		/* [한국어] 매핑된 virq 를, */
		if (msi->grps[i].gic_irq)
			/* [한국어] 해제한다. devm 이 아닌 자원은 이 셋(큐 메모리, 도메인, virq 매핑)뿐이며
			 * msi 구조체와 grps 배열, bitmap 은 devm 이 되돌린다. */
			irq_dispose_mapping(msi->grps[i].gic_irq);
	}
}
/* [한국어] 공용 코어가 호출할 수 있도록 export. */
EXPORT_SYMBOL(iproc_msi_exit);
