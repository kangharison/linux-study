// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe AER software error injection support.
 *
 * Debugging PCIe AER code is quite difficult because it is hard to
 * trigger various real hardware errors. Software based error
 * injection can fake almost all kinds of errors with the help of a
 * user space helper tool aer-inject, which can be gotten from:
 *   https://github.com/intel/aer-inject.git
 *
 * Copyright 2009 Intel Corporation.
 *     Huang Ying <ying.huang@intel.com>
 */

/*
 * [한국어 설명] 가짜 PCIe 오류를 만들어 복구 경로를 시험하는 도구 (aer_inject.c)
 *
 * === 파일의 역할 ===
 * AER 복구 코드를 시험하기가 매우 어렵다는 문제에서 출발한 파일이다.
 * 위 원문 주석이 그 사정을 밝힌다 — 실제 하드웨어 오류를 일부러 일으키는
 * 것은 거의 불가능하다. Completion Timeout 이나 Poisoned TLP 를 마음대로
 * 만들어 낼 방법이 없기 때문이다.
 *
 * 그래서 이 파일은 오류를 흉내 낸다. 방법이 영리하다 — 실제로 오류를
 * 일으키는 것이 아니라, config space 접근을 가로채서 "AER 상태 레지스터에
 * 오류 비트가 서 있는 것처럼" 보이게 만든다.
 *
 *   1) pci_bus_set_ops() [access.c] 로 그 버스의 config 접근 ops 를
 *      자기 것으로 바꿔 끼운다.
 *   2) AER 관련 레지스터를 읽으면 진짜 하드웨어 대신 사용자가 지정한
 *      가짜 값을 돌려준다.
 *   3) 그 상태에서 AER 인터럽트를 흉내 낸다(aer_irq 를 직접 호출).
 *   4) aer.c 는 진짜 오류가 난 줄 알고 정상 복구 절차를 밟는다.
 *
 * 사용자 인터페이스는 /dev/aer_inject 캐릭터 장치이고, 사용자 공간 도구
 * aer-inject 가 struct aer_error_inj 를 write 해서 무엇을 주입할지 지정한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 주입: aer-inject 도구
 *         -> write(/dev/aer_inject, struct aer_error_inj)
 *            -> [이 파일] aer_inject_write() -> aer_inject()
 *               -> pci_bus_set_ops() 로 config ops 가로채기
 *               -> aer_irq() [aer.c] 직접 호출
 *                  -> 이후는 진짜 오류와 완전히 같은 경로
 *                     -> pcie_do_recovery() -> 드라이버 콜백
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(write 시스템 호출). 다만 가로챈
 * config ops 는 인터럽트 문맥에서도 불릴 수 있어 스핀락으로 보호한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: userspace 의 aer-inject 도구.
 * 아래쪽: access.c 의 pci_bus_set_ops(가로채기), pcie/aer.c 의 aer_irq.
 * 공유 상태: 주입된 오류 목록(einjected)과 가로챈 ops 목록(pci_bus_ops_list).
 *   둘 다 전역 스핀락 inject_lock 으로 보호한다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일과 직접 연결되지 않는다(전수 확인).
 *
 * 하지만 NVMe 오류 복구 코드를 검증하는 데 실질적으로 쓸 수 있는 도구다.
 * NVMe SSD 에 ERR_FATAL 을 주입하면 nvme_error_detected() ->
 * nvme_slot_reset() -> nvme_error_resume() 이 실제로 불리고, 그 과정에서
 * 진행 중이던 I/O 가 올바르게 실패 처리되는지, 컨트롤러가 제대로
 * 재초기화되는지를 확인할 수 있다.
 *
 * 주의할 점은 이것이 "오류를 흉내 낸 것" 이라는 사실이다. 하드웨어는
 * 멀쩡하므로, 진짜 오류에서 벌어지는 일(링크가 실제로 끊기거나 DMA 가
 * 중간에 잘리는 것)은 재현되지 않는다. 소프트웨어 경로의 검증에는 충분하지만
 * 하드웨어 상호작용의 검증에는 한계가 있다.
 *
 * (기존 주석은 주입된 오류가 "nvme_reset_work() / nvme_remove() 로
 *  전파될 수 있다" 고 적었으나, 복구 경로가 직접 부르는 것은
 *  nvme_err_handler 에 등록된 콜백들이다. nvme_reset_work 은 그중
 *  nvme_slot_reset 이 nvme_reset_ctrl 을 큐잉해 간접적으로 실행되고,
 *  nvme_remove 는 복구가 실패해 장치가 제거될 때 불린다.)
 *
 * === 주요 함수/구조체 요약 ===
 * aer_inject()              : 주입의 본체. ops 를 가로채고 가짜 상태를
 *                             등록한 뒤 aer_irq 를 부른다.
 * aer_inject_write()        : /dev/aer_inject 의 write 핸들러.
 * pci_read_aer() / pci_write_aer() : 가로챈 config 접근. AER 레지스터
 *                             범위면 가짜 값을, 아니면 원래 ops 로 넘긴다.
 * find_pci_bus_ops() / pci_bus_set_aer_ops() : ops 가로채기 관리.
 * struct aer_error          : 주입된 가짜 오류 하나. 어느 장치의 어느
 *                             레지스터에 어떤 값을 보이게 할지 담는다.
 * struct pci_bus_ops        : 가로채기 전의 원래 ops 를 보관해 두는 구조.
 *                             해제할 때 되돌리기 위해 필요하다.
 */

#define dev_fmt(fmt) "aer_inject: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/stddef.h>
#include <linux/device.h>

#include "portdrv.h"

/* Override the existing corrected and uncorrected error masks */
static bool aer_mask_override;
module_param(aer_mask_override, bool, 0);
/* [한국어] 아래 구조체가 사용자 공간과 주고받는 ABI 다. 필드 순서와 크기를 바꾸면
 * 기존 도구가 깨진다 -- domain 이 맨 뒤에 있는 것도 나중에 덧붙였기 때문이다. */

struct aer_error_inj {
	/* [한국어] 대상 장치의 버스 번호.
	 * 설정자: 사용자 공간(copy_from_user).
	 * 읽는 자: aer_inject 의 pci_get_domain_bus_and_slot 과 PCI_DEVID.
	 * 값 범위: 0~255.
	 * 동기화: 스택 지역 변수에 복사된 뒤 쓰이므로 경쟁이 없다. */
	u8 bus;
	/* [한국어] 대상 장치 번호. 아래 fn 과 합쳐 devfn 이 된다.
	 * 설정자/읽는 자: 위와 같다. 값 범위 0~31. */
	u8 dev;
	/* [한국어] 대상 함수 번호. 값 범위 0~7. PCI_DEVFN(dev, fn) 으로 합쳐진다. */
	u8 fn;
	/* [한국어] 주입할 uncorrectable 오류 상태 비트들.
	 * 설정자: 사용자 공간.
	 * 읽는 자: aer_inject 가 레코드에 OR 로 얹고, 마스크 검사에도 쓴다.
	 * 값 범위: PCI_ERR_UNC_* 비트 조합. 0 이면 uncorrectable 주입을 하지 않는다.
	 * 동기화: 지역 복사본이라 경쟁 없음. */
	u32 uncor_status;
	/* [한국어] 주입할 correctable 오류 상태 비트들. PCI_ERR_COR_* 조합. */
	u32 cor_status;
	/* [한국어] 주입할 TLP 헤더 로그 첫 워드.
	 * 읽는 자: aer_inject 가 레코드에 **덮어쓴다**(상태와 달리 OR 가 아니다).
	 * 값 범위: 임의의 32비트. 커널 AER 로그가 이 값을 그대로 출력한다. */
	u32 header_log0;
	/* [한국어] 헤더 로그 둘째 워드. */
	u32 header_log1;
	/* [한국어] 헤더 로그 셋째 워드. */
	u32 header_log2;
	/* [한국어] 헤더 로그 넷째 워드. 네 워드가 TLP 헤더 하나를 이룬다. */
	u32 header_log3;
	/* [한국어] 대상 장치의 PCI 도메인 번호.
	 * **구조체 맨 뒤에 있는 것이 중요하다** -- 나중에 추가된 필드라,
	 * aer_inject_write 가 이 필드 없는 짧은 쓰기도 받아 준다(그 경우 0). */
	u32 domain;
/* [한국어] 사용자 ABI 끝. */
};

