// SPDX-License-Identifier: GPL-2.0
/*
 * PCI VPD support
 *
 * Copyright (C) 2010 Broadcom Corporation.
 */

/*
 * [한국어 설명] 장치의 EEPROM 에 든 제품 정보를 읽고 쓰는 계층 (vpd.c)
 *
 * === 파일의 역할 ===
 * VPD(Vital Product Data)는 PCI 장치가 자기 자신에 대해 기록해 둔 정보다.
 * 제조사명, 부품 번호, 일련번호, 펌웨어 버전 같은 것이 들어 있다.
 * config space 의 몇 바이트가 아니라 별도의 직렬 EEPROM 에 저장되며,
 * config space 의 VPD capability 가 그것을 읽고 쓰는 창구 역할을 한다.
 *
 * 접근 방식이 독특하고, 그것이 이 파일의 복잡성 대부분을 만든다.
 *   1) VPD Address 레지스터에 읽고 싶은 오프셋을 쓴다.
 *   2) 장치가 EEPROM 에서 그 4바이트를 가져올 때까지 기다린다.
 *      완료되면 Address 레지스터의 F 비트가 뒤집힌다.
 *   3) VPD Data 레지스터에서 값을 읽는다.
 * EEPROM 은 느려서 한 번에 수 밀리초가 걸릴 수 있다. 그래서
 * pci_vpd_wait() 이 폴링하며 기다리고, 점점 간격을 늘려 가며 잔다.
 *
 * 데이터 형식도 따로 있다. VPD 는 태그로 구분된 자원들의 나열이고
 * (문자열 태그, 읽기 전용 태그, 읽기/쓰기 태그, 끝 태그), 각 태그 안에
 * 다시 "PN"(part number), "SN"(serial number) 같은 두 글자 키워드로
 * 항목이 나뉜다. 이 파일은 그 구조를 훑어 전체 크기를 알아낸다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 열거:  probe.c 의 pci_init_capabilities()
 *          -> [이 파일] pci_vpd_init() — capability 오프셋을 찾고 뮤텍스를 초기화
 *
 * 읽기:  cat /sys/bus/pci/devices/.../vpd
 *          -> pci-sysfs.c 의 vpd_read()
 *             -> [이 파일] pci_read_vpd() -> pci_vpd_read() -> 폴링 루프
 *
 * quirk: quirks.c 가 DECLARE_PCI_FIXUP_* 로 등록한 것들이 이 파일에 있다
 *          (quirk_f0_vpd_link, quirk_blacklist_vpd 등).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용. 폴링 중에 잠들고, 뮤텍스를 잡는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci-sysfs.c(vpd 속성), 일부 네트워크 드라이버(자기 장치의 일련번호를
 *   읽어 MAC 주소를 만들거나 펌웨어 이미지를 고르는 데 쓴다).
 * 아래쪽: access.c 의 config 접근, pci.c 의 런타임 PM 참조 관리.
 * 공유 상태: struct pci_dev 의 vpd 하위 구조 — cap(capability 오프셋),
 *   len(전체 크기, 처음 읽을 때 계산해 캐시), valid(형식이 올바른가),
 *   lock(동시 접근 직렬화용 뮤텍스).
 *
 * 뮤텍스가 필요한 이유가 위 3단계 절차에 있다. Address 를 쓰고 Data 를
 * 읽는 사이에 다른 태스크가 Address 를 덮어쓰면 엉뚱한 오프셋의 값을
 * 읽는다. config 접근 자체를 보호하는 pci_lock 은 접근 한 번만 감싸므로
 * 이 절차 전체를 덮지 못한다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 하나도 직접 부르지 않는다(전수 확인).
 * NVMe SSD 도 VPD 를 노출할 수는 있고, 그 경우 lspci -vv 나 sysfs 로
 * 일련번호를 읽을 수 있다. 다만 NVMe 는 자기 스펙의 Identify Controller
 * 명령으로 훨씬 풍부한 정보(모델명, 일련번호, 펌웨어 리비전)를 제공하므로,
 * NVMe 관리 도구들은 VPD 대신 그쪽을 쓴다.
 *
 * (기존 주석은 "NVMe 장치 probe 시 VPD capability 를 찾아" 라고 적었는데,
 *  probe 는 드라이버 바인딩 시점이고 pci_vpd_init() 은 그보다 앞선 열거
 *  단계에서 불린다. 또 "다기능 NVMe 컨트롤러의 VPD 라우팅 quirk" 라고
 *  했으나 quirk_f0_vpd_link 의 대상 목록에 NVMe 컨트롤러는 없다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_vpd_init()          : capability 를 찾고 뮤텍스를 초기화한다. 열거 시 1회.
 * pci_vpd_size()          : VPD 태그들을 훑어 전체 크기를 알아낸다. 형식이
 *                           깨져 있으면 거기까지만 유효한 것으로 자른다.
 * pci_vpd_wait()          : F 비트가 뒤집히기를 기다린다. 간격을 늘려 가며
 *                           폴링하고, 상한을 넘으면 -ETIMEDOUT.
 * pci_vpd_read()          : 3단계 절차로 실제 읽기. 4바이트 단위이므로
 *                           앞뒤가 정렬되지 않은 요청은 잘라 붙인다.
 * pci_vpd_write()         : 쓰기. 읽기와 F 비트의 의미가 반대다.
 * pci_read_vpd() / pci_write_vpd() : 외부에 노출되는 진입점. 뮤텍스와
 *                           런타임 PM 참조를 여기서 관리한다.
 * pci_vpd_alloc()         : 전체 VPD 를 읽어 힙 버퍼로 돌려준다.
 * pci_vpd_find_id_string() / pci_vpd_find_ro_info_keyword() : 태그와
 *                           키워드를 찾아 그 안의 값 위치를 알려 준다.
 * pci_vpd_check_csum()    : 읽기 전용 영역의 체크섬을 검증한다.
 * quirk_f0_vpd_link()     : 다기능 장치에서 function 0 의 VPD 를 공유하게 한다.
 * quirk_blacklist_vpd()   : VPD 접근이 장치를 망가뜨리는 것으로 알려진
 *                           모델에서 아예 접근을 막는다.
 */

