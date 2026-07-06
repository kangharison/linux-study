// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Christoph Hellwig.
 */

/*
 * [한국어 설명] 파일시스템 전용 bio integrity(PI/DIF) 버퍼 헬퍼 (bio-integrity-fs.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 파일시스템이 자신의 페이지 캐시로부터 직접 소유하는 integrity
 * 메타데이터 버퍼(fs_bio_integrity_buf)를 bio에 붙였다 떼는 5개의 소규모
 * 헬퍼 함수(fs_bio_integrity_alloc/free/generate/verify/init)를 제공한다.
 * block/bio-integrity.c의 범용 integrity 프레임워크(__bio_integrity_action,
 * bio_integrity_alloc_buf 등)가 "블록 계층 관점"의 저수준 연산을 제공한다면,
 * 이 파일은 그 위에서 "파일시스템 관점"의 편의 계층을 제공한다: 파일시스템이
 * 매번 bio_integrity_init/alloc_buf/setup_default를 직접 순서대로 호출하지
 * 않고, fs_bio_integrity_generate()/fs_bio_integrity_verify() 두 개의 진입점만
 * 알면 되도록 감싸준다. 대표적인 사용처는 fs-verity, XFS/ext4의 자체
 * 체크섬(PI passthrough) 경로처럼 파일시스템이 스토리지 하드웨어의 T10 PI/DIF
 * 대신 자신만의 메타데이터 페이지를 bio에 부착하고 싶을 때이다. mempool 기반
 * 할당을 사용해 메모리 회수(reclaim) 경로나 I/O 완료 인터럽트 컨텍스트처럼
 * GFP_KERNEL이 허용되지 않는 상황에서도 항상 버퍼를 확보할 수 있게 보장한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 흐름상 이 파일은 "파일시스템 -> 블록 계층 -> 블록 드라이버"라는
 * 3단 구조 중 파일시스템과 블록 계층의 경계에 위치한다. 쓰기 경로에서는
 * 파일시스템이 데이터를 채운 bio를 submit_bio()로 넘기기 직전에
 * fs_bio_integrity_generate()를 호출하여 fs_bio_integrity_alloc()으로 버퍼를
 * 마련하고 bio_integrity_generate()로 체크섬(PI)을 계산해 둔다. 이후
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> (드라이버별
 * queue_rq, 예: nvme_queue_rq) -> doorbell/큐 제출의 표준 블록 계층 경로를
 * 그대로 탄다. 읽기 경로에서는 bio_endio()로 I/O 완료가 보고된 뒤 파일시스템이
 * 원래 기억해 둔 sector/size를 가지고 fs_bio_integrity_verify()를 호출해
 * 페이지 캐시에 채워진 데이터와 부착된 메타데이터를 대조 검증한다. 이 파일의
 * 함수들은 파일시스템 컨텍스트(보통 잡/프로세스 컨텍스트, 드물게 워크큐)에서
 * 호출되는 커널 코드이며, GFP_NOIO를 사용해 재진입(reclaim 중 I/O 재귀)을
 * 피한다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일은 include/linux/bio-integrity.h가 선언하는 bio_integrity_payload,
 * bio_integrity_init(), bio_integrity_generate(), bio_integrity_verify(),
 * bio_integrity_action() 등 block/bio-integrity.c가 구현하는 범용 integrity
 * API에 의존한다. 또한 include/linux/blk-integrity.h가 제공하는
 * blk_get_integrity()를 통해 대상 디스크(bio->bi_bdev->bd_disk)의 integrity
 * 프로파일(섹터당 메타데이터 크기, PI 타입)을 얻는다. "blk.h"는 blk-mq 저수준
 * 헬퍼 선언을 담고 있어 이 번역 단위(compilation unit)가 blk-mq 내부 심볼에
 * 접근할 수 있게 해준다. 데이터 흐름 관점에서 보면, 파일시스템이 채운 데이터
 * 페이지(bio->bi_io_vec)와 별개로 이 파일이 관리하는 fs_bio_integrity_buf 내부의
 * 단일 bio_vec가 메타데이터 페이지 하나를 가리키며, 이 메타데이터 bio_vec는
 * bio->bi_integrity를 통해 bio 본체와 연결된다. 이 파일과 block/bio-integrity.c,
 * block/t10-pi.c는 함께 "bio integrity 서브시스템"을 구성하며, t10-pi.c가
 * CRC16/CRC64 계산 알고리즘을 제공하는 최하위 계층이라면, 이 파일은 그 결과물을
 * 파일시스템이 소비하기 쉬운 형태로 감싸는 최상위 계층에 해당한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - fs_bio_integrity_alloc(): bio의 integrity action을 조회해 필요하다면
 *   mempool에서 fs_bio_integrity_buf를 할당하고 bio에 연결한다.
 * - fs_bio_integrity_free(): 위에서 할당한 버퍼를 해제하고 bio를 원상태로
 *   되돌린다(REQ_INTEGRITY 플래그 제거 포함).
 * - fs_bio_integrity_generate(): 쓰기 경로 진입점. 버퍼를 할당한 뒤 실제
 *   체크섬(PI) 값을 생성한다.
 * - fs_bio_integrity_verify(): 읽기 완료 후 진입점. bip_iter를 재초기화한
 *   뒤 저장된 메타데이터와 데이터를 비교 검증한다.
 * - fs_bio_integrity_init(): module_init 단계에서 kmem_cache와 mempool을
 *   미리 만들어 두는 부트스트랩 함수.
 * - struct fs_bio_integrity_buf: bio_integrity_payload 본체(bip)와 그 payload가
 *   가리키는 단일 bio_vec(bvec)를 한 번의 할당으로 묶어내는 컨테이너 구조체.
 *   container_of()로 bip 포인터에서 역참조해 통째로 mempool에 반환한다.
 */
