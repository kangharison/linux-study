// SPDX-License-Identifier: GPL-2.0+
/*
 * Interface for Dynamic Logical Partitioning of I/O Slots on
 * RPA-compliant PPC64 platform.
 *
 * John Rose <johnrose@austin.ibm.com>
 * Linda Xie <lxie@us.ibm.com>
 *
 * October 2003
 *
 * Copyright (C) 2003 IBM.
 */

/*
 * [한국어 설명] RPA DLPAR I/O 슬롯 인터페이스의 본체 (rpadlpar_core.c)
 *
 * === 파일의 역할 ===
 * IBM POWER 의 RPA 규격을 따르는 PPC64 시스템에서 I/O 슬롯을 논리 파티션에
 * 통째로 붙였다 떼는 DLPAR(Dynamic Logical Partitioning) 동작을 구현한다.
 * 이름 하나(DRC 이름 문자열)를 받아, 그 이름의 장치가 디바이스 트리 어디에
 * 있는지 찾고, 종류에 맞는 절차로 커널에 등장시키거나 사라지게 하는 것이 전부다.
 * 다루는 종류가 셋이다. (1) VIO — 하이퍼바이저가 제공하는 가상 I/O 장치.
 * (2) SLOT — PHB 아래의 PCI 슬롯. (3) PHB — PCI 호스트 브리지 자체.
 * 셋의 절차가 완전히 달라서 add 도 remove 도 종류별로 함수가 하나씩 있고,
 * 위쪽의 dlpar_add_slot() / dlpar_remove_slot() 이 종류를 판별해 갈라 준다.
 * PCI 쪽이 가장 복잡하다. 슬롯을 붙일 때는 EEH 자료구조를 만들고, 브리지 장치를
 * 만들어 PHB 버스에 달고, 그 아래를 스캔하고, IO 공간을 매핑하고, 자원 배정을
 * 마친 뒤, 마지막에 rpaphp 에 핫플러그 슬롯 등록까지 요청한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 같은 디렉터리의 rpaphp_* 파일들과 계층 관계를 이룬다.
 *   사용자/하이퍼바이저 도구
 *     → rpadlpar_sysfs.c (DRC 이름 문자열을 받는 sysfs 통로)
 *     → [이 파일] (파티션에 슬롯을 넣고 빼는 상위 동작)
 *     → rpaphp_core.c (그 슬롯 하나의 핫플러그)
 *     → pci_hotplug_core.c (sysfs 슬롯 노출)
 * 즉 rpaphp 가 '이미 파티션에 있는 슬롯에 카드를 꽂고 빼는' 일을 맡는다면,
 * 이 파일은 '슬롯 자체를 파티션에 넣고 빼는' 한 단계 위의 일을 맡는다.
 * 두 계층을 잇는 접점은 세 개뿐이며 모두 rpaphp_core.c 가 내보낸 것이다 —
 * rpaphp_check_drc_props()(이름으로 노드를 찾을 때), rpaphp_add_slot()(붙인 뒤
 * 핫플러그 슬롯을 만들 때), 그리고 전역 리스트 rpaphp_slot_head(뗄 슬롯을
 * 찾을 때). 반대로 rpaphp 쪽은 이 파일의 어떤 이름도 참조하지 않는다.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. 진입점 두 개가 뮤텍스를 잡고,
 * 그 안에서 RTAS 호출과 PCI 열거·제거가 일어나 여러 번 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: rpadlpar.h 가 선언한 dlpar_add_slot()/dlpar_remove_slot() 을 구현하고,
 * rpadlpar_sysfs.c 가 그 둘을 부른다. 모듈 초기화도 그쪽의 dlpar_sysfs_init() 을
 * 불러 sysfs 통로를 연다.
 * 옆쪽: rpaphp.h 의 struct slot 과 위에 적은 세 접점.
 * 아래쪽: PowerPC 전용 인터페이스가 대부분이다 — RTAS(rtas_token),
 * VIO 버스(vio_find_node, vio_register_device_node, vio_unregister_device),
 * PHB 관리(init_phb_dynamic, remove_phb_dynamic), PowerPC 의 PCI 열거
 * (of_create_pci_dev, of_scan_pci_bridge, pcibios_map_io_space,
 * pcibios_unmap_io_space, pcibios_finish_adding_to_bus, pci_find_bus_by_node,
 * pci_device_to_OF_node, PCI_DN), 그리고 EEH(pseries_eeh_init_edev_recursive).
 * 이들의 구현은 모두 arch/powerpc 에 있고 그 디렉터리가 이 트리에 없어
 * 확인 대상 밖이다 — 아래 주석에서는 전부 호출 자리의 쓰임새로만 설명했다.
 * PCI 코어 쪽에서 쓰는 것은 pci_hp_remove_devices() 와
 * pci_stop_and_remove_bus_device(), 그리고 재스캔 락 한 쌍이다.
 * 데이터 흐름: DRC 이름 문자열 → 디바이스 트리 노드 → 종류 판별 →
 * 종류별 등장/제거 절차 → PCI 장치 또는 VIO 장치가 커널에 나타나거나 사라짐.
 *
 * === 주요 함수/구조체 요약 ===
 * 이 파일에는 구조체 정의가 없다. 전역도 뮤텍스 하나뿐이다.
 * - dlpar_add_slot() / dlpar_remove_slot(): 두 개의 공개 진입점. 뮤텍스를 잡고,
 *   이름으로 노드를 찾고, 종류에 따라 갈라 준다. 구조가 서로 거울처럼 대칭이다.
 * - find_dlpar_node(): DRC 이름으로 노드를 찾고 종류까지 알려 준다. SLOT →
 *   PHB → VIO 순으로 시도하므로 그 순서가 곧 우선순위다.
 * - find_php_slot_pci_node() / find_vio_slot_node(): 위 함수가 쓰는 두 탐색기.
 *   둘 다 rpaphp_check_drc_props() 로 노드마다 이름을 대조한다.
 * - find_php_slot(): rpaphp 의 전역 슬롯 리스트에서 이 노드의 슬롯을 찾는다.
 * - dlpar_pci_add_bus() / dlpar_add_pci_slot(): PCI 슬롯을 붙이는 본체.
 *   브리지 장치 생성 → 하위 스캔 → IO 매핑 → 자원 배정 → 핫플러그 등록.
 * - dlpar_remove_pci_slot(): 그 반대. 핫플러그 등록 해제 → 장치 제거 →
 *   IO 매핑 해제 → 브리지 장치 제거.
 * - dlpar_add_phb() / dlpar_remove_phb(): PHB 통째로 붙이고 떼기.
 * - dlpar_add_vio_slot() / dlpar_remove_vio_slot(): VIO 장치 등록·해제.
 *   셋 중 가장 단순하다 — VIO 버스가 알아서 해 준다.
 * - is_dlpar_capable(): 이 파티션이 DLPAR 을 지원하는지 RTAS 토큰으로 확인한다.
 *
 * === 이 트리에서 확인할 수 없는 것 ===
 * 이 저장소는 sparse checkout 이라 drivers/{block,nvme,pci,s390,vfio} 만 있고
 * arch/powerpc 가 없다. 따라서 RTAS 서비스의 의미와 반환 규약, VIO 버스의 등록
 * 절차, PHB 를 동적으로 만들고 없애는 코드, PowerPC 전용 PCI 열거 함수들,
 * PCI_DN 매크로의 정의, EEH 자료구조의 내용은 모두 확인 대상 밖이다.
 * 아래에서 그 이름들이 나올 때는 '이 함수가 무엇을 해 준다고 이 파일이 기대하는가'
 * 만 적었고, 펌웨어나 아키텍처 코드가 실제로 무엇을 하는지는 단정하지 않았다.
 * 이 배정에서 그 표기가 자주 나오는 것은 이 파일 자체가 아키텍처 코드에 대한
 * 얇은 조율 계층이기 때문이다.
 */

/* [한국어] DEBUG 매크로를 해제한다. 이 파일은 아래에서 pr_debug 를 세 번 쓰는데,
 * DEBUG 가 정의되어 있으면 그것이 실제 출력이 되고 아니면 조용해진다. 즉 이 한 줄이
 * '기본은 조용히' 를 못박는 장치다. 커널 빌드 시스템이 파일 단위로 DEBUG 를 켤 수
 * 있게 되어 있어, 그렇게 켜진 것을 여기서 되돌리는 셈이다.
 * 정확한 치환 규칙은 include/linux/printk.h 가 이 트리에 없어 확인 못 함. */
#undef DEBUG

/* [한국어] __init 과 __exit 표시를 위해 포함한다. 모듈 진입·종료 함수에 붙는다. */
#include <linux/init.h>
/* [한국어] module_init/module_exit 와 MODULE_ 계열 매크로. 이 파일이 별도 모듈
 * (rpadlpar_io)의 진입점을 갖는다 — rpaphp 와 다른 모듈이라는 점이,
 * rpaphp 쪽이 심볼을 EXPORT_SYMBOL_GPL 로 내보내야 하는 이유다. */
#include <linux/module.h>
/* [한국어] 디바이스 트리 API — of_find_node_by_name(), for_each_child_of_node(),
 * for_each_node_by_name(), of_node_put(). DRC 이름으로 노드를 찾는 것이
 * 이 파일의 첫 단계라 가장 많이 쓰이는 헤더다. */
#include <linux/of.h>
/* [한국어] struct pci_dev / pci_bus, pci_stop_and_remove_bus_device(),
 * 재스캔 락, pci_is_bridge(), pci_name(), pci_domain_nr() 등 PCI 코어 API. */
#include <linux/pci.h>
/* [한국어] 문자열 함수 헤더. [상류 코드 관찰] 이 파일에서 strcmp/strlen 같은
 * 이름을 직접 쓰는 곳을 찾지 못했다. 다만 헤더가 다른 헤더를 끌어오기 위해
 * 필요할 수 있고 그 포함 관계는 include/ 가 이 트리에 없어 확인 못 함이므로,
 * 불필요하다고 단정하지 않는다. */
#include <linux/string.h>
/* [한국어] vm_unmap_aliases() 를 위해 포함한다. 슬롯을 뗀 뒤 남아 있는 vmalloc
 * 별칭 매핑을 정리하는 데 쓴다 — rpaphp_core.c 의 disable_slot() 도 같은
 * 함수를 부른다. */
#include <linux/vmalloc.h>

