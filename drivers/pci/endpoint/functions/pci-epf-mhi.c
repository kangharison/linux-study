// SPDX-License-Identifier: GPL-2.0
/*
 * PCI EPF driver for MHI Endpoint devices
 *
 * Copyright (C) 2023 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

/*
 * [한국어 설명] MHI 엔드포인트 장치용 PCI EPF 드라이버 (pci-epf-mhi.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 MHI(Modem Host Interface) 엔드포인트 스택과 PCI 엔드포인트(EP) 프레임워크를
 * 이어 붙이는 "접착제" 드라이버다. MHI 는 퀄컴 모뎀/SoC 가 PCIe 링크 위에서 호스트와
 * 채널 단위로 데이터를 주고받기 위해 쓰는 프로토콜이며, 호스트 쪽 구현은 MHI 호스트
 * 스택, 엔드포인트 쪽 구현은 MHI EP 스택(이 체크아웃에는 없지만 원본 스냅숏
 * 1f0e418bb6 의 drivers/bus/mhi/ep/ 에 있음)이 담당한다. MHI EP 스택은 자기가 올라탈
 * 물리 버스가 무엇인지 모르는 채로 설계되어 있고, "호스트 메모리를 읽어라 / 써라 /
 * 인터럽트를 올려라" 같은 동작을 전부 콜백(struct mhi_ep_cntrl 의 함수 포인터)으로
 * 위임한다. 이 파일이 바로 그 콜백들을 PCI EP 프레임워크(pci_epc_ 계열 API)로 구현해
 * 준다. 동시에 PCI 쪽에서 보면 이 파일은 하나의 PCI 기능(function)을 정의하는 EPF
 * 드라이버라서, 설정 헤더(vendor/device ID)를 쓰고 BAR 를 열고 MSI 개수를 정하는
 * 일도 함께 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 컨텍스트는 전부 "엔드포인트 쪽 SoC 에서 도는 리눅스 커널"이다. 즉 이 코드가
 * 도는 기계는 PCIe 링크의 호스트가 아니라 장치(모뎀/SoC) 쪽이다. 계층은 위에서부터
 * configfs(pci-ep-cfs.c) → EPF 코어(pci-epf-core.c) → 이 파일 → EPC 코어
 * (pci-epc-core.c) → EPC 컨트롤러 드라이버(예: drivers/pci/controller/dwc/pcie-qcom-ep.c)
 * 순이다. 사용자가 configfs 로 "pci_epf_mhi_sm8450" 라는 이름의 EPF 를 만들고 EPC 에
 * 붙이면 EPF 코어가 이 드라이버의 bind() 를 부르고, 링크가 살아나면 EPC 코어가
 * pci_epc_init_notify()/pci_epc_linkup() 등으로 이 파일의 event_ops 콜백을 부른다.
 * 옆 방향으로는 MHI EP 코어가 이 파일 아래에 매달려서, 자신이 필요할 때마다
 * mhi_cntrl->read_async() 같은 콜백을 통해 이 파일로 되돌아 들어온다. 그러니까 이
 * 파일은 위(PCI EP 프레임워크)와 옆(MHI EP 스택) 두 방향에서 동시에 불리는 위치다.
 *
 * === 타 모듈과의 연결 ===
 * (1) PCI EP 프레임워크: pci_epf_register_driver()/pci_epc_set_bar()/pci_epc_set_msi()/
 *     pci_epc_write_header()/pci_epc_get_features()/pci_epc_mem_alloc_addr()/
 *     pci_epc_map_addr()/pci_epc_raise_irq() 를 쓴다. 이들은 모두
 *     drivers/pci/endpoint/pci-epc-core.c 와 pci-epc-mem.c 에 있다.
 * (2) MHI EP 스택: mhi_ep_register_controller()/mhi_ep_unregister_controller()/
 *     mhi_ep_power_up()/mhi_ep_power_down() 을 부른다(선언은 include/linux/mhi_ep.h).
 *     반대로 MHI EP 코어는 이 파일이 채워 준 mhi_cntrl 의 raise_irq/alloc_map/
 *     unmap_free/read_sync/write_sync/read_async/write_async 콜백을 부른다.
 * (3) dmaengine: eDMA 를 쓰는 플랫폼에서 dma_request_channel()/dmaengine_slave_config()/
 *     dmaengine_prep_slave_single()/dmaengine_submit() 으로 큰 전송을 하드웨어 DMA 에
 *     넘긴다. 실제 채널 제공자는 DesignWare eDMA(drivers/dma/dw-edma) 다.
 * (4) 플랫폼 자원: EPC 의 부모 platform_device 에서 "mmio" 메모리 자원과 "doorbell"
 *     인터럽트를 이름으로 가져온다. 이 두 이름은 DT 바인딩
 *     Documentation/devicetree/bindings/pci/qcom,pcie-ep.yaml 에 정의되어 있다.
 * 데이터 흐름의 핵심: BAR 는 SoC 의 MHI 레지스터 블록("mmio")을 그대로 호스트에
 * 노출한다. 호스트가 그 BAR 에 도어벨을 쓰면 하드웨어가 "doorbell" IRQ 를 올리고,
 * MHI EP 코어가 같은 레지스터 블록을 ioremap 한 mhi_cntrl->mmio 로 읽어 무슨 일이
 * 생겼는지 판단한다. 페이로드 자체는 호스트 RAM 에 있으므로 read/write 콜백이
 * iATU 창이나 eDMA 로 끌어오거나 밀어 넣는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct pci_epf_mhi: 이 드라이버의 인스턴스 상태 전부. mhi_ep_cntrl 을 값으로
 *   품고 있어서 to_epf_mhi() 매크로가 container_of 로 되돌아갈 수 있다.
 * - struct pci_epf_mhi_ep_info: SoC 모델별(sdx55/sm8450/sa8775p) 상수 묶음.
 *   어떤 BAR 를 쓸지, MSI 를 몇 개 쓸지, eDMA 를 쓸지를 여기서 고른다.
 * - pci_epf_mhi_bind(): "mmio" 자원과 "doorbell" IRQ 를 확보하고 ioremap 한다.
 * - pci_epf_mhi_epc_init(): BAR/MSI/설정헤더를 프로그래밍하고 필요하면 DMA 채널을 연다.
 * - pci_epf_mhi_link_up(): mhi_cntrl 콜백 표를 채우고 MHI EP 컨트롤러를 등록한다.
 * - pci_epf_mhi_iatu_read()/_write(): iATU 창을 열어 memcpy 로 호스트 메모리를 옮긴다.
 * - pci_epf_mhi_edma_read()/_write(): 4KB 이상 전송을 eDMA 로 동기 처리한다.
 * - pci_epf_mhi_edma_read_async()/_write_async(): 같은 일을 비동기로 하고 완료를
 *   워크큐에서 MHI 코어에 알린다.
 */

/* [한국어] dmaengine API(dma_request_channel, dmaengine_prep_slave_single,
 * dma_async_issue_pending 등)를 쓰기 위해 필요하다. eDMA 를 가진 플랫폼에서
 * 4KB 이상 페이로드를 CPU memcpy 대신 하드웨어 DMA 로 옮긴다. */
#include <linux/dmaengine.h>
/* [한국어] MHI 엔드포인트 스택의 공개 인터페이스. struct mhi_ep_cntrl(콜백 표를
 * 담은 컨트롤러 구조체), struct mhi_ep_buf_info(전송 서술자), struct
 * mhi_ep_channel_config/mhi_ep_cntrl_config(채널 구성), 그리고
 * mhi_ep_register_controller()/mhi_ep_power_up() 같은 함수 선언이 여기 있다.
 * 이 파일의 존재 이유 자체가 이 헤더의 콜백들을 PCI EP 로 구현하는 것이다. */
#include <linux/mhi_ep.h>
/* [한국어] MODULE_LICENSE/MODULE_AUTHOR/module_init/module_exit 및 THIS_MODULE
 * 매크로를 쓰기 위해 필요하다. 이 드라이버는 모듈로 빌드될 수 있다. */
#include <linux/module.h>
/* [한국어] of_dma_* 계열(디바이스 트리 기반 DMA 채널 조회) 선언을 들여온다.
 * 이 파일은 dma_request_channel() 필터 방식으로 채널을 잡으므로 of_dma_ 함수를
 * 직접 부르지는 않지만, 상류 코드가 DT 기반 DMA 를 전제로 남겨 둔 포함이다. */
#include <linux/of_dma.h>
/* [한국어] EPC 의 부모 장치를 platform_device 로 되돌리는 to_platform_device() 와,
 * "mmio" 자원/"doorbell" IRQ 를 이름으로 찾는 platform_get_resource_byname()/
 * platform_get_irq_byname() 을 쓰기 위해 필요하다. */
#include <linux/platform_device.h>
/* [한국어] PCI 엔드포인트 컨트롤러(EPC) 측 API. pci_epc_set_bar/set_msi/
 * write_header/get_features/map_addr/mem_alloc_addr/raise_irq 와
 * struct pci_epc_features, struct pci_epc_event_ops 가 여기 정의돼 있다. */
#include <linux/pci-epc.h>
/* [한국어] PCI 엔드포인트 기능(EPF) 측 API. struct pci_epf, struct pci_epf_bar,
 * struct pci_epf_driver, struct pci_epf_device_id, epf_set_drvdata() 등이
 * 여기 있다. 이 드라이버는 pci_epf_bus_type 위의 장치 드라이버로 등록된다. */
#include <linux/pci-epf.h>

/* [한국어] MHI 스펙 버전 1.0 을 나타내는 값. 상위 바이트부터 major/minor 순으로
 * 채워진 형식이며, mhi_ep_register_controller() 가 이 값을 그대로 엔드포인트의
 * MHIVER 레지스터에 써서 호스트에게 지원 버전을 알린다. 호스트 MHI 드라이버는
 * 이 레지스터를 읽어 프로토콜 버전을 확인한다. */
#define MHI_VERSION_1_0 0x01000000

/* [한국어] MHI EP 코어가 넘겨 준 struct mhi_ep_cntrl 포인터에서 이 드라이버의
 * 인스턴스 구조체 struct pci_epf_mhi 로 되돌아가는 매크로.
 * 주의: 필드 이름을 "cntrl" 로 적고 있지만 struct pci_epf_mhi 의 실제 필드명은
 * mhi_cntrl 이다. 매크로 인자 이름이 cntrl 이라 container_of 의 세 번째 인자가
 * 인자 치환으로 mhi_cntrl 이 되기 때문에 컴파일이 통과한다 -- 즉 이 매크로는
 * 반드시 to_epf_mhi(mhi_cntrl) 형태로, 인자 변수 이름이 mhi_cntrl 일 때만
 * 올바로 동작한다. 이 파일의 모든 콜백이 첫 줄에서 이 매크로를 쓴다. */
#define to_epf_mhi(cntrl) container_of(cntrl, struct pci_epf_mhi, cntrl)

/* Platform specific flags */
/* [한국어] struct pci_epf_mhi_ep_info 의 flags 필드에 들어가는 비트 0.
 * 이 비트가 서 있으면 해당 SoC 는 PCIe 컨트롤러에 eDMA 엔진이 있으므로
 * 호스트 메모리 접근을 iATU+memcpy 대신 dmaengine 으로 처리한다.
 * 설정자: sm8450_info/sa8775p_info 의 정적 초기화(sdx55 는 세우지 않는다).
 * 읽는 자: pci_epf_mhi_epc_init(), _link_up(), _link_down(), _unbind(),
 * _bus_master_enable() 이 DMA 채널을 열고 닫을지 판단할 때. */
#define MHI_EPF_USE_DMA BIT(0)

/*
 * [한국어]
 * struct pci_epf_mhi_dma_transfer - 비동기 eDMA 전송 한 건의 "뒷정리 표"
 *
 * 비동기 read/write 콜백(pci_epf_mhi_edma_read_async / _write_async)은 DMA 를
 * 제출만 하고 곧바로 돌아온다. 그러면 나중에 DMA 완료 인터럽트 컨텍스트에서
 * (1) dma_map_single 로 잡아 둔 매핑을 풀고 (2) MHI 코어의 완료 콜백
 * buf_info->cb 를 불러야 하는데, 이 둘은 잠들 수 있는 문맥을 요구한다.
 * 그래서 완료에 필요한 정보를 이 구조체에 담아 리스트에 걸어 두고, 워크큐
 * (pci_epf_mhi_dma_worker)에서 꺼내 처리한다. 즉 이 구조체는 "DMA 콜백 →
 * 워크큐" 사이를 건너는 배달 봉투다. kzalloc_obj 로 건마다 새로 할당되고
 * 워커가 kfree 한다.
 */
struct pci_epf_mhi_dma_transfer {
	struct pci_epf_mhi *epf_mhi;
	/* [한국어] 이 전송을 낸 드라이버 인스턴스로 되돌아가는 포인터.
	 * 설정자: pci_epf_mhi_edma_read_async()/_write_async() 가 전송을 제출하기
	 * 직전에 채운다.
	 * 읽는 자: pci_epf_mhi_dma_async_callback() 이 DMA 완료 콜백 문맥에서
	 * 이 포인터로 list_lock 과 dma_list, dma_wq 에 접근한다. 콜백 인자로는
	 * 이 구조체 포인터 하나만 오기 때문에 역참조 경로가 반드시 필요하다.
	 * 값 범위: 유효한 struct pci_epf_mhi 포인터(NULL 불가). EPF 인스턴스는
	 * devm_kzalloc 으로 잡혀 unbind 이후에나 사라지므로 전송 수명보다 길다.
	 * 동기화: 읽기 전용으로만 쓰이므로 별도 락이 필요 없다. */

	struct mhi_ep_buf_info buf_info;
	/* [한국어] MHI 코어가 넘겨 준 전송 서술자의 "값 복사본".
	 * 설정자: _read_async()/_write_async() 가 memcpy 로 통째 복사한다.
	 * 원본 buf_info 는 MHI 코어의 스택 변수라 콜백이 돌아가는 즉시 사라지므로
	 * 포인터로 들고 있으면 안 되고 반드시 복사해야 한다.
	 * 읽는 자: pci_epf_mhi_dma_worker() 가 buf_info->cb(buf_info) 로 MHI
	 * 코어의 완료 처리(예: mhi_ep_read_completion)를 부를 때 쓴다.
	 * 값 범위: host_addr(호스트 PCI 주소), dev_addr(엔드포인트 가상 주소),
	 * size, code(완료 코드), cb/cb_buf 가 채워진 구조체.
	 * 동기화: 이 복사본은 한 전송에 전용이라 경쟁이 없다. */

	struct list_head node;
	/* [한국어] epf_mhi->dma_list 에 이 전송을 매다는 리스트 노드.
	 * 설정자: pci_epf_mhi_dma_async_callback() 이 list_add_tail 로 꼬리에 단다.
	 * 읽는 자: pci_epf_mhi_dma_worker() 가 list_splice_tail_init 으로 통째
	 * 떼어 낸 뒤 list_for_each_entry_safe 로 순회하며 list_del 한다.
	 * 값 범위: 리스트에 걸린 동안만 유효한 연결. 걸리기 전 초기화는 하지 않는다
	 * (list_add_tail 이 양쪽 포인터를 모두 덮어쓰므로 불필요).
	 * 동기화: 반드시 epf_mhi->list_lock 스핀락 아래에서만 조작한다.
	 * 콜백은 인터럽트 문맥일 수 있어 spin_lock(), 워커는 프로세스 문맥이라
	 * spin_lock_irqsave() 를 쓴다. */

	dma_addr_t paddr;
	/* [한국어] dma_map_single() 이 돌려준 엔드포인트 쪽 로컬 버스 주소(IOVA).
	 * 설정자: _read_async() 는 dst_addr 을, _write_async() 는 src_addr 을 넣는다.
	 * 읽는 자: pci_epf_mhi_dma_worker() 가 dma_unmap_single() 로 매핑을 풀 때.
	 * 값 범위: dma_mapping_error() 를 통과한 유효한 DMA 주소.
	 * 동기화: 전송 전용 값이라 락이 필요 없다. 단, 매핑을 푸는 시점이
	 * DMA 완료 이후여야 하므로 순서 자체가 워크큐로 보장된다. */

	enum dma_data_direction dir;
	/* [한국어] dma_unmap_single() 에 넘길 전송 방향. 매핑할 때 쓴 방향과
	 * 정확히 같아야 캐시 무효화/플러시가 올바로 일어난다.
	 * 설정자: 읽기 경로는 DMA_FROM_DEVICE(호스트 → 엔드포인트 버퍼),
	 * 쓰기 경로는 DMA_TO_DEVICE(엔드포인트 버퍼 → 호스트).
	 * 읽는 자: pci_epf_mhi_dma_worker() 의 dma_unmap_single() 세 번째 인자.
	 * 값 범위: DMA_TO_DEVICE 또는 DMA_FROM_DEVICE 둘 중 하나만 쓰인다.
	 * 동기화: 불변값이라 락 불필요. */

	size_t size;
	/* [한국어] 매핑한 버퍼의 바이트 길이. dma_unmap_single() 이 같은 길이를
	 * 요구하므로 따로 기억해 둔다(buf_info.size 와 같은 값이지만, 언매핑
	 * 코드가 buf_info 를 건드리지 않고도 동작하도록 분리해 놓았다).
	 * 설정자: _read_async()/_write_async() 가 buf_info->size 를 복사.
	 * 읽는 자: pci_epf_mhi_dma_worker() 의 dma_unmap_single().
	 * 값 범위: 1 이상, MHI MRU(여기서는 0x8000) 이하.
	 * 동기화: 불변값이라 락 불필요. */
};

/*
 * [한국어]
 * struct pci_epf_mhi_ep_info - SoC 모델별 상수 묶음(하드웨어 기술표)
 *
 * 이 드라이버는 sdx55 / sm8450 / sa8775p 세 종류의 퀄컴 플랫폼을 하나의 코드로
 * 지원한다. 모델마다 다른 것은 (a) PCI 설정 헤더의 device/class 코드, (b) eDMA
 * 유무 정도뿐이라, 차이를 전부 이 상수 구조체로 뽑아내고 코드는 공통으로 둔다.
 * pci_epf_mhi_ids[] 의 driver_data 에 이 구조체의 주소를 kernel_ulong_t 로
 * 숨겨 두었다가, probe 시점에 다시 캐스팅해 epf_mhi->info 에 걸어 둔다.
 * 즉 "이름 매칭 → driver_data → 하드웨어 기술표" 가 이 드라이버의 다형성 기법이다.
 */
struct pci_epf_mhi_ep_info {
	const struct mhi_ep_cntrl_config *config;
	/* [한국어] MHI 채널/버전 구성표. 세 모델 모두 &mhi_v1_config 를 가리킨다.
	 * 설정자: sdx55_info/sm8450_info/sa8775p_info 의 정적 초기화.
	 * 읽는 자: pci_epf_mhi_link_up() 이 mhi_ep_register_controller() 의 두 번째
	 * 인자로 그대로 넘긴다. MHI EP 코어는 이걸 보고 채널 배열을 할당한다.
	 * 값 범위: 유효한 const 포인터. 읽기 전용 정적 데이터.
	 * 동기화: const 정적 데이터라 락 불필요. */

	struct pci_epf_header *epf_header;
	/* [한국어] 호스트가 PCI 설정 공간에서 읽게 될 vendor/device/class 값 묶음.
	 * 설정자: 모델별 정적 초기화(sdx55_header / sm8450_header / sa8775p_header).
	 * 읽는 자: pci_epf_mhi_probe() 가 epf->header 에 복사해 두고,
	 * pci_epf_mhi_epc_init() 이 pci_epc_write_header() 로 실제 설정 공간에 쓴다.
	 * 값 범위: const 가 아닌 이유는 pci_epf_header 를 EPF 코어가 값으로 다룰 수
	 * 있게 하기 위함이며, 이 드라이버는 내용을 바꾸지 않는다.
	 * 동기화: 링크 초기화 시 한 번만 쓰이므로 락 불필요. */

	enum pci_barno bar_num;
	/* [한국어] MHI 레지스터 블록을 노출할 BAR 번호. 세 모델 모두 BAR_0 이다.
	 * MHI 스펙상 호스트는 BAR0 에서 MHI 레지스터를 찾으므로 사실상 고정값이다.
	 * 설정자: 모델별 정적 초기화.
	 * 읽는 자: pci_epf_mhi_epc_init()/_epc_deinit()/_unbind() 가
	 * epf->bar[info->bar_num] 로 해당 BAR 서술자를 고를 때.
	 * 값 범위: BAR_0..BAR_5 (enum pci_barno). 실제로는 BAR_0 만 쓴다.
	 * 동기화: 불변값. */

	u32 epf_flags;
	/* [한국어] BAR 자체의 속성 비트. 여기서는 PCI_BASE_ADDRESS_MEM_TYPE_32,
	 * 즉 "32비트 메모리 BAR" 로 고정한다(64비트 BAR 로 만들면 BAR 두 칸을
	 * 잡아먹어 뒤쪽 BAR 배치가 달라진다).
	 * 설정자: 모델별 정적 초기화.
	 * 읽는 자: pci_epf_mhi_epc_init() 이 epf_bar->flags 에 복사하고,
	 * pci_epc_set_bar() 가 이 비트를 보고 인바운드 창을 프로그래밍한다.
	 * 값 범위: include/uapi/linux/pci_regs.h 의 PCI_BASE_ADDRESS_ 비트 조합.
	 * 동기화: 불변값. */

	u32 msi_count;
	/* [한국어] 이 기능이 호스트에게 요청할 MSI 벡터 개수. 세 모델 모두 32 다.
	 * MHI 는 이벤트 링마다 벡터를 하나씩 쓰므로 여러 벡터가 필요하다.
	 * 설정자: 모델별 정적 초기화.
	 * 읽는 자: pci_epf_mhi_epc_init() 이 order_base_2(msi_count) 로 로그2 를
	 * 취해 pci_epc_set_msi() 에 넘긴다. MSI 능력 레지스터의 Multiple Message
	 * Capable 필드가 "2의 거듭제곱 지수" 형식이기 때문이다(32 → 5).
	 * 값 범위: PCI 스펙상 1,2,4,8,16,32 중 하나여야 한다.
	 * 동기화: 불변값. */

	u32 mru;
	/* [한국어] MRU(Maximum Receive Unit) -- 한 번에 받을 수 있는 최대 페이로드
	 * 크기. 0x8000(32KB)로, include/linux/mhi_ep.h 의
	 * MHI_EP_DEFAULT_MTU 와 같은 값이다.
	 * 설정자: 모델별 정적 초기화.
	 * 읽는 자: pci_epf_mhi_link_up() 이 mhi_cntrl->mru 에 복사한다. MHI EP
	 * 코어는 이 값으로 수신 버퍼 크기를 정한다.
	 * 값 범위: 0 이 아닌 크기. 여기서는 32KB 고정.
	 * 동기화: 불변값. */

	u32 flags;
	/* [한국어] 플랫폼 특성 비트 모음. 현재 정의된 비트는 MHI_EPF_USE_DMA(BIT(0))
	 * 하나뿐이다. sdx55 는 이 비트가 없어 iATU+memcpy 경로만 쓰고,
	 * sm8450/sa8775p 는 비트가 서 있어 eDMA 경로를 쓴다.
	 * 설정자: 모델별 정적 초기화.
	 * 읽는 자: epc_init/epc_deinit/link_up/link_down/bus_master_enable/unbind 이
	 * "DMA 채널을 열까/닫을까", "read_sync 를 eDMA 판으로 바꿀까" 를 판단할 때.
	 * 값 범위: 0 또는 MHI_EPF_USE_DMA.
	 * 동기화: 불변값. */
};

/* [한국어] struct mhi_ep_channel_config 항목 하나를 지정 초기화자로 찍어 내는
 * 매크로. 채널 번호/이름/방향 세 값만 다르고 나머지(num_elements)는 0 으로
 * 남겨 두어 MHI EP 코어가 호스트에게서 받은 값을 쓰게 한다. 아래 채널 표를
 * 26줄로 짧게 유지하기 위한 순수 문법 설탕이다. */
#define MHI_EP_CHANNEL_CONFIG(ch_num, ch_name, direction)	\
	{							\
		.num = ch_num,					\
		.name = ch_name,				\
		.dir = direction,				\
	}

/* [한국어] UL(Uplink) = 호스트 → 엔드포인트 방향 채널. MHI 에서 "업링크" 는
 * 호스트가 보내고 장치가 받는 쪽이므로, 장치(=이 코드가 도는 쪽) 관점에서는
 * 데이터가 들어오는 방향이다. dmaengine 의 DMA_TO_DEVICE 로 표기하는 이유는
 * MHI 코어가 방향을 "호스트 기준" 으로 해석하기 때문이다. */
#define MHI_EP_CHANNEL_CONFIG_UL(ch_num, ch_name)		\
	MHI_EP_CHANNEL_CONFIG(ch_num, ch_name, DMA_TO_DEVICE)

/* [한국어] DL(Downlink) = 엔드포인트 → 호스트 방향 채널. 장치가 보내고 호스트가
 * 받는다. 위와 같은 이유로 DMA_FROM_DEVICE 로 표기한다.
 * MHI 채널은 항상 UL/DL 이 짝수/홀수 번호 쌍으로 붙어 하나의 논리 서비스를
 * 이룬다(예: 0/1 = LOOPBACK, 20/21 = IPCR). */
#define MHI_EP_CHANNEL_CONFIG_DL(ch_num, ch_name)		\
	MHI_EP_CHANNEL_CONFIG(ch_num, ch_name, DMA_FROM_DEVICE)

/*
 * [한국어]
 * mhi_v1_channels[] - 이 엔드포인트가 호스트에게 제공할 MHI 채널 목록
 *
 * MHI 채널 번호는 스펙과 퀄컴 관례로 고정되어 있어 호스트 드라이버가 번호만
 * 보고 어떤 서비스인지 안다. 이름 문자열은 엔드포인트 쪽 MHI 클라이언트
 * 드라이버를 매칭하는 데 쓰인다. 번호가 연속이 아닌 이유(21 다음이 32, 33 다음이
 * 46)는 중간 번호대가 이 플랫폼에서 쓰지 않는 다른 서비스에 예약돼 있기 때문이다.
 * 설정자: 컴파일 타임 상수(수정 불가, const).
 * 읽는 자: mhi_v1_config.ch_cfg 를 통해 mhi_ep_register_controller() 가 읽어
 * 채널별 struct mhi_ep_chan 을 할당한다.
 */
