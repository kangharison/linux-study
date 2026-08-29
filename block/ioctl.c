// SPDX-License-Identifier: GPL-2.0
/*
 * [한국어 설명] block/ioctl.c — 블록 디바이스 범용(generic) ioctl 진입점 (block/ioctl.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 블록 특수 파일(예: /dev/nvme0n1, /dev/sda)에 대해 사용자 공간이
 * open() 후 ioctl(2) 시스템 호출로 요청하는, 특정 파일시스템이나 특정 드라이버에
 * 종속되지 않는 "블록 디바이스 공통" 명령을 처리하는 진입점이다. 파티션 테이블
 * 조작(BLKPG류), 논리/물리 블록 크기 조회(BLKSSZGET/BLKPBSZGET), 전체 용량
 * 조회(BLKGETSIZE/BLKGETSIZE64), TRIM/UNMAP(BLKDISCARD), 영역 0채우기(BLKZEROOUT),
 * 보안 삭제(BLKSECDISCARD), 버퍼 캐시 동기화(BLKFLSBUF), 읽기전용 전환(BLKROSET),
 * 레거시 CHS 지오메트리(HDIO_GETGEO), Persistent Reservation(IOC_PR_*), zone
 * 관리, blktrace 설정, 인라인 암호화 키 관리(BLKCRYPTO*) 등을 이 한 파일이 모아
 * 처리한다. 이 파일이 직접 처리하지 못하는 명령(NVMe passthrough, 드라이버별
 * vendor ioctl 등)은 -ENOIOCTLCMD를 반환해 호출자가 gendisk의
 * block_device_operations.ioctl/compat_ioctl로 위임하도록 한다. 파일 후반부에는
 * io_uring 기반 비동기 BLOCK_URING_CMD_DISCARD 명령 처리도 포함되어, 전통적인
 * 동기 ioctl 경로와 io_uring 기반 비동기 경로가 이 한 파일 안에 공존한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용자 공간이 ioctl(fd, BLKDISCARD, &range) 등을 호출하면 VFS의 vfs_ioctl()이
 * struct file_operations의 unlocked_ioctl/compat_ioctl 콜백을 호출한다. 블록
 * 특수 파일의 경우 이 콜백이 궁극적으로 blkdev_ioctl()/compat_blkdev_ioctl()
 * (이 파일)로 이어진다. 즉 이 파일은 "VFS ioctl 디스패치"와 "블록 계층 실질
 * 처리(bio 생성/제출, request_queue 조회, 파티션 테이블 조작)" 사이의 경계에
 * 위치한다. 대표적 호출 체인:
 *   사용자 공간 ioctl(BLKDISCARD) → vfs_ioctl → blkdev_ioctl(이 파일)
 *   → blkdev_common_ioctl(이 파일) → blk_ioctl_discard(이 파일)
 *   → blk_alloc_discard_bio(block/blk-lib.c)/bio_chain_and_submit(block/bio.c)
 *   → submit_bio → submit_bio_noacct → blk_mq_submit_bio → blk_mq_get_request
 *   → (드라이버, 예: NVMe) nvme_queue_rq → nvme_sq_copy_cmd/nvme_write_sq_db(도어벨 갱신)
 * io_uring 경로는 io_uring 커맨드 인프라(io_uring/cmd.c 등)가
 * BLOCK_URING_CMD_DISCARD를 blkdev_uring_cmd()(이 파일)로 전달하며, 이후
 * blkdev_cmd_discard()가 동일한 bio 제출 경로를 타되 완료를 io_uring completion
 * queue로 비동기 통지한다는 점만 다르다. 이 파일의 함수들은 모두 ioctl/io_uring
 * 시스템 호출을 처리하는 프로세스(태스크) 컨텍스트에서 실행되며,
 * bio_cmd_bio_end_io()만 예외적으로 bio 완료(인터럽트/softirq 계열) 컨텍스트에서
 * 실행된 뒤 task work로 다시 프로세스 컨텍스트에 위임한다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 모듈: block/blk-lib.c(blk_alloc_discard_bio,
 * blkdev_issue_secure_erase, blkdev_issue_zeroout — discard/write-zeroes/
 * secure-erase의 실제 bio 생성·제출), block/bio.c(bio_chain_and_submit,
 * bio_submit_or_kill), block/blk.h(truncate_bdev_range, file_to_blk_mode,
 * blk_get_meta_cap, blkdev_zone_mgmt_ioctl, blkdev_report_zones_ioctl,
 * disk_scan_partitions 등 이 디렉터리 전용 비공개 선언), block/blk-crypto-
 * internal.h(blk_crypto_ioctl), block/genhd.c 계열(bdev_add_partition/
 * bdev_del_partition/bdev_resize_partition), kernel의 blktrace 서브시스템
 * (blk_trace_ioctl), 각 드라이버가 채워 넣는 gendisk의 block_device_operations
 * (pr_ops, set_read_only, getgeo, ioctl/compat_ioctl 콜백). 이 파일에 의존하는
 * 모듈: 블록 계층의 파일 오퍼레이션 테이블(def_blk_fops 계열)이 이 파일의
 * blkdev_ioctl/compat_blkdev_ioctl/blkdev_compat_ptr_ioctl/blkdev_uring_cmd를
 * block_device_operations 콜백으로 등록해 사용한다. 데이터 흐름: (1) 사용자
 * 공간 → 커널: copy_from_user()/get_user()로 range[2](start,len 바이트),
 * blkpg_partition, pr_registration/reservation/preempt/clear 등 구조체를 읽어
 * 오고, (2) 커널 → 사용자 공간: put_user()/copy_to_user()로 크기·지오메트리·
 * PR 키 목록 등을 반환한다. 공유하는 핵심 자료구조는 struct block_device(대상
 * 디바이스), request_queue의 queue_limits(논리/물리 블록 크기, discard/secure-
 * erase/write-zeroes 최대 섹터 등 — block/blk-settings.c가 채움), struct bio
 * (discard/uring 경로의 실제 I/O 단위), struct pr_ops(드라이버 제공 PR 콜백
 * 테이블)이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - blkdev_ioctl()/compat_blkdev_ioctl(): native/32비트 호환 ioctl의 진입점.
 *   자료구조가 다른 명령을 먼저 직접 처리하고 나머지는 blkdev_common_ioctl()에
 *   위임한다.
 * - blkdev_common_ioctl(): BLKDISCARD/BLKZEROOUT/BLKSECDISCARD/BLKGETDISKSEQ/
 *   zone/PR/crypto 등 native·compat 공통 명령을 switch로 분기하는 핵심 디스패처.
 * - blk_ioctl_discard()/blk_ioctl_secure_erase()/blk_ioctl_zeroout(): 각각
 *   BLKDISCARD/BLKSECDISCARD/BLKZEROOUT을 페이지 캐시 무효화 후 blk-lib.c
 *   헬퍼로 위임하는 실질 처리 함수.
 * - blkpg_do_ioctl()/blkpg_ioctl()/compat_blkpg_ioctl(): BLKPG 파티션 추가/
 *   삭제/크기조정.
 * - blkdev_pr_*(): IOC_PR_REGISTER/RESERVE/RELEASE/PREEMPT/CLEAR/READ_KEYS/
 *   READ_RESERVATION을 드라이버의 pr_ops로 전달하는 Persistent Reservation
 *   계열 함수.
 * - blkdev_uring_cmd()/blkdev_cmd_discard()/blk_cmd_complete()/
 *   bio_cmd_bio_end_io(): io_uring 기반 비동기 discard 명령 처리 경로.
 * - struct blk_iou_cmd: io_uring 명령의 private 데이터(결과 코드, non-blocking
 *   여부).
 */
#include <linux/capability.h> /* [한국어] capable()/CAP_SYS_ADMIN 등 권한 검사 API - BLKPG/BLKROSET/BLKRASET/BLKBSZSET 등 관리자 전용 ioctl 권한 확인에 사용 */
#include <linux/compat.h> /* [한국어] compat_ptr()/compat_int_t/compat_caddr_t 등 32비트 호환 계층 정의 - CONFIG_COMPAT 블록의 compat_blkdev_ioctl 계열에서 사용 */
#include <linux/blkdev.h> /* [한국어] struct block_device, blk_mode_t, bdev_*() accessor, BLKDEV_ZERO_* 플래그 등 블록 계층 핵심 타입 선언 */
#include <linux/export.h> /* [한국어] EXPORT_SYMBOL() 매크로 - blkdev_compat_ptr_ioctl()을 다른 커널 모듈(드라이버)에 노출하기 위해 필요 */
#include <linux/gfp.h> /* [한국어] GFP_KERNEL 등 메모리 할당 플래그 - discard bio/PR 키 버퍼(kvzalloc) 등 할당에 사용 */
#include <linux/blkpg.h> /* [한국어] struct blkpg_partition, BLKPG_ADD/DEL/RESIZE_PARTITION 등 BLKPG ioctl 전용 자료구조/상수 */
#include <linux/hdreg.h> /* [한국어] struct hd_geometry, HDIO_GETGEO - 레거시 CHS(Cylinder-Head-Sector) 지오메트리 조회 ioctl 정의 */
#include <linux/backing-dev.h> /* [한국어] backing_dev_info의 ra_pages 등 - BLKRAGET/BLKRASET(readahead 페이지 수) 처리에 사용 */
#include <linux/fs.h> /* [한국어] struct file, inode_lock()/set_blocksize() 등 VFS 공통 정의 */
#include <linux/blktrace_api.h> /* [한국어] blk_trace_ioctl() 선언 - BLKTRACESETUP/START/STOP/TEARDOWN 명령을 blktrace 서브시스템으로 위임할 때 사용 */
#include <linux/pr.h> /* [한국어] struct pr_ops 및 pr_registration/reservation/preempt/clear/read_keys/read_reservation 등 Persistent Reservation 자료구조 */
#include <linux/uaccess.h> /* [한국어] copy_from_user()/copy_to_user()/get_user()/put_user() - 사용자 공간과 커널 간 데이터 교환에 필수 */
#include <linux/pagemap.h> /* [한국어] filemap_invalidate_lock()/filemap_invalidate_pages() 등 페이지 캐시 무효화 API - discard/zeroout 전 dirty 페이지 처리 */
#include <linux/io_uring/cmd.h> /* [한국어] struct io_uring_cmd, io_uring_cmd_to_pdu() 등 io_uring 커맨드 인프라 - blkdev_uring_cmd() 비동기 경로에서 사용 */
#include <linux/blk-integrity.h> /* [한국어] blk_get_meta_cap() 등 블록 무결성(PI, Protection Information) 능력 조회 - blkdev_common_ioctl()의 default 분기에서 사용 */
#include <uapi/linux/blkdev.h> /* [한국어] BLKDISCARD/BLKZEROOUT/BLKGETSIZE64 등 사용자 공간에 노출되는 ioctl 명령 번호(uAPI) 정의 */
#include "blk.h" /* [한국어] block/ 디렉터리 내부 전용 헤더 - truncate_bdev_range, file_to_blk_mode, blk_alloc_discard_bio, blkdev_zone_mgmt_ioctl 등 비공개 선언 포함 */
#include "blk-crypto-internal.h" /* [한국어] blk_crypto_ioctl() 선언 - BLKCRYPTOIMPORTKEY/GENERATEKEY/PREPAREKEY 등 인라인 암호화 키 관리 ioctl에서 사용 */

/*
 * [한국어]
 * blkpg_do_ioctl - BLKPG 파티션 추가/삭제/크기조정의 공통 실행부
 *
 * @bdev: BLKPG 대상 블록 디바이스. 반드시 "전체 디스크"여야 하며(파티션 위에
 *      또 다른 파티션을 만들 수 없음), bdev->bd_disk가 실제 파티션 테이블을 갖는다.
 * @upart: 사용자 공간의 struct blkpg_partition 포인터. pno(파티션 번호),
 *      start/length(바이트 단위 오프셋/길이) 필드를 담고 있다.
 * @op: BLKPG_ADD_PARTITION / BLKPG_DEL_PARTITION / BLKPG_RESIZE_PARTITION 중 하나.
 * @return: 성공 시 0 또는 하위 함수의 반환값, 실패 시 -EACCES/-EFAULT/-EINVAL 등의
 *      음수 errno.
 *
 * 사용자 공간 파티션 관리 도구(parted, sfdisk, kpartx, device-mapper 등)가
 * 재부팅 없이 커널 파티션 테이블을 갱신할 때 쓰는 저수준 진입점이다. start/
 * length(바이트)를 섹터 단위로 변환하기 전에 (1) 관리자 권한, (2) 사용자
 * 구조체 복사 가능 여부, (3) 대상이 파티션이 아닌 전체 디스크인지, (4) 파티션
 * 번호 유효성, (5) 논리 블록 크기 정렬, (6) 산술 오버플로우, (7) 디스크 용량
 * 초과 여부를 순서대로 검증한다. 검증을 통과하면 실제 파티션 테이블 조작은
 * block/genhd.c 계열의 bdev_add_partition()/bdev_del_partition()/
 * bdev_resize_partition()에 위임한다. ioctl 시스템 호출을 처리하는 프로세스
 * 컨텍스트에서 실행되며, 파티션 테이블 갱신 자체의 동시성 보호(디스크 단위
 * lock)는 피호출 함수 쪽 책임이다.
 * 호출자: blkpg_ioctl(), compat_blkpg_ioctl().
 * 피호출자: capable(), copy_from_user(), bdev_is_partition(), bdev_del_partition(),
 *          IS_ALIGNED(), bdev_logical_block_size(), get_capacity(),
 *          check_add_overflow(), bdev_add_partition(), bdev_resize_partition().
 *
 * 호출 체인:
 *   사용자 공간 ioctl(BLKPG) → blkdev_ioctl/compat_blkdev_ioctl →
 *   blkpg_ioctl/compat_blkpg_ioctl → [blkpg_do_ioctl] → bdev_add_partition 등
 */
static int blkpg_do_ioctl(struct block_device *bdev,
			  struct blkpg_partition __user *upart, int op)
{
	struct gendisk *disk = bdev->bd_disk; /* [한국어] 대상 디스크의 gendisk 획득 - 파티션 테이블(디스크 전체 기준 capacity, add/del/resize)을 다루려면 필요 */
	struct blkpg_partition p; /* [한국어] 사용자 공간에서 복사해 올 파티션 정보(pno/start/length)를 담을 지역 변수 */
	sector_t start, length, capacity, end; /* [한국어] 바이트 단위 요청을 섹터 단위로 변환한 값과 오버플로우 검사용 end를 담을 변수들 */

	if (!capable(CAP_SYS_ADMIN)) /* [한국어] 호출 프로세스가 CAP_SYS_ADMIN capability를 갖는지 검사 - 파티션 테이블 조작은 관리자만 허용 */
		return -EACCES; /* [한국어] 권한 없음 - 사용자 공간에는 EACCES(Permission denied)로 보고 */
	if (copy_from_user(&p, upart, sizeof(struct blkpg_partition))) /* [한국어] 사용자 공간의 blkpg_partition 구조체를 커널로 복사 */
		return -EFAULT; /* [한국어] 복사 실패 - upart가 잘못된 사용자 주소를 가리켰음을 의미 */
	if (bdev_is_partition(bdev)) /* [한국어] BLKPG 대상이 이미 파티션(예: /dev/sda1)이면 그 위에 또 파티션을 만들 수 없음 */
		return -EINVAL; /* [한국어] 파티션 위에서의 파티션 조작 요청 거부 */

	if (p.pno <= 0) /* [한국어] 파티션 번호는 1부터 시작 - 0 이하이면 유효하지 않은 번호 */
		return -EINVAL; /* [한국어] 잘못된 파티션 번호 거부 */

	if (op == BLKPG_DEL_PARTITION) /* [한국어] 삭제 요청이면 start/length 검증 없이 바로 삭제 경로로 분기 */
		return bdev_del_partition(disk, p.pno); /* [한국어] 파티션 번호에 해당하는 파티션을 genhd 파티션 테이블에서 제거 */

	if (p.start < 0 || p.length <= 0 || LLONG_MAX - p.length < p.start) /* [한국어] 시작 오프셋 음수, 길이 0 이하, 또는 start+length가 LLONG_MAX를 넘는 오버플로우인지 검사 */
		return -EINVAL; /* [한국어] 범위 파라미터가 유효하지 않음 */
	/* Check that the partition is aligned to the block size */
	/* [한국어] 위 원문 번역: 파티션이 블록 크기 경계에 정렬되어 있는지 확인 */
	if (!IS_ALIGNED(p.start | p.length, bdev_logical_block_size(bdev))) /* [한국어] start|length가 논리 블록 크기(bdev_logical_block_size)의 배수인지 검사 - NVMe LBA 정렬 위반 방지 */
		return -EINVAL; /* [한국어] 정렬 위반 - 파티션 경계가 LBA 경계와 어긋남 */

	start = p.start >> SECTOR_SHIFT; /* [한국어] 바이트 오프셋을 섹터 단위(SECTOR_SHIFT=9, 즉 512바이트 섹터)로 변환 */
	length = p.length >> SECTOR_SHIFT; /* [한국어] 길이도 동일하게 섹터 단위로 변환 */
	capacity = get_capacity(disk); /* [한국어] 디스크 전체 용량(섹터 수) 조회 - 범위 초과 검사에 사용 */

	if (check_add_overflow(start, length, &end)) /* [한국어] start+length 덧셈이 sector_t 범위를 넘는지 안전하게 검사하며 결과를 end에 저장 */
		return -EINVAL; /* [한국어] 오버플로우 발생 - 요청이 비정상적으로 큼 */

	if (start >= capacity || end > capacity) /* [한국어] 요청한 파티션 범위[start,end)가 디스크 용량을 벗어나는지 검사 */
		return -EINVAL; /* [한국어] 범위가 디스크 용량 초과 */

	switch (op) { /* [한국어] DEL은 위에서 이미 처리됐으므로 여기서는 ADD/RESIZE만 분기 */
	case BLKPG_ADD_PARTITION: /* [한국어] 새 파티션 추가 요청 */
		return bdev_add_partition(disk, p.pno, start, length); /* [한국어] genhd 파티션 테이블에 새 파티션 엔트리(pno, start, length) 생성 */
	case BLKPG_RESIZE_PARTITION: /* [한국어] 기존 파티션 크기 조정 요청 */
		return bdev_resize_partition(disk, p.pno, start, length); /* [한국어] 기존 파티션의 시작/길이 갱신 */
	default: /* [한국어] ADD/DEL/RESIZE 외 알 수 없는 op */
		return -EINVAL; /* [한국어] 지원하지 않는 파티션 연산 */
	}
}

/*
 * [한국어]
 * blkpg_ioctl - BLKPG ioctl의 native(64비트) 진입점
 *
 * @bdev: 대상 블록 디바이스.
 * @arg: 사용자 공간의 struct blkpg_ioctl_arg 포인터. op(연산 종류)와 data(실제
 *      blkpg_partition을 가리키는 포인터) 두 필드만 먼저 읽어온다.
 * @return: blkpg_do_ioctl()의 반환값, 또는 인자 자체를 읽지 못하면 -EFAULT.
 *
 * BLKPG ioctl은 2단계 간접 구조를 갖는다 - 먼저 blkpg_ioctl_arg에서 op와
 * data(사용자 공간 blkpg_partition 포인터)를 얻은 뒤, blkpg_do_ioctl()이
 * data가 가리키는 실제 파티션 정보를 다시 한번 copy_from_user()로 읽는다.
 * 실행 컨텍스트: ioctl 시스템 호출의 프로세스 컨텍스트.
 * 호출자: blkdev_ioctl() (BLKPG case).
 * 피호출자: get_user(), blkpg_do_ioctl().
 *
 * 호출 체인:
 *   blkdev_ioctl → [blkpg_ioctl] → blkpg_do_ioctl
 */
static int blkpg_ioctl(struct block_device *bdev,
		       struct blkpg_ioctl_arg __user *arg)
{
	struct blkpg_partition __user *udata; /* [한국어] blkpg_ioctl_arg.data가 가리키는 실제 사용자 공간 blkpg_partition 포인터를 담을 변수 */
	int op; /* [한국어] blkpg_ioctl_arg.op(ADD/DEL/RESIZE)를 담을 변수 */

	if (get_user(op, &arg->op) || get_user(udata, &arg->data)) /* [한국어] 사용자 공간 blkpg_ioctl_arg에서 op와 data(포인터) 필드를 각각 안전하게 읽음 */
		return -EFAULT; /* [한국어] 둘 중 하나라도 읽기 실패 - 잘못된 arg 포인터 */

	return blkpg_do_ioctl(bdev, udata, op); /* [한국어] 실제 파티션 조작은 blkpg_do_ioctl()에 위임 */
}

#ifdef CONFIG_COMPAT
/*
 * [한국어]
 * struct compat_blkpg_ioctl_arg - 32비트 호환 BLKPG ioctl 인자
 *
 * 64비트 커널에서 32비트 사용자 공간 프로세스가 BLKPG를 호출할 때 사용하는
 * ABI. 필드 의미는 struct blkpg_ioctl_arg(64비트 버전)와 동일하지만, data
 * 포인터 필드만 32비트 폭의 compat_caddr_t로 축소되어 있다.
 */
