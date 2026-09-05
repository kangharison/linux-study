// SPDX-License-Identifier: GPL-2.0

/*
 * [한국어 설명] 인터럽트 재매핑 — 장치가 낸 인터럽트를 표로 걸러 전달한다 (intel/irq_remapping.c)
 *
 * === 파일의 역할 ===
 * VT-d 유닛의 두 번째 기능을 구현한다. DMA 재매핑이 "장치가 어느 메모리에
 * 접근할 수 있는가"를 통제한다면, 인터럽트 재매핑은 "장치가 어느 인터럽트를
 * 누구에게 낼 수 있는가"를 통제한다.
 * 왜 필요한가: MSI(Message Signaled Interrupt)는 결국 특정 주소에 특정 값을
 * 쓰는 DMA 다. 그래서 장치가 주소와 값을 마음대로 정하면 아무 인터럽트나
 * 아무 CPU 에 쏠 수 있고, 그것으로 커널의 인터럽트 처리를 흔들 수 있다.
 * 재매핑을 켜면 그 메시지가 곧바로 CPU 로 가지 않고, 커널이 관리하는
 * 재매핑 표(IRTE)의 인덱스로 해석된다. 표에는 그 장치에 허용된 벡터와
 * 목적지만 적혀 있으므로, 장치는 자기 몫의 인터럽트만 낼 수 있다.
 * 실용적인 이유도 있다: x2APIC 의 32비트 APIC id 는 MSI 주소 필드에
 * 들어가지 않아, 재매핑 없이는 큰 시스템에서 x2APIC 를 쓸 수 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * DMA 재매핑과 같은 하드웨어(struct intel_iommu)를 공유하지만 경로는 완전히
 * 다르다.
 *   [DMA]   장치 → DMA 요청 → 루트/컨텍스트/페이지 테이블 → 메모리
 *   [인터럽트] 장치 → MSI 메시지 → 재매핑 표(IRTE) → 벡터·목적지 CPU
 * 위쪽으로는 커널의 인터럽트 도메인 계층이 이 파일을 벤더 백엔드로 쓴다 —
 * 인터럽트를 할당·이동·해제할 때마다 여기가 IRTE 를 채우고 무효화한다.
 * 아래쪽으로는 dmar.c 가 만든 유닛 목록을 쓰고, 그 유닛의 무효화 큐로
 * 인터럽트 항목 캐시 무효화를 보낸다.
 * 실행 컨텍스트: 커널 모듈. 인터럽트 할당은 프로세스 컨텍스트지만, 인터럽트
 * 이동(affinity 변경)은 인터럽트를 끈 문맥에서도 일어나므로 락이 그에 맞게
 * 선택되어 있다.
 *
 * === 타 모듈과의 연결 ===
 * - dmar.c: 유닛 목록과 DMAR 표 파싱 결과를 쓴다. dmar_ir_support() 가
 *   플랫폼 지원 여부를, dmar_ir_hotplug() 가 유닛 핫플러그를 알려 준다.
 * - iommu.h: 유닛 구조체의 ir_table/ir_domain 필드와 IRTA 레지스터 정의.
 * - drivers/iommu/irq_remapping.c(코어): 벤더 중립 인터페이스. 이 파일이
 *   struct irq_remap_ops 를 채워 등록한다.
 * - x86 인터럽트 계층(io_apic, apic, msi): IOAPIC 과 MSI 의 인터럽트를
 *   이 도메인을 거치게 한다.
 * - ACPI: IOAPIC 과 HPET 의 위치를 DMAR 표에서 읽어 어느 유닛이 담당하는지
 *   판별한다(ioapic_scope/hpet_scope).
 * 데이터 흐름: 부팅 시 유닛마다 재매핑 표를 만들고 하드웨어에 알린다 →
 * 인터럽트를 할당하면 빈 IRTE 를 잡아 벡터·목적지를 적는다 → 그 인덱스가
 * MSI 주소·데이터에 담겨 장치에 설정된다 → 장치가 인터럽트를 내면 하드웨어가
 * 그 인덱스로 표를 찾아 실제 벡터로 바꿔 전달한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ioapic_scope/hpet_scope: DMAR 표가 알려 준 IOAPIC·HPET 의 위치와
 *   담당 유닛. 이 둘은 PCI 장치가 아니라 별도로 추적해야 한다.
 * - struct irq_2_iommu: 커널 인터럽트 하나가 어느 유닛의 몇 번 IRTE 를
 *   쓰는지. 인터럽트 서술자에 매달린다.
 * - intel_setup_irq_remapping(): 유닛의 재매핑 표를 만들고 켠다.
 * - alloc_irte()/modify_irte()/free_irte(): 표 항목의 수명. modify_irte 가
 *   항목을 고친 뒤 캐시를 무효화하는 것이 정확성의 핵심이다.
 * - intel_irq_remapping_alloc()/free(): 커널 인터럽트 도메인 콜백.
 * - intel_ir_set_affinity(): 인터럽트를 다른 CPU 로 옮긴다.
 * - struct irq_remap_ops intel_irq_remap_ops: 코어에 등록하는 콜백 표.
 */
#define pr_fmt(fmt)     "DMAR-IR: " fmt	/* [한국어] 이 파일의 로그 접두사. DMA 쪽과 구분되도록 -IR 을 붙인다 */

#include <linux/interrupt.h>	/* [한국어] 인터럽트 서술자와 등록 */
#include <linux/dmar.h>	/* [한국어] DRHD 유닛 목록 */
#include <linux/spinlock.h>	/* [한국어] 재매핑 표를 지키는 락 */
#include <linux/slab.h>	/* [한국어] 자료구조 할당 */
#include <linux/jiffies.h>	/* [한국어] 시간 측정 */
#include <linux/hpet.h>	/* [한국어] HPET 타이머 — DMAR 표가 그 인터럽트도 지목한다 */
#include <linux/pci.h>	/* [한국어] 소스 id 와 별칭 */
#include <linux/irq.h>	/* [한국어] 인터럽트 코어 타입 */
#include <linux/irqchip/irq-msi-lib.h>	/* [한국어] MSI 도메인 공통 코드 */
#include <linux/acpi.h>	/* [한국어] ACPI 표 접근 */
#include <linux/irqdomain.h>	/* [한국어] 인터럽트 도메인 — 이 파일이 하나를 만들어 등록한다 */
#include <linux/crash_dump.h>	/* [한국어] kdump 여부 판별. 물려받은 재매핑 표를 다루는 데 쓴다 */
#include <asm/io_apic.h>	/* [한국어] IOAPIC 인터럽트 */
#include <asm/apic.h>	/* [한국어] APIC id 와 목적지 계산 */
#include <asm/smp.h>	/* [한국어] CPU 정보 */
#include <asm/cpu.h>	/* [한국어] CPU 기능 */
#include <asm/irq_remapping.h>	/* [한국어] 아키텍처별 재매핑 인터페이스 */
#include <asm/pci-direct.h>	/* [한국어] 초기 PCI 설정 공간 접근 */
#include <asm/posted_intr.h>	/* [한국어] 포스티드 인터럽트 — VMM 을 거치지 않고 게스트에 직접 전달한다 */

#include "iommu.h"	/* [한국어] 유닛 구조체와 IRTA 레지스터 */
#include "../irq_remapping.h"	/* [한국어] 코어의 벤더 중립 인터페이스 */
#include "../iommu-pages.h"	/* [한국어] 재매핑 표를 잡는 공용 할당기 */

/*
 * [한국어] struct ioapic_scope — DMAR 표가 지목한 IOAPIC 하나
 *
 * IOAPIC 은 PCI 장치가 아니지만 인터럽트를 내므로 재매핑 대상이다. 그런데
 * PCI 장치가 아니라 버스 알림으로 발견되지 않아서, DMAR 표의 device scope 가
 * 알려 준 위치를 이 배열에 따로 담아 둔다.
 *
 * bus/devfn 은 실제 PCI 위치가 아니라 "DMAR 이 이 IOAPIC 에 부여한 소스 id"
 * 다. 하드웨어가 인터럽트의 출처를 그 값으로 식별하므로, 재매핑 항목의
 * 소스 검증에 쓰인다.
 */
struct ioapic_scope {
	struct intel_iommu *iommu;
	/* [한국어] 이 IOAPIC 의 인터럽트를 담당하는 유닛.
	 * 설정자: DMAR 표 파싱이 device scope 로 이 IOAPIC 을 지목한 유닛을 찾아 채운다.
	 * 읽는 자: 그 IOAPIC 의 인터럽트를 할당·수정할 때 어느 유닛의 표를 쓸지.
	 * 동기화: 부팅 시 한 번 쓰고 이후 읽기만 한다. */
	unsigned int id;
	/* [한국어] IOAPIC 의 id. ACPI MADT 가 부여한 번호이며, 커널의 IOAPIC 배열과 대조된다.
	 * 설정자: 표 파싱. 읽는 자: 인터럽트가 어느 IOAPIC 의 것인지 판별할 때. */
	unsigned int bus;	/* PCI bus number */
	/* [한국어] DMAR 이 이 IOAPIC 에 부여한 버스 번호 (원 주석: PCI bus number).
	 * 실제 PCI 버스가 아니라, 하드웨어가 인터럽트의 출처를 식별하는 소스 id 의
	 *   일부다. 재매핑 항목의 소스 검증 필드에 들어간다.
	 * 설정자: 표 파싱. 읽는 자: IRTE 를 채울 때. */
	unsigned int devfn;	/* PCI devfn number */
	/* [한국어] 같은 소스 id 의 devfn 부분 (원 주석: PCI devfn number).
	 * bus 와 함께 16비트 소스 id 를 이룬다. */
};

/*
 * [한국어] struct hpet_scope — DMAR 표가 지목한 HPET 하나
 *
 * ioapic_scope 와 같은 이유로 존재한다. HPET(High Precision Event Timer)도
 * PCI 장치가 아니면서 MSI 를 내므로, 그 소스 id 를 표에서 받아 따로 기억한다.
 */
struct hpet_scope {
	struct intel_iommu *iommu;
	/* [한국어] 이 HPET 의 인터럽트를 담당하는 유닛.
	 * 설정자/읽는 자/동기화: ioapic_scope 의 같은 필드와 같다. */
	u8 id;
	/* [한국어] HPET 블록의 id. DMAR 표의 enumeration id 와 대응한다.
	 * 설정자: 표 파싱. 읽는 자: 어느 HPET 의 인터럽트인지 판별. */
	unsigned int bus;
	/* [한국어] DMAR 이 이 HPET 에 부여한 버스 번호. 소스 id 의 일부다.
	 * 설정자: 표 파싱. 읽는 자: IRTE 의 소스 검증 필드. */
	unsigned int devfn;
	/* [한국어] 같은 소스 id 의 devfn 부분. */
};

/*
 * [한국어] struct irq_2_iommu — 커널 인터럽트 하나와 재매핑 표 항목의 연결
 *
 * 인터럽트 서술자(irq_data)의 칩 데이터에 매달려, "이 인터럽트는 어느
 * 유닛의 몇 번 항목을 쓰는가"를 기억한다. 인터럽트를 옮기거나 해제할 때
 * 그 항목을 찾아가는 유일한 단서다.
 *
 * irte_mask 가 있는 이유: 다중 벡터 MSI 는 연속된 항목 여러 개를 한꺼번에
 * 잡는다. 그 개수는 2의 거듭제곱이어야 하고, 시작 인덱스도 그 크기에
 * 정렬되어야 한다 — 하드웨어가 인덱스의 하위 비트를 벡터 번호로 쓰기
 * 때문이다. mask 가 그 하위 비트 수를 담는다.
 */
struct irq_2_iommu {
	struct intel_iommu *iommu;
	/* [한국어] 이 인터럽트의 재매핑 항목이 있는 유닛.
	 * 설정자: 인터럽트 할당(intel_irq_remapping_alloc).
	 * 읽는 자: 항목을 고치거나 해제할 때, 그리고 캐시 무효화를 보낼 때.
	 * 동기화: 인터럽트의 수명 동안 바뀌지 않는다. */
	u16 irte_index;
	/* [한국어] 그 유닛의 재매핑 표에서 이 인터럽트가 쓰는 항목의 인덱스.
	 * 설정자: alloc_irte() 가 빈 자리를 찾아 정한다.
	 * 읽는 자: modify_irte()/free_irte(), 그리고 MSI 주소·데이터를 만들 때 —
	 *   이 인덱스가 그대로 장치에 설정되어, 장치가 인터럽트를 내면 하드웨어가
	 *   그것으로 표를 찾는다.
	 * 값 범위: 0 ~ INTR_REMAP_TABLE_ENTRIES-1. */
	u16 sub_handle;
	/* [한국어] 다중 벡터 MSI 에서 이 인터럽트가 묶음 안의 몇 번째인지.
	 * 설정자: 할당 시. 읽는 자: 실제 항목 인덱스를 irte_index + sub_handle 로
	 *   계산하는 곳.
	 * 왜 필요한가: 다중 벡터 MSI 는 연속된 항목을 한 번에 잡고, 장치는 벡터
	 *   번호를 인덱스의 하위 비트로 표현한다. 그래서 묶음의 시작만 기억하고
	 *   개별 인터럽트는 그 안의 위치로 구분한다. */
	u8  irte_mask;
	/* [한국어] 그 묶음의 크기를 나타내는 하위 비트 수(log2).
	 * 설정자: 할당 시 요청한 벡터 수에서 계산한다.
	 * 읽는 자: 해제할 때 몇 개를 놓을지, 그리고 무효화 범위를 정할 때.
	 * 값 범위: 0 이면 항목 하나. 하드웨어가 인덱스의 하위 mask 비트를 벡터
	 *   번호로 쓰므로, 시작 인덱스도 그 크기에 정렬되어 있어야 한다. */
	bool posted_msi;
	/* [한국어] 이 인터럽트가 포스티드 MSI 인지.
	 * 포스티드 인터럽트는 하드웨어가 인터럽트를 곧바로 전달하지 않고 메모리의
	 *   비트맵에 표시한 뒤 알림 벡터 하나만 보내는 방식이다. 여러 인터럽트가
	 *   하나로 합쳐져 오버헤드가 줄어든다.
	 * 설정자/읽는 자: 항목을 채우고 해석하는 경로. */
	bool posted_vcpu;
	/* [한국어] 이 인터럽트가 게스트 vCPU 에 직접 전달되는지.
	 * VMM 을 거치지 않고 게스트에 인터럽트가 도달하게 하는 기능이라, 항목의
	 *   형식과 목적지 해석이 달라진다.
	 * 설정자/읽는 자: KVM 이 장치를 게스트에 넘길 때의 설정 경로. */
};

/*
 * [한국어] struct intel_ir_data — 인터럽트 하나가 들고 다니는 재매핑 상태 전부
 *
 * 커널의 irq_data 에 chip_data 로 매달리는 구조체다. 인터럽트를 옮기거나
 * (set_affinity) 게스트에 넘기거나(set_vcpu_affinity) 해제할 때마다 이
 * 구조체를 통해 "어느 유닛의 몇 번 항목인가"와 "그 항목의 현재 내용"에
 * 도달한다.
 *
 * irte_entry 를 굳이 소프트웨어에도 복사해 두는 이유: 표의 항목은 캐시가
 * 걸린 하드웨어 자료구조라 부분 수정이 위험하다. 그래서 수정은 항상
 * "소프트웨어 사본을 고쳐 통째로 다시 쓴다"로 하고, 사본이 그 원본이 된다.
 */
struct intel_ir_data {
	struct irq_2_iommu			irq_2_iommu;
	/* [한국어] 이 인터럽트가 쓰는 재매핑 표 항목의 위치(유닛 + 인덱스 + 묶음 크기).
	 * 설정자: intel_irq_remapping_alloc() 이 alloc_irte() 결과로 채운다.
	 * 읽는 자: modify_irte()/free_irte() 가 고칠 항목을 찾는 유일한 경로.
	 * 동기화: 인덱스 자체는 할당 후 바뀌지 않지만, 그것이 가리키는 표 항목을
	 *   건드릴 때는 irq_2_ir_lock 을 잡는다. */
	struct irte				irte_entry;
	/* [한국어] 그 표 항목의 소프트웨어 사본.
	 * 설정자: prepare_irte() 가 벡터·목적지로 채우고, affinity 변경마다 갱신된다.
	 * 읽는 자: modify_irte() 가 이 값을 통째로 하드웨어 항목에 써 넣는다.
	 * 왜 사본을 두는가: 하드웨어 항목은 128비트를 원자적으로 갈아 끼워야 해서
	 *   필드 하나만 고치는 부분 수정을 할 수 없다. 사본에서 조립한 뒤 한 번에 쓴다. */
	union {
		struct msi_msg			msi_entry;
		/* [한국어] 이 인터럽트를 내기 위해 장치의 MSI 레지스터에 써 넣을 주소·데이터.
		 * 재매핑이 켜지면 이 값은 더 이상 "어느 CPU 의 몇 번 벡터"가 아니라
		 *   "재매핑 표의 몇 번 항목"을 가리키는 핸들이 된다. 실제 목적지는 표가 정한다.
		 * 설정자: fill_msi_msg() 가 표 인덱스로부터 만든다.
		 * 읽는 자: MSI 코어가 장치에 써 넣을 때.
		 * union 인 이유: IOAPIC 인터럽트는 이 필드를 쓰지 않아, 장래에 다른 종류의
		 *   전달 정보가 추가될 자리를 열어 둔 것이다. */
	};
};

#define IR_X2APIC_MODE(mode) (mode ? (1 << 11) : 0)	/* [한국어] IRTA 레지스터의 EIME 비트(11번). x2APIC 모드에서는 목적지가 32비트 APIC id 라, 표 주소를 쓸 때 이 비트로 하드웨어에 알린다 */
#define IRTE_DEST(dest) ((eim_mode) ? dest : dest << 8)	/* [한국어] 항목의 목적지 필드 채우는 법. x2APIC(eim)이면 32비트 id 를 그대로, 아니면 8비트 id 를 상위로 밀어 넣는다 — 하드웨어가 두 형식을 다르게 읽기 때문 */

static int __read_mostly eim_mode;	/* [한국어] x2APIC(확장 인터럽트 모드)로 재매핑을 켰는지. 부팅 시 한 번 정해지고 이후 읽기만 하므로 __read_mostly */
static struct ioapic_scope ir_ioapic[MAX_IO_APICS];	/* [한국어] DMAR 표가 지목한 IOAPIC 들. 인터럽트가 오면 apic 번호로 이 배열을 뒤져 담당 유닛을 찾는다 */
static struct hpet_scope ir_hpet[MAX_HPET_TBS];	/* [한국어] 같은 목적의 HPET 배열 */

/*
 * Lock ordering:
 * ->dmar_global_lock
 *	->irq_2_ir_lock
 *		->qi->q_lock
 *	->iommu->register_lock
 * Note:
 * intel_irq_remap_ops.{supported,prepare,enable,disable,reenable} are called
 * in single-threaded environment with interrupt disabled, so no need to tabke
 * the dmar_global_lock.
 */
DEFINE_RAW_SPINLOCK(irq_2_ir_lock);	/* [한국어] 재매핑 표(항목 배열과 사용 비트맵)를 지키는 락. raw 인 이유: 인터럽트 할당·이동 경로에서 잡히므로 PREEMPT_RT 에서도 잠들면 안 된다 */
static const struct irq_domain_ops intel_ir_domain_ops;	/* [한국어] 아래에서 정의되는 도메인 콜백 표의 전방 선언 */

static void iommu_disable_irq_remapping(struct intel_iommu *iommu);	/* [한국어] 초기화 실패 정리 경로가 정의보다 먼저 쓰므로 전방 선언 */
static int __init parse_ioapics_under_ir(void);	/* [한국어] 같은 이유의 전방 선언 */
static const struct msi_parent_ops dmar_msi_parent_ops;	/* [한국어] MSI 부모 도메인 콜백 표의 전방 선언 */

/*
 * [한국어]
 * ir_pre_enabled - 커널이 켜기 전부터 인터럽트 재매핑이 켜져 있었는가
 *
 * @iommu: 검사할 VT-d 유닛.
 * @return: 커널 진입 시점에 이미 재매핑이 동작 중이었으면 true.
 *
 * 왜 필요한가: kdump 로 부팅한 두 번째 커널은 크래시한 첫 커널이 켜 둔
 * 재매핑을 그대로 물려받는다. 이때 재매핑을 무턱대고 끄면, 아직 살아 있는
 * 장치들이 옛 벡터로 인터럽트를 쏘아 새 커널을 곧바로 망가뜨린다. 그래서
 * "물려받았는가"를 기억해 두고, 표를 새로 만들되 전환은 조심스럽게 한다.
 *
 * 실행 컨텍스트: 부팅 초기 단일 스레드. 플래그는 init_ir_status() 가 한 번
 * 세우고 이후 읽기만 한다.
 *
 * 호출 체인:
 *   intel_setup_irq_remapping()/iommu_enable_irq_remapping() → [이 함수]
 */
static bool ir_pre_enabled(struct intel_iommu *iommu)
{
	return (iommu->flags & VTD_FLAG_IRQ_REMAP_PRE_ENABLED);	/* [한국어] 유닛 플래그에 "진입 시 이미 켜져 있었음" 표시가 있는지 */
}

/*
 * [한국어]
 * clear_ir_pre_enabled - "물려받은 재매핑" 표시를 지운다
 *
 * @iommu: 대상 유닛.
 *
 * 물려받은 표에서 커널이 새로 만든 표로 완전히 갈아탄 뒤에 호출된다. 이후로는
 * 더 이상 옛 설정을 배려할 필요가 없으므로, 일반 경로와 똑같이 다루기 위해
 * 플래그를 내린다.
 *
 * 호출 체인:
 *   iommu_enable_irq_remapping() → [이 함수]
 */
static void clear_ir_pre_enabled(struct intel_iommu *iommu)
{
	iommu->flags &= ~VTD_FLAG_IRQ_REMAP_PRE_ENABLED;	/* [한국어] 표시를 내린다 — 이제 커널이 만든 표로 완전히 넘어왔다 */
}

/*
 * [한국어]
 * init_ir_status - 하드웨어에게 지금 재매핑이 켜져 있는지 물어 플래그를 세운다
 *
 * @iommu: 대상 유닛.
 *
 * GSTS(전역 상태) 레지스터의 IRES 비트가 "인터럽트 재매핑 활성"을 뜻한다.
 * GCMD 는 쓰기 전용이라 읽어도 현재 설정을 알 수 없지만, GSTS 는 하드웨어가
 * 실제로 반영한 상태를 돌려주므로 부팅 시점의 진짜 상태를 여기서 확인한다.
 *
 * 실행 컨텍스트: 유닛 초기화. 이 한 번의 판독이 이후 kdump 경로 전체의
 * 분기 기준이 된다.
 *
 * 호출 체인:
 *   intel_setup_irq_remapping() → [이 함수] → readl(DMAR_GSTS_REG)
 */
static void init_ir_status(struct intel_iommu *iommu)
{
	u32 gsts;	/* [한국어] 전역 상태 레지스터 값을 담을 변수 */

	gsts = readl(iommu->reg + DMAR_GSTS_REG);	/* [한국어] GSTS 를 읽는다. GCMD 는 쓰기 전용이라 현재 설정을 알려면 이 레지스터를 봐야 한다 */
	if (gsts & DMA_GSTS_IRES)	/* [한국어] IRES = 인터럽트 재매핑이 실제로 동작 중 */
		iommu->flags |= VTD_FLAG_IRQ_REMAP_PRE_ENABLED;	/* [한국어] 물려받은 상태임을 기록 — 이후 초기화가 전환을 조심스럽게 하도록 */
}

/*
 * [한국어]
 * alloc_irte - 재매핑 표에서 연속된 항목 count 개를 잡는다
 *
 * @iommu: 항목을 잡을 유닛.
 * @irq_iommu: 결과(유닛/인덱스/묶음 크기)를 적어 줄 연결 구조체.
 * @count: 필요한 항목 수. 다중 벡터 MSI 면 1보다 크다.
 * @return: 잡은 첫 항목의 인덱스, 실패하면 -1.
 *
 * 왜 2의 거듭제곱으로 올림하는가: 다중 벡터 MSI 장치는 벡터 번호를 "기준
 * 인덱스의 하위 비트를 갈아 끼우는" 방식으로 표현한다. 그래서 묶음은 크기가
 * 2의 거듭제곱이어야 하고 시작 인덱스도 그 크기에 정렬되어야 한다. 그 정렬된
 * 자리 찾기를 bitmap_find_free_region() 이 mask 인자로 해 준다.
 *
 * mask 상한 검사: 무효화 명령은 "인덱스 + 하위 mask 비트"를 한 번에 지우는데,
 * 하드웨어가 지원하는 최대 mask 가 ecap 에 적혀 있다. 그보다 큰 묶음은 한 번에
 * 무효화할 수 없으므로 아예 할당을 거절한다.
 *
 * 실행 컨텍스트: 인터럽트 할당 경로(프로세스 문맥). 표와 비트맵을
 * irq_2_ir_lock 으로 지킨다.
 *
 * 호출 체인:
 *   intel_irq_remapping_alloc() → [이 함수] → bitmap_find_free_region()
 */
static int alloc_irte(struct intel_iommu *iommu,
		      struct irq_2_iommu *irq_iommu, u16 count)
{
	struct ir_table *table = iommu->ir_table;	/* [한국어] 이 유닛의 재매핑 표(항목 배열 + 사용 비트맵) */
	unsigned int mask = 0;	/* [한국어] 묶음 크기의 log2. 기본은 0(항목 하나) */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */
	int index;	/* [한국어] 찾아낸 시작 인덱스 */

	if (!count || !irq_iommu)	/* [한국어] 0 개 요청이나 결과를 받을 곳이 없는 호출은 무의미 */
		return -1;	/* [한국어] 실패. 호출자는 인터럽트 할당을 포기한다 */

	if (count > 1) {	/* [한국어] 다중 벡터 요청 — 연속·정렬된 묶음이 필요하다 */
		count = __roundup_pow_of_two(count);	/* [한국어] 2의 거듭제곱으로 올림. 하드웨어가 벡터 번호를 인덱스 하위 비트로 표현하므로 중간 크기는 존재할 수 없다 */
		mask = ilog2(count);	/* [한국어] 그 크기를 비트 수로. bitmap_find_free_region 이 이 값으로 정렬된 자리를 찾는다 */
	}

