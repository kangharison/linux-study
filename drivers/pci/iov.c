// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Express I/O Virtualization (IOV) support
 *   Single Root IOV 1.0
 *   Address Translation Service 1.0
 *
 * Copyright (C) 2009 Intel Corporation, Yu Zhao <yu.zhao@intel.com>
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/iov.c)은 PCI Express SR-IOV(Single Root I/O
 * Virtualization) capability를 관리한다. NVMe SSD가 SR-IOV capable
 * Physical Function(PF)로 동작할 때 이 파일의 함수가 PF에서 Virtual
 * Function(VF)를 생성/제거하고 VF의 BAR, MSI-X, Resizable BAR(ReBAR),
 * ARI 등의 PCIe 자원을 설정한다.
 *
 * NVMe PCIe 호스트 드라이버(drivers/nvme/host/pci.c)에서 본 파일과
 * 직접 연결되는 부분:
 *   - nvme_driver.sriov_configure = pci_sriov_configure_simple
 *     사용자가 /sys/bus/pci/devices/.../sriov_numvfs에 값을 쓰면
 *     pci_sriov_configure_simple() -> sriov_enable()/sriov_disable()이
 *     호출되어 NVMe PF 아래 VF pci_dev가 생성되거나 제거된다.
 *   - 각 VF는 별도의 pci_dev로 nvme_probe()에 의해 개별 NVMe
 *     컨트롤러로 인식될 수 있다(PF와 VF가 동일한 NVMe 드라이버에
 *     바인딩됨).
 *   - VF BAR 리소스는 PF의 IOV BAR 영역에 매핑되며, VF당 doorbell
 *     register(BAR0), CMB(Controller Memory Buffer), P2PDMA 관련 BAR
 *     등이 독립적으로 노출된다.
 *   - VF MSI-X 벡터 수는 sriov_vf_total_msix_show /
 *     sriov_vf_msix_count_store를 통해 PF 드라이버가 노출/조정한다.
 *   - pci_restore_iov_state()는 NVMe 장치의 D3hot->D0 전환, AER 복구,
 *     PCI slot reset 후 SR-IOV 레지스터와 VF BAR, REBAR 상태를
 *     복원하는 데 사용된다.
 *
 * NVMe와 밀접한 코드:
 *   - VF BAR(address decoding): sriov_init, pci_iov_add_virtfn,
 *     pci_iov_update_resource, sriov_restore_state
 *   - VF MSI/MSI-X: sriov_vf_total_msix_show, sriov_vf_msix_count_store
 *   - VF Resizable BAR(ReBAR): sriov_restore_vf_rebar_state,
 *     pci_iov_vf_bar_set_size, pci_iov_vf_bar_get_sizes
 *   - SR-IOV 활성화/비활성화: pci_enable_sriov, pci_disable_sriov,
 *     pci_sriov_configure_simple
 *   - 버스 번호/ARI: pci_iov_virtfn_bus, pci_iov_virtfn_devfn,
 *     compute_max_vf_buses, pci_iov_bus_range
 * ===================================================================
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/log2.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <asm/div64.h>
#include "pci.h"

#define VIRTFN_ID_LEN	17	/* "virtfn%u\0" for 2^32 - 1 */ /* NVMe: VF sysfs 심볼릭 링크 이름 버퍼 길이 정의. */

/*
 * pci_iov_virtfn_bus:
 *   주어진 VF ID가 위치할 버스 번호를 산출한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
int pci_iov_virtfn_bus(struct pci_dev *dev, int vf_id)
{
	if (!dev->is_physfn) /* NVMe: PF가 아니면. */
		return -EINVAL; /* NVMe: 잘못된 인자. */
	return dev->bus->number + ((dev->devfn + dev->sriov->offset + /* NVMe: PF의 버스 번호에 VF offset/stride로 계산된 버스 오프셋을 더해 VF 버스 번호 산출. */
				    dev->sriov->stride * vf_id) >> 8);
}

/*
 * pci_iov_virtfn_devfn:
 *   주어진 VF ID의 devfn(Device/Function) 번호를 산출한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
int pci_iov_virtfn_devfn(struct pci_dev *dev, int vf_id)
{
	if (!dev->is_physfn) /* NVMe: PF가 아니면. */
		return -EINVAL; /* NVMe: 잘못된 인자. */
	return (dev->devfn + dev->sriov->offset + /* NVMe: PF devfn에 offset과 stride*vf_id를 더해 VF devfn 산출. */
		dev->sriov->stride * vf_id) & 0xff;
}
EXPORT_SYMBOL_GPL(pci_iov_virtfn_devfn); /* NVMe: 심볼 외부 공개. */

/*
 * pci_iov_vf_id:
 *   VF 디바이스로부터 PF 기준 VF 인덱스를 계산한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
int pci_iov_vf_id(struct pci_dev *dev)
{
	struct pci_dev *pf; /* NVMe: PF 디바이스 포인터 선언. */

	if (!dev->is_virtfn) /* NVMe: VF가 아니면 drvdata 획득 불가. */
		return -EINVAL; /* NVMe: 잘못된 인자. */

	pf = pci_physfn(dev); /* NVMe: VF에 대응하는 PF 획득. */
	return (pci_dev_id(dev) - (pci_dev_id(pf) + pf->sriov->offset)) / /* NVMe: VF의 PCI ID에서 PF 기준 offset/stride로 VF 인덱스 계산. */
	       pf->sriov->stride;
}
EXPORT_SYMBOL_GPL(pci_iov_vf_id);

/**
 * pci_iov_get_pf_drvdata - Return the drvdata of a PF
 * @dev: VF pci_dev
 * @pf_driver: Device driver required to own the PF
 *
 * This must be called from a context that ensures that a VF driver is attached.
 * The value returned is invalid once the VF driver completes its remove()
 * callback.
 *
 * Locking is achieved by the driver core. A VF driver cannot be probed until
 * pci_enable_sriov() is called and pci_disable_sriov() does not return until
 * all VF drivers have completed their remove().
 *
 * The PF driver must call pci_disable_sriov() before it begins to destroy the
 * drvdata.
 */
void *pci_iov_get_pf_drvdata(struct pci_dev *dev, struct pci_driver *pf_driver)
{
	struct pci_dev *pf_dev; /* NVMe: PF 디바이스 포인터 선언. */

	if (!dev->is_virtfn) /* NVMe: VF가 아니면 drvdata 획득 불가. */
		return ERR_PTR(-EINVAL);
	pf_dev = dev->physfn; /* NVMe: VF가 가리키는 PF 포인터 사용. */
	if (pf_dev->driver != pf_driver) /* NVMe: PF가 기대한 드라이버에 바인딩되어 있는지 확인. */
		return ERR_PTR(-EINVAL);
	return pci_get_drvdata(pf_dev); /* NVMe: PF 드라이버의 private 데이터 반환. */
}
EXPORT_SYMBOL_GPL(pci_iov_get_pf_drvdata);

/*
 * Per SR-IOV spec sec 3.3.10 and 3.3.11, First VF Offset and VF Stride may
 * change when NumVFs changes.
 *
 * Update iov->offset and iov->stride when NumVFs is written.
 */
static inline void pci_iov_set_numvfs(struct pci_dev *dev, int nr_virtfn)
{
	struct pci_sriov *iov = dev->sriov;

	pci_write_config_word(dev, iov->pos + PCI_SRIOV_NUM_VF, nr_virtfn); /* NVMe: NumVFs 레지스터에 요청된 VF 수 기록. */
	pci_read_config_word(dev, iov->pos + PCI_SRIOV_VF_OFFSET, &iov->offset); /* NVMe: VF Offset 레지스터를 읽어 캐시. */
	pci_read_config_word(dev, iov->pos + PCI_SRIOV_VF_STRIDE, &iov->stride); /* NVMe: VF Stride 레지스터를 읽어 캐시. */
}

/*
 * The PF consumes one bus number.  NumVFs, First VF Offset, and VF Stride
 * determine how many additional bus numbers will be consumed by VFs.
 *
 * Iterate over all valid NumVFs, validate offset and stride, and calculate
 * the maximum number of bus numbers that could ever be required.
 */
/*
 * compute_max_vf_buses:
 *   가능한 최대 VF 수에 필요한 버스 개수를 예측한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static int compute_max_vf_buses(struct pci_dev *dev)
{
	struct pci_sriov *iov = dev->sriov;
	int nr_virtfn, busnr, rc = 0; /* NVMe: VF 개수, 버스 번호, 결과 변수 초기화. */

	for (nr_virtfn = iov->total_VFs; nr_virtfn; nr_virtfn--) { /* NVMe: 최대 VF 수부터 1까지 역순으로 시뮬레이션. */
		pci_iov_set_numvfs(dev, nr_virtfn); /* NVMe: NumVFs 레지스터 설정. */
		if (!iov->offset || (nr_virtfn > 1 && !iov->stride)) { /* NVMe: offset이 0이거나 다수 VF 시 stride가 0이면 오류. */
			rc = -EIO; /* NVMe: I/O 오류. */
			goto out; /* NVMe: 정리 루틴으로 이동. */
		}

		busnr = pci_iov_virtfn_bus(dev, nr_virtfn - 1); /* NVMe: 현재 nr_virtfn일 때 마지막 VF의 버스 번호 계산. */
		if (busnr > iov->max_VF_buses) /* NVMe: 최대 필요 버스 번호 갱신. */
			iov->max_VF_buses = busnr; /* NVMe: 최대 VF 버스 번호 저장. */
	}

out: /* NVMe: 루프 종료 후 정리 레이블. */
	pci_iov_set_numvfs(dev, 0); /* NVMe: NumVFs 0. */
	return rc; /* NVMe: 오류 반환. */
}

static struct pci_bus *virtfn_add_bus(struct pci_bus *bus, int busnr)
{
	struct pci_bus *child; /* NVMe: 하위 버스 포인터 선언. */

	if (bus->number == busnr) /* NVMe: 현재 버스가 요청 번호와 같으면 그대로 사용. */
		return bus; /* NVMe: 기존 버스 반환. */

	child = pci_find_bus(pci_domain_nr(bus), busnr); /* NVMe: 해당 도메인/버스가 이미 존재하는지 검색. */
	if (child) /* NVMe: 기존 버스가 있으면 재사용. */
		return child; /* NVMe: 기존 버스 반환. */

