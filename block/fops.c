// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 1991, 1992  Linus Torvalds
 * Copyright (C) 2001  Andrea Arcangeli <andrea@suse.de> SuSE
 * Copyright (C) 2016 - 2020 Christoph Hellwig
 */
#include <linux/init.h>		/* [한국어] __init/module_init() 매크로 - blkdev_init()을 부트/모듈 로드 시점 1회 실행 함수로 등록하기 위해 필요 */
#include <linux/mm.h>		/* [한국어] 폴리오/페이지 관리 API(folio_put 등) - direct I/O가 잡은 사용자 페이지 및 페이지 캐시 폴리오 처리에 필요 */
#include <linux/blkdev.h>	/* [한국어] struct block_device, bio_*, blk_opf_t, bdev_* 헬퍼 등 block layer 핵심 타입/함수 선언 - 이 파일 전체가 의존하는 필수 헤더 */
#include <linux/blk-integrity.h> /* [한국어] bio_integrity_map_iter()/bio_integrity_unmap_user() - NVMe PI(Protection Information)/DIF 메타데이터를 bio에 매핑하기 위해 필요 */
#include <linux/buffer_head.h>	/* [한국어] struct buffer_head, block_read_full_folio() 등 - CONFIG_BUFFER_HEAD 빌드에서 buffer_head 기반 페이지 캐시 I/O를 위해 필요 */
#include <linux/mpage.h>	/* [한국어] mpage_readahead() - buffer_head 기반으로 여러 folio를 한 번에 readahead하는 헬퍼를 위해 필요 */
#include <linux/uio.h>		/* [한국어] struct iov_iter, iov_iter_count()/iov_iter_rw() 등 - 사용자 buffer <-> bio 변환의 핵심 반복자(iterator) API */
#include <linux/namei.h>	/* [한국어] 경로 이름 조회 관련 선언 - 파일 open 경로 처리에서 간접적으로 참조되는 VFS 네임스페이스 헬퍼 */
#include <linux/task_io_accounting_ops.h> /* [한국어] task_io_account_write() - 현재 태스크 단위로 쓰기 바이트 수를 회계 처리하기 위해 필요 */
#include <linux/falloc.h>	/* [한국어] FALLOC_FL_KEEP_SIZE 등 fallocate 모드 플래그 정의 - blkdev_fallocate()가 모드를 해석하는 데 필요 */
#include <linux/suspend.h>	/* [한국어] is_hibernate_resume_dev() - 하이버네이션 재개 대상 장치의 스왑파일 쓰기를 예외 허용하기 위해 필요 */
#include <linux/fs.h>		/* [한국어] struct file, struct kiocb, struct file_operations 등 VFS 핵심 타입 - 이 파일이 구현하는 콜백들의 시그니처 근간 */
#include <linux/iomap.h>	/* [한국어] struct iomap, iomap_writepages() 등 - CONFIG_BUFFER_HEAD 미사용 시 iomap 기반 페이지 캐시 I/O 경로에 필요 */
#include <linux/module.h>	/* [한국어] module_init() 매크로 및 모듈 메타데이터 지원 - blkdev_init()을 초기화 루틴으로 등록하는 데 필요 */
#include <linux/io_uring/cmd.h> /* [한국어] io_uring 커맨드 인프라 선언 - def_blk_fops.uring_cmd(blkdev_uring_cmd)를 통한 NVMe passthrough 명령 경로에 필요 */
#include "blk.h"		/* [한국어] block/ 서브시스템 내부 전용(비공개) 선언 - block layer 내부 헬퍼를 이 파일에서 사용하기 위해 포함 */

/*
 * [한국어 설명] block_device 노드의 struct file_operations/address_space_operations 구현 (fops.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 /dev/sda, /dev/nvme0n1 같은 블록 장치 특수 파일(block special
 * file)을 유저스페이스에서 open(2)/read(2)/write(2)/fsync(2)/fallocate(2)/
 * mmap(2)/ioctl(2) 할 때 커널이 실행하는 VFS(Virtual File System) 진입점을
 * 구현한다. 즉 struct file_operations def_blk_fops 및 struct
 * address_space_operations def_blk_aops의 실제 콜백 함수 본체가 정의되는
 * 곳이다. O_DIRECT read/write는 이 파일의 blkdev_direct_IO 계열 함수들을
 * 통해 곧바로 bio(block I/O)로 조립되고, buffered read/write는 페이지
 * 캐시를 경유하는 blkdev_read_folio/blkdev_writepages 등을 통해 처리된다.
 * fsync/fallocate/llseek 등 파일 단위 보조 연산 역시 모두 이 파일에서
 * block_device 단위의 연산으로 변환된다. 요약하면 "VFS 시스템 콜 <-> bio"
 * 사이의 번역 계층이며, NVMe 등 실제 드라이버 명령은 만들지 않고 항상
 * bio 형태로 하위 block layer에 위임한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 유저 프로세스의 read(2)/write(2)/pread(2)/preadv2(2) 시스템 콜은 VFS의
 * vfs_read()/vfs_write() -> kiocb->ki_filp->f_op->read_iter/write_iter
 * 경로를 거쳐 이 파일의 blkdev_read_iter()/blkdev_write_iter()에 도달한다.
 * O_DIRECT가 설정된 경우 blkdev_direct_IO() -> (요청 크기/플래그에 따라)
 * __blkdev_direct_IO_simple() / __blkdev_direct_IO_async() /
 * __blkdev_direct_IO() 중 하나가 bio를 조립하여 submit_bio()를 호출하고,
 * submit_bio() -> blk_mq_submit_bio()(block/blk-mq.c) -> request 할당/병합
 * -> dispatch 의 순서로 진행된다. 여기서 블록 계층이 드라이버에 닿는 유일한
 * 지점은 q->mq_ops->queue_rq() 라는 **함수 포인터 간접 호출**이며, 이 파일
 * (그리고 block/ 전체)에는 nvme_* 심볼을 직접 부르는 코드가 없다. NVMe
 * PCIe 트랜스포트에서는 그 간접 호출의 실체가 drivers/nvme/host/pci.c 의
 * nvme_queue_rq() 이고, 거기서 nvme_setup_cmd()가 struct request 를 64바이트
 * SQE 로 바꾼 뒤 SQ 에 복사하고 doorbell 을 기록한다(배치 제출 경로는
 * nvme_queue_rqs() -> nvme_submit_cmds()).
 * 완료는 드라이버 완료 경로가 bio_endio()를 호출하면서 이 파일의
 * blkdev_bio_end_io()/blkdev_bio_end_io_async() 콜백으로 되돌아 온다.
 * buffered I/O는 이 파일의 def_blk_aops를 통해 페이지 캐시(mm/filemap.c)와
 * 연결되고, 캐시 미스/writeback 시점에 역시 submit_bio()를 거쳐 같은 경로로
 * 흘러간다. 실행 컨텍스트는 시스템 콜을 발생시킨 유저 프로세스의 커널
 * 스레드 컨텍스트가 대부분이며, 완료 콜백(blkdev_bio_end_io류)은 드라이버의
 * 완료 인터럽트 하단부(softirq) 또는 폴링을 수행한 태스크 컨텍스트에서
 * 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * - block/bio.c: bio_alloc_bioset()/bio_init()/bio_iov_iter_get_pages() 등
 *   bio 생성과 사용자 페이지를 bio_vec으로 매핑하는 하위 API를 제공한다.
 * - block/blk-core.c, block/blk-mq.c: submit_bio()/submit_bio_wait()가 이
 *   파일에서 만든 bio를 실제 요청 큐에 밀어넣고 blk_mq_submit_bio()를 통해
 *   드라이버(NVMe)까지 전달한다.
 * - block/bdev.c: bdev_open()/bdev_release()/bdev_permission()이
 *   blkdev_open()/blkdev_release()에서 호출되어 block_device 참조 카운트와
 *   접근 권한(exclusive open 등)을 관리한다.
 * - block/ioctl.c: blkdev_ioctl()/compat_blkdev_ioctl()이 def_blk_fops의
 *   .unlocked_ioctl/.compat_ioctl 콜백으로 연결된다.
 * - io_uring 서브시스템: blkdev_uring_cmd()(block/ioctl.c 계열에 정의)가
 *   .uring_cmd로 연결되어, 드라이버가 제공하는 passthrough ioctl(NVMe라면
 *   NVME_IOCTL_IO_CMD 계열)을 io_uring 으로 발행하는 경로를 만든다.
 * - fs/iomap/, mm/filemap.c: buffered read/write/writeback이 iomap 또는
 *   buffer_head 계층을 통해 페이지 캐시와 상호작용한다.
 * - 데이터 흐름: 유저 buffer(iov_iter) -> bio_vec(물리 페이지 목록) -> bio
 *   -> struct request(blk-mq) -> mq_ops->queue_rq -> (드라이버가 만든 명령)
 *   -> (완료) bio_endio() -> kiocb->ki_complete()/blk_wake_io_task()
 *   -> 유저 buffer 반영/완료 통지.
 * - NVMe 독자를 위한 실제 접점(코드로 확인된 것만):
 *   (1) 정렬 — 이 파일의 blkdev_dio_invalid()/blkdev_fallocate()가 쓰는
 *       bdev_logical_block_size()는 NVMe에서 1 << ns->head->lba_shift 이고,
 *       lba_shift 는 Identify Namespace 의 LBA Format 디스크립터 LBADS 필드
 *       (id->lbaf[lbaf].ds)에서 온다(drivers/nvme/host/core.c:2799, 2488).
 *       512e 드라이브는 LBADS=9 라 512B 정렬만 요구하고, 4Kn 포맷은
 *       LBADS=12 라 O_DIRECT offset/len 이 4096B 배수가 아니면 여기서
 *       -EINVAL 로 걸린다.
 *   (2) FUA — dio_bio_write_op()가 붙이는 REQ_FUA 는 nvme_setup_rw()에서
 *       NVME_RW_FUA control 비트가 된다(core.c:1273).
 *   (3) 폴링 — IOCB_HIPRI -> REQ_POLLED 는 blk_poll -> mq_ops->poll 로
 *       이어지고, NVMe PCIe 에서는 nvme_poll()(pci.c:2183)이 폴링 전용 큐의
 *       CQ 를 직접 훑는다.
 *   (4) 원자적 쓰기 — IOCB_ATOMIC -> REQ_ATOMIC 은 nvme_valid_atomic_write()
 *       (core.c:1235)가 NAWUPF/AWUPF/NABSPF 로부터 계산된 큐 한도와 대조한다.
 *   (5) write stream — bio->bi_write_stream 은 nvme_setup_rw()에서 FDP
 *       placement identifier 로 변환되어 DSM 필드 상위 16비트에 실린다
 *       (core.c:1282-1288). Directive Type 1(Streams)이 아니라 FDP 다.
 *   (6) fallocate — REQ_OP_WRITE_ZEROES 는 nvme_setup_write_zeroes()가
 *       nvme_cmd_write_zeroes 로, REQ_OP_DISCARD 는 nvme_setup_discard()가
 *       Dataset Management + NVME_DSMGMT_AD(Deallocate)로 변환한다.
 * - 공유 자료구조: struct blkdev_dio(이 파일에서 정의)가 분할된 여러 bio의
 *   완료를 집계하는 핵심 컨테이너이며, struct bio/struct kiocb/struct
 *   iov_iter는 block/bio.c, fs/*, mm/filemap.c와 공유하는 표준 커널
 *   자료구조다.
 *
 * === 주요 함수/구조체 요약 ===
 * - blkdev_direct_IO(): O_DIRECT read/write의 단일 진입점. 요청 크기와
 *   플래그(IOCB_HAS_METADATA/IOCB_ATOMIC 등)에 따라 simple/async/split
 *   경로 중 하나로 분기한다.
 * - __blkdev_direct_IO_simple(): 스택 bio 하나로 처리하는 동기(sync) direct I/O.
 * - __blkdev_direct_IO_async(): 단일 bio, 비동기(-EIOCBQUEUED) direct I/O.
 * - __blkdev_direct_IO(): BIO_MAX_VECS를 초과하는 대형 I/O를 여러 bio로
 *   분할 제출하고 struct blkdev_dio로 완료를 집계하는 direct I/O 경로.
 * - blkdev_read_iter()/blkdev_write_iter(): read_iter/write_iter 콜백의
 *   실제 구현으로, O_DIRECT/buffered 분기와 크기·정렬 검증을 수행한다.
 * - blkdev_open()/blkdev_release(): 블록 장치 노드 open/close 및 bdev
 *   참조 획득/해제.
 * - blkdev_fsync()/blkdev_fallocate(): 각각 REQ_OP_FLUSH bio 와
 *   REQ_OP_WRITE_ZEROES/REQ_OP_DISCARD bio 를 만들어 내는 파일 단위 보조
 *   연산(NVMe 드라이버에서는 각각 Flush 옵코드 0x00, Write Zeroes, Dataset
 *   Management Deallocate 로 번역된다).
 * - struct blkdev_dio: 분할된 direct I/O의 완료를 모으는 컨테이너.
 *   iocb/waiter(비동기/동기 완료 대상 유니온), size(누적 전송 바이트),
 *   ref(미완료 bio 개수), flags(DIO_SHOULD_DIRTY/DIO_IS_SYNC),
 *   bio(내장된 첫 bio, blkdev_dio_pool에서 함께 할당됨) 필드로 구성된다.
 * - def_blk_fops/def_blk_aops: 이 파일의 모든 콜백을 VFS/페이지 캐시에
 *   연결하는 최종 디스패치 테이블.
 */

/*
 * [한국어]
 * bdev_file_inode - struct file에서 block device의 inode를 추출
 *
 * @file: block 장치 노드에 대해 open된 struct file. blkdev_open()이
 *        성공적으로 리턴한 뒤 VFS가 유지하는 파일 객체.
 * @return: 해당 block_device를 감싸는 inode 포인터.
 *
 * block 장치 파일은 일반 파일과 달리 자신만의 address_space를 소유하지
 * 않고 bdev의 bd_inode->i_mapping을 공유한다. 따라서 file->f_mapping은
 * 이미 bdev inode의 address_space를 가리키고 있으므로, 그 mapping->host를
 * 따라가면 다시 bdev inode 자신으로 돌아온다. 이 함수가 없으면 llseek 등에서
 * 장치 크기(i_size)를 얻기 위해 file->f_inode를 직접 쓸 수 없는 경우를
 * 매번 별도로 처리해야 한다. 단순 포인터 역참조만 수행하므로 락/원자 연산이
 * 필요 없고, 재진입 가능하며 어떤 컨텍스트에서 호출해도 안전하다.
 * 호출자: blkdev_llseek, blkdev_write_iter, blkdev_fallocate,
 *         blkdev_mmap_prepare 등 이 파일 내 다수의 함수.
 * 호출 대상: 없음(구조체 필드 역참조만 수행).
 * 호출 체인:
 *   blkdev_llseek/blkdev_write_iter 등 -> [bdev_file_inode] -> (반환된 inode로 i_size_read 등 수행)
 */
static inline struct inode *bdev_file_inode(struct file *file)
{
	return file->f_mapping->host; /* [한국어] file->f_mapping 은 bdev inode 의 address_space 이므로 그 host 를 따라가면 bdev inode 자신이 반환된다 - 장치 노드 파일은 자기 mapping 을 갖지 않고 bdev inode 의 것을 공유하기 때문 */
}

/*
 * [한국어]
 * dio_bio_write_op - direct write 요청에 사용할 bio의 opf(operation + flags) 조합을 결정
 *
 * @iocb: 쓰기를 요청한 kiocb. iocb_is_dsync()로 O_DSYNC/O_SYNC 등 동기
 *        쓰기 여부를 판정하는 데 사용된다.
 * @return: REQ_OP_WRITE | REQ_SYNC | REQ_IDLE을 기본값으로 하고, iocb가
 *          데이터 동기화 쓰기(IOCB_DSYNC)를 요구하면 REQ_FUA를 추가로
 *          설정한 blk_opf_t 값.
 *
 * O_DIRECT 쓰기 bio에 공통으로 적용할 연산+플래그 조합을 한 곳에서
 * 결정하기 위한 헬퍼다. REQ_SYNC/REQ_IDLE은 I/O 스케줄러에게 "이 쓰기는
 * 지연에 민감하니 배칭하지 말고 최대한 빨리 흘려보내라"는 힌트를 준다.
 * IOCB_DSYNC(=O_DSYNC 또는 O_SYNC로 열린 파일의 쓰기, 혹은 RWF_DSYNC)가
 * 설정된 경우 REQ_FUA(Force Unit Access)를 추가한다. FUA 를 쓰면 쓰기마다
 * 별도의 flush 요청을 뒤따라 보내는 대신 단일 요청으로 내구성을 보장할 수
 * 있어 "I/O 완료 후 별도 work item(작업 항목)"을 둘 필요가 없어진다(원본
 * 영어 주석이 말하는 바). REQ_FUA 를 드라이버가 실제로 처리하는지는 큐
 * 기능 비트(BLK_FEAT_FUA)에 달려 있고, FUA 를 지원하지 않는 큐에서는
 * block/blk-flush.c 가 이 요청을 write + flush 시퀀스로 풀어준다.
 * NVMe 접점(코드 확인됨): nvme_setup_rw()가 req->cmd_flags & REQ_FUA 를
 * 보고 SQE 의 control 필드에 NVME_RW_FUA 비트를 세운다
 * (drivers/nvme/host/core.c:1273-1274). BLK_FEAT_FUA 자체는 컨트롤러가
 * 휘발성 쓰기 캐시(VWC)를 보고할 때만 켜진다(core.c:2813). 실행 컨텍스트: direct write 시스템 콜을
 * 처리하는 유저 프로세스의 커널 컨텍스트에서 동기적으로 호출되며 별도
 * 동기화가 필요 없다(단순 계산 함수).
 * 호출자: __blkdev_direct_IO_simple, __blkdev_direct_IO,
 *         __blkdev_direct_IO_async (모두 iov_iter_rw(iter) == WRITE일 때).
 * 호출 대상: iocb_is_dsync() (fs/*.h 인라인 헬퍼).
 * 에러 경로: 없음(항상 유효한 opf를 반환).
 * 호출 체인:
 *   __blkdev_direct_IO*(WRITE 방향) -> [dio_bio_write_op] -> bio_init()/bio_alloc_bioset()의 opf 인자로 전달 -> submit_bio -> blk_mq_submit_bio -> mq_ops->queue_rq (NVMe면 nvme_queue_rq -> nvme_setup_rw)
 */
static blk_opf_t dio_bio_write_op(struct kiocb *iocb)
{
	blk_opf_t opf = REQ_OP_WRITE | REQ_SYNC | REQ_IDLE; /* [한국어] 기본 쓰기 연산에 REQ_SYNC(지연에 민감함)와 REQ_IDLE(이 태스크는 곧 유휴 상태가 됨) 힌트를 얹는다 - 둘 다 I/O 스케줄러가 참고하는 값이지 장치로 내려가는 비트가 아니다 */

	/* avoid the need for a I/O completion work item */
	if (iocb_is_dsync(iocb)) /* [한국어] O_DSYNC/O_SYNC 등 데이터 동기화가 필요한 쓰기인지 검사 */
		opf |= REQ_FUA; /* [한국어] IOCB_DSYNC 이면 FUA(Force Unit Access) 추가 - 이 쓰기 하나만으로 내구성을 확보하고 뒤이은 flush 를 생략한다. NVMe 에서는 nvme_setup_rw()가 control 에 NVME_RW_FUA 비트를 세운다(core.c:1274) */
	return opf; /* [한국어] 완성된 opf를 호출자에게 반환하여 bio 생성 시 사용 */
}

/*
 * [한국어]
 * blkdev_dio_invalid - 사용자 direct I/O 요청의 논리 블록 정렬(alignment) 검사
 *
 * @bdev: 대상 block_device. bdev_logical_block_size()로 이 장치의
 *        논리 블록 크기(NVMe라면 통상 512B 또는 4KiB LBA 크기)를 얻는다.
 * @iocb: 요청을 담은 kiocb. ki_pos(시작 offset)를 정렬 검사에 사용한다.
 * @iter: 전송할 데이터를 표현하는 iov_iter. iov_iter_count()로 전송
 *        길이를 얻는다.
 * @return: true면 offset 또는 길이가 논리 블록 크기로 정렬되지 않아
 *          요청이 유효하지 않음, false면 정렬이 맞아 유효함.
 *
 * 블록 장치는 논리 블록(logical block)보다 작은 단위를 주소지정할 수
 * 없다. O_DIRECT 는 페이지 캐시를 건너뛰고 사용자 버퍼를 그대로 장치에
 * 실어 보내므로, 부분 블록을 채워 줄 read-modify-write 를 해 줄 계층이
 * 없다. 그래서 시작 offset 과 길이가 둘 다 논리 블록 크기의 배수여야
 * 하며, 아니면 여기서 미리 -EINVAL 로 잘라낸다.
 * NVMe 접점(코드 확인됨): 여기서 쓰는 bdev_logical_block_size()는 NVMe
 * 에서 1 << ns->head->lba_shift 이고(drivers/nvme/host/core.c:2451, 2488),
 * lba_shift 는 Identify Namespace 의 LBA Format 디스크립터에서 그대로
 * 복사된다 — ns->head->lba_shift = id->lbaf[lbaf].ds (core.c:2799).
 * 여기서 ds 가 NVMe 스펙의 LBADS(LBA Data Size, 2의 지수) 필드다.
 * 따라서 512e(LBADS=9) 네임스페이스에서는 512B 배수면 통과하지만,
 * 4Kn(LBADS=12) 로 포맷된 같은 SSD 에서는 512B 오프셋 요청이 이 한 줄에
 * 걸려 -EINVAL 이 된다. 벤치마크가 "같은 드라이브인데 blocksize=512 로만
 * 실패한다"고 할 때 대개 원인이 여기다. offset과 길이를
 * OR(|) 연산으로 합친 뒤 (블록 크기-1) 마스크와 AND(&) 연산하는 방식으로,
 * 두 값 중 하나라도 하위 비트가 0이 아니면(=정렬 안 됨) 참을 반환하는
 * 비트 트릭을 사용한다. 실행 컨텍스트: direct I/O 진입 경로에서 동기
 * 호출되며 부작용이 없는 순수 계산 함수다.
 * 호출자: blkdev_direct_IO (O_DIRECT read/write 진입 시 최초 검증 단계).
 * 호출 대상: iov_iter_count(), bdev_logical_block_size() (인라인 헬퍼).
 * 에러 경로: true 반환 시 호출자(blkdev_direct_IO)가 -EINVAL로 시스템
 * 콜을 실패시킨다.
 * 호출 체인:
 *   blkdev_direct_IO -> [blkdev_dio_invalid] -> (true면) return -EINVAL
 */
static bool blkdev_dio_invalid(struct block_device *bdev, struct kiocb *iocb,
				struct iov_iter *iter)
{
	return (iocb->ki_pos | iov_iter_count(iter)) & /* [한국어] 시작 offset과 전송 길이를 OR로 합쳐 하위 정렬 비트를 한번에 검사 - 하나라도 미정렬이면 결과가 0이 아님 */
			(bdev_logical_block_size(bdev) - 1); /* [한국어] 논리 블록 크기-1 = 정렬 마스크(예: 4KiB면 0xFFF); NVMe LBA 정렬 기준 */
}

/*
 * [한국어]
 * blkdev_iov_iter_get_pages - 사용자 버퍼의 물리 페이지를 bio_vec으로 연결
 *
 * @bio: 페이지를 매핑해 넣을 대상 bio. 아직 bi_iter.bi_size가 채워지지
 *       않은, 방금 초기화/할당된 bio.
 * @iter: 사용자(또는 커널) buffer를 표현하는 iov_iter.
 * @bdev: 대상 block_device. 논리 블록 크기를 정렬 마스크로 사용한다.
 * @return: 0이면 성공(bio에 bvec이 채워짐), 음수 errno면 페이지 고정
 *          (pin)/매핑 실패.
 *
 * bio_iov_iter_get_pages()를 얇게 감싼 래퍼로, iter가 가리키는 사용자
 * 메모리의 물리 페이지들을 get_user_pages() 계열로 고정(pin)하고 bio의
 * bvec 배열에 채워 넣는다. 이렇게 구성된 bvec 목록이 하위 계층에서
 * 세그먼트로 병합된 뒤 드라이버의 DMA 서술자로 변환된다 - NVMe PCIe 라면
 * PRP(Physical Region Page) 리스트나 SGL(Scatter Gather List)이 그것이다. 정렬 마스크로 bdev_logical_block_size(bdev) - 1을
 * 넘기는 이유는, 페이지 경계뿐 아니라 논리 블록 경계에서도 세그먼트가
 * 쪼개지도록 강제하기 위함이다(부분 블록 DMA를 방지). 실행 컨텍스트:
 * direct I/O 조립 경로에서 동기 호출되며, 페이지 고정 중 페이지 폴트가
 * 발생할 수 있어 슬립 가능한 컨텍스트에서만 호출해야 한다.
 * 호출자: __blkdev_direct_IO_simple, __blkdev_direct_IO,
 *         __blkdev_direct_IO_async.
 * 호출 대상: bio_iov_iter_get_pages() (block/bio.c).
 * 에러 경로: 실패 시 호출자가 즉시 정리 후 에러를 반환하거나(단순 경로),
 *           분할 경로에서는 bio->bi_status를 BLK_STS_IOERR로 설정 후
 *           bio_endio()로 종료한다.
 * 호출 체인:
 *   __blkdev_direct_IO* -> [blkdev_iov_iter_get_pages] -> bio_iov_iter_get_pages -> (bio->bi_io_vec 채움)
 */
