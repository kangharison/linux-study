// SPDX-License-Identifier: GPL-2.0
/*
 * Volume Management Device driver
 * Copyright (c) 2015, Intel Corporation.
 */

#include <linux/device.h>          /* PCI/NVMe: 장치 모델 기반으로 NVMe SSD 열거 시 사용 */
#include <linux/interrupt.h>       /* PCI/NVMe: NVMe MSI-X 인터럽트 demux 처리를 위한 핵심 헤더 */
#include <linux/irq.h>             /* PCI/NVMe: NVMe 장치가 할당받는 virq 관리 */
#include <linux/irqchip/irq-msi-lib.h> /* PCI/NVMe: MSI/MSI-X 도메인 생성 시 NVMe MSI 연결 */
#include <linux/kernel.h>          /* PCI/NVMe: 커널 기본 매크로, NVMe 호스트 드라이버와 동일 환경 */
#include <linux/module.h>          /* PCI/NVMe: 모듈 로드/언로드, NVMe pci.c와 동일 PCI 드라이버 모델 */
#include <linux/msi.h>             /* PCI/NVMe: NVMe 장치 MSI/MSI-X 할당/해제 API */
#include <linux/pci.h>             /* PCI/NVMe: NVMe SSD의 PCIe 열거/바인딩/레지스터 접근 핵심 */
#include <linux/pci-acpi.h>        /* PCI/NVMe: ACPI companion 검색, NVMe 장치 전원/핫플러그 연동 */
#include <linux/pci-ecam.h>        /* PCI/NVMe: ECAM 기반 config 접근, NVMe 루트 포트 탐색에 사용 */
#include <linux/srcu.h>            /* PCI/NVMe: MSI-X demux 리스트 RCU 동기화, NVMe IRQ 핸들러 안전성 */
#include <linux/rculist.h>         /* PCI/NVMe: virq demux 리스트 RCU 순회 */
#include <linux/rcupdate.h>        /* PCI/NVMe: NVMe IRQ 핸들러 등록/해제 RCU 보호 */

#include <xen/xen.h>               /* PCI/NVMe: Xen 환경에서 NVMe MSI bypass 제한 여부 판단 */

#include <asm/irqdomain.h>         /* PCI/NVMe: x86 MSI 주소/destid 구성, NVMe MSI 메시지 작성 */

#define VMD_CFGBAR	0           /* PCI/NVMe: VMD config 공간 BAR0, NVMe 루트 포트 ECAM으로 매핑 */
#define VMD_MEMBAR1	2           /* PCI/NVMe: VMD MEMBAR1, 하위 NVMe SSD MMIO/PCIe 메모리 창 */
#define VMD_MEMBAR2	4           /* PCI/NVMe: VMD MEMBAR2, NVMe BAR 메모리 및 shadow 레지스터 창 */

#define PCI_REG_VMCAP		0x40 /* PCI/NVMe: VMD capability 레지스터, 버스 제한 정보 판독 */
#define BUS_RESTRICT_CAP(vmcap)	(vmcap & 0x1) /* PCI/NVMe: 버스 제한 기능 유무, NVMe 도메인 범위 결정 */
#define PCI_REG_VMCONFIG	0x44 /* PCI/NVMe: VMD config 레지스터, MSI remap 및 버스 오프셋 제어 */
#define BUS_RESTRICT_CFG(vmcfg)	((vmcfg >> 8) & 0x3) /* PCI/NVMe: 버스 오프셋 값, NVMe 하위 버스 시작점 계산 */
#define VMCONFIG_MSI_REMAP	0x2  /* PCI/NVMe: MSI remap 비트, NVMe MSI-X를 VMD 벡터로 재매핑 제어 */
#define PCI_REG_VMLOCK		0x70 /* PCI/NVMe: VMD lock/shadow enable 레지스터, NVMe 물리 주소 힌트 */
#define MB2_SHADOW_EN(vmlock)	(vmlock & 0x2) /* PCI/NVMe: MEMBAR2 shadow 레지스터 활성화 여부 */

#define MB2_SHADOW_OFFSET	0x2000 /* PCI/NVMe: MEMBAR2 내 shadow 레지스터 오프셋, NVMe BAR 물리 주소 힌트 */
#define MB2_SHADOW_SIZE		16     /* PCI/NVMe: shadow 레지스터 크기(2개 64bit), NVMe 메모리 창 변환용 */

enum vmd_features {
	/*
	 * Device may contain registers which hint the physical location of the
	 * membars, in order to allow proper address translation during
	 * resource assignment to enable guest virtualization
	 */
	VMD_FEAT_HAS_MEMBAR_SHADOW		= (1 << 0), /* PCI/NVMe: 게스트에서 NVMe BAR 물리 주소 변환 힌트 제공 */

	/*
	 * Device may provide root port configuration information which limits
	 * bus numbering
	 */
	VMD_FEAT_HAS_BUS_RESTRICTIONS		= (1 << 1), /* PCI/NVMe: NVMe 루트 포트 버스 번호 제한 지원 */

	/*
	 * Device contains physical location shadow registers in
	 * vendor-specific capability space
	 */
	VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP	= (1 << 2), /* PCI/NVMe: 벤더 특수 capability에 NVMe 물리 주소 shadow */

	/*
	 * Device may use MSI-X vector 0 for software triggering and will not
	 * be used for MSI remapping
	 */
	VMD_FEAT_OFFSET_FIRST_VECTOR		= (1 << 3), /* PCI/NVMe: VMD 자체용 vector 0 사용, NVMe MSI는 1번부터 */

	/*
	 * Device can bypass remapping MSI-X transactions into its MSI-X table,
	 * avoiding the requirement of a VMD MSI domain for child device
	 * interrupt handling.
	 */
	VMD_FEAT_CAN_BYPASS_MSI_REMAP		= (1 << 4), /* PCI/NVMe: NVMe MSI-X가 VMD 테이블 거치지 않고 직접 전달 */

	/*
	 * Enable ASPM and LTR settings on devices that aren't configured by
	 * BIOS. This is needed for laptops, which require these settings for
	 * proper power management of the SoC.
	 */
	VMD_FEAT_BIOS_PM_QUIRK		= (1 << 5), /* PCI/NVMe: BIOS 미설정 시 NVMe SSD ASPM/LTR 강제 활성화 */
};

#define VMD_BIOS_PM_QUIRK_LTR	0x1003	/* 3145728 ns */ /* PCI/NVMe: NVMe 장치 기본 LTR 지연 값 */

#define VMD_FEATS_CLIENT	(VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP |	\
				 VMD_FEAT_HAS_BUS_RESTRICTIONS |	\
				 VMD_FEAT_OFFSET_FIRST_VECTOR |		\
				 VMD_FEAT_BIOS_PM_QUIRK) /* PCI/NVMe: 클라이언트 플랫폼 VMD 기능 조합, NVMe 노트북 최적화 */

static DEFINE_IDA(vmd_instance_ida); /* PCI/NVMe: VMD 인스턴스 ID 할당, NVMe 도메인 식별자 생성용 */

/*
 * Lock for manipulating VMD IRQ lists.
 */
static DEFINE_RAW_SPINLOCK(list_lock); /* PCI/NVMe: NVMe MSI/MSI-X demux 리스트 보호용 원시 스핀락 */

/**
 * struct vmd_irq - private data to map driver IRQ to the VMD shared vector
 * @node:	list item for parent traversal.
 * @irq:	back pointer to parent.
 * @enabled:	true if driver enabled IRQ
 * @virq:	the virtual IRQ value provided to the requesting driver.
 *
 * Every MSI/MSI-X IRQ requested for a device in a VMD domain will be mapped to
 * a VMD IRQ using this structure.
 */
struct vmd_irq {
	struct list_head	node;     /* PCI/NVMe: NVMe 장치 virq가 공유 VMD 벡터 리스트에 연결 */
	struct vmd_irq_list	*irq;     /* PCI/NVMe: NVMe MSI/MSI-X가 매핑된 VMD 공유 벡터 */
	bool			enabled;  /* PCI/NVMe: NVMe 드라이버가 해당 IRQ를 enable 했는지 표시 */
	unsigned int		virq;     /* PCI/NVMe: NVMe 호스트 드라이버가 할당받은 Linux 가상 IRQ 번호 */
};

/**
 * struct vmd_irq_list - list of driver requested IRQs mapping to a VMD vector
 * @irq_list:	the list of irq's the VMD one demuxes to.
 * @srcu:	SRCU struct for local synchronization.
 * @count:	number of child IRQs assigned to this vector; used to track
 *		sharing.
 * @virq:	The underlying VMD Linux interrupt number
 */
struct vmd_irq_list {
	struct list_head	irq_list; /* PCI/NVMe: 이 VMD 벡터로 demux될 NVMe 장치 virq들 */
	struct srcu_struct	srcu;     /* PCI/NVMe: NVMe IRQ 핸들러 등록/해제와 동시 수행 방지 */
	unsigned int		count;    /* PCI/NVMe: 이 벡터에 할당된 NVMe MSI/MSI-X 개수, 부하 분산 기준 */
	unsigned int		virq;     /* PCI/NVMe: VMD 하드웨어 MSI-X 벡터의 Linux IRQ 번호 */
};

struct vmd_dev {
	struct pci_dev		*dev;        /* PCI/NVMe: VMD 자체 PCI 장치, NVMe 루트 복합체 상위 */

	raw_spinlock_t		cfg_lock;    /* PCI/NVMe: NVMe config 공간 직렬화, deadlock 방지 */
	void __iomem		*cfgbar;     /* PCI/NVMe: NVMe 루트 포트/엔드포인트 ECAM 매핑 주소 */

	int msix_count;                       /* PCI/NVMe: VMD가 사용하는 MSI-X 벡터 수, NVMe demux 범위 */
	struct vmd_irq_list	*irqs;        /* PCI/NVMe: VMD MSI-X 벡터별 NVMe virq demux 리스트 배열 */

	struct pci_sysdata	sysdata;      /* PCI/NVMe: NVMe 버스의 domain/node 등 PCIe host bridge 데이터 */
	struct resource		resources[3]; /* PCI/NVMe: NVMe 도메인용 bus/membar 리소스 창 */
	struct irq_domain	*irq_domain;  /* PCI/NVMe: 하위 NVMe 장치 MSI/MSI-X 할당용 irq domain */
	struct pci_bus		*bus;         /* PCI/NVMe: VMD 아래 NVMe SSD가 연결된 가상 루트 버스 */
	u8			busn_start;   /* PCI/NVMe: VMD 내 NVMe 버스 번호 시작 오프셋 */
	u8			first_vec;    /* PCI/NVMe: NVMe MSI demux에 사용 가능한 첫 VMD MSI-X 벡터 */
	char			*name;        /* PCI/NVMe: VMD 인스턴스 이름, NVMe 장치 로그/IRQ 식별 */
	int			instance;     /* PCI/NVMe: VMD 인스턴스 번호, NVMe PCIe 도메인 구분 */
};