static const struct mhi_ep_channel_config mhi_v1_channels[] = {
	MHI_EP_CHANNEL_CONFIG_UL(0, "LOOPBACK"),	/* [한국어] 채널 0: 루프백 시험용 UL. 받은 것을 그대로 되돌려 링크 검증에 쓴다. */
	MHI_EP_CHANNEL_CONFIG_DL(1, "LOOPBACK"),	/* [한국어] 채널 1: 루프백의 DL 짝. UL 로 받은 데이터를 호스트로 되돌린다. */
	MHI_EP_CHANNEL_CONFIG_UL(2, "SAHARA"),		/* [한국어] 채널 2: SAHARA 프로토콜 UL. 부팅 시 호스트가 펌웨어 이미지를 내려보내는 통로. */
	MHI_EP_CHANNEL_CONFIG_DL(3, "SAHARA"),		/* [한국어] 채널 3: SAHARA DL. 장치가 크래시 덤프를 호스트로 올릴 때 쓴다. */
	MHI_EP_CHANNEL_CONFIG_UL(4, "DIAG"),		/* [한국어] 채널 4: 퀄컴 DIAG 진단 프로토콜 UL(호스트 명령). */
	MHI_EP_CHANNEL_CONFIG_DL(5, "DIAG"),		/* [한국어] 채널 5: DIAG DL(장치 로그/응답). */
	MHI_EP_CHANNEL_CONFIG_UL(6, "SSR"),		/* [한국어] 채널 6: SSR(Subsystem Restart) UL. 서브시스템 재시작 통지 채널. */
	MHI_EP_CHANNEL_CONFIG_DL(7, "SSR"),		/* [한국어] 채널 7: SSR DL 짝. */
	MHI_EP_CHANNEL_CONFIG_UL(8, "QDSS"),		/* [한국어] 채널 8: QDSS(Qualcomm Debug Subsystem) 트레이스 UL. */
	MHI_EP_CHANNEL_CONFIG_DL(9, "QDSS"),		/* [한국어] 채널 9: QDSS DL. 하드웨어 트레이스 스트림을 호스트로 흘린다. */
	MHI_EP_CHANNEL_CONFIG_UL(10, "EFS"),		/* [한국어] 채널 10: EFS(Embedded File System) 접근 UL. 모뎀 NV 영역 읽기/쓰기용. */
	MHI_EP_CHANNEL_CONFIG_DL(11, "EFS"),		/* [한국어] 채널 11: EFS DL 짝. */
	MHI_EP_CHANNEL_CONFIG_UL(12, "MBIM"),		/* [한국어] 채널 12: MBIM(Mobile Broadband Interface Model) 제어 UL. 표준 WWAN 제어 채널. */
	MHI_EP_CHANNEL_CONFIG_DL(13, "MBIM"),		/* [한국어] 채널 13: MBIM DL 짝. */
	MHI_EP_CHANNEL_CONFIG_UL(14, "QMI"),		/* [한국어] 채널 14: QMI(Qualcomm MSM Interface) 제어 UL. 모뎀 서비스 요청. */
	MHI_EP_CHANNEL_CONFIG_DL(15, "QMI"),		/* [한국어] 채널 15: QMI DL 짝. */
	MHI_EP_CHANNEL_CONFIG_UL(16, "QMI"),		/* [한국어] 채널 16: 두 번째 QMI UL 인스턴스. 이름이 같아도 채널 번호가 다르면 별도 파이프다. */
	MHI_EP_CHANNEL_CONFIG_DL(17, "QMI"),		/* [한국어] 채널 17: 두 번째 QMI DL 짝. */
	MHI_EP_CHANNEL_CONFIG_UL(18, "IP-CTRL-1"),	/* [한국어] 채널 18: IP 데이터 경로 제어용 UL. */
	MHI_EP_CHANNEL_CONFIG_DL(19, "IP-CTRL-1"),	/* [한국어] 채널 19: IP-CTRL-1 DL 짝. */
	MHI_EP_CHANNEL_CONFIG_UL(20, "IPCR"),		/* [한국어] 채널 20: IPCR(IPC Router) UL. 커널 IPC 라우터 메시지 통로. */
	MHI_EP_CHANNEL_CONFIG_DL(21, "IPCR"),		/* [한국어] 채널 21: IPCR DL 짝. */
	MHI_EP_CHANNEL_CONFIG_UL(32, "DUN"),		/* [한국어] 채널 32: DUN(Dial-Up Networking) UL. AT 명령 포트로 노출된다. */
	MHI_EP_CHANNEL_CONFIG_DL(33, "DUN"),		/* [한국어] 채널 33: DUN DL 짝. */
	MHI_EP_CHANNEL_CONFIG_UL(46, "IP_SW0"),		/* [한국어] 채널 46: 소프트웨어 경로 IP 데이터 UL. 실제 네트워크 패킷이 흐른다. */
	MHI_EP_CHANNEL_CONFIG_DL(47, "IP_SW0"),		/* [한국어] 채널 47: IP_SW0 DL 짝. 장치가 호스트로 올리는 패킷 경로. */
};

/*
 * [한국어]
 * mhi_v1_config - MHI 스펙 1.0 용 컨트롤러 구성. 세 SoC 가 공유한다.
 */
static const struct mhi_ep_cntrl_config mhi_v1_config = {
	.max_channels = 128,
	/* [한국어] 이 컨트롤러가 지원한다고 선언하는 최대 채널 수. 위 표에 실제로
	 * 정의된 것은 26개뿐이지만, MHI EP 코어는 max_channels 크기의 채널 배열을
	 * 통째로 할당하고 호스트가 임의 번호(최대 127)를 열려 해도 받아 준다.
	 * 그래서 표에 없는 번호는 "정의는 안 됐지만 슬롯은 있는" 상태가 된다. */
	.num_channels = ARRAY_SIZE(mhi_v1_channels),
	/* [한국어] 위 표의 항목 수(=26). ARRAY_SIZE 로 계산하므로 표를 늘리면
	 * 자동으로 따라간다. MHI EP 코어가 ch_cfg 배열을 이 개수만큼 순회한다. */
	.ch_cfg = mhi_v1_channels,
	/* [한국어] 채널 정의 표의 시작 주소. 코어가 여기서 번호/이름/방향을 읽어
	 * 각 채널의 내부 상태 구조체를 초기화한다. */
	.mhi_version = MHI_VERSION_1_0,
	/* [한국어] 호스트에게 알릴 MHI 프로토콜 버전. mhi_ep_register_controller()
	 * 가 이 값을 엔드포인트의 MHIVER 레지스터에 직접 기록한다. */
};

/*
 * [한국어]
 * sdx55_header - SDX55 모뎀이 호스트의 PCI 설정 공간에 내보일 신원 정보
 *
 * 호스트가 링크 열거(enumeration) 중 이 기능의 설정 공간을 읽으면 아래 값들이
 * 보인다. pci_epf_mhi_epc_init() 이 pci_epc_write_header() 로 실제 하드웨어
 * 설정 공간에 기록한다. const 가 아닌 이유는 pci_epf_header 포인터를 받는
 * 상류 API 가 const 를 붙이지 않았기 때문이며 이 드라이버는 값을 바꾸지 않는다.
 */
static struct pci_epf_header sdx55_header = {
	.vendorid = PCI_VENDOR_ID_QCOM,
	/* [한국어] 벤더 ID = 퀄컴(0x17cb, include/linux/pci_ids.h). 호스트 MHI
	 * 드라이버(drivers/bus/mhi/host/pci_generic.c 계열)가 이 값과 아래
	 * deviceid 조합으로 자기 장치임을 알아본다. */
	.deviceid = 0x0306,
	/* [한국어] SDX55 모뎀의 디바이스 ID. 이 숫자는 퀄컴이 실제 SDX55 PCIe
	 * 모뎀에 부여한 값과 같아야 호스트 쪽 드라이버가 붙는다. sm8450 도 같은
	 * 0x0306 을 쓰는데, 호스트 입장에서 두 SoC 를 같은 모델로 보이게 하려는
	 * 의도다(class 코드로 구분한다). */
	.baseclass_code = PCI_BASE_CLASS_COMMUNICATION,
	/* [한국어] 기본 클래스 0x07 = 통신 컨트롤러. 호스트가 드라이버 없이도
	 * lspci 에서 "Communication controller" 로 표시하게 한다. */
	.subclass_code = PCI_CLASS_COMMUNICATION_MODEM & 0xff,
	/* [한국어] PCI_CLASS_COMMUNICATION_MODEM 은 0x0703 으로 상위 바이트에
	 * 기본 클래스(0x07)를, 하위 바이트에 서브클래스(0x03=모뎀)를 함께 담고
	 * 있다. subclass_code 필드는 u8 이므로 & 0xff 로 하위 바이트만 떼어 낸다.
	 * 이 마스킹이 없으면 상위 바이트가 잘려 경고가 나거나 값이 뒤틀린다. */
	.interrupt_pin	= PCI_INTERRUPT_INTA,
	/* [한국어] 설정 공간의 Interrupt Pin 필드를 INTA 로 채운다. 실제 인터럽트는
	 * MSI 로 올리지만, 스펙상 레거시 INTx 를 쓸 수 있는 기능은 이 필드가
	 * 0 이 아니어야 하므로 관례적으로 INTA 를 적어 둔다. */
};

/*
 * [한국어]
 * sdx55_info - SDX55 플랫폼용 하드웨어 기술표. eDMA 를 쓰지 않는 유일한 모델.
 * pci_epf_mhi_ids[] 에서 "pci_epf_mhi_sdx55" 이름과 짝지어진다.
 */
static const struct pci_epf_mhi_ep_info sdx55_info = {
	.config = &mhi_v1_config,
	/* [한국어] 위에서 정의한 공용 MHI v1 채널 구성을 그대로 쓴다. */
	.epf_header = &sdx55_header,
	/* [한국어] 바로 위 설정 헤더를 가리킨다. probe 에서 epf->header 로 옮겨진다. */
	.bar_num = BAR_0,
	/* [한국어] MHI 레지스터 블록을 BAR0 로 노출한다. MHI 호스트 스택이
	 * BAR0 에서 레지스터를 찾도록 되어 있어 사실상 고정이다. */
	.epf_flags = PCI_BASE_ADDRESS_MEM_TYPE_32,
	/* [한국어] BAR0 를 32비트 메모리 BAR 로 선언한다. 64비트로 하면 BAR0/BAR1
	 * 두 칸을 소모하므로 여기서는 32비트를 택했다. */
	.msi_count = 32,
	/* [한국어] MSI 벡터 32개. epc_init 에서 order_base_2(32)=5 로 변환돼
	 * pci_epc_set_msi() 에 전달된다. */
	.mru = 0x8000,
	/* [한국어] 32KB. MHI EP 코어의 수신 단위. mhi_ep.h 의 MHI_EP_DEFAULT_MTU
	 * 와 같은 값이다. */
	/* [한국어] .flags 를 생략했으므로 0 -- 즉 MHI_EPF_USE_DMA 가 꺼져 있다.
	 * SDX55 는 eDMA 를 쓰지 않고 iATU 창 + memcpy 경로만 사용한다. */
};

/*
 * [한국어]
 * sm8450_header - SM8450 SoC 가 호스트에 내보일 설정 헤더.
 * SDX55 와 달리 통신 클래스가 아니라 "기타(0xff)" 로 신고한다.
 */
static struct pci_epf_header sm8450_header = {
	.vendorid = PCI_VENDOR_ID_QCOM,
	/* [한국어] 퀄컴 벤더 ID. sdx55 와 같다. */
	.deviceid = 0x0306,
	/* [한국어] SDX55 와 동일한 디바이스 ID. 호스트 쪽 MHI 드라이버가 같은
	 * ID 항목으로 두 플랫폼을 모두 받아들이게 하려는 선택이다. */
	.baseclass_code = PCI_CLASS_OTHERS,
	/* [한국어] 0xff = "기타 장치". SM8450 은 모뎀 전용 칩이 아니라 AP SoC 를
	 * 엔드포인트로 세우는 구성이라 모뎀 클래스로 신고하지 않는다.
	 * subclass_code 를 생략했으므로 0 이 된다. */
	.interrupt_pin = PCI_INTERRUPT_INTA,
	/* [한국어] 관례상 INTA. 실제 인터럽트 전달은 MSI 로 한다. */
};

/*
 * [한국어]
 * sm8450_info - SM8450 플랫폼 기술표. eDMA 를 켠다는 점만 sdx55 와 다르다.
 */
static const struct pci_epf_mhi_ep_info sm8450_info = {
	.config = &mhi_v1_config,
	/* [한국어] 채널 구성은 공용 v1 표를 공유한다. */
	.epf_header = &sm8450_header,
	/* [한국어] 바로 위 SM8450 전용 설정 헤더. */
	.bar_num = BAR_0,
	/* [한국어] MHI 레지스터를 BAR0 로 노출(전 모델 공통). */
	.epf_flags = PCI_BASE_ADDRESS_MEM_TYPE_32,
	/* [한국어] 32비트 메모리 BAR. */
	.msi_count = 32,
	/* [한국어] MSI 벡터 32개. */
	.mru = 0x8000,
	/* [한국어] 수신 단위 32KB. */
	.flags = MHI_EPF_USE_DMA,
	/* [한국어] eDMA 사용 비트. 이 한 줄 때문에 epc_init 이 DMA 채널을 잡고,
	 * link_up 이 read_sync/write_sync 를 eDMA 판으로 교체하며,
	 * read_async/write_async 도 비동기 eDMA 판이 된다. */
};

/*
 * [한국어]
 * sa8775p_header - SA8775P(자동차용 SoC) 가 호스트에 내보일 설정 헤더.
 * 세 모델 중 유일하게 다른 디바이스 ID(0x0116)를 쓴다.
 */
static struct pci_epf_header sa8775p_header = {
	.vendorid = PCI_VENDOR_ID_QCOM,
	/* [한국어] 퀄컴 벤더 ID. */
	.deviceid = 0x0116,
	/* [한국어] SA8775P 전용 디바이스 ID. sdx55/sm8450 의 0x0306 과 달라
	 * 호스트가 이 플랫폼을 별도 항목으로 구분할 수 있다. */
	.baseclass_code = PCI_CLASS_OTHERS,
	/* [한국어] 0xff = 기타. SM8450 과 같은 이유(모뎀이 아닌 AP SoC). */
	.interrupt_pin = PCI_INTERRUPT_INTA,
	/* [한국어] 관례상 INTA. */
};

/*
 * [한국어]
 * sa8775p_info - SA8775P 플랫폼 기술표. sm8450 과 사실상 동일하며 헤더만 다르다.
 */
static const struct pci_epf_mhi_ep_info sa8775p_info = {
	.config = &mhi_v1_config,
	/* [한국어] 공용 v1 채널 구성. */
	.epf_header = &sa8775p_header,
	/* [한국어] 바로 위 SA8775P 전용 설정 헤더. */
	.bar_num = BAR_0,
	/* [한국어] MHI 레지스터를 BAR0 로 노출. */
	.epf_flags = PCI_BASE_ADDRESS_MEM_TYPE_32,
	/* [한국어] 32비트 메모리 BAR. */
	.msi_count = 32,
	/* [한국어] MSI 벡터 32개. */
	.mru = 0x8000,
	/* [한국어] 수신 단위 32KB. */
	.flags = MHI_EPF_USE_DMA,
	/* [한국어] eDMA 사용. SA8775P 의 PCIe 컨트롤러에도 DesignWare eDMA 가 있다. */
};

/*
 * [한국어]
 * struct pci_epf_mhi - 이 EPF 드라이버 인스턴스의 전체 상태
 *
 * pci_epf_mhi_probe() 에서 devm_kzalloc 으로 한 개 할당되어 epf_set_drvdata()
 * 로 EPF 장치에 매달린다. 그 뒤 모든 콜백이 epf_get_drvdata(epf) 또는
 * to_epf_mhi(mhi_cntrl) 로 이 구조체를 되찾는다. 즉 이 구조체가 PCI 쪽 세계
 * (epf, epc_features)와 MHI 쪽 세계(mhi_cntrl), 그리고 DMA 쪽 세계
 * (dma_chan_*, dma_wq)를 한 곳에 묶는 접합점이다. mhi_cntrl 을 포인터가 아니라
 * 값으로 품고 있기 때문에 MHI 코어가 콜백에 넘겨 준 mhi_cntrl 포인터에서
 * container_of 로 이 구조체를 복원할 수 있다.
 */
struct pci_epf_mhi {
	const struct pci_epc_features *epc_features;
	/* [한국어] EPC 컨트롤러가 보고한 능력/제약 표. 이 드라이버가 실제로 쓰는
	 * 필드는 align(인바운드/아웃바운드 매핑의 정렬 요구치) 하나뿐이다.
	 * 설정자: pci_epf_mhi_epc_init() 이 pci_epc_get_features() 결과를 저장.
	 * 읽는 자: get_align_offset() 이 호스트 PCI 주소의 하위 비트를 떼어 낼 때.
	 * 값 범위: NULL 이면 epc_init 이 -ENODATA 로 실패한다. 성공 시 EPC 드라이버
	 * 소유의 정적 구조체를 가리키므로 해제하지 않는다.
	 * 동기화: epc_init 에서 한 번 쓰고 이후로는 읽기만 하므로 락이 필요 없다. */

	const struct pci_epf_mhi_ep_info *info;
	/* [한국어] 어느 SoC 모델인지에 따라 고른 하드웨어 기술표.
	 * 설정자: pci_epf_mhi_probe() 가 id->driver_data 를 캐스팅해 저장.
	 * 읽는 자: 거의 모든 콜백이 bar_num/msi_count/mru/flags 를 읽는다.
	 * 값 범위: &sdx55_info / &sm8450_info / &sa8775p_info 중 하나.
	 * 동기화: probe 이후 불변이라 락 불필요. */

	struct mhi_ep_cntrl mhi_cntrl;
	/* [한국어] MHI EP 코어에 등록할 컨트롤러 구조체. 포인터가 아니라 값으로
	 * 품고 있다는 점이 중요하다 -- to_epf_mhi() 의 container_of 가 성립하는
	 * 근거이며, 별도 할당/해제가 없어 수명 관리가 단순해진다.
	 * 설정자: pci_epf_mhi_link_up() 이 mmio/irq/mru/cntrl_dev 와 일곱 개
	 * 콜백 포인터를 채운 뒤 mhi_ep_register_controller() 에 넘긴다.
	 * 읽는 자: MHI EP 코어 전체. 또 이 파일의 link_down/unbind/epc_deinit 이
	 * mhi_cntrl.mhi_dev 가 NULL 인지 보고 "등록된 적 있는가" 를 판정한다.
	 * 값 범위: probe 직후엔 devm_kzalloc 덕에 전부 0. 등록 후엔 코어가 채운다.
	 * 동기화: 내부 필드는 코어가 자기 락(state_lock/event_lock/list_lock)으로
	 * 보호한다. 이 파일은 mhi_dev 포인터만 관측용으로 읽는다. */

	struct pci_epf *epf;
	/* [한국어] 자기를 담고 있는 EPF 장치로 되돌아가는 포인터.
	 * 설정자: pci_epf_mhi_probe().
	 * 읽는 자: MHI 콜백들이 epf->epc, epf->func_no, epf->vfunc_no 를 얻어
	 * pci_epc_ 계열 API 를 부를 때. MHI 코어는 epf 를 모르므로 이 경로가
	 * 유일한 연결 고리다.
	 * 값 범위: 유효한 포인터(NULL 불가).
	 * 동기화: 불변값. */

	struct mutex lock;
	/* [한국어] 호스트 메모리 접근 경로 전체를 직렬화하는 뮤텍스.
	 * iATU 경로에서는 아웃바운드 창(pci_epc_mem_alloc_addr + map_addr)이
	 * 한정된 자원이라 동시 사용 시 서로 덮어쓸 위험이 있고, eDMA 경로에서는
	 * dmaengine_slave_config() 로 채널의 src/dst 주소를 매번 갈아 끼우므로
	 * 두 전송이 겹치면 엉뚱한 주소로 DMA 가 날아간다. 그래서 iatu_read/write,
	 * edma_read/write, edma_read_async/write_async 가 모두 이 락을 잡는다.
	 * 설정자: pci_epf_mhi_probe() 의 mutex_init().
	 * 읽는 자/잡는 자: 위 여섯 개 데이터 이동 콜백.
	 * 값 범위: 일반 뮤텍스. 잠들 수 있으므로 이 콜백들은 프로세스 문맥에서만
	 * 불려야 한다(MHI 코어의 워커/워크큐 문맥).
	 * 동기화: 이 락 자체가 동기화 수단. */

	void __iomem *mmio;
	/* [한국어] SoC 의 MHI 레지스터 블록을 엔드포인트 CPU 가 접근하도록
	 * ioremap 한 가상 주소. 호스트가 BAR0 로 보는 것과 물리적으로 같은 영역이다.
	 * 설정자: pci_epf_mhi_bind() 의 ioremap(). 해제는 unbind 의 iounmap().
	 * 읽는 자: pci_epf_mhi_link_up() 이 mhi_cntrl->mmio 에 넘겨 주고, 그 뒤로는
	 * MHI EP 코어의 mhi_ep_mmio_read/write 가 readl/writel 로 접근한다.
	 * 값 범위: 성공 시 유효한 __iomem 포인터, 실패 시 bind 가 -ENOMEM 반환.
	 * 동기화: MMIO 접근 순서는 readl/writel 의 배리어 의미로 보장된다. */

	resource_size_t mmio_phys;
	/* [한국어] 같은 MHI 레지스터 블록의 물리 주소. BAR 를 프로그래밍할 때
	 * 필요하다 -- 호스트가 BAR0 에 접근하면 인바운드 iATU 가 이 물리 주소로
	 * 번역해 주어야 하기 때문이다.
	 * 설정자: pci_epf_mhi_bind() 가 platform_get_resource_byname("mmio") 의
	 * res->start 를 저장.
	 * 읽는 자: pci_epf_mhi_epc_init() 이 epf_bar->phys_addr 에 복사.
	 * 값 범위: DT 가 기술한 실제 물리 주소.
	 * 동기화: bind 이후 불변. */

	struct dma_chan *dma_chan_tx;
	/* [한국어] 엔드포인트 → 호스트 방향(쓰기) 전용 eDMA 채널.
	 * 설정자: pci_epf_mhi_dma_init() 이 BIT(DMA_MEM_TO_DEV) 필터로 요청.
	 * 해제는 pci_epf_mhi_dma_deinit() 의 dma_release_channel().
	 * 읽는 자: pci_epf_mhi_edma_write() 와 _write_async().
	 * 값 범위: MHI_EPF_USE_DMA 가 꺼진 플랫폼에서는 끝까지 NULL 이다.
	 * 동기화: 채널 설정(dmaengine_slave_config)은 epf_mhi->lock 아래에서만
	 * 한다. 그래서 한 번에 한 전송만 이 채널을 쓴다. */

	struct dma_chan *dma_chan_rx;
	/* [한국어] 호스트 → 엔드포인트 방향(읽기) 전용 eDMA 채널.
	 * 설정자: pci_epf_mhi_dma_init() 이 BIT(DMA_DEV_TO_MEM) 필터로 요청.
	 * 읽는 자: pci_epf_mhi_edma_read() 와 _read_async().
	 * 값 범위: DMA 미사용 플랫폼에서는 NULL.
	 * 동기화: tx 채널과 동일하게 epf_mhi->lock 으로 직렬화된다.
	 * tx/rx 를 분리한 이유는 DesignWare eDMA 가 읽기 채널과 쓰기 채널을
	 * 물리적으로 다른 엔진으로 두기 때문이다. */

	struct workqueue_struct *dma_wq;
	/* [한국어] 비동기 DMA 완료 뒷정리를 돌릴 전용 워크큐.
	 * 설정자: pci_epf_mhi_dma_init() 의 alloc_workqueue("pci_epf_mhi_dma_wq").
	 * 파괴는 pci_epf_mhi_dma_deinit() 의 destroy_workqueue().
	 * 읽는 자: pci_epf_mhi_dma_async_callback() 이 queue_work() 로 작업을 넣을 때.
	 * 값 범위: 성공 시 유효한 포인터, 실패 시 dma_init 이 -ENOMEM.
	 * 동기화: 워크큐 내부에서 관리한다. WQ_PERCPU 로 만들어 CPU 지역성을 유지한다. */

	struct work_struct dma_work;
	/* [한국어] 위 워크큐에 넣는 유일한 작업 항목. 핸들러는
	 * pci_epf_mhi_dma_worker() 다. 항목이 하나뿐이라도 문제가 없는 이유는
	 * 워커가 dma_list 를 통째로 떼어 내 한 번에 여러 건을 처리하기 때문이다.
	 * 이미 큐에 들어 있는 동안 queue_work 를 또 불러도 워크큐가 중복 삽입을
	 * 막아 주고, 그 사이 리스트에 쌓인 항목은 다음 실행에서 처리된다.
	 * 설정자: pci_epf_mhi_dma_init() 의 INIT_WORK().
	 * 읽는 자: 워크큐 코어.
	 * 동기화: 워크큐 코어가 pending 비트로 관리한다. */

	struct list_head dma_list;
	/* [한국어] 완료됐지만 아직 뒷정리하지 않은 struct pci_epf_mhi_dma_transfer
	 * 들의 목록(FIFO).
	 * 설정자: pci_epf_mhi_dma_init() 의 INIT_LIST_HEAD 로 비어 있게 시작.
	 * 쓰는 자: pci_epf_mhi_dma_async_callback() 이 꼬리에 추가.
	 * 읽는 자: pci_epf_mhi_dma_worker() 가 통째로 떼어 내 비운다.
	 * 값 범위: 비어 있을 수도, 여러 건이 쌓여 있을 수도 있다.
	 * 동기화: 반드시 list_lock 아래에서만 만진다. */

	spinlock_t list_lock;
	/* [한국어] dma_list 를 보호하는 스핀락. 뮤텍스가 아닌 스핀락인 이유는
	 * DMA 완료 콜백이 인터럽트/tasklet 문맥에서 불릴 수 있어 잠들면 안 되기
	 * 때문이다. 콜백 쪽은 spin_lock(), 프로세스 문맥인 워커 쪽은
	 * spin_lock_irqsave() 를 쓴다.
	 * 설정자: pci_epf_mhi_dma_init() 의 spin_lock_init().
	 * 잡는 자: pci_epf_mhi_dma_async_callback(), pci_epf_mhi_dma_worker().
	 * 동기화: 이 락 자체가 동기화 수단. epf_mhi->lock 뮤텍스와는 별개의
	 * 자원(리스트)을 지키므로 중첩해 잡히지 않는다. */

	u32 mmio_size;
	/* [한국어] MHI 레지스터 블록의 바이트 크기. ioremap 크기이자 BAR0 의 크기다.
	 * 설정자: pci_epf_mhi_bind() 의 resource_size(res).
	 * 읽는 자: bind 의 ioremap(), epc_init 의 epf_bar->size.
	 * 값 범위: DT 의 "mmio" reg 항목이 정한 크기(qcom 예시에서는 0x1000 대).
	 * 동기화: bind 이후 불변. */

	int irq;
	/* [한국어] 호스트의 도어벨 쓰기를 알려 주는 "doorbell" 인터럽트 번호.
	 * 이름과 달리 이 파일은 이 IRQ 에 핸들러를 걸지 않는다 -- 번호만 확보해
	 * mhi_cntrl->irq 로 넘기고, 실제 request_irq() 는 MHI EP 코어의
	 * mhi_ep_register_controller() 가 한다(핸들러 mhi_ep_irq, 이름
	 * "doorbell_irq", IRQ_NOAUTOEN 으로 등록해 power_up 때 활성화).
	 * 설정자: pci_epf_mhi_bind() 의 platform_get_irq_byname(pdev, "doorbell").
	 * 읽는 자: pci_epf_mhi_link_up() 이 mhi_cntrl->irq 에 복사.
	 * 값 범위: 0 보다 큰 가상 IRQ 번호. 음수면 bind 실패.
	 * 동기화: bind 이후 불변. */
};

