// SPDX-License-Identifier: GPL-2.0
/*
 * [한국어 설명] 사용자/커널 버퍼를 request의 bio로 매핑하는 블록 계층 헬퍼 모음 (block/blk-map.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 blk_rq_map_user(), blk_rq_map_user_iov(), blk_rq_map_user_io(),
 * blk_rq_map_kern(), blk_rq_unmap_user() 등 "passthrough request" 전용 매핑
 * API를 제공한다. SG_IO ioctl, NVMe/SCSI character device passthrough(예:
 * nvme-cli의 admin/IO passthrough 명령), 커널 내부 모듈이 파일시스템을 거치지
 * 않고 블록 디바이스에 직접 명령을 내리는 모든 경로에서, 호출자가 넘긴 사용자
 * 공간 버퍼(iovec)나 커널 버퍼를 하나 이상의 bio로 변환해 request에 연결하는
 * 역할을 한다. 사용자 페이지를 그대로 pin해서 쓰는 zero-copy 경로(bio_map_user_iov)
 * 와, DMA 정렬/가상 경계 제약 때문에 커널 페이지에 복사해야 하는 bounce-buffer
 * 경로(bio_copy_user_iov, bio_copy_kern)를 모두 구현하며, 어느 경로를 쓸지는
 * DMA 정렬 요구사항, 가상 경계(virt boundary), iov_iter 종류에 따라
 * blk_rq_map_user_iov()가 자동으로 선택한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 "블록 계층이 request를 준비하는 단계"에 위치하며, 통상적인
 * 파일시스템 read/write 경로(generic_file_read 등)가 아니라 passthrough 명령
 * 경로에서만 쓰인다. 대표적인 제출 방향 호출 체인은 다음과 같다.
 *   SG_IO ioctl / NVMe passthrough ioctl (예: nvme-cli의 admin-passthru)
 *     -> blk_rq_map_user() / blk_rq_map_user_io() / blk_rq_map_kern()
 *     -> (bio_map_user_iov | bio_copy_user_iov | blk_rq_map_user_bvec
 *         | bio_map_kern | bio_copy_kern)
 *     -> blk_rq_append_bio()
 *     -> blk_execute_rq / blk_mq_run_hw_queue -> mq_ops->queue_rq (간접 호출)
 * I/O 완료 후에는 역방향으로 다음과 같이 실행된다.
 *   장치 완료 -> blk_mq_end_request() -> (호출자가) blk_rq_unmap_user()
 *     -> (bio_uncopy_user | bio_release_pages) -> bio_integrity_unmap_user()
 *
 * === NVMe passthrough 에서 이 파일이 놓이는 정확한 자리 (실물 확인) ===
 * drivers/nvme/host/ioctl.c 의 nvme_map_user_request() 가 다음을 호출한다:
 *     ret = blk_rq_map_user_io(req, NULL, nvme_to_user_ptr(ubuffer), ...);
 * 즉 nvme-cli 의 admin-passthru/io-passthru 와 io_uring 패스스루가 데이터
 * 버퍼를 커널에 들여오는 통로가 정확히 이 파일이다.
 *
 * 다만 이 파일은 PRP 도 SGL 도 만들지 않는다. 여기서 만들어지는 것은
 * bio 와 그 안의 bio_vec 배열(page, offset, len)뿐이다. 그 bvec 이
 * PRP/SGL 로 번역되는 것은 훨씬 뒤, 드라이버 안에서다:
 *     nvme_pci_setup_data_prp()    — PRP1/PRP2/PRP list 구성
 *     nvme_pci_setup_data_sgl()    — SGL descriptor 구성
 *     nvme_pci_setup_data_simple() — 단일 물리 세그먼트 고속 경로
 * 이 파일이 신경 쓰는 것은 오직 "그 번역이 가능한 모양의 bvec 을 만드는
 * 것"이며, 불가능하면 사용자 페이지를 포기하고 커널 페이지로 복사한다
 * (bounce). 그 판정 기준이 아래 두 큐 한계다.
 *
 * === bounce 여부를 가르는 두 한계와 NVMe 에서의 실제 값 ===
 * nvme_set_ctrl_limits() (core.c) 가 큐에 심는 값들:
 *     lim->dma_alignment      = 3;
 *         → 4바이트 정렬. 512B 를 요구하는 장치들과 달리 NVMe 는 매우 관대해
 *           대부분의 사용자 버퍼가 복사 없이 그대로 매핑된다.
 *     lim->virt_boundary_mask = ctrl->ops->get_virt_boundary(ctrl, is_admin);
 *         → PCIe 는 (NVME_CTRL_PAGE_SIZE - 1). PRP 는 "첫 항목만 오프셋을
 *           가질 수 있고 나머지는 페이지 경계에 딱 맞아야 한다"는 형식이라,
 *           중간에 페이지 경계를 어긋나게 걸치는 세그먼트가 있으면 PRP 로
 *           표현할 수 없다. 이 마스크가 그 제약을 블록 계층 언어로 옮긴 것이며,
 *           blk_rq_map_user_iov() 가 이것을 보고 zero-copy 를 포기한다.
 *     lim->max_hw_sectors     = ctrl->max_hw_sectors;
 *         → max_hw_sectors = nvme_mps_to_sectors(ctrl, id->mdts), MDTS=0 이면
 *           UINT_MAX(무제한). 한 명령이 옮길 수 있는 최대 바이트 수다.
 *     lim->max_segments       = min(USHRT_MAX,
 *                                min_not_zero(nvme_max_drv_segments(ctrl), ...))
 *         → nvme_max_drv_segments = max_hw_sectors / (페이지/섹터) + 1,
 *           즉 MDTS 에서 유도한 세그먼트 수 상한.
 * 이 한계들을 넘으면 blk_rq_append_bio() 가 실패하고, 호출자는 명령을
 * 더 작게 쪼개야 한다. nvme-cli 가 큰 전송을 나눠 보내는 이유가 이것이다.
 * 이 파일의 API 호출부는 항상 매핑을 요청한 프로세스의 컨텍스트(user/process
 * context, 슬립 가능)에서 실행되며 인터럽트 컨텍스트에서 직접 호출되지 않는다.
 * 다만 bio 완료 콜백(bio_map_kern_endio, bio_copy_kern_endio 등)은 NVMe
 * 인터럽트나 softirq(BLOCK_SOFTIRQ)/workqueue 컨텍스트에서 실행될 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * - block/blk-merge.c: blk_rq_append_bio()가 호출하는 bio_split_io_at()이
 *   정의된 파일로, 하드웨어 세그먼트 개수/최대 전송 크기 제약을 검사해
 *   분할이 필요한지(즉 이 파일이 복사 경로로 폴백해야 하는지) 판단한다.
 * - block/blk-mq.c: 이 파일이 완성한 request->bio 체인은 blk_mq_submit_bio()
 *   를 통해 하드웨어 큐(struct blk_mq_hw_ctx)로 제출된다.
 * - block/bio.c, block/blk.h: bio 할당(bio_alloc_bioset, fs_bio_set 메모리풀),
 *   페이지 추가(bio_add_page, bio_add_virt_nofail, bio_add_vmalloc),
 *   페이지/참조 해제(bio_release_pages, bio_free_pages, bio_put) 등 저수준
 *   bio 조작 헬퍼를 이 파일이 그대로 사용한다.
 * - block/bio-integrity.c: blk_rq_unmap_user()가 bio_integrity_unmap_user()를
 *   호출해 DIF/DIX 데이터 무결성 메타데이터의 사용자 매핑을 함께 해제한다.
 * - drivers/nvme/host/ioctl.c 등 NVMe passthrough 코드: SG_IO 스타일 admin/IO
 *   명령의 데이터 버퍼를 blk_rq_map_user_io()/blk_rq_map_user()로 매핑한 뒤
 *   request를 제출하고, 완료 후 blk_rq_unmap_user()로 매핑을 정리한다.
 * 데이터 흐름 관점에서는 "사용자 iovec/커널 포인터 -> struct iov_iter ->
 * struct bio(bi_io_vec 배열) -> struct request(rq->bio 체인)" 순서로 흘러가며,
 * 완료 후에는 그 역순으로 사용자 공간에 결과가 되돌아간다. 이 파일의 핵심
 * 공유 자료구조는 struct bio_map_data로, bounce-buffer 경로에서 사용자 iov의
 * 깊은 복사본을 보관하며 bio->bi_private에 저장되어 언매핑 시점까지 전달된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - blk_rq_map_user_iov(): zero-copy(bio_map_user_iov)/bounce-copy
 *   (bio_copy_user_iov)/bvec 재사용(blk_rq_map_user_bvec) 중 하나를 선택하는
 *   이 파일의 최상위 진입점. 하나의 iov_iter가 여러 bio로 분할될 수 있다.
 * - blk_rq_map_user() / blk_rq_map_user_io(): 사용자 버퍼(단일 포인터 또는
 *   iovec 배열)를 import_ubuf()/import_iovec()으로 iov_iter로 변환한 뒤
 *   blk_rq_map_user_iov()에 위임하는 얇은 래퍼.
 * - blk_rq_map_kern(): 커널 버퍼를 정렬/스택 여부에 따라 bio_map_kern()
 *   (직접 매핑, 제로카피) 또는 bio_copy_kern()(bounce 복사)으로 매핑.
 * - blk_rq_append_bio(): 만들어진 bio를 request에 연결하거나 기존 request
 *   뒤에 back-merge하며, 그 과정에서 max_hw_sectors 등 하드웨어 제약을 검사.
 * - blk_rq_unmap_user(): I/O 완료 후 매핑을 역순으로 해제(사용자 공간 복사
 *   또는 페이지 pin 해제, 무결성 메타데이터 해제 포함).
 * - struct bio_map_data: is_our_pages(block 계층이 할당한 bounce page인지),
 *   is_null_mapped(데이터 없는 명령인지), iter(원본 사용자 iov_iter의 깊은
 *   복사본), iov[](원본 iovec 배열의 깊은 복사본, flexible array member)로
 *   구성되며 bio->bi_private에 연결되어 언매핑 시점까지 생존한다.
 */
#include <linux/kernel.h> /* 커널 기본 매크로/타입(DIV_ROUND_UP, min/max 등); 페이지 수 계산 등에 사용 */
#include <linux/sched/task_stack.h> /* object_is_on_stack(); 커널 버퍼가 스택에 있는지 검사해 bounce-copy 여부 결정 */
#include <linux/module.h> /* EXPORT_SYMBOL(); 이 파일의 매핑 API를 다른 모듈(NVMe/SCSI 드라이버 등)에 노출 */
#include <linux/bio.h> /* struct bio, bio_alloc_bioset/bio_add_page 등 bio 조작 API 전반 */
#include <linux/blkdev.h> /* struct request/request_queue, queue_limits 등 블록 계층 핵심 자료구조 */
#include <linux/uio.h> /* struct iov_iter/iovec, import_ubuf/import_iovec 등 사용자 벡터 I/O 반복자 API */

#include "blk.h" /* 블록 계층 내부 전용 선언(kmalloc_flex, bio_add_max_vecs 등 비공개 헬퍼) */

/*
 * bio_map_data: NVMe passthrough 요청에서 사용자 버퍼를 복사/관리할 때 쓰이는
 *   메타데이터 구조체. bio->bi_private에 연결되며, 완료 시 사용자 공간으로의
 *   복사 여부나 페이지 회수 방식을 결정한다. 이러한 플래그들은 NVMe 컨트롤러가
 *   데이터를 담은 페이지가 블록 계층이 할당한 bounce 페이지인지, 사용자 페이지를 pin 한 것인지를
 *   구분하는 데 참조된다.
 */
struct bio_map_data {
	bool is_our_pages : 1;
	/* [한국어] 이 bio에 연결된 페이지가 block 계층이 bio_copy_user_iov()
	 * 안에서 alloc_page()로 직접 할당한 bounce page인지(true), 아니면
	 * 호출자가 미리 준비해 넘긴 map_data->pages 풀에서 가져온 페이지인지
	 * (false)를 구분하는 비트필드. 해제 방식이 달라지므로 반드시 구분해야 한다. 이 페이지들의
	 * 물리 주소를 가리키게 되므로, 이 값은 "누가 그 물리 메모리의 수명을
	 * 책임지는가"를 나타낸다.
	 * 설정자: bio_copy_user_iov()가 `bmd->is_our_pages = !map_data;`로
	 *   설정 — map_data가 NULL일 때(즉 caller가 페이지 풀을 넘기지 않을
	 *   때)만 true가 된다.
	 * 읽는 자: bio_uncopy_user()가 이 값이 true일 때만 bio_free_pages()를
	 *   호출해 bounce page를 커널에 반납한다. false면 map_data가 소유한
	 *   페이지이므로 여기서는 해제하지 않고 caller(SG_IO 등)가 관리한다.
	 * 값 범위: 0(false) 또는 1(true)뿐인 1비트 필드.
	 * 동기화: bio_map_data는 단일 bio/request에 종속되고, 매핑을 수행한
	 *   프로세스 컨텍스트에서 설정된 뒤 그 request의 완료 경로에서만
	 *   읽히므로 별도의 락이나 원자적 접근이 필요 없다. */

	bool is_null_mapped : 1;
	/* [한국어] 실제 데이터 전송이 없는 passthrough 명령(NVMe Flush나 Write Zeroes처럼
	 * 데이터 페이로드가 없는 명령(예: NVMe Flush, Identify 없는 admin 명령))을 위해
	 * map_data->null_mapped가 설정되어 들어온 경우를 그대로 옮겨 기록하는
	 * 플래그.
	 * 설정자: bio_copy_user_iov()에서
	 *   `bmd->is_null_mapped = (map_data && map_data->null_mapped);`로
	 *   계산되어 저장된다.
	 * 읽는 자: bio_uncopy_user()가 이 값이 true이면 사용자 공간 복사
	 *   (bio_copy_to_iter)와 페이지 해제 로직을 통째로 건너뛴다 — 애초에
	 *   전송된 데이터가 없기 때문이다.
	 * 값 범위: 0(일반 데이터 전송) 또는 1(null-mapped, 데이터 없음).
	 * 동기화: is_our_pages와 동일하게 단일 request 컨텍스트에서만
	 *   설정/조회되어 락이 필요 없다. */

	struct iov_iter iter;
	/* [한국어] 호출자가 넘긴 원본 iov_iter의 깊은 복사본. 구조체 자체뿐
	 * 아니라(iter_is_iovec()인 경우) 그 안의 iovec 배열까지 아래 iov[]로
	 * 복사해 두므로, 원본 iter가 호출 스택 프레임에 있어 함수 리턴 후
	 * 사라지더라도 이 사본은 I/O 완료 시점까지 안전하게 남는다.
	 * 설정자: bio_alloc_map_data()가 `bmd->iter = *data;`로 구조체를
	 *   복사한 뒤, iovec 기반이면 `bmd->iter.__iov = bmd->iov;`로 내부
	 *   포인터를 아래 iov[] 배열로 재조정한다.
	 * 읽는 자: bio_uncopy_user()가 READ 요청 완료 시
	 *   `bio_copy_to_iter(bio, bmd->iter)`를 호출해, bounce page에 담긴
	 *   NVMe READ 결과를 이 iter가 가리키는 사용자 버퍼로 복사해 되돌린다.
	 * 값 범위: 유효한 iov_iter(주로 ITER_IOVEC 타입). count/nr_segs 등은
	 *   원본 요청 크기를 그대로 반영한다.
	 * 동기화: 매핑을 수행한 프로세스와, 그 request의 언매핑을 수행하는
	 *   완료 경로만 순차적으로 접근하므로 락이 필요 없다. */

