// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2001 Jens Axboe <axboe@kernel.dk>
 */
/*
 * [한국어 설명] bio.c — 블록 계층의 근본 자료구조 struct bio 의 생명주기 구현 (bio.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Linux 블록 I/O 서브시스템의 핵심 자료구조인 struct bio 의
 * 전체 생명주기(할당·초기화·재사용·복제·절단·완료·해제)를 구현한다.
 * bio(Block I/O descriptor)는 "어느 블록 장치(bi_bdev)의 어느 섹터부터
 * (bi_iter.bi_sector) 몇 바이트를(bi_iter.bi_size) 어떤 메모리 페이지들에
 * (bi_io_vec[]) 읽거나 쓴다"만을 기술하는 **장치 중립적** 컨테이너다.
 * bio 자체에는 NVMe·SCSI·virtio 어느 프로토콜 지식도 들어 있지 않다.
 * 프로토콜 변환은 훨씬 뒤 단계 — blk-mq 가 bio 를 struct request 로 바꾸고,
 * 드라이버의 mq_ops->queue_rq 콜백이 그 request 를 자기 프로토콜의 명령으로
 * 조립할 때 — 에서 비로소 일어난다.
 * 이 파일이 실제로 책임지는 것은 다음 네 가지다.
 *   (1) bio 와 bio_vec 배열의 메모리 확보/반환 (슬랩 + mempool + per-CPU 캐시)
 *   (2) bio 에 페이지를 채워 넣는 API (bio_add_page / bio_iov_iter_get_pages 계열)
 *   (3) 부모-자식 bio 체인과 참조 카운트 (bio_chain / bio_endio / bio_put)
 *   (4) 메모리 부족 상황에서도 I/O 가 영원히 멈추지 않게 하는 세 겹의 안전망
 *       (per-CPU 캐시 → 슬랩 → mempool, 그리고 rescuer 워크큐)
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (위 → 아래). bio.c 가 직접 부르는 것은 화살표 두 칸까지이고,
 * 그 아래는 **간접 호출(mq_ops->queue_rq 함수 포인터)**로만 이어진다:
 *
 *   파일 시스템(ext4/xfs/btrfs) / kiocb Direct I/O / dm·md
 *     → bio_alloc_bioset(), bio_add_page(), bio_iov_iter_get_pages()  ← 이 파일
 *     → submit_bio() / submit_bio_noacct()                            ← blk-core.c
 *     → blk_mq_submit_bio() : bio → struct request 변환, 병합·분할 결정 ← blk-mq.c
 *         · 분할이 필요하면 bio_split_to_limits() (blk-merge.c) 가 판정하고,
 *           실제 절단은 이 파일의 bio_split() 이 기계적으로 수행한다.
 *     → blk_mq_dispatch_rq_list() → q->mq_ops->queue_rq(rq)   ← **함수 포인터**
 *         · 이 지점부터가 드라이버 영역이다. NVMe PCIe 라면 nvme_queue_rq()
 *           가 등록되어 있고, 거기서 nvme_setup_cmd()(drivers/nvme/host/core.c)
 *           가 rq 를 NVMe SQE 로 조립하고 blk_rq_dma_map()(block/blk-mq-dma.c)
 *           결과가 PRP 또는 SGL 필드가 된다.
 *         · **bio.c 안에는 nvme_* 심볼이 하나도 없다.** 블록 계층은 드라이버를
 *           직접 호출하지 않는다.
 *     → (장치 완료) → blk_mq_end_request() → bio_endio()              ← 이 파일
 *     → bio->bi_end_io() 콜백 → 파일 시스템/DIO 완료 경로
 *
 * 실행 컨텍스트:
 *   - 할당·페이지 추가·제출 경로: 프로세스(태스크) 컨텍스트. 블로킹 GFP 사용 가능.
 *   - 완료 경로(bio_endio, bio_put, bio_remaining_done, __bio_chain_endio):
 *     드라이버 완료 처리기에서 불려오므로 **하드 IRQ 컨텍스트에서도 실행**된다.
 *     그래서 이 경로에는 잠들 수 있는 연산(GFP_KERNEL 할당, mutex)이 없다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈 (bio.c 가 호출하는 쪽):
 *   - include/linux/blk_types.h : struct bio, bio_vec, bvec_iter, BIO_* 플래그 정의
 *   - block/blk.h               : bio_set, blk_throtl_bio, submit_bio_noacct_nocheck 등 내부 API
 *   - block/blk-cgroup.c        : bio_associate_blkg(), blkg_put() — cgroup(blkcg) 소유권 부착
 *   - block/bio-integrity.c     : bio_integrity_free/clone/advance — T10-PI 메타데이터 부착
 *   - block/blk-crypto.c        : bio_crypt_clone/advance/free_ctx — inline 암호화 컨텍스트
 *   - block/blk-rq-qos.c        : rq_qos_done_bio() — wbt/iolatency/iocost 완료 훅
 *   - mm/, lib/iov_iter.c       : 페이지 할당, iov_iter_extract_pages() 로 사용자 페이지 pin
 *
 * bio.c 에 의존하는 쪽:
 *   - 모든 파일 시스템, Direct I/O(fs/direct-io.c, fs/iomap/), swap, dm/md/bcache,
 *     block/blk-mq.c, block/blk-merge.c, block/blk-lib.c(discard/zeroout),
 *     그리고 자기만의 bio_set 을 만드는 스택 드라이버들.
 *
 * 데이터 흐름 (NVMe 독자용 큰 그림 — 각 단계가 어느 파일 소관인지 유의):
 *   사용자 버퍼(iov_iter)
 *     → [bio.c] bio_iov_iter_get_pages(): 페이지를 pin 하고 bio_vec 배열에 기록
 *     → [blk-mq.c] bio → request 로 승격, bio_vec 배열은 rq->bio 로 그대로 매달림
 *     → [blk-mq-dma.c] blk_rq_dma_map(): bio_vec 를 순회하며 DMA 주소를 얻음
 *     → [drivers/nvme/host/pci.c] 그 DMA 주소들을 PRP 리스트 또는 SGL 디스크립터로 기록
 *     → 장치 DMA → 완료 인터럽트 → [blk-mq.c] blk_mq_end_request()
 *     → [bio.c] bio_endio() → bi_end_io() → 상위 계층 완료
 *   즉 **bio_vec 하나가 PRP 엔트리 하나인 것이 아니다.** bio_vec 는
 *   (page, offset, len) 삼중항이라 여러 페이지에 걸칠 수 있고, PRP 는 반대로
 *   컨트롤러 페이지(NVME_CTRL_PAGE_SIZE, 보통 4KiB) 단위의 주소 목록이다.
 *   둘 사이의 변환은 bio.c 가 아니라 blk-mq-dma.c + nvme 드라이버가 한다.
 *
 * 공유 핵심 자료구조:
 *   struct bio       : I/O 요청 단위 — 이 파일이 생성·소멸을 책임짐
 *   struct bio_set   : bio + bio_vec 메모리 풀 — 전역 fs_bio_set 및 드라이버 전용 풀.
 *                      front_pad 로 "드라이버 구조체 + bio" 를 한 덩어리로 할당한다.
 *   struct bio_vec   : (page, offset, len) — 물리적으로 연속인 한 조각의 데이터 버퍼
 *   struct bvec_iter : bio 내 현재 위치(bi_sector / bi_size / bi_idx / bi_bvec_done).
 *                      부분 완료·분할·advance 는 모두 이 iterator 만 움직인다.
 *
 * === 주요 함수/구조체 요약 ===
 * bio_alloc_bioset()      : per-CPU 캐시 → 슬랩 → (실패 시) mempool 순서로 bio 할당
 * bio_init()              : bio 의 모든 필드를 알려진 초기 상태로 설정
 * bio_add_page()          : 페이지 하나를 bio_vec 에 추가(직전 bvec 와 물리 인접이면 병합)
 * bio_iov_iter_get_pages(): 사용자 iov 버퍼의 페이지를 pin 하고 bvec 배열을 채움
 * bio_chain()             : 자식 bio 의 완료를 부모 bio 의 __bi_remaining 으로 집계
 * bio_endio()             : 완료 통보 — 체인 카운트를 내리고 bi_end_io 콜백 호출
 * bio_put()               : __bi_cnt 참조 감소, 0 이면 per-CPU 캐시 또는 풀로 반환
 * bio_split()             : bio 를 sectors 경계에서 둘로 기계적 절단(정책 판단 없음)
 * bio_iov_iter_bounce()   : 정렬 요구를 못 맞추는 사용자 버퍼용 바운스 버퍼 구성
 * bioset_init()           : bio_set(슬랩 + mempool + rescuer + per-CPU 캐시) 초기화
 *
 * struct bio_alloc_cache  : per-CPU bio 재사용 캐시. free_list(태스크) 와
 *                           free_list_irq(하드 IRQ) 두 리스트로 락 없이 동작한다.
 * struct biovec_slab      : bio_vec 배열용 크기별(16/64/128/256) 슬랩 테이블
 */
#include <linux/mm.h>          /* [한국어] struct page/folio, alloc_page(), page_to_phys(), memzero_page().
                                 * bio 는 데이터를 "페이지 + 오프셋 + 길이"로 기술하므로 mm 의 페이지 개념이 전제된다.
                                 * page_to_phys() 는 bvec_try_merge_page() 에서 두 페이지가 물리적으로
                                 * 인접한지 판정할 때 쓰인다(주소 비교용이며, DMA 주소 변환은 여기서 하지 않는다). */
#include <linux/swap.h>         /* [한국어] 메모리 회수 경로(shrink_list 등): bio 할당 시 OOM reclaim과 상호작용 */
#include <linux/bio-integrity.h>/* [한국어] bio integrity(T10-PI) API 선언: bio_integrity(), bio_integrity_free(),
                                 * bio_integrity_clone(), bio_integrity_advance().
                                 * bio.c 는 integrity 를 "bio 에 딸린 부속 자원"으로만 다룬다 —
                                 * 복제 시 같이 복제하고, 해제 시 같이 해제하고, advance 시 같이 전진시킨다.
                                 * 실제 PI 튜플 생성·검증은 block/bio-integrity.c 와 block/t10-pi.c 소관. */
#include <linux/blkdev.h>       /* [한국어] struct block_device / struct request_queue / struct queue_limits 및
                                 * submit_bio() 선언. bio->bi_bdev 가 가리키는 대상 타입과,
                                 * bdev_nr_sectors()(guard_bio_eod), bdev_get_queue() 등이 여기서 온다. */
#include <linux/uio.h>          /* [한국어] struct iov_iter 와 iov_iter_extract_pages()/copy_to_iter() 계열.
                                 * Direct I/O 가 넘겨준 사용자 버퍼 기술자를 bio_vec 배열로 옮기는
                                 * bio_iov_iter_get_pages() / bio_iov_iter_bounce() 가 이 API에 전적으로 의존한다. */
#include <linux/iocontext.h>    /* [한국어] I/O 컨텍스트(I/O 스케줄러 힌트, CFQ elevator): blkcg 및 I/O 우선순위 관리에 사용 */
#include <linux/slab.h>         /* [한국어] kmem_cache_create/alloc/free 와 kmalloc/kfree.
                                 * bio 본체는 bio_set 마다 만들어진 전용 kmem_cache(bs->bio_slab)에서,
                                 * bio_vec 배열은 크기별 공용 슬랩(bvec_slabs[])에서 나온다. */
#include <linux/init.h>         /* [한국어] subsys_initcall/module_init 매크로: init_bio()를 커널 부팅 시 자동 실행 */
#include <linux/kernel.h>       /* [한국어] 범용 커널 유틸(BUG_ON, WARN_ON, min/max 등): 불변조건 검사 및 공통 연산 */
#include <linux/export.h>       /* [한국어] EXPORT_SYMBOL: bio_alloc_bioset, bio_endio 등 공개 API를 모듈에서 사용 가능하게 함 */
#include <linux/mempool.h>      /* [한국어] mempool_t — 미리 확보해 둔 예비 원소를 가진 "절대 실패하지 않는" 할당기.
                                 * 블록 계층은 "메모리를 회수하려면 먼저 I/O 를 내려보내야 하는데,
                                 * 그 I/O 를 내려보내려면 bio 를 할당해야 한다"는 순환 의존이 있어
                                 * 일반 슬랩만으로는 교착할 수 있다. mempool 이 그 순환을 끊는 1차 방어선이다. */
#include <linux/workqueue.h>    /* [한국어] work_struct/workqueue: bio_alloc_rescue 워크큐; mempool 고갈 시 교착 상태 방지용 rescuer */
#include <linux/cgroup.h>       /* [한국어] cgroup 기본 자료구조. bio 는 자신을 발행한 cgroup 을
                                 * bi_blkg(blkcg_gq) 로 붙들고 다니며, 이는 blk-throttle/iocost/iolatency 가
                                 * "이 I/O 는 누구 것인가"를 판단하는 근거가 된다. 장치 종류와는 무관하다. */
#include <linux/highmem.h>      /* [한국어] highmem 페이지 매핑(kmap_local_page 등): 32비트 커널에서 4GB 초과 물리 페이지 접근 */
#include <linux/blk-crypto.h>   /* [한국어] inline 암호화(blk-crypto) 컨텍스트 API.
                                 * bio 에 bi_crypt_context 가 붙어 있으면 복제/전진/해제 시 함께 처리해야 한다.
                                 * 참고: 이 트리의 drivers/nvme/host/ 에는 blk_crypto 참조가 하나도 없다
                                 * (grep 확인). inline 암호화는 UFS/eMMC 계열 하드웨어 기능이며,
                                 * NVMe 의 자가암호화(TCG Opal)는 block/sed-opal.c 로 전혀 다른 경로다. */
#include <linux/xarray.h>       /* [한국어] XArray(radix-tree 대체): bio_slabs XArray로 크기별 kmem_cache를 O(1)에 조회 */
#include <linux/kmemleak.h>     /* [한국어] 메모리 누수 감지기(kmemleak): per-CPU cache에서 bio 할당/해제 시 추적 등록/해제 */

#include <trace/events/block.h> /* [한국어] 블록 계층 tracepoint 정의(trace_block_bio_complete, trace_block_split).
                                 * ftrace/perf/blktrace 가 bio 완료·분할 시점을 관측하는 지점이다. */
#include "blk.h"                /* [한국어] blk 내부 헤더: bio_set, request_queue 내부 구조 및 blk_*() 함수 선언 */
#include "blk-rq-qos.h"         /* [한국어] request QoS 훅(rq_qos_done_bio 등): bio 완료 시 latency/cost 통계 갱신 */
#include "blk-cgroup.h"         /* [한국어] blkcg 내부 API: bio_associate_blkg(), bio_clone_blkg_association(), blkg_put().
                                 * bio 생성 시 현재 태스크의 blkcg 를 붙이고, 해제 시 참조를 놓는다. */

#define ALLOC_CACHE_THRESHOLD	16  /* [한국어] free_list_irq 에 이만큼 쌓이면 free_list 로 통째 이동(splice)한다.
                                     * 완료 인터럽트 핸들러는 IRQ 비활성 구간을 짧게 유지해야 하므로
                                     * 반납은 free_list_irq 에 O(1) 로만 하고, 실제 이동은 다음 할당 때
                                     * 태스크 컨텍스트에서 한 번에 처리한다. 16 은 "IRQ 구간을 너무 자주
                                     * 잡지 않으면서도 캐시가 놀지 않는" 경험적 임계값이다. */
#define ALLOC_CACHE_MAX		256 /* [한국어] per-CPU 캐시가 보관하는 bio 개수 상한(free_list + free_list_irq 합).
                                     * 넘어서면 bio_put_percpu_cache() 가 캐시에 넣지 않고 bio_free() 로
                                     * 슬랩/mempool 에 곧장 돌려준다. CPU 수 × 256 만큼 메모리가 묶이는
                                     * 것을 막는 상한이다. */

/*
 * [한국어] struct bio_alloc_cache — bio 재사용을 위한 per-CPU 캐시
 *
 * 왜 필요한가: 초당 수십만 건의 I/O 를 내는 워크로드(고속 SSD 대상 Direct I/O 등)에서는
 * bio 한 개당 kmem_cache_alloc/free 왕복 비용과 그에 딸린 슬랩 내부 동기화가
 * 눈에 띄는 오버헤드가 된다. 이 캐시는 방금 완료된 bio 를 해제하지 않고 현재 CPU 에
 * 붙들어 두었다가 다음 할당에 그대로 재사용해 그 왕복을 없앤다.
 *
 * 왜 두 개의 리스트인가: bio 반납(bio_put)은 드라이버 완료 처리기 때문에
 * **하드 IRQ 컨텍스트에서도** 일어난다. 태스크 컨텍스트와 IRQ 컨텍스트가 같은
 * 리스트를 건드리면 그 리스트는 매번 local_irq_save() 로 보호해야 한다.
 * 그래서 리스트를 둘로 나눠, IRQ 쪽은 free_list_irq 에만 넣고(짧은 IRQ 비활성 구간),
 * 태스크 쪽은 free_list 만 쓰며(선점 비활성만으로 충분), 둘의 합류는
 * ALLOC_CACHE_THRESHOLD 를 넘었을 때 bio_alloc_irq_cache_splice() 가 한 번에 한다.
 *
 * 이 캐시는 bio_set 이 BIOSET_PERCPU_CACHE 로 초기화된 경우에만 존재하고,
 * 할당 요청이 REQ_ALLOC_CACHE 플래그를 달고 있을 때만 사용된다.
 * 접근 동기화는 락이 아니라 "현재 CPU 로 한정 + 선점/IRQ 비활성"으로 이루어진다.
 */
struct bio_alloc_cache {
	struct bio		*free_list;
	/*
	 * [한국어] 태스크 컨텍스트에서 재사용할 bio 연결 리스트(bi_next로 연결).
	 * 설정자: bio_put_percpu_cache()가 in_task() 상황에서 회수한 bio를 여기 저장.
	 * 읽는 자: bio_alloc_percpu_cache()가 할당 시 이 리스트에서 꺼냄.
	 * 값 범위: NULL(비어 있음) 또는 nr개의 bio 연결 리스트.
	 * 동기화: get_cpu/put_cpu로 선점이 비활성화된 상태에서만 접근.
	 */
	struct bio		*free_list_irq;
	/*
	 * [한국어] 하드 IRQ 컨텍스트(NVMe 완료 ISR 등)에서 회수된 bio 연결 리스트.
	 * 설정자: bio_put_percpu_cache()가 in_hardirq() 상황에서 회수한 bio를 여기 저장.
	 * 읽는 자: bio_alloc_irq_cache_splice()가 free_list_irq → free_list로 이전.
	 * 값 범위: NULL(비어 있음) 또는 nr_irq개의 bio 연결 리스트.
	 * 동기화: local_irq_save/restore로 IRQ가 비활성화된 상태에서만 접근.
	 */
	unsigned int		nr;
	/*
	 * [한국어] free_list에 저장된 bio 개수.
	 * 설정자: bio_alloc_percpu_cache()가 꺼낼 때 감소, bio_put_percpu_cache()가 저장 시 증가.
	 * 읽는 자: bio_alloc_percpu_cache()와 bio_put_percpu_cache()가 캐시 가득 참 여부 판단에 사용.
	 * 값 범위: 0 ~ ALLOC_CACHE_MAX; 초과 시 bio_free()로 반납.
	 * 동기화: get_cpu/put_cpu 선점 비활성화 구간에서 nr + nr_irq를 합산해 판단.
	 */
	unsigned int		nr_irq;
	/*
	 * [한국어] free_list_irq에 저장된 bio 개수.
	 * 설정자: bio_put_percpu_cache()가 in_hardirq() 시 증가.
	 * 읽는 자: bio_alloc_irq_cache_splice()가 splice 조건 확인 시 READ_ONCE로 읽음.
	 * 값 범위: 0 ~ ALLOC_CACHE_MAX; ALLOC_CACHE_THRESHOLD 이상이면 splice 트리거.
	 * 동기화: local_irq_save/restore 구간에서만 수정; READ_ONCE로 stale 읽기 방지.
	 */
};

#define BIO_INLINE_VECS 4 /* [한국어] bio 본체 뒤(back_pad)에 인라인으로 딸려 오는 bio_vec 개수.
                           * bio_set 은 sizeof(struct bio) + front_pad + BIO_INLINE_VECS*sizeof(bio_vec)
                           * 크기의 슬랩 객체를 만든다(bs_bio_slab_size 참조). 그래서 세그먼트가
                           * 4개 이하인 bio 는 bio_vec 배열을 위한 **두 번째 할당이 아예 없다**.
                           * 파일시스템의 단일 페이지 읽기/쓰기, 저널 커밋, 메타데이터 I/O 대부분이
                           * 여기 들어간다. 5개 이상이 필요하면 bvec_slabs[] 또는 mempool 로 넘어간다. */

/*
 * [한국어] bvec_slabs[] — bio_vec 배열용 크기 등급별 슬랩 테이블
 *
 * bio 가 필요로 하는 bio_vec 개수는 1개부터 256개까지 천차만별이다. 매 크기마다
 * 전용 캐시를 두면 슬랩 종류가 폭증하고, 항상 최대 크기를 주면 메모리를 낭비한다.
 * 그래서 16 / 64 / 128 / 256 네 등급으로 반올림해 담는다(최악의 경우 4배 내부 단편화).
 *
 * bio_vec 개수와 장치 한계의 관계 (NVMe 독자 주의):
 *   bio 가 담을 수 있는 세그먼트 수(bi_max_vecs)와, 장치가 한 명령에 받아들일 수 있는
 *   세그먼트 수(queue_limits.max_segments)는 **별개의 값**이다. bio.c 는 전자만 안다.
 *   후자를 검사해 bio 를 쪼개는 일은 block/blk-merge.c 의 bio_split_to_limits() →
 *   bio_split_rw() 가 blk_mq_submit_bio() 경로에서 수행한다.
 *   NVMe PCIe 에서 후자가 어떻게 정해지는지는 이 트리에서 확인할 수 있다:
 *     drivers/nvme/host/pci.c        : dev->ctrl.max_segments = NVME_MAX_SEGS
 *     drivers/nvme/host/core.c:2437  : lim->max_segments =
 *         min(USHRT_MAX, min_not_zero(nvme_max_drv_segments(ctrl), ctrl->max_segments))
 *   여기서 nvme_max_drv_segments() 는 MDTS 로부터 유도된 max_hw_sectors 를
 *   컨트롤러 페이지 크기로 나눈 값이다. 즉 NVMe 의 실효 max_segments 는
 *   "MDTS 유래 값"과 "NVME_MAX_SEGS(=256)" 중 작은 쪽이다.
 */
static struct biovec_slab {
	/* [한국어] 이 슬랩이 담당하는 bio_vec 배열의 원소 개수(상한).
	 * 설정자: 아래 초기화 리스트에서 컴파일 타임에 고정.
	 * 읽는 자: bvec_alloc()/bvec_free()가 슬랩 크기를 되돌려 계산할 때,
	 *   그리고 bio_alloc_bioset()이 bi_max_vecs를 정할 때 참조한다.
	 * 값 범위: 16 / 64 / 128 / BIO_MAX_VECS(=256, include/linux/bio.h) 네 단계.
	 * 동기화: 부팅 시 한 번만 쓰이고 이후 읽기 전용이라 락이 필요 없다. */
	int nr_vecs;
	/* [한국어] slab 캐시 이름. /proc/slabinfo와 slabtop에 이 이름으로 나타나
	 * 어느 크기 등급이 얼마나 쓰이는지 관찰할 수 있다.
	 * 설정자: 아래 초기화 리스트(컴파일 타임 문자열 리터럴).
	 * 읽는 자: biovec_init_pool()의 kmem_cache_create(). */
	char *name;
	/* [한국어] 실제 slab 캐시 포인터. NULL이면 아직 생성되지 않은 상태다.
	 * 설정자: 부팅 시 biovec_init_pool()이 kmem_cache_create() 결과를 대입.
	 * 읽는 자: bvec_alloc()이 kmem_cache_alloc(), bvec_free()가
	 *   kmem_cache_free()를 호출할 때.
	 * 동기화: 초기화 이후 변하지 않으므로 락 없이 읽어도 안전하다. */
	struct kmem_cache *slab;
	/* [한국어] __read_mostly: 이 배열을 "거의 읽기만 하는" 데이터 섹션에 배치한다.
	 * 자주 쓰이는 다른 변수와 같은 캐시 라인에 놓이면, 그 변수가 갱신될 때마다
	 * 이 배열까지 캐시에서 무효화되어(false sharing) 성능이 떨어진다. 별도
	 * 섹션에 모아 두면 그런 간섭이 사라진다. bio 할당은 I/O마다 일어나는
	 * 핫패스라 이 배치가 의미 있다. */
} bvec_slabs[] __read_mostly = {
	/* [한국어] 크기 등급을 4단계로 나눈 이유: 모든 bio에 최대 크기(256개) 배열을
	 * 주면 대부분을 낭비하고, 정확한 크기마다 캐시를 만들면 캐시가 너무 많아진다.
	 * 4단계는 메모리 낭비(최악 4배)와 캐시 종류 수 사이의 타협점이다.
	 *
	 * 16개 = 최대 64KiB(4KiB 페이지 기준) I/O. 파일시스템의 일반적인 읽기/쓰기와
	 * 페이지 캐시 write-back 대부분이 이 등급에 들어간다. */
	{ .nr_vecs = 16, .name = "biovec-16" },
	/* [한국어] 64개 = 최대 256KiB. 대용량 순차 I/O나 read-ahead가 여기 해당한다. */
	{ .nr_vecs = 64, .name = "biovec-64" },
	/* [한국어] 128개 = 최대 512KiB(4KiB 페이지 기준, 병합이 전혀 없을 때).
	 * 세그먼트가 이 등급을 넘길 정도로 많아지면 장치의 max_segments 를 초과할
	 * 가능성이 커지고, 그때는 blk-merge.c 의 bio_split_rw() 가 bio 를 쪼갠다. */
	{ .nr_vecs = 128, .name = "biovec-128" },
	/* [한국어] BIO_MAX_VECS = bio 하나가 가질 수 있는 세그먼트 수의 절대 상한.
	 * include/linux/bio.h 에 `#define BIO_MAX_VECS 256U` 로 하드코딩된 블록 계층
	 * 고유의 상수이며, 특정 장치나 프로토콜에서 유도된 값이 아니다.
	 * 흩어진 사용자 버퍼를 O_DIRECT 로 제출하는 등 극단적 스캐터에서 이 등급이 쓰인다.
	 *
	 * NVMe 와의 수치 일치에 대하여: NVME_MAX_SEGS 도 256 이지만
	 * (drivers/nvme/host/pci.c: NVME_CTRL_PAGE_SIZE(4096) / sizeof(struct nvme_sgl_desc)(16)),
	 * 이는 "SGL 디스크립터 페이지 한 장에 몇 개가 들어가는가"에서 나온 값이고
	 * BIO_MAX_VECS 는 블록 계층이 임의로 정한 상한이다. 두 값이 같은 것은
	 * 서로 인과 관계가 없는 **수치상의 우연**이며, 한쪽이 다른 쪽을 참조하지 않는다.
	 * (드라이버 소스 어디에도 BIO_MAX_VECS 를 근거로 삼는 코드는 없다.) */
	{ .nr_vecs = BIO_MAX_VECS, .name = "biovec-max" },
};

/*
 * [한국어]
 * biovec_slab - 요청한 segment 수에 맞는 bio_vec 슬랩 포인터를 반환한다
 *
 * @nr_vecs: 필요한 bio_vec 배열의 크기 (최소 5, 최대 BIO_MAX_VECS)
 * @return:  해당 크기를 수용하는 biovec_slab 포인터; 범위 초과 시 BUG() 후 NULL
 *
 * 목적: bio_alloc_bioset()이 bio_vec 배열을 위한 적절한 슬랩 캐시를 선택할 때 사용한다.
 * nr_vecs에 따라 16/64/128/BIO_MAX_VECS 4단계 중 가장 작은 적합 슬랩을 반환한다.
 * 실행 컨텍스트: 태스크 컨텍스트; get_cpu/put_cpu 구간에서도 호출될 수 있다.
 * 호출자: bio_alloc_bioset(), bio_free()
 *
 * 호출 체인:
 *   bio_alloc_bioset() → [biovec_slab()] → bvec_slabs[n].slab
 *   bio_free()         → [biovec_slab()] → kmem_cache_free()
 *
 * 주의: 이 함수는 "몇 개까지 담을 그릇을 줄 것인가"만 정한다. 장치가 실제로
 * 그 세그먼트 수를 받아들일 수 있는지(queue_limits.max_segments)는 여기서
 * 전혀 보지 않으며, 그 판단은 제출 경로의 blk-merge.c 가 한다.
 */
static struct biovec_slab *biovec_slab(unsigned short nr_vecs)
{
	switch (nr_vecs) {
	/* smaller bios use inline vecs */
	case 5 ... 16:   /* [한국어] 5~16 세그먼트: BIO_INLINE_VECS(4)를 갓 넘긴 일반적인 크기.
	                  * 파일시스템 read-ahead 한 묶음, 페이지 캐시 write-back 대부분이 여기.
	                  * biovec-16 슬랩(16 × 16바이트 = 256바이트) 선택. */
		return &bvec_slabs[0];
	case 17 ... 64:  /* [한국어] 17~64 세그먼트: 중간 크기 I/O. 병합이 잘 안 된 순차 쓰기,
	                  * 여러 페이지에 걸친 Direct I/O 등. biovec-64 슬랩 선택. */
		return &bvec_slabs[1];
	case 65 ... 128: /* [한국어] 65~128 세그먼트: 대용량 I/O. biovec-128 슬랩 선택. */
		return &bvec_slabs[2];
	case 129 ... BIO_MAX_VECS: /* [한국어] 129~256 세그먼트: 상한 등급. 흩어진 사용자 버퍼를
	                            * 대량으로 모은 O_DIRECT 등. biovec-max 슬랩 선택. */
		return &bvec_slabs[3];
	default:
		BUG(); /* [한국어] 0~4 또는 256 초과는 이 함수에 오면 안 된다.
		        * 0~4 는 호출자가 BIO_INLINE_VECS 경로로 걸러내고,
		        * 256 초과는 bio_alloc_bioset()의 BUG_ON(nr_vecs > BIO_MAX_VECS)에서
		        * 이미 막힌다. 여기 도달했다면 호출자 쪽 불변조건이 깨진 것이므로
		        * 잘못된 크기의 배열을 넘겨주는 대신 즉시 커널을 멈춘다. */
		return NULL; /* [한국어] BUG()는 반환하지 않으므로 도달 불가.
		              * "제어가 함수 끝에 도달한다"는 컴파일러 경고만 억제한다. */
	}
}

/*
 * [한국어] struct bio_set — bio 와 bio_vec 을 공급하는 "메모리 풀 묶음"
 * (실제 정의: include/linux/blk_types.h; 여기서는 이 파일이 실제로 만지는 필드만 설명)
 *
 * ── 슬랩 객체의 메모리 레이아웃 ──────────────────────────────────────
 * bio_set 이 만드는 슬랩 객체 한 개는 다음과 같이 **세 부분이 한 덩어리**다
 * (bs_bio_slab_size() 참조):
 *
 *     ┌───────────── front_pad ─────────────┬─ struct bio ─┬─ back_pad ─┐
 *     │  호출자(스택 드라이버)의 전용 구조체  │   bio 본체    │ inline bvec │
 *     └─────────────────────────────────────┴──────────────┴────────────┘
 *      ↑ p (슬랩이 돌려준 주소)               ↑ bio = p + front_pad
 *
 *   - bio_alloc_bioset() 은 슬랩에서 p 를 받아 `bio = p + bs->front_pad` 로 되돌린다.
 *   - bio_free()/bio_slab_addr() 은 반대로 `p = bio - front_pad` 로 원주소를 복원한다.
 *   - front_pad 는 호출자가 bioset_init() 에 넘긴 값 그대로다.
 *
 * ── front_pad 관례: "내 구조체의 마지막 멤버로 bio 를 둔다" ────────────
 *   bio 를 직접 만드는 계층(파일시스템 DIO, dm/md 같은 스택 드라이버)은
 *   "이 bio 는 어느 상위 요청의 일부인가"를 기억할 자기 구조체가 필요하다.
 *   그 구조체와 bio 를 따로 할당하면 할당이 두 번이고 캐시 미스도 두 번이다.
 *   그래서 자기 구조체의 **마지막 멤버로 struct bio 를 두고**,
 *   front_pad 를 `offsetof(내 구조체, bio)` 로 지정해 한 번에 할당한다.
 *   완료 콜백에서는 container_of(bio, 내 구조체, bio) 로 되돌아간다.
 *
 *   이 트리의 실제 사례 — block/fops.c:2167 (blkdev_init):
 *       bioset_init(&blkdev_dio_pool, 4,
 *                   offsetof(struct blkdev_dio, bio),
 *                   BIOSET_NEED_BVECS | BIOSET_PERCPU_CACHE);
 *   즉 struct blkdev_dio(블록 장치 Direct I/O 상태)와 그 첫 bio 가 항상
 *   한 슬랩 객체 안에 함께 산다.
 *
 *   NVMe 독자 주의: **nvme 드라이버는 bio_set 을 만들지 않는다.**
 *   nvme 는 blk-mq request 기반 드라이버라 bio 를 할당할 일이 없고,
 *   자기 per-request 데이터(struct nvme_iod)는 bio_set 의 front_pad 가 아니라
 *   blk-mq 태그셋의 cmd_size 로 request 뒤에 붙인다(전혀 다른 메커니즘).
 *   이 트리의 drivers/nvme/host/ 전체에 front_pad/bioset 참조는 0건이다(grep 확인).
 *
 * ── 주요 필드 ─────────────────────────────────────────────────────────
 * bio_slab 필드:
 *   [한국어] 위 레이아웃 크기(front_pad+bio+back_pad)의 전용 kmem_cache.
 *   설정자: bioset_init() → bio_find_or_create_slab().
 *   읽는 자: bio_alloc_bioset()의 kmem_cache_alloc(), bio_free()의 kmem_cache_free().
 *   값 범위: 유효한 kmem_cache 포인터. 같은 크기면 여러 bio_set 이 공유한다.
 *   동기화: 생성/파괴만 bio_slab_lock 뮤텍스로 직렬화되고, 사용은 락 없이 한다.
 *
 * bio_pool 필드:
 *   [한국어] struct bio 본체의 **예비 재고**를 들고 있는 mempool.
 *   설정자: bioset_init()의 mempool_init_slab_pool()이 pool_size 개를 미리 확보.
 *   읽는 자: bio_alloc_bioset()이 일반 슬랩 할당(GFP_NOWAIT)에 실패했을 때만 사용.
 *   값 범위: 최소 pool_size 개를 보장. 고갈되면 잠들어 기다린다(GFP_KERNEL 경로).
 *   동기화: mempool 내부 스핀락 + waitqueue.
 *   존재 이유: 메모리 회수는 "더러운 페이지를 장치에 써서 내보내기"를 필요로 하고,
 *   그러려면 bio 가 있어야 한다. 일반 할당기만 쓰면 이 순환이 교착으로 굳는다.
 *
 * bvec_pool 필드:
 *   [한국어] bio_vec 배열의 예비 재고 mempool. 원소 하나의 크기는
 *   BIO_MAX_VECS 개짜리 배열(최대 크기)이라 어떤 요청이든 수용할 수 있다.
 *   설정자: bioset_init() → biovec_init_pool(). BIOSET_NEED_BVECS 일 때만.
 *   읽는 자: bio_alloc_bioset()의 fallback 경로. 이때 nr_vecs 는 BIO_MAX_VECS 로
 *           끌어올려지고, 그 사실은 bi_max_vecs == BIO_MAX_VECS 로 기록되어
 *           bio_free() 가 "슬랩이 아니라 mempool 로 돌려줘야 한다"를 판단하는 근거가 된다.
 *   동기화: mempool 내부 스핀락.
 *
 * front_pad / back_pad 필드:
 *   [한국어] 위 레이아웃 그림 참조. front_pad 는 호출자 전용 공간(바이트),
 *   back_pad 는 BIO_INLINE_VECS 개의 bio_vec 을 담는 공간이다.
 *   설정자: bioset_init(). back_pad 는 BIOSET_NEED_BVECS 플래그 유무로 결정된다.
 *   읽는 자: bs_bio_slab_size(), bio_slab_addr(), bio_alloc_bioset(), bio_free().
 *   값 범위: front_pad 는 0 이상(0 이면 bio 만 할당), back_pad 는 0 또는
 *           BIO_INLINE_VECS*sizeof(struct bio_vec).
 *   동기화: 초기화 후 불변.
 *
 * cache 필드:
 *   [한국어] per-CPU struct bio_alloc_cache 배열 포인터(BIOSET_PERCPU_CACHE 일 때만 non-NULL).
 *   설정자: bioset_init()의 alloc_percpu(); bioset_exit() 이 free_percpu().
 *   읽는 자: bio_alloc_percpu_cache()(할당), bio_put_percpu_cache()(반납),
 *           bio_cpu_dead()(CPU 오프라인 시 그 CPU 캐시 비우기).
 *   값 범위: NULL 이거나 유효한 per-CPU 포인터.
 *   동기화: 락이 아니라 "현재 CPU 한정 + 선점/IRQ 비활성"으로 보호한다.
 *
 * rescue_workqueue / rescue_list / rescue_work / rescue_lock 필드:
 *   [한국어] BIOSET_NEED_RESCUER 로 활성화되는 교착 탈출 장치.
 *   punt_bios_to_rescuer() 가 현재 태스크의 bio_list 에 갇혀 있던 bio 들을
 *   rescue_list 로 옮기고 워커를 깨우면, bio_alloc_rescue() 가 **다른 스레드에서**
 *   그것들을 제출한다. 자세한 시나리오는 punt_bios_to_rescuer() 주석 참조.
 *   동기화: rescue_list 는 rescue_lock 스핀락으로 보호.
 */
