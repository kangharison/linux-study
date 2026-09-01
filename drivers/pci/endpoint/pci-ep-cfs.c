// SPDX-License-Identifier: GPL-2.0
/*
 * configfs to configure the PCI endpoint
 *
 * Copyright (C) 2017 Texas Instruments
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */

/*
 * [한국어 설명] 사용자가 엔드포인트를 조립하는 configfs 인터페이스 (pci-ep-cfs.c)
 *
 * === 파일의 역할 ===
 * 지금까지 본 pci-epc-core.c 와 pci-epf-core.c 는 커널 안쪽의 규약이었다.
 * 이 파일은 그것을 사용자 공간에 열어 준다. 디렉터리를 만들고 파일에
 * 값을 쓰는 것만으로 "이 SoC 를 무엇처럼 보이게 할지" 를 조립할 수 있게
 * 하는 것이 목적이다.
 *
 * 왜 sysfs 가 아니라 configfs 인가. 둘의 방향이 반대이기 때문이다.
 * sysfs 는 커널이 이미 가진 객체를 사용자에게 보여 주는 것이고,
 * configfs 는 사용자가 mkdir 로 커널 객체를 만들어 내는 것이다.
 * 엔드포인트 구성은 후자에 해당한다 — 어떤 기능을 몇 개 만들지가
 * 하드웨어가 아니라 사용자의 결정이다.
 *
 * --- 실제 사용 흐름 ---
 * /sys/kernel/config/pci_ep/ 아래에 두 디렉터리가 있다.
 *   functions/   — 만들 수 있는 EPF 종류들. 드라이버가 등록될 때 생긴다.
 *   controllers/ — 시스템에 있는 EPC 들. EPC 가 등록될 때 생긴다.
 *
 * 사용자는 이렇게 조립한다.
 *   1) mkdir functions/pci_epf_test/func1
 *      -> pci_epf_make() 가 불려 EPF 객체가 만들어진다.
 *   2) echo 0x104c > functions/pci_epf_test/func1/vendorid
 *      echo 0xb500 > .../deviceid   ... 등으로 config 헤더를 정한다.
 *   3) ln -s functions/pci_epf_test/func1 controllers/<EPC이름>/
 *      -> pci_epc_epf_link() 가 불려 EPF 를 EPC 에 붙인다.
 *   4) echo 1 > controllers/<EPC이름>/start
 *      -> pci_epc_start_store() 가 링크를 올린다. 이 순간 호스트에
 *         장치가 보이기 시작한다.
 *
 * 심볼릭 링크로 연결을 표현하는 것이 configfs 의 관용적 방식이다.
 * 링크를 만들면 allow_link 콜백이, 지우면 drop_link 콜백이 불린다.
 *
 * --- 그룹 계층 ---
 * EPF 하나가 만들어지면 그 아래에 다시 두 그룹이 생긴다.
 *   primary/   — 주 인터페이스에 붙일 EPC 를 링크할 자리
 *   secondary/ — 보조 인터페이스용. NTB 처럼 EPC 둘에 붙는 구성에 쓴다.
 * 그리고 EPF 드라이버가 자기만의 설정 항목을 갖고 싶으면
 * pci_epf_type_add_cfs() 로 하위 그룹을 더 붙일 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * EPF 드라이버 등록 시
 *   pci_epf_add_cfs() [pci-epf-core.c]
 *     -> pci_ep_cfs_add_epf_group() [이 파일] -> functions/<이름>/ 생성
 * EPC 등록 시
 *   __pci_epc_create() [pci-epc-core.c]
 *     -> pci_ep_cfs_add_epc_group() [이 파일] -> controllers/<이름>/ 생성
 *
 * 사용자 조작 -> [이 파일] -> pci_epf_create() / pci_epc_add_epf() /
 *   pci_epc_start() 등 두 코어 파일의 API
 *
 * 실행 컨텍스트: 전부 사용자 프로세스의 컨텍스트(mkdir, echo, ln).
 *   configfs 가 자체 잠금으로 디렉터리 조작을 직렬화한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: configfs 파일시스템, 그리고 그것을 조작하는 사용자 공간.
 * 아래쪽: pci-epc-core.c, pci-epf-core.c, 그리고 EPF 드라이버의
 *   add_cfs 콜백.
 * 공유 상태: functions_idr(EPF 이름의 번호 배정), 그리고 두 최상위 그룹.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(drivers/nvme 트리
 * 전수 확인 — 호출 0건).
 *
 * 이 파일이 NVMe 공부에 주는 것은 "장치의 정체가 어떻게 정해지는가" 를
 * 손으로 만져 볼 수 있게 한다는 점이다. vendorid, deviceid, baseclass_code
 * 같은 파일에 값을 쓰면 그것이 그대로 config space 에 실리고, 저쪽
 * 호스트는 그 값으로 드라이버를 고른다. baseclass_code 에 0x01,
 * subclass_code 에 0x08, progif_code 에 0x02 를 쓰면 그것이 곧
 * PCI_CLASS_STORAGE_EXPRESS(0x010802)이고, 호스트에서 nvme 드라이버가
 * 붙으려 시도하게 된다.
 *
 * 물론 그것만으로 NVMe 컨트롤러가 되지는 않는다 — 호스트가 곧 BAR 0 의
 * 레지스터를 NVMe 규격대로 읽으려 들 것이고, 그것을 흉내 내는 EPF
 * 드라이버가 있어야 한다. 이 트리의 endpoint/functions/ 에는 그런
 * 드라이버가 없다(test, mhi, ntb, vntb 넷뿐).
 *
 * === 주요 함수/구조체 요약 ===
 * pci_epf_make() / pci_epf_drop() : mkdir/rmdir 에 대응. EPF 를 만들고 없앤다.
 * pci_epc_epf_link() / _unlink()  : controllers/ 아래 심볼릭 링크로
 *                          EPF 를 EPC 의 주 인터페이스에 붙인다.
 * pci_primary_epc_epf_link() / pci_secondary_epc_epf_link() : 반대 방향 —
 *                          EPF 아래 primary/ secondary/ 에서 EPC 를 링크한다.
 * pci_epf_vepf_link() / _unlink() : EPF 아래에 다른 EPF 를 링크해
 *                          가상 함수(다중 함수 장치)를 구성한다.
 * pci_epc_start_store() / _show() : start 파일. 링크를 올리고 내린다.
 * PCI_EPF_HEADER_R / _W_u32 / _W_u16 / _W_u8 : config 헤더 필드의
 *                          show/store 를 찍어 내는 매크로.
 * pci_epf_msi_interrupts_store() / pci_epf_msix_interrupts_store() :
 *                          이 함수가 요청할 인터럽트 개수를 정한다.
 * pci_ep_cfs_add_epf_group() / pci_ep_cfs_add_epc_group() : 두 코어가
 *                          부르는 등록 함수.
 * pci_ep_cfs_init() / _exit() : configfs 서브시스템 등록.
 * struct pci_epf_group / struct pci_epc_group : configfs 그룹과 실제
 *                          커널 객체를 잇는 그릇.
 */

/* [한국어] EXPORT_SYMBOL_GPL 과 모듈 초기화 매크로. */
#include <linux/module.h>
/* [한국어] idr — EPF 이름 뒤에 붙일 번호를 배정한다. 같은 종류의 EPF 를
 * 여럿 만들 때 "pci_epf_test.0", "pci_epf_test.1" 로 구분하기 위한 것이다. */
#include <linux/idr.h>
/* [한국어] kzalloc / kfree — 그룹 구조체 할당. */
#include <linux/slab.h>

/* [한국어] struct pci_epc 와 pci_epc_add_epf / pci_epc_start 등.
 * 사용자 조작을 EPC 쪽 API 로 넘긴다. */
#include <linux/pci-epc.h>
/* [한국어] struct pci_epf 와 pci_epf_create / pci_epf_bind 등.
 * EPF 쪽 API. */
#include <linux/pci-epf.h>
/* [한국어] 이 파일이 구현하는 등록 함수들의 선언. 두 코어 파일이
 * 이 헤더를 통해 여기를 부른다. */
#include <linux/pci-ep-cfs.h>

/* [한국어] EPF 이름마다 번호를 배정하는 idr.
 * "pci_epf_test" 라는 종류 안에서 여러 인스턴스를 만들 수 있어야 하고,
 * 각각이 유일한 이름을 가져야 하므로 번호를 붙인다.
 * 그 번호가 pci_epf_create() 에 넘길 이름의 '.' 뒤가 된다. */
static DEFINE_IDR(functions_idr);
/* [한국어] 위 idr 을 보호하는 뮤텍스. configfs 가 디렉터리 조작을
 * 직렬화하기는 하지만 그 보장 범위가 이 idr 까지는 아니므로 직접 잡는다. */
static DEFINE_MUTEX(functions_mutex);
/* [한국어] /sys/kernel/config/pci_ep/functions/ 그룹.
 * EPF 드라이버가 등록될 때마다 이 아래에 종류별 디렉터리가 생긴다. */
static struct config_group *functions_group;
/* [한국어] /sys/kernel/config/pci_ep/controllers/ 그룹.
 * EPC 가 등록될 때마다 이 아래에 컨트롤러 디렉터리가 생긴다. */
static struct config_group *controllers_group;

/* [한국어]
 * struct pci_epf_group - configfs 디렉터리와 실제 EPF 객체를 잇는 그릇
 *
 * 사용자가 functions/<종류>/ 아래에 mkdir 하면 이 구조체가 하나 만들어진다.
 * configfs 쪽 표현(config_group)과 커널 쪽 실체(struct pci_epf)를
 * 함께 들고 있어서 서로를 오갈 수 있게 한다.
 */
struct pci_epf_group {
	struct config_group group;
	/* [한국어] 이 EPF 의 configfs 디렉터리 자체.
	 * 구조체 맨 앞에 통째로 박아 넣어 container_of 로 오갈 수 있다.
	 * 설정자: pci_epf_make() 가 config_group_init_type_name 으로 초기화.
	 * 읽는 자: to_pci_epf_group() 이 이 필드를 기준으로 되짚는다.
	 * 값 범위: configfs 가 관리하는 그룹.
	 * 동기화: configfs 가 디렉터리 조작을 직렬화한다. */

	struct config_group primary_epc_group;
	/* [한국어] 이 EPF 아래의 primary/ 하위 디렉터리.
	 * 여기에 EPC 를 심볼릭 링크로 걸면 주 인터페이스에 붙는다.
	 * 설정자: pci_ep_cfs_add_primary_group() 이 만든다.
	 * 읽는 자: pci_primary_epc_epf_link() 가 링크 조작 때 쓴다.
	 * 값 범위: 위와 같다.
	 * 동기화: 위와 같다. */

	struct config_group secondary_epc_group;
	/* [한국어] secondary/ 하위 디렉터리. NTB 처럼 EPC 둘에 붙는
	 * 구성에서 두 번째 인터페이스를 지정하는 자리다.
	 * 대부분의 구성에서는 비어 있는 채로 남는다.
	 * 설정자: pci_ep_cfs_add_secondary_group().
	 * 읽는 자: pci_secondary_epc_epf_link().
	 * 값 범위: 위와 같다.
	 * 동기화: 위와 같다. */

