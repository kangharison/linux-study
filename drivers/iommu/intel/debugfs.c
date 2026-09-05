// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright © 2018 Intel Corporation.
 *
 * Authors: Gayatri Kammela <gayatri.kammela@intel.com>
 *	    Sohil Mehta <sohil.mehta@intel.com>
 *	    Jacob Pan <jacob.jun.pan@linux.intel.com>
 *	    Lu Baolu <baolu.lu@linux.intel.com>
 */

/*
 * [한국어 설명] Intel VT-d 하드웨어 상태를 사람이 읽을 수 있게 드러내는 창 (debugfs.c)
 *
 * === 파일의 역할 ===
 * VT-d 드라이버가 하드웨어에 만들어 둔 자료구조 — 레지스터, 루트/컨텍스트
 * 표, PASID 디렉터리와 표, 페이지 테이블, 무효화 큐, IRTE(인터럽트 재매핑
 * 표) — 를 debugfs 파일로 노출한다. 코드 경로에는 아무 영향을 주지 않고,
 * 오직 "지금 하드웨어가 무엇을 보고 있는가"를 읽어서 찍는다.
 *
 * 이 파일이 필요한 이유는 IOMMU 버그의 성격에 있다. DMA 가 엉뚱한 곳에
 * 닿거나 인터럽트가 사라졌을 때, 커널 쪽 자료구조는 멀쩡해 보여도 하드웨어가
 * 참조하는 물리 메모리의 표가 다를 수 있다. printk 로는 이 계층 구조를
 * 훑을 수 없어, 표를 실제로 걸어 내려가며 덤프하는 수단이 따로 필요하다.
 *
 * 대부분의 함수가 재귀적 "walk" 인 것도 그 때문이다. 루트 표(버스) →
 * 컨텍스트 표(devfn) → [스케일러블 모드면 PASID 디렉터리 → PASID 표] →
 * 페이지 테이블의 다섯 단계를, 하드웨어가 밟는 것과 똑같은 순서로 밟는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * VT-d 드라이버의 곁가지다. iommu.c/pasid.c/dmar.c 가 만든 상태를 읽기만
 * 하고 아무것도 바꾸지 않는다(예외: 하드웨어 성능 카운터를 켜고 끄는
 * 파일). 실행 컨텍스트는 사용자가 debugfs 파일을 read(2) 할 때의 프로세스
 * 문맥이며, 물리 메모리를 직접 따라가므로 표를 바꾸는 경로와 부딪히지
 * 않도록 iommu->lock 이나 RCU 로 보호한다.
 *
 * 호출 체인:
 *   사용자의 cat /sys/kernel/debug/iommu/intel/... → seq_file 의 show 콜백
 *     → root_tbl_walk() → ctx_tbl_walk() → pasid_dir_walk() → pasid_tbl_walk()
 *     → print_tbl_walk()
 *   intel_iommu_debugfs_init() ← iommu_init 경로에서 한 번 불려 파일들을 만든다
 *
 * === 타 모듈과의 연결 ===
 * iommu.h 의 struct intel_iommu 로 유닛과 그 레지스터·표에 접근하고,
 * pasid.h 의 PASID 항목 해석 도우미(pasid_pte_is_present,
 * get_pasid_table_from_pde)를 그대로 쓴다. perf.h 의 성능 카운터,
 * asm/irq_remapping.h 의 IRTE 정의도 끌어온다.
 *
 * 데이터 흐름은 한 방향이다: 하드웨어/메모리의 표 → 이 파일의 walk 함수 →
 * seq_file 버퍼 → 사용자. 되돌아가는 흐름은 없다.
 *
 * 공유 상태: 표를 걸어 내려가는 동안 다른 CPU 가 같은 표를 바꿀 수 있다.
 * 그래서 root_tbl_walk 는 iommu->lock 을, 유닛 순회는 RCU 를 쓴다. 그럼에도
 * 완전히 일관된 스냅숏은 아니며, 이 파일의 출력은 "진단용 근사치"로
 * 읽어야 한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - iommu_regset_show(): 모든 VT-d 레지스터를 이름과 함께 덤프한다.
 * - root_tbl_walk()/ctx_tbl_walk(): 루트→컨텍스트 표를 버스·devfn 순으로 훑는다.
 * - pasid_dir_walk()/pasid_tbl_walk(): 스케일러블 모드의 PASID 두 계층을 훑는다.
 * - pgtable_walk_level(): 페이지 테이블을 재귀로 내려가며 유효한 매핑만 찍는다.
 * - invalidation_queue_show(): 무효화 큐의 서술자와 머리/꼬리 위치를 보여 준다.
 * - ir_translation_struct_show(): 인터럽트 재매핑 표의 유효 항목을 덤프한다.
 * - struct tbl_walk: 표를 걸어 내려가는 동안의 "현재 위치" 한 벌.
 */
#include <linux/debugfs.h>
#include <linux/dmar.h>
#include <linux/pci.h>

#include <asm/irq_remapping.h>

#include "iommu.h"
#include "pasid.h"
#include "perf.h"

/*
 * [한국어] struct tbl_walk — 표를 걸어 내려가는 중의 현재 위치
 *
 * 루트 → 컨텍스트 → PASID 의 세 계층을 재귀로 내려가면서, 마지막에 한 줄을
 * 찍으려면 위 계층에서 지나온 정보가 모두 필요하다. 매 함수에 인자로 줄줄이
 * 넘기는 대신 이 구조체 하나를 seq_file 의 private 에 걸어 두고 공유한다.
 *
 * 그래서 이 구조체는 "지금까지 밟아 온 경로"의 스냅숏이다. print_tbl_walk 가
 * 그것을 한 줄로 펴서 출력한다.
 */
struct tbl_walk {
	u16 bus;
	/* [한국어] 지금 훑고 있는 PCI 버스 번호(= 루트 표의 인덱스).
	 * 설정자: ctx_tbl_walk() 가 각 devfn 을 볼 때 채운다.
	 * 읽는 자: print_tbl_walk() 가 "bb:dd.f" 형태로 찍을 때.
	 * 값 범위: 0~255. 루트 표가 버스 하나당 항목 하나를 갖는다. */
	u16 devfn;
	/* [한국어] 그 버스 안의 device/function 번호(= 컨텍스트 표의 인덱스).
	 * 설정자/읽는 자: 위와 같다.
	 * 값 범위: 0~255. PCI_SLOT/PCI_FUNC 로 쪼개 출력한다. */
	u32 pasid;
	/* [한국어] PASID 값. 스케일러블 모드에서만 의미가 있다.
	 * 설정자: pasid_tbl_walk() 가 (디렉터리 인덱스 << PASID_PDE_SHIFT) + 표 인덱스로 계산한다.
	 * 읽는 자: print_tbl_walk(). 레거시 모드에서는 -1 로 찍어 "없음"을 나타낸다.
	 * 왜 계산해야 하는가: PASID 는 두 계층으로 쪼개 저장되므로, 원래 값은
	 *   두 인덱스를 합쳐야 복원된다. */
	struct root_entry *rt_entry;
	/* [한국어] 현재 버스의 루트 표 항목.
	 * 설정자: ctx_tbl_walk(). 읽는 자: print_tbl_walk() 가 hi/lo 를 그대로 찍는다.
	 * 왜 찍는가: 이 항목의 값이 컨텍스트 표의 물리 주소를 담고 있어,
	 *   덤프를 보는 사람이 하드웨어가 실제로 어디를 가리키는지 확인할 수 있다. */
	struct context_entry *ctx_entry;
	/* [한국어] 현재 devfn 의 컨텍스트 표 항목.
	 * 설정자: ctx_tbl_walk(). 읽는 자: print_tbl_walk().
	 * 레거시 모드에서는 이 항목이 곧바로 페이지 테이블을 가리키고,
	 *   스케일러블 모드에서는 PASID 디렉터리를 가리킨다 — 같은 16바이트가
	 *   RTADDR 의 SMT 비트에 따라 전혀 다르게 해석된다. */
	struct pasid_entry *pasid_tbl_entry;
	/* [한국어] 현재 PASID 표 항목. 스케일러블 모드에서만 채워진다.
	 * 설정자: pasid_tbl_walk(). 읽는 자: print_tbl_walk().
	 * 값 범위: NULL 이면 레거시 모드라는 뜻이고, 출력에서 PASID 관련 열이
	 *   전부 0 과 -1 로 찍힌다. */
};

/*
 * [한국어] struct iommu_regset — 레지스터 하나의 오프셋과 이름
 *
 * 레지스터 덤프를 표 기반으로 만들기 위한 최소 구조다. 이름을 손으로 적지
 * 않고 IOMMU_REGSET_ENTRY 매크로가 매크로 이름 자체를 문자열로 바꿔 주므로,
 * 레지스터를 추가할 때 이름과 오프셋이 어긋날 여지가 없다.
 */
struct iommu_regset {
	int offset;
	/* [한국어] 이 레지스터의 MMIO 오프셋(유닛의 reg 기준).
	 * 설정자: IOMMU_REGSET_ENTRY 매크로가 DMAR_<이름>_REG 로 채운다.
	 * 읽는 자: iommu_regset_show() 가 readl/readq 의 주소를 만들 때. */
	const char *regs;
	/* [한국어] 출력에 쓸 레지스터 이름.
	 * 설정자: 같은 매크로가 __stringify 로 매크로 이름 자체를 문자열로 만든다.
	 * 왜 그렇게 하는가: 이름을 손으로 적으면 오프셋과 어긋날 수 있다.
	 *   매크로 인자 하나에서 둘 다 만들어 내면 그 실수가 원천적으로 불가능하다. */
};

#define DEBUG_BUFFER_SIZE	1024	/* [한국어] 아래 debug_buf 의 크기. 한 번에 만들어 낼 수 있는 문자열의 상한이다 */
static char debug_buf[DEBUG_BUFFER_SIZE];	/* [한국어] 임시 문자열 조립용 전역 버퍼. debugfs 쓰기 경로가 사용자 입력을 받는 데 쓴다 */

