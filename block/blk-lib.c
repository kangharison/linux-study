// SPDX-License-Identifier: GPL-2.0
/*
 * Functions related to generic helpers functions
 */
/*
 * [한국어 설명] block/blk-lib.c — discard/write-zeroes/secure-erase 범용 블록 계층 헬퍼 (block/blk-lib.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 블록 계층(block layer)이 파일시스템/사용자공간으로부터 받은
 * "특수 목적" I/O 요청 세 가지 — discard(TRIM/UNMAP), write zeroes(영역을
 * 0으로 채우기), secure erase(보안 삭제) — 를 실제 하드웨어가 처리할 수
 * 있는 bio(block I/O) 단위로 변환·분할·제출하는 공용 헬퍼 라이브러리다.
 * 사용자가 요청한 (sector, nr_sects) 범위는 디바이스의 queue_limits
 * (discard_granularity, max_discard_sectors, max_write_zeroes_sectors,
 * max_secure_erase_sectors 등)를 넘을 수 있으므로, 이 파일은 그 한계에
 * 맞춰 요청을 여러 bio로 쪼개고 blk_plug로 묶어 순차 제출한 뒤 필요하면
 * 완료까지 동기 대기한다. 하드웨어가 discard/write-zeroes/secure-erase를
 * 지원하지 않는 경우를 대비해, write zeroes에는 "zero-filled page를 직접
 * 쓰는" 소프트웨어 폴백 경로도 함께 제공한다. 따라서 이 파일은 파일시스템
 * 계층의 fallocate(FALLOC_FL_ZERO_RANGE), fstrim/BLKDISCARD ioctl,
 * BLKSECDISCARD ioctl 등이 최종적으로 저장장치 프로토콜(NVMe Dataset
 * Management Deallocate, NVMe Write Zeroes, NVMe Sanitize/Format NVM 등)로
 * 번역되기 직전의 "블록 계층 최상위 공개 API" 역할을 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 전체 I/O 스택에서 이 파일은 "파일시스템/블록 디바이스 ioctl 계층"과
 * "bio 제출 계층(submit_bio)" 사이의 변환 지점에 위치한다. 대표적인 호출
 * 체인은 다음과 같다.
 * (1) discard — fstrim/mount -o discard, BLKDISCARD ioctl
 *     (block/ioctl.c) -> blkdev_issue_discard() -> __blkdev_issue_discard()
 *     -> blk_alloc_discard_bio()가 만든 REQ_OP_DISCARD bio들을
 *     bio_chain_and_submit()으로 연결해 submit_bio()에 전달.
 * (2) write zeroes — vfs_fallocate(FALLOC_FL_ZERO_RANGE) ->
 *     blkdev_fallocate()(block/fops.c) 또는 zone 오류 시
 *     blk_zone_issue_zeroout()(block/blk-zoned.c) ->
 *     blkdev_issue_zeroout() -> __blkdev_issue_zeroout() -> 하드웨어
 *     오프로드 가능 시 blkdev_issue_write_zeroes()/REQ_OP_WRITE_ZEROES,
 *     불가능 시 blkdev_issue_zero_pages()/REQ_OP_WRITE 폴백.
 * (3) secure erase — BLKSECDISCARD ioctl(block/ioctl.c) ->
 *     blkdev_issue_secure_erase() -> REQ_OP_SECURE_ERASE bio 제출.
 * 이후 모든 경로는 submit_bio() -> submit_bio_noacct() ->
 * blk_mq_submit_bio() -> blk_mq_get_request()를 거쳐 각 드라이버(NVMe라면
 * nvme_queue_rq())가 SQ(Submission Queue)에 커맨드를 적재하고 도어벨을
 * 울리는 지점까지 이어진다. 이 파일 자체는 항상 "제출자"(submitter)의
 * 프로세스 컨텍스트에서 실행되며, 인터럽트 컨텍스트나 GPU/DMA 컨텍스트와는
 * 무관하다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일은 block/bio.c가 제공하는 bio_alloc(), bio_chain_and_submit(),
 * bio_submit_or_kill(), blk_next_bio(), submit_bio_wait()에 의존해 실제
 * bio 생성·체인·제출을 수행하며, block/blk-settings.c가 request_queue에
 * 채워 넣는 queue_limits(bdev_discard_granularity(),
 * bdev_write_zeroes_sectors(), bdev_max_secure_erase_sectors(),
 * bdev_logical_block_size() 등 accessor를 통해 include/linux/blkdev.h에서
 * 읽음)에 의존해 하드웨어 한계를 조회한다. 반대로 이 파일이 export하는
 * blkdev_issue_discard(), blkdev_issue_zeroout(), __blkdev_issue_zeroout(),
 * blkdev_issue_secure_erase(), __blkdev_issue_discard(),
 * blk_alloc_discard_bio()는 block/ioctl.c(BLKDISCARD/BLKZEROOUT/
 * BLKSECDISCARD ioctl 구현), block/fops.c(blkdev_fallocate),
 * block/blk-zoned.c(zone 상태 오류 시 zone을 0으로 채우는 경로),
 * fs/*(sb_issue_discard/sb_issue_zeroout 등 include/linux/blkdev.h의
 * 인라인 래퍼), drivers/block/loop.c(loop 디바이스가 discard를 zeroout으로
 * 재구성하는 경로) 등 상위 소비자에게 사용된다. 데이터 흐름 관점에서는
 * 사용자가 지정한 (sector, nr_sects) 범위 값만 오갈 뿐 실제 페이로드
 * 데이터는 없다(discard/write-zeroes/secure-erase는 모두 "데이터 없는"
 * 커맨드이거나, 있어도 고정된 zero 페이지만 사용). 공유하는 핵심
 * 자료구조는 struct bio(각 조각 요청의 단위), struct blk_plug(연속 제출을
 * 배칭해 큐 진입/도어벨 갱신 오버헤드를 줄이는 스택 로컬 구조체),
 * struct block_device 및 그 queue_limits이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - bio_discard_limit()/blk_alloc_discard_bio(): discard_granularity에
 *   맞춰 한 bio가 담을 수 있는 최대 섹터 수를 계산하고, 실제
 *   REQ_OP_DISCARD bio 한 개를 할당한다.
 * - __blkdev_issue_discard()/blkdev_issue_discard(): blk_alloc_discard_bio()
 *   를 반복 호출해 전체 범위를 다 소진할 때까지 bio 체인을 만들고, 마지막에
 *   submit_bio_wait()로 완료까지 대기한다.
 * - bio_write_zeroes_limit()/__blkdev_issue_write_zeroes()/
 *   blkdev_issue_write_zeroes(): 하드웨어 Write Zeroes 오프로드를 사용해
 *   REQ_OP_WRITE_ZEROES bio들을 생성·제출한다.
 * - __blkdev_sectors_to_bio_pages()/__blkdev_issue_zero_pages()/
 *   blkdev_issue_zero_pages(): 하드웨어 오프로드가 없을 때 미리 할당된
 *   "0으로 채워진 공유 페이지(zero folio)"를 데이터로 사용하는
 *   REQ_OP_WRITE bio를 생성해 폴백한다.
 * - __blkdev_issue_zeroout()/blkdev_issue_zeroout(): 위 두 경로(오프로드
 *   우선, 실패 시 zero page 폴백)를 조합하는 공개 API이며,
 *   BLKDEV_ZERO_NOUNMAP/NOFALLBACK/KILLABLE 플래그로 세부 동작을
 *   제어한다.
 * - blkdev_issue_secure_erase(): REQ_OP_SECURE_ERASE bio들을
 *   max_secure_erase_sectors 한도로 분할 제출하고 완료까지 대기한다.
 * 이 파일은 자체 구조체/enum을 정의하지 않으며, 위 함수들이 공유하는
 * struct bio/struct blk_plug는 각각 block/bio.c, include/linux/blkdev.h
 * 에서 정의된다.
 */
#include <linux/kernel.h> /* [한국어] round_up()/round_down()/min()/DIV_ROUND_UP 등 커널 공통 매크로 - 이 파일 곳곳의 정렬/클램핑 계산에 사용 */
#include <linux/module.h> /* [한국어] EXPORT_SYMBOL 계열 매크로 정의 - 이 파일의 공개 함수들을 다른 커널 모듈(파일시스템, 드라이버)에 노출하기 위해 필요 */
#include <linux/bio.h> /* [한국어] struct bio, bio_alloc(), bio_add_folio() 등 bio 자료구조/조작 API - 이 파일의 모든 함수가 bio를 생성·조립하는 데 사용 */
#include <linux/blkdev.h> /* [한국어] struct block_device, BLKDEV_ZERO_* 플래그, bdev_*_sectors() 등 queue_limits accessor 선언 - 하드웨어 한계 조회에 사용 */
#include <linux/scatterlist.h> /* [한국어] struct scatterlist 관련 정의 - 이 번역 단위에서 직접 쓰이는 곳은 드러나지 않으나, bio/블록 계층 공통 include 관례로 유지된 것으로 추정 */

