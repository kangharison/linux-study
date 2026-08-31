// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Endpoint *Controller* (EPC) library
 *
 * Copyright (C) 2017 Texas Instruments
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */

/*
 * [한국어 설명] 엔드포인트 컨트롤러 추상화 계층 (pci-epc-core.c)
 *
 * === 파일의 역할 ===
 * 엔드포인트 모드의 중심 파일이다. 여기서 EPC(Endpoint Controller)와
 * EPF(Endpoint Function)라는 두 개념이 만난다.
 *
 * 두 이름이 헷갈리기 쉬우니 먼저 정리한다.
 *   EPC — 하드웨어. SoC 에 박힌 PCIe 컨트롤러를 엔드포인트로 쓰는 것.
 *         BAR 를 노출하고, 호스트에 인터럽트를 올리고, 주소 창을 설정하는
 *         능력을 갖는다. 예: dwc/pcie-designware-ep.c 가 등록하는 것.
 *   EPF — 소프트웨어. 그 하드웨어를 써서 "무엇처럼 보일지" 를 정하는 것.
 *         시험용 장치처럼 보일 수도, NTB(Non-Transparent Bridge)처럼
 *         보일 수도 있다. 예: functions/pci-epf-test.c.
 *
 * 이 파일은 그 둘 사이의 규약이다. EPC 드라이버가 자기 능력을 함수 표
 * (struct pci_epc_ops)로 등록하면, EPF 드라이버는 그 표가 무엇으로
 * 채워졌는지 몰라도 여기 정의된 pci_epc_*() 함수만 부르면 된다.
 *
 * 즉 이 파일의 거의 모든 함수가 같은 모양이다.
 *   1) 인자가 말이 되는지 확인한다(함수 번호, 인터페이스 종류).
 *   2) EPC 가 그 기능을 지원하는지 본다(ops 에 함수가 있는지).
 *   3) 뮤텍스를 잡고 ops 를 부른 뒤 푼다.
 * 단순해 보이지만 이 세 단계가 있어야 EPF 드라이버가 하드웨어마다
 * 다른 처리를 하지 않아도 된다.
 *
 * --- Primary 와 Secondary 인터페이스 ---
 * 이 파일 곳곳에 enum pci_epc_interface_type 이 나온다. 하나의 SoC 가
 * 두 개의 PCIe 인터페이스를 갖고 양쪽 모두 엔드포인트로 동작하는 구성이
 * 있기 때문이다. NTB 가 그 대표로, 두 호스트 사이에 끼어 양쪽에 각각
 * 장치처럼 보이며 데이터를 중계한다. 그래서 EPF 하나가 EPC 두 개에
 * 붙을 수 있고, 어느 쪽인지 구분할 이름이 필요하다.
 *
 * --- 이벤트 통지 ---
 * 링크가 올라오고 내려가는 것, 호스트가 Bus Master 를 켜는 것 같은
 * 사건은 EPC 드라이버가 알아채고 EPF 에 알려야 한다. 그 통지 함수들이
 * 파일 뒤쪽에 모여 있다(pci_epc_linkup, pci_epc_init_notify 등).
 *
 * 그중 pci_epc_notify_pending_init() 이 흥미롭다. 링크가 이미 올라온
 * 뒤에 EPF 가 붙는 경우, 그 EPF 는 초기화 통지를 놓치게 된다. 그래서
 * EPC 가 "이미 초기화됐음" 을 기억해 두었다가 늦게 온 EPF 에게 알려 준다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 컨트롤러 드라이버(예: dwc/pcie-designware-ep.c)의 probe
 *   -> devm_pci_epc_create() [이 파일] 로 EPC 등록
 *      -> pci_epc_mem_init() [pci-epc-mem.c] 로 주소 창 준비
 *
 * 사용자가 configfs 로 EPF 를 만들어 EPC 에 연결
 *   -> pci_epc_add_epf() [이 파일]
 *      -> EPF 드라이버의 bind 콜백
 *         -> pci_epc_write_header() / _set_bar() / _set_msi() [이 파일]
 *            -> 각각 EPC 드라이버의 ops 로 내려간다
 *   -> pci_epc_start() [이 파일] 로 링크를 올린다
 *
 * 동작 중
 *   -> EPF 가 pci_epc_map_addr() 로 호스트 메모리에 접근
 *   -> EPF 가 pci_epc_raise_irq() 로 호스트에 인터럽트를 올림
 *   -> EPC 드라이버가 pci_epc_linkup() 등으로 사건을 알림
 *
 * 실행 컨텍스트: 대부분 프로세스 컨텍스트다. 뮤텍스를 잡으므로
 *   인터럽트 컨텍스트에서 부를 수 없다. 다만 통지 함수들(linkup 등)은
 *   EPC 드라이버의 인터럽트 처리에서 불릴 수 있어 락을 잡지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: endpoint/functions/ 의 EPF 드라이버들, pci-ep-cfs.c(configfs).
 * 아래쪽: 각 EPC 드라이버의 struct pci_epc_ops.
 * 옆쪽: pci-epc-mem.c(주소 창 할당), pci-epf-core.c(EPF 쪽 관리).
 * 공유 상태: struct pci_epc — ops 표, 창 목록, 붙어 있는 EPF 목록,
 *   그리고 그것들을 보호하는 뮤텍스와 스핀락.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(drivers/nvme 트리
 * 전수 확인 — pci_epc_* 호출 0건).
 *
 * 이 파일이 NVMe 공부에 주는 것은 "반대편에서 본 PCI" 다. 호스트 쪽
 * NVMe 드라이버가 하는 일들 — BAR 를 읽고, MSI-X 를 설정하고, DMA 로
 * 메모리에 접근하고 — 의 상대편이 여기 다 있다.
 *   nvme_probe() 가 BAR 를 ioremap 한다  ↔  EPF 가 pci_epc_set_bar() 로
 *     그 BAR 를 만들어 노출한다
 *   NVMe 가 pci_alloc_irq_vectors() 로 MSI-X 를 요청한다  ↔  EPF 가
 *     pci_epc_set_msix() 로 그 개수를 광고한다
 *   NVMe 컨트롤러가 DMA 로 호스트 메모리를 읽는다  ↔  EPF 가
 *     pci_epc_map_addr() 로 그 접근을 구현한다
 * 양쪽을 함께 보면 PCIe 장치가 어떻게 성립하는지가 선명해진다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_epc_get() / pci_epc_put() : 이름으로 EPC 를 찾고 참조를 관리한다.
 * __pci_epc_create() / __devm_pci_epc_create() : EPC 등록. 컨트롤러
 *                          드라이버가 probe 에서 부른다.
 * pci_epc_destroy()      : 등록 해제.
 * pci_epc_add_epf() / pci_epc_remove_epf() : EPF 를 EPC 에 붙이고 뗀다.
 *                          Primary/Secondary 인터페이스를 여기서 가른다.
 * pci_epc_write_header() : config space 헤더(벤더 ID, 클래스 등)를 쓴다.
 *                          호스트가 이 장치를 무엇으로 볼지 정하는 것.
 * pci_epc_set_bar() / pci_epc_clear_bar() : BAR 를 만들고 없앤다.
 * pci_epc_set_msi() / _get_msi() / _set_msix() / _get_msix() : 인터럽트
 *                          능력을 광고하고 호스트가 정한 값을 읽는다.
 * pci_epc_raise_irq()    : 호스트에 인터럽트를 올린다.
 * pci_epc_map_addr() / _unmap_addr() : 창의 자리를 호스트 주소에 연결한다.
 * pci_epc_mem_map() / _mem_unmap() : 위 둘에 정렬 처리를 더한 상위 API.
 * pci_epc_start() / pci_epc_stop() : 링크를 올리고 내린다.
 * pci_epc_linkup() / _linkdown() / _init_notify() / _deinit_notify() /
 * _bus_master_enable_notify() : EPC 드라이버가 EPF 에 사건을 알린다.
 * pci_epc_get_features() : 이 EPC 가 무엇을 지원하는지 EPF 가 물어본다.
 * pci_epc_get_first_free_bar() / _next_free_bar() : 쓸 수 있는 BAR 찾기.
 * pci_epc_function_is_valid() : 이 파일 거의 모든 함수가 쓰는 공통 검증.
 * struct pci_epc         : 컨트롤러 하나의 전체 상태.
 * struct pci_epc_ops     : EPC 드라이버가 채우는 함수 표.
 */

/* [한국어] struct device, device_register, class 등록 등. EPC 를 커널
 * 장치 모델에 등록하므로 필요하다. */
#include <linux/device.h>
/* [한국어] kzalloc / kfree — struct pci_epc 할당. */
#include <linux/slab.h>
/* [한국어] EXPORT_SYMBOL_GPL 과 모듈 초기화 매크로. */
#include <linux/module.h>

/* [한국어] struct pci_epc, struct pci_epc_ops, struct pci_epc_features
 * 정의와 이 파일이 구현하는 함수들의 선언. */
#include <linux/pci-epc.h>
/* [한국어] struct pci_epf 와 그 이벤트 콜백 표(struct pci_epf_event_ops).
 * 아래 통지 함수들이 이 콜백을 부른다. */
#include <linux/pci-epf.h>
/* [한국어] configfs 인터페이스. EPC 를 만들 때 그에 대응하는 configfs
 * 그룹도 함께 만들어, 사용자가 EPF 를 연결할 수 있게 한다. */
#include <linux/pci-ep-cfs.h>

/* [한국어] 모든 EPC 가 속하는 device class.
 * /sys/class/pci_epc/ 아래에 각 EPC 가 나타난다. class 를 두는 이유는
 * pci_epc_get() 이 이름으로 EPC 를 찾을 때 이 class 를 훑기 때문이다 —
 * configfs 에서 사용자가 "이 EPF 를 이 이름의 EPC 에 붙여라" 고 하면
 * 그 이름으로 여기서 찾는다. */
static const struct class pci_epc_class = {
	.name = "pci_epc",
};

/* [한국어]
 * devm_pci_epc_release - devres 가 EPC 를 자동 정리할 때 부르는 함수
 *
 * @dev: 이 자원을 소유한 device(컨트롤러 드라이버의 것).
 * @res: devres 가 관리하는 저장 공간. 여기에 EPC 포인터가 들어 있다.
 * @return: 없음.
 *
 * devm_pci_epc_create() 로 만든 EPC 는 컨트롤러 드라이버가 언바인드될 때
 * 자동으로 정리된다. 그 자동 정리의 실제 동작이 이 함수다.
 *
 * res 를 이중 포인터로 역참조하는 이유는 devres 의 규약 때문이다.
 * devres 는 임의 크기의 블록을 관리하는데, 여기서는 그 블록에
 * struct pci_epc * 하나를 넣어 두었다. 그래서 res 는 "EPC 포인터가
 * 들어 있는 자리" 이고, 실제 EPC 를 얻으려면 한 번 더 벗겨야 한다.
 *
 * 실행 컨텍스트: 드라이버 언바인드 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (드라이버 언바인드) → devres 정리 → [이 함수] → pci_epc_destroy()
 */
static void devm_pci_epc_release(struct device *dev, void *res)
{
	/* [한국어] devres 블록 안에 저장해 둔 EPC 포인터를 꺼낸다. */
	struct pci_epc *epc = *(struct pci_epc **)res;

	/* [한국어] 실제 해제는 공통 경로에 맡긴다. devm 이든 아니든
	 * 정리 절차는 같아야 하기 때문이다. */
	pci_epc_destroy(epc);
}