	child = pci_add_new_bus(bus, NULL, busnr); /* NVMe: 새 PCI 버스를 생성해 VF용 버스로 추가. */
	if (!child) /* NVMe: 버스 생성 실패 검사. */
		return NULL; /* NVMe: NULL 반환. */

	pci_bus_insert_busn_res(child, busnr, busnr); /* NVMe: 새 버스의 bus 번호 리소스를 등록. */

	return child; /* NVMe: 기존 버스 반환. */
}

/*
 * virtfn_remove_bus:
 *   VF 버스를 제거한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static void virtfn_remove_bus(struct pci_bus *physbus, struct pci_bus *virtbus)
{
	if (physbus != virtbus && list_empty(&virtbus->devices)) /* NVMe: 물리 버스와 다르고 장치가 비었을 때만 제거. */
		pci_remove_bus(virtbus); /* NVMe: 빈 VF 버스 제거. */
}

/*
 * pci_iov_resource_size:
 *   지정한 리소스 번호의 VF BAR 크기를 반환한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
resource_size_t pci_iov_resource_size(struct pci_dev *dev, int resno)
{
	if (!dev->is_physfn) /* NVMe: PF가 아니면. */
		return 0; /* NVMe: 후보 없음. */

	return dev->sriov->barsz[pci_resource_num_to_vf_bar(resno)]; /* NVMe: 리소스 번호를 VF BAR 인덱스로 변환해 크기 반환. */
}

/*
 * pci_iov_resource_set_size:
 *   VF BAR의 소프트웨어 크기를 갱신한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
void pci_iov_resource_set_size(struct pci_dev *dev, int resno, int size)
{
	if (!pci_resource_is_iov(resno)) { /* NVMe: IOV 리소스가 아니면. */
		pci_warn(dev, "%s is not an IOV resource\n", /* NVMe: 경고 메시지 출력. */
			 pci_resource_name(dev, resno)); /* NVMe: 리소스 이름 포함. */
		return; /* NVMe: 조기 반환. */
	}

	resno = pci_resource_num_to_vf_bar(resno); /* NVMe: VF BAR 인덱스로 변환. */
	dev->sriov->barsz[resno] = pci_rebar_size_to_bytes(size); /* NVMe: spec encoding 크기를 바이트로 변환해 저장. */
}

/*
 * pci_iov_is_memory_decoding_enabled:
 *   SR-IOV Memory Space Enable 비트를 읽는다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
bool pci_iov_is_memory_decoding_enabled(struct pci_dev *dev)
{
	u16 cmd; /* NVMe: control. */

	pci_read_config_word(dev, dev->sriov->pos + PCI_SRIOV_CTRL, &cmd); /* NVMe: SR-IOV Control 레지스터 읽기. */

	return cmd & PCI_SRIOV_CTRL_MSE; /* NVMe: Memory Space Enable 비트 반환. */
}

/*
 * pci_read_vf_config_common:
 *   VF0의 공통 설정값을 읽어 PF sriov 구조체에 저장한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static void pci_read_vf_config_common(struct pci_dev *virtfn)
{
	struct pci_dev *physfn = virtfn->physfn; /* NVMe: VF의 PF 포인터 획득. */

	/*
	 * Some config registers are the same across all associated VFs.
	 * Read them once from VF0 so we can skip reading them from the
	 * other VFs.
	 *
	 * PCIe r4.0, sec 9.3.4.1, technically doesn't require all VFs to
	 * have the same Revision ID and Subsystem ID, but we assume they
	 * do.
	 */
	pci_read_config_dword(virtfn, PCI_CLASS_REVISION, /* NVMe: VF0 클래스/리비전 ID 읽기. */
			      &physfn->sriov->class); /* NVMe: PF sriov 구조체에 클래스 저장. */
	pci_read_config_byte(virtfn, PCI_HEADER_TYPE, /* NVMe: 헤더 타입 읽기. */
			     &physfn->sriov->hdr_type); /* NVMe: PF sriov 구조체에 헤더 타입 저장. */
	pci_read_config_word(virtfn, PCI_SUBSYSTEM_VENDOR_ID, /* NVMe: 서브시스템 벤더 ID 읽기. */
			     &physfn->sriov->subsystem_vendor); /* NVMe: PF sriov 구조체에 서브시스템 벤더 저장. */
	pci_read_config_word(virtfn, PCI_SUBSYSTEM_ID, /* NVMe: 서브시스템 디바이스 ID 읽기. */
			     &physfn->sriov->subsystem_device); /* NVMe: PF sriov 구조체에 서브시스템 디바이스 저장. */
}

/*
 * pci_iov_sysfs_link:
 *   PF와 VF 간 sysfs 심볼릭 링크를 생성한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
int pci_iov_sysfs_link(struct pci_dev *dev,
		struct pci_dev *virtfn, int id)
{
	char buf[VIRTFN_ID_LEN]; /* NVMe: 링크 이름 버퍼. */
	int rc; /* NVMe: 결과. */

	sprintf(buf, "virtfn%u", id); /* NVMe: 링크 이름 생성. */
	rc = sysfs_create_link(&dev->dev.kobj, &virtfn->dev.kobj, buf); /* NVMe: PF 디렉터리에 VF로의 심볼릭 링크 생성. */
	if (rc) /* NVMe: 계산 실패. */
		goto failed; /* NVMe: 정리. */
	rc = sysfs_create_link(&virtfn->dev.kobj, &dev->dev.kobj, "physfn"); /* NVMe: VF 디렉터리에 PF로의 심볼릭 링크 생성. */
	if (rc) /* NVMe: 계산 실패. */
		goto failed1; /* NVMe: VF 제거 경로. */

	kobject_uevent(&virtfn->dev.kobj, KOBJ_CHANGE); /* NVMe: VF 디바이스 변경 uevent 발생. */

	return 0; /* NVMe: 후보 없음. */

failed1: /* NVMe: VF 제거 레이블. */
	sysfs_remove_link(&dev->dev.kobj, buf); /* NVMe: PF에서 VF로의 링크 제거. */
failed: /* NVMe: 초기화 실패 레이블. */
	return rc; /* NVMe: 오류 반환. */
}

#ifdef CONFIG_PCI_MSI /* NVMe: MSI 설정 시 total_msix 속성 추가. */
/*
 * sriov_vf_total_msix_show:
 *   PF가 VF에 할당 가능한 총 MSI-X 벡터 수를 sysfs에 노출한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static ssize_t sriov_vf_total_msix_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: pci_dev 변환. */
	u32 vf_total_msix = 0; /* NVMe: MSI-X 총계 초기화. */

	device_lock(dev); /* NVMe: 디바이스 락 획득. */
	if (!pdev->driver || !pdev->driver->sriov_get_vf_total_msix) /* NVMe: PF 드라이버가 콜백을 제공하지 않으면 0 반환. */
		goto unlock; /* NVMe: 락 해제 후 반환. */

	vf_total_msix = pdev->driver->sriov_get_vf_total_msix(pdev); /* NVMe: PF 드라이버가 VF당 최대 MSI-X 수 반환. */
unlock: /* NVMe: 락 해제 레이블. */
	device_unlock(dev); /* NVMe: 디바이스 락 해제. */
	return sysfs_emit(buf, "%u\n", vf_total_msix); /* NVMe: sysfs 버퍼에 값 기록. */
}
static DEVICE_ATTR_RO(sriov_vf_total_msix); /* NVMe: 읽기 전용 속성 정의. */

/*
 * sriov_vf_msix_count_store:
 *   VF당 MSI-X 벡터 개수를 변경하는 sysfs 쓰기 핸들러다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static ssize_t sriov_vf_msix_count_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct pci_dev *vf_dev = to_pci_dev(dev); /* NVMe: sysfs device에서 VF pci_dev 변환. */
	struct pci_dev *pdev = pci_physfn(vf_dev); /* NVMe: VF의 PF 획득. */
	int val, ret = 0; /* NVMe: 입력값과 결과 변수. */

	if (kstrtoint(buf, 0, &val) < 0) /* NVMe: 문자열을 정수로 변환. */
		return -EINVAL; /* NVMe: 잘못된 인자. */

	if (val < 0) /* NVMe: 음수는 유효하지 않음. */
		return -EINVAL; /* NVMe: 잘못된 인자. */

	device_lock(&pdev->dev); /* NVMe: PF 디바이스 락. */
	if (!pdev->driver || !pdev->driver->sriov_set_msix_vec_count) { /* NVMe: PF 드라이버 콜백 존재 여부 확인. */
		ret = -EOPNOTSUPP; /* NVMe: 지원하지 않음. */
		goto err_pdev; /* NVMe: PF 락 해제 경로. */
	}

	device_lock(&vf_dev->dev); /* NVMe: VF 디바이스 락. */
	if (vf_dev->driver) { /* NVMe: VF에 이미 드라이버가 바인딩되어 있으면 변경 불가. */
		/*
		 * A driver is already attached to this VF and has configured
		 * itself based on the current MSI-X vector count. Changing
		 * the vector size could mess up the driver, so block it.
		 */
		ret = -EBUSY; /* NVMe: 바쁨. */
		goto err_dev; /* NVMe: VF 락 해제 경로. */
	}

	ret = pdev->driver->sriov_set_msix_vec_count(vf_dev, val); /* NVMe: PF 드라이버에게 VF MSI-X 개수 변경 요청. */

err_dev: /* NVMe: VF 락 해제 레이블. */
	device_unlock(&vf_dev->dev); /* NVMe: VF 락 해제. */
err_pdev: /* NVMe: PF 락 해제 레이블. */
	device_unlock(&pdev->dev); /* NVMe: 락 해제. */
	return ret ? : count; /* NVMe: 오류 시 ret, 아니면 기록된 바이트 수 반환. */
}
static DEVICE_ATTR_WO(sriov_vf_msix_count); /* NVMe: 쓰기 전용 속성 정의. */
#endif /* NVMe: CONFIG_PCI_MSI 블록 종료. */

static struct attribute *sriov_vf_dev_attrs[] = { /* NVMe: VF sysfs 속성 테이블. */
#ifdef CONFIG_PCI_MSI /* NVMe: MSI 설정 시 total_msix 속성 추가. */
	&dev_attr_sriov_vf_msix_count.attr, /* NVMe: VF당 MSI-X 개수 속성 추가. */
#endif /* NVMe: CONFIG_PCI_MSI 블록 종료. */
	NULL, /* NVMe: 테이블 끝. */
};