#include "blk.h" /* [한국어] block/ 디렉터리 내부 전용 헤더 - bio_chain_and_submit(), bio_submit_or_kill() 등 블록 계층 내부(비공개) API 선언을 가져옴 */

/*
 * [한국어]
 * bio_discard_limit - 한 번의 discard bio가 담을 수 있는 최대 섹터 수 계산
 *
 * @bdev:   discard 대상 블록 디바이스 (파티션이면 절대 섹터로 보정 필요)
 * @sector: 이번에 만들 bio가 시작할 (파티션 상대) 섹터 번호
 * @return: 이번 bio에 담을 수 있는 섹터 수. 시작 섹터가 discard_granularity
 *          경계에 정렬돼 있지 않으면 "정렬 지점까지 남은 거리"를 우선
 *          반환하고, 정렬돼 있으면 "granularity의 배수로 자른 bio 최대
 *          크기"를 반환한다.
 *
 * 배경: NVMe Deallocate(Trim)를 비롯한 여러 저장장치는 discard 요청이
 * discard_granularity(전형적으로 4KB 등 namespace의 소거 단위) 경계에
 * 정렬되어 있을 때만 효율적으로 처리한다. 이 함수는 blk_alloc_discard_bio()
 * 가 매 반복마다 호출해, 다음 bio의 길이를 "정렬을 맞추기 위한 패딩" 또는
 * "정렬된 최대 크기" 중 하나로 결정하게 해 준다.
 * 동작 순서: (1) 파티션이면 디스크 전체 기준 절대 섹터로 변환한다.
 * (2) 그 섹터를 discard_granularity(섹터 단위)의 배수로 올림 정렬한다.
 * (3) 올림 결과가 원래 섹터와 다르면(=아직 비정렬), 정렬 지점까지의 거리만
 * 우선 반환해 다음 bio부터는 정렬된 섹터에서 시작하게 한다.
 * (4) 이미 정렬되어 있으면 BIO_MAX_SIZE를 granularity로 내림 정렬한 값을
 * 반환해, 이후 만들어지는 bio들도 계속 정렬 경계에서 끊기게 한다.
 * 실행 컨텍스트: blkdev_issue_discard() 호출자의 프로세스 컨텍스트에서
 * 동기적으로 실행되며 별도 동기화가 필요 없다(단일 스레드가 순차 호출).
 * 호출자: blk_alloc_discard_bio().
 * 피호출자: bdev_discard_granularity(), bdev_is_partition(), round_up(),
 * round_down() (모두 인라인/매크로 헬퍼).
 *
 * 호출 체인:
 *   blkdev_issue_discard -> __blkdev_issue_discard -> blk_alloc_discard_bio
 *   -> [bio_discard_limit]
 */
static sector_t bio_discard_limit(struct block_device *bdev, sector_t sector)
{
	unsigned int discard_granularity = bdev_discard_granularity(bdev); /* [한국어] 이 디바이스(예: NVMe namespace)의 discard 정렬 단위(바이트)를 queue_limits에서 조회 */
	sector_t granularity_aligned_sector; /* [한국어] sector를 granularity 경계로 올림 정렬한 결과를 담을 지역 변수 - 아래에서 계산 후 비교에 사용 */

	if (bdev_is_partition(bdev)) /* [한국어] bdev가 파티션(하위 디바이스)이면 파티션 시작 오프셋을 더해야 디스크 전체 기준 절대 섹터가 됨 */
		sector += bdev->bd_start_sect; /* [한국어] 파티션 시작 섹터를 더해 절대(디스크 기준) 섹터로 변환 - discard_granularity는 디스크 전체 기준으로 정의되므로 필요 */

	granularity_aligned_sector =
		round_up(sector, discard_granularity >> SECTOR_SHIFT); /* [한국어] discard_granularity(바이트)를 섹터 단위로 변환(>>SECTOR_SHIFT) 후, sector를 그 배수로 올림 정렬 */

	/*
	 * Make sure subsequent bios start aligned to the discard granularity if
	 * it needs to be split.
	 */
	/* [한국어] 위 원문 번역: 분할이 필요한 경우, 이후 만들어지는 bio들이 discard
	 * granularity 경계에서 시작하도록 보장한다.
	 * NVMe 관점: 이번에 반환되는 값은 "정렬을 맞추기 위한 패딩용 bio 크기"이며,
	 * 다음 blk_alloc_discard_bio() 호출부터는 sector가 이미 granularity 배수이므로
	 * 이 분기를 타지 않고 최대 크기 분기로 진입한다. */
	if (granularity_aligned_sector != sector) /* [한국어] 올림 정렬 결과가 원래 섹터와 다르면 = 아직 granularity 경계에 정렬되지 않은 상태 */
		return granularity_aligned_sector - sector; /* [한국어] 정렬 지점까지 남은 섹터 수만 우선 반환 - 이 bio는 "정렬 맞추기 전용" 패딩 조각이 됨 */

	/*
	 * Align the bio size to the discard granularity to make splitting the bio
	 * at discard granularity boundaries easier in the driver if needed.
	 */
	/* [한국어] 위 원문 번역: 드라이버가 필요 시 discard granularity 경계에서 bio를
	 * 쉽게 분할할 수 있도록 bio 크기 자체도 granularity에 정렬한다.
	 * NVMe 관점: sector가 이미 정렬돼 있으므로, 여기서는 "한 번에 요청 가능한
	 * 최대 길이"를 계산해 반환한다. */
	return round_down(BIO_MAX_SIZE, discard_granularity) >> SECTOR_SHIFT; /* [한국어] BIO_MAX_SIZE(바이트)를 granularity의 배수로 내림 정렬한 뒤 섹터 단위로 변환해 반환 - 이 값이 한 bio의 최대 discard 길이 */
}

/*
 * [한국어]
 * blk_alloc_discard_bio - REQ_OP_DISCARD bio 한 개를 할당하고 초기화한다
 *
 * @bdev:     discard 대상 블록 디바이스
 * @sector:   [in,out] 다음 bio가 시작할 섹터. 성공 시 이번 bio가 소비한
 *            길이만큼 전진시켜 호출자에게 돌려준다.
 * @nr_sects: [in,out] 남은 discard 대상 섹터 수. 성공 시 이번 bio가
 *            소비한 만큼 감소시켜 돌려준다.
 * @gfp_mask: bio_alloc()에 전달할 메모리 할당 플래그
 * @return:   새로 할당된 REQ_OP_DISCARD bio. 남은 길이가 0이거나 bio
 *            할당에 실패하면 NULL.
 *
 * 배경: 사용자가 요청한 discard 범위 전체를 한 bio에 담을 수 없는 경우가
 * 많으므로(디바이스 한계, granularity 정렬 등), 호출자(__blkdev_issue_discard)
 * 는 이 함수를 반복 호출해 필요한 만큼 bio를 만들어 체인한다.
 * 동작 순서: (1) bio_discard_limit()으로 이번 bio가 담을 수 있는 최대 섹터
 * 수를 구하고 남은 길이와 비교해 실제 길이(bio_sects)를 정한다.
 * (2) 남은 길이가 0이면(전체 완료) NULL을 반환해 호출자의 while 루프를
 * 종료시킨다. (3) 데이터 페이지가 없는(0번째 인자) REQ_OP_DISCARD bio를
 * 할당한다 - discard는 전송할 데이터가 없는 "메타" 커맨드이기 때문이다.
 * (4) 시작 섹터/길이를 bio에 채우고, *sector/*nr_sects를 전진시켜 다음
 * 호출을 준비한다. (5) cond_resched()로 스케줄링 포인트를 만들어, 전체
 * 디스크 discard(mkfs 등) 같은 매우 긴 루프에서 CPU를 독점하지 않게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, __blkdev_issue_discard()의 while 루프
 * 안에서 순차 호출되므로 동시성 문제 없음.
 * 호출자: __blkdev_issue_discard(), block/ioctl.c의 blk_ioctl_discard().
 * 피호출자: bio_discard_limit(), bio_alloc(), cond_resched().
 *
 * 호출 체인:
 *   __blkdev_issue_discard -> [blk_alloc_discard_bio] -> bio_discard_limit
 */