	if (mask > ecap_max_handle_mask(iommu->ecap)) {	/* [한국어] 하드웨어가 한 번에 무효화할 수 있는 최대 범위를 넘는가 */
		pr_err("Requested mask %x exceeds the max invalidation handle"	/* [한국어] 넘으면 할당해도 캐시를 지울 방법이 없어 거절한다 */
		       " mask value %Lx\n", mask,
		       ecap_max_handle_mask(iommu->ecap));
		return -1;	/* [한국어] 할당 실패. 호출자는 이 인터럽트를 포기한다 */
	}

	raw_spin_lock_irqsave(&irq_2_ir_lock, flags);	/* [한국어] 비트맵과 표를 지킨다. 인터럽트 문맥에서도 들어올 수 있어 irqsave */
	index = bitmap_find_free_region(table->bitmap,	/* [한국어] mask 에 정렬된 연속 빈 자리를 찾아 그대로 예약까지 한다 */
					INTR_REMAP_TABLE_ENTRIES, mask);
	if (index < 0) {	/* [한국어] 표가 꽉 찼다 — 이 유닛 아래 인터럽트가 65536개를 넘었다는 뜻 */
		pr_warn("IR%d: can't allocate an IRTE\n", iommu->seq_id);	/* [한국어] 어느 유닛에서 고갈됐는지 알린다 */
	} else {
		irq_iommu->iommu = iommu;	/* [한국어] 이후 수정·해제가 찾아올 수 있도록 담당 유닛을 기록 */
		irq_iommu->irte_index =  index;	/* [한국어] 묶음의 시작 인덱스 */
		irq_iommu->sub_handle = 0;	/* [한국어] 묶음 안의 위치. 첫 인터럽트는 0, 나머지는 호출자가 늘려 간다 */
		irq_iommu->irte_mask = mask;	/* [한국어] 해제와 무효화 때 몇 개를 다룰지의 근거 */
	}
	raw_spin_unlock_irqrestore(&irq_2_ir_lock, flags);	/* [한국어] 예약과 기록이 끝났다 */

	return index;	/* [한국어] 성공하면 시작 인덱스, 실패하면 -1 */
}

/*
 * [한국어]
 * qi_flush_iec - 인터럽트 항목 캐시(IEC)를 무효화한다
 *
 * @iommu: 명령을 보낼 유닛.
 * @index: 무효화할 첫 항목의 인덱스.
 * @mask: 함께 무효화할 범위(하위 mask 비트). 0 이면 항목 하나.
 * @return: qi_submit_sync() 의 결과. 0 이면 하드웨어가 완료를 알렸다.
 *
 * 왜 필요한가: 하드웨어는 재매핑 항목을 캐시에 담아 둔다. 메모리의 항목만
 * 고치면 하드웨어는 한동안 옛 목적지로 인터럽트를 계속 보낸다 — 인터럽트를
 * 다른 CPU 로 옮기는 순간 그 사이의 인터럽트가 사라지거나 엉뚱한 CPU 로 간다.
 * 그래서 항목을 고칠 때마다 반드시 이 무효화가 뒤따른다.
 *
 * SELECTIVE 를 쓰는 이유: 표 전체가 아니라 방금 고친 범위만 지워 다른
 * 인터럽트의 캐시 적중을 유지한다.
 *
 * 실행 컨텍스트: irq_2_ir_lock 을 쥔 채 호출된다. qi_submit_sync 는 완료를
 * 기다리므로, 이 락 안에서의 대기 시간이 곧 인터럽트 이동 비용이다.
 *
 * 호출 체인:
 *   modify_irte()/clear_entries() → [이 함수] → qi_submit_sync()
 */
static int qi_flush_iec(struct intel_iommu *iommu, int index, int mask)
{
	struct qi_desc desc;	/* [한국어] 큐에 넣을 무효화 서술자 */

	desc.qw0 = QI_IEC_IIDEX(index) | QI_IEC_TYPE | QI_IEC_IM(mask)	/* [한국어] 대상 인덱스 + IEC 타입 + 범위 mask + "전체가 아니라 선택 무효화" 비트 */
		   | QI_IEC_SELECTIVE;
	desc.qw1 = 0;	/* [한국어] IEC 무효화는 첫 워드만 쓴다 */
	desc.qw2 = 0;	/* [한국어] 스케일러블 모드용 확장 워드 — 여기서는 미사용 */
	desc.qw3 = 0;	/* [한국어] 같은 이유로 0 */

	return qi_submit_sync(iommu, &desc, 1, 0);	/* [한국어] 큐에 넣고 하드웨어가 완료를 표시할 때까지 기다린다. 반환 후에는 옛 항목이 확실히 사라졌다 */
}

/*
 * [한국어]
 * modify_irte - 재매핑 표의 항목 하나를 새 내용으로 갈아 끼우고 캐시를 지운다
 *
 * @irq_iommu: 어느 유닛의 몇 번 항목인지 아는 연결 구조체.
 * @irte_modified: 써 넣을 새 항목 내용(소프트웨어에서 조립해 온 128비트).
 * @return: 캐시 무효화 결과. 실패하면 음수.
 *
 * 이 함수가 이 파일의 심장이다. 인터럽트의 목적지·벡터·전달 방식이 바뀌는
 * 모든 경로가 결국 여기로 모인다.
 *
 * 원자성 문제: 항목은 128비트이고 하드웨어가 언제든 읽을 수 있다. 절반만
 * 갱신된 상태를 하드웨어가 보면 존재하지 않는 CPU 로 인터럽트를 보낸다.
 *  - 포스티드 항목(pst==1)이 관련되면 try_cmpxchg128() 로 128비트를 한 번에
 *    바꾼다. 이 경로는 KVM 과 하드웨어가 동시에 손댈 수 있어 원자성이 필수다.
 *  - 아니면 low/high 를 WRITE_ONCE 로 나눠 쓴다. 이 경우 present 비트가
 *    low 쪽에 있어, 순서상 위험한 중간 상태가 생기지 않는다.
 *
 * 쓴 뒤에는 __iommu_flush_cache 로 CPU 캐시를 메모리에 밀어내고(하드웨어가
 * 코히런트하지 않을 수 있다), qi_flush_iec 로 하드웨어의 항목 캐시를 지운다.
 * 두 단계를 모두 거쳐야 새 내용이 실제로 반영된다.
 *
 * 실행 컨텍스트: irq_2_ir_lock 을 잡고 인터럽트를 끈다. affinity 변경은
 * 인터럽트 문맥에서도 일어날 수 있어 raw 스핀락이 필요하다.
 *
 * 호출 체인:
 *   intel_ir_reconfigure_irte()/intel_ir_set_vcpu_affinity() → [이 함수]
 *     → try_cmpxchg128()/WRITE_ONCE → __iommu_flush_cache → qi_flush_iec
 */
static int modify_irte(struct irq_2_iommu *irq_iommu,
		       struct irte *irte_modified)
{
	struct intel_iommu *iommu;	/* [한국어] 대상 유닛 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */
	struct irte *irte;	/* [한국어] 하드웨어 표 안의 실제 항목 포인터 */
	int rc, index;	/* [한국어] 무효화 결과와 항목 인덱스 */

	if (!irq_iommu)	/* [한국어] 연결 정보가 없으면 고칠 항목을 특정할 수 없다 */
		return -1;	/* [한국어] 실패를 알린다 */

	raw_spin_lock_irqsave(&irq_2_ir_lock, flags);	/* [한국어] 표 수정과 캐시 무효화가 하나의 원자적 단위여야 한다 */

	iommu = irq_iommu->iommu;	/* [한국어] 할당 때 기록해 둔 담당 유닛 */

	index = irq_iommu->irte_index + irq_iommu->sub_handle;	/* [한국어] 묶음 시작 + 묶음 안 위치 = 실제 항목 번호 */
	irte = &iommu->ir_table->base[index];	/* [한국어] 그 유닛 표에서 항목의 주소 */

	if ((irte->pst == 1) || (irte_modified->pst == 1)) {	/* [한국어] 현재나 새 내용 중 하나라도 포스티드면 — KVM/하드웨어와 경쟁할 수 있어 원자적 교체가 필요하다 */
		/*
		 * We use cmpxchg16 to atomically update the 128-bit IRTE,
		 * and it cannot be updated by the hardware or other processors
		 * behind us, so the return value of cmpxchg16 should be the
		 * same as the old value.
		 */
		u128 old = irte->irte;	/* [한국어] 현재 128비트 값을 비교 기준으로 읽는다 */
		WARN_ON(!try_cmpxchg128(&irte->irte, &old, irte_modified->irte));	/* [한국어] 128비트를 한 번에 교체. 우리 말고는 아무도 바꾸지 않았어야 하므로 실패는 버그다 */
	} else {
		WRITE_ONCE(irte->low, irte_modified->low);	/* [한국어] present 비트가 든 하위 절반부터. 컴파일러가 쪼개거나 합치지 못하게 WRITE_ONCE */
		WRITE_ONCE(irte->high, irte_modified->high);	/* [한국어] 상위 절반(소스 검증 필드 등) */
	}
	__iommu_flush_cache(iommu, irte, sizeof(*irte));	/* [한국어] 하드웨어가 코히런트하지 않을 수 있어 CPU 캐시를 메모리로 밀어낸다 */

	rc = qi_flush_iec(iommu, index, 0);	/* [한국어] 하드웨어의 항목 캐시에서 옛 내용을 지운다. 이걸 빼면 한동안 옛 목적지로 인터럽트가 간다 */

	raw_spin_unlock_irqrestore(&irq_2_ir_lock, flags);	/* [한국어] 수정과 무효화가 모두 끝났다 */

	return rc;	/* [한국어] 무효화 결과가 곧 이 함수의 성패 */
}

/*
 * [한국어]
 * map_hpet_to_iommu - HPET id 로 그 인터럽트를 담당하는 유닛을 찾는다
 *
 * @hpet_id: ACPI 가 부여한 HPET 블록 번호.
 * @return: 담당 유닛, 없으면 NULL(그 HPET 은 재매핑 대상이 아니다).
 *
 * HPET 은 PCI 장치가 아니라 버스 열거로 발견되지 않는다. 그래서 DMAR 표
 * 파싱 때 ir_hpet[] 에 미리 담아 둔 목록을 선형 탐색한다. 항목 수가 몇 개
 * 되지 않아 자료구조를 더 얹을 이유가 없다.
 *
 * iommu 가 NULL 인 항목을 건너뛰는 이유: 배열은 고정 크기라 빈 칸이 있고,
 * 유닛이 핫플러그로 빠지면 이 필드만 지워 칸을 비운다.
 *
 * 호출 체인:
 *   intel_irq_remapping_select()/alloc 경로 → [이 함수]
 */
static struct intel_iommu *map_hpet_to_iommu(u8 hpet_id)
{
	int i;	/* [한국어] 배열 순회 인덱스 */

	for (i = 0; i < MAX_HPET_TBS; i++) {	/* [한국어] DMAR 이 알려 준 HPET 목록을 선형 탐색. 개수가 몇 개뿐이라 이걸로 충분하다 */
		if (ir_hpet[i].id == hpet_id && ir_hpet[i].iommu)	/* [한국어] id 가 맞고, 담당 유닛이 아직 살아 있는 칸인지 */
			return ir_hpet[i].iommu;	/* [한국어] 그 유닛이 이 HPET 의 인터럽트를 재매핑한다 */
	}
	return NULL;	/* [한국어] 목록에 없다 — 이 HPET 은 재매핑 대상이 아니다 */
}

/*
 * [한국어]
 * map_ioapic_to_iommu - IOAPIC 번호로 담당 유닛을 찾는다
 *
 * @apic: 커널이 부여한 IOAPIC 번호(ACPI MADT 의 id).
 * @return: 담당 유닛, 없으면 NULL.
 *
 * map_hpet_to_iommu 와 같은 구조다. IOAPIC 역시 PCI 장치가 아니어서 DMAR
 * 표의 device scope 가 알려 준 목록을 뒤진다. 레거시 인터럽트도 재매핑을
 * 거치게 하려면 이 대응이 반드시 있어야 한다.
 *
 * 호출 체인:
 *   intel_irq_remapping_select() → [이 함수]
 */
static struct intel_iommu *map_ioapic_to_iommu(int apic)
{
	int i;	/* [한국어] 배열 순회 인덱스 */

	for (i = 0; i < MAX_IO_APICS; i++) {	/* [한국어] DMAR 이 알려 준 IOAPIC 목록을 훑는다 */
		if (ir_ioapic[i].id == apic && ir_ioapic[i].iommu)	/* [한국어] APIC 번호가 맞고 담당 유닛이 유효한 칸 */
			return ir_ioapic[i].iommu;	/* [한국어] 찾은 유닛 */
	}
	return NULL;	/* [한국어] 재매핑 대상이 아닌 IOAPIC */
}

/*
 * [한국어]
 * map_dev_to_ir - PCI 장치를 담당하는 유닛의 인터럽트 도메인을 찾는다
 *
 * @dev: MSI 를 쓰려는 PCI 장치.
 * @return: 그 장치의 인터럽트가 통과할 재매핑 도메인, 없으면 NULL.
 *
 * IOAPIC/HPET 과 달리 PCI 장치는 DMA 쪽과 같은 규칙으로 유닛을 찾을 수 있다
 * — dmar_find_matched_drhd_unit() 이 세그먼트/버스 범위를 보고 담당 유닛을
 * 고른다. 그 유닛의 ir_domain 이 MSI 계층 구조에서 부모가 되어, 장치의 MSI
 * 가 항상 재매핑을 거치게 만든다.
 *
 * 호출 체인:
 *   dmar_msi 부모 도메인 선택 → [이 함수] → dmar_find_matched_drhd_unit()
 */
static struct irq_domain *map_dev_to_ir(struct pci_dev *dev)
{
	struct dmar_drhd_unit *drhd = dmar_find_matched_drhd_unit(dev);	/* [한국어] DMA 쪽과 같은 규칙(세그먼트/버스 범위)으로 담당 유닛을 고른다 */

	return drhd ? drhd->iommu->ir_domain : NULL;	/* [한국어] 그 유닛의 재매핑 도메인이 이 장치 MSI 의 부모가 된다 */
}

/*
 * [한국어]
 * clear_entries - 인터럽트가 쓰던 표 항목들을 지우고 비트맵에 돌려준다
 *
 * @irq_iommu: 해제할 인터럽트의 표 연결 정보.
 * @return: 캐시 무효화 결과. 이미 처리된 묶음이면 0.
 *
 * sub_handle 이 0 이 아니면 곧바로 돌아가는 이유: 다중 벡터 MSI 는 묶음
 * 하나를 통째로 잡았으므로, 해제도 묶음 단위로 한 번만 해야 한다. 묶음의
 * 첫 인터럽트(sub_handle==0)가 대표로 전부 처리하고 나머지는 그냥 넘어간다.
 * 이 검사가 없으면 같은 영역을 여러 번 반환해 비트맵이 망가진다.
 *
 * 지우는 순서가 중요하다: 먼저 항목을 0 으로 만들어 present 비트를 내리고,
 * 비트맵을 반환한 뒤, 마지막에 하드웨어 캐시를 무효화한다. 무효화가 끝나기
 * 전에는 하드웨어가 여전히 옛 항목을 캐시에 갖고 있을 수 있으므로, 이
 * 함수가 반환해야만 그 자리를 안전하게 재사용할 수 있다.
 *
 * 실행 컨텍스트: 인터럽트 해제 경로. 호출자가 irq_2_ir_lock 을 쥐고 있다.
 *
 * 호출 체인:
 *   intel_free_irq_remapping()/free_irte() 경로 → [이 함수] → qi_flush_iec()
 */
static int clear_entries(struct irq_2_iommu *irq_iommu)
{
	struct irte *start, *entry, *end;	/* [한국어] 지울 항목 범위를 훑을 포인터들 */
	struct intel_iommu *iommu;	/* [한국어] 대상 유닛 */
	int index;	/* [한국어] 묶음의 시작 인덱스 */

	if (irq_iommu->sub_handle)	/* [한국어] 묶음의 첫 인터럽트가 아니면 — 대표가 이미 전부 처리했다 */
		return 0;	/* [한국어] 중복 반환을 막는다. 이게 없으면 비트맵이 망가진다 */

	iommu = irq_iommu->iommu;	/* [한국어] 할당 때 기록해 둔 담당 유닛 */
	index = irq_iommu->irte_index;	/* [한국어] 묶음의 시작 */

	start = iommu->ir_table->base + index;	/* [한국어] 지울 첫 항목 */
	end = start + (1 << irq_iommu->irte_mask);	/* [한국어] 묶음 크기는 2^mask 개 */

	for (entry = start; entry < end; entry++) {	/* [한국어] 묶음 전체를 순회 */
		WRITE_ONCE(entry->low, 0);	/* [한국어] present 비트가 든 하위를 먼저 0 으로 — 이 시점부터 항목이 무효다 */
		WRITE_ONCE(entry->high, 0);	/* [한국어] 상위도 지워 옛 소스 검증 정보를 남기지 않는다 */
	}
	bitmap_release_region(iommu->ir_table->bitmap, index,	/* [한국어] 비트맵에 그 영역을 반환해 재사용 가능하게 한다 */
			      irq_iommu->irte_mask);

	return qi_flush_iec(iommu, index, irq_iommu->irte_mask);	/* [한국어] 묶음 전체의 하드웨어 캐시를 지운다. 이게 끝나야 자리를 안전하게 다시 쓸 수 있다 */
}

/*
 * source validation type
 */
#define SVT_NO_VERIFY		0x0  /* no verification is required */	/* [한국어] 검증 없음 — 아무 장치나 이 항목을 쓸 수 있다. 소스 id 를 모를 때의 마지막 수단 */
#define SVT_VERIFY_SID_SQ	0x1  /* verify using SID and SQ fields */	/* [한국어] SID/SQ 필드로 요청자를 검사한다. 가장 강한 검증 */
#define SVT_VERIFY_BUS		0x2  /* verify bus of request-id */	/* [한국어] requester-id 의 버스 번호만 검사. 브리지 뒤 장치처럼 devfn 을 특정할 수 없을 때 */

/*
 * source-id qualifier
 */
#define SQ_ALL_16	0x0  /* verify all 16 bits of request-id */	/* [한국어] 16비트 전부 비교 — 장치 하나만 통과한다 */
/* [한국어] 상위 13비트만 비교하고 세 번째 하위 비트를 무시한다.
 * 그 비트만 다른 함수 번호들을 같은 출처로 인정한다는 뜻. */
#define SQ_13_IGNORE_1	0x1  /* verify most significant 13 bits, ignore
			      * the third least significant bit
			      */
/* [한국어] 두 번째와 세 번째 하위 비트를 무시한다. 허용 범위가 네 배로 넓어진다. */
#define SQ_13_IGNORE_2	0x2  /* verify most significant 13 bits, ignore
			      * the second and third least significant bits
			      */
/* [한국어] 하위 3비트를 모두 무시 — 한 장치의 여덟 함수를 전부 같은 출처로 본다.
 * HPET 의 소스 id 를 잘못 보고하는 플랫폼 때문에 set_hpet_sid() 가 이걸 쓴다. */
#define SQ_13_IGNORE_3	0x3  /* verify most significant 13 bits, ignore
			      * the least three significant bits
			      */

/*
 * set SVT, SQ and SID fields of irte to verify
 * source ids of interrupt requests
 */
/*
 * [한국어]
 * set_irte_sid - 항목에 "이 인터럽트를 낼 자격이 있는 출처"를 새긴다
 *
 * @irte: 채울 재매핑 표 항목.
 * @svt: 검증 방식(SVT_NO_VERIFY / SID+SQ / 버스 범위).
 * @sq: SID 를 몇 비트까지 비교할지의 규칙.
 * @sid: 기대하는 소스 id(버스<<8 | devfn).
 *
 * 이것이 인터럽트 재매핑의 보안 핵심이다. 재매핑이 없던 시절 MSI 는 "임의의
 * 주소에 임의의 값을 쓰는 DMA"와 구별되지 않아서, 손상된 장치가 다른 장치의
 * 인터럽트를 위조하거나 존재하지 않는 벡터를 쏘아 커널을 무너뜨릴 수 있었다.
 * 항목마다 출처를 못박아 두면 하드웨어가 "이 항목을 쓸 수 있는 장치"를
 * 검사하므로, 위조된 인터럽트가 표 단계에서 걸러진다.
 *
 * disable_sourceid_checking 은 이 검사를 끄는 부팅 옵션이다. 소스 id 를 잘못
 * 보고하는 고장난 플랫폼에서 부팅 자체가 막히는 것을 피하기 위한 탈출구이며,
 * 켜면 위의 보호가 사라진다.
 *
 * 호출 체인:
 *   set_ioapic_sid()/set_hpet_sid()/set_msi_sid()/set_irte_verify_bus() → [이 함수]
 */
static void set_irte_sid(struct irte *irte, unsigned int svt,
			 unsigned int sq, unsigned int sid)
{
	if (disable_sourceid_checking)	/* [한국어] 부팅 옵션으로 검증을 껐는가 — 소스 id 를 잘못 보고하는 플랫폼용 탈출구 */
		svt = SVT_NO_VERIFY;	/* [한국어] 검증 없음으로 강등한다. 보호가 사라지는 대신 부팅은 된다 */
	irte->svt = svt;	/* [한국어] 검증 방식 */
	irte->sq = sq;	/* [한국어] SID 를 몇 비트까지 비교할지 */
	irte->sid = sid;	/* [한국어] 기대하는 소스 id */
}

/*
 * Set an IRTE to match only the bus number. Interrupt requests that reference
 * this IRTE must have a requester-id whose bus number is between or equal
 * to the start_bus and end_bus arguments.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * set_irte_verify_bus - 소스 검증을 "버스 번호 범위"로 느슨하게 건다
 *
 * @irte: 채울 항목.
 * @start_bus: 허용 버스 범위의 시작.
 * @end_bus: 허용 버스 범위의 끝.
 *
 * 왜 정확한 devfn 이 아니라 범위인가: PCIe-to-PCI 브리지 뒤의 레거시 장치는
 * 브리지가 자기 이름으로 요청을 대신 낸다. NTB 같은 하드웨어는 같은 버스의
 * 여러 devfn 으로 트래픽을 만든다. 두 경우 모두 "이 인터럽트를 낼 장치"를
 * 하나로 특정할 수 없어, 버스 범위까지만 좁히는 것이 가능한 최선이다.
 *
 * 정확도는 떨어지지만 여전히 다른 버스의 장치가 이 항목을 훔쳐 쓰는 것은
 * 막는다 — 검증을 아예 끄는 것과는 다르다.
 *
 * 호출 체인:
 *   set_msi_sid() → [이 함수] → set_irte_sid()
 */
static void set_irte_verify_bus(struct irte *irte, unsigned int start_bus,
				unsigned int end_bus)
{
	set_irte_sid(irte, SVT_VERIFY_BUS, SQ_ALL_16,	/* [한국어] 버스 범위 검증. sid 필드에 시작 버스와 끝 버스를 상·하위 바이트로 실어 보낸다 */
		     (start_bus << 8) | end_bus);
}

/*
 * [한국어]
 * set_ioapic_sid - IOAPIC 인터럽트 항목에 그 IOAPIC 의 소스 id 를 새긴다
 *
 * @irte: 채울 항목.
 * @apic: IOAPIC 번호.
 * @return: 0 성공, -1 이면 그 IOAPIC 의 소스 id 를 모른다.
 *
 * IOAPIC 은 PCI 장치가 아니라 스스로 requester-id 를 갖지 않는다. 대신
 * 플랫폼이 DMAR 표에 "이 IOAPIC 의 인터럽트는 이런 소스 id 로 나타난다"고
 * 적어 두고, 부팅 때 그것을 ir_ioapic[] 에 담아 둔다. 여기서 그 값을 꺼내
 * 항목에 새긴다.
 *
 * 소스 id 를 찾지 못하면 -1 을 돌려 인터럽트 할당을 실패시킨다. 검증 없이
 * 항목을 열어 두느니 그 인터럽트를 포기하는 쪽이 안전하기 때문이다.
 *
 * SQ_ALL_16 을 쓰는 이유: IOAPIC 의 소스 id 는 정확히 하나라 16비트 전부를
 * 비교해도 문제가 없다.
 *
 * 호출 체인:
 *   intel_irq_remapping_alloc() → [이 함수] → set_irte_sid()
 */
