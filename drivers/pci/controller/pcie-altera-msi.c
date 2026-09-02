// SPDX-License-Identifier: GPL-2.0
/*
 * Altera PCIe MSI support
 *
 * Author: Ley Foon Tan <lftan@altera.com>
 *
 * Copyright Altera Corporation (C) 2013-2015. All rights reserved
 */

/*
 * [한국어 설명] Altera(현 Intel) FPGA PCIe 코어의 MSI 컨트롤러 (pcie-altera-msi.c)
 *
 * === 파일의 역할 ===
 * Altera FPGA 안에 구현된 PCIe 하드 IP 가 내보내는 MSI 를 리눅스 인터럽트로
 * 바꿔 주는 드라이버다. 이 파일은 호스트 브리지 자체를 다루지 않는다 —
 * config 접근도, 링크 훈련도, 자원 배정도 하지 않고, **오직 MSI 만** 맡는다.
 * 브리지 쪽은 pcie-altera.c 가 따로 담당하며, 디바이스 트리에서 두 노드가
 * msi-parent 로 이어진다.
 *
 * 이 하드웨어의 MSI 방식이 이 파일의 형태를 정한다. 장치가 MSI 를 쓰려면
 * "이 주소에 이 값을 써라" 를 알아야 하는데, 여기서는 그 주소가 벡터마다
 * 4바이트씩 떨어진 별도의 메모리 창(vector_slave)이다. 장치가 그 창의
 * 어느 자리에 쓰면 하드웨어가 그것을 벡터 번호로 해석해 상태 비트를 세우고,
 * 요약 인터럽트 하나를 올린다.
 *
 * 그래서 이 파일이 하는 일이 셋이다 — 벡터 번호를 배정하고(비트맵),
 * 그 번호에 해당하는 주소를 장치에 알려 주고(compose_msi_msg),
 * 요약 인터럽트가 오면 상태 레지스터를 읽어 어느 벡터였는지 가려낸다(isr).
 *
 * 벡터 수는 최대 32 개이며 실제 개수는 디바이스 트리의 num-vectors 가 정한다.
 * FPGA 설계 시점에 정해지는 값이라 빌드마다 다를 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 드라이버는 **인터럽트 도메인 계층의 부모** 로 앉는다. 계층이 아래에서
 * 위로 이렇게 쌓인다.
 *
 *   장치 드라이버(예: nvme)가 pci_alloc_irq_vectors() 호출
 *     -> drivers/pci/msi/ 가 장치별 MSI 도메인을 만든다
 *        -> [이 파일] 이 만든 부모 도메인에 벡터 할당을 요청
 *           -> altera_irq_domain_alloc() 이 비트맵에서 빈 번호를 고르고
 *              MSI_INTMASK 의 그 비트를 켠다
 *        -> altera_compose_msi_msg() 가 주소·데이터를 만들어 돌려주고,
 *           PCI 코어가 그것을 장치의 MSI capability 에 쓴다
 *
 * 인터럽트가 올라오는 방향은 반대다.
 *
 *   장치가 vector_slave 창의 자기 자리에 DMA 쓰기
 *     -> 하드웨어가 MSI_STATUS 의 해당 비트를 세우고 요약 인터럽트를 올림
 *        -> [이 파일] altera_msi_isr() 이 체인 핸들러로 불림
 *           -> 상태의 각 비트마다 generic_handle_domain_irq()
 *              -> 그 벡터에 등록된 장치 드라이버의 핸들러
 *
 * 실행 컨텍스트: probe 와 도메인 할당·해제는 프로세스 컨텍스트,
 * altera_msi_isr() 은 인터럽트 문맥이다. 그 둘이 MSI_INTMASK 레지스터를
 * 함께 건드리지 않는 것이 중요한데, isr 은 STATUS 만 읽고 MASK 는 건드리지
 * 않으므로 실제로 겹치지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/msi/ 의 도메인 계층. 이 파일이 만든 부모 도메인 위에
 *   장치별 MSI-X 도메인이 얹힌다. altera_msi_parent_ops 가 그 접점이며,
 *   거기 적힌 플래그가 "이 부모가 무엇을 지원하는가" 를 위층에 알린다.
 * 아래쪽: irqchip 코어(chained_irq_enter/exit), irqdomain 코어,
 *   그리고 FPGA 안의 PCIe 하드 IP 레지스터 두 벌(csr, vector_slave).
 * 옆쪽: pcie-altera.c 가 같은 하드웨어의 브리지 쪽을 맡는다. 두 드라이버가
 *   코드로는 서로를 부르지 않고, 디바이스 트리의 msi-parent 참조로만 이어진다.
 *
 * 데이터 흐름:
 *   디바이스 트리(num-vectors, csr, vector_slave, 인터럽트) -> probe
 *     -> struct altera_msi -> 도메인 등록
 *   벡터 번호 <-> 물리 주소의 변환이 이 파일의 핵심 데이터 변환이다.
 *   벡터 n 의 주소 = vector_phy + n * 4 이며, 그 역방향이 isr 의 비트 위치다.
 *
 * 공유 상태: struct altera_msi 하나. used 비트맵만 잠금이 필요하고
 *   나머지는 probe 후 불변이다.
 *
 * === NVMe 관점 ===
 * NVMe 컨트롤러가 이 FPGA 브리지 아래에 붙으면, nvme_probe() 의
 * pci_alloc_irq_vectors() 가 결국 이 파일의 altera_irq_domain_alloc() 을
 * 큐 수만큼 부른다. 벡터가 최대 32 개뿐이라 NVMe 가 요청하는 만큼을 다
 * 받지 못할 수 있고, 그때 코어가 min 과 max 사이에서 협상해 큐 수를 줄인다.
 * 이 파일이 -ENOSPC 를 돌려주는 자리가 그 협상의 근거가 된다.
 *
 * === 주요 함수/구조체 요약 ===
 * altera_msi_isr()            : 요약 인터럽트를 받아 상태 비트마다 갈라 보낸다.
 *                               이 파일에서 인터럽트 문맥에 있는 유일한 함수다.
 * altera_compose_msi_msg()    : 벡터 번호를 장치가 쓸 주소·데이터로 바꾼다.
 * altera_irq_domain_alloc()   : 비트맵에서 벡터를 배정하고 마스크를 연다.
 * altera_irq_domain_free()    : 그 반대. 이미 해제된 벡터를 잡아낸다.
 * altera_msi_probe()          : 두 레지스터 창과 벡터 수를 얻어 도메인을 세운다.
 * struct altera_msi           : 이 드라이버의 상태 전부. 벡터 비트맵과
 *                               두 레지스터 창, 그리고 벡터 창의 물리 주소.
 */

