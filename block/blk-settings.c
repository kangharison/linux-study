// SPDX-License-Identifier: GPL-2.0
/*
 * Functions related to setting various queue properties from drivers
 */

/*
 * [한국어 설명] 블록 큐 한도 설정 및 스택 병합 (blk-settings.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 block layer의 request_queue(q)가 보유하는 queue_limits(lim) 구조체를
 * 초기화·검증·정규화·병합하는 모든 함수를 담는다. NVMe/SCSI/virtio 등 다양한 드라이버가
 * 하드웨어 capability(MDTS, PRP/SGL 엔트리 수, LBA 크기, Deallocate granularity 등)를
 * queue_limits 필드에 채워 넣으면, 본 파일의 blk_validate_limits()가 이를 블록 레이어
 * 표준 형식으로 정규화하여 bio splitting·request 조립 경로에서 사용할 최종 I/O 한도를
 * 확정한다. 스택형 장치(MD RAID, Device Mapper, LUKS 등)에서는 blk_stack_limits()가
 * 여러 하위 장치의 한도를 최소 공통 분모로 교차 병합(stacking)한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 위치: 커널 유저스페이스가 아닌 커널 블록 레이어 내부 (kernel context).
 * 드라이버 probe 경로:
 *   nvme_reset_work → nvme_pci_configure_admin_queue → blk_mq_init_queue →
 *   queue_limits_set → blk_validate_limits  (NVMe PCI 드라이버 예시)
 * I/O 경로 (blk-mq가 limit 참조):
 *   submit_bio → blk_mq_submit_bio → blk_mq_get_request → nvme_queue_rq →
 *   nvme_sq_copy_cmd → nvme_write_sq_db (SQ 기록 후 doorbell)
 * 스택 병합 경로:
 *   md_run / dm-table-load → queue_limits_stack_bdev → blk_stack_limits
 * blk_validate_limits()는 드라이버 probe, 큐 재설정, limits 갱신 시 호출되며,
 * bio splitting 임계값을 결정하는 blk_mq_submit_bio() 이전에 항상 완료된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - include/linux/blk-mq.h: struct request_queue, struct queue_limits 정의
 *   - block/blk.h: BLK_SAFE_MAX_SECTORS, BLK_DEF_MAX_SECTORS_CAP 등 상수
 *   - block/blk-rq-qos.h: rq_qos_queue_depth_changed() — QoS depth 전파
 *   - block/blk-wbt.h: writeback throttle (queue depth 변경 통지)
 *   - include/linux/blk-integrity.h: struct blk_integrity (DIF/DIX PI 설정)
 * 피의존 모듈 (이 파일 함수를 호출하는 쪽):
 *   - drivers/nvme/host/pci.c: blk_queue_rq_timeout, queue_limits_set 등
 *   - drivers/md/md.c: blk_set_stacking_limits, blk_stack_limits
 *   - block/blk-mq.c: blk_set_default_limits, queue_limits_commit_update
 *   - block/partitions/core.c: queue_limits_stack_bdev, bdev_alignment_offset
 * 공유 자료구조:
 *   - struct queue_limits: 이 파일의 핵심 출력물. 모든 I/O 경로가 참조한다.
 *   - struct blk_integrity: NVMe DIF/DIX PI 설정을 담는 sub-structure.
 *
 * === 주요 함수/구조체 요약 ===
 * blk_validate_limits()    — queue_limits의 모든 필드를 정규화·검증하는 게이트키퍼.
 *                            드라이버가 채운 HW 값 → 블록 레이어 표준 값 변환.
 * blk_stack_limits()       — MD/DM 등이 여러 하위 장치의 queue_limits를 교차 병합.
 *                            alignment, max_sectors, discard, atomic_write를 통합.
 * blk_set_stacking_limits()— 스택형 가상 장치 초기화: 모든 필드를 UINT_MAX/0으로 두어
 *                            하위 장치 값과의 min/max 병합 시 neutral identity가 됨.
 * queue_limits_commit_update() — limits_lock 보호 하에 큐에 검증된 limits 원자적 적용.
 * blk_validate_integrity_limits() — NVMe DIF/DIX PI 조합의 유효성 검사 및 dma_alignment
 *                                   보강. metadata_size, csum_type, interval_exp 검증.
 * blk_validate_zoned_limits()    — NVMe ZNS의 zone_append_sectors 최종 한도 결정.
 * blk_validate_atomic_write_limits() — NVMe 원자적 쓰기(NAWUPF/NABSPF 유래) 단위 검증 및 정규화.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/blk-integrity.h>
#include <linux/pagemap.h>
#include <linux/backing-dev-defs.h>
#include <linux/gcd.h>
#include <linux/lcm.h>
#include <linux/jiffies.h>
#include <linux/gfp.h>
#include <linux/dma-mapping.h>
#include <linux/t10-pi.h>
#include <linux/crc64.h>

#include "blk.h"
#include "blk-rq-qos.h"
#include "blk-wbt.h"

/*
 * struct request_queue NVMe 관련 주요 필드:
 *   queue_depth: NVMe IO SQ/CQ pair의 최대 엔트리 수(Create IO Queue에서 설정).
 *   rq_timeout: NVMe command timeout 후 abort/reset 처리 기준(밀리초).
 *   limits: queue_limits. NVMe Identify/Controller capability가 저장됨.
 *   tags/tag_set: blk-mq tag ↔ NVMe CID/SQ slot 매핑을 위한 자료구조.
 *   bdi: backing_dev_info. blk_apply_bdi_limits()가 lim->io_opt와
 *        lim->max_sectors를 각각 bdi->ra_pages(read-ahead 크기)와
 *        bdi->io_pages(writeback 한 번에 내보낼 페이지 수)로 변환해 넣는다.
 *        즉 큐 한계가 페이지 캐시의 선반입/기록 정책으로 전파되는 통로다.
 */
/*
 * [한국어]
 * blk_queue_rq_timeout - 큐 단위 request 타임아웃 값 설정
 *
 * @q: 타임아웃을 적용할 request_queue. NVMe라면 blk_mq_init_queue()로 만들어진
 *     admin/IO 큐.
 * @timeout: 새 타임아웃 값(jiffies 단위). 호출자가 msecs_to_jiffies() 등으로
 *     이미 변환해서 전달한다고 가정한다.
 * @return: 없음 (void). 검증 없이 항상 성공.
 *
 * NVMe 컨트롤러는 명령 제출 후 완료를 받지 못하면 blk-mq의 워치독 타이머가
 * nvme_timeout()을 호출해 abort/controller reset을 트리거하는데, 이때 기준
 * 시간이 q->rq_timeout이다. NVMe 드라이버는 컨트롤러 특성(PCIe vs Fabrics,
 * 링크 상태 등)에 맞춰 이 값을 조정하려고 본 함수를 호출한다. 동작은
 * WRITE_ONCE 한 줄뿐이며 별도 검증이나 락이 없다.
 * 실행 컨텍스트: 드라이버 probe/reset 경로의 프로세스 컨텍스트. 재진입 보호가
 * 없으므로 호출자가 큐 초기화 시점에만 호출하도록 보장해야 한다.
 * 호출자: nvme_alloc_admin_tag_set(), nvme_alloc_io_tag_set() 등 드라이버 큐
 * 초기화 경로. 호출 대상: 없음(단순 필드 대입으로 종료).
 * 에러 경로: 없음(void, 항상 성공).
 *
 * 호출 체인:
 *   nvme_alloc_admin_tag_set/nvme_alloc_io_tag_set → [blk_queue_rq_timeout] → (필드 대입으로 종료)
 */
void blk_queue_rq_timeout(struct request_queue *q, unsigned int timeout)
{
	/* q->rq_timeout은 nvme_timeout -> nvme_abort_cmd -> Abort 명령 CID
	 * 선택 기준이 되며, jiffies 단위로 변환되어 nvme watchdog에서 사용된다.
	 * WRITE_ONCE: 타임아웃 워커(다른 CPU/인터럽트 컨텍스트)가 락 없이 이
	 * 필드를 읽으므로, 컴파일러의 재정렬/분할 저장을 막기 위해 사용한다. */
	WRITE_ONCE(q->rq_timeout, timeout);
}
EXPORT_SYMBOL_GPL(blk_queue_rq_timeout);

/**
 * blk_set_stacking_limits - set default limits for stacking devices
 * @lim:  the queue_limits structure to reset
 *
 * Prepare queue limits for applying limits from underlying devices using
 * blk_stack_limits().
 *
 * NVMe 관점:
 *   상위 가상 장치(MD/DM 등)가 하위 NVMe SSD의 q->limits를 상속받기 전에
 *   lim을 "제한 없음" 상태로 초기화한다. 이후 blk_stack_limits()에서
 *   NVMe max_hw_sectors, max_segments, discard, zoned 등을 교차 병합한다.
 */
/*
 * [한국어]
 * blk_set_stacking_limits - 스택형(가상) 장치용 queue_limits를 "제한 없음" 상태로 초기화
 *
 * @lim: 초기화할 queue_limits. MD/DM 등 스택 드라이버가 자신의 request_queue를
 *     만들기 직전에 스택 위에 얹을 lim을 가리킨다.
 * @return: 없음 (void). 항상 성공.
 *
 * blk_stack_limits()는 각 필드를 t(상위)와 b(하위 NVMe 등) 사이의 min/max로
 * 병합하는데, 병합 연산의 항등원(identity)이 없으면 첫 하위 장치를 병합할 때
 * 기존 값과 충돌한다. 그래서 본 함수는 min으로 병합될 필드는 UINT_MAX/
 * USHRT_MAX로, max로 병합될 필드는 SECTOR_SIZE 등 가장 보수적인 값으로
 * 채워 "아직 아무 제약도 걸리지 않은" 중립 상태를 만든다. 이후 첫 번째
 * blk_stack_limits() 호출에서 실제 하위 NVMe 장치의 값으로 대체된다.
 * 실행 컨텍스트: MD/DM 큐 생성 경로의 프로세스 컨텍스트, 동시성 걱정 없는
 * 지역 구조체 초기화.
 * 호출자: drivers/md/md.c, drivers/md/dm-table.c 등 스택 드라이버의 큐 초기화.
 * 호출 대상: memset() 뿐이며, 이후 blk_stack_limits()가 별도로 호출되어
 * 실제 병합을 수행한다(본 함수 내부에서 호출하지 않음).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   md_run/dm_table_add_target → [blk_set_stacking_limits] → (이후 blk_stack_limits가 반복 호출됨)
 */
void blk_set_stacking_limits(struct queue_limits *lim)
{
	/* [한국어] 여기 채우는 값들은 **스택형 장치(MD/DM)의 출발점**이지 특정 장치의
	 * 값이 아니다. 아래 blk_stack_limits() 가 하위 장치들의 값을 하나씩 병합해
	 * 들어오므로, 각 필드는 "병합에 영향을 주지 않는 중립 원소"로 시작해야 한다 —
	 * min 으로 합쳐질 필드는 최댓값(USHRT_MAX 등)으로, max 로 합쳐질 필드는
	 * 최솟값(0)으로 두는 것이 그 이유다.
	 *
	 * NVMe 독자 주의: 아래 값들은 NVMe 가 쓰는 값이 아니다. NVMe 는
	 * nvme_set_ctrl_limits() / nvme_update_disk_info() 에서 자기 값으로 덮어쓴다.
	 * 특히 dma_alignment 는 대비가 크다 — 여기 기본값은 511(512B 정렬)이지만
	 * NVMe 는 3(4바이트 정렬)으로 훨씬 관대하다. 그래서 NVMe 에서는 어지간한
	 * 사용자 버퍼가 bounce 없이 그대로 매핑된다. */
	memset(lim, 0, sizeof(*lim));
	/* [한국어] 논리 블록 크기 기본값 512B — 커널이 섹터를 세는 단위와 같다 */
	lim->logical_block_size = SECTOR_SIZE;
	/* [한국어] 물리 블록 크기도 일단 같게 둔다. 4Kn 이나 512e 장치가 붙으면 그때 커진다 */
	lim->physical_block_size = SECTOR_SIZE;
	/* [한국어] 최소 권장 IO 크기. 물리 블록 크기와 같게 시작한다 */
	lim->io_min = SECTOR_SIZE;
	/* [한국어] discard 단위 기본값 */
	lim->discard_granularity = SECTOR_SIZE;
	/* [한국어] DMA 시작 주소 정렬 요구(마스크 형태). 511 = 512B 경계 요구라는 뜻이며,
	 * 이는 보수적인 기본값이다. 실제 장치가 더 관대하면(NVMe 는 3) 그 값으로 덮인다. */
	lim->dma_alignment = SECTOR_SIZE - 1;
	/* [한국어] 하나의 세그먼트가 넘어서는 안 되는 물리 주소 경계(기본 4GB).
	 * 32비트 DMA 엔진이 주소 상위를 못 넘기는 경우를 표현하는 값이다. */
	lim->seg_boundary_mask = BLK_SEG_BOUNDARY_MASK;

	/* Inherit limits from component devices */
	/* [한국어] 아래 세 값은 min 으로 병합되므로 최댓값에서 시작해야 한다.
	 * 0 으로 두면 첫 병합에서 0 이 되어 "세그먼트를 하나도 못 쓴다"가 되어 버린다. */
	lim->max_segments = USHRT_MAX;
	/* [한국어] discard 요청 하나에 담을 수 있는 구간 수의 상한 */
	lim->max_discard_segments = USHRT_MAX;
	/* [한국어] 아래는 전부 같은 이유로 UINT_MAX 다 — 하위 장치 값과 min 으로
	 * 합쳐지므로 "아직 아무 제한도 없다"를 최댓값으로 표현한다. 각 필드가
	 * 실제로 무엇을 제한하는지는 다음과 같다. */
	/* [한국어] 장치가 한 명령으로 옮길 수 있는 최대 섹터 수. NVMe 는 MDTS 에서 유도한다 */
	lim->max_hw_sectors = UINT_MAX;
	/* [한국어] 세그먼트 하나의 바이트 상한. NVMe 는 UINT_MAX 로 두어 사실상 무제한이다 */
	lim->max_segment_size = UINT_MAX;
	/* [한국어] 실제 적용되는 IO 크기 상한. hw 한계와 사용자 sysfs 설정의 교집합이다 */
	lim->max_sectors = UINT_MAX;
	/* [한국어] 장치 자체가 보고한 상한. 프로토콜 계층(SCSI ULP 등)이 별도로 두는 값이며
	 * NVMe 는 이 필드를 쓰지 않고 max_hw_sectors 만 채운다 */
	lim->max_dev_sectors = UINT_MAX;
	/* [한국어] Write Zeroes 한 번의 최대 섹터. NVMe 는 opcode 0x08 로 매핑되며
	 * WZSL(Identify) 이 있으면 그 값에서 유도한다 */
	lim->max_write_zeroes_sectors = UINT_MAX;
	/* [한국어] "0 을 쓰면서 동시에 할당까지 해제"할 수 있는 상한.
	 * NVMe 에서는 Write Zeroes 의 DEAC 비트에 해당하며, 커널은 아주 좁은
	 * 조건에서만 이것을 켠다(core.c:2840):
	 *     (id->dlfeat & 0x7) == 0x1 && (id->dlfeat & (1 << 3))
	 * 즉 DLFEAT 이 "해제된 블록을 읽으면 0 이 나온다"를 보장하고(하위 3비트 == 1),
	 * 동시에 DEAC 지원을 광고할 때(비트 3)만이다. 보장이 없으면 DEAC 를 켜도
	 * 읽었을 때 0 이 아닌 값이 나올 수 있어 Write Zeroes 의 의미가 깨진다. */
	lim->max_hw_wzeroes_unmap_sectors = UINT_MAX;
	/* [한국어] 위 값의 사용자 조정판(sysfs). 하드웨어 한계와 min 으로 합쳐진다 */
	lim->max_user_wzeroes_unmap_sectors = UINT_MAX;
	/* [한국어] Zone Append 한 번의 최대 섹터. NVMe ZNS 는 ZASL 에서 유도하며,
	 * 이 값이 0 이 아니라는 사실 자체가 "네이티브 append 지원"의 판정 기준이다 */
	lim->max_hw_zone_append_sectors = UINT_MAX;
	/* [한국어] discard 상한의 사용자 조정판(sysfs) */
	lim->max_user_discard_sectors = UINT_MAX;
	/* [한국어] 원자적 쓰기 하드웨어 상한. NVMe 는 (1 + NAWUPF) * 논리블록크기 */
	lim->atomic_write_hw_max = UINT_MAX;
}
EXPORT_SYMBOL(blk_set_stacking_limits);

/**
 * blk_apply_bdi_limits - NVMe optimal I/O 크기를 read-ahead/writeback에 반영
 * @bdi:  상위 block device의 backing_dev_info
 * @lim:  정규화된 queue_limits
 *
 * NVMe 연결 지점:
 *   lim->io_opt는 NVMe 컨트롤러가 권장하는 최적 I/O 크기(Optimal Write Size 등).
 *   bdi->ra_pages, bdi->io_pages가 이 값을 반영하면 NVMe SQ 엔트리 효율과
 *   read-ahead 적중률이 개선된다.
 *   NVMe SSD는 기본적으로 BLK_FEAT_ROTATIONAL이 꺼져 있다 —
 *   drivers/nvme/host/core.c는 info->is_rotational일 때만(즉 컨트롤러가
 *   스스로 회전 매체라고 보고한 드문 경우에만) 이 플래그를 세운다.
 *   따라서 일반적인 NVMe SSD는 아래의 "회전 매체 fallback" 경로를 타지 않고,
 *   io_opt가 0이면 ra_pages가 VM_READAHEAD_PAGES 기본값에 머문다.
 *   회전 디스크에만 큰 fallback read-ahead를 주는 이유는, 탐색 비용이 큰
 *   매체에서는 넉넉히 미리 읽는 편이 거의 항상 이득이기 때문이다.
 *   NVMe는 탐색이 없어 과도한 선반입이 오히려 대역폭 낭비가 된다.
 */
