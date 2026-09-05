// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2021-2024 NVIDIA CORPORATION & AFFILIATES. */

/*
 * [한국어 설명] NVIDIA Tegra241 의 명령 큐 가상화 확장 (tegra241-cmdqv.c)
 *
 * === 파일의 역할 ===
 * 표준 SMMUv3 는 명령 큐가 하나뿐이다. 무효화 명령은 그 하나의 링에 모두
 * 몰리고, 여러 CPU 가 동시에 명령을 넣으려 다투며, 가상 머신이 낸 무효화도
 * 결국 호스트가 대신 그 큐에 넣어 줘야 한다. Tegra241 의 CMDQV(Command Queue
 * Virtualization) 확장은 그 병목을 두 갈래로 푼다.
 * 첫째, 큐를 여러 개 둔다. VINTF(가상 인터페이스) 하나마다 여러 개의
 * LVCMDQ(논리 명령 큐)가 딸려 있어, CPU 마다 다른 큐를 골라 쓰면 락 경합이
 * 사라진다. 커널이 쓰는 VINTF0 가 그 용도다.
 * 둘째, 게스트에게 큐를 통째로 넘긴다. 게스트가 자기 큐에 무효화 명령을
 * 직접 써 넣으면 하드웨어가 그것을 읽어 실행하되, VINTF 에 걸어 둔 VMID 와
 * 스트림 번호 치환표를 통해 게스트가 남의 것을 건드리지 못하게 막는다.
 * 호스트 커널이 명령마다 끼어들 필요가 없어져, 게스트의 무효화 지연이
 * 크게 줄어든다. 이것이 이 확장의 진짜 목적이다.
 * 이 파일은 그 하드웨어를 찾아내고, 큐를 나눠 배정하고, 게스트에게 넘기는
 * iommufd 경로를 구현한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 하드웨어 계층은 세 겹이다:
 *
 *   CMDQV (칩 하나에 하나)
 *     └ VINTF (가상 인터페이스, 여러 개) — VMID 와 스트림 치환표를 갖는다
 *         └ LVCMDQ (논리 명령 큐, VINTF 마다 여러 개) — 실제 명령 링
 *
 * 전역 VCMDQ 가 물리적으로 존재하고, 그것을 어느 VINTF 의 몇 번째 LVCMDQ 로
 * 쓸지 CMDQ_ALLOC 레지스터로 배정한다. VINTF0 은 커널이 쓰고, 나머지는
 * iommufd 를 통해 게스트에게 하나씩 넘어간다.
 *
 * 커널이 명령을 낼 때의 흐름:
 *   arm_smmu_cmdq_issue_cmdlist() → smmu->impl_ops->get_secondary_cmdq
 *     → tegra241_cmdqv_get_cmdq()   ← 이 파일 (CPU 번호로 큐를 고른다)
 *     → 고른 LVCMDQ 에 명령을 넣는다
 *
 * 게스트에게 큐를 넘길 때의 흐름:
 *   VMM → IOMMU_VIOMMU_ALLOC → arm_vsmmu_init()
 *     → tegra241_cmdqv_init_vintf_user()   ← 이 파일 (VINTF 를 하나 배정)
 *   VMM → IOMMU_HW_QUEUE_ALLOC
 *     → tegra241_vintf_alloc_lvcmdq_user() ← 이 파일 (큐를 하나 배정)
 *   VMM → mmap()  → 게스트가 큐 레지스터에 직접 접근하게 된다
 *
 * 실행 컨텍스트는 프로브(프로세스 문맥), 명령 발행(원자적 문맥일 수 있음),
 * 오류 인터럽트(스레드 처리), iommufd ioctl(프로세스 문맥) 네 갈래다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽으로는 arm-smmu-v3.c 의 impl_ops 갈고리표에 얹힌다. 표준 드라이버가
 * 프로브 도중 tegra241_cmdqv_probe() 를 불러 이 확장을 찾아내고, 찾으면
 * arm_smmu_device 를 품은 더 큰 구조체(tegra241_cmdqv)로 바꿔 쓴다.
 * 그 뒤로는 큐 선택·리셋·제거·하드웨어 정보 노출이 모두 이 파일로 내려온다.
 * 옆으로는 arm-smmu-v3-iommufd.c 와 짝을 이룬다. 게스트 도메인을 만들고
 * 무효화를 걸러 내는 일은 그쪽이 그대로 맡고(연산표에서 그 함수들을
 * 그대로 재사용한다), 이 파일은 "가속된 큐를 게스트에게 직접 넘기는" 부분만
 * 더한다. 게스트가 가속 큐로 낼 수 없는 명령은 여전히 커널을 거친다.
 * 아래쪽으로는 iommufd 의 hw_queue·vdevice·mmap 기반 위에 선다. 큐 객체의
 * 수명과 의존 관계(LVCMDQ 는 반드시 오름차순으로 배정되고 내림차순으로
 * 해제되어야 한다)를 iommufd 코어가 대신 강제해 준다.
 * 공유 상태는 MMIO 레지스터 창 하나다 — CMDQV 전역 설정, VINTF 별 설정,
 * VCMDQ 별 페이지0/페이지1 이 그 안에 겹겹이 배치되어 있고, 파일 앞머리의
 * 주소 계산 매크로들이 그 배치를 그대로 옮겨 적은 것이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct tegra241_cmdqv: 칩의 CMDQV 하나. arm_smmu_device 를 첫 필드로 품어,
 *   표준 드라이버가 보는 SMMU 와 같은 객체이면서 확장 정보를 더 들고 있다.
 * - struct tegra241_vintf: 가상 인터페이스 하나. VMID 와 스트림 치환표를 쥐고,
 *   커널용(VINTF0)이거나 게스트용이다.
 * - struct tegra241_vcmdq: 논리 명령 큐 하나. arm_smmu_cmdq 를 품어, 표준
 *   명령 발행 코드가 그대로 쓸 수 있다.
 * - tegra241_cmdqv_get_cmdq(): CPU 번호로 큐를 골라 락 경합을 흩는다.
 * - tegra241_vintf_hw_init()/hw_deinit(): VINTF 를 켜고 끄며, LVCMDQ 를
 *   하드웨어가 요구하는 순서(켤 때 오름차순, 끌 때 내림차순)로 다룬다.
 * - tegra241_vintf_init_vsid(): 게스트가 아는 스트림 번호를 실제 번호로
 *   바꿔 주는 치환표 한 칸을 채운다 — 게스트가 남의 장치를 못 건드리게 하는 장치.
 * - tegra241_cmdqv_isr(): 큐에서 난 오류를 VINTF 별로 갈라, 커널 것은 직접
 *   처리하고 게스트 것은 게스트에게 올린다.
 */

#define dev_fmt(fmt) "tegra241_cmdqv: " fmt	/* [한국어] 이 파일의 모든 dev_* 로그 앞에 붙일 꼬리표. 어느 드라이버가 찍었는지 한눈에 알 수 있다. */

#include <linux/debugfs.h>	/* [한국어] bypass_vcmdq 를 실행 중에 토글할 수 있게 debugfs 에 노출한다. */
#include <linux/dma-mapping.h>	/* [한국어] 명령 큐 링 버퍼를 dma 로 잡기 위해. */
#include <linux/interrupt.h>	/* [한국어] 오류 인터럽트 등록. */
#include <linux/iommu.h>	/* [한국어] iommu 코어의 기본 형들. */
#include <linux/iommufd.h>	/* [한국어] 게스트에게 큐와 장치를 넘기는 객체 모델. */
#include <linux/iopoll.h>	/* [한국어] readl_poll_timeout — 설정이 실제로 반영될 때까지 기다린다. */
#include <linux/platform_device.h>	/* [한국어] CMDQV 는 ACPI 를 통해 플랫폼 장치로 나타난다. */
#include <uapi/linux/iommufd.h>	/* [한국어] 사용자 공간과 주고받는 구조체 정의. */

#include "arm-smmu-v3.h"	/* [한국어] 표준 SMMUv3 자료 모델 — 이 확장은 그 위에 얹힌다. */

/* CMDQV register page base and size defines */
/* [한국어] (위 영어 주석 참고) MMIO 창은 64K 짜리 페이지 여러 개가 줄지어 놓인
 * 모양이다. 전역 설정, VCMDQ 페이지0, VCMDQ 페이지1, 그리고 VINTF 별 페이지가
 * 차례로 온다. 아래 상수들이 그 배치를 그대로 옮긴 것이다. */
#define TEGRA241_CMDQV_CONFIG_BASE	(0)	/* [한국어] 전역 설정 레지스터가 창의 맨 앞에 있다. */
#define TEGRA241_CMDQV_CONFIG_SIZE	(SZ_64K)	/* [한국어] 전역 설정 영역의 크기. */
#define TEGRA241_VCMDQ_PAGE0_BASE	(TEGRA241_CMDQV_CONFIG_BASE + SZ_64K)	/* [한국어] 그다음이 VCMDQ 페이지0 — 생산·소비 포인터와 오류 상태가 들어 있다. */
#define TEGRA241_VCMDQ_PAGE1_BASE	(TEGRA241_VCMDQ_PAGE0_BASE + SZ_64K)	/* [한국어] 페이지1 은 링 버퍼 주소를 담는다. 페이지를 나눈 이유는 게스트에게 페이지0 만 열어 주기 위해서다. */
#define TEGRA241_VINTF_PAGE_BASE	(TEGRA241_VCMDQ_PAGE1_BASE + SZ_64K)	/* [한국어] 마지막이 VINTF 별 페이지 묶음 — 게스트에게 mmap 으로 넘기는 영역이 여기다. */

/* CMDQV global base regs */
/* [한국어] (위 영어 주석 참고) 여기부터는 칩 전체에 하나뿐인 전역 레지스터들. */
#define TEGRA241_CMDQV_CONFIG		0x0000	/* [한국어] CMDQV 전체를 켜고 끄는 설정 레지스터. */
#define  CMDQV_EN			BIT(0)	/* [한국어] 확장 전체를 켜는 비트. 끄면 표준 SMMU 명령 큐만 쓰게 된다. */

#define TEGRA241_CMDQV_PARAM		0x0004	/* [한국어] 이 칩이 큐를 몇 개 가졌는지 알려 주는 읽기 전용 레지스터. */
#define  CMDQV_NUM_SID_PER_VM_LOG2	GENMASK(15, 12)	/* [한국어] VINTF 하나가 담을 수 있는 스트림 치환 항목 수(로그). 게스트에 넘길 수 있는 장치 수의 상한이다. */
#define  CMDQV_NUM_VINTF_LOG2		GENMASK(11, 8)	/* [한국어] VINTF 개수(로그) — 동시에 가속을 받을 수 있는 가상 머신 수. */
#define  CMDQV_NUM_VCMDQ_LOG2		GENMASK(7, 4)	/* [한국어] 전역 VCMDQ 개수(로그). VINTF 수로 나누면 VINTF 하나당 큐 수가 나온다. */
#define  CMDQV_VER			GENMASK(3, 0)	/* [한국어] 확장의 개정 번호. 게스트에게 그대로 노출한다. */

#define TEGRA241_CMDQV_STATUS		0x0008	/* [한국어] 설정이 실제로 반영됐는지 알려 주는 상태 레지스터. */
#define  CMDQV_ENABLED			BIT(0)	/* [한국어] 정말 켜졌는가. 설정을 쓴 뒤 이 비트를 폴링해야 한다. */

#define TEGRA241_CMDQV_VINTF_ERR_MAP	0x0014	/* [한국어] 어느 VINTF 에서 오류가 났는지 비트맵으로 알려 준다. 인터럽트 처리의 출발점이다. */
#define TEGRA241_CMDQV_VINTF_INT_MASK	0x001C	/* [한국어] VINTF 별 인터럽트 마스크. */
#define TEGRA241_CMDQV_CMDQ_ERR_MAP(m)  (0x0024 + 0x4*(m))	/* [한국어] 어느 큐에서 오류가 났는지 비트맵. 큐가 많아 32비트 레지스터 여러 개로 나뉘어 있다. */

#define TEGRA241_CMDQV_CMDQ_ALLOC(q)	(0x0200 + 0x4*(q))	/* [한국어] 전역 큐 q 를 어느 VINTF 의 몇 번째 논리 큐로 쓸지 배정하는 레지스터. */
#define  CMDQV_CMDQ_ALLOC_VINTF		GENMASK(20, 15)	/* [한국어] 배정받을 VINTF 번호. */
#define  CMDQV_CMDQ_ALLOC_LVCMDQ	GENMASK(7, 1)	/* [한국어] 그 VINTF 안에서의 논리 큐 번호. */
#define  CMDQV_CMDQ_ALLOCATED		BIT(0)	/* [한국어] 이 배정이 실제로 살아 있는가. 이 비트를 켜야 큐가 동작한다. */

/* VINTF base regs */
/* [한국어] (위 영어 주석 참고) VINTF 하나마다 0x100 바이트씩 떨어져 있다. */
#define TEGRA241_VINTF(v)		(0x1000 + 0x100*(v))	/* [한국어] v 번째 VINTF 의 레지스터 묶음이 시작하는 자리. */

#define TEGRA241_VINTF_CONFIG		0x0000	/* [한국어] 이 VINTF 를 켜고 끄고, 누가 주인인지 정하는 레지스터. */
#define  VINTF_HYP_OWN			BIT(17)	/* [한국어] 하이퍼바이저(호스트 커널)가 이 VINTF 의 주인인가. 게스트에서 읽으면 하드웨어가 0 으로 고정해 돌려준다. */
#define  VINTF_VMID			GENMASK(16, 1)	/* [한국어] 이 VINTF 의 큐에서 나온 명령에 강제로 붙일 VMID. 게스트가 남의 VMID 를 지우지 못하게 막는 핵심 장치다. */
#define  VINTF_EN			BIT(0)	/* [한국어] 이 VINTF 를 켜는 비트. */

#define TEGRA241_VINTF_STATUS		0x0004	/* [한국어] VINTF 의 실제 상태. */
#define  VINTF_STATUS			GENMASK(3, 1)	/* [한국어] 상태 코드 — 오류 진단에 쓴다. */
#define  VINTF_ENABLED			BIT(0)	/* [한국어] 정말 켜졌는가. */

#define TEGRA241_VINTF_SID_MATCH(s)	(0x0040 + 0x4*(s))	/* [한국어] 게스트가 명령에 적을 스트림 번호(가상). 여기 적힌 값과 일치하면 아래 REPLACE 값으로 바뀐다. */
#define TEGRA241_VINTF_SID_REPLACE(s)	(0x0080 + 0x4*(s))	/* [한국어] 그때 대신 쓸 실제 스트림 번호. 이 두 레지스터가 짝을 이뤄 게스트를 가둔다. */

#define TEGRA241_VINTF_LVCMDQ_ERR_MAP_64(m) \
					(0x00C0 + 0x8*(m))	/* [한국어] 이 VINTF 의 어느 논리 큐에서 오류가 났는지 알려 주는 64비트 비트맵. */
#define  LVCMDQ_ERR_MAP_NUM_64		2	/* [한국어] 그 비트맵이 두 개 — 곧 논리 큐를 최대 128개까지 표현할 수 있다. */

/* VCMDQ base regs */
/* -- PAGE0 -- */
/* [한국어] (위 영어 주석 참고) 페이지0 에는 게스트가 직접 만져도 되는 것들이
 * 모여 있다 — 포인터와 오류 상태. 그래서 이 페이지만 게스트에게 mmap 으로
 * 열어 준다. */
#define TEGRA241_VCMDQ_PAGE0(q)		(TEGRA241_VCMDQ_PAGE0_BASE + 0x80*(q))	/* [한국어] q 번째 큐의 페이지0 레지스터 묶음. */

#define TEGRA241_VCMDQ_CONS		0x00000	/* [한국어] 소비 포인터 — 하드웨어가 어디까지 처리했는지 알려 준다. */
#define  VCMDQ_CONS_ERR			GENMASK(30, 24)	/* [한국어] 소비 포인터에 얹힌 오류 코드. 명령이 잘못됐을 때 그 이유가 여기 실린다. */

#define TEGRA241_VCMDQ_PROD		0x00004	/* [한국어] 생산 포인터 — 소프트웨어가 어디까지 썼는지 알린다. */

#define TEGRA241_VCMDQ_CONFIG		0x00008	/* [한국어] 이 큐를 켜고 끄는 레지스터. */
#define  VCMDQ_EN			BIT(0)	/* [한국어] 큐를 켜는 비트. */

#define TEGRA241_VCMDQ_STATUS		0x0000C	/* [한국어] 큐의 실제 상태. */
#define  VCMDQ_ENABLED			BIT(0)	/* [한국어] 정말 켜졌는가. */

#define TEGRA241_VCMDQ_GERROR		0x00010	/* [한국어] 이 큐에서 난 전역 오류 비트들. */
#define TEGRA241_VCMDQ_GERRORN		0x00014	/* [한국어] 그 오류를 확인했음을 알리는 레지스터. GERROR 값을 그대로 여기 쓰면 오류가 지워진다. */

/* -- PAGE1 -- */
/* [한국어] (위 영어 주석 참고) 페이지1 에는 링 버퍼의 물리 주소가 있다.
 * 게스트가 이 값을 마음대로 바꾸면 아무 메모리나 명령으로 읽게 되므로,
 * 이 페이지는 절대 게스트에게 열어 주지 않고 호스트만 쓴다. */
#define TEGRA241_VCMDQ_PAGE1(q)		(TEGRA241_VCMDQ_PAGE1_BASE + 0x80*(q))	/* [한국어] q 번째 큐의 페이지1 레지스터 묶음. */
#define  VCMDQ_ADDR			GENMASK(47, 5)	/* [한국어] 링 버퍼 주소가 놓이는 비트 자리 — 아래 5비트는 정렬 때문에 0 이어야 한다. */
#define  VCMDQ_LOG2SIZE			GENMASK(4, 0)	/* [한국어] 그 아래 비트에는 큐 크기(로그)를 함께 담는다. 주소와 크기를 한 레지스터에 겹쳐 둔 것이다. */

#define TEGRA241_VCMDQ_BASE		0x00000	/* [한국어] 링 버퍼의 물리 주소와 크기를 쓰는 자리. */
#define TEGRA241_VCMDQ_CONS_INDX_BASE	0x00008	/* [한국어] 하드웨어가 소비 포인터를 메모리에 복사해 줄 주소. 쓰지 않으면 0 으로 둔다. */

/* VINTF logical-VCMDQ pages */
/* [한국어] (위 영어 주석 참고) VINTF 별로 자기 논리 큐들의 레지스터가 따로
 * 매핑되어 있다. 게스트는 이 영역만 보므로, 전역 큐 번호를 몰라도 자기
 * 논리 큐 번호로 접근할 수 있다 — 그 자체가 격리 장치다. */
#define TEGRA241_VINTFi_PAGE0(i)	(TEGRA241_VINTF_PAGE_BASE + SZ_128K*(i))	/* [한국어] i 번째 VINTF 의 페이지0 묶음. VINTF 하나가 128K 를 차지한다. */
#define TEGRA241_VINTFi_PAGE1(i)	(TEGRA241_VINTFi_PAGE0(i) + SZ_64K)	/* [한국어] 그 뒤 64K 가 페이지1 묶음 — 호스트 전용이다. */
#define TEGRA241_VINTFi_LVCMDQ_PAGE0(i, q) \
					(TEGRA241_VINTFi_PAGE0(i) + 0x80*(q))	/* [한국어] i 번 VINTF 의 q 번 논리 큐 페이지0. */
#define TEGRA241_VINTFi_LVCMDQ_PAGE1(i, q) \
					(TEGRA241_VINTFi_PAGE1(i) + 0x80*(q))	/* [한국어] 같은 큐의 페이지1. */

/* MMIO helpers */
/* [한국어] (위 영어 주석 참고) 레지스터 이름만 적으면 주소가 나오게 하는
 * 매크로들. 접두사를 붙여 주므로 호출부가 REG_VINTF(vintf, CONFIG) 처럼
 * 짧게 쓸 수 있다. */
