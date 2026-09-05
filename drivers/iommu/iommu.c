// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2007-2008 Advanced Micro Devices, Inc.
 * Author: Joerg Roedel <jroedel@suse.de>
 */

/*
 * [한국어 설명] IOMMU 서브시스템 코어 (drivers/iommu/iommu.c)
 *
 * === 파일의 역할 ===
 * 장치가 내는 DMA 주소를 실제 물리 주소로 번역하는 하드웨어(IOMMU)를 커널이
 * 다루는 공통 계층이다. 벤더별 IOMMU(Intel VT-d, AMD-Vi, ARM SMMU 등)는 각자
 * 레지스터도 페이지 테이블 형식도 다르지만, 이 파일이 정의한 세 가지 개념
 * 위에서 모두 같은 모양으로 보인다 -- 장치(device), 그룹(group), 도메인(domain).
 *
 * 그룹은 "하드웨어가 서로 격리해 낼 수 있는 가장 작은 장치 묶음"이다. PCIe
 * 스위치 뒤에 붙어 요청자 ID 를 공유하는 장치들이나, ACS 를 지원하지 않아
 * 서로의 트래픽을 가로챌 수 있는 장치들은 한 그룹이 된다. 이것이 소유권의
 * 단위인 이유가 여기 있다 -- 같은 그룹의 장치 하나만 가상 머신에 넘기고
 * 나머지를 호스트에 두는 것은 격리가 되지 않으므로 허용되지 않는다.
 *
 * 도메인은 주소 공간, 즉 페이지 테이블 하나다. 한 그룹의 모든 장치는 같은
 * 도메인을 본다. 그룹마다 default_domain 이 있어 아무도 따로 요구하지 않으면
 * 거기에 붙어 있고, VFIO 나 iommufd 가 장치를 가져가면 자기 도메인으로
 * 갈아 끼운다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 위쪽으로는 두 종류의 사용자가 있다. 하나는 평범한 장치 드라이버로,
 * dma_map_*() 를 부르면 dma-iommu.c 가 이 파일의 map/unmap 을 호출해 IOVA 를
 * 만든다. 다른 하나는 사용자 공간에 장치를 통째로 넘기는 경로(VFIO,
 * iommufd)로, 그쪽은 도메인을 직접 만들어 붙인다.
 *
 * 아래쪽으로는 벤더 드라이버가 struct iommu_ops 를 등록해 붙는다. 이 파일은
 * 그 vtable 만 알고 하드웨어를 직접 건드리지 않는다.
 *
 *   장치 드라이버 --dma_map--> dma-iommu.c --+
 *   VFIO / iommufd ----------------------> [이 파일] --ops--> intel/ amd/ arm/
 *
 * 실행 컨텍스트: 커널 모듈. 대부분 프로세스 문맥이며 잠들 수 있다. 다만
 * map/unmap 은 DMA API 경로에서 인터럽트 문맥으로도 불릴 수 있어 잠들지
 * 않는다.
 *
 * === 타 모듈과의 연결 ===
 * - 버스 계층: 장치가 나타나고 사라질 때마다 통지를 받아(iommu_bus_notifier)
 *   그 장치를 어느 그룹에 넣을지 정하고 IOMMU 에 등록한다. PCI, platform,
 *   AMBA, fsl-mc, CDX 등 여러 버스가 같은 통로를 쓴다.
 * - dma-iommu.c: DMA API 구현. IOVA 할당(iova.c)과 이 파일의 map/unmap 을
 *   묶어 dma_addr_t 를 만들어 낸다.
 * - iommufd / VFIO: 사용자 공간에 장치를 넘기는 경로. 소유권(owner_cnt,
 *   owner)이 이 파일에서 관리되고, 그것이 "이 그룹은 지금 누구 것인가"를
 *   정한다.
 * - 벤더 드라이버: struct iommu_ops 를 통해서만 불린다. 그 vtable 이
 *   include/linux/iommu.h 에 정의돼 있고, 이 파일이 유일한 호출자다.
 *
 * 데이터 흐름: 장치 발견 -> 그룹 배정 -> 기본 도메인 붙이기 -> (DMA API 이면)
 * IOVA 할당과 매핑, (VFIO 이면) 소유권 이전과 도메인 교체.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct iommu_group: 격리 단위. 소속 장치 목록, 현재 도메인, 소유권,
 *   PASID 배열을 들고 있다. 이 파일의 거의 모든 함수가 이것을 중심으로 돈다.
 * - struct group_device: 그룹에 속한 장치 하나. 그룹과 장치는 일대다다.
 * - iommu_probe_device(): 새 장치를 IOMMU 아래로 들인다. 그룹을 찾거나
 *   만들고, 기본 도메인을 정해 붙인다.
 * - iommu_release_device(): 그 반대. 장치가 사라질 때 그룹에서 뺀다.
 * - __iommu_group_set_domain(): 그룹 전체를 다른 도메인으로 옮긴다. 도메인
 *   교체의 유일한 통로이며, 중간에 실패했을 때 되돌리는 규약도 여기 있다.
 * - iommu_map() / iommu_unmap(): 도메인의 페이지 테이블에 매핑을 넣고 뺀다.
 * - iommu_group_claim_dma_owner(): 그룹 소유권을 가져간다. VFIO 가 장치를
 *   사용자 공간에 넘기기 전에 부르는 문이다.
 */

/* [한국어] 이 파일의 모든 pr_ 로그 앞에 "iommu: " 를 붙인다 -- 어느 계층이 낸
 * 메시지인지 로그만 보고 구별하기 위해서다. */
#define pr_fmt(fmt)    "iommu: " fmt

#include <linux/amba/bus.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/bits.h>
#include <linux/bug.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/host1x_context_bus.h>
#include <linux/iommu.h>
#include <linux/iommufd.h>
#include <linux/idr.h>
#include <linux/err.h>
#include <linux/pci.h>
#include <linux/pci-ats.h>
#include <linux/bitops.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/fsl/mc.h>
#include <linux/module.h>
#include <linux/cc_platform.h>
#include <linux/cdx/cdx_bus.h>
#include <trace/events/iommu.h>
#include <linux/sched/mm.h>
#include <linux/msi.h>
#include <uapi/linux/iommufd.h>
#include <linux/generic_pt/iommu.h>

#include "dma-iommu.h"
#include "iommu-priv.h"

/* [한국어] /sys/kernel/iommu_groups 아래에 그룹들이 매달리는 sysfs 집합.
 * 사용자 공간이 "어느 장치가 같은 그룹인가"를 확인하는 유일한 통로이며,
 * VFIO 를 쓰려면 반드시 봐야 하는 정보다. */
static struct kset *iommu_group_kset;
/* [한국어] 그룹 번호 할당기. 번호가 곧 sysfs 디렉토리 이름이 된다.
 * IDA 라 해제된 번호가 재사용된다 -- 그룹은 장치가 붙고 떨어지며 계속
 * 생겼다 사라지므로 단조 증가 카운터로는 번호가 금방 커진다. */
static DEFINE_IDA(iommu_group_ida);
/* [한국어] 전역 PASID 할당기. PASID 는 한 장치 안에서 여러 주소 공간을
 * 구별하는 번호이고, 일부 구현은 그 번호 공간을 시스템 전체가 공유한다.
 * 그런 경우 여기서 할당해야 두 장치가 같은 번호를 쓰지 않는다. */
static DEFINE_IDA(iommu_global_pasid_ida);

/* [한국어] 새 그룹의 기본 도메인 종류. 0 이면 각 IOMMU 드라이버가 정한
 * 기본값을 쓰고, 부팅 인자(iommu.passthrough=)로 덮을 수 있다.
 * 설정자: 부팅 인자 파서. 읽는 자: 그룹을 만들 때 기본 도메인을 고르는 곳. */
static unsigned int iommu_def_domain_type __read_mostly;
/* [한국어] 언맵 후 TLB 무효화를 즉시 할 것인가(strict), 모아서 할 것인가(lazy).
 * lazy 는 훨씬 빠르지만 무효화 전까지 옛 매핑이 살아 있어, 그 창 동안
 * 장치가 이미 반환된 메모리에 접근할 수 있다. 그래서 보안과 성능의 맞바꿈이며
 * 기본값을 빌드 설정으로 정한다. */
static bool iommu_dma_strict __read_mostly = IS_ENABLED(CONFIG_IOMMU_DEFAULT_DMA_STRICT);
/* [한국어] 위 두 값이 부팅 인자로 지정됐는지 기억하는 비트들(IOMMU_CMD_LINE_*).
 * 왜 필요한가: 사용자가 명시한 값은 드라이버가 덮어쓰면 안 되기 때문이다.
 * 이 비트가 서 있으면 드라이버의 기본값 제안을 무시한다. */
static u32 iommu_cmd_line __read_mostly;

/* Tags used with xa_tag_pointer() in group->pasid_array */
/* [한국어] 위 영어 주석대로 pasid_array 의 포인터에 붙는 태그다. 같은 배열이
 * 두 종류의 것을 담기 때문에 구별이 필요하다 -- 도메인 자체를 담은 항목과,
 * 그 도메인에 딸린 폴트 처리 핸들을 담은 항목이다. 포인터의 하위 비트를
 * 태그로 쓰는 xa_tag_pointer() 규약이라 별도 필드가 들지 않는다. */
enum { IOMMU_PASID_ARRAY_DOMAIN = 0, IOMMU_PASID_ARRAY_HANDLE = 1 };

/*
 * [한국어] IOMMU 그룹 -- 하드웨어가 서로 격리해 낼 수 있는 가장 작은 장치 묶음.
 *
 * 이 구조체가 이 파일의 중심이다. 왜 장치가 아니라 그룹이 단위인가: PCIe
 * 스위치 뒤의 장치들은 요청자 ID 를 공유해 IOMMU 가 구별하지 못하고, ACS 가
 * 없는 브리지 아래에서는 장치끼리 서로의 DMA 를 가로챌 수 있다. 그런 장치들을
 * 따로 떼어 다른 주소 공간에 두는 것은 격리처럼 보이지만 실제로는 아니다.
 * 그래서 커널은 격리가 실제로 성립하는 최소 묶음을 찾아 그것을 단위로 삼는다.
 */
struct iommu_group {
	/* [한국어] sysfs 객체. /sys/kernel/iommu_groups/<id>/ 가 이것이다.
	 * 왜 여기 박혀 있는가: 그룹의 수명을 sysfs 참조가 관리하기 때문이다.
	 *   마지막 참조가 놓일 때 kobject release 콜백이 그룹을 해제한다.
	 * 설정자: iommu_group_alloc(). 읽는 자: sysfs 계층. */
	struct kobject kobj;

	/* [한국어] 그 아래 devices/ 하위 디렉토리.
	 * 왜 별도인가: 각 소속 장치가 이 디렉토리 아래 심볼릭 링크로 노출되고,
	 *   사용자 공간은 그 링크 목록을 읽어 "이 그룹에 무엇이 묶여 있는가"를
	 *   확인한다. VFIO 로 장치를 넘기기 전에 반드시 봐야 하는 정보다. */
	struct kobject *devices_kobj;

	/* [한국어] 소속 장치들의 목록. 원소는 struct group_device 다.
	 * 설정자: iommu_group_add_device(). 읽는 자: 도메인을 갈아 끼울 때
	 *   그룹의 모든 장치를 돌아야 하므로 이 파일 곳곳에서 순회한다.
	 * 동기화: 아래 @mutex 가 보호한다. for_each_group_device() 로 돈다. */
	struct list_head devices;

	/* [한국어] PASID 별 도메인 표. 키는 PASID, 값은 도메인이거나 폴트 핸들이다.
	 * 무엇인가: 한 장치가 여러 주소 공간을 동시에 쓸 수 있게 하는 것이 PASID 이며
	 *   (SVA 에서 프로세스마다 하나씩), 어느 PASID 가 어느 도메인에 붙어 있는지를
	 *   여기서 관리한다. PASID 0 은 이 표에 넣지 않고 @domain 이 맡는다.
	 * 값 구별: 위 IOMMU_PASID_ARRAY_* 태그로 도메인과 핸들을 가른다.
	 * 동기화: @mutex. */
	struct xarray pasid_array;

	/* [한국어] 이 그룹의 모든 상태를 보호하는 락.
	 * 무엇을 보호하는가: 장치 목록, 현재 도메인, 소유권, PASID 배열.
	 * 왜 뮤텍스인가: 도메인을 갈아 끼우는 일이 벤더 드라이버를 거쳐
	 *   하드웨어를 만지고 잠들 수 있기 때문이다. 스핀락으로는 불가능하다. */
	struct mutex mutex;

	/* [한국어] 이 그룹을 가져간 쪽이 자기 상태를 매달아 두는 자리.
	 * 설정자: iommu_group_set_iommudata(). 주로 VFIO 가 자기 문맥을 건다.
	 * 읽는 자: 그 소유자만. 이 파일은 내용을 해석하지 않는다. */
	void *iommu_data;

	/* [한국어] 위 iommu_data 를 해제하는 콜백.
	 * 왜 필요한가: 그룹이 사라질 때 소유자가 이미 없을 수 있어, 무엇을 어떻게
	 *   풀어야 하는지 그룹 자신은 모른다. 그래서 해제 방법을 함께 받아 둔다. */
	void (*iommu_data_release)(void *iommu_data);

	/* [한국어] 그룹 이름. sysfs 에 그대로 쓰인다.
	 * 값 범위: 벤더 드라이버가 지어 준 이름이거나 NULL.
	 * NULL 이면 @id 만으로 디렉토리 이름을 만든다. */
	char *name;

	/* [한국어] 그룹 번호. /sys/kernel/iommu_groups/<id> 의 그 숫자다.
	 * 설정자: iommu_group_alloc() 이 IDA 에서 받는다.
	 * 값 범위: 0 이상. 그룹이 해제되면 반납되어 재사용된다. */
	int id;

	/* [한국어] 아무도 따로 요구하지 않을 때 돌아가는 도메인.
	 * 왜 필요한가: 장치가 IOMMU 아래에 있는 동안에는 항상 어떤 주소 공간에
	 *   붙어 있어야 한다. 붙어 있지 않은 장치의 DMA 는 IOMMU 가 막거나
	 *   그냥 통과시키는데, 둘 다 안전한 기본값이 아니다.
	 * 종류: DMA 번역용이거나 identity(주소를 그대로 통과)이며, 어느 쪽인지는
	 *   iommu_def_domain_type 과 드라이버의 제안이 함께 정한다.
	 * 수명: 그룹이 살아 있는 내내. VFIO 가 장치를 가져가도 해제되지 않고,
	 *   돌려받을 때 여기로 되돌아온다. */
	struct iommu_domain *default_domain;

	/* [한국어] 모든 DMA 를 막는 도메인.
	 * 왜 필요한가: 도메인을 갈아 끼우는 사이에 장치가 옛 페이지 테이블로
	 *   접근하는 창을 없애기 위해서다. 먼저 여기로 옮겨 DMA 를 끊고,
	 *   그다음 새 도메인으로 옮긴다.
	 * 값 범위: 드라이버가 지원하지 않으면 NULL 일 수 있고, 그때는 대신
	 *   기본 도메인을 거쳐 간다. */
	struct iommu_domain *blocking_domain;

	/*
	 * During a group device reset, @resetting_domain points to the physical
	 * domain, while @domain points to the attached domain before the reset.
	 */
	/* [한국어] 위 영어 주석대로, 장치를 리셋하는 동안에만 쓰인다.
	 * 왜 두 필드가 필요한가: 리셋 중에는 하드웨어가 물리 주소로 동작해야
	 *   하지만, 리셋이 끝나면 원래 붙어 있던 도메인으로 되돌아가야 한다.
	 *   @domain 을 덮어쓰면 그 "원래"를 잃어버리므로 따로 둔다.
	 * 값 범위: 리셋 중이 아니면 NULL. */
	struct iommu_domain *resetting_domain;

	/* [한국어] 지금 이 그룹이 붙어 있는 도메인.
	 * 설정자: __iommu_group_set_domain() 계열만. 다른 곳에서 직접 쓰지 않는다.
	 * 읽는 자: 매핑 API 가 어느 페이지 테이블에 넣을지 정할 때.
	 * 값 범위: 보통 @default_domain 과 같고, VFIO 등이 가져가면 그쪽 도메인.
	 * 동기화: @mutex. */
	struct iommu_domain *domain;

	/* [한국어] 전역 그룹 목록에 매달리는 고리.
	 * 왜 필요한가: 버스 단위로 모든 그룹을 훑어야 하는 경우가 있다
	 *   (probe 지연 처리 등). 그때 이 목록을 돈다. */
	struct list_head entry;

	/* [한국어] 이 그룹을 몇 명이 소유하고 있는가.
	 * 왜 계수인가: 같은 소유자가 그룹 안의 여러 장치를 각각 가져갈 수 있고,
	 *   마지막 하나를 놓을 때 소유권이 풀려야 하기 때문이다.
	 * 0 이면 커널(DMA API)이 쓰는 상태. 1 이상이면 @owner 것이다.
	 * 동기화: @mutex. */
	unsigned int owner_cnt;

	/* [한국어] 소유자를 식별하는 불투명 포인터. 보통 VFIO 의 문맥이다.
	 * 왜 포인터로 비교하는가: 소유권은 "같은 주체인가"만 판정하면 되고,
	 *   내용을 해석할 필요가 없다. 다른 주체가 claim 하려 하면 거부된다.
	 * 값 범위: @owner_cnt 가 0 이면 의미 없음. */
	void *owner;
};

/*
 * [한국어] 그룹에 속한 장치 하나를 나타내는 항목.
 * 왜 struct device 를 목록에 직접 넣지 않는가: 장치는 여러 목록에 동시에
 * 속할 수 있고 IOMMU 만의 것이 아니기 때문이다. 그래서 고리를 따로 둔다.
 */
struct group_device {
	/* [한국어] 그룹의 devices 목록에 매달리는 고리. */
	struct list_head list;

	/* [한국어] 실제 장치.
	 * 설정자: iommu_group_add_device(). 읽는 자: 도메인을 붙이거나 뗄 때
	 *   벤더 드라이버에 넘기는 대상이 이것이다. */
	struct device *dev;

	/* [한국어] sysfs 링크 이름.
	 * 왜 저장해 두는가: 장치를 뗄 때 같은 이름으로 링크를 지워야 하는데,
	 *   그 시점에는 이름을 다시 만들 근거가 사라졌을 수 있기 때문이다. */
	char *name;
};

/* Iterate over each struct group_device in a struct iommu_group */
/* [한국어] 위 영어 주석대로 그룹의 장치들을 도는 매크로. 호출자가
 * group->mutex 를 들고 있어야 한다는 것이 암묵적 규약이다. */
#define for_each_group_device(group, pos) \
	list_for_each_entry(pos, &(group)->devices, list)

/*
 * [한국어] 그룹 sysfs 속성 하나를 기술하는 구조체.
 * 왜 표준 device_attribute 가 아닌가: 이 속성들의 대상은 struct device 가
 * 아니라 struct iommu_group 이기 때문이다. show/store 가 그룹을 직접 받도록
 * 자체 형을 정의하고, 아래에서 kobject 계층과 이어 붙인다.
 */
struct iommu_group_attribute {
	/* [한국어] sysfs 가 요구하는 공통 헤더. 이름과 권한이 들어 있다. */
	struct attribute attr;

	/* [한국어] 읽기 콜백. 그룹을 받아 버퍼에 쓴 바이트 수를 돌려준다. */
	ssize_t (*show)(struct iommu_group *group, char *buf);

	/* [한국어] 쓰기 콜백. NULL 이면 읽기 전용 속성이다. */
	ssize_t (*store)(struct iommu_group *group,
			 const char *buf, size_t count);
};

/*
 * [한국어] 예약 구간 종류를 sysfs 에 보일 이름으로 바꾸는 표.
 * 예약 구간이란 무엇인가: IOMMU 가 자유롭게 쓸 수 없는 주소 영역이다.
 * 어떤 것은 반드시 항등 매핑이어야 하고(펌웨어가 쓰는 버퍼), 어떤 것은
 * 아무도 쓰면 안 되며, MSI 창처럼 특별한 취급이 필요한 것도 있다.
 * IOVA 할당기가 이 구간들을 피해야 하므로 사용자 공간에도 노출한다.
 */
static const char * const iommu_group_resv_type_string[] = {
	[IOMMU_RESV_DIRECT]			= "direct",	/* [한국어] 반드시 항등 매핑 -- 펌웨어가 이미 이 주소로 접근 중이다 */
	[IOMMU_RESV_DIRECT_RELAXABLE]		= "direct-relaxable",	/* [한국어] 항등이면 좋지만 필수는 아니다. 부팅 후에는 회수해도 된다 */
	[IOMMU_RESV_RESERVED]			= "reserved",	/* [한국어] 아무도 쓰면 안 되는 구멍 */
	[IOMMU_RESV_MSI]			= "msi",	/* [한국어] 하드웨어가 정한 MSI 창 -- 인터럽트 메시지가 이리로 간다 */
	[IOMMU_RESV_SW_MSI]			= "msi",	/* [한국어] 소프트웨어가 잡아 주는 MSI 창. 사용자에게는 같은 이름으로 보인다 */
};

/* [한국어] iommu_cmd_line 의 비트들 -- 부팅 인자로 명시된 설정을 기억한다.
 * 이 비트가 서 있으면 드라이버가 제안하는 기본값을 무시한다. 사용자가
 * 직접 지정한 값을 코드가 덮어쓰지 않게 하는 것이 목적이다. */
#define IOMMU_CMD_LINE_DMA_API		BIT(0)	/* [한국어] 기본 도메인 종류를 사용자가 정했다 */
#define IOMMU_CMD_LINE_STRICT		BIT(1)	/* [한국어] strict/lazy 무효화를 사용자가 정했다 */

/* [한국어] 아래 전방 선언들 -- 이 파일 안에서 서로를 앞뒤로 부르기 때문에
 * 정의보다 먼저 이름을 알려야 한다. */
static int bus_iommu_probe(const struct bus_type *bus);	/* [한국어] 버스에 이미 붙어 있던 장치들을 뒤늦게 IOMMU 아래로 들인다 */
static int iommu_bus_notifier(struct notifier_block *nb,	/* [한국어] 장치가 나타나고 사라질 때 받는 통지 */
			      unsigned long action, void *data);
static void iommu_release_device(struct device *dev);	/* [한국어] 장치를 그룹에서 빼고 IOMMU 등록을 되돌린다 */
static int __iommu_attach_device(struct iommu_domain *domain,	/* [한국어] 장치 하나를 도메인에 붙인다. old 는 되돌릴 대상이다 */
				 struct device *dev, struct iommu_domain *old);
static int __iommu_attach_group(struct iommu_domain *domain,	/* [한국어] 그룹 전체를 도메인에 붙인다 */
				struct iommu_group *group);
static struct iommu_domain *__iommu_paging_domain_alloc_flags(struct device *dev,	/* [한국어] 번역용 도메인을 만든다 */
						       unsigned int type,
						       unsigned int flags);

enum {
	/* [한국어] 이 도메인 전환은 실패해서는 안 된다는 표시.
	 * 왜 이런 것이 필요한가: 해체 경로에서 그룹을 기본 도메인으로 되돌릴 때는
	 *   물러설 곳이 없다. 실패하면 장치가 어느 주소 공간에도 붙어 있지 않은
	 *   상태로 남기 때문이다. 그래서 그 경로는 이 플래그를 세워 부르고,
	 *   구현은 실패 시 되돌리는 대신 경고를 내고 밀고 나간다. */
	IOMMU_SET_DOMAIN_MUST_SUCCEED = 1 << 0,
};

static int __iommu_device_set_domain(struct iommu_group *group,	/* [한국어] 장치 하나의 도메인 전환. 실패 시 old_domain 으로 되돌린다 */
				     struct device *dev,
				     struct iommu_domain *new_domain,
				     struct iommu_domain *old_domain,
				     unsigned int flags);
static int __iommu_group_set_domain_internal(struct iommu_group *group,	/* [한국어] 그룹 전체 전환의 실제 구현. 아래 두 껍데기가 이것을 부른다 */
					     struct iommu_domain *new_domain,
					     unsigned int flags);

/*
 * [한국어]
 * __iommu_group_set_domain - 그룹을 다른 도메인으로 옮긴다 (실패 허용)
 *
 * @group:      대상 그룹. 호출자가 group->mutex 를 들고 있어야 한다.
 * @new_domain: 옮겨 갈 도메인
 * @return: 0 이면 성공. 음수면 실패했고, 그룹은 원래 도메인에 그대로 남는다.
 *
 * 평범한 경로가 쓰는 판이다. 실패하면 구현이 이미 되돌려 놓았으므로,
 * 호출자는 오류만 위로 전하면 된다.
 *
 * 호출 체인: 도메인 교체를 원하는 모든 곳 → [이 함수] → __iommu_group_set_domain_internal
 */
static int __iommu_group_set_domain(struct iommu_group *group,
				    struct iommu_domain *new_domain)
{
	return __iommu_group_set_domain_internal(group, new_domain, 0);	/* [한국어] 플래그 0 — 실패하면 되돌리라는 뜻 */
}

/*
 * [한국어]
 * __iommu_group_set_domain_nofail - 실패해서는 안 되는 도메인 전환
 *
 * @group:      대상 그룹
 * @new_domain: 옮겨 갈 도메인
 * @return: 없음
 *
 * 해체 경로가 쓴다. 그룹을 기본 도메인으로 되돌리는 일은 물러설 곳이
 * 없다 -- 실패했다고 원래 도메인에 남겨 두면, 그 도메인은 곧 해제될
 * 것이므로 장치가 사라진 페이지 테이블을 가리키게 된다.
 *
 * 그래서 MUST_SUCCEED 를 세워 부르고, 구현은 실패해도 되돌리지 않고
 * 밀고 나간다. WARN 은 그런 일이 실제로 일어났음을 남기는 것뿐이며,
 * 여기서 할 수 있는 복구는 없다.
 *
 * 호출 체인: 그룹/장치 해체 → [이 함수] → __iommu_group_set_domain_internal
 */
static void __iommu_group_set_domain_nofail(struct iommu_group *group,
					    struct iommu_domain *new_domain)
{
	WARN_ON(__iommu_group_set_domain_internal(	/* [한국어] 실패는 복구 불가이므로 기록만 남긴다 */
		group, new_domain, IOMMU_SET_DOMAIN_MUST_SUCCEED));
}

static int iommu_setup_default_domain(struct iommu_group *group,	/* [한국어] 그룹의 기본 도메인을 정해 만들고 붙인다 */
				      int target_type);
static int iommu_create_device_direct_mappings(struct iommu_domain *domain,	/* [한국어] 예약 구간을 항등 매핑으로 미리 채운다 */
					       struct device *dev);
static ssize_t iommu_group_store_type(struct iommu_group *group,	/* [한국어] sysfs 로 기본 도메인 종류를 바꾸는 경로 */
				      const char *buf, size_t count);
static struct group_device *iommu_group_alloc_device(struct iommu_group *group,	/* [한국어] 그룹에 넣을 장치 항목을 만든다 */
						     struct device *dev);
static void __iommu_group_free_device(struct iommu_group *group,	/* [한국어] 그 짝 */
				      struct group_device *grp_dev);
static void iommu_domain_init(struct iommu_domain *domain, unsigned int type,	/* [한국어] 갓 만든 도메인의 공통 필드를 채운다 */
			      const struct iommu_ops *ops);

/* [한국어] 그룹 sysfs 속성 하나를 정의하는 매크로. __ATTR 이 이름과 권한을
 * 채우고, show/store 는 위에서 정의한 그룹 전용 시그니처를 받는다. */
#define IOMMU_GROUP_ATTR(_name, _mode, _show, _store)		\
struct iommu_group_attribute iommu_group_attr_##_name =		\
	__ATTR(_name, _mode, _show, _store)

/* [한국어] sysfs 가 넘겨주는 일반 attribute 에서 우리 확장형으로 되짚는다. */
#define to_iommu_group_attr(_attr)	\
	container_of(_attr, struct iommu_group_attribute, attr)
/* [한국어] kobject 에서 그룹으로. 그룹이 kobject 를 품고 있으므로 역산이 된다. */
#define to_iommu_group(_kobj)		\
	container_of(_kobj, struct iommu_group, kobj)

/* [한국어] 등록된 IOMMU 하드웨어들의 목록. 벤더 드라이버가 자기 IOMMU 를
 * 발견할 때마다 여기 매단다. 장치를 어느 IOMMU 아래에 넣을지 정할 때 훑는다. */
static LIST_HEAD(iommu_device_list);
/* [한국어] 그 목록을 보호한다. 뮤텍스가 아니라 스핀락인 이유: 목록을 훑는
 * 일이 짧고, 장치 등록이 잠들 수 없는 문맥에서도 일어날 수 있기 때문이다. */
static DEFINE_SPINLOCK(iommu_device_lock);

/*
 * [한국어] IOMMU 아래로 장치가 들어올 수 있는 버스들.
 *
 * 왜 목록이 필요한가: 이 파일은 버스마다 통지를 받아야 장치가 나타나는
 * 것을 알 수 있는데, 어느 버스를 지켜볼지는 빌드 설정에 달려 있다.
 * 그래서 켜진 버스만 배열에 넣고, 초기화 때 그 수만큼 통지 블록을 만든다.
 *
 * platform 이 #ifdef 없이 항상 있는 이유: 장치 트리로 기술되는 장치는
 * 어느 아키텍처에나 있고, 그것이 IOMMU 를 쓰는 가장 흔한 경우다.
 */
static const struct bus_type * const iommu_buses[] = {
	&platform_bus_type,	/* [한국어] 장치 트리·ACPI 로 기술되는 장치들. 항상 존재한다 */
#ifdef CONFIG_PCI
	&pci_bus_type,	/* [한국어] 그룹 판정의 근거가 대부분 여기서 온다 — 스위치 토폴로지와 ACS */
#endif
#ifdef CONFIG_ARM_AMBA
	&amba_bustype,	/* [한국어] ARM SoC 의 온칩 장치들 */
#endif
#ifdef CONFIG_FSL_MC_BUS
	&fsl_mc_bus_type,	/* [한국어] Freescale/NXP 관리 복합체 */
#endif
#ifdef CONFIG_TEGRA_HOST1X_CONTEXT_BUS
	&host1x_context_device_bus_type,	/* [한국어] Tegra 의 host1x 문맥 — 하나의 물리 장치가 여러 문맥으로 나뉜다 */
#endif
#ifdef CONFIG_CDX_BUS
	&cdx_bus_type,	/* [한국어] AMD FPGA 장치 버스 */
#endif
};

/*
 * Use a function instead of an array here because the domain-type is a
 * bit-field, so an array would waste memory.
 */
/*
 * [한국어] 도메인 종류를 로그에 쓸 이름으로 바꾼다.
 *
 * 위 영어 주석이 배열 대신 함수를 쓴 이유를 밝힌다 -- 종류가 비트필드라
 * 값이 1, 2, 4, 8... 로 흩어져 있어 배열로 만들면 대부분이 빈 칸이 된다.
 *
 * 이름이 상수 이름과 다른 것에 주의할 것. IDENTITY 를 "Passthrough" 로,
 * DMA 와 DMA_FQ 를 똑같이 "Translated" 로 보여 준다. 사용자에게는 번역이
 * 일어나는지 아닌지가 중요하고, 무효화를 모아서 하는지(FQ)는 별도 줄로
 * 따로 알리기 때문이다.
 */
static const char *iommu_domain_type_str(unsigned int t)
{
	switch (t) {
	case IOMMU_DOMAIN_BLOCKED:
		return "Blocked";	/* [한국어] 모든 DMA 를 막는다 */
	case IOMMU_DOMAIN_IDENTITY:
		return "Passthrough";	/* [한국어] 주소를 그대로 통과시킨다 — 번역이 없으니 격리도 없다 */
	case IOMMU_DOMAIN_UNMANAGED:
		return "Unmanaged";	/* [한국어] 매핑을 커널이 아니라 VFIO 같은 소유자가 직접 관리한다 */
	case IOMMU_DOMAIN_DMA:
	case IOMMU_DOMAIN_DMA_FQ:
		return "Translated";	/* [한국어] 둘 다 DMA API 용 번역 도메인. 차이는 무효화 시점뿐이라 이름을 나누지 않는다 */
	case IOMMU_DOMAIN_PLATFORM:
		return "Platform";	/* [한국어] 하드웨어가 정한 고정 매핑 — 커널이 바꿀 수 없다 */
	default:
		return "Unknown";	/* [한국어] 새 종류가 생겼는데 이 표를 안 고쳤다 */
	}
}

/*
 * [한국어]
 * iommu_subsys_init - 기본 도메인 정책을 정하고 버스 통지를 건다
 *
 * @return: 0 이면 성공, -ENOMEM 이면 통지 블록 할당 실패
 *
 * 부팅 중 한 번 돈다. 이 함수가 정하는 것은 "앞으로 만들어질 모든 그룹이
 * 어떤 기본 도메인을 갖는가"이며, 그 결정이 시스템 전체의 성능과 격리
 * 수준을 좌우한다.
 *
 * 정책은 세 층으로 정해진다. 먼저 빌드 설정(CONFIG_IOMMU_DEFAULT_*)이
 * 기본을 잡고, 부팅 인자가 있으면 그것이 이긴다 -- IOMMU_CMD_LINE_DMA_API
 * 비트가 서 있으면 이 함수는 아예 손대지 않는다. 사용자가 명시한 값을
 * 코드가 덮어쓰지 않게 하려는 것이다.
 *
 * 세 번째 층이 이 함수의 중요한 판단이다. 메모리 암호화 플랫폼(SEV, TDX
 * 같은 기밀 컴퓨팅)에서는 passthrough 를 강제로 끈다. 그런 환경에서 주소를
 * 그대로 통과시키면 장치가 암호화되지 않은 호스트 메모리를 직접 보게 되어,
 * 기밀 컴퓨팅이 지키려는 것 자체가 무너지기 때문이다. 사용자가 명시하지
 * 않았을 때만 개입한다는 점에서 앞의 규칙과 일관된다.
 *
 * DMA_FQ 승격도 여기서 일어난다. 번역을 쓰면서 lazy 무효화를 허용했다면
 * 플러시 큐 방식으로 올린다 -- 언맵마다 TLB 를 비우지 않고 모아서 처리하는
 * 방식이라 훨씬 빠르지만, 비우기 전까지 옛 매핑이 살아 있다.
 *
 * 마지막으로 버스마다 통지 블록을 하나씩 걸어, 앞으로 장치가 나타날 때마다
 * 알림을 받는다. 이 등록이 이 파일이 세상과 이어지는 지점이다.
 *
 * 실행 컨텍스트: subsys_initcall. 부팅 중 프로세스 문맥이며 잠들 수 있다.
 *
 * 호출 체인: 커널 초기화 → [이 함수] → bus_register_notifier
 */
static int __init iommu_subsys_init(void)
{
	struct notifier_block *nb;	/* [한국어] 버스 수만큼 잡을 통지 블록 배열 */

	if (!(iommu_cmd_line & IOMMU_CMD_LINE_DMA_API)) {	/* [한국어] 사용자가 부팅 인자로 정하지 않았을 때만 개입한다 */
		if (IS_ENABLED(CONFIG_IOMMU_DEFAULT_PASSTHROUGH))
			iommu_set_default_passthrough(false);	/* [한국어] 빌드 기본이 통과 — 성능 우선 구성이다 */
		else
			iommu_set_default_translated(false);	/* [한국어] 빌드 기본이 번역 — 격리 우선 구성이다 */

		if (iommu_default_passthrough() && cc_platform_has(CC_ATTR_MEM_ENCRYPT)) {	/* [한국어] 기밀 컴퓨팅 플랫폼에서 통과는 위험하다 */
			pr_info("Memory encryption detected - Disabling default IOMMU Passthrough\n");	/* [한국어] 왜 설정과 다르게 동작하는지 남긴다 */
			iommu_set_default_translated(false);	/* [한국어] 통과시키면 장치가 암호화되지 않은 메모리를 직접 본다 */
		}
	}

	if (!iommu_default_passthrough() && !iommu_dma_strict)	/* [한국어] 번역을 쓰면서 lazy 무효화를 허용했다 */
		iommu_def_domain_type = IOMMU_DOMAIN_DMA_FQ;	/* [한국어] 플러시 큐로 올린다 — 언맵마다 TLB 를 비우지 않고 모아서 처리한다 */

	pr_info("Default domain type: %s%s\n",	/* [한국어] 최종 정책을 남긴다 — 성능 문제를 추적할 때 첫 단서가 된다 */
		iommu_domain_type_str(iommu_def_domain_type),
		(iommu_cmd_line & IOMMU_CMD_LINE_DMA_API) ?	/* [한국어] 사용자가 정한 값인지 함께 밝힌다 */
			" (set via kernel command line)" : "");

	if (!iommu_default_passthrough())	/* [한국어] 통과 모드면 무효화 정책이 의미가 없다 */
		pr_info("DMA domain TLB invalidation policy: %s mode%s\n",	/* [한국어] strict/lazy 는 보안과 성능의 맞바꿈이라 따로 알린다 */
			iommu_dma_strict ? "strict" : "lazy",
			(iommu_cmd_line & IOMMU_CMD_LINE_STRICT) ?
				" (set via kernel command line)" : "");

	nb = kzalloc_objs(*nb, ARRAY_SIZE(iommu_buses));	/* [한국어] 버스마다 하나씩. 등록한 뒤 해제하지 않으므로 수명이 커널과 같다 */
	if (!nb)
		return -ENOMEM;

	iommu_debug_init();	/* [한국어] debugfs 항목을 연다 */

	for (int i = 0; i < ARRAY_SIZE(iommu_buses); i++) {	/* [한국어] 켜져 있는 버스 전부에 */
		nb[i].notifier_call = iommu_bus_notifier;	/* [한국어] 같은 콜백을 건다 — 버스마다 다르게 다룰 것이 없다 */
		bus_register_notifier(iommu_buses[i], &nb[i]);	/* [한국어] 이 등록이 장치 발견의 통로다 */
	}

	return 0;
}
subsys_initcall(iommu_subsys_init);	/* [한국어] 장치 드라이버보다 먼저 돌아야 한다 — 그래야 장치가 나타날 때 이미 지켜보고 있다 */

/*
 * [한국어]
 * remove_iommu_group - 이 IOMMU 아래에 있던 장치 하나를 뗀다
 *
 * @dev:  검사할 장치
 * @data: 사라지는 IOMMU 하드웨어(struct iommu_device *)
 * @return: 항상 0 — 순회를 계속한다는 뜻
 *
 * bus_for_each_dev 콜백이라 버스의 모든 장치에 대해 불린다. 그중 이
 * IOMMU 를 쓰던 것만 골라 떼어 내야 하므로, 장치가 기록해 둔 iommu_dev 가
 * 사라지는 것과 같은지 비교한다.
 *
 * dev->iommu 를 먼저 보는 것에 주의할 것. IOMMU 아래에 들어오지 않은
 * 장치는 그 포인터가 NULL 이고, 그런 장치가 버스에 훨씬 많다.
 *
 * 실행 컨텍스트: IOMMU 등록 해제 경로. 잠들 수 있다.
 *
 * 호출 체인: iommu_device_unregister → bus_for_each_dev → [이 함수]
 */
static int remove_iommu_group(struct device *dev, void *data)
{
	if (dev->iommu && dev->iommu->iommu_dev == data)	/* [한국어] IOMMU 아래에 있고, 그것이 사라지는 바로 그 IOMMU 인가 */
		iommu_release_device(dev);	/* [한국어] 그룹에서 빼고 등록을 되돌린다 */

	return 0;	/* [한국어] 오류를 돌려주면 순회가 멈춘다 — 나머지 장치도 떼어야 하므로 항상 0 */
}

/**
 * iommu_device_register() - Register an IOMMU hardware instance
 * @iommu: IOMMU handle for the instance
 * @ops:   IOMMU ops to associate with the instance
 * @hwdev: (optional) actual instance device, used for fwnode lookup
 *
 * Return: 0 on success, or an error.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 벤더 드라이버가 자기 IOMMU 하드웨어를
 * 이 코어에 등록하는 유일한 문이다.
 *
 * 순서가 이 함수의 요점이다. 목록에 넣는 것이 먼저이고 장치 훑기가 나중인데,
 * 그 사이가 뒤바뀌면 안 된다 -- bus_iommu_probe() 가 훑는 장치들은 자기를
 * 맡을 IOMMU 를 이 목록에서 찾기 때문에, 목록에 없는 IOMMU 는 아무 장치도
 * 가져가지 못한다.
 *
 * ready 플래그를 마지막에 세우는 것도 같은 이유다. 훑는 도중에는 아직
 * 준비되지 않은 상태이고, 그 사이에 들어온 장치는 나중에 다시 시도된다.
 * WRITE_ONCE 인 것은 이 값을 락 없이 읽는 쪽이 있기 때문이다.
 *
 * 실패하면 스스로 unregister 를 불러 되돌린다 -- 목록에 이미 들어갔고
 * 일부 장치가 이미 붙었을 수 있어, 호출자가 정리할 방법이 없다.
 *
 * 실행 컨텍스트: 벤더 드라이버의 probe. 잠들 수 있다.
 *
 * 호출 체인: intel/amd/arm 드라이버 probe → [이 함수] → bus_iommu_probe
 */
int iommu_device_register(struct iommu_device *iommu,
			  const struct iommu_ops *ops, struct device *hwdev)
{
	int err = 0;

	/* We need to be able to take module references appropriately */
	/* [한국어] 위 영어 주석대로, ops 가 모듈 안에 있으면 그 모듈이 내려가지
	 * 않도록 참조를 들어야 한다. owner 가 없으면 참조를 들 방법이 없고,
	 * 모듈이 내려간 뒤 사라진 vtable 을 부르게 된다. */
	if (WARN_ON(is_module_address((unsigned long)ops) && !ops->owner))
		return -EINVAL;

	iommu->ops = ops;	/* [한국어] 이 파일이 하드웨어를 만지는 유일한 통로 */
	if (hwdev)
		iommu->fwnode = dev_fwnode(hwdev);	/* [한국어] 펌웨어 노드 — 장치 트리·ACPI 가 "이 장치는 저 IOMMU 아래" 라고 가리키는 근거다 */

	spin_lock(&iommu_device_lock);
	list_add_tail(&iommu->list, &iommu_device_list);	/* [한국어] 훑기보다 먼저 목록에 넣어야 한다 — 장치들이 이 목록에서 자기 IOMMU 를 찾는다 */
	spin_unlock(&iommu_device_lock);

	for (int i = 0; i < ARRAY_SIZE(iommu_buses) && !err; i++)	/* [한국어] 이미 버스에 붙어 있던 장치들을 뒤늦게 들인다 */
		err = bus_iommu_probe(iommu_buses[i]);	/* [한국어] 통지는 앞으로 올 장치만 알려 주므로, 과거분은 여기서 따라잡는다 */
	if (err)
		iommu_device_unregister(iommu);	/* [한국어] 일부가 이미 붙었을 수 있어 호출자가 정리할 수 없다 — 스스로 되돌린다 */
	else
		WRITE_ONCE(iommu->ready, true);	/* [한국어] 훑기가 끝난 뒤에야 준비 완료. 락 없이 읽는 쪽이 있어 WRITE_ONCE 다 */
	return err;
}
EXPORT_SYMBOL_GPL(iommu_device_register);	/* [한국어] 모든 벤더 IOMMU 드라이버가 부른다 */

/*
 * [한국어]
 * iommu_device_unregister - IOMMU 하드웨어 하나를 코어에서 뗀다
 *
 * @iommu: 사라지는 IOMMU
 * @return: 없음
 *
 * 등록의 역순이다. 장치를 먼저 떼고 목록에서 빼는 순서가 중요한데,
 * 반대로 하면 목록에서 사라진 IOMMU 를 remove_iommu_group() 이 찾지
 * 못해 장치들이 붙은 채로 남는다.
 *
 * 마지막의 singleton_group 반납은 위 영어 주석이 말하는 짝이다. 그룹을
 * 만들 필요가 없는 단순한 구성에서 IOMMU 하나가 그룹 하나를 통째로
 * 들고 있는 경우가 있고, 그 참조를 여기서 놓는다.
 *
 * 실행 컨텍스트: 벤더 드라이버의 remove. 잠들 수 있다.
 *
 * 호출 체인: 벤더 드라이버 remove / iommu_device_register(실패) → [이 함수]
 */
void iommu_device_unregister(struct iommu_device *iommu)
{
	for (int i = 0; i < ARRAY_SIZE(iommu_buses); i++)	/* [한국어] 목록에서 빼기 전에 장치를 먼저 뗀다 */
		bus_for_each_dev(iommu_buses[i], NULL, iommu, remove_iommu_group);	/* [한국어] 이 IOMMU 를 쓰던 장치만 콜백이 골라 낸다 */

	spin_lock(&iommu_device_lock);
	list_del(&iommu->list);	/* [한국어] 이제 아무 장치도 이 IOMMU 를 찾지 못한다 */
	spin_unlock(&iommu_device_lock);

	/* Pairs with the alloc in generic_single_device_group() */
	iommu_group_put(iommu->singleton_group);	/* [한국어] 위 영어 주석대로 그쪽에서 든 참조의 짝. NULL 이어도 안전하다 */
	iommu->singleton_group = NULL;	/* [한국어] 재등록에서 이미 놓은 그룹을 다시 놓지 않도록 */
}
EXPORT_SYMBOL_GPL(iommu_device_unregister);

#if IS_ENABLED(CONFIG_IOMMUFD_TEST)	/* [한국어] 여기부터 iommufd 셀프테스트 전용. 실제 하드웨어 없이 가짜 IOMMU 드라이버와 가짜 장치를 지어 코어 로직을 시험하기 위한 진입점들이며, 일반 빌드에는 컴파일되지 않는다 */
/*
 * [한국어]
 * iommu_device_unregister_bus - 버스 하나에 붙여 둔 가짜 IOMMU 드라이버를 거둔다 (셀프테스트 전용)
 *
 * @iommu: 등록했던 IOMMU 인스턴스
 * @bus:   그 인스턴스가 맡고 있던 버스
 * @nb:    호출자가 제공했던 알림 블록 메모리
 *
 * iommu_device_register_bus 의 정확한 역순이다. 알림 → 펌웨어 노드 → 인스턴스
 * 순으로 되돌리는데, 순서가 중요하다. 알림을 먼저 떼지 않으면 해제 도중에 추가된
 * 장치가 곧 사라질 드라이버로 프로브될 수 있다.
 *
 * 실행 컨텍스트: 셀프테스트 모듈의 정리 경로. 프로세스 문맥.
 *
 * 호출 체인: iommufd selftest, iommu_device_register_bus 의 에러 경로 → [이 함수]
 */
void iommu_device_unregister_bus(struct iommu_device *iommu,
				 const struct bus_type *bus,
				 struct notifier_block *nb)
{
	bus_unregister_notifier(bus, nb);	/* [한국어] 가짜 드라이버가 걸어 둔 버스 알림부터 뗀다 — 이후 추가되는 장치가 이 드라이버로 오지 않게 */
	fwnode_remove_software_node(iommu->fwnode);	/* [한국어] 가짜 펌웨어 노드 해제. 실제 하드웨어는 DT/ACPI 가 만든 노드를 쓰지만 셀프테스트는 소프트웨어 노드를 지어 쓴다 */
	iommu_device_unregister(iommu);	/* [한국어] 공통 해제 경로 — 전역 목록에서 빼고 붙어 있던 장치를 모두 떼어 낸다 */
}
EXPORT_SYMBOL_GPL(iommu_device_unregister_bus);

/*
 * Register an iommu driver against a single bus. This is only used by iommufd
 * selftest to create a mock iommu driver. The caller must provide
 * some memory to hold a notifier_block.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * iommu_device_register_bus - fwnode 매칭 없이 버스 하나에 IOMMU 드라이버를 등록한다 (셀프테스트 전용)
 *
 * @iommu:  등록할 IOMMU 인스턴스 (셀프테스트가 정적으로 들고 있다)
 * @ops:    그 드라이버의 콜백 표
 * @bus:    이 드라이버가 맡을 버스
 * @nb:     알림 블록으로 쓸 메모리. 코어가 할당하지 않고 호출자가 준다.
 * @return: 0 성공, 음수 실패 (실패 시 등록은 전부 되돌아간 상태)
 *
 * 정상 등록 경로인 iommu_device_register 와 비교하면 이 함수의 성격이 드러난다.
 * 정상 경로는 시스템에 있는 모든 버스에 걸쳐 등록되고 fwnode(DT/ACPI 노드)로
 * 장치와 짝을 맞추지만, 여기서는 버스 하나에만 붙고 fwnode 는 소프트웨어 노드를
 * 지어서 쓴다. 실제 IOMMU 하드웨어 없이 코어의 그룹·도메인 로직만 시험하기 위한
 * 발판이다.
 *
 * 마지막 WRITE_ONCE(ready) 가 이 함수의 계약을 요약한다 — 목록 등록과 기존 장치
 * 프로브가 모두 끝난 뒤에야 준비 완료를 공개하므로, 다른 CPU 가 반쯤 만들어진
 * 인스턴스를 쓰는 일이 없다.
 *
 * 실행 컨텍스트: 셀프테스트 모듈 초기화. 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: iommufd selftest → [이 함수] → bus_iommu_probe → __iommu_probe_device
 */
int iommu_device_register_bus(struct iommu_device *iommu,
			      const struct iommu_ops *ops,
			      const struct bus_type *bus,
			      struct notifier_block *nb)
{
	int err;	/* [한국어] 알림 등록/프로브 결과를 받는다 */

	iommu->ops = ops;	/* [한국어] 정상 등록 경로와 달리 fwnode 매칭 없이 ops 를 직접 꽂는다 */
	nb->notifier_call = iommu_bus_notifier;	/* [한국어] 호출자가 준 메모리에 코어의 표준 버스 알림 핸들러를 심는다 */
	err = bus_register_notifier(bus, nb);	/* [한국어] 이 버스에 장치가 추가/제거될 때마다 iommu_bus_notifier 가 불리게 한다 */
	if (err)	/* [한국어] 버스 알림 등록 실패 */
		return err;	/* [한국어] 알림 등록 실패 — 아직 아무 것도 만들지 않았으므로 그대로 반환 */

	iommu->fwnode = fwnode_create_software_node(NULL, NULL);	/* [한국어] 매칭용 가짜 펌웨어 노드를 짓는다. iommu_fwspec_ops 가 fwnode 로 드라이버를 찾기 때문에 실체가 없어도 노드는 필요하다 */
	if (IS_ERR(iommu->fwnode)) {	/* [한국어] 노드 생성 실패 */
		bus_unregister_notifier(bus, nb);	/* [한국어] 방금 건 알림부터 되돌린다 */
		return PTR_ERR(iommu->fwnode);	/* [한국어] 에러 포인터를 코드로 풀어 반환 */
	}

	spin_lock(&iommu_device_lock);	/* [한국어] 전역 IOMMU 인스턴스 목록 보호 — 스핀락이라 이 구간에서 잠들면 안 된다 */
	list_add_tail(&iommu->list, &iommu_device_list);	/* [한국어] 이 순간부터 fwnode 매칭 대상이 된다 */
	spin_unlock(&iommu_device_lock);	/* [한국어] 목록 갱신 끝 */

	err = bus_iommu_probe(bus);	/* [한국어] 이미 버스에 있던 장치들을 소급해서 이 드라이버로 프로브한다 */
	if (err) {	/* [한국어] 한 장치라도 실패하면 */
		iommu_device_unregister_bus(iommu, bus, nb);	/* [한국어] 등록 자체를 통째로 되돌린다 */
		return err;	/* [한국어] 프로브 실패 전달 */
	}
	WRITE_ONCE(iommu->ready, true);	/* [한국어] 마지막에 한 번에 '준비 완료'를 공개한다 — 다른 CPU 가 부분 초기화 상태를 보지 않도록 */
	return 0;	/* [한국어] 가짜 IOMMU 등록 완료 */
}
EXPORT_SYMBOL_GPL(iommu_device_register_bus);

/*
 * [한국어]
 * iommu_mock_device_add - 가짜 장치를 가짜 IOMMU 에 묶어 등록한다 (셀프테스트 전용)
 *
 * @dev:    아직 device_add 되지 않은 장치
 * @iommu:  이 장치를 맡을 가짜 IOMMU 인스턴스
 * @return: 0 성공, 음수 실패
 *
 * 순서가 핵심이다. device_add 는 버스 알림을 돌려 곧바로 __iommu_probe_device 를
 * 부르는데, 그때 이미 fwspec 이 준비되어 있어야 매칭이 성립한다. 그래서 fwspec 을
 * 먼저 심고 나서 장치를 등록한다. 실제 하드웨어에서는 이 역할을 DT/ACPI 파싱
 * (bus->dma_configure)이 대신한다.
 *
 * 실행 컨텍스트: 셀프테스트. 프로세스 문맥.
 *
 * 호출 체인: iommufd selftest → [이 함수] → iommu_fwspec_init, device_add
 */
int iommu_mock_device_add(struct device *dev, struct iommu_device *iommu)
{
	int rc;	/* [한국어] fwspec 생성과 장치 등록의 결과 */

	mutex_lock(&iommu_probe_device_lock);	/* [한국어] fwspec 조작은 프로브 직렬화 락 아래에서만 허용된다 */
	rc = iommu_fwspec_init(dev, iommu->fwnode);	/* [한국어] 이 가짜 장치를 가짜 IOMMU 의 fwnode 에 미리 묶어 둔다 — 곧 이어질 프로브가 이 fwspec 을 보고 드라이버를 찾는다 */
	mutex_unlock(&iommu_probe_device_lock);	/* [한국어] 락 구간 최소화 */

	if (rc)	/* [한국어] fwspec 생성 실패 */
		return rc;	/* [한국어] fwspec 을 못 만들면 장치를 등록하지 않는다 */

	rc = device_add(dev);	/* [한국어] 여기서 버스 알림이 돌며 __iommu_probe_device 가 실제로 불린다 */
	if (rc)	/* [한국어] 장치 등록 실패 */
		iommu_fwspec_free(dev);	/* [한국어] 미리 만든 fwspec 도 함께 거둔다 */
	return rc;	/* [한국어] 등록 결과 전달 */
}
EXPORT_SYMBOL_GPL(iommu_mock_device_add);
#endif

/*
 * [한국어]
 * dev_iommu_get - 장치의 IOMMU 상태 구조체를 얻거나 만든다
 *
 * @dev:    대상 장치
 * @return: struct dev_iommu 포인터. 할당 실패면 NULL.
 *
 * struct device 는 IOMMU 에 대해 포인터 한 칸(dev->iommu)만 알고 있고, 그 너머의
 * 모든 것 — fwspec, 담당 IOMMU 인스턴스, PASID 한계, 지연 부착 여부, 드라이버
 * 전용 문맥 — 이 이 구조체에 모여 있다. 프로브 경로에서 가장 먼저 만들어지고
 * dev_iommu_free 로 마지막에 사라진다.
 *
 * 이미 있으면 그대로 돌려주는 멱등 함수다. 생성이 전역 프로브 락으로 직렬화되기
 * 때문에 "확인 후 생성" 사이에 다른 CPU 가 끼어들 수 없고, 그래서 락 없는 이중
 * 할당 걱정이 없다.
 *
 * 실행 컨텍스트: 프로브 경로. iommu_probe_device_lock 을 든 채로. 잠들 수 있다.
 *
 * 호출 체인: iommu_init_device, iommu_fwspec_init → [이 함수]
 */
static struct dev_iommu *dev_iommu_get(struct device *dev)
{
	struct dev_iommu *param = dev->iommu;	/* [한국어] 장치별 IOMMU 상태. struct device 에 포인터 한 칸으로 매달린다 */

	lockdep_assert_held(&iommu_probe_device_lock);	/* [한국어] 이 구조체의 생성은 전역 프로브 락으로 직렬화된다 — 두 경로가 동시에 만들면 하나가 그대로 새 나간다 */

	if (param)	/* [한국어] 이미 있으면 (재시도·재진입 경로) */
		return param;	/* [한국어] 그대로 재사용 */

	param = kzalloc_obj(*param);	/* [한국어] 0 으로 채운 새 구조체 — 모든 플래그가 거짓에서 출발한다 */
	if (!param)	/* [한국어] 할당 실패 */
		return NULL;	/* [한국어] 호출자가 -ENOMEM 으로 바꾼다 */

	mutex_init(&param->lock);	/* [한국어] 이 장치의 fault 핸들러·PASID 상태를 지키는 장치 단위 락 */
	dev->iommu = param;	/* [한국어] 장치에 붙인다 — 이 줄 이후 dev_iommu_ops/dev_has_iommu 가 동작한다 */
	return param;	/* [한국어] 새로 만든 상태 반환 */
}

/*
 * [한국어]
 * dev_iommu_free - 장치의 IOMMU 상태를 해제한다
 *
 * @dev: 대상 장치
 *
 * dev_iommu_get 의 짝. dev->iommu 를 먼저 NULL 로 만든 뒤에 해제하는 순서가
 * 의도적이다 — 해제 도중 다른 경로가 이 장치를 보더라도 "IOMMU 상태 없음"으로
 * 일관되게 읽히기 때문이다.
 *
 * fwspec 은 이 구조체가 소유하므로 함께 거둔다. 그 안의 iommu_fwnode 는 DT/ACPI
 * 노드에 대한 참조라 put 이 필요하다.
 *
 * 두 곳에서 불린다. 정상 해제 경로(iommu_deinit_device)와, 드라이버가 끝내
 * 붙지 않아 fwspec 만 남은 장치를 치우는 iommu_release_device 다.
 *
 * 실행 컨텍스트: 프로브/해제 경로. 프로세스 문맥.
 *
 * 호출 체인: iommu_deinit_device, iommu_release_device, iommu_init_device 에러 경로 → [이 함수]
 */
void dev_iommu_free(struct device *dev)
{
	struct dev_iommu *param = dev->iommu;	/* [한국어] 해제할 장치별 상태를 집어 둔다 */

	dev->iommu = NULL;	/* [한국어] 먼저 끊는다 — 이후 누가 보더라도 '이 장치엔 IOMMU 상태가 없다'로 읽힌다 */
	if (param->fwspec) {	/* [한국어] 펌웨어 매칭 정보를 남겨 두었다면 */
		fwnode_handle_put(param->fwspec->iommu_fwnode);	/* [한국어] fwspec 이 잡고 있던 fwnode 참조를 반납 */
		kfree(param->fwspec);	/* [한국어] fwspec 본체 해제 */
	}
	kfree(param);	/* [한국어] 장치별 상태 해제 */
}

/*
 * Internal equivalent of device_iommu_mapped() for when we care that a device
 * actually has API ops, and don't want false positives from VFIO-only groups.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * dev_has_iommu - 이 장치에 실제 IOMMU 드라이버가 붙어 있는가
 *
 * @dev:    대상 장치
 * @return: true 면 ops 를 불러도 되는 장치
 *
 * device_iommu_mapped() 와 다른 점이 이 파일에서 자주 문제가 된다. 그룹에 들어
 * 있다고 해서 IOMMU 드라이버가 붙은 것은 아니다 — VFIO 가 iommu_group_add_device
 * 로 수동 구성한 그룹의 장치는 dev->iommu 는 있어도 담당 인스턴스가 없다. 그런
 * 장치에 ops 를 부르면 NULL 역참조가 된다.
 *
 * 그래서 해제 경로(__iommu_group_remove_device)가 iommu_deinit_device 를 부를지
 * 그냥 연결만 끊을지를 이 함수로 가른다.
 *
 * 실행 컨텍스트: 어디서든. 순수 판별식이다.
 *
 * 호출 체인: __iommu_group_remove_device 등 코어 내부 → [이 함수]
 */
static bool dev_has_iommu(struct device *dev)
{
	return dev->iommu && dev->iommu->iommu_dev;	/* [한국어] dev->iommu 만으로는 부족하다. VFIO 가 수동으로 만든 그룹처럼 IOMMU 드라이버 없이 상태만 달린 경우가 있어, 실제 담당 인스턴스까지 붙었는지 봐야 API 를 부를 수 있다 */
}

/*
 * [한국어]
 * dev_iommu_get_max_pasids - 이 장치가 쓸 수 있는 PASID 개수의 상한을 구한다
 *
 * @dev:    대상 장치. dev->iommu->iommu_dev 가 이미 채워져 있어야 한다.
 * @return: 사용 가능한 PASID 개수 (0 이면 PASID 불가)
 *
 * PASID(Process Address Space ID)는 하나의 물리 장치가 여러 주소 공간을 동시에
 * 쓰게 해 주는 식별자다. 이것이 있어야 SVA(Shared Virtual Addressing) — 장치가
 * 프로세스의 가상 주소를 그대로 쓰는 것 — 가 성립하고, GPU·가속기가 CPU 와 같은
 * 포인터를 주고받을 수 있다.
 *
 * 상한은 양쪽에서 온다. 장치가 몇 비트의 PASID 를 낼 수 있는지(PCIe 는 PASID
 * Capability, 플랫폼 장치는 pasid-num-bits 속성)와, IOMMU 가 몇 개까지 표를
 * 들 수 있는지다. 둘 중 작은 쪽이 실제 한계이며, 프로브 때 한 번 계산해
 * dev->iommu->max_pasids 에 캐시해 둔다.
 *
 * 실행 컨텍스트: 프로브 경로. 프로세스 문맥.
 *
 * 호출 체인: iommu_init_device → [이 함수] → pci_max_pasids / device_property_read_u32
 */
static u32 dev_iommu_get_max_pasids(struct device *dev)
{
	u32 max_pasids = 0, bits = 0;	/* [한국어] 장치가 낼 수 있는 PASID 개수와, 플랫폼 장치가 알리는 비트 폭 */
	int ret;	/* [한국어] 능력 조회 결과 */

	if (dev_is_pci(dev)) {	/* [한국어] PCIe 는 PASID 능력이 확장 컨피그 공간에 표준으로 있다 */
		ret = pci_max_pasids(to_pci_dev(dev));	/* [한국어] PASID Capability 의 Max PASID Width 를 읽어 개수로 환산해 준다 */
		if (ret > 0)	/* [한국어] 능력이 없으면 0 이나 음수 */
			max_pasids = ret;	/* [한국어] 지원 개수 확정 */
	} else {
		ret = device_property_read_u32(dev, "pasid-num-bits", &bits);	/* [한국어] 플랫폼 장치는 DT/ACPI 속성으로 PASID 비트 폭을 알린다 */
		if (!ret)	/* [한국어] 속성이 있었다면 */
			max_pasids = 1UL << bits;	/* [한국어] 비트 폭을 개수로 편다 */
	}

	return min_t(u32, max_pasids, dev->iommu->iommu_dev->max_pasids);	/* [한국어] 장치가 낼 수 있는 수와 IOMMU 가 받아 줄 수 있는 수 중 작은 쪽. 둘 다 만족해야 SVA/PASID 부착이 성립한다 */
}

/*
 * [한국어]
 * dev_iommu_priv_set - 벤더 드라이버의 장치별 문맥 포인터를 심는다
 *
 * @dev:  대상 장치
 * @priv: 드라이버가 소유하는 불투명 포인터
 *
 * 코어는 이 값을 절대 해석하지 않는다. 인텔 드라이버는 device_domain_info 를,
 * ARM SMMU 는 arm_smmu_master 를 여기 매달고, dev_iommu_priv_get 으로 되찾는다.
 * 드라이버가 struct device 에 자기 필드를 추가하지 않고도 장치별 상태를 붙일 수
 * 있게 해 주는 자리다.
 *
 * lockdep 검사가 조건부인 이유는 FSL PAMU 하나 때문이다. 나머지 드라이버는 모두
 * probe_device 안, 즉 전역 프로브 락 아래에서만 이 값을 설정한다.
 *
 * 실행 컨텍스트: 드라이버의 probe_device 콜백 안. 프로세스 문맥.
 *
 * 호출 체인: 벤더 드라이버 probe_device → [이 함수]
 */
void dev_iommu_priv_set(struct device *dev, void *priv)
{
	/* FSL_PAMU does something weird */
	if (!IS_ENABLED(CONFIG_FSL_PAMU))	/* [한국어] FSL PAMU 만 프로브 락 밖에서 이 값을 건드린다 (위 영어 주석의 'weird') */
		lockdep_assert_held(&iommu_probe_device_lock);	/* [한국어] 그 외 모든 드라이버는 프로브 직렬화 락 아래에서만 설정한다 */
	dev->iommu->priv = priv;	/* [한국어] 벤더 드라이버가 장치별 문맥(예: 인텔의 device_domain_info)을 여기 매단다 — 코어는 내용을 해석하지 않는 불투명 포인터다 */
}
EXPORT_SYMBOL_GPL(dev_iommu_priv_set);

/*
 * Init the dev->iommu and dev->iommu_group in the struct device and get the
 * driver probed
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * iommu_init_device - 장치를 맡을 IOMMU 드라이버를 찾아 붙이고 그룹까지 정한다
 *
 * @dev:    아직 IOMMU 아래로 들어오지 않은 장치
 * @return: 0 성공. -ENODEV 면 이 장치를 맡을 IOMMU 가 없다(흔한 정상 결과).
 *          그 외 음수는 실제 실패이며, 이 함수가 자기가 만든 것을 모두 되감은 뒤다.
 *
 * 장치가 IOMMU 세계로 들어오는 첫 관문이다. 다섯 단계가 순서대로 일어난다.
 *   1) dev->iommu 상태 구조체 생성
 *   2) 필요하면 bus->dma_configure 를 불러 펌웨어(DT/ACPI)에서 fwspec 을 채움
 *   3) fwspec 의 fwnode 로 담당 IOMMU 드라이버(ops)를 찾고 모듈 참조를 잡음
 *   4) ops->probe_device 로 드라이버에게 수락 여부를 물음
 *   5) ops->device_group 으로 이 장치가 속할 격리 그룹을 결정
 * 마지막에 PASID 한계와 지연 부착 여부까지 캐시하면 장치 초기화가 끝난다.
 *
 * 중간에 락을 놓았다 다시 잡는 구간이 있다는 점이 중요하다. dma_configure 가
 * 재진입할 수 있어 프로브 락을 잠깐 놓는데, 그 사이 다른 CPU 가 같은 장치를
 * 끝냈을 수 있으므로 돌아온 직후 상태를 다시 확인한다.
 *
 * 에러 처리는 계단식 라벨이다. 어느 단계에서 실패했든 그 지점까지 만들어진 것만
 * 정확히 역순으로 되돌린다.
 *
 * 실행 컨텍스트: 프로브 경로. iommu_probe_device_lock 을 든 채 들어오며,
 * 함수 안에서 잠깐 놓았다 다시 잡는다. 잠들 수 있다.
 *
 * 호출 체인: __iommu_probe_device → [이 함수]
 *            → dev_iommu_get, bus->dma_configure, ops->probe_device, ops->device_group
 */
static int iommu_init_device(struct device *dev)
{
	const struct iommu_ops *ops;	/* [한국어] 이 장치를 맡을 IOMMU 드라이버의 콜백 표. fwspec 매칭으로 정해진다 */
	struct iommu_device *iommu_dev;	/* [한국어] 실제로 이 장치를 담당하는 IOMMU 하드웨어 인스턴스 */
	struct iommu_group *group;	/* [한국어] 이 장치가 속할 격리 그룹 */
	int ret;	/* [한국어] 각 단계의 실패 코드 */

	if (!dev_iommu_get(dev))	/* [한국어] 장치별 IOMMU 상태부터 만든다 — 이후 모든 단계가 이 구조체를 채워 나간다 */
		return -ENOMEM;	/* [한국어] 상태 구조체조차 못 만들면 더 갈 수 없다 */
	/*
	 * For FDT-based systems and ACPI IORT/VIOT, the common firmware parsing
	 * is buried in the bus dma_configure path. Properly unpicking that is
	 * still a big job, so for now just invoke the whole thing. The device
	 * already having a driver bound means dma_configure has already run and
	 * found no IOMMU to wait for, so there's no point calling it again.
	 */
	if (!dev->iommu->fwspec && !dev->driver && dev->bus->dma_configure) {	/* [한국어] fwspec 이 아직 없고 드라이버도 안 붙었다 = 펌웨어 파싱이 아직 돌지 않았다는 뜻. 드라이버가 이미 붙었다면 dma_configure 는 이미 돌았고 IOMMU 를 못 찾은 것이므로 다시 부를 이유가 없다 (위 영어 주석) */
		mutex_unlock(&iommu_probe_device_lock);	/* [한국어] dma_configure 는 내부에서 다시 이 락을 잡을 수 있어 잠깐 놓는다 */
		dev->bus->dma_configure(dev);	/* [한국어] DT 의 iommus 속성이나 ACPI IORT/VIOT 를 파싱해 이 장치의 fwspec 을 채운다 */
		mutex_lock(&iommu_probe_device_lock);	/* [한국어] 다시 잡는다 — 놓은 사이 상태가 바뀌었을 수 있다 */
		/* If another instance finished the job for us, skip it */
		if (!dev->iommu || dev->iommu_group)	/* [한국어] 락을 놓은 동안 다른 CPU 가 같은 장치를 끝냈거나(그룹 있음) 정리해 버렸다(상태 없음) */
			return -ENODEV;	/* [한국어] 중복 작업을 피해 조용히 물러난다 */
	}
	/*
	 * At this point, relevant devices either now have a fwspec which will
	 * match ops registered with a non-NULL fwnode, or we can reasonably
	 * assume that only one of Intel, AMD, s390, PAMU or legacy SMMUv2 can
	 * be present, and that any of their registered instances has suitable
	 * ops for probing, and thus cheekily co-opt the same mechanism.
	 */
	ops = iommu_fwspec_ops(dev->iommu->fwspec);	/* [한국어] fwspec 의 fwnode 로 등록된 IOMMU 드라이버를 찾는다. fwspec 이 없으면 fwnode 가 NULL 인 인스턴스를 집는데, 인텔·AMD·s390·PAMU·구형 SMMUv2 는 시스템에 하나뿐이라 그 편법이 성립한다 (위 영어 주석) */
	if (!ops) {	/* [한국어] 이 장치를 맡을 IOMMU 가 없다 — 오류라기보다 흔한 정상 결과다 */
		ret = -ENODEV;	/* [한국어] IOMMU 없이 동작할 장치로 남는다 */
		goto err_free;	/* [한국어] 만들어 둔 장치 상태만 거두면 된다 */
	}

	if (!try_module_get(ops->owner)) {	/* [한국어] 드라이버 모듈이 언로드되지 않도록 참조를 잡는다. 장치가 붙어 있는 동안 모듈이 사라지면 ops 포인터가 통째로 무효가 된다 */
		ret = -EINVAL;	/* [한국어] 모듈이 이미 내려가는 중 */
		goto err_free;	/* [한국어] 상태만 거두고 물러난다 */
	}

	iommu_dev = ops->probe_device(dev);	/* [한국어] 벤더 드라이버에 '이 장치를 맡을 수 있는가'를 묻는다. 성공하면 담당 IOMMU 인스턴스를 돌려준다 — 인텔이면 DMAR 단위, ARM 이면 SMMU 인스턴스 */
	if (IS_ERR(iommu_dev)) {	/* [한국어] 드라이버가 거절했다 (스트림 ID 범위 밖, 하드웨어 미지원 등) */
		ret = PTR_ERR(iommu_dev);	/* [한국어] 드라이버가 준 이유를 그대로 올린다 */
		goto err_module_put;	/* [한국어] 모듈 참조만 놓으면 된다 */
	}
	dev->iommu->iommu_dev = iommu_dev;	/* [한국어] 이 줄이 채워져야 dev_has_iommu 가 참이 된다 — '진짜 IOMMU 가 붙은 장치'의 판별 기준 */

	ret = iommu_device_link(iommu_dev, dev);	/* [한국어] sysfs 에 IOMMU↔장치 양방향 심볼릭 링크를 만들어 관리자가 대응 관계를 볼 수 있게 한다 */
	if (ret)	/* [한국어] 단계 실패 — 아래 라벨로 되감는다 */
		goto err_release;	/* [한국어] 링크 실패 — 드라이버의 probe_device 부터 되돌린다 */

	group = ops->device_group(dev);	/* [한국어] 이 장치가 속할 격리 그룹을 드라이버에게 묻는다. 대개 pci_device_group(ACS/별칭 분석) 이나 generic_device_group(장치당 하나) */
	if (WARN_ON_ONCE(group == NULL))	/* [한국어] NULL 은 계약 위반 — 드라이버는 그룹이나 에러 포인터 중 하나를 줘야 한다 */
		group = ERR_PTR(-EINVAL);	/* [한국어] 드라이버 버그를 공통 에러 경로로 흘려보낸다 */
	if (IS_ERR(group)) {	/* [한국어] 그룹을 만들지 못했다 */
		ret = PTR_ERR(group);	/* [한국어] 이유 추출 */
		goto err_unlink;	/* [한국어] sysfs 링크부터 되돌린다 */
	}
	dev->iommu_group = group;	/* [한국어] 그룹 참조는 여기서 들고, __iommu_group_remove_device 가 놓는다 — 참조의 수명이 두 함수에 걸쳐 있다 */

	dev->iommu->max_pasids = dev_iommu_get_max_pasids(dev);	/* [한국어] 장치와 IOMMU 양쪽 PASID 한계의 교집합을 캐시해 둔다. SVA 부착 때마다 다시 묻지 않기 위해 */
	if (ops->is_attach_deferred)	/* [한국어] 부팅 초기에는 아직 도메인을 하드웨어에 걸 수 없는 장치가 있다 */
		dev->iommu->attach_deferred = ops->is_attach_deferred(dev);	/* [한국어] 그런 장치는 첫 DMA 가 올 때 iommu_deferred_attach 로 뒤늦게 붙인다. 이 플래그가 해제 경로의 동작까지 바꾼다 */
	return 0;	/* [한국어] 장치 초기화 완료 — 이제 그룹에 넣을 수 있다 */

err_unlink:	/* [한국어] 그룹 결정에 실패한 지점의 되감기 */
	iommu_device_unlink(iommu_dev, dev);	/* [한국어] sysfs 링크 제거 */
err_release:	/* [한국어] probe_device 까지 성공한 지점의 되감기 */
	if (ops->release_device)	/* [한국어] 드라이버가 잡은 장치별 자원이 있다면 */
		ops->release_device(dev);	/* [한국어] 놓게 한다 */
err_module_put:	/* [한국어] 모듈 참조만 잡은 지점의 되감기 */
	module_put(ops->owner);	/* [한국어] 드라이버 모듈 참조 반납 */
err_free:	/* [한국어] 장치 상태만 만든 지점의 되감기 */
	dev->iommu->iommu_dev = NULL;	/* [한국어] 먼저 지운다 — 해제 도중 다른 경로가 '유효한 장치'로 오인하지 않게 */
	dev_iommu_free(dev);	/* [한국어] 장치별 상태와 fwspec 해제 */
	return ret;	/* [한국어] 첫 실패 지점의 이유를 그대로 올린다 */
}

/*
 * [한국어]
 * iommu_deinit_device - 장치를 IOMMU 아래에서 안전하게 빼낸다
 *
 * @dev: 그룹 목록에서는 이미 빠졌고, 이제 드라이버와의 연결을 끊을 장치
 *
 * iommu_init_device 의 역순이지만, 단순한 해제가 아니라 "떠나는 장치가 남은
 * 시스템을 해치지 않게 하는" 절차가 들어 있다.
 *
 * 핵심은 퇴역 도메인이다. 장치가 그룹을 떠나면 그룹의 도메인은 곧 해제될 수
 * 있는데, 그 전에 하드웨어가 여전히 그 페이지 테이블을 가리키고 있으면 해제된
 * 메모리를 워크하게 된다. 그래서 드라이버가 release_domain(보통 전역 차단 도메인)
 * 을 지정해 두면 코어가 release_device 를 부르기 전에 장치를 그쪽으로 옮겨 준다.
 * 다만 RMRR/unity map 같은 직통 매핑이 필요한 장치를 차단 도메인에 세우면 펌웨어가
 * 쓰던 버퍼가 끊기므로, 그 경우에만 항등 도메인으로 바꿔 준다.
 *
 * 두 번째 순서 제약은 모듈 참조다. 그룹이 비었다면 도메인들을 먼저 해제해야 하는데
 * 도메인 해제가 드라이버 코드를 부르므로, module_put 은 반드시 그 뒤여야 한다.
 *
 * 실행 컨텍스트: 그룹 락을 든 채. 프로세스 문맥.
 *
 * 호출 체인: __iommu_group_remove_device, __iommu_probe_device 에러 경로 → [이 함수]
 *            → release_domain->ops->attach_dev, ops->release_device, iommu_domain_free
 */
static void iommu_deinit_device(struct device *dev)
{
	struct iommu_group *group = dev->iommu_group;	/* [한국어] 해제 전 그룹을 집어 둔다 — 아래에서 dev->iommu_group 을 NULL 로 만들기 때문 */
	const struct iommu_ops *ops = dev_iommu_ops(dev);	/* [한국어] module_put 으로 모듈을 놓기 전에 ops 포인터를 먼저 확보해 둔다 */

	lockdep_assert_held(&group->mutex);	/* [한국어] 그룹 구성 변경은 반드시 그룹 락 아래에서 */

	iommu_device_unlink(dev->iommu->iommu_dev, dev);	/* [한국어] sysfs 양방향 링크 제거 */

	/*
	 * release_device() must stop using any attached domain on the device.
	 * If there are still other devices in the group, they are not affected
	 * by this callback.
	 *
	 * If the iommu driver provides release_domain, the core code ensures
	 * that domain is attached prior to calling release_device. Drivers can
	 * use this to enforce a translation on the idle iommu. Typically, the
	 * global static blocked_domain is a good choice.
	 *
	 * Otherwise, the iommu driver must set the device to either an identity
	 * or a blocking translation in release_device() and stop using any
	 * domain pointer, as it is going to be freed.
	 *
	 * Regardless, if a delayed attach never occurred, then the release
	 * should still avoid touching any hardware configuration either.
	 */
	if (!dev->iommu->attach_deferred && ops->release_domain) {	/* [한국어] 실제로 하드웨어에 도메인을 걸어 본 적이 있고(지연 부착이 아니었고), 드라이버가 '퇴역용 도메인'을 지정한 경우에만 */
		struct iommu_domain *release_domain = ops->release_domain;	/* [한국어] 보통 전역 blocked_domain — 떠나는 장치의 DMA 를 전부 막아 두는 것이 가장 안전하다 */

		/*
		 * If the device requires direct mappings then it should not
		 * be parked on a BLOCKED domain during release as that would
		 * break the direct mappings.
		 */
		if (dev->iommu->require_direct && ops->identity_domain &&	/* [한국어] 다만 직통 매핑(인텔 RMRR, AMD unity map)이 필요한 장치는 막아 버리면 펌웨어가 계속 쓰는 버퍼로 가는 길이 끊긴다 */
		    release_domain == ops->blocked_domain)	/* [한국어] 차단 도메인으로 보내려던 경우에 한해 */
			release_domain = ops->identity_domain;	/* [한국어] 항등 도메인으로 바꾼다 — 번역 없이 통과시켜 직통 매핑을 유지 */

		release_domain->ops->attach_dev(release_domain, dev,	/* [한국어] 그룹에서 빠지기 직전의 마지막 도메인 전환. 이후 group->domain 은 해제될 수 있으므로 드라이버가 그 포인터를 붙잡고 있으면 안 된다 */
						group->domain);	/* [한국어] 현재 도메인을 함께 넘겨 드라이버가 어디서 어디로 가는 전이인지 알게 한다 */
	}

	if (ops->release_device)	/* [한국어] 드라이버가 장치별 정리를 원하면 */
		ops->release_device(dev);	/* [한국어] 장치 문맥을 놓게 한다. 위 영어 주석대로, release_domain 을 두지 않은 드라이버는 여기서 스스로 항등/차단 상태로 만들어야 한다 */

	/*
	 * If this is the last driver to use the group then we must free the
	 * domains before we do the module_put().
	 */
	if (list_empty(&group->devices)) {	/* [한국어] 이 장치가 그룹의 마지막이었다면 그룹이 들고 있던 도메인들도 함께 정리한다 */
		if (group->default_domain) {	/* [한국어] 기본 도메인이 남아 있으면 */
			iommu_domain_free(group->default_domain);	/* [한국어] 해제는 드라이버 코드를 부르므로 아래 module_put 보다 반드시 먼저 (위 영어 주석) */
			group->default_domain = NULL;	/* [한국어] 빈 그룹 표시 */
		}
		if (group->blocking_domain) {	/* [한국어] 차단 도메인도 지연 생성된 것이 남아 있을 수 있다 */
			iommu_domain_free(group->blocking_domain);	/* [한국어] 같은 이유로 지금 해제 */
			group->blocking_domain = NULL;	/* [한국어] 정리 완료 */
		}
		group->domain = NULL;	/* [한국어] 빈 그룹은 어떤 도메인도 가리키지 않는다 */
	}

	/* Caller must put iommu_group */
	dev->iommu_group = NULL;	/* [한국어] 장치에서 그룹을 끊는다. 참조 반납은 호출자 몫이다 (위 영어 주석) */
	module_put(ops->owner);	/* [한국어] 이제서야 드라이버 모듈을 놓아 준다 — 위의 도메인 해제까지 끝난 뒤여야 한다 */
	dev_iommu_free(dev);	/* [한국어] 장치별 상태 해제 */
#ifdef CONFIG_IOMMU_DMA	/* [한국어] dma_iommu 플래그는 DMA API 통합이 켜진 빌드에만 존재한다 — 꺼져 있으면 이 장치의 DMA 는 애초에 IOMMU 를 지나지 않는다 */
	dev->dma_iommu = false;	/* [한국어] DMA 계층이 다시 직접 매핑(direct/swiotlb)으로 돌아가도록 되돌린다 */
#endif
}

/*
 * [한국어]
 * pasid_array_entry_to_domain - PASID xarray 항목에서 도메인을 꺼낸다
 *
 * @entry:  group->pasid_array 에 저장된 태그 붙은 포인터
 * @return: 그 PASID 에 붙어 있는 도메인
 *
 * 그룹은 PASID 별 부착 상태를 xarray 하나에 담는데, 저장하는 것이 두 종류다.
 * 커널 내부에서 붙였으면 도메인 포인터를 바로 넣고, iommufd 처럼 부착 핸들을
 * 함께 관리해야 하는 사용자면 struct iommu_attach_handle 을 넣는다. 포인터 정렬로
 * 남는 하위 비트에 태그를 심어 둘을 구분하므로, 배열 하나로 두 경우를 모두 담을
 * 수 있다.
 *
 * 실행 컨텍스트: 어디서든. 순수 변환이다.
 *
 * 호출 체인: PASID 부착/해제/조회 경로 → [이 함수]
 */
static struct iommu_domain *pasid_array_entry_to_domain(void *entry)
{
	if (xa_pointer_tag(entry) == IOMMU_PASID_ARRAY_DOMAIN)	/* [한국어] xarray 항목의 하위 태그 비트로 두 가지 저장 형태를 구분한다 — 포인터 정렬 여유 비트를 이렇게 쓴다 */
		return xa_untag_pointer(entry);	/* [한국어] 도메인을 직접 넣은 경우 (커널 내부 PASID 부착) */
	return ((struct iommu_attach_handle *)xa_untag_pointer(entry))->domain;	/* [한국어] iommufd 처럼 핸들을 거쳐 붙인 경우 — 핸들 안에서 도메인을 꺼낸다 */
}

DEFINE_MUTEX(iommu_probe_device_lock);	/* [한국어] 장치 프로브 전 구간을 직렬화하는 전역 락. 드라이버 등록과 ACPI/OF 의 재생(replay) 호출이 동시에 들어와도 한 장치가 두 번 초기화되지 않도록 단순한 전역 순서를 강제한다 */

/*
 * [한국어]
 * __iommu_probe_device - 장치를 그룹에 넣고 도메인까지 연결한다 (프로브의 본체)
 *
 * @dev:        프로브할 장치
 * @group_list: NULL 이 아니면 기본 도메인 설정을 뒤로 미루고, 설정이 필요한 그룹을
 *              중복 없이 이 목록에 모아 준다. 부팅 시 bus_iommu_probe 가 수백 개
 *              장치를 한 번에 훑을 때 그룹마다 한 번씩만 도메인을 세우기 위한 것.
 *              NULL 이면 이 자리에서 즉시 설정한다.
 * @return:     0 성공, 음수 실패 (실패 시 모든 부수 효과가 되감긴 상태)
 *
 * 장치가 IOMMU 아래로 들어오는 전 과정의 조립부다. iommu_init_device 가 드라이버와
 * 그룹을 정해 주면, 이 함수가 그 결과를 그룹 자료구조에 반영하고 도메인을 연결한 뒤
 * 마지막으로 DMA API 를 dma-iommu 로 갈아 끼운다. 마지막 한 줄
 * (iommu_setup_dma_ops)이 지나야 비로소 이 장치의 dma_map_page 가 IOVA 를 돌려주기
 * 시작한다 — NVMe 드라이버 입장에서 "IOMMU 가 켜졌다"의 실제 의미가 그것이다.
 *
 * 도메인 연결에는 세 갈래가 있다.
 *   - 그룹에 이미 도메인이 걸려 있다: 새 장치도 같은 도메인에 붙인다. 그룹이 곧
 *     격리 단위이므로 한 장치만 다른 곳을 볼 수는 없다.
 *   - 새 그룹이고 즉시 처리: iommu_setup_default_domain 으로 종류를 정하고 세운다.
 *   - 새 그룹이고 일괄 처리: 그룹을 group_list 에 올려 두고 물러난다.
 *
 * 실행 컨텍스트: iommu_probe_device_lock 을 든 채. 안에서 그룹 락을 추가로 잡는다.
 * 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: iommu_probe_device, bus_iommu_probe, iommu_bus_notifier → [이 함수]
 *            → iommu_init_device → iommu_setup_default_domain → iommu_setup_dma_ops
 */
static int __iommu_probe_device(struct device *dev, struct list_head *group_list)
{
	struct iommu_group *group;	/* [한국어] 이 장치가 들어갈 그룹 (iommu_init_device 가 정해 준다) */
	struct group_device *gdev;	/* [한국어] 그룹 목록에 넣을 항목. 장치 포인터와 sysfs 링크 이름을 담는다 */
	int ret;	/* [한국어] 각 단계의 실패 코드 */

	/*
	 * Serialise to avoid races between IOMMU drivers registering in
	 * parallel and/or the "replay" calls from ACPI/OF code via client
	 * driver probe. Once the latter have been cleaned up we should
	 * probably be able to use device_lock() here to minimise the scope,
	 * but for now enforcing a simple global ordering is fine.
	 */
	lockdep_assert_held(&iommu_probe_device_lock);	/* [한국어] 전역 프로브 락 아래에서만 — 드라이버 등록과 재생 호출의 경쟁을 막는다 (위 영어 주석) */

	/* Device is probed already if in a group */
	if (dev->iommu_group)	/* [한국어] 그룹이 이미 붙어 있으면 프로브가 끝난 장치다 */
		return 0;	/* [한국어] 성공으로 간주하고 조용히 돌아간다 */

	ret = iommu_init_device(dev);	/* [한국어] 드라이버 찾기 → probe_device → 그룹 결정까지 한 번에 */
	if (ret)	/* [한국어] 장치 초기화 실패 */
		return ret;	/* [한국어] IOMMU 를 못 찾았거나 드라이버가 거절했다 */
	/*
	 * And if we do now see any replay calls, they would indicate someone
	 * misusing the dma_configure path outside bus code.
	 */
	if (dev->driver)	/* [한국어] 드라이버가 이미 붙은 뒤에 IOMMU 프로브가 왔다 */
		dev_WARN(dev, "late IOMMU probe at driver bind, something fishy here!\n");	/* [한국어] 버스 코드 밖에서 dma_configure 경로를 잘못 쓴 신호. 치명적이진 않지만 순서가 뒤집혔음을 남긴다 */

	group = dev->iommu_group;	/* [한국어] iommu_init_device 가 정하고 참조까지 잡아 둔 그룹 */
	gdev = iommu_group_alloc_device(group, dev);	/* [한국어] 그룹 항목과 sysfs 링크를 만든다 — 할당을 그룹 락 밖에서 먼저 끝낸다 */
	mutex_lock(&group->mutex);	/* [한국어] 여기서부터 그룹 구성 변경 구간 */
	if (IS_ERR(gdev)) {	/* [한국어] 항목 생성 실패 */
		ret = PTR_ERR(gdev);	/* [한국어] 이유 추출 */
		goto err_put_group;	/* [한국어] 장치 초기화까지 되돌린다 */
	}

	/*
	 * The gdev must be in the list before calling
	 * iommu_setup_default_domain()
	 */
	list_add_tail(&gdev->list, &group->devices);	/* [한국어] 기본 도메인 설정이 그룹의 모든 장치를 순회하므로, 그 전에 목록에 들어가 있어야 한다 (위 영어 주석) */
	WARN_ON(group->default_domain && !group->domain);	/* [한국어] 기본 도메인이 있는데 현재 도메인이 비어 있는 조합은 존재할 수 없다 */
	if (group->default_domain)	/* [한국어] 그룹이 이미 굴러가는 중이면 */
		iommu_create_device_direct_mappings(group->default_domain, dev);	/* [한국어] 새로 들어온 이 장치가 요구하는 예약 영역(RMRR/unity map)을 기존 도메인에 미리 심는다 — 심지 않으면 펌웨어가 쓰던 버퍼가 끊긴다 */
	if (group->domain) {	/* [한국어] 현재 걸린 도메인이 있으면 새 장치도 같은 곳으로 보내야 그룹의 격리 전제가 유지된다 */
		ret = __iommu_device_set_domain(group, dev, group->domain, NULL,	/* [한국어] 그룹의 현재 도메인에 이 장치만 붙인다 */
						0);	/* [한국어] 일반 실패 정책 — 실패하면 아래에서 그대로 되감는다 */
		if (ret)	/* [한국어] 그룹의 현재 도메인에 붙이지 못했다 */
			goto err_remove_gdev;	/* [한국어] 부착 실패 — 목록에서 다시 뺀다 */
	} else if (!group->default_domain && !group_list) {	/* [한국어] 새로 만들어진 그룹이고, 호출자가 일괄 처리 목록을 주지 않았다 */
		ret = iommu_setup_default_domain(group, 0);	/* [한국어] 지금 이 자리에서 기본 도메인을 정하고 세운다 */
		if (ret)	/* [한국어] 기본 도메인 설정 실패 */
			goto err_remove_gdev;	/* [한국어] 도메인 설정 실패 */
	} else if (!group->default_domain) {	/* [한국어] 일괄 처리 경로 — 도메인 결정을 호출자에게 미룬다 */
		/*
		 * With a group_list argument we defer the default_domain setup
		 * to the caller by providing a de-duplicated list of groups
		 * that need further setup.
		 */
		if (list_empty(&group->entry))	/* [한국어] 같은 그룹이 목록에 두 번 들어가지 않도록 (여러 장치가 한 그룹을 공유한다) */
			list_add_tail(&group->entry, group_list);	/* [한국어] bus_iommu_probe 가 나중에 그룹 단위로 한 번씩 처리한다 */
	}

	if (group->default_domain)	/* [한국어] 도메인이 실제로 정해진 경우에만 */
		iommu_setup_dma_ops(dev, group->default_domain);	/* [한국어] 이 장치의 dma_map_* 을 dma-iommu 구현으로 갈아 끼운다. 이 줄 이후 드라이버가 내는 DMA 주소는 IOVA 가 되고 IOMMU 를 지나게 된다 */

	mutex_unlock(&group->mutex);	/* [한국어] 그룹 구성 변경 끝 */

	return 0;	/* [한국어] 장치가 그룹에 들어가고 번역까지 준비됐다 */

err_remove_gdev:	/* [한국어] 그룹 목록에 넣은 뒤 실패한 경우 */
	list_del(&gdev->list);	/* [한국어] 목록에서 다시 뺀다 */
	__iommu_group_free_device(group, gdev);	/* [한국어] sysfs 링크와 항목 해제 */
err_put_group:	/* [한국어] 항목조차 못 만든 경우가 합류하는 지점 */
	iommu_deinit_device(dev);	/* [한국어] iommu_init_device 가 한 일을 전부 되돌린다 (드라이버 해제·모듈 참조 반납 포함) */
	mutex_unlock(&group->mutex);	/* [한국어] 되감기를 마치고 락 해제 */
	iommu_group_put(group);	/* [한국어] iommu_init_device 가 잡아 둔 그룹 참조를 놓는다 — 마지막 장치였다면 여기서 그룹이 사라진다 */

	return ret;	/* [한국어] 첫 실패 이유 전달 */
}

/*
 * [한국어]
 * iommu_probe_device - 장치 하나를 IOMMU 아래로 들인다 (외부 진입점)
 *
 * @dev:    새로 나타난 장치
 * @return: 0 성공, 음수 실패
 *
 * 부팅이 끝난 뒤 장치가 하나씩 추가될 때 쓰이는 얇은 겉면이다. 버스 알림
 * (iommu_bus_notifier)과 VFIO/셀프테스트가 여기로 들어온다. group_list 에 NULL 을
 * 넘기므로 기본 도메인 설정까지 이 자리에서 끝난다 — 부팅 때의 일괄 처리와 대비되는
 * 점이다.
 *
 * 락을 놓은 뒤에 probe_finalize 를 부르는 순서가 의도적이다. 이 콜백은 드라이버가
 * 도메인이 선 뒤에 마무리 작업을 하는 곳이라 다시 프로브 경로로 재진입할 수 있고,
 * 전역 락을 든 채로 부르면 교착이 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 *
 * 호출 체인: iommu_bus_notifier(BUS_NOTIFY_ADD_DEVICE), VFIO → [이 함수]
 *            → __iommu_probe_device → ops->probe_finalize
 */
int iommu_probe_device(struct device *dev)
{
	const struct iommu_ops *ops;	/* [한국어] probe_finalize 를 부르기 위해 필요 */
	int ret;	/* [한국어] 프로브 결과 */

	mutex_lock(&iommu_probe_device_lock);	/* [한국어] 전역 직렬화 */
	ret = __iommu_probe_device(dev, NULL);	/* [한국어] group_list 로 NULL 을 넘겨, 기본 도메인 설정까지 이 자리에서 끝내게 한다 (부팅 후 개별 장치 추가 경로) */
	mutex_unlock(&iommu_probe_device_lock);	/* [한국어] 락은 여기까지 — 아래 콜백이 다시 프로브 경로로 들어갈 수 있다 */
	if (ret)	/* [한국어] 프로브 실패 — probe_finalize 를 부르지 않는다 */
		return ret;	/* [한국어] 프로브 실패 전달 */

	ops = dev_iommu_ops(dev);	/* [한국어] 이제 드라이버가 확정되었으므로 ops 를 꺼낼 수 있다 */
	if (ops->probe_finalize)	/* [한국어] 도메인까지 선 뒤에 마무리할 일이 남은 드라이버가 있다 */
		ops->probe_finalize(dev);	/* [한국어] 전역 락 밖에서 부른다 — 이 콜백이 재진입할 수 있기 때문 */

	return 0;	/* [한국어] 장치가 IOMMU 아래로 완전히 들어왔다 */
}

/*
 * [한국어]
 * __iommu_group_free_device - 그룹 항목 하나와 그에 딸린 sysfs 링크를 거둔다
 *
 * @group:    항목이 속한 그룹
 * @grp_dev:  이미 group->devices 목록에서 빠진 항목
 *
 * 목록에서 빼는 것은 호출자의 일이고, 이 함수는 그 항목이 남긴 흔적 — 양방향
 * sysfs 링크와 이름 문자열 — 만 정리한다. 둘을 나눠 둔 덕분에 프로브 실패
 * 되감기와 정상 제거가 같은 정리 코드를 공유한다.
 *
 * 그룹이 비는 순간의 WARN 두 개가 중요한 불변식을 지킨다. 마지막 장치가 나갈 때
 * 소유권 계수가 남아 있거나 도메인이 기본값이 아니라면, VFIO 등이 소유권을 놓지
 * 않은 채 장치가 사라졌다는 뜻이다.
 *
 * 실행 컨텍스트: 그룹 락을 든 채. 프로세스 문맥.
 *
 * 호출 체인: __iommu_group_remove_device, __iommu_probe_device 에러 경로 → [이 함수]
 */
static void __iommu_group_free_device(struct iommu_group *group,
				      struct group_device *grp_dev)
{
	struct device *dev = grp_dev->dev;	/* [한국어] 항목이 가리키는 실제 장치 */

	sysfs_remove_link(group->devices_kobj, grp_dev->name);	/* [한국어] 그룹의 devices/ 아래에 걸었던 링크 제거 */
	sysfs_remove_link(&dev->kobj, "iommu_group");	/* [한국어] 장치 쪽에서 그룹을 가리키던 역링크 제거 */

	trace_remove_device_from_group(group->id, dev);	/* [한국어] 그룹 구성 변화를 ftrace 로 남긴다 */

	/*
	 * If the group has become empty then ownership must have been
	 * released, and the current domain must be set back to NULL or
	 * the default domain.
	 */
	if (list_empty(&group->devices))	/* [한국어] 마지막 장치를 빼는 중이라면 정리가 순서대로 끝났는지 확인한다 */
		WARN_ON(group->owner_cnt ||	/* [한국어] 아직 DMA 소유권을 든 사용자(VFIO/iommufd)가 남아 있으면 안 되고 */
			group->domain != group->default_domain);	/* [한국어] 도메인도 기본값으로 되돌아와 있어야 한다 */

	kfree(grp_dev->name);	/* [한국어] sysfs 링크 이름 문자열 해제 */
	kfree(grp_dev);	/* [한국어] 항목 본체 해제 */
}

/* Remove the iommu_group from the struct device. */
/*
 * [한국어] (위 영어 주석에 이어)
 * __iommu_group_remove_device - 장치를 그룹에서 빼고 참조를 반납한다
 *
 * @dev: 사라지는 장치
 *
 * 그룹 목록을 훑어 이 장치의 항목을 찾고, 목록에서 뺀 뒤 sysfs 를 정리하고,
 * IOMMU 드라이버가 붙어 있었다면 iommu_deinit_device 로 드라이버 쪽까지 되돌린다.
 *
 * dev_has_iommu 로 갈라지는 부분이 이 함수의 요점이다. VFIO 가 iommu_group_add_device
 * 로 직접 넣은 장치는 IOMMU 드라이버가 없으므로 ops 를 부르면 안 되고, 연결만 끊는다.
 *
 * 마지막 iommu_group_put 은 iommu_init_device 나 iommu_group_add_device 가 잡아 둔
 * 참조와 짝을 이룬다. 그것이 그룹의 마지막 참조였다면 이 호출로 그룹 자체가 해제된다.
 *
 * 실행 컨텍스트: 장치 제거 경로. 프로세스 문맥.
 *
 * 호출 체인: iommu_release_device → [이 함수] → iommu_deinit_device, iommu_group_put
 */
static void __iommu_group_remove_device(struct device *dev)
{
	struct iommu_group *group = dev->iommu_group;	/* [한국어] 장치가 속한 그룹 */
	struct group_device *device;	/* [한국어] 순회용 커서 */

	mutex_lock(&group->mutex);	/* [한국어] 그룹 구성 변경 구간 */
	for_each_group_device(group, device) {	/* [한국어] 그룹의 장치 목록에서 이 장치의 항목을 찾는다 */
		if (device->dev != dev)	/* [한국어] 다른 장치의 항목이면 */
			continue;	/* [한국어] 건너뛴다 */

		list_del(&device->list);	/* [한국어] 먼저 목록에서 빼 다른 순회가 보지 못하게 한다 */
		__iommu_group_free_device(group, device);	/* [한국어] sysfs 링크와 항목 해제 */
		if (dev_has_iommu(dev))	/* [한국어] 실제 IOMMU 드라이버가 붙어 있던 장치라면 */
			iommu_deinit_device(dev);	/* [한국어] 드라이버 해제·퇴역 도메인 전환·도메인 정리까지 (여기서 dev->iommu_group 도 NULL 이 된다) */
		else
			dev->iommu_group = NULL;	/* [한국어] VFIO 가 수동으로 넣었던 장치는 연결만 끊으면 된다 */
		break;	/* [한국어] 찾았으므로 순회 종료 */
	}
	mutex_unlock(&group->mutex);	/* [한국어] 구성 변경 끝 */

	/*
	 * Pairs with the get in iommu_init_device() or
	 * iommu_group_add_device()
	 */
	iommu_group_put(group);	/* [한국어] iommu_init_device 또는 iommu_group_add_device 가 잡았던 참조를 여기서 놓는다 (위 영어 주석). 마지막 장치였다면 이 호출로 그룹이 해제된다 */
}

/*
 * [한국어]
 * iommu_release_device - 장치가 시스템에서 사라질 때 IOMMU 흔적을 모두 지운다
 *
 * @dev: 제거되는 장치
 *
 * 버스 알림의 BUS_NOTIFY_REMOVED_DEVICE 가 도달하는 곳이다. 두 가지 상태를 모두
 * 처리해야 한다 — 프로브에 성공해 그룹에 들어간 장치와, fwspec 까지만 만들어지고
 * 드라이버를 못 찾아 그대로 남은 장치다. 후자는 그룹이 없으므로 dev->iommu 만
 * 거두면 된다.
 *
 * 실행 컨텍스트: 장치 제거 알림. 프로세스 문맥.
 *
 * 호출 체인: iommu_bus_notifier → [이 함수] → __iommu_group_remove_device / dev_iommu_free
 */
static void iommu_release_device(struct device *dev)
{
	struct iommu_group *group = dev->iommu_group;	/* [한국어] 그룹에 들어가 있었는지 확인 */

	if (group)	/* [한국어] 그룹 소속이면 */
		__iommu_group_remove_device(dev);	/* [한국어] 그룹 경로로 정리한다 (장치 상태 해제까지 그 안에서) */

	/* Free any fwspec if no iommu_driver was ever attached */
	if (dev->iommu)	/* [한국어] 드라이버가 끝내 붙지 않아 fwspec 만 남은 장치 */
		dev_iommu_free(dev);	/* [한국어] 그 잔여 상태를 거둔다 (위 영어 주석) */
}

/*
 * [한국어]
 * iommu_set_def_domain_type - "iommu.passthrough=" 부트 인자를 처리한다
 *
 * @str:    인자 값 문자열
 * @return: 0 성공, 음수면 값 해석 실패
 *
 * 기본 도메인 종류를 정하는 세 층 — 빌드 설정, 부트 인자, 커널 오버라이드 — 중
 * 가운데 층의 입구다. passthrough=1 이면 장치가 물리 주소를 그대로 내는 항등
 * 도메인이 기본이 되어 DMA 오버헤드가 사라지지만 격리도 함께 사라진다.
 * passthrough=0 이면 반대로 번역을 강제한다.
 *
 * 어느 쪽이든 설정 함수에 true(cmd_line)를 넘기므로 IOMMU_CMD_LINE_DMA_API 가
 * 서고, 이후 드라이버가 def_domain_type 으로 다른 선호를 내도 이 결정이 이긴다.
 * 관리자가 명시한 것을 커널이 조용히 뒤집지 않는다는 원칙이다.
 *
 * 실행 컨텍스트: early_param — 초기 부팅, 어떤 IOMMU 드라이버보다 먼저.
 *
 * 호출 체인: 부트 인자 파서 → [이 함수] → iommu_set_default_passthrough/translated
 */
static int __init iommu_set_def_domain_type(char *str)
{
	bool pt;	/* [한국어] 패스스루 여부 */
	int ret;	/* [한국어] 파싱 결과 */

	ret = kstrtobool(str, &pt);	/* [한국어] '1'/'y'/'on' 등을 불리언으로 해석 */
	if (ret)	/* [한국어] 부트 인자 값을 해석하지 못했다 */
		return ret;	/* [한국어] 해석 불가한 값이면 부트 인자 자체를 거절 */

	if (pt)	/* [한국어] iommu.passthrough=1 */
		iommu_set_default_passthrough(true);	/* [한국어] 기본 도메인을 항등(번역 없음)으로. DMA 성능은 최대지만 격리가 사라진다 */
	else
		iommu_set_default_translated(true);	/* [한국어] iommu.passthrough=0 — 기본 도메인을 번역형으로 강제한다 */

	return 0;	/* [한국어] 부트 인자 처리 완료 */
}
early_param("iommu.passthrough", iommu_set_def_domain_type);	/* [한국어] 부트 인자 등록. 두 설정 함수에 true 를 넘겨 IOMMU_CMD_LINE_DMA_API 를 세우므로, 이후 빌드 설정이나 드라이버 선호를 모두 이긴다 */

/*
 * [한국어]
 * iommu_dma_setup - "iommu.strict=" 부트 인자를 처리한다
 *
 * @str:    인자 값 문자열
 * @return: 0 성공, 음수면 값 해석 실패
 *
 * 무효화 정책을 고른다. strict=1 이면 매 dma_unmap 마다 IOTLB 무효화를 끝내고
 * 돌아오므로, 해제된 IOVA 로 오는 DMA 가 옛 페이지에 닿는 창이 아예 없다.
 * strict=0 이면 flush queue(DMA_FQ)에 모았다가 한꺼번에 무효화해 처리량을 크게
 * 올리는 대신 그 창을 허용한다.
 *
 * IOMMU_CMD_LINE_STRICT 를 세워 두는 것이 요점이다. 나중에 드라이버가
 * iommu_set_dma_strict 를 부르더라도 관리자가 명시한 값이 우선한다.
 *
 * 실행 컨텍스트: early_param — 초기 부팅.
 *
 * 호출 체인: 부트 인자 파서 → [이 함수]
 */
static int __init iommu_dma_setup(char *str)
{
	int ret = kstrtobool(str, &iommu_dma_strict);	/* [한국어] strict=1 이면 해제 때마다 즉시 IOTLB 무효화, 0 이면 flush queue 로 지연 무효화 */

	if (!ret)	/* [한국어] 값을 제대로 읽었다면 */
		iommu_cmd_line |= IOMMU_CMD_LINE_STRICT;	/* [한국어] 명령줄이 정했음을 기록해 둔다 — 나중에 드라이버나 빌드 기본값이 이 결정을 덮어쓰지 못하게 하는 표식 */
	return ret;	/* [한국어] 파싱 결과 반환 */
}
early_param("iommu.strict", iommu_dma_setup);	/* [한국어] 부트 인자 등록 */

/*
 * [한국어]
 * iommu_set_dma_strict - 지연 무효화를 끄고 즉시 무효화로 되돌린다
 *
 * 드라이버나 보안 정책이 flush queue 를 허용할 수 없을 때 부른다. 하드웨어 제약
 * (일부 IOMMU 는 범위 무효화를 제대로 못 한다)이나, 기밀 컴퓨팅처럼 해제 후 잔여
 * 번역을 절대 남길 수 없는 환경이 그런 경우다.
 *
 * 전역 플래그만 바꾸는 것으로는 부족하다. 기본 도메인 종류가 이미 DMA_FQ 로
 * 정해져 있었다면 그것도 DMA 로 내려야 한다 — 둘은 같은 번역 도메인의 두 가지
 * 무효화 정책이고, 종류가 FQ 로 남아 있으면 도메인이 여전히 큐를 쓴다.
 *
 * 실행 컨텍스트: 부팅 중 드라이버 초기화. 프로세스 문맥.
 *
 * 호출 체인: 벤더 드라이버 초기화, 보안 정책 → [이 함수]
 */
void iommu_set_dma_strict(void)
{
	iommu_dma_strict = true;	/* [한국어] 드라이버나 보안 정책이 지연 무효화를 허용하지 않을 때 코어가 부른다 */
	if (iommu_def_domain_type == IOMMU_DOMAIN_DMA_FQ)	/* [한국어] 이미 flush queue 형 기본 도메인으로 정해져 있었다면 */
		iommu_def_domain_type = IOMMU_DOMAIN_DMA;	/* [한국어] 즉시 무효화형으로 내린다. DMA 와 DMA_FQ 는 같은 번역 도메인의 두 가지 무효화 정책일 뿐이다 */
}

/*
 * [한국어]
 * iommu_group_attr_show - sysfs 읽기를 그룹 전용 콜백으로 넘긴다
 *
 * @kobj:   읽히는 sysfs 객체. 그룹이 품고 있는 것이다.
 * @__attr: 일반 attribute. 우리 확장형 안에 박혀 있다.
 * @buf:    출력 버퍼(PAGE_SIZE)
 * @return: 쓴 바이트 수. 콜백이 없으면 -EIO.
 *
 * sysfs 는 kobject 와 attribute 만 알고 iommu_group 을 모른다. 이 함수가
 * 그 사이를 잇는 어댑터이며, container_of 두 번으로 양쪽을 우리 형으로
 * 되돌린 뒤 실제 콜백을 부른다.
 *
 * 콜백이 없을 때 -EIO 인 것에 주의할 것. 읽기 전용/쓰기 전용 속성이
 * 반대 방향으로 열렸다는 뜻이고, 권한 비트가 이미 막고 있어야 하는
 * 상황이라 정상 경로에서는 오지 않는다.
 *
 * 실행 컨텍스트: 사용자 공간 read(). 잠들 수 있다.
 *
 * 호출 체인: sysfs → sysfs_ops.show → [이 함수] → 각 속성의 show
 */
static ssize_t iommu_group_attr_show(struct kobject *kobj,
				     struct attribute *__attr, char *buf)
{
	struct iommu_group_attribute *attr = to_iommu_group_attr(__attr);	/* [한국어] 일반 attribute 에서 확장형으로 */
	struct iommu_group *group = to_iommu_group(kobj);	/* [한국어] kobject 에서 그룹으로 */
	ssize_t ret = -EIO;	/* [한국어] show 가 없는 속성이면 이 값이 그대로 나간다 */

	if (attr->show)	/* [한국어] 읽기 콜백이 있는 속성만 */
		ret = attr->show(group, buf);	/* [한국어] 실제 구현은 각 속성이 가지고 있다 */
	return ret;	/* [한국어] 콜백이 쓴 바이트 수 또는 -EIO */
}

/*
 * [한국어]
 * iommu_group_attr_store - sysfs 쓰기를 그룹 전용 콜백으로 넘긴다
 *
 * @kobj:   쓰이는 sysfs 객체
 * @__attr: 일반 attribute
 * @buf:    사용자가 쓴 내용
 * @count:  그 길이
 * @return: 소비한 바이트 수. 콜백이 없으면 -EIO.
 *
 * show 쪽과 대칭인 어댑터다. 쓰기 가능한 그룹 속성은 지금 type 하나뿐이며,
 * 그것이 기본 도메인 종류를 런타임에 바꾸는 통로다.
 *
 * 실행 컨텍스트: 사용자 공간 write(). 잠들 수 있다.
 *
 * 호출 체인: sysfs → sysfs_ops.store → [이 함수] → 각 속성의 store
 */
static ssize_t iommu_group_attr_store(struct kobject *kobj,
				      struct attribute *__attr,
				      const char *buf, size_t count)
{
	struct iommu_group_attribute *attr = to_iommu_group_attr(__attr);	/* [한국어] 일반 attribute 에서 확장형으로 (container_of) */
	struct iommu_group *group = to_iommu_group(kobj);	/* [한국어] kobject 에서 그룹으로 */
	ssize_t ret = -EIO;	/* [한국어] 읽기 전용 속성에 쓰려 했다는 뜻 */

	if (attr->store)	/* [한국어] 쓰기 콜백이 있는 속성만 — 현재로선 type 뿐이다 */
		ret = attr->store(group, buf, count);	/* [한국어] 기본 도메인 종류 변경 같은 실제 동작이 여기서 일어난다 */
	return ret;	/* [한국어] 소비한 바이트 수 또는 -EIO */
}

/* [한국어] 위 두 어댑터를 kobject 계층에 연결하는 vtable.
 * 이것이 있어야 sysfs 가 그룹 속성을 다룰 수 있다. */
static const struct sysfs_ops iommu_group_sysfs_ops = {
	.show = iommu_group_attr_show,
	.store = iommu_group_attr_store,
};

/*
 * [한국어]
 * iommu_group_create_file - 그룹 디렉토리에 속성 파일 하나를 만든다
 *
 * @group: 대상 그룹
 * @attr:  만들 속성
 * @return: 0 이면 성공, 음수 errno
 *
 * sysfs_create_file 을 감싸는 한 줄이다. 감싸는 이유는 호출부가
 * iommu_group_attribute 를 그대로 넘길 수 있게 하려는 것 -- 안쪽의
 * attr.attr 을 꺼내는 일을 여기 한 곳에 모은다.
 *
 * 호출 체인: iommu_group_alloc → [이 함수]
 */
static int iommu_group_create_file(struct iommu_group *group,
				   struct iommu_group_attribute *attr)
{
	return sysfs_create_file(&group->kobj, &attr->attr);	/* [한국어] 확장형 안의 공통 헤더만 sysfs 에 넘긴다 */
}

/*
 * [한국어]
 * iommu_group_remove_file - 그 짝
 *
 * @group: 대상 그룹
 * @attr:  지울 속성
 * @return: 없음
 *
 * 호출 체인: 그룹 해체 경로 → [이 함수]
 */
static void iommu_group_remove_file(struct iommu_group *group,
				    struct iommu_group_attribute *attr)
{
	sysfs_remove_file(&group->kobj, &attr->attr);
}

/*
 * [한국어]
 * iommu_group_show_name - sysfs 의 그룹 name 속성 읽기
 *
 * @group:  대상 그룹
 * @buf:    출력 버퍼
 * @return: 쓴 바이트 수
 *
 * 이름은 벤더 드라이버가 iommu_group_set_name 으로 붙인 것이고, 붙이지 않았으면
 * 이 속성 파일 자체가 만들어지지 않는다. 그래서 여기서는 NULL 검사가 없다 —
 * 파일이 존재한다는 사실이 이름이 있다는 증거다.
 *
 * 실행 컨텍스트: 사용자 공간 read(). 잠들 수 있다.
 *
 * 호출 체인: sysfs → iommu_group_attr_show → [이 함수]
 */
static ssize_t iommu_group_show_name(struct iommu_group *group, char *buf)
{
	return sysfs_emit(buf, "%s\n", group->name);	/* [한국어] 그룹 이름은 벤더 드라이버가 iommu_group_set_name 으로 붙인 것이며, 없으면 이 속성 자체가 sysfs 에 나타나지 않는다 */
}

/**
 * iommu_insert_resv_region - Insert a new region in the
 * list of reserved regions.
 * @new: new region to insert
 * @regions: list of regions
 *
 * Elements are sorted by start address and overlapping segments
 * of the same type are merged.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_insert_resv_region - 예약 구간 하나를 정렬 삽입하고 겹치는 것을 병합한다
 *
 * @new:     넣을 구간. 이 함수가 복제하므로 호출자는 원본을 계속 소유한다.
 * @regions: 대상 목록. 시작 주소 오름차순으로 정렬된 상태가 유지된다.
 * @return:  0 성공, -ENOMEM 이면 복제 실패
 *
 * 예약 구간(reserved region)은 "IOMMU 가 마음대로 쓰면 안 되는 IOVA 범위"다.
 * 종류가 몇 가지 있고 의미가 각각 다르다.
 *   - DIRECT: 펌웨어가 계속 쓰는 버퍼라 물리 주소 = IOVA 로 직통 매핑해야 한다
 *     (인텔 RMRR, AMD unity map). 지우면 USB 컨트롤러나 관리 엔진이 죽는다.
 *   - DIRECT_RELAXABLE: 직통이 바람직하지만 필수는 아니다.
 *   - MSI / SW_MSI: 인터럽트 메시지가 향하는 주소 창. 데이터 DMA 로 이 주소를
 *     덮으면 가짜 인터럽트를 만들 수 있어 반드시 비워 둔다.
 *   - RESERVED: 그 외 쓰면 안 되는 구간.
 *
 * 알고리즘은 두 패스다. 먼저 시작 주소 기준으로 자리를 찾아 넣고, 그 다음 목록
 * 전체를 훑으며 같은 종류끼리 겹치거나 맞닿은 구간을 하나로 합친다. 병합 패스는
 * 임시 스택을 써서, 종류가 섞여 있어도 같은 종류의 직전 구간만 상대로 삼는다.
 * 판정에 top_end + 1 이 쓰이는 것에 주의할 것 — 딱 붙은 두 구간도 하나로 합친다.
 *
 * 실행 컨텍스트: 프로세스 문맥. GFP_KERNEL 로 할당하므로 잠들 수 있다.
 *
 * 호출 체인: iommu_insert_device_resv_regions → [이 함수] → iommu_alloc_resv_region
 */
static int iommu_insert_resv_region(struct iommu_resv_region *new,
				    struct list_head *regions)
{
	struct iommu_resv_region *iter, *tmp, *nr, *top;	/* [한국어] iter/tmp: 목록 순회용, nr: 새로 복제한 항목, top: 스택 맨 위의 같은 종류 항목 */
	LIST_HEAD(stack);	/* [한국어] 병합 결과를 쌓아 두는 임시 목록. 원본 목록을 훑으며 하나씩 옮겨 담고 마지막에 통째로 되돌려 놓는다 */

	nr = iommu_alloc_resv_region(new->start, new->length,	/* [한국어] 호출자의 항목을 그대로 넣지 않고 복제한다 — 원본은 장치별 목록의 소유이고 곧 iommu_put_resv_regions 로 해제되기 때문 */
				     new->prot, new->type, GFP_KERNEL);	/* [한국어] 보호 비트와 종류(DIRECT/DIRECT_RELAXABLE/MSI/RESERVED/SW_MSI)까지 그대로 복사 */
	if (!nr)	/* [한국어] 복제 실패 */
		return -ENOMEM;	/* [한국어] 호출자가 병합을 중단한다 */

	/* First add the new element based on start address sorting */
	list_for_each_entry(iter, regions, list) {	/* [한국어] 시작 주소 오름차순으로 삽입 위치를 찾는다 */
		if (nr->start < iter->start ||	/* [한국어] 이 항목보다 앞서면 여기가 자리이고 */
		    (nr->start == iter->start && nr->type <= iter->type))	/* [한국어] 시작이 같으면 종류 값이 작은 쪽을 앞에 둬 정렬을 안정화한다 */
			break;	/* [한국어] 삽입 위치 확정 */
	}
	list_add_tail(&nr->list, &iter->list);	/* [한국어] iter 바로 앞에 넣는다. 끝까지 갔다면 iter 는 헤드이므로 목록 맨 뒤가 된다 */

	/* Merge overlapping segments of type nr->type in @regions, if any */
	list_for_each_entry_safe(iter, tmp, regions, list) {	/* [한국어] 정렬된 목록을 앞에서부터 훑으며 같은 종류끼리 겹치거나 맞닿은 구간을 합친다. 순회 중 항목을 지우므로 _safe 판 */
		phys_addr_t top_end, iter_end = iter->start + iter->length - 1;	/* [한국어] 각 구간의 마지막 주소 (start + length - 1). 경계 계산을 닫힌 구간으로 통일한다 */

		/* no merge needed on elements of different types than @new */
		if (iter->type != new->type) {	/* [한국어] 이번에 넣은 것과 종류가 다른 구간은 병합 대상이 아니다 */
			list_move_tail(&iter->list, &stack);	/* [한국어] 건드리지 않고 스택으로 옮긴다 */
			continue;	/* [한국어] 다음 항목으로 */
		}

		/* look for the last stack element of same type as @iter */
		list_for_each_entry_reverse(top, &stack, list)	/* [한국어] 스택을 뒤에서부터 훑어 같은 종류의 가장 최근 항목을 찾는다 — 종류가 섞여 있어도 같은 종류끼리만 병합하기 위한 것 */
			if (top->type == iter->type)	/* [한국어] 같은 종류를 찾았다 */
				goto check_overlap;	/* [한국어] 겹침 여부 판정으로 */

		list_move_tail(&iter->list, &stack);	/* [한국어] 같은 종류가 스택에 없다 = 이 종류의 첫 구간이므로 그대로 쌓는다 */
		continue;	/* [한국어] 다음 항목으로 */

check_overlap:	/* [한국어] 스택 맨 위의 같은 종류 구간과 겹치는지 보는 지점 */
		top_end = top->start + top->length - 1;	/* [한국어] 스택 위 구간의 마지막 주소 */

		if (iter->start > top_end + 1) {	/* [한국어] 한 바이트라도 떨어져 있으면 별개 구간이다 (+1 이 있어 '맞닿은' 경우는 병합된다) */
			list_move_tail(&iter->list, &stack);	/* [한국어] 합치지 않고 그대로 쌓는다 */
		} else {
			top->length = max(top_end, iter_end) - top->start + 1;	/* [한국어] 겹치거나 맞닿았다 — 스택 위 구간을 늘려 둘을 하나로 만든다. iter 가 top 에 완전히 포함될 수도 있어 max 로 끝을 고른다 */
			list_del(&iter->list);	/* [한국어] 흡수된 항목을 목록에서 뺀다 */
			kfree(iter);	/* [한국어] 복제본이므로 여기서 해제해도 안전하다 */
		}
	}
	list_splice(&stack, regions);	/* [한국어] 병합이 끝난 스택을 원본 목록 자리로 되돌린다. 스택에 담긴 순서가 곧 정렬 순서다 */
	return 0;	/* [한국어] 삽입·병합 완료 */
}

/*
 * [한국어]
 * iommu_insert_device_resv_regions - 장치 하나의 예약 구간들을 그룹 목록에 합친다
 *
 * @dev_resv_regions:   드라이버가 방금 채워 준 장치별 목록
 * @group_resv_regions: 누적 중인 그룹 전체 목록
 * @return:             0 성공, 음수면 삽입 도중 실패
 *
 * 얇은 반복문이지만 소유권 관점에서 의미가 있다. 장치별 목록은 드라이버 소유라
 * 곧 iommu_put_resv_regions 로 돌아가야 하므로, 여기서 각 항목을 그룹 목록으로
 * 복제해 옮긴다.
 *
 * 실행 컨텍스트: 그룹 락을 든 채. 프로세스 문맥.
 *
 * 호출 체인: iommu_get_group_resv_regions → [이 함수] → iommu_insert_resv_region
 */
static int
iommu_insert_device_resv_regions(struct list_head *dev_resv_regions,
				 struct list_head *group_resv_regions)
{
	struct iommu_resv_region *entry;	/* [한국어] 장치 하나가 알린 예약 구간들을 훑는 커서 */
	int ret = 0;	/* [한국어] 병합 중 발생한 실패 */

	list_for_each_entry(entry, dev_resv_regions, list) {	/* [한국어] 이 장치의 예약 구간을 하나씩 */
		ret = iommu_insert_resv_region(entry, group_resv_regions);	/* [한국어] 그룹 전체 목록에 정렬 삽입하며 겹치는 것은 합친다 */
		if (ret)	/* [한국어] 할당 실패 등 */
			break;	/* [한국어] 더 넣지 않고 중단 — 이미 넣은 것은 호출자가 목록째 처리한다 */
	}
	return ret;	/* [한국어] 성공이면 0 */
}

/*
 * [한국어]
 * iommu_get_group_resv_regions - 그룹 전체가 요구하는 예약 구간의 합집합을 만든다
 *
 * @group:  대상 그룹
 * @head:   결과를 담을 빈 목록 (호출자가 초기화해 넘긴다)
 * @return: 0 성공, 음수면 도중 실패 (부분 결과가 head 에 남을 수 있다)
 *
 * 왜 장치가 아니라 그룹 단위인가 — 그룹이 곧 하나의 주소 공간을 공유하는 단위이기
 * 때문이다. 같은 그룹의 어떤 장치가 특정 IOVA 를 직통으로 요구하면, 그 도메인을
 * 함께 쓰는 다른 장치에게도 그 구간은 그대로 보인다. 따라서 "이 주소 공간에서
 * 자유롭게 쓸 수 없는 범위"는 언제나 그룹 전체의 합집합이다.
 *
 * VFIO/iommufd 가 이 목록을 읽어 게스트에게 내줄 IOVA 범위에서 이 구간들을 빼고,
 * dma-iommu 의 IOVA 할당자도 같은 정보로 예약 영역을 피한다.
 *
 * 실행 컨텍스트: 사용자 공간 read() 또는 VFIO ioctl. 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: sysfs reserved_regions, VFIO/iommufd → [이 함수]
 *            → ops->get_resv_regions → iommu_insert_resv_region
 */
int iommu_get_group_resv_regions(struct iommu_group *group,
				 struct list_head *head)
{
	struct group_device *device;	/* [한국어] 그룹의 장치들을 훑는 커서 */
	int ret = 0;	/* [한국어] 수집 결과 */

	mutex_lock(&group->mutex);	/* [한국어] 그룹 장치 목록 보호 */
	for_each_group_device(group, device) {	/* [한국어] 그룹은 함께 격리되는 단위이므로 예약 구간도 그룹 전체의 합집합이어야 한다 — 한 장치가 요구하는 직통 구간은 같은 도메인을 쓰는 다른 장치에게도 그대로 보인다 */
		struct list_head dev_resv_regions;	/* [한국어] 장치 하나가 알리는 구간을 잠시 담는 목록 */

		/*
		 * Non-API groups still expose reserved_regions in sysfs,
		 * so filter out calls that get here that way.
		 */
		if (!dev_has_iommu(device->dev))	/* [한국어] VFIO 가 수동 구성한 그룹의 장치는 IOMMU 드라이버가 없어 ops 를 부를 수 없다 (위 영어 주석) */
			break;	/* [한국어] 그런 그룹은 예약 구간 개념 자체가 없으므로 중단 */

		INIT_LIST_HEAD(&dev_resv_regions);	/* [한국어] 빈 목록으로 시작 */
		iommu_get_resv_regions(device->dev, &dev_resv_regions);	/* [한국어] 드라이버에게 이 장치의 예약 구간을 묻는다 — 인텔의 RMRR, AMD 의 unity map, MSI 창 등이 여기서 나온다 */
		ret = iommu_insert_device_resv_regions(&dev_resv_regions, head);	/* [한국어] 그룹 목록에 정렬 삽입·병합 */
		iommu_put_resv_regions(device->dev, &dev_resv_regions);	/* [한국어] 장치별 목록은 복제가 끝났으므로 곧바로 돌려준다 */
		if (ret)	/* [한국어] 삽입 실패 */
			break;	/* [한국어] 순회 중단 */
	}
	mutex_unlock(&group->mutex);	/* [한국어] 목록 보호 해제 */
	return ret;	/* [한국어] 0 이면 head 에 그룹 전체 예약 구간이 정렬·병합되어 담겨 있다 */
}
EXPORT_SYMBOL_GPL(iommu_get_group_resv_regions);

/*
 * [한국어]
 * iommu_group_show_resv_regions - sysfs 의 reserved_regions 속성 읽기
 *
 * @group:  대상 그룹
 * @buf:    출력 버퍼(PAGE_SIZE)
 * @return: 쓴 바이트 수
 *
 * "0x<시작> 0x<끝> <종류>" 형식으로 한 줄에 한 구간씩 찍는다. QEMU 가 VFIO 장치를
 * 게스트에 붙일 때 이 파일을 읽어, 게스트 물리 주소 배치가 이 구간들과 겹치지
 * 않도록 조정한다 — 겹치면 게스트 메모리를 매핑하려다 예약 구간과 충돌해 실패한다.
 *
 * 목록은 이 함수가 유일한 소유자이므로 출력하면서 곧바로 해제한다.
 * iommu_get_group_resv_regions 의 반환값을 보지 않는 것은 의도적이다 — 일부만
 * 모였더라도 있는 만큼은 보여 주는 편이 낫다.
 *
 * 실행 컨텍스트: 사용자 공간 read(). 잠들 수 있다.
 *
 * 호출 체인: sysfs → iommu_group_attr_show → [이 함수] → iommu_get_group_resv_regions
 */
static ssize_t iommu_group_show_resv_regions(struct iommu_group *group,
					     char *buf)
{
	struct iommu_resv_region *region, *next;	/* [한국어] 출력하며 해제할 것이므로 _safe 순회용 두 포인터 */
	struct list_head group_resv_regions;	/* [한국어] 이 호출에서만 쓰는 임시 목록 */
	int offset = 0;	/* [한국어] 버퍼에 쓴 누적 바이트 수 */

	INIT_LIST_HEAD(&group_resv_regions);	/* [한국어] 빈 목록으로 시작 */
	iommu_get_group_resv_regions(group, &group_resv_regions);	/* [한국어] 그룹 전체의 예약 구간을 모은다 (실패해도 부분 목록을 그대로 출력한다) */

	list_for_each_entry_safe(region, next, &group_resv_regions, list) {	/* [한국어] 한 줄에 한 구간씩 찍으며 동시에 해제한다 */
		offset += sysfs_emit_at(buf, offset, "0x%016llx 0x%016llx %s\n",	/* [한국어] 시작·끝 주소와 종류를 한 줄로. 사용자 공간(특히 VFIO/QEMU)이 이 목록을 읽어 게스트에게 그 주소 구간을 내주지 않도록 피해 간다 */
					(long long)region->start,	/* [한국어] 시작 주소 */
					(long long)(region->start +	/* [한국어] 끝 주소는 닫힌 구간으로 */
						    region->length - 1),	/* [한국어] 마지막 유효 바이트 */
					iommu_group_resv_type_string[region->type]);	/* [한국어] direct / direct-relaxable / reserved / msi 로 문자열화 */
		kfree(region);	/* [한국어] 복제본이므로 출력 후 즉시 해제 — 이 함수가 목록의 유일한 소유자다 */
	}

	return offset;	/* [한국어] sysfs 에 쓴 총 바이트 수 */
}

/*
 * [한국어]
 * iommu_group_show_type - sysfs 의 그룹 type 속성 읽기 (기본 도메인 종류)
 *
 * @group:  대상 그룹
 * @buf:    출력 버퍼
 * @return: 쓴 바이트 수
 *
 * 이 그룹이 지금 어떤 번역 정책 아래 있는지를 한 단어로 보여 준다. 관리자가
 * "이 장치의 DMA 가 실제로 IOMMU 를 지나는가"를 확인하는 가장 빠른 방법이며,
 * 같은 파일에 쓰면 iommu_group_store_type 이 런타임에 정책을 바꾼다.
 *
 * 다섯 값의 의미가 그대로 이 서브시스템의 정책 축이다 — blocked(전면 차단),
 * identity(번역 없음), unmanaged(사용자 공간 소유), DMA(즉시 무효화),
 * DMA-FQ(지연 무효화). 기본 도메인이 아직 서지 않았으면 "unknown".
 *
 * 실행 컨텍스트: 사용자 공간 read(). 그룹 락을 잡는다.
 *
 * 호출 체인: sysfs → iommu_group_attr_show → [이 함수]
 */
static ssize_t iommu_group_show_type(struct iommu_group *group,
				     char *buf)
{
	char *type = "unknown";	/* [한국어] 기본 도메인이 아직 없거나 알 수 없는 종류일 때의 값 */

	mutex_lock(&group->mutex);	/* [한국어] 도메인 포인터를 읽는 동안 교체되지 않도록 */
	if (group->default_domain) {	/* [한국어] 기본 도메인이 서 있어야 종류를 말할 수 있다 */
		switch (group->default_domain->type) {	/* [한국어] 도메인 종류를 사용자에게 보여 줄 이름으로 옮긴다 */
		case IOMMU_DOMAIN_BLOCKED:	/* [한국어] 모든 DMA 차단 */
			type = "blocked";	/* [한국어] 장치가 어떤 메모리에도 닿지 못한다 */
			break;
		case IOMMU_DOMAIN_IDENTITY:	/* [한국어] 항등 — 번역 없이 물리 주소 그대로 */
			type = "identity";	/* [한국어] 패스스루. 성능 최대, 격리 없음 */
			break;
		case IOMMU_DOMAIN_UNMANAGED:	/* [한국어] 코어가 관리하지 않는 도메인 — VFIO/iommufd 가 직접 매핑을 넣는다 */
			type = "unmanaged";	/* [한국어] 사용자 공간이 주소 공간을 소유한 상태 */
			break;
		case IOMMU_DOMAIN_DMA:	/* [한국어] 커널 DMA API 용 번역 도메인, 즉시 무효화 */
			type = "DMA";	/* [한국어] 해제 때마다 IOTLB 를 비운다 — 가장 안전한 기본값 */
			break;
		case IOMMU_DOMAIN_DMA_FQ:	/* [한국어] 같은 번역 도메인이지만 flush queue 로 무효화를 모은다 */
			type = "DMA-FQ";	/* [한국어] 처리량 우선. 해제 직후 잠시 옛 번역이 남는다 */
			break;
		}
	}
	mutex_unlock(&group->mutex);	/* [한국어] 읽기 완료 */

	return sysfs_emit(buf, "%s\n", type);	/* [한국어] 이 파일은 쓰기도 가능하다 — iommu_group_store_type 이 런타임에 종류를 바꾼다 */
}

static IOMMU_GROUP_ATTR(name, S_IRUGO, iommu_group_show_name, NULL);

static IOMMU_GROUP_ATTR(reserved_regions, 0444,
			iommu_group_show_resv_regions, NULL);

static IOMMU_GROUP_ATTR(type, 0644, iommu_group_show_type,
			iommu_group_store_type);

/*
 * [한국어]
 * iommu_group_release - 마지막 참조가 놓였을 때 그룹을 해제한다
 *
 * @kobj: 그룹이 품고 있던 kobject
 * @return: 없음
 *
 * 직접 부르는 곳이 없다. kobject 참조가 0 이 될 때 kobject 계층이
 * 불러 주며, 그래서 그룹의 수명이 곧 sysfs 객체의 수명이 된다.
 *
 * 두 WARN 이 이 함수의 검사다. 도메인은 위 영어 주석대로 장치를 뗄 때
 * (iommu_deinit_device) 해제되므로, 여기 도달했는데 아직 남아 있다면
 * 장치보다 그룹이 먼저 사라진 것이다 -- 있을 수 없는 순서이고, 그대로
 * 두면 도메인이 새어 나간다.
 *
 * iommu_data_release 를 먼저 부르는 것도 순서다. 소유자가 매달아 둔
 * 상태가 그룹의 다른 필드를 참조할 수 있으므로, 그룹 자체를 풀기 전에
 * 그쪽을 먼저 정리하게 한다.
 *
 * 실행 컨텍스트: 마지막 kobject_put 이 도는 문맥. 잠들 수 있다.
 *
 * 호출 체인: kobject_put(마지막) → ktype.release → [이 함수]
 */
static void iommu_group_release(struct kobject *kobj)
{
	struct iommu_group *group = to_iommu_group(kobj);

	pr_debug("Releasing group %d\n", group->id);	/* [한국어] 그룹 수명 추적 — 어느 그룹이 언제 사라졌는지 */

	if (group->iommu_data_release)
		group->iommu_data_release(group->iommu_data);	/* [한국어] 소유자 상태를 먼저 — 그룹 필드를 참조할 수 있다 */

	ida_free(&iommu_group_ida, group->id);	/* [한국어] 번호를 반납해 다음 그룹이 재사용한다 */

	/* Domains are free'd by iommu_deinit_device() */
	/* [한국어] 위 영어 주석대로 도메인은 장치를 뗄 때 해제된다. 여기 남아
	 * 있다면 장치보다 그룹이 먼저 사라진 것이고, 그런 순서는 성립할 수 없다. */
	WARN_ON(group->default_domain);
	WARN_ON(group->blocking_domain);

	kfree(group->name);	/* [한국어] NULL 이어도 안전하다 — 이름 없는 그룹이 흔하다 */
	kfree(group);
}

/* [한국어] 그룹 kobject 의 형(型). sysfs 동작과 해제 방법을 kobject 계층에
 * 알려 준다. 이 release 가 걸려 있어서 그룹이 참조 계수로 관리된다. */
static const struct kobj_type iommu_group_ktype = {
	.sysfs_ops = &iommu_group_sysfs_ops,
	.release = iommu_group_release,
};

/**
 * iommu_group_alloc - Allocate a new group
 *
 * This function is called by an iommu driver to allocate a new iommu
 * group.  The iommu group represents the minimum granularity of the iommu.
 * Upon successful return, the caller holds a reference to the supplied
 * group in order to hold the group until devices are added.  Use
 * iommu_group_put() to release this extra reference count, allowing the
 * group to be automatically reclaimed once it has no devices or external
 * references.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 참조 계수가 이 함수의 까다로운 부분이다.
 *
 * 그룹 kobject 와 devices_kobj 는 부모-자식이고, 자식이 부모의 참조를
 * 하나 든다. 그래서 아래에서 부모 참조를 한 번 놓아도 그룹은 살아 있고,
 * 이후로는 devices_kobj 하나만 관리하면 된다 -- 그것이 사라질 때 부모
 * 참조도 함께 놓여 그룹이 해제된다.
 *
 * 그 뒤집기 때문에 실패 경로가 두 갈래인 것에 주의할 것. devices_kobj 를
 * 만들기 전에는 &group->kobj 를 놓아야 하고, 만든 뒤에는 devices_kobj 를
 * 놓아야 한다. 둘을 섞으면 참조가 하나 새거나 두 번 놓인다.
 *
 * ida_alloc 실패만 kfree 로 직접 되돌리는 것도 같은 이유다. 그 시점에는
 * kobject 가 아직 초기화되지 않아 release 콜백이 걸려 있지 않다.
 *
 * 실행 컨텍스트: 벤더 드라이버의 그룹 생성 경로. 잠들 수 있다.
 *
 * 호출 체인: 벤더 드라이버 device_group 콜백 → [이 함수]
 */
struct iommu_group *iommu_group_alloc(void)
{
	struct iommu_group *group;
	int ret;

	group = kzalloc_obj(*group);
	if (!group)
		return ERR_PTR(-ENOMEM);

	group->kobj.kset = iommu_group_kset;	/* [한국어] /sys/kernel/iommu_groups 아래에 놓이게 한다 */
	mutex_init(&group->mutex);	/* [한국어] 이 그룹의 모든 상태를 지키는 락 */
	INIT_LIST_HEAD(&group->devices);	/* [한국어] 아직 장치가 없다 */
	INIT_LIST_HEAD(&group->entry);	/* [한국어] 전역 목록 고리 */
	xa_init(&group->pasid_array);	/* [한국어] PASID 별 도메인 표 */

	ret = ida_alloc(&iommu_group_ida, GFP_KERNEL);
	if (ret < 0) {
		kfree(group);	/* [한국어] kobject 가 아직 초기화 전이라 release 가 걸려 있지 않다 — 직접 푼다 */
		return ERR_PTR(ret);
	}
	group->id = ret;	/* [한국어] 이 번호가 곧 sysfs 디렉토리 이름이 된다 */

	ret = kobject_init_and_add(&group->kobj, &iommu_group_ktype,	/* [한국어] 여기부터 release 콜백이 살아난다 */
				   NULL, "%d", group->id);
	if (ret) {
		kobject_put(&group->kobj);	/* [한국어] 이제는 put 이 release 를 불러 id 반납과 kfree 까지 해 준다 */
		return ERR_PTR(ret);
	}

	group->devices_kobj = kobject_create_and_add("devices", &group->kobj);	/* [한국어] 소속 장치 링크가 걸릴 하위 디렉토리 */
	if (!group->devices_kobj) {
		kobject_put(&group->kobj); /* triggers .release & free */
		return ERR_PTR(-ENOMEM);
	}

	/*
	 * The devices_kobj holds a reference on the group kobject, so
	 * as long as that exists so will the group.  We can therefore
	 * use the devices_kobj for reference counting.
	 */
	/* [한국어] 위 영어 주석대로 자식이 부모 참조를 들고 있으므로, 여기서
	 * 부모 몫을 놓아도 그룹은 살아 있다. 이 뒤집기 이후로는 devices_kobj
	 * 하나만 관리하면 되고, 아래 실패 경로들이 그것을 놓는 이유다. */
	kobject_put(&group->kobj);

	ret = iommu_group_create_file(group,	/* [한국어] 이 그룹이 피해야 할 주소 구간을 사용자 공간에 노출한다 */
				      &iommu_group_attr_reserved_regions);
	if (ret) {
		kobject_put(group->devices_kobj);	/* [한국어] 이제 자식을 놓아야 한다 — 부모 몫은 위에서 이미 놓았다 */
		return ERR_PTR(ret);
	}

	ret = iommu_group_create_file(group, &iommu_group_attr_type);	/* [한국어] 기본 도메인 종류. 읽기뿐 아니라 쓰기로 바꿀 수도 있다 */
	if (ret) {
		kobject_put(group->devices_kobj);
		return ERR_PTR(ret);
	}

	pr_debug("Allocated group %d\n", group->id);

	return group;	/* [한국어] 호출자가 devices_kobj 참조 하나를 넘겨받는다 */
}
EXPORT_SYMBOL_GPL(iommu_group_alloc);	/* [한국어] 벤더 드라이버가 그룹을 직접 만들 때 부른다 */

/**
 * iommu_group_get_iommudata - retrieve iommu_data registered for a group
 * @group: the group
 *
 * iommu drivers can store data in the group for use when doing iommu
 * operations.  This function provides a way to retrieve it.  Caller
 * should hold a group reference.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 그룹에 매달아 둔 소유자 상태를 꺼낸다.
 *
 * 락을 잡지 않는 것에 주의할 것. 위 주석대로 호출자가 이미 그룹 참조를
 * 들고 있어야 하고, 이 필드는 소유자 자신만 쓰므로 경쟁이 없다는 전제다.
 *
 * 호출 체인: VFIO 등 그룹 소유자 → [이 함수]
 */
void *iommu_group_get_iommudata(struct iommu_group *group)
{
	return group->iommu_data;	/* [한국어] 내용은 이 파일이 해석하지 않는다 — 불투명 포인터다 */
}
EXPORT_SYMBOL_GPL(iommu_group_get_iommudata);

/**
 * iommu_group_set_iommudata - set iommu_data for a group
 * @group: the group
 * @iommu_data: new data
 * @release: release function for iommu_data
 *
 * iommu drivers can store data in the group for use when doing iommu
 * operations.  This function provides a way to set the data after
 * the group has been allocated.  Caller should hold a group reference.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 그 짝. 상태와 함께 해제 방법도 받는다.
 *
 * release 를 함께 받는 이유: 그룹이 사라질 때 소유자는 이미 없을 수 있고,
 * 그러면 이 상태를 무엇으로 풀어야 하는지 그룹 자신은 알 수 없다. 그래서
 * 해제 함수를 지금 함께 등록해 둔다.
 *
 * 호출 체인: VFIO 등 그룹 소유자 → [이 함수]
 */
void iommu_group_set_iommudata(struct iommu_group *group, void *iommu_data,
			       void (*release)(void *iommu_data))
{
	group->iommu_data = iommu_data;
	group->iommu_data_release = release;	/* [한국어] 소유자가 사라진 뒤에도 정리할 수 있도록 방법을 함께 받아 둔다 */
}
EXPORT_SYMBOL_GPL(iommu_group_set_iommudata);

/**
 * iommu_group_set_name - set name for a group
 * @group: the group
 * @name: name
 *
 * Allow iommu driver to set a name for a group.  When set it will
 * appear in a name attribute file under the group in sysfs.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 그룹에 이름을 붙이거나 지운다.
 *
 * 이름은 선택 사항이라 없을 수도 있고, 그때는 sysfs 에 name 파일 자체가
 * 없다. 그래서 이 함수는 "설정"이 아니라 "있으면 지우고 다시 만들기"로
 * 동작한다 -- 기존 파일을 먼저 없애야 새 이름으로 다시 만들 수 있다.
 *
 * name 이 NULL 이면 지우기만 하고 끝난다. 그 경우 위쪽 블록에서 곧바로
 * 돌아가는 것이 그 처리다.
 *
 * 실패 시 포인터까지 NULL 로 되돌리는 것에 주의할 것. 파일 생성에 실패한
 * 채 이름만 남으면, 나중에 이 함수가 다시 불릴 때 존재하지 않는 sysfs
 * 파일을 지우려 한다.
 *
 * 실행 컨텍스트: 벤더 드라이버의 그룹 설정 경로. 잠들 수 있다.
 *
 * 호출 체인: 벤더 드라이버 → [이 함수] → sysfs_create_file
 */
int iommu_group_set_name(struct iommu_group *group, const char *name)
{
	int ret;

	if (group->name) {	/* [한국어] 이미 이름이 있으면 파일부터 없애야 새로 만들 수 있다 */
		iommu_group_remove_file(group, &iommu_group_attr_name);
		kfree(group->name);
		group->name = NULL;
		if (!name)
			return 0;	/* [한국어] 지우기만 요청한 경우 — 여기서 끝난다 */
	}

	group->name = kstrdup(name, GFP_KERNEL);	/* [한국어] 호출자의 문자열이 곧 사라질 수 있어 복사한다 */
	if (!group->name)
		return -ENOMEM;

	ret = iommu_group_create_file(group, &iommu_group_attr_name);
	if (ret) {
		kfree(group->name);
		group->name = NULL;	/* [한국어] 포인터까지 지워야 다음 호출이 없는 파일을 지우려 하지 않는다 */
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(iommu_group_set_name);

/*
 * [한국어]
 * iommu_create_device_direct_mappings - 예약 구간을 항등 매핑으로 미리 채운다
 *
 * @domain: 채울 도메인
 * @dev:    이 장치가 요구하는 예약 구간을 묻는다
 * @return: 0 이면 성공, 음수 errno
 *
 * 왜 필요한가: 어떤 주소들은 커널이 IOMMU 를 세우기 전부터 이미 쓰이고
 * 있다. 펌웨어가 화면에 그림을 그리고 있거나, 부팅 로더가 남긴 버퍼를
 * 장치가 계속 읽고 있는 경우다. 그런 주소를 번역 대상으로 만들면 그
 * 순간 접근이 끊기므로, IOMMU 를 켜기 전에 "주소가 그대로 통과되는"
 * 매핑을 미리 넣어 둔다. 그것이 항등(direct) 매핑이다.
 *
 * 루프의 모양이 특이한 것에는 이유가 있다. 위 영어 주석대로 여러 장치의
 * 예약 구간이 겹칠 수 있어, 이미 매핑된 페이지를 다시 매핑하면 실패한다.
 * 그래서 한 페이지씩 훑으며 아직 비어 있는 구간만 모았다가(map_size)
 * 이미 매핑된 페이지를 만나면 거기서 끊어 한 번에 매핑한다. 페이지마다
 * iommu_map 을 부르는 것보다 훨씬 적은 호출로 끝난다.
 *
 * 루프 조건이 addr <= end 인 것도 그 구조 때문이다. 마지막 한 바퀴는
 * 매핑할 페이지를 보는 것이 아니라, 모아 둔 구간을 비우기 위해 도는
 * 것이며 map_end 로 곧장 뛴다.
 *
 * 주소 0 을 1 로 바꿔 묻는 것은 위 영어 주석이 밝히는 API 의 모호함
 * 때문이다 -- iommu_iova_to_phys 는 "매핑 없음"도 0 으로 답하므로,
 * 물리 주소 0 에 매핑된 경우와 구별할 수 없다.
 *
 * 실행 컨텍스트: 장치를 그룹에 들이는 경로. 잠들 수 있다(GFP_KERNEL).
 *
 * 호출 체인: iommu_setup_default_domain / 장치 추가 → [이 함수] → iommu_map
 */
static int iommu_create_device_direct_mappings(struct iommu_domain *domain,
					       struct device *dev)
{
	struct iommu_resv_region *entry;	/* [한국어] 예약 구간 하나 */
	LIST_HEAD(mappings);	/* [한국어] 이 장치가 요구하는 구간들을 받을 목록 */
	unsigned long pg_size;	/* [한국어] 이 도메인이 다룰 수 있는 가장 작은 페이지 */
	int ret = 0;

	pg_size = domain->pgsize_bitmap ? 1UL << __ffs(domain->pgsize_bitmap) : 0;	/* [한국어] 지원 크기 비트맵의 최하위 비트가 최소 페이지다 */

	if (WARN_ON_ONCE(iommu_is_dma_domain(domain) && !pg_size))	/* [한국어] 번역 도메인인데 페이지 크기를 모른다 — 드라이버가 비트맵을 안 채웠다 */
		return -EINVAL;

	iommu_get_resv_regions(dev, &mappings);	/* [한국어] 벤더 드라이버와 펌웨어가 이 장치의 예약 구간을 알려 준다 */

	/* We need to consider overlapping regions for different devices */
	list_for_each_entry(entry, &mappings, list) {
		dma_addr_t start, end, addr;
		size_t map_size = 0;	/* [한국어] 아직 매핑되지 않은 채 모아 둔 길이 */

		if (entry->type == IOMMU_RESV_DIRECT)
			dev->iommu->require_direct = 1;	/* [한국어] 이 장치는 항등 매핑이 필수 — 나중에 통과 모드를 끌 수 있는지 판단하는 근거가 된다 */

		if ((entry->type != IOMMU_RESV_DIRECT &&
		     entry->type != IOMMU_RESV_DIRECT_RELAXABLE) ||
		    !iommu_is_dma_domain(domain))
			continue;	/* [한국어] 항등이 필요한 구간이 아니거나, 번역하지 않는 도메인이라 채울 것이 없다 */

		start = ALIGN(entry->start, pg_size);	/* [한국어] 페이지 경계로 맞춘다 — IOMMU 는 페이지 단위로만 매핑한다 */
		end   = ALIGN(entry->start + entry->length, pg_size);

		for (addr = start; addr <= end; addr += pg_size) {	/* [한국어] <= 인 이유: 마지막 한 바퀴는 모아 둔 구간을 비우려고 돈다 */
			phys_addr_t phys_addr;

			if (addr == end)
				goto map_end;	/* [한국어] 끝에 닿았다 — 남은 구간만 매핑하고 이 항목을 마친다 */

			/*
			 * Return address by iommu_iova_to_phys for 0 is
			 * ambiguous. Offset to address 1 if addr is 0.
			 */
			/* [한국어] 위 영어 주석대로, 반환값 0 이 "매핑 없음"과
			 * "물리 주소 0 에 매핑됨"을 구별하지 못한다. 그래서 주소 0 은
			 * 1 로 바꿔 묻는다 -- 같은 페이지 안이라 답은 같다. */
			phys_addr = iommu_iova_to_phys(domain, addr ? addr : 1);
			if (!phys_addr) {
				map_size += pg_size;	/* [한국어] 비어 있다 — 모아 두었다가 한 번에 매핑한다 */
				continue;
			}

map_end:
			if (map_size) {	/* [한국어] 모아 둔 것이 있으면 여기서 끊어 낸다 */
				ret = iommu_map(domain, addr - map_size,	/* [한국어] IOVA 와 물리 주소가 같다 — 이것이 항등 매핑이다 */
						addr - map_size, map_size,
						entry->prot, GFP_KERNEL);
				if (ret)
					goto out;	/* [한국어] 이미 넣은 매핑은 도메인이 해제될 때 함께 사라진다 */
				map_size = 0;	/* [한국어] 다음 구간을 새로 모으기 시작한다 */
			}
		}

	}
out:
	iommu_put_resv_regions(dev, &mappings);	/* [한국어] 목록은 드라이버가 잡아 준 것이라 같은 쪽에 돌려준다 */

	return ret;
}

/* This is undone by __iommu_group_free_device() */
/*
 * [한국어]
 * iommu_group_alloc_device - 장치 하나를 그룹에 넣을 항목으로 만든다
 *
 * @group: 들어갈 그룹
 * @dev:   들어갈 장치
 * @return: 만들어진 항목. 실패하면 오류 포인터.
 *
 * 실제로 하는 일의 대부분은 sysfs 링크 두 개를 거는 것이다. 장치 쪽에는
 * "네가 속한 그룹"을 가리키는 iommu_group 링크가, 그룹의 devices/ 아래에는
 * 장치를 가리키는 링크가 걸린다. 사용자 공간이 양방향으로 탐색할 수 있게
 * 하려는 것이며, VFIO 를 쓰려면 반드시 필요한 정보다.
 *
 * 이름 충돌 처리가 이 함수의 눈에 띄는 부분이다. 서로 다른 버스의 장치가
 * 같은 kobject 이름을 가질 수 있어, 링크 생성이 -EEXIST 로 실패하면
 * ".0", ".1" 을 붙여 다시 시도한다. nowarn 판을 쓰는 것도 그래서다 --
 * 이 실패는 예상된 것이라 커널 로그에 경고를 낼 일이 아니다.
 *
 * 목록에 넣는 것은 호출자의 몫이다. 이 함수는 항목만 만들어 돌려준다.
 *
 * 실행 컨텍스트: 장치 추가 경로. 잠들 수 있다.
 *
 * 호출 체인: iommu_group_add_device / 장치 probe → [이 함수]
 */
static struct group_device *iommu_group_alloc_device(struct iommu_group *group,
						     struct device *dev)
{
	int ret, i = 0;	/* [한국어] i 는 이름 충돌 시 붙일 일련번호다 */
	struct group_device *device;

	device = kzalloc_obj(*device);
	if (!device)
		return ERR_PTR(-ENOMEM);

	device->dev = dev;

	ret = sysfs_create_link(&dev->kobj, &group->kobj, "iommu_group");
	if (ret)
		goto err_free_device;

	device->name = kasprintf(GFP_KERNEL, "%s", kobject_name(&dev->kobj));	/* [한국어] 첫 시도는 장치 이름 그대로 */
rename:	/* [한국어] 이름이 겹치면 번호를 붙여 여기로 돌아온다 */
	if (!device->name) {
		ret = -ENOMEM;
		goto err_remove_link;
	}

	ret = sysfs_create_link_nowarn(group->devices_kobj,	/* [한국어] nowarn — 충돌은 예상된 것이라 커널 로그를 어지럽히지 않는다 */
				       &dev->kobj, device->name);
	if (ret) {
		if (ret == -EEXIST && i >= 0) {	/* [한국어] i >= 0 은 오버플로 방어. 음수가 되면 재시도를 멈춘다 */
			/*
			 * Account for the slim chance of collision
			 * and append an instance to the name.
			 */
			/* [한국어] 위 영어 주석대로 드물지만 다른 버스의 장치가
			 * 같은 이름을 가질 수 있다. 번호를 붙여 다시 시도한다. */
			kfree(device->name);
			device->name = kasprintf(GFP_KERNEL, "%s.%d",
						 kobject_name(&dev->kobj), i++);
			goto rename;
		}
		goto err_free_name;
	}

	trace_add_device_to_group(group->id, dev);	/* [한국어] 어느 장치가 어느 그룹에 들어갔는지 추적점에 남긴다 */

	dev_info(dev, "Adding to iommu group %d\n", group->id);	/* [한국어] 그룹 배정은 사용자가 알아야 하는 사실이라 info 다 */

	return device;	/* [한국어] 목록에 넣는 것은 호출자의 몫이다 */

err_free_name:
	kfree(device->name);
err_remove_link:
	sysfs_remove_link(&dev->kobj, "iommu_group");	/* [한국어] 장치 쪽 링크를 되돌린다 */
err_free_device:
	kfree(device);
	dev_err(dev, "Failed to add to iommu group %d: %d\n", group->id, ret);	/* [한국어] 이 장치는 IOMMU 보호를 받지 못한다 — 반드시 남겨야 한다 */
	return ERR_PTR(ret);
}

/**
 * iommu_group_add_device - add a device to an iommu group
 * @group: the group into which to add the device (reference should be held)
 * @dev: the device
 *
 * This function is called by an iommu driver to add a device into a
 * group.  Adding a device increments the group reference count.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 순서가 이 함수의 내용이다.
 *
 * 참조를 먼저 들고 목록에 넣는 것이 마지막이다. 목록에 들어가는 순간부터
 * 다른 경로가 이 장치를 보게 되므로, 그전에 그룹 참조와 역방향 포인터가
 * 이미 서 있어야 한다. 순서를 바꾸면 순회 중인 쪽이 아직 완성되지 않은
 * 항목을 만난다.
 *
 * 참조는 위 영어 주석대로 장치가 그룹에 있는 동안 유지되고, 뺄 때 놓인다.
 * 그래서 마지막 장치가 빠지면 그룹이 자연히 사라진다.
 *
 * 실행 컨텍스트: 벤더 드라이버의 장치 추가. 잠들 수 있다.
 *
 * 호출 체인: 벤더 드라이버 → [이 함수] → iommu_group_alloc_device
 */
int iommu_group_add_device(struct iommu_group *group, struct device *dev)
{
	struct group_device *gdev;

	gdev = iommu_group_alloc_device(group, dev);	/* [한국어] sysfs 링크까지 걸어 준다 */
	if (IS_ERR(gdev))
		return PTR_ERR(gdev);

	iommu_group_ref_get(group);	/* [한국어] 장치가 그룹에 있는 동안 그룹이 사라지지 않게 한다 */
	dev->iommu_group = group;	/* [한국어] 역방향 포인터 — 장치에서 그룹을 찾는 통로 */

	mutex_lock(&group->mutex);
	list_add_tail(&gdev->list, &group->devices);	/* [한국어] 마지막에 목록에 넣는다 — 이 순간부터 다른 경로가 이 장치를 본다 */
	mutex_unlock(&group->mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(iommu_group_add_device);

/**
 * iommu_group_remove_device - remove a device from it's current group
 * @dev: device to be removed
 *
 * This function is called by an iommu driver to remove the device from
 * it's current group.  This decrements the iommu group reference count.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) add 의 짝.
 *
 * 그룹이 없는 장치를 조용히 넘기는 것에 주의할 것. IOMMU 아래에 들어오지
 * 않은 장치가 대부분이고, 그런 장치에 이 함수가 불리는 것은 오류가 아니다.
 *
 * 실행 컨텍스트: 벤더 드라이버의 장치 제거. 잠들 수 있다.
 *
 * 호출 체인: 벤더 드라이버 → [이 함수] → __iommu_group_remove_device
 */
void iommu_group_remove_device(struct device *dev)
{
	struct iommu_group *group = dev->iommu_group;

	if (!group)
		return;	/* [한국어] IOMMU 아래에 없던 장치 — 뺄 것이 없다 */

	dev_info(dev, "Removing from iommu group %d\n", group->id);	/* [한국어] 추가와 짝을 이루는 기록 */

	__iommu_group_remove_device(dev);	/* [한국어] 링크 제거, 목록에서 빼기, 참조 놓기까지 그쪽이 한다 */
}
EXPORT_SYMBOL_GPL(iommu_group_remove_device);

#if IS_ENABLED(CONFIG_LOCKDEP) && IS_ENABLED(CONFIG_IOMMU_API)
/**
 * iommu_group_mutex_assert - Check device group mutex lock
 * @dev: the device that has group param set
 *
 * This function is called by an iommu driver to check whether it holds
 * group mutex lock for the given device or not.
 *
 * Note that this function must be called after device group param is set.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 벤더 드라이버가 자기 콜백 안에서
 * 그룹 락을 들고 있는지 확인하는 수단이다.
 *
 * 왜 필요한가: 벤더 드라이버의 콜백들은 대부분 이 파일이 group->mutex 를
 * 든 채로 부르지만, 그 규약은 코드에 적혀 있지 않다. 드라이버 쪽에서
 * 그 전제가 실제로 지켜지는지 검사하고 싶을 때 이것을 쓴다.
 *
 * lockdep 을 켰을 때만 존재하는 것에 주의할 것 -- 실행 비용이 있는 검사라
 * 디버깅 빌드에서만 컴파일된다.
 *
 * 호출 체인: 벤더 드라이버 콜백 → [이 함수]
 */
void iommu_group_mutex_assert(struct device *dev)
{
	struct iommu_group *group = dev->iommu_group;

	lockdep_assert_held(&group->mutex);	/* [한국어] 위 주석대로 그룹이 이미 설정된 뒤에만 부를 수 있다 — 아니면 NULL 역참조다 */
}
EXPORT_SYMBOL_GPL(iommu_group_mutex_assert);
#endif

/*
 * [한국어]
 * iommu_group_first_dev - 그룹의 대표 장치 하나를 꺼낸다
 *
 * @group: 대상 그룹. 호출자가 group->mutex 를 들고 있어야 한다.
 * @return: 첫 장치. 빈 그룹에 부르면 안 된다.
 *
 * 왜 "아무 장치나 하나"로 충분한가: 그룹의 장치들은 같은 IOMMU 아래에
 * 있고 같은 도메인을 보므로, IOMMU 능력을 묻거나 ops 를 찾는 일에는
 * 어느 장치를 써도 답이 같다. 도메인을 만들 때나 페이지 크기를 물을 때
 * 이 함수를 쓴다.
 *
 * 빈 목록을 검사하지 않는 것에 주의할 것. 장치가 하나도 없는 그룹은
 * 곧바로 해제되므로, 이 함수가 불리는 시점에는 반드시 하나 이상 있다.
 *
 * 호출 체인: 도메인 생성·능력 조회 경로 → [이 함수]
 */
static struct device *iommu_group_first_dev(struct iommu_group *group)
{
	lockdep_assert_held(&group->mutex);	/* [한국어] 락 없이 부르면 목록이 바뀌는 중일 수 있다 */
	return list_first_entry(&group->devices, struct group_device, list)->dev;	/* [한국어] 어느 것이든 같은 답을 주므로 첫 항목이면 된다 */
}

/**
 * iommu_group_for_each_dev - iterate over each device in the group
 * @group: the group
 * @data: caller opaque data to be passed to callback function
 * @fn: caller supplied callback function
 *
 * This function is called by group users to iterate over group devices.
 * Callers should hold a reference count to the group during callback.
 * The group->mutex is held across callbacks, which will block calls to
 * iommu_group_add/remove_device.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 그룹의 모든 장치에 콜백을 돌린다.
 *
 * 위 주석이 밝히는 규약이 중요하다 -- 콜백이 도는 내내 group->mutex 를
 * 들고 있으므로, 콜백 안에서 장치를 추가하거나 제거하면 교착이 된다.
 * 콜백은 짧고 잠들지 않아야 한다.
 *
 * 콜백이 0 이 아닌 값을 주면 순회를 멈추고 그 값을 그대로 올린다.
 * "찾으면 멈추기" 와 "실패하면 중단" 을 같은 규약으로 다룬다.
 *
 * 실행 컨텍스트: 그룹 사용자(VFIO 등). 잠들 수 있다.
 *
 * 호출 체인: VFIO 등 → [이 함수] → 호출자가 준 fn
 */
int iommu_group_for_each_dev(struct iommu_group *group, void *data,
			     int (*fn)(struct device *, void *))
{
	struct group_device *device;
	int ret = 0;

	mutex_lock(&group->mutex);	/* [한국어] 콜백이 도는 내내 잡고 있다 — 그 안에서 장치를 더하거나 빼면 교착이다 */
	for_each_group_device(group, device) {
		ret = fn(device->dev, data);
		if (ret)
			break;	/* [한국어] 0 이 아니면 멈춘다 — 찾았거나 실패했거나 */
	}
	mutex_unlock(&group->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(iommu_group_for_each_dev);

/**
 * iommu_group_get - Return the group for a device and increment reference
 * @dev: get the group that this device belongs to
 *
 * This function is called by iommu drivers and users to get the group
 * for the specified device.  If found, the group is returned and the group
 * reference in incremented, else NULL.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 장치에서 그룹을 찾고 참조를 든다.
 *
 * devices_kobj 를 잡는 것에 주의할 것. 그룹 kobject 가 아니라 자식을
 * 잡는데, 이것이 iommu_group_alloc() 의 참조 뒤집기와 짝을 이룬다 --
 * 자식이 부모를 들고 있으므로 자식만 관리하면 그룹 전체가 유지된다.
 *
 * 호출 체인: VFIO·벤더 드라이버 → [이 함수]
 */
struct iommu_group *iommu_group_get(struct device *dev)
{
	struct iommu_group *group = dev->iommu_group;

	if (group)
		kobject_get(group->devices_kobj);	/* [한국어] 그룹 수명은 이 자식 kobject 가 대표한다 */

	return group;	/* [한국어] IOMMU 아래에 없는 장치면 NULL — 오류가 아니다 */
}
EXPORT_SYMBOL_GPL(iommu_group_get);

/**
 * iommu_group_ref_get - Increment reference on a group
 * @group: the group to use, must not be NULL
 *
 * This function is called by iommu drivers to take additional references on an
 * existing group.  Returns the given group for convenience.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 이미 손에 든 그룹의 참조를 하나 더 든다.
 *
 * get() 과 달리 NULL 검사가 없다. 위 주석대로 호출자가 이미 유효한
 * 그룹을 가지고 있다는 전제이며, 그 전제가 깨지면 곧바로 터진다.
 *
 * 그룹을 그대로 돌려주는 것은 호출자의 편의를 위한 것이다 --
 * group = iommu_group_ref_get(group) 처럼 이어 쓸 수 있다.
 *
 * 호출 체인: iommu_group_add_device 등 → [이 함수]
 */
struct iommu_group *iommu_group_ref_get(struct iommu_group *group)
{
	kobject_get(group->devices_kobj);
	return group;	/* [한국어] 이어 쓰기 편하도록 인자를 그대로 돌려준다 */
}
EXPORT_SYMBOL_GPL(iommu_group_ref_get);

/**
 * iommu_group_put - Decrement group reference
 * @group: the group to use
 *
 * This function is called by iommu drivers and users to release the
 * iommu group.  Once the reference count is zero, the group is released.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 참조를 놓는다. 마지막이면 그룹이 해제된다.
 *
 * NULL 을 허용하는 것에 주의할 것. 실패 경로에서 그룹을 얻었는지 아닌지
 * 따지지 않고 부를 수 있게 하려는 것이며, get() 이 NULL 을 돌려줄 수
 * 있다는 점과 짝을 이룬다.
 *
 * 호출 체인: 그룹을 다 쓴 모든 곳 → [이 함수] → (마지막이면) iommu_group_release
 */
void iommu_group_put(struct iommu_group *group)
{
	if (group)
		kobject_put(group->devices_kobj);	/* [한국어] 0 이 되면 부모 참조도 함께 놓여 그룹이 해제된다 */
}
EXPORT_SYMBOL_GPL(iommu_group_put);

/**
 * iommu_group_id - Return ID for a group
 * @group: the group to ID
 *
 * Return the unique ID for the group matching the sysfs group number.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 그룹 번호를 돌려준다.
 *
 * 이 번호가 곧 /sys/kernel/iommu_groups/<id> 의 이름이라, 사용자 공간과
 * 커널이 같은 그룹을 가리키는 공통 언어가 된다. VFIO 가 사용자에게
 * 보여 주는 그룹 번호도 이것이다.
 *
 * 호출 체인: VFIO·로그 → [이 함수]
 */
int iommu_group_id(struct iommu_group *group)
{
	return group->id;	/* [한국어] 사용자 공간과 커널이 그룹을 가리키는 공통 번호 */
}
EXPORT_SYMBOL_GPL(iommu_group_id);

static struct iommu_group *get_pci_alias_group(struct pci_dev *pdev,
					       unsigned long *devfns);

/*
 * To consider a PCI device isolated, we require ACS to support Source
 * Validation, Request Redirection, Completer Redirection, and Upstream
 * Forwarding.  This effectively means that devices cannot spoof their
 * requester ID, requests and completions cannot be redirected, and all
 * transactions are forwarded upstream, even as it passes through a
 * bridge where the target device is downstream.
 */
/*
 * [한국어] 위 영어 주석이 이 파일 전체에서 가장 중요한 정의를 설명한다 --
 * PCI 장치를 "격리됐다"고 인정하려면 ACS 가 네 가지를 모두 지원해야 한다.
 *
 * 각각이 막는 것이 다르다. SV(Source Validation)는 장치가 남의 요청자 ID 를
 * 사칭하는 것을, RR/CR(Request/Completer Redirection)은 요청과 완료가
 * 다른 곳으로 돌려지는 것을, UF(Upstream Forwarding)는 브리지가 트래픽을
 * 상류로 보내지 않고 옆 장치로 바로 넘기는 것을 막는다.
 *
 * 마지막 하나가 특히 중요하다. UF 가 없으면 같은 스위치 아래의 두 장치가
 * IOMMU 를 거치지 않고 직접 주고받을 수 있어, 아무리 페이지 테이블을
 * 나눠도 격리가 성립하지 않는다. 그래서 이 네 가지가 모두 없으면 그
 * 장치들은 한 그룹으로 묶인다 -- 그룹 판정의 실질적 근거가 이 한 줄이다.
 */
#define REQ_ACS_FLAGS   (PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF)

/*
 * For multifunction devices which are not isolated from each other, find
 * all the other non-isolated functions and look for existing groups.  For
 * each function, we also need to look for aliases to or from other devices
 * that may already have a group.
 */
/*
 * [한국어] (위 영어 주석에 이어) 같은 슬롯의 다른 기능들 중 이미 그룹을
 * 가진 것을 찾는다.
 *
 * 왜 필요한가: 다기능 장치의 기능들은 물리적으로 한 칩이라, ACS 가
 * 없으면 서로의 DMA 를 가로챌 수 있다. 그런 기능들은 한 그룹이어야 하고,
 * 그중 하나가 이미 그룹을 가지고 있다면 나머지도 그리로 들어가야 한다.
 *
 * 세 가지 조건으로 후보를 좁히는 것에 주의할 것 -- 같은 버스, 같은 슬롯,
 * 그리고 ACS 가 없을 것. 앞의 둘은 "같은 물리 장치인가"이고, 마지막은
 * "정말 격리가 안 되는가"이다. ACS 가 있는 기능은 자기 그룹을 가질 수
 * 있으므로 후보에서 뺀다.
 *
 * 실행 컨텍스트: 장치 probe 경로. 잠들 수 있다.
 *
 * 호출 체인: get_pci_alias_group → [이 함수] → get_pci_alias_group (상호 재귀)
 */
static struct iommu_group *get_pci_function_alias_group(struct pci_dev *pdev,
							unsigned long *devfns)
{
	struct pci_dev *tmp = NULL;
	struct iommu_group *group;

	if (!pdev->multifunction || pci_acs_enabled(pdev, REQ_ACS_FLAGS))
		return NULL;	/* [한국어] 단일 기능이거나 ACS 로 격리된다 — 남과 묶일 이유가 없다 */

	for_each_pci_dev(tmp) {
		if (tmp == pdev || tmp->bus != pdev->bus ||	/* [한국어] 자기 자신과 다른 버스는 제외 */
		    PCI_SLOT(tmp->devfn) != PCI_SLOT(pdev->devfn) ||	/* [한국어] 같은 슬롯이어야 같은 물리 장치의 기능이다 */
		    pci_acs_enabled(tmp, REQ_ACS_FLAGS))	/* [한국어] ACS 가 있는 기능은 자기 그룹을 가질 수 있다 */
			continue;

		group = get_pci_alias_group(tmp, devfns);	/* [한국어] 그 기능이 별칭을 통해 이미 그룹에 속했는지 본다 */
		if (group) {
			pci_dev_put(tmp);	/* [한국어] for_each_pci_dev 가 든 참조 — 루프를 벗어나므로 손으로 놓는다 */
			return group;
		}
	}

	return NULL;	/* [한국어] 아무도 그룹을 갖고 있지 않다 — 호출자가 새로 만든다 */
}

/*
 * Look for aliases to or from the given device for existing groups. DMA
 * aliases are only supported on the same bus, therefore the search
 * space is quite small (especially since we're really only looking at pcie
 * device, and therefore only expect multiple slots on the root complex or
 * downstream switch ports).  It's conceivable though that a pair of
 * multifunction devices could have aliases between them that would cause a
 * loop.  To prevent this, we use a bitmap to track where we've been.
 */
/*
 * [한국어] (위 영어 주석에 이어) DMA 별칭을 따라가며 이미 있는 그룹을 찾는다.
 *
 * DMA 별칭이란: 어떤 장치는 자기 것이 아닌 요청자 ID 로 DMA 를 낸다.
 * 옛 PCI-to-PCI 브리지 뒤의 장치나, 하드웨어 버그로 잘못된 ID 를 쓰는
 * 장치가 그렇다. IOMMU 는 요청자 ID 로만 장치를 구별하므로, 같은 ID 를
 * 쓰는 장치들은 구별할 수 없고 따라서 한 그룹이어야 한다.
 *
 * 비트맵이 이 함수의 안전장치다. 위 영어 주석대로 두 다기능 장치가 서로를
 * 별칭으로 가리키면 재귀가 무한히 돈다. 그래서 방문한 devfn 을 표시해 두고
 * 두 번째 방문에서 곧바로 돌아온다 -- test_and_set 한 번으로 검사와 표시를
 * 함께 한다.
 *
 * 상호 재귀 구조에 주의할 것. 이 함수는 별칭을 따라가고,
 * get_pci_function_alias_group 은 같은 슬롯의 다른 기능을 따라간다. 둘이
 * 서로를 불러 "별칭이거나 같은 칩인" 장치들을 모두 훑는다.
 *
 * 실행 컨텍스트: 장치 probe 경로. 잠들 수 있다.
 *
 * 호출 체인: pci_device_group → [이 함수] ↔ get_pci_function_alias_group
 */
static struct iommu_group *get_pci_alias_group(struct pci_dev *pdev,
					       unsigned long *devfns)
{
	struct pci_dev *tmp = NULL;
	struct iommu_group *group;

	if (test_and_set_bit(pdev->devfn & 0xff, devfns))
		return NULL;	/* [한국어] 이미 왔던 곳이다 — 위 영어 주석이 말하는 순환을 여기서 끊는다 */

	group = iommu_group_get(&pdev->dev);
	if (group)
		return group;	/* [한국어] 이 장치가 이미 그룹에 속했다 — 참조를 든 채로 돌려준다 */

	for_each_pci_dev(tmp) {
		if (tmp == pdev || tmp->bus != pdev->bus)
			continue;	/* [한국어] 위 영어 주석대로 별칭은 같은 버스 안에서만 성립한다 */

		/* We alias them or they alias us */
		if (pci_devs_are_dma_aliases(pdev, tmp)) {	/* [한국어] 위 영어 주석대로 방향은 상관없다 — 한쪽이라도 같은 ID 를 쓰면 구별이 안 된다 */
			group = get_pci_alias_group(tmp, devfns);	/* [한국어] 그 장치의 별칭도 따라간다 */
			if (group) {
				pci_dev_put(tmp);
				return group;
			}

			group = get_pci_function_alias_group(tmp, devfns);	/* [한국어] 그 장치와 같은 칩인 기능들도 본다 */
			if (group) {
				pci_dev_put(tmp);
				return group;
			}
		}
	}

	return NULL;	/* [한국어] 연결된 어느 장치도 그룹을 갖고 있지 않다 */
}

/*
 * [한국어] pci_for_each_dma_alias 콜백이 결과를 담아 돌려주는 자리.
 * 콜백 시그니처가 void* 하나뿐이라, 두 값을 함께 넘기려면 이렇게 묶어야 한다.
 */
struct group_for_pci_data {
	/* [한국어] 마지막으로 본 장치. 별칭 사슬의 끝, 즉 IOMMU 에 실제로
	 * 보이는 요청자를 가리키게 된다. 그룹을 못 찾았을 때 이것을 기준으로
	 * 상류를 더 훑는다. */
	struct pci_dev *pdev;

	/* [한국어] 찾은 그룹. NULL 이면 사슬 어디에도 그룹이 없었다는 뜻이고,
	 * NULL 이 아니면 참조를 든 채로 담겨 있다. */
	struct iommu_group *group;
};

/*
 * DMA alias iterator callback, return the last seen device.  Stop and return
 * the IOMMU group if we find one along the way.
 */
/*
 * [한국어] (위 영어 주석에 이어) 별칭 사슬을 훑는 콜백.
 *
 * @pdev:   지금 보고 있는 장치
 * @alias:  그 장치가 쓰는 요청자 ID(여기서는 쓰지 않는다)
 * @opaque: 결과를 담을 group_for_pci_data
 * @return: 0 이 아니면 순회를 멈춘다 -- 그룹을 찾았다는 뜻이다.
 *
 * 매번 pdev 를 덮어쓰는 것이 의도다. 위 영어 주석대로 "마지막으로 본
 * 장치"를 남기는 것이 목적이며, 그룹을 못 찾고 끝나면 그 마지막 장치가
 * 별칭 사슬의 끝 -- IOMMU 에 실제로 보이는 요청자다.
 *
 * 반환값이 곧 "멈춤" 신호인 것도 pci_for_each_dma_alias 의 규약이다.
 * 그룹을 찾자마자 더 볼 이유가 없다.
 *
 * 실행 컨텍스트: 장치 probe 경로. 잠들 수 있다.
 *
 * 호출 체인: pci_device_group → pci_for_each_dma_alias → [이 함수]
 */
static int get_pci_alias_or_group(struct pci_dev *pdev, u16 alias, void *opaque)
{
	struct group_for_pci_data *data = opaque;

	data->pdev = pdev;	/* [한국어] 매번 덮어써 마지막으로 본 장치를 남긴다 */
	data->group = iommu_group_get(&pdev->dev);	/* [한국어] 있으면 참조를 든 채 담긴다 */

	return data->group != NULL;	/* [한국어] 찾았으면 0 이 아닌 값 — 순회가 여기서 멈춘다 */
}

/*
 * Generic device_group call-back function. It just allocates one
 * iommu-group per device.
 */
/*
 * [한국어] (위 영어 주석에 이어) 장치마다 그룹 하나를 주는 가장 단순한 정책.
 *
 * 언제 쓰나: 버스 구조상 장치들이 서로를 방해할 수 없는 경우다. 온칩
 * 장치들이 각자 고유한 스트림 ID 로 IOMMU 에 보이는 ARM SoC 가 그렇고,
 * 그런 곳에서는 별칭도 ACS 도 따질 것이 없다.
 *
 * PCI 의 pci_device_group() 과 대비된다 -- 그쪽은 토폴로지를 훑어야
 * 하지만 여기서는 무조건 새 그룹이면 된다.
 *
 * 호출 체인: iommu_group_get_for_dev → ops->device_group → [이 함수]
 */
struct iommu_group *generic_device_group(struct device *dev)
{
	return iommu_group_alloc();	/* [한국어] 서로 방해할 수 없는 버스라 판정이 필요 없다 */
}
EXPORT_SYMBOL_GPL(generic_device_group);

/*
 * Generic device_group call-back function. It just allocates one
 * iommu-group per iommu driver instance shared by every device
 * probed by that iommu driver.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * generic_single_device_group - IOMMU 인스턴스 하나당 그룹 하나를 공유한다
 *
 * @dev:    그룹을 정할 장치
 * @return: 참조가 하나 늘어난 그룹, 또는 에러 포인터
 *
 * device_group 콜백의 세 가지 표준 구현 중 가장 성긴 것이다. 셋을 비교하면 격리
 * 입도의 스펙트럼이 보인다.
 *   - generic_device_group: 장치마다 그룹 하나. 격리가 가장 촘촘하다.
 *   - pci_device_group:     ACS 와 DMA 별칭을 분석해 실제로 분리 가능한 최소 단위를
 *                           찾는다. PCIe 시스템의 표준.
 *   - 이 함수:              한 IOMMU 가 맡는 모든 장치가 한 그룹. 하드웨어가 장치를
 *                           구별하지 못하거나 컨텍스트 표가 하나뿐인 IOMMU 용이다.
 *
 * 마지막 그룹이 여기 있다는 점이 중요하다. 인스턴스가 singleton_group 참조를 계속
 * 들고 있으므로, 모든 장치가 떠나도 그룹은 살아 있다가 iommu_device_unregister
 * 에서 비로소 해제된다.
 *
 * 실행 컨텍스트: 프로브 경로. iommu_probe_device_lock 아래.
 *
 * 호출 체인: iommu_init_device → ops->device_group == [이 함수]
 */
struct iommu_group *generic_single_device_group(struct device *dev)
{
	struct iommu_device *iommu = dev->iommu->iommu_dev;	/* [한국어] 이 장치를 담당하는 IOMMU 인스턴스. 그룹을 인스턴스 단위로 하나만 두기 위한 기준점 */

	if (!iommu->singleton_group) {	/* [한국어] 이 IOMMU 의 공용 그룹이 아직 없으면 (첫 장치) */
		struct iommu_group *group;	/* [한국어] 새로 만들 그룹 */

		group = iommu_group_alloc();	/* [한국어] 그룹 하나를 만든다 */
		if (IS_ERR(group))	/* [한국어] 할당 실패 */
			return group;	/* [한국어] 에러 포인터를 그대로 올린다 — 호출자가 IS_ERR 로 판별한다 */
		iommu->singleton_group = group;	/* [한국어] 인스턴스에 매달아 둔다. 이후 모든 장치가 이것을 공유한다 */
	}
	return iommu_group_ref_get(iommu->singleton_group);	/* [한국어] 장치마다 참조를 하나씩 더 잡아 돌려준다. 마지막 장치가 나가도 인스턴스가 든 참조가 남아 그룹은 유지된다 (해제는 iommu_device_unregister 가 한다) */
}
EXPORT_SYMBOL_GPL(generic_single_device_group);

/*
 * Use standard PCI bus topology, isolation features, and DMA alias quirks
 * to find or create an IOMMU group for a device.
 */
/*
 * [한국어] (위 영어 주석에 이어) PCI 장치가 어느 그룹에 들어갈지 정한다.
 * 이 파일에서 그룹 판정의 실제 알고리즘이며, 네 단계를 순서대로 밟는다.
 *
 * 1. 별칭 사슬을 따라 올라가며 이미 그룹을 가진 장치를 찾는다. 요청자
 *    ID 를 공유하는 장치는 IOMMU 가 구별할 수 없으므로 같은 그룹이어야
 *    한다.
 * 2. 사슬의 끝에서 버스를 거슬러 올라가며, ACS 로 격리가 보장되는
 *    지점까지 간다. 그 아래의 브리지들은 트래픽을 옆으로 넘길 수 있어
 *    함께 묶여야 한다.
 * 3. 거기서 다시 별칭을, 4. 같은 슬롯의 다른 기능을 본다.
 *
 * 각 단계에서 기존 그룹을 찾으면 즉시 그것을 쓴다. 넷 다 실패해야
 * 비로소 새 그룹을 만든다 -- 이 순서가 "격리가 성립하는 가장 작은
 * 묶음"을 실제로 찾아내는 방법이다.
 *
 * devfns 비트맵을 단계 3, 4 가 공유하는 것에 주의할 것. 위 4번 단계의
 * 영어 주석대로 초기화하지 않는데, 이미 훑은 devfn 을 다시 볼 이유가
 * 없기 때문이다.
 *
 * 실행 컨텍스트: 장치 probe 경로. 잠들 수 있다.
 *
 * 호출 체인: iommu_group_get_for_dev → ops->device_group → [이 함수]
 */
struct iommu_group *pci_device_group(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct group_for_pci_data data;	/* [한국어] 별칭 순회의 결과를 받을 자리 */
	struct pci_bus *bus;	/* [한국어] 상류로 거슬러 올라갈 커서 */
	struct iommu_group *group = NULL;
	u64 devfns[4] = { 0 };	/* [한국어] 256개 devfn 을 담는 비트맵 — 별칭 순회의 순환 방지용이다 */

	if (WARN_ON(!dev_is_pci(dev)))
		return ERR_PTR(-EINVAL);	/* [한국어] PCI 전용 콜백에 다른 버스 장치가 왔다 */

	/*
	 * Find the upstream DMA alias for the device.  A device must not
	 * be aliased due to topology in order to have its own IOMMU group.
	 * If we find an alias along the way that already belongs to a
	 * group, use it.
	 */
	if (pci_for_each_dma_alias(pdev, get_pci_alias_or_group, &data))	/* [한국어] 1단계 — 위 영어 주석대로 별칭 사슬에 이미 그룹이 있으면 그것을 쓴다 */
		return data.group;

	pdev = data.pdev;	/* [한국어] 사슬의 끝 — IOMMU 에 실제로 보이는 요청자다. 여기서부터 상류를 본다 */

	/*
	 * Continue upstream from the point of minimum IOMMU granularity
	 * due to aliases to the point where devices are protected from
	 * peer-to-peer DMA by PCI ACS.  Again, if we find an existing
	 * group, use it.
	 */
	/* [한국어] 2단계 — 위 영어 주석대로 ACS 가 P2P DMA 를 막아 주는
	 * 지점까지 거슬러 올라간다. 그 아래는 모두 한 그룹이다. */
	for (bus = pdev->bus; !pci_is_root_bus(bus); bus = bus->parent) {
		if (!bus->self)
			continue;	/* [한국어] 브리지 없는 버스 — 올라갈 다리가 없으니 건너뛴다 */

		if (pci_acs_path_enabled(bus->self, NULL, REQ_ACS_FLAGS))
			break;	/* [한국어] 여기부터 상류는 격리된다 — 더 묶을 필요가 없다 */

		pdev = bus->self;	/* [한국어] 격리가 없으니 이 브리지까지 같은 그룹이다 */

		group = iommu_group_get(&pdev->dev);
		if (group)
			return group;	/* [한국어] 브리지가 이미 그룹을 가졌다면 그리로 들어간다 */
	}

	/*
	 * Look for existing groups on device aliases.  If we alias another
	 * device or another device aliases us, use the same group.
	 */
	/* [한국어] 3단계 — 올라간 지점에서 다시 별칭을 본다. 상류로 올라오며
	 * 기준 장치가 바뀌었으므로 별칭 관계도 달라진다. */
	group = get_pci_alias_group(pdev, (unsigned long *)devfns);
	if (group)
		return group;

	/*
	 * Look for existing groups on non-isolated functions on the same
	 * slot and aliases of those funcions, if any.  No need to clear
	 * the search bitmap, the tested devfns are still valid.
	 */
	/* [한국어] 4단계 — 같은 슬롯의 격리되지 않은 기능들. 위 영어 주석대로
	 * 비트맵을 지우지 않는데, 이미 훑은 devfn 은 다시 볼 이유가 없다. */
	group = get_pci_function_alias_group(pdev, (unsigned long *)devfns);
	if (group)
		return group;

	/* No shared group found, allocate new */
	return iommu_group_alloc();	/* [한국어] 네 단계 모두 빈손 — 이 장치는 혼자 격리된다 */
}
EXPORT_SYMBOL_GPL(pci_device_group);

/* Get the IOMMU group for device on fsl-mc bus */
/*
 * [한국어] (위 영어 주석에 이어) fsl-mc 버스의 그룹 판정.
 *
 * PCI 처럼 토폴로지를 훑을 필요가 없다. 이 버스에서는 컨테이너 장치가
 * 격리 경계이며, 같은 컨테이너 안의 장치들은 모두 한 그룹이다. 그래서
 * 컨테이너를 찾아 그 그룹을 쓰고, 없으면 새로 만드는 두 줄로 끝난다.
 *
 * 호출 체인: iommu_group_get_for_dev → ops->device_group → [이 함수]
 */
struct iommu_group *fsl_mc_device_group(struct device *dev)
{
	struct device *cont_dev = fsl_mc_cont_dev(dev);	/* [한국어] 이 버스의 격리 경계는 컨테이너다 */
	struct iommu_group *group;

	group = iommu_group_get(cont_dev);	/* [한국어] 같은 컨테이너의 다른 장치가 이미 만들었을 수 있다 */
	if (!group)
		group = iommu_group_alloc();	/* [한국어] 이 컨테이너의 첫 장치다 */
	return group;
}
EXPORT_SYMBOL_GPL(fsl_mc_device_group);

/*
 * [한국어]
 * __iommu_alloc_identity_domain - 주소를 그대로 통과시키는 도메인을 얻는다
 *
 * @dev: 이 장치의 IOMMU 에게 묻는다
 * @return: 항등 도메인. 지원하지 않으면 -EOPNOTSUPP.
 *
 * 드라이버가 항등 도메인을 제공하는 방식이 두 가지라 갈래가 둘이다.
 * 하나는 정적 도메인 하나를 미리 만들어 두고 모두가 공유하는 방식이고
 * (ops->identity_domain), 다른 하나는 장치마다 만들어 주는 방식이다.
 *
 * 앞쪽 경로가 iommu_domain_init 을 거치지 않는 것에 주의할 것. 공유
 * 도메인은 드라이버가 이미 초기화해 둔 것이라 다시 손대면 안 된다.
 * 뒤쪽만 갓 만들어진 것이라 공통 필드를 채운다.
 *
 * 실행 컨텍스트: 도메인 생성 경로. 잠들 수 있다.
 *
 * 호출 체인: __iommu_group_alloc_default_domain → [이 함수] → ops
 */
static struct iommu_domain *__iommu_alloc_identity_domain(struct device *dev)
{
	const struct iommu_ops *ops = dev_iommu_ops(dev);
	struct iommu_domain *domain;

	if (ops->identity_domain)
		return ops->identity_domain;	/* [한국어] 드라이버가 미리 만들어 공유하는 도메인 — 이미 초기화돼 있다 */

	if (ops->domain_alloc_identity) {
		domain = ops->domain_alloc_identity(dev);	/* [한국어] 장치마다 만들어 주는 방식 */
		if (IS_ERR(domain))
			return domain;
	} else {
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 둘 다 없다 — 이 IOMMU 는 통과 모드를 지원하지 않는다 */
	}

	iommu_domain_init(domain, IOMMU_DOMAIN_IDENTITY, ops);	/* [한국어] 갓 만든 것만 공통 필드를 채운다 */
	return domain;
}

/*
 * [한국어]
 * __iommu_group_alloc_default_domain - 요청받은 종류의 기본 도메인을 실제로 만든다
 *
 * @group:    도메인을 세울 그룹
 * @req_type: 만들 종류. 0 은 여기까지 오지 않는다 — 상위에서 이미 해석된 값이다.
 * @return:   도메인 포인터, 또는 에러 포인터
 *
 * 기본 도메인 결정은 3층 구조다. 이 함수는 그 맨 아래층으로, "무엇을 만들지"는
 * 이미 정해진 상태에서 "어떻게 만들지"만 담당한다. 위층인
 * iommu_group_alloc_default_domain 이 시스템 기본값 → DMA 로 물러서는 정책을 갖고,
 * 그 위의 iommu_setup_default_domain 이 그룹 전체에 적용한다.
 *
 * 두 갈래로 나뉜다. 페이지 테이블이 필요한 종류는 __iommu_paging_domain_alloc_flags
 * 로, 항등 도메인은 __iommu_alloc_identity_domain 으로 간다.
 *
 * PASID 선제 할당이 이 함수의 미묘한 부분이다. 장치가 PASID 를 낼 수 있으면 나중에
 * SVA 를 붙일 가능성이 있는데, 하드웨어에 따라 RID 도메인과 PASID 도메인의 페이지
 * 테이블 포맷이 달라 도메인을 통째로 갈아야 할 수 있다. 그래서 처음부터 PASID 호환
 * 으로 만들어 두고, 드라이버가 지원하지 않으면(-EOPNOTSUPP) 조용히 평범한 도메인
 * 으로 물러선다.
 *
 * 실행 컨텍스트: 그룹 락을 든 채. 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: iommu_group_alloc_default_domain → [이 함수]
 *            → __iommu_paging_domain_alloc_flags / __iommu_alloc_identity_domain
 */
static struct iommu_domain *
__iommu_group_alloc_default_domain(struct iommu_group *group, int req_type)
{
	struct device *dev = iommu_group_first_dev(group);	/* [한국어] 도메인 할당은 장치 하나를 대표로 삼아 드라이버에게 요청한다 — 그룹의 모든 장치가 같은 IOMMU 를 쓰므로 어느 것이든 답이 같다 */
	struct iommu_domain *dom;	/* [한국어] 할당 결과 */

	if (group->default_domain && group->default_domain->type == req_type)	/* [한국어] 이미 원하는 종류의 기본 도메인이 서 있으면 */
		return group->default_domain;	/* [한국어] 다시 만들지 않고 그대로 쓴다 — sysfs 로 같은 종류를 재요청한 경우 등 */

	/*
	 * When allocating the DMA API domain assume that the driver is going to
	 * use PASID and make sure the RID's domain is PASID compatible.
	 */
	if (req_type & __IOMMU_DOMAIN_PAGING) {	/* [한국어] 페이지 테이블이 필요한 종류(DMA/DMA_FQ/UNMANAGED) */
		dom = __iommu_paging_domain_alloc_flags(dev, req_type,	/* [한국어] 번역 도메인을 만든다 */
			   dev->iommu->max_pasids ? IOMMU_HWPT_ALLOC_PASID : 0);	/* [한국어] PASID 를 낼 수 있는 장치라면 처음부터 PASID 호환 도메인으로 만든다. RID 도메인과 PASID 도메인의 페이지 테이블 포맷이 다른 하드웨어가 있어, 나중에 SVA 를 붙이려 할 때 갈아엎지 않으려는 선제 조치다 (위 영어 주석) */

		/*
		 * If driver does not support PASID feature then
		 * try to allocate non-PASID domain
		 */
		if (PTR_ERR(dom) == -EOPNOTSUPP)	/* [한국어] 드라이버가 PASID 호환 도메인을 지원하지 않는다면 */
			dom = __iommu_paging_domain_alloc_flags(dev, req_type, 0);	/* [한국어] 플래그 없이 평범한 도메인으로 다시 시도한다 — PASID 를 포기할 뿐 DMA 자체는 살린다 */

		return dom;	/* [한국어] 성공한 도메인 또는 에러 포인터 */
	}

	if (req_type == IOMMU_DOMAIN_IDENTITY)	/* [한국어] 번역 없는 통과 도메인 요청 */
		return __iommu_alloc_identity_domain(dev);	/* [한국어] 드라이버의 정적 identity_domain 이나 domain_alloc_identity 로 만든다 */

	return ERR_PTR(-EINVAL);	/* [한국어] 기본 도메인이 될 수 없는 종류 (BLOCKED 등) */
}

/*
 * req_type of 0 means "auto" which means to select a domain based on
 * iommu_def_domain_type or what the driver actually supports.
 */
/*
 * [한국어] (위 영어 주석에 이어) 그룹의 기본 도메인을 정해 만든다.
 *
 * 세 층으로 결정된다. 먼저 드라이버가 아예 도메인을 못박아 둔 경우
 * (ops->default_domain), 위 영어 주석대로 옛 드라이버를 위한 것이며
 * 요청과 다르면 거절한다 -- 협상의 여지가 없는 구성이다.
 *
 * 그다음 호출자가 종류를 지정했으면 그대로 시도한다.
 *
 * 지정하지 않았으면(req_type 0) 시스템 기본값으로 시도하고, 실패하면
 * DMA 로 물러선다. 이 물러섬이 이 함수의 핵심이다 -- 예컨대 기본이
 * DMA_FQ 인데 드라이버가 플러시 큐를 지원하지 않으면, 통째로 실패하는
 * 대신 평범한 DMA 도메인으로 내려앉는다. 격리는 유지하면서 성능만
 * 포기하는 쪽을 고르는 것이다.
 *
 * 기본값이 이미 DMA 일 때 곧바로 거절하는 이유: 물러설 곳이 같은
 * 종류라 다시 시도해 봐야 같은 실패다.
 *
 * 실행 컨텍스트: 그룹 설정 경로. group->mutex 를 든 채 불린다.
 *
 * 호출 체인: iommu_setup_default_domain → [이 함수]
 */
static struct iommu_domain *
iommu_group_alloc_default_domain(struct iommu_group *group, int req_type)
{
	const struct iommu_ops *ops = dev_iommu_ops(iommu_group_first_dev(group));	/* [한국어] 그룹의 장치는 모두 같은 IOMMU 아래라 아무거나 하나면 된다 */
	struct iommu_domain *dom;

	lockdep_assert_held(&group->mutex);

	/*
	 * Allow legacy drivers to specify the domain that will be the default
	 * domain. This should always be either an IDENTITY/BLOCKED/PLATFORM
	 * domain. Do not use in new drivers.
	 */
	if (ops->default_domain) {	/* [한국어] 위 영어 주석대로 옛 드라이버용 경로 — 새 드라이버는 쓰지 말라고 못박아 두었다 */
		if (req_type != ops->default_domain->type)
			return ERR_PTR(-EINVAL);	/* [한국어] 드라이버가 못박은 종류라 협상의 여지가 없다 */
		return ops->default_domain;
	}

	if (req_type)
		return __iommu_group_alloc_default_domain(group, req_type);	/* [한국어] 호출자가 지정했으면 그대로 — 물러서지 않는다 */

	/* The driver gave no guidance on what type to use, try the default */
	dom = __iommu_group_alloc_default_domain(group, iommu_def_domain_type);	/* [한국어] 시스템 기본값으로 먼저 */
	if (!IS_ERR(dom))
		return dom;

	/* Otherwise IDENTITY and DMA_FQ defaults will try DMA */
	if (iommu_def_domain_type == IOMMU_DOMAIN_DMA)
		return ERR_PTR(-EINVAL);	/* [한국어] 물러설 곳이 같은 종류라 다시 시도해도 같은 실패다 */
	dom = __iommu_group_alloc_default_domain(group, IOMMU_DOMAIN_DMA);	/* [한국어] 격리는 지키고 성능만 포기하는 쪽으로 내려앉는다 */
	if (IS_ERR(dom))
		return dom;

	pr_warn("Failed to allocate default IOMMU domain of type %u for group %s - Falling back to IOMMU_DOMAIN_DMA",	/* [한국어] 요청과 다른 구성이 됐음을 반드시 알린다 */
		iommu_def_domain_type, group->name);
	return dom;
}

/*
 * [한국어]
 * iommu_group_default_domain - 그룹의 기본 도메인을 꺼낸다
 *
 * @group: 대상 그룹
 * @return: 기본 도메인. 아직 세우지 않았으면 NULL.
 *
 * VFIO 가 장치를 돌려줄 때 어디로 되돌려야 하는지 묻는 통로다.
 *
 * 호출 체인: VFIO/iommufd → [이 함수]
 */
struct iommu_domain *iommu_group_default_domain(struct iommu_group *group)
{
	return group->default_domain;	/* [한국어] 소유자가 장치를 놓을 때 돌아갈 자리 */
}

/*
 * [한국어]
 * probe_iommu_group - 버스 순회 콜백. 장치 하나를 IOMMU 아래로 들인다
 *
 * @dev:  검사할 장치
 * @data: 새로 만들어진 그룹들을 모을 목록
 * @return: 0 이면 순회를 계속한다
 *
 * -ENODEV 를 성공으로 바꾸는 것이 이 함수의 전부다. 버스의 장치 대부분은
 * IOMMU 아래에 들어갈 대상이 아니고, 그것은 오류가 아니라 정상이다.
 * 그대로 올리면 bus_for_each_dev 가 첫 비대상 장치에서 순회를 멈춘다.
 *
 * 실행 컨텍스트: 버스 훑기. 잠들 수 있다.
 *
 * 호출 체인: bus_iommu_probe → bus_for_each_dev → [이 함수]
 */
static int probe_iommu_group(struct device *dev, void *data)
{
	struct list_head *group_list = data;
	int ret;

	mutex_lock(&iommu_probe_device_lock);	/* [한국어] 같은 그룹을 두 경로가 동시에 만들지 못하게 한다 */
	ret = __iommu_probe_device(dev, group_list);
	mutex_unlock(&iommu_probe_device_lock);
	if (ret == -ENODEV)
		ret = 0;	/* [한국어] IOMMU 대상이 아닌 장치가 대부분이다 — 여기서 멈추면 안 된다 */

	return ret;
}

/*
 * [한국어]
 * iommu_bus_notifier - 장치가 나타나고 사라지는 것을 버스에서 통지받는다
 *
 * @nb:     등록해 둔 통지 블록
 * @action: 무슨 일이 일어났는가
 * @data:   해당 장치
 * @return: NOTIFY_OK / NOTIFY_DONE
 *
 * 이 파일이 세상과 이어지는 지점이다. iommu_subsys_init 이 버스마다
 * 하나씩 걸어 두었고, 이후로는 장치가 생길 때마다 여기로 들어온다.
 *
 * 두 시점만 다루는 것에 주의할 것. ADD 는 장치가 등록된 직후이고
 * REMOVED 는 완전히 사라진 뒤다 -- 그 사이의 BIND/UNBIND 는 드라이버가
 * 붙고 떨어지는 것일 뿐 IOMMU 소속과는 무관하다.
 *
 * 실패해도 NOTIFY_DONE 일 뿐 장치 등록 자체를 막지 않는다. IOMMU 아래로
 * 들이지 못한 장치도 (보호받지 못한 채) 동작은 해야 하기 때문이다.
 *
 * 실행 컨텍스트: 버스 통지 사슬. 잠들 수 있다.
 *
 * 호출 체인: 버스 계층 → [이 함수] → iommu_probe_device / iommu_release_device
 */
static int iommu_bus_notifier(struct notifier_block *nb,
			      unsigned long action, void *data)
{
	struct device *dev = data;

	if (action == BUS_NOTIFY_ADD_DEVICE) {	/* [한국어] 장치가 막 등록됐다 — 그룹을 정하고 도메인을 붙일 때다 */
		int ret;

		ret = iommu_probe_device(dev);
		return (ret) ? NOTIFY_DONE : NOTIFY_OK;	/* [한국어] 실패해도 장치 등록을 막지는 않는다 */
	} else if (action == BUS_NOTIFY_REMOVED_DEVICE) {	/* [한국어] 완전히 사라진 뒤 — 그룹에서 뺀다 */
		iommu_release_device(dev);
		return NOTIFY_OK;
	}

	return 0;	/* [한국어] BIND/UNBIND 등 나머지는 IOMMU 소속과 무관하다 */
}

/*
 * Combine the driver's chosen def_domain_type across all the devices in a
 * group. Drivers must give a consistent result.
 */
/*
 * [한국어] (위 영어 주석에 이어) 그룹 안 장치들의 요구를 하나로 합친다.
 *
 * 왜 합쳐야 하는가: 그룹의 장치들은 같은 도메인을 공유하므로 종류가
 * 하나여야 한다. 그런데 종류를 정하는 것은 장치마다 불리는 드라이버
 * 콜백이라, 장치마다 다른 답이 나올 수 있다.
 *
 * 이 함수는 장치 하나를 볼 때마다 불려 지금까지의 결론(cur_type)과
 * 이번 답을 합친다. 0 은 "의견 없음"이라 상대의 답을 그대로 받고,
 * 같으면 문제가 없다.
 *
 * 둘 다 의견이 있는데 다르면 그것은 위 영어 주석대로 드라이버 버그다.
 * 같은 그룹의 장치들에 서로 다른 종류를 요구한다는 것은 성립할 수
 * 없는 요청이기 때문이다. 그래도 부팅을 막지 않고 IDENTITY 쪽을 택해
 * 밀고 나간다 -- 통과 모드가 더 관대해서 어느 장치든 동작은 하기
 * 때문이며, 격리를 잃는 대신 시스템이 뜨는 쪽을 고른 것이다.
 *
 * 실행 컨텍스트: 그룹 설정 경로. group->mutex 를 든 채 불린다.
 *
 * 호출 체인: iommu_get_default_domain_type → [이 함수] → ops->def_domain_type
 */
static int iommu_get_def_domain_type(struct iommu_group *group,
				     struct device *dev, int cur_type)
{
	const struct iommu_ops *ops = dev_iommu_ops(dev);
	int type;

	if (ops->default_domain) {
		/*
		 * Drivers that declare a global static default_domain will
		 * always choose that.
		 */
		/* [한국어] 위 영어 주석대로 정적 도메인을 선언한 드라이버는
		 * 선택의 여지가 없다 — 그 종류가 곧 답이다. */
		type = ops->default_domain->type;
	} else {
		if (ops->def_domain_type)
			type = ops->def_domain_type(dev);	/* [한국어] 장치를 보고 드라이버가 판단한다 */
		else
			return cur_type;	/* [한국어] 의견이 없는 드라이버 — 지금까지의 결론을 그대로 둔다 */
	}
	if (!type || cur_type == type)
		return cur_type;	/* [한국어] 0 은 "상관없음". 같으면 합칠 것도 없다 */
	if (!cur_type)
		return type;	/* [한국어] 첫 의견이다 */

	dev_err_ratelimited(	/* [한국어] 여기 왔다면 드라이버가 성립할 수 없는 요구를 한 것이다 */
		dev,
		"IOMMU driver error, requesting conflicting def_domain_type, %s and %s, for devices in group %u.\n",
		iommu_domain_type_str(cur_type), iommu_domain_type_str(type),
		group->id);

	/*
	 * Try to recover, drivers are allowed to force IDENTITY or DMA, IDENTITY
	 * takes precedence.
	 */
	/* [한국어] 위 영어 주석대로 부팅을 막는 대신 통과 모드를 택한다.
	 * 더 관대해서 어느 장치든 동작은 하기 때문이며, 격리를 잃는 대신
	 * 시스템이 뜨는 쪽을 고른 것이다. */
	if (type == IOMMU_DOMAIN_IDENTITY)
		return type;
	return cur_type;
}

/*
 * A target_type of 0 will select the best domain type. 0 can be returned in
 * this case meaning the global default should be used.
 */
/*
 * [한국어] (위 영어 주석에 이어) 그룹 전체를 훑어 최종 도메인 종류를 정한다.
 *
 * 세 가지가 결론에 개입하며, 뒤로 갈수록 강하다.
 *
 * 먼저 드라이버들의 의견을 합친다(위 함수). 그다음 빌드 설정이 개입해,
 * 공통 DMA ops 가 없는 커널에서는 DMA 도메인 자체를 쓸 수 없으므로
 * IDENTITY 로 돌린다.
 *
 * 마지막이 신뢰할 수 없는 장치다. Thunderbolt 로 꽂은 외장 장치처럼
 * 물리적으로 접근 가능한 것들이며, 그런 장치에는 반드시 번역을 걸어야
 * 한다. 통과시키면 사용자가 케이블 하나로 호스트 메모리 전체를 읽을 수
 * 있기 때문이다. 그래서 드라이버가 다른 종류를 요구했다면 그 요구를
 * 꺾는 것이 아니라 아예 probe 를 거절한다 -- 격리를 포기하느니 그
 * 장치를 쓰지 않는 쪽이다.
 *
 * target_type 은 sysfs 로 사용자가 지정한 값이며, 드라이버 의견과
 * 충돌하면 역시 거절한다.
 *
 * @group:       대상 그룹
 * @target_type: 사용자가 지정한 종류. 0 이면 자동.
 * @return: 정해진 종류. 0 이면 전역 기본값을 쓰라는 뜻. -1 이면 거절.
 *
 * 실행 컨텍스트: 그룹 설정 경로. group->mutex 를 든 채 불린다.
 *
 * 호출 체인: iommu_setup_default_domain → [이 함수]
 */
static int iommu_get_default_domain_type(struct iommu_group *group,
					 int target_type)
{
	struct device *untrusted = NULL;	/* [한국어] 신뢰할 수 없는 장치를 하나라도 찾으면 여기 담긴다 */
	struct group_device *gdev;
	int driver_type = 0;	/* [한국어] 0 은 아직 의견 없음 */

	lockdep_assert_held(&group->mutex);

	/*
	 * ARM32 drivers supporting CONFIG_ARM_DMA_USE_IOMMU can declare an
	 * identity_domain and it will automatically become their default
	 * domain. Later on ARM_DMA_USE_IOMMU will install its UNMANAGED domain.
	 * Override the selection to IDENTITY.
	 */
	if (IS_ENABLED(CONFIG_ARM_DMA_USE_IOMMU)) {
		static_assert(!(IS_ENABLED(CONFIG_ARM_DMA_USE_IOMMU) &&
				IS_ENABLED(CONFIG_IOMMU_DMA)));
		driver_type = IOMMU_DOMAIN_IDENTITY;
	}

	for_each_group_device(group, gdev) {
		driver_type = iommu_get_def_domain_type(group, gdev->dev,
							driver_type);

		if (dev_is_pci(gdev->dev) && to_pci_dev(gdev->dev)->untrusted) {
			/*
			 * No ARM32 using systems will set untrusted, it cannot
			 * work.
			 */
			if (WARN_ON(IS_ENABLED(CONFIG_ARM_DMA_USE_IOMMU)))
				return -1;
			untrusted = gdev->dev;
		}
	}

	/*
	 * If the common dma ops are not selected in kconfig then we cannot use
	 * IOMMU_DOMAIN_DMA at all. Force IDENTITY if nothing else has been
	 * selected.
	 */
	if (!IS_ENABLED(CONFIG_IOMMU_DMA)) {
		if (WARN_ON(driver_type == IOMMU_DOMAIN_DMA))
			return -1;
		if (!driver_type)
			driver_type = IOMMU_DOMAIN_IDENTITY;
	}

	if (untrusted) {	/* [한국어] Thunderbolt 외장 장치 등 물리적으로 접근 가능한 것 */
		if (driver_type && driver_type != IOMMU_DOMAIN_DMA) {
			dev_err_ratelimited(	/* [한국어] 격리를 포기하느니 이 장치를 쓰지 않는 쪽을 고른다 */
				untrusted,
				"Device is not trusted, but driver is overriding group %u to %s, refusing to probe.\n",
				group->id, iommu_domain_type_str(driver_type));
			return -1;
		}
		driver_type = IOMMU_DOMAIN_DMA;	/* [한국어] 반드시 번역 — 통과시키면 케이블 하나로 호스트 메모리를 읽을 수 있다 */
	}

	if (target_type) {	/* [한국어] sysfs 로 사용자가 지정한 값 */
		if (driver_type && target_type != driver_type)
			return -1;	/* [한국어] 드라이버가 못박은 것과 충돌하면 거절한다 */
		return target_type;
	}
	return driver_type;	/* [한국어] 0 이면 호출자가 전역 기본값을 쓴다 */
}

/*
 * [한국어]
 * iommu_group_do_probe_finalize - 도메인까지 붙은 뒤 드라이버에 마무리를 맡긴다
 *
 * @dev: 대상 장치
 * @return: 없음
 *
 * 왜 별도 단계인가: 일부 드라이버는 장치가 실제로 도메인에 붙은 뒤에야
 * 할 수 있는 일이 있다. probe 단계에서는 아직 도메인이 없어 그 작업을
 * 할 수 없으므로, 모든 설정이 끝난 뒤 한 번 더 불러 준다.
 *
 * 콜백이 없는 드라이버가 대부분이라 조용히 넘어간다.
 *
 * 호출 체인: bus_iommu_probe / iommu_probe_device → [이 함수] → ops->probe_finalize
 */
static void iommu_group_do_probe_finalize(struct device *dev)
{
	const struct iommu_ops *ops = dev_iommu_ops(dev);

	if (ops->probe_finalize)
		ops->probe_finalize(dev);	/* [한국어] 도메인이 붙은 뒤에야 할 수 있는 일을 드라이버가 여기서 한다 */
}

/*
 * [한국어]
 * bus_iommu_probe - 버스에 이미 붙어 있던 장치들을 뒤늦게 IOMMU 아래로 들인다
 *
 * @bus: 훑을 버스
 * @return: 0 이면 성공, 음수 errno
 *
 * 왜 필요한가: 통지는 앞으로 나타날 장치만 알려 준다. IOMMU 드라이버가
 * 늦게 적재되면 그전에 이미 등록된 장치들은 통지를 놓치므로, 등록 시점에
 * 한 번 훑어 따라잡아야 한다.
 *
 * 두 단계로 나뉜 것이 이 함수의 구조다. 먼저 모든 장치를 그룹에 넣기만
 * 하고(1단계), 그다음 그룹마다 기본 도메인을 세운다(2단계). 위 영어
 * 주석이 그 이유를 밝힌다 -- 도메인 종류는 그룹 안 모든 장치의 의견을
 * 합쳐 정해야 하고, 예약 구간도 모든 장치의 것을 모아야 한다. 장치를
 * 하나 넣을 때마다 도메인을 세우면 그 둘 다 불가능하다.
 *
 * 마지막 probe_finalize 를 락 밖에서 부르는 것에 주의할 것. 위 FIXME 가
 * 밝히듯 일부 ARM 드라이버의 콜백이 코어로 되돌아와 같은 락을 잡으려
 * 하므로, 들고 있으면 교착이 된다. 상류도 이것을 잘못된 상태로 인정하고
 * 있다.
 *
 * 실행 컨텍스트: IOMMU 등록 경로. 잠들 수 있다.
 *
 * 호출 체인: iommu_device_register → [이 함수] → iommu_setup_default_domain
 */
static int bus_iommu_probe(const struct bus_type *bus)
{
	struct iommu_group *group, *next;	/* [한국어] 목록에서 빼며 도므로 _safe 가 필요하다 */
	LIST_HEAD(group_list);	/* [한국어] 1단계에서 새로 만들어진 그룹들이 여기 모인다 */
	int ret;

	ret = bus_for_each_dev(bus, NULL, &group_list, probe_iommu_group);	/* [한국어] 1단계 — 장치를 그룹에 넣기만 한다 */
	if (ret)
		return ret;

	list_for_each_entry_safe(group, next, &group_list, entry) {	/* [한국어] 2단계 — 그룹이 완성된 뒤에야 도메인을 세운다 */
		struct group_device *gdev;

		mutex_lock(&group->mutex);

		/* Remove item from the list */
		list_del_init(&group->entry);	/* [한국어] _init 까지 하는 이유: 이 고리는 나중에 다시 쓰인다 */

		/*
		 * We go to the trouble of deferred default domain creation so
		 * that the cross-group default domain type and the setup of the
		 * IOMMU_RESV_DIRECT will work correctly in non-hotpug scenarios.
		 */
		/* [한국어] 위 영어 주석이 두 단계로 나눈 이유다 — 도메인 종류는
		 * 그룹 전체의 의견을 합쳐야 하고, 항등 매핑도 모든 장치의 예약
		 * 구간을 모아야 정확하다. */
		ret = iommu_setup_default_domain(group, 0);	/* [한국어] 0 = 자동 선택 */
		if (ret) {
			mutex_unlock(&group->mutex);
			return ret;	/* [한국어] 이미 처리한 그룹들은 그대로 둔다 — 되돌릴 수 없다 */
		}
		for_each_group_device(group, gdev)
			iommu_setup_dma_ops(gdev->dev, group->default_domain);	/* [한국어] 이제 이 장치의 dma_map_* 이 IOMMU 를 거친다 */
		mutex_unlock(&group->mutex);

		/*
		 * FIXME: Mis-locked because the ops->probe_finalize() call-back
		 * of some IOMMU drivers calls arm_iommu_attach_device() which
		 * in-turn might call back into IOMMU core code, where it tries
		 * to take group->mutex, resulting in a deadlock.
		 */
		/* [한국어] 위 FIXME 대로 락을 놓고 부른다. 일부 ARM 드라이버의
		 * 콜백이 코어로 되돌아와 같은 락을 잡으려 하기 때문이며,
		 * 상류도 이것을 잘못된 상태로 인정하고 있다. */
		for_each_group_device(group, gdev)
			iommu_group_do_probe_finalize(gdev->dev);
	}

	return 0;
}

/**
 * device_iommu_capable() - check for a general IOMMU capability
 * @dev: device to which the capability would be relevant, if available
 * @cap: IOMMU capability
 *
 * Return: true if an IOMMU is present and supports the given capability
 * for the given device, otherwise false.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) IOMMU 능력을 묻는다.
 *
 * 두 번의 "없으면 false" 가 이 함수의 전부다. IOMMU 아래에 없는 장치와,
 * 능력 질의를 구현하지 않은 드라이버 모두 "그 능력이 없다"로 답한다 --
 * 능력이 있다고 잘못 답하는 것보다 없다고 답하는 쪽이 안전하기 때문이다.
 *
 * 호출 체인: VFIO·DMA 계층 → [이 함수] → ops->capable
 */
bool device_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	const struct iommu_ops *ops;

	if (!dev_has_iommu(dev))
		return false;	/* [한국어] IOMMU 아래에 없으면 어떤 능력도 없다 */

	ops = dev_iommu_ops(dev);
	if (!ops->capable)
		return false;	/* [한국어] 묻지 않은 것은 없는 것으로 — 안전한 쪽으로 답한다 */

	return ops->capable(dev, cap);
}
EXPORT_SYMBOL_GPL(device_iommu_capable);

/**
 * iommu_group_has_isolated_msi() - Compute msi_device_has_isolated_msi()
 *       for a group
 * @group: Group to query
 *
 * IOMMU groups should not have differing values of
 * msi_device_has_isolated_msi() for devices in a group. However nothing
 * directly prevents this, so ensure mistakes don't result in isolation failures
 * by checking that all the devices are the same.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 그룹의 MSI 가 격리돼 있는지 묻는다.
 *
 * 왜 중요한가: MSI 는 장치가 특정 주소에 쓰기를 해서 인터럽트를 내는
 * 방식이다. 그 주소가 격리되지 않으면, 사용자 공간에 넘긴 장치가 임의의
 * 인터럽트를 호스트에 주입할 수 있다. 그래서 VFIO 가 장치를 넘기기 전에
 * 이것을 확인한다.
 *
 * &= 로 모아 하나라도 거짓이면 전체가 거짓이 되는 것이 요점이다. 위
 * 영어 주석대로 그룹 안에서 값이 갈리는 일은 없어야 하지만 막을 방법이
 * 없어, 갈렸을 때 안전한 쪽(격리 안 됨)으로 답하도록 해 둔 것이다.
 *
 * 실행 컨텍스트: VFIO 의 장치 인계 경로. 잠들 수 있다.
 *
 * 호출 체인: VFIO/iommufd → [이 함수]
 */
bool iommu_group_has_isolated_msi(struct iommu_group *group)
{
	struct group_device *group_dev;
	bool ret = true;	/* [한국어] 하나씩 &= 로 깎아 나간다 */

	mutex_lock(&group->mutex);
	for_each_group_device(group, group_dev)
		ret &= msi_device_has_isolated_msi(group_dev->dev);	/* [한국어] 하나라도 격리되지 않으면 그룹 전체가 격리되지 않은 것이다 */
	mutex_unlock(&group->mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_group_has_isolated_msi);

/**
 * iommu_set_fault_handler() - set a fault handler for an iommu domain
 * @domain: iommu domain
 * @handler: fault handler
 * @token: user data, will be passed back to the fault handler
 *
 * This function should be used by IOMMU users which want to be notified
 * whenever an IOMMU fault happens.
 *
 * The fault handler itself should return 0 on success, and an appropriate
 * error code otherwise.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 도메인에 폴트 처리기를 건다.
 *
 * cookie_type 검사가 이 함수의 핵심이다. 도메인은 사용자별 상태를 담는
 * 자리를 하나만 가지고 있고, 그것을 DMA API 가 쓰거나(IOMMU_COOKIE_DMA)
 * 폴트 처리기가 쓰거나 하나만 가능하다. 이미 다른 용도로 쓰이는 도메인에
 * 처리기를 걸면 그쪽 상태를 덮어써 버리므로, 아직 비어 있을 때만 허용한다.
 *
 * 호출 체인: IOMMU 사용자(드라이버·VFIO) → [이 함수]
 */
void iommu_set_fault_handler(struct iommu_domain *domain,
					iommu_fault_handler_t handler,
					void *token)
{
	if (WARN_ON(!domain || domain->cookie_type != IOMMU_COOKIE_NONE))
		return;	/* [한국어] 자리가 이미 다른 용도로 쓰이고 있다 — 덮어쓰면 그쪽이 깨진다 */

	domain->cookie_type = IOMMU_COOKIE_FAULT_HANDLER;	/* [한국어] 이제 이 도메인의 쿠키는 폴트 처리기 것이다 */
	domain->handler = handler;
	domain->handler_token = token;	/* [한국어] 처리기가 자기 문맥을 되찾는 통로 */
}
EXPORT_SYMBOL_GPL(iommu_set_fault_handler);

/*
 * [한국어]
 * iommu_domain_init - 갓 만들어진 도메인의 공통 필드를 채운다
 *
 * @domain: 드라이버가 막 할당한 도메인
 * @type:   무슨 종류인가(번역·통과·차단 등)
 * @ops:    이 도메인을 만든 IOMMU 의 vtable
 * @return: 없음
 *
 * 드라이버는 도메인을 할당하고 자기 필드만 채운다. 코어가 알아야 하는
 * 것들은 여기서 채워지며, 그래서 모든 도메인 생성 경로가 마지막에 이
 * 함수를 지난다.
 *
 * ops 를 이미 채웠으면 건드리지 않는 것에 주의할 것. 드라이버가 도메인
 * 종류마다 다른 연산 집합을 주고 싶을 때 미리 채워 두며, 그 경우 기본값을
 * 덮어쓰면 안 된다.
 *
 * 호출 체인: 각 도메인 생성 경로 → [이 함수]
 */
static void iommu_domain_init(struct iommu_domain *domain, unsigned int type,
			      const struct iommu_ops *ops)
{
	domain->type = type;
	domain->owner = ops;	/* [한국어] 어느 IOMMU 가 만든 도메인인지 — 다른 IOMMU 에 붙이려 할 때 걸러 내는 근거다 */
	if (!domain->ops)
		domain->ops = ops->default_domain_ops;	/* [한국어] 드라이버가 종류별 연산을 미리 채웠으면 그것을 존중한다 */
}

/*
 * [한국어]
 * __iommu_paging_domain_alloc_flags - 번역용 도메인을 만든다
 *
 * @dev:   이 장치의 IOMMU 에게 만들게 한다
 * @type:  만들어진 도메인에 새길 종류
 * @flags: iommufd 가 요구하는 속성(중첩 변환, PASID 지원 등)
 * @return: 도메인. 실패하면 오류 포인터.
 *
 * 드라이버가 도메인을 만드는 콜백이 세대별로 셋이라 갈래가 셋이다.
 * 새 콜백(domain_alloc_paging_flags)은 플래그를 받고, 그 앞 세대
 * (domain_alloc_paging)는 못 받으며, 가장 오래된 것(domain_alloc)은
 * FSL PAMU 에만 남아 #ifdef 로 묶여 있다.
 *
 * 플래그가 있으면 새 콜백만 쓸 수 있는 것에 주의할 것. 옛 콜백은
 * 플래그를 표현할 방법이 없으므로, 요구를 조용히 무시하는 대신
 * -EOPNOTSUPP 로 거절한다.
 *
 * 반환값을 두 번 검사하는 이유: 콜백 세대에 따라 실패를 오류 포인터로
 * 알리기도 하고 NULL 로 알리기도 한다. 둘 다 받아 준다.
 *
 * 실행 컨텍스트: 도메인 생성 경로. 잠들 수 있다.
 *
 * 호출 체인: iommu_paging_domain_alloc_flags / 기본 도메인 설정 → [이 함수]
 */
static struct iommu_domain *
__iommu_paging_domain_alloc_flags(struct device *dev, unsigned int type,
				  unsigned int flags)
{
	const struct iommu_ops *ops;
	struct iommu_domain *domain;

	if (!dev_has_iommu(dev))
		return ERR_PTR(-ENODEV);	/* [한국어] IOMMU 아래에 없는 장치에는 만들어 줄 도메인이 없다 */

	ops = dev_iommu_ops(dev);

	if (ops->domain_alloc_paging && !flags)
		domain = ops->domain_alloc_paging(dev);	/* [한국어] 플래그를 표현할 수 없는 세대 — 요구가 없을 때만 쓴다 */
	else if (ops->domain_alloc_paging_flags)
		domain = ops->domain_alloc_paging_flags(dev, flags, NULL);	/* [한국어] 플래그를 받는 새 콜백 */
#if IS_ENABLED(CONFIG_FSL_PAMU)
	else if (ops->domain_alloc && !flags)
		domain = ops->domain_alloc(IOMMU_DOMAIN_UNMANAGED);	/* [한국어] 가장 오래된 콜백. 이제 FSL PAMU 에만 남았다 */
#endif
	else
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 플래그를 요구했는데 표현할 콜백이 없다 — 조용히 무시하지 않고 거절한다 */

	if (IS_ERR(domain))
		return domain;	/* [한국어] 콜백 세대에 따라 실패를 알리는 방식이 다르다 */
	if (!domain)
		return ERR_PTR(-ENOMEM);	/* [한국어] 옛 콜백은 NULL 로 실패를 알린다 */

	iommu_domain_init(domain, type, ops);	/* [한국어] 코어가 알아야 하는 공통 필드를 채운다 */
	return domain;
}

/**
 * iommu_paging_domain_alloc_flags() - Allocate a paging domain
 * @dev: device for which the domain is allocated
 * @flags: Bitmap of iommufd_hwpt_alloc_flags
 *
 * Allocate a paging domain which will be managed by a kernel driver. Return
 * allocated domain if successful, or an ERR pointer for failure.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 외부에 열린 도메인 생성 API.
 *
 * 종류를 UNMANAGED 로 못박는 것이 요점이다. 매핑을 커널의 DMA API 가
 * 아니라 호출자가 직접 관리한다는 뜻이며, VFIO 와 iommufd 가 쓰는 도메인이
 * 모두 이 종류다.
 *
 * 호출 체인: VFIO/iommufd → [이 함수] → __iommu_paging_domain_alloc_flags
 */
struct iommu_domain *iommu_paging_domain_alloc_flags(struct device *dev,
						     unsigned int flags)
{
	return __iommu_paging_domain_alloc_flags(dev,
					 IOMMU_DOMAIN_UNMANAGED, flags);	/* [한국어] 매핑을 호출자가 직접 관리한다는 표시 */
}
EXPORT_SYMBOL_GPL(iommu_paging_domain_alloc_flags);

/*
 * [한국어]
 * iommu_domain_free - 도메인을 해제한다
 *
 * @domain: 해제할 도메인
 * @return: 없음
 *
 * 쿠키를 먼저 풀고 드라이버에 넘기는 순서다. 쿠키는 도메인에 딸린
 * 사용자별 상태이고 종류마다 푸는 방법이 다르므로, 어느 용도로 쓰였는지
 * 기록해 둔 cookie_type 을 보고 갈라 처리한다.
 *
 * SVA 만 mmdrop 인 것에 주의할 것. 공유 주소 공간 도메인은 프로세스의
 * mm 을 참조로 들고 있어, 그것을 놓는 것이 곧 쿠키 정리다.
 *
 * 드라이버의 free 를 마지막에 부르는 이유: 그 호출 뒤에는 도메인 구조체
 * 자체가 사라지므로, cookie_type 을 포함한 어떤 필드도 읽을 수 없다.
 *
 * 실행 컨텍스트: 도메인 해체 경로. 잠들 수 있다.
 *
 * 호출 체인: 도메인 사용자 → [이 함수] → ops->free
 */
void iommu_domain_free(struct iommu_domain *domain)
{
	switch (domain->cookie_type) {	/* [한국어] 쿠키 자리를 누가 썼는지에 따라 푸는 방법이 다르다 */
	case IOMMU_COOKIE_DMA_IOVA:
		iommu_put_dma_cookie(domain);	/* [한국어] DMA API 가 쓰던 IOVA 할당기 */
		break;
	case IOMMU_COOKIE_DMA_MSI:
		iommu_put_msi_cookie(domain);	/* [한국어] MSI 창 매핑 정보 */
		break;
	case IOMMU_COOKIE_SVA:
		mmdrop(domain->mm);	/* [한국어] 공유 주소 공간 — 프로세스 mm 참조를 놓는 것이 곧 정리다 */
		break;
	default:
		break;	/* [한국어] 쿠키를 쓰지 않았거나 폴트 처리기라 풀 것이 없다 */
	}
	if (domain->ops->free)
		domain->ops->free(domain);	/* [한국어] 마지막 — 이 뒤로는 구조체 자체가 없어 어떤 필드도 못 읽는다 */
}
EXPORT_SYMBOL_GPL(iommu_domain_free);

/*
 * Put the group's domain back to the appropriate core-owned domain - either the
 * standard kernel-mode DMA configuration or an all-DMA-blocked domain.
 */
/*
 * [한국어] (위 영어 주석에 이어) 그룹을 코어가 소유한 도메인으로 되돌린다.
 *
 * 갈래가 소유권으로 갈리는 것이 요점이다. 아직 소유자가 있다면 -- VFIO 가
 * 장치를 들고 있는 중이라면 -- 기본 도메인으로 되돌리면 안 된다. 그것은
 * 커널의 DMA 번역이 살아난다는 뜻이고, 사용자 공간에 넘긴 장치가 커널
 * 메모리를 볼 수 있게 되기 때문이다. 그래서 차단 도메인으로 보낸다.
 *
 * 소유자가 없을 때만 기본 도메인으로 돌아간다. 그것이 정상 상태다.
 *
 * nofail 판을 쓰는 이유: 이 경로는 이미 무언가를 놓는 중이라 물러설
 * 곳이 없다. 실패해도 옛 도메인에 남겨 둘 수 없다.
 *
 * 실행 컨텍스트: 장치 분리·해체 경로. 잠들 수 있다.
 *
 * 호출 체인: iommu_detach_device / 소유권 해제 → [이 함수]
 */
static void __iommu_group_set_core_domain(struct iommu_group *group)
{
	struct iommu_domain *new_domain;

	if (group->owner)
		new_domain = group->blocking_domain;	/* [한국어] 아직 사용자 공간 것이다 — 커널 번역을 살리면 커널 메모리가 노출된다 */
	else
		new_domain = group->default_domain;	/* [한국어] 아무도 안 쓴다 — 정상 상태로 되돌린다 */

	__iommu_group_set_domain_nofail(group, new_domain);	/* [한국어] 물러설 곳이 없는 경로라 실패를 허용하지 않는다 */
}

/*
 * [한국어]
 * __iommu_attach_device - 장치 하나를 도메인에 붙인다
 *
 * @domain: 붙일 도메인
 * @dev:    붙일 장치
 * @old:    직전 도메인. 드라이버가 원자적 교체를 할 수 있게 함께 준다.
 * @return: 0 이면 성공, 음수 errno
 *
 * 하드웨어를 실제로 만지는 지점이다. 드라이버가 페이지 테이블 베이스를
 * 장치의 문맥 항목에 써 넣고, 그 순간부터 이 장치의 DMA 가 새 주소
 * 공간을 본다.
 *
 * old 를 함께 넘기는 이유: 드라이버가 지원한다면 두 도메인 사이를 한
 * 번의 하드웨어 갱신으로 건너뛸 수 있다. 떼었다 붙이면 그 사이에 DMA 가
 * 갈 곳을 잃는 창이 생긴다.
 *
 * attach_deferred 를 지우는 것에 주의할 것. 부팅 초기에는 도메인을 바로
 * 붙이지 못해 미뤄 두는 경우가 있고, 실제로 붙은 이 시점에 그 표시를
 * 지워야 DMA 경로가 매번 다시 붙이려 하지 않는다.
 *
 * 실행 컨텍스트: 그룹 락을 든 채. 잠들 수 있다.
 *
 * 호출 체인: __iommu_group_set_domain_internal → [이 함수] → ops->attach_dev
 */
static int __iommu_attach_device(struct iommu_domain *domain,
				 struct device *dev, struct iommu_domain *old)
{
	int ret;

	if (unlikely(domain->ops->attach_dev == NULL))
		return -ENODEV;	/* [한국어] 붙이기를 지원하지 않는 도메인 — 있을 수 있지만 흔치 않다 */

	ret = domain->ops->attach_dev(domain, dev, old);	/* [한국어] 여기서 하드웨어가 바뀐다. old 를 주어 원자적 교체를 가능하게 한다 */
	if (ret)
		return ret;
	dev->iommu->attach_deferred = 0;	/* [한국어] 미뤄 둔 붙이기가 실제로 끝났다 — DMA 경로가 다시 시도하지 않게 한다 */
	trace_attach_device_to_domain(dev);
	return 0;
}

/**
 * iommu_attach_device - Attach an IOMMU domain to a device
 * @domain: IOMMU domain to attach
 * @dev: Device that will be attached
 *
 * Returns 0 on success and error code on failure
 *
 * Note that EINVAL can be treated as a soft failure, indicating
 * that certain configuration of the domain is incompatible with
 * the device. In this case attaching a different domain to the
 * device may succeed.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 장치 하나를 도메인에 붙이는 공개 API.
 *
 * 그룹에 장치가 하나뿐일 때만 허용하는 것이 이 함수의 전부다. 왜냐하면
 * 도메인은 그룹 단위로 붙기 때문이다 -- 장치 하나만 옮기는 것은 불가능하고,
 * 옮기면 같은 그룹의 나머지 장치도 함께 옮겨진다. 호출자는 장치 하나를
 * 다룬다고 생각하는데 실제로는 여러 장치가 움직이는 상황을 막으려고,
 * 애초에 둘 이상이면 -EINVAL 로 거절한다.
 *
 * 여러 장치를 옮기려는 호출자는 iommu_attach_group 을 써야 하며, 그것은
 * 그룹을 옮긴다는 사실이 이름에 드러난다.
 *
 * 위 영어 주석이 밝히는 -EINVAL 의 성격도 중요하다. 도메인과 장치의
 * 구성이 안 맞는다는 뜻일 수 있어, 다른 도메인으로는 성공할 수 있다.
 *
 * 실행 컨텍스트: 드라이버 문맥. 잠들 수 있다.
 *
 * 호출 체인: 장치 드라이버 → [이 함수] → __iommu_attach_group
 */
int iommu_attach_device(struct iommu_domain *domain, struct device *dev)
{
	/* Caller must be a probed driver on dev */
	struct iommu_group *group = dev->iommu_group;
	int ret;

	if (!group)
		return -ENODEV;	/* [한국어] IOMMU 아래에 없는 장치 */

	/*
	 * Lock the group to make sure the device-count doesn't
	 * change while we are attaching
	 */
	mutex_lock(&group->mutex);
	ret = -EINVAL;
	if (list_count_nodes(&group->devices) != 1)
		goto out_unlock;	/* [한국어] 도메인은 그룹 단위로 붙는다 — 둘 이상이면 남의 장치까지 함께 옮겨진다 */

	ret = __iommu_attach_group(domain, group);	/* [한국어] 장치가 하나뿐이라 그룹을 옮기는 것과 같다 */

out_unlock:
	mutex_unlock(&group->mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_attach_device);

/*
 * [한국어]
 * iommu_deferred_attach - 미뤄 두었던 도메인 붙이기를 DMA 경로에서 처리한다
 *
 * @dev:    대상 장치
 * @domain: 붙일 도메인
 * @return: 0 이면 (이미 붙었거나 방금 붙어) 진행해도 좋다. -EBUSY 면 리셋 중이다.
 *
 * 왜 미루는가: 부팅 초기에는 도메인을 붙일 수 없는 시점이 있다. 그때는
 * 표시만 해 두었다가 첫 DMA 매핑 때 실제로 붙인다.
 *
 * 락 없이 플래그부터 보는 것이 위 영어 주석의 요점이다. 이 함수는 DMA
 * 매핑 핫패스에서 매번 불리므로 락을 잡으면 그 비용이 모든 매핑에 얹힌다.
 * 경쟁이 있을 수 있지만, 드라이버가 probe 안에서 단일 스레드로 DMA 를
 * 세운다는 전제로 감수한다.
 *
 * 리셋 중 거절은 두 번째 영어 주석이 밝힌다. 리셋 도중에는 그룹이 물리
 * 도메인에 가 있고, 여기서 다른 도메인을 붙이면 리셋이 끝난 뒤 되돌릴
 * 대상이 어긋난다. 그 대가로 이 DMA 매핑이 실패하지만 다른 방법이 없다.
 *
 * 실행 컨텍스트: DMA 매핑 경로. 잠들 수 있다(뮤텍스를 잡는다).
 *
 * 호출 체인: dma-iommu.c 의 매핑 경로 → [이 함수] → __iommu_attach_device
 */
int iommu_deferred_attach(struct device *dev, struct iommu_domain *domain)
{
	/*
	 * This is called on the dma mapping fast path so avoid locking. This is
	 * racy, but we have an expectation that the driver will setup its DMAs
	 * inside probe while being single threaded to avoid racing.
	 */
	/* [한국어] 위 영어 주석대로 핫패스라 락 없이 먼저 거른다. 미룬 붙이기가
	 * 없는 것이 압도적으로 흔하므로, 그 경우 락 비용을 아예 치르지 않는다. */
	if (!dev->iommu || !dev->iommu->attach_deferred)
		return 0;

	guard(mutex)(&dev->iommu_group->mutex);	/* [한국어] 실제로 붙일 때만 락을 잡는다. guard 라 반환 시 자동으로 놓인다 */

	/*
	 * This is a concurrent attach during a device reset. Reject it until
	 * pci_dev_reset_iommu_done() attaches the device to group->domain.
	 *
	 * Note that this might fail the iommu_dma_map(). But there's nothing
	 * more we can do here.
	 */
	/* [한국어] 위 영어 주석대로 리셋 중에 붙이면 되돌릴 대상이 어긋난다.
	 * 이 DMA 매핑이 실패하는 대가를 치르더라도 거절하는 편이 낫다. */
	if (dev->iommu_group->resetting_domain)
		return -EBUSY;
	return __iommu_attach_device(domain, dev, NULL);	/* [한국어] old 가 NULL — 지금 붙어 있는 도메인이 없다 */
}

/*
 * [한국어]
 * iommu_detach_device - 장치를 도메인에서 떼어 코어 도메인으로 되돌린다
 *
 * @domain: 지금 붙어 있는 도메인. 확인용이다.
 * @dev:    대상 장치
 * @return: 없음
 *
 * attach 의 짝이며 같은 제약을 받는다 -- 그룹에 장치가 하나뿐일 때만
 * 의미가 있다.
 *
 * domain 인자를 받아 확인만 하는 것에 주의할 것. 호출자가 생각하는
 * 도메인과 실제로 붙어 있는 것이 다르면 상태가 이미 어긋난 것이므로,
 * 조용히 떼는 대신 WARN 을 내고 아무것도 하지 않는다.
 *
 * "뗀다"가 아무 도메인에도 안 붙은 상태를 뜻하지 않는 것도 중요하다.
 * 장치는 항상 어딘가에 붙어 있어야 하므로, 실제로는 기본 도메인이나
 * 차단 도메인으로 옮겨진다.
 *
 * 실행 컨텍스트: 드라이버 문맥. 잠들 수 있다.
 *
 * 호출 체인: 장치 드라이버 → [이 함수] → __iommu_group_set_core_domain
 */
void iommu_detach_device(struct iommu_domain *domain, struct device *dev)
{
	/* Caller must be a probed driver on dev */
	struct iommu_group *group = dev->iommu_group;

	if (!group)
		return;

	mutex_lock(&group->mutex);
	if (WARN_ON(domain != group->domain) ||	/* [한국어] 호출자가 아는 도메인과 실제가 다르다 — 상태가 이미 어긋났다 */
	    WARN_ON(list_count_nodes(&group->devices) != 1))	/* [한국어] attach 와 같은 제약 — 그룹 단위로만 옮길 수 있다 */
		goto out_unlock;
	__iommu_group_set_core_domain(group);	/* [한국어] 떼는 것이 아니라 기본/차단 도메인으로 옮기는 것이다 */

out_unlock:
	mutex_unlock(&group->mutex);
}
EXPORT_SYMBOL_GPL(iommu_detach_device);

/**
 * iommu_get_domain_for_dev() - Return the DMA API domain pointer
 * @dev: Device to query
 *
 * This function can be called within a driver bound to dev. The returned
 * pointer is valid for the lifetime of the bound driver.
 *
 * It should not be called by drivers with driver_managed_dma = true.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_get_domain_for_dev - 이 장치에 현재 걸린 도메인을 돌려준다 (드라이버용)
 *
 * @dev:    조회할 장치
 * @return: 현재 도메인, IOMMU 아래가 아니면 NULL
 *
 * 장치 드라이버가 자기 DMA 가 어떤 주소 공간을 지나는지 알아야 할 때 쓴다.
 * 반환값이 바인딩된 드라이버의 수명 동안 유효한 이유는, 그 사이에는 도메인 교체가
 * 일어나지 않기 때문이다 — 도메인을 바꾸려면 먼저 DMA 소유권을 가져가야 하고,
 * 소유권 획득은 드라이버가 붙어 있는 장치에 대해서는 거부된다.
 *
 * driver_managed_dma 드라이버가 부르면 안 된다는 제약(위 영어 주석)은 그 드라이버가
 * 소유권 규칙 밖에 있어 위 보장이 성립하지 않기 때문이다.
 *
 * lockdep_assert_not_held 가 있는 것에 주목할 것. 그룹 락을 든 문맥 — 즉 IOMMU
 * 드라이버 콜백 안 — 에서 필요한 것은 iommu_driver_get_domain_for_dev 쪽이다.
 *
 * 실행 컨텍스트: 장치 드라이버. 그룹 락을 들지 않은 상태.
 *
 * 호출 체인: 장치 드라이버 → [이 함수]
 */
struct iommu_domain *iommu_get_domain_for_dev(struct device *dev)
{
	/* Caller must be a probed driver on dev */
	struct iommu_group *group = dev->iommu_group;	/* [한국어] 드라이버가 자기 장치에 대해 부르므로 그룹은 이미 안정적이다 */

	if (!group)	/* [한국어] IOMMU 아래에 있지 않은 장치 */
		return NULL;	/* [한국어] 번역이 없다는 뜻 */

	lockdep_assert_not_held(&group->mutex);	/* [한국어] 그룹 락을 든 채로 부르면 안 된다 — 이 함수는 드라이버용 겉면이고, 락을 든 문맥에서 필요한 것은 아래의 iommu_driver_get_domain_for_dev 다 */

	return group->domain;	/* [한국어] 현재 걸린 도메인. 드라이버가 바인딩된 동안에는 바뀌지 않는다 (위 영어 주석) */
}
EXPORT_SYMBOL_GPL(iommu_get_domain_for_dev);

/**
 * iommu_driver_get_domain_for_dev() - Return the driver-level domain pointer
 * @dev: Device to query
 *
 * This function can be called by an iommu driver that wants to get the physical
 * domain within an iommu callback function where group->mutex is held.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_driver_get_domain_for_dev - 콜백 안에서 '옛 도메인'을 정확히 얻는다 (IOMMU 드라이버용)
 *
 * @dev:    조회할 장치
 * @return: 이 장치가 지금 실제로 붙어 있는 도메인
 *
 * 겉보기에는 group->domain 을 읽는 것과 같지만, PCI 함수 리셋 중에는 답이 달라진다.
 * 리셋 경로는 도메인을 잠시 떼었다가 리셋이 끝난 뒤 다시 붙이는데, 그 사이
 * group->domain 은 이미 '리셋 후 붙일 도메인'으로 갱신되어 있을 수 있다. 드라이버가
 * 그것을 옛 도메인으로 오해하면 같은 도메인에서 같은 도메인으로 옮기는 무의미한
 * 전이가 생긴다.
 *
 * group->resetting_domain 이 그 기간의 진짜 현재 상태를 들고 있고, 이 함수가 둘을
 * 가려 준다. 그룹 락을 든 문맥 — IOMMU 드라이버의 attach 콜백 안 — 전용이다.
 *
 * 실행 컨텍스트: IOMMU 드라이버 콜백. 그룹 락을 든 상태.
 *
 * 호출 체인: 벤더 드라이버의 attach_dev 계열 → [이 함수]
 */
struct iommu_domain *iommu_driver_get_domain_for_dev(struct device *dev)
{
	struct iommu_group *group = dev->iommu_group;	/* [한국어] IOMMU 드라이버 콜백 안에서 불리므로 그룹은 반드시 있다 */

	lockdep_assert_held(&group->mutex);	/* [한국어] 이 변형은 그룹 락을 든 문맥 전용이다 */

	/*
	 * Driver handles the low-level __iommu_attach_device(), including the
	 * one invoked by pci_dev_reset_iommu_done() re-attaching the device to
	 * the cached group->domain. In this case, the driver must get the old
	 * domain from group->resetting_domain rather than group->domain. This
	 * prevents it from re-attaching the device from group->domain (old) to
	 * group->domain (new).
	 */
	if (group->resetting_domain)	/* [한국어] PCI 함수 리셋 중이라면 group->domain 은 이미 '리셋 후에 붙일 새 도메인'으로 갱신되어 있을 수 있다 */
		return group->resetting_domain;	/* [한국어] 드라이버가 봐야 하는 '옛 도메인'은 이쪽이다. 이것을 구분하지 않으면 새 도메인에서 새 도메인으로 붙이는 무의미한 전이가 생긴다 (위 영어 주석) */

	return group->domain;	/* [한국어] 리셋 중이 아니면 현재 도메인이 곧 옛 도메인이다 */
}
EXPORT_SYMBOL_GPL(iommu_driver_get_domain_for_dev);

/*
 * For IOMMU_DOMAIN_DMA implementations which already provide their own
 * guarantees that the group and its default domain are valid and correct.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * iommu_get_dma_domain - 검사 없이 기본 도메인을 꺼낸다 (dma-iommu 전용 핫패스)
 *
 * @dev:    DMA 를 하려는 장치
 * @return: 그룹의 기본 도메인
 *
 * dma_map_page 마다 불리는 자리라 NULL 검사조차 두지 않았다. dma-iommu 는 자신이
 * iommu_setup_dma_ops 로 DMA API 를 갈아 끼운 장치에 대해서만 이 함수를 부르고,
 * 그 시점에 그룹과 기본 도메인이 모두 유효함을 알고 있다.
 *
 * iommu_get_domain_for_dev 와의 차이가 계약의 차이다 — 저쪽은 '현재' 도메인
 * (소유권이 넘어가면 바뀔 수 있는 것)을, 이쪽은 커널 DMA API 전용인 '기본' 도메인을
 * 돌려준다.
 *
 * 실행 컨텍스트: DMA 매핑 핫패스. 아토믹 문맥 포함 어디서든.
 *
 * 호출 체인: dma-iommu 의 dma_map_* 구현 → [이 함수]
 */
struct iommu_domain *iommu_get_dma_domain(struct device *dev)
{
	return dev->iommu_group->default_domain;	/* [한국어] 검사를 전혀 하지 않는다. dma-iommu 는 자신이 붙인 장치에 대해서만 부르고 그룹·기본 도메인이 유효함을 스스로 보장하므로, 핫패스에서 조건 분기를 없앤 것이다 (위 영어 주석) */
}

/*
 * [한국어]
 * iommu_make_pasid_array_entry - PASID 배열에 넣을 태그 붙은 포인터를 만든다
 *
 * @domain: 그 PASID 에 붙일 도메인
 * @handle: 부착 핸들. NULL 이면 도메인을 직접 저장한다.
 * @return: xarray 에 넣을 태그 포인터
 *
 * pasid_array_entry_to_domain 의 짝이다. 같은 xarray 에 두 종류를 담기 위해 포인터
 * 하위 비트에 태그를 심는다 — 커널 포인터는 최소 4바이트 정렬이라 남는 비트가 있다.
 *
 * 왜 두 종류인가. 커널 내부 SVA 는 도메인만 알면 되지만, iommufd 는 사용자 공간이
 * 부착을 식별할 수 있어야 해서 핸들을 함께 둔다. 핸들이 있으면 도메인은 핸들 안에
 * 들어가므로 배열에는 핸들만 저장하면 된다.
 *
 * 실행 컨텍스트: PASID 부착 경로. 그룹 락 아래.
 *
 * 호출 체인: iommu_attach_device_pasid, iommu_replace_device_pasid → [이 함수]
 */
static void *iommu_make_pasid_array_entry(struct iommu_domain *domain,
					  struct iommu_attach_handle *handle)
{
	if (handle) {	/* [한국어] iommufd 처럼 부착 핸들을 함께 관리하는 사용자 */
		handle->domain = domain;	/* [한국어] 핸들 안에 도메인을 기록해 둔다 — 나중에 pasid_array_entry_to_domain 이 여기서 꺼낸다 */
		return xa_tag_pointer(handle, IOMMU_PASID_ARRAY_HANDLE);	/* [한국어] 핸들 포인터에 '핸들' 태그를 달아 저장한다 */
	}

	return xa_tag_pointer(domain, IOMMU_PASID_ARRAY_DOMAIN);	/* [한국어] 핸들이 없으면 도메인을 직접 저장하되 '도메인' 태그를 단다. 포인터 정렬로 남는 하위 비트에 종류를 실어 배열 하나로 두 경우를 담는다 */
}

/*
 * [한국어]
 * domain_iommu_ops_compatible - 이 도메인을 이 드라이버가 다룰 수 있는가
 *
 * @ops:    부착을 시도하는 드라이버의 콜백 표
 * @domain: 붙이려는 도메인
 * @return: true 면 호환
 *
 * 한 시스템에 서로 다른 IOMMU 가 공존할 수 있다 — ARM SoC 에서 SMMUv3 와 별개
 * 벤더 IOMMU 가 같이 있거나, iommufd 셀프테스트의 가짜 드라이버가 끼는 경우다.
 * 도메인의 페이지 테이블 포맷은 만든 드라이버의 것이므로, 다른 드라이버가 그것을
 * 물면 하드웨어가 쓰레기를 워크하게 된다. 이 검사가 그 사고를 막는다.
 *
 * 정적 도메인이 예외인 이유는 소유자 필드가 비어 있기 때문이다. blocked/identity
 * 도메인은 드라이버가 컴파일 시점에 들고 있는 상수라 domain_alloc 을 거치지 않고,
 * 따라서 owner 를 채울 기회가 없다. 대신 ops 에서 직접 가리키고 있으므로 포인터
 * 비교만으로 자기 것임을 확인할 수 있다.
 *
 * 실행 컨텍스트: 부착 경로. 어디서든.
 *
 * 호출 체인: __iommu_attach_group, iommu_attach_device_pasid 등 → [이 함수]
 */
static bool domain_iommu_ops_compatible(const struct iommu_ops *ops,
					struct iommu_domain *domain)
{
	if (domain->owner == ops)	/* [한국어] 도메인을 만든 드라이버와 지금 부착을 시도하는 드라이버가 같다 */
		return true;	/* [한국어] 같은 페이지 테이블 포맷이므로 붙일 수 있다 */

	/* For static domains, owner isn't set. */
	if (domain == ops->blocked_domain || domain == ops->identity_domain)	/* [한국어] 정적 도메인은 드라이버가 컴파일 시점에 들고 있는 것이라 owner 필드가 비어 있다 (위 영어 주석) */
		return true;	/* [한국어] 그 드라이버 자신의 정적 도메인이므로 호환된다 */

	return false;	/* [한국어] 다른 드라이버의 도메인 — 이종 IOMMU 가 섞인 시스템에서 잘못된 페이지 테이블을 물리는 것을 막는다 */
}

/*
 * [한국어]
 * __iommu_attach_group - 그룹을 새 도메인에 붙인다 (락은 호출자가 든다)
 *
 * @domain: 붙일 도메인
 * @group:  대상 그룹
 * @return: 0 이면 성공. -EBUSY 면 이미 남이 쓰고 있고, -EINVAL 이면 맞지 않는다.
 *
 * 두 가지를 확인한 뒤 실제 전환에 넘긴다.
 *
 * -EBUSY 검사가 소유권의 실질이다. 그룹이 코어의 도메인(기본/차단)에
 * 있을 때만 남이 가져갈 수 있다. 이미 다른 도메인에 붙어 있다면 누군가
 * 이 그룹을 쓰고 있다는 뜻이고, 그 위에 덮어쓰면 그쪽이 모르는 사이에
 * 주소 공간이 바뀐다.
 *
 * -EINVAL 은 도메인과 IOMMU 가 맞지 않는다는 뜻이다. 도메인은 그것을
 * 만든 IOMMU 의 페이지 테이블 형식을 가지므로, 다른 벤더의 IOMMU 에
 * 붙일 수 없다. 위 iommu_attach_group 의 영어 주석대로 이 실패는 다른
 * 도메인으로는 성공할 수 있는 종류다.
 *
 * 실행 컨텍스트: group->mutex 를 든 채. 잠들 수 있다.
 *
 * 호출 체인: iommu_attach_group / iommu_attach_device → [이 함수]
 */
static int __iommu_attach_group(struct iommu_domain *domain,
				struct iommu_group *group)
{
	struct device *dev;

	if (group->domain && group->domain != group->default_domain &&
	    group->domain != group->blocking_domain)
		return -EBUSY;	/* [한국어] 코어 소유의 도메인에 있을 때만 가져갈 수 있다 — 아니면 이미 남이 쓰는 중이다 */

	dev = iommu_group_first_dev(group);
	if (!dev_has_iommu(dev) ||
	    !domain_iommu_ops_compatible(dev_iommu_ops(dev), domain))
		return -EINVAL;	/* [한국어] 도메인은 만든 IOMMU 의 페이지 테이블 형식을 가진다 — 다른 벤더에는 못 붙인다 */

	return __iommu_group_set_domain(group, domain);
}

/**
 * iommu_attach_group - Attach an IOMMU domain to an IOMMU group
 * @domain: IOMMU domain to attach
 * @group: IOMMU group that will be attached
 *
 * Returns 0 on success and error code on failure
 *
 * Note that EINVAL can be treated as a soft failure, indicating
 * that certain configuration of the domain is incompatible with
 * the group. In this case attaching a different domain to the
 * group may succeed.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어) 그룹을 도메인에 붙이는 공개 API.
 *
 * iommu_attach_device 와 달리 장치 수를 따지지 않는다. 호출자가 그룹을
 * 옮긴다는 것을 이미 알고 부르는 것이므로, 여러 장치가 함께 움직이는
 * 것이 놀랄 일이 아니다.
 *
 * 락만 잡고 안쪽으로 넘긴다 -- 안쪽 판은 락을 이미 든 경로에서도
 * 불리기 때문에 나뉘어 있다.
 *
 * 호출 체인: VFIO/iommufd → [이 함수] → __iommu_attach_group
 */
int iommu_attach_group(struct iommu_domain *domain, struct iommu_group *group)
{
	int ret;

	mutex_lock(&group->mutex);
	ret = __iommu_attach_group(domain, group);	/* [한국어] 락을 이미 든 경로도 있어 안쪽 판이 따로 있다 */
	mutex_unlock(&group->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(iommu_attach_group);

/*
 * [한국어]
 * __iommu_device_set_domain - 장치 하나를 새 도메인으로 옮긴다
 *
 * @group:      소속 그룹
 * @dev:        옮길 장치
 * @new_domain: 옮겨 갈 도메인
 * @old_domain: 지금 붙어 있는 도메인. 드라이버의 원자적 교체에 쓰인다.
 * @flags:      IOMMU_SET_DOMAIN_MUST_SUCCEED 이면 실패해도 물러설 수 없다
 * @return: 0 이면 성공, 음수 errno
 *
 * 세 가지 특수 처리가 들어 있다.
 *
 * 첫째는 항등 매핑이 필수인 장치다. 위 영어 주석대로 펌웨어가 그렇게
 * 요구한 장치를 차단 도메인에 넣으면 펌웨어가 쓰던 접근이 끊긴다.
 * 그래서 거절하며, 그 결과로 이런 장치는 VFIO 나 iommufd 로 넘길 수도
 * 없다 -- 소유권을 가져가는 과정이 차단 도메인을 거치기 때문이다.
 *
 * 둘째는 미뤄 둔 붙이기다. 아직 실제로 붙지 않은 장치를 기본 도메인으로
 * 옮기라는 요청은 할 일이 없다 -- 어차피 첫 DMA 때 그리로 붙을 것이다.
 * 반면 다른 도메인으로 가야 한다면 지금 붙여야 하므로 표시를 지운다.
 *
 * 셋째가 실패 처리이며 위 영어 주석이 그 의도를 밝힌다. 반드시 성공해야
 * 하는 경로에서 실패했다면, 차라리 차단 도메인에라도 붙여 장치가 사라진
 * 페이지 테이블을 계속 보는 것(use-after-free)을 막는다. 최신 드라이버는
 * 차단 도메인을 실패할 수 없는 정적 객체로 두라고 권한다.
 *
 * 실행 컨텍스트: group->mutex 를 든 채. 잠들 수 있다.
 *
 * 호출 체인: __iommu_group_set_domain_internal → [이 함수] → __iommu_attach_device
 */
static int __iommu_device_set_domain(struct iommu_group *group,
				     struct device *dev,
				     struct iommu_domain *new_domain,
				     struct iommu_domain *old_domain,
				     unsigned int flags)
{
	int ret;

	/*
	 * If the device requires IOMMU_RESV_DIRECT then we cannot allow
	 * the blocking domain to be attached as it does not contain the
	 * required 1:1 mapping. This test effectively excludes the device
	 * being used with iommu_group_claim_dma_owner() which will block
	 * vfio and iommufd as well.
	 */
	/* [한국어] 위 영어 주석대로 항등 매핑이 필수인 장치는 차단할 수 없다.
	 * 그 부작용으로 이런 장치는 사용자 공간에 넘길 수도 없는데, 소유권을
	 * 가져가는 과정 자체가 차단 도메인을 거치기 때문이다. */
	if (dev->iommu->require_direct &&
	    (new_domain->type == IOMMU_DOMAIN_BLOCKED ||
	     new_domain == group->blocking_domain)) {
		dev_warn(dev,	/* [한국어] 펌웨어 요구와 사용자 의도가 충돌한 것이라 벤더 문의를 안내한다 */
			 "Firmware has requested this device have a 1:1 IOMMU mapping, rejecting configuring the device without a 1:1 mapping. Contact your platform vendor.\n");
		return -EINVAL;
	}

	if (dev->iommu->attach_deferred) {	/* [한국어] 아직 실제로는 아무 도메인에도 붙지 않은 장치 */
		if (new_domain == group->default_domain)
			return 0;	/* [한국어] 어차피 첫 DMA 때 그리로 붙는다 — 지금 할 일이 없다 */
		dev->iommu->attach_deferred = 0;	/* [한국어] 다른 도메인으로 가야 하니 지금 붙인다 */
	}

	ret = __iommu_attach_device(new_domain, dev, old_domain);
	if (ret) {
		/*
		 * If we have a blocking domain then try to attach that in hopes
		 * of avoiding a UAF. Modern drivers should implement blocking
		 * domains as global statics that cannot fail.
		 */
		/* [한국어] 위 영어 주석대로 물러설 곳이 없는 경로에서 실패했다면,
		 * 차단 도메인에라도 붙여 장치가 곧 사라질 페이지 테이블을 계속
		 * 보는 것을 막는다. */
		if ((flags & IOMMU_SET_DOMAIN_MUST_SUCCEED) &&
		    group->blocking_domain &&
		    group->blocking_domain != new_domain)
			__iommu_attach_device(group->blocking_domain, dev,
					      old_domain);	/* [한국어] 이것마저 실패하면 할 수 있는 것이 없다 — 결과를 보지 않는다 */
		return ret;
	}
	return 0;
}

/*
 * If 0 is returned the group's domain is new_domain. If an error is returned
 * then the group's domain will be set back to the existing domain unless
 * IOMMU_SET_DOMAIN_MUST_SUCCEED, otherwise an error is returned and the group's
 * domains is left inconsistent. This is a driver bug to fail attach with a
 * previously good domain. We try to avoid a kernel UAF because of this.
 *
 * IOMMU groups are really the natural working unit of the IOMMU, but the IOMMU
 * API works on domains and devices.  Bridge that gap by iterating over the
 * devices in a group.  Ideally we'd have a single device which represents the
 * requestor ID of the group, but we also allow IOMMU drivers to create policy
 * defined minimum sets, where the physical hardware may be able to distiguish
 * members, but we wish to group them at a higher level (ex. untrusted
 * multi-function PCI devices).  Thus we attach each device.
 */
/*
 * [한국어] (위 영어 주석에 이어) 그룹의 도메인을 실제로 갈아 끼운다.
 * 이 파일에서 도메인 전환의 유일한 구현이며, 모든 전환이 여기를 지난다.
 *
 * 위 영어 주석의 두 번째 문단이 왜 장치마다 도는지 밝힌다. IOMMU 의
 * 자연스러운 단위는 그룹이지만 API 는 장치와 도메인으로 되어 있고,
 * 게다가 하드웨어가 구별할 수 있는데도 정책적으로 묶은 그룹이 있어
 * (신뢰할 수 없는 다기능 PCI 장치 같은) 그룹 대표 하나로는 부족하다.
 * 그래서 소속 장치를 하나씩 붙인다.
 *
 * 전환이 원자적이지 않아도 되는 이유도 그 주석에 있다. 전환 중 DMA 는
 * 버려져도 좋지만, 옛 도메인 아니면 새 도메인 둘 중 하나만 보여야 한다 --
 * 그 사이의 정의되지 않은 상태는 안 된다.
 *
 * 실패 처리가 두 갈래인 것이 이 함수의 핵심이다.
 *
 * 평범한 경로(플래그 0)는 err_revert 로 가서 이미 옮긴 장치들을 되돌린다.
 * 실패한 장치 자신은 되돌릴 것이 없으므로 건너뛰고, 그 앞의 것들만
 * 옛 도메인으로 되돌린다.
 *
 * MUST_SUCCEED 경로는 되돌리지 않고 나머지 장치도 계속 시도한다. 위
 * 영어 주석대로 이 경로는 이미 무언가를 해체하는 중이라 옛 도메인이 곧
 * 사라지기 때문이다. 되돌리면 사라질 페이지 테이블로 되돌리는 셈이 된다.
 * 그래서 최대한 밀고 나가며, 드라이버에게 최소한 옛 도메인 참조라도
 * 놓아 달라고 요구한다.
 *
 * group->domain 을 마지막에 쓰는 것에 주의할 것. 도중에 실패하면 그룹은
 * 여전히 옛 도메인을 가리키고, 그것이 err_revert 가 되돌릴 목표가 된다.
 *
 * @group:      대상 그룹
 * @new_domain: 옮겨 갈 도메인
 * @flags:      MUST_SUCCEED 이면 되돌리지 않는다
 * @return: 0 이면 성공. 음수면 실패이며, 되돌림 여부는 flags 에 달렸다.
 *
 * 실행 컨텍스트: group->mutex 를 든 채. 잠들 수 있다.
 *
 * 호출 체인: __iommu_group_set_domain(_nofail) → [이 함수] → __iommu_device_set_domain
 */
static int __iommu_group_set_domain_internal(struct iommu_group *group,
					     struct iommu_domain *new_domain,
					     unsigned int flags)
{
	struct group_device *last_gdev;	/* [한국어] 실패한 장치. 되돌림에서 여기까지만 간다 */
	struct group_device *gdev;
	int result;	/* [한국어] MUST_SUCCEED 경로에서 마지막 오류를 담아 올린다 */
	int ret;

	lockdep_assert_held(&group->mutex);

	if (group->domain == new_domain)
		return 0;	/* [한국어] 이미 거기 있다 — 하드웨어를 건드릴 이유가 없다 */

	if (WARN_ON(!new_domain))
		return -EINVAL;	/* [한국어] 아무 도메인에도 안 붙은 상태는 허용되지 않는다 */

	/*
	 * This is a concurrent attach during a device reset. Reject it until
	 * pci_dev_reset_iommu_done() attaches the device to group->domain.
	 */
	/* [한국어] 위 영어 주석대로 리셋 중에는 그룹이 물리 도메인에 가 있고,
	 * 여기서 바꾸면 리셋이 끝난 뒤 되돌릴 대상이 어긋난다. */
	if (group->resetting_domain)
		return -EBUSY;

	/*
	 * Changing the domain is done by calling attach_dev() on the new
	 * domain. This switch does not have to be atomic and DMA can be
	 * discarded during the transition. DMA must only be able to access
	 * either new_domain or group->domain, never something else.
	 */
	/* [한국어] 위 영어 주석이 이 루프의 안전 규약이다 -- 전환 중 DMA 는
	 * 버려져도 좋지만, 옛 도메인과 새 도메인 둘 중 하나만 보여야 한다.
	 * 정의되지 않은 중간 상태가 없어야 한다는 뜻이다. */
	result = 0;
	for_each_group_device(group, gdev) {
		ret = __iommu_device_set_domain(group, gdev->dev, new_domain,
						group->domain, flags);	/* [한국어] 옛 도메인을 함께 넘겨 원자적 교체를 가능하게 한다 */
		if (ret) {
			result = ret;
			/*
			 * Keep trying the other devices in the group. If a
			 * driver fails attach to an otherwise good domain, and
			 * does not support blocking domains, it should at least
			 * drop its reference on the current domain so we don't
			 * UAF.
			 */
			/* [한국어] 위 영어 주석대로, 물러설 수 없는 경로에서는
			 * 되돌리지 않고 나머지도 계속 시도한다. 옛 도메인이 곧
			 * 사라질 것이라 되돌려 봐야 사라질 곳으로 되돌리는 셈이다. */
			if (flags & IOMMU_SET_DOMAIN_MUST_SUCCEED)
				continue;
			goto err_revert;
		}
	}
	group->domain = new_domain;	/* [한국어] 마지막에 쓴다 — 도중에 실패하면 이 값이 되돌림의 목표가 된다 */
	return result;	/* [한국어] MUST_SUCCEED 에서 일부가 실패했으면 그 오류가 담겨 있다 */

err_revert:
	/*
	 * This is called in error unwind paths. A well behaved driver should
	 * always allow us to attach to a domain that was already attached.
	 */
	/* [한국어] 위 영어 주석이 되돌림이 성립하는 근거다 -- 방금까지 붙어
	 * 있던 도메인에 다시 붙는 것은 드라이버가 반드시 허용해야 한다. */
	last_gdev = gdev;	/* [한국어] 실패한 그 장치 */
	for_each_group_device(group, gdev) {
		/* No need to revert the last gdev that failed to set domain */
		if (gdev == last_gdev)
			break;	/* [한국어] 실패한 장치는 옮겨지지 않았으므로 되돌릴 것이 없다 */
		/*
		 * A NULL domain can happen only for first probe, in which case
		 * we leave group->domain as NULL and let release clean
		 * everything up.
		 */
		/* [한국어] 위 영어 주석대로 첫 probe 에서는 되돌릴 옛 도메인이
		 * 아예 없다. 그대로 두면 해체 경로가 정리한다. */
		if (group->domain)
			WARN_ON(__iommu_device_set_domain(	/* [한국어] 되돌림도 실패하면 그룹 상태가 어긋난 채 남는다 — 기록만 남긴다 */
				group, gdev->dev, group->domain, new_domain,
				IOMMU_SET_DOMAIN_MUST_SUCCEED));
	}
	return ret;
}

/*
 * [한국어]
 * iommu_detach_group - 그룹을 코어 도메인으로 되돌린다
 *
 * @domain: 지금 붙어 있는 도메인(쓰지 않는다 — API 대칭을 위한 인자다)
 * @group:  대상 그룹
 * @return: 없음
 *
 * iommu_attach_group 의 짝이다. domain 인자를 실제로 쓰지 않는 것에
 * 주의할 것 -- 어디로 되돌릴지는 소유권 상태가 정하지 호출자가 정하지
 * 않기 때문이다. 인자가 남아 있는 것은 attach 와 모양을 맞추기 위해서다.
 *
 * 실행 컨텍스트: 드라이버·VFIO 문맥. 잠들 수 있다.
 *
 * 호출 체인: VFIO/iommufd → [이 함수] → __iommu_group_set_core_domain
 */
void iommu_detach_group(struct iommu_domain *domain, struct iommu_group *group)
{
	mutex_lock(&group->mutex);
	__iommu_group_set_core_domain(group);	/* [한국어] 어디로 갈지는 소유권이 정한다 — 인자의 domain 은 쓰지 않는다 */
	mutex_unlock(&group->mutex);
}
EXPORT_SYMBOL_GPL(iommu_detach_group);

/*
 * [한국어]
 * iommu_iova_to_phys - IOVA 를 물리 주소로 되돌린다
 *
 * @domain: 조회할 도메인
 * @iova:   장치가 보는 주소
 * @return: 물리 주소. 매핑이 없으면 0.
 *
 * 두 특수 도메인은 페이지 테이블을 볼 것도 없이 답이 정해져 있다.
 * 통과 도메인은 주소가 곧 물리 주소이고, 차단 도메인은 어떤 주소도
 * 유효하지 않다.
 *
 * 반환 0 의 모호함에 주의할 것 -- "매핑 없음"과 "물리 주소 0 에 매핑됨"을
 * 구별하지 못한다. iommu_create_device_direct_mappings 가 주소 0 을 1 로
 * 바꿔 묻는 것이 이 때문이다.
 *
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인: DMA 계층·디버깅 → [이 함수] → ops->iova_to_phys
 */
phys_addr_t iommu_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	if (domain->type == IOMMU_DOMAIN_IDENTITY)
		return iova;	/* [한국어] 번역이 없는 도메인 — 주소가 곧 답이다 */

	if (domain->type == IOMMU_DOMAIN_BLOCKED)
		return 0;	/* [한국어] 어떤 주소도 유효하지 않다 */

	return domain->ops->iova_to_phys(domain, iova);	/* [한국어] 실제 페이지 테이블을 걸어 내려간다 */
}
EXPORT_SYMBOL_GPL(iommu_iova_to_phys);

/*
 * [한국어]
 * iommu_pgsize - 이번 조각에 쓸 페이지 크기와 장수를 고른다
 *
 * @domain: 대상 도메인. pgsize_bitmap 이 이 하드웨어가 PTE 하나로 표현할 수
 *          있는 크기들을 비트로 알려 준다 (x86 이면 보통 4K|2M|1G, ARM LPAE 면
 *          입도에 따라 4K/16K/64K 와 그 블록 크기들).
 * @iova:   이번 조각이 시작될 IO 가상 주소.
 * @paddr:  대응하는 물리 주소. 해제 경로는 여기에 iova 를 그대로 넘겨
 *          "물리 제약 없음"을 표현한다.
 * @size:   아직 처리하지 못하고 남은 길이.
 * @count:  NULL 이 아니면 "고른 크기의 페이지 몇 장"을 여기에 돌려준다.
 * @return: 고른 페이지 크기(바이트).
 *
 * IOMMU 페이지 테이블은 한 가지 크기만 쓰지 않는다. 큰 페이지를 쓸수록 PTE 도
 * IOTLB 엔트리도 적게 들지만, 큰 페이지는 IOVA 와 물리 주소가 '동시에' 그
 * 경계에 정렬되어 있을 때만 쓸 수 있다. 이 함수가 그 판단을 전담한다.
 *
 * 판단은 세 단계다.
 *   1) 남은 길이보다 큰 크기는 후보에서 제외 (GENMASK(__fls(size), 0)).
 *   2) 두 주소를 OR 한 합성값의 최하위 1비트보다 큰 크기는 제외 — OR 하면 덜
 *      정렬된 쪽의 하위 비트가 남으므로, 한 번의 __ffs 로 둘의 공통 최대 정렬을
 *      얻는다.
 *   3) 남은 것 중 최대치를 고른 뒤, 한 단계 위 크기로 '승격'할 여지가 있으면
 *      count 를 경계까지로 잘라 둔다. 그러면 호출자가 다음 회차에 다시 들어올
 *      때 주소가 그 경계에 맞아 큰 페이지가 선택된다. 4KB 로 시작해 2MB 블록으로
 *      갈아타는 흔한 패턴이 이 잘라내기 하나로 만들어진다.
 *
 * 실행 컨텍스트: 매핑/해제 루프 안. 락도 잡지 않고 잠들지도 않는 순수 계산이다.
 *
 * 호출 체인: __iommu_map_domain_pgtbl / __iommu_unmap_domain_pgtbl → [이 함수]
 */
static size_t iommu_pgsize(struct iommu_domain *domain, unsigned long iova,
			   phys_addr_t paddr, size_t size, size_t *count)
{
	unsigned int pgsize_idx, pgsize_idx_next;	/* [한국어] 고른 페이지 크기와 그 바로 위 크기의 비트 위치 — pgsize_bitmap 안의 인덱스로 다룬다 */
	unsigned long pgsizes;	/* [한국어] 후보 페이지 크기 집합. pgsize_bitmap 을 이번 요청 조건에 맞게 깎아 낸 비트마스크 */
	size_t offset, pgsize, pgsize_next;	/* [한국어] offset: 다음 큰 페이지 경계까지 남은 거리. pgsize/pgsize_next: 비트 위치를 실제 바이트 크기로 편 값 */
	size_t offset_end;	/* [한국어] offset + pgsize_next 를 오버플로 검사와 함께 받아 둘 임시값 */
	unsigned long addr_merge = paddr | iova;	/* [한국어] 정렬 판정용 합성 주소 — 두 주소를 OR 하면 덜 정렬된 쪽의 하위 1비트가 살아남는다. IOVA 와 물리 주소가 '동시에' 만족하는 최대 정렬을 한 번에 구하는 관용구 */

	/* Page sizes supported by the hardware and small enough for @size */
	pgsizes = domain->pgsize_bitmap & GENMASK(__fls(size), 0);	/* [한국어] 1차 후보: 하드웨어가 지원하는 크기 중 요청 길이 size 를 넘지 않는 것만. __fls(size) 는 size 이하 최대 2의 거듭제곱의 비트 위치 */

	/* Constrain the page sizes further based on the maximum alignment */
	if (likely(addr_merge))	/* [한국어] 주소가 0 이면 정렬 제약이 없다(모든 크기에 정렬됨) — 그 예외를 제외하고 */
		pgsizes &= GENMASK(__ffs(addr_merge), 0);	/* [한국어] 2차 후보: 합성 주소의 최하위 1비트 위치까지만 남긴다. 그보다 큰 페이지는 주소가 경계에 맞지 않아 PTE 로 표현할 수 없다 */

	/* Make sure we have at least one suitable page size */
	BUG_ON(!pgsizes);	/* [한국어] 후보가 하나도 없다 = 호출자가 min_pagesz 정렬 검사를 통과시켜 놓고 어긋난 값을 넘겼다는 뜻. 페이지 테이블을 망가뜨리기 전에 여기서 멈춘다 */

	/* Pick the biggest page size remaining */
	pgsize_idx = __fls(pgsizes);	/* [한국어] 남은 후보 중 가장 큰 것을 고른다 — 큰 페이지일수록 PTE 개수도 IOTLB 엔트리 소모도 줄어든다 */
	pgsize = BIT(pgsize_idx);	/* [한국어] 비트 위치를 바이트 크기로 편다 (예: 21 → 2MB) */
	if (!count)	/* [한국어] 개수를 묻지 않는 호출자는 페이지 크기 하나만 필요하다 — 승격 검토를 생략한다 */
		return pgsize;	/* [한국어] 고른 페이지 크기 하나만 돌려준다 */

	/* Find the next biggest support page size, if it exists */
	pgsizes = domain->pgsize_bitmap & ~GENMASK(pgsize_idx, 0);	/* [한국어] 방금 고른 크기보다 '큰' 지원 크기만 남긴다 — 한 단계 위 블록으로 승격할 여지가 있는지 보기 위해 */
	if (!pgsizes)	/* [한국어] 위 단계가 없으면 승격 검토 자체가 무의미 */
		goto out_set_count;	/* [한국어] 고른 크기로 남은 길이 전부를 채운다 */

	pgsize_idx_next = __ffs(pgsizes);	/* [한국어] 바로 한 단계 위 크기의 비트 위치 */
	pgsize_next = BIT(pgsize_idx_next);	/* [한국어] 그 크기를 바이트로 (예: 4KB 다음이 2MB) */

	/*
	 * There's no point trying a bigger page size unless the virtual
	 * and physical addresses are similarly offset within the larger page.
	 */
	if ((iova ^ paddr) & (pgsize_next - 1))	/* [한국어] 큰 페이지 '안에서의' 오프셋이 IOVA 와 물리 주소끼리 다르면, 아무리 전진해도 두 주소가 동시에 큰 경계에 닿는 순간이 오지 않는다 — 승격 불가 */
		goto out_set_count;	/* [한국어] 승격이 불가능하므로 작은 페이지로 끝까지 간다 */

	/* Calculate the offset to the next page size alignment boundary */
	offset = pgsize_next - (addr_merge & (pgsize_next - 1));	/* [한국어] 현재 주소에서 다음 큰 페이지 경계까지 남은 거리 */

	/*
	 * If size is big enough to accommodate the larger page, reduce
	 * the number of smaller pages.
	 */
	if (!check_add_overflow(offset, pgsize_next, &offset_end) &&	/* [한국어] 경계까지 간 뒤 큰 페이지가 통째로 하나 더 들어가는가 — 그 덧셈이 오버플로하지 않는지까지 함께 본다 */
	    offset_end <= size)	/* [한국어] 요청 길이 안에서 그것이 실제로 가능하다면 */
		size = offset;	/* [한국어] 이번 호출은 경계까지만 작은 페이지로 채운다. 호출자가 루프를 돌아 다시 들어오면 그때는 주소가 정렬되어 큰 페이지가 선택된다 — 4KB 로 시작해 2MB 블록으로 갈아타는 승격이 이 한 줄로 이뤄진다 */

out_set_count:	/* [한국어] 승격 검토를 건너뛴 두 경로가 합류하는 지점 */
	*count = size >> pgsize_idx;	/* [한국어] 고른 크기의 페이지 몇 장으로 size 를 덮는지 — 드라이버 map_pages 에 한 번에 넘길 장수 */
	return pgsize;	/* [한국어] (페이지 크기, 장수) 쌍을 확정해 돌려준다 */
}

/*
 * [한국어]
 * __iommu_map_domain_pgtbl - 벤더 드라이버의 map_pages 를 반복 호출해 범위를 매핑한다 (레거시 경로)
 *
 * @domain: 매핑을 넣을 도메인. __IOMMU_DOMAIN_PAGING 이어야 한다.
 * @iova:   매핑 시작 IO 가상 주소.
 * @paddr:  대응하는 물리 시작 주소. 이 함수가 다루는 것은 물리적으로 연속인
 *          한 구간이다 — 흩어진 메모리는 호출자가 조각내어 여러 번 부른다.
 * @size:   길이. iova|paddr|size 가 모두 최소 페이지 크기에 정렬되어야 한다.
 * @prot:   IOMMU_READ/WRITE/CACHE/NOEXEC/MMIO 조합. 드라이버가 PTE 권한 비트로 옮긴다.
 * @gfp:    페이지 테이블 페이지 할당에 쓸 플래그. GFP_KERNEL 이면 잠들 수 있다.
 * @return: 0 이면 전 범위 매핑 완료. 음수면 실패이며, 이 함수가 부분 매핑까지
 *          모두 되감은 뒤 돌아오므로 호출자는 정리할 것이 없다.
 *
 * IOMMU 코어에는 페이지 테이블을 다루는 두 가지 길이 있다. 하나는 각 벤더
 * 드라이버가 자기 포맷의 페이지 테이블을 직접 관리하고 코어는 map_pages 를
 * 조각 단위로 부르는 전통적인 길이고(이 함수), 다른 하나는 공용 페이지 테이블
 * 계층(iommupt)이 범위를 통째로 처리하는 새 길이다. iommu_map_nosync 가 도메인을
 * 보고 둘 중 하나로 갈라 준다.
 *
 * 되감기 방식에 이 함수의 성격이 드러난다. 드라이버는 실패하더라도 이미 기입한
 * 바이트 수를 mapped 로 알려 주고, 코어는 ret 을 보기 전에 먼저 size 에서 그만큼을
 * 뺀다. 그래서 실패 시 "원본 길이 − 남은 길이"가 정확히 되감아야 할 길이가 된다.
 * 매핑 API 는 이렇게 부분 성공을 밖으로 내보내지 않는다 — 성공이면 전부, 실패면
 * 아무것도.
 *
 * 실행 컨텍스트: 프로세스 문맥이 기본. gfp 에 GFP_ATOMIC 을 주면 아토믹 문맥에서도
 * 부를 수 있고, might_sleep_if 가 그 계약을 검사한다. 도메인 단위 동시 호출은
 * 드라이버 책임이다.
 *
 * 호출 체인: iommu_map_nosync → [이 함수] → iommu_pgsize, ops->map_pages
 */
static int __iommu_map_domain_pgtbl(struct iommu_domain *domain,
				    unsigned long iova, phys_addr_t paddr,
				    size_t size, int prot, gfp_t gfp)
{
	const struct iommu_domain_ops *ops = domain->ops;	/* [한국어] 이 도메인을 실제로 구현한 벤더 드라이버의 콜백 표 (intel/amd/arm-smmu ...) */
	unsigned long orig_iova = iova;	/* [한국어] 되감기 시작점 — iova 는 루프에서 전진하므로 원본을 따로 보관한다 */
	unsigned int min_pagesz;	/* [한국어] 이 도메인이 다룰 수 있는 최소 페이지 크기 */
	size_t orig_size = size;	/* [한국어] 되감을 길이를 계산하기 위한 원본 길이 */
	int ret = 0;	/* [한국어] 드라이버 map_pages 의 마지막 반환값 */

	might_sleep_if(gfpflags_allow_blocking(gfp));	/* [한국어] GFP_KERNEL 로 불렸다면 페이지 테이블 페이지 할당에서 잠들 수 있다 — 아토믹 문맥에서의 오용을 여기서 잡는다 */

	if (unlikely(!(domain->type & __IOMMU_DOMAIN_PAGING)))	/* [한국어] 페이지 테이블 자체가 없는 도메인(IDENTITY/BLOCKED)에는 매핑을 만들 수 없다 */
		return -EINVAL;	/* [한국어] 번역 없는 도메인에 매핑을 요청한 것은 호출자 오류 */

	if (WARN_ON(!ops->map_pages || domain->pgsize_bitmap == 0UL))	/* [한국어] 드라이버가 map_pages 를 두지 않았거나 지원 페이지 크기를 하나도 알리지 않았다 — 드라이버 등록 단계의 버그 */
		return -ENODEV;	/* [한국어] 하드웨어가 매핑을 제공하지 못한다 */

	/* Discourage passing strange GFP flags */
	if (WARN_ON_ONCE(gfp & (__GFP_COMP | __GFP_DMA | __GFP_DMA32 |	/* [한국어] 페이지 테이블 페이지에 써서는 안 되는 플래그들. __GFP_COMP(복합 페이지)는 페이지 테이블 할당자의 가정과 충돌하고, DMA/DMA32 존 지정은 불필요한 저역 메모리 고갈을 부른다 */
				__GFP_HIGHMEM)))	/* [한국어] HIGHMEM 은 커널 선형 매핑이 없어 드라이버가 PTE 를 직접 쓸 수조차 없다 */
		return -EINVAL;	/* [한국어] 이상한 GFP 조합은 거절 */

	/* find out the minimum page size supported */
	min_pagesz = 1 << __ffs(domain->pgsize_bitmap);	/* [한국어] 지원 크기 중 가장 작은 것 — 매핑의 모든 값이 최소한 여기에는 정렬되어야 한다 */

	/*
	 * both the virtual address and the physical one, as well as
	 * the size of the mapping, must be aligned (at least) to the
	 * size of the smallest page supported by the hardware
	 */
	if (!IS_ALIGNED(iova | paddr | size, min_pagesz)) {	/* [한국어] IOVA·물리 주소·길이 셋 중 하나라도 어긋나면 어떤 PTE 조합으로도 표현할 수 없다. OR 로 셋을 한 번에 검사한다 */
		pr_err("unaligned: iova 0x%lx pa %pa size 0x%zx min_pagesz 0x%x\n",	/* [한국어] 정렬 위반은 호출자(주로 DMA 계층)의 버그이므로 조용히 넘기지 않고 값을 그대로 남긴다 */
		       iova, &paddr, size, min_pagesz);	/* [한국어] 어긋난 값과 요구 정렬을 함께 찍어 원인 추적을 돕는다 */
		return -EINVAL;	/* [한국어] 정렬 위반 — 아무 것도 매핑하지 않고 돌아간다 */
	}

	pr_debug("map: iova 0x%lx pa %pa size 0x%zx\n", iova, &paddr, size);	/* [한국어] 요청 전체를 한 줄로 남긴다 (동적 디버그로 켤 때만 출력) */

	while (size) {	/* [한국어] 요청 범위를 페이지 크기 단위 조각으로 나눠 모두 소진할 때까지 반복 */
		size_t pgsize, count, mapped = 0;	/* [한국어] 이번 조각의 페이지 크기, 연속 장수, 그리고 드라이버가 실제로 매핑한 바이트 수 */

		pgsize = iommu_pgsize(domain, iova, paddr, size, &count);	/* [한국어] 현재 주소 정렬과 남은 길이로부터 쓸 수 있는 최대 페이지 크기와 장수를 고른다 */

		pr_debug("mapping: iova 0x%lx pa %pa pgsize 0x%zx count %zu\n",	/* [한국어] 조각 단위 추적 — 어떤 크기로 쪼개졌는지가 성능 분석의 출발점 */
			 iova, &paddr, pgsize, count);	/* [한국어] 이번 조각의 주소·크기·장수 */
		ret = ops->map_pages(domain, iova, paddr, pgsize, count, prot,	/* [한국어] 벤더 드라이버에 실제 PTE 기입을 위임한다 — 하드웨어 페이지 테이블이 바뀌는 지점이 바로 여기다 */
				     gfp, &mapped);	/* [한국어] mapped 로 '성공적으로 기입한 바이트 수'를 돌려받는다 */
		/*
		 * Some pages may have been mapped, even if an error occurred,
		 * so we should account for those so they can be unmapped.
		 */
		size -= mapped;	/* [한국어] ret 검사보다 먼저 빼는 것이 핵심. 에러가 나도 일부는 이미 매핑됐을 수 있고, 그만큼은 되감기 대상에 포함되어야 하기 때문이다 */

		if (ret)	/* [한국어] 실패면 더 진행하지 않고 되감기로 빠진다 */
			break;	/* [한국어] 루프 탈출 — size 에는 매핑하지 못한 잔여 길이가 남는다 */

		iova += mapped;	/* [한국어] IOVA 커서를 실제 매핑된 만큼 전진 */
		paddr += mapped;	/* [한국어] 물리 커서도 같은 만큼 전진 — 이 함수는 물리적으로 연속인 한 구간만 다룬다 */
	}

	/* unroll mapping in case something went wrong */
	if (ret) {	/* [한국어] 부분 매핑이 남아 있다면 원자성을 흉내 내기 위해 전부 되돌린다 */
		iommu_unmap(domain, orig_iova, orig_size - size);	/* [한국어] 실제로 매핑된 길이 = 원본 길이 − 남은 길이. 덕분에 호출자는 실패 후 아무 것도 정리할 필요가 없다 */
		return ret;	/* [한국어] 드라이버가 준 에러를 그대로 올린다 */
	}
	return 0;	/* [한국어] 요청 범위 전체가 페이지 테이블에 들어갔다 */
}

/*
 * [한국어]
 * iommu_sync_map - 새로 기입한 PTE 를 하드웨어에 보이게 만든다
 *
 * @domain: 매핑이 추가된 도메인.
 * @iova:   반영할 범위의 시작.
 * @size:   반영할 범위의 길이.
 * @return: 0 이면 성공. 드라이버가 실패를 알리면 그 에러.
 *
 * 해제 쪽의 IOTLB 무효화와 짝을 이루지만 성격이 다르다. 해제는 옛 번역을 지우는
 * 것이라 반드시 필요하고, 추가는 대부분의 하드웨어에서 아무 일도 하지 않아도
 * 된다 — 없던 자리에 생긴 엔트리는 다음 워크에서 자연스럽게 읽히기 때문이다.
 * 다만 "매핑 없음"까지 캐시하거나(negative caching) 페이지 테이블 워크 결과를
 * 캐시하는 구현이 있어서, 그런 하드웨어만 iotlb_sync_map 을 채워 둔다.
 *
 * 이 단계를 iommu_map 에서 떼어 낸 이유는 배치다. iommu_map_sg 는 세그먼트마다
 * nosync 로 기입한 뒤 마지막에 이 함수를 한 번만 부른다.
 *
 * 실행 컨텍스트: 매핑 경로와 동일. 드라이버 구현에 따라 잠들 수 있다.
 *
 * 호출 체인: iommu_map, iommu_map_sg, dma-iommu → [이 함수] → ops->iotlb_sync_map
 */
int iommu_sync_map(struct iommu_domain *domain, unsigned long iova, size_t size)
{
	const struct iommu_domain_ops *ops = domain->ops;	/* [한국어] 벤더 콜백 표 */

	if (!ops->iotlb_sync_map)	/* [한국어] 매핑 '추가' 뒤에 별도 동기화가 필요 없는 하드웨어가 다수다 — 그때는 할 일이 없다 */
		return 0;	/* [한국어] 동기화 불필요 = 성공 */
	return ops->iotlb_sync_map(domain, iova, size);	/* [한국어] '매핑 없음'까지 캐시하는 IOMMU(negative caching)나 페이지 테이블 워크 캐시를 가진 구현에서, 새로 만든 엔트리를 하드웨어에 보이게 만든다 */
}

/*
 * [한국어]
 * iommu_map_nosync - 동기화 없이 페이지 테이블에만 매핑을 기입한다
 *
 * @domain: 대상 도메인.
 * @iova:   매핑 시작 IO 가상 주소.
 * @paddr:  물리 시작 주소 (연속 구간).
 * @size:   길이.
 * @prot:   접근 권한 비트.
 * @gfp:    페이지 테이블 할당 플래그.
 * @return: 0 이면 성공. 음수면 실패이며 부분 매핑은 내부에서 되감긴 뒤다.
 *
 * 매핑 경로의 갈림길이다. 도메인이 공용 페이지 테이블 계층(iommupt)으로 만들어져
 * 있으면 pt->ops->map_range 가 범위 전체를 한 번에 처리하고, 아니면 전통적인
 * __iommu_map_domain_pgtbl 이 iommu_pgsize 로 조각내며 드라이버 map_pages 를
 * 반복 호출한다. 두 경로 모두 실패 시 스스로 되감으므로 호출자에게 보이는 계약은
 * 같다.
 *
 * 이름의 nosync 는 iotlb_sync_map 을 생략한다는 뜻이다. 여러 번 기입하고 한 번만
 * 동기화하려는 호출자(iommu_map_sg, dma-iommu)를 위한 것이며, 이 함수만 부르고
 * 동기화를 잊으면 일부 하드웨어에서 새 매핑이 보이지 않는다.
 *
 * 실행 컨텍스트: gfp 가 허용하면 잠들 수 있다.
 *
 * 호출 체인: iommu_map, iommu_map_sg, dma-iommu → [이 함수]
 *            → pt->ops->map_range 또는 __iommu_map_domain_pgtbl
 */
int iommu_map_nosync(struct iommu_domain *domain, unsigned long iova,
		phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{
	struct pt_iommu *pt = iommupt_from_domain(domain);	/* [한국어] 이 도메인이 공용 페이지 테이블 계층(iommupt)으로 만들어졌는지 확인한다. 그렇다면 벤더 콜백을 코어가 루프 돌리는 대신 공용 코드가 범위를 통째로 처리한다 */
	int ret;	/* [한국어] 두 경로가 공유하는 반환값 */

	if (pt) {	/* [한국어] 공용 페이지 테이블 경로 — 신형 드라이버들이 이쪽으로 옮겨 가고 있다 */
		size_t mapped = 0;	/* [한국어] 실패 시 되감을 길이를 공용 계층이 채워 준다 */

		ret = pt->ops->map_range(pt, iova, paddr, size, prot, gfp,	/* [한국어] 범위 전체를 한 번의 호출로 — 페이지 크기 선택과 반복이 공용 계층 안으로 들어가 있다 */
					 &mapped);	/* [한국어] 부분 성공 바이트 수 회신 창구 */
		if (ret) {	/* [한국어] 공용 계층이 실패를 알렸다 */
			iommu_unmap(domain, iova, mapped);	/* [한국어] 부분 성공분을 되감아 호출자에게 '아무 일도 없었던' 상태로 돌려준다 */
			return ret;	/* [한국어] 되감기까지 마친 뒤 에러 전달 */
		}
		return 0;	/* [한국어] 공용 경로 성공 */
	}
	ret = __iommu_map_domain_pgtbl(domain, iova, paddr, size, prot, gfp);	/* [한국어] 레거시 경로 — 코어가 iommu_pgsize 로 쪼개어 드라이버 map_pages 를 직접 반복 호출한다 */
	if (!ret)	/* [한국어] 주의: 조건이 뒤집혀 있다. 성공(ret==0)일 때 곧장 0 을 돌려주므로 아래 trace_map 은 오히려 '실패했을 때' 실행되고, 그 실패는 return 0 으로 삼켜진다. 원 의도는 if (ret) return ret; 로 보인다 — 코드는 손대지 않고 사실만 남긴다 */
		return ret;	/* [한국어] 여기서는 ret 이 0 이므로 성공 반환과 같다 */

	trace_map(iova, paddr, size);	/* [한국어] 매핑 이벤트를 ftrace 로 남긴다 (위의 조건 때문에 실제로는 실패 경로에서 실행된다) */
	iommu_debug_map(domain, paddr, size);	/* [한국어] 디버그 계층에 매핑 사실을 기록 — 나중에 해제와 짝이 맞는지 대조하는 용도 */
	return 0;	/* [한국어] 성공으로 보고한다 */
}

/*
 * [한국어]
 * iommu_map - IOVA 범위 하나를 물리 메모리에 매핑한다 (외부 공개 API)
 *
 * @domain: 매핑을 넣을 도메인. 장치는 attach 를 통해 이미 이 도메인을 보고 있다.
 * @iova:   매핑할 IO 가상 주소. IOVA 할당은 이 계층의 일이 아니다 — dma-iommu 의
 *          iova 할당자나 VFIO/iommufd 의 사용자 요청이 정해서 내려보낸다.
 * @paddr:  물리 시작 주소.
 * @size:   길이.
 * @prot:   IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE | IOMMU_NOEXEC | IOMMU_MMIO.
 * @gfp:    페이지 테이블 할당 플래그.
 * @return: 0 성공, 음수 에러. 실패 시 아무 매핑도 남지 않는다.
 *
 * 장치가 DMA 로 메모리에 닿을 수 있게 만드는 최종 지점이다. NVMe 드라이버가
 * dma_map_page 를 부르면 dma-iommu 가 IOVA 를 하나 떼어 이 함수로 내려오고,
 * 그때 비로소 장치가 낸 주소를 IOMMU 가 실제 물리 페이지로 번역할 수 있게 된다.
 *
 * 두 단계로 나뉜다. 기입(iommu_map_nosync)과 반영(iommu_sync_map)이다. 반영이
 * 실패하면 PTE 는 있으나 하드웨어가 볼지 알 수 없는 상태가 되므로 만든 것을 도로
 * 지운다 — 여기서도 "성공이면 전부, 실패면 아무것도"가 지켜진다.
 *
 * 실행 컨텍스트: gfp 가 허용하면 잠들 수 있다. 같은 도메인에 대한 동시 매핑은
 * 드라이버 또는 상위 계층이 직렬화한다.
 *
 * 호출 체인: dma-iommu, VFIO/iommufd, 드라이버 → [이 함수]
 *            → iommu_map_nosync → iommu_sync_map
 */
int iommu_map(struct iommu_domain *domain, unsigned long iova,
	      phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{
	int ret;	/* [한국어] 기입과 동기화 두 단계의 결과를 차례로 받는다 */

	ret = iommu_map_nosync(domain, iova, paddr, size, prot, gfp);	/* [한국어] 1단계: 페이지 테이블에 PTE 를 채운다 */
	if (ret)	/* [한국어] 기입 실패는 이미 내부에서 되감겼으므로 그대로 올리면 된다 */
		return ret;	/* [한국어] 실패 전달 */

	ret = iommu_sync_map(domain, iova, size);	/* [한국어] 2단계: 하드웨어가 새 엔트리를 보도록 만든다. 이 두 단계를 나눠 둔 덕분에 map_sg 는 여러 번 기입하고 sync 는 한 번만 할 수 있다 */
	if (ret)	/* [한국어] 동기화가 실패하면 PTE 는 있으나 하드웨어가 볼지 알 수 없는 어중간한 상태가 된다 */
		iommu_unmap(domain, iova, size);	/* [한국어] 만든 매핑을 도로 지워 어중간한 상태를 남기지 않는다 */

	return ret;	/* [한국어] 성공이면 0, 동기화 실패면 그 에러 */
}
EXPORT_SYMBOL_GPL(iommu_map);

/*
 * [한국어]
 * __iommu_unmap_domain_pgtbl - 드라이버 unmap_pages 를 반복 호출해 범위를 해제한다 (레거시 경로)
 *
 * @domain:       해제할 도메인.
 * @iova:         해제 시작 IO 가상 주소.
 * @size:         해제할 길이.
 * @iotlb_gather: 무효화가 필요한 구간을 모으는 수집기. 드라이버가 여기에 범위를
 *                누적하고, 실제 무효화는 상위의 iommu_iotlb_sync 가 한 번에 낸다.
 * @return:       실제로 해제된 바이트 수. 요청보다 적으면 매핑이 없는 구간을 만난
 *                것이다. 에러 코드가 아니라 진행량을 돌려주는 것이 이 계열의 관례다.
 *
 * 매핑 쪽과 대칭이지만 두 가지가 다르다. 첫째, 물리 주소가 필요 없으므로 정렬
 * 검사와 iommu_pgsize 호출 모두 IOVA 만 본다 (그래서 paddr 자리에 iova 를 한 번 더
 * 넘긴다). 둘째, PTE 를 지웠다고 끝이 아니다 — IOTLB 에 남은 옛 번역을 무효화하기
 * 전까지 장치는 여전히 옛 물리 페이지에 닿을 수 있다. 그 무효화 범위를 모으는 것이
 * iotlb_gather 이고, 드라이버가 gather 를 쓰지 않는 구현이면 코어가 형식상 범위를
 * 채워 sync 콜백이 반드시 한 번은 불리도록 보장한다.
 *
 * 실행 컨텍스트: 아토믹 문맥에서도 불릴 수 있다 (dma_unmap 은 인터럽트 문맥에서
 * 온다). 그래서 이 경로에는 메모리 할당이 없다.
 *
 * 호출 체인: __iommu_unmap → [이 함수] → iommu_pgsize, ops->unmap_pages
 */
static size_t
__iommu_unmap_domain_pgtbl(struct iommu_domain *domain, unsigned long iova,
			   size_t size, struct iommu_iotlb_gather *iotlb_gather)
{
	const struct iommu_domain_ops *ops = domain->ops;	/* [한국어] 벤더 콜백 표 */
	size_t unmapped_page, unmapped = 0;	/* [한국어] 이번 회차에 해제된 바이트와 누적 해제 바이트 */
	unsigned int min_pagesz;	/* [한국어] 정렬 검사 기준이 되는 최소 페이지 크기 */

	if (unlikely(!(domain->type & __IOMMU_DOMAIN_PAGING)))	/* [한국어] 페이지 테이블이 없는 도메인에는 지울 것도 없다 */
		return 0;	/* [한국어] 해제 계열은 에러 코드가 아니라 '해제한 바이트 수'를 돌려준다 — 0 은 아무 것도 못 지웠다는 뜻 */

	if (WARN_ON(!ops->unmap_pages || domain->pgsize_bitmap == 0UL))	/* [한국어] 드라이버가 unmap_pages 를 두지 않았다 — 등록 단계의 버그 */
		return 0;	/* [한국어] 해제 불가 */

	/* find out the minimum page size supported */
	min_pagesz = 1 << __ffs(domain->pgsize_bitmap);	/* [한국어] 지원 크기 중 최소값 */

	/*
	 * The virtual address, as well as the size of the mapping, must be
	 * aligned (at least) to the size of the smallest page supported
	 * by the hardware
	 */
	if (!IS_ALIGNED(iova | size, min_pagesz)) {	/* [한국어] 해제에는 물리 주소가 필요 없으므로 IOVA 와 길이만 본다 */
		pr_err("unaligned: iova 0x%lx size 0x%zx min_pagesz 0x%x\n",	/* [한국어] 정렬이 어긋난 해제 요청은 매핑과 짝이 맞지 않는다는 신호 */
		       iova, size, min_pagesz);	/* [한국어] 문제 값을 그대로 남긴다 */
		return 0;	/* [한국어] 0 바이트 해제로 보고 — 호출자는 요청이 무시됐음을 알 수 있다 */
	}

	pr_debug("unmap this: iova 0x%lx size 0x%zx\n", iova, size);	/* [한국어] 해제 요청 전체를 한 줄로 */

	iommu_debug_unmap_begin(domain, iova, size);	/* [한국어] 해제 직전 상태를 디버그 계층에 남긴다 — 뒤의 unmap_end 와 대조해 매핑/해제 짝을 검증한다 */

	/*
	 * Keep iterating until we either unmap 'size' bytes (or more)
	 * or we hit an area that isn't mapped.
	 */
	while (unmapped < size) {	/* [한국어] 요청 길이를 다 채울 때까지 조각 단위로 지운다 */
		size_t pgsize, count;	/* [한국어] 이번 조각의 페이지 크기와 장수 */

		pgsize = iommu_pgsize(domain, iova, iova, size - unmapped, &count);	/* [한국어] 물리 주소 자리에 iova 를 한 번 더 넘기는 것에 주의 — 해제에는 물리 주소가 무의미하므로 정렬 판정을 IOVA 만으로 하게 만드는 관용구다 */
		unmapped_page = ops->unmap_pages(domain, iova, pgsize, count, iotlb_gather);	/* [한국어] 벤더 드라이버가 PTE 를 지우고 실제로 지운 바이트 수를 돌려준다. 이 시점에는 아직 IOTLB 가 옛 번역을 들고 있을 수 있다 */
		if (!unmapped_page)	/* [한국어] 한 바이트도 못 지웠다 = 매핑이 없는 구간에 닿았다 */
			break;	/* [한국어] 요청 길이를 다 못 채웠어도 여기서 멈춘다 — 없는 매핑을 억지로 지우지 않는다 */

		pr_debug("unmapped: iova 0x%lx size 0x%zx\n",	/* [한국어] 조각 단위 해제 추적 */
			 iova, unmapped_page);	/* [한국어] 이번 조각의 주소와 크기 */
		/*
		 * If the driver itself isn't using the gather, make sure
		 * it looks non-empty so iotlb_sync will still be called.
		 */
		if (iotlb_gather->start >= iotlb_gather->end)	/* [한국어] 드라이버가 gather 에 범위를 적지 않았다면(자체적으로 즉시 무효화하는 구현) 수집기는 빈 채로 남는다 */
			iommu_iotlb_gather_add_range(iotlb_gather, iova, size);	/* [한국어] 빈 gather 를 만나면 상위의 iommu_iotlb_sync 가 아무 일도 하지 않고 지나간다. 형식상 범위를 채워 sync 콜백이 반드시 한 번은 불리도록 보장한다 */

		iova += unmapped_page;	/* [한국어] IOVA 커서를 지운 만큼 전진 */
		unmapped += unmapped_page;	/* [한국어] 누적 해제량 갱신 — 루프 종료 조건이기도 하다 */
	}

	return unmapped;	/* [한국어] 실제 해제된 총 바이트. 호출자는 iova + 반환값이 '멈춘 지점'임을 알 수 있다 */
}

/*
 * [한국어]
 * __iommu_unmap - 해제 경로의 갈림길 (공용 페이지 테이블 vs 레거시)
 *
 * @domain:       해제할 도메인.
 * @iova:         시작 주소.
 * @size:         길이.
 * @iotlb_gather: 무효화 범위 수집기. 동기화는 여기서 하지 않는다.
 * @return:       해제된 바이트 수.
 *
 * iommu_map_nosync 의 해제판이다. 도메인이 공용 페이지 테이블 계층으로 만들어져
 * 있으면 unmap_range 가 범위를 통째로 지우고, 아니면 코어가 조각 단위로 드라이버
 * unmap_pages 를 반복한다. 어느 쪽이든 IOTLB 무효화는 gather 에 쌓아 두기만 하고
 * 내리지 않는다 — 그 결정을 호출자에게 남기는 것이 iommu_unmap 과 iommu_unmap_fast
 * 를 가르는 유일한 차이다.
 *
 * 추적 훅 두 개(trace_unmap, iommu_debug_unmap_end)가 여기 모여 있어, 어느 경로로
 * 갔든 요청 길이와 실제 해제량의 대조가 한 곳에서 이뤄진다.
 *
 * 실행 컨텍스트: 아토믹 문맥 가능.
 *
 * 호출 체인: iommu_unmap, iommu_unmap_fast → [이 함수]
 *            → pt->ops->unmap_range 또는 __iommu_unmap_domain_pgtbl
 */
static size_t __iommu_unmap(struct iommu_domain *domain, unsigned long iova,
			    size_t size,
			    struct iommu_iotlb_gather *iotlb_gather)
{
	struct pt_iommu *pt = iommupt_from_domain(domain);	/* [한국어] 매핑 쪽과 대칭 — 공용 페이지 테이블 계층으로 만들어진 도메인인지 판별한다 */
	size_t unmapped;	/* [한국어] 두 경로가 공유하는 해제 바이트 수 */

	if (pt)	/* [한국어] 공용 페이지 테이블 경로 */
		unmapped = pt->ops->unmap_range(pt, iova, size, iotlb_gather);	/* [한국어] 범위 전체를 한 번에 지우고, 무효화가 필요한 구간을 gather 에 모아 준다 */
	else
		unmapped = __iommu_unmap_domain_pgtbl(domain, iova, size,	/* [한국어] 레거시 경로 — 코어가 조각 단위로 드라이버 unmap_pages 를 반복 호출한다 */
						      iotlb_gather);	/* [한국어] 무효화 범위 수집기를 그대로 넘겨 준다 */
	trace_unmap(iova, size, unmapped);	/* [한국어] 요청 길이와 실제 해제량을 함께 남긴다 — 둘이 다르면 매핑 누락이나 이중 해제의 단서가 된다 */
	iommu_debug_unmap_end(domain, iova, size, unmapped);	/* [한국어] unmap_begin 과 짝을 이뤄 디버그 계층이 매핑/해제 대응을 검증하게 한다 */
	return unmapped;	/* [한국어] 해제된 총 바이트 */
}

/**
 * iommu_unmap() - Remove mappings from a range of IOVA
 * @domain: Domain to manipulate
 * @iova: IO virtual address to start
 * @size: Length of the range starting from @iova
 *
 * iommu_unmap() will remove a translation created by iommu_map(). It cannot
 * subdivide a mapping created by iommu_map(), so it should be called with IOVA
 * ranges that match what was passed to iommu_map(). The range can aggregate
 * contiguous iommu_map() calls so long as no individual range is split.
 *
 * Returns: Number of bytes of IOVA unmapped. iova + res will be the point
 * unmapping stopped.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_unmap - 매핑을 지우고 IOTLB 무효화까지 끝낸다 (안전한 기본 API)
 *
 * @domain: 대상 도메인.
 * @iova:   해제 시작 주소.
 * @size:   길이.
 * @return: 실제 해제된 바이트 수.
 *
 * 이 함수가 돌아온 뒤에야 "그 IOVA 로 오는 DMA 가 더 이상 옛 물리 페이지에 닿지
 * 않는다"가 보장된다. 그래서 페이지를 해제하거나 다른 용도로 재사용하기 직전에
 * 반드시 거쳐야 하는 문이다. 무효화를 건너뛰면 이미 반납된 페이지에 장치가 DMA 를
 * 쏘는, 추적하기 가장 어려운 부류의 메모리 손상이 생긴다.
 *
 * 매핑을 쪼갤 수는 없다는 제약(위 영어 주석)에 주의할 것. iommu_map 으로 만든
 * 범위를 그보다 작게 잘라 해제할 수는 없고, 연속된 여러 매핑을 한 번에 지우는 것만
 * 가능하다. 페이지 테이블이 "어디까지가 한 번의 매핑이었는지"를 기억하지 않기
 * 때문이다.
 *
 * 실행 컨텍스트: 아토믹 문맥 가능. 다만 iommu_iotlb_sync 는 하드웨어 완료를
 * 기다리므로 이 경로가 곧 해제 지연의 대부분이다 — 그 비용을 미루려고 만든 것이
 * iommu_unmap_fast 와 dma-iommu 의 flush queue 다.
 *
 * 호출 체인: dma-iommu, VFIO/iommufd, 매핑 실패 되감기 → [이 함수]
 *            → __iommu_unmap → iommu_iotlb_sync
 */
size_t iommu_unmap(struct iommu_domain *domain,
		   unsigned long iova, size_t size)
{
	struct iommu_iotlb_gather iotlb_gather;	/* [한국어] 이 호출 안에서만 쓰고 버리는 무효화 범위 수집기 — 스택에 둔다 */
	size_t ret;	/* [한국어] 해제된 바이트 수 */

	iommu_iotlb_gather_init(&iotlb_gather);	/* [한국어] 빈 범위로 초기화 (start > end 인 '아직 아무 것도 없음' 상태) */
	ret = __iommu_unmap(domain, iova, size, &iotlb_gather);	/* [한국어] PTE 를 지우고 무효화가 필요한 구간을 수집한다 */
	iommu_iotlb_sync(domain, &iotlb_gather);	/* [한국어] 여기서 IOTLB 무효화를 하드웨어에 내리고 완료까지 기다린다. 이 함수가 돌아온 뒤에야 해당 IOVA 로 오는 DMA 가 옛 물리 페이지에 닿지 않음이 보장된다 — 페이지를 반납하거나 재사용해도 안전해지는 시점이 바로 이 줄이다 */

	return ret;	/* [한국어] 해제된 바이트 수를 그대로 전달 */
}
EXPORT_SYMBOL_GPL(iommu_unmap);

/**
 * iommu_unmap_fast() - Remove mappings from a range of IOVA without IOTLB sync
 * @domain: Domain to manipulate
 * @iova: IO virtual address to start
 * @size: Length of the range starting from @iova
 * @iotlb_gather: range information for a pending IOTLB flush
 *
 * iommu_unmap_fast() will remove a translation created by iommu_map().
 * It can't subdivide a mapping created by iommu_map(), so it should be
 * called with IOVA ranges that match what was passed to iommu_map(). The
 * range can aggregate contiguous iommu_map() calls so long as no individual
 * range is split.
 *
 * Basically iommu_unmap_fast() is the same as iommu_unmap() but for callers
 * which manage the IOTLB flushing externally to perform a batched sync.
 *
 * Returns: Number of bytes of IOVA unmapped. iova + res will be the point
 * unmapping stopped.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_unmap_fast - 무효화를 호출자에게 미루고 PTE 만 지운다
 *
 * @domain:       대상 도메인.
 * @iova:         해제 시작 주소.
 * @size:         길이.
 * @iotlb_gather: 호출자가 소유하는 수집기. 여러 번의 해제가 여기에 누적되고,
 *                호출자가 원하는 시점에 iommu_iotlb_sync 로 한 번에 내린다.
 * @return:       해제된 바이트 수.
 *
 * iommu_unmap 과 하는 일은 같고 마지막 한 걸음만 다르다. 그 한 걸음이 비싸기
 * 때문에 따로 존재한다 — IOTLB 무효화는 하드웨어 완료를 기다리는 동기 동작이라,
 * 매 I/O 마다 내리면 스트리밍 DMA 처리량이 눈에 띄게 깎인다. dma-iommu 의
 * DMA_FQ(flush queue) 모드는 해제된 IOVA 를 큐에 쌓아 두었다가 한꺼번에 무효화하고
 * 그때서야 IOVA 를 재사용 가능으로 돌린다.
 *
 * 대가는 창(window)이다. 이 함수가 돌아온 뒤에도 IOTLB 에는 옛 번역이 잠시 남아
 * 있어 장치가 이미 해제된 페이지에 닿을 수 있다. 그래서 이 경로는 커널이 신뢰하는
 * 도메인에서만 쓰고, VFIO/iommufd 처럼 사용자 공간에 장치를 넘기는 경로는 반드시
 * 무효화를 끝낸 뒤 페이지를 놓는다.
 *
 * 실행 컨텍스트: 아토믹 문맥 가능.
 *
 * 호출 체인: dma-iommu(flush queue) → [이 함수] → __iommu_unmap
 *            (무효화는 나중에 iommu_iotlb_sync 가 별도로)
 */
size_t iommu_unmap_fast(struct iommu_domain *domain,
			unsigned long iova, size_t size,
			struct iommu_iotlb_gather *iotlb_gather)
{
	return __iommu_unmap(domain, iova, size, iotlb_gather);	/* [한국어] 동기화를 하지 않고 gather 만 채워 돌려준다. 호출자(주로 dma-iommu 의 flush queue)가 여러 해제를 모아 한 번에 무효화하기 위한 것으로, 스트리밍 DMA 성능을 좌우하는 지점이다. 대가로 '해제 후에도 잠시 옛 번역이 살아 있는' 창이 생기므로 신뢰 도메인에서만 쓴다 */
}
EXPORT_SYMBOL_GPL(iommu_unmap_fast);

/*
 * [한국어]
 * iommu_map_sg - scatter-gather 리스트를 하나의 연속 IOVA 창으로 접는다
 *
 * @domain: 대상 도메인.
 * @iova:   매핑을 시작할 IO 가상 주소. 리스트 전체가 여기서부터 '연속으로' 놓인다.
 * @sg:     scatterlist 의 첫 세그먼트. 체인된 리스트도 sg_next 로 따라간다.
 * @nents:  세그먼트 개수.
 * @prot:   접근 권한 비트.
 * @gfp:    페이지 테이블 할당 플래그.
 * @return: 성공 시 매핑된 총 바이트(양수). 실패 시 음수 에러이며 부분 매핑은
 *          되감긴 뒤다. 반환형이 ssize_t 인 이유가 이 두 의미를 겸하기 위함이다.
 *
 * IOMMU 가 블록/네트워크 스택에 주는 가장 큰 이득이 여기 있다. 물리적으로 흩어진
 * 페이지들을 장치가 보기에는 하나의 연속 버퍼로 만들어 주므로, NVMe 라면 PRP 리스트
 * 대신 단일 SGL 항목으로, 네트워크라면 하나의 DMA 주소로 처리할 수 있게 된다.
 *
 * 두 가지 최적화가 겹쳐 있다.
 *  - 병합: 물리적으로 이어지는 세그먼트들은 len 에 누적만 하고, 끊기는 지점에서만
 *    한 번 매핑한다. 매핑 호출 수와 PTE 개수를 함께 줄인다.
 *  - 지연 동기화: 각 구간은 nosync 로 기입하고 iotlb_sync_map 은 맨 끝에 한 번만.
 *
 * 루프 조건이 i <= nents 인 것에 주목할 것. 마지막 한 바퀴는 세그먼트를 읽으려는
 * 것이 아니라, 모아 두고 아직 매핑하지 않은 잔여 구간을 비워 내기 위한 것이다.
 * 그 바퀴에서는 sg 가 전진하지 않으므로 "이어지지 않는다" 조건이 반드시 참이 된다.
 *
 * sg_dma_is_bus_address 세그먼트는 건너뛴다. P2PDMA 로 PCIe 스위치 안에서 장치끼리
 * 직접 오가는 구간이라 IOMMU 를 아예 거치지 않으며, 페이지 테이블에 넣으면 오히려
 * 틀린 번역이 생긴다.
 *
 * 실행 컨텍스트: gfp 가 허용하면 잠들 수 있다.
 *
 * 호출 체인: dma-iommu 의 dma_map_sg 구현 → [이 함수]
 *            → iommu_map_nosync (구간마다) → iommu_sync_map (한 번)
 */
ssize_t iommu_map_sg(struct iommu_domain *domain, unsigned long iova,
		     struct scatterlist *sg, unsigned int nents, int prot,
		     gfp_t gfp)
{
	size_t len = 0, mapped = 0;	/* [한국어] len: 지금까지 이어붙인 물리 연속 구간의 길이. mapped: IOVA 상에서 이미 매핑을 끝낸 총 길이 */
	phys_addr_t start;	/* [한국어] 이어붙이는 중인 연속 구간의 시작 물리 주소 */
	unsigned int i = 0;	/* [한국어] 세그먼트 인덱스 */
	int ret;	/* [한국어] 하위 매핑 호출의 결과 */

	while (i <= nents) {	/* [한국어] nents '이하'인 것에 주의 — 마지막 한 바퀴는 세그먼트를 읽기 위해서가 아니라 모아 둔 잔여 구간을 비워 내기 위한 것이다 */
		phys_addr_t s_phys = sg_phys(sg);	/* [한국어] 이번 세그먼트의 물리 시작 주소 (page + offset) */

		if (len && s_phys != start + len) {	/* [한국어] 모아 둔 구간이 있는데 이번 세그먼트가 물리적으로 이어지지 않는다 — 여기서 끊고 지금까지 모은 것을 한 번에 매핑한다. 마지막 반복(i == nents)에서는 sg 가 갱신되지 않아 이 조건이 반드시 참이 되고, 그래서 잔여분이 비워진다 */
			ret = iommu_map_nosync(domain, iova + mapped, start,	/* [한국어] IOVA 는 연속, 물리는 조각 — 흩어진 페이지들을 하나의 연속 IOVA 창으로 접는 것이 IOMMU 가 블록/네트워크 스택에 주는 가장 큰 이득이다 */
					len, prot, gfp);	/* [한국어] 이번에 모은 연속 구간 전체를 한 번의 호출로 */
			if (ret)	/* [한국어] 모아 둔 구간 하나를 매핑하다 실패 */
				goto out_err;	/* [한국어] 일부라도 실패하면 이미 만든 매핑까지 전부 되감는다 */

			mapped += len;	/* [한국어] IOVA 커서를 매핑한 만큼 전진 */
			len = 0;	/* [한국어] 다음 연속 구간 수집을 새로 시작 */
		}

		if (sg_dma_is_bus_address(sg))	/* [한국어] P2PDMA 로 이미 버스 주소가 정해진 세그먼트 — PCIe 스위치 안에서 장치끼리 직접 오가며 IOMMU 를 아예 거치지 않으므로 페이지 테이블에 넣어서는 안 된다 */
			goto next;	/* [한국어] 이 세그먼트는 건너뛴다 */

		if (len) {	/* [한국어] 직전까지의 구간과 물리적으로 이어지는 경우 */
			len += sg->length;	/* [한국어] 구간을 늘리기만 한다 — 매핑 호출 횟수와 PTE 개수를 함께 줄이는 병합 */
		} else {
			len = sg->length;	/* [한국어] 새 연속 구간의 길이 */
			start = s_phys;	/* [한국어] 새 연속 구간의 시작 물리 주소 */
		}

next:	/* [한국어] P2PDMA 세그먼트를 건너뛴 경로가 합류하는 지점 */
		if (++i < nents)	/* [한국어] 마지막 세그먼트를 지난 한 바퀴에서는 sg 를 전진시키지 않는다 — 위의 잔여분 비우기가 성립하는 이유가 이 조건이다 */
			sg = sg_next(sg);	/* [한국어] 다음 세그먼트로 (체인된 sgl 도 따라간다) */
	}

	ret = iommu_sync_map(domain, iova, mapped);	/* [한국어] 모든 매핑을 한 번에 하드웨어에 반영한다 — 세그먼트마다 sync 하지 않으려고 위에서 nosync 를 쓴 것이다 */
	if (ret)	/* [한국어] 마지막 동기화가 실패 */
		goto out_err;	/* [한국어] 동기화 실패도 전부 되감는다 */

	return mapped;	/* [한국어] 성공 시 매핑된 총 바이트(양수). 반환형이 ssize_t 라 호출자는 음수면 에러로 읽는다 */

out_err:	/* [한국어] 부분 성공을 남기지 않기 위한 되감기 경로 */
	/* undo mappings already done */
	iommu_unmap(domain, iova, mapped);	/* [한국어] 이미 만든 매핑을 지운다 — 실패한 dma_map_sg 는 아무 자원도 남기지 않아야 한다 */

	return ret;	/* [한국어] 음수 에러 코드 */
}
EXPORT_SYMBOL_GPL(iommu_map_sg);

/**
 * report_iommu_fault() - report about an IOMMU fault to the IOMMU framework
 * @domain: the iommu domain where the fault has happened
 * @dev: the device where the fault has happened
 * @iova: the faulting address
 * @flags: mmu fault flags (e.g. IOMMU_FAULT_READ/IOMMU_FAULT_WRITE/...)
 *
 * This function should be called by the low-level IOMMU implementations
 * whenever IOMMU faults happen, to allow high-level users, that are
 * interested in such events, to know about them.
 *
 * This event may be useful for several possible use cases:
 * - mere logging of the event
 * - dynamic TLB/PTE loading
 * - if restarting of the faulting device is required
 *
 * Returns 0 on success and an appropriate error code otherwise (if dynamic
 * PTE/TLB loading will one day be supported, implementations will be able
 * to tell whether it succeeded or not according to this return value).
 *
 * Specifically, -ENOSYS is returned if a fault handler isn't installed
 * (though fault handlers can also return -ENOSYS, in case they want to
 * elicit the default behavior of the IOMMU drivers).
 */
int report_iommu_fault(struct iommu_domain *domain, struct device *dev,
		       unsigned long iova, int flags)
{
	int ret = -ENOSYS;

	/*
	 * if upper layers showed interest and installed a fault handler,
	 * invoke it.
	 */
	if (domain->cookie_type == IOMMU_COOKIE_FAULT_HANDLER &&
	    domain->handler)
		ret = domain->handler(domain, dev, iova, flags,
						domain->handler_token);

	trace_io_page_fault(dev, iova, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(report_iommu_fault);

static int __init iommu_init(void)
{
	iommu_group_kset = kset_create_and_add("iommu_groups",
					       NULL, kernel_kobj);
	BUG_ON(!iommu_group_kset);

	iommu_debugfs_setup();

	return 0;
}
core_initcall(iommu_init);

int iommu_set_pgtable_quirks(struct iommu_domain *domain,
		unsigned long quirk)
{
	if (domain->type != IOMMU_DOMAIN_UNMANAGED)
		return -EINVAL;
	if (!domain->ops->set_pgtable_quirks)
		return -EINVAL;
	return domain->ops->set_pgtable_quirks(domain, quirk);
}
EXPORT_SYMBOL_GPL(iommu_set_pgtable_quirks);

/**
 * iommu_get_resv_regions - get reserved regions
 * @dev: device for which to get reserved regions
 * @list: reserved region list for device
 *
 * This returns a list of reserved IOVA regions specific to this device.
 * A domain user should not map IOVA in these ranges.
 */
void iommu_get_resv_regions(struct device *dev, struct list_head *list)
{
	const struct iommu_ops *ops = dev_iommu_ops(dev);

	if (ops->get_resv_regions)
		ops->get_resv_regions(dev, list);
}
EXPORT_SYMBOL_GPL(iommu_get_resv_regions);

/**
 * iommu_put_resv_regions - release reserved regions
 * @dev: device for which to free reserved regions
 * @list: reserved region list for device
 *
 * This releases a reserved region list acquired by iommu_get_resv_regions().
 */
void iommu_put_resv_regions(struct device *dev, struct list_head *list)
{
	struct iommu_resv_region *entry, *next;

	list_for_each_entry_safe(entry, next, list, list) {
		if (entry->free)
			entry->free(dev, entry);
		else
			kfree(entry);
	}
}
EXPORT_SYMBOL(iommu_put_resv_regions);

struct iommu_resv_region *iommu_alloc_resv_region(phys_addr_t start,
						  size_t length, int prot,
						  enum iommu_resv_type type,
						  gfp_t gfp)
{
	struct iommu_resv_region *region;

	region = kzalloc_obj(*region, gfp);
	if (!region)
		return NULL;

	INIT_LIST_HEAD(&region->list);
	region->start = start;
	region->length = length;
	region->prot = prot;
	region->type = type;
	return region;
}
EXPORT_SYMBOL_GPL(iommu_alloc_resv_region);

void iommu_set_default_passthrough(bool cmd_line)
{
	if (cmd_line)
		iommu_cmd_line |= IOMMU_CMD_LINE_DMA_API;
	iommu_def_domain_type = IOMMU_DOMAIN_IDENTITY;
}

void iommu_set_default_translated(bool cmd_line)
{
	if (cmd_line)
		iommu_cmd_line |= IOMMU_CMD_LINE_DMA_API;
	iommu_def_domain_type = IOMMU_DOMAIN_DMA;
}

bool iommu_default_passthrough(void)
{
	return iommu_def_domain_type == IOMMU_DOMAIN_IDENTITY;
}
EXPORT_SYMBOL_GPL(iommu_default_passthrough);

static const struct iommu_device *iommu_from_fwnode(const struct fwnode_handle *fwnode)
{
	const struct iommu_device *iommu, *ret = NULL;

	spin_lock(&iommu_device_lock);
	list_for_each_entry(iommu, &iommu_device_list, list)
		if (iommu->fwnode == fwnode) {
			ret = iommu;
			break;
		}
	spin_unlock(&iommu_device_lock);
	return ret;
}

const struct iommu_ops *iommu_ops_from_fwnode(const struct fwnode_handle *fwnode)
{
	const struct iommu_device *iommu = iommu_from_fwnode(fwnode);

	return iommu ? iommu->ops : NULL;
}

int iommu_fwspec_init(struct device *dev, struct fwnode_handle *iommu_fwnode)
{
	const struct iommu_device *iommu = iommu_from_fwnode(iommu_fwnode);
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

	if (!iommu)
		return driver_deferred_probe_check_state(dev);
	if (!dev->iommu && !READ_ONCE(iommu->ready))
		return -EPROBE_DEFER;

	if (fwspec)
		return iommu->ops == iommu_fwspec_ops(fwspec) ? 0 : -EINVAL;

	if (!dev_iommu_get(dev))
		return -ENOMEM;

	/* Preallocate for the overwhelmingly common case of 1 ID */
	fwspec = kzalloc_flex(*fwspec, ids, 1);
	if (!fwspec)
		return -ENOMEM;

	fwnode_handle_get(iommu_fwnode);
	fwspec->iommu_fwnode = iommu_fwnode;
	dev_iommu_fwspec_set(dev, fwspec);
	return 0;
}
EXPORT_SYMBOL_GPL(iommu_fwspec_init);

void iommu_fwspec_free(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

	if (fwspec) {
		fwnode_handle_put(fwspec->iommu_fwnode);
		kfree(fwspec);
		dev_iommu_fwspec_set(dev, NULL);
	}
}

int iommu_fwspec_add_ids(struct device *dev, const u32 *ids, int num_ids)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	int i, new_num;

	if (!fwspec)
		return -EINVAL;

	new_num = fwspec->num_ids + num_ids;
	if (new_num > 1) {
		fwspec = krealloc(fwspec, struct_size(fwspec, ids, new_num),
				  GFP_KERNEL);
		if (!fwspec)
			return -ENOMEM;

		dev_iommu_fwspec_set(dev, fwspec);
	}

	for (i = 0; i < num_ids; i++)
		fwspec->ids[fwspec->num_ids + i] = ids[i];

	fwspec->num_ids = new_num;
	return 0;
}
EXPORT_SYMBOL_GPL(iommu_fwspec_add_ids);

/**
 * iommu_setup_default_domain - Set the default_domain for the group
 * @group: Group to change
 * @target_type: Domain type to set as the default_domain
 *
 * Allocate a default domain and set it as the current domain on the group. If
 * the group already has a default domain it will be changed to the target_type.
 * When target_type is 0 the default domain is selected based on driver and
 * system preferences.
 */
static int iommu_setup_default_domain(struct iommu_group *group,
				      int target_type)
{
	struct iommu_domain *old_dom = group->default_domain;
	struct group_device *gdev;
	struct iommu_domain *dom;
	bool direct_failed;
	int req_type;
	int ret;

	lockdep_assert_held(&group->mutex);

	req_type = iommu_get_default_domain_type(group, target_type);
	if (req_type < 0)
		return -EINVAL;

	dom = iommu_group_alloc_default_domain(group, req_type);
	if (IS_ERR(dom))
		return PTR_ERR(dom);

	if (group->default_domain == dom)
		return 0;

	if (iommu_is_dma_domain(dom)) {
		ret = iommu_get_dma_cookie(dom);
		if (ret) {
			iommu_domain_free(dom);
			return ret;
		}
	}

	/*
	 * IOMMU_RESV_DIRECT and IOMMU_RESV_DIRECT_RELAXABLE regions must be
	 * mapped before their device is attached, in order to guarantee
	 * continuity with any FW activity
	 */
	direct_failed = false;
	for_each_group_device(group, gdev) {
		if (iommu_create_device_direct_mappings(dom, gdev->dev)) {
			direct_failed = true;
			dev_warn_once(
				gdev->dev->iommu->iommu_dev->dev,
				"IOMMU driver was not able to establish FW requested direct mapping.");
		}
	}

	/* We must set default_domain early for __iommu_device_set_domain */
	group->default_domain = dom;
	if (!group->domain) {
		/*
		 * Drivers are not allowed to fail the first domain attach.
		 * The only way to recover from this is to fail attaching the
		 * iommu driver and call ops->release_device. Put the domain
		 * in group->default_domain so it is freed after.
		 */
		ret = __iommu_group_set_domain_internal(
			group, dom, IOMMU_SET_DOMAIN_MUST_SUCCEED);
		if (WARN_ON(ret))
			goto out_free_old;
	} else {
		ret = __iommu_group_set_domain(group, dom);
		if (ret)
			goto err_restore_def_domain;
	}

	/*
	 * Drivers are supposed to allow mappings to be installed in a domain
	 * before device attachment, but some don't. Hack around this defect by
	 * trying again after attaching. If this happens it means the device
	 * will not continuously have the IOMMU_RESV_DIRECT map.
	 */
	if (direct_failed) {
		for_each_group_device(group, gdev) {
			ret = iommu_create_device_direct_mappings(dom, gdev->dev);
			if (ret)
				goto err_restore_domain;
		}
	}

out_free_old:
	if (old_dom)
		iommu_domain_free(old_dom);
	return ret;

err_restore_domain:
	if (old_dom)
		__iommu_group_set_domain_internal(
			group, old_dom, IOMMU_SET_DOMAIN_MUST_SUCCEED);
err_restore_def_domain:
	if (old_dom) {
		iommu_domain_free(dom);
		group->default_domain = old_dom;
	}
	return ret;
}

/*
 * Changing the default domain through sysfs requires the users to unbind the
 * drivers from the devices in the iommu group, except for a DMA -> DMA-FQ
 * transition. Return failure if this isn't met.
 *
 * We need to consider the race between this and the device release path.
 * group->mutex is used here to guarantee that the device release path
 * will not be entered at the same time.
 */
static ssize_t iommu_group_store_type(struct iommu_group *group,
				      const char *buf, size_t count)
{
	struct group_device *gdev;
	int ret, req_type;

	if (!capable(CAP_SYS_ADMIN) || !capable(CAP_SYS_RAWIO))
		return -EACCES;

	if (WARN_ON(!group) || !group->default_domain)
		return -EINVAL;

	if (sysfs_streq(buf, "identity"))
		req_type = IOMMU_DOMAIN_IDENTITY;
	else if (sysfs_streq(buf, "DMA"))
		req_type = IOMMU_DOMAIN_DMA;
	else if (sysfs_streq(buf, "DMA-FQ"))
		req_type = IOMMU_DOMAIN_DMA_FQ;
	else if (sysfs_streq(buf, "auto"))
		req_type = 0;
	else
		return -EINVAL;

	mutex_lock(&group->mutex);
	/* We can bring up a flush queue without tearing down the domain. */
	if (req_type == IOMMU_DOMAIN_DMA_FQ &&
	    group->default_domain->type == IOMMU_DOMAIN_DMA) {
		ret = iommu_dma_init_fq(group->default_domain);
		if (ret)
			goto out_unlock;

		group->default_domain->type = IOMMU_DOMAIN_DMA_FQ;
		ret = count;
		goto out_unlock;
	}

	/* Otherwise, ensure that device exists and no driver is bound. */
	if (list_empty(&group->devices) || group->owner_cnt) {
		ret = -EPERM;
		goto out_unlock;
	}

	ret = iommu_setup_default_domain(group, req_type);
	if (ret)
		goto out_unlock;

	/* Make sure dma_ops is appropriatley set */
	for_each_group_device(group, gdev)
		iommu_setup_dma_ops(gdev->dev, group->default_domain);

out_unlock:
	mutex_unlock(&group->mutex);
	return ret ?: count;
}

/**
 * iommu_device_use_default_domain() - Device driver wants to handle device
 *                                     DMA through the kernel DMA API.
 * @dev: The device.
 *
 * The device driver about to bind @dev wants to do DMA through the kernel
 * DMA API. Return 0 if it is allowed, otherwise an error.
 */
int iommu_device_use_default_domain(struct device *dev)
{
	/* Caller is the driver core during the pre-probe path */
	struct iommu_group *group = dev->iommu_group;
	int ret = 0;

	if (!group)
		return 0;

	mutex_lock(&group->mutex);
	/* We may race against bus_iommu_probe() finalising groups here */
	if (!group->default_domain) {
		ret = -EPROBE_DEFER;
		goto unlock_out;
	}
	if (group->owner_cnt) {
		if (group->domain != group->default_domain || group->owner ||
		    !xa_empty(&group->pasid_array)) {
			ret = -EBUSY;
			goto unlock_out;
		}
	}

	group->owner_cnt++;

unlock_out:
	mutex_unlock(&group->mutex);
	return ret;
}

/**
 * iommu_device_unuse_default_domain() - Device driver stops handling device
 *                                       DMA through the kernel DMA API.
 * @dev: The device.
 *
 * The device driver doesn't want to do DMA through kernel DMA API anymore.
 * It must be called after iommu_device_use_default_domain().
 */
void iommu_device_unuse_default_domain(struct device *dev)
{
	/* Caller is the driver core during the post-probe path */
	struct iommu_group *group = dev->iommu_group;

	if (!group)
		return;

	mutex_lock(&group->mutex);
	if (!WARN_ON(!group->owner_cnt || !xa_empty(&group->pasid_array)))
		group->owner_cnt--;

	mutex_unlock(&group->mutex);
}

static int __iommu_group_alloc_blocking_domain(struct iommu_group *group)
{
	struct device *dev = iommu_group_first_dev(group);
	const struct iommu_ops *ops = dev_iommu_ops(dev);
	struct iommu_domain *domain;

	if (group->blocking_domain)
		return 0;

	if (ops->blocked_domain) {
		group->blocking_domain = ops->blocked_domain;
		return 0;
	}

	/*
	 * For drivers that do not yet understand IOMMU_DOMAIN_BLOCKED create an
	 * empty PAGING domain instead.
	 */
	domain = iommu_paging_domain_alloc(dev);
	if (IS_ERR(domain))
		return PTR_ERR(domain);
	group->blocking_domain = domain;
	return 0;
}

static int __iommu_take_dma_ownership(struct iommu_group *group, void *owner)
{
	int ret;

	if ((group->domain && group->domain != group->default_domain) ||
	    !xa_empty(&group->pasid_array))
		return -EBUSY;

	ret = __iommu_group_alloc_blocking_domain(group);
	if (ret)
		return ret;
	ret = __iommu_group_set_domain(group, group->blocking_domain);
	if (ret)
		return ret;

	group->owner = owner;
	group->owner_cnt++;
	return 0;
}

/**
 * iommu_group_claim_dma_owner() - Set DMA ownership of a group
 * @group: The group.
 * @owner: Caller specified pointer. Used for exclusive ownership.
 *
 * This is to support backward compatibility for vfio which manages the dma
 * ownership in iommu_group level. New invocations on this interface should be
 * prohibited. Only a single owner may exist for a group.
 */
int iommu_group_claim_dma_owner(struct iommu_group *group, void *owner)
{
	int ret = 0;

	if (WARN_ON(!owner))
		return -EINVAL;

	mutex_lock(&group->mutex);
	if (group->owner_cnt) {
		ret = -EPERM;
		goto unlock_out;
	}

	ret = __iommu_take_dma_ownership(group, owner);
unlock_out:
	mutex_unlock(&group->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(iommu_group_claim_dma_owner);

/**
 * iommu_device_claim_dma_owner() - Set DMA ownership of a device
 * @dev: The device.
 * @owner: Caller specified pointer. Used for exclusive ownership.
 *
 * Claim the DMA ownership of a device. Multiple devices in the same group may
 * concurrently claim ownership if they present the same owner value. Returns 0
 * on success and error code on failure
 */
int iommu_device_claim_dma_owner(struct device *dev, void *owner)
{
	/* Caller must be a probed driver on dev */
	struct iommu_group *group = dev->iommu_group;
	int ret = 0;

	if (WARN_ON(!owner))
		return -EINVAL;

	if (!group)
		return -ENODEV;

	mutex_lock(&group->mutex);
	if (group->owner_cnt) {
		if (group->owner != owner) {
			ret = -EPERM;
			goto unlock_out;
		}
		group->owner_cnt++;
		goto unlock_out;
	}

	ret = __iommu_take_dma_ownership(group, owner);
unlock_out:
	mutex_unlock(&group->mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_device_claim_dma_owner);

static void __iommu_release_dma_ownership(struct iommu_group *group)
{
	if (WARN_ON(!group->owner_cnt || !group->owner ||
		    !xa_empty(&group->pasid_array)))
		return;

	group->owner_cnt = 0;
	group->owner = NULL;
	__iommu_group_set_domain_nofail(group, group->default_domain);
}

/**
 * iommu_group_release_dma_owner() - Release DMA ownership of a group
 * @group: The group
 *
 * Release the DMA ownership claimed by iommu_group_claim_dma_owner().
 */
void iommu_group_release_dma_owner(struct iommu_group *group)
{
	mutex_lock(&group->mutex);
	__iommu_release_dma_ownership(group);
	mutex_unlock(&group->mutex);
}
EXPORT_SYMBOL_GPL(iommu_group_release_dma_owner);

/**
 * iommu_device_release_dma_owner() - Release DMA ownership of a device
 * @dev: The device.
 *
 * Release the DMA ownership claimed by iommu_device_claim_dma_owner().
 */
void iommu_device_release_dma_owner(struct device *dev)
{
	/* Caller must be a probed driver on dev */
	struct iommu_group *group = dev->iommu_group;

	mutex_lock(&group->mutex);
	if (group->owner_cnt > 1)
		group->owner_cnt--;
	else
		__iommu_release_dma_ownership(group);
	mutex_unlock(&group->mutex);
}
EXPORT_SYMBOL_GPL(iommu_device_release_dma_owner);

/**
 * iommu_group_dma_owner_claimed() - Query group dma ownership status
 * @group: The group.
 *
 * This provides status query on a given group. It is racy and only for
 * non-binding status reporting.
 */
bool iommu_group_dma_owner_claimed(struct iommu_group *group)
{
	unsigned int user;

	mutex_lock(&group->mutex);
	user = group->owner_cnt;
	mutex_unlock(&group->mutex);

	return user;
}
EXPORT_SYMBOL_GPL(iommu_group_dma_owner_claimed);

static void iommu_remove_dev_pasid(struct device *dev, ioasid_t pasid,
				   struct iommu_domain *domain)
{
	const struct iommu_ops *ops = dev_iommu_ops(dev);
	struct iommu_domain *blocked_domain = ops->blocked_domain;

	WARN_ON(blocked_domain->ops->set_dev_pasid(blocked_domain,
						   dev, pasid, domain));
}

static int __iommu_set_group_pasid(struct iommu_domain *domain,
				   struct iommu_group *group, ioasid_t pasid,
				   struct iommu_domain *old)
{
	struct group_device *device, *last_gdev;
	int ret;

	for_each_group_device(group, device) {
		if (device->dev->iommu->max_pasids > 0) {
			ret = domain->ops->set_dev_pasid(domain, device->dev,
							 pasid, old);
			if (ret)
				goto err_revert;
		}
	}

	return 0;

err_revert:
	last_gdev = device;
	for_each_group_device(group, device) {
		if (device == last_gdev)
			break;
		if (device->dev->iommu->max_pasids > 0) {
			/*
			 * If no old domain, undo the succeeded devices/pasid.
			 * Otherwise, rollback the succeeded devices/pasid to
			 * the old domain. And it is a driver bug to fail
			 * attaching with a previously good domain.
			 */
			if (!old ||
			    WARN_ON(old->ops->set_dev_pasid(old, device->dev,
							    pasid, domain)))
				iommu_remove_dev_pasid(device->dev, pasid, domain);
		}
	}
	return ret;
}

static void __iommu_remove_group_pasid(struct iommu_group *group,
				       ioasid_t pasid,
				       struct iommu_domain *domain)
{
	struct group_device *device;

	for_each_group_device(group, device) {
		if (device->dev->iommu->max_pasids > 0)
			iommu_remove_dev_pasid(device->dev, pasid, domain);
	}
}

/*
 * iommu_attach_device_pasid() - Attach a domain to pasid of device
 * @domain: the iommu domain.
 * @dev: the attached device.
 * @pasid: the pasid of the device.
 * @handle: the attach handle.
 *
 * Caller should always provide a new handle to avoid race with the paths
 * that have lockless reference to handle if it intends to pass a valid handle.
 *
 * Return: 0 on success, or an error.
 */
int iommu_attach_device_pasid(struct iommu_domain *domain,
			      struct device *dev, ioasid_t pasid,
			      struct iommu_attach_handle *handle)
{
	/* Caller must be a probed driver on dev */
	struct iommu_group *group = dev->iommu_group;
	struct group_device *device;
	const struct iommu_ops *ops;
	void *entry;
	int ret;

	if (!group)
		return -ENODEV;

	ops = dev_iommu_ops(dev);

	if (!domain->ops->set_dev_pasid ||
	    !ops->blocked_domain ||
	    !ops->blocked_domain->ops->set_dev_pasid)
		return -EOPNOTSUPP;

	if (!domain_iommu_ops_compatible(ops, domain) ||
	    pasid == IOMMU_NO_PASID)
		return -EINVAL;

	mutex_lock(&group->mutex);

	/*
	 * This is a concurrent attach during a device reset. Reject it until
	 * pci_dev_reset_iommu_done() attaches the device to group->domain.
	 */
	if (group->resetting_domain) {
		ret = -EBUSY;
		goto out_unlock;
	}

	for_each_group_device(group, device) {
		/*
		 * Skip PASID validation for devices without PASID support
		 * (max_pasids = 0). These devices cannot issue transactions
		 * with PASID, so they don't affect group's PASID usage.
		 */
		if ((device->dev->iommu->max_pasids > 0) &&
		    (pasid >= device->dev->iommu->max_pasids)) {
			ret = -EINVAL;
			goto out_unlock;
		}
	}

	entry = iommu_make_pasid_array_entry(domain, handle);

	/*
	 * Entry present is a failure case. Use xa_insert() instead of
	 * xa_reserve().
	 */
	ret = xa_insert(&group->pasid_array, pasid, XA_ZERO_ENTRY, GFP_KERNEL);
	if (ret)
		goto out_unlock;

	ret = __iommu_set_group_pasid(domain, group, pasid, NULL);
	if (ret) {
		xa_release(&group->pasid_array, pasid);
		goto out_unlock;
	}

	/*
	 * The xa_insert() above reserved the memory, and the group->mutex is
	 * held, this cannot fail. The new domain cannot be visible until the
	 * operation succeeds as we cannot tolerate PRIs becoming concurrently
	 * queued and then failing attach.
	 */
	WARN_ON(xa_is_err(xa_store(&group->pasid_array,
				   pasid, entry, GFP_KERNEL)));

out_unlock:
	mutex_unlock(&group->mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_attach_device_pasid);

/**
 * iommu_replace_device_pasid - Replace the domain that a specific pasid
 *                              of the device is attached to
 * @domain: the new iommu domain
 * @dev: the attached device.
 * @pasid: the pasid of the device.
 * @handle: the attach handle.
 *
 * This API allows the pasid to switch domains. The @pasid should have been
 * attached. Otherwise, this fails. The pasid will keep the old configuration
 * if replacement failed.
 *
 * Caller should always provide a new handle to avoid race with the paths
 * that have lockless reference to handle if it intends to pass a valid handle.
 *
 * Return 0 on success, or an error.
 */
int iommu_replace_device_pasid(struct iommu_domain *domain,
			       struct device *dev, ioasid_t pasid,
			       struct iommu_attach_handle *handle)
{
	/* Caller must be a probed driver on dev */
	struct iommu_group *group = dev->iommu_group;
	struct iommu_attach_handle *entry;
	struct iommu_domain *curr_domain;
	void *curr;
	int ret;

	if (!group)
		return -ENODEV;

	if (!domain->ops->set_dev_pasid)
		return -EOPNOTSUPP;

	if (!domain_iommu_ops_compatible(dev_iommu_ops(dev), domain) ||
	    pasid == IOMMU_NO_PASID || !handle)
		return -EINVAL;

	mutex_lock(&group->mutex);

	/*
	 * This is a concurrent attach during a device reset. Reject it until
	 * pci_dev_reset_iommu_done() attaches the device to group->domain.
	 */
	if (group->resetting_domain) {
		ret = -EBUSY;
		goto out_unlock;
	}

	entry = iommu_make_pasid_array_entry(domain, handle);
	curr = xa_cmpxchg(&group->pasid_array, pasid, NULL,
			  XA_ZERO_ENTRY, GFP_KERNEL);
	if (xa_is_err(curr)) {
		ret = xa_err(curr);
		goto out_unlock;
	}

	/*
	 * No domain (with or without handle) attached, hence not
	 * a replace case.
	 */
	if (!curr) {
		xa_release(&group->pasid_array, pasid);
		ret = -EINVAL;
		goto out_unlock;
	}

	/*
	 * Reusing handle is problematic as there are paths that refers
	 * the handle without lock. To avoid race, reject the callers that
	 * attempt it.
	 */
	if (curr == entry) {
		WARN_ON(1);
		ret = -EINVAL;
		goto out_unlock;
	}

	curr_domain = pasid_array_entry_to_domain(curr);
	ret = 0;

	if (curr_domain != domain) {
		ret = __iommu_set_group_pasid(domain, group,
					      pasid, curr_domain);
		if (ret)
			goto out_unlock;
	}

	/*
	 * The above xa_cmpxchg() reserved the memory, and the
	 * group->mutex is held, this cannot fail.
	 */
	WARN_ON(xa_is_err(xa_store(&group->pasid_array,
				   pasid, entry, GFP_KERNEL)));

out_unlock:
	mutex_unlock(&group->mutex);
	return ret;
}
EXPORT_SYMBOL_NS_GPL(iommu_replace_device_pasid, "IOMMUFD_INTERNAL");

/*
 * iommu_detach_device_pasid() - Detach the domain from pasid of device
 * @domain: the iommu domain.
 * @dev: the attached device.
 * @pasid: the pasid of the device.
 *
 * The @domain must have been attached to @pasid of the @dev with
 * iommu_attach_device_pasid().
 */
void iommu_detach_device_pasid(struct iommu_domain *domain, struct device *dev,
			       ioasid_t pasid)
{
	/* Caller must be a probed driver on dev */
	struct iommu_group *group = dev->iommu_group;

	mutex_lock(&group->mutex);
	__iommu_remove_group_pasid(group, pasid, domain);
	xa_erase(&group->pasid_array, pasid);
	mutex_unlock(&group->mutex);
}
EXPORT_SYMBOL_GPL(iommu_detach_device_pasid);

ioasid_t iommu_alloc_global_pasid(struct device *dev)
{
	int ret;

	/* max_pasids == 0 means that the device does not support PASID */
	if (!dev->iommu->max_pasids)
		return IOMMU_PASID_INVALID;

	/*
	 * max_pasids is set up by vendor driver based on number of PASID bits
	 * supported but the IDA allocation is inclusive.
	 */
	ret = ida_alloc_range(&iommu_global_pasid_ida, IOMMU_FIRST_GLOBAL_PASID,
			      dev->iommu->max_pasids - 1, GFP_KERNEL);
	return ret < 0 ? IOMMU_PASID_INVALID : ret;
}
EXPORT_SYMBOL_GPL(iommu_alloc_global_pasid);

void iommu_free_global_pasid(ioasid_t pasid)
{
	if (WARN_ON(pasid == IOMMU_PASID_INVALID))
		return;

	ida_free(&iommu_global_pasid_ida, pasid);
}
EXPORT_SYMBOL_GPL(iommu_free_global_pasid);

/**
 * iommu_attach_handle_get - Return the attach handle
 * @group: the iommu group that domain was attached to
 * @pasid: the pasid within the group
 * @type: matched domain type, 0 for any match
 *
 * Return handle or ERR_PTR(-ENOENT) on none, ERR_PTR(-EBUSY) on mismatch.
 *
 * Return the attach handle to the caller. The life cycle of an iommu attach
 * handle is from the time when the domain is attached to the time when the
 * domain is detached. Callers are required to synchronize the call of
 * iommu_attach_handle_get() with domain attachment and detachment. The attach
 * handle can only be used during its life cycle.
 */
struct iommu_attach_handle *
iommu_attach_handle_get(struct iommu_group *group, ioasid_t pasid, unsigned int type)
{
	struct iommu_attach_handle *handle;
	void *entry;

	xa_lock(&group->pasid_array);
	entry = xa_load(&group->pasid_array, pasid);
	if (!entry || xa_pointer_tag(entry) != IOMMU_PASID_ARRAY_HANDLE) {
		handle = ERR_PTR(-ENOENT);
	} else {
		handle = xa_untag_pointer(entry);
		if (type && handle->domain->type != type)
			handle = ERR_PTR(-EBUSY);
	}
	xa_unlock(&group->pasid_array);

	return handle;
}
EXPORT_SYMBOL_NS_GPL(iommu_attach_handle_get, "IOMMUFD_INTERNAL");

/**
 * iommu_attach_group_handle - Attach an IOMMU domain to an IOMMU group
 * @domain: IOMMU domain to attach
 * @group: IOMMU group that will be attached
 * @handle: attach handle
 *
 * Returns 0 on success and error code on failure.
 *
 * This is a variant of iommu_attach_group(). It allows the caller to provide
 * an attach handle and use it when the domain is attached. This is currently
 * used by IOMMUFD to deliver the I/O page faults.
 *
 * Caller should always provide a new handle to avoid race with the paths
 * that have lockless reference to handle.
 */
int iommu_attach_group_handle(struct iommu_domain *domain,
			      struct iommu_group *group,
			      struct iommu_attach_handle *handle)
{
	void *entry;
	int ret;

	if (!handle)
		return -EINVAL;

	mutex_lock(&group->mutex);
	entry = iommu_make_pasid_array_entry(domain, handle);
	ret = xa_insert(&group->pasid_array,
			IOMMU_NO_PASID, XA_ZERO_ENTRY, GFP_KERNEL);
	if (ret)
		goto out_unlock;

	ret = __iommu_attach_group(domain, group);
	if (ret) {
		xa_release(&group->pasid_array, IOMMU_NO_PASID);
		goto out_unlock;
	}

	/*
	 * The xa_insert() above reserved the memory, and the group->mutex is
	 * held, this cannot fail. The new domain cannot be visible until the
	 * operation succeeds as we cannot tolerate PRIs becoming concurrently
	 * queued and then failing attach.
	 */
	WARN_ON(xa_is_err(xa_store(&group->pasid_array,
				   IOMMU_NO_PASID, entry, GFP_KERNEL)));

out_unlock:
	mutex_unlock(&group->mutex);
	return ret;
}
EXPORT_SYMBOL_NS_GPL(iommu_attach_group_handle, "IOMMUFD_INTERNAL");

/**
 * iommu_detach_group_handle - Detach an IOMMU domain from an IOMMU group
 * @domain: IOMMU domain to attach
 * @group: IOMMU group that will be attached
 *
 * Detach the specified IOMMU domain from the specified IOMMU group.
 * It must be used in conjunction with iommu_attach_group_handle().
 */
void iommu_detach_group_handle(struct iommu_domain *domain,
			       struct iommu_group *group)
{
	mutex_lock(&group->mutex);
	__iommu_group_set_core_domain(group);
	xa_erase(&group->pasid_array, IOMMU_NO_PASID);
	mutex_unlock(&group->mutex);
}
EXPORT_SYMBOL_NS_GPL(iommu_detach_group_handle, "IOMMUFD_INTERNAL");

/**
 * iommu_replace_group_handle - replace the domain that a group is attached to
 * @group: IOMMU group that will be attached to the new domain
 * @new_domain: new IOMMU domain to replace with
 * @handle: attach handle
 *
 * This API allows the group to switch domains without being forced to go to
 * the blocking domain in-between. It allows the caller to provide an attach
 * handle for the new domain and use it when the domain is attached.
 *
 * If the currently attached domain is a core domain (e.g. a default_domain),
 * it will act just like the iommu_attach_group_handle().
 *
 * Caller should always provide a new handle to avoid race with the paths
 * that have lockless reference to handle.
 */
int iommu_replace_group_handle(struct iommu_group *group,
			       struct iommu_domain *new_domain,
			       struct iommu_attach_handle *handle)
{
	void *curr, *entry;
	int ret;

	if (!new_domain || !handle)
		return -EINVAL;

	mutex_lock(&group->mutex);
	entry = iommu_make_pasid_array_entry(new_domain, handle);
	ret = xa_reserve(&group->pasid_array, IOMMU_NO_PASID, GFP_KERNEL);
	if (ret)
		goto err_unlock;

	ret = __iommu_group_set_domain(group, new_domain);
	if (ret)
		goto err_release;

	curr = xa_store(&group->pasid_array, IOMMU_NO_PASID, entry, GFP_KERNEL);
	WARN_ON(xa_is_err(curr));

	mutex_unlock(&group->mutex);

	return 0;
err_release:
	xa_release(&group->pasid_array, IOMMU_NO_PASID);
err_unlock:
	mutex_unlock(&group->mutex);
	return ret;
}
EXPORT_SYMBOL_NS_GPL(iommu_replace_group_handle, "IOMMUFD_INTERNAL");

/**
 * pci_dev_reset_iommu_prepare() - Block IOMMU to prepare for a PCI device reset
 * @pdev: PCI device that is going to enter a reset routine
 *
 * The PCIe r6.0, sec 10.3.1 IMPLEMENTATION NOTE recommends to disable and block
 * ATS before initiating a reset. This means that a PCIe device during the reset
 * routine wants to block any IOMMU activity: translation and ATS invalidation.
 *
 * This function attaches the device's RID/PASID(s) the group->blocking_domain,
 * setting the group->resetting_domain. This allows the IOMMU driver pausing any
 * IOMMU activity while leaving the group->domain pointer intact. Later when the
 * reset is finished, pci_dev_reset_iommu_done() can restore everything.
 *
 * Caller must use pci_dev_reset_iommu_prepare() with pci_dev_reset_iommu_done()
 * before/after the core-level reset routine, to unset the resetting_domain.
 *
 * Return: 0 on success or negative error code if the preparation failed.
 *
 * These two functions are designed to be used by PCI reset functions that would
 * not invoke any racy iommu_release_device(), since PCI sysfs node gets removed
 * before it notifies with a BUS_NOTIFY_REMOVED_DEVICE. When using them in other
 * case, callers must ensure there will be no racy iommu_release_device() call,
 * which otherwise would UAF the dev->iommu_group pointer.
 */
int pci_dev_reset_iommu_prepare(struct pci_dev *pdev)
{
	struct iommu_group *group = pdev->dev.iommu_group;
	unsigned long pasid;
	void *entry;
	int ret;

	if (!pci_ats_supported(pdev) || !dev_has_iommu(&pdev->dev))
		return 0;

	guard(mutex)(&group->mutex);

	/* Re-entry is not allowed */
	if (WARN_ON(group->resetting_domain))
		return -EBUSY;

	ret = __iommu_group_alloc_blocking_domain(group);
	if (ret)
		return ret;

	/* Stage RID domain at blocking_domain while retaining group->domain */
	if (group->domain != group->blocking_domain) {
		ret = __iommu_attach_device(group->blocking_domain, &pdev->dev,
					    group->domain);
		if (ret)
			return ret;
	}

	/*
	 * Stage PASID domains at blocking_domain while retaining pasid_array.
	 *
	 * The pasid_array is mostly fenced by group->mutex, except one reader
	 * in iommu_attach_handle_get(), so it's safe to read without xa_lock.
	 */
	xa_for_each_start(&group->pasid_array, pasid, entry, 1)
		iommu_remove_dev_pasid(&pdev->dev, pasid,
				       pasid_array_entry_to_domain(entry));

	group->resetting_domain = group->blocking_domain;
	return ret;
}
EXPORT_SYMBOL_GPL(pci_dev_reset_iommu_prepare);

/**
 * pci_dev_reset_iommu_done() - Restore IOMMU after a PCI device reset is done
 * @pdev: PCI device that has finished a reset routine
 *
 * After a PCIe device finishes a reset routine, it wants to restore its IOMMU
 * IOMMU activity, including new translation as well as cache invalidation, by
 * re-attaching all RID/PASID of the device's back to the domains retained in
 * the core-level structure.
 *
 * Caller must pair it with a successful pci_dev_reset_iommu_prepare().
 *
 * Note that, although unlikely, there is a risk that re-attaching domains might
 * fail due to some unexpected happening like OOM.
 */
void pci_dev_reset_iommu_done(struct pci_dev *pdev)
{
	struct iommu_group *group = pdev->dev.iommu_group;
	unsigned long pasid;
	void *entry;

	if (!pci_ats_supported(pdev) || !dev_has_iommu(&pdev->dev))
		return;

	guard(mutex)(&group->mutex);

	/* pci_dev_reset_iommu_prepare() was bypassed for the device */
	if (!group->resetting_domain)
		return;

	/* pci_dev_reset_iommu_prepare() was not successfully called */
	if (WARN_ON(!group->blocking_domain))
		return;

	/* Re-attach RID domain back to group->domain */
	if (group->domain != group->blocking_domain) {
		WARN_ON(__iommu_attach_device(group->domain, &pdev->dev,
					      group->blocking_domain));
	}

	/*
	 * Re-attach PASID domains back to the domains retained in pasid_array.
	 *
	 * The pasid_array is mostly fenced by group->mutex, except one reader
	 * in iommu_attach_handle_get(), so it's safe to read without xa_lock.
	 */
	xa_for_each_start(&group->pasid_array, pasid, entry, 1)
		WARN_ON(__iommu_set_group_pasid(
			pasid_array_entry_to_domain(entry), group, pasid,
			group->blocking_domain));

	group->resetting_domain = NULL;
}
EXPORT_SYMBOL_GPL(pci_dev_reset_iommu_done);

#if IS_ENABLED(CONFIG_IRQ_MSI_IOMMU)
/**
 * iommu_dma_prepare_msi() - Map the MSI page in the IOMMU domain
 * @desc: MSI descriptor, will store the MSI page
 * @msi_addr: MSI target address to be mapped
 *
 * The implementation of sw_msi() should take msi_addr and map it to
 * an IOVA in the domain and call msi_desc_set_iommu_msi_iova() with the
 * mapping information.
 *
 * Return: 0 on success or negative error code if the mapping failed.
 */
int iommu_dma_prepare_msi(struct msi_desc *desc, phys_addr_t msi_addr)
{
	struct device *dev = msi_desc_to_dev(desc);
	struct iommu_group *group = dev->iommu_group;
	int ret = 0;

	if (!group)
		return 0;

	mutex_lock(&group->mutex);
	/* An IDENTITY domain must pass through */
	if (group->domain && group->domain->type != IOMMU_DOMAIN_IDENTITY) {
		switch (group->domain->cookie_type) {
		case IOMMU_COOKIE_DMA_MSI:
		case IOMMU_COOKIE_DMA_IOVA:
			ret = iommu_dma_sw_msi(group->domain, desc, msi_addr);
			break;
		case IOMMU_COOKIE_IOMMUFD:
			ret = iommufd_sw_msi(group->domain, desc, msi_addr);
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
	}
	mutex_unlock(&group->mutex);
	return ret;
}
#endif /* CONFIG_IRQ_MSI_IOMMU */