	struct iovec iov[];
	/* [한국어] 호출자의 iovec 배열을 깊은 복사해 담아 두는 가변 길이 배열
	 * (flexible array member, 구조체 끝에 이어 붙는 형태). 원본 iovec이
	 * 스택 등 짧은 수명 메모리에 있어도, 이 사본이 별도로 존재하는 한
	 * 완료 시점까지 원본 iovec 을 안전하게 참조할 수 있다.
	 * 설정자: bio_alloc_map_data()에서 kmalloc_flex(*bmd, iov,
	 *   data->nr_segs, gfp_mask)로 struct bio_map_data 본체와 함께 한
	 *   번에 할당되고, iter_is_iovec(data)일 때 memcpy()로 원본 iovec
	 *   배열 내용이 채워진다.
	 * 읽는 자: 이 배열을 직접 순회하는 코드는 없고, 위 iter.__iov가 이
	 *   배열을 가리키도록 재조정되어 있어 iov_iter API(copy_page_to_iter
	 *   등)를 통해 간접적으로 읽힌다.
	 * 값 범위: 원소 개수는 data->nr_segs개(UIO_MAXIOV 이하로 제한되어
	 *   bio_alloc_map_data()에서 검사됨).
	 * 동기화: bio_map_data 전체와 생명주기를 같이하며(할당~kfree까지)
	 *   별도 동기화 없이 단일 컨텍스트에서만 접근된다. */
};

/*
 * [한국어]
 * bio_alloc_map_data - iov_iter를 복사하기 위한 bio_map_data 할당
 *
 * @data: 원본 iov_iter(호출자의 스택 등에 있을 수 있는 짧은 수명의 반복자)
 * @gfp_mask: 페이지/슬랩 할당에 사용할 GFP 플래그 (호출자가 전달한 그대로)
 * @return: 성공 시 새로 할당된 struct bio_map_data 포인터, 실패 시 NULL
 *   (iovec 개수가 UIO_MAXIOV 초과이거나 메모리 부족). 호출자인
 *   bio_copy_user_iov()는 NULL이면 즉시 -ENOMEM으로 상위에 보고한다.
 *
 * bounce-buffer 경로(bio_copy_user_iov)에서 사용자가 전달한 iovec은 호출
 * 스택 프레임에 있을 수 있어 함수가 반환하면 사라진다. 하지만 이 매핑은
 * I/O가 완료될 때까지(즉 다른 시점에) 원본 iov 정보가 필요하므로, 이
 * 함수는 iov_iter와 그 내부 iovec 배열을 힙에 깊은 복사해 수명을 늘린다.
 * 동작은 (1) nr_segs가 UIO_MAXIOV를 넘으면 거부, (2) bio_map_data 본체와
 * iov[] 배열을 kmalloc_flex()로 한 번에 할당, (3) iov_iter 구조체를
 * 복사, (4) iovec 기반이면 iovec 배열까지 memcpy하고 __iov 포인터를
 * 새 배열로 재조정하는 순서로 진행한다.
 * 실행 컨텍스트: 매핑을 요청한 프로세스 컨텍스트에서 동기적으로 실행되며,
 * 슬립 가능한 GFP 플래그가 쓰이면 슬립할 수 있다. 재진입 문제는 없다
 * (지역 변수만 사용, 전역/공유 상태 접근 없음).
 * 호출자(caller): bio_copy_user_iov().
 * 피호출자(callee): kmalloc_flex(), memcpy(), iter_is_iovec(), iter_iov().
 * 에러 처리: nr_segs 초과 또는 kmalloc_flex 실패 시 NULL 반환 — 호출자가
 * -ENOMEM으로 변환해 상위(blk_rq_map_user_iov)에 전파한다.
 *
 * 호출 체인:
 *   blk_rq_map_user_iov -> bio_copy_user_iov -> [bio_alloc_map_data]
 *
 * NVMe 연결:
 *   이 메타데이터는 나중에 request에 연결된 bio->bi_private로 남아,
 *   nvme_completion 이후 blk_rq_unmap_user -> bio_uncopy_user에서
 *   사용자 공간으로 결과를 복사하거나 커널 페이지를 회수하는 데 쓰인다.
 */
static struct bio_map_data *bio_alloc_map_data(struct iov_iter *data,
					       gfp_t gfp_mask)
{
	struct bio_map_data *bmd; /* bio->bi_private로 붙어 NVMe CQ 완료 후 사용자 공간 복사/페이지 회수에 재사용됨 */

	if (data->nr_segs > UIO_MAXIOV) /* iovec 개수 상한(UIO_MAXIOV)은 VFS 공통 제약이지 NVMe 와는 무관하다. 여기서는 아래 kmalloc 크기가 폭주하는 것을 막는 위생 검사로 쓰인다 */
		return NULL;

	bmd = kmalloc_flex(*bmd, iov, data->nr_segs, gfp_mask); /* bio_map_data + iov[] 가변 길이 할당; 이 메모리는 NVMe I/O 완료 시까지 생존 */
	if (!bmd) /* 할당 실패. 이 시점에는 아직 bio 도 만들지 않았으므로 되돌릴 것이 없다 — 그냥 -ENOMEM 으로 나가면 된다 */
		return NULL;
	bmd->iter = *data; /* 원본 iov_iter를 복사해 놓아 NVMe READ 완료 후 사용자 공간으로 데이터를 되돌릴 수 있게 함 */
	if (iter_is_iovec(data)) { /* iovec 반복자인 경우에만 복사본 저장 */
		memcpy(bmd->iov, iter_iov(data), sizeof(struct iovec) * data->nr_segs); /* 수명이 짧은 호출자 iovec 을 커널 힙에 깊은 복사한다. 언매핑 시점(완료 후)까지 원본 목적지 주소를 알아야 하는데, 그때 호출자 스택은 이미 사라졌을 수 있다. 원래 주석: NVMe PRP/SGL 후보 목록을 안전하게 보관 */
		bmd->iter.__iov = bmd->iov; /* 복사된 커널 내 iov 배열을 가리키도록 교체 */
	}
	return bmd; /* 이 포인터는 이후 bio->bi_private에 저장되어 NVMe completion path로 전달됨 */
}

/*
 * [한국어]
 * blk_mq_map_bio_put - 매핑 중 실패하거나 완료된 bio를 해제
 *
 * @bio: 해제할 bio (이미 할당되어 참조 카운트를 쥐고 있는 상태)
 * @return: 없음(void). 부수효과로 bio->__bi_cnt가 감소하고 0이 되면 bio가
 *   실제로 fs_bio_set mempool로 반환된다.
 *
 * 이 파일의 여러 매핑 함수(blk_rq_map_bio_alloc으로 할당한 bio들)가 실패
 * 경로나 완료 경로에서 반복적으로 bio_put()을 호출해야 하므로, 이름 있는
 * 래퍼로 감싸 호출부의 의도를 명확히 한다(단순 감싸기지만 "이 bio는 매핑
 * 실패/완료로 인해 정리된다"는 의미를 코드에 남긴다). 실행 컨텍스트는
 * 호출자의 컨텍스트를 그대로 따르며(프로세스 컨텍스트 실패 경로, 또는
 * bio 완료 콜백이 실행되는 인터럽트/softirq 컨텍스트 모두 가능),
 * bio_put() 자체가 참조 카운트 기반이라 재진입/동시 호출에 안전하다.
 * 호출자(caller): bio_copy_user_iov/bio_map_user_iov의 실패 경로,
 * blk_rq_map_user_bvec, bio_map_kern_endio, bio_copy_kern_endio,
 * blk_rq_unmap_user, blk_rq_map_kern 등 이 파일의 거의 모든 실패/완료 경로.
 * 피호출자(callee): bio_put().
 * 에러 처리: 반환값이 없어 실패 개념 자체가 없다.
 *
 * 호출 체인:
 *   <다양한 실패/완료 경로> -> [blk_mq_map_bio_put] -> bio_put
 *
 * NVMe 연결:
 *   매핑 단계의 실패는 요청이 아직 장치로 내려가기 전이므로, bio 를 되돌리는 것만으로 완전히 복구된다.
 */
static inline void blk_mq_map_bio_put(struct bio *bio)
{
	bio_put(bio); /* bio 참조 카운트 감소; 0 이 되면 fs_bio_set mempool 로 돌아가 다음 매핑에 재사용된다. 실패 경로에서도 메모리가 새지 않는 이유가 이 mempool 반환이다. 원래 주석: 재사용 가능 */
}

/*
 * [한국어]
 * blk_rq_map_bio_alloc - request의 cmd_flags를 상속받아 bio 할당
 *
 * @rq: 대상 request (아직 bio가 연결되지 않았거나 이어붙일 request)
 * @nr_vecs: bio_vec 배열에 미리 확보할 최대 세그먼트 수(0이면 bvec을
 *   나중에 직접 설정하는 경우, 예: blk_rq_map_user_bvec)
 * @gfp_mask: bio 자체(및 내부 bio_vec 배열) 할당에 쓸 GFP 플래그
 * @return: 성공 시 새로 할당된 struct bio *, 실패 시 NULL(메모리 부족).
 *   호출자들은 NULL이면 각자 -ENOMEM으로 변환해 상위로 전파한다.
 *
 * 이 파일의 모든 매핑 경로(zero-copy, bounce-copy, 커널 버퍼 매핑)가
 * 공통적으로 "request와 같은 방향(READ/WRITE)의 bio를 fs_bio_set에서
 * 할당"해야 하므로, 그 공통 로직을 한 곳에 모은 헬퍼다. rq->q->disk가
 * 있으면 그 partition 0(전체 디스크)을 bdev로 사용해 bio_alloc_bioset을
 * 호출하고, rq->cmd_flags를 그대로 bio의 op/flags로 넘겨 방향을 상속한다.
 * 실행 컨텍스트: 매핑을 요청한 프로세스 컨텍스트에서 동기 호출되며,
 * fs_bio_set은 멀티 큐 환경에서 각 CPU/요청이 동시에 호출해도 안전한
 * mempool 기반 bioset이므로 별도 락 없이 재진입 가능하다.
 * 호출자(caller): bio_copy_user_iov, bio_map_user_iov, bio_map_kern,
 * bio_copy_kern, blk_rq_map_user_bvec.
 * 피호출자(callee): bio_alloc_bioset().
 * 에러 처리: 할당 실패 시 NULL을 그대로 반환하며 별도 정리 작업은 없다
 * (아직 아무 자원도 잡지 않은 상태이기 때문).
 *
 * 호출 체인:
 *   bio_copy_user_iov | bio_map_user_iov | bio_map_kern | bio_copy_kern
 *     -> [blk_rq_map_bio_alloc] -> bio_alloc_bioset
 *
 * NVMe 연결:
 *   bio의 cmd_flags는 NVMe 명령의 opcode 방향(READ/WRITE)을 반영하며,
 *   이 bio 의 bvec 이 나중에 드라이버에서 DMA 매핑되고, NVMe PCIe 라면 nvme_pci_setup_data_prp/sgl 이 그것을 PRP 또는 SGL 로 기술한다.
 */
static struct bio *blk_rq_map_bio_alloc(struct request *rq,
		unsigned int nr_vecs, gfp_t gfp_mask)
{
	struct block_device *bdev = rq->q->disk ? rq->q->disk->part0 : NULL; /* request가 속한 gendisk에서 block_device를 가져옴; NVMe는 nvme0n1 등의 디스크에 대응 */
	struct bio *bio; /* 새로 할당될 bio를 담을 지역 변수; 아래에서 bio_alloc_bioset 결과로 채워짐 */

	bio = bio_alloc_bioset(bdev, nr_vecs, rq->cmd_flags, gfp_mask, /* cmd_flags에 NVMe READ/WRITE 방향이 담김 */
				&fs_bio_set); /* fs_bio_set mempool에서 bio 할당; NVMe multi-queue 경쟁 상황에서도 메모리 고갈 방지 */
	if (!bio) /* bio 할당 실패 — 아직 request 에 아무것도 연결하지 않았으므로 bmd 만 해제하면 된다 */
		return NULL;

	return bio; /* 할당된 bio 는 (page, offset, len) 삼중항의 배열을 담게 된다. 이 삼중항이 곧 DMA 매핑의 단위다 */
}

/**
 * bio_copy_from_iter - copy all pages from iov_iter to bio
 * @bio: The &struct bio which describes the I/O as destination
 * @iter: iov_iter as source
 *
 * Copy all pages from iov_iter to bio.
 * Returns 0 on success, or error on failure.
 */
/*
 * [한국어]
 * bio_copy_from_iter - 사용자 공간 데이터를 bio의 커널 페이지로 복사
 *
 * @bio: 복사 대상 bio (bio_copy_user_iov가 bounce page들을 채워 넣은 상태)
 * @iter: 사용자 공간 소스 iov_iter (WRITE로 보낼 원본 데이터)
 * @return: 0(모든 segment 복사 성공) 또는 -EFAULT(사용자 페이지 폴트 등으로
 *   요청한 만큼 복사되지 못함). 호출자 bio_copy_user_iov()는 실패 시 cleanup
 *   경로로 분기해 이미 채운 bio를 정리한다.
 *
 * WRITE 방향 passthrough 명령은 NVMe 컨트롤러가 DMA로 읽어갈 데이터가
 * 커널 메모리(bounce page)에 실제로 존재해야 하므로, SQ 제출 전에 반드시
 * 사용자 공간 데이터를 커널 페이지로 복사해 두어야 한다. 동작은 bio의 모든
 * bio_vec을 순회하며 각 세그먼트의 page/offset/len 위치로 copy_page_from_iter()
 * 를 호출하고, iter에 남은 데이터가 없으면 종료, 요청보다 적게 복사되면
 * 즉시 -EFAULT로 중단하는 순서다.
 * 실행 컨텍스트: 매핑을 요청한 프로세스 컨텍스트에서 실행되며, 사용자
 * 주소 공간(iter)에 접근하므로 반드시 그 프로세스의 mm 컨텍스트여야 한다.
 * 동시성 문제는 없다(단일 bio를 단일 스레드가 순회).
 * 호출자(caller): bio_copy_user_iov() (WRITE 및 SG_DXFER_TO_FROM_DEV 경로).
 * 피호출자(callee): bio_for_each_segment_all(), copy_page_from_iter(),
 * iov_iter_count().
 * 에러 처리: 세그먼트 하나라도 완전히 복사되지 않으면 -EFAULT를 반환하고
 * 나머지 세그먼트는 처리하지 않는다.
 *
 * 호출 체인:
 *   blk_rq_map_user_iov -> bio_copy_user_iov -> [bio_copy_from_iter]
 *
 * NVMe 연결:
 *   쓰기(WRITE) 경로에서 요청이 장치로 내려가기 전에 데이터가 커널 메모리에
 *   존재해야 하므로, 장치가 데이터를 읽어 가기 전인 이 단계에서 복사가 끝나야
 *   DMA 매핑이 안정적이다.
 */