struct compat_blkpg_ioctl_arg {
/* [한국어] compat_int_t op;
 * BLKPG_ADD_PARTITION/DEL_PARTITION/RESIZE_PARTITION 중 하나.
 * 설정자: 32비트 사용자 공간이 ioctl 호출 시 채움.
 * 읽는 자: compat_blkpg_ioctl()이 get_user()로 읽어 blkpg_do_ioctl()에 전달.
 * 값 범위: BLKPG_* 상수 중 하나 (그 외 값은 blkpg_do_ioctl()의 default에서 -EINVAL).
 * 동기화: 단일 시스템 호출 스택 지역 변수 접근이므로 별도 동기화 불필요. */
	compat_int_t op;
/* [한국어] compat_int_t flags;
 * 예약된 플래그 필드. 커널이 실제로 읽거나 쓰지 않는다(현재 미사용).
 * 설정자: 사용자 공간이 채울 수 있으나 커널은 참조하지 않음.
 * 읽는 자: 없음 - ABI 레이아웃 정렬을 위해서만 존재.
 * 값 범위: 의미 없음(미사용).
 * 동기화: 해당 없음. */
	compat_int_t flags;
/* [한국어] compat_int_t datalen;
 * data가 가리키는 버퍼의 길이. 커널이 실제로 검사하지 않는다(현재 미사용,
 * 고정 크기 struct blkpg_partition을 그대로 복사하기 때문).
 * 설정자: 사용자 공간.
 * 읽는 자: 없음 - compat_blkpg_ioctl()은 datalen을 참조하지 않음.
 * 값 범위: 의미상으로는 sizeof(struct blkpg_partition)이어야 하나 강제되지 않음.
 * 동기화: 해당 없음. */
	compat_int_t datalen;
/* [한국어] compat_caddr_t data;
 * 실제 struct blkpg_partition을 가리키는 32비트 폭 사용자 공간 포인터.
 * 설정자: 32비트 사용자 공간 프로세스가 ioctl 인자로 채움.
 * 읽는 자: compat_blkpg_ioctl()이 get_user()로 읽은 뒤 compat_ptr()로
 *   64비트 커널 포인터로 확장하여 blkpg_do_ioctl()에 전달.
 * 값 범위: 유효한 32비트 사용자 공간 주소 (NULL이면 이후 copy_from_user에서 -EFAULT).
 * 동기화: 해당 없음(단일 호출 스택 데이터). */
	compat_caddr_t data;
};

/*
 * [한국어]
 * compat_blkpg_ioctl - BLKPG ioctl의 32비트 호환 진입점
 *
 * @bdev: 대상 블록 디바이스.
 * @arg: 32비트 사용자 공간의 struct compat_blkpg_ioctl_arg 포인터.
 * @return: blkpg_do_ioctl()의 반환값, 인자를 읽지 못하면 -EFAULT.
 *
 * blkpg_ioctl()의 compat 버전. compat_caddr_t 형태의 32비트 data 포인터를
 * compat_ptr()로 64비트 커널 포인터로 확장한 뒤 blkpg_do_ioctl()에 전달해,
 * BLKPG 파티션 조작이 32/64비트 ABI 간에 동일하게 동작하게 한다.
 * 실행 컨텍스트: ioctl 시스템 호출의 프로세스 컨텍스트(32비트 호환 태스크).
 * 호출자: compat_blkdev_ioctl() (BLKPG case).
 * 피호출자: get_user(), compat_ptr(), blkpg_do_ioctl().
 *
 * 호출 체인:
 *   compat_blkdev_ioctl → [compat_blkpg_ioctl] → blkpg_do_ioctl
 */
static int compat_blkpg_ioctl(struct block_device *bdev,
			      struct compat_blkpg_ioctl_arg __user *arg)
{
	compat_caddr_t udata; /* [한국어] 32비트 폭의 사용자 공간 data 포인터(compat_caddr_t)를 담을 변수 */
	int op; /* [한국어] blkpg_ioctl_arg.op(ADD/DEL/RESIZE)를 담을 변수 */

	if (get_user(op, &arg->op) || get_user(udata, &arg->data)) /* [한국어] 32비트 compat_blkpg_ioctl_arg에서 op와 data(32비트 포인터) 필드를 각각 읽음 */
		return -EFAULT; /* [한국어] 읽기 실패 - 잘못된 arg 포인터 */

	return blkpg_do_ioctl(bdev, compat_ptr(udata), op); /* [한국어] compat_ptr(udata)로 32비트 포인터를 64비트 커널 포인터로 확장 후 blkpg_do_ioctl()에 위임 */
}
#endif

/*
 * Check that [start, start + len) is a valid range from the block device's
 * perspective, including verifying that it can be correctly translated into
 * logical block addresses.
 */
/*
 * [한국어]
 * blk_validate_byte_range - 사용자가 요청한 바이트 범위 [start, start+len)의
 *                           정합성 검증
 *
 * @bdev: 대상 블록 디바이스. 논리 블록 크기(bdev_logical_block_size)와 전체
 *      바이트 수(bdev_nr_bytes)를 조회하는 기준이 된다.
 * @start: 사용자가 요청한 시작 오프셋(바이트 단위).
 * @len: 사용자가 요청한 길이(바이트 단위).
 * @return: 0이면 유효한 범위, -EINVAL이면 정렬/길이/오버플로우/용량 초과 위반.
 *
 * BLKDISCARD, BLKZEROOUT의 io_uring 버전(BLOCK_URING_CMD_DISCARD) 등 여러
 * 진입점이 공유하는 공통 검증 로직이다. (1) start와 len을 OR한 값이 논리
 * 블록 크기의 배수가 아니면 LBA로 정확히 변환할 수 없으므로 거부한다.
 * (2) len이 0이면 아무 작업도 아니므로 거부한다. (3) start+len이 오버플로우
 * 하거나 디바이스 전체 바이트 수를 넘으면 거부한다. 이 함수는 순수 조회/계산
 * 함수로 락을 잡지 않으며, 호출자의 프로세스 컨텍스트에서 동기적으로 실행된다.
 * 호출자: blk_ioctl_discard(), blkdev_cmd_discard().
 * 피호출자: bdev_logical_block_size(), check_add_overflow(), bdev_nr_bytes().
 *
 * 호출 체인:
 *   blk_ioctl_discard / blkdev_cmd_discard → [blk_validate_byte_range]
 */
static int blk_validate_byte_range(struct block_device *bdev,
				   uint64_t start, uint64_t len)
{
	unsigned int bs_mask = bdev_logical_block_size(bdev) - 1; /* [한국어] 논리 블록 크기(NVMe LBA data size)에서 1을 뺀 정렬 검사용 비트마스크 계산 (크기가 2의 거듭제곱이라고 가정) */
	uint64_t end; /* [한국어] start+len 결과(오버플로우 검사 겸용)를 담을 변수 */

	if ((start | len) & bs_mask) /* [한국어] start 또는 len의 하위 비트가 논리 블록 크기 경계에 정렬되지 않았는지 검사 */
		return -EINVAL; /* [한국어] 정렬 위반 - LBA로 정확히 변환 불가능 */
	if (!len) /* [한국어] 길이가 0이면 처리할 대상이 없음 */
		return -EINVAL; /* [한국어] 잘못된 요청으로 거부 */
	if (check_add_overflow(start, len, &end) || end > bdev_nr_bytes(bdev)) /* [한국어] start+len 오버플로우이거나 디바이스 전체 바이트 수(bdev_nr_bytes)를 초과하는지 검사 */
		return -EINVAL; /* [한국어] 범위가 디바이스 용량을 벗어남 */

	return 0; /* [한국어] 모든 검증 통과 - 유효한 범위 */
}

/*
 * [한국어]
 * blk_ioctl_discard - BLKDISCARD ioctl 처리: 지정 범위를 discard(TRIM/UNMAP)
 *
 * @bdev: discard 대상 블록 디바이스.
 * @mode: 파일이 열린 모드(BLK_OPEN_READ/WRITE 등) - 쓰기 권한 확인에 사용.
 * @arg: 사용자 공간의 uint64_t range[2] = {start, len} (바이트 단위)을
 *      가리키는 포인터(unsigned long으로 캐스팅되어 전달됨).
 * @return: 0이면 성공, 그 외 -EFAULT/-EOPNOTSUPP/-EBADF/-EPERM/-EINVAL 및
 *      truncate_bdev_range()/bio_submit_or_kill()이 반환하는 음수 errno.
 *
 * 파일시스템의 fstrim이나 사용자의 blkdiscard(8) 유틸리티가 "이 범위는 더 이상
 * 유효한 데이터가 없다"고 저장장치에 알려, 하드웨어가 해당 공간을 회수(NVMe
 * Dataset Management Deallocate 등)하게 하는 명령이다. 동작 순서: (1) range를
 * 복사해 start/len을 얻는다. (2) 디바이스가 discard를 지원하는지
 * (bdev_max_discard_sectors) 확인한다. (3) 쓰기 가능 모드로 열렸고
 * 읽기전용이 아닌지 확인한다. (4) blk_validate_byte_range()로 범위를
 * 검증한다. (5) inode/filemap 잠금을 잡고 해당 범위의 페이지 캐시를
 * truncate_bdev_range()로 무효화한다(discard 후 stale 캐시가 남지 않도록).
 * (6) blk_alloc_discard_bio()를 반복 호출해 discard_granularity 제한에 맞춰
 * 여러 bio로 쪼개고 bio_chain_and_submit()으로 체인 제출한다(치명적 시그널
 * 대기 중이면 중단). (7) 마지막 bio를 bio_submit_or_kill()로 제출/대기한다.
 * 실행 컨텍스트: ioctl 시스템 호출의 프로세스 컨텍스트. inode_lock과
 * filemap_invalidate_lock을 잡으므로 동일 inode에 대한 다른 truncate/fault
 * 경로와 상호 배제된다.
 * 호출자: blkdev_common_ioctl() (BLKDISCARD case).
 * 피호출자: copy_from_user(), bdev_max_discard_sectors(), bdev_read_only(),
 *          blk_validate_byte_range(), inode_lock(), filemap_invalidate_lock(),
 *          truncate_bdev_range(), blk_start_plug(), blk_alloc_discard_bio(),
 *          bio_chain_and_submit(), bio_submit_or_kill(), blk_finish_plug().
 *
 * 호출 체인:
 *   사용자 공간 ioctl(BLKDISCARD) → blkdev_ioctl → blkdev_common_ioctl →
 *   [blk_ioctl_discard] → blk_alloc_discard_bio(block/blk-lib.c) →
 *   bio_chain_and_submit(block/bio.c) → submit_bio → ... → nvme_queue_rq
 */
static int blk_ioctl_discard(struct block_device *bdev, blk_mode_t mode,
		unsigned long arg)
{
	uint64_t range[2], start, len; /* [한국어] range: 사용자 공간에서 복사할 {start,len} 원본 값; start/len: 바이트 단위로 분리해 담을 지역 변수 */
	struct bio *prev = NULL, *bio; /* [한국어] prev: 직전에 만든 bio(체인 anchor, 최초 NULL); bio: 이번 반복에서 새로 만든 discard bio */
	sector_t sector, nr_sects; /* [한국어] sector/nr_sects: 섹터 단위로 변환된 시작 위치와 남은 길이 - blk_alloc_discard_bio()가 in/out으로 갱신 */
	struct blk_plug plug; /* [한국어] 연속 제출되는 discard bio들을 배칭하기 위한 스택 로컬 plug 구조체 */
	int err; /* [한국어] 각 단계의 에러 코드를 담아 fail 레이블까지 전파할 변수 */

	if (copy_from_user(range, (void __user *)arg, sizeof(range))) /* [한국어] 사용자 공간의 range[2]={start,len}를 커널로 복사 */
		return -EFAULT; /* [한국어] 복사 실패 - 잘못된 arg 포인터 */
	start = range[0]; /* [한국어] range[0]을 시작 오프셋(바이트)으로 분리 */
	len = range[1]; /* [한국어] range[1]을 길이(바이트)로 분리 */

	if (!bdev_max_discard_sectors(bdev)) /* [한국어] 디바이스의 queue_limits에서 discard 최대 섹터 수를 조회 - 0이면 NVMe namespace가 Dataset Management(Deallocate)를 지원하지 않음 */
		return -EOPNOTSUPP; /* [한국어] discard 미지원 디바이스 - Operation not supported로 보고 */

	if (!(mode & BLK_OPEN_WRITE)) /* [한국어] 파일이 쓰기 가능 모드로 열리지 않았으면 discard(쓰기성 연산) 수행 불가 */
		return -EBADF; /* [한국어] Bad file descriptor - 쓰기 권한 없이 쓰기성 ioctl 요청 */
	if (bdev_read_only(bdev)) /* [한국어] block device 자체가 읽기전용(BD_READ_ONLY) 플래그로 설정돼 있는지 확인 */
		return -EPERM; /* [한국어] 읽기전용 디바이스에 discard 요청 - Permission denied */
	err = blk_validate_byte_range(bdev, start, len); /* [한국어] 정렬/길이/오버플로우/용량 초과 등 범위 유효성 검증 */
	if (err) /* [한국어] 검증 실패 여부 확인 */
		return err; /* [한국어] 범위가 유효하지 않음 - 즉시 반환 */

	inode_lock(bdev->bd_mapping->host); /* [한국어] inode 락 획득 - 이 디바이스의 페이지 캐시 truncate와 다른 VFS 경로(예: 동시 write/fault) 사이의 경쟁 방지 */
	filemap_invalidate_lock(bdev->bd_mapping); /* [한국어] address_space invalidate 락 획득 - 페이지 캐시 무효화 동안 새 페이지 유입(fault/write) 차단 */
	err = truncate_bdev_range(bdev, mode, start, start + len - 1); /* [한국어] discard 대상 범위[start, start+len-1]의 페이지 캐시를 무효화 - stale 데이터가 discard 후에도 캐시에 남지 않도록 함 */
	if (err) /* [한국어] truncate 실패 여부 확인 */
		goto fail; /* [한국어] 실패 시 잠금 해제 경로(fail 레이블)로 점프 */

	sector = start >> SECTOR_SHIFT; /* [한국어] 바이트 시작 오프셋을 섹터 단위로 변환 */
	nr_sects = len >> SECTOR_SHIFT; /* [한국어] 바이트 길이를 섹터 단위로 변환 - 이후 blk_alloc_discard_bio()가 이 값을 in/out으로 갱신하며 소모 */

	blk_start_plug(&plug); /* [한국어] 이후 제출되는 discard bio들을 하나의 plug로 묶어 NVMe SQ batching 효과를 노림(큐 진입/도어벨 오버헤드 절감) */
	while (!fatal_signal_pending(current)) { /* [한국어] 치명적 시그널(예: SIGKILL)이 대기 중이 아닌 동안 반복 - 시그널 도착 시 즉시 루프 중단 */
		bio = blk_alloc_discard_bio(bdev, &sector, &nr_sects, /* [한국어] discard_granularity 제한에 맞춰 다음 bio를 할당하고 sector/nr_sects를 전진시킴 */
				GFP_KERNEL);
		if (!bio) /* [한국어] 남은 길이가 0이 되어 더 만들 bio가 없으면 (bio==NULL) 루프 종료 */
			break; /* [한국어] 루프 탈출 */
		prev = bio_chain_and_submit(prev, bio); /* [한국어] 이전 anchor(prev)를 이번 bio에 체인하고 제출, 이번 bio를 다음 anchor로 갱신 */
	}
	if (prev) { /* [한국어] 최소 하나의 bio라도 만들어졌으면(prev != NULL) 마지막 조각을 제출/대기 */
		err = bio_submit_or_kill(prev, BLKDEV_ZERO_KILLABLE); /* [한국어] KILLABLE 플래그로 마지막 bio 제출 - 대기 중 치명적 시그널이 오면 중단 가능 */
		if (err == -EOPNOTSUPP) /* [한국어] 디바이스가 discard를 지원하지 않는다는 응답(EOPNOTSUPP)을 받았는지 확인 - 위에서 이미 지원 여부를 확인했지만 경합(race)으로 뒤늦게 바뀔 수 있음(추정) */
			err = 0; /* [한국어] EOPNOTSUPP는 이 함수 관점에서는 에러가 아님(전체 discard가 no-op으로 처리됨) - err를 0으로 정리 */
		bio_put(prev); /* [한국어] 참조 카운트 해제 - bio_submit_or_kill()이 완료를 기다린 뒤이므로 안전하게 반환 */
	}
	blk_finish_plug(&plug); /* [한국어] plug 해제 - 지금까지 쌓인 미제출 요청들을 마저 디스패치 */
fail: /* [한국어] fail 레이블: 에러 발생 시 여기로 점프해 잠금 해제 등 정리 후 반환 */
	filemap_invalidate_unlock(bdev->bd_mapping); /* [한국어] fail 레이블: invalidate 락 해제 - 에러 경로/정상 경로 공통 정리 */
	inode_unlock(bdev->bd_mapping->host); /* [한국어] inode 락 해제 */
	return err; /* [한국어] 최종 에러 코드(0 또는 truncate/submit 단계에서의 실패) 반환 */
}

/*
 * [한국어]
 * blk_ioctl_secure_erase - BLKSECDISCARD ioctl 처리: 지정 범위 보안 삭제
 *
 * @bdev: 대상 블록 디바이스.
 * @mode: 파일이 열린 모드 - 쓰기 권한 확인에 사용.
 * @argp: 사용자 공간의 uint64_t range[2] = {start, len}(바이트 단위)을
 *      가리키는 포인터.
 * @return: 0이면 성공, 그 외 -EBADF/-EOPNOTSUPP/-EFAULT/-EINVAL 및
 *      truncate_bdev_range()/blkdev_issue_secure_erase()의 음수 errno.
 *
 * 일반 discard보다 강한 보장(복구 불가능하게 데이터 파기)을 요구하는 삭제
 * 명령이다. blk_ioctl_discard()와 유사한 흐름이되 (1) 섹터(511) 단위
 * 정렬만 검사하고(논리 블록 크기가 아님 - 원본 코드 그대로), (2) 실제 bio
 * 조립을 이 함수가 직접 하지 않고 block/blk-lib.c의
 * blkdev_issue_secure_erase()에 완전히 위임한다는 점이 다르다. 이 명령은
 * REQ_OP_SECURE_ERASE bio로 변환되며, 지원 드라이버에서는 하드웨어의 보안
 * 삭제 기능(예: ATA Security Erase, 일부 NVMe 구현의 Sanitize/Crypto
 * Erase 계열)에 매핑될 수 있다(구체적 매핑은 드라이버 종속, 추정).
 * 실행 컨텍스트: ioctl 시스템 호출의 프로세스 컨텍스트. inode/filemap 락을
 * 잡아 다른 VFS 경로와 상호 배제한다.
 * 호출자: blkdev_common_ioctl() (BLKSECDISCARD case).
 * 피호출자: bdev_max_secure_erase_sectors(), copy_from_user(),
 *          check_add_overflow(), inode_lock(), filemap_invalidate_lock(),
 *          truncate_bdev_range(), blkdev_issue_secure_erase().
 *
 * 호출 체인:
 *   사용자 공간 ioctl(BLKSECDISCARD) → blkdev_ioctl → blkdev_common_ioctl →
 *   [blk_ioctl_secure_erase] → blkdev_issue_secure_erase(block/blk-lib.c)
 */
