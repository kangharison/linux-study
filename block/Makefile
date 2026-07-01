# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the kernel block layer
#
# ============================================================================
# NVMe SSD 관점 파일 개요
# ============================================================================
# 이 Makefile은 커널 블록 계층(block layer)의 핵심 오브젝트들을 빌드한다.
# NVMe 드라이버(drivers/nvme/)는 이 계층 위에서 동작하며, blk-mq, blk-mq-tag,
# blk-mq-dma 등을 통해 멀티 큐 I/O 스케줄링, 태그(CID) 할당, DMA 매핑 등을
# 수행한다. 다시 말해, 이 파일은 NVMe SQ/CQ 제출 경로와 DMA(PRP/SGL) 준비,
# 타임아웃, flush/FUA 정책 등을 실체화하는 빌드 청사진 역할을 한다.
# blk-core.o -> blk-mq.o -> nvme-queue.c -> nvme_submit_cmd(doorbell) 로
# 이어지는 요청 처리 체인의 기반이 여기서 컴파일된다.
# 상위 Makefile(top-level kernel Makefile) -> 이 Makefile -> drivers/nvme/Makefile
# 순으로 빌드가 전개되며, NVMe 드라이버는 여기서 생성된 심볼들을 링크한다.
# ============================================================================

# 블록 계층 핵심 오브젝트 목록.
# NVMe 입장에서 bio/request가 blk-mq를 거쳐 하드웨어 큐(nvme_queue)로
# 도달하기까지 필요한 모든 기반 모듈이 포함된다.
obj-y		:= bdev.o		# block device 접근점: /dev/nvme0n1 open/read/write 시 blkdev_file_operations 경유
obj-y		+= fops.o		# block device 파일 오퍼레이션: VFS -> bdev -> blk-mq -> nvme_queue_rq 호출의 진입점
obj-y		+= bio.o		# bio 할당/클론/분할: NVMe SGL/PRP로 변환될 원시 I/O 단위
obj-y		+= elevator.o		# 레거시 단일 큐 elevator 프레임워크; NVMe는 blk-mq 기반으로 직접 사용하지 않음
obj-y		+= blk-core.o		# request_queue, gendisk 핵심: nvme_alloc_disk() -> blk_mq_alloc_disk()의 토대
obj-y		+= blk-sysfs.o		# /sys/block/nvme0n1/queue/* 속성: max_sectors, scheduler, nr_requests 등 NVMe 노출값
obj-y		+= blk-flush.o		# FLUSH/FUA 시퀀스: NVMe Flush 명령 -> 데이터 쓰기 -> Flush 완료 순서 보장
obj-y		+= blk-settings.o	# queue_limits 초기화: NVMe PRP 최대 길이, max_segments, virt_boundary 설정의 근거
obj-y		+= blk-ioc.o		# IO Context 관리: 태스크별 I/O 문맥이 blk-mq -> nvme_queue 매핑에 영향
obj-y		+= blk-map.o		# 사용자 공간 버퍼 -> bio/page 매핑: NVMe DMA 대상 페이지 준비
obj-y		+= blk-merge.o		# bio/request 병합: 연속 섹터를 NVMe PRP/SGL 체인 하나로 합치는 정책 수행
obj-y		+= blk-timeout.o	# request timeout: NVMe 명령 CQ 미완료 시 nvme_timeout -> reset/recovery 유발
obj-y		+= blk-lib.o		# 블록 레벨 헬퍼: NVMe discard/secure erase 등 IOCTL 하부 함수
obj-y		+= blk-mq.o		# 멀티 큐 코어: bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
obj-y		+= blk-mq-tag.o		# hardware tag(CID) 할당: tag -> NVMe Command Identifier 매핑, SQ/CQ 식별키
obj-y		+= blk-mq-dma.o		# SG -> DMA 주소 변환: bio_vec -> dma_addr_t, NVMe PRP/SGL 엔트리 채움
obj-y		+= blk-stat.o		# blk-mq 통계: NVMe queue depth, latency, IOPS 측정용
obj-y		+= blk-mq-sysfs.o	# /sys/kernel/debug/block 및 /sys/block/*/mq/*: NVMe hctx 상태 노출
obj-y		+= blk-mq-cpumap.o	# CPU -> hctx 매핑: CPU 코어가 어느 nvme_queue(SQ/CQ 쌍)로 라우팅될지 결정
obj-y		+= blk-mq-sched.o	# blk-mq와 I/O 스케줄러 접착: dispatch/insert 경로에서 nvme_queue_rq 호출 전 마지막 정렬
obj-y		+= ioctl.o		# generic block ioctl: NVMe discard/write-zeroes 등 blk_ioctl 처리
obj-y		+= genhd.o		# gendisk 등록/해제: /dev/nvme0n1 디바이스 노드 생성의 밑바탕
obj-y		+= ioprio.o		# I/O 우선순위: NVMe cmd_flags 우선순위 비트(bio_prio) 설정
obj-y		+= badblocks.o		# 결함 블록 관리: NVMe namespace 레이어에서 노출되는 bad block 리스트
obj-y		+= partitions/		# MBR/GPT 파티션: /dev/nvme0n1p1 생성, NVMe namespace 위 파티션 인식
obj-y		+= blk-rq-qos.o		# request QoS: blk-throttle/iolatency/iocost가 nvme_queue_rq 호출 전 입출력률 제어
obj-y		+= disk-events.o	# 미디어 변경/이젝트 이벤트: NVMe hotplug/remove 이벤트 처리의 일부
obj-y		+= blk-ia-ranges.o	# discard/secure erase 범위: NVMe Deallocate/Write Zeroes 명령 범위 계산
obj-y		+= early-lookup.o	# early boot block device 탐색: initramfs에서 NVMe 루트 장치 인식 지원