	struct pci_epf *epf;
	/* [한국어] 이 디렉터리에 대응하는 실제 EPF 객체.
	 * 사용자가 파일에 값을 쓰면 결국 이 객체의 필드가 바뀌거나
	 * 이 객체를 인자로 코어 API 가 불린다.
	 * 설정자: pci_epf_make() 가 pci_epf_create() 의 결과를 넣는다.
	 * 읽는 자: 이 파일의 거의 모든 store/show/link 함수.
	 * 값 범위: 유효한 EPF 포인터. NULL 이 되는 경우는 없다 —
	 *   생성에 실패하면 이 구조체 자체가 만들어지지 않는다.
	 * 동기화: configfs 의 디렉터리 잠금 아래에서 다뤄진다. */

	int index;
	/* [한국어] functions_idr 에서 받은 번호.
	 * 이름 뒤에 붙어 같은 종류의 EPF 들을 구분한다.
	 * 설정자: pci_epf_make() 가 idr_alloc 결과를 넣는다.
	 * 읽는 자: pci_epf_release() 가 반납할 때.
	 * 값 범위: 0 이상.
	 * 동기화: functions_mutex 로 보호되는 idr 에서 나온다. */
};

/* [한국어]
 * struct pci_epc_group - configfs 디렉터리와 실제 EPC 객체를 잇는 그릇
 *
 * EPC 가 등록될 때 controllers/ 아래에 만들어진다. EPF 와 달리 사용자가
 * mkdir 로 만드는 것이 아니라 하드웨어가 있으면 저절로 생긴다 —
 * 컨트롤러는 실재하는 하드웨어이므로 사용자가 만들어 낼 수 없다.
 */
struct pci_epc_group {
	struct config_group group;
	/* [한국어] 이 EPC 의 configfs 디렉터리.
	 * 설정자: pci_ep_cfs_add_epc_group().
	 * 읽는 자: to_pci_epc_group().
	 * 값 범위: configfs 가 관리하는 그룹.
	 * 동기화: configfs 의 디렉터리 잠금. */

	struct pci_epc *epc;
	/* [한국어] 대응하는 실제 EPC 객체.
	 * 설정자: pci_ep_cfs_add_epc_group() 이 pci_epc_get() 결과를 넣는다.
	 * 읽는 자: start 파일의 store/show 와 링크 조작 함수들.
	 * 값 범위: 유효한 EPC 포인터.
	 * 동기화: 위와 같다. */

	bool start;
	/* [한국어] 이 EPC 의 링크가 올라가 있는지.
	 * 하드웨어에서 읽는 값이 아니라 사용자가 start 파일에 쓴 값을
	 * 기억해 두는 것이다. show 가 이 값을 그대로 돌려준다.
	 * 설정자: pci_epc_start_store() 가 성공했을 때만 갱신한다.
	 * 읽는 자: pci_epc_start_show(), 그리고 링크 해제 시 정리 판단.
	 * 값 범위: true(동작 중) / false(정지).
	 * 동기화: configfs 가 같은 파일에 대한 쓰기를 직렬화한다. */
};

/* [한국어]
 * to_pci_epf_group - configfs 항목에서 바깥 EPF 그룹 구조체를 되짚는다
 *
 * @item: configfs 가 넘겨준 항목.
 * @return: 그것을 품고 있는 struct pci_epf_group.
 *
 * configfs 콜백들은 struct config_item 만 받으므로, 우리 구조체로
 * 되돌아가려면 두 단계를 거쳐야 한다. item → config_group → 바깥 구조체.
 * 커널에서 흔한 container_of 패턴이며, group 필드를 구조체 맨 앞에
 * 둔 것이 이것을 위해서다.
 *
 * 실행 컨텍스트: 어디서든. 순수 포인터 계산이라 부수효과가 없다.
 *
 * 호출 체인:
 *   이 파일의 거의 모든 configfs 콜백 → [이 함수]
 */
static inline struct pci_epf_group *to_pci_epf_group(struct config_item *item)
{
	/* [한국어] to_config_group 이 item → group 을, container_of 가
	 * group → 바깥 구조체를 담당한다. */
	return container_of(to_config_group(item), struct pci_epf_group, group);
}

/* [한국어]
 * to_pci_epc_group - configfs 항목에서 바깥 EPC 그룹 구조체를 되짚는다
 *
 * @item: configfs 가 넘겨준 항목.
 * @return: 그것을 품고 있는 struct pci_epc_group.
 *
 * 위 to_pci_epf_group() 과 같은 패턴이며 대상만 다르다.
 *
 * 실행 컨텍스트: 어디서든. 순수 포인터 계산.
 *
 * 호출 체인:
 *   start 파일의 show/store, 그리고 링크 조작 콜백들 → [이 함수]
 */
static inline struct pci_epc_group *to_pci_epc_group(struct config_item *item)
{
	return container_of(to_config_group(item), struct pci_epc_group, group);
}

/* [한국어]
 * pci_secondary_epc_epf_link - EPF 아래 secondary/ 에 EPC 를 링크하면 붙인다
 *
 * @epf_item: 링크가 만들어진 자리. 이 항목의 부모가 EPF 그룹이다.
 * @epc_item: 링크가 가리키는 EPC 항목.
 * @return: 0 이면 성공, 실패하면 그 오류가 사용자의 ln 명령에 전달된다.
 *
 * configfs 의 allow_link 콜백이다. 사용자가
 *   ln -s controllers/<EPC> functions/<종류>/<이름>/secondary/
 * 를 실행하면 불린다.
 *
 * 하는 일이 셋이다.
 *   1) pci_epc_add_epf() 로 함수 번호를 배정받아 EPC 의 목록에 넣는다.
 *   2) pci_epf_bind() 로 EPF 드라이버의 bind 를 부른다. 여기서
 *      드라이버가 BAR 메모리를 잡고 config 헤더를 쓴다.
 *   3) 밀린 초기화 통지가 있으면 전달한다.
 *
 * 3번이 중요한 이유는 pci-epc-core.c 에서 설명한 대로다 — EPC 초기화가
 * 이 링크보다 먼저 끝났다면 이 EPF 는 epc_init 통지를 놓쳤을 것이고,
 * 그러면 BAR 도 설정하지 못한 채 아무 일도 하지 않게 된다.
 *
 * 실행 컨텍스트: 사용자 프로세스(ln)의 컨텍스트. configfs 가 잠금을 쥔 채
 *   부르며, bind 안에서 메모리 할당과 하드웨어 접근이 일어나 잠들 수 있다.
 *
 * 에러 경로: bind 가 실패하면 1번에서 한 것을 되돌린다. 그렇지 않으면
 *   함수 번호가 잡힌 채로 남아 다음 시도가 실패하게 된다.
 *
 * 호출 체인:
 *   사용자의 ln → configfs → [이 함수]
 *     → pci_epc_add_epf() [pci-epc-core.c]
 *     → pci_epf_bind() [pci-epf-core.c]
 *     → pci_epc_notify_pending_init() [pci-epc-core.c]
 */
static int pci_secondary_epc_epf_link(struct config_item *epf_item,
				      struct config_item *epc_item)
{
	int ret;
	/* [한국어] ci_parent 를 쓰는 이유가 중요하다. 링크가 걸린 자리는
	 * EPF 디렉터리 자체가 아니라 그 아래 secondary/ 이므로,
	 * 한 단계 위로 올라가야 EPF 그룹에 닿는다. */
	struct pci_epf_group *epf_group = to_pci_epf_group(epf_item->ci_parent);
	/* [한국어] 링크가 가리키는 쪽은 EPC 디렉터리 자체라 그대로 변환한다. */
	struct pci_epc_group *epc_group = to_pci_epc_group(epc_item);
	struct pci_epc *epc = epc_group->epc;
	struct pci_epf *epf = epf_group->epf;

	/* [한국어] 보조 인터페이스로 붙인다. 이 호출이 함수 번호를 배정하고
	 * EPF 안의 sec_epc 필드를 채운다. */
	ret = pci_epc_add_epf(epc, epf, SECONDARY_INTERFACE);
	if (ret)
		return ret;

	/* [한국어] EPF 드라이버의 bind 를 부른다. 여기서 BAR 메모리를 잡고
	 * config 헤더를 쓴다. */
	ret = pci_epf_bind(epf);
	if (ret) {
		/* [한국어] 위에서 배정받은 함수 번호를 되돌린다. */
		pci_epc_remove_epf(epc, epf, SECONDARY_INTERFACE);
		return ret;
	}

	/* Send any pending EPC initialization complete to the EPF driver */
	/* [한국어] 상류 주석대로 밀린 통지를 전달한다. EPC 초기화가 이
	 * 링크보다 먼저 끝났다면 이 EPF 는 그 사건을 놓쳤으므로 여기서
	 * 따로 알려 줘야 BAR 설정이 이뤄진다. */
	pci_epc_notify_pending_init(epc, epf);

	return 0;
}

/* [한국어]
 * pci_secondary_epc_epf_unlink - 그 링크를 지우면 EPF 를 떼어 낸다
 *
 * @epf_item: 링크가 있던 자리.
 * @epc_item: 링크가 가리키던 EPC 항목.
 * @return: 없음. drop_link 는 실패를 알릴 수 없다.
 *
 * link 의 정확한 역순이다. unbind 를 먼저 하고 목록에서 뺀다 —
 * 반대로 하면 드라이버가 정리하는 도중에 이미 EPC 와의 연결이
 * 끊겨 있어 하드웨어를 되돌리지 못한다.
 *
 * 맨 앞의 WARN_ON_ONCE 가 이 함수에서 눈여겨볼 부분이다. 링크가 살아
 * 있는 상태에서 링크를 지우는 것은 정상적인 절차가 아니다 — 먼저
 * start 에 0 을 써서 링크를 내려야 한다. 그런데도 그 상태로 들어오면
 * 경고만 남기고 진행한다. 여기서 멈출 방법이 없기 때문이다(void 함수이며,
 * configfs 는 이미 링크를 지우기로 결정한 뒤 이 콜백을 부른다).
 *
 * 실행 컨텍스트: 사용자 프로세스(rm)의 컨텍스트.
 *
 * 호출 체인:
 *   사용자의 rm → configfs → [이 함수]
 *     → pci_epf_unbind() → pci_epc_remove_epf()
 */
static void pci_secondary_epc_epf_unlink(struct config_item *epf_item,
					 struct config_item *epc_item)
{
	/* [한국어] link 와 마찬가지로 부모를 거쳐 EPF 그룹에 닿는다. */
	struct pci_epf_group *epf_group = to_pci_epf_group(epf_item->ci_parent);
	struct pci_epc_group *epc_group = to_pci_epc_group(epc_item);
	struct pci_epc *epc;
	struct pci_epf *epf;

	/* [한국어] 링크가 아직 올라가 있는데 연결을 끊으려는 상황이다.
	 * 정상 절차는 start 에 0 을 먼저 쓰는 것이다. 여기서 멈출 방법이
	 * 없으므로 경고만 남기고 진행한다. ONCE 라 로그를 채우지 않는다. */
	WARN_ON_ONCE(epc_group->start);

	epc = epc_group->epc;
	epf = epf_group->epf;
	/* [한국어] 드라이버가 먼저 자기 자원을 정리하게 한다.
	 * 순서가 반대면 정리 도중에 EPC 연결이 이미 끊겨 하드웨어를
	 * 되돌리지 못한다. */
	pci_epf_unbind(epf);
	/* [한국어] 그다음 목록에서 빼고 함수 번호를 반납한다. */
	pci_epc_remove_epf(epc, epf, SECONDARY_INTERFACE);
}

/* [한국어] secondary/ 디렉터리의 항목 동작 표. 링크를 걸고 지우는 두 콜백만 담는다 —
 * 이 디렉터리에는 읽고 쓸 속성 파일이 없고 링크만 받기 때문이다. */