/*
 * [한국어]
 * blk_apply_bdi_limits - queue_limits의 최적 I/O 크기를 backing_dev_info(bdi)에 반영
 *
 * @bdi: 큐가 속한 gendisk의 backing_dev_info. VFS의 read-ahead/writeback
 *     서브시스템이 참조하는 페이지 수 힌트를 담는다.
 * @lim: blk_validate_limits()로 이미 정규화된 queue_limits. io_opt, max_sectors,
 *     features(BLK_FEAT_ROTATIONAL 등)를 읽기만 하고 수정하지 않는다.
 * @return: 없음 (void). 실패 조건이 없다.
 *
 * VFS read-ahead가 효율적이려면 최적 I/O 크기의 최소 2배를 미리 읽어야
 * 한다는 경험칙에 따라 ra_pages를 계산한다. 순회식(rotational) 장치가
 * io_opt를 보고하지 않는 경우에는 max_sectors를 대신 사용해 작은 기본
 * read-ahead로 떨어지는 것을 막는다. NVMe SSD는 BLK_FEAT_ROTATIONAL이
 * 꺼져 있으므로 이 fallback 경로를 타지 않고, io_opt=0이면 ra_pages는
 * VM_READAHEAD_PAGES 최소값만 보장된다. 사용자가 sysfs로 이미 ra_pages를
 * 늘려둔 경우를 존중하기 위해 max3()로 "줄어들지 않게"만 계산한다.
 * io_pages는 max_sectors를 PAGE 단위로 환산한 값으로, writeback 시 한
 * 번에 내보낼 페이지 수의 상한이 된다.
 * 실행 컨텍스트: queue_limits_commit_update() 등 큐 갱신 경로의 프로세스
 * 컨텍스트. bdi 필드에 대한 별도 락은 없고 큐 초기화/갱신 시점에서만
 * 호출된다고 가정한다.
 * 호출자: queue_limits_commit_update(). 호출 대상: max3(), lcm 계열 없음
 * (산술 매크로만 사용, 하위 함수 호출 없음).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   queue_limits_commit_update → [blk_apply_bdi_limits] → (bdi 필드 대입으로 종료)
 */
void blk_apply_bdi_limits(struct backing_dev_info *bdi,
		struct queue_limits *lim)
{
	/* NVMe io_opt(컨트롤러 권장 최적 I/O 크기)를 읽어와 지역 변수에 보관 -
	 * 아래에서 rotational fallback으로 값을 바꿀 수 있어 lim을 직접 수정하지
	 * 않고 복사본을 사용한다. */
	u64 io_opt = lim->io_opt;

	/*
	 * For read-ahead of large files to be effective, we need to read ahead
	 * at least twice the optimal I/O size. For rotational devices that do
	 * not report an optimal I/O size (e.g. ATA HDDs), use the maximum I/O
	 * size to avoid falling back to the (rather inefficient) small default
	 * read-ahead size.
	 *
	 * There is no hardware limitation for the read-ahead size and the user
	 * might have increased the read-ahead size through sysfs, so don't ever
	 * decrease it.
	 */
	/* NVMe SSD는 BLK_FEAT_ROTATIONAL 미설정이므로 io_opt=0이면 아래 fallback
	 * 경로로 들어가지 않고 max_sectors 기반 read-ahead도 사용하지 않는다. */
	if (!io_opt && (lim->features & BLK_FEAT_ROTATIONAL))
		io_opt = (u64)lim->max_sectors << SECTOR_SHIFT;

	/* NVMe io_opt 기반 read-ahead 페이지 수 = 최소 2*io_opt/PAGE. */
	bdi->ra_pages = max3(bdi->ra_pages,
				io_opt * 2 >> PAGE_SHIFT,
				VM_READAHEAD_PAGES);
	/* writeback 단위: NVMe max_sectors/PAGE 수로, SQ 엔트리당 전송 크기에
	 * 직접 영향을 준다. */
	bdi->io_pages = lim->max_sectors >> PAGE_SECTORS_SHIFT;
}

/**
 * blk_validate_zoned_limits - ZNS(Zoned Namespace) NVMe 한도 검증
 * @lim: 검증할 queue_limits
 *
 * NVMe 연결 지점:
 *   NVMe ZNS SSD는 zone open/active 수, zone append 크기, zone write granularity
 *   등을 Identify Namespace/Controller 데이터로 보고한다.
 *   BLK_FEAT_ZONED가 설정되지 않은 일반 NVMe namespace에서는 관련 필드가 0이어야
 *   하며, 그렇지 않으면 -EINVAL을 반환한다.
 */
/*
 * [한국어]
 * blk_validate_zoned_limits - ZNS(Zoned Namespace) 관련 queue_limits 검증/보정
 *
 * @lim: 검증할 queue_limits. features, max_open_zones, max_active_zones,
 *     zone_write_granularity, max_hw_zone_append_sectors, chunk_sectors,
 *     max_hw_sectors 필드를 읽고 zone_write_granularity/max_zone_append_sectors를
 *     보정해 채운다.
 * @return: 성공 시 0, 필드 조합이 모순되면 -EINVAL. blk_validate_limits()가
 *     마지막 단계에서 호출하며, 실패하면 전체 검증도 실패로 전파된다.
 *
 * NVMe ZNS 네임스페이스는 Identify Namespace의 Zone Descriptor Extension,
 * Identify Controller의 ZASL(Zone Append Size Limit) 등에서 zone 관련 한도를
 * 얻는다. BLK_FEAT_ZONED가 꺼져 있는 일반 NVMe 네임스페이스인데 zone 관련
 * 필드가 잔존(residual)해 있다면 드라이버 버그이므로 WARN_ON_ONCE로 알리고
 * -EINVAL을 반환한다. BLK_FEAT_ZONED가 켜져 있으면: (1) 커널이
 * CONFIG_BLK_DEV_ZONED로 빌드됐는지 확인하고, (2) active zone 수가 open
 * zone 수 이상인지 검사하고(활성 zone은 열린 zone을 포함하는 상위 집합),
 * (3) zone_write_granularity가 logical_block_size 미만이면 논리 블록
 * 크기로 올려 보정하고, (4) Zone Append 명령이 zone 경계를 넘을 수 없다는
 * 제약을 반영해 max_zone_append_sectors를 HW 한도, chunk_sectors(zone
 * 크기), max_hw_sectors(MDTS) 중 최솟값으로 확정한다.
 * 실행 컨텍스트: blk_validate_limits() 호출 경로의 프로세스 컨텍스트(드라이버
 * probe 또는 큐 갱신). 동시성 보호는 상위 limits_lock에 의존한다.
 * 호출자: blk_validate_limits(). 호출 대상: 없음 (매크로/산술 연산만 사용).
 * 에러 경로: 조건 위반 시 -EINVAL을 반환하며 blk_validate_limits()가 이를
 * 그대로 상위(드라이버 probe)로 전파해 큐 생성/갱신을 실패시킨다.
 *
 * 호출 체인:
 *   blk_validate_limits → [blk_validate_zoned_limits] → (없음, 순수 검증/대입)
 */
static int blk_validate_zoned_limits(struct queue_limits *lim)
{
	/* BLK_FEAT_ZONED 미설정 시 ZNS 관련 값이 residual하면 컨트롤러 상태 불일치. */
	if (!(lim->features & BLK_FEAT_ZONED)) {
		if (WARN_ON_ONCE(lim->max_open_zones) ||
		    WARN_ON_ONCE(lim->max_active_zones) ||
		    WARN_ON_ONCE(lim->zone_write_granularity) ||
		    WARN_ON_ONCE(lim->max_zone_append_sectors))
			return -EINVAL;
		return 0;
	}

	/* ZNS 지원을 알리는 BLK_FEAT_ZONED가 켜져 있는데 커널이 zoned block
	 * device 지원 없이 빌드됐다면 상위 zoned 관련 코드가 전혀 없으므로
	 * 즉시 오류로 처리한다. */
	if (WARN_ON_ONCE(!IS_ENABLED(CONFIG_BLK_DEV_ZONED)))
		return -EINVAL;

	/*
	 * Given that active zones include open zones, the maximum number of
	 * open zones cannot be larger than the maximum number of active zones.
	 */
	/* NVMe ZNS: Open Zone 수는 Active Zone 수를 초과할 수 없음(Identify). */
	if (lim->max_active_zones &&
	    lim->max_open_zones > lim->max_active_zones)
		return -EINVAL;

	/* NVMe ZNS zone write granularity는 최소 LBA 크기 이상이어야 함. */
	if (lim->zone_write_granularity < lim->logical_block_size)
		lim->zone_write_granularity = lim->logical_block_size;

	/*
	 * The Zone Append size is limited by the maximum I/O size and the zone
	 * size given that it can't span zones.
	 *
	 * If no max_hw_zone_append_sectors limit is provided, the block layer
	 * will emulated it, else we're also bound by the hardware limit.
	 *
	 * NVMe ZNS: Zone Append 명령은 zone 경계를 넘을 수 없으므로 MDTS와
	 * zone 크기 중 작은 값으로 제한한다.
	 */
	/* NVMe ZNS Zone Append 최종 한도 = min(HW limit, zone 크기, MDTS). */
	lim->max_zone_append_sectors =
		min_not_zero(lim->max_hw_zone_append_sectors,
			min(lim->chunk_sectors, lim->max_hw_sectors));
	return 0;
}

/**
 * blk_validate_integrity_limits - NVMe DIF/DIX end-to-end data protection 검증
 * @lim: 검증할 queue_limits
 *
 * NVMe 연결 지점:
 *   NVMe PI(Protection Information, DIF/DIX) 설정은 Identify Namespace의
 *   E2E Data Protection Type, Formats로부터 채워진다.
 *   metadata_size, pi_tuple_size, csum_type, interval_exp, tag_size를 검증하여
 *   상위 bio 경로에서 잘못된 PI 조합이 생성되지 않도록 한다.
 *
 * struct blk_integrity NVMe 관련 필드 설명:
 *   metadata_size: NVMe DIF/DIX 보호 정보(metadata) 바이트 크기.
 *   pi_tuple_size: T10 PI/CRC64 PI tuple 크기.
 *   csum_type: CRC, IP, CRC64 등 NVMe E2E checksum 종류.
 *   interval_exp: PI 검증 간격(2^x 바이트), NVMe LBA 데이터 크기와 연관.
 *   tag_size: NVMe PI Reftag 크기.
 *   flags: NOGENERATE/NOVERIFY/DEVICE_CAPABLE/REF_TAG/SPLIT_INTERVAL_CAPABLE
 *          등 DIF 처리 방식을 지정.
 *
 * [한국어 보강]
 * @lim: (위와 동일) integrity 서브필드를 in-place로 검증/보정한다.
 * @return: 성공 시 0, PI 필드 조합이 스펙과 어긋나면 -EINVAL,
 *     CONFIG_BLK_DEV_INTEGRITY가 꺼져 있는데 metadata_size>0이면 -EINVAL.
 *
 * PI가 전혀 없는 경우(metadata_size==0)는 관련 필드도 모두 0이어야 하며,
 * 그렇지 않으면 드라이버가 절반만 설정한 것이므로 거부한다. PI가 있는
 * 경우 pi_offset+pi_tuple_size가 metadata_size를 넘지 않는지, csum_type별로
 * pi_tuple_size가 표준 크기(T10 PI 8바이트, CRC64 PI 16바이트)와 일치하는지
 * 검사한다. interval_exp가 0이면 logical_block_size의 log2 값으로 기본
 * 설정하고, dma_alignment를 interval 경계에 맞춰 보강해 PRP/SGL 엔트리가
 * PI 검증 구간을 가로지르지 않게 한다. 마지막으로 block layer가 자동
 * 생성/검증하는 PI 메타데이터가 한 세그먼트에 다 들어가도록 max_sectors를
 * 추가로 제한한다.
 * 실행 컨텍스트: blk_validate_limits() 호출 경로의 프로세스 컨텍스트.
 * 호출자: blk_validate_limits(). 호출 대상: max_integrity_io_size() (blk.h),
 * ilog2(), pr_warn().
 * 에러 경로: 위반 시 -EINVAL 반환, blk_validate_limits()가 그대로 전파.
 *
 * 호출 체인:
 *   blk_validate_limits → [blk_validate_integrity_limits] → max_integrity_io_size()
 */
static int blk_validate_integrity_limits(struct queue_limits *lim)
{
	/* [한국어] ===== 이 검증이 무엇을 확인하는가 (NVMe 기준 실물 대조) =====
	 * 드라이버가 채워 넣은 blk_integrity 가 자기모순 없는 조합인지 본다.
	 * NVMe 가 이 구조체를 채우는 곳은 nvme_init_integrity()(core.c:2200~2253)이며,
	 * Identify Namespace 의 DPS(PI Type)와 guard type 을 다음과 같이 옮긴다:
	 *
	 *   PI Type 1/2  → BLK_INTEGRITY_REF_TAG 설정, tag_size = sizeof(u16)
	 *                  (앱 태그만; 레퍼런스 태그는 플래그로 표현)
	 *   PI Type 3    → REF_TAG 없음. Type 3 은 레퍼런스 태그를 쓰지 않기 때문이다.
	 *                  tag_size = u16 + u32 (16B guard) 또는 u16 + 6 (64B guard)
	 *   16B guard    → csum_type = BLK_INTEGRITY_CSUM_CRC    (CRC16)
	 *   64B guard    → csum_type = BLK_INTEGRITY_CSUM_CRC64
	 *   그리고 공통으로
	 *     bi->metadata_size = head->ms          // LBA 당 메타데이터 바이트 수
	 *     bi->pi_tuple_size = head->pi_size     // 8 또는 16
	 *     bi->pi_offset     = info->pi_offset   // 메타데이터 안에서 PI 의 위치
	 *     bi->flags |= BLK_INTEGRITY_DEVICE_CAPABLE  // 장치가 PRACT 로 직접 처리 가능
	 *
	 * 참고: NVMe 는 BLK_INTEGRITY_CSUM_IP 를 절대 설정하지 않는다. 아래 switch 에
	 * 그 케이스가 CRC 와 함께 묶여 있는 것은 SCSI(DIF Type 1 의 IP 체크섬) 때문이다.
	 * ================================================================= */
	struct blk_integrity *bi = &lim->integrity;

	/* [한국어] metadata_size 가 0 이면 애초에 메타데이터 영역이 없다는 뜻이므로,
	 * 그 안에 놓일 체크섬·태그가 설정되어 있으면 앞뒤가 맞지 않는다.
	 * NVMe 에서 이 경우는 PI 를 쓰지 않는 평범한 네임스페이스(ms == 0)다. */
	if (!bi->metadata_size) {
		if (bi->csum_type != BLK_INTEGRITY_CSUM_NONE ||
		    bi->tag_size || ((bi->flags & BLK_INTEGRITY_REF_TAG))) {	/* [한국어] 셋 중 하나라도 설정돼 있으면 드라이버 버그다 */
			pr_warn("invalid PI settings.\n");
			return -EINVAL;
		}
		/* PI offloads 안 됨: block layer가 generate/verify하지 않음. */
		bi->flags |= BLK_INTEGRITY_NOGENERATE | BLK_INTEGRITY_NOVERIFY;
		return 0;
	}

	/* metadata_size>0(PI 사용 요청)인데 커널이 integrity 지원 없이
	 * 빌드됐다면 상위 blk-integrity 인프라 자체가 없으므로 거부. */
	if (!IS_ENABLED(CONFIG_BLK_DEV_INTEGRITY)) {
		pr_warn("integrity support disabled.\n");
		return -EINVAL;
	}

	/* [한국어] 레퍼런스 태그는 체크섬 없이 홀로 설 수 없다. 레퍼런스 태그가 하는 일은
	 * "이 블록이 정말 그 LBA 의 것인가"(오배치 검출)인데, 그 태그 자체가 손상되지
	 * 않았음을 보장하는 것이 가드(체크섬)이기 때문이다. 검증 순서상 가드가 먼저다. */
	if (bi->csum_type == BLK_INTEGRITY_CSUM_NONE &&
	    (bi->flags & BLK_INTEGRITY_REF_TAG)) {
		pr_warn("ref tag not support without checksum.\n");
		return -EINVAL;
	}

	/* [한국어] PI 튜플은 메타데이터 영역 **안에** 완전히 들어가야 한다.
	 * NVMe 메타데이터는 PI 만 담는 것이 아니라 그 앞뒤에 벤더 영역이 있을 수 있고,
	 * pi_offset 이 그 시작 위치다(그래서 0 이 아닐 수 있다).
	 * 이 검사가 없으면 PI 를 읽고 쓰는 코드가 메타데이터 버퍼 밖을 넘본다. */
	if (bi->pi_offset + bi->pi_tuple_size > bi->metadata_size) {
		pr_warn("pi_offset (%u) + pi_tuple_size (%u) exceeds metadata_size (%u)\n",
			bi->pi_offset, bi->pi_tuple_size, bi->metadata_size);
		return -EINVAL;
	}

	/* [한국어] 체크섬 방식마다 튜플 구조체가 정해져 있으므로 크기가 정확히 맞아야 한다.
	 * 여기서 걸린다면 장치가 보고한 pi_size 와 커널이 아는 레이아웃이 어긋난 것이고,
	 * 그대로 두면 가드/앱태그/레퍼런스태그를 엉뚱한 오프셋에서 읽게 된다. */
	switch (bi->csum_type) {
	case BLK_INTEGRITY_CSUM_NONE:
		/* [한국어] 체크섬이 없으면 튜플도 없어야 한다 */
		if (bi->pi_tuple_size) {
			pr_warn("pi_tuple_size must be 0 when checksum type is none\n");
			return -EINVAL;
		}
		break;
	case BLK_INTEGRITY_CSUM_CRC:	/* [한국어] NVMe 16B guard 가 여기로 온다 (CRC16) */
	case BLK_INTEGRITY_CSUM_IP:	/* [한국어] SCSI 전용 — NVMe 는 이 값을 설정하지 않는다 */
		/* [한국어] 둘 다 T10 PI 레이아웃(guard 2 + app 2 + ref 4 = 8바이트)을 공유한다 */
		if (bi->pi_tuple_size != sizeof(struct t10_pi_tuple)) {
			pr_warn("pi_tuple_size mismatch for T10 PI: expected %zu, got %u\n",
				 sizeof(struct t10_pi_tuple),
				 bi->pi_tuple_size);
			return -EINVAL;
		}
		break;
	case BLK_INTEGRITY_CSUM_CRC64:	/* [한국어] NVMe 64B guard (확장 PI) */
		/* [한국어] guard 8 + app 2 + ref 6 = 16바이트. 가드가 넓어진 만큼 튜플도 커진다 */
		if (bi->pi_tuple_size != sizeof(struct crc64_pi_tuple)) {
			pr_warn("pi_tuple_size mismatch for CRC64 PI: expected %zu, got %u\n",
				 sizeof(struct crc64_pi_tuple),
				 bi->pi_tuple_size);
			return -EINVAL;
		}
		break;
	}

	/* [한국어] interval 은 "PI 튜플 하나가 보호하는 데이터 구간"의 크기다.
	 * 지정되지 않았으면 논리 블록 하나로 잡는다 — NVMe 는 LBA 당 메타데이터를
	 * 하나씩 두므로 이것이 자연스러운 기본값이다. */
	if (!bi->interval_exp) {
		bi->interval_exp = ilog2(lim->logical_block_size);
	} else if (bi->interval_exp < SECTOR_SHIFT ||	/* [한국어] 512B 보다 잘게 보호할 수는 없다 */
		   bi->interval_exp > ilog2(lim->logical_block_size)) {	/* [한국어] 논리 블록보다 크게 잡으면 블록 경계와 어긋난다 */
		/* 드라이버가 명시적으로 지정한 interval_exp가 섹터 크기(2^9)보다
		 * 작거나 LBA 크기보다 크면 범위를 벗어난 것이므로 거부. */
		pr_warn("invalid interval_exp %u\n", bi->interval_exp);
		return -EINVAL;
	}

	/*
	 * Some IO controllers can not handle data intervals straddling
	 * multiple bio_vecs.  For those, enforce alignment so that those are
	 * never generated, and that each buffer is aligned as expected.
	 *
	 * [한국어] 무결성 "interval"은 체크섬 하나가 보호하는 데이터 구간
	 * (보통 논리 블록 하나, 512B 또는 4096B)이다. 일부 I/O 컨트롤러는 이
	 * 구간이 두 개의 bio_vec에 걸쳐 있으면 체크섬을 계산하지 못한다 —
	 * 하드웨어가 한 세그먼트 안에서만 검사 엔진을 돌릴 수 있기 때문이다.
	 *
	 * BLK_SPLIT_INTERVAL_CAPABLE은 "우리 하드웨어는 걸쳐 있어도 처리할 수
	 * 있다"고 드라이버가 선언하는 플래그다. 이 트리에서 이를 세우는 것은
	 * drivers/block/ublk_drv.c(사용자 공간 블록 드라이버)뿐이며, NVMe 드라이버는
	 * 세우지 않는다. 따라서 PI를 쓰는 NVMe 구성에서는 아래 정렬 강화가 항상
	 * 적용된다.
	 *
	 * 정렬을 강화하면 bio_split_io_at()의 start_align_mask/len_align_mask
	 * 검사가 더 엄격해져, 애초에 interval을 가로지르는 bvec 조합이 만들어지지
	 * 않는다. 즉 "나중에 처리하지 못할 요청을 미리 차단"하는 방식이다.
	 */
	/* [한국어] 조건: 하드웨어가 걸침을 감당하지 못하고(!CAPABLE), 실제로
	 * 체크섬을 쓰는 경우(csum_type != 0). 체크섬이 없으면 interval 개념
	 * 자체가 무의미하므로 정렬을 강화할 이유가 없다. */
	if (!(bi->flags & BLK_SPLIT_INTERVAL_CAPABLE) && bi->csum_type) {
		/* [한국어] dma_alignment를 interval 크기 - 1로 끌어올린다.
		 * interval_exp가 9(512B)면 511, 12(4096B)면 4095가 되어, 모든 bvec의
		 * 시작 오프셋과 길이가 그 단위에 맞아야만 통과하게 된다.
		 * max()를 쓰는 이유: 기존 dma_alignment(NVMe는 3)가 더 엄격할 수도
		 * 있으므로 둘 중 강한 쪽을 택해 양쪽 제약을 모두 만족시킨다. */
		lim->dma_alignment = max(lim->dma_alignment,
					(1U << bi->interval_exp) - 1);
	}

	/*
	 * The block layer automatically adds integrity data for bios that don't
	 * already have it.  Limit the I/O size so that a single maximum size
	 * metadata segment can cover the integrity data for the entire I/O.
	 *
	 * NVMe: PI metadata를 담을 수 있는 단일 메타데이터 세그먼트가 전체 I/O를
	 * 커버할 수 있도록 max_sectors를 추가로 제한한다.
	 */
	/* NVMe PI metadata 버퍼 용량으로 max_sectors 추가 제한. */
	lim->max_sectors = min(lim->max_sectors,
		max_integrity_io_size(lim) >> SECTOR_SHIFT);

	return 0;
}