static inline struct vmd_dev *vmd_from_bus(struct pci_bus *bus)
{
	return container_of(bus->sysdata, struct vmd_dev, sysdata); /* PCI/NVMe: NVMe 버스의 sysdata에서 vmd_dev 획득 */
}

static inline unsigned int index_from_irqs(struct vmd_dev *vmd,
					   struct vmd_irq_list *irqs)
{
	return irqs - vmd->irqs; /* PCI/NVMe: VMD 벡터 배열 내 인덱스, NVMe MSI 메시지 destid로 사용 */
}

/*
 * Drivers managing a device in a VMD domain allocate their own IRQs as before,
 * but the MSI entry for the hardware it's driving will be programmed with a
 * destination ID for the VMD MSI-X table.  The VMD muxes interrupts in its
 * domain into one of its own, and the VMD driver de-muxes these for the
 * handlers sharing that VMD IRQ.  The vmd irq_domain provides the operations
 * and irq_chip to set this up.
 */
static void vmd_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct vmd_irq *vmdirq = data->chip_data;         /* PCI/NVMe: NVMe 장치의 virq가 매핑된 vmd_irq */
	struct vmd_irq_list *irq = vmdirq->irq;           /* PCI/NVMe: NVMe MSI가 demux될 VMD 공유 벡터 */
	struct vmd_dev *vmd = irq_data_get_irq_handler_data(data); /* PCI/NVMe: 상위 VMD 핸들러 데이터 */

	memset(msg, 0, sizeof(*msg));                     /* PCI/NVMe: NVMe MSI 메시지 초기화 */
	msg->address_hi = X86_MSI_BASE_ADDRESS_HIGH;      /* PCI/NVMe: x86 MSI 상위 주소, NVMe MSI 메시지 표준값 */
	msg->arch_addr_lo.base_address = X86_MSI_BASE_ADDRESS_LOW; /* PCI/NVMe: x86 MSI 하위 베이스 주소 */
	msg->arch_addr_lo.destid_0_7 = index_from_irqs(vmd, irq); /* PCI/NVMe: VMD 공유 벡터 인덱스를 NVMe MSI destid로 설정 */
}

static void vmd_irq_enable(struct irq_data *data)
{
	struct vmd_irq *vmdirq = data->chip_data;         /* PCI/NVMe: enable 할 NVMe virq의 VMD 매핑 구조체 */

	scoped_guard(raw_spinlock_irqsave, &list_lock) {  /* PCI/NVMe: NVMe virq demux 리스트 임계구역 진입 */
		WARN_ON(vmdirq->enabled);                 /* PCI/NVMe: NVMe IRQ 중복 enable 검사 */
		list_add_tail_rcu(&vmdirq->node, &vmdirq->irq->irq_list); /* PCI/NVMe: NVMe virq를 VMD 공유 벡터 리스트에 RCU 추가 */
		vmdirq->enabled = true;                   /* PCI/NVMe: NVMe IRQ enable 상태 기록 */
	}
}

static void vmd_pci_msi_enable(struct irq_data *data)
{
	vmd_irq_enable(data->parent_data);                /* PCI/NVMe: NVMe MSI/MSI-X virq를 상위 VMD 벡터에 연결 */
	data->chip->irq_unmask(data);                     /* PCI/NVMe: NVMe 장치 MSI 마스크 해제, 인터럽트 허용 */
}

static unsigned int vmd_pci_msi_startup(struct irq_data *data)
{
	vmd_pci_msi_enable(data);                         /* PCI/NVMe: NVMe MSI/MSI-X 초기화 시 enable 수행 */
	return 0;                                         /* PCI/NVMe: NVMe IRQ 시작 성공 반환 */
}

static void vmd_irq_disable(struct irq_data *data)
{
	struct vmd_irq *vmdirq = data->chip_data;         /* PCI/NVMe: disable 할 NVMe virq의 VMD 매핑 구조체 */

	scoped_guard(raw_spinlock_irqsave, &list_lock) {  /* PCI/NVMe: NVMe virq demux 리스트 임계구역 진입 */
		if (vmdirq->enabled) {                    /* PCI/NVMe: NVMe IRQ가 enable 상태일 때만 제거 */
			list_del_rcu(&vmdirq->node);      /* PCI/NVMe: NVMe virq를 VMD 공유 벡터 리스트에서 RCU 삭제 */
			vmdirq->enabled = false;          /* PCI/NVMe: NVMe IRQ disable 상태 기록 */
		}
	}
}

static void vmd_pci_msi_disable(struct irq_data *data)
{
	data->chip->irq_mask(data);                       /* PCI/NVMe: NVMe 장치 MSI 마스크, 인터럽트 차단 */
	vmd_irq_disable(data->parent_data);               /* PCI/NVMe: NVMe virq를 상위 VMD 벡터 리스트에서 제거 */
}

static void vmd_pci_msi_shutdown(struct irq_data *data)
{
	vmd_pci_msi_disable(data);                        /* PCI/NVMe: NVMe MSI/MSI-X 종료 시 disable 수행 */
}

static struct irq_chip vmd_msi_controller = {
	.name			= "VMD-MSI",                /* PCI/NVMe: NVMe MSI demux용 irq chip 이름 */
	.irq_compose_msi_msg	= vmd_compose_msi_msg,      /* PCI/NVMe: NVMe MSI 메시지(destid) 조합 콜백 */
};

/*
 * XXX: We can be even smarter selecting the best IRQ once we solve the
 * affinity problem.
 */
static struct vmd_irq_list *vmd_next_irq(struct vmd_dev *vmd, struct msi_desc *desc)
{
	int i, best;                                      /* PCI/NVMe: NVMe MSI demux 벡터 선택용 인덱스 */

	if (vmd->msix_count == 1 + vmd->first_vec)        /* PCI/NVMe: VMD MSI-X 벡터가 하나뿐이면 */
		return &vmd->irqs[vmd->first_vec];        /* PCI/NVMe: 모든 NVMe MSI를 첫 번째 공유 벡터로 */

	/*
	 * White list for fast-interrupt handlers. All others will share the
	 * "slow" interrupt vector.
	 */
	switch (msi_desc_to_pci_dev(desc)->class) {       /* PCI/NVMe: MSI 요청 장치 클래스 확인 */
	case PCI_CLASS_STORAGE_EXPRESS:                   /* PCI/NVMe: NVMe SSD(PCIe 스토리지)인 경우 */
		break;                                    /* PCI/NVMe: NVMe는 고속 인터럽트 전용 벡터 후보 */
	default:                                          /* PCI/NVMe: NVMe 외 장치는 느린 공유 벡터로 */
		return &vmd->irqs[vmd->first_vec];
	}

	scoped_guard(raw_spinlock_irq, &list_lock) {      /* PCI/NVMe: NVMe MSI 부하 분산을 위한 카운트 보호 */
		best = vmd->first_vec + 1;                /* PCI/NVMe: NVMe 전용 벡터 범위 시작 */
		for (i = best; i < vmd->msix_count; i++)  /* PCI/NVMe: NVMe MSI 할당 가능한 VMD 벡터 순회 */
			if (vmd->irqs[i].count < vmd->irqs[best].count) /* PCI/NVMe: 할당량이 가장 적은 VMD 벡터 선택 */
				best = i;
		vmd->irqs[best].count++;                  /* PCI/NVMe: 선택된 VMD 벡터에 NVMe MSI 할당 카운트 증가 */
	}

	return &vmd->irqs[best];                          /* PCI/NVMe: 선택된 VMD 공유 벡터 반환 */
}

static void vmd_msi_free(struct irq_domain *domain, unsigned int virq,
			 unsigned int nr_irqs);

static int vmd_msi_alloc(struct irq_domain *domain, unsigned int virq,
			 unsigned int nr_irqs, void *arg)
{
	struct msi_desc *desc = ((msi_alloc_info_t *)arg)->desc; /* PCI/NVMe: NVMe 장치의 MSI descriptor */
	struct vmd_dev *vmd = domain->host_data;          /* PCI/NVMe: NVMe가 속한 VMD 도메인 */
	struct vmd_irq *vmdirq;                           /* PCI/NVMe: 각 NVMe MSI/MSI-X 할당 정보 */

	for (int i = 0; i < nr_irqs; ++i) {               /* PCI/NVMe: NVMe가 요청한 MSI/MSI-X 개수만큼 반복 */
		vmdirq = kzalloc_obj(*vmdirq);            /* PCI/NVMe: NVMe virq당 하나의 vmd_irq 할당 */
		if (!vmdirq) {                            /* PCI/NVMe: NVMe MSI 매핑 메모리 부족 */
			vmd_msi_free(domain, virq, i);    /* PCI/NVMe: 이미 할당된 NVMe MSI 롤백 */
			return -ENOMEM;                   /* PCI/NVMe: NVMe 장치 MSI 할당 실패 반환 */
		}

		INIT_LIST_HEAD(&vmdirq->node);            /* PCI/NVMe: NVMe virq demux 리스트 노드 초기화 */
		vmdirq->irq = vmd_next_irq(vmd, desc);    /* PCI/NVMe: NVMe MSI가 매핑될 VMD 공유 벡터 선택 */
		vmdirq->virq = virq + i;                  /* PCI/NVMe: NVMe 호스트 드라이버용 Linux virq 저장 */

		irq_domain_set_info(domain, virq + i, vmdirq->irq->virq, /* PCI/NVMe: NVMe virq에 VMD irq_chip 연결 */
				    &vmd_msi_controller, vmdirq,
				    handle_untracked_irq, vmd, NULL);
	}

	return 0;                                         /* PCI/NVMe: NVMe MSI/MSI-X 할당 성공 */
}

static void vmd_msi_free(struct irq_domain *domain, unsigned int virq,
			 unsigned int nr_irqs)
{
	struct irq_data *irq_data;                        /* PCI/NVMe: 해제할 NVMe virq의 irq_data */
	struct vmd_irq *vmdirq;                           /* PCI/NVMe: NVMe virq의 VMD 매핑 구조체 */

	for (int i = 0; i < nr_irqs; ++i) {               /* PCI/NVMe: NVMe가 반납하는 MSI/MSI-X 개수만큼 반복 */
		irq_data = irq_domain_get_irq_data(domain, virq + i); /* PCI/NVMe: NVMe virq의 domain 데이터 획득 */
		vmdirq = irq_data->chip_data;             /* PCI/NVMe: NVMe virq와 연결된 vmd_irq 획득 */

		synchronize_srcu(&vmdirq->irq->srcu);     /* PCI/NVMe: NVMe IRQ 핸들러 완료 대기, 안전한 해제 */

		/* XXX: Potential optimization to rebalance */
		scoped_guard(raw_spinlock_irq, &list_lock) /* PCI/NVMe: NVMe MSI 카운트 동시 수정 방지 */
			vmdirq->irq->count--;             /* PCI/NVMe: VMD 공유 벡터의 NVMe MSI 할당 카운트 감소 */

		kfree(vmdirq);                            /* PCI/NVMe: NVMe virq 매핑 구조체 메모리 해제 */
	}
}

