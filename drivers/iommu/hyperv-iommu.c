// SPDX-License-Identifier: GPL-2.0

/*
 * Hyper-V stub IOMMU driver.
 *
 * Copyright (C) 2019, Microsoft, Inc.
 *
 * Author : Lan Tianyu <Tianyu.Lan@microsoft.com>
 */

/*
 * [한국어 설명] Hyper-V 스텁 IOMMU / 인터럽트 리매핑 드라이버 (hyperv-iommu.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 이름에 IOMMU가 들어가지만 DMA 주소 변환을 하는 진짜 IOMMU 드라이버가
 * 아니다. x86의 "인터럽트 리매핑(interrupt remapping, IR)" 프레임워크
 * (drivers/iommu/irq_remapping.c)에 자신을 등록해, Hyper-V 하이퍼바이저 위에서
 * 리눅스가 돌 때 IO-APIC 인터럽트를 다루는 방식을 조정하는 "스텁(stub)"이다.
 * 목적은 두 가지로 완전히 갈린다. (1) 게스트로 동작할 때: Hyper-V는 게스트에게
 * 인터럽트 리매핑 하드웨어를 노출하지 않으므로, IO-APIC RTE(Redirection Table
 * Entry)의 destination 필드가 8비트뿐이라 APIC ID가 256 이상인 CPU로는 IO-APIC
 * 인터럽트를 보낼 수 없다. 그래서 이 드라이버는 "리매핑 도메인인 척"하면서
 * 실제로는 IRQ affinity를 APIC ID < 256인 CPU 집합(ioapic_max_cpumask)으로
 * 제한하는 일만 한다. (2) 루트 파티션(root partition, Hyper-V의 dom0에 해당)으로
 * 동작할 때: IO-APIC RTE를 리눅스가 직접 쓰는 것이 금지되어 있고, 반드시
 * 하이퍼콜(hv_map_ioapic_interrupt / hv_unmap_ioapic_interrupt)로 하이퍼바이저에게
 * 대신 프로그래밍해 달라고 요청해야 한다. 이때 이 파일은 그 하이퍼콜 결과를
 * IO_APIC_route_entry 형태로 되돌려 받아 MSI 메시지 형식으로 포장해 주는
 * 어댑터 역할을 한다.
 * 파일 전체가 `#ifdef CONFIG_IRQ_REMAP` 하나로 감싸여 있어, 인터럽트 리매핑
 * 기능이 꺼진 커널에서는 통째로 사라진다(빈 오브젝트가 된다).
 *
 * === 전체 아키텍처에서의 위치 ===
 * x86 인터럽트 경로는 계층형 IRQ 도메인(hierarchical irq_domain)으로 구성된다:
 *
 *   [디바이스] → IO-APIC 도메인 → (이 파일의 IR 도메인) → x86 vector 도메인 → CPU IDT
 *
 * 부팅 시 `enable_IR_x2apic()` → `irq_remapping_prepare()`가
 * `remap_ops[]` 배열(intel_irq_remap_ops, amd_iommu_irq_ops,
 * hyperv_irq_remap_ops)을 순회하며 각각의 `.prepare()`를 호출한다. 이 파일의
 * `hyperv_prepare_irq_remapping()`이 여기서 성공을 반환하면, 이후
 * `irq_remapping_enable()` → `hyperv_enable_irq_remapping()`이 호출되어 x2APIC를
 * 켤지 말지를 결정한다. `.prepare()` 안에서 만들어진 `ioapic_ir_domain`은
 * `arch_get_ir_parent_domain()`(= x86 vector 도메인)을 부모로 갖는 중간 계층이고,
 * IO-APIC 코드가 `irq_find_matching_fwspec()`로 자신의 부모를 찾을 때
 * `.select` 콜백(hyperv_irq_remapping_select)이 "그 IO-APIC은 내가 맡는다"고
 * 응답해 이 도메인이 IO-APIC 아래에 끼워진다.
 * 실행 컨텍스트는 두 가지다. `__init` 함수들은 부팅 초기 단일 스레드 컨텍스트,
 * alloc/free/set_affinity/compose_msi_msg 콜백들은 IRQ 코어가 `irq_desc`의
 * 락(또는 `irq_domain_mutex`)을 잡은 상태로 호출하는 프로세스 컨텍스트다.
 *
 * === 타 모듈과의 연결 ===
 * - `irq_remapping.h`(같은 디렉토리): `struct irq_remap_ops`와 IRQ_REMAP_*
 *   모드 상수를 제공한다. 이 파일이 정의하는 `hyperv_irq_remap_ops` 심볼을
 *   drivers/iommu/irq_remapping.c의 `remap_ops[]`가 참조한다.
 * - `asm/mshyperv.h`: `hv_root_partition()`, `hv_map_ioapic_interrupt()`,
 *   `hv_unmap_ioapic_interrupt()`, `struct hv_interrupt_entry` 등 Hyper-V
 *   하이퍼콜 래퍼. 루트 파티션 경로의 실제 하드웨어 조작은 전부 여기로 나간다.
 * - `asm/io_apic.h`: `struct IO_APIC_route_entry`. 하이퍼바이저가 돌려준
 *   64비트 RTE 값을 비트필드로 해석하는 데 쓴다.
 * - `asm/hw_irq.h`: `struct irq_cfg`, `irqd_cfg()`, `vector_schedule_cleanup()`.
 *   벡터 할당은 상위(부모) 도메인인 x86 vector 도메인이 담당하고 이 파일은
 *   그 결과를 읽기만 한다.
 * - `asm/hypervisor.h`: `hypervisor_is_type(X86_HYPER_MS_HYPERV)`로 지금 돌고
 *   있는 하이퍼바이저가 Hyper-V인지 확인한다.
 * 데이터 흐름(루트 파티션): IO-APIC 코어가 IRQ를 할당 →
 * hyperv_root_irq_remapping_alloc()이 `hyperv_root_ir_data`를 만들어 chip_data에
 * 매달아 둠 → affinity가 정해지면 hyperv_root_ir_compose_msi_msg()가
 * 하이퍼콜로 RTE를 받아와 `data->entry`에 캐시하고 msi_msg로 변환 → IO-APIC
 * 코어가 그 msi_msg를 RTE에 기록(실제로는 하이퍼바이저가 이미 써 둔 값과 동일).
 *
 * === 주요 함수/구조체 요약 ===
 * - hyperv_prepare_irq_remapping(): 부팅 시 Hyper-V인지 판별하고 IR 도메인을
 *   만든다. 게스트면 APIC ID < 256인 CPU 마스크(ioapic_max_cpumask)를 계산한다.
 * - hyperv_enable_irq_remapping(): x2APIC 지원 여부만 보고 XAPIC/X2APIC 모드를
 *   반환한다. 실제로 켜는 하드웨어는 없다.
 * - hyperv_ir_set_affinity(): 게스트용. 요청된 affinity가 ioapic_max_cpumask
 *   안에 있는지 검사한 뒤 부모(vector) 도메인에 위임한다.
 * - hyperv_root_ir_compose_msi_msg(): 루트 파티션용. 하이퍼콜로 IO-APIC RTE를
 *   프로그래밍하고 그 결과를 msi_msg 형식으로 포장한다.
 * - struct hyperv_root_ir_data: 루트 파티션에서 IRQ 하나당 유지하는 상태
 *   (IO-APIC ID, level/edge, 하이퍼바이저가 돌려준 RTE 캐시).
 * - hyperv_irq_remap_ops / hyperv_ir_domain_ops / hyperv_root_ir_domain_ops:
 *   각각 IR 프레임워크, 게스트 도메인, 루트 파티션 도메인의 콜백 테이블.
 */

/* [한국어] u8/u32/u64 등 커널 고정폭 정수 타입 — RTE 비트필드 조작에 필요하다. */
#include <linux/types.h>
/* [한국어] 인터럽트 처리 일반 API — IRQ_SET_MASK_OK_DONE 같은 반환 상수를 얻는다. */
#include <linux/interrupt.h>
/* [한국어] struct irq_data, struct irq_chip, irq_domain 등 IRQ 코어 자료구조. */
#include <linux/irq.h>
/* [한국어] IOMMU 서브시스템 공통 헤더 — 이 파일은 IR 프레임워크에만 붙지만
 * drivers/iommu 아래에 있는 관례상 포함한다. */
#include <linux/iommu.h>
/* [한국어] 모듈 빌드 관련 매크로 — 실제로는 builtin이지만 심볼 가시성 매크로용. */
#include <linux/module.h>

