// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2002-2004 Greg Kroah-Hartman <greg@kroah.com>
 * (C) Copyright 2002-2004 IBM Corp.
 * (C) Copyright 2003 Matthew Wilcox
 * (C) Copyright 2003 Hewlett-Packard
 * (C) Copyright 2004 Jon Smirl <jonsmirl@yahoo.com>
 * (C) Copyright 2004 Silicon Graphics, Inc. Jesse Barnes <jbarnes@sgi.com>
 *
 * File attributes for PCI devices
 *
 * Modeled after usb's driverfs.c
 */

#include <linux/bitfield.h> /* NVMe: 헤더 포함 */
#include <linux/cleanup.h> /* NVMe: 헤더 포함 */
#include <linux/kernel.h> /* NVMe: 헤더 포함 */
#include <linux/sched.h> /* NVMe: 헤더 포함 */
#include <linux/pci.h> /* NVMe: 헤더 포함 */
#include <linux/stat.h> /* NVMe: 헤더 포함 */
#include <linux/export.h> /* NVMe: 헤더 포함 */
#include <linux/topology.h> /* NVMe: 헤더 포함 */
#include <linux/mm.h> /* NVMe: 헤더 포함 */
#include <linux/fs.h> /* NVMe: 헤더 포함 */
#include <linux/capability.h> /* NVMe: 헤더 포함 */
#include <linux/security.h> /* NVMe: 헤더 포함 */
#include <linux/slab.h> /* NVMe: 헤더 포함 */
#include <linux/vgaarb.h> /* NVMe: 헤더 포함 */
#include <linux/pm_runtime.h> /* NVMe: 헤더 포함 */
#include <linux/msi.h> /* NVMe: 헤더 포함 */
#include <linux/of.h> /* NVMe: 헤더 포함 */
#include <linux/aperture.h> /* NVMe: 헤더 포함 */
#include <linux/unaligned.h> /* NVMe: 헤더 포함 */
#include "pci.h" /* NVMe: 헤더 포함 */

#ifndef ARCH_PCI_DEV_GROUPS /* NVMe: 전처리 조건 블록 시작 */
#define ARCH_PCI_DEV_GROUPS /* NVMe: 매크로 정의 */
#endif /* NVMe: 전처리 조건 블록 종료 */

static int sysfs_initialized;	/* = 0 */

/* show configuration fields */
#define pci_config_attr(field, format_string) /* NVMe: 매크로 정의 */ \
static ssize_t /* NVMe: 정의/선언 */ \
field##_show(struct device *dev, struct device_attribute *attr, char *buf) /* NVMe: 매크로 내 sysfs 속성 값 출력 함수 선언 */ \
{ /* NVMe: 매크로 본문 시작 */ \
	struct pci_dev *pdev; /* NVMe: PCIe 장치 구조체 포인터 */ \
 /* NVMe: 매크로 연속 줄 */ \
	pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */ \
	return sysfs_emit(buf, format_string, pdev->field); /* NVMe: sysfs 속성 값 출력 */ \
} /* NVMe: 매크로 본문 종료 */ \
static DEVICE_ATTR_RO(field) /* NVMe: sysfs 장치 속성 등록 */

pci_config_attr(vendor, "0x%04x\n"); /* NVMe: PCI 설정공간 속성 생성 */
pci_config_attr(device, "0x%04x\n"); /* NVMe: PCI 설정공간 속성 생성 */
pci_config_attr(subsystem_vendor, "0x%04x\n"); /* NVMe: PCI 설정공간 속성 생성 */
pci_config_attr(subsystem_device, "0x%04x\n"); /* NVMe: PCI 설정공간 속성 생성 */
pci_config_attr(revision, "0x%02x\n"); /* NVMe: PCI 설정공간 속성 생성 */
pci_config_attr(class, "0x%06x\n"); /* NVMe: PCI 설정공간 속성 생성 */

static ssize_t irq_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
			struct device_attribute *attr, /* NVMe: 함수 매개변수 선언 */
			char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */

#ifdef CONFIG_PCI_MSI /* NVMe: 해당 설정 시에만 컴파일 */
	/*
	 * For MSI, show the first MSI IRQ; for all other cases including
	 * MSI-X, show the legacy INTx IRQ.
	 */
	if (pdev->msi_enabled) /* NVMe: MSI/MSI-X 인터럽트 모드 확인 */
		return sysfs_emit(buf, "%u\n", pci_irq_vector(pdev, 0)); /* NVMe: 첫 번째 MSI 벡터 획득 */
#endif /* NVMe: 전처리 조건 블록 종료 */

	return sysfs_emit(buf, "%u\n", pdev->irq); /* NVMe: sysfs 속성 값 출력 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(irq); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t broken_parity_status_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
					 struct device_attribute *attr, /* NVMe: 함수 매개변수 선언 */
					 char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	return sysfs_emit(buf, "%u\n", pdev->broken_parity_status); /* NVMe: sysfs 속성 값 출력 */
} /* NVMe: 함수 본문 종료 */

static ssize_t broken_parity_status_store(struct device *dev, /* NVMe: sysfs 속성 값 저장 함수 선언 */
					  struct device_attribute *attr, /* NVMe: 함수 매개변수 선언 */
					  const char *buf, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	unsigned long val; /* NVMe: 사용자 입력/플래그 값 */

	if (kstrtoul(buf, 0, &val) < 0) /* NVMe: 사용자 입력 문자열을 정수로 변환 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	pdev->broken_parity_status = !!val; /* NVMe: 코드 수행 */

	return count; /* NVMe: 처리된 바이트 수 반환 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RW(broken_parity_status); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t pci_dev_show_local_cpu(struct device *dev, bool list, /* NVMe: sysfs 속성 값 출력 함수 선언 */
				      struct device_attribute *attr, char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	const struct cpumask *mask; /* NVMe: CPU affinity 마스크 */

#ifdef CONFIG_NUMA /* NVMe: 해당 설정 시에만 컴파일 */
	if (dev_to_node(dev) == NUMA_NO_NODE) /* NVMe: 디바이스의 NUMA 노드 */
		mask = cpu_online_mask; /* NVMe: 코드 수행 */
	else /* NVMe: 대안 분기 */
		mask = cpumask_of_node(dev_to_node(dev)); /* NVMe: 디바이스의 NUMA 노드 */
#else /* NVMe: 이전 전처리 조건 미만족 시 대안 코드 */
	mask = cpumask_of_pcibus(to_pci_dev(dev)->bus); /* NVMe: PCI 버스의 CPU affinity 마스크 */
#endif /* NVMe: 전처리 조건 블록 종료 */
	return cpumap_print_to_pagebuf(list, buf, mask); /* NVMe: CPU affinity를 sysfs에 출력 */
} /* NVMe: 함수 본문 종료 */

static ssize_t local_cpus_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
			       struct device_attribute *attr, char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	return pci_dev_show_local_cpu(dev, false, attr, buf); /* NVMe: 함수 결과 반환 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(local_cpus); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t local_cpulist_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
				  struct device_attribute *attr, char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	return pci_dev_show_local_cpu(dev, true, attr, buf); /* NVMe: 함수 결과 반환 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(local_cpulist); /* NVMe: sysfs 장치 속성 등록 */

/*
 * PCI Bus Class Devices
 */
static ssize_t cpuaffinity_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
				struct device_attribute *attr, char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	const struct cpumask *cpumask = cpumask_of_pcibus(to_pci_bus(dev)); /* NVMe: PCI 버스의 CPU affinity 마스크 */

	return cpumap_print_to_pagebuf(false, buf, cpumask); /* NVMe: CPU affinity를 sysfs에 출력 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(cpuaffinity); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t cpulistaffinity_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
				    struct device_attribute *attr, char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	const struct cpumask *cpumask = cpumask_of_pcibus(to_pci_bus(dev)); /* NVMe: PCI 버스의 CPU affinity 마스크 */

	return cpumap_print_to_pagebuf(true, buf, cpumask); /* NVMe: CPU affinity를 sysfs에 출력 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(cpulistaffinity); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t power_state_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
				struct device_attribute *attr, char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	return sysfs_emit(buf, "%s\n", pci_power_name(pdev->current_state)); /* NVMe: 전원 상태 이름 변환 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(power_state); /* NVMe: sysfs 장치 속성 등록 */

