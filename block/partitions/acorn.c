// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (c) 1996-2000 Russell King.
 *
 *  Scan ADFS partitions on hard disk drives.  Unfortunately, there
 *  isn't a standard for partitioning drives on Acorn machines, so
 *  every single manufacturer of SCSI and IDE cards created their own
 *  method.
 */

/*
 * 파일 상단 요약 (NVMe 관점)
 * ============================================================================
 * 이 파일은 Linux 커널 블록 계층(block layer)의 partition scanning
 * 하위 시스템에 위치하며, Acorn/ADFS 계열의 레거시 파티션 테이블을
 * 파싱하여 parsed_partitions 구조체에 등록한다.
 *
 * NVMe SSD 입장에서는 이 단계가 I/O 요청 경로의 맨 앞단에 해당한다:
 *   사용자 bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
 *   -> nvme_submit_cmd(doorbell)
 * 파티션 테이블이 올바르게 파싱되어야 bio의 시작 섹터가 올바른 LBA로
 * 매핑되고, 이 LBA는 이후 NVMe PRP/SGL 엔트리로 변환된다.
 * 따라서 이 파일은 NVMe SQ/CQ, CID, PRP/SGL보다 상위 계층에 있지만,
 * 잘못된 파싱은 하위 NVMe 명령의 시작 LBA를 오염시키는 원인이 된다.
 * ============================================================================
 */

#include <linux/buffer_head.h>  /* read_part_sector()가 호출하는 buffer_head 기반 I/O는 submit_bio() -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 NVMe Read 명령이 될 수 있음 (추정) */
#include <linux/adfs_fs.h>

#include "check.h"  /* parsed_partitions, read_part_sector, put_partition 등 파티션 프레임워크 선언; NVMe 드라이버와 직접 링크되지 않고 block layer를 통해 간접 연결 */

/*
 * Partition types. (Oh for reusability)
 *
 * NVMe 입장에서는 이 값들은 controller가 아닌, 커널의 파티션 파서가
 * 사용하는 식별자다. CID/SQ/CQ 등의 NVMe 명령어 필드와는 직접
 * 매핑되지 않는다. 단, 식별 결과에 따라 put_partition()으로 등록된
 * 섹터 범위가 이후 NVMe read/write 명령의 SLBA로 변환된다.
 */
#define PARTITION_RISCIX_MFM	1  /* RISCiX MFM 타입; NVMe namespace에는 전달되지 않는 커널 남아 식별자 (추정) */
#define PARTITION_RISCIX_SCSI	2  /* RISCiX SCSI 타입; NVMe namespace에는 전달되지 않는 커널 남아 식별자 (추정) */
#define PARTITION_LINUX		9  /* Acorn Linux 타입; NVMe namespace에는 전달되지 않는 커널 남아 식별자 (추정) */

#if defined(CONFIG_ACORN_PARTITION_CUMANA) || \
	defined(CONFIG_ACORN_PARTITION_ADFS)
/*
 * adfs_partition()
 * 목적:
 *   ADFS 디스크 레코드(disc record)를 검사하고, 유효하면
 *   put_partition()으로 파티션을 parsed_partitions에 등록한다.
 * 호출 경로 (추정):
 *   adfspart_check_CUMANA()/adfspart_check_ADFS()
 *   -> adfs_partition() -> put_partition()
 * NVMe 연결점:
 *   등록된 first_sector와 nr_sects는 이후 submit_bio -> blk_mq_submit_bio
 *   -> blk_mq_get_request -> nvme_queue_rq 경로에서 NVMe 명령의
 *   시작 LBA(SLBA)와 전송 길이로 번역된다. doorbell, CID, SQ, CQ,
 *   PRP/SGL 등은 아직 등장하지 않는다.
 */
static struct adfs_discrecord *
adfs_partition(struct parsed_partitions *state, char *name, char *data,  /* first_sector는 NVMe namespace 기준 시작 LBA로 변환됨 (추정) */
	       unsigned long first_sector, int slot)  /* slot은 커널 파티션 테이블 인덱스; NVMe queue depth와 무관 */
{
	struct adfs_discrecord *dr;  /* NVMe media에서 읽은 섹터 내 ADFS disc record 포인터 */
	unsigned int nr_sects;  /* 파티션 섹터 수; NVMe Read/Write 길이 계산의 기초 */

	/* ADFS boot block 검사: 실패하면 NULL 반환, 상위에서 탐색 종료 */
	if (adfs_checkbblk(data))  /* boot block magic/checksum 검증 실패 시 잘못된 NVMe LBA 범위 차단 */
		return NULL;

	/* 0x1c0 오프셋에 ADFS disc record가 위치함 (추정: 고정 레이아웃) */
	dr = (struct adfs_discrecord *)(data + 0x1c0);  /* NVMe Read로 얻은 버퍼에서 on-disk metadata(0x1c0 offset) 추출 */

	/* disc_size가 0이면 유효한 파티션이 아님 */
	if (dr->disc_size == 0 && dr->disc_size_high == 0)  /* 빈 파티션은 NVMe I/O target으로 등록하지 않음 */
		return NULL;

	/*
	 * 섹터 수 계산: 상위 워드와 하위 워드를 결합 (리틀엔디안).
	 * 이 값이 NVMe I/O의 논리 블록 주소 범위로 변환된다.
	 */
	nr_sects = (le32_to_cpu(dr->disc_size_high) << 23) |  /* high word -> byte 단위 상위 -> 섹터 수 */
		   (le32_to_cpu(dr->disc_size) >> 9);  /* low word -> byte -> 섹터 수; 이 범위가 NVMe SLBA/길이로 번역됨 */

	if (name) {  /* 파티션 이름은 진단 출력용; NVMe NS label과 무관 (추정) */
		seq_buf_printf(&state->pp_buf, " [%s]", name);  /* diagnostic 버퍼에 이름 기록; NVMe controller에는 전달되지 않음 */
	}
	put_partition(state, slot, first_sector, nr_sects);  /* block layer에 partition 등록 -> 이후 bio가 NVMe namespace 위 block device를 타겟 */
	return dr;  /* 파서 다음 단계(예: CUMANA 체인)에서 추가 NVMe LBA 누적에 사용 */
}
#endif