/* [한국어] request_irq 계열과 irqreturn_t. 이 파일은 체인 핸들러를 쓰지만 그 선언들이 여기 있다. */
#include <linux/interrupt.h>
/* [한국어] chained_irq_enter()/exit(). 상위 컨트롤러의 인터럽트를 받아 하위로 나눠 주는
 * 체인 핸들러의 진입·퇴출 관용구다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] msi_lib_init_dev_msi_info(). 부모 규약에서 위층 도메인 초기화를 이 라이브러리에 맡긴다. */
#include <linux/irqchip/irq-msi-lib.h>
/* [한국어] irq_domain 타입과 irq_domain_set_info() 등. 이 파일의 뼈대다. */
#include <linux/irqdomain.h>
/* [한국어] __init / __exit 표시. */
#include <linux/init.h>
/* [한국어] module_exit 과 MODULE_ 매크로들. */
#include <linux/module.h>
/* [한국어] struct msi_msg 와 msi_parent_ops. 이 파일이 만드는 메시지의 형식이 여기 있다. */
#include <linux/msi.h>
/* [한국어] of_ 주소 헬퍼. 이 파일에서 직접 쓰는 것은 없으나 상류가 포함해 두었다. */
#include <linux/of_address.h>
/* [한국어] of_ PCI 헬퍼. 역시 이 파일에서 직접 쓰는 것은 없다. */
#include <linux/of_pci.h>
/* [한국어] PCI 코어 타입들. */
#include <linux/pci.h>
/* [한국어] struct platform_device 와 platform_get_irq() 등. 이 드라이버가 플랫폼 드라이버다. */
#include <linux/platform_device.h>
/* [한국어] 할당 계열. 이 파일은 devm 판만 쓴다. */
#include <linux/slab.h>

/* [한국어] 상태 레지스터 — 각 비트가 한 벡터의 대기 여부다. isr 이 이것을 읽어 갈라 보낸다. */
#define MSI_STATUS		0x0
/* [한국어] 오류 레지스터. 이 파일에서 읽는 곳은 없다(전수 확인). */
#define MSI_ERROR		0x4
/* [한국어] 마스크 레지스터 — 각 비트가 한 벡터의 허용 여부다.
 * 할당이 비트를 켜고 해제가 끈다. */
#define MSI_INTMASK		0x8

/* [한국어] 이 하드웨어가 가질 수 있는 벡터의 상한. 상태·마스크 레지스터가 32비트라
 * 그것이 곧 상한이 된다. 실제 개수는 디바이스 트리의 num-vectors 가 정하며
 * 이 값 이하다. */
#define MAX_MSI_VECTORS		32