struct bio *blk_alloc_discard_bio(struct block_device *bdev,
		sector_t *sector, sector_t *nr_sects, gfp_t gfp_mask)
{
	sector_t bio_sects = min(*nr_sects, bio_discard_limit(bdev, *sector)); /* [한국어] 남은 길이와 "이번 bio가 담을 수 있는 최대 길이" 중 작은 값으로 실제 bio 길이 결정 */
	struct bio *bio; /* [한국어] 새로 할당할 discard bio를 가리킬 포인터 - 아래에서 bio_alloc()으로 채워짐 */

	if (!bio_sects) /* [한국어] 이번에 담을 길이가 0 = 남은 discard 범위가 모두 소진됨 */
		return NULL; /* [한국어] 더 만들 bio가 없음을 호출자(__blkdev_issue_discard)의 while 루프에 알려 종료시킴 */

	bio = bio_alloc(bdev, 0, REQ_OP_DISCARD, gfp_mask); /* [한국어] 데이터 벡터 0개(bvec 없음)로 discard bio 할당 - discard는 실제 페이로드 전송이 없는 커맨드 */
	if (!bio) /* [한국어] bio 풀 고갈 등으로 할당 실패한 경우 */
		return NULL; /* [한국어] 실패를 호출자에 알림 - 이 경우 남은 discard 범위는 처리되지 못한 채 남을 수 있음(상위에서 별도 처리 없음, 원본 동작 그대로) */
	bio->bi_iter.bi_sector = *sector; /* [한국어] bio의 시작 섹터를 설정 - 이후 NVMe 등 드라이버가 SLBA(Start LBA) 계산의 기초로 사용 */
	bio->bi_iter.bi_size = bio_sects << SECTOR_SHIFT; /* [한국어] bio 길이를 섹터 수에서 바이트 단위로 변환해 설정 - 드라이버가 discard Length 계산에 사용 */
	*sector += bio_sects; /* [한국어] 다음 호출을 위해 시작 섹터를 이번에 소비한 만큼 전진 */
	*nr_sects -= bio_sects; /* [한국어] 다음 호출을 위해 남은 섹터 수를 이번에 소비한 만큼 감소 */
	/*
	 * We can loop for a long time in here if someone does full device
	 * discards (like mkfs).  Be nice and allow us to schedule out to avoid
	 * softlocking if preempt is disabled.
	 */
	/* [한국어] 위 원문 번역: mkfs처럼 전체 디바이스를 discard하면 이 함수가
	 * 오랫동안 반복 호출될 수 있다. preempt가 꺼져 있어도 softlockup이
	 * 발생하지 않도록 스케줄 포인트를 제공한다. */
	cond_resched(); /* [한국어] 자발적 스케줄링 포인트 - 매우 큰 discard 요청 처리 중 다른 태스크가 굶주리지 않도록 양보 */
	return bio; /* [한국어] 새로 만든 bio를 호출자에게 반환 - 호출자는 이를 bio_chain_and_submit()으로 체인/제출 */
}

/*
 * [한국어]
 * __blkdev_issue_discard - discard 범위를 여러 bio로 분할해 체인 제출한다 (완료 대기 없음)
 *
 * @bdev:     discard 대상 블록 디바이스
 * @sector:   discard를 시작할 섹터
 * @nr_sects: discard할 섹터 수
 * @gfp_mask: bio 할당 플래그
 * @biop:     [in,out] bio 체인의 "현재 anchor"를 가리키는 포인터. 호출
 *            전 *biop이 NULL이 아니면 기존 체인 뒤에 이어붙인다. 호출
 *            후에는 이 함수가 만든 마지막(아직 제출되지 않은) bio를
 *            가리키게 된다.
 * @return:   없음(void) - 에러는 이후 submit_bio_wait()의 반환값으로 전달됨
 *
 * 배경: 하나의 discard 요청은 디바이스 한계 때문에 여러 bio로 쪼개져야
 * 한다. 이 함수는 그 분할·체인 로직만 담당하고, 실제 완료 대기(동기화)는
 * 호출자(blkdev_issue_discard)에게 맡긴다 - 이렇게 분리해 두면 다른
 * 호출자가 여러 범위를 하나의 bio 체인으로 누적시킨 뒤 한 번에 제출/대기
 * 하는 것도 가능해진다.
 * 동작 순서: blk_alloc_discard_bio()가 NULL이 아닌 bio를 반환하는 동안
 * 반복하며, 매번 bio_chain_and_submit(*biop, bio)로 "이전 anchor를 새
 * bio에 체인하고 이전 anchor를 submit_bio()로 제출"한 뒤 결과(새 bio)를
 * 다시 *biop에 저장한다. blk_alloc_discard_bio() 내부에서 *sector/*nr_sects
 * 가 갱신되므로 루프 조건 자체가 종료 조건 역할을 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트. blk_start_plug()/blk_finish_plug()로
 * 감싸는 것은 호출자의 책임이다.
 * 호출자: blkdev_issue_discard(), 여러 범위의 discard를 누적 제출하려는
 * 그 외 블록 계층 내부 호출자.
 * 피호출자: blk_alloc_discard_bio(), bio_chain_and_submit().
 *
 * 호출 체인:
 *   blkdev_issue_discard -> [__blkdev_issue_discard] -> blk_alloc_discard_bio
 *   -> bio_chain_and_submit -> submit_bio -> blk_mq_submit_bio -> ... ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell)
 */
void __blkdev_issue_discard(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp_mask, struct bio **biop)
{
	struct bio *bio; /* [한국어] 매 반복에서 새로 할당된 discard bio 조각을 임시로 담을 지역 변수 */

	while ((bio = blk_alloc_discard_bio(bdev, &sector, &nr_sects,
			gfp_mask))) /* [한국어] 남은 범위가 있는 한 계속 bio를 만듦 - NULL이 반환되면(범위 소진 또는 할당 실패) 루프 종료 */
		*biop = bio_chain_and_submit(*biop, bio); /* [한국어] 이전 anchor(*biop)를 새 bio에 체인하고 이전 anchor를 submit_bio()로 제출, 새 bio를 다음 anchor로 저장 */
}
EXPORT_SYMBOL(__blkdev_issue_discard); /* [한국어] 이 심볼을 커널 심볼 테이블에 등록 - GPL 여부와 무관하게 모든 모듈(파일시스템, 드라이버)이 사용 가능 */

/*
 * [한국어]
 * blkdev_issue_discard - discard 요청을 큐에 넣고 완료까지 대기한다
 *
 * @bdev:     discard 대상 블록 디바이스
 * @sector:   discard를 시작할 섹터
 * @nr_sects: discard할 섹터 수
 * @gfp_mask: bio 할당 플래그
 * @return:   0=성공(또는 디바이스가 discard 미지원이라 조용히 무시됨),
 *            음수=submit_bio_wait()가 보고한 그 외 에러
 *
 * 배경: fstrim, mount -o discard, BLKDISCARD ioctl 등 파일시스템/사용자
 * 공간에서 호출되는 discard의 최상위 동기(synchronous) 진입점이다.
 * 동작 순서: (1) blk_start_plug()로 플러그를 걸어 이 요청이 만드는 여러
 * bio 제출을 배칭한다(드라이버의 도어벨 갱신 횟수를 줄이는 효과, NVMe
 * 관점에서는 SQ tail 갱신을 모아서 한 번에 하는 것과 유사). (2)
 * __blkdev_issue_discard()로 실제 bio 체인을 생성·제출한다. (3) 체인의
 * 마지막 bio(*bio)가 있으면 submit_bio_wait()로 그 bio의 완료(및 체인된
 * 모든 하위 bio의 완료, bio_chain 메커니즘 덕분)까지 동기 대기한다.
 * (4) 디바이스가 discard를 지원하지 않아 -EOPNOTSUPP가 나면 사용자에게는
 * 에러로 보이지 않도록 0으로 치환한다(discard는 "최선 노력" 힌트이기
 * 때문). (5) blk_finish_plug()로 플러그를 해제해 배칭된 나머지 요청을
 * 마저 흘려보낸다.
 * 실행 컨텍스트: 프로세스 컨텍스트, submit_bio_wait() 내부에서 완료까지
 * 잠들 수 있으므로(sleep 가능) 인터럽트/원자적 컨텍스트에서 호출 금지.
 * 호출자: fs/*(sb_issue_discard 등 파일시스템 discard 헬퍼),
 * block/ioctl.c(BLKDISCARD ioctl).
 * 피호출자: blk_start_plug(), __blkdev_issue_discard(), submit_bio_wait(),
 * bio_put(), blk_finish_plug().
 *
 * 호출 체인:
 *   fs 계층/BLKDISCARD ioctl -> [blkdev_issue_discard] ->
 *   __blkdev_issue_discard -> ... -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 */
/**
 * blkdev_issue_discard - queue a discard
 * @bdev:	blockdev to issue discard for
 * @sector:	start sector
 * @nr_sects:	number of sectors to discard
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 *
 * Description:
 *    Issue a discard request for the sectors in question.
 */
