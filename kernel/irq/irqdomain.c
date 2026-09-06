// SPDX-License-Identifier: GPL-2.0

/*
 * [한국어 설명] 인터럽트 도메인 — 하드웨어 번호를 리눅스 번호로 잇는 계층 (irqdomain.c)
 *
 * === 파일의 역할 ===
 * 인터럽트 컨트롤러는 자기 입력을 0 번, 1 번 하는 식으로 센다. 리눅스는
 * 시스템 전체에서 유일한 번호를 따로 매긴다. 컨트롤러가 하나뿐이면 둘을
 * 같게 둘 수도 있지만, 요즘 시스템에는 컨트롤러가 수십 개 있고 각자
 * 0 번부터 세므로 충돌한다.
 *
 * 인터럽트 도메인은 그 번호 공간을 컨트롤러마다 분리하고, "이 컨트롤러의
 * 3 번" 과 "리눅스 번호 47" 을 잇는 사상(mapping)을 관리하는 자료구조다.
 * 이 파일은 그 도메인을 만들고, 사상을 만들고 지우고, 번호를 양방향으로
 * 조회하는 일을 맡는다.
 *
 * 두 번째 역할이 계층형(hierarchical) 도메인이다. 요즘 인터럽트는 여러
 * 컨트롤러를 거쳐 CPU 에 도달한다 — PCI 장치의 MSI 가 IOMMU 리매핑을
 * 거쳐 APIC 에 닿는 식이다. 각 층이 자기 irq_data 를 갖고 parent_data 로
 * 이어지며, 할당·활성화·해제가 그 사슬을 따라 재귀적으로 일어난다.
 *
 * 세 번째 역할은 펌웨어 명세의 해석이다. 디바이스 트리에 적힌
 * "interrupts = <0 42 4>" 같은 값을 하드웨어 번호와 트리거 방식으로
 * 바꾸는 xlate/translate 콜백들이 여기 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 인터럽트가 동작하기까지의 두 축을 이 파일이 잇는다.
 *
 *   (설정 축)
 *     디바이스 트리 / ACPI / PCI 설정 공간
 *       → irq_create_fwspec_mapping()      ← 이 파일
 *       → __irq_domain_alloc_irqs()        ← 이 파일 (계층 전체)
 *       → __irq_alloc_descs()              (kernel/irq/irqdesc.c)
 *       → 리눅스 인터럽트 번호
 *
 *   (실행 축)
 *     하드웨어 인터럽트 → 컨트롤러 드라이버
 *       → generic_handle_domain_irq(domain, hwirq)
 *       → irq_resolve_mapping()            ← 이 파일 (역방향 조회)
 *       → desc->handle_irq()
 *
 * 즉 설정 시점에는 사상을 만들고, 실행 시점에는 그것을 조회한다. 조회는
 * 인터럽트마다 일어나므로 빨라야 하고, 그래서 역방향 맵이 두 가지다 —
 * 작은 번호는 배열(revmap), 큰 번호는 래딕스 트리다.
 *
 * 실행 컨텍스트: 설정 경로는 프로세스 문맥에서 domain->root->mutex 를
 * 잡는다. 조회 경로(__irq_resolve_mapping)는 인터럽트 문맥에서 RCU 로
 * 락 없이 동작한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽(이 파일을 부르는 쪽):
 *   - drivers/irqchip/ 의 모든 컨트롤러 드라이버 → 도메인 생성
 *   - drivers/of/irq.c → irq_create_of_mapping()
 *   - kernel/irq/msi.c → __irq_domain_alloc_irqs(), 도메인 생성
 *   - kernel/irq/irqdesc.c → irq_resolve_mapping() (실행 경로)
 *
 * 아래쪽(이 파일이 부르는 쪽):
 *   - kernel/irq/irqdesc.c → __irq_alloc_descs(), irq_free_descs()
 *   - kernel/irq/chip.c → irq_set_chip_and_handler(), irq_set_status_flags()
 *   - kernel/irq/generic-chip.c → irq_domain_alloc_generic_chips()
 *   - lib/radix-tree.c → 큰 하드웨어 번호의 역방향 맵
 *
 * 공유 자료구조: struct irq_domain 이 중심이다. 그 안의 revmap 배열과
 * revmap_tree 가 역방향 조회를, ops 가 드라이버 콜백을, parent/root 가
 * 계층 구조를 담는다. struct irq_data 는 서술자 안에 박힌 것이
 * 가장 바깥 층이고, 안쪽 층들은 따로 할당되어 parent_data 로 이어진다.
 *
 * === 주요 함수/구조체 요약 ===
 * - irq_domain_instantiate(): 도메인을 만드는 현대적 진입점. 정보
 *   구조체 하나로 모든 설정을 받는다.
 * - irq_create_fwspec_mapping(): 펌웨어 명세를 받아 리눅스 인터럽트
 *   번호를 돌려준다. 디바이스 트리 경로의 핵심.
 * - __irq_domain_alloc_irqs(): 계층 전체에 걸쳐 인터럽트를 할당한다.
 * - __irq_resolve_mapping(): 하드웨어 번호로 서술자를 찾는다. 인터럽트
 *   경로에서 매번 불리므로 가장 빨라야 한다.
 * - irq_domain_activate_irq(): 계층을 재귀적으로 훑으며 하드웨어를
 *   실제로 프로그래밍한다. 할당과 활성화를 나눠 되돌리기를 쉽게 한다.
 * - irq_domain_push_irq() / pop_irq(): 이미 만들어진 계층의 꼭대기에
 *   층을 끼워 넣거나 뺀다.
 */

#define pr_fmt(fmt)  "irq: " fmt	/* [한국어] 이 파일의 모든 pr_* 출력 앞에 "irq: " 를 붙인다. 부팅 로그에서 인터럽트 관련 줄을 골라내기 쉽게 하려는 것이다 */

#include <linux/acpi.h>	/* [한국어] is_acpi_device_node() — ACPI 로 기술된 컨트롤러의 이름 짓기에 필요 */
#include <linux/debugfs.h>	/* [한국어] 파일 끝의 도메인 정보 출력용 */
#include <linux/hardirq.h>	/* [한국어] 인터럽트 문맥 판별 매크로 */
#include <linux/interrupt.h>	/* [한국어] IRQ_TYPE_ 상수와 인터럽트 API 공개 선언 */
#include <linux/irq.h>	/* [한국어] struct irq_data, irq_chip — 도메인이 설정하는 대상 */
#include <linux/irqdesc.h>	/* [한국어] irq_to_desc(), irq_data_to_desc() — 서술자 조회 */
#include <linux/irqdomain.h>	/* [한국어] struct irq_domain, irq_domain_ops — 이 파일이 구현하는 자료구조의 공개 정의 */
#include <linux/module.h>	/* [한국어] EXPORT_SYMBOL_GPL, THIS_MODULE — 서술자의 소유 모듈 기록에도 쓴다 */
#include <linux/mutex.h>	/* [한국어] 도메인 목록과 도메인별 사상을 지키는 두 뮤텍스 */
#include <linux/of.h>	/* [한국어] struct device_node, of_node_to_nid() — 디바이스 트리 연동 */
#include <linux/of_address.h>	/* [한국어] 디바이스 트리 주소 해석 헬퍼 */
#include <linux/of_irq.h>	/* [한국어] struct of_phandle_args — 디바이스 트리의 인터럽트 명세 */
#include <linux/topology.h>	/* [한국어] NUMA_NO_NODE, of_node_to_nid — 서술자를 어느 노드에 둘지 정한다 */
#include <linux/seq_file.h>	/* [한국어] debugfs 출력용 seq_printf */
#include <linux/slab.h>	/* [한국어] kzalloc/kfree — 도메인과 계층 irq_data 할당 */
#include <linux/smp.h>	/* [한국어] smp_mb() — 사상 해제 시의 메모리 장벽 */
#include <linux/fs.h>	/* [한국어] DEFINE_SHOW_ATTRIBUTE 가 쓰는 파일 연산 정의 */

static LIST_HEAD(irq_domain_list);
/* [한국어] 시스템의 모든 인터럽트 도메인을 잇는 연결 리스트.
 * 설정자: __irq_domain_publish() 가 넣고 irq_domain_remove() 가 뺀다.
 * 읽는 자: irq_find_matching_fwspec() 이 펌웨어 명세에 맞는 도메인을
 *   찾을 때 훑고, irq_domain_debugfs_init() 이 항목을 만들 때 훑는다.
 * 값 범위: 빈 리스트 ~ 등록된 도메인 수만큼. 큰 시스템에서 수십 개.
 * 동기화: 아래 irq_domain_mutex 가 지킨다.
 *
 * 선형 검색이라는 점에 주목: 도메인을 찾을 때 전부 훑는다. 도메인
 * 수가 많지 않고 검색이 설정 시점에만 일어나 문제가 되지 않는다.
 * 인터럽트 경로의 조회는 이 리스트를 쓰지 않고 도메인 안의 역방향
 * 맵을 쓴다. */
static DEFINE_MUTEX(irq_domain_mutex);
/* [한국어] 위 도메인 목록과 기본 도메인 포인터를 지키는 뮤텍스.
 * 설정자·읽는 자: 목록에 넣고 빼는 곳, 검색하는 곳, debugfs 항목을
 *   만들고 지우는 곳.
 * 값 범위: 뮤텍스 — 이 아래 경로가 잠들 수 있는 할당을 한다.
 * 동기화: 이것이 동기화 수단 자체다. 도메인별 사상을 지키는
 *   domain->root->mutex 와는 완전히 별개이며, 두 락을 함께 잡는 곳은
 *   없어 순서 문제가 생기지 않는다. */

static struct irq_domain *irq_default_domain;
/* [한국어] 도메인을 지정하지 않았을 때 쓸 기본 도메인.
 * 설정자: irq_set_default_domain(). irq_domain_remove() 도 그 도메인이
 *   사라지면 NULL 로 되돌린다.
 * 읽는 자: irq_create_mapping_affinity(), __irq_resolve_mapping(),
 *   fwspec_to_domain() 등이 NULL 을 받았을 때.
 * 값 범위: NULL 또는 유효한 도메인.
 * 동기화: 설정은 부팅 초기의 단일 스레드 작업이라 락이 없다. 다만
 *   irq_domain_remove() 안의 되돌림은 irq_domain_mutex 아래에서 한다.
 *
 * 왜 이런 것이 남아 있는가: 디바이스 트리나 ACPI 로 컨트롤러를
 * 기술하지 못하는 옛 플랫폼이 인터럽트 번호를 코드에 박아 쓴다.
 * irq_get_default_domain() 의 주석이 새 코드는 쓰지 말라고 못 박는다. */

static int irq_domain_alloc_irqs_locked(struct irq_domain *domain, int irq_base,
					unsigned int nr_irqs, int node, void *arg,
					bool realloc, const struct irq_affinity_desc *affinity);
/* [한국어] 계층형 할당 함수의 전방 선언.
 * 설정자: CONFIG_IRQ_DOMAIN_HIERARCHY 여부에 따라 두 판 중 하나.
 * 읽는 자: irq_create_fwspec_mapping(), __irq_domain_alloc_irqs().
 * 값 범위: 배정된 첫 인터럽트 번호(양수) 또는 음수 오류. 계층형이
 *   없는 빌드에서는 항상 -EINVAL.
 * 동기화: 호출자가 domain->root->mutex 를 쥐고 있어야 한다.
 *
 * 전방 선언이 필요한 이유: 정의가 파일 뒤쪽의 계층형 구역에 있는데
 * 호출은 앞쪽에 있다. */
static void irq_domain_check_hierarchy(struct irq_domain *domain);
/* [한국어] 도메인이 계층형인지 판별해 플래그를 세우는 함수의 전방 선언.
 * 설정자: 계층형 빌드에서는 alloc 콜백 유무를 보고, 아니면 빈 함수.
 * 읽는 자: __irq_domain_create() 가 도메인 생성 마지막에 부른다.
 * 값 범위: 해당 없음 (반환값 없음).
 * 동기화: 아직 아무도 모르는 도메인이라 락이 필요 없다. */
static void irq_domain_free_one_irq(struct irq_domain *domain, unsigned int virq);
/* [한국어] 계층형 인터럽트 하나를 해제하는 함수의 전방 선언.
 * 설정자: 계층형 빌드에서는 MSI 여부에 따라 갈라지고, 아니면 빈 함수.
 * 읽는 자: irq_dispose_mapping().
 * 값 범위: 해당 없음.
 * 동기화: 내부에서 필요한 락을 잡는다. */

struct irqchip_fwid {
	/* [한국어] 디바이스 트리나 ACPI 노드가 없는 컨트롤러를 위한 가짜
	 * 펌웨어 노드. 도메인은 자기를 식별할 fwnode 를 필요로 하는데,
	 * MSI 장치 도메인처럼 순수한 소프트웨어 구성물에는 진짜 노드가
	 * 없다. 그래서 이름만 담은 이것을 만들어 쓴다. */
	struct fwnode_handle	fwnode;
	/* [한국어] 공용 fwnode 인터페이스. 이 구조체의 첫 필드다.
	 * 설정자: __irq_domain_alloc_fwnode() 가 fwnode_init 으로 초기화.
	 * 읽는 자: fwnode 를 받는 모든 코드. container_of 로 이 구조체를
	 *   되찾는다 — 그것이 첫 필드로 둔 이유이기도 하다.
	 * 값 범위: irqchip_fwnode_ops 를 가리키는 초기화된 핸들.
	 * 동기화: 생성 후 변경되지 않는다. */
	struct fwnode_handle	*parent;
	/* [한국어] 상위 펌웨어 노드 (선택적).
	 * 설정자: 생성 시 호출자가 지정한다.
	 * 읽는 자: irqchip_fwnode_get_parent() 를 통해 fwnode 코어가.
	 * 값 범위: NULL 또는 다른 fwnode.
	 * 동기화: 생성 후 변경되지 않는다. */
	unsigned int		type;
	/* [한국어] 이름을 어떻게 지었는지 나타내는 종류.
	 * 설정자: 생성 시.
	 * 읽는 자: irq_domain_set_name() 이 이름 재생성 여부를 정할 때.
	 * 값 범위: IRQCHIP_FWNODE_NAMED, _NAMED_ID, 또는 주소 기반.
	 * 동기화: 생성 후 변경되지 않는다. */
	char			*name;
	/* [한국어] 이 노드의 이름 문자열.
	 * 설정자: 생성 시 kasprintf 로 만든다.
	 * 읽는 자: irqchip_fwnode_get_name(), irq_domain_set_name().
	 * 값 범위: 힙에 할당된 문자열. irq_domain_free_fwnode() 가 해제한다.
	 * 동기화: 생성 후 변경되지 않는다. */
	phys_addr_t		*pa;
	/* [한국어] 컨트롤러의 물리 주소 (선택적).
	 * 설정자: 생성 시.
	 * 읽는 자: 생성 시점의 이름 짓기("irqchip@<주소>")에만 쓰인다.
	 * 값 범위: NULL 또는 물리 주소를 담은 변수의 포인터.
	 * 동기화: 생성 후 변경되지 않는다.
	 * 포인터인 것에 주목: 값이 아니라 호출자의 변수를 가리킨다.
	 *   그 변수가 사라지면 대롱거리는 포인터가 되지만, 실제로는
	 *   이름을 만든 뒤 쓰이지 않아 문제가 되지 않는다. */
};

#ifdef CONFIG_GENERIC_IRQ_DEBUGFS	/* [한국어] debugfs 로 도메인 내부를 들여다보는 빌드 */
static void debugfs_add_domain_dir(struct irq_domain *d);
/* [한국어] debugfs 항목 생성 함수의 전방 선언.
 * 설정자: 파일 끝의 정의.
 * 읽는 자: __irq_domain_publish(), irq_domain_update_bus_token().
 * 값 범위: 해당 없음.
 * 동기화: 호출자가 irq_domain_mutex 를 쥐고 있다. */
static void debugfs_remove_domain_dir(struct irq_domain *d);
/* [한국어] debugfs 항목 제거 함수의 전방 선언.
 * 설정자: 파일 끝의 정의.
 * 읽는 자: irq_domain_remove(), irq_domain_update_bus_token().
 * 값 범위: 해당 없음.
 * 동기화: 위와 같다. */
#else	/* [한국어] debugfs 가 없는 빌드 */
/*
 * [한국어]
 * debugfs_add_domain_dir - debugfs 항목 생성 (빈 함수)
 *
 * @d: 무시
 * @return: 없음
 *
 * 만들 파일 시스템이 없다. 호출부를 #ifdef 로 나누지 않으려고
 * 빈 인라인을 둔다.
 *
 * 호출 체인:
 *   __irq_domain_publish() → [이 함수]
 */
static inline void debugfs_add_domain_dir(struct irq_domain *d) { }	/* [한국어] 만들 것이 없다 */
/*
 * [한국어]
 * debugfs_remove_domain_dir - debugfs 항목 제거 (빈 함수)
 *
 * @d: 무시
 * @return: 없음
 *
 * add 가 아무것도 만들지 않았으므로 지울 것도 없다.
 *
 * 호출 체인:
 *   irq_domain_remove() → [이 함수]
 */
static inline void debugfs_remove_domain_dir(struct irq_domain *d) { }	/* [한국어] 지울 것이 없다 */
#endif	/* [한국어] CONFIG_GENERIC_IRQ_DEBUGFS 분기의 끝 */

/*
 * [한국어]
 * irqchip_fwnode_get_name - 가짜 펌웨어 노드의 이름을 돌려준다
 *
 * @fwnode: 대상 노드
 * @return: 노드 이름 문자열
 *
 * fwnode 코어의 get_name 콜백 구현이다. "%pfw" 형식 지정자나
 * fwnode_get_name() 이 이것을 거쳐 이름을 얻는다.
 *
 * container_of 관용구: fwnode 가 irqchip_fwid 의 첫 필드이므로
 * 주소가 같지만, 그것에 기대지 않고 명시적으로 변환한다. 필드 순서가
 * 바뀌어도 동작하게 하려는 것이다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   fwnode_get_name() / printk 의 "%pfw" → fwnode_ops->get_name → [이 함수]
 */
static const char *irqchip_fwnode_get_name(const struct fwnode_handle *fwnode)
{
	struct irqchip_fwid *fwid = container_of(fwnode, struct irqchip_fwid, fwnode);	/* [한국어] 첫 필드라 주소가 같지만, 필드 순서가 바뀌어도 동작하도록 명시적으로 변환한다 */

	return fwid->name;	/* [한국어] 생성 시 kasprintf 로 만든 문자열 */
}

/*
 * [한국어]
 * irqchip_fwnode_get_parent - 가짜 펌웨어 노드의 부모를 돌려준다
 *
 * @fwnode: 대상 노드
 * @return: 부모 노드, 없으면 NULL
 *
 * fwnode 코어의 get_parent 콜백 구현이다. 대부분의 가짜 노드는
 * 부모가 없어 NULL 이 나온다.
 *
 * 부모가 있는 경우: 계층형 컨트롤러에서 상위 노드를 알아야 펌웨어
 * 명세를 따라 올라갈 수 있는 경우다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   fwnode_get_parent() → fwnode_ops->get_parent → [이 함수]
 */
static struct fwnode_handle *irqchip_fwnode_get_parent(const struct fwnode_handle *fwnode)
{
	struct irqchip_fwid *fwid = container_of(fwnode, struct irqchip_fwid, fwnode);	/* [한국어] 감싸는 구조체를 되찾는다 */

	return fwid->parent;	/* [한국어] 대부분 NULL 이다 */
}

const struct fwnode_operations irqchip_fwnode_ops = {
	/* [한국어] 가짜 펌웨어 노드의 연산표.
	 * fwnode_init() 이 이 주소를 노드에 심으면, fwnode 코어가 이
	 * 노드를 다른 펌웨어 노드와 똑같이 다룰 수 있게 된다.
	 * 두 콜백만 있는 것에 주목 — 속성 읽기나 자식 순회는 지원하지
	 * 않는다. 가짜 노드에는 그런 것이 없기 때문이다. */
	.get_name = irqchip_fwnode_get_name,
	/* [한국어] 이름을 돌려주는 콜백.
	 * 설정자: 여기 정적 초기화.
	 * 읽는 자: fwnode_get_name(), printk 의 "%pfw".
	 * 값 범위: 항상 위 함수.
	 * 동기화: const 라 변경되지 않는다. */
	.get_parent = irqchip_fwnode_get_parent,
	/* [한국어] 부모 노드를 돌려주는 콜백.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: 항상 위 함수.
	 * 동기화: 위와 같다. */
};
EXPORT_SYMBOL_GPL(irqchip_fwnode_ops);	/* [한국어] is_fwnode_irqchip() 매크로가 이 주소와 비교해 가짜 노드를 판별하므로, 모듈에서도 그 판별이 가능해야 한다 */

/**
 * __irq_domain_alloc_fwnode - Allocate a fwnode_handle suitable for
 *                           identifying an irq domain
 * @type:	Type of irqchip_fwnode. See linux/irqdomain.h
 * @id:		Optional user provided id if name != NULL
 * @name:	Optional user provided domain name
 * @pa:		Optional user-provided physical address
 * @parent:	Optional parent fwnode_handle
 *
 * Allocate a struct irqchip_fwid, and return a pointer to the embedded
 * fwnode_handle (or NULL on failure).
 *
 * Note: The types IRQCHIP_FWNODE_NAMED and IRQCHIP_FWNODE_NAMED_ID are
 * solely to transport name information to irqdomain creation code. The
 * node is not stored. For other types the pointer is kept in the irq
 * domain struct.
 */
/*
 * [한국어]
 * __irq_domain_alloc_fwnode - 도메인 식별용 가짜 펌웨어 노드를 만든다
 *
 * @type:   이름 짓는 방식 (IRQCHIP_FWNODE_NAMED 등)
 * @id:     이름에 붙일 번호 (NAMED_ID 일 때만)
 * @name:   이름 문자열 (NAMED 계열일 때)
 * @pa:     컨트롤러 물리 주소 (그 외의 경우 이름에 쓴다)
 * @parent: 상위 노드 (선택적)
 * @return: 만든 노드의 fwnode 포인터, 실패 시 NULL
 *
 * 디바이스 트리나 ACPI 에 기술되지 않은 컨트롤러가 자기 도메인을
 * 식별할 노드를 얻는 통로다. MSI 장치 도메인처럼 순수한 소프트웨어
 * 구성물이나, 부팅 초기에 하드코딩으로 만들어지는 컨트롤러가 쓴다.
 *
 * 원본 주석의 구분이 중요하다. NAMED 계열 노드는 이름을 전달하는
 * 용도로만 쓰이고 도메인에 저장되지 않는다 — 도메인이 이름을 복사해
 * 가면 노드는 버려도 된다. 다른 종류는 도메인이 포인터를 계속 들고
 * 있으므로 살아 있어야 한다.
 *
 * 할당 실패 검사가 뒤로 밀린 것에 주목: kzalloc 과 kasprintf 를 먼저
 * 다 하고 나서 함께 검사한다. 검사를 나누면 각각의 실패 경로에서
 * 앞의 것을 되돌려야 해 코드가 길어진다. kfree(NULL) 이 안전해
 * 이렇게 합칠 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥. GFP_KERNEL 할당이라 잠들 수 있다.
 *
 * 호출 체인:
 *   irqchip 드라이버 초기화 / msi_create_device_irq_domain() → [이 함수]
 */
struct fwnode_handle *__irq_domain_alloc_fwnode(unsigned int type, int id,
						const char *name,
						phys_addr_t *pa,
						struct fwnode_handle *parent)
{
	struct irqchip_fwid *fwid;	/* [한국어] 만들 노드 */
	char *n;	/* [한국어] 이름 문자열 */

	fwid = kzalloc_obj(*fwid);	/* [한국어] 0 초기화. 실패해도 아래에서 함께 검사한다 */

	switch (type) {	/* [한국어] 종류에 따라 이름 짓는 방식이 다르다 */
	case IRQCHIP_FWNODE_NAMED:	/* [한국어] 호출자가 준 이름 그대로 */
		n = kasprintf(GFP_KERNEL, "%s", name);	/* [한국어] 복사한다. 호출자의 문자열이 사라질 수 있어 참조만 들 수 없다 */
		break;
	case IRQCHIP_FWNODE_NAMED_ID:	/* [한국어] 이름에 번호를 붙인다 */
		n = kasprintf(GFP_KERNEL, "%s-%d", name, id);	/* [한국어] 같은 이름의 컨트롤러가 여럿일 때 구분한다 */
		break;
	default:	/* [한국어] 이름 없이 물리 주소로 */
		n = kasprintf(GFP_KERNEL, "irqchip@%pa", pa);	/* [한국어] "irqchip@fe600000" 처럼. 주소가 곧 유일한 식별자가 된다 */
		break;
	}

	if (!fwid || !n) {	/* [한국어] 두 할당을 함께 검사한다. 나누면 각 실패 경로에서 앞의 것을 되돌려야 해 길어진다 */
		kfree(fwid);	/* [한국어] NULL 이어도 안전하다 */
		kfree(n);	/* [한국어] 마찬가지 */
		return NULL;	/* [한국어] 두 할당 중 하나라도 실패했다. 위에서 함께 되돌렸으므로 새는 것이 없다 */
	}

	fwid->type = type;	/* [한국어] 이름 짓는 방식. 도메인 생성 시 이름을 다시 만들지 결정하는 데 쓴다 */
	fwid->name = n;	/* [한국어] 방금 만든 이름 */
	fwid->pa = pa;	/* [한국어] 물리 주소. 이름을 만든 뒤로는 쓰이지 않는다 */
	fwid->parent = parent;	/* [한국어] 상위 노드 */
	fwnode_init(&fwid->fwnode, &irqchip_fwnode_ops);	/* [한국어] 공용 인터페이스를 심는다. 이 뒤로 fwnode 코어가 이것을 진짜 펌웨어 노드처럼 다룬다 */
	return &fwid->fwnode;	/* [한국어] 첫 필드의 주소. 호출자는 감싸는 구조체를 몰라도 된다 */
}
EXPORT_SYMBOL_GPL(__irq_domain_alloc_fwnode);	/* [한국어] irqchip 드라이버가 모듈일 수 있다 */

/**
 * irq_domain_free_fwnode - Free a non-OF-backed fwnode_handle
 * @fwnode: fwnode_handle to free
 *
 * Free a fwnode_handle allocated with irq_domain_alloc_fwnode.
 */
/*
 * [한국어]
 * irq_domain_free_fwnode - 가짜 펌웨어 노드를 해제한다
 *
 * @fwnode: 해제할 노드
 * @return: 없음
 *
 * 위 함수의 반대다. is_fwnode_irqchip() 검사가 이 함수의 안전장치다 —
 * 진짜 디바이스 트리 노드를 넘기면 그것은 이 파일이 만든 것이 아니라
 * 해제하면 안 된다.
 *
 * 그 판별은 노드의 ops 포인터가 irqchip_fwnode_ops 인지로 한다. 그래서
 * 그 심볼을 EXPORT 해야 했다.
 *
 * NULL 을 조용히 넘기는 것에 주목: 정리 경로에서 "만들었을 수도
 * 아닐 수도 있는" 노드를 조건 없이 넘길 수 있게 한다.
 * msi_create_device_irq_domain() 의 __free 속성이 그렇게 쓴다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   msi_remove_device_irq_domain() / irqchip 드라이버 정리 → [이 함수]
 */
void irq_domain_free_fwnode(struct fwnode_handle *fwnode)
{
	struct irqchip_fwid *fwid;	/* [한국어] 감싸는 구조체 */

	if (!fwnode || WARN_ON(!is_fwnode_irqchip(fwnode)))	/* [한국어] NULL 은 조용히 넘기고, 진짜 펌웨어 노드는 경고한다 — 그것은 이 파일이 만든 것이 아니라 해제하면 안 된다 */
		return;

	fwid = container_of(fwnode, struct irqchip_fwid, fwnode);	/* [한국어] 감싸는 구조체를 되찾는다 */
	kfree(fwid->name);	/* [한국어] 이름 문자열 먼저 */
	kfree(fwid);	/* [한국어] 구조체 본체 */
}
EXPORT_SYMBOL_GPL(irq_domain_free_fwnode);	/* [한국어] 만드는 쪽과 짝을 이룬다 */

/*
 * [한국어]
 * alloc_name - 기본 이름에 버스 토큰을 붙여 도메인 이름을 만든다
 *
 * @domain:    대상 도메인
 * @base:      기본 이름
 * @bus_token: 버스 종류 표식
 * @return:    0 성공, -ENOMEM 할당 실패
 *
 * 도메인 이름은 debugfs 디렉터리 이름이 되므로 시스템 안에서 유일해야
 * 한다. 같은 컨트롤러 위에 종류가 다른 도메인이 여럿 얹힐 수 있어
 * (예: 배선 인터럽트용과 MSI 용), 버스 토큰을 이름에 붙여 구분한다.
 *
 * DOMAIN_BUS_ANY 는 "종류 구분 없음" 이므로 붙일 것이 없다.
 *
 * IRQ_DOMAIN_NAME_ALLOCATED 플래그를 세우는 것이 중요하다. 이름이
 * 힙에 있으니 도메인을 없앨 때 해제해야 한다는 표시다. 어떤 도메인은
 * 이름을 복사하지 않고 fwnode 의 것을 그대로 가리키는데, 그때 해제하면
 * 남의 메모리를 푼다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 도메인 생성 경로.
 *
 * 호출 체인:
 *   irq_domain_set_name() → [이 함수]
 */