/* show resources */
static ssize_t resource_show(struct device *dev, struct device_attribute *attr, /* NVMe: sysfs 속성 값 출력 함수 선언 */
			     char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	int i; /* NVMe: 루프 인덱스 */
	int max; /* NVMe: 정의/선언 */
	resource_size_t start, end; /* NVMe: 코드 수행 */
	size_t len = 0; /* NVMe: 버퍼/바이트 크기 */

	if (pci_dev->subordinate) /* NVMe: 브리지 하위 버스 존재 확인 */
		max = DEVICE_COUNT_RESOURCE; /* NVMe: 코드 수행 */
	else /* NVMe: 대안 분기 */
		max = PCI_BRIDGE_RESOURCES; /* NVMe: 코드 수행 */

	for (i = 0; i < max; i++) { /* NVMe: 조건/반복 블록 시작 */
		struct resource *res =  &pci_dev->resource[i]; /* NVMe: BAR/리소스 구조체 포인터 */
		struct resource zerores = {}; /* NVMe: 빈 리소스 초기화 */

		/* For backwards compatibility */
		if (pci_resource_is_bridge_win(i) && /* NVMe: 브리지 윈도우 리소스 여부 확인 */
		    res->flags & (IORESOURCE_UNSET | IORESOURCE_DISABLED)) /* NVMe: 코드 수행 */
			res = &zerores; /* NVMe: 코드 수행 */

		pci_resource_to_user(pci_dev, i, res, &start, &end); /* NVMe: 리소스 주소 사용자 공간 변환 */
		len += sysfs_emit_at(buf, len, "0x%016llx 0x%016llx 0x%016llx\n", /* NVMe: sysfs 오프셋에 값 추가 */
				     (unsigned long long)start, /* NVMe: 코드 수행 */
				     (unsigned long long)end, /* NVMe: 코드 수행 */
				     (unsigned long long)res->flags); /* NVMe: 코드 수행 */
	} /* NVMe: 조건/반복 블록 종료 */
	return len; /* NVMe: 누적 출력 길이 반환 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(resource); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t max_link_speed_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
				   struct device_attribute *attr, char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	return sysfs_emit(buf, "%s\n", /* NVMe: sysfs 속성 값 출력 */
			  pci_speed_string(pcie_get_speed_cap(pdev))); /* NVMe: 링크 속도 문자열 변환 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(max_link_speed); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t max_link_width_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
				   struct device_attribute *attr, char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	ssize_t ret; /* NVMe: 작업 결과/오류 저장 */

	/* We read PCI_EXP_LNKCAP, so we need the device to be accessible. */
	pci_config_pm_runtime_get(pdev); /* NVMe: 설정공간 접근 전 런타임 PM 활성화 */
	ret = sysfs_emit(buf, "%u\n", pcie_get_width_cap(pdev)); /* NVMe: 링크 최대 너비 능력 읽기 */
	pci_config_pm_runtime_put(pdev); /* NVMe: 설정공간 접근 후 런타임 PM 해제 */

	return ret; /* NVMe: 이전 작업 결과 반환 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(max_link_width); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t current_link_speed_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
				       struct device_attribute *attr, char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	u16 linkstat; /* NVMe: PCIe 링크 상태 레지스터 값 */
	int err; /* NVMe: 작업 결과/오류 저장 */
	enum pci_bus_speed speed; /* NVMe: 링크 속도 열거형 */

	pci_config_pm_runtime_get(pci_dev); /* NVMe: 설정공간 접근 전 런타임 PM 활성화 */
	err = pcie_capability_read_word(pci_dev, PCI_EXP_LNKSTA, &linkstat); /* NVMe: PCIe 기능 레지스터 읽기 */
	pci_config_pm_runtime_put(pci_dev); /* NVMe: 설정공간 접근 후 런타임 PM 해제 */

	if (err) /* NVMe: 이전 PCI 작업 오류 시 처리 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	speed = pcie_link_speed[linkstat & PCI_EXP_LNKSTA_CLS]; /* NVMe: 코드 수행 */

	return sysfs_emit(buf, "%s\n", pci_speed_string(speed)); /* NVMe: 링크 속도 문자열 변환 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(current_link_speed); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t current_link_width_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
				       struct device_attribute *attr, char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	u16 linkstat; /* NVMe: PCIe 링크 상태 레지스터 값 */
	int err; /* NVMe: 작업 결과/오류 저장 */

	pci_config_pm_runtime_get(pci_dev); /* NVMe: 설정공간 접근 전 런타임 PM 활성화 */
	err = pcie_capability_read_word(pci_dev, PCI_EXP_LNKSTA, &linkstat); /* NVMe: PCIe 기능 레지스터 읽기 */
	pci_config_pm_runtime_put(pci_dev); /* NVMe: 설정공간 접근 후 런타임 PM 해제 */

	if (err) /* NVMe: 이전 PCI 작업 오류 시 처리 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	return sysfs_emit(buf, "%u\n", FIELD_GET(PCI_EXP_LNKSTA_NLW, linkstat)); /* NVMe: sysfs 속성 값 출력 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(current_link_width); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t secondary_bus_number_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
					 struct device_attribute *attr, /* NVMe: 함수 매개변수 선언 */
					 char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	u8 sec_bus; /* NVMe: 세컨더리 버스 번호 */
	int err; /* NVMe: 작업 결과/오류 저장 */

	pci_config_pm_runtime_get(pci_dev); /* NVMe: 설정공간 접근 전 런타임 PM 활성화 */
	err = pci_read_config_byte(pci_dev, PCI_SECONDARY_BUS, &sec_bus); /* NVMe: PCI 설정공간 바이트 읽기 */
	pci_config_pm_runtime_put(pci_dev); /* NVMe: 설정공간 접근 후 런타임 PM 해제 */

	if (err) /* NVMe: 이전 PCI 작업 오류 시 처리 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	return sysfs_emit(buf, "%u\n", sec_bus); /* NVMe: sysfs 속성 값 출력 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(secondary_bus_number); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t subordinate_bus_number_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
					   struct device_attribute *attr, /* NVMe: 함수 매개변수 선언 */
					   char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	u8 sub_bus; /* NVMe: 서브ordinate 버스 번호 */
	int err; /* NVMe: 작업 결과/오류 저장 */

	pci_config_pm_runtime_get(pci_dev); /* NVMe: 설정공간 접근 전 런타임 PM 활성화 */
	err = pci_read_config_byte(pci_dev, PCI_SUBORDINATE_BUS, &sub_bus); /* NVMe: PCI 설정공간 바이트 읽기 */
	pci_config_pm_runtime_put(pci_dev); /* NVMe: 설정공간 접근 후 런타임 PM 해제 */

	if (err) /* NVMe: 이전 PCI 작업 오류 시 처리 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	return sysfs_emit(buf, "%u\n", sub_bus); /* NVMe: sysfs 속성 값 출력 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(subordinate_bus_number); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t ari_enabled_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
				struct device_attribute *attr, /* NVMe: 함수 매개변수 선언 */
				char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	return sysfs_emit(buf, "%u\n", pci_ari_enabled(pci_dev->bus)); /* NVMe: ARI 지원 여부 확인 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(ari_enabled); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t modalias_show(struct device *dev, struct device_attribute *attr, /* NVMe: sysfs 속성 값 출력 함수 선언 */
			     char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	return sysfs_emit(buf, "pci:v%08Xd%08Xsv%08Xsd%08Xbc%02Xsc%02Xi%02X\n", /* NVMe: sysfs 속성 값 출력 */
			  pci_dev->vendor, pci_dev->device, /* NVMe: 코드 수행 */
			  pci_dev->subsystem_vendor, pci_dev->subsystem_device, /* NVMe: 코드 수행 */
			  (u8)(pci_dev->class >> 16), (u8)(pci_dev->class >> 8), /* NVMe: 코드 수행 */
			  (u8)(pci_dev->class)); /* NVMe: 코드 수행 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(modalias); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t enable_store(struct device *dev, struct device_attribute *attr, /* NVMe: sysfs 속성 값 저장 함수 선언 */
			     const char *buf, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	unsigned long val; /* NVMe: 사용자 입력/플래그 값 */
	ssize_t result = 0; /* NVMe: 작업 결과/오류 저장 */

	/* this can crash the machine when done on the "wrong" device */
	if (!capable(CAP_SYS_ADMIN)) /* NVMe: root 권한(CAP_SYS_ADMIN) 검사 */
		return -EPERM; /* NVMe: 권한 없음 오류 반환 */

	if (kstrtoul(buf, 0, &val) < 0) /* NVMe: 사용자 입력 문자열을 정수로 변환 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	device_lock(dev); /* NVMe: 디바이스 상태 동시 접근 보호 */
	if (dev->driver) /* NVMe: 드라이버 바인딩 여부 확인 */
		result = -EBUSY; /* NVMe: 코드 수행 */
	else if (val) /* NVMe: 제어 흐름 */
		result = pci_enable_device(pdev); /* NVMe: I/O 및 메모리 공간 활성화 */
	else if (pci_is_enabled(pdev)) /* NVMe: 장치 enable 상태 확인 */
		pci_disable_device(pdev); /* NVMe: PCI 리소스 비활성화 */
	else /* NVMe: 대안 분기 */
		result = -EIO; /* NVMe: 코드 수행 */
	device_unlock(dev); /* NVMe: 디바이스 잠금 해제 */

	return result < 0 ? result : count; /* NVMe: 함수 결과 반환 */
} /* NVMe: 함수 본문 종료 */

static ssize_t enable_show(struct device *dev, struct device_attribute *attr, /* NVMe: sysfs 속성 값 출력 함수 선언 */
			    char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev; /* NVMe: PCIe 장치 구조체 포인터 */

	pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	return sysfs_emit(buf, "%u\n", atomic_read(&pdev->enable_cnt)); /* NVMe: 활성화 참조 카운트 읽기 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RW(enable); /* NVMe: sysfs 장치 속성 등록 */

#ifdef CONFIG_NUMA /* NVMe: 해당 설정 시에만 컴파일 */
static ssize_t numa_node_store(struct device *dev, /* NVMe: sysfs 속성 값 저장 함수 선언 */
			       struct device_attribute *attr, const char *buf, /* NVMe: 함수 매개변수 선언 */
			       size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	int node; /* NVMe: NUMA 노드 번호 */

	if (!capable(CAP_SYS_ADMIN)) /* NVMe: root 권한(CAP_SYS_ADMIN) 검사 */
		return -EPERM; /* NVMe: 권한 없음 오류 반환 */

	if (kstrtoint(buf, 0, &node) < 0) /* NVMe: 사용자 입력 문자열을 정수로 변환 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	if ((node < 0 && node != NUMA_NO_NODE) || node >= MAX_NUMNODES) /* NVMe: NUMA 노드 유효성 조건 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	if (node != NUMA_NO_NODE && !node_online(node)) /* NVMe: 온라인 NUMA 노드 확인 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	if (node == dev->numa_node) /* NVMe: NUMA 노드 변경 여부 확인 */
		return count; /* NVMe: 처리된 바이트 수 반환 */

	add_taint(TAINT_FIRMWARE_WORKAROUND, LOCKDEP_STILL_OK); /* NVMe: 워크around로 커널 taint 기록 */
	pci_alert(pdev, FW_BUG "Overriding NUMA node to %d.  Contact your vendor for updates.", /* NVMe: 장치 관련 커널 메시지 출력 */
		  node); /* NVMe: 코드 수행 */

	dev->numa_node = node; /* NVMe: NVMe 장치 NUMA 노드 재설정 */
	return count; /* NVMe: 처리된 바이트 수 반환 */
} /* NVMe: 함수 본문 종료 */

static ssize_t numa_node_show(struct device *dev, struct device_attribute *attr, /* NVMe: sysfs 속성 값 출력 함수 선언 */
			      char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	return sysfs_emit(buf, "%d\n", dev->numa_node); /* NVMe: sysfs 속성 값 출력 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RW(numa_node); /* NVMe: sysfs 장치 속성 등록 */
#endif /* NVMe: 전처리 조건 블록 종료 */

static ssize_t dma_mask_bits_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
				  struct device_attribute *attr, char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	return sysfs_emit(buf, "%d\n", fls64(pdev->dma_mask)); /* NVMe: sysfs 속성 값 출력 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(dma_mask_bits); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t consistent_dma_mask_bits_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
					     struct device_attribute *attr, /* NVMe: 함수 매개변수 선언 */
					     char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	return sysfs_emit(buf, "%d\n", fls64(dev->coherent_dma_mask)); /* NVMe: sysfs 속성 값 출력 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(consistent_dma_mask_bits); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t msi_bus_show(struct device *dev, struct device_attribute *attr, /* NVMe: sysfs 속성 값 출력 함수 선언 */
			    char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	struct pci_bus *subordinate = pdev->subordinate; /* NVMe: 정의/선언 */

	return sysfs_emit(buf, "%u\n", subordinate ? /* NVMe: sysfs 속성 값 출력 */
			  !(subordinate->bus_flags & PCI_BUS_FLAGS_NO_MSI) /* NVMe: 코드 수행 */
			    : !pdev->no_msi); /* NVMe: 코드 수행 */
} /* NVMe: 함수 본문 종료 */

static ssize_t msi_bus_store(struct device *dev, struct device_attribute *attr, /* NVMe: sysfs 속성 값 저장 함수 선언 */
			     const char *buf, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	struct pci_bus *subordinate = pdev->subordinate; /* NVMe: 정의/선언 */
	unsigned long val; /* NVMe: 사용자 입력/플래그 값 */

	if (!capable(CAP_SYS_ADMIN)) /* NVMe: root 권한(CAP_SYS_ADMIN) 검사 */
		return -EPERM; /* NVMe: 권한 없음 오류 반환 */

	if (kstrtoul(buf, 0, &val) < 0) /* NVMe: 사용자 입력 문자열을 정수로 변환 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	/*
	 * "no_msi" and "bus_flags" only affect what happens when a driver
	 * requests MSI or MSI-X.  They don't affect any drivers that have
	 * already requested MSI or MSI-X.
	 */
	if (!subordinate) { /* NVMe: 조건/반복 블록 시작 */
		pdev->no_msi = !val; /* NVMe: NVMe 장치의 MSI 허용 여부 갱신 */
		pci_info(pdev, "MSI/MSI-X %s for future drivers\n", /* NVMe: 장치 관련 커널 메시지 출력 */
			 val ? "allowed" : "disallowed"); /* NVMe: 코드 수행 */
		return count; /* NVMe: 처리된 바이트 수 반환 */
	} /* NVMe: 조건/반복 블록 종료 */

	if (val) /* NVMe: 사용자 입력 값 분기 */
		subordinate->bus_flags &= ~PCI_BUS_FLAGS_NO_MSI; /* NVMe: 하위 버스 MSI 플래그 갱신 */
	else /* NVMe: 대안 분기 */
		subordinate->bus_flags |= PCI_BUS_FLAGS_NO_MSI; /* NVMe: 하위 버스 MSI 플래그 갱신 */

	dev_info(&subordinate->dev, "MSI/MSI-X %s for future drivers of devices on this bus\n", /* NVMe: 장치 관련 커널 메시지 출력 */
		 val ? "allowed" : "disallowed"); /* NVMe: 코드 수행 */
	return count; /* NVMe: 처리된 바이트 수 반환 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RW(msi_bus); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t rescan_store(const struct bus_type *bus, const char *buf, size_t count) /* NVMe: sysfs 속성 값 저장 함수 선언 */
{ /* NVMe: 함수 본문 시작 */
	unsigned long val; /* NVMe: 사용자 입력/플래그 값 */
	struct pci_bus *b = NULL; /* NVMe: PCI 버스 구조체 포인터 */

	if (kstrtoul(buf, 0, &val) < 0) /* NVMe: 사용자 입력 문자열을 정수로 변환 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	if (val) { /* NVMe: 조건/반복 블록 시작 */
		pci_lock_rescan_remove(); /* NVMe: 재스캔/제거 동작 직렬화 */
		while ((b = pci_find_next_bus(b)) != NULL) /* NVMe: 다음 PCI 버스 순회 */
			pci_rescan_bus(b); /* NVMe: 버스 재스캔으로 핫플러그/변경 반영 */
		pci_unlock_rescan_remove(); /* NVMe: 재스캔/제거 동작 직렬화 */
	} /* NVMe: 조건/반복 블록 종료 */
	return count; /* NVMe: 처리된 바이트 수 반환 */
} /* NVMe: 함수 본문 종료 */
static BUS_ATTR_WO(rescan); /* NVMe: sysfs 버스 속성 등록 */

static struct attribute *pci_bus_attrs[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&bus_attr_rescan.attr, /* NVMe: sysfs 속성 등록 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static const struct attribute_group pci_bus_group = { /* NVMe: 구조체/배열 초기화 시작 */
	.attrs = pci_bus_attrs, /* NVMe: 코드 수행 */
}; /* NVMe: 구조체/배열 초기화 종료 */

const struct attribute_group *pci_bus_groups[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&pci_bus_group, /* NVMe: attribute group 등록 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static ssize_t dev_rescan_store(struct device *dev, /* NVMe: sysfs 속성 값 저장 함수 선언 */
				struct device_attribute *attr, const char *buf, /* NVMe: 함수 매개변수 선언 */
				size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	unsigned long val; /* NVMe: 사용자 입력/플래그 값 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	if (kstrtoul(buf, 0, &val) < 0) /* NVMe: 사용자 입력 문자열을 정수로 변환 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	if (val) { /* NVMe: 조건/반복 블록 시작 */
		pci_lock_rescan_remove(); /* NVMe: 재스캔/제거 동작 직렬화 */
		pci_rescan_bus(pdev->bus); /* NVMe: 버스 재스캔으로 핫플러그/변경 반영 */
		pci_unlock_rescan_remove(); /* NVMe: 재스캔/제거 동작 직렬화 */
	} /* NVMe: 조건/반복 블록 종료 */
	return count; /* NVMe: 처리된 바이트 수 반환 */
} /* NVMe: 함수 본문 종료 */
static struct device_attribute dev_attr_dev_rescan = __ATTR(rescan, 0200, NULL, /* NVMe: 정의/선언 */
							    dev_rescan_store); /* NVMe: 코드 수행 */