static const struct irq_domain_ops vmd_msi_domain_ops = {
	.alloc		= vmd_msi_alloc,                  /* PCI/NVMe: NVMe MSI/MSI-X 할당 콜백 */
	.free		= vmd_msi_free,                   /* PCI/NVMe: NVMe MSI/MSI-X 해제 콜백 */
};

static bool vmd_init_dev_msi_info(struct device *dev, struct irq_domain *domain,
				  struct irq_domain *real_parent,
				  struct msi_domain_info *info)
{
	if (!msi_lib_init_dev_msi_info(dev, domain, real_parent, info)) /* PCI/NVMe: NVMe MSI 도메인 공통 정보 초기화 */
		return false;                         /* PCI/NVMe: NVMe MSI 도메인 초기화 실패 */

	info->chip->irq_startup		= vmd_pci_msi_startup;    /* PCI/NVMe: NVMe MSI startup 콜백 등록 */
	info->chip->irq_shutdown	= vmd_pci_msi_shutdown;   /* PCI/NVMe: NVMe MSI shutdown 콜백 등록 */
	info->chip->irq_enable		= vmd_pci_msi_enable;     /* PCI/NVMe: NVMe MSI enable 콜백 등록 */
	info->chip->irq_disable		= vmd_pci_msi_disable;    /* PCI/NVMe: NVMe MSI disable 콜백 등록 */
	return true;                                      /* PCI/NVMe: NVMe MSI 도메인 초기화 성공 */
}

#define VMD_MSI_FLAGS_SUPPORTED	(MSI_GENERIC_FLAGS_MASK | MSI_FLAG_PCI_MSIX) /* PCI/NVMe: NVMe MSI/MSI-X 지원 플래그 */
#define VMD_MSI_FLAGS_REQUIRED	(MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_NO_AFFINITY) /* PCI/NVMe: NVMe MSI affinity 미지원 강제 */

static const struct msi_parent_ops vmd_msi_parent_ops = {
	.supported_flags	= VMD_MSI_FLAGS_SUPPORTED, /* PCI/NVMe: NVMe 장치에 허용할 MSI 플래그 */
	.required_flags		= VMD_MSI_FLAGS_REQUIRED,  /* PCI/NVMe: NVMe MSI 도메인에 강제할 플래그 */
	.bus_select_token	= DOMAIN_BUS_VMD_MSI,      /* PCI/NVMe: VMD MSI 버스 도메인 식별자 */
	.bus_select_mask	= MATCH_PCI_MSI,           /* PCI/NVMe: PCI MSI/MSI-X 매칭, NVMe MSI 포함 */
	.prefix			= "VMD-",                  /* PCI/NVMe: NVMe MSI domain 이름 접두사 */
	.init_dev_msi_info	= vmd_init_dev_msi_info,   /* PCI/NVMe: NVMe 장치 MSI info 초기화 콜백 */
};

static int vmd_create_irq_domain(struct vmd_dev *vmd)
{
	struct irq_domain_info info = {
		.size		= vmd->msix_count,        /* PCI/NVMe: VMD MSI-X 벡터 수만큼 NVMe MSI domain 크기 */
		.ops		= &vmd_msi_domain_ops,    /* PCI/NVMe: NVMe MSI alloc/free 콜백 연결 */
		.host_data	= vmd,                    /* PCI/NVMe: NVMe domain 조회용 VMD 포인터 */
	};

	info.fwnode = irq_domain_alloc_named_id_fwnode("VMD-MSI",		      /* PCI/NVMe: NVMe MSI domain용 fwnode 이름 */
					       vmd->sysdata.domain);   /* PCI/NVMe: NVMe PCIe 도메인 번호를 fwnode ID로 */
	if (!info.fwnode)                                 /* PCI/NVMe: NVMe MSI fwnode 할당 실패 */
		return -ENODEV;                           /* PCI/NVMe: NVMe MSI domain 생성 불가 */

	vmd->irq_domain = msi_create_parent_irq_domain(&info, /* PCI/NVMe: NVMe MSI parent irq domain 생성 */
					       &vmd_msi_parent_ops);
	if (!vmd->irq_domain) {                           /* PCI/NVMe: NVMe MSI domain 생성 실패 */
		irq_domain_free_fwnode(info.fwnode);      /* PCI/NVMe: NVMe MSI fwnode 롤백 */
		return -ENODEV;                           /* PCI/NVMe: NVMe MSI domain 생성 실패 반환 */
	}

	return 0;                                         /* PCI/NVMe: NVMe MSI domain 생성 성공 */
}

static void vmd_set_msi_remapping(struct vmd_dev *vmd, bool enable)
{
	u16 reg;                                          /* PCI/NVMe: VMD VMCONFIG 레지스터 값 */

	pci_read_config_word(vmd->dev, PCI_REG_VMCONFIG, &reg); /* PCI/NVMe: NVMe MSI remap 제어 레지스터 읽기 */
	reg = enable ? (reg & ~VMCONFIG_MSI_REMAP) :      /* PCI/NVMe: enable 시 MSI remap 비트 클리어 */
		       (reg | VMCONFIG_MSI_REMAP);      /* PCI/NVMe: disable 시 MSI remap 비트 설정 */
	pci_write_config_word(vmd->dev, PCI_REG_VMCONFIG, reg); /* PCI/NVMe: NVMe MSI remap 상태 기록 */
}

static void vmd_remove_irq_domain(struct vmd_dev *vmd)
{
	/*
	 * Some production BIOS won't enable remapping between soft reboots.
	 * Ensure remapping is restored before unloading the driver.
	 */
	if (!vmd->msix_count)                             /* PCI/NVMe: MSI-X 벡터가 없으면(비활성 상태) */
		vmd_set_msi_remapping(vmd, true);         /* PCI/NVMe: 소프트 리부트 전 NVMe MSI remap 복원 */

	if (vmd->irq_domain) {                            /* PCI/NVMe: NVMe MSI domain이 존재하면 */
		struct fwnode_handle *fn = vmd->irq_domain->fwnode; /* PCI/NVMe: NVMe MSI domain fwnode 저장 */

		irq_domain_remove(vmd->irq_domain);       /* PCI/NVMe: NVMe MSI domain 제거 */
		irq_domain_free_fwnode(fn);               /* PCI/NVMe: NVMe MSI domain fwnode 해제 */
	}
}

static void __iomem *vmd_cfg_addr(struct vmd_dev *vmd, struct pci_bus *bus,
				  unsigned int devfn, int reg, int len)
{
	unsigned int busnr_ecam = bus->number - vmd->busn_start; /* PCI/NVMe: NVMe 버스를 VMD ECAM 오프셋으로 변환 */
	u32 offset = PCIE_ECAM_OFFSET(busnr_ecam, devfn, reg); /* PCI/NVMe: NVMe 장치 ECAM 오프셋 계산 */

	if (offset + len >= resource_size(&vmd->dev->resource[VMD_CFGBAR])) /* PCI/NVMe: NVMe config 접근 범위 초과 검사 */
		return NULL;                          /* PCI/NVMe: NVMe 장치 config 주소 무효 */

	return vmd->cfgbar + offset;              /* PCI/NVMe: NVMe 루트 포트/엔드포인트 config 공간 주소 */
}

/*
 * CPU may deadlock if config space is not serialized on some versions of this
 * hardware, so all config space access is done under a spinlock.
 */
static int vmd_pci_read(struct pci_bus *bus, unsigned int devfn, int reg,
			int len, u32 *value)
{
	struct vmd_dev *vmd = vmd_from_bus(bus);          /* PCI/NVMe: NVMe 버스가 속한 VMD 획득 */
	void __iomem *addr = vmd_cfg_addr(vmd, bus, devfn, reg, len); /* PCI/NVMe: NVMe 장치 config 주소 계산 */

	if (!addr)                                        /* PCI/NVMe: NVMe config 주소가 유효하지 않으면 */
		return -EFAULT;                           /* PCI/NVMe: NVMe 레지스터 읽기 실패 */

	guard(raw_spinlock_irqsave)(&vmd->cfg_lock);      /* PCI/NVMe: NVMe config 읽기 직렬화, deadlock 방지 */
	switch (len) {                                    /* PCI/NVMe: NVMe config 접근 폭 선택 */
	case 1:
		*value = readb(addr);                 /* PCI/NVMe: NVMe 장치 8bit 레지스터 읽기 */
		return 0;                             /* PCI/NVMe: NVMe 8bit 읽기 성공 */
	case 2:
		*value = readw(addr);                 /* PCI/NVMe: NVMe 장치 16bit 레지스터 읽기 */
		return 0;                             /* PCI/NVMe: NVMe 16bit 읽기 성공 */
	case 4:
		*value = readl(addr);                 /* PCI/NVMe: NVMe 장치 32bit 레지스터 읽기 */
		return 0;                             /* PCI/NVMe: NVMe 32bit 읽기 성공 */
	default:
		return -EINVAL;                       /* PCI/NVMe: NVMe config에서 지원하지 않는 접근 폭 */
	}
}

/*
 * VMD h/w converts non-posted config writes to posted memory writes. The
 * read-back in this function forces the completion so it returns only after
 * the config space was written, as expected.
 */
static int vmd_pci_write(struct pci_bus *bus, unsigned int devfn, int reg,
			 int len, u32 value)
{
	struct vmd_dev *vmd = vmd_from_bus(bus);          /* PCI/NVMe: NVMe 버스가 속한 VMD 획득 */
	void __iomem *addr = vmd_cfg_addr(vmd, bus, devfn, reg, len); /* PCI/NVMe: NVMe 장치 config 주소 계산 */

	if (!addr)                                        /* PCI/NVMe: NVMe config 주소가 유효하지 않으면 */
		return -EFAULT;                           /* PCI/NVMe: NVMe 레지스터 쓰기 실패 */

	guard(raw_spinlock_irqsave)(&vmd->cfg_lock);      /* PCI/NVMe: NVMe config 쓰기 직렬화, deadlock 방지 */
	switch (len) {                                    /* PCI/NVMe: NVMe config 접근 폭 선택 */
	case 1:
		writeb(value, addr);                  /* PCI/NVMe: NVMe 장치 8bit 레지스터 쓰기 */
		readb(addr);                          /* PCI/NVMe: NVMe 쓰기 완료 강제(read-back) */
		return 0;                             /* PCI/NVMe: NVMe 8bit 쓰기 성공 */
	case 2:
		writew(value, addr);                  /* PCI/NVMe: NVMe 장치 16bit 레지스터 쓰기 */
		readw(addr);                          /* PCI/NVMe: NVMe 쓰기 완료 강제(read-back) */
		return 0;                             /* PCI/NVMe: NVMe 16bit 쓰기 성공 */
	case 4:
		writel(value, addr);                  /* PCI/NVMe: NVMe 장치 32bit 레지스터 쓰기 */
		readl(addr);                          /* PCI/NVMe: NVMe 쓰기 완료 강제(read-back) */
		return 0;                             /* PCI/NVMe: NVMe 32bit 쓰기 성공 */
	default:
		return -EINVAL;                       /* PCI/NVMe: NVMe config에서 지원하지 않는 접근 폭 */
	}
}