/*
 * Returns max guaranteed bytes which we can fit in a bio.
 *
 * We request that an atomic_write is ITER_UBUF iov_iter (so a single vector),
 * so we assume that we can fit in at least PAGE_SIZE in a segment, apart from
 * the first and last segments.
 *
 * [한국어] NVMe에서 원자적 쓰기 보장의 근거는 Identify Namespace의 다음
 * 필드들이다(drivers/nvme/host/core.c:nvme_configure_atomic_write 참고):
 *   NAWUPF (Namespace Atomic Write Unit Power Fail)
 *     - 전원 손실 시에도 찢어지지 않음이 보장되는 최대 쓰기 단위.
 *       atomic_write_hw_max = (NAWUPF + 1) × logical_block_size 로 변환된다.
 *   NABSPF (Namespace Atomic Boundary Size Power Fail)
 *     - 이 경계를 걸치면 원자성이 깨지는 주소 경계.
 *       atomic_write_hw_boundary가 된다.
 *   NABO (Namespace Atomic Boundary Offset)
 *     - 경계가 0 이 아닌 오프셋에서 시작하는 경우를 나타낸다. 커널은 이것을
 *       지원하지 않아, nvme_configure_atomic_write() 는 id->nabo 가 0 이 아니면
 *       **원자적 쓰기를 통째로 끈다**(그냥 논리 블록 크기를 반환하고 끝낸다).
 *   AWUPF (컨트롤러 단위 값)
 *     - 커널은 이 값을 무시하고 NAWUPF만 신뢰한다("AWUPF ignored, only
 *       NAWUPF accepted" 경고를 dev_info_once 로 한 번 남긴다).
 *       컨트롤러 전역 값보다 네임스페이스별 값이 항상 더 정확하기 때문이다.
 *
 * 변환 결과(core.c:2420~2423)를 그대로 옮기면:
 *     atomic_write_hw_max       = (1 + NAWUPF) * bs      // 0's based
 *     atomic_write_hw_boundary  = (NABSPF + 1) * bs      // 없으면 0
 *     atomic_write_hw_unit_min  = bs                     // 논리 블록 하나
 *     atomic_write_hw_unit_max  = rounddown_pow_of_two(atomic_write_hw_max)
 * unit_max 만 2의 거듭제곱으로 내림한다는 점에 주의 — 그래야 아래 검증들이
 * 시프트/마스크 연산으로 성립한다.
 *
 * 주의: FUA(Force Unit Access)나 FAW(Firmware Activation Without Reset)는
 * 원자적 쓰기와 무관한 별개의 기능이다. FUA는 휘발성 캐시를 건너뛰고 매체에
 * 기록하라는 "지속성" 요구이고, FAW는 펌웨어 갱신 절차에 관한 것이다.
 * 여기서 다루는 것은 "찢어지지 않음(non-torn)"이라는 원자성 보장이다.
 *
 * 이 함수가 계산하는 것은 그런 하드웨어 한계와 별개로, "bio 하나에 확실히
 * 담을 수 있는 바이트 수"라는 소프트웨어 측 상한이다. 아래 영문 주석대로
 * 원자적 쓰기는 단일 벡터(ITER_UBUF)로 들어온다고 가정하므로, 첫 세그먼트와
 * 마지막 세그먼트를 뺀 나머지는 최소 PAGE_SIZE씩 담을 수 있다고 보수적으로
 * 계산한다.
 */
/*
 * [한국어]
 * blk_queue_max_guaranteed_bio - 단일 bio에 보장 가능한 최대 바이트 수 계산
 *
 * @lim: max_segments, logical_block_size를 참조하는 queue_limits (읽기 전용).
 * @return: 이 큐가 세그먼트 수/정렬 제약 하에서 "항상" 하나의 bio에 담을 수
 *     있다고 보장할 수 있는 최대 바이트 수. blk_atomic_writes_update_limits()가
 *     atomic write 단위 상한 계산에 사용한다.
 *
 * atomic write(원자적 쓰기)는 커널 내부에서 ITER_UBUF 방식의 단일 iov_iter로
 * 제출되므로 세그먼트 하나가 여러 페이지에 걸쳐 있어도 결과적으로 하나의
 * 연속된 사용자 버퍼로 취급된다. 다만 페이지 정렬이 어긋난 첫/마지막
 * 세그먼트는 최소 logical_block_size만큼만 보장할 수 있고, 중간 세그먼트는
 * 항상 PAGE_SIZE 전체를 채울 수 있다고 가정한다. 따라서 (min(세그먼트수,2) *
 * LBA크기) + (남은 세그먼트수 * PAGE_SIZE)로 계산한다. NVMe 관점에서 이
 * 값은 PRP/SGL 엔트리 수 제한과 LBA 정렬 요건을 동시에 만족하는 "보장
 * 가능한" atomic write 크기의 상한이 된다.
 * 실행 컨텍스트: blk_validate_atomic_write_limits() 호출 경로의 프로세스
 * 컨텍스트. 부수효과 없음(순수 계산 함수).
 * 호출자: blk_atomic_writes_update_limits(). 호출 대상: min()/제자리 산술뿐.
 * 에러 경로: 없음 (값 계산만 수행, 실패 없음).
 *
 * 호출 체인:
 *   blk_atomic_writes_update_limits → [blk_queue_max_guaranteed_bio] → (산술 계산으로 종료)
 */
static unsigned int blk_queue_max_guaranteed_bio(struct queue_limits *lim)
{
	/* NVMe atomic write: PRP/SGL 최대 엔트리 수와 BIO_MAX_VECS 중 작은 값. */
	unsigned int max_segments = min(BIO_MAX_VECS, lim->max_segments);
	unsigned int length; /* 최종 반환할 "보장 가능 바이트 수" 누산 변수. */

	/* 처음/마지막 세그먼트는 LBA 정렬을 위해 logical_block_size만 사용. */
	length = min(max_segments, 2) * lim->logical_block_size;
	/* 중간 세그먼트는 PAGE 단위로 DMA 매핑 가능. */
	if (max_segments > 2)
		length += (max_segments - 2) * PAGE_SIZE;

	/* 계산된 보장 가능 최대 바이트 수를 호출자(atomic write 단위 계산)에게 반환. */
	return length;
}

/*
 * [한국어]
 * blk_atomic_writes_update_limits - NVMe 원자적 쓰기(NAWUPF/NABSPF 유래) 최종 한도 계산
 *
 * @lim: atomic_write_hw_* (드라이버가 채운 하드웨어 원본 값) 필드를 읽어
 *     atomic_write_max_sectors/unit_min/unit_max/boundary_sectors(블록
 *     레이어가 노출할 최종 값)를 채우는 queue_limits.
 * @return: 없음 (void). 실패 조건 없음 - 이미 blk_validate_atomic_write_limits()
 *     에서 하드웨어 값의 유효성을 검증한 뒤에만 호출된다.
 *
 * NVMe Identify Controller가 보고하는 Atomic Write Unit Normal/Power Fail,
 * Atomic Boundary 같은 하드웨어 원시값(atomic_write_hw_*)을 그대로 상위에
 * 노출하면 MDTS(max_hw_sectors)나 단일 bio 보장 크기(guaranteed bio)를
 * 넘어설 수 있다. 그래서 먼저 unit_limit = min(MDTS 바이트, guaranteed bio
 * 바이트)를 구하고 2의 거듭제곱으로 내림해, 이 값과 하드웨어 unit_min/max를
 * 다시 min()으로 묶어 최종 atomic_write_unit_min/max를 정한다.
 * atomic_write_max_sectors는 HW 최대치와 MDTS 중 작은 값, boundary_sectors는
 * HW boundary를 섹터 단위로 환산한 값이다.
 * 실행 컨텍스트: blk_validate_atomic_write_limits() 내부, 프로세스 컨텍스트.
 * 호출자: blk_validate_atomic_write_limits(). 호출 대상:
 *     blk_queue_max_guaranteed_bio(), rounddown_pow_of_two().
 * 에러 경로: 없음 (호출 전에 이미 하드웨어 값이 검증된 상태).
 *
 * 호출 체인:
 *   blk_validate_atomic_write_limits → [blk_atomic_writes_update_limits] → blk_queue_max_guaranteed_bio
 */
static void blk_atomic_writes_update_limits(struct queue_limits *lim)
{
	/* 원자적 쓰기 단위는 MDTS(max_hw_sectors)와 bio가 보장하는 크기 중 작은 값으로 제한. */
	unsigned int unit_limit = min(lim->max_hw_sectors << SECTOR_SHIFT,
					blk_queue_max_guaranteed_bio(lim));

	/* [한국어] 2의 거듭제곱으로 내리는 것은 **커널의 요구**이지 컨트롤러의 요구가 아니다.
	 * NVMe 스펙은 NAWUPF 가 거듭제곱이어야 한다고 정하지 않는다. 커널이 이렇게 만드는
	 * 이유는 아래 정렬·경계 검사를 전부 시프트와 마스크로 처리하기 위해서다
	 * (드라이버 쪽에서도 nvme_configure_atomic_write() 가 unit_max 에 같은 처리를 한다).
	 * 그 대가로 예컨대 NAWUPF 가 48KB 를 뜻하면 32KB 까지만 원자성을 활용하게 된다. */
	unit_limit = rounddown_pow_of_two(unit_limit);

	/* NVMe atomic write 최종 최대 섹터 = min(HW 최대, MDTS). */
	lim->atomic_write_max_sectors =
		min(lim->atomic_write_hw_max >> SECTOR_SHIFT,
			lim->max_hw_sectors);
	/* NVMe atomic write 최종 최소 단위 = min(HW 최소 단위, unit_limit). */
	lim->atomic_write_unit_min =
		min(lim->atomic_write_hw_unit_min, unit_limit);
	/* NVMe atomic write 최종 최대 단위 = min(HW 최대 단위, unit_limit). */
	lim->atomic_write_unit_max =
		min(lim->atomic_write_hw_unit_max, unit_limit);
	/* NVMe atomic boundary를 섹터 단위로 환산해 최종 값으로 노출. */
	lim->atomic_write_boundary_sectors =
		lim->atomic_write_hw_boundary >> SECTOR_SHIFT;
}

/*
 * Test whether any boundary is aligned with any chunk size. Stacked
 * devices store any stripe size in t->chunk_sectors.
 */
/*
 * [한국어]
 * blk_valid_atomic_writes_boundary - atomic write 경계와 chunk 크기의 정렬 검사
 *
 * @chunk_sectors: RAID stripe 등 스택 장치가 저장한 chunk 크기(섹터 단위).
 *     0이면 스트라이핑이 없는 것으로 간주한다.
 * @boundary_sectors: NVMe atomic write boundary(섹터 단위). 이 경계를 넘는
 *     원자적 쓰기는 두 개로 쪼개질 수 있어 원자성이 깨진다.
 * @return: 두 값이 서로 배수 관계여서 함께 사용해도 안전하면 true, 한쪽이
 *     다른 쪽의 배수가 아니면(경계 불일치) false.
 *
 * chunk_sectors(스트라이프 크기)와 atomic write boundary가 서로 배수
 * 관계가 아니면, 원자적 쓰기가 stripe 경계를 넘거나 반대로 stripe가
 * atomic boundary 중간에서 끊겨 두 제약을 동시에 만족시킬 수 없다. 둘 중
 * 하나라도 0이면(제약이 없으면) 검사할 필요가 없으므로 true를 반환한다.
 * 실행 컨텍스트: blk_validate_atomic_write_limits(), blk_stack_atomic_writes_head()
 * 호출 경로의 프로세스 컨텍스트. 순수 함수(부수효과 없음).
 * 호출자: blk_validate_atomic_write_limits(), blk_stack_atomic_writes_head().
 * 호출 대상: 없음 (나눗셈 연산자만 사용).
 * 에러 경로: 없음 (bool만 반환, 호출자가 실패로 처리할지 결정).
 *
 * 호출 체인:
 *   blk_validate_atomic_write_limits/blk_stack_atomic_writes_head → [blk_valid_atomic_writes_boundary] → (산술 비교로 종료)
 */
static bool blk_valid_atomic_writes_boundary(unsigned int chunk_sectors,
					unsigned int boundary_sectors)
{
	/* chunk 또는 boundary가 0이면 정렬 조건 의미 없음. */
	if (!chunk_sectors || !boundary_sectors)
		return true;

	/* NVMe atomic boundary가 chunk(RAID stripe) 배수인지 검사. */
	if (boundary_sectors > chunk_sectors &&
	    boundary_sectors % chunk_sectors)
		return false;

	/* 반대로 chunk가 atomic boundary보다 크면 chunk가 boundary의 배수인지 검사. */
	if (chunk_sectors > boundary_sectors &&
	    chunk_sectors % boundary_sectors)
		return false;

	/* 배수 관계가 성립(또는 한쪽이 정확히 일치)하므로 두 제약이 호환됨. */
	return true;
}

/**
 * blk_validate_atomic_write_limits - NVMe 원자적 쓰기 한도 검증
 * @lim: 검증할 queue_limits
 *
 * NVMe 연결 지점:
 *   BLK_FEAT_ATOMIC_WRITES가 설정되면 컨트롤러가 보장하는 원자적 쓰기 단위를
 *   검증한다. NVMe 표준의 Atomic Write Unit Normal/Power Fail, Atomic Boundary
 *   등(Identify Controller)에서 채워진 값을 반영한다.
 *   단위는 2의 거듭제곱이어야 하며, chunk_sectors(RAID stripe)와 정렬해야 한다.
 */