#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/sched/signal.h>
#include <linux/unaligned.h>
#include "pci.h"

#define PCI_VPD_LRDT_TAG_SIZE		3
#define PCI_VPD_SRDT_LEN_MASK		0x07
#define PCI_VPD_SRDT_TAG_SIZE		1
#define PCI_VPD_STIN_END		0x0f
#define PCI_VPD_INFO_FLD_HDR_SIZE	3

/*
 * pci_vpd_lrdt_size:
 *   VPD Large Resource Data Type 항목의 데이터 길이를 추출한다.
 *   NVMe 장치의 VPD EEPROM에서 모델명/제조사 등 큰 데이터 블록 크기 파악에 사용.
 */
static u16 pci_vpd_lrdt_size(const u8 *lrdt)
{
	return get_unaligned_le16(lrdt + 1);
}

/*
 * pci_vpd_srdt_tag:
 *   VPD Short Resource Data Type 태그 번호를 추출한다.
 *   NVMe VPD의 End tag 같은 짧은 항목 식별에 사용.
 */
static u8 pci_vpd_srdt_tag(const u8 *srdt)
{
	return *srdt >> 3;
}

/*
 * pci_vpd_srdt_size:
 *   VPD Short Resource Data Type 항목의 데이터 길이를 추출한다.
 */
static u8 pci_vpd_srdt_size(const u8 *srdt)
{
	return *srdt & PCI_VPD_SRDT_LEN_MASK;
}

/*
 * pci_vpd_info_field_size:
 *   VPD 정보 필드의 실제 데이터 길이를 추출한다.
 *   NVMe 장치 VPD 내 제품 일련번호(PN), 제조사(VN) 등 키워드 항목 길이 파악에 사용.
 */
