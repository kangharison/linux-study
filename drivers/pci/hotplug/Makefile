# SPDX-License-Identifier: GPL-2.0
#
# ============================================================================
# [한국어 설명] PCI 핫플러그 컨트롤러 드라이버 빌드 (hotplug/Makefile)
#
# === 파일의 역할 ===
# 동작 중인 시스템에서 PCI/PCIe 장치를 물리적으로 넣고 빼는 것을 관장하는
# 컨트롤러 드라이버들을 빌드한다. 슬롯의 전원, 표시등, 삽입/제거 버튼과
# presence detect 신호를 다루며, 실제 열거와 제거는 drivers/pci/probe.c 및
# remove.c에 위임한다. 이 디렉토리는 "언제 열거할지"를 결정하는 층이다.
#
# === 전체 아키텍처에서의 위치 ===
# drivers/pci/Makefile -> [이 파일]
# U.2/EDSFF NVMe를 서버 전면에서 뽑았을 때의 흐름:
#   슬롯 하드웨어 인터럽트 -> pciehp_ist() (pciehp_hpc.o)
#     -> pciehp_handle_presence_or_link_change() (pciehp_ctrl.o)
#       -> pciehp_unconfigure_device() (pciehp_pci.o)
#         -> pci_stop_and_remove_bus_device() -> nvme_remove()
# 반대로 삽입 시에는 같은 경로가 pci_scan_slot() -> nvme_probe()로 이어진다.
#
# === 타 모듈과의 연결 ===
# - drivers/pci/hotplug/Kconfig: 여기 CONFIG 심볼 정의처.
# - drivers/pci/probe.c / remove.c: 실제 장치 생성과 파괴를 담당.
# - drivers/pci/pcie/portdrv.c: pciehp는 포트 서비스로 등록되어 IRQ를 받는다.
# - drivers/nvme/host/core.c: nvme_remove()가 네임스페이스를 정리하고
#   진행 중인 I/O를 실패시키는 최종 소비자.
#
# === 주요 함수/구조체 요약 ===
# - pci_hotplug.o: 코어 프레임워크. struct hotplug_slot 등록과 sysfs 노출.
# - pciehp.o     : PCIe 표준 네이티브 핫플러그. 현대 NVMe 서버의 주 경로.
# - acpiphp.o    : 펌웨어(ACPI) 주도 핫플러그. 네이티브가 없을 때의 폴백.
# - shpchp.o     : 구형 병렬 PCI용 Standard Hot-Plug Controller.
# ============================================================================
#
# Makefile for the Linux kernel pci hotplug controller drivers.
#

obj-$(CONFIG_HOTPLUG_PCI)		+= pci_hotplug.o	# [한국어] 핫플러그 코어. 아래 pci_hotplug-objs가 실제 구성 오브젝트를 지정한다
obj-$(CONFIG_HOTPLUG_PCI_COMPAQ)	+= cpqphp.o	# [한국어] Compaq 서버용 구형 컨트롤러
obj-$(CONFIG_HOTPLUG_PCI_IBM)		+= ibmphp.o	# [한국어] IBM 서버용 구형 컨트롤러

# native drivers should be linked before acpiphp in order to allow the
# native driver to attempt to bind first. We can then fall back to
# generic support.

