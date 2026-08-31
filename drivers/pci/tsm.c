// SPDX-License-Identifier: GPL-2.0
/*
 * Interface with platform TEE Security Manager (TSM) objects as defined by
 * PCIe r7.0 section 11 TEE Device Interface Security Protocol (TDISP)
 *
 * Copyright(c) 2024-2025 Intel Corporation. All rights reserved.
 */

/*
 * [한국어 설명] TEE Security Manager 연동 계층 (tsm.c)
 *
 * === 파일의 역할 ===
 * 기밀 컴퓨팅 환경에서 PCIe 장치를 안전하게 쓰기 위한 PCI 계층 인프라다.
 * 문제의 출발점은 신뢰 모델이다. 기밀 VM 은 자기를 실행시키는 호스트
 * 하이퍼바이저조차 믿지 않는다. 그런데 장치를 직접 쓰려면(패스스루)
 * 그 장치가 진짜인지, 링크 위를 흐르는 데이터를 호스트가 훔쳐보거나
 * 바꿔치기하지 않는지 보장해야 한다.
 * PCIe 스펙(r7.0 11장)이 내놓은 답이 TDISP(TEE Device Interface Security
 * Protocol)이고, 그 절차를 실제로 수행하는 주체가 플랫폼의 TSM 이다.
 * 이 파일은 TSM 을 PCI 계층에 붙이는 접착제 역할을 한다 - 직접 암호를
 * 다루거나 프로토콜을 구현하지 않고, 어떤 장치가 어떤 TSM 에 매여 있는지
 * 관리하고, sysfs 로 제어면을 열고, DOE 통로를 빌려 준다.
 * 세 가지 상태 전이를 다룬다. connect(세션 수립), bind(기밀 VM 에 넘김),
 * 그리고 그 역순의 unbind / disconnect 다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 코어와 플랫폼 TSM 드라이버 사이에 놓인다. 실행 컨텍스트는 전부
 * 프로세스 문맥이며, 인터럽트 경로가 없다.
 *
 *   PCI 열거     : pci_device_add() (drivers/pci/probe.c)
 *                    -> pci_tsm_init()      [이 파일]
 *   PCI 소멸     : pci_destroy_dev() (drivers/pci/remove.c)
 *                    -> pci_tsm_destroy()   [이 파일]
 *   TSM 모듈     : pci_tsm_register() / pci_tsm_unregister()  [이 파일]
 *   사용자 제어  : /sys/bus/pci/devices/<BDF>/tsm/connect 쓰기
 *                    -> connect_store() -> pci_tsm_connect()  [이 파일]
 *                      -> 플랫폼 TSM 의 probe/connect 콜백
 *                      -> pci_tsm_walk_fns(probe_fn) 으로 하위 함수까지 전파
 *   VM 할당      : pci_tsm_bind() / pci_tsm_guest_req() / pci_tsm_unbind()
 *
 * 상태의 층위가 두 개라는 점이 이 파일을 읽는 열쇠다.
 *   connect - 호스트와 장치 사이에 인증 세션이 섰다. DSM 단위다.
 *   bind    - 그 장치(정확히는 한 function)를 기밀 VM 에 TDI 로 넘겼다.
 * connect 없이 bind 할 수 없고, bind 가 살아 있는 채로 disconnect 할 수 없다
 * (__pci_tsm_disconnect 이 unbind_all 을 먼저 부른다).
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/probe.c 와 drivers/pci/remove.c 가 장치 생명 주기의
 *   양 끝에서 이 파일을 부른다(두 파일에서 호출을 확인).
 *   drivers/pci/pci-sysfs.c 가 CONFIG_PCI_TSM 일 때 이 파일의 두 속성 그룹을
 *   장치 속성 배열에 넣는다(같은 파일에서 확인).
 *   선언은 drivers/pci/pci.h 에 있다(pci_tsm_init, pci_tsm_destroy,
 *   pci_tsm_attr_group, pci_tsm_auth_attr_group - 해당 파일에서 확인).
 * 아래쪽: drivers/pci/doe.c 다. pci_tsm_pf0_constructor() 가
 *   pci_find_doe_mailbox() 로 CMA 우편함을 찾아 두고,
 *   pci_tsm_doe_transfer() 가 pci_doe() 로 메시지를 주고받는다.
 *   DOE(Data Object Exchange)는 config 공간 위에 만든 우편함으로,
 *   BAR 없이도 장치와 임의 길이의 데이터를 주고받을 수 있게 해 준다.
 *   장치를 신뢰하기 전에 해야 하는 인증 대화에 그 성질이 딱 맞는다.
 * 옆쪽: drivers/pci/ide.c 는 IDE(Integrity and Data Encryption, PCIe r7.0
 *   6.33) 를 실제로 설정하는 파일이다. 이 파일은 pdev->ide_cap 이 0 인지만
 *   보고 로그 문자열을 고를 뿐, IDE 레지스터를 건드리지 않는다
 *   (link_sysfs_enable 의 한 줄이 유일한 사용처다).
 * 바깥쪽: 플랫폼 TSM 드라이버. 이 파일이 EXPORT_SYMBOL_GPL 로 내보낸
 *   생성자들과 pci_tsm_doe_transfer() 를 쓰고, 반대로 이 파일은
 *   그쪽이 채운 pci_tsm_ops 콜백을 부른다. 이 트리에는 그런 드라이버가
 *   들어 있지 않아 실제 구현을 확인할 수 없다.
 * 공유 상태: struct pci_dev 의 tsm 포인터가 이 파일과 나머지를 잇는 유일한
 *   접점이다. 그 포인터와 그것이 가리키는 구조체들(pci_tsm, pci_tsm_pf0,
 *   pci_tdi)의 정의는 이 트리에 없는 linux/pci-tsm.h 에 있어 직접 확인할 수 없다.
 *   이 파일에서 읽는 필드로부터 그 구조를 역으로 알 수 있다 -
 *   pci_tsm 은 pdev / dsm_dev / tsm_dev / tdi 를 갖고, pci_tsm_pf0 은
 *   base_tsm 을 품으면서 lock 과 doe_mb 를 더 갖는다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_tsm_register()        : 플랫폼 TSM 을 등록하고, 첫 link TSM 이면
 *                             기존 PF0 들의 tsm/ sysfs 를 드러낸다.
 * pci_tsm_connect()         : DSM 세션을 세우고 딸린 함수들에 컨텍스트를 전파한다.
 * pci_tsm_bind()            : 한 function 을 기밀 VM 의 TDI 로 넘긴다.
 * pci_tsm_guest_req()       : 게스트의 TDISP 요청을 해석하지 않고 TSM 에 전달한다.
 * pci_tsm_doe_transfer()    : DSM 의 CMA DOE 우편함으로 메시지를 주고받는다.
 *                             이 파일과 doe.c 를 잇는 유일한 지점이다.
 * pci_tsm_init()/destroy()  : PCI 열거/소멸 경로에서 컨텍스트를 붙이고 뗀다.
 * find_dsm_dev()            : 어떤 함수의 보안을 누가 관리하는지 위상에서 찾는다.
 * pci_tsm_walk_fns() 계열   : DSM 아래 PF/VF/하위 장치를 정순/역순으로 훑는다.
 * pci_tsm_rwsem             : 이 파일의 모든 전역 상태를 지키는 잠금.
 * pci_tsm_pf0 의 lock       : 한 DSM 아래 bind/unbind/disconnect 를 직렬화한다.
 *
 * === sysfs 로 보이는 것 ===
 * /sys/bus/pci/devices/<BDF>/
 *   authenticated       - 인증 여부. 내용은 connect 와 같다(authenticated_show).
 *                         빈 줄이면 아직 인증되지 않았다는 뜻이다.
 *   tsm/connect         - 읽으면 붙어 있는 TSM 이름, 쓰면 "tsmN" 으로 연결.
 *   tsm/disconnect      - 붙어 있는 TSM 이름을 정확히 써야 끊긴다.
 *   tsm/bound           - 어느 TSM 에 TDI 로 묶였는지. 안 묶였으면 빈 줄.
 *   tsm/dsm             - 이 함수를 관리하는 DSM 의 BDF.
 * 이 파일들은 조건이 맞을 때만 나타난다. link TSM 이 하나도 없으면 아예 없고,
 * connect/disconnect 는 DSM 자리(PF0)에만 보인다.
 *
 * === NVMe 관점 ===
 * 이 파일에는 NVMe 관련 코드가 없다. 장치 종류를 가리지 않는 계층이다.
 * drivers/nvme 트리를 주석을 제거한 상태로 전수 검색해도 tsm, pci_tsm,
 * tdisp, doe, pci_doe, ide, pci_ide, spdm 참조는 모두 0건이다.
 * 따라서 "NVMe 드라이버가 이 경로를 탄다" 는 서술은 이 트리의 근거로는
 * 쓸 수 없다.
 *
 * 논리적인 연결점은 있다. 기밀 VM 이 NVMe SSD 를 직접 쓰려면 그 컨트롤러가
 * TDI 로 넘어와야 하고, 그 절차가 곧 이 파일의 connect - bind 다.
 * 다만 그것을 이 트리의 코드로 확인할 수는 없다. 확인되는 것은 다음 두 가지다.
 *  - 이 파일이 요구하는 전제: TDISP 를 쓰려면 장치에 CMA DOE 우편함이
 *    있어야 한다(pci_tsm_pf0_constructor 이 없으면 -ENODEV 로 실패한다).
 *  - 그 전제가 걸리는 자리: DSM 은 PF0 이거나 스위치의 업스트림 포트다
 *    (find_dsm_dev). 즉 SR-IOV VF 를 VM 에 넘기더라도 인증 대화 자체는
 *    PF0 가 대신한다.
 * NVMe 스펙 자신의 보안 기능(예: TCG Opal)은 이 경로가 아니라 NVMe 명령으로
 * 이뤄진다는 점도 구분해 둘 만하다.
 */


/* [한국어] 이 파일에서 pci_dbg / pci_warn 등이 찍는 메시지 앞에 "PCI/TSM: "
 * 접두사를 붙인다. dev_fmt 은 include 보다 먼저 정의해야 효과가 있다 -
 * 아래 헤더들이 그 매크로를 참조해 로그 매크로를 만들기 때문이다. */
#define dev_fmt(fmt) "PCI/TSM: " fmt

#include <linux/bitfield.h>	/* [한국어] FIELD_GET/FIELD_PREP 비트필드 헬퍼를 제공한다. 다만 이 파일에서 그 매크로를 쓰는 곳은 없다(주석을 제거하고 전수 확인) */
#include <linux/pci.h>	/* [한국어] struct pci_dev 와 pci_get_slot, pci_physfn, pci_num_vf, PCI_DEVFN, pci_walk_bus 등 PCI 코어 API 전반. 이 트리에는 이 헤더가 없다 */
#include <linux/pci-doe.h>	/* [한국어] pci_find_doe_mailbox 와 pci_doe 선언. 이 파일이 doe.c 와 이어지는 통로다 */
#include <linux/pci-tsm.h>	/* [한국어] struct pci_tsm, pci_tsm_pf0, pci_tdi, pci_tsm_ops 와 is_pci_tsm_pf0, enum pci_tsm_req_scope 정의. 이 트리에 없어 구조체 정의를 직접 확인할 수 없다 */
#include <linux/sysfs.h>	/* [한국어] DEVICE_ATTR_RW/RO/WO, struct attribute_group, sysfs_emit, sysfs_update_group, sysfs_streq, SYSFS_GROUP_VISIBLE 매크로 */
#include <linux/tsm.h>	/* [한국어] struct tsm_dev 와 find_tsm_dev / put_tsm_dev. PCI 에 한정되지 않는 일반 TSM 계층의 타입이다 */
#include <linux/xarray.h>	/* [한국어] 희소 배열 자료구조. 다만 이 파일에서 xa_ 계열 함수나 struct xarray 를 쓰는 곳은 없다(전수 확인). doe.c 가 우편함을 xarray 로 관리하므로 간접적으로 관련될 뿐이다 */
#include "pci.h"	/* [한국어] drivers/pci 내부 선언. 이 파일이 정의하는 pci_tsm_attr_group / pci_tsm_auth_attr_group 과 pci_tsm_init / pci_tsm_destroy 의 선언이 여기 있다(drivers/pci/pci.h 에서 확인) */

/*
 * Provide a read/write lock against the init / exit of pdev tsm
 * capabilities and arrival/departure of a TSM instance
 */
/*
 * [한국어] 이 파일의 전역 상태 셋을 지키는 잠금이다.
 *
 * 지키는 대상은 세 가지다.
 *  1) 각 pci_dev 의 tsm 포인터가 만들어지고 사라지는 시점
 *  2) 아래 두 카운터
 *  3) 그에 따라 갱신되는 sysfs 그룹
 *
 * 읽기/쓰기를 나눈 이유가 분명하다. 쓰기를 잡는 쪽은 전역 상태를 바꾸는
 * 경로다 - connect, disconnect, register, unregister, destroy.
 * 읽기를 잡는 쪽은 개별 장치의 필드만 건드리거나 값을 보여 주는 경로다 -
 * init, bind, unbind, guest_req, 그리고 sysfs show 들.
 * 특히 connect 와 init 이 배타여야 한다는 점이 여러 곳의 영어 주석에
 * 명시돼 있고, lockdep_assert_held_write 로 강제된다.
 */
static DECLARE_RWSEM(pci_tsm_rwsem);	/* [한국어] 읽기/쓰기 세마포어를 정의하고 초기화한다. 세마포어라 잠든 채 대기할 수 있어 sysfs 경로에 적합하다 */

/*
 * Count of TSMs registered that support physical link operations vs device
 * security state management.
 */
/*
 * [한국어] 아래 두 카운터는 등록된 TSM 의 종류별 개수다.
 *
 * pci_tsm_link_count 는 sysfs 가시성 판정에 직접 쓰인다
 * (pci_tsm_link_group_visible 이 이 값이 0 이면 무조건 감춘다).
 * 반면 pci_tsm_devsec_count 는 올리고 내리기만 할 뿐, 이 파일에서 그 값을
 * 읽는 곳이 없다(주석을 제거하고 전수 확인). 어디에 쓰려던 것인지는
 * 이 트리만으로는 확인할 수 없다.
 */
static int pci_tsm_link_count;	/* [한국어] 물리 링크/세션을 다루는 TSM 의 수. 설정자: pci_tsm_register 가 증가, pci_tsm_unregister 가 감소. 읽는 자: pci_tsm_link_group_visible, pci_tsm_init, __pci_tsm_destroy. 값 범위: 0 이상. 동기화: pci_tsm_rwsem 쓰기 잠금 아래에서만 변경한다 */
static int pci_tsm_devsec_count;	/* [한국어] 장치 보안 상태를 다루는 TSM 의 수. 설정자: pci_tsm_register 와 pci_tsm_unregister. 읽는 자: 이 파일에는 없다. 값 범위: 0 이상. 동기화: 위와 같다 */

/*
 * [한국어]
 * to_pci_tsm_ops - TSM 컨텍스트에서 플랫폼 TSM 의 콜백 표를 꺼낸다
 *
 * @tsm: 대상 컨텍스트. NULL 이면 안 된다 - 검사하지 않는다.
 * @return: 그 컨텍스트를 소유한 플랫폼 TSM 이 등록한 pci_ops.
 *
 * 포인터를 두 번 따라가는 한 줄짜리 접근자다. 그럼에도 따로 함수를 둔 이유는
 * 이 경로가 이 파일 곳곳에서 반복되기 때문이다. remove, unbind, bind,
 * guest_req 를 부를 때마다 여기를 지난다.
 *
 * 값의 흐름: pci_tsm_link_constructor() 가 tsm->tsm_dev 를 채우고,
 * 그 tsm_dev->pci_ops 는 플랫폼 TSM 드라이버가 등록 시 채워 둔 것이다.
 *
 * 실행 컨텍스트: 호출자를 따른다. 잠금도 부작용도 없다.
 *
 * 호출 체인:
 *   tsm_remove() / probe_fn() / __pci_tsm_unbind() / pci_tsm_bind() /
 *   pci_tsm_guest_req() / __pci_tsm_disconnect() -> [to_pci_tsm_ops]
 */
static const struct pci_tsm_ops *to_pci_tsm_ops(struct pci_tsm *tsm)
{
	return tsm->tsm_dev->pci_ops;	/* [한국어] 컨텍스트 -> 플랫폼 TSM -> 콜백 표 순으로 따라간다 */
}

/*
 * [한국어]
 * is_dsm - 이 장치가 자기 자신의 Device Security Manager 인지 판별한다
 *
 * @pdev: 판별할 PCI 장치.
 * @return: true 면 이 장치가 DSM 이다.
 *
 * 판별 방법이 재미있다. 별도의 플래그를 두지 않고 "컨텍스트가 있고,
 * 그 컨텍스트가 가리키는 DSM 이 자기 자신인가" 를 본다.
 * pci_tsm_link_constructor() 가 find_dsm_dev() 결과를 dsm_dev 에 넣으므로,
 * 자기 자신이 DSM 인 장치만 이 조건을 만족한다.
 *
 * DSM 이라는 개념이 왜 필요한가. TDISP 에서 인증 세션은 장치 하나가 아니라
 * 한 묶음(PF0 와 그 형제 function, VF, 또는 스위치 아래 장치들)에 대해
 * 한 번 맺는다. 그 묶음을 대표해 세션을 소유하는 자리가 DSM 이다.
 *
 * 실행 컨텍스트: 호출자를 따른다. 잠금 없이 pdev->tsm 을 읽으므로,
 * 호출자가 pci_tsm_rwsem 을 잡고 있어야 안전하다.
 *
 * 호출 체인:
 *   to_pci_tsm_pf0() / pci_tsm_walk_fns() 계열 / find_dsm_dev() -> [is_dsm]
 */
static inline bool is_dsm(struct pci_dev *pdev)
{
	return pdev->tsm && pdev->tsm->dsm_dev == pdev;	/* [한국어] 컨텍스트가 있고 그 dsm_dev 가 자기 자신을 가리키면 이 장치가 DSM 이다 */
}

/*
 * [한국어]
 * has_tee - 이 장치가 PCIe Device Capabilities 의 TEE 비트를 광고하는지 본다
 *
 * @pdev: 판별할 PCI 장치.
 * @return: TEE 비트가 서 있으면 0 이 아닌 값.
 *
 * PCIe config 공간의 Device Capabilities 레지스터에 있는 비트로,
 * "이 장치는 TEE 환경에서 쓰일 수 있다" 는 표시다. pdev->devcap 은
 * PCI 코어가 열거 시점에 그 레지스터를 읽어 캐시해 둔 값이다.
 *
 * PCI_EXP_DEVCAP_TEE 의 실제 비트 위치는 이 트리에 없는
 * linux/pci_regs.h 에 있어 확인할 수 없다. devcap 을 언제 채우는지도
 * linux/pci.h 와 probe.c 쪽 사정이라 여기서는 확인 범위를 넘는다.
 *
 * 이 판정이 쓰이는 자리는 두 곳이다. pci_tsm_connect() 가 하위 함수 전파를
 * 할지 정할 때, 그리고 sysfs 에서 bound / dsm 속성을 보일지 정할 때다.
 *
 * 실행 컨텍스트: 호출자를 따른다. 캐시된 값을 읽기만 하므로 잠금이 없다.
 *
 * 호출 체인:
 *   link_sysfs_enable() / pci_tsm_connect() / pci_tsm_attr_visible()
 *     -> [has_tee]
 */
static inline bool has_tee(struct pci_dev *pdev)
{
	return pdev->devcap & PCI_EXP_DEVCAP_TEE;	/* [한국어] 캐시된 Device Capabilities 에서 TEE 비트만 마스크로 뽑아낸다 */
}

