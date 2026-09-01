// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Endpoint *Function* (EPF) library
 *
 * Copyright (C) 2017 Texas Instruments
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */

/*
 * [한국어 설명] 엔드포인트 함수의 수명과 메모리 관리 (pci-epf-core.c)
 *
 * === 파일의 역할 ===
 * 앞의 pci-epc-core.c 가 하드웨어(EPC) 쪽이라면 이 파일은 소프트웨어(EPF)
 * 쪽이다. "이 SoC 를 무엇처럼 보이게 할 것인가" 를 정하는 EPF 객체를
 * 만들고, 없애고, 그 드라이버와 짝지어 주는 일을 한다.
 *
 * 핵심 개념이 셋이다.
 *
 * 1) EPF 는 가상 버스 위의 장치다.
 *    이 파일은 pci_epf_bus_type 이라는 버스를 만든다. 실제 하드웨어 버스가
 *    아니라 커널 드라이버 모델을 빌려 쓰기 위한 것이다. 사용자가 configfs
 *    로 "pci_epf_test" 라는 EPF 를 만들면 그 이름의 장치가 이 버스에
 *    올라가고, 같은 이름을 지원하는 드라이버(functions/pci-epf-test.c)가
 *    자동으로 붙는다. USB 가젯이나 platform 버스와 같은 방식이다.
 *
 * 2) BAR 뒤의 메모리는 이 파일이 잡는다.
 *    호스트가 BAR 를 읽고 쓰면 그 접근이 엔드포인트 쪽 메모리에 닿아야
 *    한다. 그 메모리를 마련하는 것이 pci_epf_alloc_space() 다.
 *    그냥 kmalloc 이 아니라 dma_alloc_coherent 를 쓰는데, 호스트가
 *    DMA 로 직접 건드릴 영역이라 캐시 일관성이 보장되어야 하기 때문이다.
 *    또 BAR 크기는 2의 거듭제곱이어야 하고 하드웨어가 정한 최소 크기와
 *    정렬 제약도 있어서, 요청한 크기를 그대로 쓰지 못하고 보정한다
 *    (pci_epf_get_required_bar_size).
 *
 * 3) 가상 EPF(vEPF)로 다중 함수를 만든다.
 *    PCIe 장치 하나가 함수를 여러 개 가질 수 있는데(최대 8개), 그것을
 *    표현하는 것이 vEPF 다. 주 EPF 아래에 vEPF 를 매달면 호스트에는
 *    함수가 여러 개인 장치로 보인다. pci_epf_add_vepf() 가 그 연결을
 *    만들고, bind/unbind 때 주 EPF 와 함께 처리된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용자가 configfs 로 EPF 를 만든다
 *   -> pci_epf_create() [이 파일]
 *      -> pci_epf_bus_type 에 장치 등록
 *         -> 이름이 맞는 EPF 드라이버가 probe 됨
 *
 * 사용자가 그 EPF 를 EPC 에 연결하고 start 를 쓴다
 *   -> pci_epf_bind() [이 파일]
 *      -> vEPF 들을 먼저 처리한 뒤 EPF 드라이버의 bind 콜백
 *         -> 그 안에서 pci_epf_alloc_space() 로 BAR 메모리 확보
 *         -> pci_epc_set_bar() [pci-epc-core.c] 로 BAR 노출
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트. 뮤텍스와 DMA 할당이 있어
 *   잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci-ep-cfs.c(configfs 인터페이스)가 사용자 조작을 이리로 넘긴다.
 * 아래쪽: pci-epc-core.c(EPC 쪽), 커널 DMA API, 드라이버 모델.
 * 옆쪽: endpoint/functions/ 의 EPF 드라이버들이 이 파일의 버스에 등록한다.
 * 공유 상태: struct pci_epf — BAR 정보 배열, vEPF 목록, 붙어 있는 EPC.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(drivers/nvme 트리
 * 전수 확인 — pci_epf_* 호출 0건).
 *
 * 다만 pci_epf_alloc_space() 가 하는 일은 NVMe 를 공부한 사람에게
 * 익숙하다. 호스트 쪽 NVMe 드라이버가 큐를 만들 때 dma_alloc_coherent
 * 로 메모리를 잡고 그 주소를 컨트롤러에 알려 주는데, 이 파일은 그 반대편
 * 에서 같은 API 로 BAR 뒤의 메모리를 잡는다. 양쪽 모두 "상대가 DMA 로
 * 직접 건드릴 메모리" 라 캐시 일관성이 필요하다는 이유가 같다.
 *
 * 또 CMB(Controller Memory Buffer)와의 대응도 볼 만하다. NVMe 컨트롤러가
 * BAR 를 통해 자기 메모리를 호스트에 노출하는 것이 CMB 인데, 그것을
 * 소프트웨어로 구현한다면 정확히 이 파일의 alloc_space + set_bar 조합이
 * 된다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_epf_create()       : EPF 객체를 만들어 가상 버스에 등록한다.
 * pci_epf_destroy()      : 그 반대.
 * pci_epf_bind() / pci_epf_unbind() : EPF 드라이버의 콜백을 부르되,
 *                          vEPF 들을 함께 처리한다.
 * pci_epf_add_vepf() / pci_epf_remove_vepf() : 다중 함수 구성을 만든다.
 * pci_epf_alloc_space()  : BAR 뒤에 놓일 메모리를 dma_alloc_coherent 로
 *                          잡고, 하드웨어 제약에 맞게 크기를 보정한다.
 * pci_epf_free_space()   : 그 반대.
 * pci_epf_get_required_bar_size() : 요청 크기를 하드웨어가 받아들일 수
 *                          있는 크기로 올린다.
 * pci_epf_assign_bar_space() : 이미 있는 메모리를 BAR 에 연결한다.
 * pci_epf_align_inbound_addr() : 인바운드 주소를 하드웨어 정렬에 맞춘다.
 * __pci_epf_register_driver() / pci_epf_unregister_driver() : EPF 드라이버
 *                          등록. functions/ 의 드라이버들이 부른다.
 * pci_epf_device_match() / _probe() / _remove() : 가상 버스의 드라이버
 *                          모델 콜백. 이름으로 짝을 찾는다.
 * pci_epf_bus_type       : EPF 들이 올라가는 가상 버스.
 */

/* [한국어] struct device 와 드라이버 모델. EPF 를 가상 버스의 장치로
 * 등록하므로 필요하다. */
#include <linux/device.h>
/* [한국어] dma_alloc_coherent / dma_free_coherent. BAR 뒤의 메모리를
 * 잡는 데 쓰며, 호스트가 DMA 로 건드릴 영역이라 일관성 있는 매핑이
 * 필요하다. 이 파일이 이 헤더를 쓰는 유일한 이유다. */
#include <linux/dma-mapping.h>
/* [한국어] kzalloc / kfree — struct pci_epf 등의 할당. */
#include <linux/slab.h>
/* [한국어] EXPORT_SYMBOL_GPL, 모듈 매크로, THIS_MODULE. */
#include <linux/module.h>

/* [한국어] struct pci_epc 와 pci_epc_*() — EPF 가 붙을 상대편. */
#include <linux/pci-epc.h>
/* [한국어] struct pci_epf, struct pci_epf_driver, struct pci_epf_bar 정의와
 * 이 파일이 구현하는 함수들의 선언. */
#include <linux/pci-epf.h>
/* [한국어] configfs 인터페이스. EPF 를 만들 때 대응하는 configfs 그룹도
 * 함께 만든다. */
#include <linux/pci-ep-cfs.h>

/* [한국어] EPF 드라이버 목록과 EPF 장치 목록을 함께 보호하는 전역 뮤텍스.
 * EPC 쪽과 달리 인스턴스마다가 아니라 전역인 이유는, 보호 대상이
 * 시스템 전체에 하나뿐인 드라이버 목록이기 때문이다.
 * 드라이버 등록·해제와 EPF 생성·소멸이 서로 경쟁할 수 있어 필요하다. */
static DEFINE_MUTEX(pci_epf_mutex);

/* [한국어] EPF 들이 올라가는 가상 버스. 실제 하드웨어 버스가 아니라
 * 커널의 드라이버 매칭 기능을 빌려 쓰기 위한 것이다.
 * 정의는 이 파일 아래쪽에 있고 여기서는 전방 선언만 한다 —
 * 위쪽 함수들이 이 심볼을 참조하기 때문이다. */
static const struct bus_type pci_epf_bus_type;
static const struct device_type pci_epf_type;

/**
 * pci_epf_unbind() - Notify the function driver that the binding between the
 *		      EPF device and EPC device has been lost
 * @epf: the EPF device which has lost the binding with the EPC device
 *
 * Invoke to notify the function driver that the binding between the EPF device
 * and EPC device has been lost.
 */
/* [한국어]
 * pci_epf_unbind - EPF 와 EPC 의 연결이 끊겼음을 기능 드라이버에 알린다
 *
 * @epf: 연결을 잃은 엔드포인트 기능.
 * @return: 없음.
 *
 * **pci_epf_bind() 의 짝이며, 그 함수가 세운 것을 역순으로 허문다.**
 * 기능 드라이버의 unbind 콜백이 여기서 불리고, 그 안에서 BAR 를 거두고
 * 할당해 둔 메모리를 놓는다.
 *
 * **VF 를 먼저 풀고 PF 를 나중에 푸는 순서가 요점이다.** VF 는 PF 의 함수
 * 번호와 EPC 포인터를 물려받아 쓰므로, PF 를 먼저 풀면 VF 가 풀리는 도중에
 * 그 정보를 잃는다. bind 가 VF → PF 순으로 세운 것과 순서가 같은데, 그것은
 * bind 실패 시 이 함수가 그대로 정리 경로가 되기 때문이다.
 *
 * **is_bound 를 확인하는 이유가 거기 있다.** bind 가 도중에 실패하면 일부만
 * bind 된 상태로 이 함수가 불린다. 그때 bind 되지 않은 것의 unbind 를
 * 부르면 드라이버가 만들지도 않은 것을 없애려 든다.
 *
 * 마지막에 module_put 으로 bind 가 올린 모듈 참조를 내린다.
 * **드라이버가 없으면 dev_WARN 을 찍고 물러난다** -- 바인드된 적이 없는
 * EPF 에 대해 불린 것이므로 프로그래밍 오류다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡는다.
 *
 * 호출 체인:
 *   pci-ep-cfs.c 의 configfs 링크 해제 / pci_epf_bind() 의 실패 경로
 *     → [이 함수] → epf->driver->ops->unbind(), module_put()
 */