static int set_ioapic_sid(struct irte *irte, int apic)
{
	int i;	/* [한국어] 목록 순회 인덱스 */
	u16 sid = 0;	/* [한국어] 찾은 소스 id. 0 은 "못 찾음"의 표식으로 쓴다 */

	if (!irte)	/* [한국어] 채울 항목이 없으면 할 일이 없다 */
		return -1;	/* [한국어] 실패 */

	for (i = 0; i < MAX_IO_APICS; i++) {	/* [한국어] DMAR 이 알려 준 IOAPIC 목록을 훑는다 */
		if (ir_ioapic[i].iommu && ir_ioapic[i].id == apic) {	/* [한국어] 유효한 칸이면서 번호가 맞는가 */
			sid = PCI_DEVID(ir_ioapic[i].bus, ir_ioapic[i].devfn);	/* [한국어] 버스와 devfn 을 16비트 소스 id 로 합친다 */
			break;	/* [한국어] 찾았으면 더 볼 필요 없다 */
		}
	}

	if (sid == 0) {	/* [한국어] 목록에 없다 — 이 IOAPIC 의 인터럽트를 검증할 방법이 없다 */
		pr_warn("Failed to set source-id of IOAPIC (%d)\n", apic);	/* [한국어] 플랫폼 표가 불완전하다는 뜻이라 알린다 */
		return -1;	/* [한국어] 검증 없이 항목을 여느니 인터럽트 할당을 실패시킨다 */
	}

	set_irte_sid(irte, SVT_VERIFY_SID_SQ, SQ_ALL_16, sid);	/* [한국어] IOAPIC 은 소스 id 가 하나뿐이라 16비트 전부를 비교한다 */

	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * set_hpet_sid - HPET 인터럽트 항목에 그 HPET 의 소스 id 를 새긴다
 *
 * @irte: 채울 항목.
 * @id: HPET 블록 번호.
 * @return: 0 성공, -1 이면 소스 id 미상.
 *
 * 구조는 set_ioapic_sid 와 같지만 검증 강도가 다르다. 원 주석이 밝히듯
 * 원래는 SQ_ALL_16(16비트 전부 비교)을 써야 하는데, 일부 플랫폼이 HPET 의
 * 소스 id 를 스펙과 다르게 보고한다. 그런 기계에서 타이머 인터럽트가 막히면
 * 시스템이 아예 뜨지 못하므로, 하위 3비트를 무시하는 SQ_13_IGNORE_3 으로
 * 느슨하게 검증한다 — 정확도를 조금 내주고 부팅 가능성을 산 타협이다.
 *
 * 호출 체인:
 *   intel_irq_remapping_alloc() → [이 함수] → set_irte_sid()
 */
static int set_hpet_sid(struct irte *irte, u8 id)
{
	int i;	/* [한국어] 목록 순회 인덱스 */
	u16 sid = 0;	/* [한국어] 찾은 소스 id */

	if (!irte)	/* [한국어] 채울 항목이 없다 */
		return -1;	/* [한국어] 실패 */

	for (i = 0; i < MAX_HPET_TBS; i++) {	/* [한국어] DMAR 이 알려 준 HPET 목록을 훑는다 */
		if (ir_hpet[i].iommu && ir_hpet[i].id == id) {	/* [한국어] 유효한 칸이면서 블록 번호가 맞는가 */
			sid = PCI_DEVID(ir_hpet[i].bus, ir_hpet[i].devfn);	/* [한국어] 소스 id 로 합친다 */
			break;	/* [한국어] 찾았다 */
		}
	}

	if (sid == 0) {	/* [한국어] 표에 없는 HPET */
		pr_warn("Failed to set source-id of HPET block (%d)\n", id);	/* [한국어] 알리고 */
		return -1;	/* [한국어] 인터럽트 할당을 실패시킨다 */
	}

	/*
	 * Should really use SQ_ALL_16. Some platforms are broken.
	 * While we figure out the right quirks for these broken platforms, use
	 * SQ_13_IGNORE_3 for now.
	 */
	set_irte_sid(irte, SVT_VERIFY_SID_SQ, SQ_13_IGNORE_3, sid);	/* [한국어] 하위 3비트를 무시하는 느슨한 검증. 원 주석대로 고장난 플랫폼을 위한 임시 타협이다 */

	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어] struct set_msi_sid_data — DMA 별칭 순회의 결과를 모으는 통계
 *
 * pci_for_each_dma_alias() 는 콜백 방식이라 판단을 한 번에 내릴 수 없다.
 * 그래서 콜백은 세기만 하고, 순회가 끝난 뒤 set_msi_sid() 가 이 네 값을
 * 보고 "이 장치의 MSI 를 어떤 규칙으로 검증할지"를 결정한다.
 */
struct set_msi_sid_data {
	struct pci_dev *pdev;
	/* [한국어] 별칭 순회에서 마지막으로 본 장치.
	 * 설정자: set_msi_sid_cb() 가 별칭마다 덮어쓴다.
	 * 읽는 자: set_msi_sid() 가 "별칭 장치의 버스가 원 장치와 다른가"를 볼 때.
	 * 왜 마지막 값만 남겨도 되는가: 순회는 위상을 따라 위로 올라가므로 마지막이
	 *   가장 상위(실제로 요청을 내는 쪽)이고, 그것이 판단에 필요한 값이다. */
	u16 alias;
	/* [한국어] 마지막으로 본 별칭 requester-id.
	 * 설정자: 콜백. 읽는 자: set_msi_sid() 의 1)·3) 분기.
	 * 값 범위: 버스<<8 | devfn 형식의 16비트. */
	int count;
	/* [한국어] 열거된 별칭의 총 개수.
	 * 설정자: 콜백이 하나씩 증가. 읽는 자: 2) 분기가 "별칭이 여럿인가"를 판단.
	 * 값 범위: 최소 1(자기 자신). */
	int busmatch_count;
	/* [한국어] 그중 첫 별칭과 같은 버스에 있던 별칭의 수.
	 * 설정자: 콜백. 읽는 자: count 와 같으면 "전부 한 버스"라는 뜻이 되어
	 *   NTB 형 하드웨어로 판정하고 버스 단위 검증을 건다.
	 * 왜 세는가: 별칭이 여럿이어도 흩어져 있으면 버스 검증이 지나치게 넓어진다.
	 *   한 버스에 모여 있을 때만 그 완화가 정당하다. */
};

/*
 * [한국어]
 * set_msi_sid_cb - DMA 별칭을 하나씩 받아 통계를 모으는 콜백
 *
 * @pdev: 이 별칭을 내는 장치.
 * @alias: 그 별칭 requester-id.
 * @opaque: 결과를 모을 set_msi_sid_data.
 * @return: 항상 0 — 순회를 끝까지 계속하라는 뜻.
 *
 * pci_for_each_dma_alias() 는 "이 장치의 트래픽이 어떤 이름으로 나타날 수
 * 있는가"를 위상 구조를 따라 올라가며 열거한다. 이 콜백은 판단을 하지 않고
 * 개수(count)와 "같은 버스에 머문 별칭 수"(busmatch_count)만 센다. 판단은
 * 순회가 끝난 뒤 set_msi_sid() 가 그 통계를 보고 한다.
 *
 * count==0 조건이 앞에 오는 이유: 첫 별칭은 비교 대상이 아직 없으므로
 * 무조건 "같은 버스"로 세어야 이후 비교의 기준이 생긴다.
 *
 * 호출 체인:
 *   set_msi_sid() → pci_for_each_dma_alias() → [이 함수]
 */
static int set_msi_sid_cb(struct pci_dev *pdev, u16 alias, void *opaque)
{
	struct set_msi_sid_data *data = opaque;	/* [한국어] 순회 결과를 모으는 통계 구조체 */

	if (data->count == 0 || PCI_BUS_NUM(alias) == PCI_BUS_NUM(data->alias))	/* [한국어] 첫 별칭이거나, 직전 별칭과 같은 버스인가 */
		data->busmatch_count++;	/* [한국어] "한 버스에 모여 있다"의 증거를 하나 더 센다 */

	data->pdev = pdev;	/* [한국어] 마지막으로 본 장치 — 순회가 끝나면 가장 상위 요청자다 */
	data->alias = alias;	/* [한국어] 마지막 별칭 id */
	data->count++;	/* [한국어] 총 별칭 수 */

	return 0;	/* [한국어] 0 을 돌려 순회를 끝까지 계속한다 */
}

/*
 * [한국어]
 * set_msi_sid - PCI 장치의 MSI 항목에 알맞은 소스 검증 규칙을 고른다
 *
 * @irte: 채울 항목.
 * @dev: MSI 를 쓰는 장치.
 * @return: 0 성공, -1 이면 인자가 잘못됐다.
 *
 * 이 함수의 어려움은 "MSI 를 낼 때 이 장치가 어떤 이름으로 보이는가"가
 * 위상 구조에 따라 달라진다는 데 있다. 그래서 먼저 DMA 별칭을 모두 열거해
 * 통계를 낸 뒤, 원 주석이 설명하는 네 가지 상황으로 나눈다.
 *
 *  1) 별칭의 버스가 장치의 버스와 다르다 → PCIe-to-PCI 브리지 뒤다. 브리지가
 *     자기 이름으로 대신 요청하므로 정확한 devfn 을 알 수 없어, 브리지의
 *     하위 버스부터 장치의 버스까지를 허용 범위로 잡는다.
 *  2) 별칭이 여럿인데 전부 같은 버스 → NTB 처럼 한 버스의 여러 devfn 으로
 *     트래픽을 내는 하드웨어다. 역시 버스 단위 검증이 최선.
 *  3) 별칭 장치의 버스가 다르다 → 위상에서 비롯된 진짜 별칭이므로 그 별칭
 *     id 로 정확히 검증한다.
 *  4) 그 밖 → 별칭이 DMA 용 quirk 일 뿐이라 MSI 도 같은 id 를 쓴다는 보장이
 *     없다. 그래서 별칭을 무시하고 장치 자신의 id 로 검증한다.
 *
 * 판단이 틀리면 두 방향의 대가가 다르다. 너무 좁게 잡으면 정상 인터럽트가
 * 하드웨어에 막혀 장치가 멈추고, 너무 넓게 잡으면 이웃 장치가 이 항목을
 * 쓸 수 있게 된다. 그래서 확신이 없는 경우는 모두 "버스까지만" 쪽으로
 * 기울여 동작을 우선한다.
 *
 * 호출 체인:
 *   intel_irq_remapping_alloc() → [이 함수] → pci_for_each_dma_alias()
 *     → set_irte_verify_bus()/set_irte_sid()
 */
static int set_msi_sid(struct irte *irte, struct pci_dev *dev)
{
	struct set_msi_sid_data data;	/* [한국어] 별칭 순회 통계 */

	if (!irte || !dev)	/* [한국어] 채울 항목이나 대상 장치가 없다 */
		return -1;	/* [한국어] 실패 */

	data.count = 0;	/* [한국어] 통계 초기화. count 0 은 콜백에서 "첫 별칭" 판정에도 쓰인다 */
	data.busmatch_count = 0;	/* [한국어] 같은 버스 별칭 수 초기화 */
	pci_for_each_dma_alias(dev, set_msi_sid_cb, &data);	/* [한국어] 이 장치의 트래픽이 나타날 수 있는 모든 이름을 위상을 따라 열거한다 */

	/*
	 * DMA alias provides us with a PCI device and alias.  The only case
	 * where the it will return an alias on a different bus than the
	 * device is the case of a PCIe-to-PCI bridge, where the alias is for
	 * the subordinate bus.  In this case we can only verify the bus.
	 *
	 * If there are multiple aliases, all with the same bus number,
	 * then all we can do is verify the bus. This is typical in NTB
	 * hardware which use proxy IDs where the device will generate traffic
	 * from multiple devfn numbers on the same bus.
	 *
	 * If the alias device is on a different bus than our source device
	 * then we have a topology based alias, use it.
	 *
	 * Otherwise, the alias is for a device DMA quirk and we cannot
	 * assume that MSI uses the same requester ID.  Therefore use the
	 * original device.
	 */
	if (PCI_BUS_NUM(data.alias) != data.pdev->bus->number)	/* [한국어] 1) 별칭의 버스가 그 별칭을 낸 장치의 버스와 다르다 → PCIe-to-PCI 브리지 뒤 */
		set_irte_verify_bus(irte, PCI_BUS_NUM(data.alias),	/* [한국어] 브리지 하위 버스부터 장치 버스까지를 허용 범위로 잡는다 */
				    dev->bus->number);
	else if (data.count >= 2 && data.busmatch_count == data.count)	/* [한국어] 2) 별칭이 여럿인데 전부 같은 버스 → NTB 형 하드웨어 */
		set_irte_verify_bus(irte, dev->bus->number, dev->bus->number);	/* [한국어] 그 한 버스만 허용 */
	else if (data.pdev->bus->number != dev->bus->number)	/* [한국어] 3) 별칭 장치가 다른 버스에 있다 → 위상에서 온 진짜 별칭 */
		set_irte_sid(irte, SVT_VERIFY_SID_SQ, SQ_ALL_16, data.alias);	/* [한국어] 그 별칭 id 로 정확히 검증 */
	else
		set_irte_sid(irte, SVT_VERIFY_SID_SQ, SQ_ALL_16,	/* [한국어] 4) 별칭이 DMA quirk 일 뿐 — MSI 는 장치 자신의 id 로 나온다고 보고 그것으로 검증 */
			     pci_dev_id(dev));

	return 0;	/* [한국어] 성공. 어느 분기든 항목에는 검증 규칙이 새겨졌다 */
}

/*
 * [한국어]
 * iommu_load_old_irte - 크래시한 이전 커널의 재매핑 표를 물려받는다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공. -EINVAL 이면 표 크기가 달라 물려받을 수 없고,
 *          -ENOMEM 이면 옛 표를 매핑하지 못했다.
 *
 * kdump 전용 경로다. 첫 커널이 죽어도 장치들은 계속 살아 있고, 그들이 쓰던
 * 인터럽트는 여전히 옛 표의 항목을 가리킨다. 새 커널이 빈 표로 갈아타는
 * 순간 그 인터럽트들은 present 가 아닌 항목을 만나 하드웨어 폴트가 되고,
 * 장치는 멈추거나 폴트 폭풍이 덤프를 방해한다. 그래서 옛 내용을 그대로
 * 복사해 두고 시작한다.
 *
 * 단계:
 *  1) IRTA 레지스터를 읽어 옛 표의 물리 주소와 크기를 알아낸다. 크기가
 *     우리 표와 다르면 인덱스가 어긋나므로 포기한다.
 *  2) memremap 으로 옛 표를 잠깐 매핑해 통째로 복사한다.
 *  3) 복사된 항목 중 present 인 것을 비트맵에 "사용 중"으로 표시한다 —
 *     이렇게 해야 새 커널의 alloc_irte 가 살아 있는 인터럽트의 자리를
 *     다시 나눠 주지 않는다.
 *
 * 왜 비트맵을 따로 세우는가: 표 내용은 복사했지만 할당 상태는 소프트웨어
 * 자료구조라 복사 대상이 아니다. 표를 훑어 재구성하는 것이 유일한 방법이다.
 *
 * 실행 컨텍스트: 유닛 초기화, 단일 스레드. 아직 표를 하드웨어에 걸기 전이다.
 *
 * 호출 체인:
 *   intel_setup_irq_remapping() → [이 함수] → memremap()/bitmap_set()
 */
static int iommu_load_old_irte(struct intel_iommu *iommu)
{
	struct irte *old_ir_table;	/* [한국어] 옛 표를 임시로 매핑해 볼 포인터 */
	phys_addr_t irt_phys;	/* [한국어] 옛 표의 물리 주소. IRTA 레지스터가 알려 준다 */
	unsigned int i;	/* [한국어] 항목 순회 인덱스 */
	size_t size;	/* [한국어] 표 전체 크기 */
	u64 irta;	/* [한국어] IRTA 레지스터 원본 값(주소 + 크기 + 모드 비트) */

	/* Check whether the old ir-table has the same size as ours */
	irta = readq(iommu->reg + DMAR_IRTA_REG);	/* [한국어] 하드웨어가 아직 들고 있는 옛 표의 위치를 읽는다 */
	if ((irta & INTR_REMAP_TABLE_REG_SIZE_MASK)	/* [한국어] 옛 표의 크기가 우리 표와 같은가 */
	     != INTR_REMAP_TABLE_REG_SIZE)
		return -EINVAL;	/* [한국어] 다르면 인덱스가 어긋나 복사해도 의미가 없다 */

	irt_phys = irta & VTD_PAGE_MASK;	/* [한국어] 하위 비트의 크기·모드 필드를 떼고 순수 물리 주소만 */
	size     = INTR_REMAP_TABLE_ENTRIES*sizeof(struct irte);	/* [한국어] 65536개 × 16바이트 = 1MB */

	/* Map the old IR table */
	old_ir_table = memremap(irt_phys, size, MEMREMAP_WB);	/* [한국어] 옛 커널이 남긴 물리 메모리를 캐시 가능하게 잠깐 매핑 */
	if (!old_ir_table)	/* [한국어] 매핑 실패 — 그 영역을 읽을 수 없다 */
		return -ENOMEM;	/* [한국어] 물려받기를 포기한다. 호출자는 경고만 내고 계속 간다 */

	/* Copy data over */
	memcpy(iommu->ir_table->base, old_ir_table, size);	/* [한국어] 옛 항목을 새 표에 통째로 복사. 살아 있는 장치의 인터럽트가 그대로 유효해진다 */

	__iommu_flush_cache(iommu, iommu->ir_table->base, size);	/* [한국어] 복사한 내용을 메모리까지 밀어낸다 — 하드웨어가 코히런트하지 않을 수 있다 */

	/*
	 * Now check the table for used entries and mark those as
	 * allocated in the bitmap
	 */
	for (i = 0; i < INTR_REMAP_TABLE_ENTRIES; i++) {	/* [한국어] 복사된 표를 훑어 할당 상태를 재구성한다 */
		if (iommu->ir_table->base[i].present)	/* [한국어] present 면 아직 살아 있는 인터럽트가 쓰는 자리다 */
			bitmap_set(iommu->ir_table->bitmap, i, 1);	/* [한국어] 비트맵에 사용 중으로 표시 — alloc_irte 가 이 자리를 다시 주지 못하게 */
	}

	memunmap(old_ir_table);	/* [한국어] 임시 매핑 해제. 복사가 끝나 더 필요 없다 */

	return 0;	/* [한국어] 물려받기 성공 */
}


/*
 * [한국어]
 * iommu_set_irq_remapping - 새 재매핑 표의 주소를 하드웨어에 알린다
 *
 * @iommu: 대상 유닛.
 * @mode: x2APIC(확장 인터럽트 모드)로 쓸 것인지.
 *
 * 표를 "설정"하는 것과 재매핑을 "켜는" 것은 별개의 단계다. 이 함수는 앞의
 * 절반 — IRTA 레지스터에 표의 물리 주소·크기·EIM 비트를 쓰고, GCMD 의
 * SIRTP(Set Interrupt Remap Table Pointer)로 그 값을 반영하라고 명령한다.
 * 실제로 인터럽트가 표를 거치기 시작하는 것은 iommu_enable_irq_remapping()
 * 이 IRE 비트를 세운 뒤다.
 *
 * gcmd 를 함께 쓰는 이유: GCMD 는 쓰기 전용이라 읽어도 현재 설정을 알 수
 * 없다. 그래서 드라이버가 자기가 세운 비트들을 iommu->gcmd 에 거울처럼
 * 보관하고, 새 명령은 항상 "그 사본 | 이번 비트"로 써야 다른 기능이 꺼지지
 * 않는다.
 *
 * IOMMU_WAIT_OP 로 GSTS 의 IRTPS 를 기다리는 이유: 하드웨어가 포인터를
 * 실제로 받아들이기 전에 다음 단계로 가면 옛 표가 쓰인다.
 *
 * 마지막 무효화: 하드웨어가 옛 표의 항목을 캐시에 갖고 있을 수 있으므로
 * 전역 IEC 무효화로 지운다. cap_esirtps 가 있으면 SIRTP 자체가 캐시를
 * 비우도록 보장되어 이 단계를 건너뛴다.
 *
 * 호출 체인:
 *   intel_setup_irq_remapping()/reenable_irq_remapping() → [이 함수]
 */
static void iommu_set_irq_remapping(struct intel_iommu *iommu, int mode)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */
	u64 addr;	/* [한국어] 표의 물리 주소 */
	u32 sts;	/* [한국어] IOMMU_WAIT_OP 이 GSTS 를 담아 볼 변수 */

	addr = virt_to_phys((void *)iommu->ir_table->base);	/* [한국어] 하드웨어는 물리 주소로만 표를 찾는다 */

	raw_spin_lock_irqsave(&iommu->register_lock, flags);	/* [한국어] 레지스터 조작과 gcmd 사본 갱신을 원자적으로 */

	writeq((addr) | IR_X2APIC_MODE(mode) | INTR_REMAP_TABLE_REG_SIZE,	/* [한국어] 주소 + EIM 비트 + 크기 필드를 한 워드에 담아 IRTA 에 쓴다 */
	       iommu->reg + DMAR_IRTA_REG);

	/* Set interrupt-remapping table pointer */
	writel(iommu->gcmd | DMA_GCMD_SIRTP, iommu->reg + DMAR_GCMD_REG);	/* [한국어] SIRTP 로 "방금 쓴 포인터를 반영하라"고 명령. gcmd 사본과 OR 해야 다른 기능이 꺼지지 않는다 */

	IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG,	/* [한국어] IRTPS 가 설 때까지 기다린다 — 하드웨어가 포인터를 실제로 받아들였다는 신호 */
		      readl, (sts & DMA_GSTS_IRTPS), sts);
	raw_spin_unlock_irqrestore(&iommu->register_lock, flags);	/* [한국어] 레지스터 조작 끝 */

	/*
	 * Global invalidation of interrupt entry cache to make sure the
	 * hardware uses the new irq remapping table.
	 */
	if (!cap_esirtps(iommu->cap))	/* [한국어] 하드웨어가 SIRTP 시 캐시를 스스로 비우지 않는다면 */
		qi_global_iec(iommu);	/* [한국어] 옛 표의 항목 캐시를 전부 지운다 */
}

/*
 * [한국어]
 * iommu_enable_irq_remapping - 재매핑을 켜고 레거시 형식 MSI 를 막는다
 *
 * @iommu: 대상 유닛.
 *
 * IRE 비트를 세우는 순간부터 이 유닛 아래 모든 인터럽트가 표를 거친다.
 * 그런데 그것만으로는 보호가 완성되지 않는다 — "호환 형식(compatibility
 * format)" MSI 는 재매핑을 우회해 CPU 로 직행하기 때문이다. 그 경로가
 * 열려 있으면 손상된 장치가 임의의 벡터를 CPU 에 직접 주입할 수 있어,
 * 재매핑을 켠 의미가 사라진다.
 *
 * 그래서 IRE 를 켠 직후 CFI(Compatibility Format Interrupt) 비트를 내려
 * 그 우회로를 닫고, GSTS 의 CFIS 로 실제로 닫혔는지 확인한다. 닫히지 않은
 * 채 진행되면 WARN 으로 "당신은 IRQ 주입에 취약하다"고 크게 경고한다 —
 * 조용히 넘어가면 보안 가정이 무너진 줄도 모르게 되기 때문이다.
 *
 * 실행 컨텍스트: register_lock 을 쥐고 인터럽트를 끈 채. 레지스터 쓰기와
 * 상태 확인이 다른 경로와 뒤섞이면 gcmd 사본이 어긋난다.
 *
 * 호출 체인:
 *   intel_enable_irq_remapping()/reenable_irq_remapping() → [이 함수]
 */
static void iommu_enable_irq_remapping(struct intel_iommu *iommu)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */
	u32 sts;	/* [한국어] GSTS 값 */

	raw_spin_lock_irqsave(&iommu->register_lock, flags);	/* [한국어] 레지스터와 gcmd 사본 보호 */

	/* Enable interrupt-remapping */
	iommu->gcmd |= DMA_GCMD_IRE;	/* [한국어] 사본에 먼저 IRE 를 세운다 — 이후 모든 GCMD 쓰기가 이 값을 기준으로 한다 */
	writel(iommu->gcmd, iommu->reg + DMAR_GCMD_REG);	/* [한국어] 실제로 재매핑을 켠다. 이 순간부터 인터럽트가 표를 거친다 */
	IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG,	/* [한국어] IRES 가 설 때까지 기다린다 */
		      readl, (sts & DMA_GSTS_IRES), sts);

	/* Block compatibility-format MSIs */
	if (sts & DMA_GSTS_CFIS) {	/* [한국어] 호환 형식 MSI 가 아직 허용되고 있는가 — 재매핑을 우회하는 구멍이다 */
		iommu->gcmd &= ~DMA_GCMD_CFI;	/* [한국어] 사본에서 CFI 를 내린다 */
		writel(iommu->gcmd, iommu->reg + DMAR_GCMD_REG);	/* [한국어] 호환 형식 MSI 우회로를 실제로 닫는다 */
		IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG,	/* [한국어] CFIS 가 꺼질 때까지 확인 */
			      readl, !(sts & DMA_GSTS_CFIS), sts);
	}

	/*
	 * With CFI clear in the Global Command register, we should be
	 * protected from dangerous (i.e. compatibility) interrupts
	 * regardless of x2apic status.  Check just to be sure.
	 */
	if (sts & DMA_GSTS_CFIS)	/* [한국어] 그래도 꺼지지 않았다면 하드웨어가 요청을 무시한 것 */
		WARN(1, KERN_WARNING	/* [한국어] 조용히 넘어가면 보안 가정이 무너진 줄 모르게 되므로 크게 경고한다 */
			"Compatibility-format IRQs enabled despite intr remapping;\n"
			"you are vulnerable to IRQ injection.\n");

	raw_spin_unlock_irqrestore(&iommu->register_lock, flags);	/* [한국어] 설정 완료 */
}

/*
 * [한국어]
 * intel_setup_irq_remapping - 유닛 하나에 재매핑 표와 인터럽트 도메인을 갖춘다
 *
 * @iommu: 준비할 VT-d 유닛.
 * @return: 0 성공, -ENOMEM 이면 어느 단계에서든 자원을 얻지 못했다.
 *
 * 이 파일에서 가장 많은 것을 조립하는 함수다. 만드는 것은 네 가지다.
 *  1) ir_table — 표 자체(1MB, 65536개 항목)와 할당 비트맵.
 *     1MB 인 이유: 항목이 16바이트이고 인덱스가 16비트라 최대 크기가 그렇다.
 *  2) fwnode + irq_domain — 커널 인터럽트 계층에서 이 유닛을 대표하는 노드.
 *     IRQ_DOMAIN_FLAG_ISOLATED_MSI 는 "이 도메인 아래 MSI 는 서로 격리된다"는
 *     선언이라, VFIO 같은 사용자가 장치를 안전하게 넘길 수 있는 근거가 된다.
 *  3) 큐 무효화(QI) — 항목을 고칠 때마다 캐시를 지워야 하는데 그 수단이 QI 다.
 *     이미 켜져 있으면 건드리지 않고, 아니면 옛 폴트를 치우고 새로 켠다.
 *  4) kdump 처리 — 물려받은 재매핑이 있으면 상황에 따라 표를 복사하거나
 *     (진짜 kdump) 아예 꺼 버린다(kdump 가 아닌데 켜져 있는 이상 상황).
 *
 * 에러 경로가 계단식 goto 인 이유: 각 단계가 앞 단계의 자원 위에 쌓이므로,
 * 실패한 지점부터 거꾸로 정확히 그만큼만 되돌려야 한다. 마지막에
 * iommu->ir_table 을 NULL 로 되돌려 "준비 안 됨" 상태를 분명히 남긴다.
 *
 * 맨 앞의 ir_table 검사: 핫플러그나 재초기화로 두 번 불릴 수 있어, 이미
 * 준비되어 있으면 아무것도 하지 않고 성공을 돌려준다.
 *
 * 실행 컨텍스트: 부팅 또는 유닛 핫플러그. 프로세스 문맥이라 GFP_KERNEL 가능.
 *
 * 호출 체인:
 *   intel_prepare_irq_remapping()/dmar_ir_add() → [이 함수]
 *     → iommu_alloc_pages_node_sz() → msi_create_parent_irq_domain()
 *     → dmar_enable_qi() → iommu_load_old_irte() → iommu_set_irq_remapping()
 */
