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

#include <linux/errno.h>	/* NVMe: EPERM/ENODEV/EIO 등 에러 코드 정의 */
#include <linux/pci.h>		/* NVMe: PCI/NVMe 장치 구조체 및 config access 함수 */
#include <linux/security.h>	/* NVMe: LOCKDOWN_PCI_ACCESS 보안 검사 */
#include <linux/syscalls.h>	/* NVMe: SYSCALL_DEFINE5 매크로 */
#include <linux/uaccess.h>	/* NVMe: 사용자 공간 접근 put_user/get_user */
#include "pci.h"		/* NVMe: PCI 코어 낸부 헤더 */

/*
 * pciconfig_read:
 *   사용자 공간에서 지정한 PCI bus/devfn/offset/len의 config space를 읽어
 *   buf에 복사한다. NVMe 관점에서는 NVMe SSD의 PCIe config 레지스터(예:
 *   Vendor ID, BAR0, CAP_PTR)를 사용자 도구가 진단할 때 사용된다.
 *   CAP_SYS_ADMIN 권한이 필요하며, 존재하지 않는 장치는 -ENODEV로
 *   처리한다.
 */
SYSCALL_DEFINE5(pciconfig_read,	/* NVMe: pciconfig_read 시스템 콜 정의(5개 인자) */
		unsigned long, bus,	/* NVMe: 대상이 속한 PCI 버스 번호 */
		unsigned long, dfn,	/* NVMe: 대상 장치/함수 번호(devfn) */
		unsigned long, off,	/* NVMe: config space 내 바이트 오프셋 */
		unsigned long, len,	/* NVMe: 읽을 바이트 수(1/2/4) */
		void __user *, buf)	/* NVMe: 사용자 공간 결과 버퍼 */
{	/* NVMe: pciconfig_read 시스템 콜 본문 시작 */
	struct pci_dev *dev;	/* NVMe: 탐색/접근 대상 PCI 장치(NVMe SSD 등) 구조체 */
	u8 byte;		/* NVMe: 1바이트 config read 결과 */
	u16 word;		/* NVMe: 2바이트 config read 결과 */
	u32 dword;		/* NVMe: 4바이트 config read 결과 */
	int err, cfg_ret;	/* NVMe: err: 반환 에러 코드, cfg_ret: config access 결과 */

	err = -EPERM;		/* NVMe: 기본 에러를 권한 부족으로 설정 */
	dev = NULL;		/* NVMe: 장치 포인터 초기화(에러 경로에서 안전하게 put) */
	if (!capable(CAP_SYS_ADMIN))	/* NVMe: NVMe config 접근도 root/CAP_SYS_ADMIN 필요 */
		goto error;	/* NVMe: 권한 없으면 -EPERM 반환 */

	err = -ENODEV;		/* NVMe: 이후 에러는 장치 부재로 설정 */
	dev = pci_get_domain_bus_and_slot(0, bus, dfn);	/* NVMe: domain 0에서 bus/dfn으로 NVMe 등 PCI 장치 검색 */
	if (!dev)		/* NVMe: 해당 슬롯에 NVMe 장치가 없으면 */
		goto error;	/* NVMe: -ENODEV 처리 */

	switch (len) {		/* NVMe: 요청한 바이트 수에 따라 config read 분기 */
	case 1:		/* NVMe: 1바이트 읽기 분기 */
		cfg_ret = pci_user_read_config_byte(dev, off, &byte);	/* NVMe: NVMe config space 1바이트 읽기 */
		break;		/* NVMe: 1바이트 분기 종료 */
	case 2:		/* NVMe: 2바이트 읽기 분기 */
		cfg_ret = pci_user_read_config_word(dev, off, &word);	/* NVMe: NVMe config space 2바이트 읽기 */
		break;		/* NVMe: 2바이트 분기 종료 */
	case 4:		/* NVMe: 4바이트 읽기 분기 */
		cfg_ret = pci_user_read_config_dword(dev, off, &dword);	/* NVMe: NVMe config space 4바이트 읽기 */
		break;		/* NVMe: 4바이트 분기 종료 */
	default:		/* NVMe: 1/2/4 외 길이 처리 */
		err = -EINVAL;	/* NVMe: 1/2/4 바이트 외 요청은 잘못된 인자 */
		goto error;	/* NVMe: 에러 처리로 이동 */
	}			/* NVMe: 요청 길이별 config read 분기 종료 */

	err = -EIO;		/* NVMe: config access 하드웨어 오류 시 반환값 */
	if (cfg_ret)		/* NVMe: pci_user_read_*가 실패하면 */
		goto error;	/* NVMe: -EIO 반환 */

	switch (len) {		/* NVMe: 커널 버퍼에서 사용자 버퍼로 복사 */
	case 1:		/* NVMe: 1바이트 결과 복사 분기 */
		err = put_user(byte, (u8 __user *)buf);	/* NVMe: 1바이트 결과를 사용자 공간에 기록 */
		break;		/* NVMe: 1바이트 복사 분기 종료 */
	case 2:		/* NVMe: 2바이트 결과 복사 분기 */
		err = put_user(word, (u16 __user *)buf);	/* NVMe: 2바이트 결과를 사용자 공간에 기록 */
		break;		/* NVMe: 2바이트 복사 분기 종료 */
	case 4:		/* NVMe: 4바이트 결과 복사 분기 */
		err = put_user(dword, (u32 __user *)buf);	/* NVMe: 4바이트 결과를 사용자 공간에 기록 */
		break;		/* NVMe: 4바이트 복사 분기 종료 */
	}			/* NVMe: 사용자 공간 결과 복사 switch 종료 */
	pci_dev_put(dev);	/* NVMe: NVMe 장치 참조 카운트 감소 */
	return err;		/* NVMe: 성공(0) 또는 put_user 실패 코드 반환 */

error:				/* NVMe: 공통 에러 처리 레이블 */
	/* ??? XFree86 doesn't even check the return value.  They
	   just look for 0xffffffff in the output, since that's what
	   they get instead of a machine check on x86.  */
	switch (len) {		/* NVMe: 에러 시 사용자 버퍼에 0xffffffff 형태로 채워 반환 */
	case 1:		/* NVMe: 1바이트 에러 마커 분기 */
		put_user(-1, (u8 __user *)buf);	/* NVMe: 1바이트 에러 마커 기록 */
		break;		/* NVMe: 1바이트 에러 마커 분기 종료 */
	case 2:		/* NVMe: 2바이트 에러 마커 분기 */
		put_user(-1, (u16 __user *)buf);	/* NVMe: 2바이트 에러 마커 기록 */
		break;		/* NVMe: 2바이트 에러 마커 분기 종료 */
	case 4:		/* NVMe: 4바이트 에러 마커 분기 */
		put_user(-1, (u32 __user *)buf);	/* NVMe: 4바이트 에러 마커 기록 */
		break;		/* NVMe: 4바이트 에러 마커 분기 종료 */
	}			/* NVMe: 에러 마커 복사 switch 종료 */
	pci_dev_put(dev);	/* NVMe: 검색한 NVMe 장치 참조 해제(NULL이면 무시) */
	return err;		/* NVMe: 에러 코드 반환 */
}			/* NVMe: pciconfig_read 시스템 콜 종료 */

