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
 *          -> [이 파일] vpd_read()  — sysfs 바이너리 속성 콜백. pci-sysfs.c 가
 *             아니라 이 파일 안에 있다(BIN_ATTR(vpd, ...) 로 등록).
 *             pci-sysfs.c 는 속성 "그룹" 을 목록에 올릴 뿐이다.
 *             -> pci_read_vpd() -> __pci_read_vpd() -> pci_vpd_read()
 *                -> Address 쓰기 / pci_vpd_wait() 폴링 / Data 읽기
 *
 * quirk: VPD 관련 DECLARE_PCI_FIXUP_* 등록은 quirks.c 가 아니라 이 파일
 *          맨 아래의 CONFIG_PCI_QUIRKS 블록에 있다(quirks.c 에는 VPD quirk 가
 *          하나도 없다 — 전수 확인). quirk_f0_vpd_link, quirk_blacklist_vpd,
 *          quirk_chelsio_extend_vpd 셋이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용. 폴링 중에 잠들고, 뮤텍스를 잡는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci-sysfs.c — 이 파일이 정의한 pci_dev_vpd_attr_group 을 sysfs 속성
 *   그룹 목록에 넣는다(pci-sysfs.c:4844). 선언은 drivers/pci/pci.h:828.
 *   probe.c 의 pci_init_capabilities() 가 pci_vpd_init() 을 부른다(probe.c:6413).
 *   커널 안의 다른 이용자로 이 트리에서 확인한 것은
 *   drivers/vfio/pci/vfio_pci_config.c:828 하나다 — VFIO 가 게스트에게
 *   VPD 를 중계할 때 pci_read_vpd() 를 쓴다.
 * 아래쪽: access.c 의 사용자 접근용 config 함수(pci_user_read_config_word 등,
 *   access.c:797 부근에서 매크로로 생성된다), pci.c:5565 의
 *   pci_config_pm_runtime_get / _put.
 * 공유 상태: struct pci_dev 의 vpd 하위 구조. 이 파일이 실제로 만지는 필드는
 *   셋뿐이다 — cap(capability 오프셋. 0 이면 VPD 없음),
 *   len(전체 크기. 처음 읽을 때 계산해 캐시하고, PCI_VPD_SZ_INVALID 면
 *   "쓰지 말 것" 표시), lock(절차 전체를 직렬화하는 뮤텍스).
 *   (기존 주석이 valid 필드를 들었으나 drivers/ 어디에도 vpd.valid 사용이
 *    없다. struct 정의가 include/linux/pci.h 에 있고 이 스파스 체크아웃에는
 *    그 헤더가 없어 실재 여부는 확인하지 못했다.)
 *
 * 뮤텍스가 필요한 이유가 위 3단계 절차에 있다. Address 를 쓰고 Data 를
 * 읽는 사이에 다른 태스크가 Address 를 덮어쓰면 엉뚱한 오프셋의 값을
 * 읽는다. config 접근 자체를 보호하는 pci_lock 은 접근 한 번만 감싸므로
 * 이 절차 전체를 덮지 못한다.
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
 * quirk_blacklist_vpd()   : VPD 접근이 장치를 망가뜨리는 것으로 알려진
 *                           모델에서 아예 접근을 막는다. vpd.len 을
 *                           PCI_VPD_SZ_INVALID 로 못박아 두는 방식이다.
 * quirk_chelsio_extend_vpd(): 반대 방향의 quirk. Chelsio 어댑터는 표준
 *                           위치 밖(0x400 또는 0xc00)에 진짜 VPD 를 두므로
 *                           계산된 크기를 무시하고 len 을 키운다.
 * vpd_read() / vpd_write() : sysfs 바이너리 속성의 콜백. 이 파일 안에 있다.
 * pci_vpd_available()      : cap 이 있는지, 필요하면 크기까지 확정한다.
 * pci_get_func0_dev()      : 같은 슬롯의 function 0 을 찾는다. F0 공유
 *                           quirk 가 걸린 장치의 우회 경로에 쓰인다.
 * pci_dev_vpd_attr_group   : sysfs 속성 그룹. pci-sysfs.c 가 목록에 넣는다.
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
 */

/* [한국어] pci.h — struct pci_dev, PCI_VPD_ADDR / PCI_VPD_DATA / PCI_VPD_ADDR_F 등
 * VPD capability 레지스터의 오프셋과 비트 정의가 여기 있다. */
#include <linux/pci.h>
/* [한국어] delay.h — usleep_range(). EEPROM 이 응답할 때까지 잠들며 기다린다. */
#include <linux/delay.h>
/* [한국어] export.h — EXPORT_SYMBOL / EXPORT_SYMBOL_GPL. 이 파일은 pci_read_vpd,
 * pci_vpd_alloc, pci_vpd_find_* 등 여덟 개를 모듈에 공개한다. */
#include <linux/export.h>
/* [한국어] sched/signal.h — fatal_signal_pending(). 느린 폴링 도중 프로세스가
 * 죽임을 당했는지 확인해 빠져나가기 위해 필요하다. */
#include <linux/sched/signal.h>
/* [한국어] unaligned.h — get_unaligned_le16 / get_unaligned_le32. VPD 의 길이 필드와
 * 쓰기 데이터가 버퍼 안 임의 위치에 있어 정렬이 보장되지 않는다. */
#include <linux/unaligned.h>
/* [한국어] pci.h(로컬) — PCI 코어 내부 선언. pci_user_read_config_ 계열,
 * pci_config_pm_runtime_get / _put, struct pci_vpd 접근에 필요하다. */
#include "pci.h"

/* [한국어] LRDT(Large Resource Data Type) 헤더 크기. 태그 1바이트 + 길이 2바이트 = 3.
 * 오프셋을 다음 태그로 옮길 때 데이터 길이에 이만큼을 더한다. */
#define PCI_VPD_LRDT_TAG_SIZE		3
/* [한국어] SRDT 태그 바이트의 하위 3비트가 길이 필드라 0x07 로 걸러 낸다.
 * 3비트뿐이라 SRDT 는 최대 7바이트까지만 담을 수 있다. */
#define PCI_VPD_SRDT_LEN_MASK		0x07
/* [한국어] SRDT(Short Resource Data Type) 헤더 크기. 태그와 길이가 한 바이트에
 * 함께 들어 있어 1이다. */
#define PCI_VPD_SRDT_TAG_SIZE		1
/* [한국어] SRDT 의 끝 태그 번호. 이 태그를 만나면 VPD 구조가 거기서 끝난다.
 * pci_vpd_size() 가 전체 크기를 확정하는 근거다. */
#define PCI_VPD_STIN_END		0x0f
/* [한국어] LRDT 안쪽 키워드 항목의 헤더 크기. 두 글자 키워드 + 길이 1바이트 = 3.
 * 항목 하나를 건너뛸 때 데이터 길이에 이만큼을 더한다. */
#define PCI_VPD_INFO_FLD_HDR_SIZE	3

/* [한국어]
 * pci_vpd_lrdt_size - LRDT 태그의 데이터 길이를 꺼낸다
 *
 * @lrdt: 태그 바이트를 가리키는 포인터.   @return: 뒤따르는 데이터의 바이트 수.
 *
 * LRDT(Large Resource Data Type)는 3바이트 헤더를 쓴다 - 태그 1바이트 뒤에
 * 길이 2바이트가 리틀엔디언으로 붙는다. 그 2바이트를 읽는 것이 전부다.
 * get_unaligned_le16 을 쓰는 이유는 이 길이 필드가 버퍼 안 임의 위치에
 * 있어 2바이트 정렬이 보장되지 않기 때문이다. 정렬을 요구하는 아키텍처에서
 * 그냥 캐스팅하면 예외가 난다.
 *
 * 실행 컨텍스트: 제약 없음. 순수 계산이다.
 *
 * 호출 체인:  pci_vpd_size() / pci_vpd_find_tag() → [이 함수] → get_unaligned_le16()
 */
static u16 pci_vpd_lrdt_size(const u8 *lrdt)
{
	/* [한국어] 태그 바로 뒤 2바이트가 리틀엔디언 길이다. lrdt+1 로 태그를 건너뛴다.
	 * 정렬이 보장되지 않아 get_unaligned 판을 쓴다. */
	return get_unaligned_le16(lrdt + 1);
}

/* [한국어]
 * pci_vpd_srdt_tag - SRDT 태그 바이트에서 태그 번호를 꺼낸다
 *
 * @srdt: 태그 바이트 포인터.   @return: 상위 5비트로 표현되는 태그 번호.
 *
 * SRDT(Short Resource Data Type)는 한 바이트에 태그와 길이를 함께 담는다.
 * 상위 5비트가 태그, 하위 3비트가 길이다. 그래서 3비트 오른쪽으로 밀면
 * 태그만 남는다. 별도의 마스크가 필요 없는 이유는 최상위 비트(PCI_VPD_LRDT)가
 * 0 이어야만 SRDT 이고, 이 함수는 그 판정이 끝난 뒤에만 불리기 때문이다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  pci_vpd_size() → [이 함수]
 */
static u8 pci_vpd_srdt_tag(const u8 *srdt)
{
	/* [한국어] 상위 5비트가 태그이므로 3비트 밀어낸다. 최상위 비트가 0 이어야만
	 * SRDT 이고 그 판정은 호출자가 이미 했으므로 추가 마스크가 필요 없다. */
	return *srdt >> 3;
}

/* [한국어]
 * pci_vpd_srdt_size - SRDT 태그 바이트에서 데이터 길이를 꺼낸다
 *
 * @srdt: 태그 바이트 포인터.   @return: 뒤따르는 데이터의 바이트 수(0..7).
 *
 * 위 pci_vpd_srdt_tag() 의 짝이다. 같은 바이트의 하위 3비트가 길이이므로
 * PCI_VPD_SRDT_LEN_MASK(0x07)로 걸러 낸다. 3비트뿐이라 SRDT 는 최대
 * 7바이트까지만 담을 수 있고, 그보다 큰 자원은 LRDT 를 써야 한다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  pci_vpd_size() → [이 함수]
 */
static u8 pci_vpd_srdt_size(const u8 *srdt)
{
	/* [한국어] 같은 바이트의 하위 3비트가 길이다. */
	return *srdt & PCI_VPD_SRDT_LEN_MASK;
}

/* [한국어]
 * pci_vpd_info_field_size - 키워드 항목의 데이터 길이를 꺼낸다
 *
 * @info_field: 항목 헤더의 시작(키워드 2바이트 + 길이 1바이트).
 * @return: 그 키워드가 담은 데이터의 바이트 수.
 *
 * LRDT 태그 안쪽은 다시 작은 항목들로 나뉜다. 항목 하나의 헤더는
 * PCI_VPD_INFO_FLD_HDR_SIZE(3바이트) - "PN", "SN" 같은 두 글자 키워드와
 * 길이 1바이트다. 그래서 인덱스 2 가 길이다.
 *
 * 길이가 1바이트뿐이라 항목 하나는 255바이트를 넘지 못한다.
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  pci_vpd_find_info_keyword() / pci_vpd_find_ro_info_keyword()
 *               → [이 함수]
 */
static u8 pci_vpd_info_field_size(const u8 *info_field)
{
	/* [한국어] 키워드 2바이트 다음이 길이 바이트라 인덱스 2 다.
	 * 1바이트뿐이므로 항목 하나는 255바이트를 넘지 못한다. */
	return info_field[2];
}

/* VPD access through PCI 2.2+ VPD capability */
/* [한국어] 아래부터는 PCI 2.2 이후의 VPD capability 를 통한 접근이다.
 * (상류 주석 "VPD access through PCI 2.2+ VPD capability") */

/* [한국어]
 * pci_get_func0_dev - 같은 슬롯의 function 0 장치를 찾아 참조를 건다
 *
 * @dev: 기준이 되는 장치.   @return: function 0 의 pci_dev(참조 계수가 올라간
 *       채로). 없으면 NULL. 호출자가 pci_dev_put() 으로 반납해야 한다.
 *
 * 다기능 장치에서 VPD EEPROM 이 실제로는 하나뿐이고 function 0 에만 연결된
 * 경우가 있다. 그런 장치에는 PCI_DEV_FLAGS_VPD_REF_F0 이 붙고, 모든 VPD
 * 접근이 function 0 으로 우회된다. 그 우회의 첫 단계가 이 함수다.
 *
 * 같은 슬롯인지는 devfn 으로 판단한다. PCI_SLOT() 으로 장치 번호만 떼어 내고
 * 기능 번호를 0 으로 다시 조립해 pci_get_slot() 에 넘긴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(pci_get_slot 이 버스 목록을 잠근다).
 *
 * 호출 체인:  vpd_read()/vpd_write()/__pci_read_vpd()/__pci_write_vpd()/
 *             quirk_f0_vpd_link() → [이 함수] → pci_get_slot()
 */