/*
 * [한국어]
 * get_align_offset - 호스트 PCI 주소가 EPC 정렬 경계에서 얼마나 벗어났는지 계산
 *
 * @epf_mhi: 이 드라이버 인스턴스. epc_features->align 을 읽기 위해 필요하다.
 * @addr: 호스트(=PCI 버스) 쪽 주소. MHI 코어가 준 buf_info->host_addr 이거나
 *        MHI 컨텍스트 구조체의 호스트 물리 주소다.
 * @return: addr 의 하위 비트, 즉 정렬 경계로부터의 오프셋(0 .. align-1).
 *
 * 왜 필요한가: PCIe 엔드포인트 컨트롤러의 아웃바운드 주소 변환기(iATU)는
 * 아무 주소나 창의 시작점으로 잡을 수 없고, 하드웨어가 정한 정렬 단위
 * (epc_features->align, 예를 들어 64KB)의 배수여야 한다. 그런데 MHI 가 읽고
 * 싶어 하는 호스트 주소는 임의의 값이다. 그래서 "원하는 주소를 정렬 경계까지
 * 내림한 지점" 부터 창을 열고, 창 안에서 이 오프셋만큼 더 들어간 곳을 실제
 * 데이터의 시작으로 삼는다. 이 함수는 그 오프셋을 구한다.
 *
 * 동작: align 이 2의 거듭제곱이라는 전제 아래 (align - 1) 마스크로 하위 비트를
 * 뽑는다. 즉 addr % align 과 같지만 나눗셈 없이 계산한다. align 이 0 이면
 * 마스크가 SIZE_MAX 가 되어 addr 전체를 오프셋으로 돌려주는 셈이 되는데,
 * EPC 드라이버들은 align 을 0(제약 없음) 또는 2의 거듭제곱으로만 보고한다
 * -- 이 트리의 pci_epc_features 정의에는 align==0 을 이 코드가 어떻게
 * 처리해야 하는지에 대한 명시적 근거를 찾지 못했다.
 *
 * 실행 컨텍스트: 호출자가 모두 프로세스 문맥(MHI 코어의 워커)이며, 순수 계산
 * 함수라 재진입에 안전하고 락을 잡지 않는다.
 * 에러 경로: 없다. 항상 값을 돌려준다.
 *
 * 호출 체인:
 *   pci_epf_mhi_alloc_map / _unmap_free / _iatu_read / _iatu_write
 *     → [get_align_offset]
 */
static size_t get_align_offset(struct pci_epf_mhi *epf_mhi, u64 addr)
{
	/* [한국어] align 은 2의 거듭제곱이므로 (align-1) 은 하위 비트가 전부 1인
	 * 마스크가 된다. 이 마스크와 AND 하면 정렬 경계에서의 잔여 오프셋만 남는다.
	 * 예: align=64KB, addr=0x1234_5678 → 0x5678 이 오프셋. */
	return addr & (epf_mhi->epc_features->align -1);
}

/*
 * [한국어]
 * __pci_epf_mhi_alloc_map - 호스트 메모리 한 구간을 엔드포인트 주소 공간에 창으로 연다
 *
 * @mhi_cntrl: MHI EP 코어가 넘겨 준 컨트롤러. to_epf_mhi() 로 이 드라이버
 *             인스턴스를 복원하는 데만 쓴다.
 * @pci_addr: 매핑하려는 호스트 쪽 PCI 주소(정렬되지 않은 원래 주소).
 * @paddr: [출력] 엔드포인트 로컬 물리 주소. 정렬 보정을 마친 "데이터 시작점"이
 *         담겨 돌아간다.
 * @vaddr: [출력] 같은 지점의 __iomem 가상 주소. memcpy_fromio 등에 바로 쓴다.
 * @offset: 미리 계산해 둔 정렬 오프셋(get_align_offset 의 결과).
 * @return: 0 성공. -ENOMEM 이면 아웃바운드 창 주소 공간이 바닥났고, 그 밖의
 *          음수면 iATU 프로그래밍이 실패한 것이다. 호출자는 그대로 상위로 전파한다.
 *
 * 왜 필요한가: 엔드포인트 CPU 는 호스트 RAM 을 직접 볼 수 없다. 보려면 EPC 의
 * 아웃바운드 창을 하나 잡아 "이 로컬 물리 주소 구간을 저 PCI 주소로 보내라" 고
 * iATU 에 프로그래밍해야 한다. 이 함수가 그 두 단계(주소 공간 예약 + 변환
 * 프로그래밍)를 묶는다.
 *
 * 동작 단계:
 *  1) pci_epc_mem_alloc_addr() 로 EPC 의 아웃바운드 주소 공간에서 size+offset
 *     바이트를 예약하고 ioremap 된 가상 주소를 받는다.
 *  2) pci_epc_map_addr() 로 그 로컬 구간이 (pci_addr - offset) 부터 시작하는
 *     호스트 주소를 향하도록 iATU 를 프로그래밍한다. 시작점을 offset 만큼
 *     당기는 이유는 창의 시작이 정렬 경계여야 하기 때문이다.
 *  3) 성공하면 출력 주소 둘 다에 offset 을 다시 더해, 호출자에게는 "정확히
 *     pci_addr 에 대응하는 지점" 을 돌려준다. 즉 정렬 보정은 이 함수 안에서
 *     완전히 감춰진다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 내부에서 잠들 수 있는 할당을 하므로 인터럽트
 * 문맥에서 부르면 안 된다. 호출자들이 epf_mhi->lock 뮤텍스를 이미 잡고 있거나
 * (iatu_read/write) MHI 코어의 워커 문맥이다(alloc_map).
 *
 * 에러 경로: 2단계가 실패하면 1단계에서 예약한 주소 공간을 반드시 되돌려야
 * 하므로 pci_epc_mem_free_addr() 를 부른 뒤 오류를 반환한다. 이때 *paddr/*vaddr
 * 에는 이미 값이 들어가 있지만 호출자는 오류 시 이를 쓰지 않는다.
 *
 * 호출 체인:
 *   MHI EP 코어(mhi_ep_alloc_host_ctx 등) → pci_epf_mhi_alloc_map
 *     → [__pci_epf_mhi_alloc_map] → pci_epc_mem_alloc_addr / pci_epc_map_addr
 *   pci_epf_mhi_iatu_read / _iatu_write → [__pci_epf_mhi_alloc_map]
 */
static int __pci_epf_mhi_alloc_map(struct mhi_ep_cntrl *mhi_cntrl, u64 pci_addr,
				 phys_addr_t *paddr, void __iomem **vaddr,
				 size_t offset, size_t size)
{
	/* [한국어] container_of 로 MHI 컨트롤러에서 이 드라이버 인스턴스를 복원한다.
	 * MHI 코어는 pci_epf_mhi 를 모르므로 이 되짚기가 유일한 통로다. */
	struct pci_epf_mhi *epf_mhi = to_epf_mhi(mhi_cntrl);
	/* [한국어] PCI 쪽 세계로 건너가기 위한 EPF 장치 포인터. func_no/vfunc_no 가
	 * 여기 들어 있어 어느 PCI 기능의 창을 여는지 지정할 수 있다. */
	struct pci_epf *epf = epf_mhi->epf;
	/* [한국어] 실제로 iATU 를 가진 엔드포인트 컨트롤러. 모든 pci_epc_ 호출의
	 * 첫 인자다. bind 시점에 EPF 코어가 epf->epc 에 채워 둔 값이다. */
	struct pci_epc *epc = epf->epc;
	int ret;

	/* [한국어] 1단계: EPC 의 아웃바운드 주소 공간(pci_epc_mem)에서 size+offset
	 * 바이트를 예약한다. offset 을 더하는 이유는 창을 정렬 경계까지 앞당겨
	 * 열기 때문에 그만큼 앞부분이 더 필요해서다. 반환값은 ioremap 된 가상
	 * 주소이고, 대응하는 로컬 물리 주소는 *paddr 로 나온다. */
	*vaddr = pci_epc_mem_alloc_addr(epc, paddr, size + offset);
	/* [한국어] 아웃바운드 창 주소 공간이 고갈됐을 때 NULL 이 온다. 창 개수와
	 * 크기는 하드웨어가 정한 한정 자원이라 실제로 마를 수 있다. */
	if (!*vaddr)
		return -ENOMEM;

	/* [한국어] 2단계: 방금 예약한 로컬 구간 *paddr 이 호스트의
	 * (pci_addr - offset) 을 향하도록 iATU 아웃바운드 변환을 건다.
	 * 시작 주소에서 offset 을 빼는 것이 정렬 보정의 핵심이다 -- 하드웨어는
	 * 정렬된 시작점만 받으므로 원하는 지점보다 앞에서 창을 시작하고,
	 * 나중에 오프셋만큼 더해 들어간다. */
	ret = pci_epc_map_addr(epc, epf->func_no, epf->vfunc_no, *paddr,
			       pci_addr - offset, size + offset);
	/* [한국어] iATU 창이 남아 있지 않거나 정렬/크기 제약을 어겼을 때 진입한다. */
	if (ret) {
		/* [한국어] 1단계에서 예약한 주소 공간을 되돌린다. 이걸 빼먹으면
		 * 아웃바운드 주소 공간이 서서히 새어 나가 결국 -ENOMEM 만 나오게 된다.
		 * 크기는 예약할 때와 똑같이 size+offset 이어야 비트맵이 맞는다. */
		pci_epc_mem_free_addr(epc, *paddr, *vaddr, size + offset);
		return ret;
	}

	/* [한국어] 정렬 때문에 앞당겨 연 만큼을 다시 더해, 호출자에게는 정확히
	 * pci_addr 에 해당하는 로컬 물리 주소를 돌려준다. */
	*paddr = *paddr + offset;
	/* [한국어] 가상 주소도 같은 양만큼 전진시킨다. 이 값이 곧 memcpy_fromio /
	 * memcpy_toio 의 대상이 된다. 짝이 되는 해제 함수
	 * __pci_epf_mhi_unmap_free() 는 반대로 offset 을 빼서 원래 시작점을 복원한다. */
	*vaddr = *vaddr + offset;

	return 0;
}

/*
 * [한국어]
 * pci_epf_mhi_alloc_map - MHI 코어의 alloc_map 콜백 구현(정렬 오프셋을 스스로 계산)
 *
 * @mhi_cntrl: MHI EP 코어의 컨트롤러 포인터.
 * @pci_addr: 매핑할 호스트 PCI 주소.
 * @paddr: [출력] 대응하는 엔드포인트 로컬 물리 주소.
 * @vaddr: [출력] 대응하는 __iomem 가상 주소.
 * @size: 매핑할 바이트 수.
 * @return: __pci_epf_mhi_alloc_map() 의 반환값을 그대로 전달. 0 성공, 음수 실패.
 *
 * 왜 필요한가: MHI EP 코어의 alloc_map 콜백 시그니처에는 offset 인자가 없다.
 * 반면 내부 구현 __pci_epf_mhi_alloc_map() 은 iatu_read/iatu_write 와 코드를
 * 공유하기 위해 offset 을 인자로 받는다. 이 얇은 래퍼가 그 간극을 메운다 --
 * 오프셋을 스스로 구해서 넘겨 주는 어댑터다.
 *
 * 동작: get_align_offset() 으로 정렬 잔여분을 구한 뒤 그대로 내부 함수에 위임한다.
 * 그 이상은 하지 않는다.
 *
 * 실행 컨텍스트: MHI EP 코어의 프로세스 문맥(컨트롤러 등록/전원 켜기 경로).
 * 이 경로는 epf_mhi->lock 을 잡지 않는데, 호스트 컨텍스트 캐시를 만드는 초기화
 * 단계에서만 불리고 데이터 경로와 겹치지 않기 때문이다.
 * 에러 경로: 내부 함수가 반환한 오류를 그대로 올린다. MHI 코어는 실패 시
 * 컨트롤러 전원 켜기를 포기한다.
 *
 * 호출 체인:
 *   MHI EP 코어(mhi_cntrl->alloc_map 호출부) → [pci_epf_mhi_alloc_map]
 *     → get_align_offset → __pci_epf_mhi_alloc_map
 */
static int pci_epf_mhi_alloc_map(struct mhi_ep_cntrl *mhi_cntrl, u64 pci_addr,
				 phys_addr_t *paddr, void __iomem **vaddr,
				 size_t size)
{
	/* [한국어] epc_features->align 을 읽기 위해 드라이버 인스턴스를 복원한다. */
	struct pci_epf_mhi *epf_mhi = to_epf_mhi(mhi_cntrl);
	/* [한국어] 호스트 주소가 EPC 정렬 경계에서 얼마나 벗어났는지 구한다.
	 * 이 값이 내부 함수의 창 앞당김 계산에 그대로 쓰인다. */
	size_t offset = get_align_offset(epf_mhi, pci_addr);

	/* [한국어] 실제 작업은 전부 내부 함수가 한다. 반환값도 손대지 않고 전달한다. */
	return __pci_epf_mhi_alloc_map(mhi_cntrl, pci_addr, paddr, vaddr,
				      offset, size);
}

/*
 * [한국어]
 * __pci_epf_mhi_unmap_free - __pci_epf_mhi_alloc_map() 이 연 창을 정확히 되돌린다
 *
 * @mhi_cntrl: MHI 컨트롤러(인스턴스 복원용).
 * @pci_addr: 매핑할 때 쓴 호스트 주소. 현재 구현에서는 실제로 쓰이지 않고
 *            대칭성을 위해 시그니처에만 남아 있다(오프셋을 인자로 따로 받으므로
 *            여기서 다시 계산할 필요가 없다).
 * @paddr: alloc_map 이 돌려준 (오프셋이 더해진) 로컬 물리 주소.
 * @vaddr: alloc_map 이 돌려준 (오프셋이 더해진) 가상 주소.
 * @offset: 매핑할 때 쓴 것과 같은 정렬 오프셋. 다르면 해제가 어긋난다.
 * @size: 매핑할 때 쓴 것과 같은 크기.
 * @return: 없음. 해제는 실패를 알릴 방법이 없어 void 다.
 *
 * 왜 필요한가: 아웃바운드 창과 그 주소 공간은 한정 자원이라 쓰고 나면 반드시
 * 돌려줘야 한다. 특히 iatu_read/write 는 전송 한 건마다 창을 열고 닫으므로
 * 이 함수가 빠지면 몇 번 만에 창이 바닥난다.
 *
 * 동작 단계:
 *  1) paddr - offset 으로 창의 진짜 시작점(정렬된 주소)을 복원한 뒤
 *     pci_epc_unmap_addr() 로 iATU 변환을 해제한다.
 *  2) 같은 방식으로 복원한 주소들과 size+offset 크기로
 *     pci_epc_mem_free_addr() 를 불러 주소 공간 예약을 반납한다.
 * 두 단계 모두 alloc 때와 "완전히 같은 인자" 여야 EPC 코어의 비트맵이 맞는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 epf_mhi->lock 을 잡고 있는 경우
 * (iatu_read/write)와 그렇지 않은 경우(unmap_free 콜백)가 모두 있다.
 * 에러 경로: 없다. pci_epc_unmap_addr() 는 인자가 잘못돼도 조용히 물러난다.
 *
 * 호출 체인:
 *   pci_epf_mhi_unmap_free / _iatu_read / _iatu_write
 *     → [__pci_epf_mhi_unmap_free] → pci_epc_unmap_addr / pci_epc_mem_free_addr
 */
static void __pci_epf_mhi_unmap_free(struct mhi_ep_cntrl *mhi_cntrl,
				     u64 pci_addr, phys_addr_t paddr,
				     void __iomem *vaddr, size_t offset,
				     size_t size)
{
	/* [한국어] 드라이버 인스턴스 복원 -- epf/epc 를 얻기 위한 첫 걸음이다. */
	struct pci_epf_mhi *epf_mhi = to_epf_mhi(mhi_cntrl);
	/* [한국어] 어느 PCI 기능의 창인지 지정할 func_no/vfunc_no 를 얻는다. */
	struct pci_epf *epf = epf_mhi->epf;
	/* [한국어] iATU 를 실제로 조작하는 엔드포인트 컨트롤러. */
	struct pci_epc *epc = epf->epc;

	/* [한국어] alloc 쪽에서 offset 을 더해 돌려줬으므로, 여기서는 빼서
	 * 창의 실제 시작(정렬된) 물리 주소를 복원한 뒤 iATU 변환을 해제한다.
	 * 이 주소가 어긋나면 EPC 코어가 해당 창을 찾지 못해 해제가 무시된다. */
	pci_epc_unmap_addr(epc, epf->func_no, epf->vfunc_no, paddr - offset);
	/* [한국어] 주소 공간 예약도 같은 방식으로 되돌린다. 물리/가상 주소 모두
	 * offset 을 빼서 원래 시작점으로 되돌리고, 크기는 예약할 때와 동일하게
	 * size + offset 을 넘긴다. 세 인자 중 하나라도 어긋나면 EPC 코어의
	 * 비트맵 해제가 엉뚱한 페이지를 풀어 버린다. */
	pci_epc_mem_free_addr(epc, paddr - offset, vaddr - offset,
			      size + offset);
}

/*
 * [한국어]
 * pci_epf_mhi_unmap_free - MHI 코어의 unmap_free 콜백 구현
 *
 * @mhi_cntrl: MHI EP 코어의 컨트롤러 포인터.
 * @pci_addr: 매핑할 때 쓴 호스트 PCI 주소. 여기서 정렬 오프셋을 다시 계산한다.
 * @paddr: pci_epf_mhi_alloc_map() 이 돌려줬던 로컬 물리 주소.
 * @vaddr: 같은 지점의 가상 주소.
 * @size: 매핑 크기.
 * @return: 없음.
 *
 * 왜 필요한가: pci_epf_mhi_alloc_map() 의 짝이다. alloc 쪽이 오프셋을 스스로
 * 계산했으므로 해제 쪽도 같은 방식으로 계산해야 정확히 같은 값이 나온다.
 * pci_addr 이 같으면 align 도 같으니 결과도 반드시 같다 -- 이 대칭성이 이
 * 설계가 성립하는 이유다.
 *
 * 동작: 오프셋을 다시 구해 내부 함수에 위임한다.
 * 실행 컨텍스트: MHI EP 코어의 프로세스 문맥(컨트롤러 전원 끄기/해제 경로).
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   MHI EP 코어(mhi_cntrl->unmap_free 호출부) → [pci_epf_mhi_unmap_free]
 *     → get_align_offset → __pci_epf_mhi_unmap_free
 */
static void pci_epf_mhi_unmap_free(struct mhi_ep_cntrl *mhi_cntrl, u64 pci_addr,
				   phys_addr_t paddr, void __iomem *vaddr,
				   size_t size)
{
	/* [한국어] align 값을 읽기 위해 드라이버 인스턴스를 복원한다. */
	struct pci_epf_mhi *epf_mhi = to_epf_mhi(mhi_cntrl);
	/* [한국어] 매핑할 때와 같은 pci_addr 로 같은 오프셋을 재계산한다.
	 * 저장해 두지 않고 다시 계산하는 편이 상태를 줄여 더 안전하다. */
	size_t offset = get_align_offset(epf_mhi, pci_addr);

	/* [한국어] 실제 해제는 내부 함수가 수행한다. */
	__pci_epf_mhi_unmap_free(mhi_cntrl, pci_addr, paddr, vaddr, offset,
				 size);
}

/*
 * [한국어]
 * pci_epf_mhi_raise_irq - MHI 코어의 raise_irq 콜백 구현. 호스트에 MSI 를 쏜다.
 *
 * @mhi_cntrl: MHI EP 코어의 컨트롤러 포인터.
 * @vector: MHI 가 정한 0-기반 MSI 벡터 번호. 이벤트 링마다 벡터가 배정돼 있어,
 *          어떤 이벤트 링에 완료가 쌓였는지를 호스트에 알리는 식별자 역할을 한다.
 * @return: 없음. pci_epc_raise_irq() 의 반환값을 검사하지 않는다 -- MHI 코어의
 *          콜백 원형이 void 라 실패를 위로 알릴 방법이 없기 때문이다.
 *
 * 왜 필요한가: 엔드포인트가 이벤트 링에 완료 항목을 채워 넣어도 호스트는 그
 * 사실을 모른다. MSI 를 한 발 쏴야 호스트 MHI 드라이버가 깨어나 이벤트 링을
 * 읽는다. 이것이 MHI 의 "장치 → 호스트" 통지 경로다. (참고로 반대 방향인
 * "호스트 → 장치" 통지는 호스트가 BAR0 의 도어벨 레지스터에 쓰고 하드웨어가
 * "doorbell" IRQ 를 올리는 경로로, 이 함수와는 무관하다.)
 *
 * 동작: 인스턴스와 epc 를 복원한 뒤 pci_epc_raise_irq() 를 PCI_IRQ_MSI 타입으로
 * 한 번 부른다. 벡터 번호에 1 을 더하는 것이 유일한 실질 로직이다.
 *
 * 실행 컨텍스트: MHI EP 코어의 이벤트 처리 경로에서 불린다. pci_epc_raise_irq()
 * 는 내부에서 epc->lock 뮤텍스를 잡으므로 잠들 수 있다 -- 따라서 이 콜백은
 * 반드시 프로세스 문맥(코어의 워커)에서만 불려야 한다.
 * 에러 경로: 없음(반환값 무시). 실패하면 호스트가 인터럽트를 못 받아 폴링
 * 지연으로 나타난다.
 *
 * 호출 체인:
 *   MHI EP 코어(mhi_cntrl->raise_irq 호출부) → [pci_epf_mhi_raise_irq]
 *     → pci_epc_raise_irq → EPC 드라이버의 ops->raise_irq
 */
static void pci_epf_mhi_raise_irq(struct mhi_ep_cntrl *mhi_cntrl, u32 vector)
{
	/* [한국어] MHI 컨트롤러에서 이 드라이버 인스턴스를 복원한다. */
	struct pci_epf_mhi *epf_mhi = to_epf_mhi(mhi_cntrl);
	/* [한국어] MSI 를 쏠 PCI 기능 번호(func_no/vfunc_no)를 얻기 위한 EPF 포인터. */
	struct pci_epf *epf = epf_mhi->epf;
	/* [한국어] 실제로 MSI 트랜잭션을 발생시킬 엔드포인트 컨트롤러. */
	struct pci_epc *epc = epf->epc;

	/*
	 * MHI supplies 0 based MSI vectors but the API expects the vector
	 * number to start from 1, so we need to increment the vector by 1.
	 */
	/* [한국어] 위 영문 주석대로, 두 API 의 번호 기준이 한 칸 어긋나 있다.
	 * MHI 는 벡터를 0..(msi_count-1) 로 세는데 pci_epc_raise_irq() 의
	 * interrupt_num 인자는 1-기반이라 1..msi_count 를 기대한다. 그래서
	 * vector + 1 을 넘긴다. 이 +1 을 빼먹으면 EPC 코어가 0 을 잘못된 벡터로
	 * 판단해 거부하거나, 벡터 하나가 통째로 어긋나 호스트가 엉뚱한 이벤트
	 * 링을 뒤지게 된다. PCI_IRQ_MSI 는 MSI-X 나 레거시 INTx 가 아니라
	 * 일반 MSI 로 쏘라는 지정이다. */
	pci_epc_raise_irq(epc, epf->func_no, epf->vfunc_no, PCI_IRQ_MSI,
			  vector + 1);
}

/*
 * [한국어]
 * pci_epf_mhi_iatu_read - 호스트 메모리를 iATU 창 + CPU 복사로 읽어 온다
 *
 * @mhi_cntrl: MHI EP 코어의 컨트롤러 포인터.
 * @buf_info: 전송 서술자. host_addr(원본, 호스트 PCI 주소), dev_addr(목적지,
 *            엔드포인트 커널 가상 주소), size(길이), cb(완료 콜백)를 담고 있다.
 *            MHI 코어가 스택에 만든 구조체를 가리키는 경우가 많으므로 이 함수가
 *            반환한 뒤에는 유효하지 않다고 봐야 한다.
 * @return: 0 성공, 음수면 창을 열지 못한 것. 이 함수 자체의 복사는 실패할 수 없다.
 *
 * 왜 필요한가: eDMA 가 없는 플랫폼(sdx55)에서는 이것이 유일한 호스트 메모리
 * 읽기 수단이다. eDMA 가 있는 플랫폼에서도 4KB 미만의 작은 전송은 DMA 설정
 * 비용이 복사 비용보다 커서 이 경로로 넘어온다(edma_read 의 앞부분 분기 참고).
 *
 * 동작 단계:
 *  1) 정렬 오프셋을 구한다.
 *  2) epf_mhi->lock 을 잡는다 -- 아웃바운드 창이 한정 자원이라 동시에 두
 *     전송이 열면 서로를 밀어낼 수 있기 때문이다.
 *  3) 창을 열어(__pci_epf_mhi_alloc_map) 호스트 구간을 로컬 주소로 노출시킨다.
 *  4) memcpy_fromio() 로 CPU 가 직접 읽어 엔드포인트 버퍼에 담는다. 이 읽기가
 *     실제 PCIe 메모리 읽기 트랜잭션(TLP)을 일으켜 호스트 RAM 까지 다녀온다.
 *  5) 창을 닫고 락을 푼다.
 *  6) 완료 콜백이 있으면 부른다. 락 밖에서 부르는 것이 중요하다 -- 콜백이
 *     다시 이 드라이버로 들어와 lock 을 잡으려 하면 교착이 되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. mutex_lock 과 창 할당 때문에 잠들 수 있다.
 * MHI EP 코어의 링 처리 워커나 컨트롤러 초기화 경로에서 불린다.
 * 에러 경로: 창 열기 실패 시 락을 풀고 오류를 그대로 반환한다. 이때
 * buf_info->cb 는 부르지 않으므로, MHI 코어가 자기 쪽에서 버퍼를 정리한다.
 *
 * 호출 체인:
 *   MHI EP 코어(read_sync/read_async) → [pci_epf_mhi_iatu_read]
 *     → __pci_epf_mhi_alloc_map → memcpy_fromio → __pci_epf_mhi_unmap_free
 *   pci_epf_mhi_edma_read (4KB 미만일 때) → [pci_epf_mhi_iatu_read]
 */
static int pci_epf_mhi_iatu_read(struct mhi_ep_cntrl *mhi_cntrl,
				 struct mhi_ep_buf_info *buf_info)
{
	/* [한국어] 드라이버 인스턴스 복원. lock 과 epc_features 에 접근하려면 필요하다. */
	struct pci_epf_mhi *epf_mhi = to_epf_mhi(mhi_cntrl);
	/* [한국어] 호스트 주소의 정렬 잔여분. 창을 앞당겨 여는 양이자, 창 안에서
	 * 실제 데이터가 시작하는 지점까지의 거리다. */
	size_t offset = get_align_offset(epf_mhi, buf_info->host_addr);
	/* [한국어] 열린 창을 통해 호스트 메모리를 보게 될 __iomem 가상 주소.
	 * 이름의 TRE 는 Transfer Ring Element -- MHI 의 전송 서술자 항목을 가리킨다. */
	void __iomem *tre_buf;
	/* [한국어] 같은 지점의 엔드포인트 로컬 물리 주소. 해제할 때 필요하다. */
	phys_addr_t tre_phys;
	int ret;

	/* [한국어] 아웃바운드 창 자원을 독점하기 위해 잠근다. 이 락이 없으면 두
	 * 전송이 동시에 pci_epc_mem_alloc_addr 과 iATU 프로그래밍을 하다가 서로의
	 * 창 설정을 덮어써 엉뚱한 호스트 주소를 읽게 된다. */
	mutex_lock(&epf_mhi->lock);