struct aer_error {
	/* [한국어] einjected 목록에 매달리기 위한 연결 고리.
	 * 설정자: aer_error_init 이 초기화하고 aer_inject 가 list_add.
	 * 읽는 자: 모든 순회. aer_inject_exit 가 list_del.
	 * 동기화: inject_lock 이 지킨다. */
	struct list_head list;
	/* [한국어] 이 레코드가 대응하는 도메인 번호.
	 * 설정자: aer_error_init.
	 * 읽는 자: __find_aer_error 의 대조.
	 * 동기화: inject_lock 아래에서만 읽고 쓴다. */
	u32 domain;
	/* [한국어] 버스 번호. 위와 같은 규칙. */
	unsigned int bus;
	/* [한국어] 장치/함수 번호. 위와 같은 규칙. */
	unsigned int devfn;
	/* [한국어] 이 장치의 AER 확장 능력 오프셋.
	 * 설정자: aer_error_init(호출자가 dev->aer_cap 을 넘긴다).
	 * 읽는 자: find_pci_config_dword 가 상대 오프셋을 구하는 기준.
	 * 값 범위: 양수. -1 이면 AER 없음을 뜻하지만, 이 파일의 호출 경로는
	 * 0 인 경우를 미리 걸러 내므로 -1 이 들어오지는 않는다. */
	int pos_cap_err;
/* [한국어] 아래부터가 시뮬레이션되는 레지스터 값들이다. 커널이 이 장치의 설정공간을
 * 읽으면 하드웨어 대신 이 값이 돌아간다. */

	u32 uncor_status;
	/* [한국어] 시뮬레이션된 Correctable Error Status.
	 * 설정자: aer_inject 가 OR 로 누적, aer_inj_write_config 가 XOR 로 소거.
	 * 읽는 자: aer_inj_read_config.
	 * 동기화: inject_lock. */
	u32 cor_status;
	/* [한국어] 시뮬레이션된 헤더 로그 첫 워드. 상태와 달리 write-1-to-clear 가 아니라
	 * 일반 대입 대상이다. */
	u32 header_log0;
	/* [한국어] 헤더 로그 둘째 워드. */
	u32 header_log1;
	/* [한국어] 헤더 로그 셋째 워드. */
	u32 header_log2;
	/* [한국어] 헤더 로그 넷째 워드. */
	u32 header_log3;
	/* [한국어] 시뮬레이션된 Root Error Status. **루트 포트 레코드에서만** 의미가 있다.
	 * 설정자: aer_inject 가 규약대로 MULTI/FATAL/FIRST_FATAL 비트를 조립한다.
	 * 읽는 자: 커널 AER 핸들러가 이 값을 읽어 어떤 오류인지 판단한다.
	 * 동기화: inject_lock. */
	u32 root_status;
	/* [한국어] 시뮬레이션된 Error Source Identification.
	 * 설정자: correctable 은 하위 16비트, uncorrectable 은 상위 16비트에
	 * 오류를 낸 장치의 BDF 를 넣는다(규약이 정한 배치다).
	 * 읽는 자: 커널 AER 핸들러가 어느 장치가 오류를 냈는지 알아내는 근거.
	 * 동기화: inject_lock. */
	u32 source_id;
/* [한국어] 장치 하나당 이 레코드가 하나씩 목록에 들어간다. */
};

struct pci_bus_ops {
	/* [한국어] pci_bus_ops_list 목록의 연결 고리.
	 * 동기화: inject_lock. 다만 pci_bus_ops_pop 은 스스로 잠금을 잡는다. */
	struct list_head list;
	/* [한국어] 후킹한 버스.
	 * 설정자: pci_bus_ops_init.
	 * 읽는 자: __find_pci_bus_ops 의 대조, aer_inject_exit 의 복원. */
	struct pci_bus *bus;
	/* [한국어] 그 버스의 **원래** pci_ops.
	 * 설정자: pci_bus_set_aer_ops 가 pci_bus_set_ops 의 반환값을 넘긴다.
	 * 읽는 자: aer_inj_read/write 가 통과 경로에서 되돌려 쓰고,
	 * aer_inject_exit 가 영구 복원한다.
	 * 값 범위: 이 모듈의 aer_inj_pci_ops 가 아닌 진짜 컨트롤러 ops. */
	struct pci_ops *ops;
/* [한국어] 이 세 필드가 후킹을 되돌릴 수 있게 해 주는 전부다. */
};

static LIST_HEAD(einjected);
/* [한국어] 주입된 오류 레코드들의 전역 목록. 장치별로 하나씩 들어간다. */

static LIST_HEAD(pci_bus_ops_list);
/* [한국어] 후킹된 버스들의 원래 ops 를 보관하는 전역 목록. */

/* Protect einjected and pci_bus_ops_list */
static DEFINE_SPINLOCK(inject_lock);

/* [한국어]
 * aer_error_init - 주입된 오류 레코드의 식별 정보를 채운다
 *
 * @err: 초기화할 레코드.
 * @domain: PCI 도메인 번호.
 * @bus: 버스 번호.
 * @devfn: 장치/함수 번호.
 * @pos_cap_err: 그 장치의 AER 확장 능력 오프셋.
 * @return: 없음.
 *
 * 이 네 값이 레코드의 '주소' 다. __find_aer_error 가 앞의 셋으로 레코드를
 * 찾고, find_pci_config_dword 가 pos_cap_err 로 어느 레지스터에 대한
 * 접근인지 계산한다.
 *
 * 상태 필드(uncor_status 등)는 여기서 건드리지 않는다 -- 호출자가 이미
 * kzalloc 으로 0 을 채운 뒤 넘기고, 값은 aer_inject 가 나중에 OR 로 얹는다.
 *
 * 실행 컨텍스트: inject_lock 을 쥔 상태에서 불린다.
 *
 * 호출 체인:
 *   aer_inject → [이 함수]
 */
static void aer_error_init(struct aer_error *err, u32 domain,
			   unsigned int bus, unsigned int devfn,
			   int pos_cap_err)
{
	INIT_LIST_HEAD(&err->list);
	err->domain = domain;
	/* [한국어] 버스 번호를 담는다. */
	err->bus = bus;
	/* [한국어] 장치/함수 번호를 담는다. */
	err->devfn = devfn;
	/* [한국어] AER 능력 오프셋을 담는다. 이 값이 있어야 오프셋 계산이 성립한다. */
	err->pos_cap_err = pos_cap_err;
/* [한국어] 상태 필드는 여기서 건드리지 않는다 -- 호출자가 kzalloc 으로 0 을 채웠고,
 * 값은 aer_inject 가 나중에 얹는다. */
}

/* inject_lock must be held before calling */
/* [한국어]
 * __find_aer_error - 도메인/버스/devfn 으로 주입된 오류 레코드를 찾는다
 *
 * @domain, @bus, @devfn: 찾을 장치의 좌표.
 * @return: 레코드, 또는 없으면 NULL.
 *
 * 이름 앞의 밑줄 두 개는 **호출자가 inject_lock 을 이미 쥐고 있어야 한다**는
 * 관례적 표시다. einjected 목록을 잠금 없이 순회하면 동시에 진행되는
 * 주입이나 모듈 제거와 경쟁한다.
 *
 * 선형 탐색인 것은 주입된 장치 수가 실무상 한둘이기 때문이다 -- 이 모듈은
 * 디버깅/테스트용이라 성능이 문제되지 않는다.
 *
 * 실행 컨텍스트: inject_lock 아래. 설정 접근 후킹 경로에서도 불리므로
 * 인터럽트가 막힌 상태일 수 있다.
 *
 * 호출 체인:
 *   aer_inj_read_config / aer_inj_write_config / __find_aer_error_by_dev
 *     → [이 함수]
 */
static struct aer_error *__find_aer_error(u32 domain, unsigned int bus,
					  unsigned int devfn)
{
	struct aer_error *err;

	list_for_each_entry(err, &einjected, list) {
		/* [한국어] 도메인/버스/devfn 세 값이 모두 맞아야 같은 장치다. */
		if (domain == err->domain &&
		    /* [한국어] 버스 번호 대조. */
		    bus == err->bus &&
		    devfn == err->devfn)
			return err;
	}
	return NULL;
}

/* inject_lock must be held before calling */
/* [한국어]
 * __find_aer_error_by_dev - pci_dev 로부터 좌표를 뽑아 레코드를 찾는다
 *
 * @dev: 대상 장치.
 * @return: 레코드, 또는 도메인이 유효하지 않거나 못 찾으면 NULL.
 *
 * 위 함수의 편의 래퍼다. pci_domain_nr 이 음수를 돌려주면(도메인 정보가
 * 없는 경우) 찾을 좌표 자체가 성립하지 않으므로 NULL 로 끝낸다.
 *
 * domain 을 int 로 받아 음수를 검사한 뒤 u32 인자에 넘기는 형태라,
 * 음수 검사가 곧 형변환 안전성 확인 역할도 한다.
 *
 * 실행 컨텍스트: inject_lock 아래.
 *
 * 호출 체인:
 *   aer_inject → [이 함수] → __find_aer_error
 */
