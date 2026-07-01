# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the linux kernel.
#

# ============================================================================
# NVMe SSD 관점 파일 요약
# ============================================================================
# 이 Makefile은 리눅스 커널 block layer의 "partition table parser"들을
# 조걸별로 컴파일한다. NVMe namespace(예: /dev/nvme0n1)가 블록 디바이스로
# 등록되면 blk_dev_scan_partitions() -> read_partitions() 경로를 통해
# 이 파서들이 호출되어 GPT, MBR, AIX, SGI/Sun 등 파티션 테이블을 해석한다.
# NVMe 입장에서는 doorbell/CID/SQ/CQ/PRP/SGL 같은 전송 메커니즘은 이 단계와
# 무관하지만, 이 파일이 생성하는 파서가 NVMe namespace 위에 /dev/nvme0n1p1
# 같은 파티션 디바이스를 노출하는 gateway 역할을 한다.
# 상위 Makefile: block/Makefile
# 주요 호출 체인(추정):
#   add_disk -> register_disk -> blk_dev_scan_partitions ->
#   read_partitions -> msdos_partition / efi_partition / ...
# ============================================================================

# obj-$(CONFIG_BLOCK)에 의해 core.o는 block layer가 활성화될 때 항상 빌드된다.
# NVMe namespace 등록 시 add_disk() -> register_disk() -> blk_dev_scan_partitions()
# 경로로 진입하며, 이때 core.o의 read_partitions() 함수가 파티션 스캔을 조율한다.
# NVMe 관점: core.o는 모든 NVMe namespace의 블록 디바이스 등록 시 거치는
#   공통 진입점으로, NVMe I/O path(SQ/CQ/tag/CID)가 시작되기 전에
#   namespace 위에 파티션 메타데이터를 노출할지 여부를 결정한다.
#   향후 nvmeXnYpN 파티션에 대한 read/write bio는 blk-mq request 할당 후
#   NVMe SQ/CQ/doorbell 경로로 전달되며, 파티션 존재 여부는 이 단계에서 결정됨.
obj-$(CONFIG_BLOCK) += core.o

# acorn.o: Acorn RISC OS partition table 파서.
# NVMe 연결: NVMe namespace 블록 디바이스에서 Acorn 포맷을 감지하면
# add_partition()을 통해 /dev/nvmeXnYpN 노드를 생성한다 (추정).
#   - read_partitions() -> check_partition() -> acorn_partition()
#   - 생성된 파티션 gendisk는 상위 NVMe namespace의 request_queue를 공유하며,
#     이후 bio는 blk-mq tag/CID 할당을 거쳐 NVMe submission queue로 향함.
obj-$(CONFIG_ACORN_PARTITION) += acorn.o

# amiga.o: Amiga Rigid Disk Block(RDB) 파서.
# NVMe 연결: NVMe SSD가 Amiga RDB로 포맷된 경우 이 파서가 파티션 항목을
# blk_partition_desc 구조체로 변환하여 상위 block layer에 전달한다.
#   - 파티션 등록 후 NVMe I/O path: vfs_read/write -> submit_bio ->
#     blk_mq_submit_bio -> nvme_queue_rq -> SQ doorbell (CID/tag 기반).
obj-$(CONFIG_AMIGA_PARTITION) += amiga.o

# atari.o: Atari TOS partition table 파서.
# NVMe 연결: NVMe namespace의 첫 번째 섹터(LBA 0)를 읽어 Atari 파티션
# 서명을 확인하고, 일치하면 파티션 슬롯을 등록한다.
#   - 파서가 동작하려면 NVMe namespace LBA 0 읽기가 먼저 완료되어야 하며,
#     이는 block layer의 sync read -> nvme_submit_sync_cmd -> CQ 수신으로 이뤄짐.
obj-$(CONFIG_ATARI_PARTITION) += atari.o

# aix.o: IBM AIX LVM partition table 파서.
# NVMe 연결: 엔터프라이즈 NVMe 장비에서 AIX LVM으로 초기화된 디스크를
# 마운트할 때 사용되며, 파서 결과는 block/genhd.c의 파티션 등록 경로로
# 전달된다 (추정).
#   - AIX LVM 파티션 내에서 발생하는 I/O 역시 결국 NVMe namespace의
#     logical block address(LBA)로 매핑되어 PRP/SGL entry에 기록됨.
obj-$(CONFIG_AIX_PARTITION) += aix.o

