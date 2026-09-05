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

#if IS_ENABLED(CONFIG_IOMMUFD_TEST)
void iommu_device_unregister_bus(struct iommu_device *iommu,
				 const struct bus_type *bus,
				 struct notifier_block *nb)
{
	bus_unregister_notifier(bus, nb);
	fwnode_remove_software_node(iommu->fwnode);
	iommu_device_unregister(iommu);
}
EXPORT_SYMBOL_GPL(iommu_device_unregister_bus);

/*
 * Register an iommu driver against a single bus. This is only used by iommufd
 * selftest to create a mock iommu driver. The caller must provide
 * some memory to hold a notifier_block.
 */
int iommu_device_register_bus(struct iommu_device *iommu,
			      const struct iommu_ops *ops,
			      const struct bus_type *bus,
			      struct notifier_block *nb)
{
	int err;

	iommu->ops = ops;
	nb->notifier_call = iommu_bus_notifier;
	err = bus_register_notifier(bus, nb);
	if (err)
		return err;

	iommu->fwnode = fwnode_create_software_node(NULL, NULL);
	if (IS_ERR(iommu->fwnode)) {
		bus_unregister_notifier(bus, nb);
		return PTR_ERR(iommu->fwnode);
	}

	spin_lock(&iommu_device_lock);
	list_add_tail(&iommu->list, &iommu_device_list);
	spin_unlock(&iommu_device_lock);

	err = bus_iommu_probe(bus);
	if (err) {
		iommu_device_unregister_bus(iommu, bus, nb);
		return err;
	}
	WRITE_ONCE(iommu->ready, true);
	return 0;
}
EXPORT_SYMBOL_GPL(iommu_device_register_bus);

int iommu_mock_device_add(struct device *dev, struct iommu_device *iommu)
{
	int rc;

	mutex_lock(&iommu_probe_device_lock);
	rc = iommu_fwspec_init(dev, iommu->fwnode);
	mutex_unlock(&iommu_probe_device_lock);

	if (rc)
		return rc;

	rc = device_add(dev);
	if (rc)
		iommu_fwspec_free(dev);
	return rc;
}
EXPORT_SYMBOL_GPL(iommu_mock_device_add);
#endif

static struct dev_iommu *dev_iommu_get(struct device *dev)
{
	struct dev_iommu *param = dev->iommu;

	lockdep_assert_held(&iommu_probe_device_lock);

	if (param)
		return param;

	param = kzalloc_obj(*param);
	if (!param)
		return NULL;

	mutex_init(&param->lock);
	dev->iommu = param;
	return param;
}

void dev_iommu_free(struct device *dev)
{
	struct dev_iommu *param = dev->iommu;

	dev->iommu = NULL;
	if (param->fwspec) {
		fwnode_handle_put(param->fwspec->iommu_fwnode);
		kfree(param->fwspec);
	}
	kfree(param);
}

/*
 * Internal equivalent of device_iommu_mapped() for when we care that a device
 * actually has API ops, and don't want false positives from VFIO-only groups.
 */
static bool dev_has_iommu(struct device *dev)
{
	return dev->iommu && dev->iommu->iommu_dev;
}

static u32 dev_iommu_get_max_pasids(struct device *dev)
{
	u32 max_pasids = 0, bits = 0;
	int ret;

	if (dev_is_pci(dev)) {
		ret = pci_max_pasids(to_pci_dev(dev));
		if (ret > 0)
			max_pasids = ret;
	} else {
		ret = device_property_read_u32(dev, "pasid-num-bits", &bits);
		if (!ret)
			max_pasids = 1UL << bits;
	}

	return min_t(u32, max_pasids, dev->iommu->iommu_dev->max_pasids);
}

void dev_iommu_priv_set(struct device *dev, void *priv)
{
	/* FSL_PAMU does something weird */
	if (!IS_ENABLED(CONFIG_FSL_PAMU))
		lockdep_assert_held(&iommu_probe_device_lock);
	dev->iommu->priv = priv;
}
EXPORT_SYMBOL_GPL(dev_iommu_priv_set);

/*
 * Init the dev->iommu and dev->iommu_group in the struct device and get the
 * driver probed
 */