/* [한국어] 이 드라이버의 상태 전부. */
struct altera_msi {
	/* [한국어] 어느 벡터가 쓰이고 있는지의 비트맵.
	 * 설정자: altera_irq_domain_alloc() 이 set_bit 으로 세우고,
	 * altera_irq_domain_free() 가 __clear_bit 으로 지운다.
	 * 읽는 자: 두 함수가 각각 빈 자리 탐색과 이중 해제 검사에 쓴다.
	 * 값 범위: 비트 n 이 서 있으면 벡터 n 이 배정된 상태다.
	 * 동기화: 아래 lock 이 지킨다. 인터럽트 핸들러는 이 비트맵을 보지 않으므로
	 * 뮤텍스로 충분하다 — isr 은 STATUS 레지스터만 읽는다. */
	DECLARE_BITMAP(used, MAX_MSI_VECTORS);
	/* [한국어] 위 비트맵을 지키는 뮤텍스(옆의 상류 주석).
	 * 설정자·읽는 자: 할당과 해제 두 함수만 잡는다.
	 * 값 범위: 뮤텍스.
	 * 동기화: 스핀락이 아니라 뮤텍스인 것이 이 드라이버의 구조를 말해 준다 —
	 * 비트맵을 다루는 두 함수가 모두 프로세스 컨텍스트에서만 불리고,
	 * 인터럽트 문맥에서는 이 잠금을 잡을 일이 없다. */
	struct mutex		lock;	/* protect "used" bitmap */
	/* [한국어] 이 드라이버의 플랫폼 장치. 로그와 fwnode 조회에 쓴다.
	 * 설정자: altera_msi_probe().
	 * 읽는 자: 로그를 남기는 모든 함수와 altera_allocate_domains().
	 * 값 범위: 유효한 플랫폼 장치 포인터.
	 * 동기화: probe 후 불변. */
	struct platform_device	*pdev;
	/* [한국어] 이 드라이버가 만든 MSI 부모 도메인.
	 * 설정자: altera_allocate_domains().
	 * 읽는 자: altera_msi_isr() 이 벡터 번호로 핸들러를 찾는 데 쓰고,
	 * altera_free_domains() 가 없앨 때 쓴다.
	 * 값 범위: 유효한 도메인 포인터. 이름과 달리 이 도메인이 **부모** 이며,
	 * 장치별 MSI-X 도메인이 그 위에 얹힌다.
	 * 동기화: probe 후 불변. */
	struct irq_domain	*inner_domain;
	/* [한국어] 상태·마스크 레지스터 창의 가상 주소.
	 * 설정자: probe 의 devm_platform_ioremap_resource_byname("csr").
	 * 읽는 자: msi_readl()/msi_writel() 이 여기에 오프셋을 더한다.
	 * 값 범위: 유효한 iomem 포인터. devres 가 관리한다.
	 * 동기화: probe 후 불변. */
	void __iomem		*csr_base;
	/* [한국어] 벡터 창의 가상 주소.
	 * 설정자: probe 의 devm_ioremap_resource("vector_slave").
	 * 읽는 자: altera_msi_isr() 이 인터럽트를 지우려고 더미 읽기를 하는 데만 쓴다.
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 후 불변.
	 * 이 창에 **쓰는** 것은 CPU 가 아니라 장치이며, 그때 쓰는 주소는 아래 vector_phy 다. */
	void __iomem		*vector_base;
	/* [한국어] 벡터 창의 **물리** 주소.
	 * 설정자: probe 가 자원의 start 를 그대로 담는다.
	 * 읽는 자: altera_compose_msi_msg() 가 벡터 주소를 계산하는 기준으로 쓴다.
	 * 값 범위: 유효한 물리 주소.
	 * 동기화: probe 후 불변.
	 * 위 vector_base 와 짝을 이루는 같은 창의 두 표현이며, 가상 주소는 CPU 용이고
	 * 이 물리 주소는 장치가 DMA 로 접근할 값이라 둘 다 필요하다. */
	phys_addr_t		vector_phy;
	/* [한국어] 이 FPGA 설계가 지원하는 벡터 개수.
	 * 설정자: probe 가 디바이스 트리의 num-vectors 에서 읽는다.
	 * 읽는 자: 도메인 크기, 비트맵 탐색 범위, isr 의 순회 범위.
	 * 값 범위: 1 ~ MAX_MSI_VECTORS(32). 이 값을 읽지 못하면 probe 가 실패한다.
	 * 동기화: probe 후 불변. */
	u32			num_of_vectors;
	/* [한국어] 요약 인터럽트의 IRQ 번호.
	 * 설정자: probe 의 platform_get_irq().
	 * 읽는 자: 체인 핸들러를 걸고 떼는 두 자리.
	 * 값 범위: 유효한 IRQ 번호. 32개 벡터가 이 선 하나를 공유한다.
	 * 동기화: probe 후 불변. */
	int			irq;
};

/* [한국어]
 * msi_writel - MSI 제어 레지스터에 쓴다
 *
 * @msi: 드라이버 상태.
 * @value: 쓸 값.
 * @reg: csr 창 안의 오프셋.
 *
 * csr 창의 세 레지스터(STATUS, ERROR, INTMASK)에 쓰는 통로다.
 *
 * _relaxed 판을 쓰는 것이 이 함수의 유일한 판단이다. 이 쓰기 전후로
 * 메모리 순서를 강제할 필요가 없다는 뜻인데, 이 파일에서 레지스터 쓰기가
 * 다른 메모리 접근과 순서를 맞춰야 하는 경우가 없기 때문이다.
 *
 * 실행 컨텍스트: 도메인 할당·해제(프로세스)와 remove. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   altera_irq_domain_alloc() / altera_irq_domain_free() / altera_msi_remove()
 *     → [이 함수] → writel_relaxed()
 */
static inline void msi_writel(struct altera_msi *msi, const u32 value,
			      const u32 reg)
{
	writel_relaxed(value, msi->csr_base + reg);
}

/* [한국어]
 * msi_readl - MSI 제어 레지스터를 읽는다
 *
 * @msi: 드라이버 상태.
 * @reg: csr 창 안의 오프셋.
 * @return: 읽은 값.
 *
 * msi_writel() 의 짝이다.
 *
 * 읽는 곳이 둘이다 — 인터럽트 핸들러가 STATUS 를, 할당·해제가 INTMASK 를
 * 읽는다. 두 레지스터가 서로 다른 문맥에서 쓰이지만 겹치지 않아 잠금이 없다.
 *
 * 실행 컨텍스트: 인터럽트 문맥(STATUS)과 프로세스 컨텍스트(INTMASK).
 * 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   altera_msi_isr() / altera_irq_domain_alloc() / altera_irq_domain_free()
 *     → [이 함수] → readl_relaxed()
 */
static inline u32 msi_readl(struct altera_msi *msi, const u32 reg)
{
	return readl_relaxed(msi->csr_base + reg);
}