/*
 * [한국어 보강]
 * @lim: (위와 동일) atomic_write_hw_* 원본 값을 검증하고, 실패 시 노출용
 *     atomic_write_* 필드를 모두 0으로 만들어 "지원 안 함"을 표시한다.
 * @return: 없음 (void). 성공/실패 여부는 lim->atomic_write_max_sectors 등이
 *     0인지 여부로 호출자가 판단한다(반환값이 아니라 필드로 결과 전달).
 *
 * NVMe Identify Controller의 AWUN(Atomic Write Unit Normal), AWUPF(Atomic
 * Write Unit Power Fail), Atomic Boundary 필드에서 얻은 하드웨어 원시값이
 * block layer가 요구하는 불변식(2의 거듭제곱, min<=max<=hw_max, RAID
 * stripe와의 정렬 등)을 만족하는지 하나씩 검사한다. 하나라도 어긋나면
 * unsupported 레이블로 점프해 모든 atomic_write_* 값을 0으로 지워 상위
 * 계층이 atomic write 기능을 아예 사용하지 못하게 한다. 모든 검사를
 * 통과하면 blk_atomic_writes_update_limits()를 호출해 MDTS/guaranteed
 * bio 크기까지 반영한 최종 한도를 확정한다.
 * 실행 컨텍스트: blk_validate_limits() 호출 경로의 프로세스 컨텍스트.
 * 호출자: blk_validate_limits(). 호출 대상: blk_valid_atomic_writes_boundary(),
 * blk_atomic_writes_update_limits(), is_power_of_2().
 * 에러 경로: 정식 에러 코드는 없다(void) - 실패 시 unsupported 레이블에서
 * 필드를 0으로 만드는 것 자체가 에러 표시 방식이다.
 *
 * 호출 체인:
 *   blk_validate_limits → [blk_validate_atomic_write_limits] → blk_valid_atomic_writes_boundary / blk_atomic_writes_update_limits
 */
static void blk_validate_atomic_write_limits(struct queue_limits *lim)
{
	unsigned int boundary_sectors; /* NVMe atomic boundary를 섹터 단위로 담을 지역 변수. */
	/* 원자적 쓰기 최대 바이트(NAWUPF 유래)를 섹터 단위로 변환. */
	unsigned int atomic_write_hw_max_sectors =
			lim->atomic_write_hw_max >> SECTOR_SHIFT;

	/* BLK_FEAT_ATOMIC_WRITES 미설정 시 NVMe atomic write 미지원으로 처리. */
	if (!(lim->features & BLK_FEAT_ATOMIC_WRITES))
		goto unsupported;

	/* UINT_MAX indicates stacked limits in initial state */
	/* NVMe: blk_set_stacking_limits()가 초기화한 UINT_MAX가 그대로 남아
	 * 있다면 실제 하위 장치 값이 병합되지 않은 것이므로 미지원 처리. */
	if (lim->atomic_write_hw_max == UINT_MAX)
		goto unsupported;

	/* NVMe atomic_write_hw_max가 0이면 컨트롤러가 atomic write를 아예
	 * 보고하지 않은 것이므로 미지원 처리. */
	if (!lim->atomic_write_hw_max)
		goto unsupported;

	/* NVMe atomic_write_hw_unit_min은 2의 거듭제곱이어야 함. */
	if (WARN_ON_ONCE(!is_power_of_2(lim->atomic_write_hw_unit_min)))
		goto unsupported;

	/* NVMe atomic_write_hw_unit_max도 2의 거듭제곱이어야 함. */
	if (WARN_ON_ONCE(!is_power_of_2(lim->atomic_write_hw_unit_max)))
		goto unsupported;

	/* NVMe atomic_write_hw_unit_min <= unit_max <= hw_max 검증. */
	if (WARN_ON_ONCE(lim->atomic_write_hw_unit_min >
			 lim->atomic_write_hw_unit_max))
		goto unsupported;

	/* NVMe atomic_write_hw_unit_max가 atomic_write_hw_max를 넘으면 모순이므로 거부. */
	if (WARN_ON_ONCE(lim->atomic_write_hw_unit_max >
			 lim->atomic_write_hw_max))
		goto unsupported;

	/* 원자적 쓰기 최대 크기는 chunk_sectors(NVMe NOIOB / ZNS zone 크기 / RAID stripe)를 초과할 수 없음. */
	if (WARN_ON_ONCE(lim->chunk_sectors &&
			atomic_write_hw_max_sectors > lim->chunk_sectors))
		goto unsupported;

	/* NVMe atomic boundary(바이트)를 섹터 단위로 환산해 이후 검사에 사용. */
	boundary_sectors = lim->atomic_write_hw_boundary >> SECTOR_SHIFT;

	/* boundary가 설정된 컨트롤러(0이 아니면)만 아래 추가 정렬 검사를 수행. */
	if (boundary_sectors) {
		/* 원자 경계(NABSPF 유래)는 원자적 쓰기 최대 크기 이상이어야 함. */
		if (WARN_ON_ONCE(lim->atomic_write_hw_max >
				 lim->atomic_write_hw_boundary))
			goto unsupported;

		/* NVMe atomic boundary와 chunk_sectors 정렬 검증. */
		if (WARN_ON_ONCE(!blk_valid_atomic_writes_boundary(
			lim->chunk_sectors, boundary_sectors)))
			goto unsupported;

		/*
		 * The boundary size just needs to be a multiple of unit_max
		 * (and not necessarily a power-of-2), so this following check
		 * could be relaxed in future.
		 * Furthermore, if needed, unit_max could even be reduced so
		 * that it is compliant with a !power-of-2 boundary.
		 */
		/* [한국어] 위 영문 주석이 인정하듯 이것은 원리적 요구가 아니라 현재 구현의 편의다.
		 * 경계 검사(주소 & mask)를 시프트로 처리하려면 2의 거듭제곱이어야 한다.
		 * NVMe 의 NABSPF 는 스펙상 거듭제곱을 강제하지 않으므로, 그렇지 않은
		 * 네임스페이스는 여기서 원자적 쓰기 지원이 통째로 꺼진다. */
		if (!is_power_of_2(boundary_sectors))
			goto unsupported;
	}

	/* [한국어] 검증을 모두 통과했으니 실제 사용할 한도를 확정한다.
	 * 하드웨어 값(atomic_write_hw_*)과 "bio 하나에 담을 수 있는 크기"라는
	 * 소프트웨어 상한 중 작은 쪽이 최종값이 된다. */
	blk_atomic_writes_update_limits(lim);
	/* [한국어] 성공 — 아래 unsupported 라벨을 건너뛴다 */
	return;

unsupported:
	/* [한국어] 지원 불가로 판정된 경우. 네 값을 모두 0 으로 만드는 것이
	 * "이 큐는 원자적 쓰기를 지원하지 않는다"의 표현이며, REQ_ATOMIC 이 붙은
	 * 요청은 상위에서 거절된다. 일부만 0 으로 두면 부분 지원처럼 보여 위험하다. */
	lim->atomic_write_max_sectors = 0;
	lim->atomic_write_boundary_sectors = 0;
	lim->atomic_write_unit_min = 0;
	lim->atomic_write_unit_max = 0;
}

/**
 * blk_validate_limits - NVMe queue_limits 정규화 및 검증
 * @lim: 검증할 queue_limits
 *
 * 목적:
 *   NVMe 컨트롤러가 노출한 max_hw_sectors, max_segments, logical_block_size,
 *   discard, zoned, integrity, atomic_write 등의 하드웨어 한도를 block layer
 *   표준 형식으로 변환하고, 상호 불가능한 조합을 걸러낸다.
 *
 * 주요 호출 경로:
 *   nvme_reset_work -> nvme_pci_configure_admin_queue -> blk_mq_init_queue /
 *   queue_limits_set -> blk_validate_limits
 *   (PCI transport에서 queue_depth, max_hw_sectors 등을 채운 뒤 호출)
 *
 * NVMe 연결 지점:
 *   - max_hw_sectors: NVMe Identify I/O Queue Command Set의 MDTS(Maximum Data
 *     Transfer Size) 또는 드라이버가 계산한 최대 섹터 수.
 *   - max_segments: PRP/SGL scatter-gather list의 최대 항목 수 제한.
 *   - logical_block_size: NVMe LBA Format의 LBA Data Size(보통 512B 또는 4KB).
 *   - features: BLK_FEAT_POLL, BLK_FEAT_NOWAIT, BLK_FEAT_FUA,
 *     BLK_FEAT_ZONED, BLK_FEAT_ATOMIC_WRITES 등 NVMe capability 반영.
 *   - discard_*: NVMe Deallocate/DSM 관련 granularities.
 *
 * struct queue_limits NVMe 관련 주요 필드 설명:
 *   logical_block_size: NVMe LBA Data Size, LBA 단위(512B/4KB 등).
 *   physical_block_size: NVMe 미디어 물리 페이지/블록 정렬.
 *   io_min/io_opt: NVMe 권장 최소/최적 I/O 크기. SQ 엔트리 효율에 영향.
 *   max_hw_sectors: 컨트롤러가 단일 명령으로 수용하는 최대 섹터 수(MDTS 기반).
 *   max_segments: PRP/SGL 리스트 최대 엔트리 수.
 *   max_segment_size: 단일 PRP/SGL 엔트리가 가리킬 수 있는 최대 바이트.
 *   seg_boundary_mask/virt_boundary_mask: 연속 PRP/SGL 엔트리가 넘지 말아야 할
 *     물리/가상 주소 경계.
 *   dma_alignment: DMA 엔진이 요구하는 시작 주소 정렬 마스크. NVMe는 3(4바이트)을
 *     쓴다 — 사양이 데이터 포인터의 하위 2비트를 0으로 요구하기 때문이다
 *     (drivers/nvme/host/core.c: lim->dma_alignment = 3).
 *   features: BLK_FEAT_POLL/NOWAIT/FUA/ZONED/ATOMIC_WRITES 등 NVMe capability.
 *   discard_*: NVMe Deallocate/DSM 명령의 granularities.
 *   atomic_write_*: NVMe 원자적 쓰기 단위. Identify Namespace의 NAWUPF(최대 단위)와
 *     NABSPF(경계)에서 유도된다. FUA(지속성)와는 무관한 별개 개념.
 *   integrity: NVMe DIF/DIX 형식의 end-to-end data protection 설정.
 *
 * [한국어 보강]
 * @return: 성공 시 0. logical/physical block size가 잘못됐거나, max_hw_sectors가
 *     PAGE_SIZE 미만이거나, segment boundary/DMA alignment가 규격을 벗어나거나,
 *     integrity/zoned 하위 검증이 실패하면 -EINVAL.
 *
 * 이 함수는 blk_validate_zoned_limits(), blk_validate_integrity_limits(),
 * blk_validate_atomic_write_limits()를 순서대로 호출하는 최상위 게이트키퍼로,
 * 드라이버가 (일부만) 채운 queue_limits를 block layer 전역에서 안전하게
 * 참조할 수 있는 완전한 값으로 만든다. 각 단계는 이전 단계가 확정한 값(예:
 * logical_block_size)에 의존하므로 순서를 바꿀 수 없다.
 * 실행 컨텍스트: 드라이버 probe(큐 최초 생성) 또는 큐 갱신(reset 후 재검증)
 * 경로의 프로세스 컨텍스트. 큐가 아직 freeze/lock 되지 않았을 수도 있으므로
 * 호출자(queue_limits_commit_update 등)가 동시성을 보장해야 한다.
 * 호출자: blk_set_default_limits(), queue_limits_commit_update().
 * 호출 대상: blk_validate_block_size(), blk_validate_atomic_write_limits(),
 * blk_validate_integrity_limits(), blk_validate_zoned_limits().
 * 에러 경로: 각 단계에서 -EINVAL을 즉시 반환하며, 상위(드라이버 probe 또는
 * queue_limits_commit_update)가 이를 그대로 전파해 큐 생성/갱신을 실패시킨다.
 *
 * 호출 체인:
 *   blk_set_default_limits/queue_limits_commit_update → [blk_validate_limits] → blk_validate_atomic_write_limits → blk_validate_integrity_limits → blk_validate_zoned_limits
 */