static int alloc_name(struct irq_domain *domain, char *base, enum irq_domain_bus_token bus_token)
{
	if (bus_token == DOMAIN_BUS_ANY)	/* [한국어] 종류 구분이 없는 도메인인가 */
		domain->name = kasprintf(GFP_KERNEL, "%s", base);	/* [한국어] 기본 이름 그대로 복사한다 */
	else	/* [한국어] 종류가 정해진 도메인 */
		domain->name = kasprintf(GFP_KERNEL, "%s-%d", base, bus_token);	/* [한국어] 같은 컨트롤러 위에 여러 종류의 도메인이 얹힐 수 있어 구분자가 필요하다 */
	if (!domain->name)	/* [한국어] 할당 실패 */
		return -ENOMEM;

	domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;	/* [한국어] 이름이 힙에 있으니 해제해야 한다는 표시. 이것이 없으면 남의 메모리를 풀거나 새게 된다 */
	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * alloc_fwnode_name - 펌웨어 노드 경로로 도메인 이름을 만든다
 *
 * @domain:    대상 도메인
 * @fwnode:    이름의 바탕이 될 펌웨어 노드
 * @bus_token: 버스 종류 표식
 * @suffix:    이름 뒤에 붙일 문자열 (선택적)
 * @return:    0 성공, -ENOMEM 할당 실패
 *
 * 디바이스 트리나 ACPI 로 기술된 컨트롤러의 도메인 이름을 만든다.
 * "%pfw" 형식 지정자가 노드의 전체 경로를 찍는다 — 예를 들어
 * "/soc/interrupt-controller@fe600000" 이다.
 *
 * suffix 가 필요한 이유는 위 alloc_name 과 다르다. 그쪽은 종류가
 * 다른 도메인을 구분하지만, 이쪽은 한 장치가 물리 인터럽트를 여러 개
 * 제공해 도메인이 여럿 필요한 경우다 — regmap-IRQ 컨트롤러가 그렇다.
 * 그러면 같은 노드 경로에서 이름이 겹친다.
 *
 * '/' 를 ':' 로 바꾸는 것이 이 함수에서 가장 실용적인 부분이다.
 * 원본 주석의 농담대로, debugfs 는 경로에 '/' 가 들어가는 것을
 * 싫어한다 — 그것은 디렉터리 구분자이기 때문이다.
 *
 * strreplace 가 제자리에서 고치고 같은 포인터를 돌려주므로,
 * domain->name 에 대입하는 것과 name 변수는 같은 메모리를 가리킨다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 도메인 생성 경로.
 *
 * 호출 체인:
 *   irq_domain_set_name() → [이 함수]
 */
static int alloc_fwnode_name(struct irq_domain *domain, const struct fwnode_handle *fwnode,
			     enum irq_domain_bus_token bus_token, const char *suffix)
{
	const char *sep = suffix ? "-" : "";	/* [한국어] 접미사가 있을 때만 구분자를 넣는다. 없으면 빈 문자열이라 아무 영향이 없다 */
	const char *suf = suffix ? : "";	/* [한국어] NULL 을 빈 문자열로 바꾼다. printf 에 NULL 을 넘기면 "(null)" 이 찍힌다 */
	char *name;	/* [한국어] 만든 이름 */

	if (bus_token == DOMAIN_BUS_ANY)	/* [한국어] 종류 구분이 없는가 */
		name = kasprintf(GFP_KERNEL, "%pfw%s%s", fwnode, sep, suf);	/* [한국어] "%pfw" 는 노드의 전체 경로를 찍는다 */
	else	/* [한국어] 종류가 정해진 경우 */
		name = kasprintf(GFP_KERNEL, "%pfw%s%s-%d", fwnode, sep, suf, bus_token);	/* [한국어] 경로 + 접미사 + 종류 */
	if (!name)	/* [한국어] 할당 실패 */
		return -ENOMEM;

	/*
	 * fwnode paths contain '/', which debugfs is legitimately unhappy
	 * about. Replace them with ':', which does the trick and is not as
	 * offensive as '\'...
	 */
	domain->name = strreplace(name, '/', ':');	/* [한국어] (위 영어 주석) 경로 구분자를 콜론으로 바꾼다. debugfs 파일 이름에 '/' 가 들어가면 디렉터리로 해석된다. 제자리에서 고치고 같은 포인터를 돌려준다 */
	domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;	/* [한국어] 힙에 있으니 해제 대상이다 */
	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * alloc_unknown_name - 이름을 지을 근거가 없을 때 번호로 짓는다
 *
 * @domain:    대상 도메인
 * @bus_token: 버스 종류 표식
 * @return:    0 성공, -ENOMEM 할당 실패
 *
 * 최후의 수단이다. 펌웨어 노드도 없고 이름도 주어지지 않은 도메인이
 * 그래도 debugfs 항목을 가지려면 유일한 이름이 필요하다.
 *
 * 정적 원자 카운터를 쓰는 것에 주목: 함수 안에 static 으로 두어 이
 * 함수만 만질 수 있게 했다. 원자 증가라 여러 CPU 가 동시에 불러도
 * 번호가 겹치지 않는다.
 *
 * 그런데 이 경로로 오는 것은 대개 잘못이다. 호출자인
 * irq_domain_set_name() 이 그 직전에 오류를 찍는다 — 이름을 지을
 * 근거가 없다는 것은 도메인 설정이 불완전하다는 뜻이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 도메인 생성 경로.
 *
 * 호출 체인:
 *   irq_domain_set_name() 의 마지막 경로 → [이 함수]
 */
static int alloc_unknown_name(struct irq_domain *domain, enum irq_domain_bus_token bus_token)
{
	static atomic_t unknown_domains;	/* [한국어] 함수 안 static 이라 이 함수만 만진다. 이름 없는 도메인에 번호를 매기는 카운터 */
	int id = atomic_inc_return(&unknown_domains);	/* [한국어] 원자 증가라 여러 CPU 가 동시에 불러도 번호가 겹치지 않는다 */

	if (bus_token == DOMAIN_BUS_ANY)	/* [한국어] 종류 구분이 없는가 */
		domain->name = kasprintf(GFP_KERNEL, "unknown-%d", id);	/* [한국어] "unknown-3" 처럼. 유일하기만 하면 된다 */
	else	/* [한국어] 종류가 있는 경우 */
		domain->name = kasprintf(GFP_KERNEL, "unknown-%d-%d", id, bus_token);	/* [한국어] 번호와 종류 둘 다 */
	if (!domain->name)	/* [한국어] 할당 실패 */
		return -ENOMEM;

	domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;	/* [한국어] 힙에 있으니 해제 대상이다 */
	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * irq_domain_set_name - 도메인 이름을 정한다
 *
 * @domain: 대상 도메인
 * @info:   생성 정보 (fwnode, bus_token, name_suffix)
 * @return: 0 성공, -EINVAL 접미사를 쓸 수 없는 조합, -ENOMEM 할당 실패
 *
 * 이름의 출처를 세 갈래로 나누는 함수다.
 *
 * (1) 가짜 펌웨어 노드(irqchip_fwid): 그 안의 이름을 쓴다. 다만
 *     NAMED 계열은 복사하고, 그 외에는 포인터를 그대로 가리킨다.
 *     차이가 나는 이유는 NAMED 노드가 이름 전달용 임시 물건이라
 *     곧 버려질 수 있어서다.
 * (2) 진짜 펌웨어 노드(DT/ACPI/소프트웨어 노드): 노드 경로로 짓는다.
 * (3) 그 외: 이미 이름이 있으면 그대로, 없으면 번호로 짓는다.
 *
 * 접미사를 가짜 노드에 쓰지 못하게 막는 이유가 원본 주석에 있다.
 * 접미사는 "진짜 장치 노드에서 이름을 따올 때 여러 도메인의 이름이
 * 겹치는 것" 을 피하려는 장치다. 가짜 노드는 이름을 마음대로 지을 수
 * 있으니 그 문제 자체가 없고, 접미사를 쓰려는 것은 호출자의 혼동이다.
 *
 * (1) 의 default 갈래가 미묘하다. 포인터를 그대로 쓰되, 버스 토큰이
 * 있으면 결국 복사한다 — 토큰을 붙인 새 문자열이 필요하기 때문이다.
 * 그러지 않은 경우에는 NAME_ALLOCATED 플래그가 세워지지 않아 해제되지
 * 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 도메인 생성 경로.
 *
 * 호출 체인:
 *   __irq_domain_create() → [이 함수] → alloc_name() 계열
 */
static int irq_domain_set_name(struct irq_domain *domain, const struct irq_domain_info *info)
{
	enum irq_domain_bus_token bus_token = info->bus_token;	/* [한국어] 도메인 종류 표식 */
	const struct fwnode_handle *fwnode = info->fwnode;	/* [한국어] 이름의 바탕이 될 노드. NULL 일 수 있다 */

	if (is_fwnode_irqchip(fwnode)) {	/* [한국어] 이 파일이 만든 가짜 노드인가 */
		const struct irqchip_fwid *fwid = container_of(fwnode, struct irqchip_fwid, fwnode);	/* [한국어] 감싸는 구조체를 되찾아 이름과 종류를 본다 */

		/*
		 * The name_suffix is only intended to be used to avoid a name
		 * collision when multiple domains are created for a single
		 * device and the name is picked using a real device node.
		 * (Typical use-case is regmap-IRQ controllers for devices
		 * providing more than one physical IRQ.) There should be no
		 * need to use name_suffix with irqchip-fwnode.
		 */
		if (info->name_suffix)	/* [한국어] (위 영어 주석) 가짜 노드에 접미사를 쓰려 하는가 */
			return -EINVAL;	/* [한국어] 가짜 노드는 이름을 마음대로 지을 수 있어 충돌 회피 장치가 필요 없다. 쓰려는 것은 호출자의 혼동이다 */

		switch (fwid->type) {	/* [한국어] 가짜 노드의 이름 짓기 방식 */
		case IRQCHIP_FWNODE_NAMED:	/* [한국어] 이름을 전달하려고 만든 임시 노드 */
		case IRQCHIP_FWNODE_NAMED_ID:	/* [한국어] 번호가 붙은 같은 종류 */
			return alloc_name(domain, fwid->name, bus_token);	/* [한국어] 복사한다. 이 노드는 곧 버려질 수 있어 포인터를 들 수 없다 */
		default:	/* [한국어] 주소 기반 이름의 영구 노드 */
			domain->name = fwid->name;	/* [한국어] 포인터를 그대로 가리킨다. NAME_ALLOCATED 를 세우지 않으므로 해제되지 않는다 — 노드가 도메인보다 오래 산다는 전제다 */
			if (bus_token != DOMAIN_BUS_ANY)	/* [한국어] 종류 표식을 붙여야 하는가 */
				return alloc_name(domain, fwid->name, bus_token);	/* [한국어] 그러면 결국 새 문자열이 필요해 복사한다 */
		}

	} else if (is_of_node(fwnode) || is_acpi_device_node(fwnode) || is_software_node(fwnode)) {	/* [한국어] 진짜 펌웨어 노드인가 — 디바이스 트리, ACPI, 또는 소프트웨어 노드 */
		return alloc_fwnode_name(domain, fwnode, bus_token, info->name_suffix);	/* [한국어] 노드 경로로 짓는다. 접미사는 여기서만 의미가 있다 */
	}

	if (domain->name)	/* [한국어] 위 default 갈래에서 포인터를 가리킨 경우인가 */
		return 0;	/* [한국어] 이미 이름이 있다 */

	if (fwnode)	/* [한국어] 노드는 있는데 위 어느 종류도 아닌가 */
		pr_err("Invalid fwnode type for irqdomain\n");	/* [한국어] 도메인 설정이 불완전하다는 신호. 그래도 진행은 한다 */
	return alloc_unknown_name(domain, bus_token);	/* [한국어] 최후의 수단 — 번호로 짓는다. 이름이 없으면 debugfs 항목을 만들 수 없다 */
}

/*
 * [한국어]
 * __irq_domain_create - 도메인 구조체를 할당하고 기본 필드를 채운다
 *
 * @info: 생성 정보
 * @return: 만든 도메인, 실패 시 ERR_PTR
 *
 * 도메인 생성의 첫 단계다. 메모리를 잡고 이름을 정하고 필드를 채운다.
 * 아직 전역 목록에 넣지 않으므로 다른 CPU 는 이 도메인을 모른다.
 *
 * 첫 검사 세 가지가 각각 다른 모순을 잡는다.
 *   - size 와 direct_max 를 함께 준 경우: 전자는 선형 배열 크기,
 *     후자는 "리눅스 번호를 하드웨어 번호로 그대로 쓰는" 방식의
 *     상한이다. 두 방식은 배타적이다.
 *   - NOMAP 을 빌드에서 뺐는데 direct_max 를 준 경우.
 *   - direct_max 와 hwirq_max 가 다른 경우: 직접 사상에서는 두 상한이
 *     같은 것을 뜻하므로 다르면 호출자가 뜻을 잘못 이해한 것이다.
 *
 * struct_size 로 revmap 배열을 구조체 뒤에 이어 붙인다. 그 배열이
 * 작은 하드웨어 번호의 빠른 역방향 조회를 담당한다. size 가 0 이면
 * 배열이 없고 모든 조회가 래딕스 트리로 간다.
 *
 * 마지막의 root 자기 참조가 계층형 락 설계의 바탕이다. 원본 주석대로,
 * 계층의 모든 층이 가장 안쪽 도메인의 뮤텍스 하나를 공유한다. 그래야
 * 계층 전체를 한 임계 구역에서 다룰 수 있다. 비계층 도메인은 자기가
 * 곧 root 이므로 domain->root->mutex 가 항상 올바른 락을 가리킨다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   __irq_domain_instantiate() → [이 함수] → irq_domain_set_name()
 */
static struct irq_domain *__irq_domain_create(const struct irq_domain_info *info)
{
	struct irq_domain *domain;	/* [한국어] 만들 도메인 */
	int err;	/* [한국어] 이름 짓기 결과 */

	if (WARN_ON((info->size && info->direct_max) ||	/* [한국어] 선형 배열과 직접 사상을 함께 요구했는가 — 두 방식은 배타적이다 */
		    (!IS_ENABLED(CONFIG_IRQ_DOMAIN_NOMAP) && info->direct_max) ||	/* [한국어] 직접 사상을 빌드에서 뺐는데 요구하는가 */
		    (info->direct_max && info->direct_max != info->hwirq_max)))	/* [한국어] 직접 사상에서는 두 상한이 같은 것을 뜻한다. 다르면 호출자가 뜻을 잘못 이해했다 */
		return ERR_PTR(-EINVAL);

	domain = kzalloc_node(struct_size(domain, revmap, info->size),	/* [한국어] 구조체 + 역방향 배열을 한 덩어리로. 배열이 작은 하드웨어 번호의 빠른 조회를 맡는다 */
			      GFP_KERNEL, of_node_to_nid(to_of_node(info->fwnode)));	/* [한국어] 컨트롤러가 붙은 NUMA 노드에 둔다. 인터럽트 경로에서 매번 읽히므로 지역성이 중요하다 */
	if (!domain)	/* [한국어] 메모리 부족 */
		return ERR_PTR(-ENOMEM);

	err = irq_domain_set_name(domain, info);	/* [한국어] 이름 짓기. debugfs 항목 이름이 되므로 유일해야 한다 */
	if (err) {	/* [한국어] 실패 */
		kfree(domain);	/* [한국어] 방금 잡은 것을 되돌린다 */
		return ERR_PTR(err);	/* [한국어] 이름 짓기 실패. 도메인 메모리는 위에서 되돌렸다 */
	}

	domain->fwnode = fwnode_handle_get(info->fwnode);	/* [한국어] 참조를 잡아 둔다. 도메인이 사는 동안 노드가 사라지면 안 된다 */
	fwnode_dev_initialized(domain->fwnode, true);	/* [한국어] "이 노드의 장치가 초기화됐다" 고 표시한다. 디바이스 트리의 지연 프로브(deferred probe) 판단에 쓰인다 */

	/* Fill structure */
	INIT_RADIX_TREE(&domain->revmap_tree, GFP_KERNEL);	/* [한국어] (위 영어 주석) 큰 하드웨어 번호의 역방향 맵. 선형 배열 범위를 넘는 번호가 여기 들어간다 */
	domain->ops = info->ops;	/* [한국어] 드라이버가 제공한 콜백 묶음 — map, alloc, translate 등 */
	domain->host_data = info->host_data;	/* [한국어] 드라이버의 사설 데이터. MSI 도메인은 여기에 msi_domain_info 를 둔다 */
	domain->bus_token = info->bus_token;	/* [한국어] 도메인 종류 표식. 검색할 때 종류를 구분한다 */
	domain->hwirq_max = info->hwirq_max;	/* [한국어] 하드웨어 번호의 상한. 사상을 만들 때 범위 검사에 쓴다 */

	if (info->direct_max)	/* [한국어] 리눅스 번호를 하드웨어 번호로 그대로 쓰는 방식인가 */
		domain->flags |= IRQ_DOMAIN_FLAG_NO_MAP;	/* [한국어] 역방향 맵 자체가 필요 없다는 표시. 조회가 항등 함수가 된다 */

	domain->revmap_size = info->size;	/* [한국어] 선형 배열의 크기. 이 값보다 작은 하드웨어 번호는 배열로, 큰 것은 래딕스 트리로 조회한다 */

	/*
	 * Hierarchical domains use the domain lock of the root domain
	 * (innermost domain).
	 *
	 * For non-hierarchical domains (as for root domains), the root
	 * pointer is set to the domain itself so that &domain->root->mutex
	 * always points to the right lock.
	 */
	mutex_init(&domain->mutex);	/* [한국어] (위 영어 주석) 이 도메인의 사상을 지킬 뮤텍스. 계층의 안쪽 도메인만 실제로 쓰인다 */
	domain->root = domain;	/* [한국어] 자기 참조. 계층에 붙으면 부모의 root 로 덮이지만, 그러기 전에도 domain->root->mutex 가 올바른 락을 가리키게 한다 */

	irq_domain_check_hierarchy(domain);	/* [한국어] alloc 콜백이 있으면 계층형으로 표시한다. 계층형 여부가 이후 할당 경로를 가른다 */

	return domain;	/* [한국어] 아직 전역 목록에 없으므로 다른 CPU 는 이 도메인을 모른다 */
}

/*
 * [한국어]
 * __irq_domain_publish - 도메인을 전역 목록에 등록한다
 *
 * @domain: 등록할 도메인
 * @return: 없음
 *
 * 이 함수가 끝나는 순간부터 다른 CPU 가 irq_find_matching_fwspec() 으로
 * 이 도메인을 찾을 수 있다. 그래서 도메인이 완전히 준비된 뒤에
 * 불려야 한다.
 *
 * debugfs 항목을 같은 락 안에서 만드는 이유: 목록에 있는 도메인과
 * debugfs 에 보이는 도메인이 어긋나면 진단이 혼란스럽다. 특히
 * irq_domain_debugfs_init() 이 같은 락 아래에서 목록을 훑으므로,
 * 두 작업이 겹쳐 항목이 두 번 만들어지는 것을 막는다.
 *
 * list_add 를 쓰는 것에 주목: 꼬리가 아니라 머리에 넣는다. 나중에
 * 만들어진 도메인이 검색에서 먼저 발견된다. 원본 주석이 검색
 * 함수에서 언급하듯, 모든 인터럽트에 맞는 레거시 컨트롤러가 있을 수
 * 있어 그것을 뒤로 미루는 편이 낫기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   __irq_domain_instantiate() → [이 함수]
 */
static void __irq_domain_publish(struct irq_domain *domain)
{
	mutex_lock(&irq_domain_mutex);	/* [한국어] 전역 목록과 debugfs 항목을 함께 지킨다 */
	debugfs_add_domain_dir(domain);	/* [한국어] 목록 등록과 같은 락 안에서. 어긋나면 진단이 혼란스럽고, debugfs 초기화와 겹쳐 두 번 만들어질 수 있다 */
	list_add(&domain->link, &irq_domain_list);	/* [한국어] 머리에 넣는다. 나중에 만든 도메인이 검색에서 먼저 발견되어, 모든 것에 맞는 레거시 컨트롤러가 뒤로 밀린다 */
	mutex_unlock(&irq_domain_mutex);	/* [한국어] 이 시점부터 다른 CPU 가 이 도메인을 찾을 수 있다 */

	pr_debug("Added domain %s\n", domain->name);	/* [한국어] 부팅 시 도메인 생성 순서를 추적할 때 유용하다 */
}

/*
 * [한국어]
 * irq_domain_free - 도메인 구조체와 그에 딸린 것들을 해제한다
 *
 * @domain: 해제할 도메인
 * @return: 없음
 *
 * __irq_domain_create() 의 반대다. 참조를 놓고 이름을 풀고 구조체를
 * 해제한다.
 *
 * fwnode_dev_initialized(false) 를 먼저 부르는 이유: 그 표시는 참조를
 * 놓기 전에 되돌려야 한다. 참조를 먼저 놓으면 노드가 사라져 표시를
 * 되돌릴 대상이 없어진다.
 *
 * 이름 해제가 조건부인 것이 핵심이다. NAME_ALLOCATED 플래그가 없는
 * 도메인은 이름이 fwnode 안의 문자열을 가리키고 있어, 해제하면 남의
 * 메모리를 푼다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_domain_remove() / __irq_domain_instantiate() 의 실패 경로 →
 *   [이 함수]
 */
static void irq_domain_free(struct irq_domain *domain)
{
	fwnode_dev_initialized(domain->fwnode, false);	/* [한국어] 초기화 표시를 되돌린다. 참조를 놓기 전에 해야 대상이 살아 있다 */
	fwnode_handle_put(domain->fwnode);	/* [한국어] 생성 때 잡은 참조를 놓는다 */
	if (domain->flags & IRQ_DOMAIN_NAME_ALLOCATED)	/* [한국어] 이름이 힙에 있는가 */
		kfree(domain->name);	/* [한국어] 플래그가 없으면 fwnode 안의 문자열을 가리키고 있어 해제하면 남의 메모리를 푼다 */
	kfree(domain);	/* [한국어] 구조체 본체. revmap 배열이 뒤에 붙어 있어 함께 사라진다 */
}

/*
 * [한국어]
 * irq_domain_instantiate_descs - 도메인 범위의 서술자를 미리 만들어 둔다
 *
 * @info: 생성 정보 (virq_base, size)
 * @return: 없음
 *
 * 리눅스 인터럽트 번호를 고정으로 쓰는 도메인이 그 번호의 서술자를
 * 미리 확보한다. 디바이스 트리를 쓰지 않는 플랫폼에서 번호가 코드에
 * 박혀 있는 경우다.
 *
 * 비희소 빌드에서 아무것도 하지 않는 이유: 그쪽은 서술자가 정적
 * 배열이라 이미 전부 존재한다. 확보할 것이 없다.
 *
 * 실패를 정보 수준 로그로만 남기는 것에 주목: 이미 다른 경로로
 * 그 번호의 서술자가 만들어져 있을 수 있다. 그것은 오류가 아니라
 * 흔한 상황이라, 메시지도 "미리 할당된 것으로 가정한다" 고 말한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 도메인 생성 경로.
 *
 * 호출 체인:
 *   __irq_domain_instantiate() → [이 함수] → irq_alloc_descs()
 */
static void irq_domain_instantiate_descs(const struct irq_domain_info *info)
{
	if (!IS_ENABLED(CONFIG_SPARSE_IRQ))	/* [한국어] 희소 서술자를 쓰는 빌드인가 */
		return;	/* [한국어] 정적 배열 빌드에서는 서술자가 이미 전부 있어 확보할 것이 없다 */

	if (irq_alloc_descs(info->virq_base, info->virq_base, info->size,	/* [한국어] 지정된 번호부터 size 개를 확보한다. 첫 두 인자가 같은 것은 "정확히 이 번호" 라는 뜻 */
			    of_node_to_nid(to_of_node(info->fwnode))) < 0) {	/* [한국어] 컨트롤러가 붙은 노드에 둔다 */
		pr_info("Cannot allocate irq_descs @ IRQ%d, assuming pre-allocated\n",	/* [한국어] 오류가 아니다. 다른 경로로 이미 만들어져 있는 흔한 상황이다 */
			info->virq_base);
	}
}

/*
 * [한국어]
 * __irq_domain_instantiate - 도메인 생성의 전체 절차
 *
 * @domain 반환값과 인자:
 * @info:             생성 정보
 * @cond_alloc_descs: virq_base 가 있을 때 서술자를 미리 확보할지
 * @force_associate:  virq_base 와 무관하게 사상을 미리 만들지
 * @return:           만든 도메인, 실패 시 ERR_PTR
 *
 * 이 파일의 모든 도메인 생성 진입점이 여기로 모인다. 순서가 정해진
 * 여섯 단계다.
 *
 *   1. 구조체 생성 (__irq_domain_create)
 *   2. 계층 연결 — 부모가 있으면 root 를 부모의 것으로 바꾼다
 *   3. 범용 칩 할당 (요청한 경우)
 *   4. 드라이버의 init 콜백
 *   5. 전역 목록 등록
 *   6. 서술자 확보와 사상 미리 만들기 (요청한 경우)
 *
 * 2 단계가 계층형 락 설계의 완성이다. 부모가 있으면 그 root 를
 * 물려받아, 계층 전체가 가장 안쪽 도메인의 뮤텍스 하나를 공유하게
 * 된다.
 *
 * 4 와 5 의 순서에 주목: 드라이버 초기화를 먼저 하고 목록에 넣는다.
 * 그래야 다른 CPU 가 이 도메인을 찾았을 때 이미 쓸 수 있는 상태다.
 *
 * 6 단계의 조건이 두 갈래인 이유는 원본 주석에 있다. 레거시 도메인은
 * 리눅스 번호가 고정이라 무조건 사상을 만들어야 하고(force_associate),
 * 다른 도메인은 virq_base 를 준 경우에만 그렇게 한다.
 *
 * 에러 경로가 역순으로 되돌리는 것에 주목: init 이 실패하면 범용 칩을
 * 먼저 없애고 도메인을 해제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_domain_instantiate() / irq_domain_create_simple() /
 *   irq_domain_create_legacy() → [이 함수]
 */
static struct irq_domain *__irq_domain_instantiate(const struct irq_domain_info *info,
						   bool cond_alloc_descs, bool force_associate)
{
	struct irq_domain *domain;	/* [한국어] 만든 도메인 */
	int err;	/* [한국어] 각 단계의 결과 */

	domain = __irq_domain_create(info);	/* [한국어] 구조체 할당과 기본 필드 채우기 */
	if (IS_ERR(domain))	/* [한국어] 실패 */
		return domain;	/* [한국어] 오류 포인터를 그대로 올린다 */

	domain->flags |= info->domain_flags;	/* [한국어] 호출자가 지정한 추가 플래그 — MSI, MSI_PARENT 등 */
	domain->exit = info->exit;	/* [한국어] 도메인 제거 시 불릴 드라이버 콜백 */
	domain->dev = info->dev;	/* [한국어] 이 도메인이 속한 장치. 전원 관리와 MSI 경로가 쓴다 */

#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY	/* [한국어] 계층형을 쓰는 빌드에만 부모 연결이 있다 */
	if (info->parent) {	/* [한국어] 계층에 붙는 도메인인가 */
		domain->root = info->parent->root;	/* [한국어] 계층 전체가 가장 안쪽 도메인의 뮤텍스 하나를 공유하게 된다. 이것이 계층형 락 설계의 완성이다 */
		domain->parent = info->parent;	/* [한국어] 부모 포인터. 할당과 활성화가 이 사슬을 따라 내려간다 */
	}
#endif

	if (info->dgc_info) {	/* [한국어] 범용 칩을 함께 만들어 달라는 요청인가 */
		err = irq_domain_alloc_generic_chips(domain, info->dgc_info);	/* [한국어] kernel/irq/generic-chip.c 에 위임한다 */
		if (err)	/* [한국어] 실패 */
			goto err_domain_free;	/* [한국어] 도메인만 되돌리면 된다 */
	}

	if (info->init) {	/* [한국어] 드라이버가 추가 초기화 콜백을 주었는가 */
		err = info->init(domain);	/* [한국어] 목록 등록 전에 부른다. 그래야 다른 CPU 가 찾았을 때 이미 쓸 수 있는 상태다 */
		if (err)	/* [한국어] 실패 */
			goto err_domain_gc_remove;	/* [한국어] 범용 칩부터 역순으로 되돌린다 */
	}

	__irq_domain_publish(domain);	/* [한국어] 전역 목록에 등록. 이 뒤로 다른 CPU 가 이 도메인을 찾을 수 있다 */

	if (cond_alloc_descs && info->virq_base > 0)	/* [한국어] 서술자를 미리 확보해야 하는가 */
		irq_domain_instantiate_descs(info);	/* [한국어] 리눅스 번호가 고정인 도메인용 */

	/*
	 * Legacy interrupt domains have a fixed Linux interrupt number
	 * associated. Other interrupt domains can request association by
	 * providing a Linux interrupt number > 0.
	 */
	if (force_associate || info->virq_base > 0) {	/* [한국어] (위 영어 주석) 레거시는 무조건, 그 밖에는 번호를 지정한 경우에만 */
		irq_domain_associate_many(domain, info->virq_base, info->hwirq_base,	/* [한국어] 구간 전체의 사상을 미리 만든다. 요청 시점에 만드는 보통 방식과 다르다 */
					  info->size - info->hwirq_base);	/* [한국어] 하드웨어 번호가 0 이 아닌 곳에서 시작할 수 있어 빼 준다 */
	}

	return domain;	/* [한국어] 완성된 도메인 */

err_domain_gc_remove:	/* [한국어] init 콜백이 실패했을 때 */
	if (info->dgc_info)	/* [한국어] 범용 칩을 만들었는가 */
		irq_domain_remove_generic_chips(domain);	/* [한국어] 역순으로 되돌린다 */
err_domain_free:	/* [한국어] 범용 칩 할당이 실패했거나 위에서 흘러온 경우 */
	irq_domain_free(domain);	/* [한국어] 도메인 구조체 해제. 아직 목록에 넣지 않아 목록에서 뺄 필요가 없다 */
	return ERR_PTR(err);	/* [한국어] 실패 원인을 오류 포인터로 */
}

/**
 * irq_domain_instantiate() - Instantiate a new irq domain data structure
 * @info: Domain information pointer pointing to the information for this domain
 *
 * Return: A pointer to the instantiated irq domain or an ERR_PTR value.
 */
/*
 * [한국어]
 * irq_domain_instantiate - 도메인을 만드는 현대적 진입점
 *
 * @info: 모든 설정을 담은 정보 구조체
 * @return: 만든 도메인, 실패 시 ERR_PTR
 *
 * 인자를 늘어놓는 대신 구조체 하나로 받는 방식이다. 도메인 설정
 * 항목이 계속 늘어나면서, 인자를 추가할 때마다 모든 호출자를 고쳐야
 * 하는 문제를 푼 결과다.
 *
 * 두 bool 인자를 모두 false 로 넘기는 것에 주목: 서술자를 미리
 * 확보하지도, 사상을 미리 만들지도 않는다. 요즘 도메인은 인터럽트가
 * 실제로 요청될 때 사상을 만들기 때문이다. 그 두 동작은 옛 방식의
 * 진입점에만 필요하다.
 *
 * ERR_PTR 를 그대로 돌려주는 것도 아래 create_simple/legacy 와 다르다.
 * 그쪽은 NULL 로 눌러 주지만 여기서는 오류 종류를 보존한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irqchip 드라이버 / msi_create_parent_irq_domain() → [이 함수] →
 *   __irq_domain_instantiate()
 */
struct irq_domain *irq_domain_instantiate(const struct irq_domain_info *info)
{
	return __irq_domain_instantiate(info, false, false);	/* [한국어] 서술자 미리 확보도 사상 미리 만들기도 하지 않는다. 요즘 도메인은 요청 시점에 사상을 만든다 */
}
EXPORT_SYMBOL_GPL(irq_domain_instantiate);	/* [한국어] irqchip 드라이버가 모듈일 수 있다 */

/**
 * irq_domain_remove() - Remove an irq domain.
 * @domain: domain to remove
 *
 * This routine is used to remove an irq domain. The caller must ensure
 * that all mappings within the domain have been disposed of prior to
 * use, depending on the revmap type.
 */
/*
 * [한국어]
 * irq_domain_remove - 도메인을 없앤다
 *
 * @domain: 없앨 도메인
 * @return: 없음
 *
 * 도메인 생성의 반대다. 원본 주석이 조건을 명확히 한다 — 호출자가
 * 모든 사상을 미리 없애 두어야 한다. 그러지 않으면 살아 있는
 * 인터럽트가 해제된 도메인을 가리키게 된다.
 *
 * 그 조건을 래딕스 트리가 비었는지로 확인한다. 다만 선형 배열
 * (revmap)에 남은 사상은 이 검사에 걸리지 않는다 — 그래서 원본
 * 주석이 "revmap 종류에 따라" 라고 단서를 단다.
 *
 * 순서가 중요하다. exit 콜백을 목록에서 빼기 전에 부르는 이유:
 * 드라이버가 그 안에서 도메인을 정상적으로 다룰 수 있어야 한다.
 * 반대로 범용 칩 제거는 락 밖에서 하는데, 그 함수가 서술자 락을
 * 잡으므로 도메인 뮤텍스와 겹치면 순서 문제가 생길 수 있어서다.
 *
 * 기본 도메인이 사라지는 경우를 처리하는 것에 주목: 그것을 남겨 두면
 * 이후 NULL 도메인 요청이 해제된 메모리를 가리킨다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irqchip 드라이버 remove / msi_remove_device_irq_domain() → [이 함수]
 */
void irq_domain_remove(struct irq_domain *domain)
{
	if (domain->exit)	/* [한국어] 드라이버가 정리 콜백을 주었는가 */
		domain->exit(domain);	/* [한국어] 목록에서 빼기 전에 부른다. 드라이버가 그 안에서 도메인을 정상적으로 다룰 수 있어야 한다 */

	mutex_lock(&irq_domain_mutex);	/* [한국어] 전역 목록과 debugfs 를 지킨다 */
	debugfs_remove_domain_dir(domain);	/* [한국어] 진단 항목을 먼저 없앤다 */

	WARN_ON(!radix_tree_empty(&domain->revmap_tree));	/* [한국어] 살아 있는 사상이 있는가 — 호출자가 미리 없앴어야 한다. 선형 배열에 남은 것은 이 검사에 걸리지 않아 원본 주석이 단서를 단다 */

	list_del(&domain->link);	/* [한국어] 전역 목록에서 뺀다. 이 뒤로 검색에 걸리지 않는다 */

	/*
	 * If the going away domain is the default one, reset it.
	 */
	if (unlikely(irq_default_domain == domain))	/* [한국어] (위 영어 주석) 사라지는 것이 기본 도메인인가 */
		irq_set_default_domain(NULL);	/* [한국어] 남겨 두면 이후 NULL 도메인 요청이 해제된 메모리를 가리킨다 */

	mutex_unlock(&irq_domain_mutex);	/* [한국어] 아래 범용 칩 제거는 락 밖에서 — 그 함수가 서술자 락을 잡아 순서 문제가 생길 수 있다 */

	if (domain->flags & IRQ_DOMAIN_FLAG_DESTROY_GC)	/* [한국어] 생성 때 범용 칩을 함께 만든 도메인인가 */
		irq_domain_remove_generic_chips(domain);	/* [한국어] 칩들을 없앤다 */

	pr_debug("Removed domain %s\n", domain->name);	/* [한국어] 이름이 아직 유효할 때 찍는다. 아래 free 가 그것을 해제한다 */
	irq_domain_free(domain);	/* [한국어] 구조체와 이름 해제 */
}
EXPORT_SYMBOL_GPL(irq_domain_remove);	/* [한국어] irqchip 드라이버와 MSI 코어가 부른다 */

/*
 * [한국어]
 * irq_domain_update_bus_token - 도메인의 종류 표식을 바꾸고 이름을 다시 짓는다
 *
 * @domain:    대상 도메인
 * @bus_token: 새 종류 표식
 * @return:    없음
 *
 * 도메인을 만든 뒤에 종류를 정하는 경우가 있다. MSI 도메인이
 * 대표적이다 — __msi_create_irq_domain() 이 도메인을 만든 다음
 * 이 함수로 종류를 붙인다.
 *
 * 종류가 바뀌면 이름도 바뀌어야 한다. 그 이름이 debugfs 디렉터리
 * 이름이라, 같은 컨트롤러 위의 여러 도메인이 종류로 구분되기
 * 때문이다. 그래서 이름을 다시 만들고 debugfs 항목도 새로 만든다.
 *
 * 새 이름을 먼저 만들고 나서 옛 것을 지우는 순서에 주목: 할당이
 * 실패해도 도메인이 이름 없는 상태로 남지 않는다.
 *
 * NAME_ALLOCATED 플래그 처리가 두 갈래인 이유: 이미 힙에 있던
 * 이름이면 해제하고, fwnode 의 것을 가리키고 있었으면 해제하지 않고
 * 플래그만 세운다. 이제 새 이름은 힙에 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   __msi_create_irq_domain() / irqchip 드라이버 → [이 함수]
 */
void irq_domain_update_bus_token(struct irq_domain *domain,
				 enum irq_domain_bus_token bus_token)
{
	char *name;	/* [한국어] 새 이름 */

	if (domain->bus_token == bus_token)	/* [한국어] 이미 그 종류인가 */
		return;	/* [한국어] 할 일이 없다. 이름을 다시 만들 이유도 없다 */

	mutex_lock(&irq_domain_mutex);	/* [한국어] debugfs 항목 교체를 다른 목록 조작과 직렬화한다 */

	domain->bus_token = bus_token;	/* [한국어] 종류를 먼저 바꾼다. 아래에서 이름에 반영된다 */

	name = kasprintf(GFP_KERNEL, "%s-%d", domain->name, bus_token);	/* [한국어] 현재 이름에 종류를 덧붙인다. 새 것을 먼저 만들어야 실패해도 도메인이 이름 없는 상태로 남지 않는다 */
	if (!name) {	/* [한국어] 할당 실패 */
		mutex_unlock(&irq_domain_mutex);	/* [한국어] 종류는 이미 바뀌었지만 이름은 옛 것 그대로다. 진단이 조금 헷갈릴 뿐 동작에는 문제가 없다 */
		return;
	}

	debugfs_remove_domain_dir(domain);	/* [한국어] 옛 이름의 항목을 없앤다. 이름을 바꾸기 전에 해야 찾을 수 있다 */

	if (domain->flags & IRQ_DOMAIN_NAME_ALLOCATED)	/* [한국어] 옛 이름이 힙에 있었는가 */
		kfree(domain->name);	/* [한국어] 해제한다 */
	else	/* [한국어] fwnode 의 것을 가리키고 있었는가 */
		domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;	/* [한국어] 해제하지 않고 플래그만 세운다. 새 이름은 힙에 있기 때문이다 */

	domain->name = name;	/* [한국어] 새 이름으로 교체 */
	debugfs_add_domain_dir(domain);	/* [한국어] 새 이름의 항목을 만든다 */

	mutex_unlock(&irq_domain_mutex);	/* [한국어] 여기서 락을 놓는다. 위 pr_debug 는 락 밖에서 찍어도 되지만 이름이 아직 유효해야 한다 */
}
EXPORT_SYMBOL_GPL(irq_domain_update_bus_token);	/* [한국어] MSI 코어와 irqchip 드라이버가 부른다 */

/**
 * irq_domain_create_simple() - Register an irq_domain and optionally map a range of irqs
 * @fwnode: firmware node for the interrupt controller
 * @size: total number of irqs in mapping
 * @first_irq: first number of irq block assigned to the domain,
 *	pass zero to assign irqs on-the-fly. If first_irq is non-zero, then
 *	pre-map all of the irqs in the domain to virqs starting at first_irq.
 * @ops: domain callbacks
 * @host_data: Controller private data pointer
 *
 * Allocates an irq_domain, and optionally if first_irq is positive then also
 * allocate irq_descs and map all of the hwirqs to virqs starting at first_irq.
 *
 * This is intended to implement the expected behaviour for most
 * interrupt controllers. If device tree is used, then first_irq will be 0 and
 * irqs get mapped dynamically on the fly. However, if the controller requires
 * static virq assignments (non-DT boot) then it will set that up correctly.
 */
/*
 * [한국어]
 * irq_domain_create_simple - 가장 흔한 형태의 도메인을 만든다
 *
 * @fwnode:    컨트롤러의 펌웨어 노드
 * @size:      담당 인터럽트 수
 * @first_irq: 고정 리눅스 번호의 시작, 또는 0 (동적 배정)
 * @ops:       도메인 콜백
 * @host_data: 드라이버 사설 데이터
 * @return:    만든 도메인, 실패 시 NULL
 *
 * 대부분의 컨트롤러 드라이버가 쓰는 진입점이다. 이름의 simple 은
 * 계층이 없는 평평한 도메인이라는 뜻이다.
 *
 * first_irq 가 두 방식을 가른다. 원본 주석이 설명하듯, 디바이스
 * 트리를 쓰면 0 을 넘겨 번호를 동적으로 받고, 그러지 않는 플랫폼은
 * 고정 번호를 지정해 부팅 때 미리 사상을 만든다.
 *
 * cond_alloc_descs 를 true 로 넘기는 것에 주목: 고정 번호를 쓸 때
 * 그 번호의 서술자를 미리 확보해야 한다. 반면 force_associate 는
 * false 라, first_irq 가 0 이면 사상을 만들지 않는다.
 *
 * ERR_PTR 를 NULL 로 눌러 주는 것은 옛 API 의 규약이다. 호출자들이
 * NULL 검사만 하도록 되어 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 대개 부팅 중 컨트롤러 초기화.
 *
 * 호출 체인:
 *   drivers/irqchip 의 많은 드라이버 → [이 함수] →
 *   __irq_domain_instantiate()
 */
struct irq_domain *irq_domain_create_simple(struct fwnode_handle *fwnode,
					    unsigned int size,
					    unsigned int first_irq,
					    const struct irq_domain_ops *ops,
					    void *host_data)
{
	struct irq_domain_info info = {	/* [한국어] 인자를 정보 구조체로 모은다. 지정하지 않은 필드는 0 이라 계층 없음, 종류 없음이 된다 */
		.fwnode		= fwnode,	/* [한국어] 컨트롤러 노드 */
		.size		= size,	/* [한국어] 선형 역방향 배열의 크기 */
		.hwirq_max	= size,	/* [한국어] 하드웨어 번호 상한. simple 도메인은 0 부터 연속이라 크기와 같다 */
		.virq_base	= first_irq,	/* [한국어] 0 이면 동적 배정 */
		.ops		= ops,	/* [한국어] 드라이버 콜백 */
		.host_data	= host_data,	/* [한국어] 드라이버 사설 데이터 */
	};
	struct irq_domain *domain = __irq_domain_instantiate(&info, true, false);	/* [한국어] 첫 true 는 고정 번호일 때 서술자를 미리 확보하라는 뜻. 두 번째 false 라 first_irq 가 0 이면 사상을 만들지 않는다 */

	return IS_ERR(domain) ? NULL : domain;	/* [한국어] 옛 API 규약대로 오류를 NULL 로 눌러 준다 */
}
EXPORT_SYMBOL_GPL(irq_domain_create_simple);	/* [한국어] 가장 많이 쓰이는 도메인 생성 API */

/*
 * [한국어]
 * irq_domain_create_legacy - 리눅스 번호가 고정된 옛 방식 도메인을 만든다
 *
 * @fwnode:      컨트롤러의 펌웨어 노드
 * @size:        담당 인터럽트 수
 * @first_irq:   고정 리눅스 번호의 시작
 * @first_hwirq: 대응하는 하드웨어 번호의 시작
 * @ops:         도메인 콜백
 * @host_data:   드라이버 사설 데이터
 * @return:      만든 도메인, 실패 시 NULL
 *
 * 위 simple 과 두 가지가 다르다.
 *
 * 첫째, 하드웨어 번호가 0 이 아닌 곳에서 시작할 수 있다. ISA
 * 인터럽트 0~15 를 리눅스 번호 0~15 에 대응시키되 컨트롤러 입장에서는
 * 다른 번호인 경우가 그렇다. 그래서 size 와 hwirq_max 를 first_hwirq
 * 만큼 밀어 잡는다 — 역방향 배열이 그 번호까지 덮어야 하기 때문이다.
 *
 * 둘째, force_associate 가 true 다. 레거시 도메인은 리눅스 번호가
 * 고정이므로 사상을 무조건 미리 만든다. 반대로 cond_alloc_descs 는
 * false 인데, 이 방식을 쓰는 플랫폼은 서술자가 이미 존재하는 것을
 * 전제하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 부팅 중.
 *
 * 호출 체인:
 *   ISA/PC 계열 컨트롤러 드라이버 → [이 함수] →
 *   __irq_domain_instantiate()
 */
struct irq_domain *irq_domain_create_legacy(struct fwnode_handle *fwnode,
					 unsigned int size,
					 unsigned int first_irq,
					 irq_hw_number_t first_hwirq,
					 const struct irq_domain_ops *ops,
					 void *host_data)
{
	struct irq_domain_info info = {	/* [한국어] 인자를 정보 구조체로 */
		.fwnode		= fwnode,	/* [한국어] 컨트롤러 노드 */
		.size		= first_hwirq + size,	/* [한국어] 하드웨어 번호가 0 이 아닌 곳에서 시작해도 역방향 배열이 그 번호까지 덮어야 한다 */
		.hwirq_max	= first_hwirq + size,	/* [한국어] 같은 이유로 상한도 밀어 잡는다 */
		.hwirq_base	= first_hwirq,	/* [한국어] 사상을 만들 시작 하드웨어 번호 */
		.virq_base	= first_irq,	/* [한국어] 대응하는 리눅스 번호의 시작 */
		.ops		= ops,	/* [한국어] 드라이버 콜백 */
		.host_data	= host_data,	/* [한국어] 드라이버 사설 데이터 */
	};
	struct irq_domain *domain = __irq_domain_instantiate(&info, false, true);	/* [한국어] 서술자는 이미 있다고 보고(false), 사상은 무조건 만든다(true). 레거시 도메인은 번호가 고정이라 미리 이어 두어야 한다 */

	return IS_ERR(domain) ? NULL : domain;	/* [한국어] 옛 API 규약대로 NULL 로 눌러 준다 */
}
EXPORT_SYMBOL_GPL(irq_domain_create_legacy);	/* [한국어] ISA/PC 계열 드라이버가 부른다 */

/**
 * irq_find_matching_fwspec() - Locates a domain for a given fwspec
 * @fwspec: FW specifier for an interrupt
 * @bus_token: domain-specific data
 */
/*
 * [한국어]
 * irq_find_matching_fwspec - 펌웨어 명세에 맞는 도메인을 찾는다
 *
 * @fwspec:    인터럽트의 펌웨어 명세 (노드 + 매개변수)
 * @bus_token: 찾을 도메인의 종류. DOMAIN_BUS_ANY 면 종류를 안 따진다.
 * @return:    찾은 도메인, 없으면 NULL
 *
 * 디바이스 트리의 "interrupts-parent" 가 가리키는 노드로부터 실제
 * 도메인을 찾는 함수다. 전역 목록을 선형으로 훑는다.
 *
 * 판별 방식이 세 갈래인 것이 이 함수의 핵심이다.
 *
 * (1) select 콜백: 도메인이 명세 전체를 보고 판단한다. 한 노드 아래
 *     여러 도메인이 있고 매개변수로 구분해야 하는 경우다. 종류가
 *     지정됐을 때만 쓴다.
 * (2) match 콜백: 노드와 종류만 보고 판단한다. 더 단순한 경우.
 * (3) 기본: 노드 포인터가 같고 종류가 맞으면 된다. 대부분이 이쪽이다.
 *
 * 원본 주석이 짚는 두 가지가 있다. 하나는 레거시 컨트롤러를 마지막에
 * 봐야 한다는 것 — 그것이 노드 없이 모든 인터럽트에 맞을 수 있기
 * 때문이다. 아직 문제가 되지 않아 그냥 두었다고 한다. 다른 하나는
 * DOMAIN_BUS_ANY 가 어떤 도메인에도 맞고, 다른 값은 정확히 일치해야
 * 한다는 것이다.
 *
 * 락을 잡고 훑는 것에 주목: 도메인이 목록에서 빠지는 중이면 안 된다.
 * 다만 찾은 도메인을 반환한 뒤에는 락이 풀리므로, 호출자가 쓰기 전에
 * 사라질 수 있다. 실제로는 이 검색이 부팅 중 설정 경로에서만
 * 일어나 문제가 되지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   fwspec_to_domain() / irqchip 드라이버 → [이 함수]
 */
struct irq_domain *irq_find_matching_fwspec(struct irq_fwspec *fwspec,
					    enum irq_domain_bus_token bus_token)
{
	struct irq_domain *h, *found = NULL;	/* [한국어] 순회 중인 도메인과 찾은 결과 */
	struct fwnode_handle *fwnode = fwspec->fwnode;	/* [한국어] 명세가 가리키는 컨트롤러 노드 */
	int rc;	/* [한국어] 판별 결과 */

	/*
	 * We might want to match the legacy controller last since
	 * it might potentially be set to match all interrupts in
	 * the absence of a device node. This isn't a problem so far
	 * yet though...
	 *
	 * bus_token == DOMAIN_BUS_ANY matches any domain, any other
	 * values must generate an exact match for the domain to be
	 * selected.
	 */
	mutex_lock(&irq_domain_mutex);	/* [한국어] (위 영어 주석) 순회 중에 도메인이 빠지지 않게 한다 */
	list_for_each_entry(h, &irq_domain_list, link) {	/* [한국어] 전역 목록을 선형으로 훑는다. 설정 시점에만 일어나 성능이 문제되지 않는다 */
		if (h->ops->select && bus_token != DOMAIN_BUS_ANY)	/* [한국어] 명세 전체를 보고 판단하는 도메인인가. 종류가 지정됐을 때만 쓴다 */
			rc = h->ops->select(h, fwspec, bus_token);	/* [한국어] 한 노드 아래 여러 도메인이 있고 매개변수로 구분해야 하는 경우다 */
		else if (h->ops->match)	/* [한국어] 노드와 종류만 보는 더 단순한 콜백 */
			rc = h->ops->match(h, to_of_node(fwnode), bus_token);	/* [한국어] 디바이스 트리 노드로 변환해 넘긴다 */
		else	/* [한국어] 콜백이 없는 대부분의 도메인 */
			rc = ((fwnode != NULL) && (h->fwnode == fwnode) &&	/* [한국어] 노드 포인터가 같고 */
			      ((bus_token == DOMAIN_BUS_ANY) ||	/* [한국어] 종류를 안 따지거나 */
			       (h->bus_token == bus_token)));	/* [한국어] 종류가 정확히 일치하는가 */

		if (rc) {	/* [한국어] 맞는 도메인을 찾았는가 */
			found = h;	/* [한국어] 기록하고 */
			break;	/* [한국어] 첫 번째로 맞는 것을 쓴다. 목록이 머리 삽입이라 나중에 만든 도메인이 우선한다 */
		}
	}
	mutex_unlock(&irq_domain_mutex);	/* [한국어] 반환 뒤에는 락이 없다. 이 검색이 부팅 중 설정 경로에서만 일어나 문제가 되지 않는다 */
	return found;	/* [한국어] 못 찾았으면 NULL */
}
EXPORT_SYMBOL_GPL(irq_find_matching_fwspec);	/* [한국어] irqchip 드라이버가 부모 도메인을 찾을 때 부른다 */

/**
 * irq_set_default_domain() - Set a "default" irq domain
 * @domain: default domain pointer
 *
 * For convenience, it's possible to set a "default" domain that will be used
 * whenever NULL is passed to irq_create_mapping(). It makes life easier for
 * platforms that want to manipulate a few hard coded interrupt numbers that
 * aren't properly represented in the device-tree.
 */
/*
 * [한국어]
 * irq_set_default_domain - 기본 도메인을 지정한다
 *
 * @domain: 기본으로 쓸 도메인. NULL 이면 해제.
 * @return: 없음
 *
 * 도메인을 지정하지 않은 요청이 어디로 갈지를 정한다. 원본 주석이
 * 용도를 밝힌다 — 디바이스 트리에 제대로 기술되지 않은 인터럽트 번호
 * 몇 개를 코드에 박아 쓰는 플랫폼을 위한 편의다.
 *
 * 락이 없는 것에 주목: 부팅 초기의 단일 스레드 작업이라는 전제다.
 * 예외는 irq_domain_remove() 안에서 불리는 경우인데, 그쪽은
 * irq_domain_mutex 를 쥔 채로 부른다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 대개 부팅 초기.
 *
 * 호출 체인:
 *   아키텍처 인터럽트 초기화 / irq_domain_remove() → [이 함수]
 */
void irq_set_default_domain(struct irq_domain *domain)
{
	pr_debug("Default domain set to @0x%p\n", domain);	/* [한국어] 기본 도메인이 언제 바뀌는지 추적할 때 유용하다 */

	irq_default_domain = domain;	/* [한국어] 락이 없다. 부팅 초기의 단일 스레드 작업이라는 전제이고, remove 경로에서는 호출자가 락을 쥐고 있다 */
}
EXPORT_SYMBOL_GPL(irq_set_default_domain);	/* [한국어] 아키텍처 코드가 모듈일 수 있다 */

/**
 * irq_get_default_domain() - Retrieve the "default" irq domain
 *
 * Returns: the default domain, if any.
 *
 * Modern code should never use this. This should only be used on
 * systems that cannot implement a firmware->fwnode mapping (which
 * both DT and ACPI provide).
 */
/*
 * [한국어]
 * irq_get_default_domain - 기본 도메인을 돌려준다
 *
 * @return: 기본 도메인, 없으면 NULL
 *
 * 원본 주석이 이례적으로 강하게 말한다 — "현대적인 코드는 절대 이것을
 * 쓰면 안 된다".
 *
 * 왜 그런가: 기본 도메인이라는 개념 자체가 "어느 컨트롤러인지 알 수
 * 없다" 는 뜻이다. 디바이스 트리와 ACPI 는 모두 그 정보를 제공하므로,
 * 그 둘 중 하나를 쓰는 시스템에서는 도메인을 명시적으로 찾을 수 있다.
 * 기본 도메인에 의존하면 컨트롤러가 여럿인 시스템에서 조용히 잘못된
 * 도메인을 쓰게 된다.
 *
 * 그래도 남아 있는 이유: 펌웨어가 인터럽트 정보를 전혀 제공하지 않는
 * 오래된 임베디드 플랫폼이 아직 있다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   옛 플랫폼 코드 → [이 함수]
 */
struct irq_domain *irq_get_default_domain(void)
{
	return irq_default_domain;	/* [한국어] 락 없는 읽기. 부팅 후에는 바뀌지 않는다는 전제다 */
}
EXPORT_SYMBOL_GPL(irq_get_default_domain);	/* [한국어] 옛 플랫폼 코드가 모듈일 수 있다 */

/*
 * [한국어]
 * irq_domain_is_nomap - 역방향 맵을 쓰지 않는 도메인인지 판별한다
 *
 * @domain: 대상 도메인
 * @return: true 직접 사상 방식, false 역방향 맵을 쓰는 보통 도메인
 *
 * NOMAP 방식이란: 리눅스 인터럽트 번호를 그대로 하드웨어 번호로 쓰는
 * 것이다. 컨트롤러가 자기 인터럽트 번호를 마음대로 정할 수 있을 때
 * (예: 소프트웨어로 벡터를 배정하는 하이퍼바이저 인터페이스) 가능하다.
 *
 * 그러면 역방향 조회가 항등 함수가 되어 배열도 트리도 필요 없다.
 * 인터럽트 경로에서 조회 비용이 0 이 된다.
 *
 * IS_ENABLED 를 함께 검사하는 이유: 그 기능을 뺀 빌드에서는 컴파일러가
 * 이 함수를 상수 false 로 접고, 호출부의 조건 분기까지 통째로 지운다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   irq_domain_set_mapping() / clear_mapping() / __irq_resolve_mapping() /
 *   irq_domain_fix_revmap() → [이 함수]
 */
static bool irq_domain_is_nomap(struct irq_domain *domain)
{
	return IS_ENABLED(CONFIG_IRQ_DOMAIN_NOMAP) &&	/* [한국어] 그 기능을 뺀 빌드에서는 컴파일러가 이 함수를 상수 false 로 접어 호출부의 분기까지 지운다 */
	       (domain->flags & IRQ_DOMAIN_FLAG_NO_MAP);	/* [한국어] 리눅스 번호를 그대로 하드웨어 번호로 쓰는 도메인인가. 그러면 조회가 항등 함수라 맵이 필요 없다 */
}

/*
 * [한국어]
 * irq_domain_clear_mapping - 역방향 맵에서 항목 하나를 지운다
 *
 * @domain: 대상 도메인
 * @hwirq:  지울 하드웨어 번호
 * @return: 없음
 *
 * 역방향 맵이 두 가지라는 것이 이 함수와 아래 짝 함수의 요점이다.
 * 작은 하드웨어 번호는 선형 배열(revmap)에, 큰 번호는 래딕스 트리에
 * 들어간다.
 *
 * 왜 둘로 나누는가: 인터럽트 경로에서 매번 조회하므로 배열 인덱싱이
 * 가장 빠르다. 그런데 하드웨어 번호가 듬성듬성하고 클 수 있어
 * (MSI 인덱스나 GIC ITS 의 이벤트 ID) 전부 배열로 잡으면 낭비가 크다.
 * 그래서 흔한 작은 번호만 배열로 덮고 나머지는 트리로 보낸다.
 *
 * rcu_assign_pointer 를 쓰는 이유: 조회 쪽(__irq_resolve_mapping)이
 * RCU 로 락 없이 읽는다. 그냥 대입하면 컴파일러가 순서를 바꿔
 * 조회하는 쪽이 반쯤 만들어진 상태를 볼 수 있다.
 *
 * lockdep_assert_held(&domain->root->mutex) 에 주목: 자기 뮤텍스가
 * 아니라 root 의 것이다. 계층형에서 모든 층이 하나의 락을 공유하는
 * 설계가 여기서도 드러난다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   irq_domain_disassociate() / irq_domain_remove_irq() /
 *   irq_domain_pop_irq() → [이 함수]
 */
static void irq_domain_clear_mapping(struct irq_domain *domain,
				     irq_hw_number_t hwirq)
{
	lockdep_assert_held(&domain->root->mutex);	/* [한국어] 자기 것이 아니라 root 의 뮤텍스다. 계층 전체가 하나의 락을 공유한다 */

	if (irq_domain_is_nomap(domain))	/* [한국어] 직접 사상 도메인인가 */
		return;	/* [한국어] 맵 자체가 없어 지울 것이 없다 */

	if (hwirq < domain->revmap_size)	/* [한국어] 선형 배열이 덮는 범위인가 */
		rcu_assign_pointer(domain->revmap[hwirq], NULL);	/* [한국어] RCU 로 보호된 대입. 조회 쪽이 락 없이 읽으므로 컴파일러가 순서를 바꾸지 못하게 한다 */
	else	/* [한국어] 배열 범위를 넘는 큰 번호 */
		radix_tree_delete(&domain->revmap_tree, hwirq);	/* [한국어] 듬성듬성한 큰 번호를 위한 트리. 전부 배열로 잡으면 낭비가 크다 */
}

/*
 * [한국어]
 * irq_domain_set_mapping - 역방향 맵에 항목 하나를 넣는다
 *
 * @domain:   대상 도메인
 * @hwirq:    하드웨어 번호
 * @irq_data: 그 번호에 대응하는 irq_data
 * @return:   없음
 *
 * 위 clear 의 정확한 반대다. 이 항목이 들어가는 순간부터
 * irq_resolve_mapping(domain, hwirq) 이 이 irq_data 를 돌려준다 —
 * 즉 그 하드웨어 인터럽트가 처리될 수 있게 된다.
 *
 * 원본 주석이 lockdep 검사의 부수 효과를 짚는다. 계층의 각 층에
 * 대해 이 함수가 불릴 때, 모든 층이 같은 root 뮤텍스를 요구하므로
 * 계층 구성이 잘못되어 root 가 어긋나 있으면 lockdep 이 잡아낸다.
 *
 * 래딕스 트리 삽입의 실패를 검사하지 않는 것에 주목: 메모리 부족으로
 * 실패할 수 있는데 반환값이 void 다. 실패하면 조회가 되지 않아
 * 인터럽트가 처리되지 않지만, 그 상황에서 되돌릴 방법도 마땅치 않다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   irq_domain_associate_locked() / irq_domain_insert_irq() /
 *   irq_domain_push_irq() → [이 함수]
 */
static void irq_domain_set_mapping(struct irq_domain *domain,
				   irq_hw_number_t hwirq,
				   struct irq_data *irq_data)
{
	/*
	 * This also makes sure that all domains point to the same root when
	 * called from irq_domain_insert_irq() for each domain in a hierarchy.
	 */
	lockdep_assert_held(&domain->root->mutex);	/* [한국어] (위 영어 주석) 계층의 각 층에 대해 불릴 때 모두 같은 락을 요구하므로, root 가 어긋나 있으면 lockdep 이 잡아낸다 */

	if (irq_domain_is_nomap(domain))	/* [한국어] 직접 사상 도메인인가 */
		return;	/* [한국어] 맵이 필요 없다 */

	if (hwirq < domain->revmap_size)	/* [한국어] 선형 배열 범위 */
		rcu_assign_pointer(domain->revmap[hwirq], irq_data);	/* [한국어] 이 대입 뒤로 조회가 성공한다 — 즉 이 하드웨어 인터럽트가 처리될 수 있게 된다 */
	else	/* [한국어] 큰 번호 */
		radix_tree_insert(&domain->revmap_tree, hwirq, irq_data);	/* [한국어] 실패를 검사하지 않는다. 반환값이 void 이고, 메모리 부족 상황에서 되돌릴 방법도 마땅치 않다 */
}

/*
 * [한국어]
 * irq_domain_disassociate - 사상 하나를 끊는다
 *
 * @domain: 대상 도메인
 * @irq:    끊을 리눅스 인터럽트 번호
 * @return: 없음
 *
 * 계층이 아닌 도메인에서 인터럽트를 떼어 내는 함수다. 순서가
 * 전부라고 해도 될 만큼 각 단계가 앞 단계에 기댄다.
 *
 *   1. NOREQUEST 표시 — 이 번호를 새로 요청하지 못하게 막는다.
 *   2. 칩과 처리기 제거 — 그 안에서 선이 마스크된다.
 *   3. synchronize_irq — 진행 중인 처리가 끝나기를 기다린다.
 *   4. 드라이버의 unmap 콜백
 *   5. 메모리 장벽
 *   6. irq_data 초기화와 역방향 맵 제거
 *
 * 3 이 핵심이다. 다른 CPU 가 이 인터럽트를 처리하는 중일 수 있는데,
 * 그 처리가 끝나기 전에 6 을 하면 처리 중인 코드가 초기화된
 * irq_data 를 보게 된다.
 *
 * smp_mb() 가 필요한 이유: unmap 콜백이 하드웨어를 건드린 것과 아래
 * 자료구조 변경 사이에 순서를 보장한다. 다른 CPU 가 자료구조 변경만
 * 먼저 보고 하드웨어가 아직 살아 있다고 판단하면 안 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. synchronize_irq 가 잠들 수 있다.
 *
 * 호출 체인:
 *   irq_dispose_mapping() → [이 함수]
 */
static void irq_domain_disassociate(struct irq_domain *domain, unsigned int irq)
{
	struct irq_data *irq_data = irq_get_irq_data(irq);	/* [한국어] 끊을 인터럽트의 irq_data */
	irq_hw_number_t hwirq;	/* [한국어] 역방향 맵에서 지울 번호 */

	if (WARN(!irq_data || irq_data->domain != domain,	/* [한국어] 존재하지 않거나 이 도메인의 것이 아닌가 — 호출자의 버그다 */
		 "virq%i doesn't exist; cannot disassociate\n", irq))
		return;

	hwirq = irq_data->hwirq;	/* [한국어] 아래에서 irq_data 를 초기화하기 전에 챙겨 둔다 */

	mutex_lock(&domain->root->mutex);	/* [한국어] 계층 공용 락 */

	irq_set_status_flags(irq, IRQ_NOREQUEST);	/* [한국어] 이 번호를 새로 요청하지 못하게 막는다. 정리 중에 누가 request_irq 하면 안 된다 */

	/* remove chip and handler */
	irq_set_chip_and_handler(irq, NULL, NULL);	/* [한국어] (위 영어 주석) 처리기를 떼면 그 안에서 선이 마스크된다. 이 뒤로 새 인터럽트가 올라오지 않는다 */

	/* Make sure it's completed */
	synchronize_irq(irq);	/* [한국어] (위 영어 주석) 다른 CPU 가 처리 중일 수 있다. 그것이 끝나기 전에 아래 자료구조를 초기화하면 처리 중인 코드가 빈 irq_data 를 본다 */

	/* Tell the PIC about it */
	if (domain->ops->unmap)	/* [한국어] (위 영어 주석) 드라이버가 정리 콜백을 주었는가 */
		domain->ops->unmap(domain, irq);	/* [한국어] 컨트롤러 고유의 정리. generic-chip 은 여기서 점유 비트를 내린다 */
	smp_mb();	/* [한국어] 하드웨어 조작과 아래 자료구조 변경 사이의 순서를 보장한다. 다른 CPU 가 자료구조 변경만 먼저 보고 하드웨어가 살아 있다고 판단하면 안 된다 */

	irq_data->domain = NULL;	/* [한국어] 도메인 연결을 끊는다 */
	irq_data->hwirq = 0;	/* [한국어] 하드웨어 번호도 지운다 */
	domain->mapcount--;	/* [한국어] 사상 수를 줄인다. debugfs 가 이 값을 보여 준다 */

	/* Clear reverse map for this hwirq */
	irq_domain_clear_mapping(domain, hwirq);	/* [한국어] (위 영어 주석) 위에서 챙겨 둔 번호로 지운다. 이 뒤로 조회가 실패한다 */

	mutex_unlock(&domain->root->mutex);	/* [한국어] 사상 조작이 끝났다. 이 뒤로 다른 CPU 가 같은 도메인을 만질 수 있다 */
}

/*
 * [한국어]
 * irq_domain_associate_locked - 사상 하나를 만든다 (락 보유)
 *
 * @domain: 대상 도메인
 * @virq:   리눅스 인터럽트 번호 (이미 할당돼 있어야 한다)
 * @hwirq:  대응시킬 하드웨어 번호
 * @return: 0 성공, -EINVAL 검사 실패, 그 외 드라이버 map 콜백의 오류
 *
 * 위 disassociate 의 반대다. 리눅스 번호와 하드웨어 번호를 이어
 * 인터럽트가 동작할 수 있게 만든다.
 *
 * 세 검사가 각각 다른 잘못을 잡는다. 하드웨어 번호가 도메인 범위를
 * 넘는 경우, 리눅스 번호의 서술자가 없는 경우, 그리고 그 번호가 이미
 * 다른 도메인에 붙어 있는 경우다. 마지막이 특히 중요한데, 덮어쓰면
 * 앞의 사상이 조용히 새면서 그 인터럽트가 고아가 된다.
 *
 * 순서에 주목: irq_data 를 먼저 채우고 map 콜백을 부른다. 콜백이
 * 그 값을 읽어야 하기 때문이다. 실패하면 되돌린다.
 *
 * -EPERM 을 조용히 넘기는 것이 원본 주석의 설명대로다. 펌웨어가
 * 예약한 인터럽트를 매핑하려 할 때 나오는데, 그것은 오류가 아니라
 * 정상적인 거절이라 로그를 어지럽힐 이유가 없다.
 *
 * NOREQUEST 를 지우는 마지막 줄이 "이제 요청해도 된다" 는 신호다.
 * 서술자는 기본적으로 그 플래그가 세워진 채 만들어진다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   irq_domain_associate() / irq_create_mapping_affinity_locked() →
 *   [이 함수] → domain->ops->map()
 */
static int irq_domain_associate_locked(struct irq_domain *domain, unsigned int virq,
				       irq_hw_number_t hwirq)
{
	struct irq_data *irq_data = irq_get_irq_data(virq);	/* [한국어] 이을 대상의 irq_data */
	int ret;	/* [한국어] map 콜백의 결과 */

	if (WARN(hwirq >= domain->hwirq_max,	/* [한국어] 하드웨어 번호가 도메인 범위를 넘는가 */
		 "error: hwirq 0x%x is too large for %s\n", (int)hwirq, domain->name))
		return -EINVAL;
	if (WARN(!irq_data, "error: virq%i is not allocated", virq))	/* [한국어] 리눅스 번호의 서술자가 없는가 — 할당을 먼저 해야 한다 */
		return -EINVAL;
	if (WARN(irq_data->domain, "error: virq%i is already associated", virq))	/* [한국어] 이미 다른 도메인에 붙어 있는가 — 덮어쓰면 앞의 사상이 새면서 그 인터럽트가 고아가 된다 */
		return -EINVAL;

	irq_data->hwirq = hwirq;	/* [한국어] 먼저 채운다. 아래 map 콜백이 이 값을 읽는다 */
	irq_data->domain = domain;	/* [한국어] 도메인 연결 */
	if (domain->ops->map) {	/* [한국어] 드라이버가 매핑 콜백을 주었는가 */
		ret = domain->ops->map(domain, virq, hwirq);	/* [한국어] 컨트롤러 고유의 설정. generic-chip 은 여기서 칩과 처리기를 건다 */
		if (ret != 0) {	/* [한국어] 실패 */
			/*
			 * If map() returns -EPERM, this interrupt is protected
			 * by the firmware or some other service and shall not
			 * be mapped. Don't bother telling the user about it.
			 */
			if (ret != -EPERM) {	/* [한국어] (위 영어 주석) 펌웨어가 예약한 인터럽트를 거절한 경우인가 */
				pr_info("%s didn't like hwirq-0x%lx to VIRQ%i mapping (rc=%d)\n",	/* [한국어] 그것은 정상적인 거절이라 로그를 어지럽히지 않는다 */
				       domain->name, hwirq, virq, ret);
			}
			irq_data->domain = NULL;	/* [한국어] 위에서 채운 것을 되돌린다 */
			irq_data->hwirq = 0;	/* [한국어] 마찬가지 */
			return ret;	/* [한국어] 드라이버의 오류를 그대로 올린다 */
		}
	}

	domain->mapcount++;	/* [한국어] 사상 수. debugfs 가 보여 준다 */
	irq_domain_set_mapping(domain, hwirq, irq_data);	/* [한국어] 역방향 맵에 넣는다. 이 뒤로 하드웨어 번호로 조회가 성공한다 */

	irq_clear_status_flags(virq, IRQ_NOREQUEST);	/* [한국어] "이제 요청해도 된다". 서술자는 기본적으로 이 플래그가 세워진 채 만들어진다 */

	return 0;	/* [한국어] 이 인터럽트가 이제 동작할 준비를 마쳤다 */
}

/*
 * [한국어]
 * irq_domain_associate - 사상 하나를 만든다 (락 획득)
 *
 * @domain: 대상 도메인
 * @virq:   리눅스 인터럽트 번호
 * @hwirq:  하드웨어 번호
 * @return: 0 성공, 음수 오류
 *
 * 위 함수에 락 획득을 씌운 공개 API 다.
 *
 * 락을 계층의 root 에서 잡는 것에 주목: 이 도메인이 계층에 속해
 * 있으면 그 안쪽 도메인의 뮤텍스다. 계층 전체가 하나의 락을 공유하는
 * 설계의 결과다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 뮤텍스라 잠들 수 있다.
 *
 * 호출 체인:
 *   irq_domain_associate_many() / irq_create_direct_mapping() →
 *   [이 함수] → irq_domain_associate_locked()
 */
int irq_domain_associate(struct irq_domain *domain, unsigned int virq,
			 irq_hw_number_t hwirq)
{
	int ret;	/* [한국어] 결과 */

	mutex_lock(&domain->root->mutex);	/* [한국어] 계층에 속해 있으면 가장 안쪽 도메인의 뮤텍스다 */
	ret = irq_domain_associate_locked(domain, virq, hwirq);	/* [한국어] 실제 사상 만들기 */
	mutex_unlock(&domain->root->mutex);	/* [한국어] 사상 생성이 끝났다 */

	return ret;	/* [한국어] 결과를 그대로 전달 */
}
EXPORT_SYMBOL_GPL(irq_domain_associate);	/* [한국어] irqchip 드라이버가 고정 사상을 만들 때 부른다 */

/*
 * [한국어]
 * irq_domain_associate_many - 연속된 사상 여러 개를 만든다
 *
 * @domain:     대상 도메인
 * @irq_base:   리눅스 번호의 시작
 * @hwirq_base: 하드웨어 번호의 시작
 * @count:      개수
 * @return:     없음
 *
 * 레거시 도메인이 부팅 때 구간 전체의 사상을 미리 만드는 경로다.
 * 두 번호가 나란히 증가한다는 전제다 — 리눅스 16 번이 하드웨어
 * 0 번이면 17 번은 1 번이다.
 *
 * 실패를 무시하는 것에 주목: 반환값이 void 이고 중간에 실패해도
 * 계속 진행한다. 이 경로가 부팅 초기의 고정 설정이라, 하나가
 * 실패해도 나머지는 만들어 두는 편이 낫다는 판단이다. 실패는
 * irq_domain_associate_locked 안의 WARN 이 알린다.
 *
 * 인터럽트마다 락을 잡았다 놓는 것도 눈에 띈다. 전체를 한 번에
 * 잡으면 효율적이겠지만, 부팅 경로라 성능이 문제되지 않고 코드가
 * 단순한 편이 낫다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 부팅 중.
 *
 * 호출 체인:
 *   __irq_domain_instantiate() (레거시 경로) → [이 함수] →
 *   irq_domain_associate()
 */
void irq_domain_associate_many(struct irq_domain *domain, unsigned int irq_base,
			       irq_hw_number_t hwirq_base, int count)
{
	struct device_node *of_node;	/* [한국어] 로그에 찍을 노드 */
	int i;	/* [한국어] 순회용 */

	of_node = irq_domain_get_of_node(domain);	/* [한국어] 디바이스 트리 노드. 없으면 NULL 이고 아래 출력이 "<no-node>" 가 된다 */
	pr_debug("%s(%s, irqbase=%i, hwbase=%i, count=%i)\n", __func__,	/* [한국어] 부팅 시 고정 사상이 어떻게 만들어지는지 추적할 때 유용하다 */
		of_node_full_name(of_node), irq_base, (int)hwirq_base, count);

	for (i = 0; i < count; i++)	/* [한국어] 두 번호가 나란히 증가한다는 전제 */
		irq_domain_associate(domain, irq_base + i, hwirq_base + i);	/* [한국어] 실패를 무시한다. 부팅 초기의 고정 설정이라 하나가 실패해도 나머지를 만들어 두는 편이 낫다 */
}
EXPORT_SYMBOL_GPL(irq_domain_associate_many);	/* [한국어] 옛 방식 드라이버가 부른다 */

#ifdef CONFIG_IRQ_DOMAIN_NOMAP	/* [한국어] 리눅스 번호를 그대로 하드웨어 번호로 쓰는 방식을 지원하는 빌드 */
/**
 * irq_create_direct_mapping() - Allocate an irq for direct mapping
 * @domain: domain to allocate the irq for or NULL for default domain
 *
 * This routine is used for irq controllers which can choose the hardware
 * interrupt numbers they generate. In such a case it's simplest to use
 * the linux irq as the hardware interrupt number. It still uses the linear
 * or radix tree to store the mapping, but the irq controller can optimize
 * the revmap path by using the hwirq directly.
 */
/*
 * [한국어]
 * irq_create_direct_mapping - 리눅스 번호를 하드웨어 번호로 쓰는 사상을 만든다
 *
 * @domain: 대상 도메인, NULL 이면 기본 도메인
 * @return: 배정된 리눅스 인터럽트 번호, 실패 시 0
 *
 * 어떤 컨트롤러는 자기가 생성할 인터럽트 번호를 마음대로 정할 수
 * 있다. 소프트웨어로 벡터를 배정하는 하이퍼바이저 인터페이스가
 * 그렇다. 그러면 리눅스 번호를 그대로 하드웨어 번호로 쓰는 것이
 * 가장 단순하고, 역방향 조회가 항등 함수가 되어 인터럽트 경로에서
 * 조회 비용이 사라진다.
 *
 * 1 번부터 찾는 것에 주목: 0 은 예약되어 장치에 배정되지 않는다.
 *
 * 상한 검사가 그 뒤에 오는 이유: 서술자를 먼저 잡아 봐야 어떤 번호를
 * 받을지 알 수 있다. 받은 번호가 도메인 범위를 넘으면 되돌린다.
 * 번호 공간이 커지면 이 실패가 늘어나므로, 이 방식은 인터럽트가
 * 많지 않은 시스템에 적합하다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   하이퍼바이저 인터페이스 드라이버 → [이 함수] →
 *   irq_alloc_desc_from() → irq_domain_associate()
 */
unsigned int irq_create_direct_mapping(struct irq_domain *domain)
{
	struct device_node *of_node;	/* [한국어] NUMA 노드를 알아낼 디바이스 트리 노드 */
	unsigned int virq;	/* [한국어] 배정받은 번호 */

	if (domain == NULL)	/* [한국어] 도메인을 지정하지 않았는가 */
		domain = irq_default_domain;	/* [한국어] 기본 도메인을 쓴다 */

	of_node = irq_domain_get_of_node(domain);	/* [한국어] 서술자를 어느 노드에 둘지 알아내기 위해 */
	virq = irq_alloc_desc_from(1, of_node_to_nid(of_node));	/* [한국어] 1 번부터 찾는다. 0 은 예약되어 장치에 배정되지 않는다 */
	if (!virq) {	/* [한국어] 빈 번호가 없는가 */
		pr_debug("create_direct virq allocation failed\n");	/* [한국어] 빈 번호가 없다. 직접 사상은 번호 공간을 많이 요구해 이 실패가 상대적으로 잦다 */
		return 0;	/* [한국어] 0 이 실패를 뜻한다 */
	}
	if (virq >= domain->hwirq_max) {	/* [한국어] 받은 번호가 도메인 범위를 넘는가. 서술자를 잡아 봐야 어떤 번호를 받을지 알 수 있어 검사가 뒤에 온다 */
		pr_err("ERROR: no free irqs available below %lu maximum\n",	/* [한국어] 번호 공간이 커지면 이 실패가 늘어난다. 이 방식은 인터럽트가 많지 않은 시스템에 적합하다 */
			domain->hwirq_max);
		irq_free_desc(virq);	/* [한국어] 방금 잡은 서술자를 되돌린다 */
		return 0;	/* [한국어] 0 이 실패를 뜻한다. 유효한 인터럽트 번호가 아니기 때문이다 */
	}
	pr_debug("create_direct obtained virq %d\n", virq);	/* [한국어] 성공 시 어떤 번호를 받았는지 남긴다. 이 번호가 곧 하드웨어 번호이기도 하다 */

	if (irq_domain_associate(domain, virq, virq)) {	/* [한국어] 두 번호가 같다 — 이것이 "직접 사상" 의 뜻이다 */
		irq_free_desc(virq);	/* [한국어] 사상 실패 시 서술자를 되돌린다 */
		return 0;	/* [한국어] 사상 실패. 위에서 서술자를 이미 되돌렸다 */
	}

	return virq;	/* [한국어] 이 번호가 곧 하드웨어 번호이기도 하다 */
}
EXPORT_SYMBOL_GPL(irq_create_direct_mapping);	/* [한국어] 하이퍼바이저 인터페이스 드라이버가 부른다 */
#endif	/* [한국어] CONFIG_IRQ_DOMAIN_NOMAP 분기의 끝 */

/*
 * [한국어]
 * irq_create_mapping_affinity_locked - 번호를 잡고 사상을 만든다 (락 보유)
 *
 * @domain:   대상 도메인
 * @hwirq:    하드웨어 번호
 * @affinity: 초기 친화도 (선택적)
 * @return:   배정된 리눅스 번호, 실패 시 0
 *
 * 사상을 새로 만드는 두 단계를 묶는다 — 리눅스 번호를 받고, 그것을
 * 하드웨어 번호에 잇는다.
 *
 * 두 단계가 한 임계 구역 안에 있어야 하는 이유: 그 사이에 다른
 * CPU 가 같은 하드웨어 번호로 사상을 만들면 두 리눅스 번호가 한
 * 하드웨어 인터럽트를 가리키게 된다. 그래서 호출자가 락을 잡고 이미
 * 사상이 있는지 확인한 뒤 이 함수를 부른다.
 *
 * 사상에 실패하면 서술자를 되돌리는 것에 주목: 번호만 잡고 잇지
 * 못한 서술자는 아무도 쓸 수 없는 채로 남는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   irq_create_mapping_affinity() / irq_create_fwspec_mapping() →
 *   [이 함수] → irq_domain_alloc_descs() → irq_domain_associate_locked()
 */
static unsigned int irq_create_mapping_affinity_locked(struct irq_domain *domain,
						       irq_hw_number_t hwirq,
						       const struct irq_affinity_desc *affinity)
{
	struct device_node *of_node = irq_domain_get_of_node(domain);	/* [한국어] NUMA 노드 판별과 로그용 */
	int virq;	/* [한국어] 배정받은 번호 */

	pr_debug("irq_create_mapping(0x%p, 0x%lx)\n", domain, hwirq);	/* [한국어] 어느 도메인의 어느 하드웨어 번호를 사상하려는지 남긴다 */

	/* Allocate a virtual interrupt number */
	virq = irq_domain_alloc_descs(-1, 1, hwirq, of_node_to_nid(of_node),	/* [한국어] (위 영어 주석) -1 은 "아무 번호나". hwirq 를 넘기는 것은 그것을 검색 시작점의 힌트로 쓰기 위해서다 */
				      affinity);
	if (virq <= 0) {	/* [한국어] 번호를 못 받았는가 */
		pr_debug("-> virq allocation failed\n");	/* [한국어] 번호 공간이 고갈됐다. 위 pr_debug 와 짝을 이뤄 어느 요청이 실패했는지 알 수 있다 */
		return 0;	/* [한국어] 0 이 실패를 뜻한다 */
	}

	if (irq_domain_associate_locked(domain, virq, hwirq)) {	/* [한국어] 두 번호를 잇는다. 호출자가 락을 쥐고 있어 그 사이에 다른 CPU 가 같은 하드웨어 번호를 가져가지 못한다 */
		irq_free_desc(virq);	/* [한국어] 잇지 못한 서술자는 아무도 쓸 수 없는 채로 남으므로 되돌린다 */
		return 0;	/* [한국어] 0 이 실패를 뜻한다 */
	}

	pr_debug("irq %lu on domain %s mapped to virtual irq %u\n",	/* [한국어] 부팅 시 인터럽트 번호 배정을 추적할 때 가장 유용한 줄이다 */
		hwirq, of_node_full_name(of_node), virq);

	return virq;	/* [한국어] 이제 이 번호로 request_irq 할 수 있다 */
}

/**
 * irq_create_mapping_affinity() - Map a hardware interrupt into linux irq space
 * @domain: domain owning this hardware interrupt or NULL for default domain
 * @hwirq: hardware irq number in that domain space
 * @affinity: irq affinity
 *
 * Only one mapping per hardware interrupt is permitted. Returns a linux
 * irq number.
 * If the sense/trigger is to be specified, set_irq_type() should be called
 * on the number returned from that call.
 */
/*
 * [한국어]
 * irq_create_mapping_affinity - 하드웨어 인터럽트를 리눅스 번호 공간에 사상한다
 *
 * @domain:   대상 도메인, NULL 이면 기본 도메인
 * @hwirq:    하드웨어 번호
 * @affinity: 초기 친화도 (선택적)
 * @return:   리눅스 인터럽트 번호, 실패 시 0
 *
 * 계층이 아닌 도메인에서 사상을 만드는 공개 진입점이다.
 *
 * 원본 주석의 두 조건이 중요하다. 하드웨어 인터럽트 하나에 사상은
 * 하나뿐이고, 트리거 방식은 이 함수가 정하지 않으므로 필요하면
 * 반환된 번호에 irq_set_irq_type() 을 따로 불러야 한다.
 *
 * 이미 사상이 있으면 그것을 돌려주는 것에 주목: 오류가 아니다.
 * 여러 드라이버가 같은 하드웨어 인터럽트를 공유하는 경우가 정상이고,
 * 그때 둘 다 같은 리눅스 번호를 받아야 한다.
 *
 * 락 안에서 확인하고 만드는 것이 핵심이다. 락 밖에서 확인하면 두
 * CPU 가 동시에 "없다" 고 보고 각자 만들 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irqchip 드라이버 / 옛 플랫폼 코드 → [이 함수] →
 *   irq_create_mapping_affinity_locked()
 */
unsigned int irq_create_mapping_affinity(struct irq_domain *domain,
					 irq_hw_number_t hwirq,
					 const struct irq_affinity_desc *affinity)
{
	int virq;	/* [한국어] 결과 번호 */

	/* Look for default domain if necessary */
	if (domain == NULL)	/* [한국어] (위 영어 주석) 도메인을 지정하지 않았는가 */
		domain = irq_default_domain;	/* [한국어] 기본 도메인을 쓴다 */
	if (domain == NULL) {	/* [한국어] 기본 도메인도 없는가 */
		WARN(1, "%s(, %lx) called with NULL domain\n", __func__, hwirq);	/* [한국어] 어느 컨트롤러의 인터럽트인지 알 방법이 없다 */
		return 0;	/* [한국어] 어느 컨트롤러의 인터럽트인지 알 방법이 없다 */
	}

	mutex_lock(&domain->root->mutex);	/* [한국어] 확인과 생성을 한 임계 구역에 묶는다. 락 밖에서 확인하면 두 CPU 가 각자 만들 수 있다 */

	/* Check if mapping already exists */
	virq = irq_find_mapping(domain, hwirq);	/* [한국어] (위 영어 주석) 이미 사상이 있는가 */
	if (virq) {	/* [한국어] 있으면 */
		pr_debug("existing mapping on virq %d\n", virq);	/* [한국어] 공유 인터럽트의 두 번째 요청이 이 경로다. 오류가 아니다 */
		goto out;	/* [한국어] 오류가 아니다. 여러 드라이버가 한 인터럽트를 공유하는 정상적인 경우이고, 그때 둘 다 같은 번호를 받아야 한다 */
	}

	virq = irq_create_mapping_affinity_locked(domain, hwirq, affinity);	/* [한국어] 새로 만든다 */
out:	/* [한국어] 기존 사상을 찾은 경로와 새로 만든 경로가 합류한다 */
	mutex_unlock(&domain->root->mutex);	/* [한국어] 기존 사상을 찾았든 새로 만들었든 여기서 푼다 */

	return virq;	/* [한국어] 리눅스 번호, 또는 실패 시 0 */
}
EXPORT_SYMBOL_GPL(irq_create_mapping_affinity);	/* [한국어] irqchip 드라이버가 부른다 */

/*
 * [한국어]
 * irq_domain_translate - 펌웨어 명세를 하드웨어 번호와 트리거로 바꾼다
 *
 * @d:      대상 도메인
 * @fwspec: 펌웨어 명세
 * @hwirq:  결과 하드웨어 번호 (출력)
 * @type:   결과 트리거 방식 (출력)
 * @return: 0 성공, 음수 드라이버 콜백의 오류
 *
 * 디바이스 트리의 "interrupts = <0 42 4>" 같은 값을 실제 번호로
 * 바꾸는 진입점이다. 세 단계로 시도한다.
 *
 * (1) translate 콜백: 계층형 도메인의 현대적 인터페이스. fwspec 을
 *     그대로 받으므로 ACPI 든 디바이스 트리든 다룰 수 있다.
 * (2) xlate 콜백: 옛 인터페이스. 디바이스 트리 노드와 매개변수 배열을
 *     따로 받는다. ACPI 를 다룰 수 없어 translate 로 대체되는 중이다.
 * (3) 둘 다 없으면 첫 매개변수를 하드웨어 번호로 본다.
 *
 * (3) 이 위험해 보이지만 실제로는 합리적이다. 매개변수 하나짜리
 * 컨트롤러가 흔하고, 그 하나는 거의 항상 인터럽트 번호다. 다만 트리거
 * 방식은 채우지 않으므로 호출자가 초기화해 둔 값(IRQ_TYPE_NONE)이
 * 그대로 남는다.
 *
 * translate 를 #ifdef 로 감싼 이유: 그 콜백은 계층형 도메인 구조체에만
 * 있다. 계층형을 뺀 빌드에서는 필드 자체가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_create_fwspec_mapping() → [이 함수] → d->ops->translate/xlate
 */
static int irq_domain_translate(struct irq_domain *d,
				struct irq_fwspec *fwspec,
				irq_hw_number_t *hwirq, unsigned int *type)
{
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY	/* [한국어] translate 콜백은 계층형 도메인 구조체에만 있는 필드다 */
	if (d->ops->translate)	/* [한국어] 현대적 인터페이스를 제공하는가 */
		return d->ops->translate(d, fwspec, hwirq, type);	/* [한국어] fwspec 을 그대로 받아 ACPI 든 디바이스 트리든 다룰 수 있다 */
#endif
	if (d->ops->xlate)	/* [한국어] 옛 인터페이스인가 */
		return d->ops->xlate(d, to_of_node(fwspec->fwnode),	/* [한국어] 디바이스 트리 노드와 매개변수 배열을 따로 받는다. ACPI 를 다룰 수 없어 translate 로 대체되는 중이다 */
				     fwspec->param, fwspec->param_count,
				     hwirq, type);

	/* If domain has no translation, then we assume interrupt line */
	*hwirq = fwspec->param[0];	/* [한국어] (위 영어 주석) 매개변수 하나짜리 컨트롤러가 흔하고 그 하나는 거의 항상 인터럽트 번호다. 트리거 방식은 채우지 않아 호출자의 초기값이 남는다 */
	return 0;
}

/*
 * [한국어]
 * of_phandle_args_to_fwspec - 디바이스 트리 인자를 fwspec 으로 옮긴다
 *
 * @np:     컨트롤러 노드
 * @args:   매개변수 배열
 * @count:  매개변수 개수
 * @fwspec: 채울 구조체 (출력)
 * @return: 없음
 *
 * 디바이스 트리의 phandle 인자 구조체를 펌웨어 중립적인 fwspec 으로
 * 변환한다. ACPI 와 디바이스 트리를 같은 코드로 다루려는 설계의
 * 접합부다.
 *
 * 배열 크기를 검사하지 않는 것에 주목: fwspec->param 은 고정 크기
 * 배열(대개 16 개)인데 count 를 그대로 믿는다. 디바이스 트리 파서가
 * 이미 상한을 강제하므로 여기서 다시 검사하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_create_of_mapping() / irq_domain_xlate_twocell() 계열 → [이 함수]
 */
void of_phandle_args_to_fwspec(struct device_node *np, const u32 *args,
			       unsigned int count, struct irq_fwspec *fwspec)
{
	int i;	/* [한국어] 순회용 */

	fwspec->fwnode = of_fwnode_handle(np);	/* [한국어] 디바이스 트리 노드를 중립적인 fwnode 로 감싼다 */
	fwspec->param_count = count;	/* [한국어] 매개변수 개수 */

	for (i = 0; i < count; i++)	/* [한국어] 상한을 검사하지 않는다 — 디바이스 트리 파서가 이미 강제한다 */
		fwspec->param[i] = args[i];	/* [한국어] 매개변수를 하나씩 복사한다 */
}
EXPORT_SYMBOL_GPL(of_phandle_args_to_fwspec);	/* [한국어] irqchip 드라이버의 xlate 구현이 부른다 */

/*
 * [한국어]
 * fwspec_to_domain - 펌웨어 명세에 맞는 도메인을 찾는다
 *
 * @fwspec: 펌웨어 명세
 * @return: 찾은 도메인, 없으면 NULL
 *
 * 두 번 시도하는 것이 이 함수의 요점이다. 먼저 DOMAIN_BUS_WIRED 로
 * 찾고, 못 찾으면 DOMAIN_BUS_ANY 로 다시 찾는다.
 *
 * 왜 그런가: 한 컨트롤러 노드 위에 여러 도메인이 있을 수 있다. GIC
 * 위에는 배선 인터럽트용 도메인과 MSI 용 도메인이 함께 있다. 디바이스
 * 트리에 적힌 인터럽트는 배선이므로 WIRED 도메인을 먼저 찾아야 한다.
 *
 * 그런데 도메인이 하나뿐인 단순한 컨트롤러는 종류를 지정하지 않아
 * DOMAIN_BUS_ANY 다. 그래서 두 번째 시도가 필요하다.
 *
 * fwnode 가 없으면 기본 도메인으로 가는 것에 주목: 디바이스 트리를
 * 쓰지 않는 플랫폼의 경로다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_create_fwspec_mapping() / irq_populate_fwspec_info() →
 *   [이 함수] → irq_find_matching_fwspec()
 */
static struct irq_domain *fwspec_to_domain(struct irq_fwspec *fwspec)
{
	struct irq_domain *domain;	/* [한국어] 찾은 도메인 */

	if (fwspec->fwnode) {	/* [한국어] 컨트롤러 노드가 지정됐는가 */
		domain = irq_find_matching_fwspec(fwspec, DOMAIN_BUS_WIRED);	/* [한국어] 배선 인터럽트용 도메인을 먼저. 한 노드 위에 배선용과 MSI 용이 함께 있을 수 있다 */
		if (!domain)	/* [한국어] 못 찾았는가 */
			domain = irq_find_matching_fwspec(fwspec, DOMAIN_BUS_ANY);	/* [한국어] 도메인이 하나뿐인 단순한 컨트롤러는 종류를 지정하지 않는다 */
	} else {	/* [한국어] 노드가 없는 경우 */
		domain = irq_default_domain;	/* [한국어] 디바이스 트리를 쓰지 않는 플랫폼의 경로다 */
	}

	return domain;	/* [한국어] 못 찾았으면 NULL */
}

#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY	/* [한국어] get_fwspec_info 콜백은 계층형 도메인에만 있다 */
/*
 * [한국어]
 * irq_populate_fwspec_info - 명세에 대한 부가 정보를 도메인에서 얻는다
 *
 * @fwspec: 펌웨어 명세
 * @info:   채울 정보 구조체 (출력)
 * @return: 0 성공 또는 정보 없음, 음수 도메인 콜백의 오류
 *
 * 인터럽트를 실제로 매핑하지 않고 그에 대한 정보만 묻는 통로다.
 * 드라이버가 "이 인터럽트가 wakeup 을 지원하는가" 같은 것을 미리
 * 알아야 할 때 쓴다.
 *
 * 정보를 제공하지 않는 도메인에 0 을 돌려주는 것에 주목: 오류가
 * 아니다. memset 으로 구조체를 비워 두었으므로 호출자는 "정보 없음"
 * 상태를 받는다.
 *
 * memset 을 조회보다 먼저 하는 이유: 도메인을 못 찾거나 콜백이 없어
 * 일찍 반환할 때도 구조체가 정의된 상태여야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 / of_irq 헬퍼 → [이 함수] → domain->ops->get_fwspec_info()
 */
int irq_populate_fwspec_info(struct irq_fwspec *fwspec, struct irq_fwspec_info *info)
{
	struct irq_domain *domain = fwspec_to_domain(fwspec);	/* [한국어] 명세가 가리키는 도메인 */

	memset(info, 0, sizeof(*info));	/* [한국어] 조회보다 먼저 비운다. 일찍 반환할 때도 구조체가 정의된 상태여야 한다 */

	if (!domain || !domain->ops->get_fwspec_info)	/* [한국어] 도메인이 없거나 정보를 제공하지 않는가 */
		return 0;	/* [한국어] 오류가 아니다. 비워 둔 구조체가 "정보 없음" 을 뜻한다 */

	return domain->ops->get_fwspec_info(fwspec, info);	/* [한국어] 도메인이 명세를 해석해 부가 정보를 채운다 */
}
#endif	/* [한국어] CONFIG_IRQ_DOMAIN_HIERARCHY 분기의 끝 */

/*
 * [한국어]
 * irq_create_fwspec_mapping - 펌웨어 명세로부터 리눅스 인터럽트 번호를 얻는다
 *
 * @fwspec: 펌웨어 명세 (컨트롤러 노드 + 매개변수)
 * @return: 리눅스 인터럽트 번호, 실패 시 0
 *
 * 디바이스 트리와 ACPI 경로의 핵심 함수다. 드라이버가 platform_get_irq()
 * 등을 부르면 결국 여기로 온다. "이 장치의 인터럽트가 리눅스에서 몇
 * 번인가" 에 답한다.
 *
 * 절차가 네 단계다.
 *
 *   1. 명세가 가리키는 도메인을 찾는다.
 *   2. 명세를 하드웨어 번호와 트리거 방식으로 해석한다.
 *   3. 이미 사상이 있으면 트리거 방식만 맞춰 보고 그 번호를 쓴다.
 *   4. 없으면 새로 만든다 — 계층형인지에 따라 경로가 갈린다.
 *
 * 3 단계의 세 갈래가 이 함수에서 가장 미묘하다.
 *   - 요청한 트리거가 없거나 현재와 같으면: 그대로 쓴다.
 *   - 현재 트리거가 정해지지 않았으면: 지금 정한다. 같은 인터럽트를
 *     두 장치가 공유하는데 한쪽만 트리거를 아는 경우다.
 *   - 둘 다 정해졌는데 다르면: 거절한다. 한 선을 레벨과 에지로
 *     동시에 쓸 수 없다.
 *
 * 4 단계의 MSI 갈래에서 락을 잠시 놓는 것에 주목: wire-to-MSI 도메인의
 * 할당 경로가 MSI 서술자 뮤텍스를 잡는데, 도메인 뮤텍스를 쥔 채 그것을
 * 잡으면 락 순서가 어긋난다. 놓았다 다시 잡는 사이에 다른 CPU 가
 * 같은 사상을 만들 수 있지만, 이 경로가 부팅 중 설정이라 실제로는
 * 일어나지 않는다.
 *
 * 트리거 비트 검사: 컨트롤러가 IRQ_TYPE_SENSE_MASK 밖의 비트를 채워
 * 돌려주면 그것은 버그다. 경고하고 잘라 낸다 — 그 비트가 다른 뜻으로
 * 해석되어 엉뚱한 설정이 되는 것을 막는다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_create_of_mapping() / acpi_register_gsi() → [이 함수] →
 *   irq_domain_translate() → irq_domain_alloc_irqs_locked() 또는
 *   irq_create_mapping_affinity_locked()
 */
unsigned int irq_create_fwspec_mapping(struct irq_fwspec *fwspec)
{
	unsigned int type = IRQ_TYPE_NONE;	/* [한국어] 해석 결과 트리거 방식. 도메인이 채우지 않으면 이 값이 남는다 */
	struct irq_domain *domain;	/* [한국어] 명세가 가리키는 도메인 */
	struct irq_data *irq_data;	/* [한국어] 트리거를 기록할 대상 */
	irq_hw_number_t hwirq;	/* [한국어] 해석 결과 하드웨어 번호 */
	int virq;	/* [한국어] 결과 리눅스 번호 */

	domain = fwspec_to_domain(fwspec);	/* [한국어] 배선 도메인을 먼저, 없으면 종류 무관으로 찾는다 */
	if (!domain) {	/* [한국어] 못 찾았는가 */
		pr_warn("no irq domain found for %s !\n",	/* [한국어] 컨트롤러 드라이버가 아직 프로브되지 않았을 수 있다. 그러면 호출자가 지연 프로브로 다시 시도한다 */
			of_node_full_name(to_of_node(fwspec->fwnode)));
		return 0;	/* [한국어] 정보를 제공하지 않는 도메인이다. 비워 둔 구조체가 "정보 없음" 을 뜻한다 */
	}

	if (irq_domain_translate(domain, fwspec, &hwirq, &type))	/* [한국어] 명세를 번호와 트리거로 해석한다 */
		return 0;	/* [한국어] 컨트롤러가 이 명세를 이해하지 못했다 */

	/*
	 * WARN if the irqchip returns a type with bits
	 * outside the sense mask set and clear these bits.
	 */
	if (WARN_ON(type & ~IRQ_TYPE_SENSE_MASK))	/* [한국어] (위 영어 주석) 컨트롤러가 트리거 마스크 밖의 비트를 채웠는가 — 드라이버의 버그다 */
		type &= IRQ_TYPE_SENSE_MASK;	/* [한국어] 잘라 낸다. 그 비트가 다른 뜻으로 해석되어 엉뚱한 설정이 되는 것을 막는다 */

	mutex_lock(&domain->root->mutex);	/* [한국어] 확인과 생성을 한 임계 구역에 묶는다 */

	/*
	 * If we've already configured this interrupt,
	 * don't do it again, or hell will break loose.
	 */
	virq = irq_find_mapping(domain, hwirq);	/* [한국어] (위 영어 주석) 이미 사상이 있는가 */
	if (virq) {	/* [한국어] 있으면 트리거 방식만 맞춰 본다 */
		/*
		 * If the trigger type is not specified or matches the
		 * current trigger type then we are done so return the
		 * interrupt number.
		 */
		if (type == IRQ_TYPE_NONE || type == irq_get_trigger_type(virq))	/* [한국어] (위 영어 주석) 요청한 트리거가 없거나 현재와 같은가 */
			goto out;	/* [한국어] 그대로 쓴다. 공유 인터럽트의 두 번째 요청이 이 경로다 */

		/*
		 * If the trigger type has not been set yet, then set
		 * it now and return the interrupt number.
		 */
		if (irq_get_trigger_type(virq) == IRQ_TYPE_NONE) {	/* [한국어] (위 영어 주석) 현재 트리거가 정해지지 않았는가 */
			irq_data = irq_get_irq_data(virq);	/* [한국어] 기록할 대상 */
			if (!irq_data) {	/* [한국어] 사상은 있는데 서술자가 없는가 — 있을 수 없는 상태다 */
				virq = 0;	/* [한국어] 사상은 있는데 서술자가 없다 — 있을 수 없는 상태라 실패로 처리한다 */
				goto out;	/* [한국어] 아래에서 락을 풀고 0 을 반환한다 */
			}

			irqd_set_trigger_type(irq_data, type);	/* [한국어] 지금 정한다. 같은 인터럽트를 두 장치가 공유하는데 한쪽만 트리거를 아는 경우다 */
			goto out;	/* [한국어] 트리거를 방금 정했다. 더 할 일이 없다 */
		}

		pr_warn("type mismatch, failed to map hwirq-%lu for %s!\n",	/* [한국어] 둘 다 정해졌는데 다르다. 한 선을 레벨과 에지로 동시에 쓸 수 없다 */
			hwirq, of_node_full_name(to_of_node(fwspec->fwnode)));
		virq = 0;	/* [한국어] 거절한다. 어느 한쪽을 따르면 다른 쪽이 오동작한다 */
		goto out;	/* [한국어] 트리거가 충돌해 거절했다 */
	}

	if (irq_domain_is_hierarchy(domain)) {	/* [한국어] 계층형 도메인인가 */
		if (irq_domain_is_msi_device(domain)) {	/* [한국어] wire-to-MSI 변환 도메인인가 */
			mutex_unlock(&domain->root->mutex);	/* [한국어] 아래 경로가 MSI 서술자 뮤텍스를 잡는다. 도메인 뮤텍스를 쥔 채 잡으면 락 순서가 어긋난다 */
			virq = msi_device_domain_alloc_wired(domain, hwirq, type);	/* [한국어] 배선 번호를 쿠키에 실어 MSI 로 할당한다 (kernel/irq/msi.c) */
			mutex_lock(&domain->root->mutex);	/* [한국어] 다시 잡는다. 그 사이에 다른 CPU 가 같은 사상을 만들 수 있지만 이 경로는 부팅 중 설정이라 실제로 일어나지 않는다 */
		} else	/* [한국어] 보통의 계층형 도메인 */
			virq = irq_domain_alloc_irqs_locked(domain, -1, 1, NUMA_NO_NODE,	/* [한국어] 계층 전체에 걸쳐 하나를 할당한다. fwspec 을 arg 로 넘겨 각 층이 해석하게 한다 */
							    fwspec, false, NULL);
		if (virq <= 0) {	/* [한국어] 실패 */
			virq = 0;	/* [한국어] 음수 오류를 0 으로 통일한다. 이 API 는 0 만을 실패로 쓴다 */
			goto out;	/* [한국어] 계층형 할당이 실패했다 */
		}
	} else {	/* [한국어] 평평한 도메인 */
		/* Create mapping */
		virq = irq_create_mapping_affinity_locked(domain, hwirq, NULL);	/* [한국어] (위 영어 주석) 번호를 잡고 사상을 만든다 */
		if (!virq)	/* [한국어] 실패 */
			goto out;
	}

	irq_data = irq_get_irq_data(virq);	/* [한국어] 트리거를 기록할 대상 */
	if (WARN_ON(!irq_data)) {	/* [한국어] 방금 할당했는데 서술자가 없는가 — 있을 수 없는 상태다 */
		virq = 0;	/* [한국어] 방금 할당했는데 서술자가 없다 — 있을 수 없는 상태다 */
		goto out;	/* [한국어] 락을 풀고 실패를 알린다 */
	}

	/* Store trigger type */
	irqd_set_trigger_type(irq_data, type);	/* [한국어] (위 영어 주석) 해석된 트리거를 기록한다. 나중에 __irq_set_trigger 가 이 값으로 하드웨어를 설정한다 */
out:	/* [한국어] 모든 경로가 여기서 합류해 락을 푼다 */
	mutex_unlock(&domain->root->mutex);	/* [한국어] 모든 경로가 여기를 지난다. goto out 이 여럿인 이유가 이 한 줄을 공유하기 위해서다 */

	return virq;	/* [한국어] 리눅스 번호, 실패 시 0 */
}
EXPORT_SYMBOL_GPL(irq_create_fwspec_mapping);	/* [한국어] ACPI 코어와 of_irq 헬퍼가 부른다 */

/*
 * [한국어]
 * irq_create_of_mapping - 디바이스 트리 인자로 인터럽트 번호를 얻는다
 *
 * @irq_data: 디바이스 트리의 phandle 인자 (노드 + 매개변수)
 * @return:   리눅스 인터럽트 번호, 실패 시 0
 *
 * 위 함수의 디바이스 트리 전용 껍데기다. of_phandle_args 를 fwspec 으로
 * 옮기고 넘긴다.
 *
 * 인자 이름이 irq_data 인 것이 헷갈린다 — struct irq_data 와 아무
 * 관계가 없고 struct of_phandle_args 다. 오래된 코드의 이름이 남은
 * 것이다.
 *
 * fwspec 을 스택에 만드는 것에 주목: 이 함수가 끝나면 사라진다.
 * 아래 경로가 그 내용을 복사해 쓰므로 안전하다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   of_irq_get() / platform_get_irq() → [이 함수] →
 *   irq_create_fwspec_mapping()
 */
unsigned int irq_create_of_mapping(struct of_phandle_args *irq_data)
{
	struct irq_fwspec fwspec;	/* [한국어] 스택에 만든다. 아래 경로가 내용을 복사해 쓰므로 안전하다 */

	of_phandle_args_to_fwspec(irq_data->np, irq_data->args,	/* [한국어] 인자 이름이 irq_data 이지만 struct irq_data 와 무관한 of_phandle_args 다. 오래된 이름이 남았다 */
				  irq_data->args_count, &fwspec);

	return irq_create_fwspec_mapping(&fwspec);	/* [한국어] 펌웨어 중립 경로에 위임 */
}
EXPORT_SYMBOL_GPL(irq_create_of_mapping);	/* [한국어] 거의 모든 디바이스 트리 드라이버가 간접적으로 부른다 */

/**
 * irq_dispose_mapping() - Unmap an interrupt
 * @virq: linux irq number of the interrupt to unmap
 */
/*
 * [한국어]
 * irq_dispose_mapping - 사상을 끊고 인터럽트 번호를 반납한다
 *
 * @virq: 반납할 리눅스 인터럽트 번호
 * @return: 없음
 *
 * irq_create_*_mapping() 계열의 반대다. 도메인이 계층형인지에 따라
 * 경로가 갈린다.
 *
 * 계층형은 irq_domain_free_one_irq() 로 간다. 그 안에서 다시 MSI
 * 여부를 보고 갈린다 — 계층 구조를 따라 각 층의 자원을 반납해야
 * 하기 때문이다.
 *
 * 평평한 도메인은 사상을 끊고 서술자를 반납하는 두 단계다. 순서가
 * 중요한데, 사상이 살아 있는 상태에서 서술자를 반납하면 도메인의
 * 역방향 맵이 해제된 메모리를 가리킨다.
 *
 * virq 가 0 인 경우를 조용히 넘기는 것에 주목: 0 은 "인터럽트 없음"
 * 을 뜻하므로 정리 경로에서 조건 없이 부를 수 있게 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 remove / of_irq 정리 → [이 함수] →
 *   irq_domain_free_one_irq() 또는 irq_domain_disassociate()
 */
void irq_dispose_mapping(unsigned int virq)
{
	struct irq_data *irq_data;	/* [한국어] 대상의 irq_data */
	struct irq_domain *domain;	/* [한국어] 그것이 속한 도메인 */

	irq_data = virq ? irq_get_irq_data(virq) : NULL;	/* [한국어] 0 은 "인터럽트 없음" 이라 정리 경로에서 조건 없이 부를 수 있게 한다 */
	if (!irq_data)	/* [한국어] 없거나 유효하지 않은 번호인가 */
		return;

	domain = irq_data->domain;	/* [한국어] 이 인터럽트가 속한 도메인 */
	if (WARN_ON(domain == NULL))	/* [한국어] 도메인에 붙지 않은 인터럽트인가 — 사상이 없으니 끊을 것도 없다 */
		return;

	if (irq_domain_is_hierarchy(domain)) {	/* [한국어] 계층형인가 */
		irq_domain_free_one_irq(domain, virq);	/* [한국어] 계층 각 층의 자원을 반납해야 한다. 그 안에서 다시 MSI 여부로 갈린다 */
	} else {	/* [한국어] 평평한 도메인 */
		irq_domain_disassociate(domain, virq);	/* [한국어] 먼저 사상을 끊는다. 순서가 반대면 역방향 맵이 해제된 메모리를 가리킨다 */
		irq_free_desc(virq);	/* [한국어] 서술자 반납 */
	}
}
EXPORT_SYMBOL_GPL(irq_dispose_mapping);	/* [한국어] 드라이버 정리 경로가 부른다 */

/**
 * __irq_resolve_mapping() - Find a linux irq from a hw irq number.
 * @domain: domain owning this hardware interrupt
 * @hwirq: hardware irq number in that domain space
 * @irq: optional pointer to return the Linux irq if required
 *
 * Returns the interrupt descriptor.
 */
/*
 * [한국어]
 * __irq_resolve_mapping - 하드웨어 번호로 서술자를 찾는다
 *
 * @domain: 대상 도메인, NULL 이면 기본 도메인
 * @hwirq:  하드웨어 인터럽트 번호
 * @irq:    리눅스 번호도 필요하면 그것을 담을 곳 (선택적)
 * @return: 서술자, 없으면 NULL
 *
 * 이 파일에서 인터럽트 경로에 있는 유일한 함수다. 인터럽트가 발생할
 * 때마다 불리므로 가장 빨라야 한다.
 *
 * 세 가지 조회 방식이 있다.
 *
 * (1) NOMAP 도메인: 리눅스 번호가 곧 하드웨어 번호라 조회가 항등
 *     함수다. 다만 그 번호의 irq_data 가 실제로 이 도메인에 붙어
 *     있는지는 확인해야 한다.
 * (2) 선형 배열: 작은 하드웨어 번호. 배열 인덱싱 한 번이라 가장 빠르다.
 * (3) 래딕스 트리: 큰 하드웨어 번호. 로그 시간이지만 듬성듬성한
 *     번호 공간을 낭비 없이 담는다.
 *
 * RCU 로 감싸는 이유: 이 함수는 인터럽트 문맥에서 락 없이 불린다.
 * 그 사이에 다른 CPU 가 사상을 없앨 수 있는데, RCU 읽기 구역 안에
 * 있으면 그 자료구조가 해제되지 않는다.
 *
 * NOMAP 경로가 RCU 밖에 있는 것에 주목: 그쪽은 irq_data 를 서술자
 * 조회로 직접 얻으므로 이 파일의 자료구조를 만지지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 문맥 포함, 제약 없음.
 *
 * 호출 체인:
 *   generic_handle_domain_irq() (kernel/irq/irqdesc.c) → [이 함수]
 */
struct irq_desc *__irq_resolve_mapping(struct irq_domain *domain,
				       irq_hw_number_t hwirq,
				       unsigned int *irq)
{
	struct irq_desc *desc = NULL;	/* [한국어] 결과. 못 찾으면 NULL 이 그대로 나간다 */
	struct irq_data *data;	/* [한국어] 맵에서 찾은 irq_data */

	/* Look for default domain if necessary */
	if (domain == NULL)	/* [한국어] (위 영어 주석) 도메인을 지정하지 않았는가 */
		domain = irq_default_domain;	/* [한국어] 기본 도메인 */
	if (domain == NULL)	/* [한국어] 그것도 없는가 */
		return desc;	/* [한국어] NULL. 호출자가 오탐으로 처리한다 */

	if (irq_domain_is_nomap(domain)) {	/* [한국어] 리눅스 번호가 곧 하드웨어 번호인 도메인인가 */
		if (hwirq < domain->hwirq_max) {	/* [한국어] 범위 안인가 */
			data = irq_domain_get_irq_data(domain, hwirq);	/* [한국어] 번호가 같으므로 서술자 조회로 바로 얻는다 */
			if (data && data->hwirq == hwirq)	/* [한국어] 그 irq_data 가 실제로 이 번호에 붙어 있는가. 조회가 항등이어도 확인은 필요하다 */
				desc = irq_data_to_desc(data);	/* [한국어] irq_data 에서 감싸는 서술자를 되찾는다 */
			if (irq && desc)	/* [한국어] 리눅스 번호도 원하고 찾았는가 */
				*irq = hwirq;	/* [한국어] 두 번호가 같다 */
		}

		return desc;	/* [한국어] RCU 를 쓰지 않는다 — 이 경로는 이 파일의 자료구조를 만지지 않는다 */
	}

	rcu_read_lock();	/* [한국어] 인터럽트 문맥에서 락 없이 불린다. 그 사이 다른 CPU 가 사상을 없애도 자료구조가 해제되지 않게 한다 */
	/* Check if the hwirq is in the linear revmap. */
	if (hwirq < domain->revmap_size)	/* [한국어] (위 영어 주석) 선형 배열이 덮는 범위인가 */
		data = rcu_dereference(domain->revmap[hwirq]);	/* [한국어] 배열 인덱싱 한 번. 가장 빠른 경로이고 대부분의 인터럽트가 여기로 온다 */
	else	/* [한국어] 큰 번호 */
		data = radix_tree_lookup(&domain->revmap_tree, hwirq);	/* [한국어] 로그 시간이지만 듬성듬성한 번호 공간을 낭비 없이 담는다 */

	if (likely(data)) {	/* [한국어] 찾았는가. 정상 경로이므로 likely */
		desc = irq_data_to_desc(data);	/* [한국어] 감싸는 서술자 */
		if (irq)	/* [한국어] 리눅스 번호도 원하는가 */
			*irq = data->irq;	/* [한국어] irq_data 가 자기 번호를 들고 있다 */
	}

	rcu_read_unlock();	/* [한국어] 반환된 서술자를 호출자가 쓰는 동안의 보호는 호출자 몫이다. 인터럽트 문맥은 자체로 RCU 읽기 구역이라 안전하다 */
	return desc;	/* [한국어] 못 찾았으면 NULL */
}
EXPORT_SYMBOL_GPL(__irq_resolve_mapping);	/* [한국어] irqchip 드라이버가 간접적으로 부른다 */

/**
 * irq_domain_xlate_onecell() - Generic xlate for direct one cell bindings
 * @d:		Interrupt domain involved in the translation
 * @ctrlr:	The device tree node for the device whose interrupt is translated
 * @intspec:	The interrupt specifier data from the device tree
 * @intsize:	The number of entries in @intspec
 * @out_hwirq:	Pointer to storage for the hardware interrupt number
 * @out_type:	Pointer to storage for the interrupt type
 *
 * Device Tree IRQ specifier translation function which works with one cell
 * bindings where the cell value maps directly to the hwirq number.
 */
/*
 * [한국어]
 * irq_domain_xlate_onecell - 셀 하나짜리 디바이스 트리 명세를 해석한다
 *
 * @d:         대상 도메인 (쓰지 않는다)
 * @ctrlr:     컨트롤러 노드 (쓰지 않는다)
 * @intspec:   명세 배열
 * @intsize:   배열 크기
 * @out_hwirq: 결과 하드웨어 번호 (출력)
 * @out_type:  결과 트리거 방식 (출력)
 * @return:    0 성공, -EINVAL 셀이 부족함
 *
 * "#interrupt-cells = <1>" 인 컨트롤러의 명세를 해석한다. 셀 하나가
 * 그대로 하드웨어 번호다.
 *
 * 트리거를 IRQ_TYPE_NONE 으로 두는 것에 주목: 셀 하나로는 트리거를
 * 표현할 수 없다. 그런 컨트롤러는 트리거가 하드웨어적으로 고정이거나
 * 소프트웨어가 정하지 않는다는 뜻이다.
 *
 * 이런 공용 구현이 여럿 있는 이유: 디바이스 트리 명세의 형식이 몇
 * 가지로 정형화되어 있어, 드라이버마다 같은 코드를 쓸 이유가 없다.
 * 드라이버는 자기 형식에 맞는 것을 ops.xlate 에 꽂기만 하면 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_domain_translate() → d->ops->xlate → [이 함수]
 */
int irq_domain_xlate_onecell(struct irq_domain *d, struct device_node *ctrlr,
			     const u32 *intspec, unsigned int intsize,
			     unsigned long *out_hwirq, unsigned int *out_type)
{
	if (WARN_ON(intsize < 1))	/* [한국어] 셀이 하나도 없는가 — 디바이스 트리가 잘못됐다 */
		return -EINVAL;
	*out_hwirq = intspec[0];	/* [한국어] 셀 하나가 그대로 하드웨어 번호 */
	*out_type = IRQ_TYPE_NONE;	/* [한국어] 셀 하나로는 트리거를 표현할 수 없다. 하드웨어적으로 고정이거나 소프트웨어가 정하지 않는다는 뜻이다 */
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_xlate_onecell);	/* [한국어] 단순한 컨트롤러 드라이버가 ops.xlate 에 꽂는다 */

/**
 * irq_domain_xlate_twocell() - Generic xlate for direct two cell bindings
 * @d:		Interrupt domain involved in the translation
 * @ctrlr:	The device tree node for the device whose interrupt is translated
 * @intspec:	The interrupt specifier data from the device tree
 * @intsize:	The number of entries in @intspec
 * @out_hwirq:	Pointer to storage for the hardware interrupt number
 * @out_type:	Pointer to storage for the interrupt type
 *
 * Device Tree IRQ specifier translation function which works with two cell
 * bindings where the cell values map directly to the hwirq number
 * and linux irq flags.
 */
/*
 * [한국어]
 * irq_domain_xlate_twocell - 셀 두 개짜리 명세를 해석한다
 *
 * @d:         대상 도메인
 * @ctrlr:     컨트롤러 노드
 * @intspec:   명세 배열
 * @intsize:   배열 크기
 * @out_hwirq: 결과 하드웨어 번호 (출력)
 * @out_type:  결과 트리거 방식 (출력)
 * @return:    0 성공, -EINVAL 셀 부족
 *
 * "#interrupt-cells = <2>" 인 컨트롤러용이다. 첫 셀이 번호, 둘째가
 * 트리거 방식이다. 가장 흔한 형식이다.
 *
 * 직접 구현하지 않고 fwspec 으로 바꿔 translate 판에 위임하는 것에
 * 주목: 두 판의 논리가 같은데 인자 형식만 다르다. 한쪽만 구현하고
 * 다른 쪽은 변환해 부르면 중복이 없다.
 *
 * 이 변환 계층이 있는 이유는 ACPI 지원이다. xlate 는 디바이스 트리
 * 전용이고 translate 는 펌웨어 중립이라, 새 코드는 translate 를 쓴다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_domain_translate() → d->ops->xlate → [이 함수] →
 *   irq_domain_translate_twocell()
 */
int irq_domain_xlate_twocell(struct irq_domain *d, struct device_node *ctrlr,
			const u32 *intspec, unsigned int intsize,
			irq_hw_number_t *out_hwirq, unsigned int *out_type)
{
	struct irq_fwspec fwspec;	/* [한국어] 펌웨어 중립 형식으로 변환할 임시 구조체 */

	of_phandle_args_to_fwspec(ctrlr, intspec, intsize, &fwspec);	/* [한국어] 디바이스 트리 형식을 중립 형식으로 */
	return irq_domain_translate_twocell(d, &fwspec, out_hwirq, out_type);	/* [한국어] 논리는 같고 인자 형식만 다르므로 한쪽만 구현하고 변환해 부른다 */
}
EXPORT_SYMBOL_GPL(irq_domain_xlate_twocell);	/* [한국어] 가장 많이 쓰이는 xlate 구현 */

/**
 * irq_domain_xlate_twothreecell() - Generic xlate for direct two or three cell bindings
 * @d:		Interrupt domain involved in the translation
 * @ctrlr:	The device tree node for the device whose interrupt is translated
 * @intspec:	The interrupt specifier data from the device tree
 * @intsize:	The number of entries in @intspec
 * @out_hwirq:	Pointer to storage for the hardware interrupt number
 * @out_type:	Pointer to storage for the interrupt type
 *
 * Device Tree interrupt specifier translation function for two or three
 * cell bindings, where the cell values map directly to the hardware
 * interrupt number and the type specifier.
 */
/*
 * [한국어]
 * irq_domain_xlate_twothreecell - 셀 두 개 또는 세 개짜리 명세를 해석한다
 *
 * @d:         대상 도메인
 * @ctrlr:     컨트롤러 노드
 * @intspec:   명세 배열
 * @intsize:   배열 크기
 * @out_hwirq: 결과 하드웨어 번호 (출력)
 * @out_type:  결과 트리거 방식 (출력)
 * @return:    0 성공, -EINVAL 셀 개수가 2 도 3 도 아님
 *
 * 셀이 세 개인 형식은 첫 셀이 "인터럽트 종류" 를 뜻하는 경우가 많다.
 * ARM GIC 가 대표적으로 <종류 번호 플래그> 형식을 쓴다. 그래서 세 셀
 * 형식에서는 번호가 두 번째 셀이다.
 *
 * 한 컨트롤러가 두 형식을 모두 받는 이유: 디바이스 트리 바인딩이
 * 시간에 따라 확장되면서 옛 형식도 계속 지원해야 하는 경우다.
 *
 * 위 twocell 과 마찬가지로 translate 판에 위임한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_domain_translate() → d->ops->xlate → [이 함수] →
 *   irq_domain_translate_twothreecell()
 */
int irq_domain_xlate_twothreecell(struct irq_domain *d, struct device_node *ctrlr,
				  const u32 *intspec, unsigned int intsize,
				  irq_hw_number_t *out_hwirq, unsigned int *out_type)
{
	struct irq_fwspec fwspec;	/* [한국어] 중립 형식 임시 구조체 */

	of_phandle_args_to_fwspec(ctrlr, intspec, intsize, &fwspec);	/* [한국어] 형식 변환 */

	return irq_domain_translate_twothreecell(d, &fwspec, out_hwirq, out_type);	/* [한국어] 실제 해석은 중립 판이 한다 */
}
EXPORT_SYMBOL_GPL(irq_domain_xlate_twothreecell);	/* [한국어] 두 형식을 모두 받는 컨트롤러용 */

/**
 * irq_domain_xlate_onetwocell() - Generic xlate for one or two cell bindings
 * @d:		Interrupt domain involved in the translation
 * @ctrlr:	The device tree node for the device whose interrupt is translated
 * @intspec:	The interrupt specifier data from the device tree
 * @intsize:	The number of entries in @intspec
 * @out_hwirq:	Pointer to storage for the hardware interrupt number
 * @out_type:	Pointer to storage for the interrupt type
 *
 * Device Tree IRQ specifier translation function which works with either one
 * or two cell bindings where the cell values map directly to the hwirq number
 * and linux irq flags.
 *
 * Note: don't use this function unless your interrupt controller explicitly
 * supports both one and two cell bindings.  For the majority of controllers
 * the _onecell() or _twocell() variants above should be used.
 */
/*
 * [한국어]
 * irq_domain_xlate_onetwocell - 셀 한 개 또는 두 개짜리 명세를 해석한다
 *
 * @d:         대상 도메인
 * @ctrlr:     컨트롤러 노드
 * @intspec:   명세 배열
 * @intsize:   배열 크기
 * @out_hwirq: 결과 하드웨어 번호 (출력)
 * @out_type:  결과 트리거 방식 (출력)
 * @return:    0 성공, -EINVAL 셀이 하나도 없음
 *
 * 셀 개수에 따라 트리거를 읽을지 말지 정한다. 유연하지만 원본 주석이
 * 경고하듯 함부로 쓰면 안 된다.
 *
 * 왜 경고하는가: 이 함수는 셀이 하나여도 통과시킨다. 그런데
 * "#interrupt-cells = <2>" 로 선언한 컨트롤러에 실수로 셀 하나만 적힌
 * 디바이스 트리가 있으면, 이 함수는 그것을 조용히 받아들이고 트리거가
 * 없는 것으로 처리한다. onecell 이나 twocell 판을 쓰면 그런 실수가
 * 명세 검증 단계에서 드러난다.
 *
 * 이 파일의 irq_domain_simple_ops 가 이것을 쓰는 것에 주목: 가장
 * 관대한 기본값이라 무엇이든 받아들인다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_domain_translate() → d->ops->xlate → [이 함수]
 */
int irq_domain_xlate_onetwocell(struct irq_domain *d,
				struct device_node *ctrlr,
				const u32 *intspec, unsigned int intsize,
				unsigned long *out_hwirq, unsigned int *out_type)
{
	if (WARN_ON(intsize < 1))	/* [한국어] 셀이 하나도 없는가 */
		return -EINVAL;
	*out_hwirq = intspec[0];	/* [한국어] 첫 셀은 항상 하드웨어 번호 */
	if (intsize > 1)	/* [한국어] 둘째 셀이 있는가 */
		*out_type = intspec[1] & IRQ_TYPE_SENSE_MASK;	/* [한국어] 마스크로 자른다. 디바이스 트리에 다른 비트가 섞여 있을 수 있다 */
	else	/* [한국어] 셀이 하나뿐인 경우 */
		*out_type = IRQ_TYPE_NONE;	/* [한국어] 트리거 정보가 없다. 이 관대함이 원본 주석의 경고 대상이다 — 셀 개수 실수를 조용히 넘긴다 */
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_xlate_onetwocell);	/* [한국어] 두 형식을 모두 받는 컨트롤러와 기본 ops 가 쓴다 */

const struct irq_domain_ops irq_domain_simple_ops = {
	/* [한국어] 아무 설정도 하지 않는 도메인을 위한 기본 연산표.
	 * map 콜백이 없어 사상을 만들 때 아무 일도 하지 않고, xlate 만
	 * 있다. 컨트롤러가 실제 하드웨어 설정을 다른 경로로 하거나,
	 * 인터럽트 번호 변환만 필요한 경우에 쓴다. */
	.xlate = irq_domain_xlate_onetwocell,
	/* [한국어] 디바이스 트리 명세 해석 콜백.
	 * 설정자: 여기 정적 초기화.
	 * 읽는 자: irq_domain_translate().
	 * 값 범위: 가장 관대한 onetwocell 판. 무엇이든 받아들인다.
	 * 동기화: const 라 변경되지 않는다.
	 * map 이 없는 것에 주목 — 사상을 만들 때 드라이버가 할 일이
	 *   없다는 뜻이고, 그러면 칩과 처리기를 다른 경로로 걸어야 한다. */
};
EXPORT_SYMBOL_GPL(irq_domain_simple_ops);	/* [한국어] 설정이 필요 없는 도메인이 그대로 가져다 쓴다 */

/**
 * irq_domain_translate_onecell() - Generic translate for direct one cell
 * bindings
 * @d:		Interrupt domain involved in the translation
 * @fwspec:	The firmware interrupt specifier to translate
 * @out_hwirq:	Pointer to storage for the hardware interrupt number
 * @out_type:	Pointer to storage for the interrupt type
 */
/*
 * [한국어]
 * irq_domain_translate_onecell - 매개변수 하나짜리 명세를 해석한다 (중립 판)
 *
 * @d:         대상 도메인 (쓰지 않는다)
 * @fwspec:    펌웨어 명세
 * @out_hwirq: 결과 하드웨어 번호 (출력)
 * @out_type:  결과 트리거 방식 (출력)
 * @return:    0 성공, -EINVAL 매개변수 부족
 *
 * 위 xlate_onecell 의 펌웨어 중립 판이다. 디바이스 트리뿐 아니라
 * ACPI 명세도 다룰 수 있다.
 *
 * 두 계열이 공존하는 이유: xlate 는 디바이스 트리 시절의 인터페이스이고
 * translate 는 ACPI 를 함께 지원하려고 나중에 추가됐다. 새 계층형
 * 도메인은 translate 만 제공하고, 옛 도메인은 xlate 만 있으며,
 * irq_domain_translate() 가 둘 중 있는 것을 고른다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_domain_translate() → d->ops->translate → [이 함수]
 */
int irq_domain_translate_onecell(struct irq_domain *d,
				 struct irq_fwspec *fwspec,
				 unsigned long *out_hwirq,
				 unsigned int *out_type)
{
	if (WARN_ON(fwspec->param_count < 1))	/* [한국어] 매개변수가 하나도 없는가 */
		return -EINVAL;
	*out_hwirq = fwspec->param[0];	/* [한국어] 첫 매개변수가 그대로 하드웨어 번호 */
	*out_type = IRQ_TYPE_NONE;	/* [한국어] 매개변수 하나로는 트리거를 표현할 수 없다 */
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_translate_onecell);	/* [한국어] 계층형 도메인이 ops.translate 에 꽂는다 */

/**
 * irq_domain_translate_twocell() - Generic translate for direct two cell
 * bindings
 * @d:		Interrupt domain involved in the translation
 * @fwspec:	The firmware interrupt specifier to translate
 * @out_hwirq:	Pointer to storage for the hardware interrupt number
 * @out_type:	Pointer to storage for the interrupt type
 *
 * Device Tree IRQ specifier translation function which works with two cell
 * bindings where the cell values map directly to the hwirq number
 * and linux irq flags.
 */
/*
 * [한국어]
 * irq_domain_translate_twocell - 매개변수 두 개짜리 명세를 해석한다 (중립 판)
 *
 * @d:         대상 도메인 (쓰지 않는다)
 * @fwspec:    펌웨어 명세
 * @out_hwirq: 결과 하드웨어 번호 (출력)
 * @out_type:  결과 트리거 방식 (출력)
 * @return:    0 성공, -EINVAL 매개변수 부족
 *
 * 가장 흔한 형식의 중립 판이다. 첫 매개변수가 번호, 둘째가 트리거다.
 *
 * 트리거를 IRQ_TYPE_SENSE_MASK 로 자르는 것에 주목: 디바이스 트리의
 * 플래그 셀에는 트리거 외의 비트가 섞일 수 있다. 예를 들어 일부
 * 바인딩은 상위 비트를 다른 용도로 쓴다. 잘라 내지 않으면 그것이
 * 트리거 설정으로 해석되어 오동작한다.
 *
 * 위 xlate_twocell 이 이 함수를 부르므로, 실제 해석 논리는 여기
 * 한 곳에만 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_domain_translate() / irq_domain_xlate_twocell() → [이 함수]
 */
int irq_domain_translate_twocell(struct irq_domain *d,
				 struct irq_fwspec *fwspec,
				 unsigned long *out_hwirq,
				 unsigned int *out_type)
{
	if (WARN_ON(fwspec->param_count < 2))	/* [한국어] 매개변수가 둘 미만인가 */
		return -EINVAL;
	*out_hwirq = fwspec->param[0];	/* [한국어] 첫 매개변수가 하드웨어 번호 */
	*out_type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;	/* [한국어] 마스크로 자른다. 플래그 셀에 트리거 외의 비트가 섞이면 그것이 트리거로 해석되어 오동작한다 */
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_translate_twocell);	/* [한국어] 가장 많이 쓰이는 translate 구현 */

/**
 * irq_domain_translate_twothreecell() - Generic translate for direct two or three cell
 * bindings
 * @d:		Interrupt domain involved in the translation
 * @fwspec:	The firmware interrupt specifier to translate
 * @out_hwirq:	Pointer to storage for the hardware interrupt number
 * @out_type:	Pointer to storage for the interrupt type
 *
 * Firmware interrupt specifier translation function for two or three cell
 * specifications, where the parameter values map directly to the hardware
 * interrupt number and the type specifier.
 */
/*
 * [한국어]
 * irq_domain_translate_twothreecell - 매개변수 두 개 또는 세 개를 해석한다
 *
 * @d:         대상 도메인 (쓰지 않는다)
 * @fwspec:    펌웨어 명세
 * @out_hwirq: 결과 하드웨어 번호 (출력)
 * @out_type:  결과 트리거 방식 (출력)
 * @return:    0 성공, -EINVAL 매개변수가 2 도 3 도 아님
 *
 * 두 형식에서 번호의 위치가 다른 것이 이 함수의 요점이다. 두 개면
 * 첫 번째가 번호이고, 세 개면 두 번째가 번호다.
 *
 * 왜 그런가: 세 매개변수 형식은 첫 자리에 "인터럽트 종류" 를 둔다.
 * ARM GIC 의 <종류 번호 플래그> 가 대표적으로, 종류가 SPI 인지 PPI
 * 인지에 따라 번호의 의미가 달라진다.
 *
 * 다만 이 공용 구현은 그 종류를 무시한다 — 번호를 그대로 쓴다.
 * 종류에 따라 번호를 조정해야 하는 컨트롤러는 자기 구현을 제공한다.
 *
 * WARN 없이 -EINVAL 을 돌려주는 것에 주목: 위 함수들과 달리 여기서는
 * 경고하지 않는다. 매개변수 개수가 다양할 수 있는 형식이라, 맞지
 * 않는 것이 정상적인 경우일 수 있어서다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_domain_translate() / irq_domain_xlate_twothreecell() → [이 함수]
 */
int irq_domain_translate_twothreecell(struct irq_domain *d, struct irq_fwspec *fwspec,
				      unsigned long *out_hwirq, unsigned int *out_type)
{
	if (fwspec->param_count == 2) {	/* [한국어] 두 매개변수 형식인가 */
		*out_hwirq = fwspec->param[0];	/* [한국어] 첫 번째가 번호 */
		*out_type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;	/* [한국어] 두 번째가 트리거 */
		return 0;
	}

	if (fwspec->param_count == 3) {	/* [한국어] 세 매개변수 형식인가 */
		*out_hwirq = fwspec->param[1];	/* [한국어] 두 번째가 번호다. 첫 자리는 인터럽트 종류(SPI/PPI 등)인데 이 공용 구현은 무시한다 */
		*out_type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;	/* [한국어] 세 번째가 트리거 */
		return 0;
	}

	return -EINVAL;	/* [한국어] 경고하지 않는다. 매개변수 개수가 다양할 수 있는 형식이라 맞지 않는 것이 정상일 수 있다 */
}
EXPORT_SYMBOL_GPL(irq_domain_translate_twothreecell);	/* [한국어] GIC 계열 컨트롤러가 쓴다 */

/*
 * [한국어]
 * irq_domain_alloc_descs - 도메인용 서술자를 확보한다
 *
 * @virq:     특정 번호를 원하면 그 번호, 아무 데나면 음수
 * @cnt:      개수
 * @hwirq:    하드웨어 번호 (검색 시작점의 힌트로 쓴다)
 * @node:     NUMA 노드
 * @affinity: 초기 친화도
 * @return:   배정된 첫 번호, 실패 시 음수
 *
 * 서술자 할당에 도메인 고유의 힌트를 얹는 함수다.
 *
 * 힌트가 무엇을 위한 것인가: 하드웨어 번호와 리눅스 번호가 대충
 * 비슷하게 배치되면 사람이 읽기 쉽다. /proc/interrupts 에서 42 번
 * 인터럽트가 하드웨어 42 번 근처이면 대응을 짐작할 수 있다. 그래서
 * hwirq % nr_irqs 를 검색 시작점으로 삼는다.
 *
 * 0 을 1 로 올리는 이유: 인터럽트 0 은 예약되어 배정되지 않는다.
 * 힌트가 0 이면 검색이 그 번호부터 시작해 헛되이 실패한다.
 *
 * 두 번째 시도가 있는 이유: 힌트 위치부터 끝까지 빈 곳이 없을 수
 * 있다. 그러면 1 번부터 다시 찾는다. 힌트는 선호일 뿐 강제가 아니다.
 *
 * THIS_MODULE 을 넘기는 것에 주목: 서술자의 소유 모듈이 irqdomain
 * 코드가 된다. 실제 소유자는 컨트롤러 드라이버이지만, 이 경로로는
 * 그것을 알 수 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_create_mapping_affinity_locked() / irq_domain_alloc_irqs_locked()
 *   → [이 함수] → __irq_alloc_descs()
 */
int irq_domain_alloc_descs(int virq, unsigned int cnt, irq_hw_number_t hwirq,
			   int node, const struct irq_affinity_desc *affinity)
{
	unsigned int hint;	/* [한국어] 검색 시작점 */

	if (virq >= 0) {	/* [한국어] 특정 번호를 요구하는가 */
		virq = __irq_alloc_descs(virq, virq, cnt, node, THIS_MODULE,	/* [한국어] 첫 두 인자가 같으면 "정확히 이 번호" 라는 뜻이다 */
					 affinity);
	} else {	/* [한국어] 아무 번호나 좋은 경우 */
		hint = hwirq % irq_get_nr_irqs();	/* [한국어] 하드웨어 번호 근처를 선호한다. /proc/interrupts 에서 두 번호의 대응을 짐작하기 쉬워진다 */
		if (hint == 0)	/* [한국어] 힌트가 0 인가 */
			hint++;	/* [한국어] 인터럽트 0 은 예약되어 배정되지 않는다. 그대로 두면 검색이 헛되이 실패한다 */
		virq = __irq_alloc_descs(-1, hint, cnt, node, THIS_MODULE,	/* [한국어] 힌트 위치부터 검색. -1 은 "아무 번호나" */
					 affinity);
		if (virq <= 0 && hint > 1) {	/* [한국어] 힌트 위쪽에 빈 곳이 없었는가 */
			virq = __irq_alloc_descs(-1, 1, cnt, node, THIS_MODULE,	/* [한국어] 1 번부터 다시 찾는다. 힌트는 선호일 뿐 강제가 아니다 */
						 affinity);
		}
	}

	return virq;	/* [한국어] 배정된 첫 번호 또는 음수 오류 */
}

/**
 * irq_domain_reset_irq_data - Clear hwirq, chip and chip_data in @irq_data
 * @irq_data:	The pointer to irq_data
 */
/*
 * [한국어]
 * irq_domain_reset_irq_data - irq_data 의 도메인 관련 필드를 지운다
 *
 * @irq_data: 대상 irq_data
 * @return:   없음
 *
 * 계층의 한 층을 해제할 때 그 층의 irq_data 를 초기 상태로 되돌린다.
 * 세 필드가 전부다 — 하드웨어 번호, 칩, 칩 데이터.
 *
 * 칩을 NULL 이 아니라 no_irq_chip 으로 두는 것에 주목: 코어 곳곳이
 * chip 포인터를 검사 없이 역참조한다. NULL 로 두면 해제 뒤에 인터럽트가
 * 올라올 때 터진다.
 *
 * domain 필드를 지우지 않는 것도 의도적이다. 이 함수는 계층의 중간
 * 층에도 불리는데, 그 층의 irq_data 는 곧 해제될 것이라 도메인
 * 연결을 지울 이유가 없다. 가장 바깥 층(서술자에 박힌 것)의 domain 은
 * irq_domain_free_irq_data() 가 따로 지운다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 해제 경로.
 *
 * 호출 체인:
 *   irq_domain_free_irqs_common() / irqchip 드라이버의 free 콜백 →
 *   [이 함수]
 */
void irq_domain_reset_irq_data(struct irq_data *irq_data)
{
	irq_data->hwirq = 0;	/* [한국어] 하드웨어 번호를 지운다 */
	irq_data->chip = &no_irq_chip;	/* [한국어] NULL 이 아니라 더미 칩. 코어가 chip 포인터를 검사 없이 역참조하므로 NULL 로 두면 해제 뒤 인터럽트가 올 때 터진다 */
	irq_data->chip_data = NULL;	/* [한국어] 칩 드라이버의 사설 데이터. 해제된 것을 가리키지 않게 한다 */
}
EXPORT_SYMBOL_GPL(irq_domain_reset_irq_data);	/* [한국어] irqchip 드라이버의 free 콜백이 부른다 */

#ifdef	CONFIG_IRQ_DOMAIN_HIERARCHY	/* [한국어] 여기부터 파일의 큰 부분이 계층형 도메인 전용이다 */
/*
 * [한국어]
 * irq_domain_insert_irq - 계층의 모든 층을 역방향 맵에 등록한다
 *
 * @virq: 대상 리눅스 인터럽트 번호
 * @return: 없음
 *
 * 계층형 할당의 마지막 단계다. 각 층의 irq_data 를 그 층의 역방향
 * 맵에 넣는다.
 *
 * 왜 층마다 등록하는가: 각 층이 자기 하드웨어 번호 공간을 갖고, 그
 * 층의 드라이버가 자기 번호로 조회할 수 있어야 한다. 예를 들어 GIC
 * ITS 는 자기 이벤트 ID 로 조회하고, 그 위의 MSI 도메인은 MSI
 * 인덱스로 조회한다.
 *
 * parent_data 를 따라 안쪽으로 내려가는 것에 주목: 가장 바깥 층
 * (서술자에 박힌 irq_data)부터 시작해 계층 끝까지 간다.
 *
 * 마지막의 NOREQUEST 지우기가 "이제 요청해도 된다" 는 신호다. 계층
 * 전체가 준비된 뒤에야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   irq_domain_alloc_irqs_locked() → [이 함수] → irq_domain_set_mapping()
 */
static void irq_domain_insert_irq(int virq)
{
	struct irq_data *data;	/* [한국어] 계층을 훑을 커서 */

	for (data = irq_get_irq_data(virq); data; data = data->parent_data) {	/* [한국어] 가장 바깥 층부터 계층 끝까지 */
		struct irq_domain *domain = data->domain;	/* [한국어] 이 층의 도메인 */

		domain->mapcount++;	/* [한국어] 층마다 사상 수를 센다 */
		irq_domain_set_mapping(domain, data->hwirq, data);	/* [한국어] 각 층이 자기 번호 공간으로 조회할 수 있어야 한다. GIC ITS 는 이벤트 ID 로, 그 위 MSI 도메인은 MSI 인덱스로 조회한다 */
	}

	irq_clear_status_flags(virq, IRQ_NOREQUEST);	/* [한국어] "이제 요청해도 된다". 계층 전체가 준비된 뒤에야 한다 */
}

/*
 * [한국어]
 * irq_domain_remove_irq - 계층의 모든 층을 역방향 맵에서 뺀다
 *
 * @virq: 대상 리눅스 인터럽트 번호
 * @return: 없음
 *
 * 위 insert 의 반대인데, 앞에 세 단계가 더 있다. 그 순서가 이 함수의
 * 핵심이다.
 *
 *   1. NOREQUEST — 새 요청을 막는다.
 *   2. 칩과 처리기 제거 — 그 안에서 선이 마스크된다.
 *   3. synchronize_irq — 진행 중인 처리를 기다린다.
 *   4. 메모리 장벽
 *   5. 층마다 맵에서 제거
 *
 * 3 이 없으면 다른 CPU 가 처리 중인 인터럽트의 맵을 지워, 그 CPU 가
 * 다음 조회에서 NULL 을 보게 된다. 처리 중인 인터럽트가 갑자기
 * 사라지는 셈이다.
 *
 * smp_mb() 는 위 세 단계와 아래 맵 제거 사이의 순서를 보장한다.
 * 다른 CPU 가 맵 제거만 먼저 보고 하드웨어가 아직 살아 있다고
 * 판단하면 안 된다.
 *
 * hwirq 를 지역 변수에 챙기는 것에 주목: 아래 clear_mapping 이
 * 그 값을 쓰는데, data 를 따라가는 동안 안전하게 읽어 둔다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   irq_domain_free_irqs() → [이 함수] → irq_domain_clear_mapping()
 */
static void irq_domain_remove_irq(int virq)
{
	struct irq_data *data;	/* [한국어] 계층을 훑을 커서 */

	irq_set_status_flags(virq, IRQ_NOREQUEST);	/* [한국어] 정리 중에 누가 request_irq 하지 못하게 막는다 */
	irq_set_chip_and_handler(virq, NULL, NULL);	/* [한국어] 처리기를 떼면 그 안에서 선이 마스크된다 */
	synchronize_irq(virq);	/* [한국어] 다른 CPU 가 처리 중일 수 있다. 기다리지 않으면 처리 중인 인터럽트의 맵이 사라져 다음 조회가 NULL 을 본다 */
	smp_mb();	/* [한국어] 위 하드웨어 조작과 아래 맵 제거 사이의 순서를 보장한다 */

	for (data = irq_get_irq_data(virq); data; data = data->parent_data) {	/* [한국어] 계층의 모든 층에 대해 */
		struct irq_domain *domain = data->domain;	/* [한국어] 이 층의 도메인 */
		irq_hw_number_t hwirq = data->hwirq;	/* [한국어] 지울 번호를 미리 챙긴다 */

		domain->mapcount--;	/* [한국어] 사상 수를 줄인다 */
		irq_domain_clear_mapping(domain, hwirq);	/* [한국어] 이 층의 맵에서 뺀다 */
	}
}

/*
 * [한국어]
 * irq_domain_insert_irq_data - 계층에 새 층의 irq_data 를 끼워 넣는다
 *
 * @domain: 새 층의 도메인
 * @child:  이 층의 바깥 층 (즉 자식)
 * @return: 만든 irq_data, 실패 시 NULL
 *
 * 계층형 자료구조를 만드는 기본 연산이다. 자식의 parent_data 에
 * 새 irq_data 를 매단다.
 *
 * 세 필드만 채우는 것에 주목: 번호, 공통 데이터, 도메인이다.
 * hwirq 와 chip 은 나중에 도메인의 alloc 콜백이 채운다.
 *
 * common 을 공유하는 것이 계층형 설계의 중요한 부분이다. 친화도나
 * 노드 같은 정보는 계층 전체에 하나뿐이어야 한다 — 층마다 다른
 * 친화도를 갖는 것은 뜻이 통하지 않기 때문이다. 그래서 모든 층의
 * irq_data 가 서술자 안의 irq_common_data 하나를 가리킨다.
 *
 * 자식과 같은 NUMA 노드에 할당하는 것도 의도적이다. 인터럽트 처리
 * 경로가 계층을 따라 내려가므로 지역성이 중요하다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_domain_alloc_irq_data() → [이 함수]
 */
static struct irq_data *irq_domain_insert_irq_data(struct irq_domain *domain,
						   struct irq_data *child)
{
	struct irq_data *irq_data;	/* [한국어] 새 층의 irq_data */

	irq_data = kzalloc_node(sizeof(*irq_data), GFP_KERNEL,	/* [한국어] 0 초기화. hwirq 와 chip 은 도메인의 alloc 콜백이 나중에 채운다 */
				irq_data_get_node(child));	/* [한국어] 자식과 같은 노드. 인터럽트 처리가 계층을 따라 내려가므로 지역성이 중요하다 */
	if (irq_data) {	/* [한국어] 할당 성공 */
		child->parent_data = irq_data;	/* [한국어] 자식의 안쪽에 매단다. 이것이 계층을 만드는 한 줄이다 */
		irq_data->irq = child->irq;	/* [한국어] 리눅스 번호는 계층 전체가 같다 */
		irq_data->common = child->common;	/* [한국어] 친화도·노드 같은 정보는 계층에 하나뿐이어야 한다. 층마다 다른 친화도는 뜻이 통하지 않는다 */
		irq_data->domain = domain;	/* [한국어] 이 층의 도메인 */
	}

	return irq_data;	/* [한국어] 실패하면 NULL */
}

/*
 * [한국어]
 * __irq_domain_free_hierarchy - 계층 사슬을 끝까지 따라가며 해제한다
 *
 * @irq_data: 해제를 시작할 층
 * @return:   없음
 *
 * parent_data 사슬을 따라가며 각 층을 kfree 한다. 재귀가 아니라
 * 반복문인 것에 주목 — 계층이 깊어져도 스택이 자라지 않는다.
 *
 * 다음 포인터를 먼저 읽고 해제하는 순서가 필수다. 해제한 뒤에
 * parent_data 를 읽으면 해제된 메모리를 읽는다.
 *
 * 가장 바깥 층에는 부르면 안 된다: 그것은 서술자 안에 박혀 있어
 * 따로 할당된 것이 아니다. 호출자들이 parent_data 부터 넘기는 이유다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_domain_free_irq_data() / irq_domain_trim_hierarchy() → [이 함수]
 */
static void __irq_domain_free_hierarchy(struct irq_data *irq_data)
{
	struct irq_data *tmp;	/* [한국어] 해제할 층을 잠시 담는다 */

	while (irq_data) {	/* [한국어] 재귀가 아니라 반복문이다. 계층이 깊어져도 스택이 자라지 않는다 */
		tmp = irq_data;	/* [한국어] 해제할 대상 */
		irq_data = irq_data->parent_data;	/* [한국어] 다음을 먼저 읽는다. 해제한 뒤 읽으면 해제된 메모리를 읽는다 */
		kfree(tmp);	/* [한국어] 이 층 해제 */
	}
}

/*
 * [한국어]
 * irq_domain_free_irq_data - 여러 인터럽트의 계층 자료구조를 해제한다
 *
 * @virq:    첫 리눅스 인터럽트 번호
 * @nr_irqs: 개수
 * @return:  없음
 *
 * 각 인터럽트의 계층 사슬을 통째로 해제한다. 가장 바깥 층은
 * 서술자에 박혀 있어 해제하지 않고 필드만 지운다.
 *
 * parent_data 를 먼저 NULL 로 만드는 순서가 중요하다. 그러지 않으면
 * 해제된 사슬을 가리키는 포인터가 서술자에 남는다.
 *
 * domain 도 지우는 것에 주목: 위 irq_domain_reset_irq_data() 는
 * 그것을 건드리지 않지만, 여기서는 계층 자체가 사라지므로 가장
 * 바깥 층의 도메인 연결도 끊어야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_domain_alloc_irq_data() 의 실패 경로 / irq_domain_free_irqs() →
 *   [이 함수] → __irq_domain_free_hierarchy()
 */
static void irq_domain_free_irq_data(unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *irq_data, *tmp;	/* [한국어] 가장 바깥 층과 그 안쪽 사슬 */
	int i;	/* [한국어] 순회용 */

	for (i = 0; i < nr_irqs; i++) {	/* [한국어] 각 인터럽트에 대해 */
		irq_data = irq_get_irq_data(virq + i);	/* [한국어] 서술자에 박힌 가장 바깥 층 */
		tmp = irq_data->parent_data;	/* [한국어] 해제할 안쪽 사슬을 챙긴다 */
		irq_data->parent_data = NULL;	/* [한국어] 먼저 끊는다. 그러지 않으면 해제된 사슬을 가리키는 포인터가 서술자에 남는다 */
		irq_data->domain = NULL;	/* [한국어] 계층 자체가 사라지므로 가장 바깥 층의 도메인 연결도 끊는다 */

		__irq_domain_free_hierarchy(tmp);	/* [한국어] 안쪽 사슬을 끝까지 해제한다. 가장 바깥 층은 서술자에 박혀 있어 해제하지 않는다 */
	}
}

/**
 * irq_domain_disconnect_hierarchy - Mark the first unused level of a hierarchy
 * @domain:	IRQ domain from which the hierarchy is to be disconnected
 * @virq:	IRQ number where the hierarchy is to be trimmed
 *
 * Marks the @virq level belonging to @domain as disconnected.
 * Returns -EINVAL if @virq doesn't have a valid irq_data pointing
 * to @domain.
 *
 * Its only use is to be able to trim levels of hierarchy that do not
 * have any real meaning for this interrupt, and that the driver marks
 * as such from its .alloc() callback.
 */
/*
 * [한국어]
 * irq_domain_disconnect_hierarchy - 계층의 이 층부터 잘라 내라고 표시한다
 *
 * @domain: 잘라 내기 시작할 층의 도메인
 * @virq:   대상 리눅스 인터럽트 번호
 * @return: 0 표시 완료, -EINVAL 그 도메인의 층이 없음
 *
 * 어떤 상황을 위한 것인가: 계층이 미리 정해져 있는데 특정 인터럽트에
 * 대해서는 안쪽 층 몇 개가 의미가 없는 경우다. 예를 들어 같은
 * 컨트롤러가 어떤 인터럽트는 리매핑을 거치고 어떤 것은 직접 CPU 로
 * 보낼 때, 후자에는 리매핑 층이 불필요하다.
 *
 * 표시 방법이 독특하다. chip 포인터에 ERR_PTR(-ENOTCONN) 을 넣는다.
 * 유효한 포인터도 NULL 도 아닌 제3의 값이라, 아래
 * irq_domain_trim_hierarchy() 가 그것을 표식으로 알아본다.
 *
 * 왜 chip 을 쓰는가: 별도의 플래그 필드를 만들면 모든 irq_data 가
 * 그 공간을 차지한다. 잘라 내는 것은 드문 일이라 기존 필드를 재활용해
 * 공간을 아꼈다.
 *
 * 드라이버가 자기 alloc 콜백 안에서 부른다는 것이 원본 주석의 조건이다.
 * 그 시점에는 아직 계층이 완성되기 전이라 안전하게 표시할 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 도메인 alloc 콜백 안.
 *
 * 호출 체인:
 *   irqchip 드라이버의 alloc 콜백 → [이 함수]
 */
int irq_domain_disconnect_hierarchy(struct irq_domain *domain,
				    unsigned int virq)
{
	struct irq_data *irqd;	/* [한국어] 표시할 층 */

	irqd = irq_domain_get_irq_data(domain, virq);	/* [한국어] 그 도메인에 해당하는 층을 찾는다 */
	if (!irqd)	/* [한국어] 없는가 — 이 인터럽트는 그 도메인을 거치지 않는다 */
		return -EINVAL;

	irqd->chip = ERR_PTR(-ENOTCONN);	/* [한국어] 유효한 포인터도 NULL 도 아닌 제3의 값을 표식으로 쓴다. 별도 플래그 필드를 두면 모든 irq_data 가 그 공간을 차지하므로 기존 필드를 재활용했다 */
	return 0;	/* [한국어] 셀 하나가 그대로 하드웨어 번호였다 */
}
EXPORT_SYMBOL_GPL(irq_domain_disconnect_hierarchy);	/* [한국어] irqchip 드라이버가 alloc 콜백 안에서 부른다 */

/*
 * [한국어]
 * irq_domain_trim_hierarchy - 표시된 지점부터 계층을 잘라 낸다
 *
 * @virq:   대상 리눅스 인터럽트 번호
 * @return: 0 성공(잘라 낼 것이 없는 경우 포함), -EINVAL 사슬이 이상함
 *
 * 위 disconnect 가 남긴 표식을 찾아 그 안쪽을 통째로 해제한다.
 *
 * 검증이 함수의 절반을 차지한다. 정상적인 사슬은 이런 모양이어야
 * 한다 — 바깥부터 유효한 칩들이 이어지다가, 표식이 하나 나오고,
 * 그 뒤로는 칩이 없는 층들만 남는다. 두 검사가 그것을 강제한다.
 *
 *   - 표식 뒤에 유효한 칩이 있으면: 계층이 잘못 구성됐다. 잘라 낼
 *     부분에 실제로 쓰이는 층이 섞여 있다는 뜻이다.
 *   - 표식 앞에 칩 없는 층이 있으면: 드라이버가 그 층을 설정하지
 *     않았다. 표식도 없이 비어 있는 것은 버그다.
 *
 * ERR_PTR 중에서 -ENOTCONN 만 표식으로 인정하는 것에 주목: 다른
 * 오류 값이 들어 있으면 드라이버가 실수로 오류 포인터를 남긴 것이다.
 *
 * 자르는 방식은 세 줄이다. 표식이 있는 층의 parent_data 를 끊고,
 * 그 아래를 통째로 해제한다. 표식 층 자체는 남는데, 그 층의 chip 은
 * 여전히 ERR_PTR 이다 — 도메인의 alloc 콜백이 그 뒤에 제대로 채워야
 * 한다는 뜻이 아니라, 이 층이 계층의 끝이 된다는 뜻이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   irq_domain_alloc_irqs_locked() → [이 함수] →
 *   __irq_domain_free_hierarchy()
 */
static int irq_domain_trim_hierarchy(unsigned int virq)
{
	struct irq_data *tail, *irqd, *irq_data;	/* [한국어] 표식 층, 순회 커서, 그 앞 층 */

	irq_data = irq_get_irq_data(virq);	/* [한국어] 가장 바깥 층 */
	tail = NULL;	/* [한국어] 아직 표식을 찾지 못했다 */

	/* The first entry must have a valid irqchip */
	if (IS_ERR_OR_NULL(irq_data->chip))	/* [한국어] (위 영어 주석) 가장 바깥 층은 반드시 유효한 칩을 가져야 한다. 표식이 여기 있으면 계층 전체가 무의미해진다 */
		return -EINVAL;

	/*
	 * Validate that the irq_data chain is sane in the presence of
	 * a hierarchy trimming marker.
	 */
	for (irqd = irq_data->parent_data; irqd; irq_data = irqd, irqd = irqd->parent_data) {	/* [한국어] (위 영어 주석) irq_data 가 한 칸 뒤를 따라간다 — 표식을 찾으면 그 앞 층을 기억해야 하기 때문이다 */
		/* Can't have a valid irqchip after a trim marker */
		if (irqd->chip && tail)	/* [한국어] (위 영어 주석) 표식 뒤에 유효한 칩이 있는가 — 잘라 낼 부분에 실제로 쓰이는 층이 섞여 있다 */
			return -EINVAL;

		/* Can't have an empty irqchip before a trim marker */
		if (!irqd->chip && !tail)	/* [한국어] (위 영어 주석) 표식 앞에 칩 없는 층이 있는가 — 드라이버가 그 층을 설정하지 않은 버그다 */
			return -EINVAL;

		if (IS_ERR(irqd->chip)) {	/* [한국어] 오류 포인터인가 */
			/* Only -ENOTCONN is a valid trim marker */
			if (PTR_ERR(irqd->chip) != -ENOTCONN)	/* [한국어] (위 영어 주석) 다른 오류 값이면 드라이버가 실수로 남긴 것이다 */
				return -EINVAL;

			tail = irq_data;	/* [한국어] 표식 층의 *앞* 층을 기억한다. 자를 때 그 층의 parent_data 를 끊어야 하기 때문이다 */
		}
	}

	/* No trim marker, nothing to do */
	if (!tail)	/* [한국어] (위 영어 주석) 표식이 없었는가 — 대부분의 경우다 */
		return 0;

	pr_info("IRQ%d: trimming hierarchy from %s\n",	/* [한국어] 계층이 잘렸다는 것은 드문 일이라 정보 수준으로 남긴다 */
		virq, tail->parent_data->domain->name);

	/* Sever the inner part of the hierarchy...  */
	irqd = tail;	/* [한국어] (위 영어 주석) 표식 층의 앞 층 */
	tail = tail->parent_data;	/* [한국어] 잘라 낼 부분의 시작 (표식 층 자체) */
	irqd->parent_data = NULL;	/* [한국어] 사슬을 끊는다. 이 층이 이제 계층의 끝이다 */
	__irq_domain_free_hierarchy(tail);	/* [한국어] 표식 층부터 끝까지 통째로 해제한다 */

	return 0;	/* [한국어] 셀 개수에 따라 트리거를 읽거나 비워 두었다 */
}

/*
 * [한국어]
 * irq_domain_alloc_irq_data - 계층 전체의 irq_data 사슬을 만든다
 *
 * @domain:  가장 바깥 도메인
 * @virq:    첫 리눅스 인터럽트 번호
 * @nr_irqs: 개수
 * @return:  0 성공, -ENOMEM 할당 실패
 *
 * 계층형 할당의 자료구조 준비 단계다. 각 인터럽트에 대해 도메인
 * 계층을 따라 내려가며 irq_data 를 하나씩 매단다.
 *
 * 가장 바깥 층은 만들지 않는다 — 원본 주석대로 그것은 서술자 안에
 * 박혀 있다. 그래서 도메인만 설정하고, 부모부터 새로 만든다.
 *
 * 실패 시 되돌리는 범위가 i + 1 인 것에 주목: 실패한 i 번 인터럽트도
 * 이미 일부 층을 만들었으므로 포함해야 한다. i 로 두면 그 인터럽트의
 * 층들이 샌다.
 *
 * 안쪽 루프의 irq_data 재대입이 사슬을 만든다. 새로 만든 층이 다음
 * 반복의 자식이 되어, 계층이 바깥에서 안쪽으로 이어진다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   irq_domain_alloc_irqs_locked() → [이 함수] →
 *   irq_domain_insert_irq_data()
 */
static int irq_domain_alloc_irq_data(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *irq_data;	/* [한국어] 사슬을 만들어 갈 커서 */
	struct irq_domain *parent;	/* [한국어] 도메인 계층을 훑을 커서 */
	int i;	/* [한국어] 인터럽트 순회용 */

	/* The outermost irq_data is embedded in struct irq_desc */
	for (i = 0; i < nr_irqs; i++) {	/* [한국어] (위 영어 주석) 각 인터럽트에 대해 */
		irq_data = irq_get_irq_data(virq + i);	/* [한국어] 서술자에 박힌 가장 바깥 층. 새로 만들지 않는다 */
		irq_data->domain = domain;	/* [한국어] 도메인만 설정한다 */

		for (parent = domain->parent; parent; parent = parent->parent) {	/* [한국어] 도메인 계층을 따라 안쪽으로 */
			irq_data = irq_domain_insert_irq_data(parent, irq_data);	/* [한국어] 새로 만든 층이 다음 반복의 자식이 된다. 이 재대입이 사슬을 만든다 */
			if (!irq_data) {	/* [한국어] 메모리 부족 */
				irq_domain_free_irq_data(virq, i + 1);	/* [한국어] i + 1 인 것에 주목 — 실패한 이 인터럽트도 이미 일부 층을 만들었으므로 포함해야 한다 */
				return -ENOMEM;	/* [한국어] 계층 사슬을 만들 메모리가 부족하다. 위에서 이 인터럽트까지 포함해 되돌렸다 */
			}
		}
	}

	return 0;	/* [한국어] 계층 자료구조가 준비됐다. 실제 하드웨어 자원 배정은 아직이다 */
}

/**
 * irq_domain_get_irq_data - Get irq_data associated with @virq and @domain
 * @domain:	domain to match
 * @virq:	IRQ number to get irq_data
 */
/*
 * [한국어]
 * irq_domain_get_irq_data - 계층에서 특정 도메인의 층을 찾는다
 *
 * @domain: 찾을 도메인
 * @virq:   리눅스 인터럽트 번호
 * @return: 그 도메인에 해당하는 irq_data, 없으면 NULL
 *
 * 계층형 코드에서 가장 많이 쓰이는 조회 함수다. 드라이버가 자기
 * 층의 irq_data 를 얻으려 할 때 부른다.
 *
 * 왜 검색이 필요한가: 콜백은 대개 도메인과 인터럽트 번호만 받는다.
 * 그런데 그 번호의 irq_data 는 계층마다 하나씩 있어, 어느 것이 자기
 * 것인지 알려면 도메인 포인터로 찾아야 한다.
 *
 * 선형 검색인 것이 문제가 되지 않는 이유: 계층 깊이가 대개 2~4 이다.
 * 가장 깊은 경우도 PCI MSI → 리매핑 → 벡터 정도다.
 *
 * 실행 컨텍스트: 제약 없음. 락을 잡지 않는데, 계층 구조가 만들어진
 * 뒤에는 바뀌지 않기 때문이다 (push/pop 은 예외이고 그쪽은 락을 잡는다).
 *
 * 호출 체인:
 *   irqchip 드라이버의 콜백 / irq_domain_set_hwirq_and_chip() 등 →
 *   [이 함수]
 */
struct irq_data *irq_domain_get_irq_data(struct irq_domain *domain,
					 unsigned int virq)
{
	struct irq_data *irq_data;	/* [한국어] 순회 커서 */

	for (irq_data = irq_get_irq_data(virq); irq_data;	/* [한국어] 가장 바깥 층부터 */
	     irq_data = irq_data->parent_data)	/* [한국어] 안쪽으로. 계층 깊이가 대개 2~4 라 선형 검색이 문제되지 않는다 */
		if (irq_data->domain == domain)	/* [한국어] 이 층이 찾는 도메인의 것인가 */
			return irq_data;

	return NULL;	/* [한국어] 이 인터럽트는 그 도메인을 거치지 않는다 */
}
EXPORT_SYMBOL_GPL(irq_domain_get_irq_data);	/* [한국어] 계층형 irqchip 드라이버가 가장 많이 부르는 함수 */

/**
 * irq_domain_set_hwirq_and_chip - Set hwirq and irqchip of @virq at @domain
 * @domain:	Interrupt domain to match
 * @virq:	IRQ number
 * @hwirq:	The hwirq number
 * @chip:	The associated interrupt chip
 * @chip_data:	The associated chip data
 */
/*
 * [한국어]
 * irq_domain_set_hwirq_and_chip - 계층의 한 층에 번호와 칩을 설정한다
 *
 * @domain:    설정할 층의 도메인
 * @virq:      리눅스 인터럽트 번호
 * @hwirq:     이 층에서의 하드웨어 번호
 * @chip:      이 층의 칩
 * @chip_data: 그 칩의 사설 데이터
 * @return:    0 성공, -ENOENT 그 도메인의 층이 없음
 *
 * 계층형 드라이버가 alloc 콜백 안에서 자기 층을 채우는 표준 방법이다.
 * 세 필드를 한 번에 설정한다.
 *
 * 층마다 하드웨어 번호가 다르다는 것이 계층형의 핵심이다. PCI MSI
 * 도메인에서는 MSI 인덱스이고, 그 아래 리매핑 도메인에서는 리매핑
 * 테이블 인덱스이며, 벡터 도메인에서는 CPU 벡터 번호다. 같은
 * 인터럽트인데 층마다 다른 번호로 불린다.
 *
 * chip 이 NULL 이면 no_irq_chip 을 넣는 것에 주목: 이 파일의 다른
 * 곳과 같은 이유다. 코어가 검사 없이 역참조한다.
 *
 * const 를 캐스팅으로 벗기는 것도 chip.c 의 irq_set_chip 과 같은
 * 이유다 — 드라이버가 상수 테이블을 넘길 수 있게 인자는 const 로
 * 받지만 필드는 그렇지 않다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 도메인 alloc 콜백 안.
 *
 * 호출 체인:
 *   irqchip 드라이버의 alloc 콜백 / msi_domain_ops_init() → [이 함수]
 */
int irq_domain_set_hwirq_and_chip(struct irq_domain *domain, unsigned int virq,
				  irq_hw_number_t hwirq,
				  const struct irq_chip *chip,
				  void *chip_data)
{
	struct irq_data *irq_data = irq_domain_get_irq_data(domain, virq);	/* [한국어] 이 도메인에 해당하는 층 */

	if (!irq_data)	/* [한국어] 그 층이 없는가 */
		return -ENOENT;	/* [한국어] 계층 구성이 잘못됐거나 이미 잘려 나간 층이다 */

	irq_data->hwirq = hwirq;	/* [한국어] 이 층에서의 번호. 층마다 다르다 — MSI 인덱스, 리매핑 테이블 인덱스, CPU 벡터 번호 식으로 */
	irq_data->chip = (struct irq_chip *)(chip ? chip : &no_irq_chip);	/* [한국어] NULL 이면 더미 칩. 코어가 검사 없이 역참조하기 때문이다. const 를 벗기는 것은 드라이버가 상수 테이블을 넘길 수 있게 하려는 것 */
	irq_data->chip_data = chip_data;	/* [한국어] 이 층 칩의 사설 데이터 */

	return 0;	/* [한국어] 두 매개변수를 번호와 트리거로 해석했다 */
}
EXPORT_SYMBOL_GPL(irq_domain_set_hwirq_and_chip);	/* [한국어] 계층형 드라이버가 alloc 콜백에서 부른다 */

/**
 * irq_domain_set_info - Set the complete data for a @virq in @domain
 * @domain:		Interrupt domain to match
 * @virq:		IRQ number
 * @hwirq:		The hardware interrupt number
 * @chip:		The associated interrupt chip
 * @chip_data:		The associated interrupt chip data
 * @handler:		The interrupt flow handler
 * @handler_data:	The interrupt flow handler data
 * @handler_name:	The interrupt handler name
 */
/*
 * [한국어]
 * irq_domain_set_info - 한 층의 모든 정보를 한 번에 설정한다 (계층형 판)
 *
 * @domain:       대상 층의 도메인
 * @virq:         리눅스 인터럽트 번호
 * @hwirq:        이 층에서의 하드웨어 번호
 * @chip:         이 층의 칩
 * @chip_data:    칩 사설 데이터
 * @handler:      흐름 처리기
 * @handler_data: 처리기 사설 데이터
 * @handler_name: 표시 이름
 * @return:       없음
 *
 * 위 set_hwirq_and_chip 에 흐름 처리기 설정을 더한 것이다. 가장
 * 바깥 층을 설정할 때 쓴다 — 흐름 처리기는 계층 전체에 하나뿐이라
 * 서술자에 직접 붙기 때문이다.
 *
 * 그래서 이 함수는 층별 설정(hwirq, chip)과 계층 공통 설정
 * (handler)을 섞어 다룬다. 안쪽 층을 설정할 때 이것을 쓰면 처리기가
 * 덮여 잘못된다 — 그때는 set_hwirq_and_chip 만 써야 한다.
 *
 * EXPORT_SYMBOL 이 GPL 판이 아닌 것에 주목: 이 파일의 다른 함수들과
 * 다르다. 역사적 이유로 남은 것이다.
 *
 * 아래 비계층형 판과 같은 이름인데 구현이 다르다. 비계층형에서는
 * 층이 하나뿐이라 chip 설정도 서술자에 직접 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 도메인 alloc 콜백 안.
 *
 * 호출 체인:
 *   irqchip 드라이버의 alloc 콜백 / irq_map_generic_chip() → [이 함수]
 */
void irq_domain_set_info(struct irq_domain *domain, unsigned int virq,
			 irq_hw_number_t hwirq, const struct irq_chip *chip,
			 void *chip_data, irq_flow_handler_t handler,
			 void *handler_data, const char *handler_name)
{
	irq_domain_set_hwirq_and_chip(domain, virq, hwirq, chip, chip_data);	/* [한국어] 층별 설정 */
	__irq_set_handler(virq, handler, 0, handler_name);	/* [한국어] 계층 공통 설정. 흐름 처리기는 계층 전체에 하나뿐이라 서술자에 직접 붙는다 — 안쪽 층에 이 함수를 쓰면 처리기가 덮인다 */
	irq_set_handler_data(virq, handler_data);	/* [한국어] 처리기의 사설 데이터. 이것도 계층 공통이다 */
}
EXPORT_SYMBOL(irq_domain_set_info);	/* [한국어] GPL 판이 아닌 것은 역사적 이유다 */

/**
 * irq_domain_free_irqs_common - Clear irq_data and free the parent
 * @domain:	Interrupt domain to match
 * @virq:	IRQ number to start with
 * @nr_irqs:	The number of irqs to free
 */
/*
 * [한국어]
 * irq_domain_free_irqs_common - 이 층을 지우고 부모에게 해제를 넘긴다
 *
 * @domain:  대상 층의 도메인
 * @virq:    첫 리눅스 인터럽트 번호
 * @nr_irqs: 개수
 * @return:  없음
 *
 * 대부분의 계층형 드라이버가 자기 free 콜백에 그대로 꽂는 함수다.
 * 특별히 반납할 자원이 없는 층이 쓴다.
 *
 * 두 단계다. 자기 층의 irq_data 를 초기 상태로 되돌리고, 부모에게
 * 해제를 넘긴다. 그 부모가 다시 자기 free 콜백에서 이 함수를 불러
 * 계층 끝까지 이어진다.
 *
 * irq_data 가 없어도 넘어가는 것에 주목: 계층이 잘렸거나 부분
 * 실패한 상태에서 불릴 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   irq_domain_free_irqs_hierarchy() → domain->ops->free → [이 함수] →
 *   irq_domain_free_irqs_parent()
 */
void irq_domain_free_irqs_common(struct irq_domain *domain, unsigned int virq,
				 unsigned int nr_irqs)
{
	struct irq_data *irq_data;	/* [한국어] 이 층의 irq_data */
	int i;	/* [한국어] 순회용 */

	for (i = 0; i < nr_irqs; i++) {	/* [한국어] 각 인터럽트에 대해 */
		irq_data = irq_domain_get_irq_data(domain, virq + i);	/* [한국어] 이 도메인의 층 */
		if (irq_data)	/* [한국어] 계층이 잘렸거나 부분 실패한 상태면 없을 수 있다 */
			irq_domain_reset_irq_data(irq_data);	/* [한국어] 번호와 칩을 초기 상태로 */
	}
	irq_domain_free_irqs_parent(domain, virq, nr_irqs);	/* [한국어] 부모에게 넘긴다. 그 부모가 다시 자기 free 콜백에서 이 함수를 불러 계층 끝까지 이어진다 */
}
EXPORT_SYMBOL_GPL(irq_domain_free_irqs_common);	/* [한국어] 대부분의 계층형 드라이버가 free 콜백에 그대로 꽂는다 */

/**
 * irq_domain_free_irqs_top - Clear handler and handler data, clear irqdata and free parent
 * @domain:	Interrupt domain to match
 * @virq:	IRQ number to start with
 * @nr_irqs:	The number of irqs to free
 */
/*
 * [한국어]
 * irq_domain_free_irqs_top - 처리기까지 떼고 아래로 해제를 넘긴다
 *
 * @domain:  가장 바깥 층의 도메인
 * @virq:    첫 리눅스 인터럽트 번호
 * @nr_irqs: 개수
 * @return:  없음
 *
 * 위 common 판에 흐름 처리기 제거를 더한 것이다. 가장 바깥 층이
 * 쓴다 — 처리기가 계층 전체에 하나뿐이라 그 층에서만 떼야 한다.
 *
 * irq_domain_set_info() 와 대칭이다. 그쪽이 층별 설정과 계층 공통
 * 설정을 함께 했듯이, 이쪽도 함께 되돌린다.
 *
 * 처리기를 먼저 떼는 순서에 주목: irq_set_handler(NULL) 이 그 안에서
 * 선을 마스크한다. 아래 층의 자원을 반납하기 전에 인터럽트가 오지
 * 않게 만드는 것이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   msi_domain_free() / irqchip 드라이버의 free 콜백 → [이 함수] →
 *   irq_domain_free_irqs_common()
 */
void irq_domain_free_irqs_top(struct irq_domain *domain, unsigned int virq,
			      unsigned int nr_irqs)
{
	int i;	/* [한국어] 순회용 */

	for (i = 0; i < nr_irqs; i++) {	/* [한국어] 각 인터럽트에 대해 */
		irq_set_handler_data(virq + i, NULL);	/* [한국어] 처리기의 사설 데이터부터 지운다 */
		irq_set_handler(virq + i, NULL);	/* [한국어] 처리기를 떼면 그 안에서 선이 마스크된다. 아래 층의 자원을 반납하기 전에 인터럽트가 오지 않게 한다 */
	}
	irq_domain_free_irqs_common(domain, virq, nr_irqs);	/* [한국어] 층별 정리와 부모로의 전달 */
}
EXPORT_SYMBOL_GPL(irq_domain_free_irqs_top);	/* [한국어] 가장 바깥 층 도메인이 쓴다 */

/*
 * [한국어]
 * irq_domain_free_irqs_hierarchy - 도메인의 free 콜백을 인터럽트마다 부른다
 *
 * @domain:   대상 도메인
 * @irq_base: 첫 리눅스 인터럽트 번호
 * @nr_irqs:  개수
 * @return:   없음
 *
 * 도메인의 free 콜백을 부르는 곳이다. 그런데 nr_irqs 개를 한 번에
 * 넘기지 않고 하나씩 부른다.
 *
 * 왜 그런가: 부분 실패한 상태를 정리해야 하기 때문이다. 할당이
 * 중간에 실패하면 일부 인터럽트만 이 층에 등록돼 있다. 등록되지
 * 않은 것에 free 콜백을 부르면 드라이버가 혼란스러워진다. 그래서
 * irq_data 가 있는 것만 골라 하나씩 부른다.
 *
 * free 콜백이 없는 도메인을 조용히 넘기는 것에 주목: 반납할 자원이
 * 없는 층이 있을 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   irq_domain_free_irqs() / irq_domain_free_irqs_parent() /
 *   irq_domain_pop_irq() → [이 함수] → domain->ops->free()
 */
static void irq_domain_free_irqs_hierarchy(struct irq_domain *domain,
					   unsigned int irq_base,
					   unsigned int nr_irqs)
{
	unsigned int i;	/* [한국어] 순회용 */

	if (!domain->ops->free)	/* [한국어] 반납할 자원이 없는 층인가 */
		return;	/* [한국어] 조용히 넘어간다 */

	for (i = 0; i < nr_irqs; i++) {	/* [한국어] 한 번에 넘기지 않고 하나씩 */
		if (irq_domain_get_irq_data(domain, irq_base + i))	/* [한국어] 이 층에 실제로 등록된 인터럽트인가 */
			domain->ops->free(domain, irq_base + i, 1);	/* [한국어] 부분 실패한 상태를 정리할 때, 등록되지 않은 것에 콜백을 부르면 드라이버가 혼란스러워진다 */
	}
}

/*
 * [한국어]
 * irq_domain_alloc_irqs_hierarchy - 도메인의 alloc 콜백을 부른다
 *
 * @domain:   대상 도메인
 * @irq_base: 첫 리눅스 인터럽트 번호
 * @nr_irqs:  개수
 * @arg:      도메인 고유의 할당 정보
 * @return:   0 성공, -ENOSYS 콜백 없음, 그 외 드라이버 오류
 *
 * 위 free 판과 달리 nr_irqs 개를 한 번에 넘긴다. 할당은 부분 실패가
 * 없으므로 — 콜백이 성공하면 전부, 실패하면 전부 아니다 — 하나씩
 * 부를 이유가 없다.
 *
 * alloc 콜백이 없으면 -ENOSYS 인 것에 주목: 계층형 도메인은 반드시
 * 이것을 제공해야 한다. irq_domain_check_hierarchy() 가 그 유무로
 * 계층형 여부를 판단하므로, 여기서 없다는 것은 모순이다.
 *
 * pr_debug 로만 알리는 것은 이 상황이 실제로는 일어나지 않기
 * 때문이다. 계층형 플래그가 alloc 유무로 세워지므로 여기 도달하려면
 * 다른 경로로 플래그가 세워져야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   irq_domain_alloc_irqs_locked() / irq_domain_alloc_irqs_parent() /
 *   irq_domain_push_irq() → [이 함수] → domain->ops->alloc()
 */
static int irq_domain_alloc_irqs_hierarchy(struct irq_domain *domain, unsigned int irq_base,
					   unsigned int nr_irqs, void *arg)
{
	if (!domain->ops->alloc) {	/* [한국어] 계층형 도메인은 반드시 제공해야 하는 콜백이다 */
		pr_debug("domain->ops->alloc() is NULL\n");	/* [한국어] 실제로는 일어나지 않는다 — 계층형 플래그가 alloc 유무로 세워지기 때문이다 */
		return -ENOSYS;	/* [한국어] 계층형 도메인은 alloc 콜백을 반드시 제공해야 한다. 여기 도달하는 것 자체가 모순이다 */
	}

	return domain->ops->alloc(domain, irq_base, nr_irqs, arg);	/* [한국어] free 와 달리 한 번에 넘긴다. 할당은 부분 실패가 없어 — 성공하면 전부, 실패하면 전부 아니다 */
}

/*
 * [한국어]
 * irq_domain_alloc_irqs_locked - 계층형 할당의 전체 절차 (락 보유)
 *
 * @domain:   가장 바깥 도메인
 * @irq_base: 특정 번호를 원하면 그 번호, 아무 데나면 음수
 * @nr_irqs:  개수
 * @node:     NUMA 노드
 * @arg:      도메인 고유의 할당 정보
 * @realloc:  서술자가 이미 있는가
 * @affinity: 초기 친화도
 * @return:   배정된 첫 번호, 실패 시 음수
 *
 * 계층형 인터럽트 할당의 중심이다. 다섯 단계로 진행한다.
 *
 *   1. 리눅스 인터럽트 번호 확보 (realloc 이면 건너뛴다)
 *   2. 계층 전체의 irq_data 사슬 생성
 *   3. 도메인의 alloc 콜백 — 계층을 따라 내려가며 하드웨어 자원 배정
 *   4. 계층 다듬기 — 드라이버가 표시한 불필요한 층 제거
 *   5. 역방향 맵에 등록
 *
 * realloc 이 무엇인가: 원본 주석대로 레거시 인터럽트 지원을 위한
 * 것이다. 서술자가 이미 만들어져 있고 그 번호에 계층만 붙이는 경우다.
 *
 * 3 과 5 가 나뉜 이유가 중요하다. alloc 콜백은 각 층의 자원을
 * 배정하지만 아직 하드웨어를 프로그래밍하지 않는다. 역방향 맵 등록도
 * 그 뒤다. 그래서 중간에 실패해도 되돌리기가 쉽다 — 아무도 이
 * 인터럽트를 조회할 수 없는 상태이기 때문이다.
 *
 * 에러 경로가 두 레이블인 것에 주목: irq_data 사슬 생성이 실패하면
 * 서술자만 되돌리고, 그 뒤에 실패하면 사슬까지 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   __irq_domain_alloc_irqs() / irq_create_fwspec_mapping() → [이 함수]
 */
static int irq_domain_alloc_irqs_locked(struct irq_domain *domain, int irq_base,
					unsigned int nr_irqs, int node, void *arg,
					bool realloc, const struct irq_affinity_desc *affinity)
{
	int i, ret, virq;	/* [한국어] 순회, 결과, 배정된 첫 번호 */

	if (realloc && irq_base >= 0) {	/* [한국어] 서술자가 이미 있고 번호도 정해져 있는가 — 레거시 인터럽트 지원 경로다 */
		virq = irq_base;	/* [한국어] 그 번호를 그대로 쓴다 */
	} else {	/* [한국어] 새로 확보해야 하는 경우 */
		virq = irq_domain_alloc_descs(irq_base, nr_irqs, 0, node,	/* [한국어] hwirq 힌트를 0 으로 주는 것에 주목 — 계층형에서는 층마다 번호가 달라 어느 것을 힌트로 쓸지 정하기 어렵다 */
					      affinity);
		if (virq < 0) {	/* [한국어] 번호를 못 받았는가 */
			pr_debug("cannot allocate IRQ(base %d, count %d)\n",	/* [한국어] 번호 공간이 고갈됐다. 요청한 시작 번호와 개수를 함께 남겨 진단을 돕는다 */
				 irq_base, nr_irqs);
			return virq;	/* [한국어] 음수 오류를 그대로 올린다 */
		}
	}

	if (irq_domain_alloc_irq_data(domain, virq, nr_irqs)) {	/* [한국어] 계층 전체의 irq_data 사슬을 만든다. 아직 하드웨어 자원은 배정하지 않는다 */
		pr_debug("cannot allocate memory for IRQ%d\n", virq);	/* [한국어] 계층 사슬을 만들 메모리가 부족하다. 위 번호 고갈과 구분해 남긴다 */
		ret = -ENOMEM;	/* [한국어] irq_domain_alloc_irq_data 가 오류 코드를 돌려주지 않아 여기서 채운다 */
		goto out_free_desc;	/* [한국어] 서술자만 되돌리면 된다 */
	}

	ret = irq_domain_alloc_irqs_hierarchy(domain, virq, nr_irqs, arg);	/* [한국어] 계층을 따라 내려가며 각 층의 자원을 배정한다. 아직 하드웨어를 프로그래밍하지는 않는다 — 그것은 활성화 단계다 */
	if (ret < 0)	/* [한국어] 벡터 고갈 등 */
		goto out_free_irq_data;	/* [한국어] 사슬까지 되돌린다 */

	for (i = 0; i < nr_irqs; i++) {	/* [한국어] 각 인터럽트에 대해 */
		ret = irq_domain_trim_hierarchy(virq + i);	/* [한국어] 드라이버가 alloc 콜백 안에서 표시한 불필요한 층을 잘라 낸다 */
		if (ret)	/* [한국어] 사슬이 이상한 모양인가 */
			goto out_free_irq_data;
	}

	for (i = 0; i < nr_irqs; i++)	/* [한국어] 모든 준비가 끝난 뒤에 */
		irq_domain_insert_irq(virq + i);	/* [한국어] 역방향 맵에 등록한다. 이 줄까지 오기 전에는 아무도 이 인터럽트를 조회할 수 없어 되돌리기가 쉽다 */

	return virq;	/* [한국어] 배정된 첫 번호 */

out_free_irq_data:	/* [한국어] 사슬을 만든 뒤 실패한 경우 */
	irq_domain_free_irq_data(virq, nr_irqs);	/* [한국어] 사슬 해제 */
out_free_desc:	/* [한국어] 두 경로가 합류한다 */
	irq_free_descs(virq, nr_irqs);	/* [한국어] 서술자 반납. realloc 이었다면 원래 있던 것을 반납하는 셈인데, 그 경로에서 여기 도달하는 것은 드물다 */
	return ret;	/* [한국어] 실패 원인 */
}

/**
 * __irq_domain_alloc_irqs - Allocate IRQs from domain
 * @domain:	domain to allocate from
 * @irq_base:	allocate specified IRQ number if irq_base >= 0
 * @nr_irqs:	number of IRQs to allocate
 * @node:	NUMA node id for memory allocation
 * @arg:	domain specific argument
 * @realloc:	IRQ descriptors have already been allocated if true
 * @affinity:	Optional irq affinity mask for multiqueue devices
 *
 * Allocate IRQ numbers and initialized all data structures to support
 * hierarchy IRQ domains.
 * Parameter @realloc is mainly to support legacy IRQs.
 * Returns error code or allocated IRQ number
 *
 * The whole process to setup an IRQ has been split into two steps.
 * The first step, __irq_domain_alloc_irqs(), is to allocate IRQ
 * descriptor and required hardware resources. The second step,
 * irq_domain_activate_irq(), is to program the hardware with preallocated
 * resources. In this way, it's easier to rollback when failing to
 * allocate resources.
 */
/*
 * [한국어]
 * __irq_domain_alloc_irqs - 계층형 도메인에서 인터럽트를 할당한다
 *
 * @domain:   대상 도메인, NULL 이면 기본 도메인
 * @irq_base: 특정 번호를 원하면 그 번호, 아무 데나면 음수
 * @nr_irqs:  개수
 * @node:     NUMA 노드
 * @arg:      도메인 고유의 할당 정보
 * @realloc:  서술자가 이미 있는가
 * @affinity: 다중 큐 장치의 친화도 배열
 * @return:   배정된 첫 번호, 실패 시 음수 오류
 *
 * 위 함수에 락 획득을 씌운 공개 API 다. MSI 코어와 irqchip
 * 드라이버가 부르는 진입점이다.
 *
 * 원본 주석의 마지막 문단이 이 서브시스템의 중요한 설계를 설명한다.
 * 인터럽트 설정을 두 단계로 나눈 이유다.
 *
 *   1 단계 (이 함수): 서술자와 하드웨어 자원을 확보한다.
 *   2 단계 (irq_domain_activate_irq): 그 자원으로 하드웨어를
 *      실제로 프로그래밍한다.
 *
 * 왜 나누는가: 1 단계에서 실패하면 되돌리기가 쉽다 — 아직 하드웨어를
 * 건드리지 않았기 때문이다. 두 단계를 합치면 중간 실패 시 이미
 * 설정한 하드웨어를 일일이 되돌려야 하고, 그 과정에서 또 실패할 수
 * 있다.
 *
 * 그 분리 덕분에 예약 모드 같은 최적화도 가능해진다 — 1 단계에서
 * 자원을 잡아 두고 2 단계를 request_irq 까지 미룬다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   __msi_domain_alloc_irqs() / irqchip 드라이버 → [이 함수] →
 *   irq_domain_alloc_irqs_locked()
 */
int __irq_domain_alloc_irqs(struct irq_domain *domain, int irq_base,
			    unsigned int nr_irqs, int node, void *arg,
			    bool realloc, const struct irq_affinity_desc *affinity)
{
	int ret;	/* [한국어] 결과 */

	if (domain == NULL) {	/* [한국어] 도메인을 지정하지 않았는가 */
		domain = irq_default_domain;	/* [한국어] 기본 도메인 */
		if (WARN(!domain, "domain is NULL; cannot allocate IRQ\n"))	/* [한국어] 그것도 없으면 어느 컨트롤러인지 알 수 없다 */
			return -EINVAL;
	}

	mutex_lock(&domain->root->mutex);	/* [한국어] 계층 전체가 공유하는 락. 할당이 여러 층을 건드리므로 하나의 임계 구역이어야 한다 */
	ret = irq_domain_alloc_irqs_locked(domain, irq_base, nr_irqs, node, arg,	/* [한국어] 실제 할당 */
					   realloc, affinity);
	mutex_unlock(&domain->root->mutex);	/* [한국어] 계층 전체의 할당이 끝났다 */

	return ret;	/* [한국어] 배정된 번호 또는 음수 오류 */
}
EXPORT_SYMBOL_GPL(__irq_domain_alloc_irqs);	/* [한국어] MSI 코어와 irqchip 드라이버가 부른다 */

/* The irq_data was moved, fix the revmap to refer to the new location */
/*
 * [한국어]
 * irq_domain_fix_revmap - 옮겨진 irq_data 를 가리키도록 역방향 맵을 고친다
 *
 * @d: 새 위치의 irq_data
 * @return: 없음
 *
 * 아래 push/pop 전용 함수다. 그 두 연산은 irq_data 의 내용을 다른
 * 메모리로 옮기는데, 역방향 맵은 옛 주소를 가리키고 있다. 그것을
 * 새 주소로 고친다.
 *
 * 왜 옮기는가: 가장 바깥 층의 irq_data 는 서술자 안에 박혀 있어
 * 주소가 고정이다. 그 위에 층을 하나 끼워 넣으려면, 원래 있던
 * 내용을 새로 할당한 메모리로 옮기고 박힌 자리를 새 층이 차지해야
 * 한다.
 *
 * 래딕스 트리 쪽에서 slot 을 찾아 교체하는 것에 주목: 지우고 다시
 * 넣으면 그 사이에 조회가 실패할 수 있다. slot 교체는 원자적이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   irq_domain_push_irq() / irq_domain_pop_irq() → [이 함수]
 */
static void irq_domain_fix_revmap(struct irq_data *d)
{
	void __rcu **slot;	/* [한국어] 래딕스 트리의 슬롯 포인터 */

	lockdep_assert_held(&d->domain->root->mutex);	/* [한국어] 계층 공용 락 */

	if (irq_domain_is_nomap(d->domain))	/* [한국어] 맵을 쓰지 않는 도메인인가 */
		return;	/* [한국어] 고칠 것이 없다 */

	/* Fix up the revmap. */
	if (d->hwirq < d->domain->revmap_size) {	/* [한국어] (위 영어 주석) 선형 배열 범위인가 */
		/* Not using radix tree */
		rcu_assign_pointer(d->domain->revmap[d->hwirq], d);	/* [한국어] (위 영어 주석) 새 주소로 대입 */
	} else {	/* [한국어] 래딕스 트리 범위 */
		slot = radix_tree_lookup_slot(&d->domain->revmap_tree, d->hwirq);	/* [한국어] 슬롯을 찾아 */
		if (slot)	/* [한국어] 있으면 */
			radix_tree_replace_slot(&d->domain->revmap_tree, slot, d);	/* [한국어] 교체한다. 지우고 다시 넣으면 그 사이에 조회가 실패할 수 있다 */
	}
}

/**
 * irq_domain_push_irq() - Push a domain in to the top of a hierarchy.
 * @domain:	Domain to push.
 * @virq:	Irq to push the domain in to.
 * @arg:	Passed to the irq_domain_ops alloc() function.
 *
 * For an already existing irqdomain hierarchy, as might be obtained
 * via a call to pci_enable_msix(), add an additional domain to the
 * head of the processing chain.  Must be called before request_irq()
 * has been called.
 */
/*
 * [한국어]
 * irq_domain_push_irq - 이미 만들어진 계층의 꼭대기에 층을 끼워 넣는다
 *
 * @domain: 끼워 넣을 도메인 (기존 계층의 바깥에 온다)
 * @virq:   대상 리눅스 인터럽트 번호
 * @arg:    그 도메인의 alloc 콜백에 넘길 인자
 * @return: 0 성공, 음수 오류
 *
 * 특이한 연산이다. 보통 계층은 인터럽트를 할당할 때 한 번에
 * 만들어지는데, 이 함수는 이미 동작하는 인터럽트의 계층에 층을
 * 덧붙인다.
 *
 * 어떤 상황에 쓰는가: PCI MSI-X 로 인터럽트를 받은 뒤, 그 위에
 * 추가 처리 층을 얹고 싶은 경우다. 어떤 가상화 환경이나 특수
 * 컨트롤러가 그렇게 한다.
 *
 * 메모리 조작이 이 함수의 핵심이다. 가장 바깥 층은 서술자에 박혀
 * 있어 주소가 고정이다. 그래서:
 *
 *   1. 새 메모리를 할당해 기존 내용을 통째로 복사한다.
 *   2. 박힌 자리를 새 도메인의 것으로 덮어쓴다.
 *   3. 복사본을 그 부모로 매단다.
 *   4. 역방향 맵이 복사본을 가리키도록 고친다.
 *
 * desc->action 검사가 안전장치다. 원본 주석이 설명하듯, 인터럽트가
 * 요청되지 않은 상태여야 한다. 요청된 상태에서 계층을 바꾸면
 * 인터럽트 처리와 경쟁하는데, 그것을 막으려면 alloc 콜백 전체에
 * 걸쳐 락을 잡아야 하고 그러면 데드락이 날 수 있다. 그래서 간단한
 * 사전 검사로 대신한다.
 *
 * 실패 시 원본을 복원하는 것에 주목: 덮어쓴 자리를 복사본으로
 * 되돌린다. 이 되돌림이 없으면 인터럽트가 망가진 채 남는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, request_irq 전.
 *
 * 호출 체인:
 *   특수 컨트롤러 드라이버 → [이 함수] →
 *   irq_domain_alloc_irqs_hierarchy()
 */
int irq_domain_push_irq(struct irq_domain *domain, int virq, void *arg)
{
	struct irq_data *irq_data = irq_get_irq_data(virq);	/* [한국어] 서술자에 박힌 가장 바깥 층. 주소가 고정이라 이 함수의 복잡함이 여기서 나온다 */
	struct irq_data *parent_irq_data;	/* [한국어] 기존 내용을 옮겨 담을 새 메모리 */
	struct irq_desc *desc;	/* [한국어] action 검사용 */
	int rv = 0;	/* [한국어] 결과 */

	/*
	 * Check that no action has been set, which indicates the virq
	 * is in a state where this function doesn't have to deal with
	 * races between interrupt handling and maintaining the
	 * hierarchy.  This will catch gross misuse.  Attempting to
	 * make the check race free would require holding locks across
	 * calls to struct irq_domain_ops->alloc(), which could lead
	 * to deadlock, so we just do a simple check before starting.
	 */
	desc = irq_to_desc(virq);	/* [한국어] (위 영어 주석) 서술자 조회 */
	if (!desc)	/* [한국어] 없는 인터럽트인가 */
		return -EINVAL;
	if (WARN_ON(desc->action))	/* [한국어] 이미 요청된 인터럽트인가 — 처리와 경쟁하게 된다. 완전한 방어는 alloc 콜백 전체에 락을 잡아야 하고 그러면 데드락이 날 수 있어 간단한 사전 검사로 대신한다 */
		return -EBUSY;

	if (domain == NULL)	/* [한국어] 끼워 넣을 도메인이 없는가 */
		return -EINVAL;

	if (WARN_ON(!irq_domain_is_hierarchy(domain)))	/* [한국어] 계층형이 아닌 도메인을 끼워 넣으려 하는가 */
		return -EINVAL;

	if (!irq_data)	/* [한국어] irq_data 가 없는가 */
		return -EINVAL;

	if (domain->parent != irq_data->domain)	/* [한국어] 끼워 넣을 도메인의 부모가 현재 가장 바깥 도메인인가 — 그래야 계층이 이어진다 */
		return -EINVAL;

	parent_irq_data = kzalloc_node(sizeof(*parent_irq_data), GFP_KERNEL,	/* [한국어] 기존 내용을 옮겨 담을 메모리. 락 밖에서 미리 잡는다 */
				       irq_data_get_node(irq_data));	/* [한국어] 같은 NUMA 노드 */
	if (!parent_irq_data)	/* [한국어] 메모리 부족 */
		return -ENOMEM;

	mutex_lock(&domain->root->mutex);	/* [한국어] 계층 조작을 직렬화한다 */

	/* Copy the original irq_data. */
	*parent_irq_data = *irq_data;	/* [한국어] (위 영어 주석) 기존 내용을 통째로 복사한다. 이것이 새 계층의 부모 층이 된다 */

	/*
	 * Overwrite the irq_data, which is embedded in struct irq_desc, with
	 * values for this domain.
	 */
	irq_data->parent_data = parent_irq_data;	/* [한국어] (위 영어 주석) 복사본을 부모로 매단다 */
	irq_data->domain = domain;	/* [한국어] 박힌 자리를 새 도메인의 것으로 */
	irq_data->mask = 0;	/* [한국어] 새 층의 비트 위치. alloc 콜백이 채운다 */
	irq_data->hwirq = 0;	/* [한국어] 새 층의 하드웨어 번호 */
	irq_data->chip = NULL;	/* [한국어] 새 층의 칩 */
	irq_data->chip_data = NULL;	/* [한국어] 새 층의 칩 데이터 */

	/* May (probably does) set hwirq, chip, etc. */
	rv = irq_domain_alloc_irqs_hierarchy(domain, virq, 1, arg);	/* [한국어] (위 영어 주석) 새 층의 자원을 배정한다. 그 콜백이 위 필드들을 채운다 */
	if (rv) {	/* [한국어] 실패 */
		/* Restore the original irq_data. */
		*irq_data = *parent_irq_data;	/* [한국어] (위 영어 주석) 덮어쓴 자리를 복사본으로 되돌린다. 이 되돌림이 없으면 인터럽트가 망가진 채 남는다 */
		kfree(parent_irq_data);	/* [한국어] 복사본 해제 */
		goto error;	/* [한국어] 원본을 이미 복원했다. 아래에서 락만 풀면 된다 */
	}

	irq_domain_fix_revmap(parent_irq_data);	/* [한국어] 옛 층의 맵 항목이 이제 복사본을 가리켜야 한다 */
	irq_domain_set_mapping(domain, irq_data->hwirq, irq_data);	/* [한국어] 새 층을 자기 도메인의 맵에 등록한다 */
error:	/* [한국어] 성공과 실패가 여기서 합류한다 */
	mutex_unlock(&domain->root->mutex);	/* [한국어] 성공과 실패가 이 한 줄을 공유한다 */

	return rv;	/* [한국어] 0 또는 오류 */
}
EXPORT_SYMBOL_GPL(irq_domain_push_irq);	/* [한국어] 특수 컨트롤러 드라이버가 부른다 */

/**
 * irq_domain_pop_irq() - Remove a domain from the top of a hierarchy.
 * @domain:	Domain to remove.
 * @virq:	Irq to remove the domain from.
 *
 * Undo the effects of a call to irq_domain_push_irq().  Must be
 * called either before request_irq() or after free_irq().
 */
/*
 * [한국어]
 * irq_domain_pop_irq - 계층의 꼭대기에서 층을 뺀다
 *
 * @domain: 뺄 도메인 (계층의 가장 바깥이어야 한다)
 * @virq:   대상 리눅스 인터럽트 번호
 * @return: 0 성공, 음수 오류
 *
 * irq_domain_push_irq() 의 정확한 반대다. 메모리 조작도 역순이다.
 *
 *   1. 사슬을 끊고 새 층을 맵에서 뺀다.
 *   2. 그 층의 자원을 반납한다.
 *   3. 부모의 내용을 박힌 자리로 되돌린다.
 *   4. 역방향 맵이 박힌 자리를 다시 가리키게 고친다.
 *   5. 부모 메모리를 해제한다.
 *
 * 3 이 push 의 복사와 대칭이다. push 가 박힌 내용을 새 메모리로
 * 옮겼으니, pop 은 그것을 되돌린다.
 *
 * 검사가 push 보다 많은 것에 주목: 뺄 도메인이 정말 계층의 가장
 * 바깥인지 확인해야 한다. 중간 층을 빼려 하면 사슬이 끊어진다.
 * irq_domain_get_irq_data() 로 찾은 것이 가장 바깥 층과 같은지
 * 보는 것이 그 확인이다.
 *
 * kfree 를 락 밖에서 하는 것도 눈에 띈다. push 는 락 안에서
 * 하는데(실패 경로라 급하다), 여기서는 굳이 임계 구역을 늘릴 이유가
 * 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, request_irq 전 또는 free_irq 후.
 *
 * 호출 체인:
 *   특수 컨트롤러 드라이버 → [이 함수] →
 *   irq_domain_free_irqs_hierarchy()
 */
int irq_domain_pop_irq(struct irq_domain *domain, int virq)
{
	struct irq_data *irq_data = irq_get_irq_data(virq);	/* [한국어] 서술자에 박힌 가장 바깥 층 */
	struct irq_data *parent_irq_data;	/* [한국어] 그 안쪽 층. 내용이 박힌 자리로 되돌아갈 것이다 */
	struct irq_data *tmp_irq_data;	/* [한국어] 검증용 — 뺄 도메인의 층을 따로 찾아 비교한다 */
	struct irq_desc *desc;	/* [한국어] action 검사용 */

	/*
	 * Check that no action is set, which indicates the virq is in
	 * a state where this function doesn't have to deal with races
	 * between interrupt handling and maintaining the hierarchy.
	 * This will catch gross misuse.  Attempting to make the check
	 * race free would require holding locks across calls to
	 * struct irq_domain_ops->free(), which could lead to
	 * deadlock, so we just do a simple check before starting.
	 */
	desc = irq_to_desc(virq);	/* [한국어] (위 영어 주석) 서술자 조회 */
	if (!desc)	/* [한국어] 없는 인터럽트인가 */
		return -EINVAL;
	if (WARN_ON(desc->action))	/* [한국어] 요청된 상태인가 — push 와 같은 이유로 막는다 */
		return -EBUSY;

	if (domain == NULL)	/* [한국어] 뺄 도메인이 없는가 */
		return -EINVAL;

	if (!irq_data)	/* [한국어] irq_data 가 없는가 */
		return -EINVAL;

	tmp_irq_data = irq_domain_get_irq_data(domain, virq);	/* [한국어] 뺄 도메인의 층을 따로 찾는다 */

	/* We can only "pop" if this domain is at the top of the list */
	if (WARN_ON(irq_data != tmp_irq_data))	/* [한국어] (위 영어 주석) 찾은 것이 가장 바깥 층과 같은가 — 중간 층을 빼려 하면 사슬이 끊어진다 */
		return -EINVAL;

	if (WARN_ON(irq_data->domain != domain))	/* [한국어] 위 검사와 겹치지만 한 번 더 확인한다 */
		return -EINVAL;

	parent_irq_data = irq_data->parent_data;	/* [한국어] 되돌릴 내용이 담긴 층 */
	if (WARN_ON(!parent_irq_data))	/* [한국어] 안쪽 층이 없는가 — push 로 끼워 넣은 것이 아니라는 뜻이다 */
		return -EINVAL;

	mutex_lock(&domain->root->mutex);	/* [한국어] 계층 조작 직렬화 */

	irq_data->parent_data = NULL;	/* [한국어] 사슬을 먼저 끊는다 */

	irq_domain_clear_mapping(domain, irq_data->hwirq);	/* [한국어] 뺄 층을 자기 도메인의 맵에서 제거 */
	irq_domain_free_irqs_hierarchy(domain, virq, 1);	/* [한국어] 그 층의 자원 반납. 사슬을 끊은 뒤라 부모까지 내려가지 않는다 */

	/* Restore the original irq_data. */
	*irq_data = *parent_irq_data;	/* [한국어] (위 영어 주석) push 의 복사와 대칭이다. 안쪽 층의 내용을 박힌 자리로 되돌린다 */

	irq_domain_fix_revmap(irq_data);	/* [한국어] 그 층의 맵 항목이 이제 박힌 자리를 가리켜야 한다 */

	mutex_unlock(&domain->root->mutex);	/* [한국어] 아래 해제는 락 밖에서. 굳이 임계 구역을 늘릴 이유가 없다 */

	kfree(parent_irq_data);	/* [한국어] 내용을 옮겼으므로 이 메모리는 더 이상 필요 없다 */

	return 0;	/* [한국어] 계층에서 층 하나를 빼는 데 성공했다 */
}
EXPORT_SYMBOL_GPL(irq_domain_pop_irq);	/* [한국어] push 와 짝을 이룬다 */

/**
 * irq_domain_free_irqs - Free IRQ number and associated data structures
 * @virq:	base IRQ number
 * @nr_irqs:	number of IRQs to free
 */
/*
 * [한국어]
 * irq_domain_free_irqs - 계층형 인터럽트를 반납한다
 *
 * @virq:    첫 리눅스 인터럽트 번호
 * @nr_irqs: 개수
 * @return:  없음
 *
 * __irq_domain_alloc_irqs() 의 반대다. 순서가 할당의 역순이다.
 *
 *   1. 역방향 맵에서 제거 (처리기 제거와 synchronize_irq 포함)
 *   2. 계층 각 층의 자원 반납
 *   3. irq_data 사슬 해제
 *   4. 서술자 반납
 *
 * 1 과 2 만 락 안에서 하는 것에 주목: 3 과 4 는 이미 아무도 이
 * 인터럽트를 조회할 수 없는 상태라 락이 필요 없다. 임계 구역을
 * 짧게 유지한다.
 *
 * 도메인을 인자로 받지 않고 irq_data 에서 얻는 것도 눈에 띈다.
 * 호출자가 도메인을 몰라도 되게 하려는 것이다.
 *
 * 세 조건을 한 WARN 으로 묶은 것: irq_data 가 없거나, 도메인에 붙어
 * 있지 않거나, 그 도메인이 free 콜백을 제공하지 않는 경우다. 셋 다
 * "이 인터럽트는 계층형으로 할당된 것이 아니다" 를 뜻한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   __msi_domain_free_irqs() / irq_domain_free_one_irq() → [이 함수]
 */
void irq_domain_free_irqs(unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *data = irq_get_irq_data(virq);	/* [한국어] 도메인을 여기서 얻는다. 호출자가 도메인을 몰라도 되게 하려는 것이다 */
	struct irq_domain *domain;	/* [한국어] 대상 도메인 */
	int i;	/* [한국어] 순회용 */

	if (WARN(!data || !data->domain || !data->domain->ops->free,	/* [한국어] 셋 다 "이 인터럽트는 계층형으로 할당된 것이 아니다" 를 뜻한다 */
		 "NULL pointer, cannot free irq\n"))
		return;

	domain = data->domain;	/* [한국어] 가장 바깥 도메인 */

	mutex_lock(&domain->root->mutex);	/* [한국어] 계층 공용 락 */
	for (i = 0; i < nr_irqs; i++)	/* [한국어] 각 인터럽트에 대해 */
		irq_domain_remove_irq(virq + i);	/* [한국어] 맵에서 제거. 그 안에서 처리기를 떼고 진행 중인 처리를 기다린다 */
	irq_domain_free_irqs_hierarchy(domain, virq, nr_irqs);	/* [한국어] 계층 각 층의 자원 반납. 도메인의 free 콜백이 부모로 이어 간다 */
	mutex_unlock(&domain->root->mutex);	/* [한국어] 아래 두 단계는 락 밖에서 — 이미 아무도 이 인터럽트를 조회할 수 없다 */

	irq_domain_free_irq_data(virq, nr_irqs);	/* [한국어] irq_data 사슬 해제 */
	irq_free_descs(virq, nr_irqs);	/* [한국어] 서술자 반납 */
}
EXPORT_SYMBOL_GPL(irq_domain_free_irqs);	/* [한국어] MSI 코어와 irqchip 드라이버가 부른다 */

/*
 * [한국어]
 * irq_domain_free_one_irq - 계층형 인터럽트 하나를 종류에 맞게 반납한다
 *
 * @domain: 대상 도메인
 * @virq:   반납할 리눅스 인터럽트 번호
 * @return: 없음
 *
 * irq_dispose_mapping() 이 계층형 도메인에 대해 부르는 갈림길이다.
 *
 * MSI 장치 도메인은 특별 취급이 필요하다. 그쪽은 MSI 서술자와
 * 저장소를 함께 정리해야 하는데, 그 지식은 kernel/irq/msi.c 에 있다.
 * 그냥 irq_domain_free_irqs() 를 부르면 서술자가 저장소에 남는다.
 *
 * wire-to-MSI 도메인이 이 경로로 온다 — 배선 인터럽트처럼 보이지만
 * 실제로는 MSI 로 구현된 인터럽트다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_dispose_mapping() → [이 함수] →
 *   msi_device_domain_free_wired() 또는 irq_domain_free_irqs()
 */
static void irq_domain_free_one_irq(struct irq_domain *domain, unsigned int virq)
{
	if (irq_domain_is_msi_device(domain))	/* [한국어] MSI 장치 도메인인가 — wire-to-MSI 가 이 경로로 온다 */
		msi_device_domain_free_wired(domain, virq);	/* [한국어] MSI 서술자와 저장소까지 함께 정리한다. 그 지식은 kernel/irq/msi.c 에 있다 */
	else	/* [한국어] 보통의 계층형 도메인 */
		irq_domain_free_irqs(virq, 1);	/* [한국어] 표준 반납 경로 */
}

/**
 * irq_domain_alloc_irqs_parent - Allocate interrupts from parent domain
 * @domain:	Domain below which interrupts must be allocated
 * @irq_base:	Base IRQ number
 * @nr_irqs:	Number of IRQs to allocate
 * @arg:	Allocation data (arch/domain specific)
 */
/*
 * [한국어]
 * irq_domain_alloc_irqs_parent - 부모 도메인에 할당을 요청한다
 *
 * @domain:   현재 층의 도메인
 * @irq_base: 첫 리눅스 인터럽트 번호
 * @nr_irqs:  개수
 * @arg:      도메인 고유의 할당 정보
 * @return:   0 성공, -ENOSYS 부모 없음, 그 외 부모의 오류
 *
 * 계층형 드라이버가 자기 alloc 콜백 안에서 부르는 표준 관용구다.
 * "내 것을 하기 전에 부모부터" 라는 규약을 구현한다.
 *
 * 왜 부모가 먼저인가: 자기 층의 설정이 부모의 결과에 기댈 수 있다.
 * MSI 도메인은 부모가 배정한 벡터 번호를 알아야 메시지를 조립할 수
 * 있다.
 *
 * 부모가 없으면 -ENOSYS 인 것에 주목: 계층의 가장 안쪽 도메인이
 * 이 함수를 부르면 그렇게 된다. 그 도메인은 부모에게 넘길 것이
 * 없으므로 자기가 실제 자원(CPU 벡터)을 배정해야 한다. 이 함수를
 * 부르는 것 자체가 설계 오류다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   계층형 드라이버의 alloc 콜백 / msi_domain_alloc() → [이 함수] →
 *   irq_domain_alloc_irqs_hierarchy()
 */
int irq_domain_alloc_irqs_parent(struct irq_domain *domain,
				 unsigned int irq_base, unsigned int nr_irqs,
				 void *arg)
{
	if (!domain->parent)	/* [한국어] 계층의 가장 안쪽인가 */
		return -ENOSYS;	/* [한국어] 넘길 부모가 없다. 그 도메인은 자기가 실제 자원을 배정해야 하므로 이 함수를 부르는 것 자체가 설계 오류다 */

	return irq_domain_alloc_irqs_hierarchy(domain->parent, irq_base,	/* [한국어] 부모의 alloc 콜백. 그 안에서 다시 자기 부모를 부를 수 있다 */
					       nr_irqs, arg);
}
EXPORT_SYMBOL_GPL(irq_domain_alloc_irqs_parent);	/* [한국어] 계층형 드라이버의 alloc 콜백이 부른다 */

/**
 * irq_domain_free_irqs_parent - Free interrupts from parent domain
 * @domain:	Domain below which interrupts must be freed
 * @irq_base:	Base IRQ number
 * @nr_irqs:	Number of IRQs to free
 */
/*
 * [한국어]
 * irq_domain_free_irqs_parent - 부모 도메인에 반납을 요청한다
 *
 * @domain:   현재 층의 도메인
 * @irq_base: 첫 리눅스 인터럽트 번호
 * @nr_irqs:  개수
 * @return:   없음
 *
 * 위 alloc 의 짝이다. 계층형 드라이버가 자기 free 콜백 안에서 마지막에
 * 부른다 — 자기 것을 먼저 정리하고 부모에게 넘긴다.
 *
 * 부모가 없을 때 조용히 넘어가는 것이 alloc 과 다르다. 반환값이
 * void 라 오류를 알릴 방법이 없고, 해제 경로에서 오류를 처리할
 * 방법도 마땅치 않다.
 *
 * 실행 컨텍스트: 프로세스 문맥, root 뮤텍스 보유.
 *
 * 호출 체인:
 *   irq_domain_free_irqs_common() / 계층형 드라이버의 free 콜백 →
 *   [이 함수] → irq_domain_free_irqs_hierarchy()
 */
void irq_domain_free_irqs_parent(struct irq_domain *domain,
				 unsigned int irq_base, unsigned int nr_irqs)
{
	if (!domain->parent)	/* [한국어] 계층의 가장 안쪽인가 */
		return;	/* [한국어] alloc 과 달리 조용히 넘어간다. 반환값이 void 라 알릴 방법이 없다 */

	irq_domain_free_irqs_hierarchy(domain->parent, irq_base, nr_irqs);	/* [한국어] 부모의 free 콜백 */
}
EXPORT_SYMBOL_GPL(irq_domain_free_irqs_parent);	/* [한국어] 계층형 드라이버의 free 콜백이 부른다 */

/*
 * [한국어]
 * __irq_domain_deactivate_irq - 계층을 재귀적으로 훑으며 비활성화한다
 *
 * @irq_data: 시작할 층 (대개 가장 바깥)
 * @return:   없음
 *
 * 비활성화는 하드웨어에 "이 인터럽트를 더 이상 보내지 마라" 를
 * 알리는 단계다. 계층의 각 층이 자기 몫을 한다 — MSI 도메인은
 * 메시지를 0 으로 밀고, 벡터 도메인은 벡터를 회수한다.
 *
 * 순서가 바깥에서 안쪽이다. 자기 층을 먼저 비활성화하고 부모로
 * 내려간다. 아래 활성화와 정확히 반대 순서다.
 *
 * 왜 그 순서인가: 바깥 층이 인터럽트 발생에 가깝다. MSI 메시지를
 * 먼저 지워야 장치가 인터럽트를 보내지 않고, 그 뒤에 벡터를 회수해도
 * 안전하다. 순서가 반대면 벡터가 회수된 뒤에도 장치가 그 벡터로
 * 인터럽트를 보낸다.
 *
 * 재귀인 것에 주목: 위 __irq_domain_free_hierarchy 는 반복문인데
 * 여기서는 재귀다. 계층 깊이가 얕아 스택 걱정이 없고, 아래 활성화
 * 함수와 대칭을 이루는 편이 읽기 쉽다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_domain_deactivate_irq() / __irq_domain_activate_irq() 의
 *   실패 경로 → [이 함수] → domain->ops->deactivate()
 */
static void __irq_domain_deactivate_irq(struct irq_data *irq_data)
{
	if (irq_data && irq_data->domain) {	/* [한국어] 유효한 층인가. 계층 끝에서 NULL 이 되어 재귀가 멈춘다 */
		struct irq_domain *domain = irq_data->domain;	/* [한국어] 이 층의 도메인 */

		if (domain->ops->deactivate)	/* [한국어] 비활성화 콜백을 제공하는가. 많은 층은 할 일이 없다 */
			domain->ops->deactivate(domain, irq_data);	/* [한국어] 자기 층을 먼저. MSI 도메인은 메시지를 0 으로 밀고, 벡터 도메인은 벡터를 회수한다 */
		if (irq_data->parent_data)	/* [한국어] 안쪽 층이 있는가 */
			__irq_domain_deactivate_irq(irq_data->parent_data);	/* [한국어] 바깥에서 안쪽 순서. 메시지를 먼저 지워야 장치가 안 보내고, 그 뒤 벡터를 회수해도 안전하다 */
	}
}

/*
 * [한국어]
 * __irq_domain_activate_irq - 계층을 재귀적으로 훑으며 활성화한다
 *
 * @irqd:    시작할 층 (대개 가장 바깥)
 * @reserve: 예약 모드인가 (더미 자원만 배정)
 * @return:  0 성공, 음수 어느 층의 오류
 *
 * 위 비활성화의 반대다. 순서도 반대로, 안쪽부터 활성화한다.
 *
 * 코드에서 그 순서가 드러나는 방식이 흥미롭다. 재귀 호출이 콜백
 * 호출보다 *앞에* 있다. 그래서 가장 안쪽까지 내려간 뒤 돌아오면서
 * 콜백이 불린다.
 *
 * 왜 안쪽이 먼저인가: 바깥 층이 안쪽의 결과에 기댄다. MSI 도메인이
 * 메시지를 조립하려면 벡터 도메인이 배정한 벡터 번호를 알아야 한다.
 *
 * 되돌리기가 각 층에 있는 것에 주목: 자기 층의 활성화가 실패하면
 * 이미 활성화된 안쪽 층들을 되돌린다. 재귀의 각 단계가 자기 몫만
 * 되돌리면 되므로, 사슬 전체가 자연스럽게 정리된다.
 *
 * reserve 인자는 그대로 아래로 전달된다. 예약 모드에서 벡터 도메인이
 * 진짜 벡터 대신 더미를 배정하는 판단을 하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_domain_activate_irq() → [이 함수] → domain->ops->activate()
 */
static int __irq_domain_activate_irq(struct irq_data *irqd, bool reserve)
{
	int ret = 0;	/* [한국어] 결과. 계층 끝에서는 0 이 그대로 나간다 */

	if (irqd && irqd->domain) {	/* [한국어] 유효한 층인가 */
		struct irq_domain *domain = irqd->domain;	/* [한국어] 이 층의 도메인 */

		if (irqd->parent_data)	/* [한국어] 안쪽 층이 있는가 */
			ret = __irq_domain_activate_irq(irqd->parent_data,	/* [한국어] 재귀가 콜백보다 앞에 있다. 가장 안쪽까지 내려간 뒤 돌아오면서 콜백이 불린다 — 바깥 층이 안쪽의 결과(벡터 번호 등)에 기대기 때문이다 */
							reserve);
		if (!ret && domain->ops->activate) {	/* [한국어] 안쪽이 성공했고 이 층에 활성화 콜백이 있는가 */
			ret = domain->ops->activate(domain, irqd, reserve);	/* [한국어] 하드웨어를 실제로 프로그래밍한다. MSI 는 여기서 메시지를 장치에 써 넣는다 */
			/* Rollback in case of error */
			if (ret && irqd->parent_data)	/* [한국어] (위 영어 주석) 자기 층이 실패했는가 */
				__irq_domain_deactivate_irq(irqd->parent_data);	/* [한국어] 이미 활성화된 안쪽을 되돌린다. 재귀의 각 단계가 자기 몫만 되돌리면 사슬 전체가 자연스럽게 정리된다 */
		}
	}
	return ret;	/* [한국어] 0 또는 어느 층의 오류 */
}

/**
 * irq_domain_activate_irq - Call domain_ops->activate recursively to activate
 *			     interrupt
 * @irq_data:	Outermost irq_data associated with interrupt
 * @reserve:	If set only reserve an interrupt vector instead of assigning one
 *
 * This is the second step to call domain_ops->activate to program interrupt
 * controllers, so the interrupt could actually get delivered.
 */
/*
 * [한국어]
 * irq_domain_activate_irq - 인터럽트를 활성화한다 (두 번째 단계)
 *
 * @irq_data: 가장 바깥 층의 irq_data
 * @reserve:  참이면 진짜 벡터 대신 예약만 한다
 * @return:   0 성공, 음수 오류
 *
 * 인터럽트 설정의 두 번째 단계다. 원본 주석대로, 첫 단계
 * (__irq_domain_alloc_irqs)가 자원을 확보했고 이 단계가 그 자원으로
 * 하드웨어를 프로그래밍한다. 이 함수가 끝나야 인터럽트가 실제로
 * 전달된다.
 *
 * 중복 활성화를 막는 것이 이 얇은 껍데기의 존재 이유다.
 * IRQD_ACTIVATED 플래그를 확인하고, 성공하면 세운다. 두 번 활성화하면
 * 벡터가 이중으로 배정되거나 메시지가 두 번 쓰인다.
 *
 * 실패 시에도 플래그를 세우지 않는 것에 주목: `if (!ret)` 조건이
 * 그것이다. 그런데 이미 활성화된 인터럽트를 다시 활성화하려 하면
 * 첫 조건에서 걸러져 ret 이 0 인 채로 남고, 결국 플래그가 다시
 * 세워진다 — 무해한 중복이다.
 *
 * reserve 인자는 예약 모드 전용이다. kernel/irq/msi.c 의
 * msi_init_virq() 가 이 값으로 더미 벡터를 요청한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_activate() (kernel/irq/chip.c) / msi_init_virq() → [이 함수] →
 *   __irq_domain_activate_irq()
 */
int irq_domain_activate_irq(struct irq_data *irq_data, bool reserve)
{
	int ret = 0;	/* [한국어] 결과 */

	if (!irqd_is_activated(irq_data))	/* [한국어] 아직 활성화되지 않았는가. 두 번 하면 벡터가 이중 배정되거나 메시지가 두 번 쓰인다 */
		ret = __irq_domain_activate_irq(irq_data, reserve);	/* [한국어] 계층을 안쪽부터 활성화한다 */
	if (!ret)	/* [한국어] 성공했는가 */
		irqd_set_activated(irq_data);	/* [한국어] 표시를 세운다. 예약 모드에서는 호출자가 이것을 곧바로 지워 request_irq 때 다시 활성화되게 한다 */
	return ret;	/* [한국어] 0 이면 계층 전체가 활성화되어 인터럽트가 실제로 전달될 수 있다 */
}

/**
 * irq_domain_deactivate_irq - Call domain_ops->deactivate recursively to
 *			       deactivate interrupt
 * @irq_data: outermost irq_data associated with interrupt
 *
 * It calls domain_ops->deactivate to program interrupt controllers to disable
 * interrupt delivery.
 */
/*
 * [한국어]
 * irq_domain_deactivate_irq - 인터럽트를 비활성화한다
 *
 * @irq_data: 가장 바깥 층의 irq_data
 * @return:   없음
 *
 * 위 활성화의 반대다. 마찬가지로 중복을 막는 얇은 껍데기다.
 *
 * 플래그를 확인하고 지우는 것이 중요한 이유: 활성화된 적 없는
 * 인터럽트를 비활성화하려 하면 각 층의 콜백이 배정한 적 없는 자원을
 * 반납하려 든다. 벡터 도메인이라면 배정되지 않은 벡터를 회수해
 * 다른 인터럽트의 것을 건드릴 수 있다.
 *
 * 이 검사 덕분에 조건 없이 부를 수 있다.
 * irq_shutdown_and_deactivate() 가 그렇게 쓴다 — 시작된 적 없는
 * 인터럽트에도 조건 없이 부른다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_shutdown_and_deactivate() / __msi_domain_free_irqs() →
 *   [이 함수] → __irq_domain_deactivate_irq()
 */
void irq_domain_deactivate_irq(struct irq_data *irq_data)
{
	if (irqd_is_activated(irq_data)) {	/* [한국어] 활성화된 적이 있는가. 없는데 비활성화하면 배정한 적 없는 자원을 반납하려 들어, 다른 인터럽트의 것을 건드릴 수 있다 */
		__irq_domain_deactivate_irq(irq_data);	/* [한국어] 계층을 바깥부터 비활성화한다 */
		irqd_clr_activated(irq_data);	/* [한국어] 표시를 지운다. 이 검사 덕분에 호출자가 조건 없이 부를 수 있다 */
	}
}

/*
 * [한국어]
 * irq_domain_check_hierarchy - 계층형 도메인인지 판별해 플래그를 세운다
 *
 * @domain: 대상 도메인
 * @return: 없음
 *
 * 판별 기준이 alloc 콜백의 유무 하나다. 계층형 도메인은 반드시
 * 그것을 제공해야 하고, 평평한 도메인은 map 콜백을 쓴다.
 *
 * 이 플래그가 갈라 놓는 것: irq_create_fwspec_mapping() 이 계층형
 * 할당 경로로 갈지 평평한 사상 경로로 갈지, irq_dispose_mapping() 이
 * 어느 해제 경로를 쓸지가 이 플래그로 정해진다.
 *
 * 도메인 생성 마지막에 불리는 것에 주목: ops 가 이미 설정된 뒤여야
 * 판별할 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 도메인 생성 경로.
 *
 * 호출 체인:
 *   __irq_domain_create() → [이 함수]
 */
static void irq_domain_check_hierarchy(struct irq_domain *domain)
{
	/* Hierarchy irq_domains must implement callback alloc() */
	if (domain->ops->alloc)	/* [한국어] (위 영어 주석) 판별 기준이 이 하나다. 계층형은 alloc 을, 평평한 도메인은 map 을 쓴다 */
		domain->flags |= IRQ_DOMAIN_FLAG_HIERARCHY;	/* [한국어] 이 플래그가 할당·해제 경로를 가른다 */
}
#else	/* CONFIG_IRQ_DOMAIN_HIERARCHY */	/* [한국어] 계층형을 뺀 빌드 — 아래는 최소한의 대체 구현이다 */
/*
 * irq_domain_get_irq_data - Get irq_data associated with @virq and @domain
 * @domain:	domain to match
 * @virq:	IRQ number to get irq_data
 */
/*
 * [한국어]
 * irq_domain_get_irq_data - 도메인의 irq_data 를 얻는다 (비계층형 판)
 *
 * @domain: 찾을 도메인
 * @virq:   리눅스 인터럽트 번호
 * @return: 그 도메인의 irq_data, 아니면 NULL
 *
 * 계층이 없으므로 층이 하나뿐이다. 사슬을 훑을 필요 없이 그 하나가
 * 찾는 도메인의 것인지만 확인한다.
 *
 * 계층형 판과 같은 이름·시그니처인 것이 중요하다. 호출자가 어느
 * 빌드인지 몰라도 되게 하려는 것이다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   irqchip 드라이버 → [이 함수]
 */
struct irq_data *irq_domain_get_irq_data(struct irq_domain *domain,
					 unsigned int virq)
{
	struct irq_data *irq_data = irq_get_irq_data(virq);	/* [한국어] 층이 하나뿐이다 */

	return (irq_data && irq_data->domain == domain) ? irq_data : NULL;	/* [한국어] 그 하나가 찾는 도메인의 것인지만 확인한다 */
}
EXPORT_SYMBOL_GPL(irq_domain_get_irq_data);	/* [한국어] 계층형 판과 같은 이름이라 호출자가 빌드를 몰라도 된다 */

/*
 * irq_domain_set_info - Set the complete data for a @virq in @domain
 * @domain:		Interrupt domain to match
 * @virq:		IRQ number
 * @hwirq:		The hardware interrupt number
 * @chip:		The associated interrupt chip
 * @chip_data:		The associated interrupt chip data
 * @handler:		The interrupt flow handler
 * @handler_data:	The interrupt flow handler data
 * @handler_name:	The interrupt handler name
 */
/*
 * [한국어]
 * irq_domain_set_info - 인터럽트 정보를 한 번에 설정한다 (비계층형 판)
 *
 * @domain:       대상 도메인 (쓰지 않는다)
 * @virq:         리눅스 인터럽트 번호
 * @hwirq:        하드웨어 번호 (쓰지 않는다)
 * @chip:         칩
 * @chip_data:    칩 사설 데이터
 * @handler:      흐름 처리기
 * @handler_data: 처리기 사설 데이터
 * @handler_name: 표시 이름
 * @return:       없음
 *
 * 계층형 판과 이름은 같지만 구현이 다르다. 층이 하나뿐이라
 * irq_domain_get_irq_data 로 찾을 필요 없이 서술자 API 로 직접
 * 설정한다.
 *
 * hwirq 를 무시하는 것에 주목: 평평한 도메인에서는 사상을 만들 때
 * irq_domain_associate_locked() 가 이미 채웠다. 여기서 다시 설정할
 * 이유가 없다.
 *
 * EXPORT 가 없는 것도 계층형 판과 다르다. 이 빌드에서는 모듈이
 * 이 함수를 쓰지 않는다는 판단이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_map_generic_chip() → [이 함수]
 */
void irq_domain_set_info(struct irq_domain *domain, unsigned int virq,
			 irq_hw_number_t hwirq, const struct irq_chip *chip,
			 void *chip_data, irq_flow_handler_t handler,
			 void *handler_data, const char *handler_name)
{
	irq_set_chip_and_handler_name(virq, chip, handler, handler_name);	/* [한국어] 층이 하나뿐이라 서술자 API 로 직접 설정한다 */
	irq_set_chip_data(virq, chip_data);	/* [한국어] 칩 사설 데이터 */
	irq_set_handler_data(virq, handler_data);	/* [한국어] 처리기 사설 데이터. hwirq 는 사상을 만들 때 이미 채워져 무시한다 */
}

/*
 * [한국어]
 * irq_domain_alloc_irqs_locked - 계층형 할당 (비계층형 판, 항상 실패)
 *
 * @domain:   무시
 * @irq_base: 무시
 * @nr_irqs:  무시
 * @node:     무시
 * @arg:      무시
 * @realloc:  무시
 * @affinity: 무시
 * @return:   항상 -EINVAL
 *
 * 계층형 할당을 지원하지 않는 빌드다. 호출부를 #ifdef 로 나누지
 * 않으려고 항상 실패하는 판을 둔다.
 *
 * 실제로는 이 함수가 불릴 일이 없다. 호출자들이
 * irq_domain_is_hierarchy() 로 먼저 걸러 내는데, 그 매크로가 이
 * 빌드에서는 항상 거짓이기 때문이다.
 *
 * 호출 체인:
 *   irq_create_fwspec_mapping() (도달하지 않음) → [이 함수]
 */
static int irq_domain_alloc_irqs_locked(struct irq_domain *domain, int irq_base,
					unsigned int nr_irqs, int node, void *arg,
					bool realloc, const struct irq_affinity_desc *affinity)
{
	return -EINVAL;	/* [한국어] 실제로는 불리지 않는다. 호출자가 irq_domain_is_hierarchy 로 먼저 걸러 내고, 그 매크로가 이 빌드에서는 항상 거짓이다 */
}

/*
 * [한국어]
 * irq_domain_check_hierarchy - 계층형 판별 (비계층형 판, 빈 함수)
 *
 * @domain: 무시
 * @return: 없음
 *
 * 이 빌드에는 계층형 도메인이 없으므로 세울 플래그도 없다.
 *
 * 호출 체인:
 *   __irq_domain_create() → [이 함수]
 */
static void irq_domain_check_hierarchy(struct irq_domain *domain) { }	/* [한국어] 세울 플래그가 없다 */
/*
 * [한국어]
 * irq_domain_free_one_irq - 계층형 인터럽트 하나 해제 (비계층형 판, 빈 함수)
 *
 * @domain: 무시
 * @virq:   무시
 * @return: 없음
 *
 * 계층형 인터럽트가 존재하지 않으므로 해제할 것도 없다.
 * irq_dispose_mapping() 의 계층형 갈래는 이 빌드에서 도달하지 않는다.
 *
 * 호출 체인:
 *   irq_dispose_mapping() (도달하지 않음) → [이 함수]
 */
static void irq_domain_free_one_irq(struct irq_domain *domain, unsigned int virq) { }	/* [한국어] 계층형 인터럽트가 없으므로 해제할 것도 없다 */

#endif	/* CONFIG_IRQ_DOMAIN_HIERARCHY */	/* [한국어] 계층형 분기의 끝 */

#ifdef CONFIG_GENERIC_IRQ_DEBUGFS	/* [한국어] /sys/kernel/debug/irq/domains/ 를 만드는 빌드 */
#include "internals.h"	/* [한국어] irq_bit_descr, BIT_MASK_DESCR, irq_debug_show_bits — 플래그를 사람이 읽을 이름으로 찍는 헬퍼. 파일 중간에 include 하는 것은 이 구역에서만 필요해서다 */

static struct dentry *domain_dir;
/* [한국어] /sys/kernel/debug/irq/domains 디렉터리.
 * 설정자: irq_domain_debugfs_init() 이 부팅 중 한 번 만든다.
 * 읽는 자: debugfs_add_domain_dir(), debugfs_remove_domain_dir().
 * 값 범위: NULL(아직 안 만들어짐) 또는 유효한 dentry.
 * 동기화: 초기화 후 바뀌지 않는다. NULL 검사가 부팅 초기에
 *   만들어진 도메인의 항목 생성을 건너뛰게 하고, 나중에
 *   irq_domain_debugfs_init() 이 몰아서 만든다 — 이 파일이
 *   MSI 저장소나 sysfs 에서 쓰는 것과 같은 패턴이다. */

static const struct irq_bit_descr irqdomain_flags[] = {
	/* [한국어] 도메인 플래그 비트를 사람이 읽을 이름으로 옮기는 표.
	 * BIT_MASK_DESCR 매크로가 상수 이름을 문자열로 만들어 주므로,
	 * 새 플래그가 추가되면 이 표에 한 줄만 더하면 된다.
	 * debugfs 출력에서 "flags: 0x00000021" 만으로는 알 수 없는 것을
	 * 이름으로 풀어 준다. */
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_HIERARCHY),
	/* [한국어] 계층형 도메인 표식.
	 * 설정자: irq_domain_check_hierarchy() 가 alloc 콜백 유무로.
	 * 읽는 자: irq_debug_show_bits().
	 * 값 범위: 이 표의 모든 항목이 (비트값, 이름) 쌍이다.
	 * 동기화: const 배열이라 변경되지 않는다. */
	BIT_MASK_DESCR(IRQ_DOMAIN_NAME_ALLOCATED),
	/* [한국어] 이름이 힙에 있어 해제해야 한다는 표식.
	 * 설정자: alloc_name() 계열.
	 * 읽는 자·값 범위·동기화: 위와 같다. */
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_IPI_PER_CPU),
	/* [한국어] CPU 마다 별개의 IPI 를 쓰는 도메인.
	 * 설정자: kernel/irq/ipi.c 의 도메인 생성 경로.
	 * 읽는 자·값 범위·동기화: 위와 같다. */
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_IPI_SINGLE),
	/* [한국어] 모든 CPU 가 하나의 IPI 를 공유하는 도메인.
	 * 설정자: 위와 같은 경로. IPI_PER_CPU 와 배타적이다.
	 * 읽는 자·값 범위·동기화: 위와 같다. */
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_MSI),
	/* [한국어] MSI 도메인 표식.
	 * 설정자: __msi_create_irq_domain() 이 항상 붙인다.
	 * 읽는 자·값 범위·동기화: 위와 같다. */
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_ISOLATED_MSI),
	/* [한국어] 이 층이 MSI 벡터 권한을 검사한다는 표식.
	 * 설정자: 인터럽트 리매핑 드라이버.
	 * 읽는 자: irq_debug_show_bits() 와 msi_device_has_isolated_msi().
	 * 값 범위·동기화: 위와 같다. VFIO 보안에 직접 영향을 준다. */
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_NO_MAP),
	/* [한국어] 역방향 맵을 쓰지 않는 직접 사상 도메인.
	 * 설정자: __irq_domain_create() 가 direct_max 를 보고.
	 * 읽는 자·값 범위·동기화: 위와 같다. */
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_MSI_PARENT),
	/* [한국어] 장치별 MSI 도메인의 부모가 될 수 있다는 표식.
	 * 설정자: msi_create_parent_irq_domain().
	 * 읽는 자·값 범위·동기화: 위와 같다. */
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_MSI_DEVICE),
	/* [한국어] 장치 전용 MSI 도메인 표식.
	 * 설정자: msi_create_device_irq_domain().
	 * 읽는 자·값 범위·동기화: 위와 같다. */
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_NONCORE),
	/* [한국어] 코어가 아닌 곳에서 정의한 플래그의 시작 경계.
	 * 설정자: 드라이버가 이 비트 이상을 자기 용도로 쓴다.
	 * 읽는 자·값 범위·동기화: 위와 같다.
	 * 이름이 표식이라기보다 경계인 것에 주목 — 이 비트 위쪽은
	 *   도메인마다 뜻이 달라 코어가 해석하지 않는다. */
};