static ssize_t remove_store(struct device *dev, struct device_attribute *attr, /* NVMe: sysfs 속성 값 저장 함수 선언 */
			    const char *buf, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	unsigned long val; /* NVMe: 사용자 입력/플래그 값 */

	if (kstrtoul(buf, 0, &val) < 0) /* NVMe: 사용자 입력 문자열을 정수로 변환 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	if (val && device_remove_file_self(dev, attr)) /* NVMe: 조건 분기 */
		pci_stop_and_remove_bus_device_locked(to_pci_dev(dev)); /* NVMe: 디바이스 핫리묘브 */
	return count; /* NVMe: 처리된 바이트 수 반환 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_IGNORE_LOCKDEP(remove, 0220, NULL, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
				  remove_store); /* NVMe: 코드 수행 */

static ssize_t bus_rescan_store(struct device *dev, /* NVMe: sysfs 속성 값 저장 함수 선언 */
				struct device_attribute *attr, /* NVMe: 함수 매개변수 선언 */
				const char *buf, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	unsigned long val; /* NVMe: 사용자 입력/플래그 값 */
	struct pci_bus *bus = to_pci_bus(dev); /* NVMe: 디바이스에서 PCI 버스 획득 */

	if (kstrtoul(buf, 0, &val) < 0) /* NVMe: 사용자 입력 문자열을 정수로 변환 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	if (val) { /* NVMe: 조건/반복 블록 시작 */
		pci_lock_rescan_remove(); /* NVMe: 재스캔/제거 동작 직렬화 */
		if (!pci_is_root_bus(bus) && list_empty(&bus->devices)) /* NVMe: 루트 버스 여부 확인 */
			pci_rescan_bus_bridge_resize(bus->self); /* NVMe: 빈 브리지 리사이즈 */
		else /* NVMe: 대안 분기 */
			pci_rescan_bus(bus); /* NVMe: 버스 재스캔으로 핫플러그/변경 반영 */
		pci_unlock_rescan_remove(); /* NVMe: 재스캔/제거 동작 직렬화 */
	} /* NVMe: 조건/반복 블록 종료 */
	return count; /* NVMe: 처리된 바이트 수 반환 */
} /* NVMe: 함수 본문 종료 */
static struct device_attribute dev_attr_bus_rescan = __ATTR(rescan, 0200, NULL, /* NVMe: 정의/선언 */
							    bus_rescan_store); /* NVMe: 코드 수행 */

static ssize_t reset_subordinate_store(struct device *dev, /* NVMe: sysfs 속성 값 저장 함수 선언 */
				struct device_attribute *attr, /* NVMe: 함수 매개변수 선언 */
				const char *buf, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	unsigned long val; /* NVMe: 사용자 입력/플래그 값 */

	if (!capable(CAP_SYS_ADMIN)) /* NVMe: root 권한(CAP_SYS_ADMIN) 검사 */
		return -EPERM; /* NVMe: 권한 없음 오류 반환 */

	if (kstrtoul(buf, 0, &val) < 0) /* NVMe: 사용자 입력 문자열을 정수로 변환 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	if (val) { /* NVMe: 조건/반복 블록 시작 */
		int ret = pci_try_reset_bridge(pdev); /* NVMe: 브리지 리셋 시도 */

		if (ret) /* NVMe: 이전 PCI 작업 오류 시 처리 */
			return ret; /* NVMe: 이전 작업 결과 반환 */
	} /* NVMe: 조건/반복 블록 종료 */

	return count; /* NVMe: 처리된 바이트 수 반환 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_WO(reset_subordinate); /* NVMe: sysfs 장치 속성 등록 */

#if defined(CONFIG_PM) && defined(CONFIG_ACPI) /* NVMe: 해당 설정 시에만 컴파일 */
static ssize_t d3cold_allowed_store(struct device *dev, /* NVMe: sysfs 속성 값 저장 함수 선언 */
				    struct device_attribute *attr, /* NVMe: 함수 매개변수 선언 */
				    const char *buf, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	unsigned long val; /* NVMe: 사용자 입력/플래그 값 */

	if (kstrtoul(buf, 0, &val) < 0) /* NVMe: 사용자 입력 문자열을 정수로 변환 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	pdev->d3cold_allowed = !!val; /* NVMe: NVMe D3cold 전환 허용 여부 설정 */
	pci_bridge_d3_update(pdev); /* NVMe: 브리지 D3cold 상태 갱신 */

	pm_runtime_resume(dev); /* NVMe: 런타임 PM 해제/재개 */

	return count; /* NVMe: 처리된 바이트 수 반환 */
} /* NVMe: 함수 본문 종료 */

static ssize_t d3cold_allowed_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
				   struct device_attribute *attr, char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	return sysfs_emit(buf, "%u\n", pdev->d3cold_allowed); /* NVMe: sysfs 속성 값 출력 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RW(d3cold_allowed); /* NVMe: sysfs 장치 속성 등록 */
#endif /* NVMe: 전처리 조건 블록 종료 */

#ifdef CONFIG_OF /* NVMe: 해당 설정 시에만 컴파일 */
static ssize_t devspec_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
			    struct device_attribute *attr, char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	struct device_node *np = pci_device_to_OF_node(pdev); /* NVMe: OF 디바이스 노드 획득 */

	if (np == NULL) /* NVMe: OF 노드 존재 확인 */
		return 0; /* NVMe: 기본값 반환 */
	return sysfs_emit(buf, "%pOF\n", np); /* NVMe: sysfs 속성 값 출력 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(devspec); /* NVMe: sysfs 장치 속성 등록 */
#endif /* NVMe: 전처리 조건 블록 종료 */

static struct attribute *pci_dev_attrs[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&dev_attr_power_state.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_resource.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_vendor.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_device.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_subsystem_vendor.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_subsystem_device.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_revision.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_class.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_irq.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_local_cpus.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_local_cpulist.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_modalias.attr, /* NVMe: sysfs 속성 등록 */
#ifdef CONFIG_NUMA /* NVMe: 해당 설정 시에만 컴파일 */
	&dev_attr_numa_node.attr, /* NVMe: sysfs 속성 등록 */
#endif /* NVMe: 전처리 조건 블록 종료 */
	&dev_attr_dma_mask_bits.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_consistent_dma_mask_bits.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_enable.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_broken_parity_status.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_msi_bus.attr, /* NVMe: sysfs 속성 등록 */
#if defined(CONFIG_PM) && defined(CONFIG_ACPI) /* NVMe: 해당 설정 시에만 컴파일 */
	&dev_attr_d3cold_allowed.attr, /* NVMe: sysfs 속성 등록 */
#endif /* NVMe: 전처리 조건 블록 종료 */
#ifdef CONFIG_OF /* NVMe: 해당 설정 시에만 컴파일 */
	&dev_attr_devspec.attr, /* NVMe: sysfs 속성 등록 */
#endif /* NVMe: 전처리 조건 블록 종료 */
	&dev_attr_ari_enabled.attr, /* NVMe: sysfs 속성 등록 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static struct attribute *pci_bridge_attrs[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&dev_attr_subordinate_bus_number.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_secondary_bus_number.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_reset_subordinate.attr, /* NVMe: sysfs 속성 등록 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static struct attribute *pcie_dev_attrs[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&dev_attr_current_link_speed.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_current_link_width.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_max_link_width.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_max_link_speed.attr, /* NVMe: sysfs 속성 등록 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static struct attribute *pcibus_attrs[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&dev_attr_bus_rescan.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_cpuaffinity.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_cpulistaffinity.attr, /* NVMe: sysfs 속성 등록 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static const struct attribute_group pcibus_group = { /* NVMe: 구조체/배열 초기화 시작 */
	.attrs = pcibus_attrs, /* NVMe: 코드 수행 */
}; /* NVMe: 구조체/배열 초기화 종료 */

