# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the PCI bus specific drivers.
#
# ============================================================================
# [한국어 설명] PCI 버스 드라이버 빌드 청사진 (drivers/pci/Makefile)
#
# === 파일의 역할 ===
# 이 Makefile은 PCI/PCIe 서브시스템을 구성하는 오브젝트 파일들을 커널 빌드에
# 편입시키는 청사진이다. 어떤 .c 파일이 vmlinux 또는 모듈에 들어갈지를
# CONFIG_* 심볼에 따라 결정하므로, "PCIe 스택에 무엇이 존재하는가"의 최종
# 목록이 여기에 있다. 개별 기능의 활성 여부는 drivers/pci/Kconfig가 정하고,
# 이 파일은 그 결정을 실제 컴파일 단위로 번역한다.
# 하위 디렉토리(msi/, pcie/, hotplug/, controller/, endpoint/ 등)로의 재귀
# 진입도 여기서 선언되므로, PCI 트리 전체의 빌드 순서가 이 파일에 압축된다.
#
# === 전체 아키텍처에서의 위치 ===
# 빌드 전개 순서:
#   최상위 커널 Makefile -> drivers/Makefile -> [이 파일]
#     -> drivers/pci/msi/Makefile, pcie/Makefile, hotplug/Makefile,
#        controller/Makefile, endpoint/Makefile, pwrctrl/Makefile, switch/Makefile
# 런타임 관점에서 여기서 만들어진 오브젝트는 NVMe보다 먼저 초기화된다.
# probe.o가 PCI 버스를 열거해 struct pci_dev를 만들고, pci-driver.o가
# nvme_driver의 .probe(nvme_probe)를 호출해 주어야 비로소 NVMe 컨트롤러가
# 존재하기 시작한다. 즉 이 Makefile이 만드는 것은 NVMe의 "토대"다.
#
# === 타 모듈과의 연결 ===
# - drivers/pci/Kconfig: 여기 쓰인 모든 CONFIG_PCI_* 심볼의 정의처.
# - drivers/nvme/host/pci.c: 이 파일이 빌드한 심볼(pci_enable_device,
#   pci_alloc_irq_vectors, pci_iomap, pcie_capability_*)의 최대 소비자.
# - block layer: NVMe가 PCI로부터 얻은 DMA 마스크와 세그먼트 제약이
#   queue_limits로 흘러가 bio 분할 단위를 결정한다.
# 데이터 흐름: config space(access.o) -> 열거(probe.o) -> 리소스 배치
#   (setup-bus.o, setup-res.o) -> 드라이버 바인딩(pci-driver.o)
#   -> 인터럽트 할당(msi/) -> NVMe queue 생성.
#
# === 주요 함수/구조체 요약 ===
# - access.o    : pci_read_config_*/pci_write_config_* 및 이를 직렬화하는 pci_lock.
# - probe.o     : pci_scan_*/pci_setup_device — struct pci_dev 생성의 본산.
# - pci-driver.o: struct pci_driver 등록과 probe/remove 디스패치.
# - pci.o       : pci_enable_device, BAR/전원 상태 제어 등 범용 API 집합.
# - setup-bus.o : 브리지 윈도우와 BAR 주소 배치(리소스 할당) 알고리즘.
# ============================================================================

# [한국어] PCI 코어 오브젝트 묶음. CONFIG_PCI 하나로 전부 따라 들어오며,
# 이 12개가 없으면 struct pci_dev 자체가 존재할 수 없다. 줄 연속(\)으로
# 이어진 하나의 논리 행이라 각 항목에 행말 주석을 붙일 수 없어 여기에 정리한다.
#   access.o      : config space 읽기/쓰기와 pci_lock 직렬화. NVMe의 Vendor/Device
#                   ID, BAR0, MSI-X capability 조회가 모두 이 경로를 지난다.
#   bus.o         : struct pci_bus 관리와 버스 리소스 목록.
#   probe.o       : 버스 열거. NVMe SSD를 발견해 pci_dev로 만드는 지점.
#   host-bridge.o : 호스트 브리지 추상화 — CPU 물리주소와 PCI 버스주소의 변환 근거.
#   remove.o      : 장치 제거 경로. NVMe 핫언플러그 시 nvme_remove()가 여기서 불린다.
#   pci.o         : pci_enable_device/pci_set_master 등 드라이버가 쓰는 범용 API.
#   pci-driver.o  : pci_register_driver와 probe/remove 디스패치. nvme_driver 등록처.
#   search.o      : pci_get_domain_bus_and_slot 등 장치 탐색 헬퍼.
#   rebar.o       : Resizable BAR. NVMe CMB(Controller Memory Buffer) 크기 조정에 관여.
#   rom.o         : 옵션 ROM 매핑. NVMe 부트 지원(EFI/Legacy)과 연관.
#   setup-res.o   : 개별 BAR에 물리 주소를 배정.
#   irq.o         : 레거시 INTx 인터럽트 처리.
#   vpd.o         : Vital Product Data — 벤더 정보 영역 접근.
#   setup-bus.o   : 브리지 윈도우 계산과 리소스 트리 배치.
#   vc.o          : Virtual Channel capability — PCIe QoS/TC 매핑.
#   mmap.o        : BAR의 유저스페이스 mmap 지원.
#   devres.o      : devm_* 형태의 관리형 리소스. 실패 경로 정리를 자동화한다.
obj-$(CONFIG_PCI)		+= access.o bus.o probe.o host-bridge.o \
				   remove.o pci.o pci-driver.o search.o \
				   rebar.o rom.o setup-res.o irq.o vpd.o \
				   setup-bus.o vc.o mmap.o devres.o