#define IOMMU_REGSET_ENTRY(_reg_)					\	/* [한국어] 레지스터 이름 하나로 {오프셋, "이름"} 쌍을 만든다. 이름과 오프셋이 어긋날 수 없게 하는 장치 */
	{ DMAR_##_reg_##_REG, __stringify(_reg_) }	/* [한국어] 매크로 인자 하나에서 오프셋 상수와 이름 문자열을 함께 만든다 — 둘이 어긋날 수 없다 */

static const struct iommu_regset iommu_regs_32[] = {	/* [한국어] 32비트 폭 레지스터 목록. readl 로 읽어야 한다 */
	IOMMU_REGSET_ENTRY(VER),	/* [한국어] 버전. 하드웨어가 구현한 VT-d 스펙 개정 번호 */
	IOMMU_REGSET_ENTRY(GCMD),	/* [한국어] 전역 명령. 쓰기 전용이라 읽어도 현재 설정을 알 수 없다 — 그래서 드라이버가 gcmd 사본을 따로 둔다 */
	IOMMU_REGSET_ENTRY(GSTS),	/* [한국어] 전역 상태. GCMD 로 요청한 것이 실제로 반영됐는지 여기서 확인한다 */
	IOMMU_REGSET_ENTRY(FSTS),	/* [한국어] 폴트 상태. 어떤 종류의 변환 실패가 있었는지 */
	IOMMU_REGSET_ENTRY(FECTL),	/* [한국어] 폴트 이벤트 제어. 폴트를 인터럽트로 알릴지 마스크할지 */
	IOMMU_REGSET_ENTRY(FEDATA),	/* [한국어] 폴트 인터럽트의 MSI 데이터(벡터) */
	IOMMU_REGSET_ENTRY(FEADDR),	/* [한국어] 폴트 인터럽트의 MSI 주소 하위 */
	IOMMU_REGSET_ENTRY(FEUADDR),	/* [한국어] 같은 주소의 상위 32비트 */
	IOMMU_REGSET_ENTRY(PMEN),	/* [한국어] 보호 메모리 영역 활성화. 펌웨어 영역을 DMA 로부터 막는 기능 */
	IOMMU_REGSET_ENTRY(PLMBASE),	/* [한국어] 보호 저메모리 영역의 시작 */
	IOMMU_REGSET_ENTRY(PLMLIMIT),	/* [한국어] 그 영역의 끝 */
	IOMMU_REGSET_ENTRY(ICS),	/* [한국어] 무효화 완료 상태 */
	IOMMU_REGSET_ENTRY(PRS),	/* [한국어] 페이지 요청 상태 — PRQ 에 처리할 요청이 있는지 */
	IOMMU_REGSET_ENTRY(PECTL),	/* [한국어] 페이지 요청 이벤트 제어 */
	IOMMU_REGSET_ENTRY(PEDATA),	/* [한국어] 페이지 요청 인터럽트의 MSI 데이터 */
	IOMMU_REGSET_ENTRY(PEADDR),	/* [한국어] 그 인터럽트의 MSI 주소 하위 */
	IOMMU_REGSET_ENTRY(PEUADDR),	/* [한국어] 같은 주소의 상위 */
};

static const struct iommu_regset iommu_regs_64[] = {	/* [한국어] 64비트 폭 레지스터 목록. readq 로 읽어야 한다 — 폭을 틀리면 하드웨어가 접근을 거부하거나 값이 깨진다 */
	IOMMU_REGSET_ENTRY(CAP),	/* [한국어] 기본 능력. 지원하는 주소 폭, 페이지 크기, 도메인 수 등이 모두 여기 들어 있다 */
	IOMMU_REGSET_ENTRY(ECAP),	/* [한국어] 확장 능력. PASID, 스케일러블 모드, 인터럽트 재매핑 지원 여부 */
	IOMMU_REGSET_ENTRY(RTADDR),	/* [한국어] 루트 표의 물리 주소. SMT 비트가 켜져 있으면 컨텍스트 항목의 해석이 통째로 달라진다 */
	IOMMU_REGSET_ENTRY(PHMBASE),	/* [한국어] 보호 고메모리 영역의 시작 */
	IOMMU_REGSET_ENTRY(PHMLIMIT),	/* [한국어] 그 영역의 끝 */
	IOMMU_REGSET_ENTRY(IQH),	/* [한국어] 무효화 큐의 머리(하드웨어가 처리한 지점) */
	IOMMU_REGSET_ENTRY(IQT),	/* [한국어] 무효화 큐의 꼬리(드라이버가 넣은 지점). 둘의 차이가 밀린 명령 수다 */
	IOMMU_REGSET_ENTRY(IQA),	/* [한국어] 무효화 큐 자체의 물리 주소 */
	IOMMU_REGSET_ENTRY(IRTA),	/* [한국어] 인터럽트 재매핑 표의 주소. EIM 비트와 크기 필드가 하위에 함께 들어 있다 */
	IOMMU_REGSET_ENTRY(PQH),	/* [한국어] 페이지 요청 큐의 머리 */
	IOMMU_REGSET_ENTRY(PQT),	/* [한국어] 페이지 요청 큐의 꼬리 */
	IOMMU_REGSET_ENTRY(PQA),	/* [한국어] 페이지 요청 큐의 물리 주소 */
	IOMMU_REGSET_ENTRY(MTRRCAP),	/* [한국어] 하드웨어가 흉내 내는 MTRR 능력 */
	IOMMU_REGSET_ENTRY(MTRRDEF),	/* [한국어] MTRR 기본 메모리 타입 */
	IOMMU_REGSET_ENTRY(MTRR_FIX64K_00000),	/* [한국어] 고정 범위 MTRR — 0~512KB 구간의 메모리 타입 */
	IOMMU_REGSET_ENTRY(MTRR_FIX16K_80000),	/* [한국어] 512KB~640KB 구간 */
	IOMMU_REGSET_ENTRY(MTRR_FIX16K_A0000),	/* [한국어] 640KB~768KB 구간 */
	IOMMU_REGSET_ENTRY(MTRR_FIX4K_C0000),	/* [한국어] 768KB부터 4KB 단위로 쪼갠 첫 구간 */
	IOMMU_REGSET_ENTRY(MTRR_FIX4K_C8000),	/* [한국어] 그다음 4KB 구간 */
	IOMMU_REGSET_ENTRY(MTRR_FIX4K_D0000),	/* [한국어] 그다음 */
	IOMMU_REGSET_ENTRY(MTRR_FIX4K_D8000),	/* [한국어] 그다음 */
	IOMMU_REGSET_ENTRY(MTRR_FIX4K_E0000),	/* [한국어] 그다음 */
	IOMMU_REGSET_ENTRY(MTRR_FIX4K_E8000),	/* [한국어] 그다음 */
	IOMMU_REGSET_ENTRY(MTRR_FIX4K_F0000),	/* [한국어] 그다음 */
	IOMMU_REGSET_ENTRY(MTRR_FIX4K_F8000),	/* [한국어] 1MB 직전까지의 마지막 고정 범위 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSBASE0),	/* [한국어] 가변 범위 MTRR 0번의 시작 주소와 메모리 타입 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSMASK0),	/* [한국어] 그 0번 범위의 크기를 정하는 마스크와 유효 비트 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSBASE1),	/* [한국어] 가변 범위 MTRR 1번의 시작 주소와 메모리 타입 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSMASK1),	/* [한국어] 그 1번 범위의 크기를 정하는 마스크와 유효 비트 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSBASE2),	/* [한국어] 가변 범위 MTRR 2번의 시작 주소와 메모리 타입 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSMASK2),	/* [한국어] 그 2번 범위의 크기를 정하는 마스크와 유효 비트 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSBASE3),	/* [한국어] 가변 범위 MTRR 3번의 시작 주소와 메모리 타입 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSMASK3),	/* [한국어] 그 3번 범위의 크기를 정하는 마스크와 유효 비트 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSBASE4),	/* [한국어] 가변 범위 MTRR 4번의 시작 주소와 메모리 타입 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSMASK4),	/* [한국어] 그 4번 범위의 크기를 정하는 마스크와 유효 비트 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSBASE5),	/* [한국어] 가변 범위 MTRR 5번의 시작 주소와 메모리 타입 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSMASK5),	/* [한국어] 그 5번 범위의 크기를 정하는 마스크와 유효 비트 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSBASE6),	/* [한국어] 가변 범위 MTRR 6번의 시작 주소와 메모리 타입 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSMASK6),	/* [한국어] 그 6번 범위의 크기를 정하는 마스크와 유효 비트 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSBASE7),	/* [한국어] 가변 범위 MTRR 7번의 시작 주소와 메모리 타입 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSMASK7),	/* [한국어] 그 7번 범위의 크기를 정하는 마스크와 유효 비트 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSBASE8),	/* [한국어] 가변 범위 MTRR 8번의 시작 주소와 메모리 타입 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSMASK8),	/* [한국어] 그 8번 범위의 크기를 정하는 마스크와 유효 비트 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSBASE9),	/* [한국어] 가변 범위 MTRR 9번의 시작 주소와 메모리 타입 */
	IOMMU_REGSET_ENTRY(MTRR_PHYSMASK9),	/* [한국어] 그 9번 범위의 크기를 정하는 마스크와 유효 비트 */
};

static struct dentry *intel_iommu_debug;	/* [한국어] 이 드라이버의 debugfs 디렉터리. 모든 파일이 그 아래 만들어진다 */

/*
 * [한국어]
 * iommu_regset_show - 모든 유닛의 VT-d 레지스터를 이름과 함께 덤프한다
 *
 * @m: 출력이 쌓일 seq_file.
 * @unused: 쓰지 않는다.
 * @return: 0 성공, -EINVAL 이면 어떤 유닛의 레지스터 주소가 유효하지 않다.
 *
 * IOMMU 문제를 진단할 때 가장 먼저 보는 파일이다. 커널 자료구조가 아니라
 * 하드웨어가 실제로 들고 있는 값을 보여 주므로, "드라이버는 켰다고 생각하는데
 * 하드웨어는 안 켜져 있다" 같은 어긋남을 잡아낼 수 있다.
 *
 * 32비트와 64비트 목록을 나눠 둔 이유: VT-d 레지스터는 폭이 정해져 있고,
 * 잘못된 폭으로 접근하면 값이 깨지거나 하드웨어가 접근을 거부한다. 그래서
 * readl 로 읽을 것과 readq 로 읽을 것을 표 단계에서 갈라 둔다.
 *
 * register_lock 을 잡는 이유: 레지스터를 읽는 동안 다른 경로가 GCMD 를
 * 쓰면 GSTS 가 중간 상태로 보인다. 진단 출력이 존재하지 않는 상태를
 * 보고하지 않도록 막는다.
 *
 * RCU 로 유닛 목록을 도는 이유: 유닛 핫플러그와 경쟁할 수 있는데, 읽기만
 * 하는 경로라 RCU 로 충분하다.
 *
 * 실행 컨텍스트: 사용자가 파일을 읽을 때의 프로세스 문맥.
 *
 * 호출 체인:
 *   read(2) → seq_file → [이 함수] → readl()/readq()
 */
static int iommu_regset_show(struct seq_file *m, void *unused)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회용 */
	struct intel_iommu *iommu;	/* [한국어] 현재 유닛 */
	unsigned long flag;	/* [한국어] 인터럽트 상태 저장용 */
	int i, ret = 0;	/* [한국어] 레지스터 순회 인덱스와 결과 */
	u64 value;	/* [한국어] 읽어 온 레지스터 값 */

	rcu_read_lock();	/* [한국어] 유닛 목록을 도는 동안 핫플러그로부터 보호. 읽기만 하므로 RCU 로 충분하다 */
	for_each_active_iommu(iommu, drhd) {	/* [한국어] 동작 중인 모든 유닛을 훑는다 */
		if (!drhd->reg_base_addr) {	/* [한국어] 레지스터 주소가 없다 — 표가 이상하다는 뜻 */
			seq_puts(m, "IOMMU: Invalid base address\n");	/* [한국어] 사용자에게 그 사실을 알리고 */
			ret = -EINVAL;	/* [한국어] 오류로 표시 */
			goto out;	/* [한국어] RCU 를 풀고 나간다 */
		}

		seq_printf(m, "IOMMU: %s Register Base Address: %llx\n",	/* [한국어] 어느 유닛의 덤프인지 머리말 */
			   iommu->name, drhd->reg_base_addr);
		seq_puts(m, "Name\t\t\tOffset\t\tContents\n");	/* [한국어] 열 제목 */
		/*
		 * Publish the contents of the 64-bit hardware registers
		 * by adding the offset to the pointer (virtual address).
		 */
		raw_spin_lock_irqsave(&iommu->register_lock, flag);	/* [한국어] 읽는 동안 다른 경로의 레지스터 쓰기와 섞이지 않게 */
		for (i = 0 ; i < ARRAY_SIZE(iommu_regs_32); i++) {	/* [한국어] 32비트 레지스터 목록 */
			value = readl(iommu->reg + iommu_regs_32[i].offset);	/* [한국어] 폭에 맞게 readl 로 읽는다 */
			seq_printf(m, "%-16s\t0x%02x\t\t0x%016llx\n",	/* [한국어] 이름·오프셋·값을 한 줄로 */
				   iommu_regs_32[i].regs, iommu_regs_32[i].offset,
				   value);
		}
		for (i = 0 ; i < ARRAY_SIZE(iommu_regs_64); i++) {	/* [한국어] 64비트 레지스터 목록 */
			value = readq(iommu->reg + iommu_regs_64[i].offset);	/* [한국어] 이쪽은 readq. 폭을 틀리면 값이 깨진다 */
			seq_printf(m, "%-16s\t0x%02x\t\t0x%016llx\n",	/* [한국어] 이름·오프셋·값을 한 줄로 */
				   iommu_regs_64[i].regs, iommu_regs_64[i].offset,
				   value);
		}
		raw_spin_unlock_irqrestore(&iommu->register_lock, flag);	/* [한국어] 이 유닛의 덤프 끝 */
		seq_putc(m, '\n');	/* [한국어] 유닛 사이를 빈 줄로 구분 */
	}
out:	/* [한국어] 유닛의 레지스터 주소가 잘못된 경우도 여기로 모여 RCU 를 푼다 */
	rcu_read_unlock();	/* [한국어] 유닛 목록 순회 종료 */

	return ret;	/* [한국어] 성공이면 0 */
}
DEFINE_SHOW_ATTRIBUTE(iommu_regset);	/* [한국어] 위 show 함수로 seq_file 용 file_operations 를 자동 생성한다 */