static inline int blkdev_iov_iter_get_pages(struct bio *bio,
		struct iov_iter *iter, struct block_device *bdev)
{
	return bio_iov_iter_get_pages(bio, iter, /* [한국어] 유저 버퍼의 페이지들을 pin 하고 bio_vec 배열로 엮는다 - 이 bvec 목록이 나중에 드라이버가 DMA 서술자를 만드는 원본이 된다(NVMe PCIe 라면 PRP 리스트 또는 SGL) */
			bdev_logical_block_size(bdev) - 1); /* [한국어] 논리 블록 크기-1을 정렬 마스크로 전달해 블록 경계에서도 세그먼트 분할 */
}

#define DIO_INLINE_BIO_VECS 4 /* [한국어] 작은 요청(<=4 페이지)에 대해 힙 할당 없이 스택 배열로 처리하기 위한 bio_vec 개수 상한. 장치 특성과 무관한 순수 소프트웨어 최적화 상수다 */

/*
 * [한국어]
 * __blkdev_direct_IO_simple - 단일 bio + 스택 bio_vec으로 처리하는 동기(sync) direct I/O
 *
 * @iocb: 동기(is_sync_kiocb) direct read/write 요청을 담은 kiocb.
 * @iter: 전송할 사용자 buffer를 표현하는 iov_iter (READ면 채울 대상,
 *        WRITE면 읽어갈 원본).
 * @bdev: 대상 block_device.
 * @nr_pages: 이 요청에 필요한 bio_vec(페이지) 개수. blkdev_direct_IO가
 *            BIO_MAX_VECS 이하로 계산해 전달한다.
 * @return: 성공 시 전송된 바이트 수(ssize_t), 실패 시 음수 errno.
 *
 * 목적: 요청이 작아(nr_pages <= BIO_MAX_VECS) 단일 bio 하나로 표현 가능하고
 * 동기 kiocb인 경우, 스택에 struct bio를 선언하고 submit_bio_wait()로
 * 제출/완료를 한 함수 안에서 끝내는 가장 가벼운 direct I/O 경로다. 힙에서
 * struct blkdev_dio를 할당하는 오버헤드 없이 함수 스택 프레임만으로 처리해
 * 소규모 O_DIRECT read/write의 지연을 최소화한다. nr_pages가
 * DIO_INLINE_BIO_VECS(4) 이하이면 bio_vec 배열도 스택에 두어 힙 할당을
 * 완전히 피한다. 실행 컨텍스트: 동기 read/write 시스템 콜을 처리하는 유저
 * 프로세스의 커널 스레드 컨텍스트에서 호출되며, submit_bio_wait() 내부에서
 * 완료 통지를 기다리는 동안 슬립한다.
 * 호출자: blkdev_direct_IO (is_sync_kiocb(iocb) && nr_pages <= BIO_MAX_VECS
 *         이고 메타데이터가 없는 경우).
 * 호출 대상: bio_init, blkdev_iov_iter_get_pages, submit_bio_wait,
 *           bio_release_pages, blk_status_to_errno, bio_uninit.
 * 에러 경로: 페이지 확보 실패 시 out 레이블로 점프해 bio만 정리 후 errno
 * 반환. bio.bi_status 가 설정되어 있으면(=드라이버가 실패를 보고) errno 로
 * 변환해 반환값에 반영.
 * 호출 체인:
 *   blkdev_direct_IO -> [__blkdev_direct_IO_simple] -> submit_bio_wait -> submit_bio -> blk_mq_submit_bio -> (mq_ops->queue_rq 간접 호출; NVMe PCIe면 nvme_queue_rq)
 *   (완료) 드라이버 완료 경로 -> bio_endio -> submit_bio_wait 내부 대기 해제
 */
static ssize_t __blkdev_direct_IO_simple(struct kiocb *iocb,
		struct iov_iter *iter, struct block_device *bdev,
		unsigned int nr_pages)
{
	struct bio_vec inline_vecs[DIO_INLINE_BIO_VECS], *vecs; /* [한국어] 스택용 고정 배열과, 실제 사용할 bio_vec 포인터(스택 또는 힙) */
	loff_t pos = iocb->ki_pos; /* [한국어] 파일(=장치) 상의 시작 오프셋(바이트) - 아래에서 512B 섹터 번호로 환산된다 */
	bool should_dirty = false; /* [한국어] READ 완료 후 사용자 페이지를 dirty로 표시할지 여부 - 기본 false, 사용자 버퍼 READ일 때만 true로 전환 */
	struct bio bio; /* [한국어] 힙 할당 없이 함수 스택에 직접 선언한 단일 bio - 이 함수 종료 전 반드시 bio_uninit()으로 정리 */
	ssize_t ret; /* [한국어] 반환값(성공 시 전송 바이트 수, 실패 시 음수 errno)을 누적할 변수 */

	if (nr_pages <= DIO_INLINE_BIO_VECS) /* [한국어] 작은 요청(4페이지 이하)은 스택 배열 사용 - 힙 할당 회피 */
		vecs = inline_vecs; /* [한국어] 스택에 선언된 inline_vecs를 그대로 사용 */
	else { /* [한국어] 4페이지 초과 요청은 힙에서 동적 할당 필요 */
		vecs = kmalloc_objs(struct bio_vec, nr_pages); /* [한국어] nr_pages개 bio_vec을 담을 배열을 힙에서 할당 */
		if (!vecs) /* [한국어] 메모리 부족으로 할당 실패 검사 */
			return -ENOMEM; /* [한국어] 할당 실패 시 즉시 ENOMEM 반환 - 아직 bio를 초기화하지 않았으므로 별도 정리 불필요 */
	}

	if (iov_iter_rw(iter) == READ) { /* [한국어] READ 방향이면 REQ_OP_READ 로 bio 를 초기화하는 분기로 진입 */
		bio_init(&bio, bdev, vecs, nr_pages, REQ_OP_READ); /* [한국어] bio를 REQ_OP_READ 연산으로 초기화 - bdev와 bio_vec 배열 연결 */
		if (user_backed_iter(iter)) /* [한국어] iter가 실제 사용자 주소 공간 메모리(페이지)를 가리키는지 확인 */
			should_dirty = true; /* [한국어] 사용자 페이지 READ이므로 완료 후 dirty 표시가 필요함을 기록 */
	} else { /* [한국어] WRITE 방향이면 dio_bio_write_op()로 결정된 연산(REQ_OP_WRITE, 필요 시 FUA 포함)으로 초기화 */
		bio_init(&bio, bdev, vecs, nr_pages, dio_bio_write_op(iocb)); /* [한국어] WRITE 용 opf(REQ_OP_WRITE|REQ_SYNC|REQ_IDLE[|REQ_FUA])로 bio 초기화 - 힙이 아닌 스택 bio 를 직접 초기화하는 형태 */
	}
	bio.bi_iter.bi_sector = pos >> SECTOR_SHIFT; /* [한국어] 파일 오프셋(byte)을 512B 고정 단위 섹터 번호로 변환. 블록 계층의 섹터 단위는 장치 논리 블록 크기와 무관하게 언제나 512B 이고,
						 * 드라이버가 자기 단위로 되돌린다 - NVMe 는 nvme_setup_rw()가 (lba_shift - 9) 만큼 시프트해 SLBA 를 얻는다 */
	bio.bi_write_hint = file_inode(iocb->ki_filp)->i_write_hint; /* [한국어] inode 에 설정된 수명(lifetime) 쓰기 힌트를 bio 에 전달 - 이를 해석할지는 드라이버/장치의 재량이다 */
	bio.bi_write_stream = iocb->ki_write_stream; /* [한국어] kiocb 에 지정된 write stream 번호를 bio 에 전달 - NVMe 에서는 nvme_setup_rw()가 이 번호를 ns->head->plids[] 의 FDP placement identifier 로 바꿔 DSM 필드에 싣는다(core.c:1282-1288) */
	bio.bi_ioprio = iocb->ki_ioprio; /* [한국어] kiocb 의 I/O 우선순위(ioprio)를 bio 에 전달 - I/O 스케줄러(bfq/mq-deadline)가 이 값으로 순서를 조정한다 */
	if (iocb->ki_flags & IOCB_ATOMIC) /* [한국어] 유저가 원자적 쓰기(RWF_ATOMIC)를 요청했는지 확인 */
		bio.bi_opf |= REQ_ATOMIC; /* [한국어] REQ_ATOMIC 설정 - "이 쓰기는 전부 반영되거나 전혀 반영되지 않아야 한다"는 요구. NVMe 에서는 nvme_valid_atomic_write()(core.c:1235)가 NAWUPF/AWUPF 로부터 계산된 큐 한도(atomic_write_unit_max, atomic_write_boundary)와 대조해 위반이면 BLK_STS_INVAL 로 거절한다 */

	ret = blkdev_iov_iter_get_pages(&bio, iter, bdev); /* [한국어] 사용자 buffer의 물리 페이지를 bio의 bvec 배열에 채움(고정/pin 수행) */
	if (unlikely(ret)) /* [한국어] 페이지 고정/매핑 실패 검사 - 저확률 분기이므로 unlikely 힌트 */
		goto out; /* [한국어] 실패 시 bio 자체는 초기화되어 있으므로 out 레이블에서 공통 정리 수행 */
	ret = bio.bi_iter.bi_size; /* [한국어] 실제로 매핑된 총 바이트 수를 반환값 후보로 저장 */

	if (iov_iter_rw(iter) == WRITE) /* [한국어] WRITE 요청이면 */
		task_io_account_write(ret); /* [한국어] 현재 태스크의 쓰기 바이트 통계에 반영 - iostat 등에서 사용 */

	if (iocb->ki_flags & IOCB_NOWAIT) /* [한국어] 논블로킹(RWF_NOWAIT) 요청인지 확인 */
		bio.bi_opf |= REQ_NOWAIT; /* [한국어] REQ_NOWAIT 설정 - 태그 고갈이나 큐 혼잡으로 블로킹이 필요해지면 blk-mq 가 슬립하지 않고 BLK_STS_AGAIN 으로 즉시 실패시킨다 */

	submit_bio_wait(&bio); /* [한국어] bio 를 submit_bio()로 제출하고 완료(bio_endio)까지 현재 태스크를 슬립시켜 동기적으로 대기 - 내부적으로 completion 을 bi_end_io 에 걸어 두는 방식이다 */

	bio_release_pages(&bio, should_dirty); /* [한국어] 고정했던 사용자 페이지를 해제하며, should_dirty가 true면 페이지를 dirty로 표시(READ 결과 반영) */
	if (unlikely(bio.bi_status)) /* [한국어] 드라이버가 보고한 완료 상태(bi_status)에 에러가 있는지 확인 */
		ret = blk_status_to_errno(bio.bi_status); /* [한국어] blk_status_t 에러 코드를 표준 errno로 변환해 반환값을 덮어씀 */

out:
	if (vecs != inline_vecs) /* [한국어] 힙에서 동적 할당했던 경우인지 확인(스택 배열이면 해제 불필요) */
		kfree(vecs); /* [한국어] 힙 할당된 bio_vec 배열 해제 */

	bio_uninit(&bio); /* [한국어] 스택 bio가 내부적으로 잡고 있던 자원(예: integrity 등)을 정리 - bio_init과 짝을 이루는 마무리 호출 */

	return ret; /* [한국어] 전송 바이트 수 또는 음수 errno를 VFS(blkdev_direct_IO 경유)로 반환 */
}

/*
 * [한국어] dio 상태 플래그 - struct blkdev_dio.flags에 OR로 조합되어 저장된다.
 * DIO_SHOULD_DIRTY는 READ 완료 후 사용자 페이지를 dirty로 표시해야 함을,
 * DIO_IS_SYNC는 이 dio가 동기(sync) kiocb에서 왔음을(=waiter로 완료를
 * 기다림) 나타낸다. 두 값 모두 struct blkdev_dio 초기화 시(__blkdev_direct_IO)
 * 설정되고, 완료 콜백(blkdev_bio_end_io)에서 읽혀 완료 처리 방식을 분기한다.
 */
enum {
	DIO_SHOULD_DIRTY	= 1,
	/* [한국어] READ 방향 direct I/O가 사용자 페이지(user_backed_iter)를 대상으로 했을 때 설정.
	 * 설정자: __blkdev_direct_IO()가 is_read && user_backed_iter(iter)일 때 dio->flags에 OR.
	 * 읽는 자: blkdev_bio_end_io()가 매 bio 완료 시 이 비트를 확인해 bio_check_pages_dirty()
	 *          호출 여부(dirty 처리) 대 bio_release_pages() 호출 여부(단순 해제)를 결정.
	 * 값 범위: 비트마스크 1(= 1<<0). flags 필드 내 다른 비트와 OR로 공존 가능.
	 * 동기화: dio->flags는 __blkdev_direct_IO()가 초기화를 마친 뒤에만 다른 bio가
	 *         참조하므로 별도 락 없이 읽기 전용으로 공유된다. */
	DIO_IS_SYNC		= 2,
	/* [한국어] 이 direct I/O가 동기(is_sync_kiocb) 요청이어서 완료를 kiocb->ki_complete가
	 * 아니라 dio->waiter(현재 태스크)를 깨우는 방식으로 처리해야 함을 나타낸다.
	 * 설정자: __blkdev_direct_IO()가 is_sync_kiocb(iocb)가 참일 때 dio->flags = DIO_IS_SYNC로 설정.
	 * 읽는 자: blkdev_bio_end_io()가 atomic_dec_and_test(&dio->ref)로 마지막 bio 완료를
	 *          감지했을 때 이 비트로 kiocb 완료 통지 대 태스크 깨우기를 분기.
	 * 값 범위: 비트마스크 2(= 1<<1).
	 * 동기화: 위와 동일하게 초기화 이후에는 읽기 전용으로 공유되며, 실제 "깨우기" 동작은
	 *         WRITE_ONCE(dio->waiter, NULL) + blk_wake_io_task()의 메모리 배리어로 보장된다. */
};

/*
 * [한국어]
 * struct blkdev_dio - direct I/O 요청의 생명주기를 관리하는 컨테이너
 *
 * 목적: 하나의 kiocb가 BIO_MAX_VECS 제한 때문에 여러 개의 bio로 분할될 때,
 *       모든 bio의 완료를 모아서 VFS에 결과를 돌려주기 전까지 필요한 상태를
 *       유지하는 컨테이너다. struct bio를 마지막 멤버로 내장(embed)함으로써
 *       dio 자신이 blkdev_dio_pool이라는 하나의 bio_set에서 "첫 번째 bio +
 *       부가 상태"를 한 번의 할당으로 얻도록 설계되었다(container_of로 상호
 *       변환).
 * 왜 분할이 필요한가: 하나의 bio 가 담을 수 있는 bio_vec 개수는
 *       BIO_MAX_VECS 로 제한되어 있다. 사용자가 그보다 큰 버퍼로 한 번에
 *       O_DIRECT read/write 를 하면 이 파일이 요청을 여러 bio 로 쪼개
 *       연달아 제출하고, 그 완료를 여기서 다시 하나로 합쳐야 한다.
 * 큐 깊이 관점(NVMe 독자용): 분할된 bio 는 각각 별개의 struct request 가
 *       되고, blk-mq 는 요청마다 태그(tag)를 하나씩 소비한다. NVMe 에서는
 *       그 태그가 그대로 커맨드 식별자로 쓰인다 —
 *       cmd->common.command_id = nvme_cid(req) (drivers/nvme/host/core.c:1417).
 *       따라서 "dio->ref 가 N" 이라는 것은 이 요청 하나가 장치 큐 깊이를 N
 *       만큼 점유하고 있다는 뜻이 된다.
 *   - ref: 아직 완료 콜백이 돌아오지 않은 bio 개수. 0이 되어야 전체 direct
 *     I/O 가 끝난 것으로 간주한다.
 *   - size: 실제로 전송/수신한 총 바이트로, 완료 시 kiocb->ki_pos 갱신과
 *     반환값 계산에 사용된다.
 */
struct blkdev_dio {
	union {
		struct kiocb		*iocb; /* [한국어] 비동기 완료 통지 대상(아래 상세) */
		/* [한국어] 비동기(非同期) 완료 시 결과를 통지할 대상 kiocb 포인터.
		 * 설정자: __blkdev_direct_IO()/__blkdev_direct_IO_async()가 is_sync가
		 *         아닐 때(dio->flags에 DIO_IS_SYNC가 없을 때) 대입.
		 * 읽는 자: blkdev_bio_end_io()/blkdev_bio_end_io_async()가 마지막 bio
		 *          완료(atomic_dec_and_test) 시 iocb->ki_complete(iocb, ret)
		 *          호출에 사용 - 완료 결과를 VFS 에 비동기로 통지.
		 * 값 범위: 유효한 kiocb 포인터. waiter와 같은 메모리를 공유하는
		 *          union이므로 동시에 두 의미로 해석되지 않는다(flags로 구분).
		 * 동기화: 완료 콜백 진입 시 한 번만 읽히고 즉시 iocb->private을
		 *         WRITE_ONCE로 지우므로 경쟁 없음. */
		struct task_struct	*waiter;
		/* [한국어] 동기(同期) 완료 시 깨워야 할 대기 태스크(현재 프로세스) 포인터.
		 * 설정자: __blkdev_direct_IO()가 is_sync_kiocb(iocb)일 때 current로 대입.
		 * 읽는 자: blkdev_bio_end_io()가 마지막 bio 완료 시 WRITE_ONCE(dio->waiter,
		 *          NULL) 후 blk_wake_io_task(waiter)로 깨움에 사용. 대기 측은
		 *          __blkdev_direct_IO()의 for(;;) 루프에서 READ_ONCE(dio->waiter)가
		 *          NULL이 될 때까지 TASK_UNINTERRUPTIBLE로 블로킹.
		 * 값 범위: 유효한 task_struct 포인터, 완료 후 NULL로 전이.
		 * 동기화: READ_ONCE/WRITE_ONCE 페어와 blk_io_schedule()의 스케줄러
		 *         배리어로 두 컨텍스트(완료 처리 vs 대기 태스크) 간
		 *         가시성을 보장한다(락 없는 플래그 기반 동기화). */
	};
	size_t			size; /* [한국어] 누적 전송 바이트(아래 상세) */
	/* [한국어] 이 direct I/O 전체에서 지금까지 성공적으로 전송/수신된 누적 바이트 수.
	 * 설정자: __blkdev_direct_IO()가 매 분할 bio 제출 전 dio->size +=
	 *         bio->bi_iter.bi_size로 누적. __blkdev_direct_IO_async()는 단일
	 *         bio이므로 한 번만 대입.
	 * 읽는 자: 완료 콜백(blkdev_bio_end_io[_async])이 성공 시 iocb->ki_pos에
	 *          더하고 최종 반환값으로 사용.
	 * 값 범위: 0 이상, 요청 전체 길이 이하의 바이트 수.
	 * 동기화: 분할 루프는 단일 스레드(제출 태스크)에서만 갱신하므로 락 불필요.
	 *         완료 콜백에서의 읽기는 ref가 0이 되어 모든 제출이 끝난 뒤에만
	 *         일어나므로 데이터 경쟁이 없다. */
	atomic_t		ref;
	/* [한국어] 아직 완료 콜백이 돌아오지 않은(=제출되었지만 끝나지 않은) bio 개수.
	 * 설정자: __blkdev_direct_IO()가 1로 초기화(atomic_set) 후, 분할마다 추가
	 *         bio를 제출하기 전 atomic_inc()로 증가.
	 * 읽는 자/갱신자: blkdev_bio_end_io()가 매 bio 완료 시 atomic_dec_and_test()로
	 *          감소시키고, 0에 도달하면(=마지막 bio) 최종 완료 처리를 수행.
	 * 값 범위: 1 이상으로 시작해 0까지 단조 감소.
	 * 동기화: 여러 bio의 완료 콜백이 서로 다른 CPU/인터럽트 컨텍스트에서
	 *         동시에 호출될 수 있으므로 원자적 감소+테스트(atomic_dec_and_test)가
	 *         필수적이다 - 일반 카운터라면 마지막 완료를 두 번 감지하거나
	 *         전혀 감지하지 못하는 경쟁이 발생한다. */
	unsigned int		flags;
	/* [한국어] DIO_SHOULD_DIRTY/DIO_IS_SYNC 비트를 OR로 조합해 저장하는 상태 플래그.
	 * 설정자: __blkdev_direct_IO()/__blkdev_direct_IO_async()가 초기화 시점에 결정.
	 * 읽는 자: blkdev_bio_end_io()/blkdev_bio_end_io_async()가 완료 처리 방식
	 *          분기(더티 처리 여부, 동기/비동기 완료 통지 방식)에 사용.
	 * 값 범위: 0, DIO_SHOULD_DIRTY(1), DIO_IS_SYNC(2), 또는 둘의 OR(3).
	 * 동기화: 초기화 이후 값이 바뀌지 않는 읽기 전용 필드이므로 락 불필요. */
	struct bio		bio ____cacheline_aligned_in_smp;
	/* [한국어] blkdev_dio_pool(bio_set)에서 이 dio 구조체와 함께 "한 번에" 할당된
	 * 첫 번째 bio. container_of(bio_ptr, struct blkdev_dio, bio)로 bio 포인터
	 * 로부터 역으로 dio 구조체 포인터를 구하는 데 사용되는 앵커(anchor) 필드다.
	 * 설정자: bio_alloc_bioset(bdev, nr_pages, opf, GFP_KERNEL, &blkdev_dio_pool)가
	 *         내부적으로 이 필드 영역에 bio를 구성.
	 * 읽는 자: blkdev_bio_end_io_async()는 container_of로 직접 dio를 복원하고,
	 *          blkdev_bio_end_io()는 분할된 "다른" bio들의 bi_private에 저장된
	 *          dio 포인터를 통해 간접적으로 이 필드(dio->bio)를 상태 집계용으로
	 *          사용(dio->bio.bi_status에 첫 에러를 모음).
	 * 값 범위: 유효한 초기화된 bio. 이 요청에서 가장 먼저 제출되는 bio 다.
	 * 동기화: ____cacheline_aligned_in_smp 속성으로 캐시라인 정렬해 인접 필드와의
	 *         false sharing(멀티 CPU에서 서로 다른 필드를 갱신하며 캐시라인을
	 *         주고받는 성능 저하)을 방지한다. */
};

static struct bio_set blkdev_dio_pool; /* [한국어] direct I/O 전용 bio 풀(아래 상세) */
/* [한국어] direct I/O 전용 bio 할당 풀(bio_set). struct blkdev_dio를 크기로
 * 하는 슬랩과, 필요한 bio_vec을 미리 확보한 mempool을 함께 관리해 direct
 * I/O 경로에서 매번 slab 할당자를 거치지 않고도 빠르게(그리고 메모리 부족
 * 상황에서도 전진 보장이 되도록) bio를 뽑아 쓸 수 있게 한다.
 * 설정자: blkdev_init()이 모듈/부트 초기화 시점에 bioset_init()으로 1회 초기화.
 * 읽는 자: __blkdev_direct_IO(), __blkdev_direct_IO_async()가 bio_alloc_bioset()
 *          호출 시 이 풀을 지정해 dio+bio를 함께 할당.
 * 값 범위: 초기화 후에는 불변인 전역 bio_set 구조체(내부적으로 mempool/slab 포인터 보유).
 * 동기화: bio_set/mempool 자체가 내부적으로 스핀락/원자 연산으로 멀티 CPU 동시
 *         할당을 지원하므로 이 파일에서 추가 락은 불필요하다. */

/*
 * [한국어]
 * blkdev_bio_end_io - 분할(split) direct I/O를 구성하는 개별 bio의 완료 콜백
 *
 * @bio: 방금 완료(인터럽트 또는 폴)로 종료된 하나의 분할 bio.
 *       bio->bi_private에 소속 struct blkdev_dio 포인터가 들어 있다.
 * @return: void (bio->bi_end_io 콜백 시그니처).
 *
 * __blkdev_direct_IO()가 큰 요청을 여러 bio로 쪼개 순차 제출했을 때, 그
 * 각각의 bio 가 장치에서 완료될 때마다 호출되는 콜백이다. 개별
 * bio의 에러를 dio->bio.bi_status(대표 상태)에 병합하고, integrity(PI/DIF)
 * 매핑을 해제한 뒤, dio->ref를 원자적으로 감소시켜 "이 bio가 마지막으로
 * 남은 미완료 bio였는지" 검사한다. 마지막이면 dio->flags의 DIO_IS_SYNC
 * 여부에 따라 (a) 비동기: iocb->ki_complete()로 VFS에 결과 통지, 또는
 * (b) 동기: dio->waiter(대기 태스크)를 blk_wake_io_task()로 깨움 중 하나를
 * 수행한다. 마지막으로 이 bio 자신이 사용한 사용자 페이지를 dirty 표시
 * 또는 해제한다. 실행 컨텍스트: 드라이버의 완료 하단부(대개 softirq) 또는
 * 폴링을 수행한 태스크 컨텍스트에서 호출되므로, 여러 CPU 에서 서로 다른
 * 분할 bio 에 대해 동시에 실행될 수 있다(그래서 atomic_dec_and_test 가 필수).
 * 호출자: bio_endio() (block/bio.c). blk-mq 드라이버라면 blk_mq_end_request
 *         -> __blk_mq_end_request -> bio_endio 경로를 거친다.
 * 호출 대상: bio_integrity_unmap_user, blk_status_to_errno, ki_complete,
 *           bio_put, blk_wake_io_task, bio_check_pages_dirty, bio_release_pages.
 * 에러 경로: bio->bi_status가 설정된 경우 dio->bio.bi_status에 최초 1회만
 * 기록되어(이미 기록된 에러는 덮어쓰지 않음) 최종 반환값에 반영된다.
 * 호출 체인:
 *   드라이버 완료 처리(인터럽트/폴) -> blk_mq_end_request -> bio_endio -> [blkdev_bio_end_io]
 *   (마지막 bio라면) -> dio->iocb->ki_complete 또는 blk_wake_io_task(dio->waiter)
 */
