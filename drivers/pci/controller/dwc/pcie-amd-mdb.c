// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for AMD MDB PCIe Bridge
 *
 * Copyright (C) 2024-2025, Advanced Micro Devices, Inc.
 */

/*
 * [한국어 설명] AMD Versal2 MDB PCIe 브리지의 DesignWare 호스트 글루
 * (pcie-amd-mdb.c)
 *
 * === 파일의 역할 ===
 * Synopsys DesignWare PCIe 코어를 AMD MDB(Media/Data Bridge) 브리지에 붙이는
 * 글루 드라이버다. config 접근, ATU 설정, 버스 스캔은
 * pcie-designware-host.c 가 맡는다.
 *
 * **이 파일의 거의 전부가 인터럽트 코드다.** 다른 DWC 글루들이 클럭·PHY·
 * 리셋 순서에 분량을 쓰는 것과 달리, 이 드라이버는 그런 것을 거의 다루지
 * 않는다 — PERST# GPIO 하나뿐이다. 대신 **인터럽트 도메인 두 벌** 을 만들고
 * 그 사이의 이층 분배를 구현하는 데 대부분을 쓴다.
 *
 * 그것을 보여 주는 증거가 dw_pcie_host_ops 표다 — 이 파일의 그 표에는
 * **콜백이 하나도 없다.** 코어에 넘길 SoC 고유 초기화가 없다는 뜻이다.
 *
 * === 인터럽트 이층 구조 (이 파일의 핵심) ===
 * 하드웨어가 주는 인터럽트 선은 **하나** 이고, 상태 레지스터 32비트가
 * 그 안에 무엇이 들어 있는지 알려 준다. 그 32비트를 두 도메인이 나눠 맡는다.
 *
 *   [mdb_domain] 32비트 전부를 담당하는 "이벤트" 도메인.
 *     비트 15    : 완료 시간 초과
 *     비트 16~23 : INTx 넷의 assert/deassert 쌍 (아래 참조)
 *     비트 24~28 : PM/PME 와 오류 메시지들
 *   [intx_domain] 그중 INTx 넷만 따로 맡는 도메인. 장치들이 실제로
 *     인터럽트를 받는 곳이 여기다.
 *
 * 분배가 두 단계로 일어난다.
 *   1) 하드웨어 인터럽트 -> amd_mdb_pcie_event()
 *      상태 레지스터에서 마스크되지 않은 비트를 골라 mdb_domain 으로 보낸다.
 *   2) 그중 비트 16(INTx 묶음)이 서 있으면 -> dw_pcie_rp_intx()
 *      상태 레지스터를 다시 읽어 INTx 넷 중 어느 것인지 가려
 *      intx_domain 으로 보낸다.
 *
 * 그래서 **INTx 용 인터럽트 번호를 intx_domain 이 아니라 mdb_domain 에서
 * 얻는다** — amd_mdb_setup_irq() 의 그 줄이 이층 구조의 이음매다.
 *
 * INTx 비트 배치도 눈여겨볼 만하다. 비트 16~23 여덟 자리에 INTx 넷이
 * 들어가며, 한 INTx 가 두 자리(assert 와 deassert)를 쓴다. 그래서 INTA 의
 * assert 는 절대 비트 16, INTB 는 18, INTC 는 20, INTD 는 22 다.
 * 이 파일의 마스크·언마스크는 **assert 자리만 건드린다.**
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   플랫폼 드라이버 코어 -> amd_mdb_pcie_probe()
 *     -> 자식 노드에서 PERST# GPIO 찾기 (없으면 자기 노드에서 다시)
 *       -> amd_mdb_add_pcie_port()
 *          -> SLCR 레지스터 창 매핑
 *          -> amd_mdb_pcie_init_irq_domains() : 도메인 두 벌
 *          -> amd_mdb_setup_irq() : 포트 초기화, 원인별 핸들러 등록
 *          -> PERST# 해제 (100ms 대기 후, 다시 100ms 대기)
 *          -> dw_pcie_host_init() -> 링크 훈련, 버스 스캔
 *
 * 인터럽트가 올 때: 위 "인터럽트 이층 구조" 참조.
 *
 * 실행 컨텍스트: probe 는 프로세스 컨텍스트, 세 핸들러는 모두
 * IRQF_NO_THREAD 로 등록돼 **하드 인터럽트 문맥** 에서 돈다.
 * 마스크·언마스크는 어느 쪽에서도 불릴 수 있어 raw 스핀락으로 지킨다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어와 커널 irqdomain 계층. 장치들이 intx_domain 에서
 *   인터럽트를 받는다.
 * 아래쪽: pcie-designware-host.c. 접점이 dw_pcie_host_ops 인데 **비어 있고**,
 *   pp->irq 와 pp->lock 만 이 파일이 채운다.
 * 옆쪽: gpiod 계층(PERST#)과 drivers/pci/pci.h 의 대기 상수 둘
 *   (PCIE_T_PVPERL_MS 100ms, PCIE_RESET_CONFIG_WAIT_MS 100ms).
 *
 * 데이터 흐름:
 *   디바이스 트리("slcr" 창, "interrupt-controller" 자식 노드,
 *   "pcie" 로 시작하는 자식 노드의 reset GPIO) -> probe
 *   인터럽트: 상태 레지스터 32비트 -> mdb_domain -> (INTx 면) intx_domain
 *
 * 공유 상태: struct amd_mdb_pcie 하나. 도메인 포인터 둘은 되감기에서
 *   NULL 로 되돌려 두 번 해제되지 않게 한다.
 *
 * === NVMe 관점 ===
 * NVMe 컨트롤러는 MSI/MSI-X 를 쓰므로 이 파일의 INTx 경로를 타지 않는다 —
 * MSI 는 DWC 코어가 따로 처리한다. 다만 이 파일이 등록하는 오류 원인
 * 핸들러들(완료 시간 초과, 정정 가능/불가능 오류)은 NVMe 링크에서 문제가
 * 생겼을 때 커널 로그에 경고를 남기는 유일한 통로다. 상류 주석이 밝히듯
 * 아직 AER 서브시스템에 연결돼 있지 않아 경고 출력이 전부다.
 *
 * === 주요 함수/구조체 요약 ===
 * amd_mdb_pcie_event()          : 1단계 분배. 상태 32비트를 mdb_domain 으로.
 * dw_pcie_rp_intx()             : 2단계 분배. INTx 넷을 가려 intx_domain 으로.
 * amd_mdb_pcie_intr_handler()   : 오류 원인마다 등록되는 핸들러. 경고만 낸다.
 * amd_mdb_pcie_init_irq_domains(): 도메인 두 벌을 만든다.
 * amd_mdb_setup_irq()           : 원인별로 인터럽트를 매핑하고 핸들러를 건다.
 * amd_mdb_pcie_init_port()      : 전부 끄기 -> 대기 지우기 -> 전부 켜기.
 * intr_cause[]                  : 비트 번호로 찾는 원인 이름·설명 표.
 * struct amd_mdb_pcie           : dw_pcie 를 맨 앞에 둔 상태 구조체.
 */

/* [한국어] [상류 코드 관찰] clk_ 로 시작하는 이름을 이 파일에서 찾지 못했다.
 * 이 드라이버는 클럭을 다루지 않는다. */
#include <linux/clk.h>
/* [한국어] mdelay — PERST# 앞뒤의 대기에 쓴다. */
#include <linux/delay.h>
/* [한국어] [상류 코드 관찰] 이것은 **옛 GPIO 번호 방식** 헤더인데, 이 파일은
 * gpiod_ 계열(디스크립터 방식)만 쓴다. 그쪽 선언은 linux/gpio/consumer.h
 * 소관이며, 다른 헤더를 통해 딸려 오는 것으로 보인다. */
#include <linux/gpio.h>
/* [한국어] irqreturn_t 와 IRQF_NO_THREAD — 이 파일의 핸들러 셋이 쓴다. */
#include <linux/interrupt.h>
/* [한국어] irq_domain_create_linear 등. 이 드라이버의 뼈대다. */
#include <linux/irqdomain.h>
/* [한국어] ARRAY_SIZE 등 기본 매크로. */
#include <linux/kernel.h>
/* [한국어] [상류 코드 관찰] __init 계열 표시를 쓰는 곳을 이 파일에서 찾지 못했다. */
#include <linux/init.h>
/* [한국어] [상류 코드 관찰] of_device_get_match_data 등 이 헤더의 이름을 쓰는 곳을
 * 찾지 못했다. 이 드라이버는 매칭 데이터를 쓰지 않는다. */
#include <linux/of_device.h>
/* [한국어] PCI_NUM_INTX — INTx 개수(4)를 여기서 얻는다. */
#include <linux/pci.h>
/* [한국어] platform_device 와 자원 확보. */
#include <linux/platform_device.h>
/* [한국어] [상류 코드 관찰] struct resource 를 쓰는 곳을 이 파일에서 찾지 못했다. */
#include <linux/resource.h>
/* [한국어] u32 등 기본 타입. */
#include <linux/types.h>

