# SPDX-License-Identifier: GPL-2.0
#
# Makefile for PCI Express features and port driver
#
# ============================================================================
# [한국어 설명] PCIe 포트 서비스와 고급 기능 빌드 (drivers/pci/pcie/Makefile)
#
# === 파일의 역할 ===
# PCIe 스펙이 정의한 포트 서비스(AER, DPC, PME, PTM, EDR, 대역폭 알림)와
# 전력 관리(ASPM)를 빌드한다. 이들은 특정 벤더 기능이 아니라 루트 포트와
# 스위치 포트가 공통으로 갖는 규격 기능이며, 엔드포인트인 NVMe의 오류 복구와
# 절전 동작을 바깥에서 관장한다.
#
# === 전체 아키텍처에서의 위치 ===
# drivers/pci/Makefile -> [이 파일]
# NVMe 장치가 오류를 일으키면 순서는 이렇다: 엔드포인트가 error TLP 보고
# -> 상위 루트 포트의 AER(aer.o)가 수집 -> err.o가 복구 절차를 조율
# -> 필요 시 NVMe의 pci_error_handlers 콜백(nvme_error_detected 등) 호출
# -> 링크 리셋 후 nvme_slot_reset()으로 컨트롤러 재초기화.
# 즉 NVMe의 .err_handler는 여기서 만들어진 코드가 불러 준다.
#
# === 타 모듈과의 연결 ===
# - drivers/nvme/host/pci.c: nvme_err_handler(pci_error_handlers) 구현체가 대상.
# - drivers/pci/pcie/portdrv.c: 서비스별 pcie_device를 만들어 개별 드라이버에 배분.
# - drivers/pci/pci-acpi.c: EDR은 펌웨어(ACPI)가 주도하는 오류 통지 경로다.
#
# === 주요 함수/구조체 요약 ===
# - portdrv.o : 포트 서비스 버스. 어떤 서비스가 가능한지 판별해 자식 장치를 만든다.
# - aer.o/err.o: 오류 수집과 복구 절차(리셋 -> 콜백 -> 재개) 조율.
# - aspm.o    : Active State Power Management. 링크 유휴 시 L0s/L1 진입 정책.
# - dpc.o     : Downstream Port Containment. 오류 확산을 링크 차단으로 막는다.
# ============================================================================

pcieportdrv-y			:= portdrv.o rcec.o	# [한국어] 포트 드라이버 합성 모듈. rcec.o는 Root Complex Event Collector 처리

obj-$(CONFIG_PCIEPORTBUS)	+= pcieportdrv.o bwctrl.o	# [한국어] 포트 서비스 버스와 대역폭 제어. 링크 속도 변경 알림이 여기서 처리된다

obj-y				+= aspm.o	# [한국어] ASPM은 CONFIG 무관하게 항상 빌드. NVMe 지연 시간에 직결되므로 런타임 정책 제어가 늘 필요하다
obj-$(CONFIG_PCIEAER)		+= aer.o err.o tlp.o	# [한국어] AER 3종. 오류 수집(aer), 복구 조율(err), TLP 헤더 로그 해석(tlp)
obj-$(CONFIG_CXL_RAS)		+= aer_cxl_rch.o	# [한국어] CXL RCH(Restricted CXL Host) 다운스트림 포트의 오류 처리 변형
obj-$(CONFIG_PCIEAER_INJECT)	+= aer_inject.o	# [한국어] 오류 주입 도구. NVMe 복구 경로를 실제 하드웨어 고장 없이 시험할 때
obj-$(CONFIG_PCIE_PME)		+= pme.o	# [한국어] Power Management Event. 절전 상태의 NVMe가 호스트를 깨우는 통지 경로
obj-$(CONFIG_PCIE_DPC)		+= dpc.o	# [한국어] Downstream Port Containment. 고장난 NVMe를 링크 차단으로 격리해 시스템 전체 오염을 막는다
obj-$(CONFIG_PCIE_PTM)		+= ptm.o	# [한국어] Precision Time Measurement. 호스트와 장치의 시계를 정렬한다
obj-$(CONFIG_PCIE_EDR)		+= edr.o	# [한국어] Error Disconnect Recover. 펌웨어가 DPC를 소유한 구성에서 ACPI 통지로 복구를 개시