/* [한국어]
 * altera_msi_isr - 요약 인터럽트를 받아 벡터별 핸들러로 갈라 보낸다
 *
 * @desc: 이 체인 인터럽트의 서술자.
 *
 * 이 하드웨어는 32개 벡터가 인터럽트 선 **하나** 를 공유한다. 어느 벡터였는지는
 * 상태 레지스터의 비트가 알려 주므로, 그것을 읽어 갈라 보내는 것이 이 함수의 일이다.
 *
 * 체인 핸들러라는 점이 중요하다. 보통의 핸들러와 달리 자기 인터럽트를 직접
 * 처리하지 않고, 상위 컨트롤러의 인터럽트를 받아 하위로 나눠 주는 역할이다.
 * chained_irq_enter/exit 이 그 상위 인터럽트의 마스킹과 EOI 를 대신해 준다.
 *
 * 바깥 while 루프가 있는 이유가 이 함수의 요점이다. 안쪽 루프를 도는 동안
 * 새 인터럽트가 올라와 상태 비트가 다시 설 수 있어, 상태가 0 이 될 때까지
 * 반복한다. 한 번만 읽으면 그 사이에 온 인터럽트를 놓치고, 그러면 그 벡터는
 * 영영 처리되지 않는다.
 *
 * 벡터를 더미로 읽는 것이 인터럽트를 지우는 방법이다(옆의 상류 주석).
 * 쓰기가 아니라 읽기로 지우는 것이 이 하드웨어의 방식이며, 지우지 않으면
 * 같은 비트가 계속 서 있어 위 루프가 끝나지 않는다.
 *
 * generic_handle_domain_irq() 가 벡터 번호로 등록된 핸들러를 찾아 부른다.
 * 실패하면 아무도 그 벡터를 요청하지 않았다는 뜻이라, 속도 제한을 걸어
 * 기록만 남긴다 — 제한이 없으면 그런 인터럽트가 반복될 때 로그가 시스템을 멈춘다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 잠들 수 없다.
 *
 * 에러 경로: 등록되지 않은 벡터는 로그로만 남는다.
 *
 * 호출 체인:
 *   상위 인터럽트 컨트롤러 → [이 함수]
 *     → chained_irq_enter() → msi_readl(MSI_STATUS)
 *     → generic_handle_domain_irq() → chained_irq_exit()
 */
static void altera_msi_isr(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct altera_msi *msi;
	/* [한국어] 상태 레지스터 값. for_each_set_bit 이 unsigned long 을 요구해 그 타입이다. */
	unsigned long status;
	/* [한국어] 지금 처리 중인 벡터 번호. */
	u32 bit;
	/* [한국어] 핸들러 호출의 결과. */
	int ret;

	chained_irq_enter(chip, desc);
	/* [한국어] 체인 핸들러를 걸 때 넘겨 둔 드라이버 상태를 되찾는다. */
	msi = irq_desc_get_handler_data(desc);

	while ((status = msi_readl(msi, MSI_STATUS)) != 0) {
		/* [한국어] 선 비트마다 — num_of_vectors 로 범위를 제한해, 쓰지 않는 상위 비트가
		 * 어쩌다 서 있어도 처리하지 않는다. */
		for_each_set_bit(bit, &status, msi->num_of_vectors) {
			/* Dummy read from vector to clear the interrupt */
			readl_relaxed(msi->vector_base + (bit * sizeof(u32)));

			ret = generic_handle_domain_irq(msi->inner_domain, bit);
			/* [한국어] 그 벡터를 아무도 요청하지 않았으면, */
			if (ret)
				/* [한국어] 속도 제한을 걸어 기록만 남긴다. 제한이 없으면 그런 인터럽트가 반복될 때
				 * 로그가 시스템을 멈춘다. */
				dev_err_ratelimited(&msi->pdev->dev, "unexpected MSI\n");
		/* [한국어] 이 벡터 처리 끝. */
		}
	}

	chained_irq_exit(chip, desc);
/* [한국어] 바깥 while 이 상태가 0 이 될 때까지 돌았으므로, 여기 왔다면 남은 인터럽트가 없다. */
}

/* [한국어] 이 부모 도메인이 위층에 **요구** 하는 플래그 묶음.
 * USE_DEF_DOM_OPS 와 USE_DEF_CHIP_OPS 는 위층이 기본 구현을 쓰라는 뜻이고,
 * NO_AFFINITY 는 벡터를 특정 CPU 로 보낼 수 없다는 뜻이다 —
 * 요약 인터럽트 하나뿐이라 그 선이 붙은 CPU 로만 간다. */
#define ALTERA_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS		| \
				   MSI_FLAG_USE_DEF_CHIP_OPS		| \
				   MSI_FLAG_NO_AFFINITY)

/* [한국어] 이 부모가 **지원** 하는 플래그 묶음.
 * MSI_GENERIC_FLAGS_MASK 가 일반적인 것들을 열고, PCI_MSIX 를 더한다.
 * MSI_FLAG_MULTI_PCI_MSI 가 없어 장치가 여러 벡터를 쓰려면 MSI-X 여야 한다. */
#define ALTERA_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK		| \
				    MSI_FLAG_PCI_MSIX)

static const struct msi_parent_ops altera_msi_parent_ops = {
	/* [한국어] 이 부모 도메인이 **요구** 하는 플래그 — 위층이 이것을 반드시 켜야 한다.
	 * NO_AFFINITY 가 들어 있는 것은 이 하드웨어가 벡터를 특정 CPU 로 보낼 수
	 * 없기 때문이다(요약 인터럽트 하나만 있다). */
	.required_flags		= ALTERA_MSI_FLAGS_REQUIRED,
	/* [한국어] 이 부모가 **지원** 하는 플래그. MSI_FLAG_PCI_MSIX 가 있고 MULTI_PCI_MSI 는
	 * 없어, 장치가 여러 벡터를 쓰려면 MSI-X 여야 한다. */
	.supported_flags	= ALTERA_MSI_FLAGS_SUPPORTED,
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	.prefix			= "Altera-",
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};
/* [한국어]
 * altera_compose_msi_msg - 벡터 번호를 장치가 쓸 주소·데이터로 바꾼다
 *
 * @data: 대상 인터럽트. hwirq 가 벡터 번호다.
 * @msg: 결과를 담을 자리.
 *
 * 이 파일의 핵심 변환이다. 장치가 MSI 를 내려면 "어디에 무엇을 써야 하는가"
 * 를 알아야 하고, 그 답이 여기서 만들어져 PCI 코어를 통해 장치의 MSI
 * capability 에 기록된다.
 *
 * 주소가 벡터 창의 시작에서 벡터 번호만큼 떨어진 자리다. 벡터마다 4바이트씩
 * 차지하므로 번호에 4 를 곱한다. 장치가 그 주소에 쓰면 하드웨어가 오프셋을
 * 벡터 번호로 되돌려 해석한다 — altera_msi_isr() 의 비트 위치가 그 결과다.
 *
 * **물리 주소** 를 쓰는 것이 중요하다. 이 값은 장치가 DMA 로 접근할 주소이지
 * CPU 가 쓸 가상 주소가 아니다. probe 가 vector_phy 에 자원의 시작 주소를
 * 그대로 담아 두는 이유가 그것이다.
 *
 * 데이터에도 벡터 번호를 넣는다. 이 하드웨어에서는 주소만으로 벡터가
 * 정해지므로 데이터 값 자체는 쓰이지 않는 것으로 보이나, 그 근거는 이
 * 트리에서 확인 못 함.
 *
 * 실행 컨텍스트: MSI 설정. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   PCI MSI 코어 → irq_chip.irq_compose_msi_msg == [이 함수]
 */
