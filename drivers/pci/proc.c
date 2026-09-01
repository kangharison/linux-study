// SPDX-License-Identifier: GPL-2.0
/*
 * Procfs interface for the PCI bus
 *
 * Copyright (c) 1997--1999 Martin Mares <mj@ucw.cz>
 */

/*
 * [한국어 설명] /proc/bus/pci 인터페이스 (proc.c)
 *
 * === 파일의 역할 ===
 * PCI 장치를 /proc 파일시스템에 노출한다. sysfs 가 생기기 전부터 있던
 * 오래된 인터페이스이며, 지금도 lspci 와 setpci 가 이것을 쓴다.
 *
 * 만드는 것은 셋이다.
 *   /proc/bus/pci/devices  - 모든 장치를 한 줄씩 나열한 텍스트 파일.
 *                            bus/devfn, vendor/device, IRQ, BAR 6개와 크기,
 *                            ROM 주소와 크기, 그리고 드라이버 이름이 담긴다.
 *   /proc/bus/pci/<bus>/   - 버스마다 디렉터리.
 *   .../<slot>.<func>      - 장치마다 파일 하나. 이 파일 자체가 그 장치의
 *                            config space 다. read 하면 config 를 읽고,
 *                            write 하면 config 에 쓴다. lseek 으로 오프셋을
 *                            정한다. mmap 으로 BAR 를 매핑할 수도 있다.
 *
 * "파일 하나 = config space 하나" 라는 설계가 이 인터페이스의 특징이다.
 * sysfs 는 값마다 파일을 따로 두지만(vendor, device, class ...), 여기서는
 * 파일 하나를 통째로 읽고 원하는 오프셋을 직접 해석한다. lspci 가
 * 임의의 capability 를 훑을 수 있는 것이 이 덕분이다.
 *
 * ioctl 도 두 개 있다 — PCIIOC_MMAP_IS_IO / PCIIOC_MMAP_IS_MEM 으로
 * 이어질 mmap 이 I/O 공간인지 메모리 공간인지 지정하고,
 * PCIIOC_WRITE_COMBINE 으로 write-combining 매핑을 요청한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록:  pci_bus_add_device() [bus.c]
 *          -> [이 파일] pci_proc_attach_device() — /proc 트리에 파일을 만든다
 * 제거:  pci_stop_dev() [remove.c]
 *          -> [이 파일] pci_proc_detach_device()
 *
 * 접근:  lspci
 *          -> open("/proc/bus/pci/00/1f.2") -> read()
 *             -> [이 파일] proc_bus_pci_read()
 *                -> pci_user_read_config_* [access.c]
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 파일 연산이므로 당연히 잠들 수 있고,
 * 하위 pci_user_read_config_* 가 리셋 중이면 대기한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: userspace(lspci, setpci, 그리고 이 인터페이스를 쓰는 옛 도구들).
 * 아래쪽: access.c 의 pci_user_read/write_config_* (차단 상태를 존중하는
 *   userspace 전용 경로), mmap.c 의 pci_mmap_page_range.
 * 옆쪽: pci-sysfs.c — 같은 정보를 다른 방식으로 노출한다. 두 인터페이스가
 *   공존하는 이유는 호환성 때문이며, 새 기능은 sysfs 에만 추가된다.
 * 공유 상태: struct pci_dev 의 procent 포인터(이 장치의 proc 항목).
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 하나도 직접 부르지 않는다(전수 확인).
 * 반대로 이 파일이 NVMe 장치를 노출한다 — NVMe SSD 도 다른 PCI 장치와
 * 똑같이 /proc/bus/pci 아래에 나타나고, lspci 로 그 config space 를
 * 들여다볼 수 있다.
 *
 * config 쓰기가 가능하다는 점은 주의할 만하다. setpci 로 NVMe 의 Command
 * 레지스터를 끄면 그 순간 드라이버가 하드웨어와 통신할 수 없게 되어
 * 진행 중인 I/O 가 전부 타임아웃난다. 그래서 이 파일의 write 경로는
 * CAP_SYS_ADMIN 을 요구한다.
 *
 * (기존 주석은 NVMe 경로로 "pci_enable_device / pci_request_regions /
 *  pci_iomap" 을 적었으나, pci_request_regions 와 pci_iomap 은
 *  drivers/nvme/ 에 호출이 0건이다. 실제로는 pci_enable_device_mem() 뒤
 *  ioremap(pci_resource_start(pdev,0), size) 로 BAR0 를 직접 매핑한다.
 *  또 그 경로는 이 파일과 아무 관계가 없어 삭제했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_proc_attach_device()  : 장치 하나를 /proc 트리에 등록한다. 버스
 *                             디렉터리가 없으면 함께 만든다.
 * pci_proc_detach_device()  : 그 반대.
 * pci_proc_detach_bus()     : 버스 디렉터리를 제거한다.
 * proc_bus_pci_read()       : config space 를 읽는다. 오프셋과 크기를 보고
 *                             dword/word/byte 접근을 조합해 채운다.
 * proc_bus_pci_write()      : config space 에 쓴다. CAP_SYS_ADMIN 필요.
 * proc_bus_pci_lseek()      : 파일 크기를 config space 크기(256 또는 4096)로
 *                             삼아 위치를 옮긴다.
 * proc_bus_pci_ioctl()      : 이어질 mmap 의 종류를 지정한다.
 * proc_bus_pci_mmap()       : BAR 를 userspace 주소 공간에 매핑한다.
 * show_device()             : /proc/bus/pci/devices 의 한 줄을 만든다.
 * pci_seq_start/next/stop() : 그 파일을 훑는 seq_file 반복자.
 */