/* [한국어] PCI 서브시스템 **내부** 헤더. PCIE_T_PVPERL_MS(100ms)와
 * PCIE_RESET_CONFIG_WAIT_MS(100ms)가 여기 있어 PERST# 앞뒤 대기에 쓴다. */
#include "../../pci.h"
/* [한국어] DWC 코어의 자료구조. **dw_pcie_rp 안의 raw_spinlock_t lock 을
 * 이 파일이 직접 초기화해 쓴다** — 그것이 이 헤더에 대한 가장 특이한
 * 의존이다. */
#include "pcie-designware.h"

/* [한국어] 대기 중인 인터럽트 원인 비트 32개. 읽으면 무엇이 올라왔는지 알 수
 * 있고, 1 을 되쓰면 지워진다(W1C). */
#define AMD_MDB_TLP_IR_STATUS_MISC		0x4C0
/* [한국어] 마스크 레지스터. **1 이면 막혀 있다는 뜻** 이라, 분배 전에 상태에서
 * 이 값을 빼야 한다. */
#define AMD_MDB_TLP_IR_MASK_MISC		0x4C4
/* [한국어] "켜기" 전용 레지스터. 1 을 쓰면 켜지고 0 은 아무 효과가 없다 —
 * 그래서 읽기-수정-쓰기가 필요 없다. */
#define AMD_MDB_TLP_IR_ENABLE_MISC		0x4C8
/* [한국어] "끄기" 전용 레지스터. 위와 짝이며 같은 규약을 따른다.
 * 켜기와 끄기를 나눠 둔 덕에 이 파일의 마스크·언마스크가 단순해진다. */
#define AMD_MDB_TLP_IR_DISABLE_MISC		0x4CC

/* [한국어] INTx 넷이 차지하는 자리 — 비트 23~16 여덟 칸이다. INTx 하나가
 * assert 와 deassert 두 칸을 쓰기 때문에 넷에 여덟 칸이 필요하다. */
#define AMD_MDB_TLP_PCIE_INTX_MASK	GENMASK(23, 16)

/* [한국어] 필드 **안에서** INTx n 의 assert 비트가 몇 번째인지. 2배 하는 것이
 * assert/deassert 쌍 배치를 반영한다 — 0, 2, 4, 6 번째 칸이다.
 * 절대 비트로는 16, 18, 20, 22 가 된다. */
#define AMD_MDB_PCIE_INTR_INTX_ASSERT(x)	BIT((x) * 2)

/* Interrupt registers definitions. */
/* [한국어] 완료 시간 초과. 논-포스티드 요청에 응답이 오지 않은 경우다. */
#define AMD_MDB_PCIE_INTR_CMPL_TIMEOUT		15
/* [한국어] INTx 묶음 비트. **이 파일의 이층 분배가 여기서 갈린다** — 이 비트가
 * 서면 dw_pcie_rp_intx() 가 다시 읽어 넷 중 어느 것인지 가린다.
 * 아래 원인 표에는 일부러 넣지 않아, 설정 루프가 이 비트를 건너뛴다. */
#define AMD_MDB_PCIE_INTR_INTX			16
/* [한국어] 장치가 보낸 PM_PME 메시지를 받았다. */
#define AMD_MDB_PCIE_INTR_PM_PME_RCVD		24
/* [한국어] PME_TO_ACK 메시지를 받았다. L2 진입 핸드셰이크의 응답이다. */
#define AMD_MDB_PCIE_INTR_PME_TO_ACK_RCVD	25
/* [한국어] 정정 가능한 오류 메시지를 받았다. */
#define AMD_MDB_PCIE_INTR_MISC_CORRECTABLE	26
/* [한국어] 치명적이지 않은 오류 메시지를 받았다. */
#define AMD_MDB_PCIE_INTR_NONFATAL		27
/* [한국어] 치명적 오류 메시지를 받았다. */
#define AMD_MDB_PCIE_INTR_FATAL			28

/* [한국어] 원인 이름에서 비트 마스크를 만드는 도우미. 토큰 붙이기(##)로
 * AMD_MDB_PCIE_INTR_ 접두사를 붙여, 아래 목록을 이름만으로 쓸 수 있게 한다. */
#define IMR(x) BIT(AMD_MDB_PCIE_INTR_ ##x)
/* [한국어] 이 드라이버가 관심 갖는 비트 전부. 원인 여섯에 INTx 필드 여덟 칸을
 * 더한 것이다.
 * 줄 끝이 역슬래시로 이어지므로 이 설명을 매크로 위에 블록으로 둔다.
 * init_port 가 이 마스크로 전부 끄고 전부 켜며, 대기 비트를 지울 때도
 * 이 범위 밖은 건드리지 않는다. */
#define AMD_MDB_PCIE_IMR_ALL_MASK			\
	(						\
		IMR(CMPL_TIMEOUT)	|		\
		IMR(PM_PME_RCVD)	|		\
		IMR(PME_TO_ACK_RCVD)	|		\
		IMR(MISC_CORRECTABLE)	|		\
		IMR(NONFATAL)		|		\
		IMR(FATAL)		|		\
		AMD_MDB_TLP_PCIE_INTX_MASK		\
	)

/**
 * struct amd_mdb_pcie - PCIe port information
 * @pci: DesignWare PCIe controller structure
 * @slcr: MDB System Level Control and Status Register (SLCR) base
 * @intx_domain: INTx IRQ domain pointer
 * @mdb_domain: MDB IRQ domain pointer
 * @perst_gpio: GPIO descriptor for PERST# signal handling
 * @intx_irq: INTx IRQ interrupt number
 */
struct amd_mdb_pcie {
	/* [한국어] DesignWare 공통 컨트롤러 구조체를 **값으로** 품는다.
	 * 그래서 to_amd_mdb_pcie() 가 container_of 로 이 글루 구조체를 되찾을 수 있다.
	 * 설정자: amd_mdb_pcie_probe() 가 dev/ops 등을 채운다.
	 * 읽는 자: DWC 코어(dw_pcie_host_init 등)와 이 파일의 인터럽트 설정.
	 * 값 범위: 내장 구조체라 항상 유효하다.
	 * 동기화: probe 이후 구성은 불변이고, 런타임 접근은 dw_pcie_rp 의 잠금이 지킨다. */
	struct dw_pcie			pci;
	/* [한국어] SLCR(시스템 수준 제어·상태) 레지스터 창의 가상 주소.
	 * 설정자: amd_mdb_add_pcie_port() 가 "slcr" 이라는 **이름으로** 매핑한다.
	 * 읽는 자: 이 파일의 모든 인터럽트 레지스터 접근.
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: 창 자체는 probe 이후 불변이고, 그 안의 레지스터 접근을
	 * dw_pcie_rp 의 잠금이 지킨다. */
	void __iomem			*slcr;
	/* [한국어] INTx 넷을 담당하는 도메인.
	 * 설정자: amd_mdb_pcie_init_irq_domains() 가 만들고,
	 * amd_mdb_pcie_free_irq_domains() 가 없앤 뒤 NULL 로 되돌린다.
	 * 읽는 자: dw_pcie_rp_intx() 가 여기로 인터럽트를 넘긴다.
	 * 값 범위: 유효한 도메인 포인터 또는 NULL.
	 * **NULL 여부가 되감기 판단 근거** 라 해제 뒤 되돌리는 것이 중요하다.
	 * 동기화: probe 이후 불변. */
	struct irq_domain		*intx_domain;
	/* [한국어] 상태 레지스터 32비트 전체를 담당하는 이벤트 도메인.
	 * 설정자·읽는 자: 위와 같다. 다만 amd_mdb_pcie_event() 가 여기로 넘긴다.
	 * 값 범위: 유효한 도메인 포인터 또는 NULL.
	 * 동기화: probe 이후 불변.
	 * **INTx 용 인터럽트 번호도 이 도메인에서 얻는다** — 그것이 이층 구조의
	 * 이음매다. */
	struct irq_domain		*mdb_domain;
	/* [한국어] PERST# 신호의 GPIO 디스크립터.
	 * 설정자: probe 가 두 자리에서 찾아 본다 — 자식 노드, 그다음 자기 노드.
	 * 읽는 자: amd_mdb_add_pcie_port() 가 리셋을 풀 때.
	 * 값 범위: 유효한 디스크립터, 또는 **GPIO 가 없는 보드에서는 NULL.**
	 * NULL 이어도 오류가 아니며, 그 경우 리셋 조작을 건너뛴다.
	 * 동기화: probe 이후 불변. */
	struct gpio_desc		*perst_gpio;
	/* [한국어] INTx 묶음에 배정된 가상 인터럽트 번호.
	 * 설정자: amd_mdb_setup_irq() 가 mdb_domain 에서 얻는다.
	 * 읽는 자: [상류 코드 관찰] 설정한 그 자리의 오류 메시지 말고는
	 * 이 파일에서 다시 읽는 곳이 없다.
	 * 값 범위: 0 이 아닌 가상 인터럽트 번호. 0 은 실패를 뜻한다.
	 * 동기화: probe 이후 불변. */
	int				intx_irq;
};

