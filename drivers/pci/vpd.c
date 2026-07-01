// SPDX-License-Identifier: GPL-2.0
/*
 * PCI VPD support
 *
 * Copyright (C) 2010 Broadcom Corporation.
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/vpd.c)은 PCI Vital Product Data(VPD)에 대한
 * 읽기/쓰기, 크기 탐지, sysfs 인터페이스, 그리고 관련 quirk를
 * 구현한다. VPD는 PCIe 장치의 EEPROM에 저장된 제조사/모델/일련번호
 * 등 제품 식별 정보를 담고 있으며, NVMe SSD 역시 PCIe endpoint로서
 * VPD를 노출할 수 있다.
 *
 * NVMe 호스트 드라이버(drivers/nvme/host/pci.c)가 직접 VPD 함수를
 * 호출하지는 않지만, NVMe 장치의 PCIe 수준 초기화, sysfs(/vpd),
 * 그리고 다기능(Multi-Function) NVMe 컨트롤러의 VPD 라우팅 quirk에서
 * 본 파일의 기능이 사용된다. 특히:
 *   - pci_vpd_init(): NVMe 장치 probe 시 VPD capability를 찾아 lock 초기화
 *   - pci_vpd_alloc(), pci_read_vpd(): /sys/bus/pci/devices/.../vpd 읽기
 *   - quirk_f0_vpd_link(): Multi-Function NVMe 등에서 function 0을 통해
 *     VPD에 접근하도록 라우팅
 *   - quirk_blacklist_vpd(): 비표준 VPD 형식으로 인해 NVMe 장치가
 *     비정상 동작할 수 있는 경우 접근 차단
 *   - pci_vpd_check_csum(): VPD checksum 검증(PCIe 장치 신뢰성 확인)
 *
 * VPD 접근은 config space의 VPD capability를 통해 이루어지며, serial
 * EEPROM에 연결된 경우 수 ms 이상 지연될 수 있어 pci_vpd_wait()에서
 * 플래그 폴링을 수행한다. NVMe 장치의 에러 복구나 PME 처리 중에도
 * sysfs를 통한 VPD 접근이 가능하므로, 런타임 전원 관리
 * (pci_config_pm_runtime_get/put)와 mutex lock으로 동시 접근을 보호한다.
 * ===================================================================
 */

#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/sched/signal.h>
#include <linux/unaligned.h>
#include "pci.h"

#define PCI_VPD_LRDT_TAG_SIZE		3 /* NVMe: Large Resource Data Type 태그 크기(태그 1바이트 + 길이 2바이트). */
#define PCI_VPD_SRDT_LEN_MASK		0x07 /* NVMe: Short Resource Data Type 태그 내 길이 필드 마스크. */
#define PCI_VPD_SRDT_TAG_SIZE		1 /* NVMe: Short Resource Data Type 태그 크기(1바이트). */
#define PCI_VPD_STIN_END		0x0f /* NVMe: VPD End 태그의 tag 값. */
#define PCI_VPD_INFO_FLD_HDR_SIZE	3 /* NVMe: VPD 정보 필드 헤더 크기(키워드 2바이트 + 길이 1바이트). */

/*
 * pci_vpd_lrdt_size:
 *   VPD Large Resource Data Type 항목의 데이터 길이를 추출한다.
 *   NVMe 장치의 VPD EEPROM에서 모델명/제조사 등 큰 데이터 블록 크기 파악에 사용.
 */
static u16 pci_vpd_lrdt_size(const u8 *lrdt)
{
	return get_unaligned_le16(lrdt + 1); /* NVMe: 태그 다음 2바이트를 little-endian으로 읽어 데이터 길이 반환. */
}

/*
 * pci_vpd_srdt_tag:
 *   VPD Short Resource Data Type 태그 번호를 추출한다.
 *   NVMe VPD의 End tag 같은 짧은 항목 식별에 사용.
 */
static u8 pci_vpd_srdt_tag(const u8 *srdt)
{
	return *srdt >> 3; /* NVMe: 상위 5비트를 시프트하여 short 태그 번호 반환. */
}

/*
 * pci_vpd_srdt_size:
 *   VPD Short Resource Data Type 항목의 데이터 길이를 추출한다.
 */
static u8 pci_vpd_srdt_size(const u8 *srdt)
{
	return *srdt & PCI_VPD_SRDT_LEN_MASK; /* NVMe: 하위 3비트를 마스크하여 short 데이터 길이 반환. */
}

/*
 * pci_vpd_info_field_size:
 *   VPD 정보 필드의 실제 데이터 길이를 추출한다.
 *   NVMe 장치 VPD 내 제품 일련번호(PN), 제조사(VN) 등 키워드 항목 길이 파악에 사용.
 */
static u8 pci_vpd_info_field_size(const u8 *info_field)
{
	return info_field[2]; /* NVMe: 헤더 3번째 바이트가 정보 필드의 데이터 길이. */
}

/* VPD access through PCI 2.2+ VPD capability */

/*
 * pci_get_func0_dev:
 *   현재 PCIe function과 동일한 슬롯의 function 0 장치를 참조 획득한다.
 *   Multi-Function NVMe 컨트롤러에서 VPD가 function 0에 공유되어 있을 때
 *   function 0을 통해 VPD에 접근하기 위해 사용된다.
 */
static struct pci_dev *pci_get_func0_dev(struct pci_dev *dev)
{
	return pci_get_slot(dev->bus, PCI_DEVFN(PCI_SLOT(dev->devfn), 0)); /* NVMe: 동일 bus/slot의 function 0 pci_dev 획득. */
}

#define PCI_VPD_MAX_SIZE	(PCI_VPD_ADDR_MASK + 1) /* NVMe: VPD 최대 접근 가능 크기(일반적으로 32KB). */
#define PCI_VPD_SZ_INVALID	UINT_MAX /* NVMe: VPD 크기를 알 수 없음/접근 금지를 나타내는 특수값. */

/*
 * pci_vpd_size:
 *   NVMe 장치의 VPD EEPROM에서 Large/Short Resource Data Type 태그를
 *   순회하며 실제 VPD 데이터 크기를 결정한다. End tag(0x0f)를 만나면
 *   종료한다. NVMe SSD의 /sys/.../vpd 노출 범위나 pci_vpd_alloc()의
 *   버퍼 크기 계산에 직접 사용된다.
 */
/**
 * pci_vpd_size - determine actual size of Vital Product Data
 * @dev:	pci device struct
 */