/*
 * [한국어]
 * irq_domain_debug_show_one - 도메인 하나의 정보를 출력한다 (재귀)
 *
 * @m:   seq_file 출력 대상
 * @d:   대상 도메인
 * @ind: 들여쓰기 칸 수
 * @return: 없음
 *
 * 도메인의 이름·크기·사상 수·플래그를 찍고, 계층형이면 부모를
 * 재귀적으로 더 깊이 들여써서 찍는다. 그래서 출력이 계층 구조를
 * 그대로 보여 준다.
 *
 * 들여쓰기가 4 씩 늘어나는 것에 주목: 부모마다 한 단계 안으로
 * 들어가, 계층 깊이가 눈에 보인다.
 *
 * 도메인의 debug_show 콜백을 부르는 것도 있다. MSI 도메인이 그것으로
 * 메시지 주소와 데이터를 덧붙인다 (kernel/irq/msi.c).
 *
 * irqd 를 NULL 로 넘기는 것에 주목: 그 콜백은 인터럽트별 출력에도
 * 쓰이는데, 여기서는 도메인 자체의 정보만 원하므로 NULL 이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, debugfs 읽기.
 *
 * 호출 체인:
 *   irq_domain_debug_show() → [이 함수] → (재귀) → d->ops->debug_show()
 */