/* [한국어] **콜백이 하나도 없는 표.** 코어에 넘길 SoC 고유 초기화가 없다는
 * 뜻이며, 이 파일이 인터럽트에만 집중한다는 사실을 가장 간명하게 보여 준다.
 * 그런데도 표를 두고 pp->ops 에 거는 것은 코어가 그 포인터의 존재를
 * 전제하기 때문으로 보이나, 근거는 이 트리에서 확인 못 함. */
static const struct dw_pcie_host_ops amd_mdb_pcie_host_ops = {
};

/* [한국어]
 * amd_mdb_intx_irq_mask - INTx 하나를 막는다
 *
 * @data: 이 인터럽트의 irq_data. hwirq 가 INTx 번호(0~3)다.
 *
 * 전용 "끄기" 레지스터에 **끄고 싶은 비트만 1 로** 쓴다. 옆의 상류 주석이
 * 밝히듯 0 을 쓰는 것은 아무 효과가 없으므로, 읽기-수정-쓰기가 필요 없다.
 * 그 덕에 다른 INTx 를 건드릴 위험이 없다.
 *
 * 비트 자리를 두 단계로 만든다.
 * 1. INTx 번호를 2배 해 필드 안의 자리를 얻는다 — INTx 하나가 assert 와
 *    deassert 두 자리를 쓰기 때문이다.
 * 2. 그 값을 비트 23~16 필드에 밀어 넣는다.
 * 결과적으로 INTA 는 비트 16, INTB 는 18, INTC 는 20, INTD 는 22 다.
 * **assert 자리만 건드리고 deassert 자리는 그대로 둔다.**
 *
 * 잠금이 필요한 이유는 레지스터 자체가 아니라 이 드라이버 전체가 같은
 * SLCR 창을 공유하기 때문이다. raw 스핀락에 인터럽트까지 막는 판을 쓰는데,
 * 이 함수가 하드 인터럽트 문맥에서도 불릴 수 있어서다.
 *
 * 그 잠금이 이 파일의 것이 아니라 **DWC 코어의 dw_pcie_rp 안에 있는
 * 것** 이라는 점이 눈에 띈다. 이 파일이 그것을 직접 초기화해 쓴다.
 *
 * 실행 컨텍스트: 인터럽트 마스크 경로. 하드 인터럽트 문맥일 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   irq 코어 → irq_chip.irq_mask == [이 함수] → writel_relaxed()
 */
static void amd_mdb_intx_irq_mask(struct irq_data *data)
{
	struct amd_mdb_pcie *pcie = irq_data_get_irq_chip_data(data);
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] 잠금이 든 DWC 루트 포트 문맥. 이 잠금을 이 파일이 초기화해 쓴다. */
	struct dw_pcie_rp *port = &pci->pp;
	/* [한국어] 인터럽트 상태를 보관할 자리. 하드 인터럽트 문맥에서도 불릴 수 있다. */
	unsigned long flags;
	/* [한국어] 쓸 비트를 담을 자리. */
	u32 val;

	raw_spin_lock_irqsave(&port->lock, flags);
	/* [한국어] 필드 안의 자리를 얻어 비트 23~16 에 밀어 넣는다. */
	val = FIELD_PREP(AMD_MDB_TLP_PCIE_INTX_MASK,
			 /* [한국어] INTx 번호를 2배 해 assert 칸을 고른다. */
			 AMD_MDB_PCIE_INTR_INTX_ASSERT(data->hwirq));

	/*
	 * Writing '1' to a bit in AMD_MDB_TLP_IR_DISABLE_MISC disables that
	 * interrupt, writing '0' has no effect.
	 */
	writel_relaxed(val, pcie->slcr + AMD_MDB_TLP_IR_DISABLE_MISC);
	raw_spin_unlock_irqrestore(&port->lock, flags);
}

/* [한국어]
 * amd_mdb_intx_irq_unmask - INTx 하나를 다시 허용한다
 *
 * @data: 이 인터럽트의 irq_data. hwirq 가 INTx 번호(0~3)다.
 *
 * 위 마스크 함수의 짝이며, 쓰는 레지스터가 "켜기" 쪽이라는 것만 다르다.
 * 비트 계산도 잠금도 똑같다.
 *
 * 켜기와 끄기가 **별개의 레지스터** 로 나뉘어 있는 것이 이 하드웨어의
 * 특징이다. 한 레지스터에 읽기-수정-쓰기를 하는 방식이었다면 잠금 없이는
 * 경쟁이 생기는데, 이 배치에서는 각 쓰기가 독립적이다.
 *
 * 실행 컨텍스트: 인터럽트 언마스크 경로. 하드 인터럽트 문맥일 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   irq 코어 → irq_chip.irq_unmask == [이 함수] → writel_relaxed()
 */
static void amd_mdb_intx_irq_unmask(struct irq_data *data)
{
	struct amd_mdb_pcie *pcie = irq_data_get_irq_chip_data(data);
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] 잠금이 든 DWC 루트 포트 문맥. */
	struct dw_pcie_rp *port = &pci->pp;
	/* [한국어] 인터럽트 상태를 보관할 자리. */
	unsigned long flags;
	/* [한국어] 쓸 비트를 담을 자리. */
	u32 val;

	raw_spin_lock_irqsave(&port->lock, flags);
	/* [한국어] 마스크 판과 같은 계산이다. */
	val = FIELD_PREP(AMD_MDB_TLP_PCIE_INTX_MASK,
			 /* [한국어] 역시 assert 칸만 고른다. */
			 AMD_MDB_PCIE_INTR_INTX_ASSERT(data->hwirq));

	/*
	 * Writing '1' to a bit in AMD_MDB_TLP_IR_ENABLE_MISC enables that
	 * interrupt, writing '0' has no effect.
	 */
	/* [한국어] 켜기 레지스터에 그 비트만 쓴다. 옆의 상류 주석대로 0 은 무시된다. */
	writel_relaxed(val, pcie->slcr + AMD_MDB_TLP_IR_ENABLE_MISC);
	/* [한국어] 잠금을 푼다. */
	raw_spin_unlock_irqrestore(&port->lock, flags);
}

static struct irq_chip amd_mdb_intx_irq_chip = {
	/* [한국어] /proc/interrupts 에 보일 이름. */
	.name		= "AMD MDB INTx",
	/* [한국어] 레벨 트리거라 핸들러 전후로 이 둘이 불린다. */
	.irq_mask	= amd_mdb_intx_irq_mask,
	.irq_unmask	= amd_mdb_intx_irq_unmask,
};

/**
 * amd_mdb_pcie_intx_map - Set the handler for the INTx and mark IRQ as valid
 * @domain: IRQ domain
 * @irq: Virtual IRQ number
 * @hwirq: Hardware interrupt number
 *
 * Return: Always returns '0'.
 */
/* [한국어]
 * amd_mdb_pcie_intx_map - INTx 도메인의 가상 인터럽트 하나를 설정한다
 *
 * @domain: INTx 도메인.
 * @irq: 배정된 가상 인터럽트 번호.
 * @hwirq: 그에 대응하는 INTx 번호(0~3).
 * @return: 언제나 0.
 *
 * 도메인이 새 인터럽트를 만들 때마다 불려, 세 가지를 붙인다.
 * 1. 위의 irq_chip 과 레벨 트리거 흐름 함수.
 * 2. 드라이버 상태(도메인의 host_data). 마스크·언마스크가 이것으로
 *    레지스터에 닿는다.
 * 3. 레벨 인터럽트 표시.
 *
 * **INTx 는 레벨 트리거** 라는 것이 요점이다. 원인이 사라질 때까지 계속
 * 올라오므로, handle_level_irq 가 핸들러를 부르기 전에 마스크하고 끝난 뒤
 * 언마스크한다 — 그래서 위 마스크·언마스크 함수가 필요하다.
 *
 * 실행 컨텍스트: 인터럽트 매핑. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   irq 코어의 매핑 → irq_domain_ops.map == [이 함수]
 *     → irq_set_chip_and_handler() → irq_set_chip_data()
 *     → irq_set_status_flags()
 */
static int amd_mdb_pcie_intx_map(struct irq_domain *domain,
				 unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &amd_mdb_intx_irq_chip,
				 handle_level_irq);
	irq_set_chip_data(irq, domain->host_data);
	/* [한국어] 레벨 인터럽트임을 표시한다. INTx 는 원인이 사라질 때까지 계속
	 * 올라오는 신호이기 때문이다. */
	irq_set_status_flags(irq, IRQ_LEVEL);

	return 0;
}

/* INTx IRQ domain operations. */
static const struct irq_domain_ops amd_intx_domain_ops = {
	.map = amd_mdb_pcie_intx_map,
};

