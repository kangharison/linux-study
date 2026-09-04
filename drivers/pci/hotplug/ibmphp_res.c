// SPDX-License-Identifier: GPL-2.0+
/*
 * IBM Hot Plug Controller Driver
 *
 * Written By: Irene Zubarev, IBM Corporation
 *
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001,2002 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <gregkh@us.ibm.com>
 *
 */

/*
 * [한국어 설명] IBM 핫플러그 컨트롤러의 자원 관리자 (ibmphp_res.c)
 *
 * === 파일의 역할 ===
 * IBM 서버(2001~2002년경 xSeries 계열)의 PCI 핫플러그 드라이버가 쓰는
 * **자기만의 주소 자원 할당기**다. 카드가 꽂히면 그 카드의 BAR 에 줄
 * I/O·메모리·프리페치 메모리 구간을 여기서 골라 주고, 뽑히면 그 구간을
 * 되돌려 받는다.
 *
 * 이 파일을 낯설게 만드는 것은 **PCI 코어의 자원 할당기를 쓰지 않는다** 는
 * 점이다. 커널의 일반 경로는
 *   pci_assign_resource()        [drivers/pci/setup-res.c:787]
 *     -> __pci_assign_resource()   [같은 파일 :663]
 *       -> pci_bus_alloc_resource() [drivers/pci/bus.c:356]
 *         -> allocate_resource()      [kernel/resource.c — 이 트리에 없음]
 * 인데, 이 드라이버는 그 사슬을 전혀 타지 않고 struct bus_node 를 뿌리로 하는
 * 자기 연결 리스트 위에서 직접 빈자리를 찾는다. 아래 "자료구조" 와
 * "할당 정책" 절이 그 구조를 자세히 다룬다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 드라이버는 파일 넷이 한 벌로 움직인다(이 파일과 ibmphp_pci.c 외에는
 * 아직 한국어 주석이 없다).
 *   ibmphp_ebda.c — BIOS 의 EBDA(Extended BIOS Data Area) 표를 읽어 슬롯·
 *                   버스·자원 목록을 만든다. 그 결과가 전역
 *                   ibmphp_ebda_pci_rsrc_head 이고, 이 파일의
 *                   ibmphp_rsrc_init() 이 그것을 유일한 입력으로 받는다.
 *   ibmphp_hpc.c  — 핫플러그 컨트롤러 하드웨어와 대화한다(전원·LED·상태).
 *   ibmphp_core.c — 슬롯 등록과 enable/disable 흐름. 카드가 꽂히면
 *                   ibmphp_configure_card() 를 부른다.
 *   ibmphp_pci.c  — 그 카드의 config 공간을 실제로 채운다. BAR 하나마다
 *                   이 파일의 ibmphp_check_resource() 로 자리를 얻고
 *                   ibmphp_add_resource() 로 장부에 올린다.
 *
 * 즉 이 파일은 **장부**이고, ibmphp_pci.c 가 그 장부를 보고 하드웨어에
 * 값을 써 넣는 쪽이다. 두 단계로 나뉘어 있다는 점이 중요하다 —
 * check 는 자리를 고르기만 하고 목록을 바꾸지 않으며, 실제 등록은 호출자가
 * BAR 쓰기에 성공한 뒤 add 로 따로 한다.
 *
 * 시간 흐름은 이렇다.
 *   [부팅] ibmphp_core.c -> ibmphp_rsrc_init()
 *            -> EBDA 목록 순회로 창(range)과 이미 쓰이는 구간(resource)을 등록
 *            -> update_bridge_ranges() 로 브리지 뒤 2차 버스의 창을 보탠다
 *            -> once_over() 로 rangeno 가 -1 인 자원을 맞추고
 *               MEM 에서 떼어 쓴 PFMEM 을 별도 목록으로 옮긴다
 *   [삽입] ibmphp_core.c -> ibmphp_pci.c -> ibmphp_check_resource()
 *            -> (BAR 쓰기 성공) -> ibmphp_add_resource()
 *   [제거] ibmphp_pci.c -> ibmphp_remove_resource() / ibmphp_remove_bus()
 *   [언로드] ibmphp_core.c -> ibmphp_free_resources()
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. __init 함수는 부팅 시 한 번만
 * 돌고, 나머지는 슬롯 enable/disable 경로에서 불린다. 이 파일에는 락이
 * 전혀 없다 — 직렬화는 상위(ibmphp_core.c)가 맡는 것으로 보이나, 그
 * 보장의 근거는 이 트리에서 확인 못 함.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: ibmphp_pci.c 가 ibmphp_check_resource / ibmphp_add_resource /
 *   ibmphp_remove_resource / ibmphp_find_resource / ibmphp_remove_bus /
 *   ibmphp_find_res_bus / ibmphp_add_pfmem_from_mem 를 부른다.
 *   ibmphp_core.c 가 ibmphp_rsrc_init() 과 ibmphp_free_resources() 를 부른다.
 *   선언은 모두 ibmphp.h 에 있다.
 * 아래쪽: PCI config 접근(pci_bus_read_config_ 계열)을 update_bridge_ranges()
 *   에서만 쓴다. 그때 쓰는 버스가 전역 ibmphp_pci_bus 인데, 그것은 실제로
 *   열거된 struct pci_bus 가 아니라 번호만 갈아 끼우며 재사용하는 껍데기다
 *   (ibmphp_ebda.c 가 만든다). 그 밖에는 slab(kzalloc_obj/kfree)과
 *   list.h 의 연결 리스트만 쓴다.
 * 공유 상태: 이 파일의 전역은 gbuses(모든 버스의 머리)와 flags(테스트용)
 *   둘뿐이며, gbuses 는 static 이라 바깥에서는 ibmphp_find_res_bus() 등
 *   함수를 통해서만 닿는다. struct bus_node / range_node / resource_node 는
 *   ibmphp.h 에 정의되어 ibmphp_pci.c 와 공유한다.
 *
 * === 주요 함수/구조체 요약 ===
 * ibmphp_rsrc_init()      : EBDA 목록을 읽어 장부 전체를 세운다(부팅 1회).
 * ibmphp_check_resource() : 요구 길이가 들어갈 가장 작은 틈을 찾는다(핵심).
 * ibmphp_add_resource()   : 고른 구간을 창별·주소순 목록에 끼워 넣는다.
 * ibmphp_remove_resource(): 그 역. PFMemFromMem 의 짝까지 함께 지운다.
 * ibmphp_remove_bus()     : 카드가 브리지였을 때 그 뒤 버스를 통째로 지운다.
 * ibmphp_find_resource()  : 시작 주소로 자원 노드를 되찾는다.
 * ibmphp_free_resources() : 모듈 언로드 시 장부 전체를 해제한다.
 * add_bus_range()         : 창을 주소순으로 끼우고 뒤따르는 rangeno 를 민다.
 * update_bridge_ranges()  : 브리지 config 공간을 읽어 2차 버스의 창을 만든다.
 * once_over()             : 초기화 끝에 -1 로 남은 rangeno 를 맞추고,
 *                           MEM 에서 떼어 쓴 PFMEM 을 별도 목록으로 옮긴다.
 * struct bus_node         : 버스 하나의 창 목록 셋과 자원 목록 넷.
 * struct range_node       : 창 하나(start/end/rangeno).
 * struct resource_node    : 쓰이는 구간 하나. next/nextRange 이중 연결.
 *
 * === 자료구조: 목록이 두 겹이다 ===
 * 전역 리스트 gbuses 에 struct bus_node 가 버스마다 하나씩 매달린다.
 * 각 버스는 자원 종류(MEM/IO/PFMEM)마다 **목록 두 개**를 갖는다.
 *
 *   [1] range 목록 — rangeIO / rangeMem / rangePFMem
 *       그 버스가 **쓸 수 있는 주소 창**이다. 하나가 struct range_node 이며
 *       start/end 와 1부터 매기는 rangeno 를 갖는다. 시작 주소 오름차순으로
 *       정렬되어 있고, 개수는 noIORanges 등에 따로 센다.
 *       출처가 둘이다 — 부팅 시 EBDA 가 알려 주는 1차 버스의 창과,
 *       update_bridge_ranges() 가 브리지 config 공간에서 직접 읽어 오는
 *       2차 버스의 창이다.
 *
 *   [2] 자원 목록 — firstIO / firstMem / firstPFMem
 *       그 창 안에서 **이미 쓰이고 있는 구간**이다. 하나가
 *       struct resource_node 이며, 자기가 속한 창의 번호를 rangeno 로 들고
 *       있다. 여기에 firstPFMemFromMem 하나가 더 있는데, PFMEM 창이 없어
 *       MEM 창에서 떼어 쓴 프리페치 자원만 모아 두는 곁가지 목록이다.
 *
 * **빈 공간을 나타내는 자료구조는 없다.** 목록에 있는 것은 "쓰이는 구간"
 * 이고, 빈자리는 *창의 경계와 이웃한 자원 사이의 틈* 으로만 존재한다.
 * ibmphp_check_resource() 가 그 틈을 훑는다.
 *
 * === 자원 목록의 이중 연결(next / nextRange) ===
 * struct resource_node 의 포인터가 둘이라는 점이 이 파일에서 가장 헷갈리는
 * 부분이며, 거의 모든 순회 코드가 아래 관용구로 되어 있다.
 *
 *     if (res->next) res = res->next;
 *     else           res = res->nextRange;
 *
 *   next      — **같은 창 안**의 다음 자원(시작 주소 오름차순).
 *   nextRange — 그 창의 마지막 자원에서 **다음 창의 첫 자원**으로 건너뛴다.
 *
 * 즉 하나의 리스트가 "창별로 묶인 2차원 구조" 를 1차원 포인터 둘로 표현한다.
 * firstIO 하나에서 시작해 위 관용구로 걸으면 그 버스의 모든 IO 자원이
 * 창 순서·주소 순서로 빠짐없이 나온다. 대신 삽입·삭제 코드가 "같은 창인가,
 * 창의 경계인가" 를 매번 따져야 해서 ibmphp_add_resource() 와
 * ibmphp_remove_resource() 가 그토록 긴 분기 덩어리가 된다.
 *
 * 예외가 하나 있다. firstPFMemFromMem 목록은 next 만 쓰고 nextRange 를 쓰지
 * 않으며 정렬도 하지 않는다 — once_over() 의 상류 주석대로 실제 계산은
 * 짝이 되는 MEM 노드로 하기 때문에 이 목록은 "짝을 찾기 위한 색인" 에
 * 가깝다.
 *
 * === 할당 정책: 최선 적합(best fit) + 정렬 ===
 * ibmphp_check_resource() 가 이 파일의 알고리즘 본체다. 창 안의 모든 틈을
 * 네 가지 위치로 나눠 각각 크기를 잰다.
 *   (a) 창의 앞머리 ~ 첫 자원 앞
 *   (b) 같은 창 안에서 이웃한 두 자원 사이
 *   (c) 창의 마지막 자원 뒤 ~ 창의 끝
 *   (d) 자원이 하나도 없는 창(첫 장치이거나 남은 창을 더 볼 때)
 * 그중 **요구 길이가 들어가는 틈 중 가장 작은 것**을 고른다. 커널 코어의
 * allocate_resource() 가 최초 적합에 가까운 것과 대비된다. 딱 맞는 틈
 * (len_cur == res->len)을 만나면 그 자리에서 곧바로 반환해 더 훑지 않는다.
 *
 * 정렬 규칙도 직접 처리한다. 일반 장치는 요구 길이 자체로, 브리지는 상류
 * 주석대로 IO 4KB(IOBRIDGE)·(PF)MEM 1MB(MEMBRIDGE) 로 나눠떨어지는 주소만
 * 쓴다. 맞지 않으면 tmp_start 를 다음 정렬 경계로 밀어 가며 다시 잰다.
 *
 * 길이 셈이 한 군데서 -1 되어 있는 것도 이 함수의 특징이다. 상류 주석대로
 * "2000-2fff, len = 1000" 을 비교하려면 len 이 0xfff 여야 해서, 함수 첫머리에
 * res->len 을 하나 줄이고 성공 반환 직전마다 다시 하나 늘린다. 실패로
 * 빠지는 경로에서는 되돌리지 않는다.
 *
 * === 왜 코어를 쓰지 않았는가 ===
 * 코드에서 근거를 읽을 수 있는 것만 적는다.
 *   - **권위 있는 자료가 EBDA 다.** ibmphp_rsrc_init() 이 채우는 창은 커널이
 *     열거해 만든 struct resource 트리가 아니라 IBM BIOS 표에서 온다. 그
 *     표에는 커널이 모르는 구성(1차 버스별 창 분할, PFMEM 을 MEM 에서 떼어
 *     쓰는 배치)이 담겨 있어 코어 트리에 그대로 얹기 어렵다.
 *   - **표현할 수 없는 정책이 있다.** PFMEM 창이 모자라면 MEM 창에서 떼어
 *     쓰는 처리(firstPFMemFromMem, once_over())는 코어 할당기에 대응하는
 *     개념이 없다.
 *   - **핫플러그 시점의 요구가 다르다.** 코어의 재배치 경로
 *     (drivers/pci/setup-bus.c 의 pci_do_resource_release_and_resize() 등)는
 *     훨씬 뒤에 생겼다. 다만 "당시 코어가 이 동작을 제공하지 않았다" 는
 *     시기적 판단의 1차 근거(설계 문서나 커밋 기록)는 이 트리에서 확인 못 함.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 시기적으로도 접점이 없다 — 이 코드는 2001~2002년 IBM 서버용이고 NVMe
 * 규격은 2011년에 나왔다. 다만 개념적으로는 대응이 있다. NVMe SSD 를
 * 핫플러그로 꽂으면 그 BAR 0(컨트롤러 레지스터)에 줄 메모리 구간을 누군가
 * 골라 주어야 하는데, 요즘은 PCI 코어가 그 일을 하고 이 파일은 같은 일을
 * 자기 장부로 했다. 두 방식을 견주어 읽으면 "핫플러그 자원 할당" 이라는
 * 문제가 커널에서 어떻게 다뤄져 왔는지가 드러난다.
 */

#include <linux/module.h> /* [한국어] MODULE_ 계열 정의. 이 파일은 직접 쓰지 않지만 드라이버 한 벌이 모듈로 빌드되기 위해 필요하다 */
#include <linux/slab.h> /* [한국어] kzalloc_obj/kfree — 버스·창·자원 노드를 모두 이것으로 잡고 놓는다 */
#include <linux/pci.h> /* [한국어] PCI 코어 API 와 PCI_HEADER_TYPE_ 계열 스펙 상수. config 접근은 update_bridge_ranges() 에서만 쓴다 */
#include <linux/list.h> /* [한국어] list_head 와 list_for_each_entry — 전역 gbuses 가 이 연결 리스트다 */
#include <linux/init.h> /* [한국어] __init 섹션 매크로. 부팅 시 한 번만 도는 함수들을 표시한다 */
#include "ibmphp.h" /* [한국어] 이 드라이버 한 벌이 공유하는 헤더. struct bus_node / range_node / resource_node 와 MEM/IO/PFMEM 상수, debug 매크로가 여기 있다 */

static int flags = 0; /* [한국어] 상류 주석대로 테스트용. ibmphp_free_resources() 가 1 로 세우고 ibmphp_print_test() 만 읽는다 — 해제 뒤에도 버스가 남아 있는지 알아채는 용도다 */

static void update_resources(struct bus_node *bus_cur, int type, int rangeno); /* [한국어] 아래 add_bus_range() 가 먼저 부르므로 전방 선언이 필요하다 */
static int once_over(void); /* [한국어] ibmphp_rsrc_init() 이 마지막에 부른다 */
static int remove_ranges(struct bus_node *, struct bus_node *); /* [한국어] ibmphp_remove_bus() 가 부른다 */
static int update_bridge_ranges(struct bus_node **); /* [한국어] ibmphp_rsrc_init() 이 부른다 */
static int add_bus_range(int type, struct range_node *, struct bus_node *); /* [한국어] alloc_bus_range() 가 부른다 */
static void fix_resources(struct bus_node *); /* [한국어] alloc_bus_range() 가 부른다 */
static struct bus_node *find_bus_wprev(u8, struct bus_node **, u8); /* [한국어] 거의 모든 함수가 부르는 버스 탐색기 */

static LIST_HEAD(gbuses); /* [한국어] **이 파일의 유일한 자료구조 뿌리.** 모든 버스 노드가 여기 매달리며, static 이라 바깥에서는 ibmphp_find_res_bus() 를 통해서만 닿는다. 정렬되어 있지 않아 탐색은 늘 선형이다 */

/* [한국어]
 * alloc_error_bus - 창을 아직 모르는 버스 노드를 급히 만들어 목록에 매단다
 *
 * @curr:  EBDA 자원 항목. flag 가 0 일 때 여기서 버스 번호를 꺼낸다.
 * @busno: 직접 지정할 버스 번호. flag 가 1 일 때 쓴다.
 * @flag:  1 이면 busno 를, 0 이면 curr->bus_num 을 버스 번호로 삼는다.
 * @return: 새 버스 노드. 인자가 잘못되었거나 할당에 실패하면 NULL.
 *
 * 이름의 "error" 는 오류 처리라기보다 **순서가 어긋난 상황**을 뜻한다. 두
 * 자리에서 불린다.
 *
 *   1) ibmphp_rsrc_init() — EBDA 목록에서 PCI 장치의 자원이 그 버스의 창
 *      정보보다 **먼저** 나오는 경우. 창을 모르니 rangeno 를 정할 수 없어,
 *      빈 버스 노드를 만들어 자원만 매달아 두고 needXxxUpdate 를 올린다.
 *      나중에 창이 등록되면 fix_resources() 가 rangeno 를 채운다.
 *   2) update_bridge_ranges() — 이전에 ibmphp 가 로드되었을 때 이미 설정해 둔
 *      브리지를 만난 경우. 그 2차 버스의 노드가 없으므로 껍데기만 만들고,
 *      나머지는 NVRAM 경로가 채운다.
 *
 * kzalloc 이라 창 목록과 자원 목록이 모두 NULL/0 으로 시작한다 — 그 상태가
 * 곧 "아직 아무것도 모른다" 를 뜻한다.
 *
 * 첫 조건이 조금 특이하다. curr 가 NULL 이면서 flag 도 0 이면 버스 번호를
 * 정할 방법이 없어 오류다. flag 가 1 이면 curr 가 NULL 이어도 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(부팅 시 __init 경로).
 *
 * 호출 체인:  ibmphp_rsrc_init() / update_bridge_ranges() → [이 함수]
 */
static struct bus_node * __init alloc_error_bus(struct ebda_pci_rsrc *curr, u8 busno, int flag)
{
	struct bus_node *newbus; /* [한국어] 만들 버스 노드 */

	if (!(curr) && !(flag)) { /* [한국어] 버스 번호를 curr 에서도 인자에서도 얻을 수 없으면 */
		err("NULL pointer passed\n"); /* [한국어] 알리고 */
		return NULL; /* [한국어] 만들 수 없다 */
	}

	newbus = kzalloc_obj(struct bus_node); /* [한국어] 0 으로 초기화해 잡는다 — 창 목록과 자원 목록이 모두 비어 있는 상태가 곧 "아직 아무것도 모른다" 는 뜻이다 */
	if (!newbus) /* [한국어] 메모리가 없으면 */
		return NULL; /* [한국어] 그대로 돌아간다 */

	if (flag) /* [한국어] flag 가 1 이면 인자로 받은 번호를 쓰고 */
		newbus->busno = busno; /* [한국어] 그 값을 넣는다 */
	else
		newbus->busno = curr->bus_num; /* [한국어] 그 값을 넣는다 */
	list_add_tail(&newbus->bus_list, &gbuses); /* [한국어] 전역 목록 끝에 매단다. 목록이 정렬되어 있지 않아 끝에 붙이면 된다 */
	return newbus; /* [한국어] 호출자가 이 노드에 자원을 직접 매단다 */
}

/* [한국어]
 * alloc_resources - EBDA 항목 하나를 struct resource_node 로 옮겨 담는다
 *
 * @curr: EBDA 가 알려 준 자원 항목 하나.
 * @return: 채워진 자원 노드. curr 가 NULL 이거나 할당 실패면 NULL.
 *
 * 버스 번호·devfn·시작·끝을 그대로 베끼고 길이만 계산한다. 길이가
 * end - start + 1 인 것은 EBDA 의 end 가 **포함 경계**이기 때문이다 — 이
 * 파일 전체가 그 규약을 따른다.
 *
 * type 은 여기서 정하지 않는다. 호출자가 EBDA 의 rsrc_type 을 보고
 * MEM/PFMEM/IO 중 하나를 직접 넣는다. rangeno 도 비어 있는데, 그것은
 * ibmphp_add_resource() 가 창을 찾아 채우거나 못 찾으면 -1 로 둔다.
 *
 * kzalloc 이라 next/nextRange 가 NULL 로 시작하고, 그 상태가 곧 "아직 어느
 * 목록에도 매달리지 않았다" 는 뜻이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(부팅 시 __init 경로).
 *
 * 호출 체인:  ibmphp_rsrc_init() → [이 함수] → kzalloc_obj()
 */
static struct resource_node * __init alloc_resources(struct ebda_pci_rsrc *curr)
{
	struct resource_node *rs; /* [한국어] 만들 자원 노드 */

	if (!curr) { /* [한국어] 입력이 없으면 */
		err("NULL passed to allocate\n"); /* [한국어] 알리고 */
		return NULL; /* [한국어] 만들 수 없다 */
	}

	rs = kzalloc_obj(struct resource_node); /* [한국어] 0 으로 초기화해 잡는다 — next/nextRange 가 NULL 인 상태가 "어느 목록에도 안 매달렸다" 는 뜻이다 */
	if (!rs) /* [한국어] 메모리가 없으면 */
		return NULL; /* [한국어] 그대로 돌아간다 */

	rs->busno = curr->bus_num; /* [한국어] 이 자원이 속한 버스 */
	rs->devfunc = curr->dev_fun; /* [한국어] 그 버스 위의 장치·함수 번호 */
	rs->start = curr->start_addr; /* [한국어] 구간의 시작 */
	rs->end = curr->end_addr; /* [한국어] 구간의 끝(포함 경계) */
	rs->len = curr->end_addr - curr->start_addr + 1; /* [한국어] 끝이 포함 경계라 +1 을 해야 길이가 된다. 이 파일 전체가 그 규약을 따른다 */
	return rs; /* [한국어] type 과 rangeno 는 호출자가 채운다 */
}

/* [한국어]
 * alloc_bus_range - 1차 버스의 창(range)을 하나 만들고 필요하면 버스도 함께 만든다
 *
 * @new_bus:   버스 노드를 담을 곳. first_bus 가 0 이면 기존 노드를 여기로 받는다.
 * @new_range: 만든 창을 담을 곳.
 * @curr:      EBDA 자원 항목. 창의 시작/끝과 버스 번호가 여기서 온다.
 * @flag:      MEM / PFMEM / IO 중 어느 종류의 창인지.
 * @first_bus: 1 이면 버스 노드도 새로 만든다. 0 이면 *new_bus 의 것을 쓴다.
 * @return: 0 성공, -ENOMEM 은 할당 실패.
 *
 * 이름이 "alloc" 이지만 실제로는 **할당 + 목록 삽입 + 뒤처리** 를 한다.
 *
 * first_bus 가 1 이면 버스 노드를 새로 만들고, 그 버스의 첫 창이므로
 * rangeno 를 1 로 두고 noXxxRanges 를 1 로 세운다.
 *
 * first_bus 가 0 이면 이미 있는 버스에 창을 보태는 것이다. 이때는
 * add_bus_range() 로 주소순 자리에 끼우고(그 안에서 뒤따르는 창들의 rangeno 가
 * 하나씩 밀린다) 개수를 올린 뒤 **fix_resources() 를 부른다**. 새 창이
 * 생겼으니 rangeno 가 -1 로 남아 있던 자원들이 이제 자리를 찾을 수 있기
 * 때문이다.
 *
 * 다만 num_ranges 가 0 인 경우(창 개수를 세어 보니 없었던 경우)에도 rangeno 를
 * 1 로 두고 add_bus_range() 를 건너뛴다 — 그 자리가 곧 목록의 처음이다.
 *
 * 실패 시 되돌리기가 한 가지뿐이다. 창 할당이 실패하면 방금 만든 버스만
 * kfree 하는데, 그 버스는 아직 gbuses 에 매달리지 않았으므로 안전하다
 * (매다는 것은 호출자의 몫이다).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(부팅 시 __init 경로).
 *
 * 호출 체인:  ibmphp_rsrc_init() → [이 함수]
 *               → add_bus_range() → fix_resources()
 */