static struct aer_error *__find_aer_error_by_dev(struct pci_dev *dev)
{
	int domain = pci_domain_nr(dev->bus);
	if (domain < 0)
		/* [한국어] 목록 끝까지 못 찾았다 -- 이 장치에는 주입된 오류가 없다. */
		return NULL;
	return __find_aer_error(domain, dev->bus->number, dev->devfn);
/* [한국어] NULL 이면 호출자가 진짜 하드웨어 접근으로 넘어간다. */
}

/* inject_lock must be held before calling */
/* [한국어]
 * __find_pci_bus_ops - 후킹하기 전의 원래 pci_ops 를 찾아 돌려준다
 *
 * @bus: 대상 버스.
 * @return: 원래 ops, 또는 이 버스를 후킹한 적이 없으면 NULL.
 *
 * 이 모듈의 핵심 구조를 이해하는 열쇠다. aer_inject 는 대상 버스의
 * bus->ops 를 aer_inj_pci_ops 로 **바꿔치기**하고, 원래 것을
 * pci_bus_ops_list 에 보관한다. 그래야 시뮬레이션 대상이 아닌 레지스터
 * 접근을 진짜 하드웨어로 흘려보낼 수 있다.
 *
 * 이름의 밑줄 두 개는 inject_lock 전제를 뜻한다.
 *
 * 실행 컨텍스트: inject_lock 아래.
 *
 * 호출 체인:
 *   aer_inj_read / aer_inj_write → [이 함수]
 */
static struct pci_ops *__find_pci_bus_ops(struct pci_bus *bus)
{
	struct pci_bus_ops *bus_ops;

	list_for_each_entry(bus_ops, &pci_bus_ops_list, list) {
		/* [한국어] 버스 포인터 자체를 비교한다 -- 같은 버스 객체인지가 기준이다. */
		if (bus_ops->bus == bus)
			/* [한국어] 보관해 둔 원래 ops 를 돌려준다. */
			return bus_ops->ops;
	/* [한국어] 목록 끝까지 못 찾았다. */
	}
	return NULL;
}

/* [한국어]
 * pci_bus_ops_pop - 보관된 원래 ops 항목을 목록에서 하나 꺼낸다
 *
 * @return: 항목 하나, 또는 목록이 비었으면 NULL.
 *
 * 모듈 제거 시 후킹을 되돌리는 데 쓴다. **잠금을 스스로 잡는** 점이
 * __find_pci_bus_ops 와 다르다 -- 이름에 밑줄이 없는 것이 그 표시다.
 *
 * 꺼내면서 목록에서 빼기 때문에, 호출자는 NULL 이 나올 때까지 반복하면
 * 목록을 안전하게 비울 수 있다. 잠금을 반복 안에서 잡았다 놓는 구조라
 * 긴 목록도 인터럽트를 오래 막지 않는다.
 *
 * 실행 컨텍스트: 모듈 제거 시의 프로세스 문맥. 잠금을 스스로 관리한다.
 *
 * 호출 체인:
 *   aer_inject_exit → [이 함수]
 */
static struct pci_bus_ops *pci_bus_ops_pop(void)
{
	unsigned long flags;
	struct pci_bus_ops *bus_ops;
/* [한국어] 아래 목록 조작을 잠금으로 감싼다. */

	spin_lock_irqsave(&inject_lock, flags);
	/* [한국어] 목록이 비었으면 NULL 을 돌려주는 판이라, 별도 empty 검사가 필요 없다. */
	bus_ops = list_first_entry_or_null(&pci_bus_ops_list,
					   /* [한국어] 첫 항목의 타입과 연결 고리 필드명을 알려 준다. */
					   struct pci_bus_ops, list);
	if (bus_ops)
		/* [한국어] 꺼내면서 목록에서 뺀다. 그래야 호출자가 NULL 이 나올 때까지 반복해
		 * 목록을 비울 수 있다. */
		list_del(&bus_ops->list);
	spin_unlock_irqrestore(&inject_lock, flags);
	/* [한국어] 잠금을 반복 안에서 잡았다 놓으므로, 긴 목록도 인터럽트를 오래 막지 않는다. */
	return bus_ops;
}

/* [한국어]
 * find_pci_config_dword - 설정 오프셋이 어느 시뮬레이션 필드에 해당하는지 찾는다
 *
 * @err: 그 장치의 오류 레코드.
 * @where: 설정공간 오프셋(절대).
 * @prw1cs: NULL 이 아니면 그 필드가 write-1-to-clear 인지를 여기 담는다.
 * @return: 해당 필드의 주소, 또는 시뮬레이션 대상이 아니면 NULL.
 *
 * 이 모듈이 가로채는 것은 AER 능력 구조 안의 **일부 레지스터뿐**이다.
 * 나머지는 진짜 하드웨어로 넘긴다. 그 판별이 여기서 일어난다.
 *
 * `where - err->pos_cap_err` 로 능력 구조 기준의 상대 오프셋을 구한 뒤
 * switch 로 가른다. 헤더 로그 네 워드는 PCI_ERR_HEADER_LOG 에 4씩 더해
 * 구별한다.
 *
 * rw1cs 표시가 붙는 것은 상태 레지스터들(UNCOR_STATUS, COR_STATUS,
 * ROOT_STATUS)이다. 하드웨어에서 그것들이 write-1-to-clear 이므로,
 * 시뮬레이션도 같은 의미로 동작해야 한다 -- aer_inj_write_config 가 이
 * 표시를 보고 대입 대신 XOR 를 쓴다.
 *
 * pos_cap_err 이 -1 이면 그 장치에 AER 능력이 없다는 뜻이라 곧바로 NULL 이다.
 *
 * 실행 컨텍스트: inject_lock 아래.
 *
 * 호출 체인:
 *   aer_inj_read_config / aer_inj_write_config → [이 함수]
 */
static u32 *find_pci_config_dword(struct aer_error *err, int where,
				  int *prw1cs)
{
	int rw1cs = 0;
	u32 *target = NULL;
/* [한국어] 아래에서 이 접근이 시뮬레이션 대상인지 판별한다. */

	if (err->pos_cap_err == -1)
		/* [한국어] AER 능력이 없는 장치다 -- 상대 오프셋을 계산할 기준이 없다. */
		return NULL;

	switch (where - err->pos_cap_err) {
	/* [한국어] Uncorrectable Error Status. */
	case PCI_ERR_UNCOR_STATUS:
		/* [한국어] 해당 시뮬레이션 필드를 가리킨다. */
		target = &err->uncor_status;
		rw1cs = 1;
		/* [한국어] 이 레지스터는 write-1-to-clear 다. */
		break;
	case PCI_ERR_COR_STATUS:
		/* [한국어] Correctable Error Status 도 같은 성질이다. */
		target = &err->cor_status;
		rw1cs = 1;
		/* [한국어] rw1cs 표시를 붙인다. */
		break;
	case PCI_ERR_HEADER_LOG:
		/* [한국어] 헤더 로그 네 워드는 rw1cs 가 아니다 -- 일반 읽기/쓰기 레지스터다. */
		target = &err->header_log0;
		break;
	case PCI_ERR_HEADER_LOG+4:
		/* [한국어] 두 번째 워드는 +4. */
		target = &err->header_log1;
		break;
	case PCI_ERR_HEADER_LOG+8:
		/* [한국어] 세 번째 워드는 +8. */
		target = &err->header_log2;
		break;
	case PCI_ERR_HEADER_LOG+12:
		/* [한국어] 네 번째 워드는 +12. 워드당 4바이트씩 떨어진다. */
		target = &err->header_log3;
		break;
	case PCI_ERR_ROOT_STATUS:
		/* [한국어] Root Error Status. 루트 포트에서만 의미가 있지만, 여기서는 오프셋만
		 * 보고 판별하므로 장치를 가리지 않는다. */
		target = &err->root_status;
		rw1cs = 1;
		/* [한국어] 이것도 write-1-to-clear 다. */
		break;
	case PCI_ERR_ROOT_ERR_SRC:
		/* [한국어] Error Source ID 는 읽기 전용 성격이라 rw1cs 가 아니다. */
		target = &err->source_id;
		break;
	}
	if (prw1cs)
		*prw1cs = rw1cs;
	return target;
}