/* [한국어]
 * dw_pcie_rp_intx - 2단계 분배. INTx 넷 중 어느 것인지 가려 넘긴다
 *
 * @irq: 이 핸들러가 걸린 인터럽트 번호. 쓰지 않는다.
 * @args: 드라이버 상태.
 * @return: 언제나 IRQ_HANDLED.
 *
 * 이층 분배의 **아랫단** 이다. mdb_domain 의 비트 16(INTx 묶음)에 걸려
 * 있어서, 그 비트가 서면 불린다.
 *
 * 하는 일은 상태 레지스터를 **다시 읽어** INTx 필드를 꺼내고, 넷을 차례로
 * 확인해 서 있는 것마다 intx_domain 으로 넘기는 것이다.
 *
 * 윗단이 이미 상태를 읽었는데 여기서 또 읽는다. 윗단은 32비트 전체를
 * 비트 단위로 분배할 뿐 필드의 내용을 넘겨 주지 않으므로, 아랫단이 자기
 * 몫을 다시 읽어야 한다.
 *
 * assert 비트만 확인한다 — deassert 비트는 무시된다.
 *
 * [상류 코드 관찰] 이름이 `dw_pcie_` 로 시작한다. 그것은 DWC 코어 공용
 * 코드의 이름 규칙인데 이 함수는 벤더 글루의 static 함수다. 이 트리의
 * 다른 어느 파일에도 같은 이름이 없어 충돌하지는 않는다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * [상류 코드 관찰] generic_handle_domain_irq() 의 반환값을 확인하지 않는다.
 * 같은 트리의 pci-xgene-msi.c 는 그것을 WARN_ON_ONCE 로 확인한다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥(IRQF_NO_THREAD).
 *
 * 에러 경로: 없다. 서 있는 비트가 하나도 없어도 IRQ_HANDLED 를 돌려준다.
 *
 * 호출 체인:
 *   amd_mdb_pcie_event() → (mdb_domain 경유) → [이 함수]
 *     → readl_relaxed() → generic_handle_domain_irq()
 */
static irqreturn_t dw_pcie_rp_intx(int irq, void *args)
{
	struct amd_mdb_pcie *pcie = args;
	unsigned long val;
	/* [한국어] 루프 첨자와, 상태에서 꺼낸 INTx 필드 값. */
	int i, int_status;

	val = readl_relaxed(pcie->slcr + AMD_MDB_TLP_IR_STATUS_MISC);
	/* [한국어] 비트 23~16 필드를 꺼내 아래로 내린다. 이제 INTA 의 assert 가
	 * 0번째 비트가 된다. */
	int_status = FIELD_GET(AMD_MDB_TLP_PCIE_INTX_MASK, val);

	for (i = 0; i < PCI_NUM_INTX; i++) {
		/* [한국어] 이 INTx 의 assert 비트가 서 있는지 본다. deassert 비트는 무시한다. */
		if (int_status & AMD_MDB_PCIE_INTR_INTX_ASSERT(i))
			/* [한국어] intx_domain 으로 넘긴다. 여기서 장치 드라이버의 핸들러까지 간다.
			 * 반환값은 확인하지 않는다. */
			generic_handle_domain_irq(pcie->intx_domain, i);
	}

	return IRQ_HANDLED;
}

#define _IC(x, s)[AMD_MDB_PCIE_INTR_ ## x] = { __stringify(x), s }

static const struct {
	/* [한국어] 매크로 이름 그대로의 문자열. /proc/interrupts 에 쓰인다. */
	const char	*sym;
	/* [한국어] 사람이 읽을 설명. **이 필드가 NULL 인지 여부가 "이 비트를 다루는가" 의
	 * 판정 기준** 이며, 설정 루프와 핸들러가 모두 그것으로 가른다. */
	const char	*str;
/* [한국어] 비트 번호로 찾는 원인 표. 크기가 32 인 것은 상태 레지스터의 비트 수와
 * 같다. **드문드문 채워져 있고**, 특히 비트 16(INTx)에는 항목이 없어
 * 설정 루프가 그것을 건너뛴다. */
} intr_cause[32] = {
	/* [한국어] 비트 15. */
	_IC(CMPL_TIMEOUT,	"Completion timeout"),
	/* [한국어] 비트 24. */
	_IC(PM_PME_RCVD,	"PM_PME message received"),
	_IC(PME_TO_ACK_RCVD,	"PME_TO_ACK message received"),
	_IC(MISC_CORRECTABLE,	"Correctable error message"),
	_IC(NONFATAL,		"Non fatal error message"),
	_IC(FATAL,		"Fatal error message"),
};

/* [한국어]
 * amd_mdb_event_irq_mask - 이벤트 비트 하나를 막는다
 *
 * @d: 이 인터럽트의 irq_data. hwirq 가 상태 레지스터의 비트 번호다.
 *
 * INTx 판과 같은 레지스터를 쓰지만 **비트 계산이 훨씬 단순하다** —
 * hwirq 가 곧 비트 번호이므로 그대로 BIT() 하면 된다. INTx 쪽이 두 단계를
 * 거치는 것은 그쪽만 필드 안에 다시 자리가 나뉘어 있기 때문이다.
 *
 * 역시 "끄기" 전용 레지스터라 읽기-수정-쓰기가 없다.
 *
 * 같은 잠금(DWC 코어의 rp->lock)을 쓴다.
 *
 * 실행 컨텍스트: 인터럽트 마스크 경로. 하드 인터럽트 문맥일 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   irq 코어 → irq_chip.irq_mask == [이 함수] → writel_relaxed()
 */
static void amd_mdb_event_irq_mask(struct irq_data *d)
{
	struct amd_mdb_pcie *pcie = irq_data_get_irq_chip_data(d);
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] 잠금이 든 DWC 루트 포트 문맥. */
	struct dw_pcie_rp *port = &pci->pp;
	/* [한국어] 인터럽트 상태를 보관할 자리. */
	unsigned long flags;
	/* [한국어] 쓸 비트를 담을 자리. */
	u32 val;

	raw_spin_lock_irqsave(&port->lock, flags);
	/* [한국어] **hwirq 가 곧 비트 번호** 라 그대로 BIT() 하면 된다. INTx 판이
	 * 두 단계를 거치는 것과 대비된다. */
	val = BIT(d->hwirq);
	/* [한국어] 끄기 레지스터에 그 비트만 쓴다. */
	writel_relaxed(val, pcie->slcr + AMD_MDB_TLP_IR_DISABLE_MISC);
	/* [한국어] 잠금을 푼다. */
	raw_spin_unlock_irqrestore(&port->lock, flags);
}

/* [한국어]
 * amd_mdb_event_irq_unmask - 이벤트 비트 하나를 다시 허용한다
 *
 * @d: 이 인터럽트의 irq_data. hwirq 가 상태 레지스터의 비트 번호다.
 *
 * 위 마스크 함수의 짝이며 "켜기" 레지스터를 쓴다.
 *
 * 실행 컨텍스트: 인터럽트 언마스크 경로. 하드 인터럽트 문맥일 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   irq 코어 → irq_chip.irq_unmask == [이 함수] → writel_relaxed()
 */
static void amd_mdb_event_irq_unmask(struct irq_data *d)
{
	struct amd_mdb_pcie *pcie = irq_data_get_irq_chip_data(d);
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] 잠금이 든 DWC 루트 포트 문맥. */
	struct dw_pcie_rp *port = &pci->pp;
	/* [한국어] 인터럽트 상태를 보관할 자리. */
	unsigned long flags;
	/* [한국어] 쓸 비트를 담을 자리. */
	u32 val;

	raw_spin_lock_irqsave(&port->lock, flags);
	/* [한국어] 마스크 판과 같은 계산이다. */
	val = BIT(d->hwirq);
	/* [한국어] 켜기 레지스터에 그 비트만 쓴다. */
	writel_relaxed(val, pcie->slcr + AMD_MDB_TLP_IR_ENABLE_MISC);
	/* [한국어] 잠금을 푼다. */
	raw_spin_unlock_irqrestore(&port->lock, flags);
}

static struct irq_chip amd_mdb_event_irq_chip = {
	/* [한국어] /proc/interrupts 에 보일 이름. INTx 판과 구별된다. */
	.name		= "AMD MDB RC-Event",
	/* [한국어] 역시 레벨 트리거 흐름이다. */
	.irq_mask	= amd_mdb_event_irq_mask,
	.irq_unmask	= amd_mdb_event_irq_unmask,
};

/* [한국어]
 * amd_mdb_pcie_event_map - 이벤트 도메인의 가상 인터럽트 하나를 설정한다
 *
 * @domain: 이벤트(mdb) 도메인.
 * @irq: 배정된 가상 인터럽트 번호.
 * @hwirq: 상태 레지스터의 비트 번호(0~31).
 * @return: 언제나 0.
 *
 * amd_mdb_pcie_intx_map() 과 구조가 같고 irq_chip 만 다르다.
 *
 * 이 도메인의 인터럽트가 두 갈래로 쓰인다는 점이 눈여겨볼 만하다 —
 * 오류 원인들은 amd_mdb_pcie_intr_handler() 로 가고, 비트 16 하나만
 * dw_pcie_rp_intx() 로 가서 아랫단 분배를 시작한다. 도메인 입장에서는
 * 둘이 구별되지 않고, 어느 핸들러를 거느냐만 다르다.
 *
 * 실행 컨텍스트: 인터럽트 매핑. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   irq 코어의 매핑 → irq_domain_ops.map == [이 함수]
 *     → irq_set_chip_and_handler() → irq_set_chip_data()
 *     → irq_set_status_flags()
 */
