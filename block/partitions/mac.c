// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/mac.c
 *
 *  Code extracted from drivers/block/genhd.c
 *  Copyright (C) 1991-1998  Linus Torvalds
 *  Re-organised Feb 1998 Russell King
 *
 *  ========================================================================
 *  NVMe 관점 파일 요약
 *  ========================================================================
 *  이 파일은 NVMe SSD 뒤에 연결된 블록 장치의 0번 섹터 및 후속 섹터를 읽어
 *  MacOS 파티션 테이블(Apple Partition Map)을 해석하는 코드이다.
 *  블록 계층에서 파티션 인식 단계는 submit_bio -> blk_mq_submit_bio ->
 *  blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로를
 *  통해 NVMe 컨트롤러가 실제 LBA를 읽기 전, 소프트웨어적으로 디스크 레이아웃을
 *  구성하는 위치에 해당한다. NVMe 입장에서는 파티션 테이블 해석 자체는
 *  호스트 소프트웨어의 책임이며, 컨트롤러는 CID, SQ, CQ, PRP/SGL 기반의
 *  Read 명령으로 요청된 LBA 영역을 그저 반환한다.
 *  ========================================================================
 */

#include <linux/ctype.h> /* 문자열 처리용; NVMe I/O 경로와 직접 무관 */
#include "check.h" /* read_part_sector(), parsed_partitions, put_partition 선언; NVMe namespace 파싱 인프라 */
#include "mac.h" /* mac_partition(), mac_driver_desc, MAC_* 매직 정의; NVMe LBA 0/APM 엔트리 레이아웃 */

#ifdef CONFIG_PPC_PMAC /* PowerMac 부팅 관련; NVMe SQ/CQ/doorbell 경로와 무관 */
#include <asm/machdep.h>
extern void note_bootable_part(dev_t dev, int part, int goodness); /* PowerMac용 루트 파티션 알림; NVMe controller 무관 */
#endif

/*
 * Code to understand MacOS partition tables.
 */

#ifdef CONFIG_PPC_PMAC
/*
 * mac_fix_string()
 *   목적: MacOS 파티션 엔트리의 고정폭 문자열에서 우측 공백을 제거한다.
 *   호출 경로: mac_partition() 낸부, PowerMac 부팅 파티션 평가 시
 *   NVMe 연결: NVMe 입장에서는 의미 없는 메타데이터 가공이며, 실제 I/O는
 *             read_part_sector() -> submit_bio -> ... -> nvme_submit_cmd(doorbell)
 *             경로에서 이미 완료된 상태에서 호출된다 (추정).
 */
static inline void mac_fix_string(char *stg, int len) /* PowerMac 문자열 정리; NVMe DMA 완료 후 CPU가 버퍼를 가공 (추정) */
{
	int i;

	/* 문자열 우측 공백 제거; NVMe DMA 완료 후 CPU가 버퍼를 가공하는 단계 (추정) */
	for (i = len - 1; i >= 0 && stg[i] == ' '; i--)
		stg[i] = 0;	/* 우측 끝 공백을 '\0'로 덮어써서 문자열 종료 (NVMe queue의 데이터 버퍼와 무관) */
}
#endif

/*
 * mac_partition()
 *   목적: NVMe SSD로부터 읽어온 블록 0번 및 이어지는 섹터들을 해석하여
 *         MacOS Apple Partition Map을 찾고, 각 파티션의 시작 LBA와 크기를
 *         parsed_partitions 구조체에 등록한다.
 *   주요 호출 경로:
 *     rescan_partitions() -> check_partition() -> mac_partition()
 *   NVMe I/O 발생 경로 (파티션 테이블 읽기):
 *     read_part_sector() -> read_part_sector() 낸부 bio 할당/제출 ->
 *     submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *     nvme_queue_rq -> nvme_submit_cmd(doorbell, CID 부여, PRP/SGL 설정)
 *   반환값:
 *     1  : Mac 파티션 테이블을 인식하고 파티션 등록 완료
 *     0  : MacOS 디스크가 아님
 *     -1 : I/O 오류 또는 비정상적인 블록 크기
 */