# cmdline.o: 커널 커맨드라인에서 전달된 파티션 테이블 파서.
# NVMe 연결: cmdline/partitions= 옵션으로 NVMe namespace(nvme0n1 등)의
# 파티션 경계를 수동 지정할 때 사용된다. 이는 NVMe 컨트롤러의 기본
# 파티션 스캔을 우회하는 경로다.
#   - 수동 지정된 파티션도 동일한 gendisk/queue를 공유하므로,
#     이후 NVMe SQ/CQ I/O 경로는 자동 스캔 파티션과 동일하게 동작함.
obj-$(CONFIG_CMDLINE_PARTITION) += cmdline.o

# mac.o: Apple Mac partition map 파서.
# NVMe 연결: Mac 포맷 NVMe SSD에서 드라이브 전체(nvmeXnY) 대신
# 파티션별 블록 디바이스(nvmeXnYpN)를 생성하는 데 기여한다.
#   - mac_partition()이 등록한 파티션은 NVMe namespace의 capacity 내에서
#     start_sect/nr_secs로 제한되며, 이는 NVMe PRP/SGL length 계산에 반영됨.
obj-$(CONFIG_MAC_PARTITION) += mac.o

# ldm.o: Windows Logical Disk Manager(LDM, 동적 디스크) 파서.
# NVMe 연결: Windows dynamic disk로 구성된 NVMe 볼륨에서 LDM 데이터베이스를
# 읽어 볼륨/구성요소 관계를 파악하고, 이를 block layer 파티션 구조로 반영한다.
#   - LDM은 여러 NVMe namespace를 스팬(span)할 수 있으나, 각 파티션 I/O는
#     최종적으로 개별 NVMe namespace의 blk-mq hctx -> CQ -> completion으로 수행됨.
obj-$(CONFIG_LDM_PARTITION) += ldm.o

# msdos.o: 전통적인 MBR(Master Boot Record) 및 확장 파티션 파서.
# NVMe 연결: NVMe SSD 중 LBA 0에 MBR 시그니처(0xAA55)가 있는 경우 이 파서가
# 동작한다. GPT가 없는 레거시 NVMe 부팅 디스크에서 자주 사용된다.
# 상대적으로 중요한 파서이며, EFI(GPT) 파서와 함께 일반적인 NVMe 환경에서
# 가장 많이 호출된다.
#   - msdos_partition()은 NVMe namespace의 LBA 0을 파싱하여 primary/extended
#     파티션을 등록하고, 이 파티션들은 NVMe namespace의 request_queue를 공유.
#   - 부팅 시 NVMe namespace 등록 직후 호출되며, 부트로더가 /dev/nvme0n1p1
#     같은 파티션을 찾을 수 있게 하는 gateway 역할을 수행.
obj-$(CONFIG_MSDOS_PARTITION) += msdos.o

# of.o: Open Firmware(DT) 파티션 정보 파서.
# NVMe 연결: PowerPC/ARM64 등의 Open Firmware 기반 시스템에서 NVMe namespace에
# 대한 파티션 정보가 device tree에 기술되어 있을 때 사용된다 (추정).
#   - of_partition()은 DT에서 파티션 offset/size를 읽어 add_partition()을
#     호출하며, 이후 NVMe I/O는 동일한 blk-mq tag/CID 할당 경로를 따름.
obj-$(CONFIG_OF_PARTITION) += of.o

# osf.o: DEC OSF/1 disklabel 파서.
# NVMe 연결: Tru64/OSF 레이블이 있는 NVMe 볼륨을 처리할 때 사용된다.
#   - osf_partition()이 등록한 파티션 범위는 NVMe namespace capacity의
#     subset이 되며, 벗어난 I/O는 NVMe 컨트롤러에서 LBA out-of-range로 거부됨.
obj-$(CONFIG_OSF_PARTITION) += osf.o

# sgi.o: SGI disklabel 파서.
# NVMe 연결: SGI IRIX 시스템용으로 포맷된 NVMe 디스크의 파티션 정보를
# 해석하여 리눅스 block layer에 노출한다 (추정).
#   - 파서가 생성한 파티션 gendisk는 NVMe namespace의 bdev와 동일한
#     request_queue를 참조하므로, NVMe SQ/CQ 완료 경로도 공유됨.
obj-$(CONFIG_SGI_PARTITION) += sgi.o

# sun.o: Sun/Solaris disklabel 파서.
# NVMe 연결: SPARC/Solaris 환경에서 NVMe 디스크의 Sun 레이블을 인식하고
# 파티션을 등록하는 데 사용된다 (추정).
#   - Sun 레이블의 slices는 NVMe namespace LBA 단위로 변환되어 block layer의
#     partition table에 저장되고, 이후 NVMe read/write cmd의 SLBA로 사용됨.
obj-$(CONFIG_SUN_PARTITION) += sun.o