static umode_t sriov_vf_attrs_are_visible(struct kobject *kobj,
					  struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj); /* NVMe: kobject에서 device 변환. */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: pci_dev 변환. */

	if (!pdev->is_virtfn) /* NVMe: VF가 아니면 속성 비가시. */
		return 0; /* NVMe: 후보 없음. */

	return a->mode; /* NVMe: 속성 모드 반환. */
}

const struct attribute_group sriov_vf_dev_attr_group = { /* NVMe: VF sysfs 속성 그룹. */
	.attrs = sriov_vf_dev_attrs, /* NVMe: 속성 테이블 연결. */
	.is_visible = sriov_vf_attrs_are_visible, /* NVMe: 가시성 콜백 연결. */
};

static struct pci_dev *pci_iov_scan_device(struct pci_dev *dev, int id,
					   struct pci_bus *bus)
{
	struct pci_sriov *iov = dev->sriov;
	struct pci_dev *virtfn; /* NVMe: 제거할 VF 포인터. */
	int rc; /* NVMe: 결과. */

	virtfn = pci_alloc_dev(bus); /* NVMe: 새 pci_dev 메모리 할당. */
	if (!virtfn) /* NVMe: VF가 이미 없으면 종료. */
		return ERR_PTR(-ENOMEM); /* NVMe: 메모리 부족 오류 반환. */

	virtfn->devfn = pci_iov_virtfn_devfn(dev, id); /* NVMe: VF devfn 설정. */
	virtfn->vendor = dev->vendor; /* NVMe: VF 벤더 ID는 PF와 동일. */
	virtfn->device = iov->vf_device; /* NVMe: VF 디바이스 ID 설정. */
	virtfn->is_virtfn = 1; /* NVMe: VF 플래그 설정. */
	virtfn->physfn = pci_dev_get(dev); /* NVMe: PF에 대한 참조 증가. */
	virtfn->no_command_memory = 1; /* NVMe: VF는 커맨드 레지스터 메모리 제어 비활성. */

	if (id == 0) /* NVMe: VF0일 때 공통 설정 읽기. */
		pci_read_vf_config_common(virtfn); /* NVMe: VF0의 클래스/헤더/서브시스템 정보 캐시. */

	rc = pci_setup_device(virtfn); /* NVMe: VF pci_dev 초기 설정. */
	if (rc) { /* NVMe: 계산 실패. */
		pci_dev_put(dev); /* NVMe: PF 참조 감소. */
		pci_bus_put(virtfn->bus); /* NVMe: 버스 참조 감소. */
		kfree(virtfn); /* NVMe: VF 구조체 해제. */
		return ERR_PTR(rc); /* NVMe: 오류 포인터 반환. */
	}

	return virtfn; /* NVMe: 초기화된 VF 반환. */
}

/*
 * pci_iov_add_virtfn:
 *   지정 ID의 VF를 실제 PCI 계층에 추가한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
int pci_iov_add_virtfn(struct pci_dev *dev, int id)
{
	struct pci_bus *bus; /* NVMe: VF 버스 포인터. */
	struct pci_dev *virtfn; /* NVMe: 제거할 VF 포인터. */
	struct resource *res; /* NVMe: 리소스 포인터. */
	int rc, i; /* NVMe: 결과 및 반복 변수. */
	u64 size; /* NVMe: BAR 크기 변수. */

	bus = virtfn_add_bus(dev->bus, pci_iov_virtfn_bus(dev, id)); /* NVMe: VF 버스 생성/검색. */
	if (!bus) { /* NVMe: 버스 없음 실패. */
		rc = -ENOMEM; /* NVMe: 메모리 부족 오류. */
		goto failed; /* NVMe: 정리. */
	}

	virtfn = pci_iov_scan_device(dev, id, bus); /* NVMe: VF pci_dev 생성. */
	if (IS_ERR(virtfn)) { /* NVMe: 생성 오류 검사. */
		rc = PTR_ERR(virtfn); /* NVMe: 오류 코드 추출. */
		goto failed0; /* NVMe: 버스 제거 경로. */
	}

	virtfn->dev.parent = dev->dev.parent; /* NVMe: VF의 부모 device를 PF와 동일하게 설정. */
	virtfn->multifunction = 0; /* NVMe: VF는 멀티펑션 아님. */

	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) { /* NVMe: VF BAR 복원 루프. */
		int idx = pci_resource_num_from_vf_bar(i); /* NVMe: VF BAR 인덱스 변환. */

		res = &dev->resource[idx]; /* NVMe: PF 리소스 참조. */
		if (!res->parent) /* NVMe: 부모 리소스가 없으면 스킵. */
			continue; /* NVMe: 다음 장치. */
		virtfn->resource[i].name = pci_name(virtfn); /* NVMe: VF 리소스 이름 설정. */
		virtfn->resource[i].flags = res->flags; /* NVMe: PF 리소스 플래그 복사. */
		size = pci_iov_resource_size(dev, idx); /* NVMe: VF BAR 크기 획득. */
		resource_set_range(&virtfn->resource[i], /* NVMe: VF BAR의 시작/끝 주소 설정. */
				   res->start + size * id, size); /* NVMe: PF IOV 영역 내 VF id에 해당하는 오프셋 할당. */
		rc = request_resource(res, &virtfn->resource[i]); /* NVMe: 리소스 트리에 VF BAR 등록. */
		BUG_ON(rc); /* NVMe: 등록 실패 시 패닉. */
	}

	pci_device_add(virtfn, virtfn->bus); /* NVMe: VF를 PCI 코어에 등록. */
	rc = pci_iov_sysfs_link(dev, virtfn, id); /* NVMe: PF-VF sysfs 링크 생성. */
	if (rc) /* NVMe: 계산 실패. */
		goto failed1; /* NVMe: VF 제거 경로. */

	pci_bus_add_device(virtfn); /* NVMe: VF를 버스 장치 목록에 추가. */

	return 0; /* NVMe: 후보 없음. */

failed1: /* NVMe: VF 제거 레이블. */
	pci_stop_and_remove_bus_device(virtfn); /* NVMe: VF 디바이스 중지 및 제거. */
	pci_dev_put(dev); /* NVMe: PF 참조 감소. */
failed0: /* NVMe: 버스 제거 레이블. */
	virtfn_remove_bus(dev->bus, bus); /* NVMe: 임시 버스 정리. */
failed: /* NVMe: 초기화 실패 레이블. */

	return rc; /* NVMe: 오류 반환. */
}

/*
 * pci_iov_remove_virtfn:
 *   지정 ID의 VF를 PCI 계층에서 제거한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
void pci_iov_remove_virtfn(struct pci_dev *dev, int id)
{
	char buf[VIRTFN_ID_LEN]; /* NVMe: 링크 이름 버퍼. */
	struct pci_dev *virtfn; /* NVMe: 제거할 VF 포인터. */

	virtfn = pci_get_domain_bus_and_slot(pci_domain_nr(dev->bus), /* NVMe: 도메인/버스/devfn으로 VF pci_dev 검색. */
					     pci_iov_virtfn_bus(dev, id), /* NVMe: VF 버스 번호. */
					     pci_iov_virtfn_devfn(dev, id)); /* NVMe: VF devfn. */
	if (!virtfn) /* NVMe: VF가 이미 없으면 종료. */
		return; /* NVMe: 조기 반환. */

	sprintf(buf, "virtfn%u", id); /* NVMe: 링크 이름 생성. */
	sysfs_remove_link(&dev->dev.kobj, buf); /* NVMe: PF에서 VF로의 링크 제거. */
	/*
	 * pci_stop_dev() could have been called for this virtfn already,
	 * so the directory for the virtfn may have been removed before.
	 * Double check to avoid spurious sysfs warnings.
	 */
	if (virtfn->dev.kobj.sd) /* NVMe: VF sysfs 디렉터리가 남아 있으면. */
		sysfs_remove_link(&virtfn->dev.kobj, "physfn"); /* NVMe: VF에서 PF로의 링크 제거. */

	pci_stop_and_remove_bus_device(virtfn); /* NVMe: VF 디바이스 중지 및 제거. */
	virtfn_remove_bus(dev->bus, virtfn->bus); /* NVMe: 빈 VF 버스 제거. */

	/* balance pci_get_domain_bus_and_slot() */
	pci_dev_put(virtfn); /* NVMe: pci_get_domain_bus_and_slot 참조 감소. */
	pci_dev_put(dev); /* NVMe: PF 참조 감소. */
}

/*
 * sriov_totalvfs_show:
 *   totalvfs sysfs 속성의 읽기 핸들러다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static ssize_t sriov_totalvfs_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: pci_dev 변환. */

	return sysfs_emit(buf, "%u\n", pci_sriov_get_totalvfs(pdev)); /* NVMe: totalvfs 값 출력. */
}

/*
 * sriov_numvfs_show:
 *   numvfs sysfs 속성의 읽기 핸들러다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static ssize_t sriov_numvfs_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: pci_dev 변환. */
	u16 num_vfs; /* NVMe: 요청 VF 수 변수. */

	/* Serialize vs sriov_numvfs_store() so readers see valid num_VFs */
	device_lock(&pdev->dev); /* NVMe: PF 디바이스 락. */
	num_vfs = pdev->sriov->num_VFs; /* NVMe: 현재 VF 수 읽기. */
	device_unlock(&pdev->dev); /* NVMe: 락 해제. */

	return sysfs_emit(buf, "%u\n", num_vfs); /* NVMe: 값 출력. */
}

/*
 * num_vfs > 0; number of VFs to enable
 * num_vfs = 0; disable all VFs
 *
 * Note: SRIOV spec does not allow partial VF
 *	 disable, so it's all or none.
 */
