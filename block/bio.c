// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2001 Jens Axboe <axboe@kernel.dk>
 */
/*
 * [한국어 설명] bio.c — bio 생명주기 및 SGL/PRP 구성 핵심 (bio.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Linux 블록 I/O 서브시스템의 핵심 자료구조인 struct bio의
 * 전체 생명주기(할당·초기화·재사용·복제·분할·완료·해제)를 구현한다.
 * bio(Block I/O descriptor)는 파일 시스템이 생성한 I/O 요청을 블록 레이어,
 * blk-mq 스케줄러, 그리고 최종적으로 NVMe 드라이버까지 운반하는 컨테이너다.
 * bio의 bi_iter(SLBA/length)와 bi_io_vec(페이지 배열)이 NVMe 명령의
 * SLBA·NLB·PRP/SGL 필드로 직접 변환되므로, 이 파일은 NVMe I/O 경로의 시발점이다.
 * 메모리 부족 상황에서도 I/O가 멈추지 않도록 mempool·per-CPU cache·rescuer
 * workqueue 세 겹의 안전망이 구현되어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (위 → 아래):
 *   파일 시스템(ext4/xfs/btrfs) / kiocb DIO
 *     → bio_alloc_bioset() / bio_iov_iter_get_pages()   ← 이 파일
 *     → submit_bio() / submit_bio_noacct()               ← blk-core.c
 *     → blk_mq_submit_bio() / blk_mq_get_request()      ← blk-mq.c
 *     → nvme_queue_rq() / nvme_submit_cmd(doorbell)      ← drivers/nvme/host/pci.c
 *     → NVMe 컨트롤러 SQ(Submission Queue) → 하드웨어
 *     → NVMe CQ(Completion Queue) → nvme_process_cq()
 *     → blk_mq_end_request() → bio_endio()              ← 이 파일
 *     → bi_end_io() 콜백 → 파일 시스템 완료 경로
 *
 * 실행 컨텍스트: 커널 유저스페이스(태스크 컨텍스트) 및 소프트/하드 IRQ 컨텍스트.
 * bio 할당·제출은 태스크 컨텍스트, bio_endio()·bio_put() 일부는 IRQ 컨텍스트에서도 실행됨.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - include/linux/blk_types.h : struct bio, struct bio_vec, struct bvec_iter 정의
 *   - block/blk.h               : 내부 인터페이스, bio_set, mempool 연결
 *   - block/blk-cgroup.c        : bio_associate_blkg(), blkg_put() — cgroup QoS 연동
 *   - block/bio-integrity.c     : bio_integrity_*(T10-PI/DIX/DIF) — NVMe PI 보호 정보 처리
 *   - block/blk-crypto.c        : bio_crypt_clone(), bio_crypt_advance() — inline 암호화
 *   - block/blk-mq.c            : blk_mq_submit_bio(), blk_mq_end_request() — 요청 큐 연결
 *   - drivers/nvme/host/pci.c   : nvme_queue_rq(), nvme_submit_cmd() — 실제 doorbell 제출
 *
 * 데이터 흐름:
 *   사용자 버퍼 페이지 → bio_iov_iter_get_pages() → bio_vec(bvec) 배열
 *   → (blk-mq) → nvme_map_data() → PRP entry / SGL segment
 *   → NVMe SQ CDW 채움 → DMA 전송 → CQ 완료 → bio_endio() → 사용자 완료
 *
 * 공유 핵심 자료구조:
 *   struct bio        : I/O 요청 단위 — 이 파일이 생성·소멸을 책임짐
 *   struct bio_set    : bio/bio_vec 메모리 풀 — 전역(fs_bio_set) 및 드라이버 전용 풀
 *   struct bio_vec    : 물리 페이지 + 오프셋 + 길이 — NVMe PRP entry 단위
 *   struct bvec_iter  : bio 내 현재 위치(sector/size/idx) — NVMe SLBA·NLB 변환 기반
 *
 * === 주요 함수/구조체 요약 ===
 * bio_alloc_bioset()    : per-CPU cache → slab → mempool 순서로 bio 할당; NVMe 제출 시작점
 * bio_init()            : bi_iter/bi_opf/bi_io_vec 초기화; SLBA·NLB·PRP 기반 준비
 * bio_add_page()        : 사용자/커널 페이지를 bvec에 추가; NVMe PRP entry 후보 구성
 * bio_iov_iter_get_pages(): 사용자 iov 버퍼 pin 및 bvec 채움; NVMe DMA의 실제 준비 단계
 * bio_split()           : NVMe MDTS·segment 한도 초과 시 bio를 두 개로 분리; CID 분할
 * bio_chain()           : 분할된 bio들의 완료를 부모로 집계; 분할 CID 모두 완료 후 상위 통보
 * bio_endio()           : NVMe CQ 처리 완료 후 상위 레이어 bi_end_io 호출; 체인 완료 집계
 * submit_bio_wait()     : bio 제출 후 CQ 완료까지 동기 대기; NVMe admin/sync 명령에 사용
 * bioset_init()         : bio_set(mempool+slab+rescuer+percpu-cache) 초기화; 풀 구성
 * bio_put()             : 참조 카운트 감소 후 per-CPU cache 또는 mempool로 bio 반환
 */
#include <linux/mm.h>          /* [한국어] 페이지 할당(alloc_page 등)/물리 주소 변환(page_to_phys): NVMe DMA용 PRP 주소 계산에 필수 */
#include <linux/swap.h>         /* [한국어] 메모리 회수 경로(shrink_list 등): bio 할당 시 OOM reclaim과 상호작용 */
#include <linux/bio-integrity.h>/* [한국어] bio integrity(T10-PI/DIX/DIF) API: NVMe PI 보호 정보를 bio에 붙이거나 검증할 때 사용 */
#include <linux/blkdev.h>       /* [한국어] 블록 장치 인터페이스(struct block_device, submit_bio 등): NVMe ns 식별 및 I/O 제출의 기반 */
#include <linux/uio.h>          /* [한국어] iov_iter/iovec: 사용자 버퍼를 기술하며, bio_iov_iter_get_pages()로 페이지를 pin해 PRP/SGL 생성 */
#include <linux/iocontext.h>    /* [한국어] I/O 컨텍스트(I/O 스케줄러 힌트, CFQ elevator): blkcg 및 I/O 우선순위 관리에 사용 */
#include <linux/slab.h>         /* [한국어] kmem_cache/kmalloc: bio·bio_vec·bio_slab 슬랩 할당기; NVMe 핫패스 bio 할당의 근간 */
#include <linux/init.h>         /* [한국어] subsys_initcall/module_init 매크로: init_bio()를 커널 부팅 시 자동 실행 */
#include <linux/kernel.h>       /* [한국어] 범용 커널 유틸(BUG_ON, WARN_ON, min/max 등): 불변조건 검사 및 공통 연산 */
#include <linux/export.h>       /* [한국어] EXPORT_SYMBOL: bio_alloc_bioset, bio_endio 등 공개 API를 모듈에서 사용 가능하게 함 */
#include <linux/mempool.h>      /* [한국어] mempool_t: 메모리 부족 시에도 bio·bio_vec를 보장 할당; NVMe doorbell 경로 안전망 */
#include <linux/workqueue.h>    /* [한국어] work_struct/workqueue: bio_alloc_rescue 워크큐; mempool 고갈 시 교착 상태 방지용 rescuer */
#include <linux/cgroup.h>       /* [한국어] cgroup 자료구조: blkcg(블록 cgroup) QoS/throttling — NVMe 장치 단위 I/O 제한에 연결됨 */
#include <linux/highmem.h>      /* [한국어] highmem 페이지 매핑(kmap_local_page 등): 32비트 커널에서 4GB 초과 물리 페이지 접근 */
#include <linux/blk-crypto.h>   /* [한국어] inline 블록 암호화(ICE/UFS/NVMe Opal): bio_crypt_clone/advance로 암호 컨텍스트를 bio에 연계 */
#include <linux/xarray.h>       /* [한국어] XArray(radix-tree 대체): bio_slabs XArray로 크기별 kmem_cache를 O(1)에 조회 */
#include <linux/kmemleak.h>     /* [한국어] 메모리 누수 감지기(kmemleak): per-CPU cache에서 bio 할당/해제 시 추적 등록/해제 */

#include <trace/events/block.h> /* [한국어] 블록 레이어 tracepoint: trace_block_bio_complete 등 perf/ftrace로 NVMe 완료 이벤트 관측 */
#include "blk.h"                /* [한국어] blk 내부 헤더: bio_set, request_queue 내부 구조 및 blk_*() 함수 선언 */
#include "blk-rq-qos.h"         /* [한국어] request QoS 훅(rq_qos_done_bio 등): bio 완료 시 latency/cost 통계 갱신 */
#include "blk-cgroup.h"         /* [한국어] blkcg 내부 API(bio_associate_blkg, blkg_put): NVMe cgroup throttling 연동 */

#define ALLOC_CACHE_THRESHOLD	16  /* [한국어] per-CPU IRQ cache가 이 값 이상이면 태스크 캐시로 splice: NVMe 완료 ISR에서 축적된 bio를 태스크로 이전 */
#define ALLOC_CACHE_MAX		256 /* [한국어] per-CPU cache 최대 크기: 초과 시 mempool로 반납하여 메모리 낭비 방지 */

