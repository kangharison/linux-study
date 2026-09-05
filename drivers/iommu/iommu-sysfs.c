// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU sysfs class support
 *
 * Copyright (C) 2014 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 */

/*
 * [한국어 설명] IOMMU 장치의 sysfs 표현 (drivers/iommu/iommu-sysfs.c)
 *
 * === 파일의 역할 ===
 * IOMMU 하드웨어 하나를 sysfs 에 드러내고, 그 아래에 어떤 장치들이 붙어 있는지
 * 링크로 보여 주는 계층이다. /sys/class/iommu/ 아래에 IOMMU 인스턴스가 나타나고,
 * 각 인스턴스의 devices/ 디렉터리에 그 IOMMU 가 담당하는 장치들의 심볼릭 링크가
 * 걸린다. 반대 방향 링크도 만들어져, 장치 쪽에서 iommu 를 따라가면 자기를 맡은
 * IOMMU 로 간다.
 *
 * 코드 자체는 짧지만 실무에서 쓸모가 크다. 여러 IOMMU 가 있는 시스템에서 "이
 * 장치는 어느 IOMMU 아래인가"를 확인하는 유일한 방법이며, 인텔 VT-d 처럼 소켓마다
 * DMAR 유닛이 있는 서버에서는 그 정보가 성능 분석의 출발점이 된다.
 *
 * /sys/kernel/iommu_groups/ 와 혼동하지 말 것. 그쪽은 격리 그룹(iommu.c 가 만든다)
 * 이고, 이쪽은 하드웨어 인스턴스다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름: 벤더 드라이버 초기화
 *         → [이 파일] iommu_device_sysfs_add : /sys/class/iommu/<이름> 생성
 *         → iommu_device_register (iommu.c)
 *       장치 프로브:
 *         → iommu.c iommu_init_device
 *           → [이 파일] iommu_device_link : 양방향 심볼릭 링크
 *
 * === 타 모듈과의 연결 ===
 * - iommu.c: 장치가 IOMMU 에 붙고 떨어질 때 link/unlink 를 부른다.
 * - 벤더 드라이버: 자기 고유 속성(예: 인텔의 캐시 정보, AMD 의 IVHD 정보)을
 *   attribute_group 으로 넘겨, IOMMU 종류마다 다른 이름 공간을 갖게 한다.
 * - 드라이버 코어: struct class 와 kobject 기반 sysfs 기계.
 *
 * === 주요 함수/구조체 요약 ===
 * - iommu_class              : /sys/class/iommu 클래스 정의.
 * - iommu_device_sysfs_add() : IOMMU 인스턴스를 sysfs 에 등록한다.
 * - iommu_device_sysfs_remove(): 그 짝.
 * - iommu_device_link()      : IOMMU ↔ 장치 양방향 링크를 만든다.
 * - iommu_device_unlink()    : 그 짝.
 */
#include <linux/device.h>	/* [한국어] struct device 와 클래스 등록 */
#include <linux/iommu.h>	/* [한국어] struct iommu_device 정의 */
#include <linux/init.h>	/* [한국어] postcore_initcall */
#include <linux/slab.h>	/* [한국어] IOMMU 장치 구조체 할당 */

/*
 * We provide a common class "devices" group which initially has no attributes.
 * As devices are added to the IOMMU, we'll add links to the group.
 */
static struct attribute *devices_attr[] = {	/* [한국어] 속성이 없는 빈 그룹이다. 파일이 아니라 '디렉터리'를 만드는 것이 목적이며, 그 안에 나중에 장치 링크들이 걸린다 (위 영어 주석) */
	NULL,	/* [한국어] 비어 있음을 나타내는 종료자 */
};

/* [한국어] 속성 없이 디렉터리만 만드는 그룹.
 * sysfs 에서 디렉터리를 얻는 표준적인 방법이며, 그 안에 iommu_device_link 가
 * 장치 링크를 하나씩 걸어 넣는다. */
