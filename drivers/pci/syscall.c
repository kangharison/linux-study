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

SYSCALL_DEFINE5(pciconfig_read, unsigned long, bus, unsigned long, dfn,
		unsigned long, off, unsigned long, len, void __user *, buf)
{
	struct pci_dev *dev;
	/* [한국어] 1바이트 읽기 결과를 담을 변수. 세 폭을 따로 선언한 이유는 put_user() 가
	 * 포인터 타입에서 폭을 결정하기 때문이다 — 하나의 u32 로 받아 잘라 쓰면
	 * 엔디안에 따라 엉뚱한 바이트가 나간다. */
	u8 byte;
	/* [한국어] 2바이트 읽기 결과. */
	u16 word;
	/* [한국어] 4바이트 읽기 결과. */
	u32 dword;
	/* [한국어] err: 시스템 호출이 최종적으로 돌려줄 값. cfg_ret: config 접근 자체의 결과.
	 * 둘을 나눈 이유는 config 실패를 -EIO 라는 단일 값으로 접어 사용자에게
	 * PCIBIOS_* 내부 코드를 노출하지 않기 위해서다. */
	int err, cfg_ret;

	err = -EPERM;
	/* [한국어] error 라벨이 pci_dev_put(dev) 를 무조건 부르므로, 아직 장치를 얻기 전에
	 * 그리로 뛸 경우를 대비해 NULL 로 초기화해 둔다. pci_dev_put(NULL) 은 안전하다. */
	dev = NULL;
	/* [한국어] config space 직접 접근은 시스템 전체를 망가뜨릴 수 있어 CAP_SYS_ADMIN 을 요구한다.
	 * [상류 코드 관찰] 읽기 경로는 security_locked_down() 을 검사하지 않는다 —
	 * 쓰기 경로(:95~96)만 검사한다. lockdown 이 무결성 보호를 목적으로 하므로
	 * 읽기는 허용하고 쓰기만 막는다는 판단으로 보인다. */
	if (!capable(CAP_SYS_ADMIN))
		goto error;

	err = -ENODEV;
	/* [한국어] 도메인을 0 으로 고정해 장치를 찾는다. 이 오래된 시스템 호출에는 도메인
	 * 인자가 없어서, 다중 PCI 도메인 시스템에서는 첫 도메인만 접근할 수 있다.
	 * 성공하면 참조 카운트가 올라가므로 모든 경로에서 내려야 한다. */
	dev = pci_get_domain_bus_and_slot(0, bus, dfn);
	/* [한국어] 그런 장치가 없으면 위에서 준비해 둔 -ENODEV 로 나간다. */
	if (!dev)
		goto error;

	/* [한국어] 요청한 접근 폭에 따라 서로 다른 헬퍼를 부른다. */
	switch (len) {
	/* [한국어] 1바이트 읽기. */
	case 1:
		cfg_ret = pci_user_read_config_byte(dev, off, &byte);
		break;
	/* [한국어] 2바이트 읽기. */
	case 2:
		cfg_ret = pci_user_read_config_word(dev, off, &word);
		break;
	/* [한국어] 4바이트 읽기. */
	case 4:
		cfg_ret = pci_user_read_config_dword(dev, off, &dword);
		break;
	default:
		err = -EINVAL;
		goto error;
	}

	err = -EIO;
	/* [한국어] config 접근이 실패했으면 위에서 준비해 둔 -EIO 로 나간다.
	 * pci_user_read_config_*() 는 PCIBIOS_* 코드를 돌려주는데, 그것을 그대로
	 * 사용자에게 넘기지 않고 -EIO 하나로 뭉뚱그린다. */
	if (cfg_ret)
		goto error;

	/* [한국어] 읽기가 성공했으니 이제 사용자 버퍼로 복사한다. 위 switch 와 같은 분기를
	 * 두 번 하는 것은 그 사이에 오류 검사가 끼어 있기 때문이다. */
	switch (len) {
	/* [한국어] 1바이트를 사용자 공간에 쓴다. put_user 는 실패 시 -EFAULT 를 돌려주고,
	 * 그 값이 그대로 시스템 호출의 반환값이 된다. */
	case 1:
		err = put_user(byte, (u8 __user *)buf);
		break;
	/* [한국어] 2바이트 복사. */
	case 2:
		err = put_user(word, (u16 __user *)buf);
		break;
	/* [한국어] 4바이트 복사. */
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
	/* [한국어] 위 영어 주석이 밝히는 대로, 실패했을 때도 사용자 버퍼를 all-ones 로 채운다.
	 * XFree86 같은 오래된 사용자 공간이 반환값을 보지 않고 0xffffffff 인지만
	 * 확인하기 때문이며, 그것이 x86 에서 응답 없는 장치를 읽었을 때 나오는 값이다. */
	switch (len) {
	/* [한국어] 1바이트 폭으로 -1(0xff)을 쓴다. 반환값을 검사하지 않는데, 이 경로에서는
	 * 이미 err 이 정해져 있어 복사 실패를 알릴 방법도 이유도 없기 때문이다. */
	case 1:
		put_user(-1, (u8 __user *)buf);
		break;
	/* [한국어] 2바이트 폭으로 0xffff. */
	case 2:
		put_user(-1, (u16 __user *)buf);
		break;
	/* [한국어] 4바이트 폭으로 0xffffffff. */
	case 4:
		put_user(-1, (u32 __user *)buf);
		break;
	}
	pci_dev_put(dev);
	return err;
}

SYSCALL_DEFINE5(pciconfig_write, unsigned long, bus, unsigned long, dfn,
		unsigned long, off, unsigned long, len, void __user *, buf)
{
	/* [한국어] 찾아낼 장치. */
	struct pci_dev *dev;
	/* [한국어] 1바이트 쓰기 값. */
	u8 byte;
	/* [한국어] 2바이트 쓰기 값. */
	u16 word;
	/* [한국어] 4바이트 쓰기 값. */
	u32 dword;
	/* [한국어] 반환값. 읽기 경로와 달리 cfg_ret 을 따로 두지 않고 err 하나로 처리한다. */
	int err = 0;

	/* [한국어] 쓰기는 권한 검사가 두 겹이다. CAP_SYS_ADMIN 에 더해, */
	if (!capable(CAP_SYS_ADMIN) ||
	    security_locked_down(LOCKDOWN_PCI_ACCESS))
		return -EPERM;

	/* [한국어] 도메인 0 에서 장치를 찾는다. 읽기 경로와 같은 제약이 있다. */
	dev = pci_get_domain_bus_and_slot(0, bus, dfn);
	/* [한국어] 장치가 없으면 -ENODEV. 읽기 경로와 달리 goto 없이 곧장 반환하는데,
	 * 아직 참조를 얻지 못했으므로 정리할 것이 없기 때문이다. */
	if (!dev)
		return -ENODEV;

	/* [한국어] 요청 폭에 따라 분기한다. 읽기와 달리 switch 가 하나뿐인 것은,
	 * 사용자 버퍼에서 값을 가져오는 일과 config 에 쓰는 일이 같은 폭 문맥 안에서
	 * 연달아 일어나기 때문이다. */
	switch (len) {
	/* [한국어] 1바이트 쓰기. */
	case 1:
		err = get_user(byte, (u8 __user *)buf);
		/* [한국어] 사용자 버퍼 접근이 실패하면(잘못된 포인터) 그 -EFAULT 를 그대로 돌려준다. */
		if (err)
			break;
		/* [한국어] config space 에 1바이트를 쓴다. */
		err = pci_user_write_config_byte(dev, off, byte);
		/* [한국어] config 쓰기 실패는, */
		if (err)
			err = -EIO;
		break;

	/* [한국어] 2바이트 쓰기. */
	case 2:
		err = get_user(word, (u16 __user *)buf);
		/* [한국어] 사용자 버퍼 읽기 실패 검사. */
		if (err)
			break;
		/* [한국어] config space 에 2바이트를 쓴다. */
		err = pci_user_write_config_word(dev, off, word);
		/* [한국어] 실패 검사. */
		if (err)
			err = -EIO;
		break;

	/* [한국어] 4바이트 쓰기. */
	case 4:
		err = get_user(dword, (u32 __user *)buf);
		/* [한국어] 사용자 버퍼 읽기 실패 검사. */
		if (err)
			break;
		/* [한국어] config space 에 4바이트를 쓴다. */
		err = pci_user_write_config_dword(dev, off, dword);
		/* [한국어] 실패 검사. */
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