/*
 * [한국어]
 * print_tbl_walk - 지금까지 밟아 온 표 경로를 한 줄로 찍는다
 *
 * @m: 출력 대상. private 에 struct tbl_walk 가 걸려 있다.
 *
 * 세 계층(루트/컨텍스트/PASID)의 원시 값을 그대로 16진수로 낸다. 해석해서
 * 예쁘게 보여 주지 않는 것이 의도다 — 드라이버의 해석이 틀렸을 가능성을
 * 의심하는 상황에서 쓰는 도구이므로, 하드웨어가 보는 비트를 날것으로
 * 보여야 한다.
 *
 * PASID 항목이 없을 때 -1 과 0 들을 찍는 이유: 레거시 모드에는 PASID 라는
 * 개념 자체가 없다. 열을 비우는 대신 "유효하지 않음"을 뜻하는 -1 을 박아
 * 두어, 출력의 열 개수가 모드와 상관없이 일정하게 유지된다 — 스크립트로
 * 파싱하기 쉬워진다.
 *
 * 호출 체인:
 *   ctx_tbl_walk()/pasid_tbl_walk() → [이 함수]
 */
static inline void print_tbl_walk(struct seq_file *m)
{
	struct tbl_walk *tbl_wlk = m->private;	/* [한국어] 지금까지 밟아 온 경로가 여기 담겨 있다 */

	seq_printf(m, "%02x:%02x.%x\t0x%016llx:0x%016llx\t0x%016llx:0x%016llx\t",	/* [한국어] BDF 와 루트·컨텍스트 항목의 원시 값. 해석하지 않고 날것으로 낸다 */
		   tbl_wlk->bus, PCI_SLOT(tbl_wlk->devfn),
		   PCI_FUNC(tbl_wlk->devfn), tbl_wlk->rt_entry->hi,
		   tbl_wlk->rt_entry->lo, tbl_wlk->ctx_entry->hi,
		   tbl_wlk->ctx_entry->lo);

	/*
	 * A legacy mode DMAR doesn't support PASID, hence default it to -1
	 * indicating that it's invalid. Also, default all PASID related fields
	 * to 0.
	 */
	if (!tbl_wlk->pasid_tbl_entry)	/* [한국어] 레거시 모드 — PASID 라는 개념 자체가 없다 */
		seq_printf(m, "%-6d\t0x%016llx:0x%016llx:0x%016llx\n", -1,	/* [한국어] -1 과 0 으로 채워 열 개수를 모드와 무관하게 유지한다 */
			   (u64)0, (u64)0, (u64)0);
	else
		seq_printf(m, "%-6d\t0x%016llx:0x%016llx:0x%016llx\n",	/* [한국어] 스케일러블 모드면 복원한 PASID 와 항목 세 워드를 */
			   tbl_wlk->pasid, tbl_wlk->pasid_tbl_entry->val[2],
			   tbl_wlk->pasid_tbl_entry->val[1],
			   tbl_wlk->pasid_tbl_entry->val[0]);
}

/*
 * [한국어]
 * pasid_tbl_walk - PASID 표 하나의 유효 항목을 모두 찍는다
 *
 * @m: 출력 대상.
 * @tbl_entry: PASID 표의 첫 항목.
 * @dir_idx: 이 표를 가리킨 디렉터리 항목의 인덱스.
 *
 * PASID 는 20비트라 표 하나로 담을 수 없어 디렉터리 + 표의 두 계층으로
 * 쪼개져 있다. 그래서 원래의 PASID 값을 복원하려면 두 인덱스를 다시
 * 합쳐야 한다: (dir_idx << PASID_PDE_SHIFT) + tbl_idx.
 *
 * present 인 항목만 찍는 이유: 표는 항상 꽉 찬 크기로 할당되므로, 전부
 * 찍으면 의미 없는 0 이 수천 줄 쏟아진다.
 *
 * 호출 체인:
 *   pasid_dir_walk() → [이 함수] → print_tbl_walk()
 */
static void pasid_tbl_walk(struct seq_file *m, struct pasid_entry *tbl_entry,
			   u16 dir_idx)
{
	struct tbl_walk *tbl_wlk = m->private;	/* [한국어] 공유 중인 경로 상태 */
	u8 tbl_idx;	/* [한국어] 표 안의 인덱스 */

	for (tbl_idx = 0; tbl_idx < PASID_TBL_ENTRIES; tbl_idx++) {	/* [한국어] 표의 모든 항목을 훑는다 */
		if (pasid_pte_is_present(tbl_entry)) {	/* [한국어] 쓰이고 있는 항목만 */
			tbl_wlk->pasid_tbl_entry = tbl_entry;	/* [한국어] 현재 항목을 경로에 기록 */
			tbl_wlk->pasid = (dir_idx << PASID_PDE_SHIFT) + tbl_idx;	/* [한국어] 두 계층의 인덱스를 합쳐 원래 PASID 값을 복원한다 */
			print_tbl_walk(m);	/* [한국어] 한 줄 출력 */
		}

		tbl_entry++;	/* [한국어] 다음 항목으로 */
	}
}

/*
 * [한국어]
 * pasid_dir_walk - PASID 디렉터리를 훑어 각 PASID 표로 내려간다
 *
 * @m: 출력 대상.
 * @pasid_dir_ptr: 디렉터리의 물리 주소(컨텍스트 항목에서 꺼낸 값).
 * @pasid_dir_size: 디렉터리 항목 수. 컨텍스트 항목이 알려 준다.
 *
 * 물리 주소를 phys_to_virt 로 되돌려 직접 읽는다 — 이 표는 하드웨어를 위한
 * 자료구조라 커널이 따로 포인터를 들고 있지 않고, 하드웨어가 보는 것과
 * 똑같은 경로로 도달해야 진단 가치가 있다.
 *
 * 디렉터리 항목이 비어 있으면(get_pasid_table_from_pde 가 NULL) 그 구간의
 * PASID 는 아직 쓰인 적이 없다는 뜻이라 건너뛴다. PASID 표는 필요할 때
 * 게으르게 할당되기 때문이다.
 *
 * 호출 체인:
 *   ctx_tbl_walk() (스케일러블 모드) → [이 함수] → pasid_tbl_walk()
 */
static void pasid_dir_walk(struct seq_file *m, u64 pasid_dir_ptr,
			   u16 pasid_dir_size)
{
	struct pasid_dir_entry *dir_entry = phys_to_virt(pasid_dir_ptr);	/* [한국어] 하드웨어가 보는 물리 주소를 커널 주소로 되돌린다 */
	struct pasid_entry *pasid_tbl;	/* [한국어] 디렉터리 항목이 가리키는 PASID 표 */
	u16 dir_idx;	/* [한국어] 디렉터리 인덱스 */

	for (dir_idx = 0; dir_idx < pasid_dir_size; dir_idx++) {	/* [한국어] 컨텍스트 항목이 알려 준 크기만큼 */
		pasid_tbl = get_pasid_table_from_pde(dir_entry);	/* [한국어] present 면 표 주소, 아니면 NULL */
		if (pasid_tbl)	/* [한국어] 그 구간의 PASID 가 쓰인 적이 있다면 */
			pasid_tbl_walk(m, pasid_tbl, dir_idx);	/* [한국어] 그 표로 내려간다. 없으면 게으른 할당이 아직 안 된 것 */

		dir_entry++;	/* [한국어] 다음 디렉터리 항목 */
	}
}

/*
 * [한국어]
 * ctx_tbl_walk - 버스 하나의 컨텍스트 표를 devfn 순으로 훑는다
 *
 * @m: 출력 대상.
 * @iommu: 이 표를 가진 유닛.
 * @bus: 훑을 버스 번호.
 *
 * VT-d 변환 사슬의 두 번째 단계다. 여기서 갈림길이 나온다 — RTADDR 의
 * SMT 비트가 켜져 있으면 컨텍스트 항목이 페이지 테이블이 아니라 PASID
 * 디렉터리를 가리킨다. 같은 16바이트가 모드에 따라 전혀 다르게 읽히므로,
 * 덤프도 그 비트를 직접 읽어 갈라야 한다.
 *
 * 원 주석이 설명하는 "두 개의 컨텍스트 표": 스케일러블 모드에서는 컨텍스트
 * 표가 상·하위 둘로 나뉘고 각각 128개 항목을 갖는다. 항목이 16바이트라
 * 4KB 페이지에 128개밖에 안 들어가기 때문이다. iommu_context_addr() 이
 * devfn 을 보고 알아서 맞는 표를 고르므로 이 함수는 그 차이를 몰라도 된다.
 *
 * context 가 NULL 이면 return 하고 present 가 아니면 continue 하는 차이:
 * NULL 은 이 버스의 루트 항목 자체가 없다는 뜻이라 나머지 devfn 도 볼
 * 필요가 없고, present 아님은 그 devfn 만 안 쓰인다는 뜻이다.
 *
 * 호출 체인:
 *   root_tbl_walk() → [이 함수] → pasid_dir_walk()/print_tbl_walk()
 */
static void ctx_tbl_walk(struct seq_file *m, struct intel_iommu *iommu, u16 bus)
{
	struct context_entry *context;	/* [한국어] 현재 devfn 의 컨텍스트 항목 */
	u16 devfn, pasid_dir_size;	/* [한국어] 순회 인덱스와 PASID 디렉터리 크기 */
	u64 pasid_dir_ptr;	/* [한국어] PASID 디렉터리의 물리 주소 */

	for (devfn = 0; devfn < 256; devfn++) {	/* [한국어] 한 버스의 모든 device/function */
		struct tbl_walk tbl_wlk = {0};	/* [한국어] 이 devfn 의 경로 상태를 새로 만든다 */

		/*
		 * Scalable mode root entry points to upper scalable mode
		 * context table and lower scalable mode context table. Each
		 * scalable mode context table has 128 context entries where as
		 * legacy mode context table has 256 context entries. So in
		 * scalable mode, the context entries for former 128 devices are
		 * in the lower scalable mode context table, while the latter
		 * 128 devices are in the upper scalable mode context table.
		 * In scalable mode, when devfn > 127, iommu_context_addr()
		 * automatically refers to upper scalable mode context table and
		 * hence the caller doesn't have to worry about differences
		 * between scalable mode and non scalable mode.
		 */
		context = iommu_context_addr(iommu, bus, devfn, 0);	/* [한국어] 컨텍스트 항목 주소. 스케일러블 모드의 상·하위 표 구분도 이 함수가 처리한다 */
		if (!context)	/* [한국어] 이 버스의 루트 항목 자체가 없다 */
			return;	/* [한국어] 나머지 devfn 도 볼 필요가 없다 */

		if (!context_present(context))	/* [한국어] 이 devfn 만 쓰이지 않는다 */
			continue;	/* [한국어] 다음 devfn 으로 */

		tbl_wlk.bus = bus;	/* [한국어] 경로에 버스 기록 */
		tbl_wlk.devfn = devfn;	/* [한국어] devfn 기록 */
		tbl_wlk.rt_entry = &iommu->root_entry[bus];	/* [한국어] 이 버스의 루트 항목 */
		tbl_wlk.ctx_entry = context;	/* [한국어] 컨텍스트 항목 */
		m->private = &tbl_wlk;	/* [한국어] 아래 계층이 이 경로를 이어받는다 */

		if (readq(iommu->reg + DMAR_RTADDR_REG) & DMA_RTADDR_SMT) {	/* [한국어] SMT 비트 — 컨텍스트 항목의 해석을 통째로 바꾸는 스위치 */
			pasid_dir_ptr = context->lo & VTD_PAGE_MASK;	/* [한국어] 스케일러블 모드에서는 lo 가 PASID 디렉터리를 가리킨다 */
			pasid_dir_size = get_pasid_dir_size(context);	/* [한국어] 디렉터리 크기도 같은 항목에 들어 있다 */
			pasid_dir_walk(m, pasid_dir_ptr, pasid_dir_size);	/* [한국어] PASID 두 계층으로 내려간다 */
			continue;	/* [한국어] 그쪽에서 출력했으므로 여기선 찍지 않는다 */
		}

		print_tbl_walk(m);	/* [한국어] 레거시 모드면 여기가 끝이라 바로 한 줄 출력 */
	}
}