static const struct attribute_group devices_attr_group = {
	.name = "devices",	/* [한국어] 이 그룹이 만들 디렉터리 이름 */
	.attrs = devices_attr,	/* [한국어] 빈 속성 목록 */
};

/* [한국어] 이 클래스의 모든 장치가 공통으로 갖는 그룹 목록.
 * 벤더별 그룹은 iommu_device_sysfs_add 의 인자로 따로 받아 dev->groups 에 넣으므로,
 * IOMMU 종류마다 다른 속성을 노출할 수 있다. */
static const struct attribute_group *dev_groups[] = {
	&devices_attr_group,	/* [한국어] 모든 IOMMU 장치가 공통으로 갖는 그룹 */
	NULL,	/* [한국어] 목록 종료자. 벤더별 그룹은 sysfs_add 인자로 따로 받는다 */
};

/*
 * [한국어]
 * release_device - sysfs 객체의 마지막 참조가 사라질 때 구조체를 반납한다
 *
 * @dev: 해제할 장치
 *
 * device_unregister 가 곧바로 해제하지 않는다는 점이 요점이다. sysfs 객체는
 * 참조 계수로 관리되어, 사용자 공간이 그 디렉터리를 열어 두고 있으면 나중에야
 * 이 콜백이 불린다.
 *
 * 실행 컨텍스트: 마지막 put_device 가 도는 문맥.
 *
 * 호출 체인: kobject 참조 해제 → class.dev_release == [이 함수]
 */
static void release_device(struct device *dev)
{
	kfree(dev);	/* [한국어] 마지막 참조가 사라질 때 구조체를 반납한다. sysfs 객체는 참조 계수로 관리되므로 device_unregister 직후가 아닐 수 있다 */
}

/* [한국어] /sys/class/iommu 클래스 정의.
 * 이 클래스가 있어서 시스템의 모든 IOMMU 인스턴스를 한 곳에서 열거할 수 있다 —
 * 인텔 서버의 소켓별 DMAR 유닛이나 ARM SoC 의 여러 SMMU 가 여기 나란히 나타난다. */
static const struct class iommu_class = {
	.name = "iommu",	/* [한국어] /sys/class/iommu 로 나타난다 */
	.dev_release = release_device,	/* [한국어] 해제 콜백 */
	.dev_groups = dev_groups,	/* [한국어] 이 클래스의 모든 장치가 갖는 그룹들 */
};

/*
 * [한국어]
 * iommu_dev_init - IOMMU sysfs 클래스를 등록한다
 *
 * @return: 0 성공, 음수면 클래스 등록 실패
 *
 * postcore_initcall 인 것이 중요하다. IOMMU 드라이버들이 자기를 등록할 때 이미
 * 클래스가 존재해야 하므로, 그보다 이른 단계에서 돌아야 한다.
 *
 * 실행 컨텍스트: 부팅 초기 initcall.
 *
 * 호출 체인: postcore_initcall → [이 함수]
 */
static int __init iommu_dev_init(void)
{
	return class_register(&iommu_class);	/* [한국어] 클래스를 등록해 /sys/class/iommu 디렉터리를 만든다 */
}
postcore_initcall(iommu_dev_init);	/* [한국어] IOMMU 드라이버보다 먼저 돌아야 한다 — 드라이버가 자기를 등록할 때 이미 클래스가 있어야 하기 때문 */

