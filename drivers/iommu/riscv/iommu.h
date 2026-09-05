/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright © 2022-2024 Rivos Inc.
 * Copyright © 2023 FORTH-ICS/CARV
 *
 * Authors
 *	Tomasz Jeznach <tjeznach@rivosinc.com>
 *	Nick Kossifidis <mick@ics.forth.gr>
 */

/*
 * [한국어 설명] RISC-V IOMMU 드라이버의 자료 모델 헤더 (riscv/iommu.h)
 *
 * === 파일의 역할 ===
 * RISC-V 규격이 정한 IOMMU 를 다루는 드라이버의 커널 쪽 자료 구조를
 * 정의한다. 하드웨어 큐 하나(riscv_iommu_queue)와 IOMMU 장치 하나
 * (riscv_iommu_device), 그리고 레지스터 접근 매크로가 전부다.
 * 하드웨어 규격 값들은 짝이 되는 iommu-bits.h 에 따로 있다.
 *
 * RISC-V IOMMU 의 구조는 ARM SMMU v3 와 닮았다. 장치는 device ID 로
 * 식별되고, 그 번호로 장치 디렉터리(DDT)를 걸어가 장치 문맥을 찾고,
 * 그 문맥이 페이지 테이블을 가리킨다. 커널과 하드웨어는 두 개의 링
 * 큐로 대화한다 — 명령 큐로 무효화를 보내고, 폴트 큐로 오류를 받는다.
 *
 * 이 헤더의 큐 구조가 흥미롭다. 생산·소비 포인터를 세 개의 원자 변수로
 * 나눠 두었는데, 여러 CPU 가 락 없이 자리를 차지하고 순서대로 발표할 수
 * 있게 하려는 설계다 — SMMU v3 의 valid_map 과 목적은 같고 방식이 다르다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 장치가 낸 DMA 는 이렇게 걸어간다:
 *
 *   장치 (device ID [+ process ID])
 *     → 장치 디렉터리(DDT)에서 장치 문맥
 *     → (선택적으로) 프로세스 디렉터리에서 프로세스 문맥
 *     → 페이지 테이블 (RISC-V Sv39/Sv48/Sv57)
 *     → 물리 주소
 *
 * 커널이 하드웨어에게 말을 거는 길은 명령 큐 하나이고, 하드웨어가
 * 커널에게 말을 거는 길은 폴트 큐와 인터럽트다.
 * 실행 컨텍스트는 여러 겹이다 — 프로브와 붙이기는 프로세스 문맥,
 * 무효화는 원자적 문맥, 폴트 처리는 인터럽트 스레드에서 돈다.
 *
 * === 타 모듈과의 연결 ===
 * - iommu-bits.h: 레지스터 오프셋과 비트 정의, 명령·폴트 기록의 형식.
 * - iommu.c: 여기 정의된 구조체를 실제로 다루는 드라이버 본체.
 * - iommu-pci.c / iommu-platform.c: 이 IOMMU 가 PCI 장치로 나타나는
 *   경우와 플랫폼 장치로 나타나는 경우의 프로브 진입점. 둘 다
 *   riscv_iommu_init() 으로 모인다.
 * - iommu 코어: iommu_device 를 통해 만난다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct riscv_iommu_queue: 명령 큐와 폴트 큐가 공유하는 링 버퍼 구조.
 *   세 원자 변수로 락 없는 삽입과 순서 있는 발표를 함께 만든다.
 * - struct riscv_iommu_device: IOMMU 하드웨어 하나. 레지스터 창, 능력,
 *   인터럽트, 두 큐, 그리고 장치 디렉터리를 쥔다.
 * - riscv_iommu_init()/remove()/disable(): 프로브 진입점들이 공유하는
 *   초기화·정리 함수.
 * - riscv_iommu_readl/readq/writel/writeq 매크로: 레지스터 접근을 짧게 쓰기 위한 것.
 */

#ifndef _RISCV_IOMMU_H_	/* [한국어] 이 헤더가 두 번 펼쳐지는 것을 막는 보호 매크로. */
#define _RISCV_IOMMU_H_	/* [한국어] 처음 펼쳐질 때 표시를 남긴다. */

#include <linux/iommu.h>	/* [한국어] iommu 코어의 iommu_device 등. */
#include <linux/types.h>	/* [한국어] 기본 정수 타입. */
#include <linux/iopoll.h>	/* [한국어] 아래 폴링 매크로가 쓰는 readx_poll_timeout. */

#include "iommu-bits.h"	/* [한국어] 하드웨어 규격 값 — 아래 배열 크기(RISCV_IOMMU_INTR_COUNT)도 여기서 온다. */

struct riscv_iommu_device;	/* [한국어] 아래 큐 구조가 이것을 가리키므로 전방 선언이 필요하다. */