static int __init alloc_bus_range(struct bus_node **new_bus, struct range_node **new_range, struct ebda_pci_rsrc *curr, int flag, u8 first_bus)
{
	struct bus_node *newbus; /* [한국어] 만들거나 받아 올 버스 노드 */
	struct range_node *newrange; /* [한국어] 만들 창 노드 */
	u8 num_ranges = 0; /* [한국어] 그 버스가 이미 가진 같은 종류 창의 개수. 새 버스이면 0 그대로 쓴다 */

	if (first_bus) { /* [한국어] 버스도 새로 만들어야 하면 */
		newbus = kzalloc_obj(struct bus_node); /* [한국어] 0 으로 초기화해 잡는다 */
		if (!newbus) /* [한국어] 메모리가 없으면 */
			return -ENOMEM; /* [한국어] 그대로 돌아간다 */

		newbus->busno = curr->bus_num; /* [한국어] EBDA 항목의 버스 번호를 넣는다 */
	} else {
		newbus = *new_bus; /* [한국어] 호출자가 찾아 둔 노드를 쓴다 */
		switch (flag) { /* [한국어] 종류에 따라 기존 창 개수를 꺼낸다 */
			case MEM:
				num_ranges = newbus->noMemRanges; /* [한국어] 메모리 창 개수 */
				break;
			case PFMEM:
				num_ranges = newbus->noPFMemRanges; /* [한국어] 프리페치 메모리 창 개수 */
				break;
			case IO:
				num_ranges = newbus->noIORanges; /* [한국어] I/O 창 개수. 이 값이 아래에서 "첫 창인가" 를 가른다 */
				break;
		}
	}

	newrange = kzalloc_obj(struct range_node); /* [한국어] 창 노드를 잡는다 */
	if (!newrange) { /* [한국어] 메모리가 없으면 */
		if (first_bus) /* [한국어] 방금 버스를 새로 만들었을 때만 */
			kfree(newbus); /* [한국어] 그것을 되돌린다. 아직 gbuses 에 매달지 않았으므로 안전하다(매다는 것은 호출자의 몫) */
		return -ENOMEM; /* [한국어] 그대로 돌아간다 */
	}
	newrange->start = curr->start_addr; /* [한국어] 창의 시작 */
	newrange->end = curr->end_addr; /* [한국어] 창의 끝 */

	if (first_bus || (!num_ranges)) /* [한국어] 새 버스이거나 이 종류의 첫 창이면 */
		newrange->rangeno = 1; /* [한국어] 번호가 1 이고 목록에 끼울 것도 없다 */
	else {
		/* need to insert our range */
		add_bus_range(flag, newrange, newbus); /* [한국어] 그렇지 않으면 주소순 자리에 끼운다. 그 안에서 뒤따르는 창들의 번호가 하나씩 밀린다 */
		debug("%d resource Primary Bus inserted on bus %x [%x - %x]\n", flag, newbus->busno, newrange->start, newrange->end);
	}

	switch (flag) {
		case MEM:
			newbus->rangeMem = newrange; /* [한국어] 버스의 메모리 창 목록 머리를 이 창으로 둔다 */
			if (first_bus) /* [한국어] 첫 창이면 */
				newbus->noMemRanges = 1; /* [한국어] 개수를 1 로 세운다 */
			else {
				debug("First Memory Primary on bus %x, [%x - %x]\n", newbus->busno, newrange->start, newrange->end);
				++newbus->noMemRanges; /* [한국어] 개수를 하나 올린다. add_bus_range() 가 이 값을 "밀 대상 수" 로 썼으므로 그 뒤에 올려야 맞는다 */
				fix_resources(newbus); /* [한국어] 새 창이 생겼으니 rangeno 가 -1 로 남아 있던 자원들이 자리를 찾을 수 있다 */
			}
			break;
		case IO:
			newbus->rangeIO = newrange; /* [한국어] 버스의 I/O 창 목록 머리 */
			if (first_bus) /* [한국어] 첫 창이면 */
				newbus->noIORanges = 1; /* [한국어] 개수를 1 로 */
			else {
				debug("First IO Primary on bus %x, [%x - %x]\n", newbus->busno, newrange->start, newrange->end);
				++newbus->noIORanges; /* [한국어] 개수를 올리고 */
				fix_resources(newbus); /* [한국어] -1 자원들을 맞춘다 */
			}
			break;
		case PFMEM:
			newbus->rangePFMem = newrange; /* [한국어] 버스의 프리페치 메모리 창 목록 머리 */
			if (first_bus) /* [한국어] 첫 창이면 */
				newbus->noPFMemRanges = 1; /* [한국어] 개수를 1 로 */
			else {
				debug("1st PFMemory Primary on Bus %x [%x - %x]\n", newbus->busno, newrange->start, newrange->end);
				++newbus->noPFMemRanges; /* [한국어] 개수를 올리고 */
				fix_resources(newbus); /* [한국어] -1 자원들을 맞춘다 */
			}

			break;
	}

	*new_bus = newbus; /* [한국어] 만들었거나 받아 온 버스를 호출자에게 돌려준다 */
	*new_range = newrange; /* [한국어] 만든 창도 돌려준다 — 호출자가 디버그 메시지에 쓴다 */
	return 0; /* [한국어] 창 등록 완료 */
}


/* Notes:
 * 1. The ranges are ordered.  The buses are not ordered.  (First come)
 *
 * 2. If cannot allocate out of PFMem range, allocate from Mem ranges.  PFmemFromMem
 * are not sorted. (no need since use mem node). To not change the entire code, we
 * also add mem node whenever this case happens so as not to change
 * ibmphp_check_mem_resource etc(and since it really is taking Mem resource)
 */

/*****************************************************************************
 * This is the Resource Management initialization function.  It will go through
 * the Resource list taken from EBDA and fill in this module's data structures
 *
 * THIS IS NOT TAKING INTO CONSIDERATION IO RESTRICTIONS OF PRIMARY BUSES,
 * SINCE WE'RE GOING TO ASSUME FOR NOW WE DON'T HAVE THOSE ON OUR BUSES FOR NOW
 *
 * Input: ptr to the head of the resource list from EBDA
 * Output: 0, -1 or error codes
 ***************************************************************************/
/* [한국어]
 * ibmphp_rsrc_init - EBDA 목록을 읽어 이 파일의 장부 전체를 세운다
 *
 * @return: 0 성공. 할당 실패면 -ENOMEM, 하위 함수의 오류는 그대로 올린다.
 *
 * 바로 위 상류 주석이 이 함수의 성격을 밝힌다 — EBDA 에서 가져온 자원 목록을
 * 훑어 이 모듈의 자료구조를 채우는 초기화 함수이며, 1차 버스의 I/O 제약은
 * 고려하지 않는다(그런 버스가 없다고 가정한다).
 *
 * EBDA 항목마다 두 축으로 갈린다.
 *
 *   [1차 버스 자원] rsrc_type 에 PRIMARYBUSMASK 가 있으면 그것은 장치가
 *   쓰는 구간이 아니라 **버스가 쓸 수 있는 창**이다. MEM/PFMEM/IO 세 갈래가
 *   똑같은 모양으로 되어 있다 — 버스 목록이 비었으면 버스와 창을 함께 만들고,
 *   이미 있으면 그 버스를 찾아 창만 보태고, 못 찾으면 새 버스를 만든다.
 *   세 갈래가 alloc_bus_range() 의 flag 만 다른 채 그대로 반복되는 것이
 *   이 함수가 길어진 주된 이유다.
 *
 *   [장치 자원] 그 밖은 장치가 이미 쓰고 있는 구간이다. alloc_resources() 로
 *   노드를 만들고 type 을 정한 뒤 ibmphp_add_resource() 로 등록한다.
 *   등록이 실패하는 경우가 이 함수의 핵심적인 예외 처리다 — 상류 주석대로
 *   **장치 자원이 그 버스의 창 정보보다 EBDA 에서 먼저 나오면** 창을 몰라
 *   rangeno 를 정할 수 없다. 그때 alloc_error_bus() 로 껍데기 버스를 만들고
 *   자원을 firstXxx 에 직접 매단 뒤 rangeno 를 -1 로, needXxxUpdate 를 1 로
 *   둔다. 나중에 창이 등록될 때 fix_resources() 가 그 -1 을 채운다.
 *
 * PCIDEVMASK 확인이 눈에 띈다. 비 PCI 장치도 EBDA 에 들어 있어 디버그
 * 메시지만 남기는데, 걸러 내는 `continue` 가 주석 처리되어 있어 실제로는
 * 그대로 처리된다. 상류 코드 그대로다.
 *
 * RESTYPE 이 셋 중 어느 것도 아닌 "예약" 값이면 상류 주석대로 아무 일도
 * 하지 않는다.
 *
 * 마지막 두 단계가 후처리다. update_bridge_ranges() 로 브리지 뒤 2차 버스의
 * 창을 보태고(EBDA 가 그 정보를 주지 않기 때문이다), once_over() 로 남은
 * -1 을 맞춘다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 부팅 시 한 번(__init).
 *
 * 호출 체인:  ibmphp_core.c 의 초기화 → [이 함수]
 *               → alloc_bus_range() / alloc_resources() / ibmphp_add_resource()
 *               → update_bridge_ranges() → once_over()
 */
int __init ibmphp_rsrc_init(void)
{
	struct ebda_pci_rsrc *curr; /* [한국어] EBDA 목록의 순회 항목 */
	struct range_node *newrange = NULL; /* [한국어] alloc_bus_range() 가 돌려주는 창. 디버그 메시지에만 쓴다 */
	struct bus_node *newbus = NULL; /* [한국어] alloc_bus_range() 가 만든 새 버스 */
	struct bus_node *bus_cur; /* [한국어] 이미 있는 버스를 찾았을 때 담을 곳 */
	struct bus_node *bus_prev; /* [한국어] find_bus_wprev() 가 채우는 앞 노드. 받아만 두고 쓰지는 않는다 */
	struct resource_node *new_io = NULL; /* [한국어] 만든 I/O 자원 노드 */
	struct resource_node *new_mem = NULL; /* [한국어] 만든 메모리 자원 노드 */
	struct resource_node *new_pfmem = NULL; /* [한국어] 만든 프리페치 메모리 자원 노드 */
	int rc; /* [한국어] 하위 함수의 결과 */

	list_for_each_entry(curr, &ibmphp_ebda_pci_rsrc_head,
			    ebda_pci_rsrc_list) { /* [한국어] ibmphp_ebda.c 가 만들어 둔 EBDA 자원 목록을 통째로 훑는다 — 이 함수의 유일한 입력이다 */
		if (!(curr->rsrc_type & PCIDEVMASK)) { /* [한국어] PCI 장치가 아닌 항목이면 */
			/* EBDA still lists non PCI devices, so ignore... */
			debug("this is not a PCI DEVICE in rsrc_init, please take care\n"); /* [한국어] 상류 주석대로 EBDA 에 비 PCI 장치도 들어 있어 알리기만 한다 */
			// continue; /* [한국어] 거르는 continue 가 주석 처리되어 있어 실제로는 그대로 처리된다. 상류 코드 그대로다 */
		}

		/* this is a primary bus resource */
		if (curr->rsrc_type & PRIMARYBUSMASK) { /* [한국어] 상류 주석대로 1차 버스의 자원, 즉 장치가 쓰는 구간이 아니라 **버스가 쓸 수 있는 창**이다 */
			/* memory */
			if ((curr->rsrc_type & RESTYPE) == MMASK) { /* [한국어] 메모리 창이면 */
				/* no bus structure exists in place yet */
				if (list_empty(&gbuses)) { /* [한국어] 아직 버스가 하나도 없으면 */
					rc = alloc_bus_range(&newbus, &newrange, curr, MEM, 1); /* [한국어] 버스와 창을 함께 만든다 */
					if (rc)
						return rc; /* [한국어] 실패하면 그대로 올린다 */
					list_add_tail(&newbus->bus_list, &gbuses); /* [한국어] 만든 버스를 전역 목록에 매단다 — 매다는 것은 alloc_bus_range() 가 아니라 여기서 한다 */
					debug("gbuses = NULL, Memory Primary Bus %x [%x - %x]\n", newbus->busno, newrange->start, newrange->end);
				} else {
					bus_cur = find_bus_wprev(curr->bus_num, &bus_prev, 1); /* [한국어] 이 번호의 버스가 이미 있는지 본다 */
					/* found our bus */
					if (bus_cur) { /* [한국어] 있으면 */
						rc = alloc_bus_range(&bus_cur, &newrange, curr, MEM, 0); /* [한국어] 그 버스에 창만 보탠다(first_bus 0) */
						if (rc)
							return rc; /* [한국어] 실패하면 그대로 올린다 */
					} else {
						/* went through all the buses and didn't find ours, need to create a new bus node */
						rc = alloc_bus_range(&newbus, &newrange, curr, MEM, 1); /* [한국어] 상류 주석대로 새 버스 노드를 만든다 */
						if (rc)
							return rc; /* [한국어] 실패하면 그대로 올린다 */

						list_add_tail(&newbus->bus_list, &gbuses); /* [한국어] 전역 목록에 매단다 */
						debug("New Bus, Memory Primary Bus %x [%x - %x]\n", newbus->busno, newrange->start, newrange->end);
					}
				}
			} else if ((curr->rsrc_type & RESTYPE) == PFMASK) { /* [한국어] 프리페치 메모리 창이면. 아래가 메모리 갈래와 똑같은 모양으로 반복된다 */
				/* prefetchable memory */
				if (list_empty(&gbuses)) { /* [한국어] 아직 버스가 없으면 */
					/* no bus structure exists in place yet */
					rc = alloc_bus_range(&newbus, &newrange, curr, PFMEM, 1); /* [한국어] 버스와 창을 함께 만든다 */
					if (rc)
						return rc; /* [한국어] 실패하면 올린다 */
					list_add_tail(&newbus->bus_list, &gbuses); /* [한국어] 전역 목록에 매단다 */
					debug("gbuses = NULL, PFMemory Primary Bus %x [%x - %x]\n", newbus->busno, newrange->start, newrange->end);
				} else {
					bus_cur = find_bus_wprev(curr->bus_num, &bus_prev, 1); /* [한국어] 이 번호의 버스가 이미 있는지 본다 */
					if (bus_cur) { /* [한국어] 있으면 */
						/* found our bus */
						rc = alloc_bus_range(&bus_cur, &newrange, curr, PFMEM, 0); /* [한국어] 창만 보탠다 */
						if (rc)
							return rc; /* [한국어] 실패하면 올린다 */
					} else {
						/* went through all the buses and didn't find ours, need to create a new bus node */
						rc = alloc_bus_range(&newbus, &newrange, curr, PFMEM, 1); /* [한국어] 새 버스를 만든다 */
						if (rc)
							return rc; /* [한국어] 실패하면 올린다 */
						list_add_tail(&newbus->bus_list, &gbuses); /* [한국어] 전역 목록에 매단다 */
						debug("1st Bus, PFMemory Primary Bus %x [%x - %x]\n", newbus->busno, newrange->start, newrange->end);
					}
				}
			} else if ((curr->rsrc_type & RESTYPE) == IOMASK) { /* [한국어] I/O 창이면. 세 번째 반복이다 */
				/* IO */
				if (list_empty(&gbuses)) { /* [한국어] 아직 버스가 없으면 */
					/* no bus structure exists in place yet */
					rc = alloc_bus_range(&newbus, &newrange, curr, IO, 1); /* [한국어] 버스와 창을 함께 만든다 */
					if (rc)
						return rc; /* [한국어] 실패하면 올린다 */
					list_add_tail(&newbus->bus_list, &gbuses); /* [한국어] 전역 목록에 매단다 */
					debug("gbuses = NULL, IO Primary Bus %x [%x - %x]\n", newbus->busno, newrange->start, newrange->end);
				} else {
					bus_cur = find_bus_wprev(curr->bus_num, &bus_prev, 1); /* [한국어] 이 번호의 버스가 이미 있는지 본다 */
					if (bus_cur) { /* [한국어] 있으면 */
						rc = alloc_bus_range(&bus_cur, &newrange, curr, IO, 0); /* [한국어] 창만 보탠다 */
						if (rc)
							return rc; /* [한국어] 실패하면 올린다 */
					} else {
						/* went through all the buses and didn't find ours, need to create a new bus node */
						rc = alloc_bus_range(&newbus, &newrange, curr, IO, 1); /* [한국어] 새 버스를 만든다 */
						if (rc)
							return rc; /* [한국어] 실패하면 올린다 */
						list_add_tail(&newbus->bus_list, &gbuses); /* [한국어] 전역 목록에 매단다 */
						debug("1st Bus, IO Primary Bus %x [%x - %x]\n", newbus->busno, newrange->start, newrange->end);
					}
				}

			} else {
				;	/* type is reserved  WHAT TO DO IN THIS CASE???
					   NOTHING TO DO??? */ /* [한국어] 상류 주석대로 예약 값이라 아무 일도 하지 않는다 */
			}
		} else {
			/* regular pci device resource */
			if ((curr->rsrc_type & RESTYPE) == MMASK) { /* [한국어] 메모리 자원이면 */
				/* Memory resource */
				new_mem = alloc_resources(curr); /* [한국어] EBDA 항목을 자원 노드로 옮겨 담는다 */
				if (!new_mem) /* [한국어] 메모리가 없으면 */
					return -ENOMEM; /* [한국어] 그대로 돌아간다 */
				new_mem->type = MEM; /* [한국어] 종류를 여기서 정한다 — alloc_resources() 는 채우지 않는다 */
				/*
				 * if it didn't find the bus, means PCI dev
				 * came b4 the Primary Bus info, so need to
				 * create a bus rangeno becomes a problem...
				 * assign a -1 and then update once the range
				 * actually appears...
				 */
				if (ibmphp_add_resource(new_mem) < 0) { /* [한국어] 창을 못 찾으면(위 상류 주석의 상황) 음수가 온다 */
					newbus = alloc_error_bus(curr, 0, 0); /* [한국어] 창을 모르는 버스 노드를 급히 만든다 */
					if (!newbus) /* [한국어] 메모리가 없으면 */
						return -ENOMEM; /* [한국어] 그대로 돌아간다 */
					newbus->firstMem = new_mem; /* [한국어] 자원을 그 버스에 직접 매단다 — 목록 삽입 로직을 거치지 않는다 */
					++newbus->needMemUpdate; /* [한국어] "자리를 못 찾은 자원이 하나 있다" 고 센다 */
					new_mem->rangeno = -1; /* [한국어] 창을 모르므로 -1 로 둔다. 나중에 fix_resources() 가 채운다 */
				}
				debug("Memory resource for device %x, bus %x, [%x - %x]\n", new_mem->devfunc, new_mem->busno, new_mem->start, new_mem->end); /* [한국어] 등록 결과와 무관하게 무엇을 다뤘는지 남긴다 */

			} else if ((curr->rsrc_type & RESTYPE) == PFMASK) { /* [한국어] 프리페치 메모리 자원이면. 메모리 갈래와 같은 모양이다 */
				/* PFMemory resource */
				new_pfmem = alloc_resources(curr); /* [한국어] 자원 노드를 만든다 */
				if (!new_pfmem) /* [한국어] 메모리가 없으면 */
					return -ENOMEM; /* [한국어] 그대로 돌아간다 */
				new_pfmem->type = PFMEM; /* [한국어] 종류를 정하고 */
				new_pfmem->fromMem = 0; /* [한국어] MEM 에서 떼어 쓴 것이 아니라고 표시한다. once_over() 가 나중에 이 값을 바꿀 수 있다 */
				if (ibmphp_add_resource(new_pfmem) < 0) { /* [한국어] 창을 못 찾으면 */
					newbus = alloc_error_bus(curr, 0, 0); /* [한국어] 껍데기 버스를 만든다 */
					if (!newbus)
						return -ENOMEM; /* [한국어] 메모리가 없으면 돌아간다 */
					newbus->firstPFMem = new_pfmem; /* [한국어] 자원을 직접 매단다 */
					++newbus->needPFMemUpdate; /* [한국어] 세어 둔다 */
					new_pfmem->rangeno = -1; /* [한국어] -1 로 둔다 */
				}

				debug("PFMemory resource for device %x, bus %x, [%x - %x]\n", new_pfmem->devfunc, new_pfmem->busno, new_pfmem->start, new_pfmem->end);
			} else if ((curr->rsrc_type & RESTYPE) == IOMASK) { /* [한국어] I/O 자원이면. 세 번째 반복이다 */
				/* IO resource */
				new_io = alloc_resources(curr); /* [한국어] 자원 노드를 만든다 */
				if (!new_io) /* [한국어] 메모리가 없으면 */
					return -ENOMEM; /* [한국어] 그대로 돌아간다 */
				new_io->type = IO; /* [한국어] 종류를 정한다 */

				/*
				 * if it didn't find the bus, means PCI dev
				 * came b4 the Primary Bus info, so need to
				 * create a bus rangeno becomes a problem...
				 * Can assign a -1 and then update once the
				 * range actually appears...
				 */
				if (ibmphp_add_resource(new_io) < 0) { /* [한국어] 창을 못 찾으면(위 상류 주석의 상황) */
					newbus = alloc_error_bus(curr, 0, 0); /* [한국어] 껍데기 버스를 만든다 */
					if (!newbus)
						return -ENOMEM; /* [한국어] 메모리가 없으면 돌아간다 */
					newbus->firstIO = new_io; /* [한국어] 자원을 직접 매단다 */
					++newbus->needIOUpdate; /* [한국어] 세어 둔다 */
					new_io->rangeno = -1; /* [한국어] -1 로 둔다 */
				}
				debug("IO resource for device %x, bus %x, [%x - %x]\n", new_io->devfunc, new_io->busno, new_io->start, new_io->end);
			}
		}
	}

	list_for_each_entry(bus_cur, &gbuses, bus_list) { /* [한국어] EBDA 를 다 읽은 뒤 버스마다 */
		/* This is to get info about PPB resources, since EBDA doesn't put this info into the primary bus info */
		rc = update_bridge_ranges(&bus_cur); /* [한국어] 상류 주석대로 EBDA 가 알려 주지 않는 브리지 뒤 자원 정보를 config 공간에서 직접 읽어 보탠다 */
		if (rc)
			return rc; /* [한국어] 실패하면 그대로 올린다 */
	}
	return once_over();	/* This is to align ranges (so no -1) */ /* [한국어] 마지막에 -1 로 남은 창 번호를 맞춘다. 그 반환값이 곧 이 함수의 결과다 */
}

/********************************************************************************
 * This function adds a range into a sorted list of ranges per bus for a particular
 * range type, it then calls another routine to update the range numbers on the
 * pci devices' resources for the appropriate resource
 *
 * Input: type of the resource, range to add, current bus
 * Output: 0 or -1, bus and range ptrs
 ********************************************************************************/
/* [한국어]
 * add_bus_range - 창을 주소순 목록에 끼우고 뒤따르는 창 번호를 하나씩 민다
 *
 * @type:    MEM / PFMEM / IO.
 * @range:   끼워 넣을 창.
 * @bus_cur: 대상 버스.
 * @return: 늘 0.
 *
 * 바로 위 상류 주석대로, 창을 정렬된 자리에 넣은 뒤 그 뒤 창들의 번호가
 * 밀리므로 자원들의 rangeno 도 함께 고쳐야 한다. 그 두 번째 일을
 * update_resources() 에 맡긴다.
 *
 * 먼저 시작 주소를 기준으로 삽입 위치를 찾으며 count 를 센다. 그 count 가
 * 곧 "앞에 몇 개가 있는가" 이고, 세 경우로 갈린다.
 *
 *   count == 0      — 목록의 맨 앞. 새 창이 1번이 되고, 뒤의 모든 창이 밀린다.
 *                     i_init 을 0 으로 두어 아래 루프가 1번부터 다시 매긴다.
 *   range_cur NULL  — 목록의 맨 뒤. 앞 창의 번호 + 1 을 받고, 밀릴 것이 없어
 *                     **그 자리에서 반환한다**(update_resources 도 부르지 않는다).
 *   그 밖           — 중간. 뒤에 오게 될 창의 번호를 그대로 물려받고, 그 뒤가
 *                     모두 밀린다.
 *
 * 번호 밀기 루프가 noRanges 를 상한으로 도는데, 이때 noRanges 는 **아직
 * 새 창을 세지 않은 개수**다. 호출자(alloc_bus_range 또는
 * update_bridge_ranges)가 이 함수를 부른 뒤에 ++ 하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  alloc_bus_range() / update_bridge_ranges() → [이 함수]
 *               → update_resources()
 */