#ifdef CONFIG_ACORN_PARTITION_RISCIX

/*
 * struct riscix_part
 * 각 필드는 RISCiX 파티션 엔트리 하나를 기술한다.
 *   start:  파티션 시작 섹터 (LE32). 이 값이 이후 NVMe read/write
 *           명령의 SLBA에 더해진다.
 *   length: 파티션 섹터 수 (LE32). NVMe 명령의 길이 필드로
 *           변환될 수 있는 범위다.
 *   one:    활성화 플래그(추정). 0이면 무시한다.
 *   name:   파티션 이름. NVMe controller 레벨의 NS 레이블과는
 *           무관하며, 커널의 진단용 출력에 사용된다.
 */
struct riscix_part {  /* RISCiX 파티션 엔트리; 각 필드는 NVMe LBA 범위 후보 */
	__le32	start;  /* 파티션 상대 시작 섹터(LE32); NVMe SLBA 후보 */
	__le32	length;  /* 파티션 섹터 수(LE32); NVMe 명령 길이 후보 */
	__le32	one;  /* 활성화 플래그(추정) */
	char	name[16];  /* 파티션 이름; NVMe NS label과 무관 */
};

/*
 * struct riscix_record
 *   magic: RISCIX_MAGIC(0x4a657320)로 포맷 식별.
 *   date:  포맷 시점(추정). NVMe SMART 로그의 timestamp와는 무관.
 *   part:  최대 8개의 riscix_part 엔트리 배열.
 *          각각의 start/length가 NVMe LBA 범위의 후보가 된다.
 */
struct riscix_record {
	__le32	magic;
#define RISCIX_MAGIC	cpu_to_le32(0x4a657320)  /* RISCIX_MAGIC 문자열 기반 magic; NVMe media signature */
	__le32	date;
	struct riscix_part part[8];
};

#if defined(CONFIG_ACORN_PARTITION_CUMANA) || \
	defined(CONFIG_ACORN_PARTITION_ADFS)
/*
 * riscix_partition()
 * 목적:
 *   RISCiX 형식의 파티션 테이블을 읽어 하위 파티션들을 등록한다.
 * 호출 경로 (추정):
 *   adfspart_check_CUMANA()/adfspart_check_ADFS()
 *   -> riscix_partition() -> put_partition()
 * NVMe 연결점:
 *   이 함수가 등록하는 섹터 범위는 bio 변환 시 NVMe PRP/SGL이
 *   가리킬 물리 메모리 페이지와 함께 LBA로 해석된다.
 */
static int riscix_partition(struct parsed_partitions *state,  /* first_sect/nr_sects는 NVMe namespace 내 LBA 범위 후보 */
			    unsigned long first_sect, int slot,  /* slot은 커널 파티션 테이블 인덱스; NVMe queue depth와 무관 */
			    unsigned long nr_sects)  /* nr_sects는 NVMe Read/Write 길이 후보 */
{
	Sector sect;  /* read_part_sector()가 채울 단일 섹터 버퍼; NVMe Read 1 sector의 결과물 */
	struct riscix_record *rr;  /* RISCiX partition table 포인터; NVMe Read 버퍼 상위 */
	
	rr = read_part_sector(state, first_sect, &sect);  /* 1 sector read -> submit_bio -> blk_mq -> nvme_queue_rq -> NVMe Read command (SLBA=first_sect) (추정) */
	if (!rr)  /* 메모리 할당 또는 NVMe read 실패: NVMe CQE status 오류일 수 있음 (추정) */
		return -1;

	seq_buf_puts(&state->pp_buf, " [RISCiX]");  /* 포맷 식별 문자열; NVMe controller에는 전달되지 않음 */


	if (rr->magic == RISCIX_MAGIC) {  /* signature OK -> partition layout 유효; NVMe media에서 읽은 metadata 신뢰 */
		unsigned long size = nr_sects > 2 ? 2 : nr_sects;  /* boot 영역 크기 제한; NVMe 전송 길이 후보 */
		int part;

		seq_buf_puts(&state->pp_buf, " <");

		/* 루트 슬롯을 우선 2섹터로 등록 (추정: RISCiX 부트 영역) */
		put_partition(state, slot++, first_sect, size);  /* root slot 등록 -> NVMe namespace LBA first_sect부터 size만큼 */
		for (part = 0; part < 8; part++) {  /* 최대 8개 엔트리; 각각의 start/length가 NVMe LBA 범위로 매핑 */
			/* one이 0이 아니고 이름이 "All"이 아닌 엔트리만 등록 */
			if (rr->part[part].one &&  /* 활성 엔트리 필터링; 잘못 등록 시 NVMe namespace 범위 이탈 가능 */
			    memcmp(rr->part[part].name, "All\0", 4)) {
				put_partition(state, slot++,  /* LE32 -> NVMe namespace 절대 SLBA */
					le32_to_cpu(rr->part[part].start),
					le32_to_cpu(rr->part[part].length));  /* LE32 -> NVMe Read/Write 길이 후보 */
				seq_buf_printf(&state->pp_buf, "(%s)", rr->part[part].name);  /* 이름은 diagnostic 출력용; NVMe NS label과 무관 */
			}
		}

		seq_buf_puts(&state->pp_buf, " >\n");
	} else {
		put_partition(state, slot++, first_sect, nr_sects);  /* signature 불일치 시 전체 범위 등록; NVMe Read로 읽은 sector가 RISCiX table이 아님 */
	}

	put_dev_sector(sect);  /* 섹터 버퍼 해제; NVMe Read 완료 후 buffer_head 정리 */
	return slot;  /* 등록된 slot 수 반환; NVMe와 무관 */
}
#endif
#endif

