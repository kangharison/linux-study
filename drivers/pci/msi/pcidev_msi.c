// SPDX-License-Identifier: GPL-2.0
/*
 * MSI[X} related functions which are available unconditionally.
 */
/*
 * [한국어 설명] 장치 발견 직후 MSI/MSI-X 를 무조건 꺼 두는 초기화 (pcidev_msi.c)
 *
 * === 파일의 역할 ===
 * 커널이 PCI 장치를 새로 발견했을 때 가장 먼저 하는 MSI 관련 작업이 여기 있다.
 * 함수는 단 둘 - pci_msi_init() 과 pci_msix_init() - 이고, 각각 하는 일도
 * 두 가지뿐이다. capability 가 어디 있는지 찾아 struct pci_dev 에 기록하고,
 * 그 capability 의 Enable 비트를 강제로 끈다.
 *
 * 왜 끄는가. 원문 주석이 "screaming interrupt" 라고 부르는 상황을 막기 위해서다.
 * 전원을 켜면 MSI/MSI-X Enable 은 0 이 기본값이라 보통은 이미 꺼져 있다.
 * 하지만 다음 경우에는 켜진 채로 커널에 넘어온다.
 *   - 펌웨어(UEFI)가 자기 드라이버로 그 장치를 쓰다가 끄지 않고 넘긴 경우
 *   - kexec 로 새 커널을 띄운 경우. 이전 커널이 설정해 둔 상태가 그대로 남는다.
 *   - 리셋이 제대로 걸리지 않은 장치
 * 이때 장치가 인터럽트를 보내면, 아직 아무 드라이버도 그 벡터를 등록하지
 * 않았으므로 커널은 처리할 핸들러를 찾지 못한다. 인터럽트 원인이 해소되지
 * 않으니 장치는 계속 보내고, 결국 그 CPU 가 인터럽트 처리에만 매달려
 * 부팅이 멈춘다. 그래서 아무것도 묻지 않고 일단 끈다.
 *
 * 이 파일이 msi.c 와 분리돼 있는 이유는 파일 맨 위 원문 주석대로
 * "unconditionally available" 이어야 하기 때문이다. CONFIG_PCI_MSI 가 꺼진
 * 커널에서도 이 초기화는 실행되어야 한다 - MSI 를 쓰지 않을수록 켜진 채로
 * 남은 MSI 가 더 위험하기 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 장치 열거
 *   pci_scan_single_device() -> pci_device_add() -> pci_init_capabilities()
 *     -> [이 파일] pci_msi_init(), pci_msix_init()
 *   (이 시점에는 드라이버가 아직 바인딩되지 않았다)
 * ... 한참 뒤 ...
 *   driver->probe() -> pci_alloc_irq_vectors() -> msi/api.c -> msi/msi.c
 *     여기서 비로소 Enable 비트가 다시 켜진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 부팅 중의 버스 스캔, 또는 hotplug 로
 * 장치가 꽂혔을 때의 재스캔 경로에서 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/probe.c 의 pci_init_capabilities().
 * 아래쪽: ../pci.h 의 config space 접근 함수(pci_read_config_word 등).
 * 공유 상태: struct pci_dev 의 msi_cap / msix_cap 필드. 이 파일이 채워 넣는
 *   이 값을 이후 msi.c 와 api.c 가 계속 참조한다. 0 이면 "이 장치에는 해당
 *   capability 가 없다" 는 뜻이고, pci_msi_supported() 등이 그것으로 판단한다.
 * 데이터 흐름: config space -> pci_dev 필드(한 번 읽고 캐시) -> 이후 모든
 *   MSI 코드가 이 캐시된 오프셋을 기준으로 접근한다.
 *
 * === NVMe 관점 ===
 * NVMe SSD 도 예외 없이 이 경로를 지난다. 커널이 PCIe 버스에서 NVMe
 * 컨트롤러를 발견하면 pci_init_capabilities() 가 이 파일의 두 함수를 부르고,
 * 그때 dev->msix_cap 에 MSI-X capability 의 오프셋이 기록된다. 이 값이
 * 없으면(=0) 나중에 nvme_pci_enable() 이 부르는 pci_alloc_irq_vectors() 가
 * MSI-X 를 시도조차 하지 못하고 MSI 나 INTx 로 내려간다.
 *
 * 재부팅 없이 커널을 바꾸는 kexec 환경에서 특히 의미가 있다. 앞 커널이
 * NVMe 의 MSI-X 를 수십 개 벡터로 켜 둔 상태로 crash dump 커널이 뜨면,
 * 그 벡터들이 그대로 살아 있어 dump 커널이 부팅 중에 인터럽트 폭풍을 맞는다.
 * 이 파일이 그것을 첫 단계에서 차단한다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_msi_init(dev)  : MSI capability(ID 0x05) 를 찾아 dev->msi_cap 에 기록하고,
 *                      Message Control 의 MSI Enable 비트(0번)를 0 으로 만든다.
 *                      찾지 못하면 dev->msi_cap 은 0 으로 남는다.
 * pci_msix_init(dev) : MSI-X capability(ID 0x11) 를 찾아 dev->msix_cap 에 기록하고,
 *                      Message Control 의 MSI-X Enable 비트(15번)를 0 으로 만든다.
 * 두 함수 모두 반환값이 없다. capability 가 없는 것은 오류가 아니라 정상이고,
 * 호출자가 확인할 것도 없기 때문이다.
 */