/* [한국어]
 * aer_inj_read - 원래 ops 로 잠시 되돌려 진짜 설정 읽기를 수행한다
 *
 * @bus, @devfn, @where, @size: 표준 설정 읽기 인자.
 * @val: 읽은 값을 담을 곳.
 * @return: 원래 ops 의 반환값, 또는 후킹 기록이 없으면 -1.
 *
 * 후킹의 '통과' 경로다. bus->ops 를 원래 것으로 **잠깐 되돌린 뒤** 호출하고
 * 곧바로 후킹 상태로 복원한다. 그냥 ops->read 를 직접 부르지 않고 이렇게
 * 하는 이유: 일부 컨트롤러 드라이버의 read 구현이 bus->ops 를 다시 참조하기
 * 때문에, 그 안에서 자기 자신(후킹 함수)이 다시 불리는 무한 재귀를 막아야
 * 한다.
 *
 * 반환값이 -1 인 것은 PCIBIOS_* 코드가 아니다(상류 그대로). 호출자
 * aer_inj_read_config 가 그 값을 그대로 위로 올린다.
 *
 * 실행 컨텍스트: inject_lock 아래. bus->ops 를 바꿨다 되돌리는 구간이
 * 그 잠금으로 보호된다.
 *
 * 호출 체인:
 *   aer_inj_read_config → [이 함수] → __find_pci_bus_ops → 원래 ops->read
 */
static int aer_inj_read(struct pci_bus *bus, unsigned int devfn, int where,
			int size, u32 *val)
{
	struct pci_ops *ops, *my_ops;
	int rv;
/* [한국어] 아래에서 원래 ops 를 찾는다. */

	ops = __find_pci_bus_ops(bus);
	/* [한국어] 이 버스를 후킹한 기록이 없다 -- 호출 경로상 있을 수 없는 상황이다. */
	if (!ops)
		/* [한국어] PCIBIOS_* 코드가 아닌 -1 을 돌려준다(상류 그대로). */
		return -1;
/* [한국어] 이제 ops 를 잠시 바꿔치기한다. */

	my_ops = bus->ops;
	/* [한국어] 원래 ops 로 되돌린다. 그냥 ops->read 를 직접 부르지 않는 이유는,
	 * 일부 컨트롤러 구현이 bus->ops 를 다시 참조해 무한 재귀에 빠질 수 있기
	 * 때문이다. */
	bus->ops = ops;
	/* [한국어] 이제 진짜 하드웨어 접근이 일어난다. */
	rv = ops->read(bus, devfn, where, size, val);
	/* [한국어] 곧바로 후킹 상태로 복원한다. 이 구간이 inject_lock 으로 보호된다. */
	bus->ops = my_ops;
/* [한국어] 원래 ops 의 결과를 그대로 돌려준다. */

	return rv;
}

/* [한국어]
 * aer_inj_write - 원래 ops 로 잠시 되돌려 진짜 설정 쓰기를 수행한다
 *
 * @bus, @devfn, @where, @size, @val: 표준 설정 쓰기 인자.
 * @return: 원래 ops 의 반환값, 또는 후킹 기록이 없으면 -1.
 *
 * aer_inj_read 와 대칭이며, 같은 재귀 방지 이유로 bus->ops 를 잠시 되돌린다.
 *
 * 실행 컨텍스트: inject_lock 아래.
 *
 * 호출 체인:
 *   aer_inj_write_config → [이 함수] → __find_pci_bus_ops → 원래 ops->write
 */
static int aer_inj_write(struct pci_bus *bus, unsigned int devfn, int where,
			 int size, u32 val)
{
	struct pci_ops *ops, *my_ops;
	int rv;
/* [한국어] 쓰기 쪽도 같은 절차다. */

	ops = __find_pci_bus_ops(bus);
	/* [한국어] 후킹 기록이 없다. */
	if (!ops)
		/* [한국어] 읽기 쪽과 같은 -1. */
		return -1;
/* [한국어] ops 를 잠시 바꿔치기한다. */

	my_ops = bus->ops;
	/* [한국어] 원래 ops 로 되돌린다. */
	bus->ops = ops;
	/* [한국어] 진짜 하드웨어 쓰기. */
	rv = ops->write(bus, devfn, where, size, val);
	/* [한국어] 후킹 상태로 복원한다. */
	bus->ops = my_ops;
/* [한국어] 결과를 그대로 돌려준다. */

	return rv;
}

/* [한국어]
 * aer_inj_read_config - 후킹된 설정 읽기: 시뮬레이션 값이 있으면 그것을 준다
 *
 * @bus, @devfn, @where, @size: 표준 설정 읽기 인자.
 * @val: 읽은 값을 담을 곳.
 * @return: 0(시뮬레이션 값 반환) 또는 진짜 읽기의 반환값.
 *
 * 이 함수가 AER 오류를 '있는 것처럼' 보이게 만드는 지점이다. 커널의 AER
 * 처리 코드가 상태 레지스터를 읽으면, 하드웨어 대신 주입된 값이 돌아온다.
 *
 * 통과(goto out) 조건이 셋이다:
 *  - size 가 4바이트가 아니다. 시뮬레이션 필드가 모두 u32 라 부분 접근을
 *    다루지 않는다.
 *  - 도메인 번호가 유효하지 않다.
 *  - 이 장치에 주입된 오류가 없다.
 * 그리고 오프셋이 시뮬레이션 대상이 아닐 때(sim == NULL)도 통과한다.
 *
 * 잠금 처리에 유의: 시뮬레이션 경로는 잠금을 풀고 **바로 반환**하고,
 * 통과 경로는 out 라벨에서 진짜 읽기를 한 뒤 푼다. 즉 진짜 하드웨어
 * 접근도 inject_lock 을 쥔 채 일어난다 -- aer_inj_read 가 bus->ops 를
 * 바꿨다 되돌리는 구간을 보호해야 하기 때문이다.
 *
 * 실행 컨텍스트: 설정 접근 경로. spin_lock_irqsave 로 인터럽트를 막는다.
 *
 * 호출 체인:
 *   pci_read_config_dword → bus->ops->read → [이 함수]
 *     → find_pci_config_dword → aer_inj_read
 */
static int aer_inj_read_config(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 *val)
{
	u32 *sim;
	struct aer_error *err;
	/* [한국어] spin_lock_irqsave 가 저장할 인터럽트 상태. */
	unsigned long flags;
	/* [한국어] pci_domain_nr 의 결과를 담을 변수. 음수 검사를 위해 int 다. */
	int domain;
	/* [한국어] 통과 경로의 반환값. */
	int rv;
/* [한국어] 아래 전체가 잠금 아래에서 일어난다. */

	spin_lock_irqsave(&inject_lock, flags);
	/* [한국어] 시뮬레이션 필드가 모두 u32 라 부분 접근은 다루지 않는다. */
	if (size != sizeof(u32))
		/* [한국어] 진짜 하드웨어로 넘긴다. */
		goto out;
	domain = pci_domain_nr(bus);
	/* [한국어] 도메인 번호가 유효하지 않다. */
	if (domain < 0)
		/* [한국어] 찾을 좌표가 성립하지 않으므로 통과시킨다. */
		goto out;
	err = __find_aer_error(domain, bus->number, devfn);
	/* [한국어] 이 장치에 주입된 오류가 없다. */
	if (!err)
		/* [한국어] 통과. */
		goto out;

	sim = find_pci_config_dword(err, where, NULL);
	/* [한국어] 이 오프셋이 시뮬레이션 대상이다. */
	if (sim) {
		*val = *sim;
		spin_unlock_irqrestore(&inject_lock, flags);
		return 0;
	}
out:
	rv = aer_inj_read(bus, devfn, where, size, val);
	spin_unlock_irqrestore(&inject_lock, flags);
	/* [한국어] 통과 경로의 결과. 진짜 하드웨어 접근도 잠금을 쥔 채 일어나는데,
	 * aer_inj_read 가 bus->ops 를 바꿨다 되돌리는 구간을 보호해야 하기 때문이다. */
	return rv;
}