static int intel_setup_irq_remapping(struct intel_iommu *iommu)
{
/*
 * [한국어] 이 유닛의 인터럽트 도메인을 만들 때 커널에 넘길 설명서.
 * 아래 msi_create_parent_irq_domain() 이 이 값들로 도메인을 구성한다.
 */
	struct irq_domain_info info = {
		.ops		= &intel_ir_domain_ops,	/* [한국어] 이 도메인의 alloc/free/activate 콜백 표 */
		.parent		= arch_get_ir_parent_domain(),	/* [한국어] 상위 도메인(실제 CPU 벡터를 나눠 주는 쪽). 재매핑 도메인은 그 위에 한 겹 얹힌다 */
		.domain_flags	= IRQ_DOMAIN_FLAG_ISOLATED_MSI,	/* [한국어] 이 아래 MSI 는 서로 격리된다는 선언. VFIO 가 장치를 안전하게 넘길 수 있는 근거 */
		.size		= INTR_REMAP_TABLE_ENTRIES,	/* [한국어] 도메인이 다룰 수 있는 인터럽트 수 = 표 항목 수 */
		.host_data	= iommu,	/* [한국어] 콜백들이 어느 유닛인지 알 수 있도록 유닛 포인터를 실어 둔다 */
	};
	struct ir_table *ir_table;	/* [한국어] 표 서술자 */
	unsigned long *bitmap;	/* [한국어] 항목 할당 비트맵 */
	void *ir_table_base;	/* [한국어] 표 본체(1MB)의 가상 주소 */

	if (iommu->ir_table)	/* [한국어] 핫플러그·재초기화로 두 번 불릴 수 있다 */
		return 0;	/* [한국어] 이미 준비됨 — 아무것도 하지 않는다 */

	ir_table = kzalloc_obj(struct ir_table);	/* [한국어] 표 서술자 할당 */
	if (!ir_table)	/* [한국어] 메모리 부족 */
		return -ENOMEM;	/* [한국어] 아직 아무것도 잡지 않았으므로 되돌릴 것이 없다 */

	/* 1MB - maximum possible interrupt remapping table size */
	ir_table_base =	/* [한국어] 1MB 연속 페이지를 잡아 표 본체로 쓴다 */
		iommu_alloc_pages_node_sz(iommu->node, GFP_KERNEL, SZ_1M);	/* [한국어] 표 본체. 항목 16바이트 × 인덱스 16비트 = 최대 1MB, 유닛과 같은 NUMA 노드에서 */
	if (!ir_table_base) {	/* [한국어] 1MB 연속 할당 실패 */
		pr_err("IR%d: failed to allocate 1M of pages\n", iommu->seq_id);	/* [한국어] 어느 유닛에서 실패했는지 알린다 */
		goto out_free_table;	/* [한국어] 서술자만 되돌린다 */
	}

	bitmap = bitmap_zalloc(INTR_REMAP_TABLE_ENTRIES, GFP_KERNEL);	/* [한국어] 항목 하나당 1비트의 사용 여부 지도 */
	if (bitmap == NULL) {	/* [한국어] 비트맵 할당 실패 */
		pr_err("IR%d: failed to allocate bitmap\n", iommu->seq_id);	/* [한국어] 알리고 */
		goto out_free_pages;	/* [한국어] 표 본체까지 되돌린다 */
	}

	info.fwnode = irq_domain_alloc_named_id_fwnode("INTEL-IR", iommu->seq_id);	/* [한국어] 도메인을 이름으로 식별할 펌웨어 노드. /proc/interrupts 등에 "INTEL-IR<n>"으로 보인다 */
	if (!info.fwnode)	/* [한국어] 노드 할당 실패 */
		goto out_free_bitmap;	/* [한국어] 비트맵까지 되돌린다 */

	iommu->ir_domain = msi_create_parent_irq_domain(&info, &dmar_msi_parent_ops);	/* [한국어] MSI 계층에서 부모 역할을 하는 도메인 생성. 장치의 MSI 는 이 도메인을 반드시 거친다 */
	if (!iommu->ir_domain) {	/* [한국어] 도메인 생성 실패 */
		pr_err("IR%d: failed to allocate irqdomain\n", iommu->seq_id);	/* [한국어] 알리고 */
		goto out_free_fwnode;	/* [한국어] fwnode 까지 되돌린다 */
	}

	ir_table->base = ir_table_base;	/* [한국어] 서술자에 표 본체를 연결 */
	ir_table->bitmap = bitmap;	/* [한국어] 할당 지도를 연결 */
	iommu->ir_table = ir_table;	/* [한국어] 이 시점부터 유닛이 표를 가진 것으로 간주된다 */

	/*
	 * If the queued invalidation is already initialized,
	 * shouldn't disable it.
	 */
	if (!iommu->qi) {	/* [한국어] 큐 무효화가 아직 없으면 — 항목을 고칠 때 캐시를 지울 수단이 없다 */
		/*
		 * Clear previous faults.
		 */
		dmar_fault(-1, iommu);	/* [한국어] 옛 커널이 남긴 폴트 기록을 먼저 치운다. 남아 있으면 QI 초기화가 방해받는다 */
		dmar_disable_qi(iommu);	/* [한국어] 반쯤 살아 있을지 모르는 큐를 확실히 끈 뒤 */

		if (dmar_enable_qi(iommu)) {	/* [한국어] 새로 켠다 */
			pr_err("Failed to enable queued invalidation\n");	/* [한국어] QI 없이는 재매핑을 안전하게 쓸 수 없다 */
			goto out_free_ir_domain;	/* [한국어] 도메인부터 전부 되돌린다 */
		}
	}

	init_ir_status(iommu);	/* [한국어] 하드웨어에 물어 "진입 시 이미 켜져 있었는지"를 기록 */

	if (ir_pre_enabled(iommu)) {	/* [한국어] 물려받은 재매핑이 있다 */
		if (!is_kdump_kernel()) {	/* [한국어] 그런데 kdump 커널이 아니다 — 정상 부팅인데 켜져 있는 이상 상황 */
			pr_info_once("IRQ remapping was enabled on %s but we are not in kdump mode\n",	/* [한국어] 한 번만 알린다 */
				     iommu->name);
			clear_ir_pre_enabled(iommu);	/* [한국어] 물려받음 표시를 지우고 */
			iommu_disable_irq_remapping(iommu);	/* [한국어] 옛 설정을 믿을 수 없으므로 일단 끄고 깨끗하게 다시 시작한다 */
		} else if (iommu_load_old_irte(iommu))	/* [한국어] 진짜 kdump 면 옛 표를 복사한다. 실패하면 */
			pr_err("Failed to copy IR table for %s from previous kernel\n",	/* [한국어] 알리되 부팅은 계속한다 — 덤프를 뜨는 것이 우선이다 */
			       iommu->name);
		else
			pr_info("Copied IR table for %s from previous kernel\n",	/* [한국어] 성공하면 살아 있는 장치의 인터럽트가 그대로 이어진다 */
				iommu->name);
	}

	iommu_set_irq_remapping(iommu, eim_mode);	/* [한국어] 표 주소를 하드웨어에 건다. 켜는 것은 아직 아니다 */

	return 0;	/* [한국어] 준비 완료 */

out_free_ir_domain:	/* [한국어] 도메인까지 만든 뒤 실패한 경우의 되감기 시작점 */
	irq_domain_remove(iommu->ir_domain);	/* [한국어] 실패 되감기: 도메인 해제 */
	iommu->ir_domain = NULL;	/* [한국어] 매달린 포인터를 남기지 않는다 */
out_free_fwnode:	/* [한국어] fwnode 까지 만든 뒤 실패한 경우 */
	irq_domain_free_fwnode(info.fwnode);	/* [한국어] 펌웨어 노드 해제 */
out_free_bitmap:	/* [한국어] 비트맵까지 잡은 뒤 실패한 경우 */
	bitmap_free(bitmap);	/* [한국어] 비트맵 해제 */
out_free_pages:	/* [한국어] 표 본체까지 잡은 뒤 실패한 경우 */
	iommu_free_pages(ir_table_base);	/* [한국어] 표 본체 1MB 해제 */
out_free_table:	/* [한국어] 서술자만 잡은 뒤 실패한 경우 */
	kfree(ir_table);	/* [한국어] 서술자 해제 */

	iommu->ir_table  = NULL;	/* [한국어] "준비 안 됨" 상태를 분명히 남긴다 */

	return -ENOMEM;	/* [한국어] 어느 단계에서 실패했든 호출자에게는 자원 부족으로 보고한다 */
}

/*
 * [한국어]
 * intel_teardown_irq_remapping - 유닛의 재매핑 자원을 모두 되돌린다
 *
 * @iommu: 정리할 유닛. NULL 이거나 표가 없으면 아무 일도 하지 않는다.
 *
 * setup 이 쌓은 것을 역순으로 허문다: 도메인 → fwnode → 표 페이지 →
 * 비트맵 → ir_table 구조체. fwnode 를 도메인보다 나중에 놓는 이유는
 * irq_domain_remove() 가 아직 fwnode 를 참조하기 때문이다. 그래서 먼저
 * 지역 변수에 보관해 두고 도메인을 없앤 뒤 해제한다.
 *
 * 마지막에 iommu->ir_table 을 NULL 로 만들어, 이후 setup 이 다시 불렸을 때
 * "이미 준비됨"으로 착각하지 않게 한다.
 *
 * 주의: 이 함수는 하드웨어를 끄지 않는다. 끄는 일은
 * iommu_disable_irq_remapping() 이 따로 하며, 순서를 지키지 않으면
 * 하드웨어가 이미 해제된 메모리를 표로 참조하게 된다.
 *
 * 호출 체인:
 *   intel_cleanup_irq_remapping()/dmar_ir_add() 실패 경로 → [이 함수]
 */
static void intel_teardown_irq_remapping(struct intel_iommu *iommu)
{
	struct fwnode_handle *fn;	/* [한국어] 도메인이 없어진 뒤에 해제해야 하므로 미리 붙잡아 둔다 */

	if (iommu && iommu->ir_table) {	/* [한국어] 준비된 적이 있어야 되돌릴 것도 있다 */
		if (iommu->ir_domain) {	/* [한국어] 도메인이 만들어졌다면 */
			fn = iommu->ir_domain->fwnode;	/* [한국어] 도메인 해제가 이 포인터를 지우기 전에 보관 */

			irq_domain_remove(iommu->ir_domain);	/* [한국어] 인터럽트 계층에서 이 도메인을 뗀다 */
			irq_domain_free_fwnode(fn);	/* [한국어] 이제 아무도 참조하지 않으므로 노드를 해제 */
			iommu->ir_domain = NULL;	/* [한국어] 매달린 포인터 제거 */
		}
		iommu_free_pages(iommu->ir_table->base);	/* [한국어] 표 본체 해제 */
		bitmap_free(iommu->ir_table->bitmap);	/* [한국어] 할당 지도 해제 */
		kfree(iommu->ir_table);	/* [한국어] 서술자 해제 */
		iommu->ir_table = NULL;	/* [한국어] 다음 setup 이 "이미 준비됨"으로 착각하지 않게 */
	}
}

/*
 * Disable Interrupt Remapping.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommu_disable_irq_remapping - 하드웨어의 인터럽트 재매핑을 끈다
 *
 * @iommu: 대상 유닛.
 *
 * 끄는 순서가 중요하다. 먼저 항목 캐시를 전역 무효화하고 그다음 IRE 비트를
 * 내린다. 반대로 하면, 재매핑이 꺼진 뒤에도 하드웨어가 캐시에 남은 옛
 * 항목으로 인터럽트를 계속 옮길 수 있다. cap_esirtps 가 있으면 하드웨어가
 * 이 무효화를 스스로 보장하므로 건너뛴다.
 *
 * GSTS 를 먼저 읽어 IRES 가 꺼져 있으면 곧바로 빠져나가는 이유: 이미 꺼진
 * 상태에서 GCMD 를 다시 쓰면 gcmd 사본과 실제 상태가 어긋날 수 있고,
 * IOMMU_WAIT_OP 가 영영 오지 않을 조건을 기다릴 수도 있다.
 *
 * 실행 컨텍스트: register_lock 을 쥐고 인터럽트를 끈 채. 부팅 실패 정리,
 * 서스펜드, kdump 가 아닌데 물려받은 재매핑을 발견했을 때 불린다.
 *
 * 호출 체인:
 *   intel_setup_irq_remapping()/disable_irq_remapping() → [이 함수]
 */
static void iommu_disable_irq_remapping(struct intel_iommu *iommu)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */
	u32 sts;	/* [한국어] GSTS 값 */

	if (!ecap_ir_support(iommu->ecap))	/* [한국어] 재매핑을 지원하지 않는 유닛이면 끌 것도 없다 */
		return;	/* [한국어] 바로 반환 */

	/*
	 * global invalidation of interrupt entry cache before disabling
	 * interrupt-remapping.
	 */
	if (!cap_esirtps(iommu->cap))	/* [한국어] 하드웨어가 캐시 무효화를 스스로 보장하지 않으면 */
		qi_global_iec(iommu);	/* [한국어] 끄기 전에 항목 캐시를 비운다. 순서를 바꾸면 꺼진 뒤에도 옛 항목이 쓰인다 */

	raw_spin_lock_irqsave(&iommu->register_lock, flags);	/* [한국어] 레지스터와 gcmd 사본 보호 */

	sts = readl(iommu->reg + DMAR_GSTS_REG);	/* [한국어] 현재 상태 확인 */
	if (!(sts & DMA_GSTS_IRES))	/* [한국어] 이미 꺼져 있다 */
		goto end;	/* [한국어] 다시 쓰면 사본이 어긋나고 기다림이 끝나지 않을 수 있다 */

	iommu->gcmd &= ~DMA_GCMD_IRE;	/* [한국어] 사본에서 IRE 를 내리고 */
	writel(iommu->gcmd, iommu->reg + DMAR_GCMD_REG);	/* [한국어] 실제로 재매핑을 끈다 */

	IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG,	/* [한국어] IRES 가 내려갈 때까지 기다린다 */
		      readl, !(sts & DMA_GSTS_IRES), sts);

end:	/* [한국어] 이미 꺼져 있어 건너뛴 경우도 여기로 모여 락을 푼다 */
	raw_spin_unlock_irqrestore(&iommu->register_lock, flags);	/* [한국어] 종료 */
}

/*
 * [한국어]
 * dmar_x2apic_optout - BIOS 가 x2APIC 을 쓰지 말라고 했는지 확인한다
 *
 * @return: BIOS 가 opt-out 을 요청했으면 0 이 아닌 값.
 *
 * DMAR 표의 플래그에 "이 플랫폼에서는 x2APIC 을 켜지 말라"는 비트가 있다.
 * 펌웨어(SMM 등)가 xAPIC 을 전제로 동작하는 기계에서, 커널이 x2APIC 으로
 * 넘어가면 펌웨어가 인터럽트를 잘못 다뤄 시스템이 불안정해지기 때문이다.
 *
 * no_x2apic_optout 부팅 옵션은 그 요청을 무시하게 한다 — BIOS 가 과도하게
 * 보수적인 기계에서 x2APIC 의 이점(CPU 255개 초과 지원, 더 빠른 IPI)을
 * 되찾기 위한 탈출구다.
 *
 * 호출 체인:
 *   intel_prepare_irq_remapping() → [이 함수]
 */
static int __init dmar_x2apic_optout(void)
{
	struct acpi_table_dmar *dmar;	/* [한국어] DMAR ACPI 표의 헤더 */
	dmar = (struct acpi_table_dmar *)dmar_tbl;	/* [한국어] 부팅 초기에 매핑해 둔 표를 가리킨다 */
	if (!dmar || no_x2apic_optout)	/* [한국어] 표가 없거나, 부팅 옵션으로 BIOS 의 요청을 무시하기로 했다면 */
		return 0;	/* [한국어] opt-out 없음 — x2APIC 을 써도 된다 */
	return dmar->flags & DMAR_X2APIC_OPT_OUT;	/* [한국어] BIOS 가 세운 opt-out 비트를 그대로 돌려준다 */
}

/*
 * [한국어]
 * intel_cleanup_irq_remapping - 준비하다 실패했을 때 모든 유닛을 원상 복구한다
 *
 * 어느 유닛 하나에서라도 준비가 실패하면 재매핑 전체를 포기한다. 일부만
 * 켜 두면 그 유닛 아래 장치만 보호되고 나머지는 무방비인, 이해하기 어렵고
 * 검증도 안 되는 상태가 되기 때문이다.
 *
 * 순서: 먼저 하드웨어를 끄고(iommu_disable) 그다음 메모리를 놓는다
 * (intel_teardown). 반대로 하면 하드웨어가 이미 해제된 메모리를 표로
 * 참조하게 된다.
 *
 * 마지막 경고: x2APIC 을 지원하는 기계에서 재매핑이 꺼진 채로 간다는 것은
 * 인터럽트 주입 공격에 노출된다는 뜻이라, 관리자가 알 수 있게 알린다.
 *
 * 호출 체인:
 *   intel_prepare_irq_remapping()/intel_enable_irq_remapping() 실패 → [이 함수]
 */
static void __init intel_cleanup_irq_remapping(void)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회용 */
	struct intel_iommu *iommu;	/* [한국어] 현재 유닛 */

	for_each_iommu(iommu, drhd) {	/* [한국어] 모든 유닛을 되돌린다 — 일부만 켜진 상태를 남기지 않는다 */
		if (ecap_ir_support(iommu->ecap)) {	/* [한국어] 재매핑을 지원하는 유닛만 손댈 것이 있다 */
			iommu_disable_irq_remapping(iommu);	/* [한국어] 먼저 하드웨어를 끈다 */
			intel_teardown_irq_remapping(iommu);	/* [한국어] 그다음 메모리를 놓는다. 순서를 바꾸면 하드웨어가 해제된 표를 참조한다 */
		}
	}

	if (x2apic_supported())	/* [한국어] x2APIC 기계인데 재매핑을 못 켰다면 */
		pr_warn("Failed to enable irq remapping. You are vulnerable to irq-injection attacks.\n");	/* [한국어] 인터럽트 주입 공격에 노출된다는 사실을 관리자에게 알린다 */
}

/*
 * [한국어]
 * intel_prepare_irq_remapping - 재매핑을 켜기 전 모든 사전 조건을 확인·준비한다
 *
 * @return: 0 이면 모든 유닛이 준비됐다. -ENODEV 면 어떤 이유로든 쓸 수 없다.
 *
 * "준비"와 "켜기"가 나뉜 이유: 재매핑을 켜면 인터럽트 경로가 통째로 바뀌므로,
 * 중간에 실패해 절반만 켜진 상태가 되면 시스템을 되돌릴 수 없다. 그래서 표
 * 할당·도메인 생성·모드 결정처럼 실패할 수 있는 일을 전부 여기서 끝내고,
 * intel_enable_irq_remapping() 은 되돌릴 필요가 없는 비트 세우기만 한다.
 *
 * 확인하는 것들:
 *  - irq_remap_broken: 이 칩셋에 재매핑을 불안정하게 만드는 errata 가 있다고
 *    알려진 경우. 안정성을 위해 아예 켜지 않고 커널을 taint 한다.
 *  - DMAR 표를 읽을 수 있는가, 표가 재매핑을 광고하는가.
 *  - IOAPIC 들이 전부 어느 유닛 아래 있는가(parse_ioapics_under_ir). 하나라도
 *    빠지면 그 IOAPIC 의 인터럽트는 검증 없이 지나가므로 켜서는 안 된다.
 *  - 모든 유닛이 재매핑을 지원하는가. 하나라도 못 하면 전체를 포기한다.
 *
 * 모드 결정: x2APIC 을 쓸 수 있고 BIOS 가 반대하지 않으면 EIM 으로 간다.
 * 단, 유닛 중 하나라도 EIM 을 지원하지 않으면 전부 xAPIC 으로 내린다 —
 * 유닛마다 모드가 다르면 목적지 필드의 해석이 갈려 일관성이 깨진다.
 *
 * 실행 컨텍스트: 부팅 초기 __init, 단일 스레드, 인터럽트 비활성.
 *
 * 호출 체인:
 *   irq_remapping_prepare() → [이 함수] → intel_setup_irq_remapping()
 */
static int __init intel_prepare_irq_remapping(void)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회용 */
	struct intel_iommu *iommu;	/* [한국어] 현재 유닛 */
	int eim = 0;	/* [한국어] x2APIC(확장 인터럽트 모드)로 갈지의 잠정 결론 */

	if (irq_remap_broken) {	/* [한국어] 이 칩셋에 재매핑을 불안정하게 만드는 errata 가 알려져 있는가 */
		pr_warn("This system BIOS has enabled interrupt remapping\n"	/* [한국어] BIOS 갱신을 권하는 안내 */
			"on a chipset that contains an erratum making that\n"
			"feature unstable.  To maintain system stability\n"
			"interrupt remapping is being disabled.  Please\n"
			"contact your BIOS vendor for an update\n");
		add_taint(TAINT_FIRMWARE_WORKAROUND, LOCKDEP_STILL_OK);	/* [한국어] 펌웨어 우회가 적용됐음을 커널에 기록 — 이후 버그 보고의 맥락이 된다 */
		return -ENODEV;	/* [한국어] 안정성을 위해 재매핑을 포기한다 */
	}

	if (dmar_table_init() < 0)	/* [한국어] DMAR ACPI 표를 읽을 수 없으면 */
		return -ENODEV;	/* [한국어] 재매핑 하드웨어의 위치를 알 방법이 없다 */

	if (!dmar_ir_support())	/* [한국어] 표가 인터럽트 재매핑을 광고하지 않으면 */
		return -ENODEV;	/* [한국어] 이 플랫폼에는 기능이 없다 */

	if (parse_ioapics_under_ir()) {	/* [한국어] 모든 IOAPIC 이 어느 유닛 아래 있는지 확인 */
		pr_info("Not enabling interrupt remapping\n");	/* [한국어] 하나라도 빠지면 그 IOAPIC 의 인터럽트가 검증 없이 지나간다 */
		goto error;	/* [한국어] 부분 보호는 하지 않는다 */
	}

	/* First make sure all IOMMUs support IRQ remapping */
	for_each_iommu(iommu, drhd)	/* [한국어] 모든 유닛을 돈다 — 하나라도 빠지면 그 유닛 아래가 무방비가 된다 */
		if (!ecap_ir_support(iommu->ecap))	/* [한국어] 유닛 하나라도 재매핑을 지원하지 않으면 */
			goto error;	/* [한국어] 전체를 포기한다 */

	/* Detect remapping mode: lapic or x2apic */
	if (x2apic_supported()) {	/* [한국어] CPU 가 x2APIC 을 지원하는가 */
		eim = !dmar_x2apic_optout();	/* [한국어] BIOS 가 반대하지 않으면 EIM 으로 간다 */
		if (!eim) {	/* [한국어] BIOS 가 opt-out 을 요청했다 */
			pr_info("x2apic is disabled because BIOS sets x2apic opt out bit.");	/* [한국어] 이유를 알리고 */
			pr_info("Use 'intremap=no_x2apic_optout' to override the BIOS setting.\n");	/* [한국어] 무시할 수 있는 방법도 알려 준다 */
		}
	}

	for_each_iommu(iommu, drhd) {	/* [한국어] 유닛마다 재매핑 지원 여부를 확인한다 */
		if (eim && !ecap_eim_support(iommu->ecap)) {	/* [한국어] 유닛 중 하나라도 EIM 을 지원하지 않으면 */
			pr_info("%s does not support EIM\n", iommu->name);	/* [한국어] 어느 유닛인지 밝히고 */
			eim = 0;	/* [한국어] 전부 xAPIC 으로 내린다. 유닛마다 모드가 다르면 목적지 해석이 갈린다 */
		}
	}

	eim_mode = eim;	/* [한국어] 최종 모드를 전역에 확정. 이후 IRTE_DEST 등이 이 값을 본다 */
	if (eim)	/* [한국어] x2APIC 으로 간다면 */
		pr_info("Queued invalidation will be enabled to support x2apic and Intr-remapping.\n");	/* [한국어] QI 가 반드시 필요해진다는 사실을 알린다 */

	/* Do the initializations early */
	for_each_iommu(iommu, drhd) {	/* [한국어] 유닛마다 EIM 지원 여부를 확인한다 */
		if (intel_setup_irq_remapping(iommu)) {	/* [한국어] 유닛마다 표·도메인·QI 를 준비한다 */
			pr_err("Failed to setup irq remapping for %s\n",	/* [한국어] 어느 유닛에서 실패했는지 */
			       iommu->name);
			goto error;	/* [한국어] 하나라도 실패하면 전체 되돌리기 */
		}
	}

	return 0;	/* [한국어] 모든 유닛이 준비됐다 — 이제 켜기만 하면 된다 */

error:	/* [한국어] 어느 단계에서 실패했든 전부 되돌리는 공통 경로 */
	intel_cleanup_irq_remapping();	/* [한국어] 지금까지 준비한 것을 전부 되돌린다 */
	return -ENODEV;	/* [한국어] 호출자는 재매핑 없이 부팅을 계속한다 */
}

/*
 * Set Posted-Interrupts capability.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * set_irq_posting_cap - 포스티드 인터럽트를 쓸 수 있는지 결론 내린다
 *
 * 포스티드 인터럽트는 하드웨어가 인터럽트를 CPU 로 곧장 보내지 않고, 게스트
 * 메모리의 서술자에 표시한 뒤 알림 벡터 하나만 보내는 방식이다. VM exit
 * 없이 게스트에 인터럽트를 전달할 수 있어 가상화 성능이 크게 달라진다.
 *
 * 두 가지 조건을 모두 확인한다.
 *  1) X86_FEATURE_CX16 (cmpxchg16b): 포스티드 형식 항목에서는 pda 필드가
 *     64비트 경계를 가로질러 놓인다. 그래서 항목을 고치려면 128비트를
 *     원자적으로 바꿔야 하고, 그 명령이 없으면 안전하게 갱신할 수 없다.
 *  2) 모든 유닛의 cap_pi_support: 하나라도 지원하지 않으면 능력 자체를
 *     내린다. 유닛에 따라 되고 안 되는 기능은 상위 계층이 쓸 수 없다.
 *
 * disable_irq_post 부팅 옵션이 있으면 처음부터 아무것도 켜지 않는다.
 *
 * 호출 체인:
 *   intel_enable_irq_remapping() → [이 함수]
 */
