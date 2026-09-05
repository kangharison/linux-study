// SPDX-License-Identifier: GPL-2.0-only
/*
 * RISC-V IOMMU as a platform device
 *
 * Copyright © 2023 FORTH-ICS/CARV
 * Copyright © 2023-2024 Rivos Inc.
 *
 * Authors
 *	Nick Kossifidis <mick@ics.forth.gr>
 *	Tomasz Jeznach <tjeznach@rivosinc.com>
 */

/*
 * [한국어 설명] RISC-V IOMMU 가 플랫폼 장치로 나타날 때의 프로브 (riscv/iommu-platform.c)
 *
 * === 파일의 역할 ===
 * (위 영어 주석 참고) SoC 안에 박혀 있는 RISC-V IOMMU 를 찾아 준비한다.
 * PCIe 판(iommu-pci.c)과 짝을 이루며, 둘 다 자원을 챙긴 뒤 공통 초기화
 * riscv_iommu_init() 으로 모인다.
 *
 * PCI 판과 가장 크게 다른 점은 인터럽트다. PCI 는 MSI 만 쓰지만, 플랫폼
 * 장치는 MSI 와 배선 인터럽트를 모두 다뤄야 한다. SoC 마다 사정이 달라
 * 어느 쪽을 쓸 수 있는지가 하드웨어 능력과 펌웨어 기술에 함께 달려 있기
 * 때문이다. 그래서 이 파일의 절반이 그 두 갈래를 고르는 코드다.
 *
 * MSI 를 쓰는 경우 이 IOMMU 는 조금 특별하다 — 자기가 인터럽트를 낼 때
 * 쓸 주소와 데이터를 자기 레지스터 표에 직접 적어 두어야 한다. 그 일을
 * riscv_iommu_write_msi_msg() 가 맡는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 프로브 흐름은 이렇다:
 *
 *   플랫폼 버스가 장치 트리(또는 ACPI)에서 장치를 찾음
 *     → riscv_iommu_platform_probe()   ← 이 파일
 *       → 레지스터 창을 매핑하고 능력을 읽는다
 *       → MSI 를 시도하고, 안 되면 배선 인터럽트로 물러난다
 *     → riscv_iommu_init()             ← 공통 코드
 *
 * 실행 컨텍스트: 드라이버 프로브. 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * - iommu.h / iommu-bits.h: 자료 모델과 하드웨어 규격 값.
 * - iommu.c: 실제 초기화와 하드웨어 조작.
 * - iommu-pci.c: 같은 하드웨어의 PCIe 판.
 * - IMSIC(riscv-imsic.h): RISC-V 의 MSI 컨트롤러 — ACPI 시스템에서
 *   MSI 도메인을 찾을 때 그쪽에 물어본다.
 * - 장치 트리 / ACPI: 레지스터 창과 인터럽트 자원을 알려 준다.
 *
 * === 주요 함수/구조체 요약 ===
 * - riscv_iommu_write_msi_msg(): 배정받은 MSI 주소·데이터를 하드웨어의
 *   MSI 설정 표에 적어 넣는다.
 * - riscv_iommu_platform_probe(): 자원을 챙기고 인터럽트 방식을 고른 뒤
 *   공통 초기화에 넘긴다.
 * - riscv_iommu_of_match / acpi_match: 장치 트리와 ACPI 양쪽의 매칭 표.
 */

#include <linux/acpi.h>	/* [한국어] ACPI 로도 이 하드웨어를 찾을 수 있다. */
#include <linux/irqchip/riscv-imsic.h>	/* [한국어] RISC-V 의 MSI 컨트롤러 — ACPI 경로에서 MSI 도메인을 찾는 데 쓴다. */
#include <linux/kernel.h>	/* [한국어] 기본 매크로들. */
#include <linux/msi.h>	/* [한국어] MSI 배정과 서술자. */
#include <linux/of_irq.h>	/* [한국어] 장치 트리에서 MSI 부모를 설정한다. */
#include <linux/of_platform.h>	/* [한국어] 장치 트리 기반 플랫폼 장치. */
#include <linux/platform_device.h>	/* [한국어] 플랫폼 드라이버 등록. */