#include <linux/blk-integrity.h>	/* [한국어] blk_get_integrity() 등 디스크 단위 integrity 프로파일 조회 API 선언 - fs_bio_integrity_verify()가 대상 디스크의 메타데이터 크기/타입을 얻기 위해 필요 */
#include <linux/bio-integrity.h>	/* [한국어] bio_integrity_payload, bio_integrity_init/alloc_buf/generate/verify 등 범용 integrity 프레임워크 API 선언 - 이 파일의 모든 헬퍼가 내부적으로 위임하는 하위 계층 */
#include "blk.h"			/* [한국어] 블록 계층 내부 전용 헤더(mempool 등 커널 심볼 및 blk-mq 저수준 선언 포함) - 드라이버 외부에 노출하지 않는 block/ 서브시스템 내부 인터페이스 사용을 위해 포함 */

/* [한국어] fs_bio_integrity_buf
 * bio_integrity_payload 본체와 그 payload가 참조할 단일 bio_vec를 하나의
 * 메모리 블록으로 묶은 컨테이너. 이렇게 묶어두면 kmem_cache/mempool 할당을
 * 한 번만 수행해도 bip와 bvec가 함께 확보되어, 별도의 두 번째 할당(및 실패
 * 처리 경로)이 필요 없다. bip가 어차피 가변 길이 꼬리(bip_vec 배열)를 가지는
 * 구조라는 점에서, 파일시스템이 필요로 하는 "메타데이터 페이지 1장"이라는
 * 가장 흔한 경우를 위해 bvec 필드 하나를 고정으로 붙여 넣은 것이다. */
struct fs_bio_integrity_buf {
	struct bio_integrity_payload	bip;
	/* [한국어] 이 fs 전용 버퍼가 감싸고 있는 실제 bio_integrity_payload.
	 * 설정자: fs_bio_integrity_alloc()이 bio_integrity_init(bio, &iib->bip,
	 *         &iib->bvec, 1)을 호출하며 초기화하고, 그 결과 포인터를
	 *         bio->bi_integrity에 연결한다(bio_integrity_init 내부 동작,
	 *         추정).
	 * 읽는 자: bio_integrity_generate()/bio_integrity_verify()가 bip_iter,
	 *         bip_vec 등을 순회하며 실제 체크섬 계산/비교를 수행한다.
	 *         fs_bio_integrity_free()는 이 필드의 주소로부터
	 *         container_of()를 이용해 fs_bio_integrity_buf 전체 주소를
	 *         역산해 mempool에 반환한다.
	 * 값 범위: bio_integrity_init() 호출 전에는 미정의 상태이며, 호출 이후
	 *         유효한 bio_integrity_payload로 취급된다. bio의 수명 동안
	 *         (fs_bio_integrity_free 호출 전까지) 유효하다.
	 * 동기화: 하나의 bio는 특정 시점에 단일 스레드(제출 시) 또는 단일
	 *         완료 콜백(인터럽트/소프트IRQ, 완료 시)에서만 다뤄지므로 이
	 *         구조체 자체에는 별도의 락이 없다. 제출과 완료 사이의 순서는
	 *         블록 계층의 요청 상태 머신이 보장한다. */

	struct bio_vec			bvec;
	/* [한국어] bip가 가리키는 단 하나의 메타데이터(PI) 페이지 조각.
	 * 설정자: bio_integrity_init()이 iib->bip.bip_vec를 이 필드의 주소로
	 *         세팅하며, 이후 bio_integrity_alloc_buf()가 실제 페이지를
	 *         할당해 bv_page/bv_offset/bv_len을 채운다(추정, 하위 계층
	 *         block/bio-integrity.c의 책임).
	 * 읽는 자: NVMe 등 블록 드라이버가 bio_integrity(bio)를 통해 이
	 *         bio_vec를 순회하며 DMA를 위한 PRP(Physical Region Page) 또는
	 *         SGL(Scatter Gather List) 엔트리로 변환한다(추정).
	 * 값 범위: 큰 메타데이터가 필요한 경우 bip_max_vcnt가 1을 넘을 수
	 *         있으나(그 경우 이 단일 bvec만으로는 부족하므로 별도 배열이
	 *         쓰일 수 있음, 추정), 이 파일이 다루는 fs_bio_integrity_buf는
	 *         항상 정확히 1개의 bvec만 가정한다(bio_integrity_init 세 번째
	 *         인자가 상수 1).
	 * 동기화: bip 필드와 동일하게 단일 bio의 수명 동안 하나의 실행
	 *         컨텍스트에서만 접근되므로 별도 동기화가 불필요하다. */
};