/* [한국어]
 * aer_inj_write_config - 후킹된 설정 쓰기: 시뮬레이션 필드면 write-1-to-clear 를 흉내 낸다
 *
 * @bus, @devfn, @where, @size, @val: 표준 설정 쓰기 인자.
 * @return: 0(시뮬레이션 처리) 또는 진짜 쓰기의 반환값.
 *
 * 읽기 쪽과 통과 조건이 같다. 다른 점은 시뮬레이션 필드에 값을 넣는 방식이다:
 *  - rw1cs 인 필드(상태 레지스터들): `*sim ^= val` -- **XOR** 를 쓴다.
 *    write-1-to-clear 는 '1 을 쓴 비트를 지운다' 는 뜻인데, 커널이 읽은
 *    값을 그대로 되쓰는 관용구를 쓰므로 XOR 가 그 결과와 같아진다
 *    (세워진 비트에 1 을 쓰면 0 이 된다).
 *    다만 서 있지 않은 비트에 1 을 쓰면 XOR 는 그것을 **세운다** --
 *    실제 하드웨어의 write-1-to-clear 는 무시하는 경우다. 커널이 읽은
 *    값만 되쓰는 한 차이가 드러나지 않는다.
 *  - 그 외 필드(헤더 로그, source id): 그냥 대입한다.
 *
 * 실행 컨텍스트: 설정 접근 경로. 인터럽트를 막는다.
 *
 * 호출 체인:
 *   pci_write_config_dword → bus->ops->write → [이 함수]
 *     → find_pci_config_dword → aer_inj_write
 */
static int aer_inj_write_config(struct pci_bus *bus, unsigned int devfn,
				int where, int size, u32 val)
{
	u32 *sim;
	struct aer_error *err;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 이 필드가 write-1-to-clear 인지를 받을 변수. */
	int rw1cs;
	/* [한국어] 도메인 번호. */
	int domain;
	/* [한국어] 통과 경로의 반환값. */
	int rv;
/* [한국어] 아래 전체가 잠금 아래다. */

	spin_lock_irqsave(&inject_lock, flags);
	/* [한국어] 읽기 쪽과 같은 통과 조건들. */
	if (size != sizeof(u32))
		/* [한국어] 진짜 하드웨어로 넘긴다. */
		goto out;
	domain = pci_domain_nr(bus);
	/* [한국어] 도메인이 유효하지 않다. */
	if (domain < 0)
		/* [한국어] 통과. */
		goto out;
	err = __find_aer_error(domain, bus->number, devfn);
	/* [한국어] 주입된 오류가 없다. */
	if (!err)
		/* [한국어] 통과. */
		goto out;

	sim = find_pci_config_dword(err, where, &rw1cs);
	/* [한국어] 시뮬레이션 대상이다. 아래에서 rw1cs 여부로 갈린다. */
	if (sim) {
		/* [한국어] write-1-to-clear 필드다. */
		if (rw1cs)
			*sim ^= val;
		else
			*sim = val;
		spin_unlock_irqrestore(&inject_lock, flags);
		return 0;
	}
out:
	rv = aer_inj_write(bus, devfn, where, size, val);
	spin_unlock_irqrestore(&inject_lock, flags);
	/* [한국어] 통과 경로의 결과. */
	return rv;
/* [한국어] 이 두 함수가 곧 후킹의 실체다. */
}

static struct pci_ops aer_inj_pci_ops = {
	/* [한국어] 읽기 후킹. */
	.read = aer_inj_read_config,
	/* [한국어] 쓰기 후킹. 이 표가 대상 버스의 bus->ops 를 대체한다. */
	.write = aer_inj_write_config,
};

/* [한국어]
 * pci_bus_ops_init - 원래 ops 보관 항목을 채운다
 *
 * @bus_ops: 초기화할 항목.
 * @bus: 후킹한 버스.
 * @ops: 그 버스의 원래 ops.
 * @return: 없음.
 *
 * 목록 노드와 두 포인터를 채우는 것이 전부다. 잠금은 호출자가 쥔다.
 *
 * 실행 컨텍스트: inject_lock 아래.
 *
 * 호출 체인:
 *   pci_bus_set_aer_ops → [이 함수]
 */
static void pci_bus_ops_init(struct pci_bus_ops *bus_ops,
			     struct pci_bus *bus,
			     struct pci_ops *ops)
{
	INIT_LIST_HEAD(&bus_ops->list);
	bus_ops->bus = bus;
	/* [한국어] 원래 ops 를 보관한다. 이 값이 있어야 복원과 통과가 가능하다. */
	bus_ops->ops = ops;
}

/* [한국어]
 * pci_bus_set_aer_ops - 버스의 pci_ops 를 후킹하고 원래 것을 보관한다
 *
 * @bus: 후킹할 버스.
 * @return: 0 성공, -ENOMEM 할당 실패.
 *
 * 할당을 **잠금 밖에서 미리** 하는 것이 이 함수의 형태를 결정한다.
 * spin_lock 아래에서는 GFP_KERNEL 할당을 할 수 없기 때문이다. 그래서
 * 먼저 잡아 두고, 필요 없으면 마지막에 kfree 한다.
 *
 * pci_bus_set_ops 도 잠금 **밖**에서 부른다. 그 함수가 돌려주는 값이
 * 이미 aer_inj_pci_ops 이면 이 버스는 앞선 주입에서 이미 후킹된 것이므로,
 * 목록에 중복으로 넣지 않고 out 으로 빠진다 -- 그러면 bus_ops 가 NULL 이
 * 아닌 채 남아 아래 kfree 가 해제한다.
 *
 * 반대로 새로 후킹한 경우에는 목록에 넣은 뒤 bus_ops 를 NULL 로 만들어,
 * 같은 kfree 가 아무 일도 하지 않게 한다. 한 줄의 kfree 로 두 경우를
 * 모두 처리하는 관용구다.
 *
 * 실행 컨텍스트: 주입 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   aer_inject → [이 함수] → pci_bus_set_ops → pci_bus_ops_init
 */
static int pci_bus_set_aer_ops(struct pci_bus *bus)
{
	struct pci_ops *ops;
	struct pci_bus_ops *bus_ops;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
/* [한국어] 아래 할당을 **잠금 밖에서 미리** 한다 -- 스핀락 아래에서는 GFP_KERNEL
 * 할당을 할 수 없기 때문이다. */

	bus_ops = kmalloc_obj(*bus_ops);
	/* [한국어] 할당 실패. */
	if (!bus_ops)
		/* [한국어] 아직 후킹하지 않았으므로 되감을 것이 없다. */
		return -ENOMEM;
	ops = pci_bus_set_ops(bus, &aer_inj_pci_ops);
	/* [한국어] pci_bus_set_ops 도 잠금 밖에서 부른 뒤 여기서 잠근다. */
	spin_lock_irqsave(&inject_lock, flags);
	/* [한국어] 반환값이 이미 이 모듈의 ops 라면, 이 버스는 앞선 주입에서 후킹된 것이다. */
	if (ops == &aer_inj_pci_ops)
		/* [한국어] 목록에 중복으로 넣지 않고 빠진다 -- bus_ops 가 NULL 이 아닌 채 남아
		 * 아래 kfree 가 해제한다. */
		goto out;
	pci_bus_ops_init(bus_ops, bus, ops);
	/* [한국어] 새로 후킹한 경우에만 목록에 넣는다. */
	list_add(&bus_ops->list, &pci_bus_ops_list);
	/* [한국어] NULL 로 만들어 아래 kfree 가 아무 일도 하지 않게 한다. 한 줄의 kfree 로
	 * 두 경우를 모두 처리하는 관용구다. */
	bus_ops = NULL;
/* [한국어] 두 경로가 여기서 만난다. */
out:
	spin_unlock_irqrestore(&inject_lock, flags);
	kfree(bus_ops);
	return 0;
}