/* [한국어] apic_ack_irq(), x2apic_supported() 등 로컬 APIC 조작 함수. */
#include <asm/apic.h>
/* [한국어] cpu_physical_id() 매크로 — 논리 CPU 번호를 APIC ID로 바꾼다.
 * 게스트 경로에서 APIC ID가 256 미만인지 판별하는 데 핵심적으로 쓰인다. */
#include <asm/cpu.h>
/* [한국어] struct irq_cfg, irqd_cfg(), struct irq_alloc_info,
 * X86_IRQ_ALLOC_TYPE_IOAPIC, vector_schedule_cleanup() 등 x86 IRQ 내부 API. */
#include <asm/hw_irq.h>
/* [한국어] struct IO_APIC_route_entry — 64비트 RTE의 비트필드 정의.
 * 하이퍼콜이 돌려준 raw 값을 해석할 때 쓴다. */
#include <asm/io_apic.h>
/* [한국어] arch_get_ir_parent_domain(), x86_fwspec_is_ioapic() 등
 * 아키텍처별 인터럽트 리매핑 훅. */
#include <asm/irq_remapping.h>
/* [한국어] hypervisor_is_type(), X86_HYPER_MS_HYPERV — 어떤 하이퍼바이저
 * 위에서 돌고 있는지 판별한다. */
#include <asm/hypervisor.h>
/* [한국어] Hyper-V 전용 하이퍼콜 래퍼 — hv_root_partition(),
 * hv_map_ioapic_interrupt(), hv_unmap_ioapic_interrupt(),
 * struct hv_interrupt_entry, HV_DEVICE_TYPE_IOAPIC. */
#include <asm/mshyperv.h>

/* [한국어] drivers/iommu 로컬 헤더 — struct irq_remap_ops와
 * IRQ_REMAP_XAPIC_MODE / IRQ_REMAP_X2APIC_MODE 상수를 가져온다.
 * 이 파일이 정의하는 hyperv_irq_remap_ops의 타입이 여기서 온다. */
#include "irq_remapping.h"

/* [한국어] 인터럽트 리매핑 프레임워크 자체가 빌드에서 빠지면 이 파일은
 * 통째로 비어 있는 오브젝트가 된다. hyperv_irq_remap_ops를 참조하는
 * irq_remapping.c도 같은 CONFIG로 가드되므로 링크 에러는 나지 않는다. */
#ifdef CONFIG_IRQ_REMAP

/*
 * According 82093AA IO-APIC spec , IO APIC has a 24-entry Interrupt
 * Redirection Table. Hyper-V exposes one single IO-APIC and so define
 * 24 IO APIC remmapping entries.
 */
/* [한국어] IO-APIC 리다이렉션 테이블의 엔트리 개수(=이 IR 도메인이 관리할
 * 인터럽트 개수). 인텔 82093AA IO-APIC 스펙상 24개이고, Hyper-V는 게스트에게
 * IO-APIC을 정확히 하나만 노출하므로 도메인 크기를 24로 고정한다.
 * irq_domain_create_hierarchy()의 size 인자로 들어간다. */
#define IOAPIC_REMAPPING_ENTRY 24

/* [한국어] IO-APIC 인터럽트를 받을 수 있는 CPU들의 마스크(게스트 전용).
 * 설정자: hyperv_prepare_irq_remapping()이 부팅 때 한 번만 채운다.
 * 읽는 자: hyperv_ir_set_affinity()가 요청 affinity 검증에,
 *          hyperv_irq_remapping_alloc()이 초기 affinity 지정에 쓴다.
 * 값 범위: APIC ID가 256 미만인 possible CPU들의 비트. 초기값 CPU_BITS_NONE(전부 0).
 * 동기화: 부팅 초기 단일 스레드에서 한 번 쓰고 그 뒤로는 읽기 전용이라 락이 없다.
 * 왜 필요한가: Hyper-V 게스트에는 인터럽트 리매핑 하드웨어가 없어 IO-APIC RTE의
 * 8비트 destination 필드만 쓸 수 있고, 그래서 APIC ID >= 256인 CPU에는
 * IO-APIC 인터럽트를 전달할 방법이 아예 없다. */
static cpumask_t ioapic_max_cpumask = { CPU_BITS_NONE };
/* [한국어] 이 드라이버가 만든 IR irq_domain 포인터.
 * 설정자: hyperv_prepare_irq_remapping()의 irq_domain_create_hierarchy() 결과.
 * 읽는 자: 현재 이 파일 안에서는 성공/실패 판별에만 쓰이지만, 도메인 자체는
 *          IRQ 코어가 .select 콜백을 통해 찾아 쓴다.
 * 값 범위: NULL(생성 실패) 또는 유효한 도메인 포인터.
 * 동기화: 부팅 초기에 한 번 설정되고 해제되지 않는다. */
static struct irq_domain *ioapic_ir_domain;

/*
 * [한국어]
 * hyperv_ir_set_affinity - 게스트 모드에서 IO-APIC IRQ의 CPU affinity를 바꾼다
 *
 * @data: 이 IR 도메인 계층의 irq_data. data->parent_data가 x86 vector 도메인의 것.
 * @mask: 사용자/커널이 요청한 목적지 CPU 마스크 (/proc/irq/N/smp_affinity 등에서 옴).
 * @force: affinity 설정을 강제할지 여부. 부모 도메인에 그대로 전달한다.
 * @return: 0 또는 IRQ_SET_MASK_OK_DONE 계열 성공 코드, 실패 시 음수 errno.
 *          -EINVAL은 요청한 CPU들이 IO-APIC이 도달할 수 없는 CPU라는 뜻.
 *
 * 왜 필요한가: Hyper-V 게스트에는 인터럽트 리매핑 하드웨어가 없다. IO-APIC RTE는
 * destination 필드가 8비트라 APIC ID 0~255까지만 지정할 수 있는데, 큰 VM에서는
 * APIC ID가 256을 넘는 CPU가 존재한다. 그런 CPU로 affinity를 옮기면 인터럽트가
 * 영원히 오지 않으므로, 여기서 미리 걸러내는 것이 이 함수의 존재 이유다.
 *
 * 동작 과정:
 *  1) 요청 마스크가 ioapic_max_cpumask의 부분집합인지 검사 — 아니면 즉시 거부.
 *  2) 부모(vector) 도메인에 위임 — 실제 벡터 재할당은 거기서 일어난다.
 *  3) 부모가 "이전 벡터를 아직 정리하지 않았다"(0 반환)고 하면
 *     vector_schedule_cleanup()으로 지연 정리를 예약한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. IRQ 코어가 irq_desc 락을 잡은 채 호출하므로
 * 잠들 수 없다. 재진입하지 않는다.
 *
 * 호출 체인:
 *   irq_set_affinity() → irq_do_set_affinity() → chip->irq_set_affinity
 *   → [hyperv_ir_set_affinity] → parent->chip->irq_set_affinity (x86 vector 도메인)
 */
static int hyperv_ir_set_affinity(struct irq_data *data,
		const struct cpumask *mask, bool force)
{
	/* [한국어] 계층형 도메인에서 한 단계 위(부모)인 x86 vector 도메인의 irq_data.
	 * 실제 벡터 할당/해제는 전부 이 부모가 담당한다. */
	struct irq_data *parent = data->parent_data;
	/* [한국어] 이 IRQ에 배정된 x86 벡터 정보(vector 번호, 목적지 CPU 등).
	 * 아래 vector_schedule_cleanup()에 넘겨 이전 벡터를 정리하는 데 쓴다. */
	struct irq_cfg *cfg = irqd_cfg(data);
	/* [한국어] 부모 도메인의 반환값을 담을 변수. */
	int ret;

	/* Return error If new irq affinity is out of ioapic_max_cpumask. */
	/* [한국어] 요청한 CPU 집합이 IO-APIC이 도달 가능한 집합 안에 완전히
	 * 들어가는지 확인한다. 하나라도 벗어나면 그 CPU가 선택됐을 때 인터럽트가
	 * 유실되므로, 부분 허용 없이 통째로 거부한다. */
	if (!cpumask_subset(mask, &ioapic_max_cpumask))
		/* [한국어] 도달 불가능한 목적지 — 사용자에게 EINVAL로 알린다. */
		return -EINVAL;

	/* [한국어] 검증을 통과했으니 실제 벡터 재할당은 부모 도메인에 맡긴다.
	 * 이 드라이버는 리매핑 테이블을 갖고 있지 않아 스스로 할 일이 없다. */
	ret = parent->chip->irq_set_affinity(parent, mask, force);
	/* [한국어] 음수면 실패, IRQ_SET_MASK_OK_DONE이면 부모가 하드웨어 갱신까지
	 * 모두 끝냈다는 뜻이라 둘 다 그대로 상위로 반환하고 끝낸다. */
	if (ret < 0 || ret == IRQ_SET_MASK_OK_DONE)
		return ret;

	/* [한국어] 여기까지 왔다면 부모가 새 벡터를 잡았지만 옛 벡터는 아직
	 * 사용 중일 수 있다(진행 중인 인터럽트가 있을 수 있음). 다음 인터럽트가
	 * 새 CPU로 도착한 뒤 안전하게 옛 벡터를 반납하도록 정리를 예약한다. */
	vector_schedule_cleanup(cfg);

	/* [한국어] 성공. 0은 "affinity를 적용했고 호출자가 후속 처리를 해도 된다"는 의미. */
	return 0;
}