/* [한국어] PowerPC 의 PCI 브리지 정의. struct pci_controller(PHB 하나를 나타낸다),
 * struct pci_dn, PCI_DN() 매크로, 그리고 pci_find_bus_by_node() 같은 PowerPC
 * 전용 헬퍼가 여기서 온다. 정의는 arch/powerpc 에 있어 이 트리에서 확인 못 함. */
#include <asm/pci-bridge.h>
/* [한국어] DEFINE_MUTEX 와 뮤텍스 API. 아래 진입점 두 개가 이 파일 전체를
 * 하나의 뮤텍스로 직렬화한다. include 위치가 asm/ 헤더 사이에 끼어 있는 것은
 * 정렬 관례에서 벗어난 모양이지만 동작에는 영향이 없다. */
#include <linux/mutex.h>
/* [한국어] RTAS(Run-Time Abstraction Services) 헤더. 이 파일이 쓰는 것은
 * rtas_token() 과 RTAS_UNKNOWN_SERVICE 하나씩으로, 파티션이 DLPAR 을 지원하는지
 * 확인하는 데만 쓴다 — 실제 슬롯 조작은 아래 계층이 대신 한다. */
#include <asm/rtas.h>
/* [한국어] VIO(Virtual I/O) 버스 헤더. struct vio_dev 와 vio_find_node(),
 * vio_register_device_node(), vio_unregister_device() 가 여기서 온다.
 * VIO 는 하이퍼바이저가 제공하는 가상 장치 버스로, 이 파일이 다루는 세 종류
 * 중 하나다. 구현은 arch/powerpc 에 있어 확인 못 함. */
#include <asm/vio.h>
/* [한국어] [상류 코드 관찰] 이것은 펌웨어 이미지 로더(request_firmware 계열)의
 * 헤더이고, PowerPC 의 펌웨어 기능 질의 헤더인 asm/firmware.h 와 다른 물건이다.
 * 같은 디렉터리의 rpaphp_core.c 는 asm/ 판을 포함한다. 이 파일에서는 어느
 * 쪽 이름도 쓰이지 않아, 어느 것을 의도했는지 코드만으로는 알 수 없다. */
#include <linux/firmware.h>

/* [한국어] drivers/pci 내부 전용 헤더. pci_hp_remove_devices() 를 여기서 얻는
 * 것으로 보이나, 그 선언 위치는 이 트리에서 찾지 못했다 — drivers/pci/pci.h 에는
 * 없고 arch/powerpc 쪽으로 보이나 확인할 수 없다. */
#include "../pci.h"
/* [한국어] 아래 계층인 rpaphp 의 공용 헤더. struct slot, 전역 rpaphp_slot_head,
 * 그리고 이 파일이 부르는 rpaphp_check_drc_props()/rpaphp_add_slot()/
 * rpaphp_deregister_slot() 의 원형이 여기 있다. 두 계층을 잇는 통로다. */
#include "rpaphp.h"
/* [한국어] 이 파일이 구현할 함수들의 선언. dlpar_add_slot()/dlpar_remove_slot()
 * 과, 이 파일이 부르는 dlpar_sysfs_init()/dlpar_sysfs_exit() 이 함께 들어 있다 —
 * 즉 이 헤더 하나로 rpadlpar_core.c 와 rpadlpar_sysfs.c 가 서로를 부른다. */
#include "rpadlpar.h"

/* [한국어] 이 파일 전체를 직렬화하는 뮤텍스. 아래 두 진입점이 시작과 끝에서
 * 잡고 놓는다.
 * 설정자: 매크로가 선언과 초기화를 함께 한다.
 * 읽는 자: dlpar_add_slot() 과 dlpar_remove_slot() 뿐이다.
 * 값 범위: 초기화된 뮤텍스.
 * 동기화: 슬롯을 붙이고 떼는 절차가 겹치면 같은 노드를 두 번 등록하거나
 *   반쯤 제거된 상태를 만들 수 있으므로, 굵은 입자로 한 번에 막는다.
 *   뮤텍스라 잠들 수 있는 문맥에서만 잡을 수 있는데, 두 진입점 모두 그 안에서
 *   PCI 열거와 RTAS 호출을 하므로 애초에 잠들 수 있어야 한다.
 *   [상류 코드 관찰] 이 뮤텍스는 rpaphp 의 전역 슬롯 리스트에도 사실상의
 *   보호가 되지만, rpaphp 쪽 부팅 경로와 sysfs 경로는 이 락을 잡지 않는다. */
static DEFINE_MUTEX(rpadlpar_mutex);

/* [한국어] 로그에 붙일 모듈 이름. 아래 두 진입점의 완료 메시지에만 쓰인다. */
#define DLPAR_MODULE_NAME "rpadlpar_io"

/* [한국어] 노드 종류 — 하이퍼바이저가 제공하는 가상 I/O 장치. 셋 중 절차가
 * 가장 단순하다. */
#define NODE_TYPE_VIO  1
/* [한국어] 노드 종류 — PHB 아래의 PCI 슬롯. 절차가 가장 복잡하다. */
#define NODE_TYPE_SLOT 2
/* [한국어] 노드 종류 — PCI 호스트 브리지 자체. 슬롯이 아니라 버스 하나를
 * 통째로 붙이고 뗀다. */
#define NODE_TYPE_PHB  3

/* [한국어]
 * find_vio_slot_node - DRC 이름으로 VIO 장치 노드를 찾는다
 *
 * @drc_name: 찾을 DRC 이름.
 * @return: 참조를 하나 올린 노드, 또는 NULL(없음).
 *
 * VIO 장치들은 디바이스 트리에서 "vdevice" 노드 아래에 모여 있으므로, 먼저
 * 그 부모를 찾고 자식을 훑으며 이름을 대조한다. 대조는 아래 계층인 rpaphp 의
 * rpaphp_check_drc_props() 에 맡긴다 — DRC 속성 형식이 v1/v2 로 나뉘는 복잡함을
 * 그쪽이 이미 흡수해 두었기 때문이다. 종류는 NULL 로 넘겨 이름만 따진다.
 * 참조 카운트 규약이 이 함수의 요점이다. 순회 매크로는 반복마다 이전 자식의
 * 참조를 놓고 다음 자식의 참조를 잡으므로, 중간에 break 로 빠져나가면 그 노드의
 * 참조가 잡힌 채 남는다. 그것이 곧 반환값이고, 호출자가 다 쓴 뒤 놓아야 한다.
 * 끝까지 돌면 커서가 NULL 이 되어 그대로 '없음' 이 된다.
 * 부모 노드의 참조는 이 함수가 직접 놓는다 — 자식만 돌려주면 되기 때문이다.
 * 실행 컨텍스트: DLPAR 진입점 안(뮤텍스를 쥔 프로세스 컨텍스트).
 *
 * 호출 체인:
 *   find_dlpar_node() → [find_vio_slot_node]
 *     → of_find_node_by_name(), rpaphp_check_drc_props(), of_node_put()
 */
static struct device_node *find_vio_slot_node(char *drc_name)
{
	/* [한국어] VIO 장치들이 모여 있는 부모 노드를 찾는다. 이 호출도 참조를 하나
	 * 올리므로 아래에서 놓아야 한다. */
	struct device_node *parent = of_find_node_by_name(NULL, "vdevice");
	/* [한국어] 자식 순회 커서이자 반환값. */
	struct device_node *dn;
	/* [한국어] 이름 대조 결과. */
	int rc;

	/* [한국어] VIO 부모 노드 자체가 없다면 이 시스템에 VIO 가 없는 것이다. */
	if (!parent)
		/* [한국어] 찾지 못했다고 알린다. 부모 참조도 잡히지 않았으므로 놓을 것이 없다. */
		return NULL;

	/* [한국어] 자식들을 훑는다. 매크로가 반복마다 참조를 옮겨 잡아 준다. */
	for_each_child_of_node(parent, dn) {
		/* [한국어] 이 자식의 DRC 이름이 찾는 이름과 맞는지 아래 계층에 물어본다.
		 * 종류를 NULL 로 주어 이름만 따진다 — VIO 는 종류로 걸러 낼 필요가 없다. */
		rc = rpaphp_check_drc_props(dn, drc_name, NULL);
		/* [한국어] 맞으면 */
		if (rc == 0)
			/* [한국어] 그 노드의 참조를 쥔 채 순회를 끝낸다. 이 break 가 반환값의 참조를
			 * 남기는 지점이다. */
			break;
	}
	/* [한국어] 부모 노드 참조는 더 필요 없으므로 놓는다. 자식 참조와는 별개다. */
	of_node_put(parent);

	/* [한국어] 찾은 노드(참조 보유) 또는 NULL 을 돌려준다. */
	return dn;
}

/* [한국어]
 * find_php_slot_pci_node - DRC 이름과 종류로 PCI 노드를 찾는다
 *
 * @drc_name: 찾을 DRC 이름.
 * @drc_type: 맞아야 할 DRC 종류(이 파일에서는 "SLOT" 또는 "PHB").
 * @return: 참조를 하나 올린 노드, 또는 NULL(없음).
 *
 * 옆의 상류 주석이 목적을 밝혀 두었다. VIO 판과 구조가 같지만 두 가지가 다르다.
 * 첫째, VIO 처럼 부모 아래를 훑는 것이 아니라 트리 전체에서 이름이 "pci" 인
 * 노드를 훑는다. 둘째, 이름뿐 아니라 종류까지 대조한다 — 같은 이름이라도
 * 슬롯인지 브리지인지 갈라야 절차가 정해지기 때문이며, 그래서 호출자가 이
 * 함수를 종류를 바꿔 두 번 부른다.
 * 참조 카운트 규약은 VIO 판과 같다. 순회 매크로가 반복마다 참조를 옮겨 잡고,
 * break 로 빠져나가면 그 노드의 참조가 남아 반환값이 된다.
 * 실행 컨텍스트: DLPAR 진입점 안(뮤텍스를 쥔 프로세스 컨텍스트).
 *
 * 호출 체인:
 *   find_dlpar_node() → [find_php_slot_pci_node] → rpaphp_check_drc_props()
 */