#define LINUX_NATIVE_MAGIC 0xdeafa1de  /* Acorn Linux native magic; NVMe media의 signature */
#define LINUX_SWAP_MAGIC   0xdeafab1e  /* Acorn Linux swap magic; NVMe media의 signature */

/*
 * struct linux_part
 * Acorn 리눅스 파티션 엔트리.
 *   magic:     LINUX_NATIVE_MAGIC 또는 LINUX_SWAP_MAGIC.
 *   start_sect: 파티션 내 상대 시작 섹터(LE32). NVMe LBA 계산의
 *               기본값이 된다.
 *   nr_sects:   파티션 섹터 수(LE32). NVMe 명령 범위 후보.
 */
struct linux_part {
	__le32 magic;  /* LINUX_NATIVE_MAGIC/SWAP_MAGIC; NVMe media signature */
	__le32 start_sect;  /* 파티션 내 상대 시작 섹터(LE32); NVMe LBA 계산 기초 */
	__le32 nr_sects;  /* 파티션 섹터 수(LE32); NVMe 명령 범위 후보 */
};

#if defined(CONFIG_ACORN_PARTITION_CUMANA) || \
	defined(CONFIG_ACORN_PARTITION_ADFS)
/*
 * linux_partition()
 * 목적:
 *   Acorn Linux 파티션 체인을 순회하며 put_partition()으로 등록.
 * 호출 경로 (추정):
 *   adfspart_check_CUMANA()/adfspart_check_ADFS()
 *   -> linux_partition() -> put_partition()
 * NVMe 연결점:
 *   magic 값이 일치하는 동안 반복 등록. 이 논리는 NVMe controller의
 *   namespace capacity를 초과하지 않는지를 커널이 검증하는 단계와
 *   연결된다(추정).
 */
static int linux_partition(struct parsed_partitions *state,  /* first_sect/nr_sects는 NVMe namespace 내 LBA 범위 후보 */
			   unsigned long first_sect, int slot,  /* slot은 커널 파티션 테이블 인덱스; NVMe queue depth와 무관 */
			   unsigned long nr_sects)  /* nr_sects는 NVMe Read/Write 길이 후보 */
{
	Sector sect;  /* read_part_sector()가 채울 단일 섹터 버퍼 */
	struct linux_part *linuxp;  /* Acorn Linux partition chain 포인터; NVMe Read 버퍼 상위 */
	unsigned long size = nr_sects > 2 ? 2 : nr_sects;  /* boot sector 영역 크기; NVMe 전송 길이 후보 */

	seq_buf_puts(&state->pp_buf, " [Linux]");
  /* 포맷 식별 문자열; NVMe controller에는 전달되지 않음 */
	put_partition(state, slot++, first_sect, size);  /* boot slot 등록 -> NVMe namespace LBA first_sect부터 size만큼 */

	linuxp = read_part_sector(state, first_sect, &sect);  /* NVMe Read command로 파티션 엔트리 섹터 획득 (SLBA=first_sect) (추정) */
	if (!linuxp)  /* 메모리 할당 또는 NVMe read 실패: NVMe CQE status 오류일 수 있음 (추정) */
		return -1;

	seq_buf_puts(&state->pp_buf, " <");
	while (linuxp->magic == cpu_to_le32(LINUX_NATIVE_MAGIC) ||  /* magic chain 순회; 각 엔트리마다 NVMe LBA 범위 등록 */
	       linuxp->magic == cpu_to_le32(LINUX_SWAP_MAGIC)) {  /* SWAP magic도 허용; NVMe media signature 비교 */
		/* 파티션 슬롯 상한 도달 시 중단: NVMe namespace가 아닌
		 * 커널 파티션 테이블의 한계다.
		 */
		if (slot == state->limit)  /* 커널 파티션 상한; NVMe queue depth와 무관 */
			break;
		put_partition(state, slot++, first_sect +  /* 상대 LBA + 파티션 기준 = NVMe namespace 내 절대 SLBA */
				 le32_to_cpu(linuxp->start_sect),
				 le32_to_cpu(linuxp->nr_sects));  /* LE32 -> NVMe 전송 길이 후보 */
		linuxp ++;  /* 다음 on-disk 엔트리; sizeof(struct linux_part)=12바이트 전진 */
	}
	seq_buf_puts(&state->pp_buf, " >");

	put_dev_sector(sect);  /* 섹터 버퍼 해제; NVMe Read 완료 후 buffer_head 정리 */
	return slot;  /* 등록된 slot 수 반환; NVMe와 무관 */
}
#endif

