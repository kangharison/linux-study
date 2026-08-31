// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe AER software error injection support.
 *
 * Debugging PCIe AER code is quite difficult because it is hard to
 * trigger various real hardware errors. Software based error
 * injection can fake almost all kinds of errors with the help of a
 * user space helper tool aer-inject, which can be gotten from:
 *   https://github.com/intel/aer-inject.git
 *
 * Copyright 2009 Intel Corporation.
 *     Huang Ying <ying.huang@intel.com>
 */

/*
 * [한국어 설명] 가짜 PCIe 오류를 만들어 복구 경로를 시험하는 도구 (aer_inject.c)
 *
 * === 파일의 역할 ===
 * AER 복구 코드를 시험하기가 매우 어렵다는 문제에서 출발한 파일이다.
 * 위 원문 주석이 그 사정을 밝힌다 — 실제 하드웨어 오류를 일부러 일으키는
 * 것은 거의 불가능하다. Completion Timeout 이나 Poisoned TLP 를 마음대로
 * 만들어 낼 방법이 없기 때문이다.
 *
 * 그래서 이 파일은 오류를 흉내 낸다. 방법이 영리하다 — 실제로 오류를
 * 일으키는 것이 아니라, config space 접근을 가로채서 "AER 상태 레지스터에
 * 오류 비트가 서 있는 것처럼" 보이게 만든다.
 *
 *   1) pci_bus_set_ops() [access.c] 로 그 버스의 config 접근 ops 를
 *      자기 것으로 바꿔 끼운다.
 *   2) AER 관련 레지스터를 읽으면 진짜 하드웨어 대신 사용자가 지정한
 *      가짜 값을 돌려준다.
 *   3) 그 상태에서 AER 인터럽트를 흉내 낸다(aer_irq 를 직접 호출).
 *   4) aer.c 는 진짜 오류가 난 줄 알고 정상 복구 절차를 밟는다.
 *
 * 사용자 인터페이스는 /dev/aer_inject 캐릭터 장치이고, 사용자 공간 도구
 * aer-inject 가 struct aer_error_inj 를 write 해서 무엇을 주입할지 지정한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 주입: aer-inject 도구
 *         -> write(/dev/aer_inject, struct aer_error_inj)
 *            -> [이 파일] aer_inject_write() -> aer_inject()
 *               -> pci_bus_set_ops() 로 config ops 가로채기
 *               -> aer_irq() [aer.c] 직접 호출
 *                  -> 이후는 진짜 오류와 완전히 같은 경로
 *                     -> pcie_do_recovery() -> 드라이버 콜백
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(write 시스템 호출). 다만 가로챈
 * config ops 는 인터럽트 문맥에서도 불릴 수 있어 스핀락으로 보호한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: userspace 의 aer-inject 도구.
 * 아래쪽: access.c 의 pci_bus_set_ops(가로채기), pcie/aer.c 의 aer_irq.
 * 공유 상태: 주입된 오류 목록(einjected)과 가로챈 ops 목록(pci_bus_ops_list).
 *   둘 다 전역 스핀락 inject_lock 으로 보호한다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일과 직접 연결되지 않는다(전수 확인).
 *
 * 하지만 NVMe 오류 복구 코드를 검증하는 데 실질적으로 쓸 수 있는 도구다.
 * NVMe SSD 에 ERR_FATAL 을 주입하면 nvme_error_detected() ->
 * nvme_slot_reset() -> nvme_error_resume() 이 실제로 불리고, 그 과정에서
 * 진행 중이던 I/O 가 올바르게 실패 처리되는지, 컨트롤러가 제대로
 * 재초기화되는지를 확인할 수 있다.
 *
 * 주의할 점은 이것이 "오류를 흉내 낸 것" 이라는 사실이다. 하드웨어는
 * 멀쩡하므로, 진짜 오류에서 벌어지는 일(링크가 실제로 끊기거나 DMA 가
 * 중간에 잘리는 것)은 재현되지 않는다. 소프트웨어 경로의 검증에는 충분하지만
 * 하드웨어 상호작용의 검증에는 한계가 있다.
 *
 * (기존 주석은 주입된 오류가 "nvme_reset_work() / nvme_remove() 로
 *  전파될 수 있다" 고 적었으나, 복구 경로가 직접 부르는 것은
 *  nvme_err_handler 에 등록된 콜백들이다. nvme_reset_work 은 그중
 *  nvme_slot_reset 이 nvme_reset_ctrl 을 큐잉해 간접적으로 실행되고,
 *  nvme_remove 는 복구가 실패해 장치가 제거될 때 불린다.)
 *
 * === 주요 함수/구조체 요약 ===
 * aer_inject()              : 주입의 본체. ops 를 가로채고 가짜 상태를
 *                             등록한 뒤 aer_irq 를 부른다.
 * aer_inject_write()        : /dev/aer_inject 의 write 핸들러.
 * pci_read_aer() / pci_write_aer() : 가로챈 config 접근. AER 레지스터
 *                             범위면 가짜 값을, 아니면 원래 ops 로 넘긴다.
 * find_pci_bus_ops() / pci_bus_set_aer_ops() : ops 가로채기 관리.
 * struct aer_error          : 주입된 가짜 오류 하나. 어느 장치의 어느
 *                             레지스터에 어떤 값을 보이게 할지 담는다.
 * struct pci_bus_ops        : 가로채기 전의 원래 ops 를 보관해 두는 구조.
 *                             해제할 때 되돌리기 위해 필요하다.
 */