static int blk_ioctl_secure_erase(struct block_device *bdev, blk_mode_t mode,
		void __user *argp)
{
	uint64_t start, len, end; /* [한국어] start/len: 바이트 단위 요청 범위; end: start+len-1(오버플로우 검사 겸 마지막 유효 바이트) */
	uint64_t range[2]; /* [한국어] 사용자 공간에서 복사할 {start,len} 원본 버퍼 */
	int err; /* [한국어] 각 단계의 에러 코드 */

	if (!(mode & BLK_OPEN_WRITE)) /* [한국어] 쓰기 가능 모드로 열리지 않았으면 보안 삭제(쓰기성 연산) 불가 */
		return -EBADF; /* [한국어] Bad file descriptor */
	if (!bdev_max_secure_erase_sectors(bdev)) /* [한국어] 디바이스가 secure erase를 지원하는지(queue_limits의 max_secure_erase_sectors) 확인 */
		return -EOPNOTSUPP; /* [한국어] 미지원 디바이스 - Operation not supported */
	if (copy_from_user(range, argp, sizeof(range))) /* [한국어] 사용자 공간의 range[2]={start,len}를 커널로 복사 */
		return -EFAULT; /* [한국어] 복사 실패 */

	start = range[0]; /* [한국어] range[0]을 시작 오프셋으로 분리 */
	len = range[1]; /* [한국어] range[1]을 길이로 분리 */
	if ((start & 511) || (len & 511)) /* [한국어] start 또는 len이 511(섹터 크기-1) 경계에 정렬되지 않았는지 검사 - 512바이트 섹터 정렬 요구 */
		return -EINVAL; /* [한국어] 정렬 위반 */
	if (check_add_overflow(start, len, &end) || /* [한국어] start+len 오버플로우 여부와 디바이스 전체 바이트 수 초과 여부를 함께 검사 */
	    end > bdev_nr_bytes(bdev)) /* [한국어] (조건식 계속) end > bdev_nr_bytes(bdev) */
		return -EINVAL; /* [한국어] 범위가 유효하지 않음 */

	inode_lock(bdev->bd_mapping->host); /* [한국어] inode 락 획득 - 캐시 무효화와 다른 VFS 경로의 경쟁 방지 */
	filemap_invalidate_lock(bdev->bd_mapping); /* [한국어] address_space invalidate 락 획득 */
	err = truncate_bdev_range(bdev, mode, start, end - 1); /* [한국어] 대상 범위[start, end-1]의 페이지 캐시 무효화(보안 삭제 후 stale 캐시 방지) */
	if (!err) /* [한국어] truncate 성공 시에만 이어서 실제 보안 삭제 발행 */
		err = blkdev_issue_secure_erase(bdev, start >> 9, len >> 9, /* [한국어] 섹터 단위(>>9)로 변환한 범위를 blk-lib.c의 secure-erase 헬퍼에 위임 - REQ_OP_SECURE_ERASE bio 생성/제출/대기까지 담당 */
						GFP_KERNEL); /* [한국어] (계속) GFP_KERNEL로 bio 할당 */
	filemap_invalidate_unlock(bdev->bd_mapping); /* [한국어] address_space invalidate 락 해제 */
	inode_unlock(bdev->bd_mapping->host); /* [한국어] inode 락 해제 */
	return err; /* [한국어] 최종 에러 코드 반환 */
}


/*
 * [한국어]
 * blk_ioctl_zeroout - BLKZEROOUT ioctl 처리: 지정 범위를 0으로 채움
 *
 * @bdev: 대상 블록 디바이스.
 * @mode: 파일이 열린 모드 - 쓰기 권한 확인에 사용.
 * @arg: 사용자 공간의 uint64_t range[2] = {start, len}(바이트 단위)을
 *      가리키는 포인터(unsigned long 캐스팅).
 * @return: 0이면 성공, 그 외 -EBADF/-EFAULT/-EINVAL 및
 *      truncate_bdev_range()/blkdev_issue_zeroout()의 음수 errno.
 *
 * discard(BLKDISCARD)는 "데이터가 더 이상 필요 없다"는 힌트에 불과해 읽으면
 * 어떤 값이 나올지 보장이 없는 반면, BLKZEROOUT은 "이 범위를 읽으면 반드시
 * 0이 나와야 한다"는 강한 보장을 요구한다. (1) start/len(511 정렬, 512바이트
 * 섹터 단위) 및 오버플로우/용량 검증 후, (2) dirty 페이지를 포함해 페이지
 * 캐시를 truncate_bdev_range()로 무효화하고, (3) block/blk-lib.c의
 * blkdev_issue_zeroout()에 BLKDEV_ZERO_NOUNMAP | BLKDEV_ZERO_KILLABLE
 * 플래그로 위임한다. NOUNMAP은 "0으로 채우되 discard/unmap으로 공간을
 * 반환하지는 말라"는 뜻으로, 디바이스가 하드웨어 Write Zeroes를 지원하면
 * (NVMe REQ_OP_WRITE_ZEROES) 이를 사용하고, 지원하지 않으면 blk-lib.c가
 * 실제 0으로 채운 페이지를 쓰는 소프트웨어 폴백 경로로 넘어간다.
 * 실행 컨텍스트: ioctl 시스템 호출의 프로세스 컨텍스트.
 * 호출자: blkdev_common_ioctl() (BLKZEROOUT case).
 * 피호출자: copy_from_user(), inode_lock(), filemap_invalidate_lock(),
 *          truncate_bdev_range(), blkdev_issue_zeroout().
 *
 * 호출 체인:
 *   사용자 공간 ioctl(BLKZEROOUT) → blkdev_ioctl → blkdev_common_ioctl →
 *   [blk_ioctl_zeroout] → blkdev_issue_zeroout(block/blk-lib.c) →
 *   __blkdev_issue_zeroout → (REQ_OP_WRITE_ZEROES 또는 zero-page write 폴백)
 */
static int blk_ioctl_zeroout(struct block_device *bdev, blk_mode_t mode,
		unsigned long arg)
{
	uint64_t range[2]; /* [한국어] 사용자 공간에서 복사할 {start,len} 원본 버퍼 */
	uint64_t start, end, len; /* [한국어] start/len: 바이트 단위 요청 범위; end: start+len-1(마지막 유효 바이트, 오버플로우 검사 겸용) */
	int err; /* [한국어] 에러 코드 저장 변수 */

	if (!(mode & BLK_OPEN_WRITE)) /* [한국어] 쓰기 가능 모드가 아니면 zero-out(쓰기성 연산) 불가 */
		return -EBADF; /* [한국어] Bad file descriptor */

	if (copy_from_user(range, (void __user *)arg, sizeof(range))) /* [한국어] 사용자 공간의 range[2]={start,len}를 커널로 복사 */
		return -EFAULT; /* [한국어] 복사 실패 */

	start = range[0]; /* [한국어] range[0]을 시작 오프셋으로 분리 */
	len = range[1]; /* [한국어] range[1]을 길이로 분리 */
	end = start + len - 1; /* [한국어] 마지막 유효 바이트 오프셋 계산(오버플로우 시 자연스럽게 랩어라운드 - 아래 end<start 검사가 이를 잡아냄) */

	if (start & 511) /* [한국어] 시작 오프셋이 512바이트(511 마스크) 경계에 정렬되지 않음 */
		return -EINVAL; /* [한국어] 정렬 위반 */
	if (len & 511) /* [한국어] 길이가 512바이트 경계에 정렬되지 않음 */
		return -EINVAL; /* [한국어] 정렬 위반 */
	if (end >= (uint64_t)bdev_nr_bytes(bdev)) /* [한국어] end가 디바이스 전체 바이트 수 이상이면 범위 초과 */
		return -EINVAL; /* [한국어] 범위 초과 */
	if (end < start) /* [한국어] end < start면 위의 뺄셈에서 오버플로우(랩어라운드)가 발생했다는 뜻 */
		return -EINVAL; /* [한국어] 오버플로우 - 잘못된 요청 */

	/* Invalidate the page cache, including dirty pages */
	/* [한국어] 위 원문 번역: dirty 페이지를 포함해 페이지 캐시를 무효화한다 */
	inode_lock(bdev->bd_mapping->host); /* [한국어] inode 락 획득 */
	filemap_invalidate_lock(bdev->bd_mapping); /* [한국어] address_space invalidate 락 획득 */
	err = truncate_bdev_range(bdev, mode, start, end); /* [한국어] 대상 범위[start,end]의 페이지 캐시 무효화(dirty 페이지 포함) - zero-out 후 stale/dirty 데이터가 남지 않도록 함 */
	if (err) /* [한국어] truncate 실패 여부 확인 */
		goto fail; /* [한국어] 실패 시 fail 레이블로 점프해 잠금만 풀고 반환 */

	err = blkdev_issue_zeroout(bdev, start >> 9, len >> 9, GFP_KERNEL, /* [한국어] 섹터 단위(>>9)로 변환한 범위를 blk-lib.c의 zero-out 헬퍼에 위임 */
				   BLKDEV_ZERO_NOUNMAP | BLKDEV_ZERO_KILLABLE); /* [한국어] NOUNMAP: 0으로 채우되 공간을 discard/unmap하지 않음(하드웨어 Write Zeroes 우선, 미지원 시 zero-page write로 폴백); KILLABLE: 대기 중 치명적 시그널 시 중단 가능 */

fail: /* [한국어] fail 레이블: 에러 발생 시 여기로 점프해 잠금 해제 등 정리 후 반환 */
	filemap_invalidate_unlock(bdev->bd_mapping); /* [한국어] fail 레이블: invalidate 락 해제(정상/에러 공통 정리) */
	inode_unlock(bdev->bd_mapping->host); /* [한국어] inode 락 해제 */
	return err; /* [한국어] 최종 에러 코드 반환 */
}

/*
 * [한국어]
 * put_ushort - unsigned short 값 하나를 사용자 공간 포인터에 기록
 *
 * @argp: 결과를 받을 사용자 공간 포인터.
 * @val:  기록할 unsigned short 값.
 * @return: put_user()의 반환값(성공 0, 실패 시 -EFAULT).
 *
 * BLKSECTGET/BLKROTATIONAL처럼 ioctl 결과가 16비트 값인 명령들이 공통으로
 * 사용하는 극히 얇은 래퍼. put_user()는 타입 크기에 맞춰 컴파일 타임에 안전한
 * 단일 워드 복사를 생성하므로, 자료형별로 이런 얇은 래퍼를 두어 blkdev_
 * common_ioctl()의 각 case에서 캐스팅 없이 바로 호출할 수 있게 한다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트. 재진입/동시성 이슈 없음(상태 없는
 * 순수 함수).
 * 호출자: blkdev_common_ioctl() (BLKSECTGET, BLKROTATIONAL 등).
 * 피호출자: put_user().
 *
 * 호출 체인:
 *   blkdev_common_ioctl → [put_ushort] → put_user
 */
static int put_ushort(unsigned short __user *argp, unsigned short val)
{
	return put_user(val, argp); /* [한국어] val을 argp(사용자 공간)에 기록 - put_user()가 접근 가능 여부를 검사 후 단일 워드 복사 수행 */
}

/*
 * [한국어]
 * put_int - int 값 하나를 사용자 공간 포인터에 기록
 *
 * @argp: 결과를 받을 사용자 공간 포인터.
 * @val:  기록할 int 값.
 * @return: put_user()의 반환값.
 *
 * BLKSSZGET/BLKALIGNOFF/BLKROGET/BLKBSZGET 등 32비트 부호 있는 정수 결과를
 * 반환하는 ioctl들이 공용으로 사용하는 래퍼.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트.
 * 호출자: blkdev_common_ioctl(), blkdev_ioctl(), compat_blkdev_ioctl().
 * 피호출자: put_user().
 *
 * 호출 체인:
 *   blkdev_common_ioctl / blkdev_ioctl → [put_int] → put_user
 */
static int put_int(int __user *argp, int val)
{
	return put_user(val, argp); /* [한국어] val을 argp에 기록 */
}

/*
 * [한국어]
 * put_uint - unsigned int 값 하나를 사용자 공간 포인터에 기록
 *
 * @argp: 결과를 받을 사용자 공간 포인터.
 * @val:  기록할 unsigned int 값.
 * @return: put_user()의 반환값.
 *
 * BLKPBSZGET/BLKIOMIN/BLKIOOPT/BLKGETZONESZ/BLKGETNRZONES 등 부호 없는
 * 32비트 결과를 반환하는 ioctl들이 공용으로 사용하는 래퍼.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트.
 * 호출자: blkdev_common_ioctl().
 * 피호출자: put_user().
 *
 * 호출 체인:
 *   blkdev_common_ioctl → [put_uint] → put_user
 */
static int put_uint(unsigned int __user *argp, unsigned int val)
{
	return put_user(val, argp); /* [한국어] val을 argp에 기록 */
}

/*
 * [한국어]
 * put_long - long 값 하나를 사용자 공간 포인터에 기록
 *
 * @argp: 결과를 받을 사용자 공간 포인터.
 * @val:  기록할 long 값.
 * @return: put_user()의 반환값.
 *
 * BLKRAGET/BLKFRAGET(readahead 페이지 수를 섹터 단위로 환산한 long 값)이
 * 사용하는 래퍼. compat 경로에서는 폭이 다른 compat_put_long()을 대신 사용한다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트.
 * 호출자: blkdev_ioctl() (BLKRAGET/BLKFRAGET case).
 * 피호출자: put_user().
 *
 * 호출 체인:
 *   blkdev_ioctl → [put_long] → put_user
 */
static int put_long(long __user *argp, long val)
{
	return put_user(val, argp); /* [한국어] val을 argp에 기록 */
}

/*
 * [한국어]
 * put_ulong - unsigned long 값 하나를 사용자 공간 포인터에 기록
 *
 * @argp: 결과를 받을 사용자 공간 포인터.
 * @val:  기록할 unsigned long 값.
 * @return: put_user()의 반환값.
 *
 * BLKGETSIZE(레거시, 섹터 수를 unsigned long으로 반환)가 사용하는 래퍼.
 * 32비트 아키텍처에서는 unsigned long 폭이 좁아 EFBIG로 걸러진 뒤에만
 * 호출된다(blkdev_ioctl()의 BLKGETSIZE case 참고).
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트.
 * 호출자: blkdev_ioctl() (BLKGETSIZE case).
 * 피호출자: put_user().
 *
 * 호출 체인:
 *   blkdev_ioctl → [put_ulong] → put_user
 */
static int put_ulong(unsigned long __user *argp, unsigned long val)
{
	return put_user(val, argp); /* [한국어] val을 argp에 기록 */
}

/*
 * [한국어]
 * put_u64 - u64 값 하나를 사용자 공간 포인터에 기록
 *
 * @argp: 결과를 받을 사용자 공간 포인터.
 * @val:  기록할 u64 값.
 * @return: put_user()의 반환값.
 *
 * BLKGETSIZE64(바이트 단위 전체 용량)와 BLKGETDISKSEQ(디스크 시퀀스 번호)처럼
 * 64비트 폭이 필요한 결과에 사용하는 래퍼. 32비트 아키텍처에서도 put_user()가
 * 64비트 원자적 복사를 보장하도록 처리한다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트.
 * 호출자: blkdev_ioctl(), blkdev_common_ioctl() (BLKGETSIZE64, BLKGETDISKSEQ 등).
 * 피호출자: put_user().
 *
 * 호출 체인:
 *   blkdev_ioctl / blkdev_common_ioctl → [put_u64] → put_user
 */
static int put_u64(u64 __user *argp, u64 val)
{
	return put_user(val, argp); /* [한국어] val을 argp에 기록 */
}

#ifdef CONFIG_COMPAT
/*
 * [한국어]
 * compat_put_long - long 값을 32비트 호환 long 포인터에 기록
 *
 * @argp: 결과를 받을 32비트 호환 사용자 공간 포인터(compat_long_t __user *).
 * @val:  기록할 (네이티브 폭의) long 값 - compat_long_t로 축소되어 기록됨.
 * @return: put_user()의 반환값.
 *
 * put_long()의 compat 버전. compat_blkdev_ioctl()의 BLKRAGET/BLKFRAGET
 * 처리에서, 32비트 사용자 공간이 기대하는 폭으로 값을 되돌려 준다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트(32비트 호환 태스크).
 * 호출자: compat_blkdev_ioctl() (BLKRAGET/BLKFRAGET case).
 * 피호출자: put_user().
 *
 * 호출 체인:
 *   compat_blkdev_ioctl → [compat_put_long] → put_user
 */
static int compat_put_long(compat_long_t __user *argp, long val)
{
	return put_user(val, argp); /* [한국어] val을 32비트 폭(compat_long_t)으로 argp에 기록 */
}

/*
 * [한국어]
 * compat_put_ulong - unsigned long 값을 32비트 호환 ulong 포인터에 기록
 *
 * @argp: 결과를 받을 32비트 호환 사용자 공간 포인터(compat_ulong_t __user *).
 * @val:  기록할 compat_ulong_t 값.
 * @return: put_user()의 반환값.
 *
 * put_ulong()의 compat 버전. compat_blkdev_ioctl()의 BLKGETSIZE(레거시 섹터
 * 수 조회) 처리에서 32비트 폭으로 값을 되돌려 준다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트(32비트 호환 태스크).
 * 호출자: compat_blkdev_ioctl() (BLKGETSIZE case).
 * 피호출자: put_user().
 *
 * 호출 체인:
 *   compat_blkdev_ioctl → [compat_put_ulong] → put_user
 */
static int compat_put_ulong(compat_ulong_t __user *argp, compat_ulong_t val)
{
	return put_user(val, argp); /* [한국어] val을 argp에 기록 */
}
#endif

#ifdef CONFIG_COMPAT
/*
 * This is the equivalent of compat_ptr_ioctl(), to be used by block
 * drivers that implement only commands that are completely compatible
 * between 32-bit and 64-bit user space
 */
/*
 * [한국어]
 * blkdev_compat_ptr_ioctl - "포인터만 compat_ptr() 변환하면 되는" 드라이버용
 *                           범용 compat_ioctl 어댑터
 *
 * @bdev: 대상 블록 디바이스.
 * @mode: 파일이 열린 모드.
 * @cmd:  ioctl 명령 번호(native와 동일하다고 가정).
 * @arg:  32비트 호환 태스크가 전달한 인자(포인터를 32비트 폭으로 표현한 값).
 * @return: disk->fops->ioctl()의 반환값, 또는 그런 콜백이 없으면
 *      -ENOIOCTLCMD(→ 상위에서 -ENOIOCTLCMD로 처리해 다른 폴백을 시도).
 *
 * 일부 저수준 블록 드라이버는 32비트/64비트 사용자 공간에서 완전히 동일한
 * ioctl 명령 집합과 자료구조 레이아웃을 사용한다(포인터 폭만 다를 뿐). 이런
 * 드라이버는 compat_ioctl 콜백을 직접 구현하는 대신 이 함수를 그대로 꽂아
 * 넣어, arg를 compat_ptr()로 확장한 뒤 동일한 disk->fops->ioctl()을
 * 재사용할 수 있게 해 준다(fs/ioctl.c의 compat_ptr_ioctl()과 동일한 발상을
 * block_device_operations에 적용한 버전).
 * 실행 컨텍스트: ioctl 시스템 호출의 프로세스 컨텍스트(32비트 호환 태스크).
 * 호출자: 각 블록 드라이버의 block_device_operations.compat_ioctl 필드에
 *         직접 등록되어, VFS/블록 계층이 compat_ioctl 경로에서 호출한다.
 * 피호출자: compat_ptr(), disk->fops->ioctl() (드라이버가 구현한 네이티브
 *          ioctl 콜백).
 *
 * 호출 체인:
 *   compat_blkdev_ioctl(또는 드라이버 자체 compat_ioctl 슬롯) →
 *   [blkdev_compat_ptr_ioctl] → disk->fops->ioctl
 */
int blkdev_compat_ptr_ioctl(struct block_device *bdev, blk_mode_t mode,
			unsigned cmd, unsigned long arg)
{
	struct gendisk *disk = bdev->bd_disk; /* [한국어] bdev가 속한 gendisk 획득 - fops->ioctl 콜백을 찾기 위함 */

	if (disk->fops->ioctl) /* [한국어] 드라이버가 네이티브 ioctl 콜백을 등록했는지 확인 */
		return disk->fops->ioctl(bdev, mode, cmd, /* [한국어] 등록돼 있으면 arg(32비트 폭 값)를 compat_ptr()로 64비트 커널 포인터로 확장해 그대로 호출 */
					 (unsigned long)compat_ptr(arg)); /* [한국어] (계속) mode/cmd는 그대로 전달 */

	return -ENOIOCTLCMD; /* [한국어] 콜백 미등록 - 처리할 수 없는 ioctl임을 상위에 알림(다른 폴백 시도 유도) */
}
EXPORT_SYMBOL(blkdev_compat_ptr_ioctl); /* [한국어] 이 심볼을 다른 드라이버 모듈(built-in 아닌 로더블 모듈)에서도 링크해 쓸 수 있도록 익스포트 */
#endif

/*
 * [한국어]
 * enum pr_direction - Persistent Reservation(PR) ioctl의 데이터 방향
 *
 * IOC_PR_* ioctl들이 디바이스 상태를 변경하는지(OUT) 아니면 조회만 하는지(IN)
 * 구분해, blkdev_pr_allowed()가 파일이 열린 모드(BLK_OPEN_READ/WRITE)에 따라
 * 필요한 최소 권한을 판정하는 데 사용한다.
 */
enum pr_direction {
	PR_IN,  /* read from device */ /* [한국어] PR_IN: 디바이스로부터 정보만 읽는 방향(IOC_PR_READ_KEYS, IOC_PR_READ_RESERVATION) - 읽기 오픈(BLK_OPEN_READ)만으로 허용 */
	PR_OUT, /* write to device */ /* [한국어] PR_OUT: 디바이스 예약 상태를 변경하는 방향(REGISTER/RESERVE/RELEASE/PREEMPT/CLEAR) - 쓰기 오픈(BLK_OPEN_WRITE)이 필요 */
};