/* [한국어] 하드웨어와 주고받는 링 큐 하나 — 명령 큐와 폴트 큐가 같은 구조를 쓴다.
 *
 * 포인터를 세 개로 나눈 것이 이 설계의 요점이다. 여러 CPU 가 동시에
 * 명령을 넣을 때, 자리를 차지하는 일(prod)과 실제로 쓴 내용을 발표하는
 * 일(tail)을 분리해야 락 없이 순서를 지킬 수 있다. head 는 하드웨어가
 * 어디까지 처리했는지 커널이 아는 값이다.
 * 세 값 모두 "무한히 증가하는" 번호이고, 실제 링 첨자는 mask 로 자른다 —
 * 그러면 한 바퀴 돌았는지를 따로 표시할 필요가 없다. */
struct riscv_iommu_queue {
	atomic_t prod;				/* unbounded producer allocation index */
	/* [한국어] (위 영어 주석 참고) 다음에 나눠 줄 자리 번호.
	 * 설정자: 명령을 넣으려는 CPU 가 원자적으로 올려 자기 자리를 차지한다.
	 * 읽는 자: 자리를 차지하는 모든 CPU.
	 * 값 범위: 무한히 증가한다 — 실제 첨자는 mask 로 자른다.
	 * 동기화: 원자 연산으로만 다룬다. */
	atomic_t head;				/* unbounded shadow ring buffer consumer index */
	/* [한국어] (위 영어 주석 참고) 하드웨어가 어디까지 처리했는지 커널이 아는 값.
	 * 설정자: 레지스터를 읽어 갱신한다.
	 * 읽는 자: 큐에 빈 자리가 있는지 계산할 때.
	 * 값 범위: tail 보다 뒤에 있다.
	 * 동기화: 원자 연산. */
	atomic_t tail;				/* unbounded shadow ring buffer producer index */
	/* [한국어] (위 영어 주석 참고) 실제로 하드웨어에 발표된 자리 번호.
	 * 설정자: 자기 자리에 명령을 다 쓴 CPU 가, 앞선 CPU 들이 모두 끝나기를
	 *         기다렸다가 여기까지 올리고 레지스터에 알린다.
	 * 읽는 자: 발표 순서를 지켜야 하는 모든 CPU.
	 * 값 범위: prod 보다 앞서지 않는다 — 그 차이가 곧 "쓰는 중인 자리"다.
	 * 동기화: 원자 연산. */
	unsigned int mask;			/* index mask, queue length - 1 */
	/* [한국어] (위 영어 주석 참고) 무한 번호를 링 첨자로 자르는 마스크.
	 * 설정자: 큐를 잡을 때 (길이-1)로 정한다 — 길이가 2의 거듭제곱이라 성립한다.
	 * 읽는 자: 번호를 첨자로 바꾸는 모든 곳.
	 * 값 범위: 2^n - 1.
	 * 동기화: 불변. */
	unsigned int irq;			/* allocated interrupt number */
	/* [한국어] (위 영어 주석 참고) 이 큐에 배정된 인터럽트 번호.
	 * 설정자: 프로브가 MSI 또는 배선 인터럽트에서 얻는다.
	 * 읽는 자: 처리기를 걸고 뗄 때.
	 * 값 범위: 0 이면 아직 배정되지 않았다.
	 * 동기화: 불변. */
	struct riscv_iommu_device *iommu;	/* iommu device handling the queue when active */
	/* [한국어] (위 영어 주석 참고) 이 큐를 가진 하드웨어.
	 * 설정자: 큐를 켤 때 채운다.
	 * 읽는 자: 인터럽트 처리기가 이 포인터로 문맥을 되찾는다.
	 * 값 범위: NULL 이면 아직 켜지지 않은 큐다.
	 * 동기화: 켠 뒤 불변. */
	void *base;				/* ring buffer kernel pointer */
	/* [한국어] (위 영어 주석 참고) 링 버퍼의 커널 주소.
	 * 설정자: 프로브가 dma 로 잡아 채운다.
	 * 읽는 자: 명령을 쓰거나 폴트를 읽는 모든 곳.
	 * 값 범위: NULL 이면 큐가 없다.
	 * 동기화: 불변. */
	dma_addr_t phys;			/* ring buffer physical address */
	/* [한국어] (위 영어 주석 참고) 그 버퍼의 장치 쪽 주소.
	 * 설정자: 위와 같은 자리.
	 * 읽는 자: 하드웨어에 큐 위치를 알릴 때.
	 * 값 범위: 큐 크기에 맞춰 정렬되어야 한다.
	 * 동기화: 불변. */
	u16 qbr;				/* base register offset, head and tail reference */
	/* [한국어] (위 영어 주석 참고) 이 큐의 기준 레지스터 오프셋.
	 * 설정자: 큐를 만들 때 명령 큐용·폴트 큐용 값을 각각 준다.
	 * 읽는 자: 포인터 레지스터 주소를 계산할 때 — 그 뒤에 head/tail 이 이어진다.
	 * 값 범위: 규격이 정한 오프셋.
	 * 동기화: 불변.
	 *         (이 필드 덕분에 두 큐가 같은 코드를 공유할 수 있다.) */
	u16 qcr;				/* control and status register offset */
	/* [한국어] (위 영어 주석 참고) 이 큐의 제어·상태 레지스터 오프셋.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: 규격이 정한 오프셋.
	 * 동기화: 불변. */
	u8 qid;					/* queue identifier, same as RISCV_IOMMU_INTR_XX */
	/* [한국어] (위 영어 주석 참고) 큐 번호 — 인터럽트 번호 배열의 첨자로도 쓰인다.
	 * 설정자: 큐를 만들 때.
	 * 읽는 자: 인터럽트를 배정하고 로그를 찍을 때.
	 * 값 범위: RISCV_IOMMU_INTR_* 중 하나.
	 * 동기화: 불변. */
};