static struct pci_ops vmd_ops = {
	.read		= vmd_pci_read,                   /* PCI/NVMe: NVMe config 읽기 콜백 */
	.write		= vmd_pci_write,                  /* PCI/NVMe: NVMe config 쓰기 콜백 */
};

#ifdef CONFIG_ACPI
static struct acpi_device *vmd_acpi_find_companion(struct pci_dev *pci_dev)
{
	struct pci_host_bridge *bridge;                   /* PCI/NVMe: NVMe 장치의 상위 host bridge */
	u32 busnr, addr;                                  /* PCI/NVMe: NVMe 버스 상대 번호, ACPI 주소 */

	if (pci_dev->bus->ops != &vmd_ops)                /* PCI/NVMe: VMD 버스가 아닌 NVMe 장치는 무시 */
		return NULL;                          /* PCI/NVMe: VMD 외 NVMe 장치 companion 없음 */

	bridge = pci_find_host_bridge(pci_dev->bus);      /* PCI/NVMe: NVMe 버스의 host bridge 획득 */
	busnr = pci_dev->bus->number - bridge->bus->number; /* PCI/NVMe: NVMe 버스의 상대 번호 계산 */
	/*
	 * The address computation below is only applicable to relative bus
	 * numbers below 32.
	 */
	if (busnr > 31)                                   /* PCI/NVMe: NVMe 버스 번호가 32 이상이면 ACPI 주소 불가 */
		return NULL;                          /* PCI/NVMe: NVMe ACPI companion 탐색 실패 */

	addr = (busnr << 24) | ((u32)pci_dev->devfn << 16) | 0x8000FFFFU; /* PCI/NVMe: NVMe 장치 ACPI _ADR 값 조합 */

	dev_dbg(&pci_dev->dev, "Looking for ACPI companion (address 0x%x)\n",
		addr);                                    /* PCI/NVMe: NVMe 장치 ACPI companion 탐색 로그 */

	return acpi_find_child_device(ACPI_COMPANION(bridge->dev.parent), addr, /* PCI/NVMe: NVMe 장치 ACPI 자식 탐색 */
				      false);
}

static bool hook_installed;                           /* PCI/NVMe: NVMe ACPI companion hook 설치 여부 */

static void vmd_acpi_begin(void)
{
	if (pci_acpi_set_companion_lookup_hook(vmd_acpi_find_companion)) /* PCI/NVMe: NVMe ACPI companion 조회 hook 등록 */
		return;                               /* PCI/NVMe: hook 등록 실패 시 아무것도 안 함 */

	hook_installed = true;                            /* PCI/NVMe: NVMe ACPI hook 설치 완료 표시 */
}

static void vmd_acpi_end(void)
{
	if (!hook_installed)                              /* PCI/NVMe: NVMe ACPI hook이 설치되지 않았으면 */
		return;                               /* PCI/NVMe: 제거할 NVMe ACPI hook 없음 */

	pci_acpi_clear_companion_lookup_hook();           /* PCI/NVMe: NVMe ACPI companion hook 제거 */
	hook_installed = false;                           /* PCI/NVMe: NVMe ACPI hook 제거 상태 기록 */
}
#else
static inline void vmd_acpi_begin(void) { }           /* PCI/NVMe: ACPI 미빌드 시 NVMe companion hook noop */
static inline void vmd_acpi_end(void) { }             /* PCI/NVMe: ACPI 미빌드 시 NVMe companion hook noop */
#endif /* CONFIG_ACPI */

static void vmd_domain_reset(struct vmd_dev *vmd)
{
	u16 bus, max_buses = resource_size(&vmd->resources[0]); /* PCI/NVMe: NVMe 도메인 최대 버스 수 */
	u8 dev, functions, fn, hdr_type;                  /* PCI/NVMe: NVMe 장치/함수/헤더 타입 루프 변수 */
	char __iomem *base;                               /* PCI/NVMe: NVMe 루트 포트 config 베이스 주소 */

	for (bus = 0; bus < max_buses; bus++) {           /* PCI/NVMe: NVMe 도메인 내 모든 버스 순회 */
		for (dev = 0; dev < 32; dev++) {          /* PCI/NVMe: NVMe 도메인 내 모든 장치 슬롯 순회 */
			base = vmd->cfgbar + PCIE_ECAM_OFFSET(bus, /* PCI/NVMe: NVMe 장치 config 공간 시작 주소 */
					    PCI_DEVFN(dev, 0), 0);

			hdr_type = readb(base + PCI_HEADER_TYPE); /* PCI/NVMe: NVMe 장치 헤더 타입 읽기 */

			functions = (hdr_type & PCI_HEADER_TYPE_MFD) ? 8 : 1; /* PCI/NVMe: NVMe 다기능 장치 함수 수 결정 */
			for (fn = 0; fn < functions; fn++) {  /* PCI/NVMe: NVMe 장치 함수별 순회 */
				base = vmd->cfgbar + PCIE_ECAM_OFFSET(bus, /* PCI/NVMe: NVMe 함수별 config 주소 */
						    PCI_DEVFN(dev, fn), 0);

				hdr_type = readb(base + PCI_HEADER_TYPE) & /* PCI/NVMe: NVMe 함수 헤더 타입 추출 */
						PCI_HEADER_TYPE_MASK;

				if (hdr_type != PCI_HEADER_TYPE_BRIDGE || /* PCI/NVMe: NVMe 브리지가 아니면 스킵 */
				    (readw(base + PCI_CLASS_DEVICE) != /* PCI/NVMe: NVMe PCI 브리지 클래스 확인 */
				     PCI_CLASS_BRIDGE_PCI))
					continue;         /* PCI/NVMe: NVMe non-bridge 장치는 리소스 리셋 불필요 */

				/*
				 * Temporarily disable the I/O range before updating
				 * PCI_IO_BASE.
				 */
				writel(0x0000ffff, base + PCI_IO_BASE_UPPER16); /* PCI/NVMe: NVMe 루트 포트 I/O 상위 리미트 해제 */
				/* Update lower 16 bits of I/O base/limit */
				writew(0x00f0, base + PCI_IO_BASE); /* PCI/NVMe: NVMe 루트 포트 I/O 범위 리셋 */
				/* Update upper 16 bits of I/O base/limit */
				writel(0, base + PCI_IO_BASE_UPPER16); /* PCI/NVMe: NVMe 루트 포트 I/O 상위 범위 리셋 */

				/* MMIO Base/Limit */
				writel(0x0000fff0, base + PCI_MEMORY_BASE); /* PCI/NVMe: NVMe 루트 포트 32bit MMIO 범위 리셋 */

				/* Prefetchable MMIO Base/Limit */
				writel(0, base + PCI_PREF_LIMIT_UPPER32); /* PCI/NVMe: NVMe 루트 포트 프리페치 상위 리미트 리셋 */
				writel(0x0000fff0, base + PCI_PREF_MEMORY_BASE); /* PCI/NVMe: NVMe 루트 포트 프리페치 MMIO 범위 리셋 */
				writel(0xffffffff, base + PCI_PREF_BASE_UPPER32); /* PCI/NVMe: NVMe 루트 포트 프리페치 상위 베이스 리셋 */
			}
		}
	}
}

static void vmd_attach_resources(struct vmd_dev *vmd)
{
	vmd->dev->resource[VMD_MEMBAR1].child = &vmd->resources[1]; /* PCI/NVMe: NVMe MEMBAR1 리소스를 VMD 하위에 연결 */
	vmd->dev->resource[VMD_MEMBAR2].child = &vmd->resources[2]; /* PCI/NVMe: NVMe MEMBAR2 리소스를 VMD 하위에 연결 */
}

static void vmd_detach_resources(struct vmd_dev *vmd)
{
	vmd->dev->resource[VMD_MEMBAR1].child = NULL;     /* PCI/NVMe: NVMe MEMBAR1 리소스 연결 해제 */
	vmd->dev->resource[VMD_MEMBAR2].child = NULL;     /* PCI/NVMe: NVMe MEMBAR2 리소스 연결 해제 */
}