int blk_validate_limits(struct queue_limits *lim)
{
	unsigned int max_hw_sectors; /* max_hw_sectors와 max_dev_sectors를 min 병합한 중간값. */
	unsigned int logical_block_sectors; /* logical_block_size를 섹터 단위로 환산한 값. */
	unsigned long seg_size; /* fast-path segment 크기 계산용 임시 변수. */
	int err; /* 하위 검증 함수(integrity)의 반환값을 담는 임시 변수. */

	/*
	 * Unless otherwise specified, default to 512 byte logical blocks and a
	 * physical block size equal to the logical block size.
	 *
	 * NVMe: 드라이버가 Identify Namespace로부터 LBA Data Size를 채우지 않으면
	 * 안전한 512B 기본값을 사용한다.
	 */
	/* NVMe LBA Data Size 미보고 시 512B 기본값. */
	if (!lim->logical_block_size)
		lim->logical_block_size = SECTOR_SIZE;
	/* NVMe LBA Data Size가 명시된 경우 2의 거듭제곱/범위 등 규격을 검사 -
	 * 위반 시 경고 로그를 남기고 큐 생성/갱신을 즉시 실패시킨다. */
	else if (blk_validate_block_size(lim->logical_block_size)) {
		pr_warn("Invalid logical block size (%d)\n", lim->logical_block_size);
		return -EINVAL;
	}
	/* NVMe physical block size는 logical block size 이상의 2의 거듭제곱. */
	if (lim->physical_block_size < lim->logical_block_size) {
		/* physical_block_size가 logical보다 작게 설정됐으면(드라이버 미설정
		 * 등) logical_block_size와 같게 끌어올려 최소 불변식을 만족시킴. */
		lim->physical_block_size = lim->logical_block_size;
	} else if (!is_power_of_2(lim->physical_block_size)) {
		/* logical 이상이지만 2의 거듭제곱이 아니면 규격 위반이므로 거부. */
		pr_warn("Invalid physical block size (%d)\n", lim->physical_block_size);
		return -EINVAL;
	}

	/*
	 * The minimum I/O size defaults to the physical block size unless
	 * explicitly overridden.
	 *
	 * NVMe: io_min은 NVMe NAND 프로그램/페이지 단위와 관련된 물리적 최소 I/O
	 * 크기를 반영한다.
	 */
	/* NVMe io_min은 physical_block_size 미만으로 낮아질 수 없음. */
	if (lim->io_min < lim->physical_block_size)
		lim->io_min = lim->physical_block_size;

	/*
	 * The optimal I/O size may not be aligned to physical block size
	 * (because it may be limited by dma engines which have no clue about
	 * block size of the disks attached to them), so we round it down here.
	 *
	 * NVMe: io_opt는 컨트롤러가 권장하는 최적 I/O 크기로, physical_block_size
	 * 배수로 내림한다.
	 */
	/* NVMe io_opt는 physical_block_size 배수로 내림(정렬 보장). */
	lim->io_opt = round_down(lim->io_opt, lim->physical_block_size);

	/*
	 * max_hw_sectors has a somewhat weird default for historical reason,
	 * but driver really should set their own instead of relying on this
	 * value.
	 *
	 * The block layer relies on the fact that every driver can
	 * handle at lest a page worth of data per I/O, and needs the value
	 * aligned to the logical block size.
	 *
	 * NVMe: NVMe 드라이버가 MDTS를 채우지 않으면 안전값 BLK_SAFE_MAX_SECTORS를
	 * 사용하지만, 실제로는 Identify에서 보고한 MDTS를 기반으로 설정해야 한다.
	 */
	/* NVMe MDTS 미보고 시 안전값 BLK_SAFE_MAX_SECTORS 사용. */
	if (!lim->max_hw_sectors)
		lim->max_hw_sectors = BLK_SAFE_MAX_SECTORS;
	/* NVMe 컨트롤러는 최소 PAGE_SIZE 이상의 I/O를 처리해야 함. */
	if (WARN_ON_ONCE(lim->max_hw_sectors < PAGE_SECTORS))
		return -EINVAL;
	/* NVMe LBA 크기(섹터) 계산. */
	logical_block_sectors = lim->logical_block_size >> SECTOR_SHIFT;
	/* NVMe LBA Data Size가 MDTS보다 클 수 없음. */
	if (WARN_ON_ONCE(logical_block_sectors > lim->max_hw_sectors))
		return -EINVAL;
	/* NVMe max_hw_sectors는 LBA 단위로 정렬. */
	lim->max_hw_sectors = round_down(lim->max_hw_sectors,
			logical_block_sectors);

	/*
	 * The actual max_sectors value is a complex beast and also takes the
	 * max_dev_sectors value (set by SCSI ULPs) and a user configurable
	 * value into account.  The ->max_sectors value is always calculated
	 * from these, so directly setting it won't have any effect.
	 *
	 * NVMe: 상위 bio 경로(submit_bio -> blk_mq_submit_bio)에서 사용할 최종
	 * 단일 I/O 크기 한도. max_hw_sectors(NVMe MDTS), max_dev_sectors,
	 * max_user_sectors 중 최소값이 적용된다.
	 */
	/* NVMe 최종 max_sectors = min(MDTS, max_dev_sectors). */
	max_hw_sectors = min_not_zero(lim->max_hw_sectors,
				lim->max_dev_sectors);
	/* 사용자가 sysfs(max_sectors_kb)로 명시적 한도를 설정했다면 그 값을
	 * 최우선으로 하되, HW 한도(max_hw_sectors)를 넘지 않도록 min 병합. */
	if (lim->max_user_sectors) {
		/* 사용자가 sysfs로 제한한 최소 세그먼트 크기 미만이면 거부. */
		if (lim->max_user_sectors < BLK_MIN_SEGMENT_SIZE / SECTOR_SIZE)
			return -EINVAL;
		/* NVMe: 사용자 한도와 MDTS 중 작은 값이 최종 I/O 크기. */
		lim->max_sectors = min(max_hw_sectors, lim->max_user_sectors);
	} else if (lim->io_opt > (BLK_DEF_MAX_SECTORS_CAP << SECTOR_SHIFT)) {
		/* NVMe io_opt가 기본 캡보다 크면 io_opt 기반으로 확장. */
		lim->max_sectors =
			min(max_hw_sectors, lim->io_opt >> SECTOR_SHIFT);
	} else if (lim->io_min > (BLK_DEF_MAX_SECTORS_CAP << SECTOR_SHIFT)) {
		/* NVMe io_min이 기본 캡보다 크면 io_min 기반으로 확장. */
		lim->max_sectors =
			min(max_hw_sectors, lim->io_min >> SECTOR_SHIFT);
	} else {
		/* NVMe: 기본 BLK_DEF_MAX_SECTORS_CAP 적용. */
		lim->max_sectors = min(max_hw_sectors, BLK_DEF_MAX_SECTORS_CAP);
	}
	/* NVMe 최종 max_sectors도 LBA 단위로 정렬. */
	lim->max_sectors = round_down(lim->max_sectors,
			logical_block_sectors);

	/*
	 * Random default for the maximum number of segments.  Driver should not
	 * rely on this and set their own.
	 *
	 * NVMe: 드라이버가 PRP/SGL 최대 엔트리 수를 채우지 않으면 기본값을 사용.
	 * 실제로는 Identify에서 보고한 Maximum PRP Entry/Number of SGL
	 * Descriptors 등을 반영해야 한다.
	 */
	/* NVMe max_segments 미보고 시 BLK_MAX_SEGMENTS 기본값. */
	if (!lim->max_segments)
		lim->max_segments = BLK_MAX_SEGMENTS;

	/* NVMe Write Zeroes unmap 한도 불일치 시 드라이버 설정 오류. */
	if (lim->max_hw_wzeroes_unmap_sectors &&
	    lim->max_hw_wzeroes_unmap_sectors != lim->max_write_zeroes_sectors)
		return -EINVAL;
	/* NVMe Write Zeroes unmap 최종 한도 = min(HW, 사용자 sysfs 설정). */
	lim->max_wzeroes_unmap_sectors = min(lim->max_hw_wzeroes_unmap_sectors,
			lim->max_user_wzeroes_unmap_sectors);

	/* NVMe Deallocate 최종 한도 = min(HW, 사용자). */
	lim->max_discard_sectors =
		min(lim->max_hw_discard_sectors, lim->max_user_discard_sectors);

	/*
	 * When discard is not supported, discard_granularity should be reported
	 * as 0 to userspace.
	 *
	 * NVMe: Deallocate(Trim/Discard)를 지원하지 않는 namespace에서는
	 * discard_granularity가 0으로 보고된다.
	 */
	/* [한국어] discard 를 지원한다면 그 단위는 최소한 물리 블록 크기여야 한다.
	 * 그보다 잘게 요청해 봐야 장치가 실제로 해제할 수 있는 단위가 아니기 때문이다.
	 * NVMe 는 이 값을 NPDG/NPDA 에서 유도해 넘긴다(core.c:2532 부근). */
	if (lim->max_discard_sectors)
		lim->discard_granularity =
			max(lim->discard_granularity, lim->physical_block_size);
	else
		lim->discard_granularity = 0;

	/* [한국어] discard 요청도 최소 한 구간은 담아야 하므로 0 은 있을 수 없다.
	 * NVMe 의 Dataset Management 는 한 명령에 여러 구간(range)을 실을 수 있어
	 * 드라이버가 이 값을 1보다 크게 잡는다. */
	if (!lim->max_discard_segments)
		lim->max_discard_segments = 1;

	/*
	 * By default there is no limit on the segment boundary alignment,
	 * but if there is one it can't be smaller than the page size as
	 * that would break all the normal I/O patterns.
	 */
	/* NVMe PRP/SGL seg_boundary_mask 미보고 시 기본값. */
	if (!lim->seg_boundary_mask)
		lim->seg_boundary_mask = BLK_SEG_BOUNDARY_MASK;
	/* NVMe segment boundary는 최소 PAGE_SIZE-1 이상이어야 함. */
	if (WARN_ON_ONCE(lim->seg_boundary_mask < BLK_MIN_SEGMENT_SIZE - 1))
		return -EINVAL;

	/*
	 * Stacking device may have both virtual boundary and max segment
	 * size limit, so allow this setting now, and long-term the two
	 * might need to move out of stacking limits since we have immutable
	 * bvec and lower layer bio splitting is supposed to handle the two
	 * correctly.
	 *
	 * NVMe: max_segment_size는 단일 PRP/SGL 엔트리가 가리킬 수 있는 최대
	 * 바이트. 4KB PRP entry 기준으로는 보통 4KB 또는 64KB 제한이 적용된다.
	 */
	if (lim->virt_boundary_mask) {
		/* virtual boundary가 있으면 max_segment_size는 UINT_MAX로 둔다. */
		if (!lim->max_segment_size)
			lim->max_segment_size = UINT_MAX;
	} else {
		/*
		 * The maximum segment size has an odd historic 64k default that
		 * drivers probably should override.  Just like the I/O size we
		 * require drivers to at least handle a full page per segment.
		 */
		/* NVMe max_segment_size 미보고 시 64KB 기본값. */
		if (!lim->max_segment_size)
			lim->max_segment_size = BLK_MAX_SEGMENT_SIZE;
		/* NVMe 단일 PRP/SGL 엔트리는 최소 PAGE_SIZE 이상 처리해야 함. */
		if (WARN_ON_ONCE(lim->max_segment_size < BLK_MIN_SEGMENT_SIZE))
			return -EINVAL;
	}

	/* setup max segment size for building new segment in fast path */
	/* [한국어] 핫패스에서 "이 세그먼트에 더 붙여도 되는가"를 매번 두 조건(크기 상한과
	 * 경계 마스크)으로 따지지 않도록, 둘 중 더 빡빡한 쪽을 미리 하나로 합쳐 둔다.
	 * PAGE_SIZE 로 한 번 더 자르는 이유는 어차피 페이지 단위로 붙여 나가기 때문이다. */
	if (lim->seg_boundary_mask > lim->max_segment_size - 1)
		seg_size = lim->max_segment_size;
	else
		seg_size = lim->seg_boundary_mask + 1;
	lim->max_fast_segment_size = min_t(unsigned int, seg_size, PAGE_SIZE);

	/*
	 * We require drivers to at least do logical block aligned I/O, but
	 * historically could not check for that due to the separate calls
	 * to set the limits.  Once the transition is finished the check
	 * below should be narrowed down to check the logical block size.
	 *
	 * NVMe: DMA 엔진은 LBA 경계에 맞춘 시작 주소를 요구하므로, 최소 512B
	 * 정렬을 보장한다.
	 */
	/* NVMe dma_alignment 미보고 시 512B-1 정렬. */
	if (!lim->dma_alignment)
		lim->dma_alignment = SECTOR_SIZE - 1;
	/* NVMe DMA alignment는 PAGE_SIZE를 초과할 수 없음. */
	if (WARN_ON_ONCE(lim->dma_alignment > PAGE_SIZE))
		return -EINVAL;

	/* NVMe 파티션/스택 alignment_offset 마스크 처리. */
	if (lim->alignment_offset) {
		/* alignment_offset을 physical_block_size 이내로 마스킹 - 그보다
		 * 큰 절대 오프셋은 의미가 없고 배수 부분은 이미 정렬된 것과 같음. */
		lim->alignment_offset &= (lim->physical_block_size - 1);
		/* 이 시점의 alignment_offset은 새로 계산된 유효한 값이므로,
		 * 이전에 남아있을 수 있는 misaligned 플래그를 새로 지운다. */
		lim->flags &= ~BLK_FLAG_MISALIGNED;
	}

	/*
	 * NVMe: Volatile Write Cache(VWC)를 지원하지 않는 컨트롤러에서는 FUA bit를
	 * 강제로 제거하여 상위 계층이 불필요한 FUA 명령을 복잡하게 제출하지
	 * 않도록 한다.
	 */
	/* NVMe VWC 미지원 시 BLK_FEAT_FUA 클리어. */
	if (!(lim->features & BLK_FEAT_WRITE_CACHE))
		lim->features &= ~BLK_FEAT_FUA;

	/* NVMe 원자적 쓰기(NAWUPF/NABSPF) 한도를 검증/정규화 - 실패해도 atomic_write_*가
	 * 0으로 클리어될 뿐 이 함수 자체를 실패시키지 않는다(void 반환). */
	blk_validate_atomic_write_limits(lim);

	/* NVMe DIF/DIX PI 설정 검증 - 실패 시 err에 -EINVAL이 담긴다. */
	err = blk_validate_integrity_limits(lim);
	/* PI 검증 실패는 전체 blk_validate_limits()의 실패로 즉시 전파. */
	if (err)
		return err;
	/* 마지막 단계로 NVMe ZNS 관련 한도를 검증하고 그 결과를 그대로 반환. */
	return blk_validate_zoned_limits(lim);
}
EXPORT_SYMBOL_GPL(blk_validate_limits);

/**
 * blk_set_default_limits - 새로 할당된 request_queue의 기본 한도 설정
 * @lim: 초기화할 queue_limits
 *
 * NVMe 연결 지점:
 *   NVMe 드라이버가 blk_mq_init_queue() 등으로 큐를 생성할 때 호출되며,
 *   사용자가 덮어쓸 수 있는 discard/write-zeroes 한도를 UINT_MAX로 초기화한
 *   뒤 blk_validate_limits()를 통해 NVMe 하드웨어 제약을 정규화한다.
 *
 * [한국어 보강]
 * @return: blk_validate_limits(lim)의 반환값을 그대로 전달 - 성공 시 0,
 *     검증 실패 시 -EINVAL.
 *
 * 사용자 sysfs가 낮출 수 있는 max_user_discard_sectors/
 * max_user_wzeroes_unmap_sectors는 blk_validate_limits() 내부의 일반적인
 * "0이면 기본값" 패턴으로는 초기화할 수 없다(0 자체가 "제한 없음"이 아니라
 * "완전 금지"를 의미하는 필드이기 때문). 따라서 이 두 필드만 별도로
 * UINT_MAX로 세팅한 뒤 나머지 정규화는 blk_validate_limits()에 위임한다.
 * 실행 컨텍스트: 큐 최초 생성 경로의 프로세스 컨텍스트.
 * 호출자: blk_mq_init_queue() 등 큐 최초 생성 경로. 호출 대상:
 * blk_validate_limits().
 * 에러 경로: blk_validate_limits()가 -EINVAL을 반환하면 그대로 전파.
 *
 * 호출 체인:
 *   blk_mq_init_queue → [blk_set_default_limits] → blk_validate_limits
 */
int blk_set_default_limits(struct queue_limits *lim)
{
	/*
	 * Most defaults are set by capping the bounds in blk_validate_limits,
	 * but these limits are special and need an explicit initialization to
	 * the max value here.
	 */
	/* NVMe Deallocate 사용자 한도를 최대로 두고 HW 값과 min 병합 준비. */
	lim->max_user_discard_sectors = UINT_MAX;
	/* NVMe Write Zeroes unmap 사용자 한도를 최대로 두고 HW 값과 min 준비. */
	lim->max_user_wzeroes_unmap_sectors = UINT_MAX;
	/* 나머지 필드 정규화/검증은 공통 경로인 blk_validate_limits()에 위임. */
	return blk_validate_limits(lim);
}

/**
 * queue_limits_commit_update - queue_limits 원자적 갱신 커밋
 * @q: 갱신할 request_queue
 * @lim: 적용할 queue_limits
 *
 * NVMe 연결 지점:
 *   NVMe 드라이버가 reset 후 Identify/Controller 정보를 다시 읽어 q->limits를
 *   갱신할 때 사용된다. limits_lock 아래에서 blk_validate_limits()로 검증한
 *   뒤 q->limits에 복사하고, disk->bdi에도 optimal I/O 크기를 전파한다.
 *   (주의) 호출자는 큐를 freeze하거나 outstanding I/O가 없음을 보장해야 한다.
 *
 * [한국어 보강]
 * @return: 성공 시 0. blk_validate_limits() 실패 시 -EINVAL, inline
 *     encryption과 DIF/DIX가 동시에 요구되면 -EINVAL. 두 경우 모두
 *     q->limits는 갱신되지 않은 채로 남는다(원자성 보장).
 *
 * 이 함수는 반드시 q->limits_lock을 쥔 상태(mutex_lock 또는 mutex_trylock
 * 성공)에서 호출되어야 하며, 성공/실패 어느 경로든 함수 끝에서 반드시
 * mutex_unlock으로 락을 해제한다(lockdep_assert_held로 사전 조건을 강제).
 * 검증에 실패하면 q->limits를 건드리지 않고 즉시 unlock 후 반환해 "전부
 * 반영 또는 전부 무시"의 원자적 갱신을 보장한다. 성공하면 q->limits를
 * 통째로 교체하고, gendisk가 있다면 blk_apply_bdi_limits()로 read-ahead
 * 힌트도 함께 갱신한다.
 * 실행 컨텍스트: 드라이버 reset/재구성 경로의 프로세스 컨텍스트. 호출자가
 * 큐를 freeze했거나 outstanding I/O가 없음을 보장해야 race 없이 안전하다.
 * 호출자: queue_limits_set(), queue_limits_commit_update_frozen().
 * 호출 대상: blk_validate_limits(), blk_apply_bdi_limits().
 * 에러 경로: out_unlock 레이블에서 항상 mutex_unlock 후 error 값을 반환.
 *
 * 호출 체인:
 *   queue_limits_set/queue_limits_commit_update_frozen → [queue_limits_commit_update] → blk_validate_limits → blk_apply_bdi_limits
 */
int queue_limits_commit_update(struct request_queue *q,
		struct queue_limits *lim)
{
	int error; /* blk_validate_limits() 또는 encryption 호환성 검사의 결과 코드. */

	/* NVMe limits 갱신은 limits_lock 보호 아래 수행되어야 함. */
	lockdep_assert_held(&q->limits_lock);

	/* NVMe Identify/Controller 값을 정규화; 실패 시 unlock 후 반환. */
	error = blk_validate_limits(lim);
	if (error)
		goto out_unlock;

#ifdef CONFIG_BLK_INLINE_ENCRYPTION
	/* NVMe inline encryption과 DIF/DIX는 동시에 활성화할 수 없음(호환성). */
	if (q->crypto_profile && lim->integrity.tag_size) {
		pr_warn("blk-integrity: Integrity and hardware inline encryption are not supported together.\n");
		error = -EINVAL;
		goto out_unlock;
	}
#endif

	/* q->limits에 NVMe 컨트롤러 capability를 원자적으로 커밋. */
	q->limits = *lim;
	/* disk->bdi에 NVMe optimal I/O 크기를 전파하여 read-ahead에 반영. */
	if (q->disk)
		blk_apply_bdi_limits(q->disk->bdi, lim);
out_unlock:
	mutex_unlock(&q->limits_lock);
	return error;
}
EXPORT_SYMBOL_GPL(queue_limits_commit_update);

/**
 * queue_limits_commit_update_frozen - 큐를 freeze한 상태에서 limits 갱신
 * @q: 갱신할 request_queue
 * @lim: 적용할 queue_limits
 *
 * NVMe 연결 지점:
 *   NVMe reset/work에서 queue_limits_start_update()로 복사본을 얻은 뒤
 *   Identify 값으로 채우고, 본 함수를 통해 I/O를 멈춘 상태에서 안전하게
 *   커밋한다. blk_mq_freeze_queue/unfreeze_queue로 SQ/CQ에 새 request가
 *   들어가지 않음을 보장한다.
 *
 * [한국어 보강]
 * @return: queue_limits_commit_update()의 반환값을 그대로 전달 - 성공 0,
 *     실패 시 -EINVAL. freeze는 성공/실패와 무관하게 항상 unfreeze된다.
 *
 * queue_limits_commit_update()는 q->limits를 통째로 교체하므로, outstanding
 * request가 새 limits와 옛 limits가 뒤섞인 상태로 처리될 위험이 있다.
 * 이를 막기 위해 blk_mq_freeze_queue()로 새 request 진입을 막고 이미
 * 처리 중인 request가 모두 drain될 때까지 기다린 뒤 limits를 교체하고,
 * blk_mq_unfreeze_queue()로 I/O를 재개한다. memflags는 freeze 중
 * 메모리 회수(reclaim) 재진입을 막기 위한 GFP 플래그 스냅샷으로,
 * unfreeze 시 그대로 복원해야 한다.
 * 실행 컨텍스트: 드라이버 reset 경로의 프로세스 컨텍스트. freeze/unfreeze
 * 쌍은 반드시 짝을 맞춰야 하며 중첩 호출도 허용된다(refcount 기반).
 * 호출자: NVMe 등 드라이버의 reset/재구성 경로. 호출 대상:
 * blk_mq_freeze_queue(), queue_limits_commit_update(), blk_mq_unfreeze_queue().
 * 에러 경로: queue_limits_commit_update() 실패도 unfreeze는 반드시 수행.
 *
 * 호출 체인:
 *   (NVMe reset 등 드라이버 경로) → [queue_limits_commit_update_frozen] → blk_mq_freeze_queue → queue_limits_commit_update → blk_mq_unfreeze_queue
 */
int queue_limits_commit_update_frozen(struct request_queue *q,
		struct queue_limits *lim)
{
	unsigned int memflags; /* freeze 시점의 GFP 플래그 스냅샷 - unfreeze에 그대로 복원. */
	int ret; /* queue_limits_commit_update()의 결과를 담아 그대로 반환. */

	/* NVMe reset 중 SQ/CQ에 새 request 진입 차단(freeze). */
	memflags = blk_mq_freeze_queue(q);
	/* freeze 상태에서 q->limits 커밋. */
	ret = queue_limits_commit_update(q, lim);
	/* NVMe I/O 재개: 새 limits 하에서 SQ/CQ에 request 투입 가능. */
	blk_mq_unfreeze_queue(q, memflags);

	/* 커밋 성공/실패 여부를 호출자에게 그대로 전달. */
	return ret;
}
EXPORT_SYMBOL_GPL(queue_limits_commit_update_frozen);

/**
 * queue_limits_set - 새로 초기화된 queue_limits를 큐에 적용
 * @q: 갱신할 request_queue
 * @lim: 적용할 queue_limits
 *
 * NVMe 연결 지점:
 *   NVMe PCI/FC/TCP transport에서 request_queue를 생성할 때 처음으로 호출.
 *   blk_mq_init_queue -> queue_limits_set -> blk_validate_limits 경로를 통해
 *   NVMe 컨트롤러 capability를 block layer에 등록한다.
 *
 * [한국어 보강]
 * @return: queue_limits_commit_update()의 반환값을 그대로 전달 - 성공 0,
 *     실패 시 -EINVAL.
 *
 * 큐가 아직 사용자에게 노출되지 않은 최초 생성 시점에는 별도의 freeze가
 * 필요 없으므로(어차피 outstanding I/O가 없음), mutex_lock으로 limits_lock만
 * 잡고 바로 queue_limits_commit_update()에 위임한다. lock/unlock을 직접
 * 감싸는 이유는 queue_limits_commit_update()가 "락을 이미 쥔 상태"를
 * 전제로 만들어져 있어(lockdep_assert_held) 최초 호출부와 갱신 호출부가
 * 락 획득 방식만 다르게 재사용할 수 있게 하기 위함이다.
 * 실행 컨텍스트: 드라이버 큐 최초 생성 경로의 프로세스 컨텍스트.
 * 호출자: nvme_alloc_admin_tag_set() 이후 큐 초기화 경로 등. 호출 대상:
 * queue_limits_commit_update() (내부에서 blk_validate_limits() 등 호출).
 * 에러 경로: queue_limits_commit_update()가 실패해도 해당 함수 내부에서
 * mutex_unlock을 수행하므로 이 함수에서 별도 unlock은 필요 없다.
 *
 * 호출 체인:
 *   blk_mq_init_queue → [queue_limits_set] → queue_limits_commit_update → blk_validate_limits
 */
