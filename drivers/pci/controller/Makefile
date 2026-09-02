# SPDX-License-Identifier: GPL-2.0
#
# ============================================================================
# [한국어 설명] PCIe 호스트 컨트롤러 드라이버 빌드 (controller/Makefile)
#
# === 파일의 역할 ===
# 루트 콤플렉스/호스트 브리지 드라이버를 빌드한다. 이 계층은 "PCI 버스가
# 존재하게 만드는" 층으로, CPU 쪽 주소 공간과 PCI 버스 주소 공간을 잇고
# 링크 트레이닝을 마쳐야 비로소 그 아래에 꽂힌 장치가 열거될 수 있다.
# 자체 IP를 쓰는 드라이버는 여기에 직접 두고, 공용 IP(DesignWare, Cadence,
# Mobiveil, PLDA)는 하위 디렉토리로 나눠 놓았다.
#
# === 전체 아키텍처에서의 위치 ===
# drivers/pci/Makefile -> [이 파일] -> cadence/, dwc/, mobiveil/, plda/
# NVMe 기준 시간 순서: 컨트롤러 probe -> 링크 업 -> pci_host_probe()
#   -> drivers/pci/probe.c 열거 -> struct pci_dev 생성
#   -> pci-driver.c가 nvme_driver와 매칭 -> nvme_probe() 호출.
# 즉 이 디렉토리의 드라이버가 실패하면 NVMe는 존재조차 하지 않는다.
#
# === 타 모듈과의 연결 ===
# - drivers/pci/controller/Kconfig: 여기 CONFIG 심볼의 정의처.
# - drivers/pci/ecam.c: 다수 드라이버가 ECAM 접근자를 재사용한다.
# - drivers/pci/setup-bus.c: 컨트롤러가 신고한 자원 범위 안에서 BAR를 배치.
# - drivers/pci/msi/: 컨트롤러가 만든 MSI 도메인 위에서 NVMe 벡터가 할당된다.
#
# === 주요 함수/구조체 요약 ===
# - pci-host-common.o : DT 파싱과 브리지 등록의 공통 절차.
# - pci-host-generic.o: ECAM만 있으면 되는 표준 호스트 브리지.
# - vmd.o             : Intel VMD. NVMe를 별도 도메인으로 감싸는 특수 경로.
# - pcie-<vendor>.o   : 각 SoC의 링크 트레이닝과 자원 창 설정.
# ============================================================================
obj-$(CONFIG_PCIE_CADENCE) += cadence/	# [한국어] Cadence IP 하위 디렉토리로 재귀
obj-$(CONFIG_PCI_FTPCI100) += pci-ftpci100.o	# [한국어] Faraday FTPCI100 브리지. 구형 ARM SoC
obj-$(CONFIG_PCI_IXP4XX) += pci-ixp4xx.o	# [한국어] Intel IXP4xx 네트워크 프로세서 브리지
obj-$(CONFIG_PCI_HYPERV) += pci-hyperv.o	# [한국어] Hyper-V 가상 PCI. 게스트가 NVMe를 패스스루로 받을 때 이 경로로 열거된다
obj-$(CONFIG_PCI_HYPERV_INTERFACE) += pci-hyperv-intf.o	# [한국어] Hyper-V PCI와 다른 모듈 간 인터페이스 계층
obj-$(CONFIG_PCI_MVEBU) += pci-mvebu.o	# [한국어] Marvell EBU(Armada 370/XP). 브리지 config space를 소프트웨어로 에뮬레이션한다
obj-$(CONFIG_PCI_AARDVARK) += pci-aardvark.o	# [한국어] Marvell Armada 3700 Aardvark 컨트롤러
obj-$(CONFIG_PCI_TEGRA) += pci-tegra.o	# [한국어] NVIDIA Tegra 20/30/K1 구형 자체 IP
obj-$(CONFIG_PCI_RCAR_GEN2) += pci-rcar-gen2.o	# [한국어] Renesas R-Car Gen2의 내부 PCI(USB 부착용)
obj-$(CONFIG_PCIE_RCAR_HOST) += pcie-rcar.o pcie-rcar-host.o	# [한국어] R-Car Gen2/3 호스트. 공통부(pcie-rcar.o)와 호스트부를 함께 빌드
obj-$(CONFIG_PCIE_RCAR_EP) += pcie-rcar.o pcie-rcar-ep.o	# [한국어] R-Car 엔드포인트. 공통부를 호스트와 공유하므로 같은 오브젝트가 다시 나열된다
obj-$(CONFIG_PCIE_RENESAS_RZG3S_HOST) += pcie-rzg3s-host.o	# [한국어] Renesas RZ/G3S 호스트
obj-$(CONFIG_PCI_HOST_COMMON) += pci-host-common.o	# [한국어] DT 파싱과 pci_host_bridge 등록의 공통 절차. 아래 여러 드라이버가 이를 재사용
obj-$(CONFIG_PCI_HOST_GENERIC) += pci-host-generic.o	# [한국어] 표준 ECAM 호스트 브리지. QEMU virt 머신에서 NVMe를 붙이는 기본 경로
obj-$(CONFIG_PCI_HOST_THUNDER_ECAM) += pci-thunder-ecam.o	# [한국어] Cavium ThunderX의 비표준 ECAM 보정
obj-$(CONFIG_PCI_HOST_THUNDER_PEM) += pci-thunder-pem.o	# [한국어] Cavium ThunderX PEM(PCIe Express MAC) 브리지
obj-$(CONFIG_PCIE_XILINX) += pcie-xilinx.o	# [한국어] Xilinx AXI PCIe 브리지
obj-$(CONFIG_PCIE_XILINX_NWL) += pcie-xilinx-nwl.o	# [한국어] Xilinx NWL(ZynqMP) 브리지
obj-$(CONFIG_PCIE_XILINX_CPM) += pcie-xilinx-cpm.o	# [한국어] Xilinx Versal CPM 브리지
obj-$(CONFIG_PCIE_XILINX_DMA_PL) += pcie-xilinx-dma-pl.o	# [한국어] Xilinx PL DMA 브리지
obj-$(CONFIG_PCI_V3_SEMI) += pci-v3-semi.o	# [한국어] V3 Semiconductor V360EPC 브리지. 구형 ARM Integrator
obj-$(CONFIG_PCI_XGENE) += pci-xgene.o	# [한국어] APM X-Gene 호스트. DT 경로용 일반 빌드
obj-$(CONFIG_PCI_XGENE_MSI) += pci-xgene-msi.o	# [한국어] X-Gene 전용 MSI 컨트롤러. NVMe 벡터가 여기서 할당된다
obj-$(CONFIG_PCI_VERSATILE) += pci-versatile.o	# [한국어] ARM Versatile 개발 보드 브리지
obj-$(CONFIG_PCIE_IPROC) += pcie-iproc.o	# [한국어] Broadcom iProc 코어 로직
obj-$(CONFIG_PCIE_IPROC_MSI) += pcie-iproc-msi.o	# [한국어] iProc 전용 MSI 컨트롤러
obj-$(CONFIG_PCIE_IPROC_PLATFORM) += pcie-iproc-platform.o	# [한국어] iProc의 DT 플랫폼 결합
obj-$(CONFIG_PCIE_IPROC_BCMA) += pcie-iproc-bcma.o	# [한국어] iProc의 BCMA 버스 결합
obj-$(CONFIG_PCIE_ALTERA) += pcie-altera.o	# [한국어] Altera/Intel FPGA soft IP 브리지
obj-$(CONFIG_PCIE_ALTERA_MSI) += pcie-altera-msi.o	# [한국어] Altera FPGA용 MSI 컨트롤러
obj-$(CONFIG_PCIE_ROCKCHIP) += pcie-rockchip.o	# [한국어] Rockchip 구형 자체 IP 공통부(RK3399). 신형 DWC 기반과는 별개
obj-$(CONFIG_PCIE_ROCKCHIP_EP) += pcie-rockchip-ep.o	# [한국어] Rockchip 자체 IP 엔드포인트 모드
obj-$(CONFIG_PCIE_ROCKCHIP_HOST) += pcie-rockchip-host.o	# [한국어] Rockchip 자체 IP 호스트 모드. RK3399 보드의 NVMe 경로
obj-$(CONFIG_PCIE_MEDIATEK) += pcie-mediatek.o	# [한국어] MediaTek 구세대 컨트롤러
obj-$(CONFIG_PCIE_MEDIATEK_GEN3) += pcie-mediatek-gen3.o	# [한국어] MediaTek Gen3 컨트롤러
obj-$(CONFIG_VMD) += vmd.o	# [한국어] Intel VMD. 루트 포트 하위 NVMe를 별도 PCI 도메인으로 감싸 OS에 하나의 장치로 보이게 한다. NVMe 열거 방식이 통째로 달라지는 특수 경로
obj-$(CONFIG_PCIE_BRCMSTB) += pcie-brcmstb.o	# [한국어] Broadcom STB 컨트롤러. 라즈베리파이 4/5의 NVMe 경로가 여기다
obj-$(CONFIG_PCI_LOONGSON) += pci-loongson.o	# [한국어] Loongson 브리지와 관련 quirk
obj-$(CONFIG_PCIE_HISI_ERR) += pcie-hisi-error.o	# [한국어] HiSilicon 오류 처리기. APEI를 통해 보고된 컨트롤러 오류를 해석
obj-$(CONFIG_PCIE_APPLE) += pcie-apple.o	# [한국어] Apple M1/M2 컨트롤러. Asahi Linux에서 내장 NVMe를 올리는 경로
obj-$(CONFIG_PCIE_MT7621) += pcie-mt7621.o	# [한국어] MediaTek MT7621 MIPS SoC 컨트롤러
obj-$(CONFIG_PCIE_ASPEED) += pcie-aspeed.o	# [한국어] ASPEED BMC 컨트롤러. 서버 관리 프로세서 쪽 경로