const struct attribute_group *pcibus_groups[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&pcibus_group, /* NVMe: attribute group 등록 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static ssize_t boot_vga_show(struct device *dev, struct device_attribute *attr, /* NVMe: sysfs 속성 값 출력 함수 선언 */
			     char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	struct pci_dev *vga_dev = vga_default_device(); /* NVMe: 기본 VGA 장치 획득 */

	if (vga_dev) /* NVMe: VGA 장치 존재 확인 */
		return sysfs_emit(buf, "%u\n", (pdev == vga_dev)); /* NVMe: sysfs 속성 값 출력 */

	return sysfs_emit(buf, "%u\n", /* NVMe: sysfs 속성 값 출력 */
			  !!(pdev->resource[PCI_ROM_RESOURCE].flags & /* NVMe: 코드 수행 */
			     IORESOURCE_ROM_SHADOW)); /* NVMe: 코드 수행 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RO(boot_vga); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t serial_number_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
				  struct device_attribute *attr, char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	u64 dsn; /* NVMe: 장치 시리얼 번호(DSN) */
	u8 bytes[8]; /* NVMe: 바이트 배열 */

	dsn = pci_get_dsn(pci_dev); /* NVMe: 장치 시리얼 번호(DSN) 획득 */
	if (!dsn) /* NVMe: DSN 존재 확인 */
		return -EIO; /* NVMe: I/O 오류 반환 */

	put_unaligned_be64(dsn, bytes); /* NVMe: 시리얼 번호를 빅엔디안 8바이트로 기록 */
	return sysfs_emit(buf, "%8phD\n", bytes); /* NVMe: sysfs 속성 값 출력 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_ADMIN_RO(serial_number); /* NVMe: sysfs 장치 속성 등록 */

static ssize_t pci_read_config(struct file *filp, struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
			       const struct bin_attribute *bin_attr, char *buf, /* NVMe: 함수 매개변수 선언 */
			       loff_t off, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *dev = to_pci_dev(kobj_to_dev(kobj)); /* NVMe: PCIe 장치 구조체 포인터 */
	unsigned int size = 64; /* NVMe: 코드 수행 */
	loff_t init_off = off; /* NVMe: 초기 오프셋 저장 */
	u8 *data = (u8 *) buf; /* NVMe: 사용자 버퍼 바이트 포인터 */

	/* Several chips lock up trying to read undefined config space */
	if (file_ns_capable(filp, &init_user_ns, CAP_SYS_ADMIN)) /* NVMe: root 권한(CAP_SYS_ADMIN) 검사 */
		size = dev->cfg_size; /* NVMe: 코드 수행 */
	else if (dev->hdr_type == PCI_HEADER_TYPE_CARDBUS) /* NVMe: 제어 흐름 */
		size = 128; /* NVMe: 코드 수행 */

	if (off > size) /* NVMe: 버퍼/크기 조건 검사 */
		return 0; /* NVMe: 기본값 반환 */
	if (off + count > size) { /* NVMe: 조건/반복 블록 시작 */
		size -= off; /* NVMe: 코드 수행 */
		count = size; /* NVMe: 버퍼 초과 시 읽을 크기 조정 */
	} else { /* NVMe: else 블록 시작 */
		size = count; /* NVMe: 쓰기 크기를 사용자 요청 count로 초기화 */
	} /* NVMe: 조건/반복 블록 종료 */

	pci_config_pm_runtime_get(dev); /* NVMe: 설정공간 접근 전 런타임 PM 활성화 */

	if ((off & 1) && size) { /* NVMe: 조건/반복 블록 시작 */
		u8 val; /* NVMe: 사용자 입력/레지스터 값 */
		pci_user_read_config_byte(dev, off, &val); /* NVMe: PCI 설정공간 바이트 읽기 */
		data[off - init_off] = val; /* NVMe: 읽은 바이트를 사용자 버퍼에 배치 */
		off++; /* NVMe: 다음 설정공간 오프셋 이동 */
		size--; /* NVMe: 남은 설정공간 크기 조정 */
	} /* NVMe: 조건/반복 블록 종료 */

	if ((off & 3) && size > 2) { /* NVMe: 조건/반복 블록 시작 */
		u16 val; /* NVMe: 사용자 입력/레지스터 값 */
		pci_user_read_config_word(dev, off, &val); /* NVMe: PCI 설정공간 워드 읽기 */
		data[off - init_off] = val & 0xff; /* NVMe: 읽은 바이트를 사용자 버퍼에 배치 */
		data[off - init_off + 1] = (val >> 8) & 0xff; /* NVMe: 코드 수행 */
		off += 2; /* NVMe: 다음 설정공간 오프셋 이동 */
		size -= 2; /* NVMe: 남은 설정공간 크기 조정 */
	} /* NVMe: 조건/반복 블록 종료 */

	while (size > 3) { /* NVMe: 조건/반복 블록 시작 */
		u32 val; /* NVMe: 사용자 입력/레지스터 값 */
		pci_user_read_config_dword(dev, off, &val); /* NVMe: PCI 설정공간 더블워드 읽기 */
		data[off - init_off] = val & 0xff; /* NVMe: 읽은 바이트를 사용자 버퍼에 배치 */
		data[off - init_off + 1] = (val >> 8) & 0xff; /* NVMe: 코드 수행 */
		data[off - init_off + 2] = (val >> 16) & 0xff; /* NVMe: 코드 수행 */
		data[off - init_off + 3] = (val >> 24) & 0xff; /* NVMe: 코드 수행 */
		off += 4; /* NVMe: 다음 설정공간 오프셋 이동 */
		size -= 4; /* NVMe: 남은 설정공간 크기 조정 */
		cond_resched(); /* NVMe: 긴 설정공간 읽기 중 스케줄러 양보 */
	} /* NVMe: 조건/반복 블록 종료 */

	if (size >= 2) { /* NVMe: 조건/반복 블록 시작 */
		u16 val; /* NVMe: 사용자 입력/레지스터 값 */
		pci_user_read_config_word(dev, off, &val); /* NVMe: PCI 설정공간 워드 읽기 */
		data[off - init_off] = val & 0xff; /* NVMe: 읽은 바이트를 사용자 버퍼에 배치 */
		data[off - init_off + 1] = (val >> 8) & 0xff; /* NVMe: 코드 수행 */
		off += 2; /* NVMe: 다음 설정공간 오프셋 이동 */
		size -= 2; /* NVMe: 남은 설정공간 크기 조정 */
	} /* NVMe: 조건/반복 블록 종료 */

	if (size > 0) { /* NVMe: 조건/반복 블록 시작 */
		u8 val; /* NVMe: 사용자 입력/레지스터 값 */
		pci_user_read_config_byte(dev, off, &val); /* NVMe: PCI 설정공간 바이트 읽기 */
		data[off - init_off] = val; /* NVMe: 읽은 바이트를 사용자 버퍼에 배치 */
	} /* NVMe: 조건/반복 블록 종료 */

	pci_config_pm_runtime_put(dev); /* NVMe: 설정공간 접근 후 런타임 PM 해제 */

	return count; /* NVMe: 처리된 바이트 수 반환 */
} /* NVMe: 조건/반복 블록 종료 */

static ssize_t pci_write_config(struct file *filp, struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
				const struct bin_attribute *bin_attr, char *buf, /* NVMe: 함수 매개변수 선언 */
				loff_t off, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *dev = to_pci_dev(kobj_to_dev(kobj)); /* NVMe: PCIe 장치 구조체 포인터 */
	unsigned int size = count; /* NVMe: 쓰기 크기를 사용자 요청 count로 초기화 */
	loff_t init_off = off; /* NVMe: 초기 오프셋 저장 */
	u8 *data = (u8 *) buf; /* NVMe: 사용자 버퍼 바이트 포인터 */
	int ret; /* NVMe: 작업 결과/오류 저장 */

	ret = security_locked_down(LOCKDOWN_PCI_ACCESS); /* NVMe: lockdown 설정공간 접근 제한 검사 */
	if (ret) /* NVMe: 이전 PCI 작업 오류 시 처리 */
		return ret; /* NVMe: 이전 작업 결과 반환 */

	if (resource_is_exclusive(&dev->driver_exclusive_resource, off, /* NVMe: 커널 전용 설정공간 충돌 검사 */
				  count)) { /* NVMe: 코드 블록 시작 */
		pci_warn_once(dev, "%s: Unexpected write to kernel-exclusive config offset %llx", /* NVMe: 장치 관련 커널 메시지 출력 */
			      current->comm, off); /* NVMe: 코드 수행 */
		add_taint(TAINT_USER, LOCKDEP_STILL_OK); /* NVMe: 워크around로 커널 taint 기록 */
	} /* NVMe: 코드 블록 종료 */

	if (off > dev->cfg_size) /* NVMe: 버퍼/크기 조건 검사 */
		return 0; /* NVMe: 기본값 반환 */
	if (off + count > dev->cfg_size) { /* NVMe: 조건/반복 블록 시작 */
		size = dev->cfg_size - off; /* NVMe: 남은 설정공간 크기 조정 */
		count = size; /* NVMe: 버퍼 초과 시 읽을 크기 조정 */
	} /* NVMe: 조건/반복 블록 종료 */

	pci_config_pm_runtime_get(dev); /* NVMe: 설정공간 접근 전 런타임 PM 활성화 */

	if ((off & 1) && size) { /* NVMe: 조건/반복 블록 시작 */
		pci_user_write_config_byte(dev, off, data[off - init_off]); /* NVMe: PCI 설정공간 바이트 쓰기 */
		off++; /* NVMe: 다음 설정공간 오프셋 이동 */
		size--; /* NVMe: 남은 설정공간 크기 조정 */
	} /* NVMe: 조건/반복 블록 종료 */

	if ((off & 3) && size > 2) { /* NVMe: 조건/반복 블록 시작 */
		u16 val = data[off - init_off]; /* NVMe: 버퍼에서 값 조립 */
		val |= (u16) data[off - init_off + 1] << 8; /* NVMe: 바이트 조합하여 16비트 값 구성 */
		pci_user_write_config_word(dev, off, val); /* NVMe: PCI 설정공간 워드 쓰기 */
		off += 2; /* NVMe: 다음 설정공간 오프셋 이동 */
		size -= 2; /* NVMe: 남은 설정공간 크기 조정 */
	} /* NVMe: 조건/반복 블록 종료 */

	while (size > 3) { /* NVMe: 조건/반복 블록 시작 */
		u32 val = data[off - init_off]; /* NVMe: 버퍼에서 값 조립 */
		val |= (u32) data[off - init_off + 1] << 8; /* NVMe: 바이트 조합하여 32비트 값 구성 */
		val |= (u32) data[off - init_off + 2] << 16; /* NVMe: 바이트 조합하여 32비트 값 구성 */
		val |= (u32) data[off - init_off + 3] << 24; /* NVMe: 바이트 조합하여 32비트 값 구성 */
		pci_user_write_config_dword(dev, off, val); /* NVMe: PCI 설정공간 더블워드 쓰기 */
		off += 4; /* NVMe: 다음 설정공간 오프셋 이동 */
		size -= 4; /* NVMe: 남은 설정공간 크기 조정 */
	} /* NVMe: 조건/반복 블록 종료 */

	if (size >= 2) { /* NVMe: 조건/반복 블록 시작 */
		u16 val = data[off - init_off]; /* NVMe: 버퍼에서 값 조립 */
		val |= (u16) data[off - init_off + 1] << 8; /* NVMe: 바이트 조합하여 16비트 값 구성 */
		pci_user_write_config_word(dev, off, val); /* NVMe: PCI 설정공간 워드 쓰기 */
		off += 2; /* NVMe: 다음 설정공간 오프셋 이동 */
		size -= 2; /* NVMe: 남은 설정공간 크기 조정 */
	} /* NVMe: 조건/반복 블록 종료 */

	if (size) /* NVMe: 남은 데이터 존재 확인 */
		pci_user_write_config_byte(dev, off, data[off - init_off]); /* NVMe: PCI 설정공간 바이트 쓰기 */

	pci_config_pm_runtime_put(dev); /* NVMe: 설정공간 접근 후 런타임 PM 해제 */

	return count; /* NVMe: 처리된 바이트 수 반환 */
} /* NVMe: 함수 본문 종료 */
static const BIN_ATTR(config, 0644, pci_read_config, pci_write_config, 0); /* NVMe: sysfs 바이너리 속성 등록 */

static const struct bin_attribute *const pci_dev_config_attrs[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&bin_attr_config, /* NVMe: 바이너리 속성 등록 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static size_t pci_dev_config_attr_bin_size(struct kobject *kobj, /* NVMe: 바이너리 속성 크기 콜백 함수 선언 */
					   const struct bin_attribute *a, /* NVMe: 함수 매개변수 선언 */
					   int n) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	if (pdev->cfg_size > PCI_CFG_SPACE_SIZE) /* NVMe: 설정공간 크기 확인 */
		return PCI_CFG_SPACE_EXP_SIZE; /* NVMe: 함수 결과 반환 */
	return PCI_CFG_SPACE_SIZE; /* NVMe: 함수 결과 반환 */
} /* NVMe: 함수 본문 종료 */

static const struct attribute_group pci_dev_config_attr_group = { /* NVMe: 구조체/배열 초기화 시작 */
	.bin_attrs = pci_dev_config_attrs, /* NVMe: 코드 수행 */
	.bin_size = pci_dev_config_attr_bin_size, /* NVMe: 코드 수행 */
}; /* NVMe: 구조체/배열 초기화 종료 */

/*
 * llseek operation for mmappable PCI resources.
 * May be left unused if the arch doesn't provide them.
 */
static __maybe_unused loff_t /* NVMe: 정의/선언 */
pci_llseek_resource(struct file *filep, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
		    struct kobject *kobj __always_unused, /* NVMe: 함수 매개변수 선언 */
		    const struct bin_attribute *attr, /* NVMe: 함수 매개변수 선언 */
		    loff_t offset, int whence) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	return fixed_size_llseek(filep, offset, whence, attr->size); /* NVMe: BAR 파일 seek 처리 */
} /* NVMe: 함수 본문 종료 */

#ifdef HAVE_PCI_LEGACY /* NVMe: 전처리 조건 블록 시작 */
/**
 * pci_read_legacy_io - read byte(s) from legacy I/O port space
 * @filp: open sysfs file
 * @kobj: kobject corresponding to file to read from
 * @bin_attr: struct bin_attribute for this file
 * @buf: buffer to store results
 * @off: offset into legacy I/O port space
 * @count: number of bytes to read
 *
 * Reads 1, 2, or 4 bytes from legacy I/O port space using an arch specific
 * callback routine (pci_legacy_read).
 */
static ssize_t pci_read_legacy_io(struct file *filp, struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
				  const struct bin_attribute *bin_attr, /* NVMe: 함수 매개변수 선언 */
				  char *buf, loff_t off, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_bus *bus = to_pci_bus(kobj_to_dev(kobj)); /* NVMe: 디바이스에서 PCI 버스 획득 */

	/* Only support 1, 2 or 4 byte accesses */
	if (count != 1 && count != 2 && count != 4) /* NVMe: 허용된 접근 크기 검사 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	return pci_legacy_read(bus, off, (u32 *)buf, count); /* NVMe: 레거시 I/O 포트 접근 */
} /* NVMe: 함수 본문 종료 */

/**
 * pci_write_legacy_io - write byte(s) to legacy I/O port space
 * @filp: open sysfs file
 * @kobj: kobject corresponding to file to read from
 * @bin_attr: struct bin_attribute for this file
 * @buf: buffer containing value to be written
 * @off: offset into legacy I/O port space
 * @count: number of bytes to write
 *
 * Writes 1, 2, or 4 bytes from legacy I/O port space using an arch specific
 * callback routine (pci_legacy_write).
 */
static ssize_t pci_write_legacy_io(struct file *filp, struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
				   const struct bin_attribute *bin_attr, /* NVMe: 함수 매개변수 선언 */
				   char *buf, loff_t off, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_bus *bus = to_pci_bus(kobj_to_dev(kobj)); /* NVMe: 디바이스에서 PCI 버스 획득 */

	/* Only support 1, 2 or 4 byte accesses */
	if (count != 1 && count != 2 && count != 4) /* NVMe: 허용된 접근 크기 검사 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	return pci_legacy_write(bus, off, *(u32 *)buf, count); /* NVMe: 레거시 I/O 포트 접근 */
} /* NVMe: 함수 본문 종료 */

/**
 * pci_mmap_legacy_mem - map legacy PCI memory into user memory space
 * @filp: open sysfs file
 * @kobj: kobject corresponding to device to be mapped
 * @attr: struct bin_attribute for this file
 * @vma: struct vm_area_struct passed to mmap
 *
 * Uses an arch specific callback, pci_mmap_legacy_mem_page_range, to mmap
 * legacy memory space (first meg of bus space) into application virtual
 * memory space.
 */
static int pci_mmap_legacy_mem(struct file *filp, struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
			       const struct bin_attribute *attr, /* NVMe: 함수 매개변수 선언 */
			       struct vm_area_struct *vma) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_bus *bus = to_pci_bus(kobj_to_dev(kobj)); /* NVMe: 디바이스에서 PCI 버스 획득 */

	return pci_mmap_legacy_page_range(bus, vma, pci_mmap_mem); /* NVMe: BAR/레거시 메모리 사용자 매핑 */
} /* NVMe: 함수 본문 종료 */