	/* [한국어] 호스트의 host_addr 부터 size 바이트를 로컬 주소 공간에 노출시킨다.
	 * 성공하면 tre_phys/tre_buf 에 정렬 보정을 마친 주소가 담긴다. */
	ret = __pci_epf_mhi_alloc_map(mhi_cntrl, buf_info->host_addr, &tre_phys,
				      &tre_buf, offset, buf_info->size);
	/* [한국어] 아웃바운드 주소 공간 고갈(-ENOMEM)이나 iATU 창 부족으로 실패한 경우. */
	if (ret) {
		/* [한국어] 실패했더라도 락은 반드시 풀어 준다. */
		mutex_unlock(&epf_mhi->lock);
		return ret;
	}

	/* [한국어] 창을 통해 CPU 가 호스트 메모리를 읽어 엔드포인트 버퍼로 옮긴다.
	 * memcpy_fromio 를 쓰는 이유는 원본이 일반 RAM 이 아니라 MMIO 창이라
	 * 컴파일러의 재배치/최적화를 막고 적절한 접근 폭을 쓰기 위해서다.
	 * 이 한 줄이 실제 PCIe 메모리 읽기 TLP 여러 개로 풀린다 -- 그래서 큰
	 * 전송에서는 eDMA 보다 훨씬 느리다. */
	memcpy_fromio(buf_info->dev_addr, tre_buf, buf_info->size);

	/* [한국어] 복사가 끝났으니 창과 주소 공간을 즉시 반납한다. 전송마다 열고
	 * 닫는 구조라 반납이 늦으면 곧바로 자원이 마른다. 인자는 열 때와 완전히
	 * 같은 조합(host_addr, offset, size)이어야 한다. */
	__pci_epf_mhi_unmap_free(mhi_cntrl, buf_info->host_addr, tre_phys,
				 tre_buf, offset, buf_info->size);

	/* [한국어] 창 자원을 다 돌려준 뒤 락을 푼다. */
	mutex_unlock(&epf_mhi->lock);

	/* [한국어] 이 함수는 read_sync 뿐 아니라 read_async 자리에도 쓰이므로
	 * (DMA 없는 플랫폼에서는 둘 다 이 함수를 가리킨다) 완료 콜백이 달려 올 수
	 * 있다. 있으면 반드시 불러 줘야 MHI 코어가 이벤트 링에 완료를 올린다. */
	if (buf_info->cb)
		/* [한국어] 락을 푼 뒤에 부르는 것이 핵심이다. 콜백(mhi_ep_read_completion
		 * 등)은 다시 MHI 코어로 들어가 이벤트 링을 갱신하고 raise_irq 까지
		 * 부를 수 있는데, 그 경로가 이 락을 다시 잡으면 교착이 된다. */
		buf_info->cb(buf_info);

	return 0;
}

/*
 * [한국어]
 * pci_epf_mhi_iatu_write - 엔드포인트 버퍼를 iATU 창 + CPU 복사로 호스트에 쓴다
 *
 * @mhi_cntrl: MHI EP 코어의 컨트롤러 포인터.
 * @buf_info: 전송 서술자. 읽기와 방향만 반대다 -- dev_addr 가 원본(엔드포인트
 *            커널 버퍼), host_addr 이 목적지(호스트 PCI 주소)다.
 * @return: 0 성공, 음수면 창을 열지 못한 것.
 *
 * 왜 필요한가: pci_epf_mhi_iatu_read() 의 거울상이다. 엔드포인트가 만든 데이터
 * (수신 패킷, 이벤트 링 항목 등)를 호스트 RAM 에 올려놓아야 호스트가 볼 수 있다.
 * MHI EP 코어는 이벤트 링 갱신처럼 작고 잦은 쓰기에 write_sync 를 쓰는데,
 * DMA 가 없는 플랫폼에서는 그 자리가 이 함수다.
 *
 * 동작 단계: 읽기와 동일하다. (1) 오프셋 계산 (2) 락 (3) 창 열기
 * (4) memcpy_toio 로 밀어 넣기 (5) 창 닫기 + 락 해제 (6) 완료 콜백.
 * 유일한 차이는 4단계의 방향이 memcpy_fromio 가 아니라 memcpy_toio 라는 점이다.
 *
 * 실행 컨텍스트: 프로세스 문맥(잠들 수 있음). MHI 코어의 링/이벤트 처리 워커.
 * 에러 경로: 창 열기 실패 시 락을 풀고 오류 반환. 완료 콜백은 부르지 않는다.
 *
 * 호출 체인:
 *   MHI EP 코어(write_sync/write_async) → [pci_epf_mhi_iatu_write]
 *     → __pci_epf_mhi_alloc_map → memcpy_toio → __pci_epf_mhi_unmap_free
 *   pci_epf_mhi_edma_write (4KB 미만일 때) → [pci_epf_mhi_iatu_write]
 */
static int pci_epf_mhi_iatu_write(struct mhi_ep_cntrl *mhi_cntrl,
				  struct mhi_ep_buf_info *buf_info)
{
	/* [한국어] 드라이버 인스턴스 복원. */
	struct pci_epf_mhi *epf_mhi = to_epf_mhi(mhi_cntrl);
	/* [한국어] 목적지 호스트 주소의 정렬 잔여분. 읽기와 계산 방식이 같다. */
	size_t offset = get_align_offset(epf_mhi, buf_info->host_addr);
	/* [한국어] 창을 통해 호스트 메모리에 쓰기 위한 __iomem 가상 주소. */
	void __iomem *tre_buf;
	/* [한국어] 같은 지점의 로컬 물리 주소. 창을 닫을 때 쓴다. */
	phys_addr_t tre_phys;
	int ret;

	/* [한국어] 아웃바운드 창 자원을 독점한다. 읽기 경로와 같은 락을 공유하므로
	 * 읽기와 쓰기도 서로 직렬화된다. */
	mutex_lock(&epf_mhi->lock);

	/* [한국어] 목적지 호스트 구간을 로컬 주소 공간에 노출시킨다. 쓰기라고 해서
	 * 창을 여는 방식이 다르지는 않다 -- 창은 양방향이다. */
	ret = __pci_epf_mhi_alloc_map(mhi_cntrl, buf_info->host_addr, &tre_phys,
				      &tre_buf, offset, buf_info->size);
	/* [한국어] 창 자원 부족으로 실패한 경우. */
	if (ret) {
		/* [한국어] 락 해제 후 오류 전파. */
		mutex_unlock(&epf_mhi->lock);
		return ret;
	}

	/* [한국어] 엔드포인트 커널 버퍼(dev_addr)의 내용을 창(tre_buf)으로 밀어
	 * 넣는다. 이 쓰기가 PCIe 메모리 쓰기 TLP 로 나가 호스트 RAM 에 도달한다.
	 * memcpy_toio 는 인자 순서가 (목적지 __iomem, 원본 커널메모리) 라
	 * memcpy_fromio 와 반대라는 점에 유의해야 한다. */
	memcpy_toio(tre_buf, buf_info->dev_addr, buf_info->size);

	/* [한국어] 창과 주소 공간을 반납한다. 창을 닫는 동작 자체가 앞선 쓰기
	 * TLP 들이 밖으로 나가도록 보장하지는 않지만, MHI 는 이어서 도어벨/MSI 를
	 * 쓰므로 PCIe 순서 규칙에 따라 데이터가 먼저 도착한다. */
	__pci_epf_mhi_unmap_free(mhi_cntrl, buf_info->host_addr, tre_phys,
				 tre_buf, offset, buf_info->size);

	/* [한국어] 창 자원을 돌려준 뒤 락 해제. */
	mutex_unlock(&epf_mhi->lock);

	/* [한국어] write_async 자리에도 이 함수가 쓰일 수 있으므로 완료 콜백이
	 * 달려 올 수 있다. */
	if (buf_info->cb)
		/* [한국어] 읽기와 같은 이유로 락 밖에서 부른다(재진입 교착 방지). */
		buf_info->cb(buf_info);

	return 0;
}

/*
 * [한국어]
 * pci_epf_mhi_dma_callback - 동기 eDMA 전송의 완료 통지 콜백
 *
 * @param: dmaengine 이 desc->callback_param 에 담아 뒀던 값. 여기서는 호출
 *         함수의 스택에 있는 struct completion 의 주소다.
 * @return: 없음.
 *
 * 왜 필요한가: pci_epf_mhi_edma_read()/_write() 는 DMA 를 제출한 뒤
 * wait_for_completion_timeout() 으로 잠들어 기다린다. 그 잠을 깨워 줄 사람이
 * 필요한데, 그 역할이 이 콜백이다. complete() 한 번이 전부다.
 *
 * 동작: complete(param) 로 완료 카운트를 올리고 대기자를 깨운다.
 *
 * 실행 컨텍스트: dmaengine 드라이버(DesignWare eDMA)의 완료 처리 문맥 --
 * 보통 인터럽트나 tasklet 이다. 그래서 잠들 수 있는 일은 절대 하면 안 되고,
 * complete() 는 스핀락만 쓰므로 안전하다. 비동기 경로가 이 콜백 대신
 * pci_epf_mhi_dma_async_callback() 을 쓰는 이유도 같다 -- 거기서는 언매핑과
 * MHI 완료 콜백이 필요한데 그건 이 문맥에서 못 하므로 워크큐로 넘긴다.
 * 에러 경로: 없다. DMA 가 실패해도 dmaengine 은 콜백을 부르며, 대기 쪽은
 * 타임아웃이나 상태 조회로 판단해야 한다. 이 드라이버는 타임아웃만 본다.
 *
 * 호출 체인:
 *   dmaengine(eDMA 완료 인터럽트) → [pci_epf_mhi_dma_callback] → complete
 *   (반대편 대기자: pci_epf_mhi_edma_read / _edma_write)
 */
static void pci_epf_mhi_dma_callback(void *param)
{
	/* [한국어] 호출자 스택의 DECLARE_COMPLETION_ONSTACK(complete) 를 깨운다.
	 * param 을 struct completion * 로 캐스팅하지 않아도 되는 이유는
	 * complete() 의 인자 타입이 그 자체이고 void * 에서 암묵 변환되기 때문이다. */
	complete(param);
}

/*
 * [한국어]
 * pci_epf_mhi_edma_read - 호스트 메모리를 eDMA 로 읽어 온다(동기, 완료까지 대기)
 *
 * @mhi_cntrl: MHI EP 코어의 컨트롤러 포인터.
 * @buf_info: 전송 서술자. host_addr 이 원본(호스트 PCI 주소), dev_addr 이
 *            목적지(엔드포인트 커널 가상 주소), size 가 길이다.
 * @return: 0 성공. -EIO 는 서술자 준비 실패, -ETIMEDOUT 은 1초 안에 DMA 가
 *          끝나지 않은 경우, 그 밖의 음수는 채널 설정/매핑 실패다.
 *
 * 왜 필요한가: iATU + memcpy 경로는 CPU 가 PCIe 트랜잭션을 한 워드씩 끌어와야
 * 해서 큰 전송에서 매우 느리다. DesignWare eDMA 가 있는 플랫폼에서는 하드웨어가
 * 링크 너머로 직접 데이터를 옮길 수 있으므로 그쪽에 맡긴다. 다만 DMA 준비
 * 비용이 고정으로 들기 때문에 작은 전송은 이득이 없다 -- 그래서 4KB 경계에서
 * 갈라진다.
 *
 * 동작 단계:
 *  1) size < 4KB 면 아예 iATU 경로로 넘긴다(조기 반환).
 *  2) 락을 잡는다. dmaengine_slave_config() 가 채널의 원격 주소를 덮어쓰므로
 *     두 전송이 겹치면 서로의 주소를 망가뜨린다.
 *  3) 채널에 "원격(호스트) 주소는 host_addr, 방향은 DEV→MEM" 이라고 설정한다.
 *  4) 목적지 커널 버퍼를 dma_map_single 로 스트리밍 매핑해 로컬 DMA 주소를 얻는다.
 *  5) 서술자를 만들고 완료 콜백을 달아 제출한다.
 *  6) dma_async_issue_pending 으로 엔진을 돌리고 1초까지 완료를 기다린다.
 *  7) 매핑 해제 → 락 해제.
 *
 * 실행 컨텍스트: 프로세스 문맥. 뮤텍스와 wait_for_completion 때문에 반드시
 * 잠들 수 있는 문맥이어야 한다. MHI EP 코어의 워커에서 불린다.
 * 에러 경로: 두 갈래의 goto 사다리를 쓴다. 매핑 전에 실패하면 err_unlock 으로
 * 곧장 가고, 매핑 후에 실패하면 err_unmap 을 거쳐 매핑을 풀고 나서 락을 푼다.
 * 타임아웃 시에는 dmaengine_terminate_sync() 로 진행 중인 전송을 확실히
 * 멈춘 뒤에야 매핑을 해제한다 -- 안 그러면 DMA 가 이미 해제된 버퍼에 쓴다.
 *
 * 호출 체인:
 *   MHI EP 코어(mhi_cntrl->read_sync) → [pci_epf_mhi_edma_read]
 *     → dmaengine_slave_config → dma_map_single
 *     → dmaengine_prep_slave_single → dmaengine_submit
 *     → dma_async_issue_pending → wait_for_completion_timeout
 *   (4KB 미만이면 → pci_epf_mhi_iatu_read)
 */
static int pci_epf_mhi_edma_read(struct mhi_ep_cntrl *mhi_cntrl,
				 struct mhi_ep_buf_info *buf_info)
{
	/* [한국어] 드라이버 인스턴스 복원. */
	struct pci_epf_mhi *epf_mhi = to_epf_mhi(mhi_cntrl);
	/* [한국어] DMA 매핑의 기준이 되는 장치. EPF 장치나 EPC 장치가 아니라
	 * "EPC 의 부모", 즉 실제 PCIe 컨트롤러 platform_device 여야 한다.
	 * DMA 마스크와 IOMMU 도메인이 그 장치에 붙어 있기 때문이다. */
	struct device *dma_dev = epf_mhi->epf->epc->dev.parent;
	/* [한국어] 읽기 방향 전용 채널. 쓰기 채널과 물리적으로 다른 엔진이라
	 * 읽기와 쓰기가 하드웨어에서는 병렬로 돌 수 있다(다만 이 락 때문에
	 * 소프트웨어에서는 직렬화된다). */
	struct dma_chan *chan = epf_mhi->dma_chan_rx;
	/* [한국어] dev_err 로그의 주체가 될 EPF 장치. sysfs 상 이름이 찍힌다. */
	struct device *dev = &epf_mhi->epf->dev;
	/* [한국어] 스택 위에 완료 객체를 만든다. DMA 콜백이 이걸 깨워 준다.
	 * 스택에 두어도 안전한 이유는 이 함수가 완료(또는 타임아웃 후
	 * terminate_sync)를 확인하기 전에는 반환하지 않기 때문이다. */
	DECLARE_COMPLETION_ONSTACK(complete);
	/* [한국어] dmaengine 이 만들어 줄 전송 서술자. 콜백을 달고 제출하는 대상이다. */
	struct dma_async_tx_descriptor *desc;
	/* [한국어] 채널의 슬레이브 설정. {} 로 전부 0 초기화한 뒤 필요한 필드만 채운다.
	 * 0 초기화가 중요한 이유는 addr_width/maxburst 같은 필드를 0 으로 남겨
	 * eDMA 드라이버가 기본값을 쓰게 하기 위해서다. */
	struct dma_slave_config config = {};
	/* [한국어] dmaengine_submit 이 돌려주는 전송 식별 쿠키. 오류 판정에만 쓴다. */
	dma_cookie_t cookie;
	/* [한국어] 목적지 커널 버퍼의 DMA(버스) 주소. dma_map_single 결과. */
	dma_addr_t dst_addr;
	int ret;

	/* [한국어] 4KB 미만이면 DMA 준비 비용(설정, 매핑, 서술자, 인터럽트)이
	 * 실제 전송보다 커진다. 이 크기에서는 CPU 복사가 더 빠르므로 iATU 경로로
	 * 위임하고 여기서 곧장 빠져나간다. 락도 아직 잡기 전이라 안전하다. */
	if (buf_info->size < SZ_4K)
		return pci_epf_mhi_iatu_read(mhi_cntrl, buf_info);

	/* [한국어] 채널 설정이 채널 전역 상태라서, 설정부터 완료까지를 하나의
	 * 임계구역으로 묶어야 한다. 그렇지 않으면 다른 전송이 config.src_addr 을
	 * 갈아 끼워 엉뚱한 호스트 주소에서 읽어 온다. */
	mutex_lock(&epf_mhi->lock);

	/* [한국어] 방향을 "장치(=원격 PCIe 쪽) → 메모리(로컬)" 로 지정한다.
	 * dmaengine 에서 이 direction 필드는 사용 중단 예정이지만 dw-edma 는
	 * 여전히 이 값과 prep 단계의 방향을 대조해 채널 방향이 맞는지 검사한다. */
	config.direction = DMA_DEV_TO_MEM;
	/* [한국어] "장치 쪽" 주소로 호스트 PCI 주소를 넣는다. dw-edma 는
	 * DMA_DEV_TO_MEM 일 때 이 src_addr 을 PCI 주소로 해석해 전송 서술자의
	 * SAR(Source Address Register)에 넣으므로, 링크 너머 호스트 RAM 이
	 * 원본이 된다. 로컬 버퍼 주소는 아래 prep 단계에서 따로 준다. */
	config.src_addr = buf_info->host_addr;

	/* [한국어] 설정을 채널에 반영한다. eDMA 드라이버는 이 시점에 값을
	 * 자기 채널 구조체에 저장만 하고, 실제 레지스터 기록은 서술자 준비
	 * 단계에서 한다. */
	ret = dmaengine_slave_config(chan, &config);
	/* [한국어] 채널 방향이 설정과 어긋나거나 드라이버가 거부한 경우. */
	if (ret) {
		dev_err(dev, "Failed to configure DMA channel\n");
		/* [한국어] 아직 아무것도 매핑하지 않았으므로 락만 풀면 된다. */
		goto err_unlock;
	}

	/* [한국어] 목적지 커널 버퍼를 DMA 가 접근할 수 있게 스트리밍 매핑한다.
	 * DMA_FROM_DEVICE 는 "장치가 이 버퍼에 쓸 것" 이라는 뜻이라, 매핑 시점에
	 * 해당 영역의 캐시 라인을 무효화해 두었다가 언매핑 때 CPU 가 새 데이터를
	 * 보게 만든다. buf_info->dev_addr 은 MHI 코어가 kmem_cache 에서 잡아 준
	 * 일반 커널 메모리라 이 매핑이 유효하다. */
	dst_addr = dma_map_single(dma_dev, buf_info->dev_addr, buf_info->size,
				  DMA_FROM_DEVICE);
	/* [한국어] IOMMU 공간 부족이나 DMA 마스크 초과로 매핑이 실패했는지 본다.
	 * dma_addr_t 는 오류를 특별한 값으로 표현하므로 반드시 이 헬퍼로 판정해야 한다. */
	ret = dma_mapping_error(dma_dev, dst_addr);
	/* [한국어] 매핑 실패 시. 로그 문구가 "remote memory" 이지만 실제로 매핑한
	 * 것은 로컬 버퍼다 -- 상류 코드의 문구를 그대로 두었다. */
	if (ret) {
		dev_err(dev, "Failed to map remote memory\n");
		/* [한국어] 매핑이 안 됐으니 풀 것도 없다. 바로 락 해제로 간다. */
		goto err_unlock;
	}

	/* [한국어] 단일 버퍼 전송 서술자를 만든다. dst_addr 은 로컬(메모리) 쪽
	 * 주소이고, 원격(호스트) 쪽 주소는 앞서 slave_config 로 이미 알려 줬다.
	 * DMA_CTRL_ACK 는 "이 서술자를 재사용하지 않으니 완료 후 바로 회수해도
	 * 된다" 는 표시이고, DMA_PREP_INTERRUPT 는 완료 시 인터럽트를 올려
	 * 콜백을 부르라는 요청이다. 후자가 없으면 complete 가 영영 오지 않는다. */
	desc = dmaengine_prep_slave_single(chan, dst_addr, buf_info->size,
					   DMA_DEV_TO_MEM,
					   DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	/* [한국어] 서술자 풀이 비었거나 인자 조합이 하드웨어 제약에 맞지 않으면
	 * NULL 이 온다. dmaengine 은 이 경우 오류 코드를 주지 않는다. */
	if (!desc) {
		dev_err(dev, "Failed to prepare DMA\n");
		/* [한국어] 마땅한 오류 코드가 없어 -EIO 로 통일한다. */
		ret = -EIO;
		/* [한국어] 이미 매핑을 했으므로 반드시 언매핑을 거쳐 나가야 한다. */
		goto err_unmap;
	}

	/* [한국어] 완료 시 불릴 함수를 단다. 여기서는 completion 만 깨우는
	 * 가벼운 콜백이라 인터럽트 문맥에서 안전하다. */
	desc->callback = pci_epf_mhi_dma_callback;
	/* [한국어] 콜백에 넘길 인자로 스택 위 completion 의 주소를 준다. */
	desc->callback_param = &complete;

	/* [한국어] 서술자를 채널의 대기열에 넣는다. 아직 엔진이 돌지는 않는다. */
	cookie = dmaengine_submit(desc);
	/* [한국어] 쿠키가 오류 값인지 판정한다. dma_cookie_t 는 음수 값으로
	 * 오류를 표현하므로 직접 비교하지 않고 이 헬퍼를 쓴다. */
	ret = dma_submit_error(cookie);
	/* [한국어] 제출 자체가 거부된 경우(채널이 이미 종료 중이라거나). */
	if (ret) {
		dev_err(dev, "Failed to do DMA submit\n");
		/* [한국어] 매핑을 풀고 나가야 한다. 서술자는 dmaengine 이 회수한다. */
		goto err_unmap;
	}

	/* [한국어] 대기열에 쌓인 서술자를 실제로 하드웨어에 밀어 넣어 전송을
	 * 시작시킨다. 이 호출이 없으면 서술자가 큐에 남아 영영 돌지 않는다. */
	dma_async_issue_pending(chan);
	/* [한국어] 완료 콜백이 complete() 를 부를 때까지 최대 1초 잠든다.
	 * 반환값은 "남은 지피(jiffy) 수" 라서 0 이면 타임아웃, 양수면 성공이다.
	 * 1초는 상류가 고른 고정 상수로, MHI 전송이 그보다 오래 걸리면
	 * 링크나 호스트에 문제가 있다고 본다. */
	ret = wait_for_completion_timeout(&complete, msecs_to_jiffies(1000));
	/* [한국어] 0 이면 시간 안에 완료가 오지 않았다는 뜻이다. */
	if (!ret) {
		dev_err(dev, "DMA transfer timeout\n");
		/* [한국어] 반드시 전송을 확실히 멈춘 뒤 아래에서 언매핑해야 한다.
		 * terminate_sync 는 진행 중인 전송을 중단하고 콜백이 더 이상 불리지
		 * 않음을 보장할 때까지 기다린다. 이 호출을 빼면 이미 해제된 매핑에
		 * DMA 가 계속 써서 메모리를 오염시킨다. 또한 스택 위의 completion 이
		 * 사라진 뒤 콜백이 불려 스택 손상이 날 수도 있다. */
		dmaengine_terminate_sync(chan);
		/* [한국어] wait_for_completion_timeout 의 0 을 호출자용 오류로 바꾼다. */
		ret = -ETIMEDOUT;
	} else {
		/* [한국어] 남은 지피 수(양수)는 호출자에게 의미가 없으므로 0(성공)으로
		 * 덮어쓴다. 이 줄이 없으면 성공인데도 반환값이 0 이 아니게 된다. */
		ret = 0;
	}

err_unmap:
	/* [한국어] 스트리밍 매핑 해제. DMA_FROM_DEVICE 방향으로 풀어야 장치가 쓴
	 * 내용이 CPU 캐시 관점에서 보이게 된다(아키텍처에 따라 캐시 무효화 수행).
	 * 매핑에 성공한 모든 경로가 이 라벨을 지나간다. */
	dma_unmap_single(dma_dev, dst_addr, buf_info->size, DMA_FROM_DEVICE);
err_unlock:
	/* [한국어] 채널 임계구역 해제. 여기까지 오는 경로는 (a) 정상 완료,
	 * (b) 타임아웃, (c) 각종 실패 세 가지다. */
	mutex_unlock(&epf_mhi->lock);

	/* [한국어] 이 함수는 read_sync 자리에만 쓰이고 완료를 스스로 기다리므로
	 * buf_info->cb 를 부르지 않는다. 동기 호출자는 반환값만 보면 된다. */
	return ret;
}

/*
 * [한국어]
 * pci_epf_mhi_edma_write - 엔드포인트 버퍼를 eDMA 로 호스트에 쓴다(동기)
 *
 * @mhi_cntrl: MHI EP 코어의 컨트롤러 포인터.
 * @buf_info: 전송 서술자. dev_addr 이 원본(엔드포인트 커널 버퍼), host_addr 이
 *            목적지(호스트 PCI 주소)다.
 * @return: 0 성공, -EIO/-ETIMEDOUT/그 밖의 음수는 read 쪽과 같은 의미다.
 *
 * 왜 필요한가: pci_epf_mhi_edma_read() 의 거울상이다. 엔드포인트가 만든 큰
 * 데이터 덩어리(수신 패킷 등)를 호스트 RAM 으로 밀어 올릴 때, CPU 가 워드 단위로
 * 밀어 넣는 대신 eDMA 엔진에 맡긴다.
 *
 * 동작 단계: read 와 같되 방향이 전부 반대다.
 *  - 채널은 dma_chan_tx (MEM→DEV 전용 엔진)
 *  - config.dst_addr 에 호스트 주소를 넣는다(원격이 목적지이므로)
 *  - dma_map_single 방향은 DMA_TO_DEVICE (장치가 이 버퍼를 읽어 갈 것)
 *  - prep 단계 방향은 DMA_MEM_TO_DEV
 *
 * 실행 컨텍스트: 프로세스 문맥(뮤텍스 + 완료 대기). MHI EP 코어의 워커.
 * 에러 경로: read 와 동일한 두 단 goto 사다리(err_unmap → err_unlock).
 * 타임아웃 시 terminate_sync 로 전송을 확실히 세운 뒤 언매핑한다.
 *
 * 호출 체인:
 *   MHI EP 코어(mhi_cntrl->write_sync) → [pci_epf_mhi_edma_write]
 *     → dmaengine_slave_config → dma_map_single
 *     → dmaengine_prep_slave_single → dmaengine_submit
 *     → dma_async_issue_pending → wait_for_completion_timeout
 *   (4KB 미만이면 → pci_epf_mhi_iatu_write)
 */
static int pci_epf_mhi_edma_write(struct mhi_ep_cntrl *mhi_cntrl,
				  struct mhi_ep_buf_info *buf_info)
{
	/* [한국어] 드라이버 인스턴스 복원. */
	struct pci_epf_mhi *epf_mhi = to_epf_mhi(mhi_cntrl);
	/* [한국어] DMA 매핑 기준 장치 -- EPC 의 부모인 PCIe 컨트롤러 platform_device. */
	struct device *dma_dev = epf_mhi->epf->epc->dev.parent;
	/* [한국어] 쓰기 방향 전용 eDMA 채널. 읽기 채널과 물리적으로 분리돼 있다. */
	struct dma_chan *chan = epf_mhi->dma_chan_tx;
	/* [한국어] 오류 로그의 주체가 될 EPF 장치. */
	struct device *dev = &epf_mhi->epf->dev;
	/* [한국어] 스택 위 완료 객체. DMA 콜백이 깨워 준다. */
	DECLARE_COMPLETION_ONSTACK(complete);
	/* [한국어] dmaengine 전송 서술자. */
	struct dma_async_tx_descriptor *desc;
	/* [한국어] 채널 슬레이브 설정. 0 초기화 후 방향/원격 주소만 채운다. */
	struct dma_slave_config config = {};
	/* [한국어] 제출 결과 쿠키. 오류 판정용. */
	dma_cookie_t cookie;
	/* [한국어] 원본 커널 버퍼의 DMA 주소. 읽기 경로의 dst_addr 에 대응한다. */
	dma_addr_t src_addr;
	int ret;