static struct pci_dev *pci_get_func0_dev(struct pci_dev *dev)
{
	/* [한국어] PCI_SLOT() 으로 devfn 에서 장치 번호만 떼어 내고 기능 번호를 0 으로
	 * 다시 조립해 같은 슬롯의 function 0 을 찾는다. 참조 계수가 올라간 채
	 * 돌아오므로 호출자가 pci_dev_put() 해야 한다. */
	return pci_get_slot(dev->bus, PCI_DEVFN(PCI_SLOT(dev->devfn), 0));
}

/* [한국어] VPD Address 레지스터의 주소 필드가 15비트(PCI_VPD_ADDR_MASK)라
 * 표현 가능한 최대 오프셋+1 이 곧 VPD 의 상한이다. 32KB. */
#define PCI_VPD_MAX_SIZE	(PCI_VPD_ADDR_MASK + 1)
/* [한국어] "이 장치의 VPD 는 쓸 수 없다" 는 표시값. 크기로 쓸 수 없는 UINT_MAX 를
 * 골라 정상 크기와 구분한다. quirk_blacklist_vpd() 도 이 값을 박아 넣는다. */
#define PCI_VPD_SZ_INVALID	UINT_MAX

/**
 * pci_vpd_size - determine actual size of Vital Product Data
 * @dev:	pci device struct
 */
/* [한국어]
 * pci_vpd_size - VPD 자원 태그들을 훑어 전체 크기를 알아낸다
 *
 * @dev: 대상 PCI 장치.
 * @return: 유효한 VPD 의 바이트 수. 형식이 처음부터 깨져 있으면
 *          PCI_VPD_SZ_INVALID(UINT_MAX)를 돌려주고, 도중에 깨졌으면
 *          거기까지의 오프셋을 돌려준다.
 *
 * VPD 에는 "전체 크기" 필드가 없다. 자원 태그를 하나씩 따라가며 끝 태그를
 * 만날 때까지 세는 것이 유일한 방법이고, 그것이 이 함수다.
 *
 * 태그는 두 가지다.
 *   LRDT - 최상위 비트가 1. 헤더 3바이트(태그 1 + 길이 2)를 읽어야 하므로
 *          길이 2바이트를 추가로 읽는다.
 *   SRDT - 최상위 비트가 0. 한 바이트에 태그와 길이가 함께 들어 있어
 *          추가 읽기가 필요 없다. 끝 태그(PCI_VPD_STIN_END)가 여기 속한다.
 *
 * 방어가 두 겹이다. 오프셋 0 의 첫 바이트가 0x00 이나 0xff 면 EEPROM 이
 * 아예 없는 것으로 본다(읽기가 실패할 때 버스가 돌려주는 값이다). 그리고
 * 어느 태그든 크기가 PCI_VPD_MAX_SIZE 를 넘기면 형식이 깨진 것으로 본다.
 *
 * 반환값 관용구 off ?: PCI_VPD_SZ_INVALID 가 두 상황을 가른다. 조금이라도
 * 읽었으면 그만큼은 유효하다고 보고, 시작부터 실패했으면 아예 못 쓴다고
 * 표시한다. 후자를 받은 pci_vpd_available() 이 cap 을 0 으로 만들어 이후
 * 모든 접근을 막는다.
 *
 * 크기 계산 자체가 VPD 읽기이므로 pci_read_vpd_any() 를 쓴다 - 크기를
 * 모르는 상태이니 크기 검사를 하지 않는 판이어야 한다(닭과 달걀 문제).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 안쪽에서 폴링하며 잠든다.
 *
 * 호출 체인:  pci_vpd_available() → [이 함수] → pci_read_vpd_any()
 */
static size_t pci_vpd_size(struct pci_dev *dev)
{
	/* [한국어] off 는 지금까지 훑은 위치, size 는 이번 태그의 데이터 길이. */
	size_t off = 0, size;
	/* [한국어] 태그 1바이트와 길이 2바이트를 담을 작업 버퍼. 상류 주석이 그 구성을 적었다. */
	unsigned char tag, header[1+2];	/* 1 byte tag, 2 bytes length */

	/* [한국어] 한 바이트(태그)를 읽어 본다. 1 이 아니면 더 읽을 수 없다는 뜻이라 루프가 끝난다.
	 * 크기를 알아내는 중이므로 크기 검사를 하지 않는 _any 판을 쓴다. */
	while (pci_read_vpd_any(dev, off, 1, header) == 1) {
		/* [한국어] 오류 로그에 찍힐 값이라 매 회차 초기화해 둔다. */
		size = 0;

		/* [한국어] 맨 앞 바이트가 0x00 이나 0xff 면 EEPROM 이 없는 것으로 본다.
		 * 읽기가 실패할 때 버스가 돌려주는 값이 그 둘이기 때문이다. */
		if (off == 0 && (header[0] == 0x00 || header[0] == 0xff))
			/* [한국어] 형식 오류 경로로. */
			goto error;

		/* [한국어] 최상위 비트가 서 있으면 LRDT — 길이가 2바이트 따로 붙는다. */
		if (header[0] & PCI_VPD_LRDT) {
			/* Large Resource Data Type Tag */
			/* [한국어] 길이 2바이트를 추가로 읽는다. 2 가 아니면 읽기가 잘린 것이다. */
			if (pci_read_vpd_any(dev, off + 1, 2, &header[1]) != 2) {
				/* [한국어] 어느 오프셋에서 실패했는지 남기고 */
				pci_warn(dev, "failed VPD read at offset %zu\n",
					 off + 1);
				/* [한국어] 조금이라도 읽었으면 그만큼은 유효하다고 보고, 시작부터 실패했으면
				 * 못 쓴다고 표시한다. off ?: 관용구가 그 둘을 가른다. */
				return off ?: PCI_VPD_SZ_INVALID;
			}
			/* [한국어] 길이 2바이트를 해석한다. */
			size = pci_vpd_lrdt_size(header);
			/* [한국어] 선언된 길이가 VPD 상한을 넘으면 형식이 깨진 것이다. */
			if (off + size > PCI_VPD_MAX_SIZE)
				/* [한국어] 오류 경로로. */
				goto error;

			/* [한국어] 헤더 3바이트와 데이터를 함께 건너뛴다. */
			off += PCI_VPD_LRDT_TAG_SIZE + size;
		/* [한국어] 최상위 비트가 0 이면 SRDT. */
		} else {
			/* Short Resource Data Type Tag */
			/* [한국어] 상위 5비트에서 태그 번호를 꺼낸다. */
			tag = pci_vpd_srdt_tag(header);
			/* [한국어] 하위 3비트에서 길이를 꺼낸다. */
			size = pci_vpd_srdt_size(header);
			/* [한국어] 역시 상한을 넘는지 본다. */
			if (off + size > PCI_VPD_MAX_SIZE)
				/* [한국어] 오류 경로로. */
				goto error;

			/* [한국어] 헤더 1바이트와 데이터를 함께 건너뛴다. */
			off += PCI_VPD_SRDT_TAG_SIZE + size;
			/* [한국어] 끝 태그를 만났다 — VPD 구조가 정상적으로 끝났다. */
			if (tag == PCI_VPD_STIN_END)	/* End tag descriptor */
				/* [한국어] 끝 태그까지 포함한 오프셋이 곧 전체 크기다. */
				return off;
		}
	}
	/* [한국어] 루프가 읽기 실패로 끝난 경우. 여기까지 훑은 만큼을 크기로 본다. */
	return off;

/* [한국어] 형식 오류 공통 경로. */
error:
	/* [한국어] 어떤 태그가 어느 오프셋에서 문제였는지 남긴다. */
	pci_info(dev, "invalid VPD tag %#04x (size %zu) at offset %zu%s\n",
		 /* [한국어] 오프셋 0 이면 EEPROM 이 아예 없을 가능성이 커서 */
		 header[0], size, off, off == 0 ?
		 /* [한국어] 그 취지의 문구를 덧붙인다. 정상일 수 있는 상황이라 경고 수준을 낮춘 것이다. */
		 "; assume missing optional EEPROM" : "");
	/* [한국어] 위와 같은 관용구 — 부분 성공과 완전 실패를 가른다. */
	return off ?: PCI_VPD_SZ_INVALID;
}

/* [한국어]
 * pci_vpd_available - VPD 를 쓸 수 있는 상태인지 확인하고 필요하면 크기를 확정한다
 *
 * @dev:        대상 PCI 장치.
 * @check_size: true 면 크기까지 확정한다. false 면 capability 존재만 본다.
 * @return: 접근 가능하면 true.
 *
 * 모든 VPD 접근의 문지기다. 두 가지를 본다.
 *   1) vpd->cap 이 0 이면 이 장치에 VPD capability 가 없다. 곧바로 false.
 *   2) 아직 크기를 모르고(len == 0) 크기 검사가 필요하면 지금 계산한다.
 *      계산 결과가 PCI_VPD_SZ_INVALID 면 형식이 깨진 것이므로 cap 을 0 으로
 *      만들어 버린다 - 다음부터는 1번에서 곧바로 걸러진다. 한 번 실패한
 *      장치를 매번 다시 긁지 않으려는 캐싱이다.
 *
 * check_size 가 false 인 경로가 따로 있는 이유는 pci_vpd_size() 자신이
 * 이 함수를 거쳐 읽기를 하기 때문이다. 크기를 알아내려고 크기를 요구하면
 * 무한 재귀가 된다.
 *
 * 락을 잡지 않은 채 vpd->len 과 vpd->cap 을 고치는 점은 눈여겨볼 만하다.
 * 호출자인 pci_vpd_read()/write() 가 뮤텍스를 잡기 *전에* 이 함수를 부르므로,
 * 두 태스크가 동시에 들어오면 크기 계산이 중복될 수 있다. 결과값이 같아
 * 실질적 해는 없지만 설계상의 사실이라 적어 둔다. 코드는 고치지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pci_vpd_read()/pci_vpd_write()/pci_vpd_alloc() → [이 함수]
 *               → pci_vpd_size()
 */