/*
 * fs_bio_set is the bio_set containing bio and iovec memory pools used by
 * IO code that does not need private memory pools.
 */
struct bio_set fs_bio_set;
EXPORT_SYMBOL(fs_bio_set);

/*
 * Our slab pool management
 */
/*
 * [한국어] bio_slab — struct bio 본체를 위한 슬랩 디스크립터
 *
 * 같은 크기(front_pad + sizeof(bio) + back_pad)의 슬랩을 여러 bio_set이 공유한다.
 * XArray(bio_slabs)로 크기→slab 매핑을 관리하고, slab_ref로 참조 카운트를 추적한다.
 */
struct bio_slab {
	struct kmem_cache *slab;
	/*
	 * [한국어] bio 본체를 위한 kmem_cache 포인터.
	 * 설정자: create_bio_slab()의 kmem_cache_create()가 초기화.
	 * 읽는 자: bio_alloc_bioset()이 kmem_cache_alloc()으로 bio를 할당.
	 * 동기화: bio_slab_lock 뮤텍스로 보호되는 생성/해제 구간 외에는 읽기 전용.
	 */
	unsigned int slab_ref;
	/*
	 * [한국어] 이 bio_slab을 공유하는 bio_set 개수(참조 카운트).
	 * 설정자: bio_find_or_create_slab()이 새 참조 시 증가, bio_put_slab()이 감소.
	 * 값 범위: 1 이상; 0이 되면 kmem_cache_destroy()로 슬랩 파괴.
	 * 동기화: bio_slab_lock 뮤텍스 보호.
	 */
	unsigned int slab_size;
	/*
	 * [한국어] 이 슬랩 캐시의 오브젝트 크기 = front_pad + sizeof(struct bio) + back_pad.
	 * 설정자: create_bio_slab()이 인자로 받은 size 를 그대로 저장.
	 * 읽는 자: bio_find_or_create_slab()/bio_put_slab()이 XArray 조회 키로 사용.
	 * 값 범위: 최소 sizeof(struct bio). front_pad 가 큰 호출자일수록 커진다.
	 * 이 값이 곧 XArray bio_slabs 의 인덱스이므로, front_pad 크기가 같은
	 * 서로 다른 bio_set 들은 자동으로 같은 슬랩을 공유하게 된다.
	 * 동기화: bio_slab_lock 보호 하에서만 생성/조회.
	 */
	char name[12];
	/*
	 * [한국어] 슬랩 캐시 이름 (예: "bio-256"). /proc/slabinfo에 표시됨.
	 * 설정자: create_bio_slab()의 snprintf().
	 * 읽는 자: kmem_cache_create()가 슬랩 이름으로 등록.
	 */
};
static DEFINE_MUTEX(bio_slab_lock); /* [한국어] bio_slabs XArray 조회/삽입/삭제와 slab_ref 증감을 직렬화하는 뮤텍스.
                                     * "찾아보고 없으면 만든다"(bio_find_or_create_slab)와
                                     * "참조를 줄이고 0이면 파괴한다"(bio_put_slab)는 둘 다
                                     * 검사-후-변경(check-then-act) 이라 원자적으로 묶여야 한다.
                                     * 스핀락이 아니라 뮤텍스인 이유: 내부에서 kmem_cache_create()/
                                     * kmem_cache_destroy() 같은 잠들 수 있는 연산을 호출하기 때문이다.
                                     * 따라서 이 락은 bio_set 생성/소멸 경로(모듈 로드·언로드, 파일시스템
                                     * 마운트 등)에서만 잡히고, I/O 핫패스에서는 절대 잡히지 않는다. */
static DEFINE_XARRAY(bio_slabs);    /* [한국어] 슬랩 크기 → bio_slab 포인터 매핑을 위한 XArray.
                                     * 동일 크기의 bio 슬랩을 여러 bio_set이 공유할 때 기존 슬랩을 재사용하여
                                     * 메모리를 절약하고 /proc/slabinfo 항목 수를 최소화한다. */

/*
 * [한국어]
 * create_bio_slab - 새로운 크기의 bio 슬랩 캐시를 생성하고 XArray에 등록한다
 *
 * @size: 슬랩 오브젝트 크기 = front_pad + sizeof(struct bio) + back_pad
 * @return: 새로 생성된 bio_slab 포인터; 메모리 부족 시 NULL
 *
 * bio_find_or_create_slab()이 XArray에서 해당 크기의 슬랩을 찾지 못할 때 호출된다.
 * SLAB_TYPESAFE_BY_RCU: 해제된 오브젝트가 RCU grace period 전에는 **다른 타입으로**
 *   재사용되지 않음을 보장한다. 즉 락 없이 bio 포인터를 따라간 코드가 "이미 해제된
 *   메모리"를 보더라도 그것은 여전히 struct bio 형태이므로, 필드를 읽는 행위 자체가
 *   메모리 오염을 일으키지 않는다(내용은 신뢰할 수 없으므로 재검증이 필요하다).
 * SLAB_HWCACHE_ALIGN: 오브젝트를 캐시라인 경계에 맞춘다. bio 는 I/O 마다 할당·해제되는
 *   핫 오브젝트라, 서로 다른 CPU 가 다루는 두 bio 가 한 캐시라인을 공유해 생기는
 *   false sharing 을 피할 가치가 있다.
 * 실행 컨텍스트: bio_slab_lock 뮤텍스를 보유한 상태에서만 호출됨.
 * 호출자: bio_find_or_create_slab()
 * 에러 경로: kmem_cache_create 실패 → fail_alloc_slab, xa_store 실패 → 슬랩 파괴 후 NULL
 *
 * 호출 체인:
 *   bioset_init() → bio_find_or_create_slab() → [create_bio_slab()]
 */
static struct bio_slab *create_bio_slab(unsigned int size)
{
	struct bio_slab *bslab = kzalloc_obj(*bslab); /* [한국어] bio_slab 디스크립터를 0으로 초기화하여 할당 */

	if (!bslab)
		return NULL; /* [한국어] 메모리 부족: bio_set 초기화 실패 전파 */

	snprintf(bslab->name, sizeof(bslab->name), "bio-%d", size); /* [한국어] "bio-256" 등 크기 포함 슬랩 이름 생성 */
	bslab->slab = kmem_cache_create(bslab->name, size,          /* [한국어] size 크기의 bio 슬랩 캐시 생성 */
			ARCH_KMALLOC_MINALIGN,
			SLAB_HWCACHE_ALIGN | SLAB_TYPESAFE_BY_RCU, NULL);
	if (!bslab->slab)
		goto fail_alloc_slab; /* [한국어] 슬랩 생성 실패: bslab 해제 후 NULL 반환 */

	bslab->slab_ref = 1;    /* [한국어] 첫 번째 bio_set이 참조: ref=1로 초기화 */
	bslab->slab_size = size; /* [한국어] XArray 키로도 사용될 크기를 저장 */

	if (!xa_err(xa_store(&bio_slabs, size, bslab, GFP_KERNEL))) /* [한국어] 크기 → bio_slab 매핑을 XArray에 등록 */
		return bslab; /* [한국어] 성공: 호출자에게 bio_slab 반환 */

	kmem_cache_destroy(bslab->slab); /* [한국어] xa_store 실패 시 방금 만든 슬랩 파괴 */

fail_alloc_slab:
	kfree(bslab); /* [한국어] bio_slab 디스크립터 해제 */
	return NULL;  /* [한국어] 생성 실패: bioset_init()이 -ENOMEM으로 처리 */
}

/*
 * [한국어]
 * bs_bio_slab_size - bio_set에서 사용할 슬랩 오브젝트 크기를 계산한다
 *
 * @bs: 크기를 계산할 bio_set
 * @return: front_pad + sizeof(struct bio) + back_pad 바이트
 *
 * 이 크기가 XArray bio_slabs의 키로 사용되어 동일 크기의 슬랩을 공유한다.
 * 호출 체인: bio_find_or_create_slab()/bio_put_slab() → [bs_bio_slab_size()]
 */
static inline unsigned int bs_bio_slab_size(struct bio_set *bs)
{
	return bs->front_pad + sizeof(struct bio) + bs->back_pad; /* [한국어] front_pad(드라이버 임베드 공간) + bio 본체 + back_pad(inline bvec) */
}

/*
 * [한국어]
 * bio_slab_addr - bio 포인터에서 슬랩 오브젝트 시작 주소를 역산한다
 *
 * @bio: 슬랩 주소를 구할 bio 포인터
 * @return: 슬랩 오브젝트의 시작 주소 (bio - front_pad)
 *
 * kmem_cache_free()/mempool_free()는 슬랩 오브젝트 시작 주소를 인자로 받는다.
 * bio 포인터는 front_pad 뒤에 위치하므로 front_pad를 빼야 실제 시작 주소가 된다.
 * kmemleak 추적도 이 주소 기준으로 등록/해제된다.
 * 호출 체인: bio_free()/bio_alloc_percpu_cache()/bio_put_percpu_cache() → [bio_slab_addr()]
 */
static inline void *bio_slab_addr(struct bio *bio)
{
	return (void *)bio - bio->bi_pool->front_pad; /* [한국어] bio - front_pad = 슬랩 오브젝트 시작; kmemleak/mempool 반납에 사용 */
}

/*
 * [한국어]
 * bio_find_or_create_slab - bio_set에 맞는 슬랩을 찾거나 새로 생성한다
 *
 * @bs: 슬랩을 찾거나 생성할 bio_set
 * @return: 해당 크기의 kmem_cache 포인터; 실패 시 NULL
 *
 * 동일 크기의 슬랩이 이미 있으면 slab_ref를 증가시켜 공유한다.
 * 없으면 create_bio_slab()으로 새 슬랩을 만들어 XArray에 등록한다.
 * 실행 컨텍스트: bio_slab_lock 뮤텍스로 직렬화됨.
 * 호출자: bioset_init()
 *
 * 호출 체인:
 *   bioset_init() → [bio_find_or_create_slab()] → create_bio_slab()
 */
static struct kmem_cache *bio_find_or_create_slab(struct bio_set *bs)
{
	unsigned int size = bs_bio_slab_size(bs); /* [한국어] front_pad+bio+back_pad 크기를 XArray 키로 계산 */
	struct bio_slab *bslab;

	mutex_lock(&bio_slab_lock);          /* [한국어] bio_slabs XArray와 slab_ref 증감을 직렬화 */
	bslab = xa_load(&bio_slabs, size);   /* [한국어] 동일 크기의 기존 슬랩이 있는지 XArray에서 조회 */
	if (bslab)
		bslab->slab_ref++;           /* [한국어] 기존 슬랩 공유: 참조 카운트 증가 */
	else
		bslab = create_bio_slab(size); /* [한국어] 새 크기: 슬랩 캐시를 새로 생성하고 XArray에 등록 */
	mutex_unlock(&bio_slab_lock);        /* [한국어] 뮤텍스 해제: 이후에는 읽기 전용으로 slap 사용 */

	if (bslab)
		return bslab->slab; /* [한국어] 성공: bio 본체 할당에 사용할 kmem_cache 반환 */
	return NULL; /* [한국어] 슬랩 생성 실패: bioset_init()이 -ENOMEM으로 처리 */
}

/*
 * [한국어]
 * bio_put_slab - bio_set이 사용하던 슬랩의 참조를 해제하고, 마지막이면 파괴한다
 *
 * @bs: 슬랩을 반납할 bio_set
 *
 * bioset_exit() 시 호출되어 slab_ref를 1 감소시킨다.
 * 0이 되면 XArray에서 제거하고 kmem_cache를 파괴한다.
 * 실행 컨텍스트: bio_slab_lock 뮤텍스로 직렬화됨.
 * 호출자: bioset_exit()
 *
 * 호출 체인:
 *   bioset_exit() → [bio_put_slab()] → kmem_cache_destroy()
 */
static void bio_put_slab(struct bio_set *bs)
{
	struct bio_slab *bslab = NULL;
	unsigned int slab_size = bs_bio_slab_size(bs); /* [한국어] XArray 키로 사용할 크기를 역산 */

	mutex_lock(&bio_slab_lock); /* [한국어] slab_ref 감소와 파괴를 원자적으로 처리 */

	bslab = xa_load(&bio_slabs, slab_size); /* [한국어] 반납할 슬랩을 XArray에서 찾음 */
	if (WARN(!bslab, KERN_ERR "bio: unable to find slab!\n"))
		goto out; /* [한국어] 슬랩을 찾지 못함: 버그지만 락 해제 후 반환 */

	WARN_ON_ONCE(bslab->slab != bs->bio_slab); /* [한국어] bio_set의 슬랩과 XArray의 슬랩이 일치해야 함 */

	WARN_ON(!bslab->slab_ref); /* [한국어] ref가 이미 0이면 이중 해제 버그 */

	if (--bslab->slab_ref) /* [한국어] 다른 bio_set이 아직 이 슬랩을 사용 중: 파괴하지 않음 */
		goto out; /* [한국어] slab_ref > 0: 공유 중이므로 XArray에 계속 유지 */

	xa_erase(&bio_slabs, slab_size); /* [한국어] 마지막 참조자: XArray에서 매핑 제거 */

	kmem_cache_destroy(bslab->slab); /* [한국어] kmem_cache 파괴: 이 크기의 bio 슬랩이 더 이상 불필요 */
	kfree(bslab); /* [한국어] bio_slab 디스크립터 해제 */

out:
	mutex_unlock(&bio_slab_lock); /* [한국어] 뮤텍스 해제: 다른 bio_set의 슬랩 조작 허용 */
}

/*
 * Make the first allocation restricted and don't dump info on allocation
 * failures, since we'll fall back to the mempool in case of failure.
 */
/*
 * [한국어]
 * try_alloc_gfp - 첫 번째 할당 시 mempool fallback을 위한 완화된 GFP 플래그를 반환한다
 *
 * @gfp: 원래 GFP 플래그
 * @return: 직접 회수(__GFP_DIRECT_RECLAIM)와 I/O(__GFP_IO)를 제거하고
 *          __GFP_NOMEMALLOC | __GFP_NORETRY | __GFP_NOWARN을 추가한 플래그
 *
 * bio_alloc_bioset()의 첫 시도에서 실패해도 mempool로 fallback하므로,
 * OOM killer 호출 없이 빠르게 실패할 수 있도록 GFP를 제한한다.
 * 호출자: bio_alloc_bioset()
 */
static inline gfp_t try_alloc_gfp(gfp_t gfp)
{
	return (gfp & ~(__GFP_DIRECT_RECLAIM | __GFP_IO)) | /* [한국어] 직접 회수·I/O 제거: 슬랩 실패 시 빠르게 포기하고 mempool로 이동 */
		__GFP_NOMEMALLOC | __GFP_NORETRY | __GFP_NOWARN; /* [한국어] mempool 예약 메모리 사용 금지·재시도 없음·경고 억제 */
}

/*
 * [한국어]
 * bio_uninit - bio 수명 종료 시 cgroup, integrity, crypto 컨텍스트를 정리한다
 *
 * @bio: 정리할 bio 포인터
 *
 * bio가 재사용(bio_reset/bio_reuse)되거나 해제(bio_free)되기 전에 반드시 호출해야 한다.
 * cgroup 참조(bi_blkg), T10-PI 보호 정보(bi_integrity), inline 암호화 컨텍스트(bi_crypt_context)
 * 세 가지를 각각 해제한다.
 * 이 세 자원은 모두 "bio 보다 오래 살 수 있는 것에 대한 참조"라서, 놓치면 곧바로
 * 누수가 된다(blkcg_gq 는 참조 카운트, integrity payload 와 crypt ctx 는 별도 할당 메모리).
 * bio 를 재사용하는 경로(bio_reset/bio_reuse)에서도 반드시 먼저 불러야, 이전 I/O 의
 * 메타데이터가 다음 I/O 에 딸려 가는 일이 없다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트 및 하드 IRQ 컨텍스트(bio_put_percpu_cache 경로).
 *   따라서 여기서 부르는 함수들은 모두 잠들지 않아야 한다.
 * 호출자: bio_free(), bio_reset(), bio_reuse(), bio_put_percpu_cache(), 그리고
 *   스택에 bio 를 잡는 코드(bdev_rw_virt 등)가 직접.
 *
 * 호출 체인:
 *   blk_mq_end_request() → bio_endio() → bio_put() → bio_free() → [bio_uninit()]
 */
void bio_uninit(struct bio *bio)
{
#ifdef CONFIG_BLK_CGROUP
	/* [한국어] CONFIG_BLK_CGROUP 이 꺼진 커널에는 bi_blkg 필드 자체가 없으므로 통째로 배제한다. */
	if (bio->bi_blkg) {	/* [한국어] bio_associate_blkg()가 붙여 준 cgroup 소유권이 있는지 확인 */
		blkg_put(bio->bi_blkg);  /* [한국어] blkcg_gq 참조 카운트 감소.
		                          * 이걸 빠뜨리면 cgroup 을 지워도 blkg 가 남아
		                          * cgroup 소멸이 영원히 지연된다. */
		bio->bi_blkg = NULL;	/* [한국어] 이중 put 방지 — 재사용 경로에서 다시 불려도 안전하게 만든다 */
	}
#endif
	if (bio_integrity(bio))	/* [한국어] bio->bi_integrity(T10-PI 페이로드)가 붙어 있는지 검사.
	                         * CONFIG_BLK_DEV_INTEGRITY 가 꺼져 있으면 항상 false 인 매크로다. */
		bio_integrity_free(bio);	/* [한국어] PI 버퍼와 bip 구조체를 해제 (block/bio-integrity.c) */

	bio_crypt_free_ctx(bio);	/* [한국어] inline 암호화 키 컨텍스트 해제.
	                             * bi_crypt_context 가 NULL 이면 아무 일도 하지 않는 no-op 이라
	                             * 위 두 경우와 달리 별도 검사가 필요 없다. */
}
EXPORT_SYMBOL(bio_uninit);	/* [한국어] bio 를 자기 메모리에 직접 잡는 파일시스템/스택 드라이버 모듈이
                             * bio_init() 과 짝을 맞추기 위해 호출할 수 있도록 공개한다. */

/*
 * [한국어]
 * bio_free - bio 본체와 bio_vec 메모리를 풀로 반환한다
 *
 * @bio: 해제할 bio 포인터
 *
 * bi_max_vecs 값에 따라 세 가지 경로로 bio_vec를 반납한다:
 *   - BIO_MAX_VECS: mempool에 반환 (메모리 압박 보장 할당 경로)
 *   - BIO_INLINE_VECS 초과: 해당 크기의 biovec 슬랩 캐시에 반환
 *   - BIO_INLINE_VECS 이하: bio 본체 안에 인라인이므로 별도 반환 불필요
 * bio 본체는 항상 bi_pool->bio_pool(mempool)에 반환된다.
 * 핵심 아이디어: 할당 경로가 어디에서 bvec 을 가져왔는지를 **bi_max_vecs 값 자체가
 * 기억한다**. 별도의 플래그가 없다. mempool 로 fallback 한 경우 nr_vecs 를
 * BIO_MAX_VECS 로 올려 놓기 때문에(bio_alloc_bioset 참조), 여기서
 * bi_max_vecs == BIO_MAX_VECS 라는 사실만으로 "mempool 에서 왔다"를 알 수 있다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트 및 하드 IRQ 컨텍스트.
 * 호출자: bio_put(), __bio_alloc_cache_prune()
 *
 * 호출 체인:
 *   bio_put() → [bio_free()] → bio_uninit() → mempool_free() / kmem_cache_free()
 */
static void bio_free(struct bio *bio)
{
	struct bio_set *bs = bio->bi_pool;	/* [한국어] 이 bio 가 태어난 풀. bio_init()이 NULL 로 두므로,
	                                     * bi_pool 이 non-NULL 이라는 것은 bio_alloc_bioset() 이
	                                     * 할당했다는 뜻이다(= 여기서 해제해도 되는 bio). */
	void *p = bio;	/* [한국어] 아래에서 front_pad 를 빼기 위해 void* 로 받아 둔다.
	                 * struct bio* 로 뺄셈하면 sizeof(struct bio) 단위가 되어 버린다. */

	WARN_ON_ONCE(!bs);	/* [한국어] bi_pool 이 NULL 인 bio 가 여기 오면 안 된다.
	                     * bio_kmalloc()/스택 bio 는 bio_free() 대상이 아니다. */
	WARN_ON_ONCE(bio->bi_max_vecs > BIO_MAX_VECS);	/* [한국어] 아래 분기가 세 경우를 모두 덮으려면
	                                                 * bi_max_vecs 가 상한 이내여야 한다.
	                                                 * 넘었다면 어딘가에서 필드가 깨진 것이다. */

	bio_uninit(bio);	/* [한국어] cgroup/integrity/crypto 부속 자원 먼저 정리.
	                     * 메모리를 풀에 돌려준 뒤에는 bio 필드를 읽을 수 없으므로 순서가 중요하다. */
	if (bio->bi_max_vecs == BIO_MAX_VECS)	/* [한국어] 경로 (1): mempool fallback 으로 받은 최대 크기 배열 */
		mempool_free(bio->bi_io_vec, &bs->bvec_pool);	/* [한국어] 예비 재고로 되돌린다.
		                                                 * mempool 은 대기자가 있으면 곧바로 깨운다. */
	else if (bio->bi_max_vecs > BIO_INLINE_VECS)	/* [한국어] 경로 (2): 5~128 개 — 크기별 슬랩에서 왔다 */
		kmem_cache_free(biovec_slab(bio->bi_max_vecs)->slab,	/* [한국어] bi_max_vecs 로 어느 등급이었는지
		                                                         * 되짚어 같은 슬랩에 돌려준다. */
				bio->bi_io_vec);
	/* [한국어] 경로 (3): bi_max_vecs <= BIO_INLINE_VECS 는 bio 본체 뒤(back_pad)의
	 * 인라인 배열이므로 따로 해제할 것이 없다 — 아래 mempool_free 한 번에 함께 사라진다. */
	mempool_free(p - bs->front_pad, &bs->bio_pool);	/* [한국어] bio 본체 반납.
	                                                 * 슬랩 객체의 시작은 bio 가 아니라 front_pad 앞이므로
	                                                 * 반드시 front_pad 만큼 되돌려 원래 주소로 만든다.
	                                                 * (bio_slab_addr()과 같은 계산을 인라인으로 한 것)
	                                                 * bio_pool 은 슬랩 기반 mempool 이라, 예비 재고가
	                                                 * 차 있으면 곧바로 kmem_cache_free 로 흘려보낸다. */
}

/*
 * Users of this function have their own bio allocation. Subsequently,
 * they must remember to pair any call to bio_init() with bio_uninit()
 * when IO has completed, or when the bio is released.
 */
/*
 * [한국어]
 * bio_init - bio 구조체를 0으로 초기화하고 필수 필드를 설정한다
 *
 * @bio:      초기화할 bio 포인터 (호출자가 메모리를 소유)
 * @bdev:     이 bio 가 I/O 를 수행할 블록 장치; NULL 가능(나중에 bio_set_dev 로 지정)
 * @table:    bio_vec 배열 포인터; BIO_INLINE_VECS 이하면 bio 본체 뒤의 인라인 배열
 * @max_vecs: table 이 담을 수 있는 bio_vec 개수
 * @opf:      연산 종류 + 플래그 (REQ_OP_READ / REQ_OP_WRITE / REQ_OP_DISCARD | REQ_SYNC …)
 *
 * bio 를 "아직 아무 데이터도 담기지 않았지만 언제든 add_page 할 수 있는" 상태로 만든다.
 * memset 대신 필드를 하나하나 대입하는 이유: struct bio 에는 이 함수가 건드리지 않는
 * 필드(bi_io_vec 뒤의 인라인 배열 등)가 있고, 컴파일러가 개별 대입을 더 잘 최적화하며,
 * 무엇보다 "어떤 필드가 초기화 대상인지"가 코드에 명시적으로 남기 때문이다.
 *
 * 왜 별도 함수인가: bio 메모리를 스스로 마련하는 호출자(스택 위의 bio, 구조체에 임베드한
 * bio, bio_kmalloc 결과)도 같은 초기화가 필요하다. 그래서 할당(bio_alloc_bioset)과
 * 초기화(bio_init)가 분리되어 있고, 짝이 되는 정리 함수는 bio_free 가 아니라 bio_uninit 이다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트. bio_associate_blkg() 가 현재 태스크의 cgroup 을 보므로
 *   "이 bio 를 발행하는 주체" 컨텍스트에서 불려야 의미가 있다.
 * 호출자: bio_alloc_bioset(), bio_reset(), bio_init_clone(), bdev_rw_virt() 등
 *
 * 호출 체인:
 *   파일시스템/DIO → bio_alloc_bioset() → [bio_init()] → (호출자가 bi_sector/페이지 채움)
 */
void bio_init(struct bio *bio, struct block_device *bdev, struct bio_vec *table,
	      unsigned short max_vecs, blk_opf_t opf)
{
	bio->bi_next = NULL;	/* [한국어] bio 를 리스트로 엮는 링크. 제출 경로에서 current->bio_list,
	                         * plug 리스트, blk-mq request 안의 bio 체인이 모두 이 필드를 쓴다.
	                         * 새 bio 는 어느 리스트에도 속하지 않으므로 NULL. */
	bio->bi_bdev = bdev;	/* [한국어] 대상 블록 장치(파티션 포함). bio_associate_blkg() 와
	                         * guard_bio_eod(), submit_bio() 의 큐 조회가 모두 여기서 출발한다. */
	bio->bi_opf = opf;	/* [한국어] 하위 8비트가 연산 종류(REQ_OP_*), 나머지가 플래그(REQ_SYNC,
	                     * REQ_FUA, REQ_META, REQ_ALLOC_CACHE …). 이 값이 최종적으로
	                     * request->cmd_flags 가 되고, 드라이버가 자기 프로토콜의 opcode 로 번역한다. */
	bio->bi_flags = 0;	/* [한국어] BIO_* 상태 비트(BIO_CLONED, BIO_PAGE_PINNED, BIO_CHAIN,
	                     * BIO_REFFED, BIO_QUIET …) 전체 초기화. 잔재가 남으면 완료 경로가
	                     * 없는 자원을 해제하려 든다. */
	bio->bi_ioprio = 0;	/* [한국어] ioprio_set(2)/IOCB 우선순위. 0 = "지정 없음"이며
	                     * 나중에 blk-ioprio 나 제출자가 덮어쓸 수 있다. */
	bio->bi_write_hint = 0;	/* [한국어] 쓰기 수명 힌트(temperature). 장치가 데이터 배치에 참고한다. */
	bio->bi_write_stream = 0;	/* [한국어] 쓰기 스트림 식별자. 같은 스트림끼리 모아 쓰도록 하는 힌트. */
	bio->bi_status = 0;	/* [한국어] blk_status_t. 0 == BLK_STS_OK.
	                     * 완료 시 드라이버가 에러를 여기에 적고, bio_endio() 가 상위로 전달한다. */
	bio->bi_bvec_gap_bit = 0;	/* [한국어] bvec 사이에 가상주소 갭이 생겼는지 기록하는 캐시 비트.
	                             * blk-merge 의 virt_boundary 검사가 매번 재계산하지 않도록 돕는다. */
	bio->bi_iter.bi_sector = 0;	/* [한국어] 시작 섹터(512바이트 단위). 호출자가 곧 덮어쓴다. */
	bio->bi_iter.bi_size = 0;	/* [한국어] 남은 바이트 수. add_page 할 때마다 늘고,
	                             * 완료·advance 할 때마다 준다. 0 이면 "다 처리됨". */
	bio->bi_iter.bi_idx = 0;	/* [한국어] 현재 처리 중인 bi_io_vec[] 인덱스. */
	bio->bi_iter.bi_bvec_done = 0;	/* [한국어] 현재 bvec 안에서 이미 처리된 바이트 수.
	                                 * bi_idx 와 함께 "부분 완료" 위치를 정확히 가리킨다. */
	bio->bi_end_io = NULL;	/* [한국어] 완료 콜백. bio_endio() 가 마지막에 호출한다.
	                         * NULL 이면 아무 통보 없이 끝난다. */
	bio->bi_private = NULL;	/* [한국어] bi_end_io 콜백이 쓰라고 비워 둔 자리.
	                         * 보통 상위 요청 구조체 포인터나 completion 을 담는다. */
#ifdef CONFIG_BLK_CGROUP
	/* [한국어] blkcg(블록 cgroup)가 빌드에 포함된 경우에만 존재하는 필드들. */
	bio->bi_blkg = NULL;	/* [한국어] 소유 cgroup 의 blkcg_gq. 아래 bio_associate_blkg 가 채운다. */
	bio->issue_time_ns = 0;	/* [한국어] iolatency/iocost 가 지연을 재기 위해 찍는 발행 시각. */
	if (bdev)	/* [한국어] 장치가 정해져야 blkg(= cgroup × 큐 조합)를 찾을 수 있다.
	             * bdev==NULL 이면 나중에 bio_set_dev() 시점에 연결된다. */
		bio_associate_blkg(bio);	/* [한국어] 현재 태스크의 blkcg 를 이 bio 에 붙이고 참조를 잡는다.
		                             * blk-throttle/iocost/iolatency 가 "누구의 I/O 인가"를
		                             * 판단하는 근거가 여기서 확정된다. 짝은 bio_uninit()의 blkg_put(). */
#ifdef CONFIG_BLK_CGROUP_IOCOST
	bio->bi_iocost_cost = 0;	/* [한국어] iocost 컨트롤러가 산정한 이 I/O 의 비용 누적치. */
#endif
#endif
#ifdef CONFIG_BLK_INLINE_ENCRYPTION
	bio->bi_crypt_context = NULL;	/* [한국어] inline 암호화 키/IV 컨텍스트. 미사용이면 NULL. */
#endif
#ifdef CONFIG_BLK_DEV_INTEGRITY
	bio->bi_integrity = NULL;	/* [한국어] T10-PI 페이로드(struct bio_integrity_payload).
	                             * bio_integrity_alloc()(block/bio-integrity.c)이 나중에 채운다. */
#endif
	bio->bi_vcnt = 0;	/* [한국어] 현재까지 채워진 bi_io_vec[] 원소 수. bio_add_page 가 늘린다. */

	atomic_set(&bio->__bi_remaining, 1);	/* [한국어] **체인 카운트**. "이 bio 가 완료되려면 몇 건의
	                                         * 완료 통보가 더 필요한가". 자기 자신 몫으로 1 에서 시작한다.
	                                         * bio_chain() 이 자식을 붙일 때마다 +1 되고,
	                                         * bio_endio() 가 한 번 불릴 때마다 -1 되어 0 이 될 때만
	                                         * 진짜 완료로 처리된다. 참조 카운트(__bi_cnt)와는 다른 개념. */
	atomic_set(&bio->__bi_cnt, 1);	/* [한국어] **참조 카운트**. "이 메모리를 몇 명이 붙들고 있는가".
	                                 * bio_get() 이 +1, bio_put() 이 -1 하며 0 이 되면 메모리를 반납한다.
	                                 * __bi_remaining 이 0 이 되어도(=I/O 완료) 참조가 남아 있으면
	                                 * 메모리는 살아 있다. */
	bio->bi_cookie = BLK_QC_T_NONE;	/* [한국어] 폴링용 쿠키. submit_bio() 가 반환하는 이 값을
	                                 * bio_poll() 에 넘겨 해당 하드웨어 큐를 직접 긁는다(하이브리드/
	                                 * 인터럽트 없는 완료 처리). 아직 제출 전이므로 "없음". */

	bio->bi_max_vecs = max_vecs;	/* [한국어] table 이 담을 수 있는 bio_vec 개수(용량).
	                                 * bi_vcnt(현재 개수)와 짝을 이루며, bio_full() 이 둘을 비교한다.
	                                 * bio_free() 는 이 값으로 "어느 풀에서 왔는지"까지 역산한다. */
	bio->bi_io_vec = table;	/* [한국어] bio_vec 배열 본체. 인라인이면 bio 바로 뒤 주소,
	                         * 별도 할당이면 슬랩/mempool 주소, 클론이면 원본의 배열. */
	bio->bi_pool = NULL;	/* [한국어] "이 bio 는 어느 bio_set 소속도 아니다"가 기본값이다.
	                         * bio_alloc_bioset() 만이 이 함수 호출 **뒤에** 자기 bs 를 채워 넣는다.
	                         * 스택 bio 나 bio_kmalloc 결과는 NULL 인 채로 남고,
	                         * 그래서 bio_free() 로 해제하면 안 되는 bio 임이 구분된다. */
}
EXPORT_SYMBOL(bio_init);	/* [한국어] bio 메모리를 스스로 마련하는 모듈이 초기화에 사용 */

/**
 * bio_reset - reinitialize a bio
 * @bio:	bio to reset
 * @bdev:	block device to use the bio for
 * @opf:	operation and flags for bio
 *
 * Description:
 *   After calling bio_reset(), @bio will be in the same state as a freshly
 *   allocated bio returned bio bio_alloc_bioset() - the only fields that are
 *   preserved are the ones that are initialized by bio_alloc_bioset(). See
 *   comment in struct bio.
 */
/*
 * [한국어]
 * bio_reset - 이미 할당된 bio를 재초기화하여 재사용한다
 *
 * @bio:  재초기화할 bio 포인터
 * @bdev: 새로 사용할 블록 장치 (NULL 이면 cgroup 연결을 맺지 않는다)
 * @opf:  새 I/O 작업 유형 및 플래그
 *
 * bio_uninit()으로 기존 상태를 정리한 뒤, BIO_RESET_BYTES 범위를 0으로 초기화한다.
 * bi_io_vec 포인터와 bi_max_vecs/bi_pool 은 보존하여 이미 할당된 메모리를 재사용한다.
 * 즉 "해제 후 재할당" 대신 "제자리 초기화"로 슬랩 왕복을 아낀다.
 * 실행 컨텍스트: 태스크 컨텍스트(bio_associate_blkg 가 current 를 본다).
 * 호출자: bio_reuse(), 그리고 자기 bio 를 여러 번 돌려쓰는 파일시스템/스택 드라이버.
 *
 * 호출 체인:
 *   bio_reuse() → [bio_reset()] → bio_uninit() + memset + bio_associate_blkg()
 */
void bio_reset(struct bio *bio, struct block_device *bdev, blk_opf_t opf)
{
	/* [한국어] bvec 배열 포인터를 대피시킨다. 아래 memset이 이 필드까지 0으로
	 * 만들어 버리는데, 배열 자체는 별도로 할당된 메모리(또는 bio 뒤에 붙은
	 * 인라인 영역)라 포인터를 잃으면 접근할 방법이 사라진다. memset 직후
	 * 곧바로 복원하는 이유가 이것이다. */
	struct bio_vec          *bv = bio->bi_io_vec;

	/* [한국어] bio가 붙들고 있던 외부 참조들을 먼저 놓는다: cgroup(blkg) 참조,
	 * 무결성 페이로드, 인라인 암호화 컨텍스트. memset보다 먼저 해야 하는
	 * 이유가 명확하다 — 0으로 덮은 뒤에는 어떤 참조를 갖고 있었는지 알 수
	 * 없어 영구적인 참조 누수가 된다. */
	bio_uninit(bio);
	/* [한국어] bio 구조체의 앞부분을 0으로 초기화한다. 전체가 아니라
	 * BIO_RESET_BYTES까지만 지우는 것이 핵심이다. 이 상수는
	 * offsetof(struct bio, bi_max_vecs)로 정의되어 있어, 그 뒤에 오는
	 * bi_max_vecs / bi_pool / bi_inline_vecs 같은 "할당 시점에 정해져
	 * 재사용해도 변하지 않는" 필드들은 보존된다.
	 * 이 경계 덕분에 bio를 해제·재할당하지 않고도 깨끗한 상태로 되돌릴 수 있다. */
	memset(bio, 0, BIO_RESET_BYTES);
	/* [한국어] 완료 카운터를 1로 되돌린다. __bi_remaining은 "이 bio가 완료되기
	 * 위해 남은 완료 횟수"로, bio_chain()으로 자식 bio를 매달 때마다 증가한다.
	 * 1이 기본값인 이유는 자기 자신의 완료 한 번을 뜻하기 때문이다.
	 * 0으로 두면 첫 bio_endio()에서 카운터가 음수가 되어 완료 처리가 깨진다. */
	atomic_set(&bio->__bi_remaining, 1);
	/* [한국어] 대피시켜 둔 bvec 배열 포인터를 복원한다. 이 한 줄 덕분에
	 * bio_reuse()가 데이터 버퍼를 그대로 물려받을 수 있다. */
	bio->bi_io_vec = bv;
	/* [한국어] 대상 블록 장치를 설정한다. NVMe 라면 /dev/nvme0n1 같은
	 * 네임스페이스의 block_device 이고, 여기서 request_queue 와 queue_limits 가
	 * 유도되어 이후 분할·병합 판정(blk-merge.c)의 기준이 된다.
	 * queue_limits 의 max_hw_sectors·max_segments·virt_boundary_mask 는
	 * NVMe 의 경우 MDTS 와 SGL 지원 여부에서 계산된 값이다
	 * (drivers/nvme/host/core.c: nvme_set_ctrl_limits). */
	bio->bi_bdev = bdev;
	/* [한국어] 장치가 지정된 경우에만 cgroup 연결을 다시 맺는다. bio_uninit()이
	 * 앞서 blkg 참조를 놓았으므로 여기서 새로 잡아야 한다. NULL 장치
	 * (bio_reset(bio, NULL, ...))에서는 어느 cgroup에 속하는지 알 수 없어
	 * 건너뛴다. */
	if (bio->bi_bdev)
		/* [한국어] 현재 태스크의 cgroup에 이 bio를 연결한다. 이 연결이 있어야
		 * blk-throttle의 대역폭 제한과 blk-iocost의 가중치 배분이 올바른
		 * cgroup에 청구된다. */
		bio_associate_blkg(bio);
	/* [한국어] 새 연산 플래그를 설정한다. memset 이후에 설정하는 순서가 중요하다 —
	 * 먼저 설정하면 지워진다.
	 * 이 값은 bio → request 로 옮겨진 뒤(rq->cmd_flags) 드라이버가 자기 프로토콜의
	 * opcode 로 번역한다. NVMe 라면 drivers/nvme/host/core.c 의 nvme_setup_cmd()가
	 * req_op(req) 를 switch 로 받아 nvme_cmd_read(0x02) / nvme_cmd_write(0x01) /
	 * nvme_cmd_flush(0x00) / nvme_cmd_dsm(0x09) 등으로 나눈다.
	 * 그 번역은 이 파일이 아니라 드라이버 소관이라는 점에 유의. */
	bio->bi_opf = opf;
}
EXPORT_SYMBOL(bio_reset);