/* 'struct pci_tsm_pf0' wraps 'struct pci_tsm' when ->dsm_dev == ->pdev (self) */
/*
 * [한국어]
 * (아래 함수의 뜻을 조금 더 풀어 쓴다. 위 영어 주석이 원본이다.)
 *
 * to_pci_tsm_pf0 - 일반 TSM 컨텍스트에서 DSM 의 PF0 래퍼를 얻는다
 *
 * @tsm: 어느 함수의 TSM 컨텍스트. 자기 자신이 DSM 이 아니어도 된다.
 * @return: 그 함수를 관리하는 DSM 의 pci_tsm_pf0. 구조가 어긋나면 NULL 이며,
 *   그때는 경고도 함께 남긴다.
 *
 * 이 파일에서 잠금을 잡을 때마다 지나는 길목이다. 뮤텍스가 개별 함수가 아니라
 * DSM 에 하나씩 있기 때문에, 어떤 함수를 잠그려면 먼저 그 함수의 DSM 을
 * 찾아 그쪽 래퍼로 건너가야 한다.
 *
 * 동작은 세 단계다.
 *  1) tsm->dsm_dev 로 DSM 장치를 얻는다(모든 link 컨텍스트가 이 필드를 갖는다).
 *  2) 그 장치가 정말 PF0 자격이 있고 실제로 DSM 인지 확인한다.
 *  3) 그 장치의 tsm 포인터를 container_of 로 바깥 래퍼로 되돌린다.
 *
 * container_of 가 성립하는 근거는 위 영어 주석이 밝히는 규약이다 -
 * dsm_dev 가 자기 자신인 컨텍스트는 반드시 pci_tsm_pf0 안의 base_tsm 이다.
 * 규약이 깨지면 잘못된 주소를 계산하게 되므로, 그 전에 pci_WARN_ONCE 로
 * 한 번만 경고하고 NULL 을 돌려준다. 다만 이 파일의 호출자들은 대부분
 * 반환값을 NULL 검사 없이 바로 쓴다 - 이 코드를 그대로 읽은 사실이며,
 * 그 의도는 이 트리만으로는 확인할 수 없다.
 *
 * 실행 컨텍스트: 호출자를 따른다. 포인터만 따라가므로 잠금이 없지만,
 * 호출자가 pci_tsm_rwsem 을 잡고 있어야 dsm_dev 의 tsm 이 사라지지 않는다.
 *
 * 호출 체인:
 *   pci_tsm_connect() / __pci_tsm_unbind() / pci_tsm_bind() /
 *   pci_tsm_guest_req() / bound_show() / __pci_tsm_disconnect() /
 *   pci_tsm_doe_transfer() -> [to_pci_tsm_pf0]
 */
static struct pci_tsm_pf0 *to_pci_tsm_pf0(struct pci_tsm *tsm)
{
	/*
	 * All "link" TSM contexts reference the device that hosts the DSM
	 * interface for a set of devices. Walk to the DSM device and cast its
	 * ->tsm context to a 'struct pci_tsm_pf0 *'.
	 */
	struct pci_dev *pf0 = tsm->dsm_dev;	/* [한국어] 이 컨텍스트를 관리하는 DSM 장치. 자기 자신일 수도 있다 */

	if (!is_pci_tsm_pf0(pf0) || !is_dsm(pf0)) {	/* [한국어] PF0 자격이 있고 실제로 DSM 으로 세워져 있어야 아래 변환이 성립한다 */
		pci_WARN_ONCE(tsm->pdev, 1, "invalid context object\n");	/* [한국어] 구조 규약이 깨진 것은 드라이버 버그이므로 한 번만 경고한다. 두 번째 인자 1 이 조건이다 */
		return NULL;	/* [한국어] 잘못된 주소를 계산하느니 NULL 을 돌려준다 */
	}

	return container_of(pf0->tsm, struct pci_tsm_pf0, base_tsm);	/* [한국어] base_tsm 멤버의 주소에서 바깥 pci_tsm_pf0 의 주소를 역산한다. 위 영어 주석이 밝히는 규약이 이 계산의 근거다 */
}

/*
 * [한국어]
 * tsm_remove - 한 함수의 TSM 컨텍스트를 플랫폼 TSM 에 돌려주고 연결을 끊는다
 *
 * @tsm: 제거할 컨텍스트. NULL 이면 아무 일도 하지 않는다.
 * @return: 없음.
 *
 * NULL 을 허용하는 것이 중요하다. 아래 DEFINE_FREE 와 여러 호출자가
 * "있으면 지우고 없으면 넘어간다" 식으로 부르기 때문이다.
 *
 * 순서에 주의. 콜백을 먼저 부르고 그다음 pdev->tsm 을 NULL 로 만든다.
 * 반대로 하면 콜백이 자기 컨텍스트를 되찾을 수 없다. 그래서 pdev 를
 * 미리 지역 변수에 담아 둔다 - 콜백이 tsm 을 해제해 버리면 tsm->pdev 를
 * 더 읽을 수 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 pci_tsm_rwsem 을 잡은 상태다.
 * 콜백이 잠들 수 있다.
 *
 * 호출 체인:
 *   remove_fn() / pci_tsm_disconnect() / pci_tsm_fn_exit() /
 *   __free(tsm_remove) 정리 경로 -> [tsm_remove]
 *     -> 플랫폼 TSM 의 remove 콜백
 */
static void tsm_remove(struct pci_tsm *tsm)
{
	struct pci_dev *pdev;	/* [한국어] 콜백 호출 뒤에도 쓸 수 있도록 미리 담아 둘 장치 포인터 */

	if (!tsm)	/* [한국어] NULL 을 그대로 받아들인다. 자동 정리 경로가 이 성질에 기댄다 */
		return;	/* [한국어] 할 일이 없다 */

	pdev = tsm->pdev;	/* [한국어] 콜백이 tsm 을 해제할 수 있으므로 그 전에 장치 포인터를 확보한다 */
	to_pci_tsm_ops(tsm)->remove(tsm);	/* [한국어] 플랫폼 TSM 이 자기 컨텍스트를 정리한다. 여기서 tsm 이 해제될 수 있다 */
	pdev->tsm = NULL;	/* [한국어] 장치와의 연결을 끊는다. 이후 이 함수는 TSM 이 붙지 않은 상태가 된다 */
}
/*
 * [한국어] 위 tsm_remove 를 __free(tsm_remove) 로 쓸 수 있게 등록한다.
 *
 * 세 번째 인자가 정리 동작이고 _T 가 그 변수다. 스코프를 벗어날 때
 * NULL 이 아니면 tsm_remove 를 부른다. pci_tsm_connect() 가 이 장치를 써서
 * 여러 갈래의 실패 경로마다 goto 를 두지 않고도 컨텍스트를 되돌린다.
 * 성공 경로에서는 no_free_ptr() 로 이 자동 정리를 취소한다.
 */
DEFINE_FREE(tsm_remove, struct pci_tsm *, if (_T) tsm_remove(_T))	/* [한국어] 정리 동작 자체가 NULL 검사를 한 번 더 하므로, tsm_remove 의 NULL 허용과 이중으로 안전하다 */

/*
 * [한국어]
 * pci_tsm_walk_fns - DSM 아래 딸린 모든 PCI 함수에 콜백을 적용한다
 *
 * @pdev: 기준이 되는 DSM 장치(보통 PF0).
 * @cb: 각 함수에 적용할 콜백. pci_walk_bus() 와 같은 시그니처라
 *   그대로 넘겨 쓸 수 있다.
 * @data: 콜백에 전달할 인자. 현재 유일한 사용처는 probe_fn 에 DSM 을 넘기는 것이다.
 * @return: 없음. 콜백의 반환값도 무시한다 - 개별 함수의 실패가 전체를
 *   막지 않는다는 정책 때문이다.
 *
 * "DSM 아래" 가 무엇인지가 이 함수의 정의다. 세 부류를 훑는다.
 *  1) 같은 슬롯의 형제 physical function - function 1 부터 7 까지.
 *     function 0 은 건너뛴다(호출자가 이미 처리했다고 위 영어 주석이 밝힌다).
 *  2) 각 PF 의 virtual function - SR-IOV 로 만들어진 VF 들.
 *  3) 이 장치가 스위치의 업스트림 포트이면서 DSM 이면, 그 아래 버스 전체.
 *
 * 왜 8까지인가. PCI 의 devfn 은 하위 3비트가 function 번호라
 * 한 device 에 최대 8개의 function 이 있을 수 있다. 그래서 0..7 을 훑는다.
 * PCI_DEVFN(slot, i) 로 같은 slot 의 i 번 function 의 devfn 을 만든다.
 *
 * VF 의 BDF 는 계산해야 한다. VF 는 PF 와 다른 버스에 있을 수 있고
 * (ARI 와 SR-IOV 의 First VF Offset / VF Stride 때문에),
 * pci_iov_virtfn_bus() 와 pci_iov_virtfn_devfn() 이 그 계산을 대신해 준다.
 *
 * 참조 관리는 __free(pci_dev_put) 이 맡는다. pci_get_slot() 과
 * pci_get_domain_bus_and_slot() 이 올린 참조가 각 루프 반복의 끝에서
 * 자동으로 내려간다 - C 의 스코프 규칙을 이용한 정리다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 장치 목록을 훑고 콜백이 잠들 수 있다.
 * 호출자가 pci_tsm_rwsem 을 쓰기로 잡은 상태에서 불린다.
 *
 * 호출 체인:
 *   pci_tsm_connect() -> [pci_tsm_walk_fns] -> probe_fn() 등
 */
static void pci_tsm_walk_fns(struct pci_dev *pdev,
			     int (*cb)(struct pci_dev *pdev, void *data),
			     void *data)
{
	/* Walk subordinate physical functions */
	for (int i = 0; i < 8; i++) {	/* [한국어] 한 device 의 function 은 최대 8개다. devfn 의 하위 3비트가 function 번호이기 때문이다 */
		struct pci_dev *pf __free(pci_dev_put) = pci_get_slot(	/* [한국어] 같은 슬롯의 i 번 function 을 찾는다. 참조가 올라가고, 이 반복의 끝에서 자동으로 내려간다 */
			pdev->bus, PCI_DEVFN(PCI_SLOT(pdev->devfn), i));	/* [한국어] PCI_SLOT 으로 상위 5비트(device 번호)를 뽑고 PCI_DEVFN 으로 i 번 function 의 devfn 을 조립한다 */

		if (!pf)	/* [한국어] 그 function 이 존재하지 않으면 */
			continue;	/* [한국어] 다음 번호로 넘어간다 */

		/* on entry function 0 has already run @cb */
		if (i > 0)	/* [한국어] function 0 은 호출자가 이미 처리했다. 위 영어 주석이 그 규약을 밝힌다 */
			cb(pf, data);	/* [한국어] 이 PF 에 콜백을 적용한다. 반환값은 무시한다 */

		/* walk virtual functions of each pf */
		for (int j = 0; j < pci_num_vf(pf); j++) {	/* [한국어] 이 PF 가 SR-IOV 로 만든 VF 개수만큼 반복한다. VF 가 없으면 0 이라 루프가 돌지 않는다 */
			struct pci_dev *vf __free(pci_dev_put) =	/* [한국어] j 번 VF 를 찾는다. 역시 반복 끝에서 참조가 자동으로 내려간다 */
				pci_get_domain_bus_and_slot(
					pci_domain_nr(pf->bus),	/* [한국어] VF 는 PF 와 같은 도메인에 있다 */
					pci_iov_virtfn_bus(pf, j),	/* [한국어] VF 의 버스 번호를 계산한다. SR-IOV 의 offset/stride 때문에 PF 와 다른 버스일 수 있다 */
					pci_iov_virtfn_devfn(pf, j));	/* [한국어] VF 의 devfn 을 계산한다 */

			if (!vf)	/* [한국어] 아직 열거되지 않았거나 사라진 VF */
				continue;	/* [한국어] 다음 VF 로 넘어간다 */

			cb(vf, data);	/* [한국어] 이 VF 에 콜백을 적용한다 */
		}
	}

	/*
	 * Walk downstream devices, assumes that an upstream DSM is
	 * limited to downstream physical functions
	 */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_UPSTREAM && is_dsm(pdev))	/* [한국어] 이 장치가 PCIe 업스트림 포트이면서 DSM 일 때만. 스위치가 아래 장치들을 대신해 TDISP 를 제공하는 구성이다 */
		pci_walk_bus(pdev->subordinate, cb, data);	/* [한국어] 그 아래 버스 전체를 PCI 코어의 순회 함수로 훑는다. 위 영어 주석이 밝히듯 하위는 physical function 만 있다고 가정한다 */
}

/*
 * [한국어]
 * pci_tsm_walk_fns_reverse - pci_tsm_walk_fns 와 정확히 반대 순서로 훑는다
 *
 * @pdev: 기준이 되는 DSM 장치.
 * @cb: 각 함수에 적용할 콜백.
 * @data: 콜백에 전달할 인자. 현재 호출자들은 모두 NULL 을 넘긴다.
 * @return: 없음.
 *
 * 왜 역순이 따로 필요한가. 만들 때와 없앨 때의 순서가 반대여야 하기 때문이다.
 * 만들 때는 위에서 아래로 - DSM 이 먼저 서고 그 아래 함수들이 붙는다.
 * 없앨 때는 아래에서 위로 - 딸린 것들이 먼저 사라지고 DSM 이 마지막이다.
 * 순서를 지키지 않으면 아직 살아 있는 하위 컨텍스트가 이미 사라진 상위를
 * 참조하게 된다.
 *
 * 그래서 세 부분이 모두 뒤집혀 있다.
 *  1) 하위 스위치 아래 장치들을 먼저(정순에서는 마지막이었다).
 *  2) function 을 7 에서 0 방향으로.
 *  3) VF 를 마지막 인덱스부터.
 * 그리고 정순과 마찬가지로 function 0 은 호출자에게 남긴다
 * (위 영어 주석의 "on exit, caller will run @cb on function 0").
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 pci_tsm_rwsem 을 쓰기로 잡고 있다.
 * __pci_tsm_disconnect() 경로에서는 DSM 뮤텍스도 잡힌 상태일 수 있는데,
 * 그때 넘어오는 콜백은 remove_fn 으로 뮤텍스를 다시 잡지 않는다.
 * 반면 __pci_tsm_unbind 를 콜백으로 넘기는 pci_tsm_unbind_all() 은
 * 뮤텍스를 잡지 않은 상태에서 부른다 - 콜백이 스스로 잡기 때문이다.
 *
 * 호출 체인:
 *   pci_tsm_unbind_all() -> [pci_tsm_walk_fns_reverse] -> __pci_tsm_unbind()
 *   __pci_tsm_disconnect() -> [pci_tsm_walk_fns_reverse] -> remove_fn()
 */
static void pci_tsm_walk_fns_reverse(struct pci_dev *pdev,
				     int (*cb)(struct pci_dev *pdev,
					       void *data),
				     void *data)
{
	/* Reverse walk downstream devices */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_UPSTREAM && is_dsm(pdev))	/* [한국어] 정순에서는 마지막이던 하위 버스 순회를 여기서는 먼저 한다 */
		pci_walk_bus_reverse(pdev->subordinate, cb, data);	/* [한국어] PCI 코어의 역순 순회 함수를 쓴다. 정의는 drivers/pci/bus.c 에 있다 */

	/* Reverse walk subordinate physical functions */
	for (int i = 7; i >= 0; i--) {	/* [한국어] function 번호를 7 에서 0 방향으로 훑는다 */
		struct pci_dev *pf __free(pci_dev_put) = pci_get_slot(	/* [한국어] 같은 슬롯의 i 번 function 을 찾는다. 반복 끝에서 참조가 자동으로 내려간다 */
			pdev->bus, PCI_DEVFN(PCI_SLOT(pdev->devfn), i));	/* [한국어] 정순과 같은 방식으로 devfn 을 조립한다 */

		if (!pf)	/* [한국어] 존재하지 않는 function */
			continue;	/* [한국어] 건너뛴다 */

		/* reverse walk virtual functions */
		for (int j = pci_num_vf(pf) - 1; j >= 0; j--) {	/* [한국어] VF 도 마지막 인덱스부터 거꾸로 훑는다 */
			struct pci_dev *vf __free(pci_dev_put) =	/* [한국어] j 번 VF 를 찾는다 */
				pci_get_domain_bus_and_slot(
					pci_domain_nr(pf->bus),	/* [한국어] VF 는 PF 와 같은 도메인에 있다 */
					pci_iov_virtfn_bus(pf, j),	/* [한국어] VF 의 버스 번호를 계산한다 */
					pci_iov_virtfn_devfn(pf, j));	/* [한국어] VF 의 devfn 을 계산한다 */

			if (!vf)	/* [한국어] 존재하지 않는 VF */
				continue;	/* [한국어] 건너뛴다 */
			cb(vf, data);	/* [한국어] 이 VF 에 콜백을 적용한다. VF 를 PF 보다 먼저 처리하는 것이 역순의 요지다 */
		}

		/* on exit, caller will run @cb on function 0 */
		if (i > 0)	/* [한국어] function 0 은 호출자가 마지막에 처리한다. 위 영어 주석이 그 규약을 밝힌다 */
			cb(pf, data);	/* [한국어] function 1 이상에만 콜백을 적용한다 */
	}
}

/*
 * [한국어]
 * link_sysfs_disable - 이 장치의 TSM 관련 sysfs 속성을 다시 계산해 감춘다
 *
 * @pdev: 대상 PCI 장치.
 * @return: 없음.
 *
 * sysfs_update_group() 은 그룹의 is_visible 콜백을 다시 돌려 파일을
 * 만들거나 지운다. 즉 이 함수 자체가 "감춘다" 고 결정하는 것이 아니라,
 * 판정 함수(pci_tsm_link_group_visible)가 이미 false 를 돌려줄 상태가 된
 * 뒤에 불려야 실제로 감춰진다.
 *
 * 그래서 호출자들은 항상 상태를 먼저 바꾼다. remove_fn() 은
 * tsm_remove() 로 pdev->tsm 을 NULL 로 만든 뒤에 부르고,
 * __pci_tsm_destroy() 는 pci_tsm_link_count 가 0 이 된 뒤에 부른다.
 *
 * 순서에도 뜻이 있다. authenticated 를 먼저 내리고 tsm/ 을 나중에 내린다
 * (link_sysfs_enable 은 같은 순서로 올린다).
 *
 * 실행 컨텍스트: 프로세스 문맥(sysfs 갱신). 잠들 수 있다.
 * 호출자가 pci_tsm_rwsem 을 잡고 있는데, 갱신 중 불리는 판정 함수는
 * 그 rwsem 을 다시 잡지 않는다.
 *
 * 호출 체인:
 *   remove_fn() / __pci_tsm_destroy() -> [link_sysfs_disable]
 *     -> sysfs_update_group()
 */
static void link_sysfs_disable(struct pci_dev *pdev)
{
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_auth_attr_group);	/* [한국어] authenticated 그룹을 다시 계산한다. 조건이 어긋나면 파일이 사라진다 */
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_attr_group);	/* [한국어] tsm/ 그룹을 다시 계산한다. 그 안의 개별 파일은 pci_tsm_attr_visible 이 또 걸러 낸다 */
}