/*
 * Init the struct device for the IOMMU. IOMMU specific attributes can
 * be provided as an attribute group, allowing a unique namespace per
 * IOMMU type.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * iommu_device_sysfs_add - IOMMU 인스턴스를 /sys/class/iommu 아래에 등록한다
 *
 * @iommu:  등록할 IOMMU 인스턴스
 * @parent: 실제 하드웨어 장치 (sysfs 트리에서의 부모)
 * @groups: 벤더별 속성 그룹. NULL 도 가능하다.
 * @fmt:    이름 포맷 문자열 (가변 인자)
 * @return: 0 성공, 음수 실패
 *
 * 벤더 드라이버가 iommu_device_register 보다 먼저 부른다. 순서가 그런 이유는,
 * 등록되는 순간 장치들이 붙기 시작하고 그때 iommu_device_link 가 이 sysfs
 * 디렉터리를 필요로 하기 때문이다.
 *
 * 이름을 가변 인자로 받는 것은 인스턴스마다 다른 이름을 붙이기 위해서다 —
 * 인텔이면 dmar0/dmar1, ARM 이면 하드웨어 주소를 포함한 이름을 쓴다.
 *
 * 실행 컨텍스트: 드라이버 초기화. 프로세스 문맥.
 *
 * 호출 체인: 벤더 드라이버 프로브 → [이 함수]
 */
int iommu_device_sysfs_add(struct iommu_device *iommu,
			   struct device *parent,
			   const struct attribute_group **groups,
			   const char *fmt, ...)
{
	va_list vargs;	/* [한국어] 이름을 포맷 문자열로 받는다 */
	int ret;	/* [한국어] 각 단계의 결과 */

	iommu->dev = kzalloc_obj(*iommu->dev);	/* [한국어] sysfs 에 나타날 device 구조체 */
	if (!iommu->dev)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 등록 불가 */

	device_initialize(iommu->dev);	/* [한국어] 참조 계수와 kobject 를 세운다. 이 뒤로는 put_device 로 해제해야 한다 */

	iommu->dev->class = &iommu_class;	/* [한국어] /sys/class/iommu 아래에 놓이게 한다 */
	iommu->dev->parent = parent;	/* [한국어] 실제 하드웨어 장치(PCI 장치나 플랫폼 장치)를 부모로 — sysfs 트리에서 위치를 잡아 준다 */
	iommu->dev->groups = groups;	/* [한국어] 벤더별 속성 그룹. IOMMU 종류마다 다른 이름 공간을 갖게 한다 (위 영어 주석) */

	va_start(vargs, fmt);	/* [한국어] 가변 인자 시작 */
	ret = kobject_set_name_vargs(&iommu->dev->kobj, fmt, vargs);	/* [한국어] 이름을 정한다 — 인텔이면 dmar0, ARM 이면 smmu3.0x... 같은 형태 */
	va_end(vargs);	/* [한국어] 가변 인자 끝 */
	if (ret)	/* [한국어] 이름 설정 실패 */
		goto error;	/* [한국어] 되감기 */

	ret = device_add(iommu->dev);	/* [한국어] sysfs 에 실제로 나타난다 */
	if (ret)	/* [한국어] 등록 실패 */
		goto error;	/* [한국어] 되감기 */

	dev_set_drvdata(iommu->dev, iommu);	/* [한국어] sysfs 속성 콜백이 이 포인터로 IOMMU 인스턴스를 되찾는다 */

	return 0;	/* [한국어] 등록 완료 */

error:	/* [한국어] 공통 되감기 */
	put_device(iommu->dev);	/* [한국어] 참조를 놓는다 — 마지막이면 release_device 가 kfree 한다 */
	return ret;	/* [한국어] 실패 이유 */
}
EXPORT_SYMBOL_GPL(iommu_device_sysfs_add);	/* [한국어] 벤더 드라이버가 iommu_device_register 전에 부른다 */

/*
 * [한국어]
 * iommu_device_sysfs_remove - IOMMU 인스턴스를 sysfs 에서 제거한다
 *
 * @iommu: 제거할 인스턴스
 *
 * drvdata 를 먼저 끊는 순서가 안전장치다. 해제가 진행되는 동안에도 사용자 공간이
 * 속성 파일을 읽을 수 있는데, 그때 콜백이 NULL 을 보고 물러나게 한다.
 *
 * 실행 컨텍스트: 드라이버 제거. 프로세스 문맥.
 *
 * 호출 체인: 벤더 드라이버 remove → [이 함수]
 */