/*
 * [한국어]
 * root_tbl_walk - 유닛 하나의 변환 표 전체를 훑는다
 *
 * @m: 출력 대상.
 * @iommu: 훑을 유닛.
 *
 * 변환 사슬의 출발점이다. 루트 표는 버스 번호로 인덱싱되는 256개 항목이라,
 * 그냥 0부터 255까지 전부 돈다.
 *
 * 원 주석이 밝히듯 루트 항목의 present 를 따로 검사하지 않는다 —
 * iommu_context_addr() 이 그 검사를 먼저 하고 없으면 NULL 을 주기 때문이다.
 * 검사를 중복하면 두 곳의 판단이 어긋날 여지만 생긴다.
 *
 * iommu->lock 을 잡는 이유: 표를 걸어 내려가는 동안 다른 CPU 가 장치를
 * 붙이거나 떼면서 컨텍스트 항목을 바꿀 수 있다. 완전한 스냅숏은 아니지만,
 * 적어도 반쯤 갱신된 항목을 따라가다 엉뚱한 물리 주소를 읽는 일은 막는다.
 *
 * 호출 체인:
 *   dmar_translation_struct_show() → [이 함수] → ctx_tbl_walk()
 */
static void root_tbl_walk(struct seq_file *m, struct intel_iommu *iommu)
{
	u16 bus;	/* [한국어] 버스 순회 인덱스 */

	spin_lock(&iommu->lock);	/* [한국어] 장치 붙이기/떼기와 경쟁하지 않게 표를 잠근다 */
	seq_printf(m, "IOMMU %s: Root Table Address: 0x%llx\n", iommu->name,	/* [한국어] 루트 표의 물리 주소 — 하드웨어가 실제로 보는 값 */
		   (u64)virt_to_phys(iommu->root_entry));
	seq_puts(m, "B.D.F\tRoot_entry\t\t\t\tContext_entry\t\t\t\tPASID\tPASID_table_entry\n");	/* [한국어] 열 제목 */

	/*
	 * No need to check if the root entry is present or not because
	 * iommu_context_addr() performs the same check before returning
	 * context entry.
	 */
	for (bus = 0; bus < 256; bus++)	/* [한국어] 루트 표는 버스 번호로 인덱싱되는 256개 항목 */
		ctx_tbl_walk(m, iommu, bus);	/* [한국어] 각 버스의 컨텍스트 표로 내려간다 */
	spin_unlock(&iommu->lock);	/* [한국어] 순회 끝 */
}

/*
 * [한국어]
 * dmar_translation_struct_show - 모든 유닛의 DMA 변환 표를 덤프한다 (debugfs show)
 *
 * @m: 출력 대상.
 * @unused: 쓰지 않는다.
 * @return: 항상 0.
 *
 * GSTS 의 TES(Translation Enable Status)를 먼저 확인하는 것이 핵심이다.
 * 변환이 꺼져 있으면 표의 내용은 아무 의미가 없다 — 하드웨어가 그것을
 * 참조하지 않기 때문이다. 그래서 표를 찍는 대신 "이 유닛은 꺼져 있다"고
 * 알린다. 이 한 줄이 "표는 멀쩡한데 왜 DMA 가 안 막히지?"의 답인 경우가 많다.
 *
 * 호출 체인:
 *   read(2) → [이 함수] → root_tbl_walk()
 */
static int dmar_translation_struct_show(struct seq_file *m, void *unused)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회용 */
	struct intel_iommu *iommu;	/* [한국어] 현재 유닛 */
	u32 sts;	/* [한국어] 전역 상태 레지스터 */

	rcu_read_lock();	/* [한국어] 유닛 목록 보호 */
	for_each_active_iommu(iommu, drhd) {	/* [한국어] 모든 유닛에 대해 */
		sts = readl(iommu->reg + DMAR_GSTS_REG);	/* [한국어] 하드웨어의 실제 상태를 읽는다 */
		if (!(sts & DMA_GSTS_TES)) {	/* [한국어] 변환이 꺼져 있으면 표의 내용은 아무 의미가 없다 */
			seq_printf(m, "DMA Remapping is not enabled on %s\n",	/* [한국어] 그 사실만 알린다 — "표는 멀쩡한데 왜 안 막히지"의 답인 경우가 많다 */
				   iommu->name);
			continue;	/* [한국어] 다음 유닛으로 */
		}
		root_tbl_walk(m, iommu);	/* [한국어] 켜져 있으면 표 전체를 훑는다 */
		seq_putc(m, '\n');	/* [한국어] 유닛 사이 구분 */
	}
	rcu_read_unlock();	/* [한국어] 순회 끝 */

	return 0;	/* [한국어] 실패할 일이 없다 */
}
DEFINE_SHOW_ATTRIBUTE(dmar_translation_struct);	/* [한국어] 같은 방식으로 읽기 전용 파일의 동작을 만든다 */

/*
 * [한국어]
 * level_to_directory_size - 그 단계의 항목 하나가 덮는 주소 범위
 *
 * @level: 페이지 테이블 단계(1이 최하위).
 * @return: 이 단계 항목 하나가 담당하는 바이트 수.
 *
 * 페이지 테이블은 단계마다 9비트씩(VTD_STRIDE_SHIFT) 주소를 쪼갠다. 그래서
 * 1단계 항목은 4KB, 2단계는 2MB, 3단계는 1GB … 로 커진다. 표를 재귀로
 * 내려가면서 "지금 이 항목이 어느 IOVA 에서 시작하는가"를 누적하는 데 쓴다.
 *
 * 호출 체인:
 *   pgtable_walk_level() → [이 함수]
 */
static inline unsigned long level_to_directory_size(int level)
{
	return BIT_ULL(VTD_PAGE_SHIFT + VTD_STRIDE_SHIFT * (level - 1));	/* [한국어] 4KB에서 시작해 단계마다 9비트씩 커진다: 1단계 4KB, 2단계 2MB, 3단계 1GB */
}

/*
 * [한국어]
 * dump_page_info - 잎에 도달했을 때 그 IOVA 와 지나온 경로를 한 줄로 찍는다
 *
 * @m: 출력 대상.
 * @iova: 이 매핑이 시작하는 IOVA.
 * @path: 각 단계에서 지나온 PTE 값들. path[5]가 최상위다.
 *
 * 경로 전체를 함께 찍는 것이 이 함수의 요점이다. 최종 물리 주소만 보면
 * "왜 그 주소가 나왔는지" 알 수 없지만, 각 단계의 PTE 를 함께 보면 어느
 * 단계에서 잘못된 값이 들어갔는지 짚을 수 있다.
 *
 * path[2]/path[1]을 조건부로 찍는 이유: 슈퍼페이지로 끝난 매핑은 아래
 * 단계를 거치지 않아 그 자리에 값이 없다. 0 을 찍으면 "0인 PTE"와
 * 구별되지 않으므로 아예 열을 생략한다.
 *
 * 호출 체인:
 *   pgtable_walk_level() → [이 함수]
 */
static inline void
dump_page_info(struct seq_file *m, unsigned long iova, u64 *path)
{
	seq_printf(m, "0x%013lx |\t0x%016llx\t0x%016llx\t0x%016llx",	/* [한국어] IOVA 의 페이지 번호와 상위 세 단계의 PTE */
		   iova >> VTD_PAGE_SHIFT, path[5], path[4], path[3]);
	if (path[2]) {	/* [한국어] 슈퍼페이지로 끝나지 않고 더 내려갔다면 */
		seq_printf(m, "\t0x%016llx", path[2]);	/* [한국어] 그 단계도 찍는다 */
		if (path[1])	/* [한국어] 1단계까지 내려갔다면 */
			seq_printf(m, "\t0x%016llx", path[1]);	/* [한국어] 마지막 PTE 도. 0 을 찍지 않고 열을 생략해 "0인 PTE"와 구별한다 */
	}
	seq_putc(m, '\n');	/* [한국어] 줄 끝 */
}

/*
 * [한국어]
 * pgtable_walk_level - 페이지 테이블을 재귀로 내려가며 유효한 매핑을 찍는다
 *
 * @m: 출력 대상.
 * @pde: 이 단계 표의 첫 항목.
 * @level: 현재 단계(1이 최하위).
 * @start: 이 표가 담당하는 IOVA 범위의 시작.
 * @path: 지나온 PTE 를 쌓아 둘 배열.
 *
 * 하드웨어가 주소를 변환할 때 밟는 경로를 그대로 재현한다. 다른 점은
 * 하나의 IOVA 만 따라가는 것이 아니라 모든 유효 항목을 훑는다는 것뿐이다.
 *
 * 재귀를 멈추는 두 조건:
 *  - 슈퍼페이지 비트가 켜져 있으면 이 항목이 곧 잎이다. 2MB/1GB 페이지가
 *    그렇게 표현된다.
 *  - level 이 1이면 더 내려갈 곳이 없다.
 *
 * level 범위 검사가 맨 앞에 있는 이유: 재귀 깊이를 제한하는 안전장치다.
 * 표가 손상돼 있으면 dma_pte_addr 이 엉뚱한 주소를 주고 무한히 내려갈 수
 * 있는데, 진단 도구가 커널을 무너뜨려서는 안 된다.
 *
 * start 를 누적하는 방식: 항목을 하나 넘길 때마다 그 단계의 담당 범위만큼
 * 더한다. 그래서 잎에 닿았을 때의 start 가 곧 그 매핑의 IOVA 다.
 *
 * 호출 체인:
 *   show_device_domain_translation() → [이 함수] (재귀) → dump_page_info()
 */
static void pgtable_walk_level(struct seq_file *m, struct dma_pte *pde,
			       int level, unsigned long start,
			       u64 *path)
{
	int i;	/* [한국어] 항목 순회 인덱스 */

	if (level > 5 || level < 1)	/* [한국어] 손상된 표를 따라 무한히 내려가는 것을 막는 안전장치 */
		return;	/* [한국어] 진단 도구가 커널을 무너뜨려서는 안 된다 */

	for (i = 0; i < BIT_ULL(VTD_STRIDE_SHIFT);	/* [한국어] 한 표에는 2^9 = 512개 항목이 있다 */
			i++, pde++, start += level_to_directory_size(level)) {	/* [한국어] 항목을 넘길 때마다 담당 IOVA 범위만큼 시작 주소를 밀어 준다 */
		if (!dma_pte_present(pde))	/* [한국어] 매핑되지 않은 항목은 */
			continue;	/* [한국어] 건너뛴다. 전부 찍으면 0 이 수천 줄 쏟아진다 */

		path[level] = pde->val;	/* [한국어] 이 단계에서 지나온 PTE 를 경로에 쌓는다 */
		if (dma_pte_superpage(pde) || level == 1)	/* [한국어] 슈퍼페이지이거나 최하위 단계면 여기가 잎이다 */
			dump_page_info(m, start, path);	/* [한국어] 누적된 start 가 곧 이 매핑의 IOVA 다 */
		else
			pgtable_walk_level(m, phys_to_virt(dma_pte_addr(pde)),	/* [한국어] 아니면 다음 단계 표로 내려간다 */
					   level - 1, start, path);	/* [한국어] 단계를 하나 낮추고, 지금까지의 시작 주소를 그대로 물려준다 */
		path[level] = 0;	/* [한국어] 이 단계를 빠져나가며 경로에서 지운다. 남겨 두면 형제 서브트리의 출력에 섞여 든다 */
	}
}

/*
 * [한국어]
 * domain_translation_struct_show - 장치 하나의 페이지 테이블을 처음부터 끝까지 따라간다
 *
 * @m: 출력 대상.
 * @info: 대상 장치의 IOMMU 쪽 정보(세그먼트/버스/devfn).
 * @pasid: 어느 PASID 의 매핑을 볼지. 일반 DMA 는 IOMMU_NO_PASID.
 * @return: 항상 0.
 *
 * 이 파일에서 가장 긴 함수이자, VT-d 변환 사슬 전체를 한 함수에서 볼 수
 * 있는 곳이다. 하는 일은 "이 장치의 페이지 테이블 루트(pgd)를 찾아
 * pgtable_walk_level 에 넘기는 것"인데, 그 루트를 찾는 경로가 모드와
 * 변환 타입에 따라 네 갈래로 갈린다.
 *
 *  1) 스케일러블 + FL_ONLY: 1단계(프로세스 페이지 테이블) 변환. 페이지
 *     테이블 주소가 PASID 항목의 val[2]에 있다.
 *  2) 스케일러블 + SL_ONLY/NESTED: 2단계 변환. 주소가 val[0]에 있다.
 *  3) 레거시 + MULTI_LEVEL/DEV_IOTLB: 컨텍스트 항목의 lo 가 곧 페이지
 *     테이블 주소(SSPTPTR)다.
 *  4) 그 밖(패스스루 등): 페이지 테이블 자체가 없으므로 아무것도 찍지 않는다.
 *
 * 같은 비트가 어디에 있는지가 이렇게 다른 이유는 두 형식이 서로 다른
 * 시기에 정의되었고, 스케일러블 모드는 1·2단계를 함께 담아야 해서 자리를
 * 새로 배치했기 때문이다.
 *
 * agaw + 2 를 단계 수로 넘기는 이유: AGAW(Adjusted Guest Address Width)는
 * 주소 폭을 단계 수로 인코딩한 값이라, 여기에 2를 더하면 실제 페이지
 * 테이블 깊이가 된다.
 *
 * 원 주석이 인정하는 한계: iommu->lock 은 장치의 도메인이 바뀌는 것을
 * 막지만, 순회 도중 iommu_unmap() 이 페이지 테이블 페이지를 해제하는 것과는
 * 경쟁한다. 완전한 해결에는 그 페이지들의 RCU 해제가 필요하다 — 진단
 * 도구를 위해 핫 경로에 비용을 얹을지의 문제라, 지금은 알려진 위험으로
 * 남겨져 있다.
 *
 * found 로 유닛 순회를 멈추는 이유: 한 장치는 한 유닛에만 속하므로,
 * 찾았으면 나머지 유닛을 볼 필요가 없다.
 *
 * 호출 체인:
 *   dev_domain_translation_struct_show()/pasid_domain_... → [이 함수]
 *     → iommu_context_addr() → pgtable_walk_level()
 */