# ultrix.o: DEC Ultrix disklabel 파서.
# NVMe 연결: Ultrix 포맷 NVMe 볼륨에서 파티션을 인식할 때 사용된다 (추정).
#   - 등록된 Ultrix 파티션에 대한 I/O 요청은 blk-mq -> nvme_ns_head ->
#     nvme_submit_user_cmd 또는 nvme_queue_rq -> SQ doorbell로 전달됨.
obj-$(CONFIG_ULTRIX_PARTITION) += ultrix.o

# ibm.o: IBM OEM partition table 파서.
# NVMe 연결: IBM 시스템에서 사용하는 특수 NVMe 파티션 레이아웃을 해석할 때
# 사용된다 (추정).
#   - IBM OEM 파티션은 NVMe namespace의 LBA 0 근처에 메타데이터를 가지며,
#     파서가 이를 인식해야만 NVMe SSD 전체 용량이 올바르게 노출됨.
obj-$(CONFIG_IBM_PARTITION) += ibm.o

# efi.o: EFI GPT(GUID Partition Table) 파서.
# NVMe 연결: 최신 NVMe SSD의 표준 파티션 형식인 GPT를 해석한다.
# NVMe namespace의 LBA 1에 위치한 GPT 헤더와 partition entry array를 읽어
# /dev/nvmeXnYp1, /dev/nvmeXnYp2 등의 파티션 노드를 생성한다.
# UEFI 부팅이 가능한 NVMe 디스크에서 이 파서가 가장 핵심적이다.
#   - efi_partition()은 NVMe namespace의 LBA 1(GPT header)과 LBA 2~(entries)
#     를 읽어 GUID/시작 LBA/끝 LBA를 파싱함.
#   - GPT 파티션은 NVMe namespace의 capacity 정보에 기반해 유효성을 검사하며,
#     잘못된 파티션 table은 NVMe I/O 중 데이터 손상으로 이어질 수 있음.
#   - efi.o + msdos.o는 NVMe 부팅 디스크에서 거의 항상 필요하며,
#     파티션 단위로 나뉜 bio가 NVMe SQ/CQ를 통해 전송되도록 하는 기반이 됨.
obj-$(CONFIG_EFI_PARTITION) += efi.o

# karma.o: Rio Karma music player partition table 파서.
# NVMe 연결: 거의 사용되지 않으며, Rio Karma 전용 포맷이 NVMe SSD에
# 기록된 경우에만 동작한다 (추정).
#   - 드물게 사용되지만, 파서가 파티션을 등록하면 이후 I/O 경로는 다른
#     파서와 동일하게 NVMe request_queue -> blk-mq -> SQ/CQ를 따름.
obj-$(CONFIG_KARMA_PARTITION) += karma.o

# sysv68.o: Motorola System V/68 partition table 파서.
# NVMe 연결: System V/68 레이블이 있는 NVMe 볼륨에서 파티션을 인식할 때
# 사용된다 (추정).
#   - sysv68 파티션에 대한 I/O 요청은 상위 block layer에서 NVMe namespace의
#     request_queue로 전달되고, blk-mq tagset에서 tag/CID를 할당받아 SQ에 기록됨.
obj-$(CONFIG_SYSV68_PARTITION) += sysv68.o

# /* NVMe 관점 핵심 요약 */
# ---------------------------------------------------------------------------
# - 이 Makefile은 NVMe namespace(nvmeXnY) 위에 파티션 노드(nvmeXnYpN)를
#   노출하는 모든 파티션 파서의 빌드 목록을 정의한다.
# - NVMe I/O 경로(doorbell, CID, SQ, CQ, PRP, SGL)는 block/partitions가 아닌
#   drivers/nvme/host/pci.c 등에서 처리되며, 이 파일은 단지 파티션 메타데이터
#   해석만 담당한다.
# - block/Makefile -> block/partitions/Makefile 호출 구조 아래에서,
#   CONFIG_BLOCK이 활성화되면 core.o가 항상 포함되고, 나머지 파서는 해당
#   CONFIG_* 옵션에 따라 선택적으로 빌드된다.
# - msdos.o와 efi.o가 일반적인 NVMe 환경(레거시 MBR 및 현대적 GPT)에서
#   가장 자주 사용되며, NVMe 부팅 디스크 동작에 직접적인 영향을 준다.
# - 파서 호출 경로(추정): add_disk -> register_disk ->
#   blk_dev_scan_partitions -> read_partitions -> msdos_partition/efi_partition.
# - 파서가 생성하는 각 파티션은 NVMe namespace의 request_queue를 공유하며,
#   bio -> blk_mq_submit_bio -> nvme_queue_rq -> SQ doorbell (CID/tag/PRP/SGL)
#   경로를 통해 실제 NVMe I/O가 전달된다.
# ---------------------------------------------------------------------------
