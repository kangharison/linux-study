// SPDX-License-Identifier: GPL-2.0
/*
 * Procfs interface for the PCI bus
 *
 * Copyright (c) 1997--1999 Martin Mares <mj@ucw.cz>
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/proc.c)은 /proc/bus/pci/* 인터페이스를 구현한다.
 * NVMe SSD도 일반 PCI endpoint로 등록되므로, 다음과 같은 NVMe 관련
 * 동작에 직접/간접적으로 영향을 준다.
 *
 *   - /proc/bus/pci/<domain>:<bus>/<dev>.<func> 형태의 장치별 config
 *     space 파일을 생성한다. NVMe 장치의 PCI config space(0x00~0xFF,
 *     확장 시 0xFFF)를 userspace에서 읽고 쓸 수 있게 한다.
 *   - lspci/setpci 등이 NVMe의 Vendor/Device ID, Class Code, BAR,
 *     ROM, IRQ, 그리고 DOE/PTM/ACS 등의 capability를 탐색할 때 사용하는
 *     통로이다.
 *   - NVMe 호스트 드라이버(drivers/nvme/host/pci.c)는 procfs를 직접
 *     호출하지 않지만, NVMe pci_dev가 생성되면 pci_proc_attach_device()
 *     이 /proc 트리에 연결하고, 제거/핫플러그 시 pci_proc_detach_device()
 *     으로 정리한다.
 *   - proc_bus_pci_mmap()을 통해 NVMe BAR(특히 BAR0의 doorbell/register
 *     영역)를 userspace에 매핑할 수 있다.
 *   - show_device()가 출력하는 /proc/bus/pci/devices 한 줄에는 NVMe
 *     장치의 bus/devfn, vendor/device, irq, BAR0~BAR5, ROM 크기 등이
 *     포함되어 대역폭/리소스 분석에 활용된다.
 *
 * 일반적인 NVMe 장치와의 연결 경로:
 *   nvme_probe (drivers/nvme/host/pci.c)
 *   -> pci_enable_device / pci_request_regions / pci_iomap
 *   -> NVMe BAR0 doorbell 매핑
 *   /proc/bus/pci/devices 또는 per-device config 파일을 통한
 *   NVMe capability/리소스 열
 *
 * 본 파일은 커널의 PCI 핵심 코드에서 NVMe endpoint 정보를 userspace로
 * 노출하는 관문(gateway) 역할을 한다.
 * ===================================================================
 */

#include <linux/init.h>        /* NVMe: 부팅 초기화 단계 매크로 제공 */
#include <linux/pci.h>         /* NVMe: PCI/pcie 구조체 및 함수 선언 포함 */
#include <linux/slab.h>        /* NVMe: 메모리 할당(kmalloc 등) 헤더 */
#include <linux/module.h>      /* NVMe: 모듈 관련 매크로 정의 */
#include <linux/proc_fs.h>     /* NVMe: procfs 생성 API 제공 */
#include <linux/seq_file.h>    /* NVMe: /proc/bus/pci/devices 출력용 seq_file */
#include <linux/capability.h>  /* NVMe: CAP_SYS_ADMIN/CAP_SYS_RAWIO 권한 확인 */
#include <linux/uaccess.h>     /* NVMe: 사용자 공간 메모리 접근 helper */
#include <linux/security.h>    /* NVMe: LOCKDOWN_PCI_ACCESS 보안 lockdown 검사 */
#include <asm/byteorder.h>     /* NVMe: CPU ↔ LE 변환 매크로 제공 */
#include "pci.h"               /* NVMe: PCI 서브시스템 날부 헤더 */

static int proc_initialized;	/* NVMe: procfs 초기화 완료 여부 플래그(0으로 시작) */

/*
 * proc_bus_pci_lseek:
 *   /proc/bus/pci/<장치> 파일의 오프셋 이동을 처리한다.
 *   NVMe 장치의 config space 크기(dev->cfg_size)를 초과하지 않도록
 *   조정한다.
 */
static loff_t proc_bus_pci_lseek(struct file *file, loff_t off, int whence)
{
	struct pci_dev *dev = pde_data(file_inode(file)); /* NVMe: proc 항목에 연결된 NVMe pci_dev 획득 */
	return fixed_size_llseek(file, off, whence, dev->cfg_size); /* NVMe: config space 크기 내에서 lseek 제한 */
}

/*
 * proc_bus_pci_read:
 *   /proc/bus/pci/<장치>에서 userspace로 PCI config space를 읽는다.
 *   NVMe 장치의 Vendor ID, Device ID, Class Code, BAR, 그리고 DOE/PTM
 *   등의 capability가 이 통로로 노출될 수 있다.
 */