#ifdef CONFIG_ACORN_PARTITION_CUMANA
/*
 * adfspart_check_CUMANA()
 * 목적:
 *   Cumana/ADFS 방식의 연결형 파티션 테이블을 탐색.
 * 호출 경로:
 *   check_partition() 계열 (block/partitions/check.c 등)에서
 *   파티션 스킴 후보로 호출 (추정).
 *   -> adfspart_check_CUMANA() -> adfs_partition()
 *      -> riscix_partition()/linux_partition() -> put_partition()
 * NVMe 연결점:
 *   첫 번째 파티션부터 다음 파티션 포인터를 따라가며 섹터 범위를
 *   누적한다. 누적된 first_sector가 NVMe I/O의 기준 LBA가 된다.
 */
int adfspart_check_CUMANA(struct parsed_partitions *state)  /* NVMe namespace 상위 block device의 파티션 scheme 후보 */
{
	unsigned long first_sector = 0;  /* NVMe namespace LBA 0부터 누적 시작 (추정) */
	unsigned int start_blk = 0;  /* 블록 단위 인덱스; sector<<9 변환 시 NVMe LBA 정렬 고려 */
	Sector sect;  /* 현재 파티션 테이블 섹터 버퍼; NVMe Read 결과물 */
	unsigned char *data;  /* NVMe media에서 읽은 원시 섹터 데이터 */
	char *name = "CUMANA/ADFS";  /* diagnostic 이름; NVMe controller에는 전달되지 않음 */
	int first = 1;  /* 첫 번째 파티션 여부 */
	int slot = 1;  /* 커널 파티션 테이블 인덱스; NVMe queue depth와 무관 */

	/*
	 * Try Cumana style partitions - sector 6 contains ADFS boot block
	 * with pointer to next 'drive'.
	 *
	 * There are unknowns in this code - is the 'cylinder number' of the
	 * next partition relative to the start of this one - I'm assuming
	 * it is.
	 *
	 * Also, which ID did Cumana use?
	 *
	 * This is totally unfinished, and will require more work to get it
	 * going. Hence it is totally untested.
	 */
	do {
		struct adfs_discrecord *dr;  /* ADFS disc record 포인터; NVMe Read 버퍼 상위 */
		unsigned int nr_sects;  /* 파티션 섹터 수; NVMe LBA offset increment */

		/* 섹터 6 + (start_blk * 2)에서 ADFS 부트 블록 읽기 */
		data = read_part_sector(state, start_blk * 2 + 6, &sect);  /* NVMe Read: LBA = start_blk*2+6 */
		if (!data)  /* NVMe read 실패 또는 메모리 할당 실패: partition scan abort */
			return -1;

		if (slot == state->limit)  /* 커널 파티션 테이블 슬롯 상한; NVMe queue depth와 무관 */
			break;

		dr = adfs_partition(state, name, data, first_sector, slot++);  /* parse + put_partition -> block dev 등록 */
		if (!dr)  /* ADFS boot block이 아니면 연결 체인 종료; 추가 NVMe Read 불필요 */
			break;

		name = NULL;  /* 이후 엔트리는 진단 이름 출력 중복 방지; NVMe와 무관 */

		/*
		 * 다음 파티션의 섹터 수를 CHS 흔적으로 계산 (추정).
		 * 이 값이 누적되어 NVMe에서 볼 때의 연속/불연속 LBA 범위를
		 * 결정한다.
		 */
		nr_sects = (data[0x1fd] + (data[0x1fe] << 8)) *  /* cylinder count (LE16); CHS -> NVMe LBA 변환의 잔재 */
			   (dr->heads + (dr->lowsector & 0x40 ? 1 : 0)) *  /* head count; NVMe LBA 계산용 CHS 잔재 */
			   dr->secspertrack;  /* track당 섹터 수; NVMe LBA 정렬 단위 (추정) */

		if (!nr_sects)  /* 크기 0이면 연결 체인 종료; 추가 NVMe Read 불필요 */
			break;

		first = 0;  /* 첫 번째 파티션 처리 완료 */
		first_sector += nr_sects;  /* NVMe namespace 내 다음 파티션 시작 LBA 누적 */
		start_blk += nr_sects >> (BLOCK_SIZE_BITS - 9);  /* BLOCK_SIZE_BITS=10이면 /2; 블록 단위 -> 섹터 단위 NVMe LBA 변환 */
		nr_sects = 0; /* hmm - should be partition size */  /* 실제 파티션 크기 불명; NVMe 전송 길이 후보 0, 하위 parser가 덮어씀 (추정) */

		switch (data[0x1fc] & 15) {  /* partition type -> 하위 parser 선택; 각 parser는 추가 NVMe Read 명령 유발 */
		case 0: /* No partition / ADFS? */
			break;

#ifdef CONFIG_ACORN_PARTITION_RISCIX
		case PARTITION_RISCIX_SCSI:  /* RISCiX SCSI type */
			/* RISCiX - we don't know how to find the next one. */  /* 연장 파티션 탐색: 추가 NVMe Read command submission 발생 */
			slot = riscix_partition(state, first_sector, slot,
						nr_sects);
			break;
#endif

		case PARTITION_LINUX:
			slot = linux_partition(state, first_sector, slot,  /* Linux partition type */
					       nr_sects);  /* Linux partition chain 탐색: 추가 NVMe Read command submission 발생 */
			break;
		}
		put_dev_sector(sect);  /* 현재 sector 버퍼 해제 */
		if (slot == -1)  /* 하위 parser 실패 시 NVMe 경로로 잘못된 파티션 전달 방지 */
			return -1;
	} while (1);
	put_dev_sector(sect);  /* do-while 탈출 시점의 마지막 버퍼 해제 */
	return first ? 0 : 1;  /* 파티션을 찾았으면 1, 아니면 0; NVMe와 무관 */
}
#endif