static int domain_translation_struct_show(struct seq_file *m,
					  struct device_domain_info *info,
					  ioasid_t pasid)
{
	bool scalable, found = false;	/* [한국어] 스케일러블 모드 여부와, 담당 유닛을 찾았는지 */
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회용 */
	struct intel_iommu *iommu;	/* [한국어] 현재 유닛 */
	u16 devfn, bus, seg;	/* [한국어] 대상 장치의 위치 */

	bus = info->bus;	/* [한국어] 버스 번호 */
	devfn = info->devfn;	/* [한국어] device/function */
	seg = info->segment;	/* [한국어] PCI 세그먼트 — 큰 시스템은 버스 번호 공간이 여러 개다 */

	rcu_read_lock();	/* [한국어] 유닛 목록 보호 */
	for_each_active_iommu(iommu, drhd) {	/* [한국어] 담당 유닛을 찾을 때까지 */
		struct context_entry *context;	/* [한국어] 이 장치의 컨텍스트 항목 */
		u64 pgd, path[6] = { 0 };	/* [한국어] 페이지 테이블 루트와, 순회 중 쌓을 경로 */
		u32 sts, agaw;	/* [한국어] 전역 상태와 주소 폭(= 표 깊이) */

		if (seg != iommu->segment)	/* [한국어] 세그먼트가 다르면 이 유닛의 관할이 아니다 */
			continue;	/* [한국어] 다음 유닛 */

		sts = readl(iommu->reg + DMAR_GSTS_REG);	/* [한국어] 하드웨어의 실제 상태 */
		if (!(sts & DMA_GSTS_TES)) {	/* [한국어] 변환이 꺼져 있으면 표가 무의미하다 */
			seq_printf(m, "DMA Remapping is not enabled on %s\n",	/* [한국어] 그 사실만 알리고 */
				   iommu->name);
			continue;	/* [한국어] 다음 유닛으로 */
		}
		if (readq(iommu->reg + DMAR_RTADDR_REG) & DMA_RTADDR_SMT)	/* [한국어] SMT 비트가 컨텍스트 항목의 해석을 결정한다 */
			scalable = true;	/* [한국어] 스케일러블 모드 — PASID 두 계층을 거친다 */
		else
			scalable = false;	/* [한국어] 레거시 모드 — 컨텍스트가 곧 페이지 테이블을 가리킨다 */

		/*
		 * The iommu->lock is held across the callback, which will
		 * block calls to domain_attach/domain_detach. Hence,
		 * the domain of the device will not change during traversal.
		 *
		 * Traversing page table possibly races with the iommu_unmap()
		 * interface. This could be solved by RCU-freeing the page
		 * table pages in the iommu_unmap() path.
		 */
		spin_lock(&iommu->lock);	/* [한국어] 순회 중 장치의 도메인이 바뀌지 않게. 다만 unmap 과의 경쟁은 원 주석대로 남아 있다 */

		context = iommu_context_addr(iommu, bus, devfn, 0);	/* [한국어] 이 장치의 컨텍스트 항목 */
		if (!context || !context_present(context))	/* [한국어] 이 유닛에 이 장치가 없다 */
			goto iommu_unlock;	/* [한국어] 락을 풀고 다음 유닛으로 */

		if (scalable) {	/* scalable mode */	/* [한국어] 스케일러블 모드의 루트 찾기 */
			struct pasid_entry *pasid_tbl, *pasid_tbl_entry;	/* [한국어] PASID 표와 그 안의 항목 */
			struct pasid_dir_entry *dir_tbl, *dir_entry;	/* [한국어] PASID 디렉터리와 항목 */
			u16 dir_idx, tbl_idx, pgtt;	/* [한국어] 두 인덱스와 변환 타입 */
			u64 pasid_dir_ptr;	/* [한국어] 디렉터리의 물리 주소 */

			pasid_dir_ptr = context->lo & VTD_PAGE_MASK;	/* [한국어] 스케일러블 모드에서는 컨텍스트가 디렉터리를 가리킨다 */

			/* Dump specified device domain mappings with PASID. */
			dir_idx = pasid >> PASID_PDE_SHIFT;	/* [한국어] PASID 의 상위가 디렉터리 인덱스 */
			tbl_idx = pasid & PASID_PTE_MASK;	/* [한국어] 하위가 표 안의 인덱스 */

			dir_tbl = phys_to_virt(pasid_dir_ptr);	/* [한국어] 하드웨어 주소를 커널 주소로 */
			dir_entry = &dir_tbl[dir_idx];	/* [한국어] 해당 디렉터리 항목 */

			pasid_tbl = get_pasid_table_from_pde(dir_entry);	/* [한국어] 그 항목이 가리키는 PASID 표 */
			if (!pasid_tbl)	/* [한국어] 그 구간의 표가 아직 할당되지 않았다 */
				goto iommu_unlock;	/* [한국어] 볼 것이 없다 */

			pasid_tbl_entry = &pasid_tbl[tbl_idx];	/* [한국어] 이 PASID 의 항목 */
			if (!pasid_pte_is_present(pasid_tbl_entry))	/* [한국어] 쓰이지 않는 PASID */
				goto iommu_unlock;	/* [한국어] 볼 것이 없다 */

			/*
			 * According to PASID Granular Translation Type(PGTT),
			 * get the page table pointer.
			 */
			pgtt = (u16)(pasid_tbl_entry->val[0] & GENMASK_ULL(8, 6)) >> 6;	/* [한국어] PGTT — 이 PASID 가 1단계인지 2단계인지 중첩인지 */
			agaw = (u8)(pasid_tbl_entry->val[0] & GENMASK_ULL(4, 2)) >> 2;	/* [한국어] 주소 폭. 페이지 테이블의 깊이를 결정한다 */

			switch (pgtt) {	/* [한국어] 변환 타입에 따라 루트가 어느 워드에 있는지가 다르다 */
			case PASID_ENTRY_PGTT_FL_ONLY:	/* [한국어] 1단계만 — 프로세스 페이지 테이블을 그대로 쓰는 SVA 형식 */
				pgd = pasid_tbl_entry->val[2];	/* [한국어] 1단계 루트는 val[2]에 */
				break;	/* [한국어] 찾았다 */
			case PASID_ENTRY_PGTT_SL_ONLY:	/* [한국어] 2단계만 — 커널이 만든 IOVA 매핑 */
			case PASID_ENTRY_PGTT_NESTED:	/* [한국어] 중첩 — 게스트 1단계 위에 호스트 2단계 */
				pgd = pasid_tbl_entry->val[0];	/* [한국어] 두 경우 모두 2단계 루트가 val[0]에 있다 */
				break;	/* [한국어] 찾았다 */
			default:	/* [한국어] 패스스루 등 — 페이지 테이블 자체가 없다 */
				goto iommu_unlock;	/* [한국어] 찍을 것이 없다 */
			}
			pgd &= VTD_PAGE_MASK;	/* [한국어] 워드에 섞인 플래그 비트를 떼고 순수 주소만 */
		} else { /* legacy mode */	/* [한국어] 레거시 모드의 루트 찾기 */
			u8 tt = (u8)(context->lo & GENMASK_ULL(3, 2)) >> 2;	/* [한국어] TT — 컨텍스트 항목의 변환 타입 */

			/*
			 * According to Translation Type(TT),
			 * get the page table pointer(SSPTPTR).
			 */
			switch (tt) {	/* [한국어] 타입에 따라 페이지 테이블이 있는지가 갈린다 */
			case CONTEXT_TT_MULTI_LEVEL:	/* [한국어] 다단계 페이지 테이블 */
			case CONTEXT_TT_DEV_IOTLB:	/* [한국어] 장치 IOTLB 를 함께 쓰는 경우 — 표 형식은 같다 */
				pgd = context->lo & VTD_PAGE_MASK;	/* [한국어] 레거시에서는 컨텍스트의 lo 가 곧 SSPTPTR */
				agaw = context->hi & 7;	/* [한국어] 주소 폭은 hi 의 하위 3비트에 */
				break;	/* [한국어] 찾았다 */
			default:	/* [한국어] 패스스루 등 */
				goto iommu_unlock;	/* [한국어] 찍을 것이 없다 */
			}
		}

		seq_printf(m, "Device %04x:%02x:%02x.%x ",	/* [한국어] 어느 장치의 덤프인지 */
			   iommu->segment, bus, PCI_SLOT(devfn), PCI_FUNC(devfn));

		if (scalable)	/* [한국어] 스케일러블이면 */
			seq_printf(m, "with pasid %x @0x%llx\n", pasid, pgd);	/* [한국어] PASID 와 페이지 테이블 루트를 함께 */
		else
			seq_printf(m, "@0x%llx\n", pgd);	/* [한국어] 레거시면 루트만 */

		seq_printf(m, "%-17s\t%-18s\t%-18s\t%-18s\t%-18s\t%-s\n",	/* [한국어] 단계별 열 제목 — x86 페이지 테이블과 같은 이름을 쓴다 */
			   "IOVA_PFN", "PML5E", "PML4E", "PDPE", "PDE", "PTE");
		pgtable_walk_level(m, phys_to_virt(pgd), agaw + 2, 0, path);	/* [한국어] 루트부터 재귀로 내려간다. agaw + 2 가 실제 표 깊이다 */

		found = true;	/* [한국어] 담당 유닛을 찾았다 */
iommu_unlock:	/* [한국어] 어느 단계에서 포기했든 여기로 모여 락을 푼다 */
		spin_unlock(&iommu->lock);	/* [한국어] 성공이든 실패든 여기로 모여 락을 푼다 */
		if (found)	/* [한국어] 찾았으면 */
			break;	/* [한국어] 한 장치는 한 유닛에만 속하므로 더 볼 필요가 없다 */
	}
	rcu_read_unlock();	/* [한국어] 유닛 순회 종료 */

	return 0;	/* [한국어] 출력하지 못했더라도 오류로 보지 않는다 — 진단 도구다 */
}

/*
 * [한국어]
 * dev_domain_translation_struct_show - 장치의 일반 DMA 매핑을 덤프한다 (debugfs show)
 *
 * @m: 출력 대상. private 에 device_domain_info 가 걸려 있다.
 * @unused: 쓰지 않는다.
 * @return: domain_translation_struct_show() 의 결과.
 *
 * 장치별 debugfs 디렉터리 안의 "domain_translation_struct" 파일이 이 함수를
 * 부른다. PASID 를 IOMMU_NO_PASID 로 넘겨 "PASID 없는 일반 DMA 의 매핑"을
 * 보겠다는 뜻을 나타낸다.
 *
 * 호출 체인:
 *   read(2) → [이 함수] → domain_translation_struct_show()
 */
static int dev_domain_translation_struct_show(struct seq_file *m, void *unused)
{
	struct device_domain_info *info = (struct device_domain_info *)m->private;	/* [한국어] 파일을 만들 때 걸어 둔 장치 정보 */

	return domain_translation_struct_show(m, info, IOMMU_NO_PASID);	/* [한국어] PASID 없는 일반 DMA 의 매핑을 보겠다는 뜻 */
}
DEFINE_SHOW_ATTRIBUTE(dev_domain_translation_struct);	/* [한국어] 장치별 파일의 동작을 생성 */