/* [한국어] 게스트 모드 IR 계층의 irq_chip.
 * IRQ 코어가 이 계층의 irq_data를 다룰 때 쓰는 콜백 묶음이다. */
static struct irq_chip hyperv_ir_chip = {
	/* [한국어] /proc/interrupts에 표시되는 칩 이름. 디버깅용 식별자다. */
	.name			= "HYPERV-IR",
	/* [한국어] 인터럽트 처리 후 로컬 APIC에 EOI를 보내는 표준 x86 핸들러.
	 * 리매핑 하드웨어가 없으므로 특별한 처리 없이 공용 함수를 그대로 쓴다. */
	.irq_ack		= apic_ack_irq,
	/* [한국어] affinity 변경 시 APIC ID < 256 제약을 강제하는 위 함수.
	 * 이 칩이 존재하는 유일한 이유가 사실상 이 콜백이다. */
	.irq_set_affinity	= hyperv_ir_set_affinity,
};

/*
 * [한국어]
 * hyperv_irq_remapping_alloc - 게스트 모드에서 IR 계층의 IRQ 자원을 할당한다
 *
 * @domain: 이 IR irq_domain (ioapic_ir_domain).
 * @virq: 할당할 리눅스 가상 IRQ 번호의 시작값.
 * @nr_irqs: 연속 할당 개수. IO-APIC 핀은 하나씩만 할당되므로 1이어야 한다.
 * @arg: struct irq_alloc_info * — IO-APIC 코어가 채운 할당 정보.
 * @return: 0 성공, -EINVAL(잘못된 요청/irq_data 없음), 부모 도메인이 낸 음수 errno.
 *
 * 왜 필요한가: 계층형 도메인에서 각 계층은 자기 몫의 자원을 할당하고 부모에게
 * 나머지를 위임해야 한다. 이 드라이버는 리매핑 테이블 엔트리 같은 실제 자원이
 * 없으므로, 할 일은 (a) 요청 검증, (b) 부모에게 벡터 할당 위임, (c) 이 계층의
 * irq_chip 연결, (d) 초기 affinity를 도달 가능한 CPU로 제한하는 것뿐이다.
 *
 * 동작 과정:
 *  1) info가 IO-APIC 타입이고 nr_irqs가 1인지 검증.
 *  2) irq_domain_alloc_irqs_parent()로 부모(vector) 도메인이 벡터를 잡게 한다.
 *  3) 이 계층의 irq_data를 찾아 chip을 hyperv_ir_chip으로 지정.
 *  4) 기본 affinity를 ioapic_max_cpumask로 좁혀 처음부터 도달 가능한 CPU만 쓰게 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, irq_domain_mutex 보유 상태.
 * 에러 경로: 부모 할당 이후 실패하면 반드시 irq_domain_free_irqs_common()으로
 * 부모가 잡은 자원을 되돌려야 한다(누수 방지).
 *
 * 호출 체인:
 *   mp_irqdomain_alloc()/irq_domain_alloc_irqs() → domain->ops->alloc
 *   → [hyperv_irq_remapping_alloc] → irq_domain_alloc_irqs_parent() (vector 도메인)
 */
static int hyperv_irq_remapping_alloc(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs,
				     void *arg)
{
	/* [한국어] IO-APIC 코어가 채워 넘긴 할당 정보. 타입/디바이스 ID/핀 속성이 들어 있다. */
	struct irq_alloc_info *info = arg;
	/* [한국어] 이 도메인 계층에 해당하는 irq_data를 담을 포인터. */
	struct irq_data *irq_data;
	/* [한국어] 부모 도메인 호출 결과. */
	int ret = 0;

	/* [한국어] 이 도메인은 IO-APIC 인터럽트만 받는다. info가 없거나 타입이
	 * 다르거나 한 번에 2개 이상 요청하면(IO-APIC 핀은 개별 할당이 원칙) 거부한다. */
	if (!info || info->type != X86_IRQ_ALLOC_TYPE_IOAPIC || nr_irqs > 1)
		return -EINVAL;

	/* [한국어] 먼저 부모(x86 vector) 도메인이 실제 인터럽트 벡터와 목적지 CPU를
	 * 할당하게 한다. 계층형 도메인은 항상 부모부터 아래로 자원을 확보한다. */
	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, arg);
	/* [한국어] 벡터가 고갈됐다면(-ENOSPC 등) 더 진행할 것이 없으니 그대로 반환. */
	if (ret < 0)
		return ret;

	/* [한국어] 방금 만들어진 계층 중 "이 도메인"에 속하는 irq_data를 찾는다.
	 * 여기에 chip을 붙여야 IRQ 코어가 이 계층의 콜백을 쓰게 된다. */
	irq_data = irq_domain_get_irq_data(domain, virq);
	/* [한국어] 정상 흐름에서는 실패할 수 없지만 방어적으로 검사한다.
	 * 실패 시 부모가 잡은 벡터를 반드시 되돌려 줘야 한다. */
	if (!irq_data) {
		/* [한국어] 부모 계층까지 포함해 방금 할당한 IRQ 자원을 전부 해제. */
		irq_domain_free_irqs_common(domain, virq, nr_irqs);
		return -EINVAL;	/* [한국어] IO-APIC 타입이 아니거나 한 번에 여러 개를 요청하면 이 도메인 소관이 아니다. */
	}

	/* [한국어] 이 계층의 irq_chip을 연결한다. 이후 affinity 변경 요청이
	 * hyperv_ir_set_affinity()를 거쳐 APIC ID 제약 검사를 받게 된다. */
	irq_data->chip = &hyperv_ir_chip;

	/*
	 * Hypver-V IO APIC irq affinity should be in the scope of
	 * ioapic_max_cpumask because no irq remapping support.
	 */
	/* [한국어] 기본 affinity를 도달 가능한 CPU 집합으로 좁힌다. 이렇게 해 두면
	 * 이후 커널이 자동으로 고르는 CPU도 항상 APIC ID < 256 범위 안이 되어,
	 * set_affinity 단계에서 거부당하는 상황 자체가 생기지 않는다. */
	irq_data_update_affinity(irq_data, &ioapic_max_cpumask);

	/* [한국어] 이 계층에서 따로 잡은 자원이 없으므로 성공만 알리고 끝낸다. */
	return 0;
}

/*
 * [한국어]
 * hyperv_irq_remapping_free - 게스트 모드에서 IR 계층의 IRQ 자원을 해제한다
 *
 * @domain: 이 IR irq_domain.
 * @virq: 해제할 가상 IRQ 시작 번호.
 * @nr_irqs: 해제할 개수.
 * @return: 없음.
 *
 * 왜 필요한가: irq_domain_ops는 .free를 요구한다. 게스트 경로에서는 alloc이
 * chip 포인터만 세팅했을 뿐 별도로 kmalloc한 것이 없어, 이 계층이 정리할
 * 고유 자원은 존재하지 않는다. 그래서 공용 헬퍼에 그대로 위임한다.
 * (반대로 루트 파티션 버전인 hyperv_root_irq_remapping_free()는 chip_data로
 * 매달아 둔 구조체와 하이퍼바이저 매핑까지 정리해야 해서 훨씬 복잡하다.)
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, irq_domain_mutex 보유 상태.
 *
 * 호출 체인:
 *   irq_domain_free_irqs() → domain->ops->free → [hyperv_irq_remapping_free]
 *   → irq_domain_free_irqs_common() → 부모(vector) 도메인의 free
 */