/**
 * pci_epc_put() - release the PCI endpoint controller
 * @epc: epc returned by pci_epc_get()
 *
 * release the refcount the caller obtained by invoking pci_epc_get()
 */
void pci_epc_put(struct pci_epc *epc)
{
	/* [한국어] pci_epc_get() 이 실패하면 ERR_PTR 을 돌려주므로, 그 값을
	 * 그대로 넘겨도 안전하게 하려는 것이다. 호출자가 성공 여부를 따로
	 * 확인하지 않고 정리 경로에서 무조건 put 할 수 있게 된다. */
	if (IS_ERR_OR_NULL(epc))
		return;

	/* [한국어] get 이 올린 모듈 참조를 내린다. 이것이 있어야 EPC 를
	 * 쓰는 동안 그 컨트롤러 드라이버 모듈이 언로드되지 않는다. */
	module_put(epc->ops->owner);
	/* [한국어] device 참조도 내린다. class_find_device_by_name() 이
	 * 올려 준 것이다. 둘을 짝지어 내리는 순서는 중요하지 않지만,
	 * get 과 대칭이 되도록 역순으로 두었다. */
	put_device(&epc->dev);
}
EXPORT_SYMBOL_GPL(pci_epc_put);

/**
 * pci_epc_get() - get the PCI endpoint controller
 * @epc_name: device name of the endpoint controller
 *
 * Invoke to get struct pci_epc * corresponding to the device name of the
 * endpoint controller
 */
struct pci_epc *pci_epc_get(const char *epc_name)
{
	/* [한국어] 실패 시 돌려줄 값. 이 함수의 모든 실패 경로가 -EINVAL 로
	 * 수렴한다 — 이름을 못 찾은 것과 모듈 참조를 못 올린 것을 구분해
	 * 주지 않는데, 호출자가 할 수 있는 일이 어차피 같기 때문이다. */
	int ret = -EINVAL;
	struct pci_epc *epc;
	struct device *dev;

	/* [한국어] pci_epc class 안에서 이름이 맞는 device 를 찾는다.
	 * 찾으면 참조를 올려서 돌려주므로, 아래에서 반드시 내려야 한다.
	 * 사용자가 configfs 에 EPC 이름을 적어 EPF 를 연결하면 그 문자열이
	 * 여기까지 내려온다. */
	dev = class_find_device_by_name(&pci_epc_class, epc_name);
	if (!dev)
		goto err;