/*
 * sriov_numvfs_store:
 *   numvfs sysfs 속성의 쓰기 핸들러로 VF 활성화/비활성화를 제어한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static ssize_t sriov_numvfs_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: pci_dev 변환. */
	int ret = 0; /* NVMe: 결과 초기화. */
	u16 num_vfs; /* NVMe: 요청 VF 수 변수. */

	if (kstrtou16(buf, 0, &num_vfs) < 0) /* NVMe: 입력 문자열을 u16로 변환. */
		return -EINVAL; /* NVMe: 잘못된 인자. */

	if (num_vfs > pci_sriov_get_totalvfs(pdev)) /* NVMe: 요청값이 totalvfs 초과 시. */
		return -ERANGE; /* NVMe: 범위 초과 오류. */

	device_lock(&pdev->dev); /* NVMe: PF 디바이스 락. */

	if (num_vfs == pdev->sriov->num_VFs) /* NVMe: 변경 없으면. */
		goto exit; /* NVMe: 종료. */

	/* is PF driver loaded */
	if (!pdev->driver) { /* NVMe: PF 드라이버 미바인딩. */
		pci_info(pdev, "no driver bound to device; cannot configure SR-IOV\n"); /* NVMe: 안내 메시지. */
		ret = -ENOENT; /* NVMe: 드라이버 없음 오류. */
		goto exit; /* NVMe: 종료. */
	}

	/* is PF driver loaded w/callback */
	if (!pdev->driver->sriov_configure) { /* NVMe: PF 드라이버가 SR-IOV 콜백 미제공. */
		pci_info(pdev, "driver does not support SR-IOV configuration via sysfs\n"); /* NVMe: 안내 메시지. */
		ret = -ENOENT; /* NVMe: 드라이버 없음 오류. */
		goto exit; /* NVMe: 종료. */
	}

	if (num_vfs == 0) { /* NVMe: 비활성화 요청. */
		/* disable VFs */
		pci_lock_rescan_remove(); /* NVMe: 버스 재스캔/제거 락. */
		ret = pdev->driver->sriov_configure(pdev, 0); /* NVMe: PF 드라이버에게 SR-IOV 비활성화 요청. */
		pci_unlock_rescan_remove(); /* NVMe: 락 해제. */
		goto exit; /* NVMe: 종료. */
	}

	/* enable VFs */
	if (pdev->sriov->num_VFs) { /* NVMe: 이미 VF가 활성화되어 있으면. */
		pci_warn(pdev, "%d VFs already enabled. Disable before enabling %d VFs\n", /* NVMe: 경고. */
			 pdev->sriov->num_VFs, num_vfs); /* NVMe: 인자. */
		ret = -EBUSY; /* NVMe: 바쁨. */
		goto exit; /* NVMe: 종료. */
	}

	pci_lock_rescan_remove(); /* NVMe: 버스 재스캔/제거 락. */
	ret = pdev->driver->sriov_configure(pdev, num_vfs); /* NVMe: PF 드라이버에게 활성화 요청. */
	pci_unlock_rescan_remove(); /* NVMe: 락 해제. */
	if (ret < 0) /* NVMe: 오류 반환. */
		goto exit; /* NVMe: 종료. */

	if (ret != num_vfs) /* NVMe: 요청 개수보다 적게 활성화됨. */
		pci_warn(pdev, "%d VFs requested; only %d enabled\n", /* NVMe: 경고. */
			 num_vfs, ret); /* NVMe: 인자. */

exit: /* NVMe: 종료 레이블. */
	device_unlock(&pdev->dev); /* NVMe: 락 해제. */

	if (ret < 0) /* NVMe: 오류 반환. */
		return ret; /* NVMe: 오류 코드 반환. */

	return count; /* NVMe: 기록된 바이트 수 반환. */
}

/*
 * sriov_offset_show:
 *   sriov_offset sysfs 읽기 핸들러다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static ssize_t sriov_offset_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: pci_dev 변환. */

	return sysfs_emit(buf, "%u\n", pdev->sriov->offset); /* NVMe: VF offset 출력. */
}

/*
 * sriov_stride_show:
 *   sriov_stride sysfs 읽기 핸들러다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static ssize_t sriov_stride_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: pci_dev 변환. */

	return sysfs_emit(buf, "%u\n", pdev->sriov->stride); /* NVMe: VF stride 출력. */
}

/*
 * sriov_vf_device_show:
 *   VF device ID sysfs 읽기 핸들러다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static ssize_t sriov_vf_device_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: pci_dev 변환. */

	return sysfs_emit(buf, "%x\n", pdev->sriov->vf_device); /* NVMe: VF device ID 출력. */
}

/*
 * sriov_drivers_autoprobe_show:
 *   drivers_autoprobe sysfs 읽기 핸들러다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static ssize_t sriov_drivers_autoprobe_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: pci_dev 변환. */

	return sysfs_emit(buf, "%u\n", pdev->sriov->drivers_autoprobe); /* NVMe: drivers_autoprobe 출력. */
}

/*
 * sriov_drivers_autoprobe_store:
 *   drivers_autoprobe sysfs 쓰기 핸들러다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static ssize_t sriov_drivers_autoprobe_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: pci_dev 변환. */
	bool drivers_autoprobe; /* NVMe: 자동 탐색 플래그. */

	if (kstrtobool(buf, &drivers_autoprobe) < 0) /* NVMe: 문자열을 bool로 변환. */
		return -EINVAL; /* NVMe: 잘못된 인자. */

	pdev->sriov->drivers_autoprobe = drivers_autoprobe; /* NVMe: 자동 탐색 설정 저장. */

	return count; /* NVMe: 기록된 바이트 수 반환. */
}

static DEVICE_ATTR_RO(sriov_totalvfs); /* NVMe: 속성 정의. */
static DEVICE_ATTR_RW(sriov_numvfs); /* NVMe: 속성 정의. */
static DEVICE_ATTR_RO(sriov_offset); /* NVMe: 속성 정의. */
static DEVICE_ATTR_RO(sriov_stride); /* NVMe: 속성 정의. */
static DEVICE_ATTR_RO(sriov_vf_device); /* NVMe: 속성 정의. */
static DEVICE_ATTR_RW(sriov_drivers_autoprobe); /* NVMe: 속성 정의. */

static struct attribute *sriov_pf_dev_attrs[] = { /* NVMe: PF sysfs 속성 테이블. */
	&dev_attr_sriov_totalvfs.attr, /* NVMe: totalvfs 속성. */
	&dev_attr_sriov_numvfs.attr, /* NVMe: numvfs 속성. */
	&dev_attr_sriov_offset.attr, /* NVMe: offset 속성. */
	&dev_attr_sriov_stride.attr, /* NVMe: stride 속성. */
	&dev_attr_sriov_vf_device.attr, /* NVMe: vf_device 속성. */
	&dev_attr_sriov_drivers_autoprobe.attr, /* NVMe: drivers_autoprobe 속성. */
#ifdef CONFIG_PCI_MSI /* NVMe: MSI 설정 시 total_msix 속성 추가. */
	&dev_attr_sriov_vf_total_msix.attr, /* NVMe: vf_total_msix 속성. */
#endif /* NVMe: CONFIG_PCI_MSI 블록 종료. */
	NULL, /* NVMe: 테이블 끝. */
};

static umode_t sriov_pf_attrs_are_visible(struct kobject *kobj,
					  struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj); /* NVMe: kobject에서 device 변환. */

	if (!dev_is_pf(dev)) /* NVMe: PF가 아니면 속성 비가시. */
		return 0; /* NVMe: 후보 없음. */

	return a->mode; /* NVMe: 속성 모드 반환. */
}

const struct attribute_group sriov_pf_dev_attr_group = { /* NVMe: PF sysfs 속성 그룹. */
	.attrs = sriov_pf_dev_attrs, /* NVMe: PF 속성 테이블 연결. */
	.is_visible = sriov_pf_attrs_are_visible, /* NVMe: 가시성 콜백 연결. */
};

int __weak pcibios_sriov_enable(struct pci_dev *pdev, u16 num_vfs)
{
	return 0; /* NVMe: 후보 없음. */
}

int __weak pcibios_sriov_disable(struct pci_dev *pdev)
{
	return 0; /* NVMe: 후보 없음. */
}

/*
 * sriov_add_vfs:
 *   요청된 수만큼 VF를 생성한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static int sriov_add_vfs(struct pci_dev *dev, u16 num_vfs)
{
	unsigned int i; /* NVMe: 반복 변수. */
	int rc; /* NVMe: 결과. */

	if (dev->no_vf_scan) /* NVMe: VF 스캔 비활성화 시. */
		return 0; /* NVMe: 후보 없음. */

	for (i = 0; i < num_vfs; i++) { /* NVMe: num_vfs만큼 VF 추가 루프. */
		rc = pci_iov_add_virtfn(dev, i); /* NVMe: 각 VF 추가. */
		if (rc) /* NVMe: 계산 실패. */
			goto failed; /* NVMe: 정리. */
	}
	return 0; /* NVMe: 후보 없음. */
failed: /* NVMe: 초기화 실패 레이블. */
	while (i--) /* NVMe: 이까지 추가된 VF를 역순으로 제거. */
		pci_iov_remove_virtfn(dev, i); /* NVMe: VF 제거. */

	return rc; /* NVMe: 오류 반환. */
}

