# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the PCI/MSI
#
# ============================================================================
# [한국어 설명] PCI MSI/MSI-X 서브시스템 빌드 (drivers/pci/msi/Makefile)
#
# === 파일의 역할 ===
# 메시지 기반 인터럽트(MSI/MSI-X)를 구성하는 오브젝트를 빌드한다. NVMe는
# 큐 쌍마다 완료 인터럽트를 하나씩 받으므로 수십~수백 개의 벡터를 요구하며,
# 이는 4개로 제한된 레거시 INTx로는 불가능하다. 그래서 NVMe에게 이 디렉토리는
# 선택 기능이 아니라 성능의 전제 조건이다.
#
# === 전체 아키텍처에서의 위치 ===
# drivers/pci/Makefile -> [이 파일]
# 호출 체인: nvme_setup_io_queues() -> pci_alloc_irq_vectors_affinity()
#   -> (api.o) -> __pci_enable_msix_range() -> (msi.o) -> msi_domain_alloc_irqs
#   -> (irqdomain.o) -> 아키텍처 IRQ 도메인 -> 실제 벡터 배정
#
# === 타 모듈과의 연결 ===
# - drivers/nvme/host/pci.c: 최대 소비자. 벡터 수가 곧 I/O 큐 개수 상한이다.
# - kernel/irq/: 상위 irqdomain 프레임워크. 여기서 실제 벡터를 얻는다.
# - drivers/pci/quirks.c: MSI가 고장난 장치를 걸러내는 보정이 존재한다.
#
# === 주요 함수/구조체 요약 ===
# - pcidev_msi.o: CONFIG_PCI만으로도 필요한 최소 MSI 상태 초기화.
# - api.o       : pci_alloc_irq_vectors 등 드라이버가 직접 부르는 공개 API.
# - msi.o       : capability 파싱, MSI-X 테이블 매핑, 마스크/언마스크 구현.
# - irqdomain.o : PCI 장치를 irqdomain 계층에 연결하고 RID를 산출.
# ============================================================================
obj-$(CONFIG_PCI)			+= pcidev_msi.o	# [한국어] MSI를 끄고 빌드해도 필요한 최소 초기화. pci_dev의 msi 관련 필드를 안전한 기본값으로 둔다
obj-$(CONFIG_PCI_MSI)			+= api.o msi.o irqdomain.o	# [한국어] MSI/MSI-X 본체 3종. NVMe의 pci_alloc_irq_vectors()가 이 셋을 차례로 지난다
obj-$(CONFIG_PCI_MSI_ARCH_FALLBACKS)	+= legacy.o	# [한국어] irqdomain을 갖추지 못한 구형 아키텍처용 폴백. arch_setup_msi_irq() 계열 훅