static void altera_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct altera_msi *msi = irq_data_get_irq_chip_data(data);
	phys_addr_t addr = msi->vector_phy + (data->hwirq * sizeof(u32));

	msg->address_lo = lower_32_bits(addr);
	/* [한국어] 상위 32비트. 물리 주소가 4GB 를 넘는 시스템에서 필요하다. */
	msg->address_hi = upper_32_bits(addr);
	/* [한국어] 데이터에도 벡터 번호를 넣는다. 이 하드웨어는 주소만으로 벡터를 가르는
	 * 것으로 보이나, 데이터가 쓰이지 않는다는 근거는 이 트리에서 확인 못 함. */
	msg->data = data->hwirq;

	dev_dbg(&msi->pdev->dev, "msi#%d address_hi %#x address_lo %#x\n",
		/* [한국어] 만든 값을 디버그 기록에 남긴다. hwirq 가 unsigned long 이라 %d 에 맞게 변환한다. */
		(int)data->hwirq, msg->address_hi, msg->address_lo);
}

static struct irq_chip altera_msi_bottom_irq_chip = {
	/* [한국어] /proc/interrupts 에 나올 칩 이름. */
	.name			= "Altera MSI",
	/* [한국어] 이 칩이 제공하는 동작은 메시지 조립 하나뿐이다. 마스킹은 위층의
	 * MSI-X 벡터 제어가 맡고, 이 도메인의 마스크는 할당·해제 때만 건드린다. */
	.irq_compose_msi_msg	= altera_compose_msi_msg,
};

/* [한국어]
 * altera_irq_domain_alloc - 벡터 하나를 배정하고 그 마스크를 연다
 *
 * @domain: 이 부모 도메인.
 * @virq: 배정할 가상 IRQ 번호.
 * @nr_irqs: 요청 개수. 언제나 1 이어야 한다.
 * @args: 쓰지 않는다.
 * @return: 0 = 성공, -ENOSPC = 남은 벡터 없음.
 *
 * 장치가 MSI 를 요청하면 위층 도메인이 이 함수를 벡터 수만큼 부른다.
 *
 * 비트맵에서 빈 자리를 찾는 것이 배정의 전부다. 32개뿐이라 선형 탐색으로
 * 충분하고, 뮤텍스가 두 요청이 같은 자리를 잡는 것을 막는다.
 *
 * 잠금 구간이 짧은 것에 주의할 만하다. 비트를 세우자마자 놓고, 도메인 등록과
 * 마스크 열기는 잠금 밖에서 한다. 그래도 안전한 이유는 그 비트를 세운
 * 시점부터 이 벡터가 이 요청의 것이 되어 다른 요청이 같은 번호를 얻을 수
 * 없기 때문이다.
 *
 * 마스크를 여는 것이 마지막이다. INTMASK 의 그 비트를 켜야 하드웨어가
 * 그 벡터의 인터럽트를 실제로 올린다. 읽기-수정-쓰기인 것은 다른 벡터의
 * 비트를 보존해야 하기 때문이다.
 *
 * WARN_ON 으로 nr_irqs 를 확인하지만 경고만 하고 진행한다. 여러 개를 요청받아도
 * 하나만 배정하는 셈이며, MSI_FLAG_PCI_MSIX 만 지원하고 MULTI_PCI_MSI 는
 * 지원하지 않으니 실제로는 그런 요청이 오지 않는다.
 *
 * 실행 컨텍스트: MSI 활성화. 뮤텍스가 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 남은 벡터가 없으면 -ENOSPC 이며, 위층이 그것을 보고 더 적은
 * 벡터로 다시 협상한다.
 *
 * 호출 체인:
 *   drivers/pci/msi/ 의 도메인 계층 → irq_domain_ops.alloc == [이 함수]
 *     → find_first_zero_bit() → irq_domain_set_info() → msi_writel(MSI_INTMASK)
 */