#define dev_fmt(fmt) "aer_inject: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/stddef.h>
#include <linux/device.h>

#include "portdrv.h"

/* Override the existing corrected and uncorrected error masks */
static bool aer_mask_override;
module_param(aer_mask_override, bool, 0);

struct aer_error_inj {
	u8 bus;
	u8 dev;
	u8 fn;
	u32 uncor_status;
	u32 cor_status;
	u32 header_log0;
	u32 header_log1;
	u32 header_log2;
	u32 header_log3;
	u32 domain;
};

struct aer_error {
	struct list_head list;
	u32 domain;
	unsigned int bus;
	unsigned int devfn;
	int pos_cap_err;

	u32 uncor_status;
	u32 cor_status;
	u32 header_log0;
	u32 header_log1;
	u32 header_log2;
	u32 header_log3;
	u32 root_status;
	u32 source_id;
};

struct pci_bus_ops {
	struct list_head list;
	struct pci_bus *bus;
	struct pci_ops *ops;
};

static LIST_HEAD(einjected);

static LIST_HEAD(pci_bus_ops_list);

/* Protect einjected and pci_bus_ops_list */
static DEFINE_SPINLOCK(inject_lock);

static void aer_error_init(struct aer_error *err, u32 domain,
			   unsigned int bus, unsigned int devfn,
			   int pos_cap_err)
{
	INIT_LIST_HEAD(&err->list);
	err->domain = domain;
	err->bus = bus;
	err->devfn = devfn;
	err->pos_cap_err = pos_cap_err;
}

/* inject_lock must be held before calling */
static struct aer_error *__find_aer_error(u32 domain, unsigned int bus,
					  unsigned int devfn)
{
	struct aer_error *err;

	list_for_each_entry(err, &einjected, list) {
		if (domain == err->domain &&
		    bus == err->bus &&
		    devfn == err->devfn)
			return err;
	}
	return NULL;
}

/* inject_lock must be held before calling */
static struct aer_error *__find_aer_error_by_dev(struct pci_dev *dev)
{
	int domain = pci_domain_nr(dev->bus);
	if (domain < 0)
		return NULL;
	return __find_aer_error(domain, dev->bus->number, dev->devfn);
}

/* inject_lock must be held before calling */
static struct pci_ops *__find_pci_bus_ops(struct pci_bus *bus)
{
	struct pci_bus_ops *bus_ops;

	list_for_each_entry(bus_ops, &pci_bus_ops_list, list) {
		if (bus_ops->bus == bus)
			return bus_ops->ops;
	}
	return NULL;
}

static struct pci_bus_ops *pci_bus_ops_pop(void)
{
	unsigned long flags;
	struct pci_bus_ops *bus_ops;

	spin_lock_irqsave(&inject_lock, flags);
	bus_ops = list_first_entry_or_null(&pci_bus_ops_list,
					   struct pci_bus_ops, list);
	if (bus_ops)
		list_del(&bus_ops->list);
	spin_unlock_irqrestore(&inject_lock, flags);
	return bus_ops;
}