/* [한국어] fs_bio_integrity_cache
 * fs_bio_integrity_buf 구조체 전용 SLAB 캐시. 매 I/O마다 범용
 * kmalloc(sizeof(struct fs_bio_integrity_buf)) 대신 전용 캐시를 쓰는 이유는
 * (1) SLAB_HWCACHE_ALIGN으로 캐시라인 정렬을 보장해 DMA/PRP 접근 시 거짓
 * 공유(false sharing)를 줄이고, (2) mempool_init_slab_pool()의 전제 조건이기
 * 때문이다(mempool이 이 캐시로부터 예비 객체들을 미리 만들어 둔다).
 * 설정자: fs_bio_integrity_init()에서 kmem_cache_create()로 부팅 초기화 단계
 *         한 번만 설정되며, 이후 절대 재할당되지 않는다(모듈 unload 경로가
 *         없는 built-in 전용 서브시스템).
 * 읽는 자: fs_bio_integrity_init() 내부에서 mempool_init_slab_pool()의
 *         인자로 전달되어, mempool이 내부적으로 이 캐시를 통해 객체를
 *         할당/해제한다. 이후 이 전역 변수 자체를 직접 참조하는 코드는
 *         없다(mempool이 소유권을 위임받음).
 * 값 범위: 초기화 전에는 NULL(BSS 초기값), 초기화 성공 후에는 유효한
 *         kmem_cache 포인터. kmem_cache_create()가 SLAB_PANIC 플래그로
 *         호출되므로 실패 시 이 포인터가 NULL로 남는 경우는 발생하지 않는다
 *         (실패하면 커널이 그 자리에서 패닉).
 * 동기화: 부팅 시 단일 스레드(initcall)에서만 쓰기가 일어나고, 그 이후에는
 *         읽기 전용으로 취급되므로 락이 필요 없다. */
static struct kmem_cache *fs_bio_integrity_cache;
/* [한국어] fs_bio_integrity_pool
 * fs_bio_integrity_cache로부터 예비 객체를 미리 확보해 두는 mempool. 이
 * 서브시스템이 메모리 회수(memory reclaim)나 I/O 완료 인터럽트처럼
 * GFP_KERNEL 할당이 데드락을 유발할 수 있는 컨텍스트에서 동작해야 하므로,
 * fs_bio_integrity_alloc()은 GFP_NOIO를 사용하면서도 이 mempool 덕분에 항상
 * 최소 하나 이상의 객체를 확보할 수 있음을 보장받는다.
 * 설정자: fs_bio_integrity_init()에서 mempool_init_slab_pool(&pool,
 *         BIO_POOL_SIZE, cache)로 BIO_POOL_SIZE개의 예비 객체를 채워
 *         초기화한다.
 * 읽는 자: fs_bio_integrity_alloc()이 mempool_alloc()으로 소비하고,
 *         fs_bio_integrity_free()가 mempool_free()로 반환한다. 즉 이 파일
 *         내에서 할당/해제 쌍을 이루는 유일한 mempool이다.
 * 값 범위: mempool_t는 내부에 락(spinlock)과 대기열, 예비 객체 배열 포인터를
 *         갖는 불투명 구조체이며, 이 파일에서는 직접 필드에 접근하지 않고
 *         mempool_* API로만 다룬다.
 * 동기화: mempool 자체가 내부적으로 스핀락으로 동시 접근을 보호하므로,
 *         여러 CPU에서 동시에 mempool_alloc()/mempool_free()를 호출해도
 *         안전하다(호출자 쪽에서 별도 락이 필요 없다). */
static mempool_t fs_bio_integrity_pool;

