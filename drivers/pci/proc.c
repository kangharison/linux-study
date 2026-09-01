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
 * 등록:  pci_bus_add_device() [bus.c:439] 안에서 bus.c:452
 *          -> [이 파일] pci_proc_attach_device() — /proc 트리에 파일을 만든다
 *             (바로 앞줄이 pci_create_sysfs_dev_files() 다. 두 인터페이스가
 *              같은 자리에서 나란히 만들어진다.)
 * 제거:  pci_stop_dev() [remove.c:103] 안에서 remove.c:111
 *          -> [이 파일] pci_proc_detach_device()
 *        pci_remove_bus() [remove.c:144] 안에서 remove.c:146
 *          -> [이 파일] pci_proc_detach_bus()
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
 * 아래쪽: access.c 의 pci_user_read_config_ 계열과 pci_user_write_config_
 *   계열 — 리셋 중이면 기다리는, userspace 전용 config 접근 경로다.
 *   그 함수들은 access.c:796-799 의 PCI_USER_READ_CONFIG /
 *   PCI_USER_WRITE_CONFIG 매크로가 만들어 낸다.
 *   그리고 mmap.c 의 pci_mmap_resource_range()(mmap.c:84)와
 *   pci_mmap_fits()(mmap.c:121).
 *   (기존 주석은 "mmap.c 의 pci_mmap_page_range" 라고 적었으나 그런 이름의
 *    함수는 이 트리에 없다. mmap.c 가 정의하는 것은 위 두 개뿐이고,
 *    이 파일이 부르는 것은 pci_mmap_resource_range() 다.)
 * 옆쪽: pci-sysfs.c — 같은 정보를 다른 방식으로 노출한다. 두 인터페이스가
 *   공존하는 이유는 호환성 때문이며, 새 기능은 sysfs 에만 추가된다.
 * 공유 상태: struct pci_dev 의 procent 포인터(이 장치의 proc 항목).
 *
 * === 주요 함수/구조체 요약 ===
 * pci_proc_init()           : device_initcall. /proc/bus/pci 디렉터리와
 *                             devices 파일을 만들고, 그 시점에 이미 열거된
 *                             장치들을 한 번에 등록한다.
 * pci_proc_attach_device()  : 장치 하나를 /proc 트리에 등록한다. 버스
 *                             디렉터리가 없으면 함께 만든다.
 * pci_proc_detach_device()  : 그 반대.
 * pci_proc_detach_bus()     : 버스 디렉터리를 제거한다.
 * proc_bus_pci_read()       : config space 를 읽는다. 오프셋 정렬에 맞춰
 *                             byte/word/dword 접근을 조합한다. 권한이 없으면
 *                             표준 헤더(64바이트, CardBus 는 128)까지만 보인다.
 * proc_bus_pci_write()      : config space 에 쓴다. 읽기의 거울상이며
 *                             권한 제한은 파일 모드(S_IWUSR)가 맡는다.
 * proc_bus_pci_lseek()      : 파일 크기를 dev->cfg_size(보통 256 또는 4096)로
 *                             삼아 위치를 옮긴다.
 * proc_bus_pci_ioctl()      : PCIIOC_CONTROLLER 로 도메인 번호를 알려 주고,
 *                             PCIIOC_MMAP_IS_IO / _IS_MEM / _WRITE_COMBINE 으로
 *                             이어질 mmap 의 종류를 지정한다.
 * proc_bus_pci_mmap()       : BAR 를 userspace 주소 공간에 매핑한다.
 *                             CAP_SYS_RAWIO 가 필요하다.
 * proc_bus_pci_open()       : 파일마다 struct pci_filp_private 를 잡는다.
 *                             ioctl 로 정한 mmap 종류를 여기에 담아 둔다.
 * proc_bus_pci_release()    : 그것을 놓는다.
 * show_device()             : /proc/bus/pci/devices 의 한 줄을 만든다.
 * pci_seq_start/next/stop() : 그 파일을 훑는 seq_file 반복자.
 *
 * struct pci_filp_private   : 열린 파일 하나에 딸린 상태. ioctl 로 정한
 *                             mmap 종류와 write-combine 요청을 담는다.
 *                             HAVE_PCI_MMAP 일 때만 존재한다.
 * proc_bus_pci_ops          : 장치 파일 하나의 파일 연산 표.
 * proc_bus_pci_devices_op   : devices 파일의 seq_file 연산 표.
 * proc_bus_pci_dir          : /proc/bus/pci 디렉터리 항목. 버스 디렉터리들의
 *                             부모다.
 * proc_initialized          : pci_proc_init() 이 끝났는지. 그 전에 열거된
 *                             장치가 attach 를 시도하는 것을 막는다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 하나도 직접 부르지 않는다(전수 확인).
 * 반대로 이 파일이 NVMe 장치를 노출한다 — NVMe SSD 도 다른 PCI 장치와
 * 똑같이 /proc/bus/pci 아래에 나타나고, lspci 로 그 config space 를
 * 들여다볼 수 있다.
 *
 * config 쓰기가 가능하다는 점은 주의할 만하다. setpci 로 NVMe 의 Command
 * 레지스터를 끄면 그 순간 드라이버가 하드웨어와 통신할 수 없게 되어
 * 진행 중인 I/O 가 전부 타임아웃난다.
 *
 * 그 쓰기를 막는 장치가 무엇인지는 정확히 알아 둘 필요가 있다.
 * proc_bus_pci_write() 자체는 capable() 을 부르지 않는다 — 확인한 검사는
 * security_locked_down(LOCKDOWN_PCI_ACCESS) 하나뿐이다(proc.c:215).
 * 실제 권한 제한은 파일 모드에서 온다. pci_proc_attach_device() 가
 * S_IRUGO | S_IWUSR 로 노드를 만들므로(proc.c:536) 쓰기는 소유자(root)만
 * 할 수 있다. capable() 을 직접 부르는 곳은 두 군데뿐이다 —
 * proc_bus_pci_read() 의 CAP_SYS_ADMIN(64/128바이트 너머를 읽을 때만,
 * proc.c:136)과 proc_bus_pci_mmap() 의 CAP_SYS_RAWIO(proc.c:345).
 *
 * (기존 주석은 NVMe 경로로 "pci_enable_device / pci_request_regions /
 *  pci_iomap" 을 적었으나, 그 세 이름 그대로는 drivers/nvme/ 에 호출이
 *  0건이다. 실제로 쓰는 것은 pci_enable_device_mem()(pci.c:4085),
 *  pci_request_mem_regions()(pci.c:4533), 그리고
 *  ioremap(pci_resource_start(pdev, 0), size)(pci.c:3001) 다.
 *  어느 쪽이든 이 파일과는 아무 관계가 없는 경로다.)
 */

/* [한국어] __init 와 device_initcall — 맨 아래 pci_proc_init() 을 부팅 초기화로 등록한다 */
#include <linux/init.h>
/* [한국어] struct pci_dev, pci_domain_nr, PCI_SLOT/PCI_FUNC, for_each_pci_dev,
 * pci_get_device, pci_dev_put — 이 파일이 다루는 대상 전부 */
#include <linux/pci.h>
/* [한국어] kmalloc_obj / kfree — proc_bus_pci_open() 이 파일마다 잡는
 * struct pci_filp_private 용 */
#include <linux/slab.h>
#include <linux/module.h>      /* [한국어] 이 파일에서 직접 쓰는 심볼은 확인되지
				* 않았다 — THIS_MODULE 도 MODULE_ 계열 매크로도
				* EXPORT_SYMBOL 도 이 파일에 나오지 않고,
				* struct proc_ops 에는 .owner 필드가 없다.
				* (기존 주석은 "파일 연산 구조체가 THIS_MODULE 을
				*  참조해 모듈이 내려가지 않게 한다" 고 적었으나
				*  근거를 찾을 수 없어 고쳤다.) 상류가 오래전부터
				* 달고 있는 포함 문이며 코드는 고치지 않는다 */
/* [한국어] proc_mkdir / proc_create_data / proc_create_seq / proc_remove /
 * proc_set_size / pde_data — /proc 항목을 만들고 없애고 되찾는 API 전부 */
#include <linux/proc_fs.h>
/* [한국어] seq_file 반복자와 seq_printf/seq_puts/seq_putc — devices 파일의 출력 경로 */
#include <linux/seq_file.h>
/* [한국어] capable() 과 CAP_SYS_ADMIN / CAP_SYS_RAWIO — 읽기 범위와 mmap 을 가르는 권한 검사 */
#include <linux/capability.h>
/* [한국어] access_ok / __put_user / __get_user — 사용자 공간 버퍼를 안전하게 다루는 도구 */
#include <linux/uaccess.h>
/* [한국어] security_locked_down() 과 LOCKDOWN_PCI_ACCESS — 커널 lockdown 정책 훅.
 * 임의 config 쓰기와 BAR mmap 은 DMA 를 아무 데나 향하게 만들 수 있어
 * root 라도 막아야 하는 경우가 있다 */
#include <linux/security.h>
/* [한국어] cpu_to_le16/32 와 le16/32_to_cpu — PCI config space 는 리틀엔디안으로
 * 정의되어 있고, 빅엔디안 아키텍처에서도 사용자에게는 그 순서로 보여야 한다 */
