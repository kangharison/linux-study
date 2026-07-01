// SPDX-License-Identifier: GPL-2.0

/*
 * block/partitions/of.c - OpenFirmware(DT) 고정 파티션 파서
 *
 * 이 파일은 NVMe SSD가 blkdev(gendisk) 형태로 노출한 네임스페이스에 대해
 * Device Tree의 "fixed-partitions" 노드를 읽어 파티션 테이블을 구성한다.
 * 파티션의 바이트 단위 오프셋/크기를 섹터 단위로 변환 및 검증한 뒤
 * parsed_partitions 상태에 등록하면, 이후 상위 파일 시스템이나 blk-mq가
 * 파티션 단위로 bio를 생성해
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 * nvme_submit_cmd(doorbell) 경로로 NVMe I/O 명령(SQ/CID/PRP/SGL)으로 변환된다.
 *
 * 호출 흐름:
 *   check_partition() (block/partitions/core.c)
 *     -> of_partition()
 *       -> validate_of_partition() / add_of_partition()
 *         -> put_partition()
 *
 * 연관 파일: block/partitions/core.c, block/partitions/check.h,
 *           drivers/nvme/host/core.c, drivers/nvme/host/pci.c
 */

#include <linux/blkdev.h>	/* gendisk/bdev -> bio -> blk-mq -> nvme_queue_rq 경로의 핵심 헤더 */
#include <linux/major.h>	/* 블록 디바이스 major 번호; NVMe namespace는 NVME_MAJOR 기반으로 노출됨 */
#include <linux/of.h>		/* Device Tree 프로퍼티 파싱; firmware가 NVMe namespace에 바인딩한 메타데이터 원천 */
#include <linux/string.h>	/* strscpy 등; 파티션 라벨은 /dev/nvme0nXpN 식별 정보로 활용될 수 있음(추정) */
#include "check.h"		/* parsed_partitions, put_partition, SECTOR_SIZE 정의; 파티션 스캔 공용 인프라 */

/*
 * validate_of_partition - DT 고정 파티션 노드의 오프셋/크기를 검증한다.
 *
 * 목적:
 *   "reg" 프로퍼티에 명시된 파티션의 시작/크기가 NVMe 섹터(SECTOR_SIZE)
 *   경계를 준수하는지 확인한다. 잘못된 오프셋은 이후 LBA -> PRP/SGL
 *   변환 시 언더레이에서 정렬 오류를 유발할 수 있다.
 *
 * 호출 경로:
 *   of_partition() -> validate_of_partition(np, slot)
 *
 * NVMe 연결점:
 *   검증된 offset/size가 put_partition()을 거쳐 gendisk의 파티션
 *   테이블(parts[])에 저장되며, 이 값은 NVMe 명령의 시작 LBA(SLBA)로
 *   변환되어 doorbell을 통해 SQ에 삽입된다.
 */
static int validate_of_partition(struct device_node *np, int slot)
{
	u64 offset, size;	/* 바이트 단위 파티션 오프셋/크기; 이후 SECTOR_SIZE로 나눠 NVMe LBA로 변환 */
	int len;		/* "reg" 프로퍼티 길이; 부족하면 offset/size 파싱 불가 -> LBA 계산 실패 */

	const __be32 *reg = of_get_property(np, "reg", &len);	/* DT에 기록된 파티션 메타데이터 획득; OpenFirmware 파서는 NVMe 미디어 직접 read_part_sector를 수행하지 않음 */
	int a_cells = of_n_addr_cells(np);	/* 주소 셀 수; offset 정밀도 및 NVMe namespace 내 바이트 오프셋 표현 범위 결정 */
	int s_cells = of_n_size_cells(np);	/* 크기 셀 수; size 정밀도 및 NVMe NLB(Number of Logical Blocks) 산출 범위 결정 */

	/* Make sure reg len match the expected addr and size cells */
	/* reg 길이가 주소 셀과 크기 셀 합과 일치하는지 확인 (DT 규격 위반 시 이후 LBA 계산 불가) */
	if (len / sizeof(*reg) != a_cells + s_cells)	/* (a_cells + s_cells)개의 __be32가 아니면 reg 파싱 실패 -> SLBA 산출 불가 */
		return -EINVAL;				/* 잘못된 DT 메타데이터; 파티션 스캔 중단 -> NVMe namespace는 단일 블록 디바이스로만 사용됨 */

	/* Validate offset conversion from bytes to sectors */
	offset = of_read_number(reg, a_cells);		/* DT 빅엔디언 주소 -> CPU u64; NVMe namespace 내 바이트 오프셋 */
	if (offset % SECTOR_SIZE)			/* SECTOR_SIZE(보통 512B) 미정렬이면 NVMe PRP/SGL 정렬 및 SLBA 계산 오류 유발 */
		return -EINVAL;				/* 오프셋 불량으로 파티션 등록 포기; NVMe I/O 시 잘못된 시작 LBA 방지(추정) */

	/* Validate size conversion from bytes to sectors */
	size = of_read_number(reg + a_cells, s_cells);	/* 파티션 크기(바이트); 0이면 유효한 NVMe Read/Write 범위 없음 */
	if (!size || size % SECTOR_SIZE)		/* 크기 0 또는 미정렬 시 NVMe NLB 계산 불가/비정상 I/O 범위 */
		return -EINVAL;				/* 크기 불량으로 파티션 등록 포기 */

	return 0;	/* 검증 성공 -> add_of_partition() -> put_partition()을 통해 gendisk 파티션 테이블 구성 */
}