static bool pci_vpd_available(struct pci_dev *dev, bool check_size)
{
	/* [한국어] 이 장치의 VPD 상태 묶음. 아래에서 cap 과 len 을 읽고 고친다. */
	struct pci_vpd *vpd = &dev->vpd;

	/* [한국어] capability 오프셋이 0 이면 이 장치에 VPD 가 없다. */
	if (!vpd->cap)
		/* [한국어] 접근 불가. */
		return false;

	/* [한국어] 아직 크기를 모르고, 크기 검사가 필요한 호출이면 지금 계산한다.
	 * check_size 가 false 인 경로는 pci_vpd_size() 자신을 위한 것이다 —
	 * 크기를 알아내려고 크기를 요구하면 무한 재귀가 된다. */
	if (vpd->len == 0 && check_size) {
		/* [한국어] 태그를 훑어 크기를 확정하고 캐시한다. 느린 작업이라 한 번만 한다. */
		vpd->len = pci_vpd_size(dev);
		/* [한국어] 형식이 처음부터 깨져 있었다. */
		if (vpd->len == PCI_VPD_SZ_INVALID) {
			/* [한국어] capability 오프셋을 지워 버린다. 다음부터는 위 검사에서 곧바로 걸린다.
			 * 한 번 실패한 장치를 매번 다시 긁지 않으려는 캐싱이다. */
			vpd->cap = 0;
			/* [한국어] 접근 불가. */
			return false;
		}
	}

	/* [한국어] 쓸 수 있다. 참고로 이 함수는 호출자가 뮤텍스를 잡기 전에 불리므로
	 * len/cap 갱신이 락 밖에서 일어난다. 동시 진입 시 계산이 중복될 수는
	 * 있으나 결과가 같아 실질적 해는 없다. 코드는 고치지 않고 사실만 적어 둔다. */
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
/* [한국어]
 * pci_vpd_wait - VPD 하드웨어가 한 번의 전송을 끝내기를 기다린다
 *
 * @dev: 대상 PCI 장치.
 * @set: true 면 F 비트가 서기를, false 면 내려가기를 기다린다.
 * @return: 0 성공. -ETIMEDOUT 은 125ms 안에 끝나지 않은 것,
 *          그 밖의 음수는 config 읽기 자체가 실패한 것.
 *
 * VPD 는 직렬 EEPROM 에 붙어 있어 한 번의 4바이트 전송에 수 밀리초가
 * 걸릴 수 있다. 완료를 알리는 인터럽트가 없으므로 폴링밖에 방법이 없고,
 * 상류 주석이 그 사정을 그대로 적어 두었다.
 *
 * 완료 신호는 VPD Address 레지스터의 F 비트(PCI_VPD_ADDR_F)다. 의미가
 * 방향마다 반대인 것이 핵심이다.
 *   읽기 - 커널이 F=0 으로 주소를 쓰고, 하드웨어가 다 읽으면 F=1 로 만든다.
 *   쓰기 - 커널이 F=1 로 주소를 쓰고, 하드웨어가 다 쓰면 F=0 으로 만든다.
 * 그래서 기다릴 값을 @set 으로 받는다.
 *
 * 잠자는 간격을 10us 에서 시작해 매번 두 배로 늘리되 1024us 에서 멈춘다.
 * 빨리 끝나는 장치는 짧게 기다려 응답이 좋고, 느린 장치는 폴링 횟수가
 * 로그 규모로 줄어 CPU 를 덜 태운다. 전체 상한은 125ms 다.
 *
 * 시간이 다 되면 경고를 찍는데, 문구가 "펌웨어 버그일 가능성이 높으니
 * 카드 벤더에 연락하라" 로 되어 있다. 정상 하드웨어라면 이 시간 안에
 * 끝나야 한다는 판단이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용. usleep_range 로 잠든다.
 *
 * 호출 체인:  pci_vpd_read() / pci_vpd_write() → [이 함수]
 *               → pci_user_read_config_word() → usleep_range()
 */
static int pci_vpd_wait(struct pci_dev *dev, bool set)
{
	/* [한국어] 이 장치의 VPD 상태 묶음. cap 오프셋을 쓰려고 가져온다. */
	struct pci_vpd *vpd = &dev->vpd;
	/* [한국어] 전체 대기 상한. 지금부터 125ms 뒤의 시각을 미리 계산해 둔다. */
	unsigned long timeout = jiffies + msecs_to_jiffies(125);
	/* [한국어] 이번에 잠들 최대 시간(마이크로초). 16 에서 시작해 두 배씩 늘린다. */
	unsigned long max_sleep = 16;
	/* [한국어] VPD Address 레지스터에서 읽은 값. F 비트를 여기서 본다. */
	u16 status;
	/* [한국어] config 읽기 결과. */
	int ret;

	/* [한국어] 완료되거나 시간이 다 될 때까지 돈다. */
	do {
		/* [한국어] VPD Address 레지스터를 읽는다. 사용자 접근용(_user_) 판을 쓰는 이유는
		 * 이 경로가 결국 sysfs 를 통한 사용자 요청에서 오기 때문이다. */
		ret = pci_user_read_config_word(dev, vpd->cap + PCI_VPD_ADDR,
						&status);
		/* [한국어] config 접근 자체가 실패했다(장치가 사라졌을 수 있다). */
		if (ret < 0)
			/* [한국어] 그 errno 를 그대로 전한다. */
			return ret;

		/* [한국어] F 비트를 bool 로 정규화해 기다리던 값과 비교한다.
		 * 읽기는 F=1 을, 쓰기는 F=0 을 기다린다 — 방향마다 의미가 반대다. */
		if (!!(status & PCI_VPD_ADDR_F) == set)
			/* [한국어] 원하는 상태가 됐다. 전송 완료. */
			return 0;

		/* [한국어] 125ms 를 넘겼는가. time_after 를 쓰는 이유는 jiffies 가 순환하기 때문이다. */
		if (time_after(jiffies, timeout))
			/* [한국어] 루프를 벗어나 아래 경고로 간다. */
			break;

		/* [한국어] 10us 에서 max_sleep 사이만큼 잠든다. 범위를 주면 커널이 다른 타이머와
		 * 묶어 깨울 수 있어 인터럽트 횟수가 줄어든다. */
		usleep_range(10, max_sleep);
		/* [한국어] 상한 1024us 까지만 늘린다. */
		if (max_sleep < 1024)
			/* [한국어] 두 배로. 빨리 끝나는 장치는 짧게 기다려 응답이 좋고, 느린 장치는
			 * 폴링 횟수가 로그 규모로 줄어 CPU 를 덜 태운다. */
			max_sleep *= 2;
	} while (true);

	/* [한국어] 정상 하드웨어라면 125ms 안에 끝나야 한다는 판단이라, 커널 문제가 아니라
	 * 장치 펌웨어 문제임을 문구로 분명히 하고 벤더 연락을 권한다. */
	pci_warn(dev, "VPD access failed.  This is likely a firmware bug on this device.  Contact the card vendor for a firmware update\n");
	/* [한국어] 시간 초과. */
	return -ETIMEDOUT;
}

/* [한국어]
 * pci_vpd_read - VPD 에서 임의 범위를 읽어 버퍼에 담는다
 *
 * @dev:        대상 PCI 장치.
 * @pos:        읽기 시작 오프셋.
 * @count:      바이트 수.
 * @arg:        결과를 담을 버퍼.
 * @check_size: 캐시된 크기로 범위를 제한할 것인가.
 * @return: 실제로 읽은 바이트 수. 실패하면 음수 errno
 *          (-ENODEV VPD 없음, -EINVAL 음수 오프셋, -EINTR 신호로 중단).
 *
 * 하드웨어는 4바이트 단위로만 읽을 수 있는데 호출자는 아무 오프셋에서
 * 아무 길이나 요구할 수 있다. 그 간극을 메우는 것이 이 함수의 일이다.
 *
 * 반복마다 세 단계를 밟는다.
 *   1) Address 레지스터에 pos & ~3 을 쓴다 - 4바이트 경계로 내림한 주소다.
 *   2) pci_vpd_wait(dev, true) 로 F 비트가 설 때까지 기다린다.
 *   3) Data 레지스터에서 32비트를 읽는다.
 * 그리고 읽어 온 4바이트 중 필요한 부분만 골라 낸다. skip = pos & 3 이
 * 앞에서 버릴 바이트 수이고, 안쪽 루프가 val 을 8비트씩 밀며 낮은
 * 바이트부터 꺼낸다(VPD Data 는 리틀엔디언이다).
 *
 * 범위 처리도 눈여겨볼 만하다. 시작이 끝을 넘으면 오류가 아니라 0 을
 * 돌려주고(파일 끝과 같은 의미), 끝이 넘치면 넘치는 만큼만 잘라 읽는다.
 * sysfs 읽기의 관례에 맞춘 것이다.
 *
 * 뮤텍스를 mutex_lock_killable 로 잡는다. EEPROM 이 느려 대기가 길어질 수
 * 있으므로, 사용자가 프로세스를 죽이려 할 때 응답할 수 있어야 하기
 * 때문이다. 루프 안에서도 매번 fatal_signal_pending 을 확인해 중간에
 * 빠져나갈 길을 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 *
 * 호출 체인:  __pci_read_vpd() → [이 함수] → pci_vpd_available()
 *               → pci_user_write_config_word() → pci_vpd_wait()
 *               → pci_user_read_config_dword()
 */
static ssize_t pci_vpd_read(struct pci_dev *dev, loff_t pos, size_t count,
			    void *arg, bool check_size)
{
	/* [한국어] 이 장치의 VPD 상태 묶음. */
	struct pci_vpd *vpd = &dev->vpd;
	/* [한국어] 이번 요청에 적용할 상한. */
	unsigned int max_len;
	/* [한국어] 오류 코드. 성공하면 0 인 채로 아래 반환식이 count 를 고른다. */
	int ret = 0;
	/* [한국어] 요청 구간의 끝(배타적). */
	loff_t end = pos + count;
	/* [한국어] void 인자를 바이트 포인터로 받아 둔다. 아래에서 한 바이트씩 채운다. */
	u8 *buf = arg;

	/* [한국어] VPD 를 쓸 수 있는 상태인지, 필요하면 크기까지 확정한다. */
	if (!pci_vpd_available(dev, check_size))
		/* [한국어] 장치에 VPD 가 없거나 형식이 깨졌다. */
		return -ENODEV;

	/* [한국어] 음수 오프셋은 있을 수 없다. */
	if (pos < 0)
		/* [한국어] 인자 오류. */
		return -EINVAL;

	/* [한국어] 크기 검사를 하는 호출이면 캐시된 유효 크기가 상한이고,
	 * 아니면 하드웨어가 표현할 수 있는 최대치가 상한이다. */
	max_len = check_size ? vpd->len : PCI_VPD_MAX_SIZE;

	/* [한국어] 시작 위치가 이미 유효 범위 밖이다. */
	if (pos >= max_len)
		/* [한국어] 오류가 아니라 0 을 돌려준다 — 파일 끝과 같은 의미다. sysfs 읽기의 관례다. */
		return 0;

	/* [한국어] 끝이 범위를 넘치면 */
	if (end > max_len) {
		/* [한국어] 상한까지로 자르고 */
		end = max_len;
		/* [한국어] 실제로 읽을 길이도 함께 줄인다. 부분 읽기를 허용하는 것이 읽기 쪽 정책이다. */
		count = end - pos;
	}

	/* [한국어] 3단계 절차 전체를 직렬화한다. Address 를 쓰고 Data 를 읽는 사이에 다른
	 * 태스크가 Address 를 덮어쓰면 엉뚱한 오프셋의 값을 읽기 때문이다.
	 * killable 판인 이유는 EEPROM 이 느려 대기가 길어질 수 있어서다. */
	if (mutex_lock_killable(&vpd->lock))
		/* [한국어] 신호로 깨어났다. */
		return -EINTR;

	/* [한국어] 요청 구간을 4바이트씩 훑는다. */
	while (pos < end) {
		/* [한국어] 이번에 읽어 올 32비트 워드. */
		u32 val;
		/* [한국어] i 는 워드 안 바이트 위치, skip 은 앞에서 버릴 바이트 수. */
		unsigned int i, skip;

		/* [한국어] 루프 안에서도 매번 확인한다. 뮤텍스를 잡은 뒤에는 kill 신호를 받아도
		 * 자동으로 빠져나가지 못하므로 직접 봐야 한다. */
		if (fatal_signal_pending(current)) {
			/* [한국어] 중단됐음을 기록하고 */
			ret = -EINTR;
			/* [한국어] 루프를 벗어난다. 아래에서 뮤텍스는 반드시 풀린다. */
			break;
		}

		/* [한국어] 1단계 — Address 레지스터에 읽을 오프셋을 쓴다. */
		ret = pci_user_write_config_word(dev, vpd->cap + PCI_VPD_ADDR,
						 /* [한국어] pos & ~3 으로 4바이트 경계까지 내림한다. 하드웨어가 정렬된 워드 단위로만
						  * 읽을 수 있기 때문이다. F 비트는 0 인 채로 두어 "읽어 오라" 는 뜻이 된다. */
						 pos & ~3);
		/* [한국어] config 쓰기 실패. */
		if (ret < 0)
			/* [한국어] 루프 탈출. */
			break;
		/* [한국어] 2단계 — F 비트가 1 이 될 때까지 기다린다. 읽기는 하드웨어가 다 가져오면
		 * F 를 세워 알린다. */
		ret = pci_vpd_wait(dev, true);
		/* [한국어] 시간 초과이거나 config 접근 실패. */
		if (ret < 0)
			/* [한국어] 루프 탈출. */
			break;

		/* [한국어] 3단계 — Data 레지스터에서 32비트를 읽는다. */
		ret = pci_user_read_config_dword(dev, vpd->cap + PCI_VPD_DATA, &val);
		/* [한국어] 읽기 실패. */
		if (ret < 0)
			/* [한국어] 루프 탈출. */
			break;

		/* [한국어] 요청 시작이 워드 경계에서 얼마나 어긋났는지. 그만큼 앞바이트를 버린다. */
		skip = pos & 3;
		/* [한국어] 워드의 네 바이트를 낮은 쪽부터 훑는다. */
		for (i = 0;  i < sizeof(u32); i++) {
			/* [한국어] skip 만큼 지난 뒤부터가 호출자가 원하는 바이트다. */
			if (i >= skip) {
				/* [한국어] val 의 최하위 바이트가 대입되며 잘린다. VPD Data 가 리틀엔디언이라
				 * 낮은 바이트가 낮은 오프셋에 해당한다. */
				*buf++ = val;
				/* [한국어] 요청 끝에 닿으면 */
				if (++pos == end)
					/* [한국어] 워드 중간이라도 멈춘다. 뒤쪽 정렬이 어긋난 요청을 처리하는 부분이다. */
					break;
			}
			/* [한국어] 다음 바이트를 최하위로 끌어내린다. */
			val >>= 8;
		}
	}

	/* [한국어] 어느 경로로 빠져나왔든 뮤텍스를 푼다. */
	mutex_unlock(&vpd->lock);
	/* [한국어] 오류가 있었으면 그 errno 를, 아니면 요청한 바이트 수를 돌려준다.
	 * 중간에 끊겨도 count 를 돌려주지 않는 이유는 어디까지 채웠는지
	 * 호출자가 알 방법이 없기 때문이다. */
	return ret ? ret : count;
}

/* [한국어]
 * pci_vpd_write - 버퍼 내용을 VPD 에 써 넣는다
 *
 * @dev:        대상 PCI 장치.
 * @pos:        쓰기 시작 오프셋. 4의 배수여야 한다.
 * @count:      바이트 수. 역시 4의 배수여야 한다.
 * @arg:        쓸 데이터.
 * @check_size: 캐시된 크기로 범위를 제한할 것인가.
 * @return: 쓴 바이트 수, 또는 음수 errno.
 *
 * 읽기와 대칭이지만 두 가지가 다르다.
 *
 * 첫째, 정렬을 강제한다. 읽기는 어긋난 요청을 잘라 붙여 처리했지만
 * 쓰기는 -EINVAL 로 거절한다. 4바이트 중 일부만 바꾸려면 읽고-고쳐-쓰기를
 * 해야 하는데, 그 사이에 다른 주체가 끼어들 수 있어 커널이 임의로 하기에는
 * 위험하기 때문이다. 정렬을 맞추는 책임을 호출자에게 넘긴 셈이다.
 *
 * 둘째, 절차 순서가 뒤집힌다. 읽기는 "주소 → 대기 → 데이터" 였지만
 * 쓰기는 "데이터 → 주소(F=1) → 대기" 다. 데이터를 먼저 넣어 두고
 * Address 에 F 비트를 세워 "이제 EEPROM 에 넣어라" 고 지시하는 방식이다.
 * 그래서 기다릴 값도 반대다 - pci_vpd_wait(dev, false) 로 F 가 내려가기를
 * 기다린다.
 *
 * 범위를 넘는 요청도 읽기와 달리 잘라 내지 않고 -EINVAL 로 거절한다.
 * 일부만 쓰고 성공을 알리면 호출자가 데이터가 온전히 들어갔다고 오해한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용.
 *
 * 호출 체인:  __pci_write_vpd() → [이 함수] → pci_user_write_config_dword()
 *               → pci_user_write_config_word() → pci_vpd_wait()
 */
static ssize_t pci_vpd_write(struct pci_dev *dev, loff_t pos, size_t count,
			     const void *arg, bool check_size)
{
	/* [한국어] 이 장치의 VPD 상태 묶음. */
	struct pci_vpd *vpd = &dev->vpd;
	/* [한국어] 이번 요청에 적용할 상한. */
	unsigned int max_len;
	/* [한국어] 쓸 데이터. const 인 점이 읽기 쪽과 다르다. */
	const u8 *buf = arg;
	/* [한국어] 요청 구간의 끝(배타적). */
	loff_t end = pos + count;
	/* [한국어] 오류 코드. */
	int ret = 0;

	/* [한국어] VPD 를 쓸 수 있는 상태인지 확인한다. */
	if (!pci_vpd_available(dev, check_size))
		/* [한국어] 장치에 VPD 가 없거나 형식이 깨졌다. */
		return -ENODEV;

	/* [한국어] 음수이거나, 시작이 4의 배수가 아니거나, 길이가 4의 배수가 아니면 거절한다.
	 * 읽기와 달리 잘라 붙이지 않는 이유는 부분 갱신에 읽고-고쳐-쓰기가 필요한데
	 * 그 사이에 다른 주체가 끼어들 수 있어 커널이 임의로 하기 위험해서다. */
	if (pos < 0 || (pos & 3) || (count & 3))
		/* [한국어] 정렬을 맞출 책임을 호출자에게 넘긴다. */
		return -EINVAL;

	/* [한국어] 읽기와 같은 방식으로 상한을 고른다. */
	max_len = check_size ? vpd->len : PCI_VPD_MAX_SIZE;

	/* [한국어] 범위를 넘치면 */
	if (end > max_len)
		/* [한국어] 거절한다. 읽기처럼 잘라 쓰지 않는 이유는, 일부만 쓰고 성공을 알리면
		 * 호출자가 데이터가 온전히 들어갔다고 오해하기 때문이다. */
		return -EINVAL;

	/* [한국어] 절차 전체를 직렬화한다. */
	if (mutex_lock_killable(&vpd->lock))
		/* [한국어] 신호로 깨어났다. */
		return -EINTR;

	/* [한국어] 요청 구간을 4바이트씩 훑는다. */
	while (pos < end) {
		/* [한국어] 1단계 — 읽기와 순서가 뒤집힌다. Data 를 먼저 넣어 둔다. */
		ret = pci_user_write_config_dword(dev, vpd->cap + PCI_VPD_DATA,
						  /* [한국어] 버퍼에서 리틀엔디언 32비트를 꺼낸다. 버퍼가 임의 위치라 정렬이 보장되지 않는다. */
						  get_unaligned_le32(buf));
		/* [한국어] config 쓰기 실패. */
		if (ret < 0)
			/* [한국어] 루프 탈출. */
			break;
		/* [한국어] 2단계 — Address 를 쓰면서 */
		ret = pci_user_write_config_word(dev, vpd->cap + PCI_VPD_ADDR,
						 /* [한국어] F 비트를 함께 세운다. 이것이 "지금 넣은 데이터를 EEPROM 에 써라" 는 지시다.
						  * 읽기에서 F 를 0 으로 두었던 것과 정반대다. */
						 pos | PCI_VPD_ADDR_F);
		/* [한국어] config 쓰기 실패. */
		if (ret < 0)
			/* [한국어] 루프 탈출. */
			break;

		/* [한국어] 3단계 — F 비트가 내려가기를 기다린다. 쓰기 완료 신호는 F=0 이다. */
		ret = pci_vpd_wait(dev, false);
		/* [한국어] 시간 초과이거나 접근 실패. */
		if (ret < 0)
			/* [한국어] 루프 탈출. */
			break;

		/* [한국어] 다음 워드로. 정렬이 보장돼 있어 읽기처럼 바이트 단위로 자를 필요가 없다. */
		buf += sizeof(u32);
		/* [한국어] 오프셋도 4바이트 전진. */
		pos += sizeof(u32);
	}

	/* [한국어] 뮤텍스 해제. */
	mutex_unlock(&vpd->lock);
	/* [한국어] 오류가 있었으면 errno, 아니면 요청한 바이트 수. */
	return ret ? ret : count;
}

/* [한국어]
 * pci_vpd_init - 열거 단계에서 VPD 접근 준비를 한다
 *
 * @dev: 방금 발견된 PCI 장치.   @return: 없음.
 *
 * capability 목록에서 VPD 를 찾아 오프셋을 vpd.cap 에 넣고 뮤텍스를
 * 초기화한다. 실제 읽기는 하지 않는다 - 열거는 모든 장치에 대해 일어나는데
 * VPD 읽기는 밀리초 단위로 느려서, 처음 요청이 올 때까지 미룬다.
 *
 * 맨 앞의 검사가 중요하다. vpd.len 이 이미 PCI_VPD_SZ_INVALID 라면
 * quirk_blacklist_vpd() 가 앞서 그렇게 못박아 둔 것이므로, cap 을 찾지
 * 않고 그대로 돌아간다. 순서가 성립하는 이유는 그 quirk 가
 * DECLARE_PCI_FIXUP_HEADER 로 등록되어 pci_init_capabilities() 보다
 * 먼저 실행되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(버스 열거).
 *
 * 호출 체인:  pci_init_capabilities() [probe.c:6413] → [이 함수]
 *               → pci_find_capability() → mutex_init()
 */
void pci_vpd_init(struct pci_dev *dev)
{
	/* [한국어] quirk_blacklist_vpd() 가 이미 못 쓴다고 박아 두었으면 */
	if (dev->vpd.len == PCI_VPD_SZ_INVALID)
		/* [한국어] capability 를 찾지도 않고 물러난다. 그 quirk 가 DECLARE_PCI_FIXUP_HEADER 로
		 * 등록되어 pci_init_capabilities() 보다 먼저 실행되기에 성립하는 순서다. */
		return;

	/* [한국어] capability 목록에서 VPD 를 찾아 오프셋을 기록한다. 없으면 0 이 되어
	 * 이후 pci_vpd_available() 이 항상 false 를 준다. */
	dev->vpd.cap = pci_find_capability(dev, PCI_CAP_ID_VPD);
	/* [한국어] 접근 직렬화용 뮤텍스를 초기화한다. 여기서는 실제 읽기를 하지 않는다 —
	 * 열거는 모든 장치에 일어나는데 VPD 읽기는 밀리초 단위로 느리기 때문이다. */
	mutex_init(&dev->vpd.lock);
}

/* [한국어]
 * vpd_read - /sys 의 vpd 파일을 읽을 때 불리는 콜백
 *
 * @filp, @kobj, @bin_attr: sysfs 코어가 넘기는 인자. kobj 에서 pci_dev 를 얻는다.
 * @buf:   결과 버퍼.   @off: 파일 오프셋.   @count: 요청 바이트 수.
 * @return: 읽은 바이트 수, 또는 음수 errno.
 *
 * 이 함수가 pci-sysfs.c 가 아니라 이 파일에 있다는 점을 짚어 둔다.
 * pci-sysfs.c 는 pci_dev_vpd_attr_group 을 속성 그룹 목록에 넣을 뿐이고,
 * 그룹의 실체와 읽기/쓰기 콜백은 전부 여기 있다.
 *
 * 하는 일이 셋이다.
 *   1) F0 공유 quirk 가 걸린 장치면 대상을 function 0 으로 바꾼다.
 *      이때 참조를 걸었으므로 끝에서 반드시 반납해야 한다.
 *   2) 런타임 PM 참조를 잡는다. 절전 중인 장치는 config 접근이 통하지
 *      않으므로 깨워 두어야 하고, 읽기가 끝나면 다시 놓아 준다.
 *   3) pci_read_vpd() 로 실제 읽기.
 *
 * PM 참조를 잡는 대상이 dev 가 아니라 vpd_dev 인 점이 맞다. 실제로
 * config 접근을 받을 장치가 깨어 있어야 하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자 read 시스템 호출).
 *
 * 호출 체인:  sysfs read → [이 함수] → pci_get_func0_dev()
 *               → pci_config_pm_runtime_get() → pci_read_vpd()
 */
static ssize_t vpd_read(struct file *filp, struct kobject *kobj,
			const struct bin_attribute *bin_attr, char *buf,
			loff_t off, size_t count)
{
	/* [한국어] sysfs 가 넘긴 kobject 에서 struct device 를 거쳐 pci_dev 로 되돌린다. */
	struct pci_dev *dev = to_pci_dev(kobj_to_dev(kobj));
	/* [한국어] 실제로 config 접근을 받을 장치. 기본은 자기 자신이다. */
	struct pci_dev *vpd_dev = dev;
	/* [한국어] 읽은 바이트 수. */
	ssize_t ret;

	/* [한국어] VPD EEPROM 이 function 0 에만 달린 장치인가. */
	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0) {
		/* [한국어] 그렇다면 같은 슬롯의 function 0 으로 대상을 바꾼다(참조가 걸린다). */
		vpd_dev = pci_get_func0_dev(dev);
		/* [한국어] function 0 이 없다 — quirk 가 걸렸는데 대상이 사라진 비정상 상태다. */
		if (!vpd_dev)
			/* [한국어] 장치 없음. */
			return -ENODEV;
	}

	/* [한국어] 절전 중이면 config 접근이 통하지 않으므로 깨워서 붙잡아 둔다.
	 * 대상이 dev 가 아니라 vpd_dev 인 것이 맞다 — 실제로 접근받을 장치가
	 * 깨어 있어야 한다. */
	pci_config_pm_runtime_get(vpd_dev);
	/* [한국어] 실제 읽기. 크기 검사를 하는 판이다. */
	ret = pci_read_vpd(vpd_dev, off, count, buf);
	/* [한국어] 런타임 PM 참조를 놓아 준다. 다시 절전으로 내려갈 수 있게 된다. */
	pci_config_pm_runtime_put(vpd_dev);

	/* [한국어] 우회했었다면 */
	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0)
		/* [한국어] pci_get_func0_dev() 이 건 참조를 반납한다. */
		pci_dev_put(vpd_dev);

	/* [한국어] 읽은 바이트 수 또는 errno 를 sysfs 코어에 돌려준다. */
	return ret;
}