/**
 * [한국어]
 * fs_bio_integrity_alloc - bio에 필요한 integrity(PI) 버퍼를 mempool에서
 *                          할당하고 초기화한다
 *
 * @bio: integrity 버퍼를 붙일 대상 bio. 아직 bi_integrity가 설정되지 않은
 *       상태여야 하며, 파일시스템이 데이터 페이지를 다 채운 뒤(쓰기) 또는
 *       읽기를 준비하는 시점에 전달된다.
 * @return: bio_integrity_action()이 반환한 action 비트마스크. 0이면 이
 *          bio에는 integrity 처리가 전혀 필요 없어 아무 버퍼도 할당하지
 *          않았다는 뜻이고, 0이 아니면 버퍼 할당과 초기화가 끝났으며 어떤
 *          후속 처리(0으로 채우기/기본 프로파일 설정)가 이미 적용되었는지를
 *          나타낸다.
 *
 * 이 함수가 필요한 이유: bio_integrity_init/alloc_buf/setup_default를 매
 * 호출부마다 올바른 순서와 조건으로 직접 나열하면 실수하기 쉽고 중복이
 * 많다. 이 함수는 그 절차를 한 곳에 모아, 파일시스템이 action 비트만으로
 * "이 bio에 정말 integrity 처리가 있었는지"를 판단하게 해준다.
 * 동작 순서: (1) bio_integrity_action()으로 이 bio에 어떤 처리가 필요한지
 * 조회한다. (2) action이 0이면 아무 것도 하지 않고 즉시 반환한다(가장 흔한
 * "integrity 미사용" 경로에서 할당 비용을 피하기 위함). (3) action이 0이
 * 아니면 GFP_NOIO로 mempool에서 fs_bio_integrity_buf 한 개를 확보한다. (4)
 * bio_integrity_init()으로 bip/bvec를 연결해 bio->bi_integrity를 활성화한다.
 * (5) bio_integrity_alloc_buf()로 실제 메타데이터 페이지를 확보하며, 이때
 * BI_ACT_ZERO 비트가 서 있으면 새로 할당된 버퍼를 0으로 채운다(주로 쓰기
 * 경로에서 부분 섹터 등으로 인해 정의되지 않은 값이 남지 않도록). (6)
 * BI_ACT_CHECK 비트가 서 있으면 bio_integrity_setup_default()로 이후 검증에
 * 쓰일 기본 플래그/시드를 설정한다.
 * 실행 컨텍스트: 파일시스템의 제출 경로(잡/프로세스 컨텍스트)에서 호출되며
 * GFP_NOIO를 쓰므로 메모리 회수 중 재귀 호출되어도 안전하다. 하나의 bio에
 * 대해서는 재진입 없이 순차적으로만 호출된다고 가정한다(같은 bio를 두 번
 * 할당하면 이전 bip 포인터가 누수된다).
 * 호출자(caller): fs_bio_integrity_generate()가 쓰기 경로에서 호출하며,
 * 그 반환값(action)을 보고 bio_integrity_generate() 호출 여부를 결정한다.
 * 피호출자(callee): bio_integrity_action(), mempool_alloc(),
 * bio_integrity_init(), bio_integrity_alloc_buf(),
 * bio_integrity_setup_default() (모두 block/bio-integrity.c 구현).
 * 에러 처리: mempool_alloc()은 GFP_NOIO에 대해 항상 성공을 보장하는
 * mempool의 계약에 따라 실패 시 예비 객체가 준비될 때까지 블로킹하며 실패를
 * 반환하지 않는다(따라서 이 함수는 실패 반환 경로가 없다).
 *
 * 호출 체인:
 *   fs_bio_integrity_generate() → [fs_bio_integrity_alloc] →
 *   bio_integrity_init()/bio_integrity_alloc_buf()/bio_integrity_setup_default()
 */
unsigned int fs_bio_integrity_alloc(struct bio *bio)
{
	struct fs_bio_integrity_buf *iib;	/* [한국어] mempool에서 확보할 컨테이너 포인터 - 아직 미할당 상태로 선언 */
	unsigned int action;			/* [한국어] 이 bio에 필요한 integrity 처리 종류를 나타내는 비트마스크 - 초기값 없음, 아래에서 즉시 대입 */

	/* 이 bio에 integrity가 필요없다면 (예: passthrough 요청) 아무 것도
	 * 하지 않는다.
	 */
	action = bio_integrity_action(bio);	/* [한국어] block/bio-integrity.c의 하위 API 호출 - bio_op/플래그를 검사해 BI_ACT_BUFFER, BI_ACT_ZERO, BI_ACT_CHECK 등의 조합을 계산해 반환(하위 파일 구현, 세부 판단 로직은 추정) */
	if (!action)				/* [한국어] action이 0이면 이 bio는 integrity 버퍼가 전혀 필요 없는 경우 - 아래 할당/초기화를 모두 건너뛰기 위한 조기 종료 분기 */
		return 0;			/* [한국어] 호출자(fs_bio_integrity_generate)에게 "처리 없음"을 알려 bio_integrity_generate() 호출을 막는다 */

	iib = mempool_alloc(&fs_bio_integrity_pool, GFP_NOIO);	/* [한국어] fs_bio_integrity_pool에서 fs_bio_integrity_buf 한 개를 확보 - GFP_NOIO는 이 경로가 메모리 회수 중에도 안전하게 재진입되도록 I/O를 유발하는 할당을 금지 */
	bio_integrity_init(bio, &iib->bip, &iib->bvec, 1);	/* [한국어] iib 내부의 bip를 초기화하고 bio->bi_integrity에 연결, bvec 배열 용량은 1개로 고정(이 구조체가 항상 단일 메타데이터 페이지만 다루기 때문) */
	bio_integrity_alloc_buf(bio, action & BI_ACT_ZERO);	/* [한국어] 실제 메타데이터 페이지를 할당 - action에 BI_ACT_ZERO 비트가 서 있으면(주로 쓰기 경로) 새 버퍼를 0으로 초기화해 정의되지 않은 값이 검증에 섞이지 않도록 함 */
	if (action & BI_ACT_CHECK)		/* [한국어] action에 BI_ACT_CHECK 비트가 서 있으면(추후 검증이 필요한 경우) 기본 프로파일을 설정해야 하는 분기 */
		bio_integrity_setup_default(bio);	/* [한국어] 이후 bio_integrity_generate()/verify()가 사용할 기본 플래그와 시드 값을 세팅(하위 계층 구현, 세부 필드는 block/bio-integrity.c 책임) */
	return action;				/* [한국어] 호출자에게 실제 적용된 action 비트마스크를 그대로 돌려주어, 이어서 bio_integrity_generate()를 호출할지 판단하게 함 */
}