/*
 * sriov_enable:
 *   SR-IOV를 활성화하고 VF들을 노출한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static int sriov_enable(struct pci_dev *dev, int nr_virtfn)
{
	int rc; /* NVMe: 결과. */
	int i; /* NVMe: 반복 변수. */
	int nres; /* NVMe: 할당된 리소스 개수. */
	u16 initial; /* NVMe: Initial VF 수. */
	struct resource *res; /* NVMe: 리소스 포인터. */
	struct pci_dev *pdev; /* NVMe: 버스상 다른 PF 포인터. */
	struct pci_sriov *iov = dev->sriov;
	int bars = 0; /* NVMe: 활성화할 BAR 비트마스크. */
	int bus; /* NVMe: 마지막 VF 버스 번호. */

	if (!nr_virtfn) /* NVMe: 0이면 활성화할 것 없음. */
		return 0; /* NVMe: 후보 없음. */

	if (iov->num_VFs) /* NVMe: VF가 남아 있으면. */
		return -EINVAL; /* NVMe: 잘못된 인자. */

	pci_read_config_word(dev, iov->pos + PCI_SRIOV_INITIAL_VF, &initial); /* NVMe: Initial VF 수 읽기. */
	if (initial > iov->total_VFs || /* NVMe: initial이 total보다 크거나. */
	    (!(iov->cap & PCI_SRIOV_CAP_VFM) && (initial != iov->total_VFs))) /* NVMe: VFM 미지원 시 initial != total이면 오류. */
		return -EIO; /* NVMe: I/O 오류. */

	if (nr_virtfn < 0 || nr_virtfn > iov->total_VFs || /* NVMe: 요청값이 0~total 범위를 벗어나거나. */
	    (!(iov->cap & PCI_SRIOV_CAP_VFM) && (nr_virtfn > initial))) /* NVMe: VFM 미지원 시 initial 초과면 오류. */
		return -EINVAL; /* NVMe: 잘못된 인자. */

	nres = 0; /* NVMe: 유효 리소스 카운트 초기화. */
	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) { /* NVMe: VF BAR 복원 루프. */
		int idx = pci_resource_num_from_vf_bar(i); /* NVMe: VF BAR 인덱스 변환. */
		resource_size_t vf_bar_sz = pci_iov_resource_size(dev, idx); /* NVMe: VF BAR 크기 획득. */

		bars |= (1 << idx); /* NVMe: 해당 BAR를 활성화 비트마스크에 추가. */
		res = &dev->resource[idx]; /* NVMe: PF 리소스 참조. */
		if (vf_bar_sz * nr_virtfn > resource_size(res)) /* NVMe: 전체 VF BAR가 예약 영역보다 크면 스킵. */
			continue; /* NVMe: 다음 장치. */
		if (res->parent) /* NVMe: 리소스가 할당되었으면. */
			nres++; /* NVMe: 유효 리소스 카운트 증가. */
	}
	if (nres != iov->nres) { /* NVMe: 할당된 리소스 수가 초기화 때와 다름. */
		pci_err(dev, "not enough MMIO resources for SR-IOV\n"); /* NVMe: MMIO 부족 오류 메시지. */
		return -ENOMEM; /* NVMe: 메모리 부족. */
	}

	bus = pci_iov_virtfn_bus(dev, nr_virtfn - 1); /* NVMe: 마지막 VF 버스 번호. */
	if (bus > dev->bus->busn_res.end) { /* NVMe: 버스 번호가 부모 버스 범위를 초과. */
		pci_err(dev, "can't enable %d VFs (bus %02x out of range of %pR)\n", /* NVMe: 오류 메시지. */
			nr_virtfn, bus, &dev->bus->busn_res); /* NVMe: 인자. */
		return -ENOMEM; /* NVMe: 메모리 부족. */
	}

	if (pci_enable_resources(dev, bars)) { /* NVMe: PF의 IOV BAR 리소스 활성화. */
		pci_err(dev, "SR-IOV: IOV BARS not allocated\n"); /* NVMe: 오류 메시지. */
		return -ENOMEM; /* NVMe: 메모리 부족. */
	}

	if (iov->link != dev->devfn) { /* NVMe: dep_link 제거. */
		pdev = pci_get_slot(dev->bus, iov->link); /* NVMe: 의존 PF 디바이스 획득. */
		if (!pdev) /* NVMe: 의존 PF 없음. */
			return -ENODEV; /* NVMe: 장치 없음. */

		if (!pdev->is_physfn) { /* NVMe: 의존 대상이 PF가 아니면. */
			pci_dev_put(pdev); /* NVMe: 의존 PF 참조 감소. */
			return -ENOSYS; /* NVMe: PF가 아니면 불가. */
		}

		rc = sysfs_create_link(&dev->dev.kobj, /* NVMe: 의존 PF와의 sysfs 링크 생성. */
					&pdev->dev.kobj, "dep_link"); /* NVMe: dep_link 이름. */
		pci_dev_put(pdev); /* NVMe: 의존 PF 참조 감소. */
		if (rc) /* NVMe: 계산 실패. */
			return rc; /* NVMe: 오류 반환. */
	}

	iov->initial_VFs = initial; /* NVMe: Initial VF 수 저장. */
	if (nr_virtfn < initial) /* NVMe: 요청한 수가 initial보다 작으면. */
		initial = nr_virtfn; /* NVMe: initial을 요청값으로 조정. */

	rc = pcibios_sriov_enable(dev, initial); /* NVMe: 플랫폼별 활성화 후킹. */
	if (rc) { /* NVMe: 계산 실패. */
		pci_err(dev, "failure %d from pcibios_sriov_enable()\n", rc); /* NVMe: 오류 메시지. */
		goto err_pcibios; /* NVMe: 활성화 롤백. */
	}

	pci_iov_set_numvfs(dev, nr_virtfn); /* NVMe: NumVFs 레지스터 설정. */
	iov->ctrl |= PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE; /* NVMe: VF Enable 및 Memory Space Enable 설정. */
	pci_cfg_access_lock(dev); /* NVMe: 락. */
	pci_write_config_word(dev, iov->pos + PCI_SRIOV_CTRL, iov->ctrl); /* NVMe: control 복원. */
	msleep(100); /* NVMe: 안정화 대기. */
	pci_cfg_access_unlock(dev); /* NVMe: 락 해제. */

	rc = sriov_add_vfs(dev, initial); /* NVMe: initial 개수만큼 VF 생성. */
	if (rc) /* NVMe: 계산 실패. */
		goto err_pcibios; /* NVMe: 활성화 롤백. */

	kobject_uevent(&dev->dev.kobj, KOBJ_CHANGE); /* NVMe: PF 변경 uevent. */
	iov->num_VFs = nr_virtfn; /* NVMe: 활성화된 VF 수 기록. */

	return 0; /* NVMe: 후보 없음. */

err_pcibios: /* NVMe: 롤백 레이블. */
	iov->ctrl &= ~(PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE); /* NVMe: VFE/MSE 클리어. */
	pci_cfg_access_lock(dev); /* NVMe: 락. */
	pci_write_config_word(dev, iov->pos + PCI_SRIOV_CTRL, iov->ctrl); /* NVMe: control 복원. */
	ssleep(1); /* NVMe: 하드웨어 비활성화 대기. */
	pci_cfg_access_unlock(dev); /* NVMe: 락 해제. */

	pcibios_sriov_disable(dev); /* NVMe: 플랫폼별 비활성화. */

	if (iov->link != dev->devfn) /* NVMe: dep_link 제거. */
		sysfs_remove_link(&dev->dev.kobj, "dep_link"); /* NVMe: dep_link 제거. */

	pci_iov_set_numvfs(dev, 0); /* NVMe: NumVFs 0. */
	return rc; /* NVMe: 오류 반환. */
}

/*
 * sriov_del_vfs:
 *   활성화된 모든 VF를 제거한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static void sriov_del_vfs(struct pci_dev *dev)
{
	struct pci_sriov *iov = dev->sriov;
	int i; /* NVMe: 반복 변수. */

	for (i = 0; i < iov->num_VFs; i++) /* NVMe: 활성화된 VF 모두 제거. */
		pci_iov_remove_virtfn(dev, i); /* NVMe: VF 제거. */
}

/*
 * sriov_disable:
 *   SR-IOV를 비활성화한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static void sriov_disable(struct pci_dev *dev)
{
	struct pci_sriov *iov = dev->sriov;

	if (!iov->num_VFs) /* NVMe: 활성화된 VF가 없으면. */
		return; /* NVMe: 조기 반환. */

	sriov_del_vfs(dev); /* NVMe: 모든 VF 제거. */
	iov->ctrl &= ~(PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE); /* NVMe: VFE/MSE 클리어. */
	pci_cfg_access_lock(dev); /* NVMe: 락. */
	pci_write_config_word(dev, iov->pos + PCI_SRIOV_CTRL, iov->ctrl); /* NVMe: control 복원. */
	ssleep(1); /* NVMe: 하드웨어 비활성화 대기. */
	pci_cfg_access_unlock(dev); /* NVMe: 락 해제. */

	pcibios_sriov_disable(dev); /* NVMe: 플랫폼별 비활성화. */

	if (iov->link != dev->devfn) /* NVMe: dep_link 제거. */
		sysfs_remove_link(&dev->dev.kobj, "dep_link"); /* NVMe: dep_link 제거. */

	iov->num_VFs = 0; /* NVMe: 활성화 카운트 0. */
	pci_iov_set_numvfs(dev, 0); /* NVMe: NumVFs 0. */
}

/*
 * sriov_init:
 *   SR-IOV 능력을 초기화하고 VF 리소스를 계산한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static int sriov_init(struct pci_dev *dev, int pos)
{
	int i, bar64; /* NVMe: 반복 및 64비트 BAR 플래그. */
	int rc; /* NVMe: 결과. */
	int nres; /* NVMe: 할당된 리소스 개수. */
	u32 pgsz; /* NVMe: 지원 페이지 크기. */
	u16 ctrl, total; /* NVMe: control/total vf. */
	struct pci_sriov *iov; /* NVMe: SR-IOV 구조체 포인터. */
	struct resource *res; /* NVMe: 리소스 포인터. */
	const char *res_name; /* NVMe: 리소스 이름. */
	struct pci_dev *pdev; /* NVMe: 버스상 다른 PF 포인터. */
	u32 sriovbars[PCI_SRIOV_NUM_BARS]; /* NVMe: BAR 사이징 임시 배열. */

	pci_read_config_word(dev, pos + PCI_SRIOV_CTRL, &ctrl); /* NVMe: 초기 SR-IOV Control 읽기. */
	if (ctrl & PCI_SRIOV_CTRL_VFE) { /* NVMe: 이미 VF Enable이면 복원 불필요. */
		pci_write_config_word(dev, pos + PCI_SRIOV_CTRL, 0); /* NVMe: SR-IOV Control 0으로 클리어. */
		ssleep(1); /* NVMe: 하드웨어 비활성화 대기. */
	}

	ctrl = 0; /* NVMe: control 초기화. */
	list_for_each_entry(pdev, &dev->bus->devices, bus_list) /* NVMe: 동일 버스의 다른 PF 검색. */
		if (pdev->is_physfn) /* NVMe: PF 발견 시. */
			goto found; /* NVMe: found로 이동. */

	pdev = NULL; /* NVMe: 다른 PF 없음. */
	if (pci_ari_enabled(dev->bus)) /* NVMe: ARI 활성화 시. */
		ctrl |= PCI_SRIOV_CTRL_ARI; /* NVMe: ARI 비트 설정. */