	/* [한국어] 4KB 미만은 DMA 준비 비용이 더 크므로 iATU + memcpy_toio 경로로
	 * 위임한다. MHI 의 이벤트 링 갱신처럼 수십 바이트짜리 쓰기가 이 경로로 간다. */
	if (buf_info->size < SZ_4K)
		return pci_epf_mhi_iatu_write(mhi_cntrl, buf_info);

	/* [한국어] 채널 설정~완료까지를 임계구역으로 묶는다. 읽기 경로와 같은
	 * 락을 쓰므로 읽기/쓰기가 서로도 직렬화된다(채널은 다르지만 락은 하나). */
	mutex_lock(&epf_mhi->lock);

	/* [한국어] 방향은 "메모리(로컬) → 장치(원격 PCIe)". dw-edma 는 이 값과
	 * 채널의 하드웨어 방향이 맞는지 prep 단계에서 대조한다. */
	config.direction = DMA_MEM_TO_DEV;
	/* [한국어] 이번에는 원격이 목적지이므로 dst_addr 에 호스트 PCI 주소를 넣는다.
	 * dw-edma 는 이 값을 전송 서술자의 DAR(Destination Address Register)에
	 * 넣어 링크 너머 호스트 RAM 을 목적지로 삼는다. */
	config.dst_addr = buf_info->host_addr;

	/* [한국어] 설정을 채널에 반영한다. */
	ret = dmaengine_slave_config(chan, &config);
	/* [한국어] 채널 방향 불일치 등으로 거부된 경우. */
	if (ret) {
		dev_err(dev, "Failed to configure DMA channel\n");
		/* [한국어] 아직 매핑 전이라 락만 풀면 된다. */
		goto err_unlock;
	}

	/* [한국어] 원본 커널 버퍼를 DMA 가 읽을 수 있게 매핑한다. DMA_TO_DEVICE 는
	 * "장치가 이 버퍼를 읽을 것" 이라는 뜻이라, 매핑 시점에 CPU 가 쓴 내용을
	 * 캐시에서 메모리로 내려보낸다(플러시). 이 방향을 반대로 쓰면 장치가
	 * 오래된 데이터를 읽어 간다. */
	src_addr = dma_map_single(dma_dev, buf_info->dev_addr, buf_info->size,
				  DMA_TO_DEVICE);
	/* [한국어] 매핑 실패 여부 판정. */
	ret = dma_mapping_error(dma_dev, src_addr);
	/* [한국어] 실패 시. 로그 문구는 상류 그대로 두었다(실제로는 로컬 버퍼 매핑). */
	if (ret) {
		dev_err(dev, "Failed to map remote memory\n");
		/* [한국어] 풀 매핑이 없으므로 락 해제로 직행. */
		goto err_unlock;
	}

	/* [한국어] 단일 버퍼 서술자를 만든다. src_addr 은 로컬(메모리) 쪽이고,
	 * 목적지 호스트 주소는 slave_config 로 이미 알려 줬다.
	 * DMA_CTRL_ACK = 서술자 재사용 없음, DMA_PREP_INTERRUPT = 완료 인터럽트 요청. */
	desc = dmaengine_prep_slave_single(chan, src_addr, buf_info->size,
					   DMA_MEM_TO_DEV,
					   DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	/* [한국어] 서술자 풀 고갈이나 제약 위반이면 NULL. */
	if (!desc) {
		dev_err(dev, "Failed to prepare DMA\n");
		/* [한국어] dmaengine 이 오류 코드를 주지 않으므로 -EIO 로 통일. */
		ret = -EIO;
		/* [한국어] 매핑을 이미 했으니 언매핑 경로로 나간다. */
		goto err_unmap;
	}

	/* [한국어] 완료 시 completion 을 깨울 콜백을 단다. */
	desc->callback = pci_epf_mhi_dma_callback;
	/* [한국어] 콜백 인자는 스택 위 completion 의 주소. */
	desc->callback_param = &complete;

	/* [한국어] 서술자를 채널 대기열에 넣는다. */
	cookie = dmaengine_submit(desc);
	/* [한국어] 제출 실패를 쿠키로 판정한다. */
	ret = dma_submit_error(cookie);
	/* [한국어] 제출이 거부된 경우. */
	if (ret) {
		dev_err(dev, "Failed to do DMA submit\n");
		/* [한국어] 매핑을 풀고 나간다. */
		goto err_unmap;
	}

	/* [한국어] 대기열을 하드웨어로 밀어 넣어 전송을 시작한다. */
	dma_async_issue_pending(chan);
	/* [한국어] 최대 1초 동안 완료를 기다린다. 반환값은 남은 지피 수. */
	ret = wait_for_completion_timeout(&complete, msecs_to_jiffies(1000));
	/* [한국어] 0 이면 타임아웃. */
	if (!ret) {
		dev_err(dev, "DMA transfer timeout\n");
		/* [한국어] 아래 언매핑 전에 반드시 전송을 확실히 중단시킨다.
		 * 그러지 않으면 이미 풀린 매핑을 통해 DMA 가 계속 읽어 가고,
		 * 스택 위 completion 이 사라진 뒤 콜백이 불릴 수도 있다. */
		dmaengine_terminate_sync(chan);
		ret = -ETIMEDOUT;
	} else {
		/* [한국어] 남은 지피 수를 성공(0)으로 정규화한다. */
		ret = 0;
	}

err_unmap:
	/* [한국어] 매핑 해제. DMA_TO_DEVICE 방향으로 풀어 매핑 시의 캐시 처리와
	 * 짝을 맞춘다. 매핑에 성공한 모든 경로가 이곳을 지난다. */
	dma_unmap_single(dma_dev, src_addr, buf_info->size, DMA_TO_DEVICE);
err_unlock:
	/* [한국어] 채널 임계구역 해제. */
	mutex_unlock(&epf_mhi->lock);

	/* [한국어] 동기 경로라 buf_info->cb 는 부르지 않는다. 호출자가 반환값으로 판단한다. */
	return ret;
}

/*
 * [한국어]
 * pci_epf_mhi_dma_worker - 완료된 비동기 DMA 전송들의 뒷정리를 프로세스 문맥에서 수행
 *
 * @work: 워크큐가 넘겨 준 work_struct. container_of 로 epf_mhi 를 복원한다.
 * @return: 없음.
 *
 * 왜 필요한가: 비동기 DMA 의 완료 콜백은 인터럽트/tasklet 문맥에서 불릴 수
 * 있는데, 거기서 해야 할 일 두 가지 -- dma_unmap_single() 과 MHI 코어의 완료
 * 콜백 buf_info->cb() -- 는 잠들 수 있는 작업이다(특히 cb 는 MHI 코어 안으로
 * 들어가 이벤트 링을 갱신하고 뮤텍스를 잡는다). 그래서 완료 콜백은 리스트에
 * 걸어 두기만 하고, 실제 뒷정리는 이 워커가 프로세스 문맥에서 처리한다.
 *
 * 동작 단계:
 *  1) 스핀락 아래에서 epf_mhi->dma_list 를 통째로 로컬 리스트 head 로 옮긴다
 *     (list_splice_tail_init). 이렇게 하면 순회하는 동안 락을 잡고 있을
 *     필요가 없어, 새 완료가 들어와도 막히지 않는다.
 *  2) 로컬 리스트를 순회하며 항목마다 언매핑 → MHI 완료 콜백 → kfree 한다.
 *
 * 실행 컨텍스트: epf_mhi->dma_wq 워크큐의 프로세스 문맥. 잠들 수 있다.
 * 재진입: 같은 work_struct 는 워크큐가 동시에 두 번 실행하지 않으므로 이
 * 함수의 본문은 한 번에 하나만 돈다.
 * 에러 경로: 없다. buf_info->cb 는 여기서 NULL 검사 없이 부르는데, 이 리스트에
 * 올라오는 항목은 전부 _read_async()/_write_async() 가 만든 것이고 MHI 코어가
 * 비동기 전송에는 항상 cb 를 채워 주기 때문이다(이 트리 밖의 코어 코드에
 * 의존하는 전제이므로, cb 가 NULL 인 비동기 전송이 오면 널 역참조가 된다).
 *
 * 호출 체인:
 *   pci_epf_mhi_dma_async_callback → queue_work → 워크큐 → [pci_epf_mhi_dma_worker]
 *     → dma_unmap_single → buf_info->cb (MHI 코어의 완료 처리) → kfree
 */
static void pci_epf_mhi_dma_worker(struct work_struct *work)
{
	/* [한국어] 워크큐는 work_struct 만 넘겨 주므로, 그것을 품고 있는
	 * 드라이버 인스턴스를 container_of 로 되찾는다. */
	struct pci_epf_mhi *epf_mhi = container_of(work, struct pci_epf_mhi, dma_work);
	/* [한국어] 언매핑의 기준 장치. 매핑할 때 쓴 것과 반드시 같아야 한다
	 * (EPC 의 부모 = 실제 PCIe 컨트롤러 platform_device). */
	struct device *dma_dev = epf_mhi->epf->epc->dev.parent;
	/* [한국어] 순회용 커서와 임시 저장. 순회 중 itr 을 kfree 하므로 다음
	 * 항목을 미리 보관하는 _safe 판 매크로가 필요하고, 그 보관처가 tmp 다. */
	struct pci_epf_mhi_dma_transfer *itr, *tmp;
	/* [한국어] MHI 완료 콜백에 넘길 서술자 포인터를 담을 지역 변수. */
	struct mhi_ep_buf_info *buf_info;
	/* [한국어] spin_lock_irqsave 가 저장할 인터럽트 상태. 워커는 프로세스
	 * 문맥이지만 리스트를 인터럽트 문맥과 공유하므로 irqsave 판을 써야 한다. */
	unsigned long flags;
	/* [한국어] 스택 위에 만든 빈 리스트 머리. 공유 리스트를 여기로 통째
	 * 옮겨 와 락 밖에서 느긋하게 순회하기 위한 장치다. */
	LIST_HEAD(head);

	/* [한국어] 공유 리스트를 만지기 전에 인터럽트를 끄고 스핀락을 잡는다.
	 * DMA 완료 콜백이 인터럽트 문맥에서 같은 리스트에 붙일 수 있으므로,
	 * 단순 spin_lock 이면 이 CPU 에서 인터럽트가 끼어들어 자기 자신과
	 * 교착(deadlock)할 수 있다. */
	spin_lock_irqsave(&epf_mhi->list_lock, flags);
	/* [한국어] 공유 리스트의 항목 전부를 로컬 head 로 옮기고 원본을 비운다.
	 * 개별 항목을 하나씩 떼어 내며 락을 반복해 잡는 것보다 훨씬 싸고,
	 * 순회 중에 새 완료가 들어와도 원본 리스트에 그냥 쌓이면 되므로
	 * 완료 콜백이 이 워커를 기다리지 않는다. */
	list_splice_tail_init(&epf_mhi->dma_list, &head);
	/* [한국어] 공유 자료구조 조작이 끝났으니 즉시 락을 놓고 인터럽트를 복구한다.
	 * 아래 순회는 로컬 리스트만 건드리므로 락이 필요 없다. */
	spin_unlock_irqrestore(&epf_mhi->list_lock, flags);

	/* [한국어] 떼어 온 항목들을 순서대로 처리한다. 본문에서 itr 을 kfree 하므로
	 * 반드시 _safe 판을 써야 한다(다음 포인터를 미리 tmp 에 담아 둔다). */
	list_for_each_entry_safe(itr, tmp, &head, node) {
		/* [한국어] 로컬 리스트에서 이 항목을 뗀다. 곧 kfree 할 메모리가
		 * 리스트에 매달린 채로 남지 않게 하려는 위생 조치다. */
		list_del(&itr->node);
		/* [한국어] 전송 때 잡아 둔 스트리밍 매핑을 푼다. 방향(itr->dir)과
		 * 크기(itr->size)가 매핑 때와 같아야 캐시 처리가 올바로 된다.
		 * 이 시점은 DMA 가 이미 끝난 뒤이므로 안전하다 -- 완료 콜백이
		 * 불린 뒤에야 이 항목이 리스트에 올라왔기 때문이다. */
		dma_unmap_single(dma_dev, itr->paddr, itr->size, itr->dir);
		/* [한국어] 복사해 둔 전송 서술자의 주소를 잡는다. 원본은 MHI 코어의
		 * 스택에 있었으므로 이미 사라졌고, 여기 있는 복사본만 유효하다. */
		buf_info = &itr->buf_info;
		/* [한국어] MHI 코어의 완료 처리(mhi_ep_read_completion 등)를 부른다.
		 * 이 안에서 코어가 이벤트 링에 완료 항목을 넣고 raise_irq 로 호스트에
		 * 통지하며, 읽기 경로라면 받은 버퍼를 클라이언트 드라이버에 넘긴다.
		 * 락을 하나도 잡지 않은 상태에서 부르므로 콜백이 이 드라이버로 다시
		 * 들어와도(예: raise_irq) 교착하지 않는다. */
		buf_info->cb(buf_info);
		/* [한국어] 배달 봉투를 해제한다. _read_async()/_write_async() 의
		 * kzalloc_obj 와 짝이 되는 유일한 해제 지점이다. */
		kfree(itr);
	}
}

/*
 * [한국어]
 * pci_epf_mhi_dma_async_callback - 비동기 eDMA 완료 콜백. 뒷정리를 워크큐로 넘긴다.
 *
 * @param: desc->callback_param 으로 달아 둔 struct pci_epf_mhi_dma_transfer 포인터.
 * @return: 없음.
 *
 * 왜 필요한가: 동기 경로의 pci_epf_mhi_dma_callback() 은 completion 하나만
 * 깨우면 끝이지만, 비동기 경로에는 기다리는 사람이 없다. 대신 언매핑과 MHI
 * 완료 콜백을 누군가는 해 줘야 하는데, 이 콜백이 도는 문맥(인터럽트/tasklet)
 * 에서는 그걸 할 수 없다. 그래서 "할 일 목록" 에 올려 두고 워커를 깨우는
 * 두 줄짜리 중계만 한다.
 *
 * 동작 단계:
 *  1) 스핀락 아래에서 이 전송을 dma_list 꼬리에 붙인다.
 *  2) 워크큐에 dma_work 를 넣어 워커를 깨운다. 이미 큐에 들어 있으면
 *     queue_work 가 false 를 돌려주고 아무 일도 하지 않는데, 그래도 문제가
 *     없다 -- 이미 예약된 실행이 리스트를 통째로 훑을 때 이 항목도 함께
 *     처리되기 때문이다.
 *
 * 실행 컨텍스트: dmaengine 의 완료 처리 문맥(보통 인터럽트나 tasklet).
 * 그래서 spin_lock_irqsave 가 아니라 spin_lock 을 쓴다 -- 이미 인터럽트가
 * 꺼져 있거나 소프트IRQ 문맥이라는 전제다. 반대편(워커)은 프로세스 문맥이라
 * spin_lock_irqsave 를 쓰므로, 이 비대칭이 락 규약의 핵심이다.
 * 에러 경로: 없다. DMA 가 실패로 끝나도 이 콜백은 불리고, 실패 여부를
 * 확인하는 코드는 없다 -- 비동기 경로는 오류를 조용히 성공으로 보고하게 된다.
 *
 * 호출 체인:
 *   dmaengine(eDMA 완료 인터럽트) → [pci_epf_mhi_dma_async_callback]
 *     → list_add_tail → queue_work → pci_epf_mhi_dma_worker
 */
static void pci_epf_mhi_dma_async_callback(void *param)
{
	/* [한국어] 콜백 인자를 전송 봉투로 되돌린다. _read_async()/_write_async()
	 * 가 desc->callback_param 에 이 포인터를 심어 두었다. */
	struct pci_epf_mhi_dma_transfer *transfer = param;
	/* [한국어] 봉투 안에 넣어 둔 드라이버 인스턴스 포인터. 리스트와 워크큐에
	 * 접근하려면 필요한데, 콜백 인자로는 봉투 하나만 오므로 이 경로가 유일하다. */
	struct pci_epf_mhi *epf_mhi = transfer->epf_mhi;

	/* [한국어] 공유 리스트를 보호한다. 여기는 인터럽트/soft-IRQ 문맥이라
	 * 잠들 수 없으므로 뮤텍스가 아니라 스핀락을 쓴다. irqsave 를 쓰지 않는
	 * 이유는 이 문맥이 이미 인터럽트가 꺼진 상태이거나 하위 문맥이라
	 * 프로세스 문맥이 끼어들 수 없기 때문이다. */
	spin_lock(&epf_mhi->list_lock);
	/* [한국어] 완료된 전송을 목록 꼬리에 붙인다. 꼬리에 붙이므로 워커가
	 * 앞에서부터 순회할 때 완료 순서대로 처리된다 -- MHI 는 채널별로 순서를
	 * 지켜야 하므로 이 FIFO 성질이 중요하다. */
	list_add_tail(&transfer->node, &epf_mhi->dma_list);
	/* [한국어] 리스트 조작 끝. 즉시 놓아 다른 완료 콜백을 막지 않는다. */
	spin_unlock(&epf_mhi->list_lock);

	/* [한국어] 워커를 예약한다. 같은 work_struct 가 이미 큐에 있으면 이
	 * 호출은 아무 일도 하지 않지만, 그 예약된 실행이 리스트 전체를 훑으므로
	 * 방금 넣은 항목도 함께 처리된다. 즉 항목 수와 큐잉 횟수가 달라도 된다. */
	queue_work(epf_mhi->dma_wq, &epf_mhi->dma_work);
}

/*
 * [한국어]
 * pci_epf_mhi_edma_read_async - 호스트 메모리 읽기를 eDMA 에 던져 놓고 즉시 반환
 *
 * @mhi_cntrl: MHI EP 코어의 컨트롤러 포인터.
 * @buf_info: 전송 서술자. host_addr(원본), dev_addr(목적지), size, 그리고
 *            반드시 cb(완료 콜백)가 채워져 있어야 한다 -- 워커가 NULL 검사
 *            없이 부르기 때문이다.
 * @return: 0 이면 "제출 성공"이지 "전송 완료"가 아니다. 실제 완료는 나중에
 *          워커가 buf_info->cb 로 알린다. 음수면 제출 자체가 실패한 것으로,
 *          이 경우 cb 는 불리지 않는다.
 *
 * 왜 필요한가: MHI 의 채널 데이터 경로(mhi_ep_read_channel)는 TRE 를 연속으로
 * 처리하기 위해 전송마다 완료를 기다리지 않는다. 동기 판을 쓰면 링 처리
 * 워커가 매 전송마다 1초까지 잠들 수 있어 처리량이 무너진다. 그래서 코어는
 * read_async 를 부르고, 이 함수는 제출만 하고 곧장 돌아온다.
 *
 * 동작 단계: 동기 판(pci_epf_mhi_edma_read)과 거의 같지만 세 가지가 다르다.
 *  (a) 4KB 미만 우회 분기가 없다 -- 비동기 경로에는 대응하는 비동기 iATU
 *      구현이 없으므로 크기와 무관하게 DMA 로 보낸다.
 *  (b) 서술자를 제출하기 전에 완료 정보 봉투(struct pci_epf_mhi_dma_transfer)를
 *      할당해 콜백 인자로 단다.
 *  (c) 완료를 기다리지 않고, 성공 시 goto err_unlock 으로 락만 풀고 나간다.
 *      이때 매핑은 일부러 풀지 않는다 -- DMA 가 아직 그 버퍼를 쓰고 있고,
 *      언매핑은 워커의 몫이다.
 *
 * 실행 컨텍스트: 프로세스 문맥(뮤텍스를 잡고 kzalloc 을 한다). MHI EP 코어의
 * 채널 링 처리 워커에서 불린다.
 * 에러 경로: 세 단 goto 사다리다. 매핑 전 실패는 err_unlock,
 * 매핑 후 서술자/할당 실패는 err_unmap(매핑을 푼다), 제출 실패는
 * err_free_transfer(봉투를 해제하고 이어서 매핑도 푼다). 성공 경로만
 * 매핑을 남겨 둔 채 빠져나간다는 점이 이 함수 읽기의 핵심이다.
 *
 * 호출 체인:
 *   MHI EP 코어(mhi_cntrl->read_async, 예: mhi_ep_read_channel)
 *     → [pci_epf_mhi_edma_read_async] → dmaengine_submit
 *   (완료 후) dmaengine → pci_epf_mhi_dma_async_callback
 *     → pci_epf_mhi_dma_worker → buf_info->cb
 */
static int pci_epf_mhi_edma_read_async(struct mhi_ep_cntrl *mhi_cntrl,
				       struct mhi_ep_buf_info *buf_info)
{
	/* [한국어] 드라이버 인스턴스 복원. */
	struct pci_epf_mhi *epf_mhi = to_epf_mhi(mhi_cntrl);
	/* [한국어] DMA 매핑 기준 장치(EPC 의 부모 platform_device). */
	struct device *dma_dev = epf_mhi->epf->epc->dev.parent;
	/* [한국어] 완료 시 워커에게 넘길 정보 봉투. NULL 로 초기화해 두어야
	 * err_unmap 경로에서 아직 할당 전인 상태를 구분할 수 있다(다만 이
	 * 구현에서는 err_free_transfer 를 통해서만 kfree 하므로 실제로 NULL
	 * kfree 가 일어나지는 않는다). */
	struct pci_epf_mhi_dma_transfer *transfer = NULL;
	/* [한국어] 읽기 방향 eDMA 채널. */
	struct dma_chan *chan = epf_mhi->dma_chan_rx;
	/* [한국어] 오류 로그의 주체. */
	struct device *dev = &epf_mhi->epf->dev;
	/* [한국어] 상류 코드가 남겨 둔 미사용 변수. 비동기 경로는 완료를 기다리지
	 * 않으므로 이 completion 은 초기화만 되고 아무도 쓰지 않는다
	 * (동기 판에서 복사해 오면서 남은 흔적이다). */
	DECLARE_COMPLETION_ONSTACK(complete);
	/* [한국어] dmaengine 전송 서술자. */
	struct dma_async_tx_descriptor *desc;
	/* [한국어] 채널 슬레이브 설정. */
	struct dma_slave_config config = {};
	/* [한국어] 제출 결과 쿠키. */
	dma_cookie_t cookie;
	/* [한국어] 목적지 커널 버퍼의 DMA 주소. */
	dma_addr_t dst_addr;
	int ret;

	/* [한국어] 채널 설정과 제출을 임계구역으로 묶는다. 제출까지만 보호하면
	 * 되는 이유는, 일단 서술자가 큐에 들어가면 그 안에 주소가 박혀 있어
	 * 이후 다른 전송이 config 를 바꿔도 영향이 없기 때문이다. */
	mutex_lock(&epf_mhi->lock);

	/* [한국어] 원격(호스트) → 로컬 방향. */
	config.direction = DMA_DEV_TO_MEM;
	/* [한국어] 원본이 되는 호스트 PCI 주소를 "장치 쪽 주소" 로 등록한다. */
	config.src_addr = buf_info->host_addr;

	/* [한국어] 채널에 설정을 반영한다. */
	ret = dmaengine_slave_config(chan, &config);
	/* [한국어] 채널이 이 방향/주소를 받아들이지 못한 경우. */
	if (ret) {
		dev_err(dev, "Failed to configure DMA channel\n");
		/* [한국어] 매핑도 할당도 아직 없으므로 락만 풀면 된다. */
		goto err_unlock;
	}

	/* [한국어] 목적지 커널 버퍼를 DMA_FROM_DEVICE 로 매핑한다. 이 매핑은
	 * 이 함수 안에서 풀지 않는다 -- 전송이 진행 중이기 때문이며, 해제는
	 * pci_epf_mhi_dma_worker() 가 완료 후에 한다. 그래서 아래에서 주소를
	 * transfer->paddr 에 반드시 기록해 둬야 한다. */
	dst_addr = dma_map_single(dma_dev, buf_info->dev_addr, buf_info->size,
				  DMA_FROM_DEVICE);
	/* [한국어] 매핑 성공 여부 판정. */
	ret = dma_mapping_error(dma_dev, dst_addr);
	/* [한국어] 매핑 실패 시. */
	if (ret) {
		dev_err(dev, "Failed to map remote memory\n");
		/* [한국어] 풀 매핑이 없으므로 err_unmap 이 아니라 err_unlock 으로 간다. */
		goto err_unlock;
	}