/* [한국어]
 * vpd_write - /sys 의 vpd 파일에 쓸 때 불리는 콜백
 *
 * @filp, @kobj, @bin_attr: sysfs 코어가 넘기는 인자.
 * @buf:   쓸 데이터.   @off: 파일 오프셋.   @count: 바이트 수.
 * @return: 쓴 바이트 수, 또는 음수 errno.
 *
 * vpd_read() 와 구조가 완전히 같고 마지막에 부르는 함수만 다르다.
 * F0 우회 → PM 참조 획득 → pci_write_vpd() → PM 참조 반납 → 참조 반납.
 *
 * 속성의 권한이 0600 이라 root 만 열 수 있다. VPD 를 잘못 쓰면 장치의
 * 일련번호나 설정이 망가질 수 있어서다. 정렬 제약(4바이트 단위)은 아래
 * pci_vpd_write() 가 강제하므로 여기서는 검사하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자 write 시스템 호출).
 *
 * 호출 체인:  sysfs write → [이 함수] → pci_get_func0_dev()
 *               → pci_config_pm_runtime_get() → pci_write_vpd()
 */
static ssize_t vpd_write(struct file *filp, struct kobject *kobj,
			 const struct bin_attribute *bin_attr, char *buf,
			loff_t off, size_t count)
{
	/* [한국어] kobject 에서 pci_dev 로 되돌린다. */
	struct pci_dev *dev = to_pci_dev(kobj_to_dev(kobj));
	/* [한국어] 실제로 쓰기를 받을 장치. 기본은 자기 자신이다. */
	struct pci_dev *vpd_dev = dev;
	/* [한국어] 쓴 바이트 수. */
	ssize_t ret;

	/* [한국어] F0 공유 quirk 가 걸린 장치인가. */
	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0) {
		/* [한국어] function 0 으로 대상을 바꾼다. */
		vpd_dev = pci_get_func0_dev(dev);
		/* [한국어] 대상이 없다. */
		if (!vpd_dev)
			/* [한국어] 장치 없음. */
			return -ENODEV;
	}

	/* [한국어] 절전 중인 장치는 config 접근이 통하지 않으므로 깨워 붙잡아 둔다. */
	pci_config_pm_runtime_get(vpd_dev);
	/* [한국어] 실제 쓰기. 정렬 제약(4바이트 단위)은 이 아래 pci_vpd_write() 가 강제한다. */
	ret = pci_write_vpd(vpd_dev, off, count, buf);
	/* [한국어] 런타임 PM 참조 반납. */
	pci_config_pm_runtime_put(vpd_dev);

	/* [한국어] 우회했었다면 */
	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0)
		/* [한국어] function 0 참조를 반납한다. */
		pci_dev_put(vpd_dev);

	/* [한국어] 쓴 바이트 수 또는 errno. */
	return ret;
}
/* [한국어] sysfs 바이너리 속성 정의. BIN_ATTR 매크로가 struct bin_attribute
 * bin_attr_vpd 를 만들어 내며, 이름 "vpd", 권한 0600, 읽기/쓰기 콜백,
 * 크기 0(가변)을 채운다. 권한이 0600 인 이유는 VPD 를 잘못 쓰면 장치의
 * 일련번호나 설정이 망가질 수 있어 root 만 열게 하려는 것이다. */