static const struct configfs_item_operations pci_secondary_epc_item_ops = {
	/* [한국어] ln 이 들어오면 이 함수가 불린다. */
	.allow_link	= pci_secondary_epc_epf_link,
	.drop_link	= pci_secondary_epc_epf_unlink,
};

/* [한국어] 그 동작 표를 담은 타입. config_group_init_type_name 에 넘겨진다. */
static const struct config_item_type pci_secondary_epc_type = {
	/* [한국어] 위에서 정의한 링크 콜백 표를 연결한다. */
	.ct_item_ops	= &pci_secondary_epc_item_ops,
	.ct_owner	= THIS_MODULE,
};

/* [한국어]
 * pci_ep_cfs_add_secondary_group - EPF 아래에 secondary/ 디렉터리를 만든다
 *
 * @epf_group: 이 하위 디렉터리를 가질 EPF 그룹.
 * @return: 만들어진 그룹. 실패할 수 없어 오류를 돌려주지 않는다.
 *
 * 사용자가 여기에 EPC 를 링크하면 보조 인터페이스에 붙는다. NTB 처럼
 * EPF 하나가 EPC 둘에 붙는 구성에서만 쓰이며, 대부분은 빈 채로 남는다.
 *
 * "default group" 으로 붙이는 점이 요점이다. 사용자가 mkdir 로 만드는
 * 것이 아니라 부모가 생길 때 자동으로 따라 생기고, 부모가 사라질 때
 * 함께 사라진다. 그래서 별도의 해제 함수가 없다.
 * 구조체를 따로 할당하지 않고 epf_group 안에 박아 둔 것도 같은 이유다 —
 * 수명이 부모와 같으니 따로 관리할 필요가 없다.
 *
 * 실행 컨텍스트: EPF 생성 중 사용자 프로세스의 컨텍스트.
 *
 * 호출 체인:
 *   pci_epf_make() → pci_epf_cfs_add_sub_groups() → [이 함수]
 */
static struct config_group
*pci_ep_cfs_add_secondary_group(struct pci_epf_group *epf_group)
{
	struct config_group *secondary_epc_group;

	/* [한국어] 부모 구조체에 박혀 있는 그룹을 쓴다. 따로 할당하지 않는다. */
	secondary_epc_group = &epf_group->secondary_epc_group;
	/* [한국어] 이름을 "secondary" 로 하고 링크 콜백을 담은 타입을 붙인다. */
	config_group_init_type_name(secondary_epc_group, "secondary",
				    &pci_secondary_epc_type);
	/* [한국어] 부모의 기본 그룹으로 등록한다. 부모와 수명이 묶인다. */
	configfs_add_default_group(secondary_epc_group, &epf_group->group);

	return secondary_epc_group;
}

/* [한국어]
 * pci_primary_epc_epf_link - EPF 아래 primary/ 에 EPC 를 링크하면 붙인다
 *
 * @epf_item: 링크가 만들어진 자리. 이 항목의 부모가 EPF 그룹이다.
 * @epc_item: 링크가 가리키는 EPC 항목.
 * @return: 0 이면 성공, 실패하면 그 오류가 사용자의 ln 명령에 전달된다.
 *
 * configfs 의 allow_link 콜백이다. 사용자가
 *   ln -s controllers/<EPC> functions/<종류>/<이름>/primary/
 * 를 실행하면 불린다.
 *
 * 하는 일이 셋이다.
 *   1) pci_epc_add_epf() 로 함수 번호를 배정받아 EPC 의 목록에 넣는다.
 *   2) pci_epf_bind() 로 EPF 드라이버의 bind 를 부른다. 여기서
 *      드라이버가 BAR 메모리를 잡고 config 헤더를 쓴다.
 *   3) 밀린 초기화 통지가 있으면 전달한다.
 *
 * 3번이 중요한 이유는 pci-epc-core.c 에서 설명한 대로다 — EPC 초기화가
 * 이 링크보다 먼저 끝났다면 이 EPF 는 epc_init 통지를 놓쳤을 것이고,
 * 그러면 BAR 도 설정하지 못한 채 아무 일도 하지 않게 된다.
 *
 * 실행 컨텍스트: 사용자 프로세스(ln)의 컨텍스트. configfs 가 잠금을 쥔 채
 *   부르며, bind 안에서 메모리 할당과 하드웨어 접근이 일어나 잠들 수 있다.
 *
 * 에러 경로: bind 가 실패하면 1번에서 한 것을 되돌린다. 그렇지 않으면
 *   함수 번호가 잡힌 채로 남아 다음 시도가 실패하게 된다.
 *
 * 호출 체인:
 *   사용자의 ln → configfs → [이 함수]
 *     → pci_epc_add_epf() [pci-epc-core.c]
 *     → pci_epf_bind() [pci-epf-core.c]
 *     → pci_epc_notify_pending_init() [pci-epc-core.c]
 */
static int pci_primary_epc_epf_link(struct config_item *epf_item,
				    struct config_item *epc_item)
{
	int ret;
	/* [한국어] 링크가 걸린 primary/ 의 부모가 EPF 그룹이다. */
	struct pci_epf_group *epf_group = to_pci_epf_group(epf_item->ci_parent);
	struct pci_epc_group *epc_group = to_pci_epc_group(epc_item);
	struct pci_epc *epc = epc_group->epc;
	struct pci_epf *epf = epf_group->epf;

	/* [한국어] 위 secondary 판과 코드가 같고 인터페이스 종류만 다르다.
	 * 주 인터페이스로 붙이므로 EPF 의 epc 필드가 채워진다. */
	ret = pci_epc_add_epf(epc, epf, PRIMARY_INTERFACE);
	if (ret)
		return ret;

	/* [한국어] EPF 드라이버의 bind 를 부른다. 여기서 BAR 메모리를 잡고 헤더를 쓴다. */
	ret = pci_epf_bind(epf);
	/* [한국어] bind 가 실패하면 */
	if (ret) {
		/* [한국어] 앞서 배정받은 함수 번호를 되돌려야 다음 시도가 막히지 않는다. */
		pci_epc_remove_epf(epc, epf, PRIMARY_INTERFACE);
		return ret;
	}

	/* Send any pending EPC initialization complete to the EPF driver */
	pci_epc_notify_pending_init(epc, epf);

	return 0;
}

/* [한국어]
 * pci_primary_epc_epf_unlink - 그 링크를 지우면 EPF 를 떼어 낸다
 *
 * @epf_item: 링크가 있던 자리.
 * @epc_item: 링크가 가리키던 EPC 항목.
 * @return: 없음. drop_link 는 실패를 알릴 수 없다.
 *
 * link 의 정확한 역순이다. unbind 를 먼저 하고 목록에서 뺀다 —
 * 반대로 하면 드라이버가 정리하는 도중에 이미 EPC 와의 연결이
 * 끊겨 있어 하드웨어를 되돌리지 못한다.
 *
 * 맨 앞의 WARN_ON_ONCE 가 이 함수에서 눈여겨볼 부분이다. 링크가 살아
 * 있는 상태에서 링크를 지우는 것은 정상적인 절차가 아니다 — 먼저
 * start 에 0 을 써서 링크를 내려야 한다. 그런데도 그 상태로 들어오면
 * 경고만 남기고 진행한다. 여기서 멈출 방법이 없기 때문이다(void 함수이며,
 * configfs 는 이미 링크를 지우기로 결정한 뒤 이 콜백을 부른다).
 *
 * 실행 컨텍스트: 사용자 프로세스(rm)의 컨텍스트.
 *
 * 호출 체인:
 *   사용자의 rm → configfs → [이 함수]
 *     → pci_epf_unbind() → pci_epc_remove_epf()
 */
static void pci_primary_epc_epf_unlink(struct config_item *epf_item,
				       struct config_item *epc_item)
{
	struct pci_epf_group *epf_group = to_pci_epf_group(epf_item->ci_parent);
	/* [한국어] 링크가 가리키는 EPC 디렉터리는 그대로 변환한다. */
	struct pci_epc_group *epc_group = to_pci_epc_group(epc_item);
	struct pci_epc *epc;
	struct pci_epf *epf;

	/* [한국어] 위 secondary 판과 같은 이유의 경고. */
	WARN_ON_ONCE(epc_group->start);

	epc = epc_group->epc;
	epf = epf_group->epf;
	/* [한국어] unbind 먼저, 그다음 목록 제거. */
	pci_epf_unbind(epf);
	pci_epc_remove_epf(epc, epf, PRIMARY_INTERFACE);
}

/* [한국어] primary/ 디렉터리의 항목 동작 표. secondary 판과 콜백만 다르다. */
static const struct configfs_item_operations pci_primary_epc_item_ops = {
	/* [한국어] 주 인터페이스로 붙이는 링크 콜백. */
	.allow_link	= pci_primary_epc_epf_link,
	.drop_link	= pci_primary_epc_epf_unlink,
};

/* [한국어] 그 동작 표를 담은 타입. */
static const struct config_item_type pci_primary_epc_type = {
	/* [한국어] 링크 콜백 표를 연결한다. */
	.ct_item_ops	= &pci_primary_epc_item_ops,
	.ct_owner	= THIS_MODULE,
};

/* [한국어]
 * pci_ep_cfs_add_primary_group - EPF 아래에 primary/ 디렉터리를 만든다
 *
 * @epf_group: 이 하위 디렉터리를 가질 EPF 그룹.
 * @return: 만들어진 그룹.
 *
 * 위 secondary 판과 같고 이름과 타입만 다르다. 여기에 EPC 를 링크하면
 * 주 인터페이스에 붙는다.
 *
 * 그런데 EPF 를 EPC 에 붙이는 방법이 둘이라는 점을 알아 두어야 한다.
 *   controllers/<EPC>/ 아래에 EPF 를 링크하거나(pci_epc_epf_link),
 *   functions/<종류>/<이름>/primary/ 아래에 EPC 를 링크하거나.
 * 결과는 같다 — 둘 다 PRIMARY_INTERFACE 로 pci_epc_add_epf 를 부른다.
 * 방향만 반대이며, 사용자가 편한 쪽을 쓰면 된다.
 *
 * 실행 컨텍스트: EPF 생성 중 사용자 프로세스의 컨텍스트.
 *
 * 호출 체인:
 *   pci_epf_make() → pci_epf_cfs_add_sub_groups() → [이 함수]
 */
static struct config_group
*pci_ep_cfs_add_primary_group(struct pci_epf_group *epf_group)
{
	/* [한국어] 부모 구조체에 박힌 그룹. 따로 할당하지 않는다. */
	struct config_group *primary_epc_group = &epf_group->primary_epc_group;

	config_group_init_type_name(primary_epc_group, "primary",
				    &pci_primary_epc_type);
	/* [한국어] 부모의 기본 그룹으로 등록해 수명을 묶는다. 사용자가 지울 수 없다. */
	configfs_add_default_group(primary_epc_group, &epf_group->group);

	return primary_epc_group;
}