#include <linux/init.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/module.h>      /* [한국어] MODULE_* 매크로와 모듈 참조 관리.
				* 이 파일의 파일 연산 구조체가 THIS_MODULE 을 참조해,
				* /proc 파일이 열려 있는 동안 모듈이 내려가지 않게 한다 */
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/capability.h>
#include <linux/uaccess.h>
#include <linux/security.h>
#include <asm/byteorder.h>
#include "pci.h"

static int proc_initialized;

static loff_t proc_bus_pci_lseek(struct file *file, loff_t off, int whence)
{
	struct pci_dev *dev = pde_data(file_inode(file));
	return fixed_size_llseek(file, off, whence, dev->cfg_size);
}

static ssize_t proc_bus_pci_read(struct file *file, char __user *buf,
				 size_t nbytes, loff_t *ppos)
{
	struct pci_dev *dev = pde_data(file_inode(file));
	unsigned int pos = *ppos;
	unsigned int cnt, size;

	/*
	 * Normal users can read only the standardized portion of the
	 * configuration space as several chips lock up when trying to read
	 * undefined locations (think of Intel PIIX4 as a typical example).
	 */

	if (capable(CAP_SYS_ADMIN))
		size = dev->cfg_size;
	else if (dev->hdr_type == PCI_HEADER_TYPE_CARDBUS)
		size = 128;
	else
		size = 64;

	if (pos >= size)
		return 0;
	if (nbytes >= size)
		nbytes = size;
	if (pos + nbytes > size)
		nbytes = size - pos;
	cnt = nbytes;

	if (!access_ok(buf, cnt))
		return -EINVAL;

	pci_config_pm_runtime_get(dev);

	if ((pos & 1) && cnt) {
		unsigned char val;
		pci_user_read_config_byte(dev, pos, &val);
		__put_user(val, buf);
		buf++;
		pos++;
		cnt--;
	}

	if ((pos & 3) && cnt > 2) {
		unsigned short val;
		pci_user_read_config_word(dev, pos, &val);
		__put_user(cpu_to_le16(val), (__le16 __user *) buf);
		buf += 2;
		pos += 2;
		cnt -= 2;
	}

	while (cnt >= 4) {
		unsigned int val;
		pci_user_read_config_dword(dev, pos, &val);
		__put_user(cpu_to_le32(val), (__le32 __user *) buf);
		buf += 4;
		pos += 4;
		cnt -= 4;
		cond_resched();
	}

	if (cnt >= 2) {
		unsigned short val;
		pci_user_read_config_word(dev, pos, &val);
		__put_user(cpu_to_le16(val), (__le16 __user *) buf);
		buf += 2;
		pos += 2;
		cnt -= 2;
	}

	if (cnt) {
		unsigned char val;
		pci_user_read_config_byte(dev, pos, &val);
		__put_user(val, buf);
		pos++;
	}

	pci_config_pm_runtime_put(dev);

	*ppos = pos;
	return nbytes;
}