static int bio_copy_from_iter(struct bio *bio, struct iov_iter *iter)
{
	struct bio_vec *bvec; /* 현재 처리 중인 bio_vec; NVMe PRP/SGL의 한 entry에 대응하는 page/offset/len */
	struct bvec_iter_all iter_all; /* bio의 모든 bvec을 순회하기 위한 내부 iterator */

	bio_for_each_segment_all(bvec, bio, iter_all) { /* bio의 모든 segment를 순회하며 사용자 데이터를 페이지에 복사 */
		ssize_t ret; /* 이번 segment에 대해 실제로 복사된 바이트 수(음수면 오류) */

		ret = copy_page_from_iter(bvec->bv_page, /* 커널 bounce page에 사용자 데이터를 복사; 이 페이지가 NVMe DMA/PRP 소스가 됨 */
					  bvec->bv_offset,
					  bvec->bv_len,
					  iter);

		if (!iov_iter_count(iter)) /* 사용자 공간에 남은 데이터가 없으면 복사 종료; NVMe WRITE 데이터 준비 완료 */
			break;

		if (ret < bvec->bv_len) /* 요청보다 적게 복사됐다면 사용자 포인터가 잘못된 것 — -EFAULT. 아직 아무것도 제출하지 않았으므로 취소할 것도 없다 */
			return -EFAULT;
	}

	return 0; /* 모든 세그먼트 복사 성공. 이제 bio 의 페이지들은 커널 소유이며 물리적으로 고정되어 있어 DMA 매핑이 가능하다 */
}

/**
 * bio_copy_to_iter - copy all pages from bio to iov_iter
 * @bio: The &struct bio which describes the I/O as source
 * @iter: iov_iter as destination
 *
 * Copy all pages from bio to iov_iter.
 * Returns 0 on success, or error on failure.
 */
/*
 * [한국어]
 * bio_copy_to_iter - bio의 커널 페이지 데이터를 사용자 공간으로 복사
 *
 * @bio: 소스 bio (NVMe READ 완료로 bounce page가 채워진 상태)
 * @iter: 사용자 공간 대상 iov_iter (bio_map_data->iter의 값 복사본이므로
 *   함수 내부에서 진행시켜도 호출자의 bmd->iter는 영향받지 않는다)
 * @return: 0(모든 segment 복사 성공) 또는 -EFAULT(사용자 버퍼 접근 실패 등
 *   으로 요청보다 적게 복사됨). 호출자 bio_uncopy_user()는 이 값을 그대로
 *   자신의 반환값으로 전달해 SG_IO 계층까지 오류가 보고되게 한다.
 *
 * READ passthrough 명령은 NVMe 컨트롤러가 DMA로 채운 데이터가 bounce
 * page(커널 메모리)에 있으므로, 완료 시점에 이를 원래 요청자의 사용자
 * 버퍼로 복사해 돌려줘야 한다. 동작은 bio의 모든 bio_vec을 순회하며 각
 * 세그먼트를 copy_page_to_iter()로 사용자 iter에 복사하고, iter가 소진되면
 * 종료, 부족하게 복사되면 -EFAULT로 중단하는 순서다.
 * 실행 컨텍스트: I/O 완료 후 blk_rq_unmap_user()가 호출되는 시점의
 * 컨텍스트(일반적으로 완료를 대기하던 프로세스 컨텍스트)에서 실행되며,
 * 사용자 주소 공간 접근이 필요하므로 반드시 그 프로세스의 mm이 살아있어야
 * 한다(bio_uncopy_user가 current->mm을 미리 확인).
 * 호출자(caller): bio_uncopy_user() (READ 방향일 때만).
 * 피호출자(callee): bio_for_each_segment_all(), copy_page_to_iter(),
 * iov_iter_count().
 * 에러 처리: 세그먼트 하나라도 부족하게 복사되면 -EFAULT 반환, 나머지
 * 세그먼트는 처리하지 않는다.
 *
 * 호출 체인:
 *   nvme completion -> blk_mq_end_request -> blk_rq_unmap_user
 *     -> bio_uncopy_user -> [bio_copy_to_iter]
 *
 * NVMe 연결:
 *   NVMe 컨트롤러가 CQ에 완료 정보를 기록한 후, 상위 계층으로 데이터를
 *   전달하기 전에 수행된다.
 */
static int bio_copy_to_iter(struct bio *bio, struct iov_iter iter)
{
	struct bio_vec *bvec; /* NVMe READ로부터 채워진 커널 bounce page를 가리키는 bio_vec */
	struct bvec_iter_all iter_all; /* 모든 bvec 순회용 내부 상태 */

	bio_for_each_segment_all(bvec, bio, iter_all) { /* NVMe CQ entry 처리 후 사용자 공간으로 결과를 전달하는 루프 */
		ssize_t ret; /* 이번 segment에 대해 실제로 복사된 바이트 수(음수면 오류) */

		ret = copy_page_to_iter(bvec->bv_page, /* bio 페이지 -> 사용자 공간 복사; NVMe READ 완료 후 데이터 전달 */
					bvec->bv_offset,
					bvec->bv_len,
					&iter);

		if (!iov_iter_count(&iter)) /* 사용자 공간에 모두 복사되면 종료; NVMe READ 결과가 상위 계층에 도달 완료 */
			break;

		if (ret < bvec->bv_len) /* READ 완료 후 사용자 공간 복사 실패; 상위 SG_IO는 -EFAULT 수신 */
			return -EFAULT;
	}

	return 0; /* NVMe READ 결과가 사용자 버퍼로 모두 전달됨; 이제 bio_map_data 해제 가능 */
}

/**
 *	bio_uncopy_user	-	finish previously mapped bio
 *	@bio: bio being terminated
 *
 *	Free pages allocated from bio_copy_user_iov() and write back data
 *	to user space in case of a read.
 */
/*
 * [한국어]
 * bio_uncopy_user - bio_map_data가 붙은 bio의 매핑을 해제
 *
 * @bio: 종료 중인 bio (bio->bi_private에 bio_copy_user_iov가 붙여 놓은
 *   struct bio_map_data가 유효하다고 가정)
 * @return: 0(정상 처리) 또는 음수 오류 코드(-EINTR: orphan된 request라서
 *   사용자 공간 복사를 건너뜀, -EFAULT: bio_copy_to_iter 복사 실패).
 *   호출자 blk_rq_unmap_user()는 이 값을 첫 오류로만 누적해 최종 반환한다.
 *
 * bio_copy_user_iov()가 만든 bounce-buffer bio는 완료 시점에 (1) READ라면
 * bounce page의 데이터를 원래 사용자 iov_iter로 복사하고, (2) block 계층이
 * 직접 할당한 페이지라면 그 페이지들을 회수해야 한다. 이 함수가 그 두 단계와
 * bio_map_data 자체의 kfree까지 담당한다. null-mapped(데이터 없는 명령)면
 * 복사/해제를 모두 건너뛴다. 완료 시점에 request가 이미 orphan(워크큐 등에서
 * 비동기로 완료되어 원래 프로세스 컨텍스트가 아님)되었으면 current->mm이
 * 없으므로, 엉뚱한 주소 공간에 쓰지 않도록 -EINTR로 처리하고 데이터는
 * 폐기한다.
 * 실행 컨텍스트: blk_rq_unmap_user()를 호출한 컨텍스트를 그대로 따르며,
 * I/O 완료 후 정리 단계이므로 일반적으로 프로세스 컨텍스트지만 workqueue
 * 컨텍스트일 수도 있다(그 경우 current->mm 검사로 안전하게 처리).
 * 호출자(caller): blk_rq_unmap_user().
 * 피호출자(callee): bio_copy_to_iter(), bio_free_pages(), kfree().
 * 에러 처리: -EINTR/-EFAULT를 반환하되, bio_map_data는 어떤 경우든 항상
 * kfree되어 메모리 누수는 없다.
 *
 * 호출 체인:
 *   nvme completion -> blk_mq_end_request -> blk_rq_unmap_user
 *     -> [bio_uncopy_user] -> bio_copy_to_iter
 *
 * NVMe 연결:
 *   NVMe I/O 완료(CQ entry 처리) 후 request 해제 단계에서 실행되며,
 *   READ 결과가 사용자 공간에 복사되고 PRP/SGL에 쓰인 커널 페이지가 반납된다.
 */
static int bio_uncopy_user(struct bio *bio)
{
	struct bio_map_data *bmd = bio->bi_private; /* [한국어] 제출 때 bio->bi_private 에 매달아 둔 원본 iovec 정보. 완료 시점에는
					 * 호출자 스택이 사라졌을 수 있어, 사용자 버퍼 주소를 되찾는 유일한 수단이다 (원래 주석: 복사/해제 정책 결정 */
	int ret = 0; /* 기본값 0(성공); null-mapped/READ 아님 등 별도 처리가 없으면 그대로 반환됨 */

	if (!bmd->is_null_mapped) { /* [한국어] null 매핑은 "데이터 버퍼 없음"을 뜻한다. 되돌릴 복사도 해제할 페이지도 없다 (원래 주석: 없는 명령 */
		/*
		 * if we're in a workqueue, the request is orphaned, so
		 * don't copy into a random user address space, just free
		 * and return -EINTR so user space doesn't expect any data.
		 */
		if (!current->mm)	/* [한국어] 커널 스레드에는 사용자 주소 공간이 없다 */ /* [한국어] 이 함수가 워커에서 불릴 수 있다는 것이 핵심이다(요청을 낸 프로세스가 이미
				 * 죽었거나 다른 문맥일 수 있다). current->mm 이 없으면 "사용자 주소 공간"이라는
				 * 개념 자체가 없으므로, 아무 주소에나 쓰지 말고 -EINTR 로 물러난다.
				 * 위 영문 주석이 말하는 orphaned request 가 이 경우다. */
			ret = -EINTR;	/* [한국어] "중단됨"으로 알린다. 데이터는 버려지지만 아래 페이지 해제는 그대로 수행된다 */
		else if (bio_data_dir(bio) == READ) /* [한국어] 읽기였을 때만 되돌린다. 쓰기는 제출 전에 이미 커널로 복사했으므로 할 일이 없다 */
			ret = bio_copy_to_iter(bio, bmd->iter);
		if (bmd->is_our_pages) /* [한국어] 우리가 할당한 bounce 페이지만 해제한다. map_data 로 호출자가 준 페이지라면
					 * 그쪽 소유이므로 건드리면 안 된다 (원래 주석: 모리 반납 */
			bio_free_pages(bio);
	}
	kfree(bmd); /* [한국어] 매핑 컨텍스트 해제 — 여기까지 오면 이 bio 에 대한 정리가 끝난다 */
	return ret; /* [한국어] 호출자는 bio 체인을 돌며 이 함수를 여러 번 부르는데, 첫 에러만 기록하고
		 * 나머지 bio 도 끝까지 정리한다 — 중간에 멈추면 페이지가 샌다 */
}

/*
 * [한국어]
 * bio_copy_user_iov - 사용자 버퍼를 bounce buffer bio로 구성
 *
 * @rq: 대상 request (완성된 bio가 blk_rq_append_bio를 통해 연결될 대상)
 * @map_data: 호출자가 미리 페이지를 준비해 온 경우의 페이지 풀(rq_map_data);
 *   NULL이면 이 함수가 직접 alloc_page()로 bounce page를 할당한다
 * @iter: 사용자(또는 커널) 버퍼를 가리키는 iov_iter — 진행(advance)되며
 *   소비된다
 * @gfp_mask: bio/페이지 할당에 쓸 GFP 플래그
 * @return: 0(성공, bio가 rq에 연결됨) 또는 음수 오류 코드(-ENOMEM: 메타데이터/
 *   페이지/bio 할당 실패, -EFAULT: 사용자 데이터 복사 실패, 그 외
 *   blk_rq_append_bio가 반환하는 코드). 호출자 blk_rq_map_user_iov()는
 *   실패 시 unmap_rq 경로로 분기한다.
 *
 * DMA 정렬 요구사항이나 IOMMU 가상 경계 제약으로 사용자 페이지를 그대로
 * pin해서 쓸 수 없는 경우, 이 함수는 커널이 새로 할당(또는 map_data가 미리
 * 준비)한 페이지로 구성된 bio를 만들고 WRITE 데이터는 그 페이지로 복사해
 * 넣는다(READ는 완료 시점에 bio_uncopy_user가 되돌려준다). 동작 순서는
 * (1) bio_alloc_map_data로 언매핑용 메타데이터 준비, (2) 필요한 페이지 수만큼
 * bio 할당, (3) map_data가 있으면 그 풀에서, 없으면 alloc_page()로 페이지를
 * 확보해 반복적으로 bio_add_page, (4) WRITE면 bio_copy_from_iter로 사용자
 * 데이터를 커널 페이지에 복사(SG_DXFER_TO_FROM_DEV 양방향이면 별도 복사),
 * (5) 완성된 bio를 blk_rq_append_bio로 request에 연결하는 순서다.
 * 실행 컨텍스트: 매핑을 요청한 프로세스 컨텍스트에서 슬립 가능하게 실행된다
 * (alloc_page/GFP_NOIO 등). 동시성 문제는 없다(단일 request/단일 스레드).
 * 호출자(caller): blk_rq_map_user_iov() (copy 경로로 결정되었을 때, 남은
 * iov_iter가 있는 한 반복 호출될 수 있다).
 * 피호출자(callee): bio_alloc_map_data, blk_rq_map_bio_alloc, alloc_page,
 * bio_add_page, bio_copy_from_iter, zero_fill_bio, blk_rq_append_bio.
 * 에러 처리: 각 단계 실패 시 cleanup/out_bmd 레이블로 goto하여 이미 확보한
 * bio/페이지/메타데이터를 역순으로 정리한 뒤 오류를 반환한다.
 *
 * 호출 체인:
 *   blk_rq_map_user_iov -> [bio_copy_user_iov]
 *     -> blk_rq_map_bio_alloc | bio_copy_from_iter | blk_rq_append_bio
 *
 * NVMe 연결:
 *   NVMe PRP는 PRP2 이후 엔트리가 페이지 오프셋 0에서 시작해야 하므로,
 *   정렬되지 않은 사용자
 *   버퍼는 이 함수에서 커널 페이지로 정리된 후 PRP list/SGL로 변환된다.
 */
static int bio_copy_user_iov(struct request *rq, struct rq_map_data *map_data,
		struct iov_iter *iter, gfp_t gfp_mask)
{
	struct bio_map_data *bmd; /* NVMe READ 완료 후 사용자 공간 복사 여부를 결정하는 메타데이터 */
	struct page *page; /* bio에 추가될 페이지; NVMe PRP/SGL entry의 물리 페이지 후보 */
	struct bio *bio; /* 조립 중인 bio. 완성되면 blk_rq_append_bio 로 request 에 붙는다 */
	int i = 0, ret; /* i: map_data 풀 내 페이지 인덱스; ret: NVMe 명령 생성 성공/실패 상태 */
	int nr_pages; /* 필요한 페이지/segment 수; NVMe PRP entry 개수 상한을 초과하지 않도록 조절 */
	unsigned int len = iter->count; /* 전송할 총 바이트 수; NVMe 명령의 데이터 길이(NLBA*sector_size)가 됨 */
	unsigned int offset = map_data ? offset_in_page(map_data->offset) : 0; /* map_data가 주어지면 페이지 내 오프셋 사용; NVMe PRP 제약에 맞추기 위한 오프셋 */

	bmd = bio_alloc_map_data(iter, gfp_mask); /* 메타데이터 할당 실패 시 -ENOMEM */
	if (!bmd) /* bio_map_data 할당 실패 — 아직 아무것도 연결하지 않았으므로 그대로 반환 */
		return -ENOMEM;