int blkdev_issue_discard(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp_mask)
{
	struct bio *bio = NULL; /* [한국어] bio 체인의 anchor - __blkdev_issue_discard()가 채워 넣고, 아래에서 완료 대기/해제에 사용 */
	struct blk_plug plug; /* [한국어] 스택에 놓는 플러그 상태 객체 - blk_start_plug/blk_finish_plug 쌍으로 이 함수 호출 동안의 bio 제출을 배칭 */
	int ret = 0; /* [한국어] 최종 반환값 - 기본값 0(성공), bio가 있으면 submit_bio_wait() 결과로 갱신 */

	blk_start_plug(&plug); /* [한국어] 플러그 시작 - 이후 submit_bio() 호출들이 즉시 큐로 내려가지 않고 plug 리스트에 모였다가 finish 시 한꺼번에 처리됨 */
	__blkdev_issue_discard(bdev, sector, nr_sects, gfp_mask, &bio); /* [한국어] 실제 discard bio 체인 생성 및 제출 - bio에 마지막 anchor 저장 */
	if (bio) { /* [한국어] 최소 하나 이상의 bio가 만들어졌다면(=discard할 범위가 있었다면) */
		ret = submit_bio_wait(bio); /* [한국어] 마지막 bio(및 체인된 모든 하위 bio)의 완료를 동기적으로 기다려 결과 코드를 받음 */
		if (ret == -EOPNOTSUPP) /* [한국어] 디바이스/드라이버가 discard를 지원하지 않는다고 보고한 경우 */
			ret = 0; /* [한국어] discard는 힌트性 요청이므로 미지원을 에러로 취급하지 않고 성공으로 간주 */
		bio_put(bio); /* [한국어] submit_bio_wait()가 끝난 bio의 참조 해제 - 이 함수가 마지막 소유자이므로 여기서 free */
	}
	blk_finish_plug(&plug); /* [한국어] 플러그 해제 - 모아뒀던 나머지 bio들을 실제로 큐에 흘려보냄(NVMe라면 SQ doorbell 갱신) */

	return ret; /* [한국어] 호출자(파일시스템 등)에게 최종 결과 반환 */
}
EXPORT_SYMBOL(blkdev_issue_discard); /* [한국어] 커널 심볼로 공개 - 파일시스템/ioctl 계층에서 사용 */

/*
 * [한국어]
 * bio_write_zeroes_limit - 하드웨어 Write Zeroes bio 한 개의 최대 섹터 수 계산
 *
 * @bdev:   대상 블록 디바이스
 * @return: 논리 블록 크기에 정렬된, 한 bio가 담을 수 있는 최대 섹터 수.
 *          디바이스가 write zeroes 오프로드를 지원하지 않으면 0.
 *
 * 배경: NVMe Write Zeroes 등 하드웨어 오프로드는 디바이스마다 한 커맨드에
 * 담을 수 있는 최대 길이(bdev_write_zeroes_sectors)가 다르고, bio 자체도
 * bi_size가 표현 가능한 최대 크기(BIO_MAX_SECTORS)를 넘을 수 없다. 이
 * 함수는 그 두 제약과 논리 블록 크기 정렬을 모두 반영한 "안전한 최댓값"을
 * 계산한다.
 * 동작 순서: (1) 논리 블록 크기(바이트)를 512B 섹터 단위로 변환한 뒤 1을
 * 뺀 마스크(bs_mask)를 만든다 - 예: 4096B 블록이면 mask=7. (2)
 * bdev_write_zeroes_sectors()(디바이스 하드웨어 한계)와
 * BIO_MAX_SECTORS & ~bs_mask(bio 한계를 논리 블록 크기로 내림 정렬한 값)
 * 중 작은 값을 반환한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 순수 계산 함수(부작용 없음).
 * 호출자: blkdev_issue_write_zeroes(), __blkdev_issue_zeroout()(오프로드
 * 가능 여부 판단).
 * 피호출자: bdev_logical_block_size(), bdev_write_zeroes_sectors() (모두
 * queue_limits accessor).
 *
 * 호출 체인:
 *   __blkdev_issue_zeroout -> [bio_write_zeroes_limit]
 */
static sector_t bio_write_zeroes_limit(struct block_device *bdev)
{
	sector_t bs_mask = (bdev_logical_block_size(bdev) >> 9) - 1; /* [한국어] 논리 블록 크기(바이트)를 512B 단위로 변환 후 1을 빼 "정렬 마스크" 생성 - 예: 4096B -> 8섹터 -> mask=7 */

	return min(bdev_write_zeroes_sectors(bdev), BIO_MAX_SECTORS & ~bs_mask); /* [한국어] 디바이스가 보고한 최대 write-zeroes 섹터 수와, bio 최대 섹터 수를 논리 블록 크기로 내림 정렬한 값 중 더 작은 값 채택 */
}

/*
 * There is no reliable way for the SCSI subsystem to determine whether a
 * device supports a WRITE SAME operation without actually performing a write
 * to media. As a result, write_zeroes is enabled by default and will be
 * disabled if a zeroing operation subsequently fails. This means that this
 * queue limit is likely to change at runtime.
 */
	/* [한국어] 위 원문 번역: SCSI 서브시스템은 실제로 미디어에 쓰기를 수행해
	 * 보지 않고는 장치가 WRITE SAME을 지원하는지 신뢰성 있게 판단할 방법이
	 * 없다. 따라서 write_zeroes는 기본적으로 활성화돼 있다가, 실제 zeroing
	 * 동작이 이후에 실패하면 비활성화된다. 즉 이 큐 한계
	 * (bdev_write_zeroes_sectors)는 런타임 중 언제든 바뀔 수 있다는 뜻이며,
	 * 아래 __blkdev_issue_zeroout()이 매 호출마다 이 값을 다시 조회하는
	 * 이유이기도 하다. */
/*
 * [한국어]
 * __blkdev_issue_write_zeroes - 하드웨어 오프로드로 REQ_OP_WRITE_ZEROES bio들을 생성·제출
 *
 * @bdev:     대상 블록 디바이스
 * @sector:   0-채우기를 시작할 섹터
 * @nr_sects: 0으로 채울 섹터 수
 * @gfp_mask: bio 할당 플래그
 * @biop:     [in,out] bio 체인 anchor - __blkdev_issue_discard()와 동일한 패턴
 * @flags:    BLKDEV_ZERO_NOUNMAP/NOFALLBACK/KILLABLE 조합
 * @limit:    bio_write_zeroes_limit()이 계산한, 한 bio의 최대 섹터 수
 * @return:   없음(void)
 *
 * 배경: 디바이스가 Write Zeroes를 지원하면(limit != 0), 실제 데이터를
 * 전송하지 않고도 특정 범위를 0으로 채울 수 있어 훨씬 효율적이다. 이
 * 함수는 그 하드웨어 오프로드 경로의 분할·제출 로직을 담당한다.
 * 동작 순서: nr_sects가 남아있는 동안 반복하며, 매 반복마다 (1)
 * BLKDEV_ZERO_KILLABLE이 설정되어 있고 프로세스에 치명적 시그널이 대기
 * 중이면 즉시 루프를 중단한다(긴 0-채우기 도중 kill 가능하게). (2) limit
 * 크기로 자른 bio를 REQ_OP_WRITE_ZEROES로 할당한다. (3)
 * BLKDEV_ZERO_NOUNMAP이 설정되어 있으면 REQ_NOUNMAP 플래그를 추가로
 * 세팅해, 이 write-zeroes가 저장공간을 반환(deallocate)하지 않도록
 * 드라이버에 지시한다. (4) 시작 섹터/길이를 채우고 bio_chain_and_submit()
 * 으로 이전 anchor를 체인·제출한다. (5) 남은 길이/다음 섹터를 갱신하고
 * cond_resched()로 양보한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, KILLABLE 플래그가 있으면 시그널 확인을
 * 위해 매 반복 fatal_signal_pending()을 검사.
 * 호출자: blkdev_issue_write_zeroes().
 * 피호출자: bio_alloc(), bio_chain_and_submit(), fatal_signal_pending(),
 * cond_resched().
 *
 * 호출 체인:
 *   blkdev_issue_write_zeroes -> [__blkdev_issue_write_zeroes] ->
 *   bio_chain_and_submit -> submit_bio -> ... -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell)
 */
static void __blkdev_issue_write_zeroes(struct block_device *bdev,
		sector_t sector, sector_t nr_sects, gfp_t gfp_mask,
		struct bio **biop, unsigned flags, sector_t limit)
{

	while (nr_sects) { /* [한국어] 요청 범위가 남아있는 한 계속 bio를 생성 - 0이 되면 루프 종료 */
		unsigned int len = min(nr_sects, limit); /* [한국어] 남은 길이와 "한 bio 최대 길이(limit)" 중 작은 값으로 이번 bio 길이 결정 */
		struct bio *bio; /* [한국어] 이번 반복에서 새로 할당할 write-zeroes bio */

		if ((flags & BLKDEV_ZERO_KILLABLE) &&
		    fatal_signal_pending(current)) /* [한국어] 호출자가 "시그널로 중단 가능"을 요청했고(KILLABLE), 실제로 현재 태스크에 치명적 시그널이 대기 중이면 */
			break; /* [한국어] 남은 범위를 포기하고 루프 탈출 - 지금까지 체인된 bio들은 그대로 제출되어 처리됨(부분 완료) */

		bio = bio_alloc(bdev, 0, REQ_OP_WRITE_ZEROES, gfp_mask); /* [한국어] 데이터 벡터 없이(0) REQ_OP_WRITE_ZEROES bio 할당 - 실제 페이로드 전송 없이 0-채우기를 지시하는 커맨드 */
		bio->bi_iter.bi_sector = sector; /* [한국어] 이번 bio의 시작 섹터 설정 - NVMe라면 Write Zeroes 커맨드의 SLBA 계산 기초 */
		if (flags & BLKDEV_ZERO_NOUNMAP) /* [한국어] 호출자가 "공간을 반환하지 말라"고 요청했으면 */
			bio->bi_opf |= REQ_NOUNMAP; /* [한국어] REQ_NOUNMAP 플래그 추가 - 드라이버가 Deallocate 비트를 세우지 않고 순수하게 0만 쓰도록 지시 */

		bio->bi_iter.bi_size = len << SECTOR_SHIFT; /* [한국어] 이번 bio 길이를 섹터에서 바이트로 변환해 설정 */
		*biop = bio_chain_and_submit(*biop, bio); /* [한국어] 이전 anchor를 이번 bio에 체인하고 제출, 이번 bio를 다음 anchor로 저장 */

		nr_sects -= len; /* [한국어] 남은 0-채우기 범위를 이번에 처리한 만큼 감소 */
		sector += len; /* [한국어] 다음 bio가 시작할 섹터를 이번에 처리한 만큼 전진 */
		cond_resched(); /* [한국어] 큰 범위를 처리하는 중 스케줄링 포인트 제공 - softlockup 방지 */
	}
}

