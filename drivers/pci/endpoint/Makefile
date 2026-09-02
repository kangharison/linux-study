# SPDX-License-Identifier: GPL-2.0
#
# Makefile for PCI Endpoint Support
#
# ============================================================================
# [한국어 설명] PCIe 엔드포인트 프레임워크 빌드 (drivers/pci/endpoint/Makefile)
#
# === 파일의 역할 ===
# 리눅스를 PCIe 엔드포인트로 동작시키는 코어 프레임워크를 빌드한다. 컨트롤러
# 측 추상화(EPC), 기능 측 추상화(EPF), 그 둘이 쓰는 메모리 할당자(epc-mem),
# 그리고 사용자에게 결합을 맡기는 configfs 인터페이스로 구성된다.
#
# === 전체 아키텍처에서의 위치 ===
# drivers/pci/Makefile -> [이 파일] -> functions/
# 상위 Makefile이 이 디렉토리를 controller/ 보다 먼저 나열하는 이유가 여기
# 있다. 링크 순서가 initcall 순서이므로, EPC/EPF 코어가 먼저 등록돼야
# 컨트롤러와 기능 드라이버가 자신을 붙일 대상을 찾을 수 있다.
#
# === 타 모듈과의 연결 ===
# - drivers/pci/endpoint/functions/: 이 코어에 등록되는 기능 드라이버들.
# - drivers/pci/controller/dwc/*-ep.c: EPC 측 구현(DesignWare 엔드포인트 모드).
#
# === 주요 함수/구조체 요약 ===
# - pci-ep-cfs.o    : configfs 트리. 기능 생성과 EPC 결합을 사용자에게 노출.
# - pci-epc-core.o  : struct pci_epc 등록과 BAR/윈도우 조작 vtable.
# - pci-epf-core.o  : struct pci_epf 등록과 드라이버 매칭.
# - pci-epc-mem.o   : 아웃바운드 윈도우용 주소 공간 비트맵 할당자.
# - pci-ep-msi.o    : 엔드포인트가 호스트로 MSI 도어벨을 쏘는 경로.
# ============================================================================

obj-$(CONFIG_PCI_ENDPOINT_CONFIGFS)	+= pci-ep-cfs.o	# [한국어] /sys/kernel/config/pci_ep/ 트리. 기능 인스턴스 생성과 EPC 바인딩을 유저스페이스가 조립한다
# [한국어] 엔드포인트 코어 3종. 줄 연속(\)으로 묶인 하나의 논리 행이라 각
# 항목에 행말 주석을 달 수 없어 여기에 정리한다.
#   pci-epc-core.o: 컨트롤러 추상화. write_header/set_bar/map_addr vtable을 정의.
#   pci-epf-core.o: 기능 추상화. EPF 드라이버 등록과 EPC와의 매칭 담당.
#   pci-epc-mem.o : 아웃바운드 윈도우 주소를 비트맵으로 관리하는 할당자.
#   functions/    : 실제 기능 드라이버들로 재귀 진입.
obj-$(CONFIG_PCI_ENDPOINT)		+= pci-epc-core.o pci-epf-core.o\
					   pci-epc-mem.o functions/
obj-$(CONFIG_PCI_ENDPOINT_MSI_DOORBELL)	+= pci-ep-msi.o	# [한국어] 엔드포인트가 호스트에게 MSI를 발생시키는 도어벨 경로. 완료 통지를 역방향으로 보낼 때
