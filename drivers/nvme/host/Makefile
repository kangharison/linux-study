# SPDX-License-Identifier: GPL-2.0
#
# ============================================================================
# [한국어 설명] NVMe 호스트 드라이버 빌드 청사진 (drivers/nvme/host/Makefile)
#
# === 파일의 역할 ===
# NVMe 호스트 스택을 어떤 모듈로 나눠 빌드할지 정한다. 이 파일의 구조가 곧
# 이 디렉토리의 설계다 -- 트랜스포트에 무관한 공통부는 nvme-core.ko 하나로
# 모으고, 각 트랜스포트(pci/rdma/fc/tcp/apple)는 그 위에 얹히는 별도 모듈이
# 된다. 그래서 PCIe SSD만 쓰는 시스템은 nvme-core 와 nvme 만 올리면 되고,
# Fabrics 코드는 메모리에 존재하지도 않는다.
#
# === 전체 아키텍처에서의 위치 ===
# drivers/nvme/Makefile -> [이 파일]
# 모듈 의존 관계는 아래와 같고, 링크 단위가 곧 의존 방향이다:
#   nvme-core.ko  (core/ioctl/sysfs/pr + 선택적 기능들)
#     ^-- nvme.ko        (pci.o)          PCIe SSD
#     ^-- nvme-fabrics.ko (fabrics.o)     연결 옵션 파싱과 Connect 공통부
#           ^-- nvme-rdma.ko / nvme-fc.ko / nvme-tcp.ko
#     ^-- nvme-apple.ko  (apple.o)        Apple SoC 내장 컨트롤러
# nvme-core 가 struct nvme_ctrl 과 상태 기계를 갖고, 트랜스포트 모듈은
# nvme_ctrl_ops 를 채워 등록만 한다.
#
# === 타 모듈과의 연결 ===
# - drivers/nvme/host/Kconfig: 여기 쓰인 모든 CONFIG_NVME_* 심볼의 정의처.
# - block/: nvme-core 가 blk-mq 위에 gendisk 와 태그셋을 올린다.
# - drivers/pci/: nvme.ko 가 PCI 드라이버로 등록되어 열거 결과를 받는다.
# - net/, drivers/infiniband/, drivers/scsi 의 FC LLDD: 각 Fabrics 트랜스포트의
#   실제 전송 수단.
#
# === 주요 함수/구조체 요약 ===
# - nvme-core-y: 트랜스포트와 무관하게 항상 필요한 네 개(core, ioctl, sysfs, pr).
# - nvme-core-$(CONFIG_*): 선택 기능들. 꺼면 오브젝트 자체가 빠지므로
#   런타임 분기가 아니라 빌드 시점에 코드가 사라진다.
# - nvme-y ~ nvme-apple-y: 각 트랜스포트 모듈의 구성. 전부 소스 하나짜리라
#   모듈 이름과 파일 이름을 이어 주는 역할만 한다.
# ============================================================================

ccflags-y				+= -I$(src)	# [한국어] 이 디렉토리를 include 경로에 넣는다. trace.h 가 TRACE_INCLUDE_PATH 로 자신을 되찾을 때 필요하다

obj-$(CONFIG_NVME_CORE)			+= nvme-core.o	# [한국어] 트랜스포트 공통 코어. struct nvme_ctrl, 상태 기계, Identify 해석, 네임스페이스 스캔이 모두 여기 있다
obj-$(CONFIG_BLK_DEV_NVME)		+= nvme.o	# [한국어] PCIe NVMe 드라이버. 가장 흔한 구성이며 nvme-core 위에 pci.o 하나만 얹는다
obj-$(CONFIG_NVME_FABRICS)		+= nvme-fabrics.o	# [한국어] Fabrics 공통부. 연결 문자열 파싱과 Connect/Property 명령을 rdma/fc/tcp 가 공유한다
obj-$(CONFIG_NVME_RDMA)			+= nvme-rdma.o	# [한국어] RDMA 트랜스포트. InfiniBand/RoCE/iWARP 위에서 메모리 등록 기반으로 동작
obj-$(CONFIG_NVME_FC)			+= nvme-fc.o	# [한국어] Fibre Channel 트랜스포트. HBA LLDD 위에 Association/Connection 세션을 올린다
obj-$(CONFIG_NVME_TCP)			+= nvme-tcp.o	# [한국어] TCP 트랜스포트. 커널 소켓 위에서 PDU 를 주고받으며 특별한 하드웨어가 필요 없다
obj-$(CONFIG_NVME_APPLE)		+= nvme-apple.o	# [한국어] Apple ANS 플랫폼 드라이버. PCI 가 아니라 platform_device 로 붙는다

nvme-core-y				+= core.o ioctl.o sysfs.o pr.o	# [한국어] 코어의 필수 네 조각 -- 상태 기계와 명령 조립(core), 유저 패스스루(ioctl), 속성 노출(sysfs), 영구 예약(pr)
nvme-core-$(CONFIG_NVME_VERBOSE_ERRORS)	+= constants.o	# [한국어] opcode/status 문자열 테이블. Kconfig 가 밝히듯 커널 이미지가 약 4KB 늘어난다
nvme-core-$(CONFIG_TRACING)		+= trace.o	# [한국어] ftrace 추적점의 opcode 해석기
nvme-core-$(CONFIG_NVME_MULTIPATH)	+= multipath.o	# [한국어] 다중 경로. 꺼면 컨트롤러마다 별도 /dev/nvmeXnY 가 생긴다
nvme-core-$(CONFIG_BLK_DEV_ZONED)	+= zns.o	# [한국어] ZNS(Zoned Namespace) 지원. 블록 계층의 존 인프라와 맞물린다
nvme-core-$(CONFIG_FAULT_INJECTION_DEBUG_FS)	+= fault_inject.o	# [한국어] 오류 주입. 실제 고장 없이 복구 경로를 시험한다
nvme-core-$(CONFIG_NVME_HWMON)		+= hwmon.o	# [한국어] 온도 등 하드웨어 모니터링을 hwmon 서브시스템에 노출
nvme-core-$(CONFIG_NVME_HOST_AUTH)	+= auth.o	# [한국어] DH-HMAC-CHAP 대역 내 인증. Fabrics 연결에서 쓰인다

nvme-y					+= pci.o	# [한국어] nvme.ko 는 pci.o 하나로 이뤄진다

nvme-fabrics-y				+= fabrics.o	# [한국어] nvme-fabrics.ko 는 fabrics.o 하나

nvme-rdma-y				+= rdma.o	# [한국어] nvme-rdma.ko 는 rdma.o 하나

nvme-fc-y				+= fc.o	# [한국어] nvme-fc.ko 는 fc.o 하나

nvme-tcp-y				+= tcp.o	# [한국어] nvme-tcp.ko 는 tcp.o 하나

nvme-apple-y				+= apple.o	# [한국어] nvme-apple.ko 는 apple.o 하나