int queue_limits_set(struct request_queue *q, struct queue_limits *lim)
{
	/* NVMe 큐 최초 생성 시 limits_lock을 잡아 commit_update의 lockdep 전제
	 * 조건(lockdep_assert_held)을 충족시킨다. */
	mutex_lock(&q->limits_lock);
	/* 실제 검증/커밋/unlock은 공통 경로인 queue_limits_commit_update에 위임. */
	return queue_limits_commit_update(q, lim);
}
EXPORT_SYMBOL_GPL(queue_limits_set);

/**
 * queue_limit_alignment_offset - 파티션/스택 시작 섹터의 정렬 오프셋 계산
 * @lim: queue_limits
 * @sector: 대상 섹터
 *
 * NVMe 연결 지점:
 *   NVMe namespace 위의 파티션이나 MD/DM 스택 장치에서 물리 블록 경계와
 *   데이터 시작 위치가 맞지 않을 때 alignment_offset을 계산한다. 이 값은
 *   상위 bio의 LBA 정렬 검사에 사용된다.
 *
 * [한국어 보강]
 * @return: sector 위치에서 granularity(= max(physical_block_size, io_min))
 *     경계까지 남은 바이트 오프셋(0이면 완전 정렬).
 *
 * 파티션이나 MD/DM 스택 장치는 하위 NVMe namespace의 물리 블록 경계와
 * 다른 위치에서 시작할 수 있다. 이 함수는 lim->alignment_offset(namespace
 * 전체의 기준 오프셋)과 sector가 granularity 내에서 차지하는 위치를
 * 비교해, "이 섹터부터 얼마나 더 가야 다음 정렬 경계가 나오는지"를 바이트
 * 단위로 계산한다. sector_div()는 64비트 sector_t를 32비트 나눗셈으로
 * 처리하기 위한 커널 헬퍼(나머지를 반환하고 sector 자체는 몫으로 갱신)이다.
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs 읽기, blk_stack_limits() 등).
 * 부수효과 없는 순수 계산 함수.
 * 호출자: blk_stack_limits(), bdev_alignment_offset(). 호출 대상:
 * sector_div() (asm-generic/div64.h 매크로).
 * 에러 경로: 없음 (항상 유효한 오프셋 값을 반환).
 *
 * 호출 체인:
 *   blk_stack_limits/bdev_alignment_offset → [queue_limit_alignment_offset] → sector_div
 */
static int queue_limit_alignment_offset(const struct queue_limits *lim,
		sector_t sector)
{
	/* NVMe 정렬 검사 기준 = max(physical_block_size, io_min). */
	unsigned int granularity = max(lim->physical_block_size, lim->io_min);
	/* NVMe sector에서 granularity 단위 remainder를 바이트로 변환. */
	unsigned int alignment = sector_div(sector, granularity >> SECTOR_SHIFT)
		<< SECTOR_SHIFT;

	/* namespace 전체 기준 alignment_offset에서 이 섹터의 국소 위치를 빼고
	 * granularity로 나머지 연산해, 다음 정렬 경계까지 남은 바이트를 구함. */
	return (granularity + lim->alignment_offset - alignment) % granularity;
}

/**
 * queue_limit_discard_alignment - discard 정렬 오프셋 계산
 * @lim: queue_limits
 * @sector: 대상 섹터
 *
 * NVMe 연결 지점:
 *   NVMe Deallocate(Trim) 명령은 discard_granularity 단위로 정렬되어야
 *   효율적으로 동작한다. 파티션 시작 섹터를 고려하여 discard alignment를
 *   보정한다.
 *
 * [한국어 보강]
 * @return: sector 위치에서 discard_granularity 경계까지 남은 바이트
 *     오프셋. Deallocate 미지원(max_discard_sectors==0)이면 0.
 *
 * queue_limit_alignment_offset()과 동일한 아이디어를 discard_granularity/
 * discard_alignment에 적용한다. NVMe Deallocate는 특정 granularity
 * 단위로 정렬된 요청이라야 효율적으로 처리되므로(정렬이 안 맞으면
 * 컨트롤러가 부분 페이지를 read-modify-write 해야 할 수 있음), 파티션
 * 시작 섹터를 반영한 정확한 정렬 오프셋을 계산해 상위(blkdev_issue_discard
 * 등)에 알려준다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 계산 함수(부수효과 없음).
 * 호출자: blk_stack_limits(), bdev_discard_alignment(). 호출 대상:
 * sector_div().
 * 에러 경로: 없음. Deallocate 미지원 시 0을 반환해 "정렬 걱정 없음"을 표시.
 *
 * 호출 체인:
 *   blk_stack_limits/bdev_discard_alignment → [queue_limit_discard_alignment] → sector_div
 */
static unsigned int queue_limit_discard_alignment(
		const struct queue_limits *lim, sector_t sector)
{
	unsigned int alignment, granularity, offset; /* 섹터/바이트 변환 중간값들. */

	/* NVMe Deallocate 미지원 시 alignment 0 반환. */
	if (!lim->max_discard_sectors)
		return 0;

	/* Why are these in bytes, not sectors? */
	/* NVMe discard_alignment를 섹터 단위로 변환. */
	alignment = lim->discard_alignment >> SECTOR_SHIFT;
	/* NVMe discard_granularity를 섹터 단위로 변환. */
	granularity = lim->discard_granularity >> SECTOR_SHIFT;

	/* Offset of the partition start in 'granularity' sectors */
	/* NVMe 파티션 시작 섹터의 granularity 내 offset. */
	offset = sector_div(sector, granularity);

	/* And why do we do this modulus *again* in blkdev_issue_discard()? */
	/* NVMe Deallocate 시작 위치를 granularity 경계로 보정. */
	offset = (granularity + alignment - offset) % granularity;

	/* Turn it back into bytes, gaah */
	/* 섹터 단위 offset을 다시 바이트 단위로 환산해 반환. */
	return offset << SECTOR_SHIFT;
}

/*
 * [한국어]
 * blk_round_down_sectors - 섹터 수를 LBA 단위로 내림하되 최소 PAGE_SIZE는 보장
 *
 * @sectors: 내림할 섹터 수 (예: 스택 병합 중간값인 max_sectors 등).
 * @lbs: 기준이 되는 logical_block_size(바이트). NVMe LBA Data Size.
 * @return: lbs 단위로 내림된 섹터 수. 단, 그 결과가 PAGE_SIZE 상당 섹터
 *     수보다 작으면 PAGE_SIZE 상당 섹터 수로 올려 반환한다.
 *
 * blk_stack_limits()가 여러 하위 NVMe 장치의 max_sectors/max_hw_sectors/
 * max_dev_sectors를 병합한 뒤, 그 결과가 LBA 경계에 맞도록 정렬해야
 * 한다(round_down). 그런데 병합 과정에서 값이 지나치게 작아지면 블록
 * 레이어가 요구하는 "최소 한 페이지는 처리 가능해야 한다"는 불변식이
 * 깨질 수 있으므로, 내림 후에도 PAGE_SIZE 상당 섹터 수 미만이면 강제로
 * 끌어올린다.
 * 실행 컨텍스트: blk_stack_limits() 호출 경로의 프로세스 컨텍스트. 순수
 * 계산 함수(부수효과 없음).
 * 호출자: blk_stack_limits(). 호출 대상: round_down() (산술 매크로).
 * 에러 경로: 없음 (항상 유효한 섹터 수를 반환).
 *
 * 호출 체인:
 *   blk_stack_limits → [blk_round_down_sectors] → (산술 계산으로 종료)
 */
static unsigned int blk_round_down_sectors(unsigned int sectors, unsigned int lbs)
{
	/* NVMe I/O 크기를 LBA 단위로 내림. */
	sectors = round_down(sectors, lbs >> SECTOR_SHIFT);
	/* NVMe: 최소 PAGE_SIZE 이상 보장. */
	if (sectors < PAGE_SIZE >> SECTOR_SHIFT)
		sectors = PAGE_SIZE >> SECTOR_SHIFT;
	/* 내림 + 최소값 보정이 끝난 최종 섹터 수를 호출자에게 반환. */
	return sectors;
}

/* Check if second and later bottom devices are compliant */
/*
 * [한국어]
 * blk_stack_atomic_writes_tail - 두 번째 이후 하위 장치의 atomic write 호환성 검사/병합
 *
 * @t: 이미 최소 하나의 하위 장치가 병합된 상위(target) queue_limits.
 * @b: 새로 병합할 하위(bottom) 장치의 queue_limits.
 * @return: 호환되어 병합에 성공하면 true, boundary가 다르거나 unit 범위가
 *     겹치지 않으면 false(이 경우 상위 호출자가 atomic write 전체를
 *     미지원으로 처리).
 *
 * MD/DM 등으로 여러 NVMe 장치를 스트라이핑/미러링할 때, 두 번째 이후
 * 추가되는 하위 장치는 이미 확정된 t->atomic_write_hw_boundary와 정확히
 * 일치해야 한다(서로 다른 boundary를 가진 장치를 섞는 조합은 아직
 * 지원하지 않음). 또한 t의 최소/최대 unit 범위가 b의 최대/최소 unit
 * 범위와 겹치는 구간이 있어야 하며, 겹치면 t의 범위를 b와 교집합으로
 * 좁히고(min/max 재계산) hw_max도 더 작은 쪽으로 줄인다.
 * 실행 컨텍스트: blk_stack_atomic_writes_limits() 호출 경로의 프로세스
 * 컨텍스트.
 * 호출자: blk_stack_atomic_writes_limits() (두 번째 이후 하위 장치 병합 시).
 * 호출 대상: 없음 (min/max 산술 연산만 사용).
 * 에러 경로: 비호환 시 false 반환, 호출자가 unsupported 레이블로 이동.
 *
 * 호출 체인:
 *   blk_stack_atomic_writes_limits → [blk_stack_atomic_writes_tail] → (산술 병합으로 종료)
 */
static bool blk_stack_atomic_writes_tail(struct queue_limits *t,
				struct queue_limits *b)
{
	/* We're not going to support different boundary sizes.. yet */
	/* NVMe atomic boundary가 다른 멀티 장치 스택은 아직 미지원. */
	if (t->atomic_write_hw_boundary != b->atomic_write_hw_boundary)
		return false;

	/* Can't support this */
	/* NVMe t의 최소 단위가 b의 최대 단위보다 크면 호환 불가. */
	if (t->atomic_write_hw_unit_min > b->atomic_write_hw_unit_max)
		return false;

	/* Or this */
	/* NVMe t의 최대 단위가 b의 최소 단위보다 작으면 호환 불가. */
	if (t->atomic_write_hw_unit_max < b->atomic_write_hw_unit_min)
		return false;

	/* 멀티 장치 스택 시 원자적 쓰기 최대치/단위는 교차 병합. */
	t->atomic_write_hw_max = min(t->atomic_write_hw_max,
				b->atomic_write_hw_max);
	t->atomic_write_hw_unit_min = max(t->atomic_write_hw_unit_min,
				b->atomic_write_hw_unit_min);
	t->atomic_write_hw_unit_max = min(t->atomic_write_hw_unit_max,
				b->atomic_write_hw_unit_max);
	/* boundary 일치 + unit 범위 교집합 존재 → 병합 성공. */
	return true;
}

/*
 * [한국어]
 * blk_stack_atomic_writes_chunk_sectors - chunk_sectors(RAID stripe)와 atomic write 단위 재정렬
 *
 * @t: 이미 atomic_write_hw_* 필드가 병합된 상위 queue_limits. chunk_sectors가
 *     0이 아니면(스트라이핑 등으로 stripe 경계가 존재) 이 값을 기준으로
 *     atomic_write_hw_unit_max/unit_min/hw_max를 추가로 좁힌다.
 * @return: 없음 (void). t의 필드를 in-place로 보정한다.
 *
 * chunk_sectors(RAID stripe 크기 등)는 2의 거듭제곱일 필요가 없지만
 * atomic write 단위는 2의 거듭제곱이어야 하므로, chunk 크기를 "넘지
 * 않는" 가장 큰 2의 거듭제곱 인자를 max_pow_of_two_factor()로 구해
 * unit_max의 상한으로 삼는다(예: chunk=24KB이면 8KB). chunk_sectors를
 * 바이트로 환산할 때 오버플로가 발생하면(매우 큰 stripe) 어차피 그런
 * 크기의 atomic write는 지원하지 않으므로 섹터 값을 그대로 바이트처럼
 * 취급해 안전한 쪽으로 보정한다.
 * 실행 컨텍스트: blk_stack_atomic_writes_limits() 호출 경로의 프로세스
 * 컨텍스트.
 * 호출자: blk_stack_atomic_writes_limits() (head/tail 병합 후 마지막 단계).
 * 호출 대상: check_shl_overflow(), max_pow_of_two_factor().
 * 에러 경로: 없음 (오버플로 시에도 안전한 값으로 대체할 뿐 실패하지 않음).
 *
 * 호출 체인:
 *   blk_stack_atomic_writes_limits → [blk_stack_atomic_writes_chunk_sectors] → max_pow_of_two_factor
 */
static void blk_stack_atomic_writes_chunk_sectors(struct queue_limits *t)
{
	unsigned int chunk_bytes; /* chunk_sectors를 바이트로 환산한 값(오버플로 가능). */

	/* chunk_sectors가 0이면 atomic write와의 정렬 검사 불필요. */
	if (!t->chunk_sectors)
		return;

	/*
	 * If chunk sectors is so large that its value in bytes overflows
	 * UINT_MAX, then just shift it down so it definitely will fit.
	 * We don't support atomic writes of such a large size anyway.
	 */
	/* NVMe: chunk_sectors*SECTOR_SHIFT overflow 방지. */
	if (check_shl_overflow(t->chunk_sectors, SECTOR_SHIFT, &chunk_bytes))
		chunk_bytes = t->chunk_sectors;

	/*
	 * Find values for limits which work for chunk size.
	 * b->atomic_write_hw_unit_{min, max} may not be aligned with chunk
	 * size, as the chunk size is not restricted to a power-of-2.
	 * So we need to find highest power-of-2 which works for the chunk
	 * size.
	 * As an example scenario, we could have t->unit_max = 16K and
	 * t->chunk_sectors = 24KB. For this case, reduce t->unit_max to a
	 * value aligned with both limits, i.e. 8K in this example.
	 */
	/* NVMe atomic write unit_max를 chunk_bytes의 최대 2의 거듭제곱 인자로 제한. */
	t->atomic_write_hw_unit_max = min(t->atomic_write_hw_unit_max,
					max_pow_of_two_factor(chunk_bytes));

	/* NVMe unit_min은 unit_max를 초과할 수 없도록 보정. */
	t->atomic_write_hw_unit_min = min(t->atomic_write_hw_unit_min,
					  t->atomic_write_hw_unit_max);
	/* 원자적 쓰기 최대치는 chunk_bytes 이하로 제한. */
	t->atomic_write_hw_max = min(t->atomic_write_hw_max, chunk_bytes);
}

/* Check stacking of first bottom device */
/*
 * [한국어]
 * blk_stack_atomic_writes_head - 첫 번째 하위 장치의 atomic write 한도를 상위로 상속
 *
 * @t: 아직 atomic write 값이 설정되지 않은 상위(target) queue_limits
 *     (atomic_write_hw_max == UINT_MAX인 초기 상태).
 * @b: 상속할 첫 번째 하위(bottom) 장치의 queue_limits.
 * @return: chunk_sectors와 boundary가 정렬 가능하면 true(상속 수행),
 *     정렬 불가능하면 false(호출자가 atomic write 전체를 미지원 처리).
 *
 * 두 번째 이후 장치는 blk_stack_atomic_writes_tail()로 "호환성 검사 후
 * 교집합"을 구하지만, 첫 번째 하위 장치는 비교 대상이 없으므로 t의
 * chunk_sectors(RAID stripe, 스택 드라이버가 이미 설정해뒀을 수 있음)와
 * b의 atomic boundary가 서로 배수 관계인지만 확인한 뒤 b의 값을 그대로
 * 복사해 상위 t의 초기 atomic write 프로필로 삼는다.
 * 실행 컨텍스트: blk_stack_atomic_writes_limits() 호출 경로의 프로세스
 * 컨텍스트.
 * 호출자: blk_stack_atomic_writes_limits() (t->atomic_write_hw_max ==
 * UINT_MAX일 때, 즉 아직 하위 장치가 하나도 병합되지 않은 상태).
 * 호출 대상: blk_valid_atomic_writes_boundary().
 * 에러 경로: 정렬 실패 시 false, 호출자가 unsupported로 처리.
 *
 * 호출 체인:
 *   blk_stack_atomic_writes_limits → [blk_stack_atomic_writes_head] → blk_valid_atomic_writes_boundary
 */
static bool blk_stack_atomic_writes_head(struct queue_limits *t,
				struct queue_limits *b)
{
	/* NVMe atomic boundary가 chunk_sectors와 정렬되어야 첫 장치 채택 가능. */
	if (!blk_valid_atomic_writes_boundary(t->chunk_sectors,
			b->atomic_write_hw_boundary >> SECTOR_SHIFT))
		return false;

	/* NVMe 첫 하위 장치의 atomic write 단위를 상위로 상속. */
	t->atomic_write_hw_unit_max = b->atomic_write_hw_unit_max;
	t->atomic_write_hw_unit_min = b->atomic_write_hw_unit_min;
	t->atomic_write_hw_max = b->atomic_write_hw_max;
	t->atomic_write_hw_boundary = b->atomic_write_hw_boundary;
	/* 정렬 검사 통과 + 값 상속 완료 → 첫 장치 채택 성공. */
	return true;
}