/**
 * pci_mmap_legacy_io - map legacy PCI IO into user memory space
 * @filp: open sysfs file
 * @kobj: kobject corresponding to device to be mapped
 * @attr: struct bin_attribute for this file
 * @vma: struct vm_area_struct passed to mmap
 *
 * Uses an arch specific callback, pci_mmap_legacy_io_page_range, to mmap
 * legacy IO space (first meg of bus space) into application virtual
 * memory space. Returns -ENOSYS if the operation isn't supported
 */
static int pci_mmap_legacy_io(struct file *filp, struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
			      const struct bin_attribute *attr, /* NVMe: 함수 매개변수 선언 */
			      struct vm_area_struct *vma) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_bus *bus = to_pci_bus(kobj_to_dev(kobj)); /* NVMe: 디바이스에서 PCI 버스 획득 */

	return pci_mmap_legacy_page_range(bus, vma, pci_mmap_io); /* NVMe: BAR/레거시 메모리 사용자 매핑 */
} /* NVMe: 함수 본문 종료 */

/**
 * pci_adjust_legacy_attr - adjustment of legacy file attributes
 * @b: bus to create files under
 * @mmap_type: I/O port or memory
 *
 * Stub implementation. Can be overridden by arch if necessary.
 */
void __weak pci_adjust_legacy_attr(struct pci_bus *b, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
				   enum pci_mmap_state mmap_type) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
} /* NVMe: 함수 본문 종료 */

/**
 * pci_create_legacy_files - create legacy I/O port and memory files
 * @b: bus to create files under
 *
 * Some platforms allow access to legacy I/O port and ISA memory space on
 * a per-bus basis.  This routine creates the files and ties them into
 * their associated read, write and mmap files from pci-sysfs.c
 *
 * On error unwind, but don't propagate the error to the caller
 * as it is ok to set up the PCI bus without these files.
 */
void pci_create_legacy_files(struct pci_bus *b) /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
{ /* NVMe: 함수 본문 시작 */
	int error; /* NVMe: 작업 결과/오류 저장 */

	if (!sysfs_initialized) /* NVMe: sysfs 초기화 상태 */
		return; /* NVMe: 반환 */

	b->legacy_io = kzalloc_objs(struct bin_attribute, 2, GFP_ATOMIC); /* NVMe: sysfs 구조체 메모리 할당 */
	if (!b->legacy_io) /* NVMe: 레거시 I/O 할당 실패 확인 */
		goto kzalloc_err; /* NVMe: 지정 레이블로 이동 */

	sysfs_bin_attr_init(b->legacy_io); /* NVMe: sysfs 바이너리 속성 구조체 초기화 */
	b->legacy_io->attr.name = "legacy_io"; /* NVMe: 레거시 I/O 속성 이름 설정 */
	b->legacy_io->size = 0xffff; /* NVMe: 레거시 I/O 속성 파일 크기 설정 */
	b->legacy_io->attr.mode = 0600; /* NVMe: 레거시 I/O 속성 권한 설정 */
	b->legacy_io->read = pci_read_legacy_io; /* NVMe: 레거시 I/O 읽기 콜백 연결 */
	b->legacy_io->write = pci_write_legacy_io; /* NVMe: 레거시 I/O 쓰기 콜백 연결 */
	/* See pci_create_attr() for motivation */
	b->legacy_io->llseek = pci_llseek_resource; /* NVMe: 레거시 I/O llseek 콜백 연결 */
	b->legacy_io->mmap = pci_mmap_legacy_io; /* NVMe: 레거시 I/O mmap 콜백 연결 */
	b->legacy_io->f_mapping = iomem_get_mapping; /* NVMe: 레거시 I/O iomem 매핑 연결 */
	pci_adjust_legacy_attr(b, pci_mmap_io); /* NVMe: 레거시 속성 아키텍처 조정 */
	error = device_create_bin_file(&b->dev, b->legacy_io); /* NVMe: sysfs 바이너리 파일 생성 */
	if (error) /* NVMe: 이전 PCI 작업 오류 시 처리 */
		goto legacy_io_err; /* NVMe: 지정 레이블로 이동 */

	/* Allocated above after the legacy_io struct */
	b->legacy_mem = b->legacy_io + 1; /* NVMe: 코드 수행 */
	sysfs_bin_attr_init(b->legacy_mem); /* NVMe: sysfs 바이너리 속성 구조체 초기화 */
	b->legacy_mem->attr.name = "legacy_mem"; /* NVMe: 레거시 메모리 속성 이름 설정 */
	b->legacy_mem->size = 1024*1024; /* NVMe: 레거시 메모리 속성 파일 크기 설정 */
	b->legacy_mem->attr.mode = 0600; /* NVMe: 레거시 메모리 속성 권한 설정 */
	b->legacy_mem->mmap = pci_mmap_legacy_mem; /* NVMe: 레거시 메모리 mmap 콜백 연결 */
	/* See pci_create_attr() for motivation */
	b->legacy_mem->llseek = pci_llseek_resource; /* NVMe: 레거시 메모리 llseek 콜백 연결 */
	b->legacy_mem->f_mapping = iomem_get_mapping; /* NVMe: 레거시 메모리 iomem 매핑 연결 */
	pci_adjust_legacy_attr(b, pci_mmap_mem); /* NVMe: 레거시 속성 아키텍처 조정 */
	error = device_create_bin_file(&b->dev, b->legacy_mem); /* NVMe: sysfs 바이너리 파일 생성 */
	if (error) /* NVMe: 이전 PCI 작업 오류 시 처리 */
		goto legacy_mem_err; /* NVMe: 지정 레이블로 이동 */

	return; /* NVMe: 반환 */

legacy_mem_err: /* NVMe: 레이블 */
	device_remove_bin_file(&b->dev, b->legacy_io); /* NVMe: sysfs 바이너리 파일 제거 */
legacy_io_err: /* NVMe: 레이블 */
	kfree(b->legacy_io); /* NVMe: 할당된 sysfs 메모리 해제 */
	b->legacy_io = NULL; /* NVMe: 레거시 포인터 정리 */
kzalloc_err: /* NVMe: 레이블 */
	dev_warn(&b->dev, "could not create legacy I/O port and ISA memory resources in sysfs\n"); /* NVMe: 장치 관련 커널 메시지 출력 */
} /* NVMe: 함수 본문 종료 */

void pci_remove_legacy_files(struct pci_bus *b) /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
{ /* NVMe: 함수 본문 시작 */
	if (b->legacy_io) { /* NVMe: 조건/반복 블록 시작 */
		device_remove_bin_file(&b->dev, b->legacy_io); /* NVMe: sysfs 바이너리 파일 제거 */
		device_remove_bin_file(&b->dev, b->legacy_mem); /* NVMe: sysfs 바이너리 파일 제거 */
		kfree(b->legacy_io); /* both are allocated here */
	} /* NVMe: 조건/반복 블록 종료 */
} /* NVMe: 함수 본문 종료 */
#endif /* HAVE_PCI_LEGACY */

#if defined(HAVE_PCI_MMAP) || defined(ARCH_GENERIC_PCI_MMAP_RESOURCE) /* NVMe: 전처리 조건 블록 시작 */
/**
 * pci_mmap_resource - map a PCI resource into user memory space
 * @kobj: kobject for mapping
 * @attr: struct bin_attribute for the file being mapped
 * @vma: struct vm_area_struct passed into the mmap
 * @write_combine: 1 for write_combine mapping
 *
 * Use the regular PCI mapping routines to map a PCI resource into userspace.
 */
static int pci_mmap_resource(struct kobject *kobj, const struct bin_attribute *attr, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
			     struct vm_area_struct *vma, int write_combine) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	int bar = (unsigned long)attr->private; /* NVMe: BAR 인덱스 */
	enum pci_mmap_state mmap_type; /* NVMe: mmap 타입 */
	struct resource *res = &pdev->resource[bar]; /* NVMe: BAR/리소스 구조체 포인터 */
	int ret; /* NVMe: 작업 결과/오류 저장 */

	ret = security_locked_down(LOCKDOWN_PCI_ACCESS); /* NVMe: lockdown 설정공간 접근 제한 검사 */
	if (ret) /* NVMe: 이전 PCI 작업 오류 시 처리 */
		return ret; /* NVMe: 이전 작업 결과 반환 */

	if (res->flags & IORESOURCE_MEM && iomem_is_exclusive(res->start)) /* NVMe: 메모리 리소스 및 exclusive 영역 확인 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	if (!pci_mmap_fits(pdev, bar, vma, PCI_MMAP_SYSFS)) /* NVMe: mmap 요청이 BAR 크기에 맞는지 확인 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	mmap_type = res->flags & IORESOURCE_MEM ? pci_mmap_mem : pci_mmap_io; /* NVMe: 코드 수행 */

	return pci_mmap_resource_range(pdev, bar, vma, mmap_type, write_combine); /* NVMe: BAR/레거시 메모리 사용자 매핑 */
} /* NVMe: 함수 본문 종료 */

static int pci_mmap_resource_uc(struct file *filp, struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
				const struct bin_attribute *attr, /* NVMe: 함수 매개변수 선언 */
				struct vm_area_struct *vma) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	return pci_mmap_resource(kobj, attr, vma, 0); /* NVMe: BAR/레거시 메모리 사용자 매핑 */
} /* NVMe: 함수 본문 종료 */

static int pci_mmap_resource_wc(struct file *filp, struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
				const struct bin_attribute *attr, /* NVMe: 함수 매개변수 선언 */
				struct vm_area_struct *vma) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	return pci_mmap_resource(kobj, attr, vma, 1); /* NVMe: BAR/레거시 메모리 사용자 매핑 */
} /* NVMe: 함수 본문 종료 */

static ssize_t pci_resource_io(struct file *filp, struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
			       const struct bin_attribute *attr, char *buf, /* NVMe: 함수 매개변수 선언 */
			       loff_t off, size_t count, bool write) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
#ifdef CONFIG_HAS_IOPORT /* NVMe: 해당 설정 시에만 컴파일 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	int bar = (unsigned long)attr->private; /* NVMe: BAR 인덱스 */
	unsigned long port = off; /* NVMe: I/O 포트 주소 */

	port += pci_resource_start(pdev, bar); /* NVMe: BAR 리소스 시작 주소 조회 */

	if (port > pci_resource_end(pdev, bar)) /* NVMe: BAR 리소스 끝 주소 조회 */
		return 0; /* NVMe: 기본값 반환 */

	if (port + count - 1 > pci_resource_end(pdev, bar)) /* NVMe: BAR 리소스 끝 주소 조회 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	switch (count) { /* [한국어] 사용자가 쓴 바이트 수로 분기 */
	case 1: /* NVMe: 1바이트 접근 */
		if (write) /* NVMe: 쓰기/읽기 모드 분기 */
			outb(*(u8 *)buf, port); /* NVMe: 레거시 I/O 포트 바이트 쓰기 */
		else /* NVMe: 대안 분기 */
			*(u8 *)buf = inb(port);
		return 1; /* NVMe: 함수 결과 반환 */
	case 2: /* NVMe: 2바이트 접근 */
		if (write) /* NVMe: 쓰기/읽기 모드 분기 */
			outw(*(u16 *)buf, port); /* NVMe: 레거시 I/O 포트 워드 쓰기 */
		else /* NVMe: 대안 분기 */
			*(u16 *)buf = inw(port);
		return 2; /* NVMe: 함수 결과 반환 */
	case 4: /* NVMe: 4바이트 접근 */
		if (write) /* NVMe: 쓰기/읽기 모드 분기 */
			outl(*(u32 *)buf, port); /* NVMe: 레거시 I/O 포트 더블워드 쓰기 */
		else /* NVMe: 대안 분기 */
			*(u32 *)buf = inl(port);
		return 4; /* NVMe: 함수 결과 반환 */
	} /* NVMe: 조건/반복 블록 종료 */
	return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */
#else /* NVMe: 이전 전처리 조건 미만족 시 대안 코드 */
	return -ENXIO; /* NVMe: 장치 없음 오류 반환 */
#endif /* NVMe: 전처리 조건 블록 종료 */
} /* NVMe: 함수 본문 종료 */

static ssize_t pci_read_resource_io(struct file *filp, struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
				    const struct bin_attribute *attr, char *buf, /* NVMe: 함수 매개변수 선언 */
				    loff_t off, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	return pci_resource_io(filp, kobj, attr, buf, off, count, false); /* NVMe: I/O 리소스 접근 */
} /* NVMe: 함수 본문 종료 */