/*
 * [한국어]
 * blkdev_issue_write_zeroes - 하드웨어 Write Zeroes 오프로드를 발행하고 완료까지 대기
 *
 * @bdev:     대상 블록 디바이스
 * @sector:   0-채우기를 시작할 섹터
 * @nr_sects: 0으로 채울 섹터 수
 * @gfp:      bio 할당 플래그
 * @flags:    BLKDEV_ZERO_* 플래그
 * @return:   0=성공, 디바이스가 실제로는 미지원임이 드러나면 -EOPNOTSUPP,
 *            그 외 submit_bio_wait()/bio_submit_or_kill() 에러
 *
 * 배경: __blkdev_issue_zeroout()이 오프로드 가능(limit != 0)이라고 판단한
 * 경우 호출되는 내부 헬퍼로, 실제 제출과 완료 대기, 그리고 "오프로드가
 * 실행 중 미지원으로 판명되는" 경우의 사후 처리를 담당한다.
 * 동작 순서: (1) bio_write_zeroes_limit()으로 한 bio 최대 길이를 구한다.
 * (2) blk_start_plug()로 플러그를 건다. (3)
 * __blkdev_issue_write_zeroes()로 실제 bio 체인을 만들고 제출한다. (4)
 * 마지막 bio가 있으면 bio_submit_or_kill()로 제출(KILLABLE이면 진입 중
 * kill 가능)하고 완료를 기다린 뒤 참조를 해제한다. (5)
 * blk_finish_plug()로 플러그를 해제한다. (6) SCSI 등 일부 디바이스는
 * 실제 I/O 에러가 나야만 "이 장치는 Write Zeroes를 지원하지 않는다"는
 * 사실이 드러나 bdev_write_zeroes_sectors()가 0으로 갱신되므로, 에러가
 * 있었고 그 갱신이 실제로 일어났다면 호출자에게 -EOPNOTSUPP를 알려줘
 * zero-page 폴백으로 전환하게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, submit_bio_wait 경로에서 sleep 가능.
 * 호출자: blkdev_issue_zeroout()의 첫 번째 시도 경로(bdev_write_zeroes_sectors
 * 가 0이 아닐 때).
 * 피호출자: bio_write_zeroes_limit(), blk_start_plug(),
 * __blkdev_issue_write_zeroes(), bio_submit_or_kill(), blk_finish_plug().
 *
 * 호출 체인:
 *   blkdev_issue_zeroout -> [blkdev_issue_write_zeroes] ->
 *   __blkdev_issue_write_zeroes -> ... -> nvme_queue_rq
 */
static int blkdev_issue_write_zeroes(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp, unsigned flags)
{
	sector_t limit = bio_write_zeroes_limit(bdev); /* [한국어] 이 디바이스에서 한 bio가 담을 수 있는 최대 write-zeroes 섹터 수 (0이면 오프로드 자체가 불가능) */
	struct bio *bio = NULL; /* [한국어] bio 체인 anchor - 아래 __blkdev_issue_write_zeroes()가 채움 */
	struct blk_plug plug; /* [한국어] 이 함수 범위의 bio 제출을 배칭하는 플러그 객체 */
	int ret = 0; /* [한국어] 최종 반환값, 기본 0 */

	blk_start_plug(&plug); /* [한국어] 플러그 시작 - 여러 bio 제출을 모아 배칭 */
	__blkdev_issue_write_zeroes(bdev, sector, nr_sects, gfp, &bio,
			flags, limit); /* [한국어] 실제 write-zeroes bio 체인 생성 및 제출 (호출부는 155~156줄에 걸침) */
	if (bio) { /* [한국어] 최소 한 개 이상의 bio가 만들어졌다면 */
		ret = bio_submit_or_kill(bio, flags); /* [한국어] flags(KILLABLE 등)를 반영해 마지막 bio를 제출/대기하거나 kill 상태면 즉시 에러 완료 */
		bio_put(bio); /* [한국어] 완료된 bio 참조 해제 */
	}
	blk_finish_plug(&plug); /* [한국어] 플러그 해제 - 배칭된 나머지 bio들을 실제로 흘려보냄 */

	/*
	 * For some devices there is no non-destructive way to verify whether
	 * WRITE ZEROES is actually supported.  These will clear the capability
	 * on an I/O error, in which case we'll turn any error into
	 * "not supported" here.
	 */
	/* [한국어] 위 원문 번역: 일부 디바이스는 WRITE ZEROES를 실제로 지원하는지
	 * 비파괴적으로 검증할 방법이 없다. 이런 디바이스는 I/O 에러가 나야
	 * 지원 capability를 클리어하므로, 그 경우 아래에서 에러를 "미지원"으로
	 * 바꿔 돌려준다. */
	if (ret && !bdev_write_zeroes_sectors(bdev)) /* [한국어] 에러가 있었고(ret != 0), 그 사이 드라이버가 write-zeroes 지원 capability를 0으로 내렸다면(=이번 실패로 미지원임이 드러남) */
		return -EOPNOTSUPP; /* [한국어] 호출자(blkdev_issue_zeroout)가 zero-page 폴백 경로로 전환할 수 있도록 EOPNOTSUPP로 통일 */
	return ret; /* [한국어] 그 외의 경우 원래 에러(또는 성공 0)를 그대로 반환 */
}

/*
 * Convert a number of 512B sectors to a number of pages.
 * The result is limited to a number of pages that can fit into a BIO.
 * Also make sure that the result is always at least 1 (page) for the cases
 * where nr_sects is lower than the number of sectors in a page.
 */
/* [한국어] 위 원문 번역: 512바이트 섹터 수를 페이지 수로 변환한다. 결과는
 * 하나의 BIO에 담을 수 있는 페이지 수(BIO_MAX_VECS)로 제한된다. 또한
 * nr_sects가 한 페이지의 섹터 수보다 작은 경우에도 결과가 항상 최소
 * 1(페이지)이 되도록 보장한다(DIV_ROUND_UP 덕분). */
/*
 * [한국어]
 * __blkdev_sectors_to_bio_pages - 섹터 수를 bio가 필요로 하는 페이지(bvec) 수로 환산
 *
 * @nr_sects: 0으로 채울 남은 섹터 수
 * @return:   nr_sects를 담는 데 필요한 페이지 수(최소 1), 단
 *            BIO_MAX_VECS를 넘지 않도록 클램핑된 값
 *
 * 배경: zero-page 폴백 경로(__blkdev_issue_zero_pages)는 실제로 zero
 * folio를 bio에 bvec으로 추가해야 하므로, 이번에 만들 bio가 최대 몇 개의
 * bvec 슬롯을 미리 예약해야 하는지 알아야 bio_alloc()을 호출할 수 있다.
 * 동작 순서: (1) DIV_ROUND_UP_SECTOR_T로 nr_sects(512B 단위)를
 * "페이지 크기에 해당하는 섹터 수(PAGE_SIZE/512)"로 올림 나눗셈해 필요한
 * 페이지 수를 구한다. (2) 그 값과 BIO_MAX_VECS(한 bio가 가질 수 있는
 * 최대 bvec 수) 중 작은 값을 반환해, 이후 bio_alloc()에 과도한 벡터 수를
 * 요청하지 않도록 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 순수 계산 함수.
 * 호출자: __blkdev_issue_zero_pages().
 * 피호출자: DIV_ROUND_UP_SECTOR_T() (매크로).
 *
 * 호출 체인:
 *   __blkdev_issue_zero_pages -> [__blkdev_sectors_to_bio_pages]
 */
static unsigned int __blkdev_sectors_to_bio_pages(sector_t nr_sects)
{
	sector_t pages = DIV_ROUND_UP_SECTOR_T(nr_sects, PAGE_SIZE / 512); /* [한국어] 섹터 수를 "페이지당 섹터 수"로 올림 나눗셈 - 나머지가 있어도 최소 1페이지 이상 확보 */

	return min(pages, (sector_t)BIO_MAX_VECS); /* [한국어] 계산된 페이지 수와 bio가 가질 수 있는 최대 bvec 수(BIO_MAX_VECS) 중 작은 값으로 클램핑 */
}