static int add_bus_range(int type, struct range_node *range, struct bus_node *bus_cur)
{
	struct range_node *range_cur = NULL; /* [한국어] 삽입 위치를 찾으며 걷는 현재 창 */
	struct range_node *range_prev; /* [한국어] 그 앞 창 */
	int count = 0, i_init; /* [한국어] 앞에 놓인 창의 개수와, 번호 밀기 루프의 시작 값 */
	int noRanges = 0; /* [한국어] 이 종류의 기존 창 개수. 아직 새 창을 세기 전의 값이다 */

	switch (type) { /* [한국어] 종류에 따라 목록 머리와 개수를 꺼낸다 */
		case MEM:
			range_cur = bus_cur->rangeMem; /* [한국어] 메모리 창 목록 */
			noRanges = bus_cur->noMemRanges; /* [한국어] 그 개수 */
			break;
		case PFMEM:
			range_cur = bus_cur->rangePFMem; /* [한국어] 프리페치 메모리 창 목록 */
			noRanges = bus_cur->noPFMemRanges; /* [한국어] 그 개수 */
			break;
		case IO:
			range_cur = bus_cur->rangeIO; /* [한국어] I/O 창 목록 */
			noRanges = bus_cur->noIORanges; /* [한국어] 그 개수 */
			break;
	}

	range_prev = NULL; /* [한국어] 앞 창을 아직 못 봤다 */
	while (range_cur) { /* [한국어] 주소순 자리를 찾을 때까지 걷는다 */
		if (range->start < range_cur->start) /* [한국어] 새 창이 더 앞에 오면 */
			break; /* [한국어] 여기가 자리다 */
		range_prev = range_cur; /* [한국어] 앞 창을 갱신하고 */
		range_cur = range_cur->next; /* [한국어] 다음으로 */
		count = count + 1; /* [한국어] 앞에 놓인 개수를 센다. 이 값이 아래 세 갈래를 가른다 */
	}
	if (!count) { /* [한국어] 앞에 아무것도 없으면 목록의 맨 앞이다 */
		/* our range will go at the beginning of the list */
		switch (type) {
			case MEM:
				bus_cur->rangeMem = range; /* [한국어] 메모리 창 목록의 머리를 새 창으로 */
				break;
			case PFMEM:
				bus_cur->rangePFMem = range; /* [한국어] 프리페치 메모리 창 목록의 머리를 */
				break;
			case IO:
				bus_cur->rangeIO = range; /* [한국어] I/O 창 목록의 머리를 */
				break;
		}
		range->next = range_cur; /* [한국어] 옛 머리를 뒤에 잇는다 */
		range->rangeno = 1; /* [한국어] 맨 앞이므로 1번이다 */
		i_init = 0; /* [한국어] 뒤의 모든 창이 밀리므로 1번부터 다시 매긴다 */
	} else if (!range_cur) { /* [한국어] 끝까지 걸었으면 목록의 맨 뒤다 */
		/* our range will go at the end of the list */
		range->next = NULL; /* [한국어] 뒤에 올 것이 없다 */
		range_prev->next = range; /* [한국어] 앞 창에 잇는다 */
		range->rangeno = range_prev->rangeno + 1; /* [한국어] 앞 창의 번호 + 1 */
		return 0; /* [한국어] 밀릴 창이 없으므로 update_resources() 도 부르지 않고 여기서 끝낸다 */
	} else {
		/* the range is in the middle */
		range_prev->next = range; /* [한국어] 앞 창에 잇고 */
		range->next = range_cur; /* [한국어] 뒤 창을 물린다 */
		range->rangeno = range_cur->rangeno; /* [한국어] 뒤에 오게 될 창의 번호를 그대로 물려받는다 */
		i_init = range_prev->rangeno; /* [한국어] 앞 창의 번호부터 뒤가 밀린다 */
	}

	for (count = i_init; count < noRanges; ++count) { /* [한국어] 그 자리부터 목록 끝까지 */
		++range_cur->rangeno; /* [한국어] 창 번호를 하나씩 올린다 */
		range_cur = range_cur->next; /* [한국어] 다음 창으로 */
	}

	update_resources(bus_cur, type, i_init + 1); /* [한국어] 창 번호가 밀린 만큼 자원들의 rangeno 도 고친다 */
	return 0; /* [한국어] 창 삽입 완료 */
}

/*******************************************************************************
 * This routine goes through the list of resources of type 'type' and updates
 * the range numbers that they correspond to.  It was called from add_bus_range fnc
 *
 * Input: bus, type of the resource, the rangeno starting from which to update
 ******************************************************************************/
/* [한국어]
 * update_resources - 창 번호가 밀린 만큼 자원들의 rangeno 도 올린다
 *
 * @bus_cur: 대상 버스.
 * @type:    MEM / PFMEM / IO.
 * @rangeno: 이 번호부터(포함) 뒤의 자원을 모두 올린다.
 *
 * 바로 위 상류 주석대로 add_bus_range() 가 부른다. 창이 중간에 끼면 그 뒤
 * 창들의 번호가 밀리는데, 자원 노드들이 자기 창 번호를 사본으로 들고 있어
 * 따로 고쳐 주어야 한다.
 *
 * 두 단계다.
 *
 *   1) 목록을 걸으며 rangeno 가 인자와 같은 첫 자원을 찾는다. 이때 걷는
 *      방식이 이 파일의 관용구다 — next 가 있으면 같은 창 안의 다음으로,
 *      없으면 nextRange 로 다음 창의 첫 자원으로 건너뛴다. 둘 다 없으면
 *      끝(eol)이다.
 *   2) 찾았으면 거기서부터 rangeno 를 하나씩 올린다.
 *
 * 2)의 루프가 **next 만 따라간다**는 점이 눈에 띈다. 1)에서 찾은 자원이
 * 그 창의 첫 자원이므로 그 창 안은 모두 밀리지만, 그다음 창의 자원들은
 * nextRange 를 타지 않아 이 루프에서 빠진다. 상류 코드 그대로이며, 의도인지
 * 누락인지는 이 트리에서 확인 못 함.
 *
 * 찾지 못하면(eol) 아무 일도 하지 않는다 — 그 번호의 자원이 아직 없다는 뜻이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  add_bus_range() → [이 함수]
 */
static void update_resources(struct bus_node *bus_cur, int type, int rangeno)
{
	struct resource_node *res = NULL; /* [한국어] 목록을 걸을 자원 */
	u8 eol = 0;	/* end of list indicator */ /* [한국어] 목록 끝에 닿았음을 표시한다 */

	switch (type) { /* [한국어] 종류에 따라 자원 목록 머리를 꺼낸다 */
		case MEM:
			if (bus_cur->firstMem)
				res = bus_cur->firstMem; /* [한국어] 메모리 자원 목록 */
			break;
		case PFMEM:
			if (bus_cur->firstPFMem)
				res = bus_cur->firstPFMem; /* [한국어] 프리페치 메모리 자원 목록 */
			break;
		case IO:
			if (bus_cur->firstIO)
				res = bus_cur->firstIO; /* [한국어] I/O 자원 목록 */
			break;
	}

	if (res) { /* [한국어] 자원이 하나도 없으면 고칠 것도 없다 */
		while (res) { /* [한국어] 그 번호의 첫 자원을 찾는다 */
			if (res->rangeno == rangeno) /* [한국어] 찾았으면 */
				break; /* [한국어] 거기서 멈춘다 */
			if (res->next) /* [한국어] 이 파일의 관용구 — 같은 창 안의 다음이 있으면 */
				res = res->next; /* [한국어] 그리로 가고 */
			else if (res->nextRange) /* [한국어] 없고 다음 창의 첫 자원이 있으면 */
				res = res->nextRange; /* [한국어] 그리로 건너뛴다 */
			else {
				eol = 1; /* [한국어] 목록 끝이다 */
				break;
			}
		}

		if (!eol) { /* [한국어] 그 번호의 자원을 찾았으면 */
			/* found the range */
			while (res) { /* [한국어] 거기서부터 */
				++res->rangeno; /* [한국어] 창 번호를 하나씩 올린다 */
				res = res->next; /* [한국어] [관찰] next 만 따라가므로 그다음 창의 자원들은 이 루프에서 빠진다. 상류 코드 그대로다 */
			}
		}
	}
}

/* [한국어]
 * fix_me - rangeno 가 -1 인 자원들을 훑어 들어갈 창을 찾아 준다
 *
 * @res:     훑기 시작할 자원(그 종류의 목록 머리).
 * @bus_cur: 대상 버스. 찾을 때마다 needXxxUpdate 를 하나 내린다.
 * @range:   그 종류의 창 목록 머리.
 *
 * fix_resources() 가 종류마다 한 번씩 부르는 실제 작업 함수다.
 *
 * 자원 하나마다 창 목록을 훑어 **start 와 end 가 모두 그 창 안에 들어가는**
 * 창을 찾는다. 찾으면 그 번호를 자원에 적고, 그 종류의 needXxxUpdate 를
 * 하나 내린다 — 그 값이 0 이 되면 더 고칠 것이 없다는 뜻이라
 * fix_resources() 가 다음부터 이 함수를 건너뛴다.
 *
 * str 은 디버그 메시지에만 쓰인다.
 *
 * [관찰] 안쪽 창 루프에서 range 를 다시 처음으로 되돌리지 않는다. 자원
 * 하나를 처리하며 range 를 앞으로 밀고 나면, 그다음 자원은 남은 창들만 보게
 * 된다. 자원 목록과 창 목록이 모두 주소순이라 대개 문제가 없지만, 앞선
 * 자원이 뒤쪽 창에 들어간 경우 그 뒤 자원은 앞 창을 놓친다. 상류 코드
 * 그대로이며 여기서는 고치지 않는다.
 *
 * 바깥 루프는 이 파일의 관용구대로 next → nextRange 순으로 걷는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  fix_resources() → [이 함수]
 */
static void fix_me(struct resource_node *res, struct bus_node *bus_cur, struct range_node *range)
{
	char *str = ""; /* [한국어] 디버그 메시지에만 쓰는 종류 이름 */
	switch (res->type) { /* [한국어] 종류에 따라 문자열을 고른다 */
		case IO:
			str = "io"; /* [한국어] I/O */
			break;
		case MEM:
			str = "mem"; /* [한국어] 메모리 */
			break;
		case PFMEM:
			str = "pfmem"; /* [한국어] 프리페치 메모리 */
			break;
	}

	while (res) { /* [한국어] 자원 목록을 끝까지 걷는다 */
		if (res->rangeno == -1) { /* [한국어] 아직 창을 못 찾은 자원이면 */
			while (range) { /* [한국어] 창 목록을 훑어 */
				if ((res->start >= range->start) && (res->end <= range->end)) { /* [한국어] 시작과 끝이 **모두** 이 창 안에 들어가면 */
					res->rangeno = range->rangeno; /* [한국어] 그 창의 번호를 적어 준다 */
					debug("%s->rangeno in fix_resources is %d\n", str, res->rangeno); /* [한국어] 어느 자원이 어느 번호를 받았는지 남긴다 */
					switch (res->type) { /* [한국어] 종류에 따라 "고칠 것이 남은 수" 를 하나 내린다 */
						case IO:
							--bus_cur->needIOUpdate; /* [한국어] I/O 카운터 */
							break;
						case MEM:
							--bus_cur->needMemUpdate; /* [한국어] 메모리 카운터 */
							break;
						case PFMEM:
							--bus_cur->needPFMemUpdate; /* [한국어] 프리페치 메모리 카운터. 이 값이 0 이 되면 fix_resources() 가 다음부터 건너뛴다 */
							break;
					}
					break; /* [한국어] 이 자원은 끝났으므로 창 훑기를 멈춘다 */
				}
				range = range->next; /* [한국어] 안 맞으면 다음 창을 본다. [관찰] 다음 자원을 볼 때 range 를 처음으로 되돌리지 않는다 */
			}
		}
		if (res->next) /* [한국어] 이 파일의 관용구 — 같은 창 안의 다음이 있으면 */
			res = res->next; /* [한국어] 그리로 */
		else
			res = res->nextRange; /* [한국어] 다음 창의 첫 자원으로 건너뛴다 */
	}

}

/*****************************************************************************
 * This routine reassigns the range numbers to the resources that had a -1
 * This case can happen only if upon initialization, resources taken by pci dev
 * appear in EBDA before the resources allocated for that bus, since we don't
 * know the range, we assign -1, and this routine is called after a new range
 * is assigned to see the resources with unknown range belong to the added range
 *
 * Input: current bus
 * Output: none, list of resources for that bus are fixed if can be
 *******************************************************************************/
/* [한국어]
 * fix_resources - 새 창이 생긴 뒤 -1 로 남아 있던 자원 번호를 맞춘다
 *
 * @bus_cur: 대상 버스.
 *
 * 바로 위 상류 주석이 배경을 밝힌다 — 초기화 때 장치의 자원이 그 버스의 창
 * 정보보다 EBDA 에서 먼저 나오면 창을 몰라 rangeno 를 -1 로 두는데, 나중에
 * 창이 등록되면 그 -1 들이 자리를 찾을 수 있게 된다. 이 함수가 그 시점에
 * 불린다.
 *
 * 종류마다 needXxxUpdate 가 0 이 아닐 때만 fix_me() 를 부른다. 그 값은
 * "아직 자리를 못 찾은 자원이 몇 개인가" 를 세는 카운터이며,
 * ibmphp_add_resource() 와 ibmphp_rsrc_init() 이 올리고 fix_me() 가 내린다.
 * 그래서 고칠 것이 없으면 목록을 걷지도 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  alloc_bus_range() / update_bridge_ranges() → [이 함수] → fix_me()
 */
static void fix_resources(struct bus_node *bus_cur)
{
	struct range_node *range; /* [한국어] fix_me() 에 넘길 창 목록 머리 */
	struct resource_node *res; /* [한국어] fix_me() 에 넘길 자원 목록 머리 */

	debug("%s - bus_cur->busno = %d\n", __func__, bus_cur->busno); /* [한국어] 어느 버스를 손보는지 남긴다 */

	if (bus_cur->needIOUpdate) { /* [한국어] I/O 에 자리를 못 찾은 자원이 남아 있으면 */
		res = bus_cur->firstIO; /* [한국어] 그 목록 머리와 */
		range = bus_cur->rangeIO; /* [한국어] 창 목록 머리를 꺼내 */
		fix_me(res, bus_cur, range); /* [한국어] 맞춰 준다 */
	}
	if (bus_cur->needMemUpdate) { /* [한국어] 메모리 쪽도 같은 방식으로 */
		res = bus_cur->firstMem; /* [한국어] 목록 머리와 */
		range = bus_cur->rangeMem; /* [한국어] 창 목록을 꺼내 */
		fix_me(res, bus_cur, range); /* [한국어] 맞춘다 */
	}
	if (bus_cur->needPFMemUpdate) { /* [한국어] 프리페치 메모리 쪽도 */
		res = bus_cur->firstPFMem; /* [한국어] 목록 머리와 */
		range = bus_cur->rangePFMem; /* [한국어] 창 목록을 꺼내 */
		fix_me(res, bus_cur, range); /* [한국어] 맞춘다 */
	}
}

/*******************************************************************************
 * This routine adds a resource to the list of resources to the appropriate bus
 * based on their resource type and sorted by their starting addresses.  It assigns
 * the ptrs to next and nextRange if needed.
 *
 * Input: resource ptr
 * Output: ptrs assigned (to the node)
 * 0 or -1
 *******************************************************************************/
/* [한국어]
 * ibmphp_add_resource - 자원 하나를 창별·주소순 목록에 끼워 넣는다
 *
 * @res: 등록할 자원. busno/type/start/end 는 채워져 있어야 한다.
 * @return: 0 성공, -ENODEV 는 버스를 못 찾은 경우, -EINVAL 은 type 이 잘못된 경우.
 *
 * 바로 위 상류 주석대로 자원을 종류별 목록에 시작 주소순으로 넣고,
 * next 와 nextRange 를 알맞게 잇는다. 이 파일의 이중 연결 구조를 실제로
 * 유지하는 곳이라 분기가 많다.
 *
 * 먼저 창을 찾는다. 자원의 [start, end] 를 통째로 품는 창이 있으면 그 번호를
 * 자원에 적는다. 없으면 상류가 느낌표로 강조해 둔 rangeno = -1 경우이며,
 * needXxxUpdate 를 올려 나중에 fix_resources() 가 고치게 한다.
 *
 * 그다음 삽입인데, 큰 갈래가 둘이다.
 *
 *   [그 종류의 첫 자원] firstXxx 에 그대로 꽂고 두 포인터를 NULL 로 둔다.
 *
 *   [그 밖] 목록을 걸으며 **rangeno 가 자기보다 작지 않은 첫 자원** 앞까지
 *   간다. 그 결과로 다시 셋으로 갈린다.
 *     - 끝까지 갔다        : 목록의 맨 뒤. 앞 자원의 nextRange 로 잇는다.
 *                            (창이 다르므로 next 가 아니다)
 *     - 같은 창을 만났다   : 그 창 안에서 다시 주소순 자리를 찾는다.
                           * 그 창의 마지막이면 앞 자원의 next 로 잇고,
 *                              앞 자원이 들고 있던 nextRange 를 **물려받는다** —
 *                              창의 마지막 자원만 nextRange 를 갖기 때문이다.
                           * 중간이나 맨 앞이면 앞 자원의 next(같은 창) 또는
 *                              nextRange(창의 경계)로 잇는다. 앞이 없으면
 *                              firstXxx 자체를 바꾼다.
 *     - 더 큰 창을 만났다  : 이 창의 첫 자원이 된다. 앞이 없으면 firstXxx 를
 *                            바꾸며 옛 머리를 nextRange 로 물고, 있으면 앞
 *                            자원의 nextRange 로 잇는다.
 *
 * 이 분기들이 "next 는 창 안, nextRange 는 창 사이" 라는 규약을 지키기 위한
 * 것이다. 하나라도 어긋나면 이후의 모든 순회가 자원을 건너뛰거나 무한히 돈다.
 *
 * 이 함수는 자리를 **확인하지 않는다**. 겹침 검사는 호출 전에
 * ibmphp_check_resource() 가 하는 일이고, 여기서는 이미 확정된 구간을
 * 장부에 올리기만 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  ibmphp_rsrc_init() / once_over() / update_bridge_ranges() /
 *               ibmphp_pci.c 의 설정 경로 → [이 함수] → find_bus_wprev()
 */
int ibmphp_add_resource(struct resource_node *res)
{
	struct resource_node *res_cur; /* [한국어] 삽입 위치를 찾으며 걷는 현재 자원 */
	struct resource_node *res_prev; /* [한국어] 그 앞 자원 */
	struct bus_node *bus_cur; /* [한국어] 이 자원이 속할 버스 */
	struct range_node *range_cur = NULL; /* [한국어] 창을 찾으며 걷는 현재 창 */
	struct resource_node *res_start = NULL; /* [한국어] 이 종류의 자원 목록 머리 */

	debug("%s - enter\n", __func__);

	if (!res) { /* [한국어] 입력이 없으면 */
		err("NULL passed to add\n"); /* [한국어] 알리고 */
		return -ENODEV; /* [한국어] 그대로 돌아간다 */
	}

	bus_cur = find_bus_wprev(res->busno, NULL, 0); /* [한국어] 이 자원이 속할 버스를 찾는다 */

	if (!bus_cur) { /* [한국어] 없으면 */
		/* didn't find a bus, something's wrong!!! */
		debug("no bus in the system, either pci_dev's wrong or allocation failed\n"); /* [한국어] 장부에 없는 버스라 등록할 수 없다 */
		return -ENODEV; /* [한국어] 그대로 돌아간다 */
	}

	/* Normal case */
	switch (res->type) { /* [한국어] 종류에 따라 창 목록과 자원 목록 머리를 꺼낸다 */
		case IO:
			range_cur = bus_cur->rangeIO; /* [한국어] I/O 창 목록 */
			res_start = bus_cur->firstIO; /* [한국어] I/O 자원 목록 머리 */
			break;
		case MEM:
			range_cur = bus_cur->rangeMem; /* [한국어] 메모리 창 목록 */
			res_start = bus_cur->firstMem; /* [한국어] 메모리 자원 목록 머리 */
			break;
		case PFMEM:
			range_cur = bus_cur->rangePFMem; /* [한국어] 프리페치 메모리 창 목록 */
			res_start = bus_cur->firstPFMem; /* [한국어] 프리페치 메모리 자원 목록 머리 */
			break;
		default: /* [한국어] 종류가 셋 중 어느 것도 아니면 */
			err("cannot read the type of the resource to add... problem\n"); /* [한국어] 알리고 */
			return -EINVAL; /* [한국어] 그대로 돌아간다 */
	}
	while (range_cur) { /* [한국어] 이 자원을 품는 창을 찾는다 */
		if ((res->start >= range_cur->start) && (res->end <= range_cur->end)) { /* [한국어] 시작과 끝이 **모두** 그 창 안에 들어가야 한다 */
			res->rangeno = range_cur->rangeno; /* [한국어] 그 창의 번호를 적어 준다 */
			break; /* [한국어] 찾았으므로 멈춘다 */
		}
		range_cur = range_cur->next; /* [한국어] 다음 창을 본다 */
	}

	/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 * this is again the case of rangeno = -1
	 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	 */

	if (!range_cur) { /* [한국어] 품는 창이 없으면 — 위 상류 주석이 느낌표로 강조한 rangeno = -1 경우다 */
		switch (res->type) { /* [한국어] 종류에 따라 */
			case IO:
				++bus_cur->needIOUpdate; /* [한국어] I/O 의 "자리를 못 찾은 수" 를 올린다 */
				break;
			case MEM:
				++bus_cur->needMemUpdate; /* [한국어] 메모리 쪽 */
				break;
			case PFMEM:
				++bus_cur->needPFMemUpdate; /* [한국어] 프리페치 메모리 쪽 */
				break;
		}
		res->rangeno = -1; /* [한국어] 창을 모른다는 표시. 나중에 fix_resources() 가 채운다 */
	}

	debug("The range is %d\n", res->rangeno); /* [한국어] 어느 창에 들어갔는지 남긴다 */
	if (!res_start) { /* [한국어] 이 종류의 첫 자원이면 */
		/* no first{IO,Mem,Pfmem} on the bus, 1st IO/Mem/Pfmem resource ever */
		switch (res->type) { /* [한국어] 종류에 따라 */
			case IO:
				bus_cur->firstIO = res; /* [한국어] I/O 목록의 머리로 삼는다 */
				break;
			case MEM:
				bus_cur->firstMem = res; /* [한국어] 메모리 목록의 머리로 */
				break;
			case PFMEM:
				bus_cur->firstPFMem = res; /* [한국어] 프리페치 메모리 목록의 머리로 */
				break;
		}
		res->next = NULL; /* [한국어] 뒤에 아무것도 없다 */
		res->nextRange = NULL; /* [한국어] 다음 창도 없다 */
	} else {
		res_cur = res_start; /* [한국어] 목록 머리부터 걷는다 */
		res_prev = NULL; /* [한국어] 앞 자원을 아직 못 봤다 */

		debug("res_cur->rangeno is %d\n", res_cur->rangeno); /* [한국어] 어느 창부터 보는지 남긴다 */

		while (res_cur) { /* [한국어] **창 번호가 자기보다 작지 않은 첫 자원** 앞까지 간다 */
			if (res_cur->rangeno >= res->rangeno) /* [한국어] 그런 자원을 만나면 */
				break; /* [한국어] 거기가 후보 자리다 */
			res_prev = res_cur; /* [한국어] 앞 자원을 갱신하고 */
			if (res_cur->next) /* [한국어] 이 파일의 관용구 — 같은 창 안의 다음이 있으면 */
				res_cur = res_cur->next; /* [한국어] 그리로 */
			else
				res_cur = res_cur->nextRange; /* [한국어] 없으면 다음 창의 첫 자원으로 건너뛴다 */
		}

		if (!res_cur) { /* [한국어] 끝까지 갔으면 목록의 맨 뒤다 */
			/* at the end of the resource list */
			debug("i should be here, [%x - %x]\n", res->start, res->end); /* [한국어] 어느 구간이 맨 뒤로 갔는지 남긴다 */
			res_prev->nextRange = res; /* [한국어] 앞 자원과 **창이 다르므로** next 가 아니라 nextRange 로 잇는다 */
			res->next = NULL; /* [한국어] 같은 창 안에 뒤가 없다 */
			res->nextRange = NULL; /* [한국어] 다음 창도 없다 */
		} else if (res_cur->rangeno == res->rangeno) { /* [한국어] 같은 창을 만났으면 */
			/* in the same range */
			while (res_cur) { /* [한국어] 그 창 안에서 다시 주소순 자리를 찾는다 */
				if (res->start < res_cur->start) /* [한국어] 새 자원이 더 앞이면 */
					break; /* [한국어] 여기가 자리다 */
				res_prev = res_cur; /* [한국어] 앞 자원을 갱신하고 */
				res_cur = res_cur->next; /* [한국어] 같은 창 안에서만 걷는다(next 만) */
			}
			if (!res_cur) { /* [한국어] 그 창의 마지막 자리이면 */
				/* the last resource in this range */
				res_prev->next = res; /* [한국어] 앞 자원의 next 로 잇고 */
				res->next = NULL; /* [한국어] 뒤에 같은 창의 자원은 없다 */
				res->nextRange = res_prev->nextRange; /* [한국어] 앞 자원이 들고 있던 다음 창 링크를 **물려받는다** — 창의 마지막 자원만 nextRange 를 갖기 때문이다 */
				res_prev->nextRange = NULL; /* [한국어] 앞 자원은 더 이상 마지막이 아니므로 끊는다 */
			} else if (res->start < res_cur->start) { /* [한국어] 창의 앞머리나 중간이면 */
				/* at the beginning or middle of the range */
				if (!res_prev)	{ /* [한국어] 앞 자원이 없으면 목록 전체의 머리가 된다 */
					switch (res->type) {
						case IO:
							bus_cur->firstIO = res; /* [한국어] I/O 목록의 머리로 */
							break;
						case MEM:
							bus_cur->firstMem = res; /* [한국어] 메모리 목록의 머리로 */
							break;
						case PFMEM:
							bus_cur->firstPFMem = res; /* [한국어] 프리페치 메모리 목록의 머리로 */
							break;
					}
				} else if (res_prev->rangeno == res_cur->rangeno) /* [한국어] 앞 자원과 창이 같으면 같은 창 링크로 잇고 */
					res_prev->next = res; /* [한국어] 그 자리에 끼운다 */
				else
					res_prev->nextRange = res; /* [한국어] 창 경계 링크로 잇는다 */

				res->next = res_cur; /* [한국어] 뒤 자원을 물린다 */
				res->nextRange = NULL; /* [한국어] 같은 창 안에 끼웠으므로 창 경계 링크는 없다 */
			}
		} else {
			/* this is the case where it is 1st occurrence of the range */
			if (!res_prev) { /* [한국어] 앞 자원이 없으면 목록 전체의 머리가 된다 */
				/* at the beginning of the resource list */
				res->next = NULL; /* [한국어] 같은 창 안에는 아직 아무것도 없다 */
				switch (res->type) { /* [한국어] 종류에 따라 */
					case IO:
						res->nextRange = bus_cur->firstIO; /* [한국어] 옛 머리를 다음 창으로 물고 */
						bus_cur->firstIO = res; /* [한국어] 자기가 머리가 된다 */
						break;
					case MEM:
						res->nextRange = bus_cur->firstMem; /* [한국어] 메모리 쪽도 같은 방식으로 */
						bus_cur->firstMem = res; /* [한국어] 머리를 바꾼다 */
						break;
					case PFMEM:
						res->nextRange = bus_cur->firstPFMem; /* [한국어] 프리페치 메모리 쪽도 */
						bus_cur->firstPFMem = res; /* [한국어] 머리를 바꾼다 */
						break;
				}
			} else if (res_cur->rangeno > res->rangeno) { /* [한국어] 앞 자원이 있고 뒤 창이 더 크면 목록 중간이다 */
				/* in the middle of the resource list */
				res_prev->nextRange = res; /* [한국어] 앞 자원의 창 경계 링크로 잇고 */
				res->next = NULL; /* [한국어] 같은 창 안에는 뒤가 없다 */
				res->nextRange = res_cur; /* [한국어] 뒤 창의 첫 자원을 물린다 */
			}
		}
	}

	debug("%s - exit\n", __func__); /* [한국어] 삽입 끝 */
	return 0; /* [한국어] 이 함수는 겹침을 확인하지 않는다 — 그 일은 ibmphp_check_resource() 가 미리 한다 */
}