static void irq_domain_debug_show_one(struct seq_file *m, struct irq_domain *d, int ind)
{
	seq_printf(m, "%*sname:   %s\n", ind, "", d->name);	/* [한국어] 도메인 이름. debugfs 파일 이름과 같다 */
	seq_printf(m, "%*ssize:   %u\n", ind + 1, "", d->revmap_size);	/* [한국어] 선형 역방향 배열의 크기. 0 이면 모든 조회가 래딕스 트리로 간다 */
	seq_printf(m, "%*smapped: %u\n", ind + 1, "", d->mapcount);	/* [한국어] 현재 살아 있는 사상 수. 도메인을 없앨 때 0 이어야 한다 */
	seq_printf(m, "%*sflags:  0x%08x\n", ind +1 , "", d->flags);	/* [한국어] 원시 플래그 값. 아래에서 이름으로도 풀어 준다 */
	irq_debug_show_bits(m, ind, d->flags, irqdomain_flags, ARRAY_SIZE(irqdomain_flags));	/* [한국어] 위 표를 써서 세워진 비트의 이름을 나열한다. 16진수만으로는 알 수 없는 것을 보여 준다 */
	if (d->ops && d->ops->debug_show)	/* [한국어] 도메인 고유의 추가 정보가 있는가 */
		d->ops->debug_show(m, d, NULL, ind + 1);	/* [한국어] irqd 를 NULL 로 — 그 콜백은 인터럽트별 출력에도 쓰이는데 여기서는 도메인 자체만 원한다 */
#ifdef	CONFIG_IRQ_DOMAIN_HIERARCHY	/* [한국어] 계층형 빌드에만 parent 필드가 있다 */
	if (!d->parent)	/* [한국어] 계층의 가장 안쪽인가 */
		return;	/* [한국어] 재귀 종료 */
	seq_printf(m, "%*sparent: %s\n", ind + 1, "", d->parent->name);	/* [한국어] 부모 이름 */
	irq_domain_debug_show_one(m, d->parent, ind + 4);	/* [한국어] 4 칸 더 들여써서 재귀. 출력이 계층 구조를 그대로 보여 준다 */
#endif
}