static u32 *find_pci_config_dword(struct aer_error *err, int where,
				  int *prw1cs)
{
	int rw1cs = 0;
	u32 *target = NULL;

	if (err->pos_cap_err == -1)
		return NULL;

	switch (where - err->pos_cap_err) {
	case PCI_ERR_UNCOR_STATUS:
		target = &err->uncor_status;
		rw1cs = 1;
		break;
	case PCI_ERR_COR_STATUS:
		target = &err->cor_status;
		rw1cs = 1;
		break;
	case PCI_ERR_HEADER_LOG:
		target = &err->header_log0;
		break;
	case PCI_ERR_HEADER_LOG+4:
		target = &err->header_log1;
		break;
	case PCI_ERR_HEADER_LOG+8:
		target = &err->header_log2;
		break;
	case PCI_ERR_HEADER_LOG+12:
		target = &err->header_log3;
		break;
	case PCI_ERR_ROOT_STATUS:
		target = &err->root_status;
		rw1cs = 1;
		break;
	case PCI_ERR_ROOT_ERR_SRC:
		target = &err->source_id;
		break;
	}
	if (prw1cs)
		*prw1cs = rw1cs;
	return target;
}

static int aer_inj_read(struct pci_bus *bus, unsigned int devfn, int where,
			int size, u32 *val)
{
	struct pci_ops *ops, *my_ops;
	int rv;

	ops = __find_pci_bus_ops(bus);
	if (!ops)
		return -1;

	my_ops = bus->ops;
	bus->ops = ops;
	rv = ops->read(bus, devfn, where, size, val);
	bus->ops = my_ops;

	return rv;
}

static int aer_inj_write(struct pci_bus *bus, unsigned int devfn, int where,
			 int size, u32 val)
{
	struct pci_ops *ops, *my_ops;
	int rv;

	ops = __find_pci_bus_ops(bus);
	if (!ops)
		return -1;

	my_ops = bus->ops;
	bus->ops = ops;
	rv = ops->write(bus, devfn, where, size, val);
	bus->ops = my_ops;

	return rv;
}

static int aer_inj_read_config(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 *val)
{
	u32 *sim;
	struct aer_error *err;
	unsigned long flags;
	int domain;
	int rv;

	spin_lock_irqsave(&inject_lock, flags);
	if (size != sizeof(u32))
		goto out;
	domain = pci_domain_nr(bus);
	if (domain < 0)
		goto out;
	err = __find_aer_error(domain, bus->number, devfn);
	if (!err)
		goto out;

	sim = find_pci_config_dword(err, where, NULL);
	if (sim) {
		*val = *sim;
		spin_unlock_irqrestore(&inject_lock, flags);
		return 0;
	}
out:
	rv = aer_inj_read(bus, devfn, where, size, val);
	spin_unlock_irqrestore(&inject_lock, flags);
	return rv;
}

static int aer_inj_write_config(struct pci_bus *bus, unsigned int devfn,
				int where, int size, u32 val)
{
	u32 *sim;
	struct aer_error *err;
	unsigned long flags;
	int rw1cs;
	int domain;
	int rv;

	spin_lock_irqsave(&inject_lock, flags);
	if (size != sizeof(u32))
		goto out;
	domain = pci_domain_nr(bus);
	if (domain < 0)
		goto out;
	err = __find_aer_error(domain, bus->number, devfn);
	if (!err)
		goto out;

	sim = find_pci_config_dword(err, where, &rw1cs);
	if (sim) {
		if (rw1cs)
			*sim ^= val;
		else
			*sim = val;
		spin_unlock_irqrestore(&inject_lock, flags);
		return 0;
	}
out:
	rv = aer_inj_write(bus, devfn, where, size, val);
	spin_unlock_irqrestore(&inject_lock, flags);
	return rv;
}

static struct pci_ops aer_inj_pci_ops = {
	.read = aer_inj_read_config,
	.write = aer_inj_write_config,
};

static void pci_bus_ops_init(struct pci_bus_ops *bus_ops,
			     struct pci_bus *bus,
			     struct pci_ops *ops)
{
	INIT_LIST_HEAD(&bus_ops->list);
	bus_ops->bus = bus;
	bus_ops->ops = ops;
}

static int pci_bus_set_aer_ops(struct pci_bus *bus)
{
	struct pci_ops *ops;
	struct pci_bus_ops *bus_ops;
	unsigned long flags;

	bus_ops = kmalloc_obj(*bus_ops);
	if (!bus_ops)
		return -ENOMEM;
	ops = pci_bus_set_ops(bus, &aer_inj_pci_ops);
	spin_lock_irqsave(&inject_lock, flags);
	if (ops == &aer_inj_pci_ops)
		goto out;
	pci_bus_ops_init(bus_ops, bus, ops);
	list_add(&bus_ops->list, &pci_bus_ops_list);
	bus_ops = NULL;
out:
	spin_unlock_irqrestore(&inject_lock, flags);
	kfree(bus_ops);
	return 0;
}