void iommu_device_sysfs_remove(struct iommu_device *iommu)
{
	dev_set_drvdata(iommu->dev, NULL);	/* [한국어] 먼저 끊는다 — 해제 도중 속성 콜백이 돌아도 NULL 을 보게 된다 */
	device_unregister(iommu->dev);	/* [한국어] sysfs 에서 제거하고 참조를 놓는다 */
	iommu->dev = NULL;	/* [한국어] 두 번 해제되지 않도록 */
}
EXPORT_SYMBOL_GPL(iommu_device_sysfs_remove);	/* [한국어] 드라이버 제거 경로 */

/*
 * IOMMU drivers can indicate a device is managed by a given IOMMU using
 * this interface.  A link to the device will be created in the "devices"
 * directory of the IOMMU device in sysfs and an "iommu" link will be
 * created under the linked device, pointing back at the IOMMU device.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * iommu_device_link - IOMMU 와 장치 사이에 양방향 심볼릭 링크를 만든다
 *
 * @iommu: 담당 IOMMU
 * @link:  그 아래로 들어온 장치
 * @return: 0 성공, 음수 실패
 *
 * 두 방향을 모두 만드는 것이 요점이다. IOMMU 쪽에서는 "내가 맡은 장치 목록"이
 * 되고, 장치 쪽에서는 "나를 맡은 IOMMU"가 된다. 한쪽만 만들면 관리자가 반대
 * 방향을 추적할 수 없으므로, 두 번째가 실패하면 첫 번째도 되돌린다.
 *
 * 실행 컨텍스트: 장치 프로브 경로. 프로세스 문맥.
 *
 * 호출 체인: iommu.c 의 iommu_init_device → [이 함수]
 */
int iommu_device_link(struct iommu_device *iommu, struct device *link)
{
	int ret;	/* [한국어] 링크 생성 결과 */

	ret = sysfs_add_link_to_group(&iommu->dev->kobj, "devices",	/* [한국어] IOMMU 쪽: devices/ 아래에 이 장치로 가는 링크 */
				      &link->kobj, dev_name(link));	/* [한국어] 링크 이름은 장치 이름 */
	if (ret)	/* [한국어] 실패 */
		return ret;	/* [한국어] 한쪽만 만들지 않는다 */

	ret = sysfs_create_link_nowarn(&link->kobj, &iommu->dev->kobj, "iommu");	/* [한국어] 장치 쪽: iommu 라는 이름으로 IOMMU 를 가리키는 역링크. nowarn 인 것은 이미 존재할 수 있기 때문이다 */
	if (ret)	/* [한국어] 역링크 생성 실패 */
		sysfs_remove_link_from_group(&iommu->dev->kobj, "devices",	/* [한국어] 앞서 만든 링크를 되돌린다 — 한 방향만 남기지 않는다 */
					     dev_name(link));	/* [한국어] 같은 이름으로 */

	return ret;	/* [한국어] 0 이면 양방향 링크가 걸렸다 */
}

/*
 * [한국어]
 * iommu_device_unlink - 양방향 링크를 걷어 낸다
 *
 * @iommu: 담당 IOMMU
 * @link:  떠나는 장치
 *
 * link 의 짝. 반환값이 없는 것은 링크 제거가 실패할 수 없기 때문이다 — 없는
 * 링크를 지우려 해도 무해하게 지나간다.
 *
 * 실행 컨텍스트: 장치 제거 경로. 프로세스 문맥.
 *
 * 호출 체인: iommu.c 의 iommu_deinit_device → [이 함수]
 */
void iommu_device_unlink(struct iommu_device *iommu, struct device *link)
{
	sysfs_remove_link(&link->kobj, "iommu");	/* [한국어] 장치 쪽 역링크 제거 */
	sysfs_remove_link_from_group(&iommu->dev->kobj, "devices", dev_name(link));	/* [한국어] IOMMU 쪽 링크 제거 */
}