/*
 * pciconfig_write:
 *   사용자 공간에서 PCI config space에 값을 쓴다. NVMe 관점에서는
 *   lspci/setpci 등이 NVMe SSD의 PCIe 레지스터(예: PCIe Capability,
 *   ASPM 제어, MPS, AER/EDR 관련 필드)를 진단/변경할 때 사용될 수
 *   있다. 단 커널 NVMe 드라이버가 런타임에 직접 호출하지는 않는다.
 *   CAP_SYS_ADMIN과 LOCKDOWN_PCI_ACCESS 검사를 수행한다.
 */
SYSCALL_DEFINE5(pciconfig_write,	/* NVMe: pciconfig_write 시스템 콜 정의(5개 인자) */
		unsigned long, bus,	/* NVMe: 대상이 속한 PCI 버스 번호 */
		unsigned long, dfn,	/* NVMe: 대상 장치/함수 번호(devfn) */
		unsigned long, off,	/* NVMe: config space 내 바이트 오프셋 */
		unsigned long, len,	/* NVMe: 쓸 바이트 수(1/2/4) */
		void __user *, buf)	/* NVMe: 사용자 공간 원본 버퍼 */
{	/* NVMe: pciconfig_write 시스템 콜 본문 시작 */
	struct pci_dev *dev;	/* NVMe: 쓰기 대상 PCI 장치(NVMe SSD) 구조체 */
	u8 byte;		/* NVMe: 1바이트 쓰기 값 */
	u16 word;		/* NVMe: 2바이트 쓰기 값 */
	u32 dword;		/* NVMe: 4바이트 쓰기 값 */
	int err = 0;		/* NVMe: 기본 반환값(성공) */

	if (!capable(CAP_SYS_ADMIN) ||	/* NVMe: NVMe config write도 고권한 필요 */
	    security_locked_down(LOCKDOWN_PCI_ACCESS))	/* NVMe: lockdown 모드에서는 config 접근 차단(보안) */
		return -EPERM;	/* NVMe: 권한/lockdown 위반 시 즉시 반환 */

	dev = pci_get_domain_bus_and_slot(0, bus, dfn);	/* NVMe: domain 0에서 대상 NVMe 장치 탐색 */
	if (!dev)		/* NVMe: 장치가 존재하지 않으면 */
		return -ENODEV;	/* NVMe: -ENODEV 반환 */

	switch (len) {		/* NVMe: 요청 길이에 따라 쓰기 수행 */
	case 1:		/* NVMe: 1바이트 쓰기 분기 */
		err = get_user(byte, (u8 __user *)buf);	/* NVMe: 사용자 공간에서 1바이트 값 복사 */
		if (err)	/* NVMe: 복사 실패 시 */
			break;	/* NVMe: get_user 에러 코드(err) 반환 준비 */
		err = pci_user_write_config_byte(dev, off, byte);	/* NVMe: NVMe config space 1바이트 기록 */
		if (err)	/* NVMe: config write 실패 시 */
			err = -EIO;	/* NVMe: 하드웨어 I/O 오류로 변환 */
		break;		/* NVMe: 1바이트 쓰기 분기 종료 */

	case 2:		/* NVMe: 2바이트 쓰기 분기 */
		err = get_user(word, (u16 __user *)buf);	/* NVMe: 사용자 공간에서 2바이트 값 복사 */
		if (err)	/* NVMe: 사용자 공간 복사 실패 시 */
			break;	/* NVMe: get_user 에러 코드 반환 준비 */
		err = pci_user_write_config_word(dev, off, word);	/* NVMe: NVMe config space 2바이트 기록 */
		if (err)	/* NVMe: config write 실패 시 */
			err = -EIO;	/* NVMe: I/O 에러 변환 */
		break;		/* NVMe: 2바이트 쓰기 분기 종료 */

	case 4:		/* NVMe: 4바이트 쓰기 분기 */
		err = get_user(dword, (u32 __user *)buf);	/* NVMe: 사용자 공간에서 4바이트 값 복사 */
		if (err)	/* NVMe: 사용자 공간 복사 실패 시 */
			break;	/* NVMe: get_user 에러 코드 반환 준비 */
		err = pci_user_write_config_dword(dev, off, dword);	/* NVMe: NVMe config space 4바이트 기록 */
		if (err)	/* NVMe: config write 실패 시 */
			err = -EIO;	/* NVMe: I/O 에러 변환 */
		break;		/* NVMe: 4바이트 쓰기 분기 종료 */

	default:		/* NVMe: 1/2/4 외 길이 처리 */
		err = -EINVAL;	/* NVMe: 허용되지 않는 길이 */
		break;		/* NVMe: -EINVAL 반환 준비 */
	}			/* NVMe: 요청 길이별 config write 분기 종료 */
	pci_dev_put(dev);	/* NVMe: NVMe 장치 참조 해제 */
	return err;		/* NVMe: 성공(0) 또는 에러 코드 반환 */
}			/* NVMe: pciconfig_write 시스템 콜 종료 */
