# SPDX-License-Identifier: GPL-2.0-only
#
# ============================================================================
# [한국어 설명] PCI 슬롯 전원 제어 드라이버 빌드 (drivers/pci/pwrctrl/Makefile)
#
# === 파일의 역할 ===
# PCIe 슬롯에 전원을 넣고 클럭을 공급해 링크가 올라올 수 있는 상태를 만드는
# 드라이버들을 빌드한다. x86 서버는 보드가 알아서 전원을 대지만, 임베디드
# 보드는 규제기(regulator)와 클럭을 소프트웨어가 순서대로 켜 줘야 한다.
# 이 순서가 틀리면 장치가 아예 열거되지 않는다.
#
# === 전체 아키텍처에서의 위치 ===
# drivers/pci/Makefile -> [이 파일]
# 시간 순서상 이 계층이 가장 먼저다: 전원 -> 클럭 -> 리셋 해제 -> 링크 트레이닝
# -> config space 접근 가능 -> probe.c 열거 -> NVMe probe. 앞단이 실패하면
# 뒤의 모든 단계가 존재하지 않는다.
#
# === 타 모듈과의 연결 ===
# - drivers/pci/pwrctrl/Kconfig: CONFIG_PCI_PWRCTRL* 정의처.
# - drivers/pci/of.c: DT에서 슬롯 노드를 찾아 이 드라이버와 결합시킨다.
# - drivers/regulator, drivers/clk: 실제 전원/클럭 공급원.
#
# === 주요 함수/구조체 요약 ===
# - core.o    : pci_pwrctrl 등록과 슬롯 재검색(rescan) 트리거 공통 로직.
# - pci-pwrctrl-pwrseq.o: pwrseq 프레임워크 기반 범용 전원 시퀀스.
# - generic.o : DT의 regulator 목록만으로 동작하는 범용 구현.
# - pci-pwrctrl-tc9563.o: Toshiba TC9563 PCIe 스위치 전용 시퀀스.
# ============================================================================

obj-$(CONFIG_PCI_PWRCTRL)		+= pci-pwrctrl-core.o	# [한국어] 전원 제어 코어 모듈. 아래 -y 행이 실제 구성 오브젝트를 지정한다
pci-pwrctrl-core-y			:= core.o	# [한국어] core.o를 pci-pwrctrl-core.ko로 묶는다. 모듈명과 소스명을 분리하려는 관례

obj-$(CONFIG_PCI_PWRCTRL_PWRSEQ)	+= pci-pwrctrl-pwrseq.o	# [한국어] pwrseq 서브시스템 연동. 전원 시퀀스를 별도 프레임워크에 위임하는 구현

obj-$(CONFIG_PCI_PWRCTRL_GENERIC)	+= pci-pwrctrl-generic.o	# [한국어] 범용 구현. DT에 나열된 regulator만 켜면 되는 단순한 슬롯용
pci-pwrctrl-generic-y			:= generic.o	# [한국어] generic.o를 pci-pwrctrl-generic.ko로 묶는 매핑

obj-$(CONFIG_PCI_PWRCTRL_TC9563)	+= pci-pwrctrl-tc9563.o	# [한국어] TC9563 스위치 전용. I2C로 스위치를 구성한 뒤에야 하단 포트가 살아난다