/*
 * [한국어]
 * pasid_domain_translation_struct_show - 특정 PASID 의 매핑을 덤프한다 (debugfs show)
 *
 * @m: 출력 대상. private 에 dev_pasid_info 가 걸려 있다.
 * @unused: 쓰지 않는다.
 * @return: domain_translation_struct_show() 의 결과.
 *
 * SVA 나 게스트 할당처럼 한 장치가 여러 PASID 를 쓰는 경우, PASID 마다
 * 서로 다른 페이지 테이블을 본다. 그래서 장치 디렉터리 아래에 PASID 별
 * 하위 디렉터리가 생기고 각각 이 파일을 갖는다.
 *
 * dev_iommu_priv_get 으로 장치 정보를 다시 얻는 이유: dev_pasid_info 는
 * PASID 쪽 정보만 갖고 있어, 버스/devfn 은 장치의 IOMMU 정보에서 가져와야
 * 한다.
 *
 * 호출 체인:
 *   read(2) → [이 함수] → domain_translation_struct_show()
 */
static int pasid_domain_translation_struct_show(struct seq_file *m, void *unused)
{
	struct dev_pasid_info *dev_pasid = (struct dev_pasid_info *)m->private;	/* [한국어] PASID 별 파일에 걸어 둔 정보 */
	struct device_domain_info *info = dev_iommu_priv_get(dev_pasid->dev);	/* [한국어] 버스/devfn 은 장치 쪽 정보에서 가져와야 한다 */

	return domain_translation_struct_show(m, info, dev_pasid->pasid);	/* [한국어] 이 PASID 의 페이지 테이블만 따라간다 */
}
DEFINE_SHOW_ATTRIBUTE(pasid_domain_translation_struct);	/* [한국어] PASID 별 파일의 동작을 생성 */

/*
 * [한국어]
 * invalidation_queue_entry_show - 무효화 큐의 서술자를 전부 찍는다
 *
 * @m: 출력 대상.
 * @iommu: 큐를 가진 유닛.
 *
 * 큐의 모든 슬롯을 순서대로 낸다 — 유효한 것만이 아니라 전부다. 큐는
 * 순환 버퍼라 "지금 어디까지 처리됐는가"가 IQH/IQT 로만 표현되고, 그
 * 바깥의 슬롯에는 예전 명령이 그대로 남아 있다. 그 잔해까지 보여야
 * "명령이 언제 멈췄는지"를 되짚을 수 있다.
 *
 * desc_status 를 함께 찍는 것이 핵심이다. 이 배열은 소프트웨어가 관리하는
 * 슬롯 상태(FREE/IN_USE/DONE/ABORT)로, 하드웨어가 완료를 표시하는 곳이기도
 * 하다. 서술자 내용과 이 상태를 나란히 보면 "하드웨어가 받았는데 완료를
 * 못 했다" 같은 상황이 드러난다.
 *
 * 서술자 크기가 모드마다 다른 이유: 스케일러블 모드(smts)에서는 서술자가
 * 32바이트(qw0~qw3), 아니면 16바이트(qw0~qw1)다. qi_shift 가 그 차이를
 * 오프셋 계산에 반영한다.
 *
 * 호출 체인:
 *   invalidation_queue_show() → [이 함수]
 */
static void invalidation_queue_entry_show(struct seq_file *m,
					  struct intel_iommu *iommu)
{
	int index, shift = qi_shift(iommu);	/* [한국어] 슬롯 인덱스와, 서술자 크기에 따른 오프셋 시프트 */
	struct qi_desc *desc;	/* [한국어] 현재 슬롯의 서술자 */
	int offset;	/* [한국어] 큐 안에서의 바이트 오프셋 */

	if (ecap_smts(iommu->ecap))	/* [한국어] 스케일러블 모드면 서술자가 32바이트(네 워드)다 */
		seq_puts(m, "Index\t\tqw0\t\t\tqw1\t\t\tqw2\t\t\tqw3\t\t\tstatus\n");	/* [한국어] 네 워드짜리 열 제목 */
	else
		seq_puts(m, "Index\t\tqw0\t\t\tqw1\t\t\tstatus\n");	/* [한국어] 아니면 두 워드 */

	for (index = 0; index < QI_LENGTH; index++) {	/* [한국어] 유효한 것만이 아니라 모든 슬롯을 찍는다 — 잔해도 진단 정보다 */
		offset = index << shift;	/* [한국어] 서술자 크기를 반영한 오프셋 */
		desc = iommu->qi->desc + offset;	/* [한국어] 그 슬롯의 서술자 */
		if (ecap_smts(iommu->ecap))	/* [한국어] 모드에 따라 찍을 워드 수가 다르다 */
			seq_printf(m, "%5d\t%016llx\t%016llx\t%016llx\t%016llx\t%016x\n",	/* [한국어] 네 워드와 소프트웨어 상태 */
				   index, desc->qw0, desc->qw1,
				   desc->qw2, desc->qw3,
				   iommu->qi->desc_status[index]);
		else
			seq_printf(m, "%5d\t%016llx\t%016llx\t%016x\n",	/* [한국어] 두 워드와 상태. 상태는 하드웨어가 완료를 표시하는 곳이기도 하다 */
				   index, desc->qw0, desc->qw1,
				   iommu->qi->desc_status[index]);
	}
}

/*
 * [한국어]
 * invalidation_queue_show - 모든 유닛의 무효화 큐 상태를 덤프한다 (debugfs show)
 *
 * @m: 출력 대상.
 * @unused: 쓰지 않는다.
 * @return: 항상 0.
 *
 * IOMMU 가 멈춘 것처럼 보일 때 보는 파일이다. IQH(하드웨어가 처리한 지점)와
 * IQT(드라이버가 넣은 지점)가 벌어져 있으면 하드웨어가 명령을 소화하지
 * 못하고 있다는 뜻이고, 그것이 unmap 이 걸려 있는 이유인 경우가 많다.
 *
 * 두 레지스터를 shift 로 나누는 이유: IQH/IQT 는 바이트 오프셋으로 표현되어
 * 있어, 서술자 크기로 나눠야 "몇 번째 슬롯"이라는 사람이 읽을 수 있는
 * 숫자가 된다.
 *
 * q_lock 을 잡는 이유: 큐에 명령을 넣는 경로와 경쟁하면 IQT 와 서술자 내용이
 * 서로 다른 시점의 것이 되어 덤프가 앞뒤가 맞지 않게 된다.
 *
 * 호출 체인:
 *   read(2) → [이 함수] → invalidation_queue_entry_show()
 */
static int invalidation_queue_show(struct seq_file *m, void *unused)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회용 */
	struct intel_iommu *iommu;	/* [한국어] 현재 유닛 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */
	struct q_inval *qi;	/* [한국어] 그 유닛의 무효화 큐 */
	int shift;	/* [한국어] 서술자 크기 시프트 */

	rcu_read_lock();	/* [한국어] 유닛 목록 보호 */
	for_each_active_iommu(iommu, drhd) {	/* [한국어] 모든 유닛에 대해 */
		qi = iommu->qi;	/* [한국어] 큐 구조체 */
		shift = qi_shift(iommu);	/* [한국어] IQH/IQT 의 바이트 오프셋을 슬롯 번호로 바꿀 시프트 */

		if (!qi || !ecap_qis(iommu->ecap))	/* [한국어] 큐가 없거나 하드웨어가 지원하지 않으면 */
			continue;	/* [한국어] 볼 것이 없다 */

		seq_printf(m, "Invalidation queue on IOMMU: %s\n", iommu->name);	/* [한국어] 어느 유닛의 큐인지 */

		raw_spin_lock_irqsave(&qi->q_lock, flags);	/* [한국어] 명령을 넣는 경로와 경쟁하면 덤프가 앞뒤가 맞지 않게 된다 */
		seq_printf(m, " Base: 0x%llx\tHead: %lld\tTail: %lld\n",	/* [한국어] 큐의 물리 주소와 머리·꼬리 위치 */
			   (u64)virt_to_phys(qi->desc),	/* [한국어] 하드웨어가 보는 큐 주소 */
			   readq(iommu->reg + DMAR_IQH_REG) >> shift,	/* [한국어] 하드웨어가 처리한 지점 — 시프트로 슬롯 번호로 바꾼다 */
			   readq(iommu->reg + DMAR_IQT_REG) >> shift);	/* [한국어] 드라이버가 넣은 지점. 둘이 벌어져 있으면 하드웨어가 밀려 있다는 뜻 */
		invalidation_queue_entry_show(m, iommu);	/* [한국어] 슬롯 내용을 전부 찍는다 */
		raw_spin_unlock_irqrestore(&qi->q_lock, flags);	/* [한국어] 이 유닛 끝 */
		seq_putc(m, '\n');	/* [한국어] 유닛 사이 구분 */
	}
	rcu_read_unlock();	/* [한국어] 순회 종료 */

	return 0;	/* [한국어] 성공 */
}
DEFINE_SHOW_ATTRIBUTE(invalidation_queue);	/* [한국어] 무효화 큐 파일의 동작을 생성 */

#ifdef CONFIG_IRQ_REMAP	/* [한국어] 인터럽트 재매핑을 켠 커널에서만 아래 덤프 함수들이 존재한다 */
/*
 * [한국어]
 * ir_tbl_remap_entry_show - 평범한(비포스티드) 재매핑 항목을 찍는다
 *
 * @m: 출력 대상.
 * @iommu: 표를 가진 유닛.
 *
 * 각 항목의 소스 id(어느 장치가 이 인터럽트를 낼 수 있는가), 목적지 CPU,
 * 벡터를 사람이 읽을 수 있게 풀어 주고, 원시 128비트도 함께 낸다. 앞의
 * 셋만으로 대부분의 문제가 짚이지만, 드라이버의 해석이 의심스러울 때를
 * 위해 날것도 남겨 둔다.
 *
 * p_pst 인 항목을 건너뛰는 이유: 포스티드 항목은 필드 배치가 달라 같은
 * 열로 찍으면 뜻이 어긋난다. 그쪽은 ir_tbl_posted_entry_show 가 따로 맡는다.
 *
 * irq_2_ir_lock 을 잡는 이유: 인터럽트 이동이 항목을 통째로 갈아 끼우는
 * 중일 수 있다.
 *
 * 호출 체인:
 *   ir_translation_struct_show() → [이 함수]
 */
static void ir_tbl_remap_entry_show(struct seq_file *m,
				    struct intel_iommu *iommu)
{
	struct irte *ri_entry;	/* [한국어] 현재 항목 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */
	int idx;	/* [한국어] 표 인덱스 */

	seq_puts(m, " Entry SrcID   DstID    Vct IRTE_high\t\tIRTE_low\n");	/* [한국어] 열 제목 — 소스 id, 목적지, 벡터, 그리고 원시 값 */

	raw_spin_lock_irqsave(&irq_2_ir_lock, flags);	/* [한국어] 인터럽트 이동이 항목을 갈아 끼우는 중일 수 있다 */
	for (idx = 0; idx < INTR_REMAP_TABLE_ENTRIES; idx++) {	/* [한국어] 65536개 항목 전부를 훑는다 */
		ri_entry = &iommu->ir_table->base[idx];	/* [한국어] 그 항목 */
		if (!ri_entry->present || ri_entry->p_pst)	/* [한국어] 쓰이지 않거나 포스티드 형식이면 */
			continue;	/* [한국어] 건너뛴다. 포스티드는 필드 배치가 달라 따로 찍는다 */

		seq_printf(m, " %-5d %02x:%02x.%01x %08x %02x  %016llx\t%016llx\n",	/* [한국어] 해석한 값과 원시 128비트를 함께 */
			   idx, PCI_BUS_NUM(ri_entry->sid),
			   PCI_SLOT(ri_entry->sid), PCI_FUNC(ri_entry->sid),
			   ri_entry->dest_id, ri_entry->vector,
			   ri_entry->high, ri_entry->low);
	}
	raw_spin_unlock_irqrestore(&irq_2_ir_lock, flags);	/* [한국어] 순회 끝 */
}

/*
 * [한국어]
 * ir_tbl_posted_entry_show - 포스티드 인터럽트 항목을 찍는다
 *
 * @m: 출력 대상.
 * @iommu: 표를 가진 유닛.
 *
 * 포스티드 항목에는 목적지 APIC id 가 없다. 대신 PDA(Posted Descriptor
 * Address) — 인터럽트를 표시할 서술자의 물리 주소 — 가 들어 있어, 열 구성
 * 자체가 다르다. 그래서 평범한 항목과 함수를 나눠 두었다.
 *
 * pda_l 을 6비트 왼쪽으로 밀어 찍는 이유: 서술자는 64바이트 정렬이라
 * 하위 6비트가 항상 0이고, 항목에는 그것을 뺀 값이 저장된다. 원래 주소를
 * 보여 주려면 되돌려 놓아야 한다.
 *
 * 호출 체인:
 *   ir_translation_struct_show() → [이 함수]
 */