#include "iommu-bits.h"	/* [한국어] 레지스터 오프셋과 능력 비트. */
#include "iommu.h"	/* [한국어] 이 드라이버의 자료 모델과 공통 진입점. */

/*
 * [한국어]
 * riscv_iommu_write_msi_msg - 배정받은 MSI 주소를 하드웨어에 적어 넣는다
 *
 * @desc: MSI 서술자 — 몇 번째 MSI 인지 알려 준다.
 * @msg: 인터럽트 컨트롤러가 정해 준 주소와 데이터.
 *
 * 이 IOMMU 는 자기가 인터럽트를 낼 때 쓸 주소와 데이터를 자기 레지스터
 * 표에 직접 적어 둔다. 그래서 MSI 가 배정될 때마다 이 콜백이 불려
 * 그 값을 옮겨 담는다.
 *
 * 주소를 마스크로 자르며 경고하는 대목이 눈에 띈다. 이 하드웨어의 MSI
 * 주소 필드가 전체 물리 주소 폭을 담지 못해, 인터럽트 컨트롤러가 정해 준
 * 주소가 그 범위 밖일 수 있다. 그러면 인터럽트가 엉뚱한 곳으로 가므로
 * 사실상 동작하지 않게 되는데, 조용히 넘기지 않고 한 번 알린다.
 *
 * 실행 컨텍스트: MSI 배정. 잠들 수 있다.
 *
 * 호출 체인:
 *   platform_device_msi_init_and_alloc_irqs() → 이 콜백
 */
static void riscv_iommu_write_msi_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	struct device *dev = msi_desc_to_dev(desc);	/* [한국어] 이 MSI 를 가진 장치. */
	struct riscv_iommu_device *iommu = dev_get_drvdata(dev);	/* [한국어] 그 드라이버 상태. */
	u16 idx = desc->msi_index;	/* [한국어] 몇 번째 MSI 인가 — 레지스터 표의 첨자가 된다. */
	u64 addr;	/* [한국어] 인터럽트를 낼 때 쓸 주소. */

	addr = ((u64)msg->address_hi << 32) | msg->address_lo;	/* [한국어] 32비트 둘로 나뉘어 온 주소를 하나로 합친다. */

	if (addr != (addr & RISCV_IOMMU_MSI_CFG_TBL_ADDR)) {	/* [한국어] 하드웨어가 담을 수 있는 범위를 넘었는가. */
		dev_err_once(dev,	/* [한국어] 부팅마다 한 번만 알린다 — 매 MSI 마다 찍으면 로그가 넘친다. */
			     "uh oh, the IOMMU can't send MSIs to 0x%llx, sending to 0x%llx instead\n",
			     addr, addr & RISCV_IOMMU_MSI_CFG_TBL_ADDR);	/* [한국어] 잘린 뒤의 주소도 함께 보여 준다 — 사실상 인터럽트가 오지 않게 된다는 신호다. */
	}

	addr &= RISCV_IOMMU_MSI_CFG_TBL_ADDR;	/* [한국어] 하드웨어가 담을 수 있는 비트만 남긴다. */

	riscv_iommu_writeq(iommu, RISCV_IOMMU_REG_MSI_CFG_TBL_ADDR(idx), addr);	/* [한국어] 그 주소를 표에 적는다. */
	riscv_iommu_writel(iommu, RISCV_IOMMU_REG_MSI_CFG_TBL_DATA(idx), msg->data);	/* [한국어] 그때 쓸 값 — 컨트롤러가 이 값으로 어느 인터럽트인지 안다. */
	riscv_iommu_writel(iommu, RISCV_IOMMU_REG_MSI_CFG_TBL_CTRL(idx), 0);	/* [한국어] 마스크를 풀어 이 MSI 를 살린다 — 0 이 "막지 않음"이다. */
}