static void hyperv_irq_remapping_free(struct irq_domain *domain,
				 unsigned int virq, unsigned int nr_irqs)
{
	/* [한국어] 이 계층의 irq_data를 떼어내고 부모 계층의 free까지 연쇄 호출하는
	 * 공용 헬퍼. 이 드라이버가 추가로 할당한 것이 없으므로 이 한 줄이 전부다. */
	irq_domain_free_irqs_common(domain, virq, nr_irqs);
}

/*
 * [한국어]
 * hyperv_irq_remapping_select - 주어진 fwspec이 이 도메인 소관인지 판별한다
 *
 * @d: 후보 irq_domain (이 파일이 만든 ioapic_ir_domain).
 * @fwspec: 인터럽트 컨트롤러를 지목하는 펌웨어 스펙(ACPI/DT에서 유래).
 * @bus_token: 버스 종류 힌트. 여기서는 쓰지 않는다.
 * @return: 이 도메인이 담당하면 1(참), 아니면 0.
 *
 * 왜 필요한가: IO-APIC 코드가 자신의 부모 도메인을 찾을 때
 * irq_find_matching_fwspec()이 등록된 도메인들을 돌며 .select를 호출한다.
 * 여기서 참을 반환해야 이 IR 도메인이 IO-APIC과 vector 도메인 사이에 끼어들어
 * 위에서 설명한 affinity 제약(또는 루트 파티션의 하이퍼콜 매핑)이 적용된다.
 * Hyper-V는 IO-APIC을 정확히 하나만 노출하므로 "IO-APIC이기만 하면 내 것"이라는
 * 단순한 판정으로 충분하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, irq_domain_mutex 보유 상태. 부팅 중
 * IO-APIC 초기화 과정에서 주로 호출된다.
 *
 * 호출 체인:
 *   mp_irqdomain_create() → irq_find_matching_fwspec() → domain->ops->select
 *   → [hyperv_irq_remapping_select] → x86_fwspec_is_ioapic()
 */
static int hyperv_irq_remapping_select(struct irq_domain *d,
				       struct irq_fwspec *fwspec,
				       enum irq_domain_bus_token bus_token)
{
	/* Claim the only I/O APIC emulated by Hyper-V */
	/* [한국어] fwspec이 IO-APIC을 가리키는지만 확인해 그대로 반환한다.
	 * Hyper-V가 에뮬레이트하는 IO-APIC은 하나뿐이라 추가 식별자 비교가 필요 없다. */
	return x86_fwspec_is_ioapic(fwspec);
}

/* [한국어] 게스트 모드에서 쓰는 irq_domain 콜백 테이블.
 * hyperv_prepare_irq_remapping()이 hv_root_partition()이 거짓일 때 이것을 고른다. */
static const struct irq_domain_ops hyperv_ir_domain_ops = {
	/* [한국어] IO-APIC이 부모 도메인을 찾을 때 "내가 맡는다"고 응답하는 콜백. */
	.select = hyperv_irq_remapping_select,
	/* [한국어] IRQ 할당 시 chip 연결과 affinity 제한을 수행하는 콜백. */
	.alloc = hyperv_irq_remapping_alloc,
	/* [한국어] IRQ 해제 콜백. 고유 자원이 없어 공용 헬퍼로 위임한다. */
	.free = hyperv_irq_remapping_free,
};

/* [한국어] 루트 파티션용 ops 테이블의 전방 선언.
 * 아래 hyperv_prepare_irq_remapping()이 이 심볼을 참조하는데 실제 정의는
 * 파일 맨 끝(루트 파티션 콜백들 뒤)에 있어서, 컴파일 순서를 맞추려고
 * 여기에 미리 선언해 둔다. */
static const struct irq_domain_ops hyperv_root_ir_domain_ops;
/*
 * [한국어]
 * hyperv_prepare_irq_remapping - 부팅 시 Hyper-V IR 도메인을 준비한다
 *
 * @return: 0 성공, -ENODEV(Hyper-V가 아니거나 이 드라이버가 필요 없음),
 *          -ENOMEM(fwnode/도메인 생성 실패).
 *
 * 왜 필요한가: x86 인터럽트 리매핑 프레임워크는 부팅 초기에 등록된
 * remap_ops 후보들의 .prepare()를 차례로 불러 "누가 IR을 담당할지" 정한다.
 * 이 함수가 0을 반환하면 이후 IO-APIC 인터럽트가 이 드라이버의 도메인을
 * 거치게 되고, -ENODEV를 반환하면 Hyper-V 스텁은 아예 관여하지 않는다.
 *
 * 동작 과정:
 *  1) 하이퍼바이저가 Hyper-V인지 확인. 아니면 -ENODEV.
 *  2) msi_ext_dest_id()가 참이면(= 확장 destination ID를 쓸 수 있으면)
 *     8비트 APIC ID 제약 자체가 없으므로 이 드라이버가 필요 없다 → -ENODEV.
 *     단, 루트 파티션에서는 이 함수가 항상 거짓이라 계속 진행된다.
 *  3) 루트 파티션이면 root ops, 게스트면 guest ops를 고른다.
 *  4) fwnode를 만들고 vector 도메인을 부모로 하는 24엔트리 계층 도메인을 만든다.
 *  5) 루트 파티션이면 여기서 끝. 게스트면 APIC ID < 256인 CPU 마스크를 계산한다.
 *
 * 실행 컨텍스트: 부팅 초기(__init), 단일 CPU, 인터럽트가 아직 본격 동작하기 전.
 * 에러 경로: 도메인 생성이 실패하면 앞서 만든 fwnode를 반드시 해제한다.
 *
 * 호출 체인:
 *   enable_IR_x2apic() → irq_remapping_prepare() → remap_ops->prepare
 *   → [hyperv_prepare_irq_remapping] → irq_domain_create_hierarchy()
 */