/*
 * [한국어]
 * blkdev_pr_allowed - Persistent Reservation ioctl 실행 권한 판정
 *
 * @bdev: 대상 블록 디바이스.
 * @mode: 파일이 열린 모드(BLK_OPEN_READ/WRITE 등).
 * @dir:  이번 PR 명령의 방향(PR_IN=조회, PR_OUT=상태 변경).
 * @return: true면 허용, false면 -EPERM으로 이어짐(호출자가 변환).
 *
 * 파티션에는 PR 개념이 의미가 없으므로(예약은 항상 디스크 전체 단위) 무조건
 * 거부한다. 관리자(CAP_SYS_ADMIN)는 항상 허용한다. 비관리자는 PR_OUT(상태
 * 변경)에 대해서는 파일이 쓰기 가능으로 열려 있어야 하고, PR_IN(조회)에
 * 대해서는 읽기 가능으로만 열려 있어도 허용한다(조회는 디바이스 상태를
 * 바꾸지 않으므로).
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트. 순수 판정 함수로 락 불필요.
 * 호출자: blkdev_pr_register/reserve/release/preempt/clear/read_keys/
 *         read_reservation() 각 PR 계열 함수 진입부.
 * 피호출자: bdev_is_partition(), capable().
 *
 * 호출 체인:
 *   blkdev_pr_* → [blkdev_pr_allowed]
 */
static bool blkdev_pr_allowed(struct block_device *bdev, blk_mode_t mode,
		enum pr_direction dir)
{
	/* no sense to make reservations for partitions */
	if (bdev_is_partition(bdev)) /* [한국어] bdev가 파티션이면(디스크 일부) PR 대상이 될 수 없음 */
		return false; /* [한국어] 파티션에 대한 PR 요청은 항상 거부 */

	if (capable(CAP_SYS_ADMIN)) /* [한국어] 호출 프로세스가 CAP_SYS_ADMIN을 가지면 방향에 관계없이 허용 */
		return true; /* [한국어] 관리자는 무조건 허용 */

	/*
	 * Only allow unprivileged reservation _out_ commands if the file
	 * descriptor is open for writing. Allow reservation _in_ commands if
	 * the file descriptor is open for reading since they do not modify the
	 * device.
	 */
	/* [한국어] 위 원문 번역: 파일 디스크립터가 쓰기용으로 열린 경우에만 비특권 예약 _out_ 명령을 허용한다. 예약 _in_ 명령은 디바이스를 변경하지 않으므로 읽기용으로 열린 경우에도 허용한다. */
	if (dir == PR_IN) /* [한국어] 조회(PR_IN) 방향이면 */
		return mode & BLK_OPEN_READ; /* [한국어] 읽기 오픈 여부만으로 허용 판정 */
	else /* [한국어] 그 외(PR_OUT, 상태 변경)이면 */
		return mode & BLK_OPEN_WRITE; /* [한국어] 쓰기 오픈 여부로 허용 판정 */
}

/*
 * [한국어]
 * blkdev_pr_register - IOC_PR_REGISTER 처리: PR 키 등록/해제
 *
 * @bdev: 대상 블록 디바이스.
 * @mode: 파일이 열린 모드 - blkdev_pr_allowed()의 권한 판정에 사용.
 * @arg:  사용자 공간의 struct pr_registration { old_key, new_key, flags } 포인터.
 * @return: 0 성공, -EPERM(권한 없음)/-EOPNOTSUPP(드라이버 미지원 또는 알 수
 *      없는 flags)/-EFAULT(복사 실패)/ops->pr_register()의 반환값.
 *
 * SCSI/NVMe류의 Persistent Reservation 프로토콜에서 "Register" 동작에
 * 대응한다 - old_key로 기존 등록을 확인하고 new_key로 교체(등록/해제/키
 * 변경)한다. 이 함수는 실제 프로토콜 처리를 하지 않고, bdev->bd_disk의
 * block_device_operations.pr_ops(드라이버가 채워 넣은 콜백 테이블)의
 * pr_register()로 그대로 위임하는 얇은 어댑터다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트.
 * 호출자: blkdev_common_ioctl() (IOC_PR_REGISTER case).
 * 피호출자: blkdev_pr_allowed(), copy_from_user(), ops->pr_register()
 *          (드라이버 구현, 예: NVMe면 Reservation Register 명령).
 *
 * 호출 체인:
 *   사용자 공간 ioctl(IOC_PR_REGISTER) → blkdev_ioctl → blkdev_common_ioctl →
 *   [blkdev_pr_register] → ops->pr_register (드라이버)
 */
static int blkdev_pr_register(struct block_device *bdev, blk_mode_t mode,
		struct pr_registration __user *arg)
{
	const struct pr_ops *ops = bdev->bd_disk->fops->pr_ops; /* [한국어] 드라이버가 등록한 PR 콜백 테이블 획득 (없으면 NULL) */
	struct pr_registration reg; /* [한국어] 사용자 공간에서 복사해 올 등록 요청(old_key/new_key/flags) */

	if (!blkdev_pr_allowed(bdev, mode, PR_OUT)) /* [한국어] PR_OUT(상태 변경) 방향으로 권한 검사 */
		return -EPERM; /* [한국어] 권한 없음 */
	if (!ops || !ops->pr_register) /* [한국어] 드라이버가 pr_ops 자체를 등록하지 않았거나 pr_register 콜백이 없으면 */
		return -EOPNOTSUPP; /* [한국어] 이 디바이스는 PR Register를 지원하지 않음 */
	if (copy_from_user(&reg, arg, sizeof(reg))) /* [한국어] 사용자 공간의 pr_registration 구조체를 커널로 복사 */
		return -EFAULT; /* [한국어] 복사 실패 */

	if (reg.flags & ~PR_FL_IGNORE_KEY) /* [한국어] PR_FL_IGNORE_KEY 이외의 알 수 없는 플래그 비트가 설정돼 있으면 */
		return -EOPNOTSUPP; /* [한국어] 알 수 없는 플래그는 지원하지 않음 */
	return ops->pr_register(bdev, reg.old_key, reg.new_key, reg.flags); /* [한국어] 드라이버의 pr_register 콜백으로 위임 - NVMe라면 Reservation Register 명령에 대응(추정) */
}

/*
 * [한국어]
 * blkdev_pr_reserve - IOC_PR_RESERVE 처리: PR 예약 획득
 *
 * @bdev: 대상 블록 디바이스.
 * @mode: 파일이 열린 모드.
 * @arg:  사용자 공간의 struct pr_reservation { key, type, flags } 포인터.
 * @return: 0 성공, -EPERM/-EOPNOTSUPP/-EFAULT/ops->pr_reserve()의 반환값.
 *
 * 등록된 key로 지정한 type(PR_WRITE_EXCLUSIVE 등)의 예약을 획득하는 PR
 * Reserve/Acquire 동작. blkdev_pr_register()와 동일한 패턴으로 드라이버의
 * pr_ops->pr_reserve()에 그대로 위임한다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트.
 * 호출자: blkdev_common_ioctl() (IOC_PR_RESERVE case).
 * 피호출자: blkdev_pr_allowed(), copy_from_user(), ops->pr_reserve()
 *          (드라이버 구현, 예: NVMe Reservation Acquire).
 *
 * 호출 체인:
 *   사용자 공간 ioctl(IOC_PR_RESERVE) → blkdev_ioctl → blkdev_common_ioctl →
 *   [blkdev_pr_reserve] → ops->pr_reserve (드라이버)
 */
static int blkdev_pr_reserve(struct block_device *bdev, blk_mode_t mode,
		struct pr_reservation __user *arg)
{
	const struct pr_ops *ops = bdev->bd_disk->fops->pr_ops; /* [한국어] 드라이버 PR 콜백 테이블 획득 */
	struct pr_reservation rsv; /* [한국어] 사용자 공간에서 복사해 올 예약 요청(key/type/flags) */

	if (!blkdev_pr_allowed(bdev, mode, PR_OUT)) /* [한국어] PR_OUT 방향 권한 검사 */
		return -EPERM; /* [한국어] 권한 없음 */
	if (!ops || !ops->pr_reserve) /* [한국어] pr_reserve 콜백 미지원 확인 */
		return -EOPNOTSUPP; /* [한국어] 미지원 */
	if (copy_from_user(&rsv, arg, sizeof(rsv))) /* [한국어] 사용자 공간 구조체 복사 */
		return -EFAULT; /* [한국어] 복사 실패 */

	if (rsv.flags & ~PR_FL_IGNORE_KEY) /* [한국어] PR_FL_IGNORE_KEY 이외의 알 수 없는 플래그 검사 */
		return -EOPNOTSUPP; /* [한국어] 미지원 플래그 */
	return ops->pr_reserve(bdev, rsv.key, rsv.type, rsv.flags); /* [한국어] 드라이버의 pr_reserve 콜백으로 위임 */
}

/*
 * [한국어]
 * blkdev_pr_release - IOC_PR_RELEASE 처리: 보유 중인 PR 예약 해제
 *
 * @bdev: 대상 블록 디바이스.
 * @mode: 파일이 열린 모드.
 * @arg:  사용자 공간의 struct pr_reservation { key, type, flags } 포인터.
 * @return: 0 성공, -EPERM/-EOPNOTSUPP/-EFAULT/ops->pr_release()의 반환값.
 *
 * 자신이 보유한 예약을 명시적으로 반납하는 PR Release 동작. flags는 반드시
 * 0이어야 하며(무시 가능한 플래그조차 없음), 그 외에는 register/reserve와
 * 동일한 패턴으로 드라이버에 위임한다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트.
 * 호출자: blkdev_common_ioctl() (IOC_PR_RELEASE case).
 * 피호출자: blkdev_pr_allowed(), copy_from_user(), ops->pr_release()
 *          (드라이버 구현, 예: NVMe Reservation Release).
 *
 * 호출 체인:
 *   사용자 공간 ioctl(IOC_PR_RELEASE) → blkdev_ioctl → blkdev_common_ioctl →
 *   [blkdev_pr_release] → ops->pr_release (드라이버)
 */
static int blkdev_pr_release(struct block_device *bdev, blk_mode_t mode,
		struct pr_reservation __user *arg)
{
	const struct pr_ops *ops = bdev->bd_disk->fops->pr_ops; /* [한국어] 드라이버 PR 콜백 테이블 획득 */
	struct pr_reservation rsv; /* [한국어] 사용자 공간에서 복사해 올 해제 요청(key/type) */

	if (!blkdev_pr_allowed(bdev, mode, PR_OUT)) /* [한국어] PR_OUT 방향 권한 검사 */
		return -EPERM; /* [한국어] 권한 없음 */
	if (!ops || !ops->pr_release) /* [한국어] pr_release 콜백 미지원 확인 */
		return -EOPNOTSUPP; /* [한국어] 미지원 */
	if (copy_from_user(&rsv, arg, sizeof(rsv))) /* [한국어] 사용자 공간 구조체 복사 */
		return -EFAULT; /* [한국어] 복사 실패 */

	if (rsv.flags) /* [한국어] Release는 어떤 플래그도 허용하지 않음(IGNORE_KEY조차) - 하나라도 설정돼 있으면 */
		return -EOPNOTSUPP; /* [한국어] 미지원 플래그 */
	return ops->pr_release(bdev, rsv.key, rsv.type); /* [한국어] 드라이버의 pr_release 콜백으로 위임 */
}

/*
 * [한국어]
 * blkdev_pr_preempt - IOC_PR_PREEMPT / IOC_PR_PREEMPT_ABORT 처리: 다른
 *                     예약자를 선점(preempt)하거나 그 등록을 강제 중단(abort)
 *
 * @bdev: 대상 블록 디바이스.
 * @mode: 파일이 열린 모드.
 * @arg:  사용자 공간의 struct pr_preempt { old_key, new_key, type, flags } 포인터.
 * @abort: true면 PREEMPT_ABORT(선점 대상의 미완료 명령까지 중단), false면
 *      일반 PREEMPT(예약만 넘겨받음).
 * @return: 0 성공, -EPERM/-EOPNOTSUPP/-EFAULT/ops->pr_preempt()의 반환값.
 *
 * blkdev_common_ioctl()이 IOC_PR_PREEMPT와 IOC_PR_PREEMPT_ABORT 두 ioctl
 * 명령을 abort 인자만 다르게 하여 이 함수 하나로 처리한다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트.
 * 호출자: blkdev_common_ioctl() (IOC_PR_PREEMPT, IOC_PR_PREEMPT_ABORT case).
 * 피호출자: blkdev_pr_allowed(), copy_from_user(), ops->pr_preempt()
 *          (드라이버 구현, 예: NVMe Reservation Acquire의 Preempt 액션).
 *
 * 호출 체인:
 *   사용자 공간 ioctl(IOC_PR_PREEMPT[_ABORT]) → blkdev_ioctl →
 *   blkdev_common_ioctl → [blkdev_pr_preempt] → ops->pr_preempt (드라이버)
 */
static int blkdev_pr_preempt(struct block_device *bdev, blk_mode_t mode,
		struct pr_preempt __user *arg, bool abort)
{
	const struct pr_ops *ops = bdev->bd_disk->fops->pr_ops; /* [한국어] 드라이버 PR 콜백 테이블 획득 */
	struct pr_preempt p; /* [한국어] 사용자 공간에서 복사해 올 선점 요청(old_key/new_key/type/flags) */

	if (!blkdev_pr_allowed(bdev, mode, PR_OUT)) /* [한국어] PR_OUT 방향 권한 검사 */
		return -EPERM; /* [한국어] 권한 없음 */
	if (!ops || !ops->pr_preempt) /* [한국어] pr_preempt 콜백 미지원 확인 */
		return -EOPNOTSUPP; /* [한국어] 미지원 */
	if (copy_from_user(&p, arg, sizeof(p))) /* [한국어] 사용자 공간 구조체 복사 */
		return -EFAULT; /* [한국어] 복사 실패 */

	if (p.flags) /* [한국어] Preempt는 어떤 플래그도 허용하지 않음 - 설정돼 있으면 */
		return -EOPNOTSUPP; /* [한국어] 미지원 플래그 */
	return ops->pr_preempt(bdev, p.old_key, p.new_key, p.type, abort); /* [한국어] 드라이버의 pr_preempt 콜백으로 위임 - abort로 PREEMPT/PREEMPT_ABORT 구분 전달 */
}

/*
 * [한국어]
 * blkdev_pr_clear - IOC_PR_CLEAR 처리: 디바이스의 모든 PR 등록/예약 제거
 *
 * @bdev: 대상 블록 디바이스.
 * @mode: 파일이 열린 모드.
 * @arg:  사용자 공간의 struct pr_clear { key, flags } 포인터.
 * @return: 0 성공, -EPERM/-EOPNOTSUPP/-EFAULT/ops->pr_clear()의 반환값.
 *
 * 자신의 key가 유효한 등록임을 증명하는 대가로, 디바이스에 걸린 모든 PR
 * 등록/예약 상태를 초기화하는 강력한 관리 동작(PR Clear)이다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트.
 * 호출자: blkdev_common_ioctl() (IOC_PR_CLEAR case).
 * 피호출자: blkdev_pr_allowed(), copy_from_user(), ops->pr_clear()
 *          (드라이버 구현, 예: NVMe Reservation Release의 Clear 액션).
 *
 * 호출 체인:
 *   사용자 공간 ioctl(IOC_PR_CLEAR) → blkdev_ioctl → blkdev_common_ioctl →
 *   [blkdev_pr_clear] → ops->pr_clear (드라이버)
 */
static int blkdev_pr_clear(struct block_device *bdev, blk_mode_t mode,
		struct pr_clear __user *arg)
{
	const struct pr_ops *ops = bdev->bd_disk->fops->pr_ops; /* [한국어] 드라이버 PR 콜백 테이블 획득 */
	struct pr_clear c; /* [한국어] 사용자 공간에서 복사해 올 clear 요청(key/flags) */

	if (!blkdev_pr_allowed(bdev, mode, PR_OUT)) /* [한국어] PR_OUT 방향 권한 검사 */
		return -EPERM; /* [한국어] 권한 없음 */
	if (!ops || !ops->pr_clear) /* [한국어] pr_clear 콜백 미지원 확인 */
		return -EOPNOTSUPP; /* [한국어] 미지원 */
	if (copy_from_user(&c, arg, sizeof(c))) /* [한국어] 사용자 공간 구조체 복사 */
		return -EFAULT; /* [한국어] 복사 실패 */

	if (c.flags) /* [한국어] Clear는 어떤 플래그도 허용하지 않음 - 설정돼 있으면 */
		return -EOPNOTSUPP; /* [한국어] 미지원 플래그 */
	return ops->pr_clear(bdev, c.key); /* [한국어] 드라이버의 pr_clear 콜백으로 위임 */
}

/*
 * [한국어]
 * blkdev_pr_read_keys - IOC_PR_READ_KEYS 처리: 등록된 PR 키 목록 조회
 *
 * @bdev: 대상 블록 디바이스.
 * @mode: 파일이 열린 모드 - PR_IN 방향 권한 검사에 사용.
 * @arg:  사용자 공간의 struct pr_read_keys { generation, num_keys, keys_ptr } 포인터.
 *      입력 시 num_keys는 사용자가 제공한 keys_ptr 배열의 용량(원소 수)을
 *      의미하고, 출력 시에는 디바이스가 실제로 가진 키의 총 개수로 갱신된다.
 * @return: 0 성공, -EPERM/-EOPNOTSUPP/-EFAULT/-EINVAL/-ENOMEM.
 *
 * 다른 PR 함수와 달리 가변 길이 결과(키 배열)를 다루므로 흐름이 더 복잡하다.
 * (1) 사용자가 요청한 num_keys가 PR_KEYS_MAX를 넘지 않는지 검사한다(과도한
 * 커널 메모리 할당 방지). (2) struct_size()로 pr_keys{generation,num_keys,
 * keys[]} 가변 배열의 정확한 바이트 크기를 계산해 kvzalloc()으로 할당한다
 * (vmalloc 폴백 가능한 kvzalloc이므로 큰 num_keys도 안전하게 처리). (3)
 * 드라이버의 pr_read_keys() 콜백을 호출한다 - 드라이버는 keys_info->num_keys
 * 개까지 채우고, 실제 보유한 키가 더 많으면 num_keys를 "총 개수"로 갱신해
 * 호출자가 더 큰 버퍼로 재시도하게 한다(주석 원문 참고). (4) 실제 채워진
 * 키(min(요청 개수, 실제 개수))만 사용자 keys_ptr로 copy_to_user 한다. (5)
 * generation/num_keys를 갱신한 pr_read_keys 구조체 전체를 다시 사용자
 * 공간으로 되돌린다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트. kvzalloc()이 필요 시 sleep할 수
 * 있으므로 인터럽트 컨텍스트에서는 호출 불가.
 * 호출자: blkdev_common_ioctl() (IOC_PR_READ_KEYS case).
 * 피호출자: blkdev_pr_allowed(), copy_from_user(), struct_size(), kvzalloc(),
 *          ops->pr_read_keys()(드라이버, 예: NVMe Reservation Report),
 *          u64_to_user_ptr(), copy_to_user(), kvfree().
 *
 * 호출 체인:
 *   사용자 공간 ioctl(IOC_PR_READ_KEYS) → blkdev_ioctl → blkdev_common_ioctl →
 *   [blkdev_pr_read_keys] → ops->pr_read_keys (드라이버)
 */
static int blkdev_pr_read_keys(struct block_device *bdev, blk_mode_t mode,
		struct pr_read_keys __user *arg)
{
	const struct pr_ops *ops = bdev->bd_disk->fops->pr_ops; /* [한국어] 드라이버 PR 콜백 테이블 획득 */
	struct pr_keys *keys_info; /* [한국어] 드라이버가 채울 가변 길이 키 배열 컨테이너 - kvzalloc으로 할당할 커널 버퍼 */
	struct pr_read_keys read_keys; /* [한국어] 사용자 공간에서 복사해 올 요청(generation/num_keys/keys_ptr) */
	u64 __user *keys_ptr; /* [한국어] 사용자 공간의 키 배열(u64[])을 가리키는 포인터 - keys_ptr을 변환해 저장 */
	size_t keys_info_len; /* [한국어] kvzalloc으로 할당할 keys_info의 전체 바이트 크기(가변 배열 포함) */
	size_t keys_copy_len; /* [한국어] 실제로 사용자 공간에 복사할 바이트 수(요청 개수와 실제 개수 중 작은 쪽 기준) */
	int ret; /* [한국어] 각 단계의 에러 코드 */

	if (!blkdev_pr_allowed(bdev, mode, PR_IN)) /* [한국어] PR_IN(조회) 방향 권한 검사 */
		return -EPERM; /* [한국어] 권한 없음 */
	if (!ops || !ops->pr_read_keys) /* [한국어] pr_read_keys 콜백 미지원 확인 */
		return -EOPNOTSUPP; /* [한국어] 미지원 */

	if (copy_from_user(&read_keys, arg, sizeof(read_keys))) /* [한국어] 사용자 공간의 pr_read_keys 구조체(generation/num_keys/keys_ptr)를 커널로 복사 */
		return -EFAULT; /* [한국어] 복사 실패 */

	if (read_keys.num_keys > PR_KEYS_MAX) /* [한국어] 사용자가 요청한 키 개수가 허용 상한(PR_KEYS_MAX=65536)을 넘는지 검사 - 과도한 커널 메모리 할당 방지 */
		return -EINVAL; /* [한국어] 범위 초과 */

	keys_info_len = struct_size(keys_info, keys, read_keys.num_keys); /* [한국어] pr_keys{generation,num_keys,keys[num_keys]} 가변 길이 구조체의 정확한 바이트 크기 계산 */