/*
 * [한국어]
 * riscv_iommu_platform_probe - SoC 안의 RISC-V IOMMU 를 준비한다
 *
 * @pdev: 찾아낸 플랫폼 장치.
 * @return: 0 성공, 음수 오류.
 *
 * 레지스터 창을 매핑하고 능력을 읽은 뒤, 인터럽트 방식을 고르는 것이
 * 이 함수의 대부분이다.
 *
 * 그 고르기가 흥미롭다. 하드웨어가 MSI 를 지원하면 먼저 그쪽을 시도하되,
 * MSI 도메인을 못 찾거나 배정에 실패하면 아래 msi_fail 로 떨어진다.
 * 거기서 하드웨어가 배선 인터럽트도 지원한다면(IGS_BOTH) fallthrough 로
 * 그 갈래로 흘러 들어가 배선 방식으로 다시 시도한다 — switch 안의
 * 레이블과 fallthrough 를 그 되돌아가기에 쓴 것이 이 함수의 요령이다.
 *
 * 실행 컨텍스트: 드라이버 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   플랫폼 버스 → .probe = [이 함수] → riscv_iommu_init()
 */
static int riscv_iommu_platform_probe(struct platform_device *pdev)
{
	enum riscv_iommu_igs_settings igs;	/* [한국어] 이 하드웨어가 지원하는 인터럽트 방식. */
	struct device *dev = &pdev->dev;	/* [한국어] 로그와 자원 관리에 쓸 커널 장치. */
	struct riscv_iommu_device *iommu = NULL;	/* [한국어] 만들 드라이버 상태. */
	struct irq_domain *msi_domain;	/* [한국어] ACPI 경로에서 찾을 MSI 도메인. */
	struct resource *res = NULL;	/* [한국어] 레지스터 창 자원 (매핑 함수가 채워 준다). */
	int vec, ret;	/* [한국어] 인터럽트 반복자와 중간 결과. */

	iommu = devm_kzalloc(dev, sizeof(*iommu), GFP_KERNEL);	/* [한국어] 드라이버 상태를 잡는다. */
	if (!iommu)	/* [한국어] 상태를 못 잡았으면 진행할 수 없다. */
		return -ENOMEM;

	iommu->dev = dev;	/* [한국어] 되짚어 갈 수 있게. */
	iommu->reg = devm_platform_get_and_ioremap_resource(pdev, 0, &res);	/* [한국어] 첫 자원을 찾아 한 번에 매핑한다 — devres 가 해제를 맡는다. */
	if (IS_ERR(iommu->reg))	/* [한국어] 자원이 없거나 매핑에 실패했다면. */
		return dev_err_probe(dev, PTR_ERR(iommu->reg),
				     "could not map register region\n");

	dev_set_drvdata(dev, iommu);	/* [한국어] 아래 MSI 콜백과 remove 가 이 값으로 상태를 되찾는다 — MSI 배정 전에 반드시 해 두어야 한다. */

	/* Check device reported capabilities / features. */
	/* [한국어] (위 영어 주석 참고) 인터럽트 방식을 고르려면 능력을 먼저 알아야 한다. */
	iommu->caps = riscv_iommu_readq(iommu, RISCV_IOMMU_REG_CAPABILITIES);	/* [한국어] 능력 비트. */
	iommu->fctl = riscv_iommu_readl(iommu, RISCV_IOMMU_REG_FCTL);	/* [한국어] 지금 켜져 있는 기능 설정. */

	iommu->irqs_count = RISCV_IOMMU_INTR_COUNT;	/* [한국어] 일단 사건 종류만큼 받기를 바란다 — 실제로 받은 수로 아래에서 덮어쓴다. */

	igs = FIELD_GET(RISCV_IOMMU_CAPABILITIES_IGS, iommu->caps);	/* [한국어] 지원하는 인터럽트 방식. */
	switch (igs) {	/* [한국어] 지원하는 인터럽트 방식에 따라 갈래를 나눈다. */
	case RISCV_IOMMU_CAPABILITIES_IGS_BOTH:	/* [한국어] 둘 다 지원하거나. */
	case RISCV_IOMMU_CAPABILITIES_IGS_MSI:	/* [한국어] MSI 만 지원하면 — 먼저 MSI 를 시도한다. */
		if (is_of_node(dev_fwnode(dev))) {	/* [한국어] 장치 트리로 찾은 장치라면. */
			of_msi_configure(dev, to_of_node(dev->fwnode));	/* [한국어] 트리가 적어 둔 msi-parent 를 따라 도메인을 건다. */
		} else {	/* [한국어] ACPI 로 찾은 장치라면. */
			msi_domain = irq_find_matching_fwnode(imsic_acpi_get_fwnode(dev),	/* [한국어] RISC-V 의 MSI 컨트롤러를 ACPI 표에서 찾는다. */
							      DOMAIN_BUS_PLATFORM_MSI);
			dev_set_msi_domain(dev, msi_domain);	/* [한국어] 찾은 도메인을 건다. */
		}

		if (!dev_get_msi_domain(dev)) {	/* [한국어] 어느 쪽으로도 도메인을 못 찾았다면. */
			dev_warn(dev, "failed to find an MSI domain\n");	/* [한국어] 오류가 아니라 경고다 — 배선으로 물러날 수 있기 때문이다. */
			goto msi_fail;	/* [한국어] 아래에서 배선 방식으로 갈지 판단한다. */
		}

		ret = platform_device_msi_init_and_alloc_irqs(dev, iommu->irqs_count,	/* [한국어] 사건 수만큼 MSI 를 배정받는다. */
							      riscv_iommu_write_msi_msg);	/* [한국어] 배정될 때마다 위 콜백이 불려 하드웨어 표를 채운다. */
		if (ret) {	/* [한국어] 배정에 실패했다면. */
			dev_warn(dev, "failed to allocate MSIs\n");	/* [한국어] 배정 실패 — 아래에서 배선으로 물러날지 판단한다. */
			goto msi_fail;	/* [한국어] 역시 배선으로 물러날지 판단한다. */
		}

		for (vec = 0; vec < iommu->irqs_count; vec++)	/* [한국어] 받은 벡터마다. */
			iommu->irqs[vec] = msi_get_virq(dev, vec);	/* [한국어] 커널이 쓰는 가상 인터럽트 번호로 바꿔 담는다. */

		/* Enable message-signaled interrupts, fctl.WSI */
		/* [한국어] (위 영어 주석 참고) WSI 는 "배선 인터럽트를 쓴다"는 뜻이라,
		 * MSI 를 쓰려면 그 비트를 내려야 한다. */
		if (iommu->fctl & RISCV_IOMMU_FCTL_WSI) {	/* [한국어] 지금 배선 방식이라면. */
			iommu->fctl ^= RISCV_IOMMU_FCTL_WSI;	/* [한국어] 그 비트만 뒤집어 내린다. */
			riscv_iommu_writel(iommu, RISCV_IOMMU_REG_FCTL, iommu->fctl);	/* [한국어] 하드웨어에 알린다. */
		}

		dev_info(dev, "using MSIs\n");	/* [한국어] 어느 방식으로 도는지 남긴다 — 인터럽트 문제를 되짚을 때 첫 단서다. */
		break;

msi_fail:	/* [한국어] MSI 를 쓸 수 없게 됐을 때 오는 자리. */
		if (igs != RISCV_IOMMU_CAPABILITIES_IGS_BOTH) {	/* [한국어] 하드웨어가 MSI 밖에 못 쓴다면. */
			return dev_err_probe(dev, -ENODEV,	/* [한국어] MSI 밖에 못 쓰는 하드웨어라 물러날 곳이 없다. */
					     "unable to use wire-signaled interrupts\n");	/* [한국어] 물러날 곳이 없어 프로브를 접는다. */
		}

		fallthrough;	/* [한국어] 둘 다 지원하는 하드웨어라면 아래 배선 갈래로 흘러 들어간다 — switch 를 되돌아가기에 쓴 요령이다. */

	case RISCV_IOMMU_CAPABILITIES_IGS_WSI:	/* [한국어] 배선 인터럽트 갈래. */
		ret = platform_irq_count(pdev);	/* [한국어] 펌웨어가 몇 개를 알려 줬는가. */
		if (ret <= 0)	/* [한국어] 하나도 없다면. */
			return dev_err_probe(dev, -ENODEV,
					     "no IRQ resources provided\n");	/* [한국어] 폴트와 완료를 알 길이 없어 진행할 수 없다. */

		iommu->irqs_count = ret;	/* [한국어] 실제로 알려 준 개수. */

		if (iommu->irqs_count > RISCV_IOMMU_INTR_COUNT)	/* [한국어] 사건 수보다 많이 알려 줬다면. */
			iommu->irqs_count = RISCV_IOMMU_INTR_COUNT;	/* [한국어] 배열 크기를 넘지 않게 자른다. */

		for (vec = 0; vec < iommu->irqs_count; vec++)	/* [한국어] 자원마다. */
			iommu->irqs[vec] = platform_get_irq(pdev, vec);	/* [한국어] 인터럽트 번호를 얻어 담는다. */

		/* Enable wire-signaled interrupts, fctl.WSI */
		/* [한국어] (위 영어 주석 참고) 이번에는 반대로 그 비트를 세워야 한다. */
		if (!(iommu->fctl & RISCV_IOMMU_FCTL_WSI)) {	/* [한국어] 지금 MSI 방식이라면. */
			iommu->fctl |= RISCV_IOMMU_FCTL_WSI;	/* [한국어] 배선 방식으로 바꾼다. */
			riscv_iommu_writel(iommu, RISCV_IOMMU_REG_FCTL, iommu->fctl);	/* [한국어] 하드웨어에 알린다. */
		}
		dev_info(dev, "using wire-signaled interrupts\n");	/* [한국어] 어느 방식으로 도는지 남긴다. */
		break;
	default:	/* [한국어] 규격에 없는 값 — 하드웨어나 흉내 장치가 잘못됐다. */
		return dev_err_probe(dev, -ENODEV, "invalid IGS\n");	/* [한국어] 규격에 없는 값이라 다룰 수 없다. */
	}

	return riscv_iommu_init(iommu);	/* [한국어] 여기서부터는 버스와 무관한 공통 초기화가 맡는다. */
};