static inline void set_irq_posting_cap(void)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회용 */
	struct intel_iommu *iommu;	/* [한국어] 현재 유닛 */

	if (!disable_irq_post) {	/* [한국어] 부팅 옵션으로 포스티드 인터럽트를 끄지 않았다면 */
		/*
		 * If IRTE is in posted format, the 'pda' field goes across the
		 * 64-bit boundary, we need use cmpxchg16b to atomically update
		 * it. We only expose posted-interrupt when X86_FEATURE_CX16
		 * is supported. Actually, hardware platforms supporting PI
		 * should have X86_FEATURE_CX16 support, this has been confirmed
		 * with Intel hardware guys.
		 */
		if (boot_cpu_has(X86_FEATURE_CX16))	/* [한국어] 128비트 원자적 교환이 가능한 CPU 인가 — 포스티드 항목 갱신의 전제 조건 */
			intel_irq_remap_ops.capability |= 1 << IRQ_POSTING_CAP;	/* [한국어] 일단 능력이 있다고 표시하고 */

		for_each_iommu(iommu, drhd)	/* [한국어] 모든 유닛이 포스티드를 지원해야만 능력을 유지한다 */
			if (!cap_pi_support(iommu->cap)) {	/* [한국어] 유닛 중 하나라도 포스티드를 지원하지 않으면 */
				intel_irq_remap_ops.capability &=	/* [한국어] 능력을 도로 내린다 — 유닛마다 다른 기능은 쓸 수 없다 */
						~(1 << IRQ_POSTING_CAP);
				break;	/* [한국어] 하나만 찾아도 결론이 난다 */
			}
	}
}

/*
 * [한국어]
 * intel_enable_irq_remapping - 준비된 유닛들의 재매핑을 실제로 켠다
 *
 * @return: 성공하면 동작 모드(IRQ_REMAP_X2APIC_MODE 또는 XAPIC_MODE),
 *          실패하면 -1.
 *
 * prepare 단계에서 실패할 만한 일은 모두 끝났으므로, 여기서는 유닛마다
 * IRE 비트를 세우기만 한다. 반환값이 모드인 이유: 호출자(APIC 초기화)가
 * 이 값을 보고 x2APIC 을 켤지 결정한다. 재매핑 없이는 CPU 255개를 넘는
 * x2APIC 목적지를 안전하게 다룰 수 없으므로, 두 결정이 여기서 묶인다.
 *
 * ir_pre_enabled 인 유닛을 건너뛰는 이유: kdump 로 물려받아 이미 켜져
 * 있으므로 다시 켤 필요가 없고, 건드리면 그 사이 인터럽트를 흘릴 수 있다.
 *
 * setup 플래그: 유닛이 하나도 없으면(for_each_iommu 가 한 번도 돌지 않으면)
 * 켤 대상이 없다는 뜻이라 실패로 처리한다.
 *
 * 호출 체인:
 *   irq_remapping_enable() → [이 함수] → iommu_enable_irq_remapping()
 *     → set_irq_posting_cap()
 */
static int __init intel_enable_irq_remapping(void)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회용 */
	struct intel_iommu *iommu;	/* [한국어] 현재 유닛 */
	bool setup = false;	/* [한국어] 켠 유닛이 하나라도 있었는지 */

	/*
	 * Setup Interrupt-remapping for all the DRHD's now.
	 */
	for_each_iommu(iommu, drhd) {	/* [한국어] 준비된 유닛을 하나씩 켠다 */
		if (!ir_pre_enabled(iommu))	/* [한국어] kdump 로 물려받아 이미 켜져 있으면 건드리지 않는다 */
			iommu_enable_irq_remapping(iommu);	/* [한국어] IRE 를 세우고 호환 형식 MSI 를 막는다 */
		setup = true;	/* [한국어] 유닛이 존재했다는 표시 */
	}

	if (!setup)	/* [한국어] 유닛이 하나도 없었다 */
		goto error;	/* [한국어] 켤 대상이 없으면 실패로 본다 */

	irq_remapping_enabled = 1;	/* [한국어] 코어에 "재매핑이 동작 중"임을 알린다. MSI 할당 경로가 이 값을 본다 */

	set_irq_posting_cap();	/* [한국어] 포스티드 인터럽트를 쓸 수 있는지 최종 결론 */

	pr_info("Enabled IRQ remapping in %s mode\n", eim_mode ? "x2apic" : "xapic");	/* [한국어] 어느 모드로 켰는지 남긴다 */

	return eim_mode ? IRQ_REMAP_X2APIC_MODE : IRQ_REMAP_XAPIC_MODE;	/* [한국어] 호출자는 이 값으로 x2APIC 을 켤지 결정한다 */

error:	/* [한국어] 켤 유닛이 없었던 경우의 정리 경로 */
	intel_cleanup_irq_remapping();	/* [한국어] 실패하면 전부 되돌리고 */
	return -1;	/* [한국어] 재매핑 없이 부팅을 계속하게 한다 */
}

/*
 * [한국어]
 * ir_parse_one_hpet_scope - DMAR 의 HPET scope 항목 하나를 ir_hpet[] 에 담는다
 *
 * @scope: 표 안의 device scope 항목.
 * @iommu: 이 scope 를 선언한 유닛.
 * @drhd: 그 유닛의 표 항목(로그용 주소).
 * @return: 0 성공, -ENOSPC 면 배열이 가득 찼다.
 *
 * device scope 의 경로 걷기: DMAR 표는 장치를 "시작 버스 + 브리지 경로"로
 * 적는다. 그래서 path 배열을 따라가며 각 브리지의 secondary bus 를 읽어
 * 최종 버스 번호를 계산한다. 이때 PCI 서브시스템이 아직 초기화되지 않아
 * 정상 API 를 쓸 수 없으므로, read_pci_config_byte 로 설정 공간을 직접 읽는다.
 *
 * 배열 재사용 검사: 같은 유닛·같은 id 가 이미 있으면 중복 등록이므로 그냥
 * 성공을 돌려준다. 핫플러그로 유닛이 다시 들어올 때 항목이 쌓이는 것을 막는다.
 * 빈 칸(iommu==NULL)을 기억해 두었다가 없으면 거기에 넣는다.
 *
 * 이 정보가 없으면 HPET 인터럽트의 소스 id 를 알 수 없어 set_hpet_sid() 가
 * 실패하고, 결국 타이머 인터럽트를 재매핑할 수 없게 된다.
 *
 * 호출 체인:
 *   ir_parse_ioapic_hpet_scope() → [이 함수] → read_pci_config_byte()
 */
static int ir_parse_one_hpet_scope(struct acpi_dmar_device_scope *scope,
				   struct intel_iommu *iommu,
				   struct acpi_dmar_hardware_unit *drhd)
{
	struct acpi_dmar_pci_path *path;	/* [한국어] 표가 적어 둔 브리지 경로 배열 */
	u8 bus;	/* [한국어] 경로를 따라가며 계산할 최종 버스 번호 */
	int count, free = -1;	/* [한국어] 경로 길이, 그리고 배열에서 찾은 빈 칸 */

	bus = scope->bus;	/* [한국어] 경로의 출발 버스 */
	path = (struct acpi_dmar_pci_path *)(scope + 1);	/* [한국어] 고정 헤더 바로 뒤부터 경로가 이어진다 */
	count = (scope->length - sizeof(struct acpi_dmar_device_scope))	/* [한국어] 항목 전체 길이에서 헤더를 빼고 */
		/ sizeof(struct acpi_dmar_pci_path);	/* [한국어] 경로 원소 크기로 나눠 개수를 얻는다 */

	while (--count > 0) {	/* [한국어] 마지막 원소는 장치 자신이므로 그 앞까지만 브리지로 따라간다 */
		/*
		 * Access PCI directly due to the PCI
		 * subsystem isn't initialized yet.
		 */
		bus = read_pci_config_byte(bus, path->device, path->function,	/* [한국어] 브리지의 secondary bus 를 읽어 한 단계 내려간다. PCI 서브시스템이 아직 없어 설정 공간을 직접 읽는다 */
					   PCI_SECONDARY_BUS);
		path++;	/* [한국어] 다음 브리지로 */
	}

	for (count = 0; count < MAX_HPET_TBS; count++) {	/* [한국어] 기존 등록이 있는지 훑으면서 빈 칸도 함께 찾는다 */
		if (ir_hpet[count].iommu == iommu &&	/* [한국어] 같은 유닛이 */
		    ir_hpet[count].id == scope->enumeration_id)	/* [한국어] 같은 HPET 을 이미 등록했다면 */
			return 0;	/* [한국어] 중복이므로 그대로 성공. 핫플러그 재등록에서 항목이 쌓이는 것을 막는다 */
		else if (ir_hpet[count].iommu == NULL && free == -1)	/* [한국어] 비어 있는 첫 칸을 기억해 둔다 */
			free = count;	/* [한국어] 나중에 여기에 넣는다 */
	}
	if (free == -1) {	/* [한국어] 빈 칸이 없다 — 배열 크기를 넘는 HPET 이 있는 플랫폼 */
		pr_warn("Exceeded Max HPET blocks\n");	/* [한국어] 알리고 */
		return -ENOSPC;	/* [한국어] 실패. 이 HPET 의 인터럽트는 재매핑할 수 없다 */
	}

	ir_hpet[free].iommu = iommu;	/* [한국어] 담당 유닛 */
	ir_hpet[free].id    = scope->enumeration_id;	/* [한국어] 표가 부여한 HPET 번호 */
	ir_hpet[free].bus   = bus;	/* [한국어] 경로를 따라 계산한 버스 번호 */
	ir_hpet[free].devfn = PCI_DEVFN(path->device, path->function);	/* [한국어] 경로의 마지막 원소가 이 HPET 의 devfn */
	pr_info("HPET id %d under DRHD base 0x%Lx\n",	/* [한국어] 어느 유닛 아래 붙었는지 남긴다 */
		scope->enumeration_id, drhd->address);

	return 0;	/* [한국어] 등록 성공 */
}

/*
 * [한국어]
 * ir_parse_one_ioapic_scope - DMAR 의 IOAPIC scope 항목 하나를 ir_ioapic[] 에 담는다
 *
 * @scope: 표 안의 device scope 항목.
 * @iommu: 이 scope 를 선언한 유닛.
 * @drhd: 그 유닛의 표 항목(로그용 주소).
 * @return: 0 성공, -ENOSPC 면 배열이 가득 찼다.
 *
 * device scope 의 경로 걷기: DMAR 표는 장치를 "시작 버스 + 브리지 경로"로
 * 적는다. 그래서 path 배열을 따라가며 각 브리지의 secondary bus 를 읽어
 * 최종 버스 번호를 계산한다. 이때 PCI 서브시스템이 아직 초기화되지 않아
 * 정상 API 를 쓸 수 없으므로, read_pci_config_byte 로 설정 공간을 직접 읽는다.
 *
 * 배열 재사용 검사: 같은 유닛·같은 id 가 이미 있으면 중복 등록이므로 그냥
 * 성공을 돌려준다. 핫플러그로 유닛이 다시 들어올 때 항목이 쌓이는 것을 막는다.
 * 빈 칸(iommu==NULL)을 기억해 두었다가 없으면 거기에 넣는다.
 *
 * 여기서 담은 대응이 parse_ioapics_under_ir() 의 검사 근거가 된다 — 커널이
 * 아는 IOAPIC 중 하나라도 여기에 없으면 재매핑을 켜지 않는다.
 *
 * 호출 체인:
 *   ir_parse_ioapic_hpet_scope() → [이 함수] → read_pci_config_byte()
 */
static int ir_parse_one_ioapic_scope(struct acpi_dmar_device_scope *scope,
				     struct intel_iommu *iommu,
				     struct acpi_dmar_hardware_unit *drhd)
{
	struct acpi_dmar_pci_path *path;	/* [한국어] 표가 적어 둔 브리지 경로 배열 */
	u8 bus;	/* [한국어] 경로를 따라가며 계산할 최종 버스 번호 */
	int count, free = -1;	/* [한국어] 경로 길이, 그리고 배열에서 찾은 빈 칸 */

	bus = scope->bus;	/* [한국어] 경로의 출발 버스 */
	path = (struct acpi_dmar_pci_path *)(scope + 1);	/* [한국어] 고정 헤더 바로 뒤부터 경로가 이어진다 */
	count = (scope->length - sizeof(struct acpi_dmar_device_scope))	/* [한국어] 항목 전체 길이에서 헤더를 빼고 */
		/ sizeof(struct acpi_dmar_pci_path);	/* [한국어] 경로 원소 크기로 나눠 개수를 얻는다 */

	while (--count > 0) {	/* [한국어] 마지막 원소는 장치 자신이므로 그 앞까지만 브리지로 따라간다 */
		/*
		 * Access PCI directly due to the PCI
		 * subsystem isn't initialized yet.
		 */
		bus = read_pci_config_byte(bus, path->device, path->function,	/* [한국어] 브리지의 secondary bus 를 읽어 한 단계 내려간다. PCI 서브시스템이 아직 없어 설정 공간을 직접 읽는다 */
					   PCI_SECONDARY_BUS);
		path++;	/* [한국어] 다음 브리지로 */
	}

	for (count = 0; count < MAX_IO_APICS; count++) {	/* [한국어] 기존 등록 확인과 빈 칸 찾기를 한 번에 */
		if (ir_ioapic[count].iommu == iommu &&	/* [한국어] 같은 유닛이 */
		    ir_ioapic[count].id == scope->enumeration_id)	/* [한국어] 같은 IOAPIC 을 이미 등록했는가 */
			return 0;	/* [한국어] 중복 등록 방지 */
		else if (ir_ioapic[count].iommu == NULL && free == -1)	/* [한국어] 첫 빈 칸을 기억 */
			free = count;	/* [한국어] 여기에 넣을 것이다 */
	}
	if (free == -1) {	/* [한국어] 빈 칸 없음 */
		pr_warn("Exceeded Max IO APICS\n");	/* [한국어] 알리고 */
		return -ENOSPC;	/* [한국어] 실패 — 호출자는 재매핑을 포기한다 */
	}

	ir_ioapic[free].bus   = bus;	/* [한국어] 경로를 따라 계산한 버스 */
	ir_ioapic[free].devfn = PCI_DEVFN(path->device, path->function);	/* [한국어] 마지막 경로 원소가 devfn */
	ir_ioapic[free].iommu = iommu;	/* [한국어] 담당 유닛 */
	ir_ioapic[free].id    = scope->enumeration_id;	/* [한국어] MADT 와 대조할 IOAPIC 번호 */
	pr_info("IOAPIC id %d under DRHD base  0x%Lx IOMMU %d\n",	/* [한국어] 어느 유닛 아래인지 남긴다 */
		scope->enumeration_id, drhd->address, iommu->seq_id);

	return 0;	/* [한국어] 등록 성공 */
}

/*
 * [한국어]
 * ir_parse_ioapic_hpet_scope - 유닛 하나의 DRHD 항목에서 IOAPIC/HPET scope 를 훑는다
 *
 * @header: DRHD 항목의 헤더(ACPI 표 안의 위치).
 * @iommu: 그 항목이 서술하는 유닛.
 * @return: 0 성공, 아니면 첫 실패의 오류 코드.
 *
 * DRHD 항목은 고정 헤더 뒤에 가변 개수의 device scope 가 이어 붙은 구조다.
 * 그래서 헤더 바로 뒤(start)부터 header->length 로 계산한 끝(end)까지,
 * 각 scope 의 length 만큼 건너뛰며 순회한다 — 표가 스스로 자기 크기를
 * 알려 주는 형식이라 이렇게밖에 걸을 수 없다.
 *
 * PCI 장치 scope 는 여기서 무시한다. DMA 쪽(dmar.c)이 따로 다루고, 이
 * 파일은 "버스 열거로 발견되지 않는" IOAPIC 과 HPET 만 책임진다.
 *
 * ret 을 루프 조건에 넣은 이유: 한 scope 에서 실패하면 나머지를 계속 담아도
 * 어차피 재매핑을 켜지 않으므로 즉시 멈춘다.
 *
 * 호출 체인:
 *   parse_ioapics_under_ir() → [이 함수] → ir_parse_one_ioapic_scope()/hpet
 */
static int ir_parse_ioapic_hpet_scope(struct acpi_dmar_header *header,
				      struct intel_iommu *iommu)
{
	int ret = 0;	/* [한국어] 첫 실패를 담아 루프를 멈추는 데 쓴다 */
	struct acpi_dmar_hardware_unit *drhd;	/* [한국어] DRHD 항목으로 본 헤더 */
	struct acpi_dmar_device_scope *scope;	/* [한국어] 현재 보고 있는 scope */
	void *start, *end;	/* [한국어] scope 들이 놓인 구간의 시작과 끝 */

	drhd = (struct acpi_dmar_hardware_unit *)header;	/* [한국어] 헤더를 DRHD 로 해석 */
	start = (void *)(drhd + 1);	/* [한국어] 고정 헤더 바로 뒤부터 scope 가 이어진다 */
	end = ((void *)drhd) + header->length;	/* [한국어] 항목 전체 길이가 구간의 끝을 알려 준다 */

	while (start < end && ret == 0) {	/* [한국어] 끝에 닿거나 실패할 때까지 */
		scope = start;	/* [한국어] 현재 위치를 scope 로 본다 */
		if (scope->entry_type == ACPI_DMAR_SCOPE_TYPE_IOAPIC)	/* [한국어] IOAPIC scope 인가 */
			ret = ir_parse_one_ioapic_scope(scope, iommu, drhd);	/* [한국어] ir_ioapic[] 에 담는다 */
		else if (scope->entry_type == ACPI_DMAR_SCOPE_TYPE_HPET)	/* [한국어] HPET scope 인가 */
			ret = ir_parse_one_hpet_scope(scope, iommu, drhd);	/* [한국어] ir_hpet[] 에 담는다. PCI 장치 scope 는 DMA 쪽이 다루므로 여기선 무시 */
		start += scope->length;	/* [한국어] 각 항목이 자기 길이를 알려 주므로 그만큼 건너뛴다 */
	}

	return ret;	/* [한국어] 첫 실패의 오류 코드, 없으면 0 */
}

/*
 * [한국어]
 * ir_remove_ioapic_hpet_scope - 사라진 유닛이 담당하던 항목을 배열에서 지운다
 *
 * @iommu: 빠져나간 유닛.
 *
 * 유닛 핫플러그 제거 경로다. 배열은 고정 크기라 항목을 옮기지 않고, 담당
 * 유닛 포인터만 NULL 로 만들어 그 칸을 "빈 칸"으로 되돌린다. map_*_to_iommu
 * 가 iommu 가 NULL 인 칸을 건너뛰고, ir_parse_one_* 가 그 칸을 재사용한다.
 *
 * 왜 id 나 bus 는 그대로 두는가: iommu 하나만 보면 빈 칸 판정이 되므로
 * 나머지를 지울 이유가 없다. 다음에 그 칸을 쓰는 쪽이 전부 덮어쓴다.
 *
 * 호출 체인:
 *   dmar_ir_hotplug() 제거 경로 → [이 함수]
 */
static void ir_remove_ioapic_hpet_scope(struct intel_iommu *iommu)
{
	int i;	/* [한국어] 배열 순회 인덱스 */

	for (i = 0; i < MAX_HPET_TBS; i++)	/* [한국어] HPET 목록에서 */
		if (ir_hpet[i].iommu == iommu)	/* [한국어] 이 유닛이 담당하던 칸을 찾아 */
			ir_hpet[i].iommu = NULL;	/* [한국어] 유닛 포인터만 지워 빈 칸으로 되돌린다 */

	for (i = 0; i < MAX_IO_APICS; i++)	/* [한국어] IOAPIC 목록도 같은 방식으로 */
		if (ir_ioapic[i].iommu == iommu)	/* [한국어] 이 유닛 담당 칸을 */
			ir_ioapic[i].iommu = NULL;	/* [한국어] 비운다. 나머지 필드는 다음 등록이 덮어쓴다 */
}

/*
 * Finds the assocaition between IOAPIC's and its Interrupt-remapping
 * hardware unit.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * parse_ioapics_under_ir - 모든 IOAPIC 이 어느 유닛 아래 있는지 확인한다
 *
 * @return: 0 이면 전부 대응됐다. -ENODEV 면 재매핑을 지원하는 유닛이 없고,
 *          -1 이면 어떤 IOAPIC 이 어느 유닛에도 속하지 않는다.
 *
 * 두 단계다. 먼저 재매핑을 지원하는 유닛마다 device scope 를 훑어 ir_ioapic[]
 * 를 채우고, 그다음 커널이 아는 IOAPIC(nr_ioapics)을 하나씩 되짚어 전부
 * 대응이 있는지 확인한다.
 *
 * 두 번째 단계가 왜 필수인가: 대응 없는 IOAPIC 이 하나라도 있으면 그
 * IOAPIC 의 인터럽트는 재매핑을 거치지 않고 CPU 로 직행한다. 나머지를
 * 아무리 잘 보호해도 그 구멍 하나로 인터럽트 주입이 가능해지므로, 부분
 * 보호를 허용하지 않고 전체를 포기한다. 이 상황은 펌웨어가 표를 잘못
 * 만든 것이라 FW_BUG 접두사로 보고한다.
 *
 * 호출 체인:
 *   intel_prepare_irq_remapping() → [이 함수] → ir_parse_ioapic_hpet_scope()
 *     → map_ioapic_to_iommu()
 */
static int __init parse_ioapics_under_ir(void)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회용 */
	struct intel_iommu *iommu;	/* [한국어] 현재 유닛 */
	bool ir_supported = false;	/* [한국어] 재매핑을 지원하는 유닛이 하나라도 있었는지 */
	int ioapic_idx;	/* [한국어] 두 번째 단계의 IOAPIC 순회 인덱스 */

	for_each_iommu(iommu, drhd) {	/* [한국어] 모든 유닛의 재매핑을 끈다 */
		int ret;	/* [한국어] scope 파싱 결과 */

		if (!ecap_ir_support(iommu->ecap))	/* [한국어] 재매핑을 지원하지 않는 유닛은 IOAPIC 을 담당할 수 없다 */
			continue;	/* [한국어] 건너뛴다 */

		ret = ir_parse_ioapic_hpet_scope(drhd->hdr, iommu);	/* [한국어] 이 유닛이 선언한 IOAPIC/HPET 을 배열에 담는다 */
		if (ret)	/* [한국어] 파싱 실패 */
			return ret;	/* [한국어] 표가 이상하면 재매핑을 켜지 않는다 */

		ir_supported = true;	/* [한국어] 지원 유닛을 하나 확인 */
	}

	if (!ir_supported)	/* [한국어] 재매핑을 지원하는 유닛이 전혀 없다 */
		return -ENODEV;	/* [한국어] 이 플랫폼에는 기능이 없는 셈 */

	for (ioapic_idx = 0; ioapic_idx < nr_ioapics; ioapic_idx++) {	/* [한국어] 커널이 아는 IOAPIC 을 하나씩 되짚는다 */
		int ioapic_id = mpc_ioapic_id(ioapic_idx);	/* [한국어] MADT 가 부여한 그 IOAPIC 의 번호 */
		if (!map_ioapic_to_iommu(ioapic_id)) {	/* [한국어] 어느 유닛도 이 IOAPIC 을 담당하지 않는다 */
			pr_err(FW_BUG "ioapic %d has no mapping iommu, "	/* [한국어] 펌웨어가 표를 잘못 만든 것이라 FW_BUG 로 보고 */
			       "interrupt remapping will be disabled\n",
			       ioapic_id);
			return -1;	/* [한국어] 구멍 하나로 인터럽트 주입이 가능해지므로 전체를 포기한다 */
		}
	}

	return 0;	/* [한국어] 모든 IOAPIC 이 어느 유닛 아래에 있다 */
}

/*
 * [한국어]
 * ir_dev_scope_init - PCI 열거가 끝난 뒤 device scope 를 실제 장치와 잇는다
 *
 * @return: dmar_dev_scope_init() 의 결과.
 *
 * rootfs_initcall 로 등록되는 이유가 이 함수의 전부다. 부팅 초기에 재매핑을
 * 켤 때는 PCI 서브시스템이 없어 표의 경로를 직접 읽는 수밖에 없었다. PCI
 * 열거가 끝난 이 시점에야 표에 적힌 장치들을 진짜 struct pci_dev 와 연결할
 * 수 있고, 그래야 이후 장치별 MSI 도메인 배정이 가능해진다.
 *
 * irq_remapping_enabled 가 아니면 곧바로 성공을 돌려준다 — 재매핑을 켜지
 * 않았다면 이 연결이 아무 데도 쓰이지 않는다.
 *
 * dmar_global_lock 을 쓰기 모드로 잡는 이유: 이 초기화는 DMAR 전역 목록을
 * 바꾸고, 같은 시점에 PCI 핫플러그 알림이 들어올 수 있다.
 *
 * 실행 컨텍스트: rootfs_initcall, 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인:
 *   rootfs_initcall → [이 함수] → dmar_dev_scope_init()
 */
static int __init ir_dev_scope_init(void)
{
	int ret;	/* [한국어] 초기화 결과 */

	if (!irq_remapping_enabled)	/* [한국어] 재매핑을 켜지 않았다면 이 연결이 쓰일 데가 없다 */
		return 0;	/* [한국어] 성공으로 돌아간다 */

	down_write(&dmar_global_lock);	/* [한국어] DMAR 전역 목록을 바꾸므로 쓰기 잠금. 동시에 PCI 핫플러그 알림이 들어올 수 있다 */
	ret = dmar_dev_scope_init();	/* [한국어] 이제 PCI 열거가 끝났으니 표의 경로를 실제 struct pci_dev 와 잇는다 */
	up_write(&dmar_global_lock);	/* [한국어] 잠금 해제 */

	return ret;	/* [한국어] 연결 결과를 그대로 보고 */
}
rootfs_initcall(ir_dev_scope_init);	/* [한국어] PCI 열거가 끝난 뒤 실행되도록 등록. 부팅 초기에는 struct pci_dev 가 없어 이 연결을 할 수 없다 */