static ssize_t proc_bus_pci_read(struct file *file, char __user *buf,
				 size_t nbytes, loff_t *ppos)
{
	struct pci_dev *dev = pde_data(file_inode(file)); /* NVMe: proc 항목에 연결된 NVMe pci_dev 획득 */
	unsigned int pos = *ppos; /* NVMe: config space 내 현재 읽기 위치 */
	unsigned int cnt, size;   /* NVMe: 복사할 바이트 수와 허용 config space 크기 */

	/*
	 * Normal users can read only the standardized portion of the
	 * configuration space as several chips lock up when trying to read
	 * undefined locations (think of Intel PIIX4 as a typical example).
	 */

	if (capable(CAP_SYS_ADMIN)) /* NVMe: 관리자면 전체 config space 접근 가능 */
		size = dev->cfg_size; /* NVMe: NVMe 장치의 전체 config space 크기 사용 */
	else if (dev->hdr_type == PCI_HEADER_TYPE_CARDBUS) /* NVMe: CardBus 헤더이면 */
		size = 128; /* NVMe: 128바이트만 허용 */
	else
		size = 64; /* NVMe: 일반 PCI 장치(NVMe 포함)는 표준 64바이트 허용 */

	if (pos >= size) /* NVMe: 읽기 위치가 허용 범위를 벗어나면 */
		return 0; /* NVMe: EOF(더 이상 읽을 데이터 없음) 반환 */
	if (nbytes >= size) /* NVMe: 요청 크기가 전체 config space 이상이면 */
		nbytes = size; /* NVMe: 최대 허용 크기로 제한 */
	if (pos + nbytes > size) /* NVMe: 끝을 넘어서 읽으려 하면 */
		nbytes = size - pos; /* NVMe: 남은 크기만큼만 읽도록 조정 */
	cnt = nbytes; /* NVMe: 실제 복사해야 할 남은 바이트 수 */

	if (!access_ok(buf, cnt)) /* NVMe: userspace 버퍼가 쓰기 가능한지 검사 */
		return -EINVAL; /* NVMe: 버퍼가 유효하지 않으면 오류 반환 */

	pci_config_pm_runtime_get(dev); /* NVMe: config space 접근 중 NVMe 장치가 runtime suspend되지 않도록 참조 증가 */

	if ((pos & 1) && cnt) { /* NVMe: 홀수 오프셋에서 시작하면 1바이트 정렬 처리 */
		unsigned char val; /* NVMe: 1바이트 읽기용 임시 변수 */
		pci_user_read_config_byte(dev, pos, &val); /* NVMe: NVMe config space에서 1바이트 읽기 */
		__put_user(val, buf); /* NVMe: 읽은 바이트를 userspace 버퍼에 기록 */
		buf++; /* NVMe: userspace 버퍼 포인터 1바이트 전진 */
		pos++; /* NVMe: config space 오프셋 1바이트 전진 */
		cnt--; /* NVMe: 남은 바이트 수 감소 */
	}

	if ((pos & 3) && cnt > 2) { /* NVMe: 2바이트 경계에 정렬되어 있지 않으면 word 단위 처리 */
		unsigned short val; /* NVMe: 2바이트 읽기용 임시 변수 */
		pci_user_read_config_word(dev, pos, &val); /* NVMe: NVMe config space에서 2바이트 읽기 */
		__put_user(cpu_to_le16(val), (__le16 __user *) buf); /* NVMe: LE16으로 변환해 userspace에 기록 */
		buf += 2; /* NVMe: userspace 버퍼 포인터 2바이트 전진 */
		pos += 2; /* NVMe: config space 오프셋 2바이트 전진 */
		cnt -= 2; /* NVMe: 남은 바이트 수 2 감소 */
	}

	while (cnt >= 4) { /* NVMe: 4바이트 단위로 남은 config space를 읽음 */
		unsigned int val; /* NVMe: 4바이트 읽기용 임시 변수 */
		pci_user_read_config_dword(dev, pos, &val); /* NVMe: NVMe config space에서 4바이트 읽기 */
		__put_user(cpu_to_le32(val), (__le32 __user *) buf); /* NVMe: LE32로 변환해 userspace에 기록 */
		buf += 4; /* NVMe: userspace 버퍼 포인터 4바이트 전진 */
		pos += 4; /* NVMe: config space 오프셋 4바이트 전진 */
		cnt -= 4; /* NVMe: 남은 바이트 수 4 감소 */
		cond_resched(); /* NVMe: 긴 읽기 중 스케줄러에게 양보 */
	}

	if (cnt >= 2) { /* NVMe: 2바이트 이하로 남았다면 word 단위 처리 */
		unsigned short val; /* NVMe: 2바이트 읽기용 임시 변수 */
		pci_user_read_config_word(dev, pos, &val); /* NVMe: NVMe config space에서 2바이트 읽기 */
		__put_user(cpu_to_le16(val), (__le16 __user *) buf); /* NVMe: LE16로 변환해 userspace에 기록 */
		buf += 2; /* NVMe: userspace 버퍼 포인터 2바이트 전진 */
		pos += 2; /* NVMe: config space 오프셋 2바이트 전진 */
		cnt -= 2; /* NVMe: 남은 바이트 수 2 감소 */
	}

	if (cnt) { /* NVMe: 마지막 1바이트가 남아 있으면 */
		unsigned char val; /* NVMe: 1바이트 읽기용 임시 변수 */
		pci_user_read_config_byte(dev, pos, &val); /* NVMe: NVMe config space에서 마지막 1바이트 읽기 */
		__put_user(val, buf); /* NVMe: userspace 버퍼에 기록 */
		pos++; /* NVMe: config space 오프셋 1바이트 전진 */
	}

	pci_config_pm_runtime_put(dev); /* NVMe: config space 접근 종료, runtime PM 참조 감소 */

	*ppos = pos; /* NVMe: 파일 오프셋을 마지막으로 읽은 위치로 갱신 */
	return nbytes; /* NVMe: userspace에 복사한 총 바이트 수 반환 */
}