	/*
	 * We need to do a deep copy of the iov_iter including the iovecs.
	 * The caller provided iov might point to an on-stack or otherwise
	 * shortlived one.
	 */
	bmd->is_our_pages = !map_data; /* map_data가 없을 때만 페이지 소유권을 가짐; 소유권에 따라 완료 시 페이지 회수 여부 결정 */
	bmd->is_null_mapped = (map_data && map_data->null_mapped); /* null mapped 명령(예: NVMe Flush/Write Zeroes) 플래그 기록; 데이터 길이 0으로 SQ에 삽입 */

	nr_pages = bio_max_segs(DIV_ROUND_UP(offset + len, PAGE_SIZE)); /* 필요한 최대 segment 수; NVMe PRP/SGL entry 수 상한과 관련 */

	ret = -ENOMEM; /* 이후 실패 경로(goto out_bmd/cleanup)의 기본 반환값을 -ENOMEM으로 선점 */
	bio = blk_rq_map_bio_alloc(rq, nr_pages, gfp_mask); /* rq->cmd_flags를 상속받아 NVMe READ/WRITE 방향이 설정된 bio 할당 */
	if (!bio) /* bio 할당 실패 - 위에서 이미 -ENOMEM으로 설정된 ret을 그대로 사용 */
		goto out_bmd; /* bio_map_data만 kfree하고 반환; bio 자체는 없으므로 bio 해제 불필요 */

	if (map_data) { /* 미리 할당된 페이지 풀(map_data)을 사용하는 경우; SG_IO에서 미리 잡아 둔 페이지 활용 */
		nr_pages = 1U << map_data->page_order; /* page_order에 따른 페이지 묶음 크기 */
		i = map_data->offset / PAGE_SIZE; /* offset에서 시작 페이지 인덱스 계산 */
	}
	while (len) { /* 남은 데이터를 페이지 단위로 분할하여 bio에 추가; 한 반복이 최대 한 페이지를 처리한다 하나를 생성 */
		unsigned int bytes = PAGE_SIZE; /* 이번 페이지에서 쓸 수 있는 최대 바이트 수(한 페이지 전체)로 초기화 */

		bytes -= offset; /* 현재 페이지의 시작 오프셋만큼 usable byte 감소 */

		if (bytes > len) /* 남은 길이보다 크면 잘라 맞춘다 — 마지막 페이지는 부분만 쓰인다 */
			bytes = len;

		if (map_data) { /* map_data 페이지 풀에서 다음 페이지를 가져오는 분기 */
			if (i == map_data->nr_entries * nr_pages) { /* 페이지 풀의 끝에 도달하면 실패 */
				ret = -ENOMEM; /* 페이지 확보 실패 */
				goto cleanup; /* cleanup 라벨로 — 지금까지 bio 에 붙인 페이지를 전부 되돌린다 */
			}

			page = map_data->pages[i / nr_pages]; /* map_data 페이지 배열에서 해당 페이지 획득 */
			page += (i % nr_pages); /* page_order 묶음 내 페이지 오프셋 적용 */

			i++; /* 다음 반복을 위해 map_data 풀 인덱스 전진 */
		} else { /* map_data가 없으면 매번 새 bounce page를 직접 할당 */
			page = alloc_page(GFP_NOIO | gfp_mask); /* bounce 버퍼용 커널 페이지 할당. GFP_NOIO 를 강제하는 이유가 중요하다 — 이 경로는 이미 블록 IO 를 처리하는 중이므로, 여기서 메모리 회수가 다시 블록 IO 를 일으키면 자기 자신을 기다리는 교착이 된다. 원래 주석: NVMe DMA에 사용할 커널 메모리 */
			if (!page) { /* 페이지 할당 실패 - 지금까지 추가한 페이지는 cleanup에서 일괄 해제 */
				ret = -ENOMEM; /* 페이지 확보 실패 — 아래 cleanup 라벨에서 지금까지 잡은 페이지를 전부 되돌린다 */
				goto cleanup;
			}
		}

		if (bio_add_page(bio, page, bytes, offset) < bytes) { /* bio 에 (page, offset, len) 삼중항 하나를 추가.
								 * 반환값이 bytes 보다 작다는 것은 bio 의 bvec 배열이 꽉 찼다는 뜻이다
								 * (BIO_MAX_VECS 또는 이 bio 에 할당한 nr_pages 상한). */
			if (!map_data) /* 추가 실패 시 직접 할당한 페이지만 해제; map_data 페이지는 caller가 관리 */
				__free_page(page);	/* [한국어] 이 페이지는 bio 에 붙지 못했으므로 아래 bio_free_pages 가 회수해 주지 않는다 */
			break; /* [한국어] 에러가 아니다. 지금까지 담은 만큼만으로 bio 하나를 완성하고,
			    * 호출자(blk_rq_map_user_iov)의 루프가 나머지를 다음 bio 로 이어 붙인다.
			    * 하나의 iov_iter 가 여러 bio 로 쪼개지는 것이 정상 동작이다. */
		}

		len -= bytes; /* 남은 길이 감소 */
		offset = 0;   /* [한국어] 오프셋은 첫 페이지에서만 의미가 있다. 사용자 버퍼가 페이지 중간에서
			   * 시작할 수 있어 첫 페이지만 앞이 잘리고, 이후 페이지들은 항상 0 부터 쓴다. */
	}

	if (map_data) /* map_data를 쓴 경우에만 다음 호출을 위해 진행 오프셋을 갱신 */
		map_data->offset += bio->bi_iter.bi_size; /* map_data 사용 시 다음 offset 갱신; 연속된 SG_IO 요청 간 상태 유지 */

	/*
	 * success
	 */
	if (iov_iter_rw(iter) == WRITE && /* WRITE이고 null mapped가 아니면 사용자 데이터를 bio로 복사 */
	     (!map_data || !map_data->null_mapped)) {
		ret = bio_copy_from_iter(bio, iter); /* 쓰기라면 지금 사용자 데이터를 bounce 페이지로 옮겨 둔다. 장치는 나중에 이 페이지에서 읽어 가므로 제출 전에 반드시 채워져 있어야 한다. 원래 주석: DMA 일관성 확보 */
		if (ret) /* 사용자 데이터 복사 실패(-EFAULT) - 이미 구성된 bio/페이지를 정리해야 함 */
			goto cleanup;
	} else if (map_data && map_data->from_user) { /* SG_DXFER_TO_FROM_DEV 양방향 복사 처리 */
		struct iov_iter iter2 = *iter; /* WRITE 판단과 무관하게 별도 복사본으로 copy-in 수행 */

		/* This is the copy-in part of SG_DXFER_TO_FROM_DEV. */
		iter2.data_source = ITER_SOURCE; /* 데이터 소스로 설정하여 bio로 복사 */
		ret = bio_copy_from_iter(bio, &iter2); /* 양방향 전송의 copy-in 단계: 사용자 데이터를 bounce page에 선복사 */
		if (ret) /* copy-in 실패 - 이미 구성된 bio/페이지를 정리해야 함 */
			goto cleanup;
	} else { /* READ이거나(그리고 null-mapped가 아니거나) 관리용 명령: 사용자 -> bio 복사가 필요 없는 경우 */
		if (bmd->is_our_pages) /* [한국어] 우리가 할당한 페이지라면 반드시 0 으로 밀어야 한다.
				    * alloc_page() 는 이전 소유자의 내용이 남은 페이지를 줄 수 있고,
				    * 읽기 명령이 그 전부를 덮어쓰지 않으면 남은 부분이 사용자 공간으로
				    * 새어 나간다. 성능이 아니라 정보 유출 방지를 위한 코드다. */
			zero_fill_bio(bio);
		iov_iter_advance(iter, bio->bi_iter.bi_size); /* 사용자 반복자를 진행시켜 이미 처리한 길이만큼 걸너뜀 */
	}

	bio->bi_private = bmd; /* bio 에 bmd 를 매달아 둔다. 완료 시점에 bio_uncopy_user() 가 이 포인터로 원본 iovec 을 되찾아 읽기 데이터를 사용자 공간에 되돌린다 — 그때 호출자 스택은 이미 사라졌을 수 있으므로 이 연결이 유일한 경로다 */

	ret = blk_rq_append_bio(rq, bio); /* 구성된 bio를 request에 연결; 이후 blk_mq 제출 단계로 진행 */
	if (ret)
		goto cleanup;
	return 0; /* bio 가 request 에 연결됐다. 호출자는 이제 blk_execute_rq 등으로 이 요청을 제출할 수 있다 */
cleanup:
	if (!map_data) /* 실패 시 직접 할당한 bounce 페이지 해제 */
		bio_free_pages(bio);
	blk_mq_map_bio_put(bio); /* bio 반환. 아직 request 에 연결하지 않았으므로 이것으로 완전히 되돌아간다 */
out_bmd:
	kfree(bmd); /* bio_map_data 해제 */
	return ret; /* 음수 오류 코드; 호출자 blk_rq_map_user_iov가 unmap_rq 경로로 처리 */
}

/*
 * [한국어]
 * bio_map_user_iov - 사용자 페이지를 직접 bio에 매핑(제로 카피)
 *
 * @rq: 대상 request
 * @iter: 사용자 버퍼 기술자(핀될 페이지들을 가리키는 iov_iter)
 * @gfp_mask: bio 할당에 쓸 GFP 플래그
 * @return: 0(성공, bio가 rq에 연결됨) 또는 음수 오류(-EINVAL: 길이 0,
 *   -ENOMEM: bio 할당 실패, 그 외 bio_iov_iter_get_pages/blk_rq_append_bio가
 *   반환하는 코드, 특히 -EREMOTEIO는 상위에서 복사 경로로 폴백하는 신호로
 *   쓰인다).
 *
 * DMA 정렬이나 IOMMU 가상 경계 제약을 만족하는 사용자 버퍼는 굳이 커널
 * 페이지로 복사할 필요 없이, 사용자 페이지 자체를 pin(get_user_pages류)
 * 하여 bio에 직접 연결하면 데이터 복사 오버헤드를 없앨 수 있다(제로카피).
 * 동작은 (1) 필요한 bio_vec 수를 iov_iter_npages로 계산해 bio 할당,
 * (2) bio_iov_iter_get_pages로 사용자 페이지를 pin하며 bio에 추가,
 * (3) blk_rq_append_bio로 request에 연결하는 순서다.
 * 실행 컨텍스트: 매핑을 요청한 프로세스 컨텍스트에서 실행되며, 페이지
 * pin은 해당 프로세스의 mm에 대해 이루어진다. 동시성 문제는 없다.
 * 호출자(caller): blk_rq_map_user_iov() (zero-copy 경로로 결정되었을 때).
 * 피호출자(callee): iov_iter_npages, blk_rq_map_bio_alloc,
 * bio_iov_iter_get_pages, blk_rq_append_bio, bio_release_pages.
 * 에러 처리: get_pages 실패 시 out_put으로, append_bio 실패 시
 * out_release(핀 해제) 후 out_put(bio 반환)으로 순서대로 정리한다.
 *
 * 호출 체인:
 *   blk_rq_map_user_iov -> [bio_map_user_iov]
 *     -> bio_iov_iter_get_pages -> blk_rq_append_bio
 *
 * NVMe 연결:
 *   이 bio 의 페이지들은 뒤에 드라이버가 DMA 매핑하고, NVMe PCIe 라면 PRP 또는 SGL
 *   segment로 사용된다. 사용자 버퍼가 물리적으로 불연속이면 PRP list로,
 *   SGL을 지원하면(nvme_ctrl_sgl_supported) SGL 디스크립터로 기술된다.
 */
static int bio_map_user_iov(struct request *rq, struct iov_iter *iter,
		gfp_t gfp_mask)
{
	unsigned int nr_vecs = iov_iter_npages(iter, BIO_MAX_VECS); /* [한국어] 이 버퍼를 담으려면 bvec 이 몇 개 필요한지 미리 센다. BIO_MAX_VECS로 제한; NVMe PRP entry 수 상한과 연관 */
	struct bio *bio; /* pin된 사용자 페이지들을 담을 zero-copy bio */
	int ret; /* get_pages/append_bio 각 단계의 성공/실패 코드 */

	if (!iov_iter_count(iter)) /* [한국어] 길이 0 짜리 매핑은 의미가 없다. 데이터 없는 명령이라면 애초에 이 함수를 부르지 않는다 (원래 주석: 상 명령 거부 */
		return -EINVAL;

	bio = blk_rq_map_bio_alloc(rq, nr_vecs, gfp_mask); /* [한국어] bvec 슬롯만 있는 빈 bio 를 만든다. 실제 페이지는 다음 줄에서 채운다 (원래 주석: 로 직접 변환 */
	if (!bio) /* bio 할당 실패 - 아직 페이지를 pin하지 않았으므로 별도 해제 없이 바로 반환 */
		return -ENOMEM;
	/*
	 * No alignment requirements on our part to support arbitrary
	 * passthrough commands.
	 */
	ret = bio_iov_iter_get_pages(bio, iter, 0); /* [한국어] 사용자 페이지를 pin 해서 bvec 에 그대로 꽂는다. 복사가 없다는 것이 이 경로의 전부다.
						 * 세 번째 인자 0 은 "정렬 요구 없음" — 위 영문 주석대로 패스스루는 임의 명령을
						 * 지원해야 해서 블록 계층이 정렬을 강제하지 않는다 (원래 주석: VMe DMA 매핑 대상 */
	if (ret) /* pin 실패 - 아직 페이지가 없으므로 bio만 반환하면 됨 */
		goto out_put;
	ret = blk_rq_append_bio(rq, bio); /* [한국어] request 에 연결. 이 안에서 큐 한계 검사가 이뤄져 실패할 수 있다 */
	if (ret) /* append 실패(세그먼트/크기 제약 초과) - 이미 pin된 페이지도 함께 해제해야 함 */
		goto out_release;
	return 0; /* 제로 카피 성공 — 사용자 페이지를 그대로 pin 해서 썼다. 복사 비용이 0 인 대신, 완료까지 그 페이지들이 pin 된 채로 묶인다 */

out_release:
	bio_release_pages(bio, false); /* [한국어] append 가 실패했으므로 방금 pin 한 페이지들을 전부 되돌린다.
					 * false 는 "dirty 로 표시하지 말라" — 읽기가 일어나지 않았으니 내용이 바뀌지 않았다 (원래 주석: 않음 */
out_put:
	blk_mq_map_bio_put(bio); /* bio 반환 — 실패 경로 */
	return ret; /* 음수 오류 코드; 호출자 blk_rq_map_user_iov가 -EREMOTEIO를 -EINVAL로 변환하거나 unmap 처리 */
}