static size_t pci_vpd_size(struct pci_dev *dev)
{
	size_t off = 0, size; /* NVMe: 현재 VPD 오프셋과 태그 데이터 크기 변수. */
	unsigned char tag, header[1+2];	/* 1 byte tag, 2 bytes length */ /* NVMe: VPD 태그 헤더 버퍼(태그1 + 길이2). */

	while (pci_read_vpd_any(dev, off, 1, header) == 1) { /* NVMe: 현재 오프셋에서 1바이트 태그를 읽는다. */
		size = 0; /* NVMe: 새 태그 크기 초기화. */

		if (off == 0 && (header[0] == 0x00 || header[0] == 0xff)) /* NVMe: VPD 시작이 0x00/0xff면 유효하지 않은 EEPROM으로 간주. */
			goto error; /* NVMe: 비정상 시작값이면 에러 처리로 이동. */

		if (header[0] & PCI_VPD_LRDT) { /* NVMe: 최상위 비트가 1이면 Large Resource Data Type. */
			/* Large Resource Data Type Tag */
			if (pci_read_vpd_any(dev, off + 1, 2, &header[1]) != 2) { /* NVMe: Large 태그의 2바이트 길이 필드 읽기. */
				pci_warn(dev, "failed VPD read at offset %zu\n",
					 off + 1); /* NVMe: 길이 필드 읽기 실패 시 경고 출력. */
				return off ?: PCI_VPD_SZ_INVALID; /* NVMe: 이미 얻은 off가 있으면 off, 없으면 INVALID 반환. */
			}
			size = pci_vpd_lrdt_size(header); /* NVMe: Large 태그의 데이터 길이 계산. */
			if (off + size > PCI_VPD_MAX_SIZE) /* NVMe: VPD 최대 크기를 초과하면 에러. */
				goto error; /* NVMe: 크기 초과 시 에러 처리. */

			off += PCI_VPD_LRDT_TAG_SIZE + size; /* NVMe: 다음 태그 위치로 오프셋 이동(태그3 + 데이터). */
		} else {
			/* Short Resource Data Type Tag */
			tag = pci_vpd_srdt_tag(header); /* NVMe: Short 태그 번호 추출. */
			size = pci_vpd_srdt_size(header); /* NVMe: Short 태그 데이터 길이 추출. */
			if (off + size > PCI_VPD_MAX_SIZE) /* NVMe: Short 태그도 최대 크기 초과 검사. */
				goto error; /* NVMe: 초과 시 에러 처리. */

			off += PCI_VPD_SRDT_TAG_SIZE + size; /* NVMe: 다음 태그 위치로 오프셋 이동(태그1 + 데이터). */
			if (tag == PCI_VPD_STIN_END)	/* End tag descriptor */ /* NVMe: End tag를 만나면 VPD 끝. */
				return off; /* NVMe: 총 VPD 크기 반환. */
		}
	}
	return off; /* NVMe: End tag 없이 끝까지 읽은 경우 현재 오프셋 반환. */

error:
	pci_info(dev, "invalid VPD tag %#04x (size %zu) at offset %zu%s\n",
		 header[0], size, off, off == 0 ?
		 "; assume missing optional EEPROM" : ""); /* NVMe: 잘못된 VPD 태그 정보 로그. */
	return off ?: PCI_VPD_SZ_INVALID; /* NVMe: off가 0이면 optional EEPROM 누락으로 가정. */
}

/*
 * pci_vpd_available:
 *   NVMe 장치에 VPD capability가 있고, 필요한 경우 유효한 VPD 크기가
 *   결정되어 있는지 확인한다. 크기가 결정되지 않았으면 pci_vpd_size()를
 *   호출하여 측정한다.
 */
static bool pci_vpd_available(struct pci_dev *dev, bool check_size)
{
	struct pci_vpd *vpd = &dev->vpd; /* NVMe: NVMe pci_dev의 VPD 구조체 포인터. */

	if (!vpd->cap) /* NVMe: VPD capability가 없으면 VPD 접근 불가. */
		return false; /* NVMe: VPD 미지원 장치. */

	if (vpd->len == 0 && check_size) { /* NVMe: 아직 VPD 크기를 측정하지 않았고 크기 확인이 필요하면. */
		vpd->len = pci_vpd_size(dev); /* NVMe: VPD 크기 측정 수행. */
		if (vpd->len == PCI_VPD_SZ_INVALID) { /* NVMe: 크기 측정 실패(비표준/누락). */
			vpd->cap = 0; /* NVMe: 이후 VPD 접근을 막기 위해 capability 값을 0으로 만든다. */
			return false; /* NVMe: VPD 사용 불가. */
		}
	}

	return true; /* NVMe: VPD 접근 가능. */
}

/*
 * Wait for last operation to complete.
 * This code has to spin since there is no other notification from the PCI
 * hardware. Since the VPD is often implemented by serial attachment to an
 * EEPROM, it may take many milliseconds to complete.
 * @set: if true wait for flag to be set, else wait for it to be cleared
 *
 * Returns 0 on success, negative values indicate error.
 */
/*
 * pci_vpd_wait:
 *   VPD ADDR 레지스터의 F(Flag) 비트가 원하는 상태가 될 때까지 폴리한다.
 *   NVMe 장치의 VPD EEPROM은 serial 인터페이스로 연결되어 있어 읽기/쓰기
 *   완료까지 수 ms 걸릴 수 있으며, 이 함수에서 최대 125ms 동안 대기한다.
 *   PME나 D3cold 복구 후에도 VPD 접근 시 이 완료 대기가 필요하다.
 */
static int pci_vpd_wait(struct pci_dev *dev, bool set)
{
	struct pci_vpd *vpd = &dev->vpd; /* NVMe: NVMe pci_dev의 VPD 상태 구조체. */
	unsigned long timeout = jiffies + msecs_to_jiffies(125); /* NVMe: 125ms 타임아웃 설정. */
	unsigned long max_sleep = 16; /* NVMe: usleep_range 최대값, 1024us까지 점점 늘린다. */
	u16 status; /* NVMe: VPD ADDR 레지스터 값. */
	int ret; /* NVMe: config read 반환값. */

	do {
		ret = pci_user_read_config_word(dev, vpd->cap + PCI_VPD_ADDR,
						&status); /* NVMe: VPD ADDR 레지스터에서 F 비트 읽기. */
		if (ret < 0) /* NVMe: config read 실패 시. */
			return ret; /* NVMe: 에러 코드 즉시 반환. */

		if (!!(status & PCI_VPD_ADDR_F) == set) /* NVMe: F 비트가 원하는 상태이면 완료. */
			return 0; /* NVMe: VPD 작업 완료. */

		if (time_after(jiffies, timeout)) /* NVMe: 타임아웃 경과 확인. */
			break; /* NVMe: 시간 초과 시 루프 탈출. */

		usleep_range(10, max_sleep); /* NVMe: CPU를 점유하지 않도록 짧게 대기. */
		if (max_sleep < 1024) /* NVMe: 최대 1024us까지 점진적으로 대기 간격 증가. */
			max_sleep *= 2; /* NVMe: exponential backoff로 max_sleep 2배 증가. */
	} while (true);

	pci_warn(dev, "VPD access failed.  This is likely a firmware bug on this device.  Contact the card vendor for a firmware update\n"); /* NVMe: NVMe 장치 펌웨어 버그 가능성 경고. */
	return -ETIMEDOUT; /* NVMe: 타임아웃 에러 반환. */
}