/*
 * [한국어] bio_alloc_cache — bio 할당용 per-CPU 캐시
 *
 * NVMe 고속 경로에서 doorbell 지연을 줄이려면 bio 할당이 빨라야 한다.
 * per-CPU 캐시는 slab/mempool 락 없이 bio를 재사용하므로 캐시 히트 시
 * 할당 비용이 사실상 0에 가깝다.
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

#define BIO_INLINE_VECS 4 /* [한국어] bio 구조체 본체 내에 inline으로 포함되는 bio_vec 배열 크기.
                           * 4개 이하의 segment(페이지)는 별도 슬랩 할당 없이 bio 안에 저장됨.
                           * 대부분의 작은 NVMe 4K~16K I/O는 이 한도 이내에서 처리된다. */

/*
 * [한국어] biovec_slab — bio_vec 배열을 위한 슬랩 풀 후보군
 *
 * 이 배열은 bio가 필요로 하는 segment 수에 따라 적절한 kmem_cache를 선택한다.
 * NVMe 명령의 PRP list 최대 개수(보통 1 PRP entry = 1 페이지)와 SGL segment 한도는
 * 컨트롤러별로 다르므로, 커널은 나중에 blk-mq/nvme_queue_rq에서 분할한다.
 *
 * nr_vecs 필드:
 *   [한국어] 한 bio_vec 배열이 수용할 수 있는 최대 segment(PRP/SGL entry) 수.
 *   설정자: bvec_slabs[] 정적 초기화 시 고정.
 *   읽는 자: biovec_slab()가 nr_vecs로 올바른 슬랩을 선택.
 *   값 범위: 16, 64, 128, BIO_MAX_VECS 중 하나.
 *   NVMe 연결: NVMe 컨트롤러의 max_segments가 이 값보다 작으면 bio_split()으로 분할.
 *
 * slab 필드:
 *   [한국어] bio_vec 메모리를 담당하는 kmem_cache 포인터.
 *   설정자: init_bio()의 kmem_cache_create() 호출로 초기화.
 *   읽는 자: bio_alloc_bioset()/bio_free()가 bio_vec 슬랩 할당/해제 시 사용.
 *   동기화: __read_mostly 섹션에 배치되어 CPU 캐시 상주; 초기화 후 읽기 전용.
 */
static struct biovec_slab {
	int nr_vecs;
	char *name;
	struct kmem_cache *slab;
} bvec_slabs[] __read_mostly = {
	{ .nr_vecs = 16, .name = "biovec-16" },
	{ .nr_vecs = 64, .name = "biovec-64" },
	{ .nr_vecs = 128, .name = "biovec-128" },
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
 * NVMe 연결: bi_max_vecs가 NVMe PRP/SGL 한도 초과 시 blk-mq/nvme_queue_rq가 bio_split()으로 분할.
 */
static struct biovec_slab *biovec_slab(unsigned short nr_vecs)
{
	switch (nr_vecs) {
	/* smaller bios use inline vecs */
	case 5 ... 16:   /* [한국어] 5~16 segment: 작은 NVMe 4K~64K I/O; biovec-16 슬랩 선택 */
		return &bvec_slabs[0];
	case 17 ... 64:  /* [한국어] 17~64 segment: 중간 크기 NVMe I/O; PRP list 짧음; biovec-64 슬랩 선택 */
		return &bvec_slabs[1];
	case 65 ... 128: /* [한국어] 65~128 segment: 대용량 NVMe I/O; biovec-128 슬랩 선택 */
		return &bvec_slabs[2];
	case 129 ... BIO_MAX_VECS: /* [한국어] 129~BIO_MAX_VECS: NVMe SGL 또는 긴 PRP list; biovec-max 슬랩 선택 */
		return &bvec_slabs[3];
	default:
		BUG(); /* [한국어] 1~4는 BIO_INLINE_VECS로 처리되어 이 함수에 오면 안 됨; 버그 표시 */
		return NULL; /* [한국어] BUG() 이후 도달 불가; 컴파일러 경고 억제용 */
	}
}

/*
 * [한국어] struct bio_set — bio·bio_vec 메모리 풀의 집합체
 * (실제 정의: include/linux/blk_types.h; 여기서는 중요 필드만 설명)
 *
 * bio_pool 필드:
 *   [한국어] struct bio 본체를 위한 mempool.
 *   설정자: bioset_init()의 mempool_init_slab_pool()이 초기화.
 *   읽는 자: bio_alloc_bioset()이 slab 실패 시 fallback으로 mempool_alloc() 호출.
 *   NVMe 연결: 메모리 부족 상황에서도 NVMe SQ doorbell 제출이 정지하지 않도록 보장.
 *
 * bvec_pool 필드:
 *   [한국어] bio_vec 배열을 위한 mempool.
 *   설정자: bioset_init()의 biovec_init_pool()이 초기화.
 *   읽는 자: bio_alloc_bioset()이 BIO_MAX_VECS 크기의 bvec 배열을 mempool에서 할당.
 *   NVMe 연결: PRP/SGL 리스트를 구성할 bvec 배열을 항상 확보할 수 있게 한다.
 *
 * front_pad/back_pad 필드:
 *   [한국어] bio 앞뒤에 드라이버 전용 데이터를 배치하는 패딩 크기.
 *   NVMe 드라이버(nvme-pci)는 request 구조체 안에 bio를 임베드할 때 front_pad를 활용.
 *   back_pad에는 BIO_INLINE_VECS 개수의 bio_vec 배열이 인라인으로 저장된다.
 *
 * cache 필드:
 *   [한국어] per-CPU bio_alloc_cache 포인터. NVMe CQ 처리 완료 후 bio_put() 시
 *   이 캐시로 bio가 반환되어 재할당 지연을 줄인다. BIOSET_PERCPU_CACHE 플래그 필요.
 *
 * rescue_workqueue 필드:
 *   [한국어] mempool 고갈 시 submit_bio_noacct() 아래에서 교착 상태를 피해
 *   bio_list를 재제출하는 rescuer 워크큐. BIOSET_NEED_RESCUER 플래그로 활성화.
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
	 * NVMe 드라이버가 request 안에 bio를 임베드(front_pad)하므로
	 * 이 크기가 request 구조체 전체 크기에 영향을 준다.
	 * XArray bio_slabs의 키(index)로도 사용된다.
	 */
	char name[12];
	/*
	 * [한국어] 슬랩 캐시 이름 (예: "bio-256"). /proc/slabinfo에 표시됨.
	 * 설정자: create_bio_slab()의 snprintf().
	 * 읽는 자: kmem_cache_create()가 슬랩 이름으로 등록.
	 */
};
static DEFINE_MUTEX(bio_slab_lock); /* [한국어] bio_slabs XArray와 slab_ref 증감을 직렬화하는 뮤텍스.
                                     * NVMe 드라이버 로드/언로드 시 bio_set 생성/소멸이 동시 발생할 수 있으므로 필요. */
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
 * SLAB_TYPESAFE_BY_RCU: 슬랩 오브젝트가 RCU grace period 동안 재사용되지 않도록 보장.
 * SLAB_HWCACHE_ALIGN: 캐시라인 정렬로 NVMe 핫패스 bio 접근 성능 향상.
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
 * 실행 컨텍스트: 태스크 컨텍스트 및 하드 IRQ 컨텍스트(bio_put_percpu_cache 경로).
 * 호출자: bio_free(), bio_reset(), bio_reuse(), bio_endio() (cgroup 경로)
 * NVMe 연결: NVMe PI/DIF 활성화 장치에서 integrity 해제 없이 bio를 재사용하면
 *            보호 정보가 오염되어 컨트롤러가 명령을 거부할 수 있다.
 *
 * 호출 체인:
 *   nvme_complete_rq() → blk_mq_end_request() → bio_endio() → bio_put() → bio_free() → [bio_uninit()]
 */
void bio_uninit(struct bio *bio)  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제
{
#ifdef CONFIG_BLK_CGROUP
	if (bio->bi_blkg) {
		blkg_put(bio->bi_blkg);  // cgroup 참조 해제: NVMe cgroup 기반 queue 제한과 연결
		bio->bi_blkg = NULL;
	}
#endif
	if (bio_integrity(bio))	// NVMe PI/DIF 보호 정보 해제
		bio_integrity_free(bio);

	bio_crypt_free_ctx(bio);	// NVMe Opal/inline crypto context 해제
}
EXPORT_SYMBOL(bio_uninit);  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제

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
 * 실행 컨텍스트: 태스크 컨텍스트 및 하드 IRQ 컨텍스트.
 * 호출자: bio_put(), bio_alloc_cache_prune()
 * NVMe 연결: NVMe CQ 수신 후에야 bio가 반환될 수 있다; CID가 살아 있는 동안 bio는 유효.
 *
 * 호출 체인:
 *   bio_put() → [bio_free()] → mempool_free() / kmem_cache_free()
 */