#ifdef CONFIG_ACORN_PARTITION_ADFS
/*
 * adfspart_check_ADFS()
 * 목적:
 *   ADFS 단일/이중 파티션 형식을 탐색하고, 비-ADFS 영역에 대해
 *   RISCiX/Linux 파티션을 추가 탐색.
 * 호출 경로 (추정):
 *   check_partition() -> adfspart_check_ADFS()
 *   -> adfs_partition() -> riscix_partition()/linux_partition()
 * NVMe 연결점:
 *   get_capacity(state->disk)는 NVMe namespace capacity를 반영한다.
 *   이 값에서 ADFS 영역을 뺀 나머지가 NVMe I/O 가능한 LBA 범위가
 *   된다(추정).
 */
int adfspart_check_ADFS(struct parsed_partitions *state)  /* NVMe namespace 상위 block device의 ADFS scheme 후보 */
{
	unsigned long start_sect, nr_sects, sectscyl, heads;  /* CHS -> NVMe LBA 변환용 임시 변수들 */
	Sector sect;  /* 현재 파티션 테이블 섹터 버퍼 */
	unsigned char *data;  /* NVMe media에서 읽은 원시 섹터 데이터 */
	struct adfs_discrecord *dr;  /* ADFS disc record 포인터 */
	unsigned char id;  /* partition type ID; NVMe controller에는 전달되지 않음 */
	int slot = 1;  /* 커널 파티션 테이블 인덱스 */

	/* 섹터 6에서 ADFS 부트 블록 읽기 */
	data = read_part_sector(state, 6, &sect);  /* NVMe Read LBA 6 */
	if (!data)  /* NVMe read/allocation 실패: scan abort */
		return -1;

	dr = adfs_partition(state, "ADFS", data, 0, slot++);  /* LBA 0 기준 ADFS 파티션 등록 */
	if (!dr) {  /* ADFS가 아니면 sector 해제 후 0 반환; NVMe Read 결과 폐기 */
		put_dev_sector(sect);  /* buffer 해제; NVMe Read 결과 폐기 */
    		return 0;  /* scan 종료; NVMe와 무관 */
	}

	/* CHS 잔재로 heads/cylinder당 섹터 수 계산 (추정) */
	heads = dr->heads + ((dr->lowsector >> 6) & 1);  /* head count; NVMe LBA 계산용 CHS 잔재 */
	sectscyl = dr->secspertrack * heads;  /* cylinder당 섹터 수; NVMe LBA 정렬 단위 */
	start_sect = ((data[0x1fe] << 8) + data[0x1fd]) * sectscyl;  /* cylinder number -> NVMe namespace 내 바이트/섹터 offset */
	id = data[0x1fc] & 15;  /* partition type ID; NVMe controller에는 전달되지 않음 */
	put_dev_sector(sect);  /* 섹터 버퍼 해제 */

	/*
	 * Work out start of non-adfs partition.
	 */
	/* NVMe namespace 전체 용량에서 비-ADFS 영역 크기 산출 (추정) */
	nr_sects = get_capacity(state->disk) - start_sect;  /* get_capacity()는 NVMe namespace capacity 반영 (추정) */

	if (start_sect) {  /* non-ADFS 영역 존재 시 추가 파티션 탐색; 추가 NVMe Read commands 발생 */
		switch (id) {
#ifdef CONFIG_ACORN_PARTITION_RISCIX
		case PARTITION_RISCIX_SCSI:
		case PARTITION_RISCIX_MFM:
			riscix_partition(state, start_sect, slot,  /* RISCiX MFM/SCSI 추가 파티션 등록 */
						nr_sects);  /* NVMe namespace 내 start_sect부터 추가 파티션 등록 */
			break;
#endif

		case PARTITION_LINUX:
			linux_partition(state, start_sect, slot,  /* Linux 추가 파티션 등록 */
					       nr_sects);  /* NVMe namespace 내 start_sect부터 Linux chain 등록 */
			break;
		}
	}
	seq_buf_puts(&state->pp_buf, "\n");  /* diagnostic newline; NVMe와 무관 */
	return 1;  /* ADFS scheme 인식 성공 */
}
#endif

#ifdef CONFIG_ACORN_PARTITION_ICS