/*
 * [한국어]
 * bio_invalidate_vmalloc_pages - vmalloc 영역의 캐시 일관성 정리
 *
 * @bio: vmalloc 주소를 담은 bio (bio->bi_private에 원본 vmalloc 주소가 저장됨)
 * @return: 없음(void). ARCH_IMPLEMENTS_FLUSH_KERNEL_VMAP_RANGE가 정의되지
 *   않은 아키텍처에서는 컴파일 시점에 아무 동작도 하지 않는 빈 함수가 된다.
 *
 * 일부 아키텍처는 vmalloc으로 매핑된 커널 가상 주소가 CPU 캐시와 별도의
 * 별칭(alias)을 가질 수 있어, DMA 디바이스(NVMe 컨트롤러 등)가 물리
 * 메모리에 직접 쓴 뒤에도 CPU가 그 vmalloc 가상 주소를 통해 읽으면 캐시된
 * 오래된 값을 볼 위험이 있다. 이 함수는 READ 방향 bio가 완료됐을
 * 때만(WRITE가 아닐 때만) bi_private에 저장된 vmalloc 주소 범위에 대해
 * invalidate_kernel_vmap_range()를 호출해 그 위험을 없앤다.
 * 실행 컨텍스트: I/O 완료 콜백(bio->bi_end_io) 내부에서 호출되므로 NVMe
 * 인터럽트/softirq 컨텍스트일 수 있다 — 슬립 불가능한 코드만 포함해야
 * 한다(invalidate_kernel_vmap_range는 캐시 유지보수 명령이라 슬립하지 않음).
 * 호출자(caller): bio_map_kern_endio().
 * 피호출자(callee): op_is_write(), bio_op(), invalidate_kernel_vmap_range()
 * (ARCH_IMPLEMENTS_FLUSH_KERNEL_VMAP_RANGE 정의 시에만).
 * 에러 처리: 실패 개념이 없는 캐시 유지보수 동작이라 반환값이 없다.
 *
 * 호출 체인:
 *   bio 완료(bi_end_io) -> bio_map_kern_endio -> [bio_invalidate_vmalloc_pages]
 *
 * NVMe 연결:
 *   커널이 vmalloc 버퍼를 DMA 대상으로 넘긴 경우, I/O 완료 후 CPU가 해당
 *   영역을 다시 읽기 전에 캐시 일관성을 맞춘다.
 */
static void bio_invalidate_vmalloc_pages(struct bio *bio)
{
#ifdef ARCH_IMPLEMENTS_FLUSH_KERNEL_VMAP_RANGE
	if (bio->bi_private && !op_is_write(bio_op(bio))) { /* [한국어] bi_private에 vmalloc 주소가 보관되어 있고 READ 방향일 때만 무효화한다.
									 * WRITE는 CPU가 쓴 값을 장치가 읽는 방향이라 완료 후 무효화가 불필요하다. */
		unsigned long i, len = 0; /* i: bvec 인덱스; len: 무효화할 총 바이트; NVMe READ 데이터 길이와 일치 */

		for (i = 0; i < bio->bi_vcnt; i++) /* bio의 모든 vec 길이 합산; 각 vec은 NVMe PRP/SGL로 변환된 버퍼 조각 */
			len += bio->bi_io_vec[i].bv_len;
		invalidate_kernel_vmap_range(bio->bi_private, len); /* vmalloc 영역 캐시 무효화; NVMe DMA가 쓴 데이터를 CPU 캐시에 반영 */
	}
#endif
}

/*
 * [한국어]
 * bio_map_kern_endio - bio_map_kern에서 생성된 bio의 완료 처리
 *
 * @bio: 완료된 bio (bio_map_kern이 bi_end_io로 등록한 콜백)
 * @return: 없음(void). bio->bi_end_io 콜백 시그니처를 따른다.
 *
 * bio_map_kern()으로 만든 "직접 매핑" bio는 별도로 회수할 bounce page가
 * 없으므로, 완료 시 해야 할 일은 (vmalloc이었다면) 캐시 무효화와 bio 참조
 * 해제뿐이다. 이 함수가 그 두 단계를 순서대로 수행한다.
 * 실행 컨텍스트: 블록 계층이 bio 완료를 통지하는 지점에서 호출되므로
 * NVMe 인터럽트/softirq(BLOCK_SOFTIRQ) 컨텍스트일 수 있다 — 슬립 불가.
 * 호출자(caller): 블록 계층 bio 완료 경로(bio_endio 계열)가 bi_end_io를
 * 통해 호출.
 * 피호출자(callee): bio_invalidate_vmalloc_pages(), blk_mq_map_bio_put().
 * 에러 처리: 반환값이 없어 실패 개념이 없다.
 *
 * 호출 체인:
 *   NVMe CQ 처리 -> bio_endio -> bi_end_io -> [bio_map_kern_endio]
 *     -> bio_invalidate_vmalloc_pages | blk_mq_map_bio_put
 *
 * NVMe 연결:
 *   NVMe CQ 처리 후 request 완료 콜백에서 호출되며, DMA 매핑 해제 전
 *   후처리를 수행한다.
 */
static void bio_map_kern_endio(struct bio *bio)
{
	bio_invalidate_vmalloc_pages(bio); /* [한국어] vmalloc 버퍼였다면 READ 완료 후 캐시 일관성을 정리한다. */
	blk_mq_map_bio_put(bio); /* bio 참조 해제; NVMe CID에 연결된 커널 버퍼 매핑 종료 */
}

/*
 * [한국어]
 * bio_map_kern - 커널 가상 주소를 bio에 직접 매핑
 *
 * @rq: 대상 request
 * @data: 커널 버퍼 (물리적으로 연속이거나 vmalloc 주소)
 * @len: 길이(바이트)
 * @gfp_mask: bio 할당에 쓸 GFP 플래그
 * @return: 성공 시 완성된 struct bio *, 실패 시 ERR_PTR(-ENOMEM)(bio 할당
 *   실패) 또는 ERR_PTR(-EINVAL)(vmalloc 페이지 추가 실패). 호출자
 *   blk_rq_map_kern()은 IS_ERR()로 검사해 PTR_ERR()을 그대로 반환한다.
 *
 * blk_rq_map_kern()이 DMA 정렬 검사를 통과한(즉 페이지 경계에 잘 들어맞고
 * 스택 버퍼가 아닌) 커널 버퍼에 대해 호출하는 "직접 매핑" 경로다. 복사
 * 없이 원본 커널 페이지를 그대로 bio에 연결하므로 bounce buffer보다
 * 효율적이다. vmalloc 주소는 물리적으로 불연속인 여러 페이지로 구성될 수
 * 있어 bio_add_vmalloc()으로 각 페이지를 개별적으로 추가하고, 그 외
 * (kmalloc/정적 버퍼 등 물리적으로 연속인) 주소는 bio_add_virt_nofail()로
 * 한 번에 추가한다. 완료 콜백은 항상 bio_map_kern_endio로 설정한다.
 * 실행 컨텍스트: 매핑을 요청한 프로세스(또는 커널 서브시스템) 컨텍스트에서
 * 동기적으로 실행되며, 슬립 가능한 GFP 플래그가 쓰이면 슬립할 수 있다.
 * 호출자(caller): blk_rq_map_kern() (정렬/스택 검사를 통과했을 때).
 * 피호출자(callee): blk_rq_map_bio_alloc, is_vmalloc_addr, bio_add_vmalloc,
 * bio_add_virt_nofail.
 * 에러 처리: bio 할당 실패는 즉시 ERR_PTR 반환, vmalloc 추가 실패는 이미
 * 할당한 bio를 blk_mq_map_bio_put로 정리한 뒤 ERR_PTR 반환.
 *
 * 호출 체인:
 *   blk_rq_map_kern -> [bio_map_kern] -> blk_rq_append_bio
 *
 * NVMe 연결:
 *   커널에서 발행하는 admin/IO passthrough 명령의 데이터 버퍼를
 *   PRP/SGL로 연결하기 위해 사용된다.
 */
static struct bio *bio_map_kern(struct request *rq, void *data, unsigned int len,
		gfp_t gfp_mask)
{
	unsigned int nr_vecs = bio_add_max_vecs(data, len); /* [한국어] 커널 버퍼에 필요한 최대 bio_vec 개수 — PRP/SGL 디스크립터 수의 상한 */
	struct bio *bio; /* 커널 버퍼가 직접 매핑될 bio */

	bio = blk_rq_map_bio_alloc(rq, nr_vecs, gfp_mask); /* NVMe READ/WRITE 방향이 설정된 bio 할당 */
	if (!bio) /* bio 할당 실패 - 아직 아무 자원도 잡지 않아 별도 정리 없이 반환 */
		return ERR_PTR(-ENOMEM); /* 페이지 확보 실패 — 아래 cleanup 라벨에서 지금까지 잡은 페이지를 전부 되돌린다 */

	if (is_vmalloc_addr(data)) { /* [한국어] vmalloc 주소는 물리적으로 불연속이라 페이지 단위로 쪼개 추가해야 한다. */
		bio->bi_private = data; /* vmalloc 기준 주소를 bi_private에 보관; 완료 시 캐시 무효화에 사용 */
		if (!bio_add_vmalloc(bio, data, len)) { /* vmalloc 페이지를 bio에 추가; NVMe PRP/SGL용 페이지 목록 구성 */
			blk_mq_map_bio_put(bio); /* 추가 실패 - 지금까지 만든 bio를 반환(아직 완료 콜백 등록 전) */
			return ERR_PTR(-EINVAL); /* vmalloc 영역 bio 구성 실패 */
		}
	} else { /* vmalloc이 아닌 물리적으로 연속된 일반 커널 주소(kmalloc/정적 버퍼 등) */
		bio_add_virt_nofail(bio, data, len); /* [한국어] 직접 매핑 영역(kmalloc 등)은 물리적으로 연속이라 세그먼트 하나로 추가한다. */
	}
	bio->bi_end_io = bio_map_kern_endio; /* I/O 완료 시 bio_map_kern_endio 호출; NVMe CQ 처리 후 vmalloc 캐시 정리 및 bio 반환 */
	return bio; /* 커널 버퍼가 담긴 bio. 이후 blk_rq_append_bio 로 request 에 연결된다 */
}

/*
 * [한국어]
 * bio_copy_kern_endio - bio_copy_kern에서 할당한 페이지 회수
 *
 * @bio: 완료된 bio (bio_copy_kern이 WRITE 방향에 대해 bi_end_io로 등록)
 * @return: 없음(void). bio->bi_end_io 콜백 시그니처를 따른다.
 *
 * bio_copy_kern()의 WRITE 경로는 원본 커널 버퍼 데이터를 bounce page로
 * 복사해 NVMe에 제출했으므로, 완료 후에는 사용자 공간으로 되돌릴 데이터가
 * 없고 단지 bounce page만 회수하면 된다. bio_free_pages()로 페이지를
 * 해제하고 bio 참조를 반환한다.
 * 실행 컨텍스트: bio 완료 콜백이므로 NVMe 인터럽트/softirq 컨텍스트일 수
 * 있다 — 슬립 불가능한 코드만 포함(bio_free_pages는 슬립하지 않음).
 * 호출자(caller): bio_copy_kern() (WRITE 경로에서 bi_end_io로 설정),
 * 그리고 bio_copy_kern_endio_read()가 READ 경로 뒤처리로 재사용.
 * 피호출자(callee): bio_free_pages(), blk_mq_map_bio_put().
 * 에러 처리: 반환값이 없어 실패 개념이 없다.
 *
 * 호출 체인:
 *   NVMe CQ 처리 -> bio_endio -> bi_end_io -> [bio_copy_kern_endio]
 *
 * NVMe 연결:
 *   NVMe 명령이 완료된 후 해당 CID의 데이터 버퍼 페이지를 정리한다.
 */
static void bio_copy_kern_endio(struct bio *bio)
{
	bio_free_pages(bio); /* NVMe 명령 완료 후 bounce buffer 커널 페이지 회수; PRP/SGL에 쓰인 임시 메모리 반납 */
	blk_mq_map_bio_put(bio); /* bio 반환; 해당 NVMe CID의 생명 주기 종료 */
}

/*
 * [한국어]
 * bio_copy_kern_endio_read - READ용 bounce buffer 결과를 원래 커널 버퍼로 복사
 *
 * @bio: 완료된 bio (bio_copy_kern이 READ 방향에 대해 bi_end_io로 등록,
 *   bio->bi_private에 원본 커널 버퍼 포인터가 저장되어 있음)
 * @return: 없음(void). bio->bi_end_io 콜백 시그니처를 따른다.
 *
 * bio_copy_kern()의 READ 경로는 NVMe 컨트롤러가 bounce page에 데이터를
 * DMA로 채우므로, 완료 시 이 데이터를 원래 호출자가 넘긴 커널 버퍼(kbuf)
 * 로 복사해 돌려줘야 한다. bio의 모든 bio_vec을 순서대로 순회하며
 * memcpy_from_bvec()으로 kbuf 위치에 이어붙이듯 복사한 뒤, 공통 마무리
 * 작업(페이지 해제, bio 반환)은 bio_copy_kern_endio()에 위임한다.
 * 실행 컨텍스트: bio 완료 콜백이므로 NVMe 인터럽트/softirq 컨텍스트일 수
 * 있다 — memcpy 기반 복사만 수행하므로 슬립하지 않는다.
 * 호출자(caller): 블록 계층 bio 완료 경로가 bi_end_io를 통해 호출.
 * 피호출자(callee): bio_for_each_segment_all(), memcpy_from_bvec(),
 * bio_copy_kern_endio().
 * 에러 처리: 반환값이 없어 실패 개념이 없다(복사 실패 가능성은 커널 버퍼
 * 간 memcpy이므로 사용자 폴트와 달리 없다고 간주).
 *
 * 호출 체인:
 *   NVMe CQ 처리 -> bio_endio -> bi_end_io -> [bio_copy_kern_endio_read]
 *     -> bio_copy_kern_endio
 *
 * NVMe 연결:
 *   NVMe READ 완료(CQ) 후 커널 호출자가 요청한 kbuf에 결과를 돌려준다.
 */
static void bio_copy_kern_endio_read(struct bio *bio)
{
	char *p = bio->bi_private; /* 원래 커널 버퍼의 시작 주소; NVMe READ 결과를 돌려줄 대상 */
	struct bio_vec *bvec; /* NVMe READ로 채워진 bounce buffer 페이지 조각 */
	struct bvec_iter_all iter_all; /* 모든 bvec 순회용 */

	bio_for_each_segment_all(bvec, bio, iter_all) { /* NVMe CQ 완료 후 bounce buffer에서 원래 kbuf로 복사 */
		memcpy_from_bvec(p, bvec); /* bvec의 page/offset/len에서 원래 커널 버퍼로 복사 */
		p += bvec->bv_len; /* 다음 대상 위치로 이동; NVMe READ 데이터의 연속적인 복원 */
	}

	bio_copy_kern_endio(bio); /* bounce 페이지 해제 및 bio 반환 */
}

/**
 *	bio_copy_kern	-	copy kernel address into bio
 *	@rq: request to fill
 *	@data: pointer to buffer to copy
 *	@len: length in bytes
 *	@op: bio/request operation
 *	@gfp_mask: allocation flags for bio and page allocation
 *
 *	copy the kernel address into a bio suitable for io to a block
 *	device. Returns an error pointer in case of error.
 */