found: /* NVMe: PF 검색 완료 레이블. */
	pci_write_config_word(dev, pos + PCI_SRIOV_CTRL, ctrl); /* NVMe: Control 레지스터 기록. */

	pci_read_config_word(dev, pos + PCI_SRIOV_TOTAL_VF, &total); /* NVMe: Total VF 수 읽기. */
	if (!total) /* NVMe: VF 미지원. */
		return 0; /* NVMe: 후보 없음. */

	pci_read_config_dword(dev, pos + PCI_SRIOV_SUP_PGSIZE, &pgsz); /* NVMe: 지원 페이지 크기 읽기. */
	i = PAGE_SHIFT > 12 ? PAGE_SHIFT - 12 : 0; /* NVMe: 페이지 시프트 기반 필터 인덱스. */
	pgsz &= ~((1 << i) - 1); /* NVMe: 페이지 크기보다 작은 비트 제거. */
	if (!pgsz) /* NVMe: 유효한 페이지 크기 없음. */
		return -EIO; /* NVMe: I/O 오류. */

	pgsz &= ~(pgsz - 1); /* NVMe: 가장 작은 지원 페이지 크기 추출. */
	pci_write_config_dword(dev, pos + PCI_SRIOV_SYS_PGSIZE, pgsz); /* NVMe: 시스템 페이지 크기 설정. */

	iov = kzalloc_obj(*iov); /* NVMe: SR-IOV 구조체 할당. */
	if (!iov) /* NVMe: PF가 아니면 할 일 없음. */
		return -ENOMEM; /* NVMe: 메모리 부족. */

	/* Sizing SR-IOV BARs with VF Enable cleared - no decode */
	__pci_size_stdbars(dev, PCI_SRIOV_NUM_BARS, /* NVMe: VF BAR 크기 측정. */
			   pos + PCI_SRIOV_BAR, sriovbars); /* NVMe: SR-IOV BAR 레지스터 오프셋. */

	nres = 0; /* NVMe: 유효 리소스 카운트 초기화. */
	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) { /* NVMe: VF BAR 복원 루프. */
		int idx = pci_resource_num_from_vf_bar(i); /* NVMe: VF BAR 인덱스 변환. */

		res = &dev->resource[idx]; /* NVMe: PF 리소스 참조. */
		res_name = pci_resource_name(dev, idx); /* NVMe: 리소스 이름. */

		/*
		 * If it is already FIXED, don't change it, something
		 * (perhaps EA or header fixups) wants it this way.
		 */
		if (res->flags & IORESOURCE_PCI_FIXED) /* NVMe: 고정 리소스. */
			bar64 = (res->flags & IORESOURCE_MEM_64) ? 1 : 0; /* NVMe: 64비트 여부 결정. */
		else /* NVMe: 없으면. */
			bar64 = __pci_read_base(dev, pci_bar_unknown, res, /* NVMe: BAR 읽기. */
						pos + PCI_SRIOV_BAR + i * 4, /* NVMe: BAR 레지스터 주소. */
						&sriovbars[i]); /* NVMe: 사이징 값 저장. */
		if (!res->flags) /* NVMe: 미구현 BAR. */
			continue; /* NVMe: 다음 장치. */
		if (resource_size(res) & (PAGE_SIZE - 1)) { /* NVMe: PAGE 정렬되지 않음. */
			rc = -EIO; /* NVMe: I/O 오류. */
			goto failed; /* NVMe: 정리. */
		}
		iov->barsz[i] = resource_size(res); /* NVMe: VF BAR 개별 크기 저장. */
		resource_set_size(res, resource_size(res) * total); /* NVMe: total VF 수만큼 전체 리소스 크기 확장. */
		pci_info(dev, "%s %pR: contains BAR %d for %d VFs\n", /* NVMe: 정보 메시지. */
			 res_name, res, i, total); /* NVMe: 메시지 인자. */
		i += bar64; /* NVMe: 64비트 BAR이면 다음 인덱스 스킵. */
		nres++; /* NVMe: 유효 리소스 카운트 증가. */
	}

	iov->pos = pos; /* NVMe: SR-IOV capability 위치 저장. */
	iov->nres = nres; /* NVMe: 유효 리소스 수 저장. */
	iov->ctrl = ctrl; /* NVMe: control 캐시. */
	iov->total_VFs = total; /* NVMe: 총 VF 수 저장. */
	iov->driver_max_VFs = total; /* NVMe: 드라이버가 사용할 최대 VF 수 초기화. */
	pci_read_config_word(dev, pos + PCI_SRIOV_VF_DID, &iov->vf_device); /* NVMe: VF Device ID 읽기. */
	iov->pgsz = pgsz; /* NVMe: 페이지 크기 저장. */
	iov->self = dev; /* NVMe: 자기 참조. */
	iov->drivers_autoprobe = true; /* NVMe: VF 자동 탐색 기본 활성. */
	pci_read_config_dword(dev, pos + PCI_SRIOV_CAP, &iov->cap); /* NVMe: SR-IOV capability 읽기. */
	pci_read_config_byte(dev, pos + PCI_SRIOV_FUNC_LINK, &iov->link); /* NVMe: Function Link 읽기. */
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) /* NVMe: Root Complex Endpoint이면. */
		iov->link = PCI_DEVFN(PCI_SLOT(dev->devfn), iov->link); /* NVMe: link devfn 조정. */
	iov->vf_rebar_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_VF_REBAR); /* NVMe: VF Resizable BAR capability 위치 저장. */

	if (pdev) /* NVMe: 동일 버스에 다른 PF가 있으면. */
		iov->dev = pci_dev_get(pdev); /* NVMe: 해당 PF 참조 증가. */
	else /* NVMe: 없으면. */
		iov->dev = dev; /* NVMe: 자신을 참조. */

	dev->sriov = iov; /* NVMe: PF에 SR-IOV 구조체 연결. */
	dev->is_physfn = 1; /* NVMe: PF 플래그 설정. */
	rc = compute_max_vf_buses(dev); /* NVMe: 최대 VF 버스 수 계산. */
	if (rc) /* NVMe: 계산 실패. */
		goto fail_max_buses; /* NVMe: 구조체 정리. */

	return 0; /* NVMe: 후보 없음. */

fail_max_buses: /* NVMe: 버스 계산 실패 레이블. */
	dev->sriov = NULL; /* NVMe: 포인터 클리어. */
	dev->is_physfn = 0; /* NVMe: PF 플래그 클리어. */
failed: /* NVMe: 초기화 실패 레이블. */
	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) { /* NVMe: VF BAR 복원 루프. */
		res = &dev->resource[pci_resource_num_from_vf_bar(i)]; /* NVMe: VF BAR 리소스. */
		res->flags = 0; /* NVMe: 리소스 플래그 클리어. */
	}

	kfree(iov); /* NVMe: 구조체 해제. */
	return rc; /* NVMe: 오류 반환. */
}

/*
 * sriov_release:
 *   SR-IOV 구조체를 해제한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static void sriov_release(struct pci_dev *dev)
{
	BUG_ON(dev->sriov->num_VFs); /* NVMe: VF가 남아 있으면 패닉. */

	if (dev != dev->sriov->dev) /* NVMe: 의존 PF가 자신이 아니면. */
		pci_dev_put(dev->sriov->dev); /* NVMe: 의존 PF 참조 감소. */

	kfree(dev->sriov); /* NVMe: SR-IOV 구조체 메모리 해제. */
	dev->sriov = NULL; /* NVMe: 포인터 클리어. */
}

/*
 * sriov_restore_vf_rebar_state:
 *   VF Resizable BAR 상태를 복원한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static void sriov_restore_vf_rebar_state(struct pci_dev *dev)
{
	unsigned int pos, nbars, i; /* NVMe: capability 위치, BAR 개수, 반복. */
	u32 ctrl; /* NVMe: REBAR control 값. */

	pos = pci_iov_vf_rebar_cap(dev); /* NVMe: VF REBAR capability 위치 획득. */
	if (!pos) /* NVMe: REBAR capability 없음. */
		return; /* NVMe: 조기 반환. */

	pci_read_config_dword(dev, pos + PCI_VF_REBAR_CTRL, &ctrl); /* NVMe: REBAR control 읽기. */
	nbars = FIELD_GET(PCI_VF_REBAR_CTRL_NBAR_MASK, ctrl); /* NVMe: BAR 개수 추출. */

	for (i = 0; i < nbars; i++, pos += 8) { /* NVMe: 각 REBAR 항목 순회. */
		int bar_idx, size; /* NVMe: BAR 인덱스와 크기. */

		pci_read_config_dword(dev, pos + PCI_VF_REBAR_CTRL, &ctrl); /* NVMe: REBAR control 읽기. */
		bar_idx = FIELD_GET(PCI_VF_REBAR_CTRL_BAR_IDX, ctrl); /* NVMe: BAR 인덱스 추출. */
		size = pci_rebar_bytes_to_size(dev->sriov->barsz[bar_idx]); /* NVMe: 바이트 크기를 spec 크기로 변환. */
		ctrl &= ~PCI_VF_REBAR_CTRL_BAR_SIZE; /* NVMe: 크기 필드 클리어. */
		ctrl |= FIELD_PREP(PCI_VF_REBAR_CTRL_BAR_SIZE, size); /* NVMe: 새 크기 기록. */
		pci_write_config_dword(dev, pos + PCI_VF_REBAR_CTRL, ctrl); /* NVMe: REBAR control 레지스터 쓰기. */
	}
}

/*
 * sriov_restore_state:
 *   suspend/resume 후 SR-IOV 레지스터 상태를 복원한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
static void sriov_restore_state(struct pci_dev *dev)
{
	int i; /* NVMe: 반복 변수. */
	u16 ctrl; /* NVMe: control 값. */
	struct pci_sriov *iov = dev->sriov;

	pci_read_config_word(dev, iov->pos + PCI_SRIOV_CTRL, &ctrl); /* NVMe: 현재 control 읽기. */
	if (ctrl & PCI_SRIOV_CTRL_VFE) /* NVMe: 이미 VF Enable이면 복원 불필요. */
		return; /* NVMe: 조기 반환. */

	/*
	 * Restore PCI_SRIOV_CTRL_ARI before pci_iov_set_numvfs() because
	 * it reads offset & stride, which depend on PCI_SRIOV_CTRL_ARI.
	 */
	ctrl &= ~PCI_SRIOV_CTRL_ARI; /* NVMe: ARI 비트 클리어. */
	ctrl |= iov->ctrl & PCI_SRIOV_CTRL_ARI; /* NVMe: 저장된 ARI 상태 복원. */
	pci_write_config_word(dev, iov->pos + PCI_SRIOV_CTRL, ctrl); /* NVMe: Control 기록. */

	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) /* NVMe: VF BAR 복원 루프. */
		pci_update_resource(dev, pci_resource_num_from_vf_bar(i)); /* NVMe: BAR 주소 갱신. */

	pci_write_config_dword(dev, iov->pos + PCI_SRIOV_SYS_PGSIZE, iov->pgsz); /* NVMe: 페이지 크기 복원. */
	pci_iov_set_numvfs(dev, iov->num_VFs); /* NVMe: NumVFs 복원. */
	pci_write_config_word(dev, iov->pos + PCI_SRIOV_CTRL, iov->ctrl); /* NVMe: control 복원. */
	if (iov->ctrl & PCI_SRIOV_CTRL_VFE) /* NVMe: VF Enable 상태였으면. */
		msleep(100); /* NVMe: 안정화 대기. */
}