static int altera_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				   unsigned int nr_irqs, void *args)
{
	struct altera_msi *msi = domain->host_data;
	unsigned long bit;
	/* [한국어] INTMASK 를 읽기-수정-쓰기 할 자리. */
	u32 mask;

	WARN_ON(nr_irqs != 1);
	/* [한국어] 비트맵을 다루는 동안 잠근다. */
	mutex_lock(&msi->lock);

	bit = find_first_zero_bit(msi->used, msi->num_of_vectors);
	/* [한국어] 빈 자리가 없으면 — 32개를 다 썼다는 뜻이다. */
	if (bit >= msi->num_of_vectors) {
		/* [한국어] 잠금을 놓고, */
		mutex_unlock(&msi->lock);
		return -ENOSPC;
	}

	set_bit(bit, msi->used);
/* [한국어] 잠금을 여기서 놓는다. 아래 등록과 마스크 열기는 잠금 밖에서 하는데,
 * 비트를 세운 시점부터 이 벡터가 이 요청의 것이 되어 다른 요청이
 * 같은 번호를 얻을 수 없기 때문이다. */

	mutex_unlock(&msi->lock);

	irq_domain_set_info(domain, virq, bit, &altera_msi_bottom_irq_chip,
			    /* [한국어] host_data 로 드라이버 상태를 넘겨, 아래 칩 콜백이 되찾을 수 있게 한다.
			     * handle_simple_irq 는 EOI 나 마스킹이 필요 없는 인터럽트용 흐름 처리기다. */
			    domain->host_data, handle_simple_irq,
			    NULL, NULL);

	mask = msi_readl(msi, MSI_INTMASK);
	/* [한국어] 이 벡터의 비트를 켠다. */
	mask |= 1 << bit;
	/* [한국어] 되쓴다. 읽기-수정-쓰기라 다른 벡터의 비트가 보존된다. */
	msi_writel(msi, mask, MSI_INTMASK);

	return 0;
}

/* [한국어]
 * altera_irq_domain_free - 벡터 하나를 놓고 그 마스크를 닫는다
 *
 * @domain: 이 부모 도메인.
 * @virq: 놓을 가상 IRQ 번호.
 * @nr_irqs: 요청 개수. 쓰지 않는다.
 *
 * altera_irq_domain_alloc() 의 짝이다.
 *
 * 이미 놓인 벡터인지 먼저 확인하는 것이 이 함수의 특징이다. 그런 경우
 * 기록만 남기고 아무것도 하지 않는데, 그대로 진행하면 남의 벡터가 될 수도
 * 있는 자리의 마스크를 닫게 된다.
 *
 * 여기서는 잠금이 전 구간을 덮는다. 할당 쪽이 짧게 잡는 것과 다른데,
 * 검사와 해제가 하나의 원자적 동작이어야 하기 때문이다 — 검사와 해제 사이에
 * 다른 주체가 끼어들면 두 번 해제하는 일이 생긴다.
 *
 * __clear_bit 을 쓰는 것이 눈에 띈다. 원자적이지 않은 판인데, 뮤텍스가
 * 이미 지키고 있어 원자성이 따로 필요 없기 때문이다. 반면 할당 쪽은
 * set_bit(원자적 판)을 쓴다 — 그쪽도 뮤텍스 안이라 필요 없지만 상류 코드가
 * 그렇게 되어 있다.
 *
 * 실행 컨텍스트: MSI 비활성화. 뮤텍스가 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 반환값이 없다. 이중 해제는 로그로만 남는다.
 *
 * 호출 체인:
 *   drivers/pci/msi/ 의 도메인 계층 → irq_domain_ops.free == [이 함수]
 *     → __clear_bit() → msi_writel(MSI_INTMASK)
 */
static void altera_irq_domain_free(struct irq_domain *domain,
				   unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct altera_msi *msi = irq_data_get_irq_chip_data(d);
	/* [한국어] INTMASK 를 읽기-수정-쓰기 할 자리. */
	u32 mask;

	mutex_lock(&msi->lock);

	if (!test_bit(d->hwirq, msi->used)) {
		/* [한국어] 이미 놓인 벡터를 또 놓으려는 것이면 기록만 남긴다 — */
		dev_err(&msi->pdev->dev, "trying to free unused MSI#%lu\n",
			/* [한국어] 어느 벡터였는지 함께 남긴다. 그대로 진행하면 남의 벡터가 될 수도 있는
			 * 자리의 마스크를 닫게 된다. */
			d->hwirq);
	} else {
		__clear_bit(d->hwirq, msi->used);
		/* [한국어] 정상 경로에서는 마스크를 읽어, */
		mask = msi_readl(msi, MSI_INTMASK);
		/* [한국어] 이 벡터의 비트를 지우고, */
		mask &= ~(1 << d->hwirq);
		/* [한국어] 되쓴다. */
		msi_writel(msi, mask, MSI_INTMASK);
	/* [한국어] 해제 끝. */
	}

	mutex_unlock(&msi->lock);
}

static const struct irq_domain_ops msi_domain_ops = {
	/* [한국어] 벡터를 배정하고 마스크를 여는 콜백. */
	.alloc	= altera_irq_domain_alloc,
	/* [한국어] 그 반대. 이 둘이 이 도메인의 전부다. */
	.free	= altera_irq_domain_free,
};

/* [한국어]
 * altera_allocate_domains - MSI 부모 도메인을 만든다
 *
 * @msi: 드라이버 상태.
 * @return: 0 = 성공, -ENOMEM.
 *
 * 이 드라이버가 인터럽트 계층에 자리를 잡는 지점이다.
 *
 * msi_create_parent_irq_domain() 이 두 가지를 함께 등록한다 — 이 파일의
 * 할당·해제 콜백(msi_domain_ops)과, 위층에 능력을 알리는 부모 규약
 * (altera_msi_parent_ops)이다. 뒤의 것이 있어야 장치별 MSI-X 도메인이
 * 이 위에 얹힐 수 있다.
 *
 * size 에 벡터 수를 넘겨 도메인의 크기를 정한다. 그 값이 디바이스 트리에서
 * 왔으므로, FPGA 설계에 따라 도메인 크기가 달라진다.
 *
 * fwnode 로 이 플랫폼 장치의 노드를 넘긴다. 디바이스 트리에서 다른 노드가
 * msi-parent 로 이 노드를 가리키면, 그 참조가 이 도메인으로 이어진다.
 *
 * 실행 컨텍스트: probe. 할당이 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 도메인 생성 실패는 -ENOMEM 이며 기록을 남긴다.
 *
 * 호출 체인:
 *   altera_msi_probe() → [이 함수] → msi_create_parent_irq_domain()
 */