/****************************************************************************
 * This routine will remove the resource from the list of resources
 *
 * Input: io, mem, and/or pfmem resource to be deleted
 * Output: modified resource list
 *        0 or error code
 ****************************************************************************/
/* [한국어]
 * ibmphp_remove_resource - 자원 하나를 목록에서 떼어 내고 해제한다
 *
 * @res: 지울 자원. start 와 end 로 찾으므로 같은 노드일 필요는 없다.
 * @return: 0 성공, -ENODEV 는 버스를 못 찾은 경우, -EINVAL 은 목록에 없거나
 *          type 이 잘못된 경우.
 *
 * 바로 위 상류 주석대로 자원을 목록에서 지운다. 인자로 받은 포인터가 아니라
 * **start/end 가 같은 노드**를 찾아 지운다는 점에 주의 — 그래서 호출자가
 * 임시로 만든 노드를 넘겨도 동작한다.
 *
 * 찾지 못했을 때 PFMEM 만 한 번 더 찾는다. 상류 주석대로 그 자원이
 * firstPFMemFromMem 목록에 있을 수 있기 때문이다. 그 경우 처리가 두 겹이다 —
 * 같은 구간을 가진 MEM 노드를 firstMem 에서 찾아 **재귀로 먼저 지우고**,
 * 그다음 PFMemFromMem 목록에서 자기를 떼어 낸다. MEM 창에서 떼어 쓴 PFMEM 은
 * 장부에 MEM 으로도 올라가 있어 둘을 함께 지워야 하기 때문이다.
 *
 * 떼어 내기가 앞뒤 관계에 따라 갈린다.
 *
 *   [머리였다] firstXxx 를 다음으로 옮긴다. next 가 있으면 같은 창의 다음,
 *             없고 nextRange 가 있으면 다음 창의 첫 자원, 둘 다 없으면 NULL.
 *   [중간이다] 앞 자원의 어느 포인터를 고칠지는 **창이 같은지**로 갈린다.
 *             같은 창이면 next, 다르면 nextRange 를 잇는다.
 *             지울 자원이 그 창의 마지막이었으면(next 가 없고 nextRange 만
 *             있으면) 앞 자원의 next 를 끊고 nextRange 를 물려준다 — 그래야
 *             앞 자원이 그 창의 새 마지막이 된다.
 *
 * 마지막 `return 0` 은 위 두 갈래가 모두 반환하므로 도달하지 않는다.
 * 상류 코드 그대로다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 자기 자신을 한 단계 재귀 호출한다.
 *
 * 호출 체인:  ibmphp_pci.c 의 해제 경로 / remove_ranges() → [이 함수]
 */
int ibmphp_remove_resource(struct resource_node *res)
{
	struct bus_node *bus_cur; /* [한국어] 이 자원이 속한 버스 */
	struct resource_node *res_cur = NULL; /* [한국어] 목록을 걸으며 지울 노드를 찾는다 */
	struct resource_node *res_prev; /* [한국어] 그 앞 노드 */
	struct resource_node *mem_cur; /* [한국어] PFMemFromMem 인 경우 짝이 되는 MEM 노드 */
	char *type = ""; /* [한국어] 오류 메시지에 쓸 종류 이름 */

	if (!res)  { /* [한국어] 입력이 없으면 */
		err("resource to remove is NULL\n"); /* [한국어] 알리고 */
		return -ENODEV; /* [한국어] 그대로 돌아간다 */
	}

	bus_cur = find_bus_wprev(res->busno, NULL, 0); /* [한국어] 이 자원이 속한 버스를 찾는다 */

	if (!bus_cur) { /* [한국어] 없으면 */
		err("cannot find corresponding bus of the io resource to remove  bailing out...\n"); /* [한국어] 장부에 없는 버스라 지울 수 없다 */
		return -ENODEV; /* [한국어] 그대로 돌아간다 */
	}

	switch (res->type) { /* [한국어] 종류에 따라 목록 머리와 이름을 꺼낸다 */
		case IO:
			res_cur = bus_cur->firstIO; /* [한국어] I/O 자원 목록 */
			type = "io"; /* [한국어] 메시지용 이름 */
			break;
		case MEM:
			res_cur = bus_cur->firstMem; /* [한국어] 메모리 자원 목록 */
			type = "mem"; /* [한국어] 메시지용 이름 */
			break;
		case PFMEM:
			res_cur = bus_cur->firstPFMem; /* [한국어] 프리페치 메모리 자원 목록 */
			type = "pfmem"; /* [한국어] 메시지용 이름 */
			break;
		default: /* [한국어] 셋 중 어느 것도 아니면 */
			err("unknown type for resource to remove\n"); /* [한국어] 알리고 */
			return -EINVAL; /* [한국어] 그대로 돌아간다 */
	}
	res_prev = NULL; /* [한국어] 앞 노드를 아직 못 봤다 */

	while (res_cur) { /* [한국어] 목록을 걷는다 */
		if ((res_cur->start == res->start) && (res_cur->end == res->end)) /* [한국어] **포인터가 아니라 시작·끝이 같은 노드**를 찾는다 — 호출자가 임시 노드를 넘겨도 되는 이유다 */
			break; /* [한국어] 찾았으므로 멈춘다 */
		res_prev = res_cur; /* [한국어] 앞 노드를 갱신하고 */
		if (res_cur->next) /* [한국어] 이 파일의 관용구 — 같은 창 안의 다음이 있으면 */
			res_cur = res_cur->next; /* [한국어] 그리로 */
		else
			res_cur = res_cur->nextRange; /* [한국어] 없으면 다음 창의 첫 자원으로 건너뛴다 */
	}

	if (!res_cur) { /* [한국어] 목록에 없으면 */
		if (res->type == PFMEM) { /* [한국어] 프리페치 메모리만 한 번 더 찾아본다 */
			/*
			 * case where pfmem might be in the PFMemFromMem list
			 * so will also need to remove the corresponding mem
			 * entry
			 */
			res_cur = bus_cur->firstPFMemFromMem; /* [한국어] 상류 주석대로 MEM 창에서 떼어 쓴 PFMEM 목록을 뒤진다 */
			res_prev = NULL; /* [한국어] 앞 노드를 다시 초기화한다 */

			while (res_cur) { /* [한국어] 그 목록을 걷는다 */
				if ((res_cur->start == res->start) && (res_cur->end == res->end)) { /* [한국어] 같은 구간을 찾으면 */
					mem_cur = bus_cur->firstMem; /* [한국어] **짝이 되는 MEM 노드**를 찾는다 — 그 구간은 장부에 MEM 으로도 올라가 있다 */
					while (mem_cur) { /* [한국어] 메모리 목록을 걷는다 */
						if ((mem_cur->start == res_cur->start)
						    && (mem_cur->end == res_cur->end)) /* [한국어] 구간이 같은 노드를 찾으면 */
							break; /* [한국어] 멈춘다 */
						if (mem_cur->next) /* [한국어] 이 파일의 관용구 — 같은 창 안의 다음이 있으면 */
							mem_cur = mem_cur->next; /* [한국어] 그리로 */
						else
							mem_cur = mem_cur->nextRange; /* [한국어] 없으면 다음 창의 첫 자원으로 */
					}
					if (!mem_cur) { /* [한국어] 짝이 없으면 장부가 어긋난 것이다 */
						err("cannot find corresponding mem node for pfmem...\n"); /* [한국어] 알리고 */
						return -EINVAL; /* [한국어] 그대로 돌아간다 */
					}

					ibmphp_remove_resource(mem_cur); /* [한국어] **재귀 호출**로 MEM 쪽을 먼저 지운다. 그쪽은 정상 목록에 있으므로 위 경로로 처리된다 */
					if (!res_prev) /* [한국어] PFMemFromMem 목록의 머리였으면 */
						bus_cur->firstPFMemFromMem = res_cur->next; /* [한국어] 머리를 다음으로 옮기고 */
					else
						res_prev->next = res_cur->next; /* [한국어] 앞 노드에 다음을 잇는다. 이 목록은 next 만 쓰므로 분기가 단순하다 */
					kfree(res_cur); /* [한국어] 노드를 해제하고 */
					return 0; /* [한국어] 끝낸다 */
				}
				res_prev = res_cur; /* [한국어] 앞 노드를 갱신하고 */
				if (res_cur->next) /* [한국어] [관찰] 이 목록은 nextRange 를 쓰지 않는데도 관용구를 그대로 쓴다 */
					res_cur = res_cur->next; /* [한국어] 그리로 */
				else
					res_cur = res_cur->nextRange; /* [한국어] 없으면 nextRange 로 — 늘 NULL 이라 사실상 루프가 끝난다 */
			}
			if (!res_cur) { /* [한국어] 거기에도 없으면 */
				err("cannot find pfmem to delete...\n"); /* [한국어] 알리고 */
				return -EINVAL; /* [한국어] 그대로 돌아간다 */
			}
		} else {
			err("the %s resource is not in the list to be deleted...\n", type); /* [한국어] 어느 종류를 못 찾았는지 알리고 */
			return -EINVAL; /* [한국어] 그대로 돌아간다 */
		}
	}
	if (!res_prev) { /* [한국어] 지울 노드가 목록의 머리였으면 */
		/* first device to be deleted */
		if (res_cur->next) { /* [한국어] 같은 창에 뒤가 있으면 */
			switch (res->type) { /* [한국어] 종류에 따라 */
				case IO:
					bus_cur->firstIO = res_cur->next; /* [한국어] I/O 목록의 머리를 그리로 옮긴다 */
					break;
				case MEM:
					bus_cur->firstMem = res_cur->next; /* [한국어] 메모리 목록의 머리를 */
					break;
				case PFMEM:
					bus_cur->firstPFMem = res_cur->next; /* [한국어] 프리페치 메모리 목록의 머리를 */
					break;
			}
		} else if (res_cur->nextRange) { /* [한국어] 같은 창에는 없고 다음 창이 있으면 */
			switch (res->type) { /* [한국어] 종류에 따라 */
				case IO:
					bus_cur->firstIO = res_cur->nextRange; /* [한국어] I/O 목록의 머리를 다음 창의 첫 자원으로 */
					break;
				case MEM:
					bus_cur->firstMem = res_cur->nextRange; /* [한국어] 메모리 목록의 머리를 */
					break;
				case PFMEM:
					bus_cur->firstPFMem = res_cur->nextRange; /* [한국어] 프리페치 메모리 목록의 머리를 */
					break;
			}
		} else {
			switch (res->type) { /* [한국어] 종류에 따라 */
				case IO:
					bus_cur->firstIO = NULL; /* [한국어] I/O 목록을 비운다 */
					break;
				case MEM:
					bus_cur->firstMem = NULL; /* [한국어] 메모리 목록을 비운다 */
					break;
				case PFMEM:
					bus_cur->firstPFMem = NULL; /* [한국어] 프리페치 메모리 목록을 비운다 */
					break;
			}
		}
		kfree(res_cur); /* [한국어] 노드를 해제하고 */
		return 0; /* [한국어] 끝낸다 */
	} else {
		if (res_cur->next) { /* [한국어] 같은 창에 뒤가 있으면 */
			if (res_prev->rangeno == res_cur->rangeno) /* [한국어] 앞 노드와 창이 같은지 보고 */
				res_prev->next = res_cur->next; /* [한국어] 같으면 같은 창 링크로 */
			else
				res_prev->nextRange = res_cur->next; /* [한국어] 창 경계 링크로 잇는다 */
		} else if (res_cur->nextRange) { /* [한국어] 같은 창에는 없고 다음 창이 있으면 */
			res_prev->next = NULL; /* [한국어] 앞 노드는 그 창의 마지막이 된다 */
			res_prev->nextRange = res_cur->nextRange; /* [한국어] 다음 창 링크를 물려준다 */
		} else {
			res_prev->next = NULL; /* [한국어] 앞 노드의 두 링크를 모두 끊는다 */
			res_prev->nextRange = NULL; /* [한국어] 그래야 앞 노드가 새 마지막이 된다 */
		}
		kfree(res_cur); /* [한국어] 노드를 해제하고 */
		return 0; /* [한국어] 끝낸다 */
	}

	return 0; /* [한국어] 위 두 갈래가 모두 반환하므로 도달하지 않는다. 상류 코드 그대로다 */
}

/* [한국어]
 * find_range - 자원이 속한 창 노드를 번호로 찾아 준다
 *
 * @bus_cur: 대상 버스.
 * @res:     창 번호(rangeno)를 들고 있는 자원.
 * @return: 그 번호의 창. 없으면 NULL.
 *
 * 자원 노드는 창의 **번호만** 사본으로 들고 있어서, 실제 경계(start/end)를
 * 알려면 매번 이렇게 되찾아야 한다. ibmphp_check_resource() 가 자원마다
 * 이 함수를 부르는 이유다.
 *
 * 번호가 -1 인 자원은 어느 창에도 맞지 않아 NULL 이 돌아오고, 호출자가
 * 그것을 오류로 처리한다.
 *
 * type 이 셋 중 어느 것도 아니면 오류만 찍고 range 가 NULL 인 채로 아래
 * 루프를 건너뛰어 NULL 을 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  ibmphp_check_resource() → [이 함수]
 */
static struct range_node *find_range(struct bus_node *bus_cur, struct resource_node *res)
{
	struct range_node *range = NULL; /* [한국어] 찾을 창 */

	switch (res->type) { /* [한국어] 종류에 따라 창 목록 머리를 꺼낸다 */
		case IO:
			range = bus_cur->rangeIO; /* [한국어] I/O 창 목록 */
			break;
		case MEM:
			range = bus_cur->rangeMem; /* [한국어] 메모리 창 목록 */
			break;
		case PFMEM:
			range = bus_cur->rangePFMem; /* [한국어] 프리페치 메모리 창 목록 */
			break;
		default: /* [한국어] 셋 중 어느 것도 아니면 */
			err("cannot read resource type in find_range\n"); /* [한국어] 알리기만 한다 — range 가 NULL 이라 아래 루프를 건너뛰고 NULL 이 반환된다 */
	}

	while (range) { /* [한국어] 번호가 같은 창을 찾는다 */
		if (res->rangeno == range->rangeno) /* [한국어] 찾으면 */
			break; /* [한국어] 멈춘다 */
		range = range->next; /* [한국어] 다음 창으로 */
	}
	return range; /* [한국어] 못 찾으면 NULL 이 돌아가고, 호출자가 그것을 오류로 처리한다 */
}

/*****************************************************************************
 * This routine will check to make sure the io/mem/pfmem->len that the device asked for
 * can fit w/i our list of available IO/MEM/PFMEM resources.  If cannot, returns -EINVAL,
 * otherwise, returns 0
 *
 * Input: resource
 * Output: the correct start and end address are inputted into the resource node,
 *        0 or -EINVAL
 *****************************************************************************/
/* [한국어]
 * ibmphp_check_resource - 요구 길이가 들어갈 가장 작은 틈을 찾아 시작 주소를 정한다
 *
 * @res:    요구 사항이 담긴 자원. busno/type/len 이 입력이고, 성공하면
 *          start/end 가 채워진다.
 * @bridge: 0 이 아니면 브리지 창을 위한 요청이라 정렬 규칙이 달라진다.
 * @return: 0 성공(res->start/end 가 채워짐), -EINVAL 은 맞는 자리가 없는 경우.
 *
 * **이 파일의 알고리즘 본체**다. 바로 위 상류 주석대로 요구 길이가 이 버스의
 * 가용 창 안에 들어가는지 보고, 들어가면 알맞은 시작·끝 주소를 자원에 적어
 * 준다.
 *
 * 이 함수는 **목록을 바꾸지 않는다.** 자리를 고르기만 하고, 실제 등록은
 * 호출자가 하드웨어에 값을 쓴 뒤 ibmphp_add_resource() 로 따로 한다.
 *
 * [정렬 규칙] 브리지면 상류 주석대로 IO 는 4KB, (PF)MEM 은 1MB 로 나눠떨어지는
 * 주소만 쓴다. 일반 장치는 요구 길이 자체가 정렬 단위다(PCI BAR 의 자연 정렬).
 * 그 값이 tmp_divide 이며, 아래 모든 후보 검사가 이것으로 걸러진다.
 *
 * [길이 셈] 함수 첫머리에서 res->len 을 1 줄인다. 상류 주석대로 "2000-2fff,
 * len = 1000" 을 비교하려면 0xfff 여야 하기 때문이다. 성공 반환 직전마다
 * 다시 1 늘려 균형을 맞추는데(주석 "To restore the balance"), 실패로 빠지는
 * 경로에서는 되돌리지 않는다.
 *
 * [탐색] 자원 목록을 처음부터 끝까지 한 번 걸으며, 각 지점에서 네 가지 틈을
 * 잰다.
 *   (a) !res_prev            — 창의 시작 ~ 첫 자원 앞
 *   (b) !res_cur->next       — 그 창의 마지막 자원 뒤 ~ 창의 끝
 *   (c) 창이 바뀌는 경계     — 새 창의 시작 ~ 그 창 첫 자원 앞
 *   (d) 같은 창 안           — 앞 자원의 끝 ~ 이 자원의 시작
 * 네 갈래의 몸통이 거의 같은 코드를 네 번 되풀이한다 — 틈 길이를 재고,
 * 지금까지 찾은 것보다 **작으면서** 요구를 만족하면 후보로 잡고, 정렬이
 * 어긋나면 tmp_start 를 다음 경계로 밀어 가며 다시 잰다.
 *
 * `(len_tmp < len_cur) || (len_cur == 0)` 이 최선 적합(best fit)을 만드는
 * 조건이다. 커널 코어의 allocate_resource() 가 최초 적합에 가까운 것과 다르다.
 * 그리고 `flag && len_cur == res->len` 이면 **딱 맞는 틈**이라 더 볼 것이
 * 없으므로 그 자리에서 반환한다.
 *
 * [루프 뒤] 자원 목록만으로는 답을 못 찾은 두 경우를 더 본다.
 *   - res_prev 가 없다 : 이 버스에 자원이 하나도 없다. 창 목록만 훑는다.
 *   - res_cur 가 없다  : 목록 끝까지 갔다. 마지막 자원의 창 번호가 전체 창
 *                        수보다 작으면 아직 안 본 빈 창이 있다는 뜻이라 창
 *                        목록을 훑고, 아니면 지금까지의 후보로 결론 낸다.
 * 이 두 덩어리도 위와 같은 모양의 코드를 다시 반복한다.
 *
 * [관찰] (d) 갈래의 길이 계산이 `res_cur->start - 1 - res_prev->end - 1` 로
 * 1 을 두 번 뺀다. 다른 갈래들은 한 번만 빼므로, 같은 창 안의 틈만 실제보다
 * 1 작게 계산된다. 상류 코드 그대로이며 여기서는 고치지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  ibmphp_pci.c 의 BAR 설정 경로 → [이 함수]
 *               → find_bus_wprev() / find_range()
 */