/*
 * pci_vpd_read:
 *   NVMe 장치의 VPD capability를 통해 count 바이트를 읽어 buf에 저장한다.
 *   sysfs /vpd 파일이나 pci_vpd_alloc()에서 호출되며, mutex로 동시 접근을
 *   보호하고 pos가 4바이트 정렬이 아닐 경우에도 바이트 단위로 처리한다.
 */
static ssize_t pci_vpd_read(struct pci_dev *dev, loff_t pos, size_t count,
			    void *arg, bool check_size)
{
	struct pci_vpd *vpd = &dev->vpd; /* NVMe: NVMe 장치의 VPD 구조체. */
	unsigned int max_len; /* NVMe: 접근 가능한 최대 VPD 길이. */
	int ret = 0; /* NVMe: 루프 내 반환값. */
	loff_t end = pos + count; /* NVMe: 읽기 종료 오프셋. */
	u8 *buf = arg; /* NVMe: 사용자 버퍼 포인터. */

	if (!pci_vpd_available(dev, check_size)) /* NVMe: VPD capability/크기 확인. */
		return -ENODEV; /* NVMe: VPD를 사용할 수 없음. */

	if (pos < 0) /* NVMe: 음수 오프셋은 유효하지 않음. */
		return -EINVAL; /* NVMe: 잘못된 인자. */

	max_len = check_size ? vpd->len : PCI_VPD_MAX_SIZE; /* NVMe: 크기 검사 여부에 따라 최대 길이 결정. */

	if (pos >= max_len) /* NVMe: 시작 오프셋이 VPD 끝 이상이면 읽을 내용 없음. */
		return 0; /* NVMe: 0바이트 반환. */

	if (end > max_len) { /* NVMe: 요청 범위가 VPD 크기를 초과하면. */
		end = max_len; /* NVMe: 종료 위치를 VPD 끝으로 제한. */
		count = end - pos; /* NVMe: 실제 읽을 바이트 수 조정. */
	}

	if (mutex_lock_killable(&vpd->lock)) /* NVMe: VPD 동시 접근 방지용 lock 획득. */
		return -EINTR; /* NVMe: 시그널로 인해 lock 획득 실패. */

	while (pos < end) { /* NVMe: 요청한 범위만큼 4바이트 단위로 읽기. */
		u32 val; /* NVMe: VPD DATA 레지스터에서 읽은 4바이트 값. */
		unsigned int i, skip; /* NVMe: 루프 인덱스와 정렬되지 않은 시작 바이트 수. */

		if (fatal_signal_pending(current)) { /* NVMe: 프로세스 종료 시그널 확인. */
			ret = -EINTR; /* NVMe: 인터럽트 처리. */
			break; /* NVMe: 읽기 루프 종료. */
		}

		ret = pci_user_write_config_word(dev, vpd->cap + PCI_VPD_ADDR,
						 pos & ~3); /* NVMe: 4바이트 정렬된 VPD 주소를 ADDR 레지스터에 기록. */
		if (ret < 0) /* NVMe: 주소 쓰기 실패. */
			break; /* NVMe: 루프 종료. */
		ret = pci_vpd_wait(dev, true); /* NVMe: VPD 하드웨어가 읽기 완료(F 비트 set)까지 대기. */
		if (ret < 0) /* NVMe: 대기 중 타임아웃/에러. */
			break; /* NVMe: 루프 종료. */

		ret = pci_user_read_config_dword(dev, vpd->cap + PCI_VPD_DATA, &val); /* NVMe: VPD DATA 레지스터에서 4바이트 읽기. */
		if (ret < 0) /* NVMe: 데이터 읽기 실패. */
			break; /* NVMe: 루프 종료. */

		skip = pos & 3; /* NVMe: 현재 오프셋의 정렬되지 않은 바이트 수 계산. */
		for (i = 0;  i < sizeof(u32); i++) { /* NVMe: 4바이트 내에서 필요한 바이트만 복사. */
			if (i >= skip) { /* NVMe: skip 바이트 이후부터 사용자 버퍼에 저장. */
				*buf++ = val; /* NVMe: 하위 바이트부터 순서대로 버퍼에 기록. */
				if (++pos == end) /* NVMe: 마지막 바이트까지 읽었으면. */
					break; /* NVMe: 남은 바이트 복사 중단. */
			}
			val >>= 8; /* NVMe: 다음 바이트로 시프트. */
		}
	}

	mutex_unlock(&vpd->lock); /* NVMe: VPD lock 해제. */
	return ret ? ret : count; /* NVMe: 에러가 있으면 에러 코드, 아니면 읽은 바이트 수 반환. */
}

/*
 * pci_vpd_write:
 *   NVMe 장치의 VPD EEPROM에 count 바이트를 쓴다. 쓰기는 4바이트 정렬
 *   단위로 이루어지며, ADDR 레지스터에 F 비트를 set하여 쓰기 동작을
 *   시작하고, F 비트가 clear될 때까지 pci_vpd_wait()로 대기한다.
 *   NVMe 장치의 VPD 업데이트(펌웨어 업데이트 전/후 설정 등) 시 사용.
 */