#include "../pci.h"		/* NVMe: PCI 서브시스템 내부 헤더, pci_dev, PCI capability 매크로 등 포함 */

/*
 * Disable the MSI[X] hardware to avoid screaming interrupts during boot.
 * This is the power on reset default so usually this should be a noop.
 */
/* NVMe: 부팅/장치 탐색 시 MSI/MSI-X 하드웨어를 끄는 공통 함수들의 시작 부분 */

/* NVMe: NVMe SSD를 포함한 PCI 장치의 MSI Capability 초기화 함수 */
void pci_msi_init(struct pci_dev *dev)
{
	u16 ctrl;			/* NVMe: MSI Message Control 레지스터(16bit)를 담을 지역 변수 */

	/* NVMe: Configuration Space에서 MSI Capability의 오프셋을 찾아 저장 */
	dev->msi_cap = pci_find_capability(dev, PCI_CAP_ID_MSI);
	if (!dev->msi_cap)		/* NVMe: MSI Capability가 없으면(예: 일부 레거시 장치) */
		return;			/* NVMe: 더 이상 할 일이 없으므로 함수 종료 */

	/* NVMe: MSI Capability의 Message Control 레지스터를 ECAM으로 읽어옴 */
	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &ctrl);
	if (ctrl & PCI_MSI_FLAGS_ENABLE) {	/* NVMe: MSI Enable 비트가 켜져 있다면 */
		/* NVMe: Enable 비트만 클리어하여 MSI 인터럽트를 금지함 */
		pci_write_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS,
				      ctrl & ~PCI_MSI_FLAGS_ENABLE);
	}

	/* NVMe: 64bit 주소 지원 플래그가 설정되어 있지 않은 경우 */
	if (!(ctrl & PCI_MSI_FLAGS_64BIT))
		/* NVMe: DMA 주소를 32bit로 마스크(MSI 메시지 주소도 32bit 제한) */
		dev->msi_addr_mask = DMA_BIT_MASK(32);
}

/* NVMe: NVMe SSD에서 필수적으로 사용하는 MSI-X Capability 초기화 함수 */
void pci_msix_init(struct pci_dev *dev)
{
	u16 ctrl;			/* NVMe: MSI-X Message Control 레지스터(16bit)를 담을 지역 변수 */

	/* NVMe: Configuration Space에서 MSI-X Capability의 오프셋을 찾아 저장 */
	dev->msix_cap = pci_find_capability(dev, PCI_CAP_ID_MSIX);
	if (!dev->msix_cap)		/* NVMe: MSI-X Capability가 없으면 */
		return;			/* NVMe: NVMe에서도 MSI-X 미지원 시 MSI 폴스백을 위해 종료 */

	/* NVMe: MSI-X Capability의 Message Control 레지스터를 ECAM으로 읽어옴 */
	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &ctrl);
	if (ctrl & PCI_MSIX_FLAGS_ENABLE) {	/* NVMe: MSI-X Enable 비트가 켜져 있다면 */
		/* NVMe: Enable 비트만 클리어하여 MSI-X 인터럽트를 금지함 */
		pci_write_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS,
				      ctrl & ~PCI_MSIX_FLAGS_ENABLE);
	}
}