/* [한국어]
 * pci_epc_start_store - start 파일에 쓰면 링크를 올리거나 내린다
 *
 * @item: EPC 의 configfs 항목.
 * @page: 사용자가 쓴 문자열. "1", "0", "y", "n" 등을 받는다.
 * @len: 그 길이. 성공 시 그대로 돌려주는 것이 sysfs/configfs 관례다.
 * @return: 성공하면 @len, 실패하면 음수 오류.
 *
 * 엔드포인트 구성의 마지막 단추다. 여기에 1 을 쓰는 순간 링크 트레이닝이
 * 시작되고, 저쪽 호스트에 이 장치가 보이기 시작한다.
 *
 * 그래서 이 시점까지 모든 준비가 끝나 있어야 한다 — config 헤더도 쓰고,
 * BAR 도 만들고, 인터럽트 개수도 정해 두어야 한다. 호스트는 링크가
 * 올라오자마자 config 를 읽으므로, 그 뒤에 바꾸면 늦는다.
 *
 * 실행 컨텍스트: 사용자 프로세스(echo)의 컨텍스트. pci_epc_start 안에서
 *   하드웨어를 만지고 잠들 수 있다.
 *
 * 에러 경로: 시작에 실패하면 epc_group->start 를 갱신하지 않는다.
 *   그래야 사용자가 다시 시도할 수 있다 — 갱신해 버리면 아래 -EALREADY
 *   검사에 걸려 영영 못 켜게 된다.
 *
 * 호출 체인:
 *   사용자의 echo → configfs → [이 함수]
 *     → pci_epc_start() 또는 pci_epc_stop() [pci-epc-core.c]
 */
static ssize_t pci_epc_start_store(struct config_item *item, const char *page,
				   size_t len)
{
	int ret;
	/* [한국어] 사용자가 쓴 값을 담을 불리언. */
	bool start;
	struct pci_epc *epc;
	struct pci_epc_group *epc_group = to_pci_epc_group(item);

	epc = epc_group->epc;

	/* [한국어] kstrtobool 은 "1"/"0" 뿐 아니라 "y"/"n", "on"/"off" 도
	 * 받아 준다. 사용자가 쓴 것이 그중 어느 것도 아니면 오류다. */
	if (kstrtobool(page, &start) < 0)
		return -EINVAL;

	/* [한국어] 이미 그 상태라면 -EALREADY. 두 번 켜면 링크 트레이닝이
	 * 중복으로 걸리고, 두 번 끄면 이미 없는 것을 또 내리게 된다.
	 * 오류로 알려 사용자가 상태를 잘못 알고 있음을 드러낸다. */
	if (start == epc_group->start)
		return -EALREADY;

	/* [한국어] 끄는 경우. stop 은 void 라 실패를 알리지 않으므로
	 * 곧바로 상태를 갱신하고 돌아간다. */
	if (!start) {
		pci_epc_stop(epc);
		epc_group->start = 0;
		return len;
	}

	/* [한국어] 켜는 경우. 여기서 링크 트레이닝이 시작된다. */
	ret = pci_epc_start(epc);
	if (ret) {
		dev_err(&epc->dev, "failed to start endpoint controller\n");
		/* [한국어] 실제 오류를 그대로 전하지 않고 -EINVAL 로 바꾼다.
		 * 사용자 입장에서는 구성이 잘못됐다는 뜻이기 때문으로 읽힌다.
		 * start 를 갱신하지 않았으므로 고친 뒤 다시 시도할 수 있다. */
		return -EINVAL;
	}

	/* [한국어] 성공했을 때만 상태를 기억한다. */
	epc_group->start = start;

	/* [한국어] 쓴 만큼을 다 소비했다고 알린다. 그렇지 않으면 사용자
	 * 공간이 나머지를 다시 쓰려 들어 무한 루프가 된다. */
	return len;
}

/* [한국어]
 * pci_epc_start_show - 현재 링크가 올라가 있는지 보여 준다
 *
 * @item: EPC 의 configfs 항목.
 * @page: 출력 버퍼.
 * @return: 쓴 바이트 수.
 *
 * 하드웨어에서 읽지 않고 기억해 둔 값을 그대로 돌려준다. 즉 "사용자가
 * 켜라고 했고 그것이 성공했는가" 이지, "지금 물리적으로 링크가 올라와
 * 있는가" 가 아니다. 호스트가 리셋되어 링크가 내려가도 이 값은 1 로
 * 남는다.
 *
 * 실행 컨텍스트: 사용자 프로세스(cat)의 컨텍스트.
 *
 * 호출 체인:
 *   사용자의 cat → configfs → [이 함수]
 */
static ssize_t pci_epc_start_show(struct config_item *item, char *page)
{
	/* [한국어] sysfs_emit 은 버퍼 넘침을 막아 주는 표준 출력 함수다. */
	return sysfs_emit(page, "%d\n", to_pci_epc_group(item)->start);
}

/* [한국어] start 속성을 만든다. 위에서 정의한 show/store 를 pci_epc_attr_start
 * 라는 이름의 구조체로 묶어 준다. */
CONFIGFS_ATTR(pci_epc_, start);

/* [한국어] 이 EPC 디렉터리에 노출할 속성 목록. 현재는 start 하나뿐이다. */
static struct configfs_attribute *pci_epc_attrs[] = {
	&pci_epc_attr_start,
	NULL,
};

/* [한국어]
 * pci_epc_epf_link - controllers/ 아래에 EPF 를 링크하면 붙인다
 *
 * @epc_item: 링크가 만들어진 EPC 디렉터리.
 * @epf_item: 링크가 가리키는 EPF 항목.
 * @return: 0 이면 성공.
 *
 * 위 pci_primary_epc_epf_link() 와 결과가 같고 방향만 반대다.
 * 사용자가
 *   ln -s functions/<종류>/<이름> controllers/<EPC>/
 * 를 실행하면 불린다. 이쪽이 문서와 예제에서 더 흔히 쓰이는 방식이다.
 *
 * ci_parent 를 쓰지 않는 점이 primary 판과의 차이다. 여기서는 링크가
 * EPC 디렉터리 바로 아래에 걸리므로 두 항목 모두 그대로 변환하면 된다.
 *
 * 실행 컨텍스트: 사용자 프로세스(ln)의 컨텍스트.
 *
 * 에러 경로: bind 실패 시 add_epf 를 되돌린다.
 *
 * 호출 체인:
 *   사용자의 ln → configfs → [이 함수]
 *     → pci_epc_add_epf() → pci_epf_bind() → pci_epc_notify_pending_init()
 */
static int pci_epc_epf_link(struct config_item *epc_item,
			    struct config_item *epf_item)
{
	int ret;
	/* [한국어] 링크 대상이 EPF 디렉터리 자체라 부모를 거치지 않는다. */
	struct pci_epf_group *epf_group = to_pci_epf_group(epf_item);
	struct pci_epc_group *epc_group = to_pci_epc_group(epc_item);
	struct pci_epc *epc = epc_group->epc;
	struct pci_epf *epf = epf_group->epf;

	/* [한국어] 이 경로는 항상 주 인터페이스다. 보조로 붙이려면
	 * EPF 아래 secondary/ 를 써야 한다. */
	ret = pci_epc_add_epf(epc, epf, PRIMARY_INTERFACE);
	if (ret)
		return ret;

	/* [한국어] EPF 드라이버의 bind 를 부른다. */
	ret = pci_epf_bind(epf);
	/* [한국어] 실패하면 */
	if (ret) {
		/* [한국어] 배정받은 함수 번호를 되돌린다. */
		pci_epc_remove_epf(epc, epf, PRIMARY_INTERFACE);
		return ret;
	}

	/* Send any pending EPC initialization complete to the EPF driver */
	pci_epc_notify_pending_init(epc, epf);

	return 0;
}

/* [한국어]
 * pci_epc_epf_unlink - 그 링크를 지우면 EPF 를 떼어 낸다
 *
 * @epc_item: 링크가 있던 EPC 디렉터리.
 * @epf_item: 링크가 가리키던 EPF 항목.
 * @return: 없음.
 *
 * link 의 역순이다. 위 primary/secondary 판과 마찬가지로 링크가 아직
 * 올라가 있으면 경고를 남긴다 — 먼저 start 에 0 을 쓰는 것이 정상 절차다.
 *
 * 실행 컨텍스트: 사용자 프로세스(rm)의 컨텍스트.
 *
 * 호출 체인:
 *   사용자의 rm → configfs → [이 함수]
 *     → pci_epf_unbind() → pci_epc_remove_epf()
 */
static void pci_epc_epf_unlink(struct config_item *epc_item,
			       struct config_item *epf_item)
{
	struct pci_epc *epc;
	/* [한국어] 떼어 낼 EPF. 아래에서 그룹에서 꺼낸다. */
	struct pci_epf *epf;
	struct pci_epf_group *epf_group = to_pci_epf_group(epf_item);
	struct pci_epc_group *epc_group = to_pci_epc_group(epc_item);

	/* [한국어] 링크가 살아 있는 채로 연결을 끊는 상황. 막을 수는 없고
	 * 경고만 남긴다. */
	WARN_ON_ONCE(epc_group->start);

	epc = epc_group->epc;
	epf = epf_group->epf;
	/* [한국어] 드라이버 정리 먼저, 목록 제거는 그다음. */
	pci_epf_unbind(epf);
	pci_epc_remove_epf(epc, epf, PRIMARY_INTERFACE);
}

/* [한국어] controllers/ 아래 EPC 디렉터리의 항목 동작 표. 여기에 EPF 를 링크한다. */
static const struct configfs_item_operations pci_epc_item_ops = {
	/* [한국어] EPF 를 이 컨트롤러에 붙이는 콜백. */
	.allow_link	= pci_epc_epf_link,
	.drop_link	= pci_epc_epf_unlink,
};

/* [한국어] EPC 디렉터리의 타입. 링크 콜백과 start 속성을 함께 담는다. */
static const struct config_item_type pci_epc_type = {
	/* [한국어] 링크 콜백 표. */
	.ct_item_ops	= &pci_epc_item_ops,
	.ct_attrs	= pci_epc_attrs,
	.ct_owner	= THIS_MODULE,
};

/* [한국어]
 * pci_ep_cfs_add_epc_group - controllers/ 아래에 이 EPC 의 디렉터리를 만든다
 *
 * @name: EPC 의 이름. 그 하드웨어 device 의 이름과 같다.
 * @return: 만들어진 그룹. 실패하면 ERR_PTR.
 *
 * EPC 가 등록될 때 __pci_epc_create() 가 부른다. 사용자가 mkdir 로
 * 만드는 것이 아니라 하드웨어가 있으면 저절로 생긴다 — 컨트롤러는
 * 실재하는 하드웨어라 사용자가 만들어 낼 수 없기 때문이다.
 * 이것이 functions/ 쪽과의 결정적 차이다.
 *
 * 여기서 pci_epc_get() 으로 참조를 올려 두는 것이 중요하다. 사용자가
 * 이 디렉터리를 통해 EPC 를 조작하는 동안 그 객체가 사라지면 안 된다.
 * 반납은 pci_ep_cfs_remove_epc_group() 이 한다.
 *
 * 실행 컨텍스트: EPC 등록 중 프로세스 컨텍스트.
 *
 * 에러 경로: 세 단계를 역순으로 되감는 표준적인 goto 사다리다.
 *
 * 호출 체인:
 *   컨트롤러 드라이버 probe → __pci_epc_create() [pci-epc-core.c]
 *     → [이 함수] → configfs_register_group() / pci_epc_get()
 */
struct config_group *pci_ep_cfs_add_epc_group(const char *name)
{
	int ret;
	/* [한국어] 이 디렉터리가 대표할 EPC. 아래에서 이름으로 찾는다. */
	struct pci_epc *epc;
	struct config_group *group;
	struct pci_epc_group *epc_group;

	/* [한국어] EPF 쪽과 달리 여기서는 구조체를 직접 할당한다.
	 * 부모에 박아 넣을 자리가 없기 때문이다. */
	epc_group = kzalloc_obj(*epc_group);
	if (!epc_group) {
		ret = -ENOMEM;
		goto err;
	}