#include <asm/byteorder.h>
/* [한국어] PCI 코어 내부 헤더. pci_user_read_config_ 계열 선언, pci_mmap_fits(),
 * pci_config_pm_runtime_get/put, pci_proc_attach_device 계열 원형이 여기 있다 */
#include "pci.h"

/* [한국어] pci_proc_init() 이 /proc/bus/pci 디렉터리를 만들었는지.
 * 설정자: pci_proc_init() 이 트리를 다 만든 뒤 1 로 세운다.
 * 읽는 자: pci_proc_attach_device() 하나뿐 — 0 이면 부모 디렉터리가 없으므로
 *   -EACCES 로 물러난다. 그렇게 놓친 장치는 pci_proc_init() 이 마지막에
 *   for_each_pci_dev 로 다시 훑어 만회한다.
 * 값 범위: 0 또는 1. 한 번 1 이 되면 되돌아가지 않는다.
 * 동기화: 부팅 initcall 에서 한 번 쓰고 이후 읽기 전용이라 락이 없다 */
static int proc_initialized;

/* [한국어]
 * proc_bus_pci_lseek - 장치 파일 안에서 읽고 쓸 위치를 옮긴다
 *
 * @file: 열린 /proc/bus/pci/<bus>/<slot>.<func> 파일
 * @off: 이동량.  @whence: SEEK_SET / SEEK_CUR / SEEK_END
 * @return: 새 위치, 또는 음수 errno
 *
 * 이 인터페이스의 설계가 한 줄에 드러난다 — 파일 하나가 곧 config space
 * 하나이고, 파일 오프셋이 곧 config space 오프셋이다. 그래서 lseek 은
 * "파일 크기가 dev->cfg_size 로 고정된 파일" 의 일반 lseek 이면 족하다.
 * fixed_size_llseek() 이 그 관용구를 커널 공통으로 구현해 둔 것이다.
 *
 * dev->cfg_size 는 보통 256(PCI) 또는 4096(PCIe extended config space)이다.
 * pci_proc_attach_device() 가 proc_set_size() 로 같은 값을 노드에 심어 두므로
 * stat 으로 본 크기와 lseek 의 끝이 일치한다.
 *
 * pde_data(file_inode(file)) 는 proc_create_data() 에 넘겨 둔 struct pci_dev
 * 포인터를 되찾는 관용구다. 이 파일의 모든 파일 연산이 같은 방식으로 시작한다.
 *
 * 실행 컨텍스트: lseek 시스템 호출의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (lseek) → proc_ops.proc_lseek → [이 함수] → fixed_size_llseek()
 */
static loff_t proc_bus_pci_lseek(struct file *file, loff_t off, int whence)
{
	struct pci_dev *dev = pde_data(file_inode(file));
	return fixed_size_llseek(file, off, whence, dev->cfg_size);
}

/* [한국어]
 * proc_bus_pci_read - config space 를 읽어 사용자 버퍼에 채운다
 *
 * @file: 열린 장치 파일.  @buf: 사용자 공간 버퍼
 * @nbytes: 요청한 바이트 수.  @ppos: 파일 위치(읽은 만큼 전진시킨다)
 * @return: 실제로 읽은 바이트 수, 0(끝), 또는 -EINVAL
 *
 * lspci 가 장치 정보를 얻는 바로 그 경로다.
 *
 * 보이는 범위가 권한에 따라 다르다. 바로 위 원문 영어 주석이 그 이유를
 * 밝힌다 — 정의되지 않은 config 위치를 읽으면 멈춰 버리는 칩이 있고
 * (원문이 Intel PIIX4 를 예로 든다), 그래서 일반 사용자에게는 스펙이
 * 표준화한 부분만 열어 준다.
 *   CAP_SYS_ADMIN     - dev->cfg_size 전체(보통 256 또는 4096)
 *   CardBus 헤더      - 128바이트
 *   그 밖             - 64바이트(표준 헤더까지)
 *
 * 본체는 "정렬을 맞춰 가며 가장 큰 단위로 읽는" 사다리다.
 *   1) 위치가 홀수면 1바이트를 읽어 짝수로 맞춘다.
 *   2) 위치가 4의 배수가 아니고 2바이트 넘게 남았으면 2바이트를 읽어
 *      4바이트 경계로 맞춘다.
 *   3) 4바이트씩 최대한 읽는다.
 *   4) 남은 것을 2바이트, 1바이트 순으로 마무리한다.
 * 정렬을 맞추는 이유는 config 접근이 정렬된 크기로만 정의되어 있기
 * 때문이고, 큰 단위를 쓰는 이유는 config 사이클 한 번이 비싸기 때문이다.
 *
 * 읽은 값은 cpu_to_le16 / cpu_to_le32 로 리틀엔디안으로 바꿔 내보낸다.
 * PCI config space 는 리틀엔디안으로 정의되어 있고, 사용자 공간 도구가
 * 그렇게 해석하기 때문이다. 1바이트에는 변환이 필요 없다.
 *
 * pci_config_pm_runtime_get/put 이 앞뒤를 감싼다. 런타임 절전으로 D3 에
 * 들어간 장치는 config 읽기가 유효하지 않으므로 잠시 깨워 둔다.
 *
 * 4바이트 루프 안의 cond_resched() 는 4096바이트를 한 번에 읽는 경우처럼
 * 루프가 길어질 때 다른 태스크에 CPU 를 양보한다. 쓰기 쪽 루프에는 이것이
 * 없는데, 코드는 고치지 않고 이 차이만 적어 둔다.
 *
 * 실행 컨텍스트: read 시스템 호출의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   (read) → proc_ops.proc_read → [이 함수]
 *     → pci_user_read_config_byte/word/dword [access.c] → __put_user()
 */
static ssize_t proc_bus_pci_read(struct file *file, char __user *buf,
				 size_t nbytes, loff_t *ppos)
{
	struct pci_dev *dev = pde_data(file_inode(file));
	/* [한국어] 파일 위치를 config space 오프셋으로 그대로 쓴다.
	 * loff_t 를 unsigned int 로 좁히는데, 아래에서 size 와 비교해 걸러지므로
	 * 범위를 벗어난 값은 통과하지 못한다 */
	unsigned int pos = *ppos;
	/* [한국어] cnt = 아직 채우지 못한 바이트 수, size = 이 사용자에게 보여 줄 상한 */
	unsigned int cnt, size;

	/*
	 * Normal users can read only the standardized portion of the
	 * configuration space as several chips lock up when trying to read
	 * undefined locations (think of Intel PIIX4 as a typical example).
	 */

	/* [한국어] 권한이 있으면 이 장치의 config space 전체를 보여 준다.
	 * PCIe 확장 config 까지면 4096, 아니면 256 이다 */
	if (capable(CAP_SYS_ADMIN))
		/* [한국어] 상한을 실제 크기로 */
		size = dev->cfg_size;
	/* [한국어] CardBus 브리지는 헤더 형식이 달라 표준 부분이 128바이트다 */
	else if (dev->hdr_type == PCI_HEADER_TYPE_CARDBUS)
		/* [한국어] 그만큼만 */
		size = 128;
	else
		/* [한국어] 그 밖에는 표준 헤더 64바이트까지. 바로 위 원문 영어 주석이 이유를 밝힌다 —
		 * 정의되지 않은 위치를 읽으면 멈추는 칩이 있기 때문이다 */
		size = 64;

	/* [한국어] 이미 상한을 넘은 위치면 EOF 로 0 을 돌려준다 */
	if (pos >= size)
		return 0;
	/* [한국어] 요청이 상한보다 크면 */
	if (nbytes >= size)
		/* [한국어] 상한으로 자른다 */
		nbytes = size;
	/* [한국어] 위치를 더한 끝이 상한을 넘으면 */
	if (pos + nbytes > size)
		/* [한국어] 남은 만큼으로 자른다. 두 검사를 따로 두는 이유는 pos 가 0 이 아닐 때
		 * 앞의 검사만으로는 부족하기 때문이다 */
		nbytes = size - pos;
	/* [한국어] 이제부터 cnt 를 줄여 가며 채운다 */
	cnt = nbytes;

	/* [한국어] 사용자 버퍼가 정말 그 프로세스의 것인지 확인한다.
	 * 아래에서 검사를 생략하는 __put_user 를 쓰므로 여기서 한 번에 확인해 둔다 */
	if (!access_ok(buf, cnt))
		return -EINVAL;

	pci_config_pm_runtime_get(dev);

	/* [한국어] 위치가 홀수다. 1바이트를 읽어 짝수 경계로 맞춘다.
	 * config 접근은 정렬된 크기로만 정의되어 있어 이 사다리가 필요하다 */
	if ((pos & 1) && cnt) {
		/* [한국어] 1바이트 값 */
		unsigned char val;
		/* [한국어] userspace 전용 config 읽기. 리셋 중이면 끝날 때까지 기다린다 */
		pci_user_read_config_byte(dev, pos, &val);
		/* [한국어] 사용자 버퍼로 복사. 1바이트에는 엔디안 변환이 필요 없다 */
		__put_user(val, buf);
		/* [한국어] 버퍼 포인터 전진 */
		buf++;
		/* [한국어] config 오프셋 전진 */
		pos++;
		/* [한국어] 남은 바이트 감소 */
		cnt--;
	}

	/* [한국어] 위치가 4의 배수가 아니고 2바이트 넘게 남았다. 2바이트를 읽어
	 * 4바이트 경계로 맞춘다 */
	if ((pos & 3) && cnt > 2) {
		/* [한국어] 2바이트 값 */
		unsigned short val;
		/* [한국어] word 단위 config 읽기 */
		pci_user_read_config_word(dev, pos, &val);
		/* [한국어] 리틀엔디안으로 바꿔 내보낸다. 빅엔디안 아키텍처에서도 사용자 도구가
		 * PCI 스펙대로 해석할 수 있게 하려는 것이다 */
		__put_user(cpu_to_le16(val), (__le16 __user *) buf);
		/* [한국어] 버퍼 2바이트 전진 */
		buf += 2;
		/* [한국어] 오프셋 2바이트 전진 */
		pos += 2;
		/* [한국어] 남은 바이트 감소 */
		cnt -= 2;
	}

	/* [한국어] 이제 4바이트 경계에 정렬되었다. 가장 큰 단위로 최대한 읽는다.
	 * config 사이클 한 번이 비싸므로 횟수를 줄이는 것이 이득이다 */
	while (cnt >= 4) {
		/* [한국어] 4바이트 값 */
		unsigned int val;
		/* [한국어] dword 단위 config 읽기 */
		pci_user_read_config_dword(dev, pos, &val);
		/* [한국어] 리틀엔디안으로 바꿔 내보낸다 */
		__put_user(cpu_to_le32(val), (__le32 __user *) buf);
		/* [한국어] 버퍼 4바이트 전진 */
		buf += 4;
		/* [한국어] 오프셋 4바이트 전진 */
		pos += 4;
		/* [한국어] 남은 바이트 감소 */
		cnt -= 4;
		cond_resched();
	}

	/* [한국어] 4바이트로 다 채우지 못하고 2바이트 이상 남았다 */
	if (cnt >= 2) {
		/* [한국어] 2바이트 값 */
		unsigned short val;
		/* [한국어] word 단위로 읽는다 */
		pci_user_read_config_word(dev, pos, &val);
		/* [한국어] 리틀엔디안으로 내보낸다 */
		__put_user(cpu_to_le16(val), (__le16 __user *) buf);
		/* [한국어] 버퍼 전진 */
		buf += 2;
		/* [한국어] 오프셋 전진 */
		pos += 2;
		/* [한국어] 남은 바이트 감소 */
		cnt -= 2;
	}

	/* [한국어] 마지막 1바이트가 남았다 */
	if (cnt) {
		/* [한국어] 1바이트 값 */
		unsigned char val;
		/* [한국어] byte 단위로 읽는다 */
		pci_user_read_config_byte(dev, pos, &val);
		/* [한국어] 그대로 내보낸다 */
		__put_user(val, buf);
		/* [한국어] 오프셋만 전진시킨다. cnt 는 아래에서 쓰지 않으므로 줄이지 않는다 */
		pos++;
	}

	pci_config_pm_runtime_put(dev);

	*ppos = pos;
	return nbytes;
}