/*
 * [한국어]
 * irq_domain_debug_show - debugfs 파일 하나의 내용을 만든다
 *
 * @m: seq_file 출력 대상 (private 에 도메인이 들어 있다)
 * @p: 쓰지 않는다
 * @return: 항상 0
 *
 * debugfs 파일을 읽을 때 불리는 show 콜백이다. 파일마다 다른 도메인이
 * private 에 연결돼 있다.
 *
 * private 이 NULL 인 특별한 경우가 있다. "default" 라는 이름의
 * 파일인데, 아래 irq_domain_debugfs_init() 이 도메인 없이 만든다.
 * 그 파일을 읽으면 현재 기본 도메인의 정보가 나온다 — 기본 도메인이
 * 바뀌어도 같은 파일로 볼 수 있게 하려는 것이다.
 *
 * 기본 도메인이 없으면 빈 파일이 되는 것에 주목: 오류가 아니라
 * "설정된 기본 도메인이 없다" 는 표현이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, debugfs 읽기.
 *
 * 호출 체인:
 *   cat /sys/kernel/debug/irq/domains/<이름> → seq_file → [이 함수]
 */
static int irq_domain_debug_show(struct seq_file *m, void *p)
{
	struct irq_domain *d = m->private;	/* [한국어] 파일마다 연결된 도메인 */

	/* Default domain? Might be NULL */
	if (!d) {	/* [한국어] (위 영어 주석) "default" 파일인가 — 도메인 없이 만들어진다 */
		if (!irq_default_domain)	/* [한국어] 설정된 기본 도메인이 있는가 */
			return 0;	/* [한국어] 빈 파일. 오류가 아니라 "없다" 는 표현이다 */
		d = irq_default_domain;	/* [한국어] 읽는 시점의 기본 도메인. 바뀌어도 같은 파일로 볼 수 있다 */
	}
	irq_domain_debug_show_one(m, d, 0);	/* [한국어] 들여쓰기 0 부터 시작해 계층을 따라 출력한다 */
	return 0;	/* [한국어] seq_file 규약상 0 이 성공이다 */
}
DEFINE_SHOW_ATTRIBUTE(irq_domain_debug);	/* [한국어] 위 show 함수를 감싸는 file_operations 를 만든다. irq_domain_debug_fops 라는 이름이 생기고 아래 debugfs_create_file 이 그것을 쓴다 */