int ibmphp_check_resource(struct resource_node *res, u8 bridge)
{
	struct bus_node *bus_cur; /* [한국어] 이 자원이 놓일 버스 */
	struct range_node *range = NULL; /* [한국어] 지금 보고 있는 자원이 속한 창 */
	struct resource_node *res_prev; /* [한국어] 목록에서 앞 자원 */
	struct resource_node *res_cur = NULL; /* [한국어] 목록을 걸으며 보는 현재 자원 */
	u32 len_cur = 0, start_cur = 0, len_tmp = 0; /* [한국어] 지금까지 찾은 가장 작은 틈의 길이·시작, 그리고 이번에 잰 틈의 길이 */
	int noranges = 0; /* [한국어] 이 종류의 창 개수. 루프 뒤에서 "아직 안 본 창이 있는가" 를 판정한다 */
	u32 tmp_start;		/* this is to make sure start address is divisible by the length needed */ /* [한국어] 시작 주소를 정렬 단위로 맞추기 위한 임시 시작 주소 */
	u32 tmp_divide; /* [한국어] 정렬 단위. 브리지면 4KB/1MB, 일반 장치면 요구 길이 자체다 */
	u8 flag = 0; /* [한국어] 이번 후보가 정렬까지 만족했는지 */

	if (!res) /* [한국어] 입력이 없으면 */
		return -EINVAL; /* [한국어] 그대로 돌아간다 */

	if (bridge) { /* [한국어] 브리지 창을 위한 요청이면 */
		/* The rules for bridges are different, 4K divisible for IO, 1M for (pf)mem*/
		if (res->type == IO) /* [한국어] I/O 이면 */
			tmp_divide = IOBRIDGE; /* [한국어] 상류 주석대로 4KB 단위 */
		else
			tmp_divide = MEMBRIDGE; /* [한국어] 메모리·프리페치 메모리이면 */
	} else
		tmp_divide = res->len; /* [한국어] 일반 장치는 요구 길이 자체가 정렬 단위다 — PCI BAR 의 자연 정렬 규칙이다 */

	bus_cur = find_bus_wprev(res->busno, NULL, 0); /* [한국어] 이 자원이 놓일 버스를 찾는다 */

	if (!bus_cur) { /* [한국어] 없으면 */
		/* didn't find a bus, something's wrong!!! */
		debug("no bus in the system, either pci_dev's wrong or allocation failed\n"); /* [한국어] 장부에 없는 버스라 자리를 고를 수 없다 */
		return -EINVAL; /* [한국어] 그대로 돌아간다 */
	}

	debug("%s - enter\n", __func__);
	debug("bus_cur->busno is %d\n", bus_cur->busno); /* [한국어] 어느 버스에서 고르는지 남긴다 */

	/* This is a quick fix to not mess up with the code very much.  i.e.,
	 * 2000-2fff, len = 1000, but when we compare, we need it to be fff */
	res->len -= 1; /* [한국어] 상류 주석대로 "2000-2fff, len = 1000" 을 비교하려면 0xfff 여야 해서 1 줄인다. 성공 반환 직전마다 다시 늘린다 */

	switch (res->type) { /* [한국어] 종류에 따라 자원 목록 머리와 창 개수를 꺼낸다 */
		case IO:
			res_cur = bus_cur->firstIO; /* [한국어] I/O 자원 목록 */
			noranges = bus_cur->noIORanges; /* [한국어] I/O 창 개수 */
			break;
		case MEM:
			res_cur = bus_cur->firstMem; /* [한국어] 메모리 자원 목록 */
			noranges = bus_cur->noMemRanges; /* [한국어] 메모리 창 개수 */
			break;
		case PFMEM:
			res_cur = bus_cur->firstPFMem; /* [한국어] 프리페치 메모리 자원 목록 */
			noranges = bus_cur->noPFMemRanges; /* [한국어] 프리페치 메모리 창 개수 */
			break;
		default: /* [한국어] 셋 중 어느 것도 아니면 */
			err("wrong type of resource to check\n"); /* [한국어] 알리고 */
			return -EINVAL; /* [한국어] 그대로 돌아간다 */
	}
	res_prev = NULL; /* [한국어] 앞 자원을 아직 못 봤다 */

	while (res_cur) { /* [한국어] 이미 쓰이는 자원을 하나씩 걸으며 그 앞뒤의 틈을 잰다 */
		range = find_range(bus_cur, res_cur); /* [한국어] 이 자원이 속한 창의 실제 경계를 되찾는다 — 자원은 번호만 들고 있다 */
		debug("%s - rangeno = %d\n", __func__, res_cur->rangeno); /* [한국어] 어느 창을 보는지 남긴다 */

		if (!range) { /* [한국어] 번호에 맞는 창이 없으면(rangeno 가 -1 인 자원 등) */
			err("no range for the device exists... bailing out...\n"); /* [한국어] 장부가 어긋난 것이다 */
			return -EINVAL; /* [한국어] 그대로 돌아간다 */
		}

		/* found our range */
		if (!res_prev) { /* [한국어] **(a) 첫 자원 앞** — 창의 시작부터 첫 자원 앞까지의 틈 */
			/* first time in the loop */
			len_tmp = res_cur->start - 1 - range->start; /* [한국어] 그 틈의 길이. 끝이 포함 경계라 -1 을 한다 */

			if ((res_cur->start != range->start) && (len_tmp >= res->len)) { /* [한국어] 첫 자원이 창의 시작과 붙어 있지 않고 틈이 요구를 만족하면 */
				debug("len_tmp = %x\n", len_tmp);

				if ((len_tmp < len_cur) || (len_cur == 0)) { /* [한국어] **최선 적합 조건** — 지금까지 찾은 것보다 작을 때만 후보를 바꾼다 */

					if ((range->start % tmp_divide) == 0) { /* [한국어] 창의 시작이 정렬 단위로 나눠떨어지면 */
						/* just perfect, starting address is divisible by length */
						flag = 1; /* [한국어] 정렬이 맞아떨어지면 그대로 후보로 잡는다 */
						len_cur = len_tmp; /* [한국어] 그 길이와 */
						start_cur = range->start; /* [한국어] 시작을 후보로 기억한다 */
					} else {
						/* Needs adjusting */
						tmp_start = range->start; /* [한국어] 창의 시작에서 출발해 */
						flag = 0; /* [한국어] 정렬이 어긋나면 시작을 다음 경계로 밀어 가며 다시 잰다 */

						while ((len_tmp = res_cur->start - 1 - tmp_start) >= res->len) { /* [한국어] 남은 틈이 요구를 만족하는 동안 */
							if ((tmp_start % tmp_divide) == 0) { /* [한국어] 정렬이 맞으면 */
								flag = 1; /* [한국어] 후보로 잡고 */
								len_cur = len_tmp; /* [한국어] 길이와 */
								start_cur = tmp_start; /* [한국어] 시작을 기억한 뒤 */
								break; /* [한국어] 멈춘다 */
							}
							tmp_start += tmp_divide - tmp_start % tmp_divide; /* [한국어] 다음 정렬 경계로 민다 */
							if (tmp_start >= res_cur->start - 1) /* [한국어] 첫 자원에 닿았으면 */
								break; /* [한국어] 더 볼 것이 없다 */
						}
					}

					if (flag && len_cur == res->len) { /* [한국어] 딱 맞는 틈이면 더 훑지 않고 여기서 끝낸다 */
						debug("but we are not here, right?\n");
						res->start = start_cur; /* [한국어] 고른 시작을 자원에 적는다 */
						res->len += 1; /* To restore the balance */ /* [한국어] 함수 첫머리에서 1 줄여 둔 길이를 되돌린다 */
						res->end = res->start + res->len - 1; /* [한국어] 끝은 시작 + 길이 - 1 */
						return 0; /* [한국어] 성공 */
					}
				}
			}
		}
		if (!res_cur->next) { /* [한국어] **(b) 이 창의 마지막 자원 뒤** — 그 자원 끝부터 창의 끝까지 */
			/* last device on the range */
			len_tmp = range->end - (res_cur->end + 1); /* [한국어] 그 틈의 길이 */

			if ((range->end != res_cur->end) && (len_tmp >= res->len)) { /* [한국어] 마지막 자원이 창의 끝과 붙어 있지 않고 틈이 요구를 만족하면 */
				debug("len_tmp = %x\n", len_tmp);
				if ((len_tmp < len_cur) || (len_cur == 0)) { /* [한국어] 최선 적합 조건 */

					if (((res_cur->end + 1) % tmp_divide) == 0) { /* [한국어] 자원 바로 뒤가 정렬 단위로 나눠떨어지면 */
						/* just perfect, starting address is divisible by length */
						flag = 1; /* [한국어] 정렬이 맞아떨어지면 그대로 후보로 잡는다 */
						len_cur = len_tmp; /* [한국어] 그 길이와 */
						start_cur = res_cur->end + 1; /* [한국어] 시작을 기억한다 */
					} else {
						/* Needs adjusting */
						tmp_start = res_cur->end + 1; /* [한국어] 자원 바로 뒤에서 출발해 */
						flag = 0; /* [한국어] 정렬이 어긋나면 시작을 다음 경계로 밀어 가며 다시 잰다 */

						while ((len_tmp = range->end - tmp_start) >= res->len) { /* [한국어] 남은 틈이 요구를 만족하는 동안 */
							if ((tmp_start % tmp_divide) == 0) { /* [한국어] 정렬이 맞으면 */
								flag = 1; /* [한국어] 후보로 잡고 */
								len_cur = len_tmp; /* [한국어] 길이와 */
								start_cur = tmp_start; /* [한국어] 시작을 기억한 뒤 */
								break; /* [한국어] 멈춘다 */
							}
							tmp_start += tmp_divide - tmp_start % tmp_divide; /* [한국어] 다음 정렬 경계로 민다 */
							if (tmp_start >= range->end) /* [한국어] 창의 끝에 닿았으면 */
								break; /* [한국어] 더 볼 것이 없다 */
						}
					}
					if (flag && len_cur == res->len) { /* [한국어] 딱 맞는 틈이면 더 훑지 않고 여기서 끝낸다 */
						res->start = start_cur; /* [한국어] 고른 시작을 적고 */
						res->len += 1; /* To restore the balance */ /* [한국어] 함수 첫머리에서 1 줄여 둔 길이를 되돌린다 */
						res->end = res->start + res->len - 1; /* [한국어] 끝을 계산한 뒤 */
						return 0; /* [한국어] 성공 */
					}
				}
			}
		}

		if (res_prev) { /* [한국어] 앞 자원이 있으면 그 사이도 본다 */
			if (res_prev->rangeno != res_cur->rangeno) { /* [한국어] 앞 자원과 창이 다르면 */
				/* 1st device on this range */
				/* [한국어] **(c) 창이 바뀌는 경계** — 새 창의 시작부터 그 창 첫 자원 앞까지 */
				len_tmp = res_cur->start - 1 - range->start; /* [한국어] 그 틈의 길이 */

				if ((res_cur->start != range->start) &&	(len_tmp >= res->len)) { /* [한국어] 창의 시작과 붙어 있지 않고 틈이 요구를 만족하면 */
					if ((len_tmp < len_cur) || (len_cur == 0)) { /* [한국어] 최선 적합 조건 */
						if ((range->start % tmp_divide) == 0) { /* [한국어] 창의 시작이 정렬 단위로 나눠떨어지면 */
							/* just perfect, starting address is divisible by length */
							flag = 1; /* [한국어] 정렬이 맞아떨어지면 그대로 후보로 잡는다 */
							len_cur = len_tmp; /* [한국어] 그 길이와 */
							start_cur = range->start; /* [한국어] 시작을 기억한다 */
						} else {
							/* Needs adjusting */
							tmp_start = range->start; /* [한국어] 창의 시작에서 출발해 */
							flag = 0; /* [한국어] 정렬이 어긋나면 시작을 다음 경계로 밀어 가며 다시 잰다 */

							while ((len_tmp = res_cur->start - 1 - tmp_start) >= res->len) { /* [한국어] 남은 틈이 요구를 만족하는 동안 */
								if ((tmp_start % tmp_divide) == 0) { /* [한국어] 정렬이 맞으면 */
									flag = 1; /* [한국어] 후보로 잡고 */
									len_cur = len_tmp; /* [한국어] 길이와 */
									start_cur = tmp_start; /* [한국어] 시작을 기억한 뒤 */
									break; /* [한국어] 멈춘다 */
								}
								tmp_start += tmp_divide - tmp_start % tmp_divide; /* [한국어] 다음 정렬 경계로 민다 */
								if (tmp_start >= res_cur->start - 1) /* [한국어] 이 창의 첫 자원에 닿았으면 */
									break; /* [한국어] 더 볼 것이 없다 */
							}
						}

						if (flag && len_cur == res->len) { /* [한국어] 딱 맞는 틈이면 더 훑지 않고 여기서 끝낸다 */
							res->start = start_cur; /* [한국어] 고른 시작을 적고 */
							res->len += 1; /* To restore the balance */ /* [한국어] 함수 첫머리에서 1 줄여 둔 길이를 되돌린다 */
							res->end = res->start + res->len - 1; /* [한국어] 끝을 계산한 뒤 */
							return 0; /* [한국어] 성공 */
						}
					}
				}
			} else {
				/* [한국어] **(d) 같은 창 안의 두 자원 사이** */
				len_tmp = res_cur->start - 1 - res_prev->end - 1; /* [한국어] [관찰] 1 을 두 번 뺀다. 다른 세 갈래는 한 번만 빼므로 이 갈래의 틈만 실제보다 1 작게 계산된다. 상류 코드 그대로다 */

				if (len_tmp >= res->len) { /* [한국어] 틈이 요구를 만족하면 */
					if ((len_tmp < len_cur) || (len_cur == 0)) { /* [한국어] 최선 적합 조건 */
						if (((res_prev->end + 1) % tmp_divide) == 0) { /* [한국어] 앞 자원 바로 뒤가 정렬 단위로 나눠떨어지면 */
							/* just perfect, starting address's divisible by length */
							flag = 1; /* [한국어] 정렬이 맞아떨어지면 그대로 후보로 잡는다 */
							len_cur = len_tmp; /* [한국어] 그 길이와 */
							start_cur = res_prev->end + 1; /* [한국어] 시작을 기억한다 */
						} else {
							/* Needs adjusting */
							tmp_start = res_prev->end + 1; /* [한국어] 앞 자원 바로 뒤에서 출발해 */
							flag = 0; /* [한국어] 정렬이 어긋나면 시작을 다음 경계로 밀어 가며 다시 잰다 */

							while ((len_tmp = res_cur->start - 1 - tmp_start) >= res->len) { /* [한국어] 남은 틈이 요구를 만족하는 동안 */
								if ((tmp_start % tmp_divide) == 0) { /* [한국어] 정렬이 맞으면 */
									flag = 1; /* [한국어] 후보로 잡고 */
									len_cur = len_tmp; /* [한국어] 길이와 */
									start_cur = tmp_start; /* [한국어] 시작을 기억한 뒤 */
									break; /* [한국어] 멈춘다 */
								}
								tmp_start += tmp_divide - tmp_start % tmp_divide; /* [한국어] 다음 정렬 경계로 민다 */
								if (tmp_start >= res_cur->start - 1) /* [한국어] 다음 자원에 닿았으면 */
									break; /* [한국어] 더 볼 것이 없다 */
							}
						}

						if (flag && len_cur == res->len) { /* [한국어] 딱 맞는 틈이면 더 훑지 않고 여기서 끝낸다 */
							res->start = start_cur; /* [한국어] 고른 시작을 적고 */
							res->len += 1; /* To restore the balance */ /* [한국어] 함수 첫머리에서 1 줄여 둔 길이를 되돌린다 */
							res->end = res->start + res->len - 1; /* [한국어] 끝을 계산한 뒤 */
							return 0; /* [한국어] 성공 */
						}
					}
				}
			}
		}
		/* end if (res_prev) */
		res_prev = res_cur; /* [한국어] 앞 자원을 갱신하고 */
		if (res_cur->next) /* [한국어] 이 파일의 관용구 — 같은 창 안의 다음이 있으면 */
			res_cur = res_cur->next; /* [한국어] 그리로 */
		else
			res_cur = res_cur->nextRange; /* [한국어] 없으면 다음 창의 첫 자원으로 건너뛴다 */
	}	/* end of while */


	if (!res_prev) { /* [한국어] 자원을 하나도 못 봤으면 이 버스의 첫 장치다 */
		/* 1st device ever */
		/* need to find appropriate range */
		switch (res->type) { /* [한국어] 창 목록만 훑으면 된다 */
			case IO:
				range = bus_cur->rangeIO; /* [한국어] I/O 창 목록 */
				break;
			case MEM:
				range = bus_cur->rangeMem; /* [한국어] 메모리 창 목록 */
				break;
			case PFMEM:
				range = bus_cur->rangePFMem; /* [한국어] 프리페치 메모리 창 목록 */
				break;
		}
		while (range) { /* [한국어] **(d) 자원이 하나도 없는 창** — 창 전체가 빈 틈이다 */
			len_tmp = range->end - range->start; /* [한국어] 창 전체의 길이. 여기는 -1 이 없다 — 이미 포함 경계끼리의 차이다 */

			if (len_tmp >= res->len) { /* [한국어] 요구를 만족하면 */
				if ((len_tmp < len_cur) || (len_cur == 0)) { /* [한국어] 최선 적합 조건 */
					if ((range->start % tmp_divide) == 0) { /* [한국어] 창의 시작이 정렬 단위로 나눠떨어지면 */
						/* just perfect, starting address's divisible by length */
						flag = 1; /* [한국어] 정렬이 맞아떨어지면 그대로 후보로 잡는다 */
						len_cur = len_tmp; /* [한국어] 그 길이와 */
						start_cur = range->start; /* [한국어] 시작을 기억한다 */
					} else {
						/* Needs adjusting */
						tmp_start = range->start; /* [한국어] 창의 시작에서 출발해 */
						flag = 0; /* [한국어] 정렬이 어긋나면 시작을 다음 경계로 밀어 가며 다시 잰다 */

						while ((len_tmp = range->end - tmp_start) >= res->len) { /* [한국어] 남은 틈이 요구를 만족하는 동안 */
							if ((tmp_start % tmp_divide) == 0) { /* [한국어] 정렬이 맞으면 */
								flag = 1; /* [한국어] 후보로 잡고 */
								len_cur = len_tmp; /* [한국어] 길이와 */
								start_cur = tmp_start; /* [한국어] 시작을 기억한 뒤 */
								break; /* [한국어] 멈춘다 */
							}
							tmp_start += tmp_divide - tmp_start % tmp_divide; /* [한국어] 다음 정렬 경계로 민다 */
							if (tmp_start >= range->end) /* [한국어] 창의 끝에 닿았으면 */
								break; /* [한국어] 더 볼 것이 없다 */
						}
					}

					if (flag && len_cur == res->len) { /* [한국어] 딱 맞는 틈이면 더 훑지 않고 여기서 끝낸다 */
						res->start = start_cur; /* [한국어] 고른 시작을 적고 */
						res->len += 1; /* To restore the balance */ /* [한국어] 함수 첫머리에서 1 줄여 둔 길이를 되돌린다 */
						res->end = res->start + res->len - 1; /* [한국어] 끝을 계산한 뒤 */
						return 0; /* [한국어] 성공 */
					}
				}
			}
			range = range->next; /* [한국어] 다음 창을 본다 */
		}		/* end of while */

		if ((!range) && (len_cur == 0)) { /* [한국어] 창을 다 훑었는데 후보가 하나도 없으면 */
			/* have gone through the list of devices and ranges and haven't found n.e.thing */
			err("no appropriate range.. bailing out...\n"); /* [한국어] 맞는 자리가 없다 */
			return -EINVAL; /* [한국어] 실패로 돌아간다. [관찰] 이 경로에서는 줄여 둔 res->len 을 되돌리지 않는다 */
		} else if (len_cur) { /* [한국어] 후보가 있으면 그중 가장 작은 것을 쓴다 */
			res->start = start_cur; /* [한국어] 그 시작을 적고 */
			res->len += 1; /* To restore the balance */ /* [한국어] 함수 첫머리에서 1 줄여 둔 길이를 되돌린다 */
			res->end = res->start + res->len - 1; /* [한국어] 끝을 계산한 뒤 */
			return 0; /* [한국어] 성공 */
		}
	}

	if (!res_cur) { /* [한국어] 자원 목록을 끝까지 걸었으면 */
		debug("prev->rangeno = %d, noranges = %d\n", res_prev->rangeno, noranges); /* [한국어] 마지막 자원의 창 번호와 전체 창 수를 남긴다 */
		if (res_prev->rangeno < noranges) { /* [한국어] 상류 주석대로 아직 안 본 창이 남아 있으면 — 마지막 자원이 마지막 창에 없다는 뜻이다 */
			/* if there're more ranges out there to check */
			switch (res->type) { /* [한국어] 종류에 따라 창 목록 머리를 꺼낸다 */
				case IO:
					range = bus_cur->rangeIO; /* [한국어] I/O 창 목록 */
					break;
				case MEM:
					range = bus_cur->rangeMem; /* [한국어] 메모리 창 목록 */
					break;
				case PFMEM:
					range = bus_cur->rangePFMem; /* [한국어] 프리페치 메모리 창 목록 */
					break;
			}
			while (range) { /* [한국어] 창을 처음부터 다시 훑는다 — 위 (d) 갈래와 같은 코드다 */
				len_tmp = range->end - range->start; /* [한국어] 창 전체의 길이 */

				if (len_tmp >= res->len) { /* [한국어] 요구를 만족하면 */
					if ((len_tmp < len_cur) || (len_cur == 0)) { /* [한국어] 최선 적합 조건 */
						if ((range->start % tmp_divide) == 0) { /* [한국어] 창의 시작이 정렬 단위로 나눠떨어지면 */
							/* just perfect, starting address's divisible by length */
							flag = 1; /* [한국어] 정렬이 맞아떨어지면 그대로 후보로 잡는다 */
							len_cur = len_tmp; /* [한국어] 그 길이와 */
							start_cur = range->start; /* [한국어] 시작을 기억한다 */
						} else {
							/* Needs adjusting */
							tmp_start = range->start; /* [한국어] 창의 시작에서 출발해 */
							flag = 0; /* [한국어] 정렬이 어긋나면 시작을 다음 경계로 밀어 가며 다시 잰다 */

							while ((len_tmp = range->end - tmp_start) >= res->len) { /* [한국어] 남은 틈이 요구를 만족하는 동안 */
								if ((tmp_start % tmp_divide) == 0) { /* [한국어] 정렬이 맞으면 */
									flag = 1; /* [한국어] 후보로 잡고 */
									len_cur = len_tmp; /* [한국어] 길이와 */
									start_cur = tmp_start; /* [한국어] 시작을 기억한 뒤 */
									break; /* [한국어] 멈춘다 */
								}
								tmp_start += tmp_divide - tmp_start % tmp_divide; /* [한국어] 다음 정렬 경계로 민다 */
								if (tmp_start >= range->end) /* [한국어] 창의 끝에 닿았으면 */
									break; /* [한국어] 더 볼 것이 없다 */
							}
						}

						if (flag && len_cur == res->len) { /* [한국어] 딱 맞는 틈이면 더 훑지 않고 여기서 끝낸다 */
							res->start = start_cur; /* [한국어] 고른 시작을 적고 */
							res->len += 1; /* To restore the balance */ /* [한국어] 함수 첫머리에서 1 줄여 둔 길이를 되돌린다 */
							res->end = res->start + res->len - 1; /* [한국어] 끝을 계산한 뒤 */
							return 0; /* [한국어] 성공 */
						}
					}
				}
				range = range->next; /* [한국어] 다음 창을 본다 */
			}	/* end of while */

			if ((!range) && (len_cur == 0)) { /* [한국어] 창을 다 훑었는데 후보가 하나도 없으면 */
				/* have gone through the list of devices and ranges and haven't found n.e.thing */
				err("no appropriate range.. bailing out...\n"); /* [한국어] 맞는 자리가 없다 */
				return -EINVAL; /* [한국어] 실패로 돌아간다 */
			} else if (len_cur) { /* [한국어] 후보가 있으면 */
				res->start = start_cur; /* [한국어] 그 시작을 적고 */
				res->len += 1; /* To restore the balance */ /* [한국어] 함수 첫머리에서 1 줄여 둔 길이를 되돌린다 */
				res->end = res->start + res->len - 1; /* [한국어] 끝을 계산한 뒤 */
				return 0; /* [한국어] 성공 */
			}
		} else {
			/* no more ranges to check on */
			if (len_cur) { /* [한국어] 앞서 잰 틈 중 후보가 있으면 */
				res->start = start_cur; /* [한국어] 그 시작을 적고 */
				res->len += 1; /* To restore the balance */ /* [한국어] 함수 첫머리에서 1 줄여 둔 길이를 되돌린다 */
				res->end = res->start + res->len - 1; /* [한국어] 끝을 계산한 뒤 */
				return 0; /* [한국어] 성공 */
			} else {
				/* have gone through the list of devices and haven't found n.e.thing */
				err("no appropriate range.. bailing out...\n"); /* [한국어] 맞는 자리가 없다 */
				return -EINVAL; /* [한국어] 실패로 돌아간다 */
			}
		}
	}	/* end if (!res_cur) */
	return -EINVAL; /* [한국어] 위 갈래들이 모두 반환하므로 사실상 도달하지 않는다. 상류 코드 그대로다 */
}

/********************************************************************************
 * This routine is called from remove_card if the card contained PPB.
 * It will remove all the resources on the bus as well as the bus itself
 * Input: Bus
 * Output: 0, -ENODEV
 ********************************************************************************/