	keys_info = kvzalloc(keys_info_len, GFP_KERNEL); /* [한국어] 계산된 크기만큼 0으로 초기화된 커널 메모리 할당(vmalloc 폴백 가능한 kvzalloc - 큰 num_keys에도 안전) */
	if (!keys_info) /* [한국어] 할당 실패 확인 */
		return -ENOMEM; /* [한국어] 메모리 부족 */

	keys_info->num_keys = read_keys.num_keys; /* [한국어] 드라이버에게 "이 배열에 최대 몇 개까지 담을 수 있는지" 알려줌(드라이버 pr_read_keys의 입력 계약) */

	ret = ops->pr_read_keys(bdev, keys_info); /* [한국어] 드라이버의 pr_read_keys 콜백 호출 - NVMe라면 Reservation Report 명령으로 실제 키 목록을 채움(추정) */
	if (ret) /* [한국어] 드라이버 호출 실패 여부 확인 */
		goto out; /* [한국어] 실패 시 out 레이블(해제)로 점프 */

	/* Copy out individual keys */
	/* [한국어] 위 원문 번역: 개별 키들을 복사해 낸다 */
	keys_ptr = u64_to_user_ptr(read_keys.keys_ptr); /* [한국어] 사용자 공간 keys_ptr(u64 포인터 형태로 인코딩된 값)을 실제 __user 포인터로 변환 */
	keys_copy_len = min(read_keys.num_keys, keys_info->num_keys) * /* [한국어] 사용자가 요청한 개수와 드라이버가 실제로 채운 개수 중 작은 쪽을 기준으로 복사할 바이트 수 산정 */
		        sizeof(keys_info->keys[0]); /* [한국어] (계속) 키 하나의 크기(u64)를 곱해 바이트 수로 변환 */

	if (copy_to_user(keys_ptr, keys_info->keys, keys_copy_len)) { /* [한국어] 실제 키 배열(keys_info->keys)을 사용자 keys_ptr로 복사 */
		ret = -EFAULT; /* [한국어] 복사 실패 시 에러 코드 설정 */
		goto out; /* [한국어] out 레이블(해제)로 점프 */
	}
	/* [한국어] 위 원문 번역: arg 구조체 자체를 복사해 낸다 */

	/* Copy out the arg struct */
	read_keys.generation = keys_info->generation; /* [한국어] generation(예약 상태 변경 세대 번호)을 드라이버가 채운 값으로 갱신 */
	read_keys.num_keys = keys_info->num_keys; /* [한국어] num_keys를 드라이버가 보고한 "실제 총 키 개수"로 갱신 - 사용자 요청보다 많으면 호출자가 더 큰 버퍼로 재시도할 수 있게 함 */

	if (copy_to_user(arg, &read_keys, sizeof(read_keys))) /* [한국어] 갱신된 pr_read_keys 구조체 전체를 사용자 공간의 arg로 되돌림 */
		ret = -EFAULT; /* [한국어] 복사 실패 시 에러 코드 설정(단, 이미 키 배열은 복사됐을 수 있음) */
out: /* [한국어] out 레이블: 성공/실패 공통으로 임시 버퍼 해제 후 반환 */
	kvfree(keys_info); /* [한국어] out 레이블: 임시 커널 버퍼 해제(kvzalloc과 짝을 이루는 kvfree) - 모든 경로 공통 정리 */
	return ret; /* [한국어] 최종 결과 반환 */
}

/*
 * [한국어]
 * blkdev_pr_read_reservation - IOC_PR_READ_RESERVATION 처리: 현재 보유 중인
 *                              PR 예약 정보 조회
 *
 * @bdev: 대상 블록 디바이스.
 * @mode: 파일이 열린 모드 - PR_IN 방향 권한 검사에 사용.
 * @arg:  사용자 공간의 struct pr_read_reservation { key, generation, type } 포인터.
 * @return: 0 성공, -EPERM/-EOPNOTSUPP/ops->pr_read_reservation()의 반환값/-EFAULT.
 *
 * blkdev_pr_read_keys()와 달리 결과가 고정 크기(단일 예약 정보)이므로 훨씬
 * 단순하다 - 드라이버 콜백이 채운 pr_held_reservation을 사용자 공간
 * pr_read_reservation 레이아웃으로 필드별 복사한 뒤 그대로 반환한다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트.
 * 호출자: blkdev_common_ioctl() (IOC_PR_READ_RESERVATION case).
 * 피호출자: blkdev_pr_allowed(), ops->pr_read_reservation()(드라이버, 예:
 *          NVMe Reservation Report), copy_to_user().
 *
 * 호출 체인:
 *   사용자 공간 ioctl(IOC_PR_READ_RESERVATION) → blkdev_ioctl →
 *   blkdev_common_ioctl → [blkdev_pr_read_reservation] →
 *   ops->pr_read_reservation (드라이버)
 */
static int blkdev_pr_read_reservation(struct block_device *bdev,
		blk_mode_t mode, struct pr_read_reservation __user *arg)
{
	const struct pr_ops *ops = bdev->bd_disk->fops->pr_ops; /* [한국어] 드라이버 PR 콜백 테이블 획득 */
	struct pr_held_reservation rsv = {}; /* [한국어] 드라이버가 채울 커널 내부 표현(key/generation/type) - 0으로 초기화 */
	struct pr_read_reservation out = {}; /* [한국어] 사용자 공간으로 되돌릴 uAPI 표현(key/generation/type) - 0으로 초기화 */
	int ret; /* [한국어] 에러 코드 */

	if (!blkdev_pr_allowed(bdev, mode, PR_IN)) /* [한국어] PR_IN(조회) 방향 권한 검사 */
		return -EPERM; /* [한국어] 권한 없음 */
	if (!ops || !ops->pr_read_reservation) /* [한국어] pr_read_reservation 콜백 미지원 확인 */
		return -EOPNOTSUPP; /* [한국어] 미지원 */

	ret = ops->pr_read_reservation(bdev, &rsv); /* [한국어] 드라이버 콜백 호출 - 현재 보유 중인 예약 정보를 rsv에 채움 */
	if (ret) /* [한국어] 호출 실패 여부 확인 */
		return ret; /* [한국어] 실패 시 즉시 반환(예: 예약이 없는 경우 등, 드라이버 정의) */

	out.key = rsv.key; /* [한국어] 커널 내부 표현의 key를 uAPI 구조체로 복사 */
	out.generation = rsv.generation; /* [한국어] generation 필드 복사 */
	out.type = rsv.type; /* [한국어] type(예약 유형, enum pr_type) 필드 복사 */

	if (copy_to_user(arg, &out, sizeof(out))) /* [한국어] 완성된 pr_read_reservation 구조체를 사용자 공간 arg로 복사 */
		return -EFAULT; /* [한국어] 복사 실패 */
	return 0; /* [한국어] 성공 반환 */
}

/*
 * [한국어]
 * blkdev_flushbuf - BLKFLSBUF ioctl 처리: 버퍼 캐시 동기화 및 무효화
 *
 * @bdev: 대상 블록 디바이스.
 * @cmd:  ioctl 명령 번호(BLKFLSBUF) - 이 함수 내부에서는 실제로 쓰이지 않음.
 * @arg:  사용됨 없음(BLKFLSBUF는 인자를 받지 않음).
 * @return: 0(성공 고정) 또는 권한 없으면 -EACCES.
 *
 * "이 디바이스에 대해 커널이 들고 있는 모든 dirty 데이터를 디스크로 내려쓰고,
 * 클린 페이지 캐시도 모두 버려라"는 강한 동기화+무효화 명령이다. 디바이스에
 * bd_holder_ops->sync 콜백(예: 파일시스템이 마운트하며 등록한 자체 동기화
 * 루틴)이 있으면 그것을 우선 사용하고, 없으면 범용 sync_blockdev()로
 * 대체한다. 이후 invalidate_bdev()로 클린 페이지도 모두 버린다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트. bd_holder_lock으로 holder_ops
 * 필드 접근을 보호한다(다른 코드가 동시에 holder/holder_ops를 바꾸는 경우
 * 대비).
 * 호출자: blkdev_common_ioctl() (BLKFLSBUF case).
 * 피호출자: capable(), mutex_lock/unlock(&bdev->bd_holder_lock),
 *          bdev->bd_holder_ops->sync(), sync_blockdev(), invalidate_bdev().
 *
 * 호출 체인:
 *   사용자 공간 ioctl(BLKFLSBUF) → blkdev_ioctl → blkdev_common_ioctl →
 *   [blkdev_flushbuf] → sync_blockdev / bd_holder_ops->sync → invalidate_bdev
 */
static int blkdev_flushbuf(struct block_device *bdev, unsigned cmd,
		unsigned long arg)
{
	if (!capable(CAP_SYS_ADMIN)) /* [한국어] 관리자 권한 확인 - 강제 캐시 flush는 다른 프로세스/파일시스템에도 영향을 줄 수 있어 특권 필요 */
		return -EACCES; /* [한국어] 권한 없음 */

	mutex_lock(&bdev->bd_holder_lock); /* [한국어] bd_holder_ops 필드(및 아래에서 참조하는 sync 콜백) 접근을 보호하는 락 획득 */
	if (bdev->bd_holder_ops && bdev->bd_holder_ops->sync) /* [한국어] holder(예: 마운트한 파일시스템)가 자체 sync 콜백을 등록해 두었는지 확인 */
		bdev->bd_holder_ops->sync(bdev); /* [한국어] 등록돼 있으면 holder 고유의 동기화 루틴 사용(예: 파일시스템 자체 journal/commit 로직 반영) */
	else { /* [한국어] holder 콜백이 없으면 */
		mutex_unlock(&bdev->bd_holder_lock); /* [한국어] 먼저 락 해제 - sync_blockdev()는 이 락을 잡지 않아도 되는 범용 경로이므로 */
		sync_blockdev(bdev); /* [한국어] 범용 블록 디바이스 동기화: dirty 페이지를 디스크로 flush */
	}

	invalidate_bdev(bdev); /* [한국어] flush 이후 남은 클린 페이지 캐시까지 모두 무효화(버림) - 다음 접근 시 디스크에서 다시 읽도록 강제 */
	return 0; /* [한국어] 항상 성공 반환 */
}

/*
 * [한국어]
 * blkdev_roset - BLKROSET ioctl 처리: 블록 디바이스 읽기전용 상태 설정
 *
 * @bdev: 대상 블록 디바이스.
 * @cmd:  ioctl 명령 번호(BLKROSET) - 사용되지 않음.
 * @arg:  사용자 공간의 int(0=쓰기 허용, 0이 아니면 읽기전용)를 가리키는 포인터.
 * @return: 0 성공, -EACCES(권한 없음)/-EFAULT(복사 실패)/드라이버 콜백의
 *      반환값.
 *
 * 드라이버가 set_read_only() 콜백을 제공하면(예: 하드웨어 write-protect
 * 스위치가 있는 매체) 먼저 그것을 호출해 실제 하드웨어/드라이버 상태를
 * 바꾸고, 성공하면 이어서 block_device의 BD_READ_ONLY 소프트 플래그도
 * 동일하게 갱신한다. 드라이버 콜백이 없으면 소프트 플래그만 갱신한다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트.
 * 호출자: blkdev_common_ioctl() (BLKROSET case).
 * 피호출자: capable(), get_user(), disk->fops->set_read_only(),
 *          bdev_set_flag()/bdev_clear_flag().
 *
 * 호출 체인:
 *   사용자 공간 ioctl(BLKROSET) → blkdev_ioctl → blkdev_common_ioctl →
 *   [blkdev_roset] → disk->fops->set_read_only (드라이버, 있는 경우)
 */
static int blkdev_roset(struct block_device *bdev, unsigned cmd,
		unsigned long arg)
{
	int ret, n; /* [한국어] ret: 드라이버 콜백 반환값; n: 사용자가 요청한 읽기전용 여부(0/비0) */

	if (!capable(CAP_SYS_ADMIN)) /* [한국어] 관리자 권한 확인 - 디바이스 쓰기 가능 여부를 바꾸는 것은 특권 동작 */
		return -EACCES; /* [한국어] 권한 없음 */

	if (get_user(n, (int __user *)arg)) /* [한국어] 사용자 공간에서 요청 값(0=쓰기 허용, 비0=읽기전용)을 읽음 */
		return -EFAULT; /* [한국어] 읽기 실패 */
	if (bdev->bd_disk->fops->set_read_only) { /* [한국어] 드라이버가 자체 set_read_only 콜백을 등록했는지 확인(하드웨어 상태까지 바꿔야 하는 디바이스) */
		ret = bdev->bd_disk->fops->set_read_only(bdev, n); /* [한국어] 등록돼 있으면 드라이버 콜백 호출 - 하드웨어/펌웨어 수준의 읽기전용 상태 반영 */
		if (ret) /* [한국어] 드라이버 콜백 실패 여부 확인 */
			return ret; /* [한국어] 실패 시 소프트 플래그를 건드리지 않고 그대로 반환 */
	}
	if (n) /* [한국어] 요청 값이 0이 아니면(읽기전용 요청) */
		bdev_set_flag(bdev, BD_READ_ONLY); /* [한국어] block_device의 BD_READ_ONLY 소프트 플래그 설정 */
	else /* [한국어] 그 외(쓰기 허용 요청)면 */
		bdev_clear_flag(bdev, BD_READ_ONLY); /* [한국어] BD_READ_ONLY 플래그 해제 */
	return 0; /* [한국어] 성공 반환 */
}

/*
 * [한국어]
 * blkdev_getgeo - HDIO_GETGEO ioctl 처리: 레거시 CHS 지오메트리 조회
 *
 * @bdev: 대상 블록 디바이스.
 * @argp: 사용자 공간의 struct hd_geometry(heads/sectors/cylinders/start) 포인터.
 * @return: 0 성공, -EINVAL(argp NULL)/-ENOTTY(드라이버 미지원)/드라이버
 *      콜백의 반환값/-EFAULT(복사 실패).
 *
 * IDE/SCSI 시절 파티션 도구가 사용하던 CHS(Cylinder-Head-Sector) 지오메트리
 * 개념은 현대 저장장치(SSD, NVMe)에는 물리적으로 대응되지 않지만, 하위 호환을
 * 위해 여전히 조회 가능해야 한다. 드라이버의 fops->getgeo() 콜백이 대개
 * "그럴듯한" 더미 값(예: heads=255, sectors=63 등 관례적 값)을 채워 넣는다.
 * start 필드만은 이 함수가 먼저 get_start_sect()로 실제 파티션 시작 섹터를
 * 채워 넣는데, 이는 "드라이버가 원하면 이 값을 덮어써도 된다"는 관례를
 * 따르기 위함이다(원본 주석 참고).
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트.
 * 호출자: blkdev_ioctl() (HDIO_GETGEO case).
 * 피호출자: disk->fops->getgeo(), get_start_sect(), copy_to_user().
 *
 * 호출 체인:
 *   사용자 공간 ioctl(HDIO_GETGEO) → blkdev_ioctl → [blkdev_getgeo] →
 *   disk->fops->getgeo (드라이버)
 */
static int blkdev_getgeo(struct block_device *bdev,
		struct hd_geometry __user *argp)
{
	struct gendisk *disk = bdev->bd_disk; /* [한국어] 대상 디스크의 gendisk 획득 - fops->getgeo 콜백을 찾기 위함 */
	struct hd_geometry geo; /* [한국어] 드라이버가 채우고 사용자 공간으로 복사할 지오메트리 결과 구조체 */
	int ret; /* [한국어] 드라이버 콜백 반환값 */

	if (!argp) /* [한국어] 사용자 공간 포인터가 NULL이면 애초에 결과를 담을 곳이 없음 */
		return -EINVAL; /* [한국어] 잘못된 인자 */
	if (!disk->fops->getgeo) /* [한국어] 드라이버가 getgeo 콜백을 등록하지 않았는지 확인 */
		return -ENOTTY; /* [한국어] Inappropriate ioctl - 이 디바이스는 CHS 지오메트리 개념을 지원하지 않음 */

	/*
	 * We need to set the startsect first, the driver may
	 * want to override it.
	 */
	/* [한국어] 위 원문 번역: 시작 섹터를 먼저 설정해야 한다 - 드라이버가 이를 덮어쓰고 싶어할 수 있기 때문 */
	memset(&geo, 0, sizeof(geo)); /* [한국어] geo 구조체를 0으로 초기화 - 드라이버가 일부 필드만 채워도 나머지는 정의된 값(0)을 갖도록 함 */
	geo.start = get_start_sect(bdev); /* [한국어] 파티션(또는 디스크) 시작 섹터를 기본값으로 채움 - 드라이버가 필요하면 뒤에서 덮어씀 */
	ret = disk->fops->getgeo(disk, &geo); /* [한국어] 드라이버의 getgeo 콜백 호출 - heads/sectors/cylinders(및 필요시 start)를 채움 */
	if (ret) /* [한국어] 드라이버 콜백 실패 여부 확인 */
		return ret; /* [한국어] 실패 시 그대로 반환 */
	if (copy_to_user(argp, &geo, sizeof(geo))) /* [한국어] 완성된 hd_geometry 구조체를 사용자 공간으로 복사 */
		return -EFAULT; /* [한국어] 복사 실패 */
	return 0; /* [한국어] 성공 반환 */
}

#ifdef CONFIG_COMPAT
/*
 * [한국어]
 * struct compat_hd_geometry - 32비트 호환용 CHS 지오메트리 구조체
 *
 * struct hd_geometry(네이티브)와 논리적으로 동일한 필드를 갖지만, 32비트
 * 사용자 공간 ABI에 맞춰 정의되어 있다(주로 start 필드의 타입/정렬 차이).
 */
struct compat_hd_geometry {
/* [한국어] unsigned char heads;
 * 가상 헤드(head) 수. 실제 물리 헤드가 없는 SSD/NVMe에서는 관례적 더미 값.
 * 설정자: 드라이버의 fops->getgeo() 콜백.
 * 읽는 자: compat_hdio_getgeo()가 copy_to_user()로 통째 복사할 때 포함.
 * 값 범위: 드라이버 정의(전형적으로 255 등 legacy 관례값).
 * 동기화: 단일 호출 스택 지역 변수. */
	unsigned char heads;
/* [한국어] unsigned char sectors;
 * 가상 트랙당 섹터 수. 마찬가지로 legacy 호환용 더미 값인 경우가 대부분.
 * 설정자: 드라이버의 fops->getgeo() 콜백.
 * 읽는 자: copy_to_user()로 사용자 공간에 그대로 전달.
 * 값 범위: 드라이버 정의(전형적으로 63 등).
 * 동기화: 해당 없음. */
	unsigned char sectors;
/* [한국어] unsigned short cylinders;
 * 가상 실린더 수.
 * 설정자: 드라이버의 fops->getgeo() 콜백.
 * 읽는 자: copy_to_user()로 사용자 공간에 그대로 전달.
 * 값 범위: 드라이버 정의.
 * 동기화: 해당 없음. */
	unsigned short cylinders;
/* [한국어] u32 start;
 * 파티션(또는 디스크)의 시작 섹터. compat_hdio_getgeo()가 get_start_sect()로
 * 우선 채우며, 드라이버가 원하면 fops->getgeo() 내부에서 덮어쓸 수 있다.
 * 설정자: compat_hdio_getgeo() 기본값, 드라이버가 필요 시 재정의.
 * 읽는 자: compat_hdio_getgeo()가 put_user()로 별도 복사(4바이트 경계 문제로
 *   나머지 필드와 분리 복사됨 - 아래 함수 본문 참고).
 * 값 범위: sector_t 범위 내의 유효한 섹터 번호.
 * 동기화: 해당 없음. */
	u32 start;
};

/*
 * [한국어]
 * compat_hdio_getgeo - HDIO_GETGEO ioctl의 32비트 호환 처리
 *
 * @bdev: 대상 블록 디바이스.
 * @ugeo: 32비트 사용자 공간의 struct compat_hd_geometry 포인터.
 * @return: 0 성공, -EINVAL/-ENOTTY/드라이버 콜백 반환값/-EFAULT.
 *
 * blkdev_getgeo()의 compat 버전. 네이티브 hd_geometry로 드라이버 콜백을
 * 호출한 뒤, ABI 차이 때문에 앞의 3개 필드(heads/sectors/cylinders, 총
 * 4바이트)와 start 필드를 따로 복사한다(copy_to_user 4바이트 + put_user로
 * start 별도) - 이는 compat_hd_geometry의 구조체 패킹이 네이티브와 정확히
 * 일치하지 않을 수 있기 때문으로 보인다(추정).
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트(32비트 호환 태스크).
 * 호출자: compat_blkdev_ioctl() (HDIO_GETGEO case).
 * 피호출자: disk->fops->getgeo(), get_start_sect(), copy_to_user(), put_user().
 *
 * 호출 체인:
 *   사용자 공간 ioctl(HDIO_GETGEO, 32비트) → compat_blkdev_ioctl →
 *   [compat_hdio_getgeo] → disk->fops->getgeo (드라이버)
 */