static int iommu_init_device(struct device *dev)
{
	const struct iommu_ops *ops;
	struct iommu_device *iommu_dev;
	struct iommu_group *group;
	int ret;

	if (!dev_iommu_get(dev))
		return -ENOMEM;
	/*
	 * For FDT-based systems and ACPI IORT/VIOT, the common firmware parsing
	 * is buried in the bus dma_configure path. Properly unpicking that is
	 * still a big job, so for now just invoke the whole thing. The device
	 * already having a driver bound means dma_configure has already run and
	 * found no IOMMU to wait for, so there's no point calling it again.
	 */
	if (!dev->iommu->fwspec && !dev->driver && dev->bus->dma_configure) {
		mutex_unlock(&iommu_probe_device_lock);
		dev->bus->dma_configure(dev);
		mutex_lock(&iommu_probe_device_lock);
		/* If another instance finished the job for us, skip it */
		if (!dev->iommu || dev->iommu_group)
			return -ENODEV;
	}
	/*
	 * At this point, relevant devices either now have a fwspec which will
	 * match ops registered with a non-NULL fwnode, or we can reasonably
	 * assume that only one of Intel, AMD, s390, PAMU or legacy SMMUv2 can
	 * be present, and that any of their registered instances has suitable
	 * ops for probing, and thus cheekily co-opt the same mechanism.
	 */
	ops = iommu_fwspec_ops(dev->iommu->fwspec);
	if (!ops) {
		ret = -ENODEV;
		goto err_free;
	}

	if (!try_module_get(ops->owner)) {
		ret = -EINVAL;
		goto err_free;
	}

	iommu_dev = ops->probe_device(dev);
	if (IS_ERR(iommu_dev)) {
		ret = PTR_ERR(iommu_dev);
		goto err_module_put;
	}
	dev->iommu->iommu_dev = iommu_dev;

	ret = iommu_device_link(iommu_dev, dev);
	if (ret)
		goto err_release;

	group = ops->device_group(dev);
	if (WARN_ON_ONCE(group == NULL))
		group = ERR_PTR(-EINVAL);
	if (IS_ERR(group)) {
		ret = PTR_ERR(group);
		goto err_unlink;
	}
	dev->iommu_group = group;

	dev->iommu->max_pasids = dev_iommu_get_max_pasids(dev);
	if (ops->is_attach_deferred)
		dev->iommu->attach_deferred = ops->is_attach_deferred(dev);
	return 0;

err_unlink:
	iommu_device_unlink(iommu_dev, dev);
err_release:
	if (ops->release_device)
		ops->release_device(dev);
err_module_put:
	module_put(ops->owner);
err_free:
	dev->iommu->iommu_dev = NULL;
	dev_iommu_free(dev);
	return ret;
}

static void iommu_deinit_device(struct device *dev)
{
	struct iommu_group *group = dev->iommu_group;
	const struct iommu_ops *ops = dev_iommu_ops(dev);

	lockdep_assert_held(&group->mutex);

	iommu_device_unlink(dev->iommu->iommu_dev, dev);

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
	if (!dev->iommu->attach_deferred && ops->release_domain) {
		struct iommu_domain *release_domain = ops->release_domain;

		/*
		 * If the device requires direct mappings then it should not
		 * be parked on a BLOCKED domain during release as that would
		 * break the direct mappings.
		 */
		if (dev->iommu->require_direct && ops->identity_domain &&
		    release_domain == ops->blocked_domain)
			release_domain = ops->identity_domain;

		release_domain->ops->attach_dev(release_domain, dev,
						group->domain);
	}

	if (ops->release_device)
		ops->release_device(dev);

	/*
	 * If this is the last driver to use the group then we must free the
	 * domains before we do the module_put().
	 */
	if (list_empty(&group->devices)) {
		if (group->default_domain) {
			iommu_domain_free(group->default_domain);
			group->default_domain = NULL;
		}
		if (group->blocking_domain) {
			iommu_domain_free(group->blocking_domain);
			group->blocking_domain = NULL;
		}
		group->domain = NULL;
	}

	/* Caller must put iommu_group */
	dev->iommu_group = NULL;
	module_put(ops->owner);
	dev_iommu_free(dev);
#ifdef CONFIG_IOMMU_DMA
	dev->dma_iommu = false;
#endif
}

static struct iommu_domain *pasid_array_entry_to_domain(void *entry)
{
	if (xa_pointer_tag(entry) == IOMMU_PASID_ARRAY_DOMAIN)
		return xa_untag_pointer(entry);
	return ((struct iommu_attach_handle *)xa_untag_pointer(entry))->domain;
}

DEFINE_MUTEX(iommu_probe_device_lock);