	group = &epc_group->group;

	/* [한국어] 이름을 EPC 이름으로 하고, 링크 콜백과 start 속성을 담은
	 * 타입을 붙인다. */
	config_group_init_type_name(group, name, &pci_epc_type);
	/* [한국어] controllers/ 아래에 등록한다. 이 순간 사용자에게 보인다. */
	ret = configfs_register_group(controllers_group, group);
	if (ret) {
		/* [한국어] 아직 device 를 손에 쥐지 않았으므로 pr_err 을 쓴다. */
		pr_err("failed to register configfs group for %s\n", name);
		goto err_register_group;
	}

	/* [한국어] 이름으로 EPC 를 찾아 참조를 올린다. 사용자가 이 디렉터리를
	 * 쓰는 동안 EPC 와 그 드라이버 모듈이 살아 있게 하려는 것이다.
	 *
	 * 등록보다 나중에 하는 순서가 눈에 띄는데, 디렉터리를 먼저 만들고
	 * 나서 참조를 잡는 셈이라 그 사이의 틈이 있다. 다만 이 함수를 부르는
	 * 것이 EPC 생성 경로 자신이라 그 시점에 EPC 가 사라질 수는 없다. */
	epc = pci_epc_get(name);
	if (IS_ERR(epc)) {
		ret = PTR_ERR(epc);
		goto err_epc_get;
	}

	/* [한국어] 찾은 EPC 를 그룹에 기록한다. 이 참조는 remove 때 내린다. */
	epc_group->epc = epc;

	return group;

err_epc_get:
	/* [한국어] 등록한 그룹을 되돌린다. */
	configfs_unregister_group(group);

err_register_group:
	/* [한국어] 할당한 구조체를 해제한다. */
	kfree(epc_group);

err:
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(pci_ep_cfs_add_epc_group);

/* [한국어]
 * pci_ep_cfs_remove_epc_group - 그 디렉터리를 없앤다
 *
 * @group: 없앨 그룹. NULL 이어도 안전하다.
 * @return: 없음.
 *
 * add 의 역순이다. EPC 가 파괴될 때 pci_epc_destroy() 가 부른다.
 *
 * NULL 을 허용하는 이유는 add 가 실패해 ERR_PTR 을 돌려준 경우에도
 * 호출자가 그대로 넘길 수 있게 하려는 것으로 읽힌다. 다만 ERR_PTR 은
 * NULL 이 아니므로 그 경우까지 막아 주지는 않는다는 점은 알아 두어야
 * 한다 — pci_epc_destroy() 는 그 값을 그대로 넘긴다.
 *
 * 실행 컨텍스트: EPC 파괴 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_epc_destroy() [pci-epc-core.c] → [이 함수]
 */
void pci_ep_cfs_remove_epc_group(struct config_group *group)
{
	struct pci_epc_group *epc_group;

	if (!group)
		return;

	/* [한국어] 그룹에서 바깥 구조체를 되짚는다. */
	epc_group = container_of(group, struct pci_epc_group, group);
	/* [한국어] add 가 올린 참조를 내린다. */
	pci_epc_put(epc_group->epc);
	/* [한국어] configfs 에서 뗀다. */
	configfs_unregister_group(&epc_group->group);
	/* [한국어] 구조체를 해제한다. EPF 쪽과 달리 release 콜백을 거치지
	 * 않고 곧바로 해제하는데, 이 그룹은 사용자가 여는 대상이 아니라
	 * 참조가 남아 있을 일이 없기 때문으로 읽힌다. */
	kfree(epc_group);
}
EXPORT_SYMBOL(pci_ep_cfs_remove_epc_group);

#define PCI_EPF_HEADER_R(_name)						       \
static ssize_t pci_epf_##_name##_show(struct config_item *item,	char *page)    \
{									       \
	struct pci_epf *epf = to_pci_epf_group(item)->epf;		       \
	if (WARN_ON_ONCE(!epf->header))					       \
		return -EINVAL;						       \
	return sysfs_emit(page, "0x%04x\n", epf->header->_name);	       \
}

#define PCI_EPF_HEADER_W_u32(_name)					       \
static ssize_t pci_epf_##_name##_store(struct config_item *item,	       \
				       const char *page, size_t len)	       \
{									       \
	u32 val;							       \
	struct pci_epf *epf = to_pci_epf_group(item)->epf;		       \
	if (WARN_ON_ONCE(!epf->header))					       \
		return -EINVAL;						       \
	if (kstrtou32(page, 0, &val) < 0)				       \
		return -EINVAL;						       \
	epf->header->_name = val;					       \
	return len;							       \
}

#define PCI_EPF_HEADER_W_u16(_name)					       \
static ssize_t pci_epf_##_name##_store(struct config_item *item,	       \
				       const char *page, size_t len)	       \
{									       \
	u16 val;							       \
	struct pci_epf *epf = to_pci_epf_group(item)->epf;		       \
	if (WARN_ON_ONCE(!epf->header))					       \
		return -EINVAL;						       \
	if (kstrtou16(page, 0, &val) < 0)				       \
		return -EINVAL;						       \
	epf->header->_name = val;					       \
	return len;							       \
}

#define PCI_EPF_HEADER_W_u8(_name)					       \
static ssize_t pci_epf_##_name##_store(struct config_item *item,	       \
				       const char *page, size_t len)	       \
{									       \
	u8 val;								       \
	struct pci_epf *epf = to_pci_epf_group(item)->epf;		       \
	if (WARN_ON_ONCE(!epf->header))					       \
		return -EINVAL;						       \
	if (kstrtou8(page, 0, &val) < 0)				       \
		return -EINVAL;						       \
	epf->header->_name = val;					       \
	return len;							       \
}

/* [한국어]
 * pci_epf_msi_interrupts_store - 이 함수가 요청할 MSI 개수를 정한다
 *
 * @item: EPF 의 configfs 항목.
 * @page: 사용자가 쓴 숫자 문자열.
 * @len: 그 길이.
 * @return: 성공하면 @len, 파싱 실패면 -EINVAL.
 *
 * 여기에 쓴 값이 그대로 하드웨어에 반영되지는 않는다. EPF 드라이버가
 * bind 단계에서 이 값을 읽어 pci_epc_set_msi() 로 광고하고, 실제로
 * 몇 개가 켜질지는 저쪽 호스트가 정한다.
 *
 * 그래서 이 파일에 값을 쓰는 시점이 중요하다 — start 로 링크를 올리기
 * 전에 써 두어야 한다. 링크가 올라간 뒤에 바꿔 봐야 호스트는 이미
 * config 를 읽은 뒤다.
 *
 * u8 인 이유는 MSI 상한이 32 라 한 바이트로 충분하기 때문이다.
 * 범위 검증은 여기서 하지 않고 pci_epc_set_msi() 가 한다.
 *
 * 실행 컨텍스트: 사용자 프로세스(echo)의 컨텍스트.
 *
 * 호출 체인:
 *   사용자의 echo → configfs → [이 함수]
 *   (이후) EPF 드라이버의 bind → pci_epc_set_msi() [pci-epc-core.c]
 */
static ssize_t pci_epf_msi_interrupts_store(struct config_item *item,
					    const char *page, size_t len)
{
	u8 val;

	/* [한국어] 진법 0 을 주면 "16", "0x10", "020" 을 모두 알아서 읽는다. */
	if (kstrtou8(page, 0, &val) < 0)
		return -EINVAL;

	/* [한국어] EPF 객체에 기록만 해 둔다. 하드웨어 반영은 bind 때. */
	to_pci_epf_group(item)->epf->msi_interrupts = val;

	return len;
}

/* [한국어]
 * pci_epf_msi_interrupts_show - 설정해 둔 MSI 개수를 보여 준다
 *
 * @item: EPF 의 configfs 항목.
 * @page: 출력 버퍼.
 * @return: 쓴 바이트 수.
 *
 * 사용자가 써 둔 값을 그대로 돌려준다. 호스트가 실제로 켠 개수가
 * 아니라는 점에 주의해야 한다 — 그 값을 알려면 EPF 드라이버가
 * pci_epc_get_msi() 로 읽어야 하고, 이 파일은 그것을 보여 주지 않는다.
 *
 * 실행 컨텍스트: 사용자 프로세스(cat)의 컨텍스트.
 *
 * 호출 체인:
 *   사용자의 cat → configfs → [이 함수]
 */
static ssize_t pci_epf_msi_interrupts_show(struct config_item *item,
					   char *page)
{
	return sysfs_emit(page, "%d\n",
			  to_pci_epf_group(item)->epf->msi_interrupts);
}

/* [한국어]
 * pci_epf_msix_interrupts_store - 이 함수가 요청할 MSI-X 개수를 정한다
 *
 * @item: EPF 의 configfs 항목.
 * @page: 사용자가 쓴 숫자 문자열.
 * @len: 그 길이.
 * @return: 성공하면 @len.
 *
 * 위 MSI 판과 같되 타입이 u16 이다. MSI-X 는 최대 2048 개라 한 바이트로는
 * 담을 수 없기 때문이다(Table Size 필드가 11비트).
 *
 * NVMe 같은 다중 큐 장치가 MSI 가 아니라 MSI-X 를 쓰는 이유가 이 차이다 —
 * CPU 코어마다 벡터를 하나씩 두려면 32개로는 턱없이 모자란다.
 *
 * 실행 컨텍스트: 사용자 프로세스(echo)의 컨텍스트.
 *
 * 호출 체인:
 *   사용자의 echo → configfs → [이 함수]
 *   (이후) EPF 드라이버의 bind → pci_epc_set_msix() [pci-epc-core.c]
 */
static ssize_t pci_epf_msix_interrupts_store(struct config_item *item,
					     const char *page, size_t len)
{
	u16 val;

	/* [한국어] MSI-X 는 최대 2048 이라 u16 으로 받는다. */
	if (kstrtou16(page, 0, &val) < 0)
		return -EINVAL;

	/* [한국어] EPF 객체에 기록만 해 둔다. 하드웨어 반영은 드라이버의 bind 때. */
	to_pci_epf_group(item)->epf->msix_interrupts = val;

	return len;
}

/* [한국어]
 * pci_epf_msix_interrupts_show - 설정해 둔 MSI-X 개수를 보여 준다
 *
 * @item: EPF 의 configfs 항목.
 * @page: 출력 버퍼.
 * @return: 쓴 바이트 수.
 *
 * 위 MSI 판과 같다. 사용자가 써 둔 값이지 호스트가 켠 값이 아니다.
 *
 * 실행 컨텍스트: 사용자 프로세스(cat)의 컨텍스트.
 *
 * 호출 체인:
 *   사용자의 cat → configfs → [이 함수]
 */
static ssize_t pci_epf_msix_interrupts_show(struct config_item *item,
					    char *page)
{
	return sysfs_emit(page, "%d\n",
			  to_pci_epf_group(item)->epf->msix_interrupts);
}

PCI_EPF_HEADER_R(vendorid)
PCI_EPF_HEADER_W_u16(vendorid)

PCI_EPF_HEADER_R(deviceid)
PCI_EPF_HEADER_W_u16(deviceid)

PCI_EPF_HEADER_R(revid)
PCI_EPF_HEADER_W_u8(revid)

PCI_EPF_HEADER_R(progif_code)
PCI_EPF_HEADER_W_u8(progif_code)

PCI_EPF_HEADER_R(subclass_code)
PCI_EPF_HEADER_W_u8(subclass_code)

PCI_EPF_HEADER_R(baseclass_code)
PCI_EPF_HEADER_W_u8(baseclass_code)