/**
 * bio_reuse - reuse a bio with the payload left intact
 * @bio:	bio to reuse
 * @opf:	operation and flags for the next I/O
 *
 * Allow reusing an existing bio for another operation with all set up
 * fields including the payload, device and end_io handler left intact.
 *
 * Typically used when @bio is first used to read data which is then written
 * to another location without modification.  @bio must not be in-flight and
 * owned by the caller.  Can't be used for cloned bios.
 *
 * Note: Can't be used when @bio has integrity or blk-crypto contexts for now.
 * Feel free to add that support when you need it, though.
 */
/*
 * [한국어]
 * bio_reuse - payload(bvec)는 그대로 유지한 채 bio를 다른 작업으로 재사용한다
 *
 * @bio: 재사용할 bio; in-flight 상태가 아니어야 하고 호출자가 소유해야 함
 * @opf: 다음 I/O를 위한 새 작업 유형 및 플래그
 *
 * bi_io_vec, bi_bdev, bi_vcnt, bi_end_io, bi_private는 모두 보존된다.
 * bi_iter.bi_size는 vcnt 기반으로 재계산된다.
 * 제약: BIO_CLONED bio, integrity/crypto 컨텍스트가 있는 bio에는 사용 불가.
 * 전형적 용도(위 영문 주석): 어떤 위치에서 데이터를 **읽어 들인 bio** 를 그대로
 * 다른 위치에 **쓰는 bio** 로 바꿔 재제출하는 것 — 복사/재배치 경로다.
 * 데이터가 이미 bi_io_vec 이 가리키는 페이지들에 들어 있으므로, 버퍼를 새로
 * 할당해 복사할 필요 없이 bio 껍데기만 갈아 끼우면 된다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트; bio 가 in-flight 인 동안에는 호출 금지
 *   (완료 경로가 같은 필드를 건드리기 때문).
 * 호출자: 데이터를 읽어 다른 위치에 다시 쓰는 상위 계층.
 *
 * 호출 체인:
 *   상위 레이어(raid/dm) → [bio_reuse()] → bio_reset()
 */
void bio_reuse(struct bio *bio, blk_opf_t opf)
{
	/* [한국어] bio_reset()이 지워 버릴 값들을 미리 스택에 대피시킨다.
	 * vcnt = bvec 개수. 이것을 보존하는 것이 이 함수의 핵심이다 — 데이터
	 * 버퍼(bi_io_vec 배열)는 그대로 두고 "몇 개가 유효한지"만 복원하면
	 * 같은 메모리를 다시 쓸 수 있다. i는 아래 루프의 인덱스. */
	unsigned short vcnt = bio->bi_vcnt, i;
	/* [한국어] 완료 콜백. 읽기와 쓰기가 같은 완료 처리를 공유하는 경우가
	 * 많아 보존한다. */
	bio_end_io_t *end_io = bio->bi_end_io;
	/* [한국어] 완료 콜백에 넘길 컨텍스트 포인터(주로 completion 구조체나
	 * 상위 자료구조). end_io와 짝이므로 함께 보존한다. */
	void *private = bio->bi_private;

	/* [한국어] 불변식 1 — 복제된 bio는 재사용할 수 없다. BIO_CLONED bio는
	 * bi_io_vec 배열을 원본과 "공유"하므로, 여기서 크기를 다시 계산하면
	 * 원본이 보는 상태와 어긋난다. */
	WARN_ON_ONCE(bio_flagged(bio, BIO_CLONED));
	/* [한국어] 불변식 2 — 무결성(PI) 페이로드가 붙어 있으면 안 된다.
	 * 위 영문 주석이 밝히듯 아직 지원하지 않는다. PI는 LBA와 연동된 reftag를
	 * 담고 있어, 다른 위치로 쓰려면 메타데이터를 다시 만들어야 하기 때문이다. */
	WARN_ON_ONCE(bio_integrity(bio));
	/* [한국어] 불변식 3 — 인라인 암호화 컨텍스트도 마찬가지다. DUN이 LBA에
	 * 연동되므로 위치가 바뀌면 IV가 달라져야 한다. */
	WARN_ON_ONCE(bio_has_crypt_ctx(bio));

	/* [한국어] bio를 초기 상태로 되돌린다. 같은 장치(bi_bdev)를 유지하고
	 * 새 연산 플래그(opf)를 적용한다. 이 호출로 bi_iter, bi_vcnt, bi_end_io,
	 * bi_private가 전부 0/NULL이 되므로, 위에서 대피시킨 값들을 아래에서
	 * 되돌려 놓아야 한다.
	 * 주의: bi_io_vec 배열 "내용"은 bio_reset()이 건드리지 않는다. 그래서
	 * 데이터 버퍼가 그대로 살아남는 것이 이 함수가 성립하는 근거다. */
	bio_reset(bio, bio->bi_bdev, opf);
	/* [한국어] 보존된 bvec 들의 길이를 모두 더해 bi_size 를 복원한다.
	 * bio_reset() 이 bi_iter 를 통째로 0 으로 만들었으므로 직접 다시 세야 한다.
	 * 이전 I/O 가 부분 완료되어 bi_size 가 줄어 있었을 수도 있으므로,
	 * 남은 값을 쓰는 대신 배열에서 다시 합산하는 것이 안전하다. */
	for (i = 0; i < vcnt; i++)
		bio->bi_iter.bi_size += bio->bi_io_vec[i].bv_len;
	/* [한국어] 유효 bvec 개수를 복원한다. bi_size 와 bi_vcnt 가 함께 맞아야
	 * bio_for_each_segment 류의 순회가 올바른 범위를 돈다. */
	bio->bi_vcnt = vcnt;
	/* [한국어] 완료 컨텍스트를 복원한다. end_io보다 먼저 복원하는 순서에
	 * 특별한 의미는 없다(이 bio는 아직 제출되지 않아 완료가 발생할 수 없다). */
	bio->bi_private = private;
	/* [한국어] 완료 콜백을 복원한다. 이제 이 bio는 새 opf로 제출될 준비가 되었다.
	 * 전형적 용도: 어떤 위치를 읽어 온 bio를 그대로 REQ_OP_WRITE로 바꿔
	 * 다른 위치에 쓰는 복사 경로(dm/md의 미러링, 리커버리). 버퍼를 새로
	 * 할당하고 복사하는 비용이 통째로 사라진다. */
	bio->bi_end_io = end_io;
}
EXPORT_SYMBOL_GPL(bio_reuse);

/*
 * [한국어]
 * __bio_chain_endio - 체인된 하위 bio가 완료될 때 상태를 부모로 전파하고 bio를 해제한다
 *
 * @bio: 방금 완료된 하위 bio
 * @return: 부모 bio 포인터 (bio_endio()가 반복 호출할 다음 대상)
 *
 * ── 부모-자식 완료 집계의 전체 그림 ───────────────────────────────────
 * bio 하나가 장치 한계를 넘어 여러 조각으로 나뉘면, 상위 계층은 "조각들이 전부
 * 끝났을 때 한 번"만 통보받고 싶다. 이를 위해 두 개의 원자 카운터가 쓰인다.
 *
 *   __bi_remaining : "완료 통보를 몇 번 더 받아야 하는가" (bio_chain 이 +1)
 *   __bi_cnt       : "이 메모리를 몇 명이 붙들고 있는가" (bio_get 이 +1)
 *
 * 흐름:
 *   1) bio_chain(child, parent) — child->bi_private = parent 로 부모를 기록하고,
 *      child->bi_end_io 를 sentinel(bio_chain_endio) 로 표시한 뒤
 *      bio_inc_remaining(parent) 로 parent->__bi_remaining 을 +1 한다.
 *      (bio_inc_remaining 은 BIO_CHAIN 플래그도 세운다 — include/linux/bio.h:657)
 *   2) child 가 완료되면 bio_endio(child) 가 불리고, bi_end_io 가 sentinel 임을
 *      알아채 이 함수로 들어온다.
 *   3) 이 함수는 에러를 부모로 올린 뒤 child 의 **참조**를 놓고(bio_put),
 *      **부모 포인터를 반환**한다.
 *   4) bio_endio() 는 그 반환값으로 `goto again` 하여 이번엔 부모를 처리한다.
 *      부모의 bio_remaining_done() 이 __bi_remaining 을 -1 하고, 0 이 아니면
 *      거기서 멈춘다. 즉 마지막 자식이 왔을 때만 부모의 bi_end_io 가 불린다.
 *
 * 왜 반환값으로 부모를 넘기고 재귀하지 않는가:
 *   자식이 다시 자식을 가진 긴 체인에서 재귀하면 **커널 스택이 넘칠 수 있다**.
 *   특히 이 경로는 하드 IRQ 컨텍스트(IRQ 스택은 더 좁다)에서도 실행된다.
 *   그래서 bio_endio() 는 재귀 대신 goto 루프로 체인을 위로 훑어 올라간다.
 *
 * 에러 전파 규칙: `bi_status && !parent->bi_status` — **먼저 난 에러를 유지**한다.
 *   여러 자식이 각기 다른 에러로 실패해도 부모에는 첫 에러만 남는다(덮어쓰지 않음).
 *
 * @bio: 방금 완료된 자식 bio
 * @return: 부모 bio 포인터 (bio_endio() 가 이어서 처리할 다음 대상)
 *
 * 실행 컨텍스트: 하드 IRQ 컨텍스트 포함(드라이버 완료 처리기에서 시작되는 경로).
 *   잠들 수 없고, 여기서 부르는 bio_put() 도 IRQ-safe 경로만 탄다.
 * 호출자: bio_endio() (bi_end_io == bio_chain_endio 인 경우에만)
 *
 * 호출 체인:
 *   blk_mq_end_request() → bio_endio(child) → [__bio_chain_endio()]
 *                        → (반환) → bio_endio() 의 again 루프 → parent 처리
 */
static struct bio *__bio_chain_endio(struct bio *bio)
{
	struct bio *parent = bio->bi_private; /* [한국어] bio_chain()이 bi_private 에 심어 둔 부모 포인터를 꺼낸다.
	                                       * bi_private 는 원래 "완료 콜백용 자유 공간"이지만,
	                                       * 체인된 bio 에서는 이 용도로 예약된다.
	                                       * 그래서 bio_chain()은 bi_private 가 이미 차 있으면 BUG_ON 한다. */

	if (bio->bi_status && !parent->bi_status) /* [한국어] 자식이 실패했고(비-0 blk_status_t) 부모는 아직 성공 상태일 때만.
	                                           * 두 번째 조건이 "첫 에러 우선" 규칙을 만든다. */
		parent->bi_status = bio->bi_status; /* [한국어] 에러 코드를 부모로 올린다. 이것이 조각난 I/O 의
		                                     * 부분 실패가 상위 계층에 보고되는 유일한 경로다. */
	bio_put(bio); /* [한국어] 자식 bio 의 참조를 놓는다. 보통 여기서 __bi_cnt 가 0 이 되어
	               * 메모리가 풀로 돌아간다(체인된 자식은 상위 계층이 따로 참조를 잡지 않으므로).
	               * 부모는 여전히 살아 있다 — 부모의 참조는 부모 자신의 완료 경로가 놓는다. */
	return parent; /* [한국어] 부모를 반환하면 bio_endio() 가 goto again 으로 이어받는다.
	                * 재귀를 피해 스택 깊이를 O(1) 로 유지하는 것이 핵심. */
}

/*
 * This function should only be used as a flag and must never be called.
 * If execution reaches here, it indicates a serious programming error.
 */
/*
 * [한국어]
 * bio_chain_endio - bio_chain()이 bi_end_io 플래그로 설정하는 sentinel 함수
 *
 * @bio: (사용되지 않음)
 *
 * 이 함수는 절대 실제 호출되어서는 안 된다. bio_endio()가 bi_end_io ==
 * bio_chain_endio를 감지하면 __bio_chain_endio()로 직접 처리한다.
 * 실제로 호출된다면 심각한 프로그래밍 버그를 의미한다.
 */
static void bio_chain_endio(struct bio *bio)
{
	BUG(); /* [한국어] 이 함수는 호출되어서는 안 됨: bio_endio()가 포인터 비교로 우회해야 함 */
}

/**
 * bio_chain - chain bio completions
 * @bio: the target bio
 * @parent: the parent bio of @bio
 *
 * The caller won't have a bi_end_io called when @bio completes - instead,
 * @parent's bi_end_io won't be called until both @parent and @bio have
 * completed; the chained bio will also be freed when it completes.
 *
 * The caller must not set bi_private or bi_end_io in @bio.
 */
/*
 * [한국어]
 * bio_chain - 여러 bio의 완료를 부모 bio로 묶어 마지막에 한 번에 완료되게 한다
 *
 * @bio:    체인할 하위 bio; bi_private·bi_end_io가 이미 설정되어 있으면 BUG
 * @parent: 모든 하위 bio 완료 시 bi_end_io가 호출될 부모 bio
 *
 * @bio의 bi_private에 parent를 저장하고 bi_end_io를 sentinel(bio_chain_endio)으로 설정한다.
 * parent의 __bi_remaining을 1 증가시켜 이 체인된 bio의 완료가 카운트되도록 한다.
 * 마지막 하위 bio가 완료되어 __bi_remaining이 0이 되어야 parent->bi_end_io()가 호출된다.
 * 사용 예: bio 가 장치 한계를 넘어 절단될 때(blk-merge.c 의 bio_submit_split →
 * bio_split), 잘라 낸 앞부분을 자식으로 삼아 나머지(부모)에 체인한다.
 * 이렇게 하면 앞·뒤 조각이 각각 별개의 request 로 장치에 나가더라도, 상위 계층의
 * bi_end_io 는 둘 다 끝난 뒤 정확히 한 번만 불린다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트(제출 경로).
 * 호출자: bio_chain_and_submit(), blk-merge.c 의 분할 경로, blk-lib.c 의
 *   discard/zeroout 다단 제출 등.
 *
 * 호출 체인:
 *   bio_split_to_limits()/blk_next_bio() → [bio_chain()] → bio_inc_remaining(parent)
 */
void bio_chain(struct bio *bio, struct bio *parent)
{
	BUG_ON(bio->bi_private || bio->bi_end_io); /* [한국어] 자식 bio 는 자기 완료 콜백을 가질 수 없다.
	                                            * 이 함수가 두 필드를 체인 용도로 독점하기 때문이다.
	                                            * 이미 차 있다면 (a) 같은 bio 를 두 번 체인했거나
	                                            * (b) 호출자가 콜백을 세팅해 두었다는 뜻이고,
	                                            * 둘 다 완료 처리가 조용히 망가지는 버그라 즉시 멈춘다. */

	bio->bi_private = parent;            /* [한국어] 부모 포인터를 심는다. __bio_chain_endio()가 이걸 읽는다.
	                                      * 부모의 수명은 자식보다 길다는 것이 전제이며,
	                                      * 아래 bio_inc_remaining() 이 그 전제를 보장한다
	                                      * (__bi_remaining 이 0 이 되기 전엔 부모가 완료되지 않는다). */
	bio->bi_end_io	= bio_chain_endio;   /* [한국어] 완료 콜백을 sentinel 함수 주소로 설정한다.
	                                      * 이 주소는 **호출하라고 넣는 것이 아니라 표식**이다.
	                                      * bio_endio() 가 `bio->bi_end_io == bio_chain_endio` 라는
	                                      * 포인터 비교로 "체인된 bio 다"를 알아채고 __bio_chain_endio()
	                                      * 로 우회한다. 별도 플래그 비트를 쓰지 않고 기존 필드를
	                                      * 재활용한 설계다. */
	bio_inc_remaining(parent);           /* [한국어] 부모의 __bi_remaining 을 원자적으로 +1 하고 BIO_CHAIN 플래그를 세운다
	                                      * (include/linux/bio.h:657: bio_set_flag → smp_mb__before_atomic → atomic_inc).
	                                      * 메모리 배리어가 필요한 이유: 다른 CPU 가 이 자식의 완료를
	                                      * 처리하며 __bi_remaining 을 감소시킬 수 있는데, 그 CPU 가
	                                      * BIO_CHAIN 플래그 설정을 보기 **전에** 증가된 카운터만 본다면
	                                      * 완료 판정 로직이 어긋난다. 배리어가 "플래그 → 카운터" 순서를 고정한다.
	                                      * 반드시 자식을 제출하기 **전에** 호출해야 한다. 제출 직후
	                                      * 다른 CPU 에서 완료가 올라올 수 있기 때문이다. */
}
EXPORT_SYMBOL(bio_chain);

/**
 * bio_chain_and_submit - submit a bio after chaining it to another one
 * @prev: bio to chain and submit
 * @new: bio to chain to
 *
 * If @prev is non-NULL, chain it to @new and submit it.
 *
 * Return: @new.
 */
/*
 * [한국어]
 * bio_chain_and_submit - 이전 bio를 새 bio에 체인한 뒤 제출한다
 *
 * @prev: 체인 후 제출할 이전 bio; NULL이면 아무것도 안 함
 * @new:  prev를 체인할 새로운 부모 bio
 * @return: @new (호출자가 계속 사용할 현재 bio)
 *
 * 한 번에 다 담기지 않는 큰 작업(대량 discard, write_zeroes, 긴 read-ahead)을
 * 여러 bio 로 나눠 흘려보내는 패턴의 뼈대다. 호출자는 루프를 돌며
 * "직전 bio 를 지금 만든 bio 에 체인해서 내보내기"를 반복하고, 마지막 bio 하나만
 * 손에 남긴다. 그 마지막 bio 가 전체 체인의 부모가 되어, 모든 조각이 끝났을 때
 * 딱 한 번 완료 통보를 받는다.
 *
 * 주의: prev 를 new 에 체인한다 — 즉 **먼저 만든 것이 자식, 나중 것이 부모**다.
 * 직관과 반대로 보이지만, 마지막까지 남는 bio 에 완료가 모여야 하므로 이 방향이 맞다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트(submit_bio 가 잠들 수 있다).
 * 호출자: blk_next_bio(), block/blk-lib.c 의 discard/zeroout 루프 등
 *
 * 호출 체인:
 *   blk_next_bio() → [bio_chain_and_submit()] → bio_chain() + submit_bio()
 */
struct bio *bio_chain_and_submit(struct bio *prev, struct bio *new)
{
	if (prev) {	/* [한국어] 첫 반복에서는 prev 가 NULL 이라 체인할 것이 없다.
	             * 호출자가 NULL 검사를 매번 쓰지 않도록 여기서 흡수한다. */
		bio_chain(prev, new);	/* [한국어] prev 의 완료가 new 의 __bi_remaining 으로 집계되게 만든다.
		                         * 반드시 submit 보다 먼저 — 제출 직후 다른 CPU 에서 완료가 올 수 있다. */
		submit_bio(prev);	/* [한국어] 자식을 블록 계층으로 내려보낸다.
		                     * 이 시점부터 prev 는 호출자 소유가 아니며 언제든 완료될 수 있다.
		                     * 완료 시 __bio_chain_endio() 가 bio_put 으로 해제하므로
		                     * 호출자는 prev 를 더 이상 만지면 안 된다. */
	}
	return new;	/* [한국어] 호출자가 계속 채워 넣을 현재 bio 를 그대로 돌려준다.
	             * 이 반환값이 다음 반복의 prev 가 된다. */
}

/*
 * [한국어]
 * blk_next_bio - 현재 bio를 체인 후 제출하고 새 bio를 반환한다
 *
 * @bio:      체인 후 제출할 현재 bio; NULL이면 제출 없이 새 bio만 반환
 * @bdev:     새 bio에 사용할 블록 장치
 * @nr_pages: 새 bio의 bvec 슬롯 수
 * @opf:      새 bio의 I/O 작업 유형
 * @gfp:      새 bio 할당에 사용할 GFP 플래그
 * @return:   새로 할당된 bio
 *
 * 파일 시스템이 대용량 read_pages/write_pages에서 bio를 연쇄 제출할 때 사용.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: mpage_readahead(), 기타 파일 시스템 읽기/쓰기 경로
 *
 * 호출 체인:
 *   파일 시스템 → [blk_next_bio()] → bio_chain_and_submit() → bio_alloc()
 */
struct bio *blk_next_bio(struct bio *bio, struct block_device *bdev,
		unsigned int nr_pages, blk_opf_t opf, gfp_t gfp)
{
	return bio_chain_and_submit(bio, bio_alloc(bdev, nr_pages, opf, gfp)); /* [한국어] 현재 bio를 체인/제출하고 새 bio 할당해 반환 */
}
EXPORT_SYMBOL_GPL(blk_next_bio);

/*
 * [한국어]
 * bio_alloc_rescue - rescue workqueue에서 대기 중인 bio들을 재제출한다
 *
 * @work: bio_set에 내장된 rescue_work
 *
 * punt_bios_to_rescuer()가 rescue_list 에 옮겨 둔 bio 들을 꺼내 submit_bio_noacct()로
 * 재제출한다. rescue_list 가 빌 때까지 반복한다.
 *
 * 이 함수의 존재 이유는 단 하나: **다른 스레드에서 제출한다**는 것이다.
 * 교착에 빠진 원래 스레드는 current->bio_list 에 갇힌 bio 를 스스로 내보낼 수 없다
 * (자세한 시나리오는 punt_bios_to_rescuer() 주석 참조). 이 워커는 자기만의
 * current->bio_list(초기값 NULL)를 가진 별개의 실행 흐름이라, 여기서 부른
 * submit_bio_noacct() 는 리스트에 쌓지 않고 곧바로 아래로 내려간다.
 * 그 bio 들이 완료되면 메모리가 mempool 로 돌아가고, 기다리던 스레드가 깨어난다.
 *
 * 실행 컨텍스트: bio_set 전용 워크큐("bioset", WQ_MEM_RECLAIM) 워커 스레드.
 *   WQ_MEM_RECLAIM 이 핵심이다 — 이 플래그가 붙은 워크큐는 전용 rescuer 스레드를
 *   보장받으므로, 시스템 전체가 메모리 부족이라 새 워커 스레드를 만들 수 없는
 *   상황에서도 이 작업만은 실행된다. 그렇지 않으면 "교착을 푸는 코드가
 *   메모리 부족 때문에 실행되지 못하는" 2차 교착이 생긴다.
 * 호출자: 워크큐 코어 (punt_bios_to_rescuer 의 queue_work 로 예약됨)
 *
 * 호출 체인:
 *   punt_bios_to_rescuer() → queue_work() → [bio_alloc_rescue()] → submit_bio_noacct()
 */
static void bio_alloc_rescue(struct work_struct *work)
{
	struct bio_set *bs = container_of(work, struct bio_set, rescue_work); /* [한국어] 워크 아이템은 bio_set 안에 임베드되어 있으므로,
	                                                                       * 그 주소에서 오프셋을 빼 소유 bio_set 을 복원한다.
	                                                                       * 워크큐 API 가 work_struct 만 넘겨주기 때문에 필요한 관용구. */
	struct bio *bio;	/* [한국어] 한 번에 하나씩 꺼내 제출할 대상 */

	while (1) {	/* [한국어] rescue_list 가 빌 때까지 반복. 도중에 다른 스레드가 더 넣을 수도 있으므로
	             * "한 번 훑기"가 아니라 pop 이 NULL 을 줄 때까지 돈다. */
		spin_lock(&bs->rescue_lock);	/* [한국어] rescue_list 는 punt_bios_to_rescuer()(태스크 컨텍스트)와
		                                 * 이 워커가 공유한다. IRQ 컨텍스트에서는 이 리스트를 건드리지 않으므로
		                                 * spin_lock_irqsave 가 아닌 평범한 spin_lock 으로 충분하다. */
		bio = bio_list_pop(&bs->rescue_list);	/* [한국어] 큐 앞에서 하나 꺼낸다(FIFO). 비어 있으면 NULL. */
		spin_unlock(&bs->rescue_lock);	/* [한국어] submit 은 잠들 수 있으므로 반드시 락 **밖에서** 해야 한다.
		                                 * 그래서 리스트 전체를 한 번에 옮기지 않고 한 개씩 pop 하는 구조다. */

		if (!bio)
			break;	/* [한국어] 더 처리할 것이 없다. 워크 아이템 종료. */

		submit_bio_noacct(bio);	/* [한국어] 정상 제출 경로로 내려보낸다.
		                             * 이 스레드의 current->bio_list 는 NULL 이므로 submit_bio_noacct 가
		                             * "재귀 감지" 분기를 타지 않고 곧바로 __submit_bio_noacct 로 들어간다.
		                             * = 갇혀 있던 bio 가 실제로 장치까지 내려가고, 완료되면 해제되어
		                             * mempool 재고가 회복된다. 이것이 교착을 푸는 실제 동작이다. */
	}
}

/*
 * submit_bio_noacct() converts recursion to iteration; this means if we're
 * running beneath it, any bios we allocate and submit will not be submitted
 * (and thus freed) until after we return.
 *
 * This exposes us to a potential deadlock if we allocate multiple bios from the
 * same bio_set while running underneath submit_bio_noacct().  If we were to
 * allocate multiple bios (say a stacking block driver that was splitting bios),
 * we would deadlock if we exhausted the mempool's reserve.
 *
 * We solve this, and guarantee forward progress by punting the bios on
 * current->bio_list to a per bio_set rescuer workqueue before blocking to wait
 * for elements being returned to the mempool.
 */
/*
 * [한국어]
 * punt_bios_to_rescuer - mempool 대기 직전에, 현재 태스크에 갇힌 bio 들을
 *                        rescuer 워크큐로 넘겨 교착을 예방한다
 *
 * @bs: 이 bio_set 에서 나온 bio 만 골라 내보낸다
 *
 * ── 어떤 교착인가 (구체적 시나리오) ─────────────────────────────────────
 * 전제 1: submit_bio_noacct() 는 **재귀를 반복으로 바꾼다**. 이미 제출 경로 안에
 *   있는 스레드가 submit_bio_noacct() 를 다시 부르면, 그 bio 는 즉시 장치로 가지
 *   않고 current->bio_list 에 **매달리기만** 한다(block/blk-core.c:1336).
 *   실제 제출은 바깥쪽 __submit_bio_noacct() 루프가 제어를 되찾은 뒤다.
 *   (이유: 스택 드라이버가 깊게 중첩되면 재귀 제출이 커널 스택을 넘치게 한다.)
 * 전제 2: mempool_alloc(GFP_KERNEL) 은 재고가 없으면 **누군가 원소를 반납할 때까지
 *   잠든다**. mempool 의 "절대 실패하지 않음" 보장은 "언젠가 반납된다"를 전제한다.
 *
 * 이제 스택 드라이버(dm/md 처럼 bio 를 쪼개 아래로 내려보내는 드라이버)를 생각하자.
 * 이 드라이버의 ->submit_bio 는 자기 bio_set 에서 bio 를 여러 개 할당한다:
 *
 *   [스레드 T, 이미 submit_bio_noacct() 아래에서 실행 중]
 *     1. bio A 를 bs 에서 할당            → 성공 (mempool 예비 재고 소모)
 *     2. submit_bio_noacct(A)             → **current->bio_list 에 매달림. 장치로 안 감.**
 *     3. bio B 를 bs 에서 할당            → 예비 재고 고갈 → mempool_alloc 이 잠듦
 *     4. mempool 에 원소가 돌아오려면 A 가 완료되어 해제되어야 함
 *     5. 그런데 A 는 T 가 반환해야 제출된다. T 는 3번에서 자고 있다.
 *     ⇒ **T 가 자기 자신을 기다리는 자기-교착.** 다른 CPU 도 도울 수 없다.
 *
 * ── 왜 mempool 만으로는 부족한가 ────────────────────────────────────────
 * mempool 은 "충분한 예비 재고 + 언젠가 반납"을 보장하는 장치일 뿐, **반납 경로가
 * 막히는 경우**는 다루지 못한다. 위 시나리오에서 막힌 것은 메모리가 아니라
 * 반납의 전제인 "A 의 제출"이며, 그 제출은 잠든 스레드 안에 갇혀 있다.
 * 예비 재고를 늘리는 것도 해법이 아니다 — 스택 드라이버가 한 번에 몇 개의 bio 를
 * 만들지는 I/O 크기에 따라 무한히 커질 수 있어서, 어떤 유한한 pool_size 로도
 * 막을 수 없다. 그래서 **제출 자체를 다른 스레드로 옮기는** 이 장치가 필요하다.
 *
 * ── 해법 ────────────────────────────────────────────────────────────────
 * mempool 에서 잠들기 **직전에**, current->bio_list 에 갇혀 있던 bio 중
 * "이 bio_set 에서 나온 것"만 골라 bs->rescue_list 로 옮기고 워커를 깨운다.
 * 워커(bio_alloc_rescue)는 자기만의 current->bio_list(NULL)를 가지므로 그것들을
 * 진짜로 장치까지 내려보낸다 → 완료 → 해제 → mempool 재고 회복 → T 가 깨어난다.
 *
 * ── 왜 "이 bio_set 것만" 옮기는가 (위 영문 주석의 요지) ──────────────────
 * 다른(상위) 스택 드라이버의 bio 를 우리 rescuer 가 처리하면, 그 처리 과정에서
 * 그 드라이버가 **다시 bs 에서 bio 를 할당**할 수 있다. 그러면 우리 rescuer 스레드
 * 안에서 똑같은 교착이 재현된다. 남의 bio 는 남의 rescuer(또는 원래 스레드)에게
 * 맡기는 것이 층 간 의존성을 깨지 않는 유일한 방법이다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트. current->bio_list 를 직접 조작하므로 반드시
 *   해당 태스크 자신이 실행해야 하며, 그래서 별도 락이 필요 없다
 *   (current->bio_list 는 그 태스크만 만지는 per-task 데이터다).
 * 호출자: bio_alloc_bioset() — 슬랩 할당이 실패해 mempool 로 넘어가기 직전.
 *
 * 호출 체인:
 *   bio_alloc_bioset() → [punt_bios_to_rescuer()] → queue_work(rescue_workqueue)
 *                      → (다른 스레드) bio_alloc_rescue() → submit_bio_noacct()
 */
static void punt_bios_to_rescuer(struct bio_set *bs)
{
	struct bio_list punt, nopunt;	/* [한국어] punt = rescuer 에게 넘길 bio(= 이 bio_set 소속),
	                                 * nopunt = 그대로 current->bio_list 에 돌려놓을 bio(= 남의 것). */
	struct bio *bio;	/* [한국어] 분류 루프의 커서 */

	if (!current->bio_list || !bs->rescue_workqueue)	/* [한국어] 두 전제 확인.
	                                                     * bio_list 가 NULL 이면 애초에 제출 경로 아래가 아니라
	                                                     * 위 교착 시나리오가 성립하지 않는다(그냥 기다리면 된다).
	                                                     * rescue_workqueue 가 없으면 BIOSET_NEED_RESCUER 없이
	                                                     * 만들어진 bio_set 이라 이 방어 장치를 쓸 수 없다. */
		return;
	if (bio_list_empty(&current->bio_list[0]) &&	/* [한국어] [0] = 지금 실행 중인 ->submit_bio 가 새로 만든 bio 들
	                                                 * (block/blk-core.c:1203 영문 주석 참조) */
	    bio_list_empty(&current->bio_list[1]))	/* [한국어] [1] = 바깥 레벨에서 아직 처리되지 않고 보류된 bio 들.
	                                             * 둘 다 비어 있으면 갇힌 bio 가 없으니 넘길 것도 없다. */
		return;

	/*
	 * In order to guarantee forward progress we must punt only bios that
	 * were allocated from this bio_set; otherwise, if there was a bio on
	 * there for a stacking driver higher up in the stack, processing it
	 * could require allocating bios from this bio_set, and doing that from
	 * our own rescuer would be bad.
	 *
	 * Since bio lists are singly linked, pop them all instead of trying to
	 * remove from the middle of the list:
	 */

	bio_list_init(&punt);	/* [한국어] 넘길 것 모으는 빈 리스트 */
	bio_list_init(&nopunt);	/* [한국어] 남길 것 모으는 빈 리스트 */

	/* [한국어] bio_list 는 **단일 연결 리스트**라 중간 원소를 골라 빼낼 수 없다
	 * (위 영문 주석의 "Since bio lists are singly linked, pop them all instead").
	 * 그래서 전부 pop 해서 두 리스트로 분류한 뒤, 남길 것만 원위치에 돌려놓는다.
	 * 순서는 pop-push 특성상 보존된다(bio_list_add 는 tail 추가). */
	while ((bio = bio_list_pop(&current->bio_list[0])))	/* [한국어] [0] 리스트를 완전히 비울 때까지 하나씩 꺼낸다 */
		bio_list_add(bio->bi_pool == bs ? &punt : &nopunt, bio);	/* [한국어] 소속 판정은 bi_pool 포인터 비교 한 번.
		                                                             * bi_pool == bs 면 "내가 기다리는 바로 그 mempool 에서
		                                                             * 나온 bio" 이므로, 이 녀석이 완료돼야 내 재고가 돈다. */
	current->bio_list[0] = nopunt;	/* [한국어] 남의 bio 만 남은 리스트를 제자리에 복원.
	                                 * 구조체 대입이라 헤드/테일 포인터가 통째로 옮겨간다. */

	bio_list_init(&nopunt);	/* [한국어] nopunt 를 재사용하기 위해 다시 비운다.
	                         * 위에서 그 내용은 이미 current->bio_list[0] 로 옮겨졌다. */
	while ((bio = bio_list_pop(&current->bio_list[1])))	/* [한국어] [1] 리스트도 같은 방식으로 분류 */
		bio_list_add(bio->bi_pool == bs ? &punt : &nopunt, bio);	/* [한국어] punt 에는 [0] 에서 모은 것 뒤에 이어 붙는다 */
	current->bio_list[1] = nopunt;	/* [한국어] [1] 도 남의 bio 만 남겨 복원 */

	spin_lock(&bs->rescue_lock);	/* [한국어] rescue_list 는 여러 태스크와 워커가 공유하므로 락이 필요하다.
	                                 * IRQ 컨텍스트에서는 접근하지 않으므로 irqsave 는 불필요. */
	bio_list_merge(&bs->rescue_list, &punt);	/* [한국어] 골라낸 bio 들을 rescuer 대기열 뒤에 통째로 이어 붙인다(O(1)). */
	spin_unlock(&bs->rescue_lock);	/* [한국어] queue_work 는 잠들지 않지만, 락 구간은 짧을수록 좋다. */

	queue_work(bs->rescue_workqueue, &bs->rescue_work);	/* [한국어] 워커를 깨운다. 이미 큐에 올라가 있으면 아무 일도 하지 않는다
	                                                         * (work_struct 는 중복 큐잉을 자체적으로 막는다) — 그래도
	                                                         * 워커는 rescue_list 를 빌 때까지 돌므로 새로 넣은 bio 도 처리된다.
	                                                         * 이 호출 직후 호출자(bio_alloc_bioset)는 mempool_alloc 으로
	                                                         * 잠들지만, 이제는 깨워 줄 다른 스레드가 확보된 상태다. */
}