/**
 * [한국어]
 * fs_bio_integrity_free - fs_bio_integrity_alloc()이 확보한 버퍼를 해제하고
 *                         bio를 원래 상태로 되돌린다
 *
 * @bio: fs_bio_integrity_alloc()으로 integrity 버퍼가 붙어 있는 bio. 이 함수
 *       호출 시점에는 bio->bi_integrity가 유효한 포인터여야 한다.
 * @return: 없음(void). 실패할 수 있는 동작이 없으므로 반환값이 필요 없다.
 *
 * 이 함수가 필요한 이유: fs_bio_integrity_alloc()이 mempool에서 확보한
 * 메모리는 명시적으로 반환하지 않으면 영구히 새어나간다(mempool은 GC가
 * 없다). 또한 bio 구조체 자체는 재사용될 수 있으므로(예: bio_reset 경로),
 * integrity 관련 상태를 깨끗이 지워야 다음 사용 시 잘못된 포인터를 참조하지
 * 않는다.
 * 동작 순서: (1) bio_integrity(bio)로 현재 붙어 있는
 * bio_integrity_payload를 얻는다. (2) bio_integrity_free_buf()로 실제
 * 메타데이터 페이지(위 alloc 단계에서 확보한 페이지)를 먼저 반환한다. (3)
 * container_of()로 bip 포인터로부터 감싸고 있던 fs_bio_integrity_buf 전체의
 * 시작 주소를 역산해 mempool_free()로 mempool에 돌려준다. (4)
 * bio->bi_integrity를 NULL로 지워 더 이상 유효하지 않은 포인터가 남지
 * 않도록 한다. (5) REQ_INTEGRITY 플래그를 지워 이 bio가 이후 재사용될 때
 * "integrity 없음" 상태로 인식되게 한다.
 * 실행 컨텍스트: 보통 I/O 완료 콜백 경로(bio_endio 계열)에서 호출되므로
 * 인터럽트/소프트IRQ 컨텍스트일 수 있다. mempool_free()는 이런 컨텍스트에서
 * 안전하게 호출 가능하도록 설계되어 있다(내부적으로 스핀락만 사용, 블로킹
 * 없음).
 * 호출자(caller): 파일시스템/블록 계층의 bio 완료 처리 경로(bio_endio 이후
 * 정리 단계, 추정)에서 호출된다.
 * 피호출자(callee): bio_integrity(), bio_integrity_free_buf(),
 * container_of(), mempool_free().
 * 에러 처리: 이 함수 자체는 실패 경로가 없다. 다만 bio->bi_integrity가
 * 이미 NULL인 상태에서 잘못 호출되면 bio_integrity(bio)가 NULL을 반환해
 * container_of() 계산이 잘못된 주소를 만들어낼 수 있으므로, 반드시
 * bi_integrity가 유효할 때만 호출해야 한다는 전제(caller 책임)가 있다.
 *
 * 호출 체인:
 *   bio 완료 처리 경로 → [fs_bio_integrity_free] → mempool_free()
 */
void fs_bio_integrity_free(struct bio *bio)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);	/* [한국어] bio에 현재 연결된 integrity payload 포인터를 조회 - fs_bio_integrity_alloc()에서 bio_integrity_init()이 세팅해 둔 바로 그 bip */

	bio_integrity_free_buf(bip);	/* [한국어] bip가 가리키는 실제 메타데이터 페이지(들)를 먼저 반환 - fs_bio_integrity_buf 컨테이너 자체보다 먼저 해제해야 페이지 참조가 끊어진 뒤 컨테이너를 반환할 수 있음 */
	mempool_free(container_of(bip, struct fs_bio_integrity_buf, bip),
			&fs_bio_integrity_pool);	/* [한국어] bip 필드의 주소로부터 container_of()로 fs_bio_integrity_buf 전체 시작 주소를 역산 - fs_bio_integrity_alloc()이 컨테이너 단위로 할당했으므로 반환도 컨테이너 단위로 이루어져야 함 */

	bio->bi_integrity = NULL;		/* [한국어] 방금 해제한 bip를 더 이상 가리키지 않도록 초기화 - 이후 이 bio가 재사용되거나 실수로 다시 참조되어도 댕글링 포인터를 따라가지 않게 함 */
	bio->bi_opf &= ~REQ_INTEGRITY;		/* [한국어] REQ_INTEGRITY 플래그를 지워 이 bio가 더 이상 integrity 처리 대상이 아님을 표시 - 이후 재사용 시 bio_integrity_action() 등이 잘못된 처리 경로를 타지 않도록 방지 */
}