/* [한국어] RISC-V IOMMU 하드웨어 하나.
 *
 * 레지스터 창, 능력 비트, 인터럽트, 두 개의 큐, 그리고 장치 디렉터리가
 * 전부다. SMMU v3 와 견주면 훨씬 단출한데, 이 규격이 더 나중에 만들어져
 * 필요한 것만 남겼기 때문이다. */
struct riscv_iommu_device {
	/* iommu core interface */
	/* [한국어] (위 영어 주석 참고) iommu 코어가 아는 장치 몸통.
	 * 설정자: 프로브가 코어에 등록하며 채운다.
	 * 읽는 자: 코어가 이 드라이버를 부를 때의 기준점.
	 * 값 범위: 등록 전에는 비어 있다.
	 * 동기화: 코어의 규칙을 따른다. */
	struct iommu_device iommu;

	/* iommu hardware */
	/* [한국어] (위 영어 주석 참고) 이 IOMMU 의 커널 장치.
	 * 설정자: 프로브.
	 * 읽는 자: 로그와 dma 할당.
	 * 값 범위: NULL 일 수 없다.
	 * 동기화: 불변. */
	struct device *dev;

	/* hardware control register space */
	/* [한국어] (위 영어 주석 참고) 레지스터 창이 매핑된 커널 주소.
	 * 설정자: 프로브가 ioremap 으로 얻는다.
	 * 읽는 자: 아래 접근 매크로의 기준점.
	 * 값 범위: NULL 이면 프로브가 실패한 것이다.
	 * 동기화: 불변. */
	void __iomem *reg;

	/* supported and enabled hardware capabilities */
	/* [한국어] (위 영어 주석 참고) 이 하드웨어가 무엇을 지원하는가.
	 * 설정자: 프로브가 능력 레지스터를 읽어 담는다.
	 * 읽는 자: 페이지 테이블 형식이나 기능을 고르는 모든 곳.
	 * 값 범위: RISCV_IOMMU_CAPABILITIES_* 비트 조합.
	 * 동기화: 프로브 이후 불변. */
	u64 caps;
	/* [한국어] 지금 켜져 있는 기능 설정.
	 * 설정자: 프로브가 하드웨어에 쓰고 그 값을 기억한다.
	 * 읽는 자: 엔디안이나 인터럽트 방식 같은 동작을 판단할 때.
	 * 값 범위: RISCV_IOMMU_FCTL_* 비트 조합.
	 * 동기화: 프로브 이후 불변. */
	u32 fctl;

	/* available interrupt numbers, MSI or WSI */
	/* [한국어] (위 영어 주석 참고) 이 하드웨어가 쓸 수 있는 인터럽트 번호들.
	 * 설정자: 프로브가 MSI 를 배정받거나 배선 인터럽트를 찾아 채운다.
	 * 읽는 자: 큐마다 자기 번호를 골라 처리기를 건다.
	 * 값 범위: 큐 종류별로 하나씩.
	 * 동기화: 불변. */
	unsigned int irqs[RISCV_IOMMU_INTR_COUNT];
	/* [한국어] 실제로 배정받은 인터럽트 개수.
	 * 설정자: 프로브.
	 * 읽는 자: 위 배열을 훑는 반복문의 한계.
	 * 값 범위: 0 이면 인터럽트 없이 동작한다 — 폴트를 알 수 없게 된다.
	 * 동기화: 불변. */
	unsigned int irqs_count;
	/* [한국어] 어느 사건을 어느 인터럽트로 보낼지 정하는 벡터 설정.
	 * 설정자: 프로브가 배정 결과에 맞춰 계산해 하드웨어에 쓴다.
	 * 읽는 자: 그 설정을 되짚어 볼 때.
	 * 값 범위: 사건마다 인터럽트 번호를 담은 비트 묶음.
	 * 동기화: 불변. */
	unsigned int icvec;