/* [한국어]
 * ibmphp_remove_bus - 브리지 카드를 뽑을 때 그 뒤 버스를 통째로 지운다
 *
 * @bus:         지울 버스.
 * @parent_busno: 그 위쪽(1차) 버스 번호.
 * @return: 0 성공, -ENODEV 는 부모 버스를 못 찾은 경우, 그 밖에는 하위 오류.
 *
 * 바로 위 상류 주석대로 카드에 PPB(PCI-PCI 브리지)가 있었을 때 remove_card
 * 경로에서 불린다.
 *
 * 순서가 요점이다. 먼저 remove_ranges() 로 **창을 지우면서 부모 버스에
 * 등록되어 있던 대응 자원까지 함께 지운다** — 2차 버스의 창은 1차 버스 쪽에서
 * 보면 브리지가 차지한 자원이기 때문이다. 그다음 이 버스의 자원 목록 넷을
 * 모두 해제하고, 마지막에 버스 자신을 gbuses 에서 떼어 낸다.
 *
 * 자원 목록 셋(IO/Mem/PFMem)은 이 파일의 관용구대로 next → nextRange 로
 * 걷고, firstPFMemFromMem 만 next 로만 걷는다 — 그 목록은 nextRange 를 쓰지
 * 않기 때문이다.
 *
 * 해제한 뒤 firstXxx 를 NULL 로 되돌리는데, 바로 아래에서 버스 자체를
 * kfree 하므로 실질적인 효과는 없다. 상류 코드 그대로다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(카드 제거).
 *
 * 호출 체인:  ibmphp_pci.c 의 제거 경로 → [이 함수] → remove_ranges()
 */
int ibmphp_remove_bus(struct bus_node *bus, u8 parent_busno)
{
	struct resource_node *res_cur; /* [한국어] 해제하며 걷는 현재 자원 */
	struct resource_node *res_tmp; /* [한국어] 해제할 노드를 잠시 붙잡아 두는 자리 */
	struct bus_node *prev_bus; /* [한국어] 이 버스의 부모(1차) 버스 */
	int rc; /* [한국어] 하위 함수의 결과 */

	prev_bus = find_bus_wprev(parent_busno, NULL, 0); /* [한국어] 부모 버스를 찾는다 — 그 장부에서도 이 버스의 창을 지워야 한다 */

	if (!prev_bus) { /* [한국어] 없으면 */
		debug("something terribly wrong. Cannot find parent bus to the one to remove\n"); /* [한국어] 장부가 어긋난 것이다 */
		return -ENODEV; /* [한국어] 그대로 돌아간다 */
	}

	debug("In ibmphp_remove_bus... prev_bus->busno is %x\n", prev_bus->busno); /* [한국어] 어느 부모 아래를 지우는지 남긴다 */

	rc = remove_ranges(bus, prev_bus); /* [한국어] **먼저** 창을 지우면서 부모 쪽 대응 자원까지 함께 지운다 */
	if (rc) /* [한국어] 실패하면 */
		return rc; /* [한국어] 그대로 올린다 — 아래 해제는 하지 않는다 */

	if (bus->firstIO) { /* [한국어] I/O 자원이 있으면 */
		res_cur = bus->firstIO; /* [한국어] 목록 머리부터 */
		while (res_cur) { /* [한국어] 끝까지 걷는다 */
			res_tmp = res_cur; /* [한국어] 해제할 노드를 붙잡고 */
			if (res_cur->next) /* [한국어] 이 파일의 관용구 — 같은 창 안의 다음이 있으면 그리로 */
				res_cur = res_cur->next; /* [한국어] 그리로 */
			else
				res_cur = res_cur->nextRange; /* [한국어] 없으면 다음 창의 첫 자원으로 건너뛴다 */
			kfree(res_tmp); /* [한국어] 붙잡아 둔 노드를 해제한다 */
			res_tmp = NULL; /* [한국어] 해제한 포인터를 지운다 */
		}
		bus->firstIO = NULL; /* [한국어] 목록을 비운다. 곧 버스도 해제하므로 실질적 효과는 없다 */
	}
	if (bus->firstMem) { /* [한국어] 메모리 자원이 있으면. 아래가 I/O 와 똑같은 모양이다 */
		res_cur = bus->firstMem; /* [한국어] 목록 머리부터 */
		while (res_cur) { /* [한국어] 끝까지 걷는다 */
			res_tmp = res_cur; /* [한국어] 해제할 노드를 붙잡고 */
			if (res_cur->next) /* [한국어] 이 파일의 관용구 — 같은 창 안의 다음이 있으면 그리로 */
				res_cur = res_cur->next; /* [한국어] 그리로 */
			else
				res_cur = res_cur->nextRange; /* [한국어] 없으면 다음 창으로 */
			kfree(res_tmp); /* [한국어] 해제한다 */
			res_tmp = NULL; /* [한국어] 포인터를 지운다 */
		}
		bus->firstMem = NULL; /* [한국어] 목록을 비운다 */
	}
	if (bus->firstPFMem) { /* [한국어] 프리페치 메모리 자원이 있으면. 세 번째 반복이다 */
		res_cur = bus->firstPFMem; /* [한국어] 목록 머리부터 */
		while (res_cur) { /* [한국어] 끝까지 걷는다 */
			res_tmp = res_cur; /* [한국어] 해제할 노드를 붙잡고 */
			if (res_cur->next) /* [한국어] 이 파일의 관용구 — 같은 창 안의 다음이 있으면 그리로 */
				res_cur = res_cur->next; /* [한국어] 그리로 */
			else
				res_cur = res_cur->nextRange; /* [한국어] 없으면 다음 창으로 */
			kfree(res_tmp); /* [한국어] 해제한다 */
			res_tmp = NULL; /* [한국어] 포인터를 지운다 */
		}
		bus->firstPFMem = NULL; /* [한국어] 목록을 비운다 */
	}

	if (bus->firstPFMemFromMem) { /* [한국어] MEM 에서 떼어 쓴 프리페치 자원이 있으면 */
		res_cur = bus->firstPFMemFromMem; /* [한국어] 목록 머리부터 */
		while (res_cur) { /* [한국어] 끝까지 걷는다 */
			res_tmp = res_cur; /* [한국어] 해제할 노드를 붙잡고 */
			res_cur = res_cur->next; /* [한국어] **이 목록만 next 로만 걷는다** — nextRange 를 쓰지 않기 때문이다 */

			kfree(res_tmp); /* [한국어] 해제한다 */
			res_tmp = NULL; /* [한국어] 포인터를 지운다 */
		}
		bus->firstPFMemFromMem = NULL; /* [한국어] 목록을 비운다 */
	}

	list_del(&bus->bus_list); /* [한국어] 전역 목록에서 이 버스를 떼어 내고 */
	kfree(bus); /* [한국어] 버스 노드 자체를 해제한다 */
	return 0; /* [한국어] 버스 제거 완료 */
}

/******************************************************************************
 * This routine deletes the ranges from a given bus, and the entries from the
 * parent's bus in the resources
 * Input: current bus, previous bus
 * Output: 0, -EINVAL
 ******************************************************************************/
/* [한국어]
 * remove_ranges - 버스의 창을 지우면서 부모 쪽 대응 자원도 함께 지운다
 *
 * @bus_cur:  창을 지울 버스(2차 버스).
 * @bus_prev: 그 부모(1차 버스).
 * @return: 0 성공, -EINVAL 은 부모에서 대응 자원을 못 찾은 경우.
 *
 * 바로 위 상류 주석대로 두 가지를 함께 한다 — 이 버스의 창을 지우고, 그
 * 창에 대응하는 **부모 버스의 자원 항목**도 지운다.
 *
 * 같은 주소 구간이 두 장부에 서로 다른 뜻으로 올라가 있기 때문이다.
 * 2차 버스에서는 "내가 쓸 수 있는 창(range)" 이고, 1차 버스에서는 "브리지가
 * 차지한 자원(resource)" 이다. update_bridge_ranges() 가 처음에 그 둘을 함께
 * 만들었으므로, 지울 때도 함께 지워야 짝이 맞는다.
 *
 * 세 종류가 똑같은 모양으로 반복된다 — 창의 시작 주소로 부모에서 자원을
 * 찾아 지우고, 창 노드 자체를 해제한다.
 *
 * 부모에서 자원을 못 찾으면 -EINVAL 로 곧바로 돌아가는데, 이때 이미 지운
 * 창들은 되돌리지 않는다. 상류 코드 그대로다.
 *
 * noXxxRanges 는 줄이지 않고 rangeXxx 만 NULL 로 둔다 — 호출자가 곧 버스를
 * 해제하므로 개수가 남아 있어도 문제가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  ibmphp_remove_bus() → [이 함수]
 *               → ibmphp_find_resource() → ibmphp_remove_resource()
 */
static int remove_ranges(struct bus_node *bus_cur, struct bus_node *bus_prev)
{
	struct range_node *range_cur; /* [한국어] 지우며 걷는 현재 창 */
	struct range_node *range_tmp; /* [한국어] 해제할 창을 잠시 붙잡아 두는 자리 */
	int i; /* [한국어] 개수만큼 도는 루프 인덱스 */
	struct resource_node *res = NULL; /* [한국어] 부모 버스에서 찾은 대응 자원 */

	if (bus_cur->noIORanges) { /* [한국어] I/O 창이 있으면 */
		range_cur = bus_cur->rangeIO; /* [한국어] 목록 머리부터 */
		for (i = 0; i < bus_cur->noIORanges; i++) { /* [한국어] 개수만큼 돈다 */
			if (ibmphp_find_resource(bus_prev, range_cur->start, &res, IO) < 0) /* [한국어] 창의 시작 주소로 **부모 버스의 자원**을 찾는다 — 그 창은 부모에서 보면 브리지가 차지한 자원이다 */
				return -EINVAL; /* [한국어] 못 찾으면 장부가 어긋난 것이다. [관찰] 이미 지운 창들은 되돌리지 않는다 */
			ibmphp_remove_resource(res); /* [한국어] 찾았으면 부모 장부에서 지운다 */

			range_tmp = range_cur; /* [한국어] 해제할 창을 붙잡고 */
			range_cur = range_cur->next; /* [한국어] 다음으로 간 뒤 */
			kfree(range_tmp); /* [한국어] 해제한다 */
			range_tmp = NULL; /* [한국어] 포인터를 지운다 */
		}
		bus_cur->rangeIO = NULL; /* [한국어] 목록을 비운다. 개수(noIORanges)는 줄이지 않는데 곧 버스를 해제하므로 문제가 없다 */
	}
	if (bus_cur->noMemRanges) { /* [한국어] 메모리 창이 있으면. 아래가 I/O 와 같은 모양이다 */
		range_cur = bus_cur->rangeMem; /* [한국어] 목록 머리부터 */
		for (i = 0; i < bus_cur->noMemRanges; i++) { /* [한국어] 개수만큼 돈다 */
			if (ibmphp_find_resource(bus_prev, range_cur->start, &res, MEM) < 0) /* [한국어] 부모에서 대응 자원을 찾고 */
				return -EINVAL; /* [한국어] 못 찾으면 그대로 돌아간다 */

			ibmphp_remove_resource(res); /* [한국어] 부모 장부에서 지운다 */
			range_tmp = range_cur; /* [한국어] 해제할 창을 붙잡고 */
			range_cur = range_cur->next; /* [한국어] 다음으로 간 뒤 */
			kfree(range_tmp); /* [한국어] 해제한다 */
			range_tmp = NULL; /* [한국어] 포인터를 지운다 */
		}
		bus_cur->rangeMem = NULL; /* [한국어] 목록을 비운다 */
	}
	if (bus_cur->noPFMemRanges) { /* [한국어] 프리페치 메모리 창이 있으면. 세 번째 반복이다 */
		range_cur = bus_cur->rangePFMem; /* [한국어] 목록 머리부터 */
		for (i = 0; i < bus_cur->noPFMemRanges; i++) { /* [한국어] 개수만큼 돈다 */
			if (ibmphp_find_resource(bus_prev, range_cur->start, &res, PFMEM) < 0) /* [한국어] 부모에서 대응 자원을 찾고 */
				return -EINVAL; /* [한국어] 못 찾으면 그대로 돌아간다 */

			ibmphp_remove_resource(res); /* [한국어] 부모 장부에서 지운다 */
			range_tmp = range_cur; /* [한국어] 해제할 창을 붙잡고 */
			range_cur = range_cur->next; /* [한국어] 다음으로 간 뒤 */
			kfree(range_tmp); /* [한국어] 해제한다 */
			range_tmp = NULL; /* [한국어] 포인터를 지운다 */
		}
		bus_cur->rangePFMem = NULL; /* [한국어] 목록을 비운다 */
	}
	return 0; /* [한국어] 창 제거 완료 */
}

/*
 * find the resource node in the bus
 * Input: Resource needed, start address of the resource, type of resource
 */
/* [한국어]
 * ibmphp_find_resource - 시작 주소로 자원 노드를 되찾는다
 *
 * @bus:           찾을 버스.
 * @start_address: 그 자원의 시작 주소.
 * @res:           찾은 노드를 담을 곳.
 * @flag:          IO / MEM / PFMEM.
 * @return: 0 성공, -ENODEV 는 bus 가 NULL, -EINVAL 은 못 찾았거나 flag 가 잘못된 경우.
 *
 * 바로 위 상류 주석대로 버스에서 자원 노드를 찾는다. 주소만으로 찾는 이유는
 * 호출자가 대개 노드 포인터를 갖고 있지 않기 때문이다 — 예를 들어
 * remove_ranges() 는 창의 start 만 알고 그에 대응하는 부모 쪽 자원을 찾는다.
 *
 * 이 파일의 관용구대로 next → nextRange 로 걷는다.
 *
 * 못 찾았을 때 PFMEM 만 한 번 더 본다. firstPFMemFromMem 목록에 있을 수 있기
 * 때문이며, ibmphp_remove_resource() 의 같은 처리와 짝을 이룬다. 그 목록은
 * next 로만 걷는다.
 *
 * [관찰] 못 찾으면 *res 를 건드리지 않고 -EINVAL 만 돌려준다. 호출자가
 * 반환값을 확인하지 않고 *res 를 읽으면 이전 값이 남아 있는데, 실제
 * 호출자들은 모두 반환값을 확인한다. 또 마지막의 `if (*res)` 는 호출자가
 * 넘긴 포인터가 가리키는 값을 읽는 것이라, 초기화되지 않은 변수를 넘기면
 * 읽는 시점의 값이 정의되지 않는다. 상류 코드 그대로다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  remove_ranges() / update_bridge_ranges() / ibmphp_pci.c
 *               → [이 함수]
 */
int ibmphp_find_resource(struct bus_node *bus, u32 start_address, struct resource_node **res, int flag)
{
	struct resource_node *res_cur = NULL; /* [한국어] 목록을 걸으며 찾는 자원 */
	char *type = ""; /* [한국어] 디버그 메시지에 쓸 종류 이름 */

	if (!bus) { /* [한국어] 버스가 없으면 */
		err("The bus passed in NULL to find resource\n"); /* [한국어] 알리고 */
		return -ENODEV; /* [한국어] 그대로 돌아간다 */
	}

	switch (flag) { /* [한국어] 종류에 따라 목록 머리와 이름을 꺼낸다 */
		case IO:
			res_cur = bus->firstIO; /* [한국어] I/O 자원 목록 */
			type = "io"; /* [한국어] 메시지용 이름 */
			break;
		case MEM:
			res_cur = bus->firstMem; /* [한국어] 메모리 자원 목록 */
			type = "mem"; /* [한국어] 메시지용 이름 */
			break;
		case PFMEM:
			res_cur = bus->firstPFMem; /* [한국어] 프리페치 메모리 자원 목록 */
			type = "pfmem"; /* [한국어] 메시지용 이름 */
			break;
		default: /* [한국어] 셋 중 어느 것도 아니면 */
			err("wrong type of flag\n"); /* [한국어] 알리고 */
			return -EINVAL; /* [한국어] 그대로 돌아간다 */
	}

	while (res_cur) { /* [한국어] 목록을 걷는다 */
		if (res_cur->start == start_address) { /* [한국어] 시작 주소가 같은 노드를 찾으면 */
			*res = res_cur; /* [한국어] 호출자에게 돌려주고 */
			break; /* [한국어] 멈춘다 */
		}
		if (res_cur->next) /* [한국어] 이 파일의 관용구 — 같은 창 안의 다음이 있으면 그리로 */
			res_cur = res_cur->next; /* [한국어] 그리로 */
		else
			res_cur = res_cur->nextRange; /* [한국어] 없으면 다음 창의 첫 자원으로 건너뛴다 */
	}

	if (!res_cur) { /* [한국어] 못 찾았으면 */
		if (flag == PFMEM) { /* [한국어] 프리페치 메모리만 한 번 더 본다 */
			res_cur = bus->firstPFMemFromMem; /* [한국어] MEM 에서 떼어 쓴 목록을 뒤진다 */
			while (res_cur) { /* [한국어] 그 목록을 걷는다 */
				if (res_cur->start == start_address) { /* [한국어] 시작 주소가 같으면 */
					*res = res_cur; /* [한국어] 호출자에게 돌려주고 */
					break; /* [한국어] 멈춘다 */
				}
				res_cur = res_cur->next; /* [한국어] 이 목록은 next 로만 걷는다 */
			}
			if (!res_cur) { /* [한국어] 거기에도 없으면 */
				debug("SOS...cannot find %s resource in the bus.\n", type); /* [한국어] 알리고 */
				return -EINVAL; /* [한국어] 그대로 돌아간다 */
			}
		} else {
			debug("SOS... cannot find %s resource in the bus.\n", type); /* [한국어] 알리고 */
			return -EINVAL; /* [한국어] 그대로 돌아간다 */
		}
	}

	if (*res) /* [한국어] [관찰] 호출자가 초기화하지 않은 포인터를 넘기면 이 읽기의 값이 정의되지 않는다. 상류 코드 그대로다 */
		debug("*res->start = %x\n", (*res)->start); /* [한국어] 무엇을 찾았는지 남긴다 */

	return 0; /* [한국어] 찾기 성공 */
}

/***********************************************************************
 * This routine will free the resource structures used by the
 * system.  It is called from cleanup routine for the module
 * Parameters: none
 * Returns: none
 ***********************************************************************/
/* [한국어]
 * ibmphp_free_resources - 장부 전체를 해제한다(모듈 언로드)
 *
 * 바로 위 상류 주석대로 모듈 정리 경로에서 불려 이 파일이 잡은 메모리를
 * 모두 놓는다.
 *
 * 맨 앞에서 전역 flags 를 1 로 세우는 것이 눈에 띈다. 그 값은
 * ibmphp_print_test() 가 "이미 해제된 뒤인데 gbuses 가 비어 있지 않다" 는
 * 이상 상황을 알아채는 데만 쓰인다. 이 파일에서 flags 를 쓰는 곳은 그 둘뿐이다.
 *
 * 버스마다 창 셋과 자원 넷을 모두 해제하고 버스 자신도 지운다.
 * list_for_each_entry_safe 를 쓰는 것은 순회 중에 노드를 지우기 때문이다.
 *
 * 창 해제 루프가 개수(noXxxRanges)만큼 도는데, 그 안에 `if (!range_cur) break;`
 * 가 있어 개수와 실제 목록 길이가 어긋나도 견딘다. ibmphp_remove_bus() 의
 * 같은 코드에는 그 방어가 없다.
 *
 * 자원 해제는 이 파일의 관용구대로 next → nextRange 로 걷고,
 * firstPFMemFromMem 만 next 로만 걷는다.
 *
 * [관찰] PFMemFromMem 의 노드들은 짝이 되는 MEM 노드와 같은 구간을 가리키지만
 * 서로 다른 노드라, 여기서 각각 한 번씩 해제되어 이중 해제는 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 언로드).
 *
 * 호출 체인:  ibmphp_core.c 의 정리 → [이 함수]
 */
void ibmphp_free_resources(void)
{
	struct bus_node *bus_cur = NULL, *next; /* [한국어] 순회 항목과, 지우는 동안 다음을 붙잡아 두는 자리 */
	struct bus_node *bus_tmp; /* [한국어] 해제할 버스를 잠시 붙잡아 두는 자리 */
	struct range_node *range_cur; /* [한국어] 지우며 걷는 현재 창 */
	struct range_node *range_tmp; /* [한국어] 해제할 창을 붙잡아 두는 자리 */
	struct resource_node *res_cur; /* [한국어] 지우며 걷는 현재 자원 */
	struct resource_node *res_tmp; /* [한국어] 해제할 자원을 붙잡아 두는 자리 */
	int i = 0; /* [한국어] 창 해제 루프의 인덱스 */
	flags = 1; /* [한국어] 해제를 시작했다고 표시한다 — ibmphp_print_test() 만 이 값을 읽는다 */

	list_for_each_entry_safe(bus_cur, next, &gbuses, bus_list) { /* [한국어] 순회 중에 노드를 지우므로 _safe 판을 쓴다 */
		if (bus_cur->noIORanges) { /* [한국어] I/O 창이 있으면 */
			range_cur = bus_cur->rangeIO; /* [한국어] 목록 머리부터 */
			for (i = 0; i < bus_cur->noIORanges; i++) { /* [한국어] 개수만큼 돈다 */
				if (!range_cur) /* [한국어] 개수보다 목록이 짧으면 */
					break; /* [한국어] 개수와 실제 목록 길이가 어긋나도 견디는 방어. ibmphp_remove_bus() 의 같은 코드에는 없다 */
				range_tmp = range_cur; /* [한국어] 해제할 창을 붙잡고 */
				range_cur = range_cur->next; /* [한국어] 다음으로 간 뒤 */
				kfree(range_tmp); /* [한국어] 해제한다 */
				range_tmp = NULL; /* [한국어] 포인터를 지운다 */
			}
		}
		if (bus_cur->noMemRanges) { /* [한국어] 메모리 창이 있으면 */
			range_cur = bus_cur->rangeMem; /* [한국어] 목록 머리부터 */
			for (i = 0; i < bus_cur->noMemRanges; i++) { /* [한국어] 개수만큼 돈다 */
				if (!range_cur) /* [한국어] 개수보다 목록이 짧으면 */
					break; /* [한국어] 멈춘다 */
				range_tmp = range_cur; /* [한국어] 해제할 창을 붙잡고 */
				range_cur = range_cur->next; /* [한국어] 다음으로 간 뒤 */
				kfree(range_tmp); /* [한국어] 해제한다 */
				range_tmp = NULL; /* [한국어] 포인터를 지운다 */
			}
		}
		if (bus_cur->noPFMemRanges) { /* [한국어] 프리페치 메모리 창이 있으면 */
			range_cur = bus_cur->rangePFMem; /* [한국어] 목록 머리부터 */
			for (i = 0; i < bus_cur->noPFMemRanges; i++) { /* [한국어] 개수만큼 돈다 */
				if (!range_cur) /* [한국어] 개수보다 목록이 짧으면 */
					break; /* [한국어] 멈춘다 */
				range_tmp = range_cur; /* [한국어] 해제할 창을 붙잡고 */
				range_cur = range_cur->next; /* [한국어] 다음으로 간 뒤 */
				kfree(range_tmp); /* [한국어] 해제한다 */
				range_tmp = NULL; /* [한국어] 포인터를 지운다 */
			}
		}

		if (bus_cur->firstIO) { /* [한국어] I/O 자원이 있으면 */
			res_cur = bus_cur->firstIO; /* [한국어] 목록 머리부터 */
			while (res_cur) { /* [한국어] 끝까지 걷는다 */
				res_tmp = res_cur; /* [한국어] 해제할 노드를 붙잡고 */
				if (res_cur->next) /* [한국어] 이 파일의 관용구 — 같은 창 안의 다음이 있으면 그리로 */
					res_cur = res_cur->next; /* [한국어] 그리로 */
				else
					res_cur = res_cur->nextRange; /* [한국어] 없으면 다음 창의 첫 자원으로 */
				kfree(res_tmp); /* [한국어] 해제한다 */
				res_tmp = NULL; /* [한국어] 포인터를 지운다 */
			}
			bus_cur->firstIO = NULL; /* [한국어] 목록을 비운다 */
		}
		if (bus_cur->firstMem) { /* [한국어] 메모리 자원이 있으면 */
			res_cur = bus_cur->firstMem; /* [한국어] 목록 머리부터 */
			while (res_cur) { /* [한국어] 끝까지 걷는다 */
				res_tmp = res_cur; /* [한국어] 해제할 노드를 붙잡고 */
				if (res_cur->next) /* [한국어] 이 파일의 관용구 — 같은 창 안의 다음이 있으면 그리로 */
					res_cur = res_cur->next; /* [한국어] 그리로 */
				else
					res_cur = res_cur->nextRange; /* [한국어] 없으면 다음 창으로 */
				kfree(res_tmp); /* [한국어] 해제한다 */
				res_tmp = NULL; /* [한국어] 포인터를 지운다 */
			}
			bus_cur->firstMem = NULL; /* [한국어] 목록을 비운다 */
		}
		if (bus_cur->firstPFMem) { /* [한국어] 프리페치 메모리 자원이 있으면 */
			res_cur = bus_cur->firstPFMem; /* [한국어] 목록 머리부터 */
			while (res_cur) { /* [한국어] 끝까지 걷는다 */
				res_tmp = res_cur; /* [한국어] 해제할 노드를 붙잡고 */
				if (res_cur->next) /* [한국어] 이 파일의 관용구 — 같은 창 안의 다음이 있으면 그리로 */
					res_cur = res_cur->next; /* [한국어] 그리로 */
				else
					res_cur = res_cur->nextRange; /* [한국어] 없으면 다음 창으로 */
				kfree(res_tmp); /* [한국어] 해제한다 */
				res_tmp = NULL; /* [한국어] 포인터를 지운다 */
			}
			bus_cur->firstPFMem = NULL; /* [한국어] 목록을 비운다 */
		}

		if (bus_cur->firstPFMemFromMem) { /* [한국어] MEM 에서 떼어 쓴 프리페치 자원이 있으면 */
			res_cur = bus_cur->firstPFMemFromMem; /* [한국어] 목록 머리부터 */
			while (res_cur) { /* [한국어] 끝까지 걷는다 */
				res_tmp = res_cur; /* [한국어] 해제할 노드를 붙잡고 */
				res_cur = res_cur->next; /* [한국어] 이 목록만 next 로만 걷는다. 짝이 되는 MEM 노드와는 별개의 노드라 이중 해제가 아니다 */

				kfree(res_tmp); /* [한국어] 해제한다 */
				res_tmp = NULL; /* [한국어] 포인터를 지운다 */
			}
			bus_cur->firstPFMemFromMem = NULL; /* [한국어] 목록을 비운다 */
		}

		bus_tmp = bus_cur; /* [한국어] 해제할 버스를 붙잡고 */
		list_del(&bus_cur->bus_list); /* [한국어] 전역 목록에서 떼어 낸 뒤 */
		kfree(bus_tmp); /* [한국어] 해제한다 */
		bus_tmp = NULL; /* [한국어] 포인터를 지운다 */
	}
}