	/* [한국어] device 에서 바깥 struct pci_epc 로 되짚는다. */
	epc = to_pci_epc(dev);
	/* [한국어] 컨트롤러 드라이버 모듈의 참조를 올린다. 성공하면
	 * device 참조는 내리지 않고 그대로 유지한 채 돌려준다 —
	 * 두 참조 모두 호출자가 pci_epc_put() 으로 내릴 몫이다.
	 * 실패한다면 그 모듈이 이미 언로드 중이라는 뜻이다. */
	if (try_module_get(epc->ops->owner))
		return epc;

err:
	/* [한국어] 실패 경로. dev 가 NULL 일 수도 있는데(위 첫 goto),
	 * put_device(NULL) 은 무해하므로 한 경로로 합쳤다. */
	put_device(dev);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(pci_epc_get);

/**
 * pci_epc_get_first_free_bar() - helper to get first unreserved BAR
 * @epc_features: pci_epc_features structure that holds the reserved bar bitmap
 *
 * Invoke to get the first unreserved BAR that can be used by the endpoint
 * function.
 */
enum pci_barno
pci_epc_get_first_free_bar(const struct pci_epc_features *epc_features)
{
	/* [한국어] BAR_0 부터 찾으라고 하면 곧 "첫 번째 빈 BAR" 다.
	 * 아래 함수의 특수한 경우일 뿐이라 따로 구현하지 않는다. */
	return pci_epc_get_next_free_bar(epc_features, BAR_0);
}
EXPORT_SYMBOL_GPL(pci_epc_get_first_free_bar);

/**
 * pci_epc_get_next_free_bar() - helper to get unreserved BAR starting from @bar
 * @epc_features: pci_epc_features structure that holds the reserved bar bitmap
 * @bar: the starting BAR number from where unreserved BAR should be searched
 *
 * Invoke to get the next unreserved BAR starting from @bar that can be used
 * for endpoint function.
 */
enum pci_barno pci_epc_get_next_free_bar(const struct pci_epc_features
					 *epc_features, enum pci_barno bar)
{
	int i;

	/* [한국어] 능력 정보가 없으면 제약도 없다고 보고 BAR_0 을 준다.
	 * 능력을 보고하지 않는 EPC 드라이버를 위한 관대한 기본값이다. */
	if (!epc_features)
		return BAR_0;

	/* If 'bar - 1' is a 64-bit BAR, move to the next BAR */
	/* [한국어] 상류 주석이 짧게 말한 것을 풀면 이렇다.
	 * PCI 의 64비트 BAR 는 연속한 두 개의 32비트 BAR 자리를 함께 쓴다.
	 * BAR 0 이 64비트면 BAR 1 은 그 상위 절반으로 잡아먹히므로 따로
	 * 쓸 수 없다.
	 * 그래서 바로 앞 BAR 가 64비트 전용이면 지금 자리는 이미 쓰인
	 * 것이니 한 칸 더 건너뛴다.
	 * bar > 0 을 함께 보는 것은 bar-1 이 음수가 되지 않게 하려는 것이다. */
	if (bar > 0 && epc_features->bar[bar - 1].only_64bit)
		bar++;

	/* [한국어] 그 자리부터 표준 BAR 6개 범위 안에서 쓸 수 있는 것을 찾는다. */
	for (i = bar; i < PCI_STD_NUM_BARS; i++) {
		/* If the BAR is not reserved or disabled, return it. */
		/* [한국어] 두 가지를 배제한다.
		 *   BAR_RESERVED — EPC 하드웨어가 자기 용도로 이미 쓰는 자리.
		 *   BAR_DISABLED — 이 하드웨어에 아예 없는 자리.
		 * 둘 다 EPF 가 건드리면 안 된다. */
		if (epc_features->bar[i].type != BAR_RESERVED &&
		    epc_features->bar[i].type != BAR_DISABLED)
			return i;
	}

	/* [한국어] 남은 자리가 없다. 호출자는 이 값을 확인해 BAR 를 더
	 * 요구하지 않도록 해야 한다. */
	return NO_BAR;
}
EXPORT_SYMBOL_GPL(pci_epc_get_next_free_bar);

/* [한국어]
 * pci_epc_function_is_valid - 함수 번호가 이 EPC 에서 말이 되는지 확인한다
 *
 * @epc: 대상 컨트롤러. ERR_PTR 이나 NULL 이어도 안전하게 다룬다.
 * @func_no: 물리 함수 번호(PF).
 * @vfunc_no: 가상 함수 번호(VF). 0 이면 PF 자신을 뜻한다.
 * @return: 유효하면 true.
 *
 * 이 파일의 거의 모든 공개 함수가 맨 앞에서 이것을 부른다. 같은 검사를
 * 스무 번 넘게 반복하지 않으려고 뽑아낸 것이다.
 *
 * 확인하는 것이 셋이다.
 *   epc 포인터가 쓸 만한가 — pci_epc_get() 의 ERR_PTR 이 그대로 흘러
 *     들어올 수 있어 IS_ERR 까지 본다.
 *   PF 번호가 범위 안인가 — 하드웨어가 지원하는 함수 수를 넘으면 안 된다.
 *   VF 번호가 범위 안인가 — SR-IOV 를 지원하지 않는 EPC 라면 max_vfs 가
 *     아예 NULL 이므로 그것부터 확인해야 한다.
 *
 * vfunc_no 가 0 일 때 VF 검사를 건너뛰는 것이 요점이다. 0 은 "VF 가
 * 아니라 PF 자신" 을 뜻하는 약속이라, SR-IOV 를 모르는 EPC 에서도
 * 통과해야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 검사이며 락을 잡지 않는다.
 *
 * 호출 체인:
 *   이 파일의 거의 모든 pci_epc_*() 공개 함수 → [이 함수]
 */
static bool pci_epc_function_is_valid(struct pci_epc *epc,
				      u8 func_no, u8 vfunc_no)
{
	/* [한국어] 포인터 자체와 PF 번호를 함께 본다. max_functions 는
	 * EPC 등록 시 컨트롤러 드라이버가 알려 준 값이다. */
	if (IS_ERR_OR_NULL(epc) || func_no >= epc->max_functions)
		return false;

	/* [한국어] VF 를 지정한 경우에만 그쪽을 확인한다.
	 * max_vfs 가 NULL 이면 이 EPC 는 SR-IOV 를 지원하지 않으므로
	 * 0 이 아닌 VF 번호는 무조건 잘못이다. 배열이 있으면 그 PF 의
	 * VF 상한과 비교한다 — PF 마다 VF 수가 다를 수 있어 배열이다. */
	if (vfunc_no > 0 && (!epc->max_vfs || vfunc_no > epc->max_vfs[func_no]))
		return false;

	return true;
}

/**
 * pci_epc_get_features() - get the features supported by EPC
 * @epc: the features supported by *this* EPC device will be returned
 * @func_no: the features supported by the EPC device specific to the
 *	     endpoint function with func_no will be returned
 * @vfunc_no: the features supported by the EPC device specific to the
 *	     virtual endpoint function with vfunc_no will be returned
 *
 * Invoke to get the features provided by the EPC which may be
 * specific to an endpoint function. Returns pci_epc_features on success
 * and NULL for any failures.
 */
const struct pci_epc_features *pci_epc_get_features(struct pci_epc *epc,
						    u8 func_no, u8 vfunc_no)
{
	const struct pci_epc_features *epc_features;

	/* [한국어] 이 파일의 표준 1단계 — 함수 번호 검증. */
	if (!pci_epc_function_is_valid(epc, func_no, vfunc_no))
		return NULL;

	/* [한국어] 표준 2단계 — EPC 가 이 기능을 지원하는지. ops 에 함수가
	 * 없으면 그 컨트롤러는 능력 정보를 보고하지 않는다는 뜻이고,
	 * 그때는 위 get_next_free_bar 가 관대한 기본값으로 처리한다. */
	if (!epc->ops->get_features)
		return NULL;

	/* [한국어] 표준 3단계 — 락을 잡고 ops 를 부른다.
	 * 조회일 뿐인데 락을 잡는 이유는 EPC 드라이버의 ops 구현이 내부
	 * 상태를 만질 수 있기 때문이다. 이 계층은 그 안을 모르므로
	 * 일률적으로 보호한다. */
	mutex_lock(&epc->lock);
	epc_features = epc->ops->get_features(epc, func_no, vfunc_no);
	mutex_unlock(&epc->lock);

	/* [한국어] EPC 드라이버가 돌려준 능력 표를 그대로 전달한다. 이 구조체는 대개
	 * 드라이버의 정적 데이터라 호출자가 해제할 것이 없다. */
	return epc_features;
}
EXPORT_SYMBOL_GPL(pci_epc_get_features);

/**
 * pci_epc_stop() - stop the PCI link
 * @epc: the link of the EPC device that has to be stopped
 *
 * Invoke to stop the PCI link
 */
void pci_epc_stop(struct pci_epc *epc)
{
	/* [한국어] 여기서는 pci_epc_function_is_valid() 를 쓰지 않는다.
	 * 링크는 EPC 전체의 성질이라 특정 함수 번호와 무관하기 때문이다.
	 * 대신 포인터 유효성과 ops 존재만 한 줄로 확인한다.
	 * IS_ERR_OR_NULL 이 아니라 IS_ERR 인 점이 눈에 띄는데, NULL 이
	 * 넘어오면 바로 아래 epc->ops 역참조에서 터진다 — 다만 이 함수는
	 * EPC 를 이미 손에 쥔 코드가 부르는 것이라 실제로 그럴 일은 없다. */
	if (IS_ERR(epc) || !epc->ops->stop)
		return;

	/* [한국어] 링크를 내린다. 호스트 입장에서는 장치가 사라진 것처럼
	 * 보이며, 핫플러그를 지원하는 슬롯이면 제거 이벤트가 발생한다. */
	mutex_lock(&epc->lock);
	epc->ops->stop(epc);
	mutex_unlock(&epc->lock);
}
EXPORT_SYMBOL_GPL(pci_epc_stop);

/**
 * pci_epc_start() - start the PCI link
 * @epc: the link of *this* EPC device has to be started
 *
 * Invoke to start the PCI link
 */
int pci_epc_start(struct pci_epc *epc)
{
	/* [한국어] ops->start 의 결과이자 이 함수의 반환값. */
	int ret;

	if (IS_ERR(epc))
		return -EINVAL;

	/* [한국어] start 를 구현하지 않은 EPC 는 링크가 늘 살아 있거나
	 * 하드웨어가 알아서 올린다는 뜻이다. 오류가 아니므로 0 을 준다.
	 * 이 파일 곳곳에서 "ops 가 없으면 0" 과 "ops 가 없으면 -EINVAL" 이
	 * 갈리는데, 없어도 무방한 기능이면 0, 반드시 있어야 하는 기능이면
	 * 오류로 구분한 것이다. */
	if (!epc->ops->start)
		return 0;

	/* [한국어] 링크 트레이닝을 시작한다. 이 시점에는 BAR 와 config
	 * 헤더 설정이 이미 끝나 있어야 한다 — 호스트가 링크가 올라오자마자
	 * config 를 읽기 때문이다. 그래서 EPF 의 bind 에서 설정을 다 하고
	 * 마지막에 이 함수를 부르는 순서가 지켜져야 한다. */
	mutex_lock(&epc->lock);
	ret = epc->ops->start(epc);
	mutex_unlock(&epc->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(pci_epc_start);

/**
 * pci_epc_raise_irq() - interrupt the host system
 * @epc: the EPC device which has to interrupt the host
 * @func_no: the physical endpoint function number in the EPC device
 * @vfunc_no: the virtual endpoint function number in the physical function
 * @type: specify the type of interrupt; INTX, MSI or MSI-X
 * @interrupt_num: the MSI or MSI-X interrupt number with range (1-N)
 *
 * Invoke to raise an INTX, MSI or MSI-X interrupt
 */
int pci_epc_raise_irq(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
		      unsigned int type, u16 interrupt_num)
{
	int ret;

	/* [한국어] 표준 1단계 — 함수 번호 검증. */
	if (!pci_epc_function_is_valid(epc, func_no, vfunc_no))
		return -EINVAL;

	/* [한국어] 인터럽트를 올릴 수 없는 EPC 도 있다. 폴링으로만 동작하는
	 * 구성이라면 오류가 아니므로 0 을 준다. */
	if (!epc->ops->raise_irq)
		return 0;

	/* [한국어] 호스트에 인터럽트를 올린다. 이것이 엔드포인트가 호스트를
	 * 깨우는 정방향 경로다 — pci-ep-msi.c 의 도어벨이 그 반대 방향이었다.
	 *
	 * type 이 INTX / MSI / MSI-X 중 하나이고, interrupt_num 은 1부터
	 * 세는 번호다. 0-기반이 아니라 1-기반인 것은 상류 kernel-doc 이
	 * 밝힌 규약이며, 호스트 쪽 NVMe 드라이버가 벡터를 0부터 세는 것과
	 * 다르므로 주의해야 한다.
	 *
	 * 실제 구현은 하드웨어마다 다르다. MSI 라면 컨트롤러가 호스트가
	 * 알려 준 MSI 주소에 데이터를 쓰는 TLP 를 만들어 보낸다. */
	mutex_lock(&epc->lock);
	ret = epc->ops->raise_irq(epc, func_no, vfunc_no, type, interrupt_num);
	mutex_unlock(&epc->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(pci_epc_raise_irq);

/**
 * pci_epc_map_msi_irq() - Map physical address to MSI address and return
 *                         MSI data
 * @epc: the EPC device which has the MSI capability
 * @func_no: the physical endpoint function number in the EPC device
 * @vfunc_no: the virtual endpoint function number in the physical function
 * @phys_addr: the physical address of the outbound region
 * @interrupt_num: the MSI interrupt number with range (1-N)
 * @entry_size: Size of Outbound address region for each interrupt
 * @msi_data: the data that should be written in order to raise MSI interrupt
 *            with interrupt number as 'interrupt num'
 * @msi_addr_offset: Offset of MSI address from the aligned outbound address
 *                   to which the MSI address is mapped
 *
 * Invoke to map physical address to MSI address and return MSI data. The
 * physical address should be an address in the outbound region. This is
 * required to implement doorbell functionality of NTB wherein EPC on either
 * side of the interface (primary and secondary) can directly write to the
 * physical address (in outbound region) of the other interface to ring
 * doorbell.
 */
int pci_epc_map_msi_irq(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			phys_addr_t phys_addr, u8 interrupt_num, u32 entry_size,
			u32 *msi_data, u32 *msi_addr_offset)
{
	/* [한국어] ops->map_msi_irq 의 결과. 성공하면 출력 인자 둘이 채워져 있다. */
	int ret;

	if (!pci_epc_function_is_valid(epc, func_no, vfunc_no))
		return -EINVAL;

	/* [한국어] 위 raise_irq 와 달리 여기서는 -EINVAL 이다. 이 기능을
	 * 요구하는 쪽(NTB)은 대안이 없으므로, 지원하지 않으면 실패로
	 * 알려야 호출자가 다른 방법을 찾을 수 있다. */
	if (!epc->ops->map_msi_irq)
		return -EINVAL;

	/* [한국어] 상류 kernel-doc 이 이 함수의 존재 이유를 잘 밝히고 있다.
	 * NTB 의 도어벨을 구현하기 위한 것이다.
	 *
	 * 보통 인터럽트를 올리려면 위 raise_irq 처럼 소프트웨어가 개입해야
	 * 한다. 그런데 NTB 는 양쪽 인터페이스가 서로의 아웃바운드 영역에
	 * 직접 쓰기만 해도 상대의 인터럽트가 발생하기를 원한다. 그러려면
	 * "어느 물리 주소에 어떤 값을 쓰면 그 인터럽트가 나는지" 를 미리
	 * 알아 두어야 하고, 이 함수가 그 값을 알려 준다.
	 *
	 * 출력이 둘이다. msi_data 는 쓸 값, msi_addr_offset 은 정렬된
	 * 아웃바운드 주소로부터의 오프셋이다. MSI 주소가 페이지 경계에
	 * 딱 맞지 않을 수 있어 그 어긋남을 따로 알려 주는 것이다. */
	mutex_lock(&epc->lock);
	ret = epc->ops->map_msi_irq(epc, func_no, vfunc_no, phys_addr,
				    interrupt_num, entry_size, msi_data,
				    msi_addr_offset);
	mutex_unlock(&epc->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(pci_epc_map_msi_irq);

/**
 * pci_epc_get_msi() - get the number of MSI interrupt numbers allocated
 * @epc: the EPC device to which MSI interrupts was requested
 * @func_no: the physical endpoint function number in the EPC device
 * @vfunc_no: the virtual endpoint function number in the physical function
 *
 * Invoke to get the number of MSI interrupts allocated by the RC
 */
int pci_epc_get_msi(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	int interrupt;

	/* [한국어] 이 함수는 오류를 음수로 알리지 않고 전부 0 으로 뭉갠다.
	 * 반환값이 "할당된 인터럽트 개수" 라 0 이 곧 "없음" 이고, 그것이
	 * 오류일 때 호출자가 할 일과 같기 때문이다. */
	if (!pci_epc_function_is_valid(epc, func_no, vfunc_no))
		return 0;

	if (!epc->ops->get_msi)
		return 0;

	/* [한국어] 호스트가 이 함수에 실제로 몇 개의 MSI 를 할당했는지 읽는다.
	 * 방향을 잘 봐야 한다 — EPF 는 set_msi() 로 "이만큼 필요하다" 고
	 * 광고할 뿐이고, 실제로 몇 개를 줄지는 호스트 쪽 드라이버가 정한다.
	 * 호스트 NVMe 드라이버가 pci_alloc_irq_vectors() 로 요청하면
	 * 그 결과가 config space 에 반영되고, 엔드포인트는 이 함수로
	 * 그 값을 읽어 자기가 쓸 수 있는 벡터 수를 안다. */
	mutex_lock(&epc->lock);
	interrupt = epc->ops->get_msi(epc, func_no, vfunc_no);
	mutex_unlock(&epc->lock);

	/* [한국어] 드라이버가 음수 오류를 돌려줘도 0 으로 바꾼다. 위와 같은
	 * 이유이며, 호출자가 음수를 개수로 오해하는 사고를 막는다. */
	if (interrupt < 0)
		return 0;

	/* [한국어] 호스트가 켠 MSI 개수. 0 이면 호스트가 아직 설정하지 않았거나
	 * MSI 대신 INTX 를 쓰기로 한 것이다. */
	return interrupt;
}
EXPORT_SYMBOL_GPL(pci_epc_get_msi);

/**
 * pci_epc_set_msi() - set the number of MSI interrupt numbers required
 * @epc: the EPC device on which MSI has to be configured
 * @func_no: the physical endpoint function number in the EPC device
 * @vfunc_no: the virtual endpoint function number in the physical function
 * @nr_irqs: number of MSI interrupts required by the EPF
 *
 * Invoke to set the required number of MSI interrupts.
 */
int pci_epc_set_msi(struct pci_epc *epc, u8 func_no, u8 vfunc_no, u8 nr_irqs)
{
	/* [한국어] ops->set_msi 의 결과. */
	int ret;

	if (!pci_epc_function_is_valid(epc, func_no, vfunc_no))
		return -EINVAL;

	/* [한국어] MSI 는 규격상 1개에서 32개까지만 가능하다. capability 의
	 * Multiple Message Capable 필드가 3비트라 2의 거듭제곱으로
	 * 1,2,4,8,16,32 만 표현할 수 있기 때문이다. 그보다 많이 쓰려면
	 * MSI-X 를 써야 하며, 그쪽은 2048개까지 된다.
	 * 여기서는 범위만 보고 2의 거듭제곱인지는 확인하지 않는데,
	 * 그 보정은 EPC 드라이버의 몫으로 남겨 둔 것이다. */
	if (nr_irqs < 1 || nr_irqs > 32)
		return -EINVAL;

	/* [한국어] MSI 를 지원하지 않는 EPC 라면 INTX 로만 동작한다는 뜻이다.
	 * 오류가 아니므로 0. */
	if (!epc->ops->set_msi)
		return 0;

	/* [한국어] config space 의 MSI capability 에 "이만큼 쓸 수 있다" 를
	 * 적는다. 이것은 광고일 뿐이고, 호스트가 실제로 몇 개를 켤지는
	 * 위 get_msi 로 나중에 읽어 봐야 안다. */
	mutex_lock(&epc->lock);
	ret = epc->ops->set_msi(epc, func_no, vfunc_no, nr_irqs);
	mutex_unlock(&epc->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(pci_epc_set_msi);

/**
 * pci_epc_get_msix() - get the number of MSI-X interrupt numbers allocated
 * @epc: the EPC device to which MSI-X interrupts was requested
 * @func_no: the physical endpoint function number in the EPC device
 * @vfunc_no: the virtual endpoint function number in the physical function
 *
 * Invoke to get the number of MSI-X interrupts allocated by the RC
 */
int pci_epc_get_msix(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	int interrupt;

	/* [한국어] 위 get_msi 와 완전히 같은 구조다. 오류를 0 으로 뭉개는
	 * 이유도 같다. */
	if (!pci_epc_function_is_valid(epc, func_no, vfunc_no))
		return 0;

	if (!epc->ops->get_msix)
		return 0;

	/* [한국어] 호스트가 실제로 켠 MSI-X 벡터 수를 읽는다.
	 * MSI 와 달리 MSI-X 는 벡터마다 주소와 데이터를 따로 가지므로,
	 * 호스트가 일부만 켜는 것도 자연스럽다. */
	mutex_lock(&epc->lock);
	interrupt = epc->ops->get_msix(epc, func_no, vfunc_no);
	mutex_unlock(&epc->lock);

	/* [한국어] 위 get_msi 와 같은 이유로 음수를 0 으로 바꾼다. */
	if (interrupt < 0)
		return 0;

	/* [한국어] 호스트가 켠 MSI-X 벡터 수. */
	return interrupt;
}
EXPORT_SYMBOL_GPL(pci_epc_get_msix);

/**
 * pci_epc_set_msix() - set the number of MSI-X interrupt numbers required
 * @epc: the EPC device on which MSI-X has to be configured
 * @func_no: the physical endpoint function number in the EPC device
 * @vfunc_no: the virtual endpoint function number in the physical function
 * @nr_irqs: number of MSI-X interrupts required by the EPF
 * @bir: BAR where the MSI-X table resides
 * @offset: Offset pointing to the start of MSI-X table
 *
 * Invoke to set the required number of MSI-X interrupts.
 */
int pci_epc_set_msix(struct pci_epc *epc, u8 func_no, u8 vfunc_no, u16 nr_irqs,
		     enum pci_barno bir, u32 offset)
{
	/* [한국어] ops->set_msix 의 결과. */
	int ret;

	if (!pci_epc_function_is_valid(epc, func_no, vfunc_no))
		return -EINVAL;

	/* [한국어] MSI-X 의 상한은 2048 이다. Message Control 의 Table Size
	 * 필드가 11비트라 0..2047 을 표현하고, 0-기반이라 실제로는 1..2048 개다.
	 * MSI 의 32개와 비교하면 훨씬 넉넉하며, 그것이 NVMe 같은 다중 큐
	 * 장치가 MSI-X 를 쓰는 이유다 — CPU 코어마다 벡터를 하나씩 두려면
	 * 32개로는 모자란다. */
	if (nr_irqs < 1 || nr_irqs > 2048)
		return -EINVAL;

	if (!epc->ops->set_msix)
		return 0;

	/* [한국어] MSI 와 달리 인자가 셋 더 붙는 이유가 MSI-X 의 구조 때문이다.
	 * MSI 는 벡터 정보가 config space 안에 다 들어가지만, MSI-X 는
	 * 벡터가 많아 config space 에 담을 수 없어 BAR 안의 메모리에 표를 둔다.
	 *   bir(BAR Indicator Register) — 그 표가 몇 번 BAR 에 있는지
	 *   offset — 그 BAR 안에서 표가 시작하는 위치
	 * 호스트는 config 의 이 두 값을 읽어 표가 어디 있는지 알아낸 뒤,
	 * 그 BAR 를 매핑해 벡터마다 주소와 데이터를 직접 써 넣는다. */
	mutex_lock(&epc->lock);
	ret = epc->ops->set_msix(epc, func_no, vfunc_no, nr_irqs, bir, offset);
	mutex_unlock(&epc->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(pci_epc_set_msix);

/**
 * pci_epc_unmap_addr() - unmap CPU address from PCI address
 * @epc: the EPC device on which address is allocated
 * @func_no: the physical endpoint function number in the EPC device
 * @vfunc_no: the virtual endpoint function number in the physical function
 * @phys_addr: physical address of the local system
 *
 * Invoke to unmap the CPU address from PCI address.
 */
void pci_epc_unmap_addr(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			phys_addr_t phys_addr)
{
	/* [한국어] unmap 은 void 라 오류를 알릴 수 없다. 잘못된 인자면 조용히 물러난다. */
	if (!pci_epc_function_is_valid(epc, func_no, vfunc_no))
		return;

	if (!epc->ops->unmap_addr)
		return;

	/* [한국어] 아웃바운드 주소 변환 설정을 해제한다. 이제 그 물리 영역에
	 * 접근해도 PCIe 링크 너머로 나가지 않는다.
	 * 창의 자리 자체를 반납하는 것은 pci_epc_mem_free_addr() 의 몫이며,
	 * 이 함수는 "연결" 만 끊는다. 자리 잡기와 연결하기를 나눈 설계의
	 * 반대편이다. */
	mutex_lock(&epc->lock);
	epc->ops->unmap_addr(epc, func_no, vfunc_no, phys_addr);
	mutex_unlock(&epc->lock);
}
EXPORT_SYMBOL_GPL(pci_epc_unmap_addr);

/**
 * pci_epc_map_addr() - map CPU address to PCI address
 * @epc: the EPC device on which address is allocated
 * @func_no: the physical endpoint function number in the EPC device
 * @vfunc_no: the virtual endpoint function number in the physical function
 * @phys_addr: physical address of the local system
 * @pci_addr: PCI address to which the physical address should be mapped
 * @size: the size of the allocation
 *
 * Invoke to map CPU address with PCI address.
 */
int pci_epc_map_addr(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
		     phys_addr_t phys_addr, u64 pci_addr, size_t size)
{
	/* [한국어] ops->map_addr 의 결과. 하드웨어에 변환 창이 남지 않았으면 실패한다. */
	int ret;

	if (!pci_epc_function_is_valid(epc, func_no, vfunc_no))
		return -EINVAL;

	if (!epc->ops->map_addr)
		return 0;

	/* [한국어] 아웃바운드 주소 변환을 설정한다. 이 파일에서 가장
	 * 실질적인 함수 중 하나다.
	 *
	 * 두 주소의 방향을 정확히 봐야 한다.
	 *   phys_addr — 이쪽 SoC 의 물리 주소. pci_epc_mem_alloc_addr() 로
	 *     창에서 빌린 자리다.
	 *   pci_addr  — 저쪽 호스트의 PCI 주소. 실제로 닿고 싶은 곳이다.
	 * 이 호출 뒤에는 phys_addr 에 접근하면 그것이 TLP 가 되어 호스트의
	 * pci_addr 에 닿는다.
	 *
	 * 호스트 쪽에서 보면 이것이 곧 장치의 DMA 다. NVMe 컨트롤러가
	 * 호스트 메모리의 PRP 나 SGL 이 가리키는 곳을 읽는 동작을,
	 * 소프트웨어로 구현한다면 이 함수를 쓰게 된다. */
	mutex_lock(&epc->lock);
	ret = epc->ops->map_addr(epc, func_no, vfunc_no, phys_addr, pci_addr,
				 size);
	mutex_unlock(&epc->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(pci_epc_map_addr);

/**
 * pci_epc_mem_map() - allocate and map a PCI address to a CPU address
 * @epc: the EPC device on which the CPU address is to be allocated and mapped
 * @func_no: the physical endpoint function number in the EPC device
 * @vfunc_no: the virtual endpoint function number in the physical function
 * @pci_addr: PCI address to which the CPU address should be mapped
 * @pci_size: the number of bytes to map starting from @pci_addr
 * @map: where to return the mapping information
 *
 * Allocate a controller memory address region and map it to a RC PCI address
 * region, taking into account the controller physical address mapping
 * constraints using the controller operation align_addr(). If this operation is
 * not defined, we assume that there are no alignment constraints for the
 * mapping.
 *
 * The effective size of the PCI address range mapped from @pci_addr is
 * indicated by @map->pci_size. This size may be less than the requested
 * @pci_size. The local virtual CPU address for the mapping is indicated by
 * @map->virt_addr (@map->phys_addr indicates the physical address).
 * The size and CPU address of the controller memory allocated and mapped are
 * respectively indicated by @map->map_size and @map->virt_base (and
 * @map->phys_base for the physical address of @map->virt_base).
 *
 * Returns 0 on success and a negative error code in case of error.
 */
int pci_epc_mem_map(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
		    u64 pci_addr, size_t pci_size, struct pci_epc_map *map)
{
	/* [한국어] 실제로 매핑할 크기. 아래 align_addr 이 정렬 때문에
	 * 이 값을 키울 수 있다. */
	size_t map_size = pci_size;
	/* [한국어] 정렬된 시작점에서 실제로 원하는 주소까지의 거리.
	 * align_addr 이 채워 주며, 아래에서 virt_addr 을 구하는 데 쓴다. */
	size_t map_offset = 0;
	int ret;

	if (!pci_epc_function_is_valid(epc, func_no, vfunc_no))
		return -EINVAL;

	/* [한국어] 크기가 0 이거나 결과를 담을 곳이 없으면 할 일이 없다. */
	if (!pci_size || !map)
		return -EINVAL;

	/*
	 * Align the PCI address to map. If the controller defines the
	 * .align_addr() operation, use it to determine the PCI address to map
	 * and the size of the mapping. Otherwise, assume that the controller
	 * has no alignment constraint.
	 */
	/* [한국어] 이 함수가 pci_epc_map_addr() 위에 한 겹 더 있는 이유가
	 * 여기 있다 — 하드웨어의 정렬 제약을 흡수하는 것이다.
	 *
	 * 많은 컨트롤러가 아웃바운드 변환을 임의의 주소에 걸 수 없고,
	 * 예컨대 64KB 경계에만 걸 수 있다. 그러면 호스트의 0x1234 를
	 * 읽고 싶어도 0x0000 부터 매핑해야 하고, 원하는 곳은 그 안의
	 * 0x1234 지점이 된다. 그 계산을 EPF 드라이버마다 하지 않도록
	 * 여기서 처리한다.
	 *
	 * align_addr 이 없는 컨트롤러는 제약이 없다고 보고 그대로 쓴다. */
	memset(map, 0, sizeof(*map));
	/* [한국어] 호출자가 원한 주소를 그대로 기록해 둔다. */
	map->pci_addr = pci_addr;
	if (epc->ops->align_addr)
		/* [한국어] 정렬된 시작 주소를 돌려받고, map_size 와 map_offset 은
		 * 참조로 갱신된다. map_size 는 커질 수 있고 map_offset 은
		 * 정렬 때문에 앞으로 밀린 거리다. */
		map->map_pci_addr =
			epc->ops->align_addr(epc, pci_addr,
					     &map_size, &map_offset);
	else
		map->map_pci_addr = pci_addr;
	map->map_size = map_size;
	/* [한국어] 실제로 쓸 수 있는 크기를 정한다.
	 * 매핑이 끝나는 지점이 요청 끝보다 앞이면, 요청한 만큼을 다 덮지
	 * 못한 것이다. 그럴 때는 덮은 만큼만 알려 준다 — 상류 kernel-doc 이
	 * "@map->pci_size 가 요청보다 작을 수 있다" 고 밝힌 부분이다.
	 * 호출자는 이 값을 확인해 남은 부분을 다시 매핑해야 한다. */
	if (map->map_pci_addr + map->map_size < pci_addr + pci_size)
		map->pci_size = map->map_pci_addr + map->map_size - pci_addr;
	else
		map->pci_size = pci_size;

	/* [한국어] 창에서 자리를 빌린다. 정렬 때문에 커졌을 수 있는
	 * map_size 만큼 필요하다. */
	map->virt_base = pci_epc_mem_alloc_addr(epc, &map->phys_base,
						map->map_size);
	if (!map->virt_base)
		return -ENOMEM;

	/* [한국어] base 는 매핑의 시작, addr 은 호출자가 실제로 원한 지점이다.
	 * 둘 사이가 map_offset 만큼 떨어져 있다.
	 * 호출자는 virt_addr 로 읽고 쓰면 되고, base 쪽은 나중에 해제할 때
	 * 필요하다 — 해제는 매핑 단위로 해야 하기 때문이다. */
	map->phys_addr = map->phys_base + map_offset;
	map->virt_addr = map->virt_base + map_offset;

	/* [한국어] 빌린 자리를 정렬된 호스트 주소에 연결한다.
	 * base 와 map_size 를 넘기는 점에 주의 — 호출자가 원한 지점이 아니라
	 * 정렬된 시작점 기준으로 하드웨어를 설정해야 한다. */
	ret = pci_epc_map_addr(epc, func_no, vfunc_no, map->phys_base,
			       map->map_pci_addr, map->map_size);
	if (ret) {
		/* [한국어] 연결에 실패했으면 빌린 자리도 되돌린다.
		 * 이 정리가 없으면 창이 조금씩 새어 결국 매핑을 못 하게 된다. */
		pci_epc_mem_free_addr(epc, map->phys_base, map->virt_base,
				      map->map_size);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(pci_epc_mem_map);

/**
 * pci_epc_mem_unmap() - unmap and free a CPU address region
 * @epc: the EPC device on which the CPU address is allocated and mapped
 * @func_no: the physical endpoint function number in the EPC device
 * @vfunc_no: the virtual endpoint function number in the physical function
 * @map: the mapping information
 *
 * Unmap and free a CPU address region that was allocated and mapped with
 * pci_epc_mem_map().
 */
void pci_epc_mem_unmap(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
		       struct pci_epc_map *map)
{
	if (!pci_epc_function_is_valid(epc, func_no, vfunc_no))
		return;

	/* [한국어] virt_base 가 매핑 성립의 표시다. pci_epc_mem_map() 이
	 * 실패했다면 memset 으로 0 이 되어 있으므로, 실패한 map 을 그대로
	 * 넘겨도 안전하다. */
	if (!map || !map->virt_base)
		return;

	/* [한국어] map 의 역순 — 먼저 연결을 끊고, 그다음 자리를 반납한다.
	 * 순서를 지켜야 하는 이유는 연결이 살아 있는 자리를 반납하면
	 * 다른 EPF 가 그 자리를 받았을 때 엉뚱한 호스트 주소에 연결된
	 * 상태를 물려받기 때문이다.
	 * base 를 쓰는 점에 주의 — 호출자가 쓰던 addr 이 아니라 매핑의
	 * 시작점 기준으로 해제해야 한다. */
	pci_epc_unmap_addr(epc, func_no, vfunc_no, map->phys_base);
	pci_epc_mem_free_addr(epc, map->phys_base, map->virt_base,
			      map->map_size);
}
EXPORT_SYMBOL_GPL(pci_epc_mem_unmap);

/**
 * pci_epc_clear_bar() - reset the BAR
 * @epc: the EPC device for which the BAR has to be cleared
 * @func_no: the physical endpoint function number in the EPC device
 * @vfunc_no: the virtual endpoint function number in the physical function
 * @epf_bar: the struct epf_bar that contains the BAR information
 *
 * Invoke to reset the BAR of the endpoint device.
 */
void pci_epc_clear_bar(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
		       struct pci_epf_bar *epf_bar)
{
	if (!pci_epc_function_is_valid(epc, func_no, vfunc_no))
		return;

	/* [한국어] BAR 5 를 64비트로 지정한 것은 애초에 불가능한 조합이다.
	 * 64비트 BAR 는 연속한 두 자리를 쓰는데 BAR 5 가 마지막이라
	 * 그 위가 없기 때문이다. 설정될 수 없었던 것이므로 지울 것도 없다.
	 * 아래 set_bar 도 같은 조합을 -EINVAL 로 막는다. */
	if (epf_bar->barno == BAR_5 &&
	    epf_bar->flags & PCI_BASE_ADDRESS_MEM_TYPE_64)
		return;

	if (!epc->ops->clear_bar)
		return;

	/* [한국어] BAR 를 없앤다. 호스트가 다시 열거하면 이 BAR 는 보이지
	 * 않게 된다. */
	mutex_lock(&epc->lock);
	epc->ops->clear_bar(epc, func_no, vfunc_no, epf_bar);
	mutex_unlock(&epc->lock);
}
EXPORT_SYMBOL_GPL(pci_epc_clear_bar);

/**
 * pci_epc_set_bar() - configure BAR in order for host to assign PCI addr space
 * @epc: the EPC device on which BAR has to be configured
 * @func_no: the physical endpoint function number in the EPC device
 * @vfunc_no: the virtual endpoint function number in the physical function
 * @epf_bar: the struct epf_bar that contains the BAR information
 *
 * Invoke to configure the BAR of the endpoint device.
 */
int pci_epc_set_bar(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
		    struct pci_epf_bar *epf_bar)
{
	/* [한국어] 이 EPC 가 BAR 에 대해 무엇을 허용하는지 담은 표. 아래 검증들이 이것을 본다. */
	const struct pci_epc_features *epc_features;
	/* [한국어] 자주 쓰므로 지역 변수로 꺼내 둔다. bar 는 BAR 번호, flags 는 타입 비트. */
	enum pci_barno bar = epf_bar->barno;
	int flags = epf_bar->flags;
	int ret;

	/* [한국어] 이 파일에서 검증이 가장 많은 함수다. BAR 는 호스트가
	 * 그대로 믿고 주소 공간을 배정하는 값이라, 잘못된 설정이 나가면
	 * 호스트 쪽 열거가 깨지기 때문이다.
	 *
	 * 먼저 이 EPC 의 능력을 가져온다. pci_epc_function_is_valid() 를
	 * 직접 부르지 않는 것은 get_features() 안에 이미 들어 있어서다. */
	epc_features = pci_epc_get_features(epc, func_no, vfunc_no);
	if (!epc_features)
		return -EINVAL;

	/* [한국어] 서브맵을 쓰겠다면서 서브맵 배열이 없으면 앞뒤가 안 맞는다. */
	if (epf_bar->num_submap && !epf_bar->submap)
		return -EINVAL;

	/* [한국어] 서브맵은 BAR 하나를 여러 조각으로 나눠 각각 다른 호스트
	 * 주소에 연결하는 기능이다. 하드웨어가 동적 인바운드 매핑과
	 * 부분 범위 매핑을 둘 다 지원해야 성립하므로, 하나라도 없으면 거절한다. */
	if (epf_bar->num_submap &&
	    !(epc_features->dynamic_inbound_mapping &&
	      epc_features->subrange_mapping))
		return -EINVAL;

	/* [한국어] Resizable BAR 의 크기 제한. PCIe r6.0 §7.8.6.2 가 최소
	 * 1MB, 최대 128TB 로 정하고 있다(SZ_128G * 1024 = 128TB).
	 * 아래 pci_epc_bar_size_to_rebar_cap() 과 같은 범위다. */
	if (epc_features->bar[bar].type == BAR_RESIZABLE &&
	    (epf_bar->size < SZ_1M || (u64)epf_bar->size > (SZ_128G * 1024)))
		return -EINVAL;

	/* [한국어] 크기가 고정된 BAR 라면 그 값과 정확히 같아야 한다.
	 * 하드웨어가 그 크기로 배선되어 있어 소프트웨어가 바꿀 수 없다. */
	if (epc_features->bar[bar].type == BAR_FIXED &&
	    (epc_features->bar[bar].fixed_size != epf_bar->size))
		return -EINVAL;

	/* [한국어] BAR 크기는 반드시 2의 거듭제곱이어야 한다. BAR 레지스터가
	 * 크기를 하위 비트의 개수로 표현하는 구조라 그 외의 값은 아예
	 * 표현할 수 없다 — 호스트는 BAR 에 전부 1 을 써 본 뒤 되읽어
	 * 0 으로 남은 하위 비트 수로 크기를 알아낸다. */
	if (!is_power_of_2(epf_bar->size))
		return -EINVAL;

	/* [한국어] 세 가지 모순된 조합을 한 번에 거른다.
	 *   1) BAR 5 + 64비트 — 위 clear_bar 에서 설명한 대로 그 위 자리가 없다.
	 *   2) I/O 공간인데 주소 비트가 켜져 있음 — flags 의 하위 비트는
	 *      크기가 아니라 종류를 나타내는 자리라 0 이어야 한다.
	 *   3) 크기가 4GB 를 넘는데 64비트 플래그가 없음 — 32비트 BAR 로는
	 *      그만한 공간을 표현할 수 없다. */
	if ((epf_bar->barno == BAR_5 && flags & PCI_BASE_ADDRESS_MEM_TYPE_64) ||
	    (flags & PCI_BASE_ADDRESS_SPACE_IO &&
	     flags & PCI_BASE_ADDRESS_IO_MASK) ||
	    (upper_32_bits(epf_bar->size) &&
	     !(flags & PCI_BASE_ADDRESS_MEM_TYPE_64)))
		return -EINVAL;

	if (!epc->ops->set_bar)
		return 0;

	/* [한국어] 검증을 다 통과했으니 하드웨어에 반영한다.
	 * 이 호출 뒤 호스트가 열거하면 이 BAR 가 보이고, 호스트는 거기에
	 * 주소 공간을 배정한다. 호스트 쪽 NVMe 드라이버가
	 * pci_resource_start(pdev, 0) 으로 얻는 값의 반대편이 이것이다. */
	mutex_lock(&epc->lock);
	ret = epc->ops->set_bar(epc, func_no, vfunc_no, epf_bar);
	mutex_unlock(&epc->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(pci_epc_set_bar);

/**
 * pci_epc_bar_size_to_rebar_cap() - convert a size to the representation used
 *				     by the Resizable BAR Capability Register
 * @size: the size to convert
 * @cap: where to store the result
 *
 * Returns 0 on success and a negative error code in case of error.
 */
int pci_epc_bar_size_to_rebar_cap(size_t size, u32 *cap)
{
	/*
	 * As per PCIe r6.0, sec 7.8.6.2, min size for a resizable BAR is 1 MB,
	 * thus disallow a requested BAR size smaller than 1 MB.
	 * Disallow a requested BAR size larger than 128 TB.
	 */
	/* [한국어] 상류 주석이 근거를 밝히고 있다. SZ_128G * 1024 가 128TB 다. */
	if (size < SZ_1M || (u64)size > (SZ_128G * 1024))
		return -EINVAL;

	/* [한국어] Resizable BAR Capability 레지스터는 지원 가능한 크기들을
	 * 비트마스크로 광고한다. 각 비트가 하나의 크기를 뜻하며, 규격이
	 * 그 대응을 정해 두었다.
	 *
	 * 두 단계로 변환한다. 먼저 1MB 를 기준으로 몇 배인지를 log2 로 구한다.
	 * 예컨대 1MB 면 0, 2MB 면 1, 4MB 면 2 다. */
	*cap = ilog2(size) - ilog2(SZ_1M);

	/* Sizes in REBAR_CAP start at BIT(4). */
	/* [한국어] 그다음 그 값을 비트 위치로 바꾼다. 상류 주석대로 1MB 가
	 * 비트 4 에서 시작하므로 4 를 더한다 — 비트 0..3 은 규격이 다른
	 * 용도로 예약해 두었기 때문이다.
	 * 결과적으로 1MB → BIT(4), 2MB → BIT(5), 4MB → BIT(6) 이 된다. */
	*cap = BIT(*cap + 4);

	return 0;
}
EXPORT_SYMBOL_GPL(pci_epc_bar_size_to_rebar_cap);

/**
 * pci_epc_write_header() - write standard configuration header
 * @epc: the EPC device to which the configuration header should be written
 * @func_no: the physical endpoint function number in the EPC device
 * @vfunc_no: the virtual endpoint function number in the physical function
 * @header: standard configuration header fields
 *
 * Invoke to write the configuration header to the endpoint controller. Every
 * endpoint controller will have a dedicated location to which the standard
 * configuration header would be written. The callback function should write
 * the header fields to this dedicated location.
 */
int pci_epc_write_header(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			 struct pci_epf_header *header)
{
	/* [한국어] ops->write_header 의 결과. */
	int ret;

	/* [한국어] 헤더 쓰기도 함수 단위 동작이므로 번호를 검증한다. */
	if (!pci_epc_function_is_valid(epc, func_no, vfunc_no))
		return -EINVAL;

	/* Only Virtual Function #1 has deviceID */
	/* [한국어] 상류 주석의 근거는 SR-IOV 의 구조다. VF 들은 config
	 * 헤더를 대부분 공유하고 개별 Device ID 를 갖지 않는다 —
	 * VF Device ID 는 PF 의 SR-IOV capability 에 하나만 있고 모든 VF 가
	 * 그것을 쓴다. 그래서 VF 마다 헤더를 쓰는 것은 의미가 없고,
	 * 여기서는 1번까지만 허용한다. */
	if (vfunc_no > 1)
		return -EINVAL;

	if (!epc->ops->write_header)
		return 0;

	/* [한국어] config space 헤더를 쓴다. 이 장치가 호스트에게 무엇으로
	 * 보일지를 정하는 가장 근본적인 설정이다 — 벤더 ID, 디바이스 ID,
	 * 클래스 코드가 여기 들어간다.
	 *
	 * 호스트 쪽에서 이 값들이 어떻게 쓰이는지 보면 이해가 쉽다.
	 * 클래스 코드를 0x010802(NVM Express)로 적으면 호스트의 PCI 코어가
	 * 그 값으로 드라이버를 찾고, nvme 모듈의 device id 표와 맞으면
	 * nvme_probe() 가 불린다. 즉 이 한 번의 쓰기가 저쪽에서 어떤
	 * 드라이버가 붙을지를 결정한다. */
	mutex_lock(&epc->lock);
	ret = epc->ops->write_header(epc, func_no, vfunc_no, header);
	mutex_unlock(&epc->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(pci_epc_write_header);

/**
 * pci_epc_add_epf() - bind PCI endpoint function to an endpoint controller
 * @epc: the EPC device to which the endpoint function should be added
 * @epf: the endpoint function to be added
 * @type: Identifies if the EPC is connected to the primary or secondary
 *        interface of EPF
 *
 * A PCI endpoint device can have one or more functions. In the case of PCIe,
 * the specification allows up to 8 PCIe endpoint functions. Invoke
 * pci_epc_add_epf() to add a PCI endpoint function to an endpoint controller.
 */
int pci_epc_add_epf(struct pci_epc *epc, struct pci_epf *epf,
		    enum pci_epc_interface_type type)
{
	/* [한국어] 이 EPF 를 매달 연결 리스트 노드. 아래에서 인터페이스 종류에 따라
	 * epf->list 또는 epf->sec_epc_list 중 하나를 가리키게 된다. */
	struct list_head *list;
	u32 func_no;
	int ret = 0;

	/* [한국어] VF 는 이 함수로 붙일 수 없다. VF 는 PF 에 종속되어
	 * 자동으로 만들어지는 것이지 독립적으로 EPC 에 등록되는 존재가
	 * 아니기 때문이다. */
	if (IS_ERR_OR_NULL(epc) || epf->is_vf)
		return -EINVAL;

	/* [한국어] 이미 그쪽 인터페이스에 붙어 있으면 거절한다.
	 * EPF 하나가 EPC 두 개(primary/secondary)에 붙을 수는 있지만,
	 * 같은 쪽에 둘을 붙일 수는 없다. */
	if (type == PRIMARY_INTERFACE && epf->epc)
		return -EBUSY;

	if (type == SECONDARY_INTERFACE && epf->sec_epc)
		return -EBUSY;

	/* [한국어] 함수 번호 배정과 목록 조작을 함께 보호한다.
	 * epc->lock 이 아니라 list_lock 인 점에 주의 — 이 파일에는 락이
	 * 둘이며, epc->lock 은 하드웨어 ops 호출을, list_lock 은 EPF 목록과
	 * 함수 번호 비트맵을 지킨다. 둘을 나눈 덕에 오래 걸리는 하드웨어
	 * 조작이 목록 조회를 막지 않는다. */
	mutex_lock(&epc->list_lock);
	/* [한국어] 비어 있는 함수 번호를 찾는다. 비트맵의 한 비트가 함수
	 * 하나이며, 0 인 자리가 아직 아무도 쓰지 않는 번호다. */
	func_no = find_first_zero_bit(&epc->function_num_map,
				      BITS_PER_LONG);
	/* [한국어] 비트맵이 unsigned long 하나라 최대 32/64 개가 상한이다.
	 * 다 찼으면 더 붙일 수 없다. */
	if (func_no >= BITS_PER_LONG) {
		ret = -EINVAL;
		goto ret;
	}

	/* [한국어] 비트맵에는 자리가 있어도 하드웨어가 지원하는 함수 수를
	 * 넘으면 안 된다. PCIe 는 함수 8개까지 허용하지만 실제 컨트롤러는
	 * 그보다 적을 수 있다. */
	if (func_no > epc->max_functions - 1) {
		dev_err(&epc->dev, "Exceeding max supported Function Number\n");
		ret = -EINVAL;
		goto ret;
	}

	/* [한국어] 그 번호를 쓴 것으로 표시한다. */
	set_bit(func_no, &epc->function_num_map);
	/* [한국어] 어느 인터페이스냐에 따라 EPF 안의 다른 필드에 기록한다.
	 * EPF 하나가 두 EPC 에 붙을 수 있어 필드가 두 벌이다 —
	 * NTB 처럼 양쪽 호스트 사이에 끼는 구성이 그것을 쓴다.
	 * 연결 리스트 노드도 두 개(list, sec_epc_list)라, 같은 EPF 가
	 * 두 EPC 의 목록에 동시에 들어갈 수 있다. */
	if (type == PRIMARY_INTERFACE) {
		epf->func_no = func_no;
		epf->epc = epc;
		/* [한국어] primary 쪽 리스트 노드를 쓴다. */
		list = &epf->list;
	} else {
		/* [한국어] secondary 쪽은 필드 이름이 다르다. EPF 하나가 EPC 둘에 붙을 수 있어
		 * 번호와 포인터를 두 벌로 갖는다. */
		epf->sec_epc_func_no = func_no;
		/* [한국어] 이 값이 NULL 이 아니게 되는 것이 secondary 에 붙었다는 표시다. */
		epf->sec_epc = epc;
		list = &epf->sec_epc_list;
	}

	/* [한국어] EPC 의 EPF 목록 끝에 매단다. 순서를 유지하는 이유는
	 * pci-ep-msi.c 처럼 "첫 번째 EPF" 를 특별히 다루는 코드가 있어서다. */
	list_add_tail(list, &epc->pci_epf);
ret:
	mutex_unlock(&epc->list_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(pci_epc_add_epf);

/**
 * pci_epc_remove_epf() - remove PCI endpoint function from endpoint controller
 * @epc: the EPC device from which the endpoint function should be removed
 * @epf: the endpoint function to be removed
 * @type: identifies if the EPC is connected to the primary or secondary
 *        interface of EPF
 *
 * Invoke to remove PCI endpoint function from the endpoint controller.
 */
void pci_epc_remove_epf(struct pci_epc *epc, struct pci_epf *epf,
			enum pci_epc_interface_type type)
{
	/* [한국어] 제거할 리스트 노드. add 때와 마찬가지로 인터페이스에 따라 갈린다. */
	struct list_head *list;
	/* [한국어] 반납할 함수 번호. 0 으로 초기화해 두어 아래 분기에서 반드시 덮인다. */
	u32 func_no = 0;

	if (IS_ERR_OR_NULL(epc) || !epf)
		return;

	/* [한국어] add 의 정확한 역순이다. 같은 list_lock 으로 보호한다. */
	mutex_lock(&epc->list_lock);
	/* [한국어] 어느 인터페이스인지에 따라 다른 필드를 본다.
	 * EPC 포인터를 NULL 로 만드는 것이 "떨어졌음" 의 표시이며,
	 * add 의 -EBUSY 검사가 이 값을 본다. */
	if (type == PRIMARY_INTERFACE) {
		func_no = epf->func_no;
		list = &epf->list;
		/* [한국어] NULL 로 만드는 것이 '떨어졌음' 의 표시다. add 의 -EBUSY 검사가 이것을 본다. */
		epf->epc = NULL;
	} else {
		/* [한국어] secondary 쪽 번호를 꺼낸다. */
		func_no = epf->sec_epc_func_no;
		list = &epf->sec_epc_list;
		epf->sec_epc = NULL;
	}
	/* [한국어] 함수 번호를 반납해 다음 EPF 가 쓸 수 있게 한다. */
	clear_bit(func_no, &epc->function_num_map);
	/* [한국어] 목록에서 뺀다. */
	list_del(list);
	mutex_unlock(&epc->list_lock);
}
EXPORT_SYMBOL_GPL(pci_epc_remove_epf);

/**
 * pci_epc_linkup() - Notify the EPF device that EPC device has established a
 *		      connection with the Root Complex.
 * @epc: the EPC device which has established link with the host
 *
 * Invoke to Notify the EPF device that the EPC device has established a
 * connection with the Root Complex.
 */
void pci_epc_linkup(struct pci_epc *epc)
{
	/* [한국어] 목록을 순회할 때 쓸 커서. */
	struct pci_epf *epf;

	if (IS_ERR_OR_NULL(epc))
		return;

	/* [한국어] 이 아래 통지 함수 넷(linkup / linkdown / init_notify /
	 * deinit_notify / bus_master_enable_notify)이 모두 같은 모양이다.
	 * 목록을 잠그고, EPF 마다 자기 락을 잡고, 콜백이 있으면 부른다.
	 *
	 * 락을 두 겹으로 잡는 이유가 있다. list_lock 은 순회 중에 EPF 가
	 * 붙거나 떨어지는 것을 막고, epf->lock 은 그 EPF 의 상태가 콜백
	 * 실행 중에 바뀌는 것을 막는다. 보호하는 대상이 다르다.
	 *
	 * 순서가 항상 list_lock → epf->lock 이라는 점이 중요하다. 반대로
	 * 잡는 곳이 있으면 교착이 생기는데, 이 파일 안에서는 그런 곳이 없다. */
	mutex_lock(&epc->list_lock);
	list_for_each_entry(epf, &epc->pci_epf, list) {
		mutex_lock(&epf->lock);
		/* [한국어] 콜백 표가 아예 없거나 이 사건에 관심이 없는 EPF 도
		 * 있으므로 둘 다 확인한다. */
		if (epf->event_ops && epf->event_ops->link_up)
			epf->event_ops->link_up(epf);
		mutex_unlock(&epf->lock);
	}
	mutex_unlock(&epc->list_lock);
}
EXPORT_SYMBOL_GPL(pci_epc_linkup);

/**
 * pci_epc_linkdown() - Notify the EPF device that EPC device has dropped the
 *			connection with the Root Complex.
 * @epc: the EPC device which has dropped the link with the host
 *
 * Invoke to Notify the EPF device that the EPC device has dropped the
 * connection with the Root Complex.
 */
void pci_epc_linkdown(struct pci_epc *epc)
{
	/* [한국어] 목록 순회용 커서. */
	struct pci_epf *epf;

	if (IS_ERR_OR_NULL(epc))
		return;

	/* [한국어] 위 linkup 과 같은 구조. 링크가 끊어졌음을 알린다.
	 * EPF 는 이 통지를 받으면 진행 중이던 전송을 접고 호스트 메모리
	 * 매핑을 정리해야 한다 — 링크가 없는 동안 그 주소에 접근하면
	 * 오류가 나기 때문이다. */
	mutex_lock(&epc->list_lock);
	list_for_each_entry(epf, &epc->pci_epf, list) {
		mutex_lock(&epf->lock);
		/* [한국어] 링크가 끊어졌음을 알리는 콜백. 관심 없는 EPF 는 이 필드가 NULL 이다. */
		if (epf->event_ops && epf->event_ops->link_down)
			epf->event_ops->link_down(epf);
		mutex_unlock(&epf->lock);
	}
	mutex_unlock(&epc->list_lock);
}
EXPORT_SYMBOL_GPL(pci_epc_linkdown);

/**
 * pci_epc_init_notify() - Notify the EPF device that EPC device initialization
 *                         is completed.
 * @epc: the EPC device whose initialization is completed
 *
 * Invoke to Notify the EPF device that the EPC device's initialization
 * is completed.
 */
void pci_epc_init_notify(struct pci_epc *epc)
{
	/* [한국어] 목록 순회용 커서. */
	struct pci_epf *epf;

	if (IS_ERR_OR_NULL(epc))
		return;

	/* [한국어] EPC 하드웨어 초기화가 끝났음을 알린다. EPF 는 이때부터
	 * BAR 를 만들고 config 헤더를 쓸 수 있다 — 그 전에는 하드웨어가
	 * 준비되지 않아 설정이 먹지 않는다. */
	mutex_lock(&epc->list_lock);
	list_for_each_entry(epf, &epc->pci_epf, list) {
		mutex_lock(&epf->lock);
		/* [한국어] EPC 초기화 완료를 알리는 콜백. EPF 는 이때부터 BAR 를 설정할 수 있다. */
		if (epf->event_ops && epf->event_ops->epc_init)
			epf->event_ops->epc_init(epf);
		mutex_unlock(&epf->lock);
	}
	/* [한국어] 초기화가 끝났다는 사실을 기록해 둔다. 이 플래그가
	 * 아래 pci_epc_notify_pending_init() 의 근거가 된다 —
	 * 이 통지 이후에 붙는 EPF 는 사건을 놓치므로, 그 사실을 기억했다가
	 * 늦게 온 EPF 에게 따로 알려 준다.
	 *
	 * 목록 순회를 마친 뒤에 세우는 순서가 중요하다. 순회 중에 세우면
	 * 그 사이 붙은 EPF 가 두 번 통지받을 수 있고, list_lock 을 쥐고
	 * 있으므로 실제로는 그럴 수 없지만 의미상으로도 "모두에게 알린 뒤" 가
	 * 맞다. */
	epc->init_complete = true;
	mutex_unlock(&epc->list_lock);
}
EXPORT_SYMBOL_GPL(pci_epc_init_notify);

/**
 * pci_epc_notify_pending_init() - Notify the pending EPC device initialization
 *                                 complete to the EPF device
 * @epc: the EPC device whose initialization is pending to be notified
 * @epf: the EPF device to be notified
 *
 * Invoke to notify the pending EPC device initialization complete to the EPF
 * device. This is used to deliver the notification if the EPC initialization
 * got completed before the EPF driver bind.
 */
void pci_epc_notify_pending_init(struct pci_epc *epc, struct pci_epf *epf)
{
	/* [한국어] 이 함수가 푸는 문제가 이 파일에서 가장 실용적이다.
	 *
	 * EPC 초기화와 EPF 바인드는 순서가 정해져 있지 않다. 사용자가
	 * configfs 로 EPF 를 만드는 시점이 EPC 초기화보다 늦을 수 있다.
	 * 그러면 그 EPF 는 위 pci_epc_init_notify() 의 순회에 없었으므로
	 * epc_init 통지를 영영 받지 못하고, BAR 도 설정하지 못한 채
	 * 아무 일도 하지 않게 된다.
	 *
	 * 그래서 EPC 가 "이미 초기화됐다" 를 기억해 두었다가, 늦게 붙은
	 * EPF 에게 바인드 직후 이 함수로 따로 알려 준다.
	 *
	 * 위 통지 함수들과 달리 list_lock 을 잡지 않는다. 목록을 순회하지
	 * 않고 지정된 EPF 하나만 다루기 때문이다. 또 이 함수를 부르는
	 * 쪽(EPF 바인드 경로)이 이미 목록 관련 보호 아래 있다. */
	if (epc->init_complete) {
		mutex_lock(&epf->lock);
		if (epf->event_ops && epf->event_ops->epc_init)
			epf->event_ops->epc_init(epf);
		mutex_unlock(&epf->lock);
	}
}
EXPORT_SYMBOL_GPL(pci_epc_notify_pending_init);

/**
 * pci_epc_deinit_notify() - Notify the EPF device about EPC deinitialization
 * @epc: the EPC device whose deinitialization is completed
 *
 * Invoke to notify the EPF device that the EPC deinitialization is completed.
 */
void pci_epc_deinit_notify(struct pci_epc *epc)
{
	/* [한국어] 목록 순회용 커서. */
	struct pci_epf *epf;

	/* [한국어] 정리 경로에서도 불릴 수 있어 포인터를 먼저 확인한다. */
	if (IS_ERR_OR_NULL(epc))
		return;

	mutex_lock(&epc->list_lock);
	/* [한국어] 붙어 있는 모든 EPF 에게 알린다. */
	list_for_each_entry(epf, &epc->pci_epf, list) {
		mutex_lock(&epf->lock);
		/* [한국어] EPC 가 해제됐음을 알리는 콜백. EPF 는 잡아 둔 자원을 놓아야 한다. */
		if (epf->event_ops && epf->event_ops->epc_deinit)
			epf->event_ops->epc_deinit(epf);
		mutex_unlock(&epf->lock);
	}
	/* [한국어] 초기화 표시를 내린다. 이제 늦게 붙는 EPF 에게
	 * pci_epc_notify_pending_init() 이 통지를 보내지 않는다 —
	 * 실제로 초기화되지 않은 상태이므로 그것이 맞다.
	 * 나중에 다시 초기화되면 pci_epc_init_notify() 가 이 값을 다시 세운다. */
	epc->init_complete = false;
	mutex_unlock(&epc->list_lock);
}
EXPORT_SYMBOL_GPL(pci_epc_deinit_notify);

/**
 * pci_epc_bus_master_enable_notify() - Notify the EPF device that the EPC
 *					device has received the Bus Master
 *					Enable event from the Root complex
 * @epc: the EPC device that received the Bus Master Enable event
 *
 * Notify the EPF device that the EPC device has generated the Bus Master Enable
 * event due to host setting the Bus Master Enable bit in the Command register.
 */
void pci_epc_bus_master_enable_notify(struct pci_epc *epc)
{
	/* [한국어] 목록 순회용 커서. */
	struct pci_epf *epf;

	/* [한국어] 다른 통지 함수들과 같은 방어적 검사. */
	if (IS_ERR_OR_NULL(epc))
		return;

	mutex_lock(&epc->list_lock);
	list_for_each_entry(epf, &epc->pci_epf, list) {
		mutex_lock(&epf->lock);
		/* [한국어] 호스트가 Command 레지스터의 Bus Master Enable 비트를
		 * 켰다는 통지다. 이 비트의 의미가 중요하다 — 그 전까지 이 장치는
		 * DMA 를 낼 수 없다. 규격이 그렇게 정해 두어, 드라이버가 준비되기
		 * 전에 장치가 멋대로 메모리에 쓰는 것을 막는다.
		 *
		 * 호스트 쪽 NVMe 드라이버가 pci_set_master() 를 부르는 순간이
		 * 이 통지의 반대편이다. 그 호출 이후에야 NVMe 컨트롤러가 큐를
		 * 읽고 데이터를 옮길 수 있고, 마찬가지로 여기서도 이 통지를
		 * 받은 뒤에야 EPF 가 호스트 메모리에 접근할 수 있다. */
		if (epf->event_ops && epf->event_ops->bus_master_enable)
			epf->event_ops->bus_master_enable(epf);
		mutex_unlock(&epf->lock);
	}
	mutex_unlock(&epc->list_lock);
}
EXPORT_SYMBOL_GPL(pci_epc_bus_master_enable_notify);

/**
 * pci_epc_destroy() - destroy the EPC device
 * @epc: the EPC device that has to be destroyed
 *
 * Invoke to destroy the PCI EPC device
 */
void pci_epc_destroy(struct pci_epc *epc)
{
	/* [한국어] 먼저 configfs 그룹을 없앤다. 사용자가 더는 이 EPC 에
	 * EPF 를 붙일 수 없게 하는 것이 우선이다 — 아래에서 device 를
	 * 없애는 동안 새 EPF 가 들어오면 곤란하다. */
	pci_ep_cfs_remove_epc_group(epc->group);
#ifdef CONFIG_PCI_DOMAINS_GENERIC
	/* [한국어] 이 EPC 에 배정했던 PCI 도메인 번호를 반납한다.
	 * 아래 __pci_epc_create() 에서 받은 것과 짝이며, 같은 조건부
	 * 컴파일 안에 있어야 대칭이 맞는다. */
	pci_bus_release_domain_nr(epc->dev.parent, epc->domain_nr);
#endif
	/* [한국어] device 등록을 해제한다. 마지막 참조가 사라지면
	 * pci_epc_release() 가 불려 구조체가 해제된다. 즉 이 호출이
	 * 곧바로 메모리를 해제하지는 않는다 — 아직 pci_epc_get() 으로
	 * 참조를 쥔 코드가 있으면 그쪽이 놓을 때까지 살아 있다. */
	device_unregister(&epc->dev);
}
EXPORT_SYMBOL_GPL(pci_epc_destroy);

/* [한국어]
 * pci_epc_release - 마지막 참조가 사라졌을 때 EPC 구조체를 해제한다
 *
 * @dev: 해제될 device. 이것을 품은 struct pci_epc 를 해제해야 한다.
 * @return: 없음.
 *
 * 커널 device 모델의 규약이다. device 는 참조 카운트로 관리되며,
 * 마지막 참조가 사라지면 이 콜백이 불린다. 여기서 실제 메모리를 해제한다.
 *
 * pci_epc_destroy() 가 직접 kfree 하지 않고 이 경로를 거치는 이유가
 * 이것이다 — destroy 시점에 다른 코드가 pci_epc_get() 으로 참조를
 * 쥐고 있을 수 있고, 그때 해제하면 use-after-free 가 된다.
 *
 * 실행 컨텍스트: 마지막 put_device() 를 부른 문맥. 대개 프로세스 컨텍스트다.
 *
 * 호출 체인:
 *   (마지막 put_device) → device 모델 → [이 함수] → kfree()
 */
static void pci_epc_release(struct device *dev)
{
	/* [한국어] 임베디드된 device 에서 바깥 구조체를 되짚어 해제한다. */
	kfree(to_pci_epc(dev));
}

/**
 * __pci_epc_create() - create a new endpoint controller (EPC) device
 * @dev: device that is creating the new EPC
 * @ops: function pointers for performing EPC operations
 * @owner: the owner of the module that creates the EPC device
 *
 * Invoke to create a new EPC device and add it to pci_epc class.
 */
struct pci_epc *
__pci_epc_create(struct device *dev, const struct pci_epc_ops *ops,
		 struct module *owner)
{
	int ret;
	struct pci_epc *epc;

	/* [한국어] 부모 device 없이는 EPC 를 만들 수 없다. 이름도 그것에서
	 * 따오고 도메인 번호도 그것으로 찾기 때문이다.
	 * WARN_ON 을 쓴 것은 이것이 호출자의 프로그래밍 오류라 개발 중에
	 * 스택 트레이스와 함께 드러나야 하기 때문이다. */
	if (WARN_ON(!dev)) {
		ret = -EINVAL;
		goto err_ret;
	}

	/* [한국어] EPC 상태 구조체. 0 으로 초기화되므로 아래에서 명시적으로 채우지 않는
	 * 필드는 전부 0 이다. */
	epc = kzalloc_obj(*epc);
	/* [한국어] 메모리 부족. 아직 device_initialize 를 하지 않았으므로 put_device 가
	 * 아니라 곧바로 err_ret 으로 간다 — 해제할 것이 없다. */
	if (!epc) {
		ret = -ENOMEM;
		goto err_ret;
	}

	/* [한국어] 락 둘과 EPF 목록을 초기화한다. 앞서 본 대로 lock 은
	 * 하드웨어 ops 호출을, list_lock 은 EPF 목록과 함수 번호 비트맵을
	 * 보호한다. 둘을 나눈 덕에 오래 걸리는 하드웨어 조작이 목록 조회를
	 * 막지 않는다. */
	mutex_init(&epc->lock);
	mutex_init(&epc->list_lock);
	INIT_LIST_HEAD(&epc->pci_epf);

	/* [한국어] device 를 초기화하되 아직 등록하지는 않는다.
	 * initialize 와 add 를 나누면 그 사이에 필드를 채울 수 있고,
	 * 실패 시 put_device() 하나로 정리할 수 있다. */
	device_initialize(&epc->dev);
	/* [한국어] class 를 지정해야 /sys/class/pci_epc/ 아래에 나타나고,
	 * pci_epc_get() 이 이름으로 찾을 수 있게 된다. */
	epc->dev.class = &pci_epc_class;
	epc->dev.parent = dev;
	/* [한국어] 마지막 참조가 사라졌을 때 불릴 해제 함수. */
	epc->dev.release = pci_epc_release;
	/* [한국어] 컨트롤러 드라이버가 채운 함수 표. 이 파일의 모든 공개
	 * 함수가 결국 이 표를 통해 하드웨어에 닿는다. */
	epc->ops = ops;

#ifdef CONFIG_PCI_DOMAINS_GENERIC
	/* [한국어] 이 EPC 에 PCI 도메인 번호를 배정받는다. 엔드포인트인데
	 * 도메인 번호가 필요한 이유는, NTB 처럼 이쪽에서도 PCI 주소 공간을
	 * 다뤄야 하는 구성이 있고 그것들을 구분할 번호가 있어야 하기 때문이다.
	 * 첫 인자가 NULL 인 것은 특정 버스에 매이지 않았다는 뜻이다. */
	epc->domain_nr = pci_bus_find_domain_nr(NULL, dev);
#else
	/*
	 * TODO: If the architecture doesn't support generic PCI
	 * domains, then a custom implementation has to be used.
	 */
	/* [한국어] 상류 주석대로 아직 해결되지 않은 부분이다. 범용 도메인을
	 * 지원하지 않는 아키텍처에서는 domain_nr 이 0 인 채로 남으며,
	 * 그래서 경고를 남긴다. WARN_ONCE 라 부팅당 한 번만 찍힌다. */
	WARN_ONCE(1, "This architecture doesn't support generic PCI domains\n");
#endif

	/* [한국어] EPC 의 이름을 부모 device 의 이름과 같게 짓는다.
	 * 사용자가 configfs 에서 EPF 를 붙일 때 이 이름을 쓰므로,
	 * 하드웨어 이름과 같게 두는 편이 알아보기 쉽다. */
	ret = dev_set_name(&epc->dev, "%s", dev_name(dev));
	if (ret)
		goto put_dev;

	/* [한국어] 이제 실제로 등록한다. 이 순간부터 /sys/class/pci_epc/ 에
	 * 나타나고 pci_epc_get() 으로 찾을 수 있게 된다. */
	ret = device_add(&epc->dev);
	if (ret)
		goto put_dev;

	/* [한국어] configfs 그룹을 만들어 사용자가 EPF 를 연결할 수 있게 한다.
	 * 반환값을 확인하지 않는데, 실패해도 EPC 자체는 동작하고 configfs 를
	 * 통한 조작만 불가능해지기 때문이다. */
	epc->group = pci_ep_cfs_add_epc_group(dev_name(dev));

	return epc;

put_dev:
	/* [한국어] device_initialize() 이후의 실패는 put_device() 하나로
	 * 정리된다. 참조가 0 이 되어 pci_epc_release() 가 불리고 거기서
	 * kfree 까지 이뤄진다 — 그래서 여기서 따로 kfree 하지 않는다. */
	put_device(&epc->dev);

err_ret:
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(__pci_epc_create);

/**
 * __devm_pci_epc_create() - create a new endpoint controller (EPC) device
 * @dev: device that is creating the new EPC
 * @ops: function pointers for performing EPC operations
 * @owner: the owner of the module that creates the EPC device
 *
 * Invoke to create a new EPC device and add it to pci_epc class.
 * While at that, it also associates the device with the pci_epc using devres.
 * On driver detach, release function is invoked on the devres data,
 * then, devres data is freed.
 */
struct pci_epc *
__devm_pci_epc_create(struct device *dev, const struct pci_epc_ops *ops,
		      struct module *owner)
{
	struct pci_epc **ptr, *epc;

	/* [한국어] EPC 포인터 하나를 담을 devres 블록을 먼저 잡는다.
	 * 순서가 중요하다 — EPC 를 먼저 만들고 나서 이 할당이 실패하면
	 * 만든 것을 되돌려야 하지만, 이쪽을 먼저 하면 실패해도 정리할 것이
	 * 없다. devres 를 쓰는 함수들의 관용적인 순서다. */
	ptr = devres_alloc(devm_pci_epc_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	/* [한국어] 실제 생성은 공통 경로에 맡긴다. */
	epc = __pci_epc_create(dev, ops, owner);
	if (!IS_ERR(epc)) {
		/* [한국어] 성공했으면 만든 EPC 를 블록에 넣고 devres 에 등록한다.
		 * 이제 드라이버가 언바인드될 때 devm_pci_epc_release() 가
		 * 자동으로 불린다. */
		*ptr = epc;
		devres_add(dev, ptr);
	} else {
		/* [한국어] 실패했으면 잡아 둔 블록을 그냥 버린다.
		 * devres_add 를 하지 않았으므로 devres_free 로 직접 해제한다. */
		devres_free(ptr);
	}

	/* [한국어] 성공이든 실패든 __pci_epc_create() 의 결과를 그대로
	 * 돌려준다. 호출자는 IS_ERR 로 판단한다. */
	return epc;
}
EXPORT_SYMBOL_GPL(__devm_pci_epc_create);

/* [한국어]
 * pci_epc_init - 모듈 적재 시 pci_epc class 를 등록한다
 *
 * @return: class_register() 의 결과. 실패하면 모듈이 적재되지 않는다.
 *
 * 이 class 가 있어야 EPC 들이 /sys/class/pci_epc/ 아래에 모이고,
 * pci_epc_get() 이 이름으로 찾을 수 있다. 그래서 어떤 EPC 가 등록되기
 * 전에 반드시 먼저 실행되어야 하며, module_init 순서가 그것을 보장한다.
 *
 * 실행 컨텍스트: 모듈 적재 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (모듈 적재) → module_init → [이 함수] → class_register()
 */
static int __init pci_epc_init(void)
{
	/* [한국어] class 를 등록한다. 이 파일 맨 위에 정의한 pci_epc_class 다. */
	return class_register(&pci_epc_class);
}
module_init(pci_epc_init);

/* [한국어]
 * pci_epc_exit - 모듈 제거 시 class 등록을 해제한다
 *
 * @return: 없음.
 *
 * init 의 반대. 이 시점에는 등록된 EPC 가 없어야 한다 — 컨트롤러
 * 드라이버들이 이 모듈에 의존하므로 그것들이 먼저 내려간 뒤에야
 * 이 모듈이 제거될 수 있고, 모듈 의존 관계가 그 순서를 지켜 준다.
 *
 * 실행 컨텍스트: 모듈 제거 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (모듈 제거) → module_exit → [이 함수] → class_unregister()
 */
static void __exit pci_epc_exit(void)
{
	class_unregister(&pci_epc_class);
}
module_exit(pci_epc_exit);

MODULE_DESCRIPTION("PCI EPC Library");
MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