obj-$(CONFIG_PCI)		+= msi/		# [한국어] MSI/MSI-X 하위 디렉토리. NVMe가 I/O 큐마다 벡터 하나를 받는 pci_alloc_irq_vectors()의 구현처
obj-$(CONFIG_PCI)		+= pcie/	# [한국어] PCIe 전용 서비스(AER/DPC/ASPM/PME/포트 드라이버). NVMe 오류 복구와 전력 상태 전환의 상위 계층
obj-$(CONFIG_PCI)		+= pwrctrl/	# [한국어] 슬롯 전원 시퀀서. 규제기/클럭을 켜야 링크가 올라오는 보드에서 NVMe 검출의 선행 조건

# [한국어] 아래 4개는 CONFIG_PCI가 켜졌을 때만 의미가 있으므로 ifdef로 감싼다.
# PCI 없이 PROC_FS/SYSFS만 켜진 구성에서 무의미한 오브젝트가 링크되는 것을 막는다.
ifdef CONFIG_PCI
obj-$(CONFIG_PROC_FS)		+= proc.o	# [한국어] /proc/bus/pci 인터페이스. lspci의 레거시 조회 경로
obj-$(CONFIG_SYSFS)		+= pci-sysfs.o slot.o	# [한국어] /sys/bus/pci/devices/*/ 속성과 슬롯 노드. NVMe의 resource0, numa_node, d3cold_allowed 노출처
obj-$(CONFIG_ACPI)		+= pci-acpi.o	# [한국어] ACPI 연동. NVMe의 _DSM 기반 전원 관리와 D3cold 진입 가부를 펌웨어에 질의
obj-$(CONFIG_GENERIC_PCI_IOMAP) += iomap.o	# [한국어] pci_iomap() 범용 구현. NVMe BAR0(레지스터 창) 매핑에 사용
endif	# [한국어] ifdef CONFIG_PCI 블록 종료