static void ir_tbl_posted_entry_show(struct seq_file *m,
				     struct intel_iommu *iommu)
{
	struct irte *pi_entry;	/* [한국어] 현재 항목 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */
	int idx;	/* [한국어] 표 인덱스 */

	seq_puts(m, " Entry SrcID   PDA_high PDA_low  Vct IRTE_high\t\tIRTE_low\n");	/* [한국어] 포스티드는 목적지 대신 서술자 주소(PDA)를 찍는다 */

	raw_spin_lock_irqsave(&irq_2_ir_lock, flags);	/* [한국어] 항목 갱신과 경쟁하지 않게 */
	for (idx = 0; idx < INTR_REMAP_TABLE_ENTRIES; idx++) {	/* [한국어] 표 전체를 훑는다 */
		pi_entry = &iommu->ir_table->base[idx];	/* [한국어] 그 항목 */
		if (!pi_entry->present || !pi_entry->p_pst)	/* [한국어] 쓰이지 않거나 포스티드가 아니면 */
			continue;	/* [한국어] 건너뛴다 */

		seq_printf(m, " %-5d %02x:%02x.%01x %08x %08x %02x  %016llx\t%016llx\n",	/* [한국어] 소스 id, PDA 상·하위, 벡터, 원시 값 */
			   idx, PCI_BUS_NUM(pi_entry->sid),
			   PCI_SLOT(pi_entry->sid), PCI_FUNC(pi_entry->sid),
			   pi_entry->pda_h, pi_entry->pda_l << 6,	/* [한국어] 하위는 64바이트 정렬로 6비트가 잘려 있어 되돌려 찍는다 */
			   pi_entry->vector, pi_entry->high,
			   pi_entry->low);
	}
	raw_spin_unlock_irqrestore(&irq_2_ir_lock, flags);	/* [한국어] 순회 끝 */
}

/*
 * For active IOMMUs go through the Interrupt remapping
 * table and print valid entries in a table format for
 * Remapped and Posted Interrupts.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * ir_translation_struct_show - 인터럽트 재매핑 표를 두 부분으로 나눠 덤프한다
 *
 * @m: 출력 대상.
 * @unused: 쓰지 않는다.
 * @return: 항상 0.
 *
 * 같은 표를 두 번 훑는다. 앞쪽은 평범한 재매핑 항목, "****" 뒤쪽은 포스티드
 * 항목이다. 한 번에 섞어 찍지 않는 이유는 두 형식의 필드 배치가 달라 열
 * 제목을 공유할 수 없기 때문이다.
 *
 * 두 순회의 조건이 다른 것도 눈여겨볼 만하다. 앞쪽은 GSTS 의 IRES 까지
 * 확인해 "실제로 켜져 있는가"를 따지지만, 뒤쪽은 표만 있으면 찍는다.
 * 포스티드 항목은 KVM 이 게스트에 장치를 넘길 때 만들어지므로, 표의 존재
 * 자체가 볼 가치가 있다는 판단이다.
 *
 * 호출 체인:
 *   read(2) → [이 함수] → ir_tbl_remap_entry_show()/ir_tbl_posted_entry_show()
 */
static int ir_translation_struct_show(struct seq_file *m, void *unused)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회용 */
	struct intel_iommu *iommu;	/* [한국어] 현재 유닛 */
	u64 irta;	/* [한국어] 표의 물리 주소 */
	u32 sts;	/* [한국어] 전역 상태 */

	rcu_read_lock();	/* [한국어] 유닛 목록 보호 */
	for_each_active_iommu(iommu, drhd) {	/* [한국어] 첫 순회 — 평범한 재매핑 항목 */
		if (!ecap_ir_support(iommu->ecap))	/* [한국어] 재매핑을 지원하지 않는 유닛은 */
			continue;	/* [한국어] 건너뛴다 */

		seq_printf(m, "Remapped Interrupt supported on IOMMU: %s\n",	/* [한국어] 어느 유닛인지 */
			   iommu->name);

		sts = readl(iommu->reg + DMAR_GSTS_REG);	/* [한국어] 실제로 켜져 있는지 확인 */
		if (iommu->ir_table && (sts & DMA_GSTS_IRES)) {	/* [한국어] 표가 있고 하드웨어도 켜져 있어야 의미가 있다 */
			irta = virt_to_phys(iommu->ir_table->base);	/* [한국어] 하드웨어가 보는 표 주소 */
			seq_printf(m, " IR table address:%llx\n", irta);	/* [한국어] 주소를 먼저 알리고 */
			ir_tbl_remap_entry_show(m, iommu);	/* [한국어] 항목들을 찍는다 */
		} else {
			seq_puts(m, "Interrupt Remapping is not enabled\n");	/* [한국어] 꺼져 있으면 표 내용은 무의미하다 */
		}
		seq_putc(m, '\n');	/* [한국어] 유닛 사이 구분 */
	}

	seq_puts(m, "****\n\n");	/* [한국어] 두 형식의 경계 — 열 구성이 여기서 바뀐다 */

	for_each_active_iommu(iommu, drhd) {	/* [한국어] 둘째 순회 — 포스티드 항목 */
		if (!cap_pi_support(iommu->cap))	/* [한국어] 포스티드를 지원하지 않는 유닛은 */
			continue;	/* [한국어] 건너뛴다 */

		seq_printf(m, "Posted Interrupt supported on IOMMU: %s\n",	/* [한국어] 어느 유닛인지 */
			   iommu->name);

		if (iommu->ir_table) {	/* [한국어] 여기서는 IRES 를 따지지 않는다 — 포스티드 항목은 KVM 이 만들므로 표의 존재만으로 볼 가치가 있다 */
			irta = virt_to_phys(iommu->ir_table->base);	/* [한국어] 표 주소 */
			seq_printf(m, " IR table address:%llx\n", irta);	/* [한국어] 알리고 */
			ir_tbl_posted_entry_show(m, iommu);	/* [한국어] 포스티드 항목들을 찍는다 */
		} else {
			seq_puts(m, "Interrupt Remapping is not enabled\n");	/* [한국어] 표가 없으면 그 사실만 */
		}
		seq_putc(m, '\n');	/* [한국어] 유닛 사이 구분 */
	}
	rcu_read_unlock();	/* [한국어] 순회 종료 */

	return 0;	/* [한국어] 성공 */
}
DEFINE_SHOW_ATTRIBUTE(ir_translation_struct);	/* [한국어] 인터럽트 재매핑 파일의 동작을 생성 */
#endif

/*
 * [한국어]
 * latency_show_one - 유닛 하나의 지연 통계를 찍는다
 *
 * @m: 출력 대상.
 * @iommu: 대상 유닛.
 * @drhd: 그 유닛의 표 정보(레지스터 주소 출력용).
 *
 * perf.c 가 모아 둔 지연 시간 통계(무효화, 페이지 폴트 처리 등)를 문자열로
 * 받아 그대로 낸다. 통계 계산은 perf.c 의 몫이고 이 함수는 전달만 한다.
 *
 * 전역 debug_buf 를 쓰는 이유: 이 경로는 seq_file 읽기 하나당 한 번씩만
 * 실행되고 사용자 공간에서 직렬화되므로, 스택에 1KB 를 잡는 대신 공용
 * 버퍼를 재사용한다.
 *
 * 호출 체인:
 *   latency_show() → [이 함수] → dmar_latency_snapshot()
 */
static void latency_show_one(struct seq_file *m, struct intel_iommu *iommu,
			     struct dmar_drhd_unit *drhd)
{
	seq_printf(m, "IOMMU: %s Register Base Address: %llx\n",	/* [한국어] 어느 유닛의 통계인지 */
		   iommu->name, drhd->reg_base_addr);

	dmar_latency_snapshot(iommu, debug_buf, DEBUG_BUFFER_SIZE);	/* [한국어] perf.c 가 모은 통계를 문자열로 받는다 */
	seq_printf(m, "%s\n", debug_buf);	/* [한국어] 그대로 낸다 — 계산은 perf.c 의 몫이다 */
}

/*
 * [한국어]
 * latency_show - 모든 유닛의 지연 통계를 덤프한다 (debugfs show)
 *
 * @m: 출력 대상.
 * @v: 쓰지 않는다.
 * @return: 항상 0.
 *
 * 이 파일은 읽기와 쓰기가 짝을 이룬다. 읽으면 통계를 보여 주고, 쓰면 어떤
 * 통계를 모을지 켜고 끈다(dmar_perf_latency_write). 통계 수집은 핫 경로에
 * 비용을 얹으므로 기본은 꺼져 있고, 필요할 때만 켠다.
 *
 * 호출 체인:
 *   read(2) → [이 함수] → latency_show_one()
 */
static int latency_show(struct seq_file *m, void *v)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회용 */
	struct intel_iommu *iommu;	/* [한국어] 현재 유닛 */

	rcu_read_lock();	/* [한국어] 유닛 목록 보호 */
	for_each_active_iommu(iommu, drhd)	/* [한국어] 모든 유닛의 통계를 */
		latency_show_one(m, iommu, drhd);	/* [한국어] 하나씩 찍는다 */
	rcu_read_unlock();	/* [한국어] 순회 종료 */

	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * dmar_perf_latency_open - 지연 통계 파일을 연다
 *
 * @inode: 파일의 inode.
 * @filp: 열린 파일.
 * @return: single_open() 의 결과.
 *
 * DEFINE_SHOW_ATTRIBUTE 를 쓰지 않고 open 을 직접 쓰는 이유: 이 파일은
 * 쓰기도 받아야 해서 file_operations 를 손으로 구성해야 하고, 그러면
 * open 도 직접 정의해야 한다.
 *
 * 호출 체인:
 *   open(2) → [이 함수] → single_open()
 */
static int dmar_perf_latency_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, latency_show, NULL);	/* [한국어] 한 번에 전부 출력하는 단순한 seq_file 로 연다 */
}

/*
 * [한국어]
 * dmar_perf_latency_write - 어떤 지연 통계를 모을지 켜고 끈다
 *
 * @filp: 열린 파일.
 * @ubuf: 사용자가 쓴 문자열.
 * @cnt: 그 길이.
 * @ppos: 파일 오프셋.
 * @return: 처리한 바이트 수, 또는 -EFAULT/-EINVAL.
 *
 * 이 파일에서 유일하게 하드웨어/드라이버 상태를 "바꾸는" 함수다. 숫자
 * 하나를 받아 무엇을 계측할지 정한다.
 *   0: 전부 끈다
 *   1: IOTLB 무효화 지연
 *   2: 장치 IOTLB(DevTLB) 무효화 지연
 *   3: 인터럽트 항목 캐시(IEC) 무효화 지연
 *
 * 켜고 끄는 것이 배타적이지 않다는 점이 중요하다 — 1, 2, 3을 차례로 쓰면
 * 셋 다 켜지고, 0 만이 전부 끈다. 통계 수집은 무효화 경로마다 시간을
 * 재는 일이라 공짜가 아니므로, 필요한 것만 골라 켤 수 있게 되어 있다.
 *
 * 입력을 63바이트로 자르는 이유: 숫자 하나만 받으면 되므로 그보다 긴
 * 입력은 의미가 없고, 고정 스택 버퍼를 넘지 않게 미리 자른다.
 *
 * 실행 컨텍스트: write(2) 의 프로세스 문맥. 유닛 목록은 RCU 로 보호한다.
 *
 * 호출 체인:
 *   write(2) → [이 함수] → dmar_latency_enable()/dmar_latency_disable()
 */