/* [한국어]
 * aer_inject - 사용자가 요청한 AER 오류를 시뮬레이션 상태로 심고 인터럽트를 쏜다
 *
 * @einj: 사용자 공간에서 복사해 온 주입 요청.
 * @return: 0 성공, 음수는 각 단계의 실패값.
 *
 * 이 모듈의 본체다. 실제 하드웨어 오류를 만들지 않고, 커널의 AER 처리
 * 코드가 **오류가 난 것처럼 보이게** 만든다. 그 방법이 셋으로 나뉜다:
 *  (1) 상태 레지스터 값을 시뮬레이션 레코드에 심는다.
 *  (2) 대상 버스의 pci_ops 를 후킹해, 그 레지스터를 읽으면 심은 값이
 *      돌아오게 한다.
 *  (3) 루트 포트의 AER 인터럽트를 소프트웨어로 쏜다.
 *
 * 단계별 요점:
 *
 * - 대상 장치와 그 위의 루트 포트를 찾는다. 루트 포트가 없으면 RCEC
 *   (Root Complex Event Collector)를 대신 쓴다 -- RCiEP 는 루트 포트
 *   아래가 아니라 RCEC 에 오류를 보고하기 때문이다.
 * - 두 장치 모두 AER 능력이 있어야 한다. 없으면 -EPROTONOSUPPORT.
 * - **할당을 잠금 밖에서 미리 한다**(err_alloc, rperr_alloc). 아래 목록
 *   조작이 스핀락 아래라 그 안에서 GFP_KERNEL 할당을 할 수 없기 때문이다.
 *   쓰이지 않은 쪽은 out_put 의 kfree 가 해제한다 -- 쓰인 쪽은 NULL 로
 *   만들어 두므로 같은 kfree 가 두 경우를 모두 처리한다.
 * - aer_mask_override 모듈 파라미터가 켜져 있으면 마스크를 잠시 풀어 둔다.
 *   원래 값을 보관했다가 아래에서 되돌린다.
 * - 잠금 안에서: 대상 장치의 레코드를 찾거나 만들고 상태를 얹는다.
 *   상태는 **OR** 로 누적하고(여러 번 주입할 수 있다) 헤더 로그는 덮어쓴다.
 * - 마스크 검사: override 가 아닌데 요청한 오류가 전부 마스크되어 있으면
 *   인터럽트가 나가지 않으므로 -EINVAL 로 거절한다.
 * - 루트 포트 쪽 레코드에는 Root Error Status 를 조립한다. 규약대로
 *   '이미 받은 적이 있으면 MULTI 비트' 를 세우고, 심각도(sever)에 따라
 *   FATAL/NONFATAL 을 가른다. source_id 는 correctable 이 하위 16비트,
 *   uncorrectable 이 상위 16비트를 쓴다.
 * - 두 버스의 ops 를 후킹한다. 대상 장치와 루트 포트가 다른 버스에 있을 수
 *   있어 둘 다 필요하다.
 * - 마지막으로 루트 포트의 AER 서비스 장치를 찾아 그 IRQ 를 소프트웨어로
 *   쏜다. 그러면 커널의 AER 핸들러가 깨어나 위에서 심은 값을 읽는다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): 마스크를 푸는 두 줄이
 * `cor_mask &= !(einj->cor_status)` 로, 비트 반전 `~` 이 아니라 **논리 부정
 * `!`** 을 쓴다. `!x` 는 0 또는 1 이므로 실제 동작은 이렇다 --
 * cor_status 가 0 이 아니면(=주입할 때) 마스크가 통째로 0 이 되어 모든
 * 오류가 언마스크되고, 0 이면 마스크의 비트 0 만 남는다. 앞의 경우가
 * 이 코드의 의도(마스크 무시)와 결과적으로 맞아떨어져 기능은 한다.
 * uncor_mask 쪽도 같다.
 *
 * 실행 컨텍스트: /dev/aer_inject 쓰기의 프로세스 문맥. 목록 조작 구간만
 * spin_lock_irqsave 로 보호된다.
 *
 * 호출 체인:
 *   aer_inject_write → [이 함수] → pci_get_domain_bus_and_slot
 *     → pcie_find_root_port → pci_bus_set_aer_ops
 *       → pcie_port_find_device → irq_inject_interrupt
 */