/*
 * [한국어]
 * bio_alloc_irq_cache_splice - IRQ 캐시(free_list_irq)를 태스크 캐시(free_list)로 이전한다
 *
 * @cache: splice할 per-CPU bio_alloc_cache
 *
 * free_list_irq에 쌓인 bio들을 free_list로 옮겨 태스크 컨텍스트에서 재사용할 수 있게 한다.
 * free_list가 비어 있고(assert), IRQ를 잠시 비활성화한 뒤 포인터 이전 및 카운터를 갱신한다.
 * 왜 옮겨 쓰는가: 반납은 IRQ 컨텍스트에서도 일어나지만 할당은 태스크 컨텍스트에서만
 * 일어난다. 두 리스트를 분리해 두면 IRQ 쪽 반납은 IRQ 비활성 구간 없이(정확히는
 * 이미 IRQ 가 꺼진 상태에서) O(1) 로 끝나고, 태스크 쪽 할당은 선점 비활성만으로
 * 안전하다. 이 함수는 태스크 캐시가 바닥났을 때만 IRQ 를 잠깐 끄고 두 리스트를
 * 잇는, 드물게 실행되는 합류 지점이다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트; get_cpu/put_cpu 로 선점이 막힌 상태에서 호출.
 * 호출자: bio_alloc_percpu_cache()
 *
 * 호출 체인:
 *   bio_alloc_percpu_cache() → [bio_alloc_irq_cache_splice()]
 */
static void bio_alloc_irq_cache_splice(struct bio_alloc_cache *cache)
{
	unsigned long flags;

	/* cache->free_list must be empty */
	if (WARN_ON_ONCE(cache->free_list)) /* [한국어] free_list가 비어 있어야 splice 가능: 중복 이전 방지 */
		return;

	local_irq_save(flags);               /* [한국어] IRQ 비활성화: free_list_irq와 nr_irq에 대한 ISR 경쟁 방지 */
	cache->free_list = cache->free_list_irq; /* [한국어] IRQ 캐시 리스트를 태스크 캐시 리스트로 이전 */
	cache->free_list_irq = NULL;          /* [한국어] IRQ 캐시 리스트 초기화: 다음 ISR이 새로 채울 수 있도록 */
	cache->nr += cache->nr_irq;           /* [한국어] IRQ 캐시 카운트를 태스크 카운트에 합산 */
	cache->nr_irq = 0;                    /* [한국어] IRQ 캐시 카운트 초기화 */
	local_irq_restore(flags);             /* [한국어] IRQ 복원. 이제 완료 인터럽트가 다시 들어와
	                                       * free_list_irq 를 새로 채울 수 있다.
	                                       * save/restore 쌍을 쓰는 이유: 호출 시점에 이미 IRQ 가
	                                       * 꺼져 있었을 수도 있으므로 무조건 켜는 local_irq_enable()
	                                       * 대신 이전 상태를 복원해야 한다. */
}

/*
 * [한국어]
 * bio_alloc_percpu_cache - per-CPU 캐시에서 bio를 할당한다
 *
 * @bs: bio를 할당할 bio_set
 * @return: 캐시에서 꺼낸 bio; 캐시 미스 시 NULL
 *
 * free_list에서 bio를 꺼낸다. free_list가 비어 있고 free_list_irq가 충분히 쌓였으면
 * bio_alloc_irq_cache_splice()로 IRQ 캐시를 태스크 캐시로 이전한 뒤 재시도한다.
 * 캐시 미스(둘 다 비어 있음)이면 NULL을 반환해 상위 함수가 slab/mempool을 사용하게 한다.
 * 이 경로의 이득: 락도 원자적 연산도 없이 리스트에서 하나 pop 하는 것이라
 * 사실상 O(1) 이고 캐시도 뜨겁다(방금 해제된 객체를 다시 쓰므로).
 * 초당 수십만 건의 I/O 를 내는 워크로드에서 슬랩 왕복 비용을 없앤다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트; get_cpu/put_cpu 로 현재 CPU 에 고정.
 * 호출자: bio_alloc_bioset() (bs->cache 가 있고 인라인 bvec 으로 충분할 때)
 *
 * 호출 체인:
 *   bio_alloc_bioset() → [bio_alloc_percpu_cache()] → (캐시 히트) bio 반환
 */
static struct bio *bio_alloc_percpu_cache(struct bio_set *bs)
{
	struct bio_alloc_cache *cache;
	struct bio *bio;

	cache = per_cpu_ptr(bs->cache, get_cpu());   /* [한국어] 현재 CPU에 고정하고 해당 CPU의 캐시 포인터 획득 */
	if (!cache->free_list) {                      /* [한국어] 태스크 캐시 비어 있음: IRQ 캐시로 보충 시도 */
		if (READ_ONCE(cache->nr_irq) >= ALLOC_CACHE_THRESHOLD) /* [한국어] IRQ 캐시가 임계값 이상이면 splice */
			bio_alloc_irq_cache_splice(cache);
		if (!cache->free_list) {               /* [한국어] splice 후에도 비어 있으면 캐시 미스 */
			put_cpu();                     /* [한국어] CPU 고정 해제 후 NULL 반환 */
			return NULL; /* [한국어] 캐시 미스: 상위 함수가 slab/mempool 할당으로 폴백 */
		}
	}
	bio = cache->free_list;                       /* [한국어] 캐시 히트: 리스트 첫 번째 bio를 꺼냄 */
	cache->free_list = bio->bi_next;              /* [한국어] bi_next로 리스트 전진(캐시 내 연결은 bi_next 사용) */
	cache->nr--;                                  /* [한국어] 태스크 캐시 카운트 감소 */
	put_cpu();                                    /* [한국어] CPU 고정 해제: 선점 다시 허용 */
	bio->bi_pool = bs;                            /* [한국어] 이 bio가 속한 bio_set 설정: bio_free()가 올바른 mempool로 반납하도록 */

	kmemleak_alloc(bio_slab_addr(bio),
		       kmem_cache_size(bs->bio_slab), 1, GFP_NOIO); /* [한국어] kmemleak에 캐시에서 재활성화된 bio 추적 등록 */
	return bio; /* [한국어] 성공: 초기화 없이 재사용할 준비된 bio 반환 */
}

/**
 * bio_alloc_bioset - allocate a bio for I/O
 * @bdev:	block device to allocate the bio for (can be %NULL)
 * @nr_vecs:	number of bvecs to pre-allocate
 * @opf:	operation and flags for bio
 * @gfp:	the GFP_* mask given to the slab allocator
 * @bs:		the bio_set to allocate from.
 *
 * Allocate a bio from the mempools in @bs.
 *
 * If %__GFP_DIRECT_RECLAIM is set then bio_alloc will always be able to
 * allocate a bio.  This is due to the mempool guarantees.  To make this work,
 * callers must never allocate more than 1 bio at a time from the general pool.
 * Callers that need to allocate more than 1 bio must always submit the
 * previously allocated bio for IO before attempting to allocate a new one.
 * Failure to do so can cause deadlocks under memory pressure.
 *
 * Note that when running under submit_bio_noacct() (i.e. any block driver),
 * bios are not submitted until after you return - see the code in
 * submit_bio_noacct() that converts recursion into iteration, to prevent
 * stack overflows.
 *
 * This would normally mean allocating multiple bios under submit_bio_noacct()
 * would be susceptible to deadlocks, but we have
 * deadlock avoidance code that resubmits any blocked bios from a rescuer
 * thread.
 *
 * However, we do not guarantee forward progress for allocations from other
 * mempools. Doing multiple allocations from the same mempool under
 * submit_bio_noacct() should be avoided - instead, use bio_set's front_pad
 * for per bio allocations.
 *
 * Returns: Pointer to new bio on success, NULL on failure.
 */
/*
 * [한국어]
 * bio_alloc_bioset - bio와 필요 시 bio_vec 배열을 mempool/slab에서 할당한다
 *
 * @bdev:    I/O 를 수행할 블록 장치; NULL 가능(나중에 bio_set_dev 로 지정)
 * @nr_vecs: 사전 할당할 bvec 슬롯 수; 0 이면 bvec 배열을 아예 붙이지 않는다(clone 용)
 * @opf:     I/O 작업 유형 및 플래그 (REQ_OP_READ/WRITE | REQ_SYNC …)
 * @gfp:     할당 플래그. __GFP_DIRECT_RECLAIM 이 있으면 mempool 덕분에 **반드시 성공**한다
 *           (다만 잠들 수 있다). 없으면 실패해 NULL 을 돌려줄 수 있다.
 * @bs:      할당에 쓸 bio_set
 * @return:  새 bio 포인터; 논블로킹 경로에서 실패 시 NULL
 *
 * ── 3단 할당 전략 ──────────────────────────────────────────────────────
 *   1단 per-CPU 캐시 : 락 없이 자기 CPU 리스트에서 pop. 가장 빠르다.
 *                      단, 캐시가 있고(bs->cache) bvec 이 인라인으로 충분할 때만.
 *   2단 슬랩         : kmem_cache_alloc. 단 GFP 를 try_alloc_gfp() 로 낮춰
 *                      "실패해도 좋으니 빨리 돌아와라"로 시도한다.
 *   3단 mempool      : 위가 다 실패했고 호출자가 잠들 수 있을 때만.
 *                      여기 들어가기 직전에 punt_bios_to_rescuer() 로 교착을 예방한다.
 *
 * 위 영문 주석이 강조하는 호출 규약: **한 번에 두 개 이상의 bio 를 같은 풀에서
 * 들고 있지 마라.** 하나를 할당했으면 제출한 뒤 다음을 할당해야 한다. 이를 어기면
 * (rescuer 가 없는 다른 mempool 에서는 특히) 교착이 난다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트. mempool 경로가 잠들 수 있으므로 IRQ 컨텍스트에서
 *   호출하려면 반드시 __GFP_DIRECT_RECLAIM 없는 GFP 를 써야 한다.
 * 호출자: bio_alloc()(fs_bio_set 을 쓰는 래퍼), 파일시스템 I/O 경로, Direct I/O,
 *   자기 bio_set 을 가진 스택 드라이버.
 *
 * 호출 체인:
 *   파일시스템/DIO → bio_alloc() → [bio_alloc_bioset()] → bio_init()/bio_init_inline()
 */
struct bio *bio_alloc_bioset(struct block_device *bdev, unsigned short nr_vecs,
			     blk_opf_t opf, gfp_t gfp, struct bio_set *bs)
{
	struct bio_vec *bvecs = NULL;	/* [한국어] 별도 할당한 bvec 배열. 인라인으로 충분하면 NULL 로 남는다. */
	struct bio *bio = NULL;	/* [한국어] 결과. 1·2단에서 못 얻으면 NULL 인 채로 3단(mempool)으로 간다. */
	gfp_t saved_gfp = gfp;	/* [한국어] **원본 GFP 보존**. 바로 아래에서 gfp 를 try_alloc_gfp() 로
	                         * 낮춰 버리므로, mempool 폴백 때 쓸 "진짜" 플래그를 여기 챙겨 둔다.
	                         * 특히 __GFP_DIRECT_RECLAIM 유무 판정이 saved_gfp 기준이어야 한다. */
	void *p;	/* [한국어] 슬랩/mempool 이 돌려준 원주소(front_pad 앞). bio = p + front_pad. */

	/* should not use nobvec bioset for nr_vecs > 0 */
	if (WARN_ON_ONCE(!mempool_initialized(&bs->bvec_pool) && nr_vecs > 0))	/* [한국어] BIOSET_NEED_BVECS 없이 만든 풀에
	                                                                         * bvec 을 달라고 하는 것은 호출자 버그다.
	                                                                         * 그런 풀은 back_pad 도 bvec_pool 도 없어
	                                                                         * 줄 수 있는 배열 자체가 존재하지 않는다. */
		return NULL;

	gfp = try_alloc_gfp(gfp);	/* [한국어] 첫 시도용으로 GFP 를 완화: 직접 회수·I/O 를 빼고
	                             * NORETRY·NOWARN 을 붙인다. 실패해도 아래 mempool 이 받아 주므로
	                             * 여기서 OOM killer 를 부르거나 오래 기다릴 이유가 없다. */
	if (bs->cache && nr_vecs <= BIO_INLINE_VECS) {
		/*
		 * Set REQ_ALLOC_CACHE even if no cached bio is available to
		 * return the allocated bio to the percpu cache when done.
		 */
		/* [한국어] REQ_ALLOC_CACHE를 세워 "이 bio는 완료 시 per-CPU 캐시로
		 * 돌려보내라"고 표시한다. 위 영문 주석이 밝히듯, 지금 캐시가 비어
		 * 있어 슬랩에서 할당하더라도 이 플래그는 설정한다 — 반납 경로를
		 * 캐시 쪽으로 유도해 다음 할당을 빠르게 만들기 위해서다. */
		opf |= REQ_ALLOC_CACHE;
		/* [한국어] per-CPU 캐시에서 bio를 꺼낸다. 락도 원자적 연산도 없이
		 * 자기 CPU의 리스트에서 pop하는 것이라 슬랩 할당보다 훨씬 빠르다.
		 * 캐시가 비었으면 NULL을 반환하고, 아래에서 슬랩으로 폴백한다.
		 * NVMe처럼 초당 수십만 개의 bio를 만드는 워크로드에서 이 캐시가
		 * 할당 비용을 크게 줄인다. */
		bio = bio_alloc_percpu_cache(bs);
	} else {
		/* [한국어] REQ_ALLOC_CACHE를 "지운다". 이 경로는 캐시를 쓸 수 없는
		 * 경우이기 때문이다 — bio_set에 캐시가 없거나(bs->cache == NULL),
		 * 요청한 bvec 수가 인라인 한도(BIO_INLINE_VECS)를 넘어 캐시된
		 * 고정 크기 bio로는 담을 수 없다.
		 * 플래그를 반드시 지워야 하는 이유: 호출자가 opf에 이미 이 비트를
		 * 켜서 넘겼을 수 있는데, 그대로 두면 완료 시 bio_put()이 캐시로
		 * 반납을 시도해 크기가 맞지 않는 객체가 캐시에 섞여 들어간다. */
		opf &= ~REQ_ALLOC_CACHE;
		/* [한국어] 슬랩 캐시에서 직접 할당한다. bs->bio_slab 의 오브젝트 크기는
		 * front_pad + sizeof(bio) + back_pad 이므로, 반환된 p 에 front_pad 를
		 * 더해야 bio 본체 위치가 된다(아래 줄). front_pad 에는 이 풀을 만든
		 * 호출자의 전용 구조체가 들어간다 — 예: block/fops.c 의 struct blkdev_dio.
		 * (NVMe 드라이버는 bio_set 을 쓰지 않는다. struct nvme_iod 는 여기가 아니라
		 *  blk-mq 태그셋의 cmd_size 로 request 뒤에 붙으며 blk_mq_rq_to_pdu() 로 얻는다.) */
		p = kmem_cache_alloc(bs->bio_slab, gfp);	/* [한국어] 완화된 gfp 라 메모리가 빠듯하면 그냥 NULL 을 준다 */
		if (p)	/* [한국어] 실패면 bio 는 NULL 인 채로 남아 아래 mempool 폴백으로 간다 */
			bio = p + bs->front_pad;	/* [한국어] void* 산술이라 바이트 단위. 슬랩 객체 시작 + front_pad = bio 본체. */
	}

	if (bio && nr_vecs > BIO_INLINE_VECS) {
		struct biovec_slab *bvs = biovec_slab(nr_vecs);

		/*
		 * Upgrade nr_vecs to take full advantage of the allocation.
		 * We also rely on this in bio_free().
		 */
		nr_vecs = bvs->nr_vecs;	/* [한국어] 요청보다 큰 등급으로 올려 기록한다(예: 20 요청 → 64).
		                         * 어차피 그 크기의 메모리를 받았으니 상한을 낮춰 잡을 이유가 없고,
		                         * 무엇보다 bio_free() 가 이 값으로 "어느 슬랩에 돌려줄지"를 역산하므로
		                         * 여기 기록된 값이 실제 슬랩 등급과 일치해야 한다(위 영문 주석의 요지). */
		bvecs = kmem_cache_alloc(bvs->slab, gfp);	/* [한국어] 선택된 등급 슬랩에서 bvec 배열을 받는다 */
		if (unlikely(!bvecs)) {	/* [한국어] bio 본체는 얻었는데 bvec 만 실패한 경우 */
			kmem_cache_free(bs->bio_slab, p);	/* [한국어] 반쪽짜리 bio 를 들고 갈 수 없으므로 본체도 되돌린다.
			                                     * 주의: bio 가 아니라 p(front_pad 포함 원주소)를 넘긴다. */
			bio = NULL;	/* [한국어] NULL 로 표시해 아래 mempool 폴백에서 처음부터 다시 시도하게 한다 */
		}
	}

	if (unlikely(!bio)) {
		/*
		 * Give up if we are not allow to sleep as non-blocking mempool
		 * allocations just go back to the slab allocation.
		 */
		if (!(saved_gfp & __GFP_DIRECT_RECLAIM))
			return NULL;

		punt_bios_to_rescuer(bs);	/* [한국어] **잠들기 전에** 현재 태스크에 갇힌 bio 들을 rescuer 로 넘긴다.
		                             * 이 한 줄이 없으면 아래 mempool_alloc 에서 자기 자신을 기다리는
		                             * 교착에 빠질 수 있다(함수 주석의 시나리오 참조).
		                             * 순서가 절대적으로 중요하다 — 잠든 뒤에는 아무것도 할 수 없다. */

		/*
		 * Don't rob the mempools by returning to the per-CPU cache if
		 * we're tight on memory.
		 */
		/* [한국어] 메모리 압박으로 폴백 경로에 들어섰다. 위 영문 주석대로
		 * "메모리가 빠듯할 때 per-CPU 캐시로 반납해 mempool을 축내지 않도록"
		 * 플래그를 지운다. 이 bio는 완료 시 캐시가 아니라 mempool로 직접
		 * 돌아가, 다른 스레드가 mempool에서 할당받을 수 있게 된다. */
		opf &= ~REQ_ALLOC_CACHE;

		p = mempool_alloc(&bs->bio_pool, saved_gfp);	/* [한국어] 예비 재고에서 꺼낸다. saved_gfp 에
		                                             * __GFP_DIRECT_RECLAIM 이 있음이 위에서 보장되었으므로
		                                             * 이 호출은 **실패하지 않는다** — 대신 재고가 없으면
		                                             * 누군가 반납할 때까지 잠든다. NULL 검사가 없는 이유. */
		bio = p + bs->front_pad;	/* [한국어] 슬랩 경로와 동일하게 front_pad 만큼 밀어 bio 본체를 얻는다 */
		if (nr_vecs > BIO_INLINE_VECS) {	/* [한국어] 인라인으로 부족한 경우에만 bvec 배열도 mempool 에서 */
			nr_vecs = BIO_MAX_VECS;	/* [한국어] bvec_pool 의 원소는 항상 **최대 크기 배열**이다.
			                         * 그래서 실제 필요량과 무관하게 상한값으로 기록한다.
			                         * 이 값이 곧 bio_free() 의 판별 근거가 된다:
			                         * bi_max_vecs == BIO_MAX_VECS ⇒ "mempool 에서 왔다". */
			bvecs = mempool_alloc(&bs->bvec_pool, saved_gfp);	/* [한국어] 마찬가지로 잠들 수는 있어도 실패하지 않는다 */
		}
	}

	if (nr_vecs && nr_vecs <= BIO_INLINE_VECS)	/* [한국어] 1~4 개면 bio 뒤(back_pad)의 인라인 배열로 충분하다.
	                                             * nr_vecs == 0 은 "bvec 이 아예 필요 없다"(clone 전용)라
	                                             * 이 분기에 들어가지 않고 아래 bio_init(…, NULL, 0, …)로 간다. */
		bio_init_inline(bio, bdev, nr_vecs, opf);	/* [한국어] bi_io_vec 을 bio 바로 뒤 주소로 설정하는 변형 초기화 */
	else
		bio_init(bio, bdev, bvecs, nr_vecs, opf);	/* [한국어] 외부 배열(또는 NULL)을 물려 초기화 */
	bio->bi_pool = bs;	/* [한국어] **반드시 bio_init 뒤에** 설정한다 — bio_init 이 이 필드를 NULL 로 밀기 때문이다.
	                     * 이 포인터가 있어야 bio_free()/bio_put_percpu_cache() 가
	                     * "어느 풀로 돌려줄지"를 알 수 있다. */
	return bio;	/* [한국어] 여기까지 왔으면 bio 는 반드시 non-NULL 이다 */
}
EXPORT_SYMBOL(bio_alloc_bioset);	/* [한국어] 자기 bio_set 을 가진 파일시스템·스택 드라이버 모듈용 공개 심볼 */

/**
 * bio_kmalloc - kmalloc a bio
 * @nr_vecs:	number of bio_vecs to allocate
 * @gfp_mask:   the GFP_* mask given to the slab allocator
 *
 * Use kmalloc to allocate a bio (including bvecs).  The bio must be initialized
 * using bio_init() before use.  To free a bio returned from this function use
 * kfree() after calling bio_uninit().  A bio returned from this function can
 * be reused by calling bio_uninit() before calling bio_init() again.
 *
 * Note that unlike bio_alloc() or bio_alloc_bioset() allocations from this
 * function are not backed by a mempool can fail.  Do not use this function
 * for allocations in the file system I/O path.
 *
 * Returns: Pointer to new bio on success, NULL on failure.
 */
/*
 * [한국어]
 * bio_kmalloc - kmalloc으로 bio와 bio_vec을 단순 할당한다 (mempool 미보장)
 *
 * @nr_vecs:  할당할 bio_vec 개수 (BIO_MAX_INLINE_VECS 이하)
 * @gfp_mask: 할당 플래그
 * @return:   bio 포인터; nr_vecs 초과 또는 메모리 부족 시 NULL
 *
 * 반환된 bio 는 bio_init() 후 사용하고, 해제 시 bio_uninit() 후 kfree() 를 호출해야 한다.
 * bio_alloc_bioset() 과 달리 mempool 뒷받침이 없으므로 **메모리 부족 시 그냥 실패한다**.
 * 그래서 위 영문 주석이 "파일시스템 I/O 경로에서는 쓰지 말라"고 못 박는다 —
 * 회수 경로에서 이 함수가 실패하면 회수 자체가 진행되지 않기 때문이다.
 *
 * bio 와 bvec 배열을 **하나의 kmalloc** 으로 붙여 받는 것이 특징이다.
 * 그래서 반환된 포인터 하나만 kfree() 하면 끝이고, 별도의 bvec 해제가 없다.
 * bio_init() 에 넘길 table 포인터는 호출자가 (bio + 1) 로 직접 계산한다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트(gfp_mask 에 따라 잠들 수 있음).
 * 호출자(이 트리 전체 grep 기준): drivers/md/raid1.c, raid10.c, dm-bufio.c,
 *   dm-flakey.c, bcache/debug.c, target/target_core_pscsi.c, fs/squashfs/block.c.
 *   — 전부 I/O 회수 경로가 아닌 곳이거나 자체 재시도 로직을 가진 코드다.
 *   **NVMe 드라이버는 이 함수를 쓰지 않는다.**
 *
 * 호출 체인:
 *   위 드라이버/파일시스템 → [bio_kmalloc()] → (호출자가) bio_init() → … → bio_uninit() + kfree()
 */
struct bio *bio_kmalloc(unsigned short nr_vecs, gfp_t gfp_mask)
{
	struct bio *bio;	/* [한국어] 원본 코드에 선언만 있고 쓰이지 않는 변수.
	                     * sizeof(*bio) 를 타입 안전하게 쓰기 위한 관용구다
	                     * (sizeof(struct bio) 라고 적는 것보다 타입 변경에 강하다). */

	if (nr_vecs > BIO_MAX_INLINE_VECS)	/* [한국어] BIO_MAX_INLINE_VECS 는 UIO_MAXIOV(=1024, include/linux/bio.h:14).
	                                     * kmalloc 한 번으로 감당할 크기의 상한을 두어
	                                     * 터무니없는 요청이 고차수 페이지 할당으로 번지는 것을 막는다. */
		return NULL;
	return kmalloc(sizeof(*bio) + nr_vecs * sizeof(struct bio_vec),	/* [한국어] bio 본체 + 그 뒤에 붙일 bvec 배열을
	                                                                 * 한 덩어리로 요청한다. 반환 주소가 곧 bio 이고,
	                                                                 * bvec 배열은 (bio + 1) 위치에서 시작한다. */
			gfp_mask);	/* [한국어] 호출자가 준 플래그를 그대로 쓴다 — mempool 폴백이 없으므로
			             * 실패 가능성은 전적으로 호출자가 감수한다. */
}
EXPORT_SYMBOL(bio_kmalloc);	/* [한국어] md/dm/bcache/squashfs 등 모듈에서 쓸 수 있도록 공개 */

/*
 * [한국어]
 * zero_fill_bio_iter - bio의 start부터 끝까지 모든 bvec 버퍼를 0으로 채운다
 *
 * @bio:   0으로 채울 bio
 * @start: 순회를 시작할 bvec_iter 위치
 *
 * 주 용도는 "장치에 물어보지 않고 0 을 돌려주는" 읽기다. 예:
 *   - dm-zero / dm-thin 의 미할당 블록 읽기 (읽으면 0 이어야 한다)
 *   - loop/null_blk 의 백엔드 범위 밖 읽기
 *   - blk-map.c 가 커널 버퍼를 미리 0 으로 만들 때
 * 이런 경우 읽기 버퍼를 0 으로 채우지 않고 그대로 반환하면 이전에 그 페이지에
 * 있던 데이터가 사용자에게 유출된다.
 *
 * 이름의 _iter 접미사: 시작 위치를 호출자가 지정할 수 있다는 뜻이다.
 * bio 전체를 채우는 zero_fill_bio(bio) 는 이 함수에 bio->bi_iter 를 넘기는
 * 인라인 래퍼다(include/linux/bio.h).
 *
 * 실행 컨텍스트: 태스크 컨텍스트. memzero_bvec 이 highmem 을 kmap_local 로
 *   임시 매핑할 수 있으므로 원자적 컨텍스트에서는 피하는 것이 안전하다.
 * 호출자(이 트리 grep 기준): block/blk-map.c, drivers/block/loop.c,
 *   null_blk, zloop, drivers/md 의 bcache/dm-io/dm-thin/dm-zero/dm-zoned 등.
 *
 * 호출 체인:
 *   dm-zero 등 → zero_fill_bio() → [zero_fill_bio_iter()] → memzero_bvec()
 */
void zero_fill_bio_iter(struct bio *bio, struct bvec_iter start)
{
	struct bio_vec bv;	/* [한국어] 순회 중 현재 세그먼트 사본 */
	struct bvec_iter iter;	/* [한국어] 매크로가 내부적으로 전진시키는 커서.
	                         * start 의 사본으로 시작하므로 호출자의 iterator 는 변하지 않는다. */

	__bio_for_each_segment(bv, bio, iter, start)	/* [한국어] start 위치부터 bio 끝까지 세그먼트 단위 순회.
	                                             * "__" 버전은 시작 위치를 인자로 받는 형태다
	                                             * (bio_for_each_segment 는 bio->bi_iter 를 쓴다). */
		memzero_bvec(&bv);	/* [한국어] bvec 이 가리키는 (page, offset, len) 영역을 0 으로.
		                     * highmem 페이지면 내부에서 kmap_local_page 로 임시 매핑한 뒤 지운다. */
}
EXPORT_SYMBOL(zero_fill_bio_iter);	/* [한국어] dm/loop/null_blk 등 가상 블록 드라이버 모듈이 사용 */

/**
 * bio_truncate - truncate the bio to small size of @new_size
 * @bio:	the bio to be truncated
 * @new_size:	new size for truncating the bio
 *
 * Description:
 *   Truncate the bio to new size of @new_size. If bio_op(bio) is
 *   REQ_OP_READ, zero the truncated part. This function should only
 *   be used for handling corner cases, such as bio eod.
 */
/*
 * [한국어]
 * bio_truncate - bio를 new_size 바이트로 잘라내고 잘린 READ 영역을 0으로 채운다
 *
 * @bio:      truncate할 bio
 * @new_size: 바이트 단위의 새 크기 (bi_size가 이 값으로 줄어듦)
 *
 * new_size >= bi_size이면 아무 작업 없이 반환.
 * READ 명령인 경우 truncated 부분(old_size - new_size)을 zero_fill_bio_iter()로 0 채움.
 * bi_iter.bi_size를 new_size로 업데이트하는 것만으로 드라이버는 올바른 bvec을 구성할 수 있다.
 * 왜 READ 만 0 으로 채우는가: READ 는 장치가 채워 줄 것이라 믿고 사용자에게 버퍼를
 * 그대로 노출한다. 잘려서 장치가 손대지 않은 뒷부분을 그대로 두면 **이전에 그 페이지에
 * 있던 커널/타 프로세스 데이터가 유출**된다. WRITE 는 반대로 장치로 나가지 않을 뿐이라
 * 정보 유출이 없어 0 채움이 불필요하다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트(I/O 제출 전).
 * 호출자: guard_bio_eod()
 *
 * 호출 체인:
 *   guard_bio_eod() → [bio_truncate()] → memzero_page()
 */
static void bio_truncate(struct bio *bio, unsigned new_size)
{
	struct bio_vec bv;	/* [한국어] 순회 중 현재 세그먼트의 (page, offset, len) 사본 */
	struct bvec_iter iter;	/* [한국어] 순회 커서. bio->bi_iter 를 건드리지 않도록 사본으로 돈다. */
	unsigned int done = 0;	/* [한국어] 지금까지 지나온 누적 바이트. 이 값과 new_size 를 비교해
	                         * "잘리는 경계가 이 세그먼트 안에 있는지"를 판정한다. */
	bool truncated = false;	/* [한국어] 경계를 이미 지났는지 표시.
	                         * 경계가 걸친 **첫** 세그먼트는 중간부터, 그 뒤 세그먼트는 처음부터 지운다. */

	if (new_size >= bio->bi_iter.bi_size)	/* [한국어] 줄일 것이 없으면(같거나 오히려 크면) 아무것도 하지 않는다.
	                                         * 이 함수는 확장 용도가 아니다. */
		return;

	if (bio_op(bio) != REQ_OP_READ)	/* [한국어] READ 가 아니면 0 채움을 건너뛰고 크기 갱신만 한다.
	                                 * (위 설명대로 정보 유출 위험이 없다) */
		goto exit;

	bio_for_each_segment(bv, bio, iter) {	/* [한국어] bi_iter 기준으로 남은 데이터를 세그먼트 단위로 순회.
	                                         * 이 매크로는 bvec 경계와 bi_bvec_done 오프셋을 모두 고려해
	                                         * "실제로 이 bio 가 담당하는 구간"만 돌려준다. */
		if (done + bv.bv_len > new_size) {	/* [한국어] 이 세그먼트의 끝이 new_size 를 넘는다
		                                     * = 이 세그먼트의 일부(또는 전부)가 잘려 나갈 영역이다. */
			size_t offset;	/* [한국어] 이 세그먼트 안에서 0 채움을 시작할 위치 */

			if (!truncated)	/* [한국어] 경계가 걸친 첫 세그먼트: 앞부분은 유효한 데이터라 보존해야 한다 */
				offset = new_size - done;	/* [한국어] 세그먼트 시작으로부터 (new_size - 누적) 바이트 뒤부터 지운다 */
			else	/* [한국어] 경계를 이미 지난 뒤의 세그먼트: 통째로 잘려 나가는 영역이다 */
				offset = 0;	/* [한국어] 세그먼트 처음부터 전부 지운다 */
			memzero_page(bv.bv_page, bv.bv_offset + offset,	/* [한국어] 페이지 내 절대 오프셋 = bvec 자체의 오프셋 + 위에서 구한 상대 오프셋.
			                                                 * memzero_page 는 highmem 페이지도 kmap_local 로 임시 매핑해 처리한다. */
				  bv.bv_len - offset);	/* [한국어] 세그먼트 끝까지의 길이만큼 0 으로 */
			truncated = true;	/* [한국어] 이후 세그먼트는 전부 0 채움 대상임을 기록 */
		}
		done += bv.bv_len;	/* [한국어] 누적 위치 전진. 경계 판정의 기준값이다. */
	}

 exit:
	/*
	 * Don't touch bvec table here and make it really immutable, since
	 * fs bio user has to retrieve all pages via bio_for_each_segment_all
	 * in its .end_bio() callback.
	 *
	 * It is enough to truncate bio by updating .bi_size since we can make
	 * correct bvec with the updated .bi_size for drivers.
	 */
	bio->bi_iter.bi_size = new_size;	/* [한국어] **크기만 줄인다.** 위 영문 주석의 핵심:
	                                     * bvec 테이블은 건드리지 않아 immutable 로 유지한다.
	                                     * 이유 (1) 파일시스템의 .bi_end_io 가 bio_for_each_segment_all() 로
	                                     * **원래 붙인 모든 페이지**를 회수해야 하는데, 테이블을 잘라 버리면
	                                     * 남은 페이지의 참조를 놓을 방법이 사라진다.
	                                     * 이유 (2) 드라이버는 bi_size 를 존중해 순회하므로, 크기만 줄여도
	                                     * 장치로 나가는 데이터 범위는 정확히 제한된다. */
}

/**
 * guard_bio_eod - truncate a BIO to fit the block device
 * @bio:	bio to truncate
 *
 * This allows us to do IO even on the odd last sectors of a device, even if the
 * block size is some multiple of the physical sector size.
 *
 * We'll just truncate the bio to the size of the device, and clear the end of
 * the buffer head manually.  Truly out-of-range accesses will turn into actual
 * I/O errors, this only handles the "we need to be able to do I/O at the final
 * sector" case.
 */
/*
 * [한국어]
 * guard_bio_eod - bio가 블록 장치의 마지막 sector를 넘지 않도록 잘라낸다
 *
 * @bio: 경계를 확인하고 필요 시 truncate할 bio
 *
 * bi_iter.bi_sector + (bi_size>>9)가 장치의 총 sector 수를 초과하면
 * bio_truncate()로 장치 마지막 sector까지만 남긴다.
 * 왜 필요한가(위 영문 주석): 파일시스템의 블록 크기가 장치의 물리 섹터 크기의 배수라
 * 마지막 블록이 장치 끝을 살짝 넘칠 수 있다. 그때 I/O 전체를 거절하면 마지막 블록을
 * 아예 읽고 쓸 수 없게 된다. 그래서 **넘치는 부분만 잘라** 장치 끝까지는 정상 수행한다.
 * 반면 **전체가** 장치 밖인 요청은 진짜 버그이므로 자르지 않고 그대로 통과시켜
 * 아래 계층이 EIO 로 실패시키게 둔다(조용히 성공한 것처럼 보이면 안 되므로).
 *
 * 실행 컨텍스트: 태스크 컨텍스트(제출 경로, 장치로 내려보내기 직전).
 * 호출자: 파일시스템의 bio 제출 헬퍼 및 block/blk-core.c 의 제출 검증 경로.
 *
 * 호출 체인:
 *   파일시스템 submit → [guard_bio_eod()] → bio_truncate() → submit_bio_noacct()
 */
void guard_bio_eod(struct bio *bio)
{
	sector_t maxsector = bdev_nr_sectors(bio->bi_bdev);	/* [한국어] 대상 장치(파티션이면 그 파티션)의 총 512B 섹터 수.
	                                                     * 파티션 bio 는 아직 파티션 상대 좌표이므로,
	                                                     * 여기서의 경계는 "파티션 끝"이다. */

	if (!maxsector)	/* [한국어] 크기 0 인 장치(아직 용량을 모르는 상태 등)는 검사할 기준이 없다.
	                 * 여기서 자르면 모든 I/O 를 0 바이트로 만들어 버리므로 그냥 통과시킨다. */
		return;

	/*
	 * If the *whole* IO is past the end of the device,
	 * let it through, and the IO layer will turn it into
	 * an EIO.
	 */
	if (unlikely(bio->bi_iter.bi_sector >= maxsector))	/* [한국어] 시작 섹터부터 이미 장치 밖.
	                                                     * 자르면 크기 0 짜리 bio 가 되어 "성공"처럼 보인다.
	                                                     * 그건 조용한 데이터 손실이므로 손대지 않고
	                                                     * 아래 계층이 EIO 로 실패시키게 둔다. */
		return;

	maxsector -= bio->bi_iter.bi_sector;	/* [한국어] 절대 경계를 **이 bio 기준의 남은 섹터 수**로 바꾼다.
	                                         * 위 검사 덕분에 음수가 되지 않음이 보장된다
	                                         * (sector_t 는 부호 없는 타입이라 언더플로가 곧 거대한 값이 된다). */
	if (likely((bio->bi_iter.bi_size >> 9) <= maxsector))	/* [한국어] bi_size 는 바이트 단위 → >>9 로 512B 섹터 수로 변환.
	                                                         * 남은 공간 안에 들어가면 자를 필요가 없다.
	                                                         * likely() — 압도적 다수가 이 경로다. */
		return;

	bio_truncate(bio, maxsector << 9);	/* [한국어] 섹터 수를 다시 바이트로 되돌려(<<9) 그 크기로 자른다.
	                                     * READ 라면 bio_truncate() 가 잘려 나간 영역을 0 으로 채워
	                                     * 사용자에게 옛 데이터가 새는 것을 막는다. */
}

/*
 * [한국어]
 * __bio_alloc_cache_prune - per-CPU 태스크 캐시에서 최대 nr개의 bio를 해제한다
 *
 * @cache: 정리할 bio_alloc_cache
 * @nr:    해제할 최대 bio 개수 (-1U이면 전체 해제)
 * @return: 실제 해제한 bio 개수
 *
 * free_list에서 bio를 꺼내 bio_free()로 mempool/slab에 반납한다.
 * kmemleak에 할당 추적을 등록해 정리 후 kmemleak 레포트가 정확하게 유지된다.
 * 실행 컨텍스트: 태스크 컨텍스트 또는 CPU 핫플러그 경로.
 * 호출자: bio_alloc_cache_prune()
 */