static const BIN_ATTR(vpd, 0600, vpd_read, vpd_write, 0);

/* [한국어] 위 속성 하나만 담은 배열. attribute_group 이 NULL 로 끝나는 배열을
 * 요구하므로 마지막 원소가 NULL 이다. */
static const struct bin_attribute *const vpd_attrs[] = {
	/* [한국어] BIN_ATTR 이 만들어 낸 구조체의 주소. */
	&bin_attr_vpd,
	/* [한국어] 배열의 끝 표시. */
	NULL,
};

/* [한국어]
 * vpd_attr_is_visible - 이 장치에 vpd 파일을 만들어 줄지 정한다
 *
 * @kobj: 장치의 kobject.   @a: 대상 바이너리 속성.   @n: 그룹 안의 인덱스.
 * @return: 0 이면 파일을 만들지 않는다. 0 이 아니면 그 값이 파일 권한이 된다.
 *
 * sysfs 속성 그룹은 is_visible 콜백으로 장치마다 파일을 켜고 끌 수 있다.
 * VPD capability 가 없는 장치에까지 vpd 파일을 만들어 두면 열 때마다
 * -ENODEV 만 돌려주는 쓸모없는 파일이 되므로, 아예 만들지 않는다.
 *
 * 판정 기준은 pdev->vpd.cap 하나다. 크기까지 확인하지 않는 이유는 그
 * 확인이 실제 EEPROM 읽기를 유발하는데, 부팅 중 모든 장치에 대해
 * 그것을 하면 지나치게 느려지기 때문이다.
 *
 * 있으면 a->attr.mode 를 그대로 돌려준다 - BIN_ATTR 에 적어 둔 0600 이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 디렉터리 생성).
 *
 * 호출 체인:  sysfs 그룹 생성 → [이 함수]
 */
static umode_t vpd_attr_is_visible(struct kobject *kobj,
				   const struct bin_attribute *a, int n)
{
	/* [한국어] sysfs 가 넘긴 kobject 에서 pci_dev 로 되돌린다. */
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj));

	/* [한국어] VPD capability 가 없는 장치라면 */
	if (!pdev->vpd.cap)
		/* [한국어] 0 을 돌려 파일을 아예 만들지 않는다. 만들어 두면 열 때마다 -ENODEV 만
		 * 돌려주는 쓸모없는 파일이 된다. 크기까지 확인하지 않는 이유는 그 확인이
		 * 실제 EEPROM 읽기를 유발해 부팅이 지나치게 느려지기 때문이다. */
		return 0;

	/* [한국어] BIN_ATTR 에 적어 둔 0600 을 그대로 파일 권한으로 쓴다. */
	return a->attr.mode;
}

/* [한국어] pci-sysfs.c 가 속성 그룹 목록에 넣는 대상(pci-sysfs.c:4844).
 * 선언은 drivers/pci/pci.h:828 에 있다. 그룹의 실체와 콜백은 전부 이 파일에 있다. */
const struct attribute_group pci_dev_vpd_attr_group = {
	/* [한국어] 이 그룹이 제공하는 바이너리 속성 배열. */
	.bin_attrs = vpd_attrs,
	/* [한국어] 장치마다 파일을 켜고 끌 판정 콜백. */
	.is_bin_visible = vpd_attr_is_visible,
};

/* [한국어]
 * pci_vpd_alloc - VPD 전체를 읽어 새 힙 버퍼로 돌려준다
 *
 * @dev:  대상 PCI 장치.
 * @size: 읽은 크기를 받을 곳. 필요 없으면 NULL.
 * @return: kmalloc 한 버퍼(호출자가 kfree 해야 한다). 실패하면 ERR_PTR -
 *          -ENODEV(VPD 없음), -ENOMEM, -EIO(요청한 만큼 못 읽음).
 *
 * 아래 파싱 함수들(pci_vpd_find_id_string, pci_vpd_find_ro_info_keyword,
 * pci_vpd_check_csum)은 전부 메모리 버퍼를 받는다. VPD 를 한 바이트씩
 * 하드웨어에서 긁으며 파싱하면 EEPROM 접근이 수백 번 일어나 견딜 수 없이
 * 느리기 때문이다. 그래서 한 번에 통째로 떠 오는 이 함수가 필요하다.
 *
 * cnt != len 을 -EIO 로 다루는 것이 요점이다. 부분 성공을 허용하면
 * 파싱 함수가 잘린 버퍼를 온전한 것으로 오해해 엉뚱한 오프셋을 읽는다.
 * 전부가 아니면 실패로 본다.
 *
 * ERR_PTR 관용구를 쓰므로 호출자는 IS_ERR() 로 검사해야 한다.
 * EXPORT_SYMBOL_GPL 이라 드라이버가 쓸 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL 할당, 느린 읽기).
 *
 * 호출 체인:  드라이버 → [이 함수] → pci_vpd_available() → pci_read_vpd()
 */
void *pci_vpd_alloc(struct pci_dev *dev, unsigned int *size)
{
	/* [한국어] 읽을 전체 길이. */
	unsigned int len;
	/* [한국어] 할당할 버퍼. */
	void *buf;
	/* [한국어] 실제로 읽힌 바이트 수. */
	int cnt;

	/* [한국어] VPD 가 있는지 확인하고 크기까지 확정한다. 아래에서 vpd.len 을 바로
	 * 쓰려면 이 호출이 먼저 성공해야 한다. */
	if (!pci_vpd_available(dev, true))
		/* [한국어] ERR_PTR 관용구 — 호출자는 IS_ERR() 로 검사해야 한다. */
		return ERR_PTR(-ENODEV);

	/* [한국어] 위에서 확정된 크기. */
	len = dev->vpd.len;
	/* [한국어] 통째로 담을 버퍼를 잡는다. 파싱 함수들이 전부 메모리 버퍼를 받는 이유는,
	 * 한 바이트씩 하드웨어에서 긁으며 파싱하면 EEPROM 접근이 수백 번 일어나
	 * 견딜 수 없이 느리기 때문이다. */
	buf = kmalloc(len, GFP_KERNEL);
	/* [한국어] 할당 실패. */
	if (!buf)
		/* [한국어] 메모리 부족. */
		return ERR_PTR(-ENOMEM);

	/* [한국어] VPD 전체를 한 번에 읽는다. */
	cnt = pci_read_vpd(dev, 0, len, buf);
	/* [한국어] 요청한 만큼 다 읽히지 않았다. */
	if (cnt != len) {
		/* [한국어] 버퍼를 반납하고 */
		kfree(buf);
		/* [한국어] 실패로 돌린다. 부분 성공을 허용하면 파싱 함수가 잘린 버퍼를 온전한
		 * 것으로 오해해 엉뚱한 오프셋을 읽는다. */
		return ERR_PTR(-EIO);
	}

	/* [한국어] 호출자가 크기를 원했으면 */
	if (size)
		/* [한국어] 채워 준다. */
		*size = len;

	/* [한국어] 버퍼를 넘긴다. 호출자가 kfree 해야 한다. */
	return buf;
}
/* [한국어] 모듈에 공개(GPL 한정). */
EXPORT_SYMBOL_GPL(pci_vpd_alloc);

/* [한국어]
 * pci_vpd_find_tag - 버퍼에서 원하는 LRDT 태그의 데이터 시작 위치를 찾는다
 *
 * @buf:  pci_vpd_alloc() 등으로 떠 온 VPD 이미지.
 * @len:  버퍼 길이.
 * @rdt:  찾는 태그 바이트(PCI_VPD_LRDT_ID_STRING, PCI_VPD_LRDT_RO_DATA 등).
 * @size: 찾은 태그의 데이터 길이를 받을 곳. 필요 없으면 NULL.
 * @return: 데이터가 시작하는 오프셋. 없으면 -ENOENT.
 *
 * 태그를 차례로 건너뛰며 찾는다. 상류 주석이 밝히듯 LRDT 만 본다 -
 * SRDT 중 실제로 쓰이는 것은 끝 태그뿐이고, 그것을 만나면 최상위 비트가
 * 0 이라 while 조건이 저절로 거짓이 되어 루프가 끝난다. 별도의 끝 판정이
 * 필요 없게 조건을 짠 것이다.
 *
 * 길이 보정이 하나 있다. 태그가 선언한 길이가 버퍼 끝을 넘으면 버퍼
 * 경계까지로 줄여 돌려준다. 잘린 VPD 이미지를 받았을 때 호출자가 버퍼
 * 밖을 읽지 않게 하는 방어다.
 *
 * 루프 조건의 i + PCI_VPD_LRDT_TAG_SIZE <= len 은 헤더 3바이트를 온전히
 * 읽을 수 있을 때만 진행하겠다는 뜻이다.
 *
 * 실행 컨텍스트: 제약 없음. 메모리 버퍼만 다루므로 하드웨어를 만지지 않는다.
 *
 * 호출 체인:  pci_vpd_find_id_string() / pci_vpd_find_ro_info_keyword()
 *               → [이 함수] → pci_vpd_lrdt_size()
 */