# bsg.o: Generic block layer SCSI/SATA passthrough 프레임워크.
# NVMe에서도 사용자 공간의 ioctl 기반 admin/cmd 제출 경로(예: nvme-cli)가
# 이 인프라를 간접적으로 활용할 수 있다(추정).
obj-$(CONFIG_BLK_DEV_BSG_COMMON) += bsg.o		# SG_IO 공통: nvme-ns-hdev pass-through의 하위 인프라(추정)
obj-$(CONFIG_BLK_DEV_BSGLIB)	+= bsg-lib.o		# SG v4 헬퍼: nvme-cli io-passthru raw 명령 제출 지원

# blk-cgroup 계열: NVMe 장치에 대한 I/O 자원 분리(QoS)를 가능하게 한다.
# blk-throttle, blk-iolatency, blk-iocost 등은 NVMe 큐 깊이와 제출률을
# 제어하여 SQ(Submission Queue) 포화 및 latency spike를 완화한다.
obj-$(CONFIG_BLK_CGROUP)	+= blk-cgroup.o		# cgroup IO 컨트롤러 기반: NVMe 장치 그룹별 IO 격리
obj-$(CONFIG_BLK_CGROUP_RWSTAT)	+= blk-cgroup-rwstat.o	# cgroup별 rw 통계: NVMe 위 다중 cgroup IO량 분리 집계
obj-$(CONFIG_BLK_CGROUP_FC_APPID) += blk-cgroup-fc-appid.o	# FC 앱 ID 추적: NVMe over FC 트래픽 cgroup 식별(추정)
obj-$(CONFIG_BLK_DEV_THROTTLING)	+= blk-throttle.o	# cgroup별 IOPS/BPS 제한: NVMe SQ doorbell 간격 조절
obj-$(CONFIG_BLK_CGROUP_IOPRIO)	+= blk-ioprio.o		# cgroup IO 우선순위: NVMe SQ 탐색 우선순위 반영 가능(추정)
obj-$(CONFIG_BLK_CGROUP_IOLATENCY)	+= blk-iolatency.o	# cgroup latency 목표: NVMe 평균 지연 QoS 제어
obj-$(CONFIG_BLK_CGROUP_IOCOST)	+= blk-iocost.o		# cgroup 비용 모델: NVMe queue 자원 가중치 분배

# 멀티 큐 I/O 스케줄러들. NVMe는 전통 elevator 대신 mq-deadline, kyber,
# BFQ 중 하나를 blk-mq-sched.o 위에서 운용한다.
# 스케줄러가 bio를 재배열/병합한 뒤, blk_mq_dispatch_rq_list()를 통해
# nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 SQ에 삽입된다.
obj-$(CONFIG_MQ_IOSCHED_DEADLINE)	+= mq-deadline.o	# deadline 스케줄러: NVMe에서 read/write deadline 보장, 단일 큐 대비 지연 예측
obj-$(CONFIG_MQ_IOSCHED_KYBER)	+= kyber-iosched.o	# kyber 스케줄러: NVMe latency target 기반 read/write/async 분리
bfq-y				:= bfq-iosched.o bfq-wf2q.o bfq-cgroup.o	# BFQ 스케줄러 구성: NVMe 상호작용 및 cgroup 서비스 클래스 분리
obj-$(CONFIG_IOSCHED_BFQ)	+= bfq.o		# BFQ 스케줄러: NVMe에서도 latency 보장을 위한 예산 기반 스케줄링(추정)