/* Find dlpar-capable pci node that contains the specified name and type */
static struct device_node *find_php_slot_pci_node(char *drc_name,
						  char *drc_type)
{
	/* [한국어] 순회 커서이자 반환값. */
	struct device_node *np;
	/* [한국어] 이름·종류 대조 결과. */
	int rc;

	/* [한국어] 트리 전체에서 이름이 "pci" 인 노드를 훑는다. rpaphp_core.c 의 부팅
	 * 경로가 같은 매크로로 같은 노드들을 훑는다는 점에서, 두 계층이 같은 집합을
	 * 서로 다른 목적으로 보는 셈이다. */
	for_each_node_by_name(np, "pci") {
		/* [한국어] 이름과 종류가 모두 맞는지 아래 계층에 물어본다. */
		rc = rpaphp_check_drc_props(np, drc_name, drc_type);
		/* [한국어] 맞으면 */
		if (rc == 0)
			/* [한국어] 참조를 쥔 채 순회를 끝낸다. */
			break;
	}

	/* [한국어] 찾은 노드(참조 보유) 또는 NULL 을 돌려준다. 끝까지 돌면 커서가
	 * NULL 이 되어 자연스럽게 '없음' 이 된다. */
	return np;
}

/* [한국어]
 * find_dlpar_node - DRC 이름으로 노드를 찾고 그 종류까지 알려 준다
 *
 * @drc_name: 찾을 DRC 이름.
 * @node_type: 찾은 노드의 종류를 담을 곳(NODE_TYPE_ 셋 중 하나).
 * @return: 옆의 상류 주석대로 참조를 하나 올린 노드, 또는 NULL(없음).
 *
 * 두 진입점이 공유하는 첫 단계다. DLPAR 은 슬롯을 버스:장치 번호가 아니라
 * 펌웨어가 붙인 이름으로 지목하므로, 그 이름이 무엇을 가리키는지부터 알아내야
 * 한다. 종류를 함께 돌려주는 이유는 이후 절차가 종류마다 완전히 다르기 때문이다.
 * 시도 순서가 곧 우선순위다 — PCI 슬롯, PCI 호스트 브리지, VIO 순으로 찾고
 * 처음 맞는 것을 쓴다. 같은 이름이 여러 종류에 걸릴 수 있는지는 펌웨어가 정하는
 * 일이라 이 트리에서 확인 못 함이지만, 코드가 이 순서를 강제한다는 사실만은
 * 분명하다.
 * 실행 컨텍스트: DLPAR 진입점 안(뮤텍스를 쥔 프로세스 컨텍스트).
 * 에러 경로: 셋 다 실패하면 NULL 을 돌려주고 호출자가 -ENODEV 로 바꾼다.
 *
 * 호출 체인:
 *   dlpar_add_slot() / dlpar_remove_slot() → [find_dlpar_node]
 *     → find_php_slot_pci_node(), find_vio_slot_node()
 */
/* Returns a device_node with its reference count incremented */
static struct device_node *find_dlpar_node(char *drc_name, int *node_type)
{
	/* [한국어] 찾은 노드. */
	struct device_node *dn;

	/* [한국어] 먼저 PCI 슬롯으로 찾아 본다. */
	dn = find_php_slot_pci_node(drc_name, "SLOT");
	/* [한국어] 찾았으면 */
	if (dn) {
		/* [한국어] 종류를 알려 주고 */
		*node_type = NODE_TYPE_SLOT;
		/* [한국어] 참조를 쥔 노드를 그대로 돌려준다. */
		return dn;
	}

	/* [한국어] 슬롯이 아니면 PCI 호스트 브리지로 찾아 본다. 같은 탐색기에 종류만
	 * 바꿔 넘기는 구조라 코드가 그대로 반복된다. */
	dn = find_php_slot_pci_node(drc_name, "PHB");
	/* [한국어] 찾았으면 */
	if (dn) {
		/* [한국어] 브리지 종류로 표시하고 */
		*node_type = NODE_TYPE_PHB;
		/* [한국어] 노드를 돌려준다. */
		return dn;
	}

	/* [한국어] 둘 다 아니면 마지막으로 VIO 장치로 찾아 본다. 이쪽은 훑는 트리 위치
	 * 자체가 다르다. */
	dn = find_vio_slot_node(drc_name);
	/* [한국어] 찾았으면 */
	if (dn) {
		/* [한국어] VIO 종류로 표시하고 */
		*node_type = NODE_TYPE_VIO;
		/* [한국어] 노드를 돌려준다. */
		return dn;
	}

	/* [한국어] 셋 다 아니면 이 이름은 이 시스템이 아는 DLPAR 대상이 아니다.
	 * node_type 은 손대지 않은 채로 남는데, 호출자가 NULL 을 먼저 검사하므로
	 * 그 값을 읽지는 않는다. */
	return NULL;
}

/* [한국어]
 * find_php_slot - 디바이스 트리 노드에 대응하는 핫플러그 슬롯을 찾는다
 *
 * @dn: 대상 노드.
 * @return: 그 노드의 slot 객체, 또는 NULL.
 *
 * 아래 계층인 rpaphp 가 소유한 전역 슬롯 리스트를 상위 계층이 직접 순회하는
 * 함수다. 그것이 가능한 것은 rpaphp_core.c 가 그 리스트를 EXPORT_SYMBOL_GPL
 * 로 내보냈기 때문이며, 두 계층을 잇는 세 접점 중 하나다.
 * 옆의 상류 kernel-doc 이 중요한 사실을 밝힌다 — 내장 PCI 슬롯은 DLPAR 로
 * 뗄 수 있어도 핫플러그 대상은 아니라서 이 리스트에 없고, 그런 노드에서는
 * NULL 이 나온다. 그래서 호출자들이 결과가 NULL 인 것을 오류로 다루지 않고
 * '핫플러그 등록 해제는 건너뛴다' 는 뜻으로 받아들인다.
 * 실행 컨텍스트: DLPAR 진입점 안(뮤텍스를 쥔 프로세스 컨텍스트).
 *
 * [상류 코드 관찰] 순회 도중 노드를 지우지 않는데도 안전한 순회 판(_safe)을
 * 쓴다. 다음 노드를 미리 담는 비용이 헛돌 뿐 동작에는 영향이 없다.
 * 또 이 리스트를 지키는 락이 없어, 이 파일의 뮤텍스 밖에서 rpaphp 가 리스트를
 * 바꾸면 순회 중에 그것을 만날 수 있다.
 *
 * 호출 체인:
 *   dlpar_remove_phb() / dlpar_remove_pci_slot() → [find_php_slot]
 */
/**
 * find_php_slot - return hotplug slot structure for device node
 * @dn: target &device_node
 *
 * This routine will return the hotplug slot structure
 * for a given device node. Note that built-in PCI slots
 * may be dlpar-able, but not hot-pluggable, so this routine
 * will return NULL for built-in PCI slots.
 */
static struct slot *find_php_slot(struct device_node *dn)
{
	/* [한국어] 순회 커서와, 안전한 순회 판이 요구하는 임시 자리(위 관찰 참조). */
	struct slot *slot, *next;

	/* [한국어] rpaphp 의 전역 슬롯 리스트를 훑는다. 다른 모듈이 소유한 리스트를
	 * 직접 도는 것이라, 두 모듈의 생명주기가 어긋나면 위험한 구조다 — 그것을
	 * 막는 것은 rpadlpar_io 가 rpaphp 를 심볼로 참조해 모듈 의존이 생긴다는 사실뿐이다. */
	list_for_each_entry_safe(slot, next, &rpaphp_slot_head,
				 rpaphp_slot_list) {
		/* [한국어] 이 슬롯이 찾는 노드의 것인가. 슬롯은 만들어질 때 자기 노드 참조를
		 * 보관해 두므로 포인터 비교로 충분하다. */
		if (slot->dn == dn)
			/* [한국어] 찾았다. */
			return slot;
	}

	/* [한국어] 없으면 NULL — 핫플러그로 등록되지 않은 노드라는 뜻이며, 오류가 아니다. */
	return NULL;
}

/* [한국어]
 * dlpar_find_new_dev - 방금 만들어진 PCI 장치를 부모 버스에서 찾아낸다
 *
 * @parent: 장치가 달렸을 부모 버스.
 * @dev_dn: 찾는 장치의 디바이스 트리 노드.
 * @return: 그 노드에 대응하는 pci_dev, 또는 NULL(없음).
 *
 * 브리지 장치를 만드는 함수가 결과를 돌려주지 않기 때문에 필요한 함수다.
 * 만들고 나서 부모 버스의 장치 목록을 훑어, 디바이스 트리 노드가 같은 장치를
 * 골라낸다. 즉 '만들었는가' 를 만든 쪽이 아니라 결과를 보고 확인하는 방식이다.
 * 비교 기준이 주소나 번호가 아니라 디바이스 트리 노드 포인터라는 점이 이
 * 플랫폼답다 — 이 시스템에서 장치의 정체를 정하는 것은 config space 가 아니라
 * 디바이스 트리다.
 * 실행 컨텍스트: DLPAR 추가 경로(뮤텍스를 쥔 프로세스 컨텍스트).
 *
 * [상류 코드 관찰] 버스의 장치 목록을 순회하면서 PCI 코어의 어떤 락도 잡지
 * 않는다. 이 경로가 뮤텍스로 직렬화되어 있어도 그 뮤텍스는 이 파일의 것일 뿐,
 * PCI 코어 쪽 변경을 막아 주지는 않는다.
 *
 * 호출 체인:
 *   dlpar_add_pci_slot() → [dlpar_find_new_dev] → pci_device_to_OF_node()
 */
static struct pci_dev *dlpar_find_new_dev(struct pci_bus *parent,
					struct device_node *dev_dn)
{
	/* [한국어] 순회 커서. NULL 로 초기화하지만 순회 매크로가 곧바로 덮어쓴다. */
	struct pci_dev *tmp = NULL;
	/* [한국어] 순회 중인 장치의 디바이스 트리 노드. */
	struct device_node *child_dn;

	/* [한국어] 부모 버스에 달린 장치들을 훑는다. */
	list_for_each_entry(tmp, &parent->devices, bus_list) {
		/* [한국어] 그 장치의 디바이스 트리 노드를 얻는다. PowerPC 가 pci_dev 와 DT 노드를
		 * 이어 두기에 가능한 조회이며, 구현은 arch/powerpc 에 있어 확인 못 함. */
		child_dn = pci_device_to_OF_node(tmp);
		/* [한국어] 찾는 노드와 같은 장치인가. 포인터 비교로 충분한 것은 한 노드에
		 * 하나의 pci_dev 만 대응하기 때문이다. */
		if (child_dn == dev_dn)
			/* [한국어] 찾았다. */
			return tmp;
	}
	/* [한국어] 없으면 브리지 장치 생성이 실패한 것이다. 호출자가 그것을 오류로 다룬다. */
	return NULL;
}