void pci_epf_unbind(struct pci_epf *epf)
{
	/* [한국어] vEPF 목록을 순회할 커서. */
	struct pci_epf *epf_vf;

	/* [한국어] 드라이버가 붙지 않은 EPF 를 unbind 하려는 것은 호출자의
	 * 논리 오류다. dev_WARN 으로 스택까지 남겨 드러나게 한다. */
	if (!epf->driver) {
		dev_WARN(&epf->dev, "epf device not bound to driver\n");
		return;
	}

	mutex_lock(&epf->lock);
	/* [한국어] 가상 함수부터 떼어 낸다. 순서가 bind 의 역순이라는 점이
	 * 중요한데, bind 가 vEPF 를 먼저 붙이고 마지막에 주 EPF 를 붙이므로
	 * 해제는 그 반대로 가야 의존 관계가 어긋나지 않는다. */
	list_for_each_entry(epf_vf, &epf->pci_vepf, list) {
		/* [한국어] is_bound 를 보는 이유는 bind 가 중간에 실패했을 수
		 * 있기 때문이다. 그때 이 함수가 정리를 맡는데, 붙지 않은 것까지
		 * unbind 하면 드라이버가 이중 해제를 하게 된다. */
		if (epf_vf->is_bound)
			epf_vf->driver->ops->unbind(epf_vf);
	}
	/* [한국어] 마지막으로 주 EPF. 역시 실제로 붙었을 때만. */
	if (epf->is_bound)
		epf->driver->ops->unbind(epf);
	mutex_unlock(&epf->lock);
	/* [한국어] bind 가 올린 모듈 참조를 내린다. 락 밖에서 하는 이유는
	 * 이 호출이 모듈 언로드를 촉발할 수 있고, 그 과정에서 다시 이 락을
	 * 잡으려 들면 교착이 되기 때문이다. */
	module_put(epf->driver->owner);
}
EXPORT_SYMBOL_GPL(pci_epf_unbind);

/**
 * pci_epf_bind() - Notify the function driver that the EPF device has been
 *		    bound to a EPC device
 * @epf: the EPF device which has been bound to the EPC device
 *
 * Invoke to notify the function driver that it has been bound to a EPC device
 */
/* [한국어]
 * pci_epf_bind - EPF 가 EPC 에 붙었음을 기능 드라이버에 알린다
 *
 * @epf: EPC 에 붙은 엔드포인트 기능.
 * @return: 성공 0, 드라이버가 없으면 -EINVAL, 모듈 참조를 못 얻으면 -EAGAIN.
 *
 * **EPF 드라이버가 실제로 하드웨어를 세우기 시작하는 지점이다.** 이 함수가
 * 부르는 bind 콜백 안에서 EPF 드라이버는 config 헤더를 쓰고, BAR 를 만들고,
 * MSI/MSI-X 개수를 광고한다. 곧 pci_epc_ 계열 함수가 실제로 불리는 곳이
 * 그 콜백이다.
 *
 * **먼저 모듈 참조를 올린다.** 기능 드라이버가 바인드된 채로 언로드되면
 * 콜백 포인터가 사라진 메모리를 가리키게 되므로, 참조를 올려 그것을 막는다.
 * 실패하면 -EAGAIN 인데, 그 모듈이 언로드 중이라 잠시 뒤 다시 시도해 볼
 * 여지가 있다는 뜻이다.
 *
 * **VF 를 먼저 처리하고 PF 를 나중에 한다.** VF 하나하나에 대해 다음을 한다.
 * 1. VF 번호가 1 이상인지 본다 -- 0 은 PF 자신을 뜻하는 자리라 VF 가
 *    가질 수 없다.
 * 2. **Primary 와 Secondary 양쪽 EPC 에 대해** 그 EPC 가 SR-IOV 를
 *    지원하는지(max_vfs 배열이 있는지), 그리고 이 VF 번호가 그 PF 의
 *    상한을 넘지 않는지 확인한다. 두 EPC 를 따로 보는 것은 NTB 처럼
 *    EPF 하나가 양쪽에 붙는 구성 때문이다.
 * 3. **PF 의 함수 번호와 EPC 포인터를 VF 에 그대로 복사한다** -- SR-IOV 에서
 *    VF 는 PF 와 같은 컨트롤러 위에 있으므로 그 정보를 물려받는다.
 * 4. VF 의 bind 를 부르고 is_bound 를 세운다.
 *
 * **실패하면 락을 놓고 pci_epf_unbind() 를 부른다.** 부분적으로 bind 된
 * 것들을 그 함수가 is_bound 를 보며 정리해 준다 -- 정리 코드를 두 벌 두지
 * 않으려는 설계이며, 락을 먼저 놓는 것은 unbind 가 같은 뮤텍스를 잡기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡는다.
 *
 * 호출 체인:
 *   pci-ep-cfs.c 의 configfs 링크 생성
 *     → [이 함수] → try_module_get(), epf->driver->ops->bind()
 */
int pci_epf_bind(struct pci_epf *epf)
{
	/* [한국어] 오류 메시지를 낼 device. EPF 쪽 이름이 찍히도록 epf->dev 를 쓴다. */
	struct device *dev = &epf->dev;
	/* [한국어] vEPF 목록 순회용 커서. */
	struct pci_epf *epf_vf;
	/* [한국어] 검증에 쓸 물리 함수 번호와 가상 함수 번호. 아래 루프에서 채운다. */
	u8 func_no, vfunc_no;
	struct pci_epc *epc;
	int ret;

	/* [한국어] unbind 와 마찬가지로 드라이버 없이 부르는 것은 오류다. */
	if (!epf->driver) {
		dev_WARN(dev, "epf device not bound to driver\n");
		return -EINVAL;
	}

	/* [한국어] 바인드가 유지되는 동안 드라이버 모듈이 언로드되지 않게
	 * 참조를 올린다. -EAGAIN 을 주는 것은 그 모듈이 지금 언로드 중이라는
	 * 뜻이고, 나중에 다시 시도하면 될 수도 있기 때문이다. */
	if (!try_module_get(epf->driver->owner))
		return -EAGAIN;

	mutex_lock(&epf->lock);
	/* [한국어] 가상 함수들을 먼저 붙인다. 주 EPF 보다 먼저 하는 이유는
	 * 주 EPF 의 bind 가 전체 구성이 갖춰진 것을 전제로 config 헤더를
	 * 쓰거나 링크를 준비할 수 있어서다. */
	list_for_each_entry(epf_vf, &epf->pci_vepf, list) {
		vfunc_no = epf_vf->vfunc_no;

		/* [한국어] vfunc_no 0 은 물리 함수 자신을 뜻하는 약속이라
		 * 가상 함수가 가질 수 없는 번호다. 1 부터 시작해야 한다. */
		if (vfunc_no < 1) {
			dev_err(dev, "Invalid virtual function number\n");
			ret = -EINVAL;
			goto ret;
		}

		/* [한국어] primary 인터페이스 쪽 검증. 이 EPF 가 붙어 있는
		 * EPC 가 이 가상 함수 번호를 감당할 수 있는지 본다. */
		epc = epf->epc;
		func_no = epf->func_no;
		/* [한국어] EPC 에 아직 붙지 않았을 수도 있어(그때는 NULL)
		 * 붙어 있을 때만 검사한다. */
		if (!IS_ERR_OR_NULL(epc)) {
			/* [한국어] max_vfs 배열이 없으면 이 컨트롤러는 SR-IOV
			 * 자체를 지원하지 않는다. */
			if (!epc->max_vfs) {
				dev_err(dev, "No support for virt function\n");
				ret = -EINVAL;
				goto ret;
			}

			/* [한국어] 그 물리 함수의 VF 상한을 넘는지. PF 마다
			 * 다를 수 있어 배열로 되어 있다. */
			if (vfunc_no > epc->max_vfs[func_no]) {
				dev_err(dev, "PF%d: Exceeds max vfunc number\n",
					func_no);
				ret = -EINVAL;
				goto ret;
			}
		}

		/* [한국어] secondary 인터페이스에도 같은 검증을 한다.
		 * 코드가 그대로 반복되는 이유는 EPF 하나가 EPC 둘에 붙을 수
		 * 있고 각각의 제약이 다를 수 있기 때문이다(NTB 구성). */
		epc = epf->sec_epc;
		func_no = epf->sec_epc_func_no;
		if (!IS_ERR_OR_NULL(epc)) {
			/* [한국어] 이 컨트롤러가 SR-IOV 자체를 지원하지 않는 경우. */
			if (!epc->max_vfs) {
				/* [한국어] 설정 문제이므로 메시지를 남긴다. */
				dev_err(dev, "No support for virt function\n");
				ret = -EINVAL;
				goto ret;
			}

			/* [한국어] 그 물리 함수의 VF 상한을 넘는 경우. */
			if (vfunc_no > epc->max_vfs[func_no]) {
				/* [한국어] 어느 PF 에서 넘쳤는지 번호와 함께 알린다. */
				dev_err(dev, "PF%d: Exceeds max vfunc number\n",
					func_no);
				ret = -EINVAL;
				goto ret;
			}
		}

		/* [한국어] 검증을 통과했으니 가상 함수에게 주 EPF 의 연결
		 * 정보를 물려준다. VF 는 PF 와 같은 EPC 에 속하고 같은 물리
		 * 함수 번호를 쓰며, 자기만의 것은 vfunc_no 하나뿐이다. */
		epf_vf->func_no = epf->func_no;
		epf_vf->sec_epc_func_no = epf->sec_epc_func_no;
		epf_vf->epc = epf->epc;
		epf_vf->sec_epc = epf->sec_epc;
		/* [한국어] 이제 이 가상 함수의 드라이버에게 bind 를 알린다.
		 * 드라이버는 여기서 BAR 메모리를 잡고 config 헤더를 쓴다. */
		ret = epf_vf->driver->ops->bind(epf_vf);
		if (ret)
			goto ret;
		/* [한국어] 성공 표시. 위 unbind 가 이 값을 보고 정리 여부를
		 * 정하므로, bind 직후에 세워야 한다. */
		epf_vf->is_bound = true;
	}

	/* [한국어] 가상 함수를 다 붙인 뒤 마지막으로 주 EPF 를 붙인다. */
	ret = epf->driver->ops->bind(epf);
	if (ret)
		goto ret;
	/* [한국어] 주 EPF 까지 붙었다. 이제 이 EPF 는 완전히 동작 가능한 상태다. */
	epf->is_bound = true;

	mutex_unlock(&epf->lock);
	return 0;

ret:
	/* [한국어] 어느 단계에서 실패했든 여기로 모인다. 락을 먼저 풀고
	 * unbind 를 부르는데, unbind 가 같은 락을 잡기 때문이다 —
	 * 이 뮤텍스는 재진입할 수 없다.
	 *
	 * unbind 가 is_bound 플래그를 보고 실제로 붙은 것만 정리하므로,
	 * 몇 번째에서 실패했는지 여기서 따로 세지 않아도 된다. 그것이
	 * is_bound 를 두는 이유다. */
	mutex_unlock(&epf->lock);
	pci_epf_unbind(epf);

	return ret;
}
EXPORT_SYMBOL_GPL(pci_epf_bind);

/**
 * pci_epf_add_vepf() - associate virtual EP function to physical EP function
 * @epf_pf: the physical EP function to which the virtual EP function should be
 *   associated
 * @epf_vf: the virtual EP function to be added
 *
 * A physical endpoint function can be associated with multiple virtual
 * endpoint functions. Invoke pci_epf_add_vepf() to add a virtual PCI endpoint
 * function to a physical PCI endpoint function.
 */