# BLK_DEV_INTEGRITY: T10 PI(DIF/DIX) 및 end-to-end 데이터 보호 지원.
# NVMe 장치가 metadata를 활용한 PI를 제공할 때, bio-integrity.o와
# blk-integrity.o가 PRP/SGL 버퍼와 함께 guard 정보를 관리한다.
obj-$(CONFIG_BLK_DEV_INTEGRITY) += bio-integrity.o	# bio integrity payload: NVMe metadata + guard 정보 연계
obj-$(CONFIG_BLK_DEV_INTEGRITY) += blk-integrity.o	# 블록 계층 integrity 프레임워크: NVMe PI 형식 등록
obj-$(CONFIG_BLK_DEV_INTEGRITY) += t10-pi.o		# T10 PI CRC 계산: NVMe DIF guard 생성/검증
obj-$(CONFIG_BLK_DEV_INTEGRITY) += bio-integrity-auto.o	# 자동 integrity 할당: NVMe PI용 metadata 버퍼 준비
obj-$(CONFIG_BLK_DEV_INTEGRITY) += bio-integrity-fs.o	# 파일시스템 연계: NVMe PI와 fs metadata 경로 연결(추정)

# BLK_DEV_ZONED: Zoned Namespace(ZNS) SSD 지원. NVMe ZNS 명령어
# (Zone Append, Reset, Finish 등)가 blk-zoned.o의 zoned block 인터페이스를
# 통해 노출된다.
obj-$(CONFIG_BLK_DEV_ZONED)	+= blk-zoned.o		# zoned block 인터페이스: NVMe ZNS zone append/reset/finish 지원

# BLK_WBT: writeback throttling. NVMe SSD의 높은 병렬 처리 능력을 고려할 때
# 쓰기 지연을 조절하여 SQ 가득 참 및 GC 유발을 억제하는 역할을 한다(추정).
obj-$(CONFIG_BLK_WBT)		+= blk-wbt.o		# writeback 스로틀링: NVMe SQ 포화 및 latency spike 완화(추정)

# BLK_DEBUG_FS: blk-mq debugfs. NVMe 드라이버 개발 시 hctx, request, tag
# 상태를 추적하는 데 필수적인 디버깅 인프라를 제공한다.
obj-$(CONFIG_BLK_DEBUG_FS)	+= blk-mq-debugfs.o	# blk-mq debugfs: NVMe hctx/request/tag/CQ 완료 상태 추적

# SED/Opal: NVMe Opal SSC self-encrypting drive의 잠금/해제 인터페이스.
obj-$(CONFIG_BLK_SED_OPAL)	+= sed-opal.o		# Opal SED 인터페이스: NVMe security send/receive 잠금 관리(추정)

# BLK_PM: runtime power management. NVMe APST(Autonomous Power State
# Transition) 및 장치 수준 절전 상태 전환과 연동된다.
obj-$(CONFIG_BLK_PM)		+= blk-pm.o		# runtime PM: NVMe APST/PCIe ASPM 및 doorbell idle 정책 연동

# BLK_INLINE_ENCRYPTION: FSCrypt/inline encryption 하드웨어 오프로드.
# NVMe 장치가 inline encryption를 지원하면, 암호화 연산이 doorbell 제출
# 이전에 하드웨어에 위임될 수 있다(추정).
obj-$(CONFIG_BLK_INLINE_ENCRYPTION)	+= blk-crypto.o		# inline encryption 코어: NVMe Key Per I/O 컨텍스트 전달(추정)
obj-$(CONFIG_BLK_INLINE_ENCRYPTION)	+= blk-crypto-profile.o	# crypto profile 관리: NVMe 장치별 암호화 능력 등록
obj-$(CONFIG_BLK_INLINE_ENCRYPTION)	+= blk-crypto-sysfs.o	# crypto sysfs: NVMe inline encryption 속성 노출
obj-$(CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK)	+= blk-crypto-fallback.o	# 소프트웨어 fallback: NVMe 하드웨어 암호화 미지원 시 커널 crypto API 사용

# holder.o: legacy block holder 인터페이스(deprecated).
obj-$(CONFIG_BLOCK_HOLDER_DEPRECATED)	+= holder.o		# 레거시 holder: NVMe multipath/dm 장치에서 하위 호환용