static int altera_allocate_domains(struct altera_msi *msi)
{
	struct irq_domain_info info = {
		.fwnode		= dev_fwnode(&msi->pdev->dev),
		/* [한국어] 위 두 콜백을 담은 표. */
		.ops		= &msi_domain_ops,
		.host_data	= msi,
		.size		= msi->num_of_vectors,
	};

	msi->inner_domain = msi_create_parent_irq_domain(&info, &altera_msi_parent_ops);
	/* [한국어] 도메인을 만들지 못하면, */
	if (!msi->inner_domain) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(&msi->pdev->dev, "failed to create MSI domain\n");
		/* [한국어] 메모리 부족으로 답한다. */
		return -ENOMEM;
	}

	return 0;
}

/* [한국어]
 * altera_free_domains - MSI 부모 도메인을 없앤다
 *
 * @msi: 드라이버 상태.
 *
 * altera_allocate_domains() 의 짝이며 한 줄이다.
 *
 * 함수로 감싼 이유는 대칭 때문이다 — 만드는 쪽이 함수이므로 없애는 쪽도
 * 함수로 두어, remove 를 읽을 때 짝이 눈에 들어온다.
 *
 * 실행 컨텍스트: remove, 또는 probe 의 되감기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   altera_msi_remove() → [이 함수] → irq_domain_remove()
 */
static void altera_free_domains(struct altera_msi *msi)
{
	irq_domain_remove(msi->inner_domain);
}

/* [한국어]
 * altera_msi_remove - 인터럽트를 끊고 도메인을 없앤다
 *
 * @pdev: 플랫폼 장치.
 *
 * 순서가 이 함수의 뼈대다.
 * 1. 모든 벡터의 마스크를 닫는다 — 하드웨어가 더는 인터럽트를 올리지 않는다.
 * 2. 체인 핸들러를 뗀다 — 이미 올라온 인터럽트도 이 파일로 오지 않는다.
 * 3. 도메인을 없앤다.
 * 4. drvdata 를 비운다.
 *
 * 1번을 먼저 하는 것이 중요하다. 핸들러를 먼저 떼면 그 사이에 올라온
 * 인터럽트를 아무도 처리하지 않아 계속 다시 올라온다.
 *
 * [상류 코드 관찰] probe 의 오류 경로가 이 함수를 부르는데, 그 시점에는
 * 아직 platform_set_drvdata() 가 실행되지 않아 여기서 얻는 msi 가 NULL 이다.
 * 그러면 첫 줄의 msi_writel() 이 NULL 을 역참조한다. 원본(1f0e418bb6)에서
 * 확인했으며 코드는 고치지 않았다. 그 경로에 닿으려면 도메인 생성은
 * 성공하고 platform_get_irq() 만 실패해야 한다.
 *
 * 실행 컨텍스트: remove, 또는 probe 의 되감기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 / altera_msi_probe() 의 err → [이 함수]
 *     → msi_writel() → irq_set_chained_handler_and_data(NULL)
 *     → altera_free_domains()
 */
static void altera_msi_remove(struct platform_device *pdev)
{
	struct altera_msi *msi = platform_get_drvdata(pdev);

	msi_writel(msi, 0, MSI_INTMASK);
	/* [한국어] 체인 핸들러를 뗀다. 마스크를 먼저 닫은 뒤라, 이 시점에는 새 인터럽트가 오지 않는다. */
	irq_set_chained_handler_and_data(msi->irq, NULL, NULL);

	altera_free_domains(msi);

	platform_set_drvdata(pdev, NULL);
}

/* [한국어]
 * altera_msi_probe - 두 레지스터 창과 벡터 수를 얻어 MSI 도메인을 세운다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이다.
 *
 * 레지스터 창이 둘이고 성격이 다르다.
 * - csr — 상태·마스크 레지스터. CPU 가 읽고 쓴다.
 * - vector_slave — 장치가 MSI 를 쓰는 창. CPU 는 인터럽트를 지울 때만 읽고,
 *   실제 쓰기는 장치가 DMA 로 한다.
 *
 * 두 번째 창에서 **물리 주소를 따로 저장** 하는 것이 이 probe 의 요점이다.
 * 매핑된 가상 주소는 CPU 용이고, 장치에 알려 줄 것은 물리 주소이기 때문이다.
 * altera_compose_msi_msg() 가 그 값을 쓴다.
 *
 * 벡터 수를 디바이스 트리에서 읽는다. FPGA 설계 시점에 정해지는 값이라
 * 코드에 고정할 수 없다. 없으면 -EINVAL 로 물러나는데, 도메인 크기를 정할
 * 수 없기 때문이다.
 *
 * 도메인을 인터럽트보다 **먼저** 만드는 순서에 주의할 만하다. 반대로 하면
 * 핸들러를 걸자마자 인터럽트가 올라왔을 때 갈라 보낼 도메인이 없다.
 *
 * subsys_initcall 로 등록되어 일반 드라이버보다 이른 단계에 초기화된다.
 * 이 도메인이 있어야 브리지 쪽 드라이버가 MSI 를 쓸 수 있기 때문이다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 각 단계의 실패를 그대로 올려보내며, 도메인을 만든 뒤의 실패만
 * altera_msi_remove() 로 되감는다 — 다만 그 경로에 NULL 역참조가 있다
 * (위 altera_msi_remove() 의 관찰 참조).
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → devm_platform_ioremap_resource_byname() → devm_ioremap_resource()
 *     → of_property_read_u32() → altera_allocate_domains()
 *     → irq_set_chained_handler_and_data()
 */