static ssize_t pci_write_resource_io(struct file *filp, struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
				     const struct bin_attribute *attr, char *buf, /* NVMe: 함수 매개변수 선언 */
				     loff_t off, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	int ret; /* NVMe: 작업 결과/오류 저장 */

	ret = security_locked_down(LOCKDOWN_PCI_ACCESS); /* NVMe: lockdown 설정공간 접근 제한 검사 */
	if (ret) /* NVMe: 이전 PCI 작업 오류 시 처리 */
		return ret; /* NVMe: 이전 작업 결과 반환 */

	return pci_resource_io(filp, kobj, attr, buf, off, count, true); /* NVMe: I/O 리소스 접근 */
} /* NVMe: 함수 본문 종료 */

/**
 * pci_remove_resource_files - cleanup resource files
 * @pdev: dev to cleanup
 *
 * If we created resource files for @pdev, remove them from sysfs and
 * free their resources.
 */
static void pci_remove_resource_files(struct pci_dev *pdev) /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
{ /* NVMe: 함수 본문 시작 */
	int i; /* NVMe: 루프 인덱스 */

	for (i = 0; i < PCI_STD_NUM_BARS; i++) { /* NVMe: 조건/반복 블록 시작 */
		struct bin_attribute *res_attr; /* NVMe: 바이너리 속성 구조체 포인터 */

		res_attr = pdev->res_attr[i]; /* NVMe: 코드 수행 */
		if (res_attr) { /* NVMe: 조건/반복 블록 시작 */
			sysfs_remove_bin_file(&pdev->dev.kobj, res_attr); /* NVMe: sysfs 바이너리 속성 파일 제거 */
			kfree(res_attr); /* NVMe: 할당된 sysfs 메모리 해제 */
		} /* NVMe: 조건/반복 블록 종료 */

		res_attr = pdev->res_attr_wc[i]; /* NVMe: 코드 수행 */
		if (res_attr) { /* NVMe: 조건/반복 블록 시작 */
			sysfs_remove_bin_file(&pdev->dev.kobj, res_attr); /* NVMe: sysfs 바이너리 속성 파일 제거 */
			kfree(res_attr); /* NVMe: 할당된 sysfs 메모리 해제 */
		} /* NVMe: 조건/반복 블록 종료 */
	} /* NVMe: 조건/반복 블록 종료 */
} /* NVMe: 함수 본문 종료 */

static int pci_create_attr(struct pci_dev *pdev, int num, int write_combine) /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
{ /* NVMe: 함수 본문 시작 */
	/* allocate attribute structure, piggyback attribute name */
	int name_len = write_combine ? 13 : 10; /* NVMe: 정의/선언 */
	struct bin_attribute *res_attr; /* NVMe: 바이너리 속성 구조체 포인터 */
	char *res_attr_name; /* NVMe: 속성 이름 문자열 */
	int retval; /* NVMe: 작업 결과/오류 저장 */

	res_attr = kzalloc(sizeof(*res_attr) + name_len, GFP_ATOMIC); /* NVMe: sysfs 구조체 메모리 할당 */
	if (!res_attr) /* NVMe: 조건 분기 */
		return -ENOMEM; /* NVMe: 메모리 부족 오류 반환 */

	res_attr_name = (char *)(res_attr + 1); /* NVMe: BAR sysfs 속성 포인터 갱신 */

	sysfs_bin_attr_init(res_attr); /* NVMe: sysfs 바이너리 속성 구조체 초기화 */
	if (write_combine) { /* NVMe: 조건/반복 블록 시작 */
		sprintf(res_attr_name, "resource%d_wc", num); /* NVMe: 속성 이름 문자열 생성 */
		res_attr->mmap = pci_mmap_resource_wc; /* NVMe: mmap 콜백 연결 */
	} else { /* NVMe: else 블록 시작 */
		sprintf(res_attr_name, "resource%d", num); /* NVMe: 속성 이름 문자열 생성 */
		if (pci_resource_flags(pdev, num) & IORESOURCE_IO) { /* NVMe: 조건/반복 블록 시작 */
			res_attr->read = pci_read_resource_io; /* NVMe: 읽기 콜백 연결 */
			res_attr->write = pci_write_resource_io; /* NVMe: 쓰기 콜백 연결 */
			if (arch_can_pci_mmap_io()) /* NVMe: 아키텍처 I/O mmap 지원 여부 */
				res_attr->mmap = pci_mmap_resource_uc; /* NVMe: mmap 콜백 연결 */
		} else { /* NVMe: else 블록 시작 */
			res_attr->mmap = pci_mmap_resource_uc; /* NVMe: mmap 콜백 연결 */
		} /* NVMe: 조건/반복 블록 종료 */
	} /* NVMe: 조건/반복 블록 종료 */
	if (res_attr->mmap) { /* NVMe: 조건/반복 블록 시작 */
		res_attr->f_mapping = iomem_get_mapping; /* NVMe: iomem 매핑 연결 */
		/*
		 * generic_file_llseek() consults f_mapping->host to determine
		 * the file size. As iomem_inode knows nothing about the
		 * attribute, it's not going to work, so override it as well.
		 */
		res_attr->llseek = pci_llseek_resource; /* NVMe: llseek 콜백 연결 */
	} /* NVMe: 조건/반복 블록 종료 */
	res_attr->attr.name = res_attr_name; /* NVMe: 속성 이름 설정 */
	res_attr->attr.mode = 0600; /* NVMe: 속성 권한 설정 */
	res_attr->size = pci_resource_len(pdev, num); /* NVMe: BAR 리소스 길이 조회 */
	res_attr->private = (void *)(unsigned long)num; /* NVMe: BAR 인덱스를 private로 저장 */
	retval = sysfs_create_bin_file(&pdev->dev.kobj, res_attr); /* NVMe: sysfs 바이너리 파일 생성 */
	if (retval) { /* NVMe: 조건/반복 블록 시작 */
		kfree(res_attr); /* NVMe: 할당된 sysfs 메모리 해제 */
		return retval; /* NVMe: 이전 작업 결과 반환 */
	} /* NVMe: 조건/반복 블록 종료 */

	if (write_combine) /* NVMe: WC 매핑 여부 분기 */
		pdev->res_attr_wc[num] = res_attr; /* NVMe: 코드 수행 */
	else /* NVMe: 대안 분기 */
		pdev->res_attr[num] = res_attr; /* NVMe: 코드 수행 */

	return 0; /* NVMe: 기본값 반환 */
} /* NVMe: 조건/반복 블록 종료 */

/**
 * pci_create_resource_files - create resource files in sysfs for @dev
 * @pdev: dev in question
 *
 * Walk the resources in @pdev creating files for each resource available.
 */
static int pci_create_resource_files(struct pci_dev *pdev) /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
{ /* NVMe: 함수 본문 시작 */
	int i; /* NVMe: 루프 인덱스 */
	int retval; /* NVMe: 작업 결과/오류 저장 */

	/* Skip devices with non-mappable BARs */
	if (pdev->non_mappable_bars) /* NVMe: 매핑 불가능 BAR 확인 */
		return 0; /* NVMe: 기본값 반환 */

	/* Expose the PCI resources from this device as files */
	for (i = 0; i < PCI_STD_NUM_BARS; i++) { /* NVMe: 조건/반복 블록 시작 */

		/* skip empty resources */
		if (!pci_resource_len(pdev, i)) /* NVMe: BAR 리소스 길이 조회 */
			continue; /* NVMe: 다음 반복 계속 */

		retval = pci_create_attr(pdev, i, 0); /* NVMe: 코드 수행 */
		/* for prefetchable resources, create a WC mappable file */
		if (!retval && arch_can_pci_mmap_wc() && /* NVMe: 아키텍처 WC mmap 지원 여부 */
		    pdev->resource[i].flags & IORESOURCE_PREFETCH) /* NVMe: 코드 수행 */
			retval = pci_create_attr(pdev, i, 1); /* NVMe: 코드 수행 */
		if (retval) { /* NVMe: 조건/반복 블록 시작 */
			pci_remove_resource_files(pdev); /* NVMe: BAR sysfs 파일 제거 */
			return retval; /* NVMe: 이전 작업 결과 반환 */
		} /* NVMe: 조건/반복 블록 종료 */
	} /* NVMe: 조건/반복 블록 종료 */
	return 0; /* NVMe: 기본값 반환 */
} /* NVMe: 함수 본문 종료 */
#else /* !(defined(HAVE_PCI_MMAP) || defined(ARCH_GENERIC_PCI_MMAP_RESOURCE)) */
int __weak pci_create_resource_files(struct pci_dev *dev) { return 0; } /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
void __weak pci_remove_resource_files(struct pci_dev *dev) { return; } /* NVMe: BAR sysfs 파일 제거 */
#endif /* NVMe: 전처리 조건 블록 종료 */

/**
 * pci_write_rom - used to enable access to the PCI ROM display
 * @filp: sysfs file
 * @kobj: kernel object handle
 * @bin_attr: struct bin_attribute for this file
 * @buf: user input
 * @off: file offset
 * @count: number of byte in input
 *
 * writing anything except 0 enables it
 */
static ssize_t pci_write_rom(struct file *filp, struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
			     const struct bin_attribute *bin_attr, char *buf, /* NVMe: 함수 매개변수 선언 */
			     loff_t off, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	if ((off ==  0) && (*buf == '0') && (count == 2)) /* NVMe: ROM 비활성화 입력 확인 */
		pdev->rom_attr_enabled = 0; /* NVMe: NVMe ROM sysfs 접근 비활성화 */
	else /* NVMe: 대안 분기 */
		pdev->rom_attr_enabled = 1; /* NVMe: NVMe ROM sysfs 접근 활성화 플래그 */

	return count; /* NVMe: 처리된 바이트 수 반환 */
} /* NVMe: 함수 본문 종료 */

/**
 * pci_read_rom - read a PCI ROM
 * @filp: sysfs file
 * @kobj: kernel object handle
 * @bin_attr: struct bin_attribute for this file
 * @buf: where to put the data we read from the ROM
 * @off: file offset
 * @count: number of bytes to read
 *
 * Put @count bytes starting at @off into @buf from the ROM in the PCI
 * device corresponding to @kobj.
 */
static ssize_t pci_read_rom(struct file *filp, struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
			    const struct bin_attribute *bin_attr, char *buf, /* NVMe: 함수 매개변수 선언 */
			    loff_t off, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	void __iomem *rom; /* NVMe: MMIO 매핑 포인터 */
	size_t size; /* NVMe: 버퍼/바이트 크기 */

	if (!pdev->rom_attr_enabled) /* NVMe: ROM 접근 활성화 여부 확인 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	rom = pci_map_rom(pdev, &size);	/* size starts out as PCI window size */
	if (!rom || !size) /* NVMe: ROM 매핑 실패 확인 */
		return -EIO; /* NVMe: I/O 오류 반환 */

	if (off >= size) /* NVMe: 오프셋/크기 조건 검사 */
		count = 0; /* NVMe: 오프셋이 크기 이상이면 0바이트 반환 */
	else { /* NVMe: 조건/반복 블록 시작 */
		if (off + count > size) /* NVMe: 버퍼/크기 조건 검사 */
			count = size - off; /* NVMe: 버퍼 초과 시 읽을 크기 조정 */

		memcpy_fromio(buf, rom + off, count); /* NVMe: IO 메모리에서 ROM 데이터 복사 */
	} /* NVMe: 조건/반복 블록 종료 */
	pci_unmap_rom(pdev, rom); /* NVMe: ROM 매핑 해제 */

	return count; /* NVMe: 처리된 바이트 수 반환 */
} /* NVMe: 함수 본문 종료 */
static const BIN_ATTR(rom, 0600, pci_read_rom, pci_write_rom, 0); /* NVMe: sysfs 바이너리 속성 등록 */

static const struct bin_attribute *const pci_dev_rom_attrs[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&bin_attr_rom, /* NVMe: 바이너리 속성 등록 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static umode_t pci_dev_rom_attr_is_visible(struct kobject *kobj, /* NVMe: 속성 노출 조건 콜백 함수 선언 */
					   const struct bin_attribute *a, int n) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	/* If the device has a ROM, try to expose it in sysfs. */
	if (!pci_resource_end(pdev, PCI_ROM_RESOURCE)) /* NVMe: BAR 리소스 끝 주소 조회 */
		return 0; /* NVMe: 기본값 반환 */

	return a->attr.mode; /* NVMe: 함수 결과 반환 */
} /* NVMe: 함수 본문 종료 */

static size_t pci_dev_rom_attr_bin_size(struct kobject *kobj, /* NVMe: 바이너리 속성 크기 콜백 함수 선언 */
					const struct bin_attribute *a, int n) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	return pci_resource_len(pdev, PCI_ROM_RESOURCE); /* NVMe: BAR 리소스 길이 조회 */
} /* NVMe: 함수 본문 종료 */

