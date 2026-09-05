/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

/*
 * [한국어 설명] AMD iommufd 연동 함수의 선언과 빌드 스위치 (iommufd.h)
 *
 * === 파일의 역할 ===
 * iommufd.c 가 제공하는 세 함수를 선언한다. 그런데 이 헤더의 진짜 역할은
 * 선언이 아니라 아래쪽 #else 절에 있다 — 그 기능을 끈 커널에서는 함수 이름
 * 자체를 NULL 로 정의한다.
 *
 * 왜 그렇게 하는가: 이 이름들은 amd_iommu_ops 의 필드 초기화에 쓰인다.
 * 기능을 껐을 때 그 자리를 비우려면 초기화 코드를 #ifdef 로 갈라야 하는데,
 * 이름을 NULL 로 만들어 두면 초기화는 한 가지 모양을 유지하고 콜백만
 * 사라진다. 코어는 NULL 콜백을 "지원하지 않음"으로 읽는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * iommu.c 의 amd_iommu_ops 정의와 iommufd.c 사이에만 놓인다. 외부에서
 * 쓰이지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 선언에 나오는 iommufd_viommu / iommu_user_data / iommu_hw_info_type 은
 * 모두 코어 iommufd 의 타입이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - amd_iommufd_hw_info(): 하드웨어 능력을 사용자에게 보고.
 * - amd_iommufd_get_viommu_size(): vIOMMU 객체의 할당 크기.
 * - amd_iommufd_viommu_init(): vIOMMU 초기화.
 * - 세 이름의 NULL 정의: 기능을 끈 빌드에서 ops 초기화를 그대로 두기 위한 장치.
 */
#ifndef AMD_IOMMUFD_H	/* [한국어] 헤더 중복 포함 방지 */
#define AMD_IOMMUFD_H

#if IS_ENABLED(CONFIG_AMD_IOMMU_IOMMUFD)	/* [한국어] iommufd 연동을 켠 빌드에서만 실제 함수가 있다 */
void *amd_iommufd_hw_info(struct device *dev, u32 *length, enum iommu_hw_info_type *type);	/* [한국어] EFR/EFR2 를 사용자 공간에 보고한다 */
size_t amd_iommufd_get_viommu_size(struct device *dev, enum iommu_viommu_type viommu_type);	/* [한국어] 코어가 잡아야 할 vIOMMU 객체의 크기를 알린다 */
int amd_iommufd_viommu_init(struct iommufd_viommu *viommu, struct iommu_domain *parent,	/* [한국어] 할당된 객체를 초기화하고 부모 도메인에 연결한다 */
			    const struct iommu_user_data *user_data);
#else
#define amd_iommufd_hw_info NULL	/* [한국어] 끈 빌드에서는 이름이 NULL 이 되어 ops 초기화가 그대로 컴파일된다 */
#define amd_iommufd_viommu_init NULL	/* [한국어] 같은 목적 */
#define amd_iommufd_get_viommu_size NULL	/* [한국어] 같은 목적. 코어는 NULL 콜백을 "지원하지 않음"으로 읽는다 */
#endif /* CONFIG_AMD_IOMMU_IOMMUFD */

#endif /* AMD_IOMMUFD_H */