static void blkdev_bio_end_io(struct bio *bio)
{
	struct blkdev_dio *dio = bio->bi_private; /* [한국어] 제출 시 bi_private 에 저장해둔 소속 blkdev_dio 컨테이너를 복원 - 분할된 여러 bio 가 같은 dio 를 공유한다 */
	bool should_dirty = dio->flags & DIO_SHOULD_DIRTY; /* [한국어] 이 direct I/O가 READ이며 사용자 페이지를 dirty 처리해야 하는지 확인 */
	bool is_sync = dio->flags & DIO_IS_SYNC; /* [한국어] 이 direct I/O가 동기 요청(waiter 깨우기 방식)인지 확인 */

	if (bio->bi_status && !dio->bio.bi_status) /* [한국어] 이 bio가 에러이고, dio 대표 상태가 아직 비어있다면(최초 에러) */
		dio->bio.bi_status = bio->bi_status; /* [한국어] 개별 bio 오류를 dio의 대표 상태로 전파 - 이후 발생하는 다른 bio 에러는 이미 기록된 첫 에러를 덮어쓰지 않음 */

	if (bio_integrity(bio)) /* [한국어] 이 bio에 PI/DIF integrity metadata가 매핑되어 있었는지 확인 */
		bio_integrity_unmap_user(bio); /* [한국어] 사용자 메타데이터 버퍼에 대한 매핑 해제 */

	if (atomic_dec_and_test(&dio->ref)) { /* [한국어] 미완료 bio 카운터를 원자적으로 1 감소시키고, 그 결과 0이 되었는지(=이 bio가 마지막이었는지) 검사 */
		if (!is_sync) { /* [한국어] 비동기 요청이면 kiocb 완료 통지 경로로 진입 - 대기 중인 태스크가 없으므로 ki_complete 로 결과를 올려보낸다 */
			struct kiocb *iocb = dio->iocb; /* [한국어] union에서 비동기용으로 저장해둔 kiocb 포인터를 지역 변수로 확보(이후 dio가 해제될 수 있으므로 미리 복사) */
			ssize_t ret; /* [한국어] VFS에 보고할 최종 반환값(전송 바이트 수 또는 음수 errno) */

			WRITE_ONCE(iocb->private, NULL); /* [한국어] iocb->private 에 남아있던 폴링용 bio 포인터를 지운다 - .iopoll(iocb_bio_iopoll)이 이미 완료된 bio 를 다시 폴하려 드는 것을 막기 위해 WRITE_ONCE 로 한 번에 가시화 */

			if (likely(!dio->bio.bi_status)) { /* [한국어] 지금까지 어떤 분할 bio에서도 에러가 없었는지 확인 - 대부분 성공 경로이므로 likely */
				ret = dio->size; /* [한국어] 성공이면 누적 전송 바이트 수를 반환값으로 사용 */
				iocb->ki_pos += ret; /* [한국어] 파일(장치) 커서 위치를 전송한 만큼 전진 - 다음 read/write 호출을 위한 상태 갱신 */
			} else {
				ret = blk_status_to_errno(dio->bio.bi_status); /* [한국어] 에러가 있었으면 blk_status_t를 표준 errno로 변환 */
			}

			dio->iocb->ki_complete(iocb, ret); /* [한국어] VFS 의 비동기 완료 콜백 호출 - io_uring/AIO 가 여기서 CQE 를 채운다 */
			bio_put(&dio->bio); /* [한국어] __blkdev_direct_IO에서 bio_get()으로 잡아둔 추가 참조를 반납 - 이제 dio(=내장된 bio)가 실제로 해제될 수 있음 */
		} else {
			struct task_struct *waiter = dio->waiter; /* [한국어] 동기 대기 중인 태스크 포인터를 지역 변수로 확보 */

			WRITE_ONCE(dio->waiter, NULL); /* [한국어] waiter 필드를 NULL로 표시 - 대기 측 for(;;) 루프의 READ_ONCE(dio->waiter)가 이를 보고 루프를 탈출 */
			blk_wake_io_task(waiter); /* [한국어] 대기 중이던 태스크를 깨움 - 완료 컨텍스트에서 제출 태스크를 실행 가능 상태로 되돌린다 */
		}
	}

	if (should_dirty) { /* [한국어] READ이고 사용자 페이지였다면 dirty 처리 경로로 진입 */
		bio_check_pages_dirty(bio); /* [한국어] 페이지를 dirty로 표시하고 필요한 경우 이 호출 안에서 bio/페이지 해제까지 수행(지연 해제 가능) */
	} else { /* [한국어] WRITE이거나 dirty 처리가 불필요한 경우 */
		bio_release_pages(bio, false); /* [한국어] 페이지 고정을 해제(dirty 표시 없이) - 실패 시: bio 해제 및 plug 종료 경로와 공유되는 정리 루틴 */
		bio_put(bio); /* [한국어] 이 분할 bio 자체를 해제 - 초기화/매핑 실패 시에도 동일하게 bio 해제 */
	}
}

/*
 * [한국어]
 * __blkdev_direct_IO - BIO_MAX_VECS를 넘는 대형 요청을 여러 bio로 분할 제출하는 direct I/O
 *
 * @iocb: direct read/write 요청을 담은 kiocb. is_sync_kiocb()로 동기/비동기를
 *        판별하고, ki_pos/ki_write_stream/ki_ioprio 등이 각 분할 bio에 전파된다.
 * @iter: 전송할 사용자 buffer 전체를 표현하는 iov_iter. 루프를 돌며 조금씩
 *        소비(advance)되어 여러 bio에 나뉘어 매핑된다.
 * @bdev: 대상 block_device.
 * @nr_pages: 첫 bio에 필요한 bio_vec 개수(호출자가 bio_max_segs()로 상한을 씌워 전달).
 * @return: 비동기면 -EIOCBQUEUED(완료는 나중에 kiocb->ki_complete로 통지),
 *          동기면 전송 바이트 수 또는 음수 errno.
 *
 * 목적: 요청이 한 bio가 담을 수 있는 세그먼트 수(BIO_MAX_VECS)를 초과하는
 * 경우, iter를 다 소비할 때까지 bio를 하나씩 할당(blkdev_dio_pool에서 첫
 * bio, 이후는 일반 bio_alloc)하여 순차적으로 submit_bio()로 제출하고, 모든
 * bio의 완료를 struct blkdev_dio 하나로 집계한다. 첫 bio는 dio를 내장하고
 * 있으므로 bio_get()으로 추가 참조를 잡아 마지막 완료 콜백이 끝날 때까지
 * (bio_put으로 상쇄될 때까지) 살아있게 만든다. 동기 kiocb라면 함수 자신이
 * 모든 bio 완료를 TASK_UNINTERRUPTIBLE 상태로 기다렸다가 결과를 반환하고,
 * 비동기 kiocb라면 제출만 마치고 즉시 -EIOCBQUEUED를 반환해 나머지는
 * blkdev_bio_end_io() 콜백에 맡긴다. 실행 컨텍스트: direct read/write
 * 시스템 콜을 처리하는 유저 프로세스의 커널 스레드 컨텍스트에서 시작되며,
 * 동기 경로는 blk_io_schedule()로 슬립한다.
 * 호출자: blkdev_direct_IO (nr_pages가 BIO_MAX_VECS를 초과하거나
 *         IOCB_HAS_METADATA가 설정된 경우).
 * 호출 대상: bio_alloc_bioset, blkdev_iov_iter_get_pages, bio_integrity_map_iter,
 *           submit_bio, bio_alloc, blk_start_plug/blk_finish_plug,
 *           blk_status_to_errno.
 * 에러 경로: 페이지 매핑 실패 시 해당 bio를 BLK_STS_IOERR로 즉시 완료시켜
 * 루프를 빠져나오고, NOWAIT인데 남은 데이터가 있거나 integrity 매핑이
 * 실패하면 fail 레이블로 점프해 아직 제출하지 않은 bio만 직접 정리한다
 * (이미 제출된 bio들은 blkdev_bio_end_io를 통해 별도로 완료됨).
 * 호출 체인:
 *   blkdev_direct_IO -> [__blkdev_direct_IO] -> submit_bio -> blk_mq_submit_bio -> (mq_ops->queue_rq 간접 호출; NVMe PCIe면 nvme_queue_rq)
 *   (완료) 드라이버 완료 처리 -> bio_endio -> blkdev_bio_end_io -> (동기) blk_wake_io_task / (비동기) ki_complete
 */
static ssize_t __blkdev_direct_IO(struct kiocb *iocb, struct iov_iter *iter,
		struct block_device *bdev, unsigned int nr_pages)
{
	struct blk_plug plug; /* [한국어] 여러 bio를 모아 한 번에 하위 큐로 흘려보내기 위한 plug 컨텍스트 - 분할 제출 성능 최적화 */
	struct blkdev_dio *dio; /* [한국어] 이 direct I/O 전체의 완료를 집계할 컨테이너 포인터 */
	struct bio *bio; /* [한국어] 루프에서 현재 채우고 있는 분할 bio 포인터 */
	bool is_read = (iov_iter_rw(iter) == READ), is_sync; /* [한국어] READ/WRITE 방향과, 이 kiocb가 동기 완료를 요구하는지 여부 */
	blk_opf_t opf = is_read ? REQ_OP_READ : dio_bio_write_op(iocb); /* [한국어] 방향에 따라 REQ_OP_READ 또는 dio_bio_write_op()가 계산한 WRITE opf 선택 - 모든 분할 bio가 동일한 opf 공유 */
	loff_t pos = iocb->ki_pos; /* [한국어] 시작 파일(장치) 오프셋 - 루프를 돌며 각 bio의 전송량만큼 전진 */
	int ret = 0; /* [한국어] 매핑/제출 중 에러 코드를 담는 변수 - 0이면 정상 진행 중 */

	bio = bio_alloc_bioset(bdev, nr_pages, opf, GFP_KERNEL, /* [한국어] bio 객체를 blkdev_dio_pool에서 할당 - blkdev_dio가 함께(embedded) 확보됨 */
			       &blkdev_dio_pool); /* [한국어] 전용 mempool 을 지정해 메모리 압박 상황에서도 할당이 전진하도록 보장한다 */
	dio = container_of(bio, struct blkdev_dio, bio); /* [한국어] 방금 할당된 bio가 포함된 blkdev_dio 구조체의 시작 주소를 역산 */
	atomic_set(&dio->ref, 1); /* [한국어] 미완료 bio 카운터를 1로 초기화 - 지금 만드는 첫 bio 몫. 이 카운터가 0이 되어야 direct I/O 전체가 끝난 것으로 본다 */
	/*
	 * Grab an extra reference to ensure the dio structure which is embedded
	 * into the first bio stays around.
	 */
	bio_get(bio); /* [한국어] dio는 첫 bio에 내장되어 있으므로, 완료 콜백이 bio_put으로 반납하더라도 이 함수가 dio->size 등을 마저 읽을 때까지 살아있도록 참조 하나를 더 잡음 */

	is_sync = is_sync_kiocb(iocb); /* [한국어] 동기 kiocb 여부 판별 - 이후 완료 통지 방식(waiter 대기 vs ki_complete)을 결정 */
	if (is_sync) { /* [한국어] 동기 요청이면 현재 태스크가 직접 완료를 기다리는 방식으로 설정 */
		dio->flags = DIO_IS_SYNC; /* [한국어] 완료 콜백이 waiter를 깨우도록 플래그 설정 */
		dio->waiter = current; /* [한국어] 동기: 현재 태스크를 깨울 대상으로 저장 - 마지막 bio 의 완료 콜백(blkdev_bio_end_io)이 이 포인터로 blk_wake_io_task 를 호출한다 */
	} else { /* [한국어] 비동기 요청이면 kiocb 완료 통지 방식으로 설정 */
		dio->flags = 0; /* [한국어] DIO_IS_SYNC를 켜지 않음 - 완료 콜백이 ki_complete 경로를 타도록 */
		dio->iocb = iocb; /* [한국어] 비동기: 마지막 bio 완료 시 ki_complete 를 호출할 kiocb 저장 */
	}

	dio->size = 0; /* [한국어] 누적 전송 바이트 수 초기화 */
	if (is_read && user_backed_iter(iter)) /* [한국어] READ이면서 사용자 주소 공간 buffer를 대상으로 하는지 확인 */
		dio->flags |= DIO_SHOULD_DIRTY; /* [한국어] 완료 시 사용자 페이지를 dirty로 표시해야 함을 기록 */

	blk_start_plug(&plug); /* [한국어] 분할 제출 구간 시작 - 이 구간 안의 bio 들은 곧바로 드라이버로 내려가지 않고 현재 태스크의 plug 리스트에 쌓였다가 blk_finish_plug 에서 한꺼번에 dispatch 된다.
				 * 드라이버가 배치 제출(mq_ops->queue_rqs)을 구현하면 이 묶음이 그대로 전달된다 - NVMe PCIe 의 nvme_queue_rqs()가 한 배치를 SQ 에 몰아 쓰고 doorbell 을 한 번만 기록하는 것이 그 예다 */

	for (;;) { /* [한국어] iter가 소진될 때까지 bio를 계속 채우고 제출하는 분할 루프 */
		bio->bi_iter.bi_sector = pos >> SECTOR_SHIFT; /* [한국어] 현재 오프셋(바이트)을 512B 고정 단위 섹터 번호로 변환. 블록 계층의 섹터는 장치 논리 블록 크기와 무관하게 항상 512B 단위이며,
						 * 드라이버가 자기 LBA 단위로 되돌린다 - NVMe 는 nvme_setup_rw()에서 bi_sector 를 (lba_shift - 9) 만큼 다시 시프트해 SLBA 를 만든다 */
		bio->bi_write_hint = file_inode(iocb->ki_filp)->i_write_hint; /* [한국어] inode의 쓰기 힌트를 이 분할 bio에도 동일하게 전파 */
		bio->bi_write_stream = iocb->ki_write_stream; /* [한국어] write stream 식별자를 이 분할 bio에도 전파 - NVMe 에서는 nvme_setup_rw()가 이 값을 FDP placement identifier 로 바꿔 DSM 필드 상위 16비트에 싣는다(core.c:1282-1288) */
		bio->bi_private = dio; /* [한국어] 완료 콜백이 소속 dio를 찾을 수 있도록 각 분할 bio에 dio 컨텍스트 연결 */
		bio->bi_end_io = blkdev_bio_end_io; /* [한국어] 이 bio 가 완료될 때 bio_endio()가 호출할 콜백 등록 - 드라이버 완료 경로(인터럽트 하단부 또는 폴링 태스크)에서 실행된다 */
		bio->bi_ioprio = iocb->ki_ioprio; /* [한국어] I/O 우선순위 전파 */

		ret = blkdev_iov_iter_get_pages(bio, iter, bdev); /* [한국어] iter의 다음 부분을 이 bio의 bvec에 매핑(사용자 페이지 고정) - 성공 시 iter가 그만큼 advance됨 */
		if (unlikely(ret)) { /* [한국어] 페이지 고정/매핑 실패 검사 */
			bio->bi_status = BLK_STS_IOERR; /* [한국어] 페이지 매핑 실패 시 이 bio 를 I/O 에러 상태로 직접 표시 - 장치에 내려보내지 않고도 아래 bio_endio()로 정상 완료 경로를 태우기 위함 */
			bio_endio(bio); /* [한국어] 아직 제출하지 않았지만 완료 콜백 경로를 그대로 태워 dio->ref 감소 및 상태 집계를 재사용 */
			break; /* [한국어] 더 이상 분할을 진행할 수 없으므로 루프 종료 */
		}
		if (iocb->ki_flags & IOCB_NOWAIT) { /* [한국어] 논블로킹 요청인지 확인 - NOWAIT 요청이 다음 분할에서 블로킹되는 것을 피하기 위한 특수 처리 진입 */
			/*
			 * This is nonblocking IO, and we need to allocate
			 * another bio if we have data left to map. As we
			 * cannot guarantee that one of the sub bios will not
			 * fail getting issued FOR NOWAIT and as error results
			 * are coalesced across all of them, be safe and ask for
			 * a retry of this from blocking context.
			 */
			if (unlikely(iov_iter_count(iter))) { /* [한국어] 이번 bio에 다 담지 못하고 iter에 데이터가 남아있는지 확인 - 남아있으면 또 다른(블로킹 가능한) bio 할당이 필요함 */
				ret = -EAGAIN; /* [한국어] NOWAIT 보장을 지킬 수 없으므로 블로킹 컨텍스트에서 재시도하도록 요청 */
				goto fail; /* [한국어] 지금까지 준비한(아직 제출 안 한) 이 bio만 정리하고 에러 반환 */
			}
			bio->bi_opf |= REQ_NOWAIT; /* [한국어] 데이터가 다 담겼다면 이 마지막 bio 에도 NOWAIT 을 부여 - 태그 고갈 등으로 블로킹이 필요해지면 blk-mq 가 BLK_STS_AGAIN 으로 즉시 실패시킨다 */
		}
		if (iocb->ki_flags & IOCB_HAS_METADATA) { /* [한국어] 유저가 PI/DIF 메타데이터를 함께 전달했는지 확인 */
			ret = bio_integrity_map_iter(bio, iocb->private); /* [한국어] 유저가 준 메타데이터 buffer 를 이 bio 의 integrity payload 로 매핑 - NVMe 라면 네임스페이스의 metadata 영역(PI: Protection Information)에 실릴 바이트다 */
			if (unlikely(ret)) /* [한국어] 메타데이터 매핑 실패 검사 */
				goto fail; /* [한국어] 실패 시 이 bio만 정리하고 에러 반환 */
		}

		if (is_read) { /* [한국어] READ 방향이면 dirty 처리 대상 확인, WRITE 방향이면 통계만 집계 */
			if (dio->flags & DIO_SHOULD_DIRTY) /* [한국어] 사용자 페이지 대상 READ인지 재확인 */
				bio_set_pages_dirty(bio); /* [한국어] 이 bio가 채울 페이지들을 미리 dirty로 표시(READ 완료 전에 표시해도 무방한 이유는 완료 콜백에서 재확인하지 않기 때문) */
		} else { /* [한국어] WRITE 방향 */
			task_io_account_write(bio->bi_iter.bi_size); /* [한국어] 이 분할 bio 몫만큼 태스크 쓰기 바이트 통계에 반영 */
		}
		dio->size += bio->bi_iter.bi_size; /* [한국어] 이번 bio가 담당하는 바이트 수를 dio 누적 크기에 더함 */
		pos += bio->bi_iter.bi_size; /* [한국어] 다음 bio가 사용할 시작 오프셋을 전진 */

		nr_pages = bio_iov_vecs_to_alloc(iter, BIO_MAX_VECS); /* [한국어] iter에 아직 남은 데이터를 담기 위해 다음 bio에 필요한 bio_vec 개수 계산(최대 BIO_MAX_VECS) */
		if (!nr_pages) { /* [한국어] 더 이상 담을 데이터가 없다면(=이번이 마지막 bio) */
			submit_bio(bio); /* [한국어] 마지막 bio 를 제출 - 더 만들 bio 가 없으므로 ref 를 추가로 올리지 않고, 초기값 1 이 이 bio 의 몫이 된다 */
			break; /* [한국어] 분할 루프 종료 */
		}
		atomic_inc(&dio->ref); /* [한국어] 남은 데이터가 있어 다음 bio를 추가로 제출할 것이므로, 미완료 카운터를 먼저 증가시켜 둠(제출 후 완료 콜백과의 경쟁 방지) */
		submit_bio(bio); /* [한국어] 현재 채운 bio 를 제출 - plug 구간 안이므로 실제 dispatch 는 blk_finish_plug 까지 지연될 수 있다 */
		bio = bio_alloc(bdev, nr_pages, opf, GFP_KERNEL); /* [한국어] 남은 데이터를 담을 다음 bio를 새로 할당(이번엔 blkdev_dio_pool이 아닌 일반 fs_bio_set 사용) */
	}

	blk_finish_plug(&plug); /* [한국어] 분할 제출 구간 종료 - 지금까지 배치된 bio들을 실제로 큐에 밀어넣도록 플러시 */

	if (!is_sync) /* [한국어] 비동기 요청이면 */
		return -EIOCBQUEUED; /* [한국어] 아직 완료되지 않았음을 VFS에 알리고 즉시 반환 - 실제 완료는 blkdev_bio_end_io가 담당 */

	for (;;) { /* [한국어] 동기 요청 - 모든 분할 bio 의 완료를 기다리는 대기 루프. 조건 변수 없이 dio->waiter 포인터 하나로 동기화한다 */
		set_current_state(TASK_UNINTERRUPTIBLE); /* [한국어] 시그널로 깨어나지 않는 대기 상태로 먼저 전환한 뒤 조건을 검사한다 - 순서를 뒤집으면 조건 검사와 슬립 사이에 도착한 완료 통지를 놓쳐 영영 깨어나지 못한다(lost wakeup) */
		if (!READ_ONCE(dio->waiter)) /* [한국어] 완료 콜백이 waiter를 NULL로 지웠는지 확인(=마지막 bio 완료됨) */
			break; /* [한국어] 완료되었으면 대기 루프 탈출 */
		blk_io_schedule(); /* [한국어] 아직 완료되지 않았으면 CPU 를 양보하고 슬립 - iowait 로 회계되며, 완료 콜백의 blk_wake_io_task 가 깨운다 */
	}
	__set_current_state(TASK_RUNNING); /* [한국어] 대기를 마쳤으므로 태스크 상태를 실행 가능으로 복귀 */

	if (!ret) /* [한국어] 분할 루프 중 에러가 없었다면(=ret이 여전히 0) - 모든 bio 완료 후 dio->size 반환 준비 */
		ret = blk_status_to_errno(dio->bio.bi_status); /* [한국어] 완료 콜백들이 집계한 대표 상태(dio->bio.bi_status)를 errno로 변환 */
	if (likely(!ret)) /* [한국어] 최종적으로 에러가 없다면(성공 경로이므로 likely) */
		ret = dio->size; /* [한국어] 반환값을 누적 전송 바이트 수로 교체 */

	bio_put(&dio->bio); /* [한국어] 앞서 bio_get()으로 잡아둔 추가 참조를 반납 - 이제 dio(및 내장 bio)가 실제로 해제될 수 있음 */
	return ret; /* [한국어] 전송 바이트 수 또는 음수 errno를 VFS로 반환 */
fail:
	bio_release_pages(bio, false); /* [한국어] 방금 만들던(아직 제출 전) bio가 고정했을 수 있는 페이지들을 해제 */
	bio_clear_flag(bio, BIO_REFFED); /* [한국어] 이 bio가 참조 카운트 방식으로 관리되지 않도록 플래그 해제 - 아래 bio_put에서 곧바로 메모리 해제되도록 함 */
	bio_put(bio); /* [한국어] 제출된 적 없는 이 bio 를 즉시 해제 - 장치로 내려간 적이 없으므로 완료 콜백을 거치지 않는다 */
	blk_finish_plug(&plug); /* [한국어] 이미 제출된 이전 bio들은 정상적으로 플러시되도록 plug 종료 */
	return ret; /* [한국어] -EAGAIN 등 에러 코드를 VFS로 반환 */
}

/*
 * [한국어]
 * blkdev_bio_end_io_async - 단일 bio 비동기 direct I/O의 완료 콜백
 *
 * @bio: __blkdev_direct_IO_async()가 제출한 단일 bio(=struct blkdev_dio에
 *       내장된 bio). 완료 인터럽트 또는 폴에 의해 지금 막 끝났다.
 * @return: void (bio->bi_end_io 콜백 시그니처).
 *
 * __blkdev_direct_IO()의 분할(split) 버전과 달리 이 경로는 항상 단일
 * bio만 사용하므로, dio->ref 같은 카운팅 없이 이 콜백 한 번으로 바로
 * 최종 완료 처리를 수행한다. container_of로 bio가 내장된 blkdev_dio를
 * 곧바로 복원하고, iocb->private을 정리한 뒤 성공/실패에 따라 반환값을
 * 계산해 iocb->ki_complete()로 VFS에 즉시 통지한다. 실행 컨텍스트: 드라이버의
 * 완료 하단부(대개 softirq) 또는 IOCB_HIPRI 폴링을 수행한 태스크 컨텍스트.
 * 호출자: bio_endio() (드라이버 완료 경로에서).
 * 호출 대상: blk_status_to_errno, bio_integrity_unmap_user, ki_complete,
 *           bio_check_pages_dirty, bio_release_pages, bio_put.
 * 에러 경로: bio->bi_status가 설정되어 있으면 errno로 변환해
 * ki_complete()에 그대로 전달 - 상위 VFS 계층(vfs_read/vfs_write)이
 * 최종적으로 이 값을 유저에게 반환한다.
 * 호출 체인:
 *   드라이버 완료 처리 -> blk_mq_end_request -> bio_endio -> [blkdev_bio_end_io_async] -> iocb->ki_complete -> (VFS 비동기 완료 통지)
 */