static ssize_t pci_vpd_write(struct pci_dev *dev, loff_t pos, size_t count,
			     const void *arg, bool check_size)
{
	struct pci_vpd *vpd = &dev->vpd; /* NVMe: NVMe 장치의 VPD 구조체. */
	unsigned int max_len; /* NVMe: 최대 쓰기 가능 길이. */
	const u8 *buf = arg; /* NVMe: 쓰기 데이터 버퍼 포인터. */
	loff_t end = pos + count; /* NVMe: 쓰기 종료 오프셋. */
	int ret = 0; /* NVMe: 루프 반환값. */

	if (!pci_vpd_available(dev, check_size)) /* NVMe: VPD 사용 가능 여부 확인. */
		return -ENODEV; /* NVMe: VPD 미지원. */

	if (pos < 0 || (pos & 3) || (count & 3)) /* NVMe: 쓰기는 4바이트 정렬과 길이 제한이 필요. */
		return -EINVAL; /* NVMe: 정렬/길이 오류. */

	max_len = check_size ? vpd->len : PCI_VPD_MAX_SIZE; /* NVMe: 크기 검사 여부에 따른 최대 길이. */

	if (end > max_len) /* NVMe: 쓰기 범위가 VPD 크기를 초과하면. */
		return -EINVAL; /* NVMe: 범위 초과 오류. */

	if (mutex_lock_killable(&vpd->lock)) /* NVMe: VPD lock 획득. */
		return -EINTR; /* NVMe: lock 획득 중 시그널. */

	while (pos < end) { /* NVMe: 4바이트 단위로 VPD 쓰기 루프. */
		ret = pci_user_write_config_dword(dev, vpd->cap + PCI_VPD_DATA,
						  get_unaligned_le32(buf)); /* NVMe: 4바이트 데이터를 VPD DATA 레지스터에 기록. */
		if (ret < 0) /* NVMe: 데이터 쓰기 실패. */
			break; /* NVMe: 루프 종료. */
		ret = pci_user_write_config_word(dev, vpd->cap + PCI_VPD_ADDR,
						 pos | PCI_VPD_ADDR_F); /* NVMe: 쓰기 주소와 F 비트 set으로 쓰기 시작. */
		if (ret < 0) /* NVMe: 주소 쓰기 실패. */
			break; /* NVMe: 루프 종료. */

		ret = pci_vpd_wait(dev, false); /* NVMe: F 비트가 clear될 때(쓰기 완료)까지 대기. */
		if (ret < 0) /* NVMe: 쓰기 완료 대기 실패. */
			break; /* NVMe: 루프 종료. */

		buf += sizeof(u32); /* NVMe: 다음 4바이트 데이터로 포인터 이동. */
		pos += sizeof(u32); /* NVMe: 다음 4바이트 주소로 오프셋 이동. */
	}

	mutex_unlock(&vpd->lock); /* NVMe: VPD lock 해제. */
	return ret ? ret : count; /* NVMe: 에러 시 에러 코드, 아니면 쓴 바이트 수 반환. */
}

/*
 * pci_vpd_init:
 *   NVMe 장치 probe 단계에서 VPD capability를 검색하고, VPD 접근을 위한
 *   mutex를 초기화한다. NVMe pci_dev의 vpd.cap이 설정되어야 sysfs /vpd
 *   파일이 노출되고, pci_read_vpd/write_vpd가 동작한다.
 */
void pci_vpd_init(struct pci_dev *dev)
{
	if (dev->vpd.len == PCI_VPD_SZ_INVALID) /* NVMe: quirk 등에서 VPD를 블랙리스트한 경우 초기화하지 않는다. */
		return; /* NVMe: VPD 접근 금지 상태이므로 early return. */

	dev->vpd.cap = pci_find_capability(dev, PCI_CAP_ID_VPD); /* NVMe: PCI config space에서 VPD capability 위치 탐색. */
	mutex_init(&dev->vpd.lock); /* NVMe: VPD 동시 접근 보호용 mutex 초기화. */
}

/*
 * vpd_read:
 *   sysfs /sys/bus/pci/devices/<NVMe BDF>/vpd 파일의 read 콜백.
 *   사용자가 cat 등으로 NVMe 장치의 VPD를 읽을 때 호출된다.
 *   PCI_DEV_FLAGS_VPD_REF_F0가 설정된 Multi-Function NVMe의 경우
 *   function 0을 통해 VPD를 읽는다. 런타임 전원 관리 참조를 획득하여
 *   NVMe 장치가 D3cold 등에서 깨어날 수 있도록 한다.
 */
static ssize_t vpd_read(struct file *filp, struct kobject *kobj,
			const struct bin_attribute *bin_attr, char *buf,
			loff_t off, size_t count)
{
	struct pci_dev *dev = to_pci_dev(kobj_to_dev(kobj)); /* NVMe: sysfs kobject에서 NVMe pci_dev 획득. */
	struct pci_dev *vpd_dev = dev; /* NVMe: 기본적으로 현재 function에서 VPD 접근. */
	ssize_t ret; /* NVMe: read 반환값. */

	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0) { /* NVMe: VPD가 function 0에 공유된 quirk가 적용되었는지 확인. */
		vpd_dev = pci_get_func0_dev(dev); /* NVMe: Multi-Function NVMe의 function 0 pci_dev 획득. */
		if (!vpd_dev) /* NVMe: function 0이 없으면. */
			return -ENODEV; /* NVMe: VPD 접근 불가. */
	}

	pci_config_pm_runtime_get(vpd_dev); /* NVMe: VPD 접근 중 NVMe 장치의 런타임 전원 활성화. */
	ret = pci_read_vpd(vpd_dev, off, count, buf); /* NVMe: function 0 또는 현재 function의 VPD 읽기. */
	pci_config_pm_runtime_put(vpd_dev); /* NVMe: 런타임 전원 참조 해제. */

	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0) /* NVMe: function 0 참조를 획득했었다면. */
		pci_dev_put(vpd_dev); /* NVMe: function 0 pci_dev 참조 카운트 감소. */

	return ret; /* NVMe: 읽은 바이트 수 또는 에러 반환. */
}

/*
 * vpd_write:
 *   sysfs /sys/bus/pci/devices/<NVMe BDF>/vpd 파일의 write 콜백.
 *   관리자가 NVMe 장치의 VPD EEPROM에 데이터를 쓸 때 사용된다.
 *   Multi-Function NVMe에서 VPD 공유 quirk가 적용된 경우 function 0을
 *   통해 쓰기를 수행하며, 런타임 전원 관리를 통해 장치를 활성 상태로 유지.
 */