/*********************************************************************************
 * This function will go over the PFmem resources to check if the EBDA allocated
 * pfmem out of memory buckets of the bus.  If so, it will change the range numbers
 * and a flag to indicate that this resource is out of memory. It will also move the
 * Pfmem out of the pfmem resource list to the PFMemFromMem list, and will create
 * a new Mem node
 * This routine is called right after initialization
 *******************************************************************************/
/* [한국어]
 * once_over - 초기화 끝에 MEM 에서 떼어 쓴 PFMEM 을 정리한다
 *
 * @return: 0 성공, -ENOMEM 은 할당 실패.
 *
 * 바로 위 상류 주석대로 초기화 직후에 한 번 불려, EBDA 가 **PFMEM 창이 아니라
 * MEM 창에서 프리페치 자원을 떼어 준** 경우를 바로잡는다.
 *
 * 판정 조건이 간단하다 — 그 버스에 PFMEM 창이 하나도 없는데(rangePFMem 이
 * NULL) PFMEM 자원은 있으면(firstPFMem 이 있으면), 그 자원들은 MEM 창에서
 * 온 것일 수밖에 없다.
 *
 * 그런 자원마다 넷을 한다.
 *   1) fromMem 을 1 로 세워 출처를 표시한다.
 *   2) firstPFMem 목록에서 떼어 낸다.
 *   3) firstPFMemFromMem 목록의 **맨 앞에** 끼운다. 상류 주석대로 정렬하지
 *      않는데, 실제 계산은 아래에서 만드는 MEM 노드로 하기 때문이다.
 *   4) 같은 구간을 가리키는 **MEM 노드를 새로 만들어** 장부에 올린다.
 *      그래야 ibmphp_check_resource() 같은 기존 코드가 이 구간을 "이미 쓰이는
 *      MEM" 으로 보게 되어, 코드를 고치지 않고도 겹침이 막힌다. 상류 주석이
 *      "ibmphp_check_mem_resource 등을 바꾸지 않으려고" 라고 밝히는 것이
 *      이 뜻이다.
 *   5) PFMEM 노드의 rangeno 를 그 MEM 노드의 것으로 맞춘다.
 *
 * 이 함수의 이름이 상황을 잘 보여 준다 — 초기화가 끝난 뒤 장부를 "한 번 훑어"
 * 정리하는 후처리다. ibmphp_rsrc_init() 의 마지막 문장이 곧 이 호출이다.
 *
 * [관찰] 안쪽 for 루프가 pfmem_cur->next 로 전진하는데, 루프 몸통에서 그
 * next 를 firstPFMemFromMem 로 바꿔 버린다. 전진 표현식이 몸통 뒤에 평가되므로
 * 그 시점에는 이미 바뀐 값이라, 두 번째 반복부터는 PFMemFromMem 목록을
 * 따라가게 된다. 상류 코드 그대로이며 여기서는 고치지 않는다.
 *
 * MEM 노드 등록이 실패하면 오류만 찍고 계속 간다 — 상류 메시지가 "PCI 장치가
 * 아닐 수도 있다" 고 여지를 남긴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 부팅 시 한 번(__init).
 *
 * 호출 체인:  ibmphp_rsrc_init() → [이 함수] → ibmphp_add_resource()
 */
static int __init once_over(void)
{
	struct resource_node *pfmem_cur; /* [한국어] 옮길 프리페치 자원 */
	struct resource_node *pfmem_prev; /* [한국어] 그 앞 노드 */
	struct resource_node *mem; /* [한국어] 새로 만들 짝 MEM 노드 */
	struct bus_node *bus_cur; /* [한국어] 버스 순회 항목 */

	list_for_each_entry(bus_cur, &gbuses, bus_list) { /* [한국어] 모든 버스를 훑는다 */
		if ((!bus_cur->rangePFMem) && (bus_cur->firstPFMem)) { /* [한국어] **판정 조건** — PFMEM 창이 하나도 없는데 PFMEM 자원은 있으면, 그것은 MEM 창에서 온 것일 수밖에 없다 */
			for (pfmem_cur = bus_cur->firstPFMem, pfmem_prev = NULL; pfmem_cur; pfmem_prev = pfmem_cur, pfmem_cur = pfmem_cur->next) { /* [한국어] [관찰] 전진식이 pfmem_cur->next 인데 몸통에서 그 값을 바꿔 버려, 두 번째 반복부터는 PFMemFromMem 목록을 따라간다. 상류 코드 그대로다 */
				pfmem_cur->fromMem = 1; /* [한국어] MEM 에서 떼어 왔다고 표시한다 */
				if (pfmem_prev) /* [한국어] 앞 노드가 있으면 */
					pfmem_prev->next = pfmem_cur->next; /* [한국어] PFMEM 목록에서 떼어 낸다 */
				else
					bus_cur->firstPFMem = pfmem_cur->next; /* [한국어] 머리를 다음으로 옮긴다 */

				if (!bus_cur->firstPFMemFromMem) /* [한국어] 곁가지 목록이 비어 있으면 */
					pfmem_cur->next = NULL; /* [한국어] 이 노드가 유일하다 */
				else
					/* we don't need to sort PFMemFromMem since we're using mem node for
					   all the real work anyways, so just insert at the beginning of the
					   list
					 */
					pfmem_cur->next = bus_cur->firstPFMemFromMem; /* [한국어] 상류 주석대로 정렬하지 않고 맨 앞에 끼운다 — 실제 계산은 아래 MEM 노드가 맡기 때문이다 */

				bus_cur->firstPFMemFromMem = pfmem_cur; /* [한국어] 곁가지 목록의 새 머리로 삼는다 */

				mem = kzalloc_obj(struct resource_node); /* [한국어] **짝이 되는 MEM 노드를 새로 만든다** */
				if (!mem) /* [한국어] 메모리가 없으면 */
					return -ENOMEM; /* [한국어] 그대로 돌아간다 */

				mem->type = MEM; /* [한국어] 종류는 메모리 */
				mem->busno = pfmem_cur->busno; /* [한국어] 같은 버스 */
				mem->devfunc = pfmem_cur->devfunc; /* [한국어] 같은 장치·함수 */
				mem->start = pfmem_cur->start; /* [한국어] 같은 시작 */
				mem->end = pfmem_cur->end; /* [한국어] 같은 끝 */
				mem->len = pfmem_cur->len; /* [한국어] 같은 길이 — 구간이 완전히 겹친다 */
				if (ibmphp_add_resource(mem) < 0) /* [한국어] MEM 장부에 올린다. 그래야 기존 코드가 이 구간을 "이미 쓰이는 MEM" 으로 보아 겹침이 막힌다 */
					err("Trouble...trouble... EBDA allocated pfmem from mem, but system doesn't display it has this space... unless not PCI device...\n"); /* [한국어] 실패해도 오류만 찍고 계속 간다 — 상류 메시지가 "PCI 장치가 아닐 수도 있다" 고 여지를 남긴다 */
				pfmem_cur->rangeno = mem->rangeno; /* [한국어] PFMEM 노드의 창 번호를 MEM 쪽 것으로 맞춘다 */
			}	/* end for pfmem */
		}	/* end if */
	}	/* end list_for_each bus */
	return 0; /* [한국어] 후처리 완료 */
}

/* [한국어]
 * ibmphp_add_pfmem_from_mem - MEM 에서 떼어 쓴 PFMEM 을 곁가지 목록에 매단다
 *
 * @pfmem: 매달 자원.
 * @return: 0 성공, -ENODEV 는 버스를 못 찾은 경우.
 *
 * once_over() 가 초기화 때 하는 일을, 카드 삽입 시점에 하는 함수다.
 * ibmphp_pci.c 가 PFMEM 창에서 자리를 못 찾아 MEM 창에서 떼어 썼을 때 부른다.
 *
 * 목록 맨 앞에 끼우기만 하고 정렬하지 않는다 — once_over() 의 상류 주석과
 * 같은 이유로, 실제 계산은 짝이 되는 MEM 노드가 맡기 때문이다.
 *
 * 이 함수는 MEM 노드를 만들지 않는다. 그 일은 호출자가 따로 하며,
 * once_over() 가 두 가지를 함께 하는 것과 다르다.
 *
 * else 가지에서 next 를 NULL 로 두는 것은 목록이 비어 있을 때다 — 인자로 받은
 * 노드의 next 에 쓰레기가 남아 있을 수 있어 명시적으로 끊는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(카드 삽입).
 *
 * 호출 체인:  ibmphp_pci.c 의 BAR 설정 경로 → [이 함수] → find_bus_wprev()
 */
int ibmphp_add_pfmem_from_mem(struct resource_node *pfmem)
{
	struct bus_node *bus_cur = find_bus_wprev(pfmem->busno, NULL, 0); /* [한국어] 이 자원이 속한 버스를 찾는다 */

	if (!bus_cur) { /* [한국어] 없으면 */
		err("cannot find bus of pfmem to add...\n"); /* [한국어] 알리고 */
		return -ENODEV; /* [한국어] 그대로 돌아간다 */
	}

	if (bus_cur->firstPFMemFromMem) /* [한국어] 곁가지 목록이 이미 있으면 */
		pfmem->next = bus_cur->firstPFMemFromMem; /* [한국어] 옛 머리를 뒤에 잇고 */
	else
		pfmem->next = NULL; /* [한국어] 뒤에 아무것도 없다 — 인자 노드에 남아 있을 수 있는 값을 명시적으로 끊는다 */

	bus_cur->firstPFMemFromMem = pfmem; /* [한국어] 맨 앞에 끼운다. once_over() 와 달리 짝 MEM 노드는 호출자가 따로 만든다 */

	return 0; /* [한국어] 등록 완료 */
}

/* This routine just goes through the buses to see if the bus already exists.
 * It is called from ibmphp_find_sec_number, to find out a secondary bus number for
 * bridged cards
 * Parameters: bus_number
 * Returns: Bus pointer or NULL
 */
/* [한국어]
 * ibmphp_find_res_bus - 버스 번호로 버스 노드를 찾는다(외부 공개 판)
 *
 * @bus_number: 찾을 버스 번호.
 * @return: 그 버스 노드. 없으면 NULL.
 *
 * 이 번호의 버스가 이미 장부에 있는지 보는 용도다.
 *
 * [관찰] 바로 위 상류 주석은 호출자를 "ibmphp_find_sec_number" 라고 적어
 * 두었지만, 그 이름의 함수는 이 트리에 없다. 실제 호출자는 ibmphp_pci.c 의
 * 다섯 곳(:892, :1207, :1344, :1612, :1681)이며, 그중 :892 와 :1344 가
 * 브리지 카드에 줄 2차 버스 번호를 정한 뒤 그 버스 노드를 되찾는 자리다.
 * 함수 이름이 바뀐 뒤 주석이 따라가지 않은 것으로 보이나, 그 개명 이력은
 * 이 트리에서 확인 못 함.
 *
 * find_bus_wprev() 를 prev 없이(flag 0) 부르는 얇은 래퍼다. 내부 함수가
 * static 이라 바깥에서 부를 수 없어 이 이름으로 한 겹 감쌌다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  ibmphp_pci.c → [이 함수] → find_bus_wprev()
 */
struct bus_node *ibmphp_find_res_bus(u8 bus_number)
{
	return find_bus_wprev(bus_number, NULL, 0);
}

/* [한국어]
 * find_bus_wprev - gbuses 를 훑어 버스를 찾고, 원하면 앞 노드도 준다
 *
 * @bus_number: 찾을 버스 번호.
 * @prev:       flag 가 1 일 때 앞 노드를 담을 곳.
 * @flag:       1 이면 prev 를 채운다. 0 이면 prev 를 건드리지 않는다.
 * @return: 찾은 버스 노드. 없으면 NULL.
 *
 * 이 파일에서 버스를 찾는 유일한 함수다. gbuses 는 static 이라 바깥에서
 * 직접 걸을 수 없고, 안에서도 모두 이 함수를 거친다.
 *
 * 이름의 wprev 는 "with prev" 다. 상류 코드에서 앞 노드가 필요한 곳은
 * ibmphp_rsrc_init() 뿐이며, 그마저도 받아 놓고 쓰지는 않는다.
 *
 * [관찰] flag 가 1 이면 **찾기 전에 매 반복마다** *prev 를 갱신하므로,
 * 끝내 못 찾았을 때도 마지막 노드의 앞 노드가 남는다. 또 list_prev_entry 는
 * 순환 리스트의 앞 노드를 주므로, 첫 노드에서는 리스트 머리를 감싼
 * 가짜 항목을 가리킨다. 지금은 그 값을 쓰는 호출자가 없어 문제가 되지
 * 않는다. 상류 코드 그대로다.
 *
 * 버스 목록은 정렬되어 있지 않아(파일 앞머리 상류 주석의 "The buses are not
 * ordered") 늘 선형 탐색이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  이 파일의 거의 모든 함수 → [이 함수]
 */
static struct bus_node *find_bus_wprev(u8 bus_number, struct bus_node **prev, u8 flag)
{
	struct bus_node *bus_cur; /* [한국어] 순회 항목 */

	list_for_each_entry(bus_cur, &gbuses, bus_list) { /* [한국어] 전역 버스 목록을 처음부터 훑는다 — 정렬되어 있지 않아 선형 탐색이다 */
		if (flag) /* [한국어] 앞 노드를 달라고 했으면 */
			*prev = list_prev_entry(bus_cur, bus_list); /* [한국어] [관찰] 찾기 전에 매 반복마다 갱신하므로 못 찾았을 때도 값이 남는다. 첫 노드에서는 리스트 머리를 감싼 가짜 항목을 가리킨다 */
		if (bus_cur->busno == bus_number) /* [한국어] 번호가 맞으면 */
			return bus_cur; /* [한국어] 그 버스를 돌려준다 */
	}

	return NULL; /* [한국어] 끝까지 못 찾았다 */
}

/* [한국어]
 * ibmphp_print_test - 장부 전체를 디버그 로그로 찍는다
 *
 * 이름 그대로 진단용이다. 버스마다 창 셋과 자원 넷을 모두 나열한다.
 *
 * 맨 앞의 조건이 특이하다 — gbuses 가 비어 있지 않은데 flags 가 1 이면
 * 오류를 찍고 돌아간다. flags 는 ibmphp_free_resources() 가 세우므로,
 * 그 조건은 "이미 다 해제했는데 버스가 남아 있다" 는 이상 상황을 잡는다.
 * 바꿔 말해 해제 뒤에 이 함수를 부르면 장부가 깨끗한지 확인해 준다.
 * [관찰] 다만 조건이 `(!list_empty) && flags` 라, 해제 전(flags 가 0)에는
 * 늘 통과해 정상 출력이 나온다.
 *
 * 창은 개수(noXxxRanges)만큼 for 로 돌고, 자원은 이 파일의 관용구대로
 * next → nextRange 로 걷되 둘 다 없으면 break 한다. firstPFMemFromMem 만
 * next 로만 걷는다.
 *
 * debug_pci 는 ibmphp.h 가 정의하는 디버그 매크로로, 빌드 설정에 따라
 * 사라진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  ibmphp_core.c / ibmphp_pci.c 의 진단 경로 → [이 함수]
 */
void ibmphp_print_test(void)
{
	int i = 0; /* [한국어] 창 출력 루프의 인덱스 */
	struct bus_node *bus_cur = NULL; /* [한국어] 버스 순회 항목 */
	struct range_node *range; /* [한국어] 출력할 창 */
	struct resource_node *res; /* [한국어] 출력할 자원 */

	debug_pci("*****************START**********************\n"); /* [한국어] 출력 시작을 알린다 */

	if ((!list_empty(&gbuses)) && flags) { /* [한국어] 해제 뒤(flags 가 1)인데 버스가 남아 있으면 */
		err("The GBUSES is not NULL?!?!?!?!?\n"); /* [한국어] 장부가 제대로 비워지지 않은 것이다 */
		return; /* [한국어] 그대로 돌아간다. [관찰] 해제 전에는 flags 가 0 이라 늘 통과해 정상 출력이 나온다 */
	}

	list_for_each_entry(bus_cur, &gbuses, bus_list) { /* [한국어] 모든 버스를 훑는다 */
		debug_pci ("This is bus # %d.  There are\n", bus_cur->busno); /* [한국어] 버스 번호 */
		debug_pci ("IORanges = %d\t", bus_cur->noIORanges); /* [한국어] I/O 창 개수 */
		debug_pci ("MemRanges = %d\t", bus_cur->noMemRanges); /* [한국어] 메모리 창 개수 */
		debug_pci ("PFMemRanges = %d\n", bus_cur->noPFMemRanges); /* [한국어] 프리페치 메모리 창 개수 */
		debug_pci ("The IO Ranges are as follows:\n"); /* [한국어] 아래에 I/O 창을 나열한다 */
		if (bus_cur->rangeIO) { /* [한국어] I/O 창이 있으면 */
			range = bus_cur->rangeIO; /* [한국어] 목록 머리부터 */
			for (i = 0; i < bus_cur->noIORanges; i++) { /* [한국어] 개수만큼 돈다 */
				debug_pci("rangeno is %d\n", range->rangeno); /* [한국어] 그 창의 번호 */
				debug_pci("[%x - %x]\n", range->start, range->end); /* [한국어] 그 창의 시작과 끝 */
				range = range->next; /* [한국어] 다음 창으로 */
			}
		}

		debug_pci("The Mem Ranges are as follows:\n"); /* [한국어] 아래에 메모리 창을 나열한다 */
		if (bus_cur->rangeMem) { /* [한국어] 메모리 창이 있으면 */
			range = bus_cur->rangeMem; /* [한국어] 목록 머리부터 */
			for (i = 0; i < bus_cur->noMemRanges; i++) { /* [한국어] 개수만큼 돈다 */
				debug_pci("rangeno is %d\n", range->rangeno); /* [한국어] 그 창의 번호 */
				debug_pci("[%x - %x]\n", range->start, range->end); /* [한국어] 그 창의 시작과 끝 */
				range = range->next; /* [한국어] 다음 창으로 */
			}
		}

		debug_pci("The PFMem Ranges are as follows:\n"); /* [한국어] 아래에 프리페치 메모리 창을 나열한다 */

		if (bus_cur->rangePFMem) { /* [한국어] 프리페치 메모리 창이 있으면 */
			range = bus_cur->rangePFMem; /* [한국어] 목록 머리부터 */
			for (i = 0; i < bus_cur->noPFMemRanges; i++) { /* [한국어] 개수만큼 돈다 */
				debug_pci("rangeno is %d\n", range->rangeno); /* [한국어] 그 창의 번호 */
				debug_pci("[%x - %x]\n", range->start, range->end); /* [한국어] 그 창의 시작과 끝 */
				range = range->next; /* [한국어] 다음 창으로 */
			}
		}

		debug_pci("The resources on this bus are as follows\n"); /* [한국어] 여기부터는 쓰이고 있는 자원 목록이다 */

		debug_pci("IO...\n"); /* [한국어] I/O 자원 */
		if (bus_cur->firstIO) { /* [한국어] 있으면 */
			res = bus_cur->firstIO; /* [한국어] 목록 머리부터 */
			while (res) { /* [한국어] 끝까지 걷는다 */
				debug_pci("The range # is %d\n", res->rangeno); /* [한국어] 이 자원이 속한 창 번호 */
				debug_pci("The bus, devfnc is %d, %x\n", res->busno, res->devfunc); /* [한국어] 이 자원을 쓰는 버스와 장치·함수 */
				debug_pci("[%x - %x], len=%x\n", res->start, res->end, res->len); /* [한국어] 구간과 길이 */
				if (res->next) /* [한국어] 같은 창 안의 다음이 있으면 */
					res = res->next; /* [한국어] 그리로 */
				else if (res->nextRange) /* [한국어] 없고 다음 창의 첫 자원이 있으면 */
					res = res->nextRange; /* [한국어] 그리로 건너뛴다 */
				else
					break; /* [한국어] 목록 끝이다 */
			}
		}
		debug_pci("Mem...\n"); /* [한국어] 메모리 자원 */
		if (bus_cur->firstMem) { /* [한국어] 있으면 */
			res = bus_cur->firstMem; /* [한국어] 목록 머리부터 */
			while (res) { /* [한국어] 끝까지 걷는다 */
				debug_pci("The range # is %d\n", res->rangeno); /* [한국어] 이 자원이 속한 창 번호 */
				debug_pci("The bus, devfnc is %d, %x\n", res->busno, res->devfunc); /* [한국어] 이 자원을 쓰는 버스와 장치·함수 */
				debug_pci("[%x - %x], len=%x\n", res->start, res->end, res->len); /* [한국어] 구간과 길이 */
				if (res->next) /* [한국어] 같은 창 안의 다음이 있으면 */
					res = res->next; /* [한국어] 그리로 */
				else if (res->nextRange) /* [한국어] 없고 다음 창이 있으면 */
					res = res->nextRange; /* [한국어] 그리로 */
				else
					break; /* [한국어] 끝이다 */
			}
		}
		debug_pci("PFMem...\n"); /* [한국어] 프리페치 메모리 자원 */
		if (bus_cur->firstPFMem) { /* [한국어] 있으면 */
			res = bus_cur->firstPFMem; /* [한국어] 목록 머리부터 */
			while (res) { /* [한국어] 끝까지 걷는다 */
				debug_pci("The range # is %d\n", res->rangeno); /* [한국어] 이 자원이 속한 창 번호 */
				debug_pci("The bus, devfnc is %d, %x\n", res->busno, res->devfunc); /* [한국어] 이 자원을 쓰는 버스와 장치·함수 */
				debug_pci("[%x - %x], len=%x\n", res->start, res->end, res->len); /* [한국어] 구간과 길이 */
				if (res->next) /* [한국어] 같은 창 안의 다음이 있으면 */
					res = res->next; /* [한국어] 그리로 */
				else if (res->nextRange) /* [한국어] 없고 다음 창이 있으면 */
					res = res->nextRange; /* [한국어] 그리로 */
				else
					break; /* [한국어] 끝이다 */
			}
		}

		debug_pci("PFMemFromMem...\n"); /* [한국어] MEM 에서 떼어 쓴 프리페치 자원 */
		if (bus_cur->firstPFMemFromMem) { /* [한국어] 있으면 */
			res = bus_cur->firstPFMemFromMem; /* [한국어] 목록 머리부터 */
			while (res) { /* [한국어] 끝까지 걷는다 */
				debug_pci("The range # is %d\n", res->rangeno); /* [한국어] 이 자원이 속한 창 번호 */
				debug_pci("The bus, devfnc is %d, %x\n", res->busno, res->devfunc); /* [한국어] 이 자원을 쓰는 버스와 장치·함수 */
				debug_pci("[%x - %x], len=%x\n", res->start, res->end, res->len); /* [한국어] 구간과 길이 */
				res = res->next; /* [한국어] 이 목록만 next 로만 걷는다 */
			}
		}
	}
	debug_pci("***********************END***********************\n"); /* [한국어] 출력 끝을 알린다 */
}