static void blkdev_bio_end_io_async(struct bio *bio)
{
	struct blkdev_dio *dio = container_of(bio, struct blkdev_dio, bio); /* [한국어] 내장된 bio 포인터로부터 blkdev_dio 컨테이너 주소를 역산 */
	struct kiocb *iocb = dio->iocb; /* [한국어] 완료를 통지할 대상 kiocb를 지역 변수로 확보 */
	ssize_t ret; /* [한국어] VFS에 보고할 최종 반환값 */

	WRITE_ONCE(iocb->private, NULL); /* [한국어] IOCB_HIPRI 폴링을 위해 저장해두었을 수 있는 iocb->private(bio 포인터)을 정리 - 완료되었으니 더 이상 폴 대상이 아님 */

	if (likely(!bio->bi_status)) { /* [한국어] 드라이버가 에러 없이 완료를 보고했는지 확인 - 대부분 성공이므로 likely */
		ret = dio->size; /* [한국어] 성공이면 이 단일 bio가 전송한 총 바이트 수를 반환값으로 사용 */
		iocb->ki_pos += ret; /* [한국어] 파일(장치) 커서를 전송한 만큼 전진 */
	} else {
		ret = blk_status_to_errno(bio->bi_status); /* [한국어] bio.bi_status를 errno로 변환 후 VFS에 전달 */
	}

	if (bio_integrity(bio)) /* [한국어] PI/DIF 메타데이터가 매핑되어 있었는지 확인 */
		bio_integrity_unmap_user(bio); /* [한국어] 매핑 해제 */

	iocb->ki_complete(iocb, ret); /* [한국어] VFS에 비동기 완료를 통지 - 유저 read/write 시스템 콜의 최종 반환값이 됨 */

	if (dio->flags & DIO_SHOULD_DIRTY) { /* [한국어] READ + 사용자 페이지 대상이었는지 확인 */
		bio_check_pages_dirty(bio); /* [한국어] 페이지 dirty 표시(내부에서 지연 해제 포함) */
	} else {
		bio_release_pages(bio, false); /* [한국어] dirty 표시 없이 페이지 고정만 해제 */
		bio_put(bio); /* [한국어] bio(및 내장 dio) 메모리 반납 */
	}
}

/*
 * [한국어]
 * __blkdev_direct_IO_async - 단일 bio로 처리하는 비동기(async) direct I/O
 *
 * @iocb: 비동기(is_sync_kiocb가 거짓인) direct read/write 요청.
 * @iter: 전송할 사용자 buffer를 표현하는 iov_iter.
 * @bdev: 대상 block_device.
 * @nr_pages: 단일 bio에 필요한 bio_vec 개수(BIO_MAX_VECS 이하로 호출자가 보장).
 * @return: 항상 -EIOCBQUEUED(성공적으로 큐잉했다는 의미. 실패는 아래
 *          out_bio_put 경로에서 음수 errno로 즉시 반환).
 *
 * __blkdev_direct_IO_simple()과 유사하게 단일 bio로 요청을 표현하지만,
 * 스택 bio 대신 blkdev_dio_pool에서 힙 bio(+내장 dio)를 할당하고
 * submit_bio_wait() 없이 submit_bio()만 호출한 뒤 곧바로 -EIOCBQUEUED를
 * 반환한다는 점이 다르다. 즉 제출 즉시 반환하며, 실제 완료 통지는 이후
 * blkdev_bio_end_io_async() 콜백이 담당한다. iter가 bvec 기반(이미
 * 페이지가 고정된 파이프/스플라이스 등)이면 값비싼 iov_iter_advance()를
 * 피하기 위해 bio_iov_iter_get_pages() 대신 bio_iov_bvec_set()으로 bvec을
 * 직접 꽂아 넣는 지름길을 사용한다. IOCB_HIPRI(하이 프라이어리티 폴링)가
 * 설정되면 REQ_POLLED를 켜고 bio 포인터를 iocb->private에 저장해, 이후
 * VFS의 .iopoll(iocb_bio_iopoll)이 그 bio 를 직접 폴할 수 있게 한다.
 *
 * 폴링이 왜 존재하는가(NVMe 성능 측정에서 중요한 지점):
 *   일반 경로에서 완료는 장치 인터럽트 -> softirq -> bio_endio -> 태스크
 *   깨우기 순으로 전달된다. 이 경로에는 인터럽트 진입/종료, softirq 예약,
 *   컨텍스트 스위치, 태스크 깨우기가 모두 포함되어 있어서 수 마이크로초
 *   수준의 고정 비용이 붙는다. 장치 자체 지연이 그보다 작아지면(고성능
 *   NVMe SSD 가 그렇다) 이 커널 오버헤드가 지연 시간의 큰 몫을 차지한다.
 *   REQ_POLLED 는 이 요청에 대해 "인터럽트로 알려주지 말라"고 표시하고,
 *   제출한 태스크가 스스로 완료 큐를 반복해서 들여다본다. CPU 를 태우는
 *   대신 깨우기/인터럽트 비용을 없애는 맞교환이다.
 *   실제 경로: io_uring 의 iopoll 루프 -> iocb_bio_iopoll(block/blk-core.c)
 *   -> bio_poll() -> blk_mq_poll() -> q->mq_ops->poll(). NVMe PCIe 에서는
 *   그 실체가 nvme_poll()(drivers/nvme/host/pci.c:2183)이며, 이 함수는
 *   NVMEQ_POLLED 로 표시된 폴링 전용 큐에 대해서만 CQ 를 훑는다. 즉 폴링을
 *   실제로 쓰려면 nvme 모듈의 poll_queues 파라미터로 폴링 전용 큐를 잡아
 *   두어야 하고, 그렇지 않으면 이 플래그를 켜도 인터럽트 경로로 떨어진다.
 *   폴링 가능 여부는 큐 기능 비트 BLK_FEAT_POLL 로 노출되고, bio_poll()은
 *   bi_cookie 가 BLK_QC_T_NONE 이면 그냥 0 을 반환한다.
 *
 * 실행 컨텍스트: 비동기 direct I/O 제출 경로에서 동기적으로 호출되며(제출
 * 까지만), 완료는 별도 컨텍스트(드라이버 완료 인터럽트 또는 이 태스크의
 * 명시적 폴링)에서 이루어진다.
 * 호출자: blkdev_direct_IO (is_sync_kiocb(iocb)가 거짓이고
 *         nr_pages <= BIO_MAX_VECS이며 메타데이터가 없는 경우).
 * 호출 대상: bio_alloc_bioset, bio_iov_bvec_set, blkdev_iov_iter_get_pages,
 *           bio_integrity_map_iter, submit_bio.
 * 에러 경로: 페이지 매핑/메타데이터 매핑 실패 시 out_bio_put 레이블로
 * 점프해 아직 제출되지 않은 bio를 즉시 해제하고 음수 errno를 반환한다
 * (이 경우 완료 콜백은 호출되지 않는다).
 * 호출 체인:
 *   blkdev_direct_IO -> [__blkdev_direct_IO_async] -> submit_bio -> blk_mq_submit_bio -> (mq_ops->queue_rq 간접 호출; NVMe PCIe면 nvme_queue_rq)
 *   -> (완료) bio_endio -> blkdev_bio_end_io_async -> kiocb->ki_complete
 *   (IOCB_HIPRI인 경우) io_uring iopoll -> iocb_bio_iopoll -> bio_poll -> blk_mq_poll -> mq_ops->poll(NVMe PCIe면 nvme_poll)
 */
static ssize_t __blkdev_direct_IO_async(struct kiocb *iocb,
					struct iov_iter *iter,
					struct block_device *bdev,
					unsigned int nr_pages)
{
	bool is_read = iov_iter_rw(iter) == READ; /* [한국어] 요청 방향(READ/WRITE) 판별 */
	blk_opf_t opf = is_read ? REQ_OP_READ : dio_bio_write_op(iocb); /* [한국어] 방향에 맞는 bio opf 결정 */
	struct blkdev_dio *dio; /* [한국어] 이 단일 bio 요청의 완료 상태를 담을 컨테이너 */
	struct bio *bio; /* [한국어] 제출할 단일 bio */
	loff_t pos = iocb->ki_pos; /* [한국어] 시작 오프셋 */
	int ret = 0; /* [한국어] 매핑 단계 에러 코드 */

	bio = bio_alloc_bioset(bdev, nr_pages, opf, GFP_KERNEL, /* [한국어] blkdev_dio_pool에서 bio(+내장 dio) 할당 */
			       &blkdev_dio_pool); /* [한국어] 이 풀에서 나온 bio 여야 container_of 로 blkdev_dio 를 복원할 수 있다 */
	dio = container_of(bio, struct blkdev_dio, bio); /* [한국어] 할당된 bio로부터 blkdev_dio 컨테이너 주소 역산 */
	dio->flags = 0; /* [한국어] 플래그 초기화 - 이후 READ+사용자페이지 조건에서만 DIO_SHOULD_DIRTY 추가 */
	dio->iocb = iocb; /* [한국어] 완료 시 통지할 kiocb 저장 - 이 경로는 항상 비동기이므로 무조건 iocb 사용(union의 waiter 미사용) */
	bio->bi_iter.bi_sector = pos >> SECTOR_SHIFT; /* [한국어] 오프셋을 512B 고정 단위 섹터 번호로 변환 - 장치 논리 블록 크기와 무관한 블록 계층 공통 단위이며, LBA 로의 환산은 드라이버 몫이다 */
	bio->bi_write_hint = file_inode(iocb->ki_filp)->i_write_hint; /* [한국어] 쓰기 힌트 전파 */
	bio->bi_write_stream = iocb->ki_write_stream; /* [한국어] write stream 식별자 전파 */
	bio->bi_end_io = blkdev_bio_end_io_async; /* [한국어] 완료 콜백 등록 */
	bio->bi_ioprio = iocb->ki_ioprio; /* [한국어] I/O 우선순위 전파 */

	if (iov_iter_is_bvec(iter)) { /* [한국어] iter 가 이미 bvec(고정된 페이지 목록) 형태인지 확인 - 그렇다면 페이지를 다시 pin 할 필요 없이 그 배열을 그대로 bio 에 꽂을 수 있다 */
		/*
		 * Users don't rely on the iterator being in any particular
		 * state for async I/O returning -EIOCBQUEUED, hence we can
		 * avoid expensive iov_iter_advance(). Bypass
		 * bio_iov_iter_get_pages() and set the bvec directly.
		 */
		bio_iov_bvec_set(bio, iter); /* [한국어] iter의 bvec 배열을 그대로 bio에 연결 - get_user_pages류의 페이지 고정 과정을 생략하는 지름길 */
	} else { /* [한국어] 일반 iter(유저 buffer 등)면 표준 페이지 고정 경로 사용 */
		ret = blkdev_iov_iter_get_pages(bio, iter, bdev); /* [한국어] 사용자 페이지를 bio bvec에 매핑 */
		if (unlikely(ret)) /* [한국어] 매핑 실패 검사 */
			goto out_bio_put; /* [한국어] 실패 시 정리 후 에러 반환 */
	}
	dio->size = bio->bi_iter.bi_size; /* [한국어] 단일 bio이므로 매핑된 전체 크기가 곧 이 요청의 총 전송 크기 - 단일 bio 총 크기 확정 */

	if (is_read) { /* [한국어] READ 방향이면 사용자 페이지 dirty 필요 여부 확인 */
		if (user_backed_iter(iter)) { /* [한국어] 실제 사용자 주소 공간을 대상으로 하는지 확인 */
			dio->flags |= DIO_SHOULD_DIRTY; /* [한국어] 완료 콜백이 dirty 처리를 하도록 플래그 기록 */
			bio_set_pages_dirty(bio); /* [한국어] 페이지를 미리 dirty로 표시 */
		}
	} else { /* [한국어] WRITE 방향이면 */
		task_io_account_write(bio->bi_iter.bi_size); /* [한국어] 쓰기 바이트 통계 집계 */
	}

	if (iocb->ki_flags & IOCB_HAS_METADATA) { /* [한국어] PI/DIF 메타데이터 동반 여부 확인 */
		ret = bio_integrity_map_iter(bio, iocb->private); /* [한국어] 메타데이터 buffer를 이 bio에 매핑 */
		WRITE_ONCE(iocb->private, NULL); /* [한국어] 매핑에 사용한 iocb->private을 정리(폴링용 bio 저장과 용도가 겹치므로 미리 비움) */
		if (unlikely(ret)) /* [한국어] 매핑 실패 검사 */
			goto out_bio_put; /* [한국어] 실패 시 정리 후 에러 반환 */
	}

	if (iocb->ki_flags & IOCB_ATOMIC) /* [한국어] 원자적 쓰기 요청인지 확인 */
		bio->bi_opf |= REQ_ATOMIC; /* [한국어] IOCB_ATOMIC -> REQ_ATOMIC: 찢어지지 않는(torn write 없는) 쓰기 요구. 한도 검증은 드라이버가 한다 - NVMe 는 nvme_valid_atomic_write()(core.c:1235) */

	if (iocb->ki_flags & IOCB_NOWAIT) /* [한국어] 논블로킹 요청인지 확인 */
		bio->bi_opf |= REQ_NOWAIT; /* [한국어] 자원(태그 등) 부족으로 슬립해야 하는 상황이면 대기하지 말고 BLK_STS_AGAIN 으로 즉시 실패하라는 표시 */

	if (iocb->ki_flags & IOCB_HIPRI) { /* [한국어] 하이 프라이어리티(=폴링) 요청인지 확인. io_uring 의 IORING_SETUP_IOPOLL 이나 preadv2/pwritev2 의 RWF_HIPRI 가 여기까지 내려온다 */
		bio->bi_opf |= REQ_POLLED; /* [한국어] "이 요청의 완료는 인터럽트로 통지하지 말고 제출자가 직접 확인한다"는 표시. blk-mq 가 이 플래그를 보고 폴링 가능한 hctx 로 요청을 보내고 bi_cookie 를 채워 준다 */
		submit_bio(bio); /* [한국어] bio 제출. 폴링 경로에서는 제출을 먼저 하고 그 다음에 private 를 채우는 순서가 중요하다 - 제출 전에 채우면 아직 bi_cookie 가 없는 bio 를 폴하게 된다 */
		WRITE_ONCE(iocb->private, bio); /* [한국어] 폴 대상 bio 를 iocb->private 에 저장 - 이후 .iopoll(iocb_bio_iopoll)이 RCU 보호 하에 이 포인터를 읽어 bio_poll()을 호출한다. 완료 콜백은 같은 자리를 NULL 로 지운다 */
	} else { /* [한국어] 일반(인터럽트 기반) 완료 경로 */
		submit_bio(bio); /* [한국어] bio 제출 - 완료는 드라이버의 완료 처리 경로가 bio_endio -> blkdev_bio_end_io_async 를 호출해 처리한다 */
	}
	return -EIOCBQUEUED; /* [한국어] 제출 완료, 실제 결과는 나중에 ki_complete를 통해 통지됨을 VFS에 알림 */

out_bio_put:
	bio_put(bio); /* [한국어] 제출되지 않은 bio를 즉시 해제 */
	return ret; /* [한국어] 매핑 실패로 인한 음수 errno 반환 */
}

/*
 * [한국어]
 * blkdev_direct_IO - VFS direct I/O(O_DIRECT read/write)의 단일 진입점
 *
 * @iocb: read_iter/write_iter로부터 전달된 kiocb. ki_filp로 block_device를
 *        찾고, ki_flags(IOCB_DIRECT가 이미 설정된 상태로 호출됨)로 세부
 *        동작을 조정한다.
 * @iter: 전송할 buffer를 표현하는 iov_iter.
 * @return: 0(빈 iter), 음수 errno(정렬/스트림/atomic 검증 실패), 또는
 *          하위 함수(__blkdev_direct_IO_simple/_async/__blkdev_direct_IO)의
 *          반환값을 그대로 전달.
 *
 * 이 함수는 struct address_space_operations는 아니지만 struct
 * file_operations의 read_iter/write_iter 구현(blkdev_read_iter/
 * blkdev_write_iter)이 IOCB_DIRECT일 때 공통으로 호출하는 direct I/O의
 * 단일 진입점이다. 먼저 정렬(blkdev_dio_invalid)과 write stream 유효성을
 * 검사한 뒤, 요청에 필요한 bio_vec 개수(nr_pages)를 계산해 세 경로 중
 * 하나로 분기한다.
 *
 * 분기 조건을 코드 그대로 정리하면:
 *   nr_pages = bio_iov_vecs_to_alloc(iter, BIO_MAX_VECS + 1)
 *     — 이 요청을 담는 데 필요한 bio_vec 개수. 상한을 BIO_MAX_VECS+1 로
 *       주는 이유는 "한 bio 로 되는가 아닌가"만 알면 되기 때문이다.
 *   (1) nr_pages <= BIO_MAX_VECS && !IOCB_HAS_METADATA && is_sync_kiocb
 *       -> __blkdev_direct_IO_simple(): struct bio 를 함수 스택에 두고
 *          submit_bio_wait() 로 제출-대기를 한 함수 안에서 끝낸다.
 *          힙 할당이 0 번이라 가장 짧은 경로다.
 *   (2) nr_pages <= BIO_MAX_VECS && !IOCB_HAS_METADATA && 비동기
 *       -> __blkdev_direct_IO_async(): bio 하나를 blkdev_dio_pool 에서
 *          받아 제출하고 즉시 -EIOCBQUEUED 로 돌아온다. io_uring/AIO 의
 *          일반 경로이며, IOCB_HIPRI 폴링도 여기서만 활성화된다.
 *   (3) 그 외(BIO_MAX_VECS 초과 = 한 bio 에 안 들어감, 또는 PI/DIF
 *       메타데이터 동반)
 *       -> __blkdev_direct_IO(): 여러 bio 로 쪼개 연속 제출하고
 *          struct blkdev_dio 의 ref 카운터로 완료를 집계한다.
 *   단, IOCB_ATOMIC 인데 (3) 으로 가야 하는 경우는 -EINVAL 이다. 요청을
 *   여러 bio 로 쪼개는 순간 "전부 아니면 전무"라는 원자성 약속을 지킬 수
 *   없기 때문이다. 실행 컨텍스트: read/write 시스템 콜을 처리하는 유저 프로세스의
 * 커널 스레드 컨텍스트에서 동기 호출된다.
 * 호출자: blkdev_read_iter, blkdev_write_iter (모두 IOCB_DIRECT일 때).
 * 호출 대상: blkdev_dio_invalid, bdev_max_write_streams,
 *           bio_iov_vecs_to_alloc, __blkdev_direct_IO_simple,
 *           __blkdev_direct_IO_async, __blkdev_direct_IO.
 * 에러 경로: 정렬 실패(-EINVAL), atomic+분할 조합 거부(-EINVAL) 외에는
 * 모두 하위 함수의 에러 경로에 위임한다.
 * 호출 체인:
 *   vfs_read/vfs_write -> kiocb->read_iter/write_iter -> blkdev_read_iter/blkdev_write_iter -> [blkdev_direct_IO]
 *   -> (__blkdev_direct_IO_simple/__blkdev_direct_IO_async/__blkdev_direct_IO) -> submit_bio -> blk_mq_submit_bio -> mq_ops->queue_rq (NVMe면 nvme_queue_rq)
 */
static ssize_t blkdev_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
	struct block_device *bdev = I_BDEV(iocb->ki_filp->f_mapping->host); /* [한국어] kiocb가 가리키는 파일의 bdev inode로부터 block_device 포인터 획득 */
	unsigned int nr_pages; /* [한국어] 이 요청을 표현하는 데 필요한 bio_vec(페이지) 개수 */

	if (!iov_iter_count(iter)) /* [한국어] 전송할 데이터가 0바이트인지 확인 */
		return 0; /* [한국어] 빈 요청은 아무 것도 하지 않고 즉시 0(전송 바이트 0) 반환 */

	if (blkdev_dio_invalid(bdev, iocb, iter)) /* [한국어] offset/len 이 논리 블록 크기 배수인지 검사 - NVMe 라면 그 크기가 1 << LBADS 다 */
		return -EINVAL; /* [한국어] 미정렬이면 하위 계층에 내려보내지 않고 즉시 거부 */

	if (iov_iter_rw(iter) == WRITE) { /* [한국어] WRITE 방향이면 write stream 번호를 확정하는 분기로 진입 - 장치가 데이터 배치(placement)를 분리할 수 있게 하는 힌트다 */
		u16 max_write_streams = bdev_max_write_streams(bdev); /* [한국어] 이 장치가 지원하는 최대 write stream 개수 조회 - NVMe 에서는 lim.max_write_streams = ns->head->nr_plids, 즉 FDP 가 노출한 placement identifier 개수다(core.c:2829) */

		if (iocb->ki_write_stream) { /* [한국어] 유저가 RWF_WRITE_STREAM 등으로 명시적 스트림 번호를 지정했는지 확인 */
			if (iocb->ki_write_stream > max_write_streams) /* [한국어] 지정된 스트림 번호가 장치 지원 범위를 초과하는지 검사 */
				return -EINVAL; /* [한국어] 범위 초과 시 요청 거부 */
		} else if (max_write_streams) { /* [한국어] 명시적 지정은 없지만 장치가 스트림을 지원하는 경우 */
			enum rw_hint write_hint = /* [한국어] inode에 설정된 쓰기 힌트(rw_hint)를 스트림 후보로 사용 */
				file_inode(iocb->ki_filp)->i_write_hint; /* [한국어] fcntl(F_SET_RW_HINT) 등으로 설정된 수명 힌트를 그대로 스트림 번호 후보로 삼는다 */

			/*
			 * Just use the write hint as write stream for block
			 * device writes.  This assumes no file system is
			 * mounted that would use the streams differently.
			 */
			if (write_hint <= max_write_streams) /* [한국어] 힌트 값이 유효한 스트림 번호 범위인지 확인 */
				iocb->ki_write_stream = write_hint; /* [한국어] 힌트를 그대로 write stream 번호로 채택 - 이후 bio.bi_write_stream 으로 전파되고, NVMe 에서는 nvme_setup_rw()가 ns->head->plids[stream-1] 의 FDP placement identifier 로 변환한다(core.c:1287-1288) */
		}
	}

	nr_pages = bio_iov_vecs_to_alloc(iter, BIO_MAX_VECS + 1); /* [한국어] 이 요청 전체에 필요한 bio_vec 개수를 계산(최대 BIO_MAX_VECS+1까지만 계산해 분할 필요 여부 판별) - 이 값으로 단순/비동기/분할 경로 선택 */
	if (likely(nr_pages <= BIO_MAX_VECS && /* [한국어] 단일 bio로 표현 가능한 크기이고 */
		   !(iocb->ki_flags & IOCB_HAS_METADATA))) { /* [한국어] PI/DIF 메타데이터를 동반하지 않는 단순한 경우인지 확인 - 대부분의 I/O가 여기 해당하므로 likely */
		if (is_sync_kiocb(iocb)) /* [한국어] 동기 kiocb이면 */
			return __blkdev_direct_IO_simple(iocb, iter, bdev, /* [한국어] 스택 bio 기반 최소 지연 경로 사용 */
							nr_pages); /* [한국어] 필요한 bio_vec 개수를 그대로 넘긴다 - 4 이하면 bio_vec 배열까지 스택에 잡힌다 */
		return __blkdev_direct_IO_async(iocb, iter, bdev, nr_pages); /* [한국어] 비동기 kiocb이면 단일 힙 bio + 즉시 반환 경로 사용 */
	} else if (iocb->ki_flags & IOCB_ATOMIC) { /* [한국어] 분할이 필요한 상황인데 원자적 쓰기가 요청되었는지 확인 - atomic write는 분할 경로를 지원하지 않음 */
		return -EINVAL; /* [한국어] 원자성을 보장할 수 없으므로 거부 */
	}
	return __blkdev_direct_IO(iocb, iter, bdev, bio_max_segs(nr_pages)); /* [한국어] 그 외(대형 I/O 또는 메타데이터 동반)는 분할 direct I/O 경로 사용 - bio_max_segs로 한 bio당 세그먼트 상한 재적용 */
}

