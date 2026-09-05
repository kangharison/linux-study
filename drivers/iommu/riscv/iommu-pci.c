// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright © 2022-2024 Rivos Inc.
 * Copyright © 2023 FORTH-ICS/CARV
 *
 * RISCV IOMMU as a PCIe device
 *
 * Authors
 *	Tomasz Jeznach <tjeznach@rivosinc.com>
 *	Nick Kossifidis <mick@ics.forth.gr>
 */

/*
 * [한국어 설명] RISC-V IOMMU 가 PCIe 장치로 나타날 때의 프로브 (riscv/iommu-pci.c)
 *
 * === 파일의 역할 ===
 * (위 영어 주석 참고) 같은 RISC-V IOMMU 라도 시스템에 붙는 방식이 두
 * 가지다. SoC 안에 박혀 플랫폼 장치로 나타나기도 하고, PCIe 버스에
 * 꽂힌 장치로 나타나기도 한다. 이 파일은 후자의 프로브만 맡는다.
 * 하드웨어를 실제로 다루는 일은 공통 코드(iommu.c)가 하며, 이 파일은
 * 레지스터 창을 매핑하고 인터럽트를 배정한 뒤 그쪽에 넘긴다.
 * 두 파일로 나눈 이유는 버스마다 자원을 얻는 방식이 완전히 다르기
 * 때문이다 — PCI 는 BAR 와 MSI 를, 플랫폼은 장치 트리의 자원 기술을 쓴다.
 * 그 차이만 여기 두고 나머지는 공유한다.
 *
 * PCIe 로 붙는 경우가 실제로 흔한 것은 가상 머신이다. QEMU 가 RISC-V
 * IOMMU 를 PCI 장치로 흉내 내며, 아래 장치 표의 첫 항목이 그것이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 프로브 흐름은 이렇다:
 *
 *   PCI 버스가 장치를 찾음
 *     → riscv_iommu_pci_probe()   ← 이 파일
 *       → BAR 를 매핑하고 능력을 읽는다
 *       → MSI 벡터를 배정한다
 *     → riscv_iommu_init()        ← 공통 코드
 *       → 큐를 잡고 장치 디렉터리를 만들고 iommu 코어에 등록한다
 *
 * 실행 컨텍스트: 드라이버 프로브. 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * - iommu.h: 여기서 채우는 struct riscv_iommu_device 의 정의.
 * - iommu-bits.h: 레지스터 오프셋과 능력 비트.
 * - iommu.c: 실제 초기화와 하드웨어 조작 — 이 파일은 그 진입점을 부른다.
 * - iommu-platform.c: 같은 하드웨어의 플랫폼 장치 판. 둘 다
 *   riscv_iommu_init() 으로 모인다.
 * - PCI 계층: BAR 매핑과 MSI 배정을 대신해 준다.
 *
 * === 주요 함수/구조체 요약 ===
 * - riscv_iommu_pci_probe(): 이 파일의 전부라 할 수 있는 함수. BAR 를
 *   매핑하고, MSI 를 받을 수 있는 하드웨어인지 확인하고, 벡터를 배정한다.
 * - riscv_iommu_pci_remove()/shutdown(): 공통 코드의 정리·중지 함수를 부른다.
 * - riscv_iommu_pci_tbl: 이 드라이버가 맡을 PCI 장치 목록 — QEMU 의
 *   흉내 장치와 Rivos 의 실제 하드웨어.
 */

#include <linux/compiler.h>	/* [한국어] 컴파일러 힌트 매크로들. */
#include <linux/init.h>	/* [한국어] 초기화 구역 표시. */
#include <linux/iommu.h>	/* [한국어] iommu 코어의 기본 형들. */
#include <linux/kernel.h>	/* [한국어] 기본 매크로들. */
#include <linux/pci.h>	/* [한국어] PCI 장치 등록과 BAR·MSI 조작. */

#include "iommu-bits.h"	/* [한국어] 레지스터 오프셋과 능력 비트 정의. */
#include "iommu.h"	/* [한국어] 이 드라이버의 자료 모델과 공통 진입점. */

/* QEMU RISC-V IOMMU implementation */
/* [한국어] (위 영어 주석 참고) QEMU 가 흉내 내는 RISC-V IOMMU 의 장치 번호.
 * 실제 하드웨어가 드물어, 이 드라이버를 쓰는 가장 흔한 경우가 가상 머신이다. */