/**
 * [한국어]
 * fs_bio_integrity_generate - 쓰기 경로에서 bio에 대한 integrity(PI) 값을
 *                             생성한다
 *
 * @bio: 데이터 페이지가 이미 채워진, 곧 submit_bio()로 제출될 쓰기 bio.
 * @return: 없음(void). 내부적으로 action이 0이면(=integrity 불필요)
 *          아무 일도 하지 않고 조용히 반환한다.
 *
 * 이 함수가 필요한 이유: 파일시스템이 매번 "버퍼를 할당하고, 할당에
 * 성공했다면 체크섬을 생성한다"는 두 단계를 직접 조합하지 않도록, 하나의
 * 진입점으로 노출하기 위함이다. EXPORT_SYMBOL_GPL로 내보내져 이 파일이
 * 속한 block/ 서브시스템 밖의 파일시스템 모듈에서도 호출할 수 있다.
 * 동작 순서: (1) fs_bio_integrity_alloc(bio)을 호출해 필요한 버퍼를
 * 확보한다. (2) 반환값이 0이 아니면(=실제로 integrity 처리가 필요했다면)
 * bio_integrity_generate()를 호출해 데이터로부터 체크섬/가드/참조 태그 등을
 * 계산하여 방금 확보한 메타데이터 버퍼에 채운다. (3) 반환값이 0이면(=애초에
 * integrity가 불필요) 아무 것도 하지 않는다.
 * 실행 컨텍스트: 파일시스템의 쓰기 제출 경로(잡/프로세스 컨텍스트)에서
 * submit_bio() 이전에 동기적으로 호출된다. 이 시점 이후 bio는 블록 계층/
 * 드라이버로 넘어가므로, 이 함수가 끝난 뒤에는 데이터와 메타데이터 모두
 * 확정된 상태여야 한다.
 * 호출자(caller): 파일시스템의 쓰기 경로(예: fs-verity, 자체 체크섬을 쓰는
 * 파일시스템의 write_iter/writepages 경로, 추정)에서 데이터를 다 채운
 * 직후 호출한다.
 * 피호출자(callee): fs_bio_integrity_alloc(), bio_integrity_generate().
 * 에러 처리: 이 함수는 실패를 보고할 방법이 없는 void 함수이다.
 * fs_bio_integrity_alloc() 내부의 mempool_alloc()이 항상 성공을 보장하는
 * mempool 계약에 의존하므로, 상위 호출자가 별도의 실패 처리를 준비할
 * 필요가 없다.
 *
 * 호출 체인:
 *   파일시스템 쓰기 경로 → [fs_bio_integrity_generate] →
 *   fs_bio_integrity_alloc() / bio_integrity_generate() → submit_bio()
 */
void fs_bio_integrity_generate(struct bio *bio)
{
	if (fs_bio_integrity_alloc(bio))	/* [한국어] 먼저 버퍼를 확보하고, 반환된 action이 0이 아닌 경우에만(=이 bio가 실제로 integrity 처리 대상인 경우) 아래 생성 단계로 진입 */
		bio_integrity_generate(bio);	/* [한국어] 데이터 페이지들을 훑어 체크섬/가드/참조 태그를 계산하고 fs_bio_integrity_alloc()이 마련해 둔 메타데이터 버퍼에 기록(하위 계층 구현) */
}
EXPORT_SYMBOL_GPL(fs_bio_integrity_generate);	/* [한국어] 이 심볼을 GPL 라이선스 모듈에 한해 외부(파일시스템 모듈 등)로 노출 - block/ 서브시스템 밖에서도 이 헬퍼 하나로 쓰기 경로의 integrity 생성을 끝낼 수 있게 함 */

/**
 * [한국어]
 * fs_bio_integrity_verify - 읽기 완료 후 저장된 메타데이터와 데이터를
 *                           대조해 integrity(PI)를 검증한다
 *
 * @bio: 읽기가 완료되어 데이터와 메타데이터가 모두 채워진 bio.
 * @sector: 이 bio가 다루는 데이터의 시작 논리 섹터 번호. bio 자체의
 *          bi_iter는 드라이버가 완료 처리 중에 이미 진행시켜 놓았을 수
 *          있으므로, 호출자가 원래의 시작 섹터를 별도로 기억해 두었다가
 *          전달해야 한다.
 * @size: 검증할 데이터의 바이트 크기. 섹터 단위로 변환되어(size >>
 *        SECTOR_SHIFT) 대응하는 메타데이터 바이트 수를 계산하는 데 쓰인다.
 * @return: blk_status_to_errno()가 변환한 표준 errno 값. 0이면 검증 성공,
 *          음수 errno(예: -EIO)이면 메타데이터와 데이터가 불일치하여
 *          integrity 검증에 실패했음을 뜻한다.
 *
 * 이 함수가 필요한 이유: bio_integrity_verify()는 bip->bip_iter가 현재
 * "검증하려는 범위"를 정확히 가리키고 있어야 동작한다. 그런데 드라이버가
 * I/O를 처리하는 동안 bip_iter는 이미 앞으로 진행되어 원래 범위를 가리키지
 * 않게 된다. 이 함수는 그 iterator를 호출자가 기억해 둔 원래 sector/size로
 * 되돌린 뒤 검증을 위임하는 재초기화 계층 역할을 한다(파일 내부의 영어
 * 주석이 이 배경을 그대로 설명하고 있다).
 * 동작 순서: (1) blk_get_integrity()로 대상 디스크의 integrity 프로파일(bi)을
 * 얻는다 - 섹터당 메타데이터 바이트 수 등의 상수가 여기서 나온다. (2)
 * bio_integrity(bio)로 현재 bio에 연결된 payload(bip)를 얻는다. (3)
 * bip->bip_iter 전체를 0으로 초기화해 이전 진행 상태를 지운다. (4)
 * bi_sector를 호출자가 넘겨준 원래 섹터로 설정한다. (5) bi_size를
 * bio_integrity_bytes(bi, size >> SECTOR_SHIFT)로 계산한다 - 바이트 크기를
 * 섹터 수로 바꾼 뒤, 섹터당 메타데이터 크기를 곱해 검증해야 할 메타데이터
 * 총 바이트 수를 얻는다. (6) bio_integrity_verify()에 이 재초기화된 iterator를
 * 넘겨 실제 비교를 수행시키고, 그 블록 계층 상태 코드(blk_status_t)를
 * blk_status_to_errno()로 표준 errno로 변환해 반환한다.
 * 실행 컨텍스트: 읽기 완료 보고(bio_endio) 이후, 파일시스템의 완료 처리
 * 경로에서 호출된다. 드라이버의 인터럽트 컨텍스트에서 직접 호출되기보다는
 * 그 이후 파일시스템 계층으로 넘어온 시점(워크큐/소프트IRQ 콜백 등, 추정)에
 * 호출되는 것이 일반적이다.
 * 호출자(caller): 파일시스템의 읽기 완료 콜백(추정)에서, 원래 요청한
 * sector/size를 인자로 넘겨 호출한다.
 * 피호출자(callee): blk_get_integrity(), bio_integrity(),
 * bio_integrity_bytes(), bio_integrity_verify(), blk_status_to_errno().
 * 에러 처리: 검증 실패는 예외(exception)가 아니라 정상적인 반환값(음수
 * errno)으로 호출자에게 전달된다. 호출자는 이 값을 자신의 읽기 완료 처리
 * 결과에 반영해(예: -EIO를 상위 read 시스템 호출 실패로 전파) 사용자에게
 * 데이터 손상을 알려야 한다.
 *
 * 호출 체인:
 *   bio_endio(읽기 완료) → 파일시스템 완료 콜백 → [fs_bio_integrity_verify] →
 *   bio_integrity_verify() → blk_status_to_errno()
 */