static ssize_t vpd_write(struct file *filp, struct kobject *kobj,
			 const struct bin_attribute *bin_attr, char *buf,
			loff_t off, size_t count)
{
	struct pci_dev *dev = to_pci_dev(kobj_to_dev(kobj)); /* NVMe: sysfs kobject에서 NVMe pci_dev 획득. */
	struct pci_dev *vpd_dev = dev; /* NVMe: 기본적으로 현재 function에서 VPD 접근. */
	ssize_t ret; /* NVMe: write 반환값. */

	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0) { /* NVMe: function 0 VPD 라우팅 quirk 확인. */
		vpd_dev = pci_get_func0_dev(dev); /* NVMe: Multi-Function NVMe의 function 0 획득. */
		if (!vpd_dev) /* NVMe: function 0 획득 실패. */
			return -ENODEV; /* NVMe: VPD 쓰기 불가. */
	}

	pci_config_pm_runtime_get(vpd_dev); /* NVMe: VPD 쓰기 중 NVMe 장치 런타임 전원 활성화. */
	ret = pci_write_vpd(vpd_dev, off, count, buf); /* NVMe: VPD 쓰기 수행. */
	pci_config_pm_runtime_put(vpd_dev); /* NVMe: 런타임 전원 참조 해제. */

	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0) /* NVMe: function 0 참조를 획득한 경우. */
		pci_dev_put(vpd_dev); /* NVMe: function 0 참조 카운트 감소. */

	return ret; /* NVMe: 쓴 바이트 수 또는 에러 반환. */
}
/*
 * BIN_ATTR(vpd, 0600, ...):
 *   NVMe 장치의 /sys/bus/pci/devices/<BDF>/vpd 바이너리 sysfs 속성을
 *   정의한다. 소유주는 읽기/쓰기 모두 가능(0600), 다른 사용자는 접근 불가.
 */
static const BIN_ATTR(vpd, 0600, vpd_read, vpd_write, 0); /* NVMe: NVMe sysfs /vpd 파일 속성 정의. */

/*
 * vpd_attrs:
 *   NVMe 장치에 노출할 VPD 바이너리 속성 배열. pci_dev_vpd_attr_group에
 *   등록되어 sysfs tree에 /vpd 파일을 만든다.
 */
static const struct bin_attribute *const vpd_attrs[] = {
	&bin_attr_vpd, /* NVMe: /vpd sysfs 파일 하나 등록. */
	NULL, /* NVMe: 배열 종료 표시. */
};

/*
 * vpd_attr_is_visible:
 *   NVMe 장치의 sysfs 그룹 등록 시 VPD capability가 있는 경우에만
 *   /vpd 파일을 노출한다. vpd.cap이 0이면 사용자에게 보이지 않는다.
 */
static umode_t vpd_attr_is_visible(struct kobject *kobj,
				   const struct bin_attribute *a, int n)
{
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj)); /* NVMe: sysfs kobject에서 NVMe pci_dev 획득. */

	if (!pdev->vpd.cap) /* NVMe: VPD capability가 없으면 /vpd 파일을 숨김. */
		return 0; /* NVMe: visibility 0 -> sysfs에 노출되지 않음. */

	return a->attr.mode; /* NVMe: VPD capability가 있으면 0600 모드로 노출. */
}

/*
 * pci_dev_vpd_attr_group:
 *   NVMe pci_dev가 sysfs에 등록할 때 VPD 속성 그룹. bin_attrs에 vpd를
 *   등록하고, is_bin_visible 콜백으로 capability 유무에 따라 노출 제어.
 */
const struct attribute_group pci_dev_vpd_attr_group = {
	.bin_attrs = vpd_attrs, /* NVMe: /vpd 바이너리 속성 배열 연결. */
	.is_bin_visible = vpd_attr_is_visible, /* NVMe: capability 기반 노출 결정. */
};

/*
 * pci_vpd_alloc:
 *   NVMe 장치의 전체 VPD 데이터를 커널 메모리에 할당하여 반환한다.
 *   NVMe 드라이버나 기타 서브시스템이 VPD를 파싱하거나 checksum 검증
 *   시 사용한다. 반환된 버퍼는 호출자가 kfree()로 해제해야 한다.
 */
void *pci_vpd_alloc(struct pci_dev *dev, unsigned int *size)
{
	unsigned int len; /* NVMe: VPD 전체 길이. */
	void *buf; /* NVMe: VPD 데이터를 저장할 커널 버퍼. */
	int cnt; /* NVMe: 실제 읽은 바이트 수. */

	if (!pci_vpd_available(dev, true)) /* NVMe: VPD 사용 가능 여부 및 크기 측정. */
		return ERR_PTR(-ENODEV); /* NVMe: VPD 미지원 시 에러 포인터 반환. */

	len = dev->vpd.len; /* NVMe: 측정된 VPD 크기. */
	buf = kmalloc(len, GFP_KERNEL); /* NVMe: VPD 크기만큼 커널 메모리 할당. */
	if (!buf) /* NVMe: 메모리 할당 실패. */
		return ERR_PTR(-ENOMEM); /* NVMe: 메모리 부족 에러. */

	cnt = pci_read_vpd(dev, 0, len, buf); /* NVMe: VPD 시작부터 전체 길이만큼 읽기. */
	if (cnt != len) { /* NVMe: 요청한 길이만큼 읽지 못하면. */
		kfree(buf); /* NVMe: 할당한 버퍼 해제. */
		return ERR_PTR(-EIO); /* NVMe: I/O 에러 반환. */
	}

	if (size) /* NVMe: 호출자가 크기를 원하면. */
		*size = len; /* NVMe: 읽은 VPD 길이를 출력 인자에 저장. */

	return buf; /* NVMe: VPD 데이터 버퍼 반환. */
}
EXPORT_SYMBOL_GPL(pci_vpd_alloc); /* NVMe: NVMe 등 다른 드라이버에서 pci_vpd_alloc 사용 가능. */

/*
 * pci_vpd_find_tag:
 *   NVMe 장치의 VPD 버퍼에서 지정한 Large Resource Data Type 태그를
 *   검색한다. ID String, Read-Only Data, Read-Write Data 등의 시작
 *   오프셋을 찾는 데 사용된다.
 */
static int pci_vpd_find_tag(const u8 *buf, unsigned int len, u8 rdt, unsigned int *size)
{
	int i = 0; /* NVMe: VPD 버퍼 내 검색 인덱스. */

	/* look for LRDT tags only, end tag is the only SRDT tag */
	while (i + PCI_VPD_LRDT_TAG_SIZE <= len && buf[i] & PCI_VPD_LRDT) { /* NVMe: Large 태그만 순회, End tag(SRDT) 전까지. */
		unsigned int lrdt_len = pci_vpd_lrdt_size(buf + i); /* NVMe: 현재 Large 태그의 데이터 길이. */
		u8 tag = buf[i]; /* NVMe: 현재 태그 값 저장. */

		i += PCI_VPD_LRDT_TAG_SIZE; /* NVMe: 태그 헤더(3바이트) 건. */
		if (tag == rdt) { /* NVMe: 찾으려는 태그(rdt)와 일치하면. */
			if (i + lrdt_len > len) /* NVMe: 길이가 버퍼를 초과하면. */
				lrdt_len = len - i; /* NVMe: 버퍼 남은 크기로 제한. */
			if (size) /* NVMe: 호출자가 데이터 길이를 원하면. */
				*size = lrdt_len; /* NVMe: 태그 데이터 길이 저장. */
			return i; /* NVMe: 태그 데이터 시작 오프셋 반환. */
		}

		i += lrdt_len; /* NVMe: 다음 Large 태그 위치로 이동. */
	}

	return -ENOENT; /* NVMe: 태그를 찾지 못함. */
}