/*
 * [한국어]
 * disable_irq_remapping - 모든 유닛의 재매핑을 끈다 (코어 콜백)
 *
 * intel_irq_remap_ops.disable 로 등록되어, 서스펜드나 재매핑 재구성 전에
 * 코어가 부른다. intel_cleanup_irq_remapping() 과 달리 메모리는 그대로
 * 두고 하드웨어만 끈다 — 곧 reenable_irq_remapping() 으로 같은 표를 다시
 * 걸 것이기 때문이다.
 *
 * 포스티드 인터럽트 능력을 내리는 이유: 재매핑이 꺼진 동안에는 포스티드
 * 항목을 쓸 수 없다. 능력을 남겨 두면 그 사이 KVM 이 게스트에 직접 전달을
 * 시도해 잘못된 항목을 만들 수 있다.
 *
 * 호출 체인:
 *   irq_remapping_disable() → [이 함수] → iommu_disable_irq_remapping()
 */
static void disable_irq_remapping(void)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회용 */
	struct intel_iommu *iommu = NULL;	/* [한국어] 현재 유닛 */

	/*
	 * Disable Interrupt-remapping for all the DRHD's now.
	 */
	for_each_iommu(iommu, drhd) {	/* [한국어] 모든 유닛에서 */
		if (!ecap_ir_support(iommu->ecap))	/* [한국어] 재매핑을 지원하지 않는 유닛은 끌 것이 없다 */
			continue;	/* [한국어] 건너뛴다 */

		iommu_disable_irq_remapping(iommu);	/* [한국어] 하드웨어만 끈다. 표는 그대로 두어 reenable 이 재사용한다 */
	}

	/*
	 * Clear Posted-Interrupts capability.
	 */
	if (!disable_irq_post)	/* [한국어] 포스티드 능력을 켜 두었다면 */
		intel_irq_remap_ops.capability &= ~(1 << IRQ_POSTING_CAP);	/* [한국어] 내린다. 꺼진 동안 KVM 이 포스티드 항목을 만들지 못하게 */
}

/*
 * [한국어]
 * reenable_irq_remapping - 서스펜드에서 깨어난 뒤 재매핑을 복원한다 (코어 콜백)
 *
 * @eim: 복원할 모드. 끄기 전과 같은 값이어야 한다.
 * @return: 0 성공, -1 이면 켤 유닛이 하나도 없었다.
 *
 * 서스펜드 동안 하드웨어의 레지스터는 초기화되지만 표가 있던 메모리는
 * 그대로 남아 있다. 그래서 표를 다시 만들지 않고, 같은 표의 주소를 다시
 * 걸고(iommu_set_irq_remapping) 켜기만 하면(iommu_enable) 모든 인터럽트가
 * 예전 항목 그대로 되살아난다.
 *
 * QI 를 먼저 되살리는 이유: 표를 거는 과정에서 캐시 무효화가 필요한데 그
 * 수단이 QI 다. 순서가 뒤바뀌면 옛 캐시가 남은 채로 켜진다.
 *
 * error 경로가 사실상 비어 있는 이유: 이 지점의 실패는 "유닛이 하나도
 * 없다"뿐이고, 그것은 서스펜드 전과 하드웨어 구성이 달라졌다는 뜻이라
 * 여기서 우아하게 복구할 방법이 없다. 원 주석도 그 점을 TODO 로 남겨 두었다.
 *
 * 호출 체인:
 *   irq_remapping_reenable() (resume) → [이 함수]
 *     → dmar_reenable_qi() → iommu_set_irq_remapping() → iommu_enable_irq_remapping()
 */
static int reenable_irq_remapping(int eim)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회용 */
	bool setup = false;	/* [한국어] 되살린 유닛이 하나라도 있었는지 */
	struct intel_iommu *iommu = NULL;	/* [한국어] 현재 유닛 */

	for_each_iommu(iommu, drhd)	/* [한국어] 표를 다시 걸기 전에 모든 유닛의 QI 부터 되살린다 */
		if (iommu->qi)	/* [한국어] 서스펜드 전에 QI 가 있던 유닛은 */
			dmar_reenable_qi(iommu);	/* [한국어] 먼저 QI 를 되살린다 — 표를 거는 과정에 캐시 무효화가 필요하다 */

	/*
	 * Setup Interrupt-remapping for all the DRHD's now.
	 */
	for_each_iommu(iommu, drhd) {	/* [한국어] 유닛마다 표를 다시 걸고 켠다 */
		if (!ecap_ir_support(iommu->ecap))	/* [한국어] 재매핑을 지원하지 않는 유닛은 건너뛴다 */
			continue;	/* [한국어] 다음 유닛으로 */

		/* Set up interrupt remapping for iommu.*/
		iommu_set_irq_remapping(iommu, eim);	/* [한국어] 서스펜드 동안 살아남은 같은 표의 주소를 다시 건다 */
		iommu_enable_irq_remapping(iommu);	/* [한국어] 켜면 예전 항목 그대로 인터럽트가 되살아난다 */
		setup = true;	/* [한국어] 되살린 유닛이 있음 */
	}

	if (!setup)	/* [한국어] 유닛이 하나도 없다 — 서스펜드 전과 하드웨어 구성이 달라졌다 */
		goto error;	/* [한국어] 여기서 복구할 방법이 없다 */

	set_irq_posting_cap();	/* [한국어] 포스티드 능력을 다시 판단해 세운다 */

	return 0;	/* [한국어] 복원 성공 */

error:	/* [한국어] 되살릴 유닛이 없었던 경우 — 우아한 복구 경로는 아직 없다 */
	/*
	 * handle error condition gracefully here!
	 */
	return -1;	/* [한국어] 원 주석의 TODO — 우아한 복구 경로는 아직 없다 */
}

/*
 * Store the MSI remapping domain pointer in the device if enabled.
 *
 * This is called from dmar_pci_bus_add_dev() so it works even when DMA
 * remapping is disabled. Only update the pointer if the device is not
 * already handled by a non default PCI/MSI interrupt domain. This protects
 * e.g. VMD devices.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * intel_irq_remap_add_device - 새로 발견된 PCI 장치에 재매핑 MSI 도메인을 붙인다
 *
 * @info: PCI 버스 알림이 전한 장치 정보.
 *
 * 이 한 줄이 "장치의 MSI 가 반드시 재매핑을 거친다"를 성립시키는 지점이다.
 * 장치의 msi_domain 을 유닛의 ir_domain 으로 바꿔 두면, 이후 그 장치가
 * MSI 를 요청할 때 커널이 자동으로 재매핑 도메인을 통해 벡터를 잡는다.
 *
 * pci_dev_has_default_msi_parent_domain 검사가 왜 필요한가: 이미 다른
 * 도메인이 배정된 장치를 덮어쓰면 안 되기 때문이다. 원 주석이 드는 예가
 * VMD 로, 그 아래 장치들은 VMD 자신의 MSI 도메인을 쓴다. 기본 도메인을
 * 쓰는 장치만 우리 것으로 바꾼다.
 *
 * DMA 재매핑이 꺼져 있어도 동작한다: dmar_pci_bus_add_dev() 에서 불리므로
 * 인터럽트 재매핑만 켠 구성에서도 장치가 빠짐없이 등록된다.
 *
 * 호출 체인:
 *   dmar_pci_bus_add_dev() → [이 함수] → map_dev_to_ir()
 */
void intel_irq_remap_add_device(struct dmar_pci_notify_info *info)
{
	if (!irq_remapping_enabled || !pci_dev_has_default_msi_parent_domain(info->dev))	/* [한국어] 재매핑이 꺼져 있거나, 이미 다른 도메인이 배정된 장치(VMD 아래 등)라면 */
		return;	/* [한국어] 손대지 않는다 */

	dev_set_msi_domain(&info->dev->dev, map_dev_to_ir(info->dev));	/* [한국어] 이 장치의 MSI 부모를 담당 유닛의 재매핑 도메인으로 바꾼다 — 이후 모든 MSI 가 표를 거친다 */
}

/*
 * [한국어]
 * prepare_irte - 평범한 인터럽트 하나의 표 항목 내용을 조립한다
 *
 * @irte: 채울 소프트웨어 사본.
 * @vector: 실제로 CPU 를 깨울 벡터 번호.
 * @dest: 목적지 APIC id.
 *
 * 재매핑의 핵심 전환이 여기서 일어난다. 재매핑이 없던 시절에는 벡터와
 * 목적지가 장치의 MSI 레지스터(또는 IOAPIC RTE)에 직접 적혀 있었다. 이제는
 * 그 값이 표 항목으로 옮겨 오고, 장치에는 "표의 몇 번"이라는 인덱스만 남는다.
 *
 * trigger_mode 를 항상 edge(0)로 두는 이유는 원 주석이 설명한다: 레벨
 * 트리거 여부는 IOAPIC 의 RTE 가 관리하고, 표 항목은 목적지·벡터만 다룬다.
 * 그래야 인터럽트를 옮길 때 표 항목만 원자적으로 바꾸면 되고, IOAPIC RTE 를
 * 건드리지 않아도 되어 레벨 트리거 마이그레이션이 단순해진다.
 *
 * redir_hint 는 하위 우선순위 전달에서 하드웨어가 목적지를 고르는 것을
 * 허용하는 힌트다.
 *
 * 호출 체인:
 *   intel_irq_remapping_prepare_irte() → [이 함수]
 */
static void prepare_irte(struct irte *irte, int vector, unsigned int dest)
{
	memset(irte, 0, sizeof(*irte));	/* [한국어] 모든 필드를 0 으로 — 명시하지 않는 비트가 쓰레기 값이 되지 않게 */

	irte->present = 1;	/* [한국어] 이 항목이 유효하다. 하드웨어는 present 가 아닌 항목을 만나면 폴트를 낸다 */
	irte->dst_mode = apic->dest_mode_logical;	/* [한국어] 논리/물리 목적지 모드. 현재 APIC 드라이버가 쓰는 방식을 그대로 따른다 */
	/*
	 * Trigger mode in the IRTE will always be edge, and for IO-APIC, the
	 * actual level or edge trigger will be setup in the IO-APIC
	 * RTE. This will help simplify level triggered irq migration.
	 * For more details, see the comments (in io_apic.c) explainig IO-APIC
	 * irq migration in the presence of interrupt-remapping.
	*/
	irte->trigger_mode = 0;	/* [한국어] 항상 edge. 실제 레벨 여부는 IOAPIC RTE 가 관리한다 — 그래야 이동이 표 항목 갱신만으로 끝난다 */
	irte->dlvry_mode = APIC_DELIVERY_MODE_FIXED;	/* [한국어] 고정 전달 — 지정한 목적지로만 간다 */
	irte->vector = vector;	/* [한국어] CPU 를 깨울 실제 벡터. 이제 장치가 아니라 표가 이 값을 갖는다 */
	irte->dest_id = IRTE_DEST(dest);	/* [한국어] 목적지 APIC id. x2APIC 여부에 따라 형식이 달라진다 */
	irte->redir_hint = 1;	/* [한국어] 하위 우선순위 전달에서 하드웨어가 목적지를 고르도록 허용하는 힌트 */
}

/*
 * [한국어]
 * prepare_irte_posted - 포스티드 MSI 용 항목의 뼈대만 만든다
 *
 * @irte: 채울 소프트웨어 사본.
 *
 * 포스티드 항목에는 벡터도 목적지 APIC id 도 없다. 인터럽트를 CPU 로 곧장
 * 보내는 대신, 목적지 CPU 의 posted-interrupt 서술자에 비트를 세우고 알림
 * 벡터 하나만 보내기 때문이다. 그 서술자의 주소(pda)는 affinity 가 정해진
 * 뒤에야 알 수 있어, 여기서는 present 와 p_pst(포스티드 형식) 두 비트만
 * 세우고 나머지는 intel_ir_reconfigure_irte_posted() 가 채운다.
 *
 * 호출 체인:
 *   intel_irq_remapping_prepare_irte() (포스티드 MSI 경로) → [이 함수]
 */
static void prepare_irte_posted(struct irte *irte)
{
	memset(irte, 0, sizeof(*irte));	/* [한국어] 전 필드 초기화 */

	irte->present = 1;	/* [한국어] 항목 유효 */
	irte->p_pst = 1;	/* [한국어] 포스티드 형식임을 표시. 벡터·목적지 대신 서술자 주소를 쓰는 항목이 된다 */
}

/*
 * [한국어] struct irq_remap_ops intel_irq_remap_ops — 코어에 등록하는 콜백 표
 *
 * drivers/iommu/irq_remapping.c 의 벤더 중립 코어가 부팅 시 이 표를 통해
 * Intel 구현을 호출한다. AMD 도 같은 모양의 표를 등록하므로, 코어는 어느
 * 벤더인지 몰라도 "준비 → 켜기 → (서스펜드) 끄기 → 되살리기"의 순서만
 * 지키면 된다.
 *
 * capability 필드가 이 초기화에 없는 이유: 포스티드 인터럽트 지원 여부는
 * 하드웨어를 다 훑어 봐야 알 수 있어, set_irq_posting_cap() 이 나중에
 * 채운다.
 */
struct irq_remap_ops intel_irq_remap_ops = {
	.prepare		= intel_prepare_irq_remapping,
	/* [한국어] 켜기 전 사전 조건 확인과 표·도메인 준비.
	 * 코어가 부팅 초기에 가장 먼저 부른다. 실패하면 재매핑 없이 부팅을 이어간다. */
	.enable			= intel_enable_irq_remapping,
	/* [한국어] 준비된 유닛들의 IRE 비트를 세워 실제로 켠다.
	 * 반환값이 동작 모드라, 호출자가 그것을 보고 x2APIC 을 켤지 정한다. */
	.disable		= disable_irq_remapping,
	/* [한국어] 하드웨어만 끄고 표는 남긴다. 서스펜드 전에 불린다. */
	.reenable		= reenable_irq_remapping,
	/* [한국어] 남겨 둔 표를 다시 걸고 켠다. 레주메 경로. */
	.enable_faulting	= enable_drhd_fault_handling,
	/* [한국어] 폴트 보고 인터럽트를 켠다. dmar.c 의 공용 구현을 그대로 쓴다 —
	 *   DMA 폴트와 인터럽트 재매핑 폴트가 같은 레지스터로 보고되기 때문이다. */
};

#ifdef CONFIG_X86_POSTED_MSI	/* [한국어] 포스티드 MSI 를 켠 커널에서만 아래 구현이 존재한다. 끈 커널에는 빈 함수가 대신 놓인다 */

/*
 * [한국어]
 * get_pi_desc_addr - 이 인터럽트가 향할 CPU 의 posted-interrupt 서술자 주소
 *
 * @irqd: 대상 인터럽트.
 * @return: 그 CPU 의 서술자 물리 주소. 목적지를 정할 수 없으면 0.
 *
 * 포스티드 MSI 에서 하드웨어는 인터럽트를 "이 물리 주소의 비트맵에 표시"
 * 하는 방식으로 전달한다. 그래서 목적지 CPU 가 정해져야 항목을 완성할 수
 * 있고, effective affinity 의 첫 CPU 를 그 목적지로 삼는다.
 *
 * 물리 주소여야 하는 이유: 이 값을 읽는 것은 CPU 가 아니라 IOMMU 하드웨어라
 * 커널 가상 주소는 의미가 없다.
 *
 * WARN_ON: affinity 마스크가 비어 있으면 커널 인터럽트 계층이 이미 잘못된
 * 상태라는 뜻이라, 조용히 0 을 쓰지 않고 경고를 남긴다.
 *
 * 호출 체인:
 *   intel_ir_reconfigure_irte_posted() → [이 함수]
 */
static phys_addr_t get_pi_desc_addr(struct irq_data *irqd)
{
	int cpu = cpumask_first(irq_data_get_effective_affinity_mask(irqd));	/* [한국어] 실제로 이 인터럽트를 받게 될 첫 CPU */

	if (WARN_ON(cpu >= nr_cpu_ids))	/* [한국어] affinity 마스크가 비었다 — 인터럽트 계층이 이미 잘못된 상태 */
		return 0;	/* [한국어] 호출자가 이 값으로 실패를 판별한다 */

	return __pa(per_cpu_ptr(&posted_msi_pi_desc, cpu));	/* [한국어] 그 CPU 의 posted-interrupt 서술자를 물리 주소로. 읽는 쪽이 IOMMU 하드웨어라 가상 주소는 무의미하다 */
}

/*
 * [한국어]
 * intel_ir_reconfigure_irte_posted - 포스티드 MSI 항목의 목적지 서술자를 갱신한다
 *
 * @irqd: 옮길 인터럽트.
 *
 * 평범한 인터럽트의 이동은 "벡터와 목적지 APIC id 를 바꾼다"지만, 포스티드
 * MSI 의 이동은 "어느 CPU 의 서술자에 표시할지를 바꾼다"이다. 그래서 새
 * 목적지 CPU 의 서술자 주소를 구해 pda 필드에 넣는다.
 *
 * pda 를 두 조각(pda_l/pda_h)으로 나눠 넣는 이유: 그 필드가 128비트 항목의
 * 64비트 경계를 가로질러 놓여 있다. 이것이 modify_irte() 가 포스티드 항목에
 * cmpxchg16b 를 요구하는 근본 이유이기도 하다 — 두 조각을 따로 쓰면 그
 * 사이에 하드웨어가 반쪽짜리 주소를 읽을 수 있다.
 *
 * 임시 항목(irte_pi)에 조립하는 이유: 공유 필드는 이미 할당 때 포스티드로
 * 설정되어 있으므로 그것만 복사해 오고(dmar_copy_shared_irte), 바뀌는 pda 만
 * 새로 채운 뒤 통째로 써 넣는다. 원본 사본을 직접 고치면 중간 상태가 남는다.
 *
 * 호출 체인:
 *   __intel_ir_reconfigure_irte() → [이 함수] → get_pi_desc_addr() → modify_irte()
 */
static void intel_ir_reconfigure_irte_posted(struct irq_data *irqd)
{
	struct intel_ir_data *ir_data = irqd->chip_data;	/* [한국어] 이 인터럽트의 재매핑 상태 */
	struct irte *irte = &ir_data->irte_entry;	/* [한국어] 현재 소프트웨어 사본 */
	struct irte irte_pi;	/* [한국어] 새 내용을 조립할 임시 항목 */
	u64 pid_addr;	/* [한국어] 목적지 CPU 의 서술자 물리 주소 */

	pid_addr = get_pi_desc_addr(irqd);	/* [한국어] 새 목적지의 서술자 주소를 구한다 */

	if (!pid_addr) {	/* [한국어] 목적지를 정할 수 없다 */
		pr_warn("Failed to setup IRQ %d for posted mode", irqd->irq);	/* [한국어] 항목을 그대로 두고 경고만 남긴다 — 옛 설정이 계속 유효하다 */
		return;	/* [한국어] 반영하지 않는다 */
	}

	memset(&irte_pi, 0, sizeof(irte_pi));	/* [한국어] 임시 항목 초기화 */

	/* The shared IRTE already be set up as posted during alloc_irte */
	dmar_copy_shared_irte(&irte_pi, irte);	/* [한국어] 할당 때 이미 포스티드로 맞춰 둔 공유 필드를 그대로 가져온다 */

	irte_pi.pda_l = (pid_addr >> (32 - PDA_LOW_BIT)) & ~(-1UL << PDA_LOW_BIT);	/* [한국어] 서술자 주소의 하위 조각. 주소가 정렬돼 있어 하위 비트를 버리고 저장한다 */
	irte_pi.pda_h = (pid_addr >> 32) & ~(-1UL << PDA_HIGH_BIT);	/* [한국어] 상위 조각. 이 필드가 64비트 경계를 가로질러 있어 128비트 원자적 갱신이 필요해진다 */

	modify_irte(&ir_data->irq_2_iommu, &irte_pi);	/* [한국어] 완성된 내용을 통째로 써 넣고 캐시를 지운다 */
}

#else
/*
 * [한국어]
 * intel_ir_reconfigure_irte_posted (빈 구현) - 포스티드 MSI 를 끈 커널용
 *
 * CONFIG_X86_POSTED_MSI 가 없으면 포스티드 MSI 자체가 존재하지 않으므로
 * 아무 일도 하지 않는다. 호출자(__intel_ir_reconfigure_irte)에서 #ifdef 로
 * 갈라 쓰는 대신 빈 함수를 두어, 호출 쪽 코드를 한 가지로 유지한다.
 */
static inline void intel_ir_reconfigure_irte_posted(struct irq_data *irqd) {}
#endif

/*
 * [한국어]
 * __intel_ir_reconfigure_irte - 조립된 항목을 실제 표에 반영한다 (전달 방식별 분기)
 *
 * @irqd: 대상 인터럽트.
 * @force_host: 게스트에 직접 전달 중이어도 호스트 설정을 강제로 되돌릴지.
 *
 * 세 가지 전달 방식이 있고, 이 함수가 그 갈림길이다.
 *  - posted_vcpu: 인터럽트가 게스트 vCPU 로 직접 전달되는 중이다. 이때
 *    호스트의 affinity 가 바뀌었다고 항목을 고치면 게스트로 가던 인터럽트가
 *    끊긴다. 그래서 force_host 가 아니면 아무것도 하지 않고 돌아간다 —
 *    호스트의 affinity 변경은 게스트 전달에 아무 의미가 없기 때문이다.
 *  - posted_msi: 호스트의 포스티드 MSI. 목적지 서술자 주소를 갱신한다.
 *  - 그 밖: 평범한 항목이므로 사본을 그대로 써 넣는다.
 *
 * posted_vcpu 를 false 로 되돌리는 위치가 중요하다. 위 검사를 통과했다는
 * 것은 "게스트 전달을 그만두고 호스트 설정으로 돌아간다"는 뜻이므로,
 * 항목을 고치기 전에 상태를 먼저 정리한다.
 *
 * 호출 체인:
 *   intel_ir_reconfigure_irte()/intel_ir_set_vcpu_affinity() → [이 함수]
 *     → intel_ir_reconfigure_irte_posted()/modify_irte()
 */
static void __intel_ir_reconfigure_irte(struct irq_data *irqd, bool force_host)
{
	struct intel_ir_data *ir_data = irqd->chip_data;	/* [한국어] 이 인터럽트의 재매핑 상태 */

	/*
	 * Don't modify IRTEs for IRQs that are being posted to vCPUs if the
	 * host CPU affinity changes.
	 */
	if (ir_data->irq_2_iommu.posted_vcpu && !force_host)	/* [한국어] 게스트 vCPU 로 직접 전달 중인데 호스트 affinity 변경이라면 */
		return;	/* [한국어] 건드리지 않는다. 고치면 게스트로 가던 인터럽트가 끊긴다 */

	ir_data->irq_2_iommu.posted_vcpu = false;	/* [한국어] 여기까지 왔다는 것은 호스트 설정으로 돌아간다는 뜻이라 상태를 먼저 정리 */

	if (ir_data->irq_2_iommu.posted_msi)	/* [한국어] 호스트 포스티드 MSI 라면 */
		intel_ir_reconfigure_irte_posted(irqd);	/* [한국어] 목적지 서술자 주소를 갱신한다 */
	else
		modify_irte(&ir_data->irq_2_iommu, &ir_data->irte_entry);	/* [한국어] 평범한 항목이면 사본을 그대로 반영 */
}

/*
 * [한국어]
 * intel_ir_reconfigure_irte - 커널이 정한 새 벡터·목적지를 항목에 반영한다
 *
 * @irqd: 대상 인터럽트.
 * @force_host: 게스트 전달 중이어도 호스트 설정을 강제할지.
 *
 * 상위 계층(vector 도메인)이 이미 새 벡터와 목적지 CPU 를 정해 irq_cfg 에
 * 담아 두었다. 이 함수는 그 값을 소프트웨어 사본에 옮겨 적고 반영을
 * __intel_ir_reconfigure_irte 에 맡긴다.
 *
 * 사본을 먼저 고치는 순서가 중요하다: 하드웨어 항목은 통째로만 갈아 끼울
 * 수 있으므로, 완성된 내용을 사본에서 만들어 한 번에 써야 한다.
 *
 * 호출 체인:
 *   intel_ir_set_affinity()/activate 경로 → [이 함수] → __intel_ir_reconfigure_irte()
 */
static void intel_ir_reconfigure_irte(struct irq_data *irqd, bool force_host)
{
	struct intel_ir_data *ir_data = irqd->chip_data;	/* [한국어] 이 인터럽트의 재매핑 상태 */
	struct irte *irte = &ir_data->irte_entry;	/* [한국어] 고칠 소프트웨어 사본 */
	struct irq_cfg *cfg = irqd_cfg(irqd);	/* [한국어] 상위 vector 도메인이 정한 새 벡터·목적지 */

	/*
	 * Atomically updates the IRTE with the new destination, vector
	 * and flushes the interrupt entry cache.
	 */
	irte->vector = cfg->vector;	/* [한국어] 새 벡터를 사본에 옮긴다 */
	irte->dest_id = IRTE_DEST(cfg->dest_apicid);	/* [한국어] 새 목적지 APIC id 를 모드에 맞는 형식으로 */

	__intel_ir_reconfigure_irte(irqd, force_host);	/* [한국어] 완성된 사본을 전달 방식에 맞게 반영한다 */
}