static const struct attribute_group pci_dev_rom_attr_group = { /* NVMe: 구조체/배열 초기화 시작 */
	.bin_attrs = pci_dev_rom_attrs, /* NVMe: 코드 수행 */
	.is_bin_visible = pci_dev_rom_attr_is_visible, /* NVMe: 코드 수행 */
	.bin_size = pci_dev_rom_attr_bin_size, /* NVMe: 코드 수행 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static ssize_t reset_store(struct device *dev, struct device_attribute *attr, /* NVMe: sysfs 속성 값 저장 함수 선언 */
			   const char *buf, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	unsigned long val; /* NVMe: 사용자 입력/플래그 값 */
	ssize_t result; /* NVMe: 작업 결과/오류 저장 */

	if (kstrtoul(buf, 0, &val) < 0) /* NVMe: 사용자 입력 문자열을 정수로 변환 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	if (val != 1) /* NVMe: 유효한 리셋 트리거 값 확인 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	pm_runtime_get_sync(dev); /* NVMe: 런타임 PM 활성화 */
	result = pci_reset_function(pdev); /* NVMe: 기능 레벨 리셋 수행 */
	pm_runtime_put(dev); /* NVMe: 런타임 PM 해제 */
	if (result < 0) /* NVMe: 이전 PCI 작업 오류 시 처리 */
		return result; /* NVMe: 이전 작업 결과 반환 */

	return count; /* NVMe: 처리된 바이트 수 반환 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_WO(reset); /* NVMe: sysfs 장치 속성 등록 */

static struct attribute *pci_dev_reset_attrs[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&dev_attr_reset.attr, /* NVMe: sysfs 속성 등록 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static umode_t pci_dev_reset_attr_is_visible(struct kobject *kobj, /* NVMe: 속성 노출 조건 콜백 함수 선언 */
					     struct attribute *a, int n) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	if (!pci_reset_supported(pdev)) /* NVMe: 리셋 메커니즘 지원 여부 */
		return 0; /* NVMe: 기본값 반환 */

	return a->mode; /* NVMe: 함수 결과 반환 */
} /* NVMe: 함수 본문 종료 */

static const struct attribute_group pci_dev_reset_attr_group = { /* NVMe: 구조체/배열 초기화 시작 */
	.attrs = pci_dev_reset_attrs, /* NVMe: 코드 수행 */
	.is_visible = pci_dev_reset_attr_is_visible, /* NVMe: 코드 수행 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static ssize_t reset_method_show(struct device *dev, /* NVMe: sysfs 속성 값 출력 함수 선언 */
				 struct device_attribute *attr, char *buf) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	ssize_t len = 0; /* NVMe: 누적 출력 길이 */
	int i, m; /* NVMe: 루프 인덱스 */

	for (i = 0; i < PCI_NUM_RESET_METHODS; i++) { /* NVMe: 조건/반복 블록 시작 */
		m = pdev->reset_methods[i]; /* NVMe: NVMe 장치 리셋 방법 배열 갱신 */
		if (!m) /* NVMe: 리셋 방법 찾지 못함 */
			break; /* NVMe: 반복/분기 종료 */

		len += sysfs_emit_at(buf, len, "%s%s", len ? " " : "", /* NVMe: sysfs 오프셋에 값 추가 */
				     pci_reset_fn_methods[m].name); /* NVMe: 코드 수행 */
	} /* NVMe: 조건/반복 블록 종료 */

	if (len) /* NVMe: 조건 분기 */
		len += sysfs_emit_at(buf, len, "\n"); /* NVMe: sysfs 오프셋에 값 추가 */

	return len; /* NVMe: 누적 출력 길이 반환 */
} /* NVMe: 함수 본문 종료 */

static int reset_method_lookup(const char *name) /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
{ /* NVMe: 함수 본문 시작 */
	int m; /* NVMe: 정의/선언 */

	for (m = 1; m < PCI_NUM_RESET_METHODS; m++) { /* NVMe: 조건/반복 블록 시작 */
		if (sysfs_streq(name, pci_reset_fn_methods[m].name)) /* NVMe: sysfs 문자열 비교 */
			return m; /* NVMe: 함수 결과 반환 */
	} /* NVMe: 조건/반복 블록 종료 */

	return 0;	/* not found */
} /* NVMe: 함수 본문 종료 */

static ssize_t reset_method_store(struct device *dev, /* NVMe: sysfs 속성 값 저장 함수 선언 */
				  struct device_attribute *attr, /* NVMe: 함수 매개변수 선언 */
				  const char *buf, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	char *tmp_options, *name; /* NVMe: 문자열 파싱용 포인터 */
	int m, n; /* NVMe: 루프/배열 인덱스 */
	u8 reset_methods[PCI_NUM_RESET_METHODS] = {}; /* NVMe: 리셋 방법 배열 */

	if (sysfs_streq(buf, "")) { /* NVMe: 조건/반복 블록 시작 */
		pdev->reset_methods[0] = 0; /* NVMe: device-specific 리셋이 최우선 방법인지 확인 */
		pci_warn(pdev, "All device reset methods disabled by user"); /* NVMe: 장치 관련 커널 메시지 출력 */
		return count; /* NVMe: 처리된 바이트 수 반환 */
	} /* NVMe: 조건/반복 블록 종료 */

	PM_RUNTIME_ACQUIRE(dev, pm); /* NVMe: 코드 수행 */
	if (PM_RUNTIME_ACQUIRE_ERR(&pm)) /* NVMe: 조건 분기 */
		return -ENXIO; /* NVMe: 장치 없음 오류 반환 */

	if (sysfs_streq(buf, "default")) { /* NVMe: 조건/반복 블록 시작 */
		pci_init_reset_methods(pdev); /* NVMe: 리셋 방법 초기화 */
		return count; /* NVMe: 처리된 바이트 수 반환 */
	} /* NVMe: 조건/반복 블록 종료 */

	char *options __free(kfree) = kstrndup(buf, count, GFP_KERNEL); /* NVMe: 사용자 입력 문자열 복사 */
	if (!options) /* NVMe: 조건 분기 */
		return -ENOMEM; /* NVMe: 메모리 부족 오류 반환 */

	n = 0; /* NVMe: 리셋 방법 인덱스 초기화 */
	tmp_options = options; /* NVMe: 문자열 파싱용 임시 포인터 설정 */
	while ((name = strsep(&tmp_options, " ")) != NULL) { /* NVMe: 조건/반복 블록 시작 */
		if (sysfs_streq(name, "")) /* NVMe: sysfs 문자열 비교 */
			continue; /* NVMe: 다음 반복 계속 */

		name = strim(name); /* NVMe: 파싱된 토큰 양쪽 공백 제거 */

		/* Leave previous methods unchanged if input is invalid */
		m = reset_method_lookup(name); /* NVMe: 코드 수행 */
		if (!m) { /* NVMe: 조건/반복 블록 시작 */
			pci_err(pdev, "Invalid reset method '%s'", name); /* NVMe: 장치 관련 커널 메시지 출력 */
			return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */
		} /* NVMe: 조건/반복 블록 종료 */

		if (pci_reset_fn_methods[m].reset_fn(pdev, PCI_RESET_PROBE)) { /* NVMe: 조건/반복 블록 시작 */
			pci_err(pdev, "Unsupported reset method '%s'", name); /* NVMe: 장치 관련 커널 메시지 출력 */
			return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */
		} /* NVMe: 조건/반복 블록 종료 */

		if (n == PCI_NUM_RESET_METHODS - 1) { /* NVMe: 조건/반복 블록 시작 */
			pci_err(pdev, "Too many reset methods\n"); /* NVMe: 장치 관련 커널 메시지 출력 */
			return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */
		} /* NVMe: 조건/반복 블록 종료 */

		reset_methods[n++] = m; /* NVMe: 검증된 리셋 방법을 우선순위 배열에 추가 */
	} /* NVMe: 조건/반복 블록 종료 */

	reset_methods[n] = 0; /* NVMe: 리셋 방법 배열 종료 표시 */

	/* Warn if dev-specific supported but not highest priority */
	if (pci_reset_fn_methods[1].reset_fn(pdev, PCI_RESET_PROBE) == 0 && /* NVMe: 리셋 방법 지원 여부 확인 */
	    reset_methods[0] != 1) /* NVMe: 코드 수행 */
		pci_warn(pdev, "Device-specific reset disabled/de-prioritized by user"); /* NVMe: 장치 관련 커널 메시지 출력 */
	memcpy(pdev->reset_methods, reset_methods, sizeof(pdev->reset_methods)); /* NVMe: 리셋 방법 배열 복사 */
	return count; /* NVMe: 처리된 바이트 수 반환 */
} /* NVMe: 함수 본문 종료 */
static DEVICE_ATTR_RW(reset_method); /* NVMe: sysfs 장치 속성 등록 */

static struct attribute *pci_dev_reset_method_attrs[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&dev_attr_reset_method.attr, /* NVMe: sysfs 속성 등록 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static const struct attribute_group pci_dev_reset_method_attr_group = { /* NVMe: 구조체/배열 초기화 시작 */
	.attrs = pci_dev_reset_method_attrs, /* NVMe: 코드 수행 */
	.is_visible = pci_dev_reset_attr_is_visible, /* NVMe: 코드 수행 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static ssize_t __resource_resize_show(struct device *dev, int n, char *buf) /* NVMe: sysfs 속성 값 출력 함수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	ssize_t ret; /* NVMe: 작업 결과/오류 저장 */

	pci_config_pm_runtime_get(pdev); /* NVMe: 설정공간 접근 전 런타임 PM 활성화 */

	ret = sysfs_emit(buf, "%016llx\n", /* NVMe: 코드 수행 */
			 pci_rebar_get_possible_sizes(pdev, n)); /* NVMe: BAR 리사이즈 가능 크기 조회 */

	pci_config_pm_runtime_put(pdev); /* NVMe: 설정공간 접근 후 런타임 PM 해제 */

	return ret; /* NVMe: 이전 작업 결과 반환 */
} /* NVMe: 함수 본문 종료 */

static ssize_t __resource_resize_store(struct device *dev, int n, /* NVMe: sysfs 속성 값 저장 함수 선언 */
				       const char *buf, size_t count) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */
	struct pci_bus *bus = pdev->bus; /* NVMe: PCI 버스 구조체 포인터 */
	unsigned long size; /* NVMe: 요청된 크기 값 */
	int ret; /* NVMe: 작업 결과/오류 저장 */
	u16 cmd; /* NVMe: PCI COMMAND 레지스터 값 */

	if (kstrtoul(buf, 0, &size) < 0) /* NVMe: 사용자 입력 문자열을 정수로 변환 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환 */

	device_lock(dev); /* NVMe: 디바이스 상태 동시 접근 보호 */
	if (dev->driver || pci_num_vf(pdev)) { /* NVMe: 조건/반복 블록 시작 */
		ret = -EBUSY; /* NVMe: 코드 수행 */
		goto unlock; /* NVMe: 지정 레이블로 이동 */
	} /* NVMe: 조건/반복 블록 종료 */

	pci_config_pm_runtime_get(pdev); /* NVMe: 설정공간 접근 전 런타임 PM 활성화 */

	if ((pdev->class >> 8) == PCI_CLASS_DISPLAY_VGA) { /* NVMe: 조건/반복 블록 시작 */
		ret = aperture_remove_conflicting_pci_devices(pdev, /* NVMe: VGA aperture 충돌 장치 제거 */
						"resourceN_resize"); /* NVMe: 코드 수행 */
		if (ret) /* NVMe: 이전 PCI 작업 오류 시 처리 */
			goto pm_put; /* NVMe: 지정 레이블로 이동 */
	} /* NVMe: 조건/반복 블록 종료 */

	pci_read_config_word(pdev, PCI_COMMAND, &cmd); /* NVMe: PCI 설정공간 워드 읽기 */
	pci_write_config_word(pdev, PCI_COMMAND, /* NVMe: PCI 설정공간 워드 쓰기 */
			      cmd & ~PCI_COMMAND_MEMORY); /* NVMe: 코드 수행 */

	pci_remove_resource_files(pdev); /* NVMe: BAR sysfs 파일 제거 */

	ret = pci_resize_resource(pdev, n, size, 0); /* NVMe: BAR 크기 재조정 */

	pci_assign_unassigned_bus_resources(bus); /* NVMe: 버스 리소스 재할당 */

	if (pci_create_resource_files(pdev)) /* NVMe: BAR sysfs 파일 생성 */
		pci_warn(pdev, "Failed to recreate resource files after BAR resizing\n"); /* NVMe: 장치 관련 커널 메시지 출력 */

	pci_write_config_word(pdev, PCI_COMMAND, cmd); /* NVMe: PCI 설정공간 워드 쓰기 */
pm_put: /* NVMe: 레이블 */
	pci_config_pm_runtime_put(pdev); /* NVMe: 설정공간 접근 후 런타임 PM 해제 */
unlock: /* NVMe: 레이블 */
	device_unlock(dev); /* NVMe: 디바이스 잠금 해제 */

	return ret ? ret : count; /* NVMe: 함수 결과 반환 */
} /* NVMe: 함수 본문 종료 */