/*
 * [한국어]
 * __blkdev_issue_zero_pages - 공유 zero 페이지를 데이터로 쓰는 REQ_OP_WRITE bio들을 생성·제출
 *
 * @bdev:     대상 블록 디바이스
 * @sector:   0-채우기를 시작할 섹터
 * @nr_sects: 0으로 채울 섹터 수
 * @gfp_mask: bio/bvec 할당 플래그
 * @biop:     [in,out] bio 체인 anchor
 * @flags:    BLKDEV_ZERO_KILLABLE 등 (NOFALLBACK은 호출자에서 이미 체크됨)
 * @return:   없음(void)
 *
 * 배경: 디바이스가 Write Zeroes 하드웨어 오프로드를 지원하지 않을 때의
 * 최후 수단(fallback) 경로다. 실제로 미리 확보해 둔 "모두 0인 공유
 * 페이지(zero folio)"의 물리 주소를 반복 재사용해 일반 WRITE bio를
 * 만들어 보낸다 - 이 경우 실제 버스/미디어 쓰기 트래픽이 발생한다는 점이
 * 하드웨어 오프로드와의 핵심 차이다.
 * 동작 순서: (1) largest_zero_folio()로 커널이 미리 확보해 둔 최대 크기의
 * 0-채움 folio를 한 번만 얻는다(매 bio마다 새로 할당하지 않고 재사용).
 * (2) nr_sects가 남는 동안 반복하며, 매 반복마다
 * __blkdev_sectors_to_bio_pages()로 이번 bio에 필요한 bvec 수를 구해
 * REQ_OP_WRITE bio를 할당한다. (3) KILLABLE + 치명적 시그널 대기 중이면
 * 루프를 중단한다. (4) 내부 do/while 루프에서 zero_folio를 필요한
 * 만큼(folio 크기 또는 남은 길이 중 작은 값) 반복해서 bio에
 * bio_add_folio()로 추가한다 - bio가 꽉 차 더 추가할 수 없으면(즉
 * 이번에 할당한 bvec 슬롯 소진) 내부 루프를 빠져나온다. (5) 완성된 bio를
 * bio_chain_and_submit()으로 체인·제출한다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blkdev_issue_zero_pages().
 * 피호출자: largest_zero_folio(), __blkdev_sectors_to_bio_pages(),
 * bio_alloc(), bio_add_folio(), bio_chain_and_submit(),
 * fatal_signal_pending(), cond_resched().
 *
 * 호출 체인:
 *   blkdev_issue_zero_pages -> [__blkdev_issue_zero_pages] ->
 *   bio_chain_and_submit -> submit_bio -> ... -> nvme_queue_rq
 */
static void __blkdev_issue_zero_pages(struct block_device *bdev,
		sector_t sector, sector_t nr_sects, gfp_t gfp_mask,
		struct bio **biop, unsigned int flags)
{
	struct folio *zero_folio = largest_zero_folio(); /* [한국어] 커널이 전역으로 미리 0-초기화해 둔 최대 크기 공유 folio 획득 - 매번 새로 0으로 채울 필요 없이 재사용, 물리 주소도 반복 재사용됨 */

	while (nr_sects) { /* [한국어] 남은 0-채우기 범위가 있는 동안 계속 bio 생성 */
		unsigned int nr_vecs = __blkdev_sectors_to_bio_pages(nr_sects); /* [한국어] 이번 bio에 필요한 bvec(페이지) 슬롯 수 계산 */
		struct bio *bio; /* [한국어] 이번 반복에서 새로 할당할 WRITE bio */

		if ((flags & BLKDEV_ZERO_KILLABLE) &&
		    fatal_signal_pending(current)) /* [한국어] 시그널로 중단 가능 요청 + 실제 치명적 시그널 대기 중이면 */
			break; /* [한국어] 남은 범위를 포기하고 루프 탈출 - 이미 체인된 bio들은 정상 제출됨 */

		bio = bio_alloc(bdev, nr_vecs, REQ_OP_WRITE, gfp_mask); /* [한국어] nr_vecs개의 bvec 슬롯을 가진 일반 WRITE bio 할당 - zero_folio를 데이터로 채워 넣을 예정 */
		bio->bi_iter.bi_sector = sector; /* [한국어] 이번 bio의 시작 섹터 설정 */

		do {
			unsigned int len; /* [한국어] 이번 bvec 한 조각에 채울 바이트 길이 */

			len = min_t(sector_t, folio_size(zero_folio),
				    nr_sects << SECTOR_SHIFT); /* [한국어] zero_folio 한 개의 크기와 "남은 길이(바이트로 환산)" 중 작은 값 - 남은 길이가 folio보다 작으면 그 길이만 사용 */
			if (!bio_add_folio(bio, zero_folio, len, 0)) /* [한국어] bio에 zero_folio 조각을 bvec으로 추가 시도 - offset 0에서 len 바이트, 실패하면(bio의 bvec 슬롯 소진) */
				break; /* [한국어] 내부 do/while 루프 탈출 - 이번 bio는 여기까지만 채우고 완성 처리 */
			nr_sects -= len >> SECTOR_SHIFT; /* [한국어] 방금 채운 만큼 남은 섹터 수 감소 */
			sector += len >> SECTOR_SHIFT; /* [한국어] 방금 채운 만큼 다음 시작 섹터 전진 */
		} while (nr_sects); /* [한국어] 아직 남은 범위가 있고 위에서 break하지 않았다면 같은 bio에 zero_folio 조각을 계속 추가 */

		*biop = bio_chain_and_submit(*biop, bio); /* [한국어] 이번에 완성된 bio를 이전 anchor에 체인하고 제출, 다음 anchor로 저장 */
		cond_resched(); /* [한국어] 큰 범위 처리 중 스케줄링 포인트 제공 */
	}
}

/*
 * [한국어]
 * blkdev_issue_zero_pages - 하드웨어 오프로드 없이 zero 페이지로 0-채우기 수행
 *
 * @bdev:     대상 블록 디바이스
 * @sector:   0-채우기를 시작할 섹터
 * @nr_sects: 0으로 채울 섹터 수
 * @gfp:      bio 할당 플래그
 * @flags:    BLKDEV_ZERO_* 플래그
 * @return:   0=성공, BLKDEV_ZERO_NOFALLBACK이 설정돼 있으면 즉시
 *            -EOPNOTSUPP, 그 외 bio_submit_or_kill() 에러
 *
 * 배경: __blkdev_issue_zeroout()이 하드웨어 오프로드를 쓸 수 없다고
 * 판단했을 때(limit == 0) 호출되는 최후 폴백 경로의 진입점이다.
 * 동작 순서: (1) 호출자가 BLKDEV_ZERO_NOFALLBACK으로 "폴백을 쓰지 말라"고
 * 명시했다면 즉시 -EOPNOTSUPP를 반환해 실제 zero-page 쓰기를 수행하지
 * 않는다. (2) 그렇지 않으면 blk_start_plug()로 플러그를 걸고
 * __blkdev_issue_zero_pages()로 실제 WRITE bio들을 생성·제출한다. (3)
 * 마지막 bio가 있으면 bio_submit_or_kill()로 제출/대기하고 참조를
 * 해제한다. (4) blk_finish_plug()로 플러그를 해제한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, sleep 가능.
 * 호출자: blkdev_issue_zeroout()(오프로드가 없거나 실패했을 때의 폴백
 * 경로).
 * 피호출자: blk_start_plug(), __blkdev_issue_zero_pages(),
 * bio_submit_or_kill(), blk_finish_plug().
 *
 * 호출 체인:
 *   blkdev_issue_zeroout -> [blkdev_issue_zero_pages] ->
 *   __blkdev_issue_zero_pages -> ... -> nvme_queue_rq
 */
static int blkdev_issue_zero_pages(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp, unsigned flags)
{
	struct bio *bio = NULL; /* [한국어] bio 체인 anchor */
	struct blk_plug plug; /* [한국어] 이 함수 범위의 bio 제출을 배칭하는 플러그 객체 */
	int ret = 0; /* [한국어] 최종 반환값, 기본 0 */

	if (flags & BLKDEV_ZERO_NOFALLBACK) /* [한국어] 호출자가 "폴백 금지"를 명시한 경우 */
		return -EOPNOTSUPP; /* [한국어] 실제 zero-page 쓰기를 시도하지 않고 즉시 미지원으로 반환 */

	blk_start_plug(&plug); /* [한국어] 플러그 시작 */
	__blkdev_issue_zero_pages(bdev, sector, nr_sects, gfp, &bio, flags); /* [한국어] 실제 zero-page WRITE bio 체인 생성 및 제출 */
	if (bio) { /* [한국어] 최소 한 개 이상의 bio가 만들어졌다면 */
		ret = bio_submit_or_kill(bio, flags); /* [한국어] 마지막 bio 제출/대기(또는 kill 상태 시 즉시 에러 완료) */
		bio_put(bio); /* [한국어] bio 참조 해제 */
	}
	blk_finish_plug(&plug); /* [한국어] 플러그 해제, 배칭된 나머지 bio 흘려보냄 */

	return ret; /* [한국어] 결과 반환 */
}