/*
 * Migrate the IO-APIC irq in the presence of intr-remapping.
 *
 * For both level and edge triggered, irq migration is a simple atomic
 * update(of vector and cpu destination) of IRTE and flush the hardware cache.
 *
 * For level triggered, we eliminate the io-apic RTE modification (with the
 * updated vector information), by using a virtual vector (io-apic pin number).
 * Real vector that is used for interrupting cpu will be coming from
 * the interrupt-remapping table entry.
 *
 * As the migration is a simple atomic update of IRTE, the same mechanism
 * is used to migrate MSI irq's in the presence of interrupt-remapping.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * intel_ir_set_affinity - 인터럽트를 다른 CPU 로 옮긴다
 *
 * @data: 옮길 인터럽트.
 * @mask: 허용되는 목적지 CPU 집합.
 * @force: 마스크가 온라인 CPU 를 포함하지 않아도 강행할지.
 * @return: IRQ_SET_MASK_OK_DONE 이면 이 계층에서 이동이 끝났다는 뜻.
 *
 * 재매핑이 인터럽트 이동을 얼마나 단순하게 만드는지 보여 주는 함수다.
 * 재매핑이 없으면 레벨 트리거 인터럽트를 옮길 때 IOAPIC RTE 를 고쳐야 하고,
 * 그 사이에 도착한 인터럽트가 유실되거나 중복될 수 있어 정교한 회피책이
 * 필요했다. 재매핑에서는 표 항목 하나를 원자적으로 바꾸는 것으로 끝난다 —
 * 장치는 여전히 같은 인덱스를 쓰고, 목적지만 표에서 달라진다.
 *
 * 단계:
 *  1) 부모(vector 도메인)에게 새 CPU 의 벡터를 잡게 한다. 여기서 실패하면
 *     아무것도 바꾸지 않고 돌아간다.
 *  2) 표 항목을 새 벡터·목적지로 갱신한다(캐시 무효화 포함).
 *  3) 이 시점 이후의 인터럽트는 전부 새 CPU 로 간다. 그제서야 옛 벡터를
 *     정리 예약한다 — 먼저 정리하면 아직 옛 목적지로 오던 인터럽트가
 *     비어 있는 벡터에 도착한다.
 *
 * 실행 컨텍스트: 인터럽트를 끈 채 불린다. modify_irte 안에서 QI 완료를
 * 기다리므로 이동 비용의 대부분이 그 대기다.
 *
 * 호출 체인:
 *   irq_set_affinity() → [이 함수] → parent->chip->irq_set_affinity()
 *     → intel_ir_reconfigure_irte() → vector_schedule_cleanup()
 */
static int
intel_ir_set_affinity(struct irq_data *data, const struct cpumask *mask,
		      bool force)
{
	struct irq_data *parent = data->parent_data;	/* [한국어] 상위 vector 도메인의 인터럽트 데이터 */
	struct irq_cfg *cfg = irqd_cfg(data);	/* [한국어] 벡터 정보. 옛 벡터 정리에도 쓴다 */
	int ret;	/* [한국어] 부모의 판단 결과 */

	ret = parent->chip->irq_set_affinity(parent, mask, force);	/* [한국어] 먼저 새 CPU 의 벡터를 잡게 한다 */
	if (ret < 0 || ret == IRQ_SET_MASK_OK_DONE)	/* [한국어] 실패했거나 부모가 이미 전부 처리했다면 */
		return ret;	/* [한국어] 표를 건드릴 것이 없다 */

	intel_ir_reconfigure_irte(data, false);	/* [한국어] 표 항목을 새 벡터·목적지로 원자적으로 갱신 + 캐시 무효화 */
	/*
	 * After this point, all the interrupts will start arriving
	 * at the new destination. So, time to cleanup the previous
	 * vector allocation.
	 */
	vector_schedule_cleanup(cfg);	/* [한국어] 이제 모든 인터럽트가 새 CPU 로 가므로 옛 벡터를 정리 예약한다. 먼저 정리하면 늦게 온 인터럽트가 빈 벡터에 도착한다 */

	return IRQ_SET_MASK_OK_DONE;	/* [한국어] 이 계층에서 이동이 완료됐음을 알린다 */
}

/*
 * [한국어]
 * intel_ir_compose_msi_msg - 장치에 써 넣을 MSI 주소·데이터를 돌려준다
 *
 * @irq_data: 대상 인터럽트.
 * @msg: 결과를 담을 곳.
 *
 * 재매핑에서 이 값은 할당 시점에 정해져 이후 절대 바뀌지 않는다. 표
 * 인덱스만 담고 있고 목적지·벡터는 표가 관리하기 때문이다. 그래서 인터럽트를
 * 다른 CPU 로 옮겨도 장치의 MSI 레지스터는 손댈 필요가 없다 — 이것이
 * 재매핑이 인터럽트 이동을 단순하게 만드는 이유의 절반이다.
 *
 * 그 결과 이 함수는 미리 만들어 둔 msi_entry 를 그대로 복사하기만 한다.
 *
 * 호출 체인:
 *   msi_domain_activate 경로 → [이 함수]
 */
static void intel_ir_compose_msi_msg(struct irq_data *irq_data,
				     struct msi_msg *msg)
{
	struct intel_ir_data *ir_data = irq_data->chip_data;	/* [한국어] 이 인터럽트의 재매핑 상태 */

	*msg = ir_data->msi_entry;	/* [한국어] 할당 때 만들어 둔 값을 그대로. 표 인덱스만 담고 있어 CPU 를 옮겨도 바뀌지 않는다 */
}

/*
 * [한국어]
 * intel_ir_set_vcpu_affinity - 인터럽트를 게스트 vCPU 로 직접 전달하도록 전환한다
 *
 * @data: 전환할 인터럽트.
 * @info: KVM 이 전한 intel_iommu_pi_data. NULL 이면 "그만두고 호스트로 돌아가라".
 * @return: 항상 0.
 *
 * 가상화 성능의 핵심 기능이다. 보통은 장치 인터럽트가 호스트 CPU 를 깨우고,
 * 호스트가 그것을 게스트에 다시 주입한다 — VM exit 이 한 번 든다. 포스티드
 * 인터럽트를 쓰면 하드웨어가 게스트 vCPU 의 서술자에 직접 표시하므로 그
 * exit 이 사라진다.
 *
 * 두 방향:
 *  - info 가 NULL: 게스트가 장치를 놓았거나 vCPU 가 내려갔다. 캐시해 둔
 *    평범한 항목으로 되돌린다(force_host).
 *  - info 가 있음: 게스트 벡터와 vCPU 서술자 주소로 포스티드 항목을 만든다.
 *
 * 원 주석이 밝히듯 포스티드 항목은 캐시하지 않는다. irte_entry 에는 늘
 * 호스트용 평범한 항목이 남아 있고, 게스트 모드에서는 그것에서 공통 필드만
 * 복사해 임시 항목을 조립한다. 그래서 되돌아올 때 다시 만들 필요 없이
 * 보관해 둔 항목을 그대로 쓸 수 있다.
 *
 * posted_vcpu 를 세우는 것이 중요한 이유: 이 표시가 있으면 호스트의
 * affinity 변경이 이 항목을 건드리지 않는다. 호스트가 인터럽트를 다른 CPU 로
 * 옮긴다고 게스트로 가던 인터럽트의 목적지를 바꿔서는 안 되기 때문이다.
 *
 * 실행 컨텍스트: KVM 이 장치를 게스트에 넘기거나 회수할 때. modify_irte 가
 * 128비트 원자적 교체를 쓰므로 하드웨어와의 경쟁에도 안전하다.
 *
 * 호출 체인:
 *   KVM (irq_set_vcpu_affinity) → [이 함수] → modify_irte()
 *     또는 __intel_ir_reconfigure_irte()
 */
static int intel_ir_set_vcpu_affinity(struct irq_data *data, void *info)
{
	struct intel_ir_data *ir_data = data->chip_data;	/* [한국어] 이 인터럽트의 재매핑 상태 */
	struct intel_iommu_pi_data *pi_data = info;	/* [한국어] KVM 이 전한 게스트 전달 정보. NULL 이면 "그만두라"는 뜻 */

	/* stop posting interrupts, back to the default mode */
	if (!pi_data) {	/* [한국어] 게스트 전달을 멈추고 호스트 모드로 돌아간다 */
		__intel_ir_reconfigure_irte(data, true);	/* [한국어] force_host 로 강제 반영 — 캐시해 둔 평범한 항목이 되살아난다 */
	} else {
		struct irte irte_pi;	/* [한국어] 포스티드 내용을 조립할 임시 항목 */

		/*
		 * We are not caching the posted interrupt entry. We
		 * copy the data from the remapped entry and modify
		 * the fields which are relevant for posted mode. The
		 * cached remapped entry is used for switching back to
		 * remapped mode.
		 */
		memset(&irte_pi, 0, sizeof(irte_pi));	/* [한국어] 초기화 */
		dmar_copy_shared_irte(&irte_pi, &ir_data->irte_entry);	/* [한국어] 소스 검증 등 공통 필드는 기존 항목에서 가져온다 */

		/* Update the posted mode fields */
		irte_pi.p_pst = 1;	/* [한국어] 포스티드 형식으로 표시 */
		irte_pi.p_urgent = 0;	/* [한국어] 긴급 전달 아님 — 게스트가 폴링할 때까지 기다려도 된다 */
		irte_pi.p_vector = pi_data->vector;	/* [한국어] 게스트 안에서 쓰일 벡터 번호. 호스트 벡터와는 별개다 */
		irte_pi.pda_l = (pi_data->pi_desc_addr >>	/* [한국어] 게스트 vCPU 의 서술자 주소 하위 조각 */
				(32 - PDA_LOW_BIT)) & ~(-1UL << PDA_LOW_BIT);
		irte_pi.pda_h = (pi_data->pi_desc_addr >> 32) &	/* [한국어] 상위 조각. 64비트 경계를 가로질러 있어 128비트 원자적 갱신이 필요하다 */
				~(-1UL << PDA_HIGH_BIT);

		ir_data->irq_2_iommu.posted_vcpu = true;	/* [한국어] 이제 호스트 affinity 변경이 이 항목을 건드리지 못하게 표시 */
		modify_irte(&ir_data->irq_2_iommu, &irte_pi);	/* [한국어] 통째로 써 넣는다. 원 주석대로 이 내용은 캐시하지 않는다 — 되돌릴 때는 보관해 둔 평범한 항목을 쓴다 */
	}

	return 0;	/* [한국어] 전환 완료 */
}

/*
 * [한국어] struct irq_chip intel_ir_chip — 재매핑된 인터럽트 하나의 조작 방법
 *
 * 커널 인터럽트 계층은 인터럽트마다 "칩"을 붙여 ack/mask/affinity 를 맡긴다.
 * 재매핑 도메인이 붙인 이 칩이 하는 일은 결국 "표 항목을 고치는 것"으로
 * 귀결된다 — 목적지를 바꾸든 게스트에 넘기든, 장치의 MSI 레지스터는 그대로
 * 두고 표만 바꾼다.
 *
 * mask/unmask 가 없는 이유: 차단은 상위 MSI 계층이 장치 쪽에서 처리하고,
 * 이 계층은 목적지 변환만 책임진다.
 */
static struct irq_chip intel_ir_chip = {
	.name			= "INTEL-IR",
	/* [한국어] /proc/interrupts 에 보이는 칩 이름. */
	.irq_ack		= apic_ack_irq,
	/* [한국어] 인터럽트 수신 확인. 평범한 경로라 APIC 에 EOI 를 보낸다. */
	.irq_set_affinity	= intel_ir_set_affinity,
	/* [한국어] 목적지 CPU 변경. 표 항목만 고치면 되므로 장치는 건드리지 않는다. */
	.irq_compose_msi_msg	= intel_ir_compose_msi_msg,
	/* [한국어] 장치에 써 넣을 MSI 주소·데이터. 표 인덱스만 담아 이동해도 바뀌지 않는다. */
	.irq_set_vcpu_affinity	= intel_ir_set_vcpu_affinity,
	/* [한국어] 게스트 vCPU 직접 전달로 전환하거나 되돌린다. KVM 이 호출한다. */
};

/*
 * With posted MSIs, the MSI vectors are multiplexed into a single notification
 * vector, and only the notification vector is sent to the APIC IRR.  Device
 * MSIs are then dispatched in a demux loop that harvests the MSIs from the
 * CPU's Posted Interrupt Request bitmap.  I.e. Posted MSIs never get sent to
 * the APIC IRR, and thus do not need an EOI.  The notification handler instead
 * performs a single EOI after processing the PIR.
 *
 * Note!  Pending SMP/CPU affinity changes, which are per MSI, must still be
 * honored, only the APIC EOI is omitted.
 *
 * For the example below, 3 MSIs are coalesced into one CPU notification. Only
 * one apic_eoi() is needed, but each MSI needs to process pending changes to
 * its CPU affinity.
 *
 * __sysvec_posted_msi_notification()
 *	irq_enter();
 *		handle_edge_irq()
 *			irq_chip_ack_parent()
 *				intel_ack_posted_msi_irq(); // No EOI
 *			handle_irq_event()
 *				driver_handler()
 *		handle_edge_irq()
 *			irq_chip_ack_parent()
 *				intel_ack_posted_msi_irq(); // No EOI
 *			handle_irq_event()
 *				driver_handler()
 *		handle_edge_irq()
 *			irq_chip_ack_parent()
 *				intel_ack_posted_msi_irq(); // No EOI
 *			handle_irq_event()
 *				driver_handler()
 *	apic_eoi()
 *	irq_exit()
 *
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct irq_chip intel_ir_chip_post_msi — 포스티드 MSI 전용 칩
 *
 * intel_ir_chip 과 단 하나, irq_ack 만 다르다. 포스티드 MSI 는 여러 장치
 * 인터럽트가 알림 벡터 하나로 합쳐져 오고, APIC 에는 그 알림 벡터 하나만
 * 도착한다. 그래서 개별 MSI 마다 EOI 를 보내면 있지도 않은 인터럽트를
 * 종료하는 셈이 된다. 위 영어 주석의 흐름도가 보여 주듯, EOI 는 알림 핸들러가
 * PIR 비트맵을 모두 처리한 뒤 한 번만 보낸다.
 *
 * 나머지 콜백이 같은 이유: affinity 변경이나 MSI 메시지 조립은 포스티드
 * 여부와 무관하게 같은 방식으로 동작한다.
 */
static struct irq_chip intel_ir_chip_post_msi = {
	.name			= "INTEL-IR-POST",
	/* [한국어] 포스티드 MSI 임을 이름으로 구분한다. */
	.irq_ack		= intel_ack_posted_msi_irq,
	/* [한국어] EOI 를 보내지 않는 ack. 알림 벡터 하나에 여러 MSI 가 합쳐져 오므로,
	 *   EOI 는 알림 핸들러가 PIR 을 다 처리한 뒤 한 번만 보낸다.
	 *   이 한 줄이 intel_ir_chip 과의 유일한 차이다. */
	.irq_set_affinity	= intel_ir_set_affinity,
	.irq_compose_msi_msg	= intel_ir_compose_msi_msg,
	.irq_set_vcpu_affinity	= intel_ir_set_vcpu_affinity,
};

/*
 * [한국어]
 * fill_msi_msg - 장치에 써 넣을 MSI 주소·데이터를 "표 인덱스" 형식으로 만든다
 *
 * @msg: 채울 MSI 메시지.
 * @index: 이 인터럽트가 쓰는 표 항목 인덱스.
 * @subhandle: 다중 벡터 MSI 에서 묶음 안의 위치.
 *
 * 재매핑의 형식 전환이 눈에 보이는 곳이다. 재매핑이 없으면 MSI 주소에는
 * 목적지 APIC id 가, 데이터에는 벡터 번호가 들어간다. 재매핑에서는 주소에
 * dmar_format 비트를 세우고 그 자리에 표 인덱스를 넣는다 — 하드웨어는 이
 * 비트를 보고 "이건 목적지가 아니라 표를 찾으라는 뜻"으로 해석한다.
 *
 * 인덱스를 0~14 비트와 15 비트로 쪼개 넣는 이유: MSI 주소의 그 위치에
 * 연속된 16비트 자리가 없어서, 스펙이 정한 두 조각에 나눠 담는다.
 *
 * subhandle 이 데이터 쪽에 가는 이유: 다중 벡터 MSI 장치는 데이터의 하위
 * 비트만 바꿔 가며 여러 벡터를 낸다. 그 값이 그대로 묶음 안의 위치가 되어,
 * 하드웨어가 index + subhandle 번 항목을 찾는다.
 *
 * 호출 체인:
 *   intel_irq_remapping_prepare_irte() → [이 함수]
 */
static void fill_msi_msg(struct msi_msg *msg, u32 index, u32 subhandle)
{
	memset(msg, 0, sizeof(*msg));	/* [한국어] 모든 필드 초기화 */

	msg->arch_addr_lo.dmar_base_address = X86_MSI_BASE_ADDRESS_LOW;	/* [한국어] MSI 가 향할 고정 주소 영역. 이 주소로의 쓰기를 하드웨어가 인터럽트로 해석한다 */
	msg->arch_addr_lo.dmar_subhandle_valid = true;	/* [한국어] 데이터 쪽 subhandle 을 의미 있게 해석하라는 표시 */
	msg->arch_addr_lo.dmar_format = true;	/* [한국어] "이 값은 목적지가 아니라 표 인덱스다" — 재매핑 형식임을 알리는 핵심 비트 */
	msg->arch_addr_lo.dmar_index_0_14 = index & 0x7FFF;	/* [한국어] 인덱스의 하위 15비트 */
	msg->arch_addr_lo.dmar_index_15 = !!(index & 0x8000);	/* [한국어] 16번째 비트는 따로 떨어진 자리에 — 연속된 16비트 공간이 없다 */

	msg->address_hi = X86_MSI_BASE_ADDRESS_HIGH;	/* [한국어] 주소 상위. 고정값이다 */

	msg->arch_data.dmar_subhandle = subhandle;	/* [한국어] 묶음 안의 위치. 다중 벡터 장치는 이 값을 바꿔 가며 여러 인터럽트를 낸다 */
}

/*
 * [한국어]
 * intel_irq_remapping_prepare_irte - 인터럽트 종류별로 항목과 MSI 메시지를 완성한다
 *
 * @data: 이 인터럽트의 재매핑 상태(항목 사본과 MSI 메시지를 담는다).
 * @irq_cfg: 상위 계층이 정한 벡터와 목적지.
 * @info: 어떤 종류의 인터럽트인지(IOAPIC/HPET/MSI/MSI-X)와 그 부가 정보.
 * @index: 할당받은 표 항목 인덱스.
 * @sub_handle: 묶음 안의 위치.
 *
 * 공통 부분(prepare_irte)을 먼저 채우고, 종류에 따라 다른 두 가지를 더한다.
 *  - 소스 검증 규칙: IOAPIC/HPET 은 표에서 받아 둔 소스 id 를, PCI 는 DMA
 *    별칭을 분석해 정한다. 이것 없이는 아무 장치나 이 항목을 쓸 수 있다.
 *  - IOAPIC 의 sub_handle 대체: IOAPIC 은 MSI 묶음이 아니라 핀 번호로
 *    구분되므로, 인자로 받은 값 대신 핀 번호를 쓴다.
 *
 * 포스티드 MSI 분기: 시스템이 포스티드를 쓰기로 했다면 항목을 포스티드
 * 형식으로 다시 뼈대만 세우고 posted_msi 를 표시한다. 목적지 서술자 주소는
 * affinity 가 정해진 뒤 activate 경로에서 채워진다.
 *
 * default 의 BUG_ON: 이 도메인에 도달할 수 있는 인터럽트 종류는 위 네 가지뿐이다.
 * 그 밖이 오면 커널 인터럽트 계층의 배선이 잘못된 것이라 조용히 넘어가지 않는다.
 *
 * 마지막 fill_msi_msg 가 "장치에 무엇을 써 넣을지"를 확정한다 — 목적지가
 * 아니라 표 인덱스다.
 *
 * 호출 체인:
 *   intel_irq_remapping_alloc() → [이 함수] → prepare_irte()/set_*_sid()/fill_msi_msg()
 */
static void intel_irq_remapping_prepare_irte(struct intel_ir_data *data,
					     struct irq_cfg *irq_cfg,
					     struct irq_alloc_info *info,
					     int index, int sub_handle)
{
	struct irte *irte = &data->irte_entry;	/* [한국어] 조립할 소프트웨어 사본 */

	prepare_irte(irte, irq_cfg->vector, irq_cfg->dest_apicid);	/* [한국어] 종류와 무관한 공통 필드부터 채운다 */

	switch (info->type) {	/* [한국어] 인터럽트 종류에 따라 소스 검증 방법이 갈린다 */
	case X86_IRQ_ALLOC_TYPE_IOAPIC:	/* [한국어] IOAPIC 인터럽트 */
		/* Set source-id of interrupt request */
		set_ioapic_sid(irte, info->devid);	/* [한국어] 표에서 받아 둔 그 IOAPIC 의 소스 id 를 새긴다 */
		apic_pr_verbose("IOAPIC[%d]: Set IRTE entry (P:%d FPD:%d Dst_Mode:%d Redir_hint:%d Trig_Mode:%d Dlvry_Mode:%X Avail:%X Vector:%02X Dest:%08X SID:%04X SQ:%X SVT:%X)\n",	/* [한국어] 디버깅용 — 완성된 항목의 모든 필드를 남긴다 */
				info->devid, irte->present, irte->fpd, irte->dst_mode,
				irte->redir_hint, irte->trigger_mode, irte->dlvry_mode,
				irte->avail, irte->vector, irte->dest_id, irte->sid,
				irte->sq, irte->svt);
		sub_handle = info->ioapic.pin;	/* [한국어] IOAPIC 은 MSI 묶음이 아니라 핀 번호로 구분되므로 인자를 덮어쓴다 */
		break;	/* [한국어] IOAPIC 처리 끝 */
	case X86_IRQ_ALLOC_TYPE_HPET:	/* [한국어] HPET 인터럽트 */
		set_hpet_sid(irte, info->devid);	/* [한국어] 그 HPET 의 소스 id 를 (느슨한 규칙으로) 새긴다 */
		break;	/* [한국어] HPET 처리 끝 */
	case X86_IRQ_ALLOC_TYPE_PCI_MSI:	/* [한국어] PCI MSI */
	case X86_IRQ_ALLOC_TYPE_PCI_MSIX:	/* [한국어] PCI MSI-X — 소스 검증 방법은 같다 */
		if (posted_msi_enabled()) {	/* [한국어] 시스템이 포스티드 MSI 를 쓰기로 했다면 */
			prepare_irte_posted(irte);	/* [한국어] 항목을 포스티드 형식의 뼈대로 다시 세운다 */
			data->irq_2_iommu.posted_msi = 1;	/* [한국어] 이후 갱신 경로가 서술자 주소를 채우도록 표시 */
		}

		set_msi_sid(irte,	/* [한국어] DMA 별칭을 분석해 알맞은 검증 규칙을 고른다 */
			    pci_real_dma_dev(msi_desc_to_pci_dev(info->desc)));	/* [한국어] MSI 서술자에서 장치를 얻고, DMA 를 실제로 내는 장치로 보정한다 */
		break;	/* [한국어] PCI 처리 끝 */
	default:	/* [한국어] 이 도메인에 올 수 없는 종류 */
		BUG_ON(1);	/* [한국어] 인터럽트 계층의 배선이 잘못된 것이라 조용히 넘어가지 않는다 */
		break;
	}
	fill_msi_msg(&data->msi_entry, index, sub_handle);	/* [한국어] 장치에 써 넣을 값을 확정한다 — 목적지가 아니라 표 인덱스다 */
}

/*
 * [한국어]
 * intel_free_irq_resources - 인터럽트들의 표 항목과 chip_data 를 놓는다
 *
 * @domain: 이 인터럽트들이 속한 재매핑 도메인.
 * @virq: 첫 가상 인터럽트 번호.
 * @nr_irqs: 개수.
 *
 * 해제 순서가 중요하다. 먼저 표 항목을 지우고(clear_entries) 캐시를 무효화한
 * 다음에야 chip_data 를 놓는다. 반대로 하면 하드웨어가 아직 캐시에 든 항목으로
 * 인터럽트를 보내는데 그것을 받을 소프트웨어 상태가 이미 사라진 상태가 된다.
 *
 * clear_entries 를 락 안에서 부르는 이유: 표와 비트맵을 동시에 건드리므로
 * 할당 경로와 경쟁하면 같은 자리를 두 인터럽트가 차지할 수 있다.
 *
 * irq_domain_reset_irq_data 로 chip/chip_data 를 끊은 뒤에 kfree 하는 것도
 * 같은 이유다 — 끊기 전에 놓으면 잠깐이나마 해제된 메모리를 가리키는
 * 포인터가 남는다.
 *
 * 할당 실패 되감기에서도 쓰이므로, chip_data 가 아직 없는 인터럽트를 만나면
 * 조용히 건너뛴다.
 *
 * 호출 체인:
 *   intel_irq_remapping_free()/alloc 실패 경로 → [이 함수] → clear_entries()
 */
static void intel_free_irq_resources(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *irq_data;	/* [한국어] 현재 인터럽트의 커널 데이터 */
	struct intel_ir_data *data;	/* [한국어] 그 인터럽트의 재매핑 상태 */
	struct irq_2_iommu *irq_iommu;	/* [한국어] 표 항목 연결 정보 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */
	int i;	/* [한국어] 순회 인덱스 */
	for (i = 0; i < nr_irqs; i++) {	/* [한국어] 요청된 개수만큼 */
		irq_data = irq_domain_get_irq_data(domain, virq  + i);	/* [한국어] 이 도메인 계층에서의 인터럽트 데이터 */
		if (irq_data && irq_data->chip_data) {	/* [한국어] 할당 실패 되감기에서는 아직 없을 수 있다 */
			data = irq_data->chip_data;	/* [한국어] 재매핑 상태 */
			irq_iommu = &data->irq_2_iommu;	/* [한국어] 표 항목 위치 */
			raw_spin_lock_irqsave(&irq_2_ir_lock, flags);	/* [한국어] 표와 비트맵을 할당 경로와의 경쟁으로부터 지킨다 */
			clear_entries(irq_iommu);	/* [한국어] 항목을 비우고 비트맵에 반환하고 캐시를 무효화한다 */
			raw_spin_unlock_irqrestore(&irq_2_ir_lock, flags);	/* [한국어] 해제 끝 */
			irq_domain_reset_irq_data(irq_data);	/* [한국어] chip/chip_data 연결을 먼저 끊는다 */
			kfree(data);	/* [한국어] 그다음에야 놓는다 — 순서를 바꾸면 해제된 메모리를 가리키는 창이 생긴다 */
		}
	}
}