/*
 * [한국어]
 * bio_copy_kern - 정렬되지 않은 커널 버퍼를 bounce buffer bio로 복사
 *
 * @rq: 대상 request
 * @data: 커널 버퍼 (스택 버퍼이거나 DMA 정렬/물리적 연속 조건을 만족하지
 *   못하는 주소일 수 있음)
 * @len: 길이(바이트)
 * @gfp_mask: bio/페이지 할당에 쓸 GFP 플래그
 * @return: 성공 시 완성된 struct bio *, 실패 시 ERR_PTR(-EINVAL)(길이
 *   오버플로우) 또는 ERR_PTR(-ENOMEM)(bio/페이지 할당 실패). 호출자
 *   blk_rq_map_kern()이 IS_ERR()로 검사한다.
 *
 * blk_rq_map_kern()이 blk_rq_aligned() 검사에 실패했거나(정렬 안 됨)
 * object_is_on_stack()이 참인(스택 버퍼) 경우 호출되는 bounce-copy
 * 경로다. 원본 버퍼가 걸치는 페이지 수만큼 새로 __GFP_ZERO 페이지를
 * 할당하고, WRITE 방향이면 원본 데이터를 그 페이지로 memcpy한다. READ
 * 방향이면 페이지는 0으로 초기화된 채로 NVMe 컨트롤러가 채우게 두고,
 * bi_private에 원본 kbuf 주소를 저장해 완료 후 bio_copy_kern_endio_read
 * 가 그 위치로 결과를 복사하게 한다.
 * 실행 컨텍스트: 매핑을 요청한 컨텍스트에서 동기적으로 실행되며,
 * GFP_NOIO를 사용해 이 할당 자체가 다시 I/O를 유발하지 않도록 한다
 * (재귀적 회수 경로 방지).
 * 호출자(caller): blk_rq_map_kern() (정렬 실패 또는 스택 버퍼일 때).
 * 피호출자(callee): blk_rq_map_bio_alloc, alloc_page, memcpy,
 * __bio_add_page, bio_free_pages, blk_mq_map_bio_put.
 * 에러 처리: 오버플로우는 즉시 -EINVAL, 이후 실패는 cleanup 레이블에서
 * 이미 추가된 bounce page를 모두 회수한 뒤 -ENOMEM을 반환한다.
 *
 * 호출 체인:
 *   blk_rq_map_kern -> [bio_copy_kern] -> blk_rq_append_bio
 *
 * NVMe 연결:
 *   NVMe PRP는 첫 엔트리 외에는 페이지 정렬을 요구하므로, 이 함수를
 *   통해 적합한 페이지로 정리한 뒤 PRP/SGL로 연결한다.
 */
static struct bio *bio_copy_kern(struct request *rq, void *data, unsigned int len,
		gfp_t gfp_mask)
{
	enum req_op op = req_op(rq); /* request의 operation(NVMe opcode 방향) 획득; READ/WRITE/FLUSH 등 */
	unsigned long kaddr = (unsigned long)data; /* 커널 버퍼 가상 주소; DMA 정렬 검사에 사용 */
	unsigned long end = (kaddr + len + PAGE_SIZE - 1) >> PAGE_SHIFT; /* 버퍼 끝이 속한 페이지 인덱스(올림) */
	unsigned long start = kaddr >> PAGE_SHIFT; /* 버퍼 시작이 속한 페이지 인덱스(내림) */
	struct bio *bio; /* bounce page들로 구성될 bio */
	void *p = data; /* 원본 커널 버퍼 내 현재 복사 위치; NVMe WRITE 시 bounce page 채우기용 */
	int nr_pages = 0; /* 필요한 페이지 수; NVMe PRP entry 수 상한 */

	/*
	 * Overflow, abort
	 */
	if (end < start) /* 오버플로우 시 잘못된 길이; NVMe 컨트롤러에 잘못된 명령 길이를 복사하지 않도록 방어 */
		return ERR_PTR(-EINVAL);

	nr_pages = end - start; /* 필요한 페이지 수; NVMe PRP entry 수 상한 */
	bio = blk_rq_map_bio_alloc(rq, nr_pages, gfp_mask); /* NVMe READ/WRITE 방향이 설정된 bio 할당 */
	if (!bio) /* bio 할당 실패 - 아직 페이지를 확보하지 않아 별도 정리 없이 반환 */
		return ERR_PTR(-ENOMEM); /* 페이지 확보 실패 — 아래 cleanup 라벨에서 지금까지 잡은 페이지를 전부 되돌린다 */

	while (len) { /* 남은 데이터를 페이지 단위로 bounce buffer에 복사; 각 페이지는 NVMe PRP/SGL 후보 */
		struct page *page; /* 이번 반복에서 새로 할당할 bounce page */
		unsigned int bytes = PAGE_SIZE; /* 이번 페이지에서 채울 바이트 수(한 페이지 전체)로 초기화 */

		if (bytes > len) /* 마지막 반복에서는 페이지 전체가 아니라 남은 길이만큼만 필요 */
			bytes = len; /* 마지막 페이지는 실제 남은 길이만큼만 사용; NVMe 명령 길이와 정확히 일치 */

		page = alloc_page(GFP_NOIO | __GFP_ZERO | gfp_mask); /* 쓰기면 0으로 초기화된 커널 페이지 할당; NVMe DMA에 안전한 메모리 */
		if (!page) /* 페이지 할당 실패 - 이미 추가된 페이지들은 cleanup에서 일괄 회수 */
			goto cleanup; /* 메모리 부족; 이미 할당된 bounce 페이지들은 cleanup에서 해제 */

		if (op_is_write(op)) /* 쓰기라면 지금 복사해 둬야 한다 — 장치는 나중에 이 bounce 페이지에서 데이터를 읽어 가므로, 그 전에 내용이 채워져 있어야 한다 */
			memcpy(page_address(page), p, bytes);

		__bio_add_page(bio, page, bytes, 0); /* 커널 페이지를 bio에 추가; PRP/SGL용 페이지 후보 */

		len -= bytes; /* 처리한 바이트만큼 감소 */
		p += bytes; /* 원본 버퍼 포인터 전진 */
	}

	if (op_is_write(op)) { /* WRITE는 복사된 페이지만 해제 */
		bio->bi_end_io = bio_copy_kern_endio; /* 완료 시 bounce page만 회수하면 되는 콜백 등록 */
	} else { /* READ는 완료 후 원래 kbuf로 복사할 수 있도록 bi_private 저장 */
		bio->bi_end_io = bio_copy_kern_endio_read; /* 완료 시 bounce page -> 원본 kbuf 복사까지 수행하는 콜백 등록 */
		bio->bi_private = data; /* NVMe CQ 완료 후 원래 커널 버퍼로 결과 복사할 때 사용 */
	}

	return bio; /* bounce 버퍼로 구성한 bio. 이후 blk_rq_append_bio 로 request 에 연결된다 */

cleanup:
	bio_free_pages(bio); /* 실패 시 이미 붙인 bounce 페이지를 전부 회수한다 */
	blk_mq_map_bio_put(bio); /* bio 반환 */
	return ERR_PTR(-ENOMEM); /* 페이지 할당 실패를 -ENOMEM으로 보고 */
}

/*
 * Append a bio to a passthrough request.  Only works if the bio can be merged
 * into the request based on the driver constraints.
 */
/*
 * [한국어]
 * blk_rq_append_bio - passthrough request에 bio를 추가/병합
 *
 * @rq: 대상 request (이 파일의 여러 함수가 만든 bio를 최종적으로 연결)
 * @bio: 추가할 bio
 * @return: 0(성공) 또는 음수 오류(-EREMOTEIO: 하드웨어 제약 초과로 분할이
 *   필요함 — 호출자는 이를 신호로 받아 bounce-copy 경로로 재시도,
 *   -EINVAL: back-merge 조건 불충족). 호출자들은 이 값을 그대로 반환하거나
 *   -EREMOTEIO만 별도로 해석해 재시도한다.
 *
 * 이 파일의 모든 매핑 함수(bio_copy_user_iov, bio_map_user_iov,
 * blk_rq_map_user_bvec, bio_map_kern, bio_copy_kern)가 만든 bio는 결국 이
 * 함수를 통해 request에 연결된다. request에 아직 bio가 없으면 첫 bio로
 * 설정하고, 이미 있으면 하드웨어가 허용하는 한 뒤에 back-merge한다. 먼저
 * bio_split_io_at()으로 이 bio 하나가 max_hw_sectors 등 하드웨어 제약을
 * 넘는지 검사해 넘으면 분할이 필요하다는 뜻이므로 -EREMOTEIO로 상위에
 * 알려 복사 경로로 유도한다. 이어서 기존 request가 있으면
 * ll_back_merge_fn()으로 병합 가능 여부를 확인하고, 가능하면 bio 리스트
 * 뒤에 연결하며 길이/segment 통계를 갱신한다.
 * 실행 컨텍스트: 매핑을 요청한 프로세스 컨텍스트에서 동기 호출되며, 하나의
 * request는 매핑 단계에서 단일 스레드만 다루므로 동시성 문제가 없다.
 * 호출자(caller): bio_copy_user_iov, bio_map_user_iov, blk_rq_map_user_bvec,
 * blk_rq_map_kern.
 * 피호출자(callee): bio_split_io_at(), ll_back_merge_fn(), bio_seg_gap(),
 * bio_crypt_free_ctx().
 * 에러 처리: 분할 필요(-EREMOTEIO) 또는 병합 실패(-EINVAL) 시 request는
 * 변경되지 않고 그대로 오류가 반환된다.
 *
 * 호출 체인:
 *   bio_copy_user_iov | bio_map_user_iov | blk_rq_map_user_bvec
 *     | blk_rq_map_kern -> [blk_rq_append_bio]
 *     -> blk_execute_rq -> (디스패치) -> mq_ops->queue_rq
 *
 * NVMe 연결:
 *   queue_limits.max_hw_sectors는 NVMe Max Data Transfer Size(MDTS)에
 *   대응하는 상한이며, bio_split_io_at으로 초과 여부를 검사한다.
 *   rq->nr_phys_segments는 NVMe PRP/SGL entry 개수와 관련이 있다.
 */
int blk_rq_append_bio(struct request *rq, struct bio *bio)
{
	const struct queue_limits *lim = &rq->q->limits; /* request queue의 한계값; NVMe MDTS/segment 제약 반영 */
	unsigned int max_bytes = lim->max_hw_sectors << SECTOR_SHIFT; /* [한국어] 최대 전송 바이트 = MDTS에서 유도된 max_hw_sectors를 바이트로 환산 */
	unsigned int nr_segs = 0; /* [한국어] bio의 물리 세그먼트 수 — NVMe PRP/SGL 디스크립터 개수가 된다 */
	int ret; /* bio_split_io_at/ll_back_merge_fn 결과를 담을 임시 변수 */

	/* check that the data layout matches the hardware restrictions */
	ret = bio_split_io_at(bio, lim, &nr_segs, max_bytes, 0); /* bio가 하드웨어 한계를 초과하면 분할 또는 복사 필요 */
	if (ret) { /* 0이 아니면 분할이 필요하거나(양수) 다른 오류(음수)가 발생한 것 */
		/* if we would have to split the bio, copy instead */
		if (ret > 0) /* 이 bvec 구성으로는 큐 한계(max_segments/virt_boundary 등) 때문에 분할이 필요하다는 뜻. 패스스루는 명령을 쪼갤 수 없으므로 EREMOTEIO 로 표시해 상위가 복사 경로로 폴백하게 한다 */
			ret = -EREMOTEIO;
		return ret; /* NVMe 명령 생성 실패; 상위에서 복사 경로로 재시도 */
	}

	if (rq->bio) { /* request에 이미 bio가 연결되어 있으면 back-merge를 시도하는 분기 */
		if (!ll_back_merge_fn(rq, bio, nr_segs)) /* 기존 request 뒤에 back-merge 가능한지 검사; NVMe PRP/SGL 연속성/MDTS 위반 시 거부 */
			return -EINVAL; /* 병합 불가 - request는 변경되지 않은 채 상위에 오류 반환 */
		rq->phys_gap_bit = bio_seg_gap(rq->q, rq->biotail, bio, /* [한국어] 세그먼트 이음매의 정렬 비트를 갱신 — blk_can_dma_map_iova()의 IOVA 병합 판정 근거 */
					       rq->phys_gap_bit);
		rq->biotail->bi_next = bio; /* bio를 request의 bio 리스트 끝에 연결; NVMe 명령 하나로 처리될 데이터 청크 추가 */
		rq->biotail = bio; /* biotail을 새로 추가된 bio로 갱신해 다음 append가 여기 이어붙도록 함 */
		rq->__data_len += bio->bi_iter.bi_size; /* request의 총 데이터 길이 누적; NVMe 명령 길이와 일치 */
		bio_crypt_free_ctx(bio); /* [한국어] bio의 암호화 컨텍스트 해제 — request가 이미 호환 컨텍스트를 갖고 있어 중복이다 */
		return 0; /* back-merge 성공 */
	}

	rq->nr_phys_segments = nr_segs; /* request의 첫 bio 설정; NVMe PRP/SGL entry 개수 초기화 */
	rq->bio = rq->biotail = bio; /* request의 bio 리스트를 이 bio로 시작 */
	rq->__data_len = bio->bi_iter.bi_size; /* request의 총 데이터 길이; NVMe 명령의 데이터 길이와 일치 */
	rq->phys_gap_bit = bio->bi_bvec_gap_bit; /* bio 내부 segment 간 물리적 gap 기록 */
	return 0; /* bio 가 request 에 연결됐다 */
}
EXPORT_SYMBOL(blk_rq_append_bio);

/* Prepare bio for passthrough IO given ITER_BVEC iter */
/*
 * [한국어]
 * blk_rq_map_user_bvec - ITER_BVEC 기반 사용자/커널 bvec을 bio로 재사용
 *
 * @rq: 대상 request
 * @iter: bvec 반복자(ITER_BVEC 타입 — 이미 준비된 bio_vec 배열을 가리킴)
 * @return: 0(성공) 또는 음수 오류(-EINVAL: 길이 0 또는 하드웨어 한계 초과,
 *   -ENOMEM: bio 할당 실패, 그 외 blk_rq_append_bio가 반환하는 값 —
 *   특히 -EREMOTEIO는 호출자 blk_rq_map_user_iov가 복사 경로로 재시도하는
 *   신호로 쓰인다).
 *
 * io_uring 고정 버퍼(fixed buffer)나 커널 내부에서 이미 struct bio_vec
 * 배열 형태로 준비된 데이터는, 페이지를 새로 pin하거나 복사할 필요 없이
 * 그 bvec을 그대로 bio에 설정(bio_iov_bvec_set)해 재사용하면 오버헤드를
 * 더 줄일 수 있다. 길이가 0이거나 하드웨어 최대 전송 크기를 넘으면 먼저
 * 거부하고, 그렇지 않으면 bvec 개수만큼의 슬롯을 새로 할당하지 않는 bio
 * (nr_vecs=0)를 만들어 bvec을 직접 연결한 뒤 request에 append한다.
 * 실행 컨텍스트: 매핑을 요청한 프로세스 컨텍스트에서 동기 호출된다.
 * 호출자(caller): blk_rq_map_user_iov() (iov_iter_is_bvec(iter)가 참일 때).
 * 피호출자(callee): blk_rq_map_bio_alloc, bio_iov_bvec_set,
 * blk_rq_append_bio, blk_mq_map_bio_put.
 * 에러 처리: append 실패 시 방금 만든 bio를 blk_mq_map_bio_put으로 반환한
 * 뒤 오류 코드를 그대로 전달한다.
 *
 * 호출 체인:
 *   blk_rq_map_user_iov -> [blk_rq_map_user_bvec] -> blk_rq_append_bio
 *
 * NVMe 연결:
 *   bvec의 각 page/offset/len이 NVMe PRP entry로 변환될 수 있도록
 *   request에 등록된다.
 */