obj-$(CONFIG_HOTPLUG_PCI_PCIE)		+= pciehp.o	# [한국어] PCIe 네이티브 핫플러그. NVMe 서버의 U.2/EDSFF 베이가 쓰는 표준 경로. 위 주석대로 acpiphp보다 먼저 링크돼 우선 바인딩을 시도한다
obj-$(CONFIG_HOTPLUG_PCI_CPCI_ZT5550)	+= cpcihp_zt5550.o	# [한국어] CompactPCI ZT5550 섀시 컨트롤러
obj-$(CONFIG_HOTPLUG_PCI_CPCI_GENERIC)	+= cpcihp_generic.o	# [한국어] CompactPCI 범용 컨트롤러
obj-$(CONFIG_HOTPLUG_PCI_SHPC)		+= shpchp.o	# [한국어] 구형 병렬 PCI 표준 핫플러그 컨트롤러
obj-$(CONFIG_HOTPLUG_PCI_POWERNV)	+= pnv-php.o	# [한국어] IBM PowerNV 플랫폼 핫플러그
obj-$(CONFIG_HOTPLUG_PCI_RPA)		+= rpaphp.o	# [한국어] IBM pSeries RPA(논리 파티션) 핫플러그
obj-$(CONFIG_HOTPLUG_PCI_RPA_DLPAR)	+= rpadlpar_io.o	# [한국어] RPA 동적 논리 파티션(DLPAR) 슬롯 추가/제거
obj-$(CONFIG_HOTPLUG_PCI_ACPI)		+= acpiphp.o	# [한국어] ACPI 주도 핫플러그. 펌웨어가 슬롯을 소유한 플랫폼용 폴백
obj-$(CONFIG_HOTPLUG_PCI_S390)		+= s390_pci_hpc.o	# [한국어] s390 zPCI 핫플러그. 하이퍼바이저가 장치를 붙였다 뗀다
obj-$(CONFIG_HOTPLUG_PCI_OCTEONEP)	+= octep_hp.o	# [한국어] Marvell OCTEON 엔드포인트 핫플러그

# acpiphp_ibm extends acpiphp, so should be linked afterwards.

obj-$(CONFIG_HOTPLUG_PCI_ACPI_AMPERE_ALTRA)	+= acpiphp_ampere_altra.o	# [한국어] Ampere Altra 서버의 표시등 제어 확장
obj-$(CONFIG_HOTPLUG_PCI_ACPI_IBM)	+= acpiphp_ibm.o	# [한국어] IBM 전용 acpiphp 확장. 위 주석대로 acpiphp를 확장하므로 그 뒤에 링크돼야 한다

pci_hotplug-objs	:=	pci_hotplug_core.o	# [한국어] 코어 모듈의 기본 구성. 아래 조건부 블록이 여기에 오브젝트를 덧붙인다

# [한국어] CompactPCI 핫플러그가 켜진 구성에서만 코어에 두 오브젝트를 더한다.
#   cpci_hotplug_core.o: CompactPCI 섀시 슬롯의 상태 기계.
#   cpci_hotplug_pci.o : 해당 슬롯의 장치 구성/해제를 PCI 코어에 위임.
ifdef CONFIG_HOTPLUG_PCI_CPCI	# [한국어] CompactPCI 지원이 켜진 구성에서만 코어에 두 오브젝트를 더한다
pci_hotplug-objs	+=	cpci_hotplug_core.o	\
				cpci_hotplug_pci.o	# [한국어] CompactPCI 슬롯의 장치 구성/해제를 PCI 코어에 위임하는 절반
endif	# [한국어] 위 ifdef 블록 종료
ifdef CONFIG_ACPI	# [한국어] ACPI 부팅 경로에서만 아래 강제 빌드가 의미를 갖는다
pci_hotplug-objs	+=	acpi_pcihp.o	# [한국어] ACPI가 켜진 구성에서만 펌웨어 협상(_OSC) 코드를 코어에 포함. NVMe 슬롯 제어권을 OS가 가져올지 여기서 정해진다
endif	# [한국어] 위 ifdef 블록 종료

# [한국어] Compaq 컨트롤러 모듈 구성. 줄 연속(\)으로 묶인 논리 행이라
# 각 항목에 행말 주석을 붙일 수 없어 여기에 정리한다.
#   cpqphp_core.o : 드라이버 등록과 슬롯 열거.
#   cpqphp_ctrl.o : 삽입/제거 이벤트 상태 기계.
#   cpqphp_sysfs.o: 슬롯 정보를 sysfs로 노출.
#   cpqphp_pci.o  : 장치 구성과 리소스 배치를 PCI 코어에 연결.
cpqphp-objs		:=	cpqphp_core.o	\
				cpqphp_ctrl.o	\
				cpqphp_sysfs.o	\
				cpqphp_pci.o