/*
 * [한국어]
 * intel_irq_remapping_alloc - 인터럽트 nr_irqs 개를 재매핑 도메인에서 잡는다
 *
 * @domain: 이 유닛의 재매핑 도메인.
 * @virq: 커널이 배정한 첫 가상 인터럽트 번호.
 * @nr_irqs: 요청 개수. 1보다 크면 다중 벡터 MSI 다.
 * @arg: irq_alloc_info — 인터럽트의 종류와 장치 정보.
 * @return: 0 성공, 음수면 실패(되감기까지 마친 상태).
 *
 * 계층 구조의 한 단계다. 먼저 부모(vector 도메인)에게 실제 CPU 벡터를 잡게
 * 하고, 그 위에 재매핑 표 항목을 얹는다. 순서가 이래야 하는 이유: 표 항목에
 * 넣을 벡터·목적지를 부모가 정해 주기 때문이다.
 *
 * 다중 벡터의 구조: 표 항목은 alloc_irte 한 번으로 묶음 전체를 잡지만,
 * intel_ir_data 는 인터럽트마다 하나씩 필요하다(커널이 인터럽트별로
 * chip_data 를 요구한다). 그래서 첫 개는 이미 만든 data 를 쓰고, 나머지는
 * 같은 묶음 정보를 복사한 뒤 sub_handle 만 다르게 준다.
 *
 * hwirq 를 (index << 16) + i 로 만드는 이유: 도메인 안에서 인터럽트를
 * 유일하게 식별해야 하는데, 표 인덱스와 묶음 안 위치를 합치면 그 조건을
 * 만족하면서 디버깅 때 원래 항목을 바로 알 수 있다.
 *
 * 칩 선택: 포스티드 MSI 로 갈 PCI MSI/MSI-X 는 EOI 를 보내지 않는 전용 칩을,
 * 그 밖은 평범한 칩을 붙인다.
 *
 * 에러 되감기: 표 항목·chip_data 는 intel_free_irq_resources 가, 부모가 잡은
 * 벡터는 irq_domain_free_irqs_common 이 놓는다. i 를 개수로 넘겨 "지금까지
 * 성공한 것만" 정확히 되돌린다.
 *
 * 호출 체인:
 *   msi_domain_alloc_irqs()/ioapic 설정 → [이 함수]
 *     → irq_domain_alloc_irqs_parent() → alloc_irte()
 *     → intel_irq_remapping_prepare_irte()
 */
static int intel_irq_remapping_alloc(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs,
				     void *arg)
{
	struct intel_iommu *iommu = domain->host_data;	/* [한국어] 도메인 생성 때 실어 둔 유닛 포인터 */
	struct irq_alloc_info *info = arg;	/* [한국어] 인터럽트 종류와 장치 정보 */
	struct intel_ir_data *data, *ird;	/* [한국어] 첫 인터럽트의 상태와, 반복에서 쓰는 현재 상태 */
	struct irq_data *irq_data;	/* [한국어] 커널 인터럽트 데이터 */
	struct irq_cfg *irq_cfg;	/* [한국어] 부모가 정한 벡터·목적지 */
	int i, ret, index;	/* [한국어] 순회 인덱스, 결과, 표 인덱스 */

	if (!info || !iommu)	/* [한국어] 필수 정보가 없으면 진행할 수 없다 */
		return -EINVAL;	/* [한국어] 잘못된 호출 */
	if (nr_irqs > 1 && info->type != X86_IRQ_ALLOC_TYPE_PCI_MSI)	/* [한국어] 여러 개를 한 번에 잡는 것은 다중 벡터 MSI 뿐이다 */
		return -EINVAL;	/* [한국어] MSI-X 나 IOAPIC 은 하나씩 잡는다 */

	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, arg);	/* [한국어] 먼저 부모에게 실제 CPU 벡터를 잡게 한다 — 표에 넣을 값이 거기서 나온다 */
	if (ret < 0)	/* [한국어] 벡터가 부족하다 */
		return ret;	/* [한국어] 아직 아무것도 잡지 않았으므로 그대로 실패 */

	ret = -ENOMEM;	/* [한국어] 이후 실패의 기본 코드 */
	data = kzalloc_obj(*data);	/* [한국어] 첫 인터럽트의 재매핑 상태 */
	if (!data)	/* [한국어] 할당 실패 */
		goto out_free_parent;	/* [한국어] 부모의 벡터를 되돌린다 */

	index = alloc_irte(iommu, &data->irq_2_iommu, nr_irqs);	/* [한국어] 표에서 연속·정렬된 묶음을 한 번에 잡는다 */
	if (index < 0) {	/* [한국어] 표가 꽉 찼거나 요청이 너무 크다 */
		pr_warn("Failed to allocate IRTE\n");	/* [한국어] 알리고 */
		kfree(data);	/* [한국어] 방금 만든 상태를 놓고 */
		goto out_free_parent;	/* [한국어] 부모의 벡터도 되돌린다 */
	}

	for (i = 0; i < nr_irqs; i++) {	/* [한국어] 묶음 안의 인터럽트를 하나씩 배선한다 */
		irq_data = irq_domain_get_irq_data(domain, virq + i);	/* [한국어] 이 계층의 인터럽트 데이터 */
		irq_cfg = irqd_cfg(irq_data);	/* [한국어] 부모가 정한 벡터·목적지 */
		if (!irq_data || !irq_cfg) {	/* [한국어] 계층 구조가 예상과 다르다 */
			if (!i)	/* [한국어] 첫 번째에서 실패했다면 아직 어디에도 연결되지 않은 data 가 남는다 */
				kfree(data);	/* [한국어] 직접 놓는다 */
			ret = -EINVAL;	/* [한국어] 배선 오류 */
			goto out_free_data;	/* [한국어] 지금까지 성공한 것만 되돌린다 */
		}

		if (i > 0) {	/* [한국어] 두 번째부터는 인터럽트마다 별도의 chip_data 가 필요하다 */
			ird = kzalloc_obj(*ird);	/* [한국어] 새로 만들고 */
			if (!ird)	/* [한국어] 실패하면 */
				goto out_free_data;	/* [한국어] 앞의 것들을 되돌린다 */
			/* Initialize the common data */
			ird->irq_2_iommu = data->irq_2_iommu;	/* [한국어] 같은 묶음 정보를 복사 */
			ird->irq_2_iommu.sub_handle = i;	/* [한국어] 묶음 안의 위치만 다르게 — 실제 항목은 index + i 번이 된다 */
		} else {
			ird = data;	/* [한국어] 첫 번째는 이미 만든 것을 그대로 쓴다 */
		}

		irq_data->hwirq = (index << 16) + i;	/* [한국어] 도메인 안의 유일한 식별자. 표 인덱스와 묶음 위치를 합쳐 만든다 */
		irq_data->chip_data = ird;	/* [한국어] 커널이 이 인터럽트를 다룰 때 찾아올 상태 */
		if (posted_msi_enabled() &&	/* [한국어] 포스티드 MSI 를 쓰는 시스템이고 */
		    ((info->type == X86_IRQ_ALLOC_TYPE_PCI_MSI) ||	/* [한국어] PCI MSI 이거나 */
		     (info->type == X86_IRQ_ALLOC_TYPE_PCI_MSIX)))	/* [한국어] MSI-X 라면 */
			irq_data->chip = &intel_ir_chip_post_msi;	/* [한국어] EOI 를 보내지 않는 전용 칩 */
		else
			irq_data->chip = &intel_ir_chip;	/* [한국어] 그 밖은 평범한 칩 */
		intel_irq_remapping_prepare_irte(ird, irq_cfg, info, index, i);	/* [한국어] 항목 내용과 MSI 메시지를 완성한다. 하드웨어 반영은 activate 에서 */
	}
	return 0;	/* [한국어] 모든 인터럽트가 배선됐다 */

out_free_data:	/* [한국어] 일부 인터럽트까지 배선한 뒤 실패한 경우의 되감기 */
	intel_free_irq_resources(domain, virq, i);	/* [한국어] i 개만 — 지금까지 성공한 것만 정확히 되돌린다 */
out_free_parent:	/* [한국어] 부모의 벡터만 잡은 상태에서 실패한 경우 */
	irq_domain_free_irqs_common(domain, virq, nr_irqs);	/* [한국어] 부모가 잡은 벡터를 놓는다 */
	return ret;	/* [한국어] 실패 원인을 그대로 보고 */
}

/*
 * [한국어]
 * intel_irq_remapping_free - 인터럽트들을 해제한다 (도메인 콜백)
 *
 * @domain: 재매핑 도메인.
 * @virq: 첫 가상 인터럽트 번호.
 * @nr_irqs: 개수.
 *
 * alloc 이 쌓은 두 층을 역순으로 허문다: 이 계층의 표 항목과 chip_data 를
 * 먼저 놓고, 그다음 부모가 잡은 CPU 벡터를 놓는다. 순서를 바꾸면 벡터가
 * 사라진 뒤에도 표 항목이 그 벡터를 가리키는 창이 생긴다.
 *
 * 호출 체인:
 *   irq_domain_free_irqs() → [이 함수] → intel_free_irq_resources()
 */
static void intel_irq_remapping_free(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs)
{
	intel_free_irq_resources(domain, virq, nr_irqs);	/* [한국어] 먼저 이 계층의 표 항목과 chip_data 를 놓고 */
	irq_domain_free_irqs_common(domain, virq, nr_irqs);	/* [한국어] 그다음 부모의 벡터를 놓는다. 순서를 바꾸면 없는 벡터를 가리키는 항목이 남는다 */
}

/*
 * [한국어]
 * intel_irq_remapping_activate - 인터럽트를 실제로 쓸 수 있게 만든다 (도메인 콜백)
 *
 * @domain: 재매핑 도메인.
 * @irq_data: 활성화할 인터럽트.
 * @reserve: 예약만 하는 호출인지(여기서는 쓰지 않는다).
 * @return: 항상 0 — 이 단계에서 실패할 일이 없다.
 *
 * 할당 때는 표 항목의 내용을 소프트웨어 사본에만 만들어 두었다. 실제로
 * 하드웨어 표에 써 넣는 것은 이 시점이다. 그래야 아직 쓰이지 않는 인터럽트가
 * present 인 항목을 차지하지 않는다.
 *
 * force_host=true 인 이유: 활성화는 호스트가 이 인터럽트를 쓰겠다는 선언이라,
 * 게스트 전달 상태가 남아 있더라도 호스트 설정으로 확실히 되돌려야 한다.
 *
 * 호출 체인:
 *   irq_domain_activate_irq() → [이 함수] → intel_ir_reconfigure_irte()
 */
static int intel_irq_remapping_activate(struct irq_domain *domain,
					struct irq_data *irq_data, bool reserve)
{
	intel_ir_reconfigure_irte(irq_data, true);	/* [한국어] 준비된 사본을 하드웨어 표에 실제로 써 넣는다. force_host 로 게스트 상태를 확실히 되돌린다 */
	return 0;	/* [한국어] 자원 확보는 alloc 에서 끝났으므로 여기서는 실패하지 않는다 */
}

/*
 * [한국어]
 * intel_irq_remapping_deactivate - 표 항목을 비워 인터럽트를 끊는다 (도메인 콜백)
 *
 * @domain: 재매핑 도메인.
 * @irq_data: 비활성화할 인터럽트.
 *
 * 0 으로 채운 항목을 써 넣어 present 를 내린다. 이 순간부터 이 인덱스로
 * 오는 인터럽트는 하드웨어에서 폴트가 되어 CPU 에 도달하지 않는다 — 아직
 * 항목 자리는 잡고 있으므로 재활성화하면 그대로 되살아난다.
 *
 * WARN_ON_ONCE(posted_vcpu): 게스트에 직접 전달 중인 인터럽트를 비활성화하는
 * 것은 KVM 이 먼저 정리했어야 할 순서를 어긴 것이다. 그래도 진행은 해야
 * 하므로 경고를 남기고 상태를 정리한 뒤 항목을 비운다.
 *
 * 호출 체인:
 *   irq_domain_deactivate_irq() → [이 함수] → modify_irte()
 */
static void intel_irq_remapping_deactivate(struct irq_domain *domain,
					   struct irq_data *irq_data)
{
	struct intel_ir_data *data = irq_data->chip_data;	/* [한국어] 이 인터럽트의 재매핑 상태 */
	struct irte entry;	/* [한국어] 0 으로 채워 써 넣을 빈 항목 */

	WARN_ON_ONCE(data->irq_2_iommu.posted_vcpu);	/* [한국어] 게스트 전달 중인데 비활성화 — KVM 이 먼저 정리했어야 할 순서를 어긴 것 */
	data->irq_2_iommu.posted_vcpu = false;	/* [한국어] 그래도 진행해야 하므로 상태를 정리하고 */

	memset(&entry, 0, sizeof(entry));	/* [한국어] 빈 항목을 만들어 */
	modify_irte(&data->irq_2_iommu, &entry);	/* [한국어] 써 넣는다. present 가 내려가 이후 인터럽트는 폴트가 된다. 자리는 그대로다 */
}

/*
 * [한국어]
 * intel_irq_remapping_select - 이 도메인이 그 인터럽트를 담당하는지 답한다
 *
 * @d: 후보 도메인(이 유닛의 것).
 * @fwspec: 찾고 있는 인터럽트의 펌웨어 명세.
 * @bus_token: 버스 종류(여기서는 쓰지 않는다).
 * @return: 담당하면 참.
 *
 * 커널은 인터럽트를 배선할 때 모든 도메인에 "이거 네 것이냐"고 묻는다.
 * PCI MSI 는 장치에 도메인 포인터가 이미 붙어 있어 이 질문이 필요 없지만,
 * IOAPIC 과 HPET 은 장치 객체가 없어 번호로만 식별되므로 이 콜백이 필요하다.
 *
 * 답하는 방법은 단순하다: 그 번호를 담당하는 유닛을 찾아, 그 유닛의
 * ir_domain 이 바로 자신인지 비교한다.
 *
 * 호출 체인:
 *   irq_find_matching_fwspec() → [이 함수] → map_ioapic_to_iommu()/map_hpet_to_iommu()
 */
static int intel_irq_remapping_select(struct irq_domain *d,
				      struct irq_fwspec *fwspec,
				      enum irq_domain_bus_token bus_token)
{
	struct intel_iommu *iommu = NULL;	/* [한국어] 찾아낸 담당 유닛 */

	if (x86_fwspec_is_ioapic(fwspec))	/* [한국어] IOAPIC 인터럽트를 찾고 있는가 */
		iommu = map_ioapic_to_iommu(fwspec->param[0]);	/* [한국어] 그 IOAPIC 번호의 담당 유닛 */
	else if (x86_fwspec_is_hpet(fwspec))	/* [한국어] HPET 인가 */
		iommu = map_hpet_to_iommu(fwspec->param[0]);	/* [한국어] 그 HPET 의 담당 유닛 */

	return iommu && d == iommu->ir_domain;	/* [한국어] 찾은 유닛의 도메인이 바로 나 자신이면 내 것이다 */
}

/*
 * [한국어] struct irq_domain_ops intel_ir_domain_ops — 재매핑 도메인의 콜백 표
 *
 * 커널 인터럽트 도메인 계층이 이 표를 통해 재매핑 계층을 부린다. 다섯
 * 콜백이 인터럽트 하나의 수명을 나눠 맡는다: select(내 것인가) → alloc
 * (표 항목 확보) → activate(하드웨어에 반영) → deactivate(항목 비우기) →
 * free(항목 반환).
 *
 * alloc 과 activate 가 나뉜 이유가 이 설계의 핵심이다. 할당 시점에는 아직
 * 쓰이지 않는 인터럽트가 present 인 항목을 차지하면 안 되고, 활성화 시점에는
 * 자원 부족으로 실패하면 안 된다. 그래서 실패할 수 있는 일은 alloc 에,
 * 반영만 하는 일은 activate 에 둔다.
 */
static const struct irq_domain_ops intel_ir_domain_ops = {
	.select = intel_irq_remapping_select,
	/* [한국어] "이 인터럽트가 내 것이냐"에 답한다. IOAPIC/HPET 처럼 장치 객체가
	 *   없는 인터럽트를 배선할 때 커널이 모든 도메인에 묻는다. */
	.alloc = intel_irq_remapping_alloc,
	/* [한국어] 표 항목을 확보하고 chip_data 를 만든다. 실패할 수 있는 일은 전부 여기서. */
	.free = intel_irq_remapping_free,
	/* [한국어] 표 항목과 chip_data 를 놓고 부모의 벡터도 반환한다. */
	.activate = intel_irq_remapping_activate,
	/* [한국어] 준비된 항목 내용을 하드웨어 표에 실제로 써 넣는다.
	 *   자원 확보가 끝난 뒤라 실패하지 않는다. */
	.deactivate = intel_irq_remapping_deactivate,
	/* [한국어] 항목을 0 으로 만들어 인터럽트를 끊는다. 자리는 그대로 두므로
	 *   재활성화하면 되살아난다. */
};

/*
 * [한국어] struct msi_parent_ops dmar_msi_parent_ops — MSI 계층에서 부모 역할의 규칙
 *
 * PCI/MSI 도메인이 자식으로 붙을 때, 이 표가 "부모로서 무엇을 지원하고
 * 어떤 자식을 받아들이는가"를 알려 준다. 이것이 있어야 장치의 MSI 도메인이
 * 재매핑 도메인 위에 자동으로 쌓인다.
 */
static const struct msi_parent_ops dmar_msi_parent_ops = {
	.supported_flags	= X86_VECTOR_MSI_FLAGS_SUPPORTED | MSI_FLAG_MULTI_PCI_MSI,
	/* [한국어] 이 부모가 자식 MSI 도메인에 허용하는 기능들.
	 * MSI_FLAG_MULTI_PCI_MSI 가 다중 벡터 MSI 를 허용하는데, 그것이 가능한 이유는
	 *   alloc_irte 가 연속·정렬된 표 항목 묶음을 잡아 주기 때문이다. */
	.bus_select_token	= DOMAIN_BUS_DMAR,
	/* [한국어] 이 도메인이 스스로를 소개하는 토큰. 자식이 부모를 찾을 때의 이름표다. */
	.bus_select_mask	= MATCH_PCI_MSI,
	/* [한국어] 받아들일 자식의 종류 — PCI MSI 도메인만 이 부모 아래 붙는다. */
	.prefix			= "IR-",
	/* [한국어] 자식 도메인 이름 앞에 붙는 접두사. /proc/interrupts 에서 재매핑을
	 *   거치는 인터럽트임을 한눈에 알 수 있게 한다. */
	.init_dev_msi_info	= msi_parent_init_dev_msi_info,
	/* [한국어] 자식 도메인의 MSI 정보를 초기화하는 공용 구현.
	 *   재매핑에 특별할 것이 없어 커널의 기본 함수를 그대로 쓴다. */
};

/*
 * Support of Interrupt Remapping Unit Hotplug
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * dmar_ir_add - 새로 꽂힌 유닛에 재매핑을 갖춘다
 *
 * @dmaru: 추가된 DRHD 유닛의 표 정보.
 * @iommu: 그 유닛.
 * @return: 0 성공, -ENODEV 면 이 유닛으로는 재매핑을 이어갈 수 없다.
 *
 * 부팅 경로(prepare → enable)를 유닛 하나에 대해 압축한 것이다. 다만 이미
 * 다른 유닛들이 동작 중이므로 조건이 더 엄격하다: 시스템이 이미 x2APIC 으로
 * 돌고 있는데 새 유닛이 EIM 을 지원하지 않으면 받아들일 수 없다. 부팅
 * 때처럼 전부 xAPIC 으로 내리는 선택지가 없기 때문이다.
 *
 * 실패하면 그 자리에서 되감는다 — 표를 허물고, 방금 등록한 IOAPIC/HPET
 * 대응도 배열에서 지운다. 남겨 두면 없는 유닛을 가리키는 항목이 된다.
 *
 * 원 주석의 TODO 가 남긴 빈틈: 부팅 때와 달리 "모든 IOAPIC 이 어느 유닛
 * 아래 있는가"를 다시 확인하지 않는다.
 *
 * 호출 체인:
 *   dmar_ir_hotplug() → [이 함수] → intel_setup_irq_remapping()
 *     → iommu_enable_irq_remapping()
 */
static int dmar_ir_add(struct dmar_drhd_unit *dmaru, struct intel_iommu *iommu)
{
	int ret;	/* [한국어] 설정 결과 */
	int eim = x2apic_enabled();	/* [한국어] 시스템이 이미 x2APIC 으로 돌고 있는가 */

	if (eim && !ecap_eim_support(iommu->ecap)) {	/* [한국어] 그런데 새 유닛이 EIM 을 지원하지 않는다 */
		pr_info("DRHD %Lx: EIM not supported by DRHD, ecap %Lx\n",	/* [한국어] 부팅 때와 달리 전부 xAPIC 으로 내릴 수 없다 */
			iommu->reg_phys, iommu->ecap);
		return -ENODEV;	/* [한국어] 이 유닛은 받아들일 수 없다 */
	}

	if (ir_parse_ioapic_hpet_scope(dmaru->hdr, iommu)) {	/* [한국어] 이 유닛이 담당할 IOAPIC/HPET 을 배열에 등록 */
		pr_warn("DRHD %Lx: failed to parse managed IOAPIC/HPET\n",	/* [한국어] 표가 이상하다 */
			iommu->reg_phys);
		return -ENODEV;	/* [한국어] 받아들이지 않는다 */
	}

	/* TODO: check all IOAPICs are covered by IOMMU */

	/* Setup Interrupt-remapping now. */
	ret = intel_setup_irq_remapping(iommu);	/* [한국어] 표·도메인·QI 를 갖춘다 */
	if (ret) {	/* [한국어] 실패 */
		pr_err("Failed to setup irq remapping for %s\n",	/* [한국어] 어느 유닛인지 알리고 */
		       iommu->name);
		intel_teardown_irq_remapping(iommu);	/* [한국어] 만들다 만 것을 허물고 */
		ir_remove_ioapic_hpet_scope(iommu);	/* [한국어] 방금 등록한 대응도 지운다 — 남기면 없는 유닛을 가리킨다 */
	} else {
		iommu_enable_irq_remapping(iommu);	/* [한국어] 성공하면 곧바로 켠다. 다른 유닛들은 이미 동작 중이다 */
	}

	return ret;	/* [한국어] 결과 보고 */
}

/*
 * [한국어]
 * dmar_ir_hotplug - 유닛의 삽입/제거를 재매핑 관점에서 처리한다
 *
 * @dmaru: 대상 DRHD 유닛.
 * @insert: 참이면 삽입, 거짓이면 제거.
 * @return: 0 성공. -EBUSY 면 지금은 뺄 수 없거나 능력이 맞지 않는다.
 *
 * 제거 쪽의 -EBUSY 가 이 함수의 핵심이다. 표의 비트맵이 비어 있지 않다는
 * 것은 아직 이 유닛의 항목을 쓰는 인터럽트가 살아 있다는 뜻이고, 그 상태로
 * 표를 없애면 그 장치들의 인터럽트가 사라진다. 그래서 유닛 제거 자체를
 * 거부한다 — 상위 계층이 장치를 먼저 정리해야 한다.
 *
 * 삽입 쪽의 포스티드 검사: 시스템이 이미 포스티드 인터럽트를 쓰고 있는데
 * 새 유닛이 그것을 지원하지 않으면 받아들일 수 없다. 유닛마다 되고 안 되는
 * 기능이 있으면 상위 계층이 일관되게 쓸 수 없기 때문이다.
 *
 * ir_table 유무로 중복을 걸러 내므로, 같은 유닛에 대해 여러 번 불려도 안전하다.
 *
 * 호출 체인:
 *   dmar_hotplug_insert()/remove() → [이 함수] → dmar_ir_add()
 *     → iommu_disable_irq_remapping()/intel_teardown_irq_remapping()
 */
int dmar_ir_hotplug(struct dmar_drhd_unit *dmaru, bool insert)
{
	int ret = 0;	/* [한국어] 기본은 성공 */
	struct intel_iommu *iommu = dmaru->iommu;	/* [한국어] 대상 유닛 */

	if (!irq_remapping_enabled)	/* [한국어] 재매핑을 쓰지 않는 시스템이면 */
		return 0;	/* [한국어] 할 일이 없다 */
	if (iommu == NULL)	/* [한국어] 유닛 객체가 없다 */
		return -EINVAL;	/* [한국어] 잘못된 호출 */
	if (!ecap_ir_support(iommu->ecap))	/* [한국어] 재매핑을 지원하지 않는 유닛은 */
		return 0;	/* [한국어] 재매핑 관점에서 할 일이 없다 */
	if (irq_remapping_cap(IRQ_POSTING_CAP) &&	/* [한국어] 시스템이 이미 포스티드 인터럽트를 쓰는데 */
	    !cap_pi_support(iommu->cap))	/* [한국어] 새 유닛이 그것을 지원하지 않으면 */
		return -EBUSY;	/* [한국어] 유닛마다 기능이 다르면 상위 계층이 일관되게 쓸 수 없다 */

	if (insert) {	/* [한국어] 유닛 삽입 */
		if (!iommu->ir_table)	/* [한국어] 아직 표가 없을 때만 — 중복 호출에 안전하다 */
			ret = dmar_ir_add(dmaru, iommu);	/* [한국어] 표를 갖추고 켠다 */
	} else {
		if (iommu->ir_table) {	/* [한국어] 유닛 제거 — 표가 있을 때만 정리할 것이 있다 */
			if (!bitmap_empty(iommu->ir_table->bitmap,	/* [한국어] 아직 이 유닛의 항목을 쓰는 인터럽트가 남아 있는가 */
					  INTR_REMAP_TABLE_ENTRIES)) {
				ret = -EBUSY;	/* [한국어] 남아 있으면 제거를 거부한다. 표를 없애면 그 장치들의 인터럽트가 사라진다 */
			} else {
				iommu_disable_irq_remapping(iommu);	/* [한국어] 비어 있으면 하드웨어를 끄고 */
				intel_teardown_irq_remapping(iommu);	/* [한국어] 표와 도메인을 허물고 */
				ir_remove_ioapic_hpet_scope(iommu);	/* [한국어] 이 유닛이 담당하던 IOAPIC/HPET 대응도 지운다 */
			}
		}
	}

	return ret;	/* [한국어] 결과 보고 */
}