static int __init hyperv_prepare_irq_remapping(void)
{
	/* [한국어] 이 도메인을 식별하는 펌웨어 노드 핸들. 도메인 하나에 하나씩
	 * 필요하며 /sys/kernel/debug/irq/domains에 이름으로 나타난다. */
	struct fwnode_handle *fn;
	/* [한국어] CPU 순회용 인덱스. 아래 APIC ID 스캔 루프에서 쓴다. */
	int i;
	/* [한국어] 만들 도메인의 이름. 루트 파티션이냐 게스트냐에 따라 달라진다. */
	const char *name;
	/* [한국어] 사용할 콜백 테이블. 역시 루트/게스트에 따라 달라진다. */
	const struct irq_domain_ops *ops;

	/*
	 * For a Hyper-V root partition, ms_hyperv_msi_ext_dest_id()
	 * will always return false.
	 */
	/* [한국어] 두 가지를 한 번에 거른다.
	 * (1) 지금 Hyper-V 위가 아니면 이 드라이버는 의미가 없다.
	 * (2) 확장 destination ID(15비트 APIC ID)를 쓸 수 있는 게스트라면
	 *     8비트 제약이 애초에 없으므로 affinity를 좁힐 이유가 없다.
	 * 루트 파티션에서는 (2)가 항상 거짓이라 이 검사를 통과해 아래로 내려간다. */
	if (!hypervisor_is_type(X86_HYPER_MS_HYPERV) ||
	    x86_init.hyper.msi_ext_dest_id())
		return -ENODEV;

	/* [한국어] 루트 파티션(Hyper-V의 특권 파티션, dom0에 해당)인지 판별한다.
	 * 같은 도메인 껍데기를 쓰지만 내부 동작이 완전히 다르다. */
	if (hv_root_partition()) {
		/* [한국어] 루트 파티션: RTE를 하이퍼콜로 프로그래밍하는 ops를 쓴다. */
		name = "HYPERV-ROOT-IR";
		ops = &hyperv_root_ir_domain_ops;	/* [한국어] 루트 파티션용 콜백 테이블 — 하이퍼콜로 RTE를 프로그래밍한다. */
	} else {
		/* [한국어] 일반 게스트: affinity를 제한하기만 하는 ops를 쓴다. */
		name = "HYPERV-IR";
		ops = &hyperv_ir_domain_ops;	/* [한국어] 게스트용 콜백 테이블 — affinity 제한만 수행한다. */
	}

	/* [한국어] 도메인 식별용 fwnode 생성. 하드웨어 펌웨어 노드가 없는
	 * 소프트웨어 도메인이므로 이름만 가진 익명 노드를 만든다. */
	fn = irq_domain_alloc_named_id_fwnode(name, 0);
	/* [한국어] 메모리 부족으로 실패하면 더 진행할 수 없다. */
	if (!fn)
		return -ENOMEM;

	/* [한국어] 계층형 도메인 생성.
	 * 부모: arch_get_ir_parent_domain() = x86 vector 도메인(실제 벡터 할당자).
	 * 크기: IOAPIC_REMAPPING_ENTRY(24) — IO-APIC 핀 개수.
	 * ops: 위에서 고른 루트/게스트용 콜백 테이블.
	 * 이 도메인이 IO-APIC 도메인과 vector 도메인 사이에 끼어드는 순간
	 * 모든 IO-APIC 인터럽트가 이 파일의 alloc/set_affinity를 통과하게 된다. */
	ioapic_ir_domain =
		irq_domain_create_hierarchy(arch_get_ir_parent_domain(),
				0, IOAPIC_REMAPPING_ENTRY, fn, ops, NULL);

	/* [한국어] 도메인 생성 실패 시 앞서 만든 fwnode를 되돌려 누수를 막는다. */
	if (!ioapic_ir_domain) {
		irq_domain_free_fwnode(fn);	/* [한국어] 도메인 생성에 실패했으니 앞서 만든 fwnode를 되돌려 누수를 막는다. */
		return -ENOMEM;	/* [한국어] 메모리 부족을 상위(IR 프레임워크)에 알린다 — 이 드라이버는 IR을 맡지 않게 된다. */
	}

	/* [한국어] 루트 파티션은 affinity를 좁힐 필요가 없다. RTE를 직접 쓰지 않고
	 * 하이퍼콜에 맡기므로 8비트 destination 제약이 리눅스 쪽 문제가 아니다. */
	if (hv_root_partition())
		return 0; /* The rest is only relevant to guests */

	/*
	 * Hyper-V doesn't provide irq remapping function for
	 * IO-APIC and so IO-APIC only accepts 8-bit APIC ID.
	 * Cpu's APIC ID is read from ACPI MADT table and APIC IDs
	 * in the MADT table on Hyper-v are sorted monotonic increasingly.
	 * APIC ID reflects cpu topology. There maybe some APIC ID
	 * gaps when cpu number in a socket is not power of two. Prepare
	 * max cpu affinity for IOAPIC irqs. Scan cpu 0-255 and set cpu
	 * into ioapic_max_cpumask if its APIC ID is less than 256.
	 */
	/* [한국어] APIC ID가 8비트에 들어가는 CPU만 골라 마스크에 넣는다.
	 * 왜 논리 CPU 번호 0~255만 보는가: Hyper-V의 MADT는 APIC ID가 단조 증가
	 * 순으로 정렬되어 있어, 논리 번호 256번째 이후 CPU의 APIC ID는 반드시
	 * 256 이상이다. 따라서 앞 256개만 검사하면 충분하다.
	 * 왜 cpu_physical_id()를 또 확인하는가: 소켓당 코어 수가 2의 거듭제곱이
	 * 아니면 APIC ID에 구멍이 생겨, 논리 번호가 256 미만이어도 APIC ID가
	 * 256 이상일 수 있기 때문이다. */
	for (i = min_t(unsigned int, nr_cpu_ids - 1, 255); i >= 0; i--)
		/* [한국어] 실제 존재 가능한 CPU이면서 APIC ID가 8비트에 들어가는
		 * 경우에만 IO-APIC 인터럽트 목적지 후보로 등록한다. */
		if (cpu_possible(i) && cpu_physical_id(i) < 256)
			cpumask_set_cpu(i, &ioapic_max_cpumask);

	/* [한국어] 준비 완료. IR 프레임워크가 이 드라이버를 IR 제공자로 채택한다. */
	return 0;
}

/*
 * [한국어]
 * hyperv_enable_irq_remapping - IR을 "활성화"하고 APIC 모드를 알려준다
 *
 * @return: IRQ_REMAP_X2APIC_MODE 또는 IRQ_REMAP_XAPIC_MODE.
 *
 * 왜 필요한가: irq_remapping_enable()은 IR 제공자에게 "이제 켜라"고 지시하고,
 * 반환값으로 x2APIC를 켜도 되는지를 판단한다. 인텔/AMD 드라이버는 여기서
 * 실제 리매핑 테이블을 하드웨어에 등록하지만, Hyper-V 스텁은 켤 하드웨어가
 * 없으므로 CPU가 x2APIC를 지원하는지만 보고 모드를 통보한다.
 * x2APIC 모드를 반환하면 커널이 32비트 APIC ID를 쓰는 x2APIC 드라이버를
 * 선택하고, 그렇지 않으면 8비트 xAPIC로 남는다.
 *
 * 실행 컨텍스트: 부팅 초기(__init), .prepare()가 성공한 직후.
 *
 * 호출 체인:
 *   enable_IR_x2apic() → irq_remapping_enable() → remap_ops->enable
 *   → [hyperv_enable_irq_remapping]
 */
static int __init hyperv_enable_irq_remapping(void)
{
	/* [한국어] CPUID로 x2APIC 지원 여부를 확인한다. 지원하면 커널이
	 * 32비트 APIC ID를 쓰는 x2APIC 모드로 진입해도 된다고 알린다.
	 * (IO-APIC 인터럽트는 여전히 8비트 제약을 받지만, 그것은 위의
	 * ioapic_max_cpumask로 따로 처리된다.) */
	if (x2apic_supported())
		return IRQ_REMAP_X2APIC_MODE;
	/* [한국어] x2APIC 미지원 — 기존 xAPIC 모드를 유지한다. */
	return IRQ_REMAP_XAPIC_MODE;
}

/* [한국어] 인터럽트 리매핑 프레임워크에 노출되는 진입점 테이블.
 * drivers/iommu/irq_remapping.c의 remap_ops[] 배열이 이 심볼을 참조하며,
 * intel/amd 드라이버와 나란히 후보로 시도된다.
 * static이 아닌 이유가 바로 그 외부 참조 때문이다. */
struct irq_remap_ops hyperv_irq_remap_ops = {
	/* [한국어] 부팅 시 "이 시스템에서 IR을 담당할 수 있는가"를 판별하고
	 * 담당한다면 도메인까지 만들어 두는 콜백. */
	.prepare		= hyperv_prepare_irq_remapping,
	/* [한국어] prepare 성공 후 호출되어 x2APIC/xAPIC 모드를 통보하는 콜백. */
	.enable			= hyperv_enable_irq_remapping,
};

/* IRQ remapping domain when Linux runs as the root partition */
/* [한국어] 루트 파티션 모드에서 IO-APIC IRQ 하나마다 유지하는 상태 구조체.
 * irq_data->chip_data에 매달려 alloc → compose_msi_msg → free 사이를 살아간다.
 * 게스트 모드에는 이런 per-IRQ 상태가 없다(할 일이 affinity 제한뿐이라서). */
struct hyperv_root_ir_data {
	u8 ioapic_id;
	/* [한국어] 이 인터럽트가 속한 IO-APIC의 하드웨어 ID.
	 * 설정자: hyperv_root_irq_remapping_alloc()이 irq_alloc_info->devid에서 복사.
	 * 읽는 자: compose_msi_msg()와 free()가 hv_map/unmap_ioapic_interrupt()의
	 *          첫 인자로 넘겨 하이퍼바이저에게 "어느 IO-APIC의 핀인지" 알린다.
	 * 값 범위: 0~255 (실제로는 Hyper-V가 노출하는 IO-APIC이 하나라 사실상 고정).
	 * 동기화: IRQ 하나에 하나씩 존재하고 IRQ 코어가 irq_desc 락 아래에서만
	 *         만지므로 별도 락이 없다. */

	bool is_level;
	/* [한국어] 이 인터럽트가 레벨 트리거인지(true) 에지 트리거인지(false).
	 * 설정자: alloc()이 irq_alloc_info->ioapic.is_level에서 복사.
	 * 읽는 자: compose_msi_msg()가 hv_map_ioapic_interrupt()에 그대로 전달해,
	 *          하이퍼바이저가 RTE의 trigger mode 비트를 올바르게 세팅하게 한다.
	 * 값 범위: true/false. ACPI MADT의 인터럽트 소스 오버라이드에서 유래한다.
	 * 동기화: 위와 동일 — 할당 시 한 번 쓰고 이후 읽기 전용. */