static int aer_inject(struct aer_error_inj *einj)
{
	struct aer_error *err, *rperr;
	struct aer_error *err_alloc = NULL, *rperr_alloc = NULL;
	struct pci_dev *dev, *rpdev;
	struct pcie_device *edev;
	struct device *device;
	unsigned long flags;
	unsigned int devfn = PCI_DEVFN(einj->dev, einj->fn);
	int pos_cap_err, rp_pos_cap_err;
	u32 sever, cor_mask, uncor_mask, cor_mask_orig = 0, uncor_mask_orig = 0;
	int ret = 0;

	dev = pci_get_domain_bus_and_slot(einj->domain, einj->bus, devfn);
	if (!dev)
		return -ENODEV;
	rpdev = pcie_find_root_port(dev);
	/* If Root Port not found, try to find an RCEC */
	if (!rpdev)
		rpdev = dev->rcec;
	if (!rpdev) {
		pci_err(dev, "Neither Root Port nor RCEC found\n");
		ret = -ENODEV;
		goto out_put;
	}

	pos_cap_err = dev->aer_cap;
	if (!pos_cap_err) {
		pci_err(dev, "Device doesn't support AER\n");
		ret = -EPROTONOSUPPORT;
		goto out_put;
	}
	pci_read_config_dword(dev, pos_cap_err + PCI_ERR_UNCOR_SEVER, &sever);
	pci_read_config_dword(dev, pos_cap_err + PCI_ERR_COR_MASK, &cor_mask);
	pci_read_config_dword(dev, pos_cap_err + PCI_ERR_UNCOR_MASK,
			      &uncor_mask);

	rp_pos_cap_err = rpdev->aer_cap;
	if (!rp_pos_cap_err) {
		pci_err(rpdev, "Root port doesn't support AER\n");
		ret = -EPROTONOSUPPORT;
		goto out_put;
	}

	err_alloc =  kzalloc_obj(struct aer_error);
	if (!err_alloc) {
		ret = -ENOMEM;
		goto out_put;
	}
	rperr_alloc =  kzalloc_obj(struct aer_error);
	if (!rperr_alloc) {
		ret = -ENOMEM;
		goto out_put;
	}

	if (aer_mask_override) {
		cor_mask_orig = cor_mask;
		cor_mask &= !(einj->cor_status);
		pci_write_config_dword(dev, pos_cap_err + PCI_ERR_COR_MASK,
				       cor_mask);

		uncor_mask_orig = uncor_mask;
		uncor_mask &= !(einj->uncor_status);
		pci_write_config_dword(dev, pos_cap_err + PCI_ERR_UNCOR_MASK,
				       uncor_mask);
	}

	spin_lock_irqsave(&inject_lock, flags);

	err = __find_aer_error_by_dev(dev);
	if (!err) {
		err = err_alloc;
		err_alloc = NULL;
		aer_error_init(err, einj->domain, einj->bus, devfn,
			       pos_cap_err);
		list_add(&err->list, &einjected);
	}
	err->uncor_status |= einj->uncor_status;
	err->cor_status |= einj->cor_status;
	err->header_log0 = einj->header_log0;
	err->header_log1 = einj->header_log1;
	err->header_log2 = einj->header_log2;
	err->header_log3 = einj->header_log3;

	if (!aer_mask_override && einj->cor_status &&
	    !(einj->cor_status & ~cor_mask)) {
		ret = -EINVAL;
		pci_warn(dev, "The correctable error(s) is masked by device\n");
		spin_unlock_irqrestore(&inject_lock, flags);
		goto out_put;
	}
	if (!aer_mask_override && einj->uncor_status &&
	    !(einj->uncor_status & ~uncor_mask)) {
		ret = -EINVAL;
		pci_warn(dev, "The uncorrectable error(s) is masked by device\n");
		spin_unlock_irqrestore(&inject_lock, flags);
		goto out_put;
	}

	rperr = __find_aer_error_by_dev(rpdev);
	if (!rperr) {
		rperr = rperr_alloc;
		rperr_alloc = NULL;
		aer_error_init(rperr, pci_domain_nr(rpdev->bus),
			       rpdev->bus->number, rpdev->devfn,
			       rp_pos_cap_err);
		list_add(&rperr->list, &einjected);
	}
	if (einj->cor_status) {
		if (rperr->root_status & PCI_ERR_ROOT_COR_RCV)
			rperr->root_status |= PCI_ERR_ROOT_MULTI_COR_RCV;
		else
			rperr->root_status |= PCI_ERR_ROOT_COR_RCV;
		rperr->source_id &= 0xffff0000;
		rperr->source_id |= PCI_DEVID(einj->bus, devfn);
	}
	if (einj->uncor_status) {
		if (rperr->root_status & PCI_ERR_ROOT_UNCOR_RCV)
			rperr->root_status |= PCI_ERR_ROOT_MULTI_UNCOR_RCV;
		if (sever & einj->uncor_status) {
			rperr->root_status |= PCI_ERR_ROOT_FATAL_RCV;
			if (!(rperr->root_status & PCI_ERR_ROOT_UNCOR_RCV))
				rperr->root_status |= PCI_ERR_ROOT_FIRST_FATAL;
		} else
			rperr->root_status |= PCI_ERR_ROOT_NONFATAL_RCV;
		rperr->root_status |= PCI_ERR_ROOT_UNCOR_RCV;
		rperr->source_id &= 0x0000ffff;
		rperr->source_id |= PCI_DEVID(einj->bus, devfn) << 16;
	}
	spin_unlock_irqrestore(&inject_lock, flags);

	if (aer_mask_override) {
		pci_write_config_dword(dev, pos_cap_err + PCI_ERR_COR_MASK,
				       cor_mask_orig);
		pci_write_config_dword(dev, pos_cap_err + PCI_ERR_UNCOR_MASK,
				       uncor_mask_orig);
	}

	ret = pci_bus_set_aer_ops(dev->bus);
	if (ret)
		goto out_put;
	ret = pci_bus_set_aer_ops(rpdev->bus);
	if (ret)
		goto out_put;

	device = pcie_port_find_device(rpdev, PCIE_PORT_SERVICE_AER);
	if (device) {
		edev = to_pcie_device(device);
		if (!get_service_data(edev)) {
			pci_warn(edev->port, "AER service is not initialized\n");
			ret = -EPROTONOSUPPORT;
			goto out_put;
		}
		pci_info(edev->port, "Injecting errors %08x/%08x into device %s\n",
			 einj->cor_status, einj->uncor_status, pci_name(dev));
		ret = irq_inject_interrupt(edev->irq);
	} else {
		pci_err(rpdev, "AER device not found\n");
		ret = -ENODEV;
	}
out_put:
	kfree(err_alloc);
	kfree(rperr_alloc);
	pci_dev_put(dev);
	return ret;
}