/*
 * proc_bus_pci_write:
 *   /proc/bus/pci/<장치>를 통해 userspace가 NVMe 장치의 config space에
 *   직접 쓴다. capability 활성화, 레지스터 변경, DOE/PTM 관련 설정 등에
 *   사용될 수 있으므로 보안 lockdown과 권한 검사가 중요하다.
 */
static ssize_t proc_bus_pci_write(struct file *file, const char __user *buf,
				  size_t nbytes, loff_t *ppos)
{
	struct inode *ino = file_inode(file); /* NVMe: proc 파일의 inode 획득 */
	struct pci_dev *dev = pde_data(ino); /* NVMe: inode에 연결된 NVMe pci_dev 획득 */
	int pos = *ppos; /* NVMe: config space 내 현재 쓰기 위치 */
	int size = dev->cfg_size; /* NVMe: NVMe 장치의 전체 config space 크기 */
	int cnt, ret; /* NVMe: 복사할 바이트 수와 반환값 저장 */

	ret = security_locked_down(LOCKDOWN_PCI_ACCESS); /* NVMe: PCI config space 쓰기 lockdown 상태 검사 */
	if (ret) /* NVMe: lockdown이 활성화되어 있으면 */
		return ret; /* NVMe: lockdown 오류 반환 */

	if (pos >= size) /* NVMe: 쓰기 위치가 config space를 벗어나면 */
		return 0; /* NVMe: 쓸 데이터 없음 반환 */
	if (nbytes >= size) /* NVMe: 요청 크기가 전체 config space 이상이면 */
		nbytes = size; /* NVMe: 최대 허용 크기로 제한 */
	if (pos + nbytes > size) /* NVMe: 끝을 넘어서 쓰려 하면 */
		nbytes = size - pos; /* NVMe: 남은 크기만큼만 쓰도록 조정 */
	cnt = nbytes; /* NVMe: 실제 복사해야 할 남은 바이트 수 */

	if (!access_ok(buf, cnt)) /* NVMe: userspace 버퍼가 읽기 가능한지 검사 */
		return -EINVAL; /* NVMe: 버퍼가 유효하지 않으면 오류 반환 */

	pci_config_pm_runtime_get(dev); /* NVMe: 쓰기 동안 NVMe 장치의 runtime suspend 방지 */

	if ((pos & 1) && cnt) { /* NVMe: 홀수 오프셋에서 시작하면 1바이트 정렬 처리 */
		unsigned char val; /* NVMe: 1바이트 쓰기용 임시 변수 */
		__get_user(val, buf); /* NVMe: userspace 버퍼에서 1바이트 가져오기 */
		pci_user_write_config_byte(dev, pos, val); /* NVMe: NVMe config space에 1바이트 쓰기 */
		buf++; /* NVMe: userspace 버퍼 포인터 1바이트 전진 */
		pos++; /* NVMe: config space 오프셋 1바이트 전진 */
		cnt--; /* NVMe: 남은 바이트 수 감소 */
	}

	if ((pos & 3) && cnt > 2) { /* NVMe: 4바이트 정렬이 안 된 상태에서 2바이트 이상 남았으면 */
		__le16 val; /* NVMe: LE16 값을 저장할 임시 변수 */
		__get_user(val, (__le16 __user *) buf); /* NVMe: userspace에서 LE16 값 가져오기 */
		pci_user_write_config_word(dev, pos, le16_to_cpu(val)); /* NVMe: CPU 형태로 변환해 NVMe config space에 2바이트 쓰기 */
		buf += 2; /* NVMe: userspace 버퍼 포인터 2바이트 전진 */
		pos += 2; /* NVMe: config space 오프셋 2바이트 전진 */
		cnt -= 2; /* NVMe: 남은 바이트 수 2 감소 */
	}

	while (cnt >= 4) { /* NVMe: 4바이트 단위로 남은 config space에 쓰기 */
		__le32 val; /* NVMe: LE32 값을 저장할 임시 변수 */
		__get_user(val, (__le32 __user *) buf); /* NVMe: userspace에서 LE32 값 가져오기 */
		pci_user_write_config_dword(dev, pos, le32_to_cpu(val)); /* NVMe: CPU 형태로 변환해 NVMe config space에 4바이트 쓰기 */
		buf += 4; /* NVMe: userspace 버퍼 포인터 4바이트 전진 */
		pos += 4; /* NVMe: config space 오프셋 4바이트 전진 */
		cnt -= 4; /* NVMe: 남은 바이트 수 4 감소 */
	}

	if (cnt >= 2) { /* NVMe: 2바이트가 남았다면 word 단위 처리 */
		__le16 val; /* NVMe: LE16 값을 저장할 임시 변수 */
		__get_user(val, (__le16 __user *) buf); /* NVMe: userspace에서 LE16 값 가져오기 */
		pci_user_write_config_word(dev, pos, le16_to_cpu(val)); /* NVMe: CPU 형태로 변환해 NVMe config space에 2바이트 쓰기 */
		buf += 2; /* NVMe: userspace 버퍼 포인터 2바이트 전진 */
		pos += 2; /* NVMe: config space 오프셋 2바이트 전진 */
		cnt -= 2; /* NVMe: 남은 바이트 수 2 감소 */
	}

	if (cnt) { /* NVMe: 마지막 1바이트가 남아 있으면 */
		unsigned char val; /* NVMe: 1바이트 쓰기용 임시 변수 */
		__get_user(val, buf); /* NVMe: userspace 버퍼에서 1바이트 가져오기 */
		pci_user_write_config_byte(dev, pos, val); /* NVMe: NVMe config space에 마지막 1바이트 쓰기 */
		pos++; /* NVMe: config space 오프셋 1바이트 전진 */
	}

	pci_config_pm_runtime_put(dev); /* NVMe: config space 쓰기 종료, runtime PM 참조 감소 */

	*ppos = pos; /* NVMe: 파일 오프셋을 마지막으로 쓴 위치로 갱신 */
	i_size_write(ino, dev->cfg_size); /* NVMe: inode 파일 크기를 NVMe config space 크기로 갱신 */
	return nbytes; /* NVMe: userspace에서 받은 총 바이트 수 반환 */
}