/*
 * [한국어]
 * __blkdev_issue_zeroout - 하드웨어 오프로드 우선, 실패 시 zero-page 폴백으로 zero-fill bio들을 생성
 *
 * @bdev:     대상 블록 디바이스
 * @sector:   0-채우기를 시작할 섹터
 * @nr_sects: 0으로 채울 섹터 수
 * @gfp_mask: bio 할당 플래그
 * @biop:     [in,out] bio 체인 anchor - 완료 대기는 호출자 책임(이 함수는
 *            제출만 하고 대기하지 않음)
 * @flags:    BLKDEV_ZERO_NOUNMAP/NOFALLBACK/KILLABLE
 * @return:   0=bio 제출 성공(완료 여부와 무관), -EPERM=읽기 전용 디바이스,
 *            -EOPNOTSUPP=오프로드 불가 + NOFALLBACK 설정
 *
 * 배경: blkdev_issue_zeroout()의 "bio만 만들고 대기는 안 하는" 버전으로,
 * block/blk-zoned.c처럼 여러 zone에 대해 zero-fill bio를 누적시킨 뒤 한
 * 번에 대기하고 싶은 호출자를 위해 분리되어 있다.
 * 동작 순서: (1) 디바이스가 read-only면 즉시 -EPERM. (2)
 * bio_write_zeroes_limit()으로 하드웨어 오프로드 가능 여부(limit != 0)를
 * 판단한다. (3) 가능하면 __blkdev_issue_write_zeroes()로 오프로드
 * 경로를 사용한다. (4) 불가능한데 NOFALLBACK이 설정돼 있으면
 * -EOPNOTSUPP를 반환해 zero-page를 쓰지 않는다. (5) 불가능하고
 * NOFALLBACK도 아니면 __blkdev_issue_zero_pages()로 폴백한다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: blkdev_issue_zeroout()(대기까지 포함하는 공개 API),
 * block/blk-zoned.c(zone 상태 전이 시 여러 zone을 zero-fill).
 * 피호출자: bdev_read_only(), bio_write_zeroes_limit(),
 * __blkdev_issue_write_zeroes(), __blkdev_issue_zero_pages().
 *
 * 호출 체인:
 *   blkdev_issue_zeroout -> [__blkdev_issue_zeroout] ->
 *   __blkdev_issue_write_zeroes 또는 __blkdev_issue_zero_pages
 */
/**
 * __blkdev_issue_zeroout - generate number of zero filed write bios
 * @bdev:	blockdev to issue
 * @sector:	start sector
 * @nr_sects:	number of sectors to write
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 * @biop:	pointer to anchor bio
 * @flags:	controls detailed behavior
 *
 * Description:
 *  Zero-fill a block range, either using hardware offload or by explicitly
 *  writing zeroes to the device.
 *
 *  If a device is using logical block provisioning, the underlying space will
 *  not be released if %flags contains BLKDEV_ZERO_NOUNMAP.
 *
 *  If %flags contains BLKDEV_ZERO_NOFALLBACK, the function will return
 *  -EOPNOTSUPP if no explicit hardware offload for zeroing is provided.
 */
int __blkdev_issue_zeroout(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp_mask, struct bio **biop,
		unsigned flags)
{
	sector_t limit = bio_write_zeroes_limit(bdev); /* [한국어] 하드웨어 write-zeroes 오프로드 가능 여부/최대 길이 - 0이면 오프로드 불가 */

	if (bdev_read_only(bdev)) /* [한국어] 대상 디바이스(또는 파티션)가 읽기 전용으로 마운트/설정된 경우 */
		return -EPERM; /* [한국어] 쓰기 계열 요청이므로 권한 없음 에러 즉시 반환 */

	if (limit) { /* [한국어] 하드웨어 오프로드 사용 가능 */
		__blkdev_issue_write_zeroes(bdev, sector, nr_sects,
				gfp_mask, biop, flags, limit); /* [한국어] REQ_OP_WRITE_ZEROES 경로로 실제 데이터 전송 없이 0-채우기 수행 (호출부는 270~271줄에 걸침) */
	} else { /* [한국어] 하드웨어 오프로드 불가능(limit == 0) */
		if (flags & BLKDEV_ZERO_NOFALLBACK) /* [한국어] 호출자가 폴백을 명시적으로 금지했다면 */
			return -EOPNOTSUPP; /* [한국어] zero-page 폴백을 시도하지 않고 미지원 반환 */
		__blkdev_issue_zero_pages(bdev, sector, nr_sects, gfp_mask,
				biop, flags); /* [한국어] zero folio를 실제로 쓰는 REQ_OP_WRITE 경로로 폴백 (호출부는 275~276줄에 걸침) */
	}
	return 0; /* [한국어] bio 제출까지는 성공 - 실제 완료 결과는 호출자가 anchor(*biop)를 통해 별도로 대기해야 함 */
}
EXPORT_SYMBOL(__blkdev_issue_zeroout); /* [한국어] 커널 심볼 공개 - block/blk-zoned.c 등에서 사용 */

/*
 * [한국어]
 * blkdev_issue_zeroout - 블록 범위를 0으로 채우고 완료까지 대기한다
 *
 * @bdev:     대상 블록 디바이스
 * @sector:   0-채우기를 시작할 섹터
 * @nr_sects: 0으로 채울 섹터 수
 * @gfp_mask: bio 할당 플래그
 * @flags:    BLKDEV_ZERO_NOUNMAP/NOFALLBACK/KILLABLE
 * @return:   0=성공, -EINVAL=정렬 위반, -EPERM=읽기 전용,
 *            그 외 write-zeroes/zero-page 경로의 에러
 *
 * 배경: vfs_fallocate(FALLOC_FL_ZERO_RANGE) 등에서 호출되는, "대기까지
 * 포함하는" 공개 zero-fill API다. __blkdev_issue_zeroout()과 달리 이
 * 함수는 오프로드 경로와 zero-page 경로를 각각 완료까지 대기하는 정적
 * 헬퍼(blkdev_issue_write_zeroes/blkdev_issue_zero_pages)를 직접
 * 호출한다(즉 __blkdev_issue_zeroout()을 재사용하지 않고 별도 경로를
 * 탄다는 점에 유의).
 * 동작 순서: (1) (sector | nr_sects)가 논리 블록 크기의 섹터 마스크에
 * 걸리면(=정렬 안 됨) -EINVAL. (2) read-only면 -EPERM. (3)
 * bdev_write_zeroes_sectors()로 오프로드 지원 여부를 먼저 확인하고,
 * 지원하면 blkdev_issue_write_zeroes()를 시도한다 - 이 함수는 내부적으로
 * "실행 중 미지원으로 판명"되는 경우까지 처리해 -EOPNOTSUPP 여부로
 * 최종 판단한다. (4) -EOPNOTSUPP가 아니면(성공 또는 다른 에러) 그
 * 결과를 그대로 반환한다. (5) 오프로드가 아예 없거나(단계 3의 if를 타지
 * 않음) -EOPNOTSUPP였다면 blkdev_issue_zero_pages()로 폴백한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, sleep 가능.
 * 호출자: block/fops.c의 blkdev_fallocate()(FALLOC_FL_ZERO_RANGE),
 * block/blk-zoned.c의 blk_zone_issue_zeroout(), block/ioctl.c(BLKZEROOUT
 * ioctl), fs/*(sb_issue_zeroout).
 * 피호출자: bdev_logical_block_size(), bdev_read_only(),
 * bdev_write_zeroes_sectors(), blkdev_issue_write_zeroes(),
 * blkdev_issue_zero_pages().
 *
 * 호출 체인:
 *   blkdev_fallocate/BLKZEROOUT ioctl -> [blkdev_issue_zeroout] ->
 *   blkdev_issue_write_zeroes 또는 blkdev_issue_zero_pages -> ... ->
 *   nvme_queue_rq
 */
/**
 * blkdev_issue_zeroout - zero-fill a block range
 * @bdev:	blockdev to write
 * @sector:	start sector
 * @nr_sects:	number of sectors to write
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 * @flags:	controls detailed behavior
 *
 * Description:
 *  Zero-fill a block range, either using hardware offload or by explicitly
 *  writing zeroes to the device.  See __blkdev_issue_zeroout() for the
 *  valid values for %flags.
 */