/*
 * pci_vpd_find_id_string:
 *   NVMe 장치 VPD에서 PCI_VPD_LRDT_ID_STRING 태그를 찾아 제품 식별
 *   문자열의 시작 오프셋을 반환한다. NVMe SSD 모델명/제조사 식별에 사용.
 */
int pci_vpd_find_id_string(const u8 *buf, unsigned int len, unsigned int *size)
{
	return pci_vpd_find_tag(buf, len, PCI_VPD_LRDT_ID_STRING, size); /* NVMe: ID String 태그 검색. */
}
EXPORT_SYMBOL_GPL(pci_vpd_find_id_string); /* NVMe: 외부 모듈에서 NVMe VPD ID String 검색 가능. */

/*
 * pci_vpd_find_info_keyword:
 *   NVMe 장치 VPD의 Read-Only/Read-Write 데이터 영역 내에서 2글자
 *   키워드(PN, VN, EC 등)를 검색한다. 호출자가 영역 시작 off와 길이 len을
 *   지정하면 해당 범위 내에서 키워드를 찾는다.
 */
static int pci_vpd_find_info_keyword(const u8 *buf, unsigned int off,
			      unsigned int len, const char *kw)
{
	int i; /* NVMe: 키워드 검색 인덱스. */

	for (i = off; i + PCI_VPD_INFO_FLD_HDR_SIZE <= off + len;) { /* NVMe: 지정된 VPD 영역 내를 순회. */
		if (buf[i + 0] == kw[0] && /* NVMe: 키워드 첫 번째 문자 비교. */
		    buf[i + 1] == kw[1]) /* NVMe: 키워드 두 번째 문자 비교. */
			return i; /* NVMe: 키워드 위치 반환. */

		i += PCI_VPD_INFO_FLD_HDR_SIZE +
		     pci_vpd_info_field_size(&buf[i]); /* NVMe: 현재 필드 크기만큼 건. */
	}

	return -ENOENT; /* NVMe: 키워드를 찾지 못함. */
}

/*
 * __pci_read_vpd:
 *   NVMe 장치의 VPD를 읽는 낮은 수준 함수로, Multi-Function quirk에
 *   따라 function 0으로 라우팅하거나 현재 function에서 직접 읽는다.
 *   check_size가 true면 VPD 크기를 초과하지 않도록 검사한다.
 */
static ssize_t __pci_read_vpd(struct pci_dev *dev, loff_t pos, size_t count, void *buf,
			      bool check_size)
{
	ssize_t ret; /* NVMe: read 반환값. */

	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0) { /* NVMe: function 0 VPD 라우팅 quirk 확인. */
		dev = pci_get_func0_dev(dev); /* NVMe: Multi-Function NVMe의 function 0 획득. */
		if (!dev) /* NVMe: function 0이 없으면. */
			return -ENODEV; /* NVMe: VPD 읽기 불가. */

		ret = pci_vpd_read(dev, pos, count, buf, check_size); /* NVMe: function 0을 통해 VPD 읽기. */
		pci_dev_put(dev); /* NVMe: function 0 참조 해제. */
		return ret; /* NVMe: 읽은 바이트 수 또는 에러 반환. */
	}

	return pci_vpd_read(dev, pos, count, buf, check_size); /* NVMe: 현재 function에서 VPD 직접 읽기. */
}

/**
 * pci_read_vpd - Read one entry from Vital Product Data
 * @dev:	PCI device struct
 * @pos:	offset in VPD space
 * @count:	number of bytes to read
 * @buf:	pointer to where to store result
 */
/*
 * pci_read_vpd:
 *   NVMe 장치의 VPD에서 지정한 오프셋부터 count 바이트를 읽는다.
 *   VPD 크기를 초과하지 않도록 검사(check_size=true)하며, sysfs나
 *   pci_vpd_alloc() 등에서 호출된다.
 */
ssize_t pci_read_vpd(struct pci_dev *dev, loff_t pos, size_t count, void *buf)
{
	return __pci_read_vpd(dev, pos, count, buf, true); /* NVMe: 크기 검사를 활성화하여 VPD 읽기. */
}
EXPORT_SYMBOL(pci_read_vpd); /* NVMe: NVMe 등 다른 코드에서 VPD 읽기 함수 사용 가능. */

/* Same, but allow to access any address */
/*
 * pci_read_vpd_any:
 *   pci_read_vpd와 동일하지만 VPD 크기 제한 없이 임의 주소에 접근한다.
 *   NVMe VPD 파싱/크기 측정 시 End tag 이전이나 비표준 영역에 접근할 때 사용.
 */
ssize_t pci_read_vpd_any(struct pci_dev *dev, loff_t pos, size_t count, void *buf)
{
	return __pci_read_vpd(dev, pos, count, buf, false); /* NVMe: 크기 검사 없이 VPD 읽기. */
}
EXPORT_SYMBOL(pci_read_vpd_any); /* NVMe: 외부 모듈에서 제한 없는 VPD 읽기 가능. */

/*
 * __pci_write_vpd:
 *   NVMe 장치의 VPD를 쓰는 낮은 수준 함수로, Multi-Function quirk에
 *   따라 function 0으로 라우팅하거나 현재 function에서 직접 쓴다.
 */
static ssize_t __pci_write_vpd(struct pci_dev *dev, loff_t pos, size_t count,
			       const void *buf, bool check_size)
{
	ssize_t ret; /* NVMe: write 반환값. */

	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0) { /* NVMe: function 0 VPD 라우팅 quirk 확인. */
		dev = pci_get_func0_dev(dev); /* NVMe: Multi-Function NVMe의 function 0 획득. */
		if (!dev) /* NVMe: function 0 획득 실패. */
			return -ENODEV; /* NVMe: VPD 쓰기 불가. */

		ret = pci_vpd_write(dev, pos, count, buf, check_size); /* NVMe: function 0을 통해 VPD 쓰기. */
		pci_dev_put(dev); /* NVMe: function 0 참조 해제. */
		return ret; /* NVMe: 쓴 바이트 수 또는 에러 반환. */
	}

	return pci_vpd_write(dev, pos, count, buf, check_size); /* NVMe: 현재 function에서 VPD 직접 쓰기. */
}