static int pci_vpd_find_tag(const u8 *buf, unsigned int len, u8 rdt, unsigned int *size)
{
	/* [한국어] 현재 훑고 있는 오프셋. */
	int i = 0;

	/* look for LRDT tags only, end tag is the only SRDT tag */
	/* [한국어] 헤더 3바이트를 온전히 읽을 수 있고, 최상위 비트가 서 있어 LRDT 인 동안 돈다.
	 * 끝 태그는 SRDT 라 최상위 비트가 0 이고, 그래서 이 조건이 저절로 거짓이 되어
	 * 루프가 끝난다 — 별도의 끝 판정이 필요 없게 조건을 짠 것이다. */
	while (i + PCI_VPD_LRDT_TAG_SIZE <= len && buf[i] & PCI_VPD_LRDT) {
		/* [한국어] 이 태그가 선언한 데이터 길이. */
		unsigned int lrdt_len = pci_vpd_lrdt_size(buf + i);
		/* [한국어] 태그 바이트를 따로 보관한다. 아래에서 i 를 옮긴 뒤에도 비교해야 한다. */
		u8 tag = buf[i];

		/* [한국어] 헤더를 건너뛴다. 이제 i 가 데이터 시작이다. */
		i += PCI_VPD_LRDT_TAG_SIZE;
		/* [한국어] 찾던 태그인가. */
		if (tag == rdt) {
			/* [한국어] 선언된 길이가 버퍼 끝을 넘으면 */
			if (i + lrdt_len > len)
				/* [한국어] 버퍼 경계까지로 줄인다. 잘린 VPD 이미지를 받았을 때 호출자가
				 * 버퍼 밖을 읽지 않게 하는 방어다. */
				lrdt_len = len - i;
			/* [한국어] 호출자가 길이를 원했으면 */
			if (size)
				/* [한국어] 채워 준다. */
				*size = lrdt_len;
			/* [한국어] 데이터가 시작하는 오프셋을 돌려준다. */
			return i;
		}

		/* [한국어] 이 태그가 아니면 데이터만큼 더 건너뛰어 다음 태그로 간다. */
		i += lrdt_len;
	}

	/* [한국어] 끝까지 못 찾았다. */
	return -ENOENT;
}

/* [한국어]
 * pci_vpd_find_id_string - 제품 이름 문자열의 위치를 찾는다
 *
 * @buf: VPD 이미지.   @len: 버퍼 길이.   @size: 문자열 길이를 받을 곳.
 * @return: 문자열이 시작하는 오프셋. 없으면 -ENOENT.
 *
 * ID String 태그(PCI_VPD_LRDT_ID_STRING)는 VPD 의 첫 자원이며, 사람이
 * 읽을 수 있는 제품 이름을 담는다. 널 종료가 아니라 길이로 구분되므로
 * @size 를 함께 받아야 쓸 수 있다.
 *
 * pci_vpd_find_tag() 에 태그 상수만 고정해 넘기는 한 줄 래퍼다. 굳이
 * 함수로 내보내는 이유는 pci_vpd_find_tag() 가 static 이기 때문이다 -
 * 드라이버에게는 "ID 문자열 찾기" 라는 좁은 기능만 열어 준다.
 *
 * EXPORT_SYMBOL_GPL 로 공개된다.
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  드라이버 → [이 함수] → pci_vpd_find_tag()
 */
int pci_vpd_find_id_string(const u8 *buf, unsigned int len, unsigned int *size)
{
	/* [한국어] 태그 상수만 고정해 넘기는 한 줄 래퍼. pci_vpd_find_tag() 이 static 이라,
	 * 드라이버에게는 "ID 문자열 찾기" 라는 좁은 기능만 열어 준다. */
	return pci_vpd_find_tag(buf, len, PCI_VPD_LRDT_ID_STRING, size);
}
/* [한국어] 모듈에 공개(GPL 한정). */
EXPORT_SYMBOL_GPL(pci_vpd_find_id_string);

/* [한국어]
 * pci_vpd_find_info_keyword - 태그 안에서 두 글자 키워드 항목을 찾는다
 *
 * @buf: VPD 이미지.   @off: 검색을 시작할 오프셋(보통 어떤 태그의 데이터 시작).
 * @len: 그 태그의 데이터 길이.   @kw: 찾는 두 글자 키워드("PN", "SN" 등).
 * @return: 그 항목의 헤더가 시작하는 오프셋. 없으면 -ENOENT.
 *
 * LRDT 태그 안쪽은 다시 작은 항목들의 나열이다. 항목 하나는 키워드
 * 2바이트 + 길이 1바이트 + 데이터로 이루어지고, 이 함수는 그 사슬을
 * 따라가며 키워드를 비교한다.
 *
 * 건너뛰는 폭이 헤더 3바이트 + 데이터 길이인 점이 핵심이다 - 항목마다
 * 길이가 달라서 고정 폭으로 뛸 수 없다.
 *
 * 반환값이 데이터가 아니라 *헤더* 위치라는 점을 유의해야 한다. 호출자인
 * pci_vpd_find_ro_info_keyword() 가 그 위치에서 길이를 먼저 읽고, 그다음
 * 3을 더해 데이터 시작으로 옮긴다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  pci_vpd_find_ro_info_keyword() → [이 함수]
 *               → pci_vpd_info_field_size()
 */
static int pci_vpd_find_info_keyword(const u8 *buf, unsigned int off,
			      unsigned int len, const char *kw)
{
	/* [한국어] 항목을 훑을 오프셋. */
	int i;

	/* [한국어] 태그 데이터 구간 안에서, 항목 헤더 3바이트를 온전히 읽을 수 있는 동안 돈다.
	 * 증가식이 비어 있는 이유는 건너뛸 폭이 항목마다 달라 본문에서 더하기 때문이다. */
	for (i = off; i + PCI_VPD_INFO_FLD_HDR_SIZE <= off + len;) {
		/* [한국어] 키워드 첫 글자와 */
		if (buf[i + 0] == kw[0] &&
		    /* [한국어] 둘째 글자를 함께 비교한다. "PN", "SN" 같은 두 글자 키워드다. */
		    buf[i + 1] == kw[1])
			/* [한국어] 찾았다. 반환값이 데이터가 아니라 헤더 위치라는 점을 유의해야 한다 —
			 * 호출자가 거기서 길이를 먼저 읽고 나서 데이터 시작으로 옮긴다. */
			return i;

		/* [한국어] 헤더 3바이트와 */
		i += PCI_VPD_INFO_FLD_HDR_SIZE +
		     /* [한국어] 그 항목의 데이터 길이를 함께 건너뛴다. 항목마다 길이가 달라
		      * 고정 폭으로 뛸 수 없다. */
		     pci_vpd_info_field_size(&buf[i]);
	}

	/* [한국어] 끝까지 못 찾았다. */
	return -ENOENT;
}

/* [한국어]
 * __pci_read_vpd - F0 우회를 처리한 뒤 실제 읽기로 넘긴다
 *
 * @dev:        대상 PCI 장치.
 * @pos:        오프셋.   @count: 바이트 수.   @buf: 결과 버퍼.
 * @check_size: 캐시된 크기로 범위를 제한할 것인가.
 * @return: 읽은 바이트 수 또는 음수 errno.
 *
 * pci_read_vpd() 와 pci_read_vpd_any() 가 check_size 만 다르고 나머지가
 * 같아서, 공통 부분을 여기 모았다.
 *
 * 하는 일은 F0 우회 하나다. PCI_DEV_FLAGS_VPD_REF_F0 이 붙은 장치는
 * 자기 VPD 창구가 없고 function 0 의 것을 써야 하므로, 대상을 바꾸고
 * 참조를 걸었다가 끝나면 반납한다. 참조 반납을 잊지 않도록 그 경우의
 * 호출을 별도 분기로 떼어 놓았다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pci_read_vpd() / pci_read_vpd_any() → [이 함수]
 *               → pci_get_func0_dev() → pci_vpd_read()
 */
static ssize_t __pci_read_vpd(struct pci_dev *dev, loff_t pos, size_t count, void *buf,
			      bool check_size)
{
	/* [한국어] 아래 두 갈래가 공통으로 쓸 반환값. */
	ssize_t ret;

	/* [한국어] VPD EEPROM 이 function 0 에만 달린 장치인가. */
	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0) {
		/* [한국어] 대상을 function 0 으로 바꾼다. 참조가 걸린 채 돌아온다.
		 * 인자 dev 를 덮어쓰는 것이 의도적이다 — 아래 호출이 새 대상을 쓴다. */
		dev = pci_get_func0_dev(dev);
		/* [한국어] function 0 이 없다. */
		if (!dev)
			/* [한국어] 장치 없음. */
			return -ENODEV;

		/* [한국어] 바뀐 대상으로 실제 읽기를 한다. */
		ret = pci_vpd_read(dev, pos, count, buf, check_size);
		/* [한국어] 참조를 반납한다. 이 분기를 따로 떼어 놓은 이유가 이 반납을 빠뜨리지
		 * 않기 위해서다. */
		pci_dev_put(dev);
		/* [한국어] 결과를 그대로 전한다. */
		return ret;
	}

	/* [한국어] 우회가 필요 없는 평범한 경우 — 자기 자신으로 읽는다. */
	return pci_vpd_read(dev, pos, count, buf, check_size);
}

/**
 * pci_read_vpd - Read one entry from Vital Product Data
 * @dev:	PCI device struct
 * @pos:	offset in VPD space
 * @count:	number of bytes to read
 * @buf:	pointer to where to store result
 */
/* [한국어]
 * pci_read_vpd - VPD 를 읽는 공개 진입점(크기 검사 있음)
 *
 * @dev: 대상 PCI 장치.   @pos: 오프셋.   @count: 바이트 수.   @buf: 결과 버퍼.
 * @return: 읽은 바이트 수 또는 음수 errno.
 *
 * check_size=true 로 __pci_read_vpd() 를 부른다. 즉 파싱해서 알아낸
 * 유효 크기 밖은 읽지 않는다. 형식을 지키는 장치에서 EEPROM 의 정의되지
 * 않은 영역을 건드리지 않으려는 것이다.
 *
 * EXPORT_SYMBOL(GPL 제한 없음)로 공개된다. 이 트리에서 확인한 커널 안
 * 호출자는 vpd_read()(같은 파일)와 drivers/vfio/pci/vfio_pci_config.c:828,
 * 그리고 pci_vpd_alloc()(같은 파일)이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  vpd_read() / VFIO / 드라이버 → [이 함수] → __pci_read_vpd()
 */
ssize_t pci_read_vpd(struct pci_dev *dev, loff_t pos, size_t count, void *buf)
{
	/* [한국어] check_size=true — 파싱해서 알아낸 유효 크기 밖은 읽지 않는다.
	 * 형식을 지키는 장치에서 EEPROM 의 정의되지 않은 영역을 건드리지 않으려는 것이다. */
	return __pci_read_vpd(dev, pos, count, buf, true);
}
/* [한국어] 모듈에 공개(GPL 한정 없음). */
EXPORT_SYMBOL(pci_read_vpd);

/* Same, but allow to access any address */
/* [한국어]
 * pci_read_vpd_any - 크기 검사 없이 VPD 의 아무 주소나 읽는다
 *
 * @dev: 대상 PCI 장치.   @pos: 오프셋.   @count: 바이트 수.   @buf: 결과 버퍼.
 * @return: 읽은 바이트 수 또는 음수 errno.
 *
 * pci_read_vpd() 와 유일하게 다른 점이 check_size=false 다. 상한이
 * 캐시된 len 이 아니라 PCI_VPD_MAX_SIZE 가 된다.
 *
 * 두 가지 쓰임이 있다. 하나는 pci_vpd_size() 자신이다 - 크기를 알아내려면
 * 크기를 모르는 상태에서 읽어야 하는 닭과 달걀 문제를 이 판이 푼다.
 * 다른 하나는 Chelsio 처럼 표준 위치 밖에 진짜 VPD 를 두는 장치인데,
 * 그쪽은 quirk_chelsio_extend_vpd() 가 len 자체를 키우는 방식으로 해결한다.
 *
 * EXPORT_SYMBOL 로 공개된다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pci_vpd_size() / 드라이버 → [이 함수] → __pci_read_vpd()
 */
ssize_t pci_read_vpd_any(struct pci_dev *dev, loff_t pos, size_t count, void *buf)
{
	/* [한국어] check_size=false — 상한이 캐시된 len 이 아니라 PCI_VPD_MAX_SIZE 가 된다.
	 * pci_vpd_size() 가 크기를 알아내려면 크기를 모르는 상태에서 읽어야 하는데,
	 * 이 판이 그 닭과 달걀 문제를 푼다. */
	return __pci_read_vpd(dev, pos, count, buf, false);
}
/* [한국어] 모듈에 공개. */
EXPORT_SYMBOL(pci_read_vpd_any);

/* [한국어]
 * __pci_write_vpd - F0 우회를 처리한 뒤 실제 쓰기로 넘긴다
 *
 * @dev:        대상 PCI 장치.
 * @pos:        오프셋.   @count: 바이트 수.   @buf: 쓸 데이터.
 * @check_size: 캐시된 크기로 범위를 제한할 것인가.
 * @return: 쓴 바이트 수 또는 음수 errno.
 *
 * __pci_read_vpd() 의 쓰기 판이며 구조가 완전히 같다. F0 공유 quirk 가
 * 걸린 장치는 function 0 으로 대상을 바꾸고, 참조를 걸었다가 반납한다.
 *
 * 두 함수를 하나로 합치지 않은 이유는 buf 의 const 여부와 아래에서 부르는
 * 함수가 다르기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  pci_write_vpd() / pci_write_vpd_any() → [이 함수]
 *               → pci_get_func0_dev() → pci_vpd_write()
 */