#ifdef HAVE_PCI_MMAP
/*
 * pci_filp_private:
 *   /proc/bus/pci/<장치> 파일을 open할 때 할당되는 사설 구조체이다.
 *   NVMe BAR(예: BAR0 doorbell/register 영역)를 mmap할 때 I/O인지
 *   메모리인지, write-combine인지 결정한다.
 */
struct pci_filp_private {
	enum pci_mmap_state mmap_state; /* NVMe: pci_mmap_io 또는 pci_mmap_mem 상태 저장 */
	int write_combine;              /* NVMe: write-combine 매핑 사용 여부 저장 */
};
#endif /* HAVE_PCI_MMAP */

/*
 * proc_bus_pci_ioctl:
 *   /proc/bus/pci/<장치> 파일에 대한 ioctl을 처리한다.
 *   NVMe BAR mmap 전 I/O 매핑인지 메모리 매핑인지, write-combine
 *   사용 여부를 설정할 수 있다.
 */
static long proc_bus_pci_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct pci_dev *dev = pde_data(file_inode(file)); /* NVMe: proc 항목에 연결된 NVMe pci_dev 획득 */
#ifdef HAVE_PCI_MMAP
	struct pci_filp_private *fpriv = file->private_data; /* NVMe: 파일 open 시 할당된 mmap 설정 구조체 획득 */
#endif /* HAVE_PCI_MMAP */
	int ret = 0; /* NVMe: ioctl 결과 초기화 */

	ret = security_locked_down(LOCKDOWN_PCI_ACCESS); /* NVMe: PCI 접근 lockdown 상태 검사 */
	if (ret) /* NVMe: lockdown이면 */
		return ret; /* NVMe: lockdown 오류 반환 */

	switch (cmd) { /* NVMe: ioctl 명령 분기 */
	case PCIIOC_CONTROLLER: /* NVMe: 장치가 속한 PCI domain 번호 요청 */
		ret = pci_domain_nr(dev->bus); /* NVMe: NVMe 장치의 PCI domain 번호 반환 */
		break; /* NVMe: domain 번호 조회 완료 */

#ifdef HAVE_PCI_MMAP
	case PCIIOC_MMAP_IS_IO: /* NVMe: 이후 mmap을 PCI I/O 공간으로 설정 */
		if (!arch_can_pci_mmap_io()) /* NVMe: 아키텍처가 PCI I/O mmap을 지원하는지 확인 */
			return -EINVAL; /* NVMe: 지원하지 않으면 오류 반환 */
		fpriv->mmap_state = pci_mmap_io; /* NVMe: mmap 상태를 I/O로 설정 */
		break; /* NVMe: I/O mmap 설정 완료 */

	case PCIIOC_MMAP_IS_MEM: /* NVMe: 이후 mmap을 PCI 메모리 공간으로 설정 */
		fpriv->mmap_state = pci_mmap_mem; /* NVMe: mmap 상태를 메모리로 설정 */
		break; /* NVMe: 메모리 mmap 설정 완료 */

	case PCIIOC_WRITE_COMBINE: /* NVMe: 이후 mmap에 write-combine 속성 적용 요청 */
		if (arch_can_pci_mmap_wc()) { /* NVMe: 아키텍처가 WC mmap을 지원하는지 확인 */
			if (arg) /* NVMe: arg가 0이 아니면 */
				fpriv->write_combine = 1; /* NVMe: write-combine 활성화 */
			else
				fpriv->write_combine = 0; /* NVMe: write-combine 비활성화 */
			break; /* NVMe: WC 설정 완료 */
		}
		/* If arch decided it can't, fall through... */
		fallthrough; /* NVMe: 지원하지 않으면 default로 넘어감 */
#endif /* HAVE_PCI_MMAP */
	default: /* NVMe: 알 수 없는 ioctl 명령 */
		ret = -EINVAL; /* NVMe: 잘못된 명령 오류 반환 */
		break; /* NVMe: default 처리 완료 */
	}

	return ret; /* NVMe: ioctl 처리 결과 반환 */
}