	/* [한국어] 전송 서술자 준비. 인자 구성은 동기 판과 동일하다.
	 * DMA_PREP_INTERRUPT 가 특히 중요하다 -- 비동기 경로에서는 완료 콜백이
	 * 유일한 진행 신호라 인터럽트가 없으면 전송이 영원히 미완으로 남는다. */
	desc = dmaengine_prep_slave_single(chan, dst_addr, buf_info->size,
					   DMA_DEV_TO_MEM,
					   DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	/* [한국어] 서술자 준비 실패. */
	if (!desc) {
		dev_err(dev, "Failed to prepare DMA\n");
		ret = -EIO;
		/* [한국어] 매핑은 했으니 반드시 풀고 나가야 한다. */
		goto err_unmap;
	}

	/* [한국어] 완료 정보를 담을 봉투를 할당한다. kzalloc_obj(*transfer) 는
	 * transfer 의 타입을 typeof 로 뽑아 그 크기만큼 0 으로 채워 할당하는
	 * 최신 커널의 매크로(include/linux/slab.h)로, kzalloc(sizeof(*transfer),
	 * GFP_KERNEL) 과 같은 뜻이면서 타입 불일치를 컴파일 타임에 막아 준다.
	 * GFP 플래그를 생략했으므로 기본값 GFP_KERNEL 이 쓰여 잠들 수 있다 --
	 * 이 함수가 프로세스 문맥이어야 하는 또 하나의 이유다. */
	transfer = kzalloc_obj(*transfer);
	/* [한국어] 메모리 부족. 서술자는 이미 만들었지만 dmaengine 이 회수하므로
	 * 따로 되돌릴 것이 없고, 매핑만 풀면 된다. */
	if (!transfer) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	/* [한국어] 완료 콜백이 리스트와 워크큐에 접근할 수 있도록 인스턴스 포인터를
	 * 심는다. 콜백 인자로는 이 봉투 하나만 전달되기 때문이다. */
	transfer->epf_mhi = epf_mhi;
	/* [한국어] 나중에 워커가 dma_unmap_single 에 넘길 DMA 주소를 저장한다.
	 * 이 값을 기록하지 않으면 매핑을 영영 풀 수 없어 IOMMU 자원이 샌다. */
	transfer->paddr = dst_addr;
	/* [한국어] 언매핑에 필요한 길이. 매핑 때와 같은 값이어야 한다. */
	transfer->size = buf_info->size;
	/* [한국어] 언매핑에 필요한 방향. 읽기이므로 DMA_FROM_DEVICE 다. */
	transfer->dir = DMA_FROM_DEVICE;
	/* [한국어] 전송 서술자를 통째로 값 복사한다. 원본 buf_info 는 MHI 코어의
	 * 스택 변수라 이 함수가 반환하는 순간 사라지므로, 포인터만 들고 있으면
	 * 워커가 나중에 접근할 때 이미 무효한 메모리다. 이 memcpy 가 그 문제를
	 * 막는 유일한 장치다. */
	memcpy(&transfer->buf_info, buf_info, sizeof(*buf_info));

	/* [한국어] 동기 판과 달리 completion 을 깨우는 콜백이 아니라, 리스트에
	 * 걸고 워커를 깨우는 콜백을 단다. */
	desc->callback = pci_epf_mhi_dma_async_callback;
	/* [한국어] 콜백 인자로 방금 채운 봉투를 넘긴다. */
	desc->callback_param = transfer;

	/* [한국어] 서술자를 채널 대기열에 제출한다. */
	cookie = dmaengine_submit(desc);
	/* [한국어] 제출 오류 판정. */
	ret = dma_submit_error(cookie);
	/* [한국어] 제출이 거부된 경우 -- 콜백이 영영 불리지 않으므로 봉투와
	 * 매핑을 여기서 직접 정리해야 한다. */
	if (ret) {
		dev_err(dev, "Failed to do DMA submit\n");
		goto err_free_transfer;
	}

	/* [한국어] 엔진을 돌려 실제 전송을 시작한다. 이 뒤로는 완료 콜백이
	 * 봉투와 매핑의 소유권을 가져가므로, 성공 경로에서는 둘 다 건드리면 안 된다. */
	dma_async_issue_pending(chan);

	/* [한국어] 성공 경로. 매핑을 푸는 err_unmap 을 반드시 건너뛰어야 하므로
	 * 곧장 err_unlock 으로 점프한다. 라벨 이름이 err_ 로 시작하지만 여기서는
	 * 정상 종료 경로로 쓰인다. ret 은 dma_submit_error 가 넣어 둔 0 이다. */
	goto err_unlock;

err_free_transfer:
	/* [한국어] 제출이 실패해 콜백이 불리지 않을 것이 확실할 때만 여기로 온다.
	 * 봉투를 해제한 뒤 아래로 흘러 매핑도 푼다. */
	kfree(transfer);
err_unmap:
	/* [한국어] 전송이 시작되지 못한 경우에만 매핑을 여기서 푼다. 성공 경로는
	 * 위의 goto 로 이 줄을 건너뛰며, 그때의 언매핑은 워커가 담당한다. */
	dma_unmap_single(dma_dev, dst_addr, buf_info->size, DMA_FROM_DEVICE);
err_unlock:
	/* [한국어] 성공/실패 모든 경로가 지나는 락 해제 지점. */
	mutex_unlock(&epf_mhi->lock);

	/* [한국어] 0 은 "제출 성공" 을 뜻할 뿐 전송 완료가 아니다. 호출자(MHI 코어)는
	 * 0 을 받으면 완료 콜백을 기다린다. */
	return ret;
}

/*
 * [한국어]
 * pci_epf_mhi_edma_write_async - 호스트로의 쓰기를 eDMA 에 던져 놓고 즉시 반환
 *
 * @mhi_cntrl: MHI EP 코어의 컨트롤러 포인터.
 * @buf_info: 전송 서술자. dev_addr(원본, 엔드포인트 버퍼), host_addr(목적지),
 *            size, 그리고 반드시 cb 가 채워져 있어야 한다.
 * @return: 0 이면 제출 성공(완료가 아니다). 음수면 제출 실패이며 cb 는 불리지 않는다.
 *
 * 왜 필요한가: pci_epf_mhi_edma_read_async() 의 거울상이다. MHI 코어가
 * 다운링크 채널로 데이터를 밀어 올릴 때(mhi_ep_queue_skb 계열 경로) 전송마다
 * 잠들지 않도록 비동기로 제출한다.
 *
 * 동작 단계: read_async 와 방향만 반대다.
 *  - 채널은 dma_chan_tx, 방향은 DMA_MEM_TO_DEV
 *  - config.dst_addr 에 호스트 주소(목적지)를 넣는다
 *  - 매핑 방향은 DMA_TO_DEVICE 이고 transfer->dir 에도 같은 값을 저장한다
 *
 * 실행 컨텍스트: 프로세스 문맥(뮤텍스 + GFP_KERNEL 할당).
 * 에러 경로: read_async 와 동일한 세 단 goto 사다리. 성공 경로만 매핑을
 * 남긴 채 빠져나가고, 그 매핑은 워커가 푼다.
 *
 * 호출 체인:
 *   MHI EP 코어(mhi_cntrl->write_async) → [pci_epf_mhi_edma_write_async]
 *     → dmaengine_submit
 *   (완료 후) dmaengine → pci_epf_mhi_dma_async_callback
 *     → pci_epf_mhi_dma_worker → buf_info->cb
 */
static int pci_epf_mhi_edma_write_async(struct mhi_ep_cntrl *mhi_cntrl,
					struct mhi_ep_buf_info *buf_info)
{
	/* [한국어] 드라이버 인스턴스 복원. */
	struct pci_epf_mhi *epf_mhi = to_epf_mhi(mhi_cntrl);
	/* [한국어] DMA 매핑 기준 장치(EPC 의 부모). */
	struct device *dma_dev = epf_mhi->epf->epc->dev.parent;
	/* [한국어] 완료 뒷정리 정보를 담을 봉투. NULL 초기화. */
	struct pci_epf_mhi_dma_transfer *transfer = NULL;
	/* [한국어] 쓰기 방향 eDMA 채널. */
	struct dma_chan *chan = epf_mhi->dma_chan_tx;
	/* [한국어] 오류 로그의 주체. */
	struct device *dev = &epf_mhi->epf->dev;
	/* [한국어] read_async 와 마찬가지로 이 completion 은 쓰이지 않는다.
	 * 동기 판에서 코드를 복사해 오면서 남은 미사용 변수다. */
	DECLARE_COMPLETION_ONSTACK(complete);
	/* [한국어] dmaengine 전송 서술자. */
	struct dma_async_tx_descriptor *desc;
	/* [한국어] 채널 슬레이브 설정. */
	struct dma_slave_config config = {};
	/* [한국어] 제출 결과 쿠키. */
	dma_cookie_t cookie;
	/* [한국어] 원본 커널 버퍼의 DMA 주소. */
	dma_addr_t src_addr;
	int ret;

	/* [한국어] 채널 설정~제출을 임계구역으로 보호한다. */
	mutex_lock(&epf_mhi->lock);

	/* [한국어] 로컬 → 원격(호스트) 방향. */
	config.direction = DMA_MEM_TO_DEV;
	/* [한국어] 목적지 호스트 PCI 주소를 "장치 쪽 주소" 로 등록한다. */
	config.dst_addr = buf_info->host_addr;

	/* [한국어] 채널에 설정을 반영. */
	ret = dmaengine_slave_config(chan, &config);
	/* [한국어] 설정 거부 시. */
	if (ret) {
		dev_err(dev, "Failed to configure DMA channel\n");
		/* [한국어] 매핑 전이라 락만 풀고 나간다. */
		goto err_unlock;
	}

	/* [한국어] 원본 버퍼를 DMA_TO_DEVICE 로 매핑한다. 이 매핑도 성공 경로에서는
	 * 여기서 풀지 않고 워커가 완료 후에 푼다. */
	src_addr = dma_map_single(dma_dev, buf_info->dev_addr, buf_info->size,
				  DMA_TO_DEVICE);
	/* [한국어] 매핑 성공 여부 판정. */
	ret = dma_mapping_error(dma_dev, src_addr);
	/* [한국어] 매핑 실패 시. */
	if (ret) {
		dev_err(dev, "Failed to map remote memory\n");
		/* [한국어] 풀 매핑이 없으므로 err_unlock 으로 직행. */
		goto err_unlock;
	}

	/* [한국어] 전송 서술자 준비. DMA_PREP_INTERRUPT 가 있어야 완료 콜백이 불린다. */
	desc = dmaengine_prep_slave_single(chan, src_addr, buf_info->size,
					   DMA_MEM_TO_DEV,
					   DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	/* [한국어] 준비 실패. */
	if (!desc) {
		dev_err(dev, "Failed to prepare DMA\n");
		ret = -EIO;
		/* [한국어] 매핑을 풀고 나간다. */
		goto err_unmap;
	}

	/* [한국어] 완료 뒷정리 봉투를 0 초기화 할당한다(기본 GFP_KERNEL). */
	transfer = kzalloc_obj(*transfer);
	/* [한국어] 메모리 부족 시 매핑만 풀고 나간다. */
	if (!transfer) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	/* [한국어] 콜백이 리스트/워크큐에 닿을 수 있도록 인스턴스 포인터를 심는다. */
	transfer->epf_mhi = epf_mhi;
	/* [한국어] 워커가 풀어야 할 DMA 주소. 읽기 경로의 dst_addr 자리에 해당한다. */
	transfer->paddr = src_addr;
	/* [한국어] 언매핑에 필요한 길이. */
	transfer->size = buf_info->size;
	/* [한국어] 언매핑 방향. 쓰기이므로 DMA_TO_DEVICE. 매핑 때와 반드시 같아야 한다. */
	transfer->dir = DMA_TO_DEVICE;
	/* [한국어] MHI 코어의 스택에 있는 서술자를 값으로 복사해 둔다. 이 함수가
	 * 반환한 뒤에는 원본이 사라지므로, 워커가 완료 콜백을 부를 때 쓸 수 있는
	 * 것은 이 복사본뿐이다. */
	memcpy(&transfer->buf_info, buf_info, sizeof(*buf_info));

	/* [한국어] 워커를 깨우는 비동기 완료 콜백을 단다. */
	desc->callback = pci_epf_mhi_dma_async_callback;
	/* [한국어] 콜백 인자는 방금 채운 봉투. */
	desc->callback_param = transfer;

	/* [한국어] 서술자를 채널 대기열에 제출. */
	cookie = dmaengine_submit(desc);
	/* [한국어] 제출 오류 판정. */
	ret = dma_submit_error(cookie);
	/* [한국어] 제출 거부 시 -- 콜백이 안 불리므로 봉투와 매핑을 직접 정리한다. */
	if (ret) {
		dev_err(dev, "Failed to do DMA submit\n");
		goto err_free_transfer;
	}

	/* [한국어] 엔진을 돌려 전송을 시작한다. 이 뒤로 봉투와 매핑의 소유권은
	 * 완료 콜백 → 워커 쪽으로 넘어간다. */
	dma_async_issue_pending(chan);

	/* [한국어] 성공 경로. 매핑을 남겨 둔 채 나가야 하므로 err_unmap 을 건너뛴다. */
	goto err_unlock;

err_free_transfer:
	/* [한국어] 콜백이 불리지 않을 것이 확실할 때만 봉투를 직접 해제한다. */
	kfree(transfer);
err_unmap:
	/* [한국어] 전송이 시작되지 못한 경우에만 매핑을 여기서 푼다. */
	dma_unmap_single(dma_dev, src_addr, buf_info->size, DMA_TO_DEVICE);
err_unlock:
	/* [한국어] 모든 경로가 지나는 락 해제 지점. */
	mutex_unlock(&epf_mhi->lock);

	/* [한국어] 0 은 제출 성공이며 완료는 나중에 콜백으로 통지된다. */
	return ret;
}

/*
 * [한국어]
 * struct epf_dma_filter - dma_request_channel() 의 필터 함수에 넘길 조건 묶음
 *
 * dmaengine 은 시스템 전체의 DMA 채널을 한 풀에서 관리하므로, 그중 "우리
 * PCIe 컨트롤러에 속하고 원하는 방향을 지원하는" 채널만 골라야 한다.
 * dma_request_channel() 은 후보 채널마다 필터 함수를 부르며 이 구조체의
 * 주소를 그대로 넘겨 주고, 필터가 true 를 돌려준 첫 채널이 배정된다.
 * pci_epf_mhi_dma_init() 의 스택에 잡혀 두 번(tx/rx) 재사용된다.
 */
struct epf_dma_filter {
	struct device *dev;
	/* [한국어] 채널이 소속돼야 하는 장치 -- EPC 의 부모인 PCIe 컨트롤러
	 * platform_device 다. 시스템에 다른 DMA 컨트롤러가 여럿 있어도 이 조건이
	 * 우리 컨트롤러의 eDMA 채널만 남긴다.
	 * 설정자: pci_epf_mhi_dma_init() 이 filter.dev = dma_dev 로 한 번 채운다.
	 * 읽는 자: pci_epf_mhi_filter() 가 chan->device->dev 와 비교한다.
	 * 값 범위: 유효한 struct device 포인터.
	 * 동기화: 스택 지역 변수라 다른 문맥과 공유되지 않는다. */