/**
 * pci_iov_init - initialize the IOV capability
 * @dev: the PCI device
 *
 * Returns 0 on success, or negative on failure.
 */
/*
 * pci_iov_init:
 *   PCI IOV 능력을 초기화한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
int pci_iov_init(struct pci_dev *dev)
{
	int pos; /* NVMe: capability 위치. */

	if (!pci_is_pcie(dev)) /* NVMe: PCIe가 아니면 SR-IOV 없음. */
		return -ENODEV; /* NVMe: 장치 없음. */

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_SRIOV); /* NVMe: SR-IOV 확장 capability 검색. */
	if (pos) /* NVMe: 발견 시. */
		return sriov_init(dev, pos); /* NVMe: 초기화 함수 호출. */

	return -ENODEV; /* NVMe: 장치 없음. */
}

/**
 * pci_iov_release - release resources used by the IOV capability
 * @dev: the PCI device
 */
/*
 * pci_iov_release:
 *   PF의 IOV 리소스를 해제한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
void pci_iov_release(struct pci_dev *dev)
{
	if (dev->is_physfn) /* NVMe: PF일 때만. */
		sriov_release(dev); /* NVMe: SR-IOV 구조체 해제. */
}

/**
 * pci_iov_remove - clean up SR-IOV state after PF driver is detached
 * @dev: the PCI device
 */
/*
 * pci_iov_remove:
 *   PF 드라이버 detach 후 SR-IOV 상태를 정리한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
void pci_iov_remove(struct pci_dev *dev)
{
	struct pci_sriov *iov = dev->sriov;

	if (!dev->is_physfn) /* NVMe: PF가 아니면. */
		return; /* NVMe: 조기 반환. */

	iov->driver_max_VFs = iov->total_VFs; /* NVMe: 드라이버 제한을 total로 복원. */
	if (iov->num_VFs) /* NVMe: VF가 남아 있으면. */
		pci_warn(dev, "driver left SR-IOV enabled after remove\n"); /* NVMe: 경고. */
}

/**
 * pci_iov_update_resource - update a VF BAR
 * @dev: the PCI device
 * @resno: the resource number
 *
 * Update a VF BAR in the SR-IOV capability of a PF.
 */
/*
 * pci_iov_update_resource:
 *   VF BAR 리소스를 갱신한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
void pci_iov_update_resource(struct pci_dev *dev, int resno)
{
	struct pci_sriov *iov = dev->is_physfn ? dev->sriov : NULL; /* NVMe: PF인 경우에만 구조체 획득. */
	struct resource *res = pci_resource_n(dev, resno); /* NVMe: 갱신 대상 리소스. */
	int vf_bar = pci_resource_num_to_vf_bar(resno); /* NVMe: VF BAR 인덱스. */
	struct pci_bus_region region; /* NVMe: bus 주소 영역. */
	u16 cmd; /* NVMe: control. */
	u32 new; /* NVMe: 새 BAR 값. */
	int reg; /* NVMe: 레지스터 오프셋. */

	/*
	 * The generic pci_restore_bars() path calls this for all devices,
	 * including VFs and non-SR-IOV devices.  If this is not a PF, we
	 * have nothing to do.
	 */
	if (!iov) /* NVMe: PF가 아니면 할 일 없음. */
		return; /* NVMe: 조기 반환. */

	pci_read_config_word(dev, iov->pos + PCI_SRIOV_CTRL, &cmd); /* NVMe: control 읽기. */
	if ((cmd & PCI_SRIOV_CTRL_VFE) && (cmd & PCI_SRIOV_CTRL_MSE)) { /* NVMe: VF가 이미 활성화/디코딩 중이면. */
		dev_WARN(&dev->dev, "can't update enabled VF BAR%d %pR\n", /* NVMe: 경고. */
			 vf_bar, res); /* NVMe: 인자. */
		return; /* NVMe: 조기 반환. */
	}

	/*
	 * Ignore unimplemented BARs, unused resource slots for 64-bit
	 * BARs, and non-movable resources, e.g., those described via
	 * Enhanced Allocation.
	 */
	if (!res->flags) /* NVMe: 미구현 BAR. */
		return; /* NVMe: 조기 반환. */

	if (res->flags & IORESOURCE_UNSET) /* NVMe: 아직 주소 미할당. */
		return; /* NVMe: 조기 반환. */

	if (res->flags & IORESOURCE_PCI_FIXED) /* NVMe: 고정 리소스. */
		return; /* NVMe: 조기 반환. */

	pcibios_resource_to_bus(dev->bus, &region, res); /* NVMe: CPU 주소를 bus 주소로 변환. */
	new = region.start; /* NVMe: bus 주소 시작을 새 BAR 값으로. */
	new |= res->flags & ~PCI_BASE_ADDRESS_MEM_MASK; /* NVMe: BAR 속성 플래그 추가. */

	reg = iov->pos + PCI_SRIOV_BAR + 4 * vf_bar; /* NVMe: 대상 BAR 레지스터 오프셋. */
	pci_write_config_dword(dev, reg, new); /* NVMe: BAR 하위 32비트 쓰기. */
	if (res->flags & IORESOURCE_MEM_64) { /* NVMe: 64비트 BAR이면. */
		new = region.start >> 16 >> 16; /* NVMe: 상위 32비트 추출. */
		pci_write_config_dword(dev, reg + 4, new); /* NVMe: BAR 상위 32비트 쓰기. */
	}
}

resource_size_t __weak pcibios_iov_resource_alignment(struct pci_dev *dev,
						      int resno)
{
	return pci_iov_resource_size(dev, resno); /* NVMe: VF BAR 크기를 기본 정렬값으로 반환. */
}

/**
 * pci_sriov_resource_alignment - get resource alignment for VF BAR
 * @dev: the PCI device
 * @resno: the resource number
 *
 * Returns the alignment of the VF BAR found in the SR-IOV capability.
 * This is not the same as the resource size which is defined as
 * the VF BAR size multiplied by the number of VFs.  The alignment
 * is just the VF BAR size.
 */
/*
 * pci_sriov_resource_alignment:
 *   VF BAR 정렬값을 반환한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
resource_size_t pci_sriov_resource_alignment(struct pci_dev *dev, int resno)
{
	return pcibios_iov_resource_alignment(dev, resno); /* NVMe: 플랫폼 정렬값 반환. */
}

/**
 * pci_restore_iov_state - restore the state of the IOV capability
 * @dev: the PCI device
 */
/*
 * pci_restore_iov_state:
 *   IOV 상태를 복원한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
void pci_restore_iov_state(struct pci_dev *dev)
{
	if (dev->is_physfn) { /* NVMe: PF일 때만. */
		sriov_restore_vf_rebar_state(dev); /* NVMe: VF REBAR 상태 복원. */
		sriov_restore_state(dev); /* NVMe: SR-IOV 레지스터 상태 복원. */
	}
}

/**
 * pci_vf_drivers_autoprobe - set PF property drivers_autoprobe for VFs
 * @dev: the PCI device
 * @auto_probe: set VF drivers auto probe flag
 */
/*
 * pci_vf_drivers_autoprobe:
 *   VF 드라이버 자동 탐색 여부를 설정한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
void pci_vf_drivers_autoprobe(struct pci_dev *dev, bool auto_probe)
{
	if (dev->is_physfn) /* NVMe: PF일 때만. */
		dev->sriov->drivers_autoprobe = auto_probe; /* NVMe: 자동 탐색 플래그 갱신. */
}

/**
 * pci_iov_bus_range - find bus range used by Virtual Function
 * @bus: the PCI bus
 *
 * Returns max number of buses (exclude current one) used by Virtual
 * Functions.
 */
/*
 * pci_iov_bus_range:
 *   해당 버스에서 VF가 사용할 최대 버스 범위를 계산한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
int pci_iov_bus_range(struct pci_bus *bus)
{
	int max = 0; /* NVMe: 최대 버스 초기화. */
	struct pci_dev *dev; /* NVMe: 버스상 PF 순회용. */

	list_for_each_entry(dev, &bus->devices, bus_list) { /* NVMe: 버스의 모든 장치 순회. */
		if (!dev->is_physfn) /* NVMe: PF가 아니면. */
			continue; /* NVMe: 다음 장치. */
		if (dev->sriov->max_VF_buses > max) /* NVMe: 최대값 갱신. */
			max = dev->sriov->max_VF_buses; /* NVMe: 최대 VF 버스 저장. */
	}

	return max ? max - bus->number : 0; /* NVMe: 현재 버스를 제외한 추가 버스 수 반환. */
}

/**
 * pci_enable_sriov - enable the SR-IOV capability
 * @dev: the PCI device
 * @nr_virtfn: number of virtual functions to enable
 *
 * Returns 0 on success, or negative on failure.
 */
/*
 * pci_enable_sriov:
 *   SR-IOV를 활성화하는 외부 인터페이스다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
int pci_enable_sriov(struct pci_dev *dev, int nr_virtfn)
{
	might_sleep(); /* NVMe: 수면 가능 표시. */

	if (!dev->is_physfn) /* NVMe: PF가 아니면. */
		return -ENOSYS; /* NVMe: PF가 아니면 불가. */

	return sriov_enable(dev, nr_virtfn); /* NVMe: 낮부 활성화 함수 호출. */
}
EXPORT_SYMBOL_GPL(pci_enable_sriov); /* NVMe: 심볼 외부 공개. */

/**
 * pci_disable_sriov - disable the SR-IOV capability
 * @dev: the PCI device
 */
/*
 * pci_disable_sriov:
 *   SR-IOV를 비활성화하는 외부 인터페이스다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
void pci_disable_sriov(struct pci_dev *dev)
{
	might_sleep(); /* NVMe: 수면 가능 표시. */

	if (!dev->is_physfn) /* NVMe: PF가 아니면. */
		return; /* NVMe: 조기 반환. */

	sriov_disable(dev); /* NVMe: SR-IOV 비활성화. */
}
EXPORT_SYMBOL_GPL(pci_disable_sriov); /* NVMe: 심볼 외부 공개. */