/* [한국어]
 * proc_bus_pci_write - 사용자 버퍼의 내용을 config space 에 쓴다
 *
 * @file: 열린 장치 파일.  @buf: 사용자 공간 버퍼
 * @nbytes: 요청한 바이트 수.  @ppos: 파일 위치
 * @return: 실제로 쓴 바이트 수, 0(끝), 또는 음수 errno
 *
 * setpci 가 쓰는 경로다. 읽기 쪽의 정확한 거울상이며, 정렬 사다리도
 * 엔디안 변환 방향만 반대일 뿐 같다(le16_to_cpu / le32_to_cpu).
 *
 * 읽기와 다른 점이 셋이다.
 *   1) 권한에 따라 범위를 줄이지 않는다. 크기는 언제나 dev->cfg_size 다.
 *      쓰기를 막는 것은 파일 모드(S_IWUSR — 소유자만)이지 이 함수 안의
 *      검사가 아니다. capable() 호출은 이 함수에 없다.
 *   2) security_locked_down(LOCKDOWN_PCI_ACCESS) 를 먼저 본다. 커널
 *      lockdown 이 걸린 시스템에서는 root 라도 막는다 — 임의 config 쓰기는
 *      DMA 를 아무 데나 향하게 만들어 커널 무결성을 깰 수 있기 때문이다.
 *   3) 마지막에 i_size_write(ino, dev->cfg_size) 로 inode 크기를 갱신한다.
 *
 * 4바이트 루프에 cond_resched() 가 없다. 코드는 고치지 않고 사실만 적어 둔다.
 *
 * 실행 컨텍스트: write 시스템 호출의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   (write) → proc_ops.proc_write → [이 함수]
 *     → __get_user() → pci_user_write_config_byte/word/dword [access.c]
 */
static ssize_t proc_bus_pci_write(struct file *file, const char __user *buf,
				  size_t nbytes, loff_t *ppos)
{
	struct inode *ino = file_inode(file);
	/* [한국어] proc_create_data() 에 심어 둔 pci_dev 를 되찾는다 */
	struct pci_dev *dev = pde_data(ino);
	/* [한국어] 쓰기 위치. 읽기 쪽과 달리 int 다 — 코드는 고치지 않고 차이만 적어 둔다 */
	int pos = *ppos;
	/* [한국어] 읽기와 달리 권한에 따라 줄이지 않는다. 언제나 config space 전체가 대상이다.
	 * 쓰기를 막는 것은 파일 모드(S_IWUSR)이지 이 함수 안의 검사가 아니다 */
	int size = dev->cfg_size;
	/* [한국어] cnt = 아직 쓰지 못한 바이트 수, ret = lockdown 검사 결과 */
	int cnt, ret;

	/* [한국어] 커널 lockdown 이 걸려 있으면 root 라도 막는다.
	 * 임의 config 쓰기는 BAR 를 옮기거나 버스 마스터를 켜서 DMA 를 아무 데나
	 * 향하게 만들 수 있고, 그것은 커널 무결성을 깨는 경로다 */
	ret = security_locked_down(LOCKDOWN_PCI_ACCESS);
	/* [한국어] 막혔으면 그 오류를 그대로 올린다 */
	if (ret)
		return ret;

	/* [한국어] 이미 끝을 넘은 위치면 EOF */
	if (pos >= size)
		return 0;
	/* [한국어] 요청이 config space 보다 크면 */
	if (nbytes >= size)
		/* [한국어] 잘라 낸다 */
		nbytes = size;
	/* [한국어] 위치를 더한 끝이 넘으면 */
	if (pos + nbytes > size)
		/* [한국어] 남은 만큼으로 자른다 */
		nbytes = size - pos;
	/* [한국어] 이제부터 cnt 를 줄여 가며 쓴다 */
	cnt = nbytes;

	/* [한국어] 사용자 버퍼 유효성을 한 번에 확인한다. 아래 __get_user 는 검사를 생략한다 */
	if (!access_ok(buf, cnt))
		return -EINVAL;

	pci_config_pm_runtime_get(dev);

	/* [한국어] 읽기 쪽과 같은 정렬 사다리. 방향만 반대다 */
	if ((pos & 1) && cnt) {
		/* [한국어] 1바이트 값 */
		unsigned char val;
		/* [한국어] 사용자 버퍼에서 가져온다 */
		__get_user(val, buf);
		/* [한국어] byte 단위 config 쓰기 */
		pci_user_write_config_byte(dev, pos, val);
		/* [한국어] 버퍼 전진 */
		buf++;
		/* [한국어] 오프셋 전진 */
		pos++;
		/* [한국어] 남은 바이트 감소 */
		cnt--;
	}

	/* [한국어] 4바이트 경계로 맞추는 2바이트 쓰기 */
	if ((pos & 3) && cnt > 2) {
		/* [한국어] __le16 로 받는 이유는 사용자가 준 바이트 순서가 리틀엔디안이기 때문이다.
		 * 타입으로 그 사실을 표시해 두면 sparse 가 변환 누락을 잡아 준다 */
		__le16 val;
		/* [한국어] 사용자 버퍼에서 가져온다 */
		__get_user(val, (__le16 __user *) buf);
		/* [한국어] CPU 바이트 순서로 바꿔 config 에 쓴다 */
		pci_user_write_config_word(dev, pos, le16_to_cpu(val));
		/* [한국어] 버퍼 전진 */
		buf += 2;
		/* [한국어] 오프셋 전진 */
		pos += 2;
		/* [한국어] 남은 바이트 감소 */
		cnt -= 2;
	}

	/* [한국어] 4바이트씩 최대한 쓴다. 읽기 쪽 루프와 달리 cond_resched() 가 없다 —
	 * 코드는 고치지 않고 이 차이만 적어 둔다 */
	while (cnt >= 4) {
		/* [한국어] 리틀엔디안 4바이트 */
		__le32 val;
		/* [한국어] 사용자 버퍼에서 가져온다 */
		__get_user(val, (__le32 __user *) buf);
		/* [한국어] CPU 순서로 바꿔 dword 단위로 쓴다 */
		pci_user_write_config_dword(dev, pos, le32_to_cpu(val));
		/* [한국어] 버퍼 전진 */
		buf += 4;
		/* [한국어] 오프셋 전진 */
		pos += 4;
		/* [한국어] 남은 바이트 감소 */
		cnt -= 4;
	}

	/* [한국어] 2바이트 이상 남았다 */
	if (cnt >= 2) {
		/* [한국어] 리틀엔디안 2바이트 */
		__le16 val;
		/* [한국어] 사용자 버퍼에서 가져온다 */
		__get_user(val, (__le16 __user *) buf);
		/* [한국어] CPU 순서로 바꿔 쓴다 */
		pci_user_write_config_word(dev, pos, le16_to_cpu(val));
		/* [한국어] 버퍼 전진 */
		buf += 2;
		/* [한국어] 오프셋 전진 */
		pos += 2;
		/* [한국어] 남은 바이트 감소 */
		cnt -= 2;
	}

	/* [한국어] 마지막 1바이트 */
	if (cnt) {
		/* [한국어] 1바이트 값 */
		unsigned char val;
		/* [한국어] 가져온다 */
		__get_user(val, buf);
		/* [한국어] 쓴다 */
		pci_user_write_config_byte(dev, pos, val);
		/* [한국어] 오프셋만 전진 */
		pos++;
	}

	pci_config_pm_runtime_put(dev);

	*ppos = pos;
	i_size_write(ino, dev->cfg_size);
	/* [한국어] 요청한 만큼 다 썼다고 보고한다. 개별 config 쓰기의 실패는 위로 전달되지
	 * 않는데, 코드는 고치지 않고 이 사실만 적어 둔다 */
	return nbytes;
}