cpqphp-$(CONFIG_HOTPLUG_PCI_COMPAQ_NVRAM) += cpqphp_nvram.o	# [한국어] Compaq NVRAM 슬롯 설정 저장 지원(선택)
cpqphp-objs += $(cpqphp-y)	# [한국어] 위 조건부 선택분을 최종 오브젝트 목록에 합류시킨다

# [한국어] IBM 컨트롤러 모듈 구성.
#   ibmphp_core.o: 드라이버 등록과 슬롯 관리.
#   ibmphp_ebda.o: EBDA(펌웨어 데이터 영역)에서 슬롯 배치 정보를 읽는다.
#   ibmphp_pci.o : 장치 구성/해제.
#   ibmphp_res.o : 슬롯별 리소스 풀 관리.
#   ibmphp_hpc.o : 컨트롤러 하드웨어와의 실제 통신(폴링 스레드 포함).
ibmphp-objs		:=	ibmphp_core.o	\
				ibmphp_ebda.o	\
				ibmphp_pci.o	\
				ibmphp_res.o	\
				ibmphp_hpc.o

# [한국어] ACPI 핫플러그 모듈 구성.
#   acpiphp_core.o: hotplug_slot 등록과 sysfs 연결.
#   acpiphp_glue.o: ACPI 네임스페이스를 훑어 슬롯을 찾고, 펌웨어 통지를
#                   받아 pci_scan/remove로 옮기는 실제 접착층.
acpiphp-objs		:=	acpiphp_core.o	\
				acpiphp_glue.o

pnv-php-objs		:=	pnv_php.o	# [한국어] PowerNV 핫플러그 모듈 구성. 소스 하나짜리라 모듈명과 파일명만 이어 준다

# [한국어] IBM pSeries RPA 핫플러그 모듈 구성.
#   rpaphp_core.o: 드라이버 등록과 OF 노드 기반 슬롯 열거.
#   rpaphp_pci.o : 슬롯 하위 PCI 장치 구성/해제.
#   rpaphp_slot.o: struct slot 할당과 sysfs 등록.
rpaphp-objs		:=	rpaphp_core.o	\
				rpaphp_pci.o	\
				rpaphp_slot.o

# [한국어] RPA 동적 논리 파티션 슬롯 조작 모듈 구성.
#   rpadlpar_core.o : 파티션에 슬롯을 추가/제거하는 본체.
#   rpadlpar_sysfs.o: 유저스페이스가 조작을 지시하는 sysfs 인터페이스.
rpadlpar_io-objs	:=	rpadlpar_core.o \
				rpadlpar_sysfs.o

# [한국어] PCIe 네이티브 핫플러그 모듈 구성. NVMe 서버에서 실제로 도는 조합이다.
#   pciehp_core.o: 포트 서비스 드라이버 등록과 슬롯 초기화.
#   pciehp_ctrl.o: presence/link 변화에 따른 상태 기계와 재시도 정책.
#   pciehp_pci.o : pci_scan_slot / pci_stop_and_remove_bus_device 호출부.
#                  NVMe의 probe/remove가 실제로 촉발되는 지점.
#   pciehp_hpc.o : Slot Control/Status 레지스터 조작과 인터럽트 처리.
pciehp-objs		:=	pciehp_core.o	\
				pciehp_ctrl.o	\
				pciehp_pci.o	\
				pciehp_hpc.o

# [한국어] 구형 병렬 PCI 표준 핫플러그 모듈 구성. 역할 분담은 pciehp와 대응된다.
#   shpchp_core.o : 드라이버 등록과 슬롯 초기화.
#   shpchp_ctrl.o : 삽입/제거 상태 기계.
#   shpchp_pci.o  : 장치 구성/해제.
#   shpchp_sysfs.o: 슬롯 정보 노출.
#   shpchp_hpc.o  : 컨트롤러 레지스터 접근과 인터럽트 처리.
shpchp-objs		:=	shpchp_core.o	\
				shpchp_ctrl.o	\
				shpchp_pci.o	\
				shpchp_sysfs.o	\
				shpchp_hpc.o