static int compat_hdio_getgeo(struct block_device *bdev,
			      struct compat_hd_geometry __user *ugeo)
{
	struct gendisk *disk = bdev->bd_disk; /* [한국어] 대상 디스크의 gendisk 획득 */
	struct hd_geometry geo; /* [한국어] 드라이버 콜백에 전달할 네이티브 폭 hd_geometry 임시 구조체 */
	int ret; /* [한국어] 드라이버 콜백 반환값 */

	if (!ugeo) /* [한국어] 사용자 공간 포인터 NULL 검사 */
		return -EINVAL; /* [한국어] 잘못된 인자 */
	if (!disk->fops->getgeo) /* [한국어] 드라이버가 getgeo 콜백을 등록하지 않았는지 확인 */
		return -ENOTTY; /* [한국어] 이 디바이스는 CHS 지오메트리를 지원하지 않음 */

	memset(&geo, 0, sizeof(geo)); /* [한국어] geo 구조체를 0으로 초기화 */
	/*
	 * We need to set the startsect first, the driver may
	 * want to override it.
	 */
	/* [한국어] 위 원문 번역: 시작 섹터를 먼저 설정해야 한다 - 드라이버가 이를 덮어쓰고 싶어할 수 있기 때문 */
	geo.start = get_start_sect(bdev); /* [한국어] 파티션(또는 디스크) 시작 섹터 기본값 채움 */
	ret = disk->fops->getgeo(disk, &geo); /* [한국어] 드라이버의 getgeo 콜백 호출(네이티브 hd_geometry 사용) */
	if (ret) /* [한국어] 드라이버 콜백 실패 여부 확인 */
		return ret; /* [한국어] 실패 시 그대로 반환 */

	ret = copy_to_user(ugeo, &geo, 4); /* [한국어] heads(1)+sectors(1)+cylinders(2) = 4바이트만 우선 복사 - compat 구조체 레이아웃의 앞부분과 일치 */
	ret |= put_user(geo.start, &ugeo->start); /* [한국어] start 필드는 별도로 put_user()로 복사 - 두 결과를 OR하여 하나라도 실패하면 아래에서 -EFAULT 처리 */
	if (ret) /* [한국어] 위 두 복사 중 하나라도 실패했으면 */
		ret = -EFAULT; /* [한국어] 최종 에러를 -EFAULT로 통일 */

	return ret; /* [한국어] 성공(0) 또는 -EFAULT 반환 */
}
#endif

/* set the logical block size */
/*
 * [한국어]
 * blkdev_bszset - BLKBSZSET ioctl 처리: 논리 블록 크기(soft block size) 설정
 *
 * @file: BLKBSZSET을 호출한 열린 파일. 배타적으로 열려 있지 않다면 이 함수가
 *      내부적으로 별도의 배타적 fd를 새로 열어 사용한다.
 * @mode: file의 open 모드.
 * @argp: 사용자 공간의 int(새 블록 크기, 바이트 단위)를 가리키는 포인터.
 * @return: 0 성공, -EACCES/-EINVAL/-EFAULT/-EBUSY 또는 set_blocksize()의 반환값.
 *
 * VFS 계층의 "소프트" 블록 크기(버퍼드 I/O가 페이지 캐시를 다루는 단위)를
 * 바꾸는 명령이다. set_blocksize()는 대상 block_device가 배타적으로 열려
 * 있을 것을 요구하므로(다른 프로세스가 다른 크기로 동시에 열고 있으면 안
 * 되기 때문), 호출한 file 자체가 이미 BLK_OPEN_EXCL로 열려 있으면 그대로
 * 사용하고, 아니라면 bdev_file_open_by_dev()로 임시 배타적 fd를 새로 열어
 * set_blocksize()에 사용한 뒤 즉시 fput()으로 닫는다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트.
 * 호출자: blkdev_common_ioctl() (BLKBSZSET/BLKBSZSET_32 case).
 * 피호출자: capable(), get_user(), set_blocksize(), bdev_file_open_by_dev(), fput().
 *
 * 호출 체인:
 *   사용자 공간 ioctl(BLKBSZSET) → blkdev_ioctl/compat_blkdev_ioctl →
 *   blkdev_common_ioctl → [blkdev_bszset] → set_blocksize
 */
static int blkdev_bszset(struct file *file, blk_mode_t mode,
		int __user *argp)
{
	// this one might be file_inode(file)->i_rdev - a rare valid /* [한국어] 아래 두 줄 원문: 이것은 file_inode(file)->i_rdev일 수도 있다 - file_inode()의 드문 정당한 사용 사례 */
	// use of file_inode() for those. /* [한국어] (계속) 이런 경우에 한해서다 */
	dev_t dev = I_BDEV(file->f_mapping->host)->bd_dev; /* [한국어] file이 가리키는 block_device의 dev_t(장치 번호) 추출 - 배타적 재오픈 시 동일 디바이스를 찾기 위함 */
	struct file *excl_file; /* [한국어] BLK_OPEN_EXCL이 아니었을 경우 새로 열 임시 배타적 파일 핸들 */
	int ret, n; /* [한국어] ret: set_blocksize() 반환값; n: 사용자가 요청한 새 블록 크기 */

	if (!capable(CAP_SYS_ADMIN)) /* [한국어] 관리자 권한 확인 - 블록 크기 변경은 특권 동작 */
		return -EACCES; /* [한국어] 권한 없음 */
	if (!argp) /* [한국어] 사용자 포인터 NULL 검사 */
		return -EINVAL; /* [한국어] 잘못된 인자 */
	if (get_user(n, argp)) /* [한국어] 사용자 공간에서 새 블록 크기 값을 읽음 */
		return -EFAULT; /* [한국어] 읽기 실패 */

	if (mode & BLK_OPEN_EXCL) /* [한국어] 호출한 file이 이미 배타적(BLK_OPEN_EXCL)으로 열려 있으면 */
		return set_blocksize(file, n); /* [한국어] 그 file을 그대로 사용해 블록 크기 설정 - 별도 재오픈 불필요 */

	excl_file = bdev_file_open_by_dev(dev, mode, &dev, NULL); /* [한국어] 배타적이 아니었다면, 동일 디바이스를 배타 모드로 새로 오픈 시도 - 다른 열림과 동시 블록 크기 변경 방지 */
	if (IS_ERR(excl_file)) /* [한국어] 오픈 실패(이미 다른 배타적 사용자가 있는 등) 여부 확인 */
		return -EBUSY; /* [한국어] Device or resource busy로 보고 */
	ret = set_blocksize(excl_file, n); /* [한국어] 새로 연 배타적 file로 실제 블록 크기 설정 수행 */
	fput(excl_file); /* [한국어] 임시로 연 배타적 file 핸들 반환(참조 해제) - 설정이 끝났으므로 더 이상 필요 없음 */
	return ret; /* [한국어] set_blocksize()의 결과 반환 */
}

/*
 * Common commands that are handled the same way on native and compat
 * user space. Note the separate arg/argp parameters that are needed
 * to deal with the compat_ptr() conversion.
 */
/*
 * [한국어]
 * blkdev_common_ioctl - native/compat 공통 block ioctl 디스패처
 *
 * @bdev: 대상 블록 디바이스.
 * @mode: 파일이 열린 모드(BLK_OPEN_READ/WRITE 등) - 여러 하위 함수의 권한
 *      판정에 그대로 전달된다.
 * @cmd:  ioctl 명령 번호. blkdev_ioctl()/compat_blkdev_ioctl()이 자신의
 *      switch에서 처리하지 못한(자료구조가 native/compat 간에 동일한) 명령만
 *      이 함수로 넘어온다.
 * @arg:  원본 unsigned long 인자(포인터 값 포함) - compat 경로에서는 이미
 *      compat_ptr() 등으로 필요한 변환이 끝난 상태로 전달된다.
 * @argp: arg를 void __user *로 캐스팅한 값 - compat_ptr() 변환이 필요한
 *      cmd/arg 조합을 처리할 때는 이 값을 사용한다(주석 원문 참고).
 * @return: 각 case가 반환하는 값. 알 수 없는 cmd면 blk_get_meta_cap()의
 *      결과(대개 -ENOIOCTLCMD)를 그대로 반환해, 호출자가 driver의
 *      fops->ioctl/compat_ioctl(NVMe passthrough 등)로 넘기게 한다.
 *
 * 이 파일에서 가장 큰 디스패치 지점으로, discard/zeroout/secure-erase,
 * 디스크 시퀀스 번호, zone report/관리, 각종 queue_limits 조회(논리/물리
 * 블록 크기, io_min/io_opt, alignment offset, max sectors, rotational 여부),
 * readahead 설정, 파티션 재스캔, blktrace 제어, 인라인 암호화 키 관리,
 * Persistent Reservation 전체 집합을 하나의 switch로 모아 처리한다. 각
 * case는 대부분 이 파일의 다른 정적 함수 또는 block/blk.h의 비공개 헬퍼로
 * 즉시 위임하는 얇은 어댑터 역할만 한다.
 * 실행 컨텍스트: ioctl 시스템 호출의 프로세스 컨텍스트. 개별 case가 필요한
 * 락(inode_lock, bd_holder_lock 등)은 각 하위 함수 내부에서 잡는다.
 * 호출자: blkdev_ioctl(), compat_blkdev_ioctl() (자신이 처리하지 못한 cmd에
 *         한해).
 * 피호출자: 이 파일의 blkdev_flushbuf/blkdev_roset/blk_ioctl_discard/
 *          blk_ioctl_secure_erase/blk_ioctl_zeroout/put_* 계열/blkdev_pr_* 계열,
 *          block/blk.h의 blkdev_report_zones_ioctl/blkdev_zone_mgmt_ioctl/
 *          disk_scan_partitions, kernel blktrace의 blk_trace_ioctl,
 *          block/blk-crypto-internal.h의 blk_crypto_ioctl,
 *          block/blk-integrity.c의 blk_get_meta_cap(default case).
 *
 * 호출 체인:
 *   blkdev_ioctl / compat_blkdev_ioctl → [blkdev_common_ioctl] → (각 case별
 *   하위 함수) → ... → (NVMe 등 드라이버의 request_queue/bio 경로)
 */
static int blkdev_common_ioctl(struct block_device *bdev, blk_mode_t mode,
			       unsigned int cmd, unsigned long arg,
			       void __user *argp)
{
	unsigned int max_sectors; /* [한국어] BLKSECTGET 처리 시 계산에 쓸 지역 변수(USHRT_MAX와 queue_max_sectors 중 작은 값) */

	switch (cmd) { /* [한국어] ioctl 명령 번호로 분기 시작 */
	case BLKFLSBUF: /* [한국어] BLKFLSBUF: 버퍼 캐시 동기화+무효화 */
		return blkdev_flushbuf(bdev, cmd, arg); /* [한국어] blkdev_flushbuf()에 위임 */
	case BLKROSET: /* [한국어] BLKROSET: 읽기전용 상태 설정 */
		return blkdev_roset(bdev, cmd, arg); /* [한국어] blkdev_roset()에 위임 */
	case BLKDISCARD: /* [한국어] BLKDISCARD: TRIM/UNMAP 요청 */
		return blk_ioctl_discard(bdev, mode, arg); /* [한국어] blk_ioctl_discard()에 위임 - 내부에서 blk_alloc_discard_bio/bio_chain_and_submit 사용 */
	case BLKSECDISCARD: /* [한국어] BLKSECDISCARD: 보안 삭제 요청 */
		return blk_ioctl_secure_erase(bdev, mode, argp); /* [한국어] blk_ioctl_secure_erase()에 위임 - 내부에서 blkdev_issue_secure_erase 사용 */
	case BLKZEROOUT: /* [한국어] BLKZEROOUT: 영역 0채우기 요청 */
		return blk_ioctl_zeroout(bdev, mode, arg); /* [한국어] blk_ioctl_zeroout()에 위임 - 내부에서 blkdev_issue_zeroout 사용 */
	case BLKGETDISKSEQ: /* [한국어] BLKGETDISKSEQ: 디스크 고유 시퀀스 번호 조회(디스크 재부착 시에도 증가하는 카운터) */
		return put_u64(argp, bdev->bd_disk->diskseq); /* [한국어] gendisk에 이미 저장돼 있는 diskseq 값을 그대로 반환 */
	case BLKREPORTZONE: /* [한국어] BLKREPORTZONE(구버전)과 */
	case BLKREPORTZONEV2: /* [한국어] BLKREPORTZONEV2(신버전) 모두 동일 처리 - zone 상태 보고 요청 */
		return blkdev_report_zones_ioctl(bdev, cmd, arg); /* [한국어] blk.h의 zone 보고 헬퍼에 위임(SMR/ZNS 등 zoned 디바이스의 zone 상태 배열 반환) */
	case BLKRESETZONE: /* [한국어] BLKRESETZONE(zone을 empty로 리셋), */
	case BLKOPENZONE: /* [한국어] BLKOPENZONE(zone을 open 상태로), */
	case BLKCLOSEZONE: /* [한국어] BLKCLOSEZONE(zone을 closed 상태로), */
	case BLKFINISHZONE: /* [한국어] BLKFINISHZONE(zone을 full로 마감) 모두 */
		return blkdev_zone_mgmt_ioctl(bdev, mode, cmd, arg); /* [한국어] blk.h의 zone 관리 헬퍼로 공통 위임 - cmd 값 자체를 그대로 넘겨 세부 동작을 구분시킴 */
	case BLKGETZONESZ: /* [한국어] BLKGETZONESZ: zone 하나의 크기(섹터 수) 조회 */
		return put_uint(argp, bdev_zone_sectors(bdev)); /* [한국어] queue_limits에 저장된 zone_sectors 값을 반환 */
	case BLKGETNRZONES: /* [한국어] BLKGETNRZONES: 전체 zone 개수 조회 */
		return put_uint(argp, bdev_nr_zones(bdev)); /* [한국어] 디바이스의 zone 개수를 반환 */
	case BLKROGET: /* [한국어] BLKROGET: 현재 읽기전용 여부 조회 */
		return put_int(argp, bdev_read_only(bdev) != 0); /* [한국어] BD_READ_ONLY 플래그를 boolean(0/1)으로 정규화해 반환 */
	case BLKSSZGET: /* get block device logical block size */ /* [한국어] BLKSSZGET: 논리 블록 크기 조회 (NVMe Identify Namespace LBAF.LBADS에 대응) */
		return put_int(argp, bdev_logical_block_size(bdev)); /* [한국어] queue_limits의 logical_block_size 반환 */
	case BLKPBSZGET: /* get block device physical block size */ /* [한국어] BLKPBSZGET: 물리 블록 크기 조회 (하드웨어가 선호하는 실제 쓰기 단위) */
		return put_uint(argp, bdev_physical_block_size(bdev)); /* [한국어] queue_limits의 physical_block_size 반환 */
	case BLKIOMIN: /* [한국어] BLKIOMIN: 최소 효율적 I/O 크기 조회 */
		return put_uint(argp, bdev_io_min(bdev)); /* [한국어] queue_limits의 io_min 반환 */
	case BLKIOOPT: /* [한국어] BLKIOOPT: 최적 I/O 크기 조회 */
		return put_uint(argp, bdev_io_opt(bdev)); /* [한국어] queue_limits의 io_opt 반환 */
	case BLKALIGNOFF: /* [한국어] BLKALIGNOFF: 파티션/디스크의 정렬 오프셋 조회 */
		return put_int(argp, bdev_alignment_offset(bdev)); /* [한국어] queue_limits의 alignment_offset 반환 */
	case BLKDISCARDZEROES: /* [한국어] BLKDISCARDZEROES: 레거시 - discard 후 반드시 0이 보장되는지 조회(폐기 예정 API) */
		return put_uint(argp, 0); /* [한국어] 현재 커널은 항상 0(보장 없음)을 반환하도록 고정 - 실제 보장 여부는 BLKZEROOUT 등 별도 명령으로 판단해야 함 */
	case BLKSECTGET: /* [한국어] BLKSECTGET: 한 요청이 가질 수 있는 최대 섹터 수 조회 */
		max_sectors = min_t(unsigned int, USHRT_MAX, /* [한국어] USHRT_MAX(16비트 필드 한계)와 queue_max_sectors(디바이스/컨트롤러 한계, 예: NVMe MDTS) 중 작은 값 계산 */
				    queue_max_sectors(bdev_get_queue(bdev))); /* [한국어] (계속) request_queue의 max_sectors 조회 */
		return put_ushort(argp, max_sectors); /* [한국어] 16비트 폭으로 반환 */
	case BLKROTATIONAL: /* [한국어] BLKROTATIONAL: 회전형 매체 여부 조회 (NVMe/SSD는 항상 비회전) */
		return put_ushort(argp, bdev_rot(bdev)); /* [한국어] bdev_rot() 결과를 그대로 반환 */
	case BLKRASET: /* [한국어] BLKRASET(구버전 readahead 설정)과 */
	case BLKFRASET: /* [한국어] BLKFRASET(신버전) 모두 */
		if(!capable(CAP_SYS_ADMIN)) /* [한국어] 관리자 권한 확인 - readahead 정책 변경은 특권 동작 */
			return -EACCES; /* [한국어] 권한 없음 */
		bdev->bd_disk->bdi->ra_pages = (arg * 512) / PAGE_SIZE; /* [한국어] arg(요청한 섹터 수)를 바이트로 환산(*512) 후 PAGE_SIZE로 나눠 backing_dev_info의 ra_pages(페이지 단위 readahead 창 크기)에 직접 대입 */
		return 0; /* [한국어] 성공 반환 */
	case BLKRRPART: /* [한국어] BLKRRPART: 파티션 테이블 재스캔 요청 */
		if (!capable(CAP_SYS_ADMIN)) /* [한국어] 관리자 권한 확인 - 파티션 재스캔은 다른 프로세스의 파티션 접근에 영향 */
			return -EACCES; /* [한국어] 권한 없음 */
		if (bdev_is_partition(bdev)) /* [한국어] 대상이 이미 파티션(디스크 일부)이면 재스캔 대상이 될 수 없음 */
			return -EINVAL; /* [한국어] 잘못된 인자 */
		return disk_scan_partitions(bdev->bd_disk, /* [한국어] 디스크 전체에 대해 파티션 테이블 재스캔 수행 */
				mode | BLK_OPEN_STRICT_SCAN); /* [한국어] (계속) 엄격한 스캔 모드 플래그를 함께 전달 */
	case BLKTRACESTART: /* [한국어] BLKTRACESTART/STOP/TEARDOWN: blktrace 세션 제어 */
	case BLKTRACESTOP: /* [한국어] (BLKTRACESTOP도 동일 처리) */
	case BLKTRACETEARDOWN: /* [한국어] (BLKTRACETEARDOWN도 동일 처리) */
		return blk_trace_ioctl(bdev, cmd, argp); /* [한국어] blktrace 서브시스템의 공통 ioctl 처리 함수로 위임 - cmd 값으로 세부 동작 구분 */
	case BLKCRYPTOIMPORTKEY: /* [한국어] BLKCRYPTOIMPORTKEY/GENERATEKEY/PREPAREKEY: 인라인 암호화 키 관리 */
	case BLKCRYPTOGENERATEKEY: /* [한국어] (GENERATEKEY도 동일 처리) */
	case BLKCRYPTOPREPAREKEY: /* [한국어] (PREPAREKEY도 동일 처리) */
		return blk_crypto_ioctl(bdev, cmd, argp); /* [한국어] blk-crypto-internal.h의 공통 처리 함수로 위임 - cmd로 세부 동작 구분 */
	case IOC_PR_REGISTER: /* [한국어] IOC_PR_REGISTER: PR 키 등록 */
		return blkdev_pr_register(bdev, mode, argp); /* [한국어] blkdev_pr_register()에 위임 */
	case IOC_PR_RESERVE: /* [한국어] IOC_PR_RESERVE: PR 예약 획득 */
		return blkdev_pr_reserve(bdev, mode, argp); /* [한국어] blkdev_pr_reserve()에 위임 */
	case IOC_PR_RELEASE: /* [한국어] IOC_PR_RELEASE: PR 예약 해제 */
		return blkdev_pr_release(bdev, mode, argp); /* [한국어] blkdev_pr_release()에 위임 */
	case IOC_PR_PREEMPT: /* [한국어] IOC_PR_PREEMPT: PR 선점 */
		return blkdev_pr_preempt(bdev, mode, argp, false); /* [한국어] blkdev_pr_preempt()에 위임(abort=false) */
	case IOC_PR_PREEMPT_ABORT: /* [한국어] IOC_PR_PREEMPT_ABORT: PR 선점+강제중단 */
		return blkdev_pr_preempt(bdev, mode, argp, true); /* [한국어] blkdev_pr_preempt()에 위임(abort=true) */
	case IOC_PR_CLEAR: /* [한국어] IOC_PR_CLEAR: PR 전체 초기화 */
		return blkdev_pr_clear(bdev, mode, argp); /* [한국어] blkdev_pr_clear()에 위임 */
	case IOC_PR_READ_KEYS: /* [한국어] IOC_PR_READ_KEYS: 등록된 PR 키 목록 조회 */
		return blkdev_pr_read_keys(bdev, mode, argp); /* [한국어] blkdev_pr_read_keys()에 위임 */
	case IOC_PR_READ_RESERVATION: /* [한국어] IOC_PR_READ_RESERVATION: 현재 보유 예약 조회 */
		return blkdev_pr_read_reservation(bdev, mode, argp); /* [한국어] blkdev_pr_read_reservation()에 위임 */
	default: /* [한국어] 위에서 열거한 어떤 cmd와도 일치하지 않는 경우 */
		return blk_get_meta_cap(bdev, cmd, argp); /* [한국어] FS_IOC_GETLBMD_CAP(블록 무결성/PI 능력 조회) 여부를 blk_get_meta_cap()이 재확인 - 그것도 아니면 -ENOIOCTLCMD를 반환해 상위가 드라이버 fops->ioctl(NVMe passthrough 등)로 넘기게 함 */
	}
}