static int __bio_alloc_cache_prune(struct bio_alloc_cache *cache,
				   unsigned int nr)
{
	unsigned int i = 0; /* [한국어] 해제한 bio 개수 카운터 */
	struct bio *bio;

	while ((bio = cache->free_list) != NULL) { /* [한국어] 태스크 캐시가 비어 있지 않으면 계속 해제 */
		cache->free_list = bio->bi_next; /* [한국어] 리스트에서 꺼냄: bi_next로 연결된 다음 bio로 전진 */
		cache->nr--;                     /* [한국어] 태스크 캐시 카운트 감소 */
		kmemleak_alloc(bio_slab_addr(bio),
			       kmem_cache_size(bio->bi_pool->bio_slab),
			       1, GFP_KERNEL); /* [한국어] kmemleak 추적 재등록: bio_free()가 kmemleak_free()를 호출할 수 있도록 */
		bio_free(bio);  /* [한국어] bio 본체와 bvec을 mempool/slab에 반환 */
		if (++i == nr)  /* [한국어] nr개 해제했으면 루프 종료 */
			break;
	}
	return i; /* [한국어] 실제 해제한 개수 반환: 나머지 nr을 IRQ 캐시에서 처리하도록 */
}

/*
 * [한국어]
 * bio_alloc_cache_prune - per-CPU 캐시에서 nr개의 bio를 해제한다 (태스크+IRQ 캐시 모두)
 *
 * @cache: 정리할 bio_alloc_cache
 * @nr:    해제할 목표 bio 개수
 *
 * 먼저 태스크 캐시(free_list)에서 해제하고, 부족하면 IRQ 캐시(free_list_irq)를
 * splice한 뒤 나머지를 해제한다.
 * 실행 컨텍스트: 태스크 컨텍스트 (bio_cpu_dead, bio_alloc_cache_destroy).
 * 호출자: bio_cpu_dead(), bio_alloc_cache_destroy()
 */
static void bio_alloc_cache_prune(struct bio_alloc_cache *cache,
				  unsigned int nr)
{
	nr -= __bio_alloc_cache_prune(cache, nr); /* [한국어] 태스크 캐시에서 먼저 nr개 해제; 미해제 잔량을 nr에 보관 */
	if (!READ_ONCE(cache->free_list)) {       /* [한국어] 태스크 캐시가 비었으면 IRQ 캐시에서 보충 */
		bio_alloc_irq_cache_splice(cache);    /* [한국어] IRQ 캐시 → 태스크 캐시 이전 */
		__bio_alloc_cache_prune(cache, nr);   /* [한국어] 이전된 IRQ 캐시에서 나머지 해제 */
	}
}

/*
 * [한국어]
 * bio_cpu_dead - CPU 오프라인 시 해당 CPU의 bio 캐시를 정리한다
 *
 * @cpu:  오프라인이 된 CPU 번호
 * @node: bio_set의 cpuhp_dead hlist 노드
 * @return: 0 (성공; cpuhp 콜백 규약)
 *
 * CPU가 오프라인 되면 해당 CPU의 per-CPU bio 캐시는 더 이상 접근되지 않는다.
 * 메모리 누수를 방지하기 위해 남은 bio를 모두 해제한다.
 * 실행 컨텍스트: CPU 핫플러그 경로 (CPUHP_BIO_DEAD 상태).
 * 호출자: cpuhp 서브시스템
 */
static int bio_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
	struct bio_set *bs;

	bs = hlist_entry_safe(node, struct bio_set, cpuhp_dead); /* [한국어] hlist 노드에서 bio_set 역산 */
	if (bs->cache) {
		struct bio_alloc_cache *cache = per_cpu_ptr(bs->cache, cpu); /* [한국어] 오프라인된 CPU의 per-CPU 캐시 포인터 획득 */

		bio_alloc_cache_prune(cache, -1U); /* [한국어] 캐시 전체 해제(-1U = UINT_MAX 개): 메모리 누수 방지 */
	}
	return 0; /* [한국어] 성공: cpuhp 콜백 규약에 따라 0 반환 */
}

/*
 * [한국어]
 * bio_alloc_cache_destroy - bio_set의 per-CPU 캐시 전체를 정리하고 해제한다
 *
 * @bs: 캐시를 정리할 bio_set
 *
 * bioset_exit() 시 호출되어 모든 CPU의 per-CPU 캐시를 순회하며 bio를 해제한다.
 * cpuhp 콜백도 제거하여 이후 CPU 핫플러그 이벤트가 발생해도 안전하다.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: bioset_exit()
 */
static void bio_alloc_cache_destroy(struct bio_set *bs)
{
	int cpu;

	if (!bs->cache)
		return; /* [한국어] 캐시가 없으면(BIOSET_PERCPU_CACHE 미사용) 즉시 반환 */

	cpuhp_state_remove_instance_nocalls(CPUHP_BIO_DEAD, &bs->cpuhp_dead); /* [한국어] CPU 핫플러그 콜백 해제 */
	for_each_possible_cpu(cpu) { /* [한국어] 모든 가능한 CPU를 순회하며 per-CPU 캐시 정리 */
		struct bio_alloc_cache *cache;

		cache = per_cpu_ptr(bs->cache, cpu);     /* [한국어] 해당 CPU의 bio_alloc_cache 포인터 획득 */
		bio_alloc_cache_prune(cache, -1U);        /* [한국어] 캐시 전체 해제 */
	}
	free_percpu(bs->cache); /* [한국어] per-CPU 캐시 메모리 자체 해제 */
	bs->cache = NULL;       /* [한국어] 포인터 초기화: dangling pointer 방지 */
}

/*
 * [한국어]
 * bio_put_percpu_cache - bio를 per-CPU 캐시로 반환한다
 *
 * @bio: 반환할 bio
 *
 * 컨텍스트에 따라 free_list(태스크) 또는 free_list_irq(IRQ)로 bio를 저장한다.
 * 캐시가 가득 찼거나(nr+nr_irq > ALLOC_CACHE_MAX), 소프트 IRQ 컨텍스트이면
 * bio_free()로 직접 해제한다.
 * bio_uninit()으로 cgroup/integrity/crypto 를 먼저 정리한 뒤에 캐시에 넣는다.
 * 캐시에 들어간 bio 는 "메모리 블록"일 뿐 어떤 외부 참조도 붙들고 있지 않아야 한다.
 *
 * 왜 세 갈래(태스크 / 하드IRQ / 그 외)인가:
 *   - in_task()    : 선점만 막으면 되므로 free_list 를 그냥 쓴다.
 *   - in_hardirq() : 이미 IRQ 가 꺼진 상태라 free_list_irq 를 락 없이 안전하게 만진다.
 *   - 나머지(softirq/BH) : 하드 IRQ 가 켜져 있어 free_list_irq 를 만지면 ISR 과 경쟁하고,
 *     그렇다고 free_list 를 쓰면 태스크 컨텍스트와 경쟁한다. 어느 쪽도 안전하지 않으므로
 *     캐시를 포기하고 곧장 bio_free() 한다. 드물게 일어나는 경로라 손해가 크지 않다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트 또는 하드 IRQ 컨텍스트(그 외는 폴백 처리).
 * 호출자: bio_put() — bi_opf 에 REQ_ALLOC_CACHE 가 켜져 있을 때만.
 *
 * 호출 체인:
 *   bio_endio() → bio_put() → [bio_put_percpu_cache()] → (캐시 저장) 또는 bio_free()
 */
static inline void bio_put_percpu_cache(struct bio *bio)
{
	struct bio_alloc_cache *cache;	/* [한국어] 현재 CPU 의 캐시 슬롯 */

	cache = per_cpu_ptr(bio->bi_pool->cache, get_cpu());	/* [한국어] get_cpu()가 선점을 끄고 현재 CPU 번호를 준다.
	                                                     * 이 시점부터 put_cpu() 까지는 CPU 가 바뀌지 않으므로
	                                                     * per-CPU 데이터를 락 없이 만질 수 있다. */
	if (READ_ONCE(cache->nr_irq) + cache->nr > ALLOC_CACHE_MAX)	/* [한국어] 캐시 총량이 상한을 넘었다.
	                                                             * nr_irq 는 IRQ 컨텍스트가 갱신하므로 찢긴 값을 읽지 않도록
	                                                             * READ_ONCE 로 한 번에 읽는다(nr 은 이 CPU 태스크만 쓰므로 불필요). */
		goto out_free;	/* [한국어] 더 쌓지 않고 진짜로 해제한다 — CPU 수 × 256 만큼 메모리가 묶이는 것을 막는다 */

	if (in_task()) {	/* [한국어] 프로세스 컨텍스트: 하드 IRQ 가 언제든 끼어들 수 있으므로 IRQ 리스트는 못 쓴다 */
		bio_uninit(bio);	/* [한국어] 캐시에 넣기 전에 외부 참조를 전부 놓는다.
		                     * 이걸 빼먹으면 캐시에 앉아 있는 동안 cgroup 참조가 계속 잡혀 있게 된다. */
		bio->bi_next = cache->free_list;	/* [한국어] bi_next 를 캐시 리스트 링크로 재사용한다.
		                                     * 완료된 bio 는 어느 제출 리스트에도 속하지 않으므로 이 필드가 비어 있다. */
		/* Not necessary but helps not to iopoll already freed bios */
		bio->bi_bdev = NULL;	/* [한국어] 필수는 아니지만(위 영문 주석), 이미 반납된 bio 를
		                         * iopoll 경로가 실수로 따라가지 않도록 장치 포인터를 끊어 둔다. */
		cache->free_list = bio;	/* [한국어] 리스트 앞에 push (LIFO). 방금 쓴 객체가 캐시에 더 남아 있어
		                         * 다음 할당에서 캐시 히트율이 높다. */
		cache->nr++;	/* [한국어] 태스크 쪽 개수 갱신. 이 CPU 의 태스크 컨텍스트만 만지므로 원자 연산 불필요. */
		kmemleak_free(bio_slab_addr(bio));	/* [한국어] kmemleak 에게 "이 객체는 해제된 것으로 취급하라"고 알린다.
		                                     * 실제로는 캐시에 살아 있지만, 추적 대상으로 남겨 두면
		                                     * 캐시에 머무는 모든 bio 가 누수로 오탐된다.
		                                     * 짝이 되는 재등록은 bio_alloc_percpu_cache()의 kmemleak_alloc(). */
	} else if (in_hardirq()) {	/* [한국어] 하드 IRQ 컨텍스트: 드라이버 완료 처리기에서 온 경로 */
		lockdep_assert_irqs_disabled();	/* [한국어] 아래 조작이 안전하려면 IRQ 가 꺼져 있어야 한다.
		                                 * 하드 IRQ 핸들러는 원래 IRQ 가 꺼진 채 실행되지만,
		                                 * 그 전제를 lockdep 으로 명시적으로 검증한다(디버그 빌드 전용). */

		bio_uninit(bio);	/* [한국어] IRQ 컨텍스트에서도 안전하다 — bio_uninit 은 잠들지 않는다 */
		bio->bi_next = cache->free_list_irq;	/* [한국어] **태스크 리스트가 아니라 IRQ 전용 리스트**에 넣는다.
		                                         * 그래야 태스크 쪽이 local_irq_save 없이 free_list 를 쓸 수 있다. */
		cache->free_list_irq = bio;	/* [한국어] IRQ 리스트 앞에 push */
		cache->nr_irq++;	/* [한국어] IRQ 쪽 개수 갱신. IRQ 가 꺼져 있고 per-CPU 이므로 원자 연산 불필요. */
		kmemleak_free(bio_slab_addr(bio));	/* [한국어] 위와 같은 이유로 추적 해제 */
	} else {	/* [한국어] softirq/BH 등 나머지 — 어느 리스트도 안전하게 쓸 수 없는 컨텍스트 */
		goto out_free;	/* [한국어] 캐시를 포기하고 정직하게 해제한다 */
	}
	put_cpu();	/* [한국어] 선점 재허용. 여기까지가 per-CPU 임계 구역이다. */
	return;
out_free:
	put_cpu();	/* [한국어] bio_free() 는 mempool 락을 잡을 수 있으므로 **선점을 먼저 풀고** 부른다.
	             * 선점 비활성 구간을 불필요하게 길게 유지하면 지연 시간이 나빠진다. */
	bio_free(bio);	/* [한국어] 슬랩/mempool 로 실제 반납 */
}

/**
 * bio_put - release a reference to a bio
 * @bio:   bio to release reference to
 *
 * Description:
 *   Put a reference to a &struct bio, either one you have gotten with
 *   bio_alloc, bio_get or bio_clone_*. The last put of a bio will free it.
 **/
/*
 * [한국어]
 * bio_put - bio의 참조 카운트를 감소시키고, 마지막 참조 시 해제한다
 *
 * @bio: 참조를 해제할 bio
 *
 * BIO_REFFED 플래그가 설정된 bio는 __bi_cnt로 참조 카운팅된다(분할/복제 등).
 * __bi_cnt가 0이 되어야 실제 해제된다.
 * REQ_ALLOC_CACHE가 설정된 bio는 per-CPU 캐시로 반환되고,
 * 그 외에는 bio_free()로 mempool/slab에 반환된다.
 * BIO_REFFED 최적화: 대다수 bio 는 참조가 하나뿐이라 원자적 감소가 낭비다.
 * 그래서 bio_get() 이 실제로 참조를 늘렸을 때만 BIO_REFFED 를 세우고
 * (include/linux/bio.h:227), 그 플래그가 없으면 원자 연산 없이 곧바로 해제한다.
 * unlikely() 가 붙은 이유가 이것이다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트 및 하드 IRQ 컨텍스트(드라이버 완료 처리기 경로).
 * 호출자: bio_endio(), __bio_chain_endio(), bio_dirty_fn(), 그리고 bio 를 잡았던 모든 코드.
 *
 * 호출 체인:
 *   blk_mq_end_request() → bio_endio() → [bio_put()] → bio_put_percpu_cache()/bio_free()
 */
void bio_put(struct bio *bio)
{
	if (unlikely(bio_flagged(bio, BIO_REFFED))) {	/* [한국어] 누군가 bio_get() 으로 참조를 더 잡은 적이 있는 bio 만
	                                                 * 카운터를 확인한다. 평범한 bio 는 이 블록을 통째로 건너뛴다. */
		BUG_ON(!atomic_read(&bio->__bi_cnt));	/* [한국어] 카운터가 이미 0 이면 이중 해제다.
		                                         * 이 상태로 진행하면 아래 감소가 음수를 만들고,
		                                         * 해제된 메모리를 두 번 반납해 풀 전체를 오염시킨다.
		                                         * 조용히 넘어가는 대신 여기서 멈추는 편이 낫다. */
		if (!atomic_dec_and_test(&bio->__bi_cnt))	/* [한국어] 원자적으로 1 감소하고 결과가 0 인지 본다.
		                                             * 0 이 아니면 = 아직 다른 참조자가 있다 → 해제하지 않는다.
		                                             * "감소 후 검사"가 한 연산으로 묶여야 두 CPU 가 동시에
		                                             * 마지막 참조라고 착각하는 일이 없다. */
			return;
	}
	if (bio->bi_opf & REQ_ALLOC_CACHE)	/* [한국어] 이 bio 가 per-CPU 캐시 대상으로 할당되었는지 확인.
	                                     * bio_alloc_bioset() 이 캐시 가능한 경우에만 이 비트를 세우고,
	                                     * mempool 폴백 경로에서는 다시 지운다. */
		/* [한국어] per-CPU 캐시로 반납한다. 슬랩/mempool 로 돌려보내는 것보다
		 * 훨씬 싸고, 같은 CPU 에서 곧 이어질 다음 bio 할당이 이 객체를 바로
		 * 재사용한다. 초당 수십만 개의 bio 를 만드는 워크로드에서 할당·해제
		 * 비용을 눈에 띄게 줄여 준다. */
		bio_put_percpu_cache(bio);
	else
		bio_free(bio);	/* [한국어] 캐시 대상이 아니면 곧장 슬랩/mempool 로 반납한다 */
}
EXPORT_SYMBOL(bio_put);	/* [한국어] bio 를 다루는 모든 모듈이 쓰는 기본 API */

/*
 * [한국어]
 * __bio_clone - 원본 bio의 반복자, 플래그, cgroup, crypto, integrity를 새 bio로 복사한다
 *
 * @bio:     복사 대상 bio (이미 bio_init()된 상태)
 * @bio_src: 복사 원본 bio
 * @gfp:     cgroup/integrity/crypto 복사 시 사용할 메모리 할당 플래그
 * @return:  0 성공; -ENOMEM 메모리 부족
 *
 * bi_iter(SLBA/length), bi_ioprio, bi_write_hint, bi_write_stream을 복사한다.
 * BIO_CLONED 플래그를 설정하여 이 bio에 페이지를 추가하거나 수정하는 것을 금지한다.
 * blkcg 연결, crypto context, integrity context를 각각 복제한다.
 * 핵심: **데이터 버퍼는 복사하지 않는다.** 클론은 원본과 같은 bio_vec 배열을 가리키며,
 * 그래서 원본이 살아 있는 동안만 유효하다. 복사되는 것은 "어디를 얼마나"(bi_iter)와
 * 부가 컨텍스트(cgroup / crypto / integrity)뿐이다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트(gfp 로 잠들 수 있음).
 * 호출자: bio_alloc_clone(), bio_init_clone()
 *
 * 호출 체인:
 *   bio_alloc_clone()/bio_init_clone() → [__bio_clone()] → bio_crypt_clone() + bio_integrity_clone()
 */
static int __bio_clone(struct bio *bio, struct bio *bio_src, gfp_t gfp)
{
	bio_set_flag(bio, BIO_CLONED);	/* [한국어] "이 bio 는 bio_vec 배열을 소유하지 않는다"는 표식.
	                                 * bio_add_page()/bio_iov_iter_get_pages() 등이 이 플래그를 보고
	                                 * WARN 후 거부한다 — 공유 배열을 수정하면 원본이 망가지기 때문이다.
	                                 * 해제 경로도 이 플래그를 보고 bvec 을 반납하지 않는다. */
	bio->bi_ioprio = bio_src->bi_ioprio;	/* [한국어] I/O 우선순위 승계 — 클론도 같은 우선순위로 다뤄져야 한다 */
	bio->bi_write_hint = bio_src->bi_write_hint;	/* [한국어] 쓰기 수명 힌트 승계 */
	bio->bi_write_stream = bio_src->bi_write_stream;	/* [한국어] 쓰기 스트림 식별자 승계 */
	bio->bi_iter = bio_src->bi_iter;	/* [한국어] iterator 통째 복사 = 같은 섹터·같은 길이·같은 bvec 위치.
	                                     * 호출자는 대개 이 직후 bi_iter 를 자기 범위로 좁힌다
	                                     * (예: bio_trim 이 offset/size 로 조정). */

	if (bio->bi_bdev) {	/* [한국어] 대상 장치가 정해진 클론만 아래 처리가 의미 있다 */
		if (bio->bi_bdev == bio_src->bi_bdev &&	/* [한국어] 원본과 **같은 장치**로 클론하는 경우에만 */
		    bio_flagged(bio_src, BIO_REMAPPED))	/* [한국어] BIO_REMAPPED = "파티션 상대 섹터 → 디스크 절대 섹터 변환이 이미 끝났다".
		                                         * 같은 장치라면 그 변환 결과가 그대로 유효하므로 승계한다.
		                                         * 승계하지 않으면 제출 경로가 오프셋을 **두 번** 더해
		                                         * 엉뚱한 위치에 I/O 를 하게 된다. 장치가 다르면 당연히 승계 불가. */
			bio_set_flag(bio, BIO_REMAPPED);
		bio_clone_blkg_association(bio, bio_src);	/* [한국어] 원본과 같은 cgroup(blkcg_gq)에 클론을 귀속시키고 참조를 잡는다.
		                                             * 현재 태스크가 아니라 **원본의** cgroup 을 따라가야 한다 —
		                                             * 클론을 만드는 스레드(예: dm 워커)는 원래 I/O 를 낸 주체가 아니기 때문이다. */
	}

	if (bio_crypt_clone(bio, bio_src, gfp) < 0)	/* [한국어] inline 암호화 컨텍스트(키·DUN)를 복제.
	                                             * 원본에 컨텍스트가 없으면 아무 일도 없이 0 을 돌려준다.
	                                             * 실패는 메모리 부족뿐이다. */
		return -ENOMEM;
	if (bio_integrity(bio_src) &&	/* [한국어] 원본에 T10-PI 페이로드가 붙어 있을 때만 */
	    bio_integrity_clone(bio, bio_src, gfp) < 0)	/* [한국어] PI 페이로드도 복제한다(block/bio-integrity.c).
	                                                 * 메타데이터 버퍼 자체는 공유하고 bip 껍데기만 새로 만든다.
	                                                 * 이걸 빠뜨리면 클론이 PI 없이 제출되어, PI 포맷 장치가
	                                                 * 메타데이터 누락으로 명령을 거부한다. */
		return -ENOMEM;
	return 0;	/* [한국어] 성공. 이 시점에 클론은 원본과 같은 데이터를 가리키는 독립된 bio 다. */
}

/**
 * bio_alloc_clone - clone a bio that shares the original bio's biovec
 * @bdev: block_device to clone onto
 * @bio_src: bio to clone from
 * @gfp: allocation priority
 * @bs: bio_set to allocate from
 *
 * Allocate a new bio that is a clone of @bio_src. This reuses the bio_vecs
 * pointed to by @bio_src->bi_io_vec, and clones the iterator pointing to
 * the current position in it.  The caller owns the returned bio, but not
 * the bio_vecs, and must ensure the bio is freed before the memory
 * pointed to by @bio_Src->bi_io_vecs.
 */
/*
 * [한국어]
 * bio_alloc_clone - 원본 bio의 biovec을 공유하는 새 bio를 할당한다
 *
 * @bdev:    새 bio에 사용할 블록 장치
 * @bio_src: 공유할 biovec의 원본 bio
 * @gfp:     할당 플래그
 * @bs:      할당에 사용할 bio_set
 * @return:  새로 할당된 clone bio; 실패 시 NULL
 *
 * bi_io_vec 배열을 **공유**하고 bi_iter 만 복사하는 가벼운 클론이다.
 * 데이터 복사가 전혀 없으므로 비용은 bio 하나 할당분뿐이다.
 *
 * **수명 규약(위 영문 주석)**: 호출자는 반환된 bio 를 소유하지만 bio_vec 배열은
 * 소유하지 않는다. 따라서 클론은 반드시 bio_src->bi_io_vec 메모리보다 **먼저**
 * 해제되어야 한다. 이를 어기면 해제된 배열을 가리키는 bio 가 남는다.
 * 실제 사용처에서는 원본이 부모로서 클론(자식)의 완료를 기다리므로
 * (bio_chain 의 __bi_remaining) 이 순서가 자연스럽게 지켜진다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: block/blk-merge.c 의 bio_submit_split_bioset() → bio_split() 경로,
 *   그리고 bio 를 나눠 여러 하위 장치로 보내는 스택 드라이버.
 *
 * 호출 체인:
 *   bio_split() → [bio_alloc_clone()] → bio_alloc_bioset() + __bio_clone()
 */
struct bio *bio_alloc_clone(struct block_device *bdev, struct bio *bio_src,
		gfp_t gfp, struct bio_set *bs)
{
	struct bio *bio;	/* [한국어] 새로 만들 클론 */

	bio = bio_alloc_bioset(bdev, 0, bio_src->bi_opf, gfp, bs);	/* [한국어] nr_vecs 를 **0** 으로 요청하는 것이 핵심이다.
	                                                             * bvec 배열을 새로 받을 필요가 없다 — 아래에서 원본 것을
	                                                             * 그대로 가리킬 것이기 때문이다. 덕분에 두 번째 할당이 없다.
	                                                             * opf 는 원본과 같은 연산·플래그를 승계한다. */
	if (!bio)	/* [한국어] 논블로킹 gfp 였다면 실패할 수 있다 */
		return NULL;

	if (__bio_clone(bio, bio_src, gfp) < 0) {	/* [한국어] iterator·cgroup·crypto·integrity 를 복제.
	                                             * 실패는 부속 컨텍스트 복제 중의 메모리 부족뿐이다. */
		bio_put(bio);	/* [한국어] 반쪽만 만들어진 클론을 되돌린다.
		                 * __bio_clone 이 도중에 잡아 둔 참조들은 bio_put → bio_free → bio_uninit
		                 * 경로에서 정리되므로 여기서 따로 풀 필요가 없다. */
		return NULL;
	}
	bio->bi_io_vec = bio_src->bi_io_vec;	/* [한국어] **배열 포인터만 대입** — 복사가 아니다.
	                                         * bio_alloc_bioset(nr_vecs=0) 이 bi_io_vec 을 NULL 로 두었으므로
	                                         * 여기서 원본 배열을 물려받는다. bi_max_vecs 는 0 인 채로 남는데,
	                                         * 이것이 곧 "이 bio 는 배열을 소유하지 않는다"는 표시가 되어
	                                         * bio_free() 가 bvec 을 해제하지 않게 만든다.
	                                         * bi_iter(위 __bio_clone 이 복사)가 이 배열 안의 어느 구간을
	                                         * 볼지 결정한다. */

	return bio;	/* [한국어] 원본과 같은 데이터를 가리키는 독립된 bio */
}
EXPORT_SYMBOL(bio_alloc_clone);	/* [한국어] dm/md 등 bio 를 나눠 내려보내는 스택 드라이버가 사용 */

/**
 * bio_init_clone - clone a bio that shares the original bio's biovec
 * @bdev: block_device to clone onto
 * @bio: bio to clone into
 * @bio_src: bio to clone from
 * @gfp: allocation priority
 *
 * Initialize a new bio in caller provided memory that is a clone of @bio_src.
 * The same bio_vecs reuse and bio lifetime rules as bio_alloc_clone() apply.
 */
/*
 * [한국어]
 * bio_init_clone - 호출자 제공 메모리에 원본 bio를 복제한다 (bi_io_vec 공유)
 *
 * @bdev:    클론 bio 가 접근할 블록 장치
 * @bio:     클론이 들어갈 메모리 (호출자가 이미 확보해 둔 것)
 * @bio_src: 복제 원본 bio
 * @gfp:     __bio_clone() 내부 할당 플래그
 * @return:  0 성공; -ENOMEM 실패
 *
 * bio_alloc_clone()과 유일하게 다른 점: bio 본체 메모리를 여기서 할당하지 않는다.
 * 자기 구조체 안에 bio 를 임베드해 두고 그 자리에 클론을 만드는 호출자를 위한 변형이다
 * (bio_set 의 front_pad 관례와 같은 발상 — 할당 횟수를 줄인다).
 * bio_vec 공유와 수명 규약은 bio_alloc_clone() 과 동일하다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: 자기 구조체에 bio 를 내장한 스택 드라이버.
 *
 * 호출 체인:
 *   스택 드라이버 → [bio_init_clone()] → bio_init() + __bio_clone()
 */
int bio_init_clone(struct block_device *bdev, struct bio *bio,
		struct bio *bio_src, gfp_t gfp)
{
	int ret;	/* [한국어] __bio_clone 의 성패 */

	bio_init(bio, bdev, bio_src->bi_io_vec, 0, bio_src->bi_opf);	/* [한국어] 원본의 bvec 배열 포인터를 table 인자로 바로 넘기고
	                                                             * max_vecs 는 0 으로 둔다 — "가리키기만 하고 소유하지 않는다".
	                                                             * bio_alloc_clone 이 두 단계로 하던 일(할당 후 포인터 대입)을
	                                                             * 여기서는 bio_init 한 번으로 처리한다. */
	ret = __bio_clone(bio, bio_src, gfp);	/* [한국어] iterator·cgroup·crypto·integrity 복제 */
	if (ret)	/* [한국어] 실패 시 */
		bio_uninit(bio);	/* [한국어] 메모리는 호출자 것이라 해제할 수 없으므로, **부속 자원만** 되돌린다.
		                     * __bio_clone 이 도중까지 잡아 둔 blkg 참조나 crypt ctx 가
		                     * 여기서 정리되지 않으면 그대로 누수가 된다.
		                     * (bio_alloc_clone 은 bio_put 이 이 일을 대신 해 준다) */
	return ret;	/* [한국어] 0 이면 호출자는 이 bio 를 제출할 수 있다 */
}
EXPORT_SYMBOL(bio_init_clone);	/* [한국어] bio 를 자기 구조체에 내장한 스택 드라이버용 */

/**
 * bio_full - check if the bio is full
 * @bio:	bio to check
 * @len:	length of one segment to be added
 *
 * Return true if @bio is full and one segment with @len bytes can't be
 * added to the bio, otherwise return false
 */
/*
 * [한국어]
 * bio_full - bio에 len 바이트를 추가할 공간이 남았는지 확인한다
 *
 * @bio: 확인할 bio
 * @len: 추가하려는 데이터 크기(바이트)
 * @return: true이면 공간 부족(가득 참); false이면 추가 가능
 *
 * bi_vcnt >= bi_max_vecs이면 segment 슬롯이 없어 새 bvec을 추가할 수 없다.
 * bi_size + len > BIO_MAX_SIZE이면 최대 데이터 크기를 초과한다.
 * **주의: 이 검사는 장치 한계와 무관하다.** 여기서 보는 두 한계는 bio 라는 자료구조
 * 자체의 한계(배열 슬롯 수, bi_size 필드의 표현 범위)이지, 장치의 max_hw_sectors 나
 * max_segments 가 아니다. 장치 한계 검사와 그에 따른 분할은 제출 이후
 * blk-merge.c 의 bio_split_to_limits() 가 담당한다.
 * 그래서 bio_full() 이 false 여도 그 bio 가 장치에 한 번에 나간다는 보장은 없다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트(I/O 구성 단계).
 * 호출자: bio_add_page(), bio_iov_iter_get_pages() 의 루프 종료 조건.
 */
static inline bool bio_full(struct bio *bio, unsigned len)
{
	if (bio->bi_vcnt >= bio->bi_max_vecs)	/* [한국어] bvec 배열 슬롯을 다 썼다.
	                                         * 병합이 안 되는 새 세그먼트를 넣을 자리가 없다는 뜻. */
		return true;
	if (bio->bi_iter.bi_size > BIO_MAX_SIZE - len)	/* [한국어] bi_size 가 담을 수 있는 최대치를 넘는다.
	                                                 * `bi_size + len > BIO_MAX_SIZE` 로 쓰지 않고 뺄셈으로 옮긴 이유:
	                                                 * 덧셈은 unsigned 오버플로로 작은 값이 되어 검사를 통과해 버릴 수 있다.
	                                                 * BIO_MAX_SIZE >= len 이 보장되므로 뺄셈 쪽은 안전하다. */
		return true;
	return false;	/* [한국어] 아직 더 담을 수 있다 */
}

/*
 * [한국어]
 * bvec_try_merge_page - 새 페이지가 기존 bio_vec segment와 물리적으로 인접하면 병합한다
 *
 * @bv:   기존 bio_vec segment
 * @page: 병합을 시도할 새 페이지
 * @len:  새 데이터의 길이
 * @off:  page 내 오프셋
 * @return: true이면 병합 성공(bv->bv_len 증가); false이면 인접하지 않아 병합 불가
 *
 * 왜 병합하는가: bio_vec 하나는 (page, offset, len) 삼중항이므로 물리적으로 이어지는
 * 두 영역은 하나의 bvec 으로 표현할 수 있다. 병합하면 세그먼트 수가 줄어
 * (1) bvec 배열 슬롯을 아끼고 (2) 이후 DMA 매핑 단계(blk-mq-dma.c)에서 만들어질
 * 디스크립터 개수가 줄어든다. NVMe 라면 그 디스크립터가 PRP 엔트리나 SGL 디스크립터다
 * — 다만 그 변환은 여기가 아니라 드라이버 단계에서 일어난다.
 *
 * 병합을 막는 세 가지 이유가 아래에 순서대로 나온다:
 *   (1) 물리 주소가 이어지지 않음 — 근본적으로 하나의 bvec 으로 표현 불가
 *   (2) Xen — 게스트의 연속 물리주소가 호스트에서는 연속이 아닐 수 있음
 *   (3) struct page 배열이 이어지지 않음(SPARSEMEM 섹션 경계) — bvec 순회 코드가
 *       `page + n` 산술로 다음 페이지를 찾으므로, 물리주소가 이어져도 struct page 가
 *       이어지지 않으면 잘못된 페이지를 가리키게 된다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: bio_add_page(), bvec_try_merge_hw_page()
 */
static bool bvec_try_merge_page(struct bio_vec *bv, struct page *page,
		unsigned int len, unsigned int off)
{
	size_t bv_end = bv->bv_offset + bv->bv_len;	/* [한국어] 기존 bvec 의 끝 오프셋(시작 페이지 기준).
	                                             * bv_len 이 PAGE_SIZE 를 넘을 수 있어 bv_end 도 페이지를 넘길 수 있다. */
	phys_addr_t vec_end_addr = page_to_phys(bv->bv_page) + bv_end - 1;	/* [한국어] 기존 bvec 의 **마지막 바이트** 물리 주소.
	                                                                     * -1 을 하는 이유: bv_end 는 "끝 다음"이라 그대로 쓰면
	                                                                     * 이미 다음 영역의 첫 바이트를 가리킨다. */
	phys_addr_t page_addr = page_to_phys(page);	/* [한국어] 새로 붙이려는 페이지의 시작 물리 주소 */

	if (vec_end_addr + 1 != page_addr + off)	/* [한국어] 기존 끝의 **바로 다음** 바이트가 새 데이터의 시작인가.
	                                             * 아니면 물리적으로 떨어져 있어 하나의 bvec 으로 표현할 수 없다. */
		return false;
	if (xen_domain() && !xen_biovec_phys_mergeable(bv, page))	/* [한국어] Xen 게스트에서는 게스트 물리주소(GFN)가 이어져도
	                                                             * 호스트 물리주소(MFN)가 이어진다는 보장이 없다.
	                                                             * DMA 는 MFN 기준이므로 하이퍼바이저에게 물어봐야 한다.
	                                                             * Xen 이 아니면 xen_domain() 이 컴파일 타임 상수 false 라
	                                                             * 이 검사 전체가 사라진다. */
		return false;

	if ((vec_end_addr & PAGE_MASK) != ((page_addr + off) & PAGE_MASK)) {	/* [한국어] 두 주소가 **서로 다른 페이지 프레임**에 속하는가.
	                                                                         * PAGE_MASK 로 하위 비트를 지워 페이지 번호만 비교한다.
	                                                                         * 같은 페이지 안이라면 struct page 문제가 없으니 아래 검사가 불필요하다. */
		if (IS_ENABLED(CONFIG_KMSAN))	/* [한국어] KMSAN(커널 메모리 초기화 검사기)은 페이지마다
		                                 * 별도의 shadow 메모리를 붙인다. 여러 페이지를 한 bvec 으로 묶으면
		                                 * shadow 추적이 어긋나므로 아예 병합을 금지한다. */
			return false;
		if (bv->bv_page + bv_end / PAGE_SIZE != page + off / PAGE_SIZE)	/* [한국어] **struct page 배열의 연속성** 확인.
		                                                                 * 좌변: 기존 bvec 의 끝 바이트가 속한 page 구조체를
		                                                                 * `시작 page + 넘어간 페이지 수`로 계산한 것.
		                                                                 * 우변: 새 데이터의 첫 바이트가 속한 page 구조체.
		                                                                 * 물리 주소는 이어져도 SPARSEMEM 섹션 경계에서는
		                                                                 * struct page 배열이 끊길 수 있는데, bvec 순회 코드는
		                                                                 * page 포인터 산술로 다음 페이지를 찾으므로
		                                                                 * 여기서 막지 않으면 엉뚱한 page 를 참조하게 된다. */
			return false;
	}

	bv->bv_len += len;	/* [한국어] 병합 성립 — 기존 bvec 의 길이만 늘린다.
	                     * 새 bvec 슬롯을 쓰지 않는 것이 병합의 이득이다. */
	return true;
}

/*
 * Try to merge a page into a segment, while obeying the hardware segment
 * size limit.
 *
 * This is kept around for the integrity metadata, which is still tries
 * to build the initial bio to the hardware limit and doesn't have proper
 * helpers to split.  Hopefully this will go away soon.
 */
/*
 * [한국어]
 * bvec_try_merge_hw_page - 하드웨어 segment 경계/크기 한도를 고려해 페이지 병합을 시도한다
 *
 * @q:      request_queue (max_segment_size, segment_boundary 포함)
 * @bv:     기존 bio_vec segment
 * @page:   병합을 시도할 새 페이지
 * @len:    새 데이터의 길이
 * @offset: page 내 오프셋
 * @return: true이면 병합 성공; false이면 하드웨어 한도 초과 또는 물리 불연속
 *
 * bvec_try_merge_page() 와 달리 **큐(장치) 한계까지** 본다. 두 가지다:
 *   segment_boundary_mask : 한 세그먼트가 넘어서는 안 되는 주소 경계.
 *     오래된 DMA 컨트롤러가 "세그먼트가 64KiB 경계를 넘으면 안 된다" 같은
 *     제약을 갖는 데서 왔다. 두 주소를 마스크로 OR 해 같은 구간인지 본다.
 *   max_segment_size      : 세그먼트 하나의 최대 바이트 수.
 *
 * 위 영문 주석이 밝히듯 이 함수는 **integrity 메타데이터 경로를 위해 남아 있다**.
 * 일반 데이터 경로는 bio 를 하드웨어 한계에 맞춰 만들지 않고 일단 크게 만든 뒤
 * blk-merge.c 에서 쪼개는 방식으로 바뀌었지만, integrity 쪽은 아직 분할 헬퍼가
 * 없어 처음부터 한계에 맞춰 쌓는다("Hopefully this will go away soon").
 *
 * NVMe 참고: PCIe NVMe 드라이버는 dma_set_max_seg_size(pdev, 0xffffffff) 로
 * 세그먼트 크기를 사실상 무제한으로 두므로(drivers/nvme/host/pci.c) 여기서
 * max_segment_size 때문에 병합이 막히는 일은 거의 없다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: block/bio-integrity.c 의 메타데이터 bvec 구성 경로.
 */