/* [한국어]
 * range_exists_already - 같은 구간의 창이 이미 있는지 본다
 *
 * @range:   비교할 창.
 * @bus_cur: 대상 버스.
 * @type:    IO / MEM / PFMEM.
 * @return: 있으면 1, 없으면 0, type 이 잘못되면 -ENODEV.
 *
 * update_bridge_ranges() 가 브리지 창을 등록하기 전에 부른다. 그 함수는
 * 부팅 때마다 브리지 config 공간을 다시 읽으므로, 이전 로드에서 이미 만들어
 * 둔 창을 또 만들지 않도록 걸러야 한다.
 *
 * start 와 end 가 **둘 다** 같아야 같은 창으로 본다. 겹치기만 하는 경우는
 * 걸러 내지 못한다.
 *
 * 반환값이 셋(1/0/-ENODEV)인데 호출자는 참/거짓으로만 쓴다. type 이 잘못되면
 * -ENODEV 가 참으로 해석되어 "이미 있다" 로 취급되는데, 실제 호출자는 늘
 * 올바른 상수를 넘긴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(부팅 시 __init 경로).
 *
 * 호출 체인:  update_bridge_ranges() → [이 함수]
 */
static int range_exists_already(struct range_node *range, struct bus_node *bus_cur, u8 type)
{
	struct range_node *range_cur = NULL; /* [한국어] 비교하며 걷는 현재 창 */
	switch (type) { /* [한국어] 종류에 따라 창 목록 머리를 꺼낸다 */
		case IO:
			range_cur = bus_cur->rangeIO; /* [한국어] I/O 창 목록 */
			break;
		case MEM:
			range_cur = bus_cur->rangeMem;
			break;
		case PFMEM: /* [한국어] 프리페치 메모리 창 목록 */
			range_cur = bus_cur->rangePFMem;
			break;
		default: /* [한국어] 셋 중 어느 것도 아니면 */
			err("wrong type passed to find out if range already exists\n"); /* [한국어] 알리고 */
			return -ENODEV; /* [한국어] 그대로 돌아간다. [관찰] 이 값은 호출자에서 참으로 해석되어 "이미 있다" 로 취급된다 */
	}

	while (range_cur) { /* [한국어] 창 목록을 훑는다 */
		if ((range_cur->start == range->start) && (range_cur->end == range->end)) /* [한국어] 시작과 끝이 **둘 다** 같아야 같은 창으로 본다 — 겹치기만 하는 경우는 걸러 내지 못한다 */
			return 1; /* [한국어] 있다 */
		range_cur = range_cur->next; /* [한국어] 다음 창으로 */
	}

	return 0; /* [한국어] 끝까지 훑었으니 없다 */
}

/* This routine will read the windows for any PPB we have and update the
 * range info for the secondary bus, and will also input this info into
 * primary bus, since BIOS doesn't. This is for PPB that are in the system
 * on bootup.  For bridged cards that were added during previous load of the
 * driver, only the ranges and the bus structure are added, the devices are
 * added from NVRAM
 * Input: primary busno
 * Returns: none
 * Note: this function doesn't take into account IO restrictions etc,
 *	 so will only work for bridges with no video/ISA devices behind them It
 *	 also will not work for onboard PPBs that can have more than 1 *bus
 *	 behind them All these are TO DO.
 *	 Also need to add more error checkings... (from fnc returns etc)
 */
/* [한국어]
 * update_bridge_ranges - 브리지 config 공간을 읽어 2차 버스의 창을 만든다
 *
 * @bus: 훑을 1차 버스. 이중 포인터지만 값을 바꾸지는 않는다.
 * @return: 0 성공, -ENODEV 는 버스가 NULL, -ENOMEM 은 할당 실패.
 *
 * 바로 위 상류 주석이 배경을 밝힌다 — EBDA 는 1차 버스의 창만 알려 주고
 * PPB(PCI-PCI 브리지) 뒤 2차 버스의 창은 알려 주지 않는다. 그래서 이 함수가
 * **config 공간을 직접 읽어** 그 정보를 채운다.
 *
 * 이 파일에서 유일하게 PCI config 접근을 하는 함수이며, 그때 쓰는
 * ibmphp_pci_bus 는 실제로 열거된 버스가 아니라 번호만 갈아 끼우며 재사용하는
 * 껍데기다. 첫 줄에서 그 번호를 지금 훑을 버스로 바꾼다.
 *
 * 장치 32 x 함수 8 을 모두 훑는 완전 탐색이다. 헤더 타입으로 갈린다.
 *   NORMAL       — 브리지가 아니다. function 을 8 로 만들어 그 장치의 나머지
 *                  함수를 건너뛴다(단일 함수 장치이기 때문이다).
 *   MULTIDEVICE  — 다기능 일반 장치. 아무 일도 하지 않고 다음 함수로 간다.
 *   BRIDGE       — 단일 함수 브리지. function 을 8 로 만든 뒤 아래로 흘러
 *                  MULTIBRIDGE 와 같은 처리를 받는다(fallthrough).
 *   MULTIBRIDGE  — 다기능 브리지. 실제 처리 본체다.
 *
 * 브리지 처리는 창 종류 셋(IO / MEM / PFMEM)에 대해 같은 모양을 반복한다.
 *   1) 2차 버스 번호를 읽어 그 버스 노드를 찾는다. 없으면 상류 주석대로
 *      이전 로드에서 설정해 둔 브리지라는 뜻이라, 껍데기만 만들고 **함수
 *      전체를 0 으로 끝낸다** — 나머지는 NVRAM 경로가 채운다.
 *   2) 브리지의 base/limit 레지스터를 읽어 창의 경계를 만든다. 끝에
 *      0xfff(IO) 또는 0xfffff((PF)MEM)를 더하는 것은 그 레지스터가 **구간의
 *      시작 단위만** 담기 때문이다 — IO 는 4KB, MEM 은 1MB 단위이고 limit 은
 *      마지막 단위의 시작을 가리키므로, 그 단위의 끝까지 채워야 실제 끝이 된다.
 *   3) 그 창을 2차 버스에 등록한다. 이미 있으면(range_exists_already) 버리고,
 *      첫 창이면 rangeno 를 1 로, 아니면 add_bus_range() 로 정렬해 끼운다.
 *      그다음 fix_resources() 로 -1 이던 자원들의 번호를 맞춘다.
 *   4) **같은 구간을 1차 버스에는 "브리지가 차지한 자원" 으로 등록한다.**
 *      이것이 remove_ranges() 가 나중에 함께 지우는 그 짝이다. 이미 있으면
 *      (ibmphp_find_resource 가 0 을 돌려주면) 만들지 않는다.
 *
 * PFMEM 만 상위 32비트 레지스터를 함께 읽고 64비트 빌드에서만 합친다.
 * 32비트 빌드에서는 그 값이 버려지므로 4GB 위의 창을 표현하지 못한다.
 *
 * 상류 주석이 남긴 한계도 그대로다 — 브리지 뒤에 버스가 하나뿐이라고
 * 가정하며(TO DO 로 남아 있다), 비디오/ISA 장치의 I/O 제약을 다루지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 부팅 시(__init).
 *
 * 호출 체인:  ibmphp_rsrc_init() → [이 함수]
 *               → find_bus_wprev() / add_bus_range() / fix_resources()
 *               → ibmphp_find_resource() / ibmphp_add_resource()
 */
static int __init update_bridge_ranges(struct bus_node **bus)
{
	u8 sec_busno, device, function, hdr_type, start_io_address, end_io_address; /* [한국어] 2차 버스 번호, 장치·함수 번호, 헤더 타입, I/O base/limit 의 하위 바이트 */
	u16 vendor_id, upper_io_start, upper_io_end, start_mem_address, end_mem_address; /* [한국어] 벤더 ID, I/O base/limit 의 상위 16비트, 메모리 base/limit */
	u32 start_address, end_address, upper_start, upper_end; /* [한국어] 조립한 창의 시작·끝과, 프리페치 메모리의 상위 32비트 */
	struct bus_node *bus_sec; /* [한국어] 브리지 뒤 2차 버스 */
	struct bus_node *bus_cur; /* [한국어] 지금 훑고 있는 1차 버스 */
	struct resource_node *io; /* [한국어] 1차 버스 쪽에 등록할 I/O 자원 */
	struct resource_node *mem; /* [한국어] 같은 메모리 자원 */
	struct resource_node *pfmem; /* [한국어] 같은 프리페치 메모리 자원 */
	struct range_node *range; /* [한국어] 2차 버스에 등록할 창 */
	unsigned int devfn; /* [한국어] config 접근에 쓸 장치·함수 번호 */

	bus_cur = *bus; /* [한국어] 이중 포인터로 받았지만 값을 바꾸지는 않는다 */
	if (!bus_cur) /* [한국어] 버스가 없으면 */
		return -ENODEV; /* [한국어] 그대로 돌아간다 */
	ibmphp_pci_bus->number = bus_cur->busno; /* [한국어] **전역 껍데기 버스의 번호를 지금 훑을 버스로 갈아 끼운다** — 실제 열거된 struct pci_bus 가 아니라 config 접근용 틀이다 */

	debug("inside %s\n", __func__);
	debug("bus_cur->busno = %x\n", bus_cur->busno); /* [한국어] 어느 버스를 훑는지 남긴다 */

	for (device = 0; device < 32; device++) { /* [한국어] 장치 32개를 */
		for (function = 0x00; function < 0x08; function++) { /* [한국어] 각각 함수 8개까지 모두 훑는 완전 탐색이다 */
			devfn = PCI_DEVFN(device, function); /* [한국어] 장치·함수를 하나의 devfn 으로 합친다 */
			pci_bus_read_config_word(ibmphp_pci_bus, devfn, PCI_VENDOR_ID, &vendor_id); /* [한국어] 벤더 ID 를 읽어 그 자리에 장치가 있는지 본다 */

			if (vendor_id != PCI_VENDOR_ID_NOTVALID) { /* [한국어] 유효한 벤더 ID 이면 장치가 있다 */
				/* found correct device!!! */
				pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_HEADER_TYPE, &hdr_type); /* [한국어] 헤더 타입을 읽어 브리지인지 가른다 */

				switch (hdr_type) { /* [한국어] 헤더 타입에 따라 */
					case PCI_HEADER_TYPE_NORMAL: /* [한국어] 일반 단일 함수 장치이면 */
						function = 0x8; /* [한국어] 함수 루프를 끝내 나머지 7개 함수를 건너뛴다 */
						break;
					case PCI_HEADER_TYPE_MULTIDEVICE: /* [한국어] 다기능 일반 장치이면 */
						break; /* [한국어] 아무 일도 하지 않고 다음 함수로 간다 */
					case PCI_HEADER_TYPE_BRIDGE: /* [한국어] 단일 함수 브리지이면 */
						function = 0x8; /* [한국어] 함수 루프를 끝내고 */
						fallthrough; /* [한국어] 아래 다기능 브리지와 같은 처리를 받는다 */
					case PCI_HEADER_TYPE_MULTIBRIDGE: /* [한국어] 다기능 브리지 — 실제 처리 본체다 */
						/* We assume here that only 1 bus behind the bridge
						   TO DO: add functionality for several:
						   temp = secondary;
						   while (temp < subordinate) {
						   ...
						   temp++;
						   }
						 */
						pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_SECONDARY_BUS, &sec_busno); /* [한국어] 브리지 뒤 2차 버스 번호를 읽는다 */
						bus_sec = find_bus_wprev(sec_busno, NULL, 0); /* [한국어] 그 버스가 장부에 있는지 본다 */
						/* this bus structure doesn't exist yet, PPB was configured during previous loading of ibmphp */
						if (!bus_sec) { /* [한국어] 없으면 — 상류 주석대로 이전에 ibmphp 가 로드되었을 때 설정해 둔 브리지다 */
							alloc_error_bus(NULL, sec_busno, 1); /* [한국어] 껍데기 버스만 만들고 */
							/* the rest will be populated during NVRAM call */
							return 0; /* [한국어] 상류 주석대로 나머지는 NVRAM 경로가 채운다. **함수 전체를 여기서 끝낸다** */
						}
						pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_IO_BASE, &start_io_address); /* [한국어] I/O base 의 하위 바이트 */
						pci_bus_read_config_byte(ibmphp_pci_bus, devfn, PCI_IO_LIMIT, &end_io_address); /* [한국어] I/O limit 의 하위 바이트 */
						pci_bus_read_config_word(ibmphp_pci_bus, devfn, PCI_IO_BASE_UPPER16, &upper_io_start); /* [한국어] I/O base 의 상위 16비트 */
						pci_bus_read_config_word(ibmphp_pci_bus, devfn, PCI_IO_LIMIT_UPPER16, &upper_io_end); /* [한국어] I/O limit 의 상위 16비트 */
						start_address = (start_io_address & PCI_IO_RANGE_MASK) << 8; /* [한국어] base 레지스터는 상위 4비트만 주소라 <<8 로 되돌린다 — I/O 창은 4KB 단위다 */
						start_address |= (upper_io_start << 16); /* [한국어] 상위 16비트를 얹어 32비트 주소를 만든다 */
						end_address = (end_io_address & PCI_IO_RANGE_MASK) << 8; /* [한국어] limit 도 같은 방식으로 */
						end_address |= (upper_io_end << 16); /* [한국어] 상위 비트를 얹는다 */

						if ((start_address) && (start_address <= end_address)) { /* [한국어] 창이 실제로 열려 있고 앞뒤가 뒤집히지 않았으면 */
							range = kzalloc_obj(struct range_node); /* [한국어] 창 노드를 잡는다 */
							if (!range) /* [한국어] 메모리가 없으면 */
								return -ENOMEM; /* [한국어] 그대로 돌아간다 */

							range->start = start_address; /* [한국어] 창의 시작 */
							range->end = end_address + 0xfff; /* [한국어] **끝에 0xfff 를 더한다** — limit 레지스터가 마지막 4KB 단위의 시작을 가리키므로 그 단위의 끝까지 채워야 실제 끝이 된다 */

							if (bus_sec->noIORanges > 0) { /* [한국어] 이 2차 버스에 이미 I/O 창이 있으면 */
								if (!range_exists_already(range, bus_sec, IO)) { /* [한국어] 같은 구간이 아직 없을 때만 */
									add_bus_range(IO, range, bus_sec); /* [한국어] 주소순 자리에 끼우고 */
									++bus_sec->noIORanges; /* [한국어] 개수를 올린다 */
								} else {
									kfree(range); /* [한국어] 만든 창을 버린다 */
									range = NULL; /* [한국어] 포인터를 지운다 */
								}
							} else {
								/* 1st IO Range on the bus */
								range->rangeno = 1; /* [한국어] 번호는 1 */
								bus_sec->rangeIO = range; /* [한국어] 목록의 머리로 삼고 */
								++bus_sec->noIORanges; /* [한국어] 개수를 1 로 만든다 */
							}
							fix_resources(bus_sec); /* [한국어] 새 창이 생겼으니 2차 버스의 -1 자원들을 맞춘다 */

							if (ibmphp_find_resource(bus_cur, start_address, &io, IO)) { /* [한국어] **1차 버스 쪽**에 같은 구간이 아직 없으면 — 그 구간은 부모에서 보면 브리지가 차지한 자원이다 */
								io = kzalloc_obj(struct resource_node); /* [한국어] 자원 노드를 잡는다 */
								if (!io) { /* [한국어] 메모리가 없으면 */
									kfree(range); /* [한국어] 만든 창을 되돌리고 */
									return -ENOMEM; /* [한국어] 그대로 돌아간다 */
								}
								io->type = IO; /* [한국어] 종류는 I/O */
								io->busno = bus_cur->busno; /* [한국어] 1차 버스에 속한다 */
								io->devfunc = ((device << 3) | (function & 0x7)); /* [한국어] 이 브리지의 장치·함수 번호 */
								io->start = start_address; /* [한국어] 같은 시작 */
								io->end = end_address + 0xfff; /* [한국어] 같은 끝 */
								io->len = io->end - io->start + 1; /* [한국어] 끝이 포함 경계라 +1 이 길이다 */
								ibmphp_add_resource(io); /* [한국어] 1차 버스 장부에 올린다. remove_ranges() 가 나중에 이것을 창과 함께 지운다 */
							}
						}

						pci_bus_read_config_word(ibmphp_pci_bus, devfn, PCI_MEMORY_BASE, &start_mem_address); /* [한국어] 메모리 base 레지스터. I/O 와 달리 16비트 하나로 읽는다 */
						pci_bus_read_config_word(ibmphp_pci_bus, devfn, PCI_MEMORY_LIMIT, &end_mem_address); /* [한국어] 메모리 limit */

						start_address = 0x00000000 | (start_mem_address & PCI_MEMORY_RANGE_MASK) << 16; /* [한국어] 상위 12비트만 주소라 <<16 으로 되돌린다 — 메모리 창은 1MB 단위다 */
						end_address = 0x00000000 | (end_mem_address & PCI_MEMORY_RANGE_MASK) << 16; /* [한국어] limit 도 같은 방식으로 */

						if ((start_address) && (start_address <= end_address)) { /* [한국어] 창이 실제로 열려 있으면 */

							range = kzalloc_obj(struct range_node); /* [한국어] 창 노드를 잡는다 */
							if (!range) /* [한국어] 메모리가 없으면 */
								return -ENOMEM; /* [한국어] 그대로 돌아간다 */

							range->start = start_address; /* [한국어] 창의 시작 */
							range->end = end_address + 0xfffff; /* [한국어] **끝에 0xfffff 를 더한다** — 1MB 단위의 끝까지 채운다 */

							if (bus_sec->noMemRanges > 0) { /* [한국어] 이 2차 버스에 이미 메모리 창이 있으면 */
								if (!range_exists_already(range, bus_sec, MEM)) { /* [한국어] 같은 구간이 아직 없을 때만 */
									add_bus_range(MEM, range, bus_sec); /* [한국어] 주소순 자리에 끼우고 */
									++bus_sec->noMemRanges; /* [한국어] 개수를 올린다 */
								} else {
									kfree(range); /* [한국어] 만든 창을 버린다 */
									range = NULL; /* [한국어] 포인터를 지운다 */
								}
							} else {
								/* 1st Mem Range on the bus */
								range->rangeno = 1; /* [한국어] 번호는 1 */
								bus_sec->rangeMem = range; /* [한국어] 목록의 머리로 삼고 */
								++bus_sec->noMemRanges; /* [한국어] 개수를 1 로 만든다 */
							}

							fix_resources(bus_sec); /* [한국어] -1 자원들을 맞춘다 */

							if (ibmphp_find_resource(bus_cur, start_address, &mem, MEM)) { /* [한국어] 1차 버스 쪽에 같은 구간이 아직 없으면 */
								mem = kzalloc_obj(struct resource_node); /* [한국어] 자원 노드를 잡는다 */
								if (!mem) { /* [한국어] 메모리가 없으면 */
									kfree(range); /* [한국어] 만든 창을 되돌리고 */
									return -ENOMEM; /* [한국어] 그대로 돌아간다 */
								}
								mem->type = MEM; /* [한국어] 종류는 메모리 */
								mem->busno = bus_cur->busno; /* [한국어] 1차 버스에 속한다 */
								mem->devfunc = ((device << 3) | (function & 0x7)); /* [한국어] 이 브리지의 장치·함수 번호 */
								mem->start = start_address; /* [한국어] 같은 시작 */
								mem->end = end_address + 0xfffff; /* [한국어] 같은 끝 */
								mem->len = mem->end - mem->start + 1; /* [한국어] 길이 */
								ibmphp_add_resource(mem); /* [한국어] 1차 버스 장부에 올린다 */
							}
						}
						pci_bus_read_config_word(ibmphp_pci_bus, devfn, PCI_PREF_MEMORY_BASE, &start_mem_address); /* [한국어] 프리페치 메모리 base */
						pci_bus_read_config_word(ibmphp_pci_bus, devfn, PCI_PREF_MEMORY_LIMIT, &end_mem_address); /* [한국어] 프리페치 메모리 limit */
						pci_bus_read_config_dword(ibmphp_pci_bus, devfn, PCI_PREF_BASE_UPPER32, &upper_start); /* [한국어] 그 상위 32비트 base — 프리페치 창만 64비트를 지원한다 */
						pci_bus_read_config_dword(ibmphp_pci_bus, devfn, PCI_PREF_LIMIT_UPPER32, &upper_end); /* [한국어] 그 상위 32비트 limit */
						start_address = 0x00000000 | (start_mem_address & PCI_MEMORY_RANGE_MASK) << 16; /* [한국어] 하위 32비트를 1MB 단위로 되돌린다 */
						end_address = 0x00000000 | (end_mem_address & PCI_MEMORY_RANGE_MASK) << 16; /* [한국어] limit 도 같은 방식으로 */
#if BITS_PER_LONG == 64
						start_address |= ((long) upper_start) << 32; /* [한국어] 64비트 빌드에서만 상위 32비트를 얹는다 */
						end_address |= ((long) upper_end) << 32; /* [한국어] limit 도 마찬가지. 32비트 빌드에서는 그 값이 버려져 4GB 위의 창을 표현하지 못한다 */
#endif

						if ((start_address) && (start_address <= end_address)) { /* [한국어] 창이 실제로 열려 있으면 */

							range = kzalloc_obj(struct range_node); /* [한국어] 창 노드를 잡는다 */
							if (!range) /* [한국어] 메모리가 없으면 */
								return -ENOMEM; /* [한국어] 그대로 돌아간다 */

							range->start = start_address; /* [한국어] 창의 시작 */
							range->end = end_address + 0xfffff; /* [한국어] 1MB 단위의 끝까지 채운다 */

							if (bus_sec->noPFMemRanges > 0) { /* [한국어] 이 2차 버스에 이미 프리페치 메모리 창이 있으면 */
								if (!range_exists_already(range, bus_sec, PFMEM)) { /* [한국어] 같은 구간이 아직 없을 때만 */
									add_bus_range(PFMEM, range, bus_sec); /* [한국어] 주소순 자리에 끼우고 */
									++bus_sec->noPFMemRanges; /* [한국어] 개수를 올린다 */
								} else {
									kfree(range); /* [한국어] 만든 창을 버린다 */
									range = NULL; /* [한국어] 포인터를 지운다 */
								}
							} else {
								/* 1st PFMem Range on the bus */
								range->rangeno = 1; /* [한국어] 번호는 1 */
								bus_sec->rangePFMem = range; /* [한국어] 목록의 머리로 삼고 */
								++bus_sec->noPFMemRanges; /* [한국어] 개수를 1 로 만든다 */
							}

							fix_resources(bus_sec); /* [한국어] -1 자원들을 맞춘다 */
							if (ibmphp_find_resource(bus_cur, start_address, &pfmem, PFMEM)) { /* [한국어] 1차 버스 쪽에 같은 구간이 아직 없으면 */
								pfmem = kzalloc_obj(struct resource_node); /* [한국어] 자원 노드를 잡는다 */
								if (!pfmem) { /* [한국어] 메모리가 없으면 */
									kfree(range); /* [한국어] 만든 창을 되돌리고 */
									return -ENOMEM; /* [한국어] 그대로 돌아간다 */
								}
								pfmem->type = PFMEM; /* [한국어] 종류는 프리페치 메모리 */
								pfmem->busno = bus_cur->busno; /* [한국어] 1차 버스에 속한다 */
								pfmem->devfunc = ((device << 3) | (function & 0x7)); /* [한국어] 이 브리지의 장치·함수 번호 */
								pfmem->start = start_address; /* [한국어] 같은 시작 */
								pfmem->end = end_address + 0xfffff; /* [한국어] 같은 끝 */
								pfmem->len = pfmem->end - pfmem->start + 1; /* [한국어] 길이 */
								pfmem->fromMem = 0; /* [한국어] MEM 에서 떼어 쓴 것이 아니다 */

								ibmphp_add_resource(pfmem); /* [한국어] 1차 버스 장부에 올린다 */
							}
						}
						break; /* [한국어] 이 브리지 처리를 끝낸다 */
				}	/* end of switch */
			}	/* end if vendor */
		}	/* end for function */
	}	/* end for device */

	return 0; /* [한국어] 모든 장치·함수를 훑었다 */
}