static int vmd_get_phys_offsets(struct vmd_dev *vmd, bool native_hint,
				resource_size_t *offset1,
				resource_size_t *offset2)
{
	struct pci_dev *dev = vmd->dev;                   /* PCI/NVMe: VMD PCI 장치, NVMe 메모리 창의 상위 */
	u64 phys1, phys2;                                 /* PCI/NVMe: NVMe MEMBAR1/2의 실제 물리 주소 */

	if (native_hint) {                                /* PCI/NVMe: VMD 자체 shadow 레지스터 사용 */
		u32 vmlock;                               /* PCI/NVMe: VMD shadow enable 레지스터 값 */
		int ret;                                  /* PCI/NVMe: NVMe 물리 주소 힌트 획득 결과 */

		ret = pci_read_config_dword(dev, PCI_REG_VMLOCK, &vmlock); /* PCI/NVMe: VMD VMLOCK 레지스터 읽기 */
		if (ret || PCI_POSSIBLE_ERROR(vmlock))    /* PCI/NVMe: NVMe 주소 변환 힌트 읽기 실패 */
			return -ENODEV;                   /* PCI/NVMe: NVMe 메모리 창 변환 정보 없음 */

		if (MB2_SHADOW_EN(vmlock)) {              /* PCI/NVMe: MEMBAR2 shadow가 활성화된 경우 */
			void __iomem *membar2;            /* PCI/NVMe: NVMe MEMBAR2 매핑 주소 */

			membar2 = pci_iomap(dev, VMD_MEMBAR2, 0); /* PCI/NVMe: NVMe MEMBAR2 메모리 매핑 */
			if (!membar2)                     /* PCI/NVMe: NVMe MEMBAR2 매핑 실패 */
				return -ENOMEM;           /* PCI/NVMe: NVMe 메모리 변환 정보 획득 실패 */
			phys1 = readq(membar2 + MB2_SHADOW_OFFSET); /* PCI/NVMe: NVMe MEMBAR1 실제 물리 주소 읽기 */
			phys2 = readq(membar2 + MB2_SHADOW_OFFSET + 8); /* PCI/NVMe: NVMe MEMBAR2 실제 물리 주소 읽기 */
			pci_iounmap(dev, membar2);        /* PCI/NVMe: NVMe MEMBAR2 매핑 해제 */
		} else
			return 0;                     /* PCI/NVMe: shadow 미활성 시 NVMe 주소 오프셋 0 */
	} else {                                          /* PCI/NVMe: Hypervisor-Emulated Vendor-Specific Capability */
		/* Hypervisor-Emulated Vendor-Specific Capability */
		int pos = pci_find_capability(dev, PCI_CAP_ID_VNDR); /* PCI/NVMe: NVMe 관련 벤더 capability 위치 */
		u32 reg, regu;                            /* PCI/NVMe: NVMe 물리 주소 하위/상위 레지스터 */

		pci_read_config_dword(dev, pos + 4, &reg); /* PCI/NVMe: NVMe shadow capability signature 읽기 */

		/* "SHDW" */
		if (pos && reg == 0x53484457) {           /* PCI/NVMe: NVMe SHDW signature 확인 */
			pci_read_config_dword(dev, pos + 8, &reg); /* PCI/NVMe: NVMe MEMBAR1 물리 주소 하위 32bit */
			pci_read_config_dword(dev, pos + 12, &regu); /* PCI/NVMe: NVMe MEMBAR1 물리 주소 상위 32bit */
			phys1 = (u64) regu << 32 | reg;   /* PCI/NVMe: NVMe MEMBAR1 64bit 물리 주소 조합 */

			pci_read_config_dword(dev, pos + 16, &reg); /* PCI/NVMe: NVMe MEMBAR2 물리 주소 하위 32bit */
			pci_read_config_dword(dev, pos + 20, &regu); /* PCI/NVMe: NVMe MEMBAR2 물리 주소 상위 32bit */
			phys2 = (u64) regu << 32 | reg;   /* PCI/NVMe: NVMe MEMBAR2 64bit 물리 주소 조합 */
		} else
			return 0;                     /* PCI/NVMe: SHDW 없으면 NVMe 주소 오프셋 0 */
	}

	*offset1 = dev->resource[VMD_MEMBAR1].start -     /* PCI/NVMe: NVMe MEMBAR1 VMM visible 시작 */
			(phys1 & PCI_BASE_ADDRESS_MEM_MASK); /* PCI/NVMe: NVMe MEMBAR1 실제 물리 주소 마스킹 */
	*offset2 = dev->resource[VMD_MEMBAR2].start -     /* PCI/NVMe: NVMe MEMBAR2 VMM visible 시작 */
			(phys2 & PCI_BASE_ADDRESS_MEM_MASK); /* PCI/NVMe: NVMe MEMBAR2 실제 물리 주소 마스킹 */

	return 0;                                         /* PCI/NVMe: NVMe 메모리 창 오프셋 계산 성공 */
}

static int vmd_get_bus_number_start(struct vmd_dev *vmd)
{
	struct pci_dev *dev = vmd->dev;                   /* PCI/NVMe: VMD 장치, NVMe 도메인 버스 범위 결정 */
	u16 reg;                                          /* PCI/NVMe: VMD VMCAP/VMCONFIG 레지스터 */

	pci_read_config_word(dev, PCI_REG_VMCAP, &reg);   /* PCI/NVMe: VMD 버스 제한 capability 읽기 */
	if (BUS_RESTRICT_CAP(reg)) {                      /* PCI/NVMe: NVMe 도메인 버스 제한 기능 존재 */
		pci_read_config_word(dev, PCI_REG_VMCONFIG, &reg); /* PCI/NVMe: NVMe 버스 오프셋 설정 읽기 */

		switch (BUS_RESTRICT_CFG(reg)) {          /* PCI/NVMe: NVMe 도메인 버스 시작점 판별 */
		case 0:
			vmd->busn_start = 0;          /* PCI/NVMe: NVMe 도메인 버스 0~127 */
			break;
		case 1:
			vmd->busn_start = 128;        /* PCI/NVMe: NVMe 도메인 버스 128~255 */
			break;
		case 2:
			vmd->busn_start = 224;        /* PCI/NVMe: NVMe 도메인 버스 224~255 */
			break;
		default:
			pci_err(dev, "Unknown Bus Offset Setting (%d)\n", /* PCI/NVMe: NVMe 버스 오프셋 오류 로그 */
				BUS_RESTRICT_CFG(reg));
			return -ENODEV;               /* PCI/NVMe: NVMe 도메인 버스 범위 설정 실패 */
		}
	}

	return 0;                                         /* PCI/NVMe: NVMe 도메인 버스 시작점 획득 성공 */
}

static irqreturn_t vmd_irq(int irq, void *data)
{
	struct vmd_irq_list *irqs = data;                 /* PCI/NVMe: 발생한 VMD 공유 벡터(NVMe MSI demux 대상) */
	struct vmd_irq *vmdirq;                           /* PCI/NVMe: demux할 개별 NVMe virq 항목 */
	int idx;                                          /* PCI/NVMe: SRCU read lock 인덱스 */

	idx = srcu_read_lock(&irqs->srcu);                /* PCI/NVMe: NVMe IRQ 핸들러 리스트 보호용 SRCU 잠금 */
	list_for_each_entry_rcu(vmdirq, &irqs->irq_list, node) /* PCI/NVMe: 이 VMD 벡터에 연결된 NVMe virq 순회 */
		generic_handle_irq(vmdirq->virq);         /* PCI/NVMe: NVMe 호스트 드라이버의 MSI/MSI-X 핸들러 호출 */
	srcu_read_unlock(&irqs->srcu, idx);               /* PCI/NVMe: NVMe IRQ demux SRCU 잠금 해제 */

	return IRQ_HANDLED;                               /* PCI/NVMe: NVMe MSI demux 처리 완료 */
}

static int vmd_alloc_irqs(struct vmd_dev *vmd)
{
	struct pci_dev *dev = vmd->dev;                   /* PCI/NVMe: VMD PCI 장치, NVMe MSI demux 상위 */
	int i, err;                                       /* PCI/NVMe: NVMe MSI-X 벡터 루프/에러 변수 */

	vmd->msix_count = pci_msix_vec_count(dev);        /* PCI/NVMe: VMD 하드웨어 MSI-X 벡터 최대 개수 조회 */
	if (vmd->msix_count < 0)                          /* PCI/NVMe: NVMe MSI-X 벡터 정보 획득 실패 */
		return -ENODEV;                           /* PCI/NVMe: NVMe MSI demux를 위한 벡터 없음 */

	vmd->msix_count = pci_alloc_irq_vectors(dev, vmd->first_vec + 1, /* PCI/NVMe: NVMe demux용 VMD MSI-X 벡터 할당 */
						vmd->msix_count, PCI_IRQ_MSIX);
	if (vmd->msix_count < 0)                          /* PCI/NVMe: VMD MSI-X 벡터 할당 실패 */
		return vmd->msix_count;                   /* PCI/NVMe: NVMe MSI demux 벡터 할당 실패 반환 */

	vmd->irqs = devm_kcalloc(&dev->dev, vmd->msix_count, sizeof(*vmd->irqs), /* PCI/NVMe: NVMe demux 리스트 배열 할당 */
				 GFP_KERNEL);
	if (!vmd->irqs)                                   /* PCI/NVMe: NVMe demux 리스트 메모리 부족 */
		return -ENOMEM;                           /* PCI/NVMe: NVMe MSI demux 구조체 할당 실패 */

	for (i = 0; i < vmd->msix_count; i++) {           /* PCI/NVMe: 각 VMD MSI-X 벡터별 NVMe demux 초기화 */
		err = init_srcu_struct(&vmd->irqs[i].srcu); /* PCI/NVMe: NVMe IRQ demux SRCU 초기화 */
		if (err)                                  /* PCI/NVMe: NVMe SRCU 초기화 실패 */
			return err;                       /* PCI/NVMe: NVMe IRQ demux 초기화 실패 반환 */

		INIT_LIST_HEAD(&vmd->irqs[i].irq_list);   /* PCI/NVMe: NVMe virq demux 리스트 초기화 */
		vmd->irqs[i].virq = pci_irq_vector(dev, i); /* PCI/NVMe: VMD MSI-X 벡터 i의 Linux IRQ 번호 */
		err = devm_request_irq(&dev->dev, vmd->irqs[i].virq, /* PCI/NVMe: NVMe MSI demux 핸들러 등록 */
				       vmd_irq, IRQF_NO_THREAD,
				       vmd->name, &vmd->irqs[i]);
		if (err)                                  /* PCI/NVMe: NVMe MSI demux 핸들러 등록 실패 */
			return err;                       /* PCI/NVMe: NVMe IRQ demux 등록 실패 반환 */
	}

	return 0;                                         /* PCI/NVMe: NVMe MSI demux 벡터 초기화 성공 */
}

/*
 * Since VMD is an aperture to regular PCIe root ports, only allow it to
 * control features that the OS is allowed to control on the physical PCI bus.
 */
static void vmd_copy_host_bridge_flags(struct pci_host_bridge *root_bridge,
				       struct pci_host_bridge *vmd_bridge)
{
	vmd_bridge->native_pcie_hotplug = root_bridge->native_pcie_hotplug; /* PCI/NVMe: NVMe 핫플러그 정책 상속 */
	vmd_bridge->native_shpc_hotplug = root_bridge->native_shpc_hotplug; /* PCI/NVMe: NVMe SHPC 핫플러그 정책 상속 */
	vmd_bridge->native_aer = root_bridge->native_aer; /* PCI/NVMe: NVMe AER 처리 정책 상속 */
	vmd_bridge->native_pme = root_bridge->native_pme; /* PCI/NVMe: NVMe PME 전원 이벤트 정책 상속 */
	vmd_bridge->native_ltr = root_bridge->native_ltr; /* PCI/NVMe: NVMe LTR 정책 상속 */
	vmd_bridge->native_dpc = root_bridge->native_dpc; /* PCI/NVMe: NVMe DPC 정책 상속 */
}

/*
 * Enable ASPM and LTR settings on devices that aren't configured by BIOS.
 */