/*
 * struct ics_part
 * ICS 파티션 엔트리.
 *   start: 파티션 시작 섹터(LE32). NVMe SLBA 후보.
 *   size:  부호 있는 섹터 수(LE32). 음수면 ADFS가 아님을 표시하는
 *          플래그(추정). 절댓값이 NVMe 명령 범위 후보.
 */
struct ics_part {  /* ICS 파티션 엔트리; 각 필드는 NVMe LBA 범위 후보 */
	__le32 start;  /* 파티션 시작 섹터(LE32); NVMe SLBA 후보 */
	__le32 size;  /* 부호 있는 섹터 수(LE32); 절댓값이 NVMe 전송 길이 후보, 음수면 ADFS 아님 표시 (추정) */
};

/*
 * adfspart_check_ICSLinux()
 * 목적:
 *   지정 섹터가 "LinuxPart" 시그니처를 갖는지 확인.
 * 호출 경로 (추정):
 *   adfspart_check_ICS() -> adfspart_check_ICSLinux()
 * NVMe 연결점:
 *   커널이 한 섹터를 읽어 signature를 검사. 이 read는
 *   submit_bio -> blk_mq_submit_bio -> ... -> nvme_queue_rq 경로로
 *   NVMe read 명령이 될 수 있다.
 */
static int adfspart_check_ICSLinux(struct parsed_partitions *state,  /* state->disk는 NVMe namespace block device (추정) */
				   unsigned long block)  /* 검사할 NVMe namespace LBA */
{
	Sector sect;
	unsigned char *data = read_part_sector(state, block, &sect);  /* NVMe Read LBA=block; signature sector */
	int result = 0;

	if (data) {  /* null이면 NVMe read/allocation 실패 */
		if (memcmp(data, "LinuxPart", 9) == 0)  /* NVMe media에서 읽은 signature 비교 */
			result = 1;
		put_dev_sector(sect);  /* 버퍼 해제; NVMe Read 결과 폐기 */
	}

	return result;
}

/*
 * valid_ics_sector()
 * 목적:
 *   ICS 파티션 섹터의 체크섬을 검증.
 * 호출 경로 (추정):
 *   adfspart_check_ICS() -> valid_ics_sector()
 * NVMe 연결점:
 *   파티션 테이블 자체의 무결성을 SW적으로 검증. NVMe CRC/PI
 *   (Protection Information)와는 다른 계층의 검증이다.
 */
static inline int valid_ics_sector(const unsigned char *data)  /* ICS partition table checksum 검증; NVMe PI/CRC와 독립적 */
{
	unsigned long sum;
	int i;

	/* 초기 시드 0x50617274("Part")로 508바이트 체크섬 계산 (추정) */
	for (i = 0, sum = 0x50617274; i < 508; i++)  /* 508 bytes; NVMe PI/CRC와 독립적인 SW 무결성 검증 */
		sum += data[i];

	sum -= le32_to_cpu(*(__le32 *)(&data[508]));  /* stored checksum과 비교; NVMe CQE status와는 다른 데이터 무결성 검증 */

	return sum == 0;
}

/*
 * adfspart_check_ICS()
 * 목적:
 *   ICS 방식 파티션 테이블(섹터 0)을 파싱.
 * 호출 경로 (추정):
 *   check_partition() -> adfspart_check_ICS()
 *   -> valid_ics_sector() -> put_partition()
 * NVMe 연결점:
 *   섹터 0에 기술된 start/size가 NVMe namespace 내 I/O 가능한
 *   LBA 범위가 된다. size가 음수일 때의 start + 1 보정은 커널
 *   내에서만 유효하며 NVMe controller가 인식하지 않는다.
 */
int adfspart_check_ICS(struct parsed_partitions *state)  /* NVMe namespace 상위 block device의 ICS scheme 후보 */
{
	const unsigned char *data;
	const struct ics_part *p;
	int slot;
	Sector sect;

	/*
	 * Try ICS style partitions - sector 0 contains partition info.
	 */
	data = read_part_sector(state, 0, &sect);  /* NVMe Read LBA 0 (partition table sector) */
	if (!data)  /* NVMe read/allocation 실패 */
    		return -1;

	if (!valid_ics_sector(data)) {  /* checksum fail -> 잘못된 partition layout이 NVMe I/O 경로로 전달되는 것 차단 */
    		put_dev_sector(sect);  /* buffer 해제 */
		return 0;  /* scheme 불일치; NVMe와 무관 */
	}

	seq_buf_puts(&state->pp_buf, " [ICS]");

	for (slot = 1, p = (const struct ics_part *)data; p->size; p++) {  /* on-disk entries; each put_partition maps NVMe LBA range */
		u32 start = le32_to_cpu(p->start);  /* LE32 -> NVMe namespace 절대 SLBA */
		s32 size = le32_to_cpu(p->size); /* yes, it's signed. */  /* signed; 부호에 따라 ADFS 여부/보정 처리 */

		if (slot == state->limit)  /* 커널 파티션 슬롯 상한; NVMe queue depth와 무관 */
			break;

		/*
		 * Negative sizes tell the RISC OS ICS driver to ignore
		 * this partition - in effect it says that this does not
		 * contain an ADFS filesystem.
		 */
		if (size < 0) {  /* 음수면 ADFS가 아닌 영역; NVMe 전송 길이로 절댓값 사용 */
			size = -size;  /* NVMe 전송 길이로 사용할 절댓값 */

			/*
			 * Our own extension - We use the first sector
			 * of the partition to identify what type this
			 * partition is.  We must not make this visible
			 * to the filesystem.
			 */
			if (size > 1 && adfspart_check_ICSLinux(state, start)) {  /* 추가 NVMe Read for LinuxPart signature */
				start += 1;  /* NVMe LBA 보정: signature sector 제외 */
				size -= 1;  /* NVMe 전송 길이 보정 */
			}
		}

		if (size)
			put_partition(state, slot++, start, size);  /* block device 등록 -> NVMe namespace LBA start부터 size만큼 */
	}

	put_dev_sector(sect);
	seq_buf_puts(&state->pp_buf, "\n");
	return 1;
}
#endif