static int blk_rq_map_user_bvec(struct request *rq, const struct iov_iter *iter)
{
	unsigned int max_bytes = rq->q->limits.max_hw_sectors << SECTOR_SHIFT; /* [한국어] passthrough bio의 최대 허용 바이트 = MDTS 유래 max_hw_sectors 환산값 */
	struct bio *bio; /* 재사용된 bvec을 담을 bio(자체 페이지 할당 없음) */
	int ret; /* append_bio 결과 */

	if (!iov_iter_count(iter) || iov_iter_count(iter) > max_bytes) /* 길이가 0이거나 max를 초과하면 NVMe 컨트롤러가 처리할 수 없음 */
		return -EINVAL;

	/* reuse the bvecs from the iterator instead of allocating new ones */
	bio = blk_rq_map_bio_alloc(rq, 0, GFP_KERNEL); /* bvec을 재사용하므로 추가 할당 없음(nr_vecs=0); NVMe PRP/SGL용 페이지 목록은 이미 iter에 존재 */
	if (!bio) /* bio 할당 실패 - 아직 아무 자원도 잡지 않아 별도 정리 없이 반환 */
		return -ENOMEM;
	bio_iov_bvec_set(bio, iter); /* 반복자의 bvec을 bio에 직접 설정; 각 bvec이 NVMe PRP entry로 변환 가능 */

	ret = blk_rq_append_bio(rq, bio); /* 재사용한 bvec bio를 request에 추가 */
	if (ret) /* append 실패 - request는 변경되지 않았으므로 방금 만든 bio만 반환 */
		blk_mq_map_bio_put(bio); /* request 연결에 실패했으면 bio 를 반환한다 */
	return ret; /* 0 이면 호출자가 이 요청을 제출할 수 있다 */
}

/**
 * blk_rq_map_user_iov - map user data to a request, for passthrough requests
 * @q:		request queue where request should be inserted
 * @rq:		request to map data to
 * @map_data:   pointer to the rq_map_data holding pages (if necessary)
 * @iter:	iovec iterator
 * @gfp_mask:	memory allocation flags
 *
 * Description:
 *    Data will be mapped directly for zero copy I/O, if possible. Otherwise
 *    a kernel bounce buffer is used.
 *
 *    A matching blk_rq_unmap_user() must be issued at the end of I/O, while
 *    still in process context.
 */
/*
 * [한국어]
 * blk_rq_map_user_iov - 사용자 iov를 request에 매핑하는 최상위 함수
 *
 * @q: request queue (하드웨어 제약 — max_hw_sectors, DMA 정렬, 가상 경계
 *   등 — 을 조회하는 데 사용)
 * @rq: 대상 request
 * @map_data: 호출자가 미리 준비한 페이지 풀(rq_map_data). NULL이면 매핑
 *   함수들이 필요 시 직접 할당/pin한다.
 * @iter: 사용자 버퍼 기술자(iov_iter) — const로 받아 내부에서 지역 복사본
 *   (i)을 진행시키며 소비하고 원본은 변경하지 않는다.
 * @gfp_mask: 메모리 할당 플래그
 * @return: 0(성공, rq->bio 체인에 하나 이상의 bio가 연결됨) 또는 음수
 *   오류(-EINVAL 등). 호출자는 실패 시 rq->bio가 NULL로 리셋되어 있음을
 *   전제로 재시도하거나 오류를 그대로 사용자에게 보고한다. 성공 시 호출자는
 *   I/O 완료 후 반드시 blk_rq_unmap_user()를 호출해야 한다(원문 주석 참고).
 *
 * SG_IO류 passthrough 명령의 데이터 버퍼를 매핑하는 이 파일의 최상위
 * 진입점이다. 먼저 DMA 정렬(iov_iter_alignment vs align), iov_iter 종류
 * (ITER_BVEC 여부), 사용자 페이지 기반 여부(user_backed_iter), 가상 경계
 * gap 정렬 여부를 검사해 zero-copy/bvec 재사용/bounce-copy 중 하나를
 * 결정한다. bvec 재사용을 시도했다가 하드웨어 한계로 실패
 * (-EREMOTEIO)하면 즉시 bounce-copy로 폴백한다. 이후 iov_iter가 소진될
 * 때까지 반복적으로 bio_copy_user_iov()/bio_map_user_iov()를 호출해 여러
 * bio를 만들어 request에 이어붙인다(하나의 bio가 담을 수 있는 크기를
 * iov_iter가 초과하면 여러 번 반복).
 * 실행 컨텍스트: 반드시 프로세스 컨텍스트에서 슬립 가능한 상태로 호출되어야
 * 한다(사용자 주소 공간 접근, 페이지 pin/할당 포함). 하나의 request는 이
 * 매핑 단계에서 단일 호출자만 다루므로 동시성 문제는 없다.
 * 호출자(caller): blk_rq_map_user(), blk_rq_map_user_io() (SG_IO 등에서
 * 사용자 버퍼를 매핑할 때).
 * 피호출자(callee): blk_lim_dma_alignment_and_pad, blk_rq_map_user_bvec,
 * bio_copy_user_iov, bio_map_user_iov, blk_rq_unmap_user.
 * 에러 처리: 도중에 실패하면 unmap_rq 레이블에서 그때까지 연결된 bio들을
 * blk_rq_unmap_user()로 모두 되돌리고, rq->bio를 NULL로 리셋한 뒤 오류를
 * 반환한다.
 *
 * 호출 체인:
 *   SG_IO ioctl -> blk_rq_map_user | blk_rq_map_user_io
 *     -> [blk_rq_map_user_iov]
 *     -> bio_map_user_iov | bio_copy_user_iov | blk_rq_map_user_bvec
 *     -> blk_rq_append_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *     -> blk_execute_rq -> (디스패치) -> mq_ops->queue_rq (간접 호출)
 *
 * NVMe 연결:
 *   DMA 정렬, 가상 경계, 최대 전송 크기 등 NVMe 컨트롤러 제약을 반영해
 *   bio 를 준비한다. PRP/SGL 변환은 이 파일이 아니라 드라이버(NVMe PCIe 라면
 *   nvme_pci_setup_data_prp/sgl)에서 일어난다.
 */
int blk_rq_map_user_iov(struct request_queue *q, struct request *rq,
			struct rq_map_data *map_data,
			const struct iov_iter *iter, gfp_t gfp_mask)
{
	bool copy = false, map_bvec = false; /* copy: bounce buffer 경로; map_bvec: 기존 bvec 재사용 경로 */
	unsigned long align = blk_lim_dma_alignment_and_pad(&q->limits); /* [한국어] DMA 정렬/패딩 요구사항. NVMe는 dma_alignment = 3(4바이트)을 쓴다 */
	struct bio *bio = NULL; /* 첫 번째 bio를 기록; 실패 시 blk_rq_unmap_user로 정리 */
	struct iov_iter i; /* iter의 지역 복사본; 아래 do-while에서 진행(advance)시키며 소비 */
	int ret = -EINVAL; /* 기본 실패값; 아무 경로도 타지 않고 함수가 끝나는 일은 없지만 안전한 기본값으로 초기화 */

	if (map_data) /* [한국어] 호출자가 페이지 풀을 준비해 넘겼다면 그것을 쓰라는 뜻이므로 복사 경로다 (원래 주석: 풀 모드 */
		copy = true;
	else if (iov_iter_alignment(iter) & align) /* [한국어] 정렬을 만족하지 못하면 bounce 버퍼로 복사해야 한다 */
		copy = true;
	else if (iov_iter_is_bvec(iter)) /* [한국어] 이미 bvec 형태라면 페이지가 이미 확보돼 있으므로 pin 도 복사도 필요 없다 (원래 주석: 매핑 */
		map_bvec = true;
	else if (!user_backed_iter(iter)) /* [한국어] pipe 처럼 사용자 페이지가 뒷받침하지 않는 iter 는 pin 할 대상이 없어 복사할 수밖에 없다 (원래 주석: 널 페이지 준비 */
		copy = true;
	else if (queue_virt_boundary(q)) /* [한국어] virt_boundary가 설정된 큐(NVMe PRP 모드)는 페이지 정렬을 요구하므로,
						 * 사용자 버퍼를 그대로 쓰지 못하고 정렬된 커널 버퍼로 복사한다 */
		copy = queue_virt_boundary(q) & iov_iter_gap_alignment(iter);

	if (map_bvec) { /* [한국어] 먼저 그대로 쓸 수 있는지 시도해 본다 */
		ret = blk_rq_map_user_bvec(rq, iter); /* bvec을 그대로 연결 시도; 하드웨어 한계 초과 시 -EREMOTEIO */
		if (!ret) /* 성공 - 더 볼 것 없이 바로 완료 */
			return 0; /* bvec 재사용 성공 — 이미 bvec 형태로 들어온 iov_iter 라 페이지를 새로 pin 하거나 복사할 필요가 없었다 */
		if (ret != -EREMOTEIO) /* bvec 재사용이 한계 초과 외의 이유로 실패면 즉시 종료 */
			goto fail;
		/* fall back to copying the data on limits mismatches */
		copy = true; /* [한국어] 한계에 안 맞으면 복사 경로로 폴백한다. 복사하면 커널이 페이지 배치를
				 * 직접 정할 수 있어 한계를 만족시킬 수 있다. */
	}

	i = *iter; /* 남은 iov_iter가 있을 때까지 bio를 생성/추가 */
	do {
		if (copy) /* 정렬/제약상 커널 페이지로 복사해야 하는 경우 */
			ret = bio_copy_user_iov(rq, map_data, &i, gfp_mask); /* bounce buffer bio를 만들어 request에 연결 */
		else /* 사용자 페이지를 그대로 pin해서 쓸 수 있는 경우 */
			ret = bio_map_user_iov(rq, &i, gfp_mask); /* zero-copy bio를 만들어 request에 연결 */
		if (ret) { /* 이번 반복에서 bio 생성/연결이 실패한 경우 */
			if (ret == -EREMOTEIO) /* EREMOTEIO 는 "이 bvec 그대로는 큐 한계에 안 맞는다"는 내부 신호다. 패스스루는 쪼갤 수 없으므로 상위에서 -EINVAL 로 바꿔 사용자에게 돌려준다 */
				ret = -EINVAL;
			goto unmap_rq; /* unmap_rq 라벨로 — 앞선 반복에서 이미 request 에 붙인 bio 들까지 전부 되돌려야 한다 */
		}
		if (!bio) /* 첫 bio를 기록해 실패 시 언매핑에 사용 */
			bio = rq->bio;
	} while (iov_iter_count(&i)); /* [한국어] 한 bio 로 다 담기지 않으면 여러 bio 로 나눠 이어 붙인다.
					 * 이들은 하나의 request 에 체인으로 매달리므로 명령은 여전히 하나다 (원래 주석: 위로 분할 연결 */

	return 0; /* 모든 bio 가 request 에 연결됐다. 이제 호출자가 blk_execute_rq 로 제출할 수 있다 */

unmap_rq:
	blk_rq_unmap_user(bio); /* 실패 시 기존에 연결된 bio들을 언매핑; pin된 페이지나 bounce buffer 회수 */
fail:
	rq->bio = NULL; /* request 와 bio 의 연결을 끊는다. 이미 blk_rq_unmap_user 가 bio 들을 해제했으므로, 이 포인터를 남겨 두면 해제된 메모리를 가리키게 된다 */
	return ret; /* -EINVAL 등 오류 코드 반환; 호출자(SG_IO 등)는 이 값을 그대로 유저스페이스로 전달 */
}
EXPORT_SYMBOL(blk_rq_map_user_iov);

/*
 * [한국어]
 * blk_rq_map_user - 단일 사용자 버퍼를 request에 매핑
 *
 * @q: request queue
 * @rq: 대상 request
 * @map_data: 페이지 풀(rq_map_data), 없으면 NULL
 * @ubuf: 사용자 버퍼 주소
 * @len: 길이(바이트)
 * @gfp_mask: 메모리 할당 플래그
 * @return: 0(성공) 또는 음수 오류(-EFAULT/-EINVAL 등 import_ubuf 실패,
 *   그 외 blk_rq_map_user_iov가 반환하는 코드). 호출자(SG_IO 등)는 이
 *   값을 그대로 유저스페이스 오류로 전달한다.
 *
 * 단일 연속 사용자 버퍼(ubuf, len)를 받는 가장 단순한 매핑 API로, 내부적
 * 으로는 결국 iov_iter 기반의 blk_rq_map_user_iov()에 위임하는 얇은
 * 래퍼다. import_ubuf()가 rq_data_dir(rq)(READ/WRITE)에 맞춰 단일 항목
 * iov_iter를 구성해 준다.
 * 실행 컨텍스트: 호출자의 프로세스 컨텍스트에서 실행되며 슬립 가능해야
 * 한다.
 * 호출자(caller): SG_IO ioctl 경로, NVMe/SCSI passthrough ioctl 핸들러,
 * blk_rq_map_user_io()(vec가 아닌 단순 버퍼 경우).
 * 피호출자(callee): import_ubuf(), blk_rq_map_user_iov().
 * 에러 처리: import_ubuf 실패 시 blk_rq_map_user_iov를 호출하지 않고 즉시
 * 오류를 반환한다.
 *
 * 호출 체인:
 *   SG_IO / ioctl -> [blk_rq_map_user] -> blk_rq_map_user_iov -> ...
 *
 * NVMe 연결:
 *   NVMe admin/ioctl passthrough 명령의 데이터 버퍼 처리 시작점.
 */
int blk_rq_map_user(struct request_queue *q, struct request *rq,
		    struct rq_map_data *map_data, void __user *ubuf,
		    unsigned long len, gfp_t gfp_mask)
{
	struct iov_iter i; /* import_ubuf가 채워줄 단일 버퍼 기반 iov_iter */
	int ret = import_ubuf(rq_data_dir(rq), ubuf, len, &i); /* 사용자 단일 버퍼를 iov_iter로 변환; rq_data_dir은 NVMe READ/WRITE 방향 */

	if (unlikely(ret < 0)) /* 사용자 공간 버퍼 import 실패 — 잘못된 포인터나 접근 권한 문제 */
		return ret;

	return blk_rq_map_user_iov(q, rq, map_data, &i, gfp_mask); /* 실제 작업은 전부 여기로 위임한다. 이 래퍼가 한 일은 raw 포인터+길이를 iov_iter 로 바꾼 것뿐이다 */
}
EXPORT_SYMBOL(blk_rq_map_user);

/*
 * blk_rq_map_user_io - iovec 기반 SG_IO 버퍼를 request에 매핑
 * @req: 대상 request
 * @map_data: 페이지 풀
 * @ubuf: 사용자 iovec 또는 단일 버퍼
 * @buf_len: 버퍼 길이
 * @gfp_mask: 메모리 할당 플래그
 * @vec: true면 ubuf를 사용자 iovec 배열로, false면 단일 연속 버퍼로 해석
 * @iov_count: vec가 true일 때 iovec 개수(또는 0이면 buf_len을 항목 수로 사용)
 * @check_iter_count: true면 SG_IO 규칙에 따라 truncate 후 반복자가 비었는지
 *   검사해 빈 전송을 거부
 * @rw: 데이터 방향(READ/WRITE, import_iovec에 그대로 전달)
 * @return: 0(성공) 또는 음수 오류(-EINVAL/-EFAULT 등 import 실패, 그 외
 *   blk_rq_map_user_iov/blk_rq_map_user가 반환하는 코드).
 *
 * SG_IO ioctl은 데이터 버퍼를 단일 포인터 또는 iovec 배열 두 가지 형태로
 * 받을 수 있어, 이 함수가 그 두 형태를 하나의 인터페이스로 통합해
 * blk_rq_map_user_iov()/blk_rq_map_user()에 위임한다. vec가 true면
 * import_iovec()으로 스택 fast_iov(UIO_FASTIOV개)를 우선 사용해 iov_iter를
 * 구성하고, SG_IO 관례상 "더 짧은 쪽이 이긴다"는 규칙에 따라
 * iov_iter_truncate(buf_len)로 길이를 맞춘다. vec가 false면 단순히
 * blk_rq_map_user()로 위임한다.
 * 실행 컨텍스트: 호출자의 프로세스 컨텍스트에서 슬립 가능하게 실행된다.
 * 호출자(caller): sg_io()/NVMe SG_IO 호환 ioctl 핸들러 등 SG_IO 스타일
 * passthrough 진입점.
 * 피호출자(callee): import_iovec(), iov_iter_truncate(), iov_iter_count(),
 * blk_rq_map_user_iov(), blk_rq_map_user(), kfree().
 * 에러 처리: import_iovec 실패나 truncate 후 빈 반복자는 즉시 오류 반환
 * (필요 시 kfree(iov)로 import_iovec이 힙에 할당했을 수 있는 iov 배열 해제).
 *
 * 호출 체인:
 *   sg_io -> [blk_rq_map_user_io] -> blk_rq_map_user_iov | blk_rq_map_user
 *
 * NVMe 연결:
 *   NVMe character/passthrough 장치를 통한 사용자 명령의 데이터 경로.
 */