#define PCI_DEVICE_ID_REDHAT_RISCV_IOMMU     0x0014	/* [한국어] Red Hat 이 QEMU 흉내 장치에 배정한 번호. */

/* Rivos Inc. assigned PCI Vendor and Device IDs */
/* [한국어] (위 영어 주석 참고) 실제 하드웨어를 만든 회사의 번호들. */
#ifndef PCI_VENDOR_ID_RIVOS	/* [한국어] 커널 공용 헤더에 아직 없을 수 있어 여기서 정의한다. */
#define PCI_VENDOR_ID_RIVOS                  0x1efd	/* [한국어] 그 회사의 벤더 번호. */
#endif

#define PCI_DEVICE_ID_RIVOS_RISCV_IOMMU_GA   0x0008	/* [한국어] 그 회사의 IOMMU 제품 번호. */

/*
 * [한국어]
 * riscv_iommu_pci_probe - PCIe 로 붙은 RISC-V IOMMU 를 준비한다
 *
 * @pdev: 찾아낸 PCI 장치.
 * @ent: 어느 표 항목에 맞았는지 (여기서는 쓰지 않는다).
 * @return: 0 성공, 음수 오류.
 *
 * PCI 쪽 자원을 챙겨 공통 초기화에 넘기는 것이 전부다. 하는 일이 넷이다 —
 * BAR 를 매핑하고, 능력을 읽고, MSI 를 배정하고, 인터럽트 방식을 MSI 로
 * 맞춘다.
 *
 * MSI 만 쓴다는 것이 플랫폼 판과 가장 크게 다른 점이다. PCI 장치는
 * 배선 인터럽트를 쓸 수 없는 것은 아니지만, 그쪽 경로를 이 드라이버가
 * 지원하지 않아 MSI 를 못 쓰는 하드웨어는 아예 거부한다.
 *
 * 실행 컨텍스트: 드라이버 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   PCI 버스 → .probe = [이 함수] → riscv_iommu_init()
 */