/*
 * [한국어]
 * link_sysfs_enable - 이 장치의 TSM 관련 sysfs 속성을 다시 계산해 드러낸다
 *
 * @pdev: 대상 PCI 장치.
 * @return: 없음.
 *
 * disable 의 대칭이다. 역시 판정 함수가 true 를 돌려줄 상태가 된 뒤에
 * 불려야 한다 - probe_fn() 은 pdev->tsm 을 채운 뒤에 부르고,
 * pci_tsm_register() 는 pci_tsm_link_count 를 올린 뒤에 부른다.
 *
 * 로그 한 줄이 사람에게 유용한 정보를 준다.
 *   "Device Security Manager"  - 이미 컨텍스트가 붙어 있는 경우
 *   "Platform TEE Security Manager" - 아직 붙지 않은 경우
 *   괄호 안의 IDE / TEE - 이 장치가 광고하는 능력
 * IDE 는 Integrity and Data Encryption, 즉 PCIe 링크 자체를 암호화하고
 * 무결성을 검증하는 기능이다(PCIe r7.0 6.33). 이 파일은 pdev->ide_cap 이
 * 0 인지만 보고 로그에 쓸 뿐, IDE 를 설정하지는 않는다 - 그 일은
 * drivers/pci/ide.c 가 한다. TEE 는 PCI_EXP_DEVCAP_TEE 비트로,
 * 이 장치가 TDISP 대상이 될 수 있다는 표시다.
 *
 * 실행 컨텍스트: 프로세스 문맥(sysfs 갱신). 잠들 수 있다.
 *
 * 호출 체인:
 *   probe_fn() / pci_tsm_register() -> [link_sysfs_enable]
 *     -> has_tee() -> sysfs_update_group()
 */
static void link_sysfs_enable(struct pci_dev *pdev)
{
	bool tee = has_tee(pdev);	/* [한국어] TEE capability 여부를 한 번만 읽어 둔다. 아래 형식 문자열에서 두 번 쓰기 때문이다 */

	pci_dbg(pdev, "%s Security Manager detected (%s%s%s)\n",	/* [한국어] 어떤 보안 관리자가 감지됐는지 디버그 로그로 남긴다 */
		pdev->tsm ? "Device" : "Platform TEE",	/* [한국어] 컨텍스트가 이미 있으면 장치 쪽 관리자, 없으면 플랫폼 쪽 관리자로 표시한다 */
		pdev->ide_cap ? "IDE" : "", pdev->ide_cap && tee ? " " : "",	/* [한국어] IDE capability 유무와, IDE 와 TEE 가 모두 있을 때의 구분용 공백을 넣는다 */
		tee ? "TEE" : "");	/* [한국어] TEE capability 유무를 표시하고 형식 문자열을 닫는다 */

	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_auth_attr_group);	/* [한국어] authenticated 그룹을 다시 계산한다. 조건이 맞으면 파일이 생긴다 */
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_attr_group);	/* [한국어] tsm/ 그룹을 다시 계산한다 */
}

/*
 * [한국어]
 * probe_fn - 순회 콜백. 함수 하나에 TSM 컨텍스트를 붙이고 sysfs 를 연다
 *
 * @pdev: 컨텍스트를 붙일 PCI 함수.
 * @dsm: 이 함수를 관리할 DSM 의 pci_dev. void * 인 이유는 순회 콜백
 *   시그니처를 맞추기 위해서다.
 * @return: 항상 0. 위 pci_tsm_connect() 의 영어 주석이 밝히듯 개별 함수의
 *   probe 실패는 connect 전체를 실패시키지 않는다. 그 함수만 이후 보안
 *   연산에서 빠질 뿐이다. 순회를 멈추지 않기 위해서도 0 이어야 한다.
 *
 * DSM 의 콜백 표를 빌려 쓰는 구조에 주목할 만하다. 이 함수는 자기만의
 * TSM 을 고르지 않고, DSM 이 이미 붙어 있는 플랫폼 TSM 의 probe 를 그대로
 * 부른다. TDISP 의 신뢰 관계가 DSM 을 정점으로 하는 나무 모양이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 두 갈래의 호출자가 있는데 잠금 상태가 다르다.
 *   pci_tsm_connect() 경로 - 쓰기 rwsem 과 DSM 뮤텍스를 잡은 상태.
 *   pci_tsm_init() 경로    - 읽기 rwsem 만 잡은 상태.
 *
 * 호출 체인:
 *   pci_tsm_connect() -> pci_tsm_walk_fns() -> [probe_fn]
 *   pci_tsm_init() -> [probe_fn]
 *     -> 플랫폼 TSM 의 probe 콜백 -> link_sysfs_enable()
 */
static int probe_fn(struct pci_dev *pdev, void *dsm)
{
	struct pci_dev *dsm_dev = dsm;	/* [한국어] void 포인터로 받은 DSM 을 원래 타입으로 되돌린다 */
	const struct pci_tsm_ops *ops = to_pci_tsm_ops(dsm_dev->tsm);	/* [한국어] DSM 에 붙어 있는 플랫폼 TSM 의 콜백 표를 빌린다 */

	pdev->tsm = ops->probe(dsm_dev->tsm->tsm_dev, pdev);	/* [한국어] 플랫폼 TSM 에게 이 함수용 컨텍스트를 만들게 한다. 실패하면 NULL 이 돌아온다 */
	pci_dbg(pdev, "setup TSM context: DSM: %s status: %s\n",
		pci_name(dsm_dev), pdev->tsm ? "success" : "failed");	/* [한국어] 어느 DSM 아래에서 성공했는지 디버그 로그로 남긴다 */
	if (pdev->tsm)	/* [한국어] 컨텍스트가 만들어졌을 때만 */
		link_sysfs_enable(pdev);	/* [한국어] 이 함수의 sysfs 를 연다. 판정 함수가 pdev->tsm 을 보므로 대입 뒤에 불러야 한다 */
	return 0;	/* [한국어] 실패해도 0 - 순회를 멈추지 않고, connect 전체도 실패시키지 않는다 */
}

/*
 * [한국어]
 * pci_tsm_connect - DSM 을 세우고 그 아래 함수들까지 컨텍스트를 붙인다
 *
 * @pdev: DSM 이 될 PCI 함수(PF0 또는 스위치 업스트림 포트).
 * @tsm_dev: 연결할 플랫폼 TSM. 호출자가 참조를 잡고 있다.
 * @return: 0 이면 성공. -ENXIO 는 플랫폼 TSM 이 이 함수를 거부한 경우,
 *   그 밖의 음수는 뮤텍스 획득 실패나 connect 콜백의 오류다.
 *
 * 세 단계로 나뉜다.
 *  1) ops->probe() 로 컨텍스트를 만든다. 이 시점의 pci_tsm 은
 *     __free(tsm_remove) 로 보호되어, 아래 어느 지점에서 return 해도
 *     자동으로 되돌려진다.
 *  2) ops->connect() 로 실제 TDISP 세션을 맺는다. 성공하면
 *     no_free_ptr() 로 자동 해제를 취소해 소유권을 pdev->tsm 에 넘긴다.
 *  3) 세션이 섰으니 딸린 함수들을 훑어 컨텍스트를 붙인다.
 *
 * 정리 방식이 흥미롭다. pdev->tsm = pci_tsm 을 2단계 전에 미리 해 두는데,
 * to_pci_tsm_pf0() 이 pdev->tsm 을 읽어야 하기 때문이다. 그래서 실패해서
 * __free 가 tsm_remove() 를 부르면 그 안에서 pdev->tsm 을 NULL 로 되돌린다.
 * 즉 미리 심어 둔 포인터가 실패 경로에서 정확히 회수된다.
 *
 * 3단계를 has_tee(pdev) 로 감싸는 이유와, 그 안에서는 하위 함수의
 * TEE 비트를 따지지 않는 이유를 위 영어 주석이 길게 설명한다.
 * 요지는 "DSM 이 관리할 수 있다고 하면 맡긴다" 는 것이다.
 *
 * 뮤텍스가 mutex_intr 인 이유도 주석에 있다. 이 경로는 항상 sysfs 를 통한
 * 사용자 요청이므로, 기다리는 도중 시그널로 빠져나올 수 있어야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 쓰기 rwsem 을 잡은 상태여야 하며
 * (아래 lockdep_assert_held_write 가 확인한다), 그 이유는 위 영어 주석대로
 * pci_tsm_init() 과 배타여야 하기 때문이다.
 *
 * 호출 체인:
 *   connect_store() -> [pci_tsm_connect]
 *     -> ops->probe() -> ops->connect() -> pci_tsm_walk_fns(probe_fn)
 */
static int pci_tsm_connect(struct pci_dev *pdev, struct tsm_dev *tsm_dev)
{
	int rc;	/* [한국어] 반환값과 잠금 결과 */
	struct pci_tsm_pf0 *tsm_pf0;	/* [한국어] DSM 뮤텍스를 들고 있는 PF0 래퍼 */
	const struct pci_tsm_ops *ops = tsm_dev->pci_ops;	/* [한국어] 플랫폼 TSM 의 콜백 표 */
	struct pci_tsm *pci_tsm __free(tsm_remove) = ops->probe(tsm_dev, pdev);	/* [한국어] 컨텍스트를 만든다. 실패 경로에서 자동으로 tsm_remove 가 불리도록 __free 로 묶어 둔다 */

	/* connect() mutually exclusive with subfunction pci_tsm_init() */
	lockdep_assert_held_write(&pci_tsm_rwsem);	/* [한국어] 쓰기 잠금 보유를 강제한다. 위 영어 주석이 그 이유를 밝힌다 */

	if (!pci_tsm)	/* [한국어] 플랫폼 TSM 이 이 함수를 맡을 수 없다고 판단한 경우 */
		return -ENXIO;	/* [한국어] -ENXIO. 아직 pdev->tsm 에 심지 않았으므로 되돌릴 것도 없다 */

	pdev->tsm = pci_tsm;	/* [한국어] to_pci_tsm_pf0 이 pdev->tsm 을 읽으므로 미리 심어 둔다. 실패하면 __free 가 tsm_remove 로 되돌린다 */
	tsm_pf0 = to_pci_tsm_pf0(pdev->tsm);	/* [한국어] DSM 래퍼를 얻는다. 이 안에 아래에서 잡을 뮤텍스가 있다 */

	/* mutex_intr assumes connect() is always sysfs/user driven */
	ACQUIRE(mutex_intr, lock)(&tsm_pf0->lock);	/* [한국어] DSM 뮤텍스를 인터럽트 가능하게 잡는다. 스코프 종료 시 자동 해제된다 */
	if ((rc = ACQUIRE_ERR(mutex_intr, &lock)))	/* [한국어] 시그널로 중단됐는지 확인한다 */
		return rc;	/* [한국어] 중단됐으면 errno 를 돌려준다. __free 가 컨텍스트를 되돌린다 */

	rc = ops->connect(pdev);	/* [한국어] 플랫폼 TSM 이 실제 TDISP 세션을 맺는다. 장치 인증과 링크 준비가 여기서 이뤄진다 */
	if (rc)	/* [한국어] 세션 수립 실패 */
		return rc;	/* [한국어] 오류를 그대로 돌려준다. __free 가 컨텍스트를 되돌린다 */

	pdev->tsm = no_free_ptr(pci_tsm);	/* [한국어] 여기서부터 성공이 확정된다. no_free_ptr 이 자동 해제를 취소해 소유권을 pdev->tsm 에 넘긴다 */

	/*
	 * Now that the DSM is established, probe() all the potential
	 * dependent functions. Failure to probe a function is not fatal
	 * to connect(), it just disables subsequent security operations
	 * for that function.
	 *
	 * Note this is done unconditionally, without regard to finding
	 * PCI_EXP_DEVCAP_TEE on the dependent function, for robustness. The DSM
	 * is the ultimate arbiter of security state relative to a given
	 * interface id, and if it says it can manage TDISP state of a function,
	 * let it.
	 */
	if (has_tee(pdev))	/* [한국어] DSM 자신이 TEE 를 광고할 때만 딸린 함수들을 훑는다 */
		pci_tsm_walk_fns(pdev, probe_fn, pdev);	/* [한국어] 형제 PF, 각 PF 의 VF, 그리고 업스트림 포트라면 그 아래 장치까지 probe_fn 을 적용한다 */
	return 0;	/* [한국어] 연결 성공 */
}

/*
 * [한국어]
 * connect_show - tsm/connect 를 읽으면 현재 연결된 TSM 이름을 보여 준다
 *
 * @dev: sysfs 가 넘겨준 device.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 쓴 바이트 수, 또는 잠금이 시그널로 중단되면 음수 errno.
 *
 * 연결되지 않았으면 빈 줄, 연결됐으면 "tsm0" 같은 이름이 나온다.
 * 그 이름은 connect_store 가 받아들이는 형식과 같아서, 사용자는
 * 읽은 값을 그대로 disconnect 에 되쓸 수 있다.
 *
 * authenticated_show() 가 이 함수를 그대로 위임 호출한다. TSM 을 통해
 * SPDM 세션이 맺어지므로 "연결됨" 과 "인증됨" 이 같은 뜻이기 때문이다.
 *
 * 잠금은 읽기 rwsem 하나뿐이다. tdi 를 보지 않으므로 DSM 뮤텍스가 필요 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥(sysfs read). 잠들 수 있다.
 *
 * 호출 체인:
 *   sysfs read -> [connect_show]
 *   authenticated_show() -> [connect_show]
 */
static ssize_t connect_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] sysfs device 를 PCI 장치로 되돌린다 */
	struct tsm_dev *tsm_dev;	/* [한국어] 현재 붙어 있는 플랫폼 TSM */
	int rc;	/* [한국어] 잠금 획득 결과 */

	ACQUIRE(rwsem_read_intr, lock)(&pci_tsm_rwsem);	/* [한국어] 읽기 잠금을 인터럽트 가능하게 잡는다. cat 도중 Ctrl-C 로 빠져나올 수 있어야 한다 */
	if ((rc = ACQUIRE_ERR(rwsem_read_intr, &lock)))	/* [한국어] 시그널로 중단됐는지 확인한다 */
		return rc;	/* [한국어] 중단됐으면 그 errno 를 돌려준다 */

	if (!pdev->tsm)	/* [한국어] 컨텍스트가 없으면 연결되지 않은 상태다 */
		return sysfs_emit(buf, "\n");	/* [한국어] 빈 줄을 출력한다. 파싱하기 쉬운 형태다 */

	tsm_dev = pdev->tsm->tsm_dev;	/* [한국어] 붙어 있는 플랫폼 TSM 을 꺼낸다 */
	return sysfs_emit(buf, "%s\n", dev_name(&tsm_dev->dev));	/* [한국어] 그 TSM 장치의 이름을 출력한다. connect_store 가 받는 형식과 같다 */
}

/* Is @tsm_dev managing physical link / session properties... */
/*
 * [한국어]
 * (아래 함수의 뜻을 조금 더 풀어 쓴다. 위 영어 주석이 원본이다.)
 *
 * is_link_tsm - 이 TSM 이 물리 링크와 세션을 다루는 종류인지 판별한다
 *
 * @tsm_dev: 판별할 플랫폼 TSM. NULL 이어도 안전하다.
 * @return: true 면 link TSM.
 *
 * 판별 기준은 link_ops.probe 콜백의 존재다. 별도의 종류 필드를 두지 않고
 * "구현한 콜백이 무엇인가" 로 종류를 정한다. 그래서 pci_tsm_register() 가
 * link_ops 와 devsec_ops 를 동시에 구현한 TSM 을 거부한다 - 그러면
 * 이 판별이 모호해지기 때문이다.
 *
 * link TSM 만 connect/bind/guest_req 경로를 탈 수 있다. 이 파일 곳곳에서
 * 그 경로 진입 전에 이 함수로 걸러 낸다.
 *
 * 실행 컨텍스트: 호출자를 따른다. 포인터 세 개를 따라가기만 하므로
 * 잠금도 부작용도 없다.
 *
 * 호출 체인:
 *   connect_store() / pci_tsm_bind() / pci_tsm_guest_req() /
 *   pci_tsm_register() / pci_tsm_unregister() / __pci_tsm_destroy() /
 *   pci_tsm_link_constructor() -> [is_link_tsm]
 */
static bool is_link_tsm(struct tsm_dev *tsm_dev)
{
	return tsm_dev && tsm_dev->pci_ops && tsm_dev->pci_ops->link_ops.probe;	/* [한국어] NULL 방어를 앞에 두고 차례로 따라간다. link_ops.probe 가 있으면 link TSM 이다 */
}

/* ...or is @tsm_dev managing device security state ? */
/*
 * [한국어]
 * (아래 함수의 뜻을 조금 더 풀어 쓴다. 위 영어 주석이 원본이다.)
 *
 * is_devsec_tsm - 이 TSM 이 장치 보안 상태(lock)를 다루는 종류인지 판별한다
 *
 * @tsm_dev: 판별할 플랫폼 TSM. NULL 이어도 안전하다.
 * @return: true 면 devsec TSM.
 *
 * link TSM 과 대칭이며 기준은 devsec_ops.lock 콜백의 존재다.
 * 이 파일에서 devsec 경로가 실제로 하는 일은 카운터를 올리고 내리는 것뿐이고
 * (pci_tsm_register / pci_tsm_unregister), 그 카운터를 읽는 곳은 없다.
 * devsec_ops.lock 을 호출하는 코드도 이 파일에는 없다. 즉 여기서는
 * "그런 종류가 있다" 는 사실만 기록되고, 실제 동작은 이 트리 밖에 있다.
 *
 * 실행 컨텍스트: 호출자를 따른다. 잠금도 부작용도 없다.
 *
 * 호출 체인:
 *   pci_tsm_register() / pci_tsm_unregister() -> [is_devsec_tsm]
 */
static bool is_devsec_tsm(struct tsm_dev *tsm_dev)
{
	return tsm_dev && tsm_dev->pci_ops && tsm_dev->pci_ops->devsec_ops.lock;	/* [한국어] NULL 방어 후 devsec_ops.lock 의 존재로 판별한다 */
}

/*
 * [한국어]
 * connect_store - tsm/connect 에 "tsmN" 을 쓰면 그 TSM 과 세션을 맺는다
 *
 * @dev: sysfs 가 넘겨준 device. DSM 자리(PF0)여야 한다 - 다른 함수에는
 *   애초에 이 파일이 보이지 않는다(pci_tsm_attr_visible 참고).
 * @attr: 쓰지 않는다.
 * @buf: 사용자가 쓴 문자열. "tsm0" 처럼 TSM 의 인덱스를 담는다.
 * @len: 그 길이.
 * @return: 성공이면 len. -EINVAL 은 형식 오류, -EBUSY 는 이미 연결된 경우,
 *   -ENXIO 는 그 id 의 link TSM 이 없는 경우, 그 밖의 음수는 잠금 실패나
 *   pci_tsm_connect() 의 오류다.
 *
 * 이것이 TDISP 를 시작시키는 사용자 진입점이다. 여기서 세션이 맺어져야
 * 그 아래 함수들이 컨텍스트를 얻고(pci_tsm_walk_fns), 그 다음에야
 * TDI 바인딩이 가능해진다.
 *
 * 잠금이 rwsem_write_kill 인 점이 중요하다. 쓰기여야 하는 이유는
 * pci_tsm_connect() 의 lockdep_assert_held_write 가 요구하기 때문이고,
 * 그 요구의 근거는 이 경로가 pci_tsm_init() 과 배타여야 한다는 것이다.
 * kill 판인 이유는 세션 수립 도중 일반 시그널로 중단되면 어정쩡한 상태가
 * 남기 때문이다.
 *
 * tsm_dev 참조는 __free(put_tsm_dev) 로 자동 해제된다. find_tsm_dev() 가
 * 참조를 올려 주고, 성공하든 실패하든 이 함수를 벗어날 때 내려간다.
 * 즉 연결이 성공해도 여기서 잡은 참조는 놓는다 - 이후의 수명 관리는
 * pdev->tsm 안의 tsm_dev 포인터를 통해 이뤄진다.
 *
 * 실행 컨텍스트: 프로세스 문맥(sysfs write). 잠들 수 있다.
 *
 * 호출 체인:
 *   sysfs write -> [connect_store] -> find_tsm_dev() -> pci_tsm_connect()
 */