bool bvec_try_merge_hw_page(struct request_queue *q, struct bio_vec *bv,
		struct page *page, unsigned len, unsigned offset)
{
	unsigned long mask = queue_segment_boundary(q);	/* [한국어] 큐의 세그먼트 경계 마스크(예: 0xffff = 64KiB 경계).
	                                                 * 제약이 없으면 전 비트 1 이라 아래 비교가 항상 통과한다. */
	phys_addr_t addr1 = bvec_phys(bv);	/* [한국어] 기존 세그먼트의 **시작** 물리 주소 */
	phys_addr_t addr2 = page_to_phys(page) + offset + len - 1;	/* [한국어] 병합했을 때의 **마지막 바이트** 물리 주소 */

	if ((addr1 | mask) != (addr2 | mask))	/* [한국어] 두 주소를 마스크로 OR 하면 각자가 속한 "경계 구간"의 끝 주소가 된다.
	                                         * 그 값이 다르면 시작과 끝이 서로 다른 구간에 있다 = 경계를 넘는다.
	                                         * 뺄셈·나눗셈 없이 구간 비교를 한 번의 OR 로 끝내는 관용구다. */
		return false;
	if (len > queue_max_segment_size(q) - bv->bv_len)	/* [한국어] 병합 후 길이가 세그먼트 크기 상한을 넘는가.
	                                                     * bio_full() 과 같은 이유로 덧셈이 아니라 뺄셈 형태로 쓴다
	                                                     * (오버플로 회피). */
		return false;
	return bvec_try_merge_page(bv, page, len, offset);	/* [한국어] 하드웨어 조건을 통과했으면 물리 인접성 검사는
	                                                     * 공통 함수에 위임한다. 여기서 bv_len 이 실제로 늘어난다. */
}

/**
 * __bio_add_page - add page(s) to a bio in a new segment
 * @bio: destination bio
 * @page: start page to add
 * @len: length of the data to add, may cross pages
 * @off: offset of the data relative to @page, may cross pages
 *
 * Add the data at @page + @off to @bio as a new bvec.  The caller must ensure
 * that @bio has space for another bvec.
 */
/*
 * [한국어]
 * __bio_add_page - bio에 새로운 segment(bio_vec)로 페이지를 추가한다
 *
 * @bio:  페이지를 추가할 bio
 * @page: 추가할 물리 페이지 (compound page의 구성 페이지 가능)
 * @len:  이 segment의 데이터 길이 (페이지 경계를 넘을 수 있음)
 * @off:  page 내 데이터 시작 오프셋
 *
 * bi_vcnt < bi_max_vecs임을 호출자가 보장해야 한다.
 * PCI P2PDMA 페이지인 경우 REQ_NOMERGE를 설정하여 인접 segment와의 병합을 금지한다.
 * bi_io_vec[bi_vcnt]에 새 bvec을 기록하고 bi_vcnt++, bi_size += len.
 * "__" 접두사가 뜻하는 것: 경계 검사를 호출자에게 떠넘긴 저수준 버전이다.
 * 병합 시도도 하지 않고 무조건 새 세그먼트를 만든다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트; BIO_CLONED bio 에서 호출 금지(WARN).
 * 호출자: bio_add_page(), bio_add_folio_nofail(), bio_add_virt_nofail()
 *
 * 호출 체인:
 *   bio_add_page() → [__bio_add_page()] → bvec_set_page()
 */
void __bio_add_page(struct bio *bio, struct page *page,
		unsigned int len, unsigned int off)
{
	WARN_ON_ONCE(bio_flagged(bio, BIO_CLONED));	/* [한국어] 클론의 공유 bvec 배열을 수정하면 원본이 망가진다.
	                                             * 여기서는 WARN 만 하고 그대로 진행한다(저수준 API 계약 위반) —
	                                             * 이 함수는 실패를 반환할 수 없기 때문이다. */
	WARN_ON_ONCE(bio_full(bio, len));	/* [한국어] 호출자가 슬롯 여유를 보장해야 한다는 계약을 검증한다.
	                                     * 위반이면 아래 bi_io_vec[bi_vcnt] 접근이 배열 밖을 쓴다 —
	                                     * 디버그 빌드에서 이 경고가 그 원인을 짚어 준다. */

	/* [한국어] 이 페이지가 PCI P2PDMA(peer-to-peer DMA) 메모리인지 검사한다.
	 * PCIe 장치의 BAR 영역을 struct page 로 노출한 것으로, 데이터가 시스템 RAM 을
	 * 거치지 않고 장치끼리 직접 오간다. NVMe 에서는 컨트롤러의
	 * CMB(Controller Memory Buffer)가 대표적이며, PCIe NVMe 드라이버는
	 * .supports_pci_p2pdma = nvme_pci_supports_pci_p2pdma 로 이 기능을 광고한다
	 * (drivers/nvme/host/pci.c 의 nvme_pci_ctrl_ops). */
	if (is_pci_p2pdma_page(page))
		/* [한국어] P2PDMA 페이지가 섞이면 병합을 금지한다.
		 * 이유: DMA 매핑 방식 자체가 다르다. 일반 시스템 메모리는
		 * dma_map_phys()로 IOVA를 얻지만, P2PDMA 메모리는
		 * pci_p2pdma_bus_addr_map()으로 PCI 버스 주소를 직접 계산한다
		 * (block/blk-mq-dma.c의 blk_dma_map_bus 참고).
		 * 한 request 안에 두 종류가 섞이면 세그먼트마다 매핑 방식을 바꿔야
		 * 하는데, blk_dma_map_iter_start()는 request 단위로 한 번만 P2PDMA
		 * 여부를 판정하므로 그런 혼합을 처리할 수 없다. 그래서 애초에
		 * 병합되지 않도록 막는다. */
		bio->bi_opf |= REQ_NOMERGE;

	bvec_set_page(&bio->bi_io_vec[bio->bi_vcnt], page, len, off);	/* [한국어] 다음 빈 슬롯에 (page, len, off) 를 기록한다.
	                                                                 * bi_vcnt 는 "채워진 개수"이자 곧 "다음 빈 인덱스"다. */
	bio->bi_iter.bi_size += len;	/* [한국어] bio 전체가 다루는 바이트 수를 늘린다.
	                                 * 이 값이 곧 I/O 길이이며, 섹터로 환산되어 장치 명령의 길이 필드가 된다. */
	bio->bi_vcnt++;	/* [한국어] 세그먼트 개수 증가. 반드시 bvec 기록 **뒤에** 증가시켜야
	                 * 중간 상태에서 다른 코드가 초기화되지 않은 슬롯을 읽지 않는다. */
}
EXPORT_SYMBOL_GPL(__bio_add_page);	/* [한국어] 슬롯 여유를 스스로 계산하는 상위 계층용 저수준 API */

/**
 * bio_add_virt_nofail - add data in the direct kernel mapping to a bio
 * @bio: destination bio
 * @vaddr: data to add
 * @len: length of the data to add, may cross pages
 *
 * Add the data at @vaddr to @bio.  The caller must have ensure a segment
 * is available for the added data.  No merging into an existing segment
 * will be performed.
 */
/*
 * [한국어]
 * bio_add_virt_nofail - 커널 직접 매핑(lowmem) 주소를 bio에 세그먼트로 추가
 *
 * @bio:   대상 bio
 * @vaddr: 추가할 데이터의 커널 가상 주소. 반드시 직접 매핑 영역(kmalloc,
 *         페이지 할당자 등에서 얻은 주소)이어야 한다. vmalloc 주소는 물리적으로
 *         연속이 아니라 virt_to_page()가 잘못된 결과를 준다.
 * @len:   길이. 페이지 경계를 넘어도 되지만, 그 경우 물리적 연속성은 호출자가
 *         보장해야 한다(직접 매핑이므로 보장된다).
 * @return: 없음
 *
 * === 이름의 "nofail"이 뜻하는 것 ===
 * 이 함수는 실패를 반환하지 않는다. 대신 호출자가 "bvec 자리가 남아 있음"을
 * 미리 보장해야 한다는 계약을 진다(영문 주석의 "The caller must have ensure
 * a segment is available"). 자리가 없으면 __bio_add_page() 안의
 * WARN_ON_ONCE(bio_full(...))가 경고를 남기고 자료구조가 손상된다.
 * 이런 설계를 택한 이유는 호출자들이 대개 bio를 방금 자기가 할당해 크기를
 * 정확히 알고 있는 커널 내부 코드라, 매번 반환값을 검사하는 것이 불필요한
 * 잡음이기 때문이다.
 *
 * === 어디에 쓰이는가 ===
 * 커널이 스스로 만든 버퍼(kmalloc/정적 버퍼)를 장치에 보낼 때다.
 * 대표 경로는 block/blk-map.c 의 blk_rq_map_kern() 으로, 직접 매핑 주소면
 * 이 함수를, vmalloc 주소면 bio_add_vmalloc() 을 쓴다(blk-map.c:867-872).
 *
 * NVMe 에서 실제로 이 경로를 타는 것이 확인된다:
 *   drivers/nvme/host/core.c:1481 __nvme_submit_sync_cmd()
 *     → blk_rq_map_kern(req, buffer, bufflen, GFP_KERNEL)
 *     → (blk-map.c) bio_add_virt_nofail()
 * 즉 Identify / Set Features / Get Log Page 같은 **제어 평면 명령**이 결과를
 * 받아 올 커널 버퍼를 bio 에 붙일 때 여기를 지난다. 사용자 데이터 I/O 경로가
 * 아니라 드라이버가 스스로 발행하는 동기 명령 경로다.
 *
 * 병합 시도를 하지 않는(No merging) 이유도 같다 — 호출자가 세그먼트 배치를
 * 이미 계획했으므로 커널이 임의로 합치면 그 계획이 어긋난다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트. 할당이나 대기가 없어 원자적 컨텍스트에서도
 * 안전하지만, 관례상 제출 준비 단계에서 호출된다.
 *
 * 호출 체인:
 *   blk_rq_map_kern / 드라이버 내부 명령 준비 → [bio_add_virt_nofail]
 *     → __bio_add_page → bvec_set_page
 */
void bio_add_virt_nofail(struct bio *bio, void *vaddr, unsigned len)
{
	/* [한국어] 가상 주소를 (page, offset) 쌍으로 분해해 bvec 하나를 추가한다.
	 *   virt_to_page(vaddr)     - 직접 매핑 주소에서 struct page를 얻는다.
	 *                             단순한 오프셋 산술이라 매우 저렴하다.
	 *   offset_in_page(vaddr)   - 페이지 안에서의 오프셋(하위 12비트).
	 * bio_vec 은 (page, offset, len) 삼중항이므로 이 분해가 필요하다.
	 * 결과 bvec 는 훨씬 뒤 단계에서 blk_rq_dma_map()(block/blk-mq-dma.c)을 거쳐
	 * DMA 주소가 되고, 그제서야 드라이버가 그것을 자기 형식의 디스크립터로 적는다
	 * (NVMe 라면 PRP 엔트리 또는 SGL 디스크립터). bio.c 는 여기까지만 관여한다. */
	__bio_add_page(bio, virt_to_page(vaddr), len, offset_in_page(vaddr));
}
EXPORT_SYMBOL_GPL(bio_add_virt_nofail);

/**
 *	bio_add_page	-	attempt to add page(s) to bio
 *	@bio: destination bio
 *	@page: start page to add
 *	@len: vec entry length, may cross pages
 *	@offset: vec entry offset relative to @page, may cross pages
 *
 *	Attempt to add page(s) to the bio_vec maplist. This will only fail
 *	if either bio->bi_vcnt == bio->bi_max_vecs or it's a cloned bio.
 */
/*
 * [한국어]
 * bio_add_page - 페이지를 bio의 기존 segment에 병합하거나, 불가능하면 새 segment로 추가한다
 *
 * @bio:    페이지를 추가할 bio
 * @page:   추가할 물리 페이지
 * @len:    이 데이터의 길이
 * @offset: page 내 데이터 시작 오프셋
 * @return: 추가된 바이트 수; 실패 시 0
 *
 * 동작: 먼저 **마지막 bvec 과의 병합**을 시도하고(물리 인접이면 그 bvec 의 길이만 늘림),
 * 불가능하면 새 bvec 슬롯을 하나 쓴다. 슬롯이 없으면 0 을 반환해
 * "이 bio 는 다 찼으니 새 bio 를 만들라"고 호출자에게 알린다.
 *
 * 반환값이 len 아니면 0 인 전부-아니면-전무 방식이라는 점에 유의. 부분 추가는 없다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: 파일시스템 I/O 구성, bio_add_folio(), bio_add_vmalloc_chunk(), blk-map.c
 *
 * 호출 체인:
 *   파일시스템 → [bio_add_page()] → bvec_try_merge_page() 또는 __bio_add_page()
 */
int bio_add_page(struct bio *bio, struct page *page,
		 unsigned int len, unsigned int offset)
{
	if (WARN_ON_ONCE(bio_flagged(bio, BIO_CLONED)))	/* [한국어] 클론은 bvec 배열을 원본과 공유하므로
	                                                 * 여기에 페이지를 추가하면 원본이 보는 데이터가 바뀐다.
	                                                 * 조용히 실패시키지 않고 WARN 을 남겨 버그를 드러낸다. */
		return 0;
	if (WARN_ON_ONCE(len == 0))	/* [한국어] 길이 0 짜리 bvec 은 무의미하고, 순회 코드가
	                             * 무한 루프나 0 나눗셈에 빠질 수 있다. 호출자 버그로 간주. */
		return 0;
	if (bio->bi_iter.bi_size > BIO_MAX_SIZE - len)	/* [한국어] bi_size 표현 한계 초과 방지.
	                                                 * bio_full() 과 같은 오버플로 회피 형태. */
		return 0;

	if (bio->bi_vcnt > 0) {	/* [한국어] 이미 세그먼트가 하나라도 있어야 "직전 것과 병합"을 시도할 수 있다 */
		struct bio_vec *bv = &bio->bi_io_vec[bio->bi_vcnt - 1];	/* [한국어] 마지막(가장 최근) bvec.
		                                                         * 순차 I/O 에서는 새로 추가되는 페이지가
		                                                         * 직전 것과 이어지는 경우가 많아 이 한 칸만 봐도 충분하다. */

		if (!zone_device_pages_have_same_pgmap(bv->bv_page, page))	/* [한국어] ZONE_DEVICE 페이지(P2PDMA, DAX 등)는
		                                                             * 어느 dev_pagemap 에 속하는지가 중요하다.
		                                                             * 서로 다른 pgmap 의 페이지를 한 bio 에 섞으면
		                                                             * 나중에 DMA 매핑 단계에서 "이 bio 는 P2P 인가 아닌가"를
		                                                             * 일관되게 판정할 수 없다. 그래서 아예 추가를 거부한다.
		                                                             * (일반 시스템 메모리끼리는 항상 true 를 준다) */
			return 0;

		if (bvec_try_merge_page(bv, page, len, offset)) {	/* [한국어] 물리 인접이면 bv->bv_len 을 늘리고 true */
			bio->bi_iter.bi_size += len;	/* [한국어] 병합 경로에서는 bi_vcnt 가 늘지 않으므로
			                                 * 전체 크기만 갱신하면 된다. 슬롯을 아낀 것이 이득. */
			return len;
		}
	}

	if (bio->bi_vcnt >= bio->bi_max_vecs)	/* [한국어] 병합이 안 됐는데 새 슬롯도 없다 = 이 bio 는 만석이다 */
		return 0;	/* [한국어] 0 을 받은 호출자는 보통 이 bio 를 제출하고 새 bio 를 할당한다 */
	__bio_add_page(bio, page, len, offset);	/* [한국어] 새 세그먼트로 추가(경계 검사는 위에서 이미 끝냈다) */
	return len;	/* [한국어] 요청한 길이 전체를 받아들였음 */
}
EXPORT_SYMBOL(bio_add_page);	/* [한국어] 파일시스템·드라이버 전반이 쓰는 가장 기본적인 페이지 추가 API */

/*
 * [한국어]
 * bio_add_folio_nofail - folio의 일부를 bio에 반드시 추가한다 (실패하지 않음을 전제)
 *
 * @bio:   추가 대상 bio
 * @folio: 추가할 folio (PAGE_SIZE 정렬, 4GiB 미만)
 * @len:   추가할 바이트 수 (BIO_MAX_SIZE 이하여야 함 — WARN)
 * @off:   folio 내 시작 오프셋
 *
 * __bio_add_page()를 직접 호출하므로 실패 경로가 없다.
 * 호출자는 bio에 충분한 bvec 슬롯이 있음을 보장해야 한다.
 * 실행 컨텍스트: 태스크 컨텍스트(파일시스템 I/O 구성 경로).
 * 호출자: folio 단위로 I/O 를 구성하는 파일시스템 — 슬롯 여유를 스스로 계산한 경우.
 *
 * 호출 체인:
 *   fs I/O path → [bio_add_folio_nofail()] → __bio_add_page()
 */
void bio_add_folio_nofail(struct bio *bio, struct folio *folio, size_t len,
			  size_t off)
{
	unsigned long nr = off / PAGE_SIZE;	/* [한국어] folio 안에서 몇 번째 page 부터 시작하는가.
	                                     * folio 는 여러 페이지의 묶음이므로, off 가 PAGE_SIZE 를 넘으면
	                                     * folio 의 첫 페이지가 아니라 그 뒤 페이지가 bvec 의 시작이 된다. */

	WARN_ON_ONCE(len > BIO_MAX_SIZE);	/* [한국어] bvec 하나가 bio 전체 한계보다 클 수는 없다.
	                                     * 위 영문 주석의 "BIOs do not support folios that are 4GiB or larger"
	                                     * 제약과 같은 맥락 — bv_len 이 unsigned int 라 표현 범위가 있다. */
	__bio_add_page(bio, folio_page(folio, nr), len, off % PAGE_SIZE);	/* [한국어] folio_page(folio, nr) 로 시작 struct page 를 얻고,
	                                                                     * off % PAGE_SIZE 로 그 페이지 안의 오프셋을 구한다.
	                                                                     * 즉 folio 좌표를 (page, page 내 오프셋)로 정규화하는 것.
	                                                                     * len 이 페이지를 넘어가도 folio 안은 물리적으로 연속이라 문제없다. */
}
EXPORT_SYMBOL_GPL(bio_add_folio_nofail);

/**
 * bio_add_folio - Attempt to add part of a folio to a bio.
 * @bio: BIO to add to.
 * @folio: Folio to add.
 * @len: How many bytes from the folio to add.
 * @off: First byte in this folio to add.
 *
 * Filesystems that use folios can call this function instead of calling
 * bio_add_page() for each page in the folio.  If @off is bigger than
 * PAGE_SIZE, this function can create a bio_vec that starts in a page
 * after the bv_page.  BIOs do not support folios that are 4GiB or larger.
 *
 * Return: Whether the addition was successful.
 */
/*
 * [한국어]
 * bio_add_folio - folio의 일부를 bio에 추가한다 (실패 가능)
 *
 * @bio:    추가 대상 bio
 * @folio:  추가할 folio
 * @len:    추가할 바이트 수 (BIO_MAX_SIZE 초과 시 false 반환)
 * @off:    folio 내 시작 오프셋
 * @return: true 추가 성공; false len 초과 또는 bvec 슬롯 부족
 *
 * len > BIO_MAX_SIZE이면 즉시 false를 반환한다.
 * 그 외에는 bio_add_page()를 위임하며, 0이면 false 반환.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: 파일 시스템 write/read 경로
 * NVMe 연결: folio 내 page → bio_vec bv_page → nvme_queue_rq()에서 PRP/SGL entry 구성.
 *
 * 호출 체인:
 *   fs write path → [bio_add_folio()] → bio_add_page() → __bio_add_page()
 */
bool bio_add_folio(struct bio *bio, struct folio *folio, size_t len,  // folio 페이지를 bio에 추가: NVMe PRP/SGL segment 후보
		   size_t off)
{
	unsigned long nr = off / PAGE_SIZE;

	if (len > BIO_MAX_SIZE)
		return false;
	return bio_add_page(bio, folio_page(folio, nr), len, off % PAGE_SIZE) > 0;  // 사용자/커널 페이지를 bio에 추가 -> NVMe PRP/SGL 후보
}
EXPORT_SYMBOL(bio_add_folio);  // folio 페이지를 bio에 추가: NVMe PRP/SGL segment 후보

/**
 * bio_add_vmalloc_chunk - add a vmalloc chunk (single page) to a bio
 * @bio: destination bio
 * @vaddr: vmalloc address to add
 * @len: total length in bytes of the data to add
 *
 * Add data starting at @vaddr to @bio and return how many bytes were added.
 * This may be less than the amount originally asked.  Returns 0 if no data
 * could be added to @bio.
 *
 * This helper calls flush_kernel_vmap_range() for the range added.  For reads
 * the caller still needs to manually call invalidate_kernel_vmap_range() in
 * the completion handler.
 */
/*
 * [한국어]
 * bio_add_vmalloc_chunk - vmalloc 가상 주소 내 단일 페이지 청크를 bio에 추가한다
 *
 * @bio:   추가 대상 bio
 * @vaddr: vmalloc 가상 주소 (vmalloc_to_page()로 물리 페이지를 얻을 수 있어야 함)
 * @len:   추가하려는 바이트 수 (PAGE_SIZE - offset을 초과할 수 없음)
 * @return: 실제로 추가된 바이트 수; bio에 공간이 없으면 0
 *
 * vmalloc 주소는 페이지 경계를 넘어 연속적이지 않을 수 있으므로,
 * 현재 페이지 내에서만 추가하고 그 이상은 bio_add_vmalloc()이 반복 호출한다.
 * 쓰기 I/O이면 flush_kernel_vmap_range()로 CPU 캐시를 flush하여 NVMe DMA 일관성을 보장한다.
 * 읽기 I/O는 완료 핸들러에서 invalidate_kernel_vmap_range()를 호출해야 한다.
 * 실행 컨텍스트: 태스크 컨텍스트 (I/O 구성 경로).
 * 호출자: bio_add_vmalloc()
 * NVMe 연결: vmalloc_to_page()로 얻은 물리 페이지가 NVMe PRP/SGL entry가 됨.
 *
 * 호출 체인:
 *   bio_add_vmalloc() → [bio_add_vmalloc_chunk()] → vmalloc_to_page() + bio_add_page()
 */
unsigned int bio_add_vmalloc_chunk(struct bio *bio, void *vaddr, unsigned len)
{
	unsigned int offset = offset_in_page(vaddr);

	len = min(len, PAGE_SIZE - offset);
	if (bio_add_page(bio, vmalloc_to_page(vaddr), len, offset) < len)  // 사용자/커널 페이지를 bio에 추가 -> NVMe PRP/SGL 후보
		return 0;
	if (op_is_write(bio_op(bio)))
		flush_kernel_vmap_range(vaddr, len);  // vmap 범위 flush: NVMe DMA 일관성 유지
	return len;
}
EXPORT_SYMBOL_GPL(bio_add_vmalloc_chunk);

/**
 * bio_add_vmalloc - add a vmalloc region to a bio
 * @bio: destination bio
 * @vaddr: vmalloc address to add
 * @len: total length in bytes of the data to add
 *
 * Add data starting at @vaddr to @bio.  Return %true on success or %false if
 * @bio does not have enough space for the payload.
 *
 * This helper calls flush_kernel_vmap_range() for the range added.  For reads
 * the caller still needs to manually call invalidate_kernel_vmap_range() in
 * the completion handler.
 */
/*
 * [한국어]
 * bio_add_vmalloc - vmalloc 가상 주소 범위 전체를 bio에 추가한다
 *
 * @bio:   추가 대상 bio
 * @vaddr: vmalloc 시작 가상 주소
 * @len:   추가할 전체 바이트 수
 * @return: true 전체 추가 성공; false bio에 bvec 슬롯이 부족하여 중단
 *
 * bio_add_vmalloc_chunk()를 반복 호출하여 페이지 단위로 분리해 추가한다.
 * vmalloc은 물리적으로 비연속이므로 페이지 단위로 PRP/SGL entry를 구성해야 한다.
 * 쓰기 I/O의 경우 각 청크마다 flush_kernel_vmap_range()가 호출된다.
 * 실행 컨텍스트: 태스크 컨텍스트 (I/O 구성 경로).
 * 호출자: 커널 내부에서 vmalloc 버퍼로 direct I/O를 구성하는 코드
 * NVMe 연결: vmalloc 영역의 각 물리 페이지가 별도의 NVMe PRP/SGL entry가 됨.
 *
 * 호출 체인:
 *   커널 I/O path → [bio_add_vmalloc()] → bio_add_vmalloc_chunk() × N
 */
bool bio_add_vmalloc(struct bio *bio, void *vaddr, unsigned int len)  // vmalloc 영역을 bio에 추가: NVMe DMA를 위해 페이지 매핑
{
	do {
		unsigned int added = bio_add_vmalloc_chunk(bio, vaddr, len);

		if (!added)
			return false;
		vaddr += added;
		len -= added;
	} while (len);

	return true;
}
EXPORT_SYMBOL_GPL(bio_add_vmalloc);  // vmalloc 영역을 bio에 추가: NVMe DMA를 위해 페이지 매핑

/*
 * [한국어]
 * __bio_release_pages - bio가 pin한 사용자 페이지를 모두 해제한다
 *
 * @bio:        해제할 bio
 * @mark_dirty: true이면 각 folio를 dirty로 마킹한 후 unpin (읽기 완료 후 사용)
 *
 * bio_for_each_folio_all()로 모든 folio를 순회하며 unpin_user_folio()를 호출한다.
 * mark_dirty=true이면 folio_lock() + folio_mark_dirty() + folio_unlock() 후 unpin.
 * NVMe 읽기 완료 후 사용자 페이지에 데이터가 기록되었으므로 dirty 마킹이 필요하다.
 * 실행 컨텍스트: 태스크 컨텍스트 또는 완료 워크큐 (bio_dirty_fn).
 * 호출자: bio_release_pages() 매크로, bio_dirty_fn()
 * NVMe 연결: NVMe DMA가 완료된 사용자 페이지의 get_user_pages() pin을 해제하는 단계.
 *
 * 호출 체인:
 *   bio_endio() → bio_release_pages() → [__bio_release_pages()] → unpin_user_folio()
 */
void __bio_release_pages(struct bio *bio, bool mark_dirty)
{
	struct folio_iter fi;

	bio_for_each_folio_all(fi, bio) {  // bio_vec(bvec) 순회: NVMe PRP entry/SGL segment를 구성하는 단위
		size_t nr_pages;

		if (mark_dirty) {
			folio_lock(fi.folio);
			folio_mark_dirty(fi.folio);
			folio_unlock(fi.folio);
		}
		nr_pages = (fi.offset + fi.length - 1) / PAGE_SIZE -
			   fi.offset / PAGE_SIZE + 1;
		unpin_user_folio(fi.folio, nr_pages);
	}
}
EXPORT_SYMBOL_GPL(__bio_release_pages);

/*
 * [한국어]
 * bio_iov_bvec_set - iov_iter의 bvec 배열을 bio에 직접 바인딩한다
 *
 * @bio:  설정 대상 bio (bi_max_vecs == 0이어야 함)
 * @iter: ITER_BVEC 타입의 iov_iter; 내부 bvec 배열이 bio에 직접 연결됨
 *
 * iter의 bvec 포인터를 bio->bi_io_vec로 직접 설정하여 복사 없이 공유한다.
 * BIO_CLONED 플래그를 설정하므로 bio_put() 시 bi_io_vec를 해제하지 않는다.
 * bi_bvec_done에 iter->iov_offset을 설정하여 bvec 내 부분 소비 위치를 보존한다.
 * 실행 컨텍스트: 태스크 컨텍스트 (io_uring / direct I/O 경로).
 * 호출자: bio_iov_iter_get_pages() — ITER_BVEC 타입 iter 처리 시
 * NVMe 연결: iter의 bvec가 곧 NVMe PRP/SGL entry의 원본 페이지 목록이 됨.
 *
 * 호출 체인:
 *   bio_iov_iter_get_pages() → [bio_iov_bvec_set()] → BIO_CLONED 설정
 */
void bio_iov_bvec_set(struct bio *bio, const struct iov_iter *iter)
{
	WARN_ON_ONCE(bio->bi_max_vecs);  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향

	bio->bi_io_vec = (struct bio_vec *)iter->bvec;
	bio->bi_iter.bi_idx = 0;
	bio->bi_iter.bi_bvec_done = iter->iov_offset;
	bio->bi_iter.bi_size = iov_iter_count(iter);  // NVMe 명령의 NLB(Length)로 변환됨
	bio_set_flag(bio, BIO_CLONED);	// NVMe PRP/SGL 원본 공유 표시
}

/*
 * Aligns the bio size to the len_align_mask, releasing excessive bio vecs that
 * __bio_iov_iter_get_pages may have inserted, and reverts the trimmed length
 * for the next iteration.
 */
/*
 * [한국어]
 * bio_iov_iter_align_down - bio 크기를 len_align_mask 경계에 맞게 내림 정렬한다
 *
 * @bio:           정렬할 bio
 * @iter:          정렬 후 되돌릴 iov_iter (iov_iter_revert() 호출)
 * @len_align_mask: 정렬 마스크 — 예: (512-1) → 512 바이트 경계
 * @return:        0 성공; -EFAULT bio_vcnt가 0이 된 경우
 *
 * __bio_iov_iter_get_pages()가 삽입한 과잉 bvec을 제거하고,
 * bio->bi_iter.bi_size를 len_align_mask 기준으로 내림 정렬한다.
 * 내려진 nbytes만큼 iov_iter_revert()로 iter 위치를 되돌린다.
 * 실행 컨텍스트: 태스크 컨텍스트 (io_uring / direct I/O 경로).
 * 호출자: bio_iov_iter_get_pages()
 * NVMe 연결: bio_size는 결국 NVMe NLB(Number of Logical Blocks)로 변환되므로
 *            sector 크기(512B) 정렬이 필수.
 *
 * 호출 체인:
 *   bio_iov_iter_get_pages() → [bio_iov_iter_align_down()] → iov_iter_revert()
 */
static int bio_iov_iter_align_down(struct bio *bio, struct iov_iter *iter,
			    unsigned len_align_mask)
{
	size_t nbytes = bio->bi_iter.bi_size & len_align_mask;  // NVMe 명령의 NLB(Length)로 변환됨

	if (!nbytes)
		return 0;

	iov_iter_revert(iter, nbytes);
	bio->bi_iter.bi_size -= nbytes;  // NVMe 명령의 NLB(Length)로 변환됨
	do {
		struct bio_vec *bv = &bio->bi_io_vec[bio->bi_vcnt - 1];  // NVMe 명령의 PRP entry/SGL segment 개수 집계

		if (nbytes < bv->bv_len) {
			bv->bv_len -= nbytes;
			break;
		}

		if (bio_flagged(bio, BIO_PAGE_PINNED))  // 사용자 페이지 pin 상태: NVMe DMA 엔진이 페이지를 안전하게 접근
			unpin_user_page(bv->bv_page);

		bio->bi_vcnt--;  // NVMe 명령의 PRP entry/SGL segment 개수 집계
		nbytes -= bv->bv_len;
	} while (nbytes);

	if (!bio->bi_vcnt)  // NVMe 명령의 PRP entry/SGL segment 개수 집계
		return -EFAULT;
	return 0;
}

/**
 * bio_iov_iter_get_pages - add user or kernel pages to a bio
 * @bio: bio to add pages to
 * @iter: iov iterator describing the region to be added
 * @len_align_mask: the mask to align the total size to, 0 for any length
 *
 * This takes either an iterator pointing to user memory, or one pointing to
 * kernel pages (BVEC iterator). If we're adding user pages, we pin them and
 * map them into the kernel. On IO completion, the caller should put those
 * pages. For bvec based iterators bio_iov_iter_get_pages() uses the provided
 * bvecs rather than copying them. Hence anyone issuing kiocb based IO needs
 * to ensure the bvecs and pages stay referenced until the submitted I/O is
 * completed by a call to ->ki_complete() or returns with an error other than
 * -EIOCBQUEUED. The caller needs to check if the bio is flagged BIO_NO_PAGE_REF
 * on IO completion. If it isn't, then pages should be released.
 *
 * The function tries, but does not guarantee, to pin as many pages as
 * fit into the bio, or are requested in @iter, whatever is smaller. If
 * MM encounters an error pinning the requested pages, it stops. Error
 * is returned only if 0 pages could be pinned.
 */
/*
 * [한국어]
 * bio_iov_iter_get_pages - 사용자/커널 페이지를 iov_iter에서 추출해 bio_vec에 채운다
 *
 * @bio:           페이지를 채울 bio
 * @iter:          사용자 버퍼를 기술하는 iov_iter; bvec 기반이면 직접 설정
 * @len_align_mask: 정렬 마스크 (0이면 임의 길이 허용)
 * @return: 0 성공; 음수 에러 코드
 *
 * BVEC 기반 iter: bio_iov_bvec_set()으로 직접 설정.
 * 사용자 메모리 iter: iov_iter_extract_bvecs()로 페이지를 pin하고 bio_vec 배열에 채움.
 * BIO_PAGE_PINNED: 페이지가 pin되었을 때 설정; 완료 후 호출자가 해제해야 함.
 * P2PDMA 지원: bi_bdev의 큐가 P2PDMA를 지원하면 ITER_ALLOW_P2PDMA 설정.
 * 실행 컨텍스트: 태스크 컨텍스트 (DIO 제출 경로).
 * 호출자: kiocb 기반 DIO (파일 시스템 direct_IO)
 * NVMe 연결: 추출된 사용자 페이지 → bio_vec → nvme_map_data() → PRP/SGL → NVMe DMA.
 *
 * 호출 체인:
 *   kiocb DIO → [bio_iov_iter_get_pages()] → iov_iter_extract_bvecs() → NVMe 제출
 */
int bio_iov_iter_get_pages(struct bio *bio, struct iov_iter *iter,  // 사용자 페이지 pin/mapping: NVMe DMA를 위해 커널이 페이지를 고정
			   unsigned len_align_mask)
{
	iov_iter_extraction_t flags = 0;

	if (WARN_ON_ONCE(bio_flagged(bio, BIO_CLONED)))  // clone된 bio: PRP/SGL 원본을 공유하므로 중간 수정 불가
		return -EIO;  // I/O 오류: NVMe 명령 제출/완료 실패 전파

	if (iov_iter_is_bvec(iter)) {
		bio_iov_bvec_set(bio, iter);
		iov_iter_advance(iter, bio->bi_iter.bi_size);  // NVMe 명령의 NLB(Length)로 변환됨
		return 0;
	}

	if (iov_iter_extract_will_pin(iter))  // 사용자 페이지 pin 여부: NVMe DMA 안전성 판단
		bio_set_flag(bio, BIO_PAGE_PINNED);	// 사용자 페이지 pin: NVMe DMA 안전 보장
	/* [한국어] 대상 장치가 P2PDMA를 지원한다고 큐에 표시했는지 확인한다.
	 * NVMe PCIe 드라이버는 CMB를 쓸 수 있을 때 QUEUE_FLAG_PCI_P2PDMA를 세운다. */
	if (bio->bi_bdev && blk_queue_pci_p2pdma(bio->bi_bdev->bd_disk->queue))
		/* [한국어] 사용자 iov에서 페이지를 추출할 때 P2PDMA 페이지를 허용한다.
		 * 이 플래그가 없으면 iov_iter가 장치 메모리 페이지를 거부한다 —
		 * 대상 장치가 그런 페이지로 DMA할 수 있는지 모르는 상태에서 통과시키면
		 * 매핑 단계에서 실패하기 때문이다. 큐가 지원을 선언했으므로 허용한다.
		 * 이 경로가 열려야 사용자 공간이 다른 PCIe 장치(GPU 등)의 메모리를
		 * O_DIRECT 버퍼로 직접 넘겨 시스템 RAM 복사를 건너뛸 수 있다. */
		flags |= ITER_ALLOW_P2PDMA;

	do {
		ssize_t ret;

		ret = iov_iter_extract_bvecs(iter, bio->bi_io_vec,
				BIO_MAX_SIZE - bio->bi_iter.bi_size,  // NVMe 명령의 NLB(Length)로 변환됨
				&bio->bi_vcnt, bio->bi_max_vecs, flags);  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향
		if (ret <= 0) {
			if (!bio->bi_vcnt)  // NVMe 명령의 PRP entry/SGL segment 개수 집계
				return ret;
			break;
		}
		bio->bi_iter.bi_size += ret;  // NVMe 명령의 NLB(Length)로 변환됨
	} while (iov_iter_count(iter) && !bio_full(bio, 0));  // bio가 가득 찼는지 확인: NVMe segment/MDTS 한도 초과 신호

	/* [한국어] 추출된 첫 페이지가 P2PDMA 메모리이면 이 bio 전체를 병합 금지로
	 * 표시한다. 첫 페이지만 검사하는 것으로 충분한 이유: iov_iter가 한 번의
	 * 추출에서 P2PDMA 페이지와 일반 페이지를 섞지 않기 때문이다(섞이면
	 * 추출 단계에서 끊긴다). 병합을 막는 이유는 __bio_add_page()의 경우와
	 * 같다 — 매핑 방식이 다른 두 종류의 메모리를 한 request에 담을 수 없다. */
	if (is_pci_p2pdma_page(bio->bi_io_vec->bv_page))
		bio->bi_opf |= REQ_NOMERGE;
	return bio_iov_iter_align_down(bio, iter, len_align_mask);
}