/* [한국어] HAVE_PCI_MMAP 는 "이 아키텍처가 PCI BAR 의 mmap 을 지원하는가" 를 뜻한다.
 * 지원하지 않는 아키텍처에서는 struct pci_filp_private 자체가 필요 없으므로
 * 타입 정의부터 통째로 빼 버린다. 그래서 아래 ioctl 안에서도 이 타입을 쓰는
 * 부분마다 같은 #ifdef 가 다시 나온다 */
#ifdef HAVE_PCI_MMAP
/* [한국어] 열린 파일 하나에 딸린 mmap 설정. ioctl 로 정한 값을 mmap 까지 나르는
 * 것이 유일한 용도다.
 * 
 * 왜 파일 단위인가: mmap 시스템 호출에는 "I/O 인가 메모리인가" 를 전달할
 * 인자가 없다. 그렇다고 장치 단위 전역 상태로 두면 같은 장치를 동시에 연
 * 두 프로세스가 서로의 설정을 덮어쓰게 된다.
 * 
 * 수명: proc_bus_pci_open() 이 잡고 proc_bus_pci_release() 가 놓는다.
 * HAVE_PCI_MMAP 가 정의된 아키텍처에서만 존재한다 */
struct pci_filp_private {
	/* [한국어] 이어질 mmap 이 I/O 공간인지 메모리 공간인지.
	 * 설정자: proc_bus_pci_ioctl() 의 PCIIOC_MMAP_IS_IO / _IS_MEM 갈래.
	 *   proc_bus_pci_open() 이 pci_mmap_io 로 초기화한다.
	 * 읽는 자: proc_bus_pci_mmap() 이 찾을 자원 종류와 write-combine 허용 여부를
	 *   이 값으로 가른다.
	 * 값 범위: enum pci_mmap_state 의 pci_mmap_io / pci_mmap_mem.
	 *   기본값이 io 라서, ioctl 없이 mmap 하면 대부분의 아키텍처에서 실패한다 —
	 *   안전한 쪽으로 기울인 기본값이다.
	 * 동기화: 파일 단위 상태이고 한 프로세스가 순서대로 쓰므로 락이 없다 */
	enum pci_mmap_state mmap_state;
	/* [한국어] write-combining 매핑을 요청했는가.
	 * 설정자: proc_bus_pci_ioctl() 의 PCIIOC_WRITE_COMBINE 갈래.
	 *   proc_bus_pci_open() 이 0 으로 초기화한다.
	 * 읽는 자: proc_bus_pci_mmap(). 다만 그 값이 그대로 쓰이지는 않는다 —
	 *   대상 BAR 가 prefetchable 일 때만 실제로 허용하고, 아니면 -EINVAL 이다.
	 * 값 범위: 0 또는 1.
	 * 동기화: 위와 같다 */
	int write_combine;
};
#endif /* HAVE_PCI_MMAP */

/* [한국어]
 * proc_bus_pci_ioctl - 도메인 번호를 알려 주고 이어질 mmap 의 종류를 정한다
 *
 * @file: 열린 장치 파일.  @cmd: PCIIOC_* 명령
 * @arg: PCIIOC_WRITE_COMBINE 에서만 쓰인다(0 이면 끄기)
 * @return: PCIIOC_CONTROLLER 면 도메인 번호, 그 밖에는 0 또는 -EINVAL
 *
 * mmap 시스템 호출에는 "I/O 공간인가 메모리 공간인가", "write-combining 을
 * 쓸 것인가" 를 전달할 인자가 없다. 그래서 mmap 전에 ioctl 로 미리 정해
 * 두고, 그 값을 struct pci_filp_private 에 담아 둔다. proc_bus_pci_mmap()
 * 이 그것을 읽는다. 파일 단위 상태이므로 같은 장치를 두 번 열면 서로
 * 간섭하지 않는다.
 *
 * 명령은 넷이다.
 *   PCIIOC_CONTROLLER   - 이 장치가 속한 PCI 도메인(세그먼트) 번호를
 *                         반환값으로 돌려준다. 다른 셋과 달리 상태를
 *                         바꾸지 않는 조회이고, HAVE_PCI_MMAP 밖에 있어
 *                         mmap 을 지원하지 않는 아키텍처에서도 쓸 수 있다.
 *   PCIIOC_MMAP_IS_IO   - 이어질 mmap 이 I/O 공간이다. 아키텍처가 I/O 공간
 *                         mmap 을 지원하지 않으면 -EINVAL.
 *   PCIIOC_MMAP_IS_MEM  - 이어질 mmap 이 메모리 공간이다.
 *   PCIIOC_WRITE_COMBINE- write-combining 매핑을 요청한다. 아키텍처가
 *                         지원하지 않으면 fallthrough 로 default 에 떨어져
 *                         -EINVAL 이 된다(원문 주석이 그 의도를 밝힌다).
 *
 * security_locked_down(LOCKDOWN_PCI_ACCESS) 를 맨 앞에서 본다. 이어질
 * mmap 이 결국 물리 메모리 창을 여는 일이라 같은 부류로 취급한다.
 *
 * CONFIG_COMPAT 에서는 proc_compat_ioctl 로도 이 함수를 그대로 쓴다.
 * 인자가 포인터가 아니라 스칼라라 32비트/64비트 표현이 같기 때문이다.
 *
 * 실행 컨텍스트: ioctl 시스템 호출의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (ioctl) → proc_ops.proc_ioctl → [이 함수]
 */
static long proc_bus_pci_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct pci_dev *dev = pde_data(file_inode(file));
/* [한국어] mmap 종류를 저장할 곳이 있을 때만 그 지역 변수를 둔다.
 * 없으면 아래 PCIIOC_MMAP_IS_IO 계열 갈래도 함께 사라져 이 변수가 미사용이 된다 */
#ifdef HAVE_PCI_MMAP
	/* [한국어] ioctl 로 정한 값을 담아 둘 파일 단위 상태 */
	struct pci_filp_private *fpriv = file->private_data;