/* [한국어]
 * pci_epf_add_vepf - 가상 기능(VF)을 물리 기능(PF)에 딸린 것으로 등록한다
 *
 * @epf_pf: 물리 엔드포인트 기능.
 * @epf_vf: 거기에 딸릴 가상 엔드포인트 기능.
 * @return: 성공 0, 인자가 잘못이면 -EINVAL, 이미 쓰이고 있으면 -EBUSY.
 *
 * **SR-IOV 를 엔드포인트 쪽에서 구성하는 함수다.** PF 하나에 VF 여럿을
 * 매달아 두면, PF 가 EPC 에 붙을 때(pci_epf_bind) VF 들도 함께 세워진다.
 *
 * **아직 EPC 에 붙기 전이어야 한다.** epf_pf->epc 나 epf_vf->epc 가 이미
 * 차 있으면 -EBUSY 로 거절하는데, 붙은 뒤에 VF 구성을 바꾸면 이미 배정된
 * 함수 번호와 어긋나기 때문이다. Secondary 쪽도 같은 이유로 함께 본다.
 *
 * **VF 번호는 PF 의 비트맵에서 배정한다.** epf_pf->vfunction_num_map 에서
 * 처음 0 인 비트를 찾아 그 번호를 준다. EPC 가 함수 번호를 배정하는 방식
 * (pci_epc_add_epf)과 같은 관용이며, 다만 여기서는 PF 마다 따로 센다.
 *
 * **epf_vf->is_vf 를 true 로 세우는 것이 중요하다.** 그 표시가 있어야
 * pci_epc_add_epf() 가 이 EPF 를 직접 받지 않고 거절한다 -- VF 는 EPC 에
 * 직접 붙는 것이 아니라 PF 를 통해서만 세워져야 하기 때문이다.
 *
 * **배정한 번호의 상한 검사는 여기서 하지 않는다.** EPC 가 지원하는 VF 수와
 * 견주는 일은 pci_epf_bind() 가 맡는데, 이 시점에는 아직 어느 EPC 에 붙을지
 * 모르기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡는다.
 *
 * 호출 체인:
 *   pci-ep-cfs.c 의 configfs 조작
 *     → [이 함수] → find_first_zero_bit(), set_bit(), list_add_tail()
 */