/*
 * [한국어]
 * blkdev_iomap_begin - 파일 offset을 block device의 LBA 영역으로 매핑(iomap 콜백)
 *
 * @inode: 대상 bdev inode.
 * @offset: 매핑하려는 파일(장치) 오프셋.
 * @length: 사용되지 않음(요청 길이, block device는 항상 EOF까지 한 번에 매핑).
 * @flags: iomap 플래그(IOMAP_WRITE 등), 이 구현에서는 참조하지 않음.
 * @iomap: 결과를 채워 넣을 출력 매개변수.
 * @srcmap: COW 등에서 사용하는 원본 매핑(block device에는 해당 없어 미사용).
 * @return: 0 성공, offset이 장치 크기를 벗어나면 -EIO.
 *
 * struct iomap_ops.iomap_begin 콜백 구현으로, block device 자체는
 * 파일시스템처럼 "구멍(hole)"이나 익스텐트 매핑이 없이 처음부터 끝까지
 * 통째로 매핑되어 있다고 간주한다. 그래서 한 번 호출로 offset부터 장치
 * 끝(isize)까지를 통째로 IOMAP_MAPPED로 반환한다 - 이후 iomap 코어가
 * writeback_range/read_folio 등에서 이 매핑 정보(iomap->bdev, offset,
 * length)를 이용해 실제 bio를 만들고 bio.bi_sector를 채운다. 실행
 * 컨텍스트: buffered read/write/readahead/writeback 경로에서 iomap 코어에
 * 의해 호출되며 락 없이 단순 계산만 수행한다.
 * 호출자: iomap 코어(fs/iomap/*) - blkdev_read_folio(iomap 버전),
 *         blkdev_readahead(iomap 버전), blkdev_writeback_range 등을 통해
 *         간접 호출됨.
 * 호출 대상: I_BDEV, i_size_read, ALIGN_DOWN, bdev_logical_block_size.
 * 에러 경로: offset이 파일(장치) 크기 이상이면 -EIO를 반환해 iomap 코어가
 * 더 이상 진행하지 않도록 한다.
 * 호출 체인:
 *   iomap_bio_read_folio/iomap_bio_readahead/blkdev_writeback_range -> [blkdev_iomap_begin] -> (iomap 채움) -> 상위에서 bio 생성 -> submit_bio
 */
static int blkdev_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
		unsigned int flags, struct iomap *iomap, struct iomap *srcmap)
{
	struct block_device *bdev = I_BDEV(inode); /* [한국어] inode로부터 block_device 포인터 획득 */
	loff_t isize = i_size_read(inode); /* [한국어] 장치의 현재 크기(바이트) 조회 */

	if (offset >= isize) /* [한국어] 요청 offset이 장치 끝을 벗어났는지 확인 */
		return -EIO; /* [한국어] 벗어났으면 매핑 불가 - I/O 에러로 보고 */

	iomap->bdev = bdev; /* [한국어] 이후 bio 생성 시 사용할 bdev 지정 */
	iomap->offset = ALIGN_DOWN(offset, bdev_logical_block_size(bdev)); /* [한국어] 요청 offset 을 논리 블록 경계로 내림 정렬 - 장치가 주소지정할 수 있는 최소 단위 아래로는 매핑을 시작할 수 없다 */
	iomap->type = IOMAP_MAPPED; /* [한국어] IOMAP_MAPPED: bdev의 이 영역은 항상 이미 매핑되어 있음(구멍 없음) */
	iomap->addr = iomap->offset; /* [한국어] block device는 파일 오프셋과 장치 상의 주소가 동일(1:1 매핑) */
	iomap->length = isize - iomap->offset; /* [한국어] 정렬된 offset부터 장치 끝까지를 한 번에 매핑 범위로 제공 */
	iomap->flags |= IOMAP_F_BUFFER_HEAD; /* noop for !CONFIG_BUFFER_HEAD */ /* [한국어] CONFIG_BUFFER_HEAD 빌드에서 buffer_head 기반 처리가 필요함을 표시(그 외 빌드에서는 무시되는 플래그) */
	return 0; /* [한국어] 매핑 성공 */
}

static const struct iomap_ops blkdev_iomap_ops = { /* [한국어] buffered I/O(iomap 경로)가 bio를 만들기 위해 필요한 bdev/LBA 정보를 제공하는 콜백 테이블 - iomap_begin 하나만 구현(block device는 매핑 해제/커밋 단계가 불필요) */
	.iomap_begin		= blkdev_iomap_begin, /* [한국어] 오프셋 -> 장치 매핑을 돌려주는 유일한 콜백. .iomap_end 가 없다는 것은 매핑 후 정리할 상태가 없다는 뜻이다 */
};

#ifdef CONFIG_BUFFER_HEAD /* [한국어] 커널이 buffer_head 기반 페이지 캐시 계층을 사용하도록 빌드된 경우(레거시 경로) - 아래 블록 전체가 buffer_head API로 def_blk_aops를 구현 */
/*
 * [한국어]
 * blkdev_get_block - buffer_head를 파일(장치) block 번호로 매핑(get_block_t 콜백)
 *
 * @inode: 대상 bdev inode.
 * @iblock: inode 관점의 논리 block 번호(0부터 시작, 페이지 캐시 block 크기 단위).
 * @bh: 매핑 결과를 채워 넣을 buffer_head.
 * @create: 새 블록 할당이 필요한지 여부 - block device는 항상 이미 존재하는
 *          공간이므로 이 값은 사용하지 않는다.
 * @return: 항상 0(block device는 매핑 실패가 없음 - 범위를 벗어나면 상위
 *          buffer_head 계층이 별도로 처리).
 *
 * block device 자신을 대상으로 하는 buffer_head는 파일시스템의 익스텐트
 * 탐색 없이 "논리 block 번호 == 장치 상의 물리 block 번호"라는 항등
 * 매핑을 가진다. 그래서 bdev와 block 번호만 그대로 buffer_head에 채우고
 * set_buffer_mapped()로 매핑되었음을 표시하면 끝난다. 이 bh->b_blocknr가
 * 이후 submit_bh/bio 생성 과정에서 bio.bi_sector로 변환되어 최종적으로
 * 그대로 bio 의 bi_sector 가 된다. 실행 컨텍스트: 페이지 캐시 read/
 * writeback 경로에서 block_read_full_folio/block_write_full_folio에
 * 의해 호출되며 락 없이 단순 대입만 수행한다.
 * 호출자: block_read_full_folio (blkdev_read_folio 경유),
 *         block_write_full_folio (blkdev_writepages 경유),
 *         mpage_readahead (blkdev_readahead 경유),
 *         block_write_begin (blkdev_write_begin 경유).
 * 호출 대상: I_BDEV, set_buffer_mapped.
 * 에러 경로: 없음(항상 성공).
 * 호출 체인:
 *   block_read_full_folio/block_write_full_folio/mpage_readahead -> [blkdev_get_block] -> (bh 매핑 완료) -> submit_bh/bio 생성 -> submit_bio -> blk_mq_submit_bio -> mq_ops->queue_rq(NVMe면 nvme_queue_rq)
 */
static int blkdev_get_block(struct inode *inode, sector_t iblock,
		struct buffer_head *bh, int create)
{
	bh->b_bdev = I_BDEV(inode); /* [한국어] 이 buffer_head가 속한 block_device 지정 - buffer_head에 bdev 설정 */
	bh->b_blocknr = iblock; /* [한국어] 논리 block 번호를 그대로 물리 block 번호로 사용(항등 매핑) - inode offset에서 LBA(blocknr) 결정 */
	set_buffer_mapped(bh); /* [한국어] 이 buffer_head가 유효한 매핑을 가짐을 표시 - 이후 계층이 매핑 여부를 이 플래그로 판단 */
	return 0; /* [한국어] 항상 성공 */
}

/*
 * We cannot call mpage_writepages() as it does not take the buffer lock.
 * We must use block_write_full_folio() directly which holds the buffer
 * lock.  The buffer lock provides the synchronisation with writeback
 * that filesystems rely on when they use the blockdev's mapping.
 */
/*
 * [한국어]
 * blkdev_writepages (CONFIG_BUFFER_HEAD) - 페이지 캐시의 더티 folio 를 쓰기 bio 로 writeback
 *
 * @mapping: 대상 bdev의 address_space.
 * @wbc: writeback 제어 정보(범위, 동기 여부 등).
 * @return: 0 성공, 그 외 마지막으로 실패한 folio의 에러 코드.
 *
 * struct address_space_operations.writepages 구현. 위 원본 영어 주석이
 * 설명하듯, mpage_writepages()는 buffer lock을 잡지 않아 이 파일시스템(?)
 * 이 block device의 mapping을 사용할 때 필요한 writeback 동기화를 제공하지
 * 못하므로, buffer lock을 직접 잡는 block_write_full_folio()를 사용해야
 * 한다. writeback_iter()로 wbc 범위 내 더티 folio를 하나씩 순회하며 각각
 * block_write_full_folio()에 blkdev_get_block을 넘겨 bio로 변환/제출한다.
 * blk_start_plug/blk_finish_plug로 감싸 여러 folio의 bio 제출을 배치
 * (batch)한다. 실행 컨텍스트: writeback 커널 스레드(kworker) 또는
 * fsync/msync 등을 호출한 태스크 컨텍스트에서 실행된다.
 * 호출자: writeback 코어(mm/page-writeback.c)의 .writepages 콜백 호출 경로,
 *         blkdev_fsync -> file_write_and_wait_range를 통한 간접 호출.
 * 호출 대상: writeback_iter, block_write_full_folio, blk_start_plug/
 *           blk_finish_plug.
 * 에러 경로: block_write_full_folio 실패 시 err에 최종 에러가 남아 반환됨
 * (writeback_iter가 반복 중 에러를 계속 갱신).
 * 호출 체인:
 *   writeback 코어/blkdev_fsync -> [blkdev_writepages] -> block_write_full_folio -> submit_bio -> blk_mq_submit_bio -> mq_ops->queue_rq(NVMe면 nvme_queue_rq)
 */
static int blkdev_writepages(struct address_space *mapping,
		struct writeback_control *wbc)
{
	struct folio *folio = NULL; /* [한국어] writeback_iter 순회 상태를 담는 커서 - 최초 호출은 NULL로 시작 */
	struct blk_plug plug; /* [한국어] 여러 folio의 bio 제출을 배치하기 위한 plug */
	int err; /* [한국어] writeback_iter가 채워주는 에러 코드 */

	blk_start_plug(&plug); /* [한국어] 배치 제출 구간 시작 */
	while ((folio = writeback_iter(mapping, wbc, folio, &err))) /* [한국어] wbc 범위 내 다음 더티 folio를 하나씩 획득 - NULL이 반환되면 순회 종료 */
		err = block_write_full_folio(folio, wbc, blkdev_get_block); /* [한국어] 각 더티 folio 를 buffer_head 잠금과 함께 REQ_OP_WRITE bio 로 변환해 제출 */
	blk_finish_plug(&plug); /* [한국어] 배치 제출 구간 종료 - 지금까지 모인 bio들을 실제로 큐에 반영 */

	return err; /* [한국어] 마지막 folio 처리 결과(에러 코드) 반환 */
}

/*
 * [한국어]
 * blkdev_read_folio (CONFIG_BUFFER_HEAD) - 단일 folio 를 읽기 bio 로 채움
 *
 * @file: 사용하지 않음(콜백 시그니처 준수용).
 * @folio: 채워야 할 페이지 캐시 folio.
 * @return: block_read_full_folio()의 반환값(통상 0, folio 잠금 해제는 내부에서 처리).
 *
 * struct address_space_operations.read_folio 구현. block_read_full_folio가
 * blkdev_get_block로 얻은 매핑 정보를 이용해 bio를 조립하고 제출한다.
 * 실행 컨텍스트: 페이지 캐시 미스 시 filemap_read 등에 의해 호출되는 유저
 * 프로세스 컨텍스트.
 * 호출자: filemap_read (buffered read 경로에서 캐시 미스 시).
 * 호출 대상: block_read_full_folio, blkdev_get_block(콜백으로 전달).
 * 에러 경로: block_read_full_folio 내부에서 bio 에러를 folio 상태에 반영.
 * 호출 체인:
 *   filemap_read -> [blkdev_read_folio] -> block_read_full_folio -> submit_bio -> blk_mq_submit_bio -> mq_ops->queue_rq(NVMe면 nvme_queue_rq)
 */
static int blkdev_read_folio(struct file *file, struct folio *folio)
{
	return block_read_full_folio(folio, blkdev_get_block); /* [한국어] buffer_head 기반으로 folio를 채우는 표준 헬퍼에 block 매핑 콜백을 전달 */
}

/*
 * [한국어]
 * blkdev_readahead (CONFIG_BUFFER_HEAD) - 순차 읽기를 읽기 bio 로 prefetch
 *
 * @rac: readahead 대상 범위/장치 정보를 담은 제어 구조체.
 * @return: void.
 *
 * struct address_space_operations.readahead 구현. mpage_readahead가 rac가
 * 지정한 범위의 여러 folio를 모아 가능한 한 적은 수의 bio로 뭉쳐 제출한다.
 * 실행 컨텍스트: 순차 read 패턴을 감지한 페이지 캐시 계층이 호출하며, 보통
 * 유저 프로세스 컨텍스트에서 비동기로 결과를 기다리지 않고 제출만 한다.
 * 호출자: 페이지 캐시 readahead 코어(mm/readahead.c).
 * 호출 대상: mpage_readahead, blkdev_get_block(콜백).
 * 에러 경로: 개별 bio 실패는 각 folio의 상태로 반영되며 이 함수는 별도
 * 반환값이 없다.
 * 호출 체인:
 *   페이지 캐시 readahead 코어 -> [blkdev_readahead] -> mpage_readahead -> submit_bio(여러 개 묶어) -> blk_mq_submit_bio -> mq_ops->queue_rq(NVMe면 nvme_queue_rq)
 */
static void blkdev_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, blkdev_get_block); /* [한국어] 여러 folio 를 가능한 한 적은 수의 bio 로 묶어 제출 - 순차 prefetch bio 조립 */
}

/*
 * [한국어]
 * blkdev_write_begin (CONFIG_BUFFER_HEAD) - buffered write를 위한 folio 준비
 *
 * @iocb: 쓰기 요청(대부분 사용되지 않고 시그니처 준수용).
 * @mapping: 대상 bdev의 address_space.
 * @pos: 쓰기 시작 오프셋.
 * @len: 쓸 바이트 길이.
 * @foliop: 준비된 folio를 반환할 출력 매개변수.
 * @fsdata: 사용하지 않음.
 * @return: block_write_begin()의 반환값(0 성공, 음수 errno 실패).
 *
 * struct address_space_operations.write_begin 구현. 해당 범위의 folio를
 * 페이지 캐시에서 찾거나 새로 만들고 필요한 buffer_head를 매핑(get_block
 * 콜백 사용)해 이후 write_end에서 사용자 데이터를 복사할 수 있는 상태로
 * 준비한다. 실행 컨텍스트: buffered write 시스템 콜을 처리하는 유저
 * 프로세스 컨텍스트.
 * 호출자: iomap_file_buffered_write 대신 buffer_head 빌드에서는 generic
 * buffered write 경로(이 파일 자체에서는 blkdev_buffered_write가 iomap을
 * 사용하므로, 이 콜백은 CONFIG_BUFFER_HEAD 빌드에서 다른 경로로 호출될 수 있음).
 * 호출 대상: block_write_begin, blkdev_get_block(콜백).
 * 에러 경로: block_write_begin 실패 시 그대로 상위에 전달.
 * 호출 체인:
 *   generic buffered write 경로 -> [blkdev_write_begin] -> block_write_begin -> blkdev_get_block
 */
static int blkdev_write_begin(const struct kiocb *iocb,
			      struct address_space *mapping, loff_t pos,
			      unsigned len, struct folio **foliop,
			      void **fsdata)
{
	return block_write_begin(mapping, pos, len, foliop, blkdev_get_block); /* [한국어] 쓸 folio 를 확보하고 buffer_head 매핑까지 마친다 - 실제 제출은 이후 writeback 이 담당 */
}

/*
 * [한국어]
 * blkdev_write_end (CONFIG_BUFFER_HEAD) - buffered write folio 마무리
 *
 * @iocb: 사용하지 않음.
 * @mapping: 사용하지 않음(시그니처 준수용).
 * @pos: 쓰기 시작 오프셋(block_write_end 내부에서 사용).
 * @len: 요청된 쓰기 길이.
 * @copied: 실제로 사용자 buffer에서 folio로 복사된 바이트 수.
 * @folio: write_begin에서 준비된 folio.
 * @fsdata: 사용하지 않음.
 * @return: block_write_end()의 반환값(실제 반영된 바이트 수).
 *
 * struct address_space_operations.write_end 구현. block_write_end가
 * buffer_head들을 dirty로 표시해 이후 writeback(blkdev_writepages)에서
 * 이후 writeback 이 쓰기 bio 로 제출하도록 준비한다. 이 함수 자신은 bio 를 만들지
 * 않고 folio 잠금 해제 및 참조 반납만 수행한다. 실행 컨텍스트: buffered
 * write 시스템 콜을 처리하는 유저 프로세스 컨텍스트.
 * 호출자: generic buffered write 경로(write_begin과 짝을 이루어 호출).
 * 호출 대상: block_write_end, folio_unlock, folio_put.
 * 에러 경로: block_write_end의 반환값을 그대로 전달.
 * 호출 체인:
 *   generic buffered write 경로 -> [blkdev_write_end] -> block_write_end -> (folio dirty 상태로 남음) -> 이후 blkdev_writepages 가 쓰기 bio 제출
 */
static int blkdev_write_end(const struct kiocb *iocb,
			    struct address_space *mapping,
			    loff_t pos, unsigned len, unsigned copied,
			    struct folio *folio, void *fsdata)
{
	int ret; /* [한국어] block_write_end의 반환값(반영된 바이트 수)을 담을 변수 */
	ret = block_write_end(pos, len, copied, folio); /* [한국어] buffer_head들을 dirty로 표시하고 실제 반영된 길이를 계산 */

	folio_unlock(folio); /* [한국어] write_begin에서 잠갔던 folio 잠금을 해제 - 다른 접근자가 folio를 사용할 수 있게 함 */
	folio_put(folio); /* [한국어] write_begin에서 얻었던 folio 참조를 반납 */

	return ret; /* [한국어] 실제 반영된 바이트 수 반환 */
}

/*
 * [한국어]
 * def_blk_aops (CONFIG_BUFFER_HEAD) - buffer_head 기반 페이지 캐시 경유 block 장치 연산 테이블
 *
 * readahead/read_folio/writepages/write_begin/write_end 등이 buffer_head를
 * 매개로 bio를 생성하여 submit_bio -> blk_mq_submit_bio -> mq_ops->queue_rq(NVMe면 nvme_queue_rq)로
 * 전달한다. 이 테이블은
 * CONFIG_BUFFER_HEAD가 켜진 빌드에서만 사용되며, 꺼진 빌드에서는 아래
 * #else 블록의 iomap 기반 def_blk_aops가 대신 컴파일된다(같은 이름의
 * 전역 심볼이 조건부로 하나만 존재).
 */
const struct address_space_operations def_blk_aops = {
	.dirty_folio	= block_dirty_folio, /* [한국어] folio를 dirty로 표시하는 표준 buffer_head 헬퍼 연결 */
	.invalidate_folio = block_invalidate_folio, /* [한국어] folio 무효화(예: truncate) 시 buffer_head 정리 헬퍼 연결 */
	.read_folio	= blkdev_read_folio, /* [한국어] 단일 folio 를 읽어 채우는 콜백 - 페이지 캐시 미스 시 filemap_read 가 호출 */
	.readahead	= blkdev_readahead, /* [한국어] 순차 접근이 감지되었을 때 여러 folio 를 미리 읽는 콜백 */
	.writepages	= blkdev_writepages, /* [한국어] 더티 folio 들을 실제 쓰기 bio 로 내려보내는 writeback 콜백 */
	.write_begin	= blkdev_write_begin, /* [한국어] buffered write 준비 */
	.write_end	= blkdev_write_end, /* [한국어] buffered write 마무리 */
	.migrate_folio	= buffer_migrate_folio_norefs, /* [한국어] 메모리 압축/마이그레이션 시 buffer_head를 보존하며 folio 이동 */
	.is_dirty_writeback = buffer_check_dirty_writeback, /* [한국어] folio가 dirty/writeback 중인지 buffer_head 기준으로 판정 */
};
#else /* CONFIG_BUFFER_HEAD */ /* [한국어] buffer_head 대신 iomap 기반 페이지 캐시 계층을 사용하는 최신 빌드(기본 경로) - 아래부터 #endif까지가 이 경우의 def_blk_aops 구현 */
/*
 * [한국어]
 * blkdev_read_folio (iomap 버전) - iomap을 통한 단일 folio read
 *
 * @file: 사용하지 않음.
 * @folio: 채워야 할 페이지 캐시 folio.
 * @return: 항상 0 - iomap_bio_read_folio가 완료/에러 처리를 folio 상태로
 *          비동기 반영하므로 이 함수 자체는 즉시 반환한다.
 *
 * struct address_space_operations.read_folio 구현(iomap 버전).
 * iomap_bio_read_folio가 blkdev_iomap_ops(=blkdev_iomap_begin)로 얻은
 * bdev/오프셋 매핑을 바탕으로 bio를 조립해 제출한다. 실행 컨텍스트:
 * 페이지 캐시 미스 시 filemap_read에 의해 호출되는 유저 프로세스 컨텍스트.
 * 호출자: filemap_read.
 * 호출 대상: iomap_bio_read_folio.
 * 에러 경로: bio 실패는 folio 상태(uptodate/error)로 비동기 반영되며,
 * 이 함수 자체는 에러를 반환하지 않는다.
 * 호출 체인:
 *   filemap_read -> [blkdev_read_folio] -> iomap_bio_read_folio -> blkdev_iomap_begin(매핑) -> bio 생성 -> submit_bio -> blk_mq_submit_bio -> mq_ops->queue_rq(NVMe면 nvme_queue_rq)
 */
static int blkdev_read_folio(struct file *file, struct folio *folio)
{
	iomap_bio_read_folio(folio, &blkdev_iomap_ops); /* [한국어] iomap 코어에 매핑 콜백 테이블을 넘겨 읽기 bio 조립/제출을 위임 */
	return 0; /* [한국어] 제출은 비동기이므로 즉시 0 반환(완료는 folio 상태로 별도 반영) */
}

/*
 * [한국어]
 * blkdev_readahead (iomap 버전) - iomap 기반 순차 prefetch
 *
 * @rac: readahead 대상 범위/장치 정보.
 * @return: void.
 *
 * struct address_space_operations.readahead 구현(iomap 버전).
 * iomap_bio_readahead가 rac 범위의 여러 folio를 모아 적은 수의 bio로
 * 묶어 제출한다. 실행 컨텍스트: 순차 read 패턴을 감지한 페이지 캐시
 * readahead 코어가 호출.
 * 호출자: 페이지 캐시 readahead 코어(mm/readahead.c).
 * 호출 대상: iomap_bio_readahead.
 * 에러 경로: 개별 bio 실패는 folio 단위 상태로 반영.
 * 호출 체인:
 *   페이지 캐시 readahead 코어 -> [blkdev_readahead] -> iomap_bio_readahead -> submit_bio(여러 개) -> blk_mq_submit_bio -> mq_ops->queue_rq(NVMe면 nvme_queue_rq)
 */
static void blkdev_readahead(struct readahead_control *rac)
{
	iomap_bio_readahead(rac, &blkdev_iomap_ops); /* [한국어] iomap 코어에 위임 - 여러 folio 를 아우르는 읽기 bio 를 생성 */
}

/*
 * [한국어]
 * blkdev_writeback_range - iomap writeback에서 한 folio 범위를 진행 중인 ioend에 추가
 *
 * @wpc: iomap writeback 컨텍스트 - 캐시된 iomap(wpc->iomap)과 누적 중인
 *       ioend(출력 bio 체인)를 보유.
 * @folio: writeback 대상 더티 folio.
 * @offset: 이 folio 내에서 처리할 시작 오프셋(파일 전체 기준).
 * @len: 처리할 길이.
 * @end_pos: 이 writeback 단위의 끝 위치(짧은 쓰기 등 EOF 처리에 사용).
 * @return: 처리된 바이트 수(iomap_add_to_ioend의 반환값), 실패 시 음수 errno.
 *
 * struct iomap_writeback_ops.writeback_range 구현. offset이 현재 캐시된
 * wpc->iomap 범위를 벗어나면(최초 호출이거나 이전 매핑 범위를 넘어간
 * 경우) blkdev_iomap_begin()을 다시 호출해 매핑을 갱신한 뒤,
 * iomap_add_to_ioend()로 이 folio의 데이터를 진행 중인 bio(ioend)에
 * 추가한다. 여러 folio가 연속된 LBA를 가리키면 하나의 bio로 병합되어
 * 실제 제출되는 쓰기 요청 수를 줄이는 효과가 있다. 실행 컨텍스트: writeback
 * 코어(iomap_writepages)가 호출하는 kworker 또는 fsync 호출 태스크
 * 컨텍스트.
 * 호출자: iomap_writepages (blkdev_writepages를 통해 등록된 콜백 테이블 경유).
 * 호출 대상: i_size_read, blkdev_iomap_begin, iomap_add_to_ioend.
 * 에러 경로: offset이 파일 크기를 벗어나면 -EIO(디버그 빌드에서
 * WARN_ON_ONCE로 콘솔에도 경고), 재매핑 실패 시 그 에러를 그대로 전달.
 * 호출 체인:
 *   iomap_writepages -> [blkdev_writeback_range] -> (필요 시) blkdev_iomap_begin -> iomap_add_to_ioend -> (누적된 bio) submit_bio -> blk_mq_submit_bio -> mq_ops->queue_rq(NVMe면 nvme_queue_rq)
 */