static u8 pci_vpd_info_field_size(const u8 *info_field)
{
	return info_field[2];
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
	return pci_get_slot(dev->bus, PCI_DEVFN(PCI_SLOT(dev->devfn), 0));
}

#define PCI_VPD_MAX_SIZE	(PCI_VPD_ADDR_MASK + 1)
#define PCI_VPD_SZ_INVALID	UINT_MAX

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
	size_t off = 0, size;
	unsigned char tag, header[1+2];	/* 1 byte tag, 2 bytes length */

	while (pci_read_vpd_any(dev, off, 1, header) == 1) {
		size = 0;

		if (off == 0 && (header[0] == 0x00 || header[0] == 0xff))
			goto error;

		if (header[0] & PCI_VPD_LRDT) {
			/* Large Resource Data Type Tag */
			if (pci_read_vpd_any(dev, off + 1, 2, &header[1]) != 2) {
				pci_warn(dev, "failed VPD read at offset %zu\n",
					 off + 1);
				return off ?: PCI_VPD_SZ_INVALID;
			}
			size = pci_vpd_lrdt_size(header);
			if (off + size > PCI_VPD_MAX_SIZE)
				goto error;

			off += PCI_VPD_LRDT_TAG_SIZE + size;
		} else {
			/* Short Resource Data Type Tag */
			tag = pci_vpd_srdt_tag(header);
			size = pci_vpd_srdt_size(header);
			if (off + size > PCI_VPD_MAX_SIZE)
				goto error;

			off += PCI_VPD_SRDT_TAG_SIZE + size;
			if (tag == PCI_VPD_STIN_END)	/* End tag descriptor */
				return off;
		}
	}
	return off;

error:
	pci_info(dev, "invalid VPD tag %#04x (size %zu) at offset %zu%s\n",
		 header[0], size, off, off == 0 ?
		 "; assume missing optional EEPROM" : "");
	return off ?: PCI_VPD_SZ_INVALID;
}

/*
 * pci_vpd_available:
 *   NVMe 장치에 VPD capability가 있고, 필요한 경우 유효한 VPD 크기가
 *   결정되어 있는지 확인한다. 크기가 결정되지 않았으면 pci_vpd_size()를
 *   호출하여 측정한다.
 */