static int vmd_pm_enable_quirk(struct pci_dev *pdev, void *userdata)
{
	unsigned long features = *(unsigned long *)userdata; /* PCI/NVMe: VMD 기능 플래그, NVMe PM quirk 적용 여부 */
	u16 ltr = VMD_BIOS_PM_QUIRK_LTR;                  /* PCI/NVMe: NVMe 장치 기본 LTR 지연 값 */
	u32 ltr_reg;                                      /* PCI/NVMe: NVMe LTR 레지스터 값 */
	int pos;                                          /* PCI/NVMe: NVMe LTR capability 오프셋 */

	if (!(features & VMD_FEAT_BIOS_PM_QUIRK))         /* PCI/NVMe: BIOS PM quirk 미지원 VMD면 */
		return 0;                             /* PCI/NVMe: NVMe ASPM/LTR 강제 설정 안 함 */

	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_LTR); /* PCI/NVMe: NVMe 장치 LTR capability 탐색 */
	if (!pos)                                         /* PCI/NVMe: NVMe 장치에 LTR capability 없음 */
		goto out_state_change;                /* PCI/NVMe: NVMe ASPM 상태만 변경 */

	/*
	 * Skip if the max snoop LTR is non-zero, indicating BIOS has set it
	 * so the LTR quirk is not needed.
	 */
	pci_read_config_dword(pdev, pos + PCI_LTR_MAX_SNOOP_LAT, &ltr_reg); /* PCI/NVMe: NVMe LTR max snoop 지연 읽기 */
	if (!!(ltr_reg & (PCI_LTR_VALUE_MASK | PCI_LTR_SCALE_MASK))) /* PCI/NVMe: BIOS가 NVMe LTR 설정했으면 */
		goto out_state_change;                /* PCI/NVMe: NVMe LTR quirk 불필요 */

	/*
	 * Set the default values to the maximum required by the platform to
	 * allow the deepest power management savings. Write as a DWORD where
	 * the lower word is the max snoop latency and the upper word is the
	 * max non-snoop latency.
	 */
	ltr_reg = (ltr << 16) | ltr;                      /* PCI/NVMe: NVMe snoop/non-snoop LTR 조합 */
	pci_write_config_dword(pdev, pos + PCI_LTR_MAX_SNOOP_LAT, ltr_reg); /* PCI/NVMe: NVMe LTR 기본값 기록 */
	pci_info(pdev, "VMD: Default LTR value set by driver\n"); /* PCI/NVMe: NVMe LTR 설정 로그 */

out_state_change:
	/*
	 * Ensure devices are in D0 before enabling PCI-PM L1 PM Substates, per
	 * PCIe r6.0, sec 5.5.4.
	 */
	pci_set_power_state_locked(pdev, PCI_D0);         /* PCI/NVMe: NVMe 장치를 D0 상태로 전환 */
	pci_enable_link_state_locked(pdev, PCIE_LINK_STATE_ALL); /* PCI/NVMe: NVMe 링크 ASPM/L1SS 전부 활성화 */
	return 0;                                         /* PCI/NVMe: NVMe 전원 관리 quirk 적용 완료 */
}

static int vmd_enable_domain(struct vmd_dev *vmd, unsigned long features)
{
	struct pci_sysdata *sd = &vmd->sysdata;           /* PCI/NVMe: NVMe 버스 sysdata 포인터 */
	struct resource *res;                             /* PCI/NVMe: NVMe 도메인 리소스 임시 포인터 */
	u32 upper_bits;                                   /* PCI/NVMe: NVMe 메모리 창 상위 32bit */
	unsigned long flags;                              /* PCI/NVMe: NVMe 리소스 플래그 */
	LIST_HEAD(resources);                             /* PCI/NVMe: NVMe root bus 등록용 리소스 리스트 */
	resource_size_t offset[2] = {0};                  /* PCI/NVMe: NVMe MEMBAR1/2 물리 오프셋 */
	resource_size_t membar2_offset = 0x2000;          /* PCI/NVMe: NVMe MEMBAR2 기본 오프셋(shadow 공간) */
	struct pci_bus *child;                            /* PCI/NVMe: NVMe 루트 포트 하위 버스 */
	struct pci_dev *dev;                              /* PCI/NVMe: NVMe 장치 포인터(리셋/PM용) */
	int ret;                                          /* PCI/NVMe: NVMe 도메인 활성화 결과 */

	/*
	 * Shadow registers may exist in certain VMD device ids which allow
	 * guests to correctly assign host physical addresses to the root ports
	 * and child devices. These registers will either return the host value
	 * or 0, depending on an enable bit in the VMD device.
	 */
	if (features & VMD_FEAT_HAS_MEMBAR_SHADOW) {      /* PCI/NVMe: NVMe BAR 변환을 위한 native shadow 지원 */
		membar2_offset = MB2_SHADOW_OFFSET + MB2_SHADOW_SIZE; /* PCI/NVMe: NVMe MEMBAR2 사용 가능 시작 오프셋 */
		ret = vmd_get_phys_offsets(vmd, true, &offset[0], &offset[1]); /* PCI/NVMe: NVMe MEMBAR1/2 물리 오프셋 획득 */
		if (ret)                                  /* PCI/NVMe: NVMe 물리 오프셋 획득 실패 */
			return ret;                       /* PCI/NVMe: NVMe 도메인 활성화 중단 */
	} else if (features & VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP) { /* PCI/NVMe: NVMe BAR 변환을 위한 VSCAP shadow 지원 */
		ret = vmd_get_phys_offsets(vmd, false, &offset[0], &offset[1]); /* PCI/NVMe: NVMe MEMBAR1/2 물리 오프셋 획득 */
		if (ret)                                  /* PCI/NVMe: NVMe 물리 오프셋 획득 실패 */
			return ret;                       /* PCI/NVMe: NVMe 도메인 활성화 중단 */
	}

	/*
	 * Certain VMD devices may have a root port configuration option which
	 * limits the bus range to between 0-127, 128-255, or 224-255
	 */
	if (features & VMD_FEAT_HAS_BUS_RESTRICTIONS) {   /* PCI/NVMe: NVMe 버스 범위 제한 VMD인 경우 */
		ret = vmd_get_bus_number_start(vmd);      /* PCI/NVMe: NVMe 도메인 버스 시작점 설정 */
		if (ret)                                  /* PCI/NVMe: NVMe 버스 시작점 설정 실패 */
			return ret;                       /* PCI/NVMe: NVMe 도메인 활성화 중단 */
	}

	res = &vmd->dev->resource[VMD_CFGBAR];            /* PCI/NVMe: VMD config BAR, NVMe ECAM 범위 산출 */
	vmd->resources[0] = (struct resource) {
		.name  = "VMD CFGBAR",                    /* PCI/NVMe: NVMe config 공간 리소스 이름 */
		.start = vmd->busn_start,                 /* PCI/NVMe: NVMe 도메인 버스 시작 번호 */
		.end   = vmd->busn_start + (resource_size(res) >> 20) - 1, /* PCI/NVMe: NVMe 도메인 버스 끝 번호(1MB/버스) */
		.flags = IORESOURCE_BUS | IORESOURCE_PCI_FIXED, /* PCI/NVMe: NVMe 버스 리소스, 고정 표시 */
	};

	/*
	 * If the window is below 4GB, clear IORESOURCE_MEM_64 so we can
	 * put 32-bit resources in the window.
	 *
	 * There's no hardware reason why a 64-bit window *couldn't*
	 * contain a 32-bit resource, but pbus_size_mem() computes the
	 * bridge window size assuming a 64-bit window will contain no
	 * 32-bit resources.  __pci_assign_resource() enforces that
	 * artificial restriction to make sure everything will fit.
	 *
	 * The only way we could use a 64-bit non-prefetchable MEMBAR is
	 * if its address is <4GB so that we can convert it to a 32-bit
	 * resource.  To be visible to the host OS, all VMD endpoints must
	 * be initially configured by platform BIOS, which includes setting
	 * up these resources.  We can assume the device is configured
	 * according to the platform needs.
	 */
	res = &vmd->dev->resource[VMD_MEMBAR1];           /* PCI/NVMe: NVMe 메모리 창 1 리소스 */
	upper_bits = upper_32_bits(res->end);             /* PCI/NVMe: NVMe MEMBAR1 상위 32bit 주소 */
	flags = res->flags & ~IORESOURCE_SIZEALIGN;       /* PCI/NVMe: NVMe MEMBAR1 정렬 플래그 제거 */
	if (!upper_bits)                                  /* PCI/NVMe: NVMe MEMBAR1이 4GB 미만이면 */
		flags &= ~IORESOURCE_MEM_64;          /* PCI/NVMe: NVMe 32bit 리소스 배치를 위해 64bit 플래그 클리어 */
	vmd->resources[1] = (struct resource) {
		.name  = "VMD MEMBAR1",                   /* PCI/NVMe: NVMe MEMBAR1 리소스 이름 */
		.start = res->start,                      /* PCI/NVMe: NVMe MEMBAR1 시작 주소 */
		.end   = res->end,                        /* PCI/NVMe: NVMe MEMBAR1 끝 주소 */
		.flags = flags,                           /* PCI/NVMe: NVMe MEMBAR1 조정된 플래그 */
		.parent = res,                            /* PCI/NVMe: NVMe MEMBAR1 상위 VMD BAR 연결 */
	};

	res = &vmd->dev->resource[VMD_MEMBAR2];           /* PCI/NVMe: NVMe 메모리 창 2 리소스 */
	upper_bits = upper_32_bits(res->end);             /* PCI/NVMe: NVMe MEMBAR2 상위 32bit 주소 */
	flags = res->flags & ~IORESOURCE_SIZEALIGN;       /* PCI/NVMe: NVMe MEMBAR2 정렬 플래그 제거 */
	if (!upper_bits)                                  /* PCI/NVMe: NVMe MEMBAR2가 4GB 미만이면 */
		flags &= ~IORESOURCE_MEM_64;          /* PCI/NVMe: NVMe 32bit 리소스 배치를 위해 64bit 플래그 클리어 */
	vmd->resources[2] = (struct resource) {
		.name  = "VMD MEMBAR2",                   /* PCI/NVMe: NVMe MEMBAR2 리소스 이름 */
		.start = res->start + membar2_offset,     /* PCI/NVMe: NVMe MEMBAR2 사용 가능 시작(shadow 공간 이후) */
		.end   = res->end,                        /* PCI/NVMe: NVMe MEMBAR2 끝 주소 */
		.flags = flags,                           /* PCI/NVMe: NVMe MEMBAR2 조정된 플래그 */
		.parent = res,                            /* PCI/NVMe: NVMe MEMBAR2 상위 VMD BAR 연결 */
	};

	/*
	 * Currently MSI remapping must be enabled in guest passthrough mode
	 * due to some missing interrupt remapping plumbing. This is probably
	 * acceptable because the guest is usually CPU-limited and MSI
	 * remapping doesn't become a performance bottleneck.
	 */
	if (!(features & VMD_FEAT_CAN_BYPASS_MSI_REMAP) || /* PCI/NVMe: NVMe MSI remap bypass 불가능하거나 */
	    offset[0] || offset[1]) {                     /* PCI/NVMe: NVMe 메모리 오프셋이 필요한 게스트 passthrough면 */
		ret = vmd_alloc_irqs(vmd);                /* PCI/NVMe: NVMe MSI demux용 VMD MSI-X 벡터 할당 */
		if (ret)                                  /* PCI/NVMe: NVMe MSI demux 벡터 할당 실패 */
			return ret;                       /* PCI/NVMe: NVMe 도메인 활성화 중단 */

		vmd_set_msi_remapping(vmd, true);         /* PCI/NVMe: NVMe MSI를 VMD MSI-X 테이블로 remap 활성화 */

		ret = vmd_create_irq_domain(vmd);         /* PCI/NVMe: NVMe 장치용 VMD MSI irq domain 생성 */
		if (ret)                                  /* PCI/NVMe: NVMe MSI domain 생성 실패 */
			return ret;                       /* PCI/NVMe: NVMe 도메인 활성화 중단 */
	} else {                                          /* PCI/NVMe: NVMe MSI remap bypass 가능한 물리 환경 */
		vmd_set_msi_remapping(vmd, false);        /* PCI/NVMe: NVMe MSI가 VMD 테이블 거치지 않도록 remap 비활성 */
	}

	pci_add_resource(&resources, &vmd->resources[0]); /* PCI/NVMe: NVMe 도메인 버스 리소스 등록 */
	pci_add_resource_offset(&resources, &vmd->resources[1], offset[0]); /* PCI/NVMe: NVMe MEMBAR1 + 오프셋 등록 */
	pci_add_resource_offset(&resources, &vmd->resources[2], offset[1]); /* PCI/NVMe: NVMe MEMBAR2 + 오프셋 등록 */

	sd->vmd_dev = vmd->dev;                           /* PCI/NVMe: NVMe sysdata에 VMD 장치 기록 */

	/*
	 * Emulated domains start at 0x10000 to not clash with ACPI _SEG
	 * domains.  Per ACPI r6.0, sec 6.5.6, _SEG returns an integer, of
	 * which the lower 16 bits are the PCI Segment Group (domain) number.
	 * Other bits are currently reserved.
	 */
	sd->domain = pci_bus_find_emul_domain_nr(0, 0x10000, INT_MAX); /* PCI/NVMe: NVMe 가상 PCIe 도메인 번호 할당 */
	if (sd->domain < 0)                               /* PCI/NVMe: NVMe 도메인 번호 할당 실패 */
		return sd->domain;                    /* PCI/NVMe: NVMe 도메인 활성화 중단 */

	sd->node = pcibus_to_node(vmd->dev->bus);         /* PCI/NVMe: NVMe 버스 NUMA 노드 설정 */

	vmd->bus = pci_create_root_bus(&vmd->dev->dev, vmd->busn_start, /* PCI/NVMe: NVMe SSD가 연결될 가상 루트 버스 생성 */
				       &vmd_ops, sd, &resources);
	if (!vmd->bus) {                                  /* PCI/NVMe: NVMe 루트 버스 생성 실패 */
		pci_bus_release_emul_domain_nr(sd->domain); /* PCI/NVMe: NVMe 도메인 번호 반납 */
		pci_free_resource_list(&resources);   /* PCI/NVMe: NVMe 리소스 리스트 해제 */
		vmd_remove_irq_domain(vmd);           /* PCI/NVMe: NVMe MSI domain 정리 */
		return -ENODEV;                       /* PCI/NVMe: NVMe 루트 버스 생성 실패 반환 */
	}

	vmd_copy_host_bridge_flags(pci_find_host_bridge(vmd->dev->bus), /* PCI/NVMe: NVMe 루트 브리지 플래그 상위로부터 상속 */
				   to_pci_host_bridge(vmd->bus->bridge));

	vmd_attach_resources(vmd);                        /* PCI/NVMe: NVMe 메모리 리소스 VMD BAR에 연결 */
	if (vmd->irq_domain)                              /* PCI/NVMe: NVMe용 VMD MSI domain이 있으면 */
		dev_set_msi_domain(&vmd->bus->dev, vmd->irq_domain); /* PCI/NVMe: NVMe 버스의 MSI domain을 VMD domain으로 */
	else                                              /* PCI/NVMe: NVMe MSI bypass 모드면 */
		dev_set_msi_domain(&vmd->bus->dev,        /* PCI/NVMe: NVMe 버스의 MSI domain을 상위 VMD 장치 것으로 */
				   dev_get_msi_domain(&vmd->dev->dev));

	WARN(sysfs_create_link(&vmd->dev->dev.kobj, &vmd->bus->dev.kobj, /* PCI/NVMe: NVMe 도메인 sysfs 링크 생성 */
			       "domain"), "Can't create symlink to domain\n");

	vmd_acpi_begin();                                 /* PCI/NVMe: NVMe 장치 ACPI companion 탐색 hook 활성화 */

	pci_scan_child_bus(vmd->bus);                     /* PCI/NVMe: VMD 아래 NVMe SSD 및 루트 포트 열거 */
	vmd_domain_reset(vmd);                            /* PCI/NVMe: NVMe 루트 포트 브리지 리소스 리셋 */

	/* When Intel VMD is enabled, the OS does not discover the Root Ports
	 * owned by Intel VMD within the MMCFG space. pci_reset_bus() applies
	 * a reset to the parent of the PCI device supplied as argument. This
	 * is why we pass a child device, so the reset can be triggered at
	 * the Intel bridge level and propagated to all the children in the
	 * hierarchy.
	 */
	list_for_each_entry(child, &vmd->bus->children, node) { /* PCI/NVMe: NVMe 루트 포트 하위 버스 순회 */
		if (!list_empty(&child->devices)) {       /* PCI/NVMe: NVMe 장치가 있는 첫 하위 버스 찾기 */
			dev = list_first_entry(&child->devices, /* PCI/NVMe: 해당 버스의 첫 NVMe 장치 */
					       struct pci_dev, bus_list);
			ret = pci_reset_bus(dev);         /* PCI/NVMe: NVMe 계층 리셋(VMD 브리지 수준으로 전파) */
			if (ret)                          /* PCI/NVMe: NVMe 버스 리셋 실패 */
				pci_warn(dev, "can't reset device: %d\n", ret); /* PCI/NVMe: NVMe 리셋 실패 경고 */

			break;                        /* PCI/NVMe: 첫 NVMe 하위 버스만 리셋 시도 */
		}
	}

	pci_assign_unassigned_bus_resources(vmd->bus);    /* PCI/NVMe: NVMe 장치 미할당 리소스 배정 */

	pci_walk_bus(vmd->bus, vmd_pm_enable_quirk, &features); /* PCI/NVMe: NVMe 장치에 ASPM/LTR quirk 적용 */

	/*
	 * VMD root buses are virtual and don't return true on pci_is_pcie()
	 * and will fail pcie_bus_configure_settings() early. It can instead be
	 * run on each of the real root ports.
	 */
	list_for_each_entry(child, &vmd->bus->children, node) /* PCI/NVMe: NVMe 실제 루트 포트별로 */
		pcie_bus_configure_settings(child);       /* PCI/NVMe: NVMe 링크 폭/속도 및 ASPM 설정 구성 */

	pci_bus_add_devices(vmd->bus);                    /* PCI/NVMe: VMD 아래 NVMe SSD를 시스템에 등록, nvme_pci_probe 연결 */

	vmd_acpi_end();                                   /* PCI/NVMe: NVMe ACPI companion 탐색 hook 비활성화 */
	return 0;                                         /* PCI/NVMe: NVMe 도메인 활성화 성공 */
}