static void bio_free(struct bio *bio)  // NVMe 완료 후 bio 메모리를 풀로 반환
{
	struct bio_set *bs = bio->bi_pool;  // bio_set/mempool: NVMe doorbell 경로 메모리 보장
	void *p = bio;

	WARN_ON_ONCE(!bs);  // NVMe 명령/상태 불변조건 위반 방지용 assert
	WARN_ON_ONCE(bio->bi_max_vecs > BIO_MAX_VECS);  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향

	bio_uninit(bio);  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제
	if (bio->bi_max_vecs == BIO_MAX_VECS)  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향
		mempool_free(bio->bi_io_vec, &bs->bvec_pool);  // bio 메모리를 mempool으로 반환해 NVMe 제출 가용성 회복
	else if (bio->bi_max_vecs > BIO_INLINE_VECS)  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향
		kmem_cache_free(biovec_slab(bio->bi_max_vecs)->slab,  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향
				bio->bi_io_vec);
	mempool_free(p - bs->front_pad, &bs->bio_pool);  // bio 메모리를 mempool으로 반환해 NVMe 제출 가용성 회복
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
 * @bdev:     이 bio가 I/O를 수행할 블록 장치(NVMe namespace); NULL 가능
 * @table:    bio_vec 배열 포인터; BIO_INLINE_VECS 이하면 bio 본체 내 인라인 배열
 * @max_vecs: bio_vec 배열의 최대 크기
 * @opf:      I/O 작업 유형 및 플래그 (REQ_OP_READ/WRITE 등)
 *
 * bio를 새 I/O 요청에 사용 가능한 초기 상태로 만든다.
 * 모든 필드를 0으로 설정한 뒤 bdev, opf, bi_io_vec, max_vecs, 카운터를 초기화한다.
 * cgroup blkg 연결(bio_associate_blkg)도 이 단계에서 수행된다.
 * 실행 컨텍스트: 태스크 컨텍스트(파일 시스템 I/O 제출 경로).
 * 호출자: bio_alloc_bioset(), bdev_rw_virt() 등
 * NVMe 연결:
 *   bi_iter.bi_sector → NVMe SLBA(Starting Logical Block Address)
 *   bi_iter.bi_size   → NVMe NLB(Number of Logical Blocks)×sector 크기
 *   bi_opf            → NVMe OPC(Opcode: 0x01 Write/0x02 Read 등)
 *   bi_io_vec[]       → NVMe PRP list / SGL segment 목록
 *
 * 호출 체인:
 *   파일시스템 → bio_alloc_bioset() → [bio_init()] → bio 사용 준비 완료
 */
void bio_init(struct bio *bio, struct block_device *bdev, struct bio_vec *table,  // bio 필드 초기화: SLBA/length/OPC/PRP-SGL 기반 준비
	      unsigned short max_vecs, blk_opf_t opf)
{
	bio->bi_next = NULL;  // bio_list/plug chain: NVMe multi-queue 병렬 제출 묶음
	bio->bi_bdev = bdev;  // NVMe namespace/block device 선택
	bio->bi_opf = opf;  // NVMe OPC로 매핑되는 operation/flags
	bio->bi_flags = 0;
	bio->bi_ioprio = 0;
	bio->bi_write_hint = 0;
	bio->bi_write_stream = 0;
	bio->bi_status = 0;  // NVMe CQ status -> request status -> bio status 전파 경로
	bio->bi_bvec_gap_bit = 0;
	bio->bi_iter.bi_sector = 0;	// NVMe SLBA 초기값(아직 미설정)
	bio->bi_iter.bi_size = 0;	// NVMe NLB 초기값(아직 미설정)
	bio->bi_iter.bi_idx = 0;
	bio->bi_iter.bi_bvec_done = 0;
	bio->bi_end_io = NULL;  // NVMe CQ 처리기와 연결된 상위 완료 콜백
	bio->bi_private = NULL;  // NVMe 완료 콜백용 private 데이터(예: completion 구조체)
#ifdef CONFIG_BLK_CGROUP
	bio->bi_blkg = NULL;
	bio->issue_time_ns = 0;
	if (bdev)
		bio_associate_blkg(bio);  // cgroup 연결: NVMe blk-cgroup throttling/latency 우선순위 반영
#ifdef CONFIG_BLK_CGROUP_IOCOST
	bio->bi_iocost_cost = 0;
#endif
#endif
#ifdef CONFIG_BLK_INLINE_ENCRYPTION
	bio->bi_crypt_context = NULL;
#endif
#ifdef CONFIG_BLK_DEV_INTEGRITY
	bio->bi_integrity = NULL;
#endif
	bio->bi_vcnt = 0;  // NVMe 명령의 PRP entry/SGL segment 개수 집계

	atomic_set(&bio->__bi_remaining, 1);	// bio_chain 시 분할된 NVMe 명령 카운트
	atomic_set(&bio->__bi_cnt, 1);  // NVMe bio 분할/참조 카운트 초기화
	bio->bi_cookie = BLK_QC_T_NONE;	// NVMe poll queue tracking ID 초기화

	bio->bi_max_vecs = max_vecs;  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향
	bio->bi_io_vec = table;
	bio->bi_pool = NULL;  // bio_set/mempool: NVMe doorbell 경로 메모리 보장
}
EXPORT_SYMBOL(bio_init);  // bio 필드 초기화: SLBA/length/OPC/PRP-SGL 기반 준비

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
 * @bdev: 새로 사용할 블록 장치(NVMe namespace)
 * @opf:  새 I/O 작업 유형 및 플래그
 *
 * bio_uninit()으로 기존 상태를 정리한 뒤, BIO_RESET_BYTES 범위를 0으로 초기화한다.
 * bi_io_vec 포인터는 보존하여 기존 메모리를 재사용한다.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: bio_reuse()
 * NVMe 연결: 동일한 PRP/SGL 버퍼를 가리키면서 bdev와 opf만 교체해
 *            NVMe SQ에 새 CID로 제출하는 경로에서 활용.
 *
 * 호출 체인:
 *   bio_reuse() → [bio_reset()] → bio_uninit() + memset + bio_associate_blkg()
 */
void bio_reset(struct bio *bio, struct block_device *bdev, blk_opf_t opf)  // bio 재사용: 동일 PRP/SGL 버퍼로 새 NVMe 명령 구성(추정)
{
	struct bio_vec          *bv = bio->bi_io_vec;

	bio_uninit(bio);  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제
	memset(bio, 0, BIO_RESET_BYTES);  // bio 상태 초기화: NVMe 명령 재사용 시 이전 상태 제거
	atomic_set(&bio->__bi_remaining, 1);	// bio_chain 시 분할된 NVMe 명령 카운트
	bio->bi_io_vec = bv;
	bio->bi_bdev = bdev;  // NVMe namespace/block device 선택
	if (bio->bi_bdev)  // NVMe namespace/block device 선택
		bio_associate_blkg(bio);  // cgroup 연결: NVMe blk-cgroup throttling/latency 우선순위 반영
	bio->bi_opf = opf;  // NVMe OPC로 매핑되는 operation/flags
}
EXPORT_SYMBOL(bio_reset);  // bio 재사용: 동일 PRP/SGL 버퍼로 새 NVMe 명령 구성(추정)

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
 * 실행 컨텍스트: 태스크 컨텍스트; in-flight 동안 호출 금지.
 * 호출자: RAID/DM 등 동일 데이터를 읽고 다른 위치에 쓰는 상위 레이어
 * NVMe 연결: 동일한 PRP/SGL 버퍼로 REQ_OP_READ → REQ_OP_WRITE 변환 후
 *            새 CID로 NVMe SQ에 재제출할 때 사용.
 *
 * 호출 체인:
 *   상위 레이어(raid/dm) → [bio_reuse()] → bio_reset()
 */
void bio_reuse(struct bio *bio, blk_opf_t opf)  // payload 유지 재사용: OPC만 Read/Write 전환해 NVMe SQ에 재제출(추정)
{
	unsigned short vcnt = bio->bi_vcnt, i;  // NVMe 명령의 PRP entry/SGL segment 개수 집계
	bio_end_io_t *end_io = bio->bi_end_io;  // NVMe CQ 처리기와 연결된 상위 완료 콜백
	void *private = bio->bi_private;  // NVMe 완료 콜백용 private 데이터(예: completion 구조체)

	WARN_ON_ONCE(bio_flagged(bio, BIO_CLONED));  // clone된 bio: PRP/SGL 원본을 공유하므로 중간 수정 불가
	WARN_ON_ONCE(bio_integrity(bio));  // NVMe PI/DIF 보호 정보 처리: PRP/SGL과 함께 보호 정보가 일치해야 컨트롤러가 명령을 수락함
	WARN_ON_ONCE(bio_has_crypt_ctx(bio));  // NVMe 명령/상태 불변조건 위반 방지용 assert

	bio_reset(bio, bio->bi_bdev, opf);  // bio 재사용: 동일 PRP/SGL 버퍼로 새 NVMe 명령 구성(추정)
	for (i = 0; i < vcnt; i++)
		bio->bi_iter.bi_size += bio->bi_io_vec[i].bv_len;  // NVMe 명령의 NLB(Length)로 변환됨
	bio->bi_vcnt = vcnt;  // NVMe 명령의 PRP entry/SGL segment 개수 집계
	bio->bi_private = private;  // NVMe 완료 콜백용 private 데이터(예: completion 구조체)
	bio->bi_end_io = end_io;  // NVMe CQ 처리기와 연결된 상위 완료 콜백
}
EXPORT_SYMBOL_GPL(bio_reuse);  // payload 유지 재사용: OPC만 Read/Write 전환해 NVMe SQ에 재제출(추정)

/*
 * [한국어]
 * __bio_chain_endio - 체인된 하위 bio가 완료될 때 상태를 부모로 전파하고 bio를 해제한다
 *
 * @bio: 방금 완료된 하위 bio
 * @return: 부모 bio 포인터 (bio_endio()가 반복 호출할 다음 대상)
 *
 * bi_private에 저장된 부모 bio의 bi_status를 하위 bio의 에러로 갱신한다.
 * 하위 bio의 참조를 해제한 뒤 부모를 반환하면 bio_endio()가 goto again으로 재귀 없이 처리한다.
 * 실행 컨텍스트: 하드 IRQ 컨텍스트 포함 (NVMe CQ 완료 처리기에서 호출 가능).
 * 호출자: bio_endio() (bi_end_io == bio_chain_endio 시)
 * NVMe 연결: 분할된 NVMe CID 각각의 CQ 완료가 이 함수를 거쳐 부모 bio 상태를 누적.
 *
 * 호출 체인:
 *   nvme_complete_rq() → blk_mq_end_request() → bio_endio() → [__bio_chain_endio()] → parent bio_endio()
 */
static struct bio *__bio_chain_endio(struct bio *bio)
{
	struct bio *parent = bio->bi_private; /* [한국어] bio_chain()이 설정한 부모 bio 포인터를 꺼냄 */

	if (bio->bi_status && !parent->bi_status) /* [한국어] 하위 bio에 에러가 있고 부모는 아직 정상이면 에러를 부모에 복사 */
		parent->bi_status = bio->bi_status; /* [한국어] NVMe CQ status 에러를 부모 bio로 전파 */
	bio_put(bio); /* [한국어] 하위 bio의 마지막 참조 해제: 메모리 반환 */
	return parent; /* [한국어] 부모 bio 반환: bio_endio()가 goto again으로 부모를 계속 처리 */
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
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: bio_chain_and_submit()
 * NVMe 연결: bio_split()으로 나뉜 여러 하위 bio(각각 별개의 CID)가 모두 NVMe CQ를 통해
 *            완료되어야만 상위 파일 시스템 완료 콜백이 실행됨.
 *
 * 호출 체인:
 *   bio_chain_and_submit() → [bio_chain()] → bio_inc_remaining(parent)
 */
void bio_chain(struct bio *bio, struct bio *parent)
{
	BUG_ON(bio->bi_private || bio->bi_end_io); /* [한국어] bi_private/bi_end_io가 이미 설정되면 이중 체인 버그 */

	bio->bi_private = parent;            /* [한국어] 완료 시 상태를 전달할 부모 bio를 bi_private에 저장 */
	bio->bi_end_io	= bio_chain_endio;   /* [한국어] 완료 핸들러를 sentinel로 설정: bio_endio()가 __bio_chain_endio()를 호출하도록 유도 */
	bio_inc_remaining(parent);           /* [한국어] parent의 __bi_remaining 증가: 이 하위 bio 완료 전까지 parent 완료 방지 */
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
 * blk_next_bio()가 연속 bio 스트림을 구성할 때 사용하는 헬퍼.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: blk_next_bio()
 * NVMe 연결: submit_bio() → blk_mq_submit_bio() → blk_mq_get_request() →
 *            nvme_queue_rq() → nvme_submit_cmd(doorbell).
 *
 * 호출 체인:
 *   blk_next_bio() → [bio_chain_and_submit()] → bio_chain() + submit_bio()
 */
struct bio *bio_chain_and_submit(struct bio *prev, struct bio *new)
{
	if (prev) {
		bio_chain(prev, new);  // 분할된 NVMe 명령 completion을 부모 bio로 집계
		submit_bio(prev);  // bio -> block 레이어 -> blk-mq -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
	}
	return new;
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
 * punt_bios_to_rescuer()가 rescue_list에 넣은 bio들을 꺼내
 * submit_bio_noacct()로 재제출한다. rescue_list가 빌 때까지 반복한다.
 * 실행 컨텍스트: "bioset" 워크큐(WQ_MEM_RECLAIM); 메모리 회수 가능 컨텍스트.
 * 호출자: bioset rescue_workqueue (queue_work() 후)
 * NVMe 연결: mempool 고갈로 인한 deadlock을 우회해 NVMe SQ doorbell이 멈추지 않도록 함.
 *
 * 호출 체인:
 *   punt_bios_to_rescuer() → queue_work() → [bio_alloc_rescue()] → submit_bio_noacct()
 */
static void bio_alloc_rescue(struct work_struct *work)
{
	struct bio_set *bs = container_of(work, struct bio_set, rescue_work); /* [한국어] rescue_work를 담은 bio_set을 역산 */
	struct bio *bio;

	while (1) {
		spin_lock(&bs->rescue_lock);                  /* [한국어] rescue_list 접근을 보호; IRQ-safe 불필요(태스크 컨텍스트) */
		bio = bio_list_pop(&bs->rescue_list);          /* [한국어] 대기 중인 bio 하나를 꺼냄; 비었으면 NULL */
		spin_unlock(&bs->rescue_lock);                 /* [한국어] 락 해제: 다른 태스크가 rescue_list에 추가 가능 */

		if (!bio)
			break; /* [한국어] 목록이 비었으면 루프 종료 */

		submit_bio_noacct(bio); /* [한국어] bio를 blk-mq 경로로 재제출: nvme_queue_rq → doorbell */
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
 * punt_bios_to_rescuer - submit_bio_noacct() 아래에서 mempool 고갈 시 교착 상태를 피하기 위해
 *                        현재 태스크의 bio_list를 rescuer workqueue로 넘긴다
 *
 * @bs: 이 bio_set에 속하는 bio만 rescuer로 이전할 대상 bio_set
 *
 * submit_bio_noacct()는 재귀를 반복으로 변환하므로, 그 아래에서 여러 bio를
 * 같은 bio_set에서 할당하면 mempool이 고갈되어 deadlock이 발생할 수 있다.
 * 이 함수는 current->bio_list의 bio 중 동일 bio_set의 것만 rescue_list로 이전하고
 * rescue_workqueue를 깨워 재제출한다.
 * 다른 bio_set의 bio는 건드리지 않아 스택 드라이버의 의존 관계를 보존한다.
 * 실행 컨텍스트: 태스크 컨텍스트; current->bio_list가 유효한 상태에서만 호출.
 * 호출자: bio_alloc_bioset() (mempool_alloc 전)
 * NVMe 연결: NVMe I/O 경로에서 메모리 부족 시에도 SQ doorbell이 영구히 멈추지 않도록 보장.
 *
 * 호출 체인:
 *   bio_alloc_bioset() → [punt_bios_to_rescuer()] → queue_work(rescue_workqueue)
 */
static void punt_bios_to_rescuer(struct bio_set *bs)  // 교착 상태 회피: NVMe 제출 경로를 workqueue로 우회
{
	struct bio_list punt, nopunt;
	struct bio *bio;

	if (!current->bio_list || !bs->rescue_workqueue)
		return;
	if (bio_list_empty(&current->bio_list[0]) &&  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
	    bio_list_empty(&current->bio_list[1]))  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
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

	bio_list_init(&punt);  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
	bio_list_init(&nopunt);  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음

	while ((bio = bio_list_pop(&current->bio_list[0])))  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
		bio_list_add(bio->bi_pool == bs ? &punt : &nopunt, bio);  // bio_set/mempool: NVMe doorbell 경로 메모리 보장
	current->bio_list[0] = nopunt;

	bio_list_init(&nopunt);  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
	while ((bio = bio_list_pop(&current->bio_list[1])))  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
		bio_list_add(bio->bi_pool == bs ? &punt : &nopunt, bio);  // bio_set/mempool: NVMe doorbell 경로 메모리 보장
	current->bio_list[1] = nopunt;

	spin_lock(&bs->rescue_lock);  // NVMe 완료/queue 상태 보호용 락 획득
	bio_list_merge(&bs->rescue_list, &punt);  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
	spin_unlock(&bs->rescue_lock);  // NVMe 완료/queue 상태 보호용 락 해제

	queue_work(bs->rescue_workqueue, &bs->rescue_work);	// 메모리 부족 시에도 NVMe SQ 제출 재개
}

/*
 * [한국어]
 * bio_alloc_irq_cache_splice - IRQ 캐시(free_list_irq)를 태스크 캐시(free_list)로 이전한다
 *
 * @cache: splice할 per-CPU bio_alloc_cache
 *
 * free_list_irq에 쌓인 bio들을 free_list로 옮겨 태스크 컨텍스트에서 재사용할 수 있게 한다.
 * free_list가 비어 있고(assert), IRQ를 잠시 비활성화한 뒤 포인터 이전 및 카운터를 갱신한다.
 * 실행 컨텍스트: 태스크 컨텍스트; get_cpu/put_cpu로 선점이 비활성화된 상태에서 호출.
 * 호출자: bio_alloc_percpu_cache()
 * NVMe 연결: NVMe CQ ISR에서 bio_put()으로 쌓인 free_list_irq를 태스크 경로로 이전해
 *            다음 NVMe 제출 시 할당 지연을 없앰.
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
	local_irq_restore(flags);             /* [한국어] IRQ 복원: NVMe 완료 ISR이 다시 실행될 수 있게 함 */
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
 * 실행 컨텍스트: 태스크 컨텍스트; get_cpu/put_cpu로 현재 CPU에 고정.
 * 호출자: bio_alloc_bioset() (REQ_ALLOC_CACHE 플래그가 설정된 경우)
 * NVMe 연결: NVMe 핫패스에서 반복 할당 시 slab/mempool 락 없이 O(1) 할당.
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
 * @bdev:    I/O를 수행할 블록 장치(NVMe namespace); NULL 가능
 * @nr_vecs: 사전 할당할 bvec 슬롯 수; 0이면 inline vecs만 사용
 * @opf:     I/O 작업 유형 및 플래그 (REQ_OP_READ/WRITE 등)
 * @gfp:     할당 플래그; __GFP_DIRECT_RECLAIM 포함 시 항상 성공 보장
 * @bs:      할당에 사용할 bio_set
 * @return:  새 bio 포인터; 비blocking 경로에서 실패 시 NULL
 *
 * 할당 순서: per-CPU cache → slab(try_alloc_gfp) → mempool(saved_gfp).
 * __GFP_DIRECT_RECLAIM이 없으면 slab 실패 시 즉시 NULL 반환(mempool 미사용).
 * nr_vecs > BIO_MAX_VECS이면 BUG; BIOSET_NEED_BVECS가 없는데 nr_vecs > 0이면 WARN+NULL.
 * submit_bio_noacct() 아래에서 호출 시 deadlock 방지를 위해 punt_bios_to_rescuer()를 호출한다.
 * 실행 컨텍스트: 태스크 컨텍스트 (IRQ 컨텍스트에서는 per-CPU cache 경로만 안전).
 * 호출자: bio_alloc(), 파일 시스템 I/O 경로
 * NVMe 연결: 할당된 bio → submit_bio() → blk_mq_submit_bio() →
 *            nvme_queue_rq() → nvme_submit_cmd(doorbell).
 *            nr_vecs가 클수록 PRP list/SGL이 더 많은 segment를 기술.
 *
 * 호출 체인:
 *   파일 시스템 → bio_alloc() → [bio_alloc_bioset()] → bio_init()
 */
struct bio *bio_alloc_bioset(struct block_device *bdev, unsigned short nr_vecs,  // bio + bio_vec 할당: NVMe doorbell 제출의 시작점
			     blk_opf_t opf, gfp_t gfp, struct bio_set *bs)
{
	struct bio_vec *bvecs = NULL;
	struct bio *bio = NULL;
	gfp_t saved_gfp = gfp;
	void *p;

	/* should not use nobvec bioset for nr_vecs > 0 */
	if (WARN_ON_ONCE(!mempool_initialized(&bs->bvec_pool) && nr_vecs > 0))  // mempool 초기화: NVMe bio 할당 보장 풀 구성
		return NULL;  // 할당 실패: NVMe doorbell 경로에서 NULL 반환(추정)

	gfp = try_alloc_gfp(gfp);
	if (bs->cache && nr_vecs <= BIO_INLINE_VECS) {
		/*
		 * Set REQ_ALLOC_CACHE even if no cached bio is available to
		 * return the allocated bio to the percpu cache when done.
		 */
		opf |= REQ_ALLOC_CACHE;	// NVMe 고속 경로 재할당을 위해 per-CPU cache 사용
		bio = bio_alloc_percpu_cache(bs);  // per-CPU bio cache: NVMe 고속 경로 재할당 지연 감소
	} else {
		opf &= ~REQ_ALLOC_CACHE;  // per-CPU cache 사용: NVMe doorbell latency에 민감한 재할당 경로 가속(추정)
		p = kmem_cache_alloc(bs->bio_slab, gfp);  // NVMe 핫패스 bio 할당: 빠른 재사용을 위해 슬랩에서 획득
		if (p)
			bio = p + bs->front_pad;
	}

	if (bio && nr_vecs > BIO_INLINE_VECS) {
		struct biovec_slab *bvs = biovec_slab(nr_vecs);

		/*
		 * Upgrade nr_vecs to take full advantage of the allocation.
		 * We also rely on this in bio_free().
		 */
		nr_vecs = bvs->nr_vecs;
		bvecs = kmem_cache_alloc(bvs->slab, gfp);  // NVMe 핫패스 bio 할당: 빠른 재사용을 위해 슬랩에서 획득
		if (unlikely(!bvecs)) {
			kmem_cache_free(bs->bio_slab, p);  // bio_vec/bio 본체 반환: NVMe 완료 후 cache 충전
			bio = NULL;
		}
	}

	if (unlikely(!bio)) {
		/*
		 * Give up if we are not allow to sleep as non-blocking mempool
		 * allocations just go back to the slab allocation.
		 */
		if (!(saved_gfp & __GFP_DIRECT_RECLAIM))
			return NULL;  // 할당 실패: NVMe doorbell 경로에서 NULL 반환(추정)

		punt_bios_to_rescuer(bs);  // 교착 상태 회피: NVMe 제출 경로를 workqueue로 우회

		/*
		 * Don't rob the mempools by returning to the per-CPU cache if
		 * we're tight on memory.
		 */
		opf &= ~REQ_ALLOC_CACHE;  // per-CPU cache 사용: NVMe doorbell latency에 민감한 재할당 경로 가속(추정)

		p = mempool_alloc(&bs->bio_pool, saved_gfp);	// mempool 보장: NVMe 제출이 영구 블록되지 않음
		bio = p + bs->front_pad;
		if (nr_vecs > BIO_INLINE_VECS) {
			nr_vecs = BIO_MAX_VECS;	// fallback 시 NVMe SGL/최대 PRP list 대응
			bvecs = mempool_alloc(&bs->bvec_pool, saved_gfp);  // mempool 보장: NVMe SQ 제출이 메모리 부족으로 영구 블록되지 않음
		}
	}

	if (nr_vecs && nr_vecs <= BIO_INLINE_VECS)
		bio_init_inline(bio, bdev, nr_vecs, opf);
	else
		bio_init(bio, bdev, bvecs, nr_vecs, opf);  // bio 필드 초기화: SLBA/length/OPC/PRP-SGL 기반 준비
	bio->bi_pool = bs;  // bio_set/mempool: NVMe doorbell 경로 메모리 보장
	return bio;
}
EXPORT_SYMBOL(bio_alloc_bioset);  // bio + bio_vec 할당: NVMe doorbell 제출의 시작점

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
 * 반환된 bio는 bio_init() 후 사용하고, 해제 시 bio_uninit() 후 kfree()를 호출해야 한다.
 * bio_alloc_bioset()과 달리 mempool로 보장되지 않으므로 메모리 부족 시 실패할 수 있다.
 * 파일 시스템 I/O 핫패스에는 사용하지 않는다.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: NVMe admin 명령 경로, 일부 드라이버 초기화 코드, 테스트 코드
 * NVMe 연결: admin 명령(IDENTIFY 등)처럼 단발성 I/O에서 mempool 없이 간단하게 bio를 구성할 때.
 *
 * 호출 체인:
 *   admin 경로 / 드라이버 초기화 → [bio_kmalloc()] → bio_init() (caller가 직접 호출)
 */
struct bio *bio_kmalloc(unsigned short nr_vecs, gfp_t gfp_mask)
{
	struct bio *bio;

	if (nr_vecs > BIO_MAX_INLINE_VECS)
		return NULL;  // 할당 실패: NVMe doorbell 경로에서 NULL 반환(추정)
	return kmalloc(sizeof(*bio) + nr_vecs * sizeof(struct bio_vec),
			gfp_mask);
}
EXPORT_SYMBOL(bio_kmalloc);

/*
 * [한국어]
 * zero_fill_bio_iter - bio의 start부터 끝까지 모든 bvec 버퍼를 0으로 채운다
 *
 * @bio:   0으로 채울 bio
 * @start: 순회를 시작할 bvec_iter 위치
 *
 * bio_truncate()에서 READ 명령의 잘린(truncated) 영역을 초기화하거나,
 * 새로 할당된 bio의 버퍼를 초기화할 때 사용된다.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: bio_truncate()
 * NVMe 연결: NVMe Read 결과의 truncated 부분을 사용자에게 0으로 보고할 때 호출.
 *
 * 호출 체인:
 *   guard_bio_eod() → bio_truncate() → [zero_fill_bio_iter()]
 */
void zero_fill_bio_iter(struct bio *bio, struct bvec_iter start)
{
	struct bio_vec bv;
	struct bvec_iter iter;

	__bio_for_each_segment(bv, bio, iter, start) /* [한국어] start 위치부터 bio 끝까지 bvec 단위로 순회 */
		memzero_bvec(&bv); /* [한국어] 각 bvec이 가리키는 물리 페이지 영역을 0으로 초기화 */
}
EXPORT_SYMBOL(zero_fill_bio_iter);

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
 * 실행 컨텍스트: 태스크 컨텍스트 (I/O 제출 전).
 * 호출자: guard_bio_eod()
 * NVMe 연결: bi_size → NLB 변환 기반; truncated 후 NVMe Read 결과의 미사용 영역이 0으로 노출.
 *
 * 호출 체인:
 *   guard_bio_eod() → [bio_truncate()] → zero_fill_bio_iter()
 */
static void bio_truncate(struct bio *bio, unsigned new_size)  // bio 크기 조정: EOD 등에서 NVMe NLB를 줄임
{
	struct bio_vec bv;
	struct bvec_iter iter;
	unsigned int done = 0;
	bool truncated = false;

	if (new_size >= bio->bi_iter.bi_size)  // NVMe 명령의 NLB(Length)로 변환됨
		return;

	if (bio_op(bio) != REQ_OP_READ)
		goto exit;

	bio_for_each_segment(bv, bio, iter) {  // bio_vec(bvec) 순회: NVMe PRP entry/SGL segment를 구성하는 단위
		if (done + bv.bv_len > new_size) {
			size_t offset;

			if (!truncated)
				offset = new_size - done;
			else
				offset = 0;
			memzero_page(bv.bv_page, bv.bv_offset + offset,  // 페이지 0 채움: NVMe read EOD truncated 영역 처리
				  bv.bv_len - offset);
			truncated = true;
		}
		done += bv.bv_len;
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
	bio->bi_iter.bi_size = new_size;  // NVMe 명령의 NLB(Length)로 변환됨
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
 * 완전히 범위를 벗어난 I/O(bi_sector >= maxsector)는 건드리지 않고
 * 상위 레이어/NVMe 컨트롤러가 EIO로 처리하도록 둔다.
 * 이 함수는 "마지막 sector에 걸친 I/O"를 올바르게 처리하기 위한 것이다.
 * 실행 컨텍스트: 태스크 컨텍스트 (I/O 제출 경로).
 * 호출자: 파일 시스템 / 상위 블록 레이어
 * NVMe 연결: NVMe SLBA 검증 전 커널 레벨 경계 검사; 잘못된 LBA 범위가
 *            NVMe SQ에 제출되는 것을 방지해 컨트롤러 에러를 사전 차단.
 *
 * 호출 체인:
 *   파일 시스템 submit → [guard_bio_eod()] → bio_truncate() → NVMe 제출
 */
void guard_bio_eod(struct bio *bio)  // 장치 경계 보호: 잘못된 NVMe SLBA 범위가 SQ에 제출되지 않도록 차단
{
	sector_t maxsector = bdev_nr_sectors(bio->bi_bdev);  // NVMe namespace/block device 선택

	if (!maxsector)
		return;

	/*
	 * If the *whole* IO is past the end of the device,
	 * let it through, and the IO layer will turn it into
	 * an EIO.
	 */
	if (unlikely(bio->bi_iter.bi_sector >= maxsector))  // NVMe Read/Write 명령의 SLBA(Starting LBA)로 변환됨
		return;

	maxsector -= bio->bi_iter.bi_sector;  // NVMe Read/Write 명령의 SLBA(Starting LBA)로 변환됨
	if (likely((bio->bi_iter.bi_size >> 9) <= maxsector))  // NVMe 명령의 NLB(Length)로 변환됨
		return;

	bio_truncate(bio, maxsector << 9);  // bio 크기 조정: EOD 등에서 NVMe NLB를 줄임
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
 * bio_uninit()으로 cgroup/integrity/crypto를 먼저 정리한 뒤 캐시에 저장한다.
 * 실행 컨텍스트: 태스크 컨텍스트 또는 하드 IRQ 컨텍스트.
 * 호출자: bio_put() (REQ_ALLOC_CACHE 설정 시)
 * NVMe 연결: NVMe CQ 처리(하드 IRQ) 완료 후 bio를 캐시로 반환; 다음 제출 시 재활용.
 *
 * 호출 체인:
 *   bio_put() → [bio_put_percpu_cache()] → (캐시 저장) or bio_free()
 */
static inline void bio_put_percpu_cache(struct bio *bio)
{
	struct bio_alloc_cache *cache;

	cache = per_cpu_ptr(bio->bi_pool->cache, get_cpu());  // bio_set/mempool: NVMe doorbell 경로 메모리 보장
	if (READ_ONCE(cache->nr_irq) + cache->nr > ALLOC_CACHE_MAX)
		goto out_free;  // 자원 해제: NVMe 완료/abort 시 메모리 반환

	if (in_task()) {
		bio_uninit(bio);  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제
		bio->bi_next = cache->free_list;  // bio_list/plug chain: NVMe multi-queue 병렬 제출 묶음
		/* Not necessary but helps not to iopoll already freed bios */
		bio->bi_bdev = NULL;  // NVMe namespace/block device 선택
		cache->free_list = bio;
		cache->nr++;
		kmemleak_free(bio_slab_addr(bio));
	} else if (in_hardirq()) {
		lockdep_assert_irqs_disabled();

		bio_uninit(bio);  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제
		bio->bi_next = cache->free_list_irq;  // bio_list/plug chain: NVMe multi-queue 병렬 제출 묶음
		cache->free_list_irq = bio;
		cache->nr_irq++;
		kmemleak_free(bio_slab_addr(bio));
	} else {
		goto out_free;  // 자원 해제: NVMe 완료/abort 시 메모리 반환
	}
	put_cpu();
	return;
out_free:
	put_cpu();
	bio_free(bio);  // NVMe 완료 후 bio 메모리를 풀로 반환
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
 * 실행 컨텍스트: 태스크 컨텍스트 및 하드 IRQ 컨텍스트.
 * 호출자: bio_endio(), __bio_chain_endio(), bio_dirty_fn() 등
 * NVMe 연결: NVMe CQ 처리 완료 후 blk_mq_end_request() → bio_endio() → bio_put().
 *            REQ_ALLOC_CACHE 설정 시 per-CPU 캐시로 반환하여 doorbell latency 감소.
 *
 * 호출 체인:
 *   nvme_complete_rq() → blk_mq_end_request() → bio_endio() → [bio_put()]
 */
void bio_put(struct bio *bio)  // NVMe CID 회수/CQ 처리 완료 후 bio 참조 해제
{
	if (unlikely(bio_flagged(bio, BIO_REFFED))) {
		BUG_ON(!atomic_read(&bio->__bi_cnt));  // NVMe bio 참조/remaining 상태 확인
		if (!atomic_dec_and_test(&bio->__bi_cnt))  // NVMe completion 순서 보장: 마지막 하위 bio 완료 시 부모로 전파
			return;
	}
	if (bio->bi_opf & REQ_ALLOC_CACHE)	// NVMe 완료 후 cache로 회수하여 재할당 지연 감소
		bio_put_percpu_cache(bio);  // per-CPU cache로 bio 회수: NVMe doorbell latency 민감 경로(추정)
	else
		bio_free(bio);  // NVMe 완료 후 bio 메모리를 풀로 반환
}
EXPORT_SYMBOL(bio_put);  // NVMe CID 회수/CQ 처리 완료 후 bio 참조 해제

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
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: bio_alloc_clone(), bio_init_clone()
 * NVMe 연결: 복제된 bi_iter은 동일한 SLBA/NLB를 의미하며, 원본과 같은 NVMe PRP/SGL을 공유.
 *
 * 호출 체인:
 *   bio_alloc_clone() / bio_init_clone() → [__bio_clone()] → bio_crypt_clone() + bio_integrity_clone()
 */
static int __bio_clone(struct bio *bio, struct bio *bio_src, gfp_t gfp)
{
	bio_set_flag(bio, BIO_CLONED);	// NVMe PRP/SGL 원본 공유 표시
	bio->bi_ioprio = bio_src->bi_ioprio;
	bio->bi_write_hint = bio_src->bi_write_hint;
	bio->bi_write_stream = bio_src->bi_write_stream;
	bio->bi_iter = bio_src->bi_iter;	// SLBA/length 복제: 동일 NVMe 명령 범위

	if (bio->bi_bdev) {  // NVMe namespace/block device 선택
		if (bio->bi_bdev == bio_src->bi_bdev &&  // NVMe namespace/block device 선택
		    bio_flagged(bio_src, BIO_REMAPPED))
			bio_set_flag(bio, BIO_REMAPPED);
		bio_clone_blkg_association(bio, bio_src);  // cgroup 복제: NVMe QoS 맥락이 분할/clone된 bio에도 유지
	}

	if (bio_crypt_clone(bio, bio_src, gfp) < 0)  // NVMe inline crypto/Opal: PRP/SGL 데이터와 암호화 컨텍스트가 nvme_queue_rq에서 연결됨
		return -ENOMEM;  // 메모리 부족: NVMe 명령 제출 전 abort(추정)
	if (bio_integrity(bio_src) &&  // NVMe PI/DIF 보호 정보 처리: PRP/SGL과 함께 보호 정보가 일치해야 컨트롤러가 명령을 수락함
	    bio_integrity_clone(bio, bio_src, gfp) < 0)
		return -ENOMEM;  // 메모리 부족: NVMe 명령 제출 전 abort(추정)
	return 0;  // 정상 완료: NVMe 처리 흐름 계속
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
 * bi_io_vec를 공유하고 bi_iter를 복사하는 lightweight clone이다.
 * 반환된 bio를 해제하기 전에 bio_src가 살아 있어야 한다(bvec 메모리를 공유하므로).
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: bio_split()
 * NVMe 연결: NVMe MDTS 분할 시 원본 PRP/SGL을 공유하는 하위 bio를 생성.
 *            clone bio는 별도의 CID로 NVMe SQ에 독립 제출됨.
 *
 * 호출 체인:
 *   bio_split() → [bio_alloc_clone()] → bio_alloc_bioset() + __bio_clone()
 */
struct bio *bio_alloc_clone(struct block_device *bdev, struct bio *bio_src,  // bio_vec 공유 clone: NVMe MDTS 분할 시 원본 PRP/SGL 재사용
		gfp_t gfp, struct bio_set *bs)
{
	struct bio *bio;

	bio = bio_alloc_bioset(bdev, 0, bio_src->bi_opf, gfp, bs);  // NVMe OPC로 매핑되는 operation/flags
	if (!bio)
		return NULL;  // 할당 실패: NVMe doorbell 경로에서 NULL 반환(추정)

	if (__bio_clone(bio, bio_src, gfp) < 0) {
		bio_put(bio);  // NVMe CID 회수/CQ 처리 완료 후 bio 참조 해제
		return NULL;  // 할당 실패: NVMe doorbell 경로에서 NULL 반환(추정)
	}
	bio->bi_io_vec = bio_src->bi_io_vec;

	return bio;
}
EXPORT_SYMBOL(bio_alloc_clone);  // bio_vec 공유 clone: NVMe MDTS 분할 시 원본 PRP/SGL 재사용

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
 * @bdev:    클론 bio가 접근할 블록 장치 (NVMe namespace)
 * @bio:     클론될 대상 메모리 (호출자가 사전 할당)
 * @bio_src: 복제 원본 bio
 * @gfp:     __bio_clone() 내부 할당 플래그
 * @return:  0 성공; 음수 errno
 *
 * bio_alloc_clone()과 달리 bio 본체 메모리는 호출자가 제공한다.
 * bio_init()으로 기본 초기화 후 __bio_clone()으로 bio_src의 메타데이터를 복사한다.
 * bi_io_vec(PRP/SGL 기반 데이터 포인터)는 원본과 공유된다.
 * 실패 시 bio_uninit()으로 cgroup/integrity/crypto 자원을 정리한다.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: dm/md, NVMe passthrough — 고정 구조체 안에 bio를 임베딩하는 경우
 * NVMe 연결: bio_src의 bi_io_vec을 공유하므로 원본 PRP/SGL이 동일하게 사용됨.
 *
 * 호출 체인:
 *   dm/md → [bio_init_clone()] → bio_init() + __bio_clone() → bio_crypt_clone() + bio_integrity_clone()
 */
int bio_init_clone(struct block_device *bdev, struct bio *bio,  // caller 제공 메모리 clone: NVMe 메타데이터/PRP 공유
		struct bio *bio_src, gfp_t gfp)
{
	int ret;

	bio_init(bio, bdev, bio_src->bi_io_vec, 0, bio_src->bi_opf);  // bio 필드 초기화: SLBA/length/OPC/PRP-SGL 기반 준비
	ret = __bio_clone(bio, bio_src, gfp);
	if (ret)
		bio_uninit(bio);  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제
	return ret;
}
EXPORT_SYMBOL(bio_init_clone);  // caller 제공 메모리 clone: NVMe 메타데이터/PRP 공유

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
 * 실행 컨텍스트: 태스크 컨텍스트 (I/O 구성 단계).
 * 호출자: bio_add_page(), bio_iov_iter_get_pages()
 * NVMe 연결: bio가 가득 차면 상위 레이어가 새 bio를 할당하거나 blk-mq가 분할;
 *            NVMe MDTS·segment 한도 준수의 첫 번째 판단 지점.
 */
static inline bool bio_full(struct bio *bio, unsigned len)  // bio가 가득 찼는지 확인: NVMe segment/MDTS 한도 초과 신호
{
	if (bio->bi_vcnt >= bio->bi_max_vecs)	// NVMe segment 한도 초과 시 새 bio 필요
		return true;  // 조건 만족: NVMe 분기/병합/완료 판정
	if (bio->bi_iter.bi_size > BIO_MAX_SIZE - len)	// NVMe 최대 전송 크기 초과 방지
		return true;  // 조건 만족: NVMe 분기/병합/완료 판정
	return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
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
 * 물리 주소 연속성, XEN 하이퍼바이저 병합 제한, 페이지 경계 KMSAN 제한을 검사한다.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: bio_add_page(), bvec_try_merge_hw_page()
 * NVMe 연결: 인접한 물리 페이지를 하나의 PRP entry 또는 SGL segment로 묶어
 *            NVMe 명령 오버헤드를 감소시킨다. 병합이 많을수록 PRP list 길이가 짧아지고
 *            NVMe 명령 오버헤드가 감소한다. 다만 XEN, CONFIG_KMSAN 등
 *            특수 환경에서는 병합이 제한될 수 있다.
 */
static bool bvec_try_merge_page(struct bio_vec *bv, struct page *page,  // 물리 인접 페이지 병합: PRP list 길이를 줄여 NVMe 명령 오버헤드 감소
		unsigned int len, unsigned int off)
{
	size_t bv_end = bv->bv_offset + bv->bv_len;
	phys_addr_t vec_end_addr = page_to_phys(bv->bv_page) + bv_end - 1;  // 페이지 물리 주소 -> NVMe DMA/PRP 주소 변환
	phys_addr_t page_addr = page_to_phys(page);  // 페이지 물리 주소 -> NVMe DMA/PRP 주소 변환

	if (vec_end_addr + 1 != page_addr + off)
		return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
	if (xen_domain() && !xen_biovec_phys_mergeable(bv, page))	// XEN 하이퍼바이저 환경에서 NVMe DMA 안전성
		return false;  // 조건 불만족: NVMe 분기/병합/완료 판정

	if ((vec_end_addr & PAGE_MASK) != ((page_addr + off) & PAGE_MASK)) {	// 페이지 경계를 넘는 NVMe PRP 병합 시 주의
		if (IS_ENABLED(CONFIG_KMSAN))
			return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
		if (bv->bv_page + bv_end / PAGE_SIZE != page + off / PAGE_SIZE)
			return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
	}

	bv->bv_len += len;
	return true;  // 조건 만족: NVMe 분기/병합/완료 판정
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
 * segment_boundary: 큐 단위 segment 경계 마스크; 두 주소가 같은 구간이어야 병합 가능.
 * max_segment_size: segment당 최대 바이트 수; 초과 시 병합 불가.
 * 실제 물리 인접 검사는 bvec_try_merge_page()에 위임한다.
 * 주로 T10-PI integrity 메타데이터 bio에서 사용된다.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: integrity 관련 bvec 구성 경로
 * NVMe 연결: NVMe 컨트롤러마다 max_segment_size·segment_boundary가 다름;
 *            bio 단계에서 준수해 blk-mq/nvme_queue_rq의 PRP/SGL 빌드 부담 감소.
 */
bool bvec_try_merge_hw_page(struct request_queue *q, struct bio_vec *bv,  // 물리 인접 페이지 병합: PRP list 길이를 줄여 NVMe 명령 오버헤드 감소
		struct page *page, unsigned len, unsigned offset)
{
	unsigned long mask = queue_segment_boundary(q);
	phys_addr_t addr1 = bvec_phys(bv);  // bio_vec의 물리 주소: NVMe PRP entry/SGL 주소 필드로 사용
	phys_addr_t addr2 = page_to_phys(page) + offset + len - 1;  // 페이지 물리 주소 -> NVMe DMA/PRP 주소 변환

	if ((addr1 | mask) != (addr2 | mask))
		return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
	if (len > queue_max_segment_size(q) - bv->bv_len)
		return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
	return bvec_try_merge_page(bv, page, len, offset);  // 물리 인접 페이지 병합: PRP list 길이를 줄여 NVMe 명령 오버헤드 감소
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
 * 실행 컨텍스트: 태스크 컨텍스트; BIO_CLONED bio에서 호출 금지(WARN).
 * 호출자: bio_add_page(), bio_add_folio_nofail(), bio_add_virt_nofail()
 * NVMe 연결: 추가된 bvec(page+offset+len)은 blk-mq/nvme_map_data()가
 *            PRP entry 또는 SGL segment로 변환하여 NVMe DMA 설정.
 *
 * 호출 체인:
 *   bio_add_page() → [__bio_add_page()] → bvec_set_page()
 */
void __bio_add_page(struct bio *bio, struct page *page,  // bio_vec 추가: NVMe DMA descriptor 구성의 시작점
		unsigned int len, unsigned int off)
{
	WARN_ON_ONCE(bio_flagged(bio, BIO_CLONED));  // clone된 bio: PRP/SGL 원본을 공유하므로 중간 수정 불가
	WARN_ON_ONCE(bio_full(bio, len));  // bio가 가득 찼는지 확인: NVMe segment/MDTS 한도 초과 신호

	if (is_pci_p2pdma_page(page))	// NVMe P2PDMA/CMB 페이지: 병합 금지
		bio->bi_opf |= REQ_NOMERGE;  // NVMe merge 금지: P2PDMA/CMB나 특수 버퍼에서 PRP/SGL 단순화(추정)

	bvec_set_page(&bio->bi_io_vec[bio->bi_vcnt], page, len, off);	// bio_vec 추가 -> 후속 PRP/SGL 변환
	bio->bi_iter.bi_size += len;  // NVMe 명령의 NLB(Length)로 변환됨
	bio->bi_vcnt++;  // NVMe 명령의 PRP entry/SGL segment 개수 집계
}
EXPORT_SYMBOL_GPL(__bio_add_page);  // bio_vec 추가: NVMe DMA descriptor 구성의 시작점

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
void bio_add_virt_nofail(struct bio *bio, void *vaddr, unsigned len)  // 커널 가상 주소를 bio에 추가: NVMe PRP/SGL로 변환
{
	__bio_add_page(bio, virt_to_page(vaddr), len, offset_in_page(vaddr));  // bio_vec 추가: NVMe DMA descriptor 구성의 시작점
}
EXPORT_SYMBOL_GPL(bio_add_virt_nofail);  // 커널 가상 주소를 bio에 추가: NVMe PRP/SGL로 변환

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
 * 먼저 마지막 bvec와 물리 인접 여부를 확인해 병합을 시도(bvec_try_merge_page).
 * 병합 불가능하면 새 bvec 슬롯이 있는지 확인하고 __bio_add_page()로 추가.
 * zone_device_pages_have_same_pgmap(): P2PDMA/device-managed 페이지의 pgmap 일관성 검사.
 * 실행 컨텍스트: 태스크 컨텍스트.
 * 호출자: 파일 시스템 I/O 구성, bio_add_folio(), bio_add_vmalloc_chunk()
 * NVMe 연결: 추가된 페이지(bvec)들이 NVMe PRP list 또는 SGL로 변환;
 *            병합이 많을수록 PRP list 길이 감소 → NVMe 명령 오버헤드 감소.
 *
 * 호출 체인:
 *   파일 시스템 → [bio_add_page()] → bvec_try_merge_page() or __bio_add_page()
 */
int bio_add_page(struct bio *bio, struct page *page,  // 사용자/커널 페이지를 bio에 추가 -> NVMe PRP/SGL 후보
		 unsigned int len, unsigned int offset)
{
	if (WARN_ON_ONCE(bio_flagged(bio, BIO_CLONED)))  // clone된 bio: PRP/SGL 원본을 공유하므로 중간 수정 불가
		return 0;  // 정상 완료: NVMe 처리 흐름 계속
	if (WARN_ON_ONCE(len == 0))  // NVMe 명령/상태 불변조건 위반 방지용 assert
		return 0;  // 정상 완료: NVMe 처리 흐름 계속
	if (bio->bi_iter.bi_size > BIO_MAX_SIZE - len)	// NVMe 최대 전송 크기 초과 방지
		return 0;  // 정상 완료: NVMe 처리 흐름 계속

	if (bio->bi_vcnt > 0) {  // NVMe 명령의 PRP entry/SGL segment 개수 집계
		struct bio_vec *bv = &bio->bi_io_vec[bio->bi_vcnt - 1];  // NVMe 명령의 PRP entry/SGL segment 개수 집계

		if (!zone_device_pages_have_same_pgmap(bv->bv_page, page))
			return 0;  // 정상 완료: NVMe 처리 흐름 계속

		if (bvec_try_merge_page(bv, page, len, offset)) {  // 물리 인접 페이지 병합: PRP list 길이를 줄여 NVMe 명령 오버헤드 감소
			bio->bi_iter.bi_size += len;  // NVMe 명령의 NLB(Length)로 변환됨
			return len;
		}
	}

	if (bio->bi_vcnt >= bio->bi_max_vecs)	// NVMe segment 한도 초과 시 새 bio 필요
		return 0;  // 정상 완료: NVMe 처리 흐름 계속
	__bio_add_page(bio, page, len, offset);  // bio_vec 추가: NVMe DMA descriptor 구성의 시작점
	return len;
}
EXPORT_SYMBOL(bio_add_page);  // 사용자/커널 페이지를 bio에 추가 -> NVMe PRP/SGL 후보

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
 * 실행 컨텍스트: 태스크 컨텍스트 (파일 시스템 I/O 구성 경로).
 * 호출자: 파일 시스템, 블록 레이어 내부 — 반드시 성공해야 하는 경우
 * NVMe 연결: folio 내 page가 NVMe PRP/SGL entry의 시작 페이지가 됨.
 *
 * 호출 체인:
 *   fs I/O path → [bio_add_folio_nofail()] → __bio_add_page()
 */
void bio_add_folio_nofail(struct bio *bio, struct folio *folio, size_t len,
			  size_t off)
{
	unsigned long nr = off / PAGE_SIZE;

	WARN_ON_ONCE(len > BIO_MAX_SIZE);  // NVMe 명령/상태 불변조건 위반 방지용 assert
	__bio_add_page(bio, folio_page(folio, nr), len, off % PAGE_SIZE);  // bio_vec 추가: NVMe DMA descriptor 구성의 시작점
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
		return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
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
		return 0;  // 정상 완료: NVMe 처리 흐름 계속
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
			return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
		vaddr += added;
		len -= added;
	} while (len);

	return true;  // 조건 만족: NVMe 분기/병합/완료 판정
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
		return 0;  // 정상 완료: NVMe 처리 흐름 계속

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
	return 0;  // 정상 완료: NVMe 처리 흐름 계속
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
		return 0;  // 정상 완료: NVMe 처리 흐름 계속
	}

	if (iov_iter_extract_will_pin(iter))  // 사용자 페이지 pin 여부: NVMe DMA 안전성 판단
		bio_set_flag(bio, BIO_PAGE_PINNED);	// 사용자 페이지 pin: NVMe DMA 안전 보장
	if (bio->bi_bdev && blk_queue_pci_p2pdma(bio->bi_bdev->bd_disk->queue))  // NVMe namespace/block device 선택
		flags |= ITER_ALLOW_P2PDMA;  // NVMe CMB/P2PDMA 경로에서 사용자 페이지 매핑 허용(추정)

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

	if (is_pci_p2pdma_page(bio->bi_io_vec->bv_page))	// NVMe P2PDMA 경로에서는 REQ_NOMERGE 유지
		bio->bi_opf |= REQ_NOMERGE;  // NVMe merge 금지: P2PDMA/CMB나 특수 버퍼에서 PRP/SGL 단순화(추정)
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

		folio = folio_alloc_greedy(GFP_KERNEL, &this_len);	// NVMe DMA에 적합한 정렬된 bounce buffer 할당(추정)
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
		return -ENOMEM;  // 메모리 부족: NVMe 명령 제출 전 abort(추정)
	return 0;  // 정상 완료: NVMe 처리 흐름 계속
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

	folio = folio_alloc_greedy(GFP_KERNEL, &len);  // NVMe DMA 적합한 정렬된 bounce buffer 할당(추정)
	if (!folio)
		return -ENOMEM;  // 메모리 부족: NVMe 명령 제출 전 abort(추정)

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
	return 0;  // 정상 완료: NVMe 처리 흐름 계속
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
void bio_iov_iter_unbounce(struct bio *bio, bool is_error, bool mark_dirty)
{
	if (op_is_write(bio_op(bio)))
		bio_free_folios(bio);
	else
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

	if (WARN_ON_ONCE(is_vmalloc_addr(data)))  // NVMe 명령/상태 불변조건 위반 방지용 assert
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
		return true;  // 조건 만족: NVMe 분기/병합/완료 판정

	BUG_ON(atomic_read(&bio->__bi_remaining) <= 0);  // NVMe bio 참조/remaining 상태 확인

	if (atomic_dec_and_test(&bio->__bi_remaining)) {  // NVMe completion 순서 보장: 마지막 하위 bio 완료 시 부모로 전파
		bio_clear_flag(bio, BIO_CHAIN);
		return true;  // 조건 만족: NVMe 분기/병합/완료 판정
	}

	return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
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

	if (WARN_ON_ONCE(sectors <= 0))  // NVMe 명령/상태 불변조건 위반 방지용 assert
		return ERR_PTR(-EINVAL);  // 오류 포인터 반환: NVMe 분할/clone 실패 전파
	if (WARN_ON_ONCE(sectors >= bio_sectors(bio)))  // NVMe 명령/상태 불변조건 위반 방지용 assert
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

	if (WARN_ON_ONCE(offset > BIO_MAX_SECTORS || size > BIO_MAX_SECTORS ||  // NVMe 명령/상태 불변조건 위반 방지용 assert
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
	bs->front_pad = front_pad;
	if (flags & BIOSET_NEED_BVECS)
		bs->back_pad = BIO_INLINE_VECS * sizeof(struct bio_vec);
	else
		bs->back_pad = 0;

	spin_lock_init(&bs->rescue_lock);
	bio_list_init(&bs->rescue_list);  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
	INIT_WORK(&bs->rescue_work, bio_alloc_rescue);  // mempool 고갈 시 bio를 다시 submit해 NVMe SQ drain 방지

	bs->bio_slab = bio_find_or_create_slab(bs);  // bio slab 공유/생성: NVMe bio 할당 성능에 영향
	if (!bs->bio_slab)
		return -ENOMEM;  // 메모리 부족: NVMe 명령 제출 전 abort(추정)

	if (mempool_init_slab_pool(&bs->bio_pool, pool_size, bs->bio_slab))  // mempool 초기화: NVMe bio 할당 보장 풀 구성
		goto bad;  // 초기화 실패: NVMe bio pool 사용 불가 처리

	if ((flags & BIOSET_NEED_BVECS) &&
	    biovec_init_pool(&bs->bvec_pool, pool_size))
		goto bad;  // 초기화 실패: NVMe bio pool 사용 불가 처리

	if (flags & BIOSET_NEED_RESCUER) {
		bs->rescue_workqueue = alloc_workqueue("bioset",
							WQ_MEM_RECLAIM, 0);
		if (!bs->rescue_workqueue)
			goto bad;  // 초기화 실패: NVMe bio pool 사용 불가 처리
	}
	if (flags & BIOSET_PERCPU_CACHE) {
		bs->cache = alloc_percpu(struct bio_alloc_cache);
		if (!bs->cache)
			goto bad;  // 초기화 실패: NVMe bio pool 사용 불가 처리
		cpuhp_state_add_instance_nocalls(CPUHP_BIO_DEAD, &bs->cpuhp_dead);
	}

	return 0;  // 정상 완료: NVMe 처리 흐름 계속
bad:
	bioset_exit(bs);  // bio_set 해제: NVMe 제출 풀 정리
	return -ENOMEM;  // 메모리 부족: NVMe 명령 제출 전 abort(추정)
}
EXPORT_SYMBOL(bioset_init);  // bio_set 초기화: NVMe 제출에 사용되는 전역/드라이버 풀 생성

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

	return 0;  // 정상 완료: NVMe 처리 흐름 계속
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
