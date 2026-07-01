// SPDX-License-Identifier: GPL-2.0
/*
 * For architectures where we want to allow direct access to the PCI config
 * stuff - it would probably be preferable on PCs too, but there people
 * just do it by hand with the magic northbridge registers.
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/syscall.c)은 사용자 공간에서 PCI 장치의 설정
 * 공간(configuration space)에 직접 접근할 수 있게 하는 시스템 콜
 * (pciconfig_read/pciconfig_write)을 구현한다.
 *
 * NVMe SSD 입장에서 본 파일은 다음과 같은 의미를 갖는다.
 *   - NVMe 장치의 PCIe config space(Vendor ID, Device ID, Class Code,
 *     BAR, Capabilities 등)를 사용자 공간 유틸리티(lspci/setpci 등)가
 *     읽고 쓸 수 있는 통로를 제공한다.
 *   - 커널 낸 NVMe 드라이버(drivers/nvme/host/pci.c)는 직접 이
 *     시스템 콜을 호출하지 않고, 대신 pci_read_config_*/pci_write_config_*
 *     계열 함수를 사용한다. 그러나 본 파일이 제공하는 권한 검사와
 *     config access 인터페이스는 NVMe 장치의 config space 탐색/진단에
 *     사용된다.
 *   - 전원 제어, EDR(Error Detection and Isolation), RCEC, slot 등 NVMe
 *     장치의 PCIe 물리적 환경을 진단하거나 제어하는 도구들이 config
 *     space를 통해 Root Port/Downstream Switch/NVMe endpoint 상태를
 *     확인할 때 이 시스템 콜을 거친다.
 *
 * 주요 호출 경로:
 *   사용자 공간(lspci/setpci) -> pciconfig_read/pciconfig_write ->
 *   pci_get_domain_bus_and_slot() -> pci_user_read/write_config_* ->
 *   NVMe SSD의 PCIe config space
 * ===================================================================
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
{
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
	}

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
	}
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
	}
	pci_dev_put(dev);	/* NVMe: 검색한 NVMe 장치 참조 해제(NULL이면 무시) */
	return err;		/* NVMe: 에러 코드 반환 */
}

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
{
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

	default:
		err = -EINVAL;	/* NVMe: 허용되지 않는 길이 */
		break;		/* NVMe: -EINVAL 반환 준비 */
	}
	pci_dev_put(dev);	/* NVMe: NVMe 장치 참조 해제 */
	return err;		/* NVMe: 성공(0) 또는 에러 코드 반환 */
}