static int aer_inject(struct aer_error_inj *einj)
{
	struct aer_error *err, *rperr;
	struct aer_error *err_alloc = NULL, *rperr_alloc = NULL;
	/* [한국어] dev 는 오류를 낼 장치, rpdev 는 그 오류를 받을 루트 포트(또는 RCEC). */
	struct pci_dev *dev, *rpdev;
	/* [한국어] AER 서비스 장치. 인터럽트 번호를 여기서 얻는다. */
	struct pcie_device *edev;
	/* [한국어] pcie_port_find_device 의 반환값. */
	struct device *device;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 장치 번호와 함수 번호를 devfn 한 벌로 합친다. */
	unsigned int devfn = PCI_DEVFN(einj->dev, einj->fn);
	/* [한국어] 두 장치의 AER 능력 오프셋. */
	int pos_cap_err, rp_pos_cap_err;
	/* [한국어] sever 는 심각도 레지스터, 두 mask 는 현재 마스크, _orig 는 override 시
	 * 되돌릴 원래 값이다. 0 으로 초기화해 두어 override 가 꺼져 있을 때도
	 * 정의된 값을 갖는다. */
	u32 sever, cor_mask, uncor_mask, cor_mask_orig = 0, uncor_mask_orig = 0;
	/* [한국어] 단계별 결과. 0 으로 시작해 실패 시에만 바뀐다. */
	int ret = 0;
/* [한국어] 아래에서 대상 장치를 찾는다. */

	dev = pci_get_domain_bus_and_slot(einj->domain, einj->bus, devfn);
	/* [한국어] 그 좌표에 장치가 없다. */
	if (!dev)
		/* [한국어] 아직 아무것도 잡지 않았으므로 라벨 없이 반환한다. */
		return -ENODEV;
	rpdev = pcie_find_root_port(dev);
	/* If Root Port not found, try to find an RCEC */
	if (!rpdev)
		rpdev = dev->rcec;
	/* [한국어] 루트 포트도 RCEC 도 없다 -- 오류를 받을 상대가 없다는 뜻이다. */
	if (!rpdev) {
		/* [한국어] 두 경우를 한 메시지로 묶어 알린다. */
		pci_err(dev, "Neither Root Port nor RCEC found\n");
		/* [한국어] 장치가 없다는 코드. */
		ret = -ENODEV;
		goto out_put;
	}

	pos_cap_err = dev->aer_cap;
	/* [한국어] 대상 장치에 AER 능력이 없다. */
	if (!pos_cap_err) {
		/* [한국어] AER 오류를 시뮬레이션할 레지스터 자체가 없다. */
		pci_err(dev, "Device doesn't support AER\n");
		/* [한국어] 프로토콜 미지원 코드. */
		ret = -EPROTONOSUPPORT;
		goto out_put;
	}
	pci_read_config_dword(dev, pos_cap_err + PCI_ERR_UNCOR_SEVER, &sever);
	/* [한국어] 현재 correctable 마스크를 읽어 둔다. 아래 마스크 검사와 override 복원에 쓴다. */
	pci_read_config_dword(dev, pos_cap_err + PCI_ERR_COR_MASK, &cor_mask);
	/* [한국어] uncorrectable 마스크도 마찬가지다. */
	pci_read_config_dword(dev, pos_cap_err + PCI_ERR_UNCOR_MASK,
			      /* [한국어] 두 마스크가 준비됐다. */
			      &uncor_mask);

	rp_pos_cap_err = rpdev->aer_cap;
	/* [한국어] 루트 포트에 AER 능력이 없다. */
	if (!rp_pos_cap_err) {
		/* [한국어] 루트 포트가 오류를 받을 수 없으면 주입이 무의미하다. */
		pci_err(rpdev, "Root port doesn't support AER\n");
		/* [한국어] 프로토콜 미지원 코드. */
		ret = -EPROTONOSUPPORT;
		goto out_put;
	}

	err_alloc =  kzalloc_obj(struct aer_error);
	/* [한국어] 대상 장치용 레코드 할당 실패. */
	if (!err_alloc) {
		/* [한국어] 메모리 부족. */
		ret = -ENOMEM;
		goto out_put;
	}
	rperr_alloc =  kzalloc_obj(struct aer_error);
	/* [한국어] 루트 포트용 레코드 할당 실패. */
	if (!rperr_alloc) {
		/* [한국어] 메모리 부족. 위에서 잡은 err_alloc 은 out_put 의 kfree 가 해제한다. */
		ret = -ENOMEM;
		goto out_put;
	}

	if (aer_mask_override) {
		/* [한국어] 원래 마스크를 보관해 둔다. 아래에서 되돌린다. */
		cor_mask_orig = cor_mask;
		/* [한국어] 코드 관찰 (상류 그대로, 수정하지 않음): 비트 반전 `~` 이 아니라 논리
		 * 부정 `!` 이다. `!x` 는 0 또는 1 이므로, cor_status 가 0 이 아니면(주입할 때)
		 * 마스크가 통째로 0 이 되어 모든 오류가 언마스크되고, 0 이면 비트 0 만 남는다.
		 * 앞의 경우가 이 코드의 의도(마스크 무시)와 결과적으로 맞아떨어진다. */
		cor_mask &= !(einj->cor_status);
		/* [한국어] 바뀐 마스크를 하드웨어에 반영한다. */
		pci_write_config_dword(dev, pos_cap_err + PCI_ERR_COR_MASK,
				       /* [한국어] 이제 마스크된 오류도 인터럽트를 낸다. */
				       cor_mask);

		uncor_mask_orig = uncor_mask;
		/* [한국어] uncorrectable 쪽도 같은 `!` 를 쓴다. */
		uncor_mask &= !(einj->uncor_status);
		/* [한국어] 바뀐 마스크를 반영한다. */
		pci_write_config_dword(dev, pos_cap_err + PCI_ERR_UNCOR_MASK,
				       /* [한국어] 두 마스크가 모두 풀렸다. */
				       uncor_mask);
	}

	spin_lock_irqsave(&inject_lock, flags);
/* [한국어] 여기부터 목록 조작이라 잠금이 필요하다. */

	err = __find_aer_error_by_dev(dev);
	/* [한국어] 이 장치에 이미 주입된 레코드가 있는지 본다. */
	if (!err) {
		/* [한국어] 없으면 미리 잡아 둔 것을 쓴다. */
		err = err_alloc;
		err_alloc = NULL;
		/* [한국어] 식별 정보를 채운다. */
		aer_error_init(err, einj->domain, einj->bus, devfn,
			       /* [한국어] AER 능력 오프셋이 여기서 레코드에 들어간다. */
			       pos_cap_err);
		list_add(&err->list, &einjected);
	/* [한국어] 이제 err 은 새 레코드이거나 기존 레코드다. */
	}
	err->uncor_status |= einj->uncor_status;
	/* [한국어] correctable 상태도 **OR 로 누적**한다 -- 여러 번 주입할 수 있다. */
	err->cor_status |= einj->cor_status;
	/* [한국어] 헤더 로그는 반대로 **덮어쓴다**. 마지막 주입의 헤더만 의미가 있기 때문이다. */
	err->header_log0 = einj->header_log0;
	/* [한국어] 둘째 워드. */
	err->header_log1 = einj->header_log1;
	/* [한국어] 셋째 워드. */
	err->header_log2 = einj->header_log2;
	/* [한국어] 넷째 워드. */
	err->header_log3 = einj->header_log3;
/* [한국어] 아래 두 검사가 '마스크되어 인터럽트가 안 날 오류' 를 미리 거른다. */

	if (!aer_mask_override && einj->cor_status &&
	    /* [한국어] 요청한 correctable 오류가 **전부** 마스크되어 있으면 인터럽트가 나가지
	     * 않는다. `~cor_mask` 와 AND 해서 하나라도 열려 있는지 본다. */
	    !(einj->cor_status & ~cor_mask)) {
		ret = -EINVAL;
		pci_warn(dev, "The correctable error(s) is masked by device\n");
		/* [한국어] 반환 전에 반드시 잠금을 푼다. */
		spin_unlock_irqrestore(&inject_lock, flags);
		/* [한국어] 레코드는 이미 목록에 들어갔지만 그대로 둔다 -- 다음 주입이 재사용한다. */
		goto out_put;
	}
	if (!aer_mask_override && einj->uncor_status &&
	    /* [한국어] uncorrectable 쪽도 같은 검사. */
	    !(einj->uncor_status & ~uncor_mask)) {
		ret = -EINVAL;
		pci_warn(dev, "The uncorrectable error(s) is masked by device\n");
		/* [한국어] 잠금 해제. */
		spin_unlock_irqrestore(&inject_lock, flags);
		/* [한국어] 같은 이유로 레코드를 남긴 채 나간다. */
		goto out_put;
	}

	rperr = __find_aer_error_by_dev(rpdev);
	/* [한국어] 루트 포트 레코드도 없으면 만든다. */
	if (!rperr) {
		/* [한국어] 미리 잡아 둔 것을 쓴다. */
		rperr = rperr_alloc;
		/* [한국어] NULL 로 만들어 out_put 의 kfree 가 두 번 해제하지 않게 한다. */
		rperr_alloc = NULL;
		/* [한국어] 루트 포트의 좌표는 einj 가 아니라 rpdev 에서 직접 얻는다. */
		aer_error_init(rperr, pci_domain_nr(rpdev->bus),
			       /* [한국어] 대상 장치와 다른 버스에 있을 수 있기 때문이다. */
			       rpdev->bus->number, rpdev->devfn,
			       rp_pos_cap_err);
		list_add(&rperr->list, &einjected);
	/* [한국어] 이제 rperr 이 준비됐다. */
	}
	if (einj->cor_status) {
		/* [한국어] 이미 correctable 오류를 받은 적이 있으면 MULTI 비트를 세운다 -- 규약이
		 * 정한 동작이다. */
		if (rperr->root_status & PCI_ERR_ROOT_COR_RCV)
			/* [한국어] 두 번째 이후는 MULTI 로 표시된다. */
			rperr->root_status |= PCI_ERR_ROOT_MULTI_COR_RCV;
		/* [한국어] 처음이면 아래에서 일반 수신 비트만 세운다. */
		else
			rperr->root_status |= PCI_ERR_ROOT_COR_RCV;
		/* [한국어] correctable 은 source_id 의 **하위 16비트**를 쓴다. 상위는 보존한다. */
		rperr->source_id &= 0xffff0000;
		/* [한국어] 오류를 낸 장치의 BDF 를 넣는다. */
		rperr->source_id |= PCI_DEVID(einj->bus, devfn);
	/* [한국어] correctable 조립 끝. */
	}
	if (einj->uncor_status) {
		/* [한국어] uncorrectable 도 같은 MULTI 규칙. */
		if (rperr->root_status & PCI_ERR_ROOT_UNCOR_RCV)
			/* [한국어] 두 번째 이후는 MULTI. */
			rperr->root_status |= PCI_ERR_ROOT_MULTI_UNCOR_RCV;
		/* [한국어] 심각도 레지스터에서 이 오류가 fatal 로 분류되는지 본다. */
		if (sever & einj->uncor_status) {
			/* [한국어] fatal 수신 비트를 세운다. */
			rperr->root_status |= PCI_ERR_ROOT_FATAL_RCV;
			/* [한국어] 이번이 **첫** uncorrectable 이면 FIRST_FATAL 도 세운다 -- 아래에서
			 * UNCOR_RCV 를 세우기 전에 검사해야 하므로 순서가 중요하다. */
			if (!(rperr->root_status & PCI_ERR_ROOT_UNCOR_RCV))
				/* [한국어] 규약이 정한 '첫 fatal' 표시다. */
				rperr->root_status |= PCI_ERR_ROOT_FIRST_FATAL;
		/* [한국어] fatal 이 아니면 nonfatal 로 분류한다. */
		} else
			/* [한국어] nonfatal 수신 비트. */
			rperr->root_status |= PCI_ERR_ROOT_NONFATAL_RCV;
		/* [한국어] **마지막에** 일반 수신 비트를 세운다. 위 FIRST_FATAL 검사가 이 비트를
		 * 보기 때문에 순서를 바꿀 수 없다. */
		rperr->root_status |= PCI_ERR_ROOT_UNCOR_RCV;
		/* [한국어] uncorrectable 은 source_id 의 **상위 16비트**를 쓴다. */
		rperr->source_id &= 0x0000ffff;
		/* [한국어] BDF 를 16비트 밀어 넣는다. */
		rperr->source_id |= PCI_DEVID(einj->bus, devfn) << 16;
	/* [한국어] 루트 포트 상태 조립 끝. */
	}
	spin_unlock_irqrestore(&inject_lock, flags);
/* [한국어] 이제 잠금 밖에서 마스크를 되돌린다. */

	if (aer_mask_override) {
		/* [한국어] 보관해 둔 원래 correctable 마스크를 복원한다. */
		pci_write_config_dword(dev, pos_cap_err + PCI_ERR_COR_MASK,
				       /* [한국어] 주입은 이미 레코드에 심었으므로 마스크를 되돌려도 무방하다. */
				       cor_mask_orig);
		pci_write_config_dword(dev, pos_cap_err + PCI_ERR_UNCOR_MASK,
				       /* [한국어] uncorrectable 마스크도 복원한다. */
				       uncor_mask_orig);
	}

	ret = pci_bus_set_aer_ops(dev->bus);
	/* [한국어] 대상 장치 버스 후킹 실패. */
	if (ret)
		/* [한국어] 메모리 부족뿐이다. */
		goto out_put;
	ret = pci_bus_set_aer_ops(rpdev->bus);
	/* [한국어] 루트 포트 버스 후킹 실패. **두 버스가 다를 수 있어** 둘 다 후킹한다. */
	if (ret)
		/* [한국어] 메모리 부족. */
		goto out_put;

	device = pcie_port_find_device(rpdev, PCIE_PORT_SERVICE_AER);
	/* [한국어] AER 서비스 장치를 찾았다. */
	if (device) {
		/* [한국어] pcie_device 로 변환해 irq 를 얻는다. */
		edev = to_pcie_device(device);
		/* [한국어] 서비스 데이터가 없으면 AER 드라이버가 아직 초기화되지 않았다는 뜻이다. */
		if (!get_service_data(edev)) {
			/* [한국어] 인터럽트를 쏴도 처리할 주체가 없다. */
			pci_warn(edev->port, "AER service is not initialized\n");
			/* [한국어] 프로토콜 미지원 코드. */
			ret = -EPROTONOSUPPORT;
			goto out_put;
		}
		pci_info(edev->port, "Injecting errors %08x/%08x into device %s\n",
			 /* [한국어] 어떤 오류를 어느 장치에 주입하는지 남긴다. */
			 einj->cor_status, einj->uncor_status, pci_name(dev));
		ret = irq_inject_interrupt(edev->irq);
	/* [한국어] 소프트웨어로 인터럽트를 쏜다 -- 이 호출이 커널 AER 핸들러를 깨우고,
	 * 그 핸들러가 위에서 심은 시뮬레이션 값을 읽는다. */
	} else {
		pci_err(rpdev, "AER device not found\n");
		/* [한국어] AER 서비스 장치가 없다. */
		ret = -ENODEV;
	}
out_put:
	kfree(err_alloc);
	kfree(rperr_alloc);
	pci_dev_put(dev);
	return ret;
}