static int riscv_iommu_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct device *dev = &pdev->dev;	/* [한국어] 로그와 자원 관리에 쓸 커널 장치. */
	struct riscv_iommu_device *iommu;	/* [한국어] 만들 드라이버 상태. */
	int rc, vec;	/* [한국어] 중간 결과와 인터럽트 반복자. */

	rc = pcim_enable_device(pdev);	/* [한국어] 장치를 깨우고 자원을 쓸 수 있게 한다. devres 판이라 실패해도 저절로 되돌아간다. */
	if (rc)	/* [한국어] 장치를 깨우지 못했으면 진행할 수 없다. */
		return rc;

	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM))	/* [한국어] 첫 BAR 가 메모리 창이어야 한다. */
		return -ENODEV;	/* [한국어] 그렇지 않으면 우리가 아는 장치가 아니다. */

	if (pci_resource_len(pdev, 0) < RISCV_IOMMU_REG_SIZE)	/* [한국어] 레지스터를 모두 담을 만큼 커야 한다. */
		return -ENODEV;	/* [한국어] 작으면 규격을 따르지 않는 장치다. */

	rc = pcim_iomap_regions(pdev, BIT(0), pci_name(pdev));	/* [한국어] 첫 BAR 만 매핑한다 — devres 가 해제를 맡는다. */
	if (rc)	/* [한국어] 매핑에 실패했다. */
		return dev_err_probe(dev, rc, "pcim_iomap_regions failed\n");	/* [한국어] 프로브 실패를 한 줄로 기록하며 돌려준다. */

	iommu = devm_kzalloc(dev, sizeof(*iommu), GFP_KERNEL);	/* [한국어] 드라이버 상태를 잡는다. */
	if (!iommu)	/* [한국어] 상태를 못 잡았으면. */
		return -ENOMEM;

	iommu->dev = dev;	/* [한국어] 되짚어 갈 수 있게. */
	iommu->reg = pcim_iomap_table(pdev)[0];	/* [한국어] 방금 매핑한 첫 BAR 의 커널 주소 — 모든 레지스터 접근의 기준점이다. */

	pci_set_master(pdev);	/* [한국어] 이 장치가 스스로 메모리를 읽고 쓸 수 있게 한다 — IOMMU 도 표와 큐를 DMA 로 읽으므로 필요하다. */
	dev_set_drvdata(dev, iommu);	/* [한국어] 아래 remove/shutdown 이 이 값으로 상태를 되찾는다. */

	/* Check device reported capabilities / features. */
	/* [한국어] (위 영어 주석 참고) 하드웨어가 무엇을 지원하는지 먼저 읽는다. */
	iommu->caps = riscv_iommu_readq(iommu, RISCV_IOMMU_REG_CAPABILITIES);	/* [한국어] 능력 비트 — 페이지 테이블 형식과 인터럽트 방식이 여기 담긴다. */
	iommu->fctl = riscv_iommu_readl(iommu, RISCV_IOMMU_REG_FCTL);	/* [한국어] 지금 켜져 있는 기능 설정 — 아래에서 인터럽트 방식을 고쳐 쓴다. */

	/* The PCI driver only uses MSIs, make sure the IOMMU supports this */
	/* [한국어] (위 영어 주석 참고) 이 드라이버의 PCI 경로는 MSI 만 다룬다.
	 * 배선 인터럽트를 쓰려면 그 자원을 어디서 얻을지가 버스마다 달라
	 * 코드가 복잡해지는데, PCI 장치는 사실상 언제나 MSI 를 쓸 수 있어
	 * 그 경로를 만들지 않았다. */
	switch (FIELD_GET(RISCV_IOMMU_CAPABILITIES_IGS, iommu->caps)) {	/* [한국어] 이 하드웨어가 어떤 인터럽트 방식을 지원하는가. */
	case RISCV_IOMMU_CAPABILITIES_IGS_MSI:	/* [한국어] MSI 만 지원하거나. */
	case RISCV_IOMMU_CAPABILITIES_IGS_BOTH:	/* [한국어] 둘 다 지원하면. */
		break;	/* [한국어] 쓸 수 있다. */
	default:	/* [한국어] 배선 인터럽트만 지원한다면. */
		return dev_err_probe(dev, -ENODEV,	/* [한국어] MSI 를 못 받으면 폴트와 완료를 알 길이 없다. */
				     "unable to use message-signaled interrupts\n");	/* [한국어] 이 경로로는 다룰 수 없다. */
	}

	/* Allocate and assign IRQ vectors for the various events */
	/* [한국어] (위 영어 주석 참고) 사건 종류마다 벡터를 하나씩 원하지만, 적게
	 * 받아도 동작한다 — 여러 사건이 한 벡터를 나눠 쓰게 된다. */
	rc = pci_alloc_irq_vectors(pdev, 1, RISCV_IOMMU_INTR_COUNT,	/* [한국어] 최소 하나, 최대 사건 수만큼. */
				   PCI_IRQ_MSIX | PCI_IRQ_MSI);	/* [한국어] MSI-X 를 먼저 시도하고 안 되면 MSI 로 물러난다. */
	if (rc <= 0)	/* [한국어] 하나도 못 받았다면. */
		return dev_err_probe(dev, -ENODEV,
				     "unable to allocate irq vectors\n");	/* [한국어] 폴트와 명령 완료를 알 길이 없어 진행할 수 없다. */

	iommu->irqs_count = rc;	/* [한국어] 실제로 받은 개수 — 요청보다 적을 수 있다. */
	for (vec = 0; vec < iommu->irqs_count; vec++)	/* [한국어] 받은 벡터마다. */
		iommu->irqs[vec] = msi_get_virq(dev, vec);	/* [한국어] 커널이 쓰는 가상 인터럽트 번호로 바꿔 담는다. */

	/* Enable message-signaled interrupts, fctl.WSI */
	/* [한국어] (위 영어 주석 참고) WSI 는 "배선 인터럽트를 쓴다"는 뜻이므로,
	 * MSI 를 쓰려면 그 비트를 내려야 한다. 이름과 동작이 반대라 헷갈리기 쉽다. */
	if (iommu->fctl & RISCV_IOMMU_FCTL_WSI) {	/* [한국어] 지금 배선 방식으로 되어 있다면. */
		iommu->fctl ^= RISCV_IOMMU_FCTL_WSI;	/* [한국어] 그 비트만 뒤집어 내린다 — 다른 설정은 그대로 살린다. */
		riscv_iommu_writel(iommu, RISCV_IOMMU_REG_FCTL, iommu->fctl);	/* [한국어] 하드웨어에 알린다. */
	}

	return riscv_iommu_init(iommu);	/* [한국어] 여기서부터는 버스와 무관한 공통 초기화가 맡는다. */
}