#ifdef HAVE_PCI_MMAP
/*
 * proc_bus_pci_mmap:
 *   /proc/bus/pci/<장치> 파일에 대해 mmap을 수행한다.
 *   NVMe의 BAR0 등 doorbell/register 영역을 userspace에 직접 매핑할
 *   때 사용될 수 있다. 보안상 CAP_SYS_RAWIO와 lockdown 검사를 거친다.
 */
static int proc_bus_pci_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct pci_dev *dev = pde_data(file_inode(file)); /* NVMe: proc 항목에 연결된 NVMe pci_dev 획득 */
	struct pci_filp_private *fpriv = file->private_data; /* NVMe: open 시 설정한 mmap 상태 및 WC 플래그 획득 */
	resource_size_t start, end; /* NVMe: 사용자가 요청한 bus 주소 범위 저장 */
	int i, ret, write_combine = 0, res_bit = IORESOURCE_MEM; /* NVMe: BAR 인덱스, 결과, WC 플래그, 리소스 타입 초기화 */

	if (!capable(CAP_SYS_RAWIO) || /* NVMe: 원시 I/O 권한이 없거나 */
	    security_locked_down(LOCKDOWN_PCI_ACCESS)) /* NVMe: PCI 접근 lockdown 상태면 */
		return -EPERM; /* NVMe: 접근 거부 반환 */

	/* Skip devices with non-mappable BARs */
	if (dev->non_mappable_bars) /* NVMe: NVMe 장치의 BAR가 mmap 불가능으로 표시되면 */
		return -EINVAL; /* NVMe: mmap 거부 */

	if (fpriv->mmap_state == pci_mmap_io) { /* NVMe: I/O 공간 mmap이 요청된 경우 */
		if (!arch_can_pci_mmap_io()) /* NVMe: 아키텍처 지원 여부 확인 */
			return -EINVAL; /* NVMe: 지원하지 않으면 오류 반환 */
		res_bit = IORESOURCE_IO; /* NVMe: 찾을 리소스 타입을 I/O로 변경 */
	}

	/* Make sure the caller is mapping a real resource for this device */
	for (i = 0; i < PCI_STD_NUM_BARS; i++) { /* NVMe: NVMe 장치의 BAR0~BAR5를 순회 */
		if (dev->resource[i].flags & res_bit && /* NVMe: 현재 BAR가 요청한 리소스 타입인지 확인 */
		    pci_mmap_fits(dev, i, vma,  PCI_MMAP_PROCFS)) /* NVMe: mmap 영역이 이 BAR에 맞는지 검사 */
			break; /* NVMe: 적합한 BAR를 찾으면 순회 종료 */
	}

	if (i >= PCI_STD_NUM_BARS) /* NVMe: 표준 BAR 중 매칭되는 것이 없으면 */
		return -ENODEV; /* NVMe: 해당 장치 리소스 없음 반환 */

	if (fpriv->mmap_state == pci_mmap_mem && /* NVMe: 메모리 mmap이고 */
	    fpriv->write_combine) { /* NVMe: write-combine이 요청되었으면 */
		if (dev->resource[i].flags & IORESOURCE_PREFETCH) /* NVMe: 해당 BAR가 prefetchable이면 */
			write_combine = 1; /* NVMe: write-combine 활성화 */
		else
			return -EINVAL; /* NVMe: prefetchable이 아니면 WC 사용 불가 */
	}

	if (dev->resource[i].flags & IORESOURCE_MEM && /* NVMe: 메모리 BAR이고 */
	    iomem_is_exclusive(dev->resource[i].start)) /* NVMe: 독점적으로 예약된 메모리 영역이면 */
		return -EINVAL; /* NVMe: mmap 거부 */

	pci_resource_to_user(dev, i, &dev->resource[i], &start, &end); /* NVMe: 커널 날부 리소스를 사용자 공간 주소로 변환 */

	/* Adjust vm_pgoff to be the offset within the resource */
	vma->vm_pgoff -= start >> PAGE_SHIFT; /* NVMe: mmap 오프셋을 BAR 내 상대 위치로 조정 */
	ret = pci_mmap_resource_range(dev, i, vma, /* NVMe: 실제 PCI 리소스 매핑 수행 */
			      fpriv->mmap_state, write_combine);
	if (ret < 0) /* NVMe: 매핑 실패 시 */
		return ret; /* NVMe: 오류 반환 */

	return 0; /* NVMe: mmap 성공 반환 */
}