	struct hv_interrupt_entry entry;
	/* [한국어] 하이퍼바이저가 프로그래밍해 준 IO-APIC RTE의 캐시본.
	 * 설정자: compose_msi_msg()가 hv_map_ioapic_interrupt() 성공 시 저장.
	 * 읽는 자: 다음 번 compose_msi_msg()(affinity 변경 시)와 free()가
	 *          이전 매핑을 해제(hv_unmap_ioapic_interrupt)하는 데 쓴다.
	 * 값 범위: entry.source == HV_DEVICE_TYPE_IOAPIC이고
	 *          entry.ioapic_rte.as_uint64 != 0이면 "유효한 매핑이 살아 있음",
	 *          source == 0이면 "매핑 없음"을 뜻한다(unmap 후 명시적으로 0으로 지움).
	 * 동기화: IRQ 코어의 irq_desc 락 아래에서만 갱신되므로 별도 락이 없다.
	 * 왜 캐시하는가: 하이퍼바이저 매핑은 명시적으로 unmap해야 회수되는 자원이라,
	 *                재프로그래밍/해제 시점에 이전 매핑을 지목할 수단이 필요하다. */
};

/*
 * [한국어]
 * hyperv_root_ir_compose_msi_msg - 루트 파티션에서 IO-APIC RTE를 하이퍼콜로
 *                                  프로그래밍하고 그 결과를 msi_msg로 포장한다
 *
 * @irq_data: 이 계층의 irq_data. chip_data에 hyperv_root_ir_data가 달려 있다.
 * @msg: 출력 인자. IO-APIC 코어가 RTE를 구성할 때 읽는 MSI 메시지.
 * @return: 없음. 하이퍼콜 실패 시 msg를 건드리지 않고 조용히 돌아간다.
 *
 * 왜 필요한가: 루트 파티션에서는 리눅스가 IO-APIC RTE를 직접 쓰는 것이
 * 금지되어 있다(하이퍼바이저가 IO-APIC을 소유한다). 대신
 * hv_map_ioapic_interrupt() 하이퍼콜로 "이 IO-APIC의 이 핀을 이 CPU의 이
 * 벡터로 보내 달라"고 요청하면, 하이퍼바이저가 RTE를 직접 쓰고 그 값을
 * 돌려준다. 리눅스 IO-APIC 코어는 MSI 메시지 형식(compose_msi_msg)으로
 * 결과를 받길 기대하므로, 돌려받은 RTE를 msi_msg 필드로 옮겨 담는다.
 *
 * 동작 과정:
 *  1) 현재 유효 affinity에서 온라인 CPU 하나를 목적지로 고른다.
 *  2) 이전 매핑이 남아 있으면 먼저 unmap 하이퍼콜로 회수한다(재프로그래밍 시).
 *  3) map 하이퍼콜로 새 매핑을 요청하고 결과 RTE를 data->entry에 캐시한다.
 *  4) RTE의 상/하위 32비트를 IO_APIC_route_entry 비트필드로 해석해
 *     msi_msg의 vector/delivery_mode/dest_mode/ir_format/ir_index로 옮긴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. IRQ 코어가 irq_desc 락을 잡은 상태에서
 * 인터럽트 활성화 시점과 affinity 변경 시점에 호출한다. 잠들 수 없다.
 * 에러 경로: map 하이퍼콜이 실패하면 msg를 채우지 않고 그냥 반환한다. 이 경우
 * 이전 매핑은 이미 해제된 뒤라 해당 인터럽트는 전달되지 않는다(하드 실패).
 *
 * 호출 체인:
 *   irq_chip_compose_msi_msg() / ioapic 활성화 경로 → chip->irq_compose_msi_msg
 *   → [hyperv_root_ir_compose_msi_msg] → hv_unmap_ioapic_interrupt(),
 *      hv_map_ioapic_interrupt()
 */
static void
hyperv_root_ir_compose_msi_msg(struct irq_data *irq_data, struct msi_msg *msg)
{
	/* [한국어] alloc()에서 매달아 둔 per-IRQ 상태(IO-APIC ID, level 여부,
	 * 이전 매핑 캐시)를 꺼낸다. */
	struct hyperv_root_ir_data *data = irq_data->chip_data;
	/* [한국어] 새 map 하이퍼콜이 돌려줄 RTE를 받을 임시 변수. */
	struct hv_interrupt_entry entry;
	/* [한국어] IRQ 코어가 계산해 둔 "유효 affinity" 마스크. */
	const struct cpumask *affinity;
	/* [한국어] 하이퍼바이저가 준 64비트 RTE를 비트필드로 해석하기 위한 뷰. */
	struct IO_APIC_route_entry e;
	/* [한국어] 부모(vector) 도메인이 할당한 벡터 정보. */
	struct irq_cfg *cfg;
	/* [한국어] 목적지 CPU 번호와 IO-APIC ID를 담을 지역 변수. */
	int cpu, ioapic_id;
	/* [한국어] 이 인터럽트에 배정된 x86 인터럽트 벡터 번호(0x20~0xef 범위). */
	u32 vector;

	/* [한국어] 벡터 할당 결과를 읽는다. 벡터 자체는 부모 도메인이 이미 잡아 뒀다. */
	cfg = irqd_cfg(irq_data);
	/* [한국어] "유효 affinity" — 사용자가 요청한 마스크가 아니라 실제로
	 * 커널이 인터럽트를 보내기로 정한 CPU 집합이다. */
	affinity = irq_data_get_effective_affinity_mask(irq_data);
	/* [한국어] IO-APIC RTE는 목적지 CPU를 하나만 지정할 수 있으므로(물리 모드),
	 * 유효 affinity와 온라인 CPU의 교집합에서 첫 번째 CPU를 고른다. */
	cpu = cpumask_first_and(affinity, cpu_online_mask);

	/* [한국어] 하이퍼콜 인자로 넘길 값들을 지역 변수로 복사해 둔다. */
	vector = cfg->vector;
	ioapic_id = data->ioapic_id;

	/* [한국어] 이전에 만들어 둔 매핑이 아직 살아 있는지 검사한다.
	 * source가 IOAPIC 타입이고 RTE 값이 0이 아니면 유효한 매핑이 있다는 뜻이다.
	 * affinity 변경으로 이 함수가 두 번째 이상 불릴 때 이 경로를 탄다. */
	if (data->entry.source == HV_DEVICE_TYPE_IOAPIC
	    && data->entry.ioapic_rte.as_uint64) {
		/* [한국어] 해제할 매핑을 지역 변수로 복사한다(하이퍼콜이 인자를
		 * 수정할 수 있으므로 캐시본을 직접 넘기지 않는다). */
		entry = data->entry;

		/* [한국어] 이전 매핑을 하이퍼바이저에서 회수한다. 반환값을 (void)로
		 * 버리는 이유: 실패해도 어차피 곧 새 매핑을 요청할 것이고, 여기서
		 * 되돌릴 방법도 없기 때문이다. */
		(void)hv_unmap_ioapic_interrupt(ioapic_id, &entry);

		/* [한국어] 캐시를 명시적으로 무효화한다. 이렇게 해 두면 아래 map이
		 * 실패하더라도 free() 경로가 이미 해제된 매핑을 다시 unmap 하지 않는다. */
		data->entry.ioapic_rte.as_uint64 = 0;
		data->entry.source = 0; /* Invalid source */	/* [한국어] source를 0으로 지워 "유효한 매핑 없음" 상태로 만든다. */
	}


	/* [한국어] 새 매핑을 요청한다. 하이퍼바이저가 실제 IO-APIC RTE를 쓰고
	 * 그 결과를 entry에 채워 돌려준다. 실패하면(자원 부족/권한 문제 등)
	 * msg를 건드리지 않고 반환 — 호출자는 이전 msg 내용을 그대로 쓰게 되지만
	 * 이미 매핑이 해제된 뒤라 해당 인터럽트는 전달되지 않는다. */
	if (hv_map_ioapic_interrupt(ioapic_id, data->is_level, cpu,
				    vector, &entry))
		return;

	/* [한국어] 성공한 매핑을 캐시한다. 다음 affinity 변경이나 free 때
	 * 이 값으로 unmap을 걸게 된다. */
	data->entry = entry;

	/* Turn it into an IO_APIC_route_entry, and generate MSI MSG. */
	/* [한국어] 하이퍼바이저가 돌려준 64비트 RTE를 32비트 두 조각으로 나눠
	 * IO_APIC_route_entry 비트필드 뷰에 싣는다. w1이 하위, w2가 상위 워드다. */
	e.w1 = entry.ioapic_rte.low_uint32;
	e.w2 = entry.ioapic_rte.high_uint32;

	/* [한국어] 출력 msi_msg를 0으로 초기화한다. 아래에서 채우지 않는 필드가
	 * 쓰레기 값으로 남지 않게 하기 위함이다. */
	memset(msg, 0, sizeof(*msg));
	/* [한국어] 인터럽트 벡터 번호 — CPU IDT의 어느 엔트리로 갈지 결정한다. */
	msg->arch_data.vector = e.vector;
	/* [한국어] 전달 모드(Fixed/LowestPri/NMI 등). 하이퍼바이저가 정한 값을 그대로 따른다. */
	msg->arch_data.delivery_mode = e.delivery_mode;
	/* [한국어] destination 해석 방식 — 논리(logical) 모드인지 물리 모드인지. */
	msg->arch_addr_lo.dest_mode_logical = e.dest_mode_logical;
	/* [한국어] 이 메시지가 리매핑 형식(remappable format)인지 표시하는 비트.
	 * 이름이 dmar_*인 것은 인텔 DMAR에서 유래한 필드를 재사용하기 때문이다. */
	msg->arch_addr_lo.dmar_format = e.ir_format;
	/* [한국어] 리매핑 테이블 인덱스의 하위 15비트. 하이퍼바이저가 관리하는
	 * 인덱스를 그대로 전달한다 — 리눅스는 이 값의 의미를 해석하지 않는다. */
	msg->arch_addr_lo.dmar_index_0_14 = e.ir_index_0_14;
}