/**
 * pci_write_vpd - Write entry to Vital Product Data
 * @dev:	PCI device struct
 * @pos:	offset in VPD space
 * @count:	number of bytes to write
 * @buf:	buffer containing write data
 */
/*
 * pci_write_vpd:
 *   NVMe 장치의 VPD에 지정한 오프셋부터 count 바이트를 쓴다.
 *   VPD 크기를 초과하지 않도록 검사(check_size=true)한다.
 */
ssize_t pci_write_vpd(struct pci_dev *dev, loff_t pos, size_t count, const void *buf)
{
	return __pci_write_vpd(dev, pos, count, buf, true); /* NVMe: 크기 검사를 활성화하여 VPD 쓰기. */
}
EXPORT_SYMBOL(pci_write_vpd); /* NVMe: 외부 모듈에서 VPD 쓰기 가능. */

/* Same, but allow to access any address */
/*
 * pci_write_vpd_any:
 *   pci_write_vpd와 동일하지만 VPD 크기 제한 없이 임의 주소에 쓴다.
 */
ssize_t pci_write_vpd_any(struct pci_dev *dev, loff_t pos, size_t count, const void *buf)
{
	return __pci_write_vpd(dev, pos, count, buf, false); /* NVMe: 크기 검사 없이 VPD 쓰기. */
}
EXPORT_SYMBOL(pci_write_vpd_any); /* NVMe: 외부 모듈에서 제한 없는 VPD 쓰기 가능. */

/*
 * pci_vpd_find_ro_info_keyword:
 *   NVMe 장치 VPD의 Read-Only 데이터 영역(RO Data)에서 지정한 2글자
 *   키워드의 시작 오프셋과 크기를 반환한다. PN(Product Name), SN(Serial
 *   Number), EC(Engineering Changes) 등 NVMe 장치 식별 정보 추출에 사용.
 */
int pci_vpd_find_ro_info_keyword(const void *buf, unsigned int len,
				 const char *kw, unsigned int *size)
{
	int ro_start, infokw_start; /* NVMe: RO Data 시작 오프셋과 키워드 오프셋. */
	unsigned int ro_len, infokw_size; /* NVMe: RO Data 길이와 키워드 데이터 길이. */

	ro_start = pci_vpd_find_tag(buf, len, PCI_VPD_LRDT_RO_DATA, &ro_len); /* NVMe: VPD에서 Read-Only Data 태그 검색. */
	if (ro_start < 0) /* NVMe: RO Data 태그 없음. */
		return ro_start; /* NVMe: 에러 코드 그대로 반환. */

	infokw_start = pci_vpd_find_info_keyword(buf, ro_start, ro_len, kw); /* NVMe: RO Data 영역 내 키워드 검색. */
	if (infokw_start < 0) /* NVMe: 키워드 없음. */
		return infokw_start; /* NVMe: 에러 코드 반환. */

	infokw_size = pci_vpd_info_field_size(buf + infokw_start); /* NVMe: 키워드 필드의 데이터 길이 추출. */
	infokw_start += PCI_VPD_INFO_FLD_HDR_SIZE; /* NVMe: 헤더(3바이트)를 건 데이터 시작 위치. */

	if (infokw_start + infokw_size > len) /* NVMe: 키워드 데이터가 VPD 버퍼를 초과하면. */
		return -EINVAL; /* NVMe: 잘못된 VPD 형식. */

	if (size) /* NVMe: 호출자가 데이터 크기를 원하면. */
		*size = infokw_size; /* NVMe: 키워드 데이터 길이 저장. */

	return infokw_start; /* NVMe: 키워드 데이터 시작 오프셋 반환. */
}
EXPORT_SYMBOL_GPL(pci_vpd_find_ro_info_keyword); /* NVMe: NVMe 등에서 RO 키워드 검색 가능. */

/*
 * pci_vpd_check_csum:
 *   NVMe 장치 VPD의 Read-Only 영역에 있는 CHKSUM 키워드 값을 이용해
 *   VPD 데이터 무결성을 검증한다. 체크섬은 VPD 시작부터 CHKSUM 바이트
 *   직전까지의 모든 바이트 합에 CHKSUM 값을 더해 0이 되어야 한다.
 */
int pci_vpd_check_csum(const void *buf, unsigned int len)
{
	const u8 *vpd = buf; /* NVMe: VPD 버퍼를 u8 배열로 접근. */
	unsigned int size; /* NVMe: CHKSUM 필드의 길이. */
	u8 csum = 0; /* NVMe: 누적 체크섬 값. */
	int rv_start; /* NVMe: CHKSUM 필드 시작 오프셋. */

	rv_start = pci_vpd_find_ro_info_keyword(buf, len, PCI_VPD_RO_KEYWORD_CHKSUM, &size); /* NVMe: RO 영역에서 CHKSUM 키워드 검색. */
	if (rv_start == -ENOENT) /* no checksum in VPD */ /* NVMe: 체크섬 키워드가 없으면. */
		return 1; /* NVMe: 체크섬이 없는 것을 유효로 간주(1 반환). */
	else if (rv_start < 0) /* NVMe: CHKSUM 검색 중 다른 에러. */
		return rv_start; /* NVMe: 에러 코드 반환. */

	if (!size) /* NVMe: CHKSUM 필드 길이가 0이면. */
		return -EINVAL; /* NVMe: 잘못된 VPD 형식. */

	while (rv_start >= 0) /* NVMe: CHKSUM 필드 바이트를 포함한 VPD 시작부터의 모든 바이트 합산. */
		csum += vpd[rv_start--]; /* NVMe: CHKSUM 위치에서 역방향으로 모든 바이트 누적. */

	return csum ? -EILSEQ : 0; /* NVMe: 합이 0이면 유효(0), 아니면 체크섬 불일치(-EILSEQ). */
}
EXPORT_SYMBOL_GPL(pci_vpd_check_csum); /* NVMe: NVMe 장치 VPD 무결성 검증 함수 외부 공개. */

#ifdef CONFIG_PCI_QUIRKS
/*
 * Quirk non-zero PCI functions to route VPD access through function 0 for
 * devices that share VPD resources between functions.  The functions are
 * expected to be identical devices.
 */
/*
 * quirk_f0_vpd_link:
 *   Multi-Function PCIe 장치(일부 NVMe 컨트롤러 포함)에서 VPD 리소스가
 *   function들 간에 공유되는 경우, function 0이 아닌 function의 VPD
 *   접근을 function 0으로 라우팅하도록 설정한다. 이로 인해 NVMe
 *   function에서도 /sys/.../vpd 접근 시 정상적인 데이터를 얻을 수 있다.
 */