static ssize_t blkdev_writeback_range(struct iomap_writepage_ctx *wpc,
		struct folio *folio, u64 offset, unsigned int len, u64 end_pos)
{
	loff_t isize = i_size_read(wpc->inode); /* [한국어] 현재 장치 크기 조회 - 범위 검사 기준 */

	if (WARN_ON_ONCE(offset >= isize)) /* [한국어] writeback 대상 offset 이 장치 크기를 벗어났는지 확인 - 정상 경로라면 발생하지 않아야 하므로 WARN 으로 크게 알린다 */
		return -EIO; /* [한국어] 비정상 상태이므로 즉시 에러 반환 */

	if (offset < wpc->iomap.offset || /* [한국어] 캐시된 iomap 매핑이 이번 offset을 포함하지 않는지 확인(시작 이전) */
	    offset >= wpc->iomap.offset + wpc->iomap.length) { /* [한국어] 또는 매핑 범위의 끝을 넘어섰는지 확인 - iomap 캐시가 유효하지 않으면 재매핑 필요 */
		int error; /* [한국어] 재매핑 결과 에러 코드 */

		error = blkdev_iomap_begin(wpc->inode, offset, isize - offset, /* [한국어] blkdev_iomap_begin으로 재매핑 - 캐시된 wpc->iomap을 이 offset부터 다시 채움 */
				IOMAP_WRITE, &wpc->iomap, NULL); /* [한국어] 쓰기 매핑임을 IOMAP_WRITE 로 알리고, srcmap 은 COW 개념이 없는 블록 장치라 NULL 이다 */
		if (error) /* [한국어] 재매핑 실패 검사 */
			return error; /* [한국어] 실패 시 즉시 에러 반환 */
	}

	return iomap_add_to_ioend(wpc, folio, offset, end_pos, len); /* [한국어] 이 folio 범위를 현재 누적 중인 ioend(bio 체인)에 추가 - 인접한 folio 들이 한 bio 로 합쳐지도록 한다 */
}

static const struct iomap_writeback_ops blkdev_writeback_ops = { /* [한국어] iomap writeback 코어가 folio 범위를 쓰기 bio 로 조립/제출할 때 사용하는 콜백 테이블 */
	.writeback_range	= blkdev_writeback_range, /* [한국어] folio 범위를 ioend에 추가하는 콜백 */
	.writeback_submit	= iomap_ioend_writeback_submit, /* [한국어] 누적된 ioend(bio 체인)를 실제로 submit_bio하는 표준 iomap 헬퍼 연결 */
};

/*
 * [한국어]
 * blkdev_writepages (iomap 버전) - iomap 기반 페이지 캐시 writeback
 *
 * @mapping: 대상 bdev의 address_space.
 * @wbc: writeback 제어 정보.
 * @return: iomap_writepages()의 반환값(0 성공, 음수 errno 실패).
 *
 * struct address_space_operations.writepages 구현(iomap 버전).
 * iomap_writepage_ctx를 이 mapping의 inode와 blkdev_writeback_ops로
 * 초기화한 뒤 iomap_writepages()에 위임한다. iomap 코어가 내부적으로
 * writeback_range/writeback_submit 콜백을 반복 호출해 더티 folio들을
 * bio 로 변환해 제출한다. 실행 컨텍스트: writeback
 * kworker 또는 fsync 호출 태스크 컨텍스트.
 * 호출자: writeback 코어의 .writepages 콜백 호출 경로,
 *         blkdev_fsync -> file_write_and_wait_range를 통한 간접 호출.
 * 호출 대상: iomap_writepages.
 * 에러 경로: iomap_writepages의 에러를 그대로 전달.
 * 호출 체인:
 *   writeback 코어/blkdev_fsync -> [blkdev_writepages] -> iomap_writepages -> blkdev_writeback_range(반복) -> submit_bio -> blk_mq_submit_bio -> mq_ops->queue_rq(NVMe면 nvme_queue_rq)
 */
static int blkdev_writepages(struct address_space *mapping,
		struct writeback_control *wbc)
{
	struct iomap_writepage_ctx wpc = { /* [한국어] iomap writeback 코어에 전달할 컨텍스트 - inode/wbc/콜백 테이블 묶음 */
		.inode		= mapping->host, /* [한국어] writeback 대상 bdev inode */
		.wbc		= wbc, /* [한국어] 호출자가 전달한 writeback 제어 정보 그대로 전달 */
		.ops		= &blkdev_writeback_ops /* [한국어] 이 파일에서 정의한 range/submit 콜백 테이블 연결 */
	};

	return iomap_writepages(&wpc); /* [한국어] 실제 순회/제출은 iomap 코어에 위임 - 더티 folio 를 쓰기 bio 로 변환해 제출한다 */
}

/*
 * [한국어]
 * def_blk_aops (iomap 버전) - iomap 기반 페이지 캐시 경유 block 장치 연산 테이블
 *
 * dirty_folio/read_folio/readahead/writepages 등이 iomap 코어를 경유해
 * bio를 생성하며, submit_bio -> blk_mq_submit_bio -> mq_ops->queue_rq(NVMe면 nvme_queue_rq)로
 * 전달된다. CONFIG_BUFFER_HEAD가 꺼진(iomap을 사용하는) 최신 빌드에서
 * 컴파일되는 버전으로, 위 #ifdef 블록의 buffer_head 버전과 이름이 같은
 * 전역 심볼이지만 두 버전 중 하나만 최종 바이너리에 링크된다.
 */
const struct address_space_operations def_blk_aops = {
	.dirty_folio	= filemap_dirty_folio, /* [한국어] 일반 페이지 캐시(비 buffer_head) dirty 표시 헬퍼 */
	.release_folio		= iomap_release_folio, /* [한국어] folio 회수 시 iomap 내부 상태 정리 */
	.invalidate_folio	= iomap_invalidate_folio, /* [한국어] folio 무효화 시 iomap 내부 상태 정리 */
	.read_folio		= blkdev_read_folio, /* [한국어] 단일 folio 를 읽어 채우는 콜백(iomap 구현) */
	.readahead		= blkdev_readahead, /* [한국어] 순차 prefetch 콜백(iomap 구현) */
	.writepages		= blkdev_writepages, /* [한국어] 더티 folio 들의 writeback 콜백(iomap 구현) */
	.is_partially_uptodate  = iomap_is_partially_uptodate, /* [한국어] folio 일부만 최신 상태인지 판정(부분 읽기 최적화) */
	.error_remove_folio	= generic_error_remove_folio, /* [한국어] 복구 불가 에러 시 folio를 캐시에서 제거하는 표준 헬퍼 */
	.migrate_folio		= filemap_migrate_folio, /* [한국어] 메모리 마이그레이션 시 일반 페이지 캐시 folio 이동 헬퍼 */
};
#endif /* CONFIG_BUFFER_HEAD */

/*
 * for a block special file file_inode(file)->i_size is zero
 * so we compute the size by hand (just as in block_read/write above)
 */
/*
 * [한국어]
 * blkdev_llseek - 블록 장치 노드의 파일 위치(오프셋) 변경
 *
 * @file: llseek을 요청한 struct file(block 장치 노드).
 * @offset: 요청된 오프셋(whence 해석에 따라 상대/절대).
 * @whence: SEEK_SET/SEEK_CUR/SEEK_END 등.
 * @return: 성공 시 새 파일 위치, 실패 시 음수 errno.
 *
 * 위 원본 영어 주석대로, block 장치 노드는 일반 파일과 달리
 * file_inode(file)->i_size가 0이므로(block 장치 inode는 일반적인 "파일
 * 크기" 개념이 없음) llseek 시 장치의 실제 바이트 크기를 직접 계산해서
 * 사용해야 한다. bdev_file_inode()로 얻은 bdev inode의 i_size(장치 전체
 * 바이트 수, 다른 경로에서 설정/갱신됨)를 fixed_size_llseek()에 전달해
 * SEEK_END 등의 계산 기준으로 삼는다. i_size가 도중에 바뀌는 것(예:
 * 파티션 재스캔)을 막기 위해 inode_lock으로 감싼다. 이렇게 계산된 새
 * 위치(ki_pos)가 이후 read/write 의 시작 오프셋이 되어 최종적으로 bio 의
 * LBA 계산의 기초가 되므로, 장치 범위를 벗어난 오프셋이 애초에 만들어지지
 * 않도록 이 단계에서 걸러주는 효과도 있다. 실행 컨텍스트: lseek(2)/
 * llseek(2) 시스템 콜을 처리하는 유저 프로세스 컨텍스트.
 * 호출자: VFS의 vfs_llseek() (def_blk_fops.llseek로 등록됨).
 * 호출 대상: bdev_file_inode, fixed_size_llseek, inode_lock/inode_unlock.
 * 에러 경로: fixed_size_llseek 자체가 계산한 음수 errno(예: 잘못된
 * whence)를 그대로 반환.
 * 호출 체인:
 *   lseek(2)/llseek(2) -> vfs_llseek -> [blkdev_llseek] -> fixed_size_llseek
 */
static loff_t blkdev_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *bd_inode = bdev_file_inode(file); /* [한국어] 실제 크기 정보를 가진 bdev inode 획득 */
	loff_t retval; /* [한국어] 계산된 새 파일 위치 또는 에러 코드 */

	inode_lock(bd_inode); /* [한국어] i_size 읽는 동안 동시 변경(예: 크기 갱신)을 막기 위해 inode 락 획득 */
	retval = fixed_size_llseek(file, offset, whence, i_size_read(bd_inode)); /* [한국어] bdev 크기를 기준으로 유효 범위 내에서 새 위치 계산 */
	inode_unlock(bd_inode); /* [한국어] inode 락 해제 */
	return retval; /* [한국어] 새 파일 위치 또는 에러 반환 */
}

/*
 * [한국어]
 * blkdev_fsync - 블록 장치 노드의 fsync(2)/fdatasync(2) 구현
 *
 * @filp: fsync을 요청한 struct file.
 * @start: 동기화할 범위의 시작 오프셋.
 * @end: 동기화할 범위의 끝 오프셋.
 * @datasync: fdatasync(메타데이터 생략 가능)인지 여부 - block device
 *            자체는 파일시스템 메타데이터가 없어 이 값으로 분기하지 않는다.
 * @return: 0 성공, 그 외 음수 errno.
 *
 * struct file_operations.fsync 구현. 먼저 file_write_and_wait_range()로
 * 해당 범위의 더티 페이지(버퍼드 쓰기로 아직 writeback되지 않은 데이터)를
 * bio Write로 흘려보내고 완료를 기다린다. 그 다음 blkdev_issue_flush()를
 * 호출하는데, 이것은 REQ_OP_FLUSH bio 하나를 발행해 완료까지 기다린다.
 * 이 단계가 필요한 이유는, writeback 이 끝났다는 것은 "장치가 요청을
 * 받았다"까지만 뜻하지 "매체에 남았다"는 뜻이 아니기 때문이다. 휘발성
 * 쓰기 캐시를 가진 장치는 전원이 끊기면 캐시 내용을 잃는다.
 * NVMe 접점(코드 확인됨): REQ_OP_FLUSH 는 nvme_setup_flush()가 옵코드
 * 0x00(Flush)로 번역한다(drivers/nvme/host/core.c:1070). 이 명령을 큐가
 * 지원하는지는 BLK_FEAT_WRITE_CACHE 로 표현되며, NVMe 는 컨트롤러가
 * VWC(Volatile Write Cache)를 보고할 때만 이 비트를 켠다(core.c:2813).
 * 캐시가 없다고 보고한 컨트롤러에서는 flush 자체가 no-op 이 된다. 장치가 Flush를
 * 지원하지 않아 -EOPNOTSUPP를 반환하면(예: 캐시가 없거나 항상 FUA를
 * 쓰는 장치) 이는 실패가 아니라 "플러시할 것이 애초에 없다"는 의미이므로
 * 0(성공)으로 간주한다. 실행 컨텍스트: fsync(2)를 호출한 유저 프로세스
 * 컨텍스트에서 동기적으로 실행되며, 완료까지 블로킹된다.
 * 호출자: vfs_fsync() -> vfs_fsync_range() (def_blk_fops.fsync로 등록됨).
 * 호출 대상: file_write_and_wait_range, blkdev_issue_flush.
 * 에러 경로: file_write_and_wait_range 실패 시 즉시 반환(Flush 시도조차
 * 하지 않음 - 아직 매체에 쓰이지 않은 데이터가 있으므로 무의미).
 * 호출 체인:
 *   fsync(2) -> vfs_fsync -> [blkdev_fsync] -> blkdev_issue_flush -> submit_bio(REQ_OP_FLUSH) -> blk_mq_submit_bio -> mq_ops->queue_rq
 *   (NVMe면 nvme_queue_rq -> nvme_setup_flush 가 opcode 0x00 Flush 를 만든다)
 */
static int blkdev_fsync(struct file *filp, loff_t start, loff_t end,
		int datasync)
{
	struct block_device *bdev = I_BDEV(filp->f_mapping->host); /* [한국어] fsync 대상 block_device 획득 */
	int error; /* [한국어] 각 단계의 에러 코드 */

	error = file_write_and_wait_range(filp, start, end); /* [한국어] 지정 범위의 더티 페이지를 먼저 writeback(bio Write)하고 완료까지 대기 - 더티 페이지를 먼저 기록 */
	if (error) /* [한국어] writeback 실패 검사 */
		return error; /* [한국어] 실패 시 Flush 단계로 진행하지 않고 즉시 반환 */

	/*
	 * There is no need to serialise calls to blkdev_issue_flush with
	 * i_mutex and doing so causes performance issues with concurrent
	 * O_SYNC writers to a block device.
	 */
	error = blkdev_issue_flush(bdev); /* [한국어] REQ_OP_FLUSH bio 를 발행하고 완료까지 대기 - 장치의 휘발성 쓰기 캐시를 매체로 밀어낸다 */
	if (error == -EOPNOTSUPP) /* [한국어] 장치가 Flush 자체를 지원하지 않는 경우인지 확인 */
		error = 0; /* [한국어] 미지원(-EOPNOTSUPP)은 실패가 아니다 - 휘발성 캐시가 없는 장치라면 writeback 이 끝난 시점에 이미 내구성이 확보되어 있다 */

	return error; /* [한국어] 최종 결과 반환 */
}

/**
 * file_to_blk_mode - get block open flags from file flags
 * @file: file whose open flags should be converted
 *
 * Look at file open flags and generate corresponding block open flags from
 * them. The function works both for file just being open (e.g. during ->open
 * callback) and for file that is already open. This is actually non-trivial
 * (see comment in the function).
 */
/*
 * [한국어]
 * file_to_blk_mode - VFS 파일 열기 플래그를 block layer의 open 모드(blk_mode_t)로 변환
 *
 * @file: 변환 대상 struct file. open 중(-> ->open 콜백 내부)일 수도 있고
 *        이미 열려 있는 파일일 수도 있다(위 원본 영어 주석 참고).
 * @return: BLK_OPEN_READ/WRITE/EXCL/NDELAY/WRITE_IOCTL 비트를 조합한
 *          blk_mode_t 값.
 *
 * VFS 레벨의 f_mode/f_flags(O_RDONLY/O_WRONLY/O_RDWR, O_EXCL, O_NDELAY 등)를
 * block layer가 이해하는 blk_mode_t 비트로 번역한다. 까다로운 지점은
 * O_EXCL 처리인데, do_dentry_open()이 open 처리 도중 f_flags에서 O_EXCL을
 * 지워버리기 때문에 "이미 열려 있는 파일"에서는 f_flags만으로 배타적
 * open 여부를 알 수 없다. 그래서 blkdev_open()이 배타적 open 시
 * file->private_data에 filp 자신을 저장해두는 관례를 이용해, 이 함수는
 * private_data가 설정되어 있으면(=holder로 자신을 등록했으면) 배타적
 * open으로 판단한다. 이렇게 얻은 mode는 bdev_permission()/bdev_open()에
 * 전달되어 최종적으로 이 블록 장치에 대한 읽기/쓰기/배타적 접근 권한을
 * 결정한다. 실행 컨텍스트: open(2) 처리 중 또는 이미 열린 파일에 대해
 * 필요할 때(예: blkdev_fallocate에서 재계산) 호출되며 락이 필요 없는
 * 순수 변환 함수다.
 * 호출자: blkdev_open, blkdev_fallocate (그리고 block/ioctl.c 등 이 파일
 *         밖의 호출자도 있어 static이 아닌 전역 함수로 선언됨).
 * 호출 대상: 없음(비트 연산만 수행).
 * 에러 경로: 없음(항상 유효한 mode 반환).
 * 호출 체인:
 *   blkdev_open/blkdev_fallocate -> [file_to_blk_mode] -> bdev_permission/bdev_open/truncate_bdev_range 등에 mode로 전달
 */
blk_mode_t file_to_blk_mode(struct file *file)
{
	blk_mode_t mode = 0; /* [한국어] 결과를 누적할 비트마스크 - 초기값은 아무 권한도 없음 */

	if (file->f_mode & FMODE_READ) /* [한국어] VFS가 이미 해석한 읽기 가능 여부 확인 */
		mode |= BLK_OPEN_READ; /* [한국어] 읽기 권한 비트 추가 - bdev_open()이 이 비트로 접근 검사를 한다 */
	if (file->f_mode & FMODE_WRITE) /* [한국어] VFS가 이미 해석한 쓰기 가능 여부 확인 */
		mode |= BLK_OPEN_WRITE; /* [한국어] 쓰기 권한 비트 추가 - 읽기 전용 장치나 마운트된 장치에 대해서는 아래 계층에서 거부될 수 있다 */
	/*
	 * do_dentry_open() clears O_EXCL from f_flags, use file->private_data
	 * to determine whether the open was exclusive for already open files.
	 */
	if (file->private_data) /* [한국어] blkdev_open이 배타적 open 시 저장해둔 holder(자기 자신) 존재 여부 확인 - 이미 열린 파일에서 배타성을 판별하는 유일한 방법 */
		mode |= BLK_OPEN_EXCL; /* [한국어] 배타적 open으로 판정 */
	else if (file->f_flags & O_EXCL) /* [한국어] private_data가 없다면(open 진행 중 등) f_flags의 원본 O_EXCL 플래그로 판별 */
		mode |= BLK_OPEN_EXCL; /* [한국어] 배타적 open으로 판정 */
	if (file->f_flags & O_NDELAY) /* [한국어] 논블로킹 open 플래그 확인 */
		mode |= BLK_OPEN_NDELAY; /* [한국어] 대응하는 block layer 플래그 추가 */

	/*
	 * If all bits in O_ACCMODE set (aka O_RDWR | O_WRONLY), the floppy
	 * driver has historically allowed ioctls as if the file was opened for
	 * writing, but does not allow and actual reads or writes.
	 */
	if ((file->f_flags & O_ACCMODE) == (O_RDWR | O_WRONLY)) /* [한국어] 역사적으로 플로피 드라이버가 사용하던 특수한 접근 모드 조합(O_ACCMODE의 모든 비트가 설정된 비표준 값)인지 확인 */
		mode |= BLK_OPEN_WRITE_IOCTL; /* [한국어] 실제 read/write는 허용하지 않지만 ioctl만은 쓰기 가능한 것처럼 허용하는 레거시 호환 플래그 추가 */

	return mode; /* [한국어] 조합된 blk_mode_t 반환 */
}

/*
 * [한국어]
 * blkdev_open - 블록 장치 노드 open(2) 구현
 *
 * @inode: 열리는 대상의 inode(i_rdev에 major/minor 번호를 담음).
 * @filp: 새로 만들어지는 struct file. private_data를 holder 식별에 사용한다.
 * @return: 0 성공, 음수 errno 실패(-ENXIO 등).
 *
 * struct file_operations.open 구현. 먼저 file_to_blk_mode()로 VFS 열기
 * 플래그를 blk_mode_t로 변환하고, 배타적(exclusive) open이면 filp 자신을
 * holder로 등록해(private_data) 이후 같은 파일에 대한 재검사와 다른
 * 프로세스와의 배타성 판정에 사용할 수 있게 한다. bdev_permission()으로
 * 이 모드의 접근이 허용되는지(예: 이미 다른 프로세스가 배타적으로 열고
 * 있는지) 확인한 뒤, blkdev_get_no_open()으로 실제 struct block_device를
 * 얻는다. 장치가 원자적 쓰기(atomic write)나 PI/DIF 무결성(integrity)
 * metadata를 지원하면 그 사실을 filp->f_mode 플래그로 남겨 이후
 * read/write 경로(IOCB_ATOMIC/IOCB_HAS_METADATA 처리)에서 참조할 수
 * 있게 한다. 마지막으로 bdev_open()이 성공하면 이 struct block_device는
 * 실제로 "열린" 상태가 되어 block_device 에 대한 참조와 holder 등록이
 * 확정되며, 이후 read/write 가 이 bdev 를 통해 bio 를 발행할 수 있게 된다.
 * 실행 컨텍스트: open(2) 시스템 콜을 처리하는 유저 프로세스 컨텍스트.
 * 호출자: VFS의 do_dentry_open() (def_blk_fops.open으로 등록됨).
 * 호출 대상: file_to_blk_mode, bdev_permission, blkdev_get_no_open,
 *           bdev_can_atomic_write, blk_get_integrity, bdev_open,
 *           blkdev_put_no_open.
 * 에러 경로: bdev_permission 실패 시 즉시 반환, blkdev_get_no_open이
 * NULL을 반환하면 -ENXIO(그런 장치 없음), bdev_open 실패 시 앞서 얻은
 * bdev 참조를 blkdev_put_no_open으로 반납한 뒤 에러를 전달.
 * 호출 체인:
 *   open(2) -> do_dentry_open -> [blkdev_open] -> bdev_permission -> blkdev_get_no_open -> bdev_open (block/bdev.c)
 */
static int blkdev_open(struct inode *inode, struct file *filp)
{
	struct block_device *bdev; /* [한국어] 이 open으로 획득할 block_device 포인터 */
	blk_mode_t mode; /* [한국어] file_to_blk_mode가 계산한 block layer open 모드 */
	int ret; /* [한국어] 각 단계의 에러 코드 */

	mode = file_to_blk_mode(filp); /* [한국어] VFS 열기 플래그를 block open 모드로 변환 */
	/* Use the file as the holder. */
	if (mode & BLK_OPEN_EXCL) /* [한국어] 배타적 open이 요청되었는지 확인 */
		filp->private_data = filp; /* [한국어] exclusive open 시 filp 자신을 holder로 사용 - 이후 file_to_blk_mode가 이미 열린 파일의 배타성을 재판별하는 근거가 됨 */
	ret = bdev_permission(inode->i_rdev, mode, filp->private_data); /* [한국어] 이 장치 노드(rdev)에 대해 이 모드/holder 로 접근이 허용되는지 검사 - 마운트 중인 장치에 대한 raw 쓰기 차단이 여기서 걸린다 */
	if (ret) /* [한국어] 권한 검사 실패 확인 */
		return ret; /* [한국어] 거부되면 bdev를 얻지 않고 즉시 실패 반환 */

	bdev = blkdev_get_no_open(inode->i_rdev, true); /* [한국어] rdev(major:minor)로 bdevfs inode 해시를 뒤져 block_device 를 찾고 참조만 올린다 - 아직 "열기"는 아니다 */
	if (!bdev) /* [한국어] 대응하는 장치가 존재하지 않는지 확인 */
		return -ENXIO; /* [한국어] 없는 장치 노드이므로 실패 반환 */

	if (bdev_can_atomic_write(bdev)) /* [한국어] 이 장치가 원자적 쓰기를 지원하는지 확인 */
		filp->f_mode |= FMODE_CAN_ATOMIC_WRITE; /* [한국어] 큐가 원자적 쓰기 한도를 보고했으면 파일 모드에 표시 - 이후 RWF_ATOMIC 요청을 허용할지 판단하는 근거가 된다 */
	if (blk_get_integrity(bdev->bd_disk)) /* [한국어] 이 디스크가 PI/DIF integrity profile을 갖는지 확인 */
		filp->f_mode |= FMODE_HAS_METADATA; /* [한국어] 장치가 integrity(메타데이터) 프로파일을 가지고 있으면 표시 - 이후 IOCB_HAS_METADATA 경로가 열린다 */

	ret = bdev_open(bdev, mode, filp->private_data, NULL, filp); /* [한국어] 실제로 이 파일에 대해 bdev 를 연다 - bd_openers 증가, holder 등록, 드라이버의 ->open 콜백 호출까지 block/bdev.c 가 수행한다 */
	if (ret) /* [한국어] bdev_open 실패 확인 */
		blkdev_put_no_open(bdev); /* [한국어] 실패했으므로 앞서 얻은 bdev 참조를 반납 - open 실패 시 bdev 참조 해제 */
	return ret; /* [한국어] 최종 결과 반환 */
}