/*
 * proc_bus_pci_open:
 *   /proc/bus/pci/<장치> 파일을 열 때 호출된다.
 *   NVMe BAR mmap을 위한 사설 구조체를 할당하고 기본값을 설정한다.
 */
static int proc_bus_pci_open(struct inode *inode, struct file *file)
{
	struct pci_filp_private *fpriv = kmalloc_obj(*fpriv); /* NVMe: 파일별 mmap 설정 구조체 할당 */

	if (!fpriv) /* NVMe: 메모리 할당 실패 시 */
		return -ENOMEM; /* NVMe: 메모리 부족 오류 반환 */

	fpriv->mmap_state = pci_mmap_io; /* NVMe: 기본 mmap 상태를 I/O로 설정 */
	fpriv->write_combine = 0;        /* NVMe: 기본적으로 write-combine 비활성화 */

	file->private_data = fpriv; /* NVMe: 파일 구조체에 할당한 구조체 연결 */
	file->f_mapping = iomem_get_mapping(); /* NVMe: iomem 매핑 객체를 파일에 연결 */

	return 0; /* NVMe: open 성공 반환 */
}

/*
 * proc_bus_pci_release:
 *   /proc/bus/pci/<장치> 파일을 닫을 때 호출된다.
 *   open 시 할당한 NVMe mmap 설정 구조체를 해제한다.
 */
static int proc_bus_pci_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data); /* NVMe: 파일 사설 구조체 메모리 해제 */
	file->private_data = NULL; /* NVMe: dangling 포인터 방지를 위해 NULL 설정 */

	return 0; /* NVMe: release 성공 반환 */
}
#endif /* HAVE_PCI_MMAP */

/*
 * proc_bus_pci_ops:
 *   /proc/bus/pci/<장치> 파일에 연결된 proc_ops 구조체이다.
 *   NVMe 장치의 config space 읽기/쓰기, ioctl, mmap 등이 이 ops를 통해
 *   userspace로 노출된다.
 */
static const struct proc_ops proc_bus_pci_ops = {
	.proc_lseek	= proc_bus_pci_lseek, /* NVMe: 파일 오프셋 조정 함수 연결 */
	.proc_read	= proc_bus_pci_read,  /* NVMe: config space 읽기 함수 연결 */
	.proc_write	= proc_bus_pci_write, /* NVMe: config space 쓰기 함수 연결 */
	.proc_ioctl	= proc_bus_pci_ioctl, /* NVMe: ioctl 처리 함수 연결 */
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = proc_bus_pci_ioctl, /* NVMe: 32비트 호환 ioctl 처리 함수 연결 */
#endif
#ifdef HAVE_PCI_MMAP
	.proc_open	= proc_bus_pci_open,    /* NVMe: 파일 open 함수 연결 */
	.proc_release	= proc_bus_pci_release, /* NVMe: 파일 release 함수 연결 */
	.proc_mmap	= proc_bus_pci_mmap,    /* NVMe: mmap 함수 연결 */
#ifdef HAVE_ARCH_PCI_GET_UNMAPPED_AREA
	.proc_get_unmapped_area = get_pci_unmapped_area, /* NVMe: mmap을 위한 가상 주소 영역 확보 함수 연결 */
#endif /* HAVE_ARCH_PCI_GET_UNMAPPED_AREA */
#endif /* HAVE_PCI_MMAP */
};

/*
 * pci_seq_start / pci_seq_next / pci_seq_stop:
 *   /proc/bus/pci/devices 파일을 위한 seq_file 반복자(iterator)이다.
 *   시스템의 모든 PCI 장치(NVMe SSD 포함)를 순회하며 show_device()로
 *   한 줄씩 출력한다.
 */
/* iterator */
static void *pci_seq_start(struct seq_file *m, loff_t *pos)
{
	struct pci_dev *dev = NULL; /* NVMe: 순회 시작 시 NULL로 초기화 */
	loff_t n = *pos;            /* NVMe: 출력할 장치의 인덱스(시작 위치) */

	for_each_pci_dev(dev) { /* NVMe: 등록된 모든 PCI 장치(NVMe 포함)를 순회 */
		if (!n--) /* NVMe: 시작 위치만큼 걸러낸 후 */
			break; /* NVMe: 해당 위치의 장치를 반환하기 위해 순회 중단 */
	}
	return dev; /* NVMe: *pos에 해당하는 pci_dev 반환(끝이면 NULL) */
}