static bool pci_vpd_available(struct pci_dev *dev, bool check_size)
{
	struct pci_vpd *vpd = &dev->vpd;

	if (!vpd->cap)
		return false;

	if (vpd->len == 0 && check_size) {
		vpd->len = pci_vpd_size(dev);
		if (vpd->len == PCI_VPD_SZ_INVALID) {
			vpd->cap = 0;
			return false;
		}
	}

	return true;
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
	struct pci_vpd *vpd = &dev->vpd;
	unsigned long timeout = jiffies + msecs_to_jiffies(125);
	unsigned long max_sleep = 16;
	u16 status;
	int ret;

	do {
		ret = pci_user_read_config_word(dev, vpd->cap + PCI_VPD_ADDR,
						&status);
		if (ret < 0)
			return ret;

		if (!!(status & PCI_VPD_ADDR_F) == set)
			return 0;

		if (time_after(jiffies, timeout))
			break;

		usleep_range(10, max_sleep);
		if (max_sleep < 1024)
			max_sleep *= 2;
	} while (true);

	pci_warn(dev, "VPD access failed.  This is likely a firmware bug on this device.  Contact the card vendor for a firmware update\n");
	return -ETIMEDOUT;
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
	struct pci_vpd *vpd = &dev->vpd;
	unsigned int max_len;
	int ret = 0;
	loff_t end = pos + count;
	u8 *buf = arg;

	if (!pci_vpd_available(dev, check_size))
		return -ENODEV;

	if (pos < 0)
		return -EINVAL;

	max_len = check_size ? vpd->len : PCI_VPD_MAX_SIZE;

	if (pos >= max_len)
		return 0;

	if (end > max_len) {
		end = max_len;
		count = end - pos;
	}

	if (mutex_lock_killable(&vpd->lock))
		return -EINTR;

	while (pos < end) {
		u32 val;
		unsigned int i, skip;

		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}

		ret = pci_user_write_config_word(dev, vpd->cap + PCI_VPD_ADDR,
						 pos & ~3);
		if (ret < 0)
			break;
		ret = pci_vpd_wait(dev, true);
		if (ret < 0)
			break;

		ret = pci_user_read_config_dword(dev, vpd->cap + PCI_VPD_DATA, &val);
		if (ret < 0)
			break;

		skip = pos & 3;
		for (i = 0;  i < sizeof(u32); i++) {
			if (i >= skip) {
				*buf++ = val;
				if (++pos == end)
					break;
			}
			val >>= 8;
		}
	}

	mutex_unlock(&vpd->lock);
	return ret ? ret : count;
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
	struct pci_vpd *vpd = &dev->vpd;
	unsigned int max_len;
	const u8 *buf = arg;
	loff_t end = pos + count;
	int ret = 0;

	if (!pci_vpd_available(dev, check_size))
		return -ENODEV;

	if (pos < 0 || (pos & 3) || (count & 3))
		return -EINVAL;

	max_len = check_size ? vpd->len : PCI_VPD_MAX_SIZE;

	if (end > max_len)
		return -EINVAL;

	if (mutex_lock_killable(&vpd->lock))
		return -EINTR;

	while (pos < end) {
		ret = pci_user_write_config_dword(dev, vpd->cap + PCI_VPD_DATA,
						  get_unaligned_le32(buf));
		if (ret < 0)
			break;
		ret = pci_user_write_config_word(dev, vpd->cap + PCI_VPD_ADDR,
						 pos | PCI_VPD_ADDR_F);
		if (ret < 0)
			break;

		ret = pci_vpd_wait(dev, false);
		if (ret < 0)
			break;

		buf += sizeof(u32);
		pos += sizeof(u32);
	}

	mutex_unlock(&vpd->lock);
	return ret ? ret : count;
}

/*
 * pci_vpd_init:
 *   NVMe 장치 probe 단계에서 VPD capability를 검색하고, VPD 접근을 위한
 *   mutex를 초기화한다. NVMe pci_dev의 vpd.cap이 설정되어야 sysfs /vpd
 *   파일이 노출되고, pci_read_vpd/write_vpd가 동작한다.
 */
void pci_vpd_init(struct pci_dev *dev)
{
	if (dev->vpd.len == PCI_VPD_SZ_INVALID)
		return;

	dev->vpd.cap = pci_find_capability(dev, PCI_CAP_ID_VPD);
	mutex_init(&dev->vpd.lock);
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
	struct pci_dev *dev = to_pci_dev(kobj_to_dev(kobj));
	struct pci_dev *vpd_dev = dev;
	ssize_t ret;

	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0) {
		vpd_dev = pci_get_func0_dev(dev);
		if (!vpd_dev)
			return -ENODEV;
	}

	pci_config_pm_runtime_get(vpd_dev);
	ret = pci_read_vpd(vpd_dev, off, count, buf);
	pci_config_pm_runtime_put(vpd_dev);

	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0)
		pci_dev_put(vpd_dev);

	return ret;
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
	struct pci_dev *dev = to_pci_dev(kobj_to_dev(kobj));
	struct pci_dev *vpd_dev = dev;
	ssize_t ret;

	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0) {
		vpd_dev = pci_get_func0_dev(dev);
		if (!vpd_dev)
			return -ENODEV;
	}

	pci_config_pm_runtime_get(vpd_dev);
	ret = pci_write_vpd(vpd_dev, off, count, buf);
	pci_config_pm_runtime_put(vpd_dev);

	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0)
		pci_dev_put(vpd_dev);

	return ret;
}
/*
 * BIN_ATTR(vpd, 0600, ...):
 *   NVMe 장치의 /sys/bus/pci/devices/<BDF>/vpd 바이너리 sysfs 속성을
 *   정의한다. 소유주는 읽기/쓰기 모두 가능(0600), 다른 사용자는 접근 불가.
 */