#endif /* HAVE_PCI_MMAP */
	/* [한국어] 반환값. PCIIOC_CONTROLLER 갈래에서는 도메인 번호가 여기 담긴다 */
	int ret = 0;

	/* [한국어] 이어질 mmap 이 결국 물리 메모리 창을 여는 일이라, lockdown 을 먼저 본다 */
	ret = security_locked_down(LOCKDOWN_PCI_ACCESS);
	/* [한국어] 막혔으면 그대로 올린다 */
	if (ret)
		return ret;

	/* [한국어] 명령마다 다르게 처리한다 */
	switch (cmd) {
	/* [한국어] 이 장치가 속한 PCI 도메인(세그먼트) 번호를 묻는다.
	 * 다른 셋과 달리 상태를 바꾸지 않는 조회이고, HAVE_PCI_MMAP 밖에 있어
	 * mmap 을 지원하지 않는 아키텍처에서도 쓸 수 있다 */
	case PCIIOC_CONTROLLER:
		ret = pci_domain_nr(dev->bus);
		break;

/* [한국어] mmap 관련 ioctl 세 갈래. 지원하지 않는 아키텍처에서는 이 명령들이
 * default 로 떨어져 -EINVAL 이 된다. PCIIOC_CONTROLLER 만 이 블록 밖에
 * 있어 어디서나 쓸 수 있다 */
#ifdef HAVE_PCI_MMAP
	/* [한국어] 이어질 mmap 이 I/O 공간이다 */
	case PCIIOC_MMAP_IS_IO:
		if (!arch_can_pci_mmap_io())
			return -EINVAL;
		/* [한국어] 상태에 기록해 둔다. proc_bus_pci_mmap() 이 이 값을 보고
		 * 찾을 자원 종류를 IORESOURCE_IO 로 바꾼다 */
		fpriv->mmap_state = pci_mmap_io;
		break;

	/* [한국어] 이어질 mmap 이 메모리 공간이다. 아키텍처 지원 여부를 묻지 않는다 —
	 * 메모리 공간 mmap 은 어디서나 가능하기 때문이다 */
	case PCIIOC_MMAP_IS_MEM:
		fpriv->mmap_state = pci_mmap_mem;
		break;

	/* [한국어] write-combining 매핑을 요청한다. 쓰기를 모아 한 번에 내보내 대역폭을
	 * 높이지만, 쓰기 순서가 보장되지 않아 레지스터에는 쓸 수 없다 */
	case PCIIOC_WRITE_COMBINE:
		if (arch_can_pci_mmap_wc()) {
			/* [한국어] 0 이 아니면 켜기 */
			if (arg)
				/* [한국어] 요청을 기록해 둔다. 실제 허용 여부는 mmap 시점에 BAR 가
				 * prefetchable 인지 보고 다시 판정한다 */
				fpriv->write_combine = 1;
			else
				/* [한국어] 0 이면 끄기 */
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

/* [한국어] mmap 과 그것을 위한 open/release 세 함수 전체. 지원하지 않는
 * 아키텍처에서는 코드가 아예 생성되지 않는다 */
#ifdef HAVE_PCI_MMAP
/* [한국어]
 * proc_bus_pci_mmap - 이 장치의 BAR 를 사용자 주소 공간에 매핑한다
 *
 * @file: 열린 장치 파일.  @vma: 커널이 준비한 가상 메모리 영역
 * @return: 0 = 성공, -EPERM / -EINVAL / -ENODEV = 실패
 *
 * X 서버나 사용자 공간 드라이버가 장치 레지스터에 직접 접근할 때 쓰는
 * 통로다. 위험한 만큼 검사가 겹겹이다.
 *
 *   1) CAP_SYS_RAWIO 와 lockdown. 물리 메모리 창을 여는 일이므로
 *      CAP_SYS_ADMIN 보다 좁고 정확한 CAP_SYS_RAWIO 를 요구한다.
 *   2) dev->non_mappable_bars 가 서 있으면 거절한다. BAR 를 사용자에게
 *      노출하면 안 되는 장치에 quirk 등이 세우는 플래그다.
 *   3) ioctl 로 정해 둔 mmap 종류가 I/O 공간이면, 아키텍처가 그것을
 *      지원하는지 확인하고 찾을 자원 종류를 IORESOURCE_IO 로 바꾼다.
 *   4) 원문 주석대로 "사용자가 요청한 범위가 이 장치의 실제 자원인지" 를
 *      확인한다. 표준 BAR 여섯 개를 훑으며 종류가 맞고 pci_mmap_fits() 가
 *      참인 것을 찾는다. 하나도 없으면 -ENODEV — 이 검사가 없으면 파일
 *      오프셋을 조작해 남의 물리 메모리를 매핑할 수 있게 된다.
 *   5) write-combining 은 그 BAR 가 prefetchable 일 때만 허용한다.
 *      prefetchable 이 아닌 영역은 쓰기 순서와 병합이 부작용을 낳는다
 *      (레지스터 쓰기 순서가 바뀌면 하드웨어가 오동작한다).
 *   6) iomem_is_exclusive() — 다른 드라이버가 배타적으로 쥔 영역이면 거절한다.
 *
 * 그 다음 vm_pgoff 를 자원 안에서의 상대 오프셋으로 고쳐 넘긴다.
 * 사용자는 "전역 물리 주소" 를 오프셋으로 주는데, pci_mmap_resource_range()
 * 는 "그 BAR 안에서의 오프셋" 을 기대하기 때문이다. pci_resource_to_user()
 * 로 사용자에게 보이는 시작 주소를 구한 뒤 그만큼 빼는 것이 그 변환이다.
 *
 * HAVE_PCI_MMAP 가 정의된 아키텍처에서만 존재한다.
 *
 * 실행 컨텍스트: mmap 시스템 호출의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   (mmap) → proc_ops.proc_mmap → [이 함수]
 *     → pci_mmap_fits() → pci_resource_to_user() → pci_mmap_resource_range()
 */
static int proc_bus_pci_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct pci_dev *dev = pde_data(file_inode(file));
	/* [한국어] ioctl 로 정해 둔 mmap 종류와 write-combine 요청 */
	struct pci_filp_private *fpriv = file->private_data;
	/* [한국어] 사용자에게 보여 줄 자원의 시작과 끝 주소 */
	resource_size_t start, end;
	/* [한국어] i = BAR 번호, write_combine = 실제 허용 여부(요청과 다를 수 있다),
	 * res_bit = 찾을 자원 종류. 기본은 메모리 공간이다 */
	int i, ret, write_combine = 0, res_bit = IORESOURCE_MEM;

	/* [한국어] CAP_SYS_ADMIN 이 아니라 CAP_SYS_RAWIO 를 요구한다. 물리 메모리 창을
	 * 여는 좁고 정확한 권한이기 때문이다. lockdown 도 함께 본다 */
	if (!capable(CAP_SYS_RAWIO) ||
	    security_locked_down(LOCKDOWN_PCI_ACCESS))
		return -EPERM;

	/* Skip devices with non-mappable BARs */
	if (dev->non_mappable_bars)
		return -EINVAL;

	/* [한국어] ioctl 로 I/O 공간을 지정했으면 */
	if (fpriv->mmap_state == pci_mmap_io) {
		/* [한국어] 아키텍처가 I/O 공간 mmap 을 지원하는지 확인한다. 대부분은 지원하지 않아
		 * 여기서 -EINVAL 이 난다 */
		if (!arch_can_pci_mmap_io())
			return -EINVAL;
		/* [한국어] 찾을 자원 종류를 I/O 로 바꾼다 */
		res_bit = IORESOURCE_IO;
	}

	/* Make sure the caller is mapping a real resource for this device */
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		/* [한국어] 종류가 맞고, 사용자가 요청한 범위가 그 BAR 안에 들어오는가.
		 * 바로 위 원문 영어 주석이 목적을 밝힌다 — 이 장치의 실제 자원을
		 * 매핑하는 것인지 확인하는 것이다. 이 검사가 없으면 파일 오프셋을 조작해
		 * 남의 물리 메모리를 매핑할 수 있게 된다 */
		if (dev->resource[i].flags & res_bit &&
		    pci_mmap_fits(dev, i, vma,  PCI_MMAP_PROCFS))
			break;
	}

	/* [한국어] 여섯 BAR 어디에도 맞지 않는다. 이 장치의 자원이 아니다 */
	if (i >= PCI_STD_NUM_BARS)
		return -ENODEV;

	/* [한국어] 메모리 공간이면서 write-combine 을 요청했다 */
	if (fpriv->mmap_state == pci_mmap_mem &&
	    fpriv->write_combine) {
		/* [한국어] prefetchable 자원일 때만 허용한다. prefetchable 이 아닌 영역은
		 * 쓰기 병합과 순서 변경이 하드웨어 오동작을 부른다 — 레지스터 쓰기
		 * 순서가 뒤바뀌면 안 되기 때문이다 */
		if (dev->resource[i].flags & IORESOURCE_PREFETCH)
			/* [한국어] 실제로 켠다 */
			write_combine = 1;
		else
			return -EINVAL;
	}

	/* [한국어] 메모리 자원이면서 다른 드라이버가 배타적으로 쥔 영역인지 본다.
	 * 배타 영역을 사용자 공간에 열어 주면 그 드라이버의 상태가 밖에서 바뀐다 */
	if (dev->resource[i].flags & IORESOURCE_MEM &&
	    iomem_is_exclusive(dev->resource[i].start))
		return -EINVAL;

	/* [한국어] 사용자에게 보이는 시작/끝 주소를 구한다. 아키텍처에 따라 커널이 쓰는
	 * 주소와 다를 수 있어 반드시 거쳐야 한다 */
	pci_resource_to_user(dev, i, &dev->resource[i], &start, &end);

	/* Adjust vm_pgoff to be the offset within the resource */
	vma->vm_pgoff -= start >> PAGE_SHIFT;
	/* [한국어] 실제 페이지 매핑은 drivers/pci/mmap.c 에 위임한다.
	 * fpriv->mmap_state 로 I/O 인지 메모리인지, write_combine 으로 캐시 속성을
	 * 함께 넘긴다 */
	ret = pci_mmap_resource_range(dev, i, vma,
			      fpriv->mmap_state, write_combine);
	/* [한국어] 매핑 실패 */
	if (ret < 0)
		return ret;

	return 0;
}