/*
 * Always keep this in sync with compat_blkdev_ioctl()
 * to handle all incompatible commands in both functions.
 *
 * New commands must be compatible and go into blkdev_common_ioctl
 */
/*
 * [한국어]
 * blkdev_ioctl - 블록 디바이스에 대한 메인(native 64비트) ioctl 진입점
 *
 * @file: ioctl(2)을 호출한 열린 파일(블록 특수 파일).
 * @cmd:  ioctl 명령 번호.
 * @arg:  명령별 인자(정수 값이거나 사용자 공간 포인터를 unsigned long으로
 *      표현한 값).
 * @return: 각 case/피호출 함수의 반환값. 이 파일이 처리하지 못하고
 *      드라이버도 처리하지 못하면 -ENOTTY(Inappropriate ioctl for device).
 *
 * VFS가 block_device_operations.ioctl 콜백으로 등록해 두는 최상위
 * 진입점이다. 먼저 (1) 자료구조가 커널 내부 전용이라 compat 버전과 함수
 * 자체를 분리해야 하는 명령(HDIO_GETGEO, BLKPG)을 처리하고, (2) "정수 폭은
 * 다르지만 명령 번호는 같은" BLKRAGET/BLKFRAGET/BLKGETSIZE를 처리하고,
 * (3) "데이터는 호환되지만 명령 번호 자체가 다른"(BLKBSZGET vs
 * BLKBSZGET_32 등) 명령을 처리하고, (4) i386에서 정렬이 다른
 * BLKTRACESETUP[2]을 처리한다. 이 네 그룹 어디에도 없으면
 * blkdev_common_ioctl()로 넘겨 native/compat 공통 처리를 시도한다. 그마저
 * -ENOIOCTLCMD를 반환하면 마지막으로 disk->fops->ioctl(드라이버가 구현한
 * vendor/passthrough ioctl, 예: NVMe ioctl passthrough)로 위임한다. 주석
 * 원문이 강조하듯, compat_blkdev_ioctl()과 항상 동기화되어야 한다 - 새
 * 명령은 원칙적으로 blkdev_common_ioctl()에 추가해야 한다.
 * 실행 컨텍스트: ioctl 시스템 호출의 프로세스 컨텍스트.
 * 호출자: VFS의 vfs_ioctl() (block_device_operations.ioctl 콜백을 통해).
 * 피호출자: blkdev_getgeo(), blkpg_ioctl(), put_long/put_ulong/put_int/
 *          put_u64(), blkdev_bszset(), blk_trace_ioctl(),
 *          blkdev_common_ioctl(), disk->fops->ioctl()(드라이버).
 *
 * 호출 체인:
 *   사용자 공간 ioctl(2) → VFS vfs_ioctl → [blkdev_ioctl] →
 *   (직접 처리 | blkdev_common_ioctl | disk->fops->ioctl)
 */
long blkdev_ioctl(struct file *file, unsigned cmd, unsigned long arg)
{
	struct block_device *bdev = I_BDEV(file->f_mapping->host); /* [한국어] file이 가리키는 inode에서 block_device 획득 - I_BDEV는 block special inode의 i_bdev 필드를 꺼내는 헬퍼(추정) */
	void __user *argp = (void __user *)arg; /* [한국어] arg를 사용자 공간 포인터로 재해석(정수 인자 명령에서는 사용되지 않음) */
	blk_mode_t mode = file_to_blk_mode(file); /* [한국어] file의 open 플래그(O_RDONLY/O_WRONLY/O_EXCL 등)를 blk_mode_t(BLK_OPEN_*) 조합으로 변환 */
	int ret; /* [한국어] blkdev_common_ioctl() 등의 반환값을 담을 변수 */

	switch (cmd) { /* [한국어] ioctl 명령 번호로 분기 시작 */
	/* These need separate implementations for the data structure */
	/* [한국어] 위 원문 번역: 자료구조가 다른 명령들은 별도 구현이 필요하다 */
	case HDIO_GETGEO: /* [한국어] HDIO_GETGEO: 레거시 CHS 지오메트리 조회 */
		return blkdev_getgeo(bdev, argp); /* [한국어] blkdev_getgeo()에 위임 */
	case BLKPG: /* [한국어] BLKPG: 파티션 추가/삭제/크기조정 */
		return blkpg_ioctl(bdev, argp); /* [한국어] blkpg_ioctl()에 위임 */

	/* Compat mode returns 32-bit data instead of 'long' */
	/* [한국어] 위 원문 번역: compat 모드는 'long' 대신 32비트 데이터를 반환한다 */
	case BLKRAGET: /* [한국어] BLKRAGET(구버전 readahead 조회)와 */
	case BLKFRAGET: /* [한국어] BLKFRAGET(신버전) 모두 동일 처리 */
		if (!argp) /* [한국어] 사용자 결과 포인터가 NULL이면 애초에 반환할 곳이 없음 */
			return -EINVAL; /* [한국어] 잘못된 인자 */
		return put_long(argp, /* [한국어] ra_pages(페이지 단위)를 바이트로 환산 후 512로 나눠 섹터 단위 값으로 변환해 반환 */
			(bdev->bd_disk->bdi->ra_pages * PAGE_SIZE) / 512); /* [한국어] (계속) 계산식의 나머지 부분 */
	case BLKGETSIZE: /* [한국어] BLKGETSIZE: 레거시 방식의 전체 섹터 수 조회(unsigned long 폭) */
		if (bdev_nr_sectors(bdev) > ~0UL) /* [한국어] 섹터 수가 unsigned long(현재 아키텍처의 폭) 범위를 넘는지 확인 - 32비트 커널에서 대용량 디스크일 때 발생 가능 */
			return -EFBIG; /* [한국어] File too large - 값이 표현 범위를 초과 */
		return put_ulong(argp, bdev_nr_sectors(bdev)); /* [한국어] 전체 섹터 수를 unsigned long 폭으로 반환 */

	/* The data is compatible, but the command number is different */
	/* [한국어] 위 원문 번역: 데이터는 호환되지만 명령 번호 자체가 다르다 */
	case BLKBSZGET: /* get block device soft block size (cf. BLKSSZGET) */ /* [한국어] BLKBSZGET: soft block size(버퍼드 I/O 단위) 조회 - BLKSSZGET(logical block size)과는 다른 개념이라는 점에 주의 */
		return put_int(argp, block_size(bdev)); /* [한국어] VFS의 block_size() 헬퍼로 현재 설정된 soft block size 반환 */
	case BLKBSZSET: /* [한국어] BLKBSZSET: soft block size 설정 */
		return blkdev_bszset(file, mode, argp); /* [한국어] blkdev_bszset()에 위임 */
	case BLKGETSIZE64: /* [한국어] BLKGETSIZE64: 전체 용량을 바이트 단위(64비트)로 조회 - 32/64비트 어디서나 안전 */
		return put_u64(argp, bdev_nr_bytes(bdev)); /* [한국어] queue_limits 기반 bdev_nr_bytes()를 u64로 반환 */

	/* Incompatible alignment on i386 */
	/* [한국어] 위 원문 번역: i386에서는 정렬 방식이 호환되지 않는다 */
	case BLKTRACESETUP: /* [한국어] BLKTRACESETUP(구버전 blktrace 설정)과 */
	case BLKTRACESETUP2: /* [한국어] BLKTRACESETUP2(신버전) 모두 */
		return blk_trace_ioctl(bdev, cmd, argp); /* [한국어] blktrace 서브시스템의 공통 ioctl 처리로 위임 */
	default: /* [한국어] 위 어떤 case에도 해당하지 않으면 */
		break; /* [한국어] switch를 빠져나가 아래의 공통 경로(blkdev_common_ioctl)로 진행 */
	}

	ret = blkdev_common_ioctl(bdev, mode, cmd, arg, argp); /* [한국어] 위 native 전용 처리 어디에도 없던 cmd는 native/compat 공통 디스패처로 위임 */
	if (ret != -ENOIOCTLCMD) /* [한국어] blkdev_common_ioctl()이 실제로 처리했다면(즉 -ENOIOCTLCMD가 아니면) */
		return ret; /* [한국어] 그 결과를 그대로 반환 */

	if (!bdev->bd_disk->fops->ioctl) /* [한국어] 여기까지 왔다는 것은 이 파일이 이 cmd를 전혀 모른다는 뜻 - 드라이버가 자체 ioctl 콜백을 등록했는지 확인 */
		return -ENOTTY; /* [한국어] 드라이버도 처리할 수 없음 - Inappropriate ioctl for device */
	return bdev->bd_disk->fops->ioctl(bdev, mode, cmd, arg); /* [한국어] 드라이버의 fops->ioctl 콜백으로 최종 위임 - NVMe라면 vendor/passthrough ioctl 등이 여기서 처리됨(추정) */
}

#ifdef CONFIG_COMPAT

#define BLKBSZGET_32		_IOR(0x12, 112, int) /* [한국어] BLKBSZGET의 32비트 전용 명령 번호 - ioctl 매직 0x12(레거시 hd/ide 계열), 번호 112, int 크기로 재정의 */
#define BLKBSZSET_32		_IOW(0x12, 113, int) /* [한국어] BLKBSZSET의 32비트 전용 명령 번호(113) */
#define BLKGETSIZE64_32		_IOR(0x12, 114, int) /* [한국어] BLKGETSIZE64의 32비트 전용 명령 번호(114) - 원래 명령과 자료구조는 호환되나 번호 자체가 다름 */

/* Most of the generic ioctls are handled in the normal fallback path.
   This assumes the blkdev's low level compat_ioctl always returns
   ENOIOCTLCMD for unknown ioctls. */
/*
 * [한국어] 위 원문 번역: 대부분의 범용 ioctl은 일반적인 폴백 경로에서 처리된다.
 * 이는 blkdev의 저수준 compat_ioctl이 알 수 없는 ioctl에 대해 항상
 * ENOIOCTLCMD를 반환한다고 가정한다.
 *
 * compat_blkdev_ioctl - 블록 디바이스에 대한 32비트 호환 ioctl 진입점
 *
 * @file: ioctl(2)을 호출한 32비트 호환 태스크의 열린 파일.
 * @cmd:  ioctl 명령 번호(32비트 사용자 공간 기준).
 * @arg:  명령별 인자(32비트 폭으로 전달됨).
 * @return: 각 case/피호출 함수의 반환값. compat_ioctl 콜백도 없거나
 *      -ENOIOCTLCMD를 유지하면 그 값을 그대로 반환한다(blkdev_ioctl()과
 *      달리 -ENOTTY로 바꾸지 않고 그대로 리턴하는 점에 주의).
 *
 * blkdev_ioctl()의 32비트 호환 버전. 자료구조가 완전히 다른 명령
 * (HDIO_GETGEO, BLKPG)은 compat 전용 헬퍼로 보내고, "정수 폭만 다른" 명령
 * (BLKRAGET/BLKFRAGET/BLKGETSIZE)은 compat_put_long/compat_put_ulong으로
 * 반환하며, "명령 번호 자체가 32/64비트에서 다른" 세 개(BLKBSZGET_32/
 * BLKBSZSET_32/BLKGETSIZE64_32, 위에서 새로 정의됨)를 매핑하고, i386
 * 정렬 문제가 있는 BLKTRACESETUP32를 처리한다. 나머지는 blkdev_common_
 * ioctl()로 넘기되, 그것도 처리 못 하면(-ENOIOCTLCMD) 드라이버가 등록한
 * disk->fops->compat_ioctl()로 한 번 더 위임을 시도한다.
 * 실행 컨텍스트: ioctl 시스템 호출의 프로세스 컨텍스트(32비트 호환 태스크).
 * 호출자: VFS의 compat_ptr_ioctl 경로 (block_device_operations.compat_ioctl
 *         콜백을 통해).
 * 피호출자: compat_hdio_getgeo(), compat_blkpg_ioctl(), compat_put_long(),
 *          compat_put_ulong(), put_int(), blkdev_bszset(), put_u64(),
 *          blk_trace_ioctl(), blkdev_common_ioctl(), disk->fops->compat_ioctl()
 *          (드라이버).
 *
 * 호출 체인:
 *   사용자 공간 ioctl(2, 32비트 태스크) → VFS compat 경로 →
 *   [compat_blkdev_ioctl] → (직접 처리 | blkdev_common_ioctl |
 *   disk->fops->compat_ioctl)
 */
long compat_blkdev_ioctl(struct file *file, unsigned cmd, unsigned long arg)
{
	int ret; /* [한국어] blkdev_common_ioctl() 등의 반환값을 담을 변수 */
	void __user *argp = compat_ptr(arg); /* [한국어] 32비트 인자를 compat_ptr()로 64비트 커널 포인터로 확장 */
	struct block_device *bdev = I_BDEV(file->f_mapping->host); /* [한국어] file이 가리키는 inode에서 block_device 획득 */
	struct gendisk *disk = bdev->bd_disk; /* [한국어] compat_ioctl 폴백 시 사용할 gendisk 미리 획득 */
	blk_mode_t mode = file_to_blk_mode(file); /* [한국어] file의 open 플래그를 blk_mode_t로 변환 */

	switch (cmd) { /* [한국어] ioctl 명령 번호로 분기 시작 */
	/* These need separate implementations for the data structure */
	/* [한국어] 위 원문 번역: 자료구조가 다른 명령들은 별도 구현이 필요하다 */
	case HDIO_GETGEO: /* [한국어] HDIO_GETGEO: 지오메트리 조회(compat 레이아웃) */
		return compat_hdio_getgeo(bdev, argp); /* [한국어] compat_hdio_getgeo()에 위임 */
	case BLKPG: /* [한국어] BLKPG: 파티션 조작(compat 레이아웃) */
		return compat_blkpg_ioctl(bdev, argp); /* [한국어] compat_blkpg_ioctl()에 위임 */

	/* [한국어] 위 원문 번역: compat 모드는 'long' 대신 32비트 데이터를 반환한다 */
	/* Compat mode returns 32-bit data instead of 'long' */
	case BLKRAGET: /* [한국어] BLKRAGET(구버전)과 */
	case BLKFRAGET: /* [한국어] BLKFRAGET(신버전) 모두 */
		if (!argp) /* [한국어] 사용자 결과 포인터 NULL 검사 */
			return -EINVAL; /* [한국어] 잘못된 인자 */
		return compat_put_long(argp, /* [한국어] ra_pages를 바이트로 환산 후 512로 나눠 섹터 단위로 변환, compat 폭으로 반환 */
			(bdev->bd_disk->bdi->ra_pages * PAGE_SIZE) / 512); /* [한국어] (계속) 계산식의 나머지 부분 */
	case BLKGETSIZE: /* [한국어] BLKGETSIZE: 레거시 전체 섹터 수 조회(compat_ulong_t 폭) */
		if (bdev_nr_sectors(bdev) > ~(compat_ulong_t)0) /* [한국어] 섹터 수가 compat_ulong_t(32비트) 범위를 넘는지 확인 - native 버전과 달리 항상 32비트 기준으로 검사 */
			return -EFBIG; /* [한국어] File too large */
		return compat_put_ulong(argp, bdev_nr_sectors(bdev)); /* [한국어] 전체 섹터 수를 compat_ulong_t 폭으로 반환 */

	/* The data is compatible, but the command number is different */
	/* [한국어] 위 원문 번역: 데이터는 호환되지만 명령 번호 자체가 다르다 */
	case BLKBSZGET_32: /* get the logical block size (cf. BLKSSZGET) */ /* [한국어] BLKBSZGET_32: 위에서 정의한 32비트 전용 명령 번호로 논리 블록 크기 조회 요청이 들어옴 */
		return put_int(argp, bdev_logical_block_size(bdev)); /* [한국어] put_int()로 논리 블록 크기 반환 - native BLKSSZGET과 동일 정보, 명령 번호만 다름 */
	case BLKBSZSET_32: /* [한국어] BLKBSZSET_32: soft block size 설정(32비트 전용 명령 번호) */
		return blkdev_bszset(file, mode, argp); /* [한국어] blkdev_bszset()에 위임 - native와 동일 처리 함수 재사용 */
	case BLKGETSIZE64_32: /* [한국어] BLKGETSIZE64_32: 전체 용량 바이트 단위 조회(32비트 전용 명령 번호) */
		return put_u64(argp, bdev_nr_bytes(bdev)); /* [한국어] put_u64()로 전체 바이트 수 반환 */

	/* Incompatible alignment on i386 */
	/* [한국어] 위 원문 번역: i386에서는 정렬 방식이 호환되지 않는다 */
	case BLKTRACESETUP32: /* [한국어] BLKTRACESETUP32: i386 등에서 정렬이 다른 32비트 blktrace 설정 명령 */
		return blk_trace_ioctl(bdev, cmd, argp); /* [한국어] blktrace 서브시스템의 공통 ioctl 처리로 위임 */
	default: /* [한국어] 위 어떤 case에도 해당하지 않으면 */
		break; /* [한국어] switch를 빠져나가 아래 공통 경로로 진행 */
	}

	ret = blkdev_common_ioctl(bdev, mode, cmd, arg, argp); /* [한국어] native/compat 공통 디스패처로 위임 */
	if (ret == -ENOIOCTLCMD && disk->fops->compat_ioctl) /* [한국어] 공통 디스패처가 처리 못 했고(-ENOIOCTLCMD) 드라이버가 compat_ioctl 콜백을 등록했으면 */
		ret = disk->fops->compat_ioctl(bdev, mode, cmd, arg); /* [한국어] 드라이버의 compat_ioctl로 최종 위임(native fops->ioctl과 달리 -ENOTTY로 바꾸지 않음) */

	return ret; /* [한국어] 최종 결과 반환 - -ENOIOCTLCMD가 그대로 남아 있을 수도 있음(blkdev_ioctl()과의 차이점) */
}
#endif

/*
 * [한국어]
 * struct blk_iou_cmd - block layer가 io_uring 명령마다 유지하는 private 데이터
 *
 * io_uring_cmd_to_pdu()가 반환하는 struct io_uring_cmd 내장 페이로드 영역에
 * 저장되며, BLOCK_URING_CMD_DISCARD 하나의 생명주기(제출→bio 완료→task
 * work→io_uring completion queue 통지) 동안 상태를 들고 다니는 용도다.
 */
struct blk_iou_cmd {
	int res;
/* [한국어] int res;
 * 이 io_uring 명령의 최종 완료 코드(errno 또는 0).
 * 설정자: blkdev_cmd_discard()가 초기값 0으로 설정하거나 -EAGAIN(멀티 bio
 *   중 일부만 non-blocking으로 처리된 경우)으로 설정; bio_cmd_bio_end_io()가
 *   bio->bi_status를 errno로 변환해 최초 1회만 갱신(이미 설정된 값은 덮지
 *   않음 - "!bic->res" 조건 참고).
 * 읽는 자: blk_cmd_complete()가 io_uring_cmd_done()에 전달하거나, -EAGAIN +
 *   nowait 조합이면 blocking 재시도를 트리거하는 조건으로 사용.
 * 값 범위: 0(성공) 또는 음수 errno(예: -EAGAIN, blk_status_to_errno()의 결과).
 * 동기화: 하나의 io_uring 명령은 특정 시점에 단일 bio 완료 경로 또는 단일
 *   task work에서만 갱신되므로 별도 락 없이 순차적으로 접근된다. */
	bool nowait;
/* [한국어] bool nowait;
 * 이 명령이 non-blocking(IO_URING_F_NONBLOCK) 방식으로 제출되었는지 여부.
 * 설정자: blkdev_uring_cmd()가 issue_flags로부터 초기 설정.
 * 읽는 자: blkdev_cmd_discard()가 GFP_NOWAIT 선택과 멀티 bio 제한 판단에
 *   사용; blk_cmd_complete()가 -EAGAIN 시 blocking 재시도 여부 결정에 사용.
 * 값 범위: true(비차단 요청, sleep 불가) / false(일반 블로킹 요청 허용).
 * 동기화: 명령 생성 시 1회 설정 후 읽기 전용으로만 참조되므로 락 불필요. */
};