/**
 * blk_stack_atomic_writes_limits - 스택형 장치의 NVMe 원자적 쓰기 한도 병합
 * @t: 상위 queue_limits
 * @b: 하위 queue_limits
 * @start: 하위 장치 내 시작 섹터
 *
 * NVMe 연결 지점:
 *   MD/DM 등이 여러 NVMe 장치를 묶을 때 각 장치의 atomic write 단위가
 *   호환되는지 검사하고, 호환되면 상위 장치로 병합한다. 호환되지 않으면
 *   상위 장치는 atomic write를 노출하지 않는다.
 *
 * [한국어 보강]
 * @return: 없음 (void). 성공/실패는 t->atomic_write_hw_max가 0인지 여부로
 *     상위(blk_stack_limits)가 판단한다.
 *
 * blk_stack_limits()가 물리/논리 블록 크기 등 기본 필드를 병합한 뒤 마지막
 * 단계로 호출하는 함수다. 하위 장치가 atomic write를 아예 지원하지
 * 않거나(BLK_FEAT_ATOMIC_WRITES 꺼짐), 단위가 0이거나, start 섹터가
 * 정렬되지 않았다면 곧바로 unsupported로 점프한다. 그렇지 않으면 t가
 * 아직 아무 하위 장치도 병합하지 않은 초기 상태(UINT_MAX)인지에 따라
 * head/tail 병합 함수를 선택 호출하고, 마지막으로 chunk_sectors(RAID
 * stripe)와의 정렬을 blk_stack_atomic_writes_chunk_sectors()로 재조정한다.
 * 실행 컨텍스트: blk_stack_limits() 호출 경로의 프로세스 컨텍스트.
 * 호출자: blk_stack_limits(). 호출 대상: blk_atomic_write_start_sect_aligned(),
 * blk_stack_atomic_writes_head(), blk_stack_atomic_writes_tail(),
 * blk_stack_atomic_writes_chunk_sectors().
 * 에러 경로: 어떤 검사든 실패하면 unsupported 레이블에서 관련 필드를 모두
 * 0으로 만들어 상위 장치가 atomic write를 노출하지 않게 한다.
 *
 * 호출 체인:
 *   blk_stack_limits → [blk_stack_atomic_writes_limits] → blk_stack_atomic_writes_head/tail → blk_stack_atomic_writes_chunk_sectors
 */
static void blk_stack_atomic_writes_limits(struct queue_limits *t,
				struct queue_limits *b, sector_t start)
{
	/* NVMe 하위 장치가 atomic write를 지원하지 않으면 상위도 미지원. */
	if (!(b->features & BLK_FEAT_ATOMIC_WRITES))
		goto unsupported;

	/* NVMe atomic_write_hw_unit_min이 0이면 실제 지원 아님. */
	if (!b->atomic_write_hw_unit_min)
		goto unsupported;

	/* NVMe start 섹터가 atomic write 단위로 정렬되어야 함. */
	if (!blk_atomic_write_start_sect_aligned(start, b))
		goto unsupported;

	/* UINT_MAX indicates no stacking of bottom devices yet */
	/* [한국어] UINT_MAX 는 "아직 아무 하위 장치도 합치지 않았다"는 초기 표식이다.
	 * 첫 장치는 비교 대상이 없으므로 값을 그대로 채우는 head 경로를 탄다. */
	if (t->atomic_write_hw_max == UINT_MAX) {
		if (!blk_stack_atomic_writes_head(t, b))
			goto unsupported;
	} else {
		/* [한국어] 둘째부터는 기존 값과 교집합을 취해야 한다 — 한 장치라도 보장하지
		 * 못하면 스택 전체가 보장하지 못하기 때문이다. */
		if (!blk_stack_atomic_writes_tail(t, b))
			goto unsupported;
	}
	/* [한국어] RAID 스트라이프 경계도 원자성을 깨뜨릴 수 있다 — 한 쓰기가 두 디스크에
	 * 나뉘어 내려가면 한쪽만 반영될 수 있기 때문이다. chunk_sectors 로 한 번 더 자른다. */
	blk_stack_atomic_writes_chunk_sectors(t);
	/* [한국어] 성공 — 아래 unsupported 라벨로 떨어지지 않도록 여기서 반환한다 */
	return;

unsupported:
	/* [한국어] 하위 장치들의 원자성 보장이 서로 호환되지 않는 경우.
	 * MD/DM 처럼 여러 장치를 묶는 구성에서, 한쪽만 원자적 쓰기를 지원하거나
	 * 단위·경계가 어긋나면 스택 장치 전체로는 보장할 수 없다. */
	t->atomic_write_hw_max = 0;
	t->atomic_write_hw_unit_max = 0;
	t->atomic_write_hw_unit_min = 0;
	t->atomic_write_hw_boundary = 0;
}

/**
 * blk_stack_limits - MD/DM 등 스택형 장치의 queue_limits 병합
 * @t: 상위 장치 queue_limits
 * @b: 하위 구성 장치 queue_limits
 * @start: 하위 장치 내 첫 데이터 섹터
 *
 * 목적:
 *   NVMe SSD 위에 software RAID, device mapper, LUKS 등 스택형 장치를 얹을 때
 *   각 구성 장치의 block size, alignment, max_sectors, discard, zoned,
 *   atomic_write 한도를 교차 검증하고 최소 공통 분모로 병합한다.
 *
 * 호출 경로:
 *   md_run -> queue_limits_stack_bdev -> blk_stack_limits
 *   (또는 dm-table-load 경로)
 *
 * NVMe 연결 지점:
 *   - 하위 NVMe q->limits를 복사하여 상위 가상 장치의 bio splitting/merge
 *     조건으로 재사용한다.
 *   - BLK_FEAT_POLL/NOWAIT 등 NVMe 특화 feature를 상위로 상속한다.
 *   - zoned NVMe(ZNS)의 zone_append_sectors, zone_write_granularity를 병합.
 *
 * [한국어 보강]
 * @return: 모든 필드가 완벽히 정렬/호환되면 0. 하나라도 misaligned 보정이
 *     발생하면 -1(치명적 에러는 아니며, t->flags에 BLK_FLAG_MISALIGNED가
 *     설정되고 값 자체는 보정되어 계속 사용 가능하다). 호출자는 반환값을
 *     경고 로그 출력 여부 판단에만 사용한다.
 *
 * 이 함수는 값 하나하나를 "상위(t)와 하위(b) 중 더 제한적인 쪽"으로
 * 좁혀나가는 병합기다. min 계열로 합쳐야 하는 필드(max_sectors, segment
 * 수 등)는 min_not_zero/min으로, max 계열(logical/physical block size,
 * alignment 등)은 max로, 배수/최소공배수 관계가 필요한 필드(io_opt,
 * alignment_offset)는 lcm_not_zero로, chunk_sectors처럼 "공약수"가
 * 필요한 필드는 gcd로 처리한다. 정렬이 깨지는 경우(physical_block_size가
 * logical_block_size의 배수가 아님 등)에는 안전한 값으로 강제 보정하고
 * BLK_FLAG_MISALIGNED를 세워 상위에 경고할 수 있게 한다. 마지막으로
 * discard/secure-erase/zoned/atomic-write 관련 필드를 각각의 하위 병합
 * 함수(queue_limit_discard_alignment, blk_stack_atomic_writes_limits)로
 * 넘겨 처리한다.
 * 실행 컨텍스트: MD/DM 등 스택 드라이버의 큐 구성 경로, 프로세스 컨텍스트.
 * t/b는 호출자가 배타적으로 소유하는 지역/임시 구조체라고 가정하므로
 * 별도 락이 없다.
 * 호출자: queue_limits_stack_bdev(). 호출 대상: queue_limit_alignment_offset(),
 * blk_round_down_sectors(), queue_limit_discard_alignment(),
 * blk_stack_atomic_writes_limits().
 * 에러 경로: 정식 실패(음수 errno)는 없다 - misalignment는 값 보정 +
 * -1 반환으로만 표시되며, 큐 생성 자체를 막지 않는다.
 *
 * 호출 체인:
 *   queue_limits_stack_bdev → [blk_stack_limits] → queue_limit_alignment_offset/blk_round_down_sectors/blk_stack_atomic_writes_limits
 */
int blk_stack_limits(struct queue_limits *t, struct queue_limits *b,
		     sector_t start)
{
	unsigned int top, bottom, alignment; /* 정렬 구간 비교 및 하위 discard alignment 계산용 임시 변수. */
	int ret = 0; /* misalignment가 하나라도 발생하면 -1로 바뀌는 최종 반환값. */

	/* NVMe feature flags 중 상위로 상속 가능한 마스크 복사. */
	t->features |= (b->features & BLK_FEAT_INHERIT_MASK);

	/*
	 * Some feaures need to be supported both by the stacking driver and all
	 * underlying devices.  The stacking driver sets these flags before
	 * stacking the limits, and this will clear the flags if any of the
	 * underlying devices does not support it.
	 *
	 * NVMe: BLK_FEAT_NOWAIT는 polled completions 외에도 async poll 경로와
	 * 관련. BLK_FEAT_POLL은 nvme_poll_irqdisable 등 상위 poll 경로에 영향.
	 */
	/* NVMe NOWAIT 지원: 모든 하위 장치가 NOWAIT 지원해야 상속. */
	if (!(b->features & BLK_FEAT_NOWAIT))
		t->features &= ~BLK_FEAT_NOWAIT;
	/* NVMe POLL 지원: 모든 하위 장치가 POLL 지원해야 상속. */
	if (!(b->features & BLK_FEAT_POLL))
		t->features &= ~BLK_FEAT_POLL;

	/* NVMe misalignment flag는 하위 장치가 한 개라도 설정되면 상속. */
	t->flags |= (b->flags & BLK_FLAG_MISALIGNED);

	/*
	 * NVMe: 상위 장치의 I/O 크기는 하위 NVMe max_sectors를 초과할 수 없다.
	 * bio가 클 경우 blk_queue_split()에서 분할된다.
	 */
	/* NVMe 상위 max_sectors = min(상위, 하위). */
	t->max_sectors = min_not_zero(t->max_sectors, b->max_sectors);
	/* NVMe 상위 사용자 한도 = min(상위, 하위). */
	t->max_user_sectors = min_not_zero(t->max_user_sectors,
			b->max_user_sectors);
	/* NVMe 상위 MDTS = min(상위, 하위). */
	t->max_hw_sectors = min_not_zero(t->max_hw_sectors, b->max_hw_sectors);
	/* NVMe SCSI ULP max_dev_sectors 병합. */
	t->max_dev_sectors = min_not_zero(t->max_dev_sectors, b->max_dev_sectors);
	/* NVMe Write Zeroes 최대 섹터 병합. */
	t->max_write_zeroes_sectors = min(t->max_write_zeroes_sectors,
					b->max_write_zeroes_sectors);
	/* NVMe Write Zeroes 사용자 unmap 한도 병합: 더 보수적인(작은) 값 채택. */
	t->max_user_wzeroes_unmap_sectors =
			min(t->max_user_wzeroes_unmap_sectors,
			    b->max_user_wzeroes_unmap_sectors);
	/* NVMe Write Zeroes HW unmap 한도 병합: 더 보수적인(작은) 값 채택. */
	t->max_hw_wzeroes_unmap_sectors =
			min(t->max_hw_wzeroes_unmap_sectors,
			    b->max_hw_wzeroes_unmap_sectors);

	/* NVMe ZNS Zone Append 최대 섹터 병합. */
	t->max_hw_zone_append_sectors = min(t->max_hw_zone_append_sectors,
					b->max_hw_zone_append_sectors);

	/*
	 * NVMe: PRP/SGL segment 경계는 상위/하위 중 더 작은 값을 따라야 한다.
	 * 그렇지 않으면 상위 bio를 분할할 때 NVMe DMA 엔진이 경계를 넘는
	 * descriptor list를 받게 될 수 있다.
	 */
	/* NVMe PRP/SGL seg_boundary_mask 병합: 더 작은 경계 선택. */
	t->seg_boundary_mask = min_not_zero(t->seg_boundary_mask,
					    b->seg_boundary_mask);
	/* NVMe virtual boundary 병합: 더 작은 경계 선택. */
	t->virt_boundary_mask = min_not_zero(t->virt_boundary_mask,
					    b->virt_boundary_mask);

	/* NVMe PRP/SGL 최대 segment 수 병합. */
	t->max_segments = min_not_zero(t->max_segments, b->max_segments);
	/* NVMe DSM/Deallocate 최대 segment 수 병합. */
	t->max_discard_segments = min_not_zero(t->max_discard_segments,
					       b->max_discard_segments);
	/* NVMe PI metadata segment 수 병합. */
	t->max_integrity_segments = min_not_zero(t->max_integrity_segments,
						 b->max_integrity_segments);

	/* NVMe 단일 PRP/SGL 엔트리 크기 병합. */
	t->max_segment_size = min_not_zero(t->max_segment_size,
					   b->max_segment_size);

	/* NVMe 하위 장치의 시작 섹터 기준 alignment offset 계산. */
	alignment = queue_limit_alignment_offset(b, start);

	/* Bottom device has different alignment.  Check that it is
	 * compatible with the current top alignment.
	 */
	/* NVMe 상위/하위 alignment가 다른 경우 호환성 검사. */
	if (t->alignment_offset != alignment) {

		/* NVMe 상위 장치의 정렬 구간(바이트). */
		top = max(t->physical_block_size, t->io_min)
			+ t->alignment_offset;
		/* NVMe 하위 장치의 정렬 구간(바이트). */
		bottom = max(b->physical_block_size, b->io_min) + alignment;

		/* Verify that top and bottom intervals line up */
		/* NVMe 상위/하위 정렬 구간이 서로 배수 관계가 아니면 misaligned. */
		if (max(top, bottom) % min(top, bottom)) {
			t->flags |= BLK_FLAG_MISALIGNED;
			ret = -1;
		}
	}

	/* NVMe logical_block_size는 상위/하위 중 큰 값으로 상속. */
	t->logical_block_size = max(t->logical_block_size,
				    b->logical_block_size);

	/* NVMe physical_block_size는 상위/하위 중 큰 값으로 상속. */
	t->physical_block_size = max(t->physical_block_size,
				     b->physical_block_size);

	/* NVMe io_min은 상위/하위 중 큰 값으로 상속. */
	t->io_min = max(t->io_min, b->io_min);
	/* NVMe io_opt는 LCM으로 병합하여 둘 다 만족. */
	t->io_opt = lcm_not_zero(t->io_opt, b->io_opt);
	/* NVMe DMA alignment는 상위/하위 중 큰 값으로 상속. */
	t->dma_alignment = max(t->dma_alignment, b->dma_alignment);

	/* Set non-power-of-2 compatible chunk_sectors boundary */
	/* [한국어] chunk_sectors 는 "이 경계를 넘는 IO 는 쪼개라"는 값이다. 두 하위 장치의
	 * 경계가 다르면 **양쪽 모두**의 경계를 지켜야 하므로 최대공약수를 쓴다.
	 * 최솟값이 아니라 GCD 인 이유가 이것이다 — 예: 8과 12 면 4 마다 잘라야 둘 다 만족한다. */
	if (b->chunk_sectors)
		t->chunk_sectors = gcd(t->chunk_sectors, b->chunk_sectors);

	/* Physical block size a multiple of the logical block size? */
	/* [한국어] 물리 블록은 논리 블록의 정수 배여야 한다(512e 면 8배). 어긋나면
	 * 정렬 계산이 전부 무의미해지므로 물리 = 논리로 낮춰 안전한 값으로 만든다. */
	if (t->physical_block_size & (t->logical_block_size - 1)) {
		t->physical_block_size = t->logical_block_size;
		t->flags |= BLK_FLAG_MISALIGNED;	/* [한국어] "이 스택은 정렬이 어긋나 있다"를 큐에 남긴다.
							 * 파일시스템이 sysfs 로 이 상태를 읽어 경고할 수 있다. */
		ret = -1;				/* [한국어] 실패가 아니라 "보정했음"이다 — 호출자는 계속 진행하되
							 * 성능 저하를 감수한다는 뜻으로 이 값을 해석한다. */
	}

	/* Minimum I/O a multiple of the physical block size? */
	/* [한국어] io_min(최소 권장 IO 크기)이 물리 블록의 배수가 아니면 읽기-수정-쓰기를
	 * 유발한다. 물리 블록 크기로 낮춘다. */
	if (t->io_min & (t->physical_block_size - 1)) {
		t->io_min = t->physical_block_size;
		t->flags |= BLK_FLAG_MISALIGNED;
		ret = -1;
	}

	/* Optimal I/O a multiple of the physical block size? */
	/* [한국어] io_opt(최적 IO 크기)는 힌트일 뿐이라, 어긋나면 낮추는 대신 0(힌트 없음)으로
	 * 만든다. 잘못된 힌트보다 힌트가 없는 편이 낫기 때문이다. */
	if (t->io_opt & (t->physical_block_size - 1)) {
		t->io_opt = 0;
		t->flags |= BLK_FLAG_MISALIGNED;
		ret = -1;
	}

	/* chunk_sectors a multiple of the physical block size? */
	/* [한국어] chunk 경계가 물리 블록 중간에 걸리면 그 경계에서 자른 IO 가 부분 블록
	 * 쓰기가 된다. 역시 0(경계 없음)으로 무력화한다. */
	if (t->chunk_sectors % (t->physical_block_size >> SECTOR_SHIFT)) {
		t->chunk_sectors = 0;
		t->flags |= BLK_FLAG_MISALIGNED;
		ret = -1;
	}

	/* Find lowest common alignment_offset */
	/* [한국어] 두 장치의 정렬 오프셋을 동시에 만족하는 지점을 찾는다.
	 * 주기가 서로 다르므로 최소공배수 주기로 맞춰 본 뒤 granularity 안으로 접는다. */
	t->alignment_offset = lcm_not_zero(t->alignment_offset, alignment)
		% max(t->physical_block_size, t->io_min);

	/* Verify that new alignment_offset is on a logical block boundary */
	/* NVMe alignment_offset가 logical_block_size 경계에 있지 않으면 misaligned. */
	if (t->alignment_offset & (t->logical_block_size - 1)) {
		t->flags |= BLK_FLAG_MISALIGNED;
		ret = -1;
	}

	/* NVMe I/O 크기 한도를 LBA 단위로 내림. */
	t->max_sectors = blk_round_down_sectors(t->max_sectors, t->logical_block_size);
	t->max_hw_sectors = blk_round_down_sectors(t->max_hw_sectors, t->logical_block_size);
	t->max_dev_sectors = blk_round_down_sectors(t->max_dev_sectors, t->logical_block_size);

	/* Discard alignment and granularity */
	/* NVMe Deallocate 지원 하위 장치가 있을 때 discard 한도 병합. */
	if (b->discard_granularity) {
		alignment = queue_limit_discard_alignment(b, start);

		/* NVMe 상위 max_discard_sectors = min(상위, 하위). */
		t->max_discard_sectors = min_not_zero(t->max_discard_sectors,
						      b->max_discard_sectors);
		t->max_hw_discard_sectors = min_not_zero(t->max_hw_discard_sectors,
							 b->max_hw_discard_sectors);
		/* NVMe discard_granularity는 더 큰 값으로 상속(보수적). */
		t->discard_granularity = max(t->discard_granularity,
					     b->discard_granularity);
		/* NVMe discard_alignment는 LCM 병합 후 granularity 내 정규화. */
		t->discard_alignment = lcm_not_zero(t->discard_alignment, alignment) %
			t->discard_granularity;
	}
	/* NVMe secure erase 최대 섹터 병합. */
	t->max_secure_erase_sectors = min_not_zero(t->max_secure_erase_sectors,
						   b->max_secure_erase_sectors);
	/* NVMe ZNS zone_write_granularity는 더 큰 값으로 상속. */
	t->zone_write_granularity = max(t->zone_write_granularity,
					b->zone_write_granularity);
	/* NVMe ZNS가 아닌 장치에서는 ZNS 관련 필드 클리어. */
	if (!(t->features & BLK_FEAT_ZONED)) {
		t->zone_write_granularity = 0;
		t->max_zone_append_sectors = 0;
	}
	/* 원자적 쓰기 한도는 별도 서브함수로 위임해 병합/호환성 검사. */
	blk_stack_atomic_writes_limits(t, b, start);

	/* misalignment가 하나라도 있었으면 -1, 완전히 호환되면 0을 반환. */
	return ret;
}
EXPORT_SYMBOL(blk_stack_limits);