/*
 * [한국어]
 * blkdev_release - 블록 장치 노드 close(2)/release 구현
 *
 * @inode: 사용하지 않음(시그니처 준수용).
 * @filp: 닫히는 struct file - private_data(holder)와 f_mapping 등이
 *        bdev_release 내부에서 참조된다.
 * @return: 항상 0.
 *
 * struct file_operations.release 구현. 실제 정리 작업은 모두
 * bdev_release()(block/bdev.c)에 위임하며, 이 함수는 VFS 콜백 시그니처를
 * 맞추는 얇은 래퍼다. bdev_release는 blkdev_open이 잡았던 참조/배타성
 * 등록을 해제하고, 참조 카운트가 0이 되면 관련 자원(및 필요 시 드라이버
 * namespace 자체의 해제 경로)까지 이어진다. 실행 컨텍스트: close(2) 또는
 * 파일 디스크립터 테이블 정리 시 커널이 호출하며, 마지막 참조 해제라면
 * 블로킹 가능한 컨텍스트에서 호출되어야 한다.
 * 호출자: VFS의 __fput() (def_blk_fops.release로 등록됨).
 * 호출 대상: bdev_release.
 * 에러 경로: 없음(release 콜백은 항상 0을 반환하는 관례 - 실패해도
 * 파일 디스크립터는 회수됨).
 * 호출 체인:
 *   close(2) -> __fput -> [blkdev_release] -> bdev_release (block/bdev.c) -> (bd_openers 0 도달 시) 드라이버 ->release 콜백 및 캐시 정리
 */
static int blkdev_release(struct inode *inode, struct file *filp)
{
	bdev_release(filp); /* [한국어] bd_openers/bd_holders 감소, holder 등록 해제, 마지막 닫힘이면 캐시 정리까지 block/bdev.c 에 위임 */
	return 0; /* [한국어] release는 항상 성공으로 간주 */
}

/*
 * [한국어]
 * blkdev_direct_write - O_DIRECT 쓰기 공통 처리(페이지 캐시 무효화 + direct I/O 제출)
 *
 * @iocb: IOCB_DIRECT가 설정된 쓰기 요청.
 * @from: 쓸 데이터를 담은 iov_iter.
 * @return: 성공 시 전송 바이트 수 또는 -EIOCBQUEUED(비동기 큐잉),
 *          실패 시 음수 errno, 페이지 캐시가 바쁘면(-EBUSY) 0(폴백 유도).
 *
 * O_DIRECT 쓰기는 페이지 캐시를 우회해 장치에 직접 쓰지만, 만약 같은
 * 범위에 대해 페이지 캐시에 캐시된(그리고 어쩌면 더티한) 페이지가 남아
 * 있다면 캐시와 장치 상의 실제 데이터가 어긋나는 일관성 문제가 생긴다.
 * 그래서 실제 direct I/O를 제출하기 전에 kiocb_invalidate_pages()로 해당
 * 범위의 페이지 캐시를 먼저 무효화한다. 무효화가 -EBUSY로 실패하면(다른
 * 곳에서 페이지를 사용 중이어서 당장 지울 수 없는 경우) 이 요청은 그냥
 * 0바이트 처리로 물러나 상위(blkdev_write_iter)가 buffered write로
 * 폴백하도록 유도한다. 무효화 후 blkdev_direct_IO()로 실제 쓰기 bio 를
 * 제출하고, 성공적으로 쓰인 바이트만큼 kiocb_invalidate_post_direct_write()
 * 로 사후 무효화를 한 번 더 수행해(direct I/O 도중 다른 경로로 페이지
 * 캐시가 다시 채워졌을 가능성에 대비) 커서와 iter 상태를 정리한다.
 * 실행 컨텍스트: O_DIRECT 쓰기 시스템 콜을 처리하는 유저 프로세스 컨텍스트.
 * 호출자: blkdev_write_iter (iocb->ki_flags & IOCB_DIRECT일 때).
 * 호출 대상: kiocb_invalidate_pages, blkdev_direct_IO,
 *           kiocb_invalidate_post_direct_write, iov_iter_revert.
 * 에러 경로: kiocb_invalidate_pages가 -EBUSY 외의 에러를 반환하면 그대로
 * 전달, blkdev_direct_IO 실패도 그대로 전달(단, 부분 쓰기는 iter를
 * 되돌려 상위가 재시도/폴백할 수 있게 함).
 * 호출 체인:
 *   blkdev_write_iter -> [blkdev_direct_write] -> blkdev_direct_IO -> submit_bio -> blk_mq_submit_bio -> mq_ops->queue_rq (NVMe면 nvme_queue_rq -> nvme_setup_rw)
 */
static ssize_t
blkdev_direct_write(struct kiocb *iocb, struct iov_iter *from)
{
	size_t count = iov_iter_count(from); /* [한국어] 원래 요청된 전체 바이트 수 - 이후 부분 쓰기 계산 및 iter 되돌리기 기준 */
	ssize_t written; /* [한국어] 각 단계의 반환값(전송 바이트 또는 에러)을 담는 변수 */

	written = kiocb_invalidate_pages(iocb, count); /* [한국어] direct I/O 전에 해당 범위의 페이지 캐시를 무효화 - 캐시와 장치 데이터 불일치 방지 */
	if (written) { /* [한국어] 무효화 자체가 0이 아닌 값을 반환했는지 확인(정상 무효화는 0) */
		if (written == -EBUSY) /* [한국어] 페이지가 사용 중이라 당장 무효화할 수 없는 경우인지 확인 */
			return 0; /* [한국어] 0바이트로 처리하고 상위가 buffered write로 폴백하도록 유도 */
		return written; /* [한국어] 그 외 에러는 그대로 호출자에 전달 */
	}

	written = blkdev_direct_IO(iocb, from); /* [한국어] 실제 O_DIRECT 쓰기 제출 - 페이지 캐시를 거치지 않고 유저 버퍼에서 곧바로 bio 를 만든다 */
	if (written > 0) { /* [한국어] 일부라도 성공적으로 전송되었는지 확인 */
		kiocb_invalidate_post_direct_write(iocb, count); /* [한국어] direct I/O 도중/이후 페이지 캐시가 다시 채워졌을 가능성에 대비한 사후 무효화 */
		iocb->ki_pos += written; /* [한국어] 파일(장치) 커서를 전송한 만큼 전진 */
		count -= written; /* [한국어] 아직 처리되지 않은 나머지 바이트 수 갱신 */
	}
	if (written != -EIOCBQUEUED) /* [한국어] 비동기로 아직 큐잉 중인 상태가 아니라면(=이미 완료되었거나 에러라면) */
		iov_iter_revert(from, count - iov_iter_count(from)); /* [한국어] 실제 소비된 양과 count 계산이 어긋난 차이만큼 iter 커서를 되돌려 상위(폴백 등)가 올바른 위치에서 이어갈 수 있게 함 */
	return written; /* [한국어] 전송 바이트 수, -EIOCBQUEUED, 또는 음수 errno 반환 */
}

/*
 * [한국어]
 * blkdev_buffered_write - 페이지 캐시를 경유하는 buffered(non-direct) write
 *
 * @iocb: (일반적으로 IOCB_DIRECT가 없는) 쓰기 요청.
 * @from: 쓸 데이터를 담은 iov_iter.
 * @return: iomap_file_buffered_write()의 반환값(전송 바이트 수 또는 음수 errno).
 *
 * iomap_file_buffered_write()에 blkdev_iomap_ops(=blkdev_iomap_begin)를
 * 넘겨 위임하는 얇은 래퍼다. iomap 코어가 내부적으로 folio를 페이지
 * 캐시에서 찾거나 만들고, 사용자 데이터를 folio에 복사한 뒤 더티로
 * 표시한다. 이 시점에는 장치로의 쓰기가 전혀 발생하지 않고, 이후 writeback
 * (blkdev_writepages, 또는 fsync/msync를 통한 명시적 flush) 단계에서
 * 비로소 쓰기 bio 로 제출된다. 실행 컨텍스트: buffered
 * write 시스템 콜을 처리하는 유저 프로세스 컨텍스트.
 * 호출자: blkdev_write_iter (direct 경로가 아니거나, direct 쓰기가 일부만
 *         성공해 나머지를 폴백해야 할 때 direct_write_fallback을 통해).
 * 호출 대상: iomap_file_buffered_write.
 * 에러 경로: iomap_file_buffered_write의 에러를 그대로 전달.
 * 호출 체인:
 *   blkdev_write_iter -> [blkdev_buffered_write] -> iomap_file_buffered_write -> (folio dirty) -> 이후 writeback -> blkdev_writepages -> submit_bio -> blk_mq_submit_bio -> mq_ops->queue_rq(NVMe면 nvme_queue_rq)
 */
static ssize_t blkdev_buffered_write(struct kiocb *iocb, struct iov_iter *from)
{
	return iomap_file_buffered_write(iocb, from, &blkdev_iomap_ops, NULL, /* [한국어] iomap 코어에 위임 - 장치 매핑은 blkdev_iomap_ops가 제공 */
			NULL); /* [한국어] 뒤의 두 NULL 은 writeback 훅과 private 컨텍스트 - 블록 장치에는 파일시스템처럼 추가로 붙일 상태가 없다 */
}

/*
 * Write data to the block device.  Only intended for the block device itself
 * and the raw driver which basically is a fake block device.
 *
 * Does not take i_mutex for the write and thus is not for general purpose
 * use.
 */
/*
 * [한국어]
 * blkdev_write_iter - 블록 장치 노드의 write_iter 진입점(struct file_operations.write_iter)
 *
 * @iocb: 쓰기 요청. ki_flags(IOCB_DIRECT/IOCB_ATOMIC/IOCB_NOWAIT 등)에 따라
 *        경로가 분기된다.
 * @from: 쓸 데이터를 담은 iov_iter.
 * @return: 성공 시 전송(또는 큐잉) 바이트 수/-EIOCBQUEUED, 실패 시 음수 errno.
 *
 * 위 원본 영어 주석대로 이 함수는 block 장치 자신과 (사실상 가짜
 * block 장치인) raw 드라이버 전용이며, 일반적인 파일시스템 write처럼
 * i_rwsem(구 i_mutex)을 잡지 않는다(대신 buffered 경로에서만 부분적으로
 * inode_lock_shared를 사용). 처리 순서: (1) 읽기 전용/스왑파일 보호 등
 * 기본 검증, (2) 요청이 장치 끝을 넘는지 확인하고 필요하면 truncate,
 * (3) inode 시간 갱신, (4) IOCB_DIRECT 여부에 따라 blkdev_direct_write
 * (O_DIRECT, 유저 버퍼에서 곧바로 bio 제출) 또는 blkdev_buffered_write(페이지
 * 캐시 경유) 호출, (5) direct write가 일부만 처리되고 남은 데이터가
 * 있으면 buffered write로 폴백, (6) 마지막으로 generic_write_sync()로
 * O_SYNC/O_DSYNC 등의 요구에 맞춰 필요하면 추가 flush를 수행한다.
 * 실행 컨텍스트: write(2)/pwrite(2)/pwritev2(2) 시스템 콜을 처리하는
 * 유저 프로세스 컨텍스트.
 * 호출자: vfs_write() -> kiocb->ki_filp->f_op->write_iter (def_blk_fops.
 *         write_iter로 등록됨).
 * 호출 대상: bdev_read_only, IS_SWAPFILE, generic_atomic_write_valid,
 *           file_update_time, blkdev_direct_write, blkdev_buffered_write,
 *           direct_write_fallback, generic_write_sync.
 * 에러 경로: 읽기 전용(-EPERM), 스왑파일 보호(-ETXTBSY), 빈 요청(0),
 * 장치 끝 도달(-ENOSPC), DIRECT 없는 NOWAIT(-EOPNOTSUPP), atomic 검증
 * 실패, atomic인데 truncate가 필요한 경우(-EINVAL) 등 다단계 검증 실패를
 * 각각 즉시 반환한다.
 * 호출 체인:
 *   write(2)/pwrite(2) -> vfs_write -> [blkdev_write_iter] -> blkdev_direct_write/blkdev_buffered_write -> blkdev_direct_IO/iomap_file_buffered_write
 *   -> submit_bio -> blk_mq_submit_bio -> mq_ops->queue_rq (NVMe면 nvme_queue_rq)
 */
static ssize_t blkdev_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp; /* [한국어] 이 쓰기 요청이 속한 struct file */
	struct inode *bd_inode = bdev_file_inode(file); /* [한국어] bdev inode 획득 - 크기/락/스왑파일 검사에 사용 */
	struct block_device *bdev = I_BDEV(bd_inode); /* [한국어] 대상 block_device 포인터 */
	bool atomic = iocb->ki_flags & IOCB_ATOMIC; /* [한국어] 원자적 쓰기 요청 여부 - 이후 여러 검증 분기에서 재사용 */
	loff_t size = bdev_nr_bytes(bdev); /* [한국어] 장치의 현재 전체 바이트 크기 - 범위 검사 기준 */
	size_t shorted = 0; /* [한국어] 장치 크기 초과로 잘라낸 바이트 수 - 처리 후 iter를 원래 길이로 되돌리는 데 사용 */
	ssize_t ret; /* [한국어] 각 단계의 반환값(전송 바이트 또는 에러) */

	if (bdev_read_only(bdev)) /* [한국어] 장치가 읽기 전용으로 열렸거나 표시되었는지 확인 */
		return -EPERM; /* [한국어] 읽기 전용으로 표시된 장치(set_disk_ro 등)에는 쓰기를 허용하지 않는다 */

	if (IS_SWAPFILE(bd_inode) && !is_hibernate_resume_dev(bd_inode->i_rdev)) /* [한국어] 이 장치가 활성 스왑으로 사용 중이면서 하이버네이션 재개 대상은 아닌지 확인 - swapfile 보호 */
		return -ETXTBSY; /* [한국어] 스왑으로 사용 중인 장치에 대한 일반 쓰기 거부(재개 장치는 예외적으로 허용) */

	if (!iov_iter_count(from)) /* [한국어] 쓸 데이터가 0바이트인지 확인 */
		return 0; /* [한국어] 빈 요청은 즉시 0 반환 */

	if (iocb->ki_pos >= size) /* [한국어] 시작 오프셋이 이미 장치 끝 이상인지 확인 */
		return -ENOSPC; /* [한국어] 더 쓸 공간이 없음 - block device는 파일처럼 확장되지 않으므로 ENOSPC */

	if ((iocb->ki_flags & (IOCB_NOWAIT | IOCB_DIRECT)) == IOCB_NOWAIT) /* [한국어] NOWAIT 만 있고 DIRECT 가 없는 조합인지 확인 - buffered 쓰기는 페이지 할당/락 대기로 언제든 슬립할 수 있어 논블로킹을 약속할 수 없다 */
		return -EOPNOTSUPP; /* [한국어] buffered 쓰기는 NOWAIT 보장을 제공할 수 없으므로 거부 */

	if (atomic) { /* [한국어] 원자적 쓰기가 요청되었으면 큐가 보고한 한도(정렬/최대 길이)와 맞는지 사전 검사하는 분기로 진입 */
		ret = generic_atomic_write_valid(iocb, from); /* [한국어] 길이/정렬 등이 원자적 쓰기 유닛 요구사항을 만족하는지 검사 */
		if (ret) /* [한국어] 검증 실패 확인 */
			return ret; /* [한국어] 실패 시 즉시 반환 */
	}

	size -= iocb->ki_pos; /* [한국어] 시작 오프셋부터 장치 끝까지 남은 공간 계산 */
	if (iov_iter_count(from) > size) { /* [한국어] 요청 길이가 장치 끝까지 남은 공간을 초과하는지 확인 - 장치는 확장될 수 없으므로 잘라내야 한다 */
		if (atomic) /* [한국어] 원자적 쓰기는 부분 처리(truncate)를 허용할 수 없으므로 확인 */
			return -EINVAL; /* [한국어] atomic + 초과는 통째로 거부 */
		shorted = iov_iter_count(from) - size; /* [한국어] 잘라낼 바이트 수 기록 - 나중에 iter를 원래 길이로 복원하는 데 사용 */
		iov_iter_truncate(from, size); /* [한국어] iter를 장치에 실제로 쓸 수 있는 길이까지만 남도록 자름 */
	}

	ret = file_update_time(file); /* [한국어] inode의 수정 시각(mtime/ctime 등) 갱신 */
	if (ret) /* [한국어] 시각 갱신 실패 확인(드묾) */
		return ret; /* [한국어] 실패 시 쓰기를 진행하지 않고 반환 */

	if (iocb->ki_flags & IOCB_DIRECT) { /* [한국어] O_DIRECT 요청인지 확인 - 페이지 캐시를 우회하는 경로로 분기한다 */
		ret = blkdev_direct_write(iocb, from); /* [한국어] 페이지 캐시 우회 direct write 수행 */
		if (ret >= 0 && iov_iter_count(from)) /* [한국어] 일부만 처리되고 iter에 데이터가 남았는지 확인(direct 경로가 전체를 처리하지 못한 경우) */
			ret = direct_write_fallback(iocb, from, ret, /* [한국어] 일부만 쓰여지면 buffered write로 fallback */
					blkdev_buffered_write(iocb, from)); /* [한국어] 인자 위치에서 buffered 쓰기를 먼저 수행하고 그 결과를 넘긴다 - direct 결과(ret)와 합산해 최종 반환값을 만든다 */
	} else { /* [한국어] buffered write 경로 */
		/*
		 * Take i_rwsem and invalidate_lock to avoid racing with
		 * set_blocksize changing i_blkbits/folio order and punching
		 * out the pagecache.
		 */
		inode_lock_shared(bd_inode); /* [한국어] set_blocksize 가 동시에 i_blkbits/folio 최소 order 를 바꾸거나 페이지 캐시를 비우는 것과의 경쟁을 막기 위해 공유 락 획득 */
		ret = blkdev_buffered_write(iocb, from); /* [한국어] 페이지 캐시 경유 쓰기 수행 */
		inode_unlock_shared(bd_inode); /* [한국어] 공유 락 해제 */
	}

	if (ret > 0) /* [한국어] 하나 이상의 바이트가 성공적으로 쓰였는지 확인 */
		ret = generic_write_sync(iocb, ret); /* [한국어] O_SYNC/O_DSYNC 로 열린 파일이면 여기서 vfs_fsync_range 를 거쳐 blkdev_fsync 까지 이어진다(=writeback + REQ_OP_FLUSH) */
	iov_iter_reexpand(from, iov_iter_count(from) + shorted); /* [한국어] 앞서 truncate로 잘라냈던 만큼을 iter 길이 계산에 복원 - 호출자가 원래 요청 크기를 올바르게 인식하도록 함 */
	return ret; /* [한국어] 최종 결과(전송 바이트 수 또는 에러) 반환 */
}

/*
 * [한국어]
 * blkdev_read_iter - 블록 장치 노드의 read_iter 진입점(struct file_operations.read_iter)
 *
 * @iocb: 읽기 요청. IOCB_DIRECT 여부로 direct/buffered 경로가 갈린다.
 * @to: 읽은 데이터를 받을 iov_iter.
 * @return: 성공 시 읽은 바이트 수 또는 -EIOCBQUEUED, 실패 시 음수 errno.
 *
 * 먼저 요청 범위가 장치 크기를 초과하면 잘라내고(EOF 처리), 남은 길이가
 * 0이면 atime 갱신도 건너뛰고 바로 반환한다(reexpand 레이블로 점프).
 * IOCB_DIRECT가 설정된 경우: (1) kiocb_write_and_wait()로 이 범위에 대해
 * 아직 writeback되지 않은 더티 페이지가 있다면 먼저 매체에 반영해
 * "쓰기 후 곧바로 direct read"가 최신 데이터를 보게 하고, (2)
 * blkdev_direct_IO()로 읽기 bio 를 직접 발행한다. direct read 가 요청의
 * 일부만 채웠다면(예: 부분 정렬 문제로 일부만 direct 경로를 타는 경우는
 * 실제로는 드물지만, 짧은 읽기/에러 등으로) 나머지는 아래의 일반
 * buffered 경로(filemap_read)로 이어서 채운다. buffered 경로는 항상
 * (direct 이후 나머지든 애초부터 buffered든) inode_lock_shared로 보호된
 * filemap_read()가 담당하며, 페이지 캐시 미스 시 blkdev_read_folio/
 * blkdev_readahead 를 통해 읽기 bio 로 채워진다. 마지막에 앞서 잘라낸
 * 만큼을 iter 길이 계산에 복원한다(reexpand). 실행 컨텍스트: read(2)/
 * pread(2)/preadv2(2) 시스템 콜을 처리하는 유저 프로세스 컨텍스트.
 * 호출자: vfs_read() -> kiocb->ki_filp->f_op->read_iter (def_blk_fops.
 *         read_iter로 등록됨).
 * 호출 대상: bdev_nr_bytes, kiocb_write_and_wait, file_accessed,
 *           blkdev_direct_IO, filemap_read.
 * 에러 경로: pos가 이미 장치 끝 이상이면 0(EOF, 에러 아님),
 * kiocb_write_and_wait/blkdev_direct_IO 실패 시 reexpand로 점프해 iter
 * 길이만 복원하고 에러를 그대로 반환.
 * 호출 체인:
 *   read(2)/pread(2) -> vfs_read -> [blkdev_read_iter] -> blkdev_direct_IO(direct) 또는 filemap_read(buffered) -> readahead/blkdev_read_folio -> submit_bio -> blk_mq_submit_bio -> mq_ops->queue_rq(NVMe면 nvme_queue_rq)
 */
static ssize_t blkdev_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *bd_inode = bdev_file_inode(iocb->ki_filp); /* [한국어] bdev inode 획득 - buffered 경로의 락에 사용 */
	struct block_device *bdev = I_BDEV(iocb->ki_filp->f_mapping->host); /* [한국어] 대상 block_device 포인터 */
	loff_t size = bdev_nr_bytes(bdev); /* [한국어] 장치의 현재 전체 바이트 크기 */
	loff_t pos = iocb->ki_pos; /* [한국어] 읽기 시작 오프셋 */
	size_t shorted = 0; /* [한국어] 장치 크기 초과로 잘라낸 바이트 수 */
	ssize_t ret = 0; /* [한국어] 누적 반환값 - direct 단계 결과가 buffered 단계(filemap_read)의 시작값으로 전달됨 */
	size_t count; /* [한국어] 이번에 실제로 읽을 바이트 수(EOF 잘라내기 반영 후) */

	if (unlikely(pos + iov_iter_count(to) > size)) { /* [한국어] 요청 범위가 장치 끝을 넘는지 확인 - bdev 크기를 초과하는 read는 잘라냄 */
		if (pos >= size) /* [한국어] 시작 위치 자체가 이미 장치 끝 이상인지 확인 */
			return 0; /* [한국어] EOF - 읽을 것이 전혀 없으므로 0 반환 */
		size -= pos; /* [한국어] 시작 위치부터 장치 끝까지 남은 길이 계산 */
		shorted = iov_iter_count(to) - size; /* [한국어] 잘라낼 바이트 수 기록 - 나중에 iter 복원에 사용 */
		iov_iter_truncate(to, size); /* [한국어] iter를 실제로 읽을 수 있는 길이까지만 남도록 자름 */
	}

	count = iov_iter_count(to); /* [한국어] 잘라내기 반영 후 실제 읽을 총 바이트 수 */
	if (!count) /* [한국어] 읽을 데이터가 없는지 확인 */
		goto reexpand; /* skip atime */ /* [한국어] atime 갱신조차 필요 없으므로 바로 마무리 단계로 점프 */

	if (iocb->ki_flags & IOCB_DIRECT) { /* [한국어] O_DIRECT 요청인지 확인 */
		ret = kiocb_write_and_wait(iocb, count); /* [한국어] 이 범위에 대해 아직 writeback되지 않은 더티 페이지를 먼저 매체에 반영 - direct read가 최신 데이터를 읽도록 보장 */
		if (ret < 0) /* [한국어] writeback 대기 실패 확인 */
			goto reexpand; /* [한국어] 실패 시 direct I/O를 시도하지 않고 마무리 단계로 */
		file_accessed(iocb->ki_filp); /* [한국어] atime 갱신(direct 경로에서는 이 시점에 한 번만 수행) */

		ret = blkdev_direct_IO(iocb, to); /* [한국어] 실제 O_DIRECT 읽기 제출 - 유저 버퍼 페이지를 곧바로 DMA 대상으로 삼는다 */
		if (ret > 0) { /* [한국어] 일부라도 성공적으로 읽혔는지 확인 */
			iocb->ki_pos += ret; /* [한국어] 파일(장치) 커서를 읽은 만큼 전진 */
			count -= ret; /* [한국어] 아직 채워지지 않은 나머지 바이트 수 갱신 */
		}
		if (ret != -EIOCBQUEUED) /* [한국어] 비동기로 아직 큐잉 중인 상태가 아니라면 */
			iov_iter_revert(to, count - iov_iter_count(to)); /* [한국어] 실제 소비량과 count 계산의 차이만큼 iter 커서를 되돌려 buffered 경로가 올바른 위치부터 이어받게 함 */
		if (ret < 0 || !count) /* [한국어] 에러가 있었거나 이미 요청을 전부 채웠는지 확인 */
			goto reexpand; /* [한국어] 더 이상 buffered 경로가 필요 없으므로 마무리 단계로 점프 */
	}

	/*
	 * Take i_rwsem and invalidate_lock to avoid racing with set_blocksize
	 * changing i_blkbits/folio order and punching out the pagecache.
	 */
	inode_lock_shared(bd_inode); /* [한국어] set_blocksize 등과의 경쟁을 막기 위해 공유 락 획득 */
	ret = filemap_read(iocb, to, ret); /* [한국어] 나머지(또는 애초 buffered 전체)를 페이지 캐시 경유로 읽음 - 캐시 미스 시 blkdev_read_folio/readahead 가 읽기 bio 로 채운다. ret(direct 단계 결과)를 기저 값으로 누적 */
	inode_unlock_shared(bd_inode); /* [한국어] 공유 락 해제 */