#ifdef CONFIG_ACORN_PARTITION_POWERTEC
/*
 * struct ptec_part
 * PowerTec 파티션 엔트리.
 *   unused1/2/5: 사용되지 않는 패딩(추정).
 *   start:       파티션 시작 섹터(LE32). NVMe SLBA 후보.
 *   size:        파티션 섹터 수(LE32). NVMe 명령 범위 후보.
 *   type:        파티션 타입 문자열. NVMe namespace 레이블과 무관.
 */
struct ptec_part {
	__le32 unused1;  /* 패딩(추정); NVMe LBA와 무관 */
	__le32 unused2;  /* 패딩(추정); NVMe LBA와 무관 */
	__le32 start;  /* 파티션 시작 섹터(LE32); NVMe SLBA 후보 */
	__le32 size;  /* 파티션 섹터 수(LE32); NVMe 전송 길이 후보 */
	__le32 unused5;  /* 패딩(추정); NVMe LBA와 무관 */
	char type[8];  /* 파티션 타입 문자열; NVMe namespace label과 무관 */
};

/*
 * valid_ptec_sector()
 * 목적:
 *   PowerTec 파티션 섹터의 체크섬을 검증하고 PC MBR signature와
 *   구분.
 * 호출 경로 (추정):
 *   adfspart_check_POWERTEC() -> valid_ptec_sector()
 * NVMe 연결점:
 *   체크섬 검증이 실패하면 NVMe 경로로 전달될 I/O가 잘못된
 *   파티션 범위에서 발생할 위험을 차단한다.
 */
static inline int valid_ptec_sector(const unsigned char *data)  /* PowerTec partition table checksum/signature 검증 */
{
	unsigned char checksum = 0x2a;
	int i;

	/*
	 * If it looks like a PC/BIOS partition, then it
	 * probably isn't PowerTec.
	 */
	/* PC MBR boot signature(0x55 0xaa)가 있으면 PowerTec 아님 */
	if (data[510] == 0x55 && data[511] == 0xaa)  /* MBR signature 감지; 잘못된 scheme으로 NVMe LBA mapping 방지 */
		return 0;

	for (i = 0; i < 511; i++)  /* 511 bytes checksum; NVMe PI와 독립적 SW 무결성 */
		checksum += data[i];

	return checksum == data[511];
}

/*
 * adfspart_check_POWERTEC()
 * 목적:
 *   PowerTec 방식 파티션 테이블을 파싱.
 * 호출 경로 (추정):
 *   check_partition() -> adfspart_check_POWERTEC()
 *   -> valid_ptec_sector() -> put_partition()
 * NVMe 연결점:
 *   최대 12개의 파티션을 등록. 각 start/size는 NVMe I/O의
 *   논리 블록 범위로 번역된다.
 */
int adfspart_check_POWERTEC(struct parsed_partitions *state)  /* NVMe namespace 상위 block device의 PowerTec scheme 후보 */
{
	Sector sect;
	const unsigned char *data;
	const struct ptec_part *p;
	int slot = 1;
	int i;

	data = read_part_sector(state, 0, &sect);  /* NVMe Read LBA 0 (PowerTec partition table) */
	if (!data)  /* NVMe read/allocation 실패 */
		return -1;

	if (!valid_ptec_sector(data)) {  /* checksum/signature mismatch -> NVMe 경로로 잘못된 파티션 전달 차단 */
		put_dev_sector(sect);  /* buffer 해제 */
		return 0;  /* scheme 불일치; NVMe와 무관 */
	}

	seq_buf_puts(&state->pp_buf, " [POWERTEC]");

	for (i = 0, p = (const struct ptec_part *)data; i < 12; i++, p++) {  /* 최대 12개; 각각 NVMe LBA 범위 등록 */
		u32 start = le32_to_cpu(p->start);  /* LE32 -> NVMe SLBA */
		u32 size = le32_to_cpu(p->size);  /* LE32 -> NVMe 전송 길이 후보 */

		if (size)
			put_partition(state, slot++, start, size);  /* block dev -> NVMe namespace LBA start~start+size */
	}

	put_dev_sector(sect);
	seq_buf_puts(&state->pp_buf, "\n");
	return 1;
}
#endif

#ifdef CONFIG_ACORN_PARTITION_EESOX
/*
 * struct eesox_part
 * EESOX SCSI 파티션 엔트리.
 *   magic: "Eesox" 시그니처. NVMe와 무관한 포맷 식별자.
 *   name:  파티션 이름. NVMe namespace 레이블과 무관.
 *   start: 다음 파티션의 시작 섹터(LE32). 이전 start와의 차이가
 *          현재 파티션의 크기로 사용된다(추정).
 *   unused6/7/8: 패딩(추정).
 */