int pci_epf_add_vepf(struct pci_epf *epf_pf, struct pci_epf *epf_vf)
{
	/* [한국어] 배정받을 가상 함수 번호. */
	u32 vfunc_no;

	if (IS_ERR_OR_NULL(epf_pf) || IS_ERR_OR_NULL(epf_vf))
		return -EINVAL;

	/* [한국어] 둘 다 아직 EPC 에 붙지 않았어야 한다. 이미 붙은 뒤에
	 * 구성을 바꾸면 호스트가 이미 열거한 것과 어긋나기 때문이다.
	 * epf_vf->epf_pf 도 확인해 이 가상 함수가 이미 다른 물리 함수에
	 * 매여 있는지 본다. */
	if (epf_pf->epc || epf_vf->epc || epf_vf->epf_pf)
		return -EBUSY;

	/* [한국어] secondary 쪽도 같은 이유로 확인한다. */
	if (epf_pf->sec_epc || epf_vf->sec_epc)
		return -EBUSY;

	mutex_lock(&epf_pf->lock);
	/* [한국어] 비어 있는 가상 함수 번호를 찾는다. pci_epc_add_epf() 의
	 * 함수 번호 배정과 같은 비트맵 방식이다. */
	vfunc_no = find_first_zero_bit(&epf_pf->vfunction_num_map,
				       BITS_PER_LONG);
	if (vfunc_no >= BITS_PER_LONG) {
		mutex_unlock(&epf_pf->lock);
		return -EINVAL;
	}

	/* [한국어] 그 번호를 쓴 것으로 표시하고 가상 함수에 기록한다. */
	set_bit(vfunc_no, &epf_pf->vfunction_num_map);
	epf_vf->vfunc_no = vfunc_no;

	/* [한국어] 부모를 가리키게 하고 "나는 가상 함수" 라고 표시한다.
	 * is_vf 는 pci_epc_add_epf() 가 확인해 vEPF 를 EPC 에 직접
	 * 붙이지 못하게 막는 데 쓰인다 — vEPF 는 부모를 통해서만 붙는다. */
	epf_vf->epf_pf = epf_pf;
	epf_vf->is_vf = true;

	/* [한국어] 부모의 vEPF 목록에 매단다. bind/unbind 가 이 목록을
	 * 순회한다. */
	list_add_tail(&epf_vf->list, &epf_pf->pci_vepf);
	mutex_unlock(&epf_pf->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(pci_epf_add_vepf);

/**
 * pci_epf_remove_vepf() - remove virtual EP function from physical EP function
 * @epf_pf: the physical EP function from which the virtual EP function should
 *   be removed
 * @epf_vf: the virtual EP function to be removed
 *
 * Invoke to remove a virtual endpoint function from the physical endpoint
 * function.
 */
/* [한국어]
 * pci_epf_remove_vepf - VF 를 PF 에서 떼어 낸다
 *
 * @epf_pf: 물리 엔드포인트 기능.
 * @epf_vf: 떼어 낼 가상 엔드포인트 기능.
 * @return: 없음.
 *
 * **pci_epf_add_vepf() 의 짝이다.** VF 번호 비트를 지우고, PF 를 가리키던
 * 포인터를 끊고, 목록에서 뺀다.
 *
 * **is_vf 표시는 되돌리지 않는다.** add 에서 true 로 세운 것이 여기서는
 * 그대로 남으므로, 한 번 VF 로 만든 EPF 는 떼어 낸 뒤에도 VF 로 남는다.
 * 그 EPF 를 EPC 에 직접 붙이려 하면 pci_epc_add_epf() 가 여전히 거절한다.
 * 코드는 손대지 않고 사실만 적는다.
 *
 * **PF 가 EPC 에 붙어 있는 중인지 확인하지 않는다** -- 붙어 있는 동안 VF 를
 * 떼어 내면 이미 세워진 VF 의 상태와 어긋나게 되나, 이 계층은 상위
 * (configfs)가 순서를 지킨다고 전제한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡는다.
 *
 * 호출 체인:
 *   pci-ep-cfs.c 의 configfs 조작
 *     → [이 함수] → clear_bit(), list_del()
 */
void pci_epf_remove_vepf(struct pci_epf *epf_pf, struct pci_epf *epf_vf)
{
	if (IS_ERR_OR_NULL(epf_pf) || IS_ERR_OR_NULL(epf_vf))
		return;

	/* [한국어] add 의 역순이다. */
	mutex_lock(&epf_pf->lock);
	/* [한국어] 번호를 반납한다. */
	clear_bit(epf_vf->vfunc_no, &epf_pf->vfunction_num_map);
	/* [한국어] 부모 연결을 끊는다. add 의 -EBUSY 검사가 이 값을 보므로
	 * NULL 로 되돌려야 나중에 다시 붙일 수 있다. */
	epf_vf->epf_pf = NULL;
	/* [한국어] 목록에서 뺀다.
	 * is_vf 는 되돌리지 않는데, 한 번 가상 함수로 만들어진 EPF 는
	 * 계속 그런 성질을 갖는다고 보는 것으로 읽힌다. 이 트리 안에서
	 * is_vf 를 false 로 되돌리는 코드는 확인되지 않는다. */
	list_del(&epf_vf->list);
	mutex_unlock(&epf_pf->lock);
}
EXPORT_SYMBOL_GPL(pci_epf_remove_vepf);

/* [한국어]
 * pci_epf_get_required_bar_size - 요청 크기를 하드웨어가 받아들일 크기로 보정
 *
 * @epf: 대상 EPF. 오류 메시지에만 쓴다.
 * @bar_size: 입출력. 들어올 때는 원하는 크기, 나갈 때는 보정된 BAR 크기.
 * @aligned_mem_size: 출력. 실제로 할당해야 할 메모리 크기.
 * @bar: 몇 번 BAR 인지. 능력 표에서 그 BAR 의 제약을 본다.
 * @epc_features: 이 EPC 의 능력.
 * @type: primary / secondary 구분. 현재 이 함수는 쓰지 않지만 인자로 받는다.
 * @return: 0 이면 성공, -ENOMEM 이면 고정 크기 BAR 에 그보다 큰 것을 요구.
 *
 * BAR 크기는 소프트웨어가 마음대로 정할 수 없다. 제약이 넷이다.
 *   최소 128바이트 — 그보다 작은 BAR 는 실용적 의미가 없다.
 *   Resizable BAR 는 최소 1MB — 규격이 정한 값(상류 주석 참고).
 *   고정 크기 BAR 는 그 값 그대로 — 하드웨어가 배선되어 바꿀 수 없다.
 *   나머지는 2의 거듭제곱 — BAR 레지스터의 구조상 다른 값을 표현할 수 없다.
 *
 * 출력이 둘인 이유가 이 함수에서 가장 덜 자명한 부분이다. 상류 주석이
 * 설명하듯 BAR 크기와 메모리 정렬이 다를 수 있다. 예를 들어 고정 크기가
 * 128바이트인데 하드웨어가 BAR 시작 주소를 4KB 경계에 맞추라고 요구하면,
 * BAR 로는 128바이트만 노출하되 메모리는 4KB 단위로 잡아야 한다.
 * 그래서 bar_size(노출할 크기)와 aligned_mem_size(잡을 크기)를 따로 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 계산.
 *
 * 호출 체인:
 *   pci_epf_alloc_space() / pci_epf_assign_bar_space() → [이 함수]
 */
static int pci_epf_get_required_bar_size(struct pci_epf *epf, size_t *bar_size,
				size_t *aligned_mem_size,
				enum pci_barno bar,
				const struct pci_epc_features *epc_features,
				enum pci_epc_interface_type type)
{
	/* [한국어] 이 BAR 가 고정 크기라면 그 값. 아니면 0. */
	u64 bar_fixed_size = epc_features->bar[bar].fixed_size;
	/* [한국어] 이 EPC 가 요구하는 BAR 시작 주소 정렬. 0 이면 제약 없음. */
	size_t align = epc_features->align;
	/* [한국어] 입출력 인자를 지역 변수로 받아 가공한다. */
	size_t size = *bar_size;

	/* [한국어] 하한 128바이트. 그보다 작게 요청해도 올려 잡는다. */
	if (size < 128)
		size = 128;

	/* According to PCIe base spec, min size for a resizable BAR is 1 MB. */
	/* [한국어] 상류 주석대로 Resizable BAR 는 최소 1MB 다.
	 * pci_epc_set_bar() 의 검증과 같은 값이라, 여기서 미리 올려 두면
	 * 그쪽에서 -EINVAL 로 걸리지 않는다. */
	if (epc_features->bar[bar].type == BAR_RESIZABLE && size < SZ_1M)
		size = SZ_1M;

	if (epc_features->bar[bar].type == BAR_FIXED && bar_fixed_size) {
		/* [한국어] 고정 크기 BAR 에 그보다 큰 것을 요구하면 방법이 없다.
		 * 하드웨어가 그 크기로 배선되어 있어 늘릴 수 없기 때문이다. */
		if (size > bar_fixed_size) {
			dev_err(&epf->dev,
				"requested BAR size is larger than fixed size\n");
			return -ENOMEM;
		}
		/* [한국어] 작게 요청했더라도 고정 크기로 맞춘다. 그 크기가
		 * 아니면 하드웨어가 받아들이지 않는다. */
		size = bar_fixed_size;
	} else {
		/* BAR size must be power of two */
		/* [한국어] 상류 주석대로 2의 거듭제곱으로 올린다.
		 * BAR 레지스터가 크기를 하위 비트의 개수로 표현하는 구조라
		 * 다른 값은 표현 자체가 불가능하다. */
		size = roundup_pow_of_two(size);
	}

	/* [한국어] 보정된 BAR 크기를 돌려준다. */
	*bar_size = size;

	/*
	 * The EPC's BAR start address must meet alignment requirements. In most
	 * cases, the alignment will match the BAR size. However, differences
	 * can occur—for example, when the fixed BAR size (e.g., 128 bytes) is
	 * smaller than the required alignment (e.g., 4 KB).
	 */
	/* [한국어] 상류 주석이 예까지 들어 설명하고 있다. 정렬 요구가 있으면
	 * 그 배수로 올리고, 없으면 BAR 크기 그대로다.
	 * 이 값이 dma_alloc_coherent 에 넘어갈 크기이며, 나중에 해제할 때도
	 * 같은 값을 써야 하므로 epf_bar[].mem_size 에 보관된다. */
	*aligned_mem_size = align ? ALIGN(size, align) : size;

	return 0;
}

/**
 * pci_epf_free_space() - free the allocated PCI EPF register space
 * @epf: the EPF device from whom to free the memory
 * @addr: the virtual address of the PCI EPF register space
 * @bar: the BAR number corresponding to the register space
 * @type: Identifies if the allocated space is for primary EPC or secondary EPC
 *
 * Invoke to free the allocated PCI EPF register space.
 */
/* [한국어]
 * pci_epf_free_space - BAR 뒤에 붙여 두었던 메모리를 놓는다
 *
 * @epf: 메모리를 놓을 엔드포인트 기능.
 * @addr: pci_epf_alloc_space() 가 돌려주었던 가상 주소.
 * @bar: 그 메모리가 붙어 있던 BAR 번호.
 * @type: Primary EPC 쪽인지 Secondary EPC 쪽인지.
 * @return: 없음.
 *
 * **pci_epf_alloc_space() 의 짝이다.** 호스트가 그 BAR 를 통해 읽고 쓰던
 * 실제 메모리를 dma_free_coherent 로 놓고, epf_bar 항목을 통째로 비운다.
 *
 * **type 으로 어느 배열을 볼지 가른다** -- Primary 면 epf->bar,
 * Secondary 면 epf->sec_epc_bar 다. EPF 하나가 두 EPC 에 붙을 수 있어
 * BAR 정보도 두 벌이다.
 *
 * **dma_free_coherent 에 넘기는 device 가 EPF 자신이 아니라
 * epc->dev.parent 인 것이 요점이다.** DMA 매핑은 실제로 버스에 접근하는
 * 하드웨어를 기준으로 해야 하고, 그것은 EPC 컨트롤러의 부모 device 다.
 * 할당할 때와 같은 device 를 넘겨야 매핑이 올바로 풀린다.
 *
 * **size 와 mem_size 를 나눠 들고 있는 이유**: BAR 로 광고한 크기와 실제로
 * 할당한 크기가 다를 수 있다(정렬 때문에 더 크게 잡는다). 해제에는 실제
 * 할당 크기인 mem_size 를 써야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. dma_free_coherent 는 잠들 수 있다.
 *
 * 호출 체인:
 *   EPF 드라이버의 unbind
 *     → [이 함수] → dma_free_coherent()
 */
void pci_epf_free_space(struct pci_epf *epf, void *addr, enum pci_barno bar,
			enum pci_epc_interface_type type)
{
	/* [한국어] DMA 할당 주체가 될 device. 아래에서 EPC 하드웨어의 것으로 정한다. */
	struct device *dev;
	struct pci_epf_bar *epf_bar;
	struct pci_epc *epc;

	/* [한국어] 할당하지 않은 BAR 에 대해 불려도 안전하게 한다.
	 * 드라이버의 정리 경로가 모든 BAR 에 대해 일괄로 부르는 일이 많다. */
	if (!addr)
		return;

	/* [한국어] EPF 하나가 EPC 둘에 붙을 수 있어 BAR 정보 배열도 두 벌이다.
	 * 어느 쪽인지에 따라 다른 배열을 본다. */
	if (type == PRIMARY_INTERFACE) {
		epc = epf->epc;
		epf_bar = epf->bar;
	} else {
		/* [한국어] secondary 인터페이스 쪽 EPC. */
		epc = epf->sec_epc;
		epf_bar = epf->sec_epc_bar;
	}

	/* [한국어] DMA 할당의 주체는 EPC 하드웨어의 device 다. EPF 의 것이
	 * 아닌 이유는, 이 메모리에 실제로 DMA 를 하는 것이 그 컨트롤러이고
	 * IOMMU 매핑이나 주소 제약도 그쪽에 걸리기 때문이다. */
	dev = epc->dev.parent;
	/* [한국어] alloc 때와 같은 크기(mem_size)로 해제해야 한다. BAR 크기
	 * (size)가 아니라는 점이 중요하다 — 정렬 때문에 둘이 다를 수 있고,
	 * dma_free_coherent 는 할당한 크기를 그대로 요구한다. */
	dma_free_coherent(dev, epf_bar[bar].mem_size, addr,
			  epf_bar[bar].phys_addr);

	/* [한국어] BAR 정보를 전부 지운다. 특히 addr 을 NULL 로 되돌리는
	 * 것이 중요한데, 이 함수의 맨 앞 검사가 그것을 보므로 두 번 해제하는
	 * 사고를 막아 준다. flags 까지 지우는 것은 다음에 이 BAR 를 다시
	 * 쓸 때 옛 타입 비트가 남아 섞이지 않게 하려는 것이다. */
	epf_bar[bar].phys_addr = 0;
	epf_bar[bar].addr = NULL;
	epf_bar[bar].size = 0;
	/* [한국어] 정렬 크기도 0 으로. 다음에 이 BAR 를 쓸 때 옛 값이 남으면 안 된다. */
	epf_bar[bar].mem_size = 0;
	/* [한국어] BAR 번호도 초기화한다. */
	epf_bar[bar].barno = 0;
	/* [한국어] 타입 비트까지 지운다. 남겨 두면 다음 설정에서 32/64비트가 섞인다. */
	epf_bar[bar].flags = 0;
}
EXPORT_SYMBOL_GPL(pci_epf_free_space);

/**
 * pci_epf_alloc_space() - allocate memory for the PCI EPF register space
 * @epf: the EPF device to whom allocate the memory
 * @size: the size of the memory that has to be allocated
 * @bar: the BAR number corresponding to the allocated register space
 * @epc_features: the features provided by the EPC specific to this EPF
 * @type: Identifies if the allocation is for primary EPC or secondary EPC
 *
 * Invoke to allocate memory for the PCI EPF register space.
 * Flag PCI_BASE_ADDRESS_MEM_TYPE_64 will automatically get set if the BAR
 * can only be a 64-bit BAR, or if the requested size is larger than 2 GB.
 */
/* [한국어]
 * pci_epf_alloc_space - BAR 뒤에 놓을 메모리를 할당하고 BAR 정보를 채운다
 *
 * @epf: 메모리를 붙일 엔드포인트 기능.
 * @size: 필요한 크기.
 * @bar: 이 메모리를 붙일 BAR 번호.
 * @epc_features: EPC 가 보고한 능력. BAR 의 제약을 여기서 본다.
 * @type: Primary EPC 쪽인지 Secondary EPC 쪽인지.
 * @return: 할당된 가상 주소, 실패면 NULL.
 *
 * **호스트가 이 장치의 BAR 를 읽고 쓸 때 실제로 닿는 메모리를 만든다.**
 * EPF 드라이버는 여기서 받은 가상 주소로 그 영역을 채우고, 호스트는 같은
 * 메모리를 BAR 를 통해 본다.
 *
 * **dma_alloc_coherent 를 쓰는 이유가 핵심이다.** 이 메모리는 CPU 와
 * PCIe 컨트롤러가 함께 보는 곳이라, 캐시 일관성이 보장되어야 한다.
 * 일반 kmalloc 으로 잡으면 CPU 가 쓴 값이 캐시에 남아 호스트가 옛 값을
 * 읽을 수 있다. 또한 dma_alloc_coherent 는 물리 주소를 함께 돌려주는데,
 * 그 주소가 곧 BAR 가 가리켜야 할 곳이다.
 *
 * **필요한 크기를 먼저 다시 계산한다.** pci_epf_get_required_bar_size() 가
 * BAR 의 고정 크기 제약과 정렬 요구를 반영해 size 와 mem_size 를 채워 준다.
 * 둘이 갈리는 것은 BAR 로 광고할 크기(size)와 실제로 할당해야 하는 크기
 * (mem_size)가 다를 수 있기 때문이다.
 *
 * **64비트 플래그를 자동으로 정한다.** 요청 크기가 32비트를 넘거나 그 BAR 가
 * 64비트 전용이면 PCI_BASE_ADDRESS_MEM_TYPE_64 를 세운다. 상류 주석이
 * 그 두 조건을 그대로 밝힌다.
 *
 * **NVMe 관점**: 호스트 nvme 드라이버가 BAR0 을 ioremap 해 doorbell 을 쓰는
 * 그 메모리를, 엔드포인트에서는 이 함수가 만든다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. dma_alloc_coherent 가 잠들 수 있다.
 *
 * 호출 체인:
 *   EPF 드라이버의 bind / epc_init 콜백
 *     → [이 함수] → pci_epf_get_required_bar_size(), dma_alloc_coherent()
 */
void *pci_epf_alloc_space(struct pci_epf *epf, size_t size, enum pci_barno bar,
			  const struct pci_epc_features *epc_features,
			  enum pci_epc_interface_type type)
{
	/* [한국어] 결과를 기록할 BAR 정보 배열. 아래에서 정한다. */
	struct pci_epf_bar *epf_bar;
	/* [한국어] 할당받은 메모리의 물리(버스) 주소. 이 값이 나중에
	 * BAR 레지스터에 실려 호스트가 접근할 곳이 된다. */
	dma_addr_t phys_addr;
	struct pci_epc *epc;
	struct device *dev;
	/* [한국어] 정렬까지 반영한 실제 할당 크기. BAR 로 노출할 size 와
	 * 다를 수 있다. */
	size_t mem_size;
	void *space;

	/* [한국어] 먼저 크기를 하드웨어가 받아들일 수 있게 보정한다.
	 * size 는 입출력이라 이 호출 뒤에는 보정된 값으로 바뀌어 있다. */
	if (pci_epf_get_required_bar_size(epf, &size, &mem_size, bar,
					  epc_features, type))
		return NULL;

	/* [한국어] 어느 인터페이스의 BAR 인지에 따라 다른 배열을 쓴다. */
	if (type == PRIMARY_INTERFACE) {
		epc = epf->epc;
		epf_bar = epf->bar;
	} else {
		/* [한국어] secondary 쪽 EPC 와 */
		epc = epf->sec_epc;
		/* [한국어] 그쪽 BAR 정보 배열을 쓴다. */
		epf_bar = epf->sec_epc_bar;
	}

	dev = epc->dev.parent;
	/* [한국어] 이 파일에서 가장 중요한 한 줄이다.
	 * kmalloc 이 아니라 dma_alloc_coherent 를 쓰는 이유가 분명하다 —
	 * 이 메모리는 호스트가 PCIe 를 통해 직접 읽고 쓸 영역이라, CPU 캐시에
	 * 남은 값과 실제 메모리 내용이 어긋나면 안 된다. coherent 매핑은
	 * 그 일관성을 보장한다.
	 *
	 * 호스트 쪽 NVMe 드라이버가 큐를 만들 때 같은 API 를 쓰는 것과
	 * 정확히 대칭이다. 양쪽 다 "상대가 DMA 로 건드릴 메모리" 이기 때문이다. */
	space = dma_alloc_coherent(dev, mem_size, &phys_addr, GFP_KERNEL);
	if (!space) {
		dev_err(dev, "failed to allocate mem space\n");
		return NULL;
	}

	/* [한국어] BAR 정보를 채운다. 이 구조체가 그대로 pci_epc_set_bar() 에
	 * 넘어가 하드웨어에 반영된다. */
	epf_bar[bar].phys_addr = phys_addr;
	epf_bar[bar].addr = space;
	/* [한국어] size 는 BAR 로 노출할 크기, mem_size 는 실제 할당한 크기.
	 * 둘을 모두 기록해야 해제할 때 올바른 크기를 쓸 수 있다. */
	epf_bar[bar].size = size;
	epf_bar[bar].mem_size = mem_size;
	epf_bar[bar].barno = bar;
	/* [한국어] 상류 kernel-doc 이 밝힌 대로 64비트 플래그를 자동으로 정한다.
	 * 두 경우에 64비트가 필요하다.
	 *   upper_32_bits(size) 가 0 이 아님 — 크기가 4GB 를 넘어 32비트
	 *     BAR 로는 표현할 수 없다(kernel-doc 의 "2 GB" 는 부호 문제를
	 *     고려한 보수적 표현이고, 코드는 상위 32비트 존재로 판단한다).
	 *   only_64bit — 이 BAR 자리가 하드웨어상 64비트 전용이다.
	 * 64비트 BAR 는 다음 BAR 자리를 함께 잡아먹는다는 점을 기억해야 한다. */
	if (upper_32_bits(size) || epc_features->bar[bar].only_64bit)
		epf_bar[bar].flags |= PCI_BASE_ADDRESS_MEM_TYPE_64;
	else
		epf_bar[bar].flags |= PCI_BASE_ADDRESS_MEM_TYPE_32;

	/* [한국어] 커널 가상 주소를 돌려준다. EPF 드라이버는 이 주소로
	 * 그 메모리를 읽고 쓰며, 같은 메모리를 호스트는 BAR 를 통해 본다. */
	return space;
}
EXPORT_SYMBOL_GPL(pci_epf_alloc_space);

/**
 * pci_epf_assign_bar_space() - Assign PCI EPF BAR space
 * @epf: EPF device to assign the BAR memory
 * @size: Size of the memory that has to be assigned
 * @bar: BAR number for which the memory is assigned
 * @epc_features: Features provided by the EPC specific to this EPF
 * @type: Identifies if the assignment is for primary EPC or secondary EPC
 * @bar_addr: Address to be assigned for the @bar
 *
 * Invoke to assign memory for the PCI EPF BAR.
 * Flag PCI_BASE_ADDRESS_MEM_TYPE_64 will automatically get set if the BAR
 * can only be a 64-bit BAR, or if the requested size is larger than 2 GB.
 */
/* [한국어]
 * pci_epf_assign_bar_space - 이미 있는 메모리 영역을 BAR 로 덮도록 크기를 정한다
 *
 * @epf: 대상 엔드포인트 기능.
 * @size: 덮어야 할 영역의 크기.
 * @bar: 쓸 BAR 번호.
 * @epc_features: EPC 가 보고한 능력.
 * @type: Primary EPC 쪽인지 Secondary EPC 쪽인지.
 * @bar_addr: 덮어야 할 영역의 시작 주소.
 * @return: 성공 0, 덮을 수 없으면 -EINVAL 또는 -ENOMEM.
 *
 * **pci_epf_alloc_space() 와 목적이 반대다.** 그쪽은 메모리를 새로 잡아
 * BAR 에 붙이지만, 이쪽은 **이미 특정 주소에 있는 것(주변장치 레지스터,
 * 플랫폼 MSI 컨트롤러의 메시지 주소 등)을 BAR 로 노출** 하려는 것이다.
 * 주소를 고를 수 없으므로 대신 BAR 의 크기를 키워 그 주소를 품게 만든다.
 *
 * **크기를 정하는 방식이 이 함수의 요점이다.** BAR 는 자기 크기 경계에
 * 정렬되어야 하므로, [bar_addr, limit] 구간을 통째로 담으려면 그 구간의
 * 주소 비트가 어디까지 같은지를 봐야 한다. 상류 주석의 비트 그림이 그것을
 * 설명한다 -- 시작과 끝을 XOR 하면 달라지는 최상위 비트가 나오고,
 * 그 자리 + 1 이 필요한 BAR 크기의 지수가 된다.
 *
 * **pos 가 최상위 비트면 거절한다.** 그 경우 필요한 크기가 dma_addr_t 로
 * 표현할 수 있는 범위를 넘기 때문이다.
 *
 * 정렬 요구를 반영해 시작 주소를 내림하고(ALIGN_DOWN), 그러고도 구간의
 * 끝을 덮지 못하면 -ENOMEM 으로 물러난다.
 *
 * **addr 을 NULL 로 두는 것이 alloc 과 다른 점이다** -- 이 메모리는 EPF 가
 * 가상 주소로 접근할 대상이 아니라 호스트에게만 보여 줄 것이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   EPF 드라이버(주변장치 레지스터를 노출하려는 경우)
 *     → [이 함수] → pci_epf_get_required_bar_size()
 */
int pci_epf_assign_bar_space(struct pci_epf *epf, size_t size,
			     enum pci_barno bar,
			     const struct pci_epc_features *epc_features,
			     enum pci_epc_interface_type type,
			     dma_addr_t bar_addr)
{
	/* [한국어] 계산해 낼 BAR 크기와 실제 필요한 정렬 크기. */
	size_t bar_size, aligned_mem_size;
	struct pci_epf_bar *epf_bar;
	/* [한국어] 덮어야 할 범위의 마지막 주소(포함). */
	dma_addr_t limit;
	/* [한국어] 아래 비트 탐색의 결과. 범위를 덮는 데 필요한 비트 수를 정한다. */
	int pos;

	/* [한국어] 크기 0 은 덮을 범위가 없다는 뜻이라 BAR 로 만들 수 없다. */
	if (!size)
		return -EINVAL;

	/* [한국어] 마지막 주소. -1 을 하는 것은 반열린 구간이 아니라
	 * 포함 구간으로 다루기 위해서다. 아래 XOR 계산이 그 전제로 성립한다. */
	limit = bar_addr + size - 1;

	/*
	 *  Bits:		15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0
	 *  bar_addr:		U  U  U  U  U  U  0 X X X X X X X X X
	 *  limit:		U  U  U  U  U  U  1 X X X X X X X X X
	 *
	 *  bar_addr^limit	0  0  0  0  0  0  1 X X X X X X X X X
	 *
	 *  U: unchanged address bits in range [bar_addr, limit]
	 *  X: bit 0 or 1
	 *
	 *  (bar_addr^limit) & BIT_ULL(pos) will find the first set bit from MSB
	 *  (pos). And value of (2 ^ pos) should be able to cover the BAR range.
	 */
	/* [한국어] 상류 주석의 그림이 설명하는 계산이다. 요지는 이렇다.
	 *
	 * 시작과 끝을 XOR 하면, 두 주소가 같은 상위 비트들은 0 이 되고
	 * 처음으로 달라지는 비트부터 아래로 1 이 섞인다. 그 "처음 달라지는
	 * 비트" 의 위치가 곧 이 범위를 덮으려면 몇 비트가 필요한지를 말해 준다.
	 *
	 * 위에서부터 내려오며 처음 1 을 만나면 멈춘다. 그 위치가 pos 다. */
	for (pos = 8 * sizeof(dma_addr_t) - 1; pos > 0; pos--)
		if ((limit ^ bar_addr) & BIT_ULL(pos))
			break;

	/* [한국어] 최상위 비트에서 이미 갈렸다는 것은 범위가 주소 공간의
	 * 절반을 넘는다는 뜻이라 BAR 로 표현할 수 없다. */
	if (pos == 8 * sizeof(dma_addr_t) - 1)
		return -EINVAL;

	/* [한국어] pos 비트까지 달라진다면 2^(pos+1) 크기면 반드시 덮인다.
	 * 요청 크기보다 클 수 있는데, BAR 는 2의 거듭제곱이어야 하고
	 * 시작 주소도 그 크기에 정렬되어야 해서 어쩔 수 없다. */
	bar_size = BIT_ULL(pos + 1);
	/* [한국어] 그 크기를 다시 하드웨어 제약에 맞게 보정한다. */
	if (pci_epf_get_required_bar_size(epf, &bar_size, &aligned_mem_size,
					  bar, epc_features, type))
		return -ENOMEM;

	/* [한국어] 어느 인터페이스의 BAR 정보인지 고른다. */
	if (type == PRIMARY_INTERFACE)
		epf_bar = epf->bar;
	else
		epf_bar = epf->sec_epc_bar;

	/* [한국어] BAR 시작 주소를 정렬 경계로 내린다. 요청한 주소보다
	 * 앞이 될 수 있으며, 그래서 BAR 가 요청 범위보다 넓게 열린다. */
	epf_bar[bar].phys_addr = ALIGN_DOWN(bar_addr, aligned_mem_size);

	/* [한국어] 내림 정렬 때문에 시작이 앞으로 밀렸는데 크기가 그만큼
	 * 늘지 않았다면 끝을 덮지 못한다. 그런 경우를 여기서 걸러 낸다.
	 * alloc_space 와 달리 이 함수는 이미 정해진 주소를 받으므로,
	 * 정렬과 크기가 맞아떨어지지 않을 수 있다. */
	if (epf_bar[bar].phys_addr + bar_size < limit)
		return -ENOMEM;

	/* [한국어] addr 이 NULL 인 것이 alloc_space 와의 결정적 차이다.
	 * 이 함수는 메모리를 잡지 않고 이미 있는 물리 영역을 BAR 에
	 * 연결만 하므로, 커널 가상 주소가 없다.
	 * 그래서 pci_epf_free_space() 로 해제해서도 안 된다 —
	 * 그쪽은 addr 이 NULL 이면 곧바로 물러나므로 실수로 불러도
	 * 무해하기는 하다. */
	epf_bar[bar].addr = NULL;
	epf_bar[bar].size = bar_size;
	epf_bar[bar].mem_size = aligned_mem_size;
	epf_bar[bar].barno = bar;
	/* [한국어] 64비트 여부 판단은 alloc_space 와 같다.
	 * 다만 여기서는 보정된 bar_size 가 아니라 인자로 받은 size 를
	 * 보는 점에 주의해야 한다 — 코드에 있는 그대로이며, 그 선택의
	 * 근거는 이 트리에서 확인하지 못했다. */
	if (upper_32_bits(size) || epc_features->bar[bar].only_64bit)
		epf_bar[bar].flags |= PCI_BASE_ADDRESS_MEM_TYPE_64;
	else
		/* [한국어] 4GB 이하이고 64비트 전용도 아니면 32비트 BAR 로 둔다. */
		epf_bar[bar].flags |= PCI_BASE_ADDRESS_MEM_TYPE_32;

	return 0;
}
EXPORT_SYMBOL_GPL(pci_epf_assign_bar_space);

/* [한국어]
 * pci_epf_remove_cfs - 이 드라이버가 만든 configfs 그룹을 모두 없앤다
 *
 * @driver: 정리할 EPF 드라이버.
 * @return: 없음.
 *
 * 드라이버가 등록될 때 자기가 지원하는 이름마다 configfs 그룹을 하나씩
 * 만든다(아래 pci_epf_add_cfs). 그래야 사용자가
 * /sys/kernel/config/pci_ep/functions/pci_epf_test/ 같은 디렉터리를 보고
 * 그 아래에 EPF 인스턴스를 만들 수 있다.
 * 이 함수는 그것들을 되돌린다.
 *
 * 실행 컨텍스트: 드라이버 등록 해제 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_epf_unregister_driver() → [이 함수]
 *   pci_epf_add_cfs() → [이 함수]   (중간 실패 시 정리)
 */
static void pci_epf_remove_cfs(struct pci_epf_driver *driver)
{
	/* [한국어] safe 순회에 필요한 임시 포인터. 아래에서 항목을 지우면서
	 * 순회하므로 다음 항목을 미리 잡아 두어야 한다. */
	struct config_group *group, *tmp;

	/* [한국어] configfs 를 빌드하지 않았으면 만든 것도 없다.
	 * IS_ENABLED 는 컴파일 시점 상수라 이 경우 함수 본체가 통째로
	 * 최적화되어 사라진다. */
	if (!IS_ENABLED(CONFIG_PCI_ENDPOINT_CONFIGFS))
		return;

	/* [한국어] 드라이버 목록을 보호하는 전역 뮤텍스. */
	mutex_lock(&pci_epf_mutex);
	/* [한국어] _safe 판을 쓰는 이유는 pci_ep_cfs_remove_epf_group() 이
	 * 그 group 을 목록에서 빼고 해제하기 때문이다. 일반 순회를 쓰면
	 * 해제된 항목에서 next 를 읽게 된다. */
	list_for_each_entry_safe(group, tmp, &driver->epf_group, group_entry)
		pci_ep_cfs_remove_epf_group(group);
	/* [한국어] 다 지웠으면 목록이 비어 있어야 한다. 남아 있다면
	 * configfs 쪽이 목록에서 빼지 않았다는 뜻이라 버그다. */
	WARN_ON(!list_empty(&driver->epf_group));
	mutex_unlock(&pci_epf_mutex);
}

/**
 * pci_epf_unregister_driver() - unregister the PCI EPF driver
 * @driver: the PCI EPF driver that has to be unregistered
 *
 * Invoke to unregister the PCI EPF driver.
 */
/* [한국어]
 * pci_epf_unregister_driver - EPF 기능 드라이버 등록을 해제한다
 *
 * @driver: 해제할 기능 드라이버.
 * @return: 없음.
 *
 * **__pci_epf_register_driver() 의 짝이며 순서를 뒤집는다** -- 먼저 configfs
 * 항목을 지우고, 그다음 드라이버 모델에서 뗀다.
 *
 * **configfs 를 먼저 지우는 순서가 중요하다.** 사용자가 그 항목을 통해 새
 * EPF 를 만들 수 있는 통로부터 막아야, driver_unregister 도중에 새 장치가
 * 바인드되는 일이 생기지 않는다.
 *
 * driver_unregister 는 이 드라이버에 바인드된 EPF 장치를 모두 언바인드한
 * 뒤에 돌아온다. 그 과정에서 각 EPF 의 unbind 콜백이 불린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 언로드). 잠들 수 있다.
 *
 * 호출 체인:
 *   EPF 기능 드라이버의 module_exit
 *     → [이 함수] → pci_epf_remove_cfs(), driver_unregister()
 */
void pci_epf_unregister_driver(struct pci_epf_driver *driver)
{
	/* [한국어] configfs 그룹을 먼저 없앤다. 사용자가 더는 이 종류의
	 * EPF 를 만들 수 없게 하는 것이 우선이다 — 아래에서 드라이버를
	 * 내리는 동안 새 EPF 가 생기면 곤란하다. */
	pci_epf_remove_cfs(driver);
	/* [한국어] 드라이버를 버스에서 뗀다. 붙어 있던 EPF 들에 대해
	 * pci_epf_device_remove() 가 불린다. */
	driver_unregister(&driver->driver);
}
EXPORT_SYMBOL_GPL(pci_epf_unregister_driver);

/* [한국어]
 * pci_epf_add_cfs - 이 드라이버가 지원하는 이름마다 configfs 그룹을 만든다
 *
 * @driver: 등록 중인 EPF 드라이버.
 * @return: 0 이면 성공. 그룹 생성 실패 시 그 오류.
 *
 * 사용자가 EPF 를 만들려면 configfs 에 그 종류의 디렉터리가 있어야 한다.
 * 드라이버가 지원하는 이름마다 하나씩 만들어 주는 것이 이 함수다.
 *
 * id 표를 훑는 이유가 여기 있다 — 드라이버 하나가 여러 이름을 지원하면
 * 그 각각이 사용자에게 별개의 종류로 보여야 한다.
 *
 * 실행 컨텍스트: 드라이버 등록 중 프로세스 컨텍스트.
 *
 * 에러 경로: 중간에 실패하면 pci_epf_remove_cfs() 로 그때까지 만든 것을
 *   전부 되돌린다. 몇 개까지 만들었는지 따로 세지 않아도 되는 것은
 *   remove_cfs 가 목록을 보고 판단하기 때문이다.
 *
 * 호출 체인:
 *   __pci_epf_register_driver() → [이 함수] → pci_ep_cfs_add_epf_group()
 */
static int pci_epf_add_cfs(struct pci_epf_driver *driver)
{
	struct config_group *group;
	const struct pci_epf_device_id *id;

	/* [한국어] configfs 가 없으면 할 일이 없다. 오류가 아니므로 0. */
	if (!IS_ENABLED(CONFIG_PCI_ENDPOINT_CONFIGFS))
		return 0;

	/* [한국어] 만든 그룹들을 담을 목록을 초기화한다. remove_cfs 가
	 * 이 목록을 보고 정리하므로 반드시 먼저 해야 한다. */
	INIT_LIST_HEAD(&driver->epf_group);

	/* [한국어] id 표를 처음부터 훑는다. 이름이 빈 항목이 끝 표시다. */
	id = driver->id_table;
	while (id->name[0]) {
		/* [한국어] 그 이름의 configfs 그룹을 만든다. 사용자에게는
		 * .../functions/<이름>/ 디렉터리로 보인다. */
		group = pci_ep_cfs_add_epf_group(id->name);
		if (IS_ERR(group)) {
			/* [한국어] 그때까지 만든 것을 전부 되돌린다. */
			pci_epf_remove_cfs(driver);
			return PTR_ERR(group);
		}

		/* [한국어] 목록에 매단다. 이 조작만 락으로 보호하고 그룹
		 * 생성 자체는 락 밖에서 하는데, configfs 쪽이 자체 잠금을
		 * 갖고 있고 그 안에서 오래 걸릴 수 있기 때문이다. */
		mutex_lock(&pci_epf_mutex);
		list_add_tail(&group->group_entry, &driver->epf_group);
		mutex_unlock(&pci_epf_mutex);
		/* [한국어] 다음 id 항목으로. 표의 끝(빈 이름)까지 반복한다. */
		id++;
	}

	return 0;
}

/**
 * __pci_epf_register_driver() - register a new PCI EPF driver
 * @driver: structure representing PCI EPF driver
 * @owner: the owner of the module that registers the PCI EPF driver
 *
 * Invoke to register a new PCI EPF driver.
 */
/* [한국어]
 * __pci_epf_register_driver - EPF 기능 드라이버를 등록한다
 *
 * @driver: 등록할 기능 드라이버.
 * @owner: 그 드라이버를 담은 모듈.
 * @return: 성공 0, ops 가 없거나 필수 콜백이 빠졌으면 -EINVAL.
 *
 * **엔드포인트 기능 드라이버가 세상에 자기를 알리는 함수다.** 등록이 끝나면
 * configfs 아래에 그 이름이 나타나고, 사용자가 그 이름으로 EPF 인스턴스를
 * 만들 수 있게 된다.
 *
 * **bind 와 unbind 를 반드시 요구한다.** 그 둘이 없으면 EPF 를 EPC 에 붙이고
 * 떼는 일 자체가 성립하지 않기 때문이다. 다른 콜백은 없어도 된다 --
 * pci_epc_ 계열의 통지 콜백들은 관심 있는 드라이버만 채운다.
 *
 * **pci_epf_bus_type 에 붙인다.** EPF 는 실제 PCI 버스에 있는 장치가 아니라
 * 소프트웨어가 만들어 낸 것이라, 커널 드라이버 모델 안에 전용 버스 타입을
 * 따로 두고 그 위에서 매칭한다.
 *
 * **driver_register 가 성공한 뒤에 configfs 항목을 만드는 순서** 는 해제
 * 경로와 정확히 대칭이다 -- 등록은 모델 → configfs, 해제는 configfs → 모델.
 * 그래야 configfs 가 열려 있는 동안에는 늘 드라이버가 등록되어 있다.
 *
 * **앞에 밑줄 둘이 붙은 이유**: owner 를 호출자가 직접 넘기지 않도록 헤더가
 * THIS_MODULE 을 채워 주는 매크로로 감싸 두었기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 로드). 잠들 수 있다.
 *
 * 호출 체인:
 *   EPF 기능 드라이버의 module_init(pci_epf_register_driver 매크로 경유)
 *     → [이 함수] → driver_register(), pci_epf_add_cfs()
 */
int __pci_epf_register_driver(struct pci_epf_driver *driver,
			      struct module *owner)
{
	int ret;

	/* [한국어] ops 표가 없으면 이 드라이버는 아무것도 할 수 없다. */
	if (!driver->ops)
		return -EINVAL;

	/* [한국어] bind 와 unbind 는 선택이 아니다. EPF 의 존재 이유가
	 * EPC 에 붙어 무언가처럼 보이는 것이라, 그 두 콜백이 없으면
	 * 등록해 봐야 쓸모가 없다.
	 * 다른 콜백들과 달리 이 둘만 필수인 이유가 그것이다. */
	if (!driver->ops->bind || !driver->ops->unbind)
		return -EINVAL;

	/* [한국어] 이 드라이버를 EPF 가상 버스에 매단다. 이 지정이 있어야
	 * pci_epf_device_match() 가 이 드라이버를 후보로 고려한다. */
	driver->driver.bus = &pci_epf_bus_type;
	/* [한국어] 소유 모듈을 기록한다. bind 가 try_module_get 으로
	 * 참조를 올릴 대상이다. 매크로가 THIS_MODULE 을 넘겨 주므로
	 * 드라이버 작성자가 신경 쓰지 않아도 된다. */
	driver->driver.owner = owner;

	/* [한국어] 드라이버 모델에 등록한다. 이 순간 이미 만들어져 있던
	 * EPF 들과 매칭이 일어날 수 있다. */
	ret = driver_register(&driver->driver);
	if (ret)
		return ret;

	/* [한국어] configfs 그룹을 만든다. 반환값을 확인하지 않는 점이
	 * 눈에 띄는데, 실패해도 드라이버 자체는 등록되어 동작하고
	 * configfs 를 통한 조작만 불가능해지기 때문으로 읽힌다.
	 * 다만 그 판단의 근거를 이 트리에서 확인하지는 못했다. */
	pci_epf_add_cfs(driver);

	return 0;
}
EXPORT_SYMBOL_GPL(__pci_epf_register_driver);

/**
 * pci_epf_destroy() - destroy the created PCI EPF device
 * @epf: the PCI EPF device that has to be destroyed.
 *
 * Invoke to destroy the PCI EPF device created by invoking pci_epf_create().
 */
/* [한국어]
 * pci_epf_destroy - pci_epf_create() 가 만든 EPF 장치를 없앤다
 *
 * @epf: 없앨 엔드포인트 기능.
 * @return: 없음.
 *
 * **한 줄짜리 함수다.** device_unregister() 한 번이 전부이며, 실제 메모리
 * 해제는 참조가 0 이 될 때 release 콜백이 맡는다.
 *
 * **따로 함수로 두는 이유**: 호출자가 커널 장치 모델의 내부 규약을 알
 * 필요가 없게 하려는 것이다. pci_epf_create() 와 이름이 짝을 이루므로,
 * 만든 쪽이 무엇을 불러야 하는지 헷갈리지 않는다.
 *
 * **EPC 에서 떼어 내는 일은 여기서 하지 않는다.** configfs 계층이 링크를
 * 먼저 끊어 pci_epf_unbind() 와 pci_epc_remove_epf() 를 부른 뒤에 이 함수가
 * 불린다는 전제다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci-ep-cfs.c 의 configfs 그룹 해제
 *     → [이 함수] → device_unregister()
 */
void pci_epf_destroy(struct pci_epf *epf)
{
	/* [한국어] device 등록만 해제하면 나머지는 따라온다.
	 * 붙어 있던 드라이버는 pci_epf_device_remove() 로 떨어지고,
	 * 마지막 참조가 사라지면 pci_epf_dev_release() 가 이름과 구조체를
	 * 해제한다. 그래서 이 함수가 한 줄로 끝난다. */
	device_unregister(&epf->dev);
}
EXPORT_SYMBOL_GPL(pci_epf_destroy);

/**
 * pci_epf_create() - create a new PCI EPF device
 * @name: the name of the PCI EPF device. This name will be used to bind the
 *	  EPF device to a EPF driver
 *
 * Invoke to create a new PCI EPF device by providing the name of the function
 * device.
 */
struct pci_epf *pci_epf_create(const char *name)
{
	int ret;
	struct pci_epf *epf;
	struct device *dev;
	/* [한국어] 이름에서 잘라 낼 길이. 아래 strchrnul 계산에 쓴다. */
	int len;

	epf = kzalloc_obj(*epf);
	if (!epf)
		return ERR_PTR(-ENOMEM);

	/* [한국어] 이름에서 첫 '.' 앞까지만 취한다. 이 처리가 왜 필요한가.
	 *
	 * 사용자가 configfs 로 EPF 를 만들 때 같은 종류를 여럿 만들 수 있어야
	 * 하는데, 디렉터리 이름은 유일해야 한다. 그래서 "pci_epf_test.0",
	 * "pci_epf_test.1" 처럼 뒤에 번호를 붙인다.
	 *
	 * 그런데 드라이버를 찾을 때 쓰는 이름은 "pci_epf_test" 여야 한다.
	 * 그래서 device 이름은 전체(dev_set_name 에 name 그대로)를 쓰되,
	 * 매칭에 쓰는 epf->name 은 '.' 앞부분만 잘라 둔다.
	 * strchrnul 은 '.' 이 없으면 문자열 끝을 돌려주므로, 번호를 붙이지
	 * 않은 이름도 그대로 처리된다. */
	len = strchrnul(name, '.') - name;
	epf->name = kstrndup(name, len, GFP_KERNEL);
	if (!epf->name) {
		/* [한국어] 아직 device_initialize 를 하지 않았으므로 여기서는
		 * put_device 가 아니라 직접 kfree 한다. */
		kfree(epf);
		return ERR_PTR(-ENOMEM);
	}

	/* VFs are numbered starting with 1. So set BIT(0) by default */
	/* [한국어] 상류 주석대로 0번 자리를 미리 막아 둔다. 가상 함수 번호는
	 * 1부터 시작해야 하고(0 은 물리 함수 자신), find_first_zero_bit 가
	 * 0 을 돌려주면 안 되기 때문이다. 초기값 1 이 곧 BIT(0) 이다. */
	epf->vfunction_num_map = 1;
	INIT_LIST_HEAD(&epf->pci_vepf);

	dev = &epf->dev;
	device_initialize(dev);
	/* [한국어] 이 가상 버스에 올린다. 이 지정 덕에 아래 device_add 때
	 * 커널이 자동으로 짝이 맞는 드라이버를 찾아 붙인다. */
	dev->bus = &pci_epf_bus_type;
	/* [한국어] release 콜백을 담은 타입. 마지막 참조가 사라지면
	 * pci_epf_dev_release() 가 불려 이름과 구조체를 해제한다. */
	dev->type = &pci_epf_type;
	/* [한국어] 이 EPF 의 상태(vEPF 목록, is_bound 등)를 보호할 락. */
	mutex_init(&epf->lock);

	/* [한국어] device 이름은 번호까지 포함한 전체를 쓴다. sysfs 에서
	 * 유일해야 하기 때문이다. 위에서 epf->name 은 따로 잘라 두었다. */
	ret = dev_set_name(dev, "%s", name);
	if (ret) {
		/* [한국어] device_initialize 이후이므로 put_device 로 정리한다.
		 * 참조가 0 이 되어 release 콜백이 불리고 거기서 name 과
		 * 구조체가 함께 해제된다. */
		put_device(dev);
		return ERR_PTR(ret);
	}

	/* [한국어] 버스에 등록한다. 이 순간 드라이버 매칭이 일어나
	 * pci_epf_device_match() → pci_epf_device_probe() 로 이어질 수 있다. */
	ret = device_add(dev);
	if (ret) {
		put_device(dev);
		return ERR_PTR(ret);
	}

	/* [한국어] 만들어진 EPF 를 돌려준다. 이 시점에는 이미 드라이버가 붙어 있을 수도
	 * 있다 — device_add 가 매칭을 촉발하기 때문이다. */
	return epf;
}
EXPORT_SYMBOL_GPL(pci_epf_create);

/**
 * pci_epf_align_inbound_addr() - Align the given address based on the BAR
 *				  alignment requirement
 * @epf: the EPF device
 * @addr: inbound address to be aligned
 * @bar: the BAR number corresponding to the given addr
 * @base: base address matching the @bar alignment requirement
 * @off: offset to be added to the @base address
 *
 * Helper function to align input @addr based on BAR's alignment requirement.
 * The aligned base address and offset are returned via @base and @off.
 *
 * NOTE: The pci_epf_alloc_space() function already accounts for alignment.
 * This API is primarily intended for use with other memory regions not
 * allocated by pci_epf_alloc_space(), such as peripheral register spaces or
 * the message address of a platform MSI controller.
 *
 * Return: 0 on success, errno otherwise.
 */
/* [한국어]
 * pci_epf_align_inbound_addr - 주어진 주소를 BAR 정렬 요구에 맞춰 나눈다
 *
 * @epf: 대상 엔드포인트 기능.
 * @bar: 그 주소가 속할 BAR 번호.
 * @addr: 정렬할 인바운드 주소.
 * @base: 정렬된 시작 주소를 담아 돌려줄 자리.
 * @off: 그 시작에서 addr 까지의 오프셋을 담아 돌려줄 자리.
 * @return: 늘 0.
 *
 * **BAR 는 자기 크기 경계에 정렬되어야 한다.** 대부분의 EP 컨트롤러가
 * BAR 시작 주소의 하위 비트를 아예 무시하기 때문이며, 상류 주석이 그
 * 사정을 밝힌다. 그래서 임의의 주소를 BAR 로 노출하려면 그 주소를
 * "정렬된 시작 + 그 안에서의 오프셋" 으로 쪼개야 한다.
 *
 * 계산은 두 줄이다 -- round_down 으로 BAR 크기 경계까지 내리고,
 * `addr & (align - 1)` 로 그만큼 밀린 거리를 구한다. 뒤쪽 식이 성립하는
 * 것은 BAR 크기가 2의 거듭제곱이기 때문이다.
 *
 * **언제 쓰는가**: 상류 주석이 명시하듯 pci_epf_alloc_space() 는 이미 정렬을
 * 고려하므로 그 경로에서는 필요 없다. 이 함수는 **그 밖의 메모리 영역** --
 * 주변장치 레지스터나 플랫폼 MSI 컨트롤러의 메시지 주소처럼 주소를 고를 수
 * 없는 것 -- 을 BAR 로 노출할 때 쓴다.
 *
 * **Primary 쪽 epf->bar 만 본다.** Secondary EPC 의 BAR 배열은 보지 않으므로,
 * NTB 구성의 Secondary 쪽에는 이 함수를 쓸 수 없다. 코드는 손대지 않고
 * 사실만 적는다.
 *
 * **늘 0 을 돌려주지만 int 반환형이다** -- 나중에 실패 조건이 생길 여지를
 * 남긴 형태로 보이며, 호출자는 지금도 반환값을 확인하도록 쓰여 있다.
 *
 * 실행 컨텍스트: 어디서든 부를 수 있다. 순수 계산이며 락도 하드웨어 접근도 없다.
 *
 * 호출 체인:
 *   EPF 드라이버 / 플랫폼 MSI 를 다루는 EPF 코드 → [이 함수]
 */
int pci_epf_align_inbound_addr(struct pci_epf *epf, enum pci_barno bar,
			       u64 addr, dma_addr_t *base, size_t *off)
{
	/*
	 * Most EP controllers require the BAR start address to be aligned to
	 * the BAR size, because they mask off the lower bits.
	 *
	 * Alignment to BAR size also works for controllers that support
	 * unaligned addresses.
	 */
	/* [한국어] 상류 주석이 근거를 잘 밝히고 있다. 대부분의 EP 컨트롤러가
	 * BAR 시작 주소의 하위 비트를 잘라 버리므로, 시작 주소가 BAR 크기의
	 * 배수여야 한다. 정렬을 지원하지 않는 컨트롤러에서도 이 방식이
	 * 무해하므로 일률적으로 BAR 크기에 맞춘다.
	 *
	 * 이 함수가 따로 있는 이유는 kernel-doc 이 밝힌 대로다.
	 * pci_epf_alloc_space() 로 잡은 메모리는 이미 정렬되어 있지만,
	 * 그 밖의 영역 — 주변장치 레지스터 공간이나 플랫폼 MSI 컨트롤러의
	 * 메시지 주소(pci-ep-msi.c 의 도어벨이 그것이다) — 을 BAR 로
	 * 노출하려면 직접 정렬을 맞춰야 한다. */
	u64 align = epf->bar[bar].size;

	/* [한국어] 정렬된 시작 주소. 원하는 주소보다 앞이거나 같다. */
	*base = round_down(addr, align);
	/* [한국어] 그 시작점에서 원하는 주소까지의 거리.
	 * align 이 2의 거듭제곱이므로 (align - 1) 마스크로 구할 수 있다.
	 * BAR 크기는 항상 2의 거듭제곱이라 이 전제가 성립한다.
	 * 호출자는 base 를 BAR 에 걸고, 호스트에게는 off 를 알려 준다. */
	*off = addr & (align - 1);

	/* [한국어] 현재 구현은 실패할 수 없지만 int 를 돌려준다.
	 * 앞으로 정렬을 맞출 수 없는 경우를 오류로 알릴 여지를 남긴 것으로
	 * 읽힌다 — 다만 이 트리에서 그 근거를 확인하지는 못했다. */
	return 0;
}
EXPORT_SYMBOL_GPL(pci_epf_align_inbound_addr);

/* [한국어]
 * pci_epf_dev_release - 마지막 참조가 사라졌을 때 EPF 를 해제한다
 *
 * @dev: 해제될 device.
 * @return: 없음.
 *
 * pci_epc_release() 와 같은 역할이다. device 참조 카운트가 0 이 되면
 * 커널이 이 콜백을 부르고, 여기서 실제 메모리를 놓는다.
 *
 * 해제할 것이 둘인데 순서가 중요하다. name 을 먼저 해제하고 구조체를
 * 나중에 해제해야 한다 — 반대로 하면 이미 해제된 구조체에서 name 을
 * 읽게 된다.
 *
 * 실행 컨텍스트: 마지막 put_device() 를 부른 문맥.
 *
 * 호출 체인:
 *   (마지막 put_device) → device 모델 → [이 함수]
 */
static void pci_epf_dev_release(struct device *dev)
{
	struct pci_epf *epf = to_pci_epf(dev);

	/* [한국어] pci_epf_create() 가 kstrndup 으로 잡은 이름. */
	kfree(epf->name);
	/* [한국어] 그다음 구조체 자체. */
	kfree(epf);
}

/* [한국어] EPF device 의 타입. release 콜백 하나만 담는다. 버스가 아니라 타입에
 * 두는 이유는 같은 버스에 성격이 다른 device 가 올 수 있기 때문이다. */
static const struct device_type pci_epf_type = {
	.release	= pci_epf_dev_release,
};

/* [한국어]
 * pci_epf_match_id - 드라이버의 id 표에서 이 EPF 와 맞는 항목을 찾는다
 *
 * @id: 드라이버가 제공한 id 표. 이름이 빈 항목으로 끝난다.
 * @epf: 짝을 찾을 EPF.
 * @return: 맞는 항목. 없으면 NULL.
 *
 * 드라이버 하나가 여러 이름을 지원할 수 있어 표로 되어 있다.
 * 표의 끝은 NULL 포인터가 아니라 이름이 빈 문자열인 항목으로 표시한다 —
 * 배열을 통째로 정적 초기화할 때 편하기 때문이며, 커널의 여러 id 표가
 * 같은 관례를 쓴다.
 *
 * probe 에도 이 함수의 결과가 전달되어, 드라이버가 어느 이름으로
 * 매칭됐는지 알고 그에 따라 다르게 동작할 수 있다.
 *
 * 실행 컨텍스트: 드라이버 매칭 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_epf_device_match() / pci_epf_device_probe() → [이 함수]
 */
static const struct pci_epf_device_id *
pci_epf_match_id(const struct pci_epf_device_id *id, const struct pci_epf *epf)
{
	/* [한국어] 이름이 빈 항목을 만날 때까지 훑는다. */
	while (id->name[0]) {
		/* [한국어] epf->name 은 create 때 '.' 뒤 번호를 떼어 둔 것이라
		 * 여기서 그대로 비교하면 된다. */
		if (strcmp(epf->name, id->name) == 0)
			return id;
		id++;
	}

	return NULL;
}

/* [한국어]
 * pci_epf_device_match - 이 EPF 와 이 드라이버가 짝인지 판단한다
 *
 * @dev: 가상 버스에 올라온 EPF 장치.
 * @drv: 후보 드라이버.
 * @return: 짝이면 0 이 아닌 값.
 *
 * 커널 드라이버 모델의 match 콜백이다. 버스에 장치가 올라오거나 드라이버가
 * 등록될 때마다 모든 조합에 대해 불려 짝을 찾는다.
 *
 * 판단 방식이 둘이다. id 표가 있으면 그것으로 찾고, 없으면 드라이버
 * 이름과 직접 비교한다. 후자는 이름 하나만 지원하는 단순한 드라이버를
 * 위한 지름길이다.
 *
 * 실행 컨텍스트: 드라이버 모델의 매칭 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   device_add() 또는 driver_register() → 버스 매칭 → [이 함수]
 */
static int pci_epf_device_match(struct device *dev, const struct device_driver *drv)
{
	struct pci_epf *epf = to_pci_epf(dev);
	const struct pci_epf_driver *driver = to_pci_epf_driver(drv);

	/* [한국어] id 표가 있으면 그쪽이 우선이다. !! 로 포인터를 0/1 로
	 * 정규화하는데, 이 콜백이 불리언 의미의 int 를 요구하기 때문이다. */
	if (driver->id_table)
		return !!pci_epf_match_id(driver->id_table, epf);

	/* [한국어] 표가 없으면 이름 하나만 지원한다고 보고 직접 비교한다. */
	return !strcmp(epf->name, drv->name);
}

/* [한국어]
 * pci_epf_device_probe - 짝이 맞은 드라이버의 probe 를 부른다
 *
 * @dev: EPF 장치.
 * @return: 드라이버 probe 의 결과. probe 가 없으면 -ENODEV.
 *
 * 버스의 probe 콜백이다. match 가 성공한 뒤 불린다.
 *
 * 여기서 epf->driver 를 채우는 것이 중요하다. 이 필드가 있어야
 * pci_epf_bind() / pci_epf_unbind() 가 드라이버의 ops 를 찾을 수 있다.
 * probe 를 부르기 "전에" 채우는데, 드라이버가 probe 안에서 자기
 * 콜백을 통해 되불릴 수 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   device_add() → 버스 매칭 → [이 함수] → 드라이버의 probe
 */
static int pci_epf_device_probe(struct device *dev)
{
	struct pci_epf *epf = to_pci_epf(dev);
	struct pci_epf_driver *driver = to_pci_epf_driver(dev->driver);

	/* [한국어] probe 없는 드라이버는 붙을 수 없다. */
	if (!driver->probe)
		return -ENODEV;

	/* [한국어] bind/unbind 가 이 값을 통해 드라이버 ops 에 닿는다.
	 * probe 호출 전에 채워야 한다. */
	epf->driver = driver;

	/* [한국어] 매칭된 id 항목을 함께 넘겨, 드라이버가 어느 이름으로
	 * 붙었는지 알 수 있게 한다. id_table 이 NULL 이면 match_id 도
	 * NULL 을 돌려주며, 드라이버는 그 경우를 감안해야 한다. */
	return driver->probe(epf, pci_epf_match_id(driver->id_table, epf));
}

/* [한국어]
 * pci_epf_device_remove - 드라이버가 떨어질 때 remove 를 부른다
 *
 * @dev: EPF 장치.
 * @return: 없음.
 *
 * probe 의 반대다. 드라이버의 remove 를 부른 뒤 epf->driver 를 지운다.
 * 순서가 중요한데, remove 안에서 드라이버가 자기 자원을 정리하며
 * 이 필드를 참조할 수 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   device_del() 또는 driver_unregister() → [이 함수] → 드라이버의 remove
 */
static void pci_epf_device_remove(struct device *dev)
{
	struct pci_epf *epf = to_pci_epf(dev);
	struct pci_epf_driver *driver = to_pci_epf_driver(dev->driver);

	/* [한국어] remove 는 선택이다. 정리할 것이 없는 드라이버도 있다. */
	if (driver->remove)
		driver->remove(epf);
	/* [한국어] 이제 이 EPF 에는 드라이버가 없다. bind 가 이 값을 확인해
	 * dev_WARN 을 내므로 반드시 지워야 한다. */
	epf->driver = NULL;
}

/* [한국어] EPF 가상 버스의 정의. 이 파일 위쪽에서 전방 선언한 그것이다. */
static const struct bus_type pci_epf_bus_type = {
	/* [한국어] /sys/bus/pci-epf/ 로 보이는 이름. */
	.name		= "pci-epf",
	.match		= pci_epf_device_match,
	.probe		= pci_epf_device_probe,
	.remove		= pci_epf_device_remove,
};

/* [한국어]
 * pci_epf_init - 모듈 적재 시 EPF 가상 버스를 등록한다
 *
 * @return: 0 이면 성공. 실패하면 모듈이 적재되지 않는다.
 *
 * 이 버스가 있어야 EPF 장치와 EPF 드라이버가 서로를 찾을 수 있다.
 * 어떤 EPF 드라이버가 등록되기 전에 먼저 실행되어야 하며,
 * 모듈 의존 관계가 그 순서를 보장한다.
 *
 * 실행 컨텍스트: 모듈 적재 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (모듈 적재) → module_init → [이 함수] → bus_register()
 */
static int __init pci_epf_init(void)
{
	int ret;

	/* [한국어] /sys/bus/pci-epf/ 가 만들어진다. */
	ret = bus_register(&pci_epf_bus_type);
	if (ret) {
		/* [한국어] dev_err 가 아니라 pr_err 인 것은 아직 이 모듈에
		 * 딸린 device 가 없기 때문이다. */
		pr_err("failed to register pci epf bus --> %d\n", ret);
		return ret;
	}

	return 0;
}
module_init(pci_epf_init);

/* [한국어]
 * pci_epf_exit - 모듈 제거 시 버스 등록을 해제한다
 *
 * @return: 없음.
 *
 * init 의 반대. 이 시점에는 등록된 EPF 드라이버가 없어야 하며,
 * 그것들이 이 모듈에 의존하므로 모듈 의존 관계가 순서를 지켜 준다.
 *
 * 실행 컨텍스트: 모듈 제거 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (모듈 제거) → module_exit → [이 함수] → bus_unregister()
 */
static void __exit pci_epf_exit(void)
{
	bus_unregister(&pci_epf_bus_type);
}
module_exit(pci_epf_exit);

MODULE_DESCRIPTION("PCI EPF Library");
MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
