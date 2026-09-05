// SPDX-License-Identifier: GPL-2.0
/*
 * IOMMU debugfs core infrastructure
 *
 * Copyright (C) 2018 Advanced Micro Devices, Inc.
 *
 * Author: Gary R Hook <gary.hook@amd.com>
 */

/*
 * [한국어 설명] IOMMU debugfs 공통 뿌리 (drivers/iommu/iommu-debugfs.c)
 *
 * === 파일의 역할 ===
 * /sys/kernel/debug/iommu 디렉터리 하나를 만들고, 그 아래에 벤더 드라이버들이
 * 자기 진단 항목을 매달 수 있게 해 주는 것이 전부다. 코드는 짧지만 그 안의
 * 경고 배너가 이 파일의 실질적인 내용이다.
 *
 * 왜 그렇게 요란한 경고를 내는가 — 이 디렉터리를 통해 노출되는 것은 페이지
 * 테이블 내용, 장치-도메인 대응, 하드웨어 레지스터 값 같은 IOMMU 내부 상태다.
 * 그것을 읽을 수 있으면 격리가 어떻게 설정되어 있는지, 어느 주소가 매핑되어
 * 있는지가 드러나므로, 프로덕션 커널에 켜져 있어서는 안 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름: iommu.c 의 iommu_init (core_initcall)
 *         → [이 파일] iommu_debugfs_setup : 뿌리 디렉터리 생성 + 경고
 *       이후 벤더 드라이버가 iommu_debugfs_dir 아래에 자기 디렉터리를 만든다.
 *
 * === 타 모듈과의 연결 ===
 * - iommu.c: 부팅 초기화에서 이 함수를 부른다.
 * - 벤더 드라이버: 노출된 iommu_debugfs_dir 을 부모로 삼아 자기 항목을 만든다
 *   (AMD 의 IOMMU 레지스터 덤프, 인텔의 DMAR 상태, ARM SMMU 의 스트림 테이블 등).
 *
 * === 주요 함수/구조체 요약 ===
 * - iommu_debugfs_dir     : 모든 IOMMU debugfs 항목의 부모 디렉터리.
 * - iommu_debugfs_setup() : 그 디렉터리를 만들고 보안 경고를 남긴다.
 */
#include <linux/pci.h>	/* [한국어] 일부 벤더 항목이 PCI 정보를 노출한다 */
#include <linux/iommu.h>	/* [한국어] 공개 선언 */
#include <linux/debugfs.h>	/* [한국어] 디렉터리 생성 API */

struct dentry *iommu_debugfs_dir;	/* [한국어] 모든 IOMMU debugfs 항목의 부모. 벤더 드라이버가 이것을 부모로 삼아 자기 디렉터리를 만든다 */
EXPORT_SYMBOL_GPL(iommu_debugfs_dir);	/* [한국어] 모듈로 빌드되는 벤더 드라이버가 쓸 수 있게 */

/**
 * iommu_debugfs_setup - create the top-level iommu directory in debugfs
 *
 * Provide base enablement for using debugfs to expose internal data of an
 * IOMMU driver. When called, this function creates the
 * /sys/kernel/debug/iommu directory.
 *
 * Emit a strong warning at boot time to indicate that this feature is
 * enabled.
 *
 * This function is called from iommu_init; drivers may then use
 * iommu_debugfs_dir to instantiate a vendor-specific directory to be used
 * to expose internal data.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_debugfs_setup - IOMMU debugfs 뿌리 디렉터리를 만든다
 *
 * 하는 일은 디렉터리 하나를 만드는 것뿐이고, 나머지 열다섯 줄이 경고 배너다.
 * 그 비중이 이 기능의 성격을 말해 준다 — 편의를 위해 격리 구성을 통째로
 * 들여다볼 수 있게 만드는 것이므로, 프로덕션 커널에 켜져 있으면 안 된다.
 *
 * 벤더 드라이버는 이 함수를 직접 부르지 않는다. iommu.c 의 부팅 초기화가 한 번
 * 부르고, 드라이버들은 그 결과로 만들어진 iommu_debugfs_dir 을 부모로 쓴다
 * (위 영어 주석).
 *
 * 실행 컨텍스트: 부팅 초기화. 프로세스 문맥.
 *
 * 호출 체인: iommu.c 의 iommu_init → [이 함수]
 */
void iommu_debugfs_setup(void)
{
	if (!iommu_debugfs_dir) {	/* [한국어] 아직 만들지 않았으면 (여러 번 불려도 안전하게) */
		iommu_debugfs_dir = debugfs_create_dir("iommu", NULL);	/* [한국어] /sys/kernel/debug/iommu 를 만든다. 부모가 NULL 이라 debugfs 최상위에 놓인다 */
		pr_warn("\n");	/* [한국어] 경고 배너 시작 — 빈 줄로 앞의 로그와 분리한다 */
		pr_warn("*************************************************************\n");	/* [한국어] 눈에 띄는 테두리 */
		pr_warn("**     NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE    **\n");	/* [한국어] 부팅 로그를 훑다가 놓치지 않도록 */
		pr_warn("**                                                         **\n");	/* [한국어] 배너 여백 */
		pr_warn("**  IOMMU DebugFS SUPPORT HAS BEEN ENABLED IN THIS KERNEL  **\n");	/* [한국어] 이 빌드에 진단 인터페이스가 켜져 있다는 사실 */
		pr_warn("**                                                         **\n");	/* [한국어] 배너 여백 */
		pr_warn("** This means that this kernel is built to expose internal **\n");	/* [한국어] 노출되는 것이 무엇인지 */
		pr_warn("** IOMMU data structures, which may compromise security on **\n");	/* [한국어] 페이지 테이블과 도메인 대응이 드러나면 격리 구성이 노출된다 */
		pr_warn("** your system.                                            **\n");	/* [한국어] 보안에 영향을 준다는 결론 */
		pr_warn("**                                                         **\n");	/* [한국어] 배너 여백 */
		pr_warn("** If you see this message and you are not debugging the   **\n");	/* [한국어] 디버깅 중이 아니라면 */
		pr_warn("** kernel, report this immediately to your vendor!         **\n");	/* [한국어] 배포판이 실수로 켜 두었다는 뜻이므로 알리라는 지시 */
		pr_warn("**                                                         **\n");	/* [한국어] 배너 여백 */
		pr_warn("**     NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE    **\n");	/* [한국어] 배너 마무리 */
		pr_warn("*************************************************************\n");	/* [한국어] 테두리 닫기 */
	}
}