/*
 * [한국어]
 * folio_alloc_greedy - 가능한 최대 크기의 folio를 greedy하게 할당한다
 *
 * @gfp:  GFP 할당 플래그
 * @size: 원하는 크기(바이트); 실패 시 절반으로 줄여가며 재시도; 성공 크기로 업데이트됨
 * @return: 할당된 folio; 실패 시 NULL
 *
 * PAGE_SIZE 초과 크기를 __GFP_NORETRY로 시도하고 실패하면 크기를 절반씩 줄인다.
 * PAGE_SIZE까지 줄어들면 일반 folio_alloc()으로 최종 시도한다.
 * bounce buffer, DIO 읽기 버퍼 등에서 큰 연속 메모리가 필요할 때 사용한다.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: bio_iov_iter_bounce_write(), bio_iov_iter_bounce_read()
 * NVMe 연결: 큰 bounce buffer = 더 적은 PRP/SGL 항목 → NVMe 명령 오버헤드 감소.
 */
static struct folio *folio_alloc_greedy(gfp_t gfp, size_t *size)
{
	struct folio *folio;

	while (*size > PAGE_SIZE) { /* [한국어] PAGE_SIZE 초과 크기를 greedy하게 시도: 실패 시 절반으로 줄임 */
		folio = folio_alloc(gfp | __GFP_NORETRY, get_order(*size)); /* [한국어] 빠른 실패(__GFP_NORETRY)로 시도: OOM killer 호출 없음 */
		if (folio)
			return folio; /* [한국어] 성공: 요청 크기의 folio 반환 */
		*size = rounddown_pow_of_two(*size - 1); /* [한국어] 실패 시 2의 제곱수로 내림해 크기 절반으로 줄임 */
	}

	return folio_alloc(gfp, get_order(*size)); /* [한국어] PAGE_SIZE까지 줄어든 경우 일반 할당 시도 */
}

/*
 * [한국어]
 * bio_free_folios - bio의 모든 bvec가 가리키는 folio를 해제한다
 *
 * @bio: folio를 해제할 bio
 *
 * bounce buffer로 할당된 folio들을 해제할 때 사용한다.
 * zero folio(공유 zero page)는 해제하지 않는다.
 * 실행 컨텍스트: 태스크 컨텍스트 (DIO 완료 경로).
 * 호출자: bio_iov_iter_unbounce() (write path), bio_iov_iter_bounce_read() 실패 경로
 */
static void bio_free_folios(struct bio *bio)
{
	struct bio_vec *bv;
	int i;

	bio_for_each_bvec_all(bv, bio, i) {  // bio_vec(bvec) 순회: NVMe PRP entry/SGL segment를 구성하는 단위
		struct folio *folio = page_folio(bv->bv_page);

		if (!is_zero_folio(folio))
			folio_put(folio);
	}
}

/*
 * [한국어]
 * bio_iov_iter_bounce_write - 쓰기 DIO를 위한 bounce buffer를 할당하고 사용자 데이터를 복사한다
 *
 * @bio:    제출할 bio (비어 있어야 함)
 * @iter:   사용자 쓰기 버퍼를 기술하는 iov_iter
 * @maxlen: bounce할 최대 바이트 수
 * @return: 0 성공; -ENOMEM 또는 -EFAULT
 *
 * folio_alloc_greedy()로 bounce buffer를 할당하고 copy_from_iter()로 사용자 데이터를 복사한다.
 * 복사 실패 시 이미 할당된 folio들을 bio_free_folios()로 해제한다.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: bio_iov_iter_bounce()
 * NVMe 연결: 할당된 bounce buffer 페이지들이 NVMe PRP/SGL로 변환되어 DMA 전송.
 */
static int bio_iov_iter_bounce_write(struct bio *bio, struct iov_iter *iter,
		size_t maxlen)
{
	size_t total_len = min(maxlen, iov_iter_count(iter));

	if (WARN_ON_ONCE(bio_flagged(bio, BIO_CLONED)))  // clone된 bio: PRP/SGL 원본을 공유하므로 중간 수정 불가
		return -EINVAL;  // 잘못된 인자: NVMe SQ에 잘못된 명령이 들어가기 전 차단
	if (WARN_ON_ONCE(bio->bi_iter.bi_size))  // NVMe 명령의 NLB(Length)로 변환됨
		return -EINVAL;  // 잘못된 인자: NVMe SQ에 잘못된 명령이 들어가기 전 차단
	if (WARN_ON_ONCE(bio->bi_vcnt >= bio->bi_max_vecs))  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향
		return -EINVAL;  // 잘못된 인자: NVMe SQ에 잘못된 명령이 들어가기 전 차단

	do {
		size_t this_len = min(total_len, SZ_1M);
		struct folio *folio;

		if (this_len > PAGE_SIZE * 2)
			this_len = rounddown_pow_of_two(this_len);

		if (bio->bi_iter.bi_size > BIO_MAX_SIZE - this_len)  // NVMe 명령의 NLB(Length)로 변환됨
			break;

		/* [한국어] 남은 길이(this_len)만큼을 담을 수 있는 가장 큰 folio를 잡는다.
		 * 큰 folio 하나 = 물리적으로 연속된 큰 영역 = 세그먼트 하나이므로,
		 * 결과적으로 NVMe PRP 엔트리나 SGL 디스크립터 개수가 줄어든다.
		 * 실패하면 folio_alloc_greedy()가 크기를 절반씩 줄여 재시도하고,
		 * 실제로 잡은 크기를 this_len에 되돌려 준다(in/out 파라미터). */
		folio = folio_alloc_greedy(GFP_KERNEL, &this_len);
		if (!folio)
			break;
		bio_add_folio_nofail(bio, folio, this_len, 0);

		if (copy_from_iter(folio_address(folio), this_len, iter) !=  // 사용자 버퍼 -> bounce buffer 복사: NVMe DMA 전송 전 준비
				this_len) {
			bio_free_folios(bio);
			return -EFAULT;
		}

		total_len -= this_len;
	} while (total_len && bio->bi_vcnt < bio->bi_max_vecs);  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향

	if (!bio->bi_iter.bi_size)  // NVMe 명령의 NLB(Length)로 변환됨
		return -ENOMEM;
	return 0;
}

/*
 * [한국어]
 * bio_iov_iter_bounce_read - 읽기 DIO를 위한 bounce buffer를 할당하고 사용자 페이지를 pin한다
 *
 * @bio:    제출할 bio (비어 있어야 함)
 * @iter:   사용자 읽기 버퍼를 기술하는 iov_iter
 * @maxlen: bounce할 최대 바이트 수 (최대 1MB)
 * @return: 0 성공; 음수 에러
 *
 * bi_io_vec[0]에 bounce buffer folio를 설정하고, bi_io_vec[1+]에 사용자 페이지를 pin한다.
 * NVMe는 bounce buffer로 데이터를 읽고, bio_iov_iter_unbounce_read()가 완료 후 사용자 페이지로 복사한다.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: bio_iov_iter_bounce()
 * NVMe 연결: bounce buffer(bi_io_vec[0])가 NVMe PRP/SGL의 첫 번째 항목으로 DMA 수신.
 */
static int bio_iov_iter_bounce_read(struct bio *bio, struct iov_iter *iter,
		size_t maxlen)
{
	size_t len = min3(iov_iter_count(iter), maxlen, SZ_1M);
	struct folio *folio;

	/* [한국어] 요청 길이(len)를 담을 수 있는 가장 큰 folio를 잡는다. 위와 같은
	 * 이유로 큰 덩어리일수록 세그먼트 수가 줄어 NVMe 커맨드 구성이 단순해진다.
	 * len은 실제로 확보된 크기로 갱신되어 반환된다. */
	folio = folio_alloc_greedy(GFP_KERNEL, &len);
	if (!folio)
		return -ENOMEM;

	do {
		ssize_t ret;

		ret = iov_iter_extract_bvecs(iter, bio->bi_io_vec + 1, len,
				&bio->bi_vcnt, bio->bi_max_vecs - 1, 0);  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향
		if (ret <= 0) {
			if (!bio->bi_vcnt) {  // NVMe 명령의 PRP entry/SGL segment 개수 집계
				folio_put(folio);
				return ret;
			}
			break;
		}
		len -= ret;
		bio->bi_iter.bi_size += ret;  // NVMe 명령의 NLB(Length)로 변환됨
	} while (len && bio->bi_vcnt < bio->bi_max_vecs - 1);  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향

	/*
	 * Set the folio directly here.  The above loop has already calculated
	 * the correct bi_size, and we use bi_vcnt for the user buffers.  That
	 * is safe as bi_vcnt is only used by the submitter and not the actual
	 * I/O path.
	 */
	bvec_set_folio(&bio->bi_io_vec[0], folio, bio->bi_iter.bi_size, 0);  // folio를 bio_vec에 등록: NVMe PRP/SGL segment 후보
	if (iov_iter_extract_will_pin(iter))  // 사용자 페이지 pin 여부: NVMe DMA 안전성 판단
		bio_set_flag(bio, BIO_PAGE_PINNED);	// 사용자 페이지 pin: NVMe DMA 안전 보장
	return 0;
}

/**
 * bio_iov_iter_bounce - bounce buffer data from an iter into a bio
 * @bio:	bio to send
 * @iter:	iter to read from / write into
 * @maxlen:	maximum size to bounce
 *
 * Helper for direct I/O implementations that need to bounce buffer because
 * we need to checksum the data or perform other operations that require
 * consistency.  Allocates folios to back the bounce buffer, and for writes
 * copies the data into it.  Needs to be paired with bio_iov_iter_unbounce()
 * called on completion.
 */
/*
 * [한국어]
 * bio_iov_iter_bounce - 체크섬/암호화 일관성을 위해 bounce buffer로 데이터를 복사한다
 *
 * @bio:    제출할 bio
 * @iter:   읽거나 쓸 iov_iter (사용자 버퍼 기술)
 * @maxlen: bounce할 최대 바이트 수
 * @return: 0 성공; 음수 에러
 *
 * 쓰기: bio_iov_iter_bounce_write()로 커널 bounce buffer를 할당하고 사용자 데이터를 복사.
 * 읽기: bio_iov_iter_bounce_read()로 bounce buffer를 할당하고 사용자 페이지를 pin.
 * 완료 후 반드시 bio_iov_iter_unbounce()를 호출해야 한다.
 * 실행 컨텍스트: 태스크 컨텍스트 (DIO 제출 경로).
 * 호출자: DIO 구현체 (checksumming, encryption 요구 시)
 *
 * 호출 체인:
 *   DIO 구현체 → [bio_iov_iter_bounce()] → bio_iov_iter_bounce_write/read()
 *
 * NVMe 연결: bounce buffer도 물리 페이지이므로 PRP/SGL로 변환되어 NVMe DMA 전송.
 */
int bio_iov_iter_bounce(struct bio *bio, struct iov_iter *iter, size_t maxlen)
{
	if (op_is_write(bio_op(bio)))                             /* [한국어] 쓰기 방향이면 사용자 버퍼를 bounce buffer로 복사 */
		return bio_iov_iter_bounce_write(bio, iter, maxlen);
	return bio_iov_iter_bounce_read(bio, iter, maxlen);       /* [한국어] 읽기 방향이면 bounce buffer + 사용자 페이지 pin 설정 */
}

/*
 * [한국어]
 * bvec_unpin - bio_vec가 가리키는 folio의 pin을 해제한다
 *
 * @bv:         pin 해제할 bio_vec
 * @mark_dirty: true이면 folio를 dirty로 표시 (읽기 DIO 완료 시)
 *
 * folio 내의 page 수를 계산하여 unpin_user_folio()로 pin을 해제한다.
 * mark_dirty이면 folio_mark_dirty_lock()으로 dirty 표시 후 pin 해제.
 * 실행 컨텍스트: 태스크 컨텍스트 (DIO 완료 경로).
 * 호출자: bio_iov_iter_unbounce_read()
 * NVMe 연결: NVMe 읽기 완료 후 DMA가 끝난 사용자 페이지의 pin을 해제.
 */
static void bvec_unpin(struct bio_vec *bv, bool mark_dirty)
{
	struct folio *folio = page_folio(bv->bv_page);
	size_t nr_pages = (bv->bv_offset + bv->bv_len - 1) / PAGE_SIZE -
			bv->bv_offset / PAGE_SIZE + 1;

	if (mark_dirty)
		folio_mark_dirty_lock(folio);
	unpin_user_folio(folio, nr_pages);
}

/*
 * [한국어]
 * bio_iov_iter_unbounce_read - 읽기 bounce 완료 후 bounce buffer에서 사용자 페이지로 데이터를 복사하고 pin을 해제한다
 *
 * @bio:        완료된 bounce 읽기 bio
 * @is_error:   true이면 I/O 에러가 발생해 데이터 복사 불필요
 * @mark_dirty: true이면 사용자 folio를 dirty 표시
 *
 * bi_io_vec[0]: NVMe DMA가 데이터를 쓴 bounce buffer folio (folio_put으로 해제)
 * bi_io_vec[1+]: 사용자가 제공한 pinned 페이지들 (bvec_unpin으로 pin 해제)
 * 실행 컨텍스트: 태스크 컨텍스트 (DIO 완료 경로).
 * 호출자: bio_iov_iter_unbounce()
 * NVMe 연결: NVMe Read 완료(CQ) 후 bounce buffer → 사용자 메모리로 데이터 복사.
 */
static void bio_iov_iter_unbounce_read(struct bio *bio, bool is_error,
		bool mark_dirty)
{
	unsigned int len = bio->bi_io_vec[0].bv_len;

	if (likely(!is_error)) {
		void *buf = bvec_virt(&bio->bi_io_vec[0]);
		struct iov_iter to;

		iov_iter_bvec(&to, ITER_DEST, bio->bi_io_vec + 1, bio->bi_vcnt,  // NVMe 명령의 PRP entry/SGL segment 개수 집계
				len);
		/* copying to pinned pages should always work */
		WARN_ON_ONCE(copy_to_iter(buf, len, &to) != len);  // bounce buffer -> 사용자 버퍼 복사: NVMe read 완료 후 데이터 반환
	} else {
		/* No need to mark folios dirty if never copied to them */
		mark_dirty = false;
	}

	if (bio_flagged(bio, BIO_PAGE_PINNED)) {  // 사용자 페이지 pin 상태: NVMe DMA 엔진이 페이지를 안전하게 접근
		int i;

		for (i = 0; i < bio->bi_vcnt; i++)  // NVMe 명령의 PRP entry/SGL segment 개수 집계
			bvec_unpin(&bio->bi_io_vec[1 + i], mark_dirty);  // pin 해제: NVMe DMA 완료 후 사용자 페이지 참조 해제
	}

	folio_put(page_folio(bio->bi_io_vec[0].bv_page));
}

/**
 * bio_iov_iter_unbounce - finish a bounce buffer operation
 * @bio:	completed bio
 * @is_error:	%true if an I/O error occurred and data should not be copied
 * @mark_dirty:	If %true, folios will be marked dirty.
 *
 * Helper for direct I/O implementations that need to bounce buffer because
 * we need to checksum the data or perform other operations that require
 * consistency.  Called to complete a bio set up by bio_iov_iter_bounce().
 * Copies data back for reads, and marks the original folios dirty if
 * requested and then frees the bounce buffer.
 */
/*
 * [한국어]
 * bio_iov_iter_unbounce - bounce buffer I/O의 뒷정리(읽기면 되복사, 쓰기면 해제)
 *
 * @bio:        완료된 bounce bio (bi_io_vec에 바운스 folio들이 들어 있다)
 * @is_error:   I/O가 실패했는가. true면 바운스 버퍼의 내용이 신뢰할 수 없으므로
 *              사용자 버퍼로 되복사하지 않는다.
 * @mark_dirty: 되복사한 사용자 folio를 dirty로 표시할지 여부.
 * @return: 없음
 *
 * === bounce buffer가 필요한 이유 ===
 * O_DIRECT는 원래 사용자 버퍼를 그대로 DMA 대상으로 삼는다. 그런데 커널이
 * 데이터를 "읽어야만" 하는 경우가 있다 — 체크섬 계산(무결성 검증)이나 압축,
 * 암호화 같은 변환이다. 사용자 버퍼는 DMA가 진행되는 동안에도 다른 스레드가
 * 자유롭게 수정할 수 있어(TOCTOU), 커널이 계산한 체크섬과 실제로 장치에
 * 기록된 내용이 달라질 수 있다.
 * 그래서 커널 소유의 임시 버퍼로 한 번 복사한 뒤 그것을 DMA 대상으로 삼는다.
 * 이것이 bounce buffer이고, 이 함수는 그 뒷정리를 담당한다.
 *
 * === 방향에 따라 할 일이 다른 이유 ===
 *   쓰기(WRITE): 사용자 → 바운스 복사는 제출 전에 이미 끝났다. 완료 후에는
 *     바운스 folio를 해제하기만 하면 된다.
 *   읽기(READ) : 장치 → 바운스로 데이터가 들어왔으므로, 이제 바운스 →
 *     사용자 버퍼로 되복사해야 한다. 그 후 folio 해제와 dirty 표시까지
 *     처리해야 하므로 별도 함수로 분리되어 있다.
 *
 * NVMe 관점: 바운스 버퍼는 folio_alloc_greedy()로 최대한 큰 연속 영역을 잡으므로
 * 세그먼트 수가 줄어 PRP/SGL 디스크립터가 단순해진다. 사용자 버퍼가 잘게
 * 흩어져 있을 때는 오히려 바운스 쪽이 커맨드 구성에 유리할 수도 있다.
 *
 * 실행 컨텍스트: 태스크 컨텍스트. 되복사에 페이지 매핑이 필요할 수 있어
 * IRQ 컨텍스트에서 호출해서는 안 된다.
 *
 * 호출 체인:
 *   direct I/O 구현(fs/direct-io.c, iomap) 완료 처리
 *     → [bio_iov_iter_unbounce]
 *       → bio_free_folios (쓰기) / bio_iov_iter_unbounce_read (읽기)
 */
void bio_iov_iter_unbounce(struct bio *bio, bool is_error, bool mark_dirty)
{
	/* [한국어] 쓰기였다면 데이터는 이미 장치로 나갔다. 바운스 folio들을
	 * 해제하는 것으로 끝난다. is_error와 mark_dirty는 쓰기에서 의미가 없어
	 * 사용되지 않는다(실패해도 해제는 똑같이 해야 하고, 커널 소유 버퍼라
	 * dirty 표시 대상이 아니다). */
	if (op_is_write(bio_op(bio)))
		bio_free_folios(bio);
	else
		/* [한국어] 읽기였다면 장치가 바운스 버퍼에 채워 준 데이터를 사용자
		 * folio로 되복사해야 한다. is_error가 true면 복사를 건너뛰고
		 * (쓰레기 데이터를 사용자에게 주면 안 된다) 해제만 하며,
		 * mark_dirty가 true면 되복사한 사용자 folio를 dirty로 표시해
		 * 이후 페이지 회수 시 잃어버리지 않게 한다. */
		bio_iov_iter_unbounce_read(bio, is_error, mark_dirty);
}

/*
 * [한국어]
 * bio_wait_end_io - bio_await()의 내부 완료 핸들러: completion으로 대기자를 깨운다
 *
 * @bio: 완료된 bio (bi_private에 completion 구조체가 저장됨)
 *
 * bi_private에 저장된 completion을 complete()로 신호 보내어 bio_await()을 깨운다.
 * 실행 컨텍스트: 하드 IRQ 컨텍스트 포함 (NVMe CQ 완료 처리기에서 호출).
 * 호출자: bio_endio() (bio_await()이 bi_end_io로 설정)
 */
static void bio_wait_end_io(struct bio *bio)
{
	complete(bio->bi_private); /* [한국어] bi_private에 저장된 completion 구조체를 완료 신호로 깨움 */
}

/**
 * bio_await - call a function on a bio, and wait until it completes
 * @bio:	the bio which describes the I/O
 * @submit:	function called to submit the bio
 * @priv:	private data passed to @submit
 *
 * Wait for the bio as well as any bio chained off it after executing the
 * passed in callback @submit.  The wait for the bio is set up before calling
 * @submit to ensure that the completion is captured.  If @submit is %NULL,
 * submit_bio() is used instead to submit the bio.
 *
 * Note: this overrides the bi_private and bi_end_io fields in the bio.
 */

/*
 * [한국어]
 * bio_await - bio를 제출하고 완료될 때까지 동기적으로 대기한다
 *
 * @bio:    제출하고 대기할 bio
 * @priv:   submit 콜백에 전달할 private 데이터
 * @submit: bio 제출 콜백; NULL이면 submit_bio() 사용
 *
 * DECLARE_COMPLETION_ONSTACK_MAP으로 스택에 completion을 생성한다.
 * bi_private에 completion 주소, bi_end_io에 bio_wait_end_io를 설정한다.
 * REQ_SYNC를 설정하여 NVMe 컨트롤러에게 동기 I/O 힌트를 전달한다.
 * bio_endio() → bio_wait_end_io() → complete() 순으로 대기가 풀린다.
 * 실행 컨텍스트: 태스크 컨텍스트 (블록 대기 가능).
 * 호출자: submit_bio_wait(), bio_submit_or_kill()
 * NVMe 연결: submit_bio() → blk_mq_submit_bio() → nvme_queue_rq() →
 *            nvme_submit_cmd(doorbell) 후 CQ 수신까지 blk_wait_io()로 블록.
 *
 * 호출 체인:
 *   submit_bio_wait() → [bio_await()] → submit_bio() → (NVMe CQ) → bio_wait_end_io() → blk_wait_io() 반환
 */
void bio_await(struct bio *bio, void *priv,  // bio 제출 후 완료 대기: submit_bio -> blk-mq -> nvme_queue_rq -> doorbell -> CQ
	       void (*submit)(struct bio *bio, void *priv))
{
	DECLARE_COMPLETION_ONSTACK_MAP(done,
			bio->bi_bdev->bd_disk->lockdep_map);  // NVMe namespace/block device 선택

	bio->bi_private = &done;  // NVMe 완료 콜백용 private 데이터(예: completion 구조체)
	bio->bi_end_io = bio_wait_end_io;  // NVMe sync/admin 명령 완료 시 대기자 깨움
	bio->bi_opf |= REQ_SYNC;	// NVMe polling/sync 완료 우선순위 힌트
	if (submit)
		submit(bio, priv);
	else
		submit_bio(bio);  // bio -> block 레이어 -> blk-mq -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
	blk_wait_io(&done);  // NVMe CQ 수신까지 동기 대기
}
EXPORT_SYMBOL_GPL(bio_await);  // bio 제출 후 완료 대기: submit_bio -> blk-mq -> nvme_queue_rq -> doorbell -> CQ

/**
 * submit_bio_wait - submit a bio, and wait until it completes
 * @bio: The &struct bio which describes the I/O
 *
 * Simple wrapper around submit_bio(). Returns 0 on success, or the error from
 * bio_endio() on failure.
 *
 * WARNING: Unlike to how submit_bio() is usually used, this function does not
 * result in bio reference to be consumed. The caller must drop the reference
 * on his own.
 */
/*
 * [한국어]
 * submit_bio_wait - bio를 제출하고 완료를 동기적으로 기다린다
 *
 * @bio: 제출하고 대기할 bio
 * @return: 0 성공; 음수 errno (bi_status → errno 변환)
 *
 * bio_await()의 간단한 래퍼. bio 참조를 소비하지 않으므로 호출자가 직접 참조를 해제해야 한다.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: bdev_rw_virt(), bio_submit_or_kill(), 파티션 스캔 등
 * NVMe 연결: NVMe admin 명령이나 sync I/O에서 CQ 수신까지 블록.
 *
 * 호출 체인:
 *   bdev_rw_virt() → [submit_bio_wait()] → bio_await() → submit_bio() → NVMe CQ
 */
int submit_bio_wait(struct bio *bio)  // NVMe admin/sync 명령: SQ 제출 후 CQ 수신까지 동기 대기
{
	bio_await(bio, NULL, NULL);  // bio 제출 후 완료 대기: submit_bio -> blk-mq -> nvme_queue_rq -> doorbell -> CQ
	return blk_status_to_errno(bio->bi_status);  // NVMe CQ status -> request status -> bio status 전파 경로
}
EXPORT_SYMBOL(submit_bio_wait);  // NVMe admin/sync 명령: SQ 제출 후 CQ 수신까지 동기 대기

/*
 * [한국어]
 * bio_endio_cb - bio_await()의 submit 콜백으로 bio_endio()를 직접 호출한다
 *
 * @bio:  완료할 bio
 * @priv: 사용되지 않음
 *
 * 프로세스가 kill되어 NVMe 명령을 실제로 제출하지 않아도 bio_endio()를 실행해
 * 완료 콜백이 정상적으로 실행되도록 한다.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: bio_submit_or_kill() (BLKDEV_ZERO_KILLABLE + 시그널 대기 중)
 */
static void bio_endio_cb(struct bio *bio, void *priv)
{
	bio_endio(bio); /* [한국어] kill 시그널 수신 시 실제 제출 대신 bio_endio()로 즉시 완료 처리 */
}

/*
 * Submit @bio synchronously, or call bio_endio on it if the current process
 * is being killed.
 */
/*
 * [한국어]
 * bio_submit_or_kill - bio를 동기 제출하거나, 프로세스가 kill 중이면 bio_endio()로 완료한다
 *
 * @bio:   제출할 bio
 * @flags: BLKDEV_ZERO_KILLABLE 포함 시 kill 대기 중 즉시 완료 가능
 * @return: 0 성공; -EINTR (kill 시그널로 실제 I/O 미수행)
 *
 * BLKDEV_ZERO_KILLABLE 설정 시 fatal_signal_pending()을 확인하여
 * kill 중이면 NVMe SQ에 제출하지 않고 bio_endio_cb()로 즉시 완료 처리한다.
 * 그 외에는 submit_bio_wait()으로 정상 동기 I/O를 수행한다.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: blkdev_issue_zeroout() 등
 * NVMe 연결: BLKDEV_ZERO_KILLABLE 없으면 NVMe SQ 제출 후 CQ 수신까지 블록.
 */
int bio_submit_or_kill(struct bio *bio, unsigned int flags)
{
	if ((flags & BLKDEV_ZERO_KILLABLE) && fatal_signal_pending(current)) { /* [한국어] kill 시그널이 있으면 NVMe 명령 제출 없이 즉시 완료 */
		bio_await(bio, NULL, bio_endio_cb); /* [한국어] submit 대신 bio_endio_cb로 즉시 완료; blk_wait_io()로 대기 후 반환 */
		return -EINTR; /* [한국어] kill 시그널로 중단됨을 호출자에게 통보 */
	}

	return submit_bio_wait(bio);  // NVMe admin/sync 명령: SQ 제출 후 CQ 수신까지 동기 대기
}

/**
 * bdev_rw_virt - synchronously read into / write from kernel mapping
 * @bdev:	block device to access
 * @sector:	sector to access
 * @data:	data to read/write
 * @len:	length in byte to read/write
 * @op:		operation (e.g. REQ_OP_READ/REQ_OP_WRITE)
 *
 * Performs synchronous I/O to @bdev for @data/@len.  @data must be in
 * the kernel direct mapping and not a vmalloc address.
 */
/*
 * [한국어]
 * bdev_rw_virt - 커널 직접 매핑 주소에서 블록 장치로 동기 읽기/쓰기를 수행한다
 *
 * @bdev:   접근할 블록 장치 (NVMe namespace)
 * @sector: 시작 sector 번호 → NVMe SLBA
 * @data:   읽기/쓰기할 커널 직접 매핑 주소 (vmalloc 불가)
 * @len:    전송 바이트 수 → NVMe NLB
 * @op:     REQ_OP_READ 또는 REQ_OP_WRITE
 * @return: 0 성공; 음수 errno
 *
 * 스택 상의 bio와 bio_vec를 사용하여 단일 I/O를 동기 수행한다 (mempool 미사용).
 * 파티션 스캔, 메타데이터 읽기 등 단순 동기 I/O에 적합하다.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: 파티션 스캔, 블록 장치 메타데이터 읽기 등
 * NVMe 연결: bio_init() → bio_add_virt_nofail() → submit_bio_wait() →
 *            nvme_queue_rq() → doorbell → CQ 수신.
 *
 * 호출 체인:
 *   파티션 스캔 → [bdev_rw_virt()] → bio_init() + submit_bio_wait()
 */
int bdev_rw_virt(struct block_device *bdev, sector_t sector, void *data,
		size_t len, enum req_op op)
{
	struct bio_vec bv;
	struct bio bio;
	int error;

	if (WARN_ON_ONCE(is_vmalloc_addr(data)))
		return -EIO;  // I/O 오류: NVMe 명령 제출/완료 실패 전파

	bio_init(&bio, bdev, &bv, 1, op);  // bio 필드 초기화: SLBA/length/OPC/PRP-SGL 기반 준비
	bio.bi_iter.bi_sector = sector;  // NVMe Read/Write 명령의 SLBA(Starting LBA)로 변환됨
	bio_add_virt_nofail(&bio, data, len);  // 커널 가상 주소를 bio에 추가: NVMe PRP/SGL로 변환
	error = submit_bio_wait(&bio);  // NVMe admin/sync 명령: SQ 제출 후 CQ 수신까지 동기 대기
	bio_uninit(&bio);  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제
	return error;
}
EXPORT_SYMBOL_GPL(bdev_rw_virt);

/*
 * [한국어]
 * __bio_advance - bio 반복자를 bytes만큼 앞으로 이동시킨다
 *
 * @bio:   advance할 bio
 * @bytes: 전진할 바이트 수
 *
 * integrity context와 crypto context를 먼저 advance한 뒤 bio_iter를 전진시킨다.
 * 이 순서가 보장되어야 PI/암호화 상태와 데이터 이터레이터가 항상 일치한다.
 * 실행 컨텍스트: 태스크 컨텍스트 (bio_split, bio_trim 등에서 사용).
 * 호출자: bio_advance() 매크로, bio_split(), bio_trim()
 * NVMe 연결: bi_sector·bi_size 갱신 → SLBA·NLB 재계산에 반영됨.
 *
 * 호출 체인:
 *   bio_split() / bio_trim() → [__bio_advance()] → bio_advance_iter()
 */
void __bio_advance(struct bio *bio, unsigned bytes)
{
	if (bio_integrity(bio))	// NVMe PI/DIF 보호 정보 해제
		bio_integrity_advance(bio, bytes);

	bio_crypt_advance(bio, bytes);  // NVMe inline crypto/Opal: PRP/SGL 데이터와 암호화 컨텍스트가 nvme_queue_rq에서 연결됨
	bio_advance_iter(bio, &bio->bi_iter, bytes);	// NVMe partial completion 시 LBA/length 갱신
}
EXPORT_SYMBOL(__bio_advance);

/*
 * [한국어]
 * bio_copy_data_iter - src bio의 src_iter 위치부터 dst bio의 dst_iter 위치로 데이터를 복사한다
 *
 * @dst:      복사 목적지 bio
 * @dst_iter: 목적지 반복자 (업데이트됨)
 * @src:      복사 원본 bio
 * @src_iter: 원본 반복자 (업데이트됨)
 *
 * bvec_kmap_local()로 각 페이지를 임시 매핑한 뒤 memcpy()로 복사한다.
 * src 또는 dst 중 하나가 끝나면 중단한다.
 * 실행 컨텍스트: 태스크 컨텍스트 (highmem 접근 필요 가능).
 * 호출자: bio_copy_data()
 * NVMe 연결: NVMe DIO 완료 후 bounce buffer → 사용자 페이지 복사에 사용.
 *
 * 호출 체인:
 *   bio_copy_data() → [bio_copy_data_iter()] → bvec_kmap_local() + memcpy()
 */
void bio_copy_data_iter(struct bio *dst, struct bvec_iter *dst_iter,
			struct bio *src, struct bvec_iter *src_iter)
{
	while (src_iter->bi_size && dst_iter->bi_size) { /* [한국어] src와 dst 모두 데이터가 남아 있는 동안 반복 */
		struct bio_vec src_bv = bio_iter_iovec(src, *src_iter); /* [한국어] src 현재 위치의 bvec 획득 */
		struct bio_vec dst_bv = bio_iter_iovec(dst, *dst_iter); /* [한국어] dst 현재 위치의 bvec 획득 */
		unsigned int bytes = min(src_bv.bv_len, dst_bv.bv_len); /* [한국어] 한 번에 복사할 바이트: 두 bvec 중 작은 쪽 */
		void *src_buf = bvec_kmap_local(&src_bv); /* [한국어] src 페이지를 로컬(highmem 포함) 임시 매핑 */
		void *dst_buf = bvec_kmap_local(&dst_bv); /* [한국어] dst 페이지를 로컬 임시 매핑 */

		memcpy(dst_buf, src_buf, bytes); /* [한국어] 실제 데이터 복사: bounce buffer → 사용자 페이지 또는 반대 방향 */

		kunmap_local(dst_buf); /* [한국어] dst 임시 매핑 해제 (역순으로 unmap) */
		kunmap_local(src_buf); /* [한국어] src 임시 매핑 해제 */

		bio_advance_iter_single(src, src_iter, bytes); /* [한국어] src 반복자를 bytes만큼 전진 */
		bio_advance_iter_single(dst, dst_iter, bytes); /* [한국어] dst 반복자를 bytes만큼 전진 */
	}
}
EXPORT_SYMBOL(bio_copy_data_iter);

/**
 * bio_copy_data - copy contents of data buffers from one bio to another
 * @src: source bio
 * @dst: destination bio
 *
 * Stops when it reaches the end of either @src or @dst - that is, copies
 * min(src->bi_size, dst->bi_size) bytes (or the equivalent for lists of bios).
 */
/*
 * [한국어]
 * bio_copy_data - 두 bio 사이에 데이터를 복사한다 (처음부터 min(src_size, dst_size)까지)
 *
 * @dst: 복사 목적지 bio
 * @src: 복사 원본 bio
 *
 * src와 dst 각각의 bi_iter를 로컬 복사하여 bio_copy_data_iter()에 전달한다.
 * 두 반복자 중 하나가 소진되면 복사를 중단한다.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: DIO bounce buffer 처리, RAID/DM 미러링, NVMe 클론 경로
 * NVMe 연결: 같은 데이터를 두 bio(쓰기/미러)에 동기화하거나,
 *            bounce buffer에서 사용자 공간으로 데이터를 복귀시킬 때 사용.
 *
 * 호출 체인:
 *   DIO/RAID path → [bio_copy_data()] → bio_copy_data_iter() → bvec_kmap_local() + memcpy()
 */
void bio_copy_data(struct bio *dst, struct bio *src)  // bio 간 데이터 복사: NVMe clone/raid/duplicate 경로
{
	struct bvec_iter src_iter = src->bi_iter;
	struct bvec_iter dst_iter = dst->bi_iter;

	bio_copy_data_iter(dst, &dst_iter, src, &src_iter);
}
EXPORT_SYMBOL(bio_copy_data);  // bio 간 데이터 복사: NVMe clone/raid/duplicate 경로

/*
 * [한국어]
 * bio_free_pages - bio의 bi_io_vec에 들어 있는 모든 페이지를 해제한다
 *
 * @bio: 페이지를 해제할 bio
 *
 * bio_for_each_segment_all()로 모든 bvec를 순회하며 __free_page()를 호출한다.
 * NVMe 완료 후 커널이 직접 할당한 bounce 페이지나 임시 페이지를 정리할 때 사용한다.
 * 사용자 페이지(get_user_pages로 pin된 경우)에는 사용하지 않는다 — unpin_user_folio()를 써야 함.
 * 실행 컨텍스트: 태스크 컨텍스트 (I/O 완료 경로).
 * 호출자: bio 생성자가 alloc_page()로 직접 할당한 후 완료 시 정리
 * NVMe 연결: 커널이 bounce buffer로 쓴 페이지를 NVMe CQ 완료 후 반환.
 *
 * 호출 체인:
 *   I/O 완료 경로 → [bio_free_pages()] → bio_for_each_segment_all() → __free_page()
 */
void bio_free_pages(struct bio *bio)  // bio에 할당된 페이지 해제: NVMe 완료/abort 시 정리
{
	struct bio_vec *bvec;
	struct bvec_iter_all iter_all;

	bio_for_each_segment_all(bvec, bio, iter_all)  // bio_vec(bvec) 순회: NVMe PRP entry/SGL segment를 구성하는 단위
		__free_page(bvec->bv_page);
}
EXPORT_SYMBOL(bio_free_pages);  // bio에 할당된 페이지 해제: NVMe 완료/abort 시 정리