static void *pci_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct pci_dev *dev = v; /* NVMe: 현재 출력 중인 NVMe/PCI 장치 */

	(*pos)++; /* NVMe: 다음 장치 인덱스로 이동 */
	dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev); /* NVMe: 다음 PCI 장치 참조 획득(현재 장치 참조는 자동 해제) */
	return dev; /* NVMe: 다음 pci_dev 반환(없으면 NULL) */
}

static void pci_seq_stop(struct seq_file *m, void *v)
{
	if (v) { /* NVMe: 현재 항목이 유효하면 */
		struct pci_dev *dev = v; /* NVMe: 반복자에서 전달받은 pci_dev */
		pci_dev_put(dev); /* NVMe: pci_dev 참조 카운트 감소(메모리 누수 방지) */
	}
}

/*
 * show_device:
 *   /proc/bus/pci/devices 파일에서 각 PCI 장치(NVMe 포함)를 한 줄로
 *   출력한다. bus/devfn, vendor/device, irq, BAR0~BAR5 및 ROM 주소/크기,
 *   바인딩된 드라이버 이름이 포함된다.
 */
static int show_device(struct seq_file *m, void *v)
{
	const struct pci_dev *dev = v; /* NVMe: 출력할 NVMe/PCI 장치 */
	const struct pci_driver *drv;  /* NVMe: 장치에 바인딩된 PCI 드라이버 포인터 */
	int i;                         /* NVMe: BAR/ROM 반복자 */

	if (dev == NULL) /* NVMe: 유효하지 않은 장치이면 */
		return 0; /* NVMe: 아무것도 출력하지 않음 */

	drv = pci_dev_driver(dev); /* NVMe: 이 장치에 바인딩된 드라이버(예: nvme) 획득 */
	seq_printf(m, "%02x%02x\t%04x%04x\t%x", /* NVMe: bus/devfn, vendor, device, irq 출력 */
			dev->bus->number,
			dev->devfn,
			dev->vendor,
			dev->device,
			dev->irq);

	/* only print standard and ROM resources to preserve compatibility */
	for (i = 0; i <= PCI_ROM_RESOURCE; i++) { /* NVMe: BAR0~BAR5와 ROM 리소스를 순회 */
		resource_size_t start, end; /* NVMe: 사용자 공간에 보여줄 리소스 주소 */
		pci_resource_to_user(dev, i, &dev->resource[i], &start, &end); /* NVMe: 커널 리소스를 bus 주소로 변환 */
		seq_printf(m, "\t%16llx", /* NVMe: 16진수 리소스 주소/플래그 출력 */
			(unsigned long long)(start |
			(dev->resource[i].flags & PCI_REGION_FLAG_MASK)));
	}
	for (i = 0; i <= PCI_ROM_RESOURCE; i++) { /* NVMe: BAR0~BAR5와 ROM의 크기를 순회 */
		resource_size_t start, end; /* NVMe: 사용자 공간에 보여줄 리소스 주소 */
		pci_resource_to_user(dev, i, &dev->resource[i], &start, &end); /* NVMe: 커널 리소스를 bus 주소로 변환 */
		seq_printf(m, "\t%16llx", /* NVMe: 각 리소스의 크기 출력 */
			dev->resource[i].start < dev->resource[i].end ?
			(unsigned long long)(end - start) + 1 : 0);
	}
	seq_putc(m, '\t'); /* NVMe: 필드 사이 탭 구분자 출력 */
	if (drv) /* NVMe: 드라이버가 바인딩되어 있으면 */
		seq_puts(m, drv->name); /* NVMe: 드라이버 이름(예: "nvme") 출력 */
	seq_putc(m, '\n'); /* NVMe: 줄바꿈 출력 */
	return 0; /* NVMe: 출력 완료 */
}

/*
 * proc_bus_pci_devices_op:
 *   /proc/bus/pci/devices 파일의 seq_operations 이다.
 *   NVMe 장치를 포함한 모든 PCI 장치를 나열할 때 사용된다.
 */
static const struct seq_operations proc_bus_pci_devices_op = {
	.start	= pci_seq_start, /* NVMe: 순회 시작 함수 연결 */
	.next	= pci_seq_next,  /* NVMe: 다음 장치로 이동 함수 연결 */
	.stop	= pci_seq_stop,  /* NVMe: 순회 종료 및 참조 해제 함수 연결 */
	.show	= show_device    /* NVMe: 장치 한 줄 출력 함수 연결 */
};

static struct proc_dir_entry *proc_bus_pci_dir; /* NVMe: /proc/bus/pci 디렉터리 항목 포인터 */