static int altera_msi_probe(struct platform_device *pdev)
{
	struct altera_msi *msi;
	struct device_node *np = pdev->dev.of_node;
	/* [한국어] 벡터 창의 자원. 물리 주소를 꺼내려고 포인터를 따로 받는다. */
	struct resource *res;
	/* [한국어] 각 단계의 결과. */
	int ret;

	msi = devm_kzalloc(&pdev->dev, sizeof(struct altera_msi),
			   /* [한국어] devres 판이라 실패하거나 드라이버가 떨어질 때 자동으로 해제된다. */
			   GFP_KERNEL);
	if (!msi)
		/* [한국어] 상태 구조를 잡지 못하면 물러난다. */
		return -ENOMEM;

	mutex_init(&msi->lock);
	msi->pdev = pdev;

	msi->csr_base = devm_platform_ioremap_resource_byname(pdev, "csr");
	/* [한국어] csr 창을 매핑하지 못하면, */
	if (IS_ERR(msi->csr_base)) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(&pdev->dev, "failed to map csr memory\n");
		/* [한국어] 오류 포인터에서 코드를 꺼내 올려보낸다. */
		return PTR_ERR(msi->csr_base);
	/* [한국어] csr 매핑 실패 처리 끝. */
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   /* [한국어] 이름으로 찾는다 — 디바이스 트리의 reg-names 가 두 창을 구분한다. */
					   "vector_slave");
	msi->vector_base = devm_ioremap_resource(&pdev->dev, res);
	/* [한국어] 벡터 창을 매핑하지 못하면, */
	if (IS_ERR(msi->vector_base))
		/* [한국어] 그 오류를 올려보낸다. 여기는 기록을 남기지 않는데,
		 * devm_ioremap_resource() 가 이미 남기기 때문이다. */
		return PTR_ERR(msi->vector_base);

	msi->vector_phy = res->start;
/* [한국어] **물리 주소를 따로 저장한다.** 위에서 얻은 가상 주소는 CPU 용이고,
 * 장치에 알려 줄 것은 이 물리 주소다. */

	if (of_property_read_u32(np, "num-vectors", &msi->num_of_vectors)) {
		/* [한국어] 벡터 수를 읽지 못하면 그 사실을 남기고, */
		dev_err(&pdev->dev, "failed to parse the number of vectors\n");
		/* [한국어] 도메인 크기를 정할 수 없으므로 잘못된 인자로 답한다. */
		return -EINVAL;
	}

	ret = altera_allocate_domains(msi);
	/* [한국어] 도메인 생성이 실패하면, */
	if (ret)
		/* [한국어] 그 오류를 올려보낸다. 아직 되감을 것이 없다. */
		return ret;

	msi->irq = platform_get_irq(pdev, 0);
	/* [한국어] 인터럽트 번호를 얻지 못하면, */
	if (msi->irq < 0) {
		/* [한국어] 그 오류를 기억하고, */
		ret = msi->irq;
		/* [한국어] 도메인을 되감는 경로로 간다. */
		goto err;
	}

	irq_set_chained_handler_and_data(msi->irq, altera_msi_isr, msi);
	/* [한국어] 이 시점에야 drvdata 가 설정된다 — 위 err 경로가 NULL 을 보게 되는 이유다. */
	platform_set_drvdata(pdev, msi);

	return 0;

err:
	altera_msi_remove(pdev);
	return ret;
}

static const struct of_device_id altera_msi_of_match[] = {
	/* [한국어] 디바이스 트리에서 이 문자열로 매칭한다. */
	{ .compatible = "altr,msi-1.0", NULL },
	/* [한국어] 표의 끝 표시. */
	{ },
};

static struct platform_driver altera_msi_driver = {
	/* [한국어] 플랫폼 드라이버로 등록된다 — 이 MSI 컨트롤러가 PCI 장치가 아니라
	 * FPGA 안의 블록이기 때문이다. */
	.driver = {
		/* [한국어] sysfs 에 나올 이름. */
		.name = "altera-msi",
		/* [한국어] 위 매칭 표. */
		.of_match_table = altera_msi_of_match,
	},
	.probe = altera_msi_probe,
	.remove = altera_msi_remove,
};

/* [한국어]
 * altera_msi_init - 이 모듈을 플랫폼 드라이버로 등록한다
 *
 * @return: 0 = 성공, 음수 오류.
 *
 * subsys_initcall 로 불려 일반 드라이버보다 이른 단계에 등록된다.
 *
 * 그 이른 시점이 필요한 이유는 이 드라이버가 **다른 드라이버의 전제** 이기
 * 때문이다. 브리지 쪽(pcie-altera.c)이 MSI 를 쓰려면 이 도메인이 이미
 * 있어야 하고, 없으면 그쪽 probe 가 미뤄진다.
 *
 * 실행 컨텍스트: 부팅 중 initcall. 프로세스 컨텍스트.
 *
 * 에러 경로: 등록 실패를 그대로 돌려준다.
 *
 * 호출 체인:
 *   subsys_initcall → [이 함수] → platform_driver_register()
 */
static int __init altera_msi_init(void)
{
	return platform_driver_register(&altera_msi_driver);
}

/* [한국어]
 * altera_msi_exit - 이 모듈의 드라이버 등록을 해제한다
 *
 * @: 인자 없음.
 *
 * altera_msi_init() 의 짝이다. 등록을 풀면 붙어 있던 장치마다
 * altera_msi_remove() 가 불린다.
 *
 * 실행 컨텍스트: 모듈 언로드. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   module_exit → [이 함수] → platform_driver_unregister()
 */
static void __exit altera_msi_exit(void)
{
	platform_driver_unregister(&altera_msi_driver);
}

subsys_initcall(altera_msi_init);
MODULE_DEVICE_TABLE(of, altera_msi_of_match);
module_exit(altera_msi_exit);
MODULE_DESCRIPTION("Altera PCIe MSI support driver");
MODULE_LICENSE("GPL v2");