/*
 * [한국어]
 * hyperv_root_ir_set_affinity - 루트 파티션에서 IRQ affinity를 변경한다
 *
 * @data: 이 IR 계층의 irq_data.
 * @mask: 요청된 목적지 CPU 마스크.
 * @force: 강제 설정 여부. 부모 도메인에 그대로 전달한다.
 * @return: 0 또는 부모가 낸 성공/실패 코드.
 *
 * 왜 필요한가: 게스트 버전과 달리 APIC ID 제약 검사가 없다. 루트 파티션에서는
 * RTE를 하이퍼바이저가 쓰므로 8비트 destination 제약이 리눅스의 관심사가
 * 아니기 때문이다. 그래서 하는 일은 부모(vector) 도메인 위임과 옛 벡터 정리
 * 예약뿐이다. 실제 하드웨어 재프로그래밍은 이 함수가 아니라, 벡터가 바뀐 뒤
 * IRQ 코어가 다시 호출하는 hyperv_root_ir_compose_msi_msg()에서 하이퍼콜로
 * 일어난다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, irq_desc 락 보유 상태.
 *
 * 호출 체인:
 *   irq_set_affinity() → chip->irq_set_affinity → [hyperv_root_ir_set_affinity]
 *   → parent->chip->irq_set_affinity (x86 vector 도메인)
 *   → (이후) hyperv_root_ir_compose_msi_msg()
 */
static int hyperv_root_ir_set_affinity(struct irq_data *data,
		const struct cpumask *mask, bool force)
{
	/* [한국어] 실제 벡터를 할당하는 상위 계층(x86 vector 도메인)의 irq_data. */
	struct irq_data *parent = data->parent_data;
	/* [한국어] 옛 벡터 정리 예약에 필요한 벡터 설정 정보. */
	struct irq_cfg *cfg = irqd_cfg(data);
	/* [한국어] 부모 호출 결과를 담는다. */
	int ret;

	/* [한국어] 벡터 재할당을 부모에게 위임한다. 게스트 버전과 달리
	 * 사전 검증 없이 바로 넘긴다. */
	ret = parent->chip->irq_set_affinity(parent, mask, force);
	/* [한국어] 실패했거나 부모가 하드웨어 갱신까지 끝냈다면 그대로 반환한다. */
	if (ret < 0 || ret == IRQ_SET_MASK_OK_DONE)
		return ret;

	/* [한국어] 새 벡터가 잡혔으므로 옛 벡터를 나중에 안전하게 회수하도록
	 * 정리를 예약한다(진행 중인 인터럽트가 끝난 뒤에 반납된다). */
	vector_schedule_cleanup(cfg);

	/* [한국어] 성공 — 호출자가 이어서 compose_msi_msg 경로를 밟게 된다. */
	return 0;
}

/* [한국어] 루트 파티션 모드 IR 계층의 irq_chip.
 * 게스트용 hyperv_ir_chip과 달리 compose_msi_msg 콜백이 추가되어 있는데,
 * 그것이 곧 "RTE를 하이퍼콜로 프로그래밍한다"는 이 모드의 핵심이다. */
static struct irq_chip hyperv_root_ir_chip = {
	/* [한국어] /proc/interrupts에 표시될 이름. 게스트 모드와 구분된다. */
	.name			= "HYPERV-ROOT-IR",
	/* [한국어] 표준 x86 EOI 처리. 로컬 APIC 조작은 루트 파티션도 직접 한다. */
	.irq_ack		= apic_ack_irq,
	/* [한국어] affinity 변경 — 부모에 위임하고 옛 벡터 정리만 예약한다. */
	.irq_set_affinity	= hyperv_root_ir_set_affinity,
	/* [한국어] IO-APIC RTE를 하이퍼콜로 프로그래밍하고 결과를 msi_msg로
	 * 돌려주는 콜백. 이 모드에서 실제 하드웨어가 건드려지는 유일한 지점이다. */
	.irq_compose_msi_msg	= hyperv_root_ir_compose_msi_msg,
};

/*
 * [한국어]
 * hyperv_root_irq_remapping_alloc - 루트 파티션에서 IR 계층 자원을 할당한다
 *
 * @domain: 이 IR irq_domain.
 * @virq: 할당할 가상 IRQ 시작 번호.
 * @nr_irqs: 할당 개수. IO-APIC 핀은 하나씩만 할당하므로 1이어야 한다.
 * @arg: struct irq_alloc_info * — IO-APIC 코어가 채운 할당 정보.
 * @return: 0 성공, -EINVAL(잘못된 요청/irq_data 없음), -ENOMEM, 부모의 음수 errno.
 *
 * 왜 필요한가: 루트 파티션 모드는 IRQ마다 하이퍼바이저 매핑 상태를 기억해야
 * 한다(재프로그래밍/해제 때 이전 매핑을 지목해야 하므로). 그 저장소가
 * struct hyperv_root_ir_data이고, 이 함수가 그것을 만들어 chip_data에 매단다.
 * 게스트 버전이 chip 포인터만 세팅하는 것과 대비된다.
 *
 * 동작 과정:
 *  1) 요청 검증(IO-APIC 타입, nr_irqs == 1).
 *  2) 부모(vector) 도메인이 벡터를 잡게 한다.
 *  3) per-IRQ 상태 구조체를 zalloc.
 *  4) 이 계층의 irq_data를 찾아 chip과 chip_data를 연결.
 *  5) IO-APIC ID와 level/edge 속성을 alloc_info에서 복사해 둔다
 *     — 나중에 compose_msi_msg()가 하이퍼콜 인자로 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, irq_domain_mutex 보유 상태.
 * 에러 경로: 각 단계 실패 시 그때까지 확보한 것(부모 벡터, kzalloc 메모리)을
 * 역순으로 되돌린다.
 *
 * 호출 체인:
 *   mp_irqdomain_alloc() → domain->ops->alloc → [hyperv_root_irq_remapping_alloc]
 *   → irq_domain_alloc_irqs_parent()
 */