static ssize_t __pci_write_vpd(struct pci_dev *dev, loff_t pos, size_t count,
			       const void *buf, bool check_size)
{
	/* [한국어] 아래 두 갈래가 공통으로 쓸 반환값. */
	ssize_t ret;

	/* [한국어] F0 공유 quirk 가 걸린 장치인가. */
	if (dev->dev_flags & PCI_DEV_FLAGS_VPD_REF_F0) {
		/* [한국어] 대상을 function 0 으로 바꾼다. */
		dev = pci_get_func0_dev(dev);
		/* [한국어] function 0 이 없다. */
		if (!dev)
			/* [한국어] 장치 없음. */
			return -ENODEV;

		/* [한국어] 바뀐 대상으로 실제 쓰기. */
		ret = pci_vpd_write(dev, pos, count, buf, check_size);
		/* [한국어] 참조 반납. */
		pci_dev_put(dev);
		/* [한국어] 결과 전달. */
		return ret;
	}

	/* [한국어] 평범한 경우. */
	return pci_vpd_write(dev, pos, count, buf, check_size);
}

/**
 * pci_write_vpd - Write entry to Vital Product Data
 * @dev:	PCI device struct
 * @pos:	offset in VPD space
 * @count:	number of bytes to write
 * @buf:	buffer containing write data
 */
/* [한국어]
 * pci_write_vpd - VPD 에 쓰는 공개 진입점(크기 검사 있음)
 *
 * @dev: 대상 PCI 장치.   @pos: 오프셋.   @count: 바이트 수.   @buf: 쓸 데이터.
 * @return: 쓴 바이트 수 또는 음수 errno.
 *
 * check_size=true 로 __pci_write_vpd() 를 부른다. 유효 크기를 넘는 요청은
 * 잘리지 않고 -EINVAL 로 거절된다.
 *
 * pos 와 count 가 모두 4의 배수여야 한다는 제약은 아래 pci_vpd_write() 가
 * 강제한다. 하드웨어가 4바이트 단위로만 쓸 수 있고, 부분 갱신을 위한
 * 읽고-고쳐-쓰기를 커널이 임의로 하지 않기 때문이다.
 *
 * EXPORT_SYMBOL 로 공개된다. 이 트리에서 확인한 커널 안 호출자는
 * vpd_write()(같은 파일) 하나다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  vpd_write() / 드라이버 → [이 함수] → __pci_write_vpd()
 */
ssize_t pci_write_vpd(struct pci_dev *dev, loff_t pos, size_t count, const void *buf)
{
	/* [한국어] check_size=true — 유효 크기를 넘는 요청은 잘리지 않고 -EINVAL 로 거절된다. */
	return __pci_write_vpd(dev, pos, count, buf, true);
}
/* [한국어] 모듈에 공개. */
EXPORT_SYMBOL(pci_write_vpd);

/* Same, but allow to access any address */
/* [한국어]
 * pci_write_vpd_any - 크기 검사 없이 VPD 의 아무 주소에나 쓴다
 *
 * @dev: 대상 PCI 장치.   @pos: 오프셋.   @count: 바이트 수.   @buf: 쓸 데이터.
 * @return: 쓴 바이트 수 또는 음수 errno.
 *
 * pci_write_vpd() 의 검사 없는 판. 상한이 PCI_VPD_MAX_SIZE 로 넓어진다.
 *
 * EXPORT_SYMBOL 로 공개하지만 이 트리 안에는 호출자가 하나도 없다
 * (전수 grep 확인). 읽기 쪽 대칭을 맞추려고 내보낸 API 로 보인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  (트리 밖 드라이버) → [이 함수] → __pci_write_vpd()
 */
ssize_t pci_write_vpd_any(struct pci_dev *dev, loff_t pos, size_t count, const void *buf)
{
	/* [한국어] check_size=false — 상한이 PCI_VPD_MAX_SIZE 로 넓어진다.
	 * 다만 이 트리 안에는 이 함수의 호출자가 하나도 없다(전수 grep 확인).
	 * 읽기 쪽 대칭을 맞추려고 내보낸 API 로 보인다. */
	return __pci_write_vpd(dev, pos, count, buf, false);
}
/* [한국어] 모듈에 공개. */
EXPORT_SYMBOL(pci_write_vpd_any);

/* [한국어]
 * pci_vpd_find_ro_info_keyword - 읽기 전용 영역에서 키워드의 데이터 위치를 찾는다
 *
 * @buf:  VPD 이미지.   @len: 버퍼 길이.
 * @kw:   찾는 두 글자 키워드("PN" 부품번호, "SN" 일련번호, "RV" 체크섬 등).
 * @size: 그 데이터의 길이를 받을 곳. 필요 없으면 NULL.
 * @return: 데이터가 시작하는 오프셋. -ENOENT 없음, -EINVAL 길이가 버퍼를 넘침.
 *
 * 드라이버가 실제로 쓰는 조회 함수다. 두 단계 검색을 하나로 묶는다.
 *   1) pci_vpd_find_tag() 으로 읽기 전용 태그(PCI_VPD_LRDT_RO_DATA)를 찾는다.
 *      쓰기 가능 영역이 아니라 이쪽만 보는 이유는 부품번호나 일련번호 같은
 *      제조 정보가 전부 여기 들어 있기 때문이다.
 *   2) 그 안에서 pci_vpd_find_info_keyword() 로 키워드 항목을 찾는다.
 *
 * 그다음 헤더 위치를 데이터 위치로 바꾼다. 먼저 헤더에서 길이를 읽고
 * (읽는 시점이 중요하다 - 오프셋을 옮기기 전이어야 한다), 그 뒤에
 * PCI_VPD_INFO_FLD_HDR_SIZE 만큼 더해 데이터 시작으로 옮긴다.
 *
 * 마지막 경계 검사는 선언된 길이가 버퍼 밖으로 뻗는 경우를 잡는다.
 * 여기서 걸러 주지 않으면 호출자가 버퍼 밖을 읽는다.
 *
 * EXPORT_SYMBOL_GPL 로 공개된다.
 * 실행 컨텍스트: 제약 없음. 메모리 버퍼만 다룬다.
 *
 * 호출 체인:  드라이버 / pci_vpd_check_csum() → [이 함수]
 *               → pci_vpd_find_tag() → pci_vpd_find_info_keyword()
 */
int pci_vpd_find_ro_info_keyword(const void *buf, unsigned int len,
				 const char *kw, unsigned int *size)
{
	/* [한국어] ro_start 는 읽기 전용 태그의 데이터 시작, infokw_start 는 그 안 항목 위치. */
	int ro_start, infokw_start;
	/* [한국어] ro_len 은 태그 데이터 길이, infokw_size 는 찾은 항목의 데이터 길이. */
	unsigned int ro_len, infokw_size;

	/* [한국어] 1단계 — 읽기 전용 태그를 찾는다. 쓰기 가능 영역을 보지 않는 이유는
	 * 부품번호나 일련번호 같은 제조 정보가 전부 이쪽에 있기 때문이다. */
	ro_start = pci_vpd_find_tag(buf, len, PCI_VPD_LRDT_RO_DATA, &ro_len);
	/* [한국어] 읽기 전용 태그가 없다. */
	if (ro_start < 0)
		/* [한국어] 그 errno(-ENOENT)를 그대로 전한다. */
		return ro_start;

	/* [한국어] 2단계 — 그 태그 안에서 두 글자 키워드 항목을 찾는다. */
	infokw_start = pci_vpd_find_info_keyword(buf, ro_start, ro_len, kw);
	/* [한국어] 키워드가 없다. */
	if (infokw_start < 0)
		/* [한국어] errno 를 그대로 전한다. 호출자인 pci_vpd_check_csum() 이 -ENOENT 를
		 * 특별히 구분해 다룬다. */
		return infokw_start;

	/* [한국어] 헤더에서 데이터 길이를 먼저 읽는다. 읽는 시점이 중요하다 —
	 * 아래에서 오프셋을 옮기기 전이어야 한다. */
	infokw_size = pci_vpd_info_field_size(buf + infokw_start);
	/* [한국어] 이제 헤더를 건너뛰어 데이터 시작으로 옮긴다. */
	infokw_start += PCI_VPD_INFO_FLD_HDR_SIZE;

	/* [한국어] 선언된 길이가 버퍼 밖으로 뻗으면 */
	if (infokw_start + infokw_size > len)
		/* [한국어] 거절한다. 여기서 걸러 주지 않으면 호출자가 버퍼 밖을 읽는다. */
		return -EINVAL;

	/* [한국어] 호출자가 길이를 원했으면 */
	if (size)
		/* [한국어] 채워 준다. */
		*size = infokw_size;

	/* [한국어] 데이터가 시작하는 오프셋. */
	return infokw_start;
}
/* [한국어] 모듈에 공개(GPL 한정). */
EXPORT_SYMBOL_GPL(pci_vpd_find_ro_info_keyword);

/* [한국어]
 * pci_vpd_check_csum - 읽기 전용 영역의 체크섬을 검증한다
 *
 * @buf: VPD 이미지.   @len: 버퍼 길이.
 * @return: 0 이면 체크섬이 맞다. 1 은 체크섬 항목이 아예 없다는 뜻이고,
 *          -EILSEQ 는 맞지 않는다는 뜻, -EINVAL 은 길이가 0 인 항목,
 *          그 밖의 음수는 조회 실패다.
 *
 * VPD 는 "RV" 키워드에 체크섬을 담는다. 규칙은 VPD 시작부터 그 체크섬
 * 바이트까지의 모든 바이트를 8비트로 더했을 때 0 이 되어야 한다는 것이다.
 * 그래서 루프가 rv_start 에서 시작해 0 까지 내려가며 더한다 - 앞에서부터
 * 더하나 뒤에서부터 더하나 결과가 같으므로 인덱스 하나로 끝낸다.
 *
 * 반환값 1 의 의미가 특별하다. 0 도 음수도 아닌 값을 따로 둔 이유는
 * "체크섬이 없는 것" 과 "체크섬이 틀린 것" 이 전혀 다른 상황이기 때문이다.
 * 전자는 정상일 수 있고(체크섬은 선택 항목이다), 후자는 데이터가 깨졌다는
 * 뜻이다. 그래서 -ENOENT 만 골라 1 로 바꿔 돌려준다.
 *
 * size 가 0 인 항목을 -EINVAL 로 거절하는 것은 그런 항목에는 더할 체크섬
 * 바이트 자체가 없어 검증이 성립하지 않기 때문이다.
 *
 * 부호 없는 u8 로 더하므로 오버플로가 자연히 256 으로 나눈 나머지가 되어
 * 별도 마스킹이 필요 없다.
 *
 * EXPORT_SYMBOL_GPL 로 공개된다.
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  드라이버 → [이 함수] → pci_vpd_find_ro_info_keyword()
 */
int pci_vpd_check_csum(const void *buf, unsigned int len)
{
	/* [한국어] 바이트 단위로 더하려고 u8 포인터로 받아 둔다. */
	const u8 *vpd = buf;
	/* [한국어] 체크섬 항목의 길이. 아래에서 0 인지만 본다. */
	unsigned int size;
	/* [한국어] 누적 합. u8 이라 오버플로가 자연히 256 으로 나눈 나머지가 되어
	 * 별도 마스킹이 필요 없다. */
	u8 csum = 0;
	/* [한국어] 체크섬 바이트의 오프셋. */
	int rv_start;

	/* [한국어] "RV" 키워드를 찾는다. 이 항목이 체크섬을 담는다. */
	rv_start = pci_vpd_find_ro_info_keyword(buf, len, PCI_VPD_RO_KEYWORD_CHKSUM, &size);
	/* [한국어] 체크섬 항목이 아예 없다 — 체크섬은 선택 항목이라 정상일 수 있다. */
	if (rv_start == -ENOENT) /* no checksum in VPD */
		/* [한국어] 그래서 0 도 음수도 아닌 1 을 돌려준다. "없음" 과 "틀림" 은 전혀 다른
		 * 상황이라 호출자가 구분할 수 있어야 하기 때문이다. */
		return 1;
	/* [한국어] 그 밖의 조회 실패는 */
	else if (rv_start < 0)
		/* [한국어] errno 를 그대로 전한다. */
		return rv_start;

	/* [한국어] 길이가 0 인 항목이면 더할 체크섬 바이트 자체가 없어 검증이 성립하지 않는다. */
	if (!size)
		/* [한국어] 인자 오류로 거절. */
		return -EINVAL;

	/* [한국어] VPD 시작(오프셋 0)부터 체크섬 바이트까지를 모두 더한다. 뒤에서 앞으로
	 * 내려가며 더하는 이유는 인덱스 하나로 루프를 끝내기 위해서다 —
	 * 덧셈은 순서에 무관하므로 결과가 같다. */
	while (rv_start >= 0)
		/* [한국어] 한 바이트씩 더하며 인덱스를 내린다. */
		csum += vpd[rv_start--];

	/* [한국어] 규칙은 총합이 0 이어야 한다는 것이다. 0 이 아니면 데이터가 깨진 것이므로
	 * -EILSEQ(잘못된 바이트열)로 알린다. */
	return csum ? -EILSEQ : 0;
}
/* [한국어] 모듈에 공개(GPL 한정). */
EXPORT_SYMBOL_GPL(pci_vpd_check_csum);

