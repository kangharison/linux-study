# SPDX-License-Identifier: GPL-2.0
#
# Makefile for PCI Endpoint Functions
#
# ============================================================================
# [한국어 설명] PCIe 엔드포인트 기능 드라이버 빌드 (endpoint/functions/Makefile)
#
# === 파일의 역할 ===
# 리눅스가 PCIe 엔드포인트(장치측)로 동작할 때, 호스트에게 어떤 기능으로
# 보일지를 구현한 드라이버들을 빌드한다. 호스트에 꽂힌 NVMe를 다루는 평소
# 방향과 정반대로, 여기서는 이 리눅스 자신이 "꽂히는 쪽"이 된다.
#
# === 전체 아키텍처에서의 위치 ===
# endpoint/Makefile -> [이 파일]
# 기능 드라이버(EPF)는 pci-epf-core.o에 등록되고, EPC(컨트롤러) 드라이버와
# configfs를 통해 짝지어진 뒤 BAR와 config space 헤더를 호스트에 노출한다.
#
# === 타 모듈과의 연결 ===
# - drivers/pci/endpoint/pci-epf-core.c: 여기 드라이버들이 등록하는 프레임워크.
# - drivers/pci/endpoint/pci-ep-cfs.c: configfs로 기능을 생성/결합하는 경로.
#
# === 주요 함수/구조체 요약 ===
# - pci-epf-test.o: 엔드포인트 기능의 참조 구현이자 테스트 장치.
# - pci-epf-ntb.o : Non-Transparent Bridge 기능. 두 호스트를 잇는다.
# - pci-epf-vntb.o: 가상 NTB. 단일 엔드포인트로 NTB를 흉내낸다.
# - pci-epf-mhi.o : Qualcomm MHI 프로토콜 엔드포인트(모뎀 계열).
# ============================================================================

obj-$(CONFIG_PCI_EPF_TEST)		+= pci-epf-test.o	# [한국어] 참조/테스트 기능. BAR 읽기쓰기와 DMA를 검증해 EPC 드라이버의 정합성을 확인한다
obj-$(CONFIG_PCI_EPF_NTB)		+= pci-epf-ntb.o	# [한국어] NTB 기능. 두 개의 EPC를 묶어 호스트 간 메모리 창을 연다
obj-$(CONFIG_PCI_EPF_VNTB) 		+= pci-epf-vntb.o	# [한국어] 가상 NTB. 물리 NTB 하드웨어 없이 단일 엔드포인트로 동등한 인터페이스를 제공
obj-$(CONFIG_PCI_EPF_MHI)		+= pci-epf-mhi.o	# [한국어] MHI 엔드포인트. 모뎀/무선 SoC가 호스트에 자신을 노출하는 프로토콜