/* [한국어]
 * proc_bus_pci_open - 파일을 열 때 mmap 설정 상태를 하나 잡는다
 *
 * @inode: 이 장치 파일의 inode(쓰지 않는다).  @file: 열리는 파일
 * @return: 0 = 성공, -ENOMEM
 *
 * ioctl 로 정한 mmap 종류를 담아 둘 struct pci_filp_private 를 잡아
 * file->private_data 에 건다. 파일 단위 상태이므로 같은 장치를 여러 번
 * 열어도 서로 간섭하지 않는다.
 *
 * 기본값이 pci_mmap_io 인 점이 눈에 띈다. ioctl 없이 곧바로 mmap 하면
 * I/O 공간으로 해석되고, 대부분의 아키텍처에서 arch_can_pci_mmap_io() 가
 * 거짓이라 -EINVAL 이 된다. 즉 "종류를 명시하지 않으면 실패" 가 기본
 * 동작이며, 안전한 쪽으로 기울인 선택이다.
 *
 * f_mapping 을 iomem_get_mapping() 으로 바꾸는 한 줄이 중요하다. 이 파일의
 * 매핑을 /dev/mem 과 같은 iomem 주소 공간에 소속시켜, 장치가 사라질 때
 * 그 주소 공간의 매핑을 한꺼번에 무효화할 수 있게 한다. 이것이 없으면
 * 뽑힌 장치의 레지스터를 사용자 공간이 계속 매핑한 채로 남는다.
 *
 * HAVE_PCI_MMAP 가 정의된 아키텍처에서만 존재한다. 그렇지 않으면
 * proc_ops 에 .proc_open 이 없고, 그 경우 procfs 가 기본 동작을 쓴다.
 *
 * 실행 컨텍스트: open 시스템 호출의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (open) → proc_ops.proc_open → [이 함수] → iomem_get_mapping()
 */
static int proc_bus_pci_open(struct inode *inode, struct file *file)
{
	struct pci_filp_private *fpriv = kmalloc_obj(*fpriv);

	/* [한국어] 메모리 부족 */
	if (!fpriv)
		return -ENOMEM;

	/* [한국어] 기본값이 I/O 공간이다. ioctl 없이 곧바로 mmap 하면 대부분의
	 * 아키텍처에서 arch_can_pci_mmap_io() 가 거짓이라 실패한다 —
	 * "종류를 명시하지 않으면 실패" 를 기본으로 삼은 안전한 선택이다 */
	fpriv->mmap_state = pci_mmap_io;
	/* [한국어] write-combining 은 명시적으로 요청해야만 켜진다 */
	fpriv->write_combine = 0;

	/* [한국어] 이후 ioctl 과 mmap 이 이 포인터로 상태를 주고받는다 */
	file->private_data = fpriv;
	/* [한국어] 이 파일의 매핑을 /dev/mem 과 같은 iomem 주소 공간에 소속시킨다.
	 * 장치가 사라질 때 그 주소 공간의 매핑을 한꺼번에 무효화할 수 있게 되며,
	 * 이것이 없으면 뽑힌 장치의 레지스터를 사용자 공간이 계속 매핑한 채로 남는다 */
	file->f_mapping = iomem_get_mapping();

	return 0;
}

/* [한국어]
 * proc_bus_pci_release - 파일을 닫을 때 mmap 설정 상태를 놓는다
 *
 * @inode: 쓰지 않는다.  @file: 닫히는 파일.  @return: 항상 0
 *
 * proc_bus_pci_open() 이 잡은 struct pci_filp_private 를 kfree 하고
 * 포인터를 NULL 로 만든다. NULL 로 만드는 것은 방어적인 처리다 —
 * release 뒤에 file->private_data 를 볼 코드 경로는 없지만, 남겨 두면
 * 해제된 메모리를 가리키는 포인터가 된다.
 *
 * 이미 만들어 둔 mmap 은 여기서 끊지 않는다. 그것은 vma 의 수명에
 * 달려 있고, munmap 이나 프로세스 종료 때 정리된다.
 *
 * HAVE_PCI_MMAP 가 정의된 아키텍처에서만 존재한다.
 *
 * 실행 컨텍스트: close 시스템 호출의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (close) → proc_ops.proc_release → [이 함수] → kfree()
 */
static int proc_bus_pci_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	/* [한국어] 해제된 메모리를 가리키는 포인터를 남기지 않는다. release 뒤에 이 값을
	 * 볼 경로는 없지만 방어적으로 지운다 */
	file->private_data = NULL;

	return 0;
}
#endif /* HAVE_PCI_MMAP */

/* [한국어] 장치 파일 하나의 파일 연산 표. proc_create_data() 가 이것을 노드에 건다.
 * 구성이 mmap 지원 여부에 따라 달라지므로 #ifdef 가 섞여 있다.
 * struct proc_ops 에는 .owner 필드가 없어 모듈 참조를 잡지 않는다 */
static const struct proc_ops proc_bus_pci_ops = {
	.proc_lseek	= proc_bus_pci_lseek,
	.proc_read	= proc_bus_pci_read,
	.proc_write	= proc_bus_pci_write,
	.proc_ioctl	= proc_bus_pci_ioctl,
/* [한국어] 32비트 사용자 공간을 지원하는 커널에서만 compat 진입점을 단다 */
#ifdef CONFIG_COMPAT
	/* [한국어] 32비트 프로세스가 64비트 커널에 ioctl 할 때의 진입점.
	 * 같은 함수를 그대로 쓰는 이유는 인자가 포인터가 아니라 스칼라라
	 * 32비트와 64비트에서 표현이 같기 때문이다 */
	.proc_compat_ioctl = proc_bus_pci_ioctl,
#endif
/* [한국어] mmap 을 지원하는 아키텍처에서만 open/release/mmap 을 단다.
 * open 이 필요한 이유는 파일마다 struct pci_filp_private 를 잡아야 해서다 */
#ifdef HAVE_PCI_MMAP
	/* [한국어] mmap 을 지원하는 아키텍처에서만 open/release/mmap 을 단다.
	 * open 이 필요한 이유는 파일마다 struct pci_filp_private 를 잡아야 해서다 */
	.proc_open	= proc_bus_pci_open,
	.proc_release	= proc_bus_pci_release,
	.proc_mmap	= proc_bus_pci_mmap,
/* [한국어] 아키텍처가 매핑 주소 선택을 직접 하겠다고 선언한 경우에만 그 훅을 단다 */
#ifdef HAVE_ARCH_PCI_GET_UNMAPPED_AREA
	/* [한국어] 아키텍처가 제공하는 매핑 주소 선택 훅. 이 심볼은 이 스파스 체크아웃
	 * 어디에도 정의가 없다 — HAVE_ARCH_PCI_GET_UNMAPPED_AREA 를 정의하는
	 * 아키텍처 코드가 함께 제공하는 것으로 보이나, 확인하지 못했다 */
	.proc_get_unmapped_area = get_pci_unmapped_area,
#endif /* HAVE_ARCH_PCI_GET_UNMAPPED_AREA */
#endif /* HAVE_PCI_MMAP */
};

/* iterator */
/* [한국어]
 * pci_seq_start - /proc/bus/pci/devices 순회의 시작 위치를 찾는다
 *
 * @m: seq_file 컨텍스트(쓰지 않는다).  @pos: 몇 번째 장치부터인가
 * @return: 그 위치의 pci_dev(참조를 잡은 채), 또는 NULL(끝)
 *
 * seq_file 반복자 셋(start/next/stop) 중 첫 번째다. seq_file 은 출력이
 * 버퍼보다 커지면 같은 위치에서 다시 시작하므로, start 는 "n 번째로
 * 건너뛰기" 를 할 수 있어야 한다.
 *
 * 구현이 단순한 선형 건너뛰기다. for_each_pci_dev() 로 처음부터 훑으며
 * n 번 지나친다. O(n) 이지만 이 파일을 읽는 일이 드물고 장치 수도 많지
 * 않아 문제가 되지 않는다.
 *
 * for_each_pci_dev() 는 매 바퀴 참조를 잡아 넘겨주고 다음 바퀴에서
 * 이전 것을 놓는다. break 로 빠져나오면 그 장치의 참조가 잡힌 채
 * 남으며, 그것을 놓는 것이 pci_seq_stop() 이다. 즉 세 함수가 참조 하나를
 * 주고받는 구조다.
 *
 * 실행 컨텍스트: read 시스템 호출의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (read /proc/bus/pci/devices) → seq_read → [이 함수] → for_each_pci_dev()
 */
static void *pci_seq_start(struct seq_file *m, loff_t *pos)
{
	/* [한국어] for_each_pci_dev 가 첫 바퀴에서 값을 채우므로 NULL 로 시작한다 */
	struct pci_dev *dev = NULL;
	/* [한국어] 건너뛸 개수. loff_t 를 지역 변수로 복사해 두고 아래에서 줄여 나간다 */
	loff_t n = *pos;

	/* [한국어] 처음부터 훑는다. 매 바퀴 참조를 잡아 주고 다음 바퀴에서 이전 것을 놓는다 */
	for_each_pci_dev(dev) {
		/* [한국어] n 번 지나쳤으면 멈춘다. 후위 감소라 n 이 0 일 때 곧바로 참이 되어
		 * 첫 장치에서 멈춘다. 여기서 break 하면 그 장치의 참조가 잡힌 채 남고,
		 * 그것을 놓는 것은 pci_seq_stop() 이다 */
		if (!n--)
			break;
	}
	return dev;
}