/**
 * queue_limits_stack_bdev - block device 기반 queue_limits 스택 병합
 * @t: 상위 queue_limits
 * @bdev: 하위 block device
 * @offset: 하위 장치 내 데이터 시작 오프셋
 * @pfx: 경고 메시지 접두사
 *
 * NVMe 연결 지점:
 *   bdev_limits(bdev)로 하위 NVMe 장치의 q->limits를 가져와
 *   blk_stack_limits()로 병합한다. 파티션의 bd_start_sect를 더해 파티션
 *   시작 위치를 반영한다.
 *
 * [한국어 보강]
 * @return: 없음 (void). 병합 결과의 misalignment 여부는 t->flags의
 *     BLK_FLAG_MISALIGNED로 확인 가능하며, 여기서는 경고 로그만 남긴다.
 *
 * blk_stack_limits()가 구조체 두 개(queue_limits)를 직접 받는 저수준
 * API인 반면, 이 함수는 block_device를 받아 bdev_limits()로 실제
 * q->limits를 얻고 파티션 시작 섹터(get_start_sect)까지 자동으로
 * 더해주는 상위 편의 함수다. MD/DM 등이 실제 bdev를 스택에 추가할 때
 * 저수준 API 대신 이 함수를 사용한다.
 * 실행 컨텍스트: 드라이버/DM 테이블 로드 경로의 프로세스 컨텍스트.
 * 호출자: drivers/md/md.c, dm-table.c 등. 호출 대상: bdev_limits(),
 * get_start_sect(), blk_stack_limits().
 * 에러 경로: blk_stack_limits()가 misalignment(-1)를 반환하면 정식 에러로
 * 취급하지 않고 pr_notice()로 경고만 남긴다(치명적이지 않음).
 *
 * 호출 체인:
 *   md_run/dm_table_add_target → [queue_limits_stack_bdev] → blk_stack_limits
 */
void queue_limits_stack_bdev(struct queue_limits *t, struct block_device *bdev,
		sector_t offset, const char *pfx)
{
	/* NVMe 파티션 시작 섹터를 offset에 더해 alignment 검사. misalignment
	 * 발생(-1) 시 pr_notice로 경고만 남기고 진행(치명적 오류 아님). */
	if (blk_stack_limits(t, bdev_limits(bdev),
			get_start_sect(bdev) + offset))
		pr_notice("%s: Warning: Device %pg is misaligned\n",
			pfx, bdev);
}
EXPORT_SYMBOL_GPL(queue_limits_stack_bdev);

/**
 * queue_limits_stack_integrity - 스택형 장치의 integrity profile 병합
 * @t: target queue limits
 * @b: base queue limits
 *
 * NVMe 연결 지점:
 *   NVMe DIF/DIX PI 설정이 하위 장치에서 상위 스택 장치로 상속될 수 있는지
 *   검사한다. 메타데이터 크기, interval, tag size, checksum 종류 등이
 *   일치해야 상위 장치도 PI를 노출할 수 있다.
 *
 * [한국어 보강]
 * @return: PI 프로필이 호환되어 상속(또는 최초 복사)에 성공하면 true.
 *     이미 스택된 상태에서 하위 장치와 필드가 달라 호환 불가하면 false
 *     (이 경우 t->integrity는 모두 0으로 초기화되어 PI 미지원이 된다).
 *
 * MD/DM 등으로 여러 NVMe 장치를 묶을 때, 각 장치가 서로 다른 DIF/DIX
 * 포맷(metadata_size, checksum 종류 등)을 쓰면 상위 가상 장치가 일관된
 * PI를 보장할 수 없다. 이 함수는 "아직 아무 장치도 스택되지 않은 상태
 * (BLK_INTEGRITY_STACKED 미설정)"라면 첫 하위 장치의 PI 프로필을 그대로
 * 복사하고, "이미 스택된 상태"라면 metadata_size/interval_exp/tag_size/
 * csum_type/pi_tuple_size/REF_TAG가 모두 일치하는지 검사해 다르면
 * incompatible로 점프한다. SPLIT_INTERVAL_CAPABLE은 하위 장치 중 하나라도
 * 지원하지 않으면 상위에서도 클리어한다(보수적 병합).
 * 실행 컨텍스트: blk_stack_limits() 밖에서 스택 드라이버가 별도로 호출하는
 * 프로세스 컨텍스트(주: blk_stack_limits() 자체는 integrity를 병합하지
 * 않으며, MD/DM이 이 함수를 별도로 호출해야 한다).
 * 호출자: drivers/md/dm-table.c, drivers/md/md.c 등 PI를 지원하려는 스택
 * 드라이버. 호출 대상: 없음 (구조체 필드 비교/대입만 수행).
 * 에러 경로: 비호환 시 false를 반환하고 t->integrity를 통째로 0-clear.
 *
 * 호출 체인:
 *   dm_table_add_target/md_run → [queue_limits_stack_integrity] → (필드 비교/대입으로 종료)
 */
bool queue_limits_stack_integrity(struct queue_limits *t,
		struct queue_limits *b)
{
	/* 상위(target) 스택 장치의 PI 프로필 포인터. */
	struct blk_integrity *ti = &t->integrity;
	/* 하위(base) 장치의 PI 프로필 포인터. */
	struct blk_integrity *bi = &b->integrity;

	/* 커널이 blk-integrity 지원 없이 빌드됐다면 PI 검사 자체가 무의미 -
	 * 항상 호환(true)으로 처리해 상위 로직이 막히지 않게 한다. */
	if (!IS_ENABLED(CONFIG_BLK_DEV_INTEGRITY))
		return true;

	/* NVMe PI가 이미 스택된 상태면 하위 장치와 모든 필드 일치 필요. */
	if (ti->flags & BLK_INTEGRITY_STACKED) {
		/* NVMe DIF/DIX metadata_size 불일치 시 상속 불가. */
		if (ti->metadata_size != bi->metadata_size)
			goto incompatible;
		/* NVMe PI interval_exp 불일치 시 상속 불가. */
		if (ti->interval_exp != bi->interval_exp)
			goto incompatible;
		/* NVMe PI tag_size 불일치 시 상속 불가. */
		if (ti->tag_size != bi->tag_size)
			goto incompatible;
		/* NVMe PI checksum type 불일치 시 상속 불가. */
		if (ti->csum_type != bi->csum_type)
			goto incompatible;
		/* NVMe PI tuple size 불일치 시 상속 불가. */
		if (ti->pi_tuple_size != bi->pi_tuple_size)
			goto incompatible;
		/* NVMe REF_TAG flag 불일치 시 상속 불가. */
		if ((ti->flags & BLK_INTEGRITY_REF_TAG) !=
		    (bi->flags & BLK_INTEGRITY_REF_TAG))
			goto incompatible;
		/* NVMe SPLIT_INTERVAL_CAPABLE는 하위 장치가 지원 안 하면 클리어. */
		if ((ti->flags & BLK_SPLIT_INTERVAL_CAPABLE) &&
		    !(bi->flags & BLK_SPLIT_INTERVAL_CAPABLE))
			ti->flags &= ~BLK_SPLIT_INTERVAL_CAPABLE;
	} else {
		/* NVMe PI profile을 상위 장치로 처음 복사. */
		ti->flags = BLK_INTEGRITY_STACKED;
		ti->flags |= (bi->flags & BLK_INTEGRITY_DEVICE_CAPABLE) |
			     (bi->flags & BLK_INTEGRITY_REF_TAG) |
			     (bi->flags & BLK_SPLIT_INTERVAL_CAPABLE);
		ti->csum_type = bi->csum_type;
		ti->pi_tuple_size = bi->pi_tuple_size;
		ti->metadata_size = bi->metadata_size;
		ti->pi_offset = bi->pi_offset;
		ti->interval_exp = bi->interval_exp;
		ti->tag_size = bi->tag_size;
	}
	/* 이미 스택된 경우 모든 필드 일치 확인 완료, 처음인 경우 복사 완료 -
	 * 두 경우 모두 호환 성공으로 true 반환. */
	return true;

incompatible:
	/* NVMe PI 호환 실패 시 상위 장치는 PI 미지원으로 초기화. */
	memset(ti, 0, sizeof(*ti));
	return false;
}
EXPORT_SYMBOL_GPL(queue_limits_stack_integrity);

/**
 * blk_set_queue_depth - NVMe queue depth 등록
 * @q: 등록할 request_queue
 * @depth: IO SQ에서 동시 진행 가능한 request 수
 *
 * 목적:
 *   NVMe 컨트롤러의 Create IO Queue pair에서 설정한 Queue Size를 block layer에
 *   알려, inflight request 수와 tag 할당 범위를 맞춘다.
 *
 * 호출 경로:
 *   nvme_setup_io_queues -> nvme_setup_io_queues -> blk_mq_tag_set_depth ->
 *   blk_set_queue_depth
 *
 * NVMe 연결 지점:
 *   - q->queue_depth는 tag_set->queue_depth와 연동되어 SQ tail doorbell write
 *     횟수 및 CID 재사용에 영향을 준다.
 *   - rq_qos_queue_depth_changed()로 QoS/scheduler에도 전파된다.
 *
 * [한국어 보강]
 * @return: 없음 (void). 실패 조건 없음.
 *
 * NVMe는 Create I/O Submission/Completion Queue 명령으로 SQ/CQ pair의
 * 엔트리 수(Queue Size)를 정하는데, 이 값이 곧 동시에 outstanding 가능한
 * command 수(=blk-mq tag 수, CID 재사용 폭)의 상한이 된다. q->queue_depth를
 * 갱신하면 wbt(writeback throttle)나 io.latency 같은 rq-qos 정책이
 * inflight 제한을 다시 계산해야 하므로 rq_qos_queue_depth_changed()로
 * 이를 통지한다.
 * 실행 컨텍스트: 드라이버 큐 구성 경로의 프로세스 컨텍스트.
 * 호출자: nvme_setup_io_queues() 등. 호출 대상: rq_qos_queue_depth_changed().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   nvme_setup_io_queues → [blk_set_queue_depth] → rq_qos_queue_depth_changed
 */
void blk_set_queue_depth(struct request_queue *q, unsigned int depth)
{
	/* NVMe IO SQ/CQ pair의 Queue Size를 block layer queue_depth에 반영.
	 * 이 값은 blk-mq tag 범위와 CID 할당 상한을 결정한다. */
	q->queue_depth = depth;
	/* NVMe queue depth 변경을 QoS/scheduler에 전파하여 inflight 제한 갱신. */
	rq_qos_queue_depth_changed(q);
}
EXPORT_SYMBOL(blk_set_queue_depth);

/**
 * bdev_alignment_offset - block device의 시작 위치 정렬 오프셋 반환
 * @bdev: 대상 block device
 *
 * NVMe 연결 지점:
 *   NVMe namespace 위의 파티션이나 스택 장치가 물리 블록 경계에 맞지 않게
 *   시작할 때, 상위 bio 경로에서 사용할 alignment_offset을 반환한다.
 *   BLK_FLAG_MISALIGNED가 설정되면 -1을 반환하여 상위에서 처리를 강제한다.
 *
 * [한국어 보강]
 * @return: 정상 정렬 상태면 alignment_offset(바이트, 파티션이면 재계산된
 *     값), 이미 misaligned로 표시된 큐라면 -1.
 *
 * sysfs의 /sys/block/<disk>/alignment_offset이 이 함수를 통해 노출된다.
 * BLK_FLAG_MISALIGNED가 이미 설정된 큐(blk_stack_limits()에서 정렬
 * 불가능이 확정된 경우)는 계산 자체가 의미 없으므로 -1(사용자 공간
 * 컨벤션상 "알 수 없음/비정상")을 즉시 반환한다. 파티션이면 파티션의
 * bd_start_sect를 반영해 queue_limit_alignment_offset()으로 재계산하고,
 * 전체 디스크라면 q->limits.alignment_offset을 그대로 반환한다.
 * 실행 컨텍스트: sysfs 읽기 등 프로세스 컨텍스트. 순수 조회 함수.
 * 호출자: block/genhd.c의 sysfs 속성 핸들러 등. 호출 대상:
 * bdev_get_queue(), queue_limit_alignment_offset().
 * 에러 경로: 없음(항상 유효한 값 또는 관례적 -1을 반환).
 *
 * 호출 체인:
 *   (sysfs alignment_offset 읽기) → [bdev_alignment_offset] → queue_limit_alignment_offset
 */
int bdev_alignment_offset(struct block_device *bdev)
{
	/* bdev가 속한 request_queue를 조회 - q->limits를 참조하기 위함. */
	struct request_queue *q = bdev_get_queue(bdev);

	/* NVMe misalignment 발생 시 상위에서 강제 처리를 위해 -1 반환. */
	if (q->limits.flags & BLK_FLAG_MISALIGNED)
		return -1;
	/* NVMe 파티션의 경우 bd_start_sect를 고려한 offset 계산. */
	if (bdev_is_partition(bdev))
		return queue_limit_alignment_offset(&q->limits,
				bdev->bd_start_sect);
	/* 전체 디스크(파티션 아님)는 큐의 alignment_offset을 그대로 반환. */
	return q->limits.alignment_offset;
}
EXPORT_SYMBOL_GPL(bdev_alignment_offset);

/**
 * bdev_discard_alignment - block device의 discard 정렬 오프셋 반환
 * @bdev: 대상 block device
 *
 * NVMe 연결 지점:
 *   NVMe Deallocate(Trim) 명령을 발행할 때 discard_granularity 경계에 맞추기
 *   위해 파티션 시작 섹터를 고려한 discard_alignment를 반환한다.
 *
 * [한국어 보강]
 * @return: 파티션이면 bd_start_sect를 반영한 discard alignment(바이트),
 *     전체 디스크면 q->limits.discard_alignment 그대로.
 *
 * bdev_alignment_offset()의 discard 버전이다. sysfs의
 * /sys/block/<disk>/discard_alignment로 노출되며, blkdev_issue_discard()가
 * NVMe Deallocate(DSM) 요청을 discard_granularity 경계에 맞춰 잘라 보낼 때
 * 이 값을 참조한다.
 * 실행 컨텍스트: sysfs 읽기 등 프로세스 컨텍스트. 순수 조회 함수.
 * 호출자: block/genhd.c sysfs 핸들러 등. 호출 대상: bdev_get_queue(),
 * queue_limit_discard_alignment().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   (sysfs discard_alignment 읽기) → [bdev_discard_alignment] → queue_limit_discard_alignment
 */
unsigned int bdev_discard_alignment(struct block_device *bdev)
{
	/* bdev가 속한 request_queue를 조회 - q->limits를 참조하기 위함. */
	struct request_queue *q = bdev_get_queue(bdev);

	/* NVMe 파티션의 경우 bd_start_sect를 고려한 discard alignment 계산. */
	if (bdev_is_partition(bdev))
		return queue_limit_discard_alignment(&q->limits,
				bdev->bd_start_sect);
	/* 전체 디스크(파티션 아님)는 큐의 discard_alignment를 그대로 반환. */
	return q->limits.discard_alignment;
}
EXPORT_SYMBOL_GPL(bdev_discard_alignment);

/* NVMe 관점 핵심 요약
 *
 * - 본 파일은 NVMe 컨트롤러가 보고한 MDTS, LBA format, PRP/SGL segment 한도,
 *   feature flags를 block layer 표준 queue_limits로 정규화한다.
 * - 정규화된 한도는 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   mq_ops->queue_rq (간접 호출; NVMe PCIe 면 nvme_queue_rq -> nvme_sq_copy_cmd -> nvme_write_sq_db) 경로에서 request 조립,
 *   bio splitting, tag/CID 할당에 직접 사용된다.
 * - NVMe SSD 위에 MD/DM 등 스택형 장치가 있을 때 본 파일의 stacking 함수가
 *   하위 NVMe q->limits를 상위 가상 장치로 병합하여 하드웨어 특성이 올바르게
 *   상속되도록 한다.
 * - queue_depth 설정은 NVMe IO SQ 크기와 blk-mq tag 범위를 일치시켜 CID
 *   고갈 및 doorbell overflow를 방지한다.
 * - integrity/atomic_write/zoned 검증은 NVMe DIF/DIX, FUA, ZNS 등 확장
 *   기능을 block layer에서 안전하게 노출하기 위한 게이트키퍼 역할을 한다.
 */