static int amd_mdb_pcie_event_map(struct irq_domain *domain,
				  unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &amd_mdb_event_irq_chip,
				 handle_level_irq);
	irq_set_chip_data(irq, domain->host_data);
	/* [한국어] 레벨 인터럽트임을 표시한다. */
	irq_set_status_flags(irq, IRQ_LEVEL);

	return 0;
}

static const struct irq_domain_ops event_domain_ops = {
	/* [한국어] 이 도메인의 인터럽트가 만들어질 때마다 위 함수가 불린다. */
	.map = amd_mdb_pcie_event_map,
};

/* [한국어]
 * amd_mdb_pcie_event - 1단계 분배. 상태 32비트를 이벤트 도메인으로 보낸다
 *
 * @irq: 이 핸들러가 걸린 인터럽트 번호. 쓰지 않는다.
 * @args: 드라이버 상태.
 * @return: 언제나 IRQ_HANDLED.
 *
 * **하드웨어 인터럽트 선 하나에 걸린 유일한 핸들러** 이며, 이층 분배의
 * 윗단이다.
 *
 * 네 단계다.
 * 1. 상태 레지스터를 읽는다.
 * 2. 마스크 레지스터를 읽어 **막혀 있는 비트를 뺀다.** 이 한 줄이 없으면
 *    마스크된 인터럽트도 분배되어, 마스크가 무의미해진다.
 * 3. 남은 비트마다 이벤트 도메인으로 넘긴다. 비트 16 이 여기 있으면
 *    그 경로로 아랫단 INTx 분배가 시작된다.
 * 4. **분배한 비트만** 상태 레지스터에 되쓴다(W1C).
 *
 * 4번의 순서와 범위가 요점이다. 분배 **뒤** 에 지우고, 지우는 값이 2번에서
 * 걸러 낸 val 이므로 **처리 도중에 새로 선 비트는 지워지지 않는다.**
 * 그 비트는 다음 인터럽트에서 처리된다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥(IRQF_NO_THREAD).
 *
 * 에러 경로: 없다. generic_handle_domain_irq() 의 반환값을 확인하지 않는다.
 *
 * 호출 체인:
 *   하드웨어 인터럽트 → [이 함수]
 *     → readl_relaxed() ×2 → generic_handle_domain_irq() → writel_relaxed()
 */
static irqreturn_t amd_mdb_pcie_event(int irq, void *args)
{
	struct amd_mdb_pcie *pcie = args;
	unsigned long val;
	/* [한국어] 루프 첨자 — 상태 레지스터의 비트 번호가 된다. */
	int i;

	val = readl_relaxed(pcie->slcr + AMD_MDB_TLP_IR_STATUS_MISC);
	/* [한국어] **막혀 있는 비트를 뺀다.** 이 한 줄이 없으면 마스크된 인터럽트도
	 * 분배되어 마스크가 무의미해진다. */
	val &= ~readl_relaxed(pcie->slcr + AMD_MDB_TLP_IR_MASK_MISC);
	/* [한국어] 남은 비트를 차례로 훑는다. 상한 32 는 상태 레지스터의 비트 수다. */
	for_each_set_bit(i, &val, 32)
		/* [한국어] 이벤트 도메인으로 넘긴다. 비트 16 이면 이 경로로 INTx 분배가
		 * 시작된다. */
		generic_handle_domain_irq(pcie->mdb_domain, i);
	/* [한국어] **분배한 비트만** 되쓴다(W1C). 처리 도중에 새로 선 비트는 val 에
	 * 없으므로 지워지지 않고, 다음 인터럽트에서 처리된다. */
	writel_relaxed(val, pcie->slcr + AMD_MDB_TLP_IR_STATUS_MISC);

	return IRQ_HANDLED;
}

/* [한국어]
 * amd_mdb_pcie_free_irq_domains - 만든 도메인 둘을 없앤다
 *
 * @pcie: 드라이버 상태.
 *
 * 되감기 함수이며, **각각 NULL 검사를 하고 없앤 뒤 NULL 로 되돌린다.**
 *
 * 그 두 가지가 다 필요하다. 검사는 부분 초기화 상태(하나만 만들어진
 * 경우)를 견디게 하고, NULL 로 되돌리는 것은 같은 도메인을 두 번 없애는
 * 것을 막는다 — 이 함수가 초기화 실패 경로와 add_pcie_port 실패 경로
 * 양쪽에서 불릴 수 있기 때문이다.
 *
 * 같은 트리의 pcie-xilinx.c 가 같은 성격의 함수에서 NULL 검사를 하지
 * 않는 것과 대비된다.
 *
 * 실행 컨텍스트: probe 되감기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   amd_mdb_pcie_init_irq_domains() / amd_mdb_add_pcie_port() 의 out
 *     → [이 함수] → irq_domain_remove()
 */
static void amd_mdb_pcie_free_irq_domains(struct amd_mdb_pcie *pcie)
{
	/* [한국어] INTx 도메인이 만들어져 있으면 없앤다. 부분 초기화 상태에서도
	 * 안전하게 하는 검사다. */
	if (pcie->intx_domain) {
		/* [한국어] 도메인을 없앤다. */
		irq_domain_remove(pcie->intx_domain);
		/* [한국어] **NULL 로 되돌린다.** 이 함수가 두 경로에서 불릴 수 있어, 같은
		 * 도메인을 두 번 없애는 것을 막는다. */
		pcie->intx_domain = NULL;
	}

	/* [한국어] 이벤트 도메인도 같은 방식으로 확인한다. */
	if (pcie->mdb_domain) {
		/* [한국어] 이벤트 도메인도 없앤다. */
		irq_domain_remove(pcie->mdb_domain);
		pcie->mdb_domain = NULL;
	}
}

/* [한국어]
 * amd_mdb_pcie_init_port - 전부 끄고, 대기 중인 것을 지우고, 전부 켠다
 *
 * @pcie: 드라이버 상태.
 * @return: 언제나 0.
 *
 * 인터럽트를 받기 전에 알려진 상태를 만드는 세 단계다.
 *
 * 1. **전부 끈다.** 부트로더가 남긴 설정이 있어도 이 한 줄로 지워진다.
 * 2. **대기 중인 것을 지운다.** 상태를 읽어 관심 있는 비트만 남긴 뒤
 *    되쓴다(W1C). 마스크를 씌우는 것은 이 드라이버가 다루지 않는 비트를
 *    건드리지 않기 위해서다.
 * 3. **전부 켠다.**
 *
 * 1번과 3번 사이에 2번을 두는 순서가 중요하다. 끈 상태에서 지워야, 지우는
 * 동안 새 인터럽트가 올라와 그대로 유실되는 일이 없다.
 *
 * **끄기·켜기가 통째 쓰기인데도 안전한** 이유는 그 두 레지스터가 1 만
 * 의미를 갖기 때문이다. 관심 밖 비트에는 0 이 들어가고, 0 은 아무 효과가
 * 없다.
 *
 * [상류 코드 관찰] 반환형이 int 인데 언제나 0 이고, 유일한 호출자도 그
 * 값을 확인하지 않는다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   amd_mdb_setup_irq() → [이 함수] → writel_relaxed() → readl_relaxed()
 */
static int amd_mdb_pcie_init_port(struct amd_mdb_pcie *pcie)
{
	unsigned long val;

	/* Disable all TLP interrupts. */
	writel_relaxed(AMD_MDB_PCIE_IMR_ALL_MASK,
		       pcie->slcr + AMD_MDB_TLP_IR_DISABLE_MISC);

	/* Clear pending TLP interrupts. */
	val = readl_relaxed(pcie->slcr + AMD_MDB_TLP_IR_STATUS_MISC);
	val &= AMD_MDB_PCIE_IMR_ALL_MASK;
	/* [한국어] 관심 있는 비트만 남겨 되쓴다(W1C). 이 드라이버가 다루지 않는 비트는
	 * 건드리지 않는다. */
	writel_relaxed(val, pcie->slcr + AMD_MDB_TLP_IR_STATUS_MISC);

	/* Enable all TLP interrupts. */
	writel_relaxed(AMD_MDB_PCIE_IMR_ALL_MASK,
		       pcie->slcr + AMD_MDB_TLP_IR_ENABLE_MISC);

	return 0;
}

/**
 * amd_mdb_pcie_init_irq_domains - Initialize IRQ domain
 * @pcie: PCIe port information
 * @pdev: Platform device
 *
 * Return: Returns '0' on success and error value on failure.
 */