int blk_rq_map_user_io(struct request *req, struct rq_map_data *map_data,
		void __user *ubuf, unsigned long buf_len, gfp_t gfp_mask,
		bool vec, int iov_count, bool check_iter_count, int rw)
{
	int ret = 0; /* 기본 성공값; vec도 아니고 buf_len도 0이면 아무 것도 하지 않고 이 값 그대로 반환 */

	if (vec) { /* iovec 배열 형태의 SG_IO 버퍼 처리 분기 */
		struct iovec fast_iov[UIO_FASTIOV]; /* 소수 iovec을 위한 스택 버퍼; import_iovec이 부족하면 힙으로 재할당 */
		struct iovec *iov = fast_iov; /* import_iovec에 전달할 포인터; 필요시 힙 배열로 교체됨 */
		struct iov_iter iter; /* import_iovec이 구성해 줄 iov_iter */

		ret = import_iovec(rw, ubuf, iov_count ? iov_count : buf_len, /* 사용자 iovec을 가져와 iov_iter 생성; rw는 NVMe READ/WRITE 방향 */
				UIO_FASTIOV, &iov, &iter);
		if (ret < 0) /* import 실패(잘못된 iovec 등) */
			return ret; /* iovec import 실패 — 사용자 iovec 배열 자체를 읽지 못했다 */

		if (iov_count) { /* 호출자가 명시적으로 iovec 개수를 지정한 경우에만 길이 재조정 */
			/* SG_IO howto says that the shorter of the two wins */
			iov_iter_truncate(&iter, buf_len); /* SG_IO 관례: iovec 합과 buf_len 중 짧은 쪽으로 자름 */
			if (check_iter_count && !iov_iter_count(&iter)) { /* 자른 뒤 남은 데이터가 없으면 전송할 것이 없음 */
				kfree(iov); /* import_iovec이 힙에 할당했을 수 있는 iov 배열 해제 */
				return -EINVAL; /* 빈 반복자는 NVMe 컨트롤러가 처리할 데이터 없음 */
			}
		}

		ret = blk_rq_map_user_iov(req->q, req, map_data, &iter, /* 변환된 iov_iter를 blk_rq_map_user_iov로 전달 */
				gfp_mask);
		kfree(iov); /* fast_iov를 썼다면 no-op, 힙 할당이었다면 여기서 해제 */
	} else if (buf_len) { /* 단일 연속 버퍼이고 길이가 0이 아닌 경우 */
		ret = blk_rq_map_user(req->q, req, map_data, ubuf, buf_len,
				gfp_mask);
	}
	return ret; /* 0 이면 매핑 완료, 음수면 실패. 실패 시 부분 매핑은 이미 내부에서 정리됐다 */
}
EXPORT_SYMBOL(blk_rq_map_user_io);

/**
 * blk_rq_unmap_user - unmap a request with user data
 * @bio:	       start of bio list
 *
 * Description:
 *    Unmap a rq previously mapped by blk_rq_map_user(). The caller must
 *    supply the original rq->bio from the blk_rq_map_user() return, since
 *    the I/O completion may have changed rq->bio.
 */
/*
 * [한국어]
 * blk_rq_unmap_user - 사용자 데이터가 매핑된 request를 언매핑
 *
 * @bio: bio 리스트의 시작(호출자가 blk_rq_map_user*() 호출 시점에 기록해
 *   둔 rq->bio 원본 — I/O 완료 후 rq->bio 자체는 바뀌었을 수 있으므로
 *   반드시 이 저장된 값을 넘겨야 한다)
 * @return: 0(모든 bio 정상 언매핑) 또는 첫 번째로 발생한 음수 오류
 *   (bio_uncopy_user가 반환한 -EINTR/-EFAULT). 호출자는 이 값을 그대로
 *   SG_IO 등 유저스페이스 오류로 보고한다.
 *
 * blk_rq_map_user_iov()가 만든 bio 체인은 종류가 섞여 있을 수 있다
 * (bounce-copy bio는 bi_private에 bio_map_data가, zero-copy bio는
 * bi_private이 NULL). 이 함수는 체인을 순회하며 각 bio 종류에 맞는 방식으로
 * 자원을 반납한다 — bounce-copy는 bio_uncopy_user()(READ면 사용자 공간
 * 복사 후 페이지 해제), zero-copy는 bio_release_pages()(pin 해제)를 호출한다.
 * 데이터 무결성(DIF/DIX) 메타데이터가 붙어 있으면 별도로
 * bio_integrity_unmap_user()도 호출한다. 마지막으로 각 bio 자체의 참조를
 * blk_mq_map_bio_put()으로 반환한다.
 * 실행 컨텍스트: I/O 완료를 기다리던 프로세스 컨텍스트(또는 완료를 처리
 * 하는 워크큐)에서 호출되며, 사용자 공간 복사가 필요할 수 있어 슬립
 * 가능한 컨텍스트여야 한다("아직 process context에 있어야 한다"는 원문
 * 주석 참고).
 * 호출자(caller): blk_rq_map_user_iov()의 실패 경로(unmap_rq), 그리고
 * NVMe/SCSI passthrough 완료 경로(정상 완료 시에도 항상 호출되어야 함).
 * 피호출자(callee): bio_uncopy_user(), bio_release_pages(),
 * bio_integrity_unmap_user(), blk_mq_map_bio_put().
 * 에러 처리: 개별 bio 언매핑 오류는 첫 번째 것만 보존해 반환하되, 나머지
 * bio들도 끝까지 순회하며 모두 정리한다(자원 누수 방지가 오류 보고보다
 * 우선).
 *
 * 호출 체인:
 *   nvme completion -> blk_mq_end_request -> [blk_rq_unmap_user]
 *     -> bio_uncopy_user | bio_release_pages
 *
 * NVMe 연결:
 *   NVMe CQ entry가 완료되고 request가 해제될 때, PRP/SGL에 사용된
 *   페이지들을 반납한다.
 */
int blk_rq_unmap_user(struct bio *bio)
{
	struct bio *next_bio; /* bio_put으로 해제되기 전에 다음 bio로 넘어가기 위해 현재 bio를 임시 보존 */
	int ret = 0, ret2; /* ret: 첫 번째 오류 저장; ret2: 각 bio 언매핑 결과 */

	while (bio) { /* bio 리스트를 순회하며 모두 정리; NVMe CQ entry 처리 후 request 해제 단계 */
		if (bio->bi_private) { /* bounce buffer bio는 bio_uncopy_user로 해제/복사 */
			ret2 = bio_uncopy_user(bio); /* READ면 사용자 공간 복사까지 수행하고 bio_map_data를 kfree */
			if (ret2 && !ret) /* 이미 이전 bio에서 오류를 기록했다면 최초 오류만 유지 */
				ret = ret2; /* 첫 오류만 보존; 상위 SG_IO는 이 오류를 수신 */
		} else {
			bio_release_pages(bio, bio_data_dir(bio) == READ); /* [한국어] pin해 두었던 사용자 페이지를 놓는다. READ였다면 커널이 쓴 내용이
							 * 있으므로 dirty로 표시해 회수 시 잃지 않게 한다 */
		}

		if (bio_integrity(bio)) /* [한국어] PI 메타데이터 버퍼는 데이터와 별도로 매핑되었으므로 따로 정리한다 */
			bio_integrity_unmap_user(bio);

		next_bio = bio; /* bio_put으로 해제되기 전에 현재 bio를 보존해 두어야 함 */
		bio = bio->bi_next; /* 다음 bio로 이동; NVMe 명령 하나가 여러 bio로 구성될 수 있음 */
		blk_mq_map_bio_put(next_bio); /* bio 참조 해제; NVMe CID에 대응하는 버퍼 자원 반납 */
	}

	return ret; /* 0이면 정상 해제; 비0이면 NVMe READ 사용자 공간 복사 등에서 문제 발생 */
}
EXPORT_SYMBOL(blk_rq_unmap_user);

/**
 * blk_rq_map_kern - map kernel data to a request, for passthrough requests
 * @rq:		request to fill
 * @kbuf:	the kernel buffer
 * @len:	length of user data
 * @gfp_mask:	memory allocation flags
 *
 * Description:
 *    Data will be mapped directly if possible. Otherwise a bounce
 *    buffer is used. Can be called multiple times to append multiple
 *    buffers.
 */
/*
 * [한국어]
 * blk_rq_map_kern - 커널 버퍼를 passthrough request에 매핑
 *
 * @rq: 대상 request. "여러 버퍼를 이어붙이기 위해 반복 호출 가능"(원문
 *   주석)하므로, 이미 bio가 연결된 request에 다시 호출하면 뒤에 이어진다.
 * @kbuf: 커널 버퍼(커널 가상 주소)
 * @len: 길이(바이트)
 * @gfp_mask: 메모리 할당 플래그
 * @return: 0(성공, bio가 rq에 연결됨) 또는 음수 오류(-EINVAL: 길이가
 *   max_hw_sectors 초과이거나 길이 0/NULL 버퍼, 그 외 bio_copy_kern/
 *   bio_map_kern/blk_rq_append_bio가 반환하는 코드).
 *
 * 커널 내부(예: NVMe 드라이버 자체 진단 명령, 커널 모듈이 발행하는
 * admin/IO passthrough)에서 사용자 공간을 거치지 않고 커널 버퍼를 직접
 * request에 매핑할 때 쓰는 진입점이다. 길이 검사(하드웨어 최대 전송 크기,
 * 0/NULL 거부) 후, blk_rq_aligned()(DMA 정렬 및 물리적 연속성 조건)와
 * object_is_on_stack()(스택 버퍼 여부)을 확인해 조건을 만족하면
 * bio_map_kern()(직접 매핑, 제로카피)을, 그렇지 않으면 bio_copy_kern()
 * (bounce buffer 복사)을 호출한다. 완성된 bio는 blk_rq_append_bio()로
 * request에 연결한다.
 * 실행 컨텍스트: 호출자의 컨텍스트에서 실행되며, alloc_page 등 슬립 가능한
 * 할당이 일어날 수 있으므로 슬립 가능한 컨텍스트에서 호출해야 한다.
 * 호출자(caller): 커널 내부 passthrough 발행자(예: NVMe 드라이버의 내부
 * admin 명령, SCSI 등 다른 서브시스템의 커널 패스스루 경로).
 * 피호출자(callee): queue_max_hw_sectors, blk_rq_aligned,
 * object_is_on_stack, bio_copy_kern, bio_map_kern, blk_rq_append_bio.
 * 에러 처리: 길이 검사 실패는 즉시 -EINVAL, bio 생성 실패는 IS_ERR/
 * PTR_ERR로 전파, append 실패는 만든 bio를 blk_mq_map_bio_put으로 반환한
 * 뒤 오류를 반환한다.
 *
 * 호출 체인:
 *   커널 passthrough 발행자 -> [blk_rq_map_kern]
 *     -> bio_map_kern | bio_copy_kern -> blk_rq_append_bio
 *     -> blk_mq_submit_bio -> blk_mq_get_request
 *     -> blk_execute_rq -> (디스패치) -> mq_ops->queue_rq (간접 호출)
 *
 * NVMe 연결:
 *   커널 드라이버나 nvme-cli가 발행하는 admin/IO 명령의 데이터 버퍼를
 *   NVMe 명령의 PRP/SGL로 연결하기 위한 진입점.
 */
int blk_rq_map_kern(struct request *rq, void *kbuf, unsigned int len,
		gfp_t gfp_mask)
{
	unsigned long addr = (unsigned long) kbuf; /* 커널 버퍼 가상 주소; DMA 정렬 및 가상 경계 검사에 사용 */
	struct bio *bio; /* bio_map_kern 또는 bio_copy_kern이 만들어 줄 bio */
	int ret; /* blk_rq_append_bio 결과 */

	if (len > (queue_max_hw_sectors(rq->q) << SECTOR_SHIFT)) /* [한국어] MDTS에서 유도된 max_hw_sectors를 초과하면 컨트롤러가 받지 못하므로 거부 */
		return -EINVAL;
	if (!len || !kbuf) /* 데이터 길이 0 또는 NULL 버퍼는 유효하지 않음; NVMe 컨트롤러가 데이터 없는 명령 외에는 거부 */
		return -EINVAL;

	if (!blk_rq_aligned(rq->q, addr, len) || object_is_on_stack(kbuf)) /* DMA 정렬/스택 버퍼는 직접 매핑 불가 -> bounce buffer */
		bio = bio_copy_kern(rq, kbuf, len, gfp_mask); /* 복사 경로: 커널 페이지에 데이터를 복사한 bio 생성; NVMe PRP/SGL에 적합한 메모리 */
	else /* 정렬/연속 조건을 만족하는 일반 커널 버퍼 */
		bio = bio_map_kern(rq, kbuf, len, gfp_mask); /* [한국어] 직접 매핑 경로: 복사 없이 커널 버퍼를 그대로 bio에 연결한다 */

	if (IS_ERR(bio)) /* 두 경로 모두 에러 포인터를 반환할 수 있음 */
		return PTR_ERR(bio); /* bio 구성 실패 — 에러 포인터를 그대로 전달 */

	ret = blk_rq_append_bio(rq, bio); /* bio 를 request 에 연결. 여기서 큐 한계 검사도 함께 이뤄진다 */
	if (unlikely(ret)) /* append 실패(하드웨어 제약 위반 등) - 흔치 않은 경로 */
		blk_mq_map_bio_put(bio); /* 연결 실패 시 bio 를 반환해 누수를 막는다 */
	return ret; /* 0 이면 매핑 완료, 비0 이면 실패 */
}
EXPORT_SYMBOL(blk_rq_map_kern);

/* NVMe 관점 핵심 요약 */

/*
 * - 본 파일은 상위 계층의 사용자/커널 버퍼를 bio로 변환하여 NVMe PRP/SGL의
 *   기반이 되는 페이지 목록을 준비한다.
 * - DMA 정렬, 가상 경계, max_hw_sectors 등의 제약을 검사/우회하여
 *   NVMe 컨트롤러가 처리할 수 있는 형태로 bio를 정리한다.
 * - blk_rq_append_bio를 거쳐 request에 bio가 연결되면, 이후
 *   호출자가 blk_execute_rq 등으로 제출하면 그때 드라이버 태그가 붙고
 *   mq_ops->queue_rq 를 통해 장치로 내려간다.
 * - READ 완료 시 CQ 처리 후 blk_rq_unmap_user -> bio_uncopy_user를 통해
 *   사용자 공간으로 데이터가 복사되거나 페이지가 반납된다.
 * - block/blk-merge.c의 bio 병합/분할과 밀접하게 연결되며, 본 파일이
 *   생성한 bio는 이후 병합 단계에서 추가 정제될 수 있다.
 */