static ssize_t connect_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t len)
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] sysfs device 를 PCI 장치로 되돌린다 */
	int rc, id;	/* [한국어] rc 는 잠금/파싱 결과, id 는 파싱한 TSM 인덱스 */

	rc = sscanf(buf, "tsm%d\n", &id);	/* [한국어] "tsm0" 같은 문자열에서 숫자만 뽑는다. 성공하면 변환 항목 수 1 을 돌려준다 */
	if (rc != 1)	/* [한국어] 형식이 맞지 않으면 */
		return -EINVAL;	/* [한국어] 거부한다 */

	ACQUIRE(rwsem_write_kill, lock)(&pci_tsm_rwsem);	/* [한국어] 쓰기 잠금을 잡는다. kill 판이라 SIGKILL 에만 중단된다 */
	if ((rc = ACQUIRE_ERR(rwsem_write_kill, &lock)))	/* [한국어] 중단 여부 확인 */
		return rc;	/* [한국어] 중단됐으면 errno 를 돌려준다 */

	if (pdev->tsm)	/* [한국어] 이미 어떤 TSM 에 붙어 있으면 */
		return -EBUSY;	/* [한국어] 먼저 disconnect 해야 한다 */

	struct tsm_dev *tsm_dev __free(put_tsm_dev) = find_tsm_dev(id);	/* [한국어] id 로 플랫폼 TSM 을 찾는다. 참조가 올라가고, 이 함수를 벗어날 때 자동으로 내려간다 */
	if (!is_link_tsm(tsm_dev))	/* [한국어] 찾지 못했거나(NULL) link TSM 이 아니면 이 경로로 연결할 수 없다. is_link_tsm 이 NULL 을 처리하므로 별도 검사가 없다 */
		return -ENXIO;	/* [한국어] -ENXIO */

	rc = pci_tsm_connect(pdev, tsm_dev);	/* [한국어] 실제 연결을 수행한다. 컨텍스트 생성, 세션 수립, 하위 함수 probe 까지 여기서 일어난다 */
	if (rc)	/* [한국어] 연결 실패 */
		return rc;	/* [한국어] 오류를 그대로 사용자에게 돌려준다 */
	return len;	/* [한국어] 입력을 모두 소비했다고 알려야 write 가 성공으로 끝난다 */
}
/* [한국어] tsm/connect 를 읽기+쓰기(0644)로 정의한다. show 와 store 이름은
 * DEVICE_ATTR_RW 매크로 규약에 따라 connect_show / connect_store 로 정해진다. */
static DEVICE_ATTR_RW(connect);

/*
 * [한국어]
 * remove_fn - 순회 콜백. 함수 하나의 TSM 컨텍스트와 sysfs 를 걷어낸다
 *
 * @pdev: 정리할 PCI 함수.
 * @data: 쓰지 않는다. 순회 콜백 시그니처를 맞추기 위한 인자다.
 * @return: 항상 0. 순회를 멈추지 않기 위해서다.
 *
 * probe_fn() 의 정확한 반대다. probe_fn 이 컨텍스트를 만들고 sysfs 를 열었다면
 * 이쪽은 컨텍스트를 없애고 sysfs 를 닫는다. 그래서
 * pci_tsm_walk_fns(probe_fn) 과 pci_tsm_walk_fns_reverse(remove_fn) 이
 * 짝을 이룬다.
 *
 * 순서가 중요하다. 컨텍스트를 먼저 없애야 link_sysfs_disable 이 다시 계산하는
 * 가시성 판정에서 pdev->tsm 이 NULL 로 보이고, 그래야 속성이 실제로 감춰진다
 * (pci_tsm_link_group_visible 이 pdev->tsm 을 본다).
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자(__pci_tsm_disconnect)가
 * DSM 뮤텍스와 쓰기 rwsem 을 모두 잡은 상태에서 불린다.
 *
 * 호출 체인:
 *   __pci_tsm_disconnect() -> pci_tsm_walk_fns_reverse() -> [remove_fn]
 *     -> tsm_remove() -> link_sysfs_disable()
 */
static int remove_fn(struct pci_dev *pdev, void *data)
{
	tsm_remove(pdev->tsm);	/* [한국어] 플랫폼 TSM 의 remove 콜백을 부르고 pdev->tsm 을 NULL 로 만든다. NULL 이면 아무 일도 하지 않는다 */
	link_sysfs_disable(pdev);	/* [한국어] 그다음 sysfs 를 다시 계산한다. 컨텍스트가 없어졌으므로 속성이 감춰진다 */
	return 0;	/* [한국어] 순회를 계속하도록 항상 0 */
}

/*
 * Note, this helper only returns an error code and takes an argument for
 * compatibility with the pci_walk_bus() callback prototype. pci_tsm_unbind()
 * always succeeds.
 */
/*
 * [한국어]
 * (아래 함수의 동작을 조금 더 풀어 쓴다. 위 영어 주석이 원본이다.)
 *
 * @pdev: TDI 를 떼어낼 PCI 함수.
 * @data: 쓰지 않는다. pci_walk_bus() 콜백 시그니처를 맞추기 위한 인자다.
 * @return: 항상 0. 위 영어 주석이 밝히듯 이 함수는 실패하지 않으며,
 *   반환값도 시그니처를 맞추기 위한 것이다. pci_walk_bus() 는
 *   0 이 아닌 값을 만나면 순회를 멈추므로 0 을 지키는 것이 중요하다.
 *
 * 하는 일은 "이 함수가 어떤 VM 에 TDI 로 넘어가 있으면 그 관계를 끊는다" 다.
 * 컨텍스트가 없거나 TDI 가 없으면 조용히 0 을 돌려준다 - 순회 콜백이라
 * 대상이 아닌 함수가 대부분이기 때문이다.
 *
 * 잠금이 두 겹인 구조가 여기서도 반복된다. 바깥 rwsem 은 호출자가 이미
 * 잡고 있어야 하고(아래 lockdep_assert_held 가 확인한다), 안쪽 DSM 뮤텍스는
 * 이 함수가 직접 잡는다. tdi 포인터를 읽고 콜백을 부르고 NULL 로 만드는
 * 세 동작이 원자적이어야 하기 때문이다.
 *
 * 주의: 여기서 잡는 뮤텍스는 이 함수 자신의 것이 아니라 이 함수를 관리하는
 * DSM 의 것이다(to_pci_tsm_pf0 이 dsm_dev 를 따라간다). 그래서 같은 DSM 아래
 * 여러 함수를 순회하는 동안 잠금과 해제가 반복된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_tsm_unbind() / pci_tsm_unbind_all() / pci_tsm_fn_exit()
 *     -> [__pci_tsm_unbind] -> 플랫폼 TSM 의 unbind 콜백
 */
static int __pci_tsm_unbind(struct pci_dev *pdev, void *data)
{
	struct pci_tdi *tdi;	/* [한국어] 현재 바인딩된 TDI */
	struct pci_tsm_pf0 *tsm_pf0;	/* [한국어] DSM 뮤텍스를 들고 있는 PF0 래퍼 */

	lockdep_assert_held(&pci_tsm_rwsem);	/* [한국어] 읽기든 쓰기든 rwsem 을 잡은 상태여야 한다. 호출 경로가 여럿이라 여기서 공통으로 확인한다 */

	if (!pdev->tsm)	/* [한국어] TSM 컨텍스트가 없으면 TDI 도 있을 수 없다 */
		return 0;	/* [한국어] 순회를 계속하도록 0 을 돌려준다 */

	tsm_pf0 = to_pci_tsm_pf0(pdev->tsm);	/* [한국어] 이 함수를 관리하는 DSM 의 래퍼를 얻는다 */
	guard(mutex)(&tsm_pf0->lock);	/* [한국어] DSM 뮤텍스를 잡는다. 인터럽트 불가능 판인 이유는 이 경로가 장치 해체에서도 불리기 때문이다. 스코프 종료 시 자동 해제 */

	tdi = pdev->tsm->tdi;	/* [한국어] 현재 바인딩된 TDI 를 뮤텍스 아래에서 읽는다 */
	if (!tdi)	/* [한국어] 어느 VM 에도 넘어가 있지 않으면 */
		return 0;	/* [한국어] 할 일이 없다 */

	to_pci_tsm_ops(pdev->tsm)->unbind(tdi);	/* [한국어] 플랫폼 TSM 이 TDI 를 해제한다. IOMMU 격리와 게스트 사설 메모리 연결이 여기서 끊긴다 */
	pdev->tsm->tdi = NULL;	/* [한국어] 먼저 콜백을 부르고 그다음 포인터를 지운다. 순서가 바뀌면 콜백이 필요로 하는 정보를 잃는다 */

	return 0;	/* [한국어] 항상 0 - 순회를 멈추지 않게 한다 */
}

/*
 * [한국어]
 * pci_tsm_unbind - 한 PCI 함수의 TDI 바인딩을 푸는 공개 진입점
 *
 * @pdev: 바인딩을 풀 PCI 함수.
 * @return: 없음. 내부 헬퍼가 실패하지 않으므로 보고할 것이 없다.
 *
 * __pci_tsm_unbind() 에 rwsem 을 씌운 얇은 래퍼다. 잠금을 읽기로 잡는 점에
 * 주목할 만하다. 이 경로는 전역 상태(카운터, sysfs)를 바꾸지 않고
 * 개별 장치의 tdi 포인터만 건드리며, 그 포인터는 안쪽 DSM 뮤텍스가
 * 따로 보호하기 때문이다. 덕분에 여러 장치의 unbind 가 동시에 진행될 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 * 이 파일 안에 호출자는 없다. EXPORT_SYMBOL_GPL 로 내보냈고
 * pci_tsm_bind() 의 커널 doc 이 VFIO 드라이버의 bind 상태와 짝을 이룰 것을
 * 전제하지만, 이 트리에서 실제 호출자를 찾을 수는 없었다
 * (drivers/vfio 포함 전수 검색 결과 호출 0건).
 *
 * 호출 체인:
 *   (호출자는 이 트리에 없음) -> [pci_tsm_unbind] -> __pci_tsm_unbind()
 */
void pci_tsm_unbind(struct pci_dev *pdev)
{
	guard(rwsem_read)(&pci_tsm_rwsem);	/* [한국어] 읽기 잠금. 스코프 종료 시 자동 해제된다 */
	__pci_tsm_unbind(pdev, NULL);	/* [한국어] 두 번째 인자는 콜백 시그니처용이라 NULL 을 넘긴다 */
}
EXPORT_SYMBOL_GPL(pci_tsm_unbind);	/* [한국어] 모듈에서 쓸 수 있도록 내보낸다 */

/**
 * pci_tsm_bind() - Bind @pdev as a TDI for @kvm
 * @pdev: PCI device function to bind
 * @kvm: Private memory attach context
 * @tdi_id: Identifier (virtual BDF) for the TDI as referenced by the TSM and DSM
 *
 * Returns 0 on success, or a negative error code on failure.
 *
 * Context: Caller is responsible for constraining the bind lifetime to the
 * registered state of the device. For example, pci_tsm_bind() /
 * pci_tsm_unbind() limited to the VFIO driver bound state of the device.
 */
/*
 * [한국어]
 * (아래 함수의 동작을 조금 더 풀어 쓴다. 위 커널 doc 주석이 원본이다.)
 *
 * @pdev: TDI 로 넘길 PCI 함수.
 * @kvm: 이 TDI 를 받을 게스트의 KVM 컨텍스트. 게스트 사설 메모리를
 *   가리키는 열쇠 역할을 한다.
 * @tdi_id: TSM 과 DSM 이 이 TDI 를 부르는 가상 BDF.
 * @return: 0 이면 성공(이미 같은 VM 에 묶여 있었던 경우도 0).
 *   -EINVAL 은 kvm 이 없거나 컨텍스트가 없는 경우, -ENXIO 는 link TSM 이
 *   아닌 경우, -EBUSY 는 다른 VM 에 이미 묶여 있는 경우,
 *   그 밖의 음수는 플랫폼 TSM 의 bind 콜백이 낸 오류다.
 *
 * 이 함수가 기밀 컴퓨팅의 결정적 순간이다. 여기까지 오면 장치는
 * 인증됐고(SPDM) 링크는 암호화 준비가 됐으며, 이제 그 장치를 게스트에게
 * "호스트가 손댈 수 없는 상태로" 넘긴다.
 *
 * 재바인딩 경쟁 처리에 주목할 만하다. 이미 TDI 가 있으면 두 갈래로 나뉜다.
 *  같은 VM 이면 0 - 두 번 부른 것을 성공으로 받아들여 멱등성을 준다.
 *  다른 VM 이면 -EBUSY - 하나의 함수를 두 VM 이 나눠 가질 수는 없다.
 *
 * 잠금은 읽기 rwsem + DSM 뮤텍스다. 바깥이 읽기여도 되는 이유는
 * pci_tsm_unbind() 와 같다 - tdi 포인터는 안쪽 뮤텍스가 보호한다.
 *
 * 위 커널 doc 의 Context 항목이 중요한 제약을 말한다. bind 의 수명은
 * 호출자가 장치의 등록 상태 안으로 제한해야 한다. 이 파일은 그것을
 * 강제하지 않으므로, 지키지 않으면 장치가 사라진 뒤에도 TDI 가 남는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 * 이 파일 안에 호출자는 없고, 이 트리에서 실제 호출자를 찾을 수 없었다.
 *
 * 호출 체인:
 *   (호출자는 이 트리에 없음) -> [pci_tsm_bind]
 *     -> 플랫폼 TSM 의 bind 콜백
 */
int pci_tsm_bind(struct pci_dev *pdev, struct kvm *kvm, u32 tdi_id)
{
	struct pci_tsm_pf0 *tsm_pf0;	/* [한국어] DSM 뮤텍스를 들고 있는 PF0 래퍼 */
	struct pci_tdi *tdi;	/* [한국어] 플랫폼 TSM 이 만들어 줄 TDI */

	if (!kvm)	/* [한국어] 게스트 컨텍스트가 없으면 사설 메모리 연결을 맺을 대상이 없다 */
		return -EINVAL;	/* [한국어] 잘못된 인자 */

	guard(rwsem_read)(&pci_tsm_rwsem);	/* [한국어] 읽기 잠금. 여러 장치의 bind 가 동시에 진행될 수 있다 */

	if (!pdev->tsm)	/* [한국어] connect 가 먼저 이뤄져 컨텍스트가 있어야 한다 */
		return -EINVAL;	/* [한국어] 순서가 어긋난 요청 */

	if (!is_link_tsm(pdev->tsm->tsm_dev))	/* [한국어] TDI 바인딩은 link TSM 만 제공한다 */
		return -ENXIO;	/* [한국어] devsec TSM 이면 이 기능이 없다 */

	tsm_pf0 = to_pci_tsm_pf0(pdev->tsm);	/* [한국어] DSM 래퍼를 얻는다 */
	guard(mutex)(&tsm_pf0->lock);	/* [한국어] DSM 뮤텍스를 잡는다. 같은 DSM 아래 bind/unbind 를 직렬화한다 */

	/* Resolve races to bind a TDI */
	if (pdev->tsm->tdi) {	/* [한국어] 이미 어떤 VM 에 묶여 있는 경우 */
		if (pdev->tsm->tdi->kvm != kvm)	/* [한국어] 그 VM 이 지금 요청한 VM 과 다르면 */
			return -EBUSY;	/* [한국어] 한 함수를 두 VM 이 나눠 가질 수 없다 */
		return 0;	/* [한국어] 같은 VM 이면 이미 원하는 상태이므로 성공으로 본다. 멱등성을 준다 */
	}

	tdi = to_pci_tsm_ops(pdev->tsm)->bind(pdev, kvm, tdi_id);	/* [한국어] 플랫폼 TSM 이 TDISP 상태를 전이시키고 TDI 를 만든다. IOMMU 와 게스트 사설 메모리 연결이 여기서 이뤄진다 */
	if (IS_ERR(tdi))	/* [한국어] 오류는 포인터에 인코딩되어 온다 */
		return PTR_ERR(tdi);	/* [한국어] 인코딩된 errno 를 꺼내 돌려준다 */

	pdev->tsm->tdi = tdi;	/* [한국어] 성공한 TDI 를 기록한다. 이후 guest_req 와 unbind 가 이 포인터를 쓴다 */

	return 0;	/* [한국어] 바인딩 성공 */
}
EXPORT_SYMBOL_GPL(pci_tsm_bind);	/* [한국어] 모듈에서 쓸 수 있도록 내보낸다 */

/**
 * pci_tsm_guest_req() - helper to marshal guest requests to the TSM driver
 * @pdev: @pdev representing a bound tdi
 * @scope: caller asserts this passthrough request is limited to TDISP operations
 * @req_in: Input payload forwarded from the guest
 * @in_len: Length of @req_in
 * @req_out: Output payload buffer response to the guest
 * @out_len: Length of @req_out on input, bytes filled in @req_out on output
 * @tsm_code: Optional TSM arch specific result code for the guest TSM
 *
 * This is a common entry point for requests triggered by userspace KVM-exit
 * service handlers responding to TDI information or state change requests. The
 * scope parameter limits requests to TDISP state management, or limited debug.
 * This path is only suitable for commands and results that are the host kernel
 * has no use, the host is only facilitating guest to TSM communication.
 *
 * Returns 0 on success and -error on failure and positive "residue" on success
 * but @req_out is filled with less then @out_len, or @req_out is NULL and a
 * residue number of bytes were not consumed from @req_in.  On success or
 * failure @tsm_code may be populated with a TSM implementation specific result
 * code for the guest to consume.
 *
 * Context: Caller is responsible for calling this within the pci_tsm_bind()
 * state of the TDI.
 */
/*
 * [한국어]
 * (아래 함수의 동작을 조금 더 풀어 쓴다. 위 커널 doc 주석이 원본이다.)
 *
 * @pdev: 이미 bind 된 TDI 에 대응하는 PCI 함수.
 * @scope: 요청의 범위. PCI_TSM_REQ_STATE_CHANGE 이하만 허용된다.
 * @req_in: 게스트가 보낸 요청 페이로드. sockptr_t 라서 커널 포인터일 수도,
 *   사용자 공간 포인터일 수도 있다.
 * @in_len: 요청 길이.
 * @req_out: 응답을 담을 버퍼. NULL 일 수 있다.
 * @out_len: 응답 버퍼 크기.
 * @tsm_code: 플랫폼 TSM 이 게스트에게 전할 구현별 결과 코드를 담을 곳.
 * @return: 플랫폼 TSM 의 guest_req 콜백 결과. 음수는 오류,
 *   양수는 위 영어 주석이 설명하는 "residue" 다.
 *
 * 기밀 컴퓨팅의 신뢰 모델이 이 함수의 모양을 결정한다. 게스트는 호스트
 * 커널을 믿지 않는다. 그런데 TDI 의 상태를 바꾸려면 플랫폼 TSM 과
 * 대화해야 하고, 그 통로는 호스트를 지난다. 그래서 호스트는 내용을
 * 해석하지 않고 그대로 전달만 한다 - 위 영어 주석의
 * "the host is only facilitating guest to TSM communication" 이 그 뜻이다.
 *
 * 다만 무엇이든 전달해 주지는 않는다. scope 로 범위를 TDISP 상태 관리와
 * 제한적인 디버그로 좁힌다. 이 검사가 없으면 게스트가 이 통로로
 * 플랫폼 TSM 에 임의 명령을 보낼 수 있다.
 *
 * 잠금이 두 겹이고 둘 다 인터럽트 가능 판인 이유: 이 경로는 게스트의
 * KVM exit 을 처리하는 사용자 공간 스레드에서 불리므로, 그 스레드가
 * 시그널을 받으면 빠져나올 수 있어야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥(KVM exit 처리). 잠들 수 있다.
 * 이 파일 안에 호출자는 없다. EXPORT_SYMBOL_GPL 로 내보낸 API 이며,
 * 위 영어 주석은 VFIO 드라이버가 bind 상태 안에서 부를 것을 전제한다.
 * 다만 이 트리에서 실제 호출자를 찾을 수는 없었다
 * (drivers/vfio 를 포함해 주석을 제거하고 전수 검색한 결과 호출 0건).
 *
 * 호출 체인:
 *   (호출자는 이 트리에 없음) -> [pci_tsm_guest_req]
 *     -> 플랫폼 TSM 의 guest_req 콜백
 */