static int vmd_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	unsigned long features = (unsigned long) id->driver_data; /* PCI/NVMe: VMD 기능 플래그, NVMe 지원 정책 결정 */
	struct vmd_dev *vmd;                              /* PCI/NVMe: VMD 드라이버 사설 데이터, NVMe 도메인 상태 */
	int err;                                          /* PCI/NVMe: NVMe probe 과정 에러 코드 */

	if (xen_domain()) {                               /* PCI/NVMe: Xen 가상화 환경에서 NVMe 동작 제한 */
		/*
		 * Xen doesn't have knowledge about devices in the VMD bus
		 * because the config space of devices behind the VMD bridge is
		 * not known to Xen, and hence Xen cannot discover or configure
		 * them in any way.
		 *
		 * Bypass of MSI remapping won't work in that case as direct
		 * write by Linux to the MSI entries won't result in functional
		 * interrupts, as Xen is the entity that manages the host
		 * interrupt controller and must configure interrupts.  However
		 * multiplexing of interrupts by the VMD bridge will work under
		 * Xen, so force the usage of that mode which must always be
		 * supported by VMD bridges.
		 */
		features &= ~VMD_FEAT_CAN_BYPASS_MSI_REMAP; /* PCI/NVMe: Xen에서는 NVMe MSI remap bypass 금지 */
	}

	if (resource_size(&dev->resource[VMD_CFGBAR]) < (1 << 20)) /* PCI/NVMe: NVMe config 공간 최소 1MB 필요 */
		return -ENOMEM;                           /* PCI/NVMe: NVMe ECAM 공간 부족으로 probe 실패 */

	vmd = devm_kzalloc(&dev->dev, sizeof(*vmd), GFP_KERNEL); /* PCI/NVMe: NVMe VMD 사설 데이터 할당 */
	if (!vmd)                                         /* PCI/NVMe: NVMe VMD 구조체 메모리 부족 */
		return -ENOMEM;                           /* PCI/NVMe: NVMe probe 메모리 할당 실패 */

	vmd->dev = dev;                                   /* PCI/NVMe: VMD PCI 장치와 NVMe 도메인 연결 */
	vmd->sysdata.domain = PCI_DOMAIN_NR_NOT_SET;      /* PCI/NVMe: NVMe 도메인 번호 아직 미할당 표시 */
	vmd->instance = ida_alloc(&vmd_instance_ida, GFP_KERNEL); /* PCI/NVMe: NVMe VMD 인스턴스 ID 할당 */
	if (vmd->instance < 0)                            /* PCI/NVMe: NVMe VMD 인스턴스 할당 실패 */
		return vmd->instance;                     /* PCI/NVMe: NVMe probe 인스턴스 할당 실패 */

	vmd->name = devm_kasprintf(&dev->dev, GFP_KERNEL, "vmd%d", /* PCI/NVMe: NVMe VMD 인스턴스 이름 생성 */
				   vmd->instance);
	if (!vmd->name) {                                 /* PCI/NVMe: NVMe VMD 이름 생성 실패 */
		err = -ENOMEM;                            /* PCI/NVMe: NVMe VMD 이름 메모리 부족 */
		goto out_release_instance;                /* PCI/NVMe: NVMe probe 실패 정리 */
	}

	err = pcim_enable_device(dev);                    /* PCI/NVMe: NVMe 상위 VMD PCI 장치 활성화 */
	if (err < 0)                                      /* PCI/NVMe: NVMe VMD PCI 장치 활성화 실패 */
		goto out_release_instance;                /* PCI/NVMe: NVMe probe 실패 정리 */

	vmd->cfgbar = pcim_iomap(dev, VMD_CFGBAR, 0);     /* PCI/NVMe: NVMe ECAM config 공간 매핑 */
	if (!vmd->cfgbar) {                               /* PCI/NVMe: NVMe config 공간 매핑 실패 */
		err = -ENOMEM;                            /* PCI/NVMe: NVMe VMD config BAR 매핑 메모리 부족 */
		goto out_release_instance;                /* PCI/NVMe: NVMe probe 실패 정리 */
	}

	pci_set_master(dev);                              /* PCI/NVMe: NVMe VMD가 DMA bus master 동작 허용 */
	if (dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(64)) && /* PCI/NVMe: NVMe DMA 64bit 마스크 시도 */
	    dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(32))) { /* PCI/NVMe: NVMe DMA 32bit 마스크 폴백 */
		err = -ENODEV;                            /* PCI/NVMe: NVMe DMA 마스크 설정 실패 */
		goto out_release_instance;                /* PCI/NVMe: NVMe probe 실패 정리 */
	}

	if (features & VMD_FEAT_OFFSET_FIRST_VECTOR)      /* PCI/NVMe: VMD가 vector 0을 자체용으로 사용하면 */
		vmd->first_vec = 1;                       /* PCI/NVMe: NVMe MSI demux는 1번 벡터부터 시작 */

	raw_spin_lock_init(&vmd->cfg_lock);               /* PCI/NVMe: NVMe config 접근 스핀락 초기화 */
	pci_set_drvdata(dev, vmd);                        /* PCI/NVMe: NVMe VMD 드라이버 데이터 등록 */
	err = vmd_enable_domain(vmd, features);           /* PCI/NVMe: NVMe PCIe 도메인 생성 및 장치 열거 */
	if (err)                                          /* PCI/NVMe: NVMe 도메인 활성화 실패 */
		goto out_release_instance;                /* PCI/NVMe: NVMe probe 실패 정리 */

	dev_info(&vmd->dev->dev, "Bound to PCI domain %04x\n", /* PCI/NVMe: NVMe PCIe 도메인 바인딩 로그 */
		 vmd->sysdata.domain);
	return 0;                                         /* PCI/NVMe: NVMe VMD 드라이버 probe 성공 */

 out_release_instance:
	ida_free(&vmd_instance_ida, vmd->instance);       /* PCI/NVMe: NVMe VMD 인스턴스 ID 반납 */
	return err;                                       /* PCI/NVMe: NVMe VMD probe 에러 반환 */
}