/* [한국어]
 * dlpar_pci_add_bus - PHB 아래에 브리지 장치를 만들고 그 하위를 열거한다
 *
 * @dn: 붙일 슬롯의 디바이스 트리 노드.
 * @return: 없음 — 실패는 로그로만 알린다(아래 관찰 참조).
 *
 * PCI 슬롯을 파티션에 붙이는 절차의 핵심이다. 다섯 단계가 순서대로 놓여 있고,
 * 그 순서가 전부 의미를 갖는다.
 *  1) EEH(Enhanced Error Handling) 자료구조를 이 노드 아래로 재귀적으로 만든다.
 *     장치가 등장한 뒤에는 이미 오류를 겪을 수 있으므로 그 전에 준비해 둔다.
 *  2) 옆의 상류 주석이 EADS 라 부르는 브리지 장치를 PHB 버스에 만들어 단다.
 *     이 한 번의 호출로 그 장치가 PHB 버스의 장치 목록에 나타난다.
 *  3) 그것이 브리지라면 그 아래를 스캔해 하위 버스와 장치를 만든다.
 *  4) 하위 버스의 IO 공간을 매핑한다. 옆의 상류 주석이 성공하지 않을 수도 있다고
 *     적어 두었고, 실제로 반환값을 확인하지 않는다.
 *  5) 자원 배정과 장치 등록을 마무리한다. 옆의 긴 상류 주석이 밝히듯 이 단계는
 *     하위 버스가 아니라 '부모' 버스에 대해 해야 한다 — 그래야 브리지 장치
 *     자신도 제대로 등록되기 때문이다.
 * 실행 컨텍스트: DLPAR 추가 경로(뮤텍스를 쥔 프로세스 컨텍스트). PCI 열거가
 * 잠들 수 있다.
 *
 * [상류 코드 관찰] 반환형이 void 라 실패를 호출자에게 전할 수 없다. 브리지 장치
 * 생성이 실패하면 로그만 남기고 조용히 돌아가며, 호출자는 그 사실을 모른 채
 * 다음 단계로 간다 — 다만 그 다음 단계가 장치를 다시 찾아보므로 결과적으로는
 * 걸러진다.
 *
 * 호출 체인:
 *   dlpar_add_pci_slot() → [dlpar_pci_add_bus]
 *     → pseries_eeh_init_edev_recursive(), of_create_pci_dev(),
 *       of_scan_pci_bridge(), pcibios_map_io_space(),
 *       pcibios_finish_adding_to_bus()
 */
static void dlpar_pci_add_bus(struct device_node *dn)
{
	/* [한국어] 노드에서 PowerPC 전용 PCI 정보를 꺼낸다. devfn 과 소속 PHB 가 여기 있다. */
	struct pci_dn *pdn = PCI_DN(dn);
	/* [한국어] 이 노드가 속한 PCI 호스트 브리지. 새 장치를 달 버스의 주인이다. */
	struct pci_controller *phb = pdn->phb;
	/* [한국어] 만들어질 브리지 장치. */
	struct pci_dev *dev = NULL;

	/* [한국어] 열거보다 먼저 EEH 자료구조를 만들어 둔다. 순서가 중요한 이유는 함수
	 * 주석의 1단계 설명 참조. rpaphp_core.c 의 enable_slot() 도 같은 순서를 지킨다. */
	pseries_eeh_init_edev_recursive(pdn);

	/* Add EADS device to PHB bus, adding new entry to bus->devices */
	/* [한국어] 옆의 상류 주석대로 브리지 장치를 만들어 PHB 버스에 단다. 어느 자리에
	 * 달지는 디바이스 트리가 알려 준 devfn 으로 정한다 — 버스를 스캔해 찾아내는
	 * 것이 아니라 트리가 미리 말해 주는 방식이다. */
	dev = of_create_pci_dev(dn, phb->bus, pdn->devfn);
	/* [한국어] 만들지 못했으면 */
	if (!dev) {
		/* [한국어] 어느 노드에서 실패했는지 남기고 */
		printk(KERN_ERR "%s: failed to create pci dev for %pOF\n",
				__func__, dn);
		/* [한국어] 조용히 돌아간다. 반환형이 void 라 호출자에게 전할 방법이 없다(위 관찰). */
		return;
	}

	/* Scan below the new bridge */
	/* [한국어] 옆의 상류 주석대로 새 브리지 아래를 스캔한다. 만든 장치가 브리지일
	 * 때만 의미가 있으므로 헤더 종류를 먼저 확인한다. */
	if (pci_is_bridge(dev))
		/* [한국어] 하위 버스와 그 아래 장치들을 디바이스 트리를 따라 만든다.
		 * 일반 PCI 처럼 config space 를 훑는 것이 아니라 트리를 읽는 방식이다. */
		of_scan_pci_bridge(dev);

	/* Map IO space for child bus, which may or may not succeed */
	/* [한국어] 옆의 상류 주석대로 하위 버스의 IO 공간을 매핑한다. 성공하지 않을 수도
	 * 있다고 적혀 있고 실제로 반환값을 확인하지 않는다 — IO 공간이 없는 구성에서도
	 * 진행해야 하기 때문으로 읽힌다. */
	pcibios_map_io_space(dev->subordinate);

	/* Finish adding it : resource allocation, adding devices, etc...
	 * Note that we need to perform the finish pass on the -parent-
	 * bus of the EADS bridge so the bridge device itself gets
	 * properly added
	 */
	/* [한국어] 위 상류 주석이 길게 설명한 마무리 단계다. 자원 배정과 장치 등록을
	 * 하위 버스가 아니라 부모(PHB) 버스에 대해 수행하는데, 그래야 방금 만든 브리지
	 * 장치 자신도 등록 대상에 포함되기 때문이다. */
	pcibios_finish_adding_to_bus(phb->bus);
}

/* [한국어]
 * dlpar_add_pci_slot - PCI 슬롯을 파티션에 붙이고 핫플러그 슬롯까지 등록한다
 *
 * @drc_name: 붙일 슬롯의 DRC 이름(로그에만 쓰인다).
 * @dn: 그 슬롯의 디바이스 트리 노드.
 * @return: 0 성공, -EINVAL(이미 붙어 있음), -EIO(생성 실패 또는 종류 불일치).
 *
 * 종류가 SLOT 일 때의 절차 전체다. 네 단계로 읽힌다.
 *  1) 이미 이 노드의 버스가 있으면 중복 추가이므로 거절한다.
 *  2) 브리지 장치를 만들고 그 아래를 열거한다(앞 함수).
 *  3) 그것이 정말 만들어졌고 브리지가 맞는지 확인한다. 앞 함수가 실패를
 *     전하지 못하므로 이 확인이 사실상의 오류 검출 지점이다.
 *  4) 마지막에 아래 계층 rpaphp 에 핫플러그 슬롯 등록을 요청한다 — 이 호출이
 *     DLPAR 과 핫플러그를 잇는 지점이고, 이때부터 사용자가 sysfs 로 이 슬롯에
 *     카드를 꽂고 뺄 수 있게 된다.
 * 실행 컨텍스트: DLPAR 추가 경로(뮤텍스를 쥔 프로세스 컨텍스트). 잠들 수 있다.
 *
 * [상류 코드 관찰] 3단계나 4단계에서 실패해도 2단계에서 만든 브리지 장치와
 * 그 아래 열거된 장치들을 되돌리지 않는다. 오류를 돌려주고 끝나므로, 반쯤
 * 붙은 상태가 남을 수 있다.
 *
 * 호출 체인:
 *   dlpar_add_slot() → [dlpar_add_pci_slot]
 *     → pci_find_bus_by_node(), dlpar_pci_add_bus(), dlpar_find_new_dev(),
 *       rpaphp_add_slot()
 */
static int dlpar_add_pci_slot(char *drc_name, struct device_node *dn)
{
	/* [한국어] 만들어졌는지 확인할 브리지 장치. */
	struct pci_dev *dev;
	/* [한국어] 이 노드가 속한 PHB. */
	struct pci_controller *phb;

	/* [한국어] 이 노드에 이미 버스가 있다면 이미 붙어 있는 슬롯이다. */
	if (pci_find_bus_by_node(dn))
		/* [한국어] 중복 추가를 거절한다. */
		return -EINVAL;

	/* Add pci bus */
	/* [한국어] 옆의 상류 주석대로 버스를 만든다. 실패해도 알 수 없으므로 아래에서
	 * 결과를 확인한다. */
	dlpar_pci_add_bus(dn);

	/* Confirm new bridge dev was created */
	/* [한국어] 새 장치를 찾을 기준이 될 PHB 버스를 얻는다. */
	phb = PCI_DN(dn)->phb;
	/* [한국어] 옆의 상류 주석대로 브리지 장치가 정말 만들어졌는지 확인한다.
	 * 앞 함수가 void 라 이 확인이 실질적인 오류 검출 지점이다. */
	dev = dlpar_find_new_dev(phb->bus, dn);

	/* [한국어] 만들어지지 않았으면 */
	if (!dev) {
		/* [한국어] 어느 슬롯인지 남기고 */
		printk(KERN_ERR "%s: unable to add bus %s\n", __func__,
			drc_name);
		/* [한국어] 입출력 오류로 보고한다. */
		return -EIO;
	}

	/* [한국어] 만들어졌더라도 브리지가 아니면 이 절차의 전제가 깨진 것이다 —
	 * 슬롯을 붙인다는 것은 그 자리에 브리지가 서는 일이기 때문이다. */
	if (dev->hdr_type != PCI_HEADER_TYPE_BRIDGE) {
		/* [한국어] 어떤 헤더 종류가 나왔는지 함께 남기고 */
		printk(KERN_ERR "%s: unexpected header type %d, unable to add bus %s\n",
			__func__, dev->hdr_type, drc_name);
		/* [한국어] 입출력 오류로 보고한다. */
		return -EIO;
	}

	/* Add hotplug slot */
	/* [한국어] 옆의 상류 주석대로 아래 계층에 핫플러그 슬롯 등록을 요청한다.
	 * 이 호출이 DLPAR 계층과 rpaphp 계층을 잇는 지점이며, 성공하면 그때부터
	 * 사용자가 sysfs 로 이 슬롯을 다룰 수 있다. */
	if (rpaphp_add_slot(dn)) {
		/* [한국어] 등록에 실패하면 어느 슬롯인지 남기고 */
		printk(KERN_ERR "%s: unable to add hotplug slot %s\n",
			__func__, drc_name);
		/* [한국어] 입출력 오류로 보고한다. 이미 만든 장치는 되돌리지 않는다(위 관찰). */
		return -EIO;
	}
	/* [한국어] 슬롯 추가 성공. */
	return 0;
}