/*
 * pci_proc_attach_device:
 *   주어진 PCI 장치(NVMe 포함)에 대해 /proc/bus/pci/<bus>/<dev>.<func>
 *   파일을 생성하고 proc_ops를 연결한다. NVMe pci_dev가 등록될 때 호출된다.
 */
int pci_proc_attach_device(struct pci_dev *dev)
{
	struct pci_bus *bus = dev->bus; /* NVMe: NVMe 장치가 연결된 PCI bus */
	struct proc_dir_entry *e;       /* NVMe: 생성된 proc 파일 항목 */
	char name[16];                  /* NVMe: bus/장치 이름 버퍼 */

	if (!proc_initialized) /* NVMe: procfs 초기화가 아직 안 되었으면 */
		return -EACCES; /* NVMe: 접근 불가 오류 반환 */

	if (!bus->procdir) { /* NVMe: 이 bus의 proc 디렉터리가 아직 없으면 */
		if (pci_proc_domain(bus)) { /* NVMe: domain 번호를 proc 경로에 표시해야 하면 */
			sprintf(name, "%04x:%02x", pci_domain_nr(bus), /* NVMe: "domain:bus" 형식 이름 생성 */
				bus->number);
		} else { /* NVMe: domain 표시가 필요 없으면 */
			sprintf(name, "%02x", bus->number); /* NVMe: "bus" 형식 이름 생성 */
		}
		bus->procdir = proc_mkdir(name, proc_bus_pci_dir); /* NVMe: /proc/bus/pci/<bus> 디렉터리 생성 */
		if (!bus->procdir) /* NVMe: 디렉터리 생성 실패 시 */
			return -ENOMEM; /* NVMe: 메모리 부족 오류 반환 */
	}

	sprintf(name, "%02x.%x", PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn)); /* NVMe: "slot.func" 형식 파일 이름 생성 */
	e = proc_create_data(name, S_IFREG | S_IRUGO | S_IWUSR, bus->procdir, /* NVMe: bus 디렉터리 아래 장치별 파일 생성 */
			     &proc_bus_pci_ops, dev);
	if (!e) /* NVMe: proc 파일 생성 실패 시 */
		return -ENOMEM; /* NVMe: 메모리 부족 오류 반환 */
	proc_set_size(e, dev->cfg_size); /* NVMe: proc 파일 크기를 NVMe config space 크기로 설정 */
	dev->procent = e; /* NVMe: pci_dev에 proc 항목 연결(해제 시 사용) */

	return 0; /* NVMe: proc 파일 생성 및 연결 완료 */
}

/*
 * pci_proc_detach_device:
 *   PCI 장치(NVMe 포함)가 제거되거나 핫플러그로 빠질 때 /proc 파일을
 *   삭제한다.
 */
int pci_proc_detach_device(struct pci_dev *dev)
{
	proc_remove(dev->procent); /* NVMe: 장치에 연결된 proc 파일 제거 */
	dev->procent = NULL;       /* NVMe: pci_dev의 proc 항목 포인터 초기화 */
	return 0;                  /* NVMe: 제거 완료 */
}

/*
 * pci_proc_detach_bus:
 *   PCI bus가 제거될 때 해당 bus의 /proc 디렉터리를 삭제한다.
 *   NVMe 장치가 연결된 bus가 사라질 때 호출될 수 있다.
 */
int pci_proc_detach_bus(struct pci_bus *bus)
{
	proc_remove(bus->procdir); /* NVMe: bus의 proc 디렉터리 제거 */
	return 0;                  /* NVMe: 제거 완료 */
}

/*
 * pci_proc_init:
 *   /proc/bus/pci 디렉터리와 /proc/bus/pci/devices를 생성하고, 이미
 *   등록된 모든 PCI 장치(NVMe SSD 포함)에 대해 per-device proc 파일을
 *   만든다. device_initcall 시점에 실행된다.
 */
static int __init pci_proc_init(void)
{
	struct pci_dev *dev = NULL; /* NVMe: 순회용 pci_dev 포인터 초기화 */
	proc_bus_pci_dir = proc_mkdir("bus/pci", NULL); /* NVMe: /proc/bus/pci 디렉터리 생성 */
	proc_create_seq("devices", 0, proc_bus_pci_dir, /* NVMe: /proc/bus/pci/devices seq 파일 생성 */
		    &proc_bus_pci_devices_op);
	proc_initialized = 1; /* NVMe: procfs 초기화 완료 플래그 설정 */
	for_each_pci_dev(dev) /* NVMe: 이미 등록된 모든 PCI 장치(NVMe 포함)를 순회 */
		pci_proc_attach_device(dev); /* NVMe: 각 장치에 /proc/bus/pci/... 파일 생성 */

	return 0; /* NVMe: 초기화 성공 반환 */
}
device_initcall(pci_proc_init); /* NVMe: 부팅 시 장치 초기화 단계에서 pci_proc_init 실행 */
