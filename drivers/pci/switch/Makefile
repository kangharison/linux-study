# SPDX-License-Identifier: GPL-2.0
#
# ============================================================================
# [한국어 설명] PCIe 스위치 관리 드라이버 빌드 (drivers/pci/switch/Makefile)
#
# === 파일의 역할 ===
# PCIe 스위치 자체를 하나의 관리 대상 장치로 다루는 드라이버를 빌드한다.
# 일반 PCI 코드가 스위치를 "브리지의 집합"으로만 보는 데 반해, 여기 드라이버는
# 스위치 벤더가 제공하는 관리 엔드포인트에 붙어 펌웨어 갱신, 포트 상태 조회,
# NTB(Non-Transparent Bridge) 설정 같은 대역외 제어를 수행한다.
#
# === 전체 아키텍처에서의 위치 ===
# drivers/pci/Makefile -> [이 파일] -> switchtec.o
# NVMe 서버에서 CPU 루트 포트 하나에 다수의 SSD를 물리려면 그 사이에 PCIe
# 스위치가 들어간다. 스위치는 NVMe I/O 경로 위에 그대로 놓이므로, 링크 폭과
# 속도, 포트 분할(bifurcation), NTB 구성이 NVMe 대역폭 상한을 결정한다.
#
# === 타 모듈과의 연결 ===
# - drivers/pci/switch/Kconfig: CONFIG_PCI_SW_SWITCHTEC 정의처.
# - drivers/pci/quirks.c: 같은 스위치의 DMA alias/ACS 보정이 별도로 존재한다.
# - drivers/nvme/host/pci.c: 스위치 하단에 열거된 NVMe가 이 스위치를 경유해
#   DMA를 수행하므로, P2PDMA 가능 여부 판정에 스위치 토폴로지가 쓰인다.
#
# === 주요 함수/구조체 요약 ===
# - switchtec.o: Microsemi/Microchip Switchtec 관리 인터페이스. /dev/switchtec*
#   캐릭터 장치로 MRPC(Management Remote Procedure Call)를 노출한다.
# ============================================================================
obj-$(CONFIG_PCI_SW_SWITCHTEC) += switchtec.o	# [한국어] Switchtec 관리 드라이버. NVMe 팬아웃 스위치의 펌웨어/포트 상태를 유저스페이스에 노출