static void quirk_f0_vpd_link(struct pci_dev *dev)
{
	struct pci_dev *f0; /* NVMe: 동일 슬롯의 function 0 pci_dev. */

	if (!PCI_FUNC(dev->devfn)) /* NVMe: 이미 function 0이면 라우팅 필요 없음. */
		return; /* NVMe: function 0은 early return. */

	f0 = pci_get_func0_dev(dev); /* NVMe: 현재 NVMe function의 function 0 획득. */
	if (!f0) /* NVMe: function 0을 찾을 수 없으면. */
		return; /* NVMe: quirk 적용 불가. */

	if (f0->vpd.cap && dev->class == f0->class && /* NVMe: function 0이 VPD를 가지고 있고. */
	    dev->vendor == f0->vendor && dev->device == f0->device) /* NVMe: 동일 vendor/device인 경우에만 적용. */
		dev->dev_flags |= PCI_DEV_FLAGS_VPD_REF_F0; /* NVMe: VPD 접근 시 function 0을 참조하도록 플래그 설정. */

	pci_dev_put(f0); /* NVMe: function 0 pci_dev 참조 해제. */
}
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, PCI_ANY_ID,
			      PCI_CLASS_NETWORK_ETHERNET, 8, quirk_f0_vpd_link); /* NVMe: Intel Ethernet 클래스의 Multi-Function 장치에 quirk 등록. */

/*
 * If a device follows the VPD format spec, the PCI core will not read or
 * write past the VPD End Tag.  But some vendors do not follow the VPD
 * format spec, so we can't tell how much data is safe to access.  Devices
 * may behave unpredictably if we access too much.  Blacklist these devices
 * so we don't touch VPD at all.
 */
/*
 * quirk_blacklist_vpd:
 *   VPD 형식을 따르지 않는 PCIe 장치(비표준 VPD EEPROM)에서 VPD 접근을
 *   완전히 차단한다. NVMe 장치 중에서도 비표준 VPD로 인해 config space
 *   접근 시 비정상 동작이나 PCIe 에러가 발생할 수 있는 경우를 보호하기
 *   위해 vpd.len을 PCI_VPD_SZ_INVALID로 설정한다.
 */
static void quirk_blacklist_vpd(struct pci_dev *dev)
{
	dev->vpd.len = PCI_VPD_SZ_INVALID; /* NVMe: VPD 접근 금지 표시. */
	pci_warn(dev, FW_BUG "disabling VPD access (can't determine size of non-standard VPD format)\n"); /* NVMe: 펌웨어 버그로 VPD 비활성화 경고. */
}
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0060, quirk_blacklist_vpd); /* NVMe: LSI 특정 device에 VPD 블랙리스트 적용. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x007c, quirk_blacklist_vpd); /* NVMe: LSI 특정 device에 VPD 블랙리스트 적용. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0413, quirk_blacklist_vpd); /* NVMe: LSI 특정 device에 VPD 블랙리스트 적용. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0078, quirk_blacklist_vpd); /* NVMe: LSI 특정 device에 VPD 블랙리스트 적용. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0079, quirk_blacklist_vpd); /* NVMe: LSI 특정 device에 VPD 블랙리스트 적용. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0073, quirk_blacklist_vpd); /* NVMe: LSI 특정 device에 VPD 블랙리스트 적용. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0071, quirk_blacklist_vpd); /* NVMe: LSI 특정 device에 VPD 블랙리스트 적용. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x005b, quirk_blacklist_vpd); /* NVMe: LSI 특정 device에 VPD 블랙리스트 적용. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x002f, quirk_blacklist_vpd); /* NVMe: LSI 특정 device에 VPD 블랙리스트 적용. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x005d, quirk_blacklist_vpd); /* NVMe: LSI 특정 device에 VPD 블랙리스트 적용. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x005f, quirk_blacklist_vpd); /* NVMe: LSI 특정 device에 VPD 블랙리스트 적용. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ATTANSIC, PCI_ANY_ID, quirk_blacklist_vpd); /* NVMe: Attansic 전체 device에 VPD 블랙리스트 적용. */
/*
 * The Amazon Annapurna Labs 0x0031 device id is reused for other non Root Port
 * device types, so the quirk is registered for the PCI_CLASS_BRIDGE_PCI class.
 */
DECLARE_PCI_FIXUP_CLASS_HEADER(PCI_VENDOR_ID_AMAZON_ANNAPURNA_LABS, 0x0031,
			       PCI_CLASS_BRIDGE_PCI, 8, quirk_blacklist_vpd); /* NVMe: Amazon Annapurna Labs bridge 클래스에 VPD 블랙리스트 적용. */

/*
 * quirk_chelsio_extend_vpd:
 *   Chelsio 어댑터의 VPD 영역이 표준 크기보다 큰 경우, 실제 접근 가능한
 *   VPD 크기를 수동으로 확장한다. NVMe 장치와 직접 관련은 없으나, VPD
 *   크기 측정 메커니즘의 한계를 보여주는 quirk 사례이다.
 */
static void quirk_chelsio_extend_vpd(struct pci_dev *dev)
{
	int chip = (dev->device & 0xf000) >> 12; /* NVMe: device ID에서 chip 종류 추출. */
	int func = (dev->device & 0x0f00) >>  8; /* NVMe: device ID에서 function 정보 추출. */
	int prod = (dev->device & 0x00ff) >>  0; /* NVMe: device ID에서 product 정보 추출. */

	/*
	 * If this is a T3-based adapter, there's a 1KB VPD area at offset
	 * 0xc00 which contains the preferred VPD values.  If this is a T4 or
	 * later based adapter, the special VPD is at offset 0x400 for the
	 * Physical Functions (the SR-IOV Virtual Functions have no VPD
	 * Capabilities).  The PCI VPD Access core routines will normally
	 * compute the size of the VPD by parsing the VPD Data Structure at
	 * offset 0x000.  This will result in silent failures when attempting
	 * to accesses these other VPD areas which are beyond those computed
	 * limits.
	 */
	if (chip == 0x0 && prod >= 0x20) /* NVMe: T3 기반 어댑터의 특정 product. */
		dev->vpd.len = 8192; /* NVMe: VPD 크기를 8KB로 확장. */
	else if (chip >= 0x4 && func < 0x8) /* NVMe: T4 이상 Physical Function. */
		dev->vpd.len = 2048; /* NVMe: VPD 크기를 2KB로 확장. */
}

DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_CHELSIO, PCI_ANY_ID,
				 quirk_chelsio_extend_vpd); /* NVMe: Chelsio 전체 device에 VPD 확장 quirk 등록. */

#endif