/* [한국어]
 * pci_seq_next - 다음 장치로 넘어간다
 *
 * @m: seq_file 컨텍스트(쓰지 않는다).  @v: 현재 장치(pci_seq_start 가 준 것)
 * @pos: 위치. 하나 증가시킨다
 * @return: 다음 pci_dev(참조를 잡은 채), 또는 NULL(끝)
 *
 * pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev) 는 "종류를 가리지 않고
 * dev 다음 장치를 달라" 는 뜻이다. 이 함수는 인자로 받은 dev 의 참조를
 * 놓고 다음 것의 참조를 잡아 돌려주므로, 반복자가 참조를 하나만 들고
 * 이동하는 모양이 된다.
 *
 * 그래서 이 함수 안에는 pci_dev_put() 이 보이지 않는다 —
 * pci_get_device() 가 대신 해 준다. 마지막에 NULL 을 돌려줄 때도 인자의
 * 참조는 이미 놓인 상태라, pci_seq_stop() 이 NULL 을 받고 아무 일도
 * 하지 않는 것과 아귀가 맞는다.
 *
 * 실행 컨텍스트: read 시스템 호출의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   seq_read → [이 함수] → pci_get_device()
 */
static void *pci_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct pci_dev *dev = v;

	/* [한국어] seq_file 이 요구하는 위치 갱신 */
	(*pos)++;
	/* [한국어] 종류를 가리지 않고(PCI_ANY_ID) dev 다음 장치를 찾는다.
	 * 이 함수가 인자의 참조를 놓고 결과의 참조를 잡아 주므로,
	 * 반복자가 참조를 하나만 들고 이동하는 모양이 된다 */
	dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev);
	return dev;
}

/* [한국어]
 * pci_seq_stop - 순회를 끝내며 들고 있던 참조를 놓는다
 *
 * @m: seq_file 컨텍스트(쓰지 않는다).  @v: 마지막으로 다룬 장치, 또는 NULL
 * @return: 없음
 *
 * start 와 next 가 잡아 둔 참조 하나를 여기서 놓는다. v 가 NULL 인 경우는
 * 순회가 끝까지 갔을 때(next 가 NULL 을 돌려준 뒤)이며, 그때는 놓을 것이
 * 없다.
 *
 * seq_file 은 버퍼가 차면 stop 을 부르고 나중에 start 부터 다시 시작한다.
 * 그래서 이 함수는 여러 번 불릴 수 있고, 매번 정확히 하나씩만 놓아야 한다.
 *
 * 실행 컨텍스트: read 시스템 호출의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   seq_read → [이 함수] → pci_dev_put()
 */
static void pci_seq_stop(struct seq_file *m, void *v)
{
	if (v) {
		/* [한국어] start 또는 next 가 잡아 둔 참조를 놓는다. v 가 NULL 인 경우는 순회가
		 * 끝까지 갔을 때이며, 그때는 next 가 이미 놓은 뒤라 할 일이 없다 */
		struct pci_dev *dev = v;
		pci_dev_put(dev);
	}
}

/* [한국어]
 * show_device - /proc/bus/pci/devices 의 한 줄을 만든다
 *
 * @m: 출력할 seq_file.  @v: 이번에 찍을 pci_dev
 * @return: 항상 0
 *
 * 이 파일의 서식은 1997년부터 굳어져 있고 lspci 가 그대로 파싱하므로
 * 바꿀 수 없다. 필드 사이는 탭으로 나뉜다.
 *
 *   bus(2자리)+devfn(2자리)  붙여서 4자리 16진수
 *   vendor(4자리)+device(4자리)  붙여서 8자리 16진수
 *   irq
 *   BAR 0~5 와 ROM 의 시작 주소 7개
 *   같은 7개의 크기
 *   드라이버 이름(없으면 빈 칸)
 *
 * 주소 필드에는 PCI_REGION_FLAG_MASK 로 걸러 낸 플래그 비트가 OR 되어
 * 들어간다. 자원 주소의 하위 비트는 정렬 때문에 언제나 0 이라 그 자리를
 * "이것이 I/O 인가 메모리인가, prefetchable 인가" 를 싣는 데 쓴다.
 * lspci 는 그 비트를 보고 종류를 구분한다.
 *
 * 크기 계산에서 start < end 를 먼저 확인하는 이유는, 할당되지 않은 자원이
 * start = end = 0 으로 남아 있기 때문이다. 그 경우 (end - start) + 1 이
 * 1 이 되어 "1바이트짜리 자원" 으로 보이므로, 명시적으로 0 을 찍는다.
 *
 * 원문 주석대로 표준 BAR 와 ROM 까지만 찍는다 — SR-IOV BAR 등을 뒤에
 * 덧붙이면 기존 파서가 깨지기 때문이다.
 *
 * pci_resource_to_user() 를 거치는 이유는 아키텍처에 따라 커널이 쓰는
 * 주소와 사용자에게 보여야 할 주소가 다를 수 있어서다.
 *
 * 실행 컨텍스트: read 시스템 호출의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   seq_read → seq_operations.show → [이 함수] → pci_resource_to_user() → seq_printf()
 */
static int show_device(struct seq_file *m, void *v)
{
	const struct pci_dev *dev = v;
	/* [한국어] 이 장치에 바인딩된 드라이버(없을 수 있다) */
	const struct pci_driver *drv;
	int i;

	/* [한국어] seq_file 이 범위를 벗어난 위치를 물어보면 v 가 NULL 로 온다 */
	if (dev == NULL)
		return 0;

	/* [한국어] 드라이버 이름을 마지막 필드에 찍기 위해 미리 얻어 둔다 */
	drv = pci_dev_driver(dev);
	/* [한국어] 첫 세 필드. bus 와 devfn 을 붙여 4자리, vendor 와 device 를 붙여 8자리로
	 * 찍는 것이 1997년부터 굳어진 서식이고 lspci 가 그대로 파싱한다 */
	seq_printf(m, "%02x%02x\t%04x%04x\t%x",
			dev->bus->number,
			dev->devfn,
			dev->vendor,
			dev->device,
			dev->irq);

	/* only print standard and ROM resources to preserve compatibility */
	for (i = 0; i <= PCI_ROM_RESOURCE; i++) {
		/* [한국어] 이 자원의 사용자용 시작/끝 주소 */
		resource_size_t start, end;
		pci_resource_to_user(dev, i, &dev->resource[i], &start, &end);
		seq_printf(m, "\t%16llx",
			(unsigned long long)(start |
			(dev->resource[i].flags & PCI_REGION_FLAG_MASK)));
	}
	/* [한국어] 같은 자원들을 한 번 더 훑으며 이번에는 크기를 찍는다 */
	for (i = 0; i <= PCI_ROM_RESOURCE; i++) {
		/* [한국어] 다시 사용자용 주소를 구한다 */
		resource_size_t start, end;
		pci_resource_to_user(dev, i, &dev->resource[i], &start, &end);
		/* [한국어] 하위 플래그 비트를 OR 해서 함께 싣는다. 자원 주소의 하위 비트는 정렬
		 * 때문에 언제나 0 이라 그 자리를 "I/O 인가 메모리인가, prefetchable 인가"
		 * 를 싣는 데 쓴다. lspci 는 그 비트로 종류를 구분한다 */
		seq_printf(m, "\t%16llx",
			dev->resource[i].start < dev->resource[i].end ?
			(unsigned long long)(end - start) + 1 : 0);
	}
	/* [한국어] 드라이버 이름 앞의 구분자 */
	seq_putc(m, '\t');
	/* [한국어] 드라이버가 바인딩되어 있으면 */
	if (drv)
		seq_puts(m, drv->name);
	/* [한국어] start < end 를 먼저 보는 이유: 할당되지 않은 자원은 start = end = 0 이라
	 * (end - start) + 1 이 1 이 되어 "1바이트짜리 자원" 으로 보인다.
	 * 그 경우 명시적으로 0 을 찍는다 */
	seq_putc(m, '\n');
	return 0;
}

/* [한국어] devices 파일의 seq_file 연산 표. proc_create_seq() 가 이것을 쓴다 */
static const struct seq_operations proc_bus_pci_devices_op = {
	/* [한국어] 순회 시작(또는 재개) — n 번째 장치로 건너뛴다 */
	.start	= pci_seq_start,
	.next	= pci_seq_next,
	.stop	= pci_seq_stop,
	.show	= show_device
};

static struct proc_dir_entry *proc_bus_pci_dir;