/*
 * add_of_partition - 검증된 DT 파티션을 parsed_partitions에 등록한다.
 *
 * 목적:
 *   DT 노드에서 읽은 바이트 단위 값을 섹터 단위로 변환하여
 *   parsed_partitions->parts[slot]에 기록한다.
 *
 * 호출 경로:
 *   of_partition() -> add_of_partition(state, slot, np)
 *
 * 사용하는 major struct 및 NVMe 연결점:
 *   - state->disk:
 *       NVMe 컨트롤러가 생성한 namespace에 해당하는 gendisk 구조체.
 *       NVMe I/O 경로의 최상위 블록 디바이스를 나타낸다.
 *   - state->parts[slot].from:
 *       이 파티션의 시작 LBA. NVMe Read/Write 명령의 SLBA(Starting LBA)로
 *       매핑된다.
 *   - state->parts[slot].size:
 *       파티션 섹터 수. NVMe 명령의 NLB(Number of Logical Blocks) 계산에
 *       사용된다.
 *   - state->parts[slot].flags:
 *       read-only 파티션은 ADDPART_FLAG_READONLY로 설정되어, 이후
 *       bdev_read_only() 경로를 통해 NVMe Write 명령 차단 여부를
 *       결정할 수 있다(추정).
 *   - state->parts[slot].info.volname:
 *       파티션 라벨. /dev/nvme0nXpN 형태의 디바이스 노드명 및 사용자
 *       공간 식별 정보로 노출될 수 있다(추정).
 */
static void add_of_partition(struct parsed_partitions *state, int slot,
			     struct device_node *np)
{
	struct partition_meta_info *info;	/* gendisk parts[]의 메타정보; /dev/nvme0nXpN 라벨/UUID 등 노출 원천 */
	const char *partname;			/* DT label/name; 사용자 공간 및 udev가 NVMe 파티션 식별에 사용(추정) */
	int len;

	const __be32 *reg = of_get_property(np, "reg", &len);	/* 등록 단계에서 reg 재획득; validate_of_partition() 통과 후이므로 길이는 유효 */
	int a_cells = of_n_addr_cells(np);
	int s_cells = of_n_size_cells(np);

	/* Convert bytes to sector size */
	/* DT의 바이트 단위 오프셋/크기를 NVMe/블록 계층이 사용하는 섹터 단위로 변환 */
	u64 offset = of_read_number(reg, a_cells) / SECTOR_SIZE;		/* 시작 섹터; 파일 시스템 bio의 시작 LBA에 더해져 NVMe SLBA가 됨 */
	u64 size = of_read_number(reg + a_cells, s_cells) / SECTOR_SIZE;	/* 섹터 수; bio 길이 -> NVMe PRP/SGL 길이 및 NLB 산출에 사용 */

	/* 파티션 정보를 parsed_partitions에 기록 -> gendisk 파티션 테이블 구성 */
	put_partition(state, slot, offset, size);	/* block/partitions/core.c -> add_partition() 경로; bio 재배치의 LBA 오프셋 기준 확정 */

	/* read-only 속성이면 블록 레이어 쓰기 금지 플래그 설정 (NVMe Write 정책과 연동 가능)(추정) */
	if (of_property_read_bool(np, "read-only"))
		state->parts[slot].flags |= ADDPART_FLAG_READONLY;	/* bdev_read_only() 체크 -> REQ_OP_WRITE 거부 시 NVMe Write 명행 제출 차단 가능(추정) */

	/*
	 * Follow MTD label logic, search for label property,
	 * fallback to node name if not found.
	 */
	info = &state->parts[slot].info;	/* gendisk->part0..partN 의 메타정보 포인터 */
	partname = of_get_property(np, "label", &len);
	if (!partname)
		partname = of_get_property(np, "name", &len);	/* label 없으면 DT 노드명을 대체 라벨로 사용 */
	strscpy(info->volname, partname, sizeof(info->volname));	/* /dev/nvme0nXpN 의식별자 또는 udev 속성으로 노출될 문자열(추정) */

	seq_buf_printf(&state->pp_buf, "(%s)", info->volname);	/* 파티션 스캔 결과 버퍼에 라벨 기록; printk -> dmesg 경로로 노출 */
}