/*
 * [한국어]
 * riscv_iommu_platform_remove - 이 IOMMU 를 내린다
 *
 * @pdev: 대상 플랫폼 장치.
 *
 * 공통 정리를 먼저 부르고, MSI 를 썼다면 그 자원도 놓는다. 순서가
 * 중요하다 — 하드웨어가 아직 인터럽트를 낼 수 있는 동안 MSI 를 놓으면
 * 사라진 처리기를 부르게 된다.
 *
 * 실행 컨텍스트: 드라이버 제거. 잠들 수 있다.
 *
 * 호출 체인:
 *   플랫폼 버스 → .remove = [이 함수] → riscv_iommu_remove()
 */
static void riscv_iommu_platform_remove(struct platform_device *pdev)
{
	struct riscv_iommu_device *iommu = dev_get_drvdata(&pdev->dev);	/* [한국어] 프로브가 붙여 둔 상태. */
	bool msi = !(iommu->fctl & RISCV_IOMMU_FCTL_WSI);	/* [한국어] MSI 로 돌고 있었는가 — 설정 비트로 되짚는다. */

	riscv_iommu_remove(iommu);	/* [한국어] 먼저 하드웨어를 멈추고 자원을 거둔다. */

	if (msi)	/* [한국어] MSI 를 썼다면. */
		platform_device_msi_free_irqs_all(&pdev->dev);	/* [한국어] 배정받은 MSI 를 놓는다 — 하드웨어가 멈춘 뒤여야 안전하다. */
};