	/* hardware queues */
	/* [한국어] (위 영어 주석 참고) 커널이 하드웨어에게 말을 거는 길.
	 * 설정자: 프로브가 링 버퍼를 잡고 켠다.
	 * 읽는 자: 무효화를 내는 모든 경로.
	 * 값 범위: 언제나 있어야 한다 — 없으면 무효화를 낼 수 없다.
	 * 동기화: 큐 내부의 원자 변수들이 지킨다. */
	struct riscv_iommu_queue cmdq;
	/* [한국어] 하드웨어가 커널에게 오류를 알리는 길.
	 * 설정자: 프로브가 링 버퍼를 잡고 켠다.
	 * 읽는 자: 폴트 인터럽트 스레드가 소비 포인터를 밀며 읽는다.
	 * 값 범위: 위와 같다.
	 * 동기화: 소비자가 하나뿐이라 읽는 쪽에는 경합이 없다. */
	struct riscv_iommu_queue fltq;

	/* device directory */
	/* [한국어] (위 영어 주석 참고) 장치 디렉터리를 몇 단계로 만들었는가.
	 * 설정자: 프로브가 하드웨어 능력과 device ID 폭을 보고 정한다.
	 * 읽는 자: 장치 문맥을 찾아갈 때 몇 번 걸어야 하는지 정한다.
	 * 값 범위: 1~3 단계, 또는 "변환하지 않음"을 뜻하는 값.
	 * 동기화: 프로브 이후 불변. */
	unsigned int ddt_mode;
	/* [한국어] 그 디렉터리 뿌리의 장치 쪽 주소.
	 * 설정자: 프로브가 잡으며 얻는다.
	 * 읽는 자: 하드웨어에 디렉터리 위치를 알릴 때.
	 * 값 범위: 페이지 경계에 정렬되어야 한다.
	 * 동기화: 불변. */
	dma_addr_t ddt_phys;
	/* [한국어] 그 뿌리의 커널 주소.
	 * 설정자: 위와 같은 자리.
	 * 읽는 자: 장치를 붙일 때 디렉터리를 걸어 내려가며 항목을 채운다.
	 * 값 범위: NULL 이면 아직 잡히지 않았다.
	 * 동기화: 아래 단계를 새로 다는 일은 원자적 교체로 한다. */
	u64 *ddt_root;
};

int riscv_iommu_init(struct riscv_iommu_device *iommu);	/* [한국어] PCI·플랫폼 두 프로브 경로가 공유하는 초기화 — 큐를 잡고 디렉터리를 만들고 코어에 등록한다. */
void riscv_iommu_remove(struct riscv_iommu_device *iommu);	/* [한국어] 그 역순으로 자원을 거둔다. */
void riscv_iommu_disable(struct riscv_iommu_device *iommu);	/* [한국어] 변환과 큐를 멈춘다 — 종료나 초기화 실패 때 쓴다. */

/* [한국어] 레지스터 접근을 짧게 쓰기 위한 매크로들.
 *
 * relaxed 판을 쓰는 이유는, 이 드라이버가 순서가 필요한 자리마다 장벽을
 * 명시적으로 넣기 때문이다. 매크로에 순서를 숨겨 두면 어디서 어떤 순서가
 * 보장되는지 읽기 어려워진다. */
#define riscv_iommu_readl(iommu, addr) \
	readl_relaxed((iommu)->reg + (addr))	/* [한국어] 32비트 레지스터를 읽는다. */

#define riscv_iommu_readq(iommu, addr) \
	readq_relaxed((iommu)->reg + (addr))	/* [한국어] 64비트 레지스터를 읽는다 — 큐 주소처럼 넓은 값에 쓴다. */

#define riscv_iommu_writel(iommu, addr, val) \
	writel_relaxed((val), (iommu)->reg + (addr))	/* [한국어] 32비트 레지스터에 쓴다. */

#define riscv_iommu_writeq(iommu, addr, val) \
	writeq_relaxed((val), (iommu)->reg + (addr))	/* [한국어] 64비트 레지스터에 쓴다. */

#define riscv_iommu_readq_timeout(iommu, addr, val, cond, delay_us, timeout_us) \
	readx_poll_timeout(readq_relaxed, (iommu)->reg + (addr), val, cond, \
			   delay_us, timeout_us)	/* [한국어] 조건이 참이 될 때까지 64비트 레지스터를 반복해 읽는다 — 설정이 반영되기를 기다릴 때 쓴다. */

#define riscv_iommu_readl_timeout(iommu, addr, val, cond, delay_us, timeout_us) \
	readx_poll_timeout(readl_relaxed, (iommu)->reg + (addr), val, cond, \
			   delay_us, timeout_us)	/* [한국어] 32비트 판. */

#endif