obj-$(CONFIG_OF)		+= of.o		# [한국어] 디바이스 트리 연동. 임베디드 보드에서 PCIe 노드와 IRQ 매핑을 DT로 기술할 때
obj-$(CONFIG_PCI_QUIRKS)	+= quirks.o	# [한국어] 하드웨어 결함 우회 모음. 특정 NVMe/스위치의 DMA alias, ACS, MSI 오작동 보정이 여기에 있다
obj-$(CONFIG_HOTPLUG_PCI)	+= hotplug/	# [한국어] 핫플러그 컨트롤러. U.2/EDSFF NVMe의 표면 삽입/제거 이벤트 처리
obj-$(CONFIG_PCI_ATS)		+= ats.o	# [한국어] Address Translation Service. 장치가 IOMMU 변환을 캐시하는 기능으로, SR-IOV/PASID의 전제
obj-$(CONFIG_PCI_IOV)		+= iov.o	# [한국어] SR-IOV. NVMe 컨트롤러를 VF로 쪼개 게스트에 직접 할당하는 가상화 경로
obj-$(CONFIG_PCI_BRIDGE_EMUL)	+= pci-bridge-emul.o	# [한국어] 하드웨어에 실제 브리지 config space가 없는 컨트롤러용 소프트웨어 에뮬레이션
obj-$(CONFIG_PCI_LABEL)		+= pci-label.o	# [한국어] SMBIOS/ACPI 기반 슬롯 라벨. 다수 NVMe 장착 시 물리 슬롯 식별에 사용
obj-$(CONFIG_X86_INTEL_MID)	+= pci-mid.o	# [한국어] Intel MID 계열 SoC 전용 처리. 서버 NVMe 경로와는 무관
obj-$(CONFIG_PCI_SYSCALL)	+= syscall.o	# [한국어] pciconfig_read/write 시스템 콜. setpci 계열 도구가 NVMe config space를 직접 만질 때
obj-$(CONFIG_PCI_STUB)		+= pci-stub.o	# [한국어] 아무것도 하지 않는 드라이버. NVMe를 호스트에서 떼어 VFIO 패스스루로 넘길 때 자리를 잡아둔다
obj-$(CONFIG_PCI_PF_STUB)	+= pci-pf-stub.o	# [한국어] SR-IOV PF 전용 stub. VF만 게스트에 주고 PF는 유휴로 둘 때
obj-$(CONFIG_PCI_ECAM)		+= ecam.o	# [한국어] Enhanced Configuration Access Mechanism. MMIO로 config space 전체를 평면 매핑하는 방식
obj-$(CONFIG_PCI_P2PDMA)	+= p2pdma.o	# [한국어] Peer-to-Peer DMA. NVMe CMB를 DMA 목적지로 삼아 host memory를 우회하는 경로의 핵심
obj-$(CONFIG_XEN_PCIDEV_FRONTEND) += xen-pcifront.o	# [한국어] Xen 반가상화 프론트엔드. 게스트가 PCI 장치를 하이퍼바이저 경유로 볼 때
obj-$(CONFIG_VGA_ARB)		+= vgaarb.o	# [한국어] VGA 자원 중재. NVMe와 무관하나 레거시 IO 자원 충돌 조정에 필요
obj-$(CONFIG_PCI_DOE)		+= doe.o	# [한국어] Data Object Exchange. CMA/SPDM 기반 장치 인증 메시지를 mailbox로 주고받는다
obj-$(CONFIG_PCI_IDE)		+= ide.o	# [한국어] Integrity and Data Encryption(PCIe 링크 암호화). 기밀 컴퓨팅용 NVMe 보호 경로
obj-$(CONFIG_PCI_TSM)		+= tsm.o	# [한국어] TEE Security Manager 연동. 신뢰 실행 환경에 장치를 안전하게 귀속시킨다
obj-$(CONFIG_PCI_DYNAMIC_OF_NODES) += of_property.o	# [한국어] 열거된 PCI 장치에 대응하는 DT 노드를 런타임 생성
obj-$(CONFIG_PCI_NPEM)		+= npem.o	# [한국어] Native PCIe Enclosure Management. NVMe 백플레인의 LED 표시 제어
obj-$(CONFIG_PCIE_TPH)		+= tph.o	# [한국어] TLP Processing Hints. DMA 쓰기의 캐시 배치를 힌트로 주어 NVMe 완료 지연을 줄인다
obj-$(CONFIG_CARDBUS)		+= setup-cardbus.o	# [한국어] CardBus 브리지 리소스 설정. 레거시 노트북 전용

# Endpoint library must be initialized before its users
# [한국어] 엔드포인트 프레임워크는 이를 사용하는 기능 드라이버(pci-epf-*)보다
# 먼저 초기화돼야 하므로, controller/ 앞에 의도적으로 배치돼 있다.
# 링크 순서가 곧 initcall 순서이므로 이 위치 자체가 의미를 갖는다.
obj-$(CONFIG_PCI_ENDPOINT)	+= endpoint/	# [한국어] 리눅스가 PCIe 엔드포인트(장치측)로 동작하는 모드. 호스트측 NVMe 경로와는 반대 방향

obj-y				+= controller/	# [한국어] 호스트 브리지/루트 컴플렉스 드라이버들. NVMe가 꽂히는 링크를 물리적으로 올리는 계층
obj-y				+= switch/	# [한국어] PCIe 스위치 관리 드라이버(switchtec). NVMe 다단 팬아웃 토폴로지의 상단

subdir-ccflags-$(CONFIG_PCI_DEBUG) := -DDEBUG	# [한국어] PCI 트리 전체에 -DDEBUG를 전파해 pr_debug()를 활성화. 열거/리소스 배치 실패 추적용

CFLAGS_trace.o := -I$(src)			# [한국어] trace.o만 소스 디렉토리를 include 경로에 추가. TRACE_INCLUDE_PATH가 상대 경로로 헤더를 되찾게 하는 관례
obj-$(CONFIG_TRACING)		+= trace.o	# [한국어] PCI 트레이스포인트 정의. config space 접근을 ftrace로 관측할 때