ssize_t pci_tsm_guest_req(struct pci_dev *pdev, enum pci_tsm_req_scope scope,
			  sockptr_t req_in, size_t in_len, sockptr_t req_out,
			  size_t out_len, u64 *tsm_code)
{
	struct pci_tsm_pf0 *tsm_pf0;	/* [한국어] DSM 뮤텍스를 들고 있는 PF0 래퍼 */
	struct pci_tdi *tdi;	/* [한국어] 현재 바인딩된 TDI */
	int rc;	/* [한국어] 잠금 획득 결과 */

	/* Forbid requests that are not directly related to TDISP operations */
	if (scope > PCI_TSM_REQ_STATE_CHANGE)	/* [한국어] 허용 범위를 넘는 요청은 거부한다. 이 검사가 게스트 -> 플랫폼 TSM 통로의 유일한 관문이다 */
		return -EINVAL;	/* [한국어] 범위를 벗어난 요청 */

	ACQUIRE(rwsem_read_intr, lock)(&pci_tsm_rwsem);	/* [한국어] 읽기 잠금을 인터럽트 가능하게 잡는다. 컨텍스트가 사라지는 것을 막는다 */
	if ((rc = ACQUIRE_ERR(rwsem_read_intr, &lock)))	/* [한국어] 시그널로 중단됐는지 확인한다 */
		return rc;	/* [한국어] 중단됐으면 errno 를 돌려준다 */

	if (!pdev->tsm)	/* [한국어] 컨텍스트가 없으면 전달할 상대가 없다 */
		return -ENXIO;	/* [한국어] -ENXIO */

	if (!is_link_tsm(pdev->tsm->tsm_dev))	/* [한국어] 게스트 요청 전달은 link TSM 만 지원한다 */
		return -ENXIO;	/* [한국어] -ENXIO */

	tsm_pf0 = to_pci_tsm_pf0(pdev->tsm);	/* [한국어] DSM 래퍼를 얻는다 */
	ACQUIRE(mutex_intr, ops_lock)(&tsm_pf0->lock);	/* [한국어] DSM 뮤텍스도 인터럽트 가능하게 잡는다. 다른 TDI 연산이 진행 중이면 기다린다 */
	if ((rc = ACQUIRE_ERR(mutex_intr, &ops_lock)))	/* [한국어] 시그널로 중단됐는지 확인한다 */
		return rc;	/* [한국어] 중단됐으면 errno 를 돌려준다 */

	tdi = pdev->tsm->tdi;	/* [한국어] 현재 바인딩된 TDI 를 꺼낸다 */
	if (!tdi)	/* [한국어] bind 되지 않았으면 게스트 요청 자체가 성립하지 않는다 */
		return -ENXIO;	/* [한국어] -ENXIO */
	return to_pci_tsm_ops(pdev->tsm)->guest_req(tdi, scope, req_in, in_len,	/* [한국어] 플랫폼 TSM 에게 그대로 넘긴다. 호스트는 내용을 해석하지 않는다 */
						    req_out, out_len, tsm_code);	/* [한국어] 응답 버퍼와 결과 코드 자리도 함께 넘긴다. 두 잠금은 return 뒤 스코프 종료 시 자동 해제된다 */
}
EXPORT_SYMBOL_GPL(pci_tsm_guest_req);	/* [한국어] 플랫폼 TSM 모듈 밖(예: VFIO)에서 쓰도록 내보낸다 */

/*
 * [한국어]
 * pci_tsm_unbind_all - 이 DSM 아래 모든 TDI 를 VM 에서 떼어낸다
 *
 * @pdev: DSM 역할을 하는 PCI 함수.
 * @return: 없음.
 *
 * 순서가 두 부분으로 나뉜다. 먼저 역순 순회로 딸린 함수들을 처리하고,
 * 마지막에 DSM 자신을 처리한다. 역순 순회 함수가 function 0 을 건너뛰도록
 * 되어 있기 때문이다(pci_tsm_walk_fns_reverse 안의 영어 주석이
 * "on exit, caller will run @cb on function 0" 이라고 밝힌다).
 *
 * 역순인 이유는 의존 관계다. VF 는 PF 에 딸려 있고 하위 장치는 상위 포트에
 * 딸려 있으므로, 아래쪽부터 떼어야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 pci_tsm_rwsem 을 쓰기로 잡고 있고,
 * DSM 뮤텍스는 잡지 않은 상태여야 한다 - __pci_tsm_unbind() 가 함수마다
 * 스스로 그 뮤텍스를 잡기 때문이다.
 *
 * 호출 체인:
 *   __pci_tsm_disconnect() -> [pci_tsm_unbind_all]
 *     -> pci_tsm_walk_fns_reverse(__pci_tsm_unbind) -> __pci_tsm_unbind()
 */
static void pci_tsm_unbind_all(struct pci_dev *pdev)
{
	pci_tsm_walk_fns_reverse(pdev, __pci_tsm_unbind, NULL);	/* [한국어] 하위 스위치 아래 장치, 각 PF 의 VF, 그리고 function 7 부터 1 까지를 역순으로 훑어 TDI 를 떼어낸다 */
	__pci_tsm_unbind(pdev, NULL);	/* [한국어] 순회가 건너뛴 function 0(즉 DSM 자신)을 마지막에 처리한다 */
}

/*
 * [한국어]
 * __pci_tsm_disconnect - DSM 의 TDISP 세션과 딸린 컨텍스트를 모두 해체한다
 *
 * @pdev: DSM 역할을 하는 PCI 함수. pdev->tsm 이 반드시 있어야 한다.
 * @return: 없음. 실패를 보고할 수단이 없는데, 장치 소멸 경로에서도
 *   불리기 때문에 되돌릴 방법이 없기 때문이다.
 *
 * 해체 순서가 곧 안전성이다.
 *  1) pci_tsm_unbind_all() - 이 DSM 아래 모든 TDI 를 VM 에서 떼어낸다.
 *     세션을 먼저 끊으면 아직 VM 이 쓰고 있는 TDI 가 붕 뜬다.
 *  2) DSM 뮤텍스를 잡는다.
 *  3) pci_tsm_walk_fns_reverse(remove_fn) - 하위 함수들의 TSM 컨텍스트와
 *     sysfs 를 역순으로 걷어낸다.
 *  4) ops->disconnect() - 마지막으로 플랫폼 TSM 이 세션을 닫는다.
 *
 * 1번을 뮤텍스 밖에서 하는 이유는 __pci_tsm_unbind() 가 함수마다 스스로
 * 같은 뮤텍스를 잡기 때문이다. 여기서 미리 잡으면 재귀 교착이 된다.
 *
 * 위 영어 주석이 밝히듯 disconnect 는 인터럽트 불가능(uninterruptible)하다.
 * 장치 해체 경로에서 불릴 수 있어 시그널로 중간에 포기할 수 없기 때문이다.
 * 그래서 mutex_intr 가 아니라 guard(mutex) 를 쓴다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 pci_tsm_rwsem 을 쓰기로 잡아야 하며,
 * 아래 lockdep_assert_held_write 가 그것을 강제한다. 잠금이 쓰기여야 하는
 * 이유를 위 영어 주석이 밝힌다 - pci_tsm_init() 과 배타여야 한다.
 *
 * 호출 체인:
 *   pci_tsm_disconnect() -> [__pci_tsm_disconnect]
 *     -> pci_tsm_unbind_all() -> pci_tsm_walk_fns_reverse(remove_fn)
 *     -> ops->disconnect()
 */
static void __pci_tsm_disconnect(struct pci_dev *pdev)
{
	struct pci_tsm_pf0 *tsm_pf0 = to_pci_tsm_pf0(pdev->tsm);	/* [한국어] DSM 뮤텍스를 들고 있는 PF0 래퍼 */
	const struct pci_tsm_ops *ops = to_pci_tsm_ops(pdev->tsm);	/* [한국어] 플랫폼 TSM 의 콜백 표 */

	/* disconnect() mutually exclusive with subfunction pci_tsm_init() */
	lockdep_assert_held_write(&pci_tsm_rwsem);	/* [한국어] 쓰기 잠금 보유를 강제한다. 디버그 빌드에서 위반을 잡는다 */

	pci_tsm_unbind_all(pdev);	/* [한국어] 먼저 모든 TDI 를 VM 에서 떼어낸다. 뮤텍스 밖에서 해야 재귀 교착을 피한다 */

	/*
	 * disconnect() is uninterruptible as it may be called for device
	 * teardown
	 */
	guard(mutex)(&tsm_pf0->lock);	/* [한국어] 여기서부터 DSM 뮤텍스 아래다. 인터럽트 가능 판을 쓰지 않는 이유는 위 영어 주석이 설명한다 */
	pci_tsm_walk_fns_reverse(pdev, remove_fn, NULL);	/* [한국어] 하위 함수들의 컨텍스트와 sysfs 를 역순으로 걷어낸다. VF 가 먼저, 그다음 PF, 마지막이 하위 스위치 아래 장치다 */
	ops->disconnect(pdev);	/* [한국어] 플랫폼 TSM 이 TDISP 세션을 닫는다. 이 시점에 딸린 것이 아무것도 남아 있지 않아야 한다 */
}

/*
 * [한국어]
 * pci_tsm_disconnect - 세션을 끊고 DSM 자신의 컨텍스트까지 없앤다
 *
 * @pdev: DSM 역할을 하는 PCI 함수.
 * @return: 없음.
 *
 * __pci_tsm_disconnect() 는 딸린 것들과 세션까지만 처리하고, DSM 자신의
 * pci_tsm 은 남겨 둔다. 그것까지 없애는 것이 이 얇은 래퍼의 역할이다.
 * 둘을 나눠 둔 이유는 두 단계 사이에서 다르게 행동해야 하는 호출자가
 * 있을 수 있어서로 보이나, 이 트리에는 __pci_tsm_disconnect() 를 단독으로
 * 부르는 곳이 없어 그 의도를 확인할 수는 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 pci_tsm_rwsem 을 쓰기로 잡아야 한다.
 *
 * 호출 체인:
 *   disconnect_store() / __pci_tsm_destroy() -> [pci_tsm_disconnect]
 *     -> __pci_tsm_disconnect() -> tsm_remove()
 */
static void pci_tsm_disconnect(struct pci_dev *pdev)
{
	__pci_tsm_disconnect(pdev);	/* [한국어] TDI 해제, 하위 컨텍스트 제거, 세션 종료까지 수행한다 */
	tsm_remove(pdev->tsm);	/* [한국어] 마지막으로 DSM 자신의 컨텍스트를 제거하고 pdev->tsm 을 NULL 로 만든다 */
}

/*
 * [한국어]
 * disconnect_store - tsm/disconnect 에 쓰면 세션을 끊는다
 *
 * @dev: sysfs 가 넘겨준 device.
 * @attr: 쓰지 않는다.
 * @buf: 사용자가 쓴 문자열. 현재 연결된 TSM 의 이름이어야 한다.
 * @len: 그 길이.
 * @return: 성공이면 len(모두 소비했다는 뜻). -ENXIO 는 연결돼 있지 않은 경우,
 *   -EINVAL 은 이름이 맞지 않는 경우, 그 밖의 음수는 잠금 실패다.
 *
 * 이름을 확인시키는 이유는 안전장치다. echo 1 같은 값으로 실수로 세션을
 * 끊는 일을 막고, 여러 TSM 이 있을 때 "지금 붙어 있는 그것" 을 사용자가
 * 정확히 알고 있음을 확인한다.
 *
 * 잠금이 rwsem_write_kill 인 점에 주의. kill 판은 치명적 시그널(SIGKILL)에만
 * 반응한다. connect_store 와 같은 종류를 쓰는데, 세션을 끊는 도중에
 * Ctrl-C 로 중간 상태를 만들면 곤란하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥(sysfs write). 잠들 수 있다.
 *
 * 호출 체인:
 *   sysfs write -> [disconnect_store] -> pci_tsm_disconnect()
 */
static ssize_t disconnect_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t len)
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] sysfs device 를 PCI 장치로 되돌린다 */
	struct tsm_dev *tsm_dev;	/* [한국어] 현재 붙어 있는 플랫폼 TSM */
	int rc;	/* [한국어] 잠금 획득 결과 */

	ACQUIRE(rwsem_write_kill, lock)(&pci_tsm_rwsem);	/* [한국어] 쓰기 잠금을 잡는다. kill 판이라 SIGKILL 에만 중단된다 */
	if ((rc = ACQUIRE_ERR(rwsem_write_kill, &lock)))	/* [한국어] 중단 여부 확인 */
		return rc;	/* [한국어] 중단됐으면 errno 를 돌려준다 */

	if (!pdev->tsm)	/* [한국어] 애초에 연결돼 있지 않으면 */
		return -ENXIO;	/* [한국어] 끊을 것이 없다 */

	tsm_dev = pdev->tsm->tsm_dev;	/* [한국어] 현재 붙어 있는 TSM 을 꺼낸다 */
	if (!sysfs_streq(buf, dev_name(&tsm_dev->dev)))	/* [한국어] 사용자가 쓴 문자열이 그 이름과 정확히 같은지 확인한다. sysfs_streq 는 끝의 개행을 무시해 준다 */
		return -EINVAL;	/* [한국어] 다르면 거부한다. 실수로 엉뚱한 세션을 끊는 것을 막는 안전장치다 */

	pci_tsm_disconnect(pdev);	/* [한국어] 세션을 끊고 컨텍스트까지 없앤다 */
	return len;	/* [한국어] 입력을 모두 소비했다고 알려야 write 가 성공으로 끝난다 */
}
/* [한국어] tsm/disconnect 를 쓰기 전용(0200)으로 정의한다. 읽어도 의미가 없는
 * 명령형 속성이라 store 만 둔다. */
static DEVICE_ATTR_WO(disconnect);

/*
 * [한국어]
 * bound_show - tsm/bound 를 읽으면 이 함수가 어느 TSM 에 TDI 로 묶였는지 보여 준다
 *
 * @dev: sysfs 가 넘겨준 device. PCI 장치다.
 * @attr: 어느 속성이 읽혔는지. 여기서는 쓰지 않는다.
 * @buf: 출력 버퍼(PAGE_SIZE).
 * @return: 쓴 바이트 수, 또는 잠금 획득이 시그널로 중단되면 음수 errno.
 *
 * connect 와 bound 는 다른 단계다.
 *   connect - 호스트가 장치와 인증 세션을 맺은 상태.
 *   bound   - 그 장치가 특정 기밀 VM 에게 TDI 로 넘어간 상태.
 * 그래서 연결은 됐지만 아직 어느 VM 에도 넘기지 않았으면 빈 줄을 출력한다.
 *
 * 잠금이 두 겹인 이유: 바깥 rwsem 은 pdev->tsm 포인터 자체가 사라지는 것을
 * 막고, 안쪽 DSM 뮤텍스는 tsm->tdi 가 바뀌는 것을 막는다. 두 값이 서로 다른
 * 경로에서 갱신되기 때문이다(전자는 connect/destroy, 후자는 bind/unbind).
 *
 * 두 잠금 모두 인터럽트 가능(intr) 판을 쓴다. 사용자가 cat 을 걸어 둔 채
 * Ctrl-C 를 눌렀을 때 빠져나올 수 있어야 하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥(sysfs read). 잠들 수 있다.
 *
 * 호출 체인:
 *   sysfs read -> [bound_show]
 */
static ssize_t bound_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] sysfs device 를 PCI 장치로 되돌린다 */
	struct pci_tsm_pf0 *tsm_pf0;	/* [한국어] DSM 뮤텍스를 들고 있는 PF0 래퍼 */
	struct pci_tsm *tsm;	/* [한국어] 현재 붙어 있는 TSM 컨텍스트 */
	int rc;	/* [한국어] 잠금 획득 결과 */

	ACQUIRE(rwsem_read_intr, lock)(&pci_tsm_rwsem);	/* [한국어] 읽기 잠금을 인터럽트 가능하게 잡는다. 스코프 종료 시 자동 해제된다 */
	if ((rc = ACQUIRE_ERR(rwsem_read_intr, &lock)))	/* [한국어] 시그널로 중단됐는지 확인한다 */
		return rc;	/* [한국어] 중단됐으면 그 errno 를 그대로 사용자에게 돌려준다 */

	tsm = pdev->tsm;	/* [한국어] 포인터를 지역 변수로 한 번만 읽는다. 아래에서 여러 번 쓰므로 스냅숏을 잡아 두는 편이 읽기 쉽다 */
	if (!tsm)	/* [한국어] 연결조차 되어 있지 않으면 */
		return sysfs_emit(buf, "\n");	/* [한국어] 빈 줄만 출력한다. 사용자 공간의 파서가 다루기 쉬운 형태다 */
	tsm_pf0 = to_pci_tsm_pf0(tsm);	/* [한국어] DSM 래퍼를 얻는다. tdi 포인터를 보호하는 뮤텍스가 거기 있다 */

	ACQUIRE(mutex_intr, ops_lock)(&tsm_pf0->lock);	/* [한국어] DSM 뮤텍스를 인터럽트 가능하게 잡는다. bind/unbind 가 진행 중이면 기다린다 */
	if ((rc = ACQUIRE_ERR(mutex_intr, &ops_lock)))	/* [한국어] 시그널로 중단됐는지 확인한다 */
		return rc;	/* [한국어] 중단됐으면 errno 를 돌려준다 */

	if (!tsm->tdi)	/* [한국어] 연결은 됐지만 아직 어느 VM 에도 넘어가지 않은 상태 */
		return sysfs_emit(buf, "\n");	/* [한국어] 빈 줄을 출력한다 */
	return sysfs_emit(buf, "%s\n", dev_name(&tsm->tsm_dev->dev));	/* [한국어] 묶여 있으면 그 플랫폼 TSM 의 이름을 출력한다 */
}
/* [한국어] tsm/bound 를 읽기 전용(0444)으로 정의한다. show 함수 이름은
 * DEVICE_ATTR_RO 매크로 규약에 따라 bound_show 로 정해져 있다. */
static DEVICE_ATTR_RO(bound);