/* [한국어]
 * amd_mdb_pcie_init_irq_domains - 이벤트·INTx 도메인 두 벌을 만든다
 *
 * @pcie: 드라이버 상태.
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 디바이스 트리의 "interrupt-controller" 자식 노드를 이름표 삼아 도메인
 * 둘을 만든다. 그 노드가 있어야 다른 디바이스 트리 노드가 이 컨트롤러를
 * 인터럽트 부모로 가리킬 수 있다.
 *
 * 크기가 다르다 — 이벤트 도메인은 32(상태 레지스터의 비트 수), INTx
 * 도메인은 4(INTx 개수)다.
 *
 * **버스 토큰을 다르게 매긴다.** 이벤트 도메인은 NEXUS, INTx 도메인은
 * WIRED 다. 같은 디바이스 트리 노드를 이름표로 쓰는 도메인 둘을 구별하는
 * 수단이며, 토큰이 없으면 조회가 어느 쪽을 고를지 알 수 없다.
 *
 * 마지막에 **DWC 코어의 잠금을 초기화한다.** 이 파일의 마스크·언마스크가
 * 그 잠금을 쓰는데, 코어의 dw_pcie_host_init() 이 그것을 초기화하는 것은
 * (pcie-designware-host.c:1493) 나중이라, 그 전에 인터럽트가 올라와도
 * 잠금이 유효하도록 여기서 미리 초기화해 둔다.
 *
 * 노드 참조 관리가 갈래마다 다르다 — 성공 경로와 mdb_out 경로는 각각
 * 따로 of_node_put 을 부르고, 그 배치 때문에 성공 시 두 번 풀리지 않는다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 자식 노드가 없으면 -ENODEV, 도메인을 못 만들면 -ENOMEM 이며,
 * 둘째가 실패하면 첫째까지 되감는다.
 *
 * 호출 체인:
 *   amd_mdb_add_pcie_port() → [이 함수]
 *     → of_get_child_by_name() → irq_domain_create_linear() ×2
 *     → irq_domain_update_bus_token() ×2 → raw_spin_lock_init()
 */
static int amd_mdb_pcie_init_irq_domains(struct amd_mdb_pcie *pcie,
					 struct platform_device *pdev)
{
	struct dw_pcie *pci = &pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 오류 기록과 자식 노드 탐색의 기준이 될 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 이 컨트롤러의 디바이스 트리 노드. */
	struct device_node *node = dev->of_node;
	/* [한국어] 도메인의 이름표가 될 자식 노드. */
	struct device_node *pcie_intc_node;
	/* [한국어] 각 단계의 반환값. */
	int err;

	pcie_intc_node = of_get_child_by_name(node, "interrupt-controller");
	/* [한국어] 인터럽트 컨트롤러 자식 노드가 없다. 다른 노드가 이 컨트롤러를
	 * 인터럽트 부모로 가리킬 수 없으므로 진행할 수 없다. */
	if (!pcie_intc_node) {
		/* [한국어] 실패를 알린다. */
		dev_err(dev, "No PCIe Intc node found\n");
		/* [한국어] 아직 만든 것이 없어 곧바로 돌아간다. */
		return -ENODEV;
	}

	pcie->mdb_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node), 32,
						    /* [한국어] 크기 32 는 상태 레지스터의 비트 수와 같다. */
						    &event_domain_ops, pcie);
	if (!pcie->mdb_domain) {
		/* [한국어] 도메인을 못 만들었다. */
		err = -ENOMEM;
		dev_err(dev, "Failed to add MDB domain\n");
		/* [한국어] 노드 참조만 풀고 돌아가는 라벨로 간다. */
		goto out;
	}

	irq_domain_update_bus_token(pcie->mdb_domain, DOMAIN_BUS_NEXUS);
/* [한국어] 버스 토큰을 NEXUS 로 매긴다. **같은 노드를 이름표로 쓰는 도메인이
 * 둘이라**, 토큰이 없으면 조회가 어느 쪽을 고를지 알 수 없다. */

	pcie->intx_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node),
						     /* [한국어] 크기 4 는 INTx 개수다. */
						     PCI_NUM_INTX, &amd_intx_domain_ops, pcie);
	if (!pcie->intx_domain) {
		/* [한국어] INTx 도메인을 못 만들었다. */
		err = -ENOMEM;
		dev_err(dev, "Failed to add INTx domain\n");
		/* [한국어] 이미 만든 이벤트 도메인까지 되감는 라벨로 간다. */
		goto mdb_out;
	}

	of_node_put(pcie_intc_node);
	irq_domain_update_bus_token(pcie->intx_domain, DOMAIN_BUS_WIRED);
/* [한국어] INTx 도메인에는 WIRED 토큰을 매긴다. 위 NEXUS 와 짝을 이뤄 둘을
 * 구별한다. */

	raw_spin_lock_init(&pp->lock);

	return 0;
mdb_out:
	amd_mdb_pcie_free_irq_domains(pcie);
out:
	of_node_put(pcie_intc_node);
	return err;
}

/* [한국어]
 * amd_mdb_pcie_intr_handler - 오류 원인 하나를 받아 경고를 남긴다
 *
 * @irq: 이 핸들러가 걸린 가상 인터럽트 번호. **여기서 원인을 되찾는다.**
 * @args: 드라이버 상태.
 * @return: 언제나 IRQ_HANDLED.
 *
 * 원인 여섯(완료 시간 초과, PM_PME 수신, PME_TO_ACK 수신, 정정 가능 오류,
 * 치명적이지 않은 오류, 치명적 오류)에 **같은 함수가 등록된다.** 어느
 * 원인인지는 인터럽트 번호로 되찾는다 — irq_data 를 얻어 그 hwirq 로
 * 원인 표를 찾는다.
 *
 * 옆의 상류 주석이 현재 상태를 밝힌다. 앞으로 AER 서브시스템에 연결할
 * 계획이고, 지금은 경고 메시지를 찍는 것이 전부다. 즉 **오류를 복구하지
 * 않는다.**
 *
 * 표에 없는 비트면 "알 수 없음" 을 한 번만 찍는다. 설정상 그런 비트에는
 * 핸들러가 걸리지 않으므로 실제로는 닿지 않는 갈래다.
 *
 * [상류 코드 관찰] irq_domain_get_irq_data() 의 반환을 NULL 검사 없이
 * 곧바로 역참조한다. 같은 형태가 pcie-xilinx.c 에도 있다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 하드 인터럽트 문맥(IRQF_NO_THREAD). dev_warn 이 인터럽트
 * 문맥에서 도는 셈이다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   amd_mdb_pcie_event() → (mdb_domain 경유) → [이 함수]
 *     → irq_domain_get_irq_data() → dev_warn()
 */
static irqreturn_t amd_mdb_pcie_intr_handler(int irq, void *args)
{
	struct amd_mdb_pcie *pcie = args;
	struct device *dev;
	/* [한국어] 인터럽트 서술 정보. 여기서 hwirq 를 꺼내 원인을 되찾는다. */
	struct irq_data *d;

	dev = pcie->pci.dev;
/* [한국어] 가상 인터럽트 번호로 서술 정보를 얻는다. 위 [상류 코드 관찰] 대로
 * NULL 검사가 없다. */

	/*
	 * In the future, error reporting will be hooked to the AER subsystem.
	 * Currently, the driver prints a warning message to the user.
	 */
	d = irq_domain_get_irq_data(pcie->mdb_domain, irq);
	if (intr_cause[d->hwirq].str)
		/* [한국어] 표에 있는 원인이면 그 설명을 경고로 남긴다. 복구는 하지 않는다. */
		dev_warn(dev, "%s\n", intr_cause[d->hwirq].str);
	/* [한국어] 표에 없는 비트다. */
	else
		dev_warn_once(dev, "Unknown IRQ %ld\n", d->hwirq);

	return IRQ_HANDLED;
}

/* [한국어]
 * amd_mdb_setup_irq - 원인별로 인터럽트를 매핑하고 핸들러 셋을 건다
 *
 * @pcie: 드라이버 상태.
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 인터럽트 설정 전부가 여기 있으며, 이층 구조의 이음매가 드러나는
 * 함수이기도 하다.
 *
 * 네 묶음이다.
 * 1. 포트를 알려진 상태로 만든다.
 * 2. 하드웨어 인터럽트 번호를 얻는다. 하나뿐이다.
 * 3. **원인 표를 훑어** 설명이 있는 비트마다 매핑을 만들고 같은 핸들러를
 *    건다. 표에 이름이 없는 비트는 건너뛰므로, 비트 16(INTx)은 여기서
 *    빠진다 — 표에 항목이 없기 때문이다.
 * 4. 그 비트 16 을 따로 매핑해 INTx 분배 핸들러를 건다. **매핑을
 *    intx_domain 이 아니라 mdb_domain 에서 얻는 것** 이 이층 구조의
 *    이음매다. 그리고 마지막으로 하드웨어 인터럽트에 윗단 핸들러를 건다.
 *
 * 세 종류의 핸들러가 모두 IRQF_NO_THREAD 로 등록된다 — 스레드로 미루지
 * 않고 하드 인터럽트 문맥에서 곧바로 돌게 하는 것이다.
 *
 * [상류 코드 관찰] 4번의 INTx 인터럽트만 이름을 NULL 로 등록한다.
 * 다른 둘은 각각 원인 이름과 "amd_mdb pcie_irq" 를 준다. 그래서
 * /proc/interrupts 에 그 항목만 이름 없이 나타난다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 되감기가 없다. 인터럽트는 devm 판이라 자동으로 풀리고, 매핑은
 * 호출자가 도메인을 없앨 때 함께 정리된다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 인터럽트를 못 얻으면 그 오류, 매핑 실패는 -ENOMEM 또는
 * -ENXIO, 핸들러 등록 실패는 그 오류를 각각 기록과 함께 올려보낸다.
 *
 * 호출 체인:
 *   amd_mdb_add_pcie_port() → [이 함수]
 *     → amd_mdb_pcie_init_port() → platform_get_irq()
 *     → irq_create_mapping() → devm_request_irq() ×3
 */