#define REG_CMDQV(_cmdqv, _regname) \
	((_cmdqv)->base + TEGRA241_CMDQV_##_regname)	/* [한국어] 전역 레지스터 주소 계산. */
#define REG_VINTF(_vintf, _regname) \
	((_vintf)->base + TEGRA241_VINTF_##_regname)	/* [한국어] 그 VINTF 의 레지스터 주소 계산. */
#define REG_VCMDQ_PAGE0(_vcmdq, _regname) \
	((_vcmdq)->page0 + TEGRA241_VCMDQ_##_regname)	/* [한국어] 그 큐의 페이지0 레지스터 주소 계산. */
#define REG_VCMDQ_PAGE1(_vcmdq, _regname) \
	((_vcmdq)->page1 + TEGRA241_VCMDQ_##_regname)	/* [한국어] 그 큐의 페이지1 레지스터 주소 계산. */


static bool disable_cmdqv;	/* [한국어] 확장을 아예 끄고 표준 명령 큐만 쓸지 정하는 모듈 인자. 이 확장이 의심스러울 때 문제를 가르는 데 쓴다. */
module_param(disable_cmdqv, bool, 0444);	/* [한국어] 부팅 인자로만 줄 수 있다(읽기 전용) — 프로브 때 한 번만 보는 값이라 나중에 바꿔도 뜻이 없다. */
MODULE_PARM_DESC(disable_cmdqv,	/* [한국어] modinfo 로 이 인자의 뜻을 확인할 수 있게 설명을 붙인다. */
	"This allows to disable CMDQV HW and use default SMMU internal CMDQ.");	/* [한국어] modinfo 에 찍힐 설명. */

static bool bypass_vcmdq;	/* [한국어] 확장은 켜 두되 명령만 표준 큐로 보내게 하는 인자. 성능 비교와 디버깅에 쓴다. */
module_param(bypass_vcmdq, bool, 0444);	/* [한국어] 부팅 인자로 준다. 다만 아래 debugfs 를 통해 실행 중에도 바꿀 수 있다. */
MODULE_PARM_DESC(bypass_vcmdq,	/* [한국어] 같은 이유로 설명을 붙인다. */
	"This allows to bypass VCMDQ for debugging use or perf comparison.");	/* [한국어] modinfo 에 찍힐 설명. */

/**
 * struct tegra241_vcmdq - Virtual Command Queue
 * @core: Embedded iommufd_hw_queue structure
 * @idx: Global index in the CMDQV
 * @lidx: Local index in the VINTF
 * @enabled: Enable status
 * @cmdqv: Parent CMDQV pointer
 * @vintf: Parent VINTF pointer
 * @prev: Previous LVCMDQ to depend on
 * @cmdq: Command Queue struct
 * @page0: MMIO Page0 base address
 * @page1: MMIO Page1 base address
 */
/* [한국어] 논리 명령 큐 하나. 커널이 쓰는 것과 게스트에게 넘긴 것이 같은
 * 구조를 쓴다.
 *
 * 전역 번호와 지역 번호를 둘 다 들고 있는 것이 요점이다. 하드웨어의 배정
 * 레지스터는 전역 번호로 접근하지만, 게스트는 자기 VINTF 안의 지역 번호만
 * 알기 때문이다. (위 영어 kernel-doc 참고) */
struct tegra241_vcmdq {
	/* [한국어] iommufd 가 아는 하드웨어 큐 몸통 — 게스트에게 넘길 때 쓰인다.
	 * 설정자: iommufd 가 객체를 잡으며 채우고, alloc_lvcmdq_user() 가 이어받는다.
	 * 읽는 자: hw_queue_to_vcmdq() 로 이 구조를 되찾는 모든 곳.
	 * 값 범위: 커널용 큐에서는 쓰이지 않고 0 으로 남는다.
	 * 동기화: iommufd 객체 수명 규칙을 따른다. */
	struct iommufd_hw_queue core;

	/* [한국어] CMDQV 전체에서의 큐 번호.
	 * 설정자: init_lvcmdq() 가 (VINTF 번호 × VINTF당 큐 수 + 지역 번호)로 계산한다.
	 * 읽는 자: CMDQ_ALLOC 레지스터를 짚을 때 — 배정은 전역 번호로만 한다.
	 * 값 범위: 0 ~ num_vcmdqs-1.
	 * 동기화: 큐를 배정할 때 한 번 정해지고 바뀌지 않는다. */
	u16 idx;
	/* [한국어] 자기 VINTF 안에서의 큐 번호.
	 * 설정자: 위와 같은 자리에서 채운다.
	 * 읽는 자: VINTF 별 레지스터 페이지를 짚을 때와, 게스트가 큐를 지목할 때.
	 * 값 범위: 0 ~ num_lvcmdqs_per_vintf-1.
	 * 동기화: 위와 같다. */
	u16 lidx;

	/* [한국어] 이 큐가 켜져 있는가.
	 * 설정자: vcmdq_write_config() 가 하드웨어 상태를 확인한 뒤 기록한다.
	 * 읽는 자: 명령을 넣을 큐를 고르는 자리에서 READ_ONCE 로 읽는다 —
	 *         꺼진 큐에 명령을 넣으면 영원히 처리되지 않는다.
	 * 값 범위: true/false.
	 * 동기화: 쓰는 쪽은 설정 경로 하나뿐이고 읽는 쪽은 여럿이라
	 *         WRITE_ONCE/READ_ONCE 로만 다룬다. */
	bool enabled;

	/* [한국어] 이 큐가 속한 CMDQV.
	 * 설정자: init_lvcmdq().
	 * 읽는 자: 전역 레지스터와 로그 장치에 닿을 때.
	 * 값 범위: NULL 일 수 없다.
	 * 동기화: 불변. */
	struct tegra241_cmdqv *cmdqv;
	/* [한국어] 이 큐가 속한 VINTF.
	 * 설정자: init_lvcmdq().
	 * 읽는 자: 주인이 커널인지 게스트인지(hyp_own) 볼 때, 로그 머리말을 지을 때.
	 * 값 범위: NULL 이면 아직 배정 전이다 — 로그 헬퍼가 그것을 검사한다.
	 * 동기화: 불변. */
	struct tegra241_vintf *vintf;
	/* [한국어] 바로 앞 번호의 논리 큐.
	 * 설정자: 게스트용 큐를 배정할 때, 앞 큐가 이미 있어야 하므로 그것을 가리킨다.
	 * 읽는 자: 해제 순서를 강제하려고 iommufd 에 의존 관계로 등록할 때.
	 * 값 범위: 0번 큐에서는 NULL.
	 * 동기화: lvcmdq_mutex 아래에서만 바뀐다.
	 *         (하드웨어가 오름차순 배정·내림차순 해제를 요구하기 때문에 필요하다.) */
	struct tegra241_vcmdq *prev;
	/* [한국어] 표준 드라이버가 아는 명령 큐 구조.
	 * 설정자: alloc_smmu_cmdq() 가 링 버퍼를 잡아 채운다.
	 * 읽는 자: arm_smmu_cmdq_issue_cmdlist() 가 이것을 받아 그대로 쓴다 —
	 *         표준 코드가 이 확장을 모르고도 동작하는 이유가 이 필드다.
	 * 값 범위: 게스트용 큐에서는 링 버퍼를 커널이 잡지 않고 게스트가 준 주소를 쓴다.
	 * 동기화: 큐 내부의 락 없는 삽입 규약이 그대로 적용된다. */
	struct arm_smmu_cmdq cmdq;

	/* [한국어] 이 큐의 페이지0 레지스터가 매핑된 커널 주소.
	 * 설정자: init_lvcmdq() 가 VINTF 별 페이지 주소로 계산해 채운다.
	 * 읽는 자: 포인터와 오류 상태를 다루는 모든 곳.
	 * 값 범위: MMIO 창 안.
	 * 동기화: 불변. */
	void __iomem *page0;
	/* [한국어] 이 큐의 페이지1 레지스터가 매핑된 커널 주소.
	 * 설정자: 위와 같다.
	 * 읽는 자: 링 버퍼 주소를 쓸 때만 — 게스트에게는 열어 주지 않는 영역이다.
	 * 값 범위: MMIO 창 안.
	 * 동기화: 불변. */
	void __iomem *page1;
};
#define hw_queue_to_vcmdq(v) container_of(v, struct tegra241_vcmdq, core)	/* [한국어] iommufd 큐 객체에서 이 드라이버의 큐로 되짚는다. */

/**
 * struct tegra241_vintf - Virtual Interface
 * @vsmmu: Embedded arm_vsmmu structure
 * @idx: Global index in the CMDQV
 * @enabled: Enable status
 * @hyp_own: Owned by hypervisor (in-kernel)
 * @cmdqv: Parent CMDQV pointer
 * @lvcmdqs: List of logical VCMDQ pointers
 * @lvcmdq_mutex: Lock to serialize user-allocated lvcmdqs
 * @base: MMIO base address
 * @mmap_offset: Offset argument for mmap() syscall
 * @sids: Stream ID mapping resources
 */
/* [한국어] 가상 인터페이스 하나 — 격리의 단위다.
 *
 * VINTF 하나에 VMID 하나와 스트림 치환표 하나가 딸려 있고, 그 아래 논리
 * 큐들이 매달린다. 게스트에게 넘긴다는 것은 곧 VINTF 하나를 통째로 준다는
 * 뜻이며, 게스트가 무엇을 하든 그 VMID 와 치환표 밖으로 나갈 수 없다.
 * (위 영어 kernel-doc 참고) */
struct tegra241_vintf {
	/* [한국어] 표준 iommufd 연동이 아는 가상 SMMU 몸통 — 반드시 첫 필드다.
	 * 설정자: arm_vsmmu_init() 이 채운 뒤 이 파일의 초기화가 이어받는다.
	 * 읽는 자: viommu_to_vintf() 로 이 구조를 되찾는 모든 곳, 그리고
	 *         도메인 할당·무효화를 그대로 재사용하는 표준 코드.
	 * 값 범위: vsmmu.vmid 가 이 VINTF 에 걸릴 VMID 다.
	 * 동기화: iommufd 객체 수명 규칙. */
	struct arm_vsmmu vsmmu;

	/* [한국어] CMDQV 전체에서의 VINTF 번호.
	 * 설정자: init_vintf() 가 ida 에서 받아 채운다.
	 * 읽는 자: 레지스터 주소 계산과 로그.
	 * 값 범위: 0 은 커널 몫, 1 이상이 게스트 몫이다.
	 * 동기화: 배정 후 불변. */
	u16 idx;

	/* [한국어] 이 VINTF 가 켜져 있는가.
	 * 설정자: vintf_write_config() 가 하드웨어 상태를 확인한 뒤 기록한다.
	 * 읽는 자: 명령 큐를 고르는 자리에서 READ_ONCE 로 읽는다.
	 * 값 범위: true/false.
	 * 동기화: WRITE_ONCE/READ_ONCE. */
	bool enabled;
	/* [한국어] 이 VINTF 의 주인이 호스트 커널인가.
	 * 설정자: 하드웨어에 설정을 쓴 뒤 되읽어 확인한다 — 게스트 커널에서는
	 *         이 비트가 0 으로 고정되므로, 쓴 값이 아니라 읽은 값을 믿어야 한다.
	 * 읽는 자: 큐가 받을 수 있는 명령 종류를 정할 때, 그리고 해제 때
	 *         구조체를 누가 놓을지 가를 때.
	 * 값 범위: VINTF0 은 참, 게스트용은 거짓.
	 * 동기화: 초기화 때 한 번 정해진다. */
	bool hyp_own;

	/* [한국어] 이 VINTF 가 속한 CMDQV.
	 * 설정자: init_vintf().
	 * 읽는 자: 전역 레지스터와 하드웨어 한계값을 볼 때.
	 * 값 범위: NULL 일 수 없다.
	 * 동기화: 불변. */
	struct tegra241_cmdqv *cmdqv;
	/* [한국어] 이 VINTF 에 매달린 논리 큐들의 포인터 배열.
	 * 설정자: init_vintf() 가 배열을 잡고, 큐를 배정할 때마다 칸을 채운다.
	 * 읽는 자: 명령 큐 선택, 오류 처리, 켜고 끄는 순서 강제.
	 * 값 범위: 아직 배정되지 않은 칸은 NULL — 게스트용 VINTF 는 처음에 모두 NULL 이다.
	 * 동기화: 게스트용에서는 lvcmdq_mutex 아래에서만 바뀐다. */
	struct tegra241_vcmdq **lvcmdqs;
	struct mutex lvcmdq_mutex; /* user space race */
	/* [한국어] (위 영어 주석 참고) 게스트가 큐를 동시에 여러 개 요청할 때
	 * 배열이 엉키지 않게 지키는 락.
	 * 설정자: 게스트용 VINTF 초기화에서 mutex_init().
	 * 읽는 자: 큐를 배정·해제하는 두 경로.
	 * 값 범위: 커널용 VINTF0 에서는 쓰이지 않는다 — 경합이 없기 때문이다.
	 * 동기화: 이 필드 자체가 동기화 장치다. */

	/* [한국어] 이 VINTF 의 레지스터가 매핑된 커널 주소.
	 * 설정자: init_vintf() 가 계산해 채운다.
	 * 읽는 자: 설정·상태·치환표를 다루는 모든 곳.
	 * 값 범위: MMIO 창 안.
	 * 동기화: 불변. */
	void __iomem *base;
	/* [한국어] 게스트가 mmap() 으로 이 VINTF 의 페이지0 을 얻을 때 쓸 오프셋.
	 * 설정자: iommufd_viommu_alloc_mmap() 이 정해 준 값.
	 * 읽는 자: 사용자 공간에 돌려주고, 해제 때 그 매핑을 거둘 때.
	 * 값 범위: 0 이면 아직 매핑을 만들지 않았다.
	 * 동기화: 게스트용 VINTF 초기화·해제에서만 바뀐다. */
	unsigned long mmap_offset;

	/* [한국어] 스트림 치환표에서 빈 칸을 나눠 주는 번호 할당기.
	 * 설정자: 게스트용 VINTF 초기화에서 ida_init().
	 * 읽는 자: 게스트가 장치를 등록할 때마다 빈 칸 하나를 받아 간다.
	 * 값 범위: 0 ~ num_sids_per_vintf-1.
	 * 동기화: ida 자체가 내부 락을 갖는다. */
	struct ida sids;
};
#define viommu_to_vintf(v) container_of(v, struct tegra241_vintf, vsmmu.core)	/* [한국어] iommufd 가상 IOMMU 객체에서 이 드라이버의 VINTF 로 되짚는다. */

/**
 * struct tegra241_vintf_sid - Virtual Interface Stream ID Mapping
 * @core: Embedded iommufd_vdevice structure, holding virtual Stream ID
 * @vintf: Parent VINTF pointer
 * @sid: Physical Stream ID
 * @idx: Mapping index in the VINTF
 */
/* [한국어] 게스트 장치 하나에 대응하는 스트림 치환표 한 칸.
 *
 * 게스트가 "내 3번 장치"라고 말하면 하드웨어가 "실제로는 0x1200번 스트림"으로
 * 바꿔 읽게 해 주는 장치다. 이 대응이 없으면 게스트가 명령에 아무 번호나 적어
 * 남의 장치 캐시를 지울 수 있다. (위 영어 kernel-doc 참고) */
struct tegra241_vintf_sid {
	/* [한국어] iommufd 가 아는 가상 장치 몸통 — 반드시 첫 필드다.
	 * 설정자: iommufd 가 채우며, virt_id 에 게스트가 붙인 번호가 들어 있다.
	 * 읽는 자: vdev_to_vsid() 로 이 구조를 되찾는 모든 곳.
	 * 값 범위: virt_id 는 32비트를 넘을 수 없다 — 초기화에서 검사한다.
	 * 동기화: iommufd 객체 수명 규칙. */
	struct iommufd_vdevice core;
	/* [한국어] 이 대응이 속한 VINTF.
	 * 설정자: 대응을 만들 때 채운다.
	 * 읽는 자: 해제 때 치환표 레지스터를 지우고 번호를 돌려줄 때.
	 * 값 범위: NULL 일 수 없다.
	 * 동기화: 불변. */
	struct tegra241_vintf *vintf;
	/* [한국어] 실제 하드웨어 스트림 번호.
	 * 설정자: 장치의 master->streams[0].id 에서 가져온다.
	 * 읽는 자: SID_REPLACE 레지스터에 써 넣고, 로그에 찍는다.
	 * 값 범위: 지금은 스트림이 하나뿐인 PCI 장치만 지원한다.
	 * 동기화: 불변. */
	u32 sid;
	/* [한국어] 치환표에서 이 대응이 차지한 칸의 번호.
	 * 설정자: ida 에서 받아 채운다.
	 * 읽는 자: 레지스터 주소를 짚을 때와 해제할 때.
	 * 값 범위: 0 ~ num_sids_per_vintf-1.
	 * 동기화: 불변. */
	u8 idx;
};
#define vdev_to_vsid(v) container_of(v, struct tegra241_vintf_sid, core)	/* [한국어] iommufd 가상 장치 객체에서 이 드라이버의 대응 구조로 되짚는다. */

/**
 * struct tegra241_cmdqv - CMDQ-V for SMMUv3
 * @smmu: SMMUv3 device
 * @dev: CMDQV device
 * @base: MMIO base address
 * @base_phys: MMIO physical base address, for mmap
 * @irq: IRQ number
 * @num_vintfs: Total number of VINTFs
 * @num_vcmdqs: Total number of VCMDQs
 * @num_lvcmdqs_per_vintf: Number of logical VCMDQs per VINTF
 * @num_sids_per_vintf: Total number of SID mappings per VINTF
 * @vintf_ids: VINTF id allocator
 * @vintfs: List of VINTFs
 */
/* [한국어] 칩의 CMDQV 확장 하나 — 이 파일의 최상위 객체.
 *
 * 첫 필드가 arm_smmu_device 인 것이 설계의 핵심이다. 표준 드라이버는 이
 * 구조체를 평범한 SMMU 로 보고 그대로 다루고, 이 파일만 container_of 로
 * 되짚어 확장 정보에 닿는다. 그래서 표준 코드에 #ifdef 를 뿌리지 않아도 된다.
 * (위 영어 kernel-doc 참고) */
struct tegra241_cmdqv {
	/* [한국어] 표준 드라이버가 아는 SMMU 몸통 — 반드시 첫 필드여야 하며,
	 * 아래 static_assert 가 그것을 빌드 시점에 확인한다.
	 * 설정자: 표준 프로브가 채운 것을 devm_krealloc 으로 확장해 이어받는다.
	 * 읽는 자: 표준 드라이버 전체.
	 * 값 범위: 이 확장을 쓰지 않으면 이 구조체 자체가 만들어지지 않는다.
	 * 동기화: 표준 드라이버의 규칙을 따른다. */
	struct arm_smmu_device smmu;
	/* [한국어] CMDQV 자체의 플랫폼 장치.
	 * 설정자: 표준 프로브가 찾아 둔 smmu->impl_dev 를 옮겨 담는다.
	 * 읽는 자: 로그를 찍을 때와 참조를 놓을 때.
	 * 값 범위: NULL 일 수 없다.
	 * 동기화: 불변. 참조를 잡고 있으므로 이 구조체보다 먼저 사라지지 않는다. */
	struct device *dev;

	/* [한국어] CMDQV MMIO 창이 매핑된 커널 주소.
	 * 설정자: 프로브에서 ioremap().
	 * 읽는 자: 모든 레지스터 접근의 기준점.
	 * 값 범위: NULL 이면 프로브가 실패한 것이다.
	 * 동기화: 불변. */
	void __iomem *base;
	/* [한국어] 그 창의 물리 주소.
	 * 설정자: 프로브에서 자원 정보에서 가져온다.
	 * 읽는 자: 게스트에게 VINTF 페이지를 mmap 으로 넘길 때 —
	 *         커널 주소가 아니라 물리 주소가 필요하다.
	 * 값 범위: 자원이 알려 준 값.
	 * 동기화: 불변. */
	phys_addr_t base_phys;
	/* [한국어] 오류 인터럽트 번호.
	 * 설정자: 프로브에서 플랫폼 자원으로부터 얻는다.
	 * 읽는 자: 인터럽트를 등록하고 해제할 때.
	 * 값 범위: 0 이하면 인터럽트가 없다는 뜻 — 오류를 알 길이 없지만
	 *         동작 자체는 한다.
	 * 동기화: 불변. */
	int irq;

	/* CMDQV Hardware Params */
	/* [한국어] (위 영어 주석 참고) 아래 넷은 PARAM 레지스터에서 읽어 푼 값들이다. */
	/* [한국어] 이 칩의 VINTF 개수.
	 * 설정자: 프로브가 PARAM 의 로그값을 풀어 채운다.
	 * 읽는 자: VINTF 번호 배정 상한, 제거 때 훑을 범위.
	 * 값 범위: 2의 거듭제곱.
	 * 동기화: 불변. */
	u16 num_vintfs;
	/* [한국어] 이 칩의 전역 VCMDQ 개수.
	 * 설정자: 위와 같다.
	 * 읽는 자: VINTF 당 큐 수를 계산할 때.
	 * 값 범위: 2의 거듭제곱.
	 * 동기화: 불변. */
	u16 num_vcmdqs;
	/* [한국어] VINTF 하나가 가질 수 있는 논리 큐 수.
	 * 설정자: 전역 큐 수를 VINTF 수로 나눠 정한다.
	 * 읽는 자: CPU 번호로 큐를 고르는 나눗셈, 배열 크기, 순회 범위.
	 * 값 범위: 1 이상.
	 * 동기화: 불변. */
	u16 num_lvcmdqs_per_vintf;
	/* [한국어] VINTF 하나가 담을 수 있는 스트림 치환 항목 수.
	 * 설정자: PARAM 의 로그값을 풀어 채운다.
	 * 읽는 자: 게스트가 장치를 등록할 때 상한으로 쓰고, 해제 때 훑는 범위.
	 * 값 범위: 2의 거듭제곱. 이 수가 곧 한 게스트에 넘길 수 있는 장치 수다.
	 * 동기화: 불변. */
	u16 num_sids_per_vintf;

	/* [한국어] VINTF 번호를 나눠 주는 할당기.
	 * 설정자: 프로브에서 ida_init().
	 * 읽는 자: VINTF 를 만들 때마다 번호 하나를 받아 간다.
	 * 값 범위: 0 은 커널용으로 먼저 잡히고, 나머지가 게스트 몫이다.
	 * 동기화: ida 내부 락. */
	struct ida vintf_ids;

	/* [한국어] 번호로 VINTF 를 찾는 포인터 배열.
	 * 설정자: 프로브가 배열을 잡고, VINTF 를 만들 때마다 칸을 채운다.
	 * 읽는 자: 인터럽트 처리가 오류 비트맵의 번호로 VINTF 를 찾을 때,
	 *         그리고 명령 큐를 고를 때 VINTF0 을 짚을 때.
	 * 값 범위: 비어 있는 칸은 NULL.
	 * 동기화: 만들기·지우기는 프로브와 iommufd ioctl 경로에서만 일어나고,
	 *         읽기는 인터럽트에서도 일어난다 — 배열 자체는 불변이고
	 *         칸의 값만 바뀐다. */
	struct tegra241_vintf **vintfs;
};

/* Config and Polling Helpers */

/*
 * [한국어]
 * tegra241_cmdqv_write_config - 설정을 쓰고 하드웨어가 따라올 때까지 기다린다
 *
 * @cmdqv: 로그를 찍을 때 쓸 CMDQV.
 * @addr_config: 설정을 쓸 레지스터 주소.
 * @addr_status: 그 결과를 확인할 상태 레지스터 주소.
 * @regval: 쓸 값. 비트 0 이 곧 "켬/끔"이다.
 * @header: 로그 앞에 붙일 머리말 (어느 계층인지 알리려고).
 * @out_enabled: 실제 결과를 기록해 둘 자리 (필요 없으면 NULL).
 * @return: 0 성공, -ETIMEDOUT 하드웨어가 따라오지 않음.
 *
 * CMDQV·VINTF·VCMDQ 세 계층 모두 "설정 레지스터에 쓰고 상태 레지스터를
 * 폴링한다"는 같은 규약을 쓴다. 그 공통 부분을 이 함수 하나로 묶었다.
 * 켜는 요청이면 상태 비트가 서고, 끄는 요청이면 상태 비트가 내려가기를
 * 기다린다 — 요청 방향에 따라 기다릴 조건이 뒤집히는 것이 요점이다.
 *
 * 결과를 out_enabled 에 적어 두는 이유는, 하드웨어가 요청을 거부했을 때
 * 소프트웨어가 "켜졌다"고 잘못 믿으면 안 되기 때문이다. 그래서 쓴 값이
 * 아니라 되읽은 값을 기록한다.
 *
 * 실행 컨텍스트: 프로브·리셋·게스트 요청 처리. 폴링 중에는 잠들지 않는다.
 *
 * 호출 체인:
 *   cmdqv/vintf/vcmdq_write_config() → [이 함수] → readl_poll_timeout()
 */
static inline int tegra241_cmdqv_write_config(struct tegra241_cmdqv *cmdqv,
					      void __iomem *addr_config,
					      void __iomem *addr_status,
					      u32 regval, const char *header,
					      bool *out_enabled)
{
	bool en = regval & BIT(0);	/* [한국어] 세 계층 모두 비트 0 이 켬/끔이라는 규약을 공유한다. */
	int ret;	/* [한국어] 폴링 결과. */

	writel(regval, addr_config);	/* [한국어] 설정을 쓴다. relaxed 가 아닌 writel 이라 앞선 메모리 접근이 먼저 보이는 것이 보장된다. */
	ret = readl_poll_timeout(addr_status, regval,	/* [한국어] 상태 레지스터를 반복해 읽으며 조건이 맞기를 기다린다. regval 을 재사용해 읽은 값을 담는다. */
				 en ? regval & BIT(0) : !(regval & BIT(0)),	/* [한국어] 켜는 요청이면 비트가 서기를, 끄는 요청이면 내려가기를 기다린다. */
				 1, ARM_SMMU_POLL_TIMEOUT_US);	/* [한국어] 1마이크로초 간격으로, 표준 드라이버와 같은 시간만큼 기다린다. */
	if (ret)	/* [한국어] 시간이 다 됐다면 하드웨어가 요청을 받아들이지 못한 것이다. */
		dev_err(cmdqv->dev, "%sfailed to %sable, STATUS=0x%08X\n",	/* [한국어] 어느 계층에서 무엇을 하려다 실패했는지 남긴다 — 머리말이 그 구분을 맡는다. */
			header, en ? "en" : "dis", regval);
	if (out_enabled)	/* [한국어] 호출자가 결과를 기억하고 싶어 하는 경우에만. */
		WRITE_ONCE(*out_enabled, regval & BIT(0));	/* [한국어] 되읽은 값을 기록한다 — 쓴 값이 아니라는 점이 중요하다. 읽는 쪽이 다른 CPU 라 WRITE_ONCE 로 쓴다. */
	return ret;	/* [한국어] 실패를 그대로 위로 올린다. */
}

/*
 * [한국어]
 * cmdqv_write_config - CMDQV 전체를 켜거나 끈다
 *
 * @cmdqv: 대상 CMDQV.
 * @regval: 쓸 설정 값.
 * @return: 0 성공, 음수 실패.
 *
 * 공통 헬퍼에 전역 레지스터 주소와 머리말만 채워 넘기는 얇은 껍데기다.
 * 결과를 기억할 자리는 넘기지 않는다 — 전역 상태는 리셋 경로에서만
 * 다루므로 따로 기억할 필요가 없다.
 *
 * 실행 컨텍스트: 프로브·리셋. 잠들지 않는다.
 *
 * 호출 체인:
 *   tegra241_cmdqv_hw_reset() → [이 함수]
 */
static inline int cmdqv_write_config(struct tegra241_cmdqv *cmdqv, u32 regval)
{
	return tegra241_cmdqv_write_config(cmdqv,	/* [한국어] 공통 규약을 그대로 쓴다. */
					   REG_CMDQV(cmdqv, CONFIG),	/* [한국어] 전역 설정 레지스터. */
					   REG_CMDQV(cmdqv, STATUS),	/* [한국어] 전역 상태 레지스터. */
					   regval, "CMDQV: ", NULL);	/* [한국어] 로그 머리말로 계층을 알린다. 상태를 기억할 자리는 없다. */
}

/*
 * [한국어]
 * vintf_write_config - VINTF 하나를 켜거나 끈다
 *
 * @vintf: 대상 VINTF.
 * @regval: 쓸 설정 값 (VMID 와 주인 표시가 함께 들어 있다).
 * @return: 0 성공, 음수 실패.
 *
 * 결과를 vintf->enabled 에 기록하는 것이 전역 판과 다른 점이다. 명령 큐를
 * 고르는 경로가 그 값을 보고 "이 VINTF 를 써도 되는가"를 판단하기 때문이다.
 *
 * 실행 컨텍스트: 리셋·초기화·해제. 잠들지 않는다.
 *
 * 호출 체인:
 *   tegra241_vintf_hw_init()/hw_deinit() → [이 함수]
 */
static inline int vintf_write_config(struct tegra241_vintf *vintf, u32 regval)
{
	char header[16];	/* [한국어] "VINTF3: " 같은 짧은 머리말을 담을 자리. */

	snprintf(header, 16, "VINTF%u: ", vintf->idx);	/* [한국어] 로그에서 어느 VINTF 인지 구분할 수 있게 번호를 넣는다. */
	return tegra241_cmdqv_write_config(vintf->cmdqv,	/* [한국어] 공통 규약을 그대로 쓰되 VINTF 레지스터를 짚는다. */
					   REG_VINTF(vintf, CONFIG),	/* [한국어] 이 VINTF 의 설정 레지스터. */
					   REG_VINTF(vintf, STATUS),	/* [한국어] 이 VINTF 의 상태 레지스터. */
					   regval, header, &vintf->enabled);	/* [한국어] 결과를 기억해 둔다 — 큐 선택 경로가 이 값을 읽는다. */
}

/*
 * [한국어]
 * lvcmdq_error_header - 로그에 붙일 "어느 큐인지" 머리말을 짓는다
 *
 * @vcmdq: 대상 큐.
 * @header: 문자열을 담을 버퍼.
 * @hlen: 그 버퍼의 크기.
 * @return: 채워진 버퍼, 또는 아직 VINTF 에 매달리지 않았으면 빈 문자열.
 *
 * 큐는 전역 번호와 지역 번호를 둘 다 갖고 어느 VINTF 에 속하는지도 중요해서,
 * 로그 한 줄만 봐도 그 셋을 다 알 수 있게 머리말을 짓는다. 큐가 많은
 * 하드웨어에서 문제를 좁히려면 이런 흔적이 꼭 필요하다.
 *
 * 아직 VINTF 에 매달리지 않은 큐를 넘기면 경고를 남기고 빈 문자열을 준다 —
 * 로그를 찍으려다 널 포인터를 밟는 일을 막는다.
 *
 * 실행 컨텍스트: 어디서나. 잠들지 않는다.
 *
 * 호출 체인:
 *   큐를 다루는 거의 모든 함수 → [이 함수]
 */
static inline char *lvcmdq_error_header(struct tegra241_vcmdq *vcmdq,
					char *header, int hlen)
{
	WARN_ON(hlen < 64);	/* [한국어] 아래 문자열이 잘리지 않을 만큼 넉넉한지 확인한다 — 잘린 로그는 오해를 부른다. */
	if (WARN_ON(!vcmdq->vintf))	/* [한국어] VINTF 에 매달리기 전의 큐라면 번호를 알 수 없다. */
		return "";	/* [한국어] 빈 머리말을 돌려줘 로그는 찍히되 크래시는 나지 않게 한다. */
	snprintf(header, hlen, "VINTF%u: VCMDQ%u/LVCMDQ%u: ",	/* [한국어] VINTF 번호, 전역 큐 번호, 지역 큐 번호를 모두 담는다. */
		 vcmdq->vintf->idx, vcmdq->idx, vcmdq->lidx);
	return header;	/* [한국어] 호출자가 그대로 %s 에 넣어 쓴다. */
}

/*
 * [한국어]
 * vcmdq_write_config - 논리 명령 큐 하나를 켜거나 끈다
 *
 * @vcmdq: 대상 큐.
 * @regval: 쓸 설정 값.
 * @return: 0 성공, 음수 실패.
 *
 * 결과를 vcmdq->enabled 에 기록한다 — 큐를 고르는 경로가 꺼진 큐를 피하려면
 * 이 값이 필요하다. 로그 머리말은 큐 번호까지 담아 문제를 좁히기 쉽게 한다.
 *
 * 실행 컨텍스트: 초기화·해제. 잠들지 않는다.
 *
 * 호출 체인:
 *   tegra241_vcmdq_hw_init()/hw_deinit() → [이 함수]
 */
static inline int vcmdq_write_config(struct tegra241_vcmdq *vcmdq, u32 regval)
{
	char header[64], *h = lvcmdq_error_header(vcmdq, header, 64);	/* [한국어] 로그 머리말을 미리 지어 둔다. 실패했을 때 곧바로 쓸 수 있게. */

	return tegra241_cmdqv_write_config(vcmdq->cmdqv,	/* [한국어] 공통 규약을 그대로 쓰되 큐의 페이지0 레지스터를 짚는다. */
					   REG_VCMDQ_PAGE0(vcmdq, CONFIG),	/* [한국어] 큐의 설정 레지스터는 페이지0 에 있다. */
					   REG_VCMDQ_PAGE0(vcmdq, STATUS),	/* [한국어] 상태 레지스터도 같은 페이지. */
					   regval, h, &vcmdq->enabled);	/* [한국어] 결과를 기억해 둔다. */
}

/* ISR Functions */

/*
 * [한국어]
 * tegra241_vintf_user_handle_error - 게스트 VINTF 의 오류를 게스트에게 올린다
 *
 * @vintf: 오류가 난 게스트용 VINTF.
 *
 * 게스트가 자기 큐에 잘못된 명령을 넣으면 하드웨어가 오류를 낸다. 그 오류는
 * 게스트의 실수이므로 게스트가 처리해야 하며, 호스트가 대신 고쳐 줄 수도
 * 없다. 그래서 오류 비트맵을 그대로 읽어 iommufd 의 사건 큐에 올리고,
 * VMM 을 거쳐 게스트 커널이 자기 오류로 보게 만든다.
 *
 * 호스트가 오류를 지우지 않는다는 점이 커널 VINTF 처리와 다르다 — 지우는
 * 일도 게스트의 몫이다.
 *
 * 실행 컨텍스트: 인터럽트 스레드. 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra241_cmdqv_isr() → [이 함수] → iommufd_viommu_report_event()
 */
static void tegra241_vintf_user_handle_error(struct tegra241_vintf *vintf)
{
	struct iommufd_viommu *viommu = &vintf->vsmmu.core;	/* [한국어] 사건을 올릴 iommufd 객체. */
	struct iommu_vevent_tegra241_cmdqv vevent_data;	/* [한국어] 게스트에게 전달할 사건 기록. */
	int i;	/* [한국어] 비트맵 반복자. */

	for (i = 0; i < LVCMDQ_ERR_MAP_NUM_64; i++) {	/* [한국어] 오류 비트맵 두 개를 모두 읽는다. */
		u64 err = readq_relaxed(REG_VINTF(vintf, LVCMDQ_ERR_MAP_64(i)));	/* [한국어] 어느 논리 큐에서 오류가 났는지 비트로 알려 준다. */

		vevent_data.lvcmdq_err_map[i] = cpu_to_le64(err);	/* [한국어] 사용자 공간과 주고받는 형식(리틀엔디안)으로 담는다. */
	}

	iommufd_viommu_report_event(viommu, IOMMU_VEVENTQ_TYPE_TEGRA241_CMDQV,	/* [한국어] 게스트의 사건 큐에 넣는다 — 형식 번호로 이 확장의 사건임을 알린다. */
				    &vevent_data, sizeof(vevent_data));
}

/*
 * [한국어]
 * tegra241_vintf0_handle_error - 커널 VINTF 의 오류를 직접 처리한다
 *
 * @vintf: VINTF0 (커널이 쓰는 것).
 *
 * 커널이 낸 명령에서 오류가 났다면 커널이 스스로 수습해야 한다. 표준
 * 드라이버의 오류 처리(__arm_smmu_cmdq_skip_err)를 그대로 불러 걸려 넘어진
 * 명령을 건너뛰게 하고, 그다음 오류 표시를 지워 큐를 다시 굴린다.
 *
 * 비트맵을 훑으며 __ffs64 로 가장 낮은 비트부터 처리하고 그 비트를 지우는
 * 방식은, 오류가 난 큐만 골라 다루면서도 순회를 최소화하는 흔한 관용구다.
 *
 * 실행 컨텍스트: 인터럽트 스레드. 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra241_cmdqv_isr() → [이 함수] → __arm_smmu_cmdq_skip_err()
 */
static void tegra241_vintf0_handle_error(struct tegra241_vintf *vintf)
{
	int i;	/* [한국어] 비트맵 반복자. */

	for (i = 0; i < LVCMDQ_ERR_MAP_NUM_64; i++) {	/* [한국어] 오류 비트맵 두 개를 모두 본다. */
		u64 map = readq_relaxed(REG_VINTF(vintf, LVCMDQ_ERR_MAP_64(i)));	/* [한국어] 오류가 난 큐들의 비트맵. */

		while (map) {	/* [한국어] 비트가 하나라도 남아 있는 동안. */
			unsigned long lidx = __ffs64(map);	/* [한국어] 가장 낮은 비트의 자리 — 곧 논리 큐 번호다. */
			struct tegra241_vcmdq *vcmdq = vintf->lvcmdqs[lidx];	/* [한국어] 그 번호의 큐를 찾는다. */
			u32 gerror = readl_relaxed(REG_VCMDQ_PAGE0(vcmdq, GERROR));	/* [한국어] 그 큐의 오류 비트들을 읽어 둔다. */

			__arm_smmu_cmdq_skip_err(&vintf->cmdqv->smmu, &vcmdq->cmdq);	/* [한국어] 걸려 넘어진 명령을 CMD_SYNC 로 덮어써 큐를 다시 굴린다 — 표준 드라이버의 처리를 그대로 쓴다. */
			writel(gerror, REG_VCMDQ_PAGE0(vcmdq, GERRORN));	/* [한국어] 읽은 값을 그대로 되쓰면 그 오류들이 확인됨으로 표시된다. */
			map &= ~BIT_ULL(lidx);	/* [한국어] 처리한 비트를 지우고 다음으로 넘어간다. */
		}
	}
}

/*
 * [한국어]
 * tegra241_cmdqv_isr - CMDQV 오류 인터럽트를 처리한다
 *
 * @irq: 인터럽트 번호 (쓰지 않는다).
 * @devid: 등록할 때 넘긴 CMDQV 포인터.
 * @return: 항상 IRQ_HANDLED.
 *
 * 오류는 세 겹의 비트맵으로 보고된다 — 어느 VINTF 인지, 그 안의 어느 큐인지,
 * 그리고 무슨 오류인지. 이 함수는 첫 겹을 풀어 VINTF 별로 갈래를 나눈다.
 * VINTF0 은 커널 몫이라 직접 수습하고, 나머지는 게스트 몫이라 그대로 올린다.
 *
 * 오류 비트맵을 먼저 통째로 로그에 남기는 것도 중요하다 — 처리 과정에서
 * 비트가 지워지므로, 나중에 문제를 되짚으려면 원본이 필요하다.
 *
 * VINTF_ERR_MAP 을 64비트가 아니라 32비트 두 번으로 읽는 이유는 주석대로
 * 레지스터 주소가 8바이트 정렬이 아니기 때문이다.
 *
 * 실행 컨텍스트: 스레드로 도는 인터럽트 처리기(IRQF_ONESHOT). 잠들 수 있다.
 *
 * 호출 체인:
 *   하드웨어 인터럽트 → [이 함수]
 *     → tegra241_vintf0_handle_error() / tegra241_vintf_user_handle_error()
 */
static irqreturn_t tegra241_cmdqv_isr(int irq, void *devid)
{
	struct tegra241_cmdqv *cmdqv = (struct tegra241_cmdqv *)devid;	/* [한국어] 등록할 때 넘겨 둔 문맥. */
	void __iomem *reg_vintf_map = REG_CMDQV(cmdqv, VINTF_ERR_MAP);	/* [한국어] 어느 VINTF 에서 오류가 났는지 알려 주는 비트맵의 주소. */
	char err_str[256];	/* [한국어] 로그 한 줄로 모아 찍기 위한 버퍼. */
	u64 vintf_map;	/* [한국어] 그 비트맵 값. */

	/* Use readl_relaxed() as register addresses are not 64-bit aligned */
	/* [한국어] (위 영어 주석 참고) 64비트 읽기를 쓰려면 주소가 8바이트 정렬이어야
	 * 하는데 이 레지스터는 그렇지 않다. 그래서 32비트씩 두 번 읽어 붙인다. */
	vintf_map = (u64)readl_relaxed(reg_vintf_map + 0x4) << 32 |	/* [한국어] 위쪽 절반을 먼저 읽어 자리를 올리고. */
		    (u64)readl_relaxed(reg_vintf_map);	/* [한국어] 아래쪽 절반을 붙인다. */

	snprintf(err_str, sizeof(err_str),	/* [한국어] 처리로 비트가 지워지기 전에 원본을 문자열로 굳혀 둔다. */
		 "vintf_map: %016llx, vcmdq_map %08x:%08x:%08x:%08x", vintf_map,
		 readl_relaxed(REG_CMDQV(cmdqv, CMDQ_ERR_MAP(3))),	/* [한국어] 큐 오류 비트맵 네 개를 높은 쪽부터 찍는다. */
		 readl_relaxed(REG_CMDQV(cmdqv, CMDQ_ERR_MAP(2))),
		 readl_relaxed(REG_CMDQV(cmdqv, CMDQ_ERR_MAP(1))),
		 readl_relaxed(REG_CMDQV(cmdqv, CMDQ_ERR_MAP(0))));

	dev_warn(cmdqv->dev, "unexpected error reported. %s\n", err_str);	/* [한국어] 이 인터럽트가 오는 것 자체가 정상이 아니므로 경고로 남긴다. */

	/* Handle VINTF0 and its LVCMDQs */
	/* [한국어] (위 영어 주석 참고) 0번은 커널 몫이라 처리 방식이 다르다. */
	if (vintf_map & BIT_ULL(0)) {	/* [한국어] 커널 VINTF 에서 오류가 났는가. */
		tegra241_vintf0_handle_error(cmdqv->vintfs[0]);	/* [한국어] 커널이 직접 수습한다. */
		vintf_map &= ~BIT_ULL(0);	/* [한국어] 처리했으니 비트를 내린다 — 아래 반복문이 다시 잡지 않게. */
	}

	/* Handle other user VINTFs and their LVCMDQs */
	/* [한국어] (위 영어 주석 참고) 나머지는 모두 게스트 몫이다. */
	while (vintf_map) {	/* [한국어] 남은 비트가 있는 동안. */
		unsigned long idx = __ffs64(vintf_map);	/* [한국어] 가장 낮은 비트의 자리 — VINTF 번호다. */

		tegra241_vintf_user_handle_error(cmdqv->vintfs[idx]);	/* [한국어] 그 게스트에게 오류를 그대로 올린다. */
		vintf_map &= ~BIT_ULL(idx);	/* [한국어] 처리한 비트를 지운다. */
	}

	return IRQ_HANDLED;	/* [한국어] 이 인터럽트는 우리 것이 맞다고 알린다. */
}

/* Command Queue Function */

/*
 * [한국어]
 * tegra241_guest_vcmdq_supports_cmd - 게스트 큐가 받을 수 있는 명령인가
 *
 * @ent: 넣으려는 명령.
 * @return: 그 큐로 보내도 되면 참.
 *
 * 게스트 소유로 설정된 큐(HYP_OWN 이 꺼진 큐)는 하드웨어가 받아들이는 명령의
 * 종류가 제한된다 — 무효화 계열만 가능하고, 스트림 표를 건드리는 설정 명령은
 * 받지 않는다. 그래야 게스트가 자기 몫을 벗어나지 못한다.
 * 여기 걸리지 않는 명령은 호출자가 표준 SMMU 명령 큐로 돌려보낸다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 원자적 문맥일 수 있어 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_supports_cmd() → [이 함수]
 */
static bool tegra241_guest_vcmdq_supports_cmd(struct arm_smmu_cmdq_ent *ent)
{
	switch (ent->opcode) {	/* [한국어] 명령 종류만 보고 판단한다. */
	case CMDQ_OP_TLBI_NH_ASID:	/* [한국어] ASID 단위 TLB 무효화 — 게스트 자기 주소 공간의 일이다. */
	case CMDQ_OP_TLBI_NH_VA:	/* [한국어] 주소 범위 TLB 무효화. */
	case CMDQ_OP_ATC_INV:	/* [한국어] 장치 캐시 무효화 — 스트림 번호는 치환표가 걸러 준다. */
		return true;	/* [한국어] 이 셋은 가속 큐로 보내도 안전하다. */
	default:	/* [한국어] 설정 무효화(CFGI)나 페이지 응답 등. */
		return false;	/* [한국어] 게스트 큐로는 보낼 수 없다 — 호출자가 표준 큐로 돌린다. */
	}
}

/*
 * [한국어]
 * tegra241_cmdqv_get_cmdq - 이번 명령을 넣을 큐를 고른다
 *
 * @smmu: 표준 드라이버가 넘긴 SMMU (실제로는 CMDQV 를 품고 있다).
 * @ent: 넣으려는 명령.
 * @return: 쓸 논리 큐, 또는 NULL 이면 표준 큐를 쓰라는 뜻.
 *
 * 이 확장이 성능을 내는 지점이다. 표준 드라이버는 큐가 하나뿐이라 모든
 * CPU 가 같은 락을 두고 다투는데, 여기서는 CPU 번호로 큐를 나눠 주므로
 * 서로 다른 CPU 는 서로 다른 큐에 동시에 명령을 넣을 수 있다.
 *
 * NULL 을 돌려주는 세 갈래가 모두 "이번에는 가속을 쓰지 말라"는 뜻이며,
 * 호출자는 그때 표준 큐로 돌아간다 — 디버깅용으로 껐거나, VINTF0 이 아직
 * 준비되지 않았거나, 고른 큐가 그 명령을 받지 못하는 경우다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 원자적 문맥일 수 있어 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_issue_cmdlist() → impl_ops->get_secondary_cmdq = [이 함수]
 */
static struct arm_smmu_cmdq *
tegra241_cmdqv_get_cmdq(struct arm_smmu_device *smmu,
			struct arm_smmu_cmdq_ent *ent)
{
	struct tegra241_cmdqv *cmdqv =	/* [한국어] 표준 SMMU 를 품은 확장 구조체로 되짚는다. */
		container_of(smmu, struct tegra241_cmdqv, smmu);
	struct tegra241_vintf *vintf = cmdqv->vintfs[0];	/* [한국어] 커널은 언제나 VINTF0 만 쓴다. */
	struct tegra241_vcmdq *vcmdq;	/* [한국어] 고를 큐. */
	u16 lidx;	/* [한국어] 그 큐의 지역 번호. */

	if (READ_ONCE(bypass_vcmdq))	/* [한국어] debugfs 로 실행 중에 바뀔 수 있어 READ_ONCE 로 읽는다. */
		return NULL;	/* [한국어] 표준 큐를 쓰라고 알린다 — 성능 비교와 문제 가르기에 쓴다. */

	/* Use SMMU CMDQ if VINTF0 is uninitialized */
	/* [한국어] (위 영어 주석 참고) 프로브 도중에는 아직 VINTF0 이 켜지지 않았는데,
	 * 그때도 명령을 낼 일이 있다. 그런 명령은 표준 큐로 보낸다. */
	if (!READ_ONCE(vintf->enabled))	/* [한국어] 다른 CPU 가 방금 켰거나 끌 수 있어 READ_ONCE 로 읽는다. */
		return NULL;	/* [한국어] 아직 쓸 수 없다. */

	/*
	 * Select a LVCMDQ to use. Here we use a temporal solution to
	 * balance out traffic on cmdq issuing: each cmdq has its own
	 * lock, if all cpus issue cmdlist using the same cmdq, only
	 * one CPU at a time can enter the process, while the others
	 * will be spinning at the same lock.
	 */
	/* [한국어] (위 영어 주석 참고) 큐마다 자기 자리 다툼이 따로 있으므로, CPU 를
	 * 큐에 흩어 놓기만 해도 경합이 크게 준다. CPU 번호를 큐 수로 나눈 나머지를
	 * 쓰는 것은 가장 단순한 분산이며, 주석이 말하듯 임시 해법이다 —
	 * 큐마다 부하가 다르면 고르지 않을 수 있다. */
	lidx = raw_smp_processor_id() % cmdqv->num_lvcmdqs_per_vintf;	/* [한국어] 선점될 수 있는 문맥이라 raw_ 판을 쓴다 — 여기서는 정확한 CPU 번호가 아니라 분산만이 목적이다. */
	vcmdq = vintf->lvcmdqs[lidx];	/* [한국어] 그 번호의 큐를 꺼낸다. */
	if (!vcmdq || !READ_ONCE(vcmdq->enabled))	/* [한국어] 아직 배정되지 않았거나 꺼진 큐라면. */
		return NULL;	/* [한국어] 표준 큐로 돌린다. */

	/* Unsupported CMD goes for smmu->cmdq pathway */
	/* [한국어] (위 영어 주석 참고) 게스트 소유 큐는 받는 명령이 제한된다.
	 * 커널 VINTF0 의 큐는 보통 모든 명령을 받지만, 검사를 한 번 거친다. */
	if (!arm_smmu_cmdq_supports_cmd(&vcmdq->cmdq, ent))	/* [한국어] 이 큐가 이 명령을 받을 수 있는가. */
		return NULL;	/* [한국어] 못 받으면 표준 큐로 돌린다. */
	return &vcmdq->cmdq;	/* [한국어] 이 큐에 넣으라고 표준 코드에 돌려준다. */
}

/* HW Reset Functions */

/*
 * When a guest-owned VCMDQ is disabled, if the guest did not enqueue a CMD_SYNC
 * following an ATC_INV command at the end of the guest queue while this ATC_INV
 * is timed out, the TIMEOUT will not be reported until this VCMDQ gets assigned
 * to the next VM, which will be a false alarm potentially causing some unwanted
 * behavior in the new VM. Thus, a guest-owned VCMDQ must flush the TIMEOUT when
 * it gets disabled. This can be done by just issuing a CMD_SYNC to SMMU CMDQ.
 */
/*
 * [한국어]
 * tegra241_vcmdq_hw_flush_timeout - 큐를 끄기 전에 남은 시간 초과를 흘려보낸다
 *
 * @vcmdq: 끄려는 큐.
 *
 * (위 영어 주석 참고) 게스트가 ATC_INV 를 내고 CMD_SYNC 로 마무리하지 않은 채
 * 큐가 꺼지면, 그 무효화의 시간 초과가 보고되지 않고 하드웨어에 남는다.
 * 그 큐를 다음 가상 머신에 넘기면 남아 있던 시간 초과가 그제야 터지는데,
 * 새 가상 머신 입장에서는 자기가 하지도 않은 일로 오류를 받는 셈이다.
 * 그래서 큐를 끌 때 표준 SMMU 큐에 CMD_SYNC 를 하나 흘려보내, 남아 있던
 * 시간 초과가 지금 여기서 정리되게 만든다.
 *
 * 표준 큐에 넣는 것이 요점이다 — 끄려는 큐에는 더 이상 명령을 넣을 수 없고,
 * 완료를 기다려 주는 표준 경로가 마침 필요한 일을 해 준다.
 *
 * 실행 컨텍스트: 큐 해제 경로. 완료를 기다리므로 시간이 걸릴 수 있다.
 *
 * 호출 체인:
 *   tegra241_vcmdq_hw_deinit() → [이 함수] → arm_smmu_cmdq_issue_cmdlist()
 */
static void tegra241_vcmdq_hw_flush_timeout(struct tegra241_vcmdq *vcmdq)
{
	struct arm_smmu_device *smmu = &vcmdq->cmdqv->smmu;	/* [한국어] 표준 명령 큐를 가진 SMMU. */
	u64 cmd_sync[CMDQ_ENT_DWORDS] = {};	/* [한국어] 명령 하나를 담을 버퍼. 0 으로 채워 시작한다. */

	cmd_sync[0] = FIELD_PREP(CMDQ_0_OP, CMDQ_OP_CMD_SYNC) |	/* [한국어] "앞선 명령이 모두 끝나기를 기다려라"는 명령을 짓는다. */
		      FIELD_PREP(CMDQ_SYNC_0_CS, CMDQ_SYNC_0_CS_NONE);	/* [한국어] 완료를 알리는 방식은 없음 — 아래 발행 함수가 폴링으로 기다린다. */

	/*
	 * It does not hurt to insert another CMD_SYNC, taking advantage of the
	 * arm_smmu_cmdq_issue_cmdlist() that waits for the CMD_SYNC completion.
	 */
	/* [한국어] (위 영어 주석 참고) CMD_SYNC 는 아무 상태도 바꾸지 않으므로 한 번
	 * 더 넣어도 해가 없다. 발행 함수가 완료까지 기다려 주는 성질을 빌려 쓰는 것이
	 * 목적이며, 그 기다림 동안 남아 있던 시간 초과가 정리된다. */
	arm_smmu_cmdq_issue_cmdlist(smmu, &smmu->cmdq, cmd_sync, 1, true);	/* [한국어] 표준 큐에 넣고 완료까지 기다린다. */
}

/* This function is for LVCMDQ, so @vcmdq must not be unmapped yet */
/*
 * [한국어]
 * tegra241_vcmdq_hw_deinit - 큐를 끄고 하드웨어 상태를 깨끗이 되돌린다
 *
 * @vcmdq: 끌 큐.
 *
 * (위 영어 주석 참고) 아직 전역 큐가 이 논리 큐에 배정된 상태여야 한다 —
 * 배정을 풀면 레지스터에 접근할 수 없기 때문이다. 그래서 해제 순서는 늘
 * "끄기 → 배정 풀기"다.
 *
 * 하는 일은 셋이다. 큐를 끄고, 남은 시간 초과를 흘려보내고, 포인터와 링
 * 버퍼 주소를 0 으로 되돌린다. 마지막으로 확인되지 않은 오류가 남아 있으면
 * 지운다 — 그대로 두면 다음 주인이 남의 오류를 보게 된다.
 *
 * 실행 컨텍스트: 초기화·해제 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra241_vcmdq_hw_init()/vintf_hw_deinit()/destroy_lvcmdq_user()
 *     → [이 함수] → vcmdq_write_config(), tegra241_vcmdq_hw_flush_timeout()
 */
static void tegra241_vcmdq_hw_deinit(struct tegra241_vcmdq *vcmdq)
{
	char header[64], *h = lvcmdq_error_header(vcmdq, header, 64);	/* [한국어] 로그 머리말을 미리 지어 둔다. */
	u32 gerrorn, gerror;	/* [한국어] 확인된 오류와 실제 오류 — 둘이 다르면 미처리 오류가 있다는 뜻이다. */

	if (vcmdq_write_config(vcmdq, 0)) {	/* [한국어] 큐를 끈다. 실패하면 하드웨어가 멈춰 있는 것이다. */
		dev_err(vcmdq->cmdqv->dev,	/* [한국어] 왜 못 껐는지 진단할 수 있게 오류 레지스터를 함께 남긴다. */
			"%sGERRORN=0x%X, GERROR=0x%X, CONS=0x%X\n", h,
			readl_relaxed(REG_VCMDQ_PAGE0(vcmdq, GERRORN)),	/* [한국어] 확인된 오류. */
			readl_relaxed(REG_VCMDQ_PAGE0(vcmdq, GERROR)),	/* [한국어] 실제 오류. */
			readl_relaxed(REG_VCMDQ_PAGE0(vcmdq, CONS)));	/* [한국어] 소비 포인터 — 어느 명령에서 멈췄는지 알려 준다. */
	}
	tegra241_vcmdq_hw_flush_timeout(vcmdq);	/* [한국어] 남아 있을지 모를 시간 초과를 지금 정리한다 — 다음 주인에게 넘기지 않기 위해서다. */

	writel_relaxed(0, REG_VCMDQ_PAGE0(vcmdq, PROD));	/* [한국어] 생산 포인터를 0 으로. */
	writel_relaxed(0, REG_VCMDQ_PAGE0(vcmdq, CONS));	/* [한국어] 소비 포인터도 0 으로 — 다음 주인이 깨끗한 큐를 받게 한다. */
	writeq_relaxed(0, REG_VCMDQ_PAGE1(vcmdq, BASE));	/* [한국어] 링 버퍼 주소를 지운다. 남겨 두면 다음 주인이 남의 메모리를 가리키게 된다. */
	writeq_relaxed(0, REG_VCMDQ_PAGE1(vcmdq, CONS_INDX_BASE));	/* [한국어] 소비 포인터 복사 주소도 같은 이유로 지운다. */

	gerrorn = readl_relaxed(REG_VCMDQ_PAGE0(vcmdq, GERRORN));	/* [한국어] 확인된 오류 카운터. */
	gerror = readl_relaxed(REG_VCMDQ_PAGE0(vcmdq, GERROR));	/* [한국어] 실제 오류 카운터. */
	if (gerror != gerrorn) {	/* [한국어] 둘이 다르면 아직 아무도 확인하지 않은 오류가 있다는 뜻이다. */
		dev_warn(vcmdq->cmdqv->dev,	/* [한국어] 조용히 지우면 오류가 있었다는 사실 자체가 사라지므로 경고를 남긴다. */
			 "%suncleared error detected, resetting\n", h);
		writel(gerror, REG_VCMDQ_PAGE0(vcmdq, GERRORN));	/* [한국어] 실제 값을 확인 레지스터에 그대로 써 오류를 지운다. */
	}

	dev_dbg(vcmdq->cmdqv->dev, "%sdeinited\n", h);	/* [한국어] 큐 수명을 추적할 수 있게 디버그 로그를 남긴다. */
}

/* This function is for LVCMDQ, so @vcmdq must be mapped prior */
/*
 * [한국어]
 * tegra241_vcmdq_hw_init - 큐를 초기화하고 켠다 (커널용)
 *
 * @vcmdq: 켤 큐.
 * @return: 0 성공, 음수 실패.
 *
 * (위 영어 주석 참고) 전역 큐가 이 논리 큐에 이미 배정되어 있어야 한다 —
 * 그래야 레지스터에 닿을 수 있다.
 *
 * 먼저 깨끗이 되돌린 뒤 링 버퍼 주소를 쓰고 켠다. 그 사이에 "이 큐가
 * 받을 수 있는 명령"을 정하는 갈고리를 다는데, VINTF 의 주인이 커널이
 * 아니면 제한된 목록을 건다. 주인 여부는 하드웨어에서 되읽은 값이라
 * 이 시점에는 이미 확정되어 있다.
 *
 * 실행 컨텍스트: 초기화·리셋 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra241_vintf_hw_init() → [이 함수] → vcmdq_write_config()
 */
static int tegra241_vcmdq_hw_init(struct tegra241_vcmdq *vcmdq)
{
	char header[64], *h = lvcmdq_error_header(vcmdq, header, 64);	/* [한국어] 로그 머리말. */
	int ret;	/* [한국어] 켜기 결과. */

	/* Reset VCMDQ */
	/* [한국어] (위 영어 주석 참고) 이전 상태가 남아 있을 수 있으므로 먼저 되돌린다. */
	tegra241_vcmdq_hw_deinit(vcmdq);	/* [한국어] 포인터와 오류를 깨끗이 한다. */

	/* vintf->hyp_own is a HW state finalized in tegra241_vintf_hw_init() */
	/* [한국어] (위 영어 주석 참고) 주인이 누구인지는 하드웨어가 정하고 되읽어
	 * 확정한 값이다. 게스트 소유라면 이 큐로 낼 수 있는 명령이 제한된다. */
	if (!vcmdq->vintf->hyp_own)	/* [한국어] 커널 소유가 아니라면. */
		vcmdq->cmdq.supports_cmd = tegra241_guest_vcmdq_supports_cmd;	/* [한국어] 무효화 계열만 받는 갈고리를 건다 — 나머지 명령은 표준 큐로 돌아간다. */

	/* Configure and enable VCMDQ */
	/* [한국어] (위 영어 주석 참고) 링 버퍼 주소와 크기를 알린 뒤 켠다. */
	writeq_relaxed(vcmdq->cmdq.q.q_base, REG_VCMDQ_PAGE1(vcmdq, BASE));	/* [한국어] 주소와 크기가 한 값에 겹쳐 있다 — 큐를 잡을 때 그렇게 만들어 두었다. */

	ret = vcmdq_write_config(vcmdq, VCMDQ_EN);	/* [한국어] 큐를 켠다. */
	if (ret) {	/* [한국어] 켜지지 않았다면. */
		dev_err(vcmdq->cmdqv->dev,	/* [한국어] 진단에 필요한 레지스터를 모두 남긴다. */
			"%sGERRORN=0x%X, GERROR=0x%X, CONS=0x%X\n", h,
			readl_relaxed(REG_VCMDQ_PAGE0(vcmdq, GERRORN)),
			readl_relaxed(REG_VCMDQ_PAGE0(vcmdq, GERROR)),
			readl_relaxed(REG_VCMDQ_PAGE0(vcmdq, CONS)));
		return ret;	/* [한국어] 호출자가 나머지 큐 초기화를 접고 되감는다. */
	}

	dev_dbg(vcmdq->cmdqv->dev, "%sinited\n", h);	/* [한국어] 큐 수명 추적용 로그. */
	return 0;	/* [한국어] 이제 이 큐로 명령을 낼 수 있다. */
}

/* Unmap a global VCMDQ from the pre-assigned LVCMDQ */
/*
 * [한국어]
 * tegra241_vcmdq_unmap_lvcmdq - 전역 큐를 논리 큐 자리에서 떼어 낸다
 *
 * @vcmdq: 떼어 낼 큐.
 *
 * (위 영어 주석 참고) 배정 레지스터의 "살아 있음" 비트만 내린다. VINTF 번호와
 * 논리 큐 번호는 그대로 두는데, 다시 붙일 때 같은 자리로 돌아가야 하기
 * 때문이다. 이 비트를 내리면 그 큐의 레지스터에 더 이상 접근할 수 없으므로,
 * 반드시 큐를 끈 뒤에 불러야 한다.
 *
 * 실행 컨텍스트: 해제 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   tegra241_vintf_hw_deinit()/destroy_lvcmdq_user() → [이 함수]
 */
static void tegra241_vcmdq_unmap_lvcmdq(struct tegra241_vcmdq *vcmdq)
{
	u32 regval = readl(REG_CMDQV(vcmdq->cmdqv, CMDQ_ALLOC(vcmdq->idx)));	/* [한국어] 지금 배정 값을 읽어 둔다 — 다른 필드를 지우지 않으려면 읽고 고쳐 써야 한다. */
	char header[64], *h = lvcmdq_error_header(vcmdq, header, 64);	/* [한국어] 로그 머리말. */

	writel(regval & ~CMDQV_CMDQ_ALLOCATED,	/* [한국어] "살아 있음" 비트만 내린다. */
	       REG_CMDQV(vcmdq->cmdqv, CMDQ_ALLOC(vcmdq->idx)));	/* [한국어] 배정 레지스터는 전역 큐 번호로 짚는다. */
	dev_dbg(vcmdq->cmdqv->dev, "%sunmapped\n", h);	/* [한국어] 수명 추적용 로그. */
}

/*
 * [한국어]
 * tegra241_vintf_hw_deinit - VINTF 를 끄고 그 아래 큐와 치환표를 모두 되돌린다
 *
 * @vintf: 끌 VINTF.
 *
 * 순서가 하드웨어 규약으로 정해져 있다는 점이 이 함수의 핵심이다. 논리 큐는
 * 반드시 높은 번호부터 내림차순으로 떼어 내야 하며, 그래서 while (lidx--)
 * 형태로 거꾸로 훑는다. 그다음 VINTF 자체를 끄고, 마지막으로 스트림 치환표를
 * 지운다 — 치환표를 먼저 지우면 아직 처리 중인 명령이 엉뚱한 번호를 쓰게 된다.
 *
 * 치환표를 반드시 지워야 하는 이유는, 이 VINTF 가 다음 가상 머신에 넘어갈 때
 * 앞 게스트의 장치 대응이 남아 있으면 그 자체가 정보 유출이자 오동작이기
 * 때문이다.
 *
 * 실행 컨텍스트: 리셋·해제 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra241_vintf_hw_init()/remove_vintf()/init_vintf_user() 되감기
 *     → [이 함수] → tegra241_vcmdq_hw_deinit(), vintf_write_config()
 */
static void tegra241_vintf_hw_deinit(struct tegra241_vintf *vintf)
{
	u16 lidx = vintf->cmdqv->num_lvcmdqs_per_vintf;	/* [한국어] 마지막 번호 다음에서 시작해 거꾸로 내려간다. */
	int sidx;	/* [한국어] 치환표 반복자. */

	/* HW requires to unmap LVCMDQs in descending order */
	/* [한국어] (위 영어 주석 참고) 순서를 어기면 하드웨어가 배정을 제대로 풀지
	 * 못한다. while (lidx--) 는 num-1 부터 0 까지 내려가는 관용구다. */
	while (lidx--) {	/* [한국어] 높은 번호부터. */
		if (vintf->lvcmdqs && vintf->lvcmdqs[lidx]) {	/* [한국어] 배열이 있고 그 칸이 채워져 있을 때만 — 게스트용 VINTF 는 큐가 없을 수도 있다. */
			tegra241_vcmdq_hw_deinit(vintf->lvcmdqs[lidx]);	/* [한국어] 먼저 큐를 끈다. */
			tegra241_vcmdq_unmap_lvcmdq(vintf->lvcmdqs[lidx]);	/* [한국어] 그다음 배정을 푼다 — 순서를 뒤집으면 레지스터에 닿지 못한다. */
		}
	}
	vintf_write_config(vintf, 0);	/* [한국어] VINTF 자체를 끈다. 실패해도 계속 정리한다 — 되돌릴 방법이 없다. */
	for (sidx = 0; sidx < vintf->cmdqv->num_sids_per_vintf; sidx++) {	/* [한국어] 치환표의 모든 칸을. */
		writel(0, REG_VINTF(vintf, SID_MATCH(sidx)));	/* [한국어] 먼저 일치 조건을 지운다 — 이것이 0 이면 그 칸은 아무것도 잡지 않는다. */
		writel(0, REG_VINTF(vintf, SID_REPLACE(sidx)));	/* [한국어] 그다음 치환 값을 지운다. 순서가 이래야 중간 상태에서 엉뚱한 치환이 일어나지 않는다. */
	}
}

/* Map a global VCMDQ to the pre-assigned LVCMDQ */
/*
 * [한국어]
 * tegra241_vcmdq_map_lvcmdq - 전역 큐를 논리 큐 자리에 붙인다
 *
 * @vcmdq: 붙일 큐.
 *
 * (위 영어 주석 참고) 배정 레지스터의 "살아 있음" 비트를 올린다. VINTF 번호와
 * 논리 큐 번호는 리셋 때 미리 적어 두었으므로 여기서는 비트 하나만 켜면 된다.
 * 이 비트가 켜져야 그 큐의 레지스터에 접근할 수 있다.
 *
 * 실행 컨텍스트: 초기화 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   tegra241_vintf_hw_init()/alloc_lvcmdq_user() → [이 함수]
 */
static void tegra241_vcmdq_map_lvcmdq(struct tegra241_vcmdq *vcmdq)
{
	u32 regval = readl(REG_CMDQV(vcmdq->cmdqv, CMDQ_ALLOC(vcmdq->idx)));	/* [한국어] 미리 적어 둔 VINTF·논리 큐 번호를 그대로 살리려고 읽고 고쳐 쓴다. */
	char header[64], *h = lvcmdq_error_header(vcmdq, header, 64);	/* [한국어] 로그 머리말. */

	writel(regval | CMDQV_CMDQ_ALLOCATED,	/* [한국어] "살아 있음" 비트를 켠다. */
	       REG_CMDQV(vcmdq->cmdqv, CMDQ_ALLOC(vcmdq->idx)));
	dev_dbg(vcmdq->cmdqv->dev, "%smapped\n", h);	/* [한국어] 수명 추적용 로그. */
}

/*
 * [한국어]
 * tegra241_vintf_hw_init - VINTF 를 켜고 그 아래 큐들을 준비한다
 *
 * @vintf: 켤 VINTF.
 * @hyp_own: 호스트 커널이 주인이라고 요청할 것인가.
 * @return: 0 성공, 음수 실패.
 *
 * VMID 를 걸고 VINTF 를 켠 뒤, 매달린 논리 큐들을 오름차순으로 붙이고 켠다.
 * 오름차순은 하드웨어의 요구이며, 해제가 내림차순인 것과 짝을 이룬다.
 *
 * hyp_own 을 쓴 뒤 되읽는 처리가 이 함수에서 가장 중요한 대목이다. 게스트
 * 커널에서 이 코드가 돌면 하드웨어가 그 비트를 0 으로 고정해 버리므로,
 * 쓴 값을 믿으면 "나는 주인이다"라고 착각하게 된다. 그래서 반드시 되읽어
 * 확정하고, 그 결과가 큐가 받을 수 있는 명령 종류를 정한다.
 *
 * 실행 컨텍스트: 리셋·게스트 VINTF 초기화. 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra241_cmdqv_hw_reset()/init_vintf_user() → [이 함수]
 *     → vintf_write_config() → tegra241_vcmdq_hw_init()
 */
static int tegra241_vintf_hw_init(struct tegra241_vintf *vintf, bool hyp_own)
{
	u32 regval;	/* [한국어] 설정 레지스터 값. */
	u16 lidx;	/* [한국어] 논리 큐 반복자. */
	int ret;	/* [한국어] 중간 단계의 결과. */

	/* Reset VINTF */
	/* [한국어] (위 영어 주석 참고) 이전 주인의 상태가 남아 있을 수 있어 먼저 되돌린다. */
	tegra241_vintf_hw_deinit(vintf);	/* [한국어] 큐, 설정, 치환표를 모두 깨끗이 한다. */

	/* Configure and enable VINTF */
	/*
	 * Note that HYP_OWN bit is wired to zero when running in guest kernel,
	 * whether enabling it here or not, as !HYP_OWN cmdq HWs only support a
	 * restricted set of supported commands.
	 */
	/* [한국어] (위 영어 주석 참고) 이 코드가 게스트 커널 안에서 돌 수도 있다.
	 * 그때는 하드웨어가 이 비트를 0 으로 못 박아, 게스트가 스스로 "주인"이라고
	 * 주장할 수 없게 만든다. 그래서 아래에서 되읽어 확인해야 한다. */
	regval = FIELD_PREP(VINTF_HYP_OWN, hyp_own) |	/* [한국어] 주인 여부를 요청한다. */
		 FIELD_PREP(VINTF_VMID, vintf->vsmmu.vmid);	/* [한국어] 이 VINTF 의 큐에서 나온 명령에 강제로 붙일 VMID — 격리의 핵심이다. */
	writel(regval, REG_VINTF(vintf, CONFIG));	/* [한국어] 켜기 전에 설정을 먼저 쓴다 — 켜진 상태에서 VMID 를 바꾸면 안 되기 때문이다. */

	ret = vintf_write_config(vintf, regval | VINTF_EN);	/* [한국어] 같은 설정에 켜기 비트를 더해 다시 쓰고, 실제로 켜질 때까지 기다린다. */
	if (ret)	/* [한국어] 켜지지 않았다면. */
		return ret;	/* [한국어] 큐를 붙이지 않고 그대로 나간다. */
	/*
	 * As being mentioned above, HYP_OWN bit is wired to zero for a guest
	 * kernel, so read it back from HW to ensure that reflects in hyp_own
	 */
	/* [한국어] (위 영어 주석 참고) 쓴 값이 아니라 읽은 값이 진실이다. 이 한 줄이
	 * "게스트가 자기를 하이퍼바이저라고 속일 수 없다"를 보장한다. */
	vintf->hyp_own = !!(VINTF_HYP_OWN & readl(REG_VINTF(vintf, CONFIG)));	/* [한국어] 되읽어 확정한다. 이 값이 큐의 명령 제한을 정한다. */

	/* HW requires to map LVCMDQs in ascending order */
	/* [한국어] (위 영어 주석 참고) 해제가 내림차순인 것과 짝을 이루는 규약이다. */
	for (lidx = 0; lidx < vintf->cmdqv->num_lvcmdqs_per_vintf; lidx++) {	/* [한국어] 낮은 번호부터. */
		if (vintf->lvcmdqs && vintf->lvcmdqs[lidx]) {	/* [한국어] 그 칸에 큐가 있을 때만 — 게스트용 VINTF 는 처음에 비어 있다. */
			tegra241_vcmdq_map_lvcmdq(vintf->lvcmdqs[lidx]);	/* [한국어] 배정을 살린다. */
			ret = tegra241_vcmdq_hw_init(vintf->lvcmdqs[lidx]);	/* [한국어] 그다음 큐를 초기화하고 켠다. */
			if (ret) {	/* [한국어] 하나라도 실패하면. */
				tegra241_vintf_hw_deinit(vintf);	/* [한국어] 지금까지 켠 것을 모두 되돌린다 — 반쯤 켜진 상태로 두면 안 된다. */
				return ret;	/* [한국어] 실패를 위로 올린다. */
			}
		}
	}

	return 0;	/* [한국어] VINTF 와 그 아래 큐들이 모두 준비됐다. */
}

/*
 * [한국어]
 * tegra241_cmdqv_hw_reset - CMDQV 전체를 껐다 켜고 큐를 나눠 배정한다
 *
 * @smmu: 표준 드라이버가 넘긴 SMMU (실제로는 CMDQV).
 * @return: 0 성공, 음수 실패.
 *
 * 표준 드라이버가 SMMU 를 리셋할 때 함께 불린다. 전역 스위치를 껐다 켜서
 * 하드웨어를 알려진 상태로 만든 뒤, 전역 큐들을 VINTF 에 고르게 나눠 준다 —
 * 0번 VINTF 가 앞의 몇 개, 1번이 그다음 몇 개 하는 식이다. 이 배정은
 * 여기서 한 번만 하고 이후로는 "살아 있음" 비트만 켜고 끈다.
 *
 * 마지막으로 커널이 쓸 VINTF0 을 주인 자격으로 켠다.
 *
 * 실행 컨텍스트: 프로브·리셋. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_reset() → impl_ops->device_reset = [이 함수]
 *     → cmdqv_write_config() → tegra241_vintf_hw_init()
 */
static int tegra241_cmdqv_hw_reset(struct arm_smmu_device *smmu)
{
	struct tegra241_cmdqv *cmdqv =	/* [한국어] 확장 구조체로 되짚는다. */
		container_of(smmu, struct tegra241_cmdqv, smmu);
	u16 qidx, lidx, idx;	/* [한국어] 전역 큐 번호, 논리 큐 번호, VINTF 번호. */
	u32 regval;	/* [한국어] 레지스터 값. */
	int ret;	/* [한국어] 중간 결과. */

	/* Reset CMDQV */
	/* [한국어] (위 영어 주석 참고) 껐다 켜서 하드웨어 내부 상태를 초기화한다. */
	regval = readl_relaxed(REG_CMDQV(cmdqv, CONFIG));	/* [한국어] 지금 설정을 읽어 둔다 — 다른 비트를 건드리지 않기 위해서다. */
	ret = cmdqv_write_config(cmdqv, regval & ~CMDQV_EN);	/* [한국어] 먼저 끈다. */
	if (ret)	/* [한국어] 꺼지지 않으면 하드웨어가 응답하지 않는 것이다. */
		return ret;	/* [한국어] VINTF0 없이는 가속 큐를 하나도 쓸 수 없어 프로브를 접는다. */
	ret = cmdqv_write_config(cmdqv, regval | CMDQV_EN);	/* [한국어] 다시 켠다. */
	if (ret)	/* [한국어] 다시 켜지지 않으면 확장을 쓸 수 없다 — 프로브가 접힌다. */
		return ret;	/* [한국어] 실패 이유를 사용자 공간에 그대로 돌려준다. */

	/* Assign preallocated global VCMDQs to each VINTF as LVCMDQs */
	/* [한국어] (위 영어 주석 참고) 전역 큐를 VINTF 에 고르게 나눈다. qidx 가
	 * 전역 번호를 죽 세어 가고, 각 VINTF 는 자기 몫만큼 연속으로 받는다. */
	for (idx = 0, qidx = 0; idx < cmdqv->num_vintfs; idx++) {	/* [한국어] 모든 VINTF 에 대해. */
		for (lidx = 0; lidx < cmdqv->num_lvcmdqs_per_vintf; lidx++) {	/* [한국어] 그 VINTF 의 논리 큐 자리마다. */
			regval  = FIELD_PREP(CMDQV_CMDQ_ALLOC_VINTF, idx);	/* [한국어] 어느 VINTF 의 것인지. */
			regval |= FIELD_PREP(CMDQV_CMDQ_ALLOC_LVCMDQ, lidx);	/* [한국어] 그 안의 몇 번째인지. */
			writel_relaxed(regval,	/* [한국어] "살아 있음" 비트는 켜지 않는다 — 실제로 쓸 때 켠다. */
				       REG_CMDQV(cmdqv, CMDQ_ALLOC(qidx++)));	/* [한국어] 전역 큐를 하나씩 소진하며 배정한다. */
		}
	}

	return tegra241_vintf_hw_init(cmdqv->vintfs[0], true);	/* [한국어] 커널이 쓸 VINTF0 을 주인 자격으로 켠다 — 여기부터 가속 큐를 쓸 수 있다. */
}

/* VCMDQ Resource Helpers */

/*
 * [한국어]
 * tegra241_vcmdq_alloc_smmu_cmdq - 표준 명령 큐 구조를 이 논리 큐에 맞춰 잡는다
 *
 * @vcmdq: 대상 큐.
 * @return: 0 성공, 음수 실패.
 *
 * 표준 드라이버의 큐 초기화 헬퍼를 그대로 빌려 쓰되, 두 곳을 바꾼다.
 * 첫째, 큐 크기를 SMMU 가 지원하는 상한으로 자른다 — 확장 큐라도 명령
 * 형식은 표준과 같아 그 한계를 따른다. 둘째, 링 버퍼 주소를 담는 형식이
 * 표준 SMMU 와 달라서, 헬퍼가 만들어 준 q_base 를 이 하드웨어의 형식으로
 * 다시 짓는다.
 *
 * 그 뒤 표준 명령 큐 초기화를 불러 락 없는 삽입에 필요한 비트맵을 잡는다.
 * 이렇게 해 두면 표준 명령 발행 코드가 이 큐를 아무 차이 없이 다룰 수 있다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra241_vintf_alloc_lvcmdq() → [이 함수]
 *     → arm_smmu_init_one_queue() → arm_smmu_cmdq_init()
 */
static int tegra241_vcmdq_alloc_smmu_cmdq(struct tegra241_vcmdq *vcmdq)
{
	struct arm_smmu_device *smmu = &vcmdq->cmdqv->smmu;	/* [한국어] 표준 헬퍼가 요구하는 SMMU 포인터. */
	struct arm_smmu_cmdq *cmdq = &vcmdq->cmdq;	/* [한국어] 이 큐에 박혀 있는 표준 명령 큐 구조. */
	struct arm_smmu_queue *q = &cmdq->q;	/* [한국어] 그 안의 링 버퍼 부분. */
	char name[16];	/* [한국어] 로그와 dma 이름에 쓸 문자열. */
	u32 regval;	/* [한국어] 능력 레지스터 값. */
	int ret;	/* [한국어] 중간 결과. */

	snprintf(name, 16, "vcmdq%u", vcmdq->idx);	/* [한국어] 전역 번호로 이름을 지어 로그에서 큐를 구분할 수 있게 한다. */

	/* Cap queue size to SMMU's IDR1.CMDQS and ensure natural alignment */
	/* [한국어] (위 영어 주석 참고) 확장 큐라도 명령 형식은 표준이라 SMMU 가
	 * 지원하는 최대 크기를 넘을 수 없다. 두 한계 중 작은 쪽을 쓴다. */
	regval = readl_relaxed(smmu->base + ARM_SMMU_IDR1);	/* [한국어] SMMU 의 능력 레지스터에서 최대 큐 크기를 읽는다. */
	q->llq.max_n_shift =	/* [한국어] 큐 크기는 2의 지수로 표현한다. */
		min_t(u32, CMDQ_MAX_SZ_SHIFT, FIELD_GET(IDR1_CMDQS, regval));	/* [한국어] 드라이버가 정한 상한과 하드웨어 한계 중 작은 값. */

	/* Use the common helper to init the VCMDQ, and then... */
	/* [한국어] (위 영어 주석 참고) 링 버퍼를 잡고 포인터 레지스터 위치를 기록하는
	 * 일은 표준 헬퍼가 그대로 해 준다. */
	ret = arm_smmu_init_one_queue(smmu, q, vcmdq->page0,	/* [한국어] 포인터 레지스터는 이 큐의 페이지0 에 있다. */
				      TEGRA241_VCMDQ_PROD, TEGRA241_VCMDQ_CONS,	/* [한국어] 그 페이지 안에서의 오프셋. */
				      CMDQ_ENT_DWORDS, name);	/* [한국어] 명령 하나가 2워드라는 것은 표준과 같다. */
	if (ret)	/* [한국어] 링 버퍼를 못 잡으면. */
		return ret;	/* [한국어] 호출자가 큐를 놓고 나간다. */

	/* ...override q_base to write VCMDQ_BASE registers */
	/* [한국어] (위 영어 주석 참고) 표준 SMMU 는 주소와 크기를 다른 형식으로 담는다.
	 * 이 하드웨어의 BASE 레지스터 형식에 맞춰 다시 지어야 한다. */
	q->q_base = q->base_dma & VCMDQ_ADDR;	/* [한국어] 링 버퍼의 물리 주소에서 이 하드웨어가 쓰는 비트만 남긴다. */
	q->q_base |= FIELD_PREP(VCMDQ_LOG2SIZE, q->llq.max_n_shift);	/* [한국어] 아래 비트에 크기(로그)를 겹쳐 담는다 — 한 레지스터에 둘을 함께 쓴다. */

	return arm_smmu_cmdq_init(smmu, cmdq);	/* [한국어] 락 없는 삽입에 쓰는 유효 비트맵을 잡는다 — 이 뒤로 표준 발행 코드가 이 큐를 그대로 쓸 수 있다. */
}

/* VINTF Logical VCMDQ Resource Helpers */

/*
 * [한국어]
 * tegra241_vintf_deinit_lvcmdq - VINTF 의 큐 배열에서 그 칸을 비운다
 *
 * @vintf: 대상 VINTF.
 * @lidx: 비울 칸의 번호.
 *
 * 소프트웨어 쪽 연결만 끊는다 — 하드웨어 배정을 푸는 일은 별도 함수가 맡는다.
 * 한 줄짜리 함수를 따로 둔 이유는, 배정 경로와 되감기 경로가 같은 일을
 * 해야 해서 그 짝을 이름으로 분명히 하기 위해서다.
 *
 * 실행 컨텍스트: 초기화 되감기·해제 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   tegra241_vintf_free_lvcmdq()/alloc_lvcmdq_user() 되감기 → [이 함수]
 */
static void tegra241_vintf_deinit_lvcmdq(struct tegra241_vintf *vintf, u16 lidx)
{
	vintf->lvcmdqs[lidx] = NULL;	/* [한국어] 그 칸을 비워, 큐를 고르는 경로가 더 이상 이 큐를 보지 않게 한다. */
}

/*
 * [한국어]
 * tegra241_vintf_init_lvcmdq - 큐를 VINTF 의 한 칸에 이어 붙인다
 *
 * @vintf: 대상 VINTF.
 * @lidx: 붙일 칸의 번호.
 * @vcmdq: 붙일 큐.
 * @return: 항상 0 — 실패할 일이 없다.
 *
 * 큐의 번호들과 레지스터 주소를 계산해 채운다. 전역 번호를 (VINTF 번호 ×
 * VINTF당 큐 수 + 지역 번호)로 구하는 것이 요점인데, 리셋 때 배정을 그
 * 순서로 해 두었기 때문에 이 계산이 하드웨어의 실제 배정과 맞아떨어진다.
 *
 * 레지스터 주소는 VINTF 별 페이지에서 계산한다 — 게스트가 자기 VINTF 페이지만
 * 보고도 자기 큐를 다룰 수 있게 하는 배치다.
 *
 * 실행 컨텍스트: 프로브·게스트 큐 배정. 잠들지 않는다.
 *
 * 호출 체인:
 *   tegra241_vintf_alloc_lvcmdq()/alloc_lvcmdq_user() → [이 함수]
 */
static int tegra241_vintf_init_lvcmdq(struct tegra241_vintf *vintf, u16 lidx,
				      struct tegra241_vcmdq *vcmdq)
{
	struct tegra241_cmdqv *cmdqv = vintf->cmdqv;	/* [한국어] 전역 정보 — VINTF 당 큐 수와 MMIO 창 주소가 필요하다. */
	u16 idx = vintf->idx;	/* [한국어] 이 VINTF 의 번호. */

	vcmdq->idx = idx * cmdqv->num_lvcmdqs_per_vintf + lidx;	/* [한국어] 리셋 때의 배정 순서와 같은 식으로 전역 번호를 구한다 — 이 계산이 하드웨어 배정과 어긋나면 엉뚱한 큐를 다루게 된다. */
	vcmdq->lidx = lidx;	/* [한국어] 지역 번호는 그대로. */
	vcmdq->cmdqv = cmdqv;	/* [한국어] 전역 문맥으로 가는 길. */
	vcmdq->vintf = vintf;	/* [한국어] 주인 VINTF 로 가는 길 — 로그 머리말이 이 값을 필요로 한다. */
	vcmdq->page0 = cmdqv->base + TEGRA241_VINTFi_LVCMDQ_PAGE0(idx, lidx);	/* [한국어] VINTF 별 페이지에서 계산한다 — 전역 큐 페이지가 아니라는 점이 중요하다. */
	vcmdq->page1 = cmdqv->base + TEGRA241_VINTFi_LVCMDQ_PAGE1(idx, lidx);	/* [한국어] 호스트 전용 페이지. */

	vintf->lvcmdqs[lidx] = vcmdq;	/* [한국어] 배열에 걸어, 이제부터 큐 선택 경로가 이 큐를 볼 수 있게 한다. */
	return 0;	/* [한국어] 실패할 일이 없지만, 호출부의 오류 처리 형태를 맞추려고 int 를 돌려준다. */
}

/*
 * [한국어]
 * tegra241_vintf_free_lvcmdq - 커널이 잡았던 논리 큐를 놓는다
 *
 * @vintf: 대상 VINTF.
 * @lidx: 놓을 칸의 번호.
 *
 * 링 버퍼는 devres 로 잡혀 있어 장치가 사라질 때 저절로 놓이므로, 여기서는
 * 배열 칸을 비우고 구조체만 놓는다.
 *
 * 게스트에게 넘긴 큐는 놓지 않는다는 점이 중요하다 — 그 구조체는 iommufd 가
 * 잡은 객체 안에 박혀 있어, 놓는 일도 iommufd 가 자기 수명 규칙에 따라 한다.
 * 여기서 놓으면 이중 해제가 된다.
 *
 * 실행 컨텍스트: 제거·해제 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra241_cmdqv_remove_vintf()/destroy_lvcmdq_user() → [이 함수]
 */
static void tegra241_vintf_free_lvcmdq(struct tegra241_vintf *vintf, u16 lidx)
{
	struct tegra241_vcmdq *vcmdq = vintf->lvcmdqs[lidx];	/* [한국어] 놓을 큐를 먼저 붙잡아 둔다 — 아래에서 배열 칸이 비워지기 때문이다. */
	char header[64];	/* [한국어] 로그 머리말 버퍼. */

	/* Note that the lvcmdq queue memory space is managed by devres */
	/* [한국어] (위 영어 주석 참고) 링 버퍼는 devm_ 계열로 잡혀 있어 여기서
	 * 따로 놓지 않는다. 장치가 사라질 때 커널이 알아서 거둔다. */

	tegra241_vintf_deinit_lvcmdq(vintf, lidx);	/* [한국어] 배열 칸을 비운다 — 이 뒤로 아무도 이 큐를 고르지 않는다. */

	dev_dbg(vintf->cmdqv->dev,	/* [한국어] 로그는 배열에서 뗀 뒤, 구조체를 놓기 전에 찍어야 한다. */
		"%sdeallocated\n", lvcmdq_error_header(vcmdq, header, 64));
	/* Guest-owned VCMDQ is free-ed with hw_queue by iommufd core */
	/* [한국어] (위 영어 주석 참고) 게스트에게 넘긴 큐는 iommufd 객체 안에 박혀
	 * 있어, 여기서 놓으면 이중 해제가 된다. 주인이 누구인지로 가른다. */
	if (vcmdq->vintf->hyp_own)	/* [한국어] 커널이 잡은 큐라면. */
		kfree(vcmdq);	/* [한국어] 우리가 놓는다. */
}

/*
 * [한국어]
 * tegra241_vintf_alloc_lvcmdq - 커널용 논리 큐를 하나 만든다
 *
 * @vintf: 대상 VINTF (커널용 VINTF0).
 * @lidx: 만들 칸의 번호.
 * @return: 만들어진 큐, 실패하면 ERR_PTR.
 *
 * 구조체를 잡고, VINTF 에 이어 붙이고, 링 버퍼까지 마련한다. 커널용 큐는
 * 프로브 때 미리 다 만들어 두므로 게스트용처럼 순서 제약이나 락이 필요 없다.
 *
 * 오류 처리는 되감기 순서를 지킨다 — 링 버퍼 마련에 실패하면 배열 칸을
 * 먼저 비우고, 그다음 구조체를 놓는다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra241_cmdqv_init_structures() → [이 함수]
 *     → tegra241_vintf_init_lvcmdq() → tegra241_vcmdq_alloc_smmu_cmdq()
 */
static struct tegra241_vcmdq *
tegra241_vintf_alloc_lvcmdq(struct tegra241_vintf *vintf, u16 lidx)
{
	struct tegra241_cmdqv *cmdqv = vintf->cmdqv;	/* [한국어] 로그를 찍을 장치. */
	struct tegra241_vcmdq *vcmdq;	/* [한국어] 만들 큐. */
	char header[64];	/* [한국어] 로그 머리말 버퍼. */
	int ret;	/* [한국어] 중간 결과. */

	vcmdq = kzalloc_obj(*vcmdq);	/* [한국어] 0 으로 채운 큐 구조체를 잡는다. */
	if (!vcmdq)	/* [한국어] 메모리가 없으면. */
		return ERR_PTR(-ENOMEM);	/* [한국어] 오류 포인터로 알린다. */

	ret = tegra241_vintf_init_lvcmdq(vintf, lidx, vcmdq);	/* [한국어] 번호와 레지스터 주소를 채우고 배열에 건다. */
	if (ret)	/* [한국어] 지금은 실패하지 않지만 형태를 지킨다. */
		goto free_vcmdq;	/* [한국어] 구조체를 놓고 나간다. */

	/* Build an arm_smmu_cmdq for each LVCMDQ */
	/* [한국어] (위 영어 주석 참고) 표준 드라이버가 그대로 쓸 수 있는 큐 구조를
	 * 만든다 — 이것이 있어야 표준 발행 코드가 이 큐에 명령을 넣는다. */
	ret = tegra241_vcmdq_alloc_smmu_cmdq(vcmdq);	/* [한국어] 링 버퍼와 유효 비트맵을 마련한다. */
	if (ret)	/* [한국어] 메모리가 모자라면. */
		goto deinit_lvcmdq;	/* [한국어] 배열 칸부터 비우고 되감는다. */

	dev_dbg(cmdqv->dev,	/* [한국어] 큐 수명 추적용 로그. */
		"%sallocated\n", lvcmdq_error_header(vcmdq, header, 64));
	return vcmdq;	/* [한국어] 호출자가 배열에서 다시 꺼내 쓸 수 있다. */

deinit_lvcmdq:	/* [한국어] 배열에 건 뒤 실패했을 때의 되감기 지점. */
	tegra241_vintf_deinit_lvcmdq(vintf, lidx);	/* [한국어] 배열 칸을 비운다 — 아무도 이 반쯤 만들어진 큐를 보지 못하게. */
free_vcmdq:	/* [한국어] 구조체만 잡았을 때의 되감기 지점. */
	kfree(vcmdq);	/* [한국어] 구조체를 놓는다. */
	return ERR_PTR(ret);	/* [한국어] 실패 이유를 오류 포인터로 감싸 돌려준다. */
}

/* VINTF Resource Helpers */

/*
 * [한국어]
 * tegra241_cmdqv_deinit_vintf - VINTF 의 소프트웨어 자원을 되돌린다
 *
 * @cmdqv: 전역 문맥.
 * @idx: 되돌릴 VINTF 번호.
 *
 * 큐 배열을 놓고, VINTF 번호를 할당기에 돌려주고, 전역 배열의 칸을 비운다.
 * 하드웨어를 끄는 일은 이미 끝나 있어야 한다 — 이 함수가 불린 뒤로는
 * 그 VINTF 의 레지스터를 다룰 근거가 사라진다.
 *
 * 실행 컨텍스트: 제거·되감기 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra241_cmdqv_remove_vintf()/init_vintf_user() 되감기 → [이 함수]
 */
static void tegra241_cmdqv_deinit_vintf(struct tegra241_cmdqv *cmdqv, u16 idx)
{
	kfree(cmdqv->vintfs[idx]->lvcmdqs);	/* [한국어] 큐 포인터 배열을 놓는다. 큐 자체는 이미 정리되어 있어야 한다. */
	ida_free(&cmdqv->vintf_ids, idx);	/* [한국어] 번호를 돌려줘 다음 가상 머신이 쓸 수 있게 한다. */
	cmdqv->vintfs[idx] = NULL;	/* [한국어] 전역 배열에서 지운다 — 인터럽트 처리가 사라진 VINTF 를 건드리지 않게. */
}

/*
 * [한국어]
 * tegra241_cmdqv_init_vintf - VINTF 번호를 배정하고 소프트웨어 자원을 잡는다
 *
 * @cmdqv: 전역 문맥.
 * @max_idx: 받을 수 있는 가장 큰 번호.
 * @vintf: 채울 VINTF 구조체.
 * @return: 배정받은 번호(0 이상), 또는 음수 오류.
 *
 * 커널용과 게스트용이 같은 함수를 쓴다. 다른 점은 max_idx 뿐이다 — 커널은
 * 0 만 받을 수 있고(0을 넘겨 0번을 강제한다), 게스트는 나머지 범위에서
 * 아무 번호나 받는다.
 *
 * 큐 포인터 배열을 여기서 잡되 큐 자체는 잡지 않는다. 커널용은 곧이어
 * 프로브가 다 채우고, 게스트용은 게스트가 요청할 때마다 하나씩 채운다.
 *
 * 실행 컨텍스트: 프로브·iommufd ioctl. 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra241_cmdqv_init_structures()/init_vintf_user() → [이 함수]
 *     → ida_alloc_max()
 */
static int tegra241_cmdqv_init_vintf(struct tegra241_cmdqv *cmdqv, u16 max_idx,
				     struct tegra241_vintf *vintf)
{

	u16 idx;	/* [한국어] 배정받을 번호. */
	int ret;	/* [한국어] 할당기 결과 — 음수면 오류, 아니면 번호다. */

	ret = ida_alloc_max(&cmdqv->vintf_ids, max_idx, GFP_KERNEL);	/* [한국어] 0 부터 max_idx 사이에서 빈 번호를 받는다. */
	if (ret < 0)	/* [한국어] 남은 번호가 없으면 — 게스트가 이미 다 차지한 경우다. */
		return ret;	/* [한국어] 호출자가 "더 이상 가속을 줄 수 없다"고 알린다. */
	idx = ret;	/* [한국어] 받은 번호. */

	vintf->idx = idx;	/* [한국어] 자기 번호를 기억한다. */
	vintf->cmdqv = cmdqv;	/* [한국어] 전역 문맥으로 가는 길. */
	vintf->base = cmdqv->base + TEGRA241_VINTF(idx);	/* [한국어] 자기 레지스터 묶음의 주소를 계산해 둔다. */

	vintf->lvcmdqs = kzalloc_objs(*vintf->lvcmdqs,	/* [한국어] 큐 포인터 배열을 잡는다. 모두 NULL 로 시작한다. */
				      cmdqv->num_lvcmdqs_per_vintf);
	if (!vintf->lvcmdqs) {	/* [한국어] 메모리가 없으면. */
		ida_free(&cmdqv->vintf_ids, idx);	/* [한국어] 방금 받은 번호를 도로 돌려준다 — 되감기를 빠뜨리면 번호가 샌다. */
		return -ENOMEM;	/* [한국어] 큐 배열을 못 잡았으므로 방금 받은 번호를 돌려준 뒤 실패로 나간다. */
	}

	cmdqv->vintfs[idx] = vintf;	/* [한국어] 전역 배열에 건다 — 이제 인터럽트 처리가 이 VINTF 를 찾을 수 있다. */
	return ret;	/* [한국어] 배정받은 번호를 돌려준다. */
}

/* Remove Helpers */

/*
 * [한국어]
 * tegra241_cmdqv_remove_vintf - VINTF 하나를 하드웨어와 소프트웨어에서 모두 거둔다
 *
 * @cmdqv: 전역 문맥.
 * @idx: 거둘 VINTF 번호.
 *
 * 순서가 곧 안전이다 — 하드웨어를 먼저 끄고(그래야 더 이상 명령이 처리되지
 * 않는다), 그다음 큐를 놓고, 마지막에 소프트웨어 자원을 되돌린다.
 *
 * 마지막에 주인에 따라 갈린다. 커널 VINTF 는 우리가 kzalloc 으로 잡았으니
 * 우리가 놓고, 게스트 VINTF 는 iommufd 객체 안에 박혀 있으니 락과 할당기만
 * 정리하고 구조체는 iommufd 에 맡긴다.
 *
 * 실행 컨텍스트: 드라이버 제거·게스트 객체 해제. 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra241_cmdqv_remove()/destroy_vintf_user() → [이 함수]
 *     → tegra241_vintf_hw_deinit() → tegra241_vintf_free_lvcmdq()
 */
static void tegra241_cmdqv_remove_vintf(struct tegra241_cmdqv *cmdqv, u16 idx)
{
	struct tegra241_vintf *vintf = cmdqv->vintfs[idx];	/* [한국어] 거둘 VINTF. */
	u16 lidx;	/* [한국어] 큐 반복자. */

	tegra241_vintf_hw_deinit(vintf);	/* [한국어] 먼저 하드웨어를 끈다 — 이 뒤로는 이 VINTF 로 명령이 처리되지 않는다. */

	/* Remove LVCMDQ resources */
	/* [한국어] (위 영어 주석 참고) 하드웨어가 멈춘 뒤에야 큐 메모리를 놓을 수 있다. */
	for (lidx = 0; lidx < vintf->cmdqv->num_lvcmdqs_per_vintf; lidx++)	/* [한국어] 모든 칸을 훑는다. */
		if (vintf->lvcmdqs[lidx])	/* [한국어] 채워진 칸만. */
			tegra241_vintf_free_lvcmdq(vintf, lidx);	/* [한국어] 배열에서 떼고 (커널 것이면) 놓는다. */

	dev_dbg(cmdqv->dev, "VINTF%u: deallocated\n", vintf->idx);	/* [한국어] 수명 추적용 로그 — 번호를 돌려주기 전에 찍는다. */
	tegra241_cmdqv_deinit_vintf(cmdqv, idx);	/* [한국어] 배열과 번호를 되돌린다. */
	if (!vintf->hyp_own) {	/* [한국어] 게스트 소유였다면. */
		mutex_destroy(&vintf->lvcmdq_mutex);	/* [한국어] 게스트용에서만 만들었던 락을 정리한다. */
		ida_destroy(&vintf->sids);	/* [한국어] 스트림 치환표 할당기도 정리한다. */
		/* Guest-owned VINTF is free-ed with viommu by iommufd core */
		/* [한국어] (위 영어 주석 참고) 구조체 자체는 iommufd 객체 안에 박혀 있어
		 * 여기서 놓지 않는다 — 놓으면 이중 해제가 된다. */
	} else {	/* [한국어] 커널 소유였다면. */
		kfree(vintf);	/* [한국어] 우리가 잡았으니 우리가 놓는다. */
	}
}

/*
 * [한국어]
 * tegra241_cmdqv_remove - 이 확장이 잡은 모든 자원을 거둔다
 *
 * @smmu: 표준 드라이버가 넘긴 SMMU (실제로는 CMDQV).
 *
 * 표준 드라이버가 SMMU 를 내릴 때 함께 불린다. 이 시점에는 게스트 VINTF 가
 * 이미 모두 사라져 있어야 하며(iommufd 객체가 먼저 정리된다), 남아 있다면
 * 수명 관리가 어긋난 것이므로 경고를 남긴다.
 *
 * 마지막에 장치 참조를 놓는 것을 잊으면 안 된다 — 프로브에서 잡아 두었기
 * 때문이다.
 *
 * 실행 컨텍스트: 드라이버 제거. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_remove() → impl_ops->device_remove = [이 함수]
 *     → tegra241_cmdqv_remove_vintf()
 */
static void tegra241_cmdqv_remove(struct arm_smmu_device *smmu)
{
	struct tegra241_cmdqv *cmdqv =	/* [한국어] 확장 구조체로 되짚는다. */
		container_of(smmu, struct tegra241_cmdqv, smmu);
	u16 idx;	/* [한국어] VINTF 반복자. */

	/* Remove VINTF resources */
	/* [한국어] (위 영어 주석 참고) 남아 있는 VINTF 를 모두 거둔다. */
	for (idx = 0; idx < cmdqv->num_vintfs; idx++) {	/* [한국어] 모든 번호를 훑는다. */
		if (cmdqv->vintfs[idx]) {	/* [한국어] 살아 있는 것만. */
			/* Only vintf0 should remain at this stage */
			/* [한국어] (위 영어 주석 참고) 게스트 VINTF 는 iommufd 객체가
			 * 먼저 사라지며 정리되었어야 한다. 남아 있다면 수명 관리 버그다. */
			WARN_ON(idx > 0);	/* [한국어] 조용히 넘기지 않고 흔적을 남긴다. */
			tegra241_cmdqv_remove_vintf(cmdqv, idx);	/* [한국어] 그래도 정리는 한다 — 자원을 흘리는 것보다 낫다. */
		}
	}

	/* Remove cmdqv resources */
	/* [한국어] (위 영어 주석 참고) VINTF 가 모두 사라진 뒤 전역 자원을 거둔다. */
	ida_destroy(&cmdqv->vintf_ids);	/* [한국어] 번호 할당기를 정리한다. */

	if (cmdqv->irq > 0)	/* [한국어] 인터럽트를 등록했다면. */
		free_irq(cmdqv->irq, cmdqv);	/* [한국어] 먼저 떼어 낸다 — 아래에서 MMIO 를 풀기 전에 반드시 해야 한다. */
	iounmap(cmdqv->base);	/* [한국어] MMIO 매핑을 푼다. */
	kfree(cmdqv->vintfs);	/* [한국어] VINTF 포인터 배열을 놓는다. */
	put_device(cmdqv->dev); /* smmu->impl_dev */
	/* [한국어] (위 영어 주석 참고) 프로브에서 잡아 둔 플랫폼 장치 참조를 놓는다.
	 * 이것을 빠뜨리면 장치가 영원히 사라지지 않는다. */
}

/* [한국어] 아래 연산표에서 참조하지만 정의는 파일 끝에 있어 미리 알려 둔다 —
 * 게스트용 VINTF 초기화는 iommufd 자료 구조를 많이 쓰므로 그쪽 함수들과
 * 함께 두는 편이 읽기 좋다. */
static int
tegra241_cmdqv_init_vintf_user(struct arm_vsmmu *vsmmu,
			       const struct iommu_user_data *user_data);

/*
 * [한국어]
 * tegra241_cmdqv_hw_info - 이 확장의 능력을 사용자 공간에 알린다
 *
 * @smmu: 표준 드라이버가 넘긴 SMMU (실제로는 CMDQV).
 * @length: 돌려주는 정보의 크기를 적을 자리.
 * @type: 사용자가 원한 형식이 들어오고, 실제로 준 형식을 적어 돌려준다.
 * @return: 새로 잡은 정보 버퍼, 실패하면 ERR_PTR.
 *
 * VMM 이 게스트에게 보여 줄 가상 CMDQV 를 지으려면 큐를 몇 개까지 줄 수
 * 있는지, 장치를 몇 개까지 등록할 수 있는지 알아야 한다. 그 값을 로그
 * 형태로 알려 준다 — 하드웨어가 2의 거듭제곱만 쓰므로 로그로 주는 편이
 * 자연스럽다.
 *
 * 표준 SMMUv3 정보를 물으면 이 함수는 불리지 않는다 — 표준 경로가 먼저
 * 처리하고, 이 확장만의 형식일 때만 여기로 온다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_hw_info() → impl_ops->hw_info = [이 함수]
 */
static void *tegra241_cmdqv_hw_info(struct arm_smmu_device *smmu, u32 *length,
				    enum iommu_hw_info_type *type)
{
	struct tegra241_cmdqv *cmdqv =	/* [한국어] 확장 구조체로 되짚는다. */
		container_of(smmu, struct tegra241_cmdqv, smmu);
	struct iommu_hw_info_tegra241_cmdqv *info;	/* [한국어] 돌려줄 정보 버퍼. */
	u32 regval;	/* [한국어] 능력 레지스터 값. */

	if (*type != IOMMU_HW_INFO_TYPE_TEGRA241_CMDQV)	/* [한국어] 이 확장의 형식을 물은 것이 아니라면. */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 우리가 답할 수 있는 질문이 아니다. */

	info = kzalloc_obj(*info);	/* [한국어] 0 으로 채워 잡는다 — 안 채운 필드가 새어 나가면 안 된다. */
	if (!info)	/* [한국어] 정보 버퍼를 못 잡으면 능력을 알려 줄 수 없다. */
		return ERR_PTR(-ENOMEM);

	regval = readl_relaxed(REG_CMDQV(cmdqv, PARAM));	/* [한국어] 개정 번호를 읽기 위해. 나머지 값은 이미 풀어 두었다. */
	info->log2vcmdqs = ilog2(cmdqv->num_lvcmdqs_per_vintf);	/* [한국어] 게스트 하나가 받을 수 있는 큐 수(로그). */
	info->log2vsids = ilog2(cmdqv->num_sids_per_vintf);	/* [한국어] 게스트 하나가 등록할 수 있는 장치 수(로그). */
	info->version = FIELD_GET(CMDQV_VER, regval);	/* [한국어] 확장의 개정 번호 — 게스트가 세대별 차이를 다룰 수 있게. */

	*length = sizeof(*info);	/* [한국어] 얼마나 채웠는지 알린다. */
	*type = IOMMU_HW_INFO_TYPE_TEGRA241_CMDQV;	/* [한국어] 어떤 형식으로 답했는지 분명히 한다. */
	return info;	/* [한국어] iommufd 가 사용자 공간으로 복사한 뒤 놓는다. */
}

/*
 * [한국어]
 * tegra241_cmdqv_get_vintf_size - 가속 가상 IOMMU 객체의 크기를 알린다
 *
 * @viommu_type: 사용자가 요청한 종류.
 * @return: 필요한 바이트 수, 지원하지 않는 종류면 0.
 *
 * 표준 arm_smmu_get_viommu_size() 가 자기가 모르는 종류를 만나면 이 함수로
 * 넘긴다. 0 을 돌려주면 요청이 거부되므로, 이 함수가 "가속 VINTF 를 줄 수
 * 있는가"의 관문이기도 하다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_get_viommu_size() → impl_ops->get_viommu_size = [이 함수]
 */
static size_t tegra241_cmdqv_get_vintf_size(enum iommu_viommu_type viommu_type)
{
	if (viommu_type != IOMMU_VIOMMU_TYPE_TEGRA241_CMDQV)	/* [한국어] 이 확장의 종류가 아니라면. */
		return 0;	/* [한국어] 지원하지 않는다고 알린다. */
	return VIOMMU_STRUCT_SIZE(struct tegra241_vintf, vsmmu.core);	/* [한국어] VINTF 구조체 크기. 매크로가 core 필드의 위치까지 함께 검사한다. */
}

/* [한국어] 표준 SMMUv3 드라이버에 걸어 두는 구현체별 갈고리표.
 *
 * 커널이 쓰는 셋과 사용자 공간이 쓰는 셋으로 나뉜다. 표준 드라이버는 이
 * 표만 보고 확장을 다루므로, 확장을 아는 코드가 이 파일 안에만 머문다. */
static struct arm_smmu_impl_ops tegra241_cmdqv_impl_ops = {
	/* For in-kernel use */
	/* [한국어] (위 영어 주석 참고) 커널이 직접 쓰는 갈고리들. */
	.get_secondary_cmdq = tegra241_cmdqv_get_cmdq,	/* [한국어] 명령을 넣을 가속 큐를 고르는 갈고리 — 성능이 이 하나에서 나온다. */
	.device_reset = tegra241_cmdqv_hw_reset,	/* [한국어] SMMU 리셋에 맞춰 확장도 리셋하는 갈고리. */
	.device_remove = tegra241_cmdqv_remove,	/* [한국어] 자원을 거두는 갈고리. */
	/* For user-space use */
	/* [한국어] (위 영어 주석 참고) iommufd 를 통해 게스트에게 넘길 때 쓰는 갈고리들. */
	.hw_info = tegra241_cmdqv_hw_info,	/* [한국어] 확장의 능력을 알리는 갈고리. */
	.get_viommu_size = tegra241_cmdqv_get_vintf_size,	/* [한국어] 가속 가상 IOMMU 를 줄 수 있는지 판단하는 갈고리. */
	.vsmmu_init = tegra241_cmdqv_init_vintf_user,	/* [한국어] 게스트에게 VINTF 를 배정하는 갈고리. */
};

/* Probe Functions */

/*
 * [한국어]
 * tegra241_cmdqv_init_structures - 커널용 VINTF 와 큐들을 미리 만들어 둔다
 *
 * @smmu: 표준 드라이버가 넘긴 SMMU (실제로는 CMDQV).
 * @return: 0 성공, 음수 실패.
 *
 * 표준 드라이버의 자료 구조 초기화 단계에서 불린다. VINTF0 을 만들고 그
 * 아래 논리 큐를 모두 미리 잡아 둔다 — 커널이 명령을 낼 때마다 큐를 만들
 * 수는 없으므로, 쓸 수 있는 큐를 전부 준비해 두는 것이다.
 *
 * 마지막에 갈고리표를 완전한 것으로 바꿔 다는 것이 중요하다. 그전까지는
 * 프로브 단계용 축약판이 걸려 있었고, 여기서부터 큐 선택 갈고리가 살아나
 * 실제로 가속이 시작된다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_init_structures() → impl_ops->init_structures = [이 함수]
 *     → tegra241_cmdqv_init_vintf() → tegra241_vintf_alloc_lvcmdq()
 */
static int tegra241_cmdqv_init_structures(struct arm_smmu_device *smmu)
{
	struct tegra241_cmdqv *cmdqv =	/* [한국어] 확장 구조체로 되짚는다. */
		container_of(smmu, struct tegra241_cmdqv, smmu);
	struct tegra241_vintf *vintf;	/* [한국어] 만들 커널용 VINTF. */
	int lidx;	/* [한국어] 큐 반복자. */
	int ret;	/* [한국어] 중간 결과. */

	vintf = kzalloc_obj(*vintf);	/* [한국어] 커널용이므로 우리가 직접 잡는다 — 게스트용은 iommufd 가 잡아 준다. */
	if (!vintf)	/* [한국어] 커널용 VINTF 를 못 잡으면 가속을 쓸 수 없다. */
		return -ENOMEM;

	/* Init VINTF0 for in-kernel use */
	/* [한국어] (위 영어 주석 참고) max_idx 를 0 으로 주어 반드시 0번을 받게 한다.
	 * 0번이 커널 몫이라는 약속이 코드 곳곳에 깔려 있다. */
	ret = tegra241_cmdqv_init_vintf(cmdqv, 0, vintf);	/* [한국어] 번호와 큐 배열을 마련한다. */
	if (ret) {	/* [한국어] 0번이 이미 잡혀 있거나 메모리가 없으면. */
		dev_err(cmdqv->dev, "failed to init vintf0: %d\n", ret);	/* [한국어] 가속을 쓸 수 없게 되므로 오류로 남긴다. */
		return ret;	/* [한국어] 가속을 받을 수 있는 가상 머신 수의 한계다. */
	}

	/* Preallocate logical VCMDQs to VINTF0 */
	/* [한국어] (위 영어 주석 참고) 명령을 낼 때 큐를 만들 수는 없으므로 미리
	 * 다 잡아 둔다. 게스트용 VINTF 가 게으르게 잡는 것과 대비된다. */
	for (lidx = 0; lidx < cmdqv->num_lvcmdqs_per_vintf; lidx++) {	/* [한국어] 이 VINTF 가 가질 수 있는 모든 큐를. */
		struct tegra241_vcmdq *vcmdq;	/* [한국어] 만들어진 큐 (배열에 걸리므로 여기서 따로 보관하지 않는다). */

		vcmdq = tegra241_vintf_alloc_lvcmdq(vintf, lidx);	/* [한국어] 구조체와 링 버퍼를 마련해 배열에 건다. */
		if (IS_ERR(vcmdq))	/* [한국어] 하나라도 실패하면. */
			return PTR_ERR(vcmdq);	/* [한국어] 프로브를 접는다 — 표준 드라이버가 되감기를 맡는다. */
	}

	/* Now, we are ready to run all the impl ops */
	/* [한국어] (위 영어 주석 참고) 큐가 모두 준비된 뒤에야 완전한 갈고리표를 건다.
	 * 그전에 큐 선택 갈고리가 살아 있으면 아직 없는 큐를 고르려 들 수 있다. */
	smmu->impl_ops = &tegra241_cmdqv_impl_ops;	/* [한국어] 이 줄 이후로 가속이 실제로 시작된다. */
	return 0;	/* [한국어] 자료 구조가 모두 준비됐다 — 이 시점부터 가속이 실제로 돈다. */
}

#ifdef CONFIG_IOMMU_DEBUGFS	/* [한국어] debugfs 를 끄고 빌드하면 이 변수 자체가 없다. */
static struct dentry *cmdqv_debugfs_dir;	/* [한국어] bypass_vcmdq 토글을 노출할 디렉터리. 칩이 여러 개여도 하나만 만든다. */
#endif

/*
 * [한국어]
 * __tegra241_cmdqv_probe - CMDQV 하드웨어를 찾아 기본 자원을 마련한다
 *
 * @smmu: 표준 프로브가 만들어 둔 SMMU.
 * @res: CMDQV 의 MMIO 자원.
 * @irq: 오류 인터럽트 번호 (없으면 0 이하).
 * @return: 확장을 품은 새 SMMU 포인터, 실패하면 NULL (표준 큐로 돌아간다).
 *
 * 이 함수의 묘미는 devm_krealloc 이다. 표준 프로브가 이미 arm_smmu_device 를
 * 잡아 두었는데, 이 확장은 그보다 큰 구조체가 필요하다. 그래서 기존 할당을
 * 더 큰 크기로 늘려, 앞부분은 그대로 두고 뒤에 확장 필드를 붙인다.
 * arm_smmu_device 가 반드시 첫 필드여야 하는 이유가 여기 있고, 위의
 * static_assert 가 그것을 빌드 시점에 확인한다.
 *
 * 실패하면 NULL 을 돌려주며, 호출자는 조용히 표준 명령 큐로 돌아간다 —
 * 가속을 못 쓸 뿐 시스템은 정상 동작한다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra241_cmdqv_probe() → [이 함수] → ioremap(), request_threaded_irq()
 */
static struct arm_smmu_device *
__tegra241_cmdqv_probe(struct arm_smmu_device *smmu, struct resource *res,
		       int irq)
{
	static const struct arm_smmu_impl_ops init_ops = {	/* [한국어] 프로브 단계에서만 쓰는 축약 갈고리표. */
		.init_structures = tegra241_cmdqv_init_structures,	/* [한국어] 자료 구조를 마련할 때 불릴 갈고리. */
		.device_remove = tegra241_cmdqv_remove,	/* [한국어] 프로브가 중간에 실패해도 자원을 거둘 수 있게 미리 걸어 둔다. */
	};
	struct tegra241_cmdqv *cmdqv = NULL;	/* [한국어] 만들 확장 구조체. */
	struct arm_smmu_device *new_smmu;	/* [한국어] 그 안의 표준 SMMU 부분 — 표준 드라이버에게 돌려줄 포인터다. */
	void __iomem *base;	/* [한국어] MMIO 매핑. */
	u32 regval;	/* [한국어] 레지스터 값. */
	int ret;	/* [한국어] 중간 결과. */

	static_assert(offsetof(struct tegra241_cmdqv, smmu) == 0);	/* [한국어] 표준 SMMU 가 첫 필드여야 devm_krealloc 으로 앞부분을 살릴 수 있다 — 빌드 시점에 못 박는다. */

	base = ioremap(res->start, resource_size(res));	/* [한국어] CMDQV 레지스터 창을 매핑한다. */
	if (!base) {	/* [한국어] 매핑에 실패하면. */
		dev_err(smmu->dev, "failed to ioremap\n");	/* [한국어] 레지스터 창을 못 잡으면 확장을 다룰 방법이 없다. */
		return NULL;	/* [한국어] 표준 큐로 돌아가라는 뜻. */
	}

	regval = readl(base + TEGRA241_CMDQV_CONFIG);	/* [한국어] 지금 설정을 읽어 둔다 — 아래에서 끌 때 다른 비트를 살리기 위해서다. */
	if (disable_cmdqv) {	/* [한국어] 부팅 인자로 확장을 껐다면. */
		dev_info(smmu->dev, "Detected disable_cmdqv=true\n");	/* [한국어] 의도한 설정임을 알린다 — 성능이 떨어져도 놀라지 않게. */
		writel(regval & ~CMDQV_EN, base + TEGRA241_CMDQV_CONFIG);	/* [한국어] 하드웨어를 확실히 꺼 둔다. 펌웨어가 켜 두었을 수 있다. */
		goto iounmap;	/* [한국어] 매핑을 풀고 표준 큐로 돌아간다. */
	}

	cmdqv = devm_krealloc(smmu->dev, smmu, sizeof(*cmdqv), GFP_KERNEL);	/* [한국어] 기존 SMMU 할당을 더 큰 크기로 늘린다 — 앞부분 내용은 그대로 살아남는다. */
	if (!cmdqv)	/* [한국어] 늘리지 못하면. */
		goto iounmap;	/* [한국어] 표준 큐로 돌아간다. 기존 할당은 그대로 유효하다. */
	new_smmu = &cmdqv->smmu;	/* [한국어] 늘어난 구조체 안의 표준 부분 — 주소는 같지만 이름을 분명히 한다. */

	cmdqv->irq = irq;	/* [한국어] 오류 인터럽트 번호. */
	cmdqv->base = base;	/* [한국어] 매핑한 레지스터 창. */
	cmdqv->dev = smmu->impl_dev;	/* [한국어] CMDQV 자체의 플랫폼 장치 — 표준 프로브가 참조를 잡아 두었다. */
	cmdqv->base_phys = res->start;	/* [한국어] 게스트에게 mmap 으로 넘길 때 필요한 물리 주소. */

	if (cmdqv->irq > 0) {	/* [한국어] 인터럽트가 있다면. */
		ret = request_threaded_irq(irq, NULL, tegra241_cmdqv_isr,	/* [한국어] 상위 처리기 없이 스레드 처리기만 등록한다 — 오류 처리가 잠들 수 있기 때문이다. */
					   IRQF_ONESHOT, "tegra241-cmdqv",	/* [한국어] 스레드가 끝날 때까지 인터럽트를 막아, 같은 오류로 몰리지 않게 한다. */
					   cmdqv);
		if (ret) {	/* [한국어] 등록에 실패하면. */
			dev_err(cmdqv->dev, "failed to request irq (%d): %d\n",	/* [한국어] 오류를 받을 수 없는 채로 가속을 켜면 문제를 놓치게 된다. */
				cmdqv->irq, ret);
			goto iounmap;	/* [한국어] 오류를 못 받는 채로 가속을 쓰는 것은 위험하므로 아예 접는다. */
		}
	}

	regval = readl_relaxed(REG_CMDQV(cmdqv, PARAM));	/* [한국어] 이 칩이 큐를 몇 개 가졌는지 읽는다. */
	cmdqv->num_vintfs = 1 << FIELD_GET(CMDQV_NUM_VINTF_LOG2, regval);	/* [한국어] 로그값이므로 되돌려 실제 개수를 구한다. */
	cmdqv->num_vcmdqs = 1 << FIELD_GET(CMDQV_NUM_VCMDQ_LOG2, regval);	/* [한국어] 전역 큐 개수. */
	cmdqv->num_lvcmdqs_per_vintf = cmdqv->num_vcmdqs / cmdqv->num_vintfs;	/* [한국어] VINTF 하나가 받을 몫 — 리셋 때 이 수만큼 연속으로 배정한다. */
	cmdqv->num_sids_per_vintf =	/* [한국어] VINTF 하나가 담을 스트림 치환 항목 수. */
		1 << FIELD_GET(CMDQV_NUM_SID_PER_VM_LOG2, regval);

	cmdqv->vintfs =	/* [한국어] 번호로 VINTF 를 찾을 배열. */
		kzalloc_objs(*cmdqv->vintfs, cmdqv->num_vintfs);
	if (!cmdqv->vintfs)	/* [한국어] 메모리가 없으면. */
		goto free_irq;	/* [한국어] 인터럽트부터 떼고 되감는다. */

	ida_init(&cmdqv->vintf_ids);	/* [한국어] VINTF 번호 할당기를 준비한다. */

#ifdef CONFIG_IOMMU_DEBUGFS	/* [한국어] debugfs 를 켜고 빌드했을 때만. */
	if (!cmdqv_debugfs_dir) {	/* [한국어] 칩이 여러 개여도 디렉터리는 하나만 만든다. */
		cmdqv_debugfs_dir =	/* [한국어] 디버그 디렉터리를 처음 한 번만 만든다. */
			debugfs_create_dir("tegra241_cmdqv", iommu_debugfs_dir);	/* [한국어] iommu 디버그 디렉터리 아래에 자리를 만든다. */
		debugfs_create_bool("bypass_vcmdq", 0644, cmdqv_debugfs_dir,	/* [한국어] 실행 중에 가속을 껐다 켤 수 있게 한다 — 성능 문제를 가를 때 유용하다. */
				    &bypass_vcmdq);
	}
#endif

	/* Provide init-level ops only, until tegra241_cmdqv_init_structures */
	/* [한국어] (위 영어 주석 참고) 아직 큐가 없으므로 큐 선택 갈고리를 걸면 안 된다.
	 * 자료 구조가 준비된 뒤에야 완전한 표로 바꿔 단다. */
	new_smmu->impl_ops = &init_ops;	/* [한국어] 프로브 단계용 축약 표를 건다. */

	return new_smmu;	/* [한국어] 표준 프로브가 이 포인터로 계속 진행한다. */

free_irq:	/* [한국어] 인터럽트를 등록한 뒤 실패했을 때의 되감기 지점. */
	if (cmdqv->irq > 0)	/* [한국어] 등록했던 경우에만. */
		free_irq(cmdqv->irq, cmdqv);	/* [한국어] 떼어 낸다. */
iounmap:	/* [한국어] 매핑만 했을 때의 되감기 지점. */
	iounmap(base);	/* [한국어] MMIO 매핑을 푼다. devm_krealloc 으로 늘린 구조체는 devres 가 알아서 거둔다. */
	return NULL;	/* [한국어] 표준 큐로 돌아가라는 뜻. */
}

/*
 * [한국어]
 * tegra241_cmdqv_probe - 이 SMMU 에 CMDQV 확장이 딸려 있는지 알아본다
 *
 * @smmu: 표준 프로브가 만들어 둔 SMMU.
 * @return: 확장을 품은 새 SMMU, 없으면 ERR_PTR(-ENODEV).
 *
 * 표준 드라이버가 ACPI 표에서 이 확장의 존재를 눈치채고 부르는 진입점이다.
 * 플랫폼 자원(MMIO 창과 인터럽트)을 꺼내 실제 프로브에 넘긴다.
 *
 * 실패하면 확장 옵션 비트를 내리고 장치 참조를 놓은 뒤 -ENODEV 를 돌려준다.
 * 표준 드라이버는 그것을 "확장 없음"으로 읽고 평범한 SMMU 로 계속 진행한다 —
 * 곧 이 확장이 없거나 망가져도 시스템은 정상 동작한다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_probe() → [이 함수] → __tegra241_cmdqv_probe()
 */
struct arm_smmu_device *tegra241_cmdqv_probe(struct arm_smmu_device *smmu)
{
	struct platform_device *pdev = to_platform_device(smmu->impl_dev);	/* [한국어] 확장은 별도의 플랫폼 장치로 나타난다. */
	struct arm_smmu_device *new_smmu;	/* [한국어] 성공하면 받을 새 포인터. */
	struct resource *res;	/* [한국어] MMIO 창 자원. */
	int irq;	/* [한국어] 오류 인터럽트. */

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);	/* [한국어] 레지스터 창을 찾는다. */
	if (!res) {	/* [한국어] 없다면 펌웨어 기술이 잘못된 것이다. */
		dev_err(&pdev->dev, "no memory resource found for CMDQV\n");	/* [한국어] 펌웨어가 이 확장을 기술했는데 창이 없다면 기술이 잘못된 것이다. */
		goto out_fallback;	/* [한국어] 표준 큐로 돌아간다. */
	}

	irq = platform_get_irq_optional(pdev, 0);	/* [한국어] 인터럽트는 없어도 동작은 한다 — optional 판을 쓴다. */
	if (irq <= 0)	/* [한국어] 없거나 잘못된 경우. */
		dev_warn(&pdev->dev,	/* [한국어] 오류를 알 길이 없다는 뜻이라 경고로 남긴다. */
			 "no interrupt. errors will not be reported\n");

	new_smmu = __tegra241_cmdqv_probe(smmu, res, irq);	/* [한국어] 실제 프로브. */
	if (new_smmu)	/* [한국어] 성공했다면. */
		return new_smmu;	/* [한국어] 확장을 품은 SMMU 를 돌려준다. */

out_fallback:	/* [한국어] 어떤 이유로든 확장을 쓸 수 없을 때. */
	dev_info(smmu->impl_dev, "Falling back to standard SMMU CMDQ\n");	/* [한국어] 성능만 떨어질 뿐 정상 동작한다는 뜻이라 정보 수준으로 남긴다. */
	smmu->options &= ~ARM_SMMU_OPT_TEGRA241_CMDQV;	/* [한국어] 확장 옵션을 내려, 이후 코드가 확장이 있다고 착각하지 않게 한다. */
	put_device(smmu->impl_dev);	/* [한국어] 표준 프로브가 잡아 둔 장치 참조를 놓는다 — 쓰지 않을 것이므로. */
	return ERR_PTR(-ENODEV);	/* [한국어] "그런 하드웨어 없음"으로 답한다. */
}

/* User space VINTF and VCMDQ Functions */

/*
 * [한국어]
 * tegra241_vintf_get_vcmdq_size - 게스트에게 줄 큐 객체의 크기를 알린다
 *
 * @viommu: 그 게스트의 VINTF (여기서는 쓰지 않는다).
 * @queue_type: 사용자가 요청한 큐 종류.
 * @return: 필요한 바이트 수, 지원하지 않는 종류면 0.
 *
 * iommufd 가 큐 객체를 자기가 잡되 크기는 드라이버에게 묻는다. 0 을
 * 돌려주면 요청이 거부되므로, 종류 검사의 관문이기도 하다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들지 않는다.
 *
 * 호출 체인:
 *   IOMMU_HW_QUEUE_ALLOC ioctl → iommufd → [이 함수]
 */
static size_t tegra241_vintf_get_vcmdq_size(struct iommufd_viommu *viommu,
					    enum iommu_hw_queue_type queue_type)
{
	if (queue_type != IOMMU_HW_QUEUE_TYPE_TEGRA241_CMDQV)	/* [한국어] 이 확장의 큐 종류가 아니라면. */
		return 0;	/* [한국어] 지원하지 않는다고 알린다. */
	return HW_QUEUE_STRUCT_SIZE(struct tegra241_vcmdq, core);	/* [한국어] 큐 구조체 크기. 매크로가 core 필드 위치도 함께 검사한다. */
}

/*
 * [한국어]
 * tegra241_vcmdq_hw_init_user - 게스트에게 넘길 큐를 하드웨어에 등록한다
 *
 * @vcmdq: 등록할 큐.
 * @return: 항상 0.
 *
 * 커널용 초기화와 결정적으로 다른 점은 큐를 켜지 않는다는 것이다. 링 버퍼
 * 주소만 하드웨어에 알려 두고, 실제로 켜는 일은 게스트가 자기 페이지0
 * 레지스터를 직접 써서 한다. 그래야 게스트가 자기 큐의 수명을 스스로
 * 다룰 수 있고, 호스트가 매번 끼어들 필요가 없다.
 *
 * 링 버퍼 주소는 게스트가 준 물리 주소인데, 그 값을 여기서 쓰는 것이
 * 안전한 이유는 호출자가 미리 정렬과 크기를 검사했고 그 주소가 결국
 * 2단계 변환 아래에 있기 때문이다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra241_vintf_alloc_lvcmdq_user() → [이 함수]
 */
static int tegra241_vcmdq_hw_init_user(struct tegra241_vcmdq *vcmdq)
{
	char header[64];	/* [한국어] 로그 머리말 버퍼. */

	/* Reset VCMDQ */
	/* [한국어] (위 영어 주석 참고) 앞 게스트의 상태가 남아 있을 수 있어 되돌린다. */
	tegra241_vcmdq_hw_deinit(vcmdq);	/* [한국어] 포인터와 오류를 깨끗이 한다. */

	/* Configure the vcmdq only; User space does the enabling */
	/* [한국어] (위 영어 주석 참고) 켜는 일은 게스트 몫이다. 호스트는 링 버퍼가
	 * 어디인지만 알려 준다 — 그 레지스터(페이지1)는 게스트에게 열어 주지 않는다. */
	writeq_relaxed(vcmdq->cmdq.q.q_base, REG_VCMDQ_PAGE1(vcmdq, BASE));	/* [한국어] 게스트가 준 주소와 크기를 담은 값. */

	dev_dbg(vcmdq->cmdqv->dev, "%sinited at host PA 0x%llx size 0x%lx\n",	/* [한국어] 어느 물리 주소를 어느 게스트에게 열어 줬는지 남긴다 — 문제를 되짚을 때 중요한 흔적이다. */
		lvcmdq_error_header(vcmdq, header, 64),
		vcmdq->cmdq.q.q_base & VCMDQ_ADDR,	/* [한국어] 값에서 주소 부분만 꺼내 찍는다. */
		1UL << (vcmdq->cmdq.q.q_base & VCMDQ_LOG2SIZE));	/* [한국어] 크기 부분은 로그값이라 되돌려 찍는다. */
	return 0;	/* [한국어] 실패할 일이 없지만 호출부 형태를 맞춘다. */
}

/*
 * [한국어]
 * tegra241_vintf_destroy_lvcmdq_user - 게스트에게 줬던 큐를 거둔다
 *
 * @hw_queue: iommufd 가 넘긴 큐 객체.
 *
 * 게스트가 큐를 놓거나 가상 머신이 사라질 때 iommufd 가 부른다. 하드웨어를
 * 끄고, 배정을 풀고, 배열에서 떼어 낸다.
 *
 * 마지막에 앞 큐에 대한 의존을 푸는 것이 중요하다. 하드웨어가 내림차순
 * 해제를 요구하므로, 큐를 배정할 때 "나는 앞 큐에 기대고 있다"고 iommufd 에
 * 등록해 두었다. 그래야 iommufd 가 앞 큐를 먼저 놓으려 할 때 막아 준다.
 *
 * 실행 컨텍스트: iommufd 객체 해제. mutex 를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   iommufd 객체 해제 → hw_queue->destroy = [이 함수]
 *     → tegra241_vcmdq_hw_deinit() → tegra241_vintf_free_lvcmdq()
 */
static void
tegra241_vintf_destroy_lvcmdq_user(struct iommufd_hw_queue *hw_queue)
{
	struct tegra241_vcmdq *vcmdq = hw_queue_to_vcmdq(hw_queue);	/* [한국어] iommufd 객체에서 이 드라이버의 큐로 되짚는다. */

	mutex_lock(&vcmdq->vintf->lvcmdq_mutex);	/* [한국어] 같은 VINTF 에 큐를 붙이려는 요청과 겹치지 않게 막는다. */
	tegra241_vcmdq_hw_deinit(vcmdq);	/* [한국어] 먼저 하드웨어를 끈다 — 게스트가 켜 둔 채 놓았을 수 있다. */
	tegra241_vcmdq_unmap_lvcmdq(vcmdq);	/* [한국어] 그다음 배정을 푼다. */
	tegra241_vintf_free_lvcmdq(vcmdq->vintf, vcmdq->lidx);	/* [한국어] 배열에서 뗀다. 구조체는 iommufd 가 놓으므로 여기서 놓지 않는다. */
	if (vcmdq->prev)	/* [한국어] 앞 큐에 의존을 걸어 두었다면. */
		iommufd_hw_queue_undepend(vcmdq, vcmdq->prev, core);	/* [한국어] 그 의존을 푼다 — 이제 앞 큐도 놓일 수 있다. */
	mutex_unlock(&vcmdq->vintf->lvcmdq_mutex);	/* [한국어] 정리가 끝났으니 다른 요청이 들어올 수 있게 한다. */
}

/*
 * [한국어]
 * tegra241_vintf_alloc_lvcmdq_user - 게스트에게 논리 큐 하나를 넘긴다
 *
 * @hw_queue: iommufd 가 이미 잡아 둔 큐 객체.
 * @lidx: 게스트가 요청한 논리 큐 번호.
 * @base_addr_pa: 게스트가 마련한 링 버퍼의 물리 주소.
 * @return: 0 성공, 음수 오류.
 *
 * 게스트 가속의 핵심 경로다. 게스트가 자기 메모리에 링 버퍼를 마련하고
 * 그 주소를 넘기면, 호스트는 검사를 거쳐 하드웨어에 등록해 준다. 그 뒤로
 * 게스트는 그 버퍼에 명령을 직접 써 넣고 자기 페이지0 레지스터로 생산
 * 포인터를 밀며, 호스트 커널은 전혀 개입하지 않는다.
 *
 * 검사가 세 겹이다. 번호가 범위 안인지, 아직 비어 있는지, 앞 번호가 이미
 * 배정되어 있는지(하드웨어가 오름차순을 요구한다). 그다음 링 버퍼의 크기가
 * 2의 거듭제곱이고 하드웨어 한계 안인지, 주소가 그 크기에 맞춰 정렬되어
 * 있는지를 본다. 정렬 검사를 빠뜨리면 하드웨어가 주소를 잘라 읽어 엉뚱한
 * 메모리를 명령으로 해석하게 된다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들 수 있다.
 *
 * 호출 체인:
 *   IOMMU_HW_QUEUE_ALLOC ioctl → iommufd → viommu_ops->hw_queue_init_phys
 *     = [이 함수] → tegra241_vcmdq_hw_init_user()
 */
static int tegra241_vintf_alloc_lvcmdq_user(struct iommufd_hw_queue *hw_queue,
					    u32 lidx, phys_addr_t base_addr_pa)
{
	struct tegra241_vintf *vintf = viommu_to_vintf(hw_queue->viommu);	/* [한국어] 이 큐가 속할 게스트의 VINTF. */
	struct tegra241_vcmdq *vcmdq = hw_queue_to_vcmdq(hw_queue);	/* [한국어] iommufd 가 잡아 둔 큐 구조체. */
	struct tegra241_cmdqv *cmdqv = vintf->cmdqv;	/* [한국어] 전역 한계값을 볼 문맥. */
	struct arm_smmu_device *smmu = &cmdqv->smmu;	/* [한국어] 큐 크기 한계를 읽을 SMMU. */
	struct tegra241_vcmdq *prev = NULL;	/* [한국어] 바로 앞 번호의 큐 — 의존을 걸 대상이다. */
	u32 log2size, max_n_shift;	/* [한국어] 요청 크기(로그)와 하드웨어 상한. */
	char header[64];	/* [한국어] 로그 머리말 버퍼. */
	int ret;	/* [한국어] 중간 결과. */

	if (hw_queue->type != IOMMU_HW_QUEUE_TYPE_TEGRA241_CMDQV)	/* [한국어] 종류가 다르면. */
		return -EOPNOTSUPP;	/* [한국어] 우리가 다룰 큐가 아니다. */
	if (lidx >= cmdqv->num_lvcmdqs_per_vintf)	/* [한국어] 이 VINTF 가 가질 수 있는 번호를 넘었다면. */
		return -EINVAL;	/* [한국어] 게스트가 잘못된 번호를 요청한 것이다. */

	mutex_lock(&vintf->lvcmdq_mutex);	/* [한국어] 여기부터 배열을 건드리므로, 동시 요청이 겹치지 않게 막는다. */

	if (vintf->lvcmdqs[lidx]) {	/* [한국어] 그 번호가 이미 쓰이고 있다면. */
		ret = -EEXIST;	/* [한국어] 같은 번호를 두 번 줄 수는 없다. */
		goto unlock;	/* [한국어] 이미 쓰이는 번호라 되돌릴 것 없이 락만 풀고 나간다. */
	}

	/*
	 * HW requires to map LVCMDQs in ascending order, so reject if the
	 * previous lvcmdqs is not allocated yet.
	 */
	/* [한국어] (위 영어 주석 참고) 하드웨어가 오름차순 배정을 요구하므로,
	 * 앞 번호를 건너뛰고 요청하면 거부한다. 게스트가 순서를 지켜야 한다. */
	if (lidx) {	/* [한국어] 0번이 아니라면 앞 번호가 있어야 한다. */
		prev = vintf->lvcmdqs[lidx - 1];	/* [한국어] 앞 번호의 큐. */
		if (!prev) {	/* [한국어] 아직 없다면. */
			ret = -EIO;	/* [한국어] 하드웨어 규약 위반이므로 거부한다. */
			goto unlock;	/* [한국어] 순서를 어긴 요청이라 락만 풀고 거부한다. */
		}
	}

	/*
	 * hw_queue->length must be a power of 2, in range of
	 *   [ 32, 2 ^ (idr[1].CMDQS + CMDQ_ENT_SZ_SHIFT) ]
	 */
	/* [한국어] (위 영어 주석 참고) 링 버퍼 크기는 2의 거듭제곱이어야 하고,
	 * 명령 두 개(32바이트)보다 크고 하드웨어 상한보다 작아야 한다.
	 * 이 검사를 빠뜨리면 하드웨어가 크기 필드를 잘라 읽어, 게스트가 의도한
	 * 것보다 넓은 메모리를 큐로 해석하게 된다. */
	max_n_shift = FIELD_GET(IDR1_CMDQS,	/* [한국어] SMMU 가 지원하는 최대 큐 크기(항목 수의 로그). */
				readl_relaxed(smmu->base + ARM_SMMU_IDR1));
	if (!is_power_of_2(hw_queue->length) || hw_queue->length < 32 ||	/* [한국어] 2의 거듭제곱이 아니거나 너무 작거나. */
	    hw_queue->length > (1 << (max_n_shift + CMDQ_ENT_SZ_SHIFT))) {	/* [한국어] 하드웨어 상한을 넘으면 — 항목 수를 바이트로 바꿔 견준다. */
		ret = -EINVAL;	/* [한국어] 크기가 규약에 맞지 않는다. */
		goto unlock;	/* [한국어] 아직 아무것도 바꾸지 않았으므로 락만 풀면 된다. */
	}
	log2size = ilog2(hw_queue->length) - CMDQ_ENT_SZ_SHIFT;	/* [한국어] 바이트 크기를 항목 수의 로그로 바꾼다 — 레지스터가 그 형식을 쓴다. */

	/* base_addr_pa must be aligned to hw_queue->length */
	/* [한국어] (위 영어 주석 참고) 하드웨어는 주소의 아래 비트를 무시하므로,
	 * 정렬되지 않은 주소를 주면 게스트가 의도하지 않은 메모리를 가리키게 된다.
	 * 게스트가 준 값을 그대로 믿을 수 없는 이유가 여기 있다. */
	if (base_addr_pa & ~VCMDQ_ADDR ||	/* [한국어] 하드웨어가 표현할 수 없는 상위 비트가 켜져 있거나. */
	    base_addr_pa & (hw_queue->length - 1)) {	/* [한국어] 크기 경계에 맞춰 정렬되지 않았다면. */
		ret = -EINVAL;	/* [한국어] 주소가 정렬되지 않았거나 표현 범위를 넘었다. */
		goto unlock;	/* [한국어] 역시 되돌릴 것이 없다. */
	}

	/*
	 * HW requires to unmap LVCMDQs in descending order, so destroy() must
	 * follow this rule. Set a dependency on its previous LVCMDQ so iommufd
	 * core will help enforce it.
	 */
	/* [한국어] (위 영어 주석 참고) 해제 순서까지 강제해야 하는데, 게스트가
	 * 놓는 순서를 우리가 정할 수는 없다. 그래서 iommufd 에 "이 큐는 앞 큐에
	 * 기대고 있다"고 등록해, 코어가 순서를 대신 지켜 주게 한다. */
	if (prev) {	/* [한국어] 앞 큐가 있다면. */
		ret = iommufd_hw_queue_depend(vcmdq, prev, core);	/* [한국어] 의존 관계를 건다 — 앞 큐가 먼저 놓이지 못하게 된다. */
		if (ret)	/* [한국어] 의존을 걸지 못하면 해제 순서를 지킬 수 없어 배정을 접는다. */
			goto unlock;
	}
	vcmdq->prev = prev;	/* [한국어] 해제 때 의존을 풀 수 있게 기억해 둔다. */

	ret = tegra241_vintf_init_lvcmdq(vintf, lidx, vcmdq);	/* [한국어] 번호와 레지스터 주소를 채우고 배열에 건다. */
	if (ret)	/* [한국어] 배열에 걸지 못했으니 의존부터 되감는다. */
		goto undepend_vcmdq;

	dev_dbg(cmdqv->dev, "%sallocated\n",	/* [한국어] 게스트에게 큐를 넘긴 사실을 남긴다. */
		lvcmdq_error_header(vcmdq, header, 64));

	tegra241_vcmdq_map_lvcmdq(vcmdq);	/* [한국어] 배정을 살려 레지스터에 접근할 수 있게 한다. */

	vcmdq->cmdq.q.q_base = base_addr_pa & VCMDQ_ADDR;	/* [한국어] 게스트가 준 주소에서 하드웨어가 쓰는 비트만 남긴다. */
	vcmdq->cmdq.q.q_base |= log2size;	/* [한국어] 아래 비트에 크기를 겹쳐 담는다 — 커널용 큐와 같은 형식이다. */

	ret = tegra241_vcmdq_hw_init_user(vcmdq);	/* [한국어] 하드웨어에 등록한다. 켜는 일은 게스트가 한다. */
	if (ret)	/* [한국어] 하드웨어 등록에 실패했으니 배정을 되돌린다. */
		goto unmap_lvcmdq;

	hw_queue->destroy = &tegra241_vintf_destroy_lvcmdq_user;	/* [한국어] 이제부터 이 객체가 사라질 때 우리 정리 함수가 불린다 — 성공한 뒤에 걸어야 이중 정리를 피한다. */
	mutex_unlock(&vintf->lvcmdq_mutex);	/* [한국어] 배정이 끝났으니 락을 놓는다. */
	return 0;	/* [한국어] 게스트가 이 큐로 직접 명령을 낼 수 있게 됐다. */

unmap_lvcmdq:	/* [한국어] 배정을 살린 뒤 실패했을 때. */
	tegra241_vcmdq_unmap_lvcmdq(vcmdq);	/* [한국어] 배정을 도로 푼다. */
	tegra241_vintf_deinit_lvcmdq(vintf, lidx);	/* [한국어] 배열 칸을 비운다. */
undepend_vcmdq:	/* [한국어] 의존을 건 뒤 실패했을 때. */
	if (vcmdq->prev)	/* [한국어] 걸어 두었다면. */
		iommufd_hw_queue_undepend(vcmdq, vcmdq->prev, core);	/* [한국어] 푼다 — 빠뜨리면 앞 큐가 영원히 놓이지 않는다. */
unlock:	/* [한국어] 락만 잡았을 때. */
	mutex_unlock(&vintf->lvcmdq_mutex);	/* [한국어] 되감기가 끝났으니 락을 놓는다. */
	return ret;	/* [한국어] 되감기를 마치고 실패 이유를 돌려준다. */
}

/*
 * [한국어]
 * tegra241_cmdqv_destroy_vintf_user - 게스트에게 줬던 VINTF 를 거둔다
 *
 * @viommu: iommufd 가 넘긴 가상 IOMMU 객체.
 *
 * 가상 머신이 사라지거나 사용자가 객체를 놓을 때 불린다. 게스트에게 열어
 * 줬던 mmap 매핑을 먼저 거두고, 그다음 VINTF 자원을 정리한다. 순서가
 * 이래야 게스트가 이미 사라진 레지스터에 접근하는 일이 없다.
 *
 * 실행 컨텍스트: iommufd 객체 해제. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommufd 객체 해제 → viommu_ops->destroy = [이 함수]
 *     → tegra241_cmdqv_remove_vintf()
 */
static void tegra241_cmdqv_destroy_vintf_user(struct iommufd_viommu *viommu)
{
	struct tegra241_vintf *vintf = viommu_to_vintf(viommu);	/* [한국어] 이 드라이버의 VINTF 로 되짚는다. */

	if (vintf->mmap_offset)	/* [한국어] 게스트에게 레지스터 페이지를 열어 줬다면. */
		iommufd_viommu_destroy_mmap(&vintf->vsmmu.core,	/* [한국어] 그 매핑을 거둔다 — 이 뒤로 게스트는 레지스터에 닿을 수 없다. */
					    vintf->mmap_offset);
	tegra241_cmdqv_remove_vintf(vintf->cmdqv, vintf->idx);	/* [한국어] 하드웨어를 끄고 자원을 되돌린다. */
}

/*
 * [한국어]
 * tegra241_vintf_destroy_vsid - 스트림 치환표의 한 칸을 비운다
 *
 * @vdev: iommufd 가 넘긴 가상 장치 객체.
 *
 * 게스트가 장치를 놓을 때 불린다. 치환표에서 그 대응을 지우고 칸 번호를
 * 돌려준다. 일치 조건을 먼저 지우는 순서가 중요하다 — 그래야 지우는 도중에
 * 엉뚱한 치환이 일어나지 않는다.
 *
 * 실행 컨텍스트: iommufd 객체 해제. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommufd 객체 해제 → vdev->destroy = [이 함수]
 */
static void tegra241_vintf_destroy_vsid(struct iommufd_vdevice *vdev)
{
	struct tegra241_vintf_sid *vsid = vdev_to_vsid(vdev);	/* [한국어] 이 드라이버의 대응 구조로 되짚는다. */
	struct tegra241_vintf *vintf = vsid->vintf;	/* [한국어] 그 대응이 속한 VINTF. */

	writel(0, REG_VINTF(vintf, SID_MATCH(vsid->idx)));	/* [한국어] 먼저 일치 조건을 지운다 — 이제 이 칸은 아무 번호도 잡지 않는다. */
	writel(0, REG_VINTF(vintf, SID_REPLACE(vsid->idx)));	/* [한국어] 그다음 치환 값을 지운다. */
	ida_free(&vintf->sids, vsid->idx);	/* [한국어] 칸 번호를 돌려줘 다른 장치가 쓸 수 있게 한다. */
	dev_dbg(vintf->cmdqv->dev,	/* [한국어] 어느 대응이 사라졌는지 남긴다. */
		"VINTF%u: deallocated SID_REPLACE%d for pSID=%x\n", vintf->idx,
		vsid->idx, vsid->sid);
}

/*
 * [한국어]
 * tegra241_vintf_init_vsid - 게스트 장치 번호를 실제 번호로 잇는다
 *
 * @vdev: iommufd 가 잡아 둔 가상 장치 객체 (게스트가 붙인 번호를 담고 있다).
 * @return: 0 성공, 음수 오류.
 *
 * 가속 큐의 격리를 완성하는 함수다. 게스트가 무효화 명령에 "내 5번 장치"라고
 * 적으면, 하드웨어가 치환표를 보고 실제 스트림 번호로 바꿔 읽는다. 이 대응이
 * 없으면 게스트가 아무 번호나 적어 남의 장치 캐시를 지울 수 있으므로,
 * 게스트가 등록한 장치만 이 표에 오른다는 사실 자체가 보안 장치다.
 *
 * MATCH 레지스터에 유효 비트를 함께 켜는 것(<<1 | 0x1)이 눈에 띈다 —
 * 하드웨어가 번호와 "이 칸이 살아 있음"을 한 레지스터에 담기 때문이다.
 * REPLACE 를 먼저 쓰고 MATCH 를 나중에 쓰는 순서도 의도적이다. 반대로 하면
 * 치환 값이 아직 없는 상태에서 일치가 일어날 수 있다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들 수 있다.
 *
 * 호출 체인:
 *   IOMMU_VDEVICE_ALLOC ioctl → iommufd → viommu_ops->vdevice_init = [이 함수]
 */
static int tegra241_vintf_init_vsid(struct iommufd_vdevice *vdev)
{
	struct device *dev = iommufd_vdevice_to_device(vdev);	/* [한국어] 게스트에게 넘길 실제 장치. */
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);	/* [한국어] 그 장치의 SMMU 쪽 상태 — 스트림 번호가 여기 있다. */
	struct tegra241_vintf *vintf = viommu_to_vintf(vdev->viommu);	/* [한국어] 그 게스트의 VINTF. */
	struct tegra241_vintf_sid *vsid = vdev_to_vsid(vdev);	/* [한국어] iommufd 가 잡아 둔 대응 구조체. */
	struct arm_smmu_stream *stream = &master->streams[0];	/* [한국어] 첫 번째 스트림 — 아래에서 하나뿐임을 확인한다. */
	u64 virt_sid = vdev->virt_id;	/* [한국어] 게스트가 이 장치에 붙인 번호. */
	int sidx;	/* [한국어] 치환표에서 받을 칸 번호. */

	if (virt_sid > UINT_MAX)	/* [한국어] 스트림 번호는 32비트다. */
		return -EINVAL;	/* [한국어] 그보다 큰 값은 레지스터에 담을 수 없다. */

	WARN_ON_ONCE(master->num_streams != 1);	/* [한국어] 지금은 스트림이 하나뿐인 장치만 넘길 수 있다 — 여러 개면 어느 것을 치환할지 정할 수 없다. */

	/* Find an empty pair of SID_REPLACE and SID_MATCH */
	/* [한국어] (위 영어 주석 참고) 두 레지스터가 짝을 이루므로 칸 번호 하나가
	 * 곧 그 짝을 가리킨다. */
	sidx = ida_alloc_max(&vintf->sids, vintf->cmdqv->num_sids_per_vintf - 1,	/* [한국어] 이 VINTF 의 치환표 안에서 빈 칸을 받는다. */
			     GFP_KERNEL);
	if (sidx < 0)	/* [한국어] 칸이 다 찼다면 — 게스트에게 넘길 수 있는 장치 수의 한계다. */
		return sidx;

	writel(stream->id, REG_VINTF(vintf, SID_REPLACE(sidx)));	/* [한국어] 먼저 실제 번호를 채운다 — 일치가 일어나기 전에 준비되어 있어야 한다. */
	writel(virt_sid << 1 | 0x1, REG_VINTF(vintf, SID_MATCH(sidx)));	/* [한국어] 게스트 번호를 한 칸 올리고 유효 비트를 켜 넣는다 — 이 순간부터 치환이 살아난다. */
	dev_dbg(vintf->cmdqv->dev,	/* [한국어] 어느 게스트 번호가 어느 실제 번호로 이어졌는지 남긴다. */
		"VINTF%u: allocated SID_REPLACE%d for pSID=%x, vSID=%x\n",
		vintf->idx, sidx, stream->id, (u32)virt_sid);

	vsid->idx = sidx;	/* [한국어] 해제 때 지울 칸 번호. */
	vsid->vintf = vintf;	/* [한국어] 해제 때 닿아야 할 VINTF. */
	vsid->sid = stream->id;	/* [한국어] 로그에 쓸 실제 번호. */

	vdev->destroy = &tegra241_vintf_destroy_vsid;	/* [한국어] 성공한 뒤에 정리 갈고리를 건다 — 실패 경로에서 불리지 않게. */
	return 0;	/* [한국어] 치환표 한 칸이 살아났다 — 이제 게스트가 이 장치를 지목할 수 있다. */
}

/* [한국어] 게스트에게 넘긴 가속 VINTF 의 연산표.
 *
 * 도메인 할당과 무효화는 표준 iommufd 연동의 함수를 그대로 쓴다 — 가속을
 * 쓴다고 해서 그 규칙이 달라지지는 않기 때문이다. 이 확장이 더하는 것은
 * 가상 장치 등록(치환표)과 하드웨어 큐 배정 둘이며, 그 둘이 있어야 게스트가
 * 명령을 직접 낼 수 있다. */
static struct iommufd_viommu_ops tegra241_cmdqv_viommu_ops = {
	.destroy = tegra241_cmdqv_destroy_vintf_user,	/* [한국어] VINTF 를 거두는 갈고리. */
	.alloc_domain_nested = arm_vsmmu_alloc_domain_nested,	/* [한국어] 중첩 도메인 만들기는 표준 연동의 것을 그대로 쓴다. */
	/* Non-accelerated commands will be still handled by the kernel */
	/* [한국어] (위 영어 주석 참고) 게스트 큐가 받지 못하는 명령은 여전히
	 * 커널이 걸러서 대신 실행한다 — 가속은 흔한 무효화에만 적용된다. */
	.cache_invalidate = arm_vsmmu_cache_invalidate,	/* [한국어] 그 대리 실행도 표준 연동의 것을 쓴다. */
	.vdevice_size = VDEVICE_STRUCT_SIZE(struct tegra241_vintf_sid, core),	/* [한국어] 가상 장치 객체의 크기 — 치환표 칸 정보를 담아야 해서 표준보다 크다. */
	.vdevice_init = tegra241_vintf_init_vsid,	/* [한국어] 그 객체를 만들며 치환표를 채우는 갈고리. */
	.get_hw_queue_size = tegra241_vintf_get_vcmdq_size,	/* [한국어] 큐 객체 크기를 알리는 갈고리. */
	.hw_queue_init_phys = tegra241_vintf_alloc_lvcmdq_user,	/* [한국어] 게스트가 준 물리 주소로 큐를 배정하는 갈고리 — 가속의 입구다. */
};

/*
 * [한국어]
 * tegra241_cmdqv_init_vintf_user - 게스트에게 VINTF 하나를 배정한다
 *
 * @vsmmu: 표준 연동이 이미 채워 둔 가상 SMMU (실제로는 VINTF 를 품고 있다).
 * @user_data: 사용자가 넘긴 요청 데이터. 결과도 여기에 적어 돌려준다.
 * @return: 0 성공, 음수 오류.
 *
 * 표준 arm_vsmmu_init() 이 자기가 모르는 종류를 만나면 이 함수로 넘긴다.
 * VINTF 번호를 하나 배정하고, 게스트 소유로 하드웨어를 켜고, 그 레지스터
 * 페이지를 사용자 공간이 mmap 할 수 있게 열어 준다.
 *
 * 게스트 소유 VINTF 에는 큐를 미리 잡아 두지 않는다는 점이 커널용과 결정적으로
 * 다르다. 게스트가 실제로 요청할 때까지 큐를 주지 않아야, 쓰지도 않을 게스트가
 * 큐를 붙잡아 두는 일이 없고 필요 이상의 자원을 노출하지도 않는다.
 *
 * 페이지0 만 열어 주는 것도 중요하다. 링 버퍼 주소가 있는 페이지1 을 열면
 * 게스트가 아무 물리 주소나 큐로 지정할 수 있게 되어 격리가 무너진다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들 수 있다.
 *
 * 호출 체인:
 *   IOMMU_VIOMMU_ALLOC ioctl → arm_vsmmu_init() → impl_ops->vsmmu_init
 *     = [이 함수] → tegra241_vintf_hw_init() → iommufd_viommu_alloc_mmap()
 */
static int
tegra241_cmdqv_init_vintf_user(struct arm_vsmmu *vsmmu,
			       const struct iommu_user_data *user_data)
{
	struct tegra241_cmdqv *cmdqv =	/* [한국어] 전역 문맥으로 되짚는다. */
		container_of(vsmmu->smmu, struct tegra241_cmdqv, smmu);
	struct tegra241_vintf *vintf = viommu_to_vintf(&vsmmu->core);	/* [한국어] iommufd 가 잡아 둔 VINTF 구조체. */
	struct iommu_viommu_tegra241_cmdqv data;	/* [한국어] 사용자와 주고받을 데이터 — mmap 오프셋을 돌려준다. */
	phys_addr_t page0_base;	/* [한국어] 게스트에게 열어 줄 페이지의 물리 주소. */
	int ret;	/* [한국어] 중간 결과. */

	/*
	 * Unsupported type should be rejected by tegra241_cmdqv_get_vintf_size.
	 * Seeing one here indicates a kernel bug or some data corruption.
	 */
	/* [한국어] (위 영어 주석 참고) 종류 검사는 크기를 묻는 단계에서 이미 끝났다.
	 * 여기까지 다른 종류가 왔다면 코드나 자료가 망가진 것이므로 경고를 남긴다. */
	if (WARN_ON(vsmmu->core.type != IOMMU_VIOMMU_TYPE_TEGRA241_CMDQV))
		return -EOPNOTSUPP;

	if (!user_data)	/* [한국어] 이 종류는 반드시 데이터를 함께 받아야 한다 — 결과를 돌려줄 자리가 필요하기 때문이다. */
		return -EINVAL;

	ret = iommu_copy_struct_from_user(&data, user_data,	/* [한국어] 사용자 구조체를 안전하게 복사해 온다. */
					  IOMMU_VIOMMU_TYPE_TEGRA241_CMDQV,
					  out_vintf_mmap_length);	/* [한국어] 마지막 필드 이름을 넘겨 버전이 다른 구조체도 다룰 수 있게 한다. */
	if (ret)	/* [한국어] 사용자 데이터를 못 읽으면 요청을 처리할 수 없다. */
		return ret;	/* [한국어] 가속을 받을 수 있는 가상 머신 수의 한계에 닿았다. */

	ret = tegra241_cmdqv_init_vintf(cmdqv, cmdqv->num_vintfs - 1, vintf);	/* [한국어] 0번을 뺀 나머지 범위에서 번호를 받는다 — 0번은 커널 몫이라 이미 잡혀 있다. */
	if (ret < 0) {	/* [한국어] 남은 VINTF 가 없다면. */
		dev_err(cmdqv->dev, "no more available vintf\n");	/* [한국어] 가속을 받을 수 있는 가상 머신 수의 한계에 닿은 것이다. */
		return ret;	/* [한국어] 되감기를 마치고 실패 이유를 사용자에게 돌려준다. */
	}

	/*
	 * Initialize the user-owned VINTF without a LVCMDQ, as it cannot pre-
	 * allocate a LVCMDQ until user space wants one, for security reasons.
	 * It is different than the kernel-owned VINTF0, which had pre-assigned
	 * and pre-allocated global VCMDQs that would be mapped to the LVCMDQs
	 * by the tegra241_vintf_hw_init() call.
	 */
	/* [한국어] (위 영어 주석 참고) 게스트용은 큐 없이 켠다. 큐 배열은 모두 NULL
	 * 이므로 hw_init 안의 큐 순회가 아무 일도 하지 않는다. hyp_own 을 거짓으로
	 * 주는 것도 중요한데, 그래야 이 VINTF 의 큐가 제한된 명령만 받게 된다. */
	ret = tegra241_vintf_hw_init(vintf, false);	/* [한국어] VMID 를 걸고 게스트 소유로 켠다. */
	if (ret)	/* [한국어] 하드웨어를 못 켰으니 받은 번호부터 되돌린다. */
		goto deinit_vintf;

	page0_base = cmdqv->base_phys + TEGRA241_VINTFi_PAGE0(vintf->idx);	/* [한국어] 이 VINTF 의 페이지0 물리 주소 — 커널 주소가 아니라 물리 주소여야 mmap 할 수 있다. */
	ret = iommufd_viommu_alloc_mmap(&vintf->vsmmu.core, page0_base, SZ_64K,	/* [한국어] 페이지0 만 64K 열어 준다. 페이지1(링 버퍼 주소)은 절대 열지 않는다. */
					&vintf->mmap_offset);
	if (ret)	/* [한국어] 매핑을 못 열면 게스트가 큐 레지스터에 닿을 수 없다. */
		goto hw_deinit_vintf;

	data.out_vintf_mmap_length = SZ_64K;	/* [한국어] 얼마나 매핑할 수 있는지. */
	data.out_vintf_mmap_offset = vintf->mmap_offset;	/* [한국어] mmap() 에 넘길 오프셋 — 이것이 있어야 사용자가 그 페이지에 닿는다. */
	ret = iommu_copy_struct_to_user(user_data, &data,	/* [한국어] 결과를 사용자 공간으로 돌려준다. */
					IOMMU_VIOMMU_TYPE_TEGRA241_CMDQV,
					out_vintf_mmap_length);
	if (ret)	/* [한국어] 결과를 못 돌려주면 사용자가 매핑을 쓸 방법이 없다. */
		goto free_mmap;

	ida_init(&vintf->sids);	/* [한국어] 스트림 치환표 칸 할당기를 준비한다 — 게스트용에만 필요하다. */
	mutex_init(&vintf->lvcmdq_mutex);	/* [한국어] 게스트가 큐를 동시에 요청할 때를 대비한 락. */

	dev_dbg(cmdqv->dev, "VINTF%u: allocated with vmid (%d)\n", vintf->idx,	/* [한국어] 어느 VINTF 가 어느 VMID 로 게스트에게 갔는지 남긴다. */
		vintf->vsmmu.vmid);

	vsmmu->core.ops = &tegra241_cmdqv_viommu_ops;	/* [한국어] 가속 연산표를 건다 — 이제 게스트가 큐와 장치를 요청할 수 있다. */
	return 0;	/* [한국어] 게스트가 이제 큐와 장치를 요청할 수 있다. */

free_mmap:	/* [한국어] mmap 을 열어 둔 뒤 실패했을 때. */
	iommufd_viommu_destroy_mmap(&vintf->vsmmu.core, vintf->mmap_offset);	/* [한국어] 열어 준 매핑을 거둔다. */
hw_deinit_vintf:	/* [한국어] 하드웨어를 켠 뒤 실패했을 때. */
	tegra241_vintf_hw_deinit(vintf);	/* [한국어] 다시 끈다 — 켜진 채로 두면 게스트 없는 VINTF 가 살아 있게 된다. */
deinit_vintf:	/* [한국어] 번호만 받은 뒤 실패했을 때. */
	tegra241_cmdqv_deinit_vintf(cmdqv, vintf->idx);	/* [한국어] 번호와 배열을 되돌린다. 구조체는 iommufd 가 놓는다. */
	return ret;	/* [한국어] 되감기를 마치고 실패 이유를 사용자에게 돌려준다. */
}

MODULE_IMPORT_NS("IOMMUFD");	/* [한국어] iommufd 가 자기 이름공간으로 내보낸 심볼을 쓸 수 있게 한다 — 아무 모듈이나 그 내부를 부르지 못하게 막는 장치다. */