static const BIN_ATTR(vpd, 0600, vpd_read, vpd_write, 0);

/*
 * vpd_attrs:
 *   NVMe 장치에 노출할 VPD 바이너리 속성 배열. pci_dev_vpd_attr_group에
 *   등록되어 sysfs tree에 /vpd 파일을 만든다.
 */
static const struct bin_attribute *const vpd_attrs[] = {
	&bin_attr_vpd,
	NULL,
};

/*
 * vpd_attr_is_visible:
 *   NVMe 장치의 sysfs 그룹 등록 시 VPD capability가 있는 경우에만
 *   /vpd 파일을 노출한다. vpd.cap이 0이면 사용자에게 보이지 않는다.
 */
static umode_t vpd_attr_is_visible(struct kobject *kobj,
				   const struct bin_attribute *a, int n)
{
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj));

	if (!pdev->vpd.cap)
		return 0;

	return a->attr.mode;
}

/*
 * pci_dev_vpd_attr_group:
 *   NVMe pci_dev가 sysfs에 등록할 때 VPD 속성 그룹. bin_attrs에 vpd를
 *   등록하고, is_bin_visible 콜백으로 capability 유무에 따라 노출 제어.
 */
const struct attribute_group pci_dev_vpd_attr_group = {
	.bin_attrs = vpd_attrs,
	.is_bin_visible = vpd_attr_is_visible,
};

/*
 * pci_vpd_alloc:
 *   NVMe 장치의 전체 VPD 데이터를 커널 메모리에 할당하여 반환한다.
 *   NVMe 드라이버나 기타 서브시스템이 VPD를 파싱하거나 checksum 검증
 *   시 사용한다. 반환된 버퍼는 호출자가 kfree()로 해제해야 한다.
 */
void *pci_vpd_alloc(struct pci_dev *dev, unsigned int *size)
{
	unsigned int len;
	void *buf;
	int cnt;

	if (!pci_vpd_available(dev, true))
		return ERR_PTR(-ENODEV);

	len = dev->vpd.len;
	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	cnt = pci_read_vpd(dev, 0, len, buf);
	if (cnt != len) {
		kfree(buf);
		return ERR_PTR(-EIO);
	}

	if (size)
		*size = len;

	return buf;
}
EXPORT_SYMBOL_GPL(pci_vpd_alloc);

/*
 * pci_vpd_find_tag:
 *   NVMe 장치의 VPD 버퍼에서 지정한 Large Resource Data Type 태그를
 *   검색한다. ID String, Read-Only Data, Read-Write Data 등의 시작
 *   오프셋을 찾는 데 사용된다.
 */
static int pci_vpd_find_tag(const u8 *buf, unsigned int len, u8 rdt, unsigned int *size)
{
	int i = 0;

	/* look for LRDT tags only, end tag is the only SRDT tag */
	while (i + PCI_VPD_LRDT_TAG_SIZE <= len && buf[i] & PCI_VPD_LRDT) {
		unsigned int lrdt_len = pci_vpd_lrdt_size(buf + i);
		u8 tag = buf[i];

		i += PCI_VPD_LRDT_TAG_SIZE;
		if (tag == rdt) {
			if (i + lrdt_len > len)
				lrdt_len = len - i;
			if (size)
				*size = lrdt_len;
			return i;
		}

		i += lrdt_len;
	}

	return -ENOENT;
}

/*
 * pci_vpd_find_id_string:
 *   NVMe 장치 VPD에서 PCI_VPD_LRDT_ID_STRING 태그를 찾아 제품 식별
 *   문자열의 시작 오프셋을 반환한다. NVMe SSD 모델명/제조사 식별에 사용.
 */
int pci_vpd_find_id_string(const u8 *buf, unsigned int len, unsigned int *size)
{
	return pci_vpd_find_tag(buf, len, PCI_VPD_LRDT_ID_STRING, size);
}
EXPORT_SYMBOL_GPL(pci_vpd_find_id_string);