/*
 * [한국어]
 * debugfs_add_domain_dir - 도메인의 debugfs 파일을 만든다
 *
 * @d: 대상 도메인
 * @return: 없음
 *
 * 이름이 dir 이지만 실제로는 파일 하나를 만든다. 도메인당 디렉터리를
 * 두던 옛 구조의 이름이 남은 것이다.
 *
 * 두 NULL 검사가 각각 다른 상황을 처리한다. 이름이 없으면 파일
 * 이름을 지을 수 없고, domain_dir 이 없으면 아직 부팅 초기라
 * 상위 디렉터리가 만들어지지 않았다. 후자는 나중에
 * irq_domain_debugfs_init() 이 몰아서 처리한다.
 *
 * 실패를 검사하지 않는 것에 주목: debugfs 는 진단용이라 실패해도
 * 인터럽트 동작에 영향이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, irq_domain_mutex 보유.
 *
 * 호출 체인:
 *   __irq_domain_publish() / irq_domain_update_bus_token() /
 *   irq_domain_debugfs_init() → [이 함수]
 */
static void debugfs_add_domain_dir(struct irq_domain *d)
{
	if (!d->name || !domain_dir)	/* [한국어] 이름이 없으면 파일 이름을 지을 수 없고, 상위 디렉터리가 없으면 아직 부팅 초기다 */
		return;	/* [한국어] 후자는 나중에 irq_domain_debugfs_init 이 몰아서 처리한다 */
	debugfs_create_file(d->name, 0444, domain_dir, d,	/* [한국어] 읽기 전용. 도메인 포인터를 private 로 넘겨 show 콜백이 쓴다 */
			    &irq_domain_debug_fops);	/* [한국어] 실패를 검사하지 않는다. 진단용이라 없어도 인터럽트는 정상 동작한다 */
}