int fs_bio_integrity_verify(struct bio *bio, sector_t sector, unsigned int size)
{
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);	/* [한국어] bio가 실제로 제출되었던 디스크의 integrity 프로파일을 조회 - 섹터당 메타데이터 크기, PI 타입 등 검증에 필요한 상수를 담고 있음 */
	struct bio_integrity_payload *bip = bio_integrity(bio);	/* [한국어] 이 bio에 이미 붙어 있는(읽기 데이터와 함께 채워진) integrity payload를 조회 */

	/*
	 * Reinitialize bip->bip_iter.
	 *
	 * This is for use in the submitter after the driver is done with the
	 * bio.  Requires the submitter to remember the sector and the size.
	 */
	/* [한국어] 위 영어 주석 설명: 드라이버가 이 bio를 다 처리한 뒤
	 * bip->bip_iter는 이미 소비되어 진행된 상태이므로, 검증을 시작하려면
	 * "제출자(submitter)"가 기억해 둔 원래 sector/size를 가지고 iterator를
	 * 처음 상태로 되돌려야 한다는 뜻이다. */
	memset(&bip->bip_iter, 0, sizeof(bip->bip_iter));	/* [한국어] bip_iter 구조체 전체를 0으로 초기화 - 드라이버가 진행시켜 놓은 이전 오프셋/카운트 값을 완전히 제거해 이후 필드 대입이 깨끗한 상태에서 시작되게 함 */
	bip->bip_iter.bi_sector = sector;			/* [한국어] 검증을 시작할 논리 섹터를 호출자가 전달한 원래 값으로 설정 - 이 값이 실제 저장된 메타데이터의 시작 위치와 일치해야 검증이 올바르게 정렬됨 */
	bip->bip_iter.bi_size = bio_integrity_bytes(bi, size >> SECTOR_SHIFT);			/* [한국어] size(바이트)를 섹터 수로 변환(size >> SECTOR_SHIFT)한 뒤, 프로파일 bi가 정의한 섹터당 메타데이터 크기를 곱해 검증 대상 메타데이터의 총 바이트 수를 계산 */
	return blk_status_to_errno(bio_integrity_verify(bio, &bip->bip_iter));		/* [한국어] 재초기화된 iterator로 실제 데이터-메타데이터 비교를 수행시키고, blk_status_t 결과(BLK_STS_OK/BLK_STS_IOERR 등)를 표준 errno(0/-EIO 등)로 변환해 반환 */
}