/* [한국어]
 * dlpar_remove_phb - PCI 호스트 브리지를 파티션에서 떼어 낸다
 *
 * @drc_name: 뗄 대상의 DRC 이름(로그에만 쓰인다).
 * @dn: 그 PHB 의 디바이스 트리 노드.
 * @return: 0 성공, -EINVAL(버스가 없음), -EIO(핫플러그 해제 실패),
 *          또는 PHB 제거 함수의 오류.
 *
 * 종류가 PHB 일 때의 제거 절차다. 슬롯 하나가 아니라 버스 전체를 없애는
 * 동작이라 슬롯 제거와는 순서가 다르다.
 *  1) 버스가 있는지 확인한다 — 없으면 애초에 붙어 있지 않은 것이다.
 *  2) 이 노드가 핫플러그 슬롯으로도 등록되어 있다면 먼저 그것을 해제한다.
 *     옆의 상류 주석대로, 내장 슬롯처럼 핫플러그로 등록되지 않은 경우에는
 *     찾지 못하는 것이 정상이라 NULL 을 오류로 다루지 않는다.
 *  3) PHB 자체를 없앤다.
 *  4) 노드가 들고 있던 PHB 포인터를 지워, 나중에 다시 붙일 때 '이미 있다' 로
 *     오판하지 않게 한다. 아래 dlpar_add_phb() 의 첫 검사가 이 값을 본다.
 * 실행 컨텍스트: DLPAR 제거 경로(뮤텍스를 쥔 프로세스 컨텍스트). 잠들 수 있다.
 *
 * [상류 코드 관찰] 같은 파일의 다른 곳은 PCI_DN(dn) 매크로로 이 정보를 꺼내는데
 * 여기서는 dn->data 를 직접 읽는다. 그 매크로의 정의는 arch/powerpc 에 있어
 * 둘이 같은 것인지 이 트리에서 확인 못 함.
 * 또 BUG_ON 으로 커널을 멈추는 방식을 쓴다 — 여기까지 왔는데 정보가 없다면
 * 자료구조가 이미 깨진 것이라는 판단으로 읽히지만, 그 근거는 코드에 없다.
 *
 * 호출 체인:
 *   dlpar_remove_slot() → [dlpar_remove_phb]
 *     → pci_find_bus_by_node(), find_php_slot(), rpaphp_deregister_slot(),
 *       remove_phb_dynamic()
 */
static int dlpar_remove_phb(char *drc_name, struct device_node *dn)
{
	/* [한국어] 핫플러그로 등록되어 있다면 찾아낼 슬롯. */
	struct slot *slot;
	/* [한국어] 노드가 들고 있는 PowerPC 전용 PCI 정보. */
	struct pci_dn *pdn;
	/* [한국어] PHB 제거 결과. */
	int rc = 0;

	/* [한국어] 버스가 없다면 이 PHB 는 붙어 있지 않다. */
	if (!pci_find_bus_by_node(dn))
		/* [한국어] 잘못된 요청으로 보고한다. */
		return -EINVAL;

	/* If pci slot is hotpluggable, use hotplug to remove it */
	/* [한국어] 옆의 상류 주석대로, 핫플러그로도 등록된 슬롯이면 그쪽부터 해제한다. */
	slot = find_php_slot(dn);
	/* [한국어] 슬롯이 있는데 해제에 실패한 경우만 오류다. 슬롯이 없는 것(NULL)은
	 * 정상이라 && 의 단축 평가로 그대로 통과한다 — 내장 슬롯은 핫플러그로 등록되지
	 * 않기 때문이다. */
	if (slot && rpaphp_deregister_slot(slot)) {
		/* [한국어] 어느 슬롯인지 남기고 */
		printk(KERN_ERR "%s: unable to remove hotplug slot %s\n",
		       __func__, drc_name);
		/* [한국어] 입출력 오류로 보고한다. */
		return -EIO;
	}

	/* [한국어] 노드가 들고 있는 PCI 정보를 꺼낸다(위 관찰대로 매크로 대신 직접 접근). */
	pdn = dn->data;
	/* [한국어] 여기까지 온 이상 정보와 PHB 가 반드시 있어야 한다는 단언이다.
	 * 위에서 버스를 이미 확인했으므로, 없다면 자료구조가 어긋난 것이다. */
	BUG_ON(!pdn || !pdn->phb);
	/* [한국어] PHB 자체를 없앤다. 버스와 그 아래 자원을 아키텍처 코드가 정리한다. */
	rc = remove_phb_dynamic(pdn->phb);
	/* [한국어] 실패하면 */
	if (rc < 0)
		/* [한국어] 그 오류를 그대로 전달한다. 앞서 해제한 핫플러그 슬롯은 되돌리지 않는다. */
		return rc;

	/* [한국어] 노드의 PHB 포인터를 지운다. 이것이 없으면 나중에 같은 노드를 다시
	 * 붙일 때 아래 dlpar_add_phb() 의 첫 검사가 '이미 있다' 로 오판한다. */
	pdn->phb = NULL;

	/* [한국어] PHB 제거 성공. */
	return 0;
}

/* [한국어]
 * dlpar_add_phb - PCI 호스트 브리지를 파티션에 붙인다
 *
 * @drc_name: 붙일 대상의 DRC 이름(로그에만 쓰인다).
 * @dn: 그 PHB 의 디바이스 트리 노드.
 * @return: 0 성공, -EINVAL(이미 있음), -EIO(생성 또는 핫플러그 등록 실패).
 *
 * 제거의 대칭이지만 훨씬 짧다. PHB 를 만드는 일 자체를 아키텍처 코드가 통째로
 * 맡아 주기 때문이다 — 슬롯 추가가 브리지 장치 생성부터 자원 배정까지 다섯
 * 단계를 손으로 밟는 것과 대비된다.
 * 중복 검사가 노드의 PHB 포인터를 보는데, 그 값을 지워 주는 것이 제거 쪽의
 * 마지막 단계다. 두 함수가 그 필드 하나로 짝을 이룬다.
 * 마지막에 핫플러그 슬롯을 등록하는 것은 슬롯 추가 경로와 같다.
 * 실행 컨텍스트: DLPAR 추가 경로(뮤텍스를 쥔 프로세스 컨텍스트). 잠들 수 있다.
 *
 * [상류 코드 관찰] 핫플러그 등록이 실패해도 방금 만든 PHB 를 없애지 않는다.
 * 슬롯 추가 경로와 같은 성질의 되감기 누락이다.
 *
 * 호출 체인:
 *   dlpar_add_slot() → [dlpar_add_phb] → init_phb_dynamic(), rpaphp_add_slot()
 */
static int dlpar_add_phb(char *drc_name, struct device_node *dn)
{
	/* [한국어] 만들어질 PHB. */
	struct pci_controller *phb;

	/* [한국어] 노드가 이미 PHB 를 들고 있는지 본다. 제거 쪽이 마지막에 이 포인터를
	 * 지우므로, 두 함수가 이 필드로 상태를 주고받는 셈이다. */
	if (PCI_DN(dn) && PCI_DN(dn)->phb) {
		/* PHB already exists */
		/* [한국어] 옆의 상류 주석대로 이미 있으므로 중복 추가를 거절한다. */
		return -EINVAL;
	}

	/* [한국어] PHB 를 만든다. 버스 생성과 자원 설정을 아키텍처 코드가 전부 처리하며,
	 * 그 구현은 arch/powerpc 에 있어 확인 못 함. */
	phb = init_phb_dynamic(dn);
	/* [한국어] 만들지 못했으면 */
	if (!phb)
		/* [한국어] 입출력 오류로 보고한다. */
		return -EIO;

	/* [한국어] 슬롯 추가 경로와 같이, 마지막에 아래 계층에 핫플러그 슬롯 등록을
	 * 요청한다. PHB 도 그 아래에 슬롯을 가질 수 있기 때문이다. */
	if (rpaphp_add_slot(dn)) {
		/* [한국어] 실패하면 어느 대상인지 남기고 */
		printk(KERN_ERR "%s: unable to add hotplug slot %s\n",
			__func__, drc_name);
		/* [한국어] 입출력 오류로 보고한다. 만든 PHB 는 되돌리지 않는다(위 관찰). */
		return -EIO;
	}
	/* [한국어] PHB 추가 성공. */
	return 0;
}

/* [한국어]
 * dlpar_add_vio_slot - 가상 I/O 장치를 파티션에 붙인다
 *
 * @drc_name: 붙일 대상의 DRC 이름(로그에만 쓰인다).
 * @dn: 그 장치의 디바이스 트리 노드.
 * @return: 0 성공, -EINVAL(이미 있음), -EIO(등록 실패).
 *
 * 세 종류 중 절차가 가장 짧다. VIO 버스가 노드 하나를 받아 장치 등록을 통째로
 * 해 주기 때문에, 이 파일은 중복 검사와 호출 한 번만 하면 된다.
 * 중복 검사 방식이 특이하다 — 조회 함수가 찾은 장치의 참조를 하나 올려 주므로,
 * '이미 있다' 로 판정하고 돌아가기 전에 그 참조를 반드시 놓아야 한다. 그것이
 * 거절 경로에만 put 이 있는 이유다. 없을 때는 잡힌 참조도 없어 놓을 것이 없다.
 * 실행 컨텍스트: DLPAR 추가 경로(뮤텍스를 쥔 프로세스 컨텍스트). 잠들 수 있다.
 *
 * 호출 체인:
 *   dlpar_add_slot() → [dlpar_add_vio_slot]
 *     → vio_find_node(), put_device(), vio_register_device_node()
 */