/*
 * [한국어]
 * blk_cmd_complete - io_uring 블록 명령의 task-work 완료 콜백
 *
 * @tw_req: 이 task work가 속한 io_uring 요청(io_uring_cmd_from_tw()로 원래의
 *      io_uring_cmd를 복원하는 데 사용).
 * @tw: task-work 토큰(io_uring 내부용, 이 함수 본문에서는 직접 사용되지 않음).
 *
 * bio 완료 인터럽트/softirq 컨텍스트에서 곧바로 io_uring 완료를 알리는 대신,
 * io_uring_cmd_do_in_task_lazy()가 예약한 task work를 통해 프로세스(태스크)
 * 컨텍스트에서 실행되도록 지연시킨 콜백이다. 결과가 -EAGAIN이고 애초에
 * nowait로 제출된 요청이었다면, 부분적으로만 처리된 멀티 bio 요청을 이제는
 * blocking 컨텍스트에서 재시도(io_uring_cmd_issue_blocking)하도록 유도한다.
 * 그 외의 경우(성공 또는 nowait가 아닌 실패)에는 io_uring_cmd_done()으로
 * completion queue(CQE)를 채워 사용자 공간에 완료를 통지한다.
 * 실행 컨텍스트: io_uring task work 컨텍스트(제출자 태스크로 복귀한 프로세스
 * 컨텍스트) - sleep 가능.
 * 호출자: io_uring_cmd_do_in_task_lazy() (bio_cmd_bio_end_io()가 예약).
 * 피호출자: io_uring_cmd_from_tw(), io_uring_cmd_to_pdu(),
 *          io_uring_cmd_issue_blocking(), io_uring_cmd_done().
 *
 * 호출 체인:
 *   bio_cmd_bio_end_io → io_uring_cmd_do_in_task_lazy → [blk_cmd_complete]
 *   → io_uring_cmd_done (또는 io_uring_cmd_issue_blocking로 재시도)
 */
static void blk_cmd_complete(struct io_tw_req tw_req, io_tw_token_t tw)
{
	struct io_uring_cmd *cmd = io_uring_cmd_from_tw(tw_req); /* [한국어] tw_req로부터 원래의 io_uring_cmd 포인터를 복원 */
	struct blk_iou_cmd *bic = io_uring_cmd_to_pdu(cmd, struct blk_iou_cmd); /* [한국어] io_uring_cmd에 내장된 blk_iou_cmd private 데이터(res/nowait) 포인터 획득 */

	if (bic->res == -EAGAIN && bic->nowait) /* [한국어] 결과가 -EAGAIN이고 원래 non-blocking으로 제출됐던 요청이면 - 즉 "일부만 처리하고 재시도가 필요한" 상태 */
		io_uring_cmd_issue_blocking(cmd); /* [한국어] blocking 컨텍스트에서 이 명령을 다시 발행하도록 io_uring에 요청(이번엔 sleep 허용되어 나머지 bio도 처리 가능) */
	else /* [한국어] 그 외(정상 완료 또는 재시도 불필요한 실패)면 */
		io_uring_cmd_done(cmd, bic->res, /* [한국어] completion queue에 최종 결과(res)를 채워 사용자 공간에 완료 통지 */
				  IO_URING_CMD_TASK_WORK_ISSUE_FLAGS); /* [한국어] (계속) task-work 컨텍스트에서의 완료 처리임을 나타내는 플래그 전달 */
}

/*
 * [한국어]
 * bio_cmd_bio_end_io - io_uring discard 명령이 제출한 bio의 완료 콜백
 *
 * @bio: 완료된 discard bio. bi_private에 원래의 struct io_uring_cmd 포인터가
 *      저장되어 있다(blkdev_cmd_discard()가 제출 직전에 설정).
 *
 * 하드웨어/드라이버(예: NVMe)가 CQ(Completion Queue) 엔트리를 통해 이 bio의
 * 완료를 알리면 블록 계층이 이 콜백을 호출한다. bio->bi_status(블록 계층
 * 표준 상태 코드)를 errno로 변환해 blk_iou_cmd.res에 기록하되, 이미 앞선
 * bio에서 에러가 기록되어 있으면(!bic->res 조건) 덮어쓰지 않아 "가장 먼저
 * 발생한 에러"를 유지한다. 그런 다음 io_uring_cmd_do_in_task_lazy()로 실제
 * 사용자 통지(blk_cmd_complete())를 프로세스 컨텍스트로 지연시키고, bio
 * 자체는 더 이상 필요 없으므로 즉시 반환(해제)한다.
 * 실행 컨텍스트: bio 완료 컨텍스트 - 전형적으로 인터럽트/softirq 또는 blk-mq
 * 완료 워크. sleep 불가능한 컨텍스트를 전제로 하므로 실제 완료 통지는
 * task work로 넘긴다.
 * 호출자: bio 완료 경로(bio_endio() → bi_end_io, blkdev_cmd_discard()가
 *         마지막 bio에 이 함수를 등록).
 * 피호출자: blk_status_to_errno(), io_uring_cmd_do_in_task_lazy(), bio_put().
 *
 * 호출 체인:
 *   (NVMe CQ 수신 → blk-mq 완료 경로) → bio_endio → [bio_cmd_bio_end_io]
 *   → io_uring_cmd_do_in_task_lazy → blk_cmd_complete
 */
static void bio_cmd_bio_end_io(struct bio *bio)
{
	struct io_uring_cmd *cmd = bio->bi_private; /* [한국어] bi_private에 저장해 둔 원래의 io_uring_cmd 포인터 복원 */
	struct blk_iou_cmd *bic = io_uring_cmd_to_pdu(cmd, struct blk_iou_cmd); /* [한국어] io_uring_cmd에 내장된 blk_iou_cmd private 데이터(res) 포인터 획득 */

	if (unlikely(bio->bi_status) && !bic->res) /* [한국어] bio가 에러 상태이고 아직 res에 에러가 기록되지 않았으면(최초 에러) */
		bic->res = blk_status_to_errno(bio->bi_status); /* [한국어] 블록 계층 상태 코드를 표준 errno로 변환해 기록 - 이후 다른 bio가 에러여도 이 최초 값을 덮지 않음 */

	io_uring_cmd_do_in_task_lazy(cmd, blk_cmd_complete); /* [한국어] 실제 io_uring 완료 통지(blk_cmd_complete)를 프로세스 컨텍스트로 지연 예약 - 현재(완료) 컨텍스트에서는 sleep 불가하므로 */
	bio_put(bio); /* [한국어] 이 bio는 더 이상 필요 없으므로 참조 해제 */
}

/*
 * [한국어]
 * blkdev_cmd_discard - BLOCK_URING_CMD_DISCARD 처리: io_uring 기반 비동기 discard
 *
 * @cmd:    실행 중인 io_uring 명령. io_uring_cmd_to_pdu()로 blk_iou_cmd에
 *        접근하고, cmd->file로 열린 파일의 모드를 확인한다.
 * @bdev:   discard 대상 블록 디바이스.
 * @start:  요청 시작 오프셋(바이트 단위).
 * @len:    요청 길이(바이트 단위).
 * @nowait: true면 non-blocking 제출(sleep 불가, GFP_NOWAIT, 멀티 bio 제한).
 * @return: 정상 접수 시 -EIOCBQUEUED(비동기로 진행 중이며 완료는 나중에
 *      io_uring completion queue로 통지됨을 의미), 즉시 실패 시 -EOPNOTSUPP/
 *      -EBADF/-EPERM/blk_validate_byte_range()의 값/filemap_invalidate_pages()
 *      의 값/-EAGAIN.
 *
 * blk_ioctl_discard()의 io_uring 버전이지만 두 가지가 다르다. 첫째, 동기
 * ioctl 경로처럼 inode_lock+filemap_invalidate_lock+truncate_bdev_range로
 * 무겁게 페이지 캐시를 잠그고 truncate하는 대신, nowait를 인식하는
 * filemap_invalidate_pages()로 더 가볍게 무효화를 시도한다(nowait이면 락을
 * 얻지 못할 때 즉시 실패). 둘째, 여러 bio로 쪼개야 하는 상황에서 nowait
 * 요청은 "첫 번째 bio만 제출하고 나머지는 즉시 -EAGAIN으로 포기"한다(주석
 * 원문 참고) - 여러 bio 중 일부만 성공하고 일부가 실패하는 애매한 상태를
 * 피하기 위함이며, 호출자(io_uring)는 blocking 컨텍스트에서 전체를
 * 재시도해야 한다. 마지막 bio에는 io_uring 완료 콜백(bio_cmd_bio_end_io)을
 * 등록해 제출 후 즉시 -EIOCBQUEUED를 반환하여 논블로킹/비동기 완료 모델을
 * 따른다.
 * 실행 컨텍스트: io_uring SQE(Submission Queue Entry) 처리 컨텍스트 - nowait
 * 여부에 따라 sleep 가능 여부가 달라진다.
 * 호출자: blkdev_uring_cmd() (BLOCK_URING_CMD_DISCARD case).
 * 피호출자: bdev_max_discard_sectors(), file_to_blk_mode(), bdev_read_only(),
 *          blk_validate_byte_range(), filemap_invalidate_pages(),
 *          blk_alloc_discard_bio(), bio_chain_and_submit(), submit_bio().
 *
 * 호출 체인:
 *   사용자 공간 io_uring SQE(BLOCK_URING_CMD_DISCARD) → blkdev_uring_cmd →
 *   [blkdev_cmd_discard] → blk_alloc_discard_bio(block/blk-lib.c) →
 *   bio_chain_and_submit(block/bio.c) → submit_bio → ... → nvme_queue_rq
 *   → (완료 시) bio_cmd_bio_end_io → blk_cmd_complete → io_uring_cmd_done
 */
static int blkdev_cmd_discard(struct io_uring_cmd *cmd,
			      struct block_device *bdev,
			      uint64_t start, uint64_t len, bool nowait)
{
	struct blk_iou_cmd *bic = io_uring_cmd_to_pdu(cmd, struct blk_iou_cmd); /* [한국어] io_uring_cmd에 내장된 blk_iou_cmd private 데이터(res) 포인터 획득 */
	gfp_t gfp = nowait ? GFP_NOWAIT : GFP_KERNEL; /* [한국어] nowait면 sleep 불가능한 GFP_NOWAIT, 아니면 일반 GFP_KERNEL로 메모리 할당 플래그 선택 */
	sector_t sector = start >> SECTOR_SHIFT; /* [한국어] 바이트 시작 오프셋을 섹터 단위로 변환 */
	sector_t nr_sects = len >> SECTOR_SHIFT; /* [한국어] 바이트 길이를 섹터 단위로 변환 - blk_alloc_discard_bio()가 in/out으로 소모 */
	struct bio *prev = NULL, *bio; /* [한국어] prev: 체인 anchor(최초 NULL); bio: 이번 반복에서 만든 discard bio */
	int err; /* [한국어] 각 단계의 에러 코드 */

	if (!bdev_max_discard_sectors(bdev)) /* [한국어] 디바이스가 discard를 지원하는지(queue_limits) 확인 */
		return -EOPNOTSUPP; /* [한국어] 미지원 - Operation not supported */
	if (!(file_to_blk_mode(cmd->file) & BLK_OPEN_WRITE)) /* [한국어] cmd->file이 쓰기 가능 모드로 열렸는지 확인(io_uring은 별도 fd 대신 cmd->file을 통해 오픈 모드를 참조) */
		return -EBADF; /* [한국어] Bad file descriptor */
	if (bdev_read_only(bdev)) /* [한국어] 디바이스 자체가 읽기전용 플래그인지 확인 */
		return -EPERM; /* [한국어] Permission denied */
	err = blk_validate_byte_range(bdev, start, len); /* [한국어] 정렬/길이/오버플로우/용량 초과 등 범위 유효성 검증(ioctl 경로와 동일 헬퍼 재사용) */
	if (err) /* [한국어] 검증 실패 여부 확인 */
		return err; /* [한국어] 유효하지 않은 범위 - 즉시 반환 */

	err = filemap_invalidate_pages(bdev->bd_mapping, start, /* [한국어] 대상 범위의 페이지 캐시를 무효화 - ioctl 경로의 inode_lock+filemap_invalidate_lock+truncate_bdev_range보다 가벼운 nowait 인식 버전 */
					start + len - 1, nowait); /* [한국어] (계속) nowait면 락을 즉시 얻지 못할 때 블로킹하지 않고 실패 반환 */
	if (err) /* [한국어] 무효화 실패 여부 확인(nowait인데 락 경합 등) */
		return err; /* [한국어] 실패를 그대로 전파 - io_uring이 blocking 컨텍스트에서 재시도하게 됨 */

	while (true) { /* [한국어] 남은 길이를 다 소진할 때까지 반복 - ioctl 경로와 달리 시그널 검사 없이 nowait/succeed 조건으로만 제어 */
		bio = blk_alloc_discard_bio(bdev, &sector, &nr_sects, gfp); /* [한국어] discard_granularity 제한에 맞춰 다음 bio 할당(gfp는 nowait 여부에 따라 GFP_NOWAIT/GFP_KERNEL) */
		if (!bio) /* [한국어] 남은 길이가 소진되어 더 만들 bio가 없으면(NULL) */
			break; /* [한국어] 루프 탈출 */
		if (nowait) { /* [한국어] non-blocking 제출 요청이면 */
			/*
			 * Don't allow multi-bio non-blocking submissions as
			 * subsequent bios may fail but we won't get a direct
			 * indication of that. Normally, the caller should
			 * retry from a blocking context.
			 */
			/* [한국어] 위 원문 번역: 여러 bio에 걸친 non-blocking 제출은 허용하지 않는다 -
			 * 후속 bio들이 실패할 수 있는데 그에 대한 직접적인 표시를 받을 방법이
			 * 없기 때문이다. 일반적으로 호출자는 blocking 컨텍스트에서 재시도해야 한다. */
			if (unlikely(nr_sects)) { /* [한국어] 아직 처리할 섹터가 남아 있다면(즉 이번 bio가 전체를 다 담지 못해 두 번째 bio가 필요한 상황) */
				bio_put(bio); /* [한국어] 방금 만든(아직 제출 안 한) bio를 즉시 해제 - 멀티 bio non-blocking을 금지하는 정책 */
				return -EAGAIN; /* [한국어] 나머지는 blocking 컨텍스트에서 재시도하라고 즉시 반환 */
			}
			bio->bi_opf |= REQ_NOWAIT; /* [한국어] 이 bio에 REQ_NOWAIT 플래그 추가 - 하위 큐 진입(bio_queue_enter)에서도 sleep 대신 즉시 -EAGAIN을 받도록 지시(NVMe SQ가 가득 찬 경우 등) */
		}

		prev = bio_chain_and_submit(prev, bio); /* [한국어] 이전 anchor(prev)를 이번 bio에 체인하고 제출, 이번 bio를 다음 anchor로 갱신 */
	}
	if (unlikely(!prev)) /* [한국어] 루프를 한 번도 못 돌아 bio가 하나도 만들어지지 않았으면(prev==NULL, 예: 최초 nowait 할당부터 실패) */
		return -EAGAIN; /* [한국어] 재시도 필요 - blocking 컨텍스트에서 다시 시도하라고 알림 */
	if (unlikely(nr_sects)) /* [한국어] 위의 nowait 멀티 bio 금지 로직을 통과했더라도, 만약 남은 섹터가 있다면(이론상 도달하지 않아야 하나 방어적 체크, 추정) */
		bic->res = -EAGAIN; /* [한국어] 최종 결과를 -EAGAIN으로 미리 설정해 완료 콜백에서 참고하게 함 */

	prev->bi_private = cmd; /* [한국어] 마지막(유일한 미제출) bio의 bi_private에 io_uring_cmd를 저장 - 완료 콜백이 이 값으로 원래 명령을 복원 */
	prev->bi_end_io = bio_cmd_bio_end_io; /* [한국어] 완료 콜백을 bio_cmd_bio_end_io로 설정 - 표준 bio_endio() 경로가 이 함수를 호출하게 됨 */
	submit_bio(prev); /* [한국어] 마지막 bio 제출 - 이 시점부터 완료는 비동기(bio_cmd_bio_end_io)로 처리됨 */
	return -EIOCBQUEUED; /* [한국어] 이미 큐에 접수되어 비동기로 진행 중임을 나타내는 io_uring 표준 반환값 */
}

/*
 * [한국어]
 * blkdev_uring_cmd - 블록 디바이스에 대한 io_uring 명령 진입점
 *
 * @cmd: 실행할 io_uring 명령. cmd->sqe(Submission Queue Entry)와
 *      cmd->cmd_op(세부 명령 종류)을 담고 있다.
 * @issue_flags: IO_URING_F_NONBLOCK 등 이번 발행(issue)의 특성 플래그.
 * @return: 각 세부 명령의 반환값(예: blkdev_cmd_discard()의 -EIOCBQUEUED),
 *      알 수 없는 cmd_op이면 -EINVAL.
 *
 * io_uring이 IORING_OP_URING_CMD로 파일에 위임한 블록 계층 전용 명령들의
 * 최상위 디스패처다. 현재는 BLOCK_URING_CMD_DISCARD 하나만 구현되어 있다.
 * 먼저 SQE의 예약/미사용 필드(ioprio, __pad1, len, rw_flags, file_index)가
 * 모두 0인지 검증해(향후 확장 대비 및 오용 방지), blk_iou_cmd의 res/nowait를
 * 초기화한 뒤, SQE의 addr/addr3 필드를 각각 start/len(바이트 단위, 이 명령
 * 전용 의미로 재해석)으로 읽어 cmd_op별 핸들러로 분기한다.
 * 실행 컨텍스트: io_uring 워커 또는 제출자 태스크 컨텍스트(issue_flags에
 * 따라 sleep 가능 여부가 달라짐).
 * 호출자: io_uring 코어(io_uring/cmd.c 등)의 uring_cmd 파일 오퍼레이션 콜백
 *         경유.
 * 피호출자: io_uring_cmd_to_pdu(), READ_ONCE(), blkdev_cmd_discard().
 *
 * 호출 체인:
 *   사용자 공간 io_uring_enter(IORING_OP_URING_CMD) → io_uring 코어 →
 *   [blkdev_uring_cmd] → blkdev_cmd_discard (BLOCK_URING_CMD_DISCARD인 경우)
 */
int blkdev_uring_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags)
{
	struct block_device *bdev = I_BDEV(cmd->file->f_mapping->host); /* [한국어] cmd->file(io_uring이 연 블록 특수 파일)의 inode에서 block_device 획득 */
	struct blk_iou_cmd *bic = io_uring_cmd_to_pdu(cmd, struct blk_iou_cmd); /* [한국어] io_uring_cmd에 내장된 blk_iou_cmd private 데이터 포인터 획득 - 이번 명령 동안 상태를 저장할 공간 */
	const struct io_uring_sqe *sqe = cmd->sqe; /* [한국어] 이 명령의 원본 Submission Queue Entry(SQE) 포인터 획득 - addr/addr3 등 필드를 읽기 위함 */
	u32 cmd_op = cmd->cmd_op; /* [한국어] 세부 명령 종류(BLOCK_URING_CMD_DISCARD 등) 획득 */
	uint64_t start, len; /* [한국어] SQE에서 읽어올 start/len(바이트 단위) 지역 변수 */

	if (unlikely(sqe->ioprio || sqe->__pad1 || sqe->len || /* [한국어] 이 블록 명령이 사용하지 않는 SQE 필드(ioprio/__pad1/len/rw_flags/file_index) 중 하나라도 0이 아니면 - 향후 확장 예약 필드에 대한 오용/버그 방지 */
		     sqe->rw_flags || sqe->file_index)) /* [한국어] (계속) 나머지 필드 조건 */
		return -EINVAL; /* [한국어] 예약 필드 위반 - 잘못된 인자 */

	bic->res = 0; /* [한국어] 이번 명령의 결과 코드를 0(아직 미완료/성공 가정)으로 초기화 */
	bic->nowait = issue_flags & IO_URING_F_NONBLOCK; /* [한국어] 이번 발행이 non-blocking(IO_URING_F_NONBLOCK)인지 여부를 blk_iou_cmd에 저장 - 이후 세부 핸들러들이 참조 */

	start = READ_ONCE(sqe->addr); /* [한국어] SQE의 addr 필드를 이 명령 전용 의미인 "시작 오프셋(바이트)"으로 재해석해 읽음(SQE 갱신 경합 방지를 위해 READ_ONCE 사용) */
	len = READ_ONCE(sqe->addr3); /* [한국어] SQE의 addr3 필드를 "길이(바이트)"로 재해석해 읽음 */

	switch (cmd_op) { /* [한국어] 세부 명령 종류로 분기 시작 */
	case BLOCK_URING_CMD_DISCARD: /* [한국어] BLOCK_URING_CMD_DISCARD: TRIM/UNMAP 요청 */
		return blkdev_cmd_discard(cmd, bdev, start, len, bic->nowait); /* [한국어] blkdev_cmd_discard()에 위임 - 결과는 즉시 반환되거나(-EIOCBQUEUED) 에러 코드 */
	}
	return -EINVAL; /* [한국어] 알 수 없는 cmd_op - 잘못된 인자 */
}
