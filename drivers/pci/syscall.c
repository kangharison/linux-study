// SPDX-License-Identifier: GPL-2.0
/*
 * For architectures where we want to allow direct access to the PCI config
 * stuff - it would probably be preferable on PCs too, but there people
 * just do it by hand with the magic northbridge registers.
 */

/*
 * [한국어 설명] config space 를 읽고 쓰는 옛 시스템 호출 (syscall.c)
 *
 * === 파일의 역할 ===
 * pciconfig_read() 와 pciconfig_write() 두 시스템 호출을 구현한다.
 * userspace 가 PCI config space 에 접근하는 세 번째 경로다 —
 * 앞의 둘은 sysfs(pci-sysfs.c)와 /proc(proc.c)이다.
 *
 * 셋 중 가장 오래됐고 지금은 거의 쓰이지 않는다. 시스템 호출 번호를
 * 차지하므로 없앨 수 없어 유지될 뿐이다. 실제로 x86 에서는 이 시스템
 * 호출이 등록조차 되어 있지 않고, alpha/arm 같은 일부 아키텍처에만 남아 있다.
 *
 * 하는 일은 단순하다. 인자로 받은 bus/dfn/off/len 으로 장치를 찾아
 * config 를 읽거나 쓰고, 결과를 사용자 버퍼에 복사한다. 권한 검사와
 * 정렬 검사를 앞에서 하고, 실제 접근은 access.c 의 userspace 전용
 * 경로(pci_user_read_config_*)에 위임한다.
 *
 * 반환값 규약이 특이하다. 오류를 음수 errno 가 아니라 PCIBIOS_* 계열
 * 상수로 돌려준다. 옛 PCI BIOS 인터페이스의 잔재다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * userspace -> syscall(pciconfig_read, bus, dfn, off, len, buf)
 *   -> [이 파일] SYSCALL_DEFINE5(pciconfig_read, ...)
 *      -> capable(CAP_SYS_ADMIN) 권한 확인
 *      -> pci_get_domain_bus_and_slot() [search.c] 로 장치를 찾고
 *      -> pci_user_read_config_* [access.c] 로 읽고
 *      -> put_user() 로 사용자 버퍼에 복사
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(시스템 호출). 사용자 메모리 접근이
 * 페이지 폴트를 일으킬 수 있어 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: userspace 의 옛 도구들.
 * 아래쪽: search.c 의 장치 조회, access.c 의 pci_user_read/write_config_*.
 * 공유 상태: 없다.
 *
 * === NVMe 관점 ===
 * NVMe 와는 관련이 없다. NVMe 장치도 원리상 이 시스템 호출로 접근할 수
 * 있지만, 현대 도구는 전부 sysfs 나 /proc 을 쓴다.
 *
 * 굳이 학습 가치를 찾자면, 같은 config space 에 세 가지 경로가 있고
 * 모두 access.c 의 pci_user_read/write_config_* 로 모인다는 점이다.
 * 그 함수들이 리셋 중 차단(block_cfg_access)을 존중하는 이유가 여기 있다 —
 * 세 경로 중 어느 것으로 들어와도 같은 보호를 받아야 하기 때문이다.
 *
 * === 주요 함수/구조체 요약 ===
 * sys_pciconfig_read()  : config 를 읽어 사용자 버퍼에 넣는다.
 *                         len 은 1/2/4 만 유효하고, 오프셋 정렬도 확인한다.
 * sys_pciconfig_write() : 사용자 버퍼의 값을 config 에 쓴다.
 *                         CAP_SYS_ADMIN 이 필요하다.
 */

#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include "pci.h"

SYSCALL_DEFINE5(pciconfig_read,
		unsigned long, bus,
		unsigned long, dfn,
		unsigned long, off,
		unsigned long, len,
		void __user *, buf)
{
	struct pci_dev *dev;
	u8 byte;
	u16 word;
	u32 dword;
	int err, cfg_ret;

	err = -EPERM;
	dev = NULL;
	if (!capable(CAP_SYS_ADMIN))
		goto error;

	err = -ENODEV;
	dev = pci_get_domain_bus_and_slot(0, bus, dfn);
	if (!dev)
		goto error;

	switch (len) {
	case 1:
		cfg_ret = pci_user_read_config_byte(dev, off, &byte);
		break;
	case 2:
		cfg_ret = pci_user_read_config_word(dev, off, &word);
		break;
	case 4:
		cfg_ret = pci_user_read_config_dword(dev, off, &dword);
		break;
	default:
		err = -EINVAL;
		goto error;
	}

	err = -EIO;
	if (cfg_ret)
		goto error;

	switch (len) {
	case 1:
		err = put_user(byte, (u8 __user *)buf);
		break;
	case 2:
		err = put_user(word, (u16 __user *)buf);
		break;
	case 4:
		err = put_user(dword, (u32 __user *)buf);
		break;
	}
	pci_dev_put(dev);
	return err;

error:
	/* ??? XFree86 doesn't even check the return value.  They
	   just look for 0xffffffff in the output, since that's what
	   they get instead of a machine check on x86.  */
	switch (len) {
	case 1:
		put_user(-1, (u8 __user *)buf);
		break;
	case 2:
		put_user(-1, (u16 __user *)buf);
		break;
	case 4:
		put_user(-1, (u32 __user *)buf);
		break;
	}
	pci_dev_put(dev);
	return err;
}

SYSCALL_DEFINE5(pciconfig_write,
		unsigned long, bus,
		unsigned long, dfn,
		unsigned long, off,
		unsigned long, len,
		void __user *, buf)
{
	struct pci_dev *dev;
	u8 byte;
	u16 word;
	u32 dword;
	int err = 0;

	if (!capable(CAP_SYS_ADMIN) ||
	    security_locked_down(LOCKDOWN_PCI_ACCESS))
		return -EPERM;

	dev = pci_get_domain_bus_and_slot(0, bus, dfn);
	if (!dev)
		return -ENODEV;

	switch (len) {
	case 1:
		err = get_user(byte, (u8 __user *)buf);
		if (err)
			break;
		err = pci_user_write_config_byte(dev, off, byte);
		if (err)
			err = -EIO;
		break;

	case 2:
		err = get_user(word, (u16 __user *)buf);
		if (err)
			break;
		err = pci_user_write_config_word(dev, off, word);
		if (err)
			err = -EIO;
		break;

	case 4:
		err = get_user(dword, (u32 __user *)buf);
		if (err)
			break;
		err = pci_user_write_config_dword(dev, off, dword);
		if (err)
			err = -EIO;
		break;

	default:
		err = -EINVAL;
		break;
	}
	pci_dev_put(dev);
	return err;
}