static int dlpar_add_vio_slot(char *drc_name, struct device_node *dn)
{
	/* [한국어] 이미 등록되어 있는지 확인할 때 받을 장치. */
	struct vio_dev *vio_dev;

	/* [한국어] 이 노드의 VIO 장치가 이미 있는지 조회한다. 찾으면 참조를 하나 올려 준다. */
	vio_dev = vio_find_node(dn);
	/* [한국어] 이미 있으면 */
	if (vio_dev) {
		/* [한국어] 조회가 올려 준 참조를 먼저 놓는다. 이 한 줄이 없으면 중복 추가를
		 * 거절할 때마다 참조가 샌다. */
		put_device(&vio_dev->dev);
		/* [한국어] 중복 추가를 거절한다. */
		return -EINVAL;
	}

	/* [한국어] 없으므로 새로 등록한다. VIO 버스가 노드를 읽어 장치를 만들고 드라이버
	 * 바인딩까지 처리한다 — PCI 쪽처럼 손으로 단계를 밟을 필요가 없다.
	 * 반환값이 NULL 이면 실패라는 규약이라 조건이 부정형이다. */
	if (!vio_register_device_node(dn)) {
		/* [한국어] 실패하면 어느 노드였는지 남기고 */
		printk(KERN_ERR
			"%s: failed to register vio node %s\n",
			__func__, drc_name);
		/* [한국어] 입출력 오류로 보고한다. */
		return -EIO;
	}
	/* [한국어] VIO 장치 추가 성공. */
	return 0;
}

/* [한국어]
 * dlpar_add_slot - DLPAR 공개 진입점. 이름으로 지목한 I/O 슬롯을 파티션에 붙인다
 *
 * @drc_name: 펌웨어가 부여한 DRC 이름. 버스:장치 번호가 아니라 문자열인 것이
 *            DLPAR 의 성격을 보여 준다 — 슬롯의 소유권을 파티션 사이에서
 *            옮기는 동작이라 물리 위치가 아니라 논리 이름으로 지목한다.
 * @return: 옆의 상류 kernel-doc 이 나열한 다섯 가지 — 0 성공, -ENODEV(이름을
 *          찾지 못함), -EINVAL(이미 붙어 있음), -ERESTARTSYS(락을 얻기 전에
 *          시그널을 받음), -EIO(내부 PCI 오류).
 *
 * 이 파일의 두 공개 진입점 중 하나다. 하는 일은 조율뿐이며 실제 작업은 종류별
 * 함수가 한다. (1) 뮤텍스를 잡아 이 절차 전체를 직렬화한다. (2) DRC 이름으로
 * 노드와 종류를 찾는다. (3) 종류에 맞는 함수로 갈라 준다. (4) 노드 참조를 놓고
 * 뮤텍스를 푼다.
 * 락을 interruptible 판으로 잡는 것이 중요하다 — 이 경로는 사용자의 sysfs
 * 쓰기에서 시작되고 그 안에서 PCI 열거로 오래 머물 수 있으므로, 기다리는 쪽이
 * 시그널로 빠져나갈 수 있어야 한다.
 * 실행 컨텍스트: 사용자의 sysfs 쓰기(프로세스 컨텍스트). 뮤텍스와 PCI 열거,
 * RTAS 호출로 여러 번 잠들 수 있다.
 *
 * [상류 코드 관찰] 두 가지.
 *  1. 성공 로그가 rc 를 확인하지 않는 자리에 있다. 종류별 함수가 실패해도
 *     "slot ... added" 가 그대로 찍히며, 실패는 반환값으로만 전달된다.
 *  2. rc 를 -EIO 로 초기화해 두어, 종류가 셋 중 어느 것도 아닌 경우에
 *     그 값이 그대로 반환된다. 아래 제거 쪽은 같은 자리를 0 으로 초기화해
 *     두 진입점의 방어 수준이 다르다.
 *
 * 호출 체인:
 *   사용자의 sysfs 쓰기 → rpadlpar_sysfs.c 의 add_slot_store() → [dlpar_add_slot]
 *     → find_dlpar_node(), dlpar_add_vio_slot() / dlpar_add_pci_slot() /
 *       dlpar_add_phb()
 */
/**
 * dlpar_add_slot - DLPAR add an I/O Slot
 * @drc_name: drc-name of newly added slot
 *
 * Make the hotplug module and the kernel aware of a newly added I/O Slot.
 * Return Codes:
 * 0			Success
 * -ENODEV		Not a valid drc_name
 * -EINVAL		Slot already added
 * -ERESTARTSYS		Signalled before obtaining lock
 * -EIO			Internal PCI Error
 */
int dlpar_add_slot(char *drc_name)
{
	/* [한국어] 찾을 노드. */
	struct device_node *dn = NULL;
	/* [한국어] 그 노드의 종류를 받을 자리. */
	int node_type;
	/* [한국어] 결과. -EIO 로 시작하는 이유는 위 관찰 2 참조. */
	int rc = -EIO;

	/* [한국어] 절차 전체를 직렬화하는 뮤텍스를 잡는다. interruptible 판이라 기다리는
	 * 동안 시그널을 받으면 포기할 수 있다 — 사용자 요청에서 시작되는 경로이므로
	 * 응답 없는 대기를 만들지 않으려는 것이다. */
	if (mutex_lock_interruptible(&rpadlpar_mutex))
		/* [한국어] 시그널로 깨어났다면 아무것도 하지 않고 물러난다. 사용자 공간이
		 * 다시 시도할 수 있는 오류다. */
		return -ERESTARTSYS;

	/* Find newly added node */
	/* [한국어] 옆의 상류 주석대로 이름으로 노드를 찾고 종류까지 알아낸다.
	 * 성공하면 참조가 하나 잡힌 채 돌아온다. */
	dn = find_dlpar_node(drc_name, &node_type);
	/* [한국어] 그런 이름이 없으면 */
	if (!dn) {
		/* [한국어] 장치 없음으로 표시하고 */
		rc = -ENODEV;
		/* [한국어] 락만 푸는 출구로 간다. 노드 참조도 잡히지 않았으므로 놓을 것이 없다. */
		goto exit;
	}

	/* [한국어] 종류에 따라 절차가 완전히 갈린다. */
	switch (node_type) {
		/* [한국어] 가상 I/O 장치 — VIO 버스에 등록만 하면 된다. */
		case NODE_TYPE_VIO:
			/* [한국어] 가장 짧은 경로. */
			rc = dlpar_add_vio_slot(drc_name, dn);
			/* [한국어] VIO 처리 끝. */
			break;
		/* [한국어] PCI 슬롯 — 브리지 장치 생성부터 자원 배정, 핫플러그 등록까지. */
		case NODE_TYPE_SLOT:
			/* [한국어] 가장 긴 경로. */
			rc = dlpar_add_pci_slot(drc_name, dn);
			/* [한국어] 슬롯 처리 끝. */
			break;
		/* [한국어] PCI 호스트 브리지 — 버스 하나를 통째로 만든다. */
		case NODE_TYPE_PHB:
			/* [한국어] 아키텍처 코드가 대부분을 처리한다. */
			rc = dlpar_add_phb(drc_name, dn);
			/* [한국어] PHB 처리 끝. */
			break;
	}
	/* [한국어] 탐색이 잡아 준 노드 참조를 놓는다. 종류별 함수들이 노드를 계속
	 * 들고 있어야 한다면 각자 참조를 올렸을 것이므로, 여기서 놓는 것이 맞다. */
	of_node_put(dn);

	/* [한국어] 완료를 알린다. 위 관찰 1 대로 rc 를 보지 않으므로 실패해도 찍힌다. */
	printk(KERN_INFO "%s: slot %s added\n", DLPAR_MODULE_NAME, drc_name);
/* [한국어] 노드를 찾지 못한 경로가 뛰어오는 출구. */
exit:
	/* [한국어] 뮤텍스를 푼다. 두 경로가 여기서 만난다. */
	mutex_unlock(&rpadlpar_mutex);
	/* [한국어] 결과를 sysfs 계층에 전달한다. 그쪽이 다시 사용자에게 전한다. */
	return rc;
}

/* [한국어]
 * dlpar_remove_vio_slot - 가상 I/O 장치를 파티션에서 떼어 낸다
 *
 * @drc_name: 옆의 상류 kernel-doc 이 적어 둔 대상 이름.
 *            [상류 코드 관찰] 인자로 받지만 본문에서 쓰지 않는다 — 같은 자리의
 *            다른 함수들이 오류 로그에 이 이름을 쓰는 것과 대비된다.
 * @dn: 그 장치의 디바이스 트리 노드.
 * @return: 0 성공, -EINVAL(옆의 상류 주석대로 VIO 장치가 없음).
 *
 * 추가 쪽과 대칭이며 역시 가장 짧다. 참조 카운트 규약도 거울처럼 뒤집힌다 —
 * 추가 쪽은 '이미 있다' 로 거절할 때 참조를 놓았지만, 이쪽은 찾았을 때 그 장치를
 * 해제한 뒤 참조를 놓는다. 해제와 참조 반납이 별개라는 점이 요점이다: 등록
 * 해제는 버스에서 떼는 일이고, 참조 반납은 조회가 올려 준 카운트를 되돌리는
 * 일이라 둘 다 필요하다.
 * 실행 컨텍스트: DLPAR 제거 경로(뮤텍스를 쥔 프로세스 컨텍스트). 잠들 수 있다.
 *
 * 호출 체인:
 *   dlpar_remove_slot() → [dlpar_remove_vio_slot]
 *     → vio_find_node(), vio_unregister_device(), put_device()
 */
/**
 * dlpar_remove_vio_slot - DLPAR remove a virtual I/O Slot
 * @drc_name: drc-name of newly added slot
 * @dn: &device_node
 *
 * Remove the kernel and hotplug representations of an I/O Slot.
 * Return Codes:
 * 0			Success
 * -EINVAL		Vio dev doesn't exist
 */
static int dlpar_remove_vio_slot(char *drc_name, struct device_node *dn)
{
	/* [한국어] 떼어 낼 장치. */
	struct vio_dev *vio_dev;

	/* [한국어] 이 노드의 VIO 장치를 찾는다. 찾으면 참조를 하나 올려 준다. */
	vio_dev = vio_find_node(dn);
	/* [한국어] 없으면 애초에 붙어 있지 않은 것이다. */
	if (!vio_dev)
		/* [한국어] 잘못된 요청으로 보고한다. 참조도 잡히지 않았으므로 놓을 것이 없다. */
		return -EINVAL;

	/* [한국어] VIO 버스에서 장치를 떼어 낸다. 드라이버 해제와 자원 정리를 버스가
	 * 처리하며, 구현은 arch/powerpc 에 있어 확인 못 함. */
	vio_unregister_device(vio_dev);

	/* [한국어] 조회가 올려 준 참조를 놓는다. 위의 등록 해제와는 별개의 일이라
	 * 둘 다 필요하다. */
	put_device(&vio_dev->dev);

	/* [한국어] VIO 장치 제거 성공. */
	return 0;
}