/*
 * [한국어]
 * dsm_show - tsm/dsm 을 읽으면 이 함수를 관리하는 DSM 의 BDF 를 보여 준다
 *
 * @dev: sysfs 가 넘겨준 device.
 * @attr: 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 쓴 바이트 수, 또는 음수 errno.
 *
 * 사용자가 위상을 이해하는 데 필요한 정보다. 어떤 VF 의 tsm/dsm 을 읽으면
 * "0000:01:00.0" 처럼 그 VF 의 보안 상태를 실제로 관리하는 장치가 나온다.
 * 그 장치는 같은 슬롯의 PF0 일 수도, 위쪽 스위치의 업스트림 포트일 수도 있다
 * (find_dsm_dev 참고).
 *
 * bound_show 와 달리 DSM 뮤텍스를 잡지 않는다. dsm_dev 는 컨텍스트가 만들어질
 * 때 한 번 정해지고 이후 바뀌지 않기 때문이다(pci_tsm_link_constructor 에서
 * 설정되는 것이 유일한 대입이다).
 *
 * 실행 컨텍스트: 프로세스 문맥(sysfs read). 잠들 수 있다.
 *
 * 호출 체인:
 *   sysfs read -> [dsm_show]
 */
static ssize_t dsm_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] sysfs device 를 PCI 장치로 되돌린다 */
	struct pci_tsm *tsm;	/* [한국어] 현재 붙어 있는 TSM 컨텍스트 */
	int rc;	/* [한국어] 잠금 획득 결과 */

	ACQUIRE(rwsem_read_intr, lock)(&pci_tsm_rwsem);	/* [한국어] 읽기 잠금을 인터럽트 가능하게 잡는다. pdev->tsm 이 사라지는 것을 막는다 */
	if ((rc = ACQUIRE_ERR(rwsem_read_intr, &lock)))	/* [한국어] 시그널로 중단됐는지 확인한다 */
		return rc;	/* [한국어] 중단됐으면 errno 를 돌려준다 */

	tsm = pdev->tsm;	/* [한국어] 포인터를 지역 변수로 읽는다 */
	if (!tsm)	/* [한국어] 컨텍스트가 없으면 관리 주체도 없다 */
		return sysfs_emit(buf, "\n");	/* [한국어] 빈 줄을 출력한다 */

	return sysfs_emit(buf, "%s\n", pci_name(tsm->dsm_dev));	/* [한국어] DSM 의 BDF 문자열을 출력한다. pci_name 은 "0000:01:00.0" 형태를 준다 */
}
/* [한국어] tsm/dsm 을 읽기 전용으로 정의한다. */
static DEVICE_ATTR_RO(dsm);

/* The 'authenticated' attribute is exclusive to the presence of a 'link' TSM */
/*
 * [한국어]
 * pci_tsm_link_group_visible - link TSM 관련 sysfs 를 이 장치에 보일지 정한다
 *
 * @kobj: 대상 장치의 sysfs kobject.
 * @return: true 면 보이고 false 면 감춘다.
 *
 * sysfs 속성을 조건부로 드러내는 장치다. 아무 장치에나 tsm/ 디렉터리가
 * 생기면 사용자가 혼란스럽고, 무엇보다 TSM 이 하나도 없는 시스템에서는
 * 그 파일들이 아무 의미가 없다.
 *
 * 판정 순서:
 *  1) link TSM 이 하나도 등록되지 않았으면 무조건 감춘다.
 *  2) PCIe 장치가 아니면 감춘다. TDISP 는 PCIe 스펙의 기능이다.
 *  3) PF0 조건을 만족하면 보인다 - DSM 이 될 수 있는 자리다.
 *  4) 그 밖에도 이미 DSM 아래에서 관리되고 있으면 보인다
 *     (위 영어 주석이 설명하는 하위 함수의 경우).
 *
 * 이 함수의 결과가 pci_tsm_link_count 에 의존하므로,
 * 첫 TSM 등록 시점과 마지막 해제 시점에 sysfs_update_group() 을 다시
 * 불러 줘야 한다. 그 일을 link_sysfs_enable/disable 이 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥(sysfs 그룹 갱신). 이 함수 자체는 잠금을
 * 잡지 않고 전역 카운터를 읽기만 한다. 호출자가 이미
 * pci_tsm_rwsem 을 쓰기로 잡고 있는 경로(pci_tsm_register)가 있어
 * 그 안에서 다시 잡으면 교착이 된다는 점에 주의.
 *
 * 호출 체인:
 *   sysfs 그룹 가시성 판정 -> [pci_tsm_link_group_visible]
 *   pci_tsm_attr_visible() -> [pci_tsm_link_group_visible]
 */
static bool pci_tsm_link_group_visible(struct kobject *kobj)
{
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj));	/* [한국어] kobject 에서 device 를 거쳐 PCI 장치를 얻는다 */

	if (!pci_tsm_link_count)	/* [한국어] link TSM 이 하나도 없으면 이 속성들이 의미가 없다 */
		return false;	/* [한국어] 감춘다 */

	if (!pci_is_pcie(pdev))	/* [한국어] TDISP 는 PCIe 스펙의 기능이므로 재래식 PCI 장치에는 해당이 없다 */
		return false;	/* [한국어] 감춘다 */

	if (is_pci_tsm_pf0(pdev))	/* [한국어] PF0 조건을 만족하면 DSM 이 될 수 있는 자리다 */
		return true;	/* [한국어] 보인다. 아직 connect 되지 않았어도 connect 를 걸 수 있어야 하기 때문이다 */

	/*
	 * Show 'authenticated' and other attributes for the managed
	 * sub-functions of a DSM.
	 */
	if (pdev->tsm)	/* [한국어] DSM 이 아니어도 이미 컨텍스트가 붙어 있으면 관리 대상이다 */
		return true;	/* [한국어] 보인다 */

	return false;	/* [한국어] 위 조건에 모두 해당하지 않으면 감춘다 */
}
/* [한국어] 위 판정 함수를 sysfs 그룹 가시성 콜백 형태로 감싸는 매크로.
 * pci_tsm_auth_attr_group 의 .is_visible 이 이것을 쓴다. 매크로 정의는
 * 이 트리에 없는 linux/sysfs.h 에 있어 전개 결과를 확인할 수는 없다. */
DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE(pci_tsm_link);

/*
 * 'link' and 'devsec' TSMs share the same 'tsm/' sysfs group, so the TSM type
 * specific attributes need individual visibility checks.
 */
/*
 * [한국어]
 * pci_tsm_attr_visible - tsm/ 그룹 안 속성 하나하나의 노출 여부를 정한다
 *
 * @kobj: 대상 장치의 kobject.
 * @attr: 판정할 속성.
 * @n: 그룹 안에서의 인덱스. 여기서는 쓰지 않는다.
 * @return: 보이면 원래 모드(예: 0644), 감추면 0.
 *
 * 그룹 전체가 아니라 파일 단위로 갈래를 나눈다. 위 영어 주석이 이유를
 * 밝히듯 link TSM 과 devsec TSM 이 같은 tsm/ 디렉터리를 공유하기 때문이다.
 *
 * 속성별 규칙:
 *  bound   - PF0 이면서 TEE capability 가 있거나,
 *            관리받는 함수인데 그 DSM 에 TEE 가 있을 때. TDI 바인딩은
 *            TEE 를 전제하므로 그 조건이 붙는다.
 *  dsm     - PF0 이거나, 관리받는 함수인데 DSM 에 TEE 가 있을 때.
 *            PF0 에는 TEE 조건이 없다 - 자기 자신이 DSM 이므로
 *            그 사실만으로 보여 줄 값이 있다.
 *  connect / disconnect - PF0 에서만. 세션은 DSM 단위로 맺고 끊기 때문에
 *            개별 VF 에서 connect 를 거는 것은 의미가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥(sysfs 그룹 갱신).
 *
 * 호출 체인:
 *   sysfs 그룹 생성/갱신 -> [pci_tsm_attr_visible]
 *     -> pci_tsm_link_group_visible()
 */
static umode_t pci_tsm_attr_visible(struct kobject *kobj,
				    struct attribute *attr, int n)
{
	if (pci_tsm_link_group_visible(kobj)) {	/* [한국어] 그룹 자체가 보이지 않는 장치라면 개별 속성도 볼 필요가 없다 */
		struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj));	/* [한국어] kobject 에서 PCI 장치를 얻는다 */

		if (attr == &dev_attr_bound.attr) {	/* [한국어] bound 속성에 대한 판정 */
			if (is_pci_tsm_pf0(pdev) && has_tee(pdev))	/* [한국어] 자기 자신이 DSM 자리이고 TEE capability 를 광고한다면 */
				return attr->mode;	/* [한국어] 원래 모드 그대로 보인다 */
			if (pdev->tsm && has_tee(pdev->tsm->dsm_dev))	/* [한국어] 관리받는 함수인데 그 DSM 이 TEE 를 광고한다면 */
				return attr->mode;	/* [한국어] 역시 보인다 */
		}

		if (attr == &dev_attr_dsm.attr) {	/* [한국어] dsm 속성에 대한 판정 */
			if (is_pci_tsm_pf0(pdev))	/* [한국어] PF0 이면 TEE 조건 없이 */
				return attr->mode;	/* [한국어] 보인다. 자기 자신이 DSM 이므로 보여 줄 값이 있다 */
			if (pdev->tsm && has_tee(pdev->tsm->dsm_dev))	/* [한국어] 관리받는 함수는 DSM 이 TEE 를 가질 때만 */
				return attr->mode;	/* [한국어] 보인다 */
		}

		if (attr == &dev_attr_connect.attr ||	/* [한국어] connect 와 disconnect 는 같은 규칙을 쓴다 */
		    attr == &dev_attr_disconnect.attr) {
			if (is_pci_tsm_pf0(pdev))	/* [한국어] 세션 제어는 DSM 자리에서만 가능하다 */
				return attr->mode;	/* [한국어] 보인다 */
		}
	}

	return 0;	/* [한국어] 위 어느 규칙에도 걸리지 않으면 감춘다. sysfs 는 모드 0 을 "파일을 만들지 않음" 으로 해석한다 */
}

/*
 * [한국어]
 * pci_tsm_group_visible - tsm/ 그룹 전체의 노출 여부
 *
 * @kobj: 대상 장치의 kobject.
 * @return: true 면 그룹을 만들고 false 면 만들지 않는다.
 *
 * 현재는 authenticated 그룹과 완전히 같은 규칙을 쓴다. 그래서 한 줄로
 * pci_tsm_link_group_visible() 에 위임한다. 두 그룹의 규칙이 앞으로
 * 달라질 수 있어 이름만 따로 두었다고 볼 수 있으나, 그 의도는
 * 이 트리만으로는 확인할 수 없다.
 *
 * 그룹이 보이더라도 그 안의 개별 파일은 pci_tsm_attr_visible() 이 다시
 * 걸러 낸다. 즉 두 단계 판정이다.
 *
 * 실행 컨텍스트: 프로세스 문맥(sysfs 그룹 갱신).
 *
 * 호출 체인:
 *   sysfs 그룹 생성/갱신 -> [pci_tsm_group_visible]
 *     -> pci_tsm_link_group_visible()
 */
static bool pci_tsm_group_visible(struct kobject *kobj)
{
	return pci_tsm_link_group_visible(kobj);	/* [한국어] authenticated 그룹과 같은 규칙을 그대로 쓴다 */
}
/* [한국어] 위 두 함수(pci_tsm_group_visible 과 pci_tsm_attr_visible)를 묶어
 * SYSFS_GROUP_VISIBLE(pci_tsm) 로 참조할 수 있는 콜백을 만드는 매크로.
 * 이름 규약상 <prefix>_group_visible 과 <prefix>_attr_visible 을 찾는다.
 * 매크로 정의는 이 트리에 없는 linux/sysfs.h 에 있다. */
DEFINE_SYSFS_GROUP_VISIBLE(pci_tsm);

/*
 * [한국어] pci_tsm_attrs - tsm/ 디렉터리에 놓일 속성 목록.
 *
 * 여기 나열된 순서가 pci_tsm_attr_visible 의 인덱스 n 과 대응하지만,
 * 그 함수는 n 을 쓰지 않고 속성 포인터를 직접 비교한다.
 * 마지막 NULL 이 목록의 끝 표시다.
 */
static struct attribute *pci_tsm_attrs[] = {
	&dev_attr_connect.attr,	/* [한국어] 쓰기: 어느 TSM 에 연결할지 지정. 읽기: 현재 연결된 TSM 이름 */
	&dev_attr_disconnect.attr,	/* [한국어] 쓰기 전용. 연결된 TSM 이름을 정확히 써야 끊긴다 */
	&dev_attr_bound.attr,	/* [한국어] 읽기 전용. 어느 VM 에 TDI 로 묶였는지 */
	&dev_attr_dsm.attr,	/* [한국어] 읽기 전용. 이 함수를 관리하는 DSM 의 BDF */
	NULL	/* [한국어] 목록 끝 센티널 */
};

/*
 * [한국어] pci_tsm_attr_group - /sys/bus/pci/devices/<BDF>/tsm/ 디렉터리 정의.
 *
 * drivers/pci/pci-sysfs.c 의 속성 그룹 배열에 CONFIG_PCI_TSM 일 때만
 * 포함되어(같은 파일에서 확인), PCI 장치마다 이 그룹이 검토된다.
 * 실제로 파일이 생기는지는 위 두 단계 가시성 판정이 결정한다.
 */
const struct attribute_group pci_tsm_attr_group = {
	.name = "tsm",	/* [한국어] 그룹에 이름을 주면 sysfs 에 그 이름의 하위 디렉터리가 생긴다. 이름이 없으면 장치 디렉터리에 바로 놓인다 */
	.attrs = pci_tsm_attrs,	/* [한국어] 위에서 정의한 속성 목록 */
	.is_visible = SYSFS_GROUP_VISIBLE(pci_tsm),	/* [한국어] 그룹과 속성 두 단계 가시성 콜백을 연결한다 */
};

/*
 * [한국어]
 * authenticated_show - 표준 이름의 인증 상태 속성을 읽는다
 *
 * @dev: sysfs 가 넘겨준 device.
 * @attr: 그대로 connect_show 에 넘긴다.
 * @buf: 출력 버퍼.
 * @return: connect_show() 의 결과 그대로.
 *
 * 왜 connect 와 똑같은 값을 다른 이름으로 또 내보내는가.
 * authenticated 는 PCI 장치의 인증 여부를 나타내는 자리이고,
 * 사용자 공간은 그 이름으로 상태를 확인한다. 이 커널에서 그 인증은
 * TSM 이 맺어 준 SPDM 세션으로 이뤄지므로, 세션이 있다는 것과
 * 연결되어 있다는 것이 같은 말이 된다 - 위 영어 주석이 그렇게 밝히고 있다.
 *
 * 그래서 이 파일을 읽으면 TSM 이름이 나오거나 빈 줄이 나온다.
 * 빈 줄이면 아직 인증되지 않은 것이다.
 *
 * 이 속성이 tsm/ 안이 아니라 장치 디렉터리에 바로 놓이는 점에 주의
 * (pci_tsm_auth_attr_group 에 name 이 없다).
 *
 * 실행 컨텍스트: 프로세스 문맥(sysfs read). connect_show 가 rwsem 을 잡는다.
 *
 * 호출 체인:
 *   sysfs read -> [authenticated_show] -> connect_show()
 */
static ssize_t authenticated_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	/*
	 * When the SPDM session established via TSM the 'authenticated' state
	 * of the device is identical to the connect state.
	 */
	return connect_show(dev, attr, buf);	/* [한국어] connect_show 를 그대로 위임 호출한다. 두 값이 정의상 같으므로 코드를 복제하지 않는다 */
}
/* [한국어] authenticated 를 읽기 전용으로 정의한다. */
static DEVICE_ATTR_RO(authenticated);

/*
 * [한국어] pci_tsm_auth_attrs - authenticated 하나만 담은 목록.
 *
 * 이 그룹만 따로 두는 이유는 가시성 규칙이 다르기 때문이 아니라
 * (현재는 같은 규칙이다) 놓이는 위치가 다르기 때문이다.
 * 아래 그룹에 name 이 없어 장치 디렉터리에 바로 만들어진다.
 */
static struct attribute *pci_tsm_auth_attrs[] = {
	&dev_attr_authenticated.attr,	/* [한국어] 인증 상태를 나타내는 표준 이름의 속성 */
	NULL	/* [한국어] 목록 끝 센티널 */
};

/*
 * [한국어] pci_tsm_auth_attr_group - authenticated 속성을 장치 디렉터리에 놓는다.
 *
 * name 필드가 없으므로 하위 디렉터리를 만들지 않고
 * /sys/bus/pci/devices/<BDF>/authenticated 로 바로 노출된다.
 * pci-sysfs.c 가 tsm 그룹보다 먼저 이 그룹을 배열에 넣어 두었다.
 */
const struct attribute_group pci_tsm_auth_attr_group = {
	.attrs = pci_tsm_auth_attrs,	/* [한국어] authenticated 하나뿐인 목록 */
	.is_visible = SYSFS_GROUP_VISIBLE(pci_tsm_link),	/* [한국어] link TSM 이 있을 때만 보인다. 개별 속성 판정은 없다 - 속성이 하나뿐이라 그룹 판정으로 충분하다 */
};

/*
 * Retrieve physical function0 device whether it has TEE capability or not
 */
/*
 * [한국어]
 * pf0_dev_get - 이 장치가 속한 슬롯의 function 0 을 참조와 함께 가져온다
 *
 * @pdev: 기준이 되는 PCI 함수. VF 일 수도 있다.
 * @return: PF0 의 pci_dev(참조 카운트 +1) 또는 NULL. 호출자가
 *   pci_dev_put() 으로 내려놓아야 한다 - find_dsm_dev() 는 __free(pci_dev_put)
 *   으로 자동 해제한다.
 *
 * 두 단계로 접근한다.
 *  1) pci_physfn() - VF 라면 그것을 만들어 낸 PF 로 올라간다.
 *     VF 는 자기 BDF 를 갖지만 보안 상태는 PF 계열에 속하기 때문이다.
 *  2) 그 PF 가 이미 function 0 이면 그대로, 아니면 같은 슬롯의 function 0 을
 *     찾는다. devfn 의 하위 3비트가 function 번호이므로,
 *     PCI_FUNC(devfn) 을 빼면 같은 device 의 function 0 devfn 이 된다.
 *
 * 위 영어 주석이 밝히듯 TEE capability 유무는 여기서 따지지 않는다.
 * 그 판단은 호출자(find_dsm_dev)와 그 위에서 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. pci_get_slot() 은 버스 목록을 훑으므로
 * 인터럽트 문맥에서 부를 것이 아니다.
 *
 * 호출 체인:
 *   find_dsm_dev() -> [pf0_dev_get] -> pci_physfn() / pci_get_slot()
 */
static struct pci_dev *pf0_dev_get(struct pci_dev *pdev)
{
	struct pci_dev *pf_dev = pci_physfn(pdev);	/* [한국어] VF 면 그 PF 로, 아니면 자기 자신을 돌려준다. 참조는 올라가지 않는다 */

	if (PCI_FUNC(pf_dev->devfn) == 0)	/* [한국어] devfn 하위 3비트가 0 이면 이미 function 0 이다 */
		return pci_dev_get(pf_dev);	/* [한국어] 참조를 하나 올려 돌려준다. 반환 규약을 아래 경로와 맞추기 위해서다 */

	return pci_get_slot(pf_dev->bus,	/* [한국어] 같은 버스에서 function 0 을 찾는다. 성공하면 참조가 올라간다 */
			    pf_dev->devfn - PCI_FUNC(pf_dev->devfn));	/* [한국어] devfn 에서 function 비트만 빼면 같은 device 의 function 0 devfn 이 된다. 예: 0x1b(device 3, function 3) - 3 = 0x18 */
}

/*
 * Find the PCI Device instance that serves as the Device Security Manager (DSM)
 * for @pdev. Note that no additional reference is held for the resulting device
 * because that resulting object always has a registered lifetime
 * greater-than-or-equal to that of the @pdev argument. This is by virtue of
 * @pdev being a descendant of, or identical to, the returned DSM device.
 */