/* [한국어]
 * aer_inject_write - /dev/aer_inject 쓰기 진입점
 *
 * @filp: 파일 핸들. 쓰지 않는다.
 * @ubuf: 사용자 공간의 요청 버퍼.
 * @usize: 그 크기.
 * @off: 파일 오프셋. 쓰지 않는다(이 장치는 스트림이 아니다).
 * @return: 성공 시 usize, 실패 시 음수 오류.
 *
 * 세 겹의 검사가 있다:
 *  1. CAP_SYS_ADMIN -- 임의의 장치에 오류를 심고 커널 오류 처리 경로를
 *     실행시키는 기능이라, 특권이 필요하다.
 *  2. 크기 하한: **offsetof(struct aer_error_inj, domain)**. 즉 domain
 *     필드는 없어도 된다. 그 필드가 나중에 추가되어, 그 이전 도구가
 *     보내는 짧은 구조체도 받아 주기 위한 호환 장치다.
 *  3. 크기 상한: 구조체 크기. 더 크면 거절한다.
 *
 * memset 으로 0 을 채운 뒤 복사하므로, 짧은 쓰기에서 빠진 domain 은
 * 0(기본 도메인)이 된다.
 *
 * 성공 시 usize 를 돌려주는 것은 write(2) 규약이다 -- 요청한 만큼 모두
 * 처리했다는 뜻이다.
 *
 * 실행 컨텍스트: 시스템 호출의 프로세스 문맥.
 *
 * 호출 체인:
 *   write(2) → fops.write → [이 함수] → copy_from_user → aer_inject
 */
static ssize_t aer_inject_write(struct file *filp, const char __user *ubuf,
				size_t usize, loff_t *off)
{
	struct aer_error_inj einj;
	int ret;
/* [한국어] 아래 세 겹의 검사가 사용자 입력을 거른다. */

	if (!capable(CAP_SYS_ADMIN))
		/* [한국어] 임의의 장치에 오류를 심고 커널 오류 처리 경로를 실행시키는 기능이라
		 * 특권이 필요하다. */
		return -EPERM;
	if (usize < offsetof(struct aer_error_inj, domain) ||
	    /* [한국어] 상한: 구조체 크기. 더 크면 거절한다. */
	    usize > sizeof(einj))
		return -EINVAL;

	memset(&einj, 0, sizeof(einj));
	/* [한국어] 사용자 버퍼를 커널 스택으로 복사한다. */
	if (copy_from_user(&einj, ubuf, usize))
		/* [한국어] 잘못된 포인터를 넘긴 경우. */
		return -EFAULT;

	ret = aer_inject(&einj);
	/* [한국어] write(2) 규약대로, 성공하면 요청한 크기를 돌려준다. */
	return ret ? ret : usize;
/* [한국어] 이 함수가 이 모듈의 유일한 사용자 진입점이다. */
}

static const struct file_operations aer_inject_fops = {
	/* [한국어] 쓰기만 지원한다 -- 읽기로 상태를 확인하는 인터페이스는 없다. */
	.write = aer_inject_write,
	/* [한국어] 모듈이 내려가는 동안 파일이 열려 있지 않도록 참조를 건다. */
	.owner = THIS_MODULE,
	.llseek = noop_llseek,
};

static struct miscdevice aer_inject_device = {
	/* [한국어] 마이너 번호를 커널에 맡긴다. */
	.minor = MISC_DYNAMIC_MINOR,
	/* [한국어] /dev/aer_inject 로 나타날 이름. */
	.name = "aer_inject",
	.fops = &aer_inject_fops,
};

/* [한국어]
 * aer_inject_init - misc 장치 /dev/aer_inject 를 등록한다
 *
 * @return: misc_register 의 반환값.
 *
 * MISC_DYNAMIC_MINOR 로 마이너 번호를 커널에 맡긴다. 등록되면
 * /dev/aer_inject 가 생기고, 사용자 도구가 거기에 struct aer_error_inj 를
 * 써서 오류를 주입한다.
 *
 * 이 시점에는 후킹도 상태 심기도 하지 않는다 -- 전부 첫 쓰기 때 일어난다.
 *
 * 실행 컨텍스트: 모듈 적재 시의 프로세스 문맥.
 *
 * 호출 체인:
 *   module_init → [이 함수] → misc_register
 */
static int __init aer_inject_init(void)
{
	return misc_register(&aer_inject_device);
}

/* [한국어]
 * aer_inject_exit - 후킹을 모두 되돌리고 주입된 레코드를 해제한다
 *
 * @return: 없음.
 *
 * 순서가 중요하다:
 *  1. misc_deregister -- **가장 먼저** 장치 노드를 없애 새 주입이 들어오지
 *     못하게 한다.
 *  2. 후킹 되돌리기 -- pci_bus_ops_pop 이 목록이 빌 때까지 항목을 하나씩
 *     꺼내고, 각 버스의 ops 를 원래 것으로 복원한 뒤 항목을 해제한다.
 *     이것을 빠뜨리면 모듈이 사라진 뒤에도 bus->ops 가 이 모듈의 함수를
 *     가리켜 커널이 죽는다.
 *  3. 주입된 오류 레코드 해제 -- 잠금 아래에서 목록을 비운다.
 *     list_for_each_entry_safe 를 쓰는 이유는 루프 안에서 항목을 해제하기
 *     때문이다.
 *
 * 2번이 3번보다 먼저인 것도 의도적이다. 후킹이 살아 있는 동안에는 설정
 * 접근이 레코드를 참조할 수 있으므로, 후킹을 먼저 끊어야 한다.
 *
 * 실행 컨텍스트: 모듈 제거 시의 프로세스 문맥.
 *
 * 호출 체인:
 *   module_exit → [이 함수] → misc_deregister → pci_bus_ops_pop
 *     → pci_bus_set_ops → kfree
 */
static void __exit aer_inject_exit(void)
{
	struct aer_error *err, *err_next;
	unsigned long flags;
	/* [한국어] 복원할 항목을 담을 포인터. */
	struct pci_bus_ops *bus_ops;
/* [한국어] 아래 순서가 이 함수의 요점이다. */

	misc_deregister(&aer_inject_device);

	while ((bus_ops = pci_bus_ops_pop())) {
		/* [한국어] 각 버스의 ops 를 원래 것으로 복원한다. 이것을 빠뜨리면 모듈이 사라진 뒤
		 * bus->ops 가 이 모듈의 함수를 가리켜 커널이 죽는다. */
		pci_bus_set_ops(bus_ops->bus, bus_ops->ops);
		/* [한국어] 항목 자체도 해제한다. */
		kfree(bus_ops);
	}

	spin_lock_irqsave(&inject_lock, flags);
	/* [한국어] 루프 안에서 항목을 해제하므로 _safe 판이 필요하다. */
	list_for_each_entry_safe(err, err_next, &einjected, list) {
		/* [한국어] 목록에서 먼저 뺀다. */
		list_del(&err->list);
		kfree(err);
	}
	spin_unlock_irqrestore(&inject_lock, flags);
/* [한국어] 후킹을 먼저 끊고(2번) 레코드를 나중에 지우는(3번) 순서도 의도적이다 --
 * 후킹이 살아 있는 동안에는 설정 접근이 레코드를 참조할 수 있다. */
}

module_init(aer_inject_init);
module_exit(aer_inject_exit);

MODULE_DESCRIPTION("PCIe AER software error injector");
MODULE_LICENSE("GPL");