/* [한국어]
 * dlpar_remove_pci_slot - PCI 슬롯 아래 장치와 브리지를 모두 떼어 낸다
 *
 * @drc_name: 대상의 DRC 이름(오류 로그에만 쓰인다).
 * @dn: 그 슬롯의 디바이스 트리 노드.
 * @return: 옆의 상류 kernel-doc 이 적은 대로 0 성공, 그 밖에 -EINVAL(버스 없음),
 *          -EIO(핫플러그 해제 실패), -ERANGE(IO 공간 해제 실패).
 *
 * 추가 경로의 역순이며 이 파일에서 가장 긴 제거 절차다.
 *  1) 재스캔 락을 잡는다. 이 함수는 처음부터 끝까지 그 락 안에서 돌며, 그래서
 *     모든 오류 경로가 공통 출구로 모여 락을 푼다.
 *  2) 버스를 찾는다. 없으면 붙어 있지 않은 슬롯이다.
 *  3) 핫플러그로도 등록되어 있다면 먼저 그 등록을 해제한다. 없는 것은 정상이라
 *     오류로 다루지 않는다(내장 슬롯 등).
 *  4) 옆의 상류 주석대로 슬롯 아래 장치를 모두 제거한다.
 *  5) IO 공간 매핑을 해제한다. 추가 경로에서는 실패해도 넘어갔지만 이쪽은
 *     오류로 다룬다 — 매핑이 남은 채 브리지를 없애면 사라진 하드웨어를 가리키는
 *     매핑이 남기 때문으로 읽힌다.
 *  6) 마지막으로 브리지 장치 자신을 제거한다. 추가 경로에서 가장 먼저 만든
 *     것을 가장 나중에 없애는 대칭이다.
 * 실행 컨텍스트: DLPAR 제거 경로(뮤텍스를 쥔 프로세스 컨텍스트). 잠들 수 있다.
 *
 * [상류 코드 관찰] 마지막에 BUG_ON 으로 브리지 장치의 존재를 단언한다. 이
 * 함수는 종류가 SLOT 일 때만 불리므로 그 자리에 브리지가 있어야 한다는 전제인데,
 * PHB 의 루트 버스라면 그 포인터가 비어 있을 수 있다 — 그래서 위의 디버그
 * 로그는 같은 포인터를 쓰기 전에 NULL 검사를 하는데 여기서는 단언으로 다룬다.
 *
 * 호출 체인:
 *   dlpar_remove_slot() → [dlpar_remove_pci_slot]
 *     → pci_find_bus_by_node(), find_php_slot(), rpaphp_deregister_slot(),
 *       pci_hp_remove_devices(), pcibios_unmap_io_space(),
 *       pci_stop_and_remove_bus_device()
 */
/**
 * dlpar_remove_pci_slot - DLPAR remove a PCI I/O Slot
 * @drc_name: drc-name of newly added slot
 * @dn: &device_node
 *
 * Remove the kernel and hotplug representations of a PCI I/O Slot.
 * Return Codes:
 * 0			Success
 * -ENODEV		Not a valid drc_name
 * -EIO			Internal PCI Error
 */
static int dlpar_remove_pci_slot(char *drc_name, struct device_node *dn)
{
	/* [한국어] 제거할 버스. */
	struct pci_bus *bus;
	/* [한국어] 핫플러그로 등록되어 있다면 찾아낼 슬롯. */
	struct slot *slot;
	/* [한국어] 결과. 공통 출구로 모이는 구조라 변수 하나에 담아 둔다. */
	int ret = 0;

	/* [한국어] PCI 코어 전역의 재스캔·제거 락을 잡는다. 이 함수는 끝까지 이 락
	 * 안에서 도는데, 장치를 하나씩 떼어 내는 동안 다른 경로가 같은 버스를 훑으면
	 * 안 되기 때문이다. 그래서 모든 오류 경로가 goto 로 공통 출구에 모인다. */
	pci_lock_rescan_remove();

	/* [한국어] 이 노드의 버스를 찾는다. */
	bus = pci_find_bus_by_node(dn);
	/* [한국어] 없으면 붙어 있지 않은 슬롯이다. */
	if (!bus) {
		/* [한국어] 잘못된 요청으로 표시하고 */
		ret = -EINVAL;
		/* [한국어] 락을 푸는 공통 출구로 간다. */
		goto out;
	}

	/* [한국어] 어느 브리지 아래를 정리하는지 남긴다. 브리지 장치가 없을 수도 있어
	 * NULL 검사를 하고 대체 문자열을 쓴다 — 함수 끝의 단언과 대비되는 방어다. */
	pr_debug("PCI: Removing PCI slot below EADS bridge %s\n",
		 bus->self ? pci_name(bus->self) : "<!PHB!>");

	/* [한국어] 이 노드가 핫플러그 슬롯으로도 등록되어 있는지 찾는다. */
	slot = find_php_slot(dn);
	/* [한국어] 등록되어 있다면 그쪽부터 해제한다. 없는 것은 정상이라 이 검사가 있다. */
	if (slot) {
		/* [한국어] 어느 도메인·버스의 슬롯을 해제하는지 남긴다. */
		pr_debug("PCI: Removing hotplug slot for %04x:%02x...\n",
			 pci_domain_nr(bus), bus->number);

		/* [한국어] 아래 계층에 핫플러그 슬롯 등록 해제를 요청한다. 장치를 떼기 전에
		 * 해야 사용자가 사라지는 중인 슬롯을 sysfs 로 조작하지 못한다. */
		if (rpaphp_deregister_slot(slot)) {
			/* [한국어] 실패하면 어느 슬롯인지 남기고 */
			printk(KERN_ERR
				"%s: unable to remove hotplug slot %s\n",
				__func__, drc_name);
			/* [한국어] 입출력 오류로 표시한 뒤 */
			ret = -EIO;
			/* [한국어] 공통 출구로 간다. */
			goto out;
		}
	}

	/* Remove all devices below slot */
	/* [한국어] 옆의 상류 주석대로 슬롯 아래의 PCI 장치를 모두 떼어 낸다.
	 * rpaphp_core.c 의 disable_slot() 이 부르는 것과 같은 함수다 — 두 계층이
	 * 같은 제거 경로를 공유한다. */
	pci_hp_remove_devices(bus);

	/* Unmap PCI IO space */
	/* [한국어] 옆의 상류 주석대로 IO 공간 매핑을 푼다. 추가 경로에서는 매핑 실패를
	 * 무시했지만 해제 실패는 오류로 다루는데, 매핑이 남은 채 브리지를 없애면
	 * 사라진 하드웨어를 가리키는 매핑이 남기 때문으로 읽힌다. */
	if (pcibios_unmap_io_space(bus)) {
		/* [한국어] 실패했음을 남기고 */
		printk(KERN_ERR "%s: failed to unmap bus range\n",
			__func__);
		/* [한국어] 범위 오류로 표시한 뒤 */
		ret = -ERANGE;
		/* [한국어] 공통 출구로 간다. */
		goto out;
	}

	/* Remove the EADS bridge device itself */
	/* [한국어] 옆의 상류 주석대로 이제 브리지 장치 자신을 제거한다. 그 전에 브리지가
	 * 존재한다고 단언한다(위 관찰 참조). */
	BUG_ON(!bus->self);
	/* [한국어] 어느 장치를 제거하는지 남긴다. */
	pr_debug("PCI: Now removing bridge device %s\n", pci_name(bus->self));
	/* [한국어] 브리지 장치를 멈추고 커널에서 떼어 낸다. 추가 경로에서 가장 먼저
	 * 만든 것을 가장 나중에 없애는 대칭이다. */
	pci_stop_and_remove_bus_device(bus->self);

 /* [한국어] 모든 경로가 모이는 출구. 라벨 앞의 공백 하나는 편집기가 라벨을
  * 열 0 으로 정렬하지 않게 하려는 커널의 흔한 관례다. */
 out:
	/* [한국어] 재스캔 락을 푼다. 성공 경로도 여기로 흘러 들어온다. */
	pci_unlock_rescan_remove();
	/* [한국어] 결과를 호출자에게 전달한다. */
	return ret;
}

/* [한국어]
 * dlpar_remove_slot - DLPAR 공개 진입점. 이름으로 지목한 I/O 슬롯을 떼어 낸다
 *
 * @drc_name: 펌웨어가 부여한 DRC 이름.
 * @return: 옆의 상류 kernel-doc 이 나열한 대로 0 성공, -ENODEV(이름을 찾지 못함),
 *          -EINVAL(이미 제거됨), -ERESTARTSYS(시그널), -EIO(내부 오류).
 *
 * 추가 진입점과 거울처럼 대칭인 구조다. 뮤텍스를 잡고, 이름으로 노드와 종류를
 * 찾고, 종류별 함수로 갈라 주고, 참조를 놓고 락을 푼다.
 * 추가 쪽과 다른 점이 둘이다. 첫째, switch 안의 종류 순서가 VIO → PHB → SLOT
 * 으로 추가 쪽(VIO → SLOT → PHB)과 다르다 — 동작에는 영향이 없다. 둘째,
 * 노드 참조를 놓은 뒤 vm_unmap_aliases() 를 부른다. 장치가 사라진 뒤 남아 있을
 * 수 있는 vmalloc 별칭 매핑을 정리하는 것으로, rpaphp_core.c 의 disable_slot()
 * 도 같은 함수를 같은 이유로 부른다.
 * 실행 컨텍스트: 사용자의 sysfs 쓰기(프로세스 컨텍스트). 여러 번 잠들 수 있다.
 *
 * [상류 코드 관찰] 두 가지.
 *  1. 추가 쪽과 마찬가지로 완료 로그가 rc 를 확인하지 않아, 실패해도
 *     "slot ... removed" 가 찍힌다.
 *  2. rc 를 0 으로 초기화한다. 추가 쪽이 -EIO 로 시작하는 것과 달라, 종류가
 *     셋 중 어느 것도 아닌 경우 아무것도 하지 않고 성공을 돌려준다.
 *
 * 호출 체인:
 *   사용자의 sysfs 쓰기 → rpadlpar_sysfs.c 의 remove_slot_store()
 *     → [dlpar_remove_slot]
 *     → find_dlpar_node(), dlpar_remove_vio_slot() / dlpar_remove_phb() /
 *       dlpar_remove_pci_slot(), vm_unmap_aliases()
 */