static int __iommu_probe_device(struct device *dev, struct list_head *group_list)
{
	struct iommu_group *group;
	struct group_device *gdev;
	int ret;

	/*
	 * Serialise to avoid races between IOMMU drivers registering in
	 * parallel and/or the "replay" calls from ACPI/OF code via client
	 * driver probe. Once the latter have been cleaned up we should
	 * probably be able to use device_lock() here to minimise the scope,
	 * but for now enforcing a simple global ordering is fine.
	 */
	lockdep_assert_held(&iommu_probe_device_lock);

	/* Device is probed already if in a group */
	if (dev->iommu_group)
		return 0;

	ret = iommu_init_device(dev);
	if (ret)
		return ret;
	/*
	 * And if we do now see any replay calls, they would indicate someone
	 * misusing the dma_configure path outside bus code.
	 */
	if (dev->driver)
		dev_WARN(dev, "late IOMMU probe at driver bind, something fishy here!\n");

	group = dev->iommu_group;
	gdev = iommu_group_alloc_device(group, dev);
	mutex_lock(&group->mutex);
	if (IS_ERR(gdev)) {
		ret = PTR_ERR(gdev);
		goto err_put_group;
	}

	/*
	 * The gdev must be in the list before calling
	 * iommu_setup_default_domain()
	 */
	list_add_tail(&gdev->list, &group->devices);
	WARN_ON(group->default_domain && !group->domain);
	if (group->default_domain)
		iommu_create_device_direct_mappings(group->default_domain, dev);
	if (group->domain) {
		ret = __iommu_device_set_domain(group, dev, group->domain, NULL,
						0);
		if (ret)
			goto err_remove_gdev;
	} else if (!group->default_domain && !group_list) {
		ret = iommu_setup_default_domain(group, 0);
		if (ret)
			goto err_remove_gdev;
	} else if (!group->default_domain) {
		/*
		 * With a group_list argument we defer the default_domain setup
		 * to the caller by providing a de-duplicated list of groups
		 * that need further setup.
		 */
		if (list_empty(&group->entry))
			list_add_tail(&group->entry, group_list);
	}

	if (group->default_domain)
		iommu_setup_dma_ops(dev, group->default_domain);

	mutex_unlock(&group->mutex);

	return 0;

err_remove_gdev:
	list_del(&gdev->list);
	__iommu_group_free_device(group, gdev);
err_put_group:
	iommu_deinit_device(dev);
	mutex_unlock(&group->mutex);
	iommu_group_put(group);

	return ret;
}

int iommu_probe_device(struct device *dev)
{
	const struct iommu_ops *ops;
	int ret;

	mutex_lock(&iommu_probe_device_lock);
	ret = __iommu_probe_device(dev, NULL);
	mutex_unlock(&iommu_probe_device_lock);
	if (ret)
		return ret;

	ops = dev_iommu_ops(dev);
	if (ops->probe_finalize)
		ops->probe_finalize(dev);

	return 0;
}

static void __iommu_group_free_device(struct iommu_group *group,
				      struct group_device *grp_dev)
{
	struct device *dev = grp_dev->dev;

	sysfs_remove_link(group->devices_kobj, grp_dev->name);
	sysfs_remove_link(&dev->kobj, "iommu_group");

	trace_remove_device_from_group(group->id, dev);

	/*
	 * If the group has become empty then ownership must have been
	 * released, and the current domain must be set back to NULL or
	 * the default domain.
	 */
	if (list_empty(&group->devices))
		WARN_ON(group->owner_cnt ||
			group->domain != group->default_domain);

	kfree(grp_dev->name);
	kfree(grp_dev);
}

/* Remove the iommu_group from the struct device. */
static void __iommu_group_remove_device(struct device *dev)
{
	struct iommu_group *group = dev->iommu_group;
	struct group_device *device;

	mutex_lock(&group->mutex);
	for_each_group_device(group, device) {
		if (device->dev != dev)
			continue;

		list_del(&device->list);
		__iommu_group_free_device(group, device);
		if (dev_has_iommu(dev))
			iommu_deinit_device(dev);
		else
			dev->iommu_group = NULL;
		break;
	}
	mutex_unlock(&group->mutex);

	/*
	 * Pairs with the get in iommu_init_device() or
	 * iommu_group_add_device()
	 */
	iommu_group_put(group);
}

static void iommu_release_device(struct device *dev)
{
	struct iommu_group *group = dev->iommu_group;

	if (group)
		__iommu_group_remove_device(dev);

	/* Free any fwspec if no iommu_driver was ever attached */
	if (dev->iommu)
		dev_iommu_free(dev);
}

static int __init iommu_set_def_domain_type(char *str)
{
	bool pt;
	int ret;

	ret = kstrtobool(str, &pt);
	if (ret)
		return ret;

	if (pt)
		iommu_set_default_passthrough(true);
	else
		iommu_set_default_translated(true);

	return 0;
}
early_param("iommu.passthrough", iommu_set_def_domain_type);

static int __init iommu_dma_setup(char *str)
{
	int ret = kstrtobool(str, &iommu_dma_strict);

	if (!ret)
		iommu_cmd_line |= IOMMU_CMD_LINE_STRICT;
	return ret;
}
early_param("iommu.strict", iommu_dma_setup);