/*
 * of_partition - NVMe SSD의 gendisk에 대해 DT 고정 파티션을 탐색한다.
 *
 * 목적:
 *   disk_to_dev(state->disk)->of_node 아래 "fixed-partitions" 호환 노드를
 *   찾아, 자식 노드들을 파티션으로 등록한다.
 *
 * 호출 경로:
 *   check_partition() (block/partitions/core.c)
 *     -> of_partition()
 *       -> validate_of_partition()
 *       -> add_of_partition()
 *         -> put_partition()
 *
 * NVMe 연결점:
 *   - state->disk는 NVMe 컨트롤러가 생성한 namespace 디스크이며,
 *     NVMe I/O 경로의 최상위 블록 디바이스를 나타낸다.
 *   - 등록된 각 파티션은 독립적인 bio 분할 대상이 되며, 각 bio가
 *     nvme_queue_rq()에서 별도 CID를 가진 NVMe 명령으로 변환된다.
 *   - 파티션 오프셋(state->parts[].from)은 파일 시스템이 본 LBA에
 *     더해져 실제 NVMe SLBA가 된다.
 */
int of_partition(struct parsed_partitions *state)
{
	struct device *ddev = disk_to_dev(state->disk);	/* NVMe namespace의 gendisk -> struct device; of_node 연결 지점 */
	struct device_node *np;
	int slot;					/* gendisk parts[] 인덱스; 1번부터 부 파티션 번호(/dev/nvme0nXpN 의 P 번호 대응)(추정) */

	/* NVMe namespace 디스크에 연결된 Device Tree 노드를 가져옴 */
	struct device_node *partitions_np = of_node_get(ddev->of_node);	/* refcount 증가; firmware가 NVMe 디스크에 바인딩한 DT 노드 획득 */

	/* fixed-partitions 노드가 없으면 파티션 등록 없이 0 반환 */
	if (!partitions_np ||
	    !of_device_is_compatible(partitions_np, "fixed-partitions"))	/* DT 호환성 불일치 시 파티션 없이 단일 NVMe 블록 디바이스로 사용 */
		return 0;	/* 파서가 인식하지 못함; check_partition()이 다음 파서(cmdline/efi 등)로 넘어감 */

	slot = 1;
	/* Validate parition offset and size */
	/* 모든 자식 파티션 노드의 오프셋/크기가 섹터 정렬되는지 사전 검증 */
	for_each_child_of_node(partitions_np, np) {	/* 각 DT 파티션 노드는 NVMe namespace 위의 독립 블록 디바이스 후보 */
		if (validate_of_partition(np, slot)) {	/* 정렬/크기 실패 시 NVMe I/O 경로의 잘못된 LBA 차단 */
			of_node_put(np);
			of_node_put(partitions_np);

			return -1;	/* 치명적 오류; check_partition() 전체 파티션 스캔 실패 처리 */
		}

		slot++;	/* 파티션 슬롯 증가; gendisk parts[] 인덱스 및 /dev/nvme0nXpN 의 P 번호 증가(추정) */
	}

	slot = 1;
	/* 검증된 파티션들을 parsed_partitions에 순차 등록 */
	for_each_child_of_node(partitions_np, np) {	/* 등록 루프; bio 분할 시 각 파티션이 별도 request_queue 논리적 대상이 됨 */
		/* state->limit을 초과하면 더 이상 등록할 수 없음 (gendisk 파티션 수 한계) */
		if (slot >= state->limit) {		/* DISK_MAX_PARTS 초과 시 이후 파티션 무시; NVMe namespace 당 파티션 수 제한 */
			of_node_put(np);
			break;
		}

		add_of_partition(state, slot, np);	/* gendisk part[slot]에 from/size/flags/volname 기록 -> NVMe SLBA 오프셋 확정 */

		slot++;
	}

	seq_buf_puts(&state->pp_buf, "\n");	/* 스캔 결과 줄바꿈; dmesg 등 커널 메시지 버퍼에 기록 */

	return 1;	/* 파티션 등록 성공; 이후 파일 시스템 I/O는 파티션 bio -> blk-mq -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 전달 */
}

/* NVMe 관점 핵심 요약 */
/*
 * - 이 파일은 NVMe 네임스페이스(gendisk)에 대한 DT 기반 파티션 테이블을
 *   구성하여, 이후 file system -> bio -> blk-mq -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell) 경로의 SLBA 기준을 잡아준다.
 * - validate_of_partition()에서 SECTOR_SIZE 정렬을 검증하므로 잘못된
 *   파티션 오프셋이 NVMe PRP/SGL 정렬이나 LBA 계산 오류로 이어지는 것을
 *   방지한다.
 * - add_of_partition()이 state->parts[]에 기록한 from/size는 NVMe
 *   Read/Write 명령의 시작 LBA(SLBA)와 길이(NLB) 산출의 기초가 된다.
 * - read-only 플래그는 향후 NVMe Write 경로에서의 쓰기 거부 정책과
 *   연동될 수 있다(추정).
 * - block/partitions/core.c의 check_part[] 배열에서 of_partition은
 *   cmdline_partition 다음, efi_partition 이전에 호출된다.
 */