PCI_EPF_HEADER_R(cache_line_size)
PCI_EPF_HEADER_W_u8(cache_line_size)

PCI_EPF_HEADER_R(subsys_vendor_id)
PCI_EPF_HEADER_W_u16(subsys_vendor_id)

PCI_EPF_HEADER_R(subsys_id)
PCI_EPF_HEADER_W_u16(subsys_id)

PCI_EPF_HEADER_R(interrupt_pin)
PCI_EPF_HEADER_W_u8(interrupt_pin)

/* [한국어] 아래 열두 줄이 각 헤더 필드의 show/store 를 configfs 속성으로 묶는다.
 * 앞의 PCI_EPF_HEADER_R / _W 매크로가 만들어 둔 함수들을 이름으로 찾아
 * 쓴다. 이 값들이 곧 호스트가 보게 될 config space 헤더가 된다. */
CONFIGFS_ATTR(pci_epf_, vendorid);
/* [한국어] 장치 ID. 벤더 ID 와 함께 호스트의 드라이버 매칭에 쓰인다. */
CONFIGFS_ATTR(pci_epf_, deviceid);
/* [한국어] 리비전. */
CONFIGFS_ATTR(pci_epf_, revid);
/* [한국어] 프로그래밍 인터페이스. 클래스 코드의 셋째 바이트다. */
CONFIGFS_ATTR(pci_epf_, progif_code);
/* [한국어] 서브클래스. 클래스 코드의 둘째 바이트. */
CONFIGFS_ATTR(pci_epf_, subclass_code);
/* [한국어] 베이스 클래스. 이 셋을 합친 0x010802 가 NVM Express 를 뜻한다. */
CONFIGFS_ATTR(pci_epf_, baseclass_code);
/* [한국어] 캐시 라인 크기. */
CONFIGFS_ATTR(pci_epf_, cache_line_size);
/* [한국어] 서브시스템 벤더 ID. */
CONFIGFS_ATTR(pci_epf_, subsys_vendor_id);
/* [한국어] 서브시스템 ID. */
CONFIGFS_ATTR(pci_epf_, subsys_id);
/* [한국어] INTx 핀 번호. */
CONFIGFS_ATTR(pci_epf_, interrupt_pin);
/* [한국어] 요청할 MSI 개수. */
CONFIGFS_ATTR(pci_epf_, msi_interrupts);
/* [한국어] 요청할 MSI-X 개수. */
CONFIGFS_ATTR(pci_epf_, msix_interrupts);

/* [한국어] 위 속성들을 묶은 목록. EPF 디렉터리에 이 파일들이 나타난다. */
static struct configfs_attribute *pci_epf_attrs[] = {
	&pci_epf_attr_vendorid,
	&pci_epf_attr_deviceid,
	&pci_epf_attr_revid,
	&pci_epf_attr_progif_code,
	&pci_epf_attr_subclass_code,
	&pci_epf_attr_baseclass_code,
	&pci_epf_attr_cache_line_size,
	&pci_epf_attr_subsys_vendor_id,
	&pci_epf_attr_subsys_id,
	&pci_epf_attr_interrupt_pin,
	&pci_epf_attr_msi_interrupts,
	&pci_epf_attr_msix_interrupts,
	NULL,
};

/* [한국어]
 * pci_epf_vepf_link - EPF 아래에 다른 EPF 를 링크하면 가상 함수로 만든다
 *
 * @epf_pf_item: 링크가 걸린 자리. 물리 함수가 될 EPF 다.
 * @epf_vf_item: 링크가 가리키는, 가상 함수가 될 EPF.
 * @return: pci_epf_add_vepf() 의 결과.
 *
 * PCIe 장치 하나가 함수를 여러 개 갖는 구성(다중 함수 장치)을 만드는
 * 방법이다. 사용자가
 *   ln -s functions/<종류>/func2 functions/<종류>/func1/
 * 처럼 EPF 아래에 다른 EPF 를 링크하면 그것이 가상 함수가 된다.
 *
 * 앞서 본 EPC 링크와 대상이 다르다는 점에 주의해야 한다 — 그쪽은
 * EPF 를 컨트롤러에 붙이는 것이고, 이쪽은 EPF 를 다른 EPF 에 매다는 것이다.
 *
 * 실행 컨텍스트: 사용자 프로세스(ln)의 컨텍스트.
 *
 * 에러 경로: add_vepf 의 오류를 그대로 전한다. 이미 EPC 에 붙어 있는
 *   EPF 라면 -EBUSY 가 나온다 — 구성을 바꾸려면 먼저 떼어 내야 한다.
 *
 * 호출 체인:
 *   사용자의 ln → configfs → [이 함수]
 *     → pci_epf_add_vepf() [pci-epf-core.c]
 */
static int pci_epf_vepf_link(struct config_item *epf_pf_item,
			     struct config_item *epf_vf_item)
{
	/* [한국어] 둘 다 EPF 디렉터리 자체라 부모를 거치지 않는다. */
	struct pci_epf_group *epf_vf_group = to_pci_epf_group(epf_vf_item);
	struct pci_epf_group *epf_pf_group = to_pci_epf_group(epf_pf_item);
	struct pci_epf *epf_pf = epf_pf_group->epf;
	struct pci_epf *epf_vf = epf_vf_group->epf;

	/* [한국어] 실제 연결은 코어가 한다. 가상 함수 번호 배정과
	 * is_vf 표시까지 거기서 이뤄진다. */
	return pci_epf_add_vepf(epf_pf, epf_vf);
}

/* [한국어]
 * pci_epf_vepf_unlink - 그 링크를 지우면 가상 함수 관계를 끊는다
 *
 * @epf_pf_item: 물리 함수 쪽 EPF.
 * @epf_vf_item: 가상 함수 쪽 EPF.
 * @return: 없음.
 *
 * link 의 역순이다. 가상 함수 번호를 반납하고 목록에서 뺀다.
 *
 * 실행 컨텍스트: 사용자 프로세스(rm)의 컨텍스트.
 *
 * 호출 체인:
 *   사용자의 rm → configfs → [이 함수]
 *     → pci_epf_remove_vepf() [pci-epf-core.c]
 */
static void pci_epf_vepf_unlink(struct config_item *epf_pf_item,
				struct config_item *epf_vf_item)
{
	struct pci_epf_group *epf_vf_group = to_pci_epf_group(epf_vf_item);
	/* [한국어] 물리 함수가 될 쪽. */
	struct pci_epf_group *epf_pf_group = to_pci_epf_group(epf_pf_item);
	/* [한국어] 그 EPF 객체. */
	struct pci_epf *epf_pf = epf_pf_group->epf;
	/* [한국어] 가상 함수가 될 EPF 객체. */
	struct pci_epf *epf_vf = epf_vf_group->epf;

	pci_epf_remove_vepf(epf_pf, epf_vf);
}

/* [한국어]
 * pci_epf_release - EPF 디렉터리의 마지막 참조가 사라졌을 때 정리한다
 *
 * @item: 해제될 configfs 항목.
 * @return: 없음.
 *
 * 사용자가 rmdir 하면 곧바로 이것이 불리는 것이 아니다. configfs 가
 * 참조를 세다가 마지막 하나가 사라질 때 부른다 — 그 사이에 누군가
 * 이 디렉터리의 파일을 열고 있을 수 있기 때문이다.
 *
 * 세 가지를 정리한다. idr 번호, EPF 객체, 그리고 그룹 구조체.
 * 순서가 중요한데, 구조체를 마지막에 해제해야 그 안의 index 와 epf 를
 * 먼저 쓸 수 있다.
 *
 * 실행 컨텍스트: 마지막 config_item_put 을 부른 문맥. 대개 사용자
 *   프로세스이지만 파일을 닫는 시점일 수도 있다.
 *
 * 호출 체인:
 *   (마지막 config_item_put) → configfs → [이 함수]
 *     → idr_remove() → pci_epf_destroy() [pci-epf-core.c]
 */
static void pci_epf_release(struct config_item *item)
{
	struct pci_epf_group *epf_group = to_pci_epf_group(item);

	/* [한국어] 배정받은 번호를 반납한다. 다음 EPF 가 그 번호를 쓸 수 있다. */
	mutex_lock(&functions_mutex);
	idr_remove(&functions_idr, epf_group->index);
	mutex_unlock(&functions_mutex);
	/* [한국어] EPF 객체를 파괴한다. 붙어 있던 드라이버가 떨어지고,
	 * 그쪽의 참조가 다 빠지면 pci_epf_dev_release() 가 이름과 구조체를
	 * 해제한다. */
	pci_epf_destroy(epf_group->epf);
	/* [한국어] 마지막으로 이 그룹 구조체. */
	kfree(epf_group);
}

/* [한국어] EPF 디렉터리의 항목 동작 표. 링크 둘과 해제 하나를 담는다. */
static const struct configfs_item_operations pci_epf_ops = {
	/* [한국어] 다른 EPF 를 링크하면 가상 함수가 된다. */
	.allow_link		= pci_epf_vepf_link,
	.drop_link		= pci_epf_vepf_unlink,
	.release		= pci_epf_release,
};

/* [한국어] EPF 디렉터리의 타입. 위 동작 표와 헤더 속성 목록을 함께 담는다. */
static const struct config_item_type pci_epf_type = {
	/* [한국어] 링크·해제 콜백 표. */
	.ct_item_ops	= &pci_epf_ops,
	.ct_attrs	= pci_epf_attrs,
	.ct_owner	= THIS_MODULE,
};

/**
 * pci_epf_type_add_cfs() - Help function drivers to expose function specific
 *                          attributes in configfs
 * @epf: the EPF device that has to be configured using configfs
 * @group: the parent configfs group (corresponding to entries in
 *         pci_epf_device_id)
 *
 * Invoke to expose function specific attributes in configfs.
 *
 * Return: A pointer to a config_group structure or NULL if the function driver
 * does not have anything to expose (attributes configured by user) or if
 * the function driver does not implement the add_cfs() method.
 *
 * Returns an error pointer if this function is called for an unbound EPF device
 * or if the EPF driver add_cfs() method fails.
 */
/* [한국어]
 * pci_epf_type_add_cfs - EPF 드라이버가 노출할 configfs 그룹을 얻는다
 *
 * @epf: 대상 EPF 장치.
 * @group: 부모 configfs 그룹.
 * @return: 드라이버가 만든 그룹, NULL(그런 설정이 없음), 또는 ERR_PTR.
 *
 * EPF(Endpoint Function) 드라이버마다 사용자가 조정할 항목이 다르다.
 * 그것을 코어가 미리 알 수 없으므로, 드라이버에게 물어 그 그룹을 받아
 * configfs 트리에 붙인다.
 *
 * 세 가지 반환이 각각 다른 뜻이다. ERR_PTR 은 드라이버가 아직 바인딩되지
 * 않아 물어볼 상대가 없다는 뜻이고, NULL 은 드라이버는 있으나 노출할 설정이
 * 없다는 뜻이며, 유효한 포인터라야 실제 그룹이다. 호출자가 셋을 구분해
 * 처리해야 한다.
 *
 * 실행 컨텍스트: configfs 조작. 프로세스 컨텍스트.
 *
 * 에러 경로: 드라이버 미바인딩만 ERR_PTR(-ENODEV)이며 로그를 남긴다.
 *
 * 호출 체인:
 *   configfs 의 EPF 그룹 생성 → [이 함수] → epf->driver->ops->add_cfs()
 */
static struct config_group *pci_epf_type_add_cfs(struct pci_epf *epf,
						 struct config_group *group)
{
	struct config_group *epf_type_group;