/*
 * pci_vpd_find_info_keyword:
 *   NVMe 장치 VPD의 Read-Only/Read-Write 데이터 영역 내에서 2글자
 *   키워드(PN, VN, EC 등)를 검색한다. 호출자가 영역 시작 off와 길이 len을
 *   지정하면 해당 범위 내에서 키워드를 찾는다.
 */
static int pci_vpd_find_info_keyword(const u8 *buf, unsigned int off,
			      unsigned int len, const char *kw)
{
	int i;

	for (i = off; i + PCI_VPD_INFO_FLD_HDR_SIZE <= off + len;) {
		if (buf[i + 0] == kw[0] &&
		    buf[i + 1] == kw[1])
			return i;

		i += PCI_VPD_INFO_FLD_HDR_SIZE +
		     pci_vpd_info_field_size(&buf[i]);
	}

	return -ENOENT;
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
	ssize_t ret;

	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0) {
		dev = pci_get_func0_dev(dev);
		if (!dev)
			return -ENODEV;

		ret = pci_vpd_read(dev, pos, count, buf, check_size);
		pci_dev_put(dev);
		return ret;
	}

	return pci_vpd_read(dev, pos, count, buf, check_size);
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
	return __pci_read_vpd(dev, pos, count, buf, true);
}
EXPORT_SYMBOL(pci_read_vpd);

/* Same, but allow to access any address */
/*
 * pci_read_vpd_any:
 *   pci_read_vpd와 동일하지만 VPD 크기 제한 없이 임의 주소에 접근한다.
 *   NVMe VPD 파싱/크기 측정 시 End tag 이전이나 비표준 영역에 접근할 때 사용.
 */
ssize_t pci_read_vpd_any(struct pci_dev *dev, loff_t pos, size_t count, void *buf)
{
	return __pci_read_vpd(dev, pos, count, buf, false);
}
EXPORT_SYMBOL(pci_read_vpd_any);

/*
 * __pci_write_vpd:
 *   NVMe 장치의 VPD를 쓰는 낮은 수준 함수로, Multi-Function quirk에
 *   따라 function 0으로 라우팅하거나 현재 function에서 직접 쓴다.
 */
static ssize_t __pci_write_vpd(struct pci_dev *dev, loff_t pos, size_t count,
			       const void *buf, bool check_size)
{
	ssize_t ret;

	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0) {
		dev = pci_get_func0_dev(dev);
		if (!dev)
			return -ENODEV;

		ret = pci_vpd_write(dev, pos, count, buf, check_size);
		pci_dev_put(dev);
		return ret;
	}

	return pci_vpd_write(dev, pos, count, buf, check_size);
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
	return __pci_write_vpd(dev, pos, count, buf, true);
}
EXPORT_SYMBOL(pci_write_vpd);

/* Same, but allow to access any address */
/*
 * pci_write_vpd_any:
 *   pci_write_vpd와 동일하지만 VPD 크기 제한 없이 임의 주소에 쓴다.
 */
ssize_t pci_write_vpd_any(struct pci_dev *dev, loff_t pos, size_t count, const void *buf)
{
	return __pci_write_vpd(dev, pos, count, buf, false);
}
EXPORT_SYMBOL(pci_write_vpd_any);

/*
 * pci_vpd_find_ro_info_keyword:
 *   NVMe 장치 VPD의 Read-Only 데이터 영역(RO Data)에서 지정한 2글자
 *   키워드의 시작 오프셋과 크기를 반환한다. PN(Product Name), SN(Serial
 *   Number), EC(Engineering Changes) 등 NVMe 장치 식별 정보 추출에 사용.
 */