static ssize_t proc_bus_pci_write(struct file *file, const char __user *buf,
				  size_t nbytes, loff_t *ppos)
{
	struct inode *ino = file_inode(file);
	struct pci_dev *dev = pde_data(ino);
	int pos = *ppos;
	int size = dev->cfg_size;
	int cnt, ret;

	ret = security_locked_down(LOCKDOWN_PCI_ACCESS);
	if (ret)
		return ret;

	if (pos >= size)
		return 0;
	if (nbytes >= size)
		nbytes = size;
	if (pos + nbytes > size)
		nbytes = size - pos;
	cnt = nbytes;

	if (!access_ok(buf, cnt))
		return -EINVAL;

	pci_config_pm_runtime_get(dev);

	if ((pos & 1) && cnt) {
		unsigned char val;
		__get_user(val, buf);
		pci_user_write_config_byte(dev, pos, val);
		buf++;
		pos++;
		cnt--;
	}

	if ((pos & 3) && cnt > 2) {
		__le16 val;
		__get_user(val, (__le16 __user *) buf);
		pci_user_write_config_word(dev, pos, le16_to_cpu(val));
		buf += 2;
		pos += 2;
		cnt -= 2;
	}

	while (cnt >= 4) {
		__le32 val;
		__get_user(val, (__le32 __user *) buf);
		pci_user_write_config_dword(dev, pos, le32_to_cpu(val));
		buf += 4;
		pos += 4;
		cnt -= 4;
	}

	if (cnt >= 2) {
		__le16 val;
		__get_user(val, (__le16 __user *) buf);
		pci_user_write_config_word(dev, pos, le16_to_cpu(val));
		buf += 2;
		pos += 2;
		cnt -= 2;
	}

	if (cnt) {
		unsigned char val;
		__get_user(val, buf);
		pci_user_write_config_byte(dev, pos, val);
		pos++;
	}

	pci_config_pm_runtime_put(dev);

	*ppos = pos;
	i_size_write(ino, dev->cfg_size);
	return nbytes;
}

#ifdef HAVE_PCI_MMAP
struct pci_filp_private {
	enum pci_mmap_state mmap_state;
	int write_combine;
};
#endif /* HAVE_PCI_MMAP */

static long proc_bus_pci_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct pci_dev *dev = pde_data(file_inode(file));
#ifdef HAVE_PCI_MMAP
	struct pci_filp_private *fpriv = file->private_data;
#endif /* HAVE_PCI_MMAP */
	int ret = 0;

	ret = security_locked_down(LOCKDOWN_PCI_ACCESS);
	if (ret)
		return ret;

	switch (cmd) {
	case PCIIOC_CONTROLLER:
		ret = pci_domain_nr(dev->bus);
		break;

#ifdef HAVE_PCI_MMAP
	case PCIIOC_MMAP_IS_IO:
		if (!arch_can_pci_mmap_io())
			return -EINVAL;
		fpriv->mmap_state = pci_mmap_io;
		break;

	case PCIIOC_MMAP_IS_MEM:
		fpriv->mmap_state = pci_mmap_mem;
		break;

	case PCIIOC_WRITE_COMBINE:
		if (arch_can_pci_mmap_wc()) {
			if (arg)
				fpriv->write_combine = 1;
			else
				fpriv->write_combine = 0;
			break;
		}
		/* If arch decided it can't, fall through... */
		fallthrough;
#endif /* HAVE_PCI_MMAP */
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

#ifdef HAVE_PCI_MMAP
static int proc_bus_pci_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct pci_dev *dev = pde_data(file_inode(file));
	struct pci_filp_private *fpriv = file->private_data;
	resource_size_t start, end;
	int i, ret, write_combine = 0, res_bit = IORESOURCE_MEM;

	if (!capable(CAP_SYS_RAWIO) ||
	    security_locked_down(LOCKDOWN_PCI_ACCESS))
		return -EPERM;

	/* Skip devices with non-mappable BARs */
	if (dev->non_mappable_bars)
		return -EINVAL;

	if (fpriv->mmap_state == pci_mmap_io) {
		if (!arch_can_pci_mmap_io())
			return -EINVAL;
		res_bit = IORESOURCE_IO;
	}

	/* Make sure the caller is mapping a real resource for this device */
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (dev->resource[i].flags & res_bit &&
		    pci_mmap_fits(dev, i, vma,  PCI_MMAP_PROCFS))
			break;
	}

	if (i >= PCI_STD_NUM_BARS)
		return -ENODEV;

	if (fpriv->mmap_state == pci_mmap_mem &&
	    fpriv->write_combine) {
		if (dev->resource[i].flags & IORESOURCE_PREFETCH)
			write_combine = 1;
		else
			return -EINVAL;
	}

	if (dev->resource[i].flags & IORESOURCE_MEM &&
	    iomem_is_exclusive(dev->resource[i].start))
		return -EINVAL;

	pci_resource_to_user(dev, i, &dev->resource[i], &start, &end);

	/* Adjust vm_pgoff to be the offset within the resource */
	vma->vm_pgoff -= start >> PAGE_SHIFT;
	ret = pci_mmap_resource_range(dev, i, vma,
			      fpriv->mmap_state, write_combine);
	if (ret < 0)
		return ret;

	return 0;
}

static int proc_bus_pci_open(struct inode *inode, struct file *file)
{
	struct pci_filp_private *fpriv = kmalloc_obj(*fpriv);

	if (!fpriv)
		return -ENOMEM;

	fpriv->mmap_state = pci_mmap_io;
	fpriv->write_combine = 0;

	file->private_data = fpriv;
	file->f_mapping = iomem_get_mapping();

	return 0;
}