int mac_partition(struct parsed_partitions *state) /* NVMe namespace 단위 파티션 스캔 진입점 */
{
	Sector sect;		/* read_part_sector()가 반환한 512바이트 섹터 버퍼 (NVMe에서 PRP/SGL로 채워진 데이터의 호스트 사본) */
	unsigned char *data;	/* sect 낸부의 실제 바이트 포인터, NVMe Read 완료 후 CPU가 해석하는 메모리 주소 */
	/* 파티션 맵 순회 인덱스(slot)와 총 엔트리 수; 각 slot마다 별도의 NVMe Read가 제출될 수 있음 */
	int slot, blocks_in_map;
	/* Mac 블록 크기(secsize)를 NVMe 512B LBA 단위로 환산하기 위한 변수들 */
	unsigned secsize, datasize, partoffset;
#ifdef CONFIG_PPC_PMAC
	int found_root = 0;		/* PowerMac 부팅에 적합한 루트 파티션 슬롯 (NVMe CID/SQ 상태와 무관) */
	int found_root_goodness = 0;	/* 루트 파티션 후보의 적합도 점수 */
#endif
	struct mac_partition *part;	/* NVMe에서 읽어온 파티션 엔트리를 해석할 때 사용하는 Apple 파티션 구조체 */
	struct mac_driver_desc *md;	/* 0번 블록에 위치한 Mac 드라이버 서술자 (NVMe LBA 0에서 읽음) */

	/* Get 0th block and look at the first partition map entry. */
	/* LBA 0을 NVMe에서 읽음: read_part_sector() -> submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(Read, SLBA=0, doorbell) */
	md = read_part_sector(state, 0, &sect);
	/* read_part_sector() 낸부 bio/mem 할당 실패 또는 NVMe CQE 오류(SQ full, LBA 초과 등) 시 NULL 반환 (추정) */
	if (!md)
		return -1;	/* NVMe Read 명령 실패 또는 메모리 부족 (CID 완료 전/후 오류 가능) */
	/* NVMe Read CQE가 성공했어도 매직 넘버 검증이 필요; MAC_DRIVER_MAGIC(0x4552) 불일치 시 Mac 디스크 아님 */
	if (be16_to_cpu(md->signature) != MAC_DRIVER_MAGIC) {
		/* LBA 0 버퍼 해제: NVMe DMA 완료된 folio 반납 */
		put_dev_sector(sect);
		return 0;	/* LBA 0에 Mac 드라이버 서술자가 없음 -> NVMe 입장에서는 일반 블록 데이터 */
	}
	/* md->block_size는 LBA 0 오프셋 2의 big-endian 16비트; NVMe Identify Namespace LBAF의 LBA Data Size와 대응 (추정) */
	secsize = be16_to_cpu(md->block_size);	/* Mac 논리 블록 크기; NVMe Read 단위(보통 512B)와 다를 수 있음 (추정) */
	/* 드라이버 서술자 파싱 후 버퍼 해제; 다음 read_part_sector() 전 필수 */
	put_dev_sector(sect);

	/*
	 * If the "block size" is not a power of 2, things get weird - we might
	 * end up with a partition straddling a sector boundary, so we wouldn't
	 * be able to read a partition entry with read_part_sector().
	 * Real block sizes are probably (?) powers of two, so just require
	 * that.
	 */
	/* NVMe LBA는 보통 512B 기준; secsize가 2의 거듭제곱이 아니면 read_part_sector()로 정렬된 APM 엔트리 읽기 불가 (추정) */
	if (!is_power_of_2(secsize))
		return -1;	/* NVMe LBA는 보통 512B 기준이므로 secsize가 2의 거듭제곱이 아니면 후속 read_part_sector()로 정렬 읽기 불가 (추정) */
	/* secsize를 NVMe 섹터(512B) 단위로 내림; secsize<512이면 datasize=0이 되어 후속 읽기가 실패할 수 있음 */
	datasize = round_down(secsize, 512);	/* NVMe가 반환하는 512B 섹터 단위로 내림 */
	/* datasize/512 = NVMe Read의 SLBA; LBA 0 또는 1에서 첫 Apple Partition Map 엔트리를 읽음 */
	data = read_part_sector(state, datasize / 512, &sect);
	/* NVMe CQE 오류, SQ full, 또는 메모리 부족으로 LBA datasize/512 읽기 실패 (추정) */
	if (!data)
		return -1;	/* NVMe Read 실패: SQ/CQ 완료 상태 비정상 또는 메모리 할당 실패 */
	/* secsize가 512B 배수가 아닐 때 섹터 내 APM 엔트리 시작 오프셋; 512B 배수면 0 */
	partoffset = secsize % 512;	/* secsize가 512보다 클 때 첫 번째 파티션 엔트리의 섹터 내 오프셋 */
	/* sizeof(*part)는 최소 184B; NVMe가 반환한 512B 버퍼 내에서 partoffset+sizeof(*part)가 벗어나면 손상 또는 잘못된 secsize */
	if (partoffset + sizeof(*part) > datasize) {
		/* 파티션 엔트리 범위 초과로 인한 조기 반환 전 버퍼 해제 */
		put_dev_sector(sect);
		return -1;	/* NVMe에서 읽은 버퍼 범위를 벗어나는 파티션 엔트리 (데이터 손상 또는 잘못된 secsize) */
	}
	/* NVMe DMA로 채워진 버퍼에서 partoffset만큼 건너 뛴 APM 엔트리 포인터 */
	part = (struct mac_partition *) (data + partoffset);
	/* LBA datasize/512의 CQE 데이터에서 MAC_PARTITION_MAGIC(0x504d) 검증; 불일치 시 Mac 포맷 아님 */
	if (be16_to_cpu(part->signature) != MAC_PARTITION_MAGIC) {
		/* APM 매직 불일치로 스캔 중단 전 버퍼 해제 */
		put_dev_sector(sect);
		return 0;	/* LBA 0 주변에 Apple Partition Map 시그니처가 없음 -> NVMe SSD는 Mac 포맷이 아님 */
	}
	/* map_count는 APM 엔트리 오프셋 4의 big-endian 32비트; NVMe LBA 0 또는 첫 엔트리에서 읽어낸 값 */
	blocks_in_map = be32_to_cpu(part->map_count);	/* Apple Partition Map의 총 엔트리 수; NVMe가 LBA 0에서 반환한 데이터의 하위 필드 */
	/* DISK_MAX_PARTS는 커널의 최대 파티션 수; NVMe namespace 용량과 무관하게 적용 */
	if (blocks_in_map < 0 || blocks_in_map >= DISK_MAX_PARTS) {
		/* 비정상적인 map_count로 스캔 중단 전 버퍼 해제 */
		put_dev_sector(sect);
		return 0;	/* 파티션 개수가 너무 많아 블록 계층이 관리할 수 없으므로 NVMe 입장에서는 단일 디스크로 취급 */
	}

	/* state->limit은 gendisk가 수용 가능한 최대 파티션 수; 초과 시 등록하지 않음 */
	if (blocks_in_map >= state->limit)
		blocks_in_map = state->limit - 1;	/* parsed_partitions 구조체의 한계를 NVMe LBA와 무관하게 맞춤 */

	/* 파티션 스캔 결과 문자열 버퍼에 " [mac]" 기록; 이후 dmesg 등에서 "nvme0n1: [mac] p1 p2 ..." 형태로 출력 */
	seq_buf_puts(&state->pp_buf, " [mac]");
	/* APM은 MBR 확장 파티션 체인(EBR)처럼 연결 리스트가 아닌 평면 구조이므로, NVMe Read 제출 횟수는 blocks_in_map에 선형 (추정) */
	for (slot = 1; slot <= blocks_in_map; ++slot) {
		/* 현재 파티션 엔트리의 Mac 논리 블록 오프셋; CHS 변환 없이 /512로 NVMe LBA로 직접 환산 */
		int pos = slot * secsize;	/* 현재 파티션 엔트리의 Mac 논리 블록 오프셋 */
		/* 이전 read_part_sector()로 얻은 folio 해제; NVMe DMA 버퍼 수명 종료 */
		put_dev_sector(sect);
		/* pos/512 = NVMe Read SLBA; APM은 CHS 대신 논리 블록 번호를 사용하므로 bio remap 시 NVMe SLBA로 직접 대응 */
		data = read_part_sector(state, pos/512, &sect);
		/* APM 엔트리 읽기 실패: NVMe CQE status가 error이거나, SQ/CQ/doorbell 단계에서 문제 발생 (추정) */
		if (!data)
			return -1;		/* NVMe Read 실패: SQ가 가득 찼거나, CID 완료에 오류가 포함됨 (추정) */
		/* NVMe가 반환한 512B 버퍼 내 pos%512 위치의 APM 엔트리; 이 위치는 NVMe PRP/SGL 버퍼의 바이트 오프셋 */
		part = (struct mac_partition *) (data + pos%512);
		/* 예상된 APM 시그니처(0x504d)가 아니면 중단; NVMe에서 읽은 데이터의 나머지는 무시됨 */
		if (be16_to_cpu(part->signature) != MAC_PARTITION_MAGIC)
			/* APM 시그니처 깨짐: 이 지점 이후의 NVMe Read는 불필요하므로 루프 탈출 */
			break;			/* 예상된 Apple Partition Map 시그니처가 아니면 중단; NVMe 데이터는 여기까지만 유효 */
		/* 파티션 슬롯(slot)을 parsed_partitions.parts[]에 등록 -> rescan_partitions 종료 후 add_gd_partition()을 거쳐 /dev/nvme0n1pN 형태의 독립 블록 디바이스 생성 (추정) */
		put_partition(state, slot,
			/* start_block은 APM 엔트리 오프셋 8의 be32; Mac 블록 단위를 NVMe 512B LBA 단위로 환산 -> bio remap 후 SLBA에 더해짐 */
			be32_to_cpu(part->start_block) * (secsize/512),	/* NVMe LBA = Mac 시작 블록 * (Mac 블록 크기 / 512B) */
			/* block_count는 APM 엔트리 오프셋 12의 be32; 파티션 크기를 NVMe 섹터 수로 환산 -> gendisk 파티션 크기 결정 */
			be32_to_cpu(part->block_count) * (secsize/512));	/* 해당 파티션의 NVMe 상에서의 총 섹터 수 */

		/* part->type[0..31]은 APM 엔트리 오프셋 16에 위치; NVMe에서 읽은 메타데이터 기반 OS 수준 플래그 설정 */
		if (!strncasecmp(part->type, "Linux_RAID", 10))
			/* RAID 파티션 플래그 기록; NVMe SQ/CQ 명령 형식에는 영향 없음 */
			state->parts[slot].flags = ADDPART_FLAG_RAID;	/* 파티션 플래그 설정; NVMe SQ/CQ에는 영향 없음 */
#ifdef CONFIG_PPC_PMAC
		/*
		 * If this is the first bootable partition, tell the
		 * setup code, in case it wants to make this the root.
		 */
		/* PowerMac 플랫폼에서만 실행; NVMe 컨트롤러/펌웨어와 무관한 부팅 정책 */
		if (machine_is(powermac)) {
			int goodness = 0;

			/* part->processor[0..15] 필드 정리; APM 엔트리 오프셋 112 (추정) */
			mac_fix_string(part->processor, 16);
			/* part->name[0..31] 필드 정리; APM 엔트리 오프셋 48 */
			mac_fix_string(part->name, 32);
			/* part->type[0..31] 필드 정리; APM 엔트리 오프셋 16 */
			mac_fix_string(part->type, 32);					
		    
			/* part->status는 APM 엔트리 오프셋 44의 be32; MAC_STATUS_BOOTABLE 비트 검사 */
			if ((be32_to_cpu(part->status) & MAC_STATUS_BOOTABLE)
			    && strcasecmp(part->processor, "powerpc") == 0)
				/* PowerPC 프로세서 매칭 시 부팅 적합도 증가; NVMe I/O 경로와 무관 */
				goodness++;

			/* part->type은 APM 엔트리 오프셋 16의 32바이트 문자열; Apple_UNIX_SVR2/Linux 등 판별 */
			if (strcasecmp(part->type, "Apple_UNIX_SVR2") == 0
			    || (strncasecmp(part->type, "Linux", 5) == 0
			        && strcasecmp(part->type, "Linux_swap") != 0)) {
				int i, l;

				/* Unix/Linux 데이터 파티션 확인 시 부팅 적합도 증가 */
				goodness++;
				/* 파티션 이름 길이 측정; APM 엔트리 오프셋 48의 name[32] 필드 */
				l = strnlen(part->name, sizeof(part->name));
				/* 이름이 "/"이면 루트 파티션으로 간주; NVMe LBA remap과 무관 */
				if (strncmp(part->name, "/", sizeof(part->name)) == 0)
					goodness++;
				/* 이름에서 "root" 서브스트링 검색; 순전히 OS 부팅 정책 (추정) */
				for (i = 0; i <= l - 4; ++i) {
					/* "root" 서브스트링 발견 시 부팅 적합도 크게 증가 */
					if (strncasecmp(part->name + i, "root",
						     4) == 0) {
						/* 루트 후보 가중치 부여; NVMe I/O 경로와 무관 */
						goodness += 2;
						break;
					}
				}
				/* 이름이 "swap"이면 적합도 감소; NVMe page cache/SWAP I/O 경로와 무관 */
				if (strncasecmp(part->name, "swap", 4) == 0)
					/* 스왑 파티션은 루트 후보에서 제외 */
					goodness--;
			}

			/* 현재까지 가장 부팅에 적합한 파티션 후보 갱신 */
			if (goodness > found_root_goodness) {
				/* 루트 파티션 슬롯 번호 기록; 이는 /dev/nvme0n1pN의 N과 대응 (추정) */
				found_root = slot;
				/* 현재 최고 부팅 적합도 점수 갱신 */
				found_root_goodness = goodness;
			}
		}
#endif /* CONFIG_PPC_PMAC */
	}
#ifdef CONFIG_PPC_PMAC
	/* found_root이 설정된 경우 PowerMac 부팅 코드에 통보; NVMe queue에는 영향 없음 */
	if (found_root_goodness)
		/* note_bootable_part는 PowerMac 고유; NVMe SQ/CQ doorbell과 무관 */
		note_bootable_part(state->disk->part0->bd_dev, found_root,
				   found_root_goodness);
#endif

	/* for 루프 종료 후 마지막 read_part_sector() folio 해제; 모든 NVMe Read 버퍼 반납 */
	put_dev_sector(sect);
	/* 파티션 목록 출력의 줄바꿈; NVMe I/O와 무관 */
	seq_buf_puts(&state->pp_buf, "\n");
	/* Mac 파티션 등록 완료; 이후 bio가 partition remap을 거쳐 submit_bio -> blk_mq_submit_bio -> nvme_queue_rq -> nvme_submit_cmd(SLBA = 파티션 시작 LBA + bi_sector)로 전달됨 */
	return 1;	/* Mac 파티션 등록 완료; 이후 blk_mq_submit_bio -> nvme_queue_rq 경로는 파티션 오프셋을 반영한 LBA로 변환됨 */
}

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 NVMe SSD의 LBA 0번 및 이후 섹터를 읽어 MacOS 파티션 맵을
 *   해석하며, 파티션별 시작/크기를 blk_mq가 이해하는 512B LBA 단위로 변환한다.
 *
 * - 파티션 테이블 읽기는 read_part_sector() 낸부 bio 제출을 통해
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell, CID, PRP/SGL) 경로로 NVMe 컨트롤러와 연결된다.
 *
 * - NVMe 컨트롤러는 파티션 개념을 모름; 컨트롤러는 단지 CID, SQ, CQ, doorbell
 *   기반의 Read/Write 명령으로 지정된 LBA를 처리할 뿐, 파티션 경계는 호스트
 *   블록 계층이 관리한다.
 *
 * - secsize, start_block, block_count 등의 Mac 전용 필드는 NVMe 물리
 *   레이아웃과 직접 대응하지 않으므로, 512B 단위로 정규화한 후에야 NVMe
 *   명령의 LBA 필드로 사용될 수 있다 (추정).
 *
 * - 다른 블록 계층 파일(block/partitions/check.c, block/partition-generic.c 등)과
 *   논리적으로 연결되어 있으며, rescan_partitions() -> check_partition() 형태로
 *   호출되어 NVMe 장치 초기화 시점에 파티션 정보를 구성한다.
 */