/* [한국어]
 * pci_proc_attach_device - 장치 하나를 /proc/bus/pci 아래에 만든다
 *
 * @dev: 갓 열거되어 등록할 PCI 장치
 * @return: 0 = 성공, -EACCES = 아직 /proc 트리가 없음, -ENOMEM
 *
 * 확인한 호출자는 둘이다 — drivers/pci/bus.c:452 의 pci_bus_add_device()
 * (정상 열거 경로. 바로 앞줄에서 sysfs 파일을 만든다)와, 같은 파일의
 * pci_proc_init()(부팅 시 이미 열거된 장치들을 한 번에 붙일 때).
 *
 * 절차:
 *   1) proc_initialized 를 본다. pci_proc_init() 이 아직 돌지 않았으면
 *      부모 디렉터리가 없으므로 -EACCES 로 물러난다. 이 경우 나중에
 *      pci_proc_init() 이 for_each_pci_dev 로 다시 붙여 준다 — 그래서
 *      실패해도 장치가 영영 빠지지는 않는다.
 *   2) 이 버스의 디렉터리가 없으면 만든다. 이름은 도메인 유무에 따라
 *      "0000:00" 또는 "00" 이다. pci_proc_domain() 이 그 판정을 하는데,
 *      그 함수는 include/linux/pci.h 에 있고 이 스파스 체크아웃에 없어
 *      구현을 확인하지 못했다.
 *   3) "1f.2" 형태로 장치 파일을 만든다. 모드는 S_IRUGO | S_IWUSR —
 *      누구나 읽을 수 있고(다만 읽히는 범위는 권한에 따라 다르다)
 *      쓰기는 소유자만 할 수 있다. proc_create_data 의 마지막 인자로
 *      넘긴 dev 를 이후 모든 파일 연산이 pde_data() 로 되찾는다.
 *   4) proc_set_size() 로 파일 크기를 config space 크기로 심는다.
 *      stat 으로 본 크기와 lseek 의 끝이 일치하게 된다.
 *   5) dev->procent 에 항목을 기억해 둔다. 제거할 때 쓴다.
 *
 * name 배열이 16바이트인데 최대 문자열은 "0000:ff"(7)와 "1f.7"(4)이라
 * 넉넉하다.
 *
 * 실행 컨텍스트: 열거 경로의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_bus_add_device() [bus.c:452] / pci_proc_init() → [이 함수]
 *     → proc_mkdir() → proc_create_data() → proc_set_size()
 */
int pci_proc_attach_device(struct pci_dev *dev)
{
	struct pci_bus *bus = dev->bus;
	/* [한국어] 만들 노드 항목 */
	struct proc_dir_entry *e;
	char name[16];

	if (!proc_initialized)
		return -EACCES;

	/* [한국어] 디렉터리와 파일 이름을 조립할 버퍼. 가장 긴 것이 "0000:ff"(7자)와
	 * "1f.7"(4자)이라 16바이트면 넉넉하다 */
	if (!bus->procdir) {
		/* [한국어] 도메인 번호를 이름에 넣어야 하는가. pci_proc_domain() 은
		 * include/linux/pci.h 에 있고 이 스파스 체크아웃에 없어 구현을 확인하지
		 * 못했다 — 이름에서 도메인 표기 여부를 정한다는 것만 코드로 확인된다 */
		if (pci_proc_domain(bus)) {
			/* [한국어] pci_proc_init() 이 아직 돌지 않았으면 부모 디렉터리가 없다.
			 * 이때 놓친 장치는 pci_proc_init() 이 마지막에 다시 훑어 만회한다 */
			sprintf(name, "%04x:%02x", pci_domain_nr(bus),
				bus->number);
		} else {
			/* [한국어] 도메인이 하나뿐인 시스템에서는 "00" 형태로 짧게 */
			sprintf(name, "%02x", bus->number);
		}
		/* [한국어] "0000:00" 형태 */
		bus->procdir = proc_mkdir(name, proc_bus_pci_dir);
		if (!bus->procdir)
			return -ENOMEM;
	}

	/* [한국어] 버스 디렉터리를 만든다 */
	sprintf(name, "%02x.%x", PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn));
	/* [한국어] 만들기 실패 */
	e = proc_create_data(name, S_IFREG | S_IRUGO | S_IWUSR, bus->procdir,
			     &proc_bus_pci_ops, dev);
	/* [한국어] 만들기 실패 */
	if (!e)
		return -ENOMEM;
	/* [한국어] stat 으로 본 크기와 lseek 의 끝이 일치하도록 config space 크기를 심는다 */
	proc_set_size(e, dev->cfg_size);
	/* [한국어] 제거할 때 쓸 수 있도록 항목을 기억해 둔다 */
	dev->procent = e;

	return 0;
}

/* [한국어]
 * pci_proc_detach_device - 장치의 /proc 파일을 없앤다
 *
 * @dev: 제거되는 PCI 장치.  @return: 항상 0
 *
 * 확인한 유일한 호출자는 drivers/pci/remove.c:111 의 pci_stop_dev() 다.
 * 그 함수는 device_release_driver() 로 드라이버를 먼저 떼어 낸 뒤 이것을
 * 부르고, 이어서 sysfs 파일도 없앤다.
 *
 * proc_remove() 는 항목을 없애면서 그것을 열고 있는 프로세스가 모두
 * 빠져나오기를 기다린다. 그래서 이 함수가 돌아온 뒤에는 이 장치의
 * 파일 연산이 더 이상 시작되지 않음이 보장된다 — pde_data() 로 얻은
 * pci_dev 포인터가 무효가 되는 것을 막는 장치다.
 *
 * dev->procent 를 NULL 로 만드는 것은 두 번 제거되는 것을 막기 위해서다.
 * proc_remove(NULL) 은 무해하지만, 해제된 항목을 다시 넘기면 안 된다.
 *
 * 반환형이 int 이지만 언제나 0 이다. 호출자도 반환값을 보지 않는다.
 * 헤더의 스텁(drivers/pci/pci.h:850)과 원형을 맞추기 위한 형태다.
 *
 * 실행 컨텍스트: 장치 제거 경로의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_stop_dev() [remove.c:111] → [이 함수] → proc_remove()
 */
int pci_proc_detach_device(struct pci_dev *dev)
{
	proc_remove(dev->procent);
	/* [한국어] 두 번 제거되는 것을 막는다. proc_remove(NULL) 자체는 무해하지만
	 * 해제된 항목을 다시 넘겨서는 안 된다 */
	dev->procent = NULL;
	return 0;
}

/* [한국어]
 * pci_proc_detach_bus - 버스 디렉터리를 통째로 없앤다
 *
 * @bus: 제거되는 PCI 버스.  @return: 항상 0
 *
 * 확인한 유일한 호출자는 drivers/pci/remove.c:146 의 pci_remove_bus() 다.
 *
 * proc_remove() 는 디렉터리를 넘기면 그 아래 항목까지 함께 없앤다.
 * 다만 정상 경로에서는 이 시점에 하위 장치들이 이미
 * pci_proc_detach_device() 로 제거되어 비어 있다.
 *
 * bus->procdir 를 NULL 로 되돌리지 않는 점이 장치 쪽과 다르다. 코드는
 * 고치지 않고 이 차이만 적어 둔다 — 버스 구조체 자체가 곧 해제되는
 * 경로라 실제 문제로 이어지지는 않는다.
 *
 * 실행 컨텍스트: 버스 제거 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_remove_bus() [remove.c:146] → [이 함수] → proc_remove()
 */
int pci_proc_detach_bus(struct pci_bus *bus)
{
	proc_remove(bus->procdir);
	return 0;
}

/* [한국어]
 * pci_proc_init - /proc/bus/pci 트리를 만든다(부팅 초기화)
 *
 * @return: 항상 0
 *
 * device_initcall 로 등록되어 부팅 중 한 번 불린다. 하는 일이 넷이다.
 *   1) /proc/bus/pci 디렉터리를 만든다. 이후 모든 버스 디렉터리의 부모다.
 *   2) 그 아래 devices 파일을 seq_file 로 만든다. 모드 0 인데도 읽을 수
 *      있는 것은 procfs 가 0 을 "기본 모드" 로 해석하기 때문이다.
 *   3) proc_initialized 를 세운다. 이 시점부터
 *      pci_proc_attach_device() 가 -EACCES 로 물러나지 않는다.
 *   4) 이미 열거된 장치들을 훑으며 붙인다. 이 initcall 보다 먼저 열거된
 *      장치들은 bus.c 의 호출이 3)의 검사에 걸려 실패했으므로, 여기서
 *      한 번에 만회한다. 순서 의존을 정면으로 다루는 처리다.
 *
 * 반환값과 proc_mkdir / proc_create_seq 의 실패를 확인하지 않는다.
 * /proc 항목을 못 만들어도 PCI 자체는 동작해야 하므로 부팅을 막지
 * 않겠다는 선택으로 보이지만, 코드는 고치지 않고 사실만 적어 둔다.
 *
 * 실행 컨텍스트: 부팅 initcall, 단일 스레드.
 *
 * 호출 체인:
 *   (device_initcall) → [이 함수]
 *     → proc_mkdir() → proc_create_seq() → pci_proc_attach_device()
 */
static int __init pci_proc_init(void)
{
	struct pci_dev *dev = NULL;
	/* [한국어] /proc/bus/pci 디렉터리. 이후 모든 버스 디렉터리의 부모가 된다.
	 * 반환값을 확인하지 않는데, 코드는 고치지 않고 사실만 적어 둔다 */
	proc_bus_pci_dir = proc_mkdir("bus/pci", NULL);
	/* [한국어] 그 아래 devices 파일을 seq_file 로 만든다. 모드가 0 인데도 읽을 수 있는
	 * 것은 procfs 가 0 을 "기본 모드" 로 해석하기 때문이다 */
	proc_create_seq("devices", 0, proc_bus_pci_dir,
		    &proc_bus_pci_devices_op);
	/* [한국어] 이 시점부터 pci_proc_attach_device() 가 물러나지 않는다.
	 * 반드시 아래 for_each_pci_dev 보다 먼저 세워야 한다 */
	proc_initialized = 1;
	for_each_pci_dev(dev)
		pci_proc_attach_device(dev);

	return 0;
}
device_initcall(pci_proc_init);