# ============================================================================
# 주요 오브젝트별 NVMe 연결점 상세 설명
# ============================================================================
#
# blk-mq.o:
#   멀티 큐 block layer의 핵심. NVMe 드라이버는 blk_mq_alloc_request()를
#   통해 request를 얻고, blk_mq_run_hw_queue() -> nvme_queue_rq()로
#   하드웨어 큐(nvme_queue)에 전달한다. 이 오브젝트 없이는 NVMe의 여러
#   SQ/CQ 병렬 제출이 불가능하다.
#
# blk-mq-tag.o:
#   hardware tag(id) 할당기. NVMe에서는 이 tag가 CID(Command Identifier)로
#   매핑되어, 컨트롤러가 완료된 명령을 CQ entry에서 식별할 수 있게 한다.
#   blk_mq_get_tag() -> nvme_alloc_request() -> CID 할당 -> SQ entry 작성.
#
# blk-mq-dma.o:
#   SG 테이블을 DMA 주소로 변환. NVMe PRP(Physical Region Page) 및 SGL
#   (Scatter-Gather List) 엔트리를 채우기 위해 bio_vec -> dma_addr_t
#   변환을 담당한다.
#
# blk-mq-cpumap.o:
#   CPU와 hardware queue의 매핑 테이블. NVMe 장치의 큐 수가 코어 수보다
#   많거나 적을 때, 어떤 CPU의 I/O가 어떤 nvme_queue(hctx)로 향할지
#   결정한다(추정).
#
# blk-mq-sched.o:
#   blk-mq와 I/O 스케줄러(BFQ, mq-deadline, kyber)의 접착 계층.
#   bio -> blk_mq_submit_bio -> sched.insert/dispatch ->
#   blk_mq_dispatch_rq_list -> nvme_queue_rq -> doorbell.
#
# blk-flush.o:
#   flush(FUA) 요청 처리. NVMe는 FLUSH 명령어를 통해 휘발성 쓰기 캐시를
#   비우며, blk-flush.o는 FLUSH -> DATA -> FLUSH 시퀀스를 조정한다.
#
# blk-timeout.o:
#   request timeout 관리. NVMe 명령이 CQ 완료 없이 일정 시간 경과 시
#   nvme_timeout() -> nvme_req_complete() -> CQ 처리/리셋 경로가 동작한다.
#
# blk-settings.o:
#   큐 한계값 설정(max_hw_sectors, max_segments, virt_boundary 등).
#   이 값들은 NVMe PRP list 최대 길이, SGL segment 수, 페이지 정렬 제약을
#   결정한다.
#
# blk-merge.o:
#   bio/request 병합. NVMe 성능을 위해 연속 섹터 요청을 하나의 PRP/SGL
#   체인으로 합치는 데 직접적인 영향을 준다. 단, 너무 긴 체인은 PRP list
#   제한을 초과할 수 있으므로 주의가 필요하다(추정).
#
# blk-map.o:
#   사용자 공간 버퍼를 bio/page에 매핑. NVMe I/O가 사용자 메모리를
#   직접 DMA할 때 필요한 페이지 준비 단계를 수행한다.
#
# elevator.o:
#   레거시 단일 큐 elevator(예: CFQ). NVMe는 기본적으로 blk-mq 기반
#   멀티 큐를 사용하므로, NVMe I/O 경로에서는 직접 관여하지 않는다.
#
# ============================================================================
# /* NVMe 관점 핵심 요약 */
# ============================================================================
# - 이 Makefile은 NVMe I/O의 "소프트웨어 반쪽"을 컴파일한다: bio 생성부터
#   blk-mq를 거쳐 nvme_queue로 request를 라우팅하는 전 과정의 기반이 된다.
# - blk-mq.o, blk-mq-tag.o, blk-mq-dma.o가 각각 SQ 제출 경로, CID(tag)
#   관리, PRP/SGL DMA 매핑이라는 NVMe 핵심 동작에 대응한다.
# - blk-flush.o, blk-timeout.o, blk-integrity.o는 NVMe FLUSH 명령,
#   명령 타임아웃, end-to-end 데이터 보호와 직접 연결된다.
# - CONFIG_MQ_IOSCHED_*, CONFIG_BLK_DEV_INTEGRITY, CONFIG_BLK_DEV_ZONED 같은
#   Kconfig 옵션은 NVMe 스케줄러 선택, PI 활성화, ZNS SSD 지원 여부를
#   빌드 시점에 결정한다.
# - 드라이버 빌드 흐름: 상위 Makefile -> block/Makefile(이 파일) ->
#   drivers/nvme/Makefile, 최종적으로 vmlinux 또는 nvme 모듈에 링크된다.
# ============================================================================