/*
 * [한국어]
 * riscv_iommu_pci_remove - 이 IOMMU 를 내린다
 *
 * @pdev: 대상 PCI 장치.
 *
 * 공통 정리 함수를 부르는 것이 전부다. BAR 매핑과 MSI 는 devres 가
 * 알아서 거둔다.
 *
 * 실행 컨텍스트: 드라이버 제거. 잠들 수 있다.
 *
 * 호출 체인:
 *   PCI 버스 → .remove = [이 함수] → riscv_iommu_remove()
 */
static void riscv_iommu_pci_remove(struct pci_dev *pdev)
{
	struct riscv_iommu_device *iommu = dev_get_drvdata(&pdev->dev);	/* [한국어] 프로브가 붙여 둔 상태. */

	riscv_iommu_remove(iommu);	/* [한국어] 코어 등록을 풀고 큐와 디렉터리를 거둔다. */
}

/*
 * [한국어]
 * riscv_iommu_pci_shutdown - 시스템이 꺼질 때 변환을 멈춘다
 *
 * @pdev: 대상 PCI 장치.
 *
 * 자원을 거두지는 않고 하드웨어만 멈춘다. kexec 로 다음 커널을 띄울 때
 * 알 수 없는 매핑을 물려주지 않기 위해서다.
 *
 * 실행 컨텍스트: 시스템 종료. 잠들 수 있다.
 *
 * 호출 체인:
 *   PCI 버스 → .shutdown = [이 함수] → riscv_iommu_disable()
 */
static void riscv_iommu_pci_shutdown(struct pci_dev *pdev)
{
	struct riscv_iommu_device *iommu = dev_get_drvdata(&pdev->dev);	/* [한국어] 프로브가 붙여 둔 상태. */

	riscv_iommu_disable(iommu);	/* [한국어] 변환과 큐를 멈춘다 — 다음 커널이 깨끗한 상태에서 시작하도록. */
}

/* [한국어] 이 드라이버가 맡을 PCI 장치 목록.
 *
 * 두 항목뿐이다 — QEMU 가 흉내 내는 장치와 Rivos 의 실제 하드웨어.
 * RISC-V IOMMU 규격이 아직 새로워 실물이 드물기 때문이다. */
static const struct pci_device_id riscv_iommu_pci_tbl[] = {
	{PCI_VDEVICE(REDHAT, PCI_DEVICE_ID_REDHAT_RISCV_IOMMU), 0},	/* [한국어] QEMU 의 흉내 장치 — 이 드라이버를 쓰는 가장 흔한 경우다. */
	{PCI_VDEVICE(RIVOS, PCI_DEVICE_ID_RIVOS_RISCV_IOMMU_GA), 0},	/* [한국어] Rivos 의 실제 하드웨어. */
	{0,}	/* [한국어] 목록의 끝을 알리는 빈 항목. */
};

/* [한국어] PCI 버스에 등록할 드라이버 서술.
 *
 * suppress_bind_attrs 가 눈에 띈다 — sysfs 로 이 드라이버를 임의로
 * 떼었다 붙였다 할 수 없게 막는다. IOMMU 를 실행 중에 떼면 그 뒤의
 * 모든 장치가 매핑을 잃기 때문이다. */
static struct pci_driver riscv_iommu_pci_driver = {
	.name = KBUILD_MODNAME,	/* [한국어] 빌드 시스템이 정한 모듈 이름을 그대로 쓴다. */
	.id_table = riscv_iommu_pci_tbl,	/* [한국어] 위 장치 목록. */
	.probe = riscv_iommu_pci_probe,	/* [한국어] 장치를 찾아 준비한다. */
	.remove = riscv_iommu_pci_remove,	/* [한국어] 자원을 거둔다. */
	.shutdown = riscv_iommu_pci_shutdown,	/* [한국어] 종료 때 변환을 멈춘다. */
	.driver = {
		.suppress_bind_attrs = true,	/* [한국어] 실행 중 떼기를 막는다 — 뗀 순간 뒤의 모든 장치가 매핑을 잃는다. */
	},
};

builtin_pci_driver(riscv_iommu_pci_driver);	/* [한국어] 모듈이 아니라 커널에 내장으로만 빌드된다 — IOMMU 는 다른 장치보다 먼저 올라와야 해서 나중에 적재되는 모듈로는 곤란하다. */