static ssize_t aer_inject_write(struct file *filp, const char __user *ubuf,
				size_t usize, loff_t *off)
{
	struct aer_error_inj einj;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (usize < offsetof(struct aer_error_inj, domain) ||
	    usize > sizeof(einj))
		return -EINVAL;

	memset(&einj, 0, sizeof(einj));
	if (copy_from_user(&einj, ubuf, usize))
		return -EFAULT;

	ret = aer_inject(&einj);
	return ret ? ret : usize;
}

static const struct file_operations aer_inject_fops = {
	.write = aer_inject_write,
	.owner = THIS_MODULE,
	.llseek = noop_llseek,
};

static struct miscdevice aer_inject_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "aer_inject",
	.fops = &aer_inject_fops,
};

static int __init aer_inject_init(void)
{
	return misc_register(&aer_inject_device);
}

static void __exit aer_inject_exit(void)
{
	struct aer_error *err, *err_next;
	unsigned long flags;
	struct pci_bus_ops *bus_ops;

	misc_deregister(&aer_inject_device);

	while ((bus_ops = pci_bus_ops_pop())) {
		pci_bus_set_ops(bus_ops->bus, bus_ops->ops);
		kfree(bus_ops);
	}

	spin_lock_irqsave(&inject_lock, flags);
	list_for_each_entry_safe(err, err_next, &einjected, list) {
		list_del(&err->list);
		kfree(err);
	}
	spin_unlock_irqrestore(&inject_lock, flags);
}

module_init(aer_inject_init);
module_exit(aer_inject_exit);

MODULE_DESCRIPTION("PCIe AER software error injector");
MODULE_LICENSE("GPL");