/* [한국어] 아래 quirk 들은 CONFIG_PCI_QUIRKS 가 켜졌을 때만 컴파일된다.
 * DECLARE_PCI_FIXUP_* 매크로가 링크 타임에 fixup 섹션으로 항목을 모으며,
 * 이 등록은 quirks.c 가 아니라 이 파일에 있다(quirks.c 에는 VPD quirk 가 없다). */
#ifdef CONFIG_PCI_QUIRKS
/*
 * Quirk non-zero PCI functions to route VPD access through function 0 for
 * devices that share VPD resources between functions.  The functions are
 * expected to be identical devices.
 */
/* [한국어]
 * quirk_f0_vpd_link - 다기능 장치의 VPD 접근을 function 0 으로 몰아 준다
 *
 * @dev: quirk 대상 장치.   @return: 없음.
 *
 * VPD EEPROM 이 물리적으로 하나뿐이고 function 0 에만 연결된 장치가 있다.
 * 그런 장치의 function 1 이상에서 VPD 를 읽으면 쓰레기가 나오거나 아예
 * 응답하지 않는다. 이 quirk 가 그런 function 에 표시를 달아, 이후 모든
 * VPD 접근이 function 0 으로 우회되게 한다.
 *
 * 판정이 신중하다. function 0 자신은 대상이 아니고(우회할 곳이 자기 자신이다),
 * function 0 에 실제로 VPD capability 가 있어야 하며, class/vendor/device 가
 * 모두 같아야 한다. 상류 주석대로 "동일한 장치일 것" 이 전제이기 때문이다.
 * 하나라도 다르면 서로 다른 기능이 우연히 같은 슬롯에 있는 것이므로
 * VPD 를 공유한다고 볼 수 없다.
 *
 * pci_get_func0_dev() 이 건 참조는 어느 경로로든 pci_dev_put() 으로 반납한다.
 *
 * 등록은 DECLARE_PCI_FIXUP_CLASS_EARLY 로 하며, 대상은 Intel 의 이더넷
 * 클래스 장치 전체다(PCI_CLASS_NETWORK_ETHERNET). EARLY 단계라
 * pci_vpd_init() 보다 먼저 실행되지는 않지만, 실제 VPD 접근보다는 앞선다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치 열거 중 fixup).
 *
 * 호출 체인:  pci_fixup_device(early) → [이 함수] → pci_get_func0_dev()
 */
static void quirk_f0_vpd_link(struct pci_dev *dev)
{
	/* [한국어] function 0 의 pci_dev 를 받을 곳. */
	struct pci_dev *f0;

	/* [한국어] 기능 번호가 0 이면 자기 자신이 function 0 이라 우회할 곳이 없다. */
	if (!PCI_FUNC(dev->devfn))
		/* [한국어] 할 일 없음. */
		return;

	/* [한국어] 같은 슬롯의 function 0 을 찾는다(참조가 걸린다). */
	f0 = pci_get_func0_dev(dev);
	/* [한국어] 없으면 우회 대상이 없다. */
	if (!f0)
		/* [한국어] 할 일 없음. */
		return;

	/* [한국어] function 0 에 실제로 VPD 가 있고, 클래스와 */
	if (f0->vpd.cap && dev->class == f0->class &&
	    /* [한국어] 벤더/디바이스 ID 까지 모두 같아야 한다. 상류 주석대로 "동일한 장치일 것"
	     * 이 전제이기 때문이다. 하나라도 다르면 서로 다른 기능이 우연히 같은 슬롯에
	     * 있는 것이라 VPD 를 공유한다고 볼 수 없다. */
	    dev->vendor == f0->vendor && dev->device == f0->device)
		/* [한국어] 조건을 다 만족하면 우회 표시를 단다. 이후 모든 VPD 접근이 function 0 으로 간다. */
		dev->dev_flags |= PCI_DEV_FLAGS_VPD_REF_F0;

	/* [한국어] 어느 경로로 왔든 참조를 반납한다. */
	pci_dev_put(f0);
}
/* [한국어] Intel 의 이더넷 클래스 장치 전체에 이 quirk 를 건다. 인자 8 은 클래스
 * 비교에 쓸 비트 수(상위 8비트만 본다는 뜻)다. EARLY 단계에 등록된다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, PCI_ANY_ID,
			      PCI_CLASS_NETWORK_ETHERNET, 8, quirk_f0_vpd_link);

/*
 * If a device follows the VPD format spec, the PCI core will not read or
 * write past the VPD End Tag.  But some vendors do not follow the VPD
 * format spec, so we can't tell how much data is safe to access.  Devices
 * may behave unpredictably if we access too much.  Blacklist these devices
 * so we don't touch VPD at all.
 */
/* [한국어]
 * quirk_blacklist_vpd - VPD 접근이 위험한 장치에서 아예 접근을 막는다
 *
 * @dev: quirk 대상 장치.   @return: 없음.
 *
 * vpd.len 에 PCI_VPD_SZ_INVALID 를 못박는다. 그러면 pci_vpd_init() 이
 * 맨 앞 검사에서 걸려 capability 오프셋조차 채우지 않고 돌아가므로,
 * 이후 pci_vpd_available() 이 항상 false 를 준다. 접근 경로가 완전히 닫힌다.
 *
 * 이유가 상류 주석에 있다. VPD 형식 규격을 따르는 장치라면 커널이 끝
 * 태그를 넘어 읽지 않지만, 규격을 지키지 않는 벤더가 있어 어디까지가
 * 안전한지 알 수 없다. 너무 많이 읽으면 장치가 예측 불가능하게 동작하므로
 * 아예 손대지 않는 편을 택한 것이다.
 *
 * FW_BUG 접두어를 붙여 경고한다 - 커널 버그가 아니라 장치 펌웨어 문제임을
 * 로그 분류상 명시하는 관례다.
 *
 * 대상 목록은 아래 DECLARE_PCI_FIXUP_HEADER 들이며 LSI Logic 의 여러
 * 모델과 Attansic 전체, 그리고 Amazon Annapurna Labs 의 0x0031 이다.
 * 마지막 것만 클래스까지 함께 보는데, 상류 주석대로 같은 device id 가
 * Root Port 아닌 다른 종류에도 재사용되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(헤더 fixup).
 *
 * 호출 체인:  pci_fixup_device(header) → [이 함수]
 */
static void quirk_blacklist_vpd(struct pci_dev *dev)
{
	/* [한국어] "이 장치의 VPD 는 쓸 수 없다" 고 못박는다. 그러면 pci_vpd_init() 이 맨 앞
	 * 검사에서 걸려 capability 오프셋조차 채우지 않아 접근 경로가 완전히 닫힌다. */
	dev->vpd.len = PCI_VPD_SZ_INVALID;
	/* [한국어] FW_BUG 접두어는 커널 버그가 아니라 장치 펌웨어 문제임을 로그 분류상
	 * 명시하는 관례다. */
	pci_warn(dev, FW_BUG "disabling VPD access (can't determine size of non-standard VPD format)\n");
}
/* [한국어] 아래는 대상 목록이다. LSI Logic 의 여러 모델과 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0060, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x007c, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0413, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0078, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0079, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0073, quirk_blacklist_vpd);
/* [한국어] LSI Logic 목록은 여기까지 이어진다. 모두 같은 quirk 함수를 걸며,
 * 한 벤더의 여러 SAS/RAID 컨트롤러 모델이 같은 비표준 VPD 형식을 쓰기 때문이다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x0071, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x005b, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x002f, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x005d, quirk_blacklist_vpd);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_LSI_LOGIC, 0x005f, quirk_blacklist_vpd);
/* [한국어] Attansic 전체(PCI_ANY_ID)가 대상이다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ATTANSIC, PCI_ANY_ID, quirk_blacklist_vpd);
/*
 * The Amazon Annapurna Labs 0x0031 device id is reused for other non Root Port
 * device types, so the quirk is registered for the PCI_CLASS_BRIDGE_PCI class.
 */
/* [한국어] Amazon Annapurna Labs 0x0031 만 클래스까지 함께 본다. 상류 주석대로
 * 같은 device id 가 Root Port 아닌 다른 종류에도 재사용되기 때문이다. */
DECLARE_PCI_FIXUP_CLASS_HEADER(PCI_VENDOR_ID_AMAZON_ANNAPURNA_LABS, 0x0031,
			       PCI_CLASS_BRIDGE_PCI, 8, quirk_blacklist_vpd);

/* [한국어]
 * quirk_chelsio_extend_vpd - Chelsio 어댑터의 VPD 크기를 강제로 넓힌다
 *
 * @dev: quirk 대상 장치.   @return: 없음.
 *
 * 앞의 blacklist quirk 와 정반대 방향이다. 접근을 막는 대신 넓힌다.
 *
 * 사정이 상류 주석에 자세하다. Chelsio 어댑터는 오프셋 0 의 표준 VPD
 * 구조와 별개로, 진짜 쓰고 싶은 값들을 다른 곳에 둔다 - T3 계열은 0xc00 의
 * 1KB 영역, T4 이후는 0x400 이다. 그런데 커널은 오프셋 0 을 파싱해 크기를
 * 정하므로, 그 계산된 한계 밖에 있는 이 영역들에 접근하면 조용히 실패한다.
 * 그래서 len 을 직접 키워 그 영역까지 읽을 수 있게 한다.
 *
 * 장치 ID 에서 세 정보를 비트로 뽑아 판정한다.
 *   chip = 상위 니블(0xf000 >> 12)  - 칩 세대
 *   func = 그다음 니블(0x0f00 >> 8) - 기능 번호
 *   prod = 하위 바이트(0x00ff)      - 제품 번호
 * chip==0 이고 prod>=0x20 이면 T3 계열로 보아 8192, chip>=4 이고 func<8 이면
 * T4 이후의 물리 기능으로 보아 2048 로 정한다. func<8 조건이 붙은 이유는
 * 상류 주석대로 SR-IOV 가상 기능에는 VPD capability 가 없기 때문이다.
 *
 * 두 조건에 모두 걸리지 않으면 아무것도 하지 않아 기본 계산을 그대로 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(헤더 fixup).
 *
 * 호출 체인:  pci_fixup_device(header) → [이 함수]
 */
static void quirk_chelsio_extend_vpd(struct pci_dev *dev)
{
	/* [한국어] 장치 ID 의 상위 니블 — 칩 세대. */
	int chip = (dev->device & 0xf000) >> 12;
	/* [한국어] 그다음 니블 — 기능 번호. */
	int func = (dev->device & 0x0f00) >>  8;
	/* [한국어] 하위 바이트 — 제품 번호. >> 0 은 의미가 없지만 위 두 줄과 모양을 맞춘 표기다. */
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
	/* [한국어] chip==0 이고 prod>=0x20 이면 T3 계열. 진짜 VPD 가 0xc00 의 1KB 영역에 있다. */
	if (chip == 0x0 && prod >= 0x20)
		/* [한국어] 거기까지 닿도록 크기를 8192 로 키운다. */
		dev->vpd.len = 8192;
	/* [한국어] chip>=4 이고 func<8 이면 T4 이후의 물리 기능. 진짜 VPD 가 0x400 에 있다.
	 * func<8 조건은 상류 주석대로 SR-IOV 가상 기능에 VPD capability 가 없어서다. */
	else if (chip >= 0x4 && func < 0x8)
		/* [한국어] 2048 로 키운다. 두 조건에 모두 걸리지 않으면 아무것도 하지 않아
		 * 기본 계산을 그대로 쓴다. */
		dev->vpd.len = 2048;
}

/* [한국어] Chelsio 의 모든 장치에 이 quirk 를 건다. 실제 적용 여부는 함수 안에서
 * 장치 ID 비트로 다시 걸러 낸다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_CHELSIO, PCI_ANY_ID,
				 quirk_chelsio_extend_vpd);

/* [한국어] CONFIG_PCI_QUIRKS 블록 끝. */
#endif