static int hyperv_root_irq_remapping_alloc(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs,
				     void *arg)
{
	/* [한국어] IO-APIC 코어가 넘긴 할당 정보 — devid(IO-APIC ID)와
	 * ioapic.is_level(트리거 모드)을 여기서 꺼내 쓴다. */
	struct irq_alloc_info *info = arg;
	/* [한국어] 이 계층의 irq_data. */
	struct irq_data *irq_data;
	/* [한국어] 새로 만들 per-IRQ 상태 구조체. */
	struct hyperv_root_ir_data *data;
	/* [한국어] 부모 도메인 호출 결과. */
	int ret = 0;

	/* [한국어] 이 도메인은 IO-APIC 인터럽트 하나씩만 처리한다.
	 * 게스트 버전과 동일한 검증이다. */
	if (!info || info->type != X86_IRQ_ALLOC_TYPE_IOAPIC || nr_irqs > 1)
		return -EINVAL;

	/* [한국어] 부모(x86 vector) 도메인이 실제 인터럽트 벡터를 할당하게 한다. */
	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, arg);
	/* [한국어] 벡터 확보 실패 — 아직 이 계층에서 잡은 것이 없으니 그냥 반환. */
	if (ret < 0)
		return ret;

	/* [한국어] per-IRQ 상태를 0으로 초기화해 할당한다. entry가 0으로
	 * 시작해야 첫 compose_msi_msg()가 "이전 매핑 없음"으로 올바르게 판단한다. */
	data = kzalloc_obj(*data);
	/* [한국어] 메모리 부족 — 부모가 잡은 벡터를 반드시 되돌려 준다. */
	if (!data) {
		irq_domain_free_irqs_common(domain, virq, nr_irqs);	/* [한국어] 부모(vector) 도메인이 잡은 벡터를 되돌려 누수를 막는다. */
		return -ENOMEM;	/* [한국어] per-IRQ 상태를 만들지 못했으니 메모리 부족을 알린다. */
	}

	/* [한국어] 이 도메인 계층의 irq_data를 찾는다. */
	irq_data = irq_domain_get_irq_data(domain, virq);
	/* [한국어] 정상 흐름에서는 실패하지 않지만 방어적으로 검사한다.
	 * 실패 시 방금 할당한 메모리와 부모 벡터를 모두 되돌린다. */
	if (!irq_data) {
		kfree(data);	/* [한국어] 방금 kzalloc한 상태 구조체를 반납한다. */
		irq_domain_free_irqs_common(domain, virq, nr_irqs);	/* [한국어] 부모가 잡은 벡터까지 모두 되돌린다. */
		return -EINVAL;	/* [한국어] irq_data를 찾지 못한 비정상 상황 — 잘못된 요청으로 보고한다. */
	}

	/* [한국어] 어느 IO-APIC의 핀인지 기록한다. 하이퍼콜의 첫 인자로 쓰인다. */
	data->ioapic_id = info->devid;
	/* [한국어] 레벨/에지 트리거 속성을 기록한다. 하이퍼바이저가 RTE의
	 * trigger mode 비트를 세팅할 때 필요하다. */
	data->is_level = info->ioapic.is_level;

	/* [한국어] 이 계층의 irq_chip을 연결한다 — 이제 compose_msi_msg가
	 * 하이퍼콜 경로로 흐른다. */
	irq_data->chip = &hyperv_root_ir_chip;
	/* [한국어] per-IRQ 상태를 chip_data에 매단다. compose_msi_msg()와
	 * free()가 irq_data->chip_data로 이 구조체를 되찾는다. */
	irq_data->chip_data = data;

	/* [한국어] 할당 완료. 실제 RTE 프로그래밍은 인터럽트가 활성화되어
	 * compose_msi_msg()가 불릴 때 일어난다. */
	return 0;
}

/*
 * [한국어]
 * hyperv_root_irq_remapping_free - 루트 파티션에서 IR 계층 자원을 해제한다
 *
 * @domain: 이 IR irq_domain.
 * @virq: 해제할 가상 IRQ 시작 번호.
 * @nr_irqs: 해제할 개수.
 * @return: 없음.
 *
 * 왜 필요한가: 해제 시 두 가지를 정리해야 한다. (1) 하이퍼바이저에 남아 있는
 * IO-APIC 매핑 — 이것은 하이퍼콜로 명시적으로 회수하지 않으면 하이퍼바이저
 * 쪽에 자원이 누수된다. (2) alloc에서 kzalloc한 per-IRQ 상태 구조체.
 * 그다음에야 공용 헬퍼로 계층 자체를 해제한다.
 *
 * 동작 과정:
 *  1) virq..virq+nr_irqs-1을 순회하며 각 irq_data의 chip_data를 확인.
 *  2) 유효한 하이퍼바이저 매핑이 남아 있으면 hv_unmap_ioapic_interrupt()로 회수.
 *  3) 상태 구조체를 kfree.
 *  4) 루프가 끝난 뒤 부모 계층까지 포함해 IRQ 자원을 해제.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, irq_domain_mutex 보유 상태.
 * 에러 처리: unmap 하이퍼콜의 반환값은 (void)로 버린다 — 해제 경로에서는
 * 실패해도 되돌릴 방법이 없고, 메모리 해제는 어차피 진행해야 하기 때문이다.
 *
 * 호출 체인:
 *   irq_domain_free_irqs() → domain->ops->free → [hyperv_root_irq_remapping_free]
 *   → hv_unmap_ioapic_interrupt(), kfree(), irq_domain_free_irqs_common()
 */
static void hyperv_root_irq_remapping_free(struct irq_domain *domain,
				 unsigned int virq, unsigned int nr_irqs)
{
	/* [한국어] 순회 중인 IRQ의 이 계층 irq_data. */
	struct irq_data *irq_data;
	/* [한국어] 그 irq_data에 매달린 per-IRQ 상태. */
	struct hyperv_root_ir_data *data;
	/* [한국어] 해제할 하이퍼바이저 매핑 캐시를 가리키는 포인터. */
	struct hv_interrupt_entry *e;
	/* [한국어] 순회 인덱스. */
	int i;

	/* [한국어] 요청된 IRQ 범위를 하나씩 돌며 정리한다. IO-APIC 경로에서는
	 * nr_irqs가 1이지만, ops 규약상 범위 처리를 지원해야 한다. */
	for (i = 0; i < nr_irqs; i++) {
		/* [한국어] i번째 IRQ의 이 계층 irq_data를 찾는다. */
		irq_data = irq_domain_get_irq_data(domain, virq + i);

		/* [한국어] alloc이 중간에 실패한 IRQ는 chip_data가 없을 수 있으므로
		 * 둘 다 유효할 때만 정리 작업을 한다. */
		if (irq_data && irq_data->chip_data) {
			/* [한국어] per-IRQ 상태를 되찾는다. */
			data = irq_data->chip_data;
			/* [한국어] 하이퍼바이저 매핑 캐시를 가리킨다. */
			e = &data->entry;

			/* [한국어] 아직 살아 있는 매핑이 있으면(source가 IOAPIC이고
			 * RTE 값이 0이 아니면) 하이퍼바이저에서 회수한다. 이 검사를
			 * 빼면 매핑을 만든 적 없는 IRQ에도 unmap을 걸게 된다. */
			if (e->source == HV_DEVICE_TYPE_IOAPIC &&
			    e->ioapic_rte.as_uint64)
				/* [한국어] 반환값을 버리는 이유: 해제 경로라 실패해도
				 * 되돌릴 수 없고, 아래 kfree는 반드시 진행해야 한다. */
				(void)hv_unmap_ioapic_interrupt(data->ioapic_id,
								&data->entry);

			/* [한국어] alloc에서 kzalloc한 per-IRQ 상태를 반납한다. */
			kfree(data);
		}
	}

	/* [한국어] 마지막으로 이 계층의 irq_data를 떼어내고 부모 계층의 free까지
	 * 연쇄 호출해 벡터를 반납한다. 하이퍼바이저 매핑 회수를 먼저 끝낸 뒤에
	 * 호출해야 chip_data 접근이 안전하다. */
	irq_domain_free_irqs_common(domain, virq, nr_irqs);
}

/* [한국어] 루트 파티션 모드에서 쓰는 irq_domain 콜백 테이블.
 * 위쪽 hyperv_prepare_irq_remapping()이 전방 선언으로 참조하고,
 * hv_root_partition()이 참일 때 이것을 도메인에 연결한다. */
static const struct irq_domain_ops hyperv_root_ir_domain_ops = {
	/* [한국어] IO-APIC 소관 판별 — 게스트 버전과 동일한 함수를 재사용한다.
	 * 어느 모드든 "Hyper-V가 노출하는 유일한 IO-APIC을 맡는다"는 점은 같다. */
	.select = hyperv_irq_remapping_select,
	/* [한국어] per-IRQ 상태 구조체를 만들어 chip_data에 매다는 할당 콜백. */
	.alloc = hyperv_root_irq_remapping_alloc,
	/* [한국어] 하이퍼바이저 매핑 회수와 상태 구조체 반납을 수행하는 해제 콜백. */
	.free = hyperv_root_irq_remapping_free,
};

/* [한국어] CONFIG_IRQ_REMAP 가드의 끝 — 여기까지가 이 파일의 전부다.
 * IR이 꺼진 커널에서는 이 오브젝트가 비게 되고, remap_ops[] 배열도 같은
 * CONFIG로 가드되어 있어 hyperv_irq_remap_ops 미정의로 인한 링크 에러는 없다. */
#endif