/*
 * [한국어]
 * (아래 함수의 동작을 조금 더 풀어 쓴다. 위 영어 주석이 원본이다.)
 *
 * @pdev: DSM 을 찾고 싶은 PCI 함수.
 * @return: DSM 역할을 하는 pci_dev. 없으면 NULL.
 *   참조를 올리지 않는다 - 위 영어 주석이 그 근거를 설명한다.
 *   반환되는 장치는 항상 @pdev 자신이거나 그 조상이므로, @pdev 가 살아 있는
 *   동안 그 장치도 반드시 살아 있기 때문이다.
 *
 * DSM 후보를 세 갈래로 찾는다.
 *  1) @pdev 자신이 PF0 조건을 만족하면 그것이 후보다.
 *  2) 아니면 같은 슬롯의 PF0 를 찾아, 그것이 이미 DSM 이면 채택한다.
 *  3) 그것도 아니면 위상을 한 단계 올라가 업스트림 포트를 본다.
 *     스위치가 그 아래 장치들을 대신해 TDISP 를 제공하는 경우다
 *     (위 영어 주석의 설명).
 *
 * 3번의 조부모 탐색이 왜 그런 모양인가. 엔드포인트의 부모는 그 장치가
 * 매달린 다운스트림 포트이고, 그 부모가 스위치의 업스트림 포트다.
 * 그래서 dev.parent->parent 를 본다. 그 다음 그것이 정말 PCI 장치인지,
 * PCIe 인지, 타입이 업스트림 포트인지 차례로 확인한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. pf0_dev_get() 안에서 참조를 잠깐 올렸다가
 * __free(pci_dev_put) 로 스코프 종료 시 자동으로 내려놓는다.
 *
 * 호출 체인:
 *   pci_tsm_link_constructor() / pci_tsm_init() -> [find_dsm_dev]
 *     -> pf0_dev_get() -> is_dsm()
 */
static struct pci_dev *find_dsm_dev(struct pci_dev *pdev)
{
	struct device *grandparent;	/* [한국어] 업스트림 포트 후보를 담을 device 포인터 */
	struct pci_dev *uport;	/* [한국어] 그것을 pci_dev 로 변환한 결과 */

	if (is_pci_tsm_pf0(pdev))	/* [한국어] 자기 자신이 PF0 조건을 만족하면 */
		return pdev;	/* [한국어] 그대로 DSM 후보다. 참조를 올리지 않고 돌려준다 */

	struct pci_dev *pf0 __free(pci_dev_put) = pf0_dev_get(pdev);	/* [한국어] 같은 슬롯의 PF0 를 참조와 함께 가져온다. 스코프를 벗어나면 자동으로 put 된다 */
	if (!pf0)	/* [한국어] PF0 가 없는 위상이면 */
		return NULL;	/* [한국어] DSM 을 찾을 수 없다 */

	if (is_dsm(pf0))	/* [한국어] 그 PF0 가 이미 DSM 으로 세워져 있으면 */
		return pf0;	/* [한국어] 그것이 답이다. 여기서 돌려주는 포인터는 __free 로 참조가 내려간 뒤에도 유효하다 - 위 영어 주석이 설명하는 수명 관계 덕분이다 */

	/*
	 * For cases where a switch may be hosting TDISP services on behalf of
	 * downstream devices, check the first upstream port relative to this
	 * endpoint.
	 */
	if (!pdev->dev.parent)	/* [한국어] 부모가 없으면 위상을 올라갈 수 없다 */
		return NULL;	/* [한국어] 포기한다 */
	grandparent = pdev->dev.parent->parent;	/* [한국어] 엔드포인트의 부모는 다운스트림 포트, 그 부모가 스위치의 업스트림 포트다 */
	if (!grandparent)	/* [한국어] 조부모가 없으면 루트 바로 아래라는 뜻이다 */
		return NULL;	/* [한국어] 포기한다 */
	if (!dev_is_pci(grandparent))	/* [한국어] 조부모가 PCI 장치가 아니면(예: 플랫폼 장치) */
		return NULL;	/* [한국어] 포기한다 */
	uport = to_pci_dev(grandparent);	/* [한국어] device 를 pci_dev 로 변환한다 */
	if (!pci_is_pcie(uport) ||	/* [한국어] PCIe 이면서 타입이 업스트림 포트여야 스위치의 그 자리다 */
	    pci_pcie_type(uport) != PCI_EXP_TYPE_UPSTREAM)
		return NULL;	/* [한국어] 조건에 맞지 않으면 포기한다 */

	if (is_dsm(uport))	/* [한국어] 그 업스트림 포트가 DSM 으로 세워져 있으면 */
		return uport;	/* [한국어] 그것이 이 엔드포인트의 DSM 이다 */
	return NULL;	/* [한국어] 세 갈래 모두 실패했다 */
}

/**
 * pci_tsm_tdi_constructor() - base 'struct pci_tdi' initialization for link TSMs
 * @pdev: PCI device function representing the TDI
 * @tdi: context to initialize
 * @kvm: Private memory attach context
 * @tdi_id: Identifier (virtual BDF) for the TDI as referenced by the TSM and DSM
 */
/*
 * [한국어]
 * (아래 함수의 동작을 조금 더 풀어 쓴다. 위 커널 doc 주석이 원본이다.)
 *
 * @pdev: 이 TDI 가 대응하는 PCI 함수.
 * @tdi: 플랫폼 TSM 이 할당한 TDI 컨텍스트. 공통 필드 세 개를 여기서 채운다.
 * @kvm: 이 TDI 를 넘겨받을 게스트의 KVM 컨텍스트.
 * @tdi_id: TSM 과 DSM 이 이 TDI 를 식별하는 데 쓰는 가상 BDF.
 * @return: 없음.
 *
 * TDI(Trusted Device Interface)는 TDISP 의 핵심 개념이다. 하나의 PCI 함수를
 * 기밀 VM 에게 "신뢰할 수 있는 상태로" 넘긴 결과가 TDI 다. 호스트가 중간에서
 * 몰래 들여다보거나 바꿔치기할 수 없도록, 링크는 IDE 로 암호화되고
 * 장치는 SPDM 으로 인증된 상태여야 한다.
 *
 * 이 함수 자체는 세 필드를 대입하는 것이 전부다. 실제 TDISP 상태 전이는
 * 플랫폼 TSM 의 bind 콜백이 수행하고, 이 생성자는 그 콜백이 만든 구조체의
 * 공통부를 채워 주는 보조다.
 *
 * 실행 컨텍스트: 호출자를 따른다. 이 파일 안에 호출자는 없고,
 * 플랫폼 TSM 모듈이 bind 콜백 안에서 부르도록 내보낸 API 다.
 *
 * 호출 체인:
 *   플랫폼 TSM 드라이버의 bind 콜백 -> [pci_tsm_tdi_constructor]
 */
void pci_tsm_tdi_constructor(struct pci_dev *pdev, struct pci_tdi *tdi,
			     struct kvm *kvm, u32 tdi_id)
{
	tdi->pdev = pdev;	/* [한국어] 이 TDI 가 어느 PCI 함수의 것인지 기록한다 */
	tdi->kvm = kvm;	/* [한국어] 어느 게스트에 묶였는지 기록한다. pci_tsm_bind 가 재바인딩 요청을 이 값과 비교해 -EBUSY 를 판정한다 */
	tdi->tdi_id = tdi_id;	/* [한국어] TSM 과 DSM 이 쓰는 가상 BDF. 게스트가 보는 BDF 와 호스트의 실제 BDF 가 다르므로 별도 식별자가 필요하다 */
}
EXPORT_SYMBOL_GPL(pci_tsm_tdi_constructor);	/* [한국어] 플랫폼 TSM 모듈용 API 로 내보낸다 */

/**
 * pci_tsm_link_constructor() - base 'struct pci_tsm' initialization for link TSMs
 * @pdev: The PCI device
 * @tsm: context to initialize
 * @tsm_dev: Platform TEE Security Manager, initiator of security operations
 */
/*
 * [한국어]
 * pci_tsm_link_constructor - link TSM 이 쓸 struct pci_tsm 의 공통부를 채운다
 *
 * @pdev: 이 컨텍스트가 대응할 PCI 함수.
 * @tsm: 플랫폼 TSM 드라이버가 할당한 컨텍스트. 여기서 공통 필드를 채운다.
 * @tsm_dev: 이 컨텍스트를 만드는 플랫폼 TSM.
 * @return: 0 이면 성공. -EINVAL 은 link TSM 이 아닌 경우,
 *   -ENXIO 는 위상에서 DSM 을 찾지 못한 경우다.
 *
 * 플랫폼 TSM 드라이버는 자기만의 확장 구조체 안에 struct pci_tsm 을 품는다.
 * 그 확장 부분은 드라이버가 채우고, 세 개의 공통 필드는 이 함수가 채운다.
 * 그중 dsm_dev 를 찾는 일이 핵심인데, 이후 모든 잠금(to_pci_tsm_pf0)이
 * 그 포인터를 따라가기 때문이다.
 *
 * 이 파일에는 호출자가 없다. EXPORT_SYMBOL_GPL 로 내보내 플랫폼 TSM
 * 모듈이 probe 콜백 안에서 부르도록 만든 API 다. 이 트리에는 그런
 * 모듈이 들어 있지 않아 실제 호출자를 확인할 수 없다.
 *
 * 실행 컨텍스트: 호출자를 따른다. 아래 find_dsm_dev() 가
 * pci_get_slot() 로 참조를 잠깐 올렸다 내리므로 잠들 수 있는 문맥이어야 한다.
 *
 * 호출 체인:
 *   플랫폼 TSM 드라이버의 probe -> [pci_tsm_link_constructor] -> find_dsm_dev()
 */
int pci_tsm_link_constructor(struct pci_dev *pdev, struct pci_tsm *tsm,
			     struct tsm_dev *tsm_dev)
{
	if (!is_link_tsm(tsm_dev))	/* [한국어] devsec 전용 TSM 이 이 생성자를 잘못 부르는 것을 막는다. link_ops.probe 가 있어야 link TSM 이다 */
		return -EINVAL;	/* [한국어] 잘못된 종류의 TSM 이라고 알린다 */

	tsm->dsm_dev = find_dsm_dev(pdev);	/* [한국어] 이 함수가 속한 TDISP 영역의 DSM 을 위상에서 찾는다. 참조를 올리지 않는다는 점은 find_dsm_dev 위의 영어 주석이 설명한다 */
	if (!tsm->dsm_dev) {	/* [한국어] DSM 이 없으면 이 함수의 보안 상태를 관리할 주체가 없다 */
		pci_warn(pdev, "failed to find Device Security Manager\n");	/* [한국어] 원인을 커널 로그로 남긴다 */
		return -ENXIO;	/* [한국어] 장치를 찾을 수 없다는 뜻으로 -ENXIO 를 돌려준다 */
	}
	tsm->pdev = pdev;	/* [한국어] 이 컨텍스트가 어느 PCI 함수의 것인지 기록한다 */
	tsm->tsm_dev = tsm_dev;	/* [한국어] 어느 플랫폼 TSM 이 이 컨텍스트를 소유하는지 기록한다. to_pci_tsm_ops 가 이 포인터로 콜백 표를 찾는다 */

	return 0;	/* [한국어] 세 필드를 모두 채웠다 */
}
EXPORT_SYMBOL_GPL(pci_tsm_link_constructor);	/* [한국어] 플랫폼 TSM 모듈이 쓸 수 있도록 내보낸다. GPL 모듈만 쓸 수 있다 */

/**
 * pci_tsm_pf0_constructor() - common 'struct pci_tsm_pf0' (DSM) initialization
 * @pdev: Physical Function 0 PCI device (as indicated by is_pci_tsm_pf0())
 * @tsm: context to initialize
 * @tsm_dev: Platform TEE Security Manager, initiator of security operations
 */
/*
 * [한국어]
 * pci_tsm_pf0_constructor - DSM 역할을 할 PF0 컨텍스트를 초기화한다
 *
 * @pdev: DSM 이 될 Physical Function 0.
 * @tsm: 플랫폼 TSM 이 할당한 PF0 래퍼. 여기서 뮤텍스와 DOE 우편함을 채운다.
 * @tsm_dev: 이 DSM 을 만드는 플랫폼 TSM.
 * @return: 0 이면 성공. -ENODEV 는 CMA DOE 우편함이 없는 경우,
 *   그 밖의 음수는 pci_tsm_link_constructor() 의 실패다.
 *
 * DSM(Device Security Manager)은 한 장치 집합의 보안 상태를 총괄하는
 * 주체이고, 이 커널에서는 그 자리를 PF0(또는 스위치의 업스트림 포트)가 맡는다.
 * 일반 pci_tsm 과 달리 두 가지를 더 갖는다.
 *  lock   - 이 DSM 아래 모든 함수의 bind/unbind/disconnect 를 직렬화하는 뮤텍스.
 *  doe_mb - 인증 메시지가 오갈 DOE 우편함.
 *
 * DOE 연결이 여기서 확정된다. pci_find_doe_mailbox() 로 vendor 가 PCI-SIG 이고
 * feature 가 CMA 인 우편함을 찾는다. CMA 는 Component Measurement and
 * Authentication 으로, SPDM 메시지를 DOE 위에 실어 나르는 프로토콜이다.
 * 그 우편함이 없으면 장치와 인증 대화를 시작할 수단이 없으므로 -ENODEV 로
 * 실패한다. 그래서 이 커널에서 "TDISP 를 쓰려면 DOE 가 반드시 있어야 한다".
 * DOE 자체의 동작은 drivers/pci/doe.c 에 있다.
 *
 * 실행 컨텍스트: 호출자를 따른다. 이 파일 안에 호출자는 없고,
 * 플랫폼 TSM 모듈이 부르도록 내보낸 API 다.
 *
 * 호출 체인:
 *   플랫폼 TSM 드라이버의 probe -> [pci_tsm_pf0_constructor]
 *     -> pci_find_doe_mailbox() -> pci_tsm_link_constructor()
 */
int pci_tsm_pf0_constructor(struct pci_dev *pdev, struct pci_tsm_pf0 *tsm,
			    struct tsm_dev *tsm_dev)
{
	mutex_init(&tsm->lock);	/* [한국어] 이 DSM 아래 모든 TDI 상태 변경을 직렬화할 뮤텍스를 초기화한다. 짝은 pci_tsm_pf0_destructor 의 mutex_destroy 다 */
	/* [한국어] PCI-SIG 가 정의한 CMA 프로토콜을 지원하는 DOE 우편함을 찾는다.
	 * doe.c 가 열거 시점에 pdev->doe_mbs 에 등록해 둔 것 중에서 고른다.
	 * 두 상수의 실제 값은 이 트리에 없는 헤더에 있어 확인할 수 없다. */
	tsm->doe_mb = pci_find_doe_mailbox(pdev, PCI_VENDOR_ID_PCI_SIG,
					   PCI_DOE_FEATURE_CMA);	/* [한국어] CMA = Component Measurement and Authentication. SPDM 메시지를 실어 나르는 DOE 프로토콜이다 */
	if (!tsm->doe_mb) {	/* [한국어] 우편함이 없으면 장치와 인증 대화를 할 수 없다 */
		pci_warn(pdev, "TSM init failure, no CMA mailbox\n");	/* [한국어] 원인을 로그로 남긴다 */
		return -ENODEV;	/* [한국어] 장치가 요건을 갖추지 못했다는 뜻으로 -ENODEV */
	}

	return pci_tsm_link_constructor(pdev, &tsm->base_tsm, tsm_dev);	/* [한국어] 공통부(dsm_dev, pdev, tsm_dev)를 채운다. 래퍼 안에 박힌 base_tsm 을 넘긴다 */
}
EXPORT_SYMBOL_GPL(pci_tsm_pf0_constructor);	/* [한국어] 플랫폼 TSM 모듈용 API 로 내보낸다 */

/*
 * [한국어]
 * pci_tsm_pf0_destructor - PF0 컨텍스트가 잡고 있던 것을 되돌린다
 *
 * @pf0_tsm: 해제할 PF0 래퍼. 구조체 자체의 free 는 소유자인 플랫폼 TSM 이 한다.
 * @return: 없음.
 *
 * 지금 되돌릴 것은 뮤텍스 하나뿐이다. doe_mb 는 이 코드가 만든 것이 아니라
 * doe.c 가 장치에 매달아 둔 것을 빌려 쓴 포인터라 여기서 해제하지 않는다.
 *
 * mutex_destroy() 는 디버그 빌드에서 "아직 잡혀 있는 뮤텍스를 파괴하는가" 를
 * 검사한다. 그러므로 이 함수는 이 DSM 아래 모든 TDI 가 unbind 되고
 * disconnect 까지 끝난 뒤에 불려야 한다.
 *
 * 실행 컨텍스트: 호출자를 따른다. 이 파일 안에 호출자는 없다.
 *
 * 호출 체인:
 *   플랫폼 TSM 드라이버의 remove -> [pci_tsm_pf0_destructor]
 */
void pci_tsm_pf0_destructor(struct pci_tsm_pf0 *pf0_tsm)
{
	mutex_destroy(&pf0_tsm->lock);	/* [한국어] DSM 뮤텍스를 파괴한다. 디버그 빌드에서는 아직 잠겨 있으면 경고가 난다 */
}
EXPORT_SYMBOL_GPL(pci_tsm_pf0_destructor);	/* [한국어] 플랫폼 TSM 모듈용 API 로 내보낸다 */

/*
 * [한국어]
 * pci_tsm_register - 플랫폼 TSM 하나를 PCI 계층에 등록한다
 *
 * @tsm_dev: 등록할 플랫폼 TSM. 그 안의 pci_ops 로 종류를 판별한다.
 * @return: 0 이면 등록 성공. -EINVAL 은 NULL 이거나 종류가 분명하지 않은 경우다.
 *
 * TSM 은 두 종류로 갈린다.
 *  link TSM   - 물리 링크와 세션을 다룬다. connect/bind 로 이어지는 쪽이다.
 *  devsec TSM - 장치 보안 상태(lock)를 다룬다.
 * 두 ops 를 동시에 구현하면 어느 경로를 타야 할지 모호해지므로 거부한다.
 *
 * 첫 link TSM 이 등록되는 순간이 중요하다. 그전까지는 tsm/ sysfs 그룹이
 * 아예 보이지 않다가(pci_tsm_link_group_visible 이 pci_tsm_link_count 를
 * 본다), 여기서 이미 열거된 모든 PF0 를 훑어 sysfs 를 드러낸다.
 * 즉 TSM 모듈을 나중에 적재해도 기존 장치들이 뒤늦게 연결 가능해진다.
 *
 * 참고로 pci_tsm_devsec_count 는 여기서 올리고 pci_tsm_unregister 에서
 * 내리기만 할 뿐, 이 파일 안에서 그 값을 읽는 곳은 없다
 * (주석을 제거하고 전수 확인).
 *
 * 실행 컨텍스트: 프로세스 문맥. 쓰기 rwsem 을 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   플랫폼 TSM 드라이버의 초기화 -> [pci_tsm_register] -> link_sysfs_enable()
 */
