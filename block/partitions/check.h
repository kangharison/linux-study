/* SPDX-License-Identifier: GPL-2.0 */
/*
 * block/partitions/check.h — NVMe namespace에 대한 파티션 테이블 파싱 인프라
 *
 * 이 헤더는 블록 계층의 파티션 검출 단계에서 사용하는
 * parsed_partitions 상태 구조체와, 디스크 섹터를 읽어오는 헬퍼,
 * 그리고 다양한 파티션 형식 검출 루틴의 선언을 담고 있다.
 * NVMe SSD에서는 namespace가 gendisk로 등록된 후(예: nvme0n1),
 * rescan_partitions() -> check_partition() 경로에서 본 헤더의 함수들이
 * 호출되어 MBR/GPT 등 파티션 테이블을 분석한다.
 * 섹터 읽기는 blk-mq를 거쳐 NVMe SQ/CQ/doorbell로 전달된다.
 */
#include <linux/pagemap.h>	// NVMe DMA 완료 페이지의 페이지 캐시/버퍼 매핑 지원
#include <linux/blkdev.h>	// gendisk, block_device, submit_bio 등 NVMe 상위 블록 인프라
#include <linux/seq_buf.h>	// 파티션 결과 문자열("nvme0n1p1 ...") 버퍼링
#include "../blk.h"		// blk-mq 요청 큐 낮은 수준 헤더; NVMe I/O 스케줄링 경로 (추정)

/*
 * add_gd_partition은 파티션 정보를 gendisk의 파티션 구조체에 등록할 때 사용된다.
 * NVMe namespace 입장에서는 namespace 전체 LBA 영역을 나눈 구간이 된다.
 */

/*
 * parsed_partitions: 파티션 검출 과정에서 하나의 NVMe namespace(gendisk)를
 * 대상으로 상태를 유지하는 구조체.
 * 검출 루틴(msdos_partition, efi_partition 등)이 이 구조체를 통해
 * 섹터를 읽고, 발견한 파티션을 parts[]에 기록한다.
 */
struct parsed_partitions {
	struct gendisk *disk;		/* NVMe namespace가 등록된 gendisk (예: nvme0n1) */
	char name[BDEVNAME_SIZE];	/* 커널 로그/메시지용 디바이스 이름 (예: "nvme0n1") */
	struct {
		sector_t from;		/* 파티션 시작 LBA; NVMe namespace 내 논리 블록 오프셋 */
		sector_t size;		/* 파티션 크기(섹터 수); NVMe PRP/SGL 매핑 시 전체 LBA 범위의 일부 */
		int flags;		/* 파티션 플래그; NVMe 입출력 경로에서는 블록 장치 플래그로 전달 */
		bool has_info;		/* partition_meta_info가 유효한지 표시 */
		struct partition_meta_info info;	/* 파티션 UUID/레이블 등; NVMe가 아닌 파일시스템/부팅용 메타데이터 */
	} *parts;				/* 검출된 파티션 배열; NVMe firmware가 아닌 커널이 구성하는 소프트 스테이트 */
	int next;				/* 다음에 사용할 parts[] 인덱스 */
	int limit;				/* parts[] 최대 개수; NVMe 디스크에서도 최대 파티션 개수 제한 적용 */
	bool access_beyond_eod;		/* 디스크 끝을 넘어 읽으려 할 때 설정; NVMe는 이 경우 LBA 범위 초과 오류를 반환 (추정) */
	struct seq_buf pp_buf;		/* 검색 결과 문자열 버퍼; 커널 메시지에 "nvme0n1p1 p2 ..." 형태로 출력 */
};

/*
 * Sector: 파티션 테이블 섹터를 읽을 때 사용하는 한 섹터짜리 버퍼 래퍼.
 * read_part_sector()가 submit_bio -> ... -> nvme_queue_rq 경로로
 * 한 섹터를 읽어와 folio 형태로 반환한다.
 */
typedef struct {
	struct folio *v;	/* 읽어온 섹터가 담긴 folio; NVMe DMA 완료 후 페이지 캐시/버퍼로 매핑됨 (추정) */
} Sector;

/*
 * read_part_sector():
 *   파티션 테이블의 특정 섹터(LBA n)를 동기 방식으로 읽어온다.
 *   목적: MBR/GPT 등 파티션 서명을 확인하기 위해 디스크 첫 부분의
 *         원시 섹터 데이터를 가져온다.
 *   호출 경로(NVMe 기준):
 *     rescan_partitions() -> check_partition() -> <형식>_partition()
 *       -> read_part_sector()
 *       -> submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *       -> nvme_queue_rq -> nvme_submit_cmd(doorbell) -> CQ 완료
 *   반환값: 섹터 데이터의 가상 주소; 실패 시 NULL.
 *          읽은 Sector는 put_dev_sector()로 반드시 해제해야 한다.
 */
void *read_part_sector(struct parsed_partitions *state, sector_t n, Sector *p);
					/* LBA n을 NVMe READ 명령(CDW10~CDW13 SLBA)으로 변환해 동기 읽기 수행; 실패 시 NULL (추정) */

/*
 * put_dev_sector():
 *   read_part_sector()로 얻은 Sector의 folio 참조를 해제한다.
 *   목적: 동기 읽기가 끝난 후 섹터 버퍼의 수명을 종료한다.
 *   호출 경로:
 *     read_part_sector() 사용 후 각 검출 루틴에서 호출.
 *     read_part_sector -> submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *       -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 완료 후
 *       더 이상 필요 없어진 folio를 반납.
 */
static inline void put_dev_sector(Sector p)
{
	folio_put(p.v);	/* folio 참조 카운트 감소; NVMe DMA가 완료된 페이지의 수명 관리 */
}