struct eesox_part {
	char	magic[6];  /* "Eesox" signature; NVMe media에서 읽은 문자열 */
	char	name[10];  /* 파티션 이름; NVMe namespace label과 무관 */
	__le32	start;  /* 다음 파티션 시작 섹터(LE32); 이전 start와의 차이가 현재 크기(추정) */
	__le32	unused6;  /* 패딩(추정) */
	__le32	unused7;  /* 패딩(추정) */
	__le32	unused8;  /* 패딩(추정) */
};

/*
 * Guess who created this format?
 */
static const char eesox_name[] = {
	'N', 'e', 'i', 'l', ' ',
	'C', 'r', 'i', 't', 'c', 'h', 'e', 'l', 'l', ' ', ' '
};

/*
 * EESOX SCSI partition format.
 *
 * This is a goddamned awful partition format.  We don't seem to store
 * the size of the partition in this table, only the start addresses.
 *
 * There are two possibilities where the size comes from:
 *  1. The individual ADFS boot block entries that are placed on the disk.
 *  2. The start address of the next entry.
 */
/*
 * adfspart_check_EESOX()
 * 목적:
 *   EESOX SCSI 파티션 테이블을 복호화하고 파티션 범위를 등록.
 * 호출 경로 (추정):
 *   check_partition() -> adfspart_check_EESOX() -> put_partition()
 * NVMe 연결점:
 *   파티션 크기가 명시적으로 저장되지 않고 다음 엔트리의 start와
 *   차이로 유추된다. 잘못 유추하면 NVMe I/O가 namespace 범위를
 *   벗어날 수 있으므로 주의가 필요하다(추정).
 */
int adfspart_check_EESOX(struct parsed_partitions *state)  /* NVMe namespace 상위 block device의 EESOX scheme 후보 */
{
	Sector sect;  /* read_part_sector()가 채울 단일 섹터 버퍼; NVMe Read 1 sector의 결과물 */
	const unsigned char *data;  /* NVMe media에서 읽은 원시 섹터 데이터 */
	unsigned char buffer[256];  /* 복호화된 파티션 테이블 버퍼; NVMe Read raw data 변환 */
	struct eesox_part *p;
	sector_t start = 0;  /* 이전 엔트리의 start; NVMe LBA 차이로 크기 유추 */
	int i, slot = 1;  /* slot은 커널 파티션 테이블 인덱스; NVMe queue depth와 무관 */

	data = read_part_sector(state, 7, &sect);  /* NVMe Read LBA 7 (EESOX partition table) */
	if (!data)  /* NVMe read/allocation 실패 */
		return -1;

	/*
	 * "Decrypt" the partition table.  God knows why...
	 */
	/* eesox_name을 XOR 키로 섹터 복호화 (추정) */
	for (i = 0; i < 256; i++)  /* byte 단위 복호화; NVMe media의 raw metadata 정규화 */
		buffer[i] = data[i] ^ eesox_name[i & 15];  /* XOR 복호화; NVMe와 무관한 데이터 변환 */

	put_dev_sector(sect);

	for (i = 0, p = (struct eesox_part *)buffer; i < 8; i++, p++) {  /* 최대 8개 엔트리; 각각 추가 NVMe LBA 범위 후보 */
		sector_t next;

		if (memcmp(p->magic, "Eesox", 6))  /* signature mismatch -> chain 종료; 추가 NVMe command 불필요 */
			break;

		next = le32_to_cpu(p->start);  /* 다음 파티션 시작 LBA */
		/* 이전 start와의 차이로 파티션 크기 결정 (추정) */
		if (i)
			put_partition(state, slot++, start, next - start);  /* block dev -> NVMe namespace LBA start~next */
		start = next;  /* 다음 iteration 기준 LBA 갱신 */
	}

	if (i != 0) {  /* 하나 이상의 EESOX 엔트리 발견 */
		sector_t size;

		/* 마지막 파티션은 디스크 끝까지로 설정 (추정) */
		size = get_capacity(state->disk);  /* NVMe namespace capacity */
		put_partition(state, slot++, start, size - start);  /* block dev -> NVMe namespace LBA start~end */
		seq_buf_puts(&state->pp_buf, "\n");
	}

	return i ? 1 : 0;  /* scheme 인식 여부 반환; NVMe와 무관 */
}
#endif

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 block/partitions/ 하위 계층에 속하며, NVMe SQ/CQ, CID,
 *   doorbell, PRP/SGL보다 상위에서 Acorn/ADFS 파티션 테이블을 파싱한다.
 * - 파싱 결과로 등록된 start_sector와 nr_sects는
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
 *   -> nvme_submit_cmd(doorbell) 경로에서 NVMe 명령의 SLBA 및 길이로
 *   변환된다.
 * - get_capacity(state->disk)는 NVMe namespace capacity를 반영하며,
 *   파티션 범위가 이를 초과하면 하위 NVMe 경로에서 오류가 발생할
 *   수 있다(추정).
 * - 본 파일은 block/partitions/check.c 등의 파티션 검색 프레임워크에서
 *   호출되며, NVMe 드라이버(drivers/nvme/)와는 직접 링크되지 않는다.
 * - 파티션 테이블의 체크섬 검증은 NVMe PI/CRC와는 독립적인
 *   소프트웨어 무결성 검증 단계다.
 */