/*
 * [한국어]
 * debugfs_remove_domain_dir - 도메인의 debugfs 파일을 지운다
 *
 * @d: 대상 도메인
 * @return: 없음
 *
 * 이름으로 찾아 지운다. dentry 포인터를 따로 저장해 두지 않는 것이
 * 이 방식의 특징이다 — 도메인 구조체에 필드를 하나 아끼는 대신
 * 지울 때 검색 비용을 치른다. 도메인 제거가 드문 일이라 합리적인
 * 맞바꿈이다.
 *
 * 이름이 NULL 이거나 domain_dir 이 없어도 안전하다 —
 * debugfs_lookup_and_remove 가 그것을 처리한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, irq_domain_mutex 보유.
 *
 * 호출 체인:
 *   irq_domain_remove() / irq_domain_update_bus_token() → [이 함수]
 */
static void debugfs_remove_domain_dir(struct irq_domain *d)
{
	debugfs_lookup_and_remove(d->name, domain_dir);	/* [한국어] dentry 를 저장하지 않고 이름으로 찾는다. 도메인 구조체의 필드를 아끼는 대신 검색 비용을 치르는데, 제거가 드문 일이라 합리적이다 */
}

/*
 * [한국어]
 * irq_domain_debugfs_init - domains 디렉터리를 만들고 기존 도메인을 등록한다
 *
 * @root: /sys/kernel/debug/irq 디렉터리
 * @return: 없음
 *
 * 이 파일의 닭과 달걀 문제를 푸는 함수다. 부팅 초기에 만들어진
 * 도메인들은 debugfs 가 준비되기 전이라 항목을 만들지 못했다. 여기서
 * 뒤늦게 몰아서 만든다. kernel/irq/irqdesc.c 의 irq_sysfs_init() 과
 * 같은 패턴이다.
 *
 * "default" 파일을 도메인 없이 만드는 것에 주목: private 이 NULL 이라
 * show 콜백이 읽는 시점의 기본 도메인을 보여 준다. 기본 도메인이
 * 바뀌어도 파일을 다시 만들 필요가 없다.
 *
 * 뮤텍스를 잡는 이유: 이 함수가 도는 동안 다른 CPU 가 도메인을
 * 만들거나 없앨 수 있다. 락이 없으면 그 도메인이 두 번 등록되거나
 * 아예 빠진다. 락을 잡으면 새 도메인은 이 함수가 끝난 뒤 진행되고,
 * 그때는 domain_dir 이 이미 설정돼 정상 경로로 등록된다.
 *
 * domain_dir 대입이 락 밖에 있는 것이 미묘하다 — 그 사이에 다른
 * CPU 가 도메인을 만들면 정상 경로로 등록되고, 아래 루프가 그것을
 * 또 등록하려 할 수 있다. 실제로는 이 함수가 부팅 초기에 불려
 * 문제가 되지 않는다.
 *
 * 실행 컨텍스트: 부팅 중, 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_debugfs_init() (kernel/irq/debugfs.c) → [이 함수]
 */