/*
 * put_partition():
 *   n번 파티션 슬롯에 [from, from+size) 범위를 기록하고,
 *   커널 메시지용 이름 문자열에 "name<n>"을 추가한다.
 *   목적: 검출 루틴이 발견한 파티션을 parsed_partitions 상태에 등록.
 *   호출 경로:
 *     msdos_partition() / efi_partition() / ...
 *       -> put_partition()
 *       -> 이후 check_partition() 종료 후 add_gd_partition() 또는
 *          유사 경로로 gendisk->part_tbl에 반영 (추정).
 *   NVMe 연결점: NVMe namespace의 연속된 LBA 구간 하나를
 *               /dev/nvme0n1pN 형태의 독립 블록 디바이스로 노출하기 위한
 *               전 단계 정보이다.
 */
static inline void
put_partition(struct parsed_partitions *p, int n, sector_t from, sector_t size)
{
	if (n < p->limit) {	/* limit 초과 시 조용히 무시: NVMe에서도 최대 파티션 수 제한 적용 */
		p->parts[n].from = from;	/* NVMe namespace LBA 공간 내 파티션 시작 */
		p->parts[n].size = size;	/* NVMe namespace LBA 공간 내 파티션 크기 */
		seq_buf_printf(&p->pp_buf, " %s%d", p->name, n);	/* " nvme0n1p1" 형태로 버퍼에 기록 */
	}
}

/* 파티션 검출 루틴은 알파벳 순으로 아래에 선언한다. */
int adfspart_check_ADFS(struct parsed_partitions *state);	/* Acorn DFS; NVMe LBA 직접 탐색 (추정) */
int adfspart_check_CUMANA(struct parsed_partitions *state);	/* Cumana; NVMe LBA 직접 탐색 (추정) */
int adfspart_check_EESOX(struct parsed_partitions *state);	/* EESOX; NVMe LBA 직접 탐색 (추정) */
int adfspart_check_ICS(struct parsed_partitions *state);	/* ICS; NVMe LBA 직접 탐색 (추정) */
int adfspart_check_POWERTEC(struct parsed_partitions *state);	/* Powertec; NVMe LBA 직접 탐색 (추정) */
int aix_partition(struct parsed_partitions *state);		/* AIX LVM; NVMe READ로 서명/헤더 검증 */
int amiga_partition(struct parsed_partitions *state);		/* Amiga RDB; NVMe LBA 0/1 헤더 판독 */
int atari_partition(struct parsed_partitions *state);		/* Atari; CHS→LBA 변환 후 NVMe namespace 오프셋 검증 (추정) */
int cmdline_partition(struct parsed_partitions *state);		/* 커널 명령행; NVMe namespace LBA 범위 수동 지정 */
int efi_partition(struct parsed_partitions *state);		/* GPT; NVMe LBA 1 헤더 + 항목 배열 CRC 검증 */
int ibm_partition(struct parsed_partitions *);			/* IBM/iSeries; NVMe READ로 DASD 레이블 확인 */
int karma_partition(struct parsed_partitions *state);		/* Rio Karma; NVMe LBA 기반 서명 검사 */
int ldm_partition(struct parsed_partitions *state);		/* Windows LDM; NVMe 미디어의 PRIVHEAD/TOC 검증 */
int mac_partition(struct parsed_partitions *state);		/* Apple; NVMe LBA 1 드라이브 디스크립터 */
int msdos_partition(struct parsed_partitions *state);		/* MBR; NVMe LBA 0 부트섹터 + 항목 루프 + 확장 파티션 체인 (추정) */
int of_partition(struct parsed_partitions *state);		/* OpenFirmware; NVMe LBA 노출 파티션 파싱 */
int osf_partition(struct parsed_partitions *state);		/* OSF/Tru64; NVMe LBA 0 디스크라벨 */
int sgi_partition(struct parsed_partitions *state);		/* SGI; NVMe LBA 0 볼륨 헤더 */
int sun_partition(struct parsed_partitions *state);		/* Sun; NVMe LBA 0 VTOC */
int sysv68_partition(struct parsed_partitions *state);		/* SYSV/68k; NVMe LBA 기본 파티션 테이블 */
int ultrix_partition(struct parsed_partitions *state);		/* Ultrix; NVMe LBA 기반 디스크라벨 */

/* NVMe 관점 핵심 요약 */
/*
 * - 파티션 스캔은 NVMe namespace가 gendisk로 등록된 직후(예: nvme_ns_probe ->
 *   add_disk -> rescan_partitions) 발생하며, 본 헤더는 그 검출 단계의
 *   데이터 구조를 정의한다.
 * - read_part_sector()의 동기 I/O는 submit_bio -> blk_mq_submit_bio ->
 *   blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)을 통해
 *   NVMe SQ/CQ 하드웨어 큐로 전달된다.
 * - parsed_partitions.parts[]는 NVMe controller가 아닌 커널이 관리하는
 *   소프트 스테이트로, namespace 전체 LBA 영역을 여러 블록 디바이스
 *   (/dev/nvme0n1pN)로 노출하기 위한 메타데이터이다.
 * - put_partition()은 limit 미만일 때만 등록하므로, NVMe SSD에서도 커널의
 *   최대 파티션 개수 제한을 그대로 따른다.
 * - 본 파일은 block/partitions/core.c의 상위 조율 로직과 각 arch/vendor별
 *   검출 루틴(msdos_partition, efi_partition 등) 사이를 연결하는
 *   헤더 인터페이스이다.
 * - 구체적인 CHS→LBA 변환, 파티션 항목 루프, 확장 파티션 체인, 메모리 할당
 *   실패 분기, magic/signature/CRC 검증 코드는 본 헤더가 아닌
 *   block/partitions/msdos.c, efi.c, core.c 등 구현 파일에 존재한다.
 */