/**
 * dlpar_remove_slot - DLPAR remove an I/O Slot
 * @drc_name: drc-name of newly added slot
 *
 * Remove the kernel and hotplug representations of an I/O Slot.
 * Return Codes:
 * 0			Success
 * -ENODEV		Not a valid drc_name
 * -EINVAL		Slot already removed
 * -ERESTARTSYS		Signalled before obtaining lock
 * -EIO			Internal Error
 */
int dlpar_remove_slot(char *drc_name)
{
	/* [한국어] 찾을 노드. */
	struct device_node *dn;
	/* [한국어] 그 노드의 종류를 받을 자리. */
	int node_type;
	/* [한국어] 결과. 추가 쪽과 달리 0 으로 시작한다(위 관찰 2 참조). */
	int rc = 0;

	/* [한국어] 추가 쪽과 같은 뮤텍스를 같은 방식으로 잡는다. 붙이는 절차와 떼는
	 * 절차가 겹치지 않게 하는 것이 이 락의 목적이다. */
	if (mutex_lock_interruptible(&rpadlpar_mutex))
		/* [한국어] 시그널로 깨어났다면 물러난다. */
		return -ERESTARTSYS;

	/* [한국어] 이름으로 노드와 종류를 찾는다. 성공하면 참조가 잡힌 채 돌아온다. */
	dn = find_dlpar_node(drc_name, &node_type);
	/* [한국어] 그런 이름이 없으면 */
	if (!dn) {
		/* [한국어] 장치 없음으로 표시하고 */
		rc = -ENODEV;
		/* [한국어] 락만 푸는 출구로 간다. */
		goto exit;
	}

	/* [한국어] 종류에 따라 갈라 준다. 순서가 추가 쪽과 다르지만 동작에는 영향이 없다. */
	switch (node_type) {
		/* [한국어] 가상 I/O 장치. */
		case NODE_TYPE_VIO:
			/* [한국어] 버스에서 떼고 참조를 놓는다. */
			rc = dlpar_remove_vio_slot(drc_name, dn);
			/* [한국어] VIO 처리 끝. */
			break;
		/* [한국어] PCI 호스트 브리지. */
		case NODE_TYPE_PHB:
			/* [한국어] 핫플러그 해제 후 PHB 자체를 없앤다. */
			rc = dlpar_remove_phb(drc_name, dn);
			/* [한국어] PHB 처리 끝. */
			break;
		/* [한국어] PCI 슬롯. */
		case NODE_TYPE_SLOT:
			/* [한국어] 장치 제거, IO 매핑 해제, 브리지 제거까지 수행한다. */
			rc = dlpar_remove_pci_slot(drc_name, dn);
			/* [한국어] 슬롯 처리 끝. */
			break;
	}
	/* [한국어] 탐색이 잡아 준 노드 참조를 놓는다. */
	of_node_put(dn);
	/* [한국어] 장치가 사라진 뒤 남아 있을 수 있는 vmalloc 별칭 매핑을 정리한다.
	 * 추가 경로에는 없는 단계이며, rpaphp_core.c 의 disable_slot() 도 같은 이유로
	 * 같은 함수를 부른다. */
	vm_unmap_aliases();

	/* [한국어] 완료를 알린다. 위 관찰 1 대로 rc 를 보지 않는다. */
	printk(KERN_INFO "%s: slot %s removed\n", DLPAR_MODULE_NAME, drc_name);
/* [한국어] 노드를 찾지 못한 경로가 뛰어오는 출구. */
exit:
	/* [한국어] 뮤텍스를 푼다. */
	mutex_unlock(&rpadlpar_mutex);
	/* [한국어] 결과를 sysfs 계층에 전달한다. */
	return rc;
}

/* [한국어]
 * is_dlpar_capable - 이 파티션이 DLPAR 을 지원하는지 확인한다
 *
 * @return: 0 이 아니면 지원함, 0 이면 지원하지 않음.
 *
 * 판별 방법이 이 플랫폼답다 — 어떤 레지스터나 능력 비트를 읽는 것이 아니라,
 * 펌웨어가 "ibm,configure-connector" 라는 RTAS 서비스를 제공하는지 묻는다.
 * 그 서비스가 있다는 것이 곧 이 파티션에서 동적 재구성이 가능하다는 뜻이다.
 * 토큰 조회가 알 수 없는 서비스라고 답하면 지원하지 않는 것이다.
 * 모듈이 올라올 때 딱 한 번 불리며, 지원하지 않는 시스템에서 모듈이 자리만
 * 차지하는 것을 막는다.
 * 실행 컨텍스트: 모듈 적재(프로세스 컨텍스트).
 *
 * [상류 코드 관찰] 비교 결과가 이미 int 인데 (int) 로 한 번 더 캐스팅한다.
 * 동작에는 영향이 없다.
 *
 * 호출 체인:
 *   rpadlpar_io_init() → [is_dlpar_capable] → rtas_token()
 */
static inline int is_dlpar_capable(void)
{
	/* [한국어] 그 이름의 RTAS 서비스가 있는지 토큰을 조회한다. 이 파일이 RTAS 를
	 * 직접 쓰는 유일한 자리이며, 실제 슬롯 조작은 아래 계층과 아키텍처 코드가 한다.
	 * RTAS 내부 동작은 arch/powerpc 가 이 트리에 없어 확인 못 함. */
	int rc = rtas_token("ibm,configure-connector");

	/* [한국어] 알 수 없는 서비스가 아니면 지원하는 것이다(위 관찰의 캐스팅 참조). */
	return (int) (rc != RTAS_UNKNOWN_SERVICE);
}

/* [한국어]
 * rpadlpar_io_init - 모듈 진입점. 지원 여부를 확인하고 sysfs 통로를 연다
 *
 * @return: 0 성공, -EPERM(이 파티션이 DLPAR 을 지원하지 않음),
 *          또는 sysfs 생성 실패 코드.
 *
 * 이 드라이버에는 probe 가 없다. 붙일 장치가 정해져 있는 것이 아니라 사용자나
 * 하이퍼바이저 도구가 이름으로 지시하는 방식이라, 필요한 것은 그 지시를 받을
 * 통로뿐이기 때문이다. 그래서 초기화는 두 줄이다 — 지원 여부를 확인하고,
 * sysfs 통로를 연다.
 * 지원하지 않는 시스템에서 -EPERM 으로 물러나는 것은 모듈이 올라와도 할 일이
 * 없기 때문이다. -ENODEV 가 아니라 -EPERM 인 것은 '장치가 없다' 가 아니라
 * '이 파티션에 그럴 권한이 없다' 는 뜻에 가깝기 때문으로 읽히지만, 그 선택을
 * 밝힌 주석은 코드에 없다.
 * 실행 컨텍스트: 모듈 적재(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   module_init → [rpadlpar_io_init] → is_dlpar_capable(), dlpar_sysfs_init()
 */
static int __init rpadlpar_io_init(void)
{

	/* [한국어] 펌웨어가 동적 재구성을 지원하는지 먼저 본다. */
	if (!is_dlpar_capable()) {
		/* [한국어] 지원하지 않으면 그 사실을 남기고 */
		printk(KERN_WARNING "%s: partition not DLPAR capable\n",
			__func__);
		/* [한국어] 권한 없음으로 물러난다. 모듈이 적재되지 않는다. */
		return -EPERM;
	}

	/* [한국어] sysfs 통로를 연다. 구현은 rpadlpar_sysfs.c 에 있고, 이 호출이
	 * 성공해야 사용자 공간이 DRC 이름을 써 넣을 파일이 생긴다. 그 결과를 그대로
	 * 모듈 적재 결과로 삼는다. */
	return dlpar_sysfs_init();
}

/* [한국어]
 * rpadlpar_io_exit - 모듈 종료점. sysfs 통로를 걷어낸다
 *
 * @return: 없음.
 *
 * 초기화의 대칭이지만 지원 여부 확인에 대응하는 것은 없다 — 확인은 상태를
 * 남기지 않는 순수한 질의였기 때문이다. 그래서 되돌릴 것은 sysfs 통로 하나뿐이다.
 * 실행 컨텍스트: 모듈 제거(프로세스 컨텍스트).
 *
 * [상류 코드 관찰] 이미 붙여 둔 슬롯을 되돌리지는 않는다. 모듈이 내려가도
 * 파티션에 붙은 I/O 슬롯은 그대로 남고, 다만 더 이상 이름으로 붙이고 뗄 수
 * 없게 될 뿐이다.
 *
 * 호출 체인:
 *   module_exit → [rpadlpar_io_exit] → dlpar_sysfs_exit()
 */
static void __exit rpadlpar_io_exit(void)
{
	/* [한국어] sysfs 통로를 없앤다. 이 뒤로는 사용자 공간이 지시를 보낼 수 없다. */
	dlpar_sysfs_exit();
}

/* [한국어] 모듈이 올라올 때 부를 함수를 등록한다. */
module_init(rpadlpar_io_init);
/* [한국어] 모듈이 내려갈 때 부를 함수를 등록한다. */
module_exit(rpadlpar_io_exit);
/* [한국어] 라이선스 선언. 이것이 없으면 rpaphp 가 EXPORT_SYMBOL_GPL 로 내보낸
 * 심볼들(rpaphp_add_slot, rpaphp_check_drc_props, rpaphp_slot_head)을 쓸 수
 * 없어 이 모듈은 링크되지 않는다 — 두 모듈의 관계에서 실질적인 의미를 갖는 선언이다. */
MODULE_LICENSE("GPL");
/* [한국어] modinfo 에 보일 설명. [상류 코드 관찰] MODULE_AUTHOR 는 없다 —
 * 파일 맨 위 상류 헤더에는 작성자 두 사람이 적혀 있는데도 그렇다. */
MODULE_DESCRIPTION("RPA Dynamic Logical Partitioning driver for I/O slots");