#define pci_dev_resource_resize_attr(n) /* NVMe: 매크로 정의 */ \
static ssize_t resource##n##_resize_show(struct device *dev, /* NVMe: 매크로 내 sysfs 속성 값 출력 함수 선언 */ \
					 struct device_attribute *attr, /* NVMe: 정의/선언 */ \
					 char *buf) /* NVMe: sysfs 출력 버퍼 */ \
{ /* NVMe: 매크로 본문 시작 */ \
	return __resource_resize_show(dev, n, buf); /* NVMe: 함수 결과 반환 */ \
} /* NVMe: 매크로 본문 종료 */ \
static ssize_t resource##n##_resize_store(struct device *dev, /* NVMe: 매크로 내 sysfs 속성 값 저장 함수 선언 */ \
					  struct device_attribute *attr, /* NVMe: 정의/선언 */ \
					  const char *buf, size_t count) /* NVMe: sysfs 사용자 버퍼 */ \
{ /* NVMe: 매크로 본문 시작 */ \
	return __resource_resize_store(dev, n, buf, count); /* NVMe: 함수 결과 반환 */ \
} /* NVMe: 매크로 본문 종료 */ \
static DEVICE_ATTR_RW(resource##n##_resize) /* NVMe: sysfs 장치 속성 등록 */

pci_dev_resource_resize_attr(0); /* NVMe: BAR 리사이즈 속성 매크로 생성 */
pci_dev_resource_resize_attr(1); /* NVMe: BAR 리사이즈 속성 매크로 생성 */
pci_dev_resource_resize_attr(2); /* NVMe: BAR 리사이즈 속성 매크로 생성 */
pci_dev_resource_resize_attr(3); /* NVMe: BAR 리사이즈 속성 매크로 생성 */
pci_dev_resource_resize_attr(4); /* NVMe: BAR 리사이즈 속성 매크로 생성 */
pci_dev_resource_resize_attr(5); /* NVMe: BAR 리사이즈 속성 매크로 생성 */

static struct attribute *resource_resize_attrs[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&dev_attr_resource0_resize.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_resource1_resize.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_resource2_resize.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_resource3_resize.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_resource4_resize.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_resource5_resize.attr, /* NVMe: sysfs 속성 등록 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static umode_t resource_resize_is_visible(struct kobject *kobj, /* NVMe: 속성 노출 조건 콜백 함수 선언 */
					  struct attribute *a, int n) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	return pci_rebar_get_current_size(pdev, n) < 0 ? 0 : a->mode; /* NVMe: BAR 현재 크기 조회 */
} /* NVMe: 함수 본문 종료 */

static const struct attribute_group pci_dev_resource_resize_group = { /* NVMe: 구조체/배열 초기화 시작 */
	.attrs = resource_resize_attrs, /* NVMe: 코드 수행 */
	.is_visible = resource_resize_is_visible, /* NVMe: 코드 수행 */
}; /* NVMe: 구조체/배열 초기화 종료 */

int __must_check pci_create_sysfs_dev_files(struct pci_dev *pdev) /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
{ /* NVMe: 함수 본문 시작 */
	if (!sysfs_initialized) /* NVMe: sysfs 초기화 상태 */
		return -EACCES; /* NVMe: 접근 거부 오류 반환 */

	return pci_create_resource_files(pdev); /* NVMe: BAR sysfs 파일 생성 */
} /* NVMe: 함수 본문 종료 */

/**
 * pci_remove_sysfs_dev_files - cleanup PCI specific sysfs files
 * @pdev: device whose entries we should free
 *
 * Cleanup when @pdev is removed from sysfs.
 */
void pci_remove_sysfs_dev_files(struct pci_dev *pdev) /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
{ /* NVMe: 함수 본문 시작 */
	if (!sysfs_initialized) /* NVMe: sysfs 초기화 상태 */
		return; /* NVMe: 반환 */

	pci_remove_resource_files(pdev); /* NVMe: BAR sysfs 파일 제거 */
} /* NVMe: 함수 본문 종료 */

static int __init pci_sysfs_init(void) /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct pci_dev *pdev = NULL; /* NVMe: PCIe 장치 구조체 포인터 */
	struct pci_bus *pbus = NULL; /* NVMe: PCI 버스 구조체 포인터 */
	int retval; /* NVMe: 작업 결과/오류 저장 */

	sysfs_initialized = 1; /* NVMe: sysfs 초기화 상태 */
	for_each_pci_dev(pdev) { /* NVMe: 코드 블록 시작 */
		retval = pci_create_sysfs_dev_files(pdev); /* NVMe: 장치 sysfs 파일 생성 */
		if (retval) { /* NVMe: 조건/반복 블록 시작 */
			pci_dev_put(pdev); /* NVMe: PCI 장치 참조 카운트 감소 */
			return retval; /* NVMe: 이전 작업 결과 반환 */
		} /* NVMe: 조건/반복 블록 종료 */
	} /* NVMe: 코드 블록 종료 */

	while ((pbus = pci_find_next_bus(pbus))) /* NVMe: 다음 PCI 버스 순회 */
		pci_create_legacy_files(pbus); /* NVMe: 레거시 파일 생성 */

	return 0; /* NVMe: 기본값 반환 */
} /* NVMe: 함수 본문 종료 */
late_initcall(pci_sysfs_init); /* NVMe: 모듈 초기화 등록 */

static struct attribute *pci_dev_dev_attrs[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&dev_attr_boot_vga.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_serial_number.attr, /* NVMe: sysfs 속성 등록 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static umode_t pci_dev_attrs_are_visible(struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
					 struct attribute *a, int n) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct device *dev = kobj_to_dev(kobj); /* NVMe: 디바이스 구조체 포인터 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	if (a == &dev_attr_boot_vga.attr && pci_is_vga(pdev)) /* NVMe: VGA 장치 여부 확인 */
		return a->mode; /* NVMe: 함수 결과 반환 */

	if (a == &dev_attr_serial_number.attr && pci_get_dsn(pdev)) /* NVMe: 장치 시리얼 번호(DSN) 획득 */
		return a->mode; /* NVMe: 함수 결과 반환 */

	return 0; /* NVMe: 기본값 반환 */
} /* NVMe: 함수 본문 종료 */

static struct attribute *pci_dev_hp_attrs[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&dev_attr_remove.attr, /* NVMe: sysfs 속성 등록 */
	&dev_attr_dev_rescan.attr, /* NVMe: sysfs 속성 등록 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static umode_t pci_dev_hp_attrs_are_visible(struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
					    struct attribute *a, int n) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct device *dev = kobj_to_dev(kobj); /* NVMe: 디바이스 구조체 포인터 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	if (pdev->is_virtfn) /* NVMe: 가상 기능(VF) 여부 확인 */
		return 0; /* NVMe: 기본값 반환 */

	return a->mode; /* NVMe: 함수 결과 반환 */
} /* NVMe: 함수 본문 종료 */

static umode_t pci_bridge_attrs_are_visible(struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
					    struct attribute *a, int n) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct device *dev = kobj_to_dev(kobj); /* NVMe: 디바이스 구조체 포인터 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	if (pci_is_bridge(pdev)) /* NVMe: 브리지 장치 여부 확인 */
		return a->mode; /* NVMe: 함수 결과 반환 */

	return 0; /* NVMe: 기본값 반환 */
} /* NVMe: 함수 본문 종료 */

static umode_t pcie_dev_attrs_are_visible(struct kobject *kobj, /* NVMe: PCIe 장치 sysfs 콜백 함수 선언 */
					  struct attribute *a, int n) /* NVMe: 함수 매개변수 선언 */
{ /* NVMe: 함수 본문 시작 */
	struct device *dev = kobj_to_dev(kobj); /* NVMe: 디바이스 구조체 포인터 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 디바이스에서 PCIe 장치 획득 */

	if (pci_is_pcie(pdev)) /* NVMe: PCIe 장치 여부 확인 */
		return a->mode; /* NVMe: 함수 결과 반환 */

	return 0; /* NVMe: 기본값 반환 */
} /* NVMe: 함수 본문 종료 */

static const struct attribute_group pci_dev_group = { /* NVMe: 구조체/배열 초기화 시작 */
	.attrs = pci_dev_attrs, /* NVMe: 코드 수행 */
}; /* NVMe: 구조체/배열 초기화 종료 */

const struct attribute_group *pci_dev_groups[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&pci_dev_group, /* NVMe: attribute group 등록 */
	&pci_dev_config_attr_group, /* NVMe: attribute group 등록 */
	&pci_dev_rom_attr_group, /* NVMe: attribute group 등록 */
	&pci_dev_reset_attr_group, /* NVMe: attribute group 등록 */
	&pci_dev_reset_method_attr_group, /* NVMe: attribute group 등록 */
	&pci_dev_vpd_attr_group, /* NVMe: attribute group 등록 */
#ifdef CONFIG_DMI /* NVMe: 해당 설정 시에만 컴파일 */
	&pci_dev_smbios_attr_group, /* NVMe: attribute group 등록 */
#endif /* NVMe: 전처리 조건 블록 종료 */
#ifdef CONFIG_ACPI /* NVMe: 해당 설정 시에만 컴파일 */
	&pci_dev_acpi_attr_group, /* NVMe: attribute group 등록 */
#endif /* NVMe: 전처리 조건 블록 종료 */
	&pci_dev_resource_resize_group, /* NVMe: attribute group 등록 */
	ARCH_PCI_DEV_GROUPS /* NVMe: 아키텍처별 추가 속성 그룹 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static const struct attribute_group pci_dev_hp_attr_group = { /* NVMe: 구조체/배열 초기화 시작 */
	.attrs = pci_dev_hp_attrs, /* NVMe: 코드 수행 */
	.is_visible = pci_dev_hp_attrs_are_visible, /* NVMe: 코드 수행 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static const struct attribute_group pci_dev_attr_group = { /* NVMe: 구조체/배열 초기화 시작 */
	.attrs = pci_dev_dev_attrs, /* NVMe: 코드 수행 */
	.is_visible = pci_dev_attrs_are_visible, /* NVMe: 코드 수행 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static const struct attribute_group pci_bridge_attr_group = { /* NVMe: 구조체/배열 초기화 시작 */
	.attrs = pci_bridge_attrs, /* NVMe: 코드 수행 */
	.is_visible = pci_bridge_attrs_are_visible, /* NVMe: 코드 수행 */
}; /* NVMe: 구조체/배열 초기화 종료 */

static const struct attribute_group pcie_dev_attr_group = { /* NVMe: 구조체/배열 초기화 시작 */
	.attrs = pcie_dev_attrs, /* NVMe: 코드 수행 */
	.is_visible = pcie_dev_attrs_are_visible, /* NVMe: 코드 수행 */
}; /* NVMe: 구조체/배열 초기화 종료 */

const struct attribute_group *pci_dev_attr_groups[] = { /* NVMe: 구조체/배열 초기화 시작 */
	&pci_dev_attr_group, /* NVMe: attribute group 등록 */
	&pci_dev_hp_attr_group, /* NVMe: attribute group 등록 */
#ifdef CONFIG_PCI_IOV /* NVMe: 해당 설정 시에만 컴파일 */
	&sriov_pf_dev_attr_group, /* NVMe: attribute group 등록 */
	&sriov_vf_dev_attr_group, /* NVMe: attribute group 등록 */
#endif /* NVMe: 전처리 조건 블록 종료 */
	&pci_bridge_attr_group, /* NVMe: attribute group 등록 */
	&pcie_dev_attr_group, /* NVMe: attribute group 등록 */
#ifdef CONFIG_PCIEAER /* NVMe: 해당 설정 시에만 컴파일 */
	&aer_stats_attr_group, /* NVMe: attribute group 등록 */
	&aer_attr_group, /* NVMe: attribute group 등록 */
#endif /* NVMe: 전처리 조건 블록 종료 */
#ifdef CONFIG_PCIEASPM /* NVMe: 해당 설정 시에만 컴파일 */
	&aspm_ctrl_attr_group, /* NVMe: attribute group 등록 */
#endif /* NVMe: 전처리 조건 블록 종료 */
#ifdef CONFIG_PCI_DOE /* NVMe: 해당 설정 시에만 컴파일 */
	&pci_doe_sysfs_group, /* NVMe: attribute group 등록 */
#endif /* NVMe: 전처리 조건 블록 종료 */
#ifdef CONFIG_PCI_TSM /* NVMe: 해당 설정 시에만 컴파일 */
	&pci_tsm_auth_attr_group, /* NVMe: attribute group 등록 */
	&pci_tsm_attr_group, /* NVMe: attribute group 등록 */
#endif /* NVMe: 전처리 조건 블록 종료 */
	NULL, /* NVMe: 속성 배열 종료 */
}; /* NVMe: 구조체/배열 초기화 종료 */