/**
 * pci_num_vf - return number of VFs associated with a PF device_release_driver
 * @dev: the PCI device
 *
 * Returns number of VFs, or 0 if SR-IOV is not enabled.
 */
/*
 * pci_num_vf:
 *   PF에 연결된 VF 개수를 반환한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
int pci_num_vf(struct pci_dev *dev)
{
	if (!dev->is_physfn) /* NVMe: PF가 아니면. */
		return 0; /* NVMe: 후보 없음. */

	return dev->sriov->num_VFs; /* NVMe: 활성 VF 수 반환. */
}
EXPORT_SYMBOL_GPL(pci_num_vf); /* NVMe: 심볼 외부 공개. */

/**
 * pci_vfs_assigned - returns number of VFs are assigned to a guest
 * @dev: the PCI device
 *
 * Returns number of VFs belonging to this device that are assigned to a guest.
 * If device is not a physical function returns 0.
 */
/*
 * pci_vfs_assigned:
 *   게스트에 할당된 VF 개수를 반환한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
int pci_vfs_assigned(struct pci_dev *dev)
{
	struct pci_dev *vfdev; /* NVMe: VF 순회용 포인터. */
	unsigned int vfs_assigned = 0; /* NVMe: 할당 카운트 초기화. */
	unsigned short dev_id; /* NVMe: VF device ID. */

	/* only search if we are a PF */
	if (!dev->is_physfn) /* NVMe: PF가 아니면. */
		return 0; /* NVMe: 후보 없음. */

	/*
	 * determine the device ID for the VFs, the vendor ID will be the
	 * same as the PF so there is no need to check for that one
	 */
	dev_id = dev->sriov->vf_device; /* NVMe: VF device ID 획득. */

	/* loop through all the VFs to see if we own any that are assigned */
	vfdev = pci_get_device(dev->vendor, dev_id, NULL); /* NVMe: 동일 vendor/device의 첫 장치 검색. */
	while (vfdev) { /* NVMe: 일치 장치가 있을 동안. */
		/*
		 * It is considered assigned if it is a virtual function with
		 * our dev as the physical function and the assigned bit is set
		 */
		if (vfdev->is_virtfn && (vfdev->physfn == dev) && /* NVMe: 해당 PF의 VF이고. */
			pci_is_dev_assigned(vfdev)) /* NVMe: 게스트에 할당되었으면. */
			vfs_assigned++; /* NVMe: 카운트 증가. */

		vfdev = pci_get_device(dev->vendor, dev_id, vfdev); /* NVMe: 다음 일치 장치 검색. */
	}

	return vfs_assigned; /* NVMe: 할당된 VF 수 반환. */
}
EXPORT_SYMBOL_GPL(pci_vfs_assigned); /* NVMe: 심볼 외부 공개. */

/**
 * pci_sriov_set_totalvfs -- reduce the TotalVFs available
 * @dev: the PCI PF device
 * @numvfs: number that should be used for TotalVFs supported
 *
 * Should be called from PF driver's probe routine with
 * device's mutex held.
 *
 * Returns 0 if PF is an SRIOV-capable device and
 * value of numvfs valid. If not a PF return -ENOSYS;
 * if numvfs is invalid return -EINVAL;
 * if VFs already enabled, return -EBUSY.
 */
/*
 * pci_sriov_set_totalvfs:
 *   드라이버가 사용할 최대 VF 수를 제한한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
int pci_sriov_set_totalvfs(struct pci_dev *dev, u16 numvfs)
{
	if (!dev->is_physfn) /* NVMe: PF가 아니면. */
		return -ENOSYS; /* NVMe: PF가 아니면 불가. */

	if (numvfs > dev->sriov->total_VFs) /* NVMe: total 초과 불가. */
		return -EINVAL; /* NVMe: 잘못된 인자. */

	/* Shouldn't change if VFs already enabled */
	if (dev->sriov->ctrl & PCI_SRIOV_CTRL_VFE) /* NVMe: VF가 활성화 중이면 변경 불가. */
		return -EBUSY; /* NVMe: 바쁨. */

	dev->sriov->driver_max_VFs = numvfs; /* NVMe: 드라이버 최대 VF 수 제한. */
	return 0; /* NVMe: 후보 없음. */
}
EXPORT_SYMBOL_GPL(pci_sriov_set_totalvfs); /* NVMe: 심볼 외부 공개. */

/**
 * pci_sriov_get_totalvfs -- get total VFs supported on this device
 * @dev: the PCI PF device
 *
 * For a PCIe device with SRIOV support, return the PCIe
 * SRIOV capability value of TotalVFs or the value of driver_max_VFs
 * if the driver reduced it.  Otherwise 0.
 */
/*
 * pci_sriov_get_totalvfs:
 *   현재 드라이버가 노출하는 totalvfs 값을 반환한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
int pci_sriov_get_totalvfs(struct pci_dev *dev)
{
	if (!dev->is_physfn) /* NVMe: PF가 아니면. */
		return 0; /* NVMe: 후보 없음. */

	return dev->sriov->driver_max_VFs; /* NVMe: 드라이버가 노출하는 totalvfs 반환. */
}
EXPORT_SYMBOL_GPL(pci_sriov_get_totalvfs); /* NVMe: 심볼 외부 공개. */

/**
 * pci_sriov_configure_simple - helper to configure SR-IOV
 * @dev: the PCI device
 * @nr_virtfn: number of virtual functions to enable, 0 to disable
 *
 * Enable or disable SR-IOV for devices that don't require any PF setup
 * before enabling SR-IOV.  Return value is negative on error, or number of
 * VFs allocated on success.
 */
/*
 * pci_sriov_configure_simple:
 *   별도 PF 준비 없이 SR-IOV를 활성화/비활성화한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
int pci_sriov_configure_simple(struct pci_dev *dev, int nr_virtfn)
{
	int rc; /* NVMe: 결과. */

	might_sleep(); /* NVMe: 수면 가능 표시. */

	if (!dev->is_physfn) /* NVMe: PF가 아니면. */
		return -ENODEV; /* NVMe: 장치 없음. */

	if (pci_vfs_assigned(dev)) { /* NVMe: VF가 게스트에 할당 중이면. */
		pci_warn(dev, "Cannot modify SR-IOV while VFs are assigned\n"); /* NVMe: 경고. */
		return -EPERM; /* NVMe: 권한 없음. */
	}

	if (nr_virtfn == 0) { /* NVMe: 비활성화 요청. */
		sriov_disable(dev); /* NVMe: SR-IOV 비활성화. */
		return 0; /* NVMe: 후보 없음. */
	}

	rc = sriov_enable(dev, nr_virtfn); /* NVMe: 활성화 시도. */
	if (rc < 0) /* NVMe: 실패. */
		return rc; /* NVMe: 오류 반환. */

	return nr_virtfn; /* NVMe: 활성화된 VF 수 반환. */
}
EXPORT_SYMBOL_GPL(pci_sriov_configure_simple); /* NVMe: 심볼 외부 공개. */

/**
 * pci_iov_vf_bar_set_size - set a new size for a VF BAR
 * @dev: the PCI device
 * @resno: the resource number
 * @size: new size as defined in the spec (0=1MB, 31=128TB)
 *
 * Set the new size of a VF BAR that supports VF resizable BAR capability.
 * Unlike pci_resize_resource(), this does not cause the resource that
 * reserves the MMIO space (originally up to total_VFs) to be resized, which
 * means that following calls to pci_enable_sriov() can fail if the resources
 * no longer fit.
 *
 * Return: 0 on success, or negative on failure.
 */
/*
 * pci_iov_vf_bar_set_size:
 *   VF Resizable BAR의 크기를 변경한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
int pci_iov_vf_bar_set_size(struct pci_dev *dev, int resno, int size)
{
	if (!pci_resource_is_iov(resno)) /* NVMe: IOV 리소스가 아니면. */
		return -EINVAL; /* NVMe: 잘못된 인자. */

	if (pci_iov_is_memory_decoding_enabled(dev)) /* NVMe: 메모리 디코딩이 활성화 중이면. */
		return -EBUSY; /* NVMe: 바쁨. */

	if (!pci_rebar_size_supported(dev, resno, size)) /* NVMe: 지원되지 않는 크기. */
		return -EINVAL; /* NVMe: 잘못된 인자. */

	return pci_rebar_set_size(dev, resno, size); /* NVMe: REBAR 크기 설정. */
}
EXPORT_SYMBOL_GPL(pci_iov_vf_bar_set_size); /* NVMe: 심볼 외부 공개. */

/**
 * pci_iov_vf_bar_get_sizes - get VF BAR sizes allowing to create up to num_vfs
 * @dev: the PCI device
 * @resno: the resource number
 * @num_vfs: number of VFs
 *
 * Get the sizes of a VF resizable BAR that can accommodate @num_vfs within
 * the currently assigned size of the resource @resno.
 *
 * Return: A bitmask of sizes in format defined in the spec (bit 0=1MB,
 * bit 31=128TB).
 */
/*
 * pci_iov_vf_bar_get_sizes:
 *   주어진 VF 수에 맞는 VF BAR 크기 후보 집합을 반환한다.
 *   NVMe: NVMe PF가 VF를 생성/관리할 때 이 함수가 호출된다.
 */
u32 pci_iov_vf_bar_get_sizes(struct pci_dev *dev, int resno, int num_vfs)
{
	u64 vf_len = pci_resource_len(dev, resno); /* NVMe: PF IOV 리소스 전체 길이. */
	u64 sizes; /* NVMe: 후보 크기 비트마스크. */

	if (!num_vfs) /* NVMe: VF 수가 0이면. */
		return 0; /* NVMe: 후보 없음. */

	do_div(vf_len, num_vfs); /* NVMe: VF당 평균 길이 계산. */
	sizes = (roundup_pow_of_two(vf_len + 1) - 1) >> ilog2(SZ_1M); /* NVMe: 1MB 단위 후보 집합 생성. */

	return sizes & pci_rebar_get_possible_sizes(dev, resno); /* NVMe: 가능한 크기와 교집합 반환. */
}
EXPORT_SYMBOL_GPL(pci_iov_vf_bar_get_sizes); /* NVMe: 심볼 외부 공개. */