int pci_vpd_find_ro_info_keyword(const void *buf, unsigned int len,
				 const char *kw, unsigned int *size)
{
	int ro_start, infokw_start;
	unsigned int ro_len, infokw_size;

	ro_start = pci_vpd_find_tag(buf, len, PCI_VPD_LRDT_RO_DATA, &ro_len);
	if (ro_start < 0)
		return ro_start;

	infokw_start = pci_vpd_find_info_keyword(buf, ro_start, ro_len, kw);
	if (infokw_start < 0)
		return infokw_start;

	infokw_size = pci_vpd_info_field_size(buf + infokw_start);
	infokw_start += PCI_VPD_INFO_FLD_HDR_SIZE;

	if (infokw_start + infokw_size > len)
		return -EINVAL;

	if (size)
		*size = infokw_size;

	return infokw_start;
}
EXPORT_SYMBOL_GPL(pci_vpd_find_ro_info_keyword);

/*
 * pci_vpd_check_csum:
 *   NVMe 장치 VPD의 Read-Only 영역에 있는 CHKSUM 키워드 값을 이용해
 *   VPD 데이터 무결성을 검증한다. 체크섬은 VPD 시작부터 CHKSUM 바이트
 *   직전까지의 모든 바이트 합에 CHKSUM 값을 더해 0이 되어야 한다.
 */
int pci_vpd_check_csum(const void *buf, unsigned int len)
{
	const u8 *vpd = buf;
	unsigned int size;
	u8 csum = 0;
	int rv_start;

	rv_start = pci_vpd_find_ro_info_keyword(buf, len, PCI_VPD_RO_KEYWORD_CHKSUM, &size);
	if (rv_start == -ENOENT) /* no checksum in VPD */
		return 1;
	else if (rv_start < 0)
		return rv_start;

	if (!size)
		return -EINVAL;

	while (rv_start >= 0)
		csum += vpd[rv_start--];

	return csum ? -EILSEQ : 0;
}
EXPORT_SYMBOL_GPL(pci_vpd_check_csum);

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
	struct pci_dev *f0;

	if (!PCI_FUNC(dev->devfn))
		return;

	f0 = pci_get_func0_dev(dev);
	if (!f0)
		return;

	if (f0->vpd.cap && dev->class == f0->class &&
	    dev->vendor == f0->vendor && dev->device == f0->device)
		dev->dev_flags |= PCI_DEV_FLAGS_VPD_REF_F0;

	pci_dev_put(f0);
}
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, PCI_ANY_ID,
			      PCI_CLASS_NETWORK_ETHERNET, 8, quirk_f0_vpd_link);

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
	dev->vpd.len = PCI_VPD_SZ_INVALID;
	pci_warn(dev, FW_BUG "disabling VPD access (can't determine size of non-standard VPD format)\n");
}
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0060, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x007c, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0413, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0078, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0079, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0073, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0071, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x005b, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x002f, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x005d, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x005f, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ATTANSIC, PCI_ANY_ID, quirk_blacklist_vpd);
/*
 * The Amazon Annapurna Labs 0x0031 device id is reused for other non Root Port
 * device types, so the quirk is registered for the PCI_CLASS_BRIDGE_PCI class.
 */
DECLARE_PCI_FIXUP_CLASS_HEADER(PCI_VENDOR_ID_AMAZON_ANNAPURNA_LABS, 0x0031,
			       PCI_CLASS_BRIDGE_PCI, 8, quirk_blacklist_vpd);

/*
 * quirk_chelsio_extend_vpd:
 *   Chelsio 어댑터의 VPD 영역이 표준 크기보다 큰 경우, 실제 접근 가능한
 *   VPD 크기를 수동으로 확장한다. NVMe 장치와 직접 관련은 없으나, VPD
 *   크기 측정 메커니즘의 한계를 보여주는 quirk 사례이다.
 */
static void quirk_chelsio_extend_vpd(struct pci_dev *dev)
{
	int chip = (dev->device & 0xf000) >> 12;
	int func = (dev->device & 0x0f00) >>  8;
	int prod = (dev->device & 0x00ff) >>  0;

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
	if (chip == 0x0 && prod >= 0x20)
		dev->vpd.len = 8192;
	else if (chip >= 0x4 && func < 0x8)
		dev->vpd.len = 2048;
}

DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_CHELSIO, PCI_ANY_ID,
				 quirk_chelsio_extend_vpd);

#endif