static void vmd_cleanup_srcu(struct vmd_dev *vmd)
{
	int i;                                            /* PCI/NVMe: NVMe demux SRCU 정리 루프 인덱스 */

	for (i = 0; i < vmd->msix_count; i++)             /* PCI/NVMe: 모든 NVMe demup VMD 벡터 순회 */
		cleanup_srcu_struct(&vmd->irqs[i].srcu);  /* PCI/NVMe: NVMe IRQ demux SRCU 정리 */
}

static void vmd_remove(struct pci_dev *dev)
{
	struct vmd_dev *vmd = pci_get_drvdata(dev);       /* PCI/NVMe: NVMe VMD 드라이버 사설 데이터 획득 */

	pci_stop_root_bus(vmd->bus);                      /* PCI/NVMe: NVMe 루트 버스 중지, nvme_pci_remove 유도 */
	sysfs_remove_link(&vmd->dev->dev.kobj, "domain"); /* PCI/NVMe: NVMe 도메인 sysfs 링크 제거 */
	pci_remove_root_bus(vmd->bus);                    /* PCI/NVMe: NVMe 루트 버스 및 하위 NVMe 장치 제거 */
	vmd_cleanup_srcu(vmd);                            /* PCI/NVMe: NVMe IRQ demux SRCU 정리 */
	vmd_detach_resources(vmd);                        /* PCI/NVMe: NVMe 메모리 리소스 VMD BAR에서 분리 */
	vmd_remove_irq_domain(vmd);                       /* PCI/NVMe: NVMe MSI domain 제거 및 remap 복원 */
	ida_free(&vmd_instance_ida, vmd->instance);       /* PCI/NVMe: NVMe VMD 인스턴스 ID 반납 */
	pci_bus_release_emul_domain_nr(vmd->sysdata.domain); /* PCI/NVMe: NVMe 가상 PCIe 도메인 번호 반납 */
}

static void vmd_shutdown(struct pci_dev *dev)
{
	struct vmd_dev *vmd = pci_get_drvdata(dev);       /* PCI/NVMe: NVMe VMD 드라이버 사설 데이터 획득 */

	vmd_remove_irq_domain(vmd);                       /* PCI/NVMe: NVMe MSI domain 제거, 시스템 종료 시 정리 */
}

#ifdef CONFIG_PM_SLEEP
static int vmd_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);           /* PCI/NVMe: NVMe VMD PCI 장치 변환 */
	struct vmd_dev *vmd = pci_get_drvdata(pdev);      /* PCI/NVMe: NVMe VMD 드라이버 데이터 */
	int i;                                            /* PCI/NVMe: NVMe IRQ 해제 루프 인덱스 */

	for (i = 0; i < vmd->msix_count; i++)             /* PCI/NVMe: 모든 NVMe demux VMD 벡터 순회 */
		devm_free_irq(dev, vmd->irqs[i].virq, &vmd->irqs[i]); /* PCI/NVMe: NVMe MSI demux 핸들러 해제 */

	return 0;                                         /* PCI/NVMe: NVMe VMD suspend 완료 */
}

static int vmd_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);           /* PCI/NVMe: NVMe VMD PCI 장치 변환 */
	struct vmd_dev *vmd = pci_get_drvdata(pdev);      /* PCI/NVMe: NVMe VMD 드라이버 데이터 */
	int err, i;                                       /* PCI/NVMe: NVMe resume 결과/루프 변수 */

	vmd_set_msi_remapping(vmd, !!vmd->irq_domain);    /* PCI/NVMe: NVMe MSI domain 존재 시 remap 복원 */

	for (i = 0; i < vmd->msix_count; i++) {           /* PCI/NVMe: 모든 NVMe demux VMD 벡터 재등록 */
		err = devm_request_irq(dev, vmd->irqs[i].virq, /* PCI/NVMe: NVMe MSI demux 핸들러 재등록 */
				       vmd_irq, IRQF_NO_THREAD,
				       vmd->name, &vmd->irqs[i]);
		if (err)                                  /* PCI/NVMe: NVMe IRQ 재등록 실패 */
			return err;                       /* PCI/NVMe: NVMe VMD resume 실패 */
	}

	return 0;                                         /* PCI/NVMe: NVMe VMD resume 완료 */
}
#endif
static SIMPLE_DEV_PM_OPS(vmd_dev_pm_ops, vmd_suspend, vmd_resume); /* PCI/NVMe: NVMe VMD 전원 관리 ops 등록 */

static const struct pci_device_id vmd_ids[] = {
	{PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_VMD_201D), /* PCI/NVMe: Intel VMD 201D, NVMe VSCAP shadow 지원 */
		.driver_data = VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP,},
	{PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_VMD_28C0), /* PCI/NVMe: Intel VMD 28C0, NVMe native shadow/버스제한/MSI bypass */
		.driver_data = VMD_FEAT_HAS_MEMBAR_SHADOW |
				VMD_FEAT_HAS_BUS_RESTRICTIONS |
				VMD_FEAT_CAN_BYPASS_MSI_REMAP,},
	{PCI_VDEVICE(INTEL, 0x467f),                       /* PCI/NVMe: Intel VMD 467f, NVMe 클라이언트 플랫폼 기능 */
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0x4c3d),                       /* PCI/NVMe: Intel VMD 4c3d, NVMe 클라이언트 플랫폼 기능 */
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xa77f),                       /* PCI/NVMe: Intel VMD a77f, NVMe 클라이언트 플랫폼 기능 */
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0x7d0b),                       /* PCI/NVMe: Intel VMD 7d0b, NVMe 클라이언트 플랫폼 기능 */
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xad0b),                       /* PCI/NVMe: Intel VMD ad0b, NVMe 클라이언트 플랫폼 기능 */
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_VMD_9A0B), /* PCI/NVMe: Intel VMD 9A0B, NVMe 클라이언트 플랫폼 기능 */
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xb60b),                       /* PCI/NVMe: Intel VMD b60b, NVMe 클라이언트 플랫폼 기능 */
                .driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xb06f),                       /* PCI/NVMe: Intel VMD b06f, NVMe 클라이언트 플랫폼 기능 */
                .driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xb07f),                       /* PCI/NVMe: Intel VMD b07f, NVMe 클라이언트 플랫폼 기능 */
                .driver_data = VMD_FEATS_CLIENT,},
	{0,}
};
MODULE_DEVICE_TABLE(pci, vmd_ids);                    /* PCI/NVMe: NVMe VMD 모듈용 PCI device ID 테이블 */

static struct pci_driver vmd_drv = {
	.name		= "vmd",                          /* PCI/NVMe: NVMe VMD PCI 드라이버 이름 */
	.id_table	= vmd_ids,                        /* PCI/NVMe: NVMe VMD 지원 장치 ID 테이블 */
	.probe		= vmd_probe,                      /* PCI/NVMe: NVMe VMD 장치 probe 콜백, NVMe 도메인 생성 */
	.remove		= vmd_remove,                     /* PCI/NVMe: NVMe VMD 장치 remove 콜백, NVMe 도메인 제거 */
	.shutdown	= vmd_shutdown,                   /* PCI/NVMe: NVMe VMD 시스템 종료 콜백 */
	.driver		= {
		.pm	= &vmd_dev_pm_ops,            /* PCI/NVMe: NVMe VMD 전원 관리 ops 연결 */
	},
};
module_pci_driver(vmd_drv);                           /* PCI/NVMe: NVMe VMD PCI 드라이버 등록 */

MODULE_AUTHOR("Intel Corporation");                   /* PCI/NVMe: NVMe VMD 드라이버 저작자 */
MODULE_DESCRIPTION("Volume Management Device driver"); /* PCI/NVMe: NVMe SSD를 그룹화하는 VMD 드라이버 설명 */
MODULE_LICENSE("GPL v2");                             /* PCI/NVMe: NVMe VMD 드라이버 라이선스 */
MODULE_VERSION("0.6");                                /* PCI/NVMe: NVMe VMD 드라이버 버전 */