static int amd_mdb_setup_irq(struct amd_mdb_pcie *pcie,
			     struct platform_device *pdev)
{
	struct dw_pcie *pci = &pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 오류 기록과 인터럽트 요청의 기준이 될 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 루프 첨자, 매핑된 인터럽트 번호, 각 단계의 반환값. */
	int i, irq, err;

	amd_mdb_pcie_init_port(pcie);

	pp->irq = platform_get_irq(pdev, 0);
	/* [한국어] 하드웨어 인터럽트를 못 얻었다. */
	if (pp->irq < 0)
		/* [한국어] 그 오류를 올려보낸다. */
		return pp->irq;

	for (i = 0; i < ARRAY_SIZE(intr_cause); i++) {
		/* [한국어] 설명이 없는 비트는 이 드라이버가 다루지 않는 것이다.
		 * **비트 16(INTx)이 여기서 걸러진다** — 표에 항목이 없기 때문이다. */
		if (!intr_cause[i].str)
			/* [한국어] 건너뛴다. */
			continue;

		irq = irq_create_mapping(pcie->mdb_domain, i);
		/* [한국어] 매핑을 만들지 못했다. */
		if (!irq) {
			/* [한국어] 실패를 알린다. */
			dev_err(dev, "Failed to map MDB domain interrupt\n");
			/* [한국어] 메모리 부족으로 본다. */
			return -ENOMEM;
		}

		err = devm_request_irq(dev, irq, amd_mdb_pcie_intr_handler,
				       /* [한국어] IRQF_NO_THREAD 로 하드 인터럽트 문맥에서 곧바로 돌게 하고,
				        * 이름으로 원인 이름을 준다 — /proc/interrupts 에 그렇게 나타난다. */
				       IRQF_NO_THREAD, intr_cause[i].sym, pcie);
		if (err) {
			/* [한국어] 핸들러를 걸지 못했다. */
			dev_err(dev, "Failed to request IRQ %d, err=%d\n",
				irq, err);
			return err;
		}
	}

	pcie->intx_irq = irq_create_mapping(pcie->mdb_domain,
					    /* [한국어] 비트 16 이다. 위 루프가 건너뛴 그 자리를 여기서 따로 매핑한다. */
					    AMD_MDB_PCIE_INTR_INTX);
	if (!pcie->intx_irq) {
		/* [한국어] 매핑을 만들지 못했다. */
		dev_err(dev, "Failed to map INTx interrupt\n");
		/* [한국어] 장치 없음으로 본다 — 위 루프의 -ENOMEM 과 다른 코드를 쓴다. */
		return -ENXIO;
	}

	err = devm_request_irq(dev, pcie->intx_irq, dw_pcie_rp_intx,
			       /* [한국어] [상류 코드 관찰] 이름을 NULL 로 준다. 다른 두 요청은 이름을 주므로,
			        * /proc/interrupts 에 이 항목만 이름 없이 나타난다. */
			       IRQF_NO_THREAD, NULL, pcie);
	if (err) {
		/* [한국어] INTx 핸들러를 걸지 못했다. */
		dev_err(dev, "Failed to request INTx IRQ %d, err=%d\n",
			pcie->intx_irq, err);
		return err;
	}

	/* Plug the main event handler. */
	err = devm_request_irq(dev, pp->irq, amd_mdb_pcie_event, IRQF_NO_THREAD,
			       "amd_mdb pcie_irq", pcie);
	if (err) {
		/* [한국어] 윗단 이벤트 핸들러를 걸지 못했다. */
		dev_err(dev, "Failed to request event IRQ %d, err=%d\n",
			pp->irq, err);
		return err;
	}

	return 0;
}

/* [한국어]
 * amd_mdb_parse_pcie_port - 포트 자식 노드에서 PERST# GPIO 를 얻는다
 *
 * @pcie: 드라이버 상태.
 * @return: 0 = 얻었다, -ENODEV = 그런 노드가 없다, 그 밖은 오류.
 *
 * 이름이 "pcie" 로 시작하는 자식 노드를 찾아 그 안의 reset GPIO 를 얻는다.
 *
 * **루프가 최대 한 번만 돈다.** 첫 노드에서 성공하든 실패하든 그 안에서
 * 돌아가기 때문이다. 옆의 상류 주석이 그 이유를 밝힌다 — 이 플랫폼이
 * 루트 포트 하나만 지원하며, 여러 개를 다루는 것은 앞으로의 과제다.
 *
 * GPIOD_OUT_HIGH 로 얻는다. PERST# 가 active-low 신호이므로 **얻는 순간
 * 리셋이 걸린 상태** 가 되며, 나중에 add_pcie_port 가 그것을 푼다.
 *
 * -ENODEV 를 돌려주는 것이 실패가 아니라 **신호** 라는 점이 중요하다.
 * probe 가 그것을 보고 자기 노드에서 GPIO 를 다시 찾는 대체 경로로 간다.
 *
 * dev_err_probe 를 쓰므로 -EPROBE_DEFER 일 때는 로그를 남기지 않는다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: GPIO 확보 실패는 그 오류를, 노드가 없으면 -ENODEV 를 돌려준다.
 *
 * 호출 체인:
 *   amd_mdb_pcie_probe() → [이 함수] → devm_fwnode_gpiod_get()
 */
static int amd_mdb_parse_pcie_port(struct amd_mdb_pcie *pcie)
{
	/* [한국어] 자식 노드 탐색과 오류 기록의 기준이 될 device. */
	struct device *dev = pcie->pci.dev;
	/* [한국어] 순회 매크로가 쓰는 커서. 본문에서 직접 읽지 않아 __maybe_unused 가
	 * 붙어 있다 — 매크로가 노드 참조 관리에 쓰기 때문이다. */
	struct device_node *pcie_port_node __maybe_unused;

	/*
	 * This platform currently supports only one Root Port, so the loop
	 * will execute only once.
	 * TODO: Enhance the driver to handle multiple Root Ports in the future.
	 */
	for_each_child_of_node_with_prefix(dev->of_node, pcie_port_node, "pcie") {
		pcie->perst_gpio = devm_fwnode_gpiod_get(dev, of_fwnode_handle(pcie_port_node),
							 /* [한국어] GPIOD_OUT_HIGH 로 얻는다. PERST# 는 active-low 라 **얻는 순간
							  * 리셋이 걸린 상태** 가 되며, 나중에 add_pcie_port 가 그것을 푼다. */
							 "reset", GPIOD_OUT_HIGH, NULL);
		if (IS_ERR(pcie->perst_gpio))
			/* [한국어] dev_err_probe 를 쓰므로 -EPROBE_DEFER 는 조용히 넘어간다. */
			return dev_err_probe(dev, PTR_ERR(pcie->perst_gpio),
					     "Failed to request reset GPIO\n");
		return 0;
	}

	return -ENODEV;
}

/* [한국어]
 * amd_mdb_add_pcie_port - 창을 잡고 인터럽트를 세운 뒤 PERST# 를 풀고 호스트를 등록한다
 *
 * @pcie: 드라이버 상태.
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * probe 의 실질적인 본체다. 다섯 단계다.
 * 1. SLCR(시스템 수준 제어·상태) 레지스터 창을 **이름으로** 매핑한다.
 *    이 파일의 모든 인터럽트 레지스터가 이 창 안에 있다.
 * 2. 인터럽트 도메인 둘을 만든다.
 * 3. 원인별 인터럽트와 핸들러 셋을 건다.
 * 4. 콜백 표를 코어에 건다 — **비어 있는 표** 다.
 * 5. PERST# 가 있으면 리셋을 풀고, 없으면 그대로 진행한다.
 * 6. DWC 호스트를 등록한다.
 *
 * 5번의 대기 둘이 규격에서 온 값이다.
 *   - 푸는 **전** 에 PCIE_T_PVPERL_MS(drivers/pci/pci.h, 100ms) — 전원이
 *     안정된 뒤 PERST# 를 풀기까지 지켜야 하는 최소 시간.
 *   - 푼 **뒤** 에 PCIE_RESET_CONFIG_WAIT_MS(같은 헤더, 100ms) — 리셋 뒤
 *     첫 config 요청을 보내기까지의 최소 시간.
 *
 * [상류 코드 관찰] 그 두 대기가 msleep 이 아니라 **mdelay** 다. 즉 CPU 를
 * 놓지 않고 도합 200ms 를 바쁜 대기한다. 바로 옆의 GPIO 조작은 잠들 수
 * 있는 판(cansleep)을 쓰는데, 대기만 바쁜 대기인 셈이다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 되감기가 한 갈래다 — 도메인만 없앤다. 나머지는 모두 devm 판이다.
 * 1번과 2번의 실패는 되감을 것이 없어 곧바로 돌아간다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트이며 200ms 바쁜 대기와 링크
 * 대기, 버스 스캔으로 오래 걸린다.
 *
 * 에러 경로: 각 단계의 실패를 기록과 함께 올려보내며, 3번과 6번의 실패만
 * 도메인 되감기를 거친다.
 *
 * 호출 체인:
 *   amd_mdb_pcie_probe() → [이 함수]
 *     → devm_platform_ioremap_resource_byname()
 *     → amd_mdb_pcie_init_irq_domains() → amd_mdb_setup_irq()
 *     → gpiod_set_value_cansleep() → dw_pcie_host_init()
 */