void iommu_set_dma_strict(void)
{
	iommu_dma_strict = true;
	if (iommu_def_domain_type == IOMMU_DOMAIN_DMA_FQ)
		iommu_def_domain_type = IOMMU_DOMAIN_DMA;
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

	if (attr->show)
		ret = attr->show(group, buf);	/* [한국어] 실제 구현은 각 속성이 가지고 있다 */
	return ret;
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
	struct iommu_group_attribute *attr = to_iommu_group_attr(__attr);
	struct iommu_group *group = to_iommu_group(kobj);
	ssize_t ret = -EIO;	/* [한국어] 읽기 전용 속성에 쓰려 했다는 뜻 */

	if (attr->store)
		ret = attr->store(group, buf, count);
	return ret;
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

static ssize_t iommu_group_show_name(struct iommu_group *group, char *buf)
{
	return sysfs_emit(buf, "%s\n", group->name);
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
static int iommu_insert_resv_region(struct iommu_resv_region *new,
				    struct list_head *regions)
{
	struct iommu_resv_region *iter, *tmp, *nr, *top;
	LIST_HEAD(stack);

	nr = iommu_alloc_resv_region(new->start, new->length,
				     new->prot, new->type, GFP_KERNEL);
	if (!nr)
		return -ENOMEM;

	/* First add the new element based on start address sorting */
	list_for_each_entry(iter, regions, list) {
		if (nr->start < iter->start ||
		    (nr->start == iter->start && nr->type <= iter->type))
			break;
	}
	list_add_tail(&nr->list, &iter->list);

	/* Merge overlapping segments of type nr->type in @regions, if any */
	list_for_each_entry_safe(iter, tmp, regions, list) {
		phys_addr_t top_end, iter_end = iter->start + iter->length - 1;

		/* no merge needed on elements of different types than @new */
		if (iter->type != new->type) {
			list_move_tail(&iter->list, &stack);
			continue;
		}

		/* look for the last stack element of same type as @iter */
		list_for_each_entry_reverse(top, &stack, list)
			if (top->type == iter->type)
				goto check_overlap;

		list_move_tail(&iter->list, &stack);
		continue;

check_overlap:
		top_end = top->start + top->length - 1;

		if (iter->start > top_end + 1) {
			list_move_tail(&iter->list, &stack);
		} else {
			top->length = max(top_end, iter_end) - top->start + 1;
			list_del(&iter->list);
			kfree(iter);
		}
	}
	list_splice(&stack, regions);
	return 0;
}

static int
iommu_insert_device_resv_regions(struct list_head *dev_resv_regions,
				 struct list_head *group_resv_regions)
{
	struct iommu_resv_region *entry;
	int ret = 0;

	list_for_each_entry(entry, dev_resv_regions, list) {
		ret = iommu_insert_resv_region(entry, group_resv_regions);
		if (ret)
			break;
	}
	return ret;
}

int iommu_get_group_resv_regions(struct iommu_group *group,
				 struct list_head *head)
{
	struct group_device *device;
	int ret = 0;

	mutex_lock(&group->mutex);
	for_each_group_device(group, device) {
		struct list_head dev_resv_regions;

		/*
		 * Non-API groups still expose reserved_regions in sysfs,
		 * so filter out calls that get here that way.
		 */
		if (!dev_has_iommu(device->dev))
			break;

		INIT_LIST_HEAD(&dev_resv_regions);
		iommu_get_resv_regions(device->dev, &dev_resv_regions);
		ret = iommu_insert_device_resv_regions(&dev_resv_regions, head);
		iommu_put_resv_regions(device->dev, &dev_resv_regions);
		if (ret)
			break;
	}
	mutex_unlock(&group->mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_get_group_resv_regions);

static ssize_t iommu_group_show_resv_regions(struct iommu_group *group,
					     char *buf)
{
	struct iommu_resv_region *region, *next;
	struct list_head group_resv_regions;
	int offset = 0;

	INIT_LIST_HEAD(&group_resv_regions);
	iommu_get_group_resv_regions(group, &group_resv_regions);

	list_for_each_entry_safe(region, next, &group_resv_regions, list) {
		offset += sysfs_emit_at(buf, offset, "0x%016llx 0x%016llx %s\n",
					(long long)region->start,
					(long long)(region->start +
						    region->length - 1),
					iommu_group_resv_type_string[region->type]);
		kfree(region);
	}

	return offset;
}

static ssize_t iommu_group_show_type(struct iommu_group *group,
				     char *buf)
{
	char *type = "unknown";

	mutex_lock(&group->mutex);
	if (group->default_domain) {
		switch (group->default_domain->type) {
		case IOMMU_DOMAIN_BLOCKED:
			type = "blocked";
			break;
		case IOMMU_DOMAIN_IDENTITY:
			type = "identity";
			break;
		case IOMMU_DOMAIN_UNMANAGED:
			type = "unmanaged";
			break;
		case IOMMU_DOMAIN_DMA:
			type = "DMA";
			break;
		case IOMMU_DOMAIN_DMA_FQ:
			type = "DMA-FQ";
			break;
		}
	}
	mutex_unlock(&group->mutex);

	return sysfs_emit(buf, "%s\n", type);
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
struct iommu_group *generic_single_device_group(struct device *dev)
{
	struct iommu_device *iommu = dev->iommu->iommu_dev;

	if (!iommu->singleton_group) {
		struct iommu_group *group;

		group = iommu_group_alloc();
		if (IS_ERR(group))
			return group;
		iommu->singleton_group = group;
	}
	return iommu_group_ref_get(iommu->singleton_group);
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

static struct iommu_domain *
__iommu_group_alloc_default_domain(struct iommu_group *group, int req_type)
{
	struct device *dev = iommu_group_first_dev(group);
	struct iommu_domain *dom;

	if (group->default_domain && group->default_domain->type == req_type)
		return group->default_domain;

	/*
	 * When allocating the DMA API domain assume that the driver is going to
	 * use PASID and make sure the RID's domain is PASID compatible.
	 */
	if (req_type & __IOMMU_DOMAIN_PAGING) {
		dom = __iommu_paging_domain_alloc_flags(dev, req_type,
			   dev->iommu->max_pasids ? IOMMU_HWPT_ALLOC_PASID : 0);

		/*
		 * If driver does not support PASID feature then
		 * try to allocate non-PASID domain
		 */
		if (PTR_ERR(dom) == -EOPNOTSUPP)
			dom = __iommu_paging_domain_alloc_flags(dev, req_type, 0);

		return dom;
	}

	if (req_type == IOMMU_DOMAIN_IDENTITY)
		return __iommu_alloc_identity_domain(dev);

	return ERR_PTR(-EINVAL);
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

static int __iommu_attach_device(struct iommu_domain *domain,
				 struct device *dev, struct iommu_domain *old)
{
	int ret;

	if (unlikely(domain->ops->attach_dev == NULL))
		return -ENODEV;

	ret = domain->ops->attach_dev(domain, dev, old);
	if (ret)
		return ret;
	dev->iommu->attach_deferred = 0;
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
int iommu_attach_device(struct iommu_domain *domain, struct device *dev)
{
	/* Caller must be a probed driver on dev */
	struct iommu_group *group = dev->iommu_group;
	int ret;

	if (!group)
		return -ENODEV;

	/*
	 * Lock the group to make sure the device-count doesn't
	 * change while we are attaching
	 */
	mutex_lock(&group->mutex);
	ret = -EINVAL;
	if (list_count_nodes(&group->devices) != 1)
		goto out_unlock;

	ret = __iommu_attach_group(domain, group);

out_unlock:
	mutex_unlock(&group->mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_attach_device);

int iommu_deferred_attach(struct device *dev, struct iommu_domain *domain)
{
	/*
	 * This is called on the dma mapping fast path so avoid locking. This is
	 * racy, but we have an expectation that the driver will setup its DMAs
	 * inside probe while being single threaded to avoid racing.
	 */
	if (!dev->iommu || !dev->iommu->attach_deferred)
		return 0;

	guard(mutex)(&dev->iommu_group->mutex);

	/*
	 * This is a concurrent attach during a device reset. Reject it until
	 * pci_dev_reset_iommu_done() attaches the device to group->domain.
	 *
	 * Note that this might fail the iommu_dma_map(). But there's nothing
	 * more we can do here.
	 */
	if (dev->iommu_group->resetting_domain)
		return -EBUSY;
	return __iommu_attach_device(domain, dev, NULL);
}

void iommu_detach_device(struct iommu_domain *domain, struct device *dev)
{
	/* Caller must be a probed driver on dev */
	struct iommu_group *group = dev->iommu_group;

	if (!group)
		return;

	mutex_lock(&group->mutex);
	if (WARN_ON(domain != group->domain) ||
	    WARN_ON(list_count_nodes(&group->devices) != 1))
		goto out_unlock;
	__iommu_group_set_core_domain(group);

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
struct iommu_domain *iommu_get_domain_for_dev(struct device *dev)
{
	/* Caller must be a probed driver on dev */
	struct iommu_group *group = dev->iommu_group;

	if (!group)
		return NULL;

	lockdep_assert_not_held(&group->mutex);

	return group->domain;
}
EXPORT_SYMBOL_GPL(iommu_get_domain_for_dev);

/**
 * iommu_driver_get_domain_for_dev() - Return the driver-level domain pointer
 * @dev: Device to query
 *
 * This function can be called by an iommu driver that wants to get the physical
 * domain within an iommu callback function where group->mutex is held.
 */
struct iommu_domain *iommu_driver_get_domain_for_dev(struct device *dev)
{
	struct iommu_group *group = dev->iommu_group;

	lockdep_assert_held(&group->mutex);

	/*
	 * Driver handles the low-level __iommu_attach_device(), including the
	 * one invoked by pci_dev_reset_iommu_done() re-attaching the device to
	 * the cached group->domain. In this case, the driver must get the old
	 * domain from group->resetting_domain rather than group->domain. This
	 * prevents it from re-attaching the device from group->domain (old) to
	 * group->domain (new).
	 */
	if (group->resetting_domain)
		return group->resetting_domain;

	return group->domain;
}
EXPORT_SYMBOL_GPL(iommu_driver_get_domain_for_dev);

/*
 * For IOMMU_DOMAIN_DMA implementations which already provide their own
 * guarantees that the group and its default domain are valid and correct.
 */
struct iommu_domain *iommu_get_dma_domain(struct device *dev)
{
	return dev->iommu_group->default_domain;
}

static void *iommu_make_pasid_array_entry(struct iommu_domain *domain,
					  struct iommu_attach_handle *handle)
{
	if (handle) {
		handle->domain = domain;
		return xa_tag_pointer(handle, IOMMU_PASID_ARRAY_HANDLE);
	}

	return xa_tag_pointer(domain, IOMMU_PASID_ARRAY_DOMAIN);
}

static bool domain_iommu_ops_compatible(const struct iommu_ops *ops,
					struct iommu_domain *domain)
{
	if (domain->owner == ops)
		return true;

	/* For static domains, owner isn't set. */
	if (domain == ops->blocked_domain || domain == ops->identity_domain)
		return true;

	return false;
}

static int __iommu_attach_group(struct iommu_domain *domain,
				struct iommu_group *group)
{
	struct device *dev;

	if (group->domain && group->domain != group->default_domain &&
	    group->domain != group->blocking_domain)
		return -EBUSY;

	dev = iommu_group_first_dev(group);
	if (!dev_has_iommu(dev) ||
	    !domain_iommu_ops_compatible(dev_iommu_ops(dev), domain))
		return -EINVAL;

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
int iommu_attach_group(struct iommu_domain *domain, struct iommu_group *group)
{
	int ret;

	mutex_lock(&group->mutex);
	ret = __iommu_attach_group(domain, group);
	mutex_unlock(&group->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(iommu_attach_group);

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
	if (dev->iommu->require_direct &&
	    (new_domain->type == IOMMU_DOMAIN_BLOCKED ||
	     new_domain == group->blocking_domain)) {
		dev_warn(dev,
			 "Firmware has requested this device have a 1:1 IOMMU mapping, rejecting configuring the device without a 1:1 mapping. Contact your platform vendor.\n");
		return -EINVAL;
	}

	if (dev->iommu->attach_deferred) {
		if (new_domain == group->default_domain)
			return 0;
		dev->iommu->attach_deferred = 0;
	}

	ret = __iommu_attach_device(new_domain, dev, old_domain);
	if (ret) {
		/*
		 * If we have a blocking domain then try to attach that in hopes
		 * of avoiding a UAF. Modern drivers should implement blocking
		 * domains as global statics that cannot fail.
		 */
		if ((flags & IOMMU_SET_DOMAIN_MUST_SUCCEED) &&
		    group->blocking_domain &&
		    group->blocking_domain != new_domain)
			__iommu_attach_device(group->blocking_domain, dev,
					      old_domain);
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
static int __iommu_group_set_domain_internal(struct iommu_group *group,
					     struct iommu_domain *new_domain,
					     unsigned int flags)
{
	struct group_device *last_gdev;
	struct group_device *gdev;
	int result;
	int ret;

	lockdep_assert_held(&group->mutex);

	if (group->domain == new_domain)
		return 0;

	if (WARN_ON(!new_domain))
		return -EINVAL;

	/*
	 * This is a concurrent attach during a device reset. Reject it until
	 * pci_dev_reset_iommu_done() attaches the device to group->domain.
	 */
	if (group->resetting_domain)
		return -EBUSY;

	/*
	 * Changing the domain is done by calling attach_dev() on the new
	 * domain. This switch does not have to be atomic and DMA can be
	 * discarded during the transition. DMA must only be able to access
	 * either new_domain or group->domain, never something else.
	 */
	result = 0;
	for_each_group_device(group, gdev) {
		ret = __iommu_device_set_domain(group, gdev->dev, new_domain,
						group->domain, flags);
		if (ret) {
			result = ret;
			/*
			 * Keep trying the other devices in the group. If a
			 * driver fails attach to an otherwise good domain, and
			 * does not support blocking domains, it should at least
			 * drop its reference on the current domain so we don't
			 * UAF.
			 */
			if (flags & IOMMU_SET_DOMAIN_MUST_SUCCEED)
				continue;
			goto err_revert;
		}
	}
	group->domain = new_domain;
	return result;

err_revert:
	/*
	 * This is called in error unwind paths. A well behaved driver should
	 * always allow us to attach to a domain that was already attached.
	 */
	last_gdev = gdev;
	for_each_group_device(group, gdev) {
		/* No need to revert the last gdev that failed to set domain */
		if (gdev == last_gdev)
			break;
		/*
		 * A NULL domain can happen only for first probe, in which case
		 * we leave group->domain as NULL and let release clean
		 * everything up.
		 */
		if (group->domain)
			WARN_ON(__iommu_device_set_domain(
				group, gdev->dev, group->domain, new_domain,
				IOMMU_SET_DOMAIN_MUST_SUCCEED));
	}
	return ret;
}

void iommu_detach_group(struct iommu_domain *domain, struct iommu_group *group)
{
	mutex_lock(&group->mutex);
	__iommu_group_set_core_domain(group);
	mutex_unlock(&group->mutex);
}
EXPORT_SYMBOL_GPL(iommu_detach_group);

phys_addr_t iommu_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	if (domain->type == IOMMU_DOMAIN_IDENTITY)
		return iova;

	if (domain->type == IOMMU_DOMAIN_BLOCKED)
		return 0;

	return domain->ops->iova_to_phys(domain, iova);
}
EXPORT_SYMBOL_GPL(iommu_iova_to_phys);

static size_t iommu_pgsize(struct iommu_domain *domain, unsigned long iova,
			   phys_addr_t paddr, size_t size, size_t *count)
{
	unsigned int pgsize_idx, pgsize_idx_next;
	unsigned long pgsizes;
	size_t offset, pgsize, pgsize_next;
	size_t offset_end;
	unsigned long addr_merge = paddr | iova;

	/* Page sizes supported by the hardware and small enough for @size */
	pgsizes = domain->pgsize_bitmap & GENMASK(__fls(size), 0);

	/* Constrain the page sizes further based on the maximum alignment */
	if (likely(addr_merge))
		pgsizes &= GENMASK(__ffs(addr_merge), 0);

	/* Make sure we have at least one suitable page size */
	BUG_ON(!pgsizes);

	/* Pick the biggest page size remaining */
	pgsize_idx = __fls(pgsizes);
	pgsize = BIT(pgsize_idx);
	if (!count)
		return pgsize;

	/* Find the next biggest support page size, if it exists */
	pgsizes = domain->pgsize_bitmap & ~GENMASK(pgsize_idx, 0);
	if (!pgsizes)
		goto out_set_count;

	pgsize_idx_next = __ffs(pgsizes);
	pgsize_next = BIT(pgsize_idx_next);

	/*
	 * There's no point trying a bigger page size unless the virtual
	 * and physical addresses are similarly offset within the larger page.
	 */
	if ((iova ^ paddr) & (pgsize_next - 1))
		goto out_set_count;

	/* Calculate the offset to the next page size alignment boundary */
	offset = pgsize_next - (addr_merge & (pgsize_next - 1));

	/*
	 * If size is big enough to accommodate the larger page, reduce
	 * the number of smaller pages.
	 */
	if (!check_add_overflow(offset, pgsize_next, &offset_end) &&
	    offset_end <= size)
		size = offset;

out_set_count:
	*count = size >> pgsize_idx;
	return pgsize;
}

static int __iommu_map_domain_pgtbl(struct iommu_domain *domain,
				    unsigned long iova, phys_addr_t paddr,
				    size_t size, int prot, gfp_t gfp)
{
	const struct iommu_domain_ops *ops = domain->ops;
	unsigned long orig_iova = iova;
	unsigned int min_pagesz;
	size_t orig_size = size;
	int ret = 0;

	might_sleep_if(gfpflags_allow_blocking(gfp));

	if (unlikely(!(domain->type & __IOMMU_DOMAIN_PAGING)))
		return -EINVAL;

	if (WARN_ON(!ops->map_pages || domain->pgsize_bitmap == 0UL))
		return -ENODEV;

	/* Discourage passing strange GFP flags */
	if (WARN_ON_ONCE(gfp & (__GFP_COMP | __GFP_DMA | __GFP_DMA32 |
				__GFP_HIGHMEM)))
		return -EINVAL;

	/* find out the minimum page size supported */
	min_pagesz = 1 << __ffs(domain->pgsize_bitmap);

	/*
	 * both the virtual address and the physical one, as well as
	 * the size of the mapping, must be aligned (at least) to the
	 * size of the smallest page supported by the hardware
	 */
	if (!IS_ALIGNED(iova | paddr | size, min_pagesz)) {
		pr_err("unaligned: iova 0x%lx pa %pa size 0x%zx min_pagesz 0x%x\n",
		       iova, &paddr, size, min_pagesz);
		return -EINVAL;
	}

	pr_debug("map: iova 0x%lx pa %pa size 0x%zx\n", iova, &paddr, size);

	while (size) {
		size_t pgsize, count, mapped = 0;

		pgsize = iommu_pgsize(domain, iova, paddr, size, &count);

		pr_debug("mapping: iova 0x%lx pa %pa pgsize 0x%zx count %zu\n",
			 iova, &paddr, pgsize, count);
		ret = ops->map_pages(domain, iova, paddr, pgsize, count, prot,
				     gfp, &mapped);
		/*
		 * Some pages may have been mapped, even if an error occurred,
		 * so we should account for those so they can be unmapped.
		 */
		size -= mapped;

		if (ret)
			break;

		iova += mapped;
		paddr += mapped;
	}

	/* unroll mapping in case something went wrong */
	if (ret) {
		iommu_unmap(domain, orig_iova, orig_size - size);
		return ret;
	}
	return 0;
}

int iommu_sync_map(struct iommu_domain *domain, unsigned long iova, size_t size)
{
	const struct iommu_domain_ops *ops = domain->ops;

	if (!ops->iotlb_sync_map)
		return 0;
	return ops->iotlb_sync_map(domain, iova, size);
}

int iommu_map_nosync(struct iommu_domain *domain, unsigned long iova,
		phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{
	struct pt_iommu *pt = iommupt_from_domain(domain);
	int ret;

	if (pt) {
		size_t mapped = 0;

		ret = pt->ops->map_range(pt, iova, paddr, size, prot, gfp,
					 &mapped);
		if (ret) {
			iommu_unmap(domain, iova, mapped);
			return ret;
		}
		return 0;
	}
	ret = __iommu_map_domain_pgtbl(domain, iova, paddr, size, prot, gfp);
	if (!ret)
		return ret;

	trace_map(iova, paddr, size);
	iommu_debug_map(domain, paddr, size);
	return 0;
}

int iommu_map(struct iommu_domain *domain, unsigned long iova,
	      phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{
	int ret;

	ret = iommu_map_nosync(domain, iova, paddr, size, prot, gfp);
	if (ret)
		return ret;

	ret = iommu_sync_map(domain, iova, size);
	if (ret)
		iommu_unmap(domain, iova, size);

	return ret;
}
EXPORT_SYMBOL_GPL(iommu_map);

static size_t
__iommu_unmap_domain_pgtbl(struct iommu_domain *domain, unsigned long iova,
			   size_t size, struct iommu_iotlb_gather *iotlb_gather)
{
	const struct iommu_domain_ops *ops = domain->ops;
	size_t unmapped_page, unmapped = 0;
	unsigned int min_pagesz;

	if (unlikely(!(domain->type & __IOMMU_DOMAIN_PAGING)))
		return 0;

	if (WARN_ON(!ops->unmap_pages || domain->pgsize_bitmap == 0UL))
		return 0;

	/* find out the minimum page size supported */
	min_pagesz = 1 << __ffs(domain->pgsize_bitmap);

	/*
	 * The virtual address, as well as the size of the mapping, must be
	 * aligned (at least) to the size of the smallest page supported
	 * by the hardware
	 */
	if (!IS_ALIGNED(iova | size, min_pagesz)) {
		pr_err("unaligned: iova 0x%lx size 0x%zx min_pagesz 0x%x\n",
		       iova, size, min_pagesz);
		return 0;
	}

	pr_debug("unmap this: iova 0x%lx size 0x%zx\n", iova, size);

	iommu_debug_unmap_begin(domain, iova, size);

	/*
	 * Keep iterating until we either unmap 'size' bytes (or more)
	 * or we hit an area that isn't mapped.
	 */
	while (unmapped < size) {
		size_t pgsize, count;

		pgsize = iommu_pgsize(domain, iova, iova, size - unmapped, &count);
		unmapped_page = ops->unmap_pages(domain, iova, pgsize, count, iotlb_gather);
		if (!unmapped_page)
			break;

		pr_debug("unmapped: iova 0x%lx size 0x%zx\n",
			 iova, unmapped_page);
		/*
		 * If the driver itself isn't using the gather, make sure
		 * it looks non-empty so iotlb_sync will still be called.
		 */
		if (iotlb_gather->start >= iotlb_gather->end)
			iommu_iotlb_gather_add_range(iotlb_gather, iova, size);

		iova += unmapped_page;
		unmapped += unmapped_page;
	}

	return unmapped;
}

static size_t __iommu_unmap(struct iommu_domain *domain, unsigned long iova,
			    size_t size,
			    struct iommu_iotlb_gather *iotlb_gather)
{
	struct pt_iommu *pt = iommupt_from_domain(domain);
	size_t unmapped;

	if (pt)
		unmapped = pt->ops->unmap_range(pt, iova, size, iotlb_gather);
	else
		unmapped = __iommu_unmap_domain_pgtbl(domain, iova, size,
						      iotlb_gather);
	trace_unmap(iova, size, unmapped);
	iommu_debug_unmap_end(domain, iova, size, unmapped);
	return unmapped;
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
size_t iommu_unmap(struct iommu_domain *domain,
		   unsigned long iova, size_t size)
{
	struct iommu_iotlb_gather iotlb_gather;
	size_t ret;

	iommu_iotlb_gather_init(&iotlb_gather);
	ret = __iommu_unmap(domain, iova, size, &iotlb_gather);
	iommu_iotlb_sync(domain, &iotlb_gather);

	return ret;
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
size_t iommu_unmap_fast(struct iommu_domain *domain,
			unsigned long iova, size_t size,
			struct iommu_iotlb_gather *iotlb_gather)
{
	return __iommu_unmap(domain, iova, size, iotlb_gather);
}
EXPORT_SYMBOL_GPL(iommu_unmap_fast);

ssize_t iommu_map_sg(struct iommu_domain *domain, unsigned long iova,
		     struct scatterlist *sg, unsigned int nents, int prot,
		     gfp_t gfp)
{
	size_t len = 0, mapped = 0;
	phys_addr_t start;
	unsigned int i = 0;
	int ret;

	while (i <= nents) {
		phys_addr_t s_phys = sg_phys(sg);

		if (len && s_phys != start + len) {
			ret = iommu_map_nosync(domain, iova + mapped, start,
					len, prot, gfp);
			if (ret)
				goto out_err;

			mapped += len;
			len = 0;
		}

		if (sg_dma_is_bus_address(sg))
			goto next;

		if (len) {
			len += sg->length;
		} else {
			len = sg->length;
			start = s_phys;
		}

next:
		if (++i < nents)
			sg = sg_next(sg);
	}

	ret = iommu_sync_map(domain, iova, mapped);
	if (ret)
		goto out_err;

	return mapped;

out_err:
	/* undo mappings already done */
	iommu_unmap(domain, iova, mapped);

	return ret;
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