static ssize_t dmar_perf_latency_write(struct file *filp,
				       const char __user *ubuf,
				       size_t cnt, loff_t *ppos)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회용 */
	struct intel_iommu *iommu;	/* [한국어] 현재 유닛 */
	int counting;	/* [한국어] 사용자가 지정한 계측 종류 */
	char buf[64];	/* [한국어] 사용자 입력을 받을 스택 버퍼 */

	if (cnt > 63)	/* [한국어] 버퍼를 넘지 않게 */
		cnt = 63;	/* [한국어] 잘라 낸다. 숫자 하나만 받으면 되므로 손해가 없다 */

	if (copy_from_user(&buf, ubuf, cnt))	/* [한국어] 사용자 공간에서 복사 */
		return -EFAULT;	/* [한국어] 잘못된 포인터 */

	buf[cnt] = 0;	/* [한국어] 문자열 종료. kstrtoint 가 이것을 요구한다 */

	if (kstrtoint(buf, 0, &counting))	/* [한국어] 숫자로 파싱 */
		return -EINVAL;	/* [한국어] 숫자가 아니면 거절 */

	switch (counting) {	/* [한국어] 어떤 계측을 켤지 */
	case 0:	/* [한국어] 전부 끈다 */
		rcu_read_lock();	/* [한국어] 유닛 목록을 도는 동안 핫플러그로부터 보호 */
		for_each_active_iommu(iommu, drhd) {	/* [한국어] 모든 유닛에서 세 계측을 함께 끈다 */
			dmar_latency_disable(iommu, DMAR_LATENCY_INV_IOTLB);	/* [한국어] IOTLB 무효화 계측 해제 */
			dmar_latency_disable(iommu, DMAR_LATENCY_INV_DEVTLB);	/* [한국어] 장치 IOTLB 계측 해제 */
			dmar_latency_disable(iommu, DMAR_LATENCY_INV_IEC);	/* [한국어] 인터럽트 항목 캐시 계측 해제 */
		}
		rcu_read_unlock();	/* [한국어] 순회 종료 */
		break;	/* [한국어] 끄기 완료 */
	case 1:	/* [한국어] IOTLB 무효화 지연만 켠다 */
		rcu_read_lock();	/* [한국어] 같은 이유의 보호 */
		for_each_active_iommu(iommu, drhd)	/* [한국어] 모든 유닛에서 */
			dmar_latency_enable(iommu, DMAR_LATENCY_INV_IOTLB);	/* [한국어] 핫 경로에 시간 측정이 얹힌다 */
		rcu_read_unlock();	/* [한국어] 순회 종료 */
		break;	/* [한국어] 완료 */
	case 2:	/* [한국어] 장치 IOTLB 무효화 지연 */
		rcu_read_lock();	/* [한국어] 같은 이유의 보호 */
		for_each_active_iommu(iommu, drhd)	/* [한국어] 모든 유닛에서 */
			dmar_latency_enable(iommu, DMAR_LATENCY_INV_DEVTLB);	/* [한국어] ATS 장치의 응답 시간이 여기 잡힌다 */
		rcu_read_unlock();	/* [한국어] 순회 종료 */
		break;	/* [한국어] 완료 */
	case 3:	/* [한국어] 인터럽트 항목 캐시 무효화 지연 */
		rcu_read_lock();	/* [한국어] 같은 이유의 보호 */
		for_each_active_iommu(iommu, drhd)	/* [한국어] 모든 유닛에서 */
			dmar_latency_enable(iommu, DMAR_LATENCY_INV_IEC);	/* [한국어] 인터럽트 이동 비용을 재는 데 쓴다 */
		rcu_read_unlock();	/* [한국어] 순회 종료 */
		break;	/* [한국어] 완료 */
	default:	/* [한국어] 정의되지 않은 값 */
		return -EINVAL;	/* [한국어] 거절 */
	}

	*ppos += cnt;	/* [한국어] 파일 오프셋을 진행시킨다 */
	return cnt;	/* [한국어] 받아들인 바이트 수 */
}

/*
 * [한국어] struct file_operations dmar_perf_latency_fops — 지연 통계 파일의 동작
 *
 * 이 파일만 file_operations 를 손으로 만든다. 다른 파일은 읽기 전용이라
 * DEFINE_SHOW_ATTRIBUTE 가 만들어 주는 것으로 충분하지만, 여기는 쓰기로
 * 계측을 켜고 꺼야 해서 write 를 끼워 넣어야 한다.
 */
static const struct file_operations dmar_perf_latency_fops = {
	.open		= dmar_perf_latency_open,
	/* [한국어] 파일을 열 때 single_open 으로 seq_file 을 준비한다. */
	.write		= dmar_perf_latency_write,
	/* [한국어] 이 파일이 손으로 만들어진 이유. 숫자를 써서 계측을 켜고 끈다. */
	.read		= seq_read,
	/* [한국어] 읽기는 seq_file 의 표준 구현을 그대로 쓴다. */
	.llseek		= seq_lseek,
	/* [한국어] 오프셋 이동도 표준 구현. */
	.release	= single_release,
	/* [한국어] single_open 이 잡은 자원을 놓는다. */
};

/*
 * [한국어]
 * intel_iommu_debugfs_init - 이 드라이버의 debugfs 파일들을 만든다
 *
 * 부팅 때 한 번 불려 /sys/kernel/debug/iommu/intel/ 아래에 전역 파일들을
 * 만든다. 장치별 파일은 장치가 IOMMU 에 붙을 때
 * intel_iommu_debugfs_create_dev() 가 따로 만든다.
 *
 * 권한이 갈리는 이유: 대부분은 0444(읽기 전용)지만 dmar_perf_latency 만
 * 0644 다. 그 파일만 상태를 바꾸기 때문이다.
 *
 * 반환값을 확인하지 않는 이유: debugfs 파일 생성 실패는 진단 기능이
 * 없어질 뿐 드라이버 동작에는 아무 영향이 없다. 그래서 커널 관례대로
 * 오류를 무시한다.
 *
 * 호출 체인:
 *   intel_iommu_init() → [이 함수] → debugfs_create_file()
 */
void __init intel_iommu_debugfs_init(void)
{
	intel_iommu_debug = debugfs_create_dir("intel", iommu_debugfs_dir);	/* [한국어] IOMMU 공통 디렉터리 아래 Intel 전용 디렉터리 */

	debugfs_create_file("iommu_regset", 0444, intel_iommu_debug, NULL,	/* [한국어] 레지스터 덤프 — 읽기 전용 */
			    &iommu_regset_fops);
	debugfs_create_file("dmar_translation_struct", 0444, intel_iommu_debug,	/* [한국어] 변환 표 덤프 */
			    NULL, &dmar_translation_struct_fops);
	debugfs_create_file("invalidation_queue", 0444, intel_iommu_debug,	/* [한국어] 무효화 큐 덤프 */
			    NULL, &invalidation_queue_fops);
#ifdef CONFIG_IRQ_REMAP	/* [한국어] 재매핑 파일은 그 기능을 켠 커널에서만 만든다 */
	debugfs_create_file("ir_translation_struct", 0444, intel_iommu_debug,	/* [한국어] 인터럽트 재매핑 표 덤프 — 그 기능을 켠 커널에서만 */
			    NULL, &ir_translation_struct_fops);
#endif
	debugfs_create_file("dmar_perf_latency", 0644, intel_iommu_debug,	/* [한국어] 유일하게 쓰기가 가능한 파일이라 0644 */
			    NULL, &dmar_perf_latency_fops);
}

/*
 * Create a debugfs directory for each device, and then create a
 * debugfs file in this directory for users to dump the page table
 * of the default domain. e.g.
 * /sys/kernel/debug/iommu/intel/0000:00:01.0/domain_translation_struct
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * intel_iommu_debugfs_create_dev - 장치 하나의 debugfs 디렉터리를 만든다
 *
 * @info: 그 장치의 IOMMU 정보. 만든 dentry 를 여기에 보관한다.
 *
 * 장치가 IOMMU 도메인에 붙을 때 불린다. info 를 파일의 private 로 걸어
 * 두므로, 나중에 그 파일을 읽으면 곧바로 이 장치의 페이지 테이블로
 * 도달할 수 있다.
 *
 * dentry 를 info 에 보관하는 이유: 장치가 떨어질 때 그 디렉터리를 통째로
 * 지워야 하는데, 이름으로 다시 찾는 것보다 들고 있는 편이 확실하다.
 *
 * 호출 체인:
 *   장치의 도메인 attach 경로 → [이 함수]
 */
void intel_iommu_debugfs_create_dev(struct device_domain_info *info)
{
	info->debugfs_dentry = debugfs_create_dir(dev_name(info->dev), intel_iommu_debug);	/* [한국어] 장치 이름(0000:00:01.0)으로 디렉터리를 만들고 dentry 를 보관한다 */

	debugfs_create_file("domain_translation_struct", 0444, info->debugfs_dentry,	/* [한국어] info 를 private 로 걸어 두면 읽을 때 곧바로 이 장치의 표로 도달한다 */
			    info, &dev_domain_translation_struct_fops);
}

/* Remove the device debugfs directory. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * intel_iommu_debugfs_remove_dev - 장치의 debugfs 디렉터리를 지운다
 *
 * @info: 그 장치의 IOMMU 정보.
 *
 * recursive 를 쓰는 이유: 디렉터리 아래에 파일과 (PASID 를 쓴다면) 하위
 * 디렉터리가 함께 있어, 통째로 지워야 한다.
 *
 * 이 호출이 반환되면 그 아래 파일을 읽던 사용자도 정리되므로, info 를
 * 해제하기 전에 반드시 먼저 불려야 한다.
 *
 * 호출 체인:
 *   장치의 도메인 detach 경로 → [이 함수]
 */
void intel_iommu_debugfs_remove_dev(struct device_domain_info *info)
{
	debugfs_remove_recursive(info->debugfs_dentry);	/* [한국어] 아래 파일과 PASID 하위 디렉터리까지 통째로 지운다 */
}

/*
 * Create a debugfs directory per pair of {device, pasid}, then create the
 * corresponding debugfs file in this directory for users to dump its page
 * table. e.g.
 * /sys/kernel/debug/iommu/intel/0000:00:01.0/1/domain_translation_struct
 *
 * The debugfs only dumps the page tables whose mappings are created and
 * destroyed by the iommu_map/unmap() interfaces. Check the mapping type
 * of the domain before creating debugfs directory.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * intel_iommu_debugfs_create_dev_pasid - {장치, PASID} 쌍의 디렉터리를 만든다
 *
 * @dev_pasid: 그 쌍의 정보.
 *
 * 한 장치가 여러 PASID 를 쓰면 PASID 마다 다른 페이지 테이블을 본다. 그래서
 * 장치 디렉터리 아래에 PASID 를 이름으로 하는 하위 디렉터리를 만들고 같은
 * 이름의 파일을 둔다.
 *
 * 원 주석이 밝히는 제약: 여기서 덤프할 수 있는 것은 iommu_map/unmap 으로
 * 만들어진 매핑뿐이다. SVA 처럼 프로세스의 페이지 테이블을 그대로 쓰는
 * 도메인은 커널이 그 표를 소유하지 않으므로, 호출자가 도메인 종류를 먼저
 * 확인하고 부른다.
 *
 * 호출 체인:
 *   PASID attach 경로 → [이 함수]
 */
void intel_iommu_debugfs_create_dev_pasid(struct dev_pasid_info *dev_pasid)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev_pasid->dev);	/* [한국어] 부모가 될 장치 디렉터리를 찾기 위해 */
	char dir_name[10];	/* [한국어] PASID 를 16진수 문자열로 담을 버퍼 */

	sprintf(dir_name, "%x", dev_pasid->pasid);	/* [한국어] 디렉터리 이름은 PASID 값 자체 */
	dev_pasid->debugfs_dentry = debugfs_create_dir(dir_name, info->debugfs_dentry);	/* [한국어] 장치 디렉터리 아래에 만든다 */

	debugfs_create_file("domain_translation_struct", 0444, dev_pasid->debugfs_dentry,	/* [한국어] 같은 이름의 파일이지만 이쪽은 PASID 별 페이지 테이블을 본다 */
			    dev_pasid, &pasid_domain_translation_struct_fops);
}

/* Remove the device pasid debugfs directory. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * intel_iommu_debugfs_remove_dev_pasid - {장치, PASID} 디렉터리를 지운다
 *
 * @dev_pasid: 그 쌍의 정보.
 *
 * PASID 가 장치에서 떨어질 때 불린다. 장치 디렉터리 자체는 남고 그 아래
 * PASID 하위 디렉터리만 사라진다.
 *
 * 호출 체인:
 *   PASID detach 경로 → [이 함수]
 */
void intel_iommu_debugfs_remove_dev_pasid(struct dev_pasid_info *dev_pasid)
{
	debugfs_remove_recursive(dev_pasid->debugfs_dentry);	/* [한국어] PASID 하위 디렉터리만 지운다. 장치 디렉터리는 남는다 */
}