int blkdev_issue_zeroout(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp_mask, unsigned flags)
{
	int ret; /* [한국어] 각 경로(write-zeroes/zero-page)의 결과를 담을 지역 변수 */

	if ((sector | nr_sects) & ((bdev_logical_block_size(bdev) >> 9) - 1)) /* [한국어] 시작 섹터 또는 길이가 논리 블록 크기(섹터 단위) 배수가 아니면(OR 후 마스크 검사로 둘 다 한 번에 확인) */
		return -EINVAL; /* [한국어] 정렬 위반 - 하드웨어가 처리할 수 없는 잘못된 요청이므로 즉시 거부 */
	if (bdev_read_only(bdev)) /* [한국어] 대상이 읽기 전용이면 */
		return -EPERM; /* [한국어] 쓰기 계열 요청 거부 */

	if (bdev_write_zeroes_sectors(bdev)) { /* [한국어] 디바이스가 하드웨어 write-zeroes 오프로드를 지원한다고 보고하면 */
		ret = blkdev_issue_write_zeroes(bdev, sector, nr_sects,
				gfp_mask, flags); /* [한국어] 오프로드 경로 시도 - 성공/실패/실행중 미지원 판명까지 모두 이 안에서 처리됨 (호출부는 306~307줄에 걸침) */
		if (ret != -EOPNOTSUPP) /* [한국어] 결과가 "미지원으로 판명"이 아니라면(성공이거나 복구 불가능한 다른 에러) */
			return ret; /* [한국어] 그 결과를 그대로 호출자에 반환 - 폴백 시도 안 함 */
	}

	return blkdev_issue_zero_pages(bdev, sector, nr_sects, gfp_mask, flags); /* [한국어] 오프로드 미지원(애초에 없었거나 방금 EOPNOTSUPP로 판명)인 경우 zero-page 폴백 경로로 전환 */
}
EXPORT_SYMBOL(blkdev_issue_zeroout); /* [한국어] 커널 심볼 공개 */

/*
 * [한국어]
 * blkdev_issue_secure_erase - 보안 삭제(secure erase) 요청을 bio로 변환해 발행하고 완료까지 대기
 *
 * @bdev:     대상 블록 디바이스
 * @sector:   보안 삭제를 시작할 섹터
 * @nr_sects: 보안 삭제할 섹터 수
 * @gfp:      bio 할당 플래그
 * @return:   0=성공, -EOPNOTSUPP=디바이스가 secure erase 미지원,
 *            -EINVAL=정렬 위반, -EPERM=읽기 전용,
 *            그 외 submit_bio_wait() 에러
 *
 * 배경: BLKSECDISCARD ioctl 등에서 호출되는, 일반 discard보다 강한 보증
 * (실제로 데이터가 복구 불가능하게 지워짐)을 요구하는 삭제 요청의
 * 진입점이다. NVMe에서는 이 REQ_OP_SECURE_ERASE가 Sanitize 또는
 * Format NVM 등 컨트롤러의 보안 소거 기능으로 매핑될 수 있다.
 * 동작 순서: (1) 논리 블록 크기 기반 정렬 마스크(bs_mask)를 만든다. (2)
 * bdev_max_secure_erase_sectors()로 디바이스가 한 커맨드에 처리 가능한
 * 최대 섹터 수를 얻는다. (3) "len << SECTOR_SHIFT" 계산이 오버플로하지
 * 않도록 max_sectors를 BIO_MAX_SECTORS로 상한 클램핑한 뒤, 논리 블록
 * 크기로 내림 정렬한다. (4) 정렬 후 max_sectors가 0이면(=애초에 미지원)
 * -EOPNOTSUPP. (5) 시작/길이가 정렬 안 됐으면 -EINVAL. (6) read-only면
 * -EPERM. (7) blk_start_plug()로 플러그를 걸고, 남은 길이가 있는 동안
 * blk_next_bio()로 REQ_OP_SECURE_ERASE bio를 만들어 이전 bio를 체인·제출
 * 하면서 반복한다. (8) 마지막 bio를 submit_bio_wait()로 대기 후 해제하고
 * blk_finish_plug()로 마무리한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, sleep 가능.
 * 호출자: block/ioctl.c(BLKSECDISCARD ioctl 처리 경로).
 * 피호출자: bdev_logical_block_size(), bdev_max_secure_erase_sectors(),
 * bdev_read_only(), blk_start_plug(), blk_next_bio(), submit_bio_wait(),
 * bio_put(), blk_finish_plug(), cond_resched().
 *
 * 호출 체인:
 *   BLKSECDISCARD ioctl -> [blkdev_issue_secure_erase] -> blk_next_bio ->
 *   bio_chain_and_submit -> submit_bio -> ... -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell, Sanitize/Format NVM 계열 추정)
 */
int blkdev_issue_secure_erase(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp)
{
	sector_t bs_mask = (bdev_logical_block_size(bdev) >> 9) - 1; /* [한국어] 논리 블록 크기(바이트)를 섹터 단위로 변환 후 1을 뺀 정렬 마스크 */
	unsigned int max_sectors = bdev_max_secure_erase_sectors(bdev); /* [한국어] 디바이스가 한 번의 secure-erase 커맨드로 처리 가능한 최대 섹터 수 조회 */
	struct bio *bio = NULL; /* [한국어] bio 체인 anchor */
	struct blk_plug plug; /* [한국어] 이 함수 범위의 bio 제출 배칭용 플러그 객체 */
	int ret = 0; /* [한국어] 최종 반환값, 기본 0 */

	/* [한국어] 위 원문 번역: "len << SECTOR_SHIFT" 연산이 오버플로하지 않도록
	 * 보장한다. bi_size는 표현 범위가 한정되므로, max_sectors를 미리
	 * BIO_MAX_SECTORS로 제한해 두어야 아래 bio->bi_iter.bi_size 대입에서
	 * 값이 넘치지 않는다. */
	/* make sure that "len << SECTOR_SHIFT" doesn't overflow */
	if (max_sectors > BIO_MAX_SECTORS) /* [한국어] 디바이스가 보고한 한도가 bio가 표현 가능한 최댓값보다 크면 */
		max_sectors = BIO_MAX_SECTORS; /* [한국어] bio 표현 한계로 클램핑 */
	max_sectors &= ~bs_mask; /* [한국어] 논리 블록 크기의 배수로 내림 정렬 - 정렬 안 된 최대치로 인해 마지막 조각이 비정렬되는 것을 방지 */

	if (max_sectors == 0) /* [한국어] 정렬 결과 0이 됐다면(디바이스가 애초에 secure erase를 지원하지 않아 한계가 0이었거나, 한계가 논리 블록 크기보다 작았던 경우) */
		return -EOPNOTSUPP; /* [한국어] secure erase 미지원으로 판단해 즉시 반환 */
	if ((sector | nr_sects) & bs_mask) /* [한국어] 시작 섹터 또는 길이가 논리 블록 크기에 정렬돼 있지 않으면 */
		return -EINVAL; /* [한국어] 정렬 위반으로 거부 */
	if (bdev_read_only(bdev)) /* [한국어] 대상이 읽기 전용이면 */
		return -EPERM; /* [한국어] 쓰기/삭제 계열 요청 거부 */

	blk_start_plug(&plug); /* [한국어] 플러그 시작 - bio 제출 배칭 */
	while (nr_sects) { /* [한국어] 남은 삭제 범위가 있는 동안 계속 bio 생성 */
		unsigned int len = min_t(sector_t, nr_sects, max_sectors); /* [한국어] 남은 길이와 한 커맨드 최대 길이 중 작은 값으로 이번 bio 길이 결정 */

		bio = blk_next_bio(bio, bdev, 0, REQ_OP_SECURE_ERASE, gfp); /* [한국어] 이전 bio(있다면)를 체인·제출하고, 데이터 벡터 없는 새 REQ_OP_SECURE_ERASE bio를 할당해 반환받음 */
		bio->bi_iter.bi_sector = sector; /* [한국어] 이번 bio의 시작 섹터 설정 */
		bio->bi_iter.bi_size = len << SECTOR_SHIFT; /* [한국어] 이번 bio 길이를 섹터에서 바이트로 변환해 설정 - 위에서 오버플로 방지 처리를 했으므로 안전 */

		sector += len; /* [한국어] 다음 bio가 시작할 섹터를 전진 */
		nr_sects -= len; /* [한국어] 남은 삭제 범위를 이번에 처리한 만큼 감소 */
		cond_resched(); /* [한국어] 큰 범위 처리 중 스케줄링 포인트 제공 */
	}
	if (bio) { /* [한국어] 최소 한 개 이상의 bio가 만들어졌다면(=nr_sects가 애초에 0보다 컸다면) */
		ret = submit_bio_wait(bio); /* [한국어] 마지막 bio(및 체인된 하위 bio 전체)의 완료를 동기 대기 */
		bio_put(bio); /* [한국어] bio 참조 해제 */
	}
	blk_finish_plug(&plug); /* [한국어] 플러그 해제, 배칭된 나머지 bio 흘려보냄 */

	return ret; /* [한국어] 결과 반환 - nr_sects가 애초에 0이었다면 bio가 하나도 안 만들어져 ret=0 그대로 반환됨 */
}
EXPORT_SYMBOL(blkdev_issue_secure_erase); /* [한국어] 커널 심볼 공개 - BLKSECDISCARD ioctl 등에서 사용 */