/*
 * [한국어]
 * riscv_iommu_platform_shutdown - 시스템이 꺼질 때 변환을 멈춘다
 *
 * @pdev: 대상 플랫폼 장치.
 *
 * 자원을 거두지는 않고 하드웨어만 멈춘다. kexec 로 다음 커널을 띄울 때
 * 알 수 없는 매핑을 물려주지 않기 위해서다.
 *
 * 실행 컨텍스트: 시스템 종료. 잠들 수 있다.
 *
 * 호출 체인:
 *   플랫폼 버스 → .shutdown = [이 함수] → riscv_iommu_disable()
 */
static void riscv_iommu_platform_shutdown(struct platform_device *pdev)
{
	riscv_iommu_disable(dev_get_drvdata(&pdev->dev));	/* [한국어] 상태를 꺼내 곧바로 넘긴다 — 변환과 큐를 멈춘다. */
};

/* [한국어] 장치 트리에서 이 드라이버가 맡을 노드를 알아보는 표. */
static const struct of_device_id riscv_iommu_of_match[] = {
	{.compatible = "riscv,iommu",},	/* [한국어] 규격을 따르는 모든 구현이 이 문자열을 쓴다. */
	{},	/* [한국어] 표의 끝. */
};

/* [한국어] ACPI 에서 이 드라이버가 맡을 장치를 알아보는 표.
 *
 * 장치 트리와 ACPI 를 모두 지원해야 하는 이유는, RISC-V 서버가 ACPI 를
 * 쓰고 임베디드 계열이 장치 트리를 쓰기 때문이다. */