/**
 * [한국어]
 * fs_bio_integrity_init - 이 서브시스템이 쓸 kmem_cache/mempool을 부팅 시
 *                         미리 만들어 두는 초기화 함수
 *
 * (파라미터 없음)
 * @return: 항상 0. __init 함수의 관례상 initcall 프레임워크가 이 반환값을
 *          받지만, 이 함수는 실패 시 panic()으로 커널을 멈추므로 0이 아닌
 *          값을 반환할 일이 없다.
 *
 * 이 함수가 필요한 이유: fs_bio_integrity_alloc()이 사용할 mempool은 미리
 * 채워져 있어야 "메모리가 정말 부족한 순간"에도 할당이 보장된다. 이
 * 예약(pre-reserve)은 첫 I/O가 발생하기 전, 즉 부팅 초기화 단계에 끝나 있어야
 * 의미가 있다.
 * 동작 순서: (1) kmem_cache_create()로 fs_bio_integrity_buf 전용 SLAB
 * 캐시를 만든다 - SLAB_HWCACHE_ALIGN으로 캐시라인 정렬을 요청하고,
 * SLAB_PANIC으로 생성 실패 시 즉시 패닉하도록 요청한다(이 함수 내에서 별도
 * NULL 체크가 필요 없는 이유). (2) mempool_init_slab_pool()로 그 캐시를
 * 기반으로 BIO_POOL_SIZE개의 예비 객체를 갖는 mempool을 초기화한다. (3)
 * 초기화가 실패하면(0이 아닌 값을 반환하면) panic()으로 커널을 멈춘다 - 이
 * 서브시스템 없이는 이후 integrity가 필요한 파일시스템 I/O가 아예 성립할
 * 수 없기 때문에, 조용히 넘어가는 대신 즉시 실패를 드러내는 편이 안전하다는
 * 설계 판단이다. (4) 성공하면 0을 반환한다.
 * 실행 컨텍스트: 커널 부팅 중 initcall 프레임워크가 단일 스레드로 호출한다.
 * 다른 CPU나 인터럽트와 경쟁하지 않는 것으로 가정할 수 있는 시점이다.
 * 호출자(caller): fs_initcall() 매크로가 이 함수의 주소를 initcall 섹션에
 * 등록하며, 커널 부팅 시퀀스가 순서대로 이를 호출한다.
 * 피호출자(callee): kmem_cache_create(), mempool_init_slab_pool(), panic().
 * 에러 처리: kmem_cache_create()의 실패는 SLAB_PANIC 플래그가 대신
 * 처리한다(내부에서 즉시 패닉). mempool_init_slab_pool()의 실패는 이 함수가
 * 직접 검사해 panic()을 호출한다 - 두 경우 모두 "이 서브시스템 없이 부팅을
 * 계속하지 않는다"는 동일한 정책을 반영한다.
 *
 * 호출 체인:
 *   커널 부팅(initcall 프레임워크) → [fs_bio_integrity_init] →
 *   kmem_cache_create() / mempool_init_slab_pool()
 */
static int __init fs_bio_integrity_init(void)
{
	fs_bio_integrity_cache = kmem_cache_create("fs_bio_integrity",
			sizeof(struct fs_bio_integrity_buf), 0,	/* [한국어] 캐시 이름 "fs_bio_integrity"(디버깅 시 /proc/slabinfo 등에서 식별용), 객체 크기는 컨테이너 구조체 전체 크기, align 인자는 0(기본 정렬 사용) */
			SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);	/* [한국어] SLAB_HWCACHE_ALIGN은 객체를 캐시라인 경계에 정렬시켜 DMA/동시 접근 시 성능을 높이고, SLAB_PANIC은 생성 실패 시 이 함수 대신 SLAB 코드가 즉시 패닉하도록 위임, 마지막 인자(생성자 콜백)는 사용하지 않아 NULL */
	if (mempool_init_slab_pool(&fs_bio_integrity_pool, BIO_POOL_SIZE,
			fs_bio_integrity_cache))	/* [한국어] fs_bio_integrity_pool을 fs_bio_integrity_cache 기반으로 BIO_POOL_SIZE개의 예비 객체로 채움 - 0이 아닌 값(실패)이면 아래 panic 분기로 진입 */
		panic("fs_bio_integrity: can't create pool\n");	/* [한국어] mempool 초기화 실패는 이 서브시스템 전체가 동작할 수 없음을 의미하므로, 조용한 실패 대신 명확한 메시지와 함께 부팅을 즉시 중단시킴 */
	return 0;	/* [한국어] 초기화 성공 - initcall 프레임워크에는 0(성공)만 보고되며, 실패 경로는 이미 위에서 panic으로 소진됨 */
}
fs_initcall(fs_bio_integrity_init);	/* [한국어] 이 함수를 fs_initcall 단계(디바이스/드라이버 initcall보다 이르고, 파일시스템이 마운트되기 전 단계)에 등록 - 실제 파일시스템 I/O가 시작되기 전에 mempool이 반드시 준비되어 있도록 순서를 보장 */

/*
 * [한국어] 파일 전체 요약 (재확인용)
 *
 * - 이 파일은 파일시스템이 자체 관리하는 integrity(PI) 메타데이터 버퍼를
 *   bio에 붙였다 떼는 절차를, block/bio-integrity.c가 제공하는 범용 API
 *   위에 감싸 두 개의 진입점(fs_bio_integrity_generate/verify)으로 단순화한
 *   편의 계층이다.
 * - fs_bio_integrity_alloc()/free()는 fs_bio_integrity_buf라는 컨테이너
 *   구조체(bip + bvec 1개)를 mempool 단위로 할당/반환하는 짝을 이루는
 *   함수이며, container_of()를 통해 bip 포인터만으로 컨테이너 전체를
 *   되찾는 패턴을 사용한다.
 * - fs_bio_integrity_generate()는 쓰기 경로에서, fs_bio_integrity_verify()는
 *   읽기 완료 경로에서 호출되는 서로 대칭적인 진입점이다. 후자는 드라이버가
 *   진행시켜 놓은 bip_iter를 원래 sector/size로 되돌리는 재초기화 책임까지
 *   함께 진다.
 * - fs_bio_integrity_init()은 fs_initcall 단계에서 kmem_cache/mempool을
 *   미리 마련해, 이후 어떤 I/O 경로(메모리 회수 중이든 아니든)에서도
 *   fs_bio_integrity_alloc()이 항상 성공하도록 보장한다.
 * - 이 파일은 block/bio-integrity.c(범용 프레임워크), block/t10-pi.c(CRC
 *   계산), include/linux/blk-integrity.h·bio-integrity.h(공개 API 선언)와
 *   함께 bio integrity 서브시스템 전체를 구성한다.
 */