/*
 * bio_set_pages_dirty() and bio_check_pages_dirty() are support functions
 * for performing direct-IO in BIOs.
 *
 * The problem is that we cannot run folio_mark_dirty() from interrupt context
 * because the required locks are not interrupt-safe.  So what we can do is to
 * mark the pages dirty _before_ performing IO.  And in interrupt context,
 * check that the pages are still dirty.   If so, fine.  If not, redirty them
 * in process context.
 *
 * Note that this code is very hard to test under normal circumstances because
 * direct-io pins the pages with get_user_pages().  This makes
 * is_page_cache_freeable return false, and the VM will not clean the pages.
 * But other code (eg, flusher threads) could clean the pages if they are mapped
 * pagecache.
 *
 * Simply disabling the call to bio_set_pages_dirty() is a good way to test the
 * deferred bio dirtying paths.
 */

/*
 * bio_set_pages_dirty() will mark all the bio's pages as dirty.
 */
/*
 * [한국어]
 * bio_set_pages_dirty - bio의 모든 folio를 dirty로 마킹한다
 *
 * @bio: 마킹할 bio
 *
 * folio_lock() → folio_mark_dirty() → folio_unlock() 순서로 각 folio를 dirty 처리한다.
 * DIO(직접 I/O) 읽기 경로에서, 인터럽트 컨텍스트의 NVMe CQ 완료 핸들러가
 * folio_mark_dirty()를 안전하게 호출할 수 없으므로,
 * I/O 시작 전(태스크 컨텍스트)에 미리 dirty를 마킹해두는 전략.
 * 실행 컨텍스트: 태스크 컨텍스트 (I/O 제출 전 경로).
 * 호출자: DIO read 경로 — bio를 제출하기 직전에 호출
 * NVMe 연결: NVMe DMA 읽기가 완료되면 페이지가 dirty 상태가 되어야
 *            페이지 캐시가 올바르게 갱신됨.
 *
 * 호출 체인:
 *   DIO read → [bio_set_pages_dirty()] → folio_lock() + folio_mark_dirty()
 */
void bio_set_pages_dirty(struct bio *bio)  // DIO 완료 전 페이지 dirty 마킹: NVMe flush/cache 일관성 힌트
{
	struct folio_iter fi;

	bio_for_each_folio_all(fi, bio) {  // bio_vec(bvec) 순회: NVMe PRP entry/SGL segment를 구성하는 단위
		folio_lock(fi.folio);
		folio_mark_dirty(fi.folio);
		folio_unlock(fi.folio);
	}
}
EXPORT_SYMBOL_GPL(bio_set_pages_dirty);  // DIO 완료 전 페이지 dirty 마킹: NVMe flush/cache 일관성 힌트

/*
 * bio_check_pages_dirty() will check that all the BIO's pages are still dirty.
 * If they are, then fine.  If, however, some pages are clean then they must
 * have been written out during the direct-IO read.  So we take another ref on
 * the BIO and re-dirty the pages in process context.
 *
 * It is expected that bio_check_pages_dirty() will wholly own the BIO from
 * here on.  It will unpin each page and will run one bio_put() against the
 * BIO.
 */

static void bio_dirty_fn(struct work_struct *work);

static DECLARE_WORK(bio_dirty_work, bio_dirty_fn);    /* [한국어] bio dirty 재마킹 지연 작업: 인터럽트 컨텍스트에서 직접 호출 불가하므로 워크큐로 지연 */
static DEFINE_SPINLOCK(bio_dirty_lock);                /* [한국어] bio_dirty_list 보호 스핀락: NVMe CQ 인터럽트와 워크큐 사이 경쟁 방지 */
static struct bio *bio_dirty_list;                     /* [한국어] 재마킹이 필요한 bio 단방향 링크드 리스트 (bi_private 링크 사용) */

/*
 * This runs in process context
 */
/*
 * [한국어]
 * bio_dirty_fn - bio_dirty_list에 쌓인 bio들의 페이지를 프로세스 컨텍스트에서 dirty 마킹한다
 *
 * @work: 사용하지 않음 (struct work_struct 시그니처용)
 *
 * bio_check_pages_dirty()가 NVMe CQ 완료 후 인터럽트 컨텍스트에서 dirty가 해제된 페이지를
 * 발견하면 bio를 bio_dirty_list에 넣고 이 워크를 스케줄링한다.
 * 이 함수는 프로세스 컨텍스트에서 실행되어 folio_mark_dirty()를 안전하게 호출할 수 있다.
 * spin_lock_irq()로 bio_dirty_list를 통째로 빼낸 뒤, 순서대로 dirty 처리 후 bio_put()으로 해제.
 * 실행 컨텍스트: 워크큐 (프로세스 컨텍스트).
 * 호출자: bio_check_pages_dirty() → schedule_work(&bio_dirty_work)
 * NVMe 연결: NVMe DMA 읽기로 인해 dirty 상태가 변경된 페이지를 뒤늦게 재마킹.
 *
 * 호출 체인:
 *   bio_check_pages_dirty() → schedule_work() → [bio_dirty_fn()] → bio_release_pages() + bio_put()
 */
static void bio_dirty_fn(struct work_struct *work)
{
	struct bio *bio, *next;

	spin_lock_irq(&bio_dirty_lock);         /* [한국어] bio_dirty_list 접근 직전 락 획득 + IRQ 비활성화 */
	next = bio_dirty_list;                  /* [한국어] 전체 리스트를 로컬로 가져옴 */
	bio_dirty_list = NULL;                  /* [한국어] 전역 리스트를 비워서 새 항목이 별도 처리되도록 */
	spin_unlock_irq(&bio_dirty_lock);       /* [한국어] 락 해제 + IRQ 재활성화 */

	while ((bio = next) != NULL) {
		next = bio->bi_private;  /* [한국어] 다음 bio를 미리 저장: bi_private가 링크드 리스트 포인터로 사용됨 */

		bio_release_pages(bio, true);  /* [한국어] dirty=true: 각 folio를 dirty 마킹 후 unpin (읽기 DMA 완료 후 상태 반영) */
		bio_put(bio);  /* [한국어] bio 참조 해제: bio_check_pages_dirty()에서 bio_get()된 참조 반환 */
	}
}

/*
 * [한국어]
 * bio_check_pages_dirty - NVMe DIO 읽기 완료 후 페이지 dirty 상태를 검증하고, 필요 시 재마킹을 워크큐로 지연한다
 *
 * @bio: 완료된 DIO 읽기 bio; 이 함수 호출 후 bio의 소유권이 이전됨
 *
 * 동작 흐름:
 *   1. 모든 folio가 dirty이면 → bio_release_pages(false) + bio_put()으로 즉시 정리.
 *   2. 하나라도 clean이면 (VM이 flush): bio를 bio_dirty_list에 연결하고
 *      schedule_work()로 bio_dirty_fn()을 예약.
 *   프로세스 컨텍스트의 bio_dirty_fn()이 re-dirty 및 unpin을 완료한다.
 * 실행 컨텍스트: 인터럽트 컨텍스트 가능 (NVMe CQ 완료 경로에서 직접 호출 가능).
 *   → folio_mark_dirty()가 인터럽트 안전하지 않으므로 dirty 검사만 하고 재마킹은 워크큐로.
 * 호출자: DIO 읽기 bio의 bi_end_io 콜백 또는 bio_endio() 경로
 * NVMe 연결: NVMe DMA 읽기 완료 후 페이지 캐시 일관성을 유지하는 메커니즘.
 *
 * 호출 체인:
 *   bio_endio() → bi_end_io → [bio_check_pages_dirty()] → bio_dirty_fn(워크큐)
 */
void bio_check_pages_dirty(struct bio *bio)  // NVMe DIO 완료 후 페이지 dirty 상태 검증/재마킹
{
	struct folio_iter fi;
	unsigned long flags;

	bio_for_each_folio_all(fi, bio) {  /* [한국어] 모든 folio를 순회하며 dirty 여부 확인 */
		if (!folio_test_dirty(fi.folio))   /* [한국어] 하나라도 clean이면 defer 경로로 이동 */
			goto defer;
	}

	bio_release_pages(bio, false);  /* [한국어] 모두 dirty: mark_dirty=false로 unpin만 수행 (이미 dirty) */
	bio_put(bio);  /* [한국어] 이 함수가 bio 소유권을 가졌으므로 bio 참조 해제 */
	return;
defer:
	spin_lock_irqsave(&bio_dirty_lock, flags);  /* [한국어] bio_dirty_list 보호 + IRQ 저장 (인터럽트 컨텍스트 안전) */
	bio->bi_private = bio_dirty_list;  /* [한국어] bio_dirty_list의 헤드에 삽입: bi_private를 링크 포인터로 활용 */
	bio_dirty_list = bio;              /* [한국어] 새 헤드로 설정 */
	spin_unlock_irqrestore(&bio_dirty_lock, flags);  /* [한국어] 락 해제 + IRQ 복원 */
	schedule_work(&bio_dirty_work);    /* [한국어] 프로세스 컨텍스트에서 dirty 재마킹을 위한 워크큐 예약 */
}
EXPORT_SYMBOL_GPL(bio_check_pages_dirty);  // NVMe DIO 완료 후 페이지 dirty 상태 검증/재마킹

/*
 * [한국어]
 * bio_remaining_done - bio_chain으로 묶인 모든 하위 bio가 완료되었는지 확인한다
 *
 * @bio: 완료 여부를 확인할 bio
 * @return: true이면 모든 하위 bio 완료 (bi_end_io 호출 가능); false이면 아직 대기 중
 *
 * BIO_CHAIN 플래그가 없으면 단일 bio이므로 항상 true.
 * BIO_CHAIN이 설정된 경우 __bi_remaining를 1 감소; 0이 되면 체인 완료.
 * 실행 컨텍스트: 하드 IRQ 컨텍스트 포함 (atomic_dec_and_test 사용).
 * 호출자: bio_endio()
 * NVMe 연결: 여러 CID로 분할된 NVMe 명령이 모두 CQ를 통해 완료되어야
 *            __bi_remaining가 0이 되고 상위 파일 시스템 완료 콜백이 실행됨.
 *
 * 호출 체인:
 *   bio_endio() → [bio_remaining_done()] → (true면) bi_end_io() 호출
 */
static inline bool bio_remaining_done(struct bio *bio)  // 모든 NVMe 하위 명령이 CQ를 통해 완료되었는지 판정
{
	/*
	 * If we're not chaining, then ->__bi_remaining is always 1 and
	 * we always end io on the first invocation.
	 */
	if (!bio_flagged(bio, BIO_CHAIN))
		return true;

	BUG_ON(atomic_read(&bio->__bi_remaining) <= 0);  // NVMe bio 참조/remaining 상태 확인

	if (atomic_dec_and_test(&bio->__bi_remaining)) {  // NVMe completion 순서 보장: 마지막 하위 bio 완료 시 부모로 전파
		bio_clear_flag(bio, BIO_CHAIN);
		return true;
	}

	return false;
}

/**
 * bio_endio - end I/O on a bio
 * @bio:	bio
 *
 * Description:
 *   bio_endio() will end I/O on the whole bio. bio_endio() is the preferred
 *   way to end I/O on a bio. No one should call bi_end_io() directly on a
 *   bio unless they own it and thus know that it has an end_io function.
 *
 *   bio_endio() can be called several times on a bio that has been chained
 *   using bio_chain().  The ->bi_end_io() function will only be called the
 *   last time.
 **/
/*
 * [한국어]
 * bio_endio - bio의 I/O를 종료하고 상위 레이어 완료 콜백을 호출한다
 *
 * @bio: 완료된 bio
 *
 * 호출 순서:
 *   1. bio_remaining_done(): 체인된 하위 bio가 아직 대기 중이면 즉시 반환.
 *   2. bio_integrity_endio(): T10-PI 검증; 실패 시 bi_status 설정 후 반환.
 *   3. blk_zone_bio_endio(): ZNS zone 상태 업데이트.
 *   4. rq_qos_done_bio(): I/O cost/latency QoS 통계 기록.
 *   5. tracepoint: BIO_TRACE_COMPLETION 시 ftrace 이벤트 발행.
 *   6. bi_end_io == bio_chain_endio이면 goto again으로 스택 오버플로 방지 처리.
 *   7. 최종 bi_end_io() 호출.
 * 실행 컨텍스트: 하드 IRQ 컨텍스트 포함 (NVMe CQ 처리기에서 직접 호출 가능).
 * 호출자: blk_mq_end_request() (NVMe 완료 경로)
 * NVMe 연결: nvme_process_cq() → nvme_complete_rq() → blk_mq_end_request() → [bio_endio()].
 *
 * 호출 체인:
 *   nvme_process_cq() → blk_mq_end_request() → [bio_endio()] → bi_end_io()
 */
void bio_endio(struct bio *bio)  // NVMe CQ 수신 후 상위 레이어로 completion 전파
{
again:
	if (!bio_remaining_done(bio))	// bio_chain으로 묶인 NVMe 분할 명령, 아직 부모 완료 불가
		return;
	if (!bio_integrity_endio(bio))	// NVMe PI/DIF 검증 실패 시 상위로 전파
		return;

	blk_zone_bio_endio(bio);	// NVMe ZNS zone 상태 갱신

	rq_qos_done_bio(bio);  // NVMe QoS(latency/iocost) 완료 기록

	if (bio->bi_bdev && bio_flagged(bio, BIO_TRACE_COMPLETION)) {  // NVMe namespace/block device 선택
		trace_block_bio_complete(bdev_get_queue(bio->bi_bdev), bio);	// NVMe 완료 추적(tracepoint)
		bio_clear_flag(bio, BIO_TRACE_COMPLETION);
	}

	/*
	 * Need to have a real endio function for chained bios, otherwise
	 * various corner cases will break (like stacking block devices that
	 * save/restore bi_end_io) - however, we want to avoid unbounded
	 * recursion and blowing the stack. Tail call optimization would
	 * handle this, but compiling with frame pointers also disables
	 * gcc's sibling call optimization.
	 */
	if (bio->bi_end_io == bio_chain_endio) {	// NVMe 분할 bio의 chained completion 처리
		bio = __bio_chain_endio(bio);
		goto again;  // chained bio completion 반복: NVMe 분할 명령 모두 소진
	}

#ifdef CONFIG_BLK_CGROUP
	/*
	 * Release cgroup info.  We shouldn't have to do this here, but quite
	 * a few callers of bio_init fail to call bio_uninit, so we cover up
	 * for that here at least for now.
	 */
	if (bio->bi_blkg) {
		blkg_put(bio->bi_blkg);  // cgroup 참조 해제: NVMe cgroup 기반 queue 제한과 연결
		bio->bi_blkg = NULL;
	}
#endif

	if (bio->bi_end_io)  // NVMe CQ 처리기와 연결된 상위 완료 콜백
		bio->bi_end_io(bio);  // NVMe CQ 처리기와 연결된 상위 완료 콜백
}
EXPORT_SYMBOL(bio_endio);  // NVMe CQ 수신 후 상위 레이어로 completion 전파

/**
 * bio_split - split a bio
 * @bio:	bio to split
 * @sectors:	number of sectors to split from the front of @bio
 * @gfp:	gfp mask
 * @bs:		bio set to allocate from
 *
 * Allocates and returns a new bio which represents @sectors from the start of
 * @bio, and updates @bio to represent the remaining sectors.
 *
 * Unless this is a discard request the newly allocated bio will point
 * to @bio's bi_io_vec. It is the caller's responsibility to ensure that
 * neither @bio nor @bs are freed before the split bio.
 */
/*
 * [한국어]
 * bio_split - bio의 앞부분 sectors만큼을 새 bio로 분리한다
 *
 * @bio:     분할할 원본 bio; 함수 반환 후 나머지 sectors만 남음
 * @sectors: 새 bio에 넣을 sector 수 (1 이상, bio_sectors(bio) 미만)
 * @gfp:     새 bio 할당 플래그
 * @bs:      새 bio를 할당할 bio_set
 * @return:  앞부분 sectors를 담은 새 bio; 실패 시 ERR_PTR
 *
 * bio_alloc_clone()으로 원본 bi_io_vec를 공유하는 새 bio를 생성한다.
 * 새 bio의 bi_size = sectors << 9; 원본 bio는 bio_advance()로 해당만큼 전진.
 * REQ_OP_ZONE_APPEND: NVMe ZNS 컨트롤러가 쓰기 위치를 결정하므로 분할 불가.
 * REQ_ATOMIC: NVMe 원자 쓰기 단위 분할 불가.
 * 실행 컨텍스트: 태스크 컨텍스트 (blk-mq 제출 경로).
 * 호출자: blk_queue_split() (blk-mq 제출 전 처리), dm/md 등
 * NVMe 연결: MDTS·max_segments 초과 시 여러 CID로 분할; bio_chain()으로 완료 집계.
 *
 * 호출 체인:
 *   blk_queue_split() → [bio_split()] → bio_alloc_clone() + bio_advance()
 */
struct bio *bio_split(struct bio *bio, int sectors,  // NVMe MDTS/segment 한도 초과 시 여러 CID로 분할
		      gfp_t gfp, struct bio_set *bs)
{
	struct bio *split;

	if (WARN_ON_ONCE(sectors <= 0))
		return ERR_PTR(-EINVAL);  // 오류 포인터 반환: NVMe 분할/clone 실패 전파
	if (WARN_ON_ONCE(sectors >= bio_sectors(bio)))
		return ERR_PTR(-EINVAL);  // 오류 포인터 반환: NVMe 분할/clone 실패 전파

	/* Zone append commands cannot be split */
	if (WARN_ON_ONCE(bio_op(bio) == REQ_OP_ZONE_APPEND))  // ZNS zone append: NVMe 컨트롤러가 쓰기 위치를 결정하므로 분할 불가
		return ERR_PTR(-EINVAL);  // 오류 포인터 반환: NVMe 분할/clone 실패 전파

	/* atomic writes cannot be split */
	if (bio->bi_opf & REQ_ATOMIC)	// NVMe atomic write는 분할/trim 불가
		return ERR_PTR(-EINVAL);  // 오류 포인터 반환: NVMe 분할/clone 실패 전파

	split = bio_alloc_clone(bio->bi_bdev, bio, gfp, bs);  // bio_vec 공유 clone: NVMe MDTS 분할 시 원본 PRP/SGL 재사용
	if (!split)
		return ERR_PTR(-ENOMEM);  // 오류 포인터 반환: NVMe 분할/clone 실패 전파

	split->bi_iter.bi_size = sectors << 9;  // NVMe 명령의 NLB(Length)로 변환됨

	if (bio_integrity(split))  // NVMe PI/DIF 보호 정보 처리: PRP/SGL과 함께 보호 정보가 일치해야 컨트롤러가 명령을 수락함
		bio_integrity_trim(split);

	bio_advance(bio, split->bi_iter.bi_size);  // NVMe partial completion 후 남은 sector 범위 갱신

	if (bio_flagged(bio, BIO_TRACE_COMPLETION))
		bio_set_flag(split, BIO_TRACE_COMPLETION);

	return split;
}
EXPORT_SYMBOL(bio_split);  // NVMe MDTS/segment 한도 초과 시 여러 CID로 분할

/**
 * bio_trim - trim a bio
 * @bio:	bio to trim
 * @offset:	number of sectors to trim from the front of @bio
 * @size:	size we want to trim @bio to, in sectors
 *
 * This function is typically used for bios that are cloned and submitted
 * to the underlying device in parts.
 */
/*
 * [한국어]
 * bio_trim - bio의 앞뒤를 잘라내어 일부 sector만 남긴다
 *
 * @bio:    trim할 bio (clone된 bio에 주로 사용)
 * @offset: 앞에서 자를 sector 수
 * @size:   남길 sector 수
 *
 * bio_advance()로 앞부분(offset)을 건너뛰고, bi_size를 size << 9로 설정한다.
 * integrity context도 같이 trim된다(bio_integrity_trim).
 * REQ_ATOMIC 설정 시 size > 0이면 WARN 후 반환(원자 쓰기는 분할 불가).
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: DM/MD 드라이버, bio clone 후 부분 제출하는 상위 레이어
 * NVMe 연결: 조정된 bi_sector → SLBA, bi_size → NLB로 nvme_queue_rq()에서 변환.
 *
 * 호출 체인:
 *   dm/md → [bio_trim()] → bio_advance() + bi_size 설정
 */
void bio_trim(struct bio *bio, sector_t offset, sector_t size)
{
	/* We should never trim an atomic write */
	if (WARN_ON_ONCE(bio->bi_opf & REQ_ATOMIC && size))  // NVMe atomic write 단위: 분할/trim 불가
		return;

	if (WARN_ON_ONCE(offset > BIO_MAX_SECTORS || size > BIO_MAX_SECTORS ||
			 offset + size > bio_sectors(bio)))
		return;

	size <<= 9;
	if (offset == 0 && size == bio->bi_iter.bi_size)  // NVMe 명령의 NLB(Length)로 변환됨
		return;

	bio_advance(bio, offset << 9);  // NVMe partial completion 후 남은 sector 범위 갱신
	bio->bi_iter.bi_size = size;  // NVMe 명령의 NLB(Length)로 변환됨

	if (bio_integrity(bio))	// NVMe PI/DIF 보호 정보 해제
		bio_integrity_trim(bio);
}
EXPORT_SYMBOL_GPL(bio_trim);

/*
 * create memory pools for biovec's in a bio_set.
 * use the global biovec slabs created for general use.
 */
/*
 * [한국어]
 * biovec_init_pool - bio_vec mempool을 최대 크기 슬랩(biovec-max)으로 초기화한다
 *
 * @pool:         초기화할 mempool
 * @pool_entries: mempool 예약 엔트리 수
 * @return:       0 성공; 음수 에러
 *
 * bvec_slabs의 마지막(BIO_MAX_VECS 크기) 슬랩을 사용하여 bio_vec 배열 mempool을 구성한다.
 * 메모리 압박 시에도 bio_vec 배열 할당이 보장된다.
 * 실행 컨텍스트: bioset_init() 초기화 경로.
 * 호출자: bioset_init() (BIOSET_NEED_BVECS 플래그 설정 시)
 * NVMe 연결: 메모리 부족 시에도 PRP/SGL 기술에 필요한 bio_vec 배열 확보 보장.
 *
 * 호출 체인:
 *   bioset_init() → [biovec_init_pool()] → mempool_init_slab_pool()
 */
int biovec_init_pool(mempool_t *pool, int pool_entries)
{
	struct biovec_slab *bp = bvec_slabs + ARRAY_SIZE(bvec_slabs) - 1; /* [한국어] biovec-max 슬랩을 선택: 최대 PRP/SGL 목록을 커버 */

	return mempool_init_slab_pool(pool, pool_entries, bp->slab); /* [한국어] 최대 크기 bio_vec 슬랩으로 mempool 초기화 */
}

/*
 * bioset_exit - exit a bioset initialized with bioset_init()
 *
 * May be called on a zeroed but uninitialized bioset (i.e. allocated with
 * kzalloc()).
 */
/*
 * [한국어]
 * bioset_exit - bioset_init()으로 초기화된 bio_set을 정리하고 자원을 해제한다
 *
 * @bs: 정리할 bio_set
 *
 * 정리 순서: per-CPU 캐시 → rescue workqueue → bio_pool → bvec_pool → bio_slab.
 * 이미 초기화되지 않은 필드는 안전하게 무시된다.
 * 실행 컨텍스트: 태스크 컨텍스트 (드라이버 언로드 경로).
 * 호출자: bioset_init() 실패 경로, 드라이버/모듈 언로드
 * NVMe 연결: NVMe 드라이버 언로드 시 bio_set 정리로 메모리 누수 방지.
 *
 * 호출 체인:
 *   드라이버 언로드 → [bioset_exit()] → bio_alloc_cache_destroy() + mempool_exit() + bio_put_slab()
 */
void bioset_exit(struct bio_set *bs)
{
	bio_alloc_cache_destroy(bs);                           /* [한국어] per-CPU 캐시 전체 정리 및 cpuhp 콜백 제거 */
	if (bs->rescue_workqueue)
		destroy_workqueue(bs->rescue_workqueue);       /* [한국어] rescue workqueue 파괴: mempool 고갈 시 재제출 메커니즘 해제 */
	bs->rescue_workqueue = NULL;                           /* [한국어] 포인터 초기화: 이중 파괴 방지 */

	mempool_exit(&bs->bio_pool);                           /* [한국어] bio 본체 mempool 해제 */
	mempool_exit(&bs->bvec_pool);                          /* [한국어] bio_vec 배열 mempool 해제 */

	if (bs->bio_slab)
		bio_put_slab(bs);                              /* [한국어] bio 슬랩 참조 해제; 마지막이면 kmem_cache_destroy() */
	bs->bio_slab = NULL;                                   /* [한국어] 포인터 초기화 */
}
EXPORT_SYMBOL(bioset_exit);

/**
 * bioset_init - Initialize a bio_set
 * @bs:		pool to initialize
 * @pool_size:	Number of bio and bio_vecs to cache in the mempool
 * @front_pad:	Number of bytes to allocate in front of the returned bio
 * @flags:	Flags to modify behavior, currently %BIOSET_NEED_BVECS
 *              and %BIOSET_NEED_RESCUER
 *
 * Description:
 *    Set up a bio_set to be used with @bio_alloc_bioset. Allows the caller
 *    to ask for a number of bytes to be allocated in front of the bio.
 *    Front pad allocation is useful for embedding the bio inside
 *    another structure, to avoid allocating extra data to go with the bio.
 *    Note that the bio must be embedded at the END of that structure always,
 *    or things will break badly.
 *    If %BIOSET_NEED_BVECS is set in @flags, a separate pool will be allocated
 *    for allocating iovecs.  This pool is not needed e.g. for bio_init_clone().
 *    If %BIOSET_NEED_RESCUER is set, a workqueue is created which can be used
 *    to dispatch queued requests when the mempool runs out of space.
 *
 */
/*
 * [한국어]
 * bioset_init - bio 할당 풀(bio_set)을 초기화한다
 *
 * @bs:        초기화할 bio_set
 * @pool_size: bio/bio_vec mempool의 예약 엔트리 수
 * @front_pad: bio 앞에 드라이버 전용 데이터를 위한 패딩 바이트 수
 * @flags:     BIOSET_NEED_BVECS | BIOSET_NEED_RESCUER | BIOSET_PERCPU_CACHE 조합
 * @return:    0 성공; -ENOMEM 실패
 *
 * 초기화 순서:
 *   1. front_pad/back_pad 설정 (BIOSET_NEED_BVECS이면 back_pad에 inline bvec 공간)
 *   2. rescue 메커니즘 초기화 (lock, list, work)
 *   3. bio_slab 공유/생성 (bio_find_or_create_slab)
 *   4. bio_pool(mempool) 초기화
 *   5. (BIOSET_NEED_BVECS) bvec_pool(mempool) 초기화
 *   6. (BIOSET_NEED_RESCUER) rescue workqueue 생성
 *   7. (BIOSET_PERCPU_CACHE) per-CPU 캐시 할당 및 cpuhp 등록
 * 실행 컨텍스트: 태스크 컨텍스트 (드라이버/모듈 초기화 경로).
 * 호출자: init_bio() (fs_bio_set), NVMe/SCSI/DM 드라이버
 * NVMe 연결: fs_bio_set는 파일 시스템 bio의 기본 풀; BIOSET_NEED_RESCUER로 deadlock 방지;
 *            BIOSET_PERCPU_CACHE로 NVMe 핫패스 bio 할당 지연 감소.
 *
 * 호출 체인:
 *   init_bio() → [bioset_init(&fs_bio_set, ...)] → bio_find_or_create_slab() + mempool_init_*()
 */
int bioset_init(struct bio_set *bs,  // bio_set 초기화: NVMe 제출에 사용되는 전역/드라이버 풀 생성
		unsigned int pool_size,
		unsigned int front_pad,
		int flags)
{
	/* [한국어] front_pad = bio 본체 "앞"에 붙일 드라이버 전용 공간 크기.
	 * 슬랩 오브젝트 레이아웃이 [front_pad][struct bio][back_pad]가 되어,
	 * 드라이버가 bio 하나당 자기 데이터를 별도 할당 없이 함께 쓸 수 있다.
	 * NVMe에서는 struct nvme_iod가 이 자리를 차지해, bio→request 변환 시
	 * PRP/SGL 구성 상태를 담는다. */
	bs->front_pad = front_pad;
	/* [한국어] back_pad = bio 본체 "뒤"에 붙일 인라인 bvec 배열 공간.
	 * BIO_INLINE_VECS(보통 4)개까지는 별도 할당 없이 여기 담아, 작은 I/O에서
	 * bvec 슬랩 할당을 통째로 생략한다. 대부분의 파일시스템 I/O가 4개 이하
	 * 세그먼트라 이 최적화의 적중률이 높다. */
	if (flags & BIOSET_NEED_BVECS)
		bs->back_pad = BIO_INLINE_VECS * sizeof(struct bio_vec);
	else
		/* [한국어] bvec이 필요 없는 bio_set(예: 데이터 없는 flush 전용)은
		 * 뒤쪽 공간을 잡지 않아 오브젝트 크기를 줄인다. */
		bs->back_pad = 0;

	/* [한국어] ★ rescue 메커니즘 ★
	 * 스택형 드라이버(dm/md)에서 bio를 분할해 하위로 보내는데, 그 하위 제출이
	 * 다시 같은 bio_set에서 할당을 시도할 수 있다. mempool이 비어 있으면
	 * 앞선 bio의 완료를 기다려야 하는데, 그 bio는 아직 제출되지 않고 현재
	 * 스레드의 bio_list에 대기 중이라 영원히 완료되지 않는다 — 자기 자신을
	 * 기다리는 교착이다.
	 * rescue_list와 워커가 이 상황을 푼다: 대기 중인 bio를 별도 워커가
	 * 대신 제출해 진행을 만든다. */
	spin_lock_init(&bs->rescue_lock);
	bio_list_init(&bs->rescue_list);
	INIT_WORK(&bs->rescue_work, bio_alloc_rescue);

	/* [한국어] 이 bio_set의 오브젝트 크기(front_pad + bio + back_pad)에 맞는
	 * 슬랩 캐시를 찾거나 만든다. 같은 크기를 쓰는 bio_set끼리는 캐시를
	 * 공유해 슬랩 종류가 무한정 늘어나지 않게 한다. */
	bs->bio_slab = bio_find_or_create_slab(bs);
	if (!bs->bio_slab)
		return -ENOMEM;

	/* [한국어] 위 슬랩 위에 mempool을 얹는다. mempool은 pool_size개를 미리
	 * 확보해 두어, 메모리가 고갈되어도 진행 중인 I/O가 완료될 만큼은
	 * 반드시 할당된다 — write-back 교착을 막는 안전망이다. */
	if (mempool_init_slab_pool(&bs->bio_pool, pool_size, bs->bio_slab))
		goto bad;

	/* [한국어] 인라인 한도를 넘는 bvec 배열을 위한 mempool. 같은 이유로
	 * 예약분이 필요하다 — 큰 I/O도 메모리 압박 하에서 진행되어야 한다. */
	if ((flags & BIOSET_NEED_BVECS) &&
	    biovec_init_pool(&bs->bvec_pool, pool_size))
		goto bad;

	if (flags & BIOSET_NEED_RESCUER) {
		/* [한국어] rescue 워커를 실행할 워크큐. WQ_MEM_RECLAIM이 필수인
		 * 이유: 이 워커가 푸는 문제 자체가 메모리 부족 상황의 교착이라,
		 * 워커 실행이 다시 메모리를 기다리면 아무것도 해결되지 않는다.
		 * 전용 rescuer 스레드가 그 순환을 끊는다. */
		bs->rescue_workqueue = alloc_workqueue("bioset",
							WQ_MEM_RECLAIM, 0);
		if (!bs->rescue_workqueue)
			goto bad;
	}
	if (flags & BIOSET_PERCPU_CACHE) {
		/* [한국어] per-CPU bio 캐시. 완료된 bio를 슬랩에 돌려주는 대신
		 * 자기 CPU의 리스트에 쌓아 두었다가 다음 할당에 재사용한다.
		 * 락도 원자적 연산도 없어 매우 빠르며, 초당 수십만 bio를 만드는
		 * NVMe 워크로드에서 할당 비용을 크게 줄인다. */
		bs->cache = alloc_percpu(struct bio_alloc_cache);
		if (!bs->cache)
			goto bad;
		/* [한국어] CPU 핫플러그 알림에 등록한다. CPU가 오프라인되면 그
		 * CPU의 캐시에 남은 bio를 회수해야 한다 — 그대로 두면 그 메모리가
		 * 영원히 묶인다. _nocalls 변형은 등록만 하고 지금 콜백을 부르지
		 * 않는다는 뜻으로, 아직 캐시가 비어 있어 회수할 것이 없기 때문이다. */
		cpuhp_state_add_instance_nocalls(CPUHP_BIO_DEAD, &bs->cpuhp_dead);
	}

	return 0;
bad:
	/* [한국어] 어느 단계에서 실패하든 bioset_exit()에 전부 맡긴다.
	 * 이것이 가능한 이유는 호출자가 bs를 0으로 초기화해 넘기고,
	 * bioset_exit()이 각 필드의 NULL/초기 상태를 확인해 "만들어진 것만"
	 * 정리하도록 작성되어 있기 때문이다. 단계별 goto 라벨 사다리를
	 * 두지 않아도 되는 깔끔한 구조다. */
	bioset_exit(bs);
	return -ENOMEM;
}
EXPORT_SYMBOL(bioset_init);

/*
 * [한국어]
 * init_bio - bio 서브시스템의 전역 슬랩, cpuhp, fs_bio_set을 초기화한다
 *
 * subsys_initcall로 등록되어 커널 부팅 시 일찍 실행된다.
 * 초기화 순서:
 *   1. BUILD_BUG_ON으로 bi_flags 크기 컴파일 타임 검사
 *   2. bvec_slabs[] 배열의 각 biovec 슬랩 캐시 생성 (16/64/128/max)
 *   3. CPUHP_BIO_DEAD CPU 핫플러그 다중 인스턴스 상태 등록
 *   4. fs_bio_set 초기화 (BIOSET_NEED_BVECS | BIOSET_PERCPU_CACHE)
 * 실패 시 panic()으로 부팅 중단 (bio 없이는 블록 I/O 불가).
 * 실행 컨텍스트: 커널 초기화 경로 (단일 스레드).
 * NVMe 연결: NVMe 드라이버가 로드되기 전에 bio 인프라가 준비되어야 함.
 *            fs_bio_set와 biovec 슬랩이 없으면 NVMe doorbell 제출 경로가 동작하지 않음.
 *
 * 호출 체인:
 *   subsys_initcall → [init_bio()] → bioset_init(&fs_bio_set)
 */
static int __init init_bio(void)  // bio 서브시스템 초기화: NVMe 드라이버보다 먼저 준비되어야 함
{
	int i;

	BUILD_BUG_ON(BIO_FLAG_LAST > 8 * sizeof_field(struct bio, bi_flags));	// struct bio flags 크기 불변: NVMe 드라이버 바이너리 호환성

	for (i = 0; i < ARRAY_SIZE(bvec_slabs); i++) {
		struct biovec_slab *bvs = bvec_slabs + i;

		bvs->slab = kmem_cache_create(bvs->name,  // bio/bio_vec slab 생성: NVMe bio 할당 성능에 영향
				bvs->nr_vecs * sizeof(struct bio_vec), 0,
				SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);
	}

	cpuhp_setup_state_multi(CPUHP_BIO_DEAD, "block/bio:dead", NULL,
					bio_cpu_dead);

	if (bioset_init(&fs_bio_set, BIO_POOL_SIZE, 0,  // bio_set 초기화: NVMe 제출에 사용되는 전역/드라이버 풀 생성
			BIOSET_NEED_BVECS | BIOSET_PERCPU_CACHE))
		panic("bio: can't allocate bios\n");

	return 0;
}
subsys_initcall(init_bio);  // bio 서브시스템 초기화: NVMe 드라이버보다 먼저 준비되어야 함

/* NVMe 관점 핵심 요약
 *
 * - bio는 파일 시스템부터 NVMe SQ doorbell까지 I/O를 운송하는 핵심 컨테이너이며,
 *   bi_iter는 SLBA/length, bi_io_vec은 PRP/SGL로 변환된다.
 * - submit_bio() -> blk_mq_submit_bio() -> blk_mq_get_request() ->
 *   nvme_queue_rq() -> nvme_submit_cmd(doorbell) 호출 연쇄를 통해 NVMe SQ에 CID가 할당된다.
 * - bio_split()과 bio_chain()은 NVMe MDTS, segment 한도, zone append/atomic
 *   write 제약을 준수하면서도 대용량 I/O를 여러 명령으로 쪼개는 데 사용된다.
 * - bio_endio()는 nvme_process_cq() -> nvme_complete_rq() 이후에 호출되며,
 *   bio_chain으로 묶인 모든 하위 bio가 완료된 뒤에야 상위 완료 콜백이 실행된다.
 * - bio_alloc_bioset()/bio_put()의 per-CPU cache와 mempool/rescuer 메커니즘은
 *   메모리 부족 상황에서도 NVMe doorbell 제출이 정지하지 않도록 보장한다.
 */