static int amd_mdb_add_pcie_port(struct amd_mdb_pcie *pcie,
				 struct platform_device *pdev)
{
	struct dw_pcie *pci = &pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 오류 기록의 기준이 될 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 각 단계의 반환값. */
	int err;

	pcie->slcr = devm_platform_ioremap_resource_byname(pdev, "slcr");
	/* [한국어] SLCR 창을 못 잡았다. */
	if (IS_ERR(pcie->slcr))
		/* [한국어] 아직 만든 것이 없어 곧바로 돌아간다. */
		return PTR_ERR(pcie->slcr);

	err = amd_mdb_pcie_init_irq_domains(pcie, pdev);
	/* [한국어] 도메인을 못 만들었다. */
	if (err)
		/* [한국어] 그 함수가 이미 되감았으므로 오류만 올려보낸다. */
		return err;

	err = amd_mdb_setup_irq(pcie, pdev);
	/* [한국어] 인터럽트 설정이 실패했다. */
	if (err) {
		/* [한국어] 실패를 알린다. */
		dev_err(dev, "Failed to set up interrupts, err=%d\n", err);
		/* [한국어] 도메인을 되감는 라벨로 간다. */
		goto out;
	}

	pp->ops = &amd_mdb_pcie_host_ops;

	if (pcie->perst_gpio) {
		/* [한국어] 푸는 **전** 에 규격이 정한 최소 시간을 지킨다. mdelay 라 CPU 를
		 * 놓지 않는 바쁜 대기다. */
		mdelay(PCIE_T_PVPERL_MS);
		gpiod_set_value_cansleep(pcie->perst_gpio, 0);
		/* [한국어] 푼 **뒤** 에도 기다린다. 리셋 뒤 첫 config 요청을 보내기까지의
		 * 최소 시간이며, 이것도 바쁜 대기다. */
		mdelay(PCIE_RESET_CONFIG_WAIT_MS);
	}

	/* [한국어] DWC 코어에 넘겨 링크 훈련과 버스 스캔을 시작한다. */
	err = dw_pcie_host_init(pp);
	/* [한국어] 호스트 등록이 실패했다. */
	if (err) {
		/* [한국어] 실패를 알린다. */
		dev_err(dev, "Failed to initialize host, err=%d\n", err);
		/* [한국어] 도메인을 되감는 라벨로 간다. */
		goto out;
	}

	/* [한국어] 여기까지 오면 컨트롤러가 완전히 동작한다. */
	return 0;

out:
	amd_mdb_pcie_free_irq_domains(pcie);
	return err;
}

/* [한국어]
 * amd_mdb_pcie_probe - PERST# GPIO 를 두 자리에서 찾아 본 뒤 포트를 세운다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이며, **PERST# GPIO 를 찾는 두 갈래** 가 이 함수의
 * 전부라 해도 좋다.
 *
 * 1. 상태 구조체를 할당하고 device 와 drvdata 를 심는다.
 * 2. 자식 노드("pcie" 로 시작하는)에서 GPIO 를 찾아 본다.
 * 3. 그 결과가 -ENODEV 면 — 즉 그런 자식 노드가 없으면 — **자기 노드에서
 *    다시 찾는다.** 옆의 상류 주석이 밝히듯 이것은 실패가 아니라
 *    대체 경로다. 옛 디바이스 트리는 브리지 노드를 따로 두지 않고
 *    호스트 브리지 노드에 reset GPIO 를 직접 적었기 때문이다.
 *    그 밖의 오류는 그대로 올려보낸다.
 * 4. 포트를 세운다.
 *
 * 3번이 optional 판을 쓴다는 점이 중요하다. GPIO 가 없어도 오류가 아니라
 * NULL 이 되며, add_pcie_port 가 그 경우 리셋 조작을 건너뛴다. 즉
 * **PERST# 가 아예 없는 보드도 지원된다.**
 *
 * 되감기 코드가 없다 — 잡는 것이 모두 devm 판이다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 할당 실패는 -ENOMEM, GPIO 확보 실패는 그 오류, 그 밖은
 * amd_mdb_add_pcie_port() 의 결과를 그대로 돌려준다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → devm_kzalloc() → platform_set_drvdata()
 *     → amd_mdb_parse_pcie_port() → devm_gpiod_get_optional()
 *     → amd_mdb_add_pcie_port()
 */
static int amd_mdb_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 오류 기록과 자원 확보의 기준이 될 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 이 드라이버의 상태 구조체. */
	struct amd_mdb_pcie *pcie;
	/* [한국어] 그 안의 DWC 문맥을 가리킬 지름길. */
	struct dw_pcie *pci;
	/* [한국어] GPIO 탐색의 결과. */
	int ret;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* [한국어] 메모리 부족이다. */
	if (!pcie)
		/* [한국어] 아직 잡은 것이 없어 곧바로 돌아간다. */
		return -ENOMEM;

	pci = &pcie->pci;
	/* [한국어] 코어가 오류를 기록할 때 쓸 device 를 심는다. */
	pci->dev = dev;

	/* [한국어] **아무것도 잡기 전에** drvdata 를 매단다. 이 파일은 dw_pcie 를 맨 앞에
	 * 두어 container_of 로도 되돌릴 수 있지만, 그 경로를 쓰지 않는다. */
	platform_set_drvdata(pdev, pcie);

	/* [한국어] 먼저 자식 노드에서 PERST# GPIO 를 찾아 본다. */
	ret = amd_mdb_parse_pcie_port(pcie);
	/*
	 * If amd_mdb_parse_pcie_port returns -ENODEV, it indicates that the
	 * PCIe Bridge node was not found in the device tree. This is not
	 * considered a fatal error and will trigger a fallback where the
	 * reset GPIO is acquired directly from the PCIe Host Bridge node.
	 */
	/* [한국어] 찾지 못했다. 두 갈래로 나뉜다. */
	if (ret) {
		/* [한국어] -ENODEV 가 아닌 오류다. 즉 노드는 있는데 GPIO 확보가 실패한 경우이며,
		 * 이것은 진짜 오류다. */
		if (ret != -ENODEV)
			/* [한국어] 그대로 올려보낸다. */
			return ret;

		/* [한국어] -ENODEV 였다. 옆의 상류 주석대로 이것은 실패가 아니라 **대체 경로의
		 * 신호** 다 — 자기 노드에서 GPIO 를 다시 찾는다. optional 판이라
		 * GPIO 가 아예 없어도 오류가 아니고 NULL 이 된다. */
		pcie->perst_gpio = devm_gpiod_get_optional(dev, "reset",
							   GPIOD_OUT_HIGH);
		/* [한국어] 확보 자체가 실패했다. */
		if (IS_ERR(pcie->perst_gpio))
			/* [한국어] dev_err_probe 로 기록을 남기며 그 오류를 올려보낸다. */
			return dev_err_probe(dev, PTR_ERR(pcie->perst_gpio),
					     "Failed to request reset GPIO\n");
	}

	/* [한국어] GPIO 유무가 정해졌으니 포트를 세운다. */
	return amd_mdb_add_pcie_port(pcie, pdev);
}

static const struct of_device_id amd_mdb_pcie_of_match[] = {
	{
		/* [한국어] 이 드라이버가 지원하는 유일한 SoC. */
		.compatible = "amd,versal2-mdb-host",
	},
	/* [한국어] 표의 끝 표시. */
	{},
};

static struct platform_driver amd_mdb_pcie_driver = {
	/* [한국어] 드라이버 이름과 매칭 표, 바인딩 제한. */
	.driver = {
		/* [한국어] sysfs 에 보일 이름. */
		.name	= "amd-mdb-pcie",
		/* [한국어] 위의 디바이스 트리 매칭 표를 건다. */
		.of_match_table = amd_mdb_pcie_of_match,
		.suppress_bind_attrs = true,
	},
	/* [한국어] 장치가 붙을 때 부를 진입점. remove 는 두지 않았다. */
	.probe = amd_mdb_pcie_probe,
};

builtin_platform_driver(amd_mdb_pcie_driver);