void __init irq_domain_debugfs_init(struct dentry *root)
{
	struct irq_domain *d;	/* [한국어] 순회용 */

	domain_dir = debugfs_create_dir("domains", root);	/* [한국어] /sys/kernel/debug/irq/domains. 이 대입 뒤로 새 도메인이 정상 경로로 등록된다 */

	debugfs_create_file("default", 0444, domain_dir, NULL,	/* [한국어] 도메인 없이 만든다. private 이 NULL 이라 show 콜백이 읽는 시점의 기본 도메인을 보여 준다 */
			    &irq_domain_debug_fops);
	mutex_lock(&irq_domain_mutex);	/* [한국어] 순회 중에 도메인이 생기거나 사라지면 두 번 등록되거나 빠진다 */
	list_for_each_entry(d, &irq_domain_list, link)	/* [한국어] 디렉터리가 없던 시절에 만들어진 도메인들 */
		debugfs_add_domain_dir(d);	/* [한국어] 이제 domain_dir 이 있으므로 실제로 만들어진다 */
	mutex_unlock(&irq_domain_mutex);	/* [한국어] 뒤늦은 등록이 끝났다. 이 뒤로는 새 도메인이 정상 경로로 항목을 만든다 */
}
#endif	/* [한국어] CONFIG_GENERIC_IRQ_DEBUGFS 분기의 끝 */