	u32 dma_mask;
	/* [한국어] 요구하는 전송 방향의 비트마스크. BIT(DMA_MEM_TO_DEV) 이면 쓰기
	 * 채널, BIT(DMA_DEV_TO_MEM) 이면 읽기 채널을 찾는다는 뜻이다.
	 * 설정자: pci_epf_mhi_dma_init() 이 tx 요청 전과 rx 요청 전에 각각 다시 채운다.
	 * 읽는 자: pci_epf_mhi_filter() 가 caps.directions 와 AND 해서 겹치는지 본다.
	 * 값 범위: enum dma_transfer_direction 값을 비트 위치로 쓴 마스크.
	 * 동기화: 스택 지역 변수. */
};

/*
 * [한국어]
 * pci_epf_mhi_filter - dma_request_channel() 이 후보 채널마다 부르는 선별 함수
 *
 * @chan: 검사 대상 후보 DMA 채널. dmaengine 이 자기 풀에서 하나씩 넘겨 준다.
 * @node: dma_request_channel() 의 세 번째 인자로 넘긴 struct epf_dma_filter 포인터.
 *        인자 이름이 node 인 것은 dmaengine 의 dma_filter_fn 원형이
 *        DT 노드를 넘기는 관례에서 왔기 때문이며, 여기서는 필터 조건 구조체다.
 * @return: true 면 이 채널을 쓰겠다는 뜻이라 dmaengine 이 배정을 확정한다.
 *          false 면 다음 후보로 넘어간다. 끝까지 true 가 없으면 요청이 실패한다.
 *
 * 왜 필요한가: 시스템에는 여러 DMA 컨트롤러가 있고, 같은 컨트롤러 안에서도
 * 채널마다 지원 방향이 다르다(dw-edma 는 읽기 엔진과 쓰기 엔진이 별개다).
 * 우리에게 필요한 것은 "이 PCIe 컨트롤러 소속이면서 원하는 방향을 지원하는"
 * 채널이므로 두 조건을 여기서 검사한다.
 *
 * 동작 단계:
 *  1) caps 를 0 으로 밀고 dma_get_slave_caps() 로 채널 능력을 조회한다.
 *  2) 채널의 부모 장치가 우리 PCIe 컨트롤러인지 비교한다.
 *  3) 요구 방향 마스크와 채널이 지원하는 방향 집합이 겹치는지 본다.
 *
 * 실행 컨텍스트: dma_request_channel() 을 부른 문맥, 즉
 * pci_epf_mhi_dma_init() 의 프로세스 문맥에서 동기적으로 불린다.
 * 에러 경로: dma_get_slave_caps() 의 반환값을 확인하지 않는다. 실패하면 caps 가
 * 0 으로 남아 방향 검사가 자연히 false 가 되므로 결과적으로 후보에서 탈락한다 --
 * 앞의 memset 이 이 안전한 실패를 보장하는 장치다.
 *
 * 호출 체인:
 *   pci_epf_mhi_dma_init → dma_request_channel → [pci_epf_mhi_filter]
 *     → dma_get_slave_caps
 */
static bool pci_epf_mhi_filter(struct dma_chan *chan, void *node)
{
	/* [한국어] void * 로 온 필터 조건을 제 타입으로 되돌린다. */
	struct epf_dma_filter *filter = node;
	/* [한국어] 채널의 슬레이브 능력(지원 방향, 버스 폭, 최대 버스트 등)을 받을 상자. */
	struct dma_slave_caps caps;

	/* [한국어] 조회 전에 0 으로 민다. dma_get_slave_caps() 가 실패하면 구조체를
	 * 손대지 않을 수 있는데, 그때 스택 쓰레기가 directions 에 남아 있으면
	 * 엉뚱한 채널을 통과시킬 수 있다. 이 memset 이 그 위험을 없앤다. */
	memset(&caps, 0, sizeof(caps));
	/* [한국어] 채널 능력을 조회한다. 반환값을 보지 않는 이유는 위 memset 덕에
	 * 실패해도 안전하게 탈락하기 때문이다. */
	dma_get_slave_caps(chan, &caps);

	/* [한국어] 두 조건의 AND 다.
	 * (1) chan->device->dev == filter->dev : 이 채널이 우리 PCIe 컨트롤러의
	 *     DMA 엔진에 속하는가. 시스템의 다른 DMA 컨트롤러를 배제한다.
	 * (2) filter->dma_mask & caps.directions : 요구 방향 비트가 채널이
	 *     지원하는 방향 집합에 들어 있는가. dw-edma 는 읽기 채널에
	 *     BIT(DMA_DEV_TO_MEM), 쓰기 채널에 BIT(DMA_MEM_TO_DEV) 만 세워 주므로
	 *     이 검사가 tx/rx 를 정확히 갈라낸다.
	 * 주의: & 의 결과는 bool 이 아니라 정수지만, 반환 타입이 bool 이라
	 *       0 이 아니면 true 로 승격된다. */
	return chan->device->dev == filter->dev && filter->dma_mask &
					caps.directions;
}

/*
 * [한국어]
 * pci_epf_mhi_dma_init - eDMA 채널 두 개와 완료 처리용 워크큐를 준비한다
 *
 * @epf_mhi: 이 드라이버 인스턴스. 결과물(채널, 워크큐, 리스트, 락)을 여기에 채운다.
 * @return: 0 성공. -ENODEV 는 요구 조건에 맞는 채널을 못 찾은 것,
 *          -ENOMEM 은 워크큐 생성 실패다. 호출자(epc_init)는 오류 시
 *          BAR 설정을 이미 마친 상태로 실패를 반환한다.
 *
 * 왜 필요한가: MHI_EPF_USE_DMA 플래그가 선 플랫폼은 호스트 메모리 접근을
 * eDMA 로 하는데, 그러려면 (a) 방향별 채널 두 개와 (b) 비동기 완료 뒷정리를
 * 돌릴 워크큐 인프라가 있어야 한다. 이 함수가 그 셋업을 한 번에 한다.
 *
 * 동작 단계:
 *  1) 능력 마스크에 DMA_SLAVE 를 세운다 -- 우리는 memcpy 채널이 아니라
 *     "주소가 고정된 장치와 메모리 사이" 전송을 하는 슬레이브 채널이 필요하다.
 *  2) 필터 조건을 (우리 컨트롤러, MEM_TO_DEV) 로 세워 tx 채널을 요청한다.
 *  3) 방향 마스크만 DEV_TO_MEM 으로 바꿔 rx 채널을 요청한다.
 *  4) 완료 뒷정리용 워크큐를 만든다.
 *  5) 리스트/워크/스핀락을 초기화한다.
 *
 * 실행 컨텍스트: pci_epf_mhi_epc_init() 의 프로세스 문맥. EPC 초기화 통지를
 * 받은 시점이라 링크는 아직 데이터가 흐르기 전이다. 채널 요청과 워크큐 생성
 * 모두 잠들 수 있다.
 * 에러 경로: 뒤로 감기 식 goto 사다리다. rx 실패면 tx 만 반납하고,
 * 워크큐 실패면 rx → tx 순으로 반납한다. 반납한 채널 포인터는 NULL 로
 * 되돌려 두는데, 이는 나중에 deinit 이 두 번 불려도 안전하도록 하려는
 * 의도로 보이지만 pci_epf_mhi_dma_deinit() 은 NULL 검사를 하지 않는다.
 *
 * 호출 체인:
 *   EPC 코어(pci_epc_init_notify) → pci_epf_mhi_epc_init
 *     → [pci_epf_mhi_dma_init] → dma_request_channel(×2) → alloc_workqueue
 */
static int pci_epf_mhi_dma_init(struct pci_epf_mhi *epf_mhi)
{
	/* [한국어] 채널이 소속돼야 할 장치 -- EPC 의 부모인 PCIe 컨트롤러.
	 * eDMA 는 이 컨트롤러에 내장돼 있으므로 채널의 부모도 여기다. */
	struct device *dma_dev = epf_mhi->epf->epc->dev.parent;
	/* [한국어] 오류 로그의 주체가 될 EPF 장치. */
	struct device *dev = &epf_mhi->epf->dev;
	/* [한국어] 필터 함수에 넘길 조건. tx 요청과 rx 요청 사이에 dma_mask 만
	 * 갈아 끼워 재사용한다. */
	struct epf_dma_filter filter;
	/* [한국어] 요구하는 채널 능력의 비트맵. dmaengine 은 이 마스크에 세워진
	 * 능력을 모두 갖춘 채널만 후보로 올린다. */
	dma_cap_mask_t mask;
	int ret;

	/* [한국어] 능력 마스크를 전부 0 으로 민다. 스택 변수라 초기화가 필수다. */
	dma_cap_zero(mask);
	/* [한국어] DMA_SLAVE 능력을 요구한다. 슬레이브 채널은 한쪽 끝의 주소가
	 * slave_config 로 고정되는 전송을 뜻하며, 여기서는 그 고정 주소가
	 * 호스트 PCI 주소다. DMA_MEMCPY 채널이었다면 양쪽 다 로컬 메모리여서
	 * 링크 너머를 가리킬 수 없다. */
	dma_cap_set(DMA_SLAVE, mask);

	/* [한국어] 후보를 우리 PCIe 컨트롤러 소속으로 한정한다. */
	filter.dev = dma_dev;
	/* [한국어] 첫 요청은 쓰기(엔드포인트 → 호스트) 방향 채널이다. */
	filter.dma_mask = BIT(DMA_MEM_TO_DEV);
	/* [한국어] 조건에 맞는 채널을 하나 배정받는다. dmaengine 이 후보마다
	 * pci_epf_mhi_filter() 를 부르고, true 를 돌려준 첫 채널이 우리 것이 된다.
	 * 배정된 채널은 dma_release_channel() 전까지 독점 사용이다. */
	epf_mhi->dma_chan_tx = dma_request_channel(mask, pci_epf_mhi_filter,
						   &filter);
	/* [한국어] dma_request_channel() 은 실패 시 NULL 을 돌려주지만, 구현에
	 * 따라 ERR_PTR 이 올 수도 있어 두 경우를 한꺼번에 거른다. */
	if (IS_ERR_OR_NULL(epf_mhi->dma_chan_tx)) {
		dev_err(dev, "Failed to request tx channel\n");
		/* [한국어] 아직 아무 자원도 잡지 않았으므로 곧장 반환한다.
		 * 오류 값을 -ENODEV 로 통일하는 이유는 NULL 인 경우 오류 코드가
		 * 아예 없기 때문이다. */
		return -ENODEV;
	}

	/* [한국어] 두 번째 요청은 읽기(호스트 → 엔드포인트) 방향. filter.dev 는
	 * 그대로 두고 방향 마스크만 바꿔 같은 구조체를 재사용한다. */
	filter.dma_mask = BIT(DMA_DEV_TO_MEM);
	/* [한국어] 읽기 채널 배정. dw-edma 에서 이 채널은 쓰기 채널과 물리적으로
	 * 다른 엔진이라 두 요청이 서로 다른 채널을 받는다. */
	epf_mhi->dma_chan_rx = dma_request_channel(mask, pci_epf_mhi_filter,
						   &filter);
	/* [한국어] 읽기 채널 확보 실패. */
	if (IS_ERR_OR_NULL(epf_mhi->dma_chan_rx)) {
		dev_err(dev, "Failed to request rx channel\n");
		ret = -ENODEV;
		/* [한국어] 이미 잡은 tx 채널을 반드시 반납해야 한다. 안 그러면
		 * 그 채널이 영영 점유된 채로 남는다. */
		goto err_release_tx;
	}

	/* [한국어] 비동기 완료 뒷정리를 돌릴 전용 워크큐. WQ_PERCPU 는 작업을
	 * 큐잉한 CPU 에서 실행하라는 뜻으로, DMA 완료 인터럽트를 받은 CPU 에서
	 * 그대로 뒷정리를 이어가 캐시 지역성을 살린다. 세 번째 인자 0 은
	 * max_active 를 기본값으로 두라는 의미다. */
	epf_mhi->dma_wq = alloc_workqueue("pci_epf_mhi_dma_wq", WQ_PERCPU, 0);
	/* [한국어] 워크큐 생성 실패(메모리 부족). */
	if (!epf_mhi->dma_wq) {
		ret = -ENOMEM;
		/* [한국어] 채널 두 개를 모두 반납하고 나가야 한다. */
		goto err_release_rx;
	}

	/* [한국어] 완료 대기 목록을 빈 상태로 초기화한다. 첫 완료 콜백이
	 * list_add_tail 을 부르기 전에 반드시 끝나 있어야 한다. */
	INIT_LIST_HEAD(&epf_mhi->dma_list);
	/* [한국어] 워크 항목과 핸들러를 연결한다. 이 초기화가 없으면
	 * queue_work 가 초기화되지 않은 구조체를 건드려 커널이 죽는다. */
	INIT_WORK(&epf_mhi->dma_work, pci_epf_mhi_dma_worker);
	/* [한국어] 리스트를 지킬 스핀락 초기화. 위 두 초기화와 함께,
	 * DMA 가 실제로 돌기 전(=epc_init 반환 전)에 끝나야 한다. */
	spin_lock_init(&epf_mhi->list_lock);

	return 0;

err_release_rx:
	/* [한국어] 워크큐 생성이 실패했을 때만 지나는 지점. 읽기 채널을 반납한다. */
	dma_release_channel(epf_mhi->dma_chan_rx);
	/* [한국어] 반납한 채널 포인터를 지워 나중에 실수로 쓰지 못하게 한다.
	 * 이 정리가 없으면 해제된 채널 구조체를 가리키는 댕글링 포인터가 남는다. */
	epf_mhi->dma_chan_rx = NULL;
err_release_tx:
	/* [한국어] 읽기 채널 실패와 워크큐 실패 두 경로가 모두 지난다.
	 * 쓰기 채널을 반납한다. */
	dma_release_channel(epf_mhi->dma_chan_tx);
	/* [한국어] 쓰기 채널 포인터도 지운다. */
	epf_mhi->dma_chan_tx = NULL;

	/* [한국어] 위에서 정한 -ENODEV 또는 -ENOMEM 을 그대로 올린다. */
	return ret;
}

/*
 * [한국어]
 * pci_epf_mhi_dma_deinit - pci_epf_mhi_dma_init() 이 잡은 자원을 모두 반납한다
 *
 * @epf_mhi: 이 드라이버 인스턴스.
 * @return: 없음.
 *
 * 왜 필요한가: 링크가 끊기거나 EPF 가 떨어져 나갈 때 eDMA 채널을 붙잡고
 * 있으면 다른 사용자가 그 채널을 못 쓴다. 워크큐도 마찬가지로 커널 스레드를
 * 계속 잡아먹는다. 그래서 대칭되는 해제 함수가 필요하다.
 *
 * 동작 단계:
 *  1) 워크큐를 먼저 파괴한다. destroy_workqueue() 는 큐에 남은 작업을 전부
 *     끝낸 뒤에야 돌아오므로, 아직 처리되지 않은 완료 항목들의 언매핑과
 *     MHI 콜백이 여기서 마무리된다. 채널보다 먼저 없애는 이 순서가 중요하다 --
 *     반대로 하면 워커가 이미 해제된 채널로 만들어진 매핑을 다루게 된다.
 *  2) tx/rx 채널을 반납하고 포인터를 지운다.
 *
 * 실행 컨텍스트: 프로세스 문맥. destroy_workqueue() 가 잠들며 기다린다.
 * epc_deinit / link_down / bus_master_enable 실패 경로 / unbind 에서 불린다.
 * 에러 경로: 없다. 다만 dma_init 이 실패한 상태에서 이 함수가 불리면
 * NULL 포인터를 그대로 넘기게 되는데, 호출자들이 mhi_cntrl->mhi_dev 존재
 * 여부로 걸러 주므로 실제로 그 경로가 열리지는 않는다.
 *
 * 호출 체인:
 *   pci_epf_mhi_epc_deinit / _link_down / _unbind / _bus_master_enable(실패 시)
 *     → [pci_epf_mhi_dma_deinit] → destroy_workqueue → dma_release_channel(×2)
 */
static void pci_epf_mhi_dma_deinit(struct pci_epf_mhi *epf_mhi)
{
	/* [한국어] 워크큐를 먼저 없앤다. 남아 있는 dma_work 실행이 끝날 때까지
	 * 기다려 주므로, 미처리 완료 항목의 dma_unmap_single 과 buf_info->cb 가
	 * 여기서 전부 소진된다. 채널을 먼저 반납하면 그 뒷정리가 이미 사라진
	 * 채널의 매핑을 다루게 되므로 이 순서를 바꾸면 안 된다. */
	destroy_workqueue(epf_mhi->dma_wq);
	/* [한국어] 쓰기 채널을 dmaengine 풀에 돌려준다. 다른 드라이버가 다시
	 * 요청할 수 있게 된다. */
	dma_release_channel(epf_mhi->dma_chan_tx);
	/* [한국어] 읽기 채널도 반납한다. */
	dma_release_channel(epf_mhi->dma_chan_rx);
	/* [한국어] 댕글링 포인터를 없앤다. 링크가 다시 올라와 dma_init 이
	 * 재실행되기 전까지 NULL 인 상태가 정상이다. */
	epf_mhi->dma_chan_tx = NULL;
	/* [한국어] 읽기 채널 포인터도 지운다. */
	epf_mhi->dma_chan_rx = NULL;
}

/*
 * [한국어]
 * pci_epf_mhi_epc_init - EPC 초기화 완료 통지를 받아 PCI 기능을 실제로 구성한다
 *
 * @epf: 이 콜백의 주인인 EPF 장치. epf_get_drvdata() 로 우리 인스턴스를 꺼낸다.
 * @return: 0 성공. BAR/MSI/헤더 설정 실패 시 각각의 오류 코드, EPC 능력 조회
 *          실패 시 -ENODATA, DMA 초기화 실패 시 그 오류를 반환한다. EPC 코어는
 *          반환값을 확인하지만 실패해도 다른 EPF 의 통지는 계속 진행한다.
 *
 * 왜 필요한가: BAR 나 MSI 설정은 EPC 하드웨어가 살아 있어야 가능한데, 그
 * 시점은 EPF 가 바인딩되는 순간이 아니라 EPC 드라이버가 초기화를 마치고
 * pci_epc_init_notify() 를 부르는 순간이다(전원 관리나 리셋 후 다시 불릴 수도
 * 있다). 그래서 하드웨어 프로그래밍은 bind 가 아니라 이 콜백에서 한다.
 *
 * 동작 단계:
 *  1) BAR 서술자를 채우고 pci_epc_set_bar() 로 인바운드 창을 연다. 이 BAR 뒤에
 *     있는 것은 dma_alloc_coherent 로 잡은 버퍼가 아니라 SoC 의 MHI 레지스터
 *     블록 자체다 -- 그래서 호스트의 BAR0 접근이 곧바로 MHI 레지스터 접근이 된다.
 *  2) MSI 개수를 설정한다.
 *  3) 설정 헤더(vendor/device/class)를 쓴다.
 *  4) EPC 능력표를 받아 둔다(align 값이 필요하다).
 *  5) 플랫폼이 eDMA 를 쓰면 채널과 워크큐를 준비한다.
 *
 * 실행 컨텍스트: EPC 코어의 pci_epc_init_notify() 문맥, 프로세스 문맥이다.
 * pci_epc_ 계열 함수들이 내부에서 epc->lock 뮤텍스를 잡으므로 잠들 수 있다.
 * 에러 경로: 중간에 실패해도 앞서 한 설정을 되돌리지 않는다. BAR 는 열린 채
 * 남는데, 이후 pci_epf_mhi_epc_deinit() 이나 _unbind() 가 pci_epc_clear_bar()
 * 로 정리하므로 자원이 새지는 않는다.
 *
 * 호출 체인:
 *   EPC 드라이버 → pci_epc_init_notify → epf->event_ops->epc_init
 *     → [pci_epf_mhi_epc_init] → pci_epc_set_bar / set_msi / write_header
 *       / get_features / pci_epf_mhi_dma_init
 */
static int pci_epf_mhi_epc_init(struct pci_epf *epf)
{
	/* [한국어] probe 에서 epf_set_drvdata 로 매달아 둔 인스턴스를 꺼낸다. */
	struct pci_epf_mhi *epf_mhi = epf_get_drvdata(epf);
	/* [한국어] 이 SoC 모델의 하드웨어 기술표. BAR 번호와 MSI 개수를 여기서 읽는다. */
	const struct pci_epf_mhi_ep_info *info = epf_mhi->info;
	/* [한국어] 우리가 쓸 BAR 의 서술자. epf->bar[] 배열은 EPF 코어가 소유하고
	 * 있고, 여기에 물리 주소/크기/플래그를 채워 EPC 에 넘기는 방식이다. */
	struct pci_epf_bar *epf_bar = &epf->bar[info->bar_num];
	/* [한국어] 실제 하드웨어를 프로그래밍할 엔드포인트 컨트롤러. */
	struct pci_epc *epc = epf->epc;
	/* [한국어] 오류 로그의 주체가 될 EPF 장치. */
	struct device *dev = &epf->dev;
	int ret;

	/* [한국어] BAR 뒤에 놓을 실제 물리 주소 -- SoC 의 MHI 레지스터 블록이다.
	 * 다른 EPF 드라이버(pci-epf-test 등)는 여기에 pci_epf_alloc_space() 로
	 * 잡은 DMA 버퍼 주소를 넣지만, MHI 는 레지스터 자체를 노출해야 하므로
	 * bind 에서 확보해 둔 platform 자원의 물리 주소를 그대로 쓴다. */
	epf_bar->phys_addr = epf_mhi->mmio_phys;
	/* [한국어] BAR 크기 = MHI 레지스터 블록 크기. DT 의 "mmio" reg 항목이 정한다.
	 * 하드웨어가 2의 거듭제곱 크기를 요구하면 EPC 드라이버가 걸러 낸다. */
	epf_bar->size = epf_mhi->mmio_size;
	/* [한국어] 이 서술자가 몇 번 BAR 인지 명시한다. epf->bar[] 의 첨자와 같은
	 * 값이지만, EPC 코어가 서술자만 받고도 알 수 있도록 안에도 적어 준다. */
	epf_bar->barno = info->bar_num;
	/* [한국어] BAR 속성 비트(32비트 메모리 BAR). EPC 드라이버가 이 값을
	 * BAR 레지스터의 하위 비트로 프로그래밍해 호스트가 BAR 종류를 알게 한다. */
	epf_bar->flags = info->epf_flags;
	/* [한국어] 인바운드 주소 변환을 설정한다. 이 호출이 성공하면 호스트가
	 * BAR0 에 접근할 때 하드웨어가 epf_bar->phys_addr 로 번역해 준다.
	 * 즉 이 한 줄이 "호스트에게 MHI 레지스터를 보여 주는" 결정적 순간이다. */
	ret = pci_epc_set_bar(epc, epf->func_no, epf->vfunc_no, epf_bar);
	/* [한국어] BAR 크기가 하드웨어 제약(2의 거듭제곱, 최소 크기 등)에 맞지
	 * 않거나 인바운드 창이 모자라면 실패한다. */
	if (ret) {
		dev_err(dev, "Failed to set BAR: %d\n", ret);
		return ret;
	}

	/* [한국어] MSI 개수를 설정한다. PCI 스펙의 MSI 능력 레지스터는 개수를
	 * 그대로가 아니라 "2의 몇 제곱인지" 로 담으므로 order_base_2() 로
	 * 로그2 를 취해 넘긴다(32 → 5). 이 값이 호스트에게 "최대 32개까지
	 * 벡터를 줄 수 있다" 고 알리는 신고다. 실제로 몇 개가 배정될지는
	 * 호스트가 정한다. */
	ret = pci_epc_set_msi(epc, epf->func_no, epf->vfunc_no,
			      order_base_2(info->msi_count));
	/* [한국어] EPC 가 MSI 를 지원하지 않거나 요청 개수가 범위를 벗어난 경우. */
	if (ret) {
		dev_err(dev, "Failed to set MSI configuration: %d\n", ret);
		return ret;
	}

	/* [한국어] vendor/device/class 를 실제 설정 공간에 기록한다. epf->header 는
	 * probe 에서 info->epf_header 로 채워 둔 값이다. 이 기록이 끝나야 호스트가
	 * 열거 과정에서 올바른 신원을 읽는다. */
	ret = pci_epc_write_header(epc, epf->func_no, epf->vfunc_no,
				   epf->header);
	/* [한국어] 설정 공간 쓰기가 거부된 경우(EPC 가 헤더 쓰기를 지원하지 않는
	 * 구성 등). */
	if (ret) {
		dev_err(dev, "Failed to set Configuration header: %d\n", ret);
		return ret;
	}

	/* [한국어] EPC 의 능력/제약표를 받아 둔다. 이 드라이버가 실제로 쓰는 것은
	 * align 하나지만, 그것이 없으면 get_align_offset() 이 동작하지 못해
	 * iATU 경로 전체가 깨진다. 여기서 미리 받아 두는 이유는 데이터 경로에서
	 * 매번 조회하는 비용을 피하기 위해서다. */
	epf_mhi->epc_features = pci_epc_get_features(epc, epf->func_no, epf->vfunc_no);
	/* [한국어] EPC 드라이버가 get_features 를 구현하지 않았으면 NULL 이 온다.
	 * align 을 알 수 없으면 진행할 수 없으므로 -ENODATA(데이터가 없다)로
	 * 실패한다. */
	if (!epf_mhi->epc_features)
		return -ENODATA;

	/* [한국어] 이 플랫폼이 eDMA 를 쓰는가. sm8450/sa8775p 만 참이다. */
	if (info->flags & MHI_EPF_USE_DMA) {
		/* [한국어] 채널 두 개와 완료 처리 워크큐를 준비한다. link_up 에서
		 * 콜백 표를 eDMA 판으로 바꾸기 전에 반드시 끝나 있어야 한다. */
		ret = pci_epf_mhi_dma_init(epf_mhi);
		/* [한국어] 채널을 못 잡았거나 워크큐 생성 실패. */
		if (ret) {
			dev_err(dev, "Failed to initialize DMA: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

/*
 * [한국어]
 * pci_epf_mhi_epc_deinit - EPC 가 초기화 해제됨을 알려 올 때 모든 것을 되돌린다
 *
 * @epf: 이 콜백의 주인인 EPF 장치.
 * @return: 없음(EPC 코어가 오류를 받을 수 없는 원형이다).
 *
 * 왜 필요한가: EPC 하드웨어가 리셋되거나 전원이 내려가면 BAR 설정과 MHI 스택이
 * 모두 무의미해진다. 이때 정리하지 않으면 (a) MHI 스택이 죽은 하드웨어에
 * 계속 접근하고 (b) eDMA 채널과 워크큐가 계속 점유된 채 남는다.
 *
 * 동작 단계:
 *  1) mhi_cntrl->mhi_dev 로 "MHI 컨트롤러가 등록된 적이 있는가" 를 판정한다.
 *     이 필드는 mhi_ep_register_controller() 가 채워 주는 것이라, NULL 이면
 *     아직 link_up 이 오지 않았거나 등록이 실패한 상태다.
 *  2) 등록돼 있으면 전원 끄기 → DMA 해제 → 컨트롤러 등록 해제 순으로 되돌린다.
 *  3) 마지막으로 BAR 를 닫는다. 이건 등록 여부와 무관하게 항상 한다 --
 *     epc_init 에서 BAR 를 열었다면 반드시 닫아야 하기 때문이다.
 *
 * 실행 컨텍스트: EPC 코어의 pci_epc_deinit_notify() 문맥, 프로세스 문맥.
 * 에러 경로: 없다. 각 해제 함수의 결과를 확인하지 않는다.
 *
 * 호출 체인:
 *   EPC 드라이버 → pci_epc_deinit_notify → epf->event_ops->epc_deinit
 *     → [pci_epf_mhi_epc_deinit] → mhi_ep_power_down
 *       → pci_epf_mhi_dma_deinit → mhi_ep_unregister_controller
 *       → pci_epc_clear_bar
 */
static void pci_epf_mhi_epc_deinit(struct pci_epf *epf)
{
	/* [한국어] 인스턴스 복원. */
	struct pci_epf_mhi *epf_mhi = epf_get_drvdata(epf);
	/* [한국어] BAR 번호와 DMA 사용 여부를 읽기 위한 기술표. */
	const struct pci_epf_mhi_ep_info *info = epf_mhi->info;
	/* [한국어] 닫아야 할 BAR 의 서술자. epc_init 에서 채워 둔 값이 그대로 있다. */
	struct pci_epf_bar *epf_bar = &epf->bar[info->bar_num];
	/* [한국어] MHI 컨트롤러. 등록 여부 판정과 전원 끄기에 쓴다. */
	struct mhi_ep_cntrl *mhi_cntrl = &epf_mhi->mhi_cntrl;
	/* [한국어] BAR 를 닫을 엔드포인트 컨트롤러. */
	struct pci_epc *epc = epf->epc;

	/* [한국어] mhi_dev 가 NULL 이 아니면 mhi_ep_register_controller() 가
	 * 성공적으로 끝났다는 뜻이다(코어가 컨트롤러용 MHI 장치를 만들어 이
	 * 필드에 넣는다). link_up 이 아직 오지 않았거나 등록이 실패했다면
	 * NULL 이라 이 블록을 통째로 건너뛴다. */
	if (mhi_cntrl->mhi_dev) {
		/* [한국어] MHI 스택의 전원을 내린다. 진행 중인 채널 처리와 이벤트
		 * 처리를 멈추고 워커들을 정지시킨다. 반드시 DMA 해제보다 먼저 해야
		 * 새 전송이 채널로 들어오지 않는다. */
		mhi_ep_power_down(mhi_cntrl);
		/* [한국어] eDMA 를 쓰는 플랫폼에서만 채널/워크큐를 정리한다. */
		if (info->flags & MHI_EPF_USE_DMA)
			/* [한국어] 워크큐를 비우고 채널을 반납한다. power_down 이 먼저
			 * 끝났으므로 이 시점에는 새 DMA 가 제출되지 않는다. */
			pci_epf_mhi_dma_deinit(epf_mhi);
		/* [한국어] MHI EP 버스에서 컨트롤러를 뗀다. 여기서 코어가 채널 배열,
		 * 커맨드 링, kmem_cache, "doorbell" IRQ 핸들러를 모두 해제하고
		 * mhi_dev 를 정리한다. */
		mhi_ep_unregister_controller(mhi_cntrl);
	}

	/* [한국어] 인바운드 창을 닫아 호스트가 더는 MHI 레지스터에 접근하지
	 * 못하게 한다. if 블록 밖에 있는 이유는, MHI 등록이 없었더라도 epc_init 이
	 * BAR 를 열었을 수 있기 때문이다. */
	pci_epc_clear_bar(epc, epf->func_no, epf->vfunc_no, epf_bar);
}

/*
 * [한국어]
 * pci_epf_mhi_link_up - PCIe 링크가 살아났을 때 MHI EP 컨트롤러를 등록한다
 *
 * @epf: 이 콜백의 주인인 EPF 장치.
 * @return: 0 성공. mhi_ep_register_controller() 가 실패하면 그 오류를 반환한다.
 *
 * 왜 필요한가: MHI 컨트롤러를 등록하면 코어가 곧바로 MHIVER 레지스터를 쓰고
 * "doorbell" IRQ 에 핸들러를 걸며 호스트와 대화할 준비를 한다. 이 일들은
 * 링크가 살아 있어야 의미가 있으므로 link_up 통지를 기다렸다가 한다.
 * 또 이 함수가 이 파일의 "정책 결정" 지점이다 -- 콜백 표를 iATU 판으로 채울지
 * eDMA 판으로 채울지가 여기서 갈린다.
 *
 * 동작 단계:
 *  1) mmio/irq/mru 를 MHI 컨트롤러에 옮겨 심는다. 이 셋은 bind 와 기술표에서
 *     이미 확보해 둔 값이다.
 *  2) cntrl_dev 로 PCIe 컨트롤러 장치를 지정한다.
 *  3) 콜백 표를 일단 전부 iATU 판으로 채운다(연쇄 대입으로 sync/async 를 같은
 *     함수로 만든다).
 *  4) eDMA 플랫폼이면 네 개를 각각 eDMA 판으로 덮어쓴다. alloc_map/unmap_free/
 *     raise_irq 는 DMA 와 무관하므로 그대로 둔다.
 *  5) mhi_ep_register_controller() 로 등록한다.
 *
 * 실행 컨텍스트: EPC 코어의 pci_epc_linkup() 문맥, 프로세스 문맥.
 * 에러 경로: 등록이 실패하면 epc_init 이 잡아 둔 DMA 자원을 여기서 되돌린다.
 * BAR 는 그대로 두는데, 이후 epc_deinit/unbind 가 정리한다.
 *
 * 호출 체인:
 *   EPC 드라이버 → pci_epc_linkup → epf->event_ops->link_up
 *     → [pci_epf_mhi_link_up] → mhi_ep_register_controller
 *       (그 안에서 request_irq(mhi_cntrl->irq, mhi_ep_irq, ..., "doorbell_irq"),
 *        MHIVER 레지스터 기록, 채널/커맨드 링 할당)
 */
static int pci_epf_mhi_link_up(struct pci_epf *epf)
{
	/* [한국어] 인스턴스 복원. */
	struct pci_epf_mhi *epf_mhi = epf_get_drvdata(epf);
	/* [한국어] mru/flags/config 를 읽을 하드웨어 기술표. */
	const struct pci_epf_mhi_ep_info *info = epf_mhi->info;
	/* [한국어] 채워 넣을 MHI 컨트롤러. 인스턴스 안에 값으로 들어 있어
	 * 따로 할당할 필요가 없다. */
	struct mhi_ep_cntrl *mhi_cntrl = &epf_mhi->mhi_cntrl;
	/* [한국어] cntrl_dev 로 쓸 부모 장치를 얻기 위한 EPC 포인터. */
	struct pci_epc *epc = epf->epc;
	/* [한국어] 오류 로그의 주체. */
	struct device *dev = &epf->dev;
	int ret;

	/* [한국어] MHI 레지스터 블록의 가상 주소를 넘긴다. 이 뒤로 MHI 코어의
	 * mhi_ep_mmio_read/write 가 이 포인터에 readl/writel 을 한다.
	 * 같은 물리 영역을 호스트는 BAR0 로, 엔드포인트는 이 포인터로 보는
	 * 이중 접근 구조가 MHI 의 핵심이다. */
	mhi_cntrl->mmio = epf_mhi->mmio;
	/* [한국어] 도어벨 IRQ 번호를 넘긴다. 등록 함수가 이 번호에
	 * request_irq(..., mhi_ep_irq, IRQF_TRIGGER_HIGH, "doorbell_irq", ...) 를
	 * 하며, IRQ_NOAUTOEN 을 세워 두어 power_up 때 비로소 활성화한다. */
	mhi_cntrl->irq = epf_mhi->irq;
	/* [한국어] 최대 수신 단위(32KB). 코어가 수신 버퍼 크기를 정할 때 쓴다. */
	mhi_cntrl->mru = info->mru;

	/* Assign the struct dev of PCI EP as MHI controller device */
	/* [한국어] 위 영문 주석대로, MHI 컨트롤러의 "물리 장치" 로 PCIe 엔드포인트
	 * 컨트롤러의 부모(platform_device)를 지정한다. EPF 장치나 EPC 장치가 아닌
	 * 이유는 DMA 마스크, IOMMU 도메인, DT 노드가 모두 그 장치에 붙어 있어
	 * MHI 코어가 메모리를 할당하거나 진단 로그를 남길 때 올바른 기준이
	 * 되기 때문이다. 데이터 경로의 dma_dev 와도 같은 장치다. */
	mhi_cntrl->cntrl_dev = epc->dev.parent;
	/* [한국어] 호스트에 MSI 를 올리는 콜백. DMA 유무와 무관하게 하나뿐이다. */
	mhi_cntrl->raise_irq = pci_epf_mhi_raise_irq;
	/* [한국어] 호스트 컨텍스트 구조체를 매핑하는 콜백. eDMA 로는 할 수 없는
	 * 일(코어가 그 영역을 CPU 로 직접 읽고 써야 한다)이라 항상 iATU 판이다. */
	mhi_cntrl->alloc_map = pci_epf_mhi_alloc_map;
	/* [한국어] 위 매핑을 되돌리는 콜백. 역시 항상 iATU 판. */
	mhi_cntrl->unmap_free = pci_epf_mhi_unmap_free;
	/* [한국어] 연쇄 대입으로 read_sync 와 read_async 를 둘 다 iATU 판으로
	 * 만든다. DMA 가 없는 플랫폼(sdx55)에서는 이 상태가 최종값이며, 비동기
	 * 자리에 동기 구현이 들어가도 동작한다 -- iatu_read 가 끝에서
	 * buf_info->cb 를 불러 주기 때문이다. 등록 함수는 네 콜백이 모두
	 * NULL 이 아닌지 검사하므로 이 기본값 채우기가 필수다. */
	mhi_cntrl->read_sync = mhi_cntrl->read_async = pci_epf_mhi_iatu_read;
	/* [한국어] 쓰기 쪽도 같은 방식으로 iATU 판을 기본값으로 깐다. */
	mhi_cntrl->write_sync = mhi_cntrl->write_async = pci_epf_mhi_iatu_write;
	/* [한국어] eDMA 를 쓰는 플랫폼이면 네 개를 전용 구현으로 덮어쓴다. */
	if (info->flags & MHI_EPF_USE_DMA) {
		/* [한국어] 동기 읽기는 eDMA 판으로. 4KB 미만은 이 함수가 알아서
		 * iATU 판에 위임하므로 작은 전송의 성능 손해가 없다. */
		mhi_cntrl->read_sync = pci_epf_mhi_edma_read;
		/* [한국어] 동기 쓰기도 마찬가지. */
		mhi_cntrl->write_sync = pci_epf_mhi_edma_write;
		/* [한국어] 비동기 읽기는 제출만 하고 돌아오는 판으로. 채널 데이터
		 * 경로의 처리량이 여기에 달려 있다. */
		mhi_cntrl->read_async = pci_epf_mhi_edma_read_async;
		/* [한국어] 비동기 쓰기도 같은 이유로 교체한다. */
		mhi_cntrl->write_async = pci_epf_mhi_edma_write_async;
	}

	/* Register the MHI EP controller */
	/* [한국어] MHI EP 버스에 컨트롤러를 등록한다. 이 안에서 코어가 콜백 네 개의
	 * 존재를 검증하고, 채널 배열과 커맨드 링을 할당하고, MHIVER 레지스터에
	 * 버전을 쓰고, 도어벨 IRQ 핸들러를 걸고, 컨트롤러용 MHI 장치를 만들어
	 * mhi_cntrl->mhi_dev 에 넣는다. 그 mhi_dev 가 이후 "등록됐는가" 판정의
	 * 기준이 된다. */
	ret = mhi_ep_register_controller(mhi_cntrl, info->config);
	/* [한국어] 콜백 누락, 메모리 부족, IRQ 요청 실패 등으로 등록이 실패한 경우. */
	if (ret) {
		dev_err(dev, "Failed to register MHI EP controller: %d\n", ret);
		/* [한국어] epc_init 이 잡아 둔 DMA 채널과 워크큐를 되돌린다.
		 * 등록에 실패한 이상 이 자원들을 계속 붙잡고 있을 이유가 없다. */
		if (info->flags & MHI_EPF_USE_DMA)
			pci_epf_mhi_dma_deinit(epf_mhi);
		return ret;
	}

	return 0;
}

/*
 * [한국어]
 * pci_epf_mhi_link_down - PCIe 링크가 끊겼을 때 MHI 스택을 통째로 내린다
 *
 * @epf: 이 콜백의 주인인 EPF 장치.
 * @return: 항상 0. 실패할 수 있는 일을 하지 않는다.
 *
 * 왜 필요한가: 링크가 끊기면 호스트 메모리에 접근할 수 없으므로 MHI 스택이
 * 계속 돌면 모든 전송이 실패하거나 멈춘다. 그 전에 정상적으로 내려서
 * 진행 중인 작업을 정리하고 자원을 반납한다. 다시 링크가 올라오면
 * link_up 이 처음부터 새로 등록한다 -- 즉 링크 다운/업은 완전한 재시작이다.
 *
 * 동작 단계: epc_deinit 의 앞부분과 완전히 같다. mhi_dev 로 등록 여부를 보고,
 * 등록돼 있으면 전원 끄기 → DMA 해제 → 등록 해제 순으로 되돌린다.
 * 차이는 BAR 를 닫지 않는다는 점이다 -- 링크가 끊겼을 뿐 EPC 하드웨어는
 * 살아 있으므로 BAR 설정은 유지해 두었다가 링크가 복구되면 그대로 쓴다.
 *
 * 실행 컨텍스트: EPC 코어의 링크 다운 통지 문맥, 프로세스 문맥.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   EPC 드라이버(링크 다운 인터럽트) → EPC 코어 → epf->event_ops->link_down
 *     → [pci_epf_mhi_link_down] → mhi_ep_power_down
 *       → pci_epf_mhi_dma_deinit → mhi_ep_unregister_controller
 */
static int pci_epf_mhi_link_down(struct pci_epf *epf)
{
	/* [한국어] 인스턴스 복원. */
	struct pci_epf_mhi *epf_mhi = epf_get_drvdata(epf);
	/* [한국어] DMA 사용 여부를 읽을 기술표. */
	const struct pci_epf_mhi_ep_info *info = epf_mhi->info;
	/* [한국어] 내려야 할 MHI 컨트롤러. */
	struct mhi_ep_cntrl *mhi_cntrl = &epf_mhi->mhi_cntrl;

	/* [한국어] 등록된 적이 있는지 확인한다. link_up 이 실패했거나 아직 오지
	 * 않았다면 mhi_dev 가 NULL 이라 아무것도 하지 않는다. 링크 다운 통지가
	 * 중복으로 와도 두 번째부터는 이 검사에서 걸러진다. */
	if (mhi_cntrl->mhi_dev) {
		/* [한국어] MHI 스택 전원을 내려 채널/이벤트 처리를 멈춘다. */
		mhi_ep_power_down(mhi_cntrl);
		/* [한국어] eDMA 플랫폼이면 채널과 워크큐를 반납한다. */
		if (info->flags & MHI_EPF_USE_DMA)
			/* [한국어] 워크큐를 비우며 미처리 완료를 소진한 뒤 채널을 놓는다. */
			pci_epf_mhi_dma_deinit(epf_mhi);
		/* [한국어] 컨트롤러 등록을 해제한다. 도어벨 IRQ 핸들러도 여기서
		 * 풀리고 mhi_dev 가 정리되어, 다음 link_up 이 처음 상태에서
		 * 다시 시작할 수 있게 된다. */
		mhi_ep_unregister_controller(mhi_cntrl);
	}

	/* [한국어] 이 콜백은 실패할 수 있는 일을 하지 않으므로 항상 성공이다.
	 * BAR 를 닫지 않는 것이 epc_deinit 과의 유일한 차이다. */
	return 0;
}

/* [한국어]
 * pci_epf_mhi_bus_master_enable - 호스트가 Bus Master 를 켰을 때 MHI 스택을 다시 올린다
 *
 * @epf: 이 EPF 인스턴스. epf_get_drvdata() 로 struct pci_epf_mhi 를 되찾는다.
 * @return: 항상 0. 전원 인가에 실패해도 0 을 돌려주고 정리만 한다 --
 *          EPC 코어의 통지 경로에는 되돌릴 만한 상위 동작이 없기 때문이다.
 *
 * PCI 설정 공간 Command 레지스터의 Bus Master Enable(BME) 비트는 호스트가
 * "이제 이 장치가 스스로 메모리 트랜잭션을 일으켜도 좋다" 고 허락하는 스위치다.
 * MHI 엔드포인트는 호스트 메모리에서 링을 읽고 쓰며 동작하므로, BME 가 서기
 * 전에 스택을 올리면 첫 DMA 가 곧바로 차단된다. 그래서 link_up 에서 컨트롤러를
 * 등록만 해 두고, 실제 전원 인가(mhi_ep_power_up)는 이 콜백까지 미룬다.
 *
 * 조건이 두 개인 이유:
 *   - !mhi_cntrl->enabled : 이미 올라와 있으면 두 번 올리지 않는다. 호스트가
 *     BME 를 껐다 켜면 이 콜백이 다시 오므로 재진입 방어가 필요하다.
 *   - mhi_cntrl->mhi_dev : 이 필드는 mhi_ep_register_controller() 가 채운다.
 *     NULL 이면 link_up 이 아직 안 왔거나 등록에 실패한 상태이므로, 올릴 대상
 *     자체가 없다. 두 콜백의 도착 순서를 하드웨어가 보장하지 않기 때문에
 *     BME 가 link_up 보다 먼저 와도 여기서 조용히 넘어간다.
 *
 * 에러 경로: power_up 이 실패하면 link_up 이 잡아 둔 자원을 되감는다 --
 * eDMA 플랫폼이면 채널/워크큐를 반납하고, 컨트롤러 등록을 해제한다. 이러면
 * mhi_dev 가 다시 NULL 이 되어, 다음 BME 통지 때 위 조건에서 걸러진다.
 * 즉 복구는 호스트가 링크를 내렸다 올려 link_up 을 다시 부르는 길뿐이다.
 *
 * 실행 컨텍스트: EPC 드라이버가 BME 인터럽트를 받아 부르는
 * pci_epc_bus_master_enable_notify() 문맥. 프로세스 문맥이며 잠들 수 있다.
 *
 * 호출 체인:
 *   EPC 드라이버(BME 인터럽트) → pci_epc_bus_master_enable_notify
 *     → epf->event_ops->bus_master_enable → [이 함수] → mhi_ep_power_up
 */
static int pci_epf_mhi_bus_master_enable(struct pci_epf *epf)
{
	struct pci_epf_mhi *epf_mhi = epf_get_drvdata(epf);
	const struct pci_epf_mhi_ep_info *info = epf_mhi->info;
	/* [한국어] [한국어] 전원을 올릴 대상. 인스턴스 안에 값으로 박혀 있으므로 주소만 취한다. */
	struct mhi_ep_cntrl *mhi_cntrl = &epf_mhi->mhi_cntrl;
	/* [한국어] [한국어] 실패 로그의 주체가 될 EPF 장치. */
	struct device *dev = &epf->dev;
	/* [한국어] [한국어] mhi_ep_power_up 의 반환값을 담을 임시 변수. */
	int ret;

	/*
	 * Power up the MHI EP stack if link is up and stack is in power down
	 * state.
	 */
	if (!mhi_cntrl->enabled && mhi_cntrl->mhi_dev) {
		ret = mhi_ep_power_up(mhi_cntrl);
		/* [한국어] [한국어] 전원 인가 실패. 여기서 되감지 않으면 link_up 이 잡아 둔 DMA 채널과
		 * 컨트롤러 등록이 영영 남는다 -- 이 콜백은 상위에 실패를 전할 수 없으므로
		 * 정리 책임이 전부 이 자리에 있다. */
		if (ret) {
			/* [한국어] [한국어] 실패 원인을 남긴다. 반환값은 버려지므로 이 로그가 유일한 단서다. */
			dev_err(dev, "Failed to power up MHI EP: %d\n", ret);
			/* [한국어] [한국어] eDMA 를 쓰는 SoC(sm8450/sa8775p)에서만 DMA 자원을 잡아 두었다.
			 * iATU 복사만 쓰는 sdx55 에서는 반납할 채널이 없다. */
			if (info->flags & MHI_EPF_USE_DMA)
				/* [한국어] [한국어] 워크큐를 비워 진행 중인 완료 콜백을 소진한 뒤 채널을 놓는다.
				 * 순서상 power_up 이 실패했으므로 새 전송이 들어올 일은 없다. */
				pci_epf_mhi_dma_deinit(epf_mhi);
			mhi_ep_unregister_controller(mhi_cntrl);
		}
	}

	return 0;
}

/* [한국어]
 * pci_epf_mhi_bind - EPF 가 EPC 에 묶일 때 MHI 레지스터 창과 도어벨 IRQ 를 확보한다
 *
 * @epf: configfs 로 EPC 에 바인딩된 이 EPF 인스턴스. epf->epc 는 이 시점에
 *       이미 채워져 있다(EPF 코어가 bind 를 부르기 전에 연결한다).
 * @return: 0 성공. -ENODEV 는 DT 에 "mmio" reg 가 없을 때, -ENOMEM 은
 *          ioremap 실패, 그 외 음수는 "doorbell" IRQ 조회 실패값 그대로.
 *          실패하면 EPF 코어가 바인딩을 취소하므로 unbind 는 불리지 않는다.
 *
 * 이 드라이버가 다루는 자원은 EPF 자신의 것이 아니라 **엔드포인트 컨트롤러
 * 하드웨어에 딸린 것**이다. MHI 레지스터 블록(MMIO)과 도어벨 인터럽트는
 * SoC 의 PCIe EP 컨트롤러 노드에 "mmio"/"doorbell" 이라는 이름으로 기술되어
 * 있고, 그 노드를 소유한 플랫폼 디바이스는 epc->dev.parent 다. 그래서
 * to_platform_device(epc->dev.parent) 로 거슬러 올라가 자원을 꺼낸다 --
 * EPF 는 가상 버스 위의 소프트웨어 객체라 자기 몫의 DT 노드가 없다.
 *
 * 단계:
 *   1) platform_get_resource_byname(IORESOURCE_MEM, "mmio") 으로 물리 주소
 *      범위를 얻어 phys_addr/size 에 저장한다. 이 값은 나중에 epc_init 에서
 *      그대로 BAR 뒤에 실려, 호스트가 BAR 를 읽으면 이 레지스터 블록이 보인다.
 *   2) ioremap 으로 같은 범위를 커널 가상 주소에 매핑한다. 호스트가 아니라
 *      **엔드포인트 쪽 MHI 코어**가 같은 레지스터를 직접 볼 통로다.
 *   3) platform_get_irq_byname(pdev, "doorbell") 로 호스트가 링에 항목을 넣고
 *      울리는 도어벨 인터럽트 번호를 받는다. 핸들러 등록은 여기서 하지 않고
 *      link_up 이 mhi_cntrl->irq 로 넘겨 MHI 코어가 맡는다.
 *
 * 에러 경로: IRQ 조회가 실패하면 방금 잡은 ioremap 을 직접 iounmap 한다.
 * 되감을 것이 하나뿐이라 goto 라벨 없이 인라인으로 처리한다.
 *
 * 실행 컨텍스트: configfs 에서 EPF 를 EPC 에 링크할 때의 프로세스 문맥.
 * epc_init/link_up 보다 반드시 먼저 실행된다.
 *
 * 호출 체인:
 *   configfs (pci-ep-cfs.c) → pci_epc_add_epf → epf->driver->ops->bind
 *     → [이 함수] → platform_get_resource_byname / ioremap / platform_get_irq_byname
 */
static int pci_epf_mhi_bind(struct pci_epf *epf)
{
	struct pci_epf_mhi *epf_mhi = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;
	/* [한국어] [한국어] "mmio"/"doorbell" 조회 실패를 알릴 로그 주체. */
	struct device *dev = &epf->dev;
	/* [한국어] [한국어] 자원의 실제 주인을 찾아 올라간다. EPF 는 가상 버스 위의 객체라
	 * 자기 DT 노드가 없고, MMIO 와 도어벨은 EPC 하드웨어 노드에 기술되어 있다.
	 * epc->dev.parent 가 바로 그 노드를 소유한 플랫폼 디바이스다. */
	struct platform_device *pdev = to_platform_device(epc->dev.parent);
	/* [한국어] [한국어] platform_get_resource_byname 이 돌려줄 메모리 자원 서술자. */
	struct resource *res;
	/* [한국어] [한국어] IRQ 번호(양수)와 오류(음수)를 함께 받는 변수. 그래서 아래에서
	 * !ret 이 아니라 ret < 0 으로 판정한다 -- 0 도 유효한 IRQ 일 수 있다. */
	int ret;

	/* Get MMIO base address from Endpoint controller */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mmio");
	if (!res) {
		/* [한국어] [한국어] DT 의 EP 컨트롤러 노드에 reg-names = "mmio" 항목이 없다는 뜻.
		 * 이 드라이버는 MHI 레지스터 블록 없이는 아무것도 못 하므로 곧바로 실패한다. */
		dev_err(dev, "Failed to get \"mmio\" resource\n");
		/* [한국어] [한국어] 장치가 이 드라이버에 맞지 않는다는 표준 코드. 아직 잡은 자원이
		 * 없으므로 되감을 것도 없다. */
		return -ENODEV;
	}

	epf_mhi->mmio_phys = res->start;
	/* [한국어] [한국어] BAR 크기로도 쓰일 값. epc_init 이 epf_bar->size 에 그대로 싣는다. */
	epf_mhi->mmio_size = resource_size(res);

	epf_mhi->mmio = ioremap(epf_mhi->mmio_phys, epf_mhi->mmio_size);
	/* [한국어] [한국어] ioremap 실패. 가상 주소 공간이 모자라거나 범위가 겹칠 때 NULL 이다. */
	if (!epf_mhi->mmio)
		/* [한국어] [한국어] 아직 IRQ 를 잡기 전이므로 되감을 것 없이 바로 빠진다. */
		return -ENOMEM;

	ret = platform_get_irq_byname(pdev, "doorbell");
	/* [한국어] [한국어] IRQ 조회 실패(-EPROBE_DEFER 포함). 음수만 오류로 본다. */
	if (ret < 0) {
		/* [한국어] [한국어] 바로 위에서 성공한 ioremap 을 직접 되돌린다. 되감을 자원이
		 * 하나뿐이라 goto 라벨 없이 인라인으로 처리한다. */
		iounmap(epf_mhi->mmio);
		return ret;
	}

	/* [한국어] [한국어] 도어벨 IRQ 번호를 인스턴스에 보관한다. 여기서 요청하지 않고
	 * 번호만 들고 있다가 link_up 이 mhi_cntrl->irq 로 넘기면, 핸들러 등록은
	 * MHI 코어의 mhi_ep_register_controller 가 맡는다 -- 인터럽트가 실제로
	 * 울리는 것은 호스트가 링에 항목을 넣은 뒤이므로 그때까지 미룬다. */
	epf_mhi->irq = ret;

	return 0;
}

/* [한국어]
 * pci_epf_mhi_unbind - EPF 를 EPC 에서 떼어낼 때 MHI 스택과 BAR 를 모두 되돌린다
 *
 * @epf: 떼어낼 EPF 인스턴스.
 * @return: 없음. 언바인드 경로에는 실패를 보고할 상위가 없으므로 void 다.
 *
 * bind 가 잡은 것(ioremap)과 epc_init/link_up 이 잡은 것(BAR, 컨트롤러 등록)을
 * 한자리에서 모두 되감는다. epc_deinit 이 이미 왔다면 mhi_dev 가 NULL 이라
 * 첫 블록은 통째로 건너뛰고, BAR 해제와 iounmap 만 수행된다.
 *
 * 상류 주석이 짚는 핵심: 여기서는 **강제로** 전원을 내린다. 그래서 이 EPF 를
 * 다시 bind 해도 스택이 저절로 살아나지 않고, 호스트가 Bus Master Enable 을
 * 다시 줘야(pci_epf_mhi_bus_master_enable) 비로소 동작 상태로 돌아온다.
 *
 * 되감기 순서가 중요하다:
 *   1) mhi_ep_power_down  -- 채널/이벤트 처리를 먼저 멈춘다. 이걸 미루면
 *      아래에서 DMA 채널을 반납하는 동안 MHI 코어가 그 채널을 쓸 수 있다.
 *   2) pci_epf_mhi_dma_deinit (eDMA 플랫폼만) -- 워크큐를 비우고 채널 반납.
 *   3) mhi_ep_unregister_controller -- 도어벨 IRQ 핸들러가 풀리고 mhi_dev 가
 *      정리된다.
 *   4) iounmap -- 이제 아무도 mmio 를 보지 않으므로 매핑을 푼다.
 *   5) pci_epc_clear_bar -- 호스트 쪽에서 BAR 를 지운다. epc_deinit 과 달리
 *      언바인드는 되돌아올 일이 없으므로 BAR 도 함께 닫는다.
 *
 * 실행 컨텍스트: configfs 언링크 또는 EPC 제거 시의 프로세스 문맥.
 *
 * 호출 체인:
 *   configfs / pci_epc_remove_epf → epf->driver->ops->unbind → [이 함수]
 *     → mhi_ep_power_down → pci_epf_mhi_dma_deinit
 *       → mhi_ep_unregister_controller → iounmap → pci_epc_clear_bar
 */
static void pci_epf_mhi_unbind(struct pci_epf *epf)
{
	struct pci_epf_mhi *epf_mhi = epf_get_drvdata(epf);
	const struct pci_epf_mhi_ep_info *info = epf_mhi->info;
	/* [한국어] [한국어] 닫아야 할 BAR 의 서술자. epc_init 이 열어 둔 바로 그 BAR 다. */
	struct pci_epf_bar *epf_bar = &epf->bar[info->bar_num];
	/* [한국어] [한국어] 강제 전원 차단과 등록 해제의 대상. */
	struct mhi_ep_cntrl *mhi_cntrl = &epf_mhi->mhi_cntrl;
	/* [한국어] [한국어] pci_epc_clear_bar 를 부를 컨트롤러. */
	struct pci_epc *epc = epf->epc;

	/*
	 * Forcefully power down the MHI EP stack. Only way to bring the MHI EP
	 * stack back to working state after successive bind is by getting Bus
	 * Master Enable event from host.
	 */
	if (mhi_cntrl->mhi_dev) {
		mhi_ep_power_down(mhi_cntrl);
		if (info->flags & MHI_EPF_USE_DMA)
			/* [한국어] [한국어] eDMA SoC 에서만 채널/워크큐를 반납한다. power_down 이 먼저 왔으므로
			 * 새 전송은 더 들어오지 않고, 여기서 남은 완료만 소진된다. */
			pci_epf_mhi_dma_deinit(epf_mhi);
		mhi_ep_unregister_controller(mhi_cntrl);
	}

	/* [한국어] [한국어] bind 가 잡은 커널 가상 매핑을 푼다. 위에서 컨트롤러 등록을
	 * 해제했으므로 이 시점에 mmio 를 보는 주체는 더 이상 없다. */
	iounmap(epf_mhi->mmio);
	/* [한국어] [한국어] 호스트 쪽에서 BAR 를 지운다. epc_deinit 과 달리 언바인드는
	 * 되돌아올 일이 없으므로 BAR 까지 함께 닫는다. func_no/vfunc_no 는
	 * SR-IOV 다중 함수 EPC 에서 어느 함수의 BAR 인지 고르는 좌표다. */
	pci_epc_clear_bar(epc, epf->func_no, epf->vfunc_no, epf_bar);
}

static const struct pci_epc_event_ops pci_epf_mhi_event_ops = {
	/* [한국어] [한국어] EPC 가 초기화될 때 BAR/MSI/설정공간 헤더를 세운다. */
	.epc_init = pci_epf_mhi_epc_init,
	/* [한국어] [한국어] EPC 가 내려갈 때 MHI 스택을 접고 BAR 를 닫는다. */
	.epc_deinit = pci_epf_mhi_epc_deinit,
	.link_up = pci_epf_mhi_link_up,
	.link_down = pci_epf_mhi_link_down,
	.bus_master_enable = pci_epf_mhi_bus_master_enable,
};

/* [한국어]
 * pci_epf_mhi_probe - EPF 인스턴스를 만들고 SoC 기술표를 붙인다
 *
 * @epf: configfs 에서 이름으로 만들어진 EPF 장치. 아직 EPC 에 묶이지 않았다.
 * @id: 이 EPF 이름과 맞아떨어진 pci_epf_device_id 항목. driver_data 에
 *      SoC별 pci_epf_mhi_ep_info 포인터가 kernel_ulong_t 로 실려 온다.
 * @return: 0 성공, -ENOMEM 인스턴스 할당 실패. 실패하면 EPF 코어가 이 장치의
 *          probe 를 실패로 처리하고 configfs 생성이 에러로 끝난다.
 *
 * pci_epf_bus_type 의 match 는 EPF 이름 문자열을 id_table 의 .name 과 견준다.
 * 그래서 사용자가 configfs 에서 "pci_epf_mhi_sm8450" 이라는 이름으로 EPF 를
 * 만들면 sm8450_info 가, "pci_epf_mhi_sa8775p" 로 만들면 sa8775p_info 가
 * 실려 온다 -- 같은 드라이버가 SoC별 BAR 번호/MSI 개수/MRU/DMA 사용 여부를
 * 오직 이 한 포인터로 갈아 끼우는 구조다.
 *
 * 여기서 하는 일은 전부 "메모리 위의 준비" 뿐이고, 하드웨어는 건드리지 않는다:
 *   - devm_kzalloc 으로 인스턴스 확보 (EPF 장치 수명에 묶여 자동 해제)
 *   - epf->header 에 SoC별 설정공간 헤더를 걸어 둔다. 실제 기록은 epc_init 의
 *     pci_epc_write_header() 가 한다.
 *   - info/epf 상호 포인터를 채워 두 방향 탐색이 되게 한다.
 *   - event_ops 를 걸어 EPC 코어가 init/deinit/link/BME 통지를 보낼 창구를 연다.
 *   - lock 뮤텍스 초기화. DMA 경로가 이 잠금으로 직렬화된다.
 *   - epf_set_drvdata 로 매달아, 이후 모든 콜백이 epf_get_drvdata 로 되찾는다.
 *
 * 실행 컨텍스트: configfs 에서 EPF 디렉터리를 만들 때의 프로세스 문맥.
 * bind 보다 먼저, 단 한 번 실행된다.
 *
 * 호출 체인:
 *   configfs mkdir → pci_epf_create → driver_register 매칭
 *     → driver->probe → [이 함수] → devm_kzalloc / mutex_init / epf_set_drvdata
 */
static int pci_epf_mhi_probe(struct pci_epf *epf,
			     const struct pci_epf_device_id *id)
{
	struct pci_epf_mhi_ep_info *info =
			(struct pci_epf_mhi_ep_info *)id->driver_data;
	struct pci_epf_mhi *epf_mhi;
	/* [한국어] [한국어] 이 단계의 로그 주체. probe 는 아직 EPC 를 모르므로 EPF 장치를 쓴다. */
	struct device *dev = &epf->dev;

	epf_mhi = devm_kzalloc(dev, sizeof(*epf_mhi), GFP_KERNEL);
	/* [한국어] [한국어] 인스턴스 할당 실패. devm 이므로 여기서만 실패를 보면 된다. */
	if (!epf_mhi)
		/* [한국어] [한국어] EPF 코어가 이 값을 보고 probe 를 실패로 처리한다. */
		return -ENOMEM;

	epf->header = info->epf_header;
	/* [한국어] [한국어] SoC 기술표를 인스턴스에 고정한다. 이후 모든 콜백이 BAR 번호,
	 * MSI 개수, MRU, DMA 사용 여부를 오직 이 포인터에서 읽는다. */
	epf_mhi->info = info;
	/* [한국어] [한국어] 역방향 포인터. mhi_cntrl 만 받는 MHI 콜백들이 container_of 로
	 * 인스턴스를 되찾은 뒤 여기서 다시 EPF 로 올라간다. */
	epf_mhi->epf = epf;

	epf->event_ops = &pci_epf_mhi_event_ops;
/* [한국어] [한국어] EPC 코어가 init/deinit/link_up/link_down/BME 를 통지할 창구.
 * 이걸 걸어 두지 않으면 링크 이벤트가 이 드라이버에 전혀 도달하지 않는다. */

	mutex_init(&epf_mhi->lock);

	epf_set_drvdata(epf, epf_mhi);
/* [한국어] [한국어] 인스턴스를 EPF 장치에 매단다. 이후 모든 콜백의 첫 줄이
 * epf_get_drvdata 로 이 포인터를 되찾는 것으로 시작한다. */

	return 0;
}

static const struct pci_epf_device_id pci_epf_mhi_ids[] = {
	/* [한국어] [한국어] sa8775p: eDMA 를 쓰는 자동차용 SoC. 사용자가 configfs 에서
	 * 이 이름으로 EPF 를 만들면 pci_epf_bus_type 의 match 가 이름을 견주어
	 * driver_data 의 기술표를 probe 에 실어 보낸다. */
	{ .name = "pci_epf_mhi_sa8775p", .driver_data = (kernel_ulong_t)&sa8775p_info },
	/* [한국어] [한국어] sdx55: DMA 엔진 없이 iATU 창 매핑 + memcpy 로만 동작하는 모뎀 SoC. */
	{ .name = "pci_epf_mhi_sdx55", .driver_data = (kernel_ulong_t)&sdx55_info },
	{ .name = "pci_epf_mhi_sm8450", .driver_data = (kernel_ulong_t)&sm8450_info },
	{},
};

static const struct pci_epf_ops pci_epf_mhi_ops = {
	/* [한국어] [한국어] 언바인드 시 MHI 스택을 접고 BAR/매핑을 되돌린다. */
	.unbind	= pci_epf_mhi_unbind,
	/* [한국어] [한국어] 바인드 시 "mmio" 자원과 "doorbell" IRQ 를 확보한다.
	 * EPF 코어는 bind/unbind 가 모두 있어야 등록을 받아 준다
	 * (pci-epf-core.c 의 __pci_epf_register_driver 검사). */
	.bind	= pci_epf_mhi_bind,
};

static struct pci_epf_driver pci_epf_mhi_driver = {
	/* [한국어] [한국어] configfs 에 나타날 드라이버 이름. 사용자는 이 이름 아래에서
	 * id_table 의 각 항목 이름으로 EPF 인스턴스를 만든다. */
	.driver.name	= "pci_epf_mhi",
	/* [한국어] [한국어] id_table 이름이 맞아떨어졌을 때 불릴 인스턴스 생성자. */
	.probe		= pci_epf_mhi_probe,
	.id_table	= pci_epf_mhi_ids,
	.ops		= &pci_epf_mhi_ops,
	.owner		= THIS_MODULE,
};

/* [한국어]
 * pci_epf_mhi_init - 모듈 적재 시 EPF 드라이버를 가상 버스에 등록한다
 *
 * @return: pci_epf_register_driver() 의 반환값 그대로. 음수면 모듈 적재가
 *          실패한다.
 *
 * pci_epf_register_driver() 는 __pci_epf_register_driver(drv, THIS_MODULE) 로
 * 펼쳐지는 매크로다(drivers/pci/endpoint/pci-epf-core.c:873). 그쪽에서
 * ops->bind/unbind 가 있는지 검사한 뒤 driver.bus 를 pci_epf_bus_type 으로
 * 정하고 driver_register() 를 부르며, 마지막에 pci_epf_add_cfs() 로 이 드라이버
 * 이름의 configfs 항목을 만들어 사용자가 EPF 를 생성할 수 있게 한다.
 *
 * 실행 컨텍스트: 모듈 적재 시각의 프로세스 문맥. 등록만 하고 하드웨어는
 * 건드리지 않는다 -- 실제 동작은 사용자가 configfs 로 EPF 를 만들고 EPC 에
 * 링크한 뒤에야 probe/bind 로 시작된다.
 *
 * 호출 체인:
 *   module_init → [이 함수] → pci_epf_register_driver
 *     → __pci_epf_register_driver → driver_register → pci_epf_add_cfs
 */
static int __init pci_epf_mhi_init(void)
{
	return pci_epf_register_driver(&pci_epf_mhi_driver);
}
module_init(pci_epf_mhi_init);

/* [한국어]
 * pci_epf_mhi_exit - 모듈 제거 시 EPF 드라이버 등록을 해제한다
 *
 * @return: 없음.
 *
 * pci_epf_unregister_driver() 가 configfs 항목을 걷어내고 driver_unregister()
 * 를 부른다. 그 과정에서 아직 살아 있는 EPF 인스턴스가 있으면 드라이버 코어가
 * 각각에 대해 unbind 를 몰아 주므로, MHI 스택 정리는 pci_epf_mhi_unbind 가
 * 맡는다 -- 이 함수 자체는 추가로 되감을 것이 없다.
 *
 * 참고: 이 드라이버가 struct pci_epf_driver 에 .owner = THIS_MODULE 을 따로
 * 두는 것은 형식적인 값이 아니다. EPF 코어가 바인딩 전후에
 * try_module_get(epf->driver->owner) / module_put(epf->driver->owner) 로
 * 이 값을 실제로 사용한다(pci-epf-core.c:200, :169). 덕분에 EPF 가 EPC 에
 * 묶여 있는 동안에는 이 모듈이 내려가지 않는다.
 *
 * 실행 컨텍스트: 모듈 제거 시각의 프로세스 문맥.
 *
 * 호출 체인:
 *   module_exit → [이 함수] → pci_epf_unregister_driver → driver_unregister
 */
static void __exit pci_epf_mhi_exit(void)
{
	pci_epf_unregister_driver(&pci_epf_mhi_driver);
}
module_exit(pci_epf_mhi_exit);

MODULE_DESCRIPTION("PCI EPF driver for MHI Endpoint devices");
MODULE_AUTHOR("Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>");
MODULE_LICENSE("GPL");