# pcie-hisi.o quirks are needed even without CONFIG_PCIE_DW
obj-y				+= dwc/	# [한국어] DesignWare 하위 디렉토리. 위 주석대로 pcie-hisi.o의 quirk가 CONFIG_PCIE_DW 없이도 필요해 무조건 진입한다
obj-y				+= mobiveil/	# [한국어] Mobiveil 하위 디렉토리로 무조건 진입
obj-y				+= plda/	# [한국어] PLDA 하위 디렉토리로 무조건 진입


# The following drivers are for devices that use the generic ACPI
# pci_root.c driver but don't support standard ECAM config access.
# They contain MCFG quirks to replace the generic ECAM accessors with
# device-specific ones that are shared with the DT driver.

# The ACPI driver is generic and should not require driver-specific
# config options to be enabled, so we always build these drivers on
# ARM64 and use internal ifdefs to only build the pieces we need
# depending on whether ACPI, the DT driver, or both are enabled.

ifdef CONFIG_ACPI	# [한국어] ACPI 부팅 경로에서만 아래 강제 빌드가 의미를 갖는다
ifdef CONFIG_PCI_QUIRKS	# [한국어] quirk 인프라가 있어야 MCFG 접근자 교체가 성립한다
obj-$(CONFIG_ARM64) += pci-thunder-ecam.o	# [한국어] ACPI 경로용 강제 빌드. ACPI 드라이버가 벤더 CONFIG에 의존하지 않도록 ARM64에서 항상 포함하고 내부 ifdef로 걸러낸다
obj-$(CONFIG_ARM64) += pci-thunder-pem.o	# [한국어] ThunderX PEM의 MCFG quirk를 ACPI 경로에 제공
obj-$(CONFIG_ARM64) += pci-xgene.o	# [한국어] X-Gene의 MCFG quirk를 ACPI 경로에 제공. DT 드라이버와 접근자를 공유
endif	# [한국어] 위 ifdef 블록 종료
endif	# [한국어] 위 ifdef 블록 종료