int pci_tsm_register(struct tsm_dev *tsm_dev)
{
	struct pci_dev *pdev = NULL;	/* [한국어] for_each_pci_dev 순회 커서. NULL 로 시작해야 처음부터 훑는다 */

	if (!tsm_dev)	/* [한국어] NULL 방어 */
		return -EINVAL;	/* [한국어] 잘못된 인자 */

	/* The TSM device must only implement one of link_ops or devsec_ops */
	if (!is_link_tsm(tsm_dev) && !is_devsec_tsm(tsm_dev))	/* [한국어] 둘 중 어느 종류도 아니면 PCI 계층이 할 수 있는 일이 없다 */
		return -EINVAL;	/* [한국어] 거부한다 */

	if (is_link_tsm(tsm_dev) && is_devsec_tsm(tsm_dev))	/* [한국어] 두 종류를 동시에 구현하면 경로가 모호해진다 */
		return -EINVAL;	/* [한국어] 역시 거부한다 */

	guard(rwsem_write)(&pci_tsm_rwsem);	/* [한국어] 전역 카운터와 sysfs 갱신을 보호한다. 이 rwsem 이 pci_tsm_init 과의 경쟁을 막는 장치다 */

	/* On first enable, update sysfs groups */
	if (is_link_tsm(tsm_dev) && pci_tsm_link_count++ == 0) {	/* [한국어] link TSM 이면서 카운터를 올린 결과가 0 이었다면 = 이번이 첫 등록이다. 후위 증가라 비교는 증가 전 값으로 이뤄진다 */
		for_each_pci_dev(pdev)	/* [한국어] 시스템의 모든 PCI 장치를 순회한다. 순회 중 참조를 자동으로 관리하는 매크로다 */
			if (is_pci_tsm_pf0(pdev))	/* [한국어] DSM 후보인 PF0 에만 sysfs 를 연다 */
				link_sysfs_enable(pdev);	/* [한국어] tsm/ 와 authenticated 속성 그룹을 다시 계산해 드러낸다 */
	} else if (is_devsec_tsm(tsm_dev)) {	/* [한국어] devsec TSM 인 경우. link TSM 이 아니었으므로 위 분기와 배타적이다 */
		pci_tsm_devsec_count++;	/* [한국어] 카운터만 올린다. sysfs 갱신은 하지 않는다 */
	}

	return 0;	/* [한국어] 등록 성공 */
}

/*
 * [한국어]
 * pci_tsm_fn_exit - DSM 이 아닌 함수 하나의 TSM 상태를 걷어낸다
 *
 * @pdev: 정리할 PCI 함수.
 * @return: 없음.
 *
 * 순서가 의미를 갖는다. 먼저 TDI 바인딩을 풀어 게스트 VM 과의 연결을 끊고,
 * 그 다음에 TSM 컨텍스트 자체를 없앤다. 반대로 하면 아직 VM 이 쓰고 있는
 * TDI 를 관리할 주체가 사라진다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 pci_tsm_rwsem 을 쓰기로 잡고 있다
 * (__pci_tsm_destroy 의 lockdep_assert_held_write 로 확인된다).
 *
 * 호출 체인:
 *   __pci_tsm_destroy() -> [pci_tsm_fn_exit]
 *     -> __pci_tsm_unbind() -> tsm_remove()
 */
static void pci_tsm_fn_exit(struct pci_dev *pdev)
{
	__pci_tsm_unbind(pdev, NULL);	/* [한국어] 활성 TDI 가 있으면 플랫폼 TSM 의 unbind 콜백으로 해제한다. 없으면 즉시 0 을 돌려준다 */
	tsm_remove(pdev->tsm);	/* [한국어] 플랫폼 TSM 의 remove 콜백을 부르고 pdev->tsm 을 NULL 로 만든다 */
}

/**
 * __pci_tsm_destroy() - destroy the TSM context for @pdev
 * @pdev: device to cleanup
 * @tsm_dev: the TSM device being removed, or NULL if @pdev is being removed.
 *
 * At device removal or TSM unregistration all established context
 * with the TSM is torn down. Additionally, if there are no more TSMs
 * registered, the PCI tsm/ sysfs attributes are hidden.
 */
/*
 * [한국어]
 * (아래 함수의 동작을 조금 더 풀어 쓴다. 위 커널 doc 주석이 원본이다.)
 *
 * @pdev: 정리 대상 PCI 함수.
 * @tsm_dev: 사라지는 TSM. 장치 쪽이 사라지는 경우에는 NULL 이 온다.
 * @return: 없음.
 *
 * 이 함수는 두 방향의 소멸을 하나로 처리한다.
 *  (가) 장치가 사라진다  - pci_tsm_destroy() 가 tsm_dev = NULL 로 부른다.
 *  (나) TSM 이 사라진다  - pci_tsm_unregister() 가 모든 장치에 대해 부른다.
 *
 * 그래서 중간에 "이 장치가 지금 사라지는 그 TSM 에 붙어 있었나" 를 확인하는
 * 분기가 있다. 다른 TSM 소속이면 건드리지 않고 돌아간다.
 *
 * sysfs 를 내리는 조건도 (나) 전용이다. (가)에서는 장치 자체가 사라지면서
 * sysfs 도 함께 없어지므로 따로 내릴 필요가 없다 - 위 영어 주석이 그 이유를
 * 밝히고 있다. 조건에 !pci_tsm_link_count 가 들어간 것은
 * pci_tsm_unregister() 가 카운터를 먼저 내린 뒤 이 함수를 부르기 때문이다.
 * 즉 "마지막 link TSM 이 사라졌을 때만" 감춘다.
 *
 * 마지막 분기는 DSM 이냐 아니냐를 가른다. DSM 이면 그 아래 딸린 함수들까지
 * 통째로 정리해야 하므로 pci_tsm_disconnect() 로 가고, 일반 함수면
 * 자기 것만 정리하는 pci_tsm_fn_exit() 로 간다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 pci_tsm_rwsem 을 쓰기로 잡아야 하며,
 * 바로 아래 lockdep_assert_held_write 가 그것을 강제한다.
 *
 * 호출 체인:
 *   pci_tsm_destroy() / pci_tsm_unregister() -> [__pci_tsm_destroy]
 *     -> pci_tsm_disconnect() 또는 pci_tsm_fn_exit()
 */
static void __pci_tsm_destroy(struct pci_dev *pdev, struct tsm_dev *tsm_dev)
{
	struct pci_tsm *tsm = pdev->tsm;	/* [한국어] 현재 붙어 있는 TSM 컨텍스트를 미리 읽어 둔다. 아래에서 여러 번 쓴다 */

	lockdep_assert_held_write(&pci_tsm_rwsem);	/* [한국어] 쓰기 잠금이 필요한 이유는 이 경로가 pci_tsm_init 과 배타여야 하기 때문이다. 디버그 빌드에서 위반을 잡아 준다 */

	/*
	 * First, handle the TSM removal case to shutdown @pdev sysfs, this is
	 * skipped if the device itself is being removed since sysfs goes away
	 * naturally at that point
	 */
	if (is_link_tsm(tsm_dev) && is_pci_tsm_pf0(pdev) && !pci_tsm_link_count)	/* [한국어] 세 조건이 모두 참일 때만. TSM 이 사라지는 경우이고, 대상이 DSM 후보이고, 그것이 마지막 link TSM 이었을 때다 */
		link_sysfs_disable(pdev);	/* [한국어] tsm/ 와 authenticated 속성 그룹을 감춘다 */

	/* Nothing else to do if this device never attached to the departing TSM */
	if (!tsm)	/* [한국어] 이 장치에 애초에 TSM 컨텍스트가 없었다면 */
		return;	/* [한국어] 더 할 일이 없다 */

	/* Now lookup the tsm_dev to destroy TSM context */
	if (!tsm_dev)	/* [한국어] tsm_dev 가 NULL 이면 장치 소멸 경로다 */
		tsm_dev = tsm->tsm_dev;	/* [한국어] 이 장치가 붙어 있던 TSM 을 대상으로 삼는다 */
	else if (tsm_dev != tsm->tsm_dev)	/* [한국어] TSM 소멸 경로인데 이 장치가 다른 TSM 소속이라면 */
		return;	/* [한국어] 건드리지 않고 돌아간다 */

	if (is_link_tsm(tsm_dev) && is_pci_tsm_pf0(pdev))	/* [한국어] link TSM 이고 대상이 DSM 후보라면 딸린 함수들까지 정리해야 한다 */
		pci_tsm_disconnect(pdev);	/* [한국어] TDI 해제, 하위 컨텍스트 제거, 세션 종료를 순서대로 수행한다 */
	else	/* [한국어] 그 밖의 경우 - devsec TSM 이거나 DSM 이 아닌 함수 */
		pci_tsm_fn_exit(pdev);	/* [한국어] 자기 TDI 와 컨텍스트만 정리한다 */
}

/*
 * [한국어]
 * pci_tsm_destroy - PCI 장치가 사라질 때 그 TSM 상태를 정리한다
 *
 * @pdev: 제거되는 PCI 장치.
 * @return: 없음.
 *
 * PCI 코어의 장치 소멸 경로에서 불린다. 확인된 호출자는
 * drivers/pci/remove.c 의 pci_destroy_dev() 하나다.
 * 그 자리의 영어 주석이 밝히듯, 장치가 아직 D0 에 있는 동안
 * (즉 전원이 살아 있는 동안) IDE 와 SPDM 을 정리해야 한다.
 *
 * tsm_dev 에 NULL 을 넘겨 "장치 쪽이 사라진다" 는 뜻을 전한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 쓰기 rwsem 을 잡으므로 잠들 수 있다.
 * guard() 는 스코프를 벗어날 때 자동으로 잠금을 푸는 매크로다.
 *
 * 호출 체인:
 *   pci_destroy_dev() (drivers/pci/remove.c) -> [pci_tsm_destroy]
 *     -> __pci_tsm_destroy()
 */
void pci_tsm_destroy(struct pci_dev *pdev)
{
	guard(rwsem_write)(&pci_tsm_rwsem);	/* [한국어] 쓰기 잠금. connect/disconnect/bind 와 배타적으로 실행되어야 한다. 스코프 종료 시 자동 해제 */
	__pci_tsm_destroy(pdev, NULL);	/* [한국어] NULL 을 넘겨 장치 소멸 경로임을 알린다 */
}

/*
 * [한국어]
 * pci_tsm_init - 새로 열거된 PCI 함수에 TSM 컨텍스트를 붙인다
 *
 * @pdev: 방금 PCI 코어에 추가된 장치.
 * @return: 없음. 실패는 조용히 무시된다 - TSM 없이도 장치는 정상 동작한다.
 *
 * PCI 열거 경로에서 불린다. 확인된 호출자는 drivers/pci/probe.c 의
 * pci_device_add() 하나다. 그 자리의 영어 주석은 새로 생긴 SR-IOV VF 를
 * 예로 든다.
 *
 * 왜 필요한가. 보통은 connect() 가 DSM 을 세우면서 그 아래 함수들까지
 * 한꺼번에 probe 해 준다(pci_tsm_walk_fns). 그런데 그 뒤에 생기는 함수가
 * 있다 - SR-IOV 를 켜서 VF 가 새로 나타나는 경우다. 그런 함수는 이 경로로
 * 뒤늦게 컨텍스트를 얻는다. 위 영어 주석이 그 두 경우와, 드물게
 * connect() 가 초기 버스 스캔과 경쟁하는 경우를 함께 설명한다.
 *
 * DSM 자체는 여기서 만들어지지 않는다. DSM 을 세우는 유일한 길은
 * 사용자가 sysfs 로 connect 를 쓰는 것이며, 그래서 dsm->tsm 이 아직
 * NULL 이면 아무것도 하지 않고 물러난다.
 *
 * 잠금이 읽기인 이유: 이 경로는 컨텍스트를 만들기만 하고 전역 상태
 * (카운터, sysfs 그룹)를 바꾸지 않는다. 반대로 connect/disconnect 는 쓰기
 * 잠금을 잡아 이 경로와 배타를 이룬다(pci_tsm_connect 의
 * lockdep_assert_held_write 주석이 그 의도를 밝힌다).
 *
 * 실행 컨텍스트: 프로세스 문맥(버스 스캔). 읽기 rwsem 을 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_device_add() (drivers/pci/probe.c) -> [pci_tsm_init]
 *     -> find_dsm_dev() -> probe_fn()
 */
void pci_tsm_init(struct pci_dev *pdev)
{
	guard(rwsem_read)(&pci_tsm_rwsem);	/* [한국어] 읽기 잠금이면 충분하다. 여러 함수가 동시에 열거되어도 서로 방해하지 않는다 */

	/*
	 * Subfunctions are either probed synchronous with connect() or later
	 * when either the SR-IOV configuration is changed, or, unlikely,
	 * connect() raced initial bus scanning.
	 */
	if (pdev->tsm)	/* [한국어] 이미 컨텍스트가 있으면 connect() 가 앞서 만들어 준 것이다 */
		return;	/* [한국어] 중복 생성을 피한다 */

	if (pci_tsm_link_count) {	/* [한국어] link TSM 이 하나도 없으면 붙일 대상이 없다 */
		struct pci_dev *dsm = find_dsm_dev(pdev);	/* [한국어] 이 함수가 속할 TDISP 영역의 DSM 을 위상에서 찾는다 */

		if (!dsm)	/* [한국어] DSM 이 없는 위상이면 관리 주체가 없다 */
			return;	/* [한국어] 아무것도 하지 않고 물러난다 */

		/*
		 * The only path to init a Device Security Manager capable
		 * device is via connect().
		 */
		if (!dsm->tsm)	/* [한국어] DSM 은 있지만 아직 connect 되지 않았다면 그 아래 함수도 붙일 수 없다 */
			return;	/* [한국어] 나중에 사용자가 connect 할 때 walk_fns 가 이 함수까지 훑어 준다 */

		probe_fn(pdev, dsm);	/* [한국어] 플랫폼 TSM 의 probe 콜백으로 컨텍스트를 만들고 sysfs 를 연다 */
	}
}

/*
 * [한국어]
 * pci_tsm_unregister - 플랫폼 TSM 을 걷어내고 그에 매인 모든 컨텍스트를 없앤다
 *
 * @tsm_dev: 사라지는 플랫폼 TSM.
 * @return: 없음.
 *
 * 카운터를 먼저 내리고 장치를 순회하는 순서가 중요하다.
 * __pci_tsm_destroy() 안의 sysfs 감추기 조건이 !pci_tsm_link_count 이므로,
 * 카운터가 먼저 0 이 되어야 그 분기가 성립한다.
 *
 * 순회를 역순으로 하는 이유는 정리 순서 때문으로 보인다 - 나중에 열거된
 * 장치(하위 함수, VF, 하위 스위치 아래 장치)가 먼저 정리되어야 상위 DSM 을
 * 안전하게 없앨 수 있다. for_each_pci_dev_reverse 의 정확한 정의는
 * 이 트리에 없는 linux/pci.h 에 있어 확인할 수 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 쓰기 rwsem 을 잡는다.
 *
 * 호출 체인:
 *   플랫폼 TSM 드라이버의 종료 -> [pci_tsm_unregister] -> __pci_tsm_destroy()
 */
void pci_tsm_unregister(struct tsm_dev *tsm_dev)
{
	struct pci_dev *pdev = NULL;	/* [한국어] 역순 순회 커서. NULL 로 시작한다 */

	guard(rwsem_write)(&pci_tsm_rwsem);	/* [한국어] 쓰기 잠금. __pci_tsm_destroy 가 요구하는 잠금이다 */
	if (is_link_tsm(tsm_dev))	/* [한국어] link TSM 이면 */
		pci_tsm_link_count--;	/* [한국어] 카운터를 먼저 내린다. 아래 순회에서 sysfs 감추기 조건이 성립하려면 이 순서여야 한다 */
	if (is_devsec_tsm(tsm_dev))	/* [한국어] devsec TSM 이면 */
		pci_tsm_devsec_count--;	/* [한국어] 그쪽 카운터를 내린다 */
	for_each_pci_dev_reverse(pdev)	/* [한국어] 모든 PCI 장치를 역순으로 훑는다 */
		__pci_tsm_destroy(pdev, tsm_dev);	/* [한국어] 이 TSM 에 매인 컨텍스트만 골라 없앤다. 다른 TSM 소속은 그냥 지나간다 */
}

/*
 * [한국어]
 * pci_tsm_doe_transfer - DSM 의 CMA 우편함으로 메시지 한 번을 주고받는다
 *
 * @pdev: DSM 역할을 하는 PCI 함수. 다른 함수로는 부를 수 없다.
 * @type: DOE 프로토콜 종류(feature). vendor 는 PCI-SIG 로 고정된다.
 * @req: 보낼 요청 페이로드.
 * @req_sz: 요청 길이(바이트).
 * @resp: 응답을 받을 버퍼.
 * @resp_sz: 응답 버퍼 길이.
 * @return: pci_doe() 의 결과. -ENXIO 는 대상이 DSM 이 아니거나
 *   CMA 우편함이 없는 경우다.
 *
 * 이 함수가 tsm.c 와 doe.c 를 잇는 유일한 지점이다.
 * TDISP 는 장치와 대화해야 하는 프로토콜인데, 그 대화가 오갈 물리적
 * 통로가 DOE 다. DOE 는 config 공간 위에 만든 우편함이라
 * BAR 가 배정되기 전에도, BAR 와 무관하게도 쓸 수 있다 - 보안 협상처럼
 * 장치를 신뢰하기 전에 해야 하는 대화에 적합한 이유가 그것이다.
 *
 * 우편함은 pci_tsm_pf0_constructor() 가 미리 찾아 tsm->doe_mb 에 넣어 두었다.
 * 여기서는 그 우편함에 pci_doe() 를 부른다. pci_doe() 는 요청을 작업 큐에
 * 넣고 완료까지 기다리는 동기 래퍼다(drivers/pci/doe.c 에서 확인).
 *
 * DSM 으로 제한하는 이유는 TDISP 구조 때문이다. 인증 세션은 장치 집합당
 * 하나이고 그 주체가 DSM 이므로, 개별 VF 나 하위 함수가 따로 우편함을
 * 두드릴 이유가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥. pci_doe() 가 완료를 기다리므로 잠든다.
 * 이 파일 안에 호출자는 없고, 플랫폼 TSM 모듈이 쓰도록 내보낸 API 다.
 *
 * 호출 체인:
 *   플랫폼 TSM 드라이버 -> [pci_tsm_doe_transfer] -> pci_doe() (doe.c)
 */
int pci_tsm_doe_transfer(struct pci_dev *pdev, u8 type, const void *req,
			 size_t req_sz, void *resp, size_t resp_sz)
{
	struct pci_tsm_pf0 *tsm;	/* [한국어] DOE 우편함을 들고 있는 PF0 래퍼 */

	if (!pdev->tsm || !is_pci_tsm_pf0(pdev))	/* [한국어] 컨텍스트가 없거나 DSM 후보가 아니면 우편함에 접근할 자격이 없다 */
		return -ENXIO;	/* [한국어] 장치를 찾을 수 없다는 뜻으로 -ENXIO */

	tsm = to_pci_tsm_pf0(pdev->tsm);	/* [한국어] 일반 컨텍스트를 PF0 래퍼로 변환한다. doe_mb 는 래퍼에만 있다 */
	if (!tsm->doe_mb)	/* [한국어] 생성자에서 찾아 두었어야 할 우편함이 비어 있는 경우 */
		return -ENXIO;	/* [한국어] 통로가 없으므로 실패시킨다 */

	return pci_doe(tsm->doe_mb, PCI_VENDOR_ID_PCI_SIG, type, req, req_sz,	/* [한국어] vendor 를 PCI-SIG 로 고정해 표준 프로토콜만 쓰도록 한다. 요청을 큐에 넣고 응답까지 기다린다 */
		       resp, resp_sz);	/* [한국어] 응답 버퍼와 그 크기를 넘긴다. 반환값은 실제 응답 길이 또는 음수 오류다 */
}
EXPORT_SYMBOL_GPL(pci_tsm_doe_transfer);	/* [한국어] 플랫폼 TSM 모듈용 API 로 내보낸다 */