	/* [한국어] 드라이버가 붙지 않았으면 물어볼 상대가 없다.
	 * 이 시점에는 이미 붙어 있어야 정상이다 — pci_epf_create() 가
	 * device_add 로 매칭을 촉발했기 때문이다. */
	if (!epf->driver) {
		dev_err(&epf->dev, "epf device not bound to driver\n");
		return ERR_PTR(-ENODEV);
	}

	/* [한국어] 상류 kernel-doc 이 밝힌 대로, 드라이버가 노출할 설정
	 * 항목이 없으면 이 콜백을 구현하지 않는다. 오류가 아니므로
	 * ERR_PTR 이 아니라 NULL 을 준다 — 호출자가 그 둘을 구분한다. */
	if (!epf->driver->ops->add_cfs)
		return NULL;

	/* [한국어] 드라이버가 자기만의 configfs 그룹을 만들어 돌려준다.
	 * 예컨대 시험용 드라이버라면 전송 크기나 시험 종류를 정하는
	 * 파일들을 여기에 담을 수 있다.
	 * 락을 잡는 이유는 드라이버가 이 콜백 안에서 EPF 상태를 만질 수
	 * 있기 때문이다. */
	mutex_lock(&epf->lock);
	epf_type_group = epf->driver->ops->add_cfs(epf, group);
	mutex_unlock(&epf->lock);

	return epf_type_group;
}

/* [한국어]
 * pci_ep_cfs_add_type_group - 드라이버 전용 설정 그룹이 있으면 붙인다
 *
 * @epf_group: 대상 EPF 그룹.
 * @return: 없음.
 *
 * 위 pci_epf_type_add_cfs() 를 부르고 그 결과를 처리한다. 세 갈래다.
 *   NULL    — 드라이버가 노출할 것이 없다. 정상이므로 조용히 넘어간다.
 *   ERR_PTR — 만들려다 실패했다. 메시지를 남기되 EPF 자체는 살린다.
 *   그 외   — 성공. 기본 그룹으로 붙여 부모와 수명을 묶는다.
 *
 * 실패해도 EPF 를 없애지 않는 점이 이 함수의 판단이다. 드라이버 전용
 * 설정이 없어도 기본 설정(vendorid 등)만으로 동작할 수 있기 때문이다.
 *
 * 실행 컨텍스트: EPF 생성 중 사용자 프로세스의 컨텍스트.
 *
 * 호출 체인:
 *   pci_epf_make() → pci_epf_cfs_add_sub_groups() → [이 함수]
 *     → pci_epf_type_add_cfs() → 드라이버의 add_cfs 콜백
 */
static void pci_ep_cfs_add_type_group(struct pci_epf_group *epf_group)
{
	struct config_group *group;

	group = pci_epf_type_add_cfs(epf_group->epf, &epf_group->group);
	/* [한국어] 드라이버가 노출할 것이 없다. 정상이다. */
	if (!group)
		return;

	if (IS_ERR(group)) {
		/* [한국어] %pe 는 오류 포인터를 "-ENOMEM" 같은 이름으로 찍어
		 * 주는 커널 확장 서식이다. 숫자보다 읽기 쉽다. */
		dev_err(&epf_group->epf->dev,
			"failed to create epf type specific attributes: %pe\n",
			group);
		return;
	}

	/* [한국어] 기본 그룹으로 붙인다. 사용자가 만들거나 지울 수 없고
	 * 부모와 함께 생겼다 사라진다. */
	configfs_add_default_group(group, &epf_group->group);
}

/* [한국어]
 * pci_epf_cfs_add_sub_groups - EPF 디렉터리 아래의 하위 그룹들을 만든다
 *
 * @epf_group: 대상 EPF 그룹.
 * @return: 없음.
 *
 * EPF 하나가 만들어지면 그 아래에 세 가지가 따라 생긴다.
 *   primary/   — 주 인터페이스에 붙일 EPC 를 링크할 자리
 *   secondary/ — 보조 인터페이스용
 *   드라이버 전용 그룹 — 있는 경우에만
 *
 * 오류를 상위로 전하지 않고 메시지만 남기는 점이 이 함수의 성격이다.
 * 하위 그룹을 못 만들어도 EPF 자체는 쓸 수 있으므로, 부분적으로라도
 * 살려 두는 편이 낫다는 판단이다.
 *
 * 다만 그 결과 primary/ 가 없는 EPF 가 남을 수 있는데, 그때는
 * controllers/ 쪽에서 링크하는 다른 경로를 쓰면 된다.
 *
 * 실행 컨텍스트: EPF 생성 중 사용자 프로세스의 컨텍스트.
 *
 * 호출 체인:
 *   사용자의 mkdir → configfs → pci_epf_make() → [이 함수]
 */
static void pci_epf_cfs_add_sub_groups(struct pci_epf_group *epf_group)
{
	struct config_group *group;

	/* [한국어] primary/ 를 만든다. */
	group = pci_ep_cfs_add_primary_group(epf_group);
	if (IS_ERR(group)) {
		dev_err(&epf_group->epf->dev,
			"failed to create 'primary' EPC interface: %pe\n",
			group);
		/* [한국어] 여기서 돌아가면 secondary 와 드라이버 그룹도
		 * 만들지 않는다. primary 조차 안 되는 상황이면 나머지도
		 * 안 될 가능성이 높다고 본 것이다. */
		return;
	}

	/* [한국어] secondary/ 를 만든다. */
	group = pci_ep_cfs_add_secondary_group(epf_group);
	if (IS_ERR(group)) {
		dev_err(&epf_group->epf->dev,
			"failed to create 'secondary' EPC interface: %pe\n",
			group);
		return;
	}

	/* [한국어] 마지막으로 드라이버 전용 그룹. 없으면 조용히 넘어간다. */
	pci_ep_cfs_add_type_group(epf_group);
}

/* [한국어]
 * pci_epf_make - 사용자가 mkdir 하면 EPF 를 하나 만든다
 *
 * @group: 부모 그룹. functions/<종류>/ 에 해당하며, 그 이름이 곧
 *   만들 EPF 의 종류다.
 * @name: 사용자가 지은 디렉터리 이름.
 * @return: 만들어진 그룹. 실패하면 ERR_PTR 이 사용자의 mkdir 에 전달된다.
 *
 * configfs 의 make_group 콜백이며, 이 파일에서 가장 중요한 함수다.
 * 사용자가
 *   mkdir /sys/kernel/config/pci_ep/functions/pci_epf_test/func1
 * 을 실행하면 불린다.
 *
 * 눈여겨볼 것은 EPF 에 넘길 이름을 만드는 방식이다. 사용자가 지은
 * 디렉터리 이름(func1)이 아니라 "<부모 이름>.<번호>" 로 새로 만든다.
 *   부모 이름 = 종류 이름 = "pci_epf_test"
 *   번호      = idr 에서 받은 것
 * 그래서 EPF 의 device 이름은 "pci_epf_test.0" 이 된다.
 *
 * 왜 그렇게 하는가. pci_epf_create() 가 '.' 앞부분만 잘라 매칭용
 * 이름으로 쓰기 때문이다(pci-epf-core.c 참고). 종류 이름을 앞에 두어야
 * 그 종류의 드라이버가 붙는다. 사용자가 지은 이름은 configfs 디렉터리
 * 이름으로만 남고 드라이버 매칭에는 관여하지 않는다.
 *
 * 실행 컨텍스트: 사용자 프로세스(mkdir)의 컨텍스트. 메모리 할당과
 *   드라이버 probe 가 일어나 잠들 수 있다.
 *
 * 에러 경로: goto 사다리로 역순 정리. 성공 경로에서도 epf_name 을
 *   해제하는 점에 주의 — pci_epf_create() 가 그 문자열을 복사해 두므로
 *   여기서 계속 들고 있을 필요가 없다.
 *
 * 호출 체인:
 *   사용자의 mkdir → configfs → [이 함수]
 *     → idr_alloc() → pci_epf_create() [pci-epf-core.c]
 *     → pci_epf_cfs_add_sub_groups()
 */
static struct config_group *pci_epf_make(struct config_group *group,
					 const char *name)
{
	struct pci_epf_group *epf_group;
	struct pci_epf *epf;
	/* [한국어] EPF 에 넘길 "<종류>.<번호>" 이름. 아래에서 만든다. */
	char *epf_name;
	int index, err;

	/* [한국어] 그룹 구조체를 잡는다. 0 으로 초기화된다. */
	epf_group = kzalloc_obj(*epf_group);
	if (!epf_group)
		return ERR_PTR(-ENOMEM);

	/* [한국어] 번호를 배정받는다. idr 에 epf_group 자체를 저장하지만
	 * 이 파일에서 그 값을 되찾아 쓰는 곳은 없다 — 번호 배정만이
	 * 목적이고 저장은 idr API 의 형식상 필요한 것이다. */
	mutex_lock(&functions_mutex);
	index = idr_alloc(&functions_idr, epf_group, 0, 0, GFP_KERNEL);
	mutex_unlock(&functions_mutex);
	/* [한국어] idr 이 다 찼거나 메모리가 부족한 경우. */
	if (index < 0) {
		err = index;
		goto free_group;
	}

	epf_group->index = index;

	/* [한국어] configfs 그룹은 사용자가 지은 이름 그대로 초기화한다.
	 * 디렉터리에 보이는 것이 이 이름이다. */
	config_group_init_type_name(&epf_group->group, name, &pci_epf_type);

	/* [한국어] EPF 에 넘길 이름은 따로 만든다. 부모 그룹의 이름(=종류)에
	 * 번호를 붙인 것으로, 드라이버 매칭이 이 앞부분으로 이뤄진다. */
	epf_name = kasprintf(GFP_KERNEL, "%s.%d",
			     group->cg_item.ci_name, epf_group->index);
	if (!epf_name) {
		err = -ENOMEM;
		goto remove_idr;
	}

	/* [한국어] 실제 EPF 객체를 만든다. 이 안에서 가상 버스에 등록되고
	 * 짝이 맞는 드라이버가 곧바로 붙을 수 있다. */
	epf = pci_epf_create(epf_name);
	if (IS_ERR(epf)) {
		err = PTR_ERR(epf);
		/* [한국어] 어떤 이름으로 실패했는지 남긴다. 이 시점에는 아직 device 가 없어
		 * pr_err 를 쓴다. */
		pr_err("failed to create endpoint function device (%s): %d\n",
			epf_name, err);
		goto free_name;
	}

	/* [한국어] 서로를 가리키게 한다. EPF 쪽에서도 자기 configfs 그룹을
	 * 알아야 드라이버가 하위 그룹을 붙일 수 있다. */
	epf->group = &epf_group->group;
	epf_group->epf = epf;

	/* [한국어] pci_epf_create 가 이름을 복사해 두었으므로 여기서 해제한다.
	 * 성공 경로에서도 해제하는 것이 맞다. */
	kfree(epf_name);

	/* [한국어] primary/, secondary/, 그리고 드라이버 전용 그룹을 붙인다.
	 * 실패해도 오류를 전하지 않으므로 반환값이 없다. */
	pci_epf_cfs_add_sub_groups(epf_group);

	return &epf_group->group;

free_name:
	kfree(epf_name);

remove_idr:
	/* [한국어] 배정받은 번호를 반납한다. */
	mutex_lock(&functions_mutex);
	idr_remove(&functions_idr, epf_group->index);
	mutex_unlock(&functions_mutex);

free_group:
	kfree(epf_group);

	return ERR_PTR(err);
}