reexpand:
	if (unlikely(shorted)) /* [한국어] 앞서 EOF로 잘라낸 적이 있는지 확인 */
		iov_iter_reexpand(to, iov_iter_count(to) + shorted); /* [한국어] 잘라낸 만큼을 iter 길이 계산에 복원 - 호출자가 원래 요청 크기를 올바르게 인식하도록 함 */
	return ret; /* [한국어] 최종 결과(읽은 바이트 수 또는 에러) 반환 */
}

/*
 * [한국어] BLKDEV_FALLOC_FL_SUPPORTED - blkdev_fallocate()가 인식하는 fallocate(2) 모드 플래그 집합.
 * 이 파일에서 지원을 선언하는 네 플래그를 OR로 묶은 매크로이며, 각 비트의 의미는 다음과 같다:
 *   - FALLOC_FL_KEEP_SIZE: 이 연산으로 인해 파일(장치) 크기가 늘어나지 않아야 함을 지시.
 *     block device는 크기가 고정적이므로 다른 플래그와 조합되어 "범위를 넘지 말라"는
 *     의미로 주로 사용된다.
 *   - FALLOC_FL_PUNCH_HOLE: 지정 범위를 "구멍"으로 만듦 - 이 파일에서는
 *     BLKDEV_ZERO_NOFALLBACK 으로 blkdev_issue_zeroout()에 전달된다. 즉
 *     write-zeroes 오프로드가 가능할 때만 수행하고, 지원하지 않는 장치에서
 *     범위 전체를 실제로 0 으로 덮어쓰는 폴백은 하지 않는다(그렇게 하면
 *     "빠른 해제"라는 의도와 정반대가 되므로).
 *   - FALLOC_FL_ZERO_RANGE: 지정 범위를 0으로 채우되 할당 해제는 하지 말라
 *     (BLKDEV_ZERO_NOUNMAP). blk-lib.c 가 이 bio 에 REQ_NOUNMAP 을 붙인다.
 *   - FALLOC_FL_WRITE_ZEROES: 명시적으로 "0 쓰기"를 요청 - 제약 없이
 *     write-zeroes 오프로드와 폴백을 모두 허용한다.
 *
 * NVMe 접점(코드 확인됨): REQ_OP_WRITE_ZEROES 는 nvme_setup_write_zeroes()가
 * nvme_cmd_write_zeroes 옵코드로 번역한다(drivers/nvme/host/core.c:1206).
 * 위 REQ_NOUNMAP 이 여기서 갈린다 - REQ_NOUNMAP 이 없고 네임스페이스가
 * NVME_NS_DEAC 를 지원할 때만 control 에 NVME_WZ_DEAC(Deallocate) 비트를
 * 세운다(core.c:1213-1215). 따라서 ZERO_RANGE 는 "진짜로 0 을 저장하라",
 * PUNCH_HOLE/WRITE_ZEROES 는 "할당을 놓아줘도 좋다"에 해당한다.
 * 예외적으로 NVME_QUIRK_DEALLOCATE_ZEROES 가 붙은 컨트롤러에서는
 * nvme_setup_write_zeroes()가 곧바로 nvme_setup_discard()로 우회해
 * Dataset Management + NVME_DSMGMT_AD 로 처리한다(core.c:1203-1204).
 * 이 매크로는 여러 줄에 걸친 백슬래시(\) 라인 연속으로 정의되어 있으므로, 값 자체를
 * 바꾸지 않기 위해 매크로 본문 내부에는 별도 인라인 주석을 삽입하지 않았다(백슬래시
 * 다음에 다른 문자가 오면 라인 연속이 깨져 매크로가 잘리는 사고로 이어질 수 있음).
 */
#define	BLKDEV_FALLOC_FL_SUPPORTED					\
		(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE |		\
		 FALLOC_FL_ZERO_RANGE | FALLOC_FL_WRITE_ZEROES)

/*
 * [한국어]
 * blkdev_fallocate - 블록 장치 노드의 fallocate(2) 구현
 *
 * @file: 대상 block 장치 노드.
 * @mode: FALLOC_FL_* 플래그 조합(BLKDEV_FALLOC_FL_SUPPORTED로 제한된 값만 허용).
 * @start: 연산 대상 범위의 시작 오프셋.
 * @len: 연산 대상 범위의 길이.
 * @return: 0 성공, 음수 errno 실패.
 *
 * struct file_operations.fallocate 구현. 일반 파일시스템의 fallocate와
 * 달리 block device는 공간을 "미리 할당"할 필요가 없으므로(이미 전체
 * 공간이 존재), 이 구현은 요청을 세 가지 중 하나로 해석해 모두
 * blkdev_issue_zeroout()에 위임한다: (1) FALLOC_FL_WRITE_ZEROES - 제약 없는
 * 명시적 0 쓰기, (2) FALLOC_FL_PUNCH_HOLE - 오프로드가 될 때만 수행하고
 * 폴백은 금지(BLKDEV_ZERO_NOFALLBACK), (3) FALLOC_FL_ZERO_RANGE - 할당
 * 해제 없이 0으로 채움(BLKDEV_ZERO_NOUNMAP).
 * 처리 순서: 지원하지 않는 플래그 거부 -> WRITE_ZEROES인데 unmap write
 * zeroes를 지원하지 않는 장치면 거부 -> 범위가 장치 끝을 넘는지 검사
 * (KEEP_SIZE면 잘라서 허용) -> 논리 블록 정렬 검사 -> inode/페이지 캐시
 * 락 획득 -> mode에 따라 BLKDEV_ZERO_* 플래그 결정 -> 해당 범위 페이지
 * 캐시 무효화(truncate_bdev_range) -> blkdev_issue_zeroout()로 실제 bio
 * 발행. 페이지 캐시를 먼저 버리는 순서가 중요하다 - 나중에 버리면 이미
 * 매체가 0 이 된 범위 위로 낡은 더티 페이지가 다시 writeback 될 수 있다. 실행 컨텍스트: fallocate(2) 시스템 콜을 처리하는 유저
 * 프로세스 컨텍스트에서 inode_lock과 filemap_invalidate_lock을 잡고
 * 동기적으로 실행되며 완료까지 블로킹한다.
 * 호출자: vfs_fallocate() (def_blk_fops.fallocate로 등록됨).
 * 호출 대상: bdev_write_zeroes_unmap_sectors, bdev_nr_bytes,
 *           bdev_logical_block_size, truncate_bdev_range,
 *           blkdev_issue_zeroout, file_to_blk_mode.
 * 에러 경로: 각 사전 검증 단계 실패는 즉시 반환(-EOPNOTSUPP/-EINVAL).
 * 락 획득 이후의 실패(알 수 없는 mode, truncate_bdev_range 실패)는 fail
 * 레이블로 점프해 반드시 락을 해제한 뒤 에러를 반환한다.
 * 호출 체인:
 *   fallocate(2) -> vfs_fallocate -> [blkdev_fallocate] -> truncate_bdev_range
 *     -> blkdev_issue_zeroout -> submit_bio(REQ_OP_WRITE_ZEROES)
 *     -> blk_mq_submit_bio -> mq_ops->queue_rq (NVMe면 nvme_queue_rq
 *        -> nvme_setup_write_zeroes)
 */
static long blkdev_fallocate(struct file *file, int mode, loff_t start,
			     loff_t len)
{
	struct inode *inode = bdev_file_inode(file); /* [한국어] bdev inode 획득 - 락과 페이지 캐시 매핑에 사용 */
	struct block_device *bdev = I_BDEV(inode); /* [한국어] 대상 block_device 포인터 */
	loff_t end = start + len - 1; /* [한국어] 범위의 마지막 바이트 오프셋(포함) 계산 */
	loff_t isize; /* [한국어] 장치의 현재 크기 - 범위 검사 기준 */
	unsigned int flags; /* [한국어] blkdev_issue_zeroout에 전달할 BLKDEV_ZERO_* 플래그 */
	int error; /* [한국어] 각 단계의 에러 코드 */

	/* Fail if we don't recognize the flags. */
	if (mode & ~BLKDEV_FALLOC_FL_SUPPORTED) /* [한국어] 지원하는 4개 플래그 이외의 비트가 섞여 있는지 확인 - 지원하지 않는 fallocate 플래그 거부 */
		return -EOPNOTSUPP; /* [한국어] 인식할 수 없는 플래그는 거부 */
	/*
	 * Don't allow writing zeroes if the device does not enable the
	 * unmap write zeroes operation.
	 */
	if ((mode & FALLOC_FL_WRITE_ZEROES) && /* [한국어] 명시적 Write Zeroes 가 요청되었는지 확인 */
	    !bdev_write_zeroes_unmap_sectors(bdev)) /* [한국어] 장치가 이 연산(unmap 방식 write zeroes)을 지원하지 않는지 확인 */
		return -EOPNOTSUPP; /* [한국어] 미지원 장치에서는 거부 */

	/* Don't go off the end of the device. */
	isize = bdev_nr_bytes(bdev); /* [한국어] 장치 전체 크기(바이트) 조회 - 아래 범위 검사의 기준. 이 값은 set_capacity()가 설정한 섹터 수에서 유래한다 */
	if (start >= isize) /* [한국어] 시작 위치가 이미 장치 끝 이상인지 확인 */
		return -EINVAL; /* [한국어] 유효 범위 밖이므로 거부 */
	if (end >= isize) { /* [한국어] 끝 위치가 장치 크기를 넘는지 확인 */
		if (mode & FALLOC_FL_KEEP_SIZE) { /* [한국어] 크기를 유지해야 하는 모드인지 확인(범위를 장치 끝까지로 잘라 허용 가능) */
			len = isize - start; /* [한국어] 실제 처리 가능한 길이로 축소 */
			end = start + len - 1; /* [한국어] 축소된 길이에 맞춰 끝 오프셋 재계산 */
		} else /* [한국어] KEEP_SIZE가 아니면 범위 초과를 허용하지 않음(block device는 확장 불가) */
			return -EINVAL; /* [한국어] 거부 */
	}

	/*
	 * Don't allow IO that isn't aligned to logical block size.
	 */
	if ((start | len) & (bdev_logical_block_size(bdev) - 1)) /* [한국어] 시작/길이가 논리 블록 경계에 정렬되었는지 OR 후 마스크 한 번으로 검사 - 장치는 논리 블록보다 작은 단위를 주소지정할 수 없다(NVMe 라면 이 크기가 1 << LBADS) */
		return -EINVAL; /* [한국어] 미정렬이면 거부 */

	inode_lock(inode); /* [한국어] 이 범위에 대한 다른 수정과의 배타적 접근을 위해 inode 락 획득 */
	filemap_invalidate_lock(inode->i_mapping); /* [한국어] 페이지 캐시 무효화 락 획득 - truncate_bdev_range와의 경쟁 방지 */

	switch (mode) { /* [한국어] mode 를 부분 마스크가 아니라 "정확히 이 값인가"로 비교한다 - 아래 나열되지 않은 조합(예: PUNCH_HOLE 단독)은 default 로 떨어져 -EOPNOTSUPP 이 된다 */
	case FALLOC_FL_ZERO_RANGE: /* [한국어] 범위를 0으로 채우되 할당은 유지 */
	case FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE: /* [한국어] 위와 같으나 크기 확장 금지 플래그가 함께 온 경우(장치는 어차피 크기가 고정이라 동작은 동일) */
		flags = BLKDEV_ZERO_NOUNMAP; /* [한국어] 할당 해제 없이 0으로 채움 - "읽으면 0" 은 보장하되 매체 할당은 건드리지 않는다 */
		break;
	case FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE: /* [한국어] 범위를 구멍으로 만들라는 요청. 장치에게는 "이 LBA 범위는 더 이상 쓰지 않는다"는 통보에 해당한다 */
		flags = BLKDEV_ZERO_NOFALLBACK; /* [한국어] 할당 해제(discard/unmap)만 시도하고, 장치가 지원하지 않으면 실제 0 쓰기로 폴백하지 말고 실패하라 - 폴백하면 "빠른 해제"가 아니라 범위 전체 쓰기가 되어 의도와 정반대가 된다 */
		break;
	case FALLOC_FL_WRITE_ZEROES: /* [한국어] 명시적 0 쓰기 요청 */
		flags = 0; /* [한국어] 제약 없음 - unmap 방식과 실제 0 쓰기 폴백을 모두 허용한다 */
		break;
	default:
		error = -EOPNOTSUPP; /* [한국어] 지원 플래그 집합 내에 있지만 이 함수가 명시적으로 처리하지 않는 조합(예: PUNCH_HOLE 단독) */
		goto fail; /* [한국어] 락 해제 경로로 점프 */
	}

	/*
	 * Invalidate the page cache, including dirty pages, for valid
	 * de-allocate mode calls to fallocate().
	 */
	error = truncate_bdev_range(bdev, file_to_blk_mode(file), start, end); /* [한국어] 해당 범위의 페이지 캐시(더티 페이지 포함)를 무효화 - 이후 매체 상태와 캐시가 어긋나지 않도록 함 */
	if (error) /* [한국어] 무효화 실패 확인 */
		goto fail; /* [한국어] 실패 시 락 해제 경로로 점프 */

	error = blkdev_issue_zeroout(bdev, start >> SECTOR_SHIFT, /* [한국어] 실제 bio 발행을 block/blk-lib.c 에 위임하고 완료까지 대기한다. 이 함수는 discard bio 를 만들지 않는다 - 할당 해제는 write-zeroes 의 unmap 힌트로만 전달된다 */
				     len >> SECTOR_SHIFT, GFP_KERNEL, flags); /* [한국어] 오프셋/길이를 바이트에서 512B 섹터 단위로 환산해 넘긴다. blkdev_issue_zeroout()은 장치가 write-zeroes 오프로드를 보고하면 REQ_OP_WRITE_ZEROES bio 를 만들고, 아니면 0 으로 채운 페이지를 실제로 쓰는 폴백으로 내려간다(block/blk-lib.c) */
 fail:
	filemap_invalidate_unlock(inode->i_mapping); /* [한국어] 페이지 캐시 무효화 락 해제 - 성공/실패 모든 경로에서 반드시 실행 */
	inode_unlock(inode); /* [한국어] inode 락 해제 */
	return error; /* [한국어] 최종 결과 반환 */
}

/*
 * [한국어]
 * blkdev_mmap_prepare - block device 노드의 mmap(2) 준비(struct file_operations.mmap_prepare)
 *
 * @desc: mmap을 준비 중인 VMA(가상 메모리 영역) 서술자.
 * @return: generic_file_mmap_prepare()/generic_file_readonly_mmap_prepare()의
 *          반환값(0 성공, 음수 errno 실패).
 *
 * block device를 mmap하면 실제로는 그 address_space(def_blk_aops가 연결된
 * 페이지 캐시)를 매핑하는 것과 같다. 장치가 읽기 전용으로 열려 있다면
 * PROT_WRITE로 매핑되는 것을 막아야 하므로
 * generic_file_readonly_mmap_prepare()로 위임하고, 그렇지 않으면 일반
 * generic_file_mmap_prepare()로 위임한다. 이 함수 자체는 페이지 폴트를
 * 처리하지 않으며, 실제 read/write 폴트 처리는 공통 페이지 캐시 폴트
 * 핸들러가 담당하고 캐시 미스 시 blkdev_read_folio 등을 통해 간접적으로
 * 읽기 bio 로 이어진다. 실행 컨텍스트: mmap(2) 시스템 콜을 처리하는 유저
 * 프로세스 컨텍스트.
 * 호출자: VFS의 mmap 준비 경로(def_blk_fops.mmap_prepare로 등록됨).
 * 호출 대상: bdev_read_only, generic_file_readonly_mmap_prepare,
 *           generic_file_mmap_prepare.
 * 에러 경로: 하위 generic_file_*_mmap_prepare의 에러를 그대로 전달.
 * 호출 체인:
 *   mmap(2) -> VFS mmap 준비 경로 -> [blkdev_mmap_prepare] -> generic_file_mmap_prepare/generic_file_readonly_mmap_prepare
 *   (이후 페이지 폴트 시) 페이지 캐시 폴트 핸들러 -> blkdev_read_folio -> submit_bio -> blk_mq_submit_bio -> mq_ops->queue_rq(NVMe면 nvme_queue_rq)
 */
static int blkdev_mmap_prepare(struct vm_area_desc *desc)
{
	struct file *file = desc->file; /* [한국어] mmap 대상 struct file */

	if (bdev_read_only(I_BDEV(bdev_file_inode(file)))) /* [한국어] 장치가 읽기 전용인지 확인 */
		return generic_file_readonly_mmap_prepare(desc); /* [한국어] 쓰기 매핑을 금지하는 읽기 전용 mmap 준비 경로 사용 */

	return generic_file_mmap_prepare(desc); /* [한국어] 일반(읽기/쓰기 모두 가능한) mmap 준비 경로 사용 */
}

/*
 * [한국어]
 * def_blk_fops - 블록 장치 노드를 위한 struct file_operations 디스패치 테이블
 *
 * VFS는 open(2)로 block 장치 노드를 열 때(이 파일의 blkdev_open을 거쳐)
 * 최종적으로 이 구조체를 struct file->f_op에 연결하고, 이후 모든
 * read/write/ioctl/mmap/fsync 등의 시스템 콜이 이 테이블의 콜백을 통해
 * block layer로 진입한다. .read_iter/.write_iter 가 bio -> submit_bio ->
 * blk_mq_submit_bio -> mq_ops->queue_rq 경로의 시작점이고,
 * .iopoll(iocb_bio_iopoll)은 bio_poll -> blk_mq_poll -> mq_ops->poll 로
 * 완료를 직접 확인하며, .fsync(blkdev_fsync)는 REQ_OP_FLUSH bio 를,
 * .fallocate(blkdev_fallocate)는 REQ_OP_WRITE_ZEROES bio 를 발행한다.
 * .uring_cmd(blkdev_uring_cmd, 이 파일 밖에 정의됨)는 드라이버가 제공하는
 * passthrough ioctl 을 io_uring 으로 발행하는 통로다.
 *
 * NVMe 로 내려갔을 때 각 항목이 무엇이 되는지(모두 드라이버 쪽 코드로
 * 확인된 것):
 *   .read_iter/.write_iter -> nvme_setup_rw() -> nvme_cmd_read/nvme_cmd_write
 *   .iopoll                -> nvme_poll()(폴링 전용 큐에 한함)
 *   .fsync                 -> nvme_setup_flush() -> 옵코드 0x00 Flush
 *   .fallocate             -> nvme_setup_write_zeroes() -> Write Zeroes
 *   .uring_cmd             -> NVMe passthrough(NVME_IOCTL_IO_CMD 계열)
 */
const struct file_operations def_blk_fops = {
	.open		= blkdev_open, /* [한국어] open(2) -> bdev 참조 획득 + holder 등록(bdev_open) */
	.release	= blkdev_release, /* [한국어] close(2) -> bd_openers 감소 및 마지막 닫힘 시 캐시 정리(bdev_release) */
	.llseek		= blkdev_llseek, /* [한국어] lseek(2) -> 장치 크기(i_size)를 상한으로 하는 고정 크기 위치 계산 */
	.read_iter	= blkdev_read_iter, /* [한국어] read(2)/pread(2) -> blkdev_direct_IO(O_DIRECT) 또는 filemap_read(buffered) */
	.write_iter	= blkdev_write_iter, /* [한국어] write(2)/pwrite(2) -> blkdev_direct_write(O_DIRECT) 또는 blkdev_buffered_write */
	.iopoll		= iocb_bio_iopoll, /* [한국어] IOCB_HIPRI 요청의 완료를 인터럽트 대신 직접 확인 -> bio_poll -> blk_mq_poll -> mq_ops->poll */
	.mmap_prepare	= blkdev_mmap_prepare, /* [한국어] mmap(2) 준비 - 실제 I/O 는 나중에 페이지 폴트가 페이지 캐시 경로를 태우면서 발생한다 */
	.fsync		= blkdev_fsync, /* [한국어] fsync(2)/fdatasync(2) -> writeback 후 REQ_OP_FLUSH bio 발행 */
	.unlocked_ioctl	= blkdev_ioctl, /* [한국어] block device 전용 ioctl(2) 처리(block/ioctl.c에 정의) */
#ifdef CONFIG_COMPAT
	.compat_ioctl	= compat_blkdev_ioctl, /* [한국어] 32비트 호환(compat) ioctl(2) 처리 - CONFIG_COMPAT 빌드에서만 등록 */
#endif
	.splice_read	= filemap_splice_read, /* [한국어] splice(2) 읽기 - 페이지 캐시를 경유하는 표준 헬퍼 재사용 */
	.splice_write	= iter_file_splice_write, /* [한국어] splice(2) 쓰기 - iov_iter 기반 표준 헬퍼가 결국 write_iter(blkdev_write_iter)를 호출 */
	.fallocate	= blkdev_fallocate, /* [한국어] fallocate(2) -> 페이지 캐시 무효화 후 REQ_OP_WRITE_ZEROES bio 발행 */
	.uring_cmd	= blkdev_uring_cmd, /* [한국어] io_uring 커맨드 -> 드라이버 passthrough ioctl 통로(이 파일 밖에 정의; NVMe 라면 NVME_IOCTL_IO_CMD 계열) */
	.fop_flags	= FOP_BUFFER_RASYNC, /* [한국어] buffered read를 비동기(io_uring 등)로도 안전하게 처리할 수 있음을 VFS에 알리는 플래그 */
};

/*
 * [한국어]
 * blkdev_init - block/fops.c 모듈 초기화: blkdev_dio_pool bio_set 생성
 *
 * @return: 0 성공, 음수 errno 실패(bioset_init 실패 시 - 통상 부트 초기 단계
 *          메모리 부족 상황에서만 발생).
 *
 * direct I/O 경로(__blkdev_direct_IO, __blkdev_direct_IO_async)가 사용할
 * blkdev_dio_pool을 부트/모듈 로드 시점에 한 번 초기화한다. 4는 mempool의
 * 최소 예약 개수(min_nr)로, 메모리 부족 상황에서도 최소한 이 개수만큼은
 * bio(+dio) 할당이 전진할 수 있도록 보장한다. offsetof(struct blkdev_dio,
 * bio)를 bioset_init에 넘기는 것이 이 풀의 핵심으로, bio_set이 "bio 앞에
 * front_pad 바이트만큼 여유 공간을 두고 할당"하도록 지시해 결과적으로
 * bio_alloc_bioset() 호출 시 struct blkdev_dio 전체(그 안에 내장된 bio
 * 포함)가 한 번의 할당으로 만들어지게 한다. BIOSET_NEED_BVECS는 이 풀이
 * bio_vec 배열도 함께 관리하도록, BIOSET_PERCPU_CACHE는 CPU별 캐시를 두어
 * 할당 성능을 높이도록 지시하는 플래그다. 실행 컨텍스트: 커널 부트 또는
 * 모듈 로드 시점에 단 한 번 호출된다(module_init 매크로에 의해 등록).
 * 호출자: 커널 초기화 인프라(module_init 매크로를 통해 등록된 초기화
 *         함수 목록을 부트 시점에 순회 호출).
 * 호출 대상: bioset_init (block/bio.c).
 * 에러 경로: bioset_init 실패 시 그 errno를 그대로 반환 - 초기화 실패는
 * 부트 실패로 이어질 수 있는 치명적 상황이다.
 * 호출 체인:
 *   커널 부트/모듈 로드 -> module_init(blkdev_init) -> [blkdev_init] -> bioset_init(&blkdev_dio_pool, ...)
 *   (이후) __blkdev_direct_IO/__blkdev_direct_IO_async -> bio_alloc_bioset(..., &blkdev_dio_pool)
 */
static __init int blkdev_init(void)
{
	return bioset_init(&blkdev_dio_pool, 4, /* [한국어] blkdev_dio_pool을 초기화 - 최소 예약 개수 4 */
				offsetof(struct blkdev_dio, bio), /* [한국어] bio 앞에 struct blkdev_dio의 나머지 필드만큼 front_pad를 두도록 지정 - dio 구조체 내 bio 오프셋 지정으로 bio+dio 단일 할당 실현 */
				BIOSET_NEED_BVECS|BIOSET_PERCPU_CACHE); /* [한국어] bio_vec 배열도 함께 관리 + CPU별 캐시로 할당 성능 향상 */
}
module_init(blkdev_init); /* [한국어] blkdev_init을 부트/모듈 로드 시 1회 실행되는 초기화 함수로 등록 */