static int proc_bus_pci_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	file->private_data = NULL;

	return 0;
}
#endif /* HAVE_PCI_MMAP */

static const struct proc_ops proc_bus_pci_ops = {
	.proc_lseek	= proc_bus_pci_lseek,
	.proc_read	= proc_bus_pci_read,
	.proc_write	= proc_bus_pci_write,
	.proc_ioctl	= proc_bus_pci_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = proc_bus_pci_ioctl,
#endif
#ifdef HAVE_PCI_MMAP
	.proc_open	= proc_bus_pci_open,
	.proc_release	= proc_bus_pci_release,
	.proc_mmap	= proc_bus_pci_mmap,
#ifdef HAVE_ARCH_PCI_GET_UNMAPPED_AREA
	.proc_get_unmapped_area = get_pci_unmapped_area,
#endif /* HAVE_ARCH_PCI_GET_UNMAPPED_AREA */
#endif /* HAVE_PCI_MMAP */
};

/* iterator */
static void *pci_seq_start(struct seq_file *m, loff_t *pos)
{
	struct pci_dev *dev = NULL;
	loff_t n = *pos;

	for_each_pci_dev(dev) {
		if (!n--)
			break;
	}
	return dev;
}

static void *pci_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct pci_dev *dev = v;

	(*pos)++;
	dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev);
	return dev;
}

static void pci_seq_stop(struct seq_file *m, void *v)
{
	if (v) {
		struct pci_dev *dev = v;
		pci_dev_put(dev);
	}
}

static int show_device(struct seq_file *m, void *v)
{
	const struct pci_dev *dev = v;
	const struct pci_driver *drv;
	int i;

	if (dev == NULL)
		return 0;

	drv = pci_dev_driver(dev);
	seq_printf(m, "%02x%02x\t%04x%04x\t%x",
			dev->bus->number,
			dev->devfn,
			dev->vendor,
			dev->device,
			dev->irq);

	/* only print standard and ROM resources to preserve compatibility */
	for (i = 0; i <= PCI_ROM_RESOURCE; i++) {
		resource_size_t start, end;
		pci_resource_to_user(dev, i, &dev->resource[i], &start, &end);
		seq_printf(m, "\t%16llx",
			(unsigned long long)(start |
			(dev->resource[i].flags & PCI_REGION_FLAG_MASK)));
	}
	for (i = 0; i <= PCI_ROM_RESOURCE; i++) {
		resource_size_t start, end;
		pci_resource_to_user(dev, i, &dev->resource[i], &start, &end);
		seq_printf(m, "\t%16llx",
			dev->resource[i].start < dev->resource[i].end ?
			(unsigned long long)(end - start) + 1 : 0);
	}
	seq_putc(m, '\t');
	if (drv)
		seq_puts(m, drv->name);
	seq_putc(m, '\n');
	return 0;
}

static const struct seq_operations proc_bus_pci_devices_op = {
	.start	= pci_seq_start,
	.next	= pci_seq_next,
	.stop	= pci_seq_stop,
	.show	= show_device
};

static struct proc_dir_entry *proc_bus_pci_dir;

int pci_proc_attach_device(struct pci_dev *dev)
{
	struct pci_bus *bus = dev->bus;
	struct proc_dir_entry *e;
	char name[16];

	if (!proc_initialized)
		return -EACCES;

	if (!bus->procdir) {
		if (pci_proc_domain(bus)) {
			sprintf(name, "%04x:%02x", pci_domain_nr(bus),
				bus->number);
		} else {
			sprintf(name, "%02x", bus->number);
		}
		bus->procdir = proc_mkdir(name, proc_bus_pci_dir);
		if (!bus->procdir)
			return -ENOMEM;
	}

	sprintf(name, "%02x.%x", PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn));
	e = proc_create_data(name, S_IFREG | S_IRUGO | S_IWUSR, bus->procdir,
			     &proc_bus_pci_ops, dev);
	if (!e)
		return -ENOMEM;
	proc_set_size(e, dev->cfg_size);
	dev->procent = e;

	return 0;
}

int pci_proc_detach_device(struct pci_dev *dev)
{
	proc_remove(dev->procent);
	dev->procent = NULL;
	return 0;
}

int pci_proc_detach_bus(struct pci_bus *bus)
{
	proc_remove(bus->procdir);
	return 0;
}

static int __init pci_proc_init(void)
{
	struct pci_dev *dev = NULL;
	proc_bus_pci_dir = proc_mkdir("bus/pci", NULL);
	proc_create_seq("devices", 0, proc_bus_pci_dir,
		    &proc_bus_pci_devices_op);
	proc_initialized = 1;
	for_each_pci_dev(dev)
		pci_proc_attach_device(dev);

	return 0;
}
device_initcall(pci_proc_init);