/* [한국어]
 * pci_epf_drop - 사용자가 rmdir 하면 참조를 하나 내린다
 *
 * @group: 부모 그룹.
 * @item: 없앨 항목.
 * @return: 없음.
 *
 * 한 줄짜리 함수인데, 여기서 실제 해제가 일어나지 않는다는 점이 요점이다.
 * 참조를 하나 내릴 뿐이고, 그것이 마지막이면 configfs 가
 * pci_epf_release() 를 불러 그쪽에서 정리한다.
 *
 * 왜 나눠 두었는가. rmdir 하는 순간에도 다른 프로세스가 그 디렉터리의
 * 파일을 열고 있을 수 있기 때문이다. 참조 카운트가 그 상황을 안전하게
 * 처리한다 — 디렉터리는 즉시 사라지지만 실제 메모리 해제는 마지막
 * 사용자가 손을 뗄 때까지 미뤄진다.
 *
 * 실행 컨텍스트: 사용자 프로세스(rmdir)의 컨텍스트.
 *
 * 호출 체인:
 *   사용자의 rmdir → configfs → [이 함수] → config_item_put()
 *     → (마지막 참조라면) pci_epf_release()
 */
static void pci_epf_drop(struct config_group *group, struct config_item *item)
{
	config_item_put(item);
}

/* [한국어] functions/<종류>/ 디렉터리의 그룹 동작 표. 사용자가 그 아래에
 * mkdir/rmdir 할 수 있게 하는 두 콜백이다. */
static const struct configfs_group_operations pci_epf_group_ops = {
	/* [한국어] mkdir 하면 EPF 를 하나 만든다. */
	.make_group     = &pci_epf_make,
	.drop_item      = &pci_epf_drop,
};

/* [한국어] 그 종류 디렉터리의 타입. */
static const struct config_item_type pci_epf_group_type = {
	/* [한국어] 위 그룹 동작 표를 연결한다. */
	.ct_group_ops	= &pci_epf_group_ops,
	.ct_owner	= THIS_MODULE,
};

/* [한국어]
 * pci_ep_cfs_add_epf_group - functions/ 아래에 이 종류의 디렉터리를 만든다
 *
 * @name: EPF 종류의 이름. 드라이버 id 표의 이름과 같다.
 * @return: 만들어진 그룹. 실패하면 ERR_PTR.
 *
 * EPF 드라이버가 등록될 때 pci_epf_add_cfs() 가 부른다.
 * 이 호출로 functions/pci_epf_test/ 같은 디렉터리가 생기고, 사용자는
 * 그 아래에 mkdir 해서 실제 인스턴스를 만들 수 있게 된다.
 *
 * "default group" 으로 등록하는 점이 중요하다. 사용자가 만들거나 지울
 * 수 없고 드라이버의 수명에 묶인다 — 종류는 드라이버가 있어야 존재하는
 * 것이지 사용자가 만들어 낼 수 있는 것이 아니기 때문이다.
 * 반면 그 아래의 인스턴스는 사용자가 mkdir 로 만든다.
 *
 * 실행 컨텍스트: 드라이버 등록 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   EPF 드라이버 등록 → __pci_epf_register_driver() → pci_epf_add_cfs()
 *   [pci-epf-core.c] → [이 함수]
 */
struct config_group *pci_ep_cfs_add_epf_group(const char *name)
{
	struct config_group *group;

	/* [한국어] functions/ 아래에 기본 그룹으로 등록한다.
	 * pci_epf_group_type 에 make_group 콜백이 들어 있어, 사용자가
	 * 이 디렉터리 아래에 mkdir 하면 pci_epf_make() 가 불린다. */
	group = configfs_register_default_group(functions_group, name,
						&pci_epf_group_type);
	if (IS_ERR(group))
		/* [한국어] 실패해도 오류를 그대로 돌려준다. 다만 호출자인
		 * pci_epf_add_cfs() 는 그때까지 만든 것을 되돌린다. */
		pr_err("failed to register configfs group for %s function: %pe\n",
		       name, group);

	/* [한국어] 성공이든 실패든 그대로 돌려준다. 호출자가 IS_ERR 로 판단한다. */
	return group;
}
EXPORT_SYMBOL(pci_ep_cfs_add_epf_group);

/* [한국어]
 * pci_ep_cfs_remove_epf_group - 그 종류의 디렉터리를 없앤다
 *
 * @group: 없앨 그룹. ERR_PTR 이나 NULL 이어도 안전하다.
 * @return: 없음.
 *
 * add 의 역순이다. 드라이버가 등록 해제될 때 pci_epf_remove_cfs() 가
 * 목록을 순회하며 부른다.
 *
 * list_del 을 여기서 하는 것이 중요하다. 호출자가 _safe 순회를 쓰면서
 * 이 함수가 목록에서 빼기를 기대하고 있고, 실제로 그 뒤에 목록이
 * 비었는지 WARN_ON 으로 확인한다. 즉 두 파일의 약속이다.
 *
 * ERR_PTR 까지 허용하는 이유는 add 가 실패한 값을 호출자가 그대로
 * 넘길 수 있게 하려는 것이다.
 *
 * 실행 컨텍스트: 드라이버 등록 해제 중 프로세스 컨텍스트.
 *   호출자가 pci_epf_mutex 를 쥐고 있다.
 *
 * 호출 체인:
 *   pci_epf_unregister_driver() → pci_epf_remove_cfs() [pci-epf-core.c]
 *     → [이 함수]
 */
void pci_ep_cfs_remove_epf_group(struct config_group *group)
{
	if (IS_ERR_OR_NULL(group))
		return;

	/* [한국어] 드라이버의 epf_group 목록에서 뺀다. 호출자의 WARN_ON 이
	 * 이것을 전제로 목록이 비었는지 확인한다. */
	list_del(&group->group_entry);
	/* [한국어] configfs 에서 없앤다. 그 아래에 사용자가 만든 인스턴스가
	 * 있으면 그것들도 함께 정리된다. */
	configfs_unregister_default_group(group);
}
EXPORT_SYMBOL(pci_ep_cfs_remove_epf_group);

/* [한국어] functions/ 디렉터리의 타입. 동작 표가 없어 사용자가 여기에 직접
 * mkdir 할 수는 없다 — 그 아래 종류 디렉터리에서만 만들 수 있다. */
static const struct config_item_type pci_functions_type = {
	/* [한국어] 이 모듈이 소유자임을 알려 사용 중에 언로드되지 않게 한다. */
	.ct_owner	= THIS_MODULE,
};

/* [한국어] controllers/ 디렉터리의 타입. 역시 사용자가 만들 수 없다 —
 * 컨트롤러는 하드웨어가 있어야 생긴다. */
static const struct config_item_type pci_controllers_type = {
	/* [한국어] 소유 모듈. */
	.ct_owner	= THIS_MODULE,
};

/* [한국어] 최상위 pci_ep/ 디렉터리의 타입. */
static const struct config_item_type pci_ep_type = {
	/* [한국어] 소유 모듈. */
	.ct_owner	= THIS_MODULE,
};

/* [한국어] configfs 서브시스템 정의. 정적으로 초기화해 두고 init 에서 등록한다. */
static struct configfs_subsystem pci_ep_cfs_subsys = {
	/* [한국어] 최상위 그룹. */
	.su_group = {
		/* [한국어] 그 그룹의 항목 부분. */
		.cg_item = {
			/* [한국어] /sys/kernel/config/ 아래에 이 이름으로 나타난다. */
			.ci_namebuf = "pci_ep",
			.ci_type = &pci_ep_type,
		},
	},
	.su_mutex = __MUTEX_INITIALIZER(pci_ep_cfs_subsys.su_mutex),
};

/* [한국어]
 * pci_ep_cfs_init - configfs 서브시스템과 두 최상위 디렉터리를 만든다
 *
 * @return: 0 이면 성공. 실패하면 모듈이 적재되지 않는다.
 *
 * /sys/kernel/config/pci_ep/ 와 그 아래 functions/, controllers/ 를 만든다.
 * 이 셋이 있어야 EPC 와 EPF 가 등록될 때 각자의 디렉터리를 그 아래에
 * 붙일 수 있으므로, 다른 어떤 등록보다 먼저 실행되어야 한다.
 *
 * functions/ 와 controllers/ 도 기본 그룹이다 — 사용자가 만들거나
 * 지울 수 없는 고정된 구조다.
 *
 * 실행 컨텍스트: 모듈 적재 중 프로세스 컨텍스트.
 *
 * 에러 경로: goto 사다리로 역순 정리한다.
 *
 * 호출 체인:
 *   (모듈 적재) → module_init → [이 함수]
 *     → configfs_register_subsystem() → configfs_register_default_group() x2
 */
static int __init pci_ep_cfs_init(void)
{
	int ret;
	/* [한국어] 최상위 그룹. 정적으로 정의된 서브시스템 안에 있다. */
	struct config_group *root = &pci_ep_cfs_subsys.su_group;

	/* [한국어] 이름과 타입은 정적 초기화로 이미 채워져 있고,
	 * 여기서는 리스트 등 실행 시점 필드를 초기화한다. */
	config_group_init(root);

	/* [한국어] /sys/kernel/config/pci_ep/ 가 만들어진다. */
	ret = configfs_register_subsystem(&pci_ep_cfs_subsys);
	if (ret) {
		pr_err("Error %d while registering subsystem %s\n",
		       ret, root->cg_item.ci_namebuf);
		goto err;
	}

	/* [한국어] functions/ — EPF 종류들이 모일 곳. 전역 변수에 담아 두어
	 * pci_ep_cfs_add_epf_group() 이 쓴다. */
	functions_group = configfs_register_default_group(root, "functions",
							  &pci_functions_type);
	if (IS_ERR(functions_group)) {
		/* [한국어] 오류를 꺼내 아래 정리 경로로 간다. */
		ret = PTR_ERR(functions_group);
		/* [한국어] 어느 그룹에서 실패했는지 구분해 알린다. */
		pr_err("Error %d while registering functions group\n",
		       ret);
		goto err_functions_group;
	}

	/* [한국어] controllers/ — EPC 들이 모일 곳. 마찬가지로 전역에 담는다. */
	controllers_group =
		configfs_register_default_group(root, "controllers",
						&pci_controllers_type);
	/* [한국어] controllers/ 생성 실패. */
	if (IS_ERR(controllers_group)) {
		/* [한국어] 오류를 꺼낸다. */
		ret = PTR_ERR(controllers_group);
		/* [한국어] 어느 그룹인지 밝혀 진단을 돕는다. */
		pr_err("Error %d while registering controllers group\n",
		       ret);
		goto err_controllers_group;
	}

	return 0;

err_controllers_group:
	configfs_unregister_default_group(functions_group);

err_functions_group:
	configfs_unregister_subsystem(&pci_ep_cfs_subsys);

err:
	return ret;
}
module_init(pci_ep_cfs_init);

/* [한국어] 모듈 제거 시 서브시스템 등록을 해제한다. 그 아래 그룹들은 함께
 * 정리되므로 따로 없앨 것이 없다. 이 시점에는 등록된 EPC/EPF 가 없어야
 * 하며, 모듈 의존 관계가 그 순서를 지켜 준다. */
static void __exit pci_ep_cfs_exit(void)
{
	configfs_unregister_default_group(controllers_group);
	configfs_unregister_default_group(functions_group);
	configfs_unregister_subsystem(&pci_ep_cfs_subsys);
}
module_exit(pci_ep_cfs_exit);

MODULE_DESCRIPTION("PCI EP CONFIGFS");
MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