static const struct acpi_device_id riscv_iommu_acpi_match[] = {
	{ "RSCV0004", 0 },	/* [한국어] ACPI 가 이 IOMMU 에 배정한 하드웨어 ID. */
	{}	/* [한국어] 표의 끝. */
};
MODULE_DEVICE_TABLE(acpi, riscv_iommu_acpi_match);	/* [한국어] 자동 적재 장치가 이 표를 본다. */

/* [한국어] 플랫폼 버스에 등록할 드라이버 서술.
 *
 * 매칭 표가 둘인 것이 눈에 띈다 — 장치 트리와 ACPI 어느 쪽으로 나타나든
 * 같은 드라이버가 맡는다. */
static struct platform_driver riscv_iommu_platform_driver = {
	.probe = riscv_iommu_platform_probe,	/* [한국어] 장치를 찾아 준비한다. */
	.remove = riscv_iommu_platform_remove,	/* [한국어] 자원을 거둔다. */
	.shutdown = riscv_iommu_platform_shutdown,	/* [한국어] 종료 때 변환을 멈춘다. */
	.driver = {
		.name = "riscv,iommu",	/* [한국어] sysfs 와 로그에 쓰일 이름. */
		.of_match_table = riscv_iommu_of_match,	/* [한국어] 장치 트리 매칭. */
		.suppress_bind_attrs = true,	/* [한국어] 실행 중 떼기를 막는다 — 뗀 순간 뒤의 모든 장치가 매핑을 잃는다. */
		.acpi_match_table = riscv_iommu_acpi_match,	/* [한국어] ACPI 매칭. */
	},
};

builtin_platform_driver(riscv_iommu_platform_driver);	/* [한국어] 모듈이 아니라 커널에 내장으로만 빌드된다 — IOMMU 는 다른 장치보다 먼저 올라와야 한다. */
